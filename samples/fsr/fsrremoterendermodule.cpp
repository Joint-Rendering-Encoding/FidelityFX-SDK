#include "fsrremoterendermodule.h"
#include "validation_remap.h"
#include "render/device.h"
#include "render/dynamicresourcepool.h"
#include "render/profiler.h"
#include "render/swapchain.h"
#include "render/uploadheap.h"
#include "core/scene.h"

#include <functional>

using namespace cauldron;

void FSRRemoteRenderModule::Init(const json& initData)
{
    //////////////////////////////////////////////////////////////////////////
    // Resource setup

    // Fetch needed resources
    m_pColorTarget     = GetFramework()->GetColorTargetForCallback(GetName());
    m_pDepthTarget     = GetFramework()->GetRenderTexture(L"DepthTarget");
    m_pMotionVectors   = GetFramework()->GetRenderTexture(L"GBufferMotionVectorRT");
    m_pReactiveMask    = GetFramework()->GetRenderTexture(L"ReactiveMask");
    m_pCompositionMask = GetFramework()->GetRenderTexture(L"TransCompMask");

    CauldronAssert(ASSERT_CRITICAL, m_pColorTarget, L"Could not get color target for FSR render modules");
    CauldronAssert(ASSERT_CRITICAL, m_pDepthTarget, L"Could not get depth target for FSR render modules");
    CauldronAssert(ASSERT_CRITICAL, m_pMotionVectors, L"Could not get motion vectors for FSR render modules");
    CauldronAssert(ASSERT_CRITICAL, m_pReactiveMask, L"Could not get reactive mask for FSR render modules");
    CauldronAssert(ASSERT_CRITICAL, m_pCompositionMask, L"Could not get composition mask for FSR render modules");

    // Check remote mode
    m_RelayMode = initData.value("Mode", "Renderer") == "Relay";

    // Relay mode is first in line by RenderModules order, but Renderer mode needs to be put in before SwapChainRenderModule explicitly
    if (!m_RelayMode)
    {
        // Register the outbound data transfer callback
        ExecuteCallback callbackPreSwap      = std::bind(&FSRRemoteRenderModule::OutboundDataTransfer, this, std::placeholders::_1, std::placeholders::_2);
        ExecutionTuple  callbackPreSwapTuple = std::make_pair(L"FSRRemoteRenderModule::PreSwapChain", std::make_pair(this, callbackPreSwap));
        GetFramework()->RegisterExecutionCallback(L"SwapChainRenderModule", true, callbackPreSwapTuple);
    }

    // Create DX12Ops
    m_DX12Ops = DX12Ops();

    // Module is always enabled
    SetModuleEnabled(true);

    // That's all we need for now
    SetModuleReady(true);
}

FSRRemoteRenderModule::~FSRRemoteRenderModule()
{
    // Protection
    if (ModuleEnabled())
        EnableModule(false);
}

void FSRRemoteRenderModule::OnResize(const ResolutionInfo& resInfo)
{
    if (!ModuleEnabled())
        return;
}

void FSRRemoteRenderModule::Execute(double deltaTime, CommandList* pCmdList)
{
    // Execute is only meaningful in relay mode, becuase this render module is always first in line
    if (m_RelayMode)
        InboundDataTransfer(deltaTime, pCmdList);
}

void FSRRemoteRenderModule::InboundDataTransfer(double deltaTime, CommandList* pCmdList)
{
    static uint8_t* data;
    static size_t   dataSize;

    // Read the data from a file
    if (data == nullptr)
    {
        FILE* pFile = fopen("dump.bin", "rb");
        fseek(pFile, 0, SEEK_END);
        dataSize = ftell(pFile);
        fseek(pFile, 0, SEEK_SET);

        data = new uint8_t[dataSize];
        fread(data, 1, dataSize, pFile);
        fclose(pFile);
    }

    // Allocate the staging buffer if it doesn't exist
    uint8_t* pResourceData = m_DX12Ops.GetStagingData();
    if (pResourceData == nullptr)
        pResourceData = new uint8_t[dataSize];

    // Transfer the data to the staging buffer
    memcpy(pResourceData, data, dataSize);

    // Transfer the resources to the GPU
    const GPUResource* pResources[] = {m_pColorTarget->GetResource(),
                                       m_pDepthTarget->GetResource(),
                                       m_pMotionVectors->GetResource(),
                                       m_pReactiveMask->GetResource(),
                                       m_pCompositionMask->GetResource()};

    m_DX12Ops.TransferResourcesToGPU(pResources, _countof(pResources), pCmdList);

    // FidelityFX contexts modify the set resource view heaps, so set the cauldron one back
    SetAllResourceViewHeaps(pCmdList);
}

void FSRRemoteRenderModule::OutboundDataTransfer(double deltaTime, CommandList* pCmdList)
{
    const GPUResource* pResources[] = {m_pColorTarget->GetResource(),
                                       m_pDepthTarget->GetResource(),
                                       m_pMotionVectors->GetResource(),
                                       m_pReactiveMask->GetResource(),
                                       m_pCompositionMask->GetResource()};

    // Transfer the resources to the CPU
    m_DX12Ops.TransferResourcesToCPU(pResources, _countof(pResources));
    uint8_t* pResourceData = m_DX12Ops.GetStagingData();

    // Write the data to a file
    FILE* pFile = fopen("dump.bin", "wb");
    fwrite(pResourceData, 1, m_DX12Ops.CalculateTotalSize(pResources, _countof(pResources)), pFile);
    fclose(pFile);

    // FidelityFX contexts modify the set resource view heaps, so set the cauldron one back
    SetAllResourceViewHeaps(pCmdList);
}
