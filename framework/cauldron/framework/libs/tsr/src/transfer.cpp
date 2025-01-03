#include "transfer.h"
#include "assert.h"
#include <string>

TSROps::~TSROps()
{
    for (size_t i = 0; i < m_BufferCount; i++)
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

bool TSROps::bufferStateMatches(uint64_t bufferIndex, BufferState state)
{
    AssertCritical(bufferIndex < m_BufferCount, L"Invalid buffer index");
    ID3D12Fence* pFence = std::get<1>(p_SharedBuffer[bufferIndex]);
    return pFence->GetCompletedValue() == static_cast<UINT64>(state);
}

bool TSROps::bufferStateMatchesAll(BufferState state)
{
    for (size_t i = 0; i < m_BufferCount; i++)
    {
        if (!bufferStateMatches(i, state))
            return false;
    }
    return true;
}

size_t TSROps::CalculateTotalSize(FSRResources pResources)
{
    size_t totalSize = 0;

    for (size_t i = 0; i < pResources.size(); i++)
    {
        const TSRGraphicsResource* pResource = pResources[i];
        D3D12_RESOURCE_DESC        desc      = pResource->desc;
        uint64_t                   stride    = pResource->stride;

        totalSize += desc.Width * desc.Height * stride;
    }

    return totalSize;
}

void TSROps::CreateSharedBuffers(FSRResources pResources, bool shouldCreate)
{
    for (size_t i = 0; i < m_BufferCount; i++)
    {
        ID3D12Resource* pResource    = nullptr;
        ID3D12Fence*    pFence       = nullptr;
        std::wstring    resourceName = m_pSharedName + std::to_wstring(i) + L"_RESOURCE";
        std::wstring    fenceName    = m_pSharedName + std::to_wstring(i) + L"_FENCE";

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
            AssertCritical(
                m_pDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_SHARED, &bufferDesc, state, nullptr, IID_PPV_ARGS(&pResource)) == S_OK,
                L"Failed to create shared buffer");

            // Create a fence
            AssertCritical(m_pDevice->CreateFence(static_cast<UINT64>(BufferState::IDLE), D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&pFence)) == S_OK,
                           L"Failed to create shared fence");

            // Create the shared handles
            HANDLE handle = {};
            AssertCritical(m_pDevice->CreateSharedHandle(pResource, nullptr, GENERIC_ALL, resourceName.c_str(), &handle) == S_OK, L"Failed to create shared handle for resource");

            handle = {};
            AssertCritical(m_pDevice->CreateSharedHandle(pFence, nullptr, GENERIC_ALL, fenceName.c_str(), &handle) == S_OK, L"Failed to create shared handle for fence");
        }
        else
        {
            // Open the shared handles
            HANDLE handle = {};
            AssertCritical(m_pDevice->OpenSharedHandleByName(resourceName.c_str(), GENERIC_ALL, &handle) == S_OK, L"Failed to open shared handle by name for resource");
            AssertCritical(m_pDevice->OpenSharedHandle(handle, IID_PPV_ARGS(&pResource)) == S_OK, L"Failed to open shared handle for resource");

            handle = {};
            AssertCritical(m_pDevice->OpenSharedHandleByName(fenceName.c_str(), GENERIC_ALL, &handle) == S_OK, L"Failed to open shared handle by name for fence");
            AssertCritical(m_pDevice->OpenSharedHandle(handle, IID_PPV_ARGS(&pFence)) == S_OK, L"Failed to open shared handle for fence");
        }

        p_SharedBuffer[i] = std::make_tuple(pResource, pFence);
    }
}

void TSROps::PerformTransfer(FSRResources pResources, uint64_t bufferIndex, ID3D12GraphicsCommandList2* pCmdList, bool toSharedBuffer)
{
    AssertCritical(bufferIndex < m_BufferCount, L"Invalid buffer index");

    // Get the device and command list
    ID3D12Resource* pSharedResource = std::get<0>(p_SharedBuffer[bufferIndex]);
    ID3D12Fence*    pSharedFence    = std::get<1>(p_SharedBuffer[bufferIndex]);

    // Verify the shared buffer is in the correct state
    UINT64 desiredState = static_cast<UINT64>(toSharedBuffer ? BufferState::IDLE : BufferState::READY);
    AssertCritical(pSharedFence->GetCompletedValue() == desiredState, L"The shared buffer is not in the correct state");

    // Keep track of the current offset in the staging resource
    size_t offset = 0;

    for (size_t i = 0; i < pResources.size(); i++)
    {
        TSRGraphicsResource* pResource = const_cast<TSRGraphicsResource*>(pResources[i]);
        D3D12_RESOURCE_DESC  desc      = pResource->desc;
        DXGI_FORMAT          format    = pResource->format;
        UINT64               size      = desc.Width * desc.Height * pResource->stride;

        // Transition the resource to appropriate state
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = const_cast<ID3D12Resource*>(pResource->resource);
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
            barrier.Transition.StateAfter  = toSharedBuffer ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST;

            pCmdList->ResourceBarrier(1, &barrier);
        }

        // Perform the transfer command
        {
            D3D12_TEXTURE_COPY_LOCATION actualResource{};
            actualResource.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            actualResource.pResource        = const_cast<ID3D12Resource*>(pResource->resource);
            actualResource.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION sharedResource{};
            sharedResource.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            sharedResource.pResource                          = pSharedResource;
            sharedResource.PlacedFootprint.Footprint.Depth    = 1;
            sharedResource.PlacedFootprint.Footprint.Height   = desc.Height;
            sharedResource.PlacedFootprint.Footprint.Width    = static_cast<UINT>(desc.Width);
            sharedResource.PlacedFootprint.Footprint.Format   = format;
            sharedResource.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(desc.Width * pResource->stride);
            sharedResource.PlacedFootprint.Offset             = offset;

            if (toSharedBuffer)
            {
                pCmdList->CopyTextureRegion(&sharedResource, 0, 0, 0, &actualResource, nullptr);
            }
            else
            {
                pCmdList->CopyTextureRegion(&actualResource, 0, 0, 0, &sharedResource, nullptr);
            }
        }

        // Transition the resource back to its original state
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = const_cast<ID3D12Resource*>(pResource->resource);
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = toSharedBuffer ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

            pCmdList->ResourceBarrier(1, &barrier);
        }

        // Update the offset
        offset += size;
    }

    // Signal the fence to indicate the transfer is complete
    AssertCritical(m_pQueue->Signal(pSharedFence, static_cast<UINT64>(toSharedBuffer ? BufferState::READY : BufferState::IDLE)) == S_OK,
                   L"Failed to signal the fence");
    return;
}
