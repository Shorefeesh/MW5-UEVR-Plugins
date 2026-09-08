#include "HUD.h"
#include "Renderer.h"
#include "Offsets.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <RE.h>

HUD::HUD() {
    Instance                  = this;
    PluginExtension::Instance = this;
    Name                      = "HUD";
    Version                   = "2.2.2";
    VersionInt                = 222;
    VersionCheckFnName        = L"OnFetchHUDPluginData";
    VersionPropertyName       = L"HUDVersion";
    Renderer                  = new ::Renderer();
}

HUD::~HUD() {
    FWidget3DSceneProxy::GetDynamicMeshElements.Uninstall();
    FWidget3DSceneProxy::CanBeOccluded.Uninstall();
    FMotionBlurFilterPS::TRDGLambdaPass::ExecuteImpl.Uninstall();
    PostProcessMotionBlur::AddMotionBlurVelocityPass.Uninstall();
    FRendererModule::BeginRenderingViewFamily.Uninstall();
    SCanvas::OnPaint.Uninstall();
}

void HUD::OnInitialize() {
    Offsets::FindAll();
    PostProcessMotionBlur::AddMotionBlurVelocityPass.DetourOffset(Offsets::AddMotionBlurVelocityPass_Offset, &PostProcessMotionBlur_AddMotionBlurVelocityPass);
    FMotionBlurFilterPS::TRDGLambdaPass::ExecuteImpl.DetourOffset(Offsets::ExecuteImpl_Offset, &FMotionBlurFilterPS_TRDGLambdaPass_ExecuteImpl);
    FRendererModule::BeginRenderingViewFamily.DetourOffset(Offsets::FRendererModule_BeginRenderingViewFamily_Offset, &FRendererModule_BeginRenderingViewFamily);
    SCanvas::OnPaint.DetourOffset(Offsets::SCanvas_OnPaint_Offset, &SCanvas_OnPaint);
}

int32_t HUD::SCanvas_OnPaint(SCanvas* self,
                             void*    args,
                             void*    allottedGeometry,
                             void*    myCullingRect,
                             void*    outDrawElements,
                             int32_t  layerId,
                             void*    inWidgetStyle,
                             bool     bParentEnabled) {
    if (Instance->CurrentlyInMech)
        return layerId;

    // If we are outside the mech we will want these markers, esp for the star map view
    return SCanvas::OnPaint.OriginalFn(self, args, allottedGeometry, myCullingRect, outDrawElements, layerId, inWidgetStyle, bParentEnabled);
}

void HUD::FRendererModule_BeginRenderingViewFamily(FRendererModule* self, FCanvas* canvas, FSceneViewFamily* viewFamily) {
    FRendererModule::BeginRenderingViewFamily.OriginalFn(self, canvas, viewFamily);

    const uint32_t frameIndex = viewFamily->FrameNumber % 3;

    // This is on the game thread, read the current values for this frame here to avoid threading issues from reading on the render thread
    Instance->CurrentlyInMech       = Instance->ValidateMech();
    Instance->InMech[frameIndex]    = Instance->CurrentlyInMech;
    Instance->ZoomLevel[frameIndex] = Instance->CurrentlyInMech && Instance->CurrentZoomLevel ? *Instance->CurrentZoomLevel : 1.0f;
    Instance->CurrentBrightness     = Instance->CurrentlyInMech && Instance->Brightness ? *Instance->Brightness : 1.0f;
}

void HUD::Reset() {
    CurrentlyInMech   = false;
    Pawn              = nullptr;
    TargetBoxId       = {};
    Brightness        = nullptr;
    CurrentZoomLevel  = nullptr;
    CurrentBrightness = 1.0f;
    ZoomCameraRotator = nullptr;
    ArmsCrosshairRotator = nullptr;
    ArmsTargetRotator = nullptr;
    ZoomCameraDisplay = nullptr;
    TorsoCrosshairComponent = nullptr;
    ArmsTargetCrosshairComponent = nullptr;
    ZoomOutline = nullptr;
    ZoomOutlineClipParent = nullptr;
    CurrentArmAimDirection = nullptr;
    ZoomDisplayAttached = false;
    ZoomOutlineMoved = false;
    ZoomAlignmentLogged = false;

    for (auto& w : HUDWidgets) {
        w.ComponentId       = FPrimitiveComponentId{};
        w.Texture           = nullptr;
        w.SRV               = nullptr;
        w.RenderTargetSizeX = 0;
        w.RenderTargetSizeY = 0;
        std::memset(w.MVPs, 0, sizeof(w.MVPs));
        std::memset(w.RequestDraw, 0, sizeof(w.RequestDraw));
    }

    for (auto& vec : MarkerWidgets) {
        vec.clear();
    }

    for (auto& z : ZoomLevel) {
        z = 1.0f;
    }

    for (auto& im : InMech) {
        im = false;
    }
}

