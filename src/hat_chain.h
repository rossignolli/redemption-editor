#pragma once

// The hat chain of a human skeleton.
//
// Floating hats are recorded that way: a story character's skeleton hangs the
// hat from a short chain under the head (Micah: head 94 -> 121 -> 122 -> 123
// -> 124, hat group 737..753 on 124; Dutch: head 88 -> 111 -> ... -> 114, hat
// group 818 on 114). The engine's animation fits the hat to the head by moving
// the chain's first bone (local x 0.16 for Micah, 0.17 for Dutch) only while
// the ped is on screen; off screen the animation output leaves that track
// unwritten and the pose finalizer takes the bind pose (0.26), 10 cm higher.
// The recording copies what the finalizer wrote, so every sample of a ped the
// game camera was not looking at carries the high hat.
//
// A hat point is a bone at the RDR2 human skeleton's hat offset from its
// parent, (0, +-0.11, -0.09) (seen in three skeletons of three clips: the
// base skeleton's own points and the first bone of the hat group). The chain
// runs from a hat point's parent up to the head, the first ancestor with
// many children (the face bones); a hat point hung on the head itself (the
// generic peds' 959-bone layout) has no chain. The chain's top bone (the
// head's child) is the one the animation moves; the subtree under it (the
// rest of the chain, the hat points and the hat group) follows it.
//
// Playback (FixSample, before each publish of a puppet's pose) puts the
// fitted value back where the recording has the bind one: the value the
// same record had on screen, else the value another record of the same
// model had (Fitted), else kDefaultFitX; then the object transforms of the
// subtree are composed again from the locals (the finalizer's own rule,
// Compose ParentThenLocal with the parent's scale). Only a chain that carries
// a hat group is touched: the twin chain (always at the bind pose, nothing on
// it) and a hatless head stay as recorded. Measured on recorded clips with
// ReplayClipTool hats: Micah's hat high in 87% of one, Dutch 93%, Bill 99%;
// 28 hats never fitted in a whole clip (peds the camera never saw).
//
// Pure: no engine access; unit tested in tests/scenery_speech_tests.cpp and used
// offline by ReplayClipTool hats.

#include "pose_math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

namespace replay::hatchain
{
    constexpr float kPointY = 0.11f;
    constexpr float kPointZ = -0.09f;
    constexpr float kPointTolerance = 0.015f;
    // The head's direct children (Micah 14, Dutch 10); no bone of a chain
    // has more than four.
    constexpr std::uint32_t kHeadMinChildren = 6;
    constexpr std::uint32_t kMaxChain = 6;
    // Every human chain of the measured clips has four bones (235 of 235); a
    // horse's bone at the hat offset made a chain of one.
    constexpr std::size_t kChainBones = 4;
    // The top bone's local x: 0.16 to 0.18 fitted, 0.26 the bind pose.
    constexpr float kTopMinX = 0.10f;
    constexpr float kTopMaxX = 0.30f;

    struct Chain
    {
        std::vector<std::uint16_t> bones; // the hat point's parent first, up to the head's child
        std::uint16_t head = 0;
        std::uint16_t top = 0; // the head's child: the bone the animation moves
        bool carriesGroup = false; // a hat group hangs on it (the hat the player sees)
    };

    inline bool IsHatPoint(const std::array<float, 3>& t)
    {
        return std::fabs(t[0]) < kPointTolerance && std::fabs(std::fabs(t[1]) - kPointY) < kPointTolerance &&
            std::fabs(t[2] - kPointZ) < kPointTolerance;
    }

    // `parents` one per bone (negative: a root or a bone without a known
    // parent), `translations` the locals at any sample. One entry per
    // distinct chain.
    inline bool NearOffset(const std::array<float, 3>& t, float x, float y, float z)
    {
        return std::fabs(t[0] - x) < kPointTolerance && std::fabs(t[1] - y) < kPointTolerance && std::fabs(t[2] - z) < kPointTolerance;
    }

