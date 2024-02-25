#include "dx12ops.h"
#include <chrono>

DX12Ops::~DX12Ops()
{
    if (p_StagingResource)
    {
        p_StagingResource->Release();
        p_StagingResource = nullptr;
    }
}

size_t DX12Ops::CalculateTotalSize(const GPUResource** pResources, size_t numResources)
{
    size_t totalSize = 0;

    for (size_t i = 0; i < numResources; i++)
    {
        const GPUResource* pResource = pResources[i];
        D3D12_RESOURCE_DESC desc = pResource->GetImpl()->DX12Desc();
        ResourceFormat format = pResource->GetTextureResource()->GetFormat();

        totalSize += desc.Width * desc.Height * GetResourceFormatStride(format);
    }

    return totalSize;
}

void DX12Ops::CreateStagingResource(WriteSource source, size_t size)
{
    // Release the old resource, if any
    if (p_StagingResource)
    {
        // Check if the requested size is the same as the current size
        D3D12_RESOURCE_DESC desc = p_StagingResource->GetDesc();
        if (desc.Width == size)
            return;

        p_StagingResource->Release();
        p_StagingResource = nullptr;
    }

    ID3D12Device* pDevice = GetDevice()->GetImpl()->DX12Device();

    // Create a staging resource
    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Alignment           = 0;
    bufferDesc.DepthOrArraySize    = 1;
    bufferDesc.Dimension           = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Flags               = D3D12_RESOURCE_FLAG_NONE;
    bufferDesc.Format              = DXGI_FORMAT_UNKNOWN;
    bufferDesc.Height              = 1;
    bufferDesc.Width               = size;
    bufferDesc.Layout              = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufferDesc.MipLevels           = 1;
    bufferDesc.SampleDesc.Count    = 1;
    bufferDesc.SampleDesc.Quality  = 0;

    // Heap properties
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type                  = source == WriteSource::GPU ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_STATES state = source == WriteSource::GPU ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ;
    HRESULT hr = pDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, state, nullptr, IID_PPV_ARGS(&p_StagingResource));
    CauldronThrowOnFail(hr);

    // Create the corresponding CPU data location
    p_StagingData = (uint8_t*) malloc(size * sizeof(uint8_t));
}

void DX12Ops::TransferResourcesToCPU(const GPUResource** pResources, size_t numResources)
{
    // Create a staging resource
    size_t totalSize = CalculateTotalSize(pResources, numResources);
    CreateStagingResource(WriteSource::GPU, totalSize);

    // Create a command list to be used for the copy operation
    ID3D12Device* pDevice  = GetDevice()->GetImpl()->DX12Device();
    CommandList*  pCmdList = GetDevice()->CreateCommandList(L"ResourceToCPU", CommandQueue::Graphics);

    // Keep track of the current offset in the staging resource
    size_t offset = 0;

    for (size_t i = 0; i < numResources; i++)
    {
        GPUResource*        pResource = const_cast<GPUResource*>(pResources[i]);
        D3D12_RESOURCE_DESC desc      = pResource->GetImpl()->DX12Desc();
        ResourceFormat      format    = pResource->GetTextureResource()->GetFormat();
        UINT64              size      = desc.Width * desc.Height * GetResourceFormatStride(format);

        // Transition the resource to copy source
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = pResource->GetImpl()->DX12Resource();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;

            pCmdList->GetImpl()->DX12CmdList()->ResourceBarrier(1, &barrier);
        }

        // Copy the data from the GPU resource to the staging resource
        {
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.pResource        = pResource->GetImpl()->DX12Resource();
            source.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.pResource                          = p_StagingResource;
            destination.PlacedFootprint.Footprint.Depth    = 1;
            destination.PlacedFootprint.Footprint.Height   = desc.Height;
            destination.PlacedFootprint.Footprint.Width    = static_cast<UINT>(desc.Width);
            destination.PlacedFootprint.Footprint.Format   = GetDXGIFormat(format);
            destination.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(desc.Width * GetResourceFormatStride(format));
            destination.PlacedFootprint.Offset             = offset;

            pCmdList->GetImpl()->DX12CmdList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }

        // Transition the resource back to its original state
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = pResource->GetImpl()->DX12Resource();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

            pCmdList->GetImpl()->DX12CmdList()->ResourceBarrier(1, &barrier);
        }

        // Update the offset
        offset += size;
    }

    // Close the command list
    ID3D12Fence* pFence;
    CauldronThrowOnFail(pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pFence)));
    CauldronThrowOnFail(GetDevice()->GetImpl()->DX12CmdQueue(CommandQueue::Graphics)->Signal(pFence, 1));
    CauldronThrowOnFail(pCmdList->GetImpl()->DX12CmdList()->Close());

    ID3D12CommandList* CmdListList[] = {pCmdList->GetImpl()->DX12CmdList()};
    GetDevice()->GetImpl()->DX12CmdQueue(CommandQueue::Graphics)->ExecuteCommandLists(1, CmdListList);

    // Wait for the command list to finish executing
    HANDLE mHandleFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    pFence->SetEventOnCompletion(1, mHandleFenceEvent);
    WaitForSingleObject(mHandleFenceEvent, INFINITE);
    CloseHandle(mHandleFenceEvent);
    pFence->Release();

    uint8_t*    pData     = NULL;
    D3D12_RANGE range{0, totalSize};
    HRESULT hr  = p_StagingResource->Map(0, &range, reinterpret_cast<void**>(&pData));

    // Copy the data to the destination pointer
    if (SUCCEEDED(hr))
    {
        // Copy memory to destination
        memcpy(p_StagingData, pData, totalSize);
        p_StagingResource->Unmap(0, NULL);
    }
    else
    {
        // Handle error
        p_StagingResource->Release();
        CauldronThrowOnFail(hr);
        return;
    }

    // Delete the command list
    delete pCmdList;
}

