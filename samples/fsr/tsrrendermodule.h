#pragma once

#include "render/rendermodule.h"
#include "core/framework.h"
#include "core/uimanager.h"

#include <functional>
#include <tsr.h>

/**
 * @class TSRRenderModule
 *
 * TSRRenderModule takes care of:
 *      - Copying necessary resources from/to the GPU shared buffers
 */
class TSRRenderModule : public cauldron::RenderModule
{
public:
    /**
     * @brief   Constructor with default behavior.
     */
    TSRRenderModule()
        : RenderModule(L"TSRRenderModule")
    {
    }

    /**
     * @brief   Tear down the TSR API Context and release resources.
     */
    virtual ~TSRRenderModule();

    /**
     * @brief   Initialize TSR API Context, create resources, and setup UI section for TSR.
     */
    void Init(const json& initData) override;

    /**
     * @brief   Setup parameters that the TSR API needs this frame and then send the resources to the upscaler process.
     */
    void Execute(double deltaTime, cauldron::CommandList* pCmdList) override;

    /**
     * @brief   Recreate the TSR API Context to resize internal resources. Called by the framework when the resolution changes.
     */
    void OnResize(const cauldron::ResolutionInfo& resInfo) override;

private:
    // Resolution info
    uint32_t m_RenderWidth  = 2560;
    uint32_t m_RenderHeight = 1440;

    // TSR variables
    bool m_RendererModeEnabled = false;
    bool m_UpscalerModeEnabled = false;
    bool m_OnlyResizing        = false;

    // TSROps
    std::unique_ptr<TSROps> m_TSROps;

    // TSR GPU Transfer functions
    void OutboundDataTransfer(double deltaTime, cauldron::CommandList* pCmdList);
    void InboundDataTransfer(double deltaTime, cauldron::CommandList* pCmdList);

    // FidelityFX Super Resolution resources
    const cauldron::Texture* m_pColorTarget   = nullptr;
    const cauldron::Texture* m_pDepthTarget   = nullptr;
    const cauldron::Texture* m_pMotionVectors = nullptr;

    TSRGraphicsResource getTSRResourceFromTexture(const cauldron::Texture* res) const;

    // Function to get FSR resources from the texture objects
    FSRResources getFSRResources() const
    {
        TSRGraphicsResource colorTarget   = getTSRResourceFromTexture(m_pColorTarget);
        TSRGraphicsResource depthTarget   = getTSRResourceFromTexture(m_pDepthTarget);
        TSRGraphicsResource motionVectors = getTSRResourceFromTexture(m_pMotionVectors);

        return {&colorTarget, &depthTarget, &motionVectors};
    }
};
