// AMD Cauldron code
//
// Copyright(c) 2023 Advanced Micro Devices, Inc.All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sub-license, and / or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "fpslimiterrendermodule.h"
#include "render/dx12/device_dx12.h"

#include "core/framework.h"
#include "render/device.h"
#include "render/parameterset.h"
#include "render/pipelineobject.h"
#include "render/profiler.h"
#include "render/rootsignature.h"

#include <thread>
#include <synchapi.h>

using namespace cauldron;
#define USE_BUSY_WAIT   1

// Used in a few places
static uint32_t       sSeed;
static const uint32_t BufferLength = 32768 * 32;

FPSLimiterRenderModule::FPSLimiterRenderModule()
    : RenderModule(L"FPSLimiterRenderModule")
{
}

FPSLimiterRenderModule::~FPSLimiterRenderModule()
{
    delete m_pBuffer;
    delete m_pRootSignature;
    delete m_pParameters;
    delete m_pPipelineObj;
}

void FPSLimiterRenderModule::Init(const json& initData)
{
    // Init from config
    const CauldronConfig* pConfig = GetConfig();
    m_LimitFPS                    = pConfig->LimitFPS;
    m_LimitGPU                    = pConfig->GPULimitFPS;
    m_TargetFPS                   = pConfig->LimitedFrameRate;
    m_UseReflex                   = pConfig->UseReflex;

    // Check if Reflex is supported
    sl::Result                           res             = sl::Result::eOk;
    bool                                 reflexSupported = true;
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

                sl::Result reflexRes = slIsFeatureSupported(sl::kFeatureReflex, adapterInfo);
                sl::Result pclRes    = slIsFeatureSupported(sl::kFeaturePCL, adapterInfo);

                if (reflexRes == sl::Result::eOk && pclRes == sl::Result::eOk)
                    success++;
            }
            i++;
        }

        if (success == 0)
        {
            reflexSupported = false;
            m_UseReflex = false;
            CauldronWarning(L"Reflex is not supported on this system");
        }
    }
    else
        CauldronCritical(L"Failed to create DXGI Factory");

    if (m_UseReflex)
    {
        res = slSetFeatureLoaded(sl::kFeaturePCL, true);
        CauldronAssert(ASSERT_CRITICAL, res == sl::Result::eOk, L"Failed to load PCL (%d)", res);

        res = slSetFeatureLoaded(sl::kFeatureReflex, true);
        CauldronAssert(ASSERT_CRITICAL, res == sl::Result::eOk, L"Failed to load Reflex (%d)", res);
        m_ReflexLoaded = true;
    }

    if (reflexSupported)
    {
        // Set up Reflex
        m_ReflexOptions.mode         = sl::ReflexMode::eLowLatency;
        m_ReflexOptions.frameLimitUs = (uint32_t)(1000000 / m_TargetFPS);
        sl::Result res               = slReflexSetOptions(m_ReflexOptions);
        CauldronAssert(ASSERT_CRITICAL, res == sl::Result::eOk, L"Failed to set Reflex options (%d)", res);
    }

    // Create FPS limiter buffer and transition it right away
    BufferDesc bufDesc = BufferDesc::Data(L"FPSLimiter_Buffer", BufferLength, 4, 0, ResourceFlags::AllowUnorderedAccess);
    m_pBuffer          = Buffer::CreateBufferResource(&bufDesc, ResourceState::CommonResource);
    GetDevice()->ExecuteResourceTransitionImmediate(
        1, &Barrier::Transition(m_pBuffer->GetResource(), ResourceState::CommonResource, ResourceState::UnorderedAccess));

    // Root signature
    RootSignatureDesc signatureDesc;
    signatureDesc.AddConstantBufferView(0, ShaderBindStage::Compute, 1);
    signatureDesc.AddBufferUAVSet(0, ShaderBindStage::Compute, 1);

    m_pRootSignature = RootSignature::CreateRootSignature(L"FPSLimiter_RootSignature", signatureDesc);

    // Setup the pipeline object
    PipelineDesc psoDesc;
    psoDesc.SetRootSignature(m_pRootSignature);

    DefineList defineList;
    psoDesc.AddShaderDesc(ShaderBuildDesc::Compute(L"fpslimiter.hlsl", L"CSMain", ShaderModel::SM6_0, &defineList));

    m_pPipelineObj = PipelineObject::CreatePipelineObject(L"FPSLimiter_PipelineObj", psoDesc);

    m_pParameters = ParameterSet::CreateParameterSet(m_pRootSignature);
    m_pParameters->SetBufferUAV(m_pBuffer, 0);
    m_pParameters->SetRootConstantBufferResource(GetDynamicBufferPool()->GetResource(), 4, 0);

    // Register UI
    m_UISection.SectionName = "FPS Limiter";

    m_UISection.AddCheckBox("Enable FPS Limiter", &m_LimitFPS);
    m_UISection.AddCheckBox("Use Reflex", &m_UseReflex, [&](bool enabled) {
        if (!reflexSupported)
        {
            m_UseReflex = false;
            return;
        }

        m_UseReflex = enabled;
        if (m_UseReflex && !m_ReflexLoaded)
            CauldronCritical(L"Reflex must be selected at startup");
    });
    m_UISection.AddCheckBox("GPU Limiter", &m_LimitGPU, nullptr, &m_LimitFPS);
    m_UISection.AddIntSlider("Target FPS", &m_TargetFPS, 5, 240, nullptr, &m_LimitFPS);
    GetUIManager()->RegisterUIElements(m_UISection);

    // We are now ready for use
    SetModuleReady(true);

    // Enable the module by default
    SetModuleEnabled(true);
}