    // The chain is found by its own shape, from the head down: the top bone
    // (x 0.10 to 0.30 on the head), then (-0.09, 0, 0), (0, 0, 0.09) and the
    // end (0, 0, 0). Climbing from the hat points would take the bone they
    // hang on for the head when it has many children (in a generic ped of 637
    // bones, the hat's own root, 8 children, under the chain's end) and miss
    // the chain. The chain carries a hat when a group hangs under its end
    // (kHatGroupMinBones bones: Micah 20, the generic ped 19; the twin chain
    // and a hatless head 2 or 3).
    constexpr std::uint32_t kHatGroupMinBones = 8;

    inline std::vector<Chain> FindChains(const std::vector<std::int32_t>& parents, const std::vector<std::array<float, 3>>& translations)
    {
        std::vector<Chain> chains;
        const std::size_t count = parents.size() < translations.size() ? parents.size() : translations.size();
        std::vector<std::vector<std::uint16_t>> kids(count);
        const auto valid = [&](std::int32_t bone) { return bone >= 0 && static_cast<std::size_t>(bone) < count; };
        for (std::size_t bone = 0; bone < count; ++bone)
            if (valid(parents[bone]))
                kids[static_cast<std::size_t>(parents[bone])].push_back(static_cast<std::uint16_t>(bone));
        const auto child = [&](std::size_t of, float x, float y, float z) -> int {
            for (const std::uint16_t c : kids[of])
                if (NearOffset(translations[c], x, y, z))
                    return c;
            return -1;
        };
        for (std::size_t top = 0; top < count; ++top)
        {
            const std::int32_t head = parents[top];
            if (!valid(head) || kids[static_cast<std::size_t>(head)].size() < kHeadMinChildren)
                continue;
            const std::array<float, 3>& t = translations[top];
            if (!(t[0] >= kTopMinX && t[0] <= kTopMaxX) || std::fabs(t[1]) > 0.03f || std::fabs(t[2]) > 0.03f)
                continue;
            const int a = child(top, -0.09f, 0.0f, 0.0f);
            const int b = a < 0 ? -1 : child(static_cast<std::size_t>(a), 0.0f, 0.0f, 0.09f);
            const int e = b < 0 ? -1 : child(static_cast<std::size_t>(b), 0.0f, 0.0f, 0.0f);
            if (e < 0)
                continue;
            Chain chain;
            chain.bones = {static_cast<std::uint16_t>(e), static_cast<std::uint16_t>(b), static_cast<std::uint16_t>(a),
                static_cast<std::uint16_t>(top)};
            chain.head = static_cast<std::uint16_t>(head);
            chain.top = static_cast<std::uint16_t>(top);
            // The bones under the end (parents come before children).
            std::vector<std::uint8_t> under(count, 0);
            under[static_cast<std::size_t>(e)] = 1;
            std::uint32_t group = 0;
            for (std::size_t bone = static_cast<std::size_t>(e) + 1; bone < count; ++bone)
                if (valid(parents[bone]) && under[static_cast<std::size_t>(parents[bone])])
                {
                    under[bone] = 1;
                    ++group;
                }
            chain.carriesGroup = group >= kHatGroupMinBones;
            chains.push_back(chain);
        }
        return chains;
    }

    // The chains' bones, ascending, without repeats.
    inline std::vector<std::uint16_t> ChainBones(const std::vector<Chain>& chains, std::size_t boneCount)
    {
        std::vector<std::uint8_t> in(boneCount, 0);
        for (const Chain& chain : chains)
            for (const std::uint16_t bone : chain.bones)
                if (bone < boneCount)
                    in[bone] = 1;
        std::vector<std::uint16_t> out;
        for (std::size_t bone = 0; bone < boneCount; ++bone)
            if (in[bone])
                out.push_back(static_cast<std::uint16_t>(bone));
        return out;
    }

