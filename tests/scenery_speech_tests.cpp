// Unit tests: the recorded player's lines go to the hidden player only at his
// puppet; a rope's own entity is not recorded as a prop; the scenery keeps its
// visibility (a mission's hidden objects stay hidden); the graphics component
// holding the engine's "animate as if visible" byte is found from the ped; a
// loose fragment piece keeps the groups it carries and is made from the whole
// object by breaking its root off and taking off the groups it no longer had;
// loot and pickups present before the take are scenery entries, the take a
// flip or a leaving; a skeleton that appears near a recorded actor is an
// animation, and the scenery keeps the objects attached to it (a chest's
// lining and loot); the horse voice bank; the puppets' foley variants and the
// PED_VARIABLES block (ped_audio_state.h); the weather manager's transition
// over time in the environment track; the timecycle modifier and weather
// variations; the speech request fields and the resume held while the old
// copy of the line is still stopping; the gunshot's start after its fire event
// (shot_audio_delay.h) and the weapon object's sounds the shot track does not
// remake (sound_owner_table.h OffShot); a camp's prop set objects and a mission
// camp reached later become late scenery entries; the moon's session angles
// recorded, played and given back; the never animated record, the model's bind
// pose read the finalizer's way, and the T-pose watch (tpose_check.h).
#include "animal_bank.h"
#include "anim_lod_hook.h"
#include "anim_visible.h"
#include "bench_script.h"
#include "entity_query.h"
#include "entity_query_track.h"
#include "fire_initial.h"
#include "frag_piece.h"
#include "environment_track.h"
#include "hat_chain.h"
#include "no_record_fallback.h"
#include "hat_look.h"
#include "object_check.h"
#include "object_pose.h"
#include "ped_audio_state.h"
#include "rope_models.h"
#include "speech_player_route.h"
#include "speech_request.h"
#include "speech_resume.h"
#include "world_theatre.h"
#include "timecycle_track.h"
#include "shot_audio_delay.h"
#include "sound_generic_core.h"
#include "sound_owner_table.h"
#include "moon_track.h"
#include "tpose_check.h"
#include "qa_health.h"
#include "log_policy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    int g_scenerySpeechFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_scenerySpeechFailures;
    }

    // GET_HASH_KEY: the engine registered the rope models by these names.
    constexpr std::uint32_t Joaat(const char* text)
    {
        std::uint32_t hash = 0;
        for (; *text; ++text)
        {
            char c = *text;
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
            hash += static_cast<std::uint8_t>(c);
            hash += hash << 10;
            hash ^= hash >> 6;
        }
        hash += hash << 3;
        hash ^= hash >> 11;
        hash += hash << 15;
        return hash;
    }

    void TestSpeechRoute()
    {
        using namespace replay::speech::route;
        Check(Choose(false, true, true, true, 0.5f).speaker == Speaker::Puppet && Choose(false, true, true, true, 0.5f).why == Why::KeyOff,
            "SpeechPlayerOnPlayer=0: the puppet");
        Check(Choose(true, false, true, true, 0.5f).why == Why::NotPlayer, "another speaker's line stays on its puppet");
        Check(Choose(true, true, false, true, 0.5f).why == Why::NoPlayer, "no player ped: the puppet");
        // The follow outside a mission: the hidden player at the puppet.
        Check(Choose(true, true, true, true, 1.2f).speaker == Speaker::Player, "player at the puppet says the line");
        Check(Choose(true, true, true, true, kNearM).speaker == Speaker::Player, "at the limit, still the player");
        // A mission: the player stayed where the replay was opened (about 1 km away).
        const Decision far = Choose(true, true, true, true, 1000.0f);
        Check(far.speaker == Speaker::Puppet && far.why == Why::Far, "player far away: the puppet says the line");
        Check(Choose(true, true, true, true, kNearM + 0.01f).why == Why::Far, "just past the limit: the puppet");
        Check(Choose(true, true, true, true, -1.0f).why == Why::Far, "distance not read: the puppet");
        Check(std::string_view(WhyName(Why::Far)) == "player_far" && std::string_view(WhyName(Why::Near)) == "player_near",
            "route names for the log");
        // RetryOnPuppet: with the follow on, lines refused on the hidden player.
        Check(RetryOnPuppet(true, false, false, false), "refused on the hidden player: the puppet says it");
        Check(RetryOnPuppet(true, true, false, true), "another phrase on the player, stopped: the puppet says it");
        Check(!RetryOnPuppet(true, true, false, false), "another phrase not stopped (it sounds): no second line");
        Check(!RetryOnPuppet(true, true, true, false), "created on the player as recorded: stays there");
        Check(!RetryOnPuppet(false, false, false, false), "a puppet's refusal is not asked again here");
        // HoldAtPuppet: the player's pain voices (named, no voice object) die on the
        // puppet, so a far hidden player is brought to it for them.
        const std::uint32_t pain = 0xA5D5FD9Eu; // a player pain voice of 1268-Blackwater
        Check(std::string_view(WhyName(Why::Held)) == "held_at_puppet", "held route name for the log");
        Check(HoldAtPuppet(far, true, true, false, pain, 0, 0), "far, follow moving, pain voice: hold him at the puppet");
        Check(!HoldAtPuppet(far, true, false, false, pain, 0, 0), "follow skipped (mission, vehicle): the puppet as before");
        Check(!HoldAtPuppet(far, false, true, false, pain, 0, 0), "distance not read: the puppet as before");
        Check(!HoldAtPuppet(far, true, true, false, pain, 0x1234u, 0x22), "a line with a voice object keeps its puppet");
        Check(!HoldAtPuppet(far, true, true, false, 0, 0, 0), "no voice named: the puppet as before");
        Check(!HoldAtPuppet(far, true, true, true, pain, 0, 0), "an animal line is never held");
        Check(!HoldAtPuppet(Choose(true, true, true, true, 1.0f), true, true, false, pain, 0, 0), "already near: no hold needed");
        Check(!HoldAtPuppet(Choose(true, false, true, true, 40.0f), true, true, false, pain, 0, 0), "another speaker: no hold");
        Check(!HoldAtPuppet(Choose(false, true, true, true, 40.0f), true, true, false, pain, 0, 0), "key off: no hold");
        Check(!HoldAtPuppet(Choose(true, true, false, true, 40.0f), true, true, false, pain, 0, 0), "no player ped: no hold");
        // SameCharacter: John's save playing an Arthur clip gave Arthur's
        // puppet John's pain. The hidden player stands in only as the recorded character.
        const std::uint32_t arthur = 0x0D7114C9u; // joaat("player_zero")
        const std::uint32_t john = 0x00B69710u; // joaat("player_three")
        Check(SameCharacter(arthur, arthur) && !SameCharacter(arthur, john) && !SameCharacter(john, arthur),
            "the same model is the same character, Arthur and John are not");
        Check(!SameCharacter(0, 0) && !SameCharacter(0, arthur) && !SameCharacter(arthur, 0),
            "an unknown model is not the same character");
        const Decision other = Choose(true, true, true, SameCharacter(arthur, john), 1.0f);
        Check(other.speaker == Speaker::Puppet && other.why == Why::CharacterMismatch,
            "Arthur's line with John live: his puppet, even with John standing at it");
        Check(Choose(true, true, true, false, 40.0f).why == Why::CharacterMismatch, "far and another character: mismatch first");
        Check(Choose(true, true, true, SameCharacter(john, john), 1.0f).speaker == Speaker::Player,
            "John's clip in John's save: the hidden player as before");
        Check(Choose(true, false, true, false, 1.0f).why == Why::NotPlayer, "another speaker's line is no mismatch");
        Check(Choose(true, true, false, false, 1.0f).why == Why::NoPlayer, "no player ped: no mismatch");
        Check(Choose(false, true, true, false, 1.0f).why == Why::KeyOff, "key off: no mismatch");
        Check(!HoldAtPuppet(Choose(true, true, true, false, 40.0f), true, true, false, pain, 0, 0),
            "another character is never held at the puppet for a pain line");
        Check(std::string_view(WhyName(Why::CharacterMismatch)) == "character_mismatch", "mismatch route name for the log");
        Check(!OwnVoiceAllowed(Why::CharacterMismatch), "another character: no own voice retry (silent before wrong voice)");
        Check(OwnVoiceAllowed(Why::Far) && OwnVoiceAllowed(Why::NotPlayer) && OwnVoiceAllowed(Why::KeyOff),
            "the own voice retry stays for the other routes");
        // HoldActive: at least kHoldMinMs, then while the line lives, never past kHoldMaxMs.
        Check(!HoldActive(0, 5000, true), "no hold set");
        Check(HoldActive(1000, 1000, false) && HoldActive(1000, 1000 + kHoldMinMs - 1, false), "held for the minimum");
        Check(!HoldActive(1000, 1000 + kHoldMinMs, false), "released after the minimum when the line is gone");
        Check(HoldActive(1000, 1000 + kHoldMinMs + 3000, true), "kept while the line still plays");
        Check(!HoldActive(1000, 1000 + kHoldMaxMs, true), "never past the maximum");
        Check(!HoldActive(5000, 4000, true), "a tick before the request is no hold");
    }

    void TestRopeModels()
    {
        using namespace replay::ropemodels;
        Check(kRopeModelCount == 19, "the 19 rope types of 0x6D7984");
        for (const RopeModel& rope : kRopeModels)
            Check(Joaat(rope.name) == rope.hash, "each rope model hash is the joaat of its name");
        Check(IsRopeModel(0x0B4EBCE9u) && std::string_view(RopeModelName(0x0B4EBCE9u)) == "ROPE_REINS",
            "the object by every horse is ROPE_REINS");
        Check(IsRopeModel(0x25DDA17Du) && IsRopeModel(0xD61D9FF3u), "ROPE_REINS_ROPE and ROPE_THIN");
        Check(!IsRopeModel(0x4334F471u) && !IsRopeModel(0xD0F1B75Cu) && RopeModelName(0) == nullptr,
            "a chest, a magneto and no model are not ropes");
    }

    void TestSceneryVisibility()
    {
        using namespace replay::objcheck;
        // The magneto of 035: hidden the whole recording.
        const std::vector<std::uint32_t> none;
        Check(!VisibleAt(false, none, 0) && !VisibleAt(false, none, 100000), "hidden when recorded, never shown");
        Check(VisibleAt(true, none, 5000), "visible stays visible");
        // Shown at 1500, hidden again at 3000.
        const std::vector<std::uint32_t> flips{1500, 3000};
        Check(!VisibleAt(false, flips, 1499) && VisibleAt(false, flips, 1500) && VisibleAt(false, flips, 2999) &&
                !VisibleAt(false, flips, 3000),
            "flips at their times");
        // The scan: a flip only on a change, in order, up to the cap.
        std::vector<std::uint32_t> seen;
        Check(!NoteVisibility(false, seen, false, 250) && seen.empty(), "no change, no flip");
        Check(NoteVisibility(false, seen, true, 500) && seen.size() == 1 && seen[0] == 500, "shown: one flip");
        Check(!NoteVisibility(false, seen, true, 750), "still shown: nothing");
        Check(NoteVisibility(false, seen, false, 1000) && seen.size() == 2, "hidden again: second flip");
        Check(!NoteVisibility(false, seen, true, 900), "a time before the last flip is refused");
        std::vector<std::uint32_t> many;
        bool state = true;
        for (std::uint32_t n = 0; n < kMaxVisibilityFlips + 10; ++n)
        {
            state = !state;
            NoteVisibility(true, many, state, 100 + n * 250);
        }
        Check(many.size() == kMaxVisibilityFlips, "flips capped");
        // The buffer window [10000, 20000): earlier flips fold into the start.
        std::uint8_t visible = 0;
        std::vector<std::uint32_t> cut{5000, 12000, 25000};
        CutVisibility(visible, cut, 10000, 20000);
        Check(visible == 1 && cut.size() == 1 && cut[0] == 2000, "window: folded, moved and dropped");
        Check(!VisibleAt(visible != 0, cut, 2000) && VisibleAt(visible != 0, cut, 1999), "the cut entry plays as recorded");
    }

    // A skeleton shaped like Micah's (764 bones; here the parts that matter): a
    // head with face bones, the hat chain 121..124 with the hat group's first bone
    // far past it, and the twin chain 105..108 with nothing on it (hat_chain.h).
    // Identity rotations and unit scales: an object is its parent's translation
    // plus its local.
    struct TestSkeleton
    {
        std::vector<std::int32_t> parents;
        std::vector<std::array<float, 3>> translations;
        std::uint16_t head = 0, top = 0, end = 0, hatRoot = 0, twinTop = 0;
    };

    TestSkeleton MicahLike(float topX)
    {
        TestSkeleton k;
        const auto add = [&](std::int32_t parent, float x, float y, float z) {
            k.parents.push_back(parent);
            k.translations.push_back({x, y, z});
            return static_cast<std::uint16_t>(k.parents.size() - 1);
        };
        const std::uint16_t root = add(-1, 0.0f, 0.0f, 0.0f);
        const std::uint16_t neck = add(root, 0.0f, 0.0f, 1.5f);
        k.head = add(neck, 0.05f, 0.0f, 0.0f);
        for (int face = 0; face < 8; ++face)
            add(k.head, 0.01f * face, 0.02f, 0.03f);
        // The twin chain, always at the bind pose, its hat points.
        k.twinTop = add(k.head, 0.26f, 0.0f, 0.0f);
        std::uint16_t at = add(k.twinTop, -0.09f, 0.0f, 0.0f);
        at = add(at, 0.0f, 0.0f, 0.09f);
        const std::uint16_t twinEnd = add(at, 0.0f, 0.0f, 0.0f);
        add(twinEnd, 0.0f, 0.11f, -0.09f);
        add(twinEnd, 0.0f, -0.11f, -0.09f);
        // The hat chain.
        k.top = add(k.head, topX, -0.01f, 0.01f);
        at = add(k.top, -0.09f, 0.0f, 0.0f);
        at = add(at, 0.0f, 0.0f, 0.09f);
        k.end = add(at, 0.0f, 0.0f, 0.0f);
        add(k.end, 0.0f, 0.11f, -0.09f);
        add(k.end, 0.0f, -0.11f, -0.09f);
        // The rest of the body up to the appended groups.
        while (k.parents.size() < 90)
            add(root, 0.1f, 0.0f, 0.5f);
        // The hat group, appended after the base skeleton.
        k.hatRoot = add(k.end, 0.0f, 0.11f, -0.09f);
        const std::uint16_t back = add(k.hatRoot, 0.0f, -0.11f, 0.09f);
        add(back, 0.08f, 0.0f, -0.09f); // brim bones
        for (int brim = 0; brim < 5; ++brim)
            add(back, -0.08f + 0.03f * brim, 0.19f, -0.09f);
        return k;
    }

    void Objects(const TestSkeleton& k, const std::vector<replay::pose::BoneQts>& locals, std::vector<replay::pose::BoneQts>& objects)
    {
        objects.assign(locals.size(), replay::pose::BoneQts{});
        for (std::size_t bone = 0; bone < locals.size(); ++bone)
        {
            const std::int32_t parent = k.parents[bone];
            objects[bone] = parent < 0 ? locals[bone]
                                       : replay::pose::Compose(objects[static_cast<std::size_t>(parent)], locals[bone],
                                             replay::pose::CompositionOrder::ParentThenLocal, true);
        }
    }

    std::vector<replay::pose::BoneQts> Locals(const TestSkeleton& k)
    {
        std::vector<replay::pose::BoneQts> locals(k.translations.size());
        for (std::size_t bone = 0; bone < locals.size(); ++bone)
            for (int axis = 0; axis < 3; ++axis)
                locals[bone].translation[axis] = k.translations[bone][static_cast<std::size_t>(axis)];
        return locals;
    }

    bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

    void TestHatChains()
    {
        using namespace replay::hatchain;
        const TestSkeleton k = MicahLike(0.26f);
        const std::vector<Chain> chains = FindChains(k.parents, k.translations);
        Check(chains.size() == 2, "the hat chain and its twin");
        const Chain* hat = nullptr;
        const Chain* twin = nullptr;
        for (const Chain& chain : chains)
            (chain.top == k.top ? hat : twin) = &chain;
        Check(hat && hat->carriesGroup && hat->head == k.head && hat->bones.size() == kChainBones && hat->bones.front() == k.end,
            "the hat chain: four bones from the head's child, the hat group on its last bone");
        Check(twin && twin->top == k.twinTop && !twin->carriesGroup, "the twin chain carries no hat");
        // A hat point hung on the head itself (the generic 959-bone layout): no chain.
        std::vector<std::int32_t> parents = k.parents;
        std::vector<std::array<float, 3>> translations = k.translations;
        parents.push_back(k.head);
        translations.push_back({0.0f, 0.11f, -0.09f});
        Check(FindChains(parents, translations).size() == 2, "a hat point on the head adds no chain");
        // A horse's bone at the hat offset under a bone of few children: refused (not four bones).
        std::vector<std::int32_t> horse{-1, 0, 1, 1, 1, 1, 1, 1, 1, 2};
        std::vector<std::array<float, 3>> horseT(10, {0.3f, 0.0f, 0.0f});
        horseT[9] = {0.0f, 0.11f, -0.09f};
        Check(FindChains(horse, horseT).empty(), "a chain of one bone is no hat chain");
        // Build: the fix arms on the hat chain only, with the hat in its subtree.
        const PuppetFix fix = BuildFix(0xDE361D65u, k.parents, k.translations, replay::pose::CompositionOrder::ParentThenLocal, true);
        Check(fix.armed && fix.items.size() == 1 && fix.items[0].top == k.top && fix.items[0].hatRoot == k.hatRoot &&
                fix.items[0].chain.size() == 3,
            "the fix: the hat chain, its hat root and its other bones");
        bool hatInSubtree = false;
        for (const std::uint16_t bone : fix.items[0].subtree)
            hatInSubtree = hatInSubtree || bone == k.hatRoot;
        Check(hatInSubtree, "the hat group follows the top bone");
    }

    void TestHatFix()
    {
        using namespace replay::hatchain;
        const TestSkeleton k = MicahLike(0.26f);
        PuppetFix fix = BuildFix(0xDE361D65u, k.parents, k.translations, replay::pose::CompositionOrder::ParentThenLocal, true);
        Fitted fitted;
        // Off screen from the start: nothing learned, the default.
        std::vector<replay::pose::BoneQts> locals = Locals(k);
        std::vector<replay::pose::BoneQts> objects;
        Objects(k, locals, objects);
        const float hatBefore = objects[k.hatRoot].translation[0];
        Check(FixSample(fix, fitted, locals, objects) == 1 && fix.lastSource == FitSource::Default &&
                Near(locals[k.top].translation[0], kDefaultFitX),
            "never seen fitted: the default value");
        Check(Near(objects[k.hatRoot].translation[0], hatBefore - (0.26f - kDefaultFitX)), "the hat came down with the top bone");
        Check(Near(objects[k.twinTop].translation[0], objects[k.head].translation[0] + 0.26f), "the twin chain is left alone");
        // On screen: the record's value is learned and the sample untouched.
        TestSkeleton seen = MicahLike(0.16f);
        locals = Locals(seen);
        Objects(seen, locals, objects);
        Check(FixSample(fix, fitted, locals, objects) == 0 && Near(locals[k.top].translation[0], 0.16f), "a fitted sample is kept");
        Check(fitted.count(0xDE361D65u) && Near(fitted[0xDE361D65u], 0.16f), "the model learned 0.16");
        // Off screen again: the record's own value.
        locals = Locals(k);
        Objects(k, locals, objects);
        Check(FixSample(fix, fitted, locals, objects) == 1 && fix.lastSource == FitSource::Record &&
                Near(locals[k.top].translation[0], 0.16f),
            "back off screen: the record's fitted value");
        // Another record of the model, never on screen: the model's value.
        PuppetFix other = BuildFix(0xDE361D65u, k.parents, k.translations, replay::pose::CompositionOrder::ParentThenLocal, true);
        locals = Locals(k);
        Objects(k, locals, objects);
        Check(FixSample(other, fitted, locals, objects) == 1 && other.lastSource == FitSource::Model &&
                Near(locals[k.top].translation[0], 0.16f),
            "another record of the model: the model's value");
        // The hat in the hand: left as recorded.
        TestSkeleton hand = MicahLike(0.26f);
        hand.translations[k.end] = {-0.23f, 0.85f, -0.23f};
        locals = Locals(hand);
        Objects(hand, locals, objects);
        const std::uint64_t handledBefore = fix.handled;
        Check(FixSample(fix, fitted, locals, objects) == 0 && fix.handled == handledBefore + 1 && Near(locals[k.top].translation[0], 0.26f),
            "the hat in the hand is left alone");
        // A bone of the hat the engine wrote directly (its object off the
        // compose of its parent and local): it keeps its recorded object.
        locals = Locals(k);
        Objects(k, locals, objects);
        for (std::size_t bone = k.hatRoot; bone < objects.size(); ++bone) // the hat and what hangs on it, written by the engine
            objects[bone].translation[2] += 0.30f;
        const float directZ = objects[k.hatRoot].translation[2];
        const float directX = objects[k.hatRoot].translation[0];
        const std::uint64_t keptBefore = fix.kept;
        Check(FixSample(fix, fitted, locals, objects) == 1 && fix.kept == keptBefore + 1 && Near(objects[k.hatRoot].translation[2], directZ) &&
                Near(objects[k.hatRoot].translation[0], directX),
            "an object written directly keeps its recorded place");
        // No hat (no group on the chain): not armed, nothing changed.
        TestSkeleton hatless = MicahLike(0.26f);
        hatless.parents.resize(90);
        hatless.translations.resize(90);
        PuppetFix none = BuildFix(0x53DD98DFu, hatless.parents, hatless.translations, replay::pose::CompositionOrder::ParentThenLocal, true);
        locals = Locals(hatless);
        Objects(hatless, locals, objects);
        Check(!none.armed && FixSample(none, fitted, locals, objects) == 0 && Near(locals[hatless.top].translation[0], 0.26f),
            "a hatless head is left as recorded");
    }

    // Base + 0x39ECE40 / 0x39ECE48, the ped's component handle at +0x9C, the row of
    // 0x148 bytes, the tagged pointer at row + 0x98 and the owner check at
    // component + 8.
    void TestAnimVisibleComponent()
    {
        using namespace replay::animvisible;
        std::map<std::uintptr_t, std::vector<std::uint8_t>> memory;
        auto put = [&memory](std::uintptr_t at, const void* data, std::size_t size) {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            memory[at] = std::vector<std::uint8_t>(bytes, bytes + size);
        };
        auto read = [&memory](std::uintptr_t at, void* out, std::size_t size) {
            const auto found = memory.find(at);
            if (found == memory.end() || found->second.size() != size)
                return false;
            std::memcpy(out, found->second.data(), size);
            return true;
        };
        const std::uintptr_t base = 0x140000000ull;
        const std::uintptr_t ped = 0x2A0001230ull;
        const std::uintptr_t table = 0x300000000ull;
        const std::uintptr_t component = 0x310000040ull;
        const std::uint32_t baseIndex = 0x20;
        const std::uint32_t slot = 0x25;
        const std::uint8_t pedType = kEntityTypePed;
        const std::uint32_t handle = 0x00A0000u | slot; // high bits above the slot mask are ignored
        const std::uintptr_t tagged = component | 1u;
        const std::uintptr_t owner = ped | 0x7u; // low 4 bits are flags
        put(ped + kEntityTypeOffset, &pedType, 1);
        put(ped + kComponentHandleOffset, &handle, 4);
        put(base + kComponentBaseIndexRva, &baseIndex, 4);
        put(base + kComponentTableRva, &table, 8);
        const std::uintptr_t row = table + static_cast<std::uintptr_t>(slot - baseIndex) * kComponentRowBytes;
        put(row + kGraphicsComponentSlot, &tagged, 8);
        put(component + kComponentOwnerOffset, &owner, 8);
        Check(GraphicsComponent(base, ped, read) == component, "the graphics component of the ped (tag bit cleared)");
        Check(GraphicsComponent(base, 0, read) == 0 && GraphicsComponent(0, ped, read) == 0, "no ped or no base: nothing");

        const std::uint8_t vehicleType = 3;
        put(ped + kEntityTypeOffset, &vehicleType, 1);
        Check(GraphicsComponent(base, ped, read) == 0, "an entity that is not a ped is refused");
        put(ped + kEntityTypeOffset, &pedType, 1);

        const std::uintptr_t stranger = 0x2B0004560ull;
        put(component + kComponentOwnerOffset, &stranger, 8);
        Check(GraphicsComponent(base, ped, read) == 0, "a component owned by another entity is refused");
        put(component + kComponentOwnerOffset, &owner, 8);

        const std::uint32_t lowHandle = 0x10; // below the table's base index
        put(ped + kComponentHandleOffset, &lowHandle, 4);
        Check(GraphicsComponent(base, ped, read) == 0, "a slot below the base index is refused");
        const std::uint32_t farHandle = baseIndex + kMaxSlot;
        put(ped + kComponentHandleOffset, &farHandle, 4);
        Check(GraphicsComponent(base, ped, read) == 0, "a slot past any ped pool is refused before it becomes an address");
        put(ped + kComponentHandleOffset, &handle, 4);

        const std::uintptr_t empty = 0;
        put(row + kGraphicsComponentSlot, &empty, 8);
        Check(GraphicsComponent(base, ped, read) == 0, "a ped without a graphics component");
        put(row + kGraphicsComponentSlot, &tagged, 8);
        memory.erase(base + kComponentTableRva);
        Check(GraphicsComponent(base, ped, read) == 0, "an unreadable table: nothing");
        Check(kAnimVisibleOffset == 0x4A8, "the byte 0x8F0991 reads");
    }

    // The anim director of a ped as 0x2BDDE1C finds it (base + 0x3CD4380 /
    // 0x3CD4388, 16-byte rows, tag bit, director at +0x28).
    void TestAnimLodDirector()
    {
        using namespace replay::animlod;
        std::map<std::uintptr_t, std::vector<std::uint8_t>> memory;
        auto put = [&memory](std::uintptr_t at, const void* data, std::size_t size) {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            memory[at] = std::vector<std::uint8_t>(bytes, bytes + size);
        };
        auto read = [&memory](std::uintptr_t at, void* out, std::size_t size) {
            const auto found = memory.find(at);
            if (found == memory.end() || found->second.size() != size)
                return false;
            std::memcpy(out, found->second.data(), size);
            return true;
        };
        const std::uintptr_t base = 0x140000000ull;
        const std::uintptr_t ped = 0x2A0001230ull;
        const std::uintptr_t table = 0x320000000ull;
        const std::uintptr_t anim = 0x330000080ull;
        const std::uintptr_t director = 0x340000100ull;
        const std::uint32_t baseIndex = 0x10;
        const std::uint32_t slot = 0x18;
        const std::uint8_t pedType = kEntityTypePed;
        const std::uint32_t handle = 0x00C0000u | slot;
        const std::uintptr_t tagged = anim | 1u;
        put(ped + kEntityTypeOffset, &pedType, 1);
        put(ped + kComponentHandleOffset, &handle, 4);
        put(base + kAnimTableBaseIndexRva, &baseIndex, 4);
        put(base + kAnimTableRva, &table, 8);
        const std::uintptr_t row = table + static_cast<std::uintptr_t>(slot - baseIndex) * kAnimRowBytes;
        put(row, &tagged, 8);
        put(anim + kDirectorOffset, &director, 8);
        Check(DirectorOf(base, ped, read) == director, "the anim director of the ped (tag bit cleared, row of 16 bytes)");
        const std::uint8_t objectType = 5;
        put(ped + kEntityTypeOffset, &objectType, 1);
        Check(DirectorOf(base, ped, read) == 0, "an entity that is not a ped has no director here");
        put(ped + kEntityTypeOffset, &pedType, 1);
        const std::uint32_t farHandle = baseIndex + kMaxSlot;
        put(ped + kComponentHandleOffset, &farHandle, 4);
        Check(DirectorOf(base, ped, read) == 0, "a slot past any ped pool is refused");
        put(ped + kComponentHandleOffset, &handle, 4);
        const std::uintptr_t none = 0;
        put(row, &none, 8);
        Check(DirectorOf(base, ped, read) == 0, "no anim component: no director");
        Check(kMaxForced >= 256, "every recorded ped can be forced");
    }

    // The engine mount link as 0xCC857C reads it, and when a puppet's "mount"
    // chain is forwarded (entity_query.h).
    void TestEntityQuery()
    {
        using namespace replay::entityquery;
        std::map<std::uintptr_t, std::vector<std::uint8_t>> memory;
        auto put = [&memory](std::uintptr_t at, const void* data, std::size_t size) {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            memory[at] = std::vector<std::uint8_t>(bytes, bytes + size);
        };
        auto read = [&memory](std::uintptr_t at, void* out, std::size_t size) {
            const auto found = memory.find(at);
            if (found == memory.end() || found->second.size() != size)
                return false;
            std::memcpy(out, found->second.data(), size);
            return true;
        };
        const std::uintptr_t base = 0x140000000ull;
        const std::uintptr_t ped = 0x2A0001230ull;
        const std::uintptr_t table = 0x320000000ull;
        const std::uintptr_t ride = 0x350000040ull;
        const std::uintptr_t horse = 0x2A0004560ull;
        const std::uint32_t indexBase = 0x20;
        const std::uint32_t slot = 0x2C;
        const std::uint32_t handle = 0x00A0000u | slot;
        put(ped + kComponentHandleOffset, &handle, 4);
        put(base + kComponentIndexBaseRva, &indexBase, 4);
        put(base + kComponentTableRva, &table, 8);
        const std::uintptr_t row = table + static_cast<std::uintptr_t>(slot - indexBase) * kComponentRowBytes;
        Check(ComponentRow(base, ped, read) == row, "the component row (0x148 bytes, slot minus the index base)");
        const std::uintptr_t taggedRide = ride | 1u;
        put(row + kRowRideSlot, &taggedRide, 8);
        const std::uint32_t mounted = 0x13;
        put(ride + kRideFlagsOffset, &mounted, 4);
        put(ride + kRideMountOffset, &horse, 8);
        Check(EngineMountOf(base, ped, read) == horse, "mounted: flags bit 4, the mount at +0x178 (tag bit cleared)");
        const std::uint32_t climbing = 0x03;
        put(ride + kRideFlagsOffset, &climbing, 4);
        Check(EngineMountOf(base, ped, read) == 0, "the ride component without bit 4: not mounted");
        const std::uintptr_t none = 0;
        put(row + kRowRideSlot, &none, 8);
        Check(EngineMountOf(base, ped, read) == 0, "no ride component (a puppet): not mounted");
        const std::uint32_t outside = indexBase - 1;
        put(ped + kComponentHandleOffset, &outside, 4);
        Check(ComponentRow(base, ped, read) == 0, "a handle below the index base is refused");

        Check(ForwardsToMount(kMountHead, 3, 0, 0x2B0000000ull, true), "a puppet seated by the replay: forwarded");
        Check(!ForwardsToMount(kMountHead, 3, horse, 0x2B0000000ull, true), "the engine link holds: the engine answers");
        Check(!ForwardsToMount(kMountHead, 1, 0, 0x2B0000000ull, true), "a chain without a rest is not forwarded");
        Check(!ForwardsToMount(0x72277E17u, 3, 0, 0x2B0000000ull, true), "another head is not forwarded");
        Check(!ForwardsToMount(kMountHead, 3, 0, 0, true), "no seat in the replay: the engine answers");
        Check(!ForwardsToMount(kMountHead, 3, 0, 0x2B0000000ull, false), "a mount entity that is not a ped audio one is refused");

        Check(NullAnswer(0x000fffff00000000ull | 0x1234u, 0x1234u), "the null sound's name");
        Check(NullAnswer(0x000fffff00000000ull, 0x1234u) && NullAnswer(0x000fffffffffffffull, 0x1234u), "low half 0 or all ones");
        Check(!NullAnswer(0x000fffff000d9e35ull, 0x1234u), "a sound ref is an answer");

        Query q;
        q.n = 3;
        q.chain[0] = kMountHead;
        q.chain[1] = 0xA99DA182u;
        q.chain[2] = 0x12345678u;
        Check(ChainText(q) == "0xa07a1994>0xa99da182>0x12345678", "the chain text");
        q.n = kMaxChain + 2;
        Check(ChainText(q).size() > 3 && ChainText(q).substr(ChainText(q).size() - 4) == ">...", "a longer chain is cut");
    }

    // The recorded answers (entity_query_track.h).
    void TestEntityQueryTrack()
    {
        using namespace replay::entityquery;
        const std::uint32_t mountA[4] = {kMountHead, 0xA99DA182u, 0x23568AAEu, 0x68CD267Cu};
        const std::uint32_t mountB[4] = {kMountHead, 0xA99DA182u, 0x23568AAEu, 0xD4A63068u};
        const std::uint64_t nullRef = 0x000fffff00440dcbull;
        const std::uint64_t saddle = 0x0080000f000d9cecull;
        Track track;
        Check(track.Note(3566, 1, 4, mountA, nullRef), "the first answer of an owner and chain is kept");
        Check(!track.Note(3700, 1, 4, mountA, nullRef), "the same answer again is not");
        Check(track.Note(4891, 1, 4, mountA, saddle), "a changed answer is kept");
        Check(track.Note(4891, 1, 4, mountB, saddle), "another chain is another key");
        Check(track.Note(4900, 7, 4, mountA, nullRef), "another owner is another key");
        Check(track.chains.size() == 2 && track.events.size() == 4, "two chains, four changes");
        Check(track.chains[0].hash == ChainHash(4, mountA) && track.chains[0].hash != track.chains[1].hash, "chain hashes");
        Check(track.Note(4000, 1, 4, mountB, nullRef) && track.events.back().timeMs == 4900, "a late drain keeps the order");
        const std::uint32_t tooLong[kMaxChain + 1] = {};
        Check(!track.Note(5000, 1, kMaxChain + 1, tooLong, saddle) && track.droppedLong == 1, "a chain too long is left out");

        Track cut;
        CutInto(track, cut, 4000, 4895);
        // in force at 4000: owner 1 chain 0 = null (3566); inside: 4891 (1, A, saddle), 4891 (1, B, saddle).
        Check(cut.events.size() == 3 && cut.events[0].timeMs == 0 && cut.events[0].answer == nullRef &&
                  cut.events[1].timeMs == 891 && cut.events[2].timeMs == 891,
            "the cut: the answer in force at base moves to 0, the rest shifts, later ones go");
        Check(cut.Note(1000, 1, 4, mountA, saddle) == false, "after a cut the last answers are known again");

        Track trimmed = track;
        const std::size_t dropped = TrimBefore(trimmed, 4500);
        Check(dropped == 0 && trimmed.events.size() == 5 && trimmed.events[0].timeMs == 4500 && trimmed.events[0].answer == nullRef,
            "the trim keeps the answer in force at keepFrom (one event before it, moved)");
        Track trimmed2 = track;
        trimmed2.Note(4950, 1, 4, mountA, nullRef);
        Check(TrimBefore(trimmed2, 4920) == 2, "the trim folds the older answers of each key into the one in force (two dropped)");

        const std::vector<Timeline> lines = Timelines(track);
        std::uint64_t answer = 0;
        const Timeline* a = nullptr;
        for (const Timeline& t : lines)
            if (t.owner == 1 && t.chain == 0)
                a = &t;
        Check(a && !a->At(3000, answer), "no answer before the first query");
        Check(a && a->At(4000, answer) && answer == nullRef, "the null answer while climbing");
        Check(a && a->At(4891, answer) && answer == saddle && a->At(9000, answer) && answer == saddle, "the saddle once seated");
        Check(lines.size() == 3, "one timeline per owner and chain");
    }

    // A mission chest with no skeleton at the snapshot: the reread near an actor
    // turns a skeleton that appears into an animation, and takes a baseline for an
    // object the snapshot never read.
    void TestSkeletonReread()
    {
        using namespace replay::objcheck;
        Check(ShouldRereadSkeleton(SkeletonWatch::Absent, 0.8f, 1000, 1000, 0) &&
                  ShouldRereadSkeleton(SkeletonWatch::Unread, 3.0f, 1000, 500, 7),
            "reread: an object without bones near an actor, when due");
        Check(!ShouldRereadSkeleton(SkeletonWatch::Present, 0.8f, 1000, 0, 0), "reread: not an object whose locals are kept");
        Check(!ShouldRereadSkeleton(SkeletonWatch::Absent, 3.5f, 1000, 0, 0) && !ShouldRereadSkeleton(SkeletonWatch::Absent, -1.0f, 1000, 0, 0),
            "reread: not far from every recorded actor");
        Check(!ShouldRereadSkeleton(SkeletonWatch::Absent, 0.8f, 999, 1000, 0), "reread: not before it is due");
        Check(!ShouldRereadSkeleton(SkeletonWatch::Absent, 0.8f, 1000, 0, kSkeletonReadsPerScan), "reread: the per scan cap");
        Check(AfterReread(SkeletonWatch::Absent, true, 3, 0, true) == SkeletonReread::Appeared,
            "reread: a skeleton that appears on a scenery object is an animation (the chest's lid)");
        Check(AfterReread(SkeletonWatch::Absent, true, 3, 0, false) == SkeletonReread::Baseline,
            "reread: on a map object it is a baseline (recorded once its bones move, as the reference's map objects)");
        Check(AfterReread(SkeletonWatch::Unread, true, 3, 0, true) == SkeletonReread::Baseline &&
                  AfterReread(SkeletonWatch::Unread, true, 3, kMaxSkeletonBaselines, false) == SkeletonReread::Nothing,
            "reread: an object the snapshot never read takes a baseline, up to the cap");
        Check(AfterReread(SkeletonWatch::Absent, false, 0, 0, true) == SkeletonReread::Nothing &&
                  AfterReread(SkeletonWatch::Absent, true, 1, 0, true) == SkeletonReread::Nothing &&
                  AfterReread(SkeletonWatch::Present, true, 3, 0, true) == SkeletonReread::Nothing,
            "reread: no skeleton, a rigid one bone skeleton, or locals already kept, is nothing");
        Check(AfterRebuild(true) == SkeletonReread::Appeared && AfterRebuild(false) == SkeletonReread::Baseline,
            "reread: a skeleton rebuilt with another bone count is a new one (scenery: animation, map: baseline)");
        Check(NextSkeletonWatch(SkeletonWatch::Absent, SkeletonReread::Appeared, true) == SkeletonWatch::Present &&
                  NextSkeletonWatch(SkeletonWatch::Unread, SkeletonReread::Baseline, true) == SkeletonWatch::Present,
            "reread: locals kept from then on");
        Check(NextSkeletonWatch(SkeletonWatch::Unread, SkeletonReread::Nothing, false) == SkeletonWatch::Absent &&
                  NextSkeletonWatch(SkeletonWatch::Unread, SkeletonReread::Nothing, true) == SkeletonWatch::Unread,
            "reread: an unread object with no skeleton becomes absent (a later one is an animation); at the cap it stays unread");
    }

    // A chair (10 bones) taken by the spawned watch for its root (moved=1,
    // animated=0) must not be recorded rigid for good. A prop registered without a
    // pose keeps its bones watched and is posed when they move, a skeleton comes on
    // it or its skeleton is rebuilt.
    void TestPropBoneWatch()
    {
        using namespace replay::objcheck;
        namespace spawn = replay::object_pose::spawn;
        // The chair's register: moved first, so no pose at the register.
        Check(spawn::Decide(true, false, 174) == spawn::Verdict::Record && !spawn::PoseSpawned(false, 10),
            "bone watch: the chair is recorded for its root, without a pose (the 043 defect)");
        const bool poseable = replay::object_pose::PoseableProp(false, true, 0);
        Check(WatchPropBones(poseable, false, false, 0), "bone watch: a free prop without a pose is watched");
        Check(!WatchPropBones(poseable, true, false, 0) && !WatchPropBones(poseable, false, true, 0),
            "bone watch: not a posed prop, nor one waiting for a place or a skeleton");
        Check(!WatchPropBones(replay::object_pose::PoseableProp(false, true, 0x1234u), false, false, 0) &&
                  !WatchPropBones(replay::object_pose::PoseableProp(false, false, 0), false, false, 0),
            "bone watch: not a real weapon, nor a prop playback never poses (hat, gore or vehicle piece)");
        Check(WatchPropBones(poseable, false, false, kMaxPropBoneWatches - 1) &&
                  !WatchPropBones(poseable, false, false, kMaxPropBoneWatches),
            "bone watch: at most kMaxPropBoneWatches props at once");
        // Rigid arrows and thrown objects (absent) holding every place do not keep a
        // later prop with a skeleton out.
        Check(PropBoneWatchRoom(kMaxPropBoneWatches - 1, true, false) == PropBoneRoom::Watch &&
                  PropBoneWatchRoom(kMaxPropBoneWatches, false, true) == PropBoneRoom::Evict &&
                  PropBoneWatchRoom(kMaxPropBoneWatches, true, true) == PropBoneRoom::Full &&
                  PropBoneWatchRoom(kMaxPropBoneWatches, false, false) == PropBoneRoom::Full,
            "bone watch: full, a prop with a skeleton takes the oldest absent watch's place; one without is left out");
        Check(PropBonesDue(1250, 1250, 0) && !PropBonesDue(1249, 1250, 0) &&
                  !PropBonesDue(1250, 1250, kPropBoneReadsPerTick),
            "bone watch: a read when due, within the per tick reads");
        Check(PropBonesAtRegister(true, 10) == SkeletonWatch::Present && PropBonesAtRegister(false, 0) == SkeletonWatch::Absent &&
                  PropBonesAtRegister(false, 1) == SkeletonWatch::Absent && PropBonesAtRegister(false, 300) == SkeletonWatch::Unread,
            "bone watch: the register's state (locals kept, no skeleton, a skeleton not copied)");

        // The chair's 10 locals at the register (identity rotations), then
        // the tilt: bone 1 turned 10 degrees about x.
        std::vector<float> baseline(10 * spawn::kFloatsPerBone, 0.0f);
        for (std::size_t bone = 0; bone < 10; ++bone)
            baseline[bone * spawn::kFloatsPerBone + 3] = 1.0f;
        std::vector<float> tilted = baseline;
        const float half = 0.5f * 10.0f * 3.14159265f / 180.0f;
        tilted[1 * spawn::kFloatsPerBone + 0] = std::sin(half);
        tilted[1 * spawn::kFloatsPerBone + 3] = std::cos(half);
        const bool still = spawn::LocalsChanged(baseline.data(), baseline.data(), 10);
        const bool tilt = spawn::LocalsChanged(baseline.data(), tilted.data(), 10);
        Check(!still && tilt, "bone watch: the tilt is a local change");
        Check(AfterPropBoneRead(SkeletonWatch::Present, true, 10, 10, still) == PropBoneStep::Keep,
            "bone watch: the chair standing still stays watched");
        const PropBoneStep moved = AfterPropBoneRead(SkeletonWatch::Present, true, 10, 10, tilt);
        Check(moved == PropBoneStep::Moved && PropBonesPose(moved), "bone watch: the chair's bones moving pose it");
        Check(AfterPropBoneRead(SkeletonWatch::Present, true, 12, 10, false) == PropBoneStep::Rebuilt &&
                  PropBonesPose(PropBoneStep::Rebuilt),
            "bone watch: a skeleton rebuilt with another bone count poses it (task 50, recorded all along)");
        Check(AfterPropBoneRead(SkeletonWatch::Absent, true, 4, 0, false) == PropBoneStep::Appeared &&
                  PropBonesPose(PropBoneStep::Appeared),
            "bone watch: a skeleton that comes on a prop registered without one poses it (task 50)");
        Check(AfterPropBoneRead(SkeletonWatch::Unread, true, 10, 0, false) == PropBoneStep::Baseline &&
                  !PropBonesPose(PropBoneStep::Baseline),
            "bone watch: a skeleton not copied at the register takes a baseline at its first good read");
        Check(AfterPropBoneRead(SkeletonWatch::Present, false, 0, 10, false) == PropBoneStep::Keep &&
                  AfterPropBoneRead(SkeletonWatch::Absent, false, 1, 0, false) == PropBoneStep::Keep,
            "bone watch: a read that copied nothing decides nothing");
        Check(NextPropBoneWatch(SkeletonWatch::Present, false, 0) == SkeletonWatch::Absent &&
                  NextPropBoneWatch(SkeletonWatch::Present, false, 10) == SkeletonWatch::Present &&
                  NextPropBoneWatch(SkeletonWatch::Unread, true, 10) == SkeletonWatch::Present &&
                  NextPropBoneWatch(SkeletonWatch::Absent, false, 0) == SkeletonWatch::Absent,
            "bone watch: a skeleton gone is absent (its return poses), a failed copy keeps the state");
        Check(std::string(PropBoneStepText(PropBoneStep::Moved)) == "moved" &&
                  std::string(PropBoneStepText(PropBoneStep::Keep)) == "keep",
            "bone watch: the verdict names in the log");
    }

    // The lining and the loot attached to a chest, kept with their parent.
    void TestSceneryChildren()
    {
        using namespace replay::objcheck;
        Check(ChildAppearMs(true, 0, 1250) == 0 && ChildAppearMs(true, 4000, 5250) == 4000 && ChildAppearMs(false, 0, 1250) == 1250,
            "children: there when the parent appeared when seen in its first scan, else when seen");
        Check(!ChildLeft(true, true, false) && ChildLeft(false, true, false) && ChildLeft(true, false, false) &&
                  ChildLeft(true, true, true),
            "children: an entry leaves when gone, detached (picked up) or recorded by another path");
        const float zero[3]{0.0f, 0.0f, 0.0f};
        const float unit[4]{0.0f, 0.0f, 0.0f, 1.0f};
        const float far[3]{0.0f, 51.0f, 0.0f};
        const float flat[4]{0.0f, 0.0f, 0.0f, 0.5f};
        Check(FramePlausible(-1, zero, unit) && FramePlausible(0, zero, unit) && !FramePlausible(-2, zero, unit) &&
                  !FramePlausible(1024, zero, unit) && !FramePlausible(0, far, unit) && !FramePlausible(0, zero, flat),
            "children: the engine frame kept only when plausible");
        const float nan3[3]{std::nanf(""), 0.0f, 0.0f};
        Check(!FramePlausible(0, nan3, unit), "children: a NaN offset is not plausible");

        // The chest turned 90 degrees about z (heading 90), the lining 0.3 m
        // in front of it in the chest's frame (+y), turned with it: in the
        // world it stands at -0.3 x, and the relative frame gives back the
        // 0.3 m forward and no turn.
        const float h = 0.70710678f;
        const float parentPos[3]{10.0f, 20.0f, 5.0f};
        const float parentRot[4]{0.0f, 0.0f, h, h};
        const float childPos[3]{9.7f, 20.0f, 5.1f};
        const float childRot[4]{0.0f, 0.0f, h, h};
        float rp[3]{};
        float rq[4]{};
        RelativeFrame(parentPos, parentRot, childPos, childRot, rp, rq);
        const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
        Check(near(rp[0], 0.0f) && near(rp[1], 0.3f) && near(rp[2], 0.1f), "children: the offset in the parent's frame");
        Check(near(rq[0], 0.0f) && near(rq[1], 0.0f) && near(rq[2], 0.0f) && near(std::fabs(rq[3]), 1.0f),
            "children: no turn relative to a parent turned the same");
        // The child turned 90 degrees more than its parent: a quarter turn about z.
        const float childTurned[4]{0.0f, 0.0f, 1.0f, 0.0f};
        RelativeFrame(parentPos, parentRot, childPos, childTurned, rp, rq);
        Check(near(rq[0], 0.0f) && near(rq[1], 0.0f) && near(std::fabs(rq[2]), h) && near(std::fabs(rq[3]), h) &&
                  rq[2] * rq[3] > 0.0f,
            "children: the quarter turn relative to the parent");

        Check(ChooseChildHost(false, false, true) == ChildHost::StandIn && ChooseChildHost(true, true, true) == ChildHost::Prop &&
                  ChooseChildHost(true, false, true) == ChildHost::StandIn && ChooseChildHost(true, false, false) == ChildHost::None &&
                  ChooseChildHost(false, true, false) == ChildHost::None,
            "children: the prop hosts once it took over and is built, the stand-in while shown, nothing otherwise");
        Check(AttachByFrame(ChildHost::Prop, true, true) && !AttachByFrame(ChildHost::Prop, true, false) &&
                  !AttachByFrame(ChildHost::StandIn, true, true) && !AttachByFrame(ChildHost::Prop, false, true),
            "children: the engine frame only on a posed prop, else the parent frame at the root");
    }

    // The foley variant indices (ped_audio_state.h).
    void TestFoleyVariants()
    {
        using namespace replay::pedaudiostate;
        Check(Joaat("PED_VARIABLES") == kPedVariablesName && Joaat("is_player") == kIsPlayer,
            "the block's name and is_player are the joaat of their names (0x317AD88, 0x31A3C81)");
        Check(Joaat("player") == 0x6F0783F5u, "0x4A276B23 carries the bits of joaat(player) (0x31A402B)");
        Check(kComponentOffset == 0x210 && kVariantAOffset == 0xEC && kVariantBOffset == 0xF0 && kResourceOffset == 0x168,
            "layout: component +0x210, indices +0xEC/+0xF0, resource +0x168");

        const Variants undrawn;
        const Variants player{0, 0};
        const Variants npc{1, 2};
        Check(!undrawn.Drawn() && player.Drawn() && npc.Drawn(), "-1 is undrawn (constructor 0x3178DC6)");
        Check(!NeedsResource(0) && !NeedsResource(1) && NeedsResource(2) && NeedsResource(3) && !NeedsResource(4),
            "the activation requests +0x168 for an index 2 or 3 only (sub 2, cmp 1, jbe)");

        Check(Decide(npc, player, false) == Decision::Write, "the recorded player's puppet drew 1/2: write 0/0");
        Check(Decide(player, player, false) == Decision::Same, "already the target: nothing");
        Check(Decide(undrawn, npc, false) == Decision::Write, "an undrawn puppet takes any target (its activation follows it)");
        Check(Decide(Variants{1, 1}, npc, false) == Decision::NoResource,
            "a drawn puppet without the resource is not given an index 2 or 3");
        Check(Decide(Variants{1, 1}, npc, true) == Decision::Write, "with the resource held it is");
        Check(Decide(Variants{3, 1}, Variants{1, 1}, false) == Decision::Write, "a target without 2 or 3 needs no resource");
        Check(Decide(npc, undrawn, false) == Decision::NoTarget, "no target: nothing");
        Check(Decide(Variants{1000, 1}, player, false) == Decision::Implausible, "a value out of range: the layout is not ours");
        Check(Decide(npc, Variants{-5, 0}, false) == Decision::Implausible && Decide(npc, Variants{0, -2}, false) == Decision::Implausible,
            "a negative target is refused");

        VariantTrack track;
        Check(track.Note(0x00030001u, player, true), "the player's 0/0 is kept");
        Check(track.Note(0x00010002u, npc, false), "an NPC's draw is kept");
        Check(!track.Note(0x00010002u, Variants{2, 2}, false) && track.changed == 1, "an owner seen again keeps its first draw");
        Check(!track.Note(0x00050001u, undrawn, false), "an undrawn owner is not kept");
        Check(track.rows.size() == 2 && track.rows[0].owner == 0x00010002u && track.Valid(), "sorted by owner");
        Variants target;
        Check(TargetFor(track, 0x00010002u, false, player, target) == TargetSource::Recorded && target == npc,
            "a recorded owner gets its own draw");
        Check(TargetFor(track, 0x00030001u, true, Variants{}, target) == TargetSource::Recorded && target == player,
            "the recorded player gets his recorded 0/0");
        VariantTrack old;
        Check(TargetFor(old, 0x00030001u, true, player, target) == TargetSource::Player && target == player,
            "an old clip: the recorded player's puppet takes the live player's indices");
        Check(TargetFor(old, 0x00030001u, true, Variants{}, target) == TargetSource::Player && target == player,
            "the live player unreadable: the draw's own 0/0");
        Check(TargetFor(old, 0x00010002u, false, player, target) == TargetSource::None, "an old clip leaves the others to the engine");

        VariantTrack cut;
        CutInto(track, cut, [](std::uint32_t owner) { return owner == 0x00030001u; });
        Check(cut.rows.size() == 1 && cut.rows[0].player == 1, "the cut keeps the rows of the kept owners");
        VariantTrack bad = track;
        std::swap(bad.rows[0], bad.rows[1]);
        Check(!bad.Valid(), "unsorted rows are refused");
        bad = track;
        bad.rows[0].a = kMaxVariant + 1;
        Check(!bad.Valid(), "an index out of range is refused");
    }

    // The PED_VARIABLES block as 0x258A9B4 and 0x253708C address it, and the read
    // probe's range test (ped_audio_state.h).
    void TestPedVariablesBlock()
    {
        using namespace replay::pedaudiostate;
        std::map<std::uintptr_t, std::vector<std::uint8_t>> memory;
        auto put = [&memory](std::uintptr_t at, const void* data, std::size_t size) {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            memory[at] = std::vector<std::uint8_t>(bytes, bytes + size);
        };
        auto read = [&memory](std::uintptr_t at, void* out, std::size_t size) {
            const auto found = memory.find(at);
            if (found == memory.end() || found->second.size() != size)
                return false;
            std::memcpy(out, found->second.data(), size);
            return true;
        };
        const std::uintptr_t base = 0x140000000ull;
        const std::uintptr_t holder = 0x2A0001000ull;
        const std::uintptr_t descriptor = 0x330000000ull;
        const std::uintptr_t values = 0x331000000ull;
        const std::int16_t index = 7;
        put(holder + kHolderBlockIndexOffset, &index, 2);
        const std::uintptr_t entry = base + kBlockTableRva + 7 * kBlockEntryBytes;
        put(entry, &descriptor, 8);
        put(entry + kBlockValuesOffset, &values, 8);
        const std::uint32_t count = 3;
        put(descriptor + kDescriptorCountOffset, &count, 4);
        const std::uint32_t names[3] = {kIsPlayer, 0x8BAE9C31u, 0x12345678u};
        for (std::uint32_t n = 0; n < 3; ++n)
            put(descriptor + kDescriptorNamesOffset + n * kDescriptorNameStride, &names[n], 4);
        const float recordedValues[3] = {1.0f, 2.5f, 0.0f};
        put(values, recordedValues, sizeof(recordedValues));
        Block recorded;
        Check(ReadBlock(base, holder, read, recorded) && recorded.count == 3 && recorded.index == 7 && recorded.values == values &&
                  recorded.names[1] == 0x8BAE9C31u && recorded.data[1] == 2.5f,
            "the block: index word +0x12, entry {descriptor, values}, names every 8 bytes from +0xC");
        float value = 0.0f;
        Check(ValueOf(recorded, kIsPlayer, value) && value == 1.0f && !ValueOf(recorded, 0xDEADBEEFu, value), "a value by name");
        Check(BlockText(recorded) == "is_player=1;speed=2.5;0x12345678=0", "the log text labels the known names");

        const float puppetValues[3] = {0.0f, 2.5f, 0.0f};
        put(values, puppetValues, sizeof(puppetValues));
        Block puppet;
        Check(ReadBlock(base, holder, read, puppet), "the puppet's block");
        std::uint32_t differing = 0;
        Check(DiffText(recorded, puppet, differing) == "is_player:1>0" && differing == 1,
            "the difference: the recorded player's is_player 1, the puppet's 0");
        Check(BlockChanged(recorded, puppet) && !BlockChanged(puppet, puppet), "change detection");
        Check(DiffText(puppet, puppet, differing) == "none" && differing == 0, "no difference");

        const std::int16_t none = -1;
        put(holder + kHolderBlockIndexOffset, &none, 2);
        Check(!ReadBlock(base, holder, read, puppet), "no block (index -1): nothing");
        put(holder + kHolderBlockIndexOffset, &index, 2);
        const std::uint32_t tooMany = kMaxBlockVariables + 1;
        put(descriptor + kDescriptorCountOffset, &tooMany, 4);
        Check(!ReadBlock(base, holder, read, puppet), "a count past the cap is refused");

        BlockHistory history;
        Check(history.Note(100, recorded) && !history.Note(200, recorded) && history.Note(300, puppet), "history on change only");
        Check(history.At(50) == &history.samples[0].block && history.At(250) == &history.samples[0].block &&
                  history.At(400) == &history.samples[1].block,
            "the sample in force at the playhead");

        const Range ranges[2] = {{values, 3, 0x00030001u}, {0x332000000ull, 20, 0x00010002u}};
        Check(RangeOf(ranges, 2, values + 4) == 0 && RangeOf(ranges, 2, values + 12) == -1 &&
                  RangeOf(ranges, 2, values + 2) == -1 && RangeOf(ranges, 2, 0x332000000ull + 76) == 1,
            "the probe: a float inside a published block, aligned");
    }

    // The timecycle modifier and the weather variations: keys from the engine's
    // tables, the recorder's samples, the state at a playhead, cut and trim, and
    // the playback plan.
    void TestTimecycleTrack()
    {
        using namespace replay;
        using namespace replay::tctrack;
        // The engine's tables: three modifiers, the map sorted by hash.
        const std::uint32_t wind = Joaat("WINTER2_mountainWind");
        const std::uint32_t camp = Joaat("winter2_camp_clouds1");
        const std::uint32_t climb = Joaat("winter2_climb_clouds2");
        const std::vector<std::uint64_t> pointers{0x7FF100001000ull, 0x7FF100002000ull, 0x7FF100003000ull};
        std::vector<MapEntry> map{{wind, 0x7FF100002000ull}, {camp, 0x7FF100003000ull}, {climb, 0x7FF100001000ull}};
        std::sort(map.begin(), map.end(), [](const MapEntry& a, const MapEntry& b) { return a.key < b.key; });
        Check(KeyOfIndex(pointers, map, 1) == wind && KeyOfIndex(pointers, map, 0) == climb, "timecycle: index to key through the map");
        Check(KeyOfIndex(pointers, map, -1) == 0 && KeyOfIndex(pointers, map, 3) == 0, "timecycle: no key for none or out of range");
        Check(IndexOfKey(pointers, map, camp) == 2 && IndexOfKey(pointers, map, wind) == 1, "timecycle: key to index");
        Check(IndexOfKey(pointers, map, 0) == -1 && IndexOfKey(pointers, map, 0x12345678u) == -1, "timecycle: unknown key has no index");
        // Another session with the array in another order: the key finds its new index.
        const std::vector<std::uint64_t> moved{0x7FF100003000ull, 0x7FF100001000ull, 0x7FF100002000ull};
        Check(IndexOfKey(moved, map, wind) == 2, "timecycle: the key is what travels, not the index");

        // Variations: default ones are not stored, kept sorted by type.
        const std::uint32_t blizzard = Joaat("BLIZZARD");
        const std::uint32_t ground = Joaat("GROUNDBLIZZARD");
        TimecycleSample s;
        Check(AddVariation(s, blizzard, blizzard) && AddVariation(s, ground, 0) && s.variationCount == 0,
            "timecycle: a type on its own hash or 0 has no variation");
        AddVariation(s, ground, Joaat("groundblizzard_winter2"));
        AddVariation(s, blizzard, Joaat("blizzard_winter2"));
        Check(s.variationCount == 2 && s.variations[0].type < s.variations[1].type && Valid(s), "timecycle: variations sorted, valid");
        Check(VariationOf(s, blizzard) == Joaat("blizzard_winter2") && VariationOf(s, Joaat("SUNNY")) == 0, "timecycle: variation of a type");
        TimecycleSample full;
        for (std::uint32_t n = 1; n <= kMaxVariations; ++n)
            AddVariation(full, n * 16u, n * 16u + 1u);
        Check(!AddVariation(full, 0xFFFFFFF0u, 1u) && full.variationCount == kMaxVariations, "timecycle: the sample holds a fixed number");
        const std::vector<std::uint32_t> table{Joaat("blizzard_a"), Joaat("blizzard_winter2")};
        Check(VariationIndex(table, Joaat("blizzard_winter2")) == 1 && VariationIndex(table, 7u) == -1, "timecycle: variation index by key");
        Check(PlanVariation(blizzard, blizzard, 0, -1) == VariationStep::None, "timecycle: default wanted and live: nothing");
        Check(PlanVariation(blizzard, table[1], 0, -1) == VariationStep::Clear, "timecycle: default wanted, live set: clear");
        Check(PlanVariation(blizzard, blizzard, table[1], 1) == VariationStep::Set, "timecycle: set by index");
        Check(PlanVariation(blizzard, table[1], table[1], 1) == VariationStep::None, "timecycle: already there: nothing");
        Check(PlanVariation(blizzard, 0, table[1], -1) == VariationStep::Missing, "timecycle: unknown variation is reported");

        // The recorder: a held state is one sample; a ramp after it keeps the
        // last held poll so the interpolation does not stretch it back.
        std::vector<TimecycleSample> track;
        Recorder recorder;
        TimecycleSample p;
        p.modifierKey = wind;
        p.modifierIndex = 1;
        for (std::uint32_t t = 0; t <= 1000; t += 100)
        {
            p.timeMs = t;
            recorder.Poll(track, p);
        }
        Check(track.size() == 1, "timecycle: a held modifier is one sample");
        p.transitionKey = camp;
        p.transitionIndex = 2;
        p.speed = 0.2f;
        for (std::uint32_t t = 1100; t <= 1500; t += 100)
        {
            p.timeMs = t;
            p.progress = 0.2f * static_cast<float>(t - 1100) / 1000.0f;
            recorder.Poll(track, p);
        }
        Check(track.size() == 7 && track[1].timeMs == 1000 && track[2].timeMs == 1100 && track[1].transitionKey == 0,
            "timecycle: the held poll before the change is kept, then every poll of the transition");
        p.timeMs = 1550;
        Check(recorder.Finish(track, p) && track.back().timeMs == 1550, "timecycle: the state at the stop is the last sample");
        TimecycleSample at;
        Check(At(track, 500, at) && at.modifierKey == wind && at.transitionKey == 0 && at.progress == 0.0f,
            "timecycle: the held modifier before the transition");
        Check(At(track, 1250, at) && at.transitionKey == camp && std::fabs(at.progress - 0.03f) < 1e-5f,
            "timecycle: progress interpolated between polls");
        Check(At(track, 1050, at) && at.transitionKey == 0, "timecycle: the keys change at the sample, not halfway");
        Check(At(track, 99999, at) && at.timeMs == 99999 && at.transitionKey == camp, "timecycle: after the last sample, the last");

        const std::vector<TimecycleSample> cut = Cut(track, 1250, 1400);
        Check(cut.size() == 3 && cut[0].timeMs == 0 && std::fabs(cut[0].progress - 0.03f) < 1e-5f && cut[1].timeMs == 50 &&
                  cut[2].timeMs == 150,
            "timecycle: the cut starts with the state at its base");
        std::vector<TimecycleSample> trimmed = track;
        Check(DropBefore(trimmed, 1150) == 2 && trimmed.front().timeMs == 1100, "timecycle: the trim keeps the sample in force");

        // Playback plan: speed 0 (the playhead drives the progress), the
        // progress kept short of the strength, unknown keys written as none.
        TimecycleSample play = track[3]; // transition at 0.02
        bool unresolved = true;
        ModifierFields f = PlanModifier(play, 1, 2, &unresolved);
        Check(!unresolved && f.index == 1 && f.transitionIndex == 2 && f.speed == 0.0f && std::fabs(f.progress - 0.02f) < 1e-5f &&
                  f.fadeOut == 0,
            "timecycle: plan for a transition");
        play.progress = 5.0f;
        f = PlanModifier(play, 1, 2);
        Check(f.progress < f.strength, "timecycle: a transition is never written finished");
        f = PlanModifier(play, -1, 2, &unresolved);
        Check(unresolved && f.index == -1 && f.transitionIndex == 2, "timecycle: a modifier this session lacks is written as none");
        TimecycleSample fade;
        fade.modifierKey = wind;
        fade.fadeOut = 1;
        fade.progress = 0.0f;
        f = PlanModifier(fade, 4, -1);
        Check(f.fadeOut == 1 && f.progress > 0.0f && f.index == 4, "timecycle: a fade is never written finished");
        TimecycleSample none;
        f = PlanModifier(none, -1, -1, &unresolved);
        Check(!unresolved && f.index == -1 && f.transitionIndex == -1 && f.strength == kDefaultStrength && f.progress == 0.0f,
            "timecycle: no modifier in the clip clears the session's");
        TimecycleSample bad;
        bad.strength = std::numeric_limits<float>::quiet_NaN();
        Check(!Valid(bad) && !recorder.Poll(track, bad), "timecycle: a value that is not a number is not recorded");
    }

    // The ring buffer folds the visibility flips before keepFrom into the state.
    void TestVisibilityTrim()
    {
        std::uint8_t visible = 0;
        std::vector<std::uint32_t> flips{900, 1500, 3000};
        replay::objcheck::TrimVisibility(visible, flips, 2000);
        Check(visible == 0 && flips.size() == 1 && flips[0] == 3000, "trim: two flips before keepFrom fold back to hidden");
        replay::objcheck::TrimVisibility(visible, flips, 3500);
        Check(visible == 1 && flips.empty(), "trim: the last flip folds in");
    }

    // The loose piece of a fragment object. A crate-like tree: 0 the body; 1 and 2
    // under it; 3 and 4 under 1; 5 under 2; 6 and 7 under 5.
    void TestFragPiece()
    {
        using namespace replay::fragpiece;
        const std::int16_t parents[8] = {-1, 0, 0, 1, 1, 2, 5, 5};
        std::uint8_t bits[0x80] = {};
        bits[0] = 0xE0; // 5, 6, 7
        const std::vector<std::uint16_t> board = GroupsOfBits(bits, sizeof(bits));
        Check(board == std::vector<std::uint16_t>{5, 6, 7} && Root(board) == 5, "piece: the break's bitset gives its groups, the lowest is its root");
        bits[0x7F] = 0x80;
        Check(GroupsOfBits(bits, sizeof(bits)).back() == 1023, "piece: the last bit of the engine's bitset is group 1023");
        Check(GroupsOfBits(nullptr, 0).empty() && Root({}) == -1, "piece: nothing set, no root");

        Check(ValidGroups({5, 6, 7}) && !ValidGroups({}) && !ValidGroups({6, 5}) && !ValidGroups({5, 5}) && !ValidGroups({1024}),
            "piece: a recorded set is not empty, strictly ascending and inside the bitset");

        Plan plan = PlanPiece(parents, 8, {5, 6, 7});
        Check(plan.ok && plan.root == 5 && plan.breakOff == 5 && plan.remove.empty() && plan.stray == 0,
            "piece: the whole subtree breaks off as it is (QA mark 08: only the board, not the crate)");
        plan = PlanPiece(parents, 8, {5, 7});
        Check(plan.ok && plan.breakOff == 5 && plan.remove == std::vector<std::uint16_t>{6},
            "piece: a group already gone from the subtree is taken off the new piece");
        plan = PlanPiece(parents, 8, {5});
        Check(plan.ok && plan.remove == std::vector<std::uint16_t>{6, 7}, "piece: a bare root loses both children");
        plan = PlanPiece(parents, 8, {2});
        Check(plan.ok && plan.breakOff == 2 && plan.remove == std::vector<std::uint16_t>{5},
            "piece: only the topmost missing group is taken off (what is under it goes with it)");
        plan = PlanPiece(parents, 8, {0, 1, 3});
        Check(plan.ok && plan.breakOff == -1 && plan.remove == std::vector<std::uint16_t>{2, 4},
            "piece: holding the type's root, nothing breaks off and the missing groups are taken off");
        plan = PlanPiece(parents, 8, {3, 5});
        Check(plan.ok && plan.root == 3 && plan.stray == 1 && plan.remove.empty(), "piece: a group outside the root's subtree is stray");
        Check(!PlanPiece(parents, 8, {}).ok && !PlanPiece(parents, 8, {8}).ok && !PlanPiece(nullptr, 8, {5}).ok &&
                  !PlanPiece(parents, 0, {5}).ok && !PlanPiece(parents, 8, {6, 5}).ok,
            "piece: no plan without a valid set inside the type");
        // A parent that is not lower than its child ends the branch.
        const std::int16_t malformed[8] = {-1, 0, 0, 1, 1, 2, 7, 5};
        const std::vector<std::uint8_t> under = Subtree(malformed, 8, 5);
        Check(under[5] && !under[6] && under[7] && !under[2], "piece: a malformed parent is not under the root");
        // The mark 08 case: a crate whose loose piece is one leaf (group 14).
        std::int16_t crate[20];
        for (int g = 0; g < 20; ++g)
            crate[g] = static_cast<std::int16_t>(g == 0 ? -1 : g < 4 ? 0 : (g - 4) / 4 + 1);
        plan = PlanPiece(crate, 20, {14});
        Check(plan.ok && plan.breakOff == 14 && plan.remove.empty(), "piece: the crate's loose board breaks off alone");

        std::uint8_t gone[kMaxGroups / 8] = {};
        Check(CarriedFromCache(parents, 8, 5, gone) == std::vector<std::uint16_t>{5, 6, 7},
            "cache: the piece's root and every group under it still present");
        gone[0] = 0x40; // 6
        Check(CarriedFromCache(parents, 8, 5, gone) == std::vector<std::uint16_t>{5, 7}, "cache: a gone slot is not carried");
        gone[0] = 0x20; // 5
        Check(CarriedFromCache(parents, 8, 5, gone).empty(), "cache: a gone root carries nothing");
        gone[0] = 0x04; // 2: the groups outside the root are never read as carried
        Check(CarriedFromCache(parents, 8, 5, gone) == std::vector<std::uint16_t>{5, 6, 7} &&
                  CarriedFromCache(parents, 8, 8, gone).empty() && CarriedFromCache(parents, 8, -1, gone).empty(),
            "cache: only the root's subtree, the root inside the type");
        Check(std::string_view(SourceText(0)) == "break" && std::string_view(SourceText(1)) == "cache" &&
                  std::string_view(SourceText(9)) == "?",
            "piece: source names for the log");

        // The wagon dynamite bench: a wagon wheel, hub 1 under
        // the root and its parts 2..5 under the hub.
        const std::int16_t wheel[6] = {-1, 0, 1, 1, 1, 1};
        Check(Under(wheel, 6, 3, 1) && Under(wheel, 6, 3, 0) && !Under(wheel, 6, 1, 3) && !Under(wheel, 6, 1, 1) &&
                  !Under(wheel, 6, 6, 1) && !Under(nullptr, 6, 3, 1) && Under(malformed, 8, 7, 2) && !Under(malformed, 8, 6, 2),
            "order: a group lies under its parents, not under itself or its children");
        // Rear wheel: the hub came off with every part (26:29), then the parts
        // broke off that piece (25:28, 27:30): the hub piece goes first.
        Check(CameOffBefore(wheel, 6, {5}, {1, 2, 3, 4, 5}) && !CameOffBefore(wheel, 6, {1, 2, 3, 4, 5}, {5}),
            "order: the piece that carried this root comes off first");
        // Front wheel: part 2 (31:34) left before the hub (30:33 carries 1, 3, 5):
        // the part goes first, from the wheel (the hub first would take it along).
        Check(CameOffBefore(wheel, 6, {1, 3, 5}, {2}) && !CameOffBefore(wheel, 6, {2}, {1, 3, 5}),
            "order: a part the hub piece did not carry left the wheel before it");
        // Siblings and the same root do not order each other.
        Check(!CameOffBefore(wheel, 6, {2}, {4}) && !CameOffBefore(wheel, 6, {4}, {2}) && !CameOffBefore(wheel, 6, {2}, {2}) &&
                  !CameOffBefore(wheel, 6, {}, {2}) && !CameOffBefore(wheel, 6, {2}, {9}),
            "order: siblings, one root, no root or a root outside the type: no order");
    }
}