bool HUD::OnNewPawn(API::UObject* activePawn) {
    if (Pawn)
        Reset();

    Pawn = activePawn;
    if (!activePawn)
        return false;

    API::UObject* mechView,* mechMesh = nullptr;

    bool success = TryGetProperty(Pawn, L"MechViewComponent", mechView) &&
                   TryGetProperty(Pawn, L"MechMeshComponent", mechMesh);

    if (!success) {
        LogInfo("Failed to get Mech references - probably not in a Mech");
        return false;
    }

    API::UObject* cockpit,* mechCockpit,* armsCrosshair,* headCrosshair,* targetBox,* hudManager,* torsoCrosshairWidget = nullptr;

    success = TryGetProperty(mechMesh, L"FirstPersonCockpitComponent", cockpit) &&
              TryGetProperty(cockpit, L"ChildActor", mechCockpit) &&
              TryGetProperty(mechCockpit, L"VR_TorsoCrosshair", TorsoCrosshairComponent) &&
              TryGetProperty(mechCockpit, L"VR_ArmsCrosshair", armsCrosshair) &&
              TryGetProperty(mechCockpit, L"VR_ArmsTargetCrosshair", ArmsTargetCrosshairComponent) &&
              TryGetProperty(mechCockpit, L"VR_HeadCrosshair", headCrosshair) &&
              TryGetProperty(mechCockpit, L"VR_TargetBox", targetBox) &&
              TryGetProperty(mechCockpit, L"VR_HUDCameraRotator", ZoomCameraRotator) &&
              TryGetProperty(mechCockpit, L"VR_ArmsCrosshairRotator", ArmsCrosshairRotator) &&
              TryGetProperty(mechCockpit, L"VR_ArmsTargetCrosshairRotator", ArmsTargetRotator) &&
              TryGetProperty(mechCockpit, L"VR_HUDCameraDisplay", ZoomCameraDisplay) &&
              TryGetProperty(mechCockpit, L"VR_HUDManager", hudManager) &&
              TryGetPropertyStruct(hudManager, L"Brightness", Brightness) &&
              TryGetPropertyStruct(hudManager, L"ZoomLevel", CurrentZoomLevel) &&
              TryGetPropertyStruct(hudManager, L"CurrentArmAimDirection", CurrentArmAimDirection) &&
              TryGetProperty(hudManager, L"VR_TorsoCrosshair_Widget", torsoCrosshairWidget) &&
              TryGetProperty(torsoCrosshairWidget, L"TargetBoxBorder", ZoomOutline);

    // Clipping removal is an optional compatibility improvement. Do not prevent the
    // rest of the HUD from initializing if a different asset revision hides this widget.
    if (success)
        TryGetProperty(torsoCrosshairWidget, L"CrosshairPanel", ZoomOutlineClipParent, false);

    if (!success) {
        LogInfo("Failed to get Mech HUD references, retrying next frame...");
        Pawn = nullptr;
        Reset();
        return false;
    }

    if (!HUDManagerTickHookInstalled) {
        if (!AddEventHook(hudManager->get_class(), L"ReceiveTick", &HUDManager_ReceiveTick, &HUDManagerReceiveTickOriginal)) {
            Pawn = nullptr;
            Reset();
            return false;
        }
        HUDManagerTickHookInstalled = true;
    }

    ImproveZoomCaptureResolution(mechCockpit);
    DisableZoomOutlineClipping();

    success = TryGetWidget3DRenderData("VR_TorsoCrosshair", WidgetType::TorsoCrosshair, TorsoCrosshairComponent) &&
              TryGetWidget3DRenderData("VR_ArmsCrosshair", WidgetType::ArmsCrosshair, armsCrosshair) &&
              TryGetWidget3DRenderData("VR_ArmsTargetCrosshair", WidgetType::ArmsTargetCrosshair, ArmsTargetCrosshairComponent) &&
              TryGetWidget3DRenderData("VR_HeadCrosshair", WidgetType::HeadCrosshair, headCrosshair);

    if (!success) {
        LogInfo("Failed to get Mech HUD data, retrying next frame...");
        Pawn = nullptr;
        Reset();
        return false;
    }

    TargetBoxId = ((UPrimitiveComponent*)targetBox)->ComponentId;

    LogInfo("Mech HUD references acquired");
    return true;
}

