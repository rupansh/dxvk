#pragma once

#include "d3d11_cmdlist.h"
#include "d3d11_context.h"

#include <vector>

namespace dxvk {
  
  struct D3D11DeferredContextMapEntry {
    uint64_t                  ResourceCookie = 0u;
    D3D11_MAPPED_SUBRESOURCE  MapInfo = { };
  };
  
  class D3D11DeferredContext : public D3D11CommonContext<D3D11DeferredContext> {
    friend class D3D11CommonContext<D3D11DeferredContext>;
  public:
    
    D3D11DeferredContext(
            D3D11Device*    pParent,
      const Rc<DxvkDevice>& Device,
            UINT            ContextFlags);

    ~D3D11DeferredContext();
    
    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID                      riid,
            void**                      ppvObject);

    HRESULT STDMETHODCALLTYPE GetData(
            ID3D11Asynchronous*         pAsync,
            void*                       pData,
            UINT                        DataSize,
            UINT                        GetDataFlags);
    
    void STDMETHODCALLTYPE Begin(
            ID3D11Asynchronous*         pAsync);

    void STDMETHODCALLTYPE End(
            ID3D11Asynchronous*         pAsync);

    void STDMETHODCALLTYPE Flush();

    void STDMETHODCALLTYPE Flush1(
            D3D11_CONTEXT_TYPE          ContextType,
            HANDLE                      hEvent);

    HRESULT STDMETHODCALLTYPE Signal(
            ID3D11Fence*                pFence,
            UINT64                      Value);
    
    HRESULT STDMETHODCALLTYPE Wait(
            ID3D11Fence*                pFence,
            UINT64                      Value);

    void STDMETHODCALLTYPE ExecuteCommandList(
            ID3D11CommandList*          pCommandList,
            BOOL                        RestoreContextState);
    
    HRESULT STDMETHODCALLTYPE FinishCommandList(
            BOOL                        RestoreDeferredContextState,
            ID3D11CommandList**         ppCommandList);
    
    HRESULT STDMETHODCALLTYPE Map(
            ID3D11Resource*             pResource,
            UINT                        Subresource,
            D3D11_MAP                   MapType,
            UINT                        MapFlags,
            D3D11_MAPPED_SUBRESOURCE*   pMappedResource);
    
    void STDMETHODCALLTYPE Unmap(
            ID3D11Resource*             pResource,
            UINT                        Subresource);
    
    void STDMETHODCALLTYPE SwapDeviceContextState(
           ID3DDeviceContextState*           pState,
           ID3DDeviceContextState**          ppPreviousState);

    D3D10DeviceLock LockContext() {
      return D3D10DeviceLock();
    }

    /**
     * \brief Receives a BUILD_2-retired list from the Helios UMD
     *
     * Called only from the runtime's serialized DC::RecycleCommandList
     * callback, never from free-threaded IC::RecycleDestroyCommandList. The
     * caller transfers one COM reference only on a true result; false leaves
     * that reference with the caller for ordinary destruction.
     */
    bool RecycleCommandList(D3D11CommandList* pCommandList);

    // This context is owned by the Helios DDI frontend rather than exposed
    // through DXVK's public device interface. Enable this before it records.
    void EnableHeliosDdiLogicalReset() {
      m_heliosDdiLogicalResetEnabled = true;
    }

  private:
    // BUILD_2 recycle candidates belong to this exact deferred context. They
    // are not a device-global release pool: only its later CreateCommandList
    // may consume them, and actual DC destruction drains them. This and the
    // diagnostic counters precede m_commandList because the constructor calls
    // CreateCommandList to initialize that member.
    std::vector<Com<D3D11CommandList>> m_recycledCommandLists;

    // Diagnostics are per deferred context so the default-off timing path has
    // no shared atomic cache line. Emitted once from ~D3D11DeferredContext.
    uint64_t m_heliosRecycleHits     = 0u;
    uint64_t m_heliosRecycleMisses   = 0u;
    uint64_t m_heliosRecycleAdmitted = 0u;
    uint64_t m_heliosRecycleDropped  = 0u;

    // Command list that we're recording
    Com<D3D11CommandList> m_commandList;
    
    // Info about currently mapped (sub)resources. Using a vector
    // here is reasonable since there will usually only be a small
    // number of mapped resources per command list.
    std::vector<D3D11DeferredContextMapEntry> m_mappedResources;
    
    // Begun and ended queries, will also be stored in command list
    std::vector<Com<D3D11Query, false>> m_queriesBegun;

    // Chunk ID within the current command list
    uint64_t m_chunkId = 0ull;

    D3DDestructionNotifier m_destructionNotifier;

    HRESULT MapBuffer(
            ID3D11Resource*               pResource,
            D3D11_MAPPED_SUBRESOURCE*     pMappedResource);
    
    HRESULT MapImage(
            ID3D11Resource*               pResource,
            UINT                          Subresource,
            D3D11_MAPPED_SUBRESOURCE*     pMappedResource);

    void UpdateMappedBuffer(
            D3D11Buffer*                  pDstBuffer,
            UINT                          Offset,
            UINT                          Length,
      const void*                         pSrcData,
            UINT                          CopyFlags);

    void FinalizeQueries();

    Com<D3D11CommandList> CreateCommandList();
    
    void EmitCsChunk(DxvkCsChunkRef&& chunk);

    uint64_t GetCurrentChunkId() const;

    void TrackTextureSequenceNumber(
            D3D11CommonTexture*           pResource,
            UINT                          Subresource);

    void TrackBufferSequenceNumber(
            D3D11Buffer*                  pResource);

    D3D11_MAPPED_SUBRESOURCE FindMapEntry(
            uint64_t                      Coookie);

    void AddMapEntry(
            uint64_t                      Cookie,
      const D3D11_MAPPED_SUBRESOURCE&     MapInfo);

    static DxvkCsChunkFlags GetCsChunkFlags(
            D3D11Device*                  pDevice);
    
  };
  
}
