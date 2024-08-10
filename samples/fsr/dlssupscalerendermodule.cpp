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
#include "render/profiler.h"
#include "render/dynamicresourcepool.h"
#include "core/scene.h"

#include <functional>

#include "sl_matrix_helpers.h"

using namespace cauldron;
using namespace math;

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

    // Set up DLSS
    m_DLSSOptions.mode                   = initData["mode"].get<sl::DLSSMode>();
    m_DLSSOptions.dlaaPreset             = initData["dlaaPreset"].get<sl::DLSSPreset>();
    m_DLSSOptions.qualityPreset          = initData["qualityPreset"].get<sl::DLSSPreset>();
    m_DLSSOptions.balancedPreset         = initData["balancedPreset"].get<sl::DLSSPreset>();
    m_DLSSOptions.performancePreset      = initData["performancePreset"].get<sl::DLSSPreset>();
    m_DLSSOptions.ultraPerformancePreset = initData["ultraPerformancePreset"].get<sl::DLSSPreset>();

    // Get render resolution from config
    m_RenderWidth  = GetConfig()->InitialRenderWidth;
    m_RenderHeight = GetConfig()->InitialRenderHeight;

    // Fetch needed resources
    m_pColorTarget   = GetFramework()->GetColorTargetForCallback(GetName());
    m_pDepthTarget   = GetFramework()->GetRenderTexture(L"DepthTarget");
    m_pMotionVectors = GetFramework()->GetRenderTexture(L"GBufferMotionVectorRT");

    CauldronAssert(ASSERT_CRITICAL, m_pColorTarget, L"Could not get color target for DLSSUpscale render modules");
    CauldronAssert(ASSERT_CRITICAL, m_pDepthTarget, L"Could not get depth target for DLSSUpscale render modules");
    CauldronAssert(ASSERT_CRITICAL, m_pMotionVectors, L"Could not get motion vectors for DLSSUpscale render modules");

    // Set up a temporary color target
    TextureDesc           desc    = m_pColorTarget->GetDesc();
    const ResolutionInfo& resInfo = GetFramework()->GetResolutionInfo();
    desc.Width                    = resInfo.RenderWidth;
    desc.Height                   = resInfo.RenderHeight;
    desc.Name                     = L"DLSS_Copy_Color";
    m_pTempColorTarget            = GetDynamicResourcePool()->CreateRenderTexture(
        &desc, [](TextureDesc& desc, uint32_t displayWidth, uint32_t displayHeight, uint32_t renderingWidth, uint32_t renderingHeight) {
            desc.Width  = renderingWidth;
            desc.Height = renderingHeight;
        });

    // Start disabled as this will be enabled externally
    SetModuleEnabled(false);

    // That's all we need for now
    SetModuleReady(true);
}

DLSSUpscaleRenderModule::~DLSSUpscaleRenderModule()
{
    // Protection
    if (ModuleEnabled())
        EnableModule(false);
}

void DLSSUpscaleRenderModule::EnableModule(bool enabled)
{
    if (enabled)
    {
        GetFramework()->EnableUpscaling(true);

        // Test the configuration
        sl::DLSSOptimalSettings m_DLSSSettings;
        m_DLSSOptions.outputWidth  = GetFramework()->GetResolutionInfo().DisplayWidth;
        m_DLSSOptions.outputHeight = GetFramework()->GetResolutionInfo().DisplayHeight;
        sl::Result res             = slDLSSGetOptimalSettings(m_DLSSOptions, m_DLSSSettings);
        CauldronAssert(ASSERT_CRITICAL, res == sl::Result::eOk, L"Failed to get optimal DLSS settings");
        CauldronWarning(L"DLSS optimal settings: %dx%d, sharpness: %f. Resolution range (%d, %d) to (%d, %d)",
                        m_DLSSSettings.optimalRenderWidth,
                        m_DLSSSettings.optimalRenderHeight,
                        m_DLSSSettings.optimalSharpness,
                        m_DLSSSettings.renderWidthMin,
                        m_DLSSSettings.renderHeightMin,
                        m_DLSSSettings.renderWidthMax,
                        m_DLSSSettings.renderHeightMax);

        // Check if viewport is within the optimal range
        CauldronAssert(ASSERT_CRITICAL,
                       m_RenderWidth >= m_DLSSSettings.renderWidthMin,
                       L"Render width is below optimal DLSS range (%d, %d)",
                       m_RenderWidth,
                       m_DLSSSettings.renderWidthMin);
        CauldronAssert(ASSERT_CRITICAL,
                       m_RenderWidth <= m_DLSSSettings.renderWidthMax,
                       L"Render width is above optimal DLSS range (%d, %d)",
                       m_RenderWidth,
                       m_DLSSSettings.renderWidthMax);
        CauldronAssert(ASSERT_CRITICAL,
                       m_RenderHeight >= m_DLSSSettings.renderHeightMin,
                       L"Render height is below optimal DLSS range (%d, %d)",
                       m_RenderHeight,
                       m_DLSSSettings.renderHeightMin);
        CauldronAssert(ASSERT_CRITICAL,
                       m_RenderHeight <= m_DLSSSettings.renderHeightMax,
                       L"Render height is above optimal DLSS range (%d, %d)",
                       m_RenderHeight,
                       m_DLSSSettings.renderHeightMax);
    }
    else
    {
        GetFramework()->EnableUpscaling(false);
    }

    SetModuleEnabled(enabled);
}

