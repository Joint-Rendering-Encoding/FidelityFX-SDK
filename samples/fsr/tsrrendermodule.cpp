#include "tsrrendermodule.h"
#include "validation_remap.h"
#include "render/device.h"
#include "render/dynamicresourcepool.h"
#include "render/profiler.h"
#include "render/swapchain.h"
#include "render/uploadheap.h"
#include "core/scene.h"

// We need to include internal headers to access the DX12 resources
#include "render/dx12/gpuresource_dx12.h"
#include "render/renderdefines.h"
#include "render/texture.h"

#include <functional>

using namespace cauldron;

TSRGraphicsResource TSRRenderModule::getTSRResourceFromTexture(const cauldron::Texture* res) const
{
    const auto&         impl       = res->GetResource();
    D3D12_RESOURCE_DESC desc       = impl->GetImpl()->DX12Desc();
    ResourceFormat      format     = impl->GetTextureResource()->GetFormat();
    DXGI_FORMAT         dxgiFormat = GetDXGIFormat(format);

    return TSRGraphicsResource{impl->GetImpl()->DX12Resource(), desc, GetResourceFormatStride(format), dxgiFormat};
}

void TSRRenderModule::Init(const json& initData)
{
    //////////////////////////////////////////////////////////////////////////
    // Resource setup

    // Check TSR mode
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
        ExecuteCallback callbackPreSwap      = std::bind(&TSRRenderModule::OutboundDataTransfer, this, std::placeholders::_1, std::placeholders::_2);
        ExecutionTuple  callbackPreSwapTuple = std::make_pair(L"TSRRenderModule::PreSwapChain", std::make_pair(this, callbackPreSwap));
        GetFramework()->RegisterExecutionCallback(L"SwapChainRenderModule", true, callbackPreSwapTuple);
    }

    if (!m_OnlyResizing)
    {
        // Create TSROps
        m_TSROps = std::make_unique<TSROps>(GetFramework()->GetName(),
                                            GetDevice()->GetImpl()->DX12Device(),
                                            GetDevice()->GetImpl()->DX12CmdQueue(CommandQueue::Graphics),
                                            GetFramework()->GetBufferCount());

        // Intialize the shared buffers
        m_TSROps->CreateSharedBuffers(getFSRResources(), !m_UpscalerModeEnabled);

        // The framework will run MainLoop based on the outcome of this function
        GetFramework()->SetReadyFunction([this]() {
            uint64_t bufferIndex = GetFramework()->GetBufferIndex();
            if (m_RendererModeEnabled)
                return m_TSROps->bufferStateMatches(bufferIndex, TSROps::BufferState::IDLE);
            else
                return m_TSROps->bufferStateMatches(bufferIndex, TSROps::BufferState::READY);
        });
    }

    if (m_RendererModeEnabled)
    {
        // The framework will only exit if there's no buffer to consume anymore
        GetFramework()->SetCanExitFunction([this]() { return m_TSROps->bufferStateMatchesAll(TSROps::BufferState::IDLE); });
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

TSRRenderModule::~TSRRenderModule()
{
    // Protection
    if (ModuleEnabled())
        EnableModule(false);
}

void TSRRenderModule::OnResize(const ResolutionInfo& resInfo)
{
    if (!ModuleEnabled() || resInfo.RenderWidth == m_RenderWidth && resInfo.RenderHeight == m_RenderHeight)
        return;

    // If we are not in benchmark mode, we don't need to force the resolution
    if (!GetConfig()->EnableBenchmark)
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

void TSRRenderModule::Execute(double deltaTime, CommandList* pCmdList)
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

void TSRRenderModule::InboundDataTransfer(double deltaTime, CommandList* pCmdList)
{
    GPUScopedProfileCapture sampleMarker(pCmdList, L"TSR (Inbound)");

    // Main loop never runs if there is no available buffer, so we can assume next buffer is in READY state
    // Transfer the resources from the shared buffer to this process
    uint64_t bufferIndex = GetFramework()->GetBufferIndex();
    m_TSROps->TransferFromSharedBuffer(getFSRResources(), bufferIndex, pCmdList->GetImpl()->DX12CmdList());
}

void TSRRenderModule::OutboundDataTransfer(double deltaTime, CommandList* pCmdList)
{
    GPUScopedProfileCapture sampleMarker(pCmdList, L"TSR (Outbound)");

    // Main loop never runs if there is no available buffer, so we can assume next buffer is in IDLE state
    // Transfer the resources from this process to the shared buffer
    uint64_t bufferIndex = GetFramework()->GetBufferIndex();
    m_TSROps->TransferToSharedBuffer(getFSRResources(), bufferIndex, pCmdList->GetImpl()->DX12CmdList());
}
