#pragma once

#include <cstdint>

namespace dxvk {

  /**
   * \brief Helios cross-process present-ordering publication (WS1 #4)
   *
   * A producer publishes, per presented Venus resource id, the producer pid,
   * fence id, and timeline value. The consumer looks the slot up before it
   * reads the imported surface and waits on the corresponding named fence.
   *
   * HPS2 is a 4096-slot file-backed seqlock table. Slots use \c seq as both
   * the writer lock and the reader epoch; pid plus creation time form the
   * producer generation. Named fences include that generation so a persistent
   * slot can never resolve to a new process that reused both pid and fence id.
   */
  class HeliosPresentSync {

  public:

    static bool publish(uint32_t resid, uint32_t pid, uint32_t fenceId, uint64_t value,
      bool kwaitOrdered = false);

    static bool release(uint32_t resid, uint32_t fenceId);

    static bool lookup(uint32_t resid, uint32_t* pid, uint32_t* fenceId,
      uint64_t* producerStart, uint64_t* value, bool* kwaitOrdered = nullptr);

    static uint64_t processStartTime();

    static void noteGateFlush();

    static uint64_t gateFlushCount();

  };

}
