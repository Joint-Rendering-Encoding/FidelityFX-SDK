#pragma once

#include "render/rendermodule.h"
#include "core/framework.h"
#include "core/uimanager.h"

#include <functional>

namespace cauldron
{
    class Texture;
}  // namespace cauldron

/**
 * @class FSRRemoteRenderModule
 *
 * FSRRemoteRenderModule takes care of:
 *      - Copying necessary resources from the GPU to the CPU
 *      - Sending the resources to the relay server
 */
class FSRRemoteRenderModule : public cauldron::RenderModule
{
public:
    /**
     * @brief   Constructor with default behavior.
     */
    FSRRemoteRenderModule()
        : RenderModule(L"FSRRemoteRenderModule")
    {
    }

    /**
     * @brief   Tear down the FSR Remote API Context and release resources.
     */
    virtual ~FSRRemoteRenderModule();

    /**
     * @brief   Initialize FSR Remote API Context, create resources, and setup UI section for FSR 1.
     */
    void Init(const json& initData) override;

    /**
     * @brief   If render module is enabled, initialize the FSR Remote API Context. If disabled, destroy the FSR Remote API Context.
     */
    void EnableModule(bool enabled) override;

    /**
     * @brief   Setup parameters that the FSR Remote API needs this frame and then send the resources to the relay server.
     */
    void Execute(double deltaTime, cauldron::CommandList* pCmdList) override;

    /**
     * @brief   Recreate the FSR Remote API Context to resize internal resources. Called by the framework when the resolution changes.
     */
    void OnResize(const cauldron::ResolutionInfo& resInfo) override;

private:

    // FidelityFX Super Resolution resources
    const cauldron::Texture* m_pColorTarget = nullptr;
};
