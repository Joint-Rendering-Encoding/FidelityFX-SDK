#pragma once

#include "validation_remap.h"

using namespace cauldron;

class DX12Ops
{
public:
    DX12Ops() = default;
    ~DX12Ops();

    // WriteSource is used to specify the source of the data to be written to the resource.
    enum class WriteSource : uint32_t
    {
        CPU = 0,
        GPU,
    };

    size_t CalculateTotalSize(const GPUResource** pResources, size_t numResources);

    void CreateStagingResource(WriteSource source, size_t size);

    void TransferResourcesToCPU(const GPUResource** pResources, size_t numResources);

    void TransferResourcesToGPU(const GPUResource** pResources, size_t numResources, CommandList* pCmdList);

    uint8_t* GetStagingData() const { return p_StagingData; }

private:
    ID3D12Resource* p_StagingResource = nullptr;
    uint8_t* p_StagingData = nullptr;
};
