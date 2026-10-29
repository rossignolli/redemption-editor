#pragma once

// The shake lives in the camera markers, as a replay editor usually keeps it
// (each marker's shake type, intensity and speed, from that marker on; no
// separate track). Older clips had two: each marker's shake (plus movement and
// Simple Camera's Custom numbers) and a separate shake events track (Simple
// Camera's model, camera_effects.h) that decided while its shake was on. Both
// are folded here into one per marker state (editor::MarkerShake), and an old
// clip is migrated on load without a change in what plays.
//
// The model. The shake markers are the markers the camera uses (every one
// but a plain Anchor) plus the Anchors the migration added (origin 1: the
// camera skips them, so the path never changes). At t the marker in force
// is the last shake marker at or before t; before the first one there is
// no shake. Its state plays until the next shake marker:
//   - Cut: as it is (FadeOut: a 250 ms smooth fade before the next marker);
//   - Blend: the numbers named by blendMask go linearly in time to the next
//     marker's (EaseWithCamera: the gain with the camera blend's own ease).
// The pattern's phase (cycles) is the frequency x the clock rate integrated
// from where the shake turned on, or the last Restart or NewPattern marker
// (a Restart starts from its startCycles), or the last marker that fires a
// one shot (FiresImpulse: Explosion, an impulse that settles in about 3 s,
// starts again at a marker that sets it rather than going on from a clock
// that has long run out); the fade in runs on the clock
// from the turn on or the last Restart. A profile plays at the clock
// cycles x 250 ms (4 Hz is the profile's own speed). Pure and
// deterministic: a scrub shows what the play shows.
//
// The migration (Migrate) keeps every frame: the old shake and the new one
// agree to 1e-5 (degrees and metres), sampled each frame, and the camera
// path is the same, which Verify measures (SHAKE_MIGRATION in the log, the
// tests, ReplayClipTool shake-migration).

#include "camera_track.h"

#include <cstdint>
#include <string>
#include <vector>

namespace replay::camera::mshake
{
    using editor::MarkerShake;

    // ---- The state's rules (inline: the editor add-on uses them too).

    // Every marker but a plain Anchor; an Anchor added by the migration.
    inline bool HoldsShake(const Keyframe& k)
    {
        return k.markerType != MarkerType::Anchor || k.origin == kMarkerOriginShakeMigration;
    }

    // The ranges the editor and the files hold the numbers to.
    struct Range
    {
        float minimum;
        float maximum;
    };
    constexpr Range kGainRange{0.0f, 4.0f};      // intensity 10 is 3.36
    constexpr Range kAmpRange{0.0f, 5.0f};
    constexpr Range kHzRange{0.0f, 30.0f};
    constexpr Range kCouplingRange{0.0f, 2.0f};
    constexpr Range kWeightRange{0.0f, 2.0f};
    constexpr Range kSpeedRefRange{1.0f, 100.0f};
    constexpr Range kRateRange{editor::kShakeRateMin, editor::kShakeRateMax};
    // 0 noise, 1..10 the realistic profiles, 12 Gunshot (11 is the Custom
    // type, the noise: never a profile value).
    constexpr std::uint8_t kProfileCount = 13;
    constexpr bool ValidProfile(std::uint8_t profile)
    {
        return profile < kProfileCount && profile != static_cast<std::uint8_t>(editor::ShakeType::Custom);
    }
    constexpr float kFadeOutMs = 250.0f;         // FadeOut
    constexpr double kProfileClockHz = 4.0;      // a profile's own speed

    bool Usable(const MarkerShake& s);      // finite numbers, sets in range
    MarkerShake Clamped(MarkerShake s);     // held to the ranges
    bool Same(const MarkerShake& a, const MarkerShake& b);
    // The state as the next marker starts from it: no Restart, no new
    // pattern, no offsets (the shake goes on).
    MarkerShake Continued(MarkerShake s);

    // The 1..10 intensity and its gain ((i + 1) / 6)^2.
    float GainOf(std::uint8_t intensity);
    std::uint8_t IntensityOf(float gain); // the nearest, 1..10
    // The snapshot's old ShakeType field: None, the profile, or Custom (the noise).
    editor::ShakeType SummaryType(const MarkerShake& s);

    // Simple Camera's presets (the noise's numbers): Subtle, Handheld,
    // Vehicle, Earthquake (camera_effects.h kPresets 1..4).
    constexpr std::uint8_t kPresetCount = 4;
    const char* PresetName(std::uint8_t preset);
    void ApplyPreset(MarkerShake& s, std::uint8_t preset);
    // The preset whose numbers the state has (noise only), 0xFF none.
    std::uint8_t MatchingPreset(const MarkerShake& s);
    // A profile (1..10, 12): amp 1, 4 Hz, the shake on.
    void ApplyProfile(MarkerShake& s, std::uint8_t profile);
    // "Handheld", "Simple: Earthquake", "Simple: Custom", "Off".
    std::string Describe(const MarkerShake& s);

    // ---- Evaluation.

