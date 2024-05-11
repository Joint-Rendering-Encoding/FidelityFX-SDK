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

#include "dlssupscalerendermodule.h"
#include "validation_remap.h"
#include "render/swapchain.h"

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>

#include <functional>

using namespace cauldron;

void DLSSUpscaleRenderModule::Init(const json& initData)
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
                res                               = slIsFeatureSupported(sl::kFeatureDLSS, adapterInfo);
                if (res == sl::Result::eOk)
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
    res = slGetFeatureRequirements(sl::kFeatureDLSS, requirements);
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

    // Start disabled as this will be enabled externally
    SetModuleEnabled(false);

    // That's all we need for now
    SetModuleReady(true);
}

DLSSUpscaleRenderModule::~DLSSUpscaleRenderModule()
{
    // Protection
    if (ModuleEnabled())
        EnableModule(false);  // Destroy DLSS context
}

void DLSSUpscaleRenderModule::EnableModule(bool enabled)
{
}

void DLSSUpscaleRenderModule::OnResize(const ResolutionInfo& resInfo)
{
}

void DLSSUpscaleRenderModule::Execute(double deltaTime, CommandList* pCmdList)
{
}
