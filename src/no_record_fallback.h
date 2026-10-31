#pragma once

// A body puppet whose skeleton, after the recorded layers went on, has a bone
// count no record of the entity has.
//
// Example: Arthur raises the bandana; the segment records 27 layers and 1066
// bones, but on the puppet the same 27 layers build 985 bones every time,
// while a later appearance of the same segment (26 layers, the old neckwear
// gone) builds 1066 and arms exact. _SET_META_PED_TAG replaces the asset (one
// per category), so a recorded list holding two assets of one category
// cannot be put back tag by tag.
//
// The rules (a replay editor never hides its puppet for a variation: fixed
// skeleton, the variation applied again):
//  - a body that was posed and lost its arm to a skeleton change is armed
//    through the skeleton map in that tick (bones matched by group, the rest
//    skipped), not hidden while the layers are tried again;
//  - behind the mapped pose the recorded layers are applied again, and the
//    retries stop once an attempt gives the answer of the one before (the
//    engine's answer for these layers);
//  - then (ini NoRecordAltAppearance) the entity's other recorded
//    appearances of the same segment, nearest in time first, one at a time:
//    the one that builds the record's bones arms exact; none: the recorded
//    layers go back on and the mapped pose stays;
//  - every step logs which recorded layers are not on the puppet and why
//    (texture, its category taken by another asset, absent), with the
//    categories learned from the puppets (_GET_CATEGORY_OF_COMPONENT_AT_INDEX).
//
// Pure: unit tested in ReplayCoreTests (scenery_speech_tests.cpp).