namespace
{
    // A shelf: the cigar hidden at 135.8 s with Arthur 0.49 m from it, the clip
    // played after the loot.
    void TestLoot()
    {
        using namespace replay::objcheck;
        // Who is scenery: a pickup whoever owns it, a script's or temporary object,
        // never a rope's entity; the map's loot is not.
        Check(SceneryCandidate(false, 14, true, false), "a pickup owned by the map is scenery");
        Check(SceneryCandidate(true, 14, false, false) && SceneryCandidate(false, kOwnerMission, false, false) &&
                SceneryCandidate(false, kOwnerTempA, false, false),
            "a script's or a temporary object is scenery as before");
        Check(!SceneryCandidate(false, 14, false, false), "the map's loot is not scenery (the watch's)");
        Check(!SceneryCandidate(true, kOwnerMission, true, true), "a rope's entity never");
        // The watch: free, drawn, a recorded ped within reach, the player near.
        Check(LootWatchable(false, false, true, 0.49f, 0.49f), "the shelf's cigar at Arthur's hand is watched");
        Check(!LootWatchable(true, false, true, 0.49f, 0.49f), "scenery is not watched (its own path)");
        Check(!LootWatchable(false, true, true, 0.49f, 0.49f), "an attached object is not watched");
        Check(!LootWatchable(false, false, false, 0.49f, 0.49f), "a hidden object is not watched");
        Check(!LootWatchable(false, false, true, kLootReachM + 0.01f, 1.0f), "out of reach: not watched");
        Check(!LootWatchable(false, false, true, 1.0f, kLootPlayerRadiusM + 1.0f), "the player far: not watched");
        Check(!LootWatchable(false, false, true, -1.0f, 1.0f), "no actor position: not watched");
        Check(LootKeepWatching(4.0f) && !LootKeepWatching(kLootUnwatchM + 0.1f) && !LootKeepWatching(-1.0f),
            "the watch lets go once every ped walked away");
        // Still in place: the hidden shelf original stays; a carried pelt leaves the
        // spot and is not watched any more.
        const float spot[3]{-1896.3f, 1335.56f, 199.91f};
        const float same[3]{-1896.3f, 1335.6f, 199.95f};
        const float carried[3]{-1896.0f, 1335.2f, 200.9f};
        Check(LootInPlace(spot, same) && !LootInPlace(spot, carried), "in place within the settle distance only");
        // The take.
        Check(LootTakeOf(true, true, 0.5f, 0.5f) == LootTake::None, "still drawn: no take");
        Check(LootTakeOf(true, false, 0.5f, 0.5f) == LootTake::Hidden, "hidden at the hand: taken");
        Check(LootTakeOf(false, false, 0.5f, 0.5f) == LootTake::Gone, "removed at the hand: taken");
        Check(LootTakeOf(true, false, 6.0f, 0.5f) == LootTake::Dropped, "hidden with nobody within reach: not a take");
        Check(LootTakeOf(true, false, 0.5f, 20.0f) == LootTake::Hidden, "hidden by an NPC 20 m from the player: taken");
        Check(LootTakeOf(false, false, 0.5f, 20.0f) == LootTake::Dropped,
            "removed 20 m from the player: maybe the streaming, not a take");
        Check(LootTakeOf(false, false, 0.5f, -1.0f) == LootTake::Dropped, "no player position: not a take");
        // The entry: drawn until the take.
        std::uint32_t gone = 0;
        std::uint8_t visible = 0;
        std::vector<std::uint32_t> flips{1, 2};
        LootLife(LootTake::Hidden, 135800, gone, visible, flips);
        Check(gone == kNeverGone && visible == 1 && flips.size() == 1 && flips[0] == 135800, "hidden: one flip at the take");
        Check(SceneryShown(0, gone, false, 0, false, 120000) && VisibleAt(visible != 0, flips, 120000),
            "the stand-in is on the shelf before the take (the QA's mark at 145.4 s: the item 1.3 s before its take)");
        Check(!VisibleAt(visible != 0, flips, 135800) && !VisibleAt(visible != 0, flips, 170000), "hidden from the take on");
        // The drop search takes the hidden original as the hand copy's
        // replacement at 136.85 s: the prop takes over after the flip.
        Check(!SceneryShown(0, gone, true, 136853, true, 137000), "the prop took over");
        LootLife(LootTake::Gone, 5000, gone, visible, flips);
        Check(gone == 5000 && visible == 1 && flips.empty(), "removed: gone at the take");
        Check(SceneryShown(0, gone, false, 0, false, 4999) && !SceneryShown(0, gone, false, 0, false, 5000), "alive until the take");
        // The buffer window: a take before the window's start leaves the
        // stand-in hidden the whole clip; one inside it stays a flip.
        LootLife(LootTake::Hidden, 135800, gone, visible, flips);
        std::uint8_t cutVisible = visible;
        std::vector<std::uint32_t> cutFlips = flips;
        CutVisibility(cutVisible, cutFlips, 140000, 200000);
        Check(cutVisible == 0 && cutFlips.empty(), "window after the take: hidden from its start");
        cutVisible = visible;
        cutFlips = flips;
        CutVisibility(cutVisible, cutFlips, 100000, 200000);
        Check(cutVisible == 1 && cutFlips.size() == 1 && cutFlips[0] == 35800, "window across the take: the flip moved");
        // When it appears: the snapshot's and the map's from the start, any
        // other from its first sight.
        Check(LootAppearMs(true, false, 9000) == 0 && LootAppearMs(false, true, 9000) == 0 && LootAppearMs(false, false, 9000) == 9000,
            "appear: snapshot and map from 0, a late object from its first sight");
        // In a clip: the map's loot (owner 14) is a loot entry; a camp's or a
        // bench object (mission) is not; an unread owner is not.
        Check(IsLootEntry(false, 14) && !IsLootEntry(true, 14) && !IsLootEntry(false, kOwnerMission) && !IsLootEntry(false, kOwnerUnknown),
            "loot entries told by their owner");
        // Review: the playback's original search takes the closest object of
        // the model. A row of three cans 0.25 m apart, the middle one taken
        // (stand 0 at x 0.25): listed first, the left can (object 0, 0.25 m
        // off) must not be hidden in place of the middle one (object 1,
        // 0.01 m off, the hidden original).
        {
            const std::vector<OriginalPair> row{{0, 0, 0.0625f}, {0, 1, 0.0001f}, {0, 2, 0.0625f}};
            const auto chosen = NearestOriginals(row);
            Check(chosen.size() == 1 && chosen[0].first == 0 && chosen[0].second == 1, "the taken can's own original, not its neighbor");
            // Two stands of the model (two cans taken): each gets its own
            // object, the closest pairs first.
            const std::vector<OriginalPair> two{{0, 0, 0.04f}, {0, 1, 0.0001f}, {1, 1, 0.0004f}, {1, 0, 0.0002f}};
            const auto both = NearestOriginals(two);
            Check(both.size() == 2 && both[0] == std::make_pair<std::size_t, std::size_t>(0, 1) &&
                    both[1] == std::make_pair<std::size_t, std::size_t>(1, 0),
                "two stands, one object each, nearest first");
            Check(NearestOriginals({}).empty(), "no candidates, nothing hidden");
        }
        // The bench ops.
        replay::bench::Script script;
        std::string error;
        Check(replay::bench::ParseScript("pickup_spawn r1 PICKUP_WEAPON_REVOLVER_CATTLEMAN -300 780 118.5\npickup_remove r1\n"
                                         "world_visible nearest 8 pick\nworld_visible nearest 2 off\nworld_visible P_CIGAR02X 1.5 on\n",
                  script, error) &&
                script.steps.size() == 5,
            "loot bench ops parse");
        Check(!replay::bench::ParseScript("pickup_spawn r1 WEAPON_REVOLVER_CATTLEMAN 0 0 0\n", script, error), "a pickup name starts with PICKUP_");
        Check(!replay::bench::ParseScript("world_visible nearest 20 off\n", script, error), "world_visible radius at most 10 m");
        Check(!replay::bench::ParseScript("world_visible nearest 2 hide\n", script, error), "world_visible on or off");
    }

