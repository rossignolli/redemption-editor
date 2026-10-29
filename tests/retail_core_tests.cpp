#include "camera_track.h"
#include "playback_time.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>

int RunPoseStreamTests();
int RunEntityLifecycleTests();
int RunProfilerTests();
int RunPoseSegmentsTests();
int RunReplayTransportTests();
int RunRopeTrackTests();
int RunBenchScriptTests();
int RunClipFileTests();
int RunRecordingWindowTests();
int RunCameraTrackTests();
int RunCamera2Tests();
int RunCameraCineTests();
int RunCameraCustomTests();
int RunCameraFxTests(); // shake events
int RunCameraMarkerShakeTests();
int RunCameraSplineTests(); // Spline tuning
int RunCameraAttachTests(); // bone attach, near clip, DOF
int RunRecordGateTests();
int RunWorldStateTests();
int RunEditorInputTests();
int RunEditorKeysTests(); // remappable keys
int RunEditorUiQaTests();
int RunEditorCursorTests(); // the cursor under ReShade 6.8
int RunUiScaleTests(); // Replay Settings' Interface scale (src/ui_scale.h)
int RunUiLayoutTests(); // the layout maths and the UI harness checks
int RunLifecycleTests();
int RunReplayFixTests();
int RunVehiclePiecesTests(); // vehicle pieces
int RunTrainReplayTests(); // trains
int RunSoundCensusTests(); // sound census
int RunSoundFxProbeTests();
int RunSoundGenericTests();
int RunSoundGenericPlayerTests();
int RunCaptureProbeTests();
int RunSoundSlotPolicyTests(); // entity sound slots
int RunSoundCheckTests(); // sound check and capture
int RunSeekSettleTests(); // scrub, settle and far jump
int RunPoseJerkTests(); // rider and mount, jerk detector
int RunPuppetWatchTests(); // town NPCs: discovery and missing puppet detector
int RunPuppetBlinkTests(); // puppet blink detector: classification and episodes
int RunSkeletonMapTests(); // horse T-pose: per-group skeleton map
int RunSkeletonSignatureTests(); // skeleton signature at the arm
int RunEntityPoolsTests(); // entity pool reader
int RunModHygieneTests(); // log policy, clock, guarded copy, region cache, ropes
int RunIniPublicTests(); // ini whitelist, dev reads, first-run ini, report clips, crash notice
int RunStoragePathsTests(); // the base folder (Program Files fallback), read roots, layered settings
int RunRecIndicatorTests(); // the recording indicator: modes, fallback, brief clock
int RunI18nTests(); // the languages: lang files, fallbacks, UTF-8, the choice, the pseudo language
int RunEntityCacheTests(); // entity cache, event ranges, lifecycle stamp
int RunTrainConsistTests(); // trains: consist walk, links, speed, puppet judge
int RunGallopTests(); // gallop: player horse audio sites, trusted regions, replay effect sites
int RunSoundParticlesTests(); // clip area, puppet one-shots, per-frame sounds, registered fx rules
int RunFxCheckTests();
int RunParticleReplayTests(); // particle effects in the replay
int RunFxResidueTests();
int RunWeaponBonesTests();
int RunFrozenPoseTests();
int RunMuscleWatchTests();
int RunStrapStateTests(); // weapon straps: the rebuild request and bits played on a puppet weapon
int RunWeaponOrderTests(); // a puppet weapon's components: the wraps last (the bolt-action wrap over the scope)
int RunWeaponLookTests(); // weapon accessories and dirt
int RunWeaponButtTests(); // weapon butt strikes
int RunVehicleSoundTests(); // vehicle sounds
int RunMissingSoundTests();
int RunHorseAudioEvidenceTests(); // player's horse audio evidence: field table, diff
int RunFootMaterialTests(); // footstep surface: slot layout, pairing
int RunPuppetSoundsTests(); // puppet sounds: unrecorded persistent, layers and LOD, creator B ref
int RunHorseSoundGapsTests(); // horse sound gaps: foot wetness, animal lines, sound report
int RunSaddleSkidSoundTests(); // saddle and skid sounds: the packets model and the sound check's notice
int RunSpeechTests(); // speech
int RunSpeechPromptGhostTests(); // ghost lines of the interaction prompt
int RunSkidHoofSoundTests(); // skid and hoof sounds: the step rule, the ports' verdicts
int RunMixProbeTests(); // mix probe
int RunStillPoseTests();
int RunLootObjectsTests(); // loot drawers, vehicle children, reinstance, live objects, gore
int RunDynamiteFuseTests(); // dynamite fuse kept alive, wagon reins rest mode
int RunSpawnedPropsTests(); // spawned object recheck, wagon wheel pieces
int RunMapDataTests(); // map data owners, map dressing
int RunLootTests(); // loot: taken map dressing, removals, near watch, shared places
int RunWorldRecordingTests(); // world recording
int RunObjectLookTests(); // object textures
int RunVoicesTests(); // voices
int RunPlayerWeaponBankTests(); // gunshot sounds
int RunDecalAddGuardTests(); // decal crashes 0x2C42D1A and 0x15E5BD
int RunWorldFocusTests();
int RunGameListenerTests();
int RunVolumeProbeTests(); // volume probe
int RunWindReplayTests(); // wind in the replay
int RunPedSpeedTests(); // hoof volume
int RunShaderVarsTests(); // horse muscles
int RunFootEventTests(); // hoof volume
int RunFootMovementTests();
int RunCategoryStateTests(); // category mix
int RunSoundWorldTests(); // world sounds
int RunSpeechConversationTests(); // speech conversations
int RunSoundCheck036Tests();
int RunPoseFidelityTests(); // pose fidelity judge
int RunPoseCaptureRulesTests(); // capture rules
int RunPlayerHoldTests(); // the player never hidden (QA 224)
int RunSessionResetTests(); // the session reset after a script restart
int RunPauseSpansTests(); // pause spans with running totals
int RunStopPathTests(); // clip pointer audit split, deferred log
int RunPlaybackPerfTests(); // sample reuse, world clear lookups, playback benchmarks
int RunMountPlacementTests(); // rider follows mount at the ped pre-render
int RunJitterTests();
int RunProjectTests(); // projects, trim and markers, project file
int RunProjectExportTests(); // whole project export: pieces, seams, sound
int RunSkyTrackTests(); // the clouds at the playhead
int RunEnvOverrideTests(); // the editor's environment settings
int RunExportScheduleTests(); // render export
int RunCrashEvidenceTests();
int RunEventRingTests(); // the observer rings' protocol
int RunQaKitTests();
int RunMusicStopTests();
int RunRenderTargetTests();
int RunDrawStoreProbeTests(); // crash 0x363068
int RunWorldModelTests();
int RunTheatreTests(); // closed theater
int RunTheatreExitTests(); // the closed theater's safe exit
int RunObjectCheckTests();
int RunSpeechSeekTests(); // speech after a jump, camera mod
int RunSpeechSlotsTests(); // speech slots, lines playing at the recording's start
int RunScenerySpeechTests(); // speech route, rope models, scenery visibility
int RunLodDoorsCameraTests(); // LOD, doors, camera
int RunAppearanceTests(); // appearance
int RunWorldCleanupTests();
int RunDecalGateTests(); // decal request ring producer gate
int RunTimeScaleTests();

