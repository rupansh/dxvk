#include "dxvk_helios_present_sync.h"

#include "../util/log/log.h"
#include "../util/util_string.h"
#include "../util/util_env.h"

#include <windows.h>

#include <atomic>
#include <mutex>

namespace dxvk {

  namespace {

    constexpr uint32_t HpsMagic     = 0x31535048u; // 'HPS1'
    constexpr uint32_t HpsSlotCount = 64u;

    struct HpsHeader {
      uint32_t magic;
      uint32_t slotCount;
      uint32_t reserved[6];
    };

    // 32 bytes. `seq` odd while a write is in flight (seqlock). `resid` is
    // CAS-claimed once and never returns to 0 while the file lives — stale
    // slots from exited producers are re-CLAIMABLE only via pid liveness
    // (see claimSlot), never silently reused for a different resid.
    struct HpsSlot {
      volatile LONG seq;
      uint32_t      resid;
      uint32_t      pid;
      uint32_t      fenceId;
      volatile LONG64 value;
      uint64_t      reserved2;
    };

    static_assert(sizeof(HpsHeader) == 32);
    static_assert(sizeof(HpsSlot) == 32);

    struct HpsMapping {
      HpsHeader* header = nullptr;
      HpsSlot*   slots  = nullptr;
    };

    HpsMapping g_map;
    std::once_flag g_mapOnce;

    std::string mapPath() {
      std::string path = env::getEnvVar("HELIOS_PRESENT_SYNC_PATH");
      if (path.empty())
        path = "C:\\ProgramData\\Helios\\helios_present_sync.bin";
      return path;
    }

    // Map (creating on first use) the shared table. Every process maps the
    // same on-disk file; Windows keeps all views of one file coherent, so
    // this behaves as shared memory without any Global\ section name (a
    // session-1 producer and a session-0 consumer would otherwise need
    // cross-session object-namespace rights).
    void initMapping() {
      const std::string path = mapPath();

      HANDLE file = ::CreateFileA(path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

      if (file == INVALID_HANDLE_VALUE) {
        Logger::warn(str::format("HeliosPresentSync: CreateFile(", path,
          ") failed: ", ::GetLastError()));
        return;
      }

      const uint32_t size = sizeof(HpsHeader) + HpsSlotCount * sizeof(HpsSlot);

      HANDLE mapping = ::CreateFileMappingA(file, nullptr,
        PAGE_READWRITE, 0, size, nullptr);
      const DWORD mapErr = ::GetLastError();
      // The file handle is not needed once the mapping exists.
      ::CloseHandle(file);

      if (!mapping) {
        Logger::warn(str::format("HeliosPresentSync: CreateFileMapping failed: ", mapErr));
        return;
      }

      void* view = ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
      ::CloseHandle(mapping); // the view keeps the section referenced

      if (!view) {
        Logger::warn(str::format("HeliosPresentSync: MapViewOfFile failed: ", ::GetLastError()));
        return;
      }

      auto* header = reinterpret_cast<HpsHeader*>(view);

      // First mapper stamps the header; racers spin briefly on a partial
      // stamp. A wrong-versioned file is refused loudly (never guessed at).
      LONG prev = ::InterlockedCompareExchange(
        reinterpret_cast<volatile LONG*>(&header->magic), LONG(HpsMagic), 0);

      if (prev != 0 && uint32_t(prev) != HpsMagic) {
        Logger::err(str::format("HeliosPresentSync: ", path,
          " has foreign magic 0x", std::hex, uint32_t(prev), " — refusing table"));
        ::UnmapViewOfFile(view);
        return;
      }

      if (prev == 0)
        header->slotCount = HpsSlotCount;

      for (uint32_t spin = 0; header->slotCount == 0 && spin < 4096; spin++)
        ::Sleep(0);

      if (header->slotCount != HpsSlotCount) {
        Logger::err(str::format("HeliosPresentSync: slot count mismatch (",
          header->slotCount, " vs ", HpsSlotCount, ") — refusing table"));
        ::UnmapViewOfFile(view);
        return;
      }

      g_map.header = header;
      g_map.slots  = reinterpret_cast<HpsSlot*>(header + 1);

      Logger::info(str::format("HeliosPresentSync: table mapped (", path, ")"));
    }

    HpsSlot* findSlot(uint32_t resid) {
      for (uint32_t i = 0; i < HpsSlotCount; i++) {
        if (g_map.slots[i].resid == resid)
          return &g_map.slots[i];
      }
      return nullptr;
    }

    bool pidAlive(uint32_t pid) {
      HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (!proc)
        return ::GetLastError() == ERROR_ACCESS_DENIED; // exists, just shielded
      DWORD code = 0;
      const bool alive = ::GetExitCodeProcess(proc, &code) && code == STILL_ACTIVE;
      ::CloseHandle(proc);
      return alive;
    }

    // Claim a slot for `resid`: free slot first, else recycle one whose
    // producer is dead (its name is unresolvable anyway).
    HpsSlot* claimSlot(uint32_t resid) {
      for (uint32_t i = 0; i < HpsSlotCount; i++) {
        HpsSlot* slot = &g_map.slots[i];
        if (slot->resid == 0
         && ::InterlockedCompareExchange(
              reinterpret_cast<volatile LONG*>(&slot->resid),
              LONG(resid), 0) == 0)
          return slot;
      }

      for (uint32_t i = 0; i < HpsSlotCount; i++) {
        HpsSlot* slot = &g_map.slots[i];
        const uint32_t pid = slot->pid;
        const uint32_t old = slot->resid;
        if (old != 0 && pid != 0 && !pidAlive(pid)
         && ::InterlockedCompareExchange(
              reinterpret_cast<volatile LONG*>(&slot->resid),
              LONG(resid), LONG(old)) == LONG(old))
          return slot;
      }

      return nullptr;
    }

  }