    // A camp's prop set objects and a mission camp reached later (at 996;-1218).
    void TestLateScenery()
    {
        using namespace replay::objcheck;
        // The prop set's owner (23, the spawner 0x7FD248) is scenery; the map's owners
        // are not.
        Check(IsSceneryOwner(false, kOwnerPropSet) && SceneryCandidate(false, kOwnerPropSet, false, false),
            "a prop set's object is scenery");
        Check(!SceneryCandidate(false, kOwnerPropSet, false, true), "a prop set's rope entity never");
        Check(!IsSceneryOwner(false, 14) && !IsSceneryOwner(false, 25) && !IsLootEntry(false, kOwnerPropSet),
            "the map stays the map; a prop set's object is no loot entry");
        // The map's owners: the map data owners proven from the game's code; the
        // learned owner does not add to them.
        Check(MapOwnerMask(kOwnerUnknown) == kWorldOwnerMask && (MapOwnerMask(4) & (1u << 4)) == 0 &&
                (MapOwnerMask(25) & (1u << 25)) != 0 && MapOwnerMask() == kMapDataOwnerMask,
            "map owners: the map data owners, the learned owner adds nothing");
        const std::uint32_t mask = MapOwnerMask(25);
        Check(SpawnSceneryOwner(false, kOwnerPropSet, false, mask) && SpawnSceneryOwner(false, kOwnerMission, false, mask) &&
                SpawnSceneryOwner(true, 25, false, mask) && SpawnSceneryOwner(false, kOwnerTempA, false, mask),
            "watch: a prop set's, a script's, a mission entity and a temporary object qualify");
        Check(SpawnSceneryOwner(false, 4, false, mask) && SpawnSceneryOwner(false, 0, false, mask),
            "watch: any other creator's object qualifies (no owner rule, as the reference)");
        Check(!SpawnSceneryOwner(false, 14, false, mask) && !SpawnSceneryOwner(false, 17, false, mask) &&
                !SpawnSceneryOwner(false, 24, false, mask) && !SpawnSceneryOwner(false, 25, false, mask),
            "watch: a streamed map object is left to the map");
        Check(SpawnSceneryOwner(false, 4, false, MapOwnerMask(4)),
            "watch: a learned owner that is no map data (4, the persistence system) is no longer the map's");
        Check(!SpawnSceneryOwner(false, kOwnerUnknown, false, mask) && !SpawnSceneryOwner(false, 40, false, mask),
            "watch: an unread owner never");
        Check(!SpawnSceneryOwner(false, kOwnerPropSet, true, mask) && !SpawnSceneryOwner(true, 4, true, mask), "watch: a rope's entity never");
        // The verdict: still for a second, once.
        Check(SpawnSceneryVerdict(kSpawnSceneryAfterMs - 1, false, true, 0) == SpawnScenery::Wait, "not still a second yet: wait");
        Check(SpawnSceneryVerdict(kSpawnSceneryAfterMs, false, true, 0) == SpawnScenery::Add, "still a second: an entry");
        Check(SpawnSceneryVerdict(5000, true, true, 0) == SpawnScenery::Already &&
                SpawnSceneryVerdict(5000, true, false, 0) == SpawnScenery::Already,
            "the scenery has it already: no second stand-in");
        Check(SpawnSceneryVerdict(5000, false, false, 0) == SpawnScenery::Map, "the map's: left");
        Check(SpawnSceneryVerdict(5000, false, true, kMaxSpawnScenery - 1) == SpawnScenery::Add &&
                SpawnSceneryVerdict(5000, false, true, kMaxSpawnScenery) == SpawnScenery::Full,
            "the watch's cap");
        Check(std::string_view(SpawnSceneryText(SpawnScenery::Map)) == "map" &&
                std::string_view(SpawnSceneryText(SpawnScenery::Already)) == "scenery_already",
            "verdict names for the log");
        // A tent P_TENTARMYPUP01X first seen at 31105 ms, static at 51288: an entry
        // from its first sight, so a mark at 35071 has the tent.
        {
            const std::uint32_t first = 31105;
            const std::uint32_t decidedAt = first + kSpawnSceneryAfterMs;
            Check(SpawnSceneryVerdict(decidedAt - first, false, SpawnSceneryOwner(false, kOwnerPropSet, false, mask), 0) ==
                    SpawnScenery::Add,
                "D09: the tent is an entry 1 s after its first sight");
            Check(SceneryShown(first, kNeverGone, false, 0, false, 35071) && !SceneryShown(first, kNeverGone, false, 0, false, 31000),
                "D09: drawn at the QA's mark, not before it was set up");
            // A buffer clip saved from 20 s: the entry moves to the window.
            std::uint32_t appear = first;
            std::uint32_t gone = kNeverGone;
            Check(CutLife(appear, gone, 20000, 60000) && appear == 11105 && gone == kNeverGone, "D09: cut to a buffer window");
        }
        // The far table. Looked at every scan until seen far, then once a second; an
        // entry appears from the first sight when it stood there.
        Check(SceneryLookDue(false, 0, 0) && !SceneryLookDue(true, 999, 1000) && SceneryLookDue(true, 1000, 1000),
            "far: looked at again once due");
        Check(SceneryLateAppearMs(true, 5000, false, 80000) == 5000, "far and unmoved: from the first sight");
        Check(SceneryLateAppearMs(true, 5000, true, 80000) == 80000, "far and moved since: from now");
        Check(SceneryLateAppearMs(false, 5000, false, 80000) == 80000, "first seen within the radius: from now");
        Check(SceneryLateAppearMs(true, 90000, false, 80000) == 80000, "a first sight after now (a reset clock) is not used");
        // A camp stool 120 m away at 5 s; the player rides in at 8 m/s from 10 s; scans
        // four times a second. Marked seen at the first scan (out of the radius) it
        // would never be an entry: it is decided at the first scan within 60 m, and the
        // object is looked at about once a second meanwhile.
        {
            bool seenFar = false;
            std::uint32_t nextMs = 0, firstMs = 0, looks = 0, decidedMs = 0, appearMs = 0;
            for (std::uint32_t nowMs = 5000; nowMs <= 30000 && !decidedMs; nowMs += 250)
            {
                if (!SceneryLookDue(seenFar, nowMs, nextMs))
                    continue;
                ++looks;
                const float ridden = nowMs > 10000 ? 8.0f * static_cast<float>(nowMs - 10000) / 1000.0f : 0.0f;
                const float distance = 120.0f - ridden;
                if (distance > kSceneryRadiusM)
                {
                    if (!seenFar)
                        firstMs = nowMs;
                    seenFar = true;
                    nextMs = nowMs + kSceneryFarRecheckMs;
                    continue;
                }
                decidedMs = nowMs;
                appearMs = SceneryLateAppearMs(seenFar, firstMs, false, nowMs);
            }
            Check(decidedMs >= 17500 && decidedMs <= 18500, "D03: decided when the player comes within 60 m (17.5 s)");
            Check(appearMs == 5000, "D03: the camp appears from its first sight, not at 60 m");
            Check(looks <= 15, "D03: about one look a second while far (the cost stays bounded)");
            Check(SceneryShown(appearMs, kNeverGone, false, 0, false, 88370), "D03: the stool is drawn at the QA's mark");
        }
        // The bench ops.
        replay::bench::Script script;
        std::string error;
        Check(replay::bench::ParseScript("propset_spawn tenda PG_PLAYER_CAMPFIRE_ADD02X_TENTBED -812 -472 44 90\n"
                                         "propset_spawn banco PG_PLAYER_CAMPFIRE_ADD03X_STOOL -812 -476 44\n"
                                         "propset_census tenda\npropset_delete tenda\n",
                  script, error) &&
                script.steps.size() == 4,
            "prop set bench ops parse");
        Check(!replay::bench::ParseScript("propset_spawn t P_TENTARMYPUP01X 0 0 0\n", script, error), "a prop set name starts with PG_");
        Check(!replay::bench::ParseScript("propset_spawn t PG_PLAYER_CAMPFIRE_ADD02X_TENTBED 0 0 0 999\n", script, error),
            "a heading in degrees");
        Check(!replay::bench::ParseScript("propset_spawn t PG_PLAYER_CAMPFIRE_ADD02X_TENTBED 0 x 0\n", script, error),
            "coordinates are numbers");
    }

