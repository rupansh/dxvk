#include "dxvk_helios_scanout_acquire.h"

#include "dxvk_device.h"

#include "../util/log/log.h"
#include "../util/util_env.h"
#include "../util/util_string.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace dxvk {

  namespace helios_acquire {

#ifdef _WIN32

    using PfnEnabled  = bool     (__cdecl*)();
    using PfnLookup   = bool     (__cdecl*)(uint32_t, uint32_t*, uint32_t*);
    using PfnSnapshot = uint32_t (__cdecl*)(uint32_t*, uint32_t);
    using PfnResid    = uint32_t (__cdecl*)(VkDeviceMemory);

    /* The three helios_scanout_* exports live in the module CONTAINING this
     * code (helios_umd.dll — dxvk is statically linked into it). They exist
     * from DLL load or never (standalone d3d11.dll), so one resolution pass
     * decides for the module lifetime — all three or none.
     * UNCHANGED_REFCOUNT so the probe does not pin the module — the runtime
     * loads and unloads the UMD DLL once per D3D11 device (measured; a leaked
     * refcount here would silently change that lifecycle). */
    struct UmdExports {
      PfnEnabled  enabled  = nullptr;
      PfnLookup   lookup   = nullptr;
      PfnSnapshot snapshot = nullptr;
    };

    static const UmdExports* umdExports() {
      static const UmdExports s_exports = [] {
        UmdExports e = { };

        HMODULE mod = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                              | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
              reinterpret_cast<LPCWSTR>(&umdExports), &mod) || !mod)
          return e;

        e.enabled  = reinterpret_cast<PfnEnabled> (GetProcAddress(mod, "helios_scanout_acquire_enabled"));
        e.lookup   = reinterpret_cast<PfnLookup>  (GetProcAddress(mod, "helios_scanout_ledger_lookup"));
        e.snapshot = reinterpret_cast<PfnSnapshot>(GetProcAddress(mod, "helios_scanout_ledger_snapshot"));

        if (!e.enabled || !e.lookup || !e.snapshot) {
          // All-or-nothing: a partial surface is a build skew, not a mode.
          e = { };
          Logger::info("helios-acquire: helios_scanout_* exports absent - feature off");
        }

        return e;
      }();
      return &s_exports;
    }

    /* The venus ICD's memory->resid export, from the loaded-module list (the
     * ICD DLL's name is configuration; the export name is the ABI - the same
     * discipline as the UMD bridge's find_export_in_loaded_modules). Success
     * cached; a miss retries, because the ICD only loads with the first
     * DxvkInstance - though every caller here holds a live device, so a miss
     * is a stale-ICD deploy, counted by the resid-miss census counter. */
    static PfnResid residExport() {
      static std::atomic<void*> s_cache = { nullptr };

      if (void* cached = s_cache.load(std::memory_order_acquire))
        return reinterpret_cast<PfnResid>(cached);

      void* fn = nullptr;
      HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());

      if (snapshot != INVALID_HANDLE_VALUE) {
        // Explicit W flavor: the ANSI names are UNICODE-sensitive macros.
        MODULEENTRY32W module = { };
        module.dwSize = sizeof(module);

        if (Module32FirstW(snapshot, &module)) {
          do {
            fn = reinterpret_cast<void*>(GetProcAddress(module.hModule, "helios_venus_memory_res_id"));
            if (fn)
              break;
          } while (Module32NextW(snapshot, &module));
        }

        CloseHandle(snapshot);
      }

      if (fn)
        s_cache.store(fn, std::memory_order_release);

      return reinterpret_cast<PfnResid>(fn);
    }

    bool enabled() {
      const auto* e = umdExports();
      return e->enabled && e->enabled();
    }

    bool ledgerLookup(uint32_t resid, uint32_t* issued, uint32_t* retired) {
      const auto* e = umdExports();
      return e->lookup && e->lookup(resid, issued, retired);
    }

    uint32_t ledgerSnapshot(DxvkHeliosLedgerSlot* slots, uint32_t maxSlots) {
      const auto* e = umdExports();
      if (!e->snapshot || !slots || !maxSlots)
        return 0u;

      // The export writes (resid, issued, retired) u32 triples, which is
      // exactly DxvkHeliosLedgerSlot's layout.
      static_assert(sizeof(DxvkHeliosLedgerSlot) == 3u * sizeof(uint32_t));
      return e->snapshot(reinterpret_cast<uint32_t*>(slots), maxSlots);
    }

    uint32_t residFromMemory(VkDeviceMemory memory) {
      if (memory == VK_NULL_HANDLE)
        return 0u;

      PfnResid fn = residExport();
      return fn ? fn(memory) : 0u;
    }