void HUD::ImproveZoomCaptureResolution(API::UObject* mechCockpit) {
    API::UObject* normalCapture  = nullptr;
    API::UObject* thermalCapture = nullptr;
    TryGetProperty(mechCockpit, L"VR_NormalCamera", normalCapture, false);
    TryGetProperty(mechCockpit, L"VR_ThermalCamera", thermalCapture, false);

    ImproveZoomCaptureResolution(normalCapture, "normal");
    ImproveZoomCaptureResolution(thermalCapture, "thermal");
}

void HUD::ImproveZoomCaptureResolution(API::UObject* sceneCapture, const char* eyeName) {
    if (!sceneCapture)
        return;

    API::UObject* textureTarget = nullptr;
    int32_t*      width         = nullptr;
    int32_t*      height        = nullptr;
    if (!TryGetProperty(sceneCapture, L"TextureTarget", textureTarget, false) || !textureTarget ||
        !TryGetPropertyStruct(textureTarget, L"SizeX", width, false) ||
        !TryGetPropertyStruct(textureTarget, L"SizeY", height, false) ||
        !width || !height || *width <= 0 || *height <= 0)
        return;

    // The packed mod assets are 320x215. Double their linear resolution while avoiding the
    // disproportionate GPU cost of matching the full headset render target.
    constexpr int32_t targetLongEdge = 640;
    const int32_t currentLongEdge = std::max(*width, *height);
    if (currentLongEdge >= targetLongEdge) {
        LogInfo("Zoom capture %s resolution retained at %dx%d", eyeName, *width, *height);
        return;
    }

    const int32_t originalWidth  = *width;
    const int32_t originalHeight = *height;
    const float scale = static_cast<float>(targetLongEdge) / static_cast<float>(currentLongEdge);
    struct ResizeTargetParams {
        int32_t Width;
        int32_t Height;
    } params{
        std::max(1, static_cast<int32_t>(std::lround(static_cast<float>(*width) * scale))),
        std::max(1, static_cast<int32_t>(std::lround(static_cast<float>(*height) * scale)))
    };

    textureTarget->call_function(L"ResizeTarget", &params);
    LogInfo("Zoom capture %s resolution raised from %dx%d to %dx%d", eyeName, originalWidth, originalHeight, params.Width, params.Height);
}

void* HUD::HUDManager_ReceiveTick(API::UObject* self, FFrame* frame, void* result) {
    const bool zoomActive = Instance->CurrentZoomLevel && *Instance->CurrentZoomLevel > 1.02f;

    void* returnValue = nullptr;
    if (Instance->HUDManagerReceiveTickOriginal)
        returnValue = Instance->HUDManagerReceiveTickOriginal(self, frame, result);

    if (zoomActive) {
        Instance->SetZoomOutlineOpacity(0.0f);
        if (Instance->AlignZoomCameraToArmsTarget() && !Instance->ZoomAlignmentLogged) {
            Instance->LogInfo("Zoom camera and display follow the arms-target reticle; weapon reticle retains Blueprint zoom compensation");
            Instance->ZoomAlignmentLogged = true;
        }
    } else {
        Instance->SetZoomOutlineOpacity(1.0f);
        if (Instance->ZoomOutlineMoved)
            Instance->SetZoomOutlineTranslation({0.0f, 0.0f});
        Instance->ZoomAlignmentLogged = false;
    }

    return returnValue;
}

bool HUD::AlignZoomCameraToArmsTarget() {
    if (!ZoomCameraRotator || !ArmsTargetRotator)
        return false;

    static const auto sceneComponentClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.SceneComponent");
    static const auto getRotation = sceneComponentClass ? sceneComponentClass->find_function(L"K2_GetComponentRotation") : nullptr;
    if (!getRotation)
        return false;

    const auto returnProperty = getRotation->find_property(L"ReturnValue");
    if (!returnProperty)
        return false;

    std::vector<uint8_t> params(getRotation->get_properties_size(), 0);
    ArmsTargetRotator->process_event(getRotation, params.data());
    vec3 rotation{};
    std::memcpy(&rotation, params.data() + returnProperty->get_offset(), sizeof(rotation));

    if (!ZoomDisplayAttached && !AttachZoomDisplayToArmsTarget())
        return false;

    const vec3 displayLocation = reinterpret_cast<USceneComponent*>(ArmsTargetCrosshairComponent)->ComponentToWorld.Translation;
    return SetWorldLocation(ZoomCameraDisplay, displayLocation) &&
           SetWorldRotation(ZoomCameraRotator, rotation) &&
           AlignWeaponCrosshairToZoomCamera();
}