    // The recorded voice's bank of an animal, loaded and held ahead of its lines
    // (animal_bank.h).
    void TestAnimalBank()
    {
        namespace bank = replay::animalbank;
        // Horse voices are the bank names' hashes.
        Check(bank::BankVoice("HORSE", 1) == 0x6D4D0CB7u, "bank: HORSE_NEAR_01 is the voice 0x6D4D0CB7");
        Check(bank::BankVoice("HORSE", 2) == 0xC66C3EF8u, "bank: HORSE_NEAR_02 is the voice 0xC66C3EF8 (005, 0 to 40 s)");
        Check(bank::BankVoice("HORSE", 3) == 0x94C95BAFu, "bank: HORSE_NEAR_03 is the voice 0x94C95BAF (005, after 40 s)");
        Check(bank::BankVoice("HORSE", 0) == 0 && bank::BankVoice("", 1) == 0 && bank::BankVoice("HORSE", 100) == 0,
            "bank: no bank 0, no empty name, at most 99");
        Check(bank::NameHash("horse_near_02") == bank::NameHash("HORSE_NEAR_02"), "bank: the name hash is lower-cased");
        Check(bank::PlausibleName("HORSE", 32) && bank::PlausibleName("DOG_2", 32) && !bank::PlausibleName("", 32) &&
                !bank::PlausibleName("Horse", 32) && !bank::PlausibleName("HORSE", 5),
            "bank: a type name is upper case, digits and underscores, terminated");

        std::vector<bank::TypeBanks> types(4);
        types[0].type = bank::kHorseType;
        std::snprintf(types[0].name, sizeof(types[0].name), "HORSE");
        types[0].count = 4;
        types[1].type = 5;
        std::snprintf(types[1].name, sizeof(types[1].name), "DOG");
        types[1].count = 2;
        types[2].type = 1; // not loadable (the loader takes 2 to 0x6B)
        std::snprintf(types[2].name, sizeof(types[2].name), "COW");
        types[2].count = 3;
        types[3].type = 6;
        std::snprintf(types[3].name, sizeof(types[3].name), "CAT");
        types[3].count = 0;
        const bank::VoiceMap map = bank::BuildVoiceMap(types);
        const auto horse2 = map.find(0xC66C3EF8u);
        Check(map.size() == 6, "bank map: four horse banks and two dog banks");
        Check(horse2 != map.end() && horse2->second.type == bank::kHorseType && horse2->second.bank == 2,
            "bank map: 0xC66C3EF8 is bank 2 of the horse type 0x3B");
        Check(map.count(bank::BankVoice("COW", 1)) == 0 && map.count(bank::BankVoice("CAT", 1)) == 0,
            "bank map: types the loader never loads or without banks are left out");

        const std::uint32_t v2 = bank::BankVoice("HORSE", 2);
        const std::uint32_t v3 = bank::BankVoice("HORSE", 3);
        const std::uint32_t dog = bank::BankVoice("DOG", 1);
        std::vector<bank::Due> due{{39996, v3, bank::kHorseType, 3}, {13, v2, bank::kHorseType, 2},
            {500, v2, bank::kHorseType, 2}, {2000, dog, 5, 1}, {39900, v2, bank::kHorseType, 2},
            {40500, v3, bank::kHorseType, 3}};
        bank::SortDue(due);
        Check(due.front().timeMs == 13 && due.back().timeMs == 40500, "plan: lines sorted by time");
        std::vector<bank::Hold> holds = bank::PlanHolds(due, 0, 4000);
        Check(holds.size() == 2 && holds[0].type == bank::kHorseType && holds[0].voice == v2 && holds[0].bank == 2 &&
                holds[1].type == 5 && holds[1].voice == dog,
            "plan: at the start the horse holds bank 2 and the dog its bank (both due within the lead)");
        holds = bank::PlanHolds(due, 39000, 4000);
        Check(holds.size() == 1 && holds[0].voice == v2 && holds[0].firstMs == 39900,
            "plan: bank 2 is kept while its last line is still ahead");
        holds = bank::PlanHolds(due, 39950, 4000);
        Check(holds.size() == 1 && holds[0].voice == v3 && holds[0].bank == 3,
            "plan: bank 3 is asked right after the last line of bank 2 was due");
        Check(bank::PlanHolds(due, 41000, 4000).empty() && bank::PlanHolds(due, 3000, 500).empty(),
            "plan: nothing held without a line within the lead");

        // The picker detour's decision.
        bank::HoldView view;
        view.hold = bank::Hold{bank::kHorseType, v3, 3, 39996};
        view.loadedVoice = v2;
        view.count = 4;
        view.kept = true;
        bank::PickDecision decision = bank::DecidePick(0, &view, 1);
        Check(decision.kind == bank::PickKind::Force && decision.type == bank::kHorseType && decision.bank == 3,
            "pick: a held type without its voice is returned with its bank number");
        view.loadedVoice = v3;
        decision = bank::DecidePick(bank::kHorseType, &view, 1);
        Check(decision.kind == bank::PickKind::Keep && decision.type == 0,
            "pick: the engine rotating a type that holds its voice gets no type");
        decision = bank::DecidePick(7, &view, 1);
        Check(decision.kind == bank::PickKind::Pass && decision.type == 7, "pick: another type passes");
        view.loadedVoice = v2;
        view.kept = false;
        decision = bank::DecidePick(7, &view, 1);
        Check(decision.kind == bank::PickKind::Pass && decision.type == 7,
            "pick: a type the manager would release is not forced (no load and release loop)");
        view.kept = true;
        view.count = 2;
        decision = bank::DecidePick(7, &view, 1);
        Check(decision.kind == bank::PickKind::Pass, "pick: a bank number past the type's count is never written");
        bank::HoldView two[2];
        two[0].hold = bank::Hold{5, dog, 1, 0};
        two[0].loadedVoice = dog;
        two[0].count = 2;
        two[0].kept = true;
        two[1] = view;
        two[1].count = 4;
        decision = bank::DecidePick(5, two, 2);
        Check(decision.kind == bank::PickKind::Force && decision.type == bank::kHorseType,
            "pick: a missing bank is loaded before a kept one is protected");
        const bank::Hold packed = bank::Unpack(bank::Pack(bank::Hold{bank::kHorseType, v3, 3, 0}));
        Check(packed.type == bank::kHorseType && packed.voice == v3 && packed.bank == 3, "pick: holds pack and unpack");

        // The script thread's follow.
        std::vector<bank::HoldState> states;
        std::uint32_t loaded = v2;
        const auto loadedOf = [&loaded](int) { return loaded; };
        std::vector<bank::Hold> plan{bank::Hold{bank::kHorseType, v3, 3, 39996}};
        bank::FollowEvents events = bank::Follow(states, plan, 1000, loadedOf);
        Check(events.asked.size() == 1 && events.loaded.empty() && states.size() == 1, "follow: a new hold is asked");
        events = bank::Follow(states, plan, 1300, loadedOf);
        Check(events.asked.empty() && events.loaded.empty() && events.gaveUp.empty(), "follow: still loading");
        loaded = v3;
        events = bank::Follow(states, plan, 1450, loadedOf);
        Check(events.loaded.size() == 1 && events.loaded[0].loadedMs - events.loaded[0].askedMs == 450,
            "follow: loaded 450 ms after the ask");
        Check(bank::Active(states).size() == 1, "follow: a loaded hold stays active (the detour keeps it)");
        events = bank::Follow(states, {}, 2000, loadedOf);
        Check(events.released.size() == 1 && states.empty(), "follow: released when no line asks for it");
        loaded = v2;
        bank::Follow(states, plan, 3000, loadedOf);
        events = bank::Follow(states, plan, 3000 + bank::kGiveUpMs, loadedOf);
        Check(events.gaveUp.size() == 1 && bank::Active(states).empty(), "follow: given up after the wait, the manager left alone");
        events = bank::Follow(states, plan, 3000 + bank::kGiveUpMs + bank::kRetryAfterGiveUpMs - 1, loadedOf);
        Check(events.gaveUp.empty() && events.asked.empty() && events.retried.empty() && bank::Active(states).empty(),
            "follow: a given-up hold is left alone for the retry wait");
        const std::uint64_t retryAt = 3000 + bank::kGiveUpMs + bank::kRetryAfterGiveUpMs;
        events = bank::Follow(states, plan, retryAt, loadedOf);
        Check(events.retried.size() == 1 && events.asked.empty() && bank::Active(states).size() == 1,
            "follow: (review) a given-up hold still planned is asked again after the retry wait");
        loaded = v3;
        events = bank::Follow(states, plan, retryAt + 300, loadedOf);
        Check(events.loaded.size() == 1 && events.loaded[0].loadedMs - events.loaded[0].askedMs == 300,
            "follow: the retry's wait is measured from the retry");
        loaded = v2;
        states.clear();
        bank::Follow(states, plan, 50000, loadedOf);
        bank::Follow(states, plan, 50000 + bank::kGiveUpMs, loadedOf);
        bank::Follow(states, plan, 50000 + bank::kGiveUpMs + bank::kRetryAfterGiveUpMs, loadedOf);
        events = bank::Follow(states, plan, 50000 + bank::kGiveUpMs + bank::kRetryAfterGiveUpMs + bank::kGiveUpMs, loadedOf);
        Check(events.gaveUp.size() == 1 && events.retried.empty(),
            "follow: a retry that does not load gives up again after the same wait");

        // The pinned request.
        Check(bank::JudgePinned(true, v3, v3) == bank::PinnedBank::Ready, "pinned: the bank is loaded");
        Check(bank::JudgePinned(true, v3, v2) == bank::PinnedBank::Missing, "pinned: another bank: by context");
        Check(bank::JudgePinned(false, v3, 0) == bank::PinnedBank::Unknown, "pinned: unknown voice: as before");

        // The engine sites (offline 1491.50).
        using namespace replay::theatre;
        Check(SiteTarget(kAnimalBankPickSite) == 0x2F985B0 && SiteTarget(kAnimalBankPickSite) == kAnimalBankPickSite.target,
            "sites: the animal bank update calls the type picker 0x2F985B0");
        Check(SiteTarget(kAnimalNameTableSite) == 0x3E276A0, "sites: the name builder reads the type name table 0x3E276A0");
        Check(SiteTarget(kAnimalBankFormatSite) == 0x37FEC88, "sites: and formats with .rdata 0x37FEC88");
        Check(bank::RecordOffset(bank::kHorseType, bank::kHolder) == 0x1930 &&
                bank::RecordOffset(bank::kHorseType, bank::kHolderObject) == 0x1940,
            "records: the horse record's holder is the +0x1930/+0x1940 the picker tests (0x2F985D6, 0x2F98626)");
    }

