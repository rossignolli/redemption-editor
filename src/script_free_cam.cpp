// script_free_cam.cpp: the free camera: start and stop, orbit edit, follow, keys and pad, actions, the camera overlay, the view and lens.
// Split from script.cpp (moved code); shared internals in script_internal.h.

#include "../script_internal.h"

namespace
{
    constexpr Hash kInputLookLeftRight = 0xA987235Fu;
    constexpr Hash kInputLookUpDown = 0xD2047988u;
    float g_freeCamSpeedScale = 1.0f;  // the wheel's speed scale
    replay::freecam::TargetFrame g_fcTargetFrame{};
    ULONGLONG g_freeCamLastUpdateAt = 0;
}

    bool StartFreeCam(ULONGLONG now)
    {
        if (!g_transport.Playing())
        {
            ShowObjective("Redemption Editor: the free camera can only be used during PLAY");
            return false;
        }

        StopFreeCam();
        g_freeCamPosition = CAM::GET_FINAL_RENDERED_CAM_COORD();
        g_freeCamRotation = CAM::GET_FINAL_RENDERED_CAM_ROT(2);
        g_freeCamFov = CAM::GET_FINAL_RENDERED_CAM_FOV();
        g_freeCam = CAM::CREATE_CAM_WITH_PARAMS(
            "DEFAULT_SCRIPTED_CAMERA",
            g_freeCamPosition.x,
            g_freeCamPosition.y,
            g_freeCamPosition.z,
            g_freeCamRotation.x,
            g_freeCamRotation.y,
            g_freeCamRotation.z,
            g_freeCamFov,
            TRUE,
            2);

        if (!g_freeCam || !CAM::DOES_CAM_EXIST(g_freeCam))
        {
            g_freeCam = 0;
            ShowObjective("Redemption Editor: could not create the free camera");
            return false;
        }

        g_freeCamPlayer = PLAYER::PLAYER_PED_ID();
        if (g_freeCamPlayer && ENTITY::DOES_ENTITY_EXIST(g_freeCamPlayer))
        {
            g_freeCamPlayerWasFrozen = ENTITY::_IS_ENTITY_FROZEN(g_freeCamPlayer) != FALSE;
            if (!g_freeCamPlayerWasFrozen)
                ENTITY::FREEZE_ENTITY_POSITION(g_freeCamPlayer, TRUE);
        }

        CAM::SET_CAM_ACTIVE(g_freeCam, TRUE);
        CAM::RENDER_SCRIPT_CAMS(TRUE, TRUE, 250, TRUE, TRUE, 0);
        NewLensCamera(g_freeLens);
        g_freeCamLastUpdateAt = now;
        g_freeCamEnabled = true;
        g_cameraTrackManualOverride = g_cameraTrack.Empty();
        ShowObjective("Redemption Editor: free camera on");
        return true;
    }

    // Where Edit Camera starts: the marker's live view. A Free marker still on the
    // placeholder pose (the world origin, saved in older clips) starts from the
    // recorded game camera at its time instead of far away.
    replay::camera::LivePose EditStartPose(const replay::camera::Keyframe& key)
    {
        replay::camera::LivePose live = replay::camera::Resolve(key, key.timeMs, MarkerScene());
        const replay::editor::Vec3& at = key.pose.position;
        const bool placeholder = key.cameraMode == replay::editor::CameraMode::Free && key.attachTarget == 0 &&
            at.x == 0.0f && at.y == 0.0f && at.z == 0.0f;
        replay::editor::CameraState game{};
        if (placeholder && replay::camera::EvaluateGame(g_clip.gameCamera, key.timeMs, game, &g_clip.cuts))
        {
            live.pose.position = game.position;
            const replay::editor::Vec3 rotation = live.hasLookAt
                ? replay::camera::LookAtWithScreenOffset(live.lookAtPoint, game.position, live.lookAtOffset, live.pose.fov,
                      live.lookAtRoll, MarkerScene().aspect)
                : game.rotation;
            live.pose.orientation = replay::camera::Track::FromEulerDegrees(rotation);
            LogFile() << "CAMERA_MARKER,action=edit_placeholder,id=" << key.id << '\n';
        }
        return live;
    }
    OrbitEdit g_orbit;
namespace
{

