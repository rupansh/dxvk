#include "util_flush.h"
#include "util_string.h"
#include "log/log.h"

namespace dxvk {

  GpuFlushTracker::GpuFlushTracker(
          GpuFlushType          maxType,
          uint32_t              maxPendingSubmissionChunkCount)
  : m_maxType(maxType),
    m_maxPendingSubmissionChunkCount(maxPendingSubmissionChunkCount) {

  }

  bool GpuFlushTracker::considerFlush(
          GpuFlushType          flushType,
          uint64_t              chunkId,
          uint32_t              lastCompleteSubmissionId,
          uint64_t              estimatedCost,
          GpuFlushTrackerDiagnostic* diagnostic) {
    constexpr uint32_t minPendingSubmissions = 2;

    constexpr uint32_t minChunkCount =  3u;
    const GpuFlushType requestedType = flushType;

    // Do not flush if there is nothing to flush
    uint32_t chunkCount = uint32_t(chunkId - m_lastFlushChunkId);
    uint32_t pendingSubmissions = uint32_t(
      m_lastFlushSubmissionId - lastCompleteSubmissionId);

    const auto report = [&] (GpuFlushTrackerBranch branch, bool accepted) {
      if (diagnostic) {
        diagnostic->requestedType = requestedType;
        diagnostic->effectiveType = flushType;
        diagnostic->branch = branch;
        diagnostic->accepted = accepted;
        diagnostic->chunkCount = chunkCount;
        diagnostic->pendingSubmissionCount = pendingSubmissions;
      }
      return accepted;
    };

    if (!chunkCount)
      return report(GpuFlushTrackerBranch::NoChunks, false);

    // Deliberately ignore cost heuristic if we're not categorically ignoring
    // submission requests anyway, since we should never submit enough to time
    // out with the chunk-based heuristic.
    if (flushType > m_maxType)
      return report(GpuFlushTrackerBranch::MaxTypeCost,
        estimatedCost >= GpuCostEstimate::MaxCostPerSubmission);

    // Take any earlier missed flush with a stronger hint into account, so
    // that we still flush those as soon as possible. Ignore synchronization
    // commands since they will either perform a flush or not need it at all.
    flushType = std::min(flushType, m_lastMissedType);

    if (flushType != GpuFlushType::ImplicitSynchronization)
      m_lastMissedType = flushType;

    switch (flushType) {
      case GpuFlushType::ExplicitFlush: {
        // This shouldn't really be called for explicit flushes,
        // but handle them anyway for the sake of completeness
        return report(GpuFlushTrackerBranch::Explicit, true);
      }

      case GpuFlushType::ImplicitStrongHint: {
        // Flush aggressively with a strong hint to reduce readback latency.
        return report(GpuFlushTrackerBranch::StrongChunkCount,
          chunkCount >= minChunkCount);
      }

      case GpuFlushType::ImplicitWeakHint: {
        // Aim for a higher number of chunks per submission with
        // a weak hint in order to avoid submitting too often.
        if (chunkCount < 2 * minChunkCount)
          return report(GpuFlushTrackerBranch::WeakMinimumChunkCount, false);

        // Actual heuristic is shared with synchronization commands
      } [[fallthrough]];

      case GpuFlushType::ImplicitSynchronization: {
        // If the GPU is about to go idle, flush aggressively. This may be
        // required if the application is spinning on a query or resource.
        if (pendingSubmissions < minPendingSubmissions)
          return report(GpuFlushTrackerBranch::PendingSubmissionFloor, true);

        // Use the number of pending submissions to decide whether to flush. Other
        // than ignoring the minimum chunk count condition, we should treat this
        // the same as weak hints to avoid unnecessary synchronization.
        uint32_t threshold = std::min(
          m_maxPendingSubmissionChunkCount, pendingSubmissions * minChunkCount);
        return report(GpuFlushTrackerBranch::PendingSubmissionChunkCount,
          chunkCount >= threshold);
      }

      case GpuFlushType::None:
        return report(GpuFlushTrackerBranch::None, false);
    }

    // Should be unreachable
    return report(GpuFlushTrackerBranch::None, false);
  }


  void GpuFlushTracker::notifyFlush(
          uint64_t              chunkId,
          uint64_t              submissionId) {
    m_lastMissedType = GpuFlushType::None;

    m_lastFlushChunkId = chunkId;
    m_lastFlushSubmissionId = submissionId;
  }

}
