#pragma once

// Evidence for the horse muscle detail that flickers on a puppet.
//
// The engine's animation frame of a creature holds more than bones: every
// DOF is (track, id) and the track names are a table in .rdata (build
// 1491.50: names from 0x3624FE8, "animatedNormalMaps" at 0x3625100; the
// parallel id bytes at 0x3624F90 give boneTranslation 0, boneRotation 1,
// boneScale 2, visibility 4, blendShape 0x15, animatedNormalMaps 0x17).
// The muscles that move under a horse's skin are animated normal maps:
// blend weights on track 0x17 that the expressions compute each frame. The
// replay records and writes only the bone rows (the pose finalizer's
// output); track 0x17 of a puppet is whatever its own expressions produced,
// and an expression runs only while the director's effective LOD is in its
// range (anim_visible.h, anim_lod_hook.h). The recording forces LOD 0 and
// visible on every recorded ped (RecordPedAnimLod0); the playback did not.
//
// Two detectors, pure data:
//  - Watch: the track 0x17 values of one entity, tick after tick. A value
//    that goes A, B, A (or set, unset, set: the expression switched off and
//    on) is an alternation, the "fight between states" on screen.
//  - RowWatch: the recorded bone rows of a clip (ReplayClipTool
//    muscle-rows), the same A, B, A test per row on consecutive raw samples:
//    a flicker recorded into the clip, not made by the playback.
//
// Unit tested in tests/muscle_watch_tests.cpp.