void DLSSUpscaleRenderModule::OnResize(const ResolutionInfo& resInfo)
{
    if (!ModuleEnabled())
        return;
}

void DLSSUpscaleRenderModule::Execute(double deltaTime, CommandList* pCmdList)
{
    GPUScopedProfileCapture sampleMarker(pCmdList, L"DLSS2");
    sl::Result              res = sl::Result::eOk;
    const ResolutionInfo&   resInfo = GetFramework()->GetResolutionInfo();
    CameraComponent*        pCamera = GetScene()->GetCurrentCamera();
    sl::Extent              renderExtent = {0, 0, resInfo.RenderWidth, resInfo.RenderHeight};
    sl::Extent              fullExtent = {0, 0, resInfo.DisplayWidth, resInfo.DisplayHeight};

    // Copy color target to temporary target
    std::vector<Barrier> barriers;
    barriers.push_back(Barrier::Transition(
        m_pTempColorTarget->GetResource(), ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource, ResourceState::CopyDest));
    barriers.push_back(Barrier::Transition(
        m_pColorTarget->GetResource(), ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource, ResourceState::CopySource));
    ResourceBarrier(pCmdList, static_cast<uint32_t>(barriers.size()), barriers.data());

    TextureCopyDesc desc(m_pColorTarget->GetResource(), m_pTempColorTarget->GetResource());
    CopyTextureRegion(pCmdList, &desc);

    barriers[0] = Barrier::Transition(
        m_pTempColorTarget->GetResource(), ResourceState::CopyDest, ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource);
    barriers[1] = Barrier::Transition(
        m_pColorTarget->GetResource(), ResourceState::CopySource, ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource);
    ResourceBarrier(pCmdList, static_cast<uint32_t>(barriers.size()), barriers.data());

    // Transition color target for unordered access
    barriers.clear();
    barriers.push_back(Barrier::Transition(
        m_pColorTarget->GetResource(), ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource, ResourceState::UnorderedAccess));
    ResourceBarrier(pCmdList, static_cast<uint32_t>(barriers.size()), barriers.data());

    // Tag required resources
    sl::Resource colorIn = {sl::ResourceType::eTex2d,
                             (void*)m_pTempColorTarget->GetResource()->GetImpl()->DX12Resource(),
                             nullptr,
                             nullptr,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    sl::Resource colorOut = {sl::ResourceType::eTex2d,
                             (void*)m_pColorTarget->GetResource()->GetImpl()->DX12Resource(),
                             nullptr,
                             nullptr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    sl::Resource depth    = {sl::ResourceType::eTex2d,
                             (void*)m_pDepthTarget->GetResource()->GetImpl()->DX12Resource(),
                             nullptr,
                             nullptr,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    sl::Resource mvec     = {sl::ResourceType::eTex2d,
                             (void*)m_pMotionVectors->GetResource()->GetImpl()->DX12Resource(),
                             nullptr,
                             nullptr,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};

    sl::ResourceTag colorInTag  = sl::ResourceTag{&colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent};
    sl::ResourceTag colorOutTag = sl::ResourceTag{&colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &fullExtent};
    sl::ResourceTag depthTag    = sl::ResourceTag{&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent};
    sl::ResourceTag mvecTag     = sl::ResourceTag{&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent};

    sl::ResourceTag tags[] = {colorInTag, colorOutTag, depthTag, mvecTag};
    res                    = slSetTag(m_Viewport, tags, _countof(tags), pCmdList->GetImpl()->DX12CmdList());
    CauldronAssert(ASSERT_CRITICAL, res == sl::Result::eOk, L"Failed to set DLSS tags (%d)", res);

    // Set DLSS options
    m_DLSSOptions.outputWidth           = resInfo.DisplayWidth;
    m_DLSSOptions.outputHeight          = resInfo.DisplayHeight;
    m_DLSSOptions.preExposure           = GetScene()->GetSceneExposure();
    m_DLSSOptions.sharpness             = 0.8f;
    m_DLSSOptions.useAutoExposure       = sl::eTrue;
    m_DLSSOptions.colorBuffersHDR       = sl::eTrue;
    m_DLSSOptions.alphaUpscalingEnabled = sl::eFalse;
    res                                 = slDLSSSetOptions(m_Viewport, m_DLSSOptions);
    CauldronAssert(ASSERT_CRITICAL, res == sl::Result::eOk, L"Failed to set DLSS options (%d)", res);

    // Get a new frame token
    uint32_t pFrameIndex = GetFramework()->GetFrameID();
    slGetNewFrameToken(m_pFrameToken, &pFrameIndex);

    // Provide common constants
    sl::Constants constants{};
    constants.mvecScale              = {1.0, 1.0};
    constants.jitterOffset           = {-pCamera->GetJitter(resInfo.RenderWidth, resInfo.RenderHeight).getX(), -pCamera->GetJitter(resInfo.RenderWidth, resInfo.RenderHeight).getY()};
    constants.depthInverted          = GetConfig()->InvertedDepth ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.cameraPinholeOffset    = {0.0f, 0.0f};
    constants.reset                  = sl::Boolean::eFalse;
    constants.motionVectors3D        = sl::Boolean::eFalse;
    constants.orthographicProjection = sl::Boolean::eFalse;
    constants.motionVectorsDilated   = sl::Boolean::eFalse;
    constants.motionVectorsJittered  = sl::Boolean::eFalse;

    // Camera constants
    constants.cameraViewToClip = pCamera->GetViewProjection();
    constants.clipToCameraView = pCamera->GetInverseViewProjection();
    constants.clipToPrevClip   = pCamera->GetPreviousViewProjection();
    constants.prevClipToClip   = inverse(pCamera->GetPreviousViewProjection());

    // Camera position and direction
    constants.cameraPos   = pCamera->GetCameraPos();
    constants.cameraUp    = pCamera->GetUp().getXYZ();
    constants.cameraRight = pCamera->GetRight().getXYZ();
    constants.cameraFwd   = pCamera->GetDirection().getXYZ();

    // Camera planes
    constants.cameraNear = pCamera->GetNearPlane();
    constants.cameraFar  = pCamera->GetFarPlane();
    constants.cameraFOV  = pCamera->GetFovY();

    // Rest of the camera constants
    constants.cameraAspectRatio    = resInfo.GetDisplayAspectRatio();
    constants.cameraMotionIncluded = sl::Boolean::eTrue;

    sl::recalculateCameraMatrices(constants);
    res = slSetConstants(constants, *m_pFrameToken, m_Viewport);
    CauldronAssert(ASSERT_CRITICAL, res == sl::Result::eOk, L"Failed to set DLSS constants (%d)", res);

    // Evaluate DLSS
    const sl::BaseStructure* inputs[] = {&m_Viewport};

    res = slEvaluateFeature(sl::kFeatureDLSS, *m_pFrameToken, inputs, _countof(inputs), pCmdList->GetImpl()->DX12CmdList());
    CauldronAssert(ASSERT_CRITICAL, res == sl::Result::eOk, L"Failed to evaluate DLSS (%d)", res);

    // Transition color target back to pixel shader resource
    barriers.clear();
    barriers.push_back(Barrier::Transition(
        m_pColorTarget->GetResource(), ResourceState::UnorderedAccess, ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource));
    ResourceBarrier(pCmdList, static_cast<uint32_t>(barriers.size()), barriers.data());

    SetAllResourceViewHeaps(pCmdList);

    // We are now done with upscaling
    GetFramework()->SetUpscalingState(UpscalerState::PostUpscale);
}