    // A mission's SET_WEATHER_TYPE(GROUNDBLIZZARD, ..., 40f): Light snow with a
    // 40 s transition to Ground blizzard, the engine's dry switch at the end.
    void TestWeatherTransitionTrack()
    {
        using namespace replay;
        const std::uint32_t snowLight = Joaat("SNOWLIGHT");
        const std::uint32_t ground = Joaat("GROUNDBLIZZARD");
        std::vector<EnvironmentSample> track;
        EnvironmentSample s;
        s.weatherFrom = s.weatherTo = snowLight;
        s.clockS = 50000;
        s.transitionKnown = true;
        s.timeMs = 0;
        Check(envtrack::Add(track, s), "transition: first sample");
        s.timeMs = 1000;
        s.transitionTo = ground;
        s.transitionTotalMs = 40000.0f;
        s.transitionLeftMs = 40000.0f;
        Check(envtrack::Add(track, s), "transition: a transition starting is a change (the base pair did not move)");
        s.timeMs = 1250;
        s.transitionLeftMs = 39750.0f;
        Check(!envtrack::Add(track, s), "transition: the time left running down at the engine's pace is not a change");
        s.timeMs = 11000;
        s.transitionLeftMs = 30000.0f;
        Check(!envtrack::Add(track, s), "transition: ten seconds on the straight line, still no sample");
        s.timeMs = 12000;
        s.transitionLeftMs = 29900.0f; // paused or slowed: 100 ms of game time in 1 s
        Check(envtrack::Add(track, s), "transition: a time left off the straight line is kept");
        s.timeMs = 12250;
        s.transitionLeftMs = 29650.0f;
        Check(!envtrack::Add(track, s), "transition: back on the line from the kept sample");
        // The engine ends it at 0 (0x75FFEC -> 0x733F34): the target is the weather, the fields cleared.
        EnvironmentSample done = s;
        done.timeMs = 42000; // 29900 ms after the stray sample
        done.weatherFrom = done.weatherTo = ground;
        done.transitionTo = 0;
        done.transitionTotalMs = done.transitionLeftMs = 0.0f;
        Check(envtrack::Add(track, done), "transition: the end (new pair, no target) is kept");
        Check(track.size() == 4, "transition: 40 s cost four samples");

        EnvironmentSample at;
        Check(envtrack::At(track, 500, at) && at.transitionTo == 0, "transition: none before it starts");
        Check(envtrack::At(track, 6000, at) && at.transitionTo == ground && std::fabs(at.transitionLeftMs - 35000.0f) < 1.0f,
            "transition: between two samples of the same transition the time left runs down from the first");
        Check(envtrack::At(track, 11800, at) && std::fabs(at.transitionLeftMs - 29900.0f) < 1.0f,
            "transition: and never below the next one (the stray)");
        Check(envtrack::At(track, 30000, at) && at.weatherTo == snowLight && at.transitionTo == ground &&
                std::fabs(at.transitionLeftMs - 11900.0f) < 1.0f,
            "transition: after the last sample of it, the time left runs at 1 ms a ms (the pair is still the old one)");
        Check(envtrack::At(track, 41950, at) && at.transitionLeftMs == 0.0f, "transition: never below 0");
        Check(envtrack::At(track, 42000, at) && at.weatherTo == ground && at.transitionTo == 0, "transition: ended in the new pair");

        // A cut inside the transition keeps it at its base.
        const std::vector<EnvironmentSample> cut = envtrack::Cut(track, 20000, 45000);
        Check(!cut.empty() && cut[0].timeMs == 0 && cut[0].transitionTo == ground && std::fabs(cut[0].transitionLeftMs - 21900.0f) < 1.0f,
            "transition: a cut starts inside it with the time left at the base");
        Check(cut.size() == 2 && cut[1].timeMs == 22000 && cut[1].transitionTo == 0, "transition: the cut keeps the end");

        // A sample from before ENVT v2 (unknown) differs from a known one, never interpolated.
        EnvironmentSample unknown;
        Check(envtrack::TransitionChanged(unknown, track[0]), "transition: known against unknown is a change");
        Check(!envtrack::Valid([] {
            EnvironmentSample bad;
            bad.transitionTotalMs = -1.0f;
            return bad;
        }()), "transition: a negative total is refused");
    }

    // What the replay writes in the weather manager's fields.
    void TestWeatherTransitionWrite()
    {
        using namespace replay::envtrack;
        TransitionWrite w = TransitionFields(40000.0f, 30000.0f);
        Check(std::fabs(w.progress - 0.25f) < 1e-5f && w.totalMs == 40000.0f && std::fabs(w.leftMs - 30000.0f) < 0.5f,
            "write: inside the transition the recorded total and time left");
        w = TransitionFields(40000.0f, 200.0f);
        Check(w.leftMs >= kMinLeftMs && std::fabs(w.progress - 0.995f) < 1e-5f &&
                std::fabs(TransitionProgress(w.totalMs, w.leftMs) - w.progress) < 1e-5f,
            "write: near the end the time left stays above the floor and the progress is the recorded one");
        w = TransitionFields(40000.0f, 0.0f);
        Check(w.leftMs >= kMinLeftMs && w.progress == kMaxProgress && std::isfinite(w.totalMs),
            "write: at the end the progress caps below 1 (the engine never ends it on its own)");
        w = TransitionFields(40000.0f, -50.0f);
        Check(w.leftMs >= kMinLeftMs && w.progress == kMaxProgress, "write: a negative time left (an engine frame past 0)");
        w = TransitionFields(0.0f, 0.0f);
        Check(w.leftMs >= kMinLeftMs && std::isfinite(w.totalMs) && w.totalMs > 0.0f, "write: a zero total");
        w = TransitionFields(40000.0f, 50000.0f);
        Check(w.progress == 0.0f && w.leftMs <= w.totalMs, "write: more left than total is the start");
        // One engine frame (33 ms at 30 fps) never reaches 0 from what is written.
        for (float left : {40000.0f, 5000.0f, 900.0f, 10.0f, 0.0f})
        {
            w = TransitionFields(40000.0f, left);
            Check(w.leftMs - 33.0f > 0.0f, "write: one engine frame stays above 0");
        }
    }

    // SET_WIND_DIRECTION takes degrees and stores (-sin d, -cos d) (0x73A01C,
    // 0x10594BC); the track keeps atan2(y, x) of GET_WIND_DIRECTION's vector.
    void TestWindDirectionDegrees()
    {
        using namespace replay::envtrack;
        const float toRad = kPi / 180.0f;
        bool ok = true;
        for (float heading = -3.1f; heading < 3.15f; heading += 0.37f)
        {
            const float d = WindDegreesFromHeading(heading);
            const float x = -std::sin(d * toRad);
            const float y = -std::cos(d * toRad);
            ok = ok && d >= 0.0f && d < 360.0f && std::fabs(x - std::cos(heading)) < 1e-4f && std::fabs(y - std::sin(heading)) < 1e-4f;
        }
        Check(ok, "wind: the degrees store the recorded vector back, never negative (a negative value releases)");
        // The vector the setter stored for 231.2586 (a script's value) gives the degrees back.
        const float d = 231.2586f;
        Check(std::fabs(WindDegreesFromVector(-std::sin(d * toRad), -std::cos(d * toRad)) - d) < 1e-3f,
            "wind: a stored override goes back as its degrees");
        Check(std::fabs(WindDegreesFromVector(0.0f, -1.0f) - 0.0f) < 1e-4f, "wind: 0 degrees is the vector (0, -1)");
    }
    // no_record_fallback.h: Arthur's bandana.
    replay::entity::MetaPedAsset Asset(std::uint32_t drawable, std::uint32_t albedo)
    {
        replay::entity::MetaPedAsset a;
        a.drawable = drawable;
        a.albedo = albedo;
        a.normal = drawable ^ 1u;
        a.material = drawable ^ 2u;
        return a;
    }

    void TestNoRecordLayerDiff()
    {
        using namespace replay::norecord;
        // Recorded: base, previous neckwear 0xc3c45008, bandana up 0x4a9da893 (same
        // category), shirt 0x92d15ab2.
        const std::vector<replay::entity::MetaPedAsset> recorded = {Asset(0xefacf496, 1), Asset(0xc3c45008, 2),
            Asset(0x4a9da893, 3), Asset(0x92d15ab2, 4)};
        CategoryCache cache;
        cache.Learn(0xc3c45008, Joaat("NECKWEAR")); // learned while the previous list was on the puppet
        std::vector<PuppetLayer> puppet;
        puppet.push_back({Asset(0xefacf496, 1), Joaat("HEADS")});
        puppet.push_back({Asset(0x4a9da893, 3), Joaat("NECKWEAR")}); // replaced the previous neckwear
        replay::entity::MetaPedAsset shirt = Asset(0x92d15ab2, 4);
        shirt.tint0 = 7; // the shirt with another tint
        puppet.push_back({shirt, Joaat("SHIRTS_FULL")});
        for (const PuppetLayer& layer : puppet)
            cache.Learn(layer.asset.drawable, layer.category);
        const LayerDiff diff = DiffLayers(recorded, puppet, cache);
        Check(diff.recorded == 4 && diff.present == 2 && diff.puppet == 3, "layer diff counts the recorded layers present");
        Check(diff.missing.size() == 2 && diff.missing[0].index == 1 && diff.missing[0].why == LayerWhy::CategoryTaken &&
                diff.missing[0].takenBy == 0x4a9da893 && diff.missing[0].category == Joaat("NECKWEAR"),
            "a recorded layer whose category another asset holds is category_taken, with the holder");
        Check(diff.missing[1].index == 3 && diff.missing[1].why == LayerWhy::Texture,
            "the same drawable with another tint is texture");
        Check(diff.extra.size() == 1 && diff.extra[0].asset.drawable == 0x92d15ab2, "the puppet's unrecorded layer is extra");
        Check(diff.sharedCategory == 2, "two recorded layers in one category are counted (not applicable tag by tag)");
        const std::string missing = FormatMissing(diff, 8);
        Check(missing.find("1:0xc3c45008:0x00000002:category_taken:") == 0 && missing.find(":0x4a9da893") != std::string::npos &&
                missing.find(";3:0x92d15ab2:0x00000004:texture:") != std::string::npos,
            "the missing list names index, drawable, albedo, why and category");
        Check(FormatMissing(diff, 1).find(";+1") != std::string::npos && FormatExtra(LayerDiff{}, 8) == "none",
            "the lists are capped and say none when empty");
        CategoryCache empty;
        const LayerDiff unknown = DiffLayers({Asset(0x0bcc365a, 9)}, {}, empty);
        Check(unknown.missing.size() == 1 && unknown.missing[0].why == LayerWhy::Absent && unknown.missing[0].category == 0,
            "an unknown category not on the puppet is absent");
    }

    void TestNoRecordAlternates()
    {
        using namespace replay;
        using namespace replay::norecord;
        const entity::ReplayId arthur{0, 1};
        std::vector<entity::PoseSegment> segments(2);
        segments[0].entity = arthur;
        segments[0].startMs = 0;
        segments[0].recordId = 0x80000008;
        segments[0].appearance.assets = {Asset(1, 1), Asset(2, 2)};
        segments[1].entity = arthur;
        segments[1].startMs = 2324;
        segments[1].recordId = 0x8000000d;
        segments[1].appearance.assets = {Asset(1, 1), Asset(2, 2), Asset(3, 3)}; // the 27 layers
        AppearanceTrack track;
        track.id = arthur.Packed();
        track.changes.resize(3);
        track.changes[0].timeMs = 0;
        track.changes[0].appearance.assets = {Asset(1, 1), Asset(5, 5)};
        track.changes[1].timeMs = 9768;
        track.changes[1].appearance.assets = {Asset(1, 1), Asset(3, 3), Asset(6, 6)};
        track.changes[2].timeMs = 11275;
        track.changes[2].appearance.assets = {Asset(1, 1), Asset(3, 3), Asset(7, 7)}; // builds the 1066 bones
        const entity::Appearance create;
        const std::uint64_t worn = AppearancePrint(segments[1].appearance);
        const std::vector<Alternate> found = Alternates(&track, segments, arthur, &segments[1], create, 2339, worn, kMaxAlternates);
        Check(found.size() == 2 && found[0].key == 2 && found[1].key == 3,
            "the alternates are the segment's other appearances, nearest first, without the worn one or the earlier segment's");
        Check(AppearanceForKey(&track, &segments[1], create, 3) == &track.changes[2].appearance &&
                AppearanceForKey(&track, &segments[1], create, 0) == &segments[1].appearance &&
                AppearanceForKey(&track, &segments[1], create, 9) == nullptr,
            "an alternate key gives back its appearance");
        const std::vector<Alternate> late = Alternates(&track, segments, arthur, &segments[1], create, 11000,
            AppearancePrint(track.changes[1].appearance), kMaxAlternates);
        Check(late.size() == 2 && late[0].key == 3 && late[1].key == 0, "nearest to the playhead first, the segment's own included");
        const std::vector<Alternate> first = Alternates(&track, segments, arthur, &segments[0], create, 100,
            AppearancePrint(segments[0].appearance), kMaxAlternates);
        Check(first.size() == 1 && first[0].key == 1, "the first segment's span ends at the next segment");
    }

