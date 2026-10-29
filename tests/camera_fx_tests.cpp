// The shake events track, Simple Camera's model: each event sets one parameter
// (kind) to a float value, snap or ramp from the event before of the same kind.
// The fold, the ramps by kind, Preset (the mod's table), Randomize, On/Off back
// to the markers, determinism, Profile, the edits, the parser and the list
// line, the CFXE v1/v2 conversion, the CFXE payload and the project round
// trips.
#include "camera_effects.h"
#include "camera_marker_shake.h"
#include "camera_shake.h"
#include "camera_track.h"
#include "clip_file.h"
#include "replay_project.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using namespace replay;
    using camera::fx::EffectTrack;
    using camera::fx::Kind;
    using camera::fx::Mode;
    using camera::fx::ShakeEvent;
    using editor::ShakeType;
    namespace fx = camera::fx;

    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL (camera-fx): %s\n", message);
        ++g_failures;
    }

    bool Near(double a, double b, double tolerance = 0.001) { return std::fabs(a - b) <= tolerance; }

    ShakeEvent Event(std::uint64_t t, Kind kind, float value, Mode mode = Mode::Snap)
    {
        ShakeEvent e;
        e.timeMs = t;
        e.kind = kind;
        e.value = value;
        e.mode = mode;
        return e;
    }

    // A list as the mod prints it: three events at t=0.00.
    void TestFold()
    {
        EffectTrack track;
        Check(!fx::StateAt(track, 0).enabled && fx::StateAt(track, 0).hz == 4.0f && fx::StateAt(track, 0).speedHz == 0.5f,
            "no events: the mod's defaults, off");
        track.Add(Event(0, Kind::Amp, 1.5f));
        track.Add(Event(0, Kind::Freq, 0.3f));
        track.Add(Event(0, Kind::OnOff, 1.0f));
        Check(track.Events().size() == 3, "three events at one time");
        const fx::State s = fx::StateAt(track, 0);
        Check(s.enabled && s.amp == 1.5f && Near(s.hz, 0.3) && s.onSinceMs == 0.0, "the fold at t=0");
        const fx::FxShake shake = fx::Evaluate(track, 2000);
        Check(shake.active && shake.profile == ShakeType::None && shake.custom.amp == 1.5f && Near(shake.cycles, 0.6, 1e-6),
            "Evaluate: Simple Camera's noise, 0.3 Hz for 2 s = 0.6 cycles");
        Check(fx::FormatListLine(0, Kind::Amp, 1.5f, Mode::Snap) == "t=0.00 Shake Amp=1.50 snap" &&
                  fx::FormatListLine(1234, Kind::OnOff, 1.0f, Mode::Ramp) == "t=1.23 Shake On/Off=1.00 ramp",
            "the list line reads like the mod's");
        // A later event of one kind leaves the others as they are.
        track.Add(Event(3000, Kind::Amp, 0.5f));
        const fx::State later = fx::StateAt(track, 3500);
        Check(later.amp == 0.5f && Near(later.hz, 0.3) && later.enabled, "one kind changes, the rest held");
        Check(fx::ValueOf(later, Kind::Amp) == 0.5f && fx::ValueOf(later, Kind::OnOff) == 1.0f, "ValueOf");
    }

    void TestRamps()
    {
        EffectTrack track;
        track.Add(Event(1000, Kind::Amp, 0.0f));
        track.Add(Event(1500, Kind::Freq, 2.0f));           // another kind in between
        track.Add(Event(3000, Kind::Amp, 2.0f, Mode::Ramp)); // from the Amp at 1000
        track.Add(Event(2000, Kind::RotWeight, 0.5f, Mode::Ramp)); // no RotWeight before: acts as snap
        const fx::State mid = fx::StateAt(track, 2000);
        Check(Near(mid.amp, 1.0) && mid.ramping, "ramp: from the event before of the same kind (halfway)");
        Check(Near(fx::StateAt(track, 1999).rotWeight, 1.0) && Near(fx::StateAt(track, 2000).rotWeight, 0.5),
            "ramp with no event before: a snap at its time");
        Check(Near(fx::StateAt(track, 3000).amp, 2.0) && !fx::StateAt(track, 3001).ramping, "ramp ends at its event");
        Check(Near(fx::StateAt(track, 999).amp, 0.0) && !fx::StateAt(track, 999).ramping, "before the first: the default");
        // A switch ramps by crossing its half step: On/Off 0 -> 1 over 1..3 s
        // turns on at 2 s (and the phase starts there).
        EffectTrack onRamp;
        onRamp.Add(Event(1000, Kind::OnOff, 0.0f));
        onRamp.Add(Event(3000, Kind::OnOff, 1.0f, Mode::Ramp));
        Check(!fx::StateAt(onRamp, 1900).enabled && fx::StateAt(onRamp, 2100).enabled &&
                  Near(fx::StateAt(onRamp, 2500).onSinceMs, 2000.0),
            "On/Off ramp switches halfway");
        // The frequency ramp is integrated exactly (trapezoid on the line).
        EffectTrack hz;
        hz.Add(Event(0, Kind::OnOff, 1.0f));
        hz.Add(Event(0, Kind::Freq, 4.0f));
        hz.Add(Event(2000, Kind::Freq, 8.0f, Mode::Ramp));
        Check(Near(fx::Evaluate(hz, 1000).cycles, 5.0, 1e-6) && Near(fx::Evaluate(hz, 3000).cycles, 12.0 + 8.0, 1e-6),
            "Freq ramp: 4..6 Hz over 1 s = 5 cycles; 12 by 2 s, then 8 Hz");
    }

    void TestPreset()
    {
        for (std::uint8_t p = 0; p < fx::kPresetCount; ++p)
        {
            EffectTrack track;
            track.Add(Event(100, Kind::Preset, static_cast<float>(p)));
            const fx::State s = fx::StateAt(track, 200);
            const fx::PresetValues& v = fx::kPresets[p];
            Check(s.amp == v.amp && s.hz == v.hz && s.speedAmp == v.speedAmp && s.speedHz == v.speedHz, "Preset: the mod's table");
            Check(s.enabled == (p > 0), "Preset: Off leaves the shake off, the others turn it on");
        }
        Check(fx::kPresets[3].amp == 0.7f && fx::kPresets[3].hz == 6.0f && fx::kPresets[4].amp == 1.5f && fx::kPresets[4].hz == 10.0f &&
                  std::strcmp(fx::PresetName(2), "Handheld") == 0,
            "Preset: Vehicle 0.7/6 Hz, Earthquake 1.5/10 Hz, names");
        // An Amp after a Preset overrides it; a Preset after an Amp resets it.
        EffectTrack track;
        track.Add(Event(0, Kind::Preset, 2.0f));
        track.Add(Event(500, Kind::Amp, 1.2f));
        track.Add(Event(900, Kind::Preset, 1.0f));
        Check(fx::StateAt(track, 600).amp == 1.2f && fx::StateAt(track, 1000).amp == fx::kPresets[1].amp, "Preset and Amp in order");
        ShakeEvent named;
        std::string error;
        Check(fx::ParseEventFields({"kind=preset", "value=earthquake"}, named, error) && named.value == 4.0f, "Preset by name");
    }

    void TestRandomize()
    {
        EffectTrack track;
        track.Add(Event(0, Kind::OnOff, 1.0f));
        track.Add(Event(0, Kind::Amp, 1.0f));
        const std::uint32_t r = track.Add(Event(2000, Kind::Randomize, 77.0f, Mode::Ramp));
        Check(r && track.Find(r)->mode == Mode::Snap, "Randomize is always Snap");
        Check(!fx::Valid(Event(0, Kind::Randomize, 1.0f, Mode::Ramp)), "a Randomize ramp is not valid");
        Check(fx::StateAt(track, 1999).seed == 0 && fx::StateAt(track, 2000).seed == 77, "Randomize sets the seed");
        const fx::FxShake after = fx::Evaluate(track, 2500);
        Check(after.runStartMs == 2000 && Near(after.cycles, 2.0, 1e-6) && after.custom.seed == 77,
            "Randomize starts the phase again (4 Hz for 0.5 s = 2 cycles)");
        Check(Near(after.sinceOnMs, 2500.0), "the fade in counts from the turn on, not the Randomize");
    }

    camera::Track MarkersWithShake()
    {
        camera::Track track;
        camera::Keyframe a;
        a.timeMs = 0;
        a.cameraMode = editor::CameraMode::Free;
        a.blend = editor::Blend::Linear;
        a.ease = false;
        a.legacyShake.type = ShakeType::Running;
        a.pose = {{0, 0, 0}, camera::Track::FromEulerDegrees({0, 0, 0}), 45.0f};
        camera::Keyframe b = a;
        b.timeMs = 6000;
        b.pose.position = {10, 0, 0};
        track.AddMarker(a);
        track.AddMarker(b);
        return track;
    }

    void TestOnOffAndMarkers()
    {
        camera::Track plain = MarkersWithShake();
        camera::Track track = plain;
        track.LegacyEffects().Add(Event(2000, Kind::Preset, 4.0f)); // Earthquake: on
        track.LegacyEffects().Add(Event(4000, Kind::OnOff, 0.0f));  // off: the markers again
        editor::CameraState x{}, y{};
        camera::EvaluateMarkers(track, 1500, {}, x);
        camera::EvaluateMarkers(plain, 1500, {}, y);
        Check(x.position.x == y.position.x && x.rotation.z == y.rotation.z, "before the track turns on: the markers' shake");
        camera::EvaluateMarkers(track, 2600, {}, x);
        camera::EvaluateMarkers(plain, 2600, {}, y);
        Check(std::fabs(x.rotation.x - y.rotation.x) + std::fabs(x.rotation.z - y.rotation.z) > 0.01f, "on: the track decides");
        Check(fx::Evaluate(track.LegacyEffects(), 3000).active && !fx::Evaluate(track.LegacyEffects(), 4500).active, "On/Off 0 turns it off");
        camera::EvaluateMarkers(track, 4500, {}, x);
        camera::EvaluateMarkers(plain, 4500, {}, y);
        Check(x.position.x == y.position.x && x.rotation.x == y.rotation.x && x.rotation.z == y.rotation.z,
            "off again: the markers' shake plays");
        Check(!camera::SameMarkers(track, plain), "the events are part of the markers (undo, edited)");
        camera::Track cleared = track;
        cleared.Clear();
        Check(cleared.LegacyEffects().Empty(), "Clear() empties the events too");
    }

    void TestDeterminism()
    {
        camera::Track track = MarkersWithShake();
        track.LegacyEffects().Add(Event(0, Kind::Amp, 0.8f));
        track.LegacyEffects().Add(Event(0, Kind::OnOff, 1.0f));
        track.LegacyEffects().Add(Event(1000, Kind::Freq, 9.0f, Mode::Ramp));
        track.LegacyEffects().Add(Event(1500, Kind::SpeedFreq, 1.5f));
        track.LegacyEffects().Add(Event(2500, Kind::Randomize, 5.0f));
        track.LegacyEffects().Add(Event(3000, Kind::Profile, 1.0f)); // Handheld from 3 s
        // Scrub: the same time gives the same camera whatever came before.
        editor::CameraState fresh{}, scrubbed{}, other{};
        const camera::Track copy = track;
        camera::EvaluateMarkers(copy, 2750, {}, fresh);
        for (std::uint64_t t : {5000ull, 100ull, 2900ull, 1200ull})
            camera::EvaluateMarkers(track, t, {}, other);
        camera::EvaluateMarkers(track, 2750, {}, scrubbed);
        Check(std::memcmp(&fresh, &scrubbed, sizeof(fresh)) == 0, "scrub equals play (after other times)");
        const fx::FxShake once = fx::Evaluate(track.LegacyEffects(), 1234), twice = fx::Evaluate(track.LegacyEffects(), 1234);
        Check(std::memcmp(&once.custom, &twice.custom, sizeof(editor::ShakeCustom)) == 0 && once.cycles == twice.cycles,
            "Evaluate is pure");
        // Profile: the generator is the profile, Freq its clock (4 Hz = 100 %).
        const fx::FxShake p = fx::Evaluate(track.LegacyEffects(), 3200);
        Check(p.active && p.profile == ShakeType::Handheld && Near(fx::ProfileClockMs(4.0), 1000.0), "Profile and its clock");
        camera::EvaluateMarkers(track, 3200, {}, other);
        editor::CameraState raw{};
        camera::EvaluateMarkersRaw(track, 3200, {}, raw);
        Check(std::fabs(other.rotation.x - raw.rotation.x) + std::fabs(other.rotation.z - raw.rotation.z) > 1e-4f,
            "Profile shakes the camera");
        // Rot Weight 0 leaves the rotation alone with a profile too.
        track.LegacyEffects().Add(Event(3100, Kind::RotWeight, 0.0f));
        camera::EvaluateMarkers(track, 3200, {}, other);
        Check(std::fabs(other.rotation.x - raw.rotation.x) + std::fabs(other.rotation.y - raw.rotation.y) +
                      std::fabs(other.rotation.z - raw.rotation.z) < 1e-5f,
            "Rot Weight 0: no rotation from the profile");
    }

    void TestEdits()
    {
        EffectTrack track;
        const std::uint32_t a = track.Add(Event(1000, Kind::Amp, 1.0f));
        Check(a != 0, "add");
        Check(track.Add(Event(1000, Kind::Freq, 2.0f)) != 0, "events may share a time (the mod's three at t=0)");
        Check(track.Find(track.Add(Event(1500, Kind::Amp, 9.0f)))->value == 5.0f, "a value is held in its kind's range");
        Check(track.Add(Event(1500, static_cast<Kind>(11), 1.0f)) == 0, "an unknown kind refused");
        const std::uint32_t b = track.Add(Event(500, Kind::Profile, 3.4f));
        Check(track.Events().front().id == b && track.Find(b)->value == 3.0f, "kept sorted; an index rounded");
        Check(track.Move(b, 4000) && track.Events().back().id == b, "move, sorted again");
        Check(track.Update(a, Event(0, Kind::PosWeight, 0.25f, Mode::Ramp)) && track.Find(a)->timeMs == 1000 &&
                  track.Find(a)->kind == Kind::PosWeight && track.Find(a)->mode == Mode::Ramp,
            "update: new kind, value and mode, the time kept");
        Check(track.Remove(a) && !track.Find(a), "remove");
        std::vector<ShakeEvent> bad{Event(100, Kind::Amp, 1.0f), Event(110, Kind::Amp, 7.0f)};
        bad[0].id = 1;
        bad[1].id = 2;
        Check(!track.Assign(bad, 3) && track.Events().size() == 3, "assign refuses a value out of range");
        std::vector<ShakeEvent> good{Event(100, Kind::Amp, 1.0f), Event(100, Kind::OnOff, 1.0f)};
        good[0].id = 1;
        good[1].id = 2;
        Check(track.Assign(good, 3) && track.Events().size() == 2 && track.NextId() == 3, "assign");
    }

    void TestParse()
    {
        ShakeEvent e;
        std::string error;
        Check(fx::ParseEventFields({"t=43900", "kind=amp", "value=1.5", "mode=snap"}, e, error) && e.timeMs == 43900 &&
                  e.kind == Kind::Amp && e.value == 1.5f && e.mode == Mode::Snap,
            "parse an event line");
        Check(fx::FormatEventFields(e) == "t=43900 kind=amp value=1.500 mode=snap", "format an event line");
        Check(fx::ParseEventFields({"kind=profile", "value=horseback"}, e, error) && e.value == 8.0f, "Profile by name");
        Check(fx::ParseEventFields({"kind=onoff", "value=on"}, e, error) && e.value == 1.0f, "On/Off by name");
        Check(!fx::ParseEventFields({"kind=quake"}, e, error) && !error.empty(), "unknown kind refused");
        Check(!fx::ParseEventFields({"kind=freq", "value=31"}, e, error), "Freq above 30 refused");
        Check(!fx::ParseEventFields({"shake=handheld"}, e, error), "the old fields are not the new ones");
        Check(fx::IsLegacyLine({"t=10", "shake=handheld", "shake_i=4"}) && !fx::IsLegacyLine({"t=10", "kind=amp", "value=1"}),
            "an old line is told apart");
        fx::LegacyEvent old;
        Check(fx::ParseLegacyFields({"t=10", "shake=explosion", "shake_i=8", "shake_speed=150", "motion=speed", "mode=ramp"}, old, error) &&
                  old.timeMs == 10 && old.shake == ShakeType::Explosion && old.intensity == 8 && old.speedIndex == 6 &&
                  old.motion == editor::ShakeMotion::Speed && old.mode == Mode::Ramp,
            "parse an old line");
    }

    float StateValue(const EffectTrack& track, double t, Kind kind) { return fx::ValueOf(fx::StateAt(track, t), kind); }

    void TestConvertLegacy()
    {
        // Handheld 5 at 1 s, Handheld 8 ramped by 3 s, None at 5 s, Custom at 6 s.
        std::vector<fx::LegacyEvent> old(4);
        old[0].timeMs = 1000;
        old[0].shake = ShakeType::Handheld;
        old[0].intensity = 5;
        old[1] = old[0];
        old[1].timeMs = 3000;
        old[1].intensity = 8;
        old[1].mode = Mode::Ramp;
        old[2].timeMs = 5000;
        old[2].shake = ShakeType::None;
        old[3].timeMs = 6000;
        old[3].shake = ShakeType::Custom;
        old[3].intensity = 5;
        old[3].speedIndex = 8; // 200 %
        old[3].custom = editor::DefaultShakeCustom();
        old[3].custom.amp = 0.5f;
        old[3].custom.hz = 3.0f;
        old[3].custom.still = 1;
        old[3].custom.seed = 42;
        const std::vector<ShakeEvent> events = fx::ConvertLegacy(old);
        EffectTrack track;
        Check(track.Assign(events, static_cast<std::uint32_t>(events.size()) + 1), "converted events are valid");
        Check(!fx::StateAt(track, 900).enabled, "before the first old event: off");
        const fx::State a = fx::StateAt(track, 1000);
        Check(a.enabled && a.profile == static_cast<std::uint8_t>(ShakeType::Handheld) && Near(a.amp, camera::shake::Gain(5)) &&
                  Near(a.hz, 4.0) && a.speedAmp == 0.0f,
            "a profile: Profile, Amp its gain, Freq 4 Hz x 100 %, no speed coupling");
        Check(Near(StateValue(track, 2000, Kind::Amp), (camera::shake::Gain(5) + camera::shake::Gain(8)) * 0.5),
            "an old ramp of the same type ramps Amp");
        Check(!fx::StateAt(track, 5500).enabled, "None: off");
        const fx::State c = fx::StateAt(track, 6000);
        Check(c.enabled && c.profile == 0 && Near(c.amp, 0.5 * camera::shake::Gain(5)) && Near(c.hz, 6.0) && c.still && c.seed == 42,
            "Custom: Profile 0 with its numbers (Freq x the speed), Stop When Still, Randomize");
        // Only what changes is written: the second Handheld writes Amp alone.
        int at3000 = 0;
        for (const ShakeEvent& e : events)
            at3000 += e.timeMs == 3000 ? 1 : 0;
        Check(at3000 == 1, "only what changes");
    }

    void TestFormats()
    {
        camera::Track track;
        track.LegacyEffects().Add(Event(0, Kind::Amp, 1.5f));
        track.LegacyEffects().Add(Event(0, Kind::Freq, 0.3f));
        track.LegacyEffects().Add(Event(0, Kind::OnOff, 1.0f));
        track.LegacyEffects().Add(Event(2500, Kind::Amp, 0.25f, Mode::Ramp));
        const std::vector<std::uint8_t> bytes = clipfile::EncodeShakeEvents(track);
        camera::Track back;
        std::string error;
        Check(clipfile::ShakeEventsSectionVersion() == 3 &&
                  clipfile::DecodeShakeEvents(bytes.data(), bytes.size(), 3, back, error) && back.LegacyEffects() == track.LegacyEffects(),
            "CFXE v3 payload round trip");
        Check(bytes.size() == 4 + 8 + 4 * 14, "CFXE v3: 14 bytes an event");
        Check(!clipfile::DecodeShakeEvents(bytes.data(), bytes.size(), 4, back, error), "an unknown CFXE version is refused");

        // v1 and v2 payloads as they were written.
        const auto put = [](std::vector<std::uint8_t>& out, const auto& value) {
            const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
            out.insert(out.end(), p, p + sizeof(value));
        };
        const auto oldEvent = [&](std::vector<std::uint8_t>& out, std::uint32_t id, std::uint32_t t, ShakeType shake,
                                  std::uint8_t intensity, bool v2, const editor::ShakeCustom& c) {
            put(out, id);
            put(out, t);
            put(out, static_cast<std::uint8_t>(shake));
            put(out, intensity);
            put(out, static_cast<std::uint8_t>(4)); // 100 %
            put(out, static_cast<std::uint8_t>(0)); // motion off
            put(out, static_cast<std::uint8_t>(0)); // snap
            if (v2)
            {
                put(out, c.amp);
                put(out, c.hz);
                put(out, c.speedAmp);
                put(out, c.speedHz);
                put(out, c.rotWeight);
                put(out, c.posWeight);
                put(out, c.speedRef);
                put(out, c.seed);
                put(out, c.still);
                put(out, static_cast<std::uint8_t>(0));
            }
        };
        editor::ShakeCustom c = editor::DefaultShakeCustom();
        c.amp = 1.0f;
        c.hz = 7.0f;
        std::vector<std::uint8_t> v1;
        put(v1, std::uint32_t{3});
        put(v1, std::uint64_t{2});
        oldEvent(v1, 1, 200, ShakeType::Wagon, 5, false, c);
        oldEvent(v1, 2, 800, ShakeType::None, 5, false, c);
        camera::Track fromV1;
        Check(clipfile::DecodeShakeEvents(v1.data(), v1.size(), 1, fromV1, error) &&
                  fx::StateAt(fromV1.LegacyEffects(), 300).profile == static_cast<std::uint8_t>(ShakeType::Wagon) &&
                  fx::StateAt(fromV1.LegacyEffects(), 300).enabled && !fx::StateAt(fromV1.LegacyEffects(), 900).enabled,
            "CFXE v1 converted (Wagon, then None)");
        std::vector<std::uint8_t> v2;
        put(v2, std::uint32_t{2});
        put(v2, std::uint64_t{1});
        oldEvent(v2, 1, 400, ShakeType::Custom, 5, true, c);
        camera::Track fromV2;
        Check(clipfile::DecodeShakeEvents(v2.data(), v2.size(), 2, fromV2, error) &&
                  fx::StateAt(fromV2.LegacyEffects(), 500).profile == 0 && Near(fx::StateAt(fromV2.LegacyEffects(), 500).hz, 7.0),
            "CFXE v2 converted (Custom numbers)");
        Check(!clipfile::DecodeShakeEvents(v2.data(), v2.size(), 1, fromV2, error), "a v2 payload read as v1 is refused");

        // The project keeps them with the clip's markers (the same payload).
        project::Project p;
        p.name = "Fx";
        project::Clip clip;
        clip.file = "001-Test.rpclip";
        clip.rawDurationMs = 10000;
        clip.inMs = 0;
        clip.outMs = 10000;
        clip.hasMarkers = true;
        camera::Keyframe k;
        k.timeMs = 0;
        clip.markers.AddMarker(k);
        k.timeMs = 10000;
        clip.markers.AddMarker(k);
        clip.markers.LegacyEffects() = track.LegacyEffects();
        p.clips.push_back(clip);
        const std::vector<std::uint8_t> encoded = project::Encode(p);
        project::Project loaded;
        // Folded into the clip's markers (the project keeps no events): the
        // same shake on and off.
        bool folded = project::Decode(encoded.data(), encoded.size(), loaded, error) && loaded.clips.size() == 1 &&
            loaded.clips[0].markers.LegacyEffects().Empty();
        for (std::uint64_t t = 0; folded && t <= 10000; t += 250)
            folded = camera::mshake::Resolve(loaded.clips[0].markers, t).active == fx::Evaluate(track.LegacyEffects(), t).active;
        Check(folded, "project round trip keeps the shake events (folded into the markers)");

        // A project saved with a CFXE v2 clip opens converted: the last
        // entry's tail rewritten to version 2 and the v2 payload, the entry
        // size and the file hash fixed. A project saved now has no tail; the
        // old one is the entry with the tail appended.
        std::vector<std::uint8_t> old = encoded;
        const std::size_t end = old.size() - 8;
        const std::size_t tail = end;
        bool found = true;
        std::size_t sizeAt = 0;
        // The first match, the entry's own size (CAMR 8 made the entry 40
        // bytes longer and a u32 inside the markers can match too: the last
        // match is not the size).
        for (std::size_t at = 0; found && at + 4 <= tail && !sizeAt; ++at)
        {
            std::uint32_t entry = 0;
            std::memcpy(&entry, old.data() + at, 4);
            if (at + 4 + entry == end)
                sizeAt = at;
        }
        Check(found && sizeAt, "project: the clip's CFXE tail found");
        if (found && sizeAt)
        {
            old.resize(tail);
            put(old, std::uint16_t{2});
            put(old, static_cast<std::uint64_t>(v2.size()));
            old.insert(old.end(), v2.begin(), v2.end());
            const std::uint32_t entry = static_cast<std::uint32_t>(old.size() - sizeAt - 4);
            std::memcpy(old.data() + sizeAt, &entry, 4);
            std::uint64_t hash = 0xCBF29CE484222325ull;
            for (std::uint8_t b : old)
            {
                hash ^= b;
                hash *= 0x100000001B3ull;
            }
            put(old, hash);
            project::Project fromOld;
            Check(project::Decode(old.data(), old.size(), fromOld, error) && fromOld.clips.size() == 1 &&
                      camera::mshake::Resolve(fromOld.clips[0].markers, 500).active &&
                      Near(camera::mshake::Resolve(fromOld.clips[0].markers, 500).custom.hz, 7.0),
                "project with CFXE v2 opens, converted and folded into the markers");
        }
    }
}

int RunCameraFxTests()
{
    TestFold();
    TestRamps();
    TestPreset();
    TestRandomize();
    TestOnOffAndMarkers();
    TestDeterminism();
    TestEdits();
    TestParse();
    TestConvertLegacy();
    TestFormats();
    if (g_failures == 0)
        std::puts("camera-fx (shake events by parameter: fold, ramps, preset, randomize, markers, determinism, v1/v2, formats) tests passed");
    return g_failures;
}