    struct Resolved
    {
        bool active = false;
        int marker = -1;            // index of the shake marker in force (-1: none)
        int next = -1;              // the next shake marker
        std::uint8_t profile = 0;   // 0: the noise with `custom`
        float gain = 0.0f;          // the intensity's gain x the fade out
        editor::ShakeCustom custom = editor::DefaultShakeCustom(); // amp, hz, couplings, weights, speedRef, seed, still
        double cycles = 0.0;        // the pattern's phase (the speed coupling is added by Delta)
        double fadeMs = 0.0;        // the fade in's clock
        std::uint64_t originMs = 0; // where the phase started (the coupling's grid)
        bool blending = false;
    };
    // Whether a shake marker fires its one shot afresh (the phase starts at
    // it, from 0 or a Restart's startCycles): it holds a one shot profile
    // (shake::OneShot) and is not the plain continuation of the shake marker
    // before (`previous`, null for none), that is the same profile, rate and
    // Real Time, and every number either the same or one `previous` blends
    // into it. A marker added inside the shake (HeldAt) is a continuation;
    // a marker switched to Explosion, or given another intensity, fires.
    bool FiresImpulse(const MarkerShake* previous, const MarkerShake& s);

    // The shake's state at t (no camera speed: that is Delta's).
    Resolved Resolve(const Track& track, std::uint64_t timeMs);
    // The shake the markers add at t (degrees and world metres).
    ShakeDelta Delta(const Track& track, std::uint64_t timeMs, const Scene& scene);
    // The state in force at t as a marker would hold it (blended numbers,
    // no restart): what a new marker added there starts from. Off when no
    // shake plays.
    MarkerShake HeldAt(const Track& track, std::uint64_t timeMs);

    // The OLD rules: the events track while its shake is on, else the old
    // marker shake (Keyframe::legacyShake). Kept for the migration's proof; a
    // migrated track has neither.
    ShakeDelta LegacyDelta(const Track& track, std::uint64_t timeMs, const Scene& scene);

    // ---- Migration.

    // Folds the old marker shake and the old events track into the markers,
    // clears them and fills track.Migration() (its `before`: the track as it
    // was). Nothing when there is nothing old.
    void Migrate(Track& track);

    struct Proof
    {
        std::uint32_t samples = 0;
        double shakeRotation = 0.0; // the largest difference of the shake, degrees
        double shakePosition = 0.0; // metres
        double pathPosition = 0.0;  // the camera without the shake, metres
        double pathRotation = 0.0;  // degrees
        double pathFov = 0.0;
        double cameraRotation = 0.0; // the rendered camera (path and shake), degrees: float precision of the pose
        double cameraPosition = 0.0;
        double peakRotation = 0.0; // the largest shake the old rules gave (the proof is not about nothing)
        double peakPosition = 0.0;
        std::uint64_t worstMs = 0;  // where the shake differs most
        // Not run (the public build: the proof is the developer build's, its
        // cost the declared duration's; FormatLine then gives the counts only).
        bool skipped = false;
        bool Exact(double tolerance = 1e-5) const
        {
            return shakeRotation <= tolerance && shakePosition <= tolerance && pathPosition <= tolerance &&
                pathRotation <= tolerance && pathFov <= tolerance;
        }
    };
    // The old track and the migrated one compared every stepMs (0: each 30 Hz
    // frame) from 0 to durationMs and at every marker (and 1 ms either side):
    // the shake (the camera with it minus without it) and the path. Memory is
    // the markers' (the grid is walked, not stored); the time is the
    // duration's, so the game runs it in the developer build only.
    Proof Verify(const Track& before, const Track& after, const Scene& scene, std::uint64_t durationMs, std::uint32_t stepMs = 0);

    // "SHAKE_MIGRATION,legacy_markers=..,events=..,on_markers=..,
    // breakpoints=..,markers_added=..,pruned=..,inexact=..,samples=..,
    // max_rot_deg=..,max_pos_m=..,max_path_m=..,max_path_deg=..,max_cam_deg=..,
    // max_cam_m=..,peak_rot_deg=..,peak_pos_m=..,worst_ms=..,exact=0|1" (max_rot/pos: the shake each adds;
    // max_cam: the rendered camera, rounded by its float pose). A skipped proof:
    // the counts up to inexact, then ",proof=skipped".
    std::string FormatLine(const ShakeMigrationReport& report, const Proof& proof);

    // ---- The director's and the bench's marker fields (shake keys of a
    // marker line), and a marker's shake as such a line's fields.
    //   shake=off|<profile name>|subtle|simple_handheld|vehicle|earthquake|noise
    //   shake_i=1..10 shake_gain= shake_amp= shake_hz= shake_spdamp=
    //   shake_spdhz= shake_rot= shake_pos= shake_ref= shake_still=0|1
    //   shake_seed= shake_rate=0.01..40 shake_speed=5..200 (percent)
    //   shake_realtime=0|1 shake_transition=cut|blend shake_restart=0|1
    //   shake_motion=off|speed|still (the shake "with movement")
    // The old keys (shake=custom with shake_amp ...) read the same way.
    // `known` false when the key is not a shake key.
    bool ParseField(const std::string& key, const std::string& value, MarkerShake& s, bool& known, std::string& error);
    std::string FormatFields(const MarkerShake& s);
}
