#include "dxvk_helios_feed_trace.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "../util/util_env.h"
#include "../util/util_flush.h"
#include "../util/log/log.h"
#include "../util/util_string.h"

namespace dxvk::helios_feed {

  namespace {

    constexpr uint64_t BinWidthNs = 5'000'000ull;
    // A short targeted run is roughly 60 seconds. Keep a five-second margin
    // while retaining a fixed roughly-10.6 MiB in-memory counter footprint.
    constexpr size_t BinCount = 13'000u;

    enum class Metric : uint32_t {
      DeferredFinish,
      ImmediateExecute,
      InlineReplayAdmitted,
      InlineReplayBytes,
      InlineReplayCapFlushes,
      InlineReplayByteCapFlushes,
      InlineReplayHardCountCapFlushes,
      InlineReplayFallbackFastPathDisabled,
      InlineReplayFallbackDisabled,
      InlineReplayFallbackIneligible,
      WaitFrameSubmitted,
      WaitFrameSubmittedCsThreadSyncNs,
      WaitFrameSubmittedSubmissionSyncNs,
      UmdRenderCallback,
      UmdRenderCallbackNs,
      UmdPresentCallback,
      UmdPresentCallbackNs,
      CsChunkEnqueued,
      CsChunkDequeued,
      CsDepthLast,
      CsDepthMax,
      CsWorkerWork,
      CsWorkerWorkNs,
      CsWorkerIdle,
      CsWorkerIdleNs,
      SubmissionEnqueued,
      SubmissionDequeued,
      SubmissionDepthLast,
      SubmissionDepthMax,
      VkQueueSubmit,
      VkQueueSubmitNs,
      VkQueueSubmitCommandBuffers,
      VkQueueSubmitWaitSemaphores,
      VkQueueSubmitSignalSemaphores,
      GetDataCalls,
      GetDataReady,
      GetDataNotReady,
      MapWriteDiscard,
      MapDoNotWait,
      MapDoNotWaitRefusal,
      MapGpuWait,
      MapGpuWaitNs,
      FlushTrackerRequestedExplicit,
      FlushTrackerRequestedImplicitSynchronization,
      FlushTrackerRequestedImplicitStrongHint,
      FlushTrackerRequestedImplicitWeakHint,
      FlushTrackerRequestedNone,
      FlushTrackerEffectiveExplicit,
      FlushTrackerEffectiveImplicitSynchronization,
      FlushTrackerEffectiveImplicitStrongHint,
      FlushTrackerEffectiveImplicitWeakHint,
      FlushTrackerEffectiveNone,
      FlushTrackerNoChunksAccepted,
      FlushTrackerNoChunksRejected,
      FlushTrackerMaxTypeCostAccepted,
      FlushTrackerMaxTypeCostRejected,
      FlushTrackerExplicitAccepted,
      FlushTrackerExplicitRejected,
      FlushTrackerStrongChunkCountAccepted,
      FlushTrackerStrongChunkCountRejected,
      FlushTrackerWeakMinimumChunkCountAccepted,
      FlushTrackerWeakMinimumChunkCountRejected,
      FlushTrackerPendingSubmissionFloorAccepted,
      FlushTrackerPendingSubmissionFloorRejected,
      FlushTrackerPendingSubmissionChunkCountAccepted,
      FlushTrackerPendingSubmissionChunkCountRejected,
      FlushTrackerNoneAccepted,
      FlushTrackerNoneRejected,
      FlushTrackerAcceptedChunkCount1,
      FlushTrackerAcceptedChunkCount2,
      FlushTrackerAcceptedChunkCount3,
      FlushTrackerAcceptedChunkCount4,
      FlushTrackerAcceptedChunkCount5,
      FlushTrackerAcceptedChunkCount6,
      FlushTrackerAcceptedChunkCount7,
      FlushTrackerAcceptedChunkCount8,
      FlushTrackerAcceptedChunkCount9,
      FlushTrackerAcceptedChunkCount10,
      FlushTrackerAcceptedChunkCount11,
      FlushTrackerAcceptedChunkCount12,
      FlushTrackerAcceptedChunkCount13,
      FlushTrackerAcceptedChunkCount14,
      FlushTrackerAcceptedChunkCount15,
      FlushTrackerAcceptedChunkCount16,
      FlushTrackerAcceptedChunkCount17,
      FlushTrackerAcceptedChunkCount18,
      FlushTrackerAcceptedChunkCount19,
      FlushTrackerAcceptedChunkCount20,
      FlushTrackerAcceptedChunkCount21OrMore,
      FlushTrackerAcceptedPendingSubmissions0,
      FlushTrackerAcceptedPendingSubmissions1,
      FlushTrackerAcceptedPendingSubmissions2,
      FlushTrackerAcceptedPendingSubmissions3,
      FlushTrackerAcceptedPendingSubmissions4,
      FlushTrackerAcceptedPendingSubmissions5,
      FlushTrackerAcceptedPendingSubmissions6,
      FlushTrackerAcceptedPendingSubmissions7OrMore,
      ExecuteFlushExplicitNonEmpty,
      ExecuteFlushExplicitEmpty,
      ExecuteFlushImplicitSynchronizationNonEmpty,
      ExecuteFlushImplicitSynchronizationEmpty,
      ExecuteFlushImplicitStrongHintNonEmpty,
      ExecuteFlushImplicitStrongHintEmpty,
      ExecuteFlushImplicitWeakHintNonEmpty,
      ExecuteFlushImplicitWeakHintEmpty,
      ExecuteFlushNoneNonEmpty,
      ExecuteFlushNoneEmpty,
      Count,
    };

