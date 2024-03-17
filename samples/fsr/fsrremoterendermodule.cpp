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
    m_DX12Ops = std::make_unique<DX12Ops>();

    // Open the connection
    m_Connection = std::make_unique<Connection>(initData.value("Address", "127.0.0.1"), initData.value("Port", "12000"));

    // Intialize the internal connection buffer
    size_t newSize = m_DX12Ops->CalculateTotalSize(getFSRResources());
    m_Connection->getQueue().reset(newSize);

    // Start the server, if we are in renderer mode
    if (!m_RelayMode)
        m_Connection->run_server();

    // Set up the UI sections
    if (m_RelayMode)
    {
        m_UISection.SectionName = "FSR Remote";
        m_UISection.AddCheckBox("Connect to renderer", &m_Connected, [this](void*) {
            if (m_Connected)
                m_Connection->run_client();

            this->m_Connected = true;
        });

        // Check if we should start the connection on load
        m_StartOnLoad = initData.value("StartOnLoad", false);

        // Connect on load
        if (m_StartOnLoad)
        {
            ResolutionInfo resInfo = GetFramework()->GetResolutionInfo();
            resInfo.RenderWidth    = initData.value("RenderWidth", resInfo.RenderWidth);
            resInfo.RenderHeight   = initData.value("RenderHeight", resInfo.RenderHeight);
            
            // Set the resolution
            m_Connection->reconfigure(resInfo);

            // Connect
            m_Connection->run_client();
            m_Connected = true;
        }
    }

    // The framework will run MainLoop based on the outcome of this function
    GetFramework()->SetReadyFunction([this]() {
        if (m_RelayMode)
            return m_Connection->getQueue().nextBufferReady() || !m_Connected;
        else
            return m_Connection->getQueue().nextBufferEmpty();
    });

    // Register the UI section
    GetUIManager()->RegisterUIElements(m_UISection);

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
    if (!ModuleEnabled() || resInfo.RenderWidth == m_Connection->getResInfo().RenderWidth)
        return;

    // If we are in relay mode, only accept the resolution that we are relaying
    if (m_RelayMode && resInfo.RenderWidth != m_Connection->getResInfo().RenderWidth)
    {
        // Not only that, but also re-enable the upscaling
        CauldronWarning(L"FSRRemoteRenderModule: Relay resolution does not match the renderer resolution. Re-enabling upscaling.");
        GetFramework()->EnableUpscaling(true, [&](uint32_t displayWidth, uint32_t displayHeight) { return m_Connection->getResInfo(); });
        return;
    }

    // Resize the internal connection buffer
    size_t newSize = m_DX12Ops->CalculateTotalSize(getFSRResources());
    m_Connection->getQueue().reset(newSize);

    // Notify the renderer if we are in relay mode
    if (m_RelayMode)
        m_Connection->reconfigure(resInfo);
}

void FSRRemoteRenderModule::Execute(double deltaTime, CommandList* pCmdList)
{
    // Since this render module is always first in relay mode, we can proceed with the data transfer
    if (m_RelayMode)
        return InboundDataTransfer(deltaTime, pCmdList);

    // On renderer mode, if we received a reconfigure message, we need to reconfigure the resolution
    // So that before swap chain we can get the correct resolution
    if (m_Connection->shouldReconfigure())
        GetFramework()->EnableUpscaling(true, [&](uint32_t displayWidth, uint32_t displayHeight) { return m_Connection->getResInfo(); });
    GetFramework()->SetUpscalingState(UpscalerState::PostUpscale);
}

void FSRRemoteRenderModule::InboundDataTransfer(double deltaTime, CommandList* pCmdList)
{
    // Until we are connected, we have to run the main loop
    if (!m_Connected)
        return;

    // Enter read lock
    std::lock_guard<std::mutex> lock(m_Connection.get()->getQueue().getReadLock());

    // Get the next ready buffer
    FSRData* buffer = m_Connection->getQueue().getNextReadyBuffer();
    CauldronAssert(ASSERT_CRITICAL, buffer, L"Could not get a buffer for FSR data transfer");

    // Transfer the resources to the GPU
    m_DX12Ops->TransferResourcesToGPU(getFSRResources(), buffer, pCmdList);

    // Release the buffer
    m_Connection->getQueue().releaseBuffer(buffer);

    // FidelityFX contexts modify the set resource view heaps, so set the cauldron one back
    SetAllResourceViewHeaps(pCmdList);
}

void FSRRemoteRenderModule::OutboundDataTransfer(double deltaTime, CommandList* pCmdList)
{
    // Enter write lock
    std::lock_guard<std::mutex> lock(m_Connection->getQueue().getWriteLock());

    // Get the total size of the resources
    size_t size = m_DX12Ops->CalculateTotalSize(getFSRResources());

    // Check if the buffer size is equal to the requested size
    if (size != m_Connection->getQueue().getBufferSize())
    {
        // Relay probably reconfigured the buffer size, so we need wait until the next frame
        return;
    }

    // Try to get a buffer
    FSRData* buffer = m_Connection->getQueue().getNextEmptyBuffer(size);
    CauldronAssert(ASSERT_CRITICAL, buffer, L"Could not get a buffer for FSR data transfer");

    // Transfer the resources to the CPU
    m_DX12Ops->TransferResourcesToCPU(getFSRResources(), buffer);

    // Mark the buffer as ready
    m_Connection->getQueue().markBufferReady(buffer);

    // FidelityFX contexts modify the set resource view heaps, so set the cauldron one back
    SetAllResourceViewHeaps(pCmdList);
}
