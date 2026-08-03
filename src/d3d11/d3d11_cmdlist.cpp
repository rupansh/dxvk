#include "d3d11_cmdlist.h"
#include "d3d11_device.h"
#include "d3d11_buffer.h"
#include "d3d11_texture.h"

namespace dxvk {
    
  D3D11CommandList::D3D11CommandList(
          D3D11Device*  pDevice,
          UINT          ContextFlags,
          D3D11DeferredContext* pOrigin)
  : D3D11DeviceChild<ID3D11CommandList>(pDevice),
    m_contextFlags(ContextFlags),
    m_heliosOrigin(pOrigin),
    m_destructionNotifier(this) { }
  
  
  D3D11CommandList::~D3D11CommandList() {
    
  }
  
  
  HRESULT STDMETHODCALLTYPE D3D11CommandList::QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;
    
    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(ID3D11DeviceChild)
     || riid == __uuidof(ID3D11CommandList)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(ID3DDestructionNotifier)) {
      *ppvObject = ref(&m_destructionNotifier);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D11CommandList), riid)) {
      Logger::warn("D3D11CommandList::QueryInterface: Unknown interface query");
      Logger::warn(str::format(riid));
    }

    return E_NOINTERFACE;
  }
  
  
  UINT STDMETHODCALLTYPE D3D11CommandList::GetContextFlags() {
    return m_contextFlags;
  }


  void D3D11CommandList::ResetForReuse() {
    // clear(), rather than swap/shrink_to_fit(), releases every payload
    // reference while retaining the vector allocations for the next list.
    // In particular this never pins a DxvkCsChunk or staging allocation in
    // the recycle cache.
    m_chunks.clear();
    m_queries.clear();
    m_resources.clear();

    m_heliosEndsClean = true;
    m_heliosUsedBindings = { };

    // Context flags and the origin pointer are immutable identity. The cache
    // never crosses that origin, so neither is recording payload to reset.
  }
  
  
  void D3D11CommandList::AddQuery(D3D11Query* pQuery) {
    m_queries.emplace_back(pQuery);
  }


  uint64_t D3D11CommandList::AddChunk(DxvkCsChunkRef&& Chunk, uint64_t Cost) {
    m_chunks.emplace_back(std::move(Chunk), Cost);
    return m_chunks.size() - 1;
  }
  
  
  uint64_t D3D11CommandList::AddCommandList(
          D3D11CommandList*   pCommandList) {
    // Helios: callers skip empty fast-path lists, but keep the tail
    // arithmetic below well-defined if one slips through anyway.
    if (unlikely(pCommandList->m_chunks.empty() && m_chunks.empty()))
      return 0u;

    // This will be the chunk ID of the first chunk
    // added, for the purpose of resource tracking.
    uint64_t baseChunkId = m_chunks.size();
    
    for (const auto& chunk : pCommandList->m_chunks)
      m_chunks.push_back(chunk);

    for (const auto& query : pCommandList->m_queries)
      m_queries.push_back(query);

    for (const auto& resource : pCommandList->m_resources) {
      TrackedResource entry = resource;
      entry.chunkId += baseChunkId;

      m_resources.push_back(std::move(entry));
    }

    // Return ID of the last chunk added. The command list
    // added can never be empty, so do not handle zero.
    return m_chunks.size() - 1;
  }


  void D3D11CommandList::EmitToCsThread(
    const D3D11ChunkDispatchProc& DispatchProc) {
    for (const auto& query : m_queries)
      query->DoDeferredEnd();

    for (size_t i = 0, j = 0; i < m_chunks.size(); i++) {
      // If there are resources to track for the current chunk,
      // use a strong flush hint to dispatch GPU work quickly.
      GpuFlushType flushType = GpuFlushType::ImplicitWeakHint;

      if (j < m_resources.size() && m_resources[j].chunkId == i)
        flushType = GpuFlushType::ImplicitStrongHint;

      // Dispatch the chunk and capture its sequence number
      uint64_t seq = DispatchProc(DxvkCsChunkRef(m_chunks[i].chunk), m_chunks[i].cost, flushType);

      // Track resource sequence numbers for the added chunk
      while (j < m_resources.size() && m_resources[j].chunkId == i)
        TrackResourceSequenceNumber(m_resources[j++].ref, seq);
    }
  }


  D3D11CommandListReplay D3D11CommandList::CreateReplay() {
    D3D11CommandListReplay result;
    result.chunks.reserve(m_chunks.size());

    for (const auto& query : m_queries)
      query->DoDeferredEnd();

    for (const auto& chunk : m_chunks)
      result.chunks.emplace_back(chunk.chunk);

    return result;
  }


  void D3D11CommandList::TrackResourceSequenceNumbers(uint64_t Seq) {
    for (const auto& resource : m_resources)
      TrackResourceSequenceNumber(resource.ref, Seq);
  }


  void D3D11CommandListReplay::operator () (DxvkContext* ctx) {
    for (auto& chunk : chunks) {
      chunk->executeAll(ctx);
      chunk = DxvkCsChunkRef();
    }
  }
  
  
  void D3D11CommandList::TrackResourceUsage(
          ID3D11Resource*     pResource,
          D3D11_RESOURCE_DIMENSION ResourceType,
          UINT                Subresource,
          uint64_t            ChunkId) {
    TrackedResource entry;
    entry.ref = D3D11ResourceRef(pResource, Subresource, ResourceType);
    entry.chunkId = ChunkId;

    m_resources.push_back(std::move(entry));
  }


  void D3D11CommandList::TrackResourceSequenceNumber(
    const D3D11ResourceRef&   Resource,
          uint64_t            Seq) {
    ID3D11Resource* iface = Resource.Get();

    switch (Resource.GetType()) {
      case D3D11_RESOURCE_DIMENSION_UNKNOWN:
        break;

      case D3D11_RESOURCE_DIMENSION_BUFFER: {
        auto impl = static_cast<D3D11Buffer*>(iface);
        impl->TrackSequenceNumber(Seq);
      } break;

      case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
        auto impl = static_cast<D3D11Texture1D*>(iface)->GetCommonTexture();
        impl->TrackSequenceNumber(Resource.GetSubresource(), Seq);
      } break;

      case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
        auto impl = static_cast<D3D11Texture2D*>(iface)->GetCommonTexture();
        impl->TrackSequenceNumber(Resource.GetSubresource(), Seq);
      } break;

      case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
        auto impl = static_cast<D3D11Texture3D*>(iface)->GetCommonTexture();
        impl->TrackSequenceNumber(Resource.GetSubresource(), Seq);
      } break;
    }
  }
  
}
