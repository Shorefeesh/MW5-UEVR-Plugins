#include <PluginExtension.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>

class HeadAim final : public PluginExtension {
    enum class HeadAimMode : uint8_t {
        Disabled, ArmsOnly, TorsoAndArms
    };

    API::UObject* Pawn                   = nullptr;
    HeadAimMode*  HeadAimMode            = nullptr;
    vec2*         ArmTwist               = nullptr;
    vec3*         LastViewRotator        = nullptr;
    bool*         ArmlockEnabled         = nullptr;
    bool*         EnableArmLock          = nullptr;
    float*        ZoomLevel              = nullptr;
    float*        ZoomCameraFOV          = nullptr;
    float*        HeadAimPitchOffset     = nullptr;
    vec2*         TorsoRotation          = nullptr;
    vec3*         MechRelativeRotation   = nullptr;
    vec2*         HeadTarget             = nullptr;
    vec2*         ArmsTarget             = nullptr;
    bool*         HeadAimLocked          = nullptr;
    void**        CurrentTarget          = nullptr;
    void**        PreviousFriendlyTarget = nullptr;
    void**        PreviousHostileTarget  = nullptr;
    void**        PendingLockTarget      = nullptr;
    void**        LockedOnTargetOverride = nullptr;
    API::UObject* LockOnComponent        = nullptr;

    bool  InMech                = false;
    bool  HooksInstalled        = false;
    float Delta                 = 0;
    vec2  RotationSpeed         = vec2(0, 0);
    vec2  PreviousTorsoRotation = vec2(0, 0);

#define ARM_SPRING_CONSTANT 200.0f
#define ARM_DAMPING_CONSTANT 32.0f

    vec2 CurrentArmVelocity{};
    vec2 CurrentArmRotation{};
    quat SmoothedHmdQuat  = quat(1, 0, 0, 0);
    bool SmoothedQuatInit = false;
    quat SmoothedGazeQuat  = quat(1, 0, 0, 0);
    bool SmoothedGazeQuatInit = false;
    quat ZoomCenterQuat = quat(1, 0, 0, 0);
    bool ZoomCenterInit = false;
    bool ZoomEdgePanning = false;
    vec2 EffectiveArmsTarget{};

    struct TwistBounds {
        // Yaw, Pitch
        vec2 TwistRate;
        bool IsYawUnbound;
        bool IsPitchUnbound;
        vec2 BoundsLow;
        vec2 BoundsHigh;
    };

    struct TorsoStats {
        TwistBounds Torso;
        TwistBounds Arm;
    };

    TorsoStats* TorsoStats = nullptr;

public:
    inline static HeadAim* Instance;

    HeadAim() {
        Instance                  = this;
        PluginExtension::Instance = this;
        Name                      = "HeadAim";
        Version                   = "2.6.12";
        VersionInt                = 2612;
        VersionCheckFnName        = L"OnFetchHeadAimPluginData";
        VersionPropertyName       = L"HeadAimVersion";
    }

    virtual void on_pre_engine_tick(API::UGameEngine* engine, const float delta) override {
        if (const auto activePawn = API::get()->get_local_pawn(0); activePawn != Pawn) {
            SmoothedQuatInit = false;
            SmoothedGazeQuatInit = false;
            ZoomCenterInit = false;
            ZoomEdgePanning = false;
            InMech = OnNewPawn(activePawn);
        }

        this->Delta = delta;

        if (InMech && *HeadAimMode == HeadAimMode::ArmsOnly) {
            // LastRelativeViewRotator is used by the targeting logic, setting this here doesn't seem to affect anything else but targeting
            *LastViewRotator = vec3(HeadTarget->y + TorsoRotation->y, HeadTarget->x + TorsoRotation->x, 0.0f);

            // Force disable arm lock, it's being handled manually
            *ArmlockEnabled = false;
            *EnableArmLock  = true;
        }
    }

