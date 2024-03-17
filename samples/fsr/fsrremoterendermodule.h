#pragma once

#include "render/rendermodule.h"
#include "core/framework.h"
#include "core/uimanager.h"

#include "utils/dx12ops.h"
#include "utils/connops.h"
#include "common.h"

#include <functional>

using namespace common;

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
     * @brief   Setup parameters that the FSR Remote API needs this frame and then send the resources to the relay server.
     */
    void Execute(double deltaTime, cauldron::CommandList* pCmdList) override;

    /**
     * @brief   Recreate the FSR Remote API Context to resize internal resources. Called by the framework when the resolution changes.
     */
    void OnResize(const cauldron::ResolutionInfo& resInfo) override;
    
    // Public variables
    bool m_Connected = false;

private:
    // For UI params
    cauldron::UISection m_UISection;

    // FSR Remote variables
    bool                        m_StartOnLoad = false;
    bool                        m_RelayMode   = false;
    std::unique_ptr<Connection> m_Connection;

    // DX12Ops
    std::unique_ptr<DX12Ops> m_DX12Ops;

    // FSR Remote CPU-GPU Highway 101
    void OutboundDataTransfer(double deltaTime, cauldron::CommandList* pCmdList);
    void InboundDataTransfer(double deltaTime, cauldron::CommandList* pCmdList);

    // FidelityFX Super Resolution resources
    const cauldron::Texture* m_pColorTarget     = nullptr;
    const cauldron::Texture* m_pDepthTarget     = nullptr;
    const cauldron::Texture* m_pMotionVectors   = nullptr;
    const cauldron::Texture* m_pReactiveMask    = nullptr;
    const cauldron::Texture* m_pCompositionMask = nullptr;

    FSRResources getFSRResources() const
    {
        return {m_pColorTarget->GetResource(),
                m_pDepthTarget->GetResource(),
                m_pMotionVectors->GetResource(),
                m_pReactiveMask->GetResource(),
                m_pCompositionMask->GetResource()};
    }
};
