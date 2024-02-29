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
        if (p_StagingResource)
        {
            p_StagingResource->Release();
            p_StagingResource = nullptr;
        }
    }

    // WriteSource is used to specify the source of the data to be written to the resource.
    enum class WriteSource : uint32_t
    {
        CPU = 0,
        GPU,
    };

    size_t CalculateTotalSize(FSRResources pResources);

    void CreateStagingResource(WriteSource source, size_t size);

    void TransferResourcesToCPU(FSRResources pResources, const FSRData* pDst);

    void TransferResourcesToGPU(FSRResources pResources, FSRData* const pSrc, CommandList* pCmdList);

private:
    ID3D12Resource* p_StagingResource = nullptr;
};
