#pragma once

#include <cstdint>

namespace dxvk {

  /**
   * \brief Helios cross-process present-ordering publication (WS1 #4)
   *
   * A producer (dwm's D3D11 device) publishes, per presented venus resource
   * id, the (producer pid, timeline value) pair whose named present fence
   * (\c Global\HeliosPresentFence_<pid>, a WDDM monitored fence signaled by
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
     * \returns \c false when the slot could not be written
     */
    static bool publish(uint32_t resid, uint32_t pid, uint64_t value);

    /**
     * \brief Looks up the latest published (pid, value) for a resource id
     * \returns \c false when no slot exists for \c resid
     */
    static bool lookup(uint32_t resid, uint32_t* pid, uint64_t* value);

  };

}
