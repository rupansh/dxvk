#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dxvk {

  /**
   * \brief GPU cost estimate for various operations
   *
   * These provide only a very rough estimate for GPU execution times,
   * which can be useful to avoid GPU time-outs in some situations.
   */
  struct GpuCostEstimate {
    /** Assume that compute dispatches are much more expensive than draws
     *  regardless of workgroup counts. This is not always true, but may
     *  help account for immediate synchronization or complex shaders that
     *  we do not generally have any up-front knowledge about. */
    static constexpr uint64_t Dispatch              = 4u;
    static constexpr uint64_t DispatchIndirect      = 5u;
    /** Assume a high base cost per render pass. We're not counting draws
     *  in order to avoid splitting passes on tiling GPUs, and draw costs
     *  can vary wildly anyway. */
    static constexpr uint64_t RenderPass            = 10u;
    /** Transfer cost can vary wildly, but so do use cases. Just assume
     *  a low cost, especially since synchronization on back-to-back
     *  transfers is unlikely to be necessary. */
    static constexpr uint64_t Transfer              = 2u;

    /** Cost threshold at which submissions are always preferred */
    static constexpr uint64_t MaxCostPerSubmission  = 1'500u;
  };


  /**
   * \brief GPU context flush type
   */
  enum class GpuFlushType : uint32_t {
    /** Flush or Present called by application */
    ExplicitFlush           = 0,
    /** Function that requires GPU synchronization and
     *  may require a flush called by application */
    ImplicitSynchronization = 1,
    /** GPU command that applications are likely to synchronize
     *  with soon has been recorded into the command list */
    ImplicitStrongHint      = 2,
    /** GPU commands have been recorded and a flush should be
     *  performed if the current command list is large enough. */
    ImplicitWeakHint        = 3,

    /** No flush. Must be the highest enum value. */
    None                    = ~0u
  };


  /**
   * \brief Branch that produced a GPU flush tracker decision
   *
   * Pair this with \ref GpuFlushTrackerDiagnostic::accepted. The same
   * threshold branch can accept or reject, while the early-exit branches are
   * intrinsically one-sided.
   */
  enum class GpuFlushTrackerBranch : uint8_t {
    NoChunks,
    MaxTypeCost,
    Explicit,
    StrongChunkCount,
    WeakMinimumChunkCount,
    PendingSubmissionFloor,
    PendingSubmissionChunkCount,
    None,
  };


  /**
   * \brief Optional observation of one GPU flush tracker decision
   *
   * This records the request exactly as received and its effective type after
   * a previously missed stronger request has been folded in. It has no input
   * role in the tracker and is populated only when explicitly requested.
   */
  struct GpuFlushTrackerDiagnostic {
    GpuFlushType          requestedType;
    GpuFlushType          effectiveType;
    GpuFlushTrackerBranch branch;
    bool                  accepted;
    uint32_t              chunkCount;
    uint32_t              pendingSubmissionCount;
  };


  /**
   * \brief GPU flush tracker
   *
   * Helper class that implements a context flush
   * heuristic for various scenarios.
   */
  class GpuFlushTracker {

  public:

    GpuFlushTracker(
            GpuFlushType          maxAllowed,
            uint32_t              maxPendingSubmissionChunkCount = 20u);

    /**
     * \brief Queries type of last missed submission request
     *
     * If \c considerFlush has returned \c false, the strongest request type
     * will be tracked so that a submission can be performed as soon as the
     * corresponding heuristic allows it.
     * \returns Missed submission request type, or \c GpuFlushType::None if
     *    no submission request has been missed.
     */
    GpuFlushType getPendingType() const {
      return m_lastMissedType;
    }

    /**
     * \brief Checks whether a context flush should be performed
     *
     * Note that this modifies internal state, and depending on the
     * flush type, this may influence the decision for future flushes.
     * \param [in] flushType Flush type
     * \param [in] chunkId GPU command sequence number
     * \param [in] lastCompleteSubmissionId Last completed command submission ID
     * \param [in] estimatedCost Estimated submission cost
     * \param [out] diagnostic Optional passive decision observation
     * \returns \c true if a flush should be performed
     */
    bool considerFlush(
            GpuFlushType          flushType,
            uint64_t              chunkId,
            uint32_t              lastCompleteSubmissionId,
            uint64_t              estimatedCost,
            GpuFlushTrackerDiagnostic* diagnostic = nullptr);

    /**
     * \brief Notifies tracker about a context flush
     *
     * \param [in] chunkId GPU command sequence number
     * \param [in] submissionId Command submission ID
     */
    void notifyFlush(
            uint64_t              chunkId,
            uint64_t              submissionId);

  private:

    GpuFlushType  m_maxType               = GpuFlushType::ImplicitWeakHint;
    GpuFlushType  m_lastMissedType        = GpuFlushType::None;

    uint32_t      m_maxPendingSubmissionChunkCount = 20u;

    uint64_t      m_lastFlushChunkId      = 0ull;
    uint64_t      m_lastFlushSubmissionId = 0ull;

  };

}
