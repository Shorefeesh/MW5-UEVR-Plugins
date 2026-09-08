#pragma once

#include <PluginExtension.h>
#include <MW5.h>
#include "WidgetRenderData.h"

class Renderer;

class HUD final : public PluginExtension {
public:
    static inline HUD* Instance;

    HUD();
    virtual ~HUD() override;

private:
    Renderer*                           Renderer          = nullptr;
    API::UObject*                       Pawn              = nullptr;
    FPrimitiveComponentId               TargetBoxId       = {};
    float*                              CurrentZoomLevel  = nullptr;
    float*                              Brightness        = nullptr;
    float                               CurrentBrightness = 1.0f;
    HUDWidgetRenderData                 HUDWidgets[4]{};
    std::vector<MarkerWidgetRenderData> MarkerWidgets[3]{};
    float                               ZoomLevel[3]{};
    bool                                InMech[3]{};
    bool                                CurrentlyInMech      = false;
    API::UObject*                       ZoomCameraRotator     = nullptr;
    API::UObject*                       ArmsCrosshairRotator  = nullptr;
    API::UObject*                       ArmsTargetRotator     = nullptr;
    API::UObject*                       ZoomCameraDisplay     = nullptr;
    API::UObject*                       TorsoCrosshairComponent = nullptr;
    API::UObject*                       ArmsTargetCrosshairComponent = nullptr;
    API::UObject*                       ZoomOutline           = nullptr;
    API::UObject*                       ZoomOutlineClipParent = nullptr;
    vec3*                               CurrentArmAimDirection = nullptr;
    BP_FUNC                             HUDManagerReceiveTickOriginal = nullptr;
    bool                                HUDManagerTickHookInstalled   = false;
    bool                                ZoomDisplayAttached           = false;
    bool                                ZoomOutlineMoved              = false;
    bool                                ZoomAlignmentLogged           = false;

    void Reset();

    virtual void OnInitialize() override;

    bool OnNewPawn(API::UObject* activePawn);

    void ImproveZoomCaptureResolution(API::UObject* mechCockpit);
    void ImproveZoomCaptureResolution(API::UObject* sceneCapture, const char* eyeName);

    bool AlignZoomCameraToArmsTarget();
    bool AlignWeaponCrosshairToZoomCamera();
    bool AttachZoomDisplayToArmsTarget();
    static bool SetWorldRotation(API::UObject* component, const vec3& rotation);
    static bool SetWorldLocation(API::UObject* component, const vec3& location);
    bool MoveZoomOutlineToArmsTarget();
    bool SetZoomOutlineTranslation(const FVector2D& translation);
    bool SetZoomOutlineOpacity(float opacity);
    bool DisableZoomOutlineClipping();

    static void* HUDManager_ReceiveTick(API::UObject* self, FFrame* frame, void* result);

    bool TryGetWidget3DRenderData(const char* name, WidgetType type, API::UObject* component, bool logFailures = true);

    static int32_t SCanvas_OnPaint(SCanvas* self,
                                   void*    args,
                                   void*    allottedGeometry,
                                   void*    myCullingRect,
                                   void*    outDrawElements,
                                   int32_t  layerId,
                                   void*    inWidgetStyle,
                                   bool     bParentEnabled);

    static void FRendererModule_BeginRenderingViewFamily(FRendererModule* self, FCanvas* canvas, FSceneViewFamily* viewFamily);

    static void FWidget3DSceneProxy_GetDynamicMeshElements(FWidget3DSceneProxy*             self,
                                                           const TArray<const FSceneView*>& views,
                                                           const FSceneViewFamily&          viewFamily,
                                                           uint32_t                         visibilityMap,
                                                           void*                            collector);

    static bool FWidget3DSceneProxy_CanBeOccluded(FWidget3DSceneProxy* self);

    static void ProcessHUDWidget(const FWidget3DSceneProxy* proxy, const TArray<const FSceneView*>& views, uint32_t frameIndex, HUDWidgetRenderData* widget);

    static void ProcessMarkerWidget(FWidget3DSceneProxy*                 proxy,
                                    const TArray<const FSceneView*>&     views,
                                    bool                           isZoomCamera,
                                    float                                zoomLevel,
                                    FPrimitiveComponentId                targetBoxId,
                                    std::vector<MarkerWidgetRenderData>& markers);

    static mat4 GetModelMatrix(const mat4& localToWorld, int32_t sizeX, int32_t sizeY, float zoomScale);

    bool ValidateMech();

    static void PostProcessMotionBlur_AddMotionBlurVelocityPass(FRDGBuilder*                graphBuilder,
                                                                const FViewInfo&            view,
                                                                const FMotionBlurViewports& viewports,
                                                                FRDGTexture*                colorTexture,
                                                                FRDGTexture*                depthTexture,
                                                                FRDGTexture*                velocityTexture,
                                                                FRDGTexture**               velocityFlatTextureOutput,
                                                                FRDGTexture**               velocityTileTextureOutput);

    static void FMotionBlurFilterPS_TRDGLambdaPass_ExecuteImpl(FMotionBlurFilterPS::TRDGLambdaPass* self, FRHICommandList* rhiCmdList);
};