namespace
{
    int g_failures = 0;
    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    bool Near(float actual, float expected, float tolerance = 0.02f)
    {
        return std::fabs(actual - expected) <= tolerance;
    }

    void TestForwardCameraTransport()
    {
        const replay::transport::AdvanceResult middle =
            replay::transport::Advance(100, 25, 200);
        Check(middle.timeMs == 125 && !middle.reachedBoundary,
            "camera transport advances monotonically");

        const replay::transport::AdvanceResult end =
            replay::transport::Advance(190, 25, 200);
        Check(end.timeMs == 200 && end.reachedBoundary,
            "camera transport clamps at the forward boundary");
    }

    void TestScrubTransport()
    {
        using namespace replay::transport;
        // Forward scrub: the wall delta scaled by the cursor speed, clamped
        // at the end of the timeline.
        Check(ScrubDeltaMs(16, kScrubSpeed) == 32, "scrub delta scales the frame delta");
        Check(ScrubDeltaMs(16, -kScrubSpeed) == -32, "scrub delta is negative backward");
        Check(ScrubDeltaMs(0, kScrubSpeed) == 0, "no frame delta, no scrub delta");
        const MoveResult forward = Move(100, 32, 200);
        Check(forward.timeMs == 132 && !forward.reachedStart && !forward.reachedEnd,
            "scrub moves forward");
        const MoveResult end = Move(190, 32, 200);
        Check(end.timeMs == 200 && end.reachedEnd && !end.reachedStart,
            "scrub clamps at the end");
        // Backward scrub clamps at the start of the timeline.
        const MoveResult backward = Move(100, -32, 200);
        Check(backward.timeMs == 68 && !backward.reachedStart && !backward.reachedEnd,
            "scrub moves backward");
        const MoveResult start = Move(20, -32, 200);
        Check(start.timeMs == 0 && start.reachedStart && !start.reachedEnd,
            "scrub clamps at the start");
        const MoveResult exact = Move(32, -32, 200);
        Check(exact.timeMs == 0 && exact.reachedStart, "scrub reaching zero is the start");
        // Fine scrub: a fixed step per frame in the held direction.
        Check(FineScrubDeltaMs(1) == kFineScrubStepMs && FineScrubDeltaMs(-1) == -kFineScrubStepMs &&
                FineScrubDeltaMs(0) == 0,
            "fine scrub step per frame");
        Check(Move(5, FineScrubDeltaMs(-1), 200).timeMs == 0, "fine scrub clamps at the start");
    }