bool HUD::AlignWeaponCrosshairToZoomCamera() {
    if (!ArmsCrosshairRotator || !ArmsTargetRotator || !CurrentArmAimDirection || !CurrentZoomLevel)
        return false;

    const auto& targetTransform = reinterpret_cast<USceneComponent*>(ArmsTargetRotator)->ComponentToWorld;
    const vec3 targetDirection = normalize(targetTransform.Rotation * vec3{1.0f, 0.0f, 0.0f});
    const vec3 weaponDirection = normalize(*CurrentArmAimDirection);
    const vec3 displayedDirection = normalize(targetDirection + (weaponDirection - targetDirection) * *CurrentZoomLevel);

    static const auto mathClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.KismetMathLibrary");
    static const auto makeRotFromX = mathClass ? mathClass->find_function(L"MakeRotFromX") : nullptr;
    static const auto mathDefault = mathClass ? mathClass->get_class_default_object() : nullptr;
    if (!makeRotFromX || !mathDefault)
        return false;

    const auto directionProperty = makeRotFromX->find_property(L"X");
    const auto returnProperty = makeRotFromX->find_property(L"ReturnValue");
    if (!directionProperty || !returnProperty)
        return false;

    std::vector<uint8_t> params(makeRotFromX->get_properties_size(), 0);
    std::memcpy(params.data() + directionProperty->get_offset(), &displayedDirection, sizeof(displayedDirection));
    mathDefault->process_event(makeRotFromX, params.data());
    vec3 rotation{};
    std::memcpy(&rotation, params.data() + returnProperty->get_offset(), sizeof(rotation));
    return SetWorldRotation(ArmsCrosshairRotator, rotation);
}

bool HUD::AttachZoomDisplayToArmsTarget() {
    if (!ZoomCameraDisplay || !ArmsTargetRotator)
        return false;

    static const auto sceneComponentClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.SceneComponent");
    static const auto attachToComponent = sceneComponentClass ? sceneComponentClass->find_function(L"K2_AttachToComponent") : nullptr;
    if (!attachToComponent)
        return false;

    const auto parentProperty = attachToComponent->find_property(L"Parent");
    if (!parentProperty)
        return false;

    // Zeroed attachment rules select KeepRelative, preserving the authored display
    // mesh offset, orientation, and scale while changing its rotator parent.
    std::vector<uint8_t> params(attachToComponent->get_properties_size(), 0);
    std::memcpy(params.data() + parentProperty->get_offset(), &ArmsTargetRotator, sizeof(ArmsTargetRotator));
    ZoomCameraDisplay->process_event(attachToComponent, params.data());
    ZoomDisplayAttached = true;
    LogInfo("Zoom display attached to the arms-target rotator");
    return true;
}

bool HUD::SetWorldRotation(API::UObject* component, const vec3& rotation) {
    if (!component)
        return false;

    static const auto sceneComponentClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.SceneComponent");
    static const auto setRotation = sceneComponentClass ? sceneComponentClass->find_function(L"K2_SetWorldRotation") : nullptr;
    if (!setRotation)
        return false;

    const auto rotationProperty = setRotation->find_property(L"NewRotation");
    if (!rotationProperty)
        return false;

    std::vector<uint8_t> params(setRotation->get_properties_size(), 0);
    std::memcpy(params.data() + rotationProperty->get_offset(), &rotation, sizeof(rotation));
    component->process_event(setRotation, params.data());
    return true;
}

bool HUD::SetWorldLocation(API::UObject* component, const vec3& location) {
    if (!component)
        return false;

    static const auto sceneComponentClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.SceneComponent");
    static const auto setLocation = sceneComponentClass ? sceneComponentClass->find_function(L"K2_SetWorldLocation") : nullptr;
    if (!setLocation)
        return false;

    const auto locationProperty = setLocation->find_property(L"NewLocation");
    if (!locationProperty)
        return false;

    std::vector<uint8_t> params(setLocation->get_properties_size(), 0);
    std::memcpy(params.data() + locationProperty->get_offset(), &location, sizeof(location));
    component->process_event(setLocation, params.data());
    return true;
}

