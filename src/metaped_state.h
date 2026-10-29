#pragma once

// The whole metaped look of a recorded ped or horse, beyond the layers.
//
// The layers (drawable, albedo, normal, material, palette, tints, in the
// engine's order) live in the create packet, pose segments and APPC. What a
// CREATE_PED puppet would otherwise get at random is everything the layers do
// not carry: the metaped expressions (face shape, body shape, the horse's sex,
// the morphs a ped is dressed with), the ped scale and the damage cleanliness.
// An outfit equip sets those for story characters; a generic ped (outfit 0)
// keeps the random face of the puppet.
//
// Expressions are read from the engine's structure, not asked one by one
// (build 1491.50, the getter _GET_CHAR_EXPRESSION 0xFB3F24 -> 0xFCD7CC ->
// 0xB44250):
//   creature  = [entry & ~1 + 0x28], entry = qword [table + (idx - base) * 16],
//               idx = [ped + 0x9C] & 0x1FFFF (0x2BDDE1C, table 0x3CD4388,
//               base 0x3CD4380)
//   container = [creature + 0x18 + sel * 8], sel the first byte of
//               creature + 0x104 + bufferIndex * 4 + 0..3 that is not 0x14
//               (0xB32AD8, bufferIndex the byte at 0x3CE2BF0)
//   frame     = container + 0x58 (0x2BC96C4): [frame] the value buffer,
//               [frame + 8] the frame data: [data] the DOF table, dword
//               [data + 0x2C] its count (0x26580BC), 6 bytes a DOF:
//               +0 u16 value offset, +2 u16 id, +4 u8 track, sorted by
//               (track << 16 | id); expressions are track 0x40 (0x2BC96C4);
//               a value dword 0xFFFFFFFF is "not set" (0x265FDBC).
// Every id of track 0x40 is one _GET_CHAR_EXPRESSION / _SET_CHAR_EXPRESSION
// index (the setter 0xFBBCF8 -> 0xB59744 queues the (u16 id, float) pair on
// the dressing object [[ped + 0x28] + 0x750] + 0xB20).
//
// Pure data: no engine or native access, unit tested in tests/appearance_tests.cpp.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace replay::metaped
{
    struct Expression
    {
        std::uint16_t id = 0;
        float value = 0.0f;
    };

    enum class Source : std::uint8_t
    {
        Create = 0,  // the entity entered the recording
        Segment = 1, // a skeleton change (a pose segment)
        Change = 2,  // a change seen by the appearance poll
        Late = 3,    // the create read was an engine still building the ped
    };
    inline const char* SourceText(Source source)
    {
        switch (source)
        {
        case Source::Create: return "create";
        case Source::Segment: return "segment";
        case Source::Change: return "change";
        case Source::Late: return "late";
        default: return "?";
        }
    }

    enum StateFlags : std::uint32_t
    {
        kHasExpressions = 1u << 0, // the reader was on and read the frame
        kHasScale = 1u << 1,
        kHasCleanliness = 1u << 2,
        kHasShopItems = 1u << 3,   // the shop items were read (the list may be empty)
    };

    // The shop items the ped wears, with their wearable state. The game dresses
    // the story player by shop item, not by layer (the shared clothing routine of
    // the mission and shop scripts: per item
    // _APPLY_SHOP_ITEM_TO_PED(ped, item, false, isMp, false) and, with a state,
    // _UPDATE_SHOP_ITEM_WEARABLE_STATE(ped, item, state, 0, isMp, 1); then
    // _SET_ACTIVE_META_PED_COMPONENTS_UPDATED and _UPDATE_PED_VARIATION; isMp false
    // in story mode). The engine's shop item apply runs the same layer routine as
    // _SET_META_PED_TAG plus more (build 1491.50: the entry apply 0xB56A28 ->
    // 0xB5DE10 calls the tag core 0xB59050 and 0xB502BC, 0xB51918, 0xB5A698); a
    // puppet dressed by tags alone has every layer (feet under the boots, skin
    // through the trousers). Read with _GET_SHOP_ITEM_COMPONENT_AT_INDEX per
    // component index (resolveSelection false: the cached entries; the engine maps
    // a component index to the shop item entry of the dressing object, 0xB440C8 ->
    // 0xB45C00, list at +0xB58, count +0xB60).
    struct ShopItem
    {
        std::uint32_t item = 0;  // the shop item hash
        std::uint32_t state = 0; // its wearable state (0: none)
        std::uint8_t flags = 0;  // kShopItemStatus: the native's outStatusFlag (evidence)
    };
    constexpr std::uint8_t kShopItemStatus = 1u << 0;
    constexpr std::size_t kMaxShopItems = 64;

    // One item per hash (the components of one item share it), in the
    // order first seen; 0 (a component of no shop item) is not an item.
    inline bool AddShopItem(std::vector<ShopItem>& items, std::uint32_t item, std::uint32_t state, bool status)
    {
        if (!item || items.size() >= kMaxShopItems)
            return false;
        for (const ShopItem& have : items)
            if (have.item == item)
                return false;
        ShopItem added;
        added.item = item;
        added.state = state;
        added.flags = status ? kShopItemStatus : 0;
        items.push_back(added);
        return true;
    }

    struct State
    {
        std::uint32_t timeMs = 0;
        Source source = Source::Create;
        std::uint32_t flags = 0;
        std::uint32_t outfit = 0;
        std::int32_t metapedType = -1; // _GET_META_PED_TYPE (0 male .. 3 animal, 4 none)
        std::uint32_t layerCount = 0;  // the layers worn at that time (evidence)
        std::uint64_t layersPrint = 0; // AppearancePrint of them
        float scale = 1.0f;
        std::int32_t cleanliness = -1; // _GET_PED_DAMAGE_CLEANLINESS
        std::vector<Expression> expressions; // ascending id, only the stable ones
        std::vector<ShopItem> shopItems;     // kHasShopItems (MPED v2)
    };

    struct Track
    {
        std::uint32_t id = 0; // packed ReplayId
        std::vector<State> states; // time ascending
    };

    inline const Track* FindTrack(const std::vector<Track>& tracks, std::uint32_t id)
    {
        for (const Track& track : tracks)
            if (track.id == id)
                return &track;
        return nullptr;
    }
    inline Track* FindTrack(std::vector<Track>& tracks, std::uint32_t id)
    {
        for (Track& track : tracks)
            if (track.id == id)
                return &track;
        return nullptr;
    }

    // The state to wear at timeMs: the latest at or before it. None before
    // it: the first one at most lookaheadMs later (a ped that entered the
    // recording while the engine still built it is read a few frames later;
    // its puppet is dressed with that, not left random). key = index + 1.
    struct Wanted
    {
        const State* state = nullptr;
        std::uint32_t key = 0;
        std::uint32_t lookaheadMs = 0;
    };
    inline Wanted StateAt(const Track* track, std::uint32_t timeMs, std::uint32_t lookaheadMs)
    {
        Wanted wanted;
        if (!track)
            return wanted;
        for (std::size_t index = 0; index < track->states.size(); ++index)
        {
            const State& state = track->states[index];
            if (state.timeMs > timeMs)
            {
                if (!wanted.state && state.timeMs - timeMs <= lookaheadMs)
                {
                    wanted.state = &state;
                    wanted.key = static_cast<std::uint32_t>(index + 1);
                    wanted.lookaheadMs = state.timeMs - timeMs;
                }
                break;
            }
            wanted.state = &state;
            wanted.key = static_cast<std::uint32_t>(index + 1);
        }
        return wanted;
    }

    inline std::uint64_t ExpressionsPrint(const std::vector<Expression>& expressions)
    {
        std::uint64_t hash = 0xCBF29CE484222325ull;
        for (const Expression& e : expressions)
        {
            std::uint32_t bits = 0;
            // Quantised to 1/1000: the same morph read twice is the same print.
            const float q = std::round(e.value * 1000.0f);
            std::memcpy(&bits, &q, sizeof(bits));
            for (const std::uint32_t v : {static_cast<std::uint32_t>(e.id), bits})
                for (int shift = 0; shift < 32; shift += 8)
                {
                    hash ^= (v >> shift) & 0xFFu;
                    hash *= 0x100000001B3ull;
                }
        }
        return hash ^ (static_cast<std::uint64_t>(expressions.size()) * 0x9E3779B97F4A7C15ull);
    }

    inline void SortById(std::vector<Expression>& expressions)
    {
        std::sort(expressions.begin(), expressions.end(),
            [](const Expression& a, const Expression& b) { return a.id < b.id; });
    }

    inline const Expression* FindExpression(const std::vector<Expression>& sorted, std::uint16_t id)
    {
        const auto at = std::lower_bound(sorted.begin(), sorted.end(), id,
            [](const Expression& e, std::uint16_t key) { return e.id < key; });
        return at != sorted.end() && at->id == id ? &*at : nullptr;
    }

    // Two reads of the same ped some time apart: the ids present in both
    // with the same value. A morph the ped is dressed with stays; a face
    // the animation drives (speech, blinks) moves between the reads and is
    // left out, so the playback never freezes it on the puppet.
    inline void StableExpressions(const std::vector<Expression>& first, const std::vector<Expression>& second, float eps,
        std::vector<Expression>& out)
    {
        out.clear();
        for (const Expression& a : first)
        {
            const Expression* b = FindExpression(second, a.id);
            if (b && std::fabs(a.value - b->value) <= eps)
                out.push_back(a);
        }
    }

    // Recorded against what a puppet has (both ascending id).
    struct Diff
    {
        std::uint32_t recorded = 0;
        std::uint32_t present = 0; // recorded ids the puppet has
        std::uint32_t differ = 0;  // ... with another value
        std::uint32_t missing = 0; // recorded ids the puppet lacks
        float maxDelta = 0.0f;
        std::uint16_t worstId = 0;
        bool Exact() const { return differ == 0 && missing == 0; }
    };
    inline Diff Compare(const std::vector<Expression>& recorded, const std::vector<Expression>& puppet, float eps)
    {
        Diff diff;
        diff.recorded = static_cast<std::uint32_t>(recorded.size());
        for (const Expression& want : recorded)
        {
            const Expression* got = FindExpression(puppet, want.id);
            if (!got)
            {
                ++diff.missing;
                continue;
            }
            ++diff.present;
            const float delta = std::fabs(got->value - want.value);
            if (delta > eps)
            {
                ++diff.differ;
                if (delta > diff.maxDelta)
                {
                    diff.maxDelta = delta;
                    diff.worstId = want.id;
                }
            }
        }
        return diff;
    }

    // The recorded expressions the puppet does not have yet (another value,
    // or none): what _SET_CHAR_EXPRESSION has to set. An empty puppet list
    // (not read) sets them all.
    inline void ToApply(const std::vector<Expression>& recorded, const std::vector<Expression>& puppet, float eps,
        std::vector<Expression>& out)
    {
        out.clear();
        for (const Expression& want : recorded)
        {
            const Expression* got = FindExpression(puppet, want.id);
            if (!got || std::fabs(got->value - want.value) > eps)
                out.push_back(want);
        }
    }

    // ---- dressing by shop items ----------------------------------------------
    // How a body puppet is dressed. An outfit equip stays the outfit's (NPCs).
    // Otherwise the recorded layers, and on top the recorded shop items and states
    // as the game dresses (ShopItem), when the metaped state in force carries them
    // for the very layers being worn (its layersPrint is the AppearancePrint of the
    // layers recorded with it) and this puppet did not fail the shop path before
    // (its layers were not the recorded ones after it: back to the layers for good).
    enum class DressPath : std::uint8_t
    {
        Layers,
        ShopItems,
    };
    inline const char* DressPathText(DressPath path) { return path == DressPath::ShopItems ? "shop_items" : "layers"; }

    struct DressFacts
    {
        bool hasState = false;         // a metaped state at the playhead
        bool stateHasItems = false;    // ... with shop items read and at least one
        bool printMatches = false;     // ... recorded with these layers
        bool shopRefused = false;      // the shop path failed the exact check on this puppet
        bool wearEmpty = false;        // no layer to wear
    };
    struct DressChoice
    {
        DressPath path = DressPath::Layers;
        const char* why = "no_state"; // why the layers: no_state, no_shop_items, print_differs, shop_refused, no_layers; "ok"
    };
    inline DressChoice ChooseDress(const DressFacts& facts)
    {
        DressChoice choice;
        if (facts.wearEmpty)
            choice.why = "no_layers";
        else if (!facts.hasState)
            choice.why = "no_state";
        else if (!facts.stateHasItems)
            choice.why = "no_shop_items";
        else if (!facts.printMatches)
            choice.why = "print_differs";
        else if (facts.shopRefused)
            choice.why = "shop_refused";
        else
        {
            choice.path = DressPath::ShopItems;
            choice.why = "ok";
        }
        return choice;
    }

    // The recorded shop items against the puppet's (read back the same
    // way). matched: same item and state; stateDiffer: the item with
    // another state; missing: not on the puppet; extra: on the puppet only.
    struct ShopDiff
    {
        std::uint32_t recorded = 0;
        std::uint32_t puppet = 0;
        std::uint32_t matched = 0;
        std::uint32_t stateDiffer = 0;
        std::uint32_t missing = 0;
        std::uint32_t extra = 0;
        std::uint32_t firstMissing = 0;   // item hash, evidence
        std::uint32_t firstDiffer = 0;    // item hash, evidence
        bool Exact() const { return stateDiffer == 0 && missing == 0 && extra == 0; }
    };
    inline ShopDiff CompareShopItems(const std::vector<ShopItem>& recorded, const std::vector<ShopItem>& puppet)
    {
        ShopDiff diff;
        diff.recorded = static_cast<std::uint32_t>(recorded.size());
        diff.puppet = static_cast<std::uint32_t>(puppet.size());
        for (const ShopItem& want : recorded)
        {
            const ShopItem* got = nullptr;
            for (const ShopItem& have : puppet)
                if (have.item == want.item)
                {
                    got = &have;
                    break;
                }
            if (!got)
            {
                ++diff.missing;
                if (!diff.firstMissing)
                    diff.firstMissing = want.item;
            }
            else if (got->state != want.state)
            {
                ++diff.stateDiffer;
                if (!diff.firstDiffer)
                    diff.firstDiffer = want.item;
            }
            else
                ++diff.matched;
        }
        for (const ShopItem& have : puppet)
        {
            bool known = false;
            for (const ShopItem& want : recorded)
                known = known || want.item == have.item;
            if (!known)
                ++diff.extra;
        }
        return diff;
    }

    // ---- the engine's DOF table (layout above) --------------------------
    constexpr std::uint8_t kExpressionTrack = 0x40;
    constexpr std::size_t kDofBytes = 6;
    constexpr std::uint32_t kMaxDofs = 32768;  // a sanity bound on the count
    constexpr std::uint32_t kNotSet = 0xFFFFFFFFu;

    struct Dof
    {
        std::uint16_t offset = 0;
        std::uint16_t id = 0;
        std::uint8_t track = 0;
    };
    inline Dof DofAt(const std::uint8_t* table, std::size_t index)
    {
        Dof dof;
        const std::uint8_t* at = table + index * kDofBytes;
        std::memcpy(&dof.offset, at, 2);
        std::memcpy(&dof.id, at + 2, 2);
        dof.track = at[4];
        return dof;
    }

    // The bytes of the value buffer the expression DOFs reach (0: none).
    inline std::size_t ExpressionBufferBytes(const std::uint8_t* table, std::uint32_t count)
    {
        std::size_t bytes = 0;
        for (std::uint32_t index = 0; index < count; ++index)
        {
            const Dof dof = DofAt(table, index);
            if (dof.track == kExpressionTrack)
                bytes = (std::max)(bytes, static_cast<std::size_t>(dof.offset) + 4u);
        }
        return bytes;
    }

    // The (id, value) pairs of the expression track, ascending id. Values
    // not set (0xFFFFFFFF) and non-finite ones are skipped and counted.
    // False when the table is not the layout (ids not ascending inside the
    // track, an offset past the buffer).
    inline bool ParseExpressions(const std::uint8_t* table, std::uint32_t count, const std::uint8_t* buffer,
        std::size_t bufferBytes, std::vector<Expression>& out, std::uint32_t& notSet)
    {
        out.clear();
        notSet = 0;
        if (count > kMaxDofs)
            return false;
        std::uint32_t lastKey = 0;
        bool any = false;
        for (std::uint32_t index = 0; index < count; ++index)
        {
            const Dof dof = DofAt(table, index);
            const std::uint32_t key = (static_cast<std::uint32_t>(dof.track) << 16) | dof.id;
            if (any && key < lastKey)
                return false; // the engine binary-searches it: always sorted
            any = true;
            lastKey = key;
            if (dof.track != kExpressionTrack)
                continue;
            if (static_cast<std::size_t>(dof.offset) + 4u > bufferBytes)
                return false;
            std::uint32_t bits = 0;
            std::memcpy(&bits, buffer + dof.offset, 4);
            float value = 0.0f;
            std::memcpy(&value, &bits, 4);
            if (bits == kNotSet || !std::isfinite(value))
            {
                ++notSet;
                continue;
            }
            out.push_back({dof.id, value});
        }
        return true;
    }

    // A ped scale worth applying (the engine's 1.0 is left alone).
    inline bool ScaleValid(float scale) { return std::isfinite(scale) && scale > 0.2f && scale < 5.0f; }
    inline bool ScaleNotDefault(float scale) { return ScaleValid(scale) && std::fabs(scale - 1.0f) > 0.005f; }

    // One text block per state (ReplayClipTool appearance).
    inline std::string Describe(const State& state, const char* indent)
    {
        std::string out;
        char line[256];
        std::snprintf(line, sizeof(line),
            "%smetaped t=%u source=%s outfit=0x%08x type=%d layers=%u layers_print=%016llx scale=%s cleanliness=%d "
            "expressions=%zu%s shop_items=%s\n",
            indent, state.timeMs, SourceText(state.source), state.outfit, state.metapedType, state.layerCount,
            static_cast<unsigned long long>(state.layersPrint),
            (state.flags & kHasScale) ? std::to_string(state.scale).c_str() : "unread", state.cleanliness,
            state.expressions.size(), (state.flags & kHasExpressions) ? "" : " (reader off)",
            (state.flags & kHasShopItems) ? std::to_string(state.shopItems.size()).c_str() : "unread");
        out += line;
        // The shop items, item:state (state 0: none), * status flag.
        for (std::size_t n = 0; n < state.shopItems.size(); ++n)
        {
            const ShopItem& item = state.shopItems[n];
            if (n % 6 == 0)
                out += std::string(n ? "\n" : "") + indent + "  shop ";
            std::snprintf(line, sizeof(line), "0x%08x:0x%08x%s ", item.item, item.state,
                (item.flags & kShopItemStatus) ? "*" : "");
            out += line;
        }
        if (!state.shopItems.empty())
            out += "\n";
        std::size_t column = 0;
        for (const Expression& e : state.expressions)
        {
            if (column == 0)
                out += std::string(indent) + "  ";
            std::snprintf(line, sizeof(line), "0x%04x=%.3f ", e.id, e.value);
            out += line;
            if (++column == 8)
            {
                out += "\n";
                column = 0;
            }
        }
        if (column)
            out += "\n";
        return out;
    }

    // ---- the QA rule appearance_not_recorded ----------------------------
    // A body whose puppet had nothing recorded to wear: no layer and no
    // outfit in what the dressing took (create packet, segment or change at
    // the playhead, or the lookahead), or, in a clip that has the metaped
    // section, no metaped state for it at all.
    struct NotRecordedFacts
    {
        std::size_t layers = 0;
        std::uint32_t outfit = 0;
        bool clipHasMetaped = false;
        bool entityHasMetaped = false;
    };
    inline const char* NotRecordedWhy(const NotRecordedFacts& facts)
    {
        if (facts.layers == 0 && facts.outfit == 0)
            return "no_layers";
        if (facts.clipHasMetaped && !facts.entityHasMetaped)
            return "no_metaped_state";
        return nullptr;
    }
}