    bool UpdateOrbitEdit(float deltaSeconds, float lookX, float lookY)
    {
        const replay::camera::Keyframe* key = g_editingMarker ? g_cameraTrack.Find(g_editingMarker) : nullptr;
        replay::camera::EntityState target{};
        if (!key || !key->lookAtTarget || key->cameraMode != replay::editor::CameraMode::Free ||
            !replay::camera::LookupLookAt(*key, key->timeMs, MarkerScene(), target)) // around the bone
        {
            g_orbit.marker = 0;
            return false;
        }
        const replay::editor::Vec3 base = target.hasHead ? target.head
            : replay::editor::Vec3{target.position.x, target.position.y, target.position.z + replay::camera::kLookAtAimHeight};
        constexpr float kDeg = 3.14159265f / 180.0f;
        if (g_orbit.marker != key->id)
        {
            // Start from where the camera is now around the target.
            g_orbit = {};
            g_orbit.marker = key->id;
            const float dx = g_freeCamPosition.x - base.x, dy = g_freeCamPosition.y - base.y, dz = g_freeCamPosition.z - base.z;
            g_orbit.distance = std::max(0.5f, std::sqrt(dx * dx + dy * dy + dz * dz));
            g_orbit.yaw = std::atan2(-dx, dy) / kDeg;
            g_orbit.pitch = std::asin(std::clamp(dz / g_orbit.distance, -1.0f, 1.0f)) / kDeg;
            g_orbit.offset = key->lookAtOffset;
            g_orbit.roll = key->lookAtRoll;
        }
        const bool fast = InterlockedCompareExchange(&g_fastMove, 0, 0) != 0;
        const float speed = fast ? 3.0f : 1.0f;
        if (InterlockedCompareExchange(&g_moveLeft, 0, 0)) g_orbit.yaw -= 60.0f * speed * deltaSeconds;
        if (InterlockedCompareExchange(&g_moveRight, 0, 0)) g_orbit.yaw += 60.0f * speed * deltaSeconds;
        if (InterlockedCompareExchange(&g_moveForward, 0, 0)) g_orbit.pitch += 40.0f * speed * deltaSeconds;
        if (InterlockedCompareExchange(&g_moveBackward, 0, 0)) g_orbit.pitch -= 40.0f * speed * deltaSeconds;
        g_orbit.pitch = std::clamp(g_orbit.pitch, -80.0f, 85.0f);
        if (InterlockedCompareExchange(&g_turnUp, 0, 0)) g_orbit.distance *= 1.0f - 0.8f * speed * deltaSeconds;
        if (InterlockedCompareExchange(&g_turnDown, 0, 0)) g_orbit.distance *= 1.0f + 0.8f * speed * deltaSeconds;
        g_orbit.distance = std::clamp(g_orbit.distance, 0.4f, 150.0f);
        if (InterlockedCompareExchange(&g_moveUp, 0, 0)) g_orbit.height += 0.8f * speed * deltaSeconds;
        if (InterlockedCompareExchange(&g_moveDown, 0, 0)) g_orbit.height -= 0.8f * speed * deltaSeconds;
        g_orbit.height = std::clamp(g_orbit.height, -2.0f, 3.0f);
        g_orbit.offset.x = std::clamp(g_orbit.offset.x + lookX * 0.6f * deltaSeconds * 10.0f, -1.0f, 1.0f);
        g_orbit.offset.y = std::clamp(g_orbit.offset.y - lookY * 0.6f * deltaSeconds * 10.0f, -1.0f, 1.0f);
        if (InterlockedCompareExchange(&g_rollLeft, 0, 0)) g_orbit.roll -= 30.0f * deltaSeconds;
        if (InterlockedCompareExchange(&g_rollRight, 0, 0)) g_orbit.roll += 30.0f * deltaSeconds;
        const float fovAmount = 30.0f * deltaSeconds;
        if (InterlockedCompareExchange(&g_fovIn, 0, 0)) g_freeCamFov -= fovAmount;
        if (InterlockedCompareExchange(&g_fovOut, 0, 0)) g_freeCamFov += fovAmount;
        g_freeCamFov = std::clamp(g_freeCamFov, replay::editor::kMarkerFovMin, replay::editor::kMarkerFovMax);

        const replay::editor::Vec3 point{base.x, base.y, base.z + g_orbit.height};
        const float cp = std::cos(g_orbit.pitch * kDeg), sp = std::sin(g_orbit.pitch * kDeg);
        const float sy = std::sin(g_orbit.yaw * kDeg), cy = std::cos(g_orbit.yaw * kDeg);
        const replay::editor::Vec3 position{point.x - sy * cp * g_orbit.distance, point.y + cy * cp * g_orbit.distance,
            point.z + sp * g_orbit.distance};
        const replay::editor::Vec3 rotation = replay::camera::LookAtWithScreenOffset(point, position, g_orbit.offset, g_freeCamFov,
            g_orbit.roll, MarkerScene().aspect);
        g_freeCamPosition = {position.x, position.y, position.z};
        g_freeCamRotation = {rotation.x, rotation.y, rotation.z};
        g_freeCamVelocity = {};
        PlaceCam(g_freeCam, g_freeCamCut, "orbit", position.x, position.y, position.z, rotation.x, rotation.y, rotation.z,
            g_freeCamFov, deltaSeconds, false);
        return true;
    }

    // ------------------------------------------------------------------
    // The free camera's flight state, its target and its actions.

    replay::log::LogLimit g_fcCollisionLogs;
    replay::log::LogLimit g_fcProbeLogs;
    replay::log::LogLimit g_fcTraceLogs;
    std::uint32_t g_fcTraceTick = 0;

    // Evidence: one FREECAM_TICK line every 20 ticks of the free camera (the
    // branch that placed it, the keys it saw, where it is).
    void TraceFreeCam(const char* branch, const replay::freecam::Input* input = nullptr)
    {
        if (++g_fcTraceTick % 20 != 0 || !g_fcTraceLogs.Take(600))
            return;
        LogFile() << "FREECAM_TICK,branch=" << branch << ",at=" << g_freeCamPosition.x << '/' << g_freeCamPosition.y << '/'
                  << g_freeCamPosition.z << ",rot=" << g_freeCamRotation.x << '/' << g_freeCamRotation.y << '/' << g_freeCamRotation.z
                  << ",fwd=" << (input ? input->moveForward : 0.0f) << ",right=" << (input ? input->moveRight : 0.0f)
                  << ",up=" << (input ? input->moveUp : 0.0f) << ",pad=" << (g_fcPadActive ? 1 : 0)
                  << ",follow=" << static_cast<int>(g_fcFollow) << ",target=" << g_fcTarget << ",lost=" << (g_fcTargetLost ? 1 : 0)
                  << ",locked=" << (g_fcLocked ? 1 : 0) << ",editor=" << (g_editorCoversScreen ? 1 : 0)
                  << ",track=" << (g_cameraTrackEnabled ? 1 : 0) << ",override=" << (g_cameraTrackManualOverride ? 1 : 0)
                  << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
    }

    // The flight state follows the globals other code sets (Edit Camera, the
    // marker track, the orbit, the start): a view set from outside carries
    // no momentum.
    void SyncFreeCamState()
    {
        const replay::editor::Vec3 at{g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z};
        const replay::editor::Vec3 rotation{g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z};
        const auto same = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
        if (same(at.x, g_fcState.position.x) && same(at.y, g_fcState.position.y) && same(at.z, g_fcState.position.z) &&
            same(rotation.x, g_fcState.rotation.x) && same(rotation.y, g_fcState.rotation.y) &&
            same(rotation.z, g_fcState.rotation.z) && same(g_freeCamFov, g_fcState.fov))
            return;
        g_fcState.SetView(at, rotation, g_freeCamFov);
        g_fcState.velocity = {g_freeCamVelocity.x, g_freeCamVelocity.y, g_freeCamVelocity.z};
    }
}

    // The flight state set to the free camera's view as it is now (a view set
    // from outside: Edit Camera's marker). SyncFreeCamState only resets when the
    // view differs; this resets always.
    void ResetFreeCamToView()
    {
        g_fcState.SetView({g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z},
            {g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z}, g_freeCamFov);
        g_freeCamSmoothedLookX = g_freeCamSmoothedLookY = 0.0f;
    }
namespace
{