bool HUD::MoveZoomOutlineToArmsTarget() {
    if (!TorsoCrosshairComponent || !ArmsTargetCrosshairComponent || !ZoomOutline)
        return false;

    static const auto sceneComponentClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.SceneComponent");
    static const auto getLocation = sceneComponentClass ? sceneComponentClass->find_function(L"K2_GetComponentLocation") : nullptr;
    if (!getLocation)
        return false;

    const auto locationReturn = getLocation->find_property(L"ReturnValue");
    if (!locationReturn)
        return false;

    auto getComponentLocation = [&](API::UObject* component, vec3& location) {
        std::vector<uint8_t> params(getLocation->get_properties_size(), 0);
        component->process_event(getLocation, params.data());
        std::memcpy(&location, params.data() + locationReturn->get_offset(), sizeof(location));
    };

    vec3 torsoLocation{}, armsLocation{};
    getComponentLocation(TorsoCrosshairComponent, torsoLocation);
    getComponentLocation(ArmsTargetCrosshairComponent, armsLocation);

    const auto playerController = API::get()->get_player_controller(0);
    const auto project = playerController ? playerController->get_class()->find_function(L"ProjectWorldLocationToScreen") : nullptr;
    if (!project)
        return false;

    const auto worldLocationProperty = project->find_property(L"WorldLocation");
    const auto screenLocationProperty = project->find_property(L"ScreenLocation");
    if (!worldLocationProperty || !screenLocationProperty)
        return false;

    auto projectLocation = [&](const vec3& worldLocation, FVector2D& screenLocation) {
        std::vector<uint8_t> params(project->get_properties_size(), 0);
        std::memcpy(params.data() + worldLocationProperty->get_offset(), &worldLocation, sizeof(worldLocation));
        playerController->process_event(project, params.data());
        std::memcpy(&screenLocation, params.data() + screenLocationProperty->get_offset(), sizeof(screenLocation));
    };

    FVector2D torsoScreen{}, armsScreen{};
    projectLocation(torsoLocation, torsoScreen);
    projectLocation(armsLocation, armsScreen);
    // Convert the desired screen displacement into the torso WidgetComponent's
    // local Slate units. A raw screen-pixel delta changes with camera zoom and
    // viewport resolution, which produced the observed 1/zoom displacement.
    const auto& transform = reinterpret_cast<USceneComponent*>(TorsoCrosshairComponent)->ComponentToWorld;
    constexpr float basisSample = 100.0f;
    const vec3 widgetXWorld = torsoLocation + transform.Rotation * vec3{0.0f, transform.Scale3D.y * basisSample, 0.0f};
    const vec3 widgetYWorld = torsoLocation + transform.Rotation * vec3{0.0f, 0.0f, transform.Scale3D.z * basisSample};

    FVector2D widgetXScreen{}, widgetYScreen{};
    projectLocation(widgetXWorld, widgetXScreen);
    projectLocation(widgetYWorld, widgetYScreen);

    const FVector2D basisX{(widgetXScreen.X - torsoScreen.X) / basisSample, (widgetXScreen.Y - torsoScreen.Y) / basisSample};
    const FVector2D basisY{(widgetYScreen.X - torsoScreen.X) / basisSample, (widgetYScreen.Y - torsoScreen.Y) / basisSample};
    const FVector2D screenDelta{armsScreen.X - torsoScreen.X, armsScreen.Y - torsoScreen.Y};
    const float determinant = basisX.X * basisY.Y - basisX.Y * basisY.X;
    if (std::abs(determinant) < 0.000001f)
        return false;

    return SetZoomOutlineTranslation({
        (screenDelta.X * basisY.Y - screenDelta.Y * basisY.X) / determinant,
        (basisX.X * screenDelta.Y - basisX.Y * screenDelta.X) / determinant,
    });
}

