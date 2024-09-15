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
    m_UpscalerModeEnabled = GetFramework()->IsOnlyCapability(FrameworkCapability::Upscaler);
    m_RendererModeEnabled = GetFramework()->IsOnlyCapability(FrameworkCapability::Renderer);
    m_OnlyResizing        = GetFramework()->HasCapability(FrameworkCapability::Renderer | FrameworkCapability::Upscaler);

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
    if (m_RendererModeEnabled)
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
            uint64_t bufferIndex = GetFramework()->GetBufferIndex();
            if (m_RendererModeEnabled)
                return m_DX12Ops->bufferStateMatches(bufferIndex, DX12Ops::BufferState::IDLE);
            else
                return m_DX12Ops->bufferStateMatches(bufferIndex, DX12Ops::BufferState::READY);
        });
    }

    if (m_RendererModeEnabled)
    {
        // The framework will only exit if there's no buffer to consume anymore
        GetFramework()->SetCanExitFunction([this]() {
            return m_DX12Ops->bufferStateMatchesAll(DX12Ops::BufferState::IDLE);
        });
    }

    // On Renderer, enable upscaling
    m_RenderWidth  = GetConfig()->InitialRenderWidth;
    m_RenderHeight = GetConfig()->InitialRenderHeight;
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

    // Workaround to supress warnings from the framework (only in renderer mode)
    GetFramework()->SetUpscalingState(UpscalerState::PostUpscale);
}

void FSRRemoteRenderModule::InboundDataTransfer(double deltaTime, CommandList* pCmdList)
{
    GPUScopedProfileCapture sampleMarker(pCmdList, L"FSR Remote (Inbound)");

    // Main loop never runs if there is no available buffer, so we can assume next buffer is in READY state
    // Transfer the resources from the shared buffer to this process
    uint64_t bufferIndex = GetFramework()->GetBufferIndex();
    m_DX12Ops->TransferFromSharedBuffer(getFSRResources(), bufferIndex, pCmdList);
}

void FSRRemoteRenderModule::OutboundDataTransfer(double deltaTime, CommandList* pCmdList)
{
    GPUScopedProfileCapture sampleMarker(pCmdList, L"FSR Remote (Outbound)");

    // Main loop never runs if there is no available buffer, so we can assume next buffer is in IDLE state
    // Transfer the resources from this process to the shared buffer
    uint64_t bufferIndex = GetFramework()->GetBufferIndex();
    m_DX12Ops->TransferToSharedBuffer(getFSRResources(), bufferIndex, pCmdList);
}