    void StoreFreeCamState()
    {
        g_freeCamPosition.x = g_fcState.position.x;
        g_freeCamPosition.y = g_fcState.position.y;
        g_freeCamPosition.z = g_fcState.position.z;
        g_freeCamRotation.x = g_fcState.rotation.x;
        g_freeCamRotation.y = g_fcState.rotation.y;
        g_freeCamRotation.z = g_fcState.rotation.z;
        g_freeCamFov = g_fcState.fov;
        g_freeCamVelocity.x = g_fcState.velocity.x;
        g_freeCamVelocity.y = g_fcState.velocity.y;
        g_freeCamVelocity.z = g_fcState.velocity.z;
        g_freeCamSmoothedLookX = g_fcState.smoothedMouseX;
        g_freeCamSmoothedLookY = g_fcState.smoothedMouseY;
    }

    // The target's recorded frame at the playhead (camera2's recorded root).
    bool FreeCamTargetFrame(std::uint32_t id, replay::freecam::TargetFrame& frame, replay::editor::Vec3* head = nullptr)
    {
        replay::camera::EntityState state{};
        if (!id || !MarkerEntity(id, g_transport.PlayheadMs(), state))
            return false;
        frame.position = state.position;
        frame.rotation = {state.pitch, state.roll, state.heading};
        if (head)
            *head = state.hasHead ? state.head : replay::editor::Vec3{state.position.x, state.position.y, state.position.z + 0.6f};
        return true;
    }

    // Follow: the camera carried by the target's move since the last tick
    // (a seek moves it with the target, keeping the framing).
    void FreeCamFollowTick()
    {
        if (g_fcFollow == replay::freecam::Follow::Off || !g_fcTarget)
        {
            g_fcTargetFrameValid = false;
            return;
        }
        replay::freecam::TargetFrame now{};
        if (!FreeCamTargetFrame(g_fcTarget, now))
        {
            g_fcTargetLost = true;
            g_fcTargetFrameValid = false;
            return;
        }
        g_fcTargetLost = false;
        if (g_fcTargetFrameValid)
        {
            SyncFreeCamState();
            const float roll = g_fcState.rotation.y;
            replay::freecam::ApplyFollow(g_fcState, g_fcFollow, g_fcTargetFrame, now);
            if (g_editingMarker) // Edit Camera stays level: a Rigid follow keeps the marker's roll
                replay::freecam::KeepRoll(g_fcState, roll);
            StoreFreeCamState();
        }
        g_fcTargetFrame = now;
        g_fcTargetFrameValid = true;
    }

    replay::freecam::Input FreeCamKeyInput(float rawLookX, float rawLookY)
    {
        const auto held = [](volatile LONG& flag) { return InterlockedCompareExchange(&flag, 0, 0) != 0; };
        replay::freecam::Input input;
        input.mouseX = rawLookX;
        input.mouseY = rawLookY;
        input.turnX = (held(g_turnRight) ? 1.0f : 0.0f) - (held(g_turnLeft) ? 1.0f : 0.0f);
        input.turnY = (held(g_turnUp) ? 1.0f : 0.0f) - (held(g_turnDown) ? 1.0f : 0.0f);
        input.roll = (held(g_rollRight) ? 1.0f : 0.0f) - (held(g_rollLeft) ? 1.0f : 0.0f);
        input.moveForward = (held(g_moveForward) ? 1.0f : 0.0f) - (held(g_moveBackward) ? 1.0f : 0.0f);
        input.moveRight = (held(g_moveRight) ? 1.0f : 0.0f) - (held(g_moveLeft) ? 1.0f : 0.0f);
        input.moveUp = (held(g_moveUp) ? 1.0f : 0.0f) - (held(g_moveDown) ? 1.0f : 0.0f);
        input.zoom = (held(g_fovIn) ? 1.0f : 0.0f) - (held(g_fovOut) ? 1.0f : 0.0f);
        input.fast = held(g_fastMove);
        input.slow = held(g_slowMove);
        return input;
    }

    // The pad (when it, not the keyboard, is in use): left stick moves, right stick
    // turns (a rate, not the mouse's delta), the triggers go up and down, the
    // bumpers roll, the D-pad zooms. [hypothesis: the RDR2 control names below
    // read under SET_PLAYER_CONTROL off, as INPUT_LOOK_* do]
    bool ReadFreeCamPad(replay::freecam::Input& input)
    {
        if (PAD::IS_USING_KEYBOARD_AND_MOUSE(0))
            return false;
        static const Hash moveLr = MISC::GET_HASH_KEY("INPUT_MOVE_LR");
        static const Hash moveUd = MISC::GET_HASH_KEY("INPUT_MOVE_UD");
        static const Hash rise = MISC::GET_HASH_KEY("INPUT_ATTACK");
        static const Hash sink = MISC::GET_HASH_KEY("INPUT_AIM");
        static const Hash rollLeft = MISC::GET_HASH_KEY("INPUT_FRONTEND_LB");
        static const Hash rollRight = MISC::GET_HASH_KEY("INPUT_FRONTEND_RB");
        static const Hash zoomIn = MISC::GET_HASH_KEY("INPUT_FRONTEND_UP");
        static const Hash zoomOut = MISC::GET_HASH_KEY("INPUT_FRONTEND_DOWN");
        for (const Hash control : {moveLr, moveUd, rise, sink, rollLeft, rollRight, zoomIn, zoomOut})
            PAD::DISABLE_CONTROL_ACTION(0, control, TRUE);
        const auto axis = [](Hash control) {
            const float value = PAD::GET_DISABLED_CONTROL_NORMAL(0, control);
            return std::fabs(value) > 0.12f ? value : 0.0f; // the stick's dead zone
        };
        // The right stick: the look axes are a position on a pad.
        input.turnX += input.mouseX;
        input.turnY -= input.mouseY;
        input.turnX = std::clamp(input.turnX, -1.0f, 1.0f);
        input.turnY = std::clamp(input.turnY, -1.0f, 1.0f);
        input.mouseX = input.mouseY = 0.0f;
        input.moveRight += axis(moveLr);
        input.moveForward -= axis(moveUd);
        input.moveUp += axis(rise) - axis(sink);
        if (PAD::IS_DISABLED_CONTROL_PRESSED(0, rollLeft))
            input.roll -= 1.0f;
        if (PAD::IS_DISABLED_CONTROL_PRESSED(0, rollRight))
            input.roll += 1.0f;
        if (PAD::IS_DISABLED_CONTROL_PRESSED(0, zoomIn))
            input.zoom += 1.0f;
        if (PAD::IS_DISABLED_CONTROL_PRESSED(0, zoomOut))
            input.zoom -= 1.0f;
        input.analog = true;
        return true;
    }

