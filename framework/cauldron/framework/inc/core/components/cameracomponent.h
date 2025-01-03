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
#pragma once

#include "core/component.h"
#include "misc/math.h"
#include <functional>
#include "windows.h"

namespace cauldron
{
    class CameraComponent;

    /**
     * @class CameraComponentMgr
     *
     * Component manager class for <c><i>CameraComponent</i></c>s.
     *
     * @ingroup CauldronComponent
     */
    class CameraComponentMgr : public ComponentMgr
    {
    public:
        static const wchar_t* s_ComponentName;

    public:
        /**
         * @brief   Constructor with default behavior.
         */
        CameraComponentMgr();

        /**
         * @brief   Destructor with default behavior.
         */
        virtual ~CameraComponentMgr();

        /**
         * @brief   Component creator.
         */
        virtual Component* SpawnComponent(Entity* pOwner, ComponentData* pData) override { return reinterpret_cast<Component*>(SpawnCameraComponent(pOwner, pData)); }

        /**
         * @brief   Allocates a new <c><i>CameraComponent</i></c> for the given entity.
         */
        CameraComponent* SpawnCameraComponent(Entity* pOwner, ComponentData* pData);

        /**
         * @brief   Gets the component type string ID.
         */
        virtual const wchar_t* ComponentType() const override { return s_ComponentName; }

        /**
         * @brief   Initializes the component manager.
         */
        virtual void Initialize() override;

        /**
         * @brief   Shuts down the component manager.
         */
        virtual void Shutdown() override;

        /**
         * @brief   Component manager instance accessor.
         */
        static CameraComponentMgr* Get() { return s_pComponentManager; }

    private:
        static CameraComponentMgr* s_pComponentManager;
    };

    enum class CameraType
    {
        Perspective,
        Orthographic
    };

    /**
     * @enum CameraAnimationData
     *
     * @ingroup CauldronComponent
     */
    struct CameraAnimationData
    {
        bool  enabled = false;
        double p, q, xo, yo, zo, spd;
        float lx, ly, lz;
    };

    /**
     * @struct CameraComponentData
     *
     * Initialization data structure for the <c><i>CameraComponent</i></c>.
     *
     * @ingroup CauldronComponent
     */
    struct CameraComponentData : public ComponentData
    {
        CameraType    Type       = CameraType::Perspective; ///< <c><i>CameraType</i></c>. Either perspective or orthographic.
        float         Znear      = 0.1f;                    ///< Camera near Z
        float         Zfar       = 100.0f;                  ///< Camera far Z

        union
        {
            struct {
                float Yfov;
                float AspectRatio;
            } Perspective;
            struct {
                float Xmag;
                float Ymag;
            } Orthographic;
        };
        std::wstring  Name       = L"";
    };

    /**
     * @struct CameraComponentData
     *
     * Shareable camera data structure for the <c><i>CameraComponent</i></c>.
     *
     * @ingroup CauldronComponent
     */
    struct ShareableCameraData
    {
        CameraComponentData m_Data;

        float m_Distance;
        float m_Yaw;
        float m_Pitch;

        Mat4 m_OwnerTransform;
        Mat4 m_ViewMatrix;
        Mat4 m_ProjectionMatrix;
        Mat4 m_ViewProjectionMatrix;

        Mat4 m_InvViewMatrix;
        Mat4 m_InvProjectionMatrix;
        Mat4 m_InvViewProjectionMatrix;

        Mat4 m_PrevViewMatrix;
        Mat4 m_PrevViewProjectionMatrix;

        float m_Speed;
        bool  m_Dirty;
        bool  m_ArcBallMode;

        Vec2 m_jitterValues;
        Mat4 m_ProjJittered;
        Mat4 m_PrevProjJittered;
    };

    typedef std::function<void(Vec2& values)> CameraJitterCallback;

    /**
     * @class CameraComponent
     *
     * Camera component class. Implements camera functionality on an entity.
     *
     * @ingroup CauldronComponent
     */
    class CameraComponent : public Component
    {
    public:

        /**
         * @brief   Constructor.
         */
        CameraComponent(Entity* pOwner, ComponentData* pData, CameraComponentMgr* pManager);

        /**
         * @brief   Destructor.
         */
        virtual ~CameraComponent();