bool HUD::DisableZoomOutlineClipping() {
    if (!ZoomOutline)
        return false;

    static const auto widgetClass = API::get()->find_uobject<API::UClass>(L"Class /Script/UMG.Widget");
    static const auto setClipping = widgetClass ? widgetClass->find_function(L"SetClipping") : nullptr;
    static const auto getParent = widgetClass ? widgetClass->find_function(L"GetParent") : nullptr;
    if (!setClipping || !getParent)
        return false;

    const auto clippingProperty = setClipping->find_property(L"InClipping");
    const auto parentReturn = getParent->find_property(L"ReturnValue");
    if (!clippingProperty || !parentReturn)
        return false;

    // Clear clipping on the border and every UMG parent. CrosshairPanel is one known
    // ClipToBounds ancestor, but cooked widget revisions contain additional clipped
    // panels above it.
    constexpr uint8_t inherit = 0;
    API::UObject* widget = ZoomOutline;
    int unclipped = 0;
    while (widget && unclipped < 32) {
        std::vector<uint8_t> clippingParams(setClipping->get_properties_size(), 0);
        std::memcpy(clippingParams.data() + clippingProperty->get_offset(), &inherit, sizeof(inherit));
        widget->process_event(setClipping, clippingParams.data());
        ++unclipped;

        std::vector<uint8_t> parentParams(getParent->get_properties_size(), 0);
        widget->process_event(getParent, parentParams.data());
        API::UObject* parent = nullptr;
        std::memcpy(&parent, parentParams.data() + parentReturn->get_offset(), sizeof(parent));
        widget = parent;
    }

    LogInfo("Disabled clipping on %d zoom-outline widget ancestors", unclipped);
    return unclipped > 0;
}

bool HUD::SetZoomOutlineTranslation(const FVector2D& translation) {
    if (!ZoomOutline)
        return false;

    static const auto widgetClass = API::get()->find_uobject<API::UClass>(L"Class /Script/UMG.Widget");
    static const auto setTranslation = widgetClass ? widgetClass->find_function(L"SetRenderTranslation") : nullptr;
    if (!setTranslation)
        return false;

    const auto translationProperty = setTranslation->find_property(L"Translation");
    if (!translationProperty)
        return false;

    std::vector<uint8_t> params(setTranslation->get_properties_size(), 0);
    std::memcpy(params.data() + translationProperty->get_offset(), &translation, sizeof(translation));
    ZoomOutline->process_event(setTranslation, params.data());
    ZoomOutlineMoved = translation.X != 0.0f || translation.Y != 0.0f;
    return true;
}

bool HUD::SetZoomOutlineOpacity(const float opacity) {
    if (!ZoomOutline)
        return false;

    static const auto widgetClass = API::get()->find_uobject<API::UClass>(L"Class /Script/UMG.Widget");
    static const auto setOpacity = widgetClass ? widgetClass->find_function(L"SetRenderOpacity") : nullptr;
    if (!setOpacity)
        return false;

    const auto opacityProperty = setOpacity->find_property(L"InOpacity");
    if (!opacityProperty)
        return false;

    std::vector<uint8_t> params(setOpacity->get_properties_size(), 0);
    std::memcpy(params.data() + opacityProperty->get_offset(), &opacity, sizeof(opacity));
    ZoomOutline->process_event(setOpacity, params.data());
    return true;
}

bool HUD::TryGetWidget3DRenderData(const char* name, WidgetType type, API::UObject* component, const bool logFailures) {
    auto widget = &HUDWidgets[(int)type];

    widget->Type                   = type;
    const auto* primitiveComponent = (UPrimitiveComponent*)component;
    widget->ComponentId            = primitiveComponent->ComponentId;

    if (!FWidget3DSceneProxy::GetDynamicMeshElements.Installed) {
        auto* proxy = (FWidget3DSceneProxy*)primitiveComponent->SceneProxy;
        if (!proxy) {
            if (logFailures)
                LogError("Failed to get %s Widget3DSceneProxy, retrying next frame...", name);
            return false;
        }
        // Using the first Widget3DSceneProxy we find to detour the GetDynamicMeshElements vfunc globally
        FWidget3DSceneProxy::GetDynamicMeshElements.DetourFromInstance(proxy, &FWidget3DSceneProxy_GetDynamicMeshElements);
        // Also detour CanBeOccluded to always return false to avoid hud markers disappearing when occluded
        FWidget3DSceneProxy::CanBeOccluded.DetourFromInstance(proxy, &FWidget3DSceneProxy_CanBeOccluded);
    }

    return true;
}