    // Collision and Walk: the engine's synchronous line probe (as in
    // replay_events.cpp and replay_bench.cpp: flags -1, result 2) and the ground
    // height.
    replay::freecam::World FreeCamWorld()
    {
        replay::freecam::World world;
        world.sweep = [](const replay::editor::Vec3& from, const replay::editor::Vec3& to, float radius,
                          replay::editor::Vec3& hit, replay::editor::Vec3& normal) {
            const float dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
            const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (length < 0.0001f)
                return false;
            const float reach = (length + radius) / length;
            const ScrHandle probe = SHAPETEST::START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE(from.x, from.y, from.z,
                from.x + dx * reach, from.y + dy * reach, from.z + dz * reach, -1, PLAYER::PLAYER_PED_ID(), 7);
            BOOL didHit = FALSE;
            Vector3 end{};
            Vector3 surface{};
            Entity entity = 0;
            const int status = SHAPETEST::GET_SHAPE_TEST_RESULT(probe, &didHit, &end, &surface, &entity);
            if (g_fcProbeLogs.Take(5))
            {
                LogFile() << "FREECAM_PROBE,status=" << status << ",hit=" << (didHit ? 1 : 0) << ",normal=" << surface.x << '/'
                          << surface.y << '/' << surface.z << '\n';
                // Evidence (log only, once a session): which flags and options see what is
                // under the camera. Map collision is hit by -1, 1, 511; a script-made object by
                // none, 16 included.
                static bool diagnosed = false;
                if (!diagnosed)
                {
                diagnosed = true;
                for (const int flags : {-1, 1, 16, 511, 0x7FFFFFFF})
                    for (const int options : {7, 4, 0})
                    {
                        const ScrHandle down = SHAPETEST::START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE(from.x, from.y, from.z,
                            from.x, from.y, from.z - 12.0f, flags, PLAYER::PLAYER_PED_ID(), options);
                        BOOL downHit = FALSE;
                        Vector3 downEnd{}, downNormal{};
                        Entity downEntity = 0;
                        const int downStatus = SHAPETEST::GET_SHAPE_TEST_RESULT(down, &downHit, &downEnd, &downNormal, &downEntity);
                        LogFile() << "FREECAM_PROBE_DIAG,flags=" << flags << ",options=" << options << ",status=" << downStatus
                                  << ",hit=" << (downHit ? 1 : 0) << ",z=" << downEnd.z << ",from_z=" << from.z
                                  << ",entity=" << downEntity << ",type=" << (downEntity ? ENTITY::GET_ENTITY_TYPE(downEntity) : -1) << '\n';
                    }
                }
            }
            if (status != 2 || !didHit)
                return false;
            hit = {end.x, end.y, end.z};
            normal = {surface.x, surface.y, surface.z};
            return true;
        };
        world.ground = [](float x, float y, float z, float& groundZ) {
            return MISC::GET_GROUND_Z_FOR_3D_COORD(x, y, z, &groundZ, FALSE) != FALSE;
        };
        return world;
    }

    // The clip's peds, horses and vehicles at the playhead (what the editor
    // lists as targets).
    std::vector<replay::freecam::Candidate> FreeCamCandidates()
    {
        std::vector<replay::freecam::Candidate> out;
        for (const replay::playback::Puppet& puppet : g_puppets.list)
        {
            const replay::entity::EntityKind kind = puppet.packet.kind;
            if (kind != replay::entity::EntityKind::Ped && kind != replay::entity::EntityKind::Horse &&
                kind != replay::entity::EntityKind::Vehicle)
                continue;
            replay::freecam::TargetFrame frame{};
            replay::editor::Vec3 head{};
            if (FreeCamTargetFrame(puppet.packet.id.Packed(), frame, &head))
                out.push_back({puppet.packet.id.Packed(), head});
        }
        return out;
    }
}

    void SetFreeCamTarget(std::uint32_t id, const char* how)
    {
        g_fcTarget = id;
        g_fcTargetFrameValid = false;
        g_fcTargetLost = false;
        if (!id)
            g_fcFollow = replay::freecam::Follow::Off;
        ShowObjective(id ? "Redemption Editor: free camera target set" : "Redemption Editor: free camera target cleared");
        LogFile() << "FREECAM_TARGET,how=" << how << ",target=" << id << ",main=" << (id && id == MainCharacterId() ? 1 : 0)
                  << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
    }

    // Keys (OnKeyboardMessage) and the editor (FreeCamAction,
    // SetCameraOverlay) leave bits; taken here on the script thread.
    void ProcessFreeCamActions()
    {
        // A target belongs to its clip: another clip (or a new recording)
        // drops it and the follow.
        static std::uint32_t clipSeen = ~0u;
        if (clipSeen != g_clipLoadSequence)
        {
            clipSeen = g_clipLoadSequence;
            g_fcTarget = 0;
            g_fcFollow = replay::freecam::Follow::Off;
            g_fcTargetFrameValid = false;
            g_fcLocked = false;
        }
        const LONG bits = InterlockedExchange(&g_fcActionBits, 0);
        if (!bits)
            return;
        bool saveOptions = false;
        const auto toggle = [&](bool& value, const char* on, const char* off) {
            value = !value;
            ShowObjective(value ? on : off);
            saveOptions = true;
        };
        if (bits & kOverlayToggle)
        {
            g_camOverlayEnabled = !g_camOverlayEnabled;
            ShowObjective(g_camOverlayEnabled ? "Redemption Editor: camera markers in the world on" : "Redemption Editor: camera markers in the world off");
            SaveOverlayOptions();
        }
        const bool flying = g_freeCamEnabled && g_transport.Playing();
        if (bits & kFcDrone)
        {
            g_fcSettings.motion = g_fcSettings.motion == replay::freecam::Motion::Drone ? replay::freecam::Motion::Classic
                                                                                      : replay::freecam::Motion::Drone;
            g_fcState.Reset();
            ShowObjective(g_fcSettings.motion == replay::freecam::Motion::Drone ? "Redemption Editor: free camera Drone on"
                                                                               : "Redemption Editor: free camera Drone off");
            saveOptions = true;
        }
        if (bits & kFcAcrobatic)
        {
            toggle(g_fcSettings.acrobatic, "Redemption Editor: acrobatic rotation on", "Redemption Editor: acrobatic rotation off");
            if (!g_fcSettings.acrobatic)
                g_freeCamRotation.x = std::clamp(g_freeCamRotation.x, -89.0f, 89.0f);
        }
        if (bits & kFcWalk)
            toggle(g_fcSettings.walk, "Redemption Editor: free camera Walk on", "Redemption Editor: free camera Walk off");
        if (bits & kFcCollision)
            toggle(g_fcSettings.collision, "Redemption Editor: free camera collision on", "Redemption Editor: free camera collision off");
        if (bits & kFcLockAltitude)
            toggle(g_fcSettings.lockAltitude, "Redemption Editor: altitude locked", "Redemption Editor: altitude free");
        if (bits & kFcLock)
        {
            g_fcLocked = !g_fcLocked;
            ShowObjective(g_fcLocked ? "Redemption Editor: free camera held still" : "Redemption Editor: free camera moves");
        }
        if (bits & kFcClearTarget)
            SetFreeCamTarget(0, "editor");
        if (flying && (bits & (kFcPick | kFcPickNearest)))
        {
            const std::vector<replay::freecam::Candidate> candidates = FreeCamCandidates();
            const replay::editor::Vec3 at{g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z};
            const replay::editor::Vec3 rotation{g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z};
            const std::uint32_t picked = (bits & kFcPickNearest) ? replay::freecam::PickNearest(at, candidates)
                                                                 : replay::freecam::PickAimed(at, rotation, candidates);
            if (picked)
                SetFreeCamTarget(picked, (bits & kFcPickNearest) ? "nearest" : "aimed");
            else
                ShowObjective("Redemption Editor: no character or vehicle there");
        }
        if (bits & kFcFollow)
        {
            if (!g_fcTarget && flying)
            {
                const std::vector<replay::freecam::Candidate> candidates = FreeCamCandidates();
                const std::uint32_t picked = replay::freecam::PickAimed({g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z},
                    {g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z}, candidates);
                if (picked)
                    SetFreeCamTarget(picked, "follow");
            }
            if (!g_fcTarget)
                ShowObjective(replay::keys::Fill("Redemption Editor: aim at a character or vehicle to follow it ({key_pick} picks one)",
                    replay::keys::Current()).c_str());
            else
            {
                g_fcFollow = static_cast<replay::freecam::Follow>((static_cast<int>(g_fcFollow) + 1) % replay::freecam::kFollowCount);
                g_fcTargetFrameValid = false;
                ShowObjective(g_fcFollow == replay::freecam::Follow::Rigid ? "Redemption Editor: follow the target (rigid)"
                        : g_fcFollow == replay::freecam::Follow::Translate ? "Redemption Editor: follow the target (position only)"
                                                                           : "Redemption Editor: follow off");
            }
        }
        if (flying && (bits & kFcLevel))
        {
            SyncFreeCamState();
            replay::freecam::LevelHorizon(g_fcState);
            StoreFreeCamState();
            g_cameraTrackManualOverride = true;
        }
        if (flying && (bits & kFcBehind))
        {
            replay::freecam::TargetFrame frame{};
            replay::editor::Vec3 head{};
            const std::uint32_t who = g_fcTarget ? g_fcTarget : MainCharacterId();
            if (FreeCamTargetFrame(who, frame, &head))
            {
                SyncFreeCamState();
                replay::freecam::ShotBehind(g_fcState, frame, &head);
                StoreFreeCamState();
                g_cameraTrackManualOverride = true;
                g_freeCamCut = {}; // a cut, not a move
            }
        }
        if (saveOptions)
            SaveFreeCamOptions();
        LogFreeCamConfig("action");
    }
