#pragma once

// The player's puppet is never hidden in a replay (a design rule).
//
// QA 224 (a tester: "Arthur becomes invisible, mostly when the recording stops
// and starts another"): the clip's player record had lost its first 68 s. A
// pose track used to drop its oldest chunks past a 32 MiB budget of its own
// (pose_stream.h StreamConfig::byteBudget); Arthur's record, 1050 bones on
// the move, is the heaviest of a scene and reached it first (33.36 MB kept,
// 34 two-second chunks dropped, the horse beside him at 29.6 MB). The
// playback hid every body before its record's first frame, so he was missing
// for the whole start while his horse rode on alone. The capture now leaves
// the bytes to the recorder's own budgets (pose_capture_rules.h
// kCaptureTrackByteBudget); for clips already recorded, and any hole a
// recording may still leave, the player's puppet holds the nearest recorded
// pose (the track's sample clamps to its first and last frame) instead of
// being hidden.
//
// When his first frame has him on a horse (QA 224: in the saddle of 0:1 at
// 68 s, the horse ridden since 0 s), the held pose rides that horse: before his
// first frame he keeps the rider-in-mount offset of that frame on the mount's
// recorded transform (Carry), instead of standing frozen where the record
// begins, hundreds of metres from the camera.
//
// Pure rules, compiled by the core tests (tests/player_hold_tests.cpp): which
// created entity is the recorded player, the puppet's visibility verdict for a
// tick, and the carried transform.

#include "pose_math.h"

#include <cstdint>
#include <vector>

namespace replay::playerhold
{
    // One created entity, as the pick sees it.
    struct Candidate
    {
        std::uint32_t packed = 0;
        bool ped = false;
        std::uint32_t model = 0;
        std::uint32_t createMs = 0;
    };

    // The recorded player among the created entities: the earliest created ped
    // with the place's player model (PLAC, the model of PLAYER_PED_ID when the
    // recording began; the lower id on a tie: a cutscene's copy of Arthur comes
    // later). 0 for a clip without a place model: no guess.
    inline std::uint32_t PickPlayer(const std::vector<Candidate>& created, std::uint32_t placeModel)
    {
        const Candidate* best = nullptr;
        if (!placeModel)
            return 0;
        for (const Candidate& candidate : created)
        {
            if (!candidate.ped || !candidate.packed || candidate.model != placeModel)
                continue;
            if (!best || candidate.createMs < best->createMs ||
                (candidate.createMs == best->createMs && candidate.packed < best->packed))
                best = &candidate;
        }
        return best ? best->packed : 0;
    }

    // What the Ready path knows of a body puppet for one tick.
    struct Gate
    {
        bool player = false;          // the recorded player's puppet
        bool sampled = false;         // its record gave a pose (clamped to the track's ends)
        bool beforeFirst = false;     // the playhead before its record's first frame
        bool afterLast = false;       // after its last frame, with no later record
        bool unposed = false;         // a body whose skeleton the override cannot write yet
        bool recordedVisible = true;  // the recorded ped visibility (PVIS)
        bool neverAnimated = false;   // the never animated, bind pose hide (tpose_check.h)
    };

    struct Verdict
    {
        bool inTrack = false;        // the pose is the record's (or the player's held one)
        bool visible = false;        // drawn this tick
        bool held = false;           // the player holds his first or last pose
        bool keptVisible = false;    // the player drawn where another puppet would be hidden
    };

    // Any puppet: drawn inside its record's span, posed, and visible in the
    // recording. The player: drawn whenever his record gives a pose, holding the
    // nearest one outside its span, whatever the recorded visibility says. The
    // one case left hidden is a skeleton the override cannot write yet
    // (unposed): with no pose to hold it would show the bind pose, the T-pose;
    // that hole is transient (creation, a re-arm) and logged as
    // POSE_V2_UNPOSED_HIDDEN.
    inline Verdict Decide(const Gate& gate)
    {
        Verdict verdict;
        const bool span = !gate.beforeFirst && !gate.afterLast;
        if (!gate.player)
        {
            verdict.inTrack = span;
            verdict.visible = gate.sampled && span && !gate.unposed && gate.recordedVisible && !gate.neverAnimated;
            return verdict;
        }
        verdict.inTrack = gate.sampled;
        verdict.held = gate.sampled && !span;
        verdict.visible = gate.sampled && !gate.unposed;
        verdict.keptVisible =
            verdict.visible && (!span || !gate.recordedVisible || gate.neverAnimated);
        return verdict;
    }

    // The rider's transform on the mount now, keeping the offset he had on it
    // at his first frame (rigid: the mount's rotation and position carry him).
    inline pose::WorldTransform Carry(
        const pose::WorldTransform& riderFirst, const pose::WorldTransform& mountFirst, const pose::WorldTransform& mountNow)
    {
        const pose::Quat inverse = pose::Conjugate(pose::Normalize(mountFirst.rotation));
        const pose::Vec3 delta{riderFirst.position.x - mountFirst.position.x, riderFirst.position.y - mountFirst.position.y,
            riderFirst.position.z - mountFirst.position.z};
        const pose::Vec3 local = pose::Rotate(inverse, delta);
        const pose::Quat relative = pose::Multiply(inverse, riderFirst.rotation);
        const pose::Quat now = pose::Normalize(mountNow.rotation);
        const pose::Vec3 offset = pose::Rotate(now, local);
        pose::WorldTransform out;
        out.position = {mountNow.position.x + offset.x, mountNow.position.y + offset.y, mountNow.position.z + offset.z};
        out.rotation = pose::Normalize(pose::Multiply(now, relative));
        return out;
    }
}