void DX12Ops::TransferResourcesToGPU(const GPUResource** pResources, size_t numResources, CommandList* pCmdList)
{
    // Create a staging resource
    size_t totalSize = CalculateTotalSize(pResources, numResources);
    CreateStagingResource(WriteSource::CPU, totalSize);

    // Map the staging resource
    uint8_t* pData;
    HRESULT  hr = p_StagingResource->Map(0, NULL, reinterpret_cast<void**>(&pData));

    // Copy the data to the staging resource
    if (SUCCEEDED(hr))
    {
        // Copy memory to destination
        memcpy(pData, p_StagingData, totalSize);
        p_StagingResource->Unmap(0, NULL);
    }
    else
    {
        // Handle error
        p_StagingResource->Release();
        CauldronThrowOnFail(hr);
        return;
    }

    // Keep track of the current offset in the staging resource
    size_t offset = 0;

    for (size_t i = 0; i < numResources; i++)
    {
        GPUResource*        pResource = const_cast<GPUResource*>(pResources[i]);
        D3D12_RESOURCE_DESC desc      = pResource->GetImpl()->DX12Desc();
        ResourceFormat      format    = pResource->GetTextureResource()->GetFormat();
        UINT64              size      = desc.Width * desc.Height * GetResourceFormatStride(format);

        // Transition the resource to copy destination
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = pResource->GetImpl()->DX12Resource();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;

            pCmdList->GetImpl()->DX12CmdList()->ResourceBarrier(1, &barrier);
        }

        // Transition the staging resource to copy source
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = p_StagingResource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_GENERIC_READ;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;

            pCmdList->GetImpl()->DX12CmdList()->ResourceBarrier(1, &barrier);
        }

        // Copy the data from the staging resource to the GPU resource
        {
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.pResource                          = p_StagingResource;
            source.PlacedFootprint.Footprint.Depth    = 1;
            source.PlacedFootprint.Footprint.Height   = desc.Height;
            source.PlacedFootprint.Footprint.Width    = static_cast<UINT>(desc.Width);
            source.PlacedFootprint.Footprint.Format   = GetDXGIFormat(format);
            source.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(desc.Width * GetResourceFormatStride(format));
            source.PlacedFootprint.Offset             = offset;

            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.pResource        = pResource->GetImpl()->DX12Resource();
            destination.SubresourceIndex = 0;

            pCmdList->GetImpl()->DX12CmdList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }

        // Transition the staging resource back to its original state
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = p_StagingResource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_GENERIC_READ;

            pCmdList->GetImpl()->DX12CmdList()->ResourceBarrier(1, &barrier);
        }

        // Transition the resource back to its original state
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = pResource->GetImpl()->DX12Resource();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

            pCmdList->GetImpl()->DX12CmdList()->ResourceBarrier(1, &barrier);
        }

        // Update the offset
        offset += size;
    }
}
