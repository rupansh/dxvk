#include "dxvk_helios_present_sync.h"

#include "../util/log/log.h"
#include "../util/util_string.h"
#include "../util/util_env.h"

#include <aclapi.h>
#include <sddl.h>
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <mutex>

namespace dxvk {

  namespace {

    constexpr uint32_t HpsMagic     = 0x32535048u; // 'HPS2'
    constexpr uint32_t HpsSlotCount = 4096u;
    constexpr uint32_t HpsFenceIdKwaitBit = 0x40000000u;
    constexpr const char* HpsDefaultPath =
      "C:\\ProgramData\\Helios\\helios_present_sync_v2.bin";

    // HPS2 is written by ordinary D3D producers and read/written by two
    // cross-principal consumers: dwm (Window Manager\DWM-N) and RDPIDD
    // (WUDFHost as LOCAL SERVICE). ProgramData's inherited ACL only grants the
    // latter read access, so a file first created by dwm makes every RDP read
    // silently unordered. Keep the table private to authenticated local
    // principals while granting the three identities that use it read/write.
    constexpr const char* HpsDefaultSddl =
      "D:P"
      "(A;;GA;;;SY)"                  // Local System
      "(A;;GA;;;BA)"                  // Built-in administrators
      "(A;;GRGW;;;AU)"                // Interactive D3D producers
      "(A;;GRGW;;;LS)"                // RDPIDD / WUDFHost
      "(A;;GRGW;;;S-1-5-90-0)";       // Window Manager group

    struct HpsHeader {
      uint32_t magic;
      uint32_t slotCount;
      uint32_t reserved[6];
    };

    // 32 bytes. The combined `(seq, resid)` 64-bit state makes key changes
    // atomic against readers and old HPS2 writers. PID plus creation time is
    // retained for safe stale-producer recycling.
    struct HpsSlot {
      volatile LONG seq;
      uint32_t      resid;
      uint32_t      pid;
      uint32_t      fenceId;
      volatile LONG64 value;
      uint64_t      producerStart;
    };

    constexpr uint32_t HpsMappingBytes =
      sizeof(HpsHeader) + HpsSlotCount * sizeof(HpsSlot);
    static_assert(sizeof(HpsHeader) == 32);
    static_assert(sizeof(HpsSlot) == 32);
    static_assert(HpsMappingBytes == 131104u);
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
    std::once_flag g_reclaimOnce;

    void initMapping() {
      std::string path = env::getEnvVar("HELIOS_PRESENT_SYNC_PATH");
      const bool defaultPath = path.empty();
      if (defaultPath)
        path = HpsDefaultPath;

      PSECURITY_DESCRIPTOR descriptor = nullptr;
      SECURITY_ATTRIBUTES attributes = { };
      SECURITY_ATTRIBUTES* createAttributes = nullptr;
      DWORD aclRepairError = ERROR_SUCCESS;

      if (defaultPath) {
        if (::ConvertStringSecurityDescriptorToSecurityDescriptorA(
              HpsDefaultSddl, SDDL_REVISION_1, &descriptor, nullptr)) {
          attributes.nLength = sizeof(attributes);
          attributes.lpSecurityDescriptor = descriptor;
          attributes.bInheritHandle = FALSE;
          createAttributes = &attributes;

          // Security attributes only affect a newly-created file. Repair an
          // HPS2 file left by an older build as well. The first dwm process is
          // normally its owner and can update the DACL; the package installer
          // performs the same repair before any graphics process starts.
          BOOL daclPresent = FALSE;
          BOOL daclDefaulted = FALSE;
          PACL dacl = nullptr;
          if (::GetSecurityDescriptorDacl(
                descriptor, &daclPresent, &dacl, &daclDefaulted)
           && daclPresent) {
            aclRepairError = ::SetNamedSecurityInfoA(
              const_cast<char*>(path.c_str()), SE_FILE_OBJECT,
              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
              nullptr, nullptr, dacl, nullptr);
            if (aclRepairError == ERROR_FILE_NOT_FOUND
             || aclRepairError == ERROR_PATH_NOT_FOUND)
              aclRepairError = ERROR_SUCCESS;
          }
        } else {
          Logger::warn(str::format(
            "HeliosPresentSync: could not build default file security: ",
            ::GetLastError()));
        }
      }

      HANDLE file = ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        createAttributes, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      const DWORD openError = ::GetLastError();
      if (descriptor)
        ::LocalFree(descriptor);
      if (file == INVALID_HANDLE_VALUE) {
        Logger::warn(str::format("HeliosPresentSync: CreateFile(", path,
          ") failed: ", openError,
          aclRepairError != ERROR_SUCCESS
            ? str::format(" (ACL repair failed: ", aclRepairError, ")")
            : std::string()));
        return;
      }

      HANDLE mapping = ::CreateFileMappingA(file, nullptr, PAGE_READWRITE, 0,
        HpsMappingBytes, nullptr);
      const DWORD mapErr = ::GetLastError();
      ::CloseHandle(file);
      if (!mapping) {
        Logger::warn(str::format("HeliosPresentSync: CreateFileMapping failed: ", mapErr));
        return;
      }

      void* view = ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, HpsMappingBytes);
      ::CloseHandle(mapping);
      if (!view) {
        Logger::warn(str::format("HeliosPresentSync: MapViewOfFile failed: ", ::GetLastError()));
        return;
      }