    // Every bone under a chain's top bone (the top included), ascending.
    // The engine orders a skeleton's bones parent first, so one pass does.
    inline std::vector<std::uint16_t> Subtree(const std::vector<Chain>& chains, const std::vector<std::int32_t>& parents)
    {
        const std::size_t count = parents.size();
        std::vector<std::uint8_t> in(count, 0);
        for (const Chain& chain : chains)
            if (chain.top < count)
                in[chain.top] = 1;
        std::vector<std::uint16_t> out;
        for (std::size_t bone = 0; bone < count; ++bone)
        {
            const std::int32_t parent = parents[bone];
            if (!in[bone] && parent >= 0 && static_cast<std::size_t>(parent) < bone && in[static_cast<std::size_t>(parent)])
                in[bone] = 1;
            if (in[bone])
                out.push_back(static_cast<std::uint16_t>(bone));
        }
        return out;
    }

    // The fitted top bone is at 0.16 to 0.18; the bind pose at 0.26. Above
    // this the sample has the unfitted hat (the frames between the two
    // values too: 0.24, 0.25 when the ped turned off screen mid-interval).
    constexpr float kFitMaxX = 0.20f;
    // When nothing fitted was seen for the record or its model: the value
    // most chains fit to (0.16 to 0.18 seen, 1 cm off at most).
    constexpr float kDefaultFitX = 0.17f;
    // A chain bone besides the top this far from its parent: the hat is in
    // the hand (the chain's last bone up to 0.85 m away); the sample is left
    // as recorded. The chain's rest offsets are 0.09 at most.
    constexpr float kHandledM = 0.15f;
    // A subtree bone whose recorded object is this far from the one its
    // recorded parent and local compose to was written directly by the
    // engine (a spring of the brim, the hat in the hand): it keeps its
    // recorded object.
    constexpr float kConsistentM = 0.02f;

    // Per model, the fitted value some record of the clip showed (shared by
    // every puppet of a playback; cleared at its start).
    using Fitted = std::map<std::uint32_t, float>;

    enum class FitSource : std::uint8_t
    {
        None,
        Record,  // the same record on screen
        Model,   // another record of the same model
        Default  // kDefaultFitX
    };

    inline const char* FitSourceName(FitSource source)
    {
        switch (source)
        {
        case FitSource::Record: return "record";
        case FitSource::Model: return "model";
        case FitSource::Default: return "default";
        default: return "none";
        }
    }

    struct PuppetFix
    {
        struct Item
        {
            std::uint16_t top = 0;
            std::uint16_t head = 0;
            std::uint16_t hatRoot = 0;          // the hat group's first bone
            std::vector<std::uint16_t> chain;   // the chain's other bones
            std::vector<std::uint16_t> subtree; // ascending, top included
            float fitted = -1.0f;               // the record's own, once seen
        };
        bool armed = false;
        std::uint32_t model = 0;
        std::vector<Item> items;
        std::vector<std::int32_t> parents; // the record's layout parents
        replay::pose::CompositionOrder order = replay::pose::CompositionOrder::ParentThenLocal;
        bool inheritScale = true;
        std::uint64_t fixed = 0;   // samples put back to the fitted value
        std::uint64_t learned = 0; // samples already fitted
        std::uint64_t handled = 0; // samples with the hat in the hand, left alone
        std::uint64_t kept = 0;    // subtree objects kept as recorded (written directly)
        FitSource lastSource = FitSource::None;
        float lastFit = 0.0f;
    };

    // The fix of a record's layout: its hat chains that carry a hat group,
    // each with the subtree that follows its top bone. Not armed when there
    // is none (no hat, a hat hung on the head, another skeleton).
    inline PuppetFix BuildFix(std::uint32_t model, const std::vector<std::int32_t>& parents,
        const std::vector<std::array<float, 3>>& translations, replay::pose::CompositionOrder order, bool inheritScale)
    {
        PuppetFix fix;
        fix.model = model;
        fix.parents = parents;
        fix.order = order;
        fix.inheritScale = inheritScale;
        for (const Chain& chain : FindChains(parents, translations))
        {
            if (!chain.carriesGroup)
                continue;
            PuppetFix::Item item;
            item.top = chain.top;
            item.head = chain.head;
            for (const std::uint16_t bone : chain.bones)
                if (bone != chain.top)
                    item.chain.push_back(bone);
            item.subtree = Subtree({chain}, parents);
            // The hat's root: the end's child that has the most bones under it.
            std::uint32_t best = 0;
            for (const std::uint16_t bone : item.subtree)
            {
                if (parents[bone] != static_cast<std::int32_t>(chain.bones.front()))
                    continue;
                std::uint32_t size = 0;
                std::vector<std::uint8_t> under(parents.size(), 0);
                under[bone] = 1;
                for (std::size_t other = bone + 1u; other < parents.size(); ++other)
                    if (parents[other] >= 0 && under[static_cast<std::size_t>(parents[other])])
                    {
                        under[other] = 1;
                        ++size;
                    }
                if (size >= best)
                {
                    best = size;
                    item.hatRoot = bone;
                }
            }
            fix.items.push_back(item);
        }
        fix.armed = !fix.items.empty();
        return fix;
    }