    constexpr size_t MetricCount = size_t(Metric::Count);

    struct Bin {
      std::array<std::atomic<uint64_t>, MetricCount> values;

      Bin() {
        for (auto& value : values)
          value.store(0u, std::memory_order_relaxed);
      }
    };

    static_assert(MetricCount == 107u);

    Metric flushTrackerTypeMetric(GpuFlushType type, bool effective) {
      switch (type) {
        case GpuFlushType::ExplicitFlush:
          return effective ? Metric::FlushTrackerEffectiveExplicit
            : Metric::FlushTrackerRequestedExplicit;
        case GpuFlushType::ImplicitSynchronization:
          return effective ? Metric::FlushTrackerEffectiveImplicitSynchronization
            : Metric::FlushTrackerRequestedImplicitSynchronization;
        case GpuFlushType::ImplicitStrongHint:
          return effective ? Metric::FlushTrackerEffectiveImplicitStrongHint
            : Metric::FlushTrackerRequestedImplicitStrongHint;
        case GpuFlushType::ImplicitWeakHint:
          return effective ? Metric::FlushTrackerEffectiveImplicitWeakHint
            : Metric::FlushTrackerRequestedImplicitWeakHint;
        case GpuFlushType::None:
          return effective ? Metric::FlushTrackerEffectiveNone
            : Metric::FlushTrackerRequestedNone;
      }

      return effective ? Metric::FlushTrackerEffectiveNone
        : Metric::FlushTrackerRequestedNone;
    }

    Metric flushTrackerBranchMetric(GpuFlushTrackerBranch branch, bool accepted) {
      static_assert(uint32_t(GpuFlushTrackerBranch::None) == 7u);

      return Metric(uint32_t(Metric::FlushTrackerNoChunksAccepted)
        + 2u * uint32_t(branch) + (accepted ? 0u : 1u));
    }