namespace
{

    // ------------------------------------------------------------------
    // The camera markers in the world. The path is built when the markers change
    // (camera_overlay.h BuildPath, up to 2400 evaluations), projected every tick
    // with the view placed this tick and written to the overlay channel; the
    // add-on draws it after the export's capture (never in a video).
    replay::overlaychannel::Channel* g_overlayChannel = nullptr;
    HANDLE g_overlayMapping = nullptr;
    bool g_overlayChannelFailed = false;
    replay::camera::Track g_overlayTrack;
    std::uint32_t g_overlayClipSequence = ~0u;
    bool g_overlayPathBuilt = false;
    replay::camera::overlay::WorldPath g_overlayPath;
    std::vector<replay::camera::overlay::TrailPoint> g_overlayTrailPoints;
    std::uint64_t g_overlayTrailAt = ~0ull;
    std::uint32_t g_overlayTrailTarget = 0;
    std::uint32_t g_overlayCheckedClip = ~0u;
    std::uint32_t g_overlayShownFrames = 0;
    replay::log::LogLimit g_overlayLogs;

    replay::overlaychannel::Channel* OverlayChannel()
    {
        if (g_overlayChannel || g_overlayChannelFailed)
            return g_overlayChannel;
        namespace oc = replay::overlaychannel;
        wchar_t name[64];
        oc::Name(name, GetCurrentProcessId());
        const std::uint64_t size = sizeof(oc::Channel);
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(size >> 32),
            static_cast<DWORD>(size & 0xFFFFFFFFu), name);
        void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(oc::Channel)) : nullptr;
        if (!view)
        {
            if (mapping)
                CloseHandle(mapping);
            g_overlayChannelFailed = true;
            LogFile() << "CAMERA_OVERLAY,phase=channel,ok=0,error=" << GetLastError() << '\n';
            return nullptr;
        }
        g_overlayMapping = mapping;
        // A hot reload finds the old mapping: made again (the add-on reads
        // `bytes` and the published frame each present).
        g_overlayChannel = new (view) oc::Channel();
        g_overlayChannel->bytes = static_cast<std::uint32_t>(sizeof(oc::Channel));
        LogFile() << "CAMERA_OVERLAY,phase=channel,ok=1,bytes=" << sizeof(oc::Channel) << '\n';
        return g_overlayChannel;
    }
}

    void UpdateCameraOverlay()
    {
        REPLAY_PROFILE_ZONE("tick.camoverlay");
        namespace oc = replay::overlaychannel;
        namespace ov = replay::camera::overlay;
        oc::Channel* channel = OverlayChannel();
        if (!channel)
            return;
        oc::Frame& frame = oc::BeginWrite(*channel);
        const bool playing = g_transport.Playing();
        const bool editing = g_editingMarker != 0;
        const bool still = g_transport.Paused() || !replay::playback::NormalPlay();
        const bool show = g_camOverlayEnabled && playing && g_lastPlacedView.valid && !replay::exportprobe::HidesHud() &&
            !ClipLoadWaiting() && (still || editing || g_freeCamEnabled || g_camOverlayWhilePlaying);
        if (!show)
        {
            frame.flags = oc::FrameHidden;
            oc::EndWrite(*channel, frame);
            return;
        }
        const replay::camera::Scene scene = MarkerScene();
        // The path drawn is the one that plays: the Spline defaults and the camera
        // weight (which applies on play) included.
        static replay::editor::SplineTuning builtDefaults = replay::editor::DefaultSplineTuning();
        if (!g_overlayPathBuilt || g_overlayClipSequence != g_clipLoadSequence || !replay::camera::SameMarkers(g_overlayTrack, g_cameraTrack) ||
            !replay::camera::spline::Same(builtDefaults, g_splineDefaults))
        {
            const std::uint64_t t0 = replay::prof::Now();
            g_overlayTrack = g_cameraTrack;
            g_overlayClipSequence = g_clipLoadSequence;
            builtDefaults = g_splineDefaults;
            replay::camera::Scene withWeight = scene;
            withWeight.splineWeight = true;
            g_overlayPath = ov::BuildPath(g_cameraTrack, withWeight);
            g_overlayPathBuilt = true;
            const double ms = static_cast<double>(replay::prof::Now() - t0) * 1000.0 / static_cast<double>(replay::prof::TicksPerSecond());
            if (g_overlayLogs.Take(100))
                LogFile() << "CAMERA_OVERLAY,phase=path,points=" << g_overlayPath.points.size() << ",markers=" << g_overlayPath.markers.size()
                          << ",evaluations=" << g_overlayPath.evaluations << ",ms=" << ms << '\n';
        }
        const std::uint64_t playheadMs = g_transport.PlayheadMs();
        // The free camera's target: its recorded path around the playhead.
        const std::uint32_t trailTarget = g_camOverlayTrail ? g_fcTarget : 0u;
        if (trailTarget != g_overlayTrailTarget || g_overlayTrailAt == ~0ull ||
            (playheadMs > g_overlayTrailAt ? playheadMs - g_overlayTrailAt : g_overlayTrailAt - playheadMs) > 250)
        {
            g_overlayTrailTarget = trailTarget;
            g_overlayTrailAt = playheadMs;
            g_overlayTrailPoints = trailTarget ? ov::BuildTrail(scene.lookup, trailTarget, playheadMs > 4000 ? playheadMs - 4000 : 0,
                                                     std::min<std::uint64_t>(playheadMs + 8000, PlaybackDurationMs()), 100)
                                               : std::vector<ov::TrailPoint>{};
        }
        ov::ProjectSettings settings;
        settings.playheadMs = playheadMs;
        settings.editingId = g_editingMarker;
        const replay::camera::SegmentPlan plan = replay::camera::Plan(g_cameraTrack, playheadMs);
        if (plan.valid && plan.current >= 0)
            settings.currentId = g_cameraTrack.Keyframes()[plan.current].id;
        settings.labelEveryMs = g_camOverlayTimeLabels ? 1000u : 0u;
        settings.dim = !still && !editing && !g_freeCamEnabled;
        if (g_freeCamEnabled && !editing && g_cameraTrackEnabled)
        {
            replay::editor::CameraState marker{};
            if (replay::camera::EvaluateMarkersRaw(g_cameraTrack, playheadMs, scene, marker))
            {
                settings.showCamera = true;
                settings.cameraAt = marker.position;
            }
        }
        if (g_fcTarget)
        {
            replay::freecam::TargetFrame frameNow{};
            replay::editor::Vec3 head{};
            if (FreeCamTargetFrame(g_fcTarget, frameNow, &head))
            {
                settings.targetId = g_fcTarget;
                settings.targetValid = true;
                settings.targetAt = {head.x, head.y, head.z + 0.35f};
            }
        }
        const ov::ProjectReport report = ov::Project(g_overlayPath, g_overlayTrailPoints, g_lastPlacedView.view, settings, frame);
        oc::EndWrite(*channel, frame);
        if (report.dropped && g_overlayLogs.Take(100))
            LogFile() << "CAMERA_OVERLAY,phase=dropped,lines=" << report.lines << ",dots=" << report.dots << ",labels=" << report.labels
                      << ",dropped=" << report.dropped << '\n';
        // Evidence, once per clip: our projection of a marker against the
        // engine's (GET_SCREEN_COORD_FROM_WORLD_COORD, 0..1, y down). The
        // overlay lines up with the picture only when they agree.
        if (g_overlayCheckedClip != g_clipLoadSequence && ++g_overlayShownFrames > 30)
        {
            for (const ov::MarkerPoint& m : g_overlayPath.markers)
            {
                const replay::editor::Vec3 local = ov::ToCamera(g_lastPlacedView.view, m.at);
                if (local.y < 1.0f)
                    continue;
                float sx = 0.0f, sy = 0.0f;
                if (!GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD(m.at.x, m.at.y, m.at.z, &sx, &sy))
                    continue;
                replay::editor::Vec2 a{}, b{};
                if (!ov::ProjectSegment(g_lastPlacedView.view, m.at, m.at, a, b))
                    continue;
                int width = 0, height = 0;
                GRAPHICS::GET_SCREEN_RESOLUTION(&width, &height);
                const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 16.0f / 9.0f;
                const float oursX = 0.5f + a.x / aspect * 0.5f, oursY = 0.5f - a.y * 0.5f;
                g_overlayCheckedClip = g_clipLoadSequence;
                LogFile() << "CAMERA_OVERLAY,phase=projection_check,marker=" << m.id << ",ours=" << oursX << '/' << oursY
                          << ",engine=" << sx << '/' << sy << ",err_x=" << (oursX - sx) << ",err_y=" << (oursY - sy)
                          << ",aspect=" << aspect << ",fov=" << g_lastPlacedView.view.fov << ",meters=" << local.y << '\n';
                break;
            }
        }
    }