    void TestCameraTrack()
    {
        replay::camera::Track track;
        replay::editor::CameraState first{};
        first.position = {0.0f, 0.0f, 1.0f};
        first.rotation = {0.0f, 0.0f, 0.0f};
        first.fov = 45.0f;
        replay::editor::CameraState second{};
        second.position = {10.0f, 2.0f, 3.0f};
        second.rotation = {0.0f, 0.0f, 90.0f};
        second.fov = 65.0f;
        track.AddOrReplace(
            0, first, replay::editor::Easing::Linear,
            replay::editor::Path::Linear);
        track.AddOrReplace(
            1000, second, replay::editor::Easing::Linear,
            replay::editor::Path::Linear);

        replay::editor::CameraState evaluated{};
        Check(track.Evaluate(500, evaluated),
            "camera track evaluates between keyframes");
        Check(Near(evaluated.position.x, 5.0f) &&
                Near(evaluated.position.y, 1.0f) &&
                Near(evaluated.fov, 55.0f),
            "camera track interpolates position and FOV");

        // The recorded game camera: halfway between two samples, the yaw
        // crosses 180 the short way (170 to -170 passes 180, not 0).
        const std::vector<replay::camera::GameSample> game = {
            {0, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 170.0f}, 40.0f},
            {100, {10.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -170.0f}, 60.0f},
        };
        replay::editor::CameraState sampled{};
        Check(replay::camera::EvaluateGame(game, 50, sampled) && Near(sampled.position.x, 5.0f) &&
                Near(sampled.fov, 50.0f) && Near(std::fabs(sampled.rotation.z), 180.0f),
            "game camera interpolates and wraps the yaw");
        Check(replay::camera::EvaluateGame(game, 500, sampled) && Near(sampled.position.x, 10.0f),
            "game camera holds the last sample");
        Check(!replay::camera::EvaluateGame({}, 0, sampled), "game camera without samples");