// This is called when the Widget3DSceneProxy is asked to render
// We use it to capture the MVP matrices of the widgets and flag them for rendering in our injected post-process pass
// The original function is not called or the widgets will be drawn twice - once normally and once in post-processing
void HUD::FWidget3DSceneProxy_GetDynamicMeshElements(FWidget3DSceneProxy*             self,
                                                     const TArray<const FSceneView*>& views,
                                                     const FSceneViewFamily&          viewFamily,
                                                     uint32_t                         visibilityMap,
                                                     void*                            collector) {
    if (!self || views.Count == 0)
        return;

    // Using frameIndex to implement a simple ring buffer to avoid GPU/CPU sync issues
    // Sometimes this can be called multiple times before the post process pass runs so the FrameNumber modulo keeps them in sync
    const uint32_t frameIndex = viewFamily.FrameNumber % 3;

    if (self->BlendMode == EWidgetBlendMode::Opaque || !Instance->InMech[frameIndex]) {
        // Either we are out of mech or this is a screen, render as normal
        FWidget3DSceneProxy::GetDynamicMeshElements.OriginalFn(self, views, viewFamily, visibilityMap, collector);
        return;
    }

    auto&                widgets = Instance->HUDWidgets;
    HUDWidgetRenderData* widget  = nullptr;
    // Find which widget this is - match the render proxy component ID to our stored ones
    for (auto& w : widgets) {
        if (w.ComponentId.PrimIdValue == self->PrimitiveComponentId.PrimIdValue) {
            widget = &w;
            break;
        }
    }

    const auto view         = views.Data[0];
    const bool isZoomCamera = views.Count == 1 && view->bIsSceneCapture;
    const bool isMainPass   = views.Count == 2 || view->StereoPass == eSSP_FULL && !isZoomCamera;

    if (widget) {
        // HUD widget, only render in the main pass
        if (isMainPass)
            ProcessHUDWidget(self, views, frameIndex, widget);
    } else if (isMainPass || isZoomCamera) // Marker widget, only render if it's either the main pass or zoom camera, internal logic will skip as needed
        ProcessMarkerWidget(self, views, isZoomCamera, Instance->ZoomLevel[frameIndex], Instance->TargetBoxId, Instance->MarkerWidgets[frameIndex]);
}

bool HUD::FWidget3DSceneProxy_CanBeOccluded(FWidget3DSceneProxy*) {
    // The widget component has a material that cannot be changed without recreating the component due to unreal bugs
    // Unfortunately the material has depth testing enabled which will cause it to skip rendering if occluded by parts of the cockpit
    // This hook makes the widget component behave as if it has depth testing disabled so it always gets rendered if it passes frustum culling
    return false;
}

void HUD::ProcessHUDWidget(const FWidget3DSceneProxy*       proxy,
                           const TArray<const FSceneView*>& views,
                           const uint32_t                   frameIndex,
                           HUDWidgetRenderData*             widget) {
    widget->Texture = nullptr;
    widget->SRV     = nullptr;

    if (!proxy || !proxy->RenderTarget || !proxy->RenderTarget->Resource || !proxy->RenderTarget->Resource->Texture2DRHI)
        return;

    const ComPtr<ID3D11Texture2D>          tex = proxy->RenderTarget->Resource->Texture2DRHI->GetNativeResource();
    const ComPtr<ID3D11ShaderResourceView> srv = proxy->RenderTarget->Resource->Texture2DRHI->GetNativeShaderResourceView();

    if (!tex || !srv)
        return;

    widget->Texture           = tex;
    widget->SRV               = srv;
    widget->RenderTargetSizeX = proxy->RenderTarget->SizeX;
    widget->RenderTargetSizeY = proxy->RenderTarget->SizeY;


    const auto& m = GetModelMatrix(proxy->LocalToWorld, widget->RenderTargetSizeX, widget->RenderTargetSizeY, 1.0f);
    // Cache the required data for our post-process pass
    for (auto eye = 0; eye < views.Count; eye++) {
        const auto& matrices = views.Data[eye]->ViewMatrices;

        widget->MVPs[frameIndex][eye] = matrices.ProjectionNoAAMatrix *
                                        matrices.TranslatedViewMatrix *
                                        translate(mat4(1), matrices.PreViewTranslation) * m;

        widget->RequestDraw[frameIndex][eye] = true;
    }
}

