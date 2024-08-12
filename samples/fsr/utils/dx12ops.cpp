#include "dx12ops.h"

size_t DX12Ops::CalculateTotalSize(FSRResources pResources)
{
    size_t totalSize = 0;

    for (size_t i = 0; i < pResources.size(); i++)
    {
        const GPUResource*  pResource = pResources[i];
        D3D12_RESOURCE_DESC desc      = pResource->GetImpl()->DX12Desc();
        ResourceFormat      format    = pResource->GetTextureResource()->GetFormat();

        totalSize += desc.Width * desc.Height * GetResourceFormatStride(format);
    }

    return totalSize;
}

void DX12Ops::CreateSharedBuffers(FSRResources pResources, bool shouldCreate)
{
    ID3D12Device* pDevice = GetDevice()->GetImpl()->DX12Device();

    for (size_t i = 0; i < GetFramework()->GetBufferCount(); i++)
    {
        ID3D12Resource* pResource    = nullptr;
        ID3D12Fence*    pFence       = nullptr;
        std::wstring    resourceName = GetFramework()->GetName() + std::to_wstring(i) + L"_RESOURCE";
        std::wstring    fenceName    = GetFramework()->GetName() + std::to_wstring(i) + L"_FENCE";

        if (shouldCreate)
        {
            // Create a shared buffer
            D3D12_RESOURCE_DESC bufferDesc = {};
            bufferDesc.Alignment           = 0;
            bufferDesc.DepthOrArraySize    = 1;
            bufferDesc.Dimension           = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDesc.Flags               = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;  // D3D12_RESOURCE_FLAG_NONE
            bufferDesc.Format              = DXGI_FORMAT_UNKNOWN;
            bufferDesc.Height              = 1;
            bufferDesc.Width               = CalculateTotalSize(pResources);
            bufferDesc.Layout              = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            bufferDesc.MipLevels           = 1;
            bufferDesc.SampleDesc.Count    = 1;
            bufferDesc.SampleDesc.Quality  = 0;

            // Heap properties
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type                  = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
            CauldronThrowOnFail(pDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_SHARED, &bufferDesc, state, nullptr, IID_PPV_ARGS(&pResource)));

            // Create a fence
            CauldronThrowOnFail(pDevice->CreateFence(static_cast<UINT64>(BufferState::IDLE), D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&pFence)));

            // Create the shared handles
            HANDLE handle = {};
            CauldronThrowOnFail(pDevice->CreateSharedHandle(pResource, nullptr, GENERIC_ALL, resourceName.c_str(), &handle));

            handle = {};
            CauldronThrowOnFail(pDevice->CreateSharedHandle(pFence, nullptr, GENERIC_ALL, fenceName.c_str(), &handle));
        }
        else
        {
            // Open the shared handles
            HANDLE handle = {};
            CauldronThrowOnFail(pDevice->OpenSharedHandleByName(resourceName.c_str(), GENERIC_ALL, &handle));
            CauldronThrowOnFail(pDevice->OpenSharedHandle(handle, IID_PPV_ARGS(&pResource)));

            handle = {};
            CauldronThrowOnFail(pDevice->OpenSharedHandleByName(fenceName.c_str(), GENERIC_ALL, &handle));
            CauldronThrowOnFail(pDevice->OpenSharedHandle(handle, IID_PPV_ARGS(&pFence)));
        }

        p_SharedBuffer[i] = std::make_tuple(pResource, pFence);
    }
}

void DX12Ops::PerformTransfer(FSRResources pResources, uint64_t bufferIndex, CommandList* pCmdList, bool toSharedBuffer)
{
    CauldronAssert(ASSERT_CRITICAL, bufferIndex < GetFramework()->GetBufferCount(), L"Invalid buffer index");

    // Get the device and command list
    ID3D12Resource*             pSharedResource = std::get<0>(p_SharedBuffer[bufferIndex]);
    ID3D12Fence*                pSharedFence    = std::get<1>(p_SharedBuffer[bufferIndex]);
    ID3D12Device*               pDevice         = GetDevice()->GetImpl()->DX12Device();
    ID3D12GraphicsCommandList2* pCmd            = pCmdList->GetImpl()->DX12CmdList();
    ID3D12CommandQueue*         pQueue          = GetDevice()->GetImpl()->DX12CmdQueue(CommandQueue::Graphics);

    // Verify the shared buffer is in the correct state
    UINT64 desiredState = static_cast<UINT64>(toSharedBuffer ? BufferState::IDLE : BufferState::READY);
    CauldronAssert(ASSERT_CRITICAL, pSharedFence->GetCompletedValue() == desiredState, L"The shared buffer is not in the correct state");

    // Keep track of the current offset in the staging resource
    size_t offset = 0;

    for (size_t i = 0; i < pResources.size(); i++)
    {
        GPUResource*        pResource = const_cast<GPUResource*>(pResources[i]);
        D3D12_RESOURCE_DESC desc      = pResource->GetImpl()->DX12Desc();
        ResourceFormat      format    = pResource->GetTextureResource()->GetFormat();
        UINT64              size      = desc.Width * desc.Height * GetResourceFormatStride(format);

        // Transition the resource to appropriate state
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = pResource->GetImpl()->DX12Resource();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
            barrier.Transition.StateAfter  = toSharedBuffer ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST;

            pCmd->ResourceBarrier(1, &barrier);
        }

        // Perform the transfer command
        {
            D3D12_TEXTURE_COPY_LOCATION actualResource{};
            actualResource.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            actualResource.pResource        = pResource->GetImpl()->DX12Resource();
            actualResource.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION sharedResource{};
            sharedResource.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            sharedResource.pResource                          = pSharedResource;
            sharedResource.PlacedFootprint.Footprint.Depth    = 1;
            sharedResource.PlacedFootprint.Footprint.Height   = desc.Height;
            sharedResource.PlacedFootprint.Footprint.Width    = static_cast<UINT>(desc.Width);
            sharedResource.PlacedFootprint.Footprint.Format   = GetDXGIFormat(format);
            sharedResource.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(desc.Width * GetResourceFormatStride(format));
            sharedResource.PlacedFootprint.Offset             = offset;

            if (toSharedBuffer)
            {
                pCmd->CopyTextureRegion(&sharedResource, 0, 0, 0, &actualResource, nullptr);
            }
            else
            {
                pCmd->CopyTextureRegion(&actualResource, 0, 0, 0, &sharedResource, nullptr);
            }
        }

        // Transition the resource back to its original state
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = pResource->GetImpl()->DX12Resource();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = toSharedBuffer ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

            pCmd->ResourceBarrier(1, &barrier);
        }

        // Update the offset
        offset += size;
    }

    // Signal the fence to indicate the transfer is complete
    CauldronThrowOnFail(pQueue->Signal(pSharedFence, static_cast<UINT64>(toSharedBuffer ? BufferState::READY : BufferState::IDLE)));
    return;
}
