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

    // Check remote mode
    m_UpscalerModeEnabled = initData.value("Mode", "Renderer") == "Upscaler";
    m_OnlyResizing        = initData.value("Mode", "Renderer") == "Default";

    // Fetch needed resources
    if (!m_OnlyResizing)
    {
        m_pColorTarget   = GetFramework()->GetColorTargetForCallback(GetName());
        m_pDepthTarget   = GetFramework()->GetRenderTexture(L"DepthTarget");
        m_pMotionVectors = GetFramework()->GetRenderTexture(L"GBufferMotionVectorRT");

        CauldronAssert(ASSERT_CRITICAL, m_pColorTarget, L"Could not get color target for FSR render modules");
        CauldronAssert(ASSERT_CRITICAL, m_pDepthTarget, L"Could not get depth target for FSR render modules");
        CauldronAssert(ASSERT_CRITICAL, m_pMotionVectors, L"Could not get motion vectors for FSR render modules");
    }

    // Upscaler mode is first in line by RenderModules order, but Renderer mode needs to be put in before SwapChainRenderModule explicitly
    if (!m_UpscalerModeEnabled && !m_OnlyResizing)
    {
        // Register the outbound data transfer callback
        ExecuteCallback callbackPreSwap      = std::bind(&FSRRemoteRenderModule::OutboundDataTransfer, this, std::placeholders::_1, std::placeholders::_2);
        ExecutionTuple  callbackPreSwapTuple = std::make_pair(L"FSRRemoteRenderModule::PreSwapChain", std::make_pair(this, callbackPreSwap));
        GetFramework()->RegisterExecutionCallback(L"SwapChainRenderModule", true, callbackPreSwapTuple);
    }

    if (!m_OnlyResizing)
    {
        // Create DX12Ops
        m_DX12Ops = std::make_unique<DX12Ops>();

        // Intialize the shared buffers
        m_DX12Ops->CreateSharedBuffers(getFSRResources(), !m_UpscalerModeEnabled);

        // The framework will run MainLoop based on the outcome of this function
        GetFramework()->SetReadyFunction([this]() {
            if (!m_UpscalerModeEnabled)
                return m_DX12Ops->bufferStateMatches(m_BufferIndex, DX12Ops::BufferState::IDLE);
            else
                return m_DX12Ops->bufferStateMatches(m_BufferIndex, DX12Ops::BufferState::READY);
        });
    }

    // On Renderer, enable upscaling
    m_RenderWidth  = initData.value("RenderWidth", 2560);
    m_RenderHeight = initData.value("RenderHeight", 1440);
    if (!m_UpscalerModeEnabled || m_OnlyResizing)
    {
        GetFramework()->EnableUpscaling(true, [&](uint32_t displayWidth, uint32_t displayHeight) {
            return ResolutionInfo{
                m_RenderWidth,
                m_RenderHeight,
                displayWidth,
                displayHeight,
            };
        });
    }

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
    if (!ModuleEnabled() || resInfo.RenderWidth == m_RenderWidth && resInfo.RenderHeight == m_RenderHeight)
        return;

    // Force enable upscaling with our resolution
    GetFramework()->EnableUpscaling(true, [&](uint32_t displayWidth, uint32_t displayHeight) {
        return ResolutionInfo{
            m_RenderWidth,
            m_RenderHeight,
            displayWidth,
            displayHeight,
        };
    });
}

void FSRRemoteRenderModule::Execute(double deltaTime, CommandList* pCmdList)
{
    // Skip if we are in only resizing mode
    if (m_OnlyResizing)
        return;

    // Since this render module is always first in upscaler mode, we can proceed with the data transfer
    if (m_UpscalerModeEnabled)
        return InboundDataTransfer(deltaTime, pCmdList);

    // Workaround to supress warnings from the framework
    GetFramework()->SetUpscalingState(UpscalerState::PostUpscale);
}

void FSRRemoteRenderModule::InboundDataTransfer(double deltaTime, CommandList* pCmdList)
{
    // Main loop never runs if there is no available buffer, so we can assume next buffer is in READY state
    // Transfer the resources from the shared buffer to this process
    m_DX12Ops->TransferFromSharedBuffer(getFSRResources(), m_BufferIndex, pCmdList);

    // Increase the buffer index
    m_BufferIndex = (m_BufferIndex + 1) % FSR_BUFFER_COUNT;

    // FidelityFX contexts modify the set resource view heaps, so set the cauldron one back
    SetAllResourceViewHeaps(pCmdList);
}

void FSRRemoteRenderModule::OutboundDataTransfer(double deltaTime, CommandList* pCmdList)
{
    // Main loop never runs if there is no available buffer, so we can assume next buffer is in IDLE state
    // Transfer the resources from this process to the shared buffer
    m_DX12Ops->TransferToSharedBuffer(getFSRResources(), m_BufferIndex, pCmdList);

    // Increase the buffer index
    m_BufferIndex = (m_BufferIndex + 1) % FSR_BUFFER_COUNT;

    // FidelityFX contexts modify the set resource view heaps, so set the cauldron one back
    SetAllResourceViewHeaps(pCmdList);
}