    // Before a publish: the unfitted top bone gets the fitted value and the
    // subtree's objects are composed again; a fitted sample teaches the
    // record's (and the model's) value. Returns the samples fixed (0 or 1
    // per item). `locals` and `objects` are the sample's, one per bone.
    inline std::uint32_t FixSample(PuppetFix& fix, Fitted& fitted, std::vector<replay::pose::BoneQts>& locals,
        std::vector<replay::pose::BoneQts>& objects)
    {
        if (!fix.armed)
            return 0;
        std::uint32_t changed = 0;
        const std::size_t count = locals.size() < objects.size() ? locals.size() : objects.size();
        for (PuppetFix::Item& item : fix.items)
        {
            if (item.top >= count)
                continue;
            float& x = locals[item.top].translation[0];
            if (x <= kFitMaxX)
            {
                item.fitted = x;
                fitted[fix.model] = x;
                ++fix.learned;
                continue;
            }
            bool handled = false;
            for (const std::uint16_t bone : item.chain)
            {
                if (bone >= count)
                    continue;
                const float* t = locals[bone].translation;
                handled = handled || t[0] * t[0] + t[1] * t[1] + t[2] * t[2] > kHandledM * kHandledM;
            }
            if (handled)
            {
                ++fix.handled;
                continue;
            }
            const replay::pose::BoneQts topBefore = locals[item.top];
            const auto model = fitted.find(fix.model);
            FitSource source = FitSource::Default;
            float value = kDefaultFitX;
            if (item.fitted > 0.0f)
            {
                source = FitSource::Record;
                value = item.fitted;
            }
            else if (model != fitted.end())
            {
                source = FitSource::Model;
                value = model->second;
            }
            x = value;
            fix.lastSource = source;
            fix.lastFit = value;
            // The subtree's recorded objects, read before any is replaced:
            // a bone the engine composed from its local follows the new
            // top; one it wrote directly keeps its recorded object (and its
            // children follow that one).
            static thread_local std::vector<replay::pose::BoneQts> before;
            before.resize(count);
            for (const std::uint16_t bone : item.subtree)
                if (bone < count)
                    before[bone] = objects[bone];
            for (const std::uint16_t bone : item.subtree)
            {
                if (bone >= count || bone >= fix.parents.size())
                    continue;
                const std::int32_t parent = fix.parents[bone];
                if (parent < 0 || static_cast<std::size_t>(parent) >= count)
                    continue; // a root or an object-space bone keeps its recorded object
                const std::size_t p = static_cast<std::size_t>(parent);
                const replay::pose::BoneQts& parentBefore = bone == item.top || p >= before.size() ||
                        std::find(item.subtree.begin(), item.subtree.end(), static_cast<std::uint16_t>(p)) == item.subtree.end()
                    ? objects[p]
                    : before[p];
                const replay::pose::BoneQts expected = replay::pose::Compose(parentBefore,
                    bone == item.top ? topBefore : locals[bone], fix.order, fix.inheritScale);
                if (replay::pose::TranslationDistance(expected, before[bone]) > kConsistentM)
                {
                    ++fix.kept;
                    continue;
                }
                objects[bone] = replay::pose::Compose(objects[p], locals[bone], fix.order, fix.inheritScale);
            }
            ++fix.fixed;
            ++changed;
        }
        return changed;
    }
}
