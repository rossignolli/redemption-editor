#pragma once

// Appearance changes over time.
//
// A ped's or horse's metaped layers are recorded in its create packet and
// again in every pose segment (a skeleton change, entity_events.h). A layer
// change that keeps the bone count (the hogtie ropes, a pelt put on a horse,
// a satchel, a hat put back) starts no segment, so the recorder polls a cheap
// fingerprint of the layers and stores the new appearance as a timed state
// change; playback wears, at the playhead, the latest of the create packet,
// the pose segment and these changes.
//
// Pure data: no engine or native access, unit tested in ReplayCoreTests.

#include "entity_events.h"
#include "entity_lifecycle.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace replay
{
    struct AppearanceChange
    {
        std::uint32_t timeMs = 0;
        entity::Appearance appearance;
    };

    // The appearance changes of one recorded ped or horse, time ascending.
    // Keyed by the entity's packed id; only entities with a change have one.
    struct AppearanceTrack
    {
        std::uint32_t id = 0;
        std::vector<AppearanceChange> changes;
    };

    // One layer's contribution to a fingerprint: the four asset guids (the
    // tints are left out: they cost a second native per layer).
    inline std::uint64_t LayerPrint(std::uint32_t drawable, std::uint32_t albedo, std::uint32_t normal, std::uint32_t material)
    {
        std::uint64_t hash = 0xCBF29CE484222325ull;
        for (const std::uint32_t value : {drawable, albedo, normal, material})
        {
            for (int shift = 0; shift < 32; shift += 8)
            {
                hash ^= (value >> shift) & 0xFFu;
                hash *= 0x100000001B3ull;
            }
        }
        return hash;
    }

    // Order-independent sum of the layer prints plus the layer count, so the
    // engine listing the same layers in another order is no change. The
    // recorder computes the same value from the live ped (entity_appearance).
    inline std::uint64_t CombineLayerPrints(std::uint64_t sum, std::size_t count)
    {
        return sum ^ (static_cast<std::uint64_t>(count) * 0x9E3779B97F4A7C15ull);
    }

    inline std::uint64_t AppearancePrint(const entity::Appearance& appearance)
    {
        std::uint64_t sum = 0;
        for (const entity::MetaPedAsset& asset : appearance.assets)
            sum += LayerPrint(asset.drawable, asset.albedo, asset.normal, asset.material);
        return CombineLayerPrints(sum, appearance.assets.size());
    }

    // What the entity wears at timeMs: the latest change at or before it,
    // unless the pose segment in force (`segment`, null for the create
    // packet) started later, which then carries the appearance. `key` is 0
    // when the segment or create packet applies, else the change's index + 1
    // (playback re-applies layers only when the key changes). A change at the
    // same time as the segment start wins (a window cut folds both to 0 and
    // keeps only a change newer than the segment, see window::Cut).
    struct WantedAppearance
    {
        const entity::Appearance* appearance = nullptr;
        std::uint32_t key = 0;
    };
    inline WantedAppearance AppearanceAt(const AppearanceTrack* track, const entity::PoseSegment* segment,
        const entity::Appearance& create, std::uint32_t timeMs)
    {
        WantedAppearance wanted;
        wanted.appearance = segment ? &segment->appearance : &create;
        if (!track)
            return wanted;
        for (std::size_t index = 0; index < track->changes.size(); ++index)
        {
            const AppearanceChange& change = track->changes[index];
            if (change.timeMs > timeMs)
                break;
            if (segment && change.timeMs < segment->startMs)
                continue;
            wanted.appearance = &change.appearance;
            wanted.key = static_cast<std::uint32_t>(index + 1);
        }
        return wanted;
    }

    // What a new puppet is dressed with before it is shown. AppearanceAt at
    // the playhead; when that has no layer and no outfit (a ped registered
    // while the engine still built it: 705 bones, 0 layers, the layers came
    // 48 ms later with the first segment), the first segment or change of the
    // entity with layers at most lookaheadMs later. Without it the puppet goes
    // to Ready with every category removed and the layers come with the
    // segment switch, a frame or more later.
    constexpr std::uint32_t kDressLookaheadMs = 2000;
    struct DressAppearance
    {
        const entity::Appearance* appearance = nullptr;
        std::uint32_t key = 0;          // AppearanceAt's key (0: segment or create packet)
        std::uint32_t lookaheadMs = 0;  // > 0: taken from later
        const char* from = "create";    // create, segment, change, lookahead_segment, lookahead_change, none
        bool Empty() const { return !appearance || (appearance->assets.empty() && !appearance->outfit); }
    };
    inline DressAppearance DressAt(const AppearanceTrack* track, const std::vector<entity::PoseSegment>& segments,
        entity::ReplayId id, const entity::PoseSegment* segment, const entity::Appearance& create, std::uint32_t timeMs,
        std::uint32_t lookaheadMs)
    {
        DressAppearance dress;
        const WantedAppearance wanted = AppearanceAt(track, segment, create, timeMs);
        dress.appearance = wanted.appearance;
        dress.key = wanted.key;
        dress.from = wanted.key ? "change" : segment ? "segment" : "create";
        if (!dress.Empty())
            return dress;
        std::uint32_t bestMs = 0xFFFFFFFFu;
        for (const entity::PoseSegment& later : segments)
        {
            if (later.entity != id || later.startMs <= timeMs || later.startMs - timeMs > lookaheadMs ||
                later.startMs >= bestMs || (later.appearance.assets.empty() && !later.appearance.outfit))
                continue;
            bestMs = later.startMs;
            dress.appearance = &later.appearance;
            dress.key = 0;
            dress.from = "lookahead_segment";
        }
        if (track)
            for (const AppearanceChange& change : track->changes)
            {
                if (change.timeMs <= timeMs || change.timeMs - timeMs > lookaheadMs || change.timeMs >= bestMs ||
                    (change.appearance.assets.empty() && !change.appearance.outfit))
                    continue;
                bestMs = change.timeMs;
                dress.appearance = &change.appearance;
                dress.from = "lookahead_change";
            }
        if (bestMs != 0xFFFFFFFFu)
            dress.lookaheadMs = bestMs - timeMs;
        else
            dress.from = "none";
        return dress;
    }

    inline const AppearanceTrack* FindAppearanceTrack(const std::vector<AppearanceTrack>& tracks, std::uint32_t id)
    {
        for (const AppearanceTrack& track : tracks)
        {
            if (track.id == id)
                return &track;
        }
        return nullptr;
    }
}
