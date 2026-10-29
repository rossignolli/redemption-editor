// The puppets' LOD rule (puppet_lod.h), the door rebind and map wait
// (door_rebind.h), the replay camera's cut rule (camera_cut.h) and the
// puppet_low_lod anomaly of the QA kit.
#include "camera_cut.h"
#include "door_rebind.h"
#include "puppet_lod.h"
#include "qa_health.h"

#include <cmath>
#include <cstdio>
#include <string>

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

    void TestPuppetLod()
    {
        using namespace replay::lod;
        // Without the script write (PuppetScriptPose=0), own and any writes
        // are the same count.
        Config config;
        config.scriptPose = false;
        PuppetState state;
        const auto observe = [&](PuppetState& s, const Config& c, bool armed, std::uint64_t writes, bool moved, bool shown) {
            return Observe(s, c, armed, writes, writes, moved, shown);
        };
        Verdict v = observe(state, config, true, 10, true, true);
        Check(!v.stale && !v.force, "lod: the first tick only learns the write count");
        v = observe(state, config, true, 11, true, true);
        Check(!v.stale, "lod: a write since the last tick is not stale");
        v = observe(state, config, true, 11, false, true);
        Check(!v.stale && !v.force, "lod: paused (playhead still) the old pose is the right one");
        v = observe(state, config, true, 11, true, true);
        Check(v.stale && v.force && !v.episode && !v.scriptPose && state.staleTicks == 1 && state.forcedTicks == 1,
            "lod: no write while playing: stale, the update forced");
        v = observe(state, config, true, 11, true, true);
        Check(v.stale && v.episode && v.skipStart && state.episodes == 1, "lod: two stale ticks in a row are a low LOD episode");
        v = observe(state, config, true, 11, true, true);
        Check(v.stale && !v.episode && !v.skipStart && state.episodes == 1, "lod: one episode until a write comes");
        v = observe(state, config, true, 12, true, true);
        Check(!v.stale && state.run == 0 && state.skipRun == 0, "lod: a write ends the run");
        v = observe(state, config, true, 12, true, false);
        Check(!v.stale, "lod: a hidden puppet is not counted");
        Config off;
        off.forceUpdate = false;
        off.scriptPose = false;
        PuppetState quiet;
        observe(quiet, off, true, 5, true, true);
        v = observe(quiet, off, true, 5, true, true);
        Check(v.stale && !v.force && quiet.forcedTicks == 0, "lod: PuppetForceAnimUpdate=0 counts without forcing");
        v = observe(quiet, off, false, 5, true, true);
        Check(!v.stale && !quiet.hasWrites, "lod: not armed forgets the write count");

        Summary summary;
        summary.Add(0x00110072u, state);
        summary.Add(0x002200C9u, quiet);
        Check(summary.staleTicks == 4 && summary.forcedTicks == 3 && summary.puppetsStale == 2 && summary.episodes == 1 &&
                summary.worstPacked == 0x00110072u && summary.scriptTicks == 0 && summary.puppetsScript == 0,
            "lod: the summary adds every puppet and keeps the worst");
    }

    // A ped at the clip start, 2 m from the camera, that the engine updates
    // one frame in three (own writes at ticks 0, 3, 6...). With the script
    // write the frames in between show the published pose: no episode, the
    // script writes on every tick while the engine skips him and
    // kScriptPoseHoldTicks after the last skip.
    void TestScriptPose()
    {
        using namespace replay::lod;
        Config config; // defaults: scriptPose on
        Check(config.scriptPose, "script pose: on by default");
        PuppetState state;
        std::uint64_t own = 100, script = 0;
        Verdict v = Observe(state, config, true, own, own + script, true, true);
        Check(!v.scriptPose && !v.stale, "script pose: a first tick learns the counts");
        std::uint32_t scriptTicks = 0, episodes = 0, skips = 0;
        for (int tick = 1; tick <= 30; ++tick)
        {
            if (tick % 3 == 0)
                ++own; // the engine's own update this frame
            v = Observe(state, config, true, own, own + script, true, true);
            if (v.scriptPose)
            {
                ++script; // ApplyOverrideNow after the publish counts in applies and scriptWrites
                ++scriptTicks;
            }
            episodes += v.episode ? 1u : 0u;
            skips += v.skipStart ? 1u : 0u;
        }
        Check(state.staleTicks == 20, "script pose: 20 of 30 ticks the engine skipped the puppet");
        Check(scriptTicks == 30 && state.scriptTicks == 30, "script pose: the script wrote from the first skip on, every tick");
        Check(episodes == 0 && state.episodes == 0, "script pose: never shown with an old pose");
        Check(skips == 10, "script pose: one anim skip run per gap of two frames");
        // The engine back to every frame: the hold runs out.
        std::uint32_t after = 0;
        for (std::uint32_t tick = 0; tick < kScriptPoseHoldTicks + 5; ++tick)
        {
            ++own;
            v = Observe(state, config, true, own, own + script, true, true);
            if (v.scriptPose)
            {
                ++script;
                ++after;
            }
        }
        // The hold counts from the last skipped tick (29): that tick and tick
        // 30 used two of its ticks.
        Check(after == kScriptPoseHoldTicks - 2 && !v.scriptPose && !v.stale,
            "script pose: kScriptPoseHoldTicks from the last skip, then the finalizer alone");
        // Paused: no write from the script, nothing counted.
        PuppetState paused;
        Observe(paused, config, true, 5, 5, true, true);
        Observe(paused, config, true, 5, 5, true, true); // stale, hold starts
        const std::uint32_t before = paused.scriptTicks;
        v = Observe(paused, config, true, 5, 6, false, true);
        Check(!v.scriptPose && !v.stale && paused.scriptTicks == before, "script pose: paused, no script write");
        // Disarmed: the hold is dropped.
        Observe(paused, config, false, 0, 0, true, true);
        v = Observe(paused, config, true, 7, 7, true, true);
        Check(!v.scriptPose && paused.scriptHold == 0, "script pose: a new arm starts without a hold");
        Summary summary;
        summary.Add(0x00000001u, state);
        Check(summary.scriptTicks == state.scriptTicks && summary.puppetsScript == 1, "script pose: summed in the summary");
    }

    void TestGameplayAngle()
    {
        using replay::lod::AngleFromForwardDeg;
        // Yaw 0 looks along +y in the game's frame.
        Check(std::fabs(AngleFromForwardDeg(0.0f, 0.0f, 0.0f, 10.0f, 0.0f)) < 0.01f, "angle: straight ahead is 0");
        Check(std::fabs(AngleFromForwardDeg(0.0f, 0.0f, 0.0f, -10.0f, 0.0f) - 180.0f) < 0.01f, "angle: behind is 180");
        Check(std::fabs(AngleFromForwardDeg(0.0f, 90.0f, -10.0f, 0.0f, 0.0f)) < 0.01f, "angle: yaw 90 looks along -x");
        Check(std::fabs(AngleFromForwardDeg(0.0f, 0.0f, 10.0f, 0.0f, 0.0f) - 90.0f) < 0.01f, "angle: to the side is 90");
        Check(std::fabs(AngleFromForwardDeg(90.0f, 0.0f, 0.0f, 0.0f, 5.0f)) < 0.01f, "angle: pitch 90 looks up");
        Check(AngleFromForwardDeg(10.0f, 20.0f, 0.0f, 0.0f, 0.0f) < 0.0f, "angle: the camera's own point");
    }

    void TestDoorRebind()
    {
        using namespace replay::doors;
        std::uint64_t next = 0;
        Check(RebindDue(1000, next, 250) && next == 1250, "doors: a rebind pass, the next one 250 ms later");
        Check(!RebindDue(1100, next, 250) && RebindDue(1250, next, 250), "doors: not before the interval");
        std::uint64_t never = 0;
        Check(!RebindDue(5000, never, 0), "doors: DoorRebindMs=0 never looks again");

        // A one-sample track is held, not skipped.
        Check(ClassifyTrack(0) == TrackPlay::Skip, "doors: no sample, nothing to play");
        Check(ClassifyTrack(1) == TrackPlay::Held, "doors: one sample is the door's state for the clip");
        Check(ClassifyTrack(2) == TrackPlay::Moving && ClassifyTrack(256) == TrackPlay::Moving, "doors: two or more move");
        Check(HeldApplyDue(false, 5000, 4900, kHeldRefreshMs), "doors: a held door is placed as soon as it is bound");
        Check(!HeldApplyDue(true, 5000, 4900, kHeldRefreshMs), "doors: not again before the refresh");
        Check(HeldApplyDue(true, 5900, 4900, kHeldRefreshMs), "doors: refreshed after kHeldRefreshMs");

        MapSettle settle;
        settle.Begin(0, 3000);
        Check(settle.Active() && !settle.Feed(0, 214, 0).done, "map: not before the minimum");
        Check(!settle.Feed(300, 145, 0).done, "map: the count still falling");
        Check(!settle.Feed(900, 147, 0).done, "map: within the tolerance, not yet held long enough");
        const MapSettle::Verdict held = settle.Feed(1100, 146, 0);
        Check(held.done && !held.timedOut && settle.EndMs() == 1100, "map: held 750 ms after the minimum: done");

        settle.Begin(0, 3000);
        settle.Feed(0, 100, 1);
        Check(!settle.Feed(1500, 100, 1).done, "map: a door near the camera still missing holds the start");
        const MapSettle::Verdict door = settle.Feed(1600, 101, 0);
        Check(door.done && !door.timedOut, "map: the door found, the count held: done");

        settle.Begin(0, 3000);
        std::uint32_t objects = 100;
        MapSettle::Verdict capped;
        for (std::uint64_t t = 0; t <= 3000; t += 100)
        {
            objects += 20; // objects keep coming back
            capped = settle.Feed(t, objects, 0);
            if (capped.done)
                break;
        }
        Check(capped.done && capped.timedOut && settle.EndMs() == 3000, "map: never stable: the cap ends the wait");

        settle.Begin(0, 0);
        Check(!settle.Active() && settle.Feed(0, 0, 3).done, "map: MapObjectsWaitMs=0 does not wait");
    }

    void TestCameraCut()
    {
        using namespace replay::camcut;
        Check(ModeFromIni("legacy") == Mode::Legacy && ModeFromIni("LEGACY") == Mode::Legacy &&
                ModeFromIni("params") == Mode::Params && ModeFromIni("") == Mode::Bits && ModeFromIni(nullptr) == Mode::Bits,
            "camera: the ini mode (bits, is the default)");
        // The bytes of 0x174AE8 in the dump 1491.50.
        const std::uint8_t code[] = {0xB8, 0xC0, 0x00, 0x00, 0x00, 0x66, 0x85, 0x05, 0x58, 0xC1, 0xD2, 0x03, 0x0F, 0x97, 0xC0, 0xC3};
        Check(FlagsRvaFromCode(code, kFinalFrameCutTestRva) == kFinalFrameFlagsRva,
            "camera: the final frame's flags from the cut test's code (0x3EA0C4C)");
        std::uint8_t other[sizeof(code)];
        std::memcpy(other, code, sizeof(code));
        other[13] = 0x94; // sete: another function
        Check(FlagsRvaFromCode(other, kFinalFrameCutTestRva) == 0, "camera: other bytes, no address");

        Tracker tracker;
        Placement p;
        p.camera = 7;
        p.realDt = 1.0f / 60.0f;
        p.position[0] = 10.0f;
        Check(Decide(tracker, p) == kWhyFirst, "camera: a new camera's first placement is a cut");
        p.position[0] = 10.3f; // 18 m/s
        p.rotation[2] = 5.0f;
        Check(Decide(tracker, p) == kWhyNone, "camera: a normal move is not a cut");
        Check(Decide(tracker, p) == kWhyNone, "camera: paused (same place) is not a cut");
        p.position[0] = 40.0f;
        Check(Decide(tracker, p) == kWhyMoved, "camera: 30 m in a tick is a cut");
        p.rotation[2] = 120.0f;
        Check(Decide(tracker, p) == kWhyTurned, "camera: a 115 degree turn in a tick is a cut");
        p.rotation[2] = -179.0f;
        Placement wrap = p;
        Decide(tracker, wrap);
        wrap.rotation[2] = 179.0f;
        Check(Decide(tracker, wrap) == kWhyNone, "camera: turning across 180 degrees is not a cut");
        wrap.forceCut = true;
        Check(Decide(tracker, wrap) == kWhyForced, "camera: the caller's cut (seek, marker without blend)");
        Placement another = wrap;
        another.forceCut = false;
        another.camera = 8;
        Check((Decide(tracker, another) & kWhyFirst) != 0 && tracker.placements == 1, "camera: another handle starts again");

        {
            Tracker zoom;
            Placement z;
            z.camera = 3;
            z.fov = 50.0f;
            Decide(zoom, z);
            z.fov = 45.0f;
            Check(Decide(zoom, z) == kWhyNone, "camera: a FOV lerp step is not a cut");
            z.fov = 25.0f;
            Check(Decide(zoom, z) == kWhyZoomed, "camera: a 20 degree FOV jump in place is a cut");
        }
        {
            // The boundary table.
            const auto frame = [](int current, int next, bool blending) {
                MarkerFrame f;
                f.valid = true;
                f.current = current;
                f.next = next;
                f.blending = blending;
                return f;
            };
            Check(!MarkerBoundaryCut(frame(0, 1, true), frame(1, 2, false)),
                "boundary: A blending to B, then B without blend (end of a chain): no cut");
            Check(!MarkerBoundaryCut(frame(0, 1, true), frame(1, -1, false)),
                "boundary: A blending to B, B the last marker: no cut");
            Check(!MarkerBoundaryCut(frame(0, 1, true), frame(1, 2, true)),
                "boundary: A blending to B, then B blending to C: no cut");
            Check(MarkerBoundaryCut(frame(0, 1, false), frame(1, 2, false)),
                "boundary: A without blend, then B without blend: cut");
            Check(MarkerBoundaryCut(frame(0, 1, false), frame(1, 2, true)),
                "boundary: A without blend, then B starting a blend: cut (the ghost frame)");
            Check(MarkerBoundaryCut(frame(0, 1, true), frame(2, 3, false)),
                "boundary: a tick that crossed a marker: cut");
            Check(!MarkerBoundaryCut(frame(1, 2, true), frame(1, 2, true)) && !MarkerBoundaryCut(frame(1, 2, false), frame(1, 2, false)),
                "boundary: the same marker: no cut");
            MarkerFrame game;
            Check(!MarkerBoundaryCut(game, frame(0, 1, false)) && !MarkerBoundaryCut(frame(0, 1, false), game),
                "boundary: to or from the game camera is the caller's cut");
        }

        const float want[3] = {1.0f, 2.0f, 3.0f}, rot[3] = {-10.0f, 0.0f, 179.5f};
        const float gotRot[3] = {-10.2f, 0.0f, -179.8f}, badRot[3] = {0.0f, 0.0f, 0.0f};
        Check(Took(want, rot, want, gotRot), "camera: the placement took (angles across 180)");
        Check(!Took(want, rot, want, badRot), "camera: a rotation that did not take");

        Rate rate;
        bool closed = false;
        for (std::uint64_t t = 0; t <= 1008; t += 16)
            closed = rate.Add(t, t == 0, t == 0) || closed;
        Check(closed && rate.frames >= 60 && rate.cuts == 1 && rate.placedCuts == 1, "camera: one cut in a second of frames");
    }

    void TestLowLodAnomaly()
    {
        replay::qa::Anomaly a;
        Check(replay::qa::ClassifyLine("POSE_V2_PUPPET_LOW_LOD,id=17:201,kind=ped,on_screen=1,camera_m=25,stale_ticks=4,"
                                       "forced=1,lod_multiplier=10,playhead_ms=31000",
                  0, a) &&
                a.kind == replay::qa::Kind::PuppetLowLod && a.entity == "17:201" && a.playheadMs == 31000 &&
                a.detail.find("stale_ticks=4") != std::string::npos,
            "qa: POSE_V2_PUPPET_LOW_LOD is puppet_low_lod");
        Check(std::string(replay::qa::KindText(replay::qa::Kind::PuppetLowLod)) == "puppet_low_lod", "qa: the kind's name");
    }
}

int RunLodDoorsCameraTests()
{
    TestPuppetLod();
    TestScriptPose();
    TestGameplayAngle();
    TestDoorRebind();
    TestCameraCut();
    TestLowLodAnomaly();
    if (g_failures == 0)
        std::puts("lod, doors and camera cut tests passed");
    return g_failures;
}