  bool HeliosPresentSync::publish(uint32_t resid, uint32_t pid, uint32_t fenceId, uint64_t value) {
    std::call_once(g_mapOnce, initMapping);

    if (!g_map.slots || !resid)
      return false;

    HpsSlot* slot = findSlot(resid);
    if (!slot)
      slot = claimSlot(resid);

    if (!slot) {
      static std::atomic<uint32_t> s_full = { 0u };
      const uint32_t n = s_full.fetch_add(1u) + 1u;
      if (n == 1u || (n % 512u) == 0u)
        Logger::warn(str::format("HeliosPresentSync: table FULL, publish dropped (x", n, ")"));
      return false;
    }

    // Seqlock write. One producer per resid (the presenting process), so
    // there is no writer-writer race past the claim.
    ::InterlockedIncrement(&slot->seq);            // -> odd
    slot->pid = pid;
    slot->fenceId = fenceId;
    slot->value = LONG64(value);
    ::InterlockedIncrement(&slot->seq);            // -> even
    return true;
  }


  bool HeliosPresentSync::lookup(uint32_t resid, uint32_t* pid, uint32_t* fenceId, uint64_t* value) {
    std::call_once(g_mapOnce, initMapping);

    if (!g_map.slots || !resid)
      return false;

    HpsSlot* slot = findSlot(resid);
    if (!slot)
      return false;

    for (uint32_t attempt = 0; attempt < 8; attempt++) {
      const LONG seq0 = ::InterlockedCompareExchange(&slot->seq, 0, 0);
      if (seq0 & 1)
        continue;
      const uint32_t p = slot->pid;
      const uint32_t f = slot->fenceId;
      const uint64_t v = uint64_t(::InterlockedCompareExchange64(&slot->value, 0, 0));
      const LONG seq1 = ::InterlockedCompareExchange(&slot->seq, 0, 0);
      if (seq0 != seq1 || slot->resid != resid)
        continue;
      *pid = p;
      *fenceId = f;
      *value = v;
      return p != 0;
    }

    return false;
  }

}