        // Camera modes: a Target keyframe is resolved at the evaluated time
        // (the resolver stands in for the engine), and blends with a Free one.
        replay::camera::Track modes;
        replay::editor::CameraState fixed{};
        fixed.position = {0.0f, 0.0f, 0.0f};
        fixed.fov = 40.0f;
        modes.AddOrReplace(0, fixed, replay::editor::Easing::Linear, replay::editor::Path::Linear);
        const std::uint32_t aimed = modes.AddOrReplace(1000, fixed, replay::editor::Easing::Linear, replay::editor::Path::Linear);
        modes.SetMode(aimed, replay::editor::CameraMode::Target, 7, {});
        const replay::camera::Resolver resolver = [](const replay::camera::Keyframe& key, std::uint64_t, replay::camera::Pose& pose) {
            // target 7 stands at +X: looking at it is yaw -90.
            const replay::editor::Vec3 rotation = replay::camera::LookAtDegrees(key.pose.position, {10.0f, 0.0f, 0.0f});
            pose.orientation = replay::camera::Track::FromEulerDegrees(rotation);
            return key.target == 7;
        };
        replay::editor::CameraState blended{};
        Check(modes.Evaluate(1000, blended, resolver) && Near(blended.rotation.z, -90.0f),
            "target keyframe looks at its target");
        Check(modes.Evaluate(500, blended, resolver) && Near(blended.rotation.z, -45.0f),
            "free to target blends halfway");
        const replay::editor::Vec3 local = replay::camera::ToHeadingFrame({0.0f, 2.0f, 1.0f}, 90.0f);
        const replay::editor::Vec3 back = replay::camera::FromHeadingFrame(local, 90.0f);
        Check(Near(back.x, 0.0f) && Near(back.y, 2.0f) && Near(back.z, 1.0f), "heading frame round trip");
        // Heading 90 faces -X, so a point ahead of the target (world -X) is local +Y.
        const replay::editor::Vec3 ahead = replay::camera::ToHeadingFrame({-3.0f, 0.0f, 0.0f}, 90.0f);
        Check(Near(ahead.y, 3.0f) && Near(ahead.x, 0.0f), "heading frame forward is the heading");
    }


}