#include "appearance_track.h"
#include "entity_events.h"
#include "entity_lifecycle.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace replay::norecord
{
    // ---- the layer diff ----

    enum class LayerWhy : std::uint8_t
    {
        Texture,       // the drawable is on the puppet with other textures or tints
        CategoryTaken, // another asset holds its category on the puppet
        Absent,        // not on the puppet, category unknown or free
    };

    inline const char* LayerWhyText(LayerWhy why)
    {
        switch (why)
        {
        case LayerWhy::Texture: return "texture";
        case LayerWhy::CategoryTaken: return "category_taken";
        default: return "absent";
        }
    }

    struct PuppetLayer
    {
        entity::MetaPedAsset asset;
        std::uint32_t category = 0; // _GET_CATEGORY_OF_COMPONENT_AT_INDEX (0: not read)
    };

    struct MissingLayer
    {
        std::uint32_t index = 0; // in the recorded list
        entity::MetaPedAsset asset;
        std::uint32_t category = 0; // learned (0: unknown)
        std::uint32_t takenBy = 0;  // the puppet asset's drawable in that category (CategoryTaken)
        LayerWhy why = LayerWhy::Absent;
    };

    struct LayerDiff
    {
        std::uint32_t recorded = 0;
        std::uint32_t present = 0;
        std::uint32_t puppet = 0;
        // Recorded layers whose category another recorded layer shares
        // (both known): the list cannot be put back tag by tag.
        std::uint32_t sharedCategory = 0;
        std::vector<MissingLayer> missing;
        std::vector<PuppetLayer> extra; // on the puppet, not recorded
    };

    inline bool SameAsset(const entity::MetaPedAsset& a, const entity::MetaPedAsset& b)
    {
        return a.drawable == b.drawable && a.albedo == b.albedo && a.normal == b.normal && a.material == b.material &&
            a.palette == b.palette && a.tint0 == b.tint0 && a.tint1 == b.tint1 && a.tint2 == b.tint2;
    }

    // Drawable to category, learned from the puppets' layers (a category is
    // a property of the drawable). Bounded; a full cache keeps what it has.
    class CategoryCache
    {
    public:
        static constexpr std::size_t kMax = 8192;
        void Learn(std::uint32_t drawable, std::uint32_t category)
        {
            if (!drawable || !category || (map_.size() >= kMax && !map_.count(drawable)))
                return;
            map_[drawable] = category;
        }
        std::uint32_t Of(std::uint32_t drawable) const
        {
            const auto it = map_.find(drawable);
            return it == map_.end() ? 0u : it->second;
        }
        bool Knows(std::uint32_t drawable) const { return map_.count(drawable) != 0; }
        std::size_t Size() const { return map_.size(); }
        void Clear() { map_.clear(); }

    private:
        std::unordered_map<std::uint32_t, std::uint32_t> map_;
    };

    inline LayerDiff DiffLayers(const std::vector<entity::MetaPedAsset>& recorded, const std::vector<PuppetLayer>& puppet,
        const CategoryCache& categories)
    {
        LayerDiff diff;
        diff.recorded = static_cast<std::uint32_t>(recorded.size());
        diff.puppet = static_cast<std::uint32_t>(puppet.size());
        std::vector<bool> used(puppet.size(), false);
        for (std::size_t r = 0; r < recorded.size(); ++r)
        {
            const entity::MetaPedAsset& want = recorded[r];
            bool found = false;
            for (std::size_t p = 0; p < puppet.size() && !found; ++p)
            {
                if (!used[p] && SameAsset(puppet[p].asset, want))
                {
                    used[p] = true;
                    found = true;
                }
            }
            if (found)
            {
                ++diff.present;
                continue;
            }
            MissingLayer miss;
            miss.index = static_cast<std::uint32_t>(r);
            miss.asset = want;
            miss.category = categories.Of(want.drawable);
            const auto sameDrawable = std::find_if(puppet.begin(), puppet.end(),
                [&](const PuppetLayer& layer) { return layer.asset.drawable == want.drawable; });
            if (sameDrawable != puppet.end())
            {
                miss.why = LayerWhy::Texture;
                if (!miss.category)
                    miss.category = sameDrawable->category;
            }
            else if (miss.category)
            {
                const auto holder = std::find_if(puppet.begin(), puppet.end(),
                    [&](const PuppetLayer& layer) { return layer.category == miss.category; });
                if (holder != puppet.end())
                {
                    miss.why = LayerWhy::CategoryTaken;
                    miss.takenBy = holder->asset.drawable;
                }
            }
            diff.missing.push_back(miss);
        }
        for (std::size_t p = 0; p < puppet.size(); ++p)
        {
            if (!used[p])
                diff.extra.push_back(puppet[p]);
        }
        for (std::size_t r = 0; r < recorded.size(); ++r)
        {
            const std::uint32_t category = categories.Of(recorded[r].drawable);
            if (!category)
                continue;
            for (std::size_t o = 0; o < recorded.size(); ++o)
            {
                if (o != r && recorded[o].drawable != recorded[r].drawable && categories.Of(recorded[o].drawable) == category)
                {
                    ++diff.sharedCategory;
                    break;
                }
            }
        }
        return diff;
    }

    // "index:drawable:albedo:why:category[:taken_by]" joined by ';' (at
    // most `cap`, then "+n"), for the log line.
    inline std::string FormatMissing(const LayerDiff& diff, std::size_t cap)
    {
        std::string text;
        char buffer[96];
        for (std::size_t i = 0; i < diff.missing.size() && i < cap; ++i)
        {
            const MissingLayer& m = diff.missing[i];
            if (m.why == LayerWhy::CategoryTaken)
                std::snprintf(buffer, sizeof(buffer), "%s%u:0x%08x:0x%08x:%s:0x%08x:0x%08x", text.empty() ? "" : ";", m.index,
                    m.asset.drawable, m.asset.albedo, LayerWhyText(m.why), m.category, m.takenBy);
            else
                std::snprintf(buffer, sizeof(buffer), "%s%u:0x%08x:0x%08x:%s:0x%08x", text.empty() ? "" : ";", m.index,
                    m.asset.drawable, m.asset.albedo, LayerWhyText(m.why), m.category);
            text += buffer;
        }
        if (diff.missing.size() > cap)
            text += ";+" + std::to_string(diff.missing.size() - cap);
        return text.empty() ? "none" : text;
    }

    // "drawable:albedo:category" joined by ';' (at most `cap`, then "+n").
    inline std::string FormatExtra(const LayerDiff& diff, std::size_t cap)
    {
        std::string text;
        char buffer[64];
        for (std::size_t i = 0; i < diff.extra.size() && i < cap; ++i)
        {
            const PuppetLayer& e = diff.extra[i];
            std::snprintf(buffer, sizeof(buffer), "%s0x%08x:0x%08x:0x%08x", text.empty() ? "" : ";", e.asset.drawable,
                e.asset.albedo, e.category);
            text += buffer;
        }
        if (diff.extra.size() > cap)
            text += ";+" + std::to_string(diff.extra.size() - cap);
        return text.empty() ? "none" : text;
    }

    // ---- the steps behind the mapped pose ----

    constexpr std::uint64_t kStepMs = 400;      // the engine rebuilds the skeleton within it
    constexpr std::uint32_t kMaxRetries = 6;

    enum class Step : std::uint8_t
    {
        Wait,      // nothing this tick
        Retry,     // apply the recorded layers again
        Done,      // stays mapped
    };

    inline const char* StepText(Step step)
    {
        switch (step)
        {
        case Step::Retry: return "retry";
        case Step::Done: return "done";
        default: return "wait";
        }
    }

    struct Fallback
    {
        bool active = false;
        bool done = false;           // the steps ran out (or it armed exact) for `record`
        std::uint32_t record = 0;    // the segment's record being chased
        std::uint32_t wantBones = 0; // its bone count
        std::uint32_t retries = 0;
        std::uint32_t lastBones = 0;
        std::uint32_t lastPresent = 0;
        bool haveLast = false;
        bool sameAnswer = false;
        std::uint64_t dueAt = 0;

        // A no-record arm through the map for `record` (a new record: from
        // the start; the same: the steps go on, a re-map after an attempt
        // that built yet another bone count; once done, not again until
        // Reset: the restore re-maps the puppet).
        // True when it began (false: it goes on, or is done, for `record`).
        bool Begin(std::uint32_t recordId, std::uint32_t bones, std::uint64_t now)
        {
            if ((active || done) && record == recordId)
                return false;
            *this = Fallback{};
            active = true;
            record = recordId;
            wantBones = bones;
            dueAt = now + kStepMs;
            return true;
        }
        // The puppet armed exact for the record.
        void Finish()
        {
            if (active)
                done = true;
            active = false;
        }
        // The segment or the appearance at the playhead changed.
        void Reset() { *this = Fallback{}; }

        // Once a tick while the body is mapped for `record`: the bone count
        // and the recorded layers present on the puppet now.
        Step Next(std::uint32_t cloneBones, std::uint32_t present, std::uint64_t now)
        {
            if (!active || now < dueAt)
                return Step::Wait;
            if (haveLast && cloneBones == lastBones && present == lastPresent)
                sameAnswer = true;
            haveLast = true;
            lastBones = cloneBones;
            lastPresent = present;
            dueAt = now + kStepMs;
            if (!sameAnswer && retries < kMaxRetries)
            {
                ++retries;
                return Step::Retry;
            }
            active = false;
            done = true;
            return Step::Done;
        }
    };

    // ArmPoseV2Puppet: no record for the skeleton's bones. A body that was
    // posed and lost its arm to a skeleton change goes through the map at
    // once; a new puppet (its layers maybe not in yet; a horse can arm exact
    // after one retry) keeps the retries first, hidden. A map refused for
    // this change falls back to the retries.
    inline bool MapNow(bool enabled, bool lostToChange, bool mapRefused)
    {
        return enabled && lostToChange && !mapRefused;
    }
}
