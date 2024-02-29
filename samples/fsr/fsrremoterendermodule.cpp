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
    }

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
    if (!ModuleEnabled())
        return;

    // Resize the internal connection buffer
    size_t newSize = m_DX12Ops->CalculateTotalSize(getFSRResources());
    m_Connection->getQueue().reset(newSize);
}

void FSRRemoteRenderModule::Execute(double deltaTime, CommandList* pCmdList)
{
    // Execute is only meaningful in relay mode, becuase this render module is always first in line
    if (m_RelayMode)
        InboundDataTransfer(deltaTime, pCmdList);
}

void FSRRemoteRenderModule::InboundDataTransfer(double deltaTime, CommandList* pCmdList)
{
    // Enter read lock
    std::lock_guard<std::mutex> lock(m_Connection.get()->getQueue().getReadLock());

    // Get the next ready buffer
    FSRData* buffer = m_Connection->getQueue().getNextReadyBuffer();

    // If we didn't get a buffer, there is no data to process
    if (!buffer)
        return;

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

    // If we didn't get a buffer, it means that the current write index is not empty
    if (!buffer)
    {
        if (!m_WarningSent)
        {
            CauldronWarning(L"FSRRemoteRenderModule::OutboundDataTransfer: No empty buffer available");
            m_WarningSent = true;
        }
        return;
    }

    // Transfer the resources to the CPU
    m_DX12Ops->TransferResourcesToCPU(getFSRResources(), buffer);

    // Mark the buffer as ready
    m_Connection->getQueue().markBufferReady(buffer);

    // Reset the warning
    m_WarningSent = false;

    // FidelityFX contexts modify the set resource view heaps, so set the cauldron one back
    SetAllResourceViewHeaps(pCmdList);
}