    virtual void OnInitialize() override {
        const auto targetLockHandler = API::get()->find_uobject<API::UClass>(L"BlueprintGeneratedClass /Game/MechWarriorVR/VR_TargetLockHandler.VR_TargetLockHandler_C");
        if (!targetLockHandler) {
            LogError("Failed to find VR Target Lock Handler class");
            return;
        }

        AddEventHook(targetLockHandler, L"OnForceLookingAtTarget", &OnForceLookingAtTarget);
    }

    struct RotationDegrees {
        float Roll;
        float Pitch;
        float Yaw;
    };

    struct OnCalculateHeadAimParams {
        RotationDegrees Cockpit;
        RotationDegrees Torso;
    };

    static void* OnCalculateHeadAim(API::UObject*, FFrame* frame, void* const) {
        if (*Instance->HeadAimMode == HeadAimMode::Disabled)
            return nullptr;
        const auto rotations = *frame->GetParams<OnCalculateHeadAimParams>();
        Instance->ProcessHeadAim(rotations.Torso);
        Instance->ProcessArmTwist(rotations.Cockpit, rotations.Torso);
        return nullptr;
    }

    static void* OnSetTarget(API::UObject*, FFrame* frame, void* const) {
        *Instance->CurrentTarget          = *frame->GetParams<void**>();
        *Instance->PreviousFriendlyTarget = nullptr;
        *Instance->PreviousHostileTarget  = nullptr;
        *Instance->PendingLockTarget      = nullptr;
        *Instance->LockedOnTargetOverride = nullptr;
        return nullptr;
    }

    struct OnForceLookingAtTargetParams {
        API::UObject* LockOnComponent;
        bool          LookingAtTarget;
    };

    static void* OnForceLookingAtTarget(API::UObject*, FFrame* frame, void* const) {
        const auto params          = *frame->GetParams<OnForceLookingAtTargetParams>();
        bool       lookingAtTarget = params.LookingAtTarget;
        params.LockOnComponent->call_function(L"OnLookingAtTargetChanged", &lookingAtTarget);
        return nullptr;
    }

private:
    bool OnNewPawn(API::UObject* activePawn) {
        Pawn = activePawn;
        if (!activePawn)
            return false;

        API::UObject* mechViewC,* mechMeshC,* cockpitC,* torsoTwistC,* mechCockpit,* userSettings,* vrHUDManager,* mechRootC,* targetTrackingC,* zoomCamera;

        const auto playerController = API::get()->get_player_controller(0);

        bool success = TryGetProperty(Pawn, L"MechViewComponent", mechViewC) && TryGetProperty(Pawn, L"MechMeshComponent", mechMeshC);

        if (!success) {
            LogInfo("Failed to get Mech references - probably not in a Mech");
            return false;
        }

        success = TryGetProperty(mechMeshC, L"FirstPersonCockpitComponent", cockpitC) &&
                  TryGetProperty(cockpitC, L"ChildActor", mechCockpit) &&
                  TryGetPropertyStruct(mechCockpit, L"HeadAimMode", HeadAimMode) &&
                  TryGetProperty(Pawn, L"TorsoTwistComponent", torsoTwistC) &&
                  TryGetPropertyStruct(torsoTwistC, L"bArmlockEnabled", ArmlockEnabled) &&
                  TryGetPropertyStruct(torsoTwistC, L"ArmTwist", ArmTwist) &&
                  TryGetPropertyStruct(mechViewC, L"LastRelativeViewRotator", LastViewRotator) &&
                  TryGetPropertyStruct(torsoTwistC, L"TorsoStats", TorsoStats) &&
                  TryGetProperty(playerController, L"MWGameUserSettings", userSettings) &&
                  TryGetPropertyStruct(userSettings, L"EnableArmLock", EnableArmLock) &&
                  TryGetProperty(mechCockpit, L"VR_HUDManager", vrHUDManager) &&
                  TryGetPropertyStruct(vrHUDManager, L"ZoomLevel", ZoomLevel) &&
                  TryGetProperty(mechCockpit, L"VR_NormalCamera", zoomCamera) &&
                  TryGetPropertyStruct(zoomCamera, L"FOVAngle", ZoomCameraFOV) &&
                  TryGetPropertyStruct(torsoTwistC, L"TorsoTwist", TorsoRotation) &&
                  TryGetProperty(Pawn, L"RootComponent", mechRootC) &&
                  TryGetPropertyStruct(mechRootC, L"RelativeRotation", MechRelativeRotation) &&
                  TryGetPropertyStruct(mechCockpit, L"HeadAimPitchOffset", HeadAimPitchOffset) &&
                  TryGetPropertyStruct(mechCockpit, L"HeadTarget", HeadTarget) &&
                  TryGetPropertyStruct(mechCockpit, L"ArmsTarget", ArmsTarget) &&
                  TryGetPropertyStruct(mechCockpit, L"HeadAimLocked", HeadAimLocked) &&
                  TryGetProperty(Pawn, L"TargetTracking", targetTrackingC) &&
                  TryGetPropertyStruct(targetTrackingC, L"CurrentTarget", CurrentTarget) &&
                  TryGetPropertyStruct(targetTrackingC, L"PreviousFriendlyTarget", PreviousFriendlyTarget) &&
                  TryGetPropertyStruct(targetTrackingC, L"PreviousHostileTarget", PreviousHostileTarget) &&
                  TryGetProperty(Pawn, L"LockOnComponent", LockOnComponent) &&
                  TryGetPropertyStruct(LockOnComponent, L"PendingLockTarget", PendingLockTarget) &&
                  TryGetPropertyStruct(LockOnComponent, L"LockedOnTargetOverride", LockedOnTargetOverride);

        if (!success) {
            LogInfo("Failed to get cockpit references, retrying next frame...");
            Pawn = nullptr;
            return false;
        }

        if (!HooksInstalled) {
            if (!AddEventHook(mechCockpit->get_class(), L"OnCalculateHeadAim", &OnCalculateHeadAim))
                return false;

            if (!AddEventHook(mechCockpit->get_class(), L"OnSetTarget", &OnSetTarget))
                return false;

            HooksInstalled = true;
        }

        LogInfo("Mech references found and hooks enabled");
        return true;
    }

