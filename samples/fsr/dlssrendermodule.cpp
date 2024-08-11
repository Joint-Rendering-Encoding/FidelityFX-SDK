// This file is part of the FidelityFX SDK.
//
// Copyright (c) 2023 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "dlssrendermodule.h"
#include "validation_remap.h"
#include "render/swapchain.h"
#include "render/profiler.h"
#include "render/dynamicresourcepool.h"
#include "core/scene.h"

#include <functional>

using namespace cauldron;
using namespace math;

void DLSSRenderModule::Init(const json& initData)
{
    sl::Result res = sl::Result::eOk;

    // Check if DLSS is supported
    Microsoft::WRL::ComPtr<IDXGIFactory> factory;
    if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory)))
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter{};
        uint32_t                             i       = 0;
        uint32_t                             success = 0;
        while (factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND)
        {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc)))
            {
                sl::AdapterInfo adapterInfo{};
                adapterInfo.deviceLUID            = (uint8_t*)&desc.AdapterLuid;
                adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);

                sl::Result dlssRes   = slIsFeatureSupported(sl::kFeatureDLSS_G, adapterInfo);
                sl::Result reflexRes = slIsFeatureSupported(sl::kFeatureReflex, adapterInfo);
                sl::Result pclRes    = slIsFeatureSupported(sl::kFeaturePCL, adapterInfo);

                if (dlssRes == sl::Result::eOk && reflexRes == sl::Result::eOk && pclRes == sl::Result::eOk)
                    success++;
            }
            i++;
        }
        CauldronAssert(ASSERT_CRITICAL, success > 0, L"DLSS is not supported on this system");
    }
    else
        CauldronCritical(L"Failed to create DXGI Factory");

    // Check DLSS feature requirements
    sl::FeatureRequirements requirements{};
    res = slGetFeatureRequirements(sl::kFeatureDLSS_G, requirements);
    CauldronAssert(ASSERT_CRITICAL, res == sl::Result::eOk, L"Failed to get DLSS feature requirements");

#ifdef FFX_API_DX12
    CauldronAssert(ASSERT_CRITICAL, requirements.flags & sl::FeatureRequirementFlags::eD3D12Supported, L"DLSS requires DirectX 12");
#elif defined(FFX_API_VK)
    CauldronAssert(ASSERT_CRITICAL, requirements.flags & sl::FeatureRequirementFlags::eVulkanSupported, L"DLSS requires Vulkan");
#else
    CauldronCritical(L"DLSS requires DirectX 12 or Vulkan");
#endif

    // Check rest of the requirements
    if (requirements.flags & sl::FeatureRequirementFlags::eHardwareSchedulingRequired)
        CauldronWarning(L"DLSS requires hardware scheduling, ensure that your system is configured correctly");

    if (requirements.flags & sl::FeatureRequirementFlags::eVSyncOffRequired)
        CauldronAssert(ASSERT_CRITICAL, !GetSwapChain()->IsVSyncEnabled(), L"DLSS requires VSync to be off");

    // Set up DLSS
    m_DLSSGOptions.flags = sl::DLSSGFlags::eDynamicResolutionEnabled;
    m_DLSSGOptions.mode  = initData["mode"].get<sl::DLSSGMode>();

    // Set up Reflex
    m_ReflexOptions.mode         = sl::ReflexMode::eLowLatency;
    m_ReflexOptions.frameLimitUs = 0;
    res                          = slReflexSetOptions(m_ReflexOptions);
    CauldronAssert(ASSERT_CRITICAL, res == sl::Result::eOk, L"Failed to set Reflex options (%d)", res);

    // Fetch needed resources
    m_pDepthTarget   = GetFramework()->GetRenderTexture(L"DepthTarget");
    m_pMotionVectors = GetFramework()->GetRenderTexture(L"GBufferMotionVectorRT");

    CauldronAssert(ASSERT_CRITICAL, m_pDepthTarget, L"Could not get depth target for DLSS render modules");
    CauldronAssert(ASSERT_CRITICAL, m_pMotionVectors, L"Could not get motion vectors for DLSS render modules");

    // Start disabled as this will be enabled externally
    SetModuleEnabled(false);

    // That's all we need for now
    SetModuleReady(true);
}

DLSSRenderModule::~DLSSRenderModule()
{
    // Protection
    if (ModuleEnabled())
        EnableModule(false);
}

