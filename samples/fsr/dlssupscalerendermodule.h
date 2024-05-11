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

#pragma once

#include "render/rendermodule.h"
#include "core/framework.h"
#include "core/uimanager.h"

#include <functional>

namespace cauldron
{
    class Texture;
}  // namespace cauldron

/// @defgroup FfxFsrSample FidelityFX Super Resolution Sample
/// Sample documentation for FidelityFX Super Resolution

/**
 * @class DLSSUpscaleRenderModule
 *
 * DLSSUpscaleRenderModule takes care of:
 *      - creating UI section that enable users to select upscaling options
 *      - creating GPU resources
 *      - clearing and/or generating the reactivity masks
 *      - dispatch workloads for upscaling using DLSS
 */
class DLSSUpscaleRenderModule : public cauldron::RenderModule
{
public:
    /**
     * @brief   Constructor with default behavior.
     */
    DLSSUpscaleRenderModule()
        : RenderModule(L"DLSSUpscaleRenderModule")
    {
    }

    /**
     * @brief   Tear down the DLSS API Context and release resources.
     */
    virtual ~DLSSUpscaleRenderModule();

    /**
     * @brief   Initialize DLSS API Context, create resources, and setup UI section for DLSS Upscale.
     */
    void Init(const json& initData);

    /**
     * @brief   If render module is enabled, initialize the DLSS API Context. If disabled, destroy the DLSS API Context.
     */
    void EnableModule(bool enabled) override;

    /**
     * @brief   Setup parameters that the DLSS API needs this frame and then call the FFX Dispatch.
     */
    void Execute(double deltaTime, cauldron::CommandList* pCmdList) override;

    /**
     * @brief   Recreate the DLSS API Context to resize internal resources. Called by the framework when the resolution changes.
     */
    void OnResize(const cauldron::ResolutionInfo& resInfo) override;

private:
    float m_UpscaleRatio = 2.f;
};