namespace
{

    void UpdateFreeCamView(ULONGLONG now)
    {
        REPLAY_PROFILE_ZONE("tick.freecam");
        if (!g_freeCamEnabled || !g_freeCam || !CAM::DOES_CAM_EXIST(g_freeCam))
        {
            g_freeCamCut = {}; // the next freecam view starts with a cut
            return;
        }

        CAM::SET_WIDESCREEN_BORDERS(FALSE, 0); // no cinematic letterbox on the freecam
        // Esc cancels Edit Camera (the editor's overlay), it does not open the game's
        // pause menu over it.
        if (g_editingMarker)
        {
            PAD::DISABLE_CONTROL_ACTION(2, MISC::GET_HASH_KEY("INPUT_FRONTEND_PAUSE"), TRUE);
            PAD::DISABLE_CONTROL_ACTION(2, MISC::GET_HASH_KEY("INPUT_FRONTEND_PAUSE_ALTERNATE"), TRUE);
        }
        float deltaSeconds = static_cast<float>(now - g_freeCamLastUpdateAt) / 1000.0f;
        if (deltaSeconds > 0.05f)
            deltaSeconds = 0.05f;
        g_freeCamLastUpdateAt = now;

        // Read the look axes even after disabling them, so the scripted camera
        // owns mouse-look without also moving RDR2's gameplay camera.
        PAD::DISABLE_CONTROL_ACTION(0, kInputLookLeftRight, TRUE);
        PAD::DISABLE_CONTROL_ACTION(0, kInputLookUpDown, TRUE);
        const float rawLookX =
            PAD::GET_DISABLED_CONTROL_NORMAL(0, kInputLookLeftRight);
        const float rawLookY =
            PAD::GET_DISABLED_CONTROL_NORMAL(0, kInputLookUpDown);

        const bool cameraInput =
            std::fabs(rawLookX) > 0.0001f ||
            std::fabs(rawLookY) > 0.0001f ||
            InterlockedCompareExchange(&g_turnLeft, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_turnRight, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_turnUp, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_turnDown, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_rollLeft, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_rollRight, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_moveForward, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_moveBackward, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_moveLeft, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_moveRight, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_moveUp, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_moveDown, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_fovIn, 0, 0) != 0 ||
            InterlockedCompareExchange(&g_fovOut, 0, 0) != 0;
        // With the editor's UI up the keyboard is the editor's (Ctrl+Z must not roll
        // the camera, typing a clip name must not move it); H hides the UI to fly,
        // Edit Camera hands the keys over.
        const bool keysForEditor = g_editorCoversScreen && !g_editingMarker;
        if (keysForEditor)
        {
            g_freeCamVelocity = {};
            FreeCamFollowTick(); // a followed target still carries the camera
            TraceFreeCam("keys_for_editor");
            PlaceCam(g_freeCam, g_freeCamCut, "freecam", g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z,
                g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z, g_freeCamFov, deltaSeconds, false);
            return;
        }
        if (g_editResolvePending && g_editingMarker)
        {
            // Edit Camera: the marker's live view, now that the playhead
            // (and the puppets) stand at its time; until then the camera
            // holds still.
            const replay::camera::Keyframe* key = g_cameraTrack.Find(g_editingMarker);
            if (key && replay::playback::Preparing())
                return;
            g_editResolvePending = false;
            if (key && g_transport.PlayheadMs() == std::min<std::uint64_t>(key->timeMs, PlaybackDurationMs()))
            {
                const replay::camera::LivePose live = EditStartPose(*key);
                const replay::editor::Vec3 rotation = replay::camera::Track::ToEulerDegrees(live.pose.orientation);
                g_freeCamPosition = {live.pose.position.x, live.pose.position.y, live.pose.position.z};
                g_freeCamRotation = {rotation.x, rotation.y, rotation.z};
                g_freeCamFov = live.pose.fov;
                g_freeCamVelocity = {};
                ResetFreeCamToView(); // no roll, banking or turn rate left from before
                g_orbit.marker = 0; // the orbit starts again from this view
                PlaceCam(g_freeCam, g_freeCamCut, "freecam", g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z,
                    g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z, g_freeCamFov, deltaSeconds, true);
                return;
            }
        }
        else
            g_editResolvePending = false;
        if (cameraInput)
            g_cameraTrackManualOverride = true;
        if (UpdateOrbitEdit(deltaSeconds, rawLookX, rawLookY))
        {
            TraceFreeCam("orbit");
            return;
        }

        if (g_cameraTrackEnabled && !g_cameraTrackManualOverride)
        {
            replay::editor::CameraState evaluated{};
            replay::camera::SegmentPlan freePlan;
            if (EvaluateCameraTrack(g_transport.PlayheadMs(), evaluated, 0.0f, &freePlan))
            {
                TraceFreeCam("track");
                // The same marker boundary rule as the replay camera (this camera renders
                // while the freecam is on).
                static replay::camcut::MarkerFrame lastFreeMarker;
                const replay::camcut::MarkerFrame freeMarker = MarkerFrameOf(true, freePlan);
                const bool markerCut = replay::camcut::MarkerBoundaryCut(lastFreeMarker, freeMarker);
                lastFreeMarker = freeMarker;
                g_freeCamVelocity = {};
                g_freeCamSmoothedLookX = 0.0f;
                g_freeCamSmoothedLookY = 0.0f;
                g_freeCamPosition = {evaluated.position.x, evaluated.position.y, evaluated.position.z};
                g_freeCamRotation = {evaluated.rotation.x, evaluated.rotation.y, evaluated.rotation.z};
                g_freeCamFov = evaluated.fov;
                PlaceCam(g_freeCam, g_freeCamCut, "freecam", g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z,
                    g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z, g_freeCamFov, deltaSeconds,
                    markerCut || !replay::playback::NormalPlay());
                return;
            }
        }

        // The flight itself (free_cam.h). Classic is the base flight
        // (tests/camera_cine_tests.cpp replays it by hand); Drone, acrobatic, Walk,
        // collision and the locked altitude are the options. The pad moves it too
        // (sticks, triggers, bumpers). The wheel scales the speed (x0.05 to x4), Ctrl
        // moves slow (a quarter).
        {
            // The wheel reads as the weapon controls in RDR2 (tested in game: the cursor
            // scroll of the GTA V never fires; NEXT is the wheel down, PREV up).
            // Down (next) slows, up (prev) speeds up. Disabled so the player's weapon
            // does not change under the free camera.
            static const Hash wheelDown = MISC::GET_HASH_KEY("INPUT_NEXT_WEAPON");
            static const Hash wheelUp = MISC::GET_HASH_KEY("INPUT_PREV_WEAPON");
            static const Hash selectDown = MISC::GET_HASH_KEY("INPUT_SELECT_NEXT_WEAPON");
            static const Hash selectUp = MISC::GET_HASH_KEY("INPUT_SELECT_PREV_WEAPON");
            for (const Hash control : {wheelDown, wheelUp, selectDown, selectUp})
                PAD::DISABLE_CONTROL_ACTION(0, control, TRUE);
            if (PAD::IS_DISABLED_CONTROL_JUST_PRESSED(0, wheelUp))
                g_freeCamSpeedScale = std::min(4.0f, g_freeCamSpeedScale * 1.25f);
            if (PAD::IS_DISABLED_CONTROL_JUST_PRESSED(0, wheelDown))
                g_freeCamSpeedScale = std::max(0.05f, g_freeCamSpeedScale / 1.25f);
        }
        SyncFreeCamState();
        FreeCamFollowTick();
        if (g_fcLocked)
        {
            TraceFreeCam("locked");
            g_fcState.velocity = {};
            StoreFreeCamState();
            PlaceCam(g_freeCam, g_freeCamCut, "freecam", g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z,
                g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z, g_freeCamFov, deltaSeconds, false);
            return;
        }
        replay::freecam::Input input = FreeCamKeyInput(rawLookX, rawLookY);
        const bool pad = ReadFreeCamPad(input);
        g_fcPadActive = pad || (g_fcPadActive && !cameraInput);
        g_fcSettings.speedScale = g_freeCamSpeedScale;
        const replay::freecam::World world = FreeCamWorld();
        // Edit Camera flies level (free_cam.h LevelFlight): the acrobatic rotation
        // and the Drone banking roll the horizon, and a marker's roll is its own.
        const replay::freecam::Settings flight = g_editingMarker ? replay::freecam::LevelFlight(g_fcSettings) : g_fcSettings;
        const replay::freecam::StepReport step = replay::freecam::Step(g_fcState, input, flight, deltaSeconds, &world);
        if (step.collided && g_fcCollisionLogs.Take(20))
            LogFile() << "FREECAM_COLLISION,at=" << g_fcState.position.x << '/' << g_fcState.position.y << '/' << g_fcState.position.z
                      << ",speed=" << step.speed << '\n';
        StoreFreeCamState();
        TraceFreeCam("flight", &input);

        PlaceCam(g_freeCam, g_freeCamCut, "freecam", g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z,
            g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z, g_freeCamFov, deltaSeconds, false);
    }

