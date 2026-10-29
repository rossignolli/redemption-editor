#pragma once

// A body puppet's skeleton or pose segment changing without a hole.
//
// The engine can rebuild a puppet's skeleton in place (a hat knocked off:
// 959 to 942 bones). Releasing the arm and arming again on later ticks (the
// segment switch, then the signature gate) leaves ticks with no pose written
// and the body hidden: a blink, although the pose of the new skeleton is
// ready all along (the neighbour record's alternate pose, published every
// tick for the rebuild frame).
//
// The rule, as a replay writes the pose of the frame it is on: in the tick
// the change is seen, the arm moves to the new skeleton
// with the record of its bone count and the pose is written at once
// (ArmPoseV2Exact writes it from the script thread); the signature check is
// done after, on the armed body (ScheduleSignatureRecheck), not before it.
// A segment whose record is the one already armed does not release the arm.
// A segment change the skeleton has not reached yet keeps posing the armed
// record (its last pose holds) until the engine rebuilds it. A skeleton
// that shrank to a count no record has, within the armed record's body group
// (a gore rebuild in progress), keeps the arm and takes the armed pose's
// first bones (prefix_hold.h).
//
// Pure: unit tested in ReplayCoreTests (segment_switch_tests.cpp).

#include "prefix_hold.h"

#include <cstdint>

namespace replay::segswitch
{
    struct Facts
    {
        bool armed = false;            // an exact arm (a mapped arm keeps the old path)
        bool resolved = false;         // the puppet's skeleton was read this tick
        bool samePointer = false;      // the armed skeleton object
        std::uint32_t armedBones = 0;
        std::uint32_t cloneBones = 0;
        std::uint32_t armedRecord = 0;
        std::uint32_t segmentRecord = 0; // the segment at the playhead
        std::uint32_t segmentBones = 0;
        std::uint32_t recordForClone = 0; // a record of the entity with cloneBones (0: none)
        std::uint32_t bodyBones = 0;      // the armed record's body group (prefix_hold.h; 0: none)
    };

    enum class Action : std::uint8_t
    {
        Keep,     // nothing to do this tick
        RearmNow, // arm `record` on the current skeleton and write its pose now
        Release,  // the old path: release, arm on a later tick (hidden meanwhile)
        HoldPrefix, // keep the arm: the armed pose's first bones on the shrunk skeleton
    };

    struct Decision
    {
        Action action = Action::Keep;
        std::uint32_t record = 0;
        const char* why = "none";
    };

    inline Decision Decide(const Facts& f)
    {
        Decision d;
        if (!f.armed)
            return d;
        if (!f.resolved)
        {
            d.action = Action::Release;
            d.why = "unresolved";
            return d;
        }
        const bool skeletonSame = f.samePointer && f.cloneBones == f.armedBones;
        if (skeletonSame)
        {
            if (f.armedRecord == f.segmentRecord)
                return d; // posing the segment's record
            if (f.segmentBones == f.cloneBones && f.segmentRecord)
            {
                // The segment's record fits the skeleton as it is: swap the
                // record in the tick (no release, no hidden frame).
                d.action = Action::RearmNow;
                d.record = f.segmentRecord;
                d.why = "segment_record";
                return d;
            }
            // The segment changed and the engine has not rebuilt the skeleton
            // yet: the armed record's pose holds, the alternate pose waits for
            // the rebuild frame.
            d.why = "wait_rebuild";
            return d;
        }
        if (f.recordForClone)
        {
            d.action = Action::RearmNow;
            d.record = f.recordForClone;
            d.why = "skeleton_changed";
            return d;
        }
        if (f.samePointer && prefixhold::Writable(f.cloneBones, f.armedBones, f.bodyBones))
        {
            // Rebuilt in place to fewer bones than the record, within its
            // body group: posed with the record's first bones until a record
            // fits (prefix_hold.h), not hidden.
            d.action = Action::HoldPrefix;
            d.why = "prefix_hold";
            return d;
        }
        d.action = Action::Release;
        d.why = "no_record_for_bones";
        return d;
    }

    // SwitchPoseV2Segment: the arm stays when it already poses the new
    // segment's record on a skeleton with its bones (armed in the tick the
    // skeleton changed, before the segment switch saw the playhead cross).
    inline bool SwitchKeepsArm(bool armed, bool mapped, std::uint32_t armedRecord, std::uint32_t armedBones,
        std::uint32_t newRecord, std::uint32_t newBones)
    {
        return armed && !mapped && armedRecord == newRecord && armedBones == newBones;
    }

    // SwitchPoseV2Segment: otherwise an exact arm holds the previous record
    // (its last pose) instead of being released: RefreshPoseV2Override, in
    // the same tick, swaps the record when the skeleton already has the new
    // segment's bones (Decide: segment_record), or re-arms in the tick the
    // engine rebuilds it (skeleton_changed); bounded by a timeout.
    inline bool SwitchHoldsArm(bool armed, bool mapped)
    {
        return armed && !mapped;
    }

    // Ticks a shown body went without a pose around skeleton changes: from
    // the tick a change is seen to the tick the puppet is armed again.
    struct Gap
    {
        std::uint32_t switches = 0;       // changes seen
        std::uint32_t rearmedInTick = 0;  // ... armed again in the same tick
        std::uint32_t released = 0;       // ... released (the old path)
        std::uint32_t holeTicks = 0;      // ticks without an arm after a change
        std::uint32_t worstHole = 0;
        std::uint32_t open = 0;           // the running hole of the current change (ticks)
        bool inHole = false;

        void Change(Action action)
        {
            ++switches;
            if (action == Action::RearmNow || action == Action::HoldPrefix)
                ++rearmedInTick;
            else if (action == Action::Release)
            {
                ++released;
                inHole = true;
                open = 0;
            }
        }
        // Once a tick per body after its arm logic: armed or not.
        void Tick(bool armed)
        {
            if (!inHole)
                return;
            if (armed)
            {
                inHole = false;
                if (open > worstHole)
                    worstHole = open;
                return;
            }
            ++open;
            ++holeTicks;
        }
    };
}
