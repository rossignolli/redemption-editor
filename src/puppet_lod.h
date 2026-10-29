#pragma once

// The ped and horse puppets kept at full detail and with their skeleton
// updated while the replay plays them.
//
// The engine can draw a puppet at a low LOD (e.g. a hat as a white low polygon
// block above the head) and, in those frames, the pose finalizer does not run
// for it (the replay's pose is not written: POSE_JERK causes=not_written), so
// the puppet blinks and jumps. Rung 1, on the replay's own puppets:
//  - SET_PED_LOD_MULTIPLIER (0xDC2C5C242AAC342B; handler 0xFBEDFC ->
//    0xFED924, writes the multiplier and the entity's LOD distance
//    multiplier x 100) high, so the puppet keeps its high LOD far away;
//  - SET_ENTITY_ALWAYS_PRERENDER (0xACAD101E1FB66689; 0xF8E744 -> 0xF9EFB8,
//    the global list 0x3F2FB50): the pre-render (skeleton matrices for the
//    draw) runs even off screen;
//  - FORCE_PED_AI_AND_ANIMATION_UPDATE (0x2208438012482A1A; 0xFB3970 ->
//    0xFAF2C0 -> 0xFCC720, reset flags of the ped's update) on a tick that
//    finds no pose write since the last one (the frame in between rendered
//    an old pose).
// Undone when the puppet is deleted (prerender off before the entity goes,
// so the global list never keeps it; multiplier back to 1).
//
// No native tells the draw LOD chosen for a frame. A skipped finalizer is the
// engine skipping the puppet's animation update (the finalizer is reached only
// from the motion tree: no update, no call), not the draw LOD: it happens at
// 2 m from the camera too, far more often when the hidden player stays far
// from the scene, and FORCE_PED_AI_AND_ANIMATION_UPDATE on a stale tick does
// not make the next tick write. As a replay writes the ped's skeleton every
// frame whatever the animation passes do, a puppet the engine skipped gets the
// published pose from the script thread every tick for kScriptPoseHoldTicks
// after its last skipped tick (ApplyOverrideNow, the guarded write already
// used at every arm and every tick for vehicles). Two counts: own writes (the
// finalizer's: the engine updated the puppet) and any writes (own plus the
// script's). A stale tick is a tick without an own write; an episode
// (puppet_low_lod: the puppet shown with an old pose, on screen, for
// kLowLodRunTicks ticks in a row while the playhead moved; one
// POSE_V2_PUPPET_LOW_LOD line per episode) needs a run of ticks without any
// write.
//
// Pure: unit tested in tests/lod_doors_camera_tests.cpp.

#include <cmath>
#include <cstdint>

namespace replay::lod
{
    struct Config
    {
        std::uint32_t multiplier = 10; // PuppetLodMultiplier (0: native not called)
        bool prerender = true;         // PuppetAlwaysPrerender
        bool forceUpdate = true;       // PuppetForceAnimUpdate
        bool scriptPose = true;        // PuppetScriptPose
        // PuppetAnimLod0: the puppet's anim director gets LOD 0 and visible
        // through the detour of 0x2BD410C (anim_lod_hook.h), as the recorded
        // ped had while it was recorded. The skeleton is the replay's anyway;
        // what this keeps is the expressions that write more than bones (the
        // animated normal maps of a horse's muscles, track 0x17), which the
        // engine switches on and off with the effective LOD of the frame.
        bool animLod0 = true;
    };

    // Ticks the script write goes on after the last tick the engine skipped
    // the puppet (a timesliced puppet skips one or two frames in three).
    inline constexpr std::uint32_t kScriptPoseHoldTicks = 90;

    // Ticks in a row without a pose write that make a low LOD episode.
    inline constexpr std::uint32_t kLowLodRunTicks = 2;

    struct PuppetState
    {
        int clone = 0;             // the entity the settings were applied to (0: none)
        bool hasWrites = false;
        std::uint64_t lastWrites = 0;   // own writes (the finalizer's)
        bool hasAny = false;
        std::uint64_t lastAny = 0;      // own plus script writes
        std::uint32_t scriptHold = 0;   // ticks of script writes left
        std::uint32_t scriptTicks = 0;  // ticks the script wrote the pose
        std::uint32_t skipRun = 0;      // stale ticks in a row (own writes)
        bool inSkip = false;
        std::uint32_t staleTicks = 0;   // ticks that found no own write since the last one
        std::uint32_t forcedTicks = 0;  // FORCE_PED_AI_AND_ANIMATION_UPDATE calls
        std::uint32_t run = 0;          // ticks without any write in a row
        std::uint32_t episodes = 0;
        bool inEpisode = false;
    };