    void TestNoRecordSteps()
    {
        using namespace replay::norecord;
        Check(MapNow(true, true, false) && !MapNow(false, true, false) && !MapNow(true, false, false) && !MapNow(true, true, true),
            "only a posed body that lost its arm to a change goes through the map at once");
        std::vector<Alternate> two(2);
        two[0].key = 3;
        two[1].key = 2;
        Fallback f;
        Check(f.Begin(0x8000000d, 1066, two, 1000) && f.alternates == 2 && f.keys[0] == 3, "the steps begin with the alternates' keys");
        Check(f.Next(985, 24, 1100, true) == Step::Wait, "nothing before the step period");
        Check(f.Next(985, 24, 1400, true) == Step::Retry && f.retries == 1, "the recorded layers once more");
        Check(f.Next(985, 24, 1800, true) == Step::Alternate && f.sameAnswer && f.AlternateKey() == 3,
            "the same answer stops the retries: the nearest alternate");
        Check(!f.Begin(0x8000000d, 1066, two, 1900), "a re-map for the same record goes on");
        Check(f.Next(990, 20, 2200, true) == Step::Alternate && f.AlternateKey() == 2, "then the next alternate");
        Check(f.Next(990, 20, 2600, true) == Step::Restore && !f.active && f.done, "none fit: the recorded layers back");
        Check(!f.Begin(0x8000000d, 1066, two, 3000) && f.Next(985, 24, 9000, true) == Step::Wait,
            "done: the restore's re-map does not start again");
        f.Reset();
        Check(f.Begin(0x8000000d, 1066, two, 3000), "a new segment or change starts again");
        Check(f.Next(985, 24, 3400, false) == Step::Retry && f.Next(985, 24, 3800, false) == Step::Done && f.done,
            "alternates off: the mapped pose stays after the same answer");
        Fallback g;
        g.Begin(1, 1066, {}, 0);
        std::uint32_t retries = 0;
        for (std::uint64_t t = 400, bones = 900; t < 10000; t += 400, ++bones)
        {
            const Step step = g.Next(static_cast<std::uint32_t>(bones), 10, t, true);
            if (step == Step::Retry)
                ++retries;
            else if (step != Step::Wait)
                break;
        }
        Check(retries == kMaxRetries && g.done, "answers that keep changing are retried up to the old six");
        Fallback h;
        h.Begin(1, 1066, two, 0);
        h.Next(985, 24, 400, true);
        h.Finish();
        Check(!h.active && h.done && h.Next(985, 24, 5000, true) == Step::Wait, "an exact arm ends the steps");
    }


    // A loose hat's layer read from its own tags, and how playback builds it
    // (hat_look.h).
    void TestHatLook()
    {
        using namespace replay::hatlook;
        // A tag as the engine lays it out: the packed palette qword (palette
        // high dword, tint0 byte 2, tint1 byte 1, tint2 byte 0, 0x2B3FE34),
        // then +0xC drawable, +0x10 albedo, +0x14 normal, +0x18 material.
        std::uint8_t bytes[kTagBytes]{};
        const std::uint64_t packed = (0x9A2F1E44ull << 32) | (0x12u << 16) | (0x34u << 8) | 0x56u;
        std::memcpy(bytes, &packed, sizeof(packed));
        const std::uint32_t words[4] = {0xC1AE4E12u, 0x11111111u, 0x22222222u, 0x33333333u};
        std::memcpy(bytes + 0xC, words, sizeof(words));
        bytes[8] = 0xEE; // +0x8 is not part of the look
        const replay::entity::MetaPedAsset asset = DecodeTag(bytes);
        Check(asset.drawable == 0xC1AE4E12u && asset.albedo == 0x11111111u && asset.normal == 0x22222222u &&
                  asset.material == 0x33333333u,
            "hat look: drawable +0xC, albedo +0x10, normal +0x14, material +0x18");
        Check(asset.palette == 0x9A2F1E44u && asset.tint0 == 0x12 && asset.tint1 == 0x34 && asset.tint2 == 0x56,
            "hat look: the packed qword unpacks as 0xB5E4E4 (palette high, tint0 byte 2, tint1 byte 1, tint2 byte 0)");
        Check(UsableTag(asset) && !UsableTag(replay::entity::MetaPedAsset{}), "hat look: a tag without a drawable is dropped");
        Check(TagsToRead(0, true) == 0 && TagsToRead(1, true) == 1 && TagsToRead(9, true) == kMaxTags &&
                  TagsToRead(3, false) == 1,
            "hat look: the count capped, one tag when the stride is unproven");
        Check(sizeof(kTagCoreSignature) == 63 && kTagCoreSignature[22] == 0x80 && kTagCoreSignature[25] == kEntityTypeObject &&
                  kTagCoreSignature[31] == (kTagCountOffset & 0xFF) && kTagCoreSignature[40] == (kTagArrayOffset & 0xFF) &&
                  kTagWalkSignature[6] == kTagStride,
            "hat look: the signatures carry the offsets the reader uses");

        const std::vector<std::uint32_t> none;
        const std::vector<std::uint32_t> learned = {0x12345678u};
        Check(IsHatModel(0x30762E1Au, none) && IsHatModel(0xE6856EC4u, none) && IsHatModel(0x0B8A6250u, none) &&
                  !IsHatModel(0x12345678u, none) && IsHatModel(0x12345678u, learned),
            "hat look: the known hat models and the clip's own");
        Check(ChooseRebuild(true, true, true) == Rebuild::Dress, "hat look: a hat model with a recorded look is dressed");
        Check(ChooseRebuild(true, true, false) == Rebuild::Model,
            "hat look: another object with tags keeps its model (a knock drops only a hat)");
        Check(ChooseRebuild(true, false, true) == Rebuild::Hide,
            "hat look: a hat model without a look is hidden (the bare model is the grey octagon)");
        Check(ChooseRebuild(true, false, false) == Rebuild::Model, "hat look: any other object keeps its model");
        Check(ChooseRebuild(false, true, true) == Rebuild::Model && ChooseRebuild(false, false, true) == Rebuild::Model,
            "hat look: HatLook=0 builds the bare model, as 0.187");

        // The knock after a wear (40 of 40 same-tick knocks dropped nothing).
        Check(WhenToKnock(0, 5000, false) == KnockStep::Knock, "knock: an owner that already wore the hat knocks at once");
        Check(WhenToKnock(1000, 1000, true) == KnockStep::Wait && WhenToKnock(1000, 1000 + kWearSettleMs - 1, true) == KnockStep::Wait,
            "knock: never in the tick of the wear");
        Check(WhenToKnock(1000, 1000 + kWearSettleMs, true) == KnockStep::Knock, "knock: once the layer is on");
        Check(WhenToKnock(1000, 1000 + kWearWaitMs - 1, false) == KnockStep::Wait &&
                  WhenToKnock(1000, 1000 + kWearWaitMs, false) == KnockStep::KnockLate,
            "knock: the wait for the layer is bounded");
        Check(WhenToKnock(5000, 4000, true) == KnockStep::Wait, "knock: a clock behind the wear waits");

        const std::vector<replay::entity::MetaPedAsset> recorded = {asset};
        replay::entity::MetaPedAsset other = asset;
        other.drawable = 0xE1F678E9u;
        Check(VerifyDropped(recorded, recorded, true) == Verify::Same, "dress: the dropped hat shows the recorded drawable");
        Check(VerifyDropped(recorded, {other}, true) == Verify::Other, "dress: the helper's own hat is refused");
        Check(VerifyDropped(recorded, {}, false) == Verify::Unread && VerifyDropped({}, recorded, true) == Verify::Unread,
            "dress: an unread object is not refused");
        Check(kJobTimeoutMs > kHelperWaitMs + kAssetWaitMs + kWearWaitMs + kDropWaitMs,
            "dress: a job outlives every step's own limit (the helper's model wait included)");
    }

    // speech_request.h.
    void TestSpeechRequest()
    {
        using namespace replay::speech::request;
        // The line's copies sit where the line init 0x2FABBAC writes them.
        std::uint8_t line[0x2C0] = {};
        auto put = [&](std::size_t offset, std::uint32_t value) { std::memcpy(line + offset, &value, 4); };
        put(0x214, 2);      // +0xD8
        put(0x21C, 1);      // +0xDC volume type
        put(0x220, 3);      // +0xE0 audibility
        put(0x224, 2);      // +0xE8
        put(0x218, 3);      // +0xF8 source
        put(0x27C, 30000);  // +0x168 preload
        put(0x2BC, 0x80u);  // flag bytes
        Fields recorded;
        Check(FromLineBlock(line + kLineBlockFrom, sizeof(line) - kLineBlockFrom, recorded) && recorded.read,
            "request: the block from line +0x1F8 to the line's end is read");
        Check(recorded.value[VolumeType] == 1 && recorded.value[Audibility] == 3 && recorded.value[D8] == 2 &&
                  recorded.value[E8Tables] == 2 && recorded.value[Source] == 3 && recorded.value[PreloadMs] == 30000 &&
                  recorded.value[LineBits] == 0x80u,
            "request: each field from its line offset");
        Fields shortRead;
        Check(!FromLineBlock(line + kLineBlockFrom, kLineBlockBytes - 1, shortRead) && !shortRead.read,
            "request: a short block is not read");

        // The ask: type 1 and 2 as recorded, 3 and 0 never; the four fields in range only.
        Ask ask = BuildAsk(kTypeSingle, recorded, true, true);
        Check(ask.lineType == 1 && ask.count == 4, "request: the AI's type 1 is asked with its four request fields");
        bool volume = false, audibility = false, source = false;
        for (std::uint8_t n = 0; n < ask.count; ++n)
        {
            volume = volume || (ask.writes[n].offset == 0xDC && ask.writes[n].value == 1);
            audibility = audibility || (ask.writes[n].offset == 0xE0 && ask.writes[n].value == 3);
            source = source || ask.writes[n].offset == 0xF8;
        }
        Check(volume && audibility && !source, "request: +0xDC and +0xE0 written, the source left to the slot manager");
        Check(BuildAsk(kTypeLayered, recorded, true, false).lineType == 2 && BuildAsk(kTypeLayered, recorded, true, false).count == 0,
            "request: type 2 asked, fields off");
        Check(BuildAsk(kTypePreResolved, recorded, true, false).lineType == 0 && BuildAsk(0, recorded, true, false).lineType == 0,
            "request: type 3 (pre-resolved object) and unknown are not asked");
        Check(BuildAsk(kTypeSingle, recorded, false, false).Empty(), "request: both switches off ask nothing (the 0.187 request)");
        Fields outOfRange = recorded;
        outOfRange.value[VolumeType] = 4;   // the volume vector refuses >= 4 (0x2F9B800)
        outOfRange.value[Audibility] = 7;
        outOfRange.value[D8] = 5;
        outOfRange.value[E8Tables] = 9;
        Check(BuildAsk(kTypeSingle, outOfRange, true, true).count == 0, "request: a value the engine would refuse is not written");
        Check(BuildAsk(kTypeSingle, Fields{}, true, true).count == 0, "request: an old clip (fields unread) writes no field");

        // The diff and the tally.
        Fields replayed = recorded;
        replayed.value[VolumeType] = 0;
        replayed.value[E8Tables] = 4;
        Check(DiffMask(recorded, replayed) == ((1u << VolumeType) | (1u << E8Tables)), "request: the mask names the two fields");
        const std::string text = DiffText(1, recorded, 2, replayed);
        Check(text == "type:1>2|volume_type:0x1>0x0|e8_tables:0x2>0x4", "request: the diff text, recorded > replayed");
        Check(DiffText(1, recorded, 1, recorded) == "none", "request: equal lines diff to none");
        Check(DiffText(2, Fields{}, 2, recorded) == "unread" && DiffText(1, Fields{}, 2, recorded) == "type:1>2|unread",
            "request: an old clip diffs to unread (with the type still compared)");
        Tally tally;
        tally.Note(1, recorded, 2, replayed, BuildAsk(1, recorded, false, false));
        tally.Note(1, recorded, 1, recorded, BuildAsk(1, recorded, true, true));
        tally.Note(2, Fields{}, 2, recorded, BuildAsk(2, Fields{}, true, true));
        Check(tally.lines == 3 && tally.typeAsked == 2 && tally.typeKept == 2 && tally.typeOther == 1 &&
                  tally.fieldsAsked == 1 && tally.unread == 1 && tally.same == 1,
            "request: the tally counts asks, kept and other types, unread and equal lines");
        Check(tally.FieldText() == "volume_type:1|e8_tables:1", "request: the fields that differed, how often");
        Check(tally.TakeLog(1) && !tally.TakeLog(1) && tally.TakeLog(2) && tally.TakeLog(7) && !tally.TakeLog(3),
            "request: one diff line per recorded type (3 and above share one)");
    }

    // speech_resume.h.
    void TestResumeOldCopy()
    {
        using namespace replay::speech::resume;
        Check(ClassifyOldCopy(false, true, 6, true) == OldCopy::Unread, "old copy: a failed read");
        Check(ClassifyOldCopy(true, false, 6, true) == OldCopy::Gone, "old copy: freed (the pool wrote over the vtable)");
        Check(ClassifyOldCopy(true, true, 6, false) == OldCopy::Other, "old copy: another line in the element");
        Check(ClassifyOldCopy(true, true, 6, true) == OldCopy::Stopping, "old copy: live, state 6, same line");
        Check(ClassifyOldCopy(true, true, 3, true) == OldCopy::Active, "old copy: the stop did not take");
        Check(ClassifyOldCopy(true, true, 0, true) == OldCopy::Gone, "old copy: no state");

        Check(StepForOldCopy(true, OldCopy::Stopping, 0) == OldCopyStep::Wait, "old copy: a stopping copy holds the resume");
        Check(StepForOldCopy(true, OldCopy::Stopping, kOldCopyWaitMs - 1) == OldCopyStep::Wait, "old copy: within the window");
        Check(StepForOldCopy(true, OldCopy::Stopping, kOldCopyWaitMs) == OldCopyStep::AskTimedOut,
            "old copy: asked anyway at the end of the window");
        Check(StepForOldCopy(false, OldCopy::Stopping, 0) == OldCopyStep::Ask, "old copy: SpeechResumeWaitOld=0 asks at once");
        Check(StepForOldCopy(true, OldCopy::Gone, 0) == OldCopyStep::Ask && StepForOldCopy(true, OldCopy::Active, 0) == OldCopyStep::Ask &&
                  StepForOldCopy(true, OldCopy::Other, 0) == OldCopyStep::Ask && StepForOldCopy(true, OldCopy::Unread, 0) == OldCopyStep::Ask,
            "old copy: only a stopping copy waits");

        std::vector<StoppedCopy> copies;
        StoppedCopy a;
        a.speaker = 0x2C014Fu;
        a.line = 0x1000;
        a.voice = 0xB84B7711u;
        a.context = 0x55u;
        a.variation = 1;
        a.stopUs = 1000000;
        copies.push_back(a);
        StoppedCopy b = a;
        b.line = 0x2000;
        b.stopUs = 2000000; // the same line stopped again later
        copies.push_back(b);
        StoppedCopy c = a;
        c.variation = 2; // another phrase of the same speaker
        c.line = 0x3000;
        copies.push_back(c);
        const StoppedCopy* found = FindStoppedCopy(copies, a.speaker, a.voice, a.context, 1);
        Check(found && found->line == 0x2000, "old copy: the latest stop of the same line");
        Check(FindStoppedCopy(copies, a.speaker, a.voice, a.context, 3) == nullptr && FindStoppedCopy(copies, 7, a.voice, a.context, 1) == nullptr,
            "old copy: another variation or speaker is not the line");
        // The copy's own identity (read before the stop) wins over the
        // recorded one: a copy said with the puppet's own voice is still it.
        StoppedCopy own = a;
        Check(SameCopyIdentity(own, a.voice, a.context, 1) && !SameCopyIdentity(own, 0x1234u, a.context, 1),
            "old copy: without its own identity the recorded one is compared");
        own.ownRead = true;
        own.ownVoice = 0x1234u;
        own.ownContext = a.context;
        own.ownVariation = 1;
        Check(SameCopyIdentity(own, 0x1234u, a.context, 1) && !SameCopyIdentity(own, a.voice, a.context, 1),
            "old copy: its own voice (the own-voice retry) is compared when read");
        PruneStoppedCopies(copies, 2000000 + kOldCopyWaitMs * 1000ull);
        Check(copies.size() == 1 && copies[0].line == 0x2000, "old copy: the copies stopped before the window go");
        PruneStoppedCopies(copies, 2000000 + kOldCopyWaitMs * 1000ull + 1);
        Check(copies.empty(), "old copy: and the last one past it");
    }

    // The fires a clip starts with.
    void PutFire32(std::uint8_t* state, std::size_t offset, std::uint32_t value) { std::memcpy(state + offset, &value, 4); }
    void PutFireF(std::uint8_t* state, std::size_t offset, float value) { std::memcpy(state + offset, &value, 4); }
    void PutFire64(std::uint8_t* state, std::size_t offset, std::uint64_t value) { std::memcpy(state + offset, &value, 8); }
    float GetDescF(const std::uint8_t* desc, std::size_t offset)
    {
        float value = 0.0f;
        std::memcpy(&value, desc + offset, 4);
        return value;
    }
    std::uint32_t GetDesc32(const std::uint8_t* desc, std::size_t offset)
    {
        std::uint32_t value = 0;
        std::memcpy(&value, desc + offset, 4);
        return value;
    }

    void TestFireOwnership()
    {
        using namespace replay::fire;
        std::uint8_t state[0x1F0] = {};
        Check(!PtfxOwnedFire(state) && !StopInRangeImmune(state) && !PersistentFire(state), "a plain fire is the clip's");
        PutFire32(state, kFireFlags, 0x180);
        Check(PtfxOwnedFire(state), "flags 0x80 (Init with desc+0xB0): the effect's fire");
        PutFire32(state, kFireFlags, 0);
        PutFire64(state, kFireRegOwner, 0x7FF700001000ull);
        Check(PtfxOwnedFire(state), "an effect at +0x1A8: the effect's fire");
        PutFire64(state, kFireRegOwner, 0);
        PutFire32(state, kFireFlags, 0x800);
        Check(StopInRangeImmune(state), "+0xE8 bit 11: StopFireInRange skips it");
        PutFire32(state, kFireBurnTime, 0x7F7FFFFFu);
        Check(PersistentFire(state), "burn time FLT_MAX: burns until stopped");
        Check(ClearVerdictFor(state, false, false) == ClearVerdict::StopImmune, "immune before anything else of the world");
        Check(ClearVerdictFor(state, true, false) == ClearVerdict::Protected, "an adopted fire is protected");
        Check(ClearVerdictFor(state, true, true) == ClearVerdict::Ours, "ours first");
        PutFire32(state, kFireFlags, 0x80);
        Check(ClearVerdictFor(state, false, false) == ClearVerdict::PtfxOwned, "the effect's fire is left to its effect");
        PutFire32(state, kFireFlags, 0x1);
        Check(ClearVerdictFor(state, false, false) == ClearVerdict::Stop, "any other fire is stopped");
        Check(std::string(ClearVerdictName(ClearVerdict::Stop)) == "other", "verdict names");
    }

    void TestFireDescRebuild()
    {
        using namespace replay::fire;
        std::uint8_t state[0x1F0] = {};
        const float local[4] = {-755.0f, -456.0f, 44.0f, 0.0f};
        const float normal[4] = {0.0f, 0.0f, 1.0f, 0.0f};
        const float world[4] = {-755.0f, -456.0f, 44.0f, 0.0f};
        std::memcpy(state + kFireLocalPos, local, 16);
        std::memcpy(state + kFireNormal, normal, 16);
        std::memcpy(state + kFireGroupVec, world, 12);
        PutFire32(state, 0x5C, 7); // incident group
        PutFire32(state, kFireType, 12);
        PutFire32(state, kFireBone, 0xFFFFFFFFu);
        PutFire64(state, kFireMaterial, 0x1234);
        PutFireF(state, kFireBurnTime, 30.0f);
        PutFireF(state, kFireMaxStrength, 2.0f);
        PutFireF(state, kFirePeakTime, 3.0f);
        PutFire32(state, kFireVfxInfo, 0xABCD);
        PutFire32(state, kFireSeed, 0x1F);
        PutFire32(state, kFireFlags, (1u << 11) | (1u << 13) | (1u << 2) | 0x7);
        PutFire64(state, kFireEntity, 0x7FF712345678ull);
        std::uint8_t desc[0x120];
        std::memset(desc, 0xCC, sizeof(desc));
        DescFromFire(state, desc);
        Check(GetDescF(desc, 0x00) == -755.0f && GetDescF(desc, 0x08) == 44.0f, "desc +0x00 is the local position (+0x10)");
        Check(GetDescF(desc, 0x18) == 1.0f, "desc +0x10 is the normal (+0x20)");
        Check(GetDescF(desc, 0x20) == -755.0f && GetDesc32(desc, 0x2C) == 0xFFFFFFFFu,
            "desc +0x20 is the position, no incident group (the refcounted table is left alone)");
        Check(GetDesc32(desc, 0x54) == 12 && GetDesc32(desc, 0x50) == 0xFFFFFFFFu, "type and bone");
        Check(GetDesc32(desc, 0x60) == 0x1234 && GetDescF(desc, 0x68) == 30.0f && GetDescF(desc, 0x6C) == 2.0f &&
                GetDescF(desc, 0x70) == 3.0f,
            "material, burn time, strength and peak");
        Check(GetDesc32(desc, 0xA0) == 0xABCD && GetDesc32(desc, 0xE4) == 0x1F, "vfx info and seed");
        Check(GetDescF(desc, 0x78) == 1.0f && GetDescF(desc, 0x118) == -1.0f && GetDesc32(desc, 0x10C) == 0xFFFFFFFFu,
            "constructor defaults where Init takes nothing from the fire");
        Check(GetDesc32(desc, 0x40) == 0x12345678u, "the entity goes in +0x40 (the recorder maps it to an id, then zeroes it)");
        Check(GetDesc32(desc, 0xB0) == 0 && GetDesc32(desc, 0xB4) == 0 && GetDesc32(desc, 0xC0) == 0 && GetDesc32(desc, 0xF0) == 0,
            "no effect owner, culprit or parent");
        Check(desc[0x114] == (0x01 | 0x20) && desc[0x115] == 0x02, "flags back: bit 11 -> 0x01, bit 13 -> 0x20, bit 2 -> +0x115 0x02");
        PutFire32(state, kFireBurnTime, 0x7F7FFFFFu);
        DescFromFire(state, desc);
        Check((desc[0x114] & 0x40) != 0, "a persistent fire keeps desc+0x114 bit 0x40");
    }

    void TestFireMatch()
    {
        using namespace replay::fire;
        std::vector<FireSpot> wanted(2);
        wanted[0] = {{0.0f, 0.0f, 0.0f}, 12};
        wanted[1] = {{10.0f, 0.0f, 0.0f}, 11};
        std::vector<FireSpot> live(4);
        live[0] = {{0.3f, 0.0f, 0.0f}, 12};
        live[1] = {{0.1f, 0.0f, 0.0f}, 12};  // closer: wins
        live[2] = {{10.0f, 0.2f, 0.0f}, 12}; // right place, other type
        live[3] = {{0.0f, 0.0f, 0.6f}, 12};  // too far
        const std::vector<int> match = MatchFireSpots(wanted, live);
        Check(match.size() == 2 && match[0] == 1, "the closest live fire of the same type within 0.5 m");
        Check(match[1] == -1, "another type is another fire");
        Check(MatchFireSpots(wanted, {}) == std::vector<int>({-1, -1}), "no live fire: nothing matched (old behaviour)");
        std::vector<FireSpot> twins(2, FireSpot{{0.0f, 0.0f, 0.0f}, 12});
        std::vector<FireSpot> one(1, FireSpot{{0.05f, 0.0f, 0.0f}, 12});
        const std::vector<int> once = MatchFireSpots(twins, one);
        Check((once[0] == 0) != (once[1] == 0), "a live fire stands for one recorded fire only");
    }
}

