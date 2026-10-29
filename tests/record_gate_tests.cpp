// The recording gate (record_gate.h): the gate decision and its hysteresis, the
// restricted-camera spans, the cuts and the cut-aware sampling of transforms,
// the game camera and the marker blend.
#include "record_gate.h"
#include "camera_track.h"
#include "entity_transform.h"
#include "replay_recording.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    using namespace replay;
    using namespace replay::gate;

    int g_gateFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_gateFailures;
    }

    bool Near(float actual, float expected, float tolerance = 0.02f)
    {
        return std::fabs(actual - expected) <= tolerance;
    }

    void TestCutsceneRule()
    {
        Check(CutsceneRaw(SignalAnimSceneCam), "anim scene camera alone is a cutscene");
        Check(!CutsceneRaw(SignalScriptCam), "a script camera alone is not a cutscene");
        Check(CutsceneRaw(SignalScriptCam | SignalLetterbox) && CutsceneRaw(SignalScriptCam | SignalControlOff) &&
                CutsceneRaw(SignalScriptCam | SignalPlayerAnimScene),
            "a foreign script camera with letterbox, no control or a player anim scene is a cutscene");
        Check(!CutsceneRaw(SignalScriptCam | SignalGameplayCam | SignalControlOff),
            "with the gameplay camera rendering a script camera does not count");
        Check(!CutsceneRaw(SignalCinematicCam | SignalControlOff | SignalLetterbox),
            "the cinematic camera never gates");
        Check(!CutsceneRaw(SignalPlayerAnimScene | SignalControlOff),
            "an ambient anim scene (campfire, chair) without a script camera is not a cutscene");
    }

    void TestHysteresis()
    {
        Hysteresis h;
        Check(!h.Update(true) && !h.Update(true) && !h.active, "hysteresis: two raw ticks do not turn it on");
        Check(h.Update(true) && h.active, "hysteresis: on at the third consecutive tick");
        for (int n = 0; n < 9; ++n)
            h.Update(false);
        Check(h.active, "hysteresis: nine off ticks keep it on");
        h.Update(true);
        for (int n = 0; n < 9; ++n)
            h.Update(false);
        Check(h.active, "hysteresis: an on tick restarts the off count");
        Check(h.Update(false) && !h.active, "hysteresis: off at the tenth consecutive tick");
        h.Update(true);
        h.Update(false);
        h.Update(true);
        h.Update(true);
        Check(!h.active, "hysteresis: a broken run does not turn it on");
    }

    void TestGateDecision()
    {
        Gate gate;
        Decision d = gate.Update(SignalPauseMenu, false);
        Check(d.paused && d.reasons == ReasonMenu, "gate: the pause menu pauses at once");
        d = gate.Update(SignalFadedOut, false);
        Check(d.paused && d.reasons == ReasonFade, "gate: a faded out screen pauses");
        d = gate.Update(SignalLoading, false);
        Check(d.paused && d.reasons == ReasonLoading, "gate: a loading screen pauses");
        d = gate.Update(0, false);
        Check(!d.paused && d.reasons == ReasonNone, "gate: nothing, no pause");

        Gate skip;
        skip.Update(SignalAnimSceneCam, false);
        d = skip.Update(SignalAnimSceneCam, false);
        Check(!d.paused, "gate: a cutscene waits for the hysteresis");
        d = skip.Update(SignalAnimSceneCam, false);
        Check(d.paused && d.cutscene && d.reasons == ReasonCutscene, "gate: RecordCutscenes=0 pauses on a cutscene");

        Gate record;
        for (int n = 0; n < 3; ++n)
            d = record.Update(SignalAnimSceneCam, true);
        Check(!d.paused && d.cutscene && (d.reasons & ReasonCutscene), "gate: RecordCutscenes=1 keeps recording the cutscene");
        d = record.Update(SignalAnimSceneCam | SignalFadedOut, true);
        Check(d.paused && (d.reasons & ReasonFade), "gate: a fade pauses even when cutscenes are recorded");

        Check(!PauseMakesCut(ReasonMenu), "gate: a menu pause is no cut");
        Check(PauseMakesCut(ReasonMenu | ReasonCutscene) && PauseMakesCut(ReasonFade) && PauseMakesCut(ReasonLoading),
            "gate: cutscene, fade and loading make a cut");
        Check(std::string(ReasonName(ReasonCutscene | ReasonFade)) == "fade" && std::string(ReasonName(ReasonMenu | ReasonFade)) == "menu" &&
                std::string(ReasonName(ReasonCutscene)) == "cutscene" && std::string(ReasonName(0)) == "none",
            "gate: reason names in order");
    }

    void TestSpans()
    {
        std::vector<Span> spans;
        UpdateSpan(spans, SpanKind::ScriptCamera, true, 100);
        UpdateSpan(spans, SpanKind::ScriptCamera, true, 200);
        Check(spans.size() == 1 && spans[0].open && spans[0].startMs == 100 && spans[0].endMs == 200, "spans: open span follows the clock");
        UpdateSpan(spans, SpanKind::ControlsOff, true, 150);
        UpdateSpan(spans, SpanKind::ScriptCamera, false, 300);
        Check(spans.size() == 2 && !spans[0].open && spans[0].endMs == 300 && spans[1].open, "spans: closing one leaves the other");
        UpdateSpan(spans, SpanKind::Cutscene, true, 400);
        UpdateSpan(spans, SpanKind::Cutscene, false, 400);
        Check(spans.size() == 2, "spans: a span with no length is dropped");
        CloseSpans(spans, 500);
        Check(!spans[1].open && spans[1].endMs == 500, "spans: CloseSpans ends the open ones");
        Check(SpanAt(spans, SpanKind::ScriptCamera, 100) && SpanAt(spans, SpanKind::ScriptCamera, 299) &&
                !SpanAt(spans, SpanKind::ScriptCamera, 300) && !SpanAt(spans, SpanKind::Cutscene, 200),
            "spans: SpanAt, start inclusive, end exclusive");
    }

    void TestCuts()
    {
        std::vector<std::uint32_t> cuts;
        AddCut(cuts, 500);
        AddCut(cuts, 200);
        AddCut(cuts, 500);
        AddCut(cuts, 900);
        Check(cuts.size() == 3 && cuts[0] == 200 && cuts[1] == 500 && cuts[2] == 900, "cuts: sorted, no duplicates");
        std::uint32_t cut = 0;
        Check(CutBetween(cuts, 400, 533, cut) && cut == 500, "cuts: the cut between two samples");
        Check(CutBetween(cuts, 467, 500, cut) && cut == 500, "cuts: a sample exactly at the cut is after it");
        Check(!CutBetween(cuts, 500, 533, cut), "cuts: a sample exactly at the cut starts the next span");
        Check(!CutBetween(cuts, 520, 880, cut), "cuts: no cut in between");
        Check(Near(CutAwareFactor(&cuts, 400, 533, 450, 0.37f), 0.0f), "cut-aware: before the cut, the earlier sample");
        Check(Near(CutAwareFactor(&cuts, 400, 533, 500, 0.75f), 1.0f), "cut-aware: from the cut on, the later sample");
        Check(Near(CutAwareFactor(&cuts, 520, 553, 530, 0.3f), 0.3f), "cut-aware: no cut, plain interpolation");
        Check(Near(CutAwareFactor(nullptr, 400, 533, 450, 0.37f), 0.37f), "cut-aware: no cut list");

        // A transform track across a cut.
        entity::RelativeTrack track(30);
        for (const std::uint32_t t : {400u, 433u, 533u, 566u})
        {
            entity::TransformSample sample;
            sample.timeMs = t;
            sample.relative.position = {t < 500 ? 0.0f : 10.0f, 0.0f, 0.0f};
            track.Append(sample);
        }
        entity::TransformSample out;
        track.Sample(480, out, &cuts);
        Check(Near(out.relative.position.x, 0.0f), "transform: before the cut holds the earlier side");
        track.Sample(510, out, &cuts);
        Check(Near(out.relative.position.x, 10.0f), "transform: after the cut takes the later side");
        track.Sample(483, out);
        Check(out.relative.position.x > 0.5f && out.relative.position.x < 9.5f, "transform: without cuts it blends");

        // The game camera across a cut.
        const std::vector<camera::GameSample> game = {{400, {0, 0, 0}, {0, 0, 0}, 40.0f}, {533, {10, 0, 0}, {0, 0, 90}, 60.0f}};
        editor::CameraState state{};
        camera::EvaluateGame(game, 480, state, &cuts);
        Check(Near(state.position.x, 0.0f) && Near(state.fov, 40.0f), "game camera: before the cut, the earlier sample");
        camera::EvaluateGame(game, 520, state, &cuts);
        Check(Near(state.position.x, 10.0f) && Near(state.fov, 60.0f), "game camera: after the cut, the later sample");
        camera::EvaluateGame(game, 480, state);
        Check(state.position.x > 1.0f, "game camera: without cuts it interpolates");
    }

    void TestMarkerBlend()
    {
        camera::Keyframe a;
        a.timeMs = 0;
        a.cameraMode = camera::CameraMode::Free;
        a.blend = camera::Blend::Linear;
        a.ease = false;
        a.pose = {{0, 0, 0}, camera::Track::FromEulerDegrees({0, 0, 0}), 45.0f};
        camera::Keyframe b = a;
        b.timeMs = 1000;
        b.blend = camera::Blend::None;
        b.pose.position = {10, 0, 0};
        camera::Track track;
        track.AddMarker(a);
        track.AddMarker(b);
        camera::Scene scene;
        editor::CameraState state{};
        camera::EvaluateMarkers(track, 500, scene, state);
        Check(Near(state.position.x, 5.0f), "markers: a blend without cuts");
        const std::vector<std::uint32_t> cuts = {700};
        scene.cuts = &cuts;
        camera::SegmentPlan plan;
        camera::EvaluateMarkers(track, 500, scene, state, nullptr, 0.0f, &plan);
        Check(Near(state.position.x, 0.0f) && !plan.blending, "markers: a blend across a cut holds the source marker");
        camera::EvaluateMarkers(track, 1000, scene, state);
        Check(Near(state.position.x, 10.0f), "markers: the next marker after the cut");

        // LockCutsceneCamera: inside a recorded cutscene the game camera.
        const std::vector<camera::GameSample> game = {{0, {50, 0, 0}, {0, 0, 0}, 70.0f}, {2000, {50, 0, 0}, {0, 0, 0}, 70.0f}};
        const std::vector<Span> spans = {{200, 400, SpanKind::Cutscene, false}};
        camera::Scene locked;
        locked.game = &game;
        locked.gameOnly = [&](std::uint64_t t) { return SpanAt(spans, SpanKind::Cutscene, t) != nullptr; };
        camera::EvaluateMarkers(track, 300, locked, state);
        Check(Near(state.position.x, 50.0f) && Near(state.fov, 70.0f), "lock: a marker inside a cutscene is the game camera");
        camera::EvaluateMarkers(track, 500, locked, state);
        Check(Near(state.position.x, 5.0f), "lock: outside the cutscene the markers work as before");
    }

    void TestScorchedAt()
    {
        ScorchTrack track;
        Check(!ScorchedAt(track, 100), "charred: no state, not charred");
        track.states = {{200, true}, {600, false}};
        Check(!ScorchedAt(track, 199) && ScorchedAt(track, 200) && ScorchedAt(track, 599) && !ScorchedAt(track, 600),
            "charred: the last change at or before the playhead (both directions)");
    }
}

int RunRecordGateTests()
{
    TestCutsceneRule();
    TestHysteresis();
    TestGateDecision();
    TestSpans();
    TestCuts();
    TestMarkerBlend();
    TestScorchedAt();
    if (g_gateFailures == 0)
        std::printf("Record gate tests passed.\n");
    return g_gateFailures;
}