static void TimerSleepQPC(int64_t targetQPC)
{
    LARGE_INTEGER currentQPC;
    do
    {
        QueryPerformanceCounter(&currentQPC);
    } while (currentQPC.QuadPart < targetQPC);
}

static void TimerSleep(std::chrono::steady_clock::duration duration)
{
    using ticks         = std::chrono::duration<int64_t, std::ratio<1, 10000000>>;
    static HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER dueTime{};
    dueTime.QuadPart = -std::chrono::duration_cast<ticks>(duration).count();
    SetWaitableTimerEx(timer, &dueTime, 0, NULL, NULL, NULL, 0);
    WaitForSingleObject(timer, -1);
}

void FPSLimiterRenderModule::Execute(double deltaTime, cauldron::CommandList* pCmdList)
{
    if (!m_LimitFPS)
        return;

    if (m_UseReflex)
    {
        sl::FrameToken* m_pFrameToken = nullptr;

        // Get a new frame token
        slGetNewFrameToken(m_pFrameToken, GetFramework()->GetFrameID32());

        // Sleep with Reflex
        sl::Result res = slReflexSleep(*m_pFrameToken);
        CauldronAssert(ASSERT_CRITICAL, res == sl::Result::eOk, L"Failed to sleep with Reflex (%d)", res);

        // Ping PCL
        res = slPCLSetMarker(sl::PCLMarker::ePCLatencyPing, *m_pFrameToken);
        CauldronAssert(ASSERT_CRITICAL, res == sl::Result::eOk, L"Failed to ping PCL (%d)", res);

        return;
    }

    // If we aren't doing GPU-based limiting, sleep the CPU
    if (!m_LimitGPU)
    {
        CPUScopedProfileCapture marker(L"FPSLimiter");

        // CPU limiter
        #if USE_BUSY_WAIT
            LARGE_INTEGER qpf;
            QueryPerformanceFrequency(&qpf);
            int64_t targetFrameTicks = qpf.QuadPart / m_TargetFPS;

            static LARGE_INTEGER lastFrame = {};
            LARGE_INTEGER timeNow;
            QueryPerformanceCounter(&timeNow);
            int64_t delta = timeNow.QuadPart - lastFrame.QuadPart;
            if (delta < targetFrameTicks)
            {
                TimerSleepQPC(lastFrame.QuadPart + targetFrameTicks);
            }
            QueryPerformanceCounter(&lastFrame);
        #else
            // CPU limiter
            uint64_t                            targetFrameTimeUs = 1000000 / m_TargetFPS;
            std::chrono::steady_clock::duration targetFrameTime =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::microseconds{targetFrameTimeUs});
            static std::chrono::steady_clock::time_point lastFrameTime = std::chrono::steady_clock::now();
            std::chrono::steady_clock::time_point        timeNow       = std::chrono::steady_clock::now();
            std::chrono::steady_clock::duration          delta         = timeNow - lastFrameTime;

            if (delta < targetFrameTime)
            {
                TimerSleep(targetFrameTime - delta);
                timeNow += targetFrameTime - delta;
            }

            lastFrameTime = timeNow;
        #endif
        return;
    }
    else
    {
        GPUScopedProfileCapture marker(pCmdList, L"FPSLimiter");

        uint64_t lastFrameTimeUs = 0;
        auto&    timings         = GetProfiler()->GetGPUTimings();
        if (!timings.empty())
        {
            std::chrono::nanoseconds currentFrameEnd = timings[0].EndTime;
            lastFrameTimeUs = std::chrono::duration_cast<std::chrono::duration<uint64_t, std::micro>>(currentFrameEnd - m_LastFrameEnd).count();
            m_LastFrameEnd  = currentFrameEnd;
        }

        // calculate number of loops
        const double DampenFactor         = 0.05;
        const double MaxTargetFrameTimeUs = 200000.0;  // 200ms 5fps to match CPU limiter UI.
        const double MinTargetFrameTimeUs = 50.0;

        if (m_FrameTimeHistoryCount >= _countof(m_FrameTimeHistory))
        {
            m_FrameTimeHistorySum -= m_FrameTimeHistory[m_FrameTimeHistoryCount % _countof(m_FrameTimeHistory)];
        }

        m_FrameTimeHistorySum += lastFrameTimeUs;
        m_FrameTimeHistory[m_FrameTimeHistoryCount % _countof(m_FrameTimeHistory)] = lastFrameTimeUs;
        m_FrameTimeHistoryCount++;

        uint64_t targetFrameTimeUs = 1000000 / m_TargetFPS;

        double recentFrameTimeMean = double(m_FrameTimeHistorySum) / double(std::min(m_FrameTimeHistoryCount, _countof(m_FrameTimeHistory)));

        double clampedTargetFrameTimeMs = std::max(std::min(double(targetFrameTimeUs), MaxTargetFrameTimeUs), MinTargetFrameTimeUs);
        double deltaRatio               = (recentFrameTimeMean - clampedTargetFrameTimeMs) / clampedTargetFrameTimeMs;

        m_Overhead -= m_Overhead * deltaRatio * DampenFactor;
        m_Overhead = std::min(std::max(1.0, m_Overhead), 1000000.0);

        uint32_t numLoops = static_cast<uint32_t>(m_Overhead);

        auto cbv = GetDynamicBufferPool()->AllocConstantBuffer(4, &numLoops);
        m_pParameters->UpdateRootConstantBuffer(&cbv, 0);
        m_pParameters->Bind(pCmdList, m_pPipelineObj);

        SetPipelineState(pCmdList, m_pPipelineObj);

        Dispatch(pCmdList, BufferLength / 32, 1, 1);
    }
}
