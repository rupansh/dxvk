#pragma once

#include <cstdint>

namespace dxvk {

  /**
   * \brief Helios cross-process present-ordering publication (WS1 #4)
   *
   * A producer (dwm's D3D11 device) publishes, per presented venus resource
   * id, the (producer pid, fence id, timeline value) triple whose named
   * present fence (\c Global\HeliosPresentFence_<pid>_<fenceId> — the fence
   * id disambiguates multiple D3D11 devices in one producer process, dwm
   * creates several — a WDDM monitored fence signaled by
   * the producer ICD's retire thread at HOST GPU completion) reaches
   * \c value once every submission the presented frame depends on has
   * completed. A consumer (the IddCx copy path running on this same engine
   * inside WUDFHost) looks the slot up by resid before reading the imported
   * surface and waits — bounded — on the imported fence.
   *
   * Transport: a small memory-mapped FILE under ProgramData (both principals
   * demonstrably have rights there; no Global\ section namespace or handle
   * duplication involved). Slots are seqlock-protected: writers bump \c seq
   * to odd, write, bump to even; readers retry on odd/changed seq. Slot
   * claims CAS the resid field so concurrent producers never share a slot.
   */
  class HeliosPresentSync {

  public:

    /**
     * \brief Publishes (pid, value) for a presented resource id
     *
     * Producer side; called on the present path (cheap: a few relaxed
     * atomics). Fails loudly-but-throttled when the table is unavailable
     * or full.
     *
     * \c kwaitOrdered advertises that this present's FLIP is kernel-held
     * (dxgkrnl GPU-side wait) until \c value retires — a consumer that
     * finds the value unretired is running ahead of the flip and may keep
     * its current staged bytes instead of blocking (the dwm 9 ms
     * composition stalls, 28th session). Carried as bit 30 of the slot's
     * fenceId — free in both id spaces: UMD ids are a small low counter,
     * ICD ids are 0x80000000|counter.
     * \returns \c false when the slot could not be written
     */
    static bool publish(uint32_t resid, uint32_t pid, uint32_t fenceId, uint64_t value,
      bool kwaitOrdered = false);

    /**
     * \brief Looks up the latest published (pid, value) for a resource id
     *
     * \c fenceId is returned STRIPPED of the kwait flag bit; the flag is
     * reported via \c kwaitOrdered when non-null.
     * \returns \c false when no slot exists for \c resid
     */
    static bool lookup(uint32_t resid, uint32_t* pid, uint32_t* fenceId, uint64_t* value,
      bool* kwaitOrdered = nullptr);

    /**
     * \brief Counts a bind-time staleness-gate flush
     *
     * The d3d11 layer's HeliosGateStagedSrvFreshness flushes when a bound
     * staged SRV's producer published past its last re-stage (27th-session
     * fix). Uncounted, the consumer-side flush cost is invisible; the
     * total is reported in the present-wait telemetry line.
     */
    static void noteGateFlush();

    /**
     * \brief Bind-time staleness-gate flushes so far (process-wide)
     */
    static uint64_t gateFlushCount();

  };

}