namespace
{
    // Fires matched with the creates on their weapon's audio, the play state
    // samples, the outcomes and the text.
    void TestShotAudioDelay()
    {
        using namespace replay::shotdelay;
        Tracker tracker;
        Fire a;
        a.shooter = (31u << 16) | 1335u;
        a.audio = 0x1000100;
        a.fireUs = 1000000;
        a.eventMs = 74786;
        a.distanceM = 78.0f;
        tracker.AddFire(a);
        Fire b = a;
        b.fireUs = 1100000;
        b.eventMs = 74886;
        tracker.AddFire(b);
        Fire c = a;
        c.shooter = (26u << 16) | 1334u;
        c.audio = 0x2000100;
        c.fireUs = 1000000;
        c.distanceM = 10.0f;
        tracker.AddFire(c);
        std::vector<std::size_t> matched;
        // A create before the fire (beyond the clock slack), one on another
        // audio and one refused.
        std::vector<Create> rows = {{990000, 0x1000100, 0xA0, 1}, {1004000, 0x3000100, 0xB0, 2}, {1003000, 0x2000100, 0, 3}};
        tracker.OnCreates(rows, matched);
        Check(matched.empty(), "shot delay: a create before the fire or on another audio is no match; a refused one is not followed");
        Check(tracker.At(2).outcome == Outcome::Refused, "shot delay: the creator's refusal closes the fire as refused");
        // The two fires of one weapon take its creates in order.
        rows = {{1016000, 0x1000100, 0xA1, 0x10, true}, {1117000, 0x1000100, 0xA2, 0x11, false}};
        const std::uint32_t frames[3] = {0x251688D, 0x2EFAEAF, 0x2F0B800};
        Check(InFirePlay(frames, 3) && !InFirePlay(frames, 1), "shot delay: the fire path is a frame in the gunshot's play function");
        tracker.OnCreates(rows, matched);
        Check(matched.size() == 2 && tracker.At(0).sound == 0xA1 && tracker.At(1).sound == 0xA2,
            "shot delay: the oldest open fire of the audio takes the first create");
        Check(std::fabs(tracker.At(0).CreateMs() - 16.0) < 1e-6 && std::fabs(tracker.At(1).CreateMs() - 17.0) < 1e-6,
            "shot delay: the create delay from the fire");
        tracker.At(0).bank = Bank::Loaded;
        tracker.At(1).bank = BankOfName("loading");
        tracker.Sample(0, true, 1, 1033000);
        tracker.Sample(0, true, 2, 1050000);
        tracker.Sample(1, true, 3, 1150000);
        Check(tracker.At(0).outcome == Outcome::Started && std::fabs(tracker.At(0).StartMs() - 50.0) < 1e-6 &&
                std::fabs(tracker.At(0).StartLowerMs() - 33.0) < 1e-6,
            "shot delay: the first playing sample bounds the start (the last preparing one from below)");
        Check(tracker.At(1).outcome == Outcome::EndedUnseen, "shot delay: already ended at the first sample");
        tracker.Expire(1200000);
        Check(tracker.Size() == 0, "shot delay: closed shots leave the open list");
        const std::vector<Shot> finished = tracker.TakeFinished();
        Check(finished.size() == 3, "shot delay: the finished list for the per-shot lines");
        const auto& tallies = tracker.Tallies();
        const Tally& t31 = tallies.at(a.shooter);
        Check(t31.otherPath == 1, "shot delay: a create without the fire path is counted apart");
        Check(t31.shots == 2 && t31.created == 2 && t31.started == 1 && t31.endedUnseen == 1 && t31.banks[2] == 1 && t31.banks[3] == 1,
            "shot delay: the shooter's tally");
        Check(std::fabs(t31.createMaxMs - 17.0) < 1e-6 && std::fabs(t31.startMaxMs - 50.0) < 1e-6,
            "shot delay: the maxima");
        Check(tallies.at(c.shooter).refused == 1 && tallies.at(c.shooter).created == 0, "shot delay: the refused shooter");
        // Unmatched and never started by time, lost when the sound goes.
        Fire d = a;
        d.fireUs = 5000000;
        tracker.AddFire(d);
        Fire e = a;
        e.audio = 0x4000100;
        e.fireUs = 5000000;
        tracker.AddFire(e);
        Fire f = a;
        f.audio = 0x5000100;
        f.fireUs = 5000000;
        tracker.AddFire(f);
        rows = {{5010000, 0x4000100, 0xC0, 0x20}, {5010000, 0x5000100, 0xC1, 0x21}};
        tracker.OnCreates(rows, matched);
        tracker.Sample(matched[1], false, -1, 5020000);
        tracker.Expire(5000000 + kCreateWindowUs + 1);
        Check(tracker.Size() == 1, "shot delay: the uncreated fire closes after the create window, the lost one at once");
        tracker.Expire(5000000 + kStartWindowUs + 1);
        const Tally& t = tracker.Tallies().at(a.shooter);
        Check(t.unmatched == 1 && t.neverStarted == 1 && t.lost == 1, "shot delay: unmatched, never started and lost");
        const std::string text = SummaryLines("play", tracker.Tallies(), nullptr, [](std::uint32_t id) { return std::to_string(id >> 16); });
        Check(text.find("POSE_V2_SHOT_AUDIO_DELAY,phase=play,shooter=31,shots=5,created=4,refused=0,unmatched=1,started=1,ended_unseen=1,lost=1,never_started=1,other_path=3,") != std::string::npos,
            "shot delay: the per-shooter line");
        Check(text.find("dist_avg_m=78.0,dist_max_m=78.0,travel_avg_ms=227.4") != std::string::npos, "shot delay: the distance and the travel reference");
        Check(text.find("POSE_V2_SHOT_AUDIO_DELAY_TOTAL,phase=play,shooters=2,shots=6,") != std::string::npos, "shot delay: the total line");
        // The open list is bounded (oldest closes as unmatched).
        Tracker many;
        for (std::size_t n = 0; n < kMaxOpen + 3; ++n)
            many.AddFire(a);
        Check(many.Size() == kMaxOpen && many.Tallies().at(a.shooter).unmatched == 3, "shot delay: the open list is bounded");
        // The clip reference: the first fire path packet of the shot's weapon near the
        // shot (other callers, like 0x3125951 100 to 130 ms after the shot, are not
        // the gunshot's create).
        std::vector<RefShot> shots = {{a.shooter, 0x290053Cu, 74786}, {a.shooter, 0x290053Cu, 75870}, {c.shooter, 0x260053Bu, 76000},
            {c.shooter, 0x260053Bu, 77000}};
        std::vector<RefPacket> packets = {{0x290053Cu, 74700, true, false}, {0x290053Cu, 74784, true, true}, {0x290053Cu, 74917, true, true},
            {0x290053Cu, 75900, true, true}, {0x111u, 76010, true, true}, {0x260053Bu, 76700, true, true},
            {0x260053Bu, 77005, true, false}};
        const std::uint32_t echoCaller = 0x3125951u;
        Check(InFirePlay(&kFirePlayBegin, 1) && !InFirePlay(&kFirePlayEnd, 1) && !InFirePlay(&echoCaller, 1),
            "shot delay: the fire path bounds (the echo caller is outside)");
        const auto refs = ClipReference(shots, packets);
        Check(refs.at(a.shooter).count == 2 && std::fabs(refs.at(a.shooter).minMs + 2.0) < 1e-6 && std::fabs(refs.at(a.shooter).maxMs - 30.0) < 1e-6,
            "shot delay: the clip reference skips packets off the fire path and takes the first one (signed)");
        Check(refs.find(c.shooter) == refs.end(), "shot delay: no packet of that weapon in the window, no reference");
        const std::string withRef = SummaryLines("play", tracker.Tallies(), &refs, [](std::uint32_t id) { return std::to_string(id >> 16); });
        Check(withRef.find("clip_ref_n=2,clip_ref_avg_ms=14.0,clip_ref_min_ms=-2.0,clip_ref_max_ms=30.0") != std::string::npos,
            "shot delay: the reference beside the replay's numbers");
    }

    // A campfire's sizzle is a script sound on the meat, a weapon object; the shot
    // track does not remake it.
    void TestWeaponOffShot()
    {
        namespace owner = replay::soundowner;
        using replay::census::AudioBase;
        owner::Cover cover;
        owner::ShotWindows shots;
        shots.Add((41u << 16) | 1340u, 74786);
        shots.Seal();
        owner::PacketKey key;
        key.owner = (48u << 16) | 172u; // the meat (052-ScarlettMeadows)
        key.cls = replay::census::OwnerClass::Weapon;
        key.stableId = 0x10e55f6;
        key.caller = 0x251688D;
        key.callerInWrappers = replay::soundgeneric::InAudioEntityWrappers(key.caller);
        key.timeMs = 57286;
        key.audioKnown = true;
        key.audioBase = AudioBase::WeaponAudio;
        Check(key.callerInWrappers, "off shot: the sizzle's caller is inside the creator's wrappers");
        Check(owner::FamilyOf(key, cover) == owner::Family::Gunshot, "off shot: without the shot list the old rule (gunshot)");
        Check(owner::FamilyOf(key, cover, &shots) == owner::Family::Owned && owner::OffShot(key, &shots),
            "off shot: a script sound on a weapon object with no shot of it is owned (the generic player's)");
        Check(owner::TakerOf(owner::Family::Owned, owner::Overrides()) == owner::Taker::Generic, "off shot: owned goes to the generic player");
        // The shooter's weapon: the fire sound, the echo and a sound inside the window stay the shot's.
        key.owner = (41u << 16) | 1340u;
        key.timeMs = 74784 + owner::kShotAfterMs;
        Check(owner::FamilyOf(key, cover, &shots) == owner::Family::Gunshot, "off shot: inside the window after the shot");
        key.timeMs = 74786 - owner::kShotBeforeMs;
        Check(owner::FamilyOf(key, cover, &shots) == owner::Family::Gunshot, "off shot: inside the window before the shot");
        key.timeMs = 74786 + owner::kShotAfterMs + 1;
        Check(owner::FamilyOf(key, cover, &shots) == owner::Family::Owned, "off shot: after the window");
        key.timeMs = 74786 - owner::kShotBeforeMs - 1;
        Check(owner::FamilyOf(key, cover, &shots) == owner::Family::Owned, "off shot: before the window");
        // A named weapon caller (the fire path, the weapon foley) stays the shot's anywhere.
        key.caller = 0x2EFAEAF;
        key.callerInWrappers = replay::soundgeneric::InAudioEntityWrappers(key.caller);
        Check(!key.callerInWrappers && owner::FamilyOf(key, cover, &shots) == owner::Family::Gunshot,
            "off shot: a named weapon caller stays the shot's");
        key.caller = 0x319092B;
        key.callerInWrappers = false;
        key.timeMs = 1000;
        Check(owner::FamilyOf(key, cover, &shots) == owner::Family::Gunshot, "off shot: the weapon foley caller is untouched");
        // Not weapon audio: unaffected.
        key.audioBase = AudioBase::Entity;
        key.callerInWrappers = true;
        Check(owner::FamilyOf(key, cover, &shots) == owner::Family::Owned && !owner::OffShot(key, &shots),
            "off shot: only weapon audio packets");
        // An invalid weapon id is not a window; id 0:0 is.
        owner::ShotWindows zero;
        zero.Add(replay::census::kNoOwner, 100);
        zero.Add(0u, 100);
        zero.Seal();
        Check(zero.Size() == 1 && zero.Covers(0u, 150) && !zero.Covers(replay::census::kNoOwner, 100), "off shot: windows by valid weapon id");
        // A weapon audio packet of no recorded owner stays the shot's (no window can be checked).
        key.audioBase = AudioBase::WeaponAudio;
        key.caller = 0x251688D;
        key.callerInWrappers = true;
        key.owner = replay::census::kNoOwner;
        key.timeMs = 50000;
        Check(!owner::OffShot(key, &shots) && owner::FamilyOf(key, cover, &shots) == owner::Family::Gunshot,
            "off shot: an unknown weapon owner stays the shot's");
        key.owner = 0xFFFF0007u;
        Check(!owner::OffShot(key, &shots), "off shot: any instance of slot 0xFFFF is unknown");
    }
}

namespace
{
    // The moon's two session angles (moon_track.h). The recorder keeps changes
    // (and the held poll before one); the playhead state goes the short way round
    // the turn and holds a jump; cut and trim keep the state at the base; the
    // editor's phase row and the live clock win over the track; the give-back
    // writes until the clock's jump back is a few frames old.
    void TestMoonTrack()
    {
        using namespace replay;
        using namespace replay::moontrack;
        const auto sample = [](std::uint32_t t, double angle, double orbit) {
            MoonSample s;
            s.timeMs = t;
            s.angle = angle;
            s.orbit = orbit;
            s.moon[2] = 1.0f;
            s.year = 1899;
            s.month = 5;
            s.day = 7;
            return s;
        };
        const auto closeTo = [](double a, double b) { return std::fabs(a - b) < 1e-9; };

        // The engine's constants (sky update 0x145781C, build 1491.50).
        Check(closeTo(kTurn, 6.2831854820251465) && closeTo(kTurnPerDarkSecond * 9000.0, kTurn) &&
                  std::fabs(kOrbitPerSecond * 86400.0 * 27.322 - 2.0 * 3.14159265358979323846) < 1e-5,
            "moon: a turn per 2.5 dark game hours, an orbit per 27.322 game days");

        // Recorder: the first, then only changes; a held stretch keeps its last poll.
        std::vector<MoonSample> track;
        Recorder recorder;
        Check(recorder.Poll(track, sample(0, 1.0, 2.0)) && track.size() == 1, "moon: the first poll is kept");
        Check(!recorder.Poll(track, sample(250, 1.0 + kEpsilon / 2, 2.0)) && !recorder.Poll(track, sample(500, 1.0, 2.0)) &&
                  track.size() == 1,
            "moon: a still moon is one sample");
        Check(recorder.Poll(track, sample(750, 1.01, 2.0)) && track.size() == 3 && track[1].timeMs == 500 && track[2].timeMs == 750,
            "moon: a move keeps the last still poll before it");
        MoonSample dated = sample(1000, 1.01, 2.0);
        dated.day = 8;
        Check(recorder.Poll(track, dated) && track.size() == 4, "moon: a new date is a change");
        Check(!recorder.Poll(track, sample(900, 3.0, 2.0)), "moon: time going back is refused");
        MoonSample bad = sample(1250, std::numeric_limits<double>::quiet_NaN(), 2.0);
        Check(!recorder.Poll(track, bad) && !Valid(bad), "moon: an angle that is not a number is refused");
        Check(!Valid(sample(0, 100.0, 0.0)) && Valid(sample(0, -6.0, 6.2)), "moon: only values the engine's fmod can leave");
        Check(recorder.Finish(track, sample(1500, 1.01, 2.0)) && track.back().timeMs == 1500, "moon: the stop is always kept");

        // At: the short way round the fmod wrap, kept in the engine's range.
        std::vector<MoonSample> wrap{sample(0, kTurn - 0.02, 0.5), sample(1000, 0.02, 0.5002)};
        MoonSample at;
        Check(At(wrap, 500, at) && std::fabs(WrapDelta(0.0, at.angle)) < 1e-9 && std::fabs(at.angle) < kTurn &&
                  closeTo(at.orbit, 0.5001),
            "moon: interpolated across the wrap, not back round the sky");
        Check(At(wrap, 250, at) && closeTo(at.angle, kTurn - 0.01), "moon: a quarter of the way");
        // A clock jump between two samples is held, not swept.
        std::vector<MoonSample> jump{sample(0, 1.0, 0.5), sample(250, 2.5, 0.5)};
        Check(At(jump, 200, at) && closeTo(at.angle, 1.0) && At(jump, 250, at) && closeTo(at.angle, 2.5), "moon: a jump steps at the later sample");
        std::vector<MoonSample> orbitJump{sample(0, 1.0, 0.5), sample(250, 1.001, 1.5)};
        Check(At(orbitJump, 125, at) && closeTo(at.angle, 1.0) && closeTo(at.orbit, 0.5), "moon: a jump of the orbit (a date) steps too");
        Check(At(wrap, 0, at) && closeTo(at.angle, kTurn - 0.02) && At(wrap, 5000, at) && closeTo(at.angle, 0.02) && at.timeMs == 5000,
            "moon: before the first the first, after the last the last");
        Check(!At(std::vector<MoonSample>{}, 0, at), "moon: no track, no state");
        Check(closeTo(WrapDelta(0.1, kTurn - 0.1), -0.2) && closeTo(WrapDelta(-0.1, 0.1), 0.2), "moon: wrap delta the short way");

        // Cut and trim.
        std::vector<MoonSample> line{sample(0, 0.0, 0.0), sample(1000, 0.1, 0.0), sample(2000, 0.2, 0.0), sample(3000, 0.3, 0.0)};
        const std::vector<MoonSample> cut = Cut(line, 1500, 2500);
        Check(cut.size() == 2 && cut[0].timeMs == 0 && closeTo(cut[0].angle, 0.15) && cut[1].timeMs == 500 && closeTo(cut[1].angle, 0.2),
            "moon: a cut starts from the state at its base");
        std::vector<MoonSample> trimmed = line;
        Check(DropBefore(trimmed, 2500) == 2 && trimmed.size() == 2 && trimmed[0].timeMs == 2000, "moon: the trim keeps the sample in force");

        // Who owns the moon during a tick.
        Check(PlanPlayback(true, true, false) == Source::Recorded, "moon: the recorded track plays");
        Check(PlanPlayback(true, true, true) == Source::ClockLive, "moon: a live clock leaves the moon to the game");
        Check(PlanPlayback(true, false, false) == Source::NoTrack, "moon: an old clip keeps the session's moon");
        Check(PlanPlayback(false, true, false) == Source::Off && std::string(SourceName(Source::Off)) == "off",
            "moon: MoonReplay=0 writes nothing");

        // Give back: every tick until three frames after the clock went back, at least twice.
        GiveBack give;
        give.Arm(1.5, 0.25, 100);
        Check(give.armed && !give.Step(100) && !give.Step(101) && !give.Step(102) && give.Step(103) && !give.armed &&
                  give.ticks == 4 && closeTo(give.angle, 1.5) && closeTo(give.orbit, 0.25),
            "moon: the give-back lasts until the jump back is consumed");
        give.Arm(1.5, 0.25, 100);
        Check(!give.Step(105) && give.Step(106), "moon: at least two writes even when frames ran ahead");
        give.Arm(1.5, 0.25, 0x7FFFFFFE);
        Check(!give.Step(0x7FFFFFFF) && !give.Step(static_cast<std::int32_t>(0x80000000u)) && give.Step(static_cast<std::int32_t>(0x80000001u)),
            "moon: the frame counter wraps");
        give.Arm(1.5, 0.25, 100);
        bool done = false;
        for (int n = 0; n < 40 && !done; ++n)
            done = give.Step(50); // a frame counter behind the arm (never)
        Check(done && give.ticks == GiveBack::kMaxTicks, "moon: the give-back ends after its tick cap");

        // The log's comparison.
        const float up[3] = {0.0f, 0.0f, 1.0f};
        const float east[3] = {1.0f, 0.0f, 0.0f};
        const float zero[3] = {0.0f, 0.0f, 0.0f};
        Check(std::fabs(DirectionGapDeg(up, east) - 90.0f) < 1e-3f && DirectionGapDeg(up, zero) < 0.0f &&
                  std::fabs(ElevationDeg(up) - 90.0f) < 1e-3f && std::fabs(ElevationDeg(east)) < 1e-3f,
            "moon: direction gap and elevation in degrees");
    }

    // ---- T-pose ----

    replay::pose::Quat TurnZ(float radians)
    {
        replay::pose::Quat q;
        q.z = std::sin(radians * 0.5f);
        q.w = std::cos(radians * 0.5f);
        return q;
    }

    replay::pose::SkeletonLayout ChainLayout(std::uint32_t bones)
    {
        replay::pose::SkeletonLayout layout;
        layout.boneCount = bones;
        for (std::uint32_t bone = 0; bone < bones; ++bone)
        {
            layout.parent.push_back(bone == 0 ? replay::pose::kRootParent : static_cast<std::int32_t>(bone - 1));
            layout.ambiguous.push_back(0);
            layout.highPrecision.push_back(0);
            layout.evaluationOrder.push_back(bone);
        }
        return layout;
    }

