#include <PluginExtension.h>

#include <atomic>

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
    float*        HeadAimPitchOffset     = nullptr;
    vec2*         TorsoRotation          = nullptr;
    vec2*         NativeTorsoInput       = nullptr;
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
#define ARM_DAMPING_CONSTANT 25.0f

    vec2 CurrentArmVelocity{};
    vec2 CurrentArmRotation{};
    quat SmoothedHmdQuat  = quat(1, 0, 0, 0);
    bool SmoothedQuatInit = false;
    quat SmoothedGazeQuat  = quat(1, 0, 0, 0);
    bool SmoothedGazeQuatInit = false;
    std::atomic<float> TorsoYawInput{0.0f};
    std::atomic<float> TorsoPitchInput{0.0f};
    std::atomic<float> ViewYawCompensation{0.0f};
    std::atomic<float> ViewPitchCompensation{0.0f};
    std::atomic<float> ViewPitchOffset{0.0f};
    std::atomic<float> LocomotionReferenceYaw{0.0f};
    uint8_t LastLoggedMode = 0xff;

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
        Version                   = "2.13.2";
        VersionInt                = 2132;
        VersionCheckFnName        = L"OnFetchHeadAimPluginData";
        VersionPropertyName       = L"HeadAimVersion";
    }

    virtual void on_pre_engine_tick(API::UGameEngine* engine, const float delta) override {
        if (const auto activePawn = API::get()->get_local_pawn(0); activePawn != Pawn) {
            InMech = OnNewPawn(activePawn);
        }

        this->Delta = delta;

        // This installation always uses the custom control scheme. The mod's enum is not exposed
        // consistently in its UI, so select its non-disabled arms-only state automatically.
        if (InMech && HeadAimMode && *HeadAimMode == HeadAimMode::Disabled)
            *HeadAimMode = HeadAimMode::ArmsOnly;

        if (InMech && HeadAimMode && static_cast<uint8_t>(*HeadAimMode) != LastLoggedMode) {
            LastLoggedMode = static_cast<uint8_t>(*HeadAimMode);
            LogInfo("Head aim mode changed to %u", static_cast<unsigned>(*HeadAimMode));
        }

        if (InMech && *HeadAimMode == HeadAimMode::ArmsOnly) {
            // LastRelativeViewRotator is used by the targeting logic, setting this here doesn't seem to affect anything else but targeting
            *LastViewRotator = vec3(HeadTarget->y + TorsoRotation->y, HeadTarget->x + TorsoRotation->x, 0.0f);

            // Force disable arm lock, it's being handled manually
            *ArmlockEnabled = false;
            *EnableArmLock  = true;
        }
    }

    virtual void on_xinput_get_state(uint32_t* retval, uint32_t userIndex, XINPUT_STATE* state) override {
        if (!retval || *retval != ERROR_SUCCESS || !state || userIndex != 0 || !IsHeadAimEnabled())
            return;

        const SHORT physicalLeftX = state->Gamepad.sThumbLX;
        const SHORT physicalLeftY = state->Gamepad.sThumbLY;
        const SHORT physicalRightX = state->Gamepad.sThumbRX;

        if (API::VR::get_mod_value<bool>("HeadAim_HeadRelativeLeftStick")) {
            const auto thumbToFloat = [](const SHORT value) {
                return clamp(static_cast<float>(value) / 32767.0f, -1.0f, 1.0f);
            };
            const auto floatToThumb = [](const float value) {
                return static_cast<SHORT>(clamp(value, -1.0f, 1.0f) * 32767.0f);
            };

            const float x = thumbToFloat(physicalLeftX);
            const float y = thumbToFloat(physicalLeftY);
            const float yaw = radians(LocomotionReferenceYaw.load(std::memory_order_relaxed));
            const float c = cos(yaw);
            const float s = sin(yaw);
            state->Gamepad.sThumbLY = floatToThumb(-s * x + c * y);
        }

        // The transformed lateral component is intentionally discarded. Legs are controlled only
        // by physical right-stick X through MW5's existing left-stick leg-turn channel.
        state->Gamepad.sThumbLX = physicalRightX;

        const auto controllerToThumb = [](const float input) -> SHORT {
            if (abs(input) < 0.0001f)
                return 0;
            constexpr float deadzone = static_cast<float>(XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE) / 32767.0f;
            const float stick = sign(input) * (deadzone + (1.0f - deadzone) * abs(input));
            return static_cast<SHORT>(clamp(stick, -1.0f, 1.0f) * 32767.0f);
        };
        state->Gamepad.sThumbRX = controllerToThumb(TorsoYawInput.load(std::memory_order_relaxed));
        state->Gamepad.sThumbRY = controllerToThumb(TorsoPitchInput.load(std::memory_order_relaxed));
    }

    virtual void on_pre_calculate_stereo_view_offset(UEVR_StereoRenderingDeviceHandle, int, float,
                                                      UEVR_Vector3f*, UEVR_Rotatorf* rotation, bool isDouble) override {
        if (!rotation || !IsHeadAimEnabled())
            return;

        // Blend toward torso-lag compensation through the expanded yaw range. Both stereo eyes
        // receive exactly the same render-only rotation; tracking poses remain unmodified.
        const float yawCompensation = ViewYawCompensation.load(std::memory_order_relaxed);
        const float pitchAdjustment = ViewPitchCompensation.load(std::memory_order_relaxed) +
                                      ViewPitchOffset.load(std::memory_order_relaxed);
        if (isDouble) {
            auto* rotationDouble = reinterpret_cast<UEVR_Rotatord*>(rotation);
            rotationDouble->yaw += static_cast<double>(yawCompensation);
            rotationDouble->pitch += static_cast<double>(pitchAdjustment);
        } else {
            rotation->yaw += yawCompensation;
            rotation->pitch += pitchAdjustment;
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
        Instance->ProcessHeadAim();
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
        TorsoYawInput.store(0.0f, std::memory_order_relaxed);
        TorsoPitchInput.store(0.0f, std::memory_order_relaxed);
        ViewYawCompensation.store(0.0f, std::memory_order_relaxed);
        ViewPitchCompensation.store(0.0f, std::memory_order_relaxed);
        ViewPitchOffset.store(0.0f, std::memory_order_relaxed);
        LocomotionReferenceYaw.store(0.0f, std::memory_order_relaxed);
        LastLoggedMode = 0xff;
        SmoothedQuatInit = false;
        SmoothedGazeQuatInit = false;
        if (!activePawn)
            return false;

        API::UObject* mechViewC,* mechMeshC,* cockpitC,* torsoTwistC,* mechCockpit,* userSettings,* vrHUDManager,* mechRootC,* targetTrackingC;

        const auto playerController = API::get()->get_player_controller(0);

        bool success = TryGetProperty(Pawn, L"MechViewComponent", mechViewC) &&
                       TryGetProperty(Pawn, L"MechMeshComponent", mechMeshC);

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
                  TryGetPropertyStruct(torsoTwistC, L"TorsoTwist", TorsoRotation) &&
                  TryGetPropertyStruct(torsoTwistC, L"TorsoInput", NativeTorsoInput) &&
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

    void ProcessHeadAim() {
        constexpr auto  fwd         = vec3(1, 0, 0);
        constexpr float deadbandRad = radians(0.15f); // 0.10–0.25
        constexpr float maxAngleRad = radians(3.0f);  // 3–5
        constexpr float lambdaSlow  = 20.0f;          // 14–20
        constexpr float lambdaFast  = 50.0f;          // 35–50

        quat q;
        vec3 pose;
        GetHMDPoseAndRotation(q, pose);
        const quat qHeadAimOffset = angleAxis(radians(*HeadAimPitchOffset), vec3(0, 1, 0));
        q = normalize(q);

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

        const float headYawRad   = atan2(headDir.y, headDir.x);
        const float headPitchRad = atan2(headDir.z, sqrt(headDir.x * headDir.x + headDir.y * headDir.y));
        const float headYawDeg   = -degrees(headYawRad);
        const float headPitchDeg = -degrees(headPitchRad);

        const float viewPitchOffsetDeg = clamp(API::VR::get_mod_value<float>("HeadAim_TorsoPitchOffset"), -30.0f, 30.0f);
        const float desiredTorsoYawDeg = MapHeadYawToTorso(headYawDeg);
        const float desiredCockpitYawDeg = MapHeadYawToCockpitView(headYawDeg);
        const float desiredTorsoPitchDeg = MapHeadPitchToTorso(headPitchDeg);
        const float desiredCockpitPitchDeg = MapHeadPitchToCockpitView(headPitchDeg);
        LocomotionReferenceYaw.store(headYawDeg, std::memory_order_relaxed);
        ViewPitchOffset.store(viewPitchOffsetDeg, std::memory_order_relaxed);
        UpdateTorsoController(desiredTorsoYawDeg, desiredTorsoPitchDeg,
                              desiredCockpitYawDeg, desiredCockpitPitchDeg,
                              headYawDeg, headPitchDeg);

        quat aimQuat = normalize(SmoothedHmdQuat * qHeadAimOffset);
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
            // The mod's HeadAimPitchOffset calibrates head fallback only. Eye tracking has its own
            // explicit yaw/pitch calibration and must not inherit the legacy head-aim offset.
            const quat gazeTarget = normalize(qRotationOffset * qGaze * qGazeCalibration);

            if (!SmoothedGazeQuatInit) {
                SmoothedGazeQuat = gazeTarget;
                SmoothedGazeQuatInit = true;
            }

            const float gazeAngle = QuatAngleRad(SmoothedGazeQuat, gazeTarget);
            const float gazeEffectiveAngle = max(0.0f, gazeAngle - deadbandRad);
            const float gazeAdaptive = clamp(gazeEffectiveAngle / maxAngleRad, 0.0f, 1.0f);
            const float gazeLambda = mix(lambdaSlow, lambdaFast, gazeAdaptive);
            const float gazeT = 1.0f - expf(-gazeLambda * Delta);
            SmoothedGazeQuat = normalize(slerp(SmoothedGazeQuat, gazeTarget, gazeT));
            aimQuat = SmoothedGazeQuat;
        } else {
            SmoothedGazeQuatInit = false;
        }

        const vec3 dir = normalize(aimQuat * fwd);

        const float yawRad   = atan2(dir.y, dir.x);
        const float pitchRad = atan2(dir.z, sqrt(dir.x * dir.x + dir.y * dir.y));

        const float yawDeg   = -degrees(yawRad);
        const float pitchDeg = -degrees(pitchRad);

        // Apply the same render-only offsets to gaze so the reticle remains aligned with the
        // visibly shifted stereo view. The underlying OpenXR gaze pose remains untouched.
        const float aimTargetYawDeg = yawDeg + ViewYawCompensation.load(std::memory_order_relaxed);
        const float aimTargetPitchDeg = pitchDeg + viewPitchOffsetDeg +
                                        ViewPitchCompensation.load(std::memory_order_relaxed);
        float armsTargetYawDeg   = *HeadAimLocked ? 0 : aimTargetYawDeg;
        float armsTargetPitchDeg = *HeadAimLocked ? 0 : aimTargetPitchDeg;

        if (!TorsoStats->Arm.IsYawUnbound)
            armsTargetYawDeg = clamp(armsTargetYawDeg, TorsoStats->Arm.BoundsLow.x, TorsoStats->Arm.BoundsHigh.x);
        if (!TorsoStats->Arm.IsPitchUnbound)
            armsTargetPitchDeg = clamp(armsTargetPitchDeg, TorsoStats->Arm.BoundsLow.y, TorsoStats->Arm.BoundsHigh.y);

        *ArmsTarget = vec2(armsTargetYawDeg, armsTargetPitchDeg);

        // HeadTarget is the target for torso-mounted weapons, not the torso's physical rotation.
        // Keep both weapon groups eye-driven (or head-driven when gaze is unavailable).
        *HeadTarget = vec2(aimTargetYawDeg, aimTargetPitchDeg);
    }

    float MapHeadYawToTorso(const float headYawDeg) const {
        constexpr float deadzone = 1.0f;
        constexpr float innerRange = 15.0f;
        constexpr float outerRange = 75.0f;
        const float magnitude = max(0.0f, abs(headYawDeg) - deadzone);
        if (magnitude == 0.0f)
            return 0.0f;

        const float direction = headYawDeg < 0.0f ? -1.0f : 1.0f;
        const float limit = TorsoStats->Torso.IsYawUnbound
            ? 180.0f
            : (direction < 0.0f ? abs(TorsoStats->Torso.BoundsLow.x) : abs(TorsoStats->Torso.BoundsHigh.x));
        if (limit <= innerRange)
            return direction * min(magnitude, limit);
        if (magnitude <= innerRange)
            return direction * magnitude;

        const float outerAmount = min((magnitude - innerRange) / (outerRange - innerRange), 1.0f);
        return direction * mix(innerRange, limit, outerAmount);
    }

    static float MapHeadYawToCockpitView(const float headYawDeg) {
        constexpr float innerRange = 15.0f;
        constexpr float outerRange = 75.0f;
        constexpr float cockpitLimitAtOuterRange = 20.0f;
        const float magnitude = abs(headYawDeg);
        if (magnitude <= innerRange)
            return headYawDeg;

        const float direction = headYawDeg < 0.0f ? -1.0f : 1.0f;
        if (magnitude <= outerRange) {
            const float outerAmount = (magnitude - innerRange) / (outerRange - innerRange);
            return direction * mix(innerRange, cockpitLimitAtOuterRange, outerAmount);
        }

        // Preserve unrestricted head movement after reaching the mapped torso range.
        return direction * (cockpitLimitAtOuterRange + magnitude - outerRange);
    }

    float MapHeadPitchToTorso(const float headPitchDeg) const {
        constexpr float deadzone = 1.0f;
        constexpr float innerRange = 15.0f;
        constexpr float outerRange = 30.0f;
        const float low = TorsoStats->Torso.IsPitchUnbound ? -90.0f : TorsoStats->Torso.BoundsLow.y;
        const float high = TorsoStats->Torso.IsPitchUnbound ? 90.0f : TorsoStats->Torso.BoundsHigh.y;
        // Playspace level is the stable torso-pitch neutral. Do not capture the player's head or
        // the mech's transient torso pose when entering a cockpit.
        const float neutral = clamp(0.0f, low, high);
        const float magnitude = max(0.0f, abs(headPitchDeg) - deadzone);
        if (magnitude == 0.0f)
            return neutral;

        const float direction = headPitchDeg < 0.0f ? -1.0f : 1.0f;
        const float limit = direction < 0.0f ? neutral - low : high - neutral;
        if (limit <= innerRange)
            return neutral + direction * min(magnitude, limit);
        if (magnitude <= innerRange)
            return neutral + direction * magnitude;

        const float outerAmount = min((magnitude - innerRange) / (outerRange - innerRange), 1.0f);
        return neutral + direction * mix(innerRange, limit, outerAmount);
    }

    static float MapHeadPitchToCockpitView(const float headPitchDeg) {
        constexpr float innerRange = 15.0f;
        constexpr float outerRange = 30.0f;
        constexpr float cockpitLimitAtOuterRange = 20.0f;
        const float magnitude = abs(headPitchDeg);
        if (magnitude <= innerRange)
            return headPitchDeg;

        const float direction = headPitchDeg < 0.0f ? -1.0f : 1.0f;
        if (magnitude <= outerRange) {
            const float outerAmount = (magnitude - innerRange) / (outerRange - innerRange);
            return direction * mix(innerRange, cockpitLimitAtOuterRange, outerAmount);
        }

        return direction * (cockpitLimitAtOuterRange + magnitude - outerRange);
    }

    void UpdateTorsoController(const float desiredYawDeg, const float desiredPitchDeg,
                               const float desiredCockpitYawDeg, const float desiredCockpitPitchDeg,
                               const float rawHeadYawDeg, const float rawHeadPitchDeg) {
        if (!IsHeadAimEnabled()) {
            TorsoYawInput.store(0.0f, std::memory_order_relaxed);
            TorsoPitchInput.store(0.0f, std::memory_order_relaxed);
            ViewYawCompensation.store(0.0f, std::memory_order_relaxed);
            ViewPitchCompensation.store(0.0f, std::memory_order_relaxed);
            return;
        }

        constexpr float stopToleranceDeg = 0.25f;
        constexpr float proportionalBandDeg = 12.0f;
        const float maximumInput = clamp(API::VR::get_mod_value<float>("HeadAim_TorsoMaximumInput"), 0.1f, 1.0f);
        const float yawError = TorsoStats->Torso.IsYawUnbound
            ? degrees(atan2(sin(radians(desiredYawDeg - TorsoRotation->x)),
                            cos(radians(desiredYawDeg - TorsoRotation->x))))
            : desiredYawDeg - TorsoRotation->x;
        const float pitchError = desiredPitchDeg - TorsoRotation->y;
        const float yawInput = abs(yawError) <= stopToleranceDeg
            ? 0.0f : clamp(yawError / proportionalBandDeg, -maximumInput, maximumInput);
        const float pitchInput = abs(pitchError) <= stopToleranceDeg
            ? 0.0f : clamp(pitchError / proportionalBandDeg, -maximumInput, maximumInput);

        // Drive the component's ordinary input independently of HeadTarget (torso-weapon aim).
        *NativeTorsoInput = vec2(yawInput, pitchInput);
        TorsoYawInput.store(yawInput, std::memory_order_relaxed);
        TorsoPitchInput.store(pitchInput, std::memory_order_relaxed);

        constexpr float lagBlendStart = 15.0f;
        constexpr float lagBlendEnd = 75.0f;
        const float lagBlend = clamp((abs(rawHeadYawDeg) - lagBlendStart) /
                                     (lagBlendEnd - lagBlendStart), 0.0f, 1.0f);
        const float settledViewCompression = desiredCockpitYawDeg - rawHeadYawDeg;
        ViewYawCompensation.store(settledViewCompression + lagBlend * yawError,
                                  std::memory_order_relaxed);

        constexpr float pitchLagBlendStart = 15.0f;
        constexpr float pitchLagBlendEnd = 30.0f;
        const float pitchLagBlend = clamp((abs(rawHeadPitchDeg) - pitchLagBlendStart) /
                                          (pitchLagBlendEnd - pitchLagBlendStart), 0.0f, 1.0f);
        const float settledPitchCompression = desiredCockpitPitchDeg - rawHeadPitchDeg;
        ViewPitchCompensation.store(settledPitchCompression + pitchLagBlend * pitchError,
                                    std::memory_order_relaxed);
    }

    bool IsHeadAimEnabled() const {
        return InMech && HeadAimMode && *HeadAimMode != HeadAimMode::Disabled;
    }

    void ProcessArmTwist(RotationDegrees cockpitRelativeRot, RotationDegrees torsoAimRotation) {
        const float zoom        = max(0.001f, *ZoomLevel);
        const float invZoom     = 1.0f / zoom;
        const float oneMinusInv = 1.0f - invZoom;

        // Calculate the target in cockpit relative coordinates so that it's stable relative to the cockpit regardless of torso twist or pitch,
        // otherwise the target will have to chase the torso as it rotates
        vec2 targetArmRotation = *ArmsTarget * invZoom;

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
            // Apply torsoDelta to catch the current frame's torso rotation and add the difference between the cockpit and torso aim accounting for zoom level
            yawOffset   = -torsoDelta.x * invZoom + torsoAimRotation.Yaw * oneMinusInv;
            pitchOffset = torsoDelta.y * invZoom - torsoAimRotation.Pitch * oneMinusInv;
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