      auto* header = reinterpret_cast<HpsHeader*>(view);
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
      g_map.slots = reinterpret_cast<HpsSlot*>(header + 1);
      Logger::info(str::format("HeliosPresentSync: HPS2 table mapped (", path,
        ", ", HpsSlotCount, " slots, ", HpsMappingBytes, " bytes)"));
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

    bool producerAlive(uint32_t pid, uint64_t producerStart) {
      if (!pid)
        return false;
      HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (!proc) {
        // ERROR_INVALID_PARAMETER is the documented no-such-process result.
        // Any other refusal is ambiguous, so preserve the slot: startup
        // reclamation must never evict a producer merely because it could not
        // prove that producer's exact generation.
        return ::GetLastError() != ERROR_INVALID_PARAMETER;
      }
      DWORD code = 0;
      if (!::GetExitCodeProcess(proc, &code)) {
        ::CloseHandle(proc);
        return true;
      }
      if (code != STILL_ACTIVE) {
        ::CloseHandle(proc);
        return false;
      }
      FILETIME creation = { }, exit = { }, kernel = { }, user = { };
      if (!::GetProcessTimes(proc, &creation, &exit, &kernel, &user)) {
        ::CloseHandle(proc);
        return true;
      }
      const uint64_t start =
        (uint64_t(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
      ::CloseHandle(proc);
      return start == producerStart;
    }

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
        reinterpret_cast<volatile LONG64*>(&slot->seq), LONG64(unlocked),
        LONG64(expected))) == expected;
    }

    bool freeSlot(HpsSlot* slot, LONG unlockedSeq, uint32_t resid) {
      const uint64_t expected = packSlotState(unlockedSeq, resid);
      const uint64_t freed = packSlotState(unlockedSeq, 0u);
      return uint64_t(::InterlockedCompareExchange64(
        reinterpret_cast<volatile LONG64*>(&slot->seq), LONG64(freed),
        LONG64(expected))) == expected;
    }