    // A track of `frames` frames at the 30 Hz rate; bone 3 turns from
    // `turnFrame` on (never when past the end); the origin moves when `carried`.
    replay::pose::TrackExport StillTrack(std::uint32_t frames, std::uint32_t turnFrame, bool carried)
    {
        constexpr std::uint32_t kBones = 8;
        const replay::pose::SkeletonLayout layout = ChainLayout(kBones);
        replay::pose::StreamConfig config;
        config.keyframeIntervalMs = 1000;
        replay::pose::PoseTrack track;
        track.Begin(layout, config);
        std::vector<replay::pose::BoneQts> locals(kBones);
        for (std::uint32_t bone = 0; bone < kBones; ++bone)
        {
            locals[bone].rotation = TurnZ(0.1f * static_cast<float>(bone));
            locals[bone].translation[1] = bone ? 0.1f : 0.0f;
        }
        float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10, 20, 30, 1};
        for (std::uint32_t frame = 0; frame < frames; ++frame)
        {
            // From 0.6, not 0.3 (bone 3 rests at 0.3: the first turned frame would not
            // change and the turn would start one late).
            if (frame >= turnFrame)
                locals[3].rotation = TurnZ(0.6f + 0.05f * static_cast<float>(frame - turnFrame));
            if (carried)
                world[12] = 10.0f + 0.1f * static_cast<float>(frame);
            std::vector<replay::pose::BoneQts> objects(kBones);
            for (std::uint32_t bone = 0; bone < kBones; ++bone)
                objects[bone] = locals[bone];
            replay::pose::FrameInput input;
            input.timeMs = 30 + frame * 33u;
            input.world = world;
            input.locals = locals.data();
            input.objects = objects.data();
            track.Append(input);
        }
        replay::pose::TrackExport exported;
        track.Export(exported);
        return exported;
    }

    void TestNeverAnimatedSignature()
    {
        using namespace replay::tpose;
        const TrackSignature still = ReadSignature(StillTrack(1725, ~0u, false)); // 068 22:379: 1725 frames, 57.5 s
        Check(still.parsed && still.frames == 1725 && still.poseFrames == 0 && still.fullKeyframes == 1 &&
                  still.relativeKeyframes > 50 && still.worldFrames == 0 && NeverAnimated(still) &&
                  std::string_view(SignatureVerdict(still)) == "never_animated",
            "tpose: a record that never changed pose is never animated (relative keyframes carry nothing)");
        const TrackSignature carried = ReadSignature(StillTrack(300, ~0u, true));
        Check(NeverAnimated(carried) && carried.worldFrames == 299, "tpose: the origin moving does not count as animation");
        const TrackSignature turned = ReadSignature(StillTrack(300, 150, false));
        Check(turned.parsed && !NeverAnimated(turned) && turned.poseFrames >= 140 && turned.firstPoseMs == 30 + 150 * 33 &&
                  std::string_view(SignatureVerdict(turned)) == "animated",
            "tpose: one bone turning makes the record animated, from its frame");
        const TrackSignature late = ReadSignature(StillTrack(300, 299, false));
        Check(!NeverAnimated(late) && late.poseFrames == 1, "tpose: a single moving frame is enough");
        const TrackSignature brief = ReadSignature(StillTrack(40, ~0u, false));
        Check(!NeverAnimated(brief) && std::string_view(SignatureVerdict(brief)) == "still_short",
            "tpose: a still record under 60 frames is not judged");
        replay::pose::TrackExport broken = StillTrack(100, ~0u, false);
        broken.chunks[0].frameOffsets[1] = static_cast<std::uint32_t>(broken.chunks[0].bytes.size());
        const TrackSignature unreadable = ReadSignature(broken);
        Check(!unreadable.parsed && !NeverAnimated(unreadable) && std::string_view(SignatureVerdict(unreadable)) == "unreadable",
            "tpose: a frame header past the chunk is refused, never judged");
        Check(ReadSignature(replay::pose::TrackExport{}).parsed && !NeverAnimated(ReadSignature(replay::pose::TrackExport{})),
            "tpose: an empty track is read and not judged");
        // The recorder's counters of 068 (POSE_V2_CAPTURE_STOP 22:379).
        Check(RecordedNeverAnimated(1725, 0, 1724 * 861ull, 1724 * 861ull), "tpose: 068 22:379 counters are never animated");
        Check(!RecordedNeverAnimated(1725, 3, 100, 100) && !RecordedNeverAnimated(1725, 0, 100, 99) &&
                  !RecordedNeverAnimated(0, 0, 0, 0) && !RecordedNeverAnimated(1, 0, 0, 0),
            "tpose: a detour call, one changed channel or no samples is not never animated");
    }

    // A fake engine arena: a skeleton with its groups, skeleton data and bone
    // records at the finalizer's offsets; addresses are offsets from 0x10000.
    struct FakeArena
    {
        static constexpr std::uintptr_t kBase = 0x10000;
        std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x4000, 0);
        template <typename T>
        void Put(std::uintptr_t address, const T& value)
        {
            std::memcpy(bytes.data() + (address - kBase), &value, sizeof(T));
        }
        bool Read(std::uintptr_t address, void* out, std::size_t size) const
        {
            if (address < kBase || address - kBase > bytes.size() || size > bytes.size() - (address - kBase))
                return false;
            std::memcpy(out, bytes.data() + (address - kBase), size);
            return true;
        }
    };

    void TestBindPoseRead()
    {
        using namespace replay::tpose;
        FakeArena arena;
        const std::uintptr_t skeleton = FakeArena::kBase + 0x100;
        const std::uintptr_t dataA = FakeArena::kBase + 0x400, dataB = FakeArena::kBase + 0x500;
        const std::uintptr_t recordsA = FakeArena::kBase + 0x800, recordsB = FakeArena::kBase + 0xC00;
        arena.Put<std::uint32_t>(skeleton + kSkeletonGroupCountOffset, 2);
        arena.Put<std::uint32_t>(skeleton + kSkeletonBoneCountOffset, 5);
        arena.Put<std::uintptr_t>(skeleton + kSkeletonGroupDataOffset, dataA);
        arena.Put<std::uintptr_t>(skeleton + kSkeletonGroupDataOffset + 8, dataB);
        arena.Put<std::uint16_t>(dataA + kDataBoneCountOffset, 3);
        arena.Put<std::uintptr_t>(dataA + kDataBonesOffset, recordsA);
        arena.Put<std::uint16_t>(dataB + kDataBoneCountOffset, 2);
        arena.Put<std::uintptr_t>(dataB + kDataBonesOffset, recordsB);
        for (std::uint32_t bone = 0; bone < 3; ++bone)
        {
            arena.Put(recordsA + bone * kBoneRecordBytes, TurnZ(0.1f * static_cast<float>(bone)));
            arena.Put<float>(recordsA + bone * kBoneRecordBytes + 0x10, 99.0f); // translation: not taken
        }
        for (std::uint32_t bone = 0; bone < 2; ++bone)
            arena.Put(recordsB + bone * kBoneRecordBytes, TurnZ(1.0f + 0.1f * static_cast<float>(bone)));
        const auto read = [&](std::uintptr_t address, void* out, std::size_t size) { return arena.Read(address, out, size); };
        std::vector<replay::pose::Quat> bind;
        Check(std::string_view(ReadBindRotations(read, skeleton, bind)).empty() && bind.size() == 5 &&
                  std::fabs(bind[1].z - TurnZ(0.1f).z) < 1e-6f && std::fabs(bind[3].z - TurnZ(1.0f).z) < 1e-6f &&
                  std::fabs(bind[4].w - TurnZ(1.1f).w) < 1e-6f,
            "tpose: the bind rotations group after group, 0x50 bytes a bone");
        arena.Put<std::uint32_t>(skeleton + kSkeletonBoneCountOffset, 6);
        Check(std::string_view(ReadBindRotations(read, skeleton, bind)) == "count_mismatch" && bind.empty(),
            "tpose: groups that do not add up to the skeleton's bones are refused");
        arena.Put<std::uint32_t>(skeleton + kSkeletonBoneCountOffset, 4);
        Check(std::string_view(ReadBindRotations(read, skeleton, bind)) == "count_mismatch", "tpose: a group past the bones is refused");
        arena.Put<std::uint32_t>(skeleton + kSkeletonBoneCountOffset, 5);
        arena.Put<std::uint32_t>(skeleton + kSkeletonGroupCountOffset, 0);
        Check(std::string_view(ReadBindRotations(read, skeleton, bind)) == "bad_groups", "tpose: no group, no read");
        arena.Put<std::uint32_t>(skeleton + kSkeletonGroupCountOffset, kMaxGroups + 1);
        Check(std::string_view(ReadBindRotations(read, skeleton, bind)) == "bad_groups", "tpose: a group count past the pointers");
        arena.Put<std::uint32_t>(skeleton + kSkeletonGroupCountOffset, 2);
        arena.Put<std::uintptr_t>(skeleton + kSkeletonGroupDataOffset + 8, 0);
        Check(std::string_view(ReadBindRotations(read, skeleton, bind)) == "no_group_data", "tpose: a null group pointer");
        arena.Put<std::uintptr_t>(skeleton + kSkeletonGroupDataOffset + 8, 0x7FFF0000ull);
        Check(std::string_view(ReadBindRotations(read, skeleton, bind)) == "read_group", "tpose: an unreadable group stops the read");
        Check(std::string_view(ReadBindRotations(read, 0, bind)) == "no_skeleton", "tpose: no skeleton");

        // The drawn locals: the pose buffer pointer at +0, the bone count at +0x108.
        const std::uintptr_t buffer = FakeArena::kBase + 0x1800;
        arena.Put<std::uintptr_t>(skeleton + kSkeletonPoseBufferOffset, buffer);
        replay::pose::BoneQts bone2;
        bone2.rotation = TurnZ(0.7f);
        arena.Put(buffer + 2 * sizeof(replay::pose::BoneQts), bone2);
        std::vector<replay::pose::BoneQts> drawn;
        Check(std::string_view(ReadDrawnLocals(read, skeleton, drawn)).empty() && drawn.size() == 5 &&
                  std::fabs(drawn[2].rotation.z - TurnZ(0.7f).z) < 1e-6f,
            "tpose: the drawn locals from the pose buffer");
        arena.Put<std::uintptr_t>(skeleton + kSkeletonPoseBufferOffset, 0);
        Check(std::string_view(ReadDrawnLocals(read, skeleton, drawn)) == "bad_buffer" && drawn.empty(), "tpose: no pose buffer");

        // The layout is used only when the finalizer's code is the one read.
        std::vector<std::uintptr_t> asked;
        const bool proven = BindLayoutProven([&](std::uintptr_t rva, const std::uint8_t* code, std::size_t size) {
            asked.push_back(rva);
            return code != nullptr && size > 0;
        });
        Check(proven && asked.size() == 5 && asked[0] == 0x266C334 && asked[1] == 0x266C630,
            "tpose: five code sites of the finalizer are checked");
        Check(!BindLayoutProven([](std::uintptr_t rva, const std::uint8_t*, std::size_t) { return rva != 0x266C67C; }),
            "tpose: one site that differs refuses the layout");
        Check(kFinalizerBindLoadSignature[15] == 0x48 && kFinalizerBindLoadSignature[18] == 0x28 &&
                  kFinalizerBindLoadSignature[22] == 0x5E && kFinalizerBindStrideSignature[3] == kBoneRecordBytes,
            "tpose: the signatures carry the offsets they prove (+28h, +5Eh, 50h)");
    }

    void TestBindPoseVerdict()
    {
        using namespace replay::tpose;
        std::vector<replay::pose::Quat> bind;
        std::vector<replay::pose::BoneQts> pose(100);
        for (std::uint32_t bone = 0; bone < 100; ++bone)
        {
            bind.push_back(TurnZ(0.02f * static_cast<float>(bone)));
            pose[bone].rotation = bind.back();
        }
        BindMatch m = CompareLocalsWithBind(pose, bind);
        Check(m.compared == 100 && m.posed == 0 && m.maxRad < 1e-3f && IsBindPose(m), "tpose: the bind pose itself");
        for (std::uint32_t bone = 0; bone < 100; ++bone)
            pose[bone].rotation = TurnZ(0.02f * static_cast<float>(bone) + 0.01f); // the stream's error, half a degree
        Check(IsBindPose(CompareLocalsWithBind(pose, bind)), "tpose: rotation noise under 5 degrees stays the bind pose");
        for (std::uint32_t bone = 0; bone < 5; ++bone)
            pose[bone].rotation = TurnZ(1.4f); // an arm hanging down
        m = CompareLocalsWithBind(pose, bind);
        Check(m.posed == 5 && IsBindPose(m), "tpose: up to 5 percent of the bones turned is still the bind pose");
        for (std::uint32_t bone = 0; bone < 20; ++bone)
            pose[bone].rotation = TurnZ(1.4f + 0.02f * static_cast<float>(bone));
        m = CompareLocalsWithBind(pose, bind);
        // 068: the animated uncle 19:376 never came under 133 of 599 bones away
        // from the bind-posed one, the animated Hosea 24:381 98 of 861.
        Check(m.posed == 20 && !IsBindPose(m) && m.maxRad > 1.3f, "tpose: a fifth of the bones turned is a pose");
        // Bones without a usable rotation are not compared.
        std::vector<replay::pose::BoneQts> sparse(100);
        for (std::uint32_t bone = 0; bone < 100; ++bone)
        {
            sparse[bone].rotation = bind[bone];
            if (bone >= 10)
                sparse[bone].rotation = replay::pose::Quat{0.0f, 0.0f, 0.0f, 0.0f};
        }
        m = CompareLocalsWithBind(sparse, bind);
        Check(m.compared == 10 && !IsBindPose(m), "tpose: under 16 compared bones there is no verdict");
        Check(RotationAngle(TurnZ(0.0f), TurnZ(3.14159265f)) > 3.1f &&
                  std::fabs(RotationAngle(TurnZ(0.5f), replay::pose::Quat{-TurnZ(0.5f).x, -TurnZ(0.5f).y, -TurnZ(0.5f).z, -TurnZ(0.5f).w})) < 1e-3f,
            "tpose: q and -q are the same rotation");
        // Fewer bind rows than bones: only the common bones.
        std::vector<replay::pose::Quat> shortBind(bind.begin(), bind.begin() + 50);
        Check(CompareLocalsWithBind(pose, shortBind).compared == 50, "tpose: only the bones both sides have");

        // The hide.
        TrackSignature never;
        never.parsed = true;
        never.frames = 1725;
        never.firstMs = 30;
        never.lastMs = 57501;
        TrackSignature animated = never;
        animated.poseFrames = 1724;
        BindMatch bindPose;
        bindPose.bones = bindPose.compared = 861;
        BindMatch corpse = bindPose;
        corpse.posed = 152; // 057: still corpses, 86 to 216 bones away from each other
        Check(Decide(HideMode::BindProven, never, &bindPose) == HideVerdict::HideBind &&
                  Hides(Decide(HideMode::BindProven, never, &bindPose)),
            "tpose: never animated in the bind pose: hidden (068 22:379)");
        Check(Decide(HideMode::BindProven, never, &corpse) == HideVerdict::ShowPosed &&
                  !Hides(Decide(HideMode::Signature, never, &corpse)),
            "tpose: never animated in another pose (a still corpse): shown, in every mode");
        // A corpse whose ragdoll turned only its physics bones (pelvis, spine, neck,
        // head, limbs: some 21 of 861) is under the watch's 5 % but never hidden
        // (IsHideBindPose).
        BindMatch ragdoll = bindPose;
        ragdoll.posed = 21;
        Check(IsBindPose(ragdoll) && !IsHideBindPose(ragdoll) && Decide(HideMode::BindProven, never, &ragdoll) == HideVerdict::ShowPosed,
            "tpose: a corpse with only its ragdoll bones turned is shown");
        BindMatch nearBind = bindPose;
        nearBind.posed = 4; // max(2, 861 / 200)
        Check(IsHideBindPose(nearBind) && Decide(HideMode::BindProven, never, &nearBind) == HideVerdict::HideBind,
            "tpose: a handful of bones off the bind pose is still hidden");
        nearBind.posed = 5;
        Check(!IsHideBindPose(nearBind) && Decide(HideMode::BindProven, never, &nearBind) == HideVerdict::ShowPosed,
            "tpose: one bone more is shown");
        Check(Decide(HideMode::BindProven, never, nullptr) == HideVerdict::ShowUnproven &&
                  Decide(HideMode::Signature, never, nullptr) == HideVerdict::HideUnproven,
            "tpose: the bind pose unknown: shown, hidden only with HideNeverAnimated=2");
        Check(Decide(HideMode::Off, never, &bindPose) == HideVerdict::Off && Decide(HideMode::BindProven, animated, &bindPose) == HideVerdict::Show,
            "tpose: HideNeverAnimated=0, or an animated record: shown");
        Check(std::string_view(HideVerdictText(HideVerdict::HideBind)) == "hide_bind" &&
                  std::string_view(HideVerdictText(HideVerdict::ShowUnproven)) == "show_unproven",
            "tpose: verdict names");
    }

    void TestTPoseWatch()
    {
        using namespace replay::tpose;
        TPoseWatch watch;
        WatchReport report;
        Check(!watch.Note(7, true, report), "tpose watch: one bind check is not an episode yet");
        Check(watch.Note(7, true, report) && report.checks == kConfirmChecks, "tpose watch: the second in a row opens it");
        Check(!watch.Note(7, true, report), "tpose watch: one line per episode");
        Check(!watch.Note(7, false, report) && !watch.Note(7, true, report), "tpose watch: a posed check closes it");
        Check(watch.Note(7, true, report), "tpose watch: a new run opens a second episode");
        watch.Forget(7);
        Check(!watch.Note(7, true, report), "tpose watch: hidden in between, the run starts again");
        Check(!watch.Note(9, false, report) && !watch.Note(9, false, report), "tpose watch: a posed body never opens one");
        watch.NoteUnread();
        WatchSummary s = watch.Summary();
        Check(s.episodes == 2 && s.entities == 1 && s.checks == 9 && s.bindChecks == 6 && s.unread == 1 && s.open == 0,
            "tpose watch: the summary counts");
        watch.Note(7, true, report);
        Check(watch.Summary().open == 1, "tpose watch: an episode still open at the end");
        watch.Reset();
        Check(watch.Summary().checks == 0, "tpose watch: reset");

        // When the watch judges.
        WatchGate gate;
        gate.Configure(1500, true);
        Check(gate.Tick(0, 0, false, false, true, false, 1.0) && gate.Judge(0, true) == WatchSkip::Preparing,
            "tpose gate: not while the replay prepares");
        Check(gate.Tick(100, 0, false, false, false, true, 1.0) && gate.Judge(100, true) == WatchSkip::Hidden,
            "tpose gate: not while the screen is faded (the clip load wait)");
        gate.Tick(200, 10, false, false, false, false, 1.0);
        Check(gate.Judge(200, true) == WatchSkip::Settle && gate.Judge(1599, true) == WatchSkip::Settle,
            "tpose gate: the settle window runs from the last faded frame");
        Check(!gate.Tick(1800, 1610, false, false, false, false, 1.0) && gate.Judge(1800, true) == WatchSkip::None,
            "tpose gate: normal play past the window judges");
        Check(gate.Judge(1800, false) == WatchSkip::OffScreen, "tpose gate: a body off screen is not judged");
        Check(gate.Tick(1816, 38225, false, false, false, false, 1.0) && gate.Judge(2000, true) == WatchSkip::Settle,
            "tpose gate: a jump of the playhead while playing settles");
        Check(gate.Tick(3400, 39600, true, false, false, false, 1.0) && gate.Judge(4800, true) == WatchSkip::Settle &&
                  gate.Judge(4901, true) == WatchSkip::None,
            "tpose gate: a pause settles for the window, then a paused body is judged");
        Check(gate.Tick(5000, 38849, true, false, false, false, 1.0) && gate.Judge(5100, true) == WatchSkip::Settle,
            "tpose gate: a seek while paused settles");
        Check(!gate.Tick(5016, 38849, true, false, false, false, 1.0) && gate.Tick(5032, 38849, true, true, false, false, 1.0),
            "tpose gate: paused still is quiet, a scrub is an event");
        Check(!gate.Tick(9000, 38849, true, false, false, false, 1.0) && gate.Tick(9016, 38849, false, false, false, false, 1.0),
            "tpose gate: a resume is an event");
        Check(!gate.Tick(9100, 38960, false, false, false, false, 2.0), "tpose gate: fast play at 2x is not a jump");
        gate.Configure(0, false);
        Check(gate.Tick(9116, 1000, false, false, false, false, 1.0) && gate.Judge(9116, false) == WatchSkip::None,
            "tpose gate: TPoseSettleMs=0 and TPoseOnScreenOnly=0 judge at once");
        TPoseWatch held;
        held.Note(5, true, report);
        held.Hold(5, WatchSkip::Settle);
        Check(!held.Note(5, true, report), "tpose gate: a held check restarts the run (the 035 seeks)");
        Check(held.Note(5, true, report), "tpose gate: two judged checks in a row still open an episode (a real T-pose)");
        held.Hold(5, WatchSkip::OffScreen);
        Check(!held.Note(5, true, report) && !held.Note(5, true, report) && held.Summary().episodes == 1,
            "tpose gate: an open episode seen again after a hold is the same one");
        Check(held.Summary().skipped[static_cast<int>(WatchSkip::Settle)] == 1 &&
                  held.Summary().skipped[static_cast<int>(WatchSkip::OffScreen)] == 1,
            "tpose gate: skips counted per reason");

        // The QA ring and the quiet log keep the lines.
        replay::qa::Anomaly anomaly;
        Check(replay::qa::ClassifyLine("POSE_V2_TPOSE,id=22:379,kind=ped,model=0x490733e8,record=0x16017b,posed=0,compared=861,"
                                       "max_deg=1,checks=2,never_animated=1,mapped=0,camera_m=4,on_screen=1,playhead_ms=54771\n",
                  0, anomaly) &&
                  anomaly.kind == replay::qa::Kind::TPose && anomaly.entity == "22:379" && anomaly.playheadMs == 54771 &&
                  std::string(replay::qa::KindText(replay::qa::Kind::TPose)) == "tpose",
            "tpose: POSE_V2_TPOSE is the QA kind tpose");
        Check(replay::qa::ClassifyLine("POSE_V2_NEVER_ANIMATED,id=18:375,kind=ped,verdict=show_unproven,playhead_ms=1\n", 0, anomaly) &&
                  anomaly.kind == replay::qa::Kind::TPose,
            "tpose: a never animated body shown unproven is a QA T-pose");
        Check(!replay::qa::ClassifyLine("POSE_V2_NEVER_ANIMATED,id=18:375,kind=ped,verdict=hide_bind,playhead_ms=1\n", 0, anomaly),
            "tpose: a hidden one is not an anomaly");
        Check(replay::log::EssentialLine("POSE_V2_TPOSE,id=1:2\n") && replay::log::EssentialLine("POSE_V2_NEVER_ANIMATED,id=1:2\n") &&
                  replay::log::EssentialLine("POSE_V2_TPOSE_SUMMARY,checks=0\n"),
            "tpose: the lines survive Debug=0");
    }
}

int RunScenerySpeechTests()
{
    g_scenerySpeechFailures = 0;
    TestSpeechRoute();
    TestRopeModels();
    TestSceneryVisibility();
    TestHatChains();
    TestHatFix();
    TestAnimVisibleComponent();
    TestAnimLodDirector();
    TestEntityQuery();
    TestEntityQueryTrack();
    TestVisibilityTrim();
    TestFragPiece();
    TestLoot();
    TestSkeletonReread();
    TestPropBoneWatch();
    TestSceneryChildren();
    TestLateScenery();
    TestAnimalBank();
    TestFoleyVariants();
    TestPedVariablesBlock();
    TestWeatherTransitionTrack();
    TestWeatherTransitionWrite();
    TestWindDirectionDegrees();
    TestTimecycleTrack();
    TestNoRecordLayerDiff();
    TestNoRecordAlternates();
    TestNoRecordSteps();
    TestHatLook();
    TestSpeechRequest();
    TestResumeOldCopy();
    TestFireOwnership();
    TestFireDescRebuild();
    TestFireMatch();
    TestShotAudioDelay();
    TestWeaponOffShot();
    TestMoonTrack();
    TestNeverAnimatedSignature();
    TestBindPoseRead();
    TestBindPoseVerdict();
    TestTPoseWatch();
    if (g_scenerySpeechFailures == 0)
        std::printf("qa 09/10 batch tests passed\n");
    return g_scenerySpeechFailures;
}
