#pragma once

#include "validation_remap.h"
#include "../common.h"

using namespace cauldron;
using namespace common;

class DX12Ops
{
public:
    DX12Ops() = default;

    ~DX12Ops()
    {
        for (size_t i = 0; i < GetFramework()->GetBufferCount(); i++)
        {
            ID3D12Resource* pResource = std::get<0>(p_SharedBuffer[i]);
            ID3D12Fence*    pFence    = std::get<1>(p_SharedBuffer[i]);

            if (pResource)
            {
                pResource->Release();
            }

            if (pFence)
            {
                pFence->Release();
            }
        }
    }

    enum class BufferState : UINT64
    {
        IDLE = 0,
        READY,
    };

    bool bufferStateMatches(uint64_t bufferIndex, BufferState state)
    {
        CauldronAssert(ASSERT_CRITICAL, bufferIndex < GetFramework()->GetBufferCount(), L"Invalid buffer index");
        ID3D12Fence* pFence = std::get<1>(p_SharedBuffer[bufferIndex]);
        return pFence->GetCompletedValue() == static_cast<UINT64>(state);
    }

    void CreateSharedBuffers(FSRResources pResources, bool shouldCreate = false);

    void TransferToSharedBuffer(FSRResources pResources, uint64_t bufferIndex, CommandList* pCmdList)
    {
        PerformTransfer(pResources, bufferIndex, pCmdList, true);
    }

    void TransferFromSharedBuffer(FSRResources pResources, uint64_t bufferIndex, CommandList* pCmdList)
    {
        PerformTransfer(pResources, bufferIndex, pCmdList, false);
    }

private:
    std::tuple<ID3D12Resource*, ID3D12Fence*> p_SharedBuffer[FSR_REMOTE_SHARED_BUFFER_MAX];

    size_t CalculateTotalSize(FSRResources pResources);

    void PerformTransfer(FSRResources pResources, uint64_t bufferIndex, CommandList* pCmdList, bool toSharedBuffer);
};