    Metric executeFlushMetric(GpuFlushType type, bool nonEmpty) {
      switch (type) {
        case GpuFlushType::ExplicitFlush:
          return nonEmpty ? Metric::ExecuteFlushExplicitNonEmpty
            : Metric::ExecuteFlushExplicitEmpty;
        case GpuFlushType::ImplicitSynchronization:
          return nonEmpty ? Metric::ExecuteFlushImplicitSynchronizationNonEmpty
            : Metric::ExecuteFlushImplicitSynchronizationEmpty;
        case GpuFlushType::ImplicitStrongHint:
          return nonEmpty ? Metric::ExecuteFlushImplicitStrongHintNonEmpty
            : Metric::ExecuteFlushImplicitStrongHintEmpty;
        case GpuFlushType::ImplicitWeakHint:
          return nonEmpty ? Metric::ExecuteFlushImplicitWeakHintNonEmpty
            : Metric::ExecuteFlushImplicitWeakHintEmpty;
        case GpuFlushType::None:
          return nonEmpty ? Metric::ExecuteFlushNoneNonEmpty
            : Metric::ExecuteFlushNoneEmpty;
      }

      return nonEmpty ? Metric::ExecuteFlushNoneNonEmpty
        : Metric::ExecuteFlushNoneEmpty;
    }

    class Trace {

    public:

      Trace()
      : m_start(std::chrono::steady_clock::now()),
        m_startMonotonicNs(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          m_start.time_since_epoch()).count())),
        m_startUnixNs(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count())) { }

      void add(Metric metric, uint64_t value = 1u) {
        if (auto* bin = getBin())
          bin->values[size_t(metric)].fetch_add(value, std::memory_order_relaxed);
        else
          m_overflow.fetch_add(value, std::memory_order_relaxed);
      }

      void addDuration(Metric count, Metric duration, uint64_t durationNs) {
        if (auto* bin = getBin()) {
          bin->values[size_t(count)].fetch_add(1u, std::memory_order_relaxed);
          bin->values[size_t(duration)].fetch_add(durationNs, std::memory_order_relaxed);
        } else {
          m_overflow.fetch_add(1u, std::memory_order_relaxed);
        }
      }

      void addDepth(Metric count, Metric last, Metric max, uint64_t depth,
          uint64_t countValue = 1u) {
        if (auto* bin = getBin()) {
          auto& values = bin->values;
          values[size_t(count)].fetch_add(countValue, std::memory_order_relaxed);
          values[size_t(last)].store(depth, std::memory_order_relaxed);

          auto& maximum = values[size_t(max)];
          uint64_t observed = maximum.load(std::memory_order_relaxed);
          while (observed < depth && !maximum.compare_exchange_weak(observed, depth,
            std::memory_order_relaxed, std::memory_order_relaxed)) { }
        } else {
          m_overflow.fetch_add(countValue, std::memory_order_relaxed);
        }
      }

      void addPair(Metric first, Metric second) {
        if (auto* bin = getBin()) {
          bin->values[size_t(first)].fetch_add(1u, std::memory_order_relaxed);
          bin->values[size_t(second)].fetch_add(1u, std::memory_order_relaxed);
        } else {
          m_overflow.fetch_add(1u, std::memory_order_relaxed);
        }
      }

      void addSubmit(uint64_t durationNs, uint64_t commandBufferCount,
          uint64_t waitSemaphoreCount, uint64_t signalSemaphoreCount) {
        if (auto* bin = getBin()) {
          auto& values = bin->values;
          values[size_t(Metric::VkQueueSubmit)].fetch_add(1u, std::memory_order_relaxed);
          values[size_t(Metric::VkQueueSubmitNs)].fetch_add(durationNs,
            std::memory_order_relaxed);
          values[size_t(Metric::VkQueueSubmitCommandBuffers)].fetch_add(commandBufferCount,
            std::memory_order_relaxed);
          values[size_t(Metric::VkQueueSubmitWaitSemaphores)].fetch_add(waitSemaphoreCount,
            std::memory_order_relaxed);
          values[size_t(Metric::VkQueueSubmitSignalSemaphores)].fetch_add(signalSemaphoreCount,
            std::memory_order_relaxed);
        } else {
          m_overflow.fetch_add(1u, std::memory_order_relaxed);
        }
      }

      void addWaitFrameSubmitted(uint64_t csThreadSyncNs, uint64_t submissionSyncNs) {
        if (auto* bin = getBin()) {
          auto& values = bin->values;
          values[size_t(Metric::WaitFrameSubmitted)].fetch_add(1u, std::memory_order_relaxed);
          values[size_t(Metric::WaitFrameSubmittedCsThreadSyncNs)].fetch_add(
            csThreadSyncNs, std::memory_order_relaxed);
          values[size_t(Metric::WaitFrameSubmittedSubmissionSyncNs)].fetch_add(
            submissionSyncNs, std::memory_order_relaxed);
        } else {
          m_overflow.fetch_add(1u, std::memory_order_relaxed);
        }
      }

      void addFlushTrackerDecision(const GpuFlushTrackerDiagnostic& diagnostic) {
        if (auto* bin = getBin()) {
          auto& values = bin->values;
          values[size_t(flushTrackerTypeMetric(diagnostic.requestedType, false))].fetch_add(
            1u, std::memory_order_relaxed);
          values[size_t(flushTrackerTypeMetric(diagnostic.effectiveType, true))].fetch_add(
            1u, std::memory_order_relaxed);
          values[size_t(flushTrackerBranchMetric(diagnostic.branch, diagnostic.accepted))].fetch_add(
            1u, std::memory_order_relaxed);

          if (diagnostic.accepted) {
            const uint32_t chunkBucket = diagnostic.chunkCount < 21u
              ? diagnostic.chunkCount - 1u : 20u;
            const uint32_t pendingBucket = diagnostic.pendingSubmissionCount < 7u
              ? diagnostic.pendingSubmissionCount : 7u;
            values[size_t(Metric::FlushTrackerAcceptedChunkCount1) + chunkBucket].fetch_add(
              1u, std::memory_order_relaxed);
            values[size_t(Metric::FlushTrackerAcceptedPendingSubmissions0) + pendingBucket].fetch_add(
              1u, std::memory_order_relaxed);
          }
        } else {
          m_overflow.fetch_add(1u, std::memory_order_relaxed);
        }
      }

      void dump() {
        // A process can create and tear down probe devices before the
        // workload device. Serialize re-dumps and overwrite the same PID
        // file so the final clean teardown captures all bins accumulated by
        // this process. Do not call this from DLL detachment.
        std::lock_guard lock(m_dumpMutex);

        const std::string path = outputPath();
        std::ofstream file(path, std::ios::out | std::ios::trunc);

        if (!file) {
          Logger::warn(str::format("Helios DXVK feed trace: cannot open ", path));
          return;
        }

        file << "elapsed_ms,bin_start_monotonic_ns,bin_start_unix_ns,deferred_finish,immediate_execute,"
          "inline_replay_admitted,inline_replay_bytes,inline_replay_cap_flushes,"
          "inline_replay_byte_cap_flushes,inline_replay_hard_count_cap_flushes,"
          "inline_fallback_fastpath_disabled,"
          "inline_fallback_disabled,inline_fallback_ineligible,"
          "wait_frame_submitted,wait_frame_submitted_cs_thread_sync_ns,"
          "wait_frame_submitted_submission_sync_ns,"
          "umd_render_callback,umd_render_callback_ns,"
          "umd_present_callback,umd_present_callback_ns,"
          "cs_chunk_enqueue,cs_chunk_dequeue,cs_depth_last,cs_depth_max,"
          "cs_worker_work,cs_worker_work_ns,cs_worker_idle,cs_worker_idle_ns,"
          "submission_enqueue,submission_dequeue,submission_depth_last,"
          "submission_depth_max,vk_queue_submit,vk_queue_submit_ns,"
          "vk_queue_submit_command_buffers,vk_queue_submit_wait_semaphores,"
          "vk_queue_submit_signal_semaphores,"
          "getdata_calls,getdata_ready,getdata_not_ready,map_write_discard,"
          "map_do_not_wait,map_do_not_wait_refusal,map_gpu_wait,map_gpu_wait_ns,"
          "flush_tracker_requested_explicit,"
          "flush_tracker_requested_implicit_synchronization,"
          "flush_tracker_requested_implicit_strong_hint,"
          "flush_tracker_requested_implicit_weak_hint,flush_tracker_requested_none,"
          "flush_tracker_effective_explicit,"
          "flush_tracker_effective_implicit_synchronization,"
          "flush_tracker_effective_implicit_strong_hint,"
          "flush_tracker_effective_implicit_weak_hint,flush_tracker_effective_none,"
          "flush_tracker_no_chunks_accepted,flush_tracker_no_chunks_rejected,"
          "flush_tracker_max_type_cost_accepted,flush_tracker_max_type_cost_rejected,"
          "flush_tracker_explicit_accepted,flush_tracker_explicit_rejected,"
          "flush_tracker_strong_chunk_count_accepted,"
          "flush_tracker_strong_chunk_count_rejected,"
          "flush_tracker_weak_minimum_chunk_count_accepted,"
          "flush_tracker_weak_minimum_chunk_count_rejected,"
          "flush_tracker_pending_submission_floor_accepted,"
          "flush_tracker_pending_submission_floor_rejected,"
          "flush_tracker_pending_submission_chunk_count_accepted,"
          "flush_tracker_pending_submission_chunk_count_rejected,"
          "flush_tracker_none_accepted,flush_tracker_none_rejected,"
          "flush_tracker_accepted_chunk_count_1,flush_tracker_accepted_chunk_count_2,"
          "flush_tracker_accepted_chunk_count_3,flush_tracker_accepted_chunk_count_4,"
          "flush_tracker_accepted_chunk_count_5,flush_tracker_accepted_chunk_count_6,"
          "flush_tracker_accepted_chunk_count_7,flush_tracker_accepted_chunk_count_8,"
          "flush_tracker_accepted_chunk_count_9,flush_tracker_accepted_chunk_count_10,"
          "flush_tracker_accepted_chunk_count_11,flush_tracker_accepted_chunk_count_12,"
          "flush_tracker_accepted_chunk_count_13,flush_tracker_accepted_chunk_count_14,"
          "flush_tracker_accepted_chunk_count_15,flush_tracker_accepted_chunk_count_16,"
          "flush_tracker_accepted_chunk_count_17,flush_tracker_accepted_chunk_count_18,"
          "flush_tracker_accepted_chunk_count_19,flush_tracker_accepted_chunk_count_20,"
          "flush_tracker_accepted_chunk_count_21_or_more,"
          "flush_tracker_accepted_pending_submissions_0,"
          "flush_tracker_accepted_pending_submissions_1,"
          "flush_tracker_accepted_pending_submissions_2,"
          "flush_tracker_accepted_pending_submissions_3,"
          "flush_tracker_accepted_pending_submissions_4,"
          "flush_tracker_accepted_pending_submissions_5,"
          "flush_tracker_accepted_pending_submissions_6,"
          "flush_tracker_accepted_pending_submissions_7_or_more,"
          "execute_flush_explicit_nonempty,execute_flush_explicit_empty,"
          "execute_flush_implicit_synchronization_nonempty,"
          "execute_flush_implicit_synchronization_empty,"
          "execute_flush_implicit_strong_hint_nonempty,"
          "execute_flush_implicit_strong_hint_empty,"
          "execute_flush_implicit_weak_hint_nonempty,"
          "execute_flush_implicit_weak_hint_empty,"
          "execute_flush_none_nonempty,execute_flush_none_empty,"
          "events_after_capture_window\n";

        const uint64_t overflow = m_overflow.load(std::memory_order_relaxed);

        const size_t lastBin = getLastBin();
        for (size_t i = 0; i <= lastBin; i++) {
          const auto& values = m_bins[i].values;
          file << (i * BinWidthNs / 1'000'000ull)
            << ',' << (m_startMonotonicNs + i * BinWidthNs)
            << ',' << (m_startUnixNs + i * BinWidthNs);
          for (const auto& value : values)
            file << ',' << value.load(std::memory_order_relaxed);
          file << ',' << (i == 0u ? overflow : 0u) << '\n';
        }

        file.close();
        Logger::info(str::format("Helios DXVK feed trace: wrote ", path));
      }

    private:

      std::array<Bin, BinCount>                        m_bins;
      std::chrono::steady_clock::time_point             m_start;
      uint64_t                                          m_startMonotonicNs;
      uint64_t                                          m_startUnixNs;
      std::atomic<uint64_t>                             m_overflow = { 0u };
      std::mutex                                        m_dumpMutex;

      Bin* getBin() {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
          now - m_start).count();
        const uint64_t index = elapsed > 0 ? uint64_t(elapsed) / BinWidthNs : 0u;
        return index < BinCount ? &m_bins[size_t(index)] : nullptr;
      }

      size_t getLastBin() const {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
          now - m_start).count();
        const uint64_t index = elapsed > 0 ? uint64_t(elapsed) / BinWidthNs : 0u;
        return index < BinCount ? size_t(index) : BinCount - 1u;
      }

      static uint32_t processId() {
#ifdef _WIN32
        return uint32_t(::GetCurrentProcessId());
#else
        return uint32_t(::getpid());
#endif
      }

      static std::string outputPath() {
        std::string path = env::getEnvVar("HELIOS_DXVK_FEED_FILE");

        if (path.empty()) {
#ifdef _WIN32
          path = "C:\\ProgramData\\Helios\\dxvk-feed";
#else
          path = "/tmp/dxvk-feed";
#endif
        } else if (path.back() == '/' || path.back() == '\\') {
          path += "dxvk-feed";
        } else {
          const size_t extension = env::matchFileExtension(path, "csv");
          if (extension != std::string::npos)
            path.erase(extension);
        }

        return path + "-" + std::to_string(processId()) + ".csv";
      }
    };

    bool isEnabled() {
      static const bool enabled = env::getEnvVar("HELIOS_DXVK_FEED_TRACE") == "1";
      return enabled;
    }

    Trace& trace() {
      static Trace instance;
      return instance;
    }

    void add(Metric metric, uint64_t value = 1u) {
      if (isEnabled())
        trace().add(metric, value);
    }

    void addDepth(Metric count, Metric last, Metric maximum, uint64_t depth,
        uint64_t countValue = 1u) {
      if (isEnabled())
        trace().addDepth(count, last, maximum, depth, countValue);
    }

    void addDuration(Metric count, Metric duration, uint64_t durationNs) {
      if (isEnabled())
        trace().addDuration(count, duration, durationNs);
    }

    void addSubmit(uint64_t durationNs, uint64_t commandBufferCount,
        uint64_t waitSemaphoreCount, uint64_t signalSemaphoreCount) {
      if (isEnabled())
        trace().addSubmit(durationNs, commandBufferCount,
          waitSemaphoreCount, signalSemaphoreCount);
    }

  }

  bool enabled() {
    return isEnabled();
  }

  void initialize() {
    if (isEnabled())
      trace();
  }

  void deferredFinish() {
    add(Metric::DeferredFinish);
  }

  void immediateExecute() {
    add(Metric::ImmediateExecute);
  }

  void inlineReplayAdmitted() {
    add(Metric::InlineReplayAdmitted);
  }

  void inlineReplayBytes(uint64_t bytes) {
    add(Metric::InlineReplayBytes, bytes);
  }

  void inlineReplayCapFlush() {
    add(Metric::InlineReplayCapFlushes);
  }

  void inlineReplayByteCapFlush() {
    add(Metric::InlineReplayByteCapFlushes);
  }

  void inlineReplayHardCountCapFlush() {
    add(Metric::InlineReplayHardCountCapFlushes);
  }

  void inlineReplayFallbackFastPathDisabled() {
    add(Metric::InlineReplayFallbackFastPathDisabled);
  }

  void inlineReplayFallbackDisabled() {
    add(Metric::InlineReplayFallbackDisabled);
  }

  void inlineReplayFallbackIneligible() {
    add(Metric::InlineReplayFallbackIneligible);
  }

  void flushTrackerDecision(const GpuFlushTrackerDiagnostic& diagnostic) {
    if (isEnabled())
      trace().addFlushTrackerDecision(diagnostic);
  }

  void executeFlush(GpuFlushType type, bool nonEmpty) {
    add(executeFlushMetric(type, nonEmpty));
  }

  void waitFrameSubmitted(uint64_t csThreadSyncNs, uint64_t submissionSyncNs) {
    if (isEnabled())
      trace().addWaitFrameSubmitted(csThreadSyncNs, submissionSyncNs);
  }

  uint64_t timestampNs() {
    if (!isEnabled())
      return 0u;

    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  }

  void umdRenderCallback(uint64_t durationNs) {
    addDuration(Metric::UmdRenderCallback, Metric::UmdRenderCallbackNs, durationNs);
  }

  void umdPresentCallback(uint64_t durationNs) {
    addDuration(Metric::UmdPresentCallback, Metric::UmdPresentCallbackNs, durationNs);
  }

  void csChunkEnqueued(uint64_t depth) {
    addDepth(Metric::CsChunkEnqueued, Metric::CsDepthLast, Metric::CsDepthMax, depth);
  }

  void csChunkDequeued(uint64_t count, uint64_t depth) {
    // A worker swap can drain multiple chunks atomically. Count all chunks
    // against the same boundary without taking a timestamp per chunk.
    addDepth(Metric::CsChunkDequeued, Metric::CsDepthLast, Metric::CsDepthMax,
      depth, count);
  }

  void csWorkerWork(uint64_t durationNs) {
    addDuration(Metric::CsWorkerWork, Metric::CsWorkerWorkNs, durationNs);
  }

  void csWorkerIdle(uint64_t durationNs) {
    addDuration(Metric::CsWorkerIdle, Metric::CsWorkerIdleNs, durationNs);
  }

  void submissionEnqueued(uint64_t depth) {
    addDepth(Metric::SubmissionEnqueued, Metric::SubmissionDepthLast,
      Metric::SubmissionDepthMax, depth);
  }

  void submissionDequeued(uint64_t depth) {
    addDepth(Metric::SubmissionDequeued, Metric::SubmissionDepthLast,
      Metric::SubmissionDepthMax, depth);
  }

  void vkQueueSubmit(
          uint64_t durationNs,
          uint64_t commandBufferCount,
          uint64_t waitSemaphoreCount,
          uint64_t signalSemaphoreCount) {
    addSubmit(durationNs, commandBufferCount,
      waitSemaphoreCount, signalSemaphoreCount);
  }

  void getData(bool ready) {
    if (isEnabled()) {
      trace().addPair(Metric::GetDataCalls,
        ready ? Metric::GetDataReady : Metric::GetDataNotReady);
    }
  }

  void mapWriteDiscard() {
    add(Metric::MapWriteDiscard);
  }

  void mapDoNotWait() {
    add(Metric::MapDoNotWait);
  }

  void mapDoNotWaitRefusal() {
    add(Metric::MapDoNotWaitRefusal);
  }

  void mapGpuWait(uint64_t durationNs) {
    addDuration(Metric::MapGpuWait, Metric::MapGpuWaitNs, durationNs);
  }

  void dump() {
    if (isEnabled())
      trace().dump();
  }

}