    // The free camera's near clip and DOF, so what the editor shows while a marker
    // is framed (Edit Camera) or the free camera flies is what the clip will play
    // (the replay camera, ApplyMarkerDof, does not render while the free camera is
    // on: UpdateReplayCamera returns at g_freeCamEnabled).
    //  - Edit Camera: the edited marker's near clip and DOF;
    //  - the free camera playing the markers: the segment's (as the replay camera);
    //  - the free camera flying: Free Camera > Near Clip, no DOF.
    void ApplyFreeCamLens()
    {
        if (!g_freeCamEnabled || !g_freeCam || !CAM::DOES_CAM_EXIST(g_freeCam))
            return;
        g_cameraLive = {};
        const replay::editor::CameraState state{{g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z},
            {g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z}, g_freeCamFov};
        const std::uint64_t playheadMs = g_transport.PlayheadMs();
        const replay::camera::Keyframe* editing = g_editingMarker ? g_cameraTrack.Find(g_editingMarker) : nullptr;
        replay::camera::FocusPlan focus;
        float nearClip = g_fcNearClip;
        std::uint32_t markerId = 0;
        if (editing)
        {
            g_cameraLive.flags |= replay::editor::CameraLiveEditCamera;
            nearClip = replay::camera::MarkerNearClip(*editing);
            focus = replay::camera::PlanFocus(editing, state, MarkerScene(), editing->timeMs);
            markerId = editing->id;
        }
        else if (g_cameraTrackEnabled && !g_cameraTrackManualOverride && !g_cameraTrack.Empty())
        {
            const replay::camera::SegmentPlan plan = replay::camera::Plan(g_cameraTrack, playheadMs);
            nearClip = replay::camera::NearClipAt(g_cameraTrack, plan);
            focus = replay::camera::BlendFocus(g_cameraTrack, plan, state, MarkerScene(), playheadMs, nullptr, g_camFocusConfig).plan;
            const std::vector<replay::camera::Keyframe>& keys = g_cameraTrack.Keyframes();
            if (plan.valid && plan.dofIndex >= 0 && plan.dofIndex < static_cast<int>(keys.size()))
                markerId = keys[plan.dofIndex].id;
        }
        ApplyNearClip(g_freeCam, g_freeLens, nearClip, editing ? "edit" : "free");
        LensState& lens = g_freeLens;
        if (focus.source == replay::camera::FocusSource::Default)
        {
            lens.focusSent = -1.0f;
            if (lens.dof)
            {
                CAM::_0x1FC6C727D30FFDDE(g_freeCam); // the engine's DOF clear
                lens.dof = false;
                g_lensChangedAt = GetTickCount64(); // back to Default, shown through the freeze
            }
            if (lens.engineFocus)
            {
                if (lens.pausedBefore >= 0)
                    CAM::_PAUSE_CAMERA_FOCUS(g_freeCam, lens.pausedBefore ? TRUE : FALSE);
                lens.engineFocus = false;
            }
            return;
        }
        const float lensMm = SendDofBlock(g_freeCam, focus, state.fov);
        lens.dof = true;
        float sent = 0.0f;
        if (focus.source == replay::camera::FocusSource::Engine)
        {
            if (!lens.engineFocus)
            {
                std::uint16_t word = 0;
                lens.pausedBefore = ReadCamFlags(g_freeCam, word) ? ((word & kCamFocusPausedBit) ? 1 : 0) : -1;
                CAM::_PAUSE_CAMERA_FOCUS(g_freeCam, FALSE);
                lens.engineFocus = true;
            }
            lens.focusSent = -1.0f;
        }
        else
        {
            if (lens.engineFocus)
            {
                if (lens.pausedBefore >= 0)
                    CAM::_PAUSE_CAMERA_FOCUS(g_freeCam, lens.pausedBefore ? TRUE : FALSE);
                lens.engineFocus = false;
            }
            if (focus.source != replay::camera::FocusSource::Sharp)
            {
                sent = focus.distance;
                SendFocusDistance(g_freeCam, lens, sent, editing ? "edit" : "free");
            }
            else
                lens.focusSent = -1.0f;
        }
        CAM::ALLOW_MOTION_BLUR_DECAY(g_freeCam, FALSE);
        NoteDofReadback(g_freeCam, editing ? "edit" : "free", focus, lensMm, sent, markerId);
    }
}

    void UpdateFreeCam(ULONGLONG now)
    {
        UpdateFreeCamView(now);
        ApplyFreeCamLens();
    }