int main()
{
    TestForwardCameraTransport();
    TestScrubTransport();
    TestCameraTrack();
    g_failures += RunPoseStreamTests();
    g_failures += RunEntityLifecycleTests();
    g_failures += RunProfilerTests();
    g_failures += RunPoseSegmentsTests();
    g_failures += RunReplayTransportTests();
    g_failures += RunRopeTrackTests();
    g_failures += RunBenchScriptTests();
    g_failures += RunClipFileTests();
    g_failures += RunRecordingWindowTests();
    g_failures += RunCameraTrackTests();
    g_failures += RunRecordGateTests();
    g_failures += RunWorldStateTests();
    g_failures += RunEditorInputTests();
    g_failures += RunEditorKeysTests();
    g_failures += RunEditorUiQaTests();
    g_failures += RunEditorCursorTests();
    g_failures += RunUiScaleTests();
    g_failures += RunUiLayoutTests();
    g_failures += RunLifecycleTests();
    g_failures += RunReplayFixTests();
    g_failures += RunVehiclePiecesTests();
    g_failures += RunTrainReplayTests();
    g_failures += RunSoundCensusTests();
    g_failures += RunSoundFxProbeTests();
    g_failures += RunSoundGenericTests();
    g_failures += RunSoundGenericPlayerTests();
    g_failures += RunCaptureProbeTests();
    g_failures += RunSoundSlotPolicyTests();
    g_failures += RunSoundCheckTests();
    g_failures += RunSeekSettleTests();
    g_failures += RunPoseJerkTests();
    g_failures += RunPuppetWatchTests();
    g_failures += RunPuppetBlinkTests();
    g_failures += RunSkeletonMapTests();
    g_failures += RunSkeletonSignatureTests();
    g_failures += RunEntityPoolsTests();
    g_failures += RunModHygieneTests();
    g_failures += RunIniPublicTests();
    g_failures += RunStoragePathsTests();
    g_failures += RunRecIndicatorTests();
    g_failures += RunI18nTests();
    g_failures += RunEntityCacheTests();
    g_failures += RunTrainConsistTests();
    g_failures += RunGallopTests();
    g_failures += RunSoundParticlesTests();
    g_failures += RunFxCheckTests();
    g_failures += RunParticleReplayTests();
    g_failures += RunFxResidueTests();
    g_failures += RunWeaponBonesTests();
    g_failures += RunFrozenPoseTests();
    g_failures += RunMuscleWatchTests();
    g_failures += RunStrapStateTests();
    g_failures += RunWeaponOrderTests();
    g_failures += RunWeaponLookTests();
    g_failures += RunWeaponButtTests();
    g_failures += RunVehicleSoundTests();
    g_failures += RunMissingSoundTests();
    g_failures += RunHorseAudioEvidenceTests();
    g_failures += RunFootMaterialTests();
    g_failures += RunPuppetSoundsTests();
    g_failures += RunHorseSoundGapsTests();
    g_failures += RunMixProbeTests();
    g_failures += RunStillPoseTests();
    g_failures += RunLootObjectsTests();
    g_failures += RunDynamiteFuseTests();
    g_failures += RunSpawnedPropsTests();
    g_failures += RunMapDataTests();
    g_failures += RunLootTests();
    g_failures += RunObjectLookTests();
    g_failures += RunVoicesTests();
    g_failures += RunPlayerWeaponBankTests();
    g_failures += RunDecalAddGuardTests();
    g_failures += RunWorldFocusTests();
    g_failures += RunGameListenerTests();
    g_failures += RunVolumeProbeTests();
    g_failures += RunWindReplayTests();
    g_failures += RunPedSpeedTests();
    g_failures += RunShaderVarsTests();
    g_failures += RunFootEventTests();
    g_failures += RunFootMovementTests();
    g_failures += RunCategoryStateTests();
    g_failures += RunSoundWorldTests();
    g_failures += RunSpeechConversationTests();
    g_failures += RunSoundCheck036Tests();
    g_failures += RunSaddleSkidSoundTests();
    g_failures += RunSpeechTests();
    g_failures += RunSpeechPromptGhostTests();
    g_failures += RunSkidHoofSoundTests();
    g_failures += RunPoseFidelityTests();
    g_failures += RunPoseCaptureRulesTests();
    g_failures += RunPlayerHoldTests();
    g_failures += RunPauseSpansTests();
    g_failures += RunSessionResetTests();
    g_failures += RunStopPathTests();
    g_failures += RunPlaybackPerfTests();
    g_failures += RunMountPlacementTests();
    g_failures += RunJitterTests();
    g_failures += RunProjectTests();
    g_failures += RunProjectExportTests();
    g_failures += RunSkyTrackTests();
    g_failures += RunEnvOverrideTests();
    g_failures += RunExportScheduleTests();
    g_failures += RunCrashEvidenceTests();
    g_failures += RunEventRingTests();
    g_failures += RunQaKitTests();
    g_failures += RunMusicStopTests();
    g_failures += RunRenderTargetTests();
    g_failures += RunDrawStoreProbeTests();
    g_failures += RunWorldModelTests();
    g_failures += RunTheatreTests();
    g_failures += RunTheatreExitTests();
    g_failures += RunObjectCheckTests();
    g_failures += RunSpeechSeekTests();
    g_failures += RunSpeechSlotsTests();
    g_failures += RunScenerySpeechTests();
    g_failures += RunWorldCleanupTests();
    g_failures += RunDecalGateTests();
    g_failures += RunTimeScaleTests();
    g_failures += RunLodDoorsCameraTests();
    g_failures += RunAppearanceTests();
    g_failures += RunCamera2Tests();
    g_failures += RunCameraCineTests();
    g_failures += RunCameraCustomTests();
    g_failures += RunCameraFxTests();
    g_failures += RunCameraMarkerShakeTests();
    g_failures += RunCameraSplineTests();
    g_failures += RunCameraAttachTests();
    g_failures += RunWorldRecordingTests();

    if (g_failures != 0)
    {
        std::fprintf(
            stderr, "%d retail core test(s) failed.\n", g_failures);
        return 1;
    }

    std::puts("All retail-only core tests passed.");
    return 0;
}