#else

    /* Non-Windows build of the engine: no Helios UMD, no KMD ledger. */
    bool enabled() { return false; }
    bool ledgerLookup(uint32_t, uint32_t*, uint32_t*) { return false; }
    uint32_t ledgerSnapshot(DxvkHeliosLedgerSlot*, uint32_t) { return 0u; }
    uint32_t residFromMemory(VkDeviceMemory) { return 0u; }

#endif

  }


  DxvkHeliosScanoutAcquire::DxvkHeliosScanoutAcquire(DxvkDevice* device)
  : m_device(device) {

  }


  DxvkHeliosScanoutAcquire::~DxvkHeliosScanoutAcquire() {
    // Backstop only - ~DxvkDevice runs shutdown() before waitForIdle. In
    // module detachment the signaler thread is already gone and joining
    // would deadlock the loader lock; mirror ~DxvkDevice's early return.
    if (this_thread::isInModuleDetachment())
      return;

    shutdown();
  }


  void DxvkHeliosScanoutAcquire::setEventHandle(HANDLE event) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    m_event = event;
    // A signaler already polling on the condvar picks the event up next pass.
    m_cond.notify_all();
  }


  Rc<DxvkFence> DxvkHeliosScanoutAcquire::armFence(uint32_t resid, uint32_t issued, uint32_t retired) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    if (m_stop)
      return nullptr;

    auto& gate = m_gates[resid];

    if (gate.fence == nullptr) {
      // Landmine #1 (spec §5.4): a PLAIN INTERNAL timeline. The default
      // sharedType (FLAG_BITS_MAX_ENUM) means no export chain, no import, no
      // D3DKMT bookkeeping - an imported OPAQUE_WIN32 wait would degrade into
      // a blocking CPU wait inside vkQueueSubmit on the submit thread
      // (vn_queue.c sync-payload import path), the exact banned pattern.
      // Dedicated semaphore: values are the ledger's retired counter and the
      // vn VUID-03258 guard silently drops CPU signals that are not strictly
      // increasing per semaphore, so this timeline is shared with NOTHING
      // (not the tracking fence, not the present fence).
      DxvkFenceCreateInfo fenceInfo = { };
      fenceInfo.initialValue = retired;

      try {
        gate.fence = m_device->createFence(fenceInfo);
      } catch (const DxvkError& e) {
        m_gates.erase(resid);
        m_createFails.fetch_add(1u, std::memory_order_relaxed);
        Logger::err(str::format("helios-acquire: gate fence creation failed for resid ",
          resid, ": ", e.message()));
        return nullptr;
      }

      gate.lastSignaled = retired;
    }

    if (uint64_t(issued) > gate.highestArmed) {
      gate.highestArmed = uint64_t(issued);
      gate.armTime = high_resolution_clock::now();
    }

    startThreadLocked();
    return gate.fence;
  }


  void DxvkHeliosScanoutAcquire::shutdown() {
    // Spec §5.3 order: stop new arms -> signal every gate to the highest
    // value ever armed -> join the signaler -> drop the fences. Guarantees no
    // armed wait outlives its signaler; the caller (~DxvkDevice) only then
    // runs waitForIdle, which would otherwise park behind an unsatisfiable
    // TOP_OF_PIPE wait until the vn 8 s forward-progress deadline loses the
    // device.
    bool join = false;

    {
      std::lock_guard<dxvk::mutex> lock(m_mutex);
      m_stop = true;

      for (auto& entry : m_gates) {
        auto& gate = entry.second;

        if (gate.highestArmed > gate.lastSignaled)
          signalGateLocked(entry.first, gate, gate.highestArmed);
      }

      join = m_threadStarted;
      m_cond.notify_all();

#ifdef _WIN32
      if (m_event)
        SetEvent(m_event);
#endif
    }

    if (join && m_thread.joinable())
      m_thread.join();

    {
      std::lock_guard<dxvk::mutex> lock(m_mutex);
      // Drops this object's refs only: an in-flight command list waiting a
      // gate holds its own Rc until the list retires, so the semaphore
      // outlives every pending submission that names it.
      m_gates.clear();
      m_threadStarted = false;
    }
  }


  void DxvkHeliosScanoutAcquire::startThreadLocked() {
    if (m_threadStarted || m_stop)
      return;

    m_threadStarted = true;
    m_thread = dxvk::thread([this] { run(); });
  }


  void DxvkHeliosScanoutAcquire::run() {
    env::setThreadName("helios-acquire");

    while (true) {
      HANDLE event = nullptr;

      {
        std::unique_lock<dxvk::mutex> lock(m_mutex);

        if (m_stop)
          return;

#ifdef _WIN32
        event = m_event;
#endif

        if (!event) {
          // No registered retirement event (KMD table full, old KMD mid-skew,
          // or not delivered yet): pure level-triggered polling. The ledger
          // is the truth either way; the event is only a latency optimization.
          m_cond.wait_for(lock, std::chrono::milliseconds(10));

          if (m_stop)
            return;
        }
      }

#ifdef _WIN32
      if (event) {
        // Event OR timeout - LEVEL-TRIGGERED either way (landmine #4): every
        // wake re-reads the whole ledger, so a lost or coalesced KeSetEvent is
        // a <=10 ms hiccup, never a hang, and the armed-wait release latency
        // stays three orders of magnitude under the vn 8 s deadline.
        if (WaitForSingleObject(event, 10u) == WAIT_FAILED) {
          // Dead handle (teardown-order pathology): a failed wait returns
          // immediately, which would turn this loop into a hot spin. Fall
          // back to condvar pacing instead.
          std::lock_guard<dxvk::mutex> lock(m_mutex);

          if (m_event == event)
            m_event = nullptr;
        }
      }
#endif

      processRetirements();
    }
  }


  void DxvkHeliosScanoutAcquire::processRetirements() {
    DxvkHeliosLedgerSlot slots[8u] = { };
    const uint32_t n = helios_acquire::ledgerSnapshot(slots, 8u);

    std::lock_guard<dxvk::mutex> lock(m_mutex);

    for (auto it = m_gates.begin(); it != m_gates.end(); ) {
      auto& gate = it->second;

      const DxvkHeliosLedgerSlot* slot = nullptr;

      for (uint32_t i = 0; i < n; i++) {
        if (slots[i].resid == it->first) {
          slot = &slots[i];
          break;
        }
      }

      // A vanished slot means every read of this resid retired (the KMD
      // reclaims a slot only at issued == retired), and venus resource ids
      // never recycle, so it is permanent: release everything armed, then
      // drop the gate once it has caught up.
      const uint64_t target = slot ? uint64_t(slot->retired) : gate.highestArmed;

      if (target > gate.lastSignaled)
        signalGateLocked(it->first, gate, target);

      if (!slot && gate.lastSignaled >= gate.highestArmed) {
        it = m_gates.erase(it);
        continue;
      }

      ++it;
    }
  }


  void DxvkHeliosScanoutAcquire::signalGateLocked(uint32_t resid, Gate& gate, uint64_t value) {
    // CPU-signal precedent: DxvkKeyedMutex::ReleaseSync. vkSignalSemaphore
    // requires a strictly increasing value; getValue() is the venus feedback
    // read (cheap, guest-side) and is the authority on what has been reached
    // - the queue-side release of an armed wait IS this signal arriving.
    const bool coversArm = gate.highestArmed > gate.lastSignaled
                        && value >= gate.highestArmed;

    if (value > gate.fence->getValue()) {
      VkSemaphoreSignalInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
      info.semaphore = gate.fence->handle();
      info.value = value;

      auto vkd = m_device->vkd();
      VkResult vr = vkd->vkSignalSemaphore(vkd->device(), &info);

      if (vr != VK_SUCCESS) {
        m_signalFails.fetch_add(1u, std::memory_order_relaxed);
        Logger::err(str::format("helios-acquire: vkSignalSemaphore(resid ", resid,
          " -> ", value, ") failed: ", vr));
      } else {
        m_signals.fetch_add(1u, std::memory_order_relaxed);
      }
    }

    if (coversArm) {
      // arm-to-signal latency of the newest armed value, for the census line.
      const auto now = high_resolution_clock::now();
      const uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        now - gate.armTime).count());

      uint64_t prev = m_maxArmToSigUs.load(std::memory_order_relaxed);
      while (us > prev
          && !m_maxArmToSigUs.compare_exchange_weak(prev, us, std::memory_order_relaxed))
        continue;
    }

    if (value > gate.lastSignaled)
      gate.lastSignaled = value;
  }

}