    struct Verdict
    {
        bool stale = false;      // no own write since the last tick (the engine skipped the puppet)
        bool force = false;
        bool scriptPose = false; // write the published pose from the script thread this tick
        bool skipStart = false;  // a run of kLowLodRunTicks stale ticks starts (evidence line)
        bool episode = false;    // a low LOD episode starts on this tick (no write at all)
    };

    // One tick of an armed puppet. `ownWrites`: the override slot's writes
    // by the finalizer (applies + alt + guard - script); `anyWrites`: with
    // the script thread's; `playheadMoved`: the playhead advanced since the
    // last tick (paused, the old pose is the right one).
    inline Verdict Observe(PuppetState& state, const Config& config, bool armed, std::uint64_t ownWrites,
        std::uint64_t anyWrites, bool playheadMoved, bool shown)
    {
        Verdict verdict;
        if (!armed)
        {
            state.hasWrites = false;
            state.hasAny = false;
            state.run = 0;
            state.skipRun = 0;
            state.inEpisode = false;
            state.inSkip = false;
            state.scriptHold = 0;
            return verdict;
        }
        const bool ownSame = state.hasWrites && ownWrites == state.lastWrites;
        const bool anySame = state.hasAny && anyWrites == state.lastAny;
        state.hasWrites = true;
        state.lastWrites = ownWrites;
        state.hasAny = true;
        state.lastAny = anyWrites;
        const bool counting = playheadMoved && shown;
        if (!ownSame)
        {
            state.skipRun = 0;
            state.inSkip = false;
        }
        else if (counting)
        {
            verdict.stale = true;
            ++state.staleTicks;
            ++state.skipRun;
            if (config.forceUpdate)
            {
                verdict.force = true;
                ++state.forcedTicks;
            }
            if (config.scriptPose)
                state.scriptHold = kScriptPoseHoldTicks;
            if (state.skipRun >= kLowLodRunTicks && !state.inSkip)
            {
                state.inSkip = true;
                verdict.skipStart = true;
            }
        }
        if (playheadMoved && state.scriptHold > 0)
        {
            --state.scriptHold;
            verdict.scriptPose = true;
            ++state.scriptTicks;
        }
        if (!anySame)
        {
            state.run = 0;
            state.inEpisode = false;
        }
        else if (counting)
        {
            ++state.run;
            if (state.run >= kLowLodRunTicks && !state.inEpisode)
            {
                state.inEpisode = true;
                ++state.episodes;
                verdict.episode = true;
            }
        }
        return verdict;
    }

    // The evidence of POSE_V2_PUPPET_ANIM_SKIP: the angle in degrees between
    // a camera's forward (rotation order 2: pitch x, yaw z, in degrees, as
    // GET_GAMEPLAY_CAM_ROT gives it) and the direction from the camera to a
    // point; 0 straight ahead, 180 behind. -1 when the point is the camera.
    inline float AngleFromForwardDeg(float pitchDeg, float yawDeg, float dx, float dy, float dz)
    {
        constexpr float kRad = 3.14159265f / 180.0f;
        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (length < 1e-4f)
            return -1.0f;
        const float pitch = pitchDeg * kRad;
        const float yaw = yawDeg * kRad;
        const float fx = -std::sin(yaw) * std::cos(pitch);
        const float fy = std::cos(yaw) * std::cos(pitch);
        const float fz = std::sin(pitch);
        float cosine = (fx * dx + fy * dy + fz * dz) / length;
        cosine = cosine > 1.0f ? 1.0f : cosine < -1.0f ? -1.0f : cosine;
        return std::acos(cosine) / kRad;
    }

    struct Summary
    {
        std::uint32_t puppets = 0;        // clones the settings were applied to
        std::uint32_t undone = 0;         // undone at delete
        std::uint64_t staleTicks = 0;
        std::uint64_t forcedTicks = 0;
        std::uint32_t puppetsStale = 0;   // puppets with a stale tick
        std::uint32_t episodes = 0;
        std::uint32_t worstPacked = 0;
        std::uint32_t worstStale = 0;
        std::uint64_t scriptTicks = 0;      // ticks a puppet's pose came from the script thread
        std::uint32_t puppetsScript = 0;    // puppets with a script tick

        void Add(std::uint32_t packed, const PuppetState& state)
        {
            scriptTicks += state.scriptTicks;
            puppetsScript += state.scriptTicks ? 1u : 0u;
            staleTicks += state.staleTicks;
            forcedTicks += state.forcedTicks;
            episodes += state.episodes;
            puppetsStale += state.staleTicks ? 1u : 0u;
            if (state.staleTicks > worstStale)
            {
                worstStale = state.staleTicks;
                worstPacked = packed;
            }
        }
    };
}
