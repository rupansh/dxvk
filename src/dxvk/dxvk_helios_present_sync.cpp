#include "dxvk_helios_present_sync.h"

#include "../util/log/log.h"
#include "../util/util_string.h"
#include "../util/util_env.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <mutex>

namespace dxvk {

  namespace {

    constexpr uint32_t HpsMagic     = 0x31535048u; // 'HPS1'
    constexpr uint32_t HpsSlotCount = 64u;

    // Bit 30 of the slot's fenceId = "this value's flip is kernel-held
    // until the value retires" (kwait-ordered publish). Free in both fence
    // id spaces: UMD ids are a tiny low counter, ICD ids 0x80000000|counter.
    // Stripped by lookup(); the on-disk name is built from the real id.
    constexpr uint32_t HpsFenceIdKwaitBit = 0x40000000u;

    struct HpsHeader {
      uint32_t magic;
      uint32_t slotCount;
      uint32_t reserved[6];
    };

    // 32 bytes. `seq` is an inter-process writer lock as well as a seqlock:
    // writers CAS it even->odd before changing key or payload, then publish an
    // even value. `resid` is released only by its producer when the backing
    // resource's real VkDeviceMemory lifetime ends. A dead process's stable
    // (even) claim remains reclaimable as the crash-path backstop.
    struct HpsSlot {
      volatile LONG seq;
      uint32_t      resid;
      uint32_t      pid;
      uint32_t      fenceId;
      volatile LONG64 value;
      // Producer process CREATION TIME (FILETIME as u64): pid liveness alone
      // is wrong for recycling — the table file persists across boots and
      // Windows reuses pids, so a dead producer's slot can look alive
      // forever (observed live: 64/64 slots full of stale claims). A pid
      // whose current creation time differs from this stamp is a reused
      // pid; legacy slots carry 0 here and are recycled on first pressure.
      uint64_t      producerStart;
    };

    static_assert(sizeof(HpsHeader) == 32);
    static_assert(sizeof(HpsSlot) == 32);
    static_assert(sizeof(HpsHeader) % alignof(HpsSlot) == 0);
    static_assert(offsetof(HpsSlot, seq) == 0);
    static_assert(offsetof(HpsSlot, resid) == sizeof(LONG));
    static_assert(alignof(HpsSlot) >= alignof(LONG64));

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

    uint64_t packSlotState(LONG seq, uint32_t resid) {
      return (uint64_t(resid) << 32) | uint32_t(seq);
    }

    LONG stateSeq(uint64_t state) {
      return LONG(uint32_t(state));
    }

    uint32_t stateResid(uint64_t state) {
      return uint32_t(state >> 32);
    }

    uint64_t readSlotState(const HpsSlot* slot) {
      return uint64_t(::InterlockedCompareExchange64(
        reinterpret_cast<volatile LONG64*>(const_cast<volatile LONG*>(&slot->seq)), 0, 0));
    }

