#pragma once

// The dynamic mixer's scenes and the entities' audio mix groups, recorded and
// replayed the way a replay editor usually replays mixer scenes (start and
// stop with the scene hash), pure part.
//
// Engine facts (build 1491.50):
//   - the scripted conversation manager's update (0x2FC4F58 -> 0x2FCC0FC)
//     picks the dominant playing conversation (0x2FA3AAC: state 3 at +0x130,
//     the highest priority +0x144, then the nearest) among its conversations
//     (+0x860, count +0x980), stops the scene of every other one (0x2FC2964:
//     conversation +0xE8 -> mixer stop 0x327A07C) and starts, once, the scene
//     0x5B9DE6B4 on the dominant one (0x2FC2200 -> mixer start 0x2F19018 at
//     0x2FC2330, scene pointer kept at conversation +0xE8);
//   - that scene gets a filter (0x2FC2200): a list of the conversation's
//     participants' tags (participant array +0x30, count u16 +0x38, stride
//     0xA0: tag u32 at +0x7C for each participant with the byte +0x98 set),
//     mode 1 and invert 1, plus the metadata 0xB77A4E4D; the mixer's match
//     (0x25315B0 -> 0x2531748 -> 0x2531528) compares the list with the audio
//     mix group hashes of the sound's entity (entity audio vtable +0x78,
//     words +0xEC, hashed by 0x252AE84). Inverted: the scene acts on the
//     sounds of the entities that are NOT in the participants' mix groups.
//     That is the conversation duck: everyone but the speakers;
//   - the replay says the lines one by one on the direct path, so no
//     conversation exists and the manager starts no scene: without this,
//     ambient chatter plays at its normal level under a conversation;
//   - an entity's mix group is set by ADD_ENTITY_TO_AUDIO_MIX_GROUP (handler
//     0xF65E34 -> 0x2EE52E8(mixer 0x5B98760, entity, slot 0, hash, fade)) and
//     read by _GET_ENTITY_AUDIO_MIX_GROUP (slot 0, 0 when none);
//   - a script scene lives in the calling script thread's slot of the script
//     audio object (0x5C81C20 + 0x37D0, 32 slots of 0x380 keyed by the
//     active thread's id, 0x31BAD14); 0x31C8820 returns true without
//     starting anything when that slot already holds a scene of the hash;
//     the hash variant of START_AUDIO_SCENE is the native 0xDC2F83A0612CA34D
//     (handler 0xF65254 -> 0x31C8910) and of STOP_AUDIO_SCENE 0x6AB944DF68B512D3
//     (handler 0xF653E4 -> 0x31C946C): the playback uses them;
//   - a scene's variables (SET_AUDIO_SCENE_VARIABLE -> 0x31C66EC, and the
//     engine's own 0x3279628(scene, name hash, value)) are the array of 10
//     {name hash, value} at scene +0x18, count at +0x68; the recording polls
//     it and the playback sets the recorded values with 0x3279628;
//   - the engine's code scenes (caller other than the two above) are started
//     by systems driven by the game state (weather and ambient zones
//     0x31CEFD4, the refcounted scene table 0x3211FE7, the audio engine's
//     update 0x2F29985, ...); the playback may start them again directly in
//     the mixer (0x2F19018 with a reference of the replay's own), only when
//     the engine does not already have a scene of the hash open (then the
//     engine owns it), behind MixSceneReplayCode (default 0);
//   - a stop whose scene no longer carries the instance's hash (+0x328) is a
//     stale reference; the recording polls each open scene (+0x310
//     reference, +0x328 hash) and ends it when it vanished.
// So the recording keeps every scene start and stop (hash, who started it,
// the participants' tags of a conversation scene), the scene variables and
// each recorded ped's mix group; the playback puts the puppets in the
// recorded groups and starts the conversation scenes again with the recorded
// tags (a conversation object of the replay's own) and the script scenes by
// hash.
//
// Pure: unit tested in voices_tests.cpp.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace replay::mixscene
{
    // Return addresses of the mixer start calls (RVA, build 1491.50).
    constexpr std::uint32_t kCallerConversation = 0x2FC2335; // 0x2FC2200 at 0x2FC2330
    constexpr std::uint32_t kCallerScript = 0x31C88EB;       // START_AUDIO_SCENE: 0x31C8820 at 0x31C88E6
    constexpr std::uint32_t kConversationScene = 0x5B9DE6B4;

    enum class Kind : std::uint8_t
    {
        Code = 0,         // the engine's own code: recorded, logged, not replayed
        Script = 1,       // a script's START_AUDIO_SCENE: replayed by hash
        Conversation = 2, // the conversation duck: replayed with the participants' tags
    };
    constexpr std::uint8_t kKindCount = 3;

    inline Kind KindOfCaller(std::uint32_t callerRva)
    {
        if (callerRva == kCallerConversation)
            return Kind::Conversation;
        if (callerRva == kCallerScript)
            return Kind::Script;
        return Kind::Code;
    }

    inline const char* KindName(Kind kind)
    {
        switch (kind)
        {
        case Kind::Code: return "code";
        case Kind::Script: return "script";
        case Kind::Conversation: return "conversation";
        }
        return "unknown";
    }

    inline bool Replayable(Kind kind) { return kind == Kind::Script || kind == Kind::Conversation; }

    // Which kinds the playback starts again (bit per kind).
    constexpr std::uint8_t kReplayScript = 1;
    constexpr std::uint8_t kReplayConversation = 2;
    constexpr std::uint8_t kReplayCode = 4;
    constexpr std::uint8_t kReplayDefault = kReplayScript | kReplayConversation;
    inline bool Replayable(Kind kind, std::uint8_t kinds)
    {
        switch (kind)
        {
        case Kind::Script: return (kinds & kReplayScript) != 0;
        case Kind::Conversation: return (kinds & kReplayConversation) != 0;
        case Kind::Code: return (kinds & kReplayCode) != 0;
        }
        return false;
    }

    // The scene object's fields the replay reads (build 1491.50): the
    // reference the mixer clears on stop (+0x310, written by 0x2F18E40), the
    // hash (+0x328, written by 0x2F19018), the variables (+0x18, 10 pairs of
    // name hash and float, count at +0x68, 0x3279628).
    constexpr std::uint32_t kSceneReference = 0x310;
    constexpr std::uint32_t kSceneHash = 0x328;
    constexpr std::uint32_t kSceneVariables = 0x18;
    constexpr std::uint32_t kSceneVariableCount = 0x68;
    constexpr std::size_t kMaxVariables = 10;

    // A hash list from the ini ("0x1bfc4208,0x24bfc688", decimal or hex,
    // separated by commas, spaces or semicolons).
    inline std::vector<std::uint32_t> ParseHashList(const char* text)
    {
        std::vector<std::uint32_t> out;
        if (!text)
            return out;
        const char* p = text;
        while (*p)
        {
            while (*p == ',' || *p == ';' || *p == ' ' || *p == '\t')
                ++p;
            if (!*p)
                break;
            std::uint64_t value = 0;
            int base = 10;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            {
                base = 16;
                p += 2;
            }
            bool digits = false;
            for (;; ++p)
            {
                const char c = *p;
                int d = -1;
                if (c >= '0' && c <= '9')
                    d = c - '0';
                else if (base == 16 && c >= 'a' && c <= 'f')
                    d = c - 'a' + 10;
                else if (base == 16 && c >= 'A' && c <= 'F')
                    d = c - 'A' + 10;
                if (d < 0)
                    break;
                value = value * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(d);
                if (value > 0xFFFFFFFFull)
                    value = 0xFFFFFFFFull + 1; // too big: dropped below
                digits = true;
            }
            if (digits && value && value <= 0xFFFFFFFFull)
                out.push_back(static_cast<std::uint32_t>(value));
            while (*p && *p != ',' && *p != ';' && *p != ' ' && *p != '\t')
                ++p; // the rest of a malformed item
        }
        return out;
    }

    constexpr std::uint32_t kOpen = 0xFFFFFFFFu;
    constexpr std::size_t kMaxTags = 8;

    struct Instance
    {
        std::uint32_t id = 0;     // per recording session, never reused
        std::uint32_t hash = 0;   // the scene's metadata hash
        std::uint32_t caller = 0; // RVA of the start's return address
        Kind kind = Kind::Code;
        std::uint32_t startMs = 0;
        std::uint32_t stopMs = kOpen; // kOpen: still active at the end
        std::uint8_t tagCount = 0;
        std::uint32_t tags[kMaxTags] = {};

        bool ActiveAt(std::uint32_t timeMs) const { return startMs <= timeMs && (stopMs == kOpen || timeMs < stopMs); }
    };

    // An entity's audio mix group from timeMs on (0: none).
    struct GroupSample
    {
        std::uint32_t timeMs = 0;
        std::uint32_t entity = 0; // replay id, packed
        std::uint32_t group = 0;
    };

    // A scene variable's value from timeMs on (MIXS v2).
    struct VarSample
    {
        std::uint32_t timeMs = 0;
        std::uint32_t id = 0;   // the instance
        std::uint32_t name = 0; // the variable's name hash
        float value = 0.0f;
    };

    // MIXS v3: the recording scanned the mixer's scene pool
    // (mix_scene_pool_core.h): the scenes open when it began were adopted from
    // the pool and the drivers' direct scenes (the speech duck, the state
    // scenes) are in it; their absence means off.
    constexpr std::uint32_t kTrackPoolScanned = 1;

    // The recording's part (Recording::mixScenes, section MIXS).
    struct Track
    {
        std::vector<Instance> instances; // by start time
        std::vector<GroupSample> groups; // by time
        std::vector<VarSample> vars;     // by time (MIXS v2)
        std::uint32_t lostStops = 0;     // stops of a scene the recording never saw start
        std::uint32_t flags = 0;         // kTrackPoolScanned (MIXS v3)

        void Clear()
        {
            instances.clear();
            groups.clear();
            vars.clear();
            lostStops = 0;
            flags = 0;
        }
        bool Empty() const { return instances.empty() && groups.empty() && vars.empty(); }
        std::size_t ApproxBytes() const
        {
            return instances.capacity() * sizeof(Instance) + groups.capacity() * sizeof(GroupSample) +
                vars.capacity() * sizeof(VarSample);
        }

        bool HasInstance(std::uint32_t id) const
        {
            for (const Instance& instance : instances)
                if (instance.id == id)
                    return true;
            return false;
        }

        // The variables of instance `id` in force at timeMs (last value per
        // name at or before it), at most kMaxVariables names.
        std::vector<std::pair<std::uint32_t, float>> VarsAt(std::uint32_t id, std::uint32_t timeMs) const
        {
            std::vector<std::pair<std::uint32_t, float>> out;
            for (const VarSample& sample : vars)
            {
                if (sample.timeMs > timeMs)
                    break;
                if (sample.id != id)
                    continue;
                bool found = false;
                for (auto& entry : out)
                    if (entry.first == sample.name)
                    {
                        entry.second = sample.value;
                        found = true;
                    }
                if (!found && out.size() < kMaxVariables)
                    out.emplace_back(sample.name, sample.value);
            }
            return out;
        }

        Instance* Find(std::uint32_t id)
        {
            for (Instance& instance : instances)
                if (instance.id == id)
                    return &instance;
            return nullptr;
        }

        // The ring buffer drops what is older than keepFromMs: a scene that
        // ended before it goes; one still active stays (it starts before the
        // window and CutInto starts it at the window's first instant). Per
        // entity the last group before keepFromMs stays, moved to keepFromMs.
        void DropBefore(std::uint32_t keepFromMs)
        {
            instances.erase(std::remove_if(instances.begin(), instances.end(),
                                [keepFromMs](const Instance& i) { return i.stopMs != kOpen && i.stopMs < keepFromMs; }),
                instances.end());
            std::map<std::uint32_t, std::size_t> last; // entity -> index of its last sample before keepFromMs
            for (std::size_t n = 0; n < groups.size(); ++n)
                if (groups[n].timeMs < keepFromMs)
                    last[groups[n].entity] = n;
            std::vector<GroupSample> kept;
            kept.reserve(groups.size());
            for (std::size_t n = 0; n < groups.size(); ++n)
            {
                if (groups[n].timeMs >= keepFromMs)
                    continue;
                const auto found = last.find(groups[n].entity);
                if (found != last.end() && found->second == n && groups[n].group != 0)
                {
                    GroupSample moved = groups[n];
                    moved.timeMs = keepFromMs;
                    kept.push_back(moved);
                }
            }
            for (const GroupSample& sample : groups)
                if (sample.timeMs >= keepFromMs)
                    kept.push_back(sample);
            std::stable_sort(kept.begin(), kept.end(),
                [](const GroupSample& a, const GroupSample& b) { return a.timeMs < b.timeMs; });
            groups.swap(kept);
            // The variables: per instance still kept and name, the last value
            // before keepFromMs moves to keepFromMs; the rest of the old ones go.
            std::vector<VarSample> keptVars;
            keptVars.reserve(vars.size());
            std::map<std::uint64_t, std::size_t> lastVar; // (id << 32 | name) -> index
            for (std::size_t n = 0; n < vars.size(); ++n)
                if (vars[n].timeMs < keepFromMs)
                    lastVar[(static_cast<std::uint64_t>(vars[n].id) << 32) | vars[n].name] = n;
            for (std::size_t n = 0; n < vars.size(); ++n)
            {
                if (vars[n].timeMs >= keepFromMs || !HasInstance(vars[n].id))
                    continue;
                const auto found = lastVar.find((static_cast<std::uint64_t>(vars[n].id) << 32) | vars[n].name);
                if (found != lastVar.end() && found->second == n)
                {
                    VarSample moved = vars[n];
                    moved.timeMs = keepFromMs;
                    keptVars.push_back(moved);
                }
            }
            for (const VarSample& sample : vars)
                if (sample.timeMs >= keepFromMs && HasInstance(sample.id))
                    keptVars.push_back(sample);
            std::stable_sort(keptVars.begin(), keptVars.end(),
                [](const VarSample& a, const VarSample& b) { return a.timeMs < b.timeMs; });
            vars.swap(keptVars);
        }

        // The window [baseMs, endMs] as a clip of its own: times move by
        // baseMs; a scene active at baseMs starts at 0, one still active at
        // endMs stays open; each entity's group at baseMs is a sample at 0.
        void CutInto(Track& out, std::uint32_t baseMs, std::uint32_t endMs) const
        {
            out.Clear();
            out.lostStops = lostStops;
            out.flags = flags;
            for (const Instance& instance : instances)
            {
                if (instance.startMs > endMs)
                    continue;
                if (instance.stopMs != kOpen && instance.stopMs <= baseMs)
                    continue;
                Instance copy = instance;
                copy.startMs = instance.startMs > baseMs ? instance.startMs - baseMs : 0;
                copy.stopMs = instance.stopMs == kOpen || instance.stopMs > endMs ? kOpen : instance.stopMs - baseMs;
                out.instances.push_back(copy);
            }
            std::map<std::uint32_t, std::uint32_t> at; // entity -> group at baseMs
            for (const GroupSample& sample : groups)
                if (sample.timeMs <= baseMs)
                    at[sample.entity] = sample.group;
            for (const auto& entry : at)
                if (entry.second != 0)
                    out.groups.push_back(GroupSample{0, entry.first, entry.second});
            for (const GroupSample& sample : groups)
                if (sample.timeMs > baseMs && sample.timeMs <= endMs)
                    out.groups.push_back(GroupSample{sample.timeMs - baseMs, sample.entity, sample.group});
            // Variables of the kept instances: the values in force at baseMs at
            // 0, then the changes inside the window.
            for (const Instance& instance : out.instances)
                for (const auto& entry : VarsAt(instance.id, baseMs))
                    out.vars.push_back(VarSample{0, instance.id, entry.first, entry.second});
            for (const VarSample& sample : vars)
                if (sample.timeMs > baseMs && sample.timeMs <= endMs && out.HasInstance(sample.id))
                    out.vars.push_back(VarSample{sample.timeMs - baseMs, sample.id, sample.name, sample.value});
        }

        // The entity's recorded group at timeMs (0: none or never sampled).
        std::uint32_t GroupAt(std::uint32_t entity, std::uint32_t timeMs) const
        {
            std::uint32_t group = 0;
            for (const GroupSample& sample : groups)
            {
                if (sample.timeMs > timeMs)
                    break;
                if (sample.entity == entity)
                    group = sample.group;
            }
            return group;
        }

        // Entities with any recorded group (the playback's candidates).
        std::vector<std::uint32_t> GroupedEntities() const
        {
            std::vector<std::uint32_t> out;
            for (const GroupSample& sample : groups)
                if (std::find(out.begin(), out.end(), sample.entity) == out.end())
                    out.push_back(sample.entity);
            return out;
        }
    };

    // ---- the recorder's bookkeeping ------------------------------------

    // The scenes the engine has active now, by scene pointer (the session's
    // own state, independent of the clip, so a fresh recording or a cut
    // window starts with them).
    struct Live
    {
        std::uint32_t nextId = 1;
        std::map<std::uint64_t, Instance> open; // scene pointer -> instance (startMs: first seen)

        // A start: returns the instance (a pointer seen again replaces the old one).
        const Instance& Start(std::uint64_t scene, std::uint32_t hash, std::uint32_t caller, std::uint32_t nowMs,
            const std::uint32_t* tags, std::size_t tagCount)
        {
            Instance instance;
            instance.id = nextId++;
            instance.hash = hash;
            instance.caller = caller;
            instance.kind = KindOfCaller(caller);
            instance.startMs = nowMs;
            instance.tagCount = static_cast<std::uint8_t>(std::min(tagCount, kMaxTags));
            for (std::size_t n = 0; n < instance.tagCount; ++n)
                instance.tags[n] = tags[n];
            Instance& slot = open[scene];
            slot = instance;
            return slot;
        }
        // A start event for a scene already adopted from the pool
        // (same pointer and hash, caller 0): the instance stays, it gets the
        // caller. Returns its id (0: not an adopted one).
        std::uint32_t Claim(std::uint64_t scene, std::uint32_t hash, std::uint32_t caller)
        {
            const auto found = open.find(scene);
            if (found == open.end() || found->second.hash != hash || found->second.caller != 0)
                return 0;
            found->second.caller = caller;
            found->second.kind = KindOfCaller(caller);
            return found->second.id;
        }
        // A stop: the instance id it ends (0: never seen start).
        std::uint32_t Stop(std::uint64_t scene)
        {
            const auto found = open.find(scene);
            if (found == open.end())
                return 0;
            const std::uint32_t id = found->second.id;
            open.erase(found);
            return id;
        }
    };

    // Applies a start to the track (and a stop by id).
    inline void TrackStart(Track& track, const Instance& instance) { track.instances.push_back(instance); }
    inline bool TrackStop(Track& track, std::uint32_t id, std::uint32_t nowMs)
    {
        if (!id)
        {
            ++track.lostStops;
            return false;
        }
        if (Instance* instance = track.Find(id))
        {
            if (instance->stopMs == kOpen)
                instance->stopMs = std::max(nowMs, instance->startMs);
            return true;
        }
        return false;
    }
    // Every scene the engine still has active is in the track (a fresh
    // recording after a clear starts with them at nowMs). Returns how many
    // were added.
    inline std::size_t TrackEnsureOpen(Track& track, const Live& live, std::uint32_t nowMs)
    {
        std::size_t added = 0;
        for (const auto& entry : live.open)
        {
            if (track.Find(entry.second.id))
                continue;
            Instance copy = entry.second;
            copy.startMs = nowMs;
            copy.stopMs = kOpen;
            track.instances.push_back(copy);
            ++added;
        }
        return added;
    }

    // ---- the playback's plan --------------------------------------------

    // The replayable instances active at timeMs (ids), at most `limit`.
    inline std::vector<std::uint32_t> Wanted(const Track& track, std::uint32_t timeMs, std::size_t limit)
    {
        std::vector<std::uint32_t> out;
        for (const Instance& instance : track.instances)
            if (Replayable(instance.kind) && instance.ActiveAt(timeMs) && out.size() < limit)
                out.push_back(instance.id);
        return out;
    }

    // The same with the kinds the playback replays; a script or
    // code scene whose hash the engine already has open (engineOpen) is the
    // engine's (it started it again by itself), and the hashes in `skip`
    // (MixSceneCodeSkip) are never replayed as code scenes. A conversation
    // scene is never filtered by hash (every conversation shares it).
    inline std::vector<std::uint32_t> Wanted(const Track& track, std::uint32_t timeMs, std::size_t limit, std::uint8_t kinds,
        const std::vector<std::uint32_t>& engineOpen, const std::vector<std::uint32_t>& skip)
    {
        std::vector<std::uint32_t> out;
        for (const Instance& instance : track.instances)
        {
            if (!Replayable(instance.kind, kinds) || !instance.ActiveAt(timeMs) || out.size() >= limit)
                continue;
            if (instance.kind != Kind::Conversation &&
                std::find(engineOpen.begin(), engineOpen.end(), instance.hash) != engineOpen.end())
                continue;
            if (instance.kind == Kind::Code && std::find(skip.begin(), skip.end(), instance.hash) != skip.end())
                continue;
            out.push_back(instance.id);
        }
        return out;
    }

    // The same with the scenes the replay
    // owns (`owned`: the hidden live player's state scenes the clip has;
    // started whatever the kinds and never left to the engine, whose start
    // the replay refuses) and the scenes never started here (`never`: the
    // speech ducks, which the engine's own driver starts from the inputs
    // the replay writes).
    inline std::vector<std::uint32_t> Wanted(const Track& track, std::uint32_t timeMs, std::size_t limit, std::uint8_t kinds,
        const std::vector<std::uint32_t>& engineOpen, const std::vector<std::uint32_t>& skip,
        const std::vector<std::uint32_t>& owned, const std::vector<std::uint32_t>& never)
    {
        std::vector<std::uint32_t> out;
        for (const Instance& instance : track.instances)
        {
            if (!instance.ActiveAt(timeMs) || out.size() >= limit)
                continue;
            if (std::find(never.begin(), never.end(), instance.hash) != never.end())
                continue;
            const bool own = instance.kind != Kind::Conversation &&
                std::find(owned.begin(), owned.end(), instance.hash) != owned.end();
            if (!own)
            {
                if (!Replayable(instance.kind, kinds))
                    continue;
                if (instance.kind != Kind::Conversation &&
                    std::find(engineOpen.begin(), engineOpen.end(), instance.hash) != engineOpen.end())
                    continue;
                if (instance.kind == Kind::Code && std::find(skip.begin(), skip.end(), instance.hash) != skip.end())
                    continue;
            }
            out.push_back(instance.id);
        }
        return out;
    }

    // A stop of a scene whose hash at the stop is not the instance's: the
    // reference was stale (the scene had already gone).
    inline bool StaleStop(const Instance& instance, std::uint32_t hashAtStop) { return hashAtStop != instance.hash; }

    // Is a polled scene still the instance's? (reference not cleared and the
    // hash still the instance's.)
    inline bool SceneAlive(std::uint32_t hash, std::uint64_t reference, std::uint32_t instanceHash)
    {
        return reference != 0 && hash == instanceHash;
    }

    // The recorder's variable change test: a new value (beyond 1e-4) or the
    // first sight of the name.
    inline bool VarChanged(bool seen, float last, float now)
    {
        if (!seen)
            return true;
        const float d = now - last;
        return d > 1e-4f || d < -1e-4f;
    }

    struct Diff
    {
        std::vector<std::uint32_t> start;
        std::vector<std::uint32_t> stop;
    };
    inline Diff Plan(const std::vector<std::uint32_t>& wanted, const std::vector<std::uint32_t>& running)
    {
        Diff diff;
        for (std::uint32_t id : wanted)
            if (std::find(running.begin(), running.end(), id) == running.end())
                diff.start.push_back(id);
        for (std::uint32_t id : running)
            if (std::find(wanted.begin(), wanted.end(), id) == wanted.end())
                diff.stop.push_back(id);
        return diff;
    }

    // What the playback does with a puppet's mix group now.
    enum class GroupAction : std::uint8_t
    {
        None,   // already right
        Add,    // put it in the recorded group
        Remove, // the replay put it in a group the recording no longer has
    };
    inline GroupAction DecideGroup(std::uint32_t recorded, std::uint32_t current, bool appliedByReplay)
    {
        if (recorded != 0)
            return current == recorded ? GroupAction::None : GroupAction::Add;
        return appliedByReplay && current != 0 ? GroupAction::Remove : GroupAction::None;
    }

    // Is any of the entity's tags (its mix group) among a conversation
    // scene's participants? (The scene acts on the entities that are not.)
    inline bool Participant(const Instance& instance, std::uint32_t group)
    {
        if (!group)
            return false;
        for (std::size_t n = 0; n < instance.tagCount; ++n)
            if (instance.tags[n] == group)
                return true;
        return false;
    }
}