#include "pose_math.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace replay::musclewatch
{
    constexpr std::uint8_t kBlendShapeTrack = 0x15;
    constexpr std::uint8_t kAnimatedNormalMapsTrack = 0x17;
    constexpr std::size_t kDofBytes = 6; // +0 u16 value offset, +2 u16 id, +4 u8 track (metaped_state.h)
    constexpr std::uint32_t kMaxDofs = 32768;
    constexpr std::uint32_t kNotSet = 0xFFFFFFFFu;

    // The DOFs of one track: ids ascending, the byte offset of each value.
    struct TrackDofs
    {
        std::vector<std::uint16_t> ids;
        std::vector<std::uint16_t> offsets;
        std::size_t bufferBytes = 0; // the value bytes the track reaches
    };

    // False when the table is not the layout (keys not ascending: the engine
    // binary-searches it).
    inline bool CollectTrack(const std::uint8_t* table, std::uint32_t count, std::uint8_t track, TrackDofs& out)
    {
        out = TrackDofs{};
        if (!table || count > kMaxDofs)
            return false;
        std::uint32_t lastKey = 0;
        for (std::uint32_t index = 0; index < count; ++index)
        {
            const std::uint8_t* at = table + static_cast<std::size_t>(index) * kDofBytes;
            std::uint16_t offset = 0, id = 0;
            std::memcpy(&offset, at, 2);
            std::memcpy(&id, at + 2, 2);
            const std::uint32_t key = (static_cast<std::uint32_t>(at[4]) << 16) | id;
            if (index != 0 && key < lastKey)
                return false;
            lastKey = key;
            if (at[4] != track)
                continue;
            out.ids.push_back(id);
            out.offsets.push_back(offset);
            out.bufferBytes = (std::max)(out.bufferBytes, static_cast<std::size_t>(offset) + 4u);
        }
        return true;
    }

    // DOFs per track.
    inline void Histogram(const std::uint8_t* table, std::uint32_t count, std::uint32_t (&perTrack)[256])
    {
        std::fill(std::begin(perTrack), std::end(perTrack), 0u);
        if (!table || count > kMaxDofs)
            return;
        for (std::uint32_t index = 0; index < count; ++index)
            ++perTrack[table[static_cast<std::size_t>(index) * kDofBytes + 4]];
    }

    // "track:count" for every track that has DOFs, hex track, ';' separated.
    inline std::string HistogramText(const std::uint32_t (&perTrack)[256])
    {
        std::string text;
        char item[32];
        for (int track = 0; track < 256; ++track)
        {
            if (!perTrack[track])
                continue;
            std::snprintf(item, sizeof(item), "%s%x:%u", text.empty() ? "" : ";", track, perTrack[track]);
            text += item;
        }
        return text.empty() ? std::string("-") : text;
    }

    // The values of a track read from a copy of the frame's value buffer
    // (raw bits; kNotSet is "not written this frame").
    inline bool ValuesOf(const TrackDofs& dofs, const std::uint8_t* buffer, std::size_t bytes, std::vector<std::uint32_t>& bits)
    {
        bits.assign(dofs.offsets.size(), kNotSet);
        for (std::size_t n = 0; n < dofs.offsets.size(); ++n)
        {
            if (static_cast<std::size_t>(dofs.offsets[n]) + 4u > bytes)
                return false;
            std::memcpy(&bits[n], buffer + dofs.offsets[n], 4);
        }
        return true;
    }

    inline bool IsSet(std::uint32_t bits)
    {
        if (bits == kNotSet)
            return false;
        float value = 0.0f;
        std::memcpy(&value, &bits, 4);
        return std::isfinite(value);
    }

    inline float ValueOf(std::uint32_t bits)
    {
        float value = 0.0f;
        std::memcpy(&value, &bits, 4);
        return value;
    }

    // A weight that moves less than this is not a change.
    constexpr float kWeightEps = 0.02f;
    // An A, B, A: back to within this share of the smaller step.
    constexpr float kReturnShare = 0.5f;

    struct Tick
    {
        bool changed = false;      // a value moved more than eps, or switched set / unset
        bool alternation = false;  // a value went A, B, A over the last three ticks
        std::uint32_t set = 0;     // values set this tick
        std::uint32_t unset = 0;   // values not written this tick
        std::uint32_t setFlips = 0;     // values set on one tick and unset on the other (or back)
        std::uint32_t alternating = 0;  // values in an A, B, A this tick
        float maxDelta = 0.0f;     // the largest step of a value set on both ticks
        std::uint16_t worstId = 0; // its DOF id
        float sum = 0.0f;          // the sum of the set values
    };

    struct Watch
    {
        std::vector<std::uint16_t> ids;
        std::vector<std::uint32_t> prev;
        std::vector<std::uint32_t> prev2;
        std::uint32_t have = 0; // ticks held (0, 1, 2)
        std::uint64_t ticks = 0;
        std::uint64_t changedTicks = 0;
        std::uint64_t alternationTicks = 0;
        std::uint64_t setFlips = 0;
        std::uint64_t unsetValues = 0;
        float maxDelta = 0.0f;
        std::uint16_t maxDeltaId = 0;
    };

    inline Tick Observe(Watch& watch, const std::vector<std::uint16_t>& ids, const std::vector<std::uint32_t>& bits,
        float eps = kWeightEps)
    {
        Tick tick;
        if (ids.size() != bits.size())
            return tick;
        if (ids != watch.ids)
        {
            watch.ids = ids;
            watch.have = 0; // another layout: start again
        }
        for (std::size_t n = 0; n < bits.size(); ++n)
        {
            if (IsSet(bits[n]))
            {
                ++tick.set;
                tick.sum += ValueOf(bits[n]);
            }
            else
            {
                ++tick.unset;
            }
        }
        if (watch.have >= 1)
        {
            for (std::size_t n = 0; n < bits.size(); ++n)
            {
                const bool setNow = IsSet(bits[n]);
                const bool setPrev = IsSet(watch.prev[n]);
                if (setNow != setPrev)
                {
                    ++tick.setFlips;
                    tick.changed = true;
                }
                else if (setNow)
                {
                    const float step = std::fabs(ValueOf(bits[n]) - ValueOf(watch.prev[n]));
                    if (step > tick.maxDelta)
                    {
                        tick.maxDelta = step;
                        tick.worstId = ids[n];
                    }
                    if (step > eps)
                        tick.changed = true;
                }
                if (watch.have < 2)
                    continue;
                const bool setFirst = IsSet(watch.prev2[n]);
                bool alternating = false;
                if (setFirst == setNow && setPrev != setNow)
                {
                    alternating = true; // on, off, on (or off, on, off)
                }
                else if (setFirst && setPrev && setNow)
                {
                    const float a = ValueOf(watch.prev2[n]), b = ValueOf(watch.prev[n]), c = ValueOf(bits[n]);
                    const float d1 = std::fabs(b - a), d2 = std::fabs(c - b), back = std::fabs(c - a);
                    alternating = d1 > eps && d2 > eps && back < kReturnShare * (std::min)(d1, d2);
                }
                if (alternating)
                    ++tick.alternating;
            }
            tick.alternation = tick.alternating != 0;
        }
        ++watch.ticks;
        watch.changedTicks += tick.changed ? 1u : 0u;
        watch.alternationTicks += tick.alternation ? 1u : 0u;
        watch.setFlips += tick.setFlips;
        watch.unsetValues += tick.unset;
        if (tick.maxDelta > watch.maxDelta)
        {
            watch.maxDelta = tick.maxDelta;
            watch.maxDeltaId = tick.worstId;
        }
        watch.prev2.swap(watch.prev);
        watch.prev = bits;
        watch.have = (std::min)(watch.have + 1u, 2u);
        return tick;
    }

    // ---- recorded rows (ReplayClipTool muscle-rows) ----------------------

    // Steps under these are noise: translation in m, rotation in rad, scale.
    constexpr float kRowEps[3] = {0.002f, 0.01f, 0.01f};

    // The distance between two values of a row (0 translation, 1 rotation, 2 scale).
    inline float RowDistance(const pose::BoneQts& a, const pose::BoneQts& b, int row)
    {
        if (row == 1)
        {
            const float dot = a.rotation.x * b.rotation.x + a.rotation.y * b.rotation.y + a.rotation.z * b.rotation.z +
                a.rotation.w * b.rotation.w;
            const float c = (std::min)(1.0f, std::fabs(dot));
            return 2.0f * std::acos(c);
        }
        const float* x = row == 0 ? a.translation : a.scale;
        const float* y = row == 0 ? b.translation : b.scale;
        const float dx = x[0] - y[0], dy = x[1] - y[1], dz = x[2] - y[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    struct RowWatch
    {
        std::vector<pose::BoneQts> prev;
        std::vector<pose::BoneQts> prev2;
        std::uint32_t have = 0;
        std::uint32_t samples = 0;
        std::vector<std::uint32_t> alternations; // bones x 3 (t, r, s)
        std::vector<std::uint32_t> changes;      // bones x 3
    };

    // One raw sample of a record's locals. Returns the rows in an A, B, A.
    inline std::uint32_t ObserveRows(RowWatch& watch, const std::vector<pose::BoneQts>& locals)
    {
        if (watch.have != 0 && locals.size() != watch.prev.size())
            watch.have = 0;
        if (watch.alternations.size() != locals.size() * 3u)
        {
            watch.alternations.assign(locals.size() * 3u, 0u);
            watch.changes.assign(locals.size() * 3u, 0u);
        }
        std::uint32_t alternating = 0;
        if (watch.have >= 1)
        {
            for (std::size_t bone = 0; bone < locals.size(); ++bone)
            {
                for (int row = 0; row < 3; ++row)
                {
                    const float d2 = RowDistance(watch.prev[bone], locals[bone], row);
                    if (d2 > kRowEps[row])
                        ++watch.changes[bone * 3u + static_cast<std::size_t>(row)];
                    if (watch.have < 2)
                        continue;
                    const float d1 = RowDistance(watch.prev2[bone], watch.prev[bone], row);
                    const float back = RowDistance(watch.prev2[bone], locals[bone], row);
                    if (d1 > kRowEps[row] && d2 > kRowEps[row] && back < kReturnShare * (std::min)(d1, d2))
                    {
                        ++watch.alternations[bone * 3u + static_cast<std::size_t>(row)];
                        ++alternating;
                    }
                }
            }
        }
        ++watch.samples;
        watch.prev2.swap(watch.prev);
        watch.prev = locals;
        watch.have = (std::min)(watch.have + 1u, 2u);
        return alternating;
    }
}