        /**
         * @brief   Gets the shareable camera data.
         */
        const ShareableCameraData GetShareableData() const
        {
            return {
                *m_pData,
                m_Distance,
                m_Yaw,
                m_Pitch,
                m_pOwner->GetTransform(),
                m_ViewMatrix,
                m_ProjectionMatrix,
                m_ViewProjectionMatrix,
                m_InvViewMatrix,
                m_InvProjectionMatrix,
                m_InvViewProjectionMatrix,
                m_PrevViewMatrix,
                m_PrevViewProjectionMatrix,
                m_Speed,
                m_Dirty,
                m_ArcBallMode,
                m_jitterValues,
                m_ProjJittered,
                m_PrevProjJittered
            };
        }

        /**
         * @brief   Sets the shareable camera data.
         */
        void SetShareableData(const ShareableCameraData& data)
        {
            // Copy the internal data
            memcpy(m_pData, &data.m_Data, sizeof(CameraComponentData));

            // Set the rest of the data
            m_Distance = data.m_Distance;
            m_Yaw      = data.m_Yaw;
            m_Pitch    = data.m_Pitch;
            m_pOwner->SetTransform(data.m_OwnerTransform);
            m_ViewMatrix               = data.m_ViewMatrix;
            m_ProjectionMatrix         = data.m_ProjectionMatrix;
            m_ViewProjectionMatrix     = data.m_ViewProjectionMatrix;
            m_InvViewMatrix            = data.m_InvViewMatrix;
            m_InvProjectionMatrix      = data.m_InvProjectionMatrix;
            m_InvViewProjectionMatrix  = data.m_InvViewProjectionMatrix;
            m_PrevViewMatrix           = data.m_PrevViewMatrix;
            m_PrevViewProjectionMatrix = data.m_PrevViewProjectionMatrix;
            m_Speed                    = data.m_Speed;
            m_Dirty                    = data.m_Dirty;
            m_ArcBallMode              = data.m_ArcBallMode;
            m_jitterValues             = data.m_jitterValues;
            m_ProjJittered             = data.m_ProjJittered;
            m_PrevProjJittered         = data.m_PrevProjJittered;
        }

        /**
         * @brief   Component update. Update the camera if dirty. Processes input, updates all matrices.
         */
        virtual void Update(double deltaTime) override;

        /**
         * @brief   Component data accessor.
         */
        CameraComponentData& GetData() { return *m_pData; }
        const CameraComponentData& GetData() const { return *m_pData; }

        /**
         * @brief   Marks the camera dirty.
         */
        void SetDirty() { m_Dirty = true; }

        /**
         * @brief   Gets the camera's translation matrix.
         */
        const Vec4& GetCameraTranslation() const { return m_pOwner->GetTransform().getCol3(); }

        /**
         * @brief   Gets the camera's position.
         */
        const Vec3  GetCameraPos() const { return m_pOwner->GetTransform().getTranslation(); }

        /**
         * @brief   Gets the camera's right vector.
         */
        const Vec4 GetRight() const { return m_InvViewMatrix.getCol0(); }

        /**
         * @brief   Gets the camera's up vector.
         */
        const Vec4 GetUp() const { return m_InvViewMatrix.getCol1(); }

        /**
         * @brief   Gets the camera's direction.
         */
        const Vec4 GetDirection() const { return m_InvViewMatrix.getCol2(); }

        /**
         * @brief   Gets the camera's view matrix.
         */
        const Mat4& GetView() const { return m_ViewMatrix; }

        /**
         * @brief   Gets the camera's projection matrix.
         */
        const Mat4& GetProjection() const { return m_ProjectionMatrix; }

        /**
         * @brief   Gets the camera's view projection matrix.
         */
        const Mat4& GetViewProjection() const { return m_ViewProjectionMatrix; }

        /**
         * @brief   Gets the camera's inverse view matrix.
         */
        const Mat4& GetInverseView() const { return m_InvViewMatrix; }

        /**
         * @brief   Gets the camera's inverse projection matrix.
         */
        const Mat4& GetInverseProjection() const { return m_InvProjectionMatrix; }

        /**
         * @brief   Gets the camera's inverse view projection matrix.
         */
        const Mat4& GetInverseViewProjection() const { return m_InvViewProjectionMatrix; }

