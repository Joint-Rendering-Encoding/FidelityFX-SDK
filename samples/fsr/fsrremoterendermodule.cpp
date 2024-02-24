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
    m_pColorTarget = GetFramework()->GetColorTargetForCallback(GetName());
    switch (GetFramework()->GetSwapChain()->GetSwapChainDisplayMode())
    {
    case DisplayMode::DISPLAYMODE_LDR:
        m_pColorTarget = GetFramework()->GetRenderTexture(L"LDR8Color");
        break;
    case DisplayMode::DISPLAYMODE_HDR10_2084:
    case DisplayMode::DISPLAYMODE_FSHDR_2084:
        m_pColorTarget = GetFramework()->GetRenderTexture(L"HDR10Color");
        break;
    case DisplayMode::DISPLAYMODE_HDR10_SCRGB:
    case DisplayMode::DISPLAYMODE_FSHDR_SCRGB:
        m_pColorTarget = GetFramework()->GetRenderTexture(L"HDR16Color");
        break;
    }
    CauldronAssert(ASSERT_CRITICAL, m_pColorTarget, L"Could not get one of the needed resources for FSRRemote Rendermodule.");

    // Start disabled as this will be enabled externally
    SetModuleEnabled(false);

    // That's all we need for now
    SetModuleReady(true);
}

FSRRemoteRenderModule::~FSRRemoteRenderModule()
{
    // Protection
    if (ModuleEnabled())
        EnableModule(false);  // Destroy FSR context
}

void FSRRemoteRenderModule::EnableModule(bool enabled)
{
    // If disabling the render module, we need to disable the upscaler with the framework
    if (enabled)
    {
        // Toggle this now so we avoid the context changes in OnResize
        SetModuleEnabled(enabled);
    }
    else
    {
        // Toggle this now so we avoid the context changes in OnResize
        SetModuleEnabled(enabled);

        GetFramework()->EnableUpscaling(false);
    }
}

void FSRRemoteRenderModule::OnResize(const ResolutionInfo& resInfo)
{
    if (!ModuleEnabled())
        return;
}

bool done         = false;
int  captureFrame = 60;
int  frameCounter = 0;
void FSRRemoteRenderModule::Execute(double deltaTime, CommandList* pCmdList)
{
    D3D12_RESOURCE_DESC fromDesc = m_pColorTarget->GetResource()->GetImpl()->DX12Desc();
    UINT64              width    = fromDesc.Width;
    UINT64              height   = fromDesc.Height;
    ResourceFormat      format   = m_pColorTarget->GetResource()->GetTextureResource()->GetFormat();

    if (done || frameCounter < captureFrame)
    {
        frameCounter++;

        if (!done)
            GetFramework()->GetSwapChain()->CopyDataToResource(m_pColorTarget->GetResource(), NULL);
    }
    else
    {
        void* pCPUData = malloc(width * height * GetResourceFormatStride(format));
        GetFramework()->GetSwapChain()->CopyResourceToCPU(m_pColorTarget->GetResource(), pCPUData);

        // Write the data to a PGM file
        std::ofstream pgmFile("output.pgm", std::ios::out | std::ios::binary);
        if (!pgmFile)
        {
            Log::Write(LOGLEVEL_ERROR, L"Unable to open file for output");
            return;
        }

        // Write PGM header
        pgmFile << "P5" << std::endl;
        pgmFile << width << " " << height << std::endl;
        pgmFile << "255" << std::endl;

        // Convert RGBA data to grayscale and write to file
        unsigned char* rgbaData      = reinterpret_cast<unsigned char*>(pCPUData);
        unsigned char* grayscaleData = new unsigned char[width * height];
        for (int i = 0; i < width * height; ++i)
        {
            // Average RGB components to obtain grayscale
            grayscaleData[i] = (rgbaData[i * 4] + rgbaData[i * 4 + 1] + rgbaData[i * 4 + 2]) / 3;
        }

        // Write grayscale data to file
        pgmFile.write(reinterpret_cast<char*>(grayscaleData), width * height);

        // Clean up
        delete[] grayscaleData;

        // Close file
        pgmFile.close();

        // Set done flag
        done = true;

        // Free the data
        free(const_cast<void*>(pCPUData));

        Log::Write(LOGLEVEL_INFO, L"Written example file...");
    }

    // FidelityFX contexts modify the set resource view heaps, so set the cauldron one back
    SetAllResourceViewHeaps(pCmdList);
}
