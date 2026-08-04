#pragma once

#include <cstdint>

namespace dxvk {

  enum class GpuFlushType : uint32_t;
  struct GpuFlushTrackerDiagnostic;

}

namespace dxvk::helios_feed {

  /**
   * \brief Opt-in bounded DXVK queue-feed trace
   *
   * The trace is enabled only when HELIOS_DXVK_FEED_TRACE=1. Event sites use
   * monotonic 5 ms bins and relaxed atomic updates; output is produced once at
   * clean device teardown. There is deliberately no per-event logging or I/O.
   */
  bool enabled();

  /**
   * \brief Starts the process-wide trace before worker threads are created
   *
   * This deliberately moves fixed-bin construction out of the first
   * producer event. It is a no-op unless tracing is enabled.
   */
  void initialize();

  void deferredFinish();
  void immediateExecute();

  void inlineReplayAdmitted();
  void inlineReplayBytes(uint64_t bytes);
  void inlineReplayCapFlush();
  void inlineReplayByteCapFlush();
  void inlineReplayHardCountCapFlush();
  void inlineReplayFallbackFastPathDisabled();
  void inlineReplayFallbackDisabled();
  void inlineReplayFallbackIneligible();

  void flushTrackerDecision(const GpuFlushTrackerDiagnostic& diagnostic);
  void executeFlush(GpuFlushType type, bool nonEmpty);

  void waitFrameSubmitted(
    uint64_t csThreadSyncNs,
    uint64_t submissionSyncNs);

  /**
   * \brief Monotonic timestamp for UMD callback attribution
   *
   * Returns zero while the opt-in feed trace is disabled, avoiding clock reads
   * on the production path. Enabled callers use two timestamps around the
   * runtime callback and publish only the elapsed scalar below.
   */
  uint64_t timestampNs();
  void umdRenderCallback(uint64_t durationNs);
  void umdPresentCallback(uint64_t durationNs);

  void csChunkEnqueued(uint64_t depth);
  void csChunkDequeued(uint64_t count, uint64_t depth);
  void csWorkerWork(uint64_t durationNs);
  void csWorkerIdle(uint64_t durationNs);

  void submissionEnqueued(uint64_t depth);
  void submissionDequeued(uint64_t depth);
  void vkQueueSubmit(
    uint64_t durationNs,
    uint64_t commandBufferCount,
    uint64_t waitSemaphoreCount,
    uint64_t signalSemaphoreCount);

  void getData(bool ready);
  void mapWriteDiscard();
  void mapDoNotWait();
  void mapDoNotWaitRefusal();
  void mapGpuWait(uint64_t durationNs);

  void dump();

}