    void ProcessHeadAim(const RotationDegrees torsoAimRotation) {
        constexpr auto  fwd         = vec3(1, 0, 0);
        constexpr float deadbandRad = radians(0.15f); // 0.10–0.25
        constexpr float maxAngleRad = radians(3.0f);  // 3–5
        constexpr float lambdaSlow  = 20.0f;          // 14–20
        constexpr float lambdaFast  = 50.0f;          // 35–50

        quat q;
        vec3 pose;
        GetHMDPoseAndRotation(q, pose);
        const quat qHeadAimOffset = angleAxis(radians(*HeadAimPitchOffset), vec3(0, 1, 0));
        q                         = normalize(q * qHeadAimOffset);

        if (!SmoothedQuatInit) {
            SmoothedHmdQuat  = q;
            SmoothedQuatInit = true;
        }

        const float ang      = QuatAngleRad(SmoothedHmdQuat, q);
        const float angEff   = max(0.0f, ang - deadbandRad);
        const float adaptive = clamp(angEff / maxAngleRad, 0.0f, 1.0f);
        const float lambda   = mix(lambdaSlow, lambdaFast, adaptive);
        const float t        = 1.0f - expf(-lambda * Delta);
        SmoothedHmdQuat      = normalize(slerp(SmoothedHmdQuat, q, t));

        const vec3 headDir = normalize(SmoothedHmdQuat * fwd);

        vec3 cameraAimDirection = headDir;
        vec3 weaponAimDirection = headDir;
        vec3 headAimDirection = headDir;
        UEVR_Vector3f gazePose{};
        UEVR_Quaternionf gazeRotation{};
        const auto apiVersion = API::get()->param()->version;
        const auto openxr = API::get()->param()->openxr;
        const bool hasEyeGazeApi = apiVersion != nullptr && apiVersion->major == 2 && apiVersion->minor >= 40;
        const bool hasGaze = hasEyeGazeApi && API::VR::is_openxr() && openxr != nullptr && openxr->get_eye_gaze_pose != nullptr &&
                             openxr->get_eye_gaze_pose(&gazePose, &gazeRotation);

        if (hasGaze) {
            const auto rotationOffset = API::VR::get_rotation_offset();
            const quat qGaze(gazeRotation.w, gazeRotation.z, gazeRotation.x, gazeRotation.y);
            const quat qRotationOffset(rotationOffset.w, rotationOffset.z, rotationOffset.x, rotationOffset.y);
            const float gazeYawCalibrationDeg = clamp(API::VR::get_mod_value<float>("HeadAim_GazeYawOffset"), -20.0f, 20.0f);
            const float gazePitchCalibrationDeg = clamp(API::VR::get_mod_value<float>("HeadAim_GazePitchOffset"), -20.0f, 20.0f);
            const quat qGazeCalibration = MakeYawPitchRollQuat(
                -radians(gazeYawCalibrationDeg), radians(gazePitchCalibrationDeg), 0.0f);
            const quat gazeTarget = normalize(qRotationOffset * qGaze * qGazeCalibration * qHeadAimOffset);

            if (!SmoothedGazeQuatInit) {
                SmoothedGazeQuat = gazeTarget;
                SmoothedGazeQuatInit = true;
            }

            const bool zoomActive = ZoomLevel && ZoomCameraFOV && *ZoomLevel > 1.02f;
            if (zoomActive) {
                if (!ZoomCenterInit) {
                    ZoomCenterQuat = SmoothedGazeQuat;
                    ZoomCenterInit = true;
                    ZoomEdgePanning = false;
                }

                const vec3 rawGazeDirection = normalize(gazeTarget * fwd);
                const float captureHalfFOV = radians(clamp(*ZoomCameraFOV, 1.0f, 170.0f) * 0.5f);
                const float displayHalfFOV = radians(clamp(*ZoomCameraFOV * *ZoomLevel, 1.0f, 170.0f) * 0.5f);
                constexpr float zoomAspect = 640.0f / 430.0f;
                const float captureTanX = std::tan(captureHalfFOV);
                const float captureTanY = captureTanX / zoomAspect;
                const float displayTanX = std::tan(displayHalfFOV);
                const float displayTanY = displayTanX / zoomAspect;

                auto gazeUV = [&]() {
                    const vec3 localGaze = inverse(ZoomCenterQuat) * rawGazeDirection;
                    const float forward = max(0.001f, localGaze.x);
                    return vec2(localGaze.y / (forward * displayTanX),
                                localGaze.z / (forward * displayTanY));
                };

                // The torso center is a gaze snap target, independent of the zoom
                // window's current position. Snap only while gaze is within 1.5
                // degrees of it. As soon as gaze leaves that cone, resume the same
                // edge-panning controller used everywhere else.
                constexpr float centerLookSnapAngle = radians(1.5f);
                bool lookingAtTorsoCenter = false;
                {
                    // TorsoAim is the small reticle offset from the cockpit after the
                    // component hierarchy has already applied TorsoRotation. Negating
                    // its yaw and pitch produces the ArmsTarget-space torso direction
                    // without applying the physical torso twist a second time.
                    const float torsoTargetYaw = -torsoAimRotation.Yaw;
                    const float torsoTargetPitch = -torsoAimRotation.Pitch;
                    const quat torsoAimQuat = MakeYawPitchRollQuat(
                        -radians(torsoTargetYaw), radians(torsoTargetPitch), 0.0f);
                    const vec3 torsoAimDirection = normalize(torsoAimQuat * fwd);
                    const float gazeToTorsoAngle = acos(clamp(dot(rawGazeDirection, torsoAimDirection), -1.0f, 1.0f));
                    lookingAtTorsoCenter = gazeToTorsoAngle <= centerLookSnapAngle;
                    if (lookingAtTorsoCenter) {
                        const vec3 zoomCenterDirection = normalize(ZoomCenterQuat * fwd);
                        const float d = clamp(dot(zoomCenterDirection, torsoAimDirection), -1.0f, 1.0f);
                        const vec3 axis = cross(zoomCenterDirection, torsoAimDirection);
                        if (d > -0.9999f) {
                            const quat toTorso = normalize(quat(1.0f + d, axis.x, axis.y, axis.z));
                            ZoomCenterQuat = normalize(toTorso * ZoomCenterQuat);
                        }
                        ZoomEdgePanning = false;
                    }
                }

                vec2 uv = gazeUV();
                const float edge = max(std::abs(uv.x), std::abs(uv.y));
                if (!lookingAtTorsoCenter) {
                    if (!ZoomEdgePanning && edge >= 0.85f)
                        ZoomEdgePanning = true;
                    else if (ZoomEdgePanning && edge <= 0.65f)
                        ZoomEdgePanning = false;
                }

                if (ZoomEdgePanning) {
                    constexpr float panLambda = 8.0f;
                    const float panT = 1.0f - expf(-panLambda * Delta);
                    ZoomCenterQuat = normalize(slerp(ZoomCenterQuat, gazeTarget, panT));
                    uv = gazeUV();
                }

                // The gaze ray intersects the magnified display at uv. Unproject
                // that same point through the capture camera to recover its world ray.
                uv = clamp(uv, vec2(-1.25f), vec2(1.25f));
                const vec3 captureLocalDirection = normalize(vec3(1.0f,
                                                                  uv.x * captureTanX,
                                                                  uv.y * captureTanY));
                cameraAimDirection = normalize(ZoomCenterQuat * fwd);
                weaponAimDirection = normalize(ZoomCenterQuat * captureLocalDirection);
                headAimDirection = weaponAimDirection;
                SmoothedGazeQuat = ZoomCenterQuat;
            } else {
                ZoomCenterInit = false;
                ZoomEdgePanning = false;

                const float gazeAngle = QuatAngleRad(SmoothedGazeQuat, gazeTarget);
                const float gazeEffectiveAngle = max(0.0f, gazeAngle - deadbandRad);
                const float gazeAdaptive = clamp(gazeEffectiveAngle / maxAngleRad, 0.0f, 1.0f);
                const float gazeLambda = mix(lambdaSlow, lambdaFast, gazeAdaptive);
                const float gazeT = 1.0f - expf(-gazeLambda * Delta);
                SmoothedGazeQuat = normalize(slerp(SmoothedGazeQuat, gazeTarget, gazeT));
                cameraAimDirection = normalize(SmoothedGazeQuat * fwd);
                weaponAimDirection = cameraAimDirection;
                headAimDirection = cameraAimDirection;
            }
        } else {
            SmoothedGazeQuatInit = false;
            ZoomCenterInit = false;
            ZoomEdgePanning = false;
        }

        auto directionToTarget = [&](const vec3& direction) {
            const float yaw = -degrees(atan2(direction.y, direction.x));
            const float pitch = -degrees(atan2(direction.z, sqrt(direction.x * direction.x + direction.y * direction.y)));
            vec2 target = *HeadAimLocked ? vec2(0.0f) : vec2(yaw, pitch);
            if (!TorsoStats->Arm.IsYawUnbound)
                target.x = clamp(target.x, TorsoStats->Arm.BoundsLow.x, TorsoStats->Arm.BoundsHigh.x);
            if (!TorsoStats->Arm.IsPitchUnbound)
                target.y = clamp(target.y, TorsoStats->Arm.BoundsLow.y, TorsoStats->Arm.BoundsHigh.y);
            return target;
        };

        *ArmsTarget = directionToTarget(cameraAimDirection);
        EffectiveArmsTarget = directionToTarget(weaponAimDirection);
        const float headYaw = -degrees(atan2(headAimDirection.y, headAimDirection.x));
        const float headPitch = -degrees(atan2(headAimDirection.z,
                                               sqrt(headAimDirection.x * headAimDirection.x +
                                                    headAimDirection.y * headAimDirection.y)));
        *HeadTarget = *HeadAimLocked ? vec2(0.0f) : vec2(headYaw, headPitch);
    }