    void writeSlotPayload(HpsSlot* slot, uint32_t pid, uint32_t fenceId,
        uint64_t value, uint64_t producerStart, bool kwaitOrdered) {
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

    void reclaimStaleSlots() {
      uint32_t reclaimed = 0u;

      for (uint32_t i = 0; i < HpsSlotCount; i++) {
        HpsSlot* slot = &g_map.slots[i];
        const uint64_t state = readSlotState(slot);
        const uint32_t resid = stateResid(state);
        if (!resid || (stateSeq(state) & 1))
          continue;

        const uint32_t pid = slot->pid;
        const uint64_t start = uint64_t(::InterlockedCompareExchange64(
          reinterpret_cast<volatile LONG64*>(&slot->producerStart), 0, 0));
        if (readSlotState(slot) != state || producerAlive(pid, start))
          continue;

        LONG writeSeq = 0;
        if (!tryClaimSlot(slot, state, resid, &writeSeq))
          continue;

        // Re-check the complete producer generation under the writer epoch.
        // Another publisher may have replaced this resid between the liveness
        // query and our CAS even when the numeric resid itself stayed equal.
        if (slot->pid != pid || slot->producerStart != start) {
          (void)unlockSlot(slot, writeSeq, resid);
          continue;
        }

        clearSlot(slot);
        if (!unlockSlot(slot, writeSeq, resid))
          continue;
        if (freeSlot(slot, LONG(uint32_t(writeSeq) + 1u), resid))
          reclaimed += 1u;
      }

      if (reclaimed) {
        Logger::info(str::format("HeliosPresentSync: reclaimed ", reclaimed,
          " stale HPS2 slots at map startup"));
      }
    }

    void ensureMapping() {
      std::call_once(g_mapOnce, initMapping);
      if (g_map.slots)
        std::call_once(g_reclaimOnce, reclaimStaleSlots);
    }

  }

  bool HeliosPresentSync::publish(uint32_t resid, uint32_t pid, uint32_t fenceId,
      uint64_t value, bool kwaitOrdered) {
    ensureMapping();
    if (!g_map.slots || !resid)
      return false;
    const uint64_t start = selfStartTime();
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
        if (slot->resid == resid) {
          writeSlotPayload(slot, pid, fenceId, value, start, kwaitOrdered);
          return unlockSlot(slot, writeSeq, resid);
        }
        if (!unlockSlot(slot, writeSeq, resid))
          return false;
      }

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

      for (uint32_t i = 0; i < HpsSlotCount; i++) {
        HpsSlot* slot = &g_map.slots[i];
        const uint64_t state = readSlotState(slot);
        if (!stateResid(state) || (stateSeq(state) & 1))
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
      Logger::warn(str::format("HeliosPresentSync: HPS2 table FULL (", HpsSlotCount,
        " slots), publish dropped (x", n, ")"));
    return false;
  }

  bool HeliosPresentSync::release(uint32_t resid, uint32_t fenceId) {
    ensureMapping();
    if (!g_map.slots || !resid || !fenceId)
      return false;
    const uint32_t pid = uint32_t(::GetCurrentProcessId());
    const uint64_t start = selfStartTime();
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
        if (!unlockSlot(slot, writeSeq, resid))
          return false;
        return freeSlot(slot, LONG(uint32_t(writeSeq) + 1u), resid);
      }
    }
    return false;
  }

  bool HeliosPresentSync::lookup(uint32_t resid, uint32_t* pid, uint32_t* fenceId,
      uint64_t* producerStart, uint64_t* value, bool* kwaitOrdered) {
    ensureMapping();
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
        const uint64_t start = uint64_t(::InterlockedCompareExchange64(
          reinterpret_cast<volatile LONG64*>(&slot->producerStart), 0, 0));
        const uint64_t v = uint64_t(::InterlockedCompareExchange64(&slot->value, 0, 0));
        if (readSlotState(slot) != state0)
          continue;
        *pid = p;
        *fenceId = f & ~HpsFenceIdKwaitBit;
        if (producerStart)
          *producerStart = start;
        *value = v;
        if (kwaitOrdered)
          *kwaitOrdered = (f & HpsFenceIdKwaitBit) != 0u;
        return p != 0 && start != 0;
      }
    }
    return false;
  }

  uint64_t HeliosPresentSync::processStartTime() {
    return selfStartTime();
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