    uint64_t selfStartTime() {
      static const uint64_t s_start = [] {
        FILETIME creation = { }, exit = { }, kernel = { }, user = { };
        if (!::GetProcessTimes(::GetCurrentProcess(), &creation, &exit, &kernel, &user))
          return uint64_t(0u);
        return (uint64_t(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
      }();
      return s_start;
    }

    // A slot's producer is alive iff its pid exists AND the process' creation
    // time matches the stamp the producer wrote — anything else (dead pid,
    // reused pid, legacy zero stamp) makes the slot recyclable. A shielded
    // process (ACCESS_DENIED) is conservatively treated as alive.
    bool producerAlive(uint32_t pid, uint64_t producerStart) {
      if (!pid)
        return false;
      HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (!proc)
        return ::GetLastError() == ERROR_ACCESS_DENIED; // exists, just shielded
      DWORD code = 0;
      bool alive = ::GetExitCodeProcess(proc, &code) && code == STILL_ACTIVE;
      if (alive) {
        FILETIME creation = { }, exit = { }, kernel = { }, user = { };
        if (::GetProcessTimes(proc, &creation, &exit, &kernel, &user)) {
          const uint64_t start =
            (uint64_t(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
          alive = (start == producerStart);
        }
      }
      ::CloseHandle(proc);
      return alive;
    }

    // CAS the sequence from an even reader epoch to an odd writer epoch.
    // Every table mutation must own this lock before touching *either* key or
    // payload. We intentionally never guess that another process's odd epoch
    // is dead: without an owner field for the in-flight writer, breaking it
    // could expose a partially written tuple. A writer crash therefore poisons
    // at most one slot (fail closed); normal stale-pid recycling applies only
    // to stable even slots.
    bool tryClaimSlot(HpsSlot* slot, uint64_t expectedState,
        uint32_t newResid, LONG* writeSeq) {
      const LONG seq = stateSeq(expectedState);
      if (seq & 1)
        return false;

      const LONG locked = LONG(uint32_t(seq) + 1u);
      const LONG64 desired = LONG64(packSlotState(locked, newResid));
      if (uint64_t(::InterlockedCompareExchange64(
            reinterpret_cast<volatile LONG64*>(&slot->seq), desired,
            LONG64(expectedState))) != expectedState)
        return false;

      *writeSeq = locked;
      return true;
    }

    bool unlockSlot(HpsSlot* slot, LONG writeSeq, uint32_t resid) {
      const uint64_t expected = packSlotState(writeSeq, resid);
      const uint64_t unlocked = packSlotState(LONG(uint32_t(writeSeq) + 1u), resid);
      return uint64_t(::InterlockedCompareExchange64(
        reinterpret_cast<volatile LONG64*>(&slot->seq),
        LONG64(unlocked), LONG64(expected))) == expected;
    }

    bool freeSlot(HpsSlot* slot, LONG unlockedSeq, uint32_t resid) {
      const uint64_t expected = packSlotState(unlockedSeq, resid);
      const uint64_t freed = packSlotState(unlockedSeq, 0u);
      return uint64_t(::InterlockedCompareExchange64(
        reinterpret_cast<volatile LONG64*>(&slot->seq),
        LONG64(freed), LONG64(expected))) == expected;
    }

    void writeSlotPayload(HpsSlot* slot, uint32_t pid,
        uint32_t fenceId, uint64_t value, uint64_t producerStart,
        bool kwaitOrdered) {
      slot->pid = pid;
      slot->fenceId = fenceId | (kwaitOrdered ? HpsFenceIdKwaitBit : 0u);
      slot->value = LONG64(value);
      slot->producerStart = producerStart;
    }

    void clearSlot(HpsSlot* slot) {
      slot->pid = 0u;
      slot->fenceId = 0u;
      slot->value = 0;
      slot->producerStart = 0u;
    }

  }


  bool HeliosPresentSync::publish(uint32_t resid, uint32_t pid, uint32_t fenceId, uint64_t value,
      bool kwaitOrdered) {
    std::call_once(g_mapOnce, initMapping);

    if (!g_map.slots || !resid)
      return false;

    const uint64_t start = selfStartTime();
    if (!pid || !start)
      return false;

    // A slot can be momentarily locked by the publisher/releaser in another
    // process. Retry a bounded number of complete scans; no timing or process
    // heuristic is involved, and a permanently odd (crashed) slot stays
    // fail-closed as documented in tryLockSlot.
    for (uint32_t attempt = 0; attempt < 8; attempt++) {
      // Refresh a resource already owned by this producer first.
      for (uint32_t i = 0; i < HpsSlotCount; i++) {
        HpsSlot* slot = &g_map.slots[i];
        const uint64_t state = readSlotState(slot);
        if (stateResid(state) != resid || (stateSeq(state) & 1))
          continue;

        LONG writeSeq = 0;
        if (!tryClaimSlot(slot, state, resid, &writeSeq))
          continue;

        if (slot->resid == resid) {
          writeSlotPayload(slot, pid, fenceId, value, start, kwaitOrdered);
          return unlockSlot(slot, writeSeq, resid);
        }

        if (!unlockSlot(slot, writeSeq, resid))
          return false;
      }

      // Then claim an unused slot. The key and complete payload become visible
      // in the same writer epoch, so lookup never observes an old payload under
      // the new resid.
      for (uint32_t i = 0; i < HpsSlotCount; i++) {
        HpsSlot* slot = &g_map.slots[i];
        const uint64_t state = readSlotState(slot);
        if (stateResid(state) != 0u || (stateSeq(state) & 1))
          continue;

        LONG writeSeq = 0;
        if (!tryClaimSlot(slot, state, resid, &writeSeq))
          continue;

        if (slot->resid == resid) {
          writeSlotPayload(slot, pid, fenceId, value, start, kwaitOrdered);
          return unlockSlot(slot, writeSeq, resid);
        }

        if (!unlockSlot(slot, writeSeq, resid))
          return false;
      }

      // Finally recycle a stable slot whose exact producer instance is gone.
      // `producerAlive` runs while we own the writer epoch, so the pid/start
      // tuple cannot change underneath the liveness decision.
      for (uint32_t i = 0; i < HpsSlotCount; i++) {
        HpsSlot* slot = &g_map.slots[i];
        const uint64_t state = readSlotState(slot);
        const uint32_t oldResid = stateResid(state);
        if (!oldResid || (stateSeq(state) & 1))
          continue;

        const uint32_t oldPid = slot->pid;
        const uint64_t oldStart = uint64_t(::InterlockedCompareExchange64(
          reinterpret_cast<volatile LONG64*>(&slot->producerStart), 0, 0));
        if (readSlotState(slot) != state || producerAlive(oldPid, oldStart))
          continue;

        LONG writeSeq = 0;
        if (!tryClaimSlot(slot, state, resid, &writeSeq))
          continue;

        if (slot->resid == resid) {
          writeSlotPayload(slot, pid, fenceId, value, start, kwaitOrdered);
          return unlockSlot(slot, writeSeq, resid);
        }

        if (!unlockSlot(slot, writeSeq, resid))
          return false;
      }
    }

    static std::atomic<uint32_t> s_full = { 0u };
    const uint32_t n = s_full.fetch_add(1u) + 1u;
    if (n == 1u || (n % 512u) == 0u)
      Logger::warn(str::format("HeliosPresentSync: table FULL, publish dropped (x", n, ")"));
    return false;
  }


  bool HeliosPresentSync::release(uint32_t resid, uint32_t fenceId) {
    std::call_once(g_mapOnce, initMapping);

    if (!g_map.slots || !resid || !fenceId)
      return false;

    const uint32_t pid = uint32_t(::GetCurrentProcessId());
    const uint64_t start = selfStartTime();

    // Do not substitute pid liveness for the creation stamp. If the stamp is
    // unavailable, retaining the slot until crash-path recycling is safer
    // than risking deletion of a pid-reused producer's current publication.
    if (!pid || !start)
      return false;

    for (uint32_t attempt = 0; attempt < 8; attempt++) {
      for (uint32_t i = 0; i < HpsSlotCount; i++) {
        HpsSlot* slot = &g_map.slots[i];
        const uint64_t state = readSlotState(slot);
        if (stateResid(state) != resid || (stateSeq(state) & 1))
          continue;

        LONG writeSeq = 0;
        if (!tryClaimSlot(slot, state, resid, &writeSeq))
          continue;

        const bool owned = slot->pid == pid
                        && slot->producerStart == start
                        && (slot->fenceId & ~HpsFenceIdKwaitBit) == fenceId;
        if (owned)
          clearSlot(slot);

        if (!owned) {
          if (!unlockSlot(slot, writeSeq, resid))
            return false;
          return false;
        }

        // Keep the key nonzero until the writer epoch is completely even, then
        // free the (seq,resid) pair in one CAS. This is also safe against an
        // already-loaded legacy writer: it cannot claim resid==0 while this
        // release is in flight, and after the pair-free CAS either it wins the
        // free key or a new writer does — never both.
        if (!unlockSlot(slot, writeSeq, resid))
          return false;

        return freeSlot(slot, LONG(uint32_t(writeSeq) + 1u), resid);
      }
    }

    return false;
  }

  bool HeliosPresentSync::lookup(uint32_t resid, uint32_t* pid, uint32_t* fenceId, uint64_t* value,
      bool* kwaitOrdered) {
    std::call_once(g_mapOnce, initMapping);

    if (!g_map.slots || !resid)
      return false;

    for (uint32_t i = 0; i < HpsSlotCount; i++) {
      HpsSlot* slot = &g_map.slots[i];
      for (uint32_t attempt = 0; attempt < 8; attempt++) {
        const uint64_t state0 = readSlotState(slot);
        if (stateResid(state0) != resid)
          break;
        if (stateSeq(state0) & 1)
          continue;

        const uint32_t p = slot->pid;
        const uint32_t f = slot->fenceId;
        const uint64_t v = uint64_t(::InterlockedCompareExchange64(&slot->value, 0, 0));
        if (readSlotState(slot) != state0)
          continue;

        *pid = p;
        *fenceId = f & ~HpsFenceIdKwaitBit;
        *value = v;
        if (kwaitOrdered)
          *kwaitOrdered = (f & HpsFenceIdKwaitBit) != 0u;
        return p != 0;
      }
    }

    return false;
  }


  namespace {
    volatile LONG64 g_gateFlushes = 0;
  }

  void HeliosPresentSync::noteGateFlush() {
    ::InterlockedIncrement64(&g_gateFlushes);
  }

  uint64_t HeliosPresentSync::gateFlushCount() {
    return uint64_t(::InterlockedCompareExchange64(&g_gateFlushes, 0, 0));
  }

}