    void ProcessArmTwist(RotationDegrees cockpitRelativeRot, RotationDegrees torsoAimRotation) {
        // Calculate the target in cockpit relative coordinates so that it's stable relative to the cockpit regardless of torso twist or pitch,
        // otherwise the target will have to chase the torso as it rotates
        // The zoom camera now follows ArmsTarget, so gameplay aim must use the full
        // eye-directed angle. Dividing by zoom was compensation for the old fixed,
        // torso-centred zoom window and caused weapons to fire at 1/zoom.
        vec2 targetArmRotation = EffectiveArmsTarget;

        static bool springInit = false;
        if (!springInit) {
            CurrentArmRotation = targetArmRotation;
            CurrentArmVelocity = vec2(0.0f);
            springInit         = true;
        }

        // Calculate velocity and new rotation
        const vec2 diff      = DeltaAngleDeg2(CurrentArmRotation, targetArmRotation);
        vec2       accel     = ARM_SPRING_CONSTANT * diff - ARM_DAMPING_CONSTANT * CurrentArmVelocity;
        CurrentArmVelocity   += accel * Delta;
        const float maxSpeed = TorsoStats->Arm.TwistRate.x; // Just assuming x==y
        float       v2       = dot(CurrentArmVelocity, CurrentArmVelocity);
        if (v2 > maxSpeed * maxSpeed) {
            CurrentArmVelocity *= maxSpeed / sqrt(v2);
        }
        CurrentArmRotation += CurrentArmVelocity * Delta;

        // Calculate aim direction
        constexpr vec3 fwd(1.0f, 0.0f, 0.0f);
        const quat     qArm = MakeYawPitchRollQuat(
            -radians(CurrentArmRotation.x),
            radians(CurrentArmRotation.y),
            0.0f
        );
        const vec3 localArmTwistDir = qArm * fwd;

        const vec2 torsoDelta = *TorsoRotation - PreviousTorsoRotation;

        float yawOffset;
        float pitchOffset;

        // Kind of hacky below but this makes sure we line up with the torso aim even when the torso target is off center to the cockpit
        // torsoAimRotation is the difference between the cockpit/camera angle and the torso crosshair/aim angle
        if (*HeadAimLocked) {
            yawOffset   = torsoAimRotation.Yaw;
            pitchOffset = -torsoAimRotation.Pitch;
        } else {
            // Apply the current frame's torso motion without fixed-window zoom
            // compensation. The moving camera and target share the same direction.
            yawOffset   = -torsoDelta.x;
            pitchOffset = torsoDelta.y;
        }

        // CockpitRelativeRot is the rotation of the cockpit relative to the mech
        // Add this to the offsets and create a new quaternion
        const float relativeYawRad   = radians(-cockpitRelativeRot.Yaw + yawOffset);
        const float relativePitchRad = radians(cockpitRelativeRot.Pitch + pitchOffset);
        const float relativeRollRad  = radians(-cockpitRelativeRot.Roll);
        const quat  qRelative        = MakeYawPitchRollQuat(relativeYawRad, relativePitchRad, relativeRollRad);

        // Rotate the arm twist direction by the relative quaternion
        const vec3 worldArmTwistDir = qRelative * localArmTwistDir;

        // Construct the yaw and pitch angles from the worldArmTwistDir
        const float yawRad   = atan2(worldArmTwistDir.y, worldArmTwistDir.x);
        const float xyLen    = sqrt(worldArmTwistDir.x * worldArmTwistDir.x + worldArmTwistDir.y * worldArmTwistDir.y);
        const float pitchRad = atan2(worldArmTwistDir.z, xyLen);
        *ArmTwist            = vec2(
            -degrees(yawRad),
            -degrees(pitchRad)
        );

        // Update the previous torso rotation, used for applying the current frames' rotation to the calcs before the skeleton has been updated
        PreviousTorsoRotation = *TorsoRotation;
    }
};

// ReSharper disable once CppInconsistentNaming
std::unique_ptr<HeadAim> g_plugin{new HeadAim()}; // NOLINT(misc-use-internal-linkage)
