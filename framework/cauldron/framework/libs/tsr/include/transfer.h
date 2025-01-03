#pragma once
#include <d3d12.h>
#include <tuple>
#include <array>
#include <memory>

struct TSRGraphicsResource
{
    const ID3D12Resource* resource;
    D3D12_RESOURCE_DESC   desc;
    uint64_t              stride;
    DXGI_FORMAT           format;
};

typedef std::array<const TSRGraphicsResource*, 3> FSRResources;

class TSROps
{
public:
    TSROps(const wchar_t* pSharedName, ID3D12Device* pDevice, ID3D12CommandQueue* pQueue, uint64_t bufferCount)
        : m_pSharedName(pSharedName)
        , m_pDevice(pDevice)
        , m_pQueue(pQueue)
        , m_BufferCount(bufferCount)
    {
    }
    ~TSROps();

    enum class BufferState : UINT64
    {
        IDLE = 0,
        READY,
    };

    bool bufferStateMatches(uint64_t bufferIndex, BufferState state);

    bool bufferStateMatchesAll(BufferState state);

    void CreateSharedBuffers(FSRResources pResources, bool shouldCreate = false);

    void TransferToSharedBuffer(FSRResources pResources, uint64_t bufferIndex, ID3D12GraphicsCommandList2* pCmdList)
    {
        PerformTransfer(pResources, bufferIndex, pCmdList, true);
    }

    void TransferFromSharedBuffer(FSRResources pResources, uint64_t bufferIndex, ID3D12GraphicsCommandList2* pCmdList)
    {
        PerformTransfer(pResources, bufferIndex, pCmdList, false);
    }

private:
    const wchar_t*      m_pSharedName = nullptr;
    ID3D12Device*       m_pDevice     = nullptr;
    ID3D12CommandQueue* m_pQueue      = nullptr;
    uint64_t            m_BufferCount = 0;

    std::tuple<ID3D12Resource*, ID3D12Fence*> p_SharedBuffer[TSR_SHARED_BUFFER_MAX];

    size_t CalculateTotalSize(FSRResources pResources);

    void PerformTransfer(FSRResources pResources, uint64_t bufferIndex, ID3D12GraphicsCommandList2* pCmdList, bool toSharedBuffer);
};