void DLSSRenderModule::EnableModule(bool enabled)
{
    sl::Result res = sl::Result::eOk;

    if (enabled)
    {
        // Load PCL
        res = slSetFeatureLoaded(sl::kFeaturePCL, true);
        CauldronAssert(ASSERT_CRITICAL, res == sl::Result::eOk, L"Failed to load PCL (%d)", res);

        // Load Reflex
        res = slSetFeatureLoaded(sl::kFeatureReflex, true);
        CauldronAssert(ASSERT_CRITICAL, res == sl::Result::eOk, L"Failed to load Reflex (%d)", res);

        // Load DLSS-G
        res = slSetFeatureLoaded(sl::kFeatureDLSS_G, true);
        CauldronAssert(ASSERT_CRITICAL, res == sl::Result::eOk, L"Failed to load DLSS-G (%d)", res);
    }
    else
    {
        // Unload DLSS-G
        // BUG: This doesn't really work, and we don't need it anyway as we don't change the upscalers during runtime
        // res = slSetFeatureLoaded(sl::kFeatureDLSS_G, false);
        // CauldronAssert(ASSERT_CRITICAL, res == sl::Result::eOk, L"Failed to unload DLSS-G (%d)", res);
    }

    GetFramework()->EnableFrameInterpolation(enabled);
    SetModuleEnabled(enabled);
}

void DLSSRenderModule::OnResize(const ResolutionInfo& resInfo)
{
    if (!ModuleEnabled())
        return;
}

void DLSSRenderModule::Execute(double deltaTime, CommandList* pCmdList)
{
    GPUScopedProfileCapture sampleMarker(pCmdList, L"DLSS3");
    sl::Result              res          = sl::Result::eOk;
    const ResolutionInfo&   resInfo      = GetFramework()->GetResolutionInfo();
    CameraComponent*        pCamera      = GetScene()->GetCurrentCamera();
    sl::Extent              renderExtent = {0, 0, resInfo.RenderWidth, resInfo.RenderHeight};

    // Tag required resources
    sl::Resource depth = {sl::ResourceType::eTex2d,
                          (void*)m_pDepthTarget->GetResource()->GetImpl()->DX12Resource(),
                          nullptr,
                          nullptr,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    sl::Resource mvec  = {sl::ResourceType::eTex2d,
                          (void*)m_pMotionVectors->GetResource()->GetImpl()->DX12Resource(),
                          nullptr,
                          nullptr,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};

    sl::ResourceTag depthTag = sl::ResourceTag{&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent};
    sl::ResourceTag mvecTag  = sl::ResourceTag{&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent};

    sl::ResourceTag tags[] = {depthTag, mvecTag};
    res                    = slSetTag(m_Viewport, tags, _countof(tags), pCmdList->GetImpl()->DX12CmdList());
    CauldronAssert(ASSERT_CRITICAL, res == sl::Result::eOk, L"Failed to set DLSS tags (%d)", res);

    // Set DLSS options
    m_DLSSGOptions.dynamicResHeight = resInfo.RenderHeight;
    m_DLSSGOptions.dynamicResWidth  = resInfo.RenderWidth;
    res                             = slDLSSGSetOptions(m_Viewport, m_DLSSGOptions);
    CauldronAssert(ASSERT_CRITICAL, res == sl::Result::eOk, L"Failed to set DLSS options (%d)", res);

    // Get a new frame token
    uint32_t pFrameIndex = static_cast<uint32_t>(GetFramework()->GetFrameID());
    slGetNewFrameToken(m_pFrameToken, &pFrameIndex);

    // Sleep with Reflex
    res = slReflexSleep(*m_pFrameToken);  // TODO: Integrate Reflex in FPS limiter
    CauldronAssert(ASSERT_CRITICAL, res == sl::Result::eOk, L"Failed to sleep with Reflex (%d)", res);

    // Ping PCL
    if (GetFramework()->GetBufferIndexMonotonic() > 1)
    {
        res = slPCLSetMarker(sl::PCLMarker::ePresentEnd, *m_pFrameToken);
        CauldronAssert(ASSERT_CRITICAL, res == sl::Result::eOk, L"Failed to ping PCL (%d)", res);
    }
    
    if (GetFramework()->GetBufferIndexMonotonic() > 0)
    {
        res = slPCLSetMarker(sl::PCLMarker::ePresentStart, *m_pFrameToken);
        CauldronAssert(ASSERT_CRITICAL, res == sl::Result::eOk, L"Failed to ping PCL (%d)", res);
    }

    SetAllResourceViewHeaps(pCmdList);
}