void HUD::ProcessMarkerWidget(FWidget3DSceneProxy*                 proxy,
                              const TArray<const FSceneView*>&     views,
                              const bool                           isZoomCamera,
                              const float                          zoomLevel,
                              const FPrimitiveComponentId          targetBoxId,
                              std::vector<MarkerWidgetRenderData>& markers) {
    if (zoomLevel > 1.02f && !isZoomCamera || zoomLevel <= 1.02f && isZoomCamera) {
        // If there is no zoom camera, render markers normally
        // If the camera is active and at zoom 1.0 the markers will continue rendering in the main camera
        // Otherwise if the camera is active and zoomed in we skip rendering the normal camera's markers, they will be rendered into the zoom camera overlay instead
        return;
    }

    if (!proxy || !proxy->RenderTarget || !proxy->RenderTarget->Resource || !proxy->RenderTarget->Resource->Texture2DRHI)
        return;

    const ComPtr<ID3D11Texture2D>          tex = proxy->RenderTarget->Resource->Texture2DRHI->GetNativeResource();
    const ComPtr<ID3D11ShaderResourceView> srv = proxy->RenderTarget->Resource->Texture2DRHI->GetNativeShaderResourceView();

    if (!tex || !srv)
        return;

    MarkerWidgetRenderData widget;
    widget.ComponentId       = proxy->PrimitiveComponentId;
    widget.Texture           = tex;
    widget.SRV               = srv;
    widget.RenderTargetSizeX = proxy->RenderTarget->SizeX;
    widget.RenderTargetSizeY = proxy->RenderTarget->SizeY;

    // Ignore zoom level for the target box - scaling this is already handled in blueprints
    const float zoomScale = targetBoxId.PrimIdValue == widget.ComponentId.PrimIdValue ? 1.0f : 1.0f / zoomLevel;

    const auto& m = GetModelMatrix(proxy->LocalToWorld, widget.RenderTargetSizeX, widget.RenderTargetSizeY, zoomScale);
    for (auto eye = 0; eye < views.Count; eye++) {
        const auto& matrices = views.Data[eye]->ViewMatrices;
        widget.MVPs[eye]     = matrices.ProjectionNoAAMatrix *
                               matrices.TranslatedViewMatrix *
                               translate(mat4(1), matrices.PreViewTranslation) * m;
    }

    markers.push_back(widget);
}

mat4 HUD::GetModelMatrix(const mat4& localToWorld, const int32_t sizeX, const int32_t sizeY, const float zoomScale) {
    // This gives the same result as the original function would have done and scales the widget based on scale & size
    const float scaleMult  = (float)sizeX / 2.0f;
    const float scaleRatio = (float)sizeY / (float)sizeX;

    auto scale = vec3{1, 1, 1} * zoomScale;
    scale      *= scaleMult;
    scale.y    *= scaleRatio;

    auto m = localToWorld;
    m      = rotate(m, glm::half_pi<float>(), vec3(0, 1, 0));
    m      = rotate(m, glm::half_pi<float>(), vec3(0, 0, 1));
    m[0]   *= scale.x;
    m[1]   *= scale.y;
    m[2]   *= scale.z;
    return m;
}

void HUD::PostProcessMotionBlur_AddMotionBlurVelocityPass(FRDGBuilder*,
                                                          const FViewInfo&,
                                                          const FMotionBlurViewports&,
                                                          FRDGTexture*,
                                                          FRDGTexture*,
                                                          FRDGTexture*,
                                                          FRDGTexture**,
                                                          FRDGTexture**) {
    // Skip this, it's part of motion blur which we don't want to use
}

void HUD::FMotionBlurFilterPS_TRDGLambdaPass_ExecuteImpl(FMotionBlurFilterPS::TRDGLambdaPass* self, FRHICommandList*) {
    const auto& lambda       = self->ExecuteLambda;
    const auto  view         = lambda.View;
    const auto& params       = lambda.PixelShaderParameters;
    const auto  pass         = view->StereoPass;
    const auto  frameNumber  = lambda.View->Family->FrameNumber;
    const auto& viewport     = params->Filter.Color;
    const auto  sceneColor   = params->Filter.ColorTexture;
    const auto& renderTarget = params->RenderTargets.Outputs[0].Texture;
    const float zoomLevel    = Instance->ZoomLevel[frameNumber % 3];
    const bool  inMech       = Instance->InMech[frameNumber % 3] && Instance->CurrentlyInMech;
    Instance->Renderer->RenderHUD(Instance->HUDWidgets, Instance->MarkerWidgets, Instance->CurrentBrightness, zoomLevel, pass, frameNumber, viewport, sceneColor, renderTarget,
                                  inMech);
}

bool HUD::ValidateMech() {
    if (const auto activePawn = API::get()->get_local_pawn(0)) {
        if (activePawn != Pawn)
            return OnNewPawn(activePawn);
        return CurrentlyInMech;
    }

    // We have no active pawn, reset state if we had one before
    if (Pawn != nullptr)
        Reset();

    return false;
}

// ReSharper disable once CppInconsistentNaming
std::unique_ptr<HUD> g_plugin{new HUD()}; // NOLINT(misc-use-internal-linkage)