        /**
         * @brief   Gets the camera's previous view matrix.
         */
        const Mat4& GetPreviousView() const { return m_PrevViewMatrix; }

        /**
         * @brief   Gets the camera's previous view projection matrix.
         */
        const Mat4& GetPreviousViewProjection() const { return m_PrevViewProjectionMatrix; }

        /**
         * @brief   Gets the camera's jittered projection matrix.
         */
        const Mat4& GetProjectionJittered() const { return m_ProjJittered; }

        /**
         * @brief   Gets the camera's previous jittered projection matrix.
         */
        const Mat4& GetPrevProjectionJittered() const { return m_PrevProjJittered; }

        /**
         * @brief   Gets the camera's near plane value.
         */
        const float GetNearPlane() const { return m_pData->Znear; }

        /**
         * @brief   Gets the camera's far plane value.
         */
        const float GetFarPlane() const { return m_pData->Zfar; }

        /**
         * @brief   Gets the camera's horizontal field of view.
         */
        const float GetFovX() const { return std::min<float>(m_pData->Perspective.Yfov * m_pData->Perspective.AspectRatio, CAULDRON_PI2); }

        /**
         * @brief   Gets the camera's vertical field of view.
         */
        const float GetFovY() const { return m_pData->Perspective.Yfov; }

        /**
         * @brief   Gets the camera's jitter values. Used for upscaling.
         */
        const Vec2 GetJitter(uint32_t renderWidth, uint32_t renderHeight)
        {
            return Vec2(m_jitterValues.getX() * renderWidth / -2.f, m_jitterValues.getY() * renderHeight / 2.f);
        }

        /**
         * @brief   Sets the camera's jitter update callback to use.
         */
        static void SetJitterCallbackFunc(CameraJitterCallback callbackFunc) { s_pSetJitterCallback = callbackFunc; }

    private:
        CameraComponent() = delete;

        void ResetCamera();
        void SetViewBasedMatrices();
        void UpdateMatrices();

        void UpdateYawPitch();
        void LookAt(const Vec4& eyePos, const Vec4& lookAt);

        Mat4 CalculatePerspectiveMatrix();
        Mat4 CalculateOrthogonalMatrix();

        void SetProjectionJitteredMatrix();

    protected:
        // Keep a pointer on our initialization data for matrix reconstruction
        CameraComponentData*    m_pData;

        // Shared data handle and view
        std::string m_SharedDataName;
        HANDLE      m_hSharedData = nullptr;
        LPVOID      m_pSharedView = nullptr;
        ShareableCameraData* m_pSharedData[TSR_SHARED_BUFFER_MAX] = {nullptr};

        const Mat4  m_ResetMatrix = Mat4::identity(); // Used to reset camera to initial state
        float       m_Distance = 1.f;                 // Distance to look at
        float       m_Yaw = 0.f;                      // Current camera Yaw (in Radians)
        float       m_Pitch = 0.f;                    // Current camera Pitch (in Radians)

        // Core matrix information
        Mat4   m_ViewMatrix = Mat4::identity();
        Mat4   m_ProjectionMatrix = Mat4::identity();
        Mat4   m_ViewProjectionMatrix = Mat4::identity();

        // Inverses
        Mat4   m_InvViewMatrix = Mat4::identity();
        Mat4   m_InvProjectionMatrix = Mat4::identity();
        Mat4   m_InvViewProjectionMatrix = Mat4::identity();

        // Temporal matrices
        Mat4   m_PrevViewMatrix = Mat4::identity();
        Mat4   m_PrevViewProjectionMatrix = Mat4::identity();

        float           m_Speed = 1.f;          // Camera speed modifier to move faster/slower when moving around
        bool            m_Dirty = true;         // Whether or not we need to recalculate everything
        bool            m_ArcBallMode = true;   // Use arc-ball rotation or WASD free cam

        // Jitter
        Vec2 m_jitterValues     = Vec2(0, 0);
        Mat4 m_ProjJittered     = Mat4::identity();
        Mat4 m_PrevProjJittered = Mat4::identity();
        static CameraJitterCallback s_pSetJitterCallback;
    };

} // namespace cauldron
