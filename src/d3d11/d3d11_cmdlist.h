#pragma once

#include <functional>

#include "d3d11_context.h"
#include "../util/util_small_vector.h"

namespace dxvk {
  
  using D3D11ChunkDispatchProc = std::function<uint64_t (DxvkCsChunkRef&&, uint64_t, GpuFlushType)>;

  /**
   * \brief Replay payload for a tiny deferred command list
   *
   * The payload owns a temporary reference to each deferred chunk. It
   * releases each reference as soon as the chunk has run, exactly like the
   * ordinary CS queue path. One embedded entry keeps the dominant tiny-list
   * case allocation-free.
   */
  struct D3D11CommandListReplay {
    small_vector<DxvkCsChunkRef, 1u> chunks;

    void operator () (DxvkContext* ctx);
  };

  class D3D11CommandList : public D3D11DeviceChild<ID3D11CommandList> {
    
  public:
    
    D3D11CommandList(
            D3D11Device*  pDevice,
            UINT          ContextFlags,
            D3D11DeferredContext* pOrigin);
    
    ~D3D11CommandList();
    
    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID  riid,
            void**  ppvObject) final;
    
    UINT STDMETHODCALLTYPE GetContextFlags() final;
    
    void AddQuery(
            D3D11Query*         pQuery);
    
    uint64_t AddChunk(
            DxvkCsChunkRef&&    Chunk,
            uint64_t            Cost);

    uint64_t AddCommandList(
            D3D11CommandList*   pCommandList);

    void EmitToCsThread(
      const D3D11ChunkDispatchProc& DispatchProc);

    /**
     * \brief Prepares the one-chunk list for inline immediate-context replay
     *
     * Marks deferred query ends at ExecuteCommandList time, as the regular
     * CS-thread path does. Callers must have checked CanReplayInline().
     */
    D3D11CommandListReplay CreateReplay();

    bool CanReplayInline() const {
      return m_chunks.size() == 1u;
    }

    uint32_t GetChunkCount() const {
      return uint32_t(m_chunks.size());
    }

    uint64_t GetReplayCost() const {
      return m_chunks[0].cost;
    }

    /**
     * \brief Exact recorded byte size of an inline-replayable list
     *
     * Callers must have checked \ref CanReplayInline. The returned size is
     * the deferred chunk's actual occupied bytes, not its 16 KiB capacity.
     */
    uint64_t GetReplayByteSize() const {
      return uint64_t(m_chunks[0].chunk->usedBytes());
    }

    GpuFlushType GetReplayFlushType() const {
      return m_resources.empty()
        ? GpuFlushType::ImplicitWeakHint
        : GpuFlushType::ImplicitStrongHint;
    }

    void TrackResourceSequenceNumbers(uint64_t Seq);

    void TrackResourceUsage(
            ID3D11Resource*     pResource,
            D3D11_RESOURCE_DIMENSION ResourceType,
            UINT                Subresource,
            uint64_t            ChunkId);

    /**
     * \brief Whether replaying this list leaves the DXVK context clean
     *
     * Helios: stock lists always end with a recorded ResetCommandListState
     * sweep. Fast-path lists retain their final binding footprint separately;
     * the executing context carries it until a later execute boundary or
     * ordinary CS command consumes it.
     */
    bool EndsClean() const {
      return m_heliosEndsClean;
    }

    void SetEndsClean(bool EndsClean) {
      m_heliosEndsClean = EndsClean;
    }

    const D3D11MaxUsedBindings& GetUsedBindings() const {
      return m_heliosUsedBindings;
    }

    void SetUsedBindings(const D3D11MaxUsedBindings& UsedBindings) {
      m_heliosUsedBindings = UsedBindings;
    }

    /**
     * \brief Clears a DDI-recycled command list for its originating context
     *
     * Drops all chunk, query, and resource references plus the captured
     * Helios finish metadata, but deliberately does not shrink the vectors.
     * Thus the DDI recycle path reuses only bookkeeping capacity: CS chunks
     * are released to their normal owners and no staging storage is retained.
     * Private data and the destruction notifier remain object lifetime state
     * and are not reset here.
     */
    void ResetForReuse();

    /// The DDI cache must only return a list to the deferred context that
    /// originally recorded it. This compares opaque addresses only; it never
    /// dereferences an origin that may already be in its destroy path.
    bool IsReusableBy(const D3D11DeferredContext* pContext) const {
      return m_heliosOrigin == pContext;
    }

    /**
     * \brief Whether this list carries no work at all
     *
     * Only possible on the CL fast path (a stock list always contains its
     * trailing reset sweep). Executing an empty list is a no-op apart from
     * the API-level context-state reset.
     */
    bool IsEmpty() const {
      return m_chunks.empty() && m_queries.empty() && m_resources.empty();
    }

  private:

    struct ChunkEntry {
      ChunkEntry() = default;
      ChunkEntry(DxvkCsChunkRef&& c, uint64_t v)
      : chunk(std::move(c)), cost(v) { }
      DxvkCsChunkRef chunk = { };
      uint64_t cost = 0u;
    };

    struct TrackedResource {
      D3D11ResourceRef  ref;
      uint64_t          chunkId;
    };

    UINT m_contextFlags = 0u;

    // Set once at construction. The UMD's RecycleCommandList callback carries
    // its originating DC; this makes a malformed cross-DC handoff rejectable
    // without a global pool or a context lifetime dependency.
    D3D11DeferredContext* m_heliosOrigin = nullptr;

    bool m_heliosEndsClean = true;

    // The fast path omits the recorded trailing reset. Capture exactly the
    // deferred context's final physical-tail footprint while it is still
    // live, since ExecuteCommandList(FALSE) clears the CPU shadow before a
    // later boundary must consume it.
    D3D11MaxUsedBindings m_heliosUsedBindings = { };

    std::vector<ChunkEntry>             m_chunks;
    std::vector<Com<D3D11Query, false>> m_queries;
    std::vector<TrackedResource>        m_resources;

    D3DDestructionNotifier              m_destructionNotifier;

    void TrackResourceSequenceNumber(
      const D3D11ResourceRef&   Resource,
            uint64_t            Seq);
    
  };
  
}
