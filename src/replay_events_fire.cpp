// replay_events_fire.cpp: fires: the untracked and initial fires recorded; fires started, held, adopted and cleared in playback.
// Split from replay_events.cpp (moved code); shared internals in replay_events_internal.h.

#include "replay_events_internal.h"

namespace replay::events
{
    namespace
    {
    std::uint32_t g_poseV2FireClearAtMs = 0xFFFFFFFFu; // playhead of the rebuild clear (none: keep no record)
    std::uint64_t g_poseV2FireStateRefused = 0;

    // Age and seed writes through the guarded helper (pool, generation of
    // our start, known field, SEH); a refusal is counted and the first ones
    // logged, the fire is left as the engine has it.
    bool WriteOurFire(const OurFire& ours, std::size_t offset, const void* data, std::size_t size)
    {
        std::string error;
        if (replay::fire::WriteFireState(ours.fire, ours.generation, offset, data, size, error))
            return true;
        if (++g_poseV2FireStateRefused <= 8)
            Ctx().log << "POSE_V2_FIRE_STATE_REFUSED,start=" << ours.start << ",offset=0x" << std::hex << offset
                      << std::dec << ",error=" << error << '\n';
        return false;
    }
    bool g_poseV2FireHoldWrites = false;
    std::size_t g_poseV2FireFirstSampleOf = 0; // item count it was built from
    std::string g_poseV2FireCheckTag;
    float g_poseV2FireCheckCenter[3] = {};
    float g_poseV2FireCheckRadius = 0.0f;
    }

    // The engine's active fires the start detour never saw (fire_observer.h
    // ReadActiveFires) within 150 m of the player, not yet in the clip. A
    // ptfx-owned one (fire_initial.h) is its effect's: counted, logged once per
    // slot, left out (the effect's track brings it back, and without its effect
    // the engine ends it the next frame). Any other one is counted and logged
    // only (POSE_V2_FIRE_UNTRACKED).
    void RecordPoseV2UntrackedFires(std::uint32_t timeMs, const Vector3& player)
    {
        static std::vector<replay::fire::EngineFire> live;
        if (!replay::fire::ReadActiveFires(live))
            return;
        for (const replay::fire::EngineFire& fire : live)
        {
            if (fire.tracked || g_poseV2FireActiveSlots.count(fire.slot))
                continue;
            float pos[3] = {};
            std::memcpy(pos, fire.state + replay::fire::kFireWorldPos, sizeof(pos));
            const float dx = pos[0] - player.x;
            const float dy = pos[1] - player.y;
            if (dx * dx + dy * dy > 150.0f * 150.0f)
                continue;
            const std::uint32_t type = replay::fire::FireField<std::uint32_t>(fire.state, replay::fire::kFireType);
            const std::uint32_t flags = replay::fire::FireField<std::uint32_t>(fire.state, replay::fire::kFireFlags);
            const bool ptfx = replay::fire::PtfxOwnedFire(fire.state);
            if (g_poseV2FireUntrackedSeen[fire.slot])
                continue;
            g_poseV2FireUntrackedSeen[fire.slot] = 1;
            ++(ptfx ? g_poseV2FirePtfxOwned : g_poseV2FireUntrackedSkipped);
            if (g_poseV2FireUntrackedLogs < 16)
            {
                ++g_poseV2FireUntrackedLogs;
                Ctx().log << "POSE_V2_FIRE_UNTRACKED,slot=" << fire.slot << ",type=" << type << ",pos=" << pos[0] << ";"
                    << pos[1] << ";" << pos[2] << ",flags=0x" << std::hex << flags << ",owner=0x"
                    << replay::fire::FireField<std::uint64_t>(fire.state, replay::fire::kFireRegOwner) << std::dec
                    << ",verdict=" << (ptfx ? "ptfx_owned" : "left_out") << ",time_ms=" << timeMs << '\n';
            }
        }
    }

    // Every engine fire within 150 m of the player while recording (the
    // front may leave the recorder's 30 m), with its end.
    // The fire state the recording starts with: fires lit and remnants left
    // before record start (within 150 m), as events at time 0.
    void RecordPoseV2InitialFires()
    {
        static std::vector<replay::fire::LiveFire> fires;
        static std::vector<replay::fire::LiveRemnant> remnants;
        replay::fire::SnapshotLive(fires, remnants);
        const Vector3 player = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE, FALSE);
        const auto inRange = [&](const float* p) {
            const float dx = p[0] - player.x;
            const float dy = p[1] - player.y;
            return dx * dx + dy * dy <= 150.0f * 150.0f;
        };
        int litCount = 0;
        int remnantCount = 0;
        for (const replay::fire::LiveFire& live : fires)
        {
            replay::entity::FireStartEvent event;
            std::memcpy(event.position, live.state + 0x30, sizeof(event.position));
            if (!inRange(event.position))
                continue;
            event.timeMs = 0;
            event.slot = live.slot;
            std::memcpy(&event.type, live.state + 0x9C, sizeof(event.type));
            std::memcpy(&event.group, live.state + 0x5C, sizeof(event.group));
            std::memcpy(&event.rng, live.state + 0x1C8, sizeof(event.rng));
            std::memcpy(&event.initialAge, live.state + 0x188, sizeof(event.initialAge));
            std::memcpy(event.desc, live.desc, sizeof(event.desc));
            ZeroFireDescPointers(event);
            std::uint64_t a = 0;
            std::uint64_t b = 0;
            std::memcpy(&a, live.desc + 0x40, sizeof(a));
            std::memcpy(&b, live.desc + 0x48, sizeof(b));
            event.entityA = RecordedIdAtAddress(a);
            event.entityB = RecordedIdAtAddress(b);
            TakeFireWorldEntity(a, event.entityA, event.entityAModel, event.entityAPos);
            TakeFireWorldEntity(b, event.entityB, event.entityBModel, event.entityBPos);
            event.initial = true;
            const std::size_t index = Ctx().clip.fireStarts.Add(event);
            g_poseV2FireActiveSlots[live.slot] = static_cast<std::uint32_t>(index);
            g_poseV2FireOwners[live.fire] = static_cast<std::uint32_t>(index);
            ++litCount;
        }
        for (const replay::fire::LiveRemnant& live : remnants)
        {
            if (!inRange(live.position))
                continue;
            replay::entity::FireStartEvent event;
            std::memcpy(event.position, live.position, sizeof(event.position));
            std::memcpy(&event.type, live.desc + 0x54, sizeof(event.type));
            event.timeMs = 0;
            event.initial = true;
            event.initialRemnant = true;
            std::memcpy(event.desc, live.desc, sizeof(event.desc));
            ZeroFireDescPointers(event);
            const std::size_t index = Ctx().clip.fireStarts.Add(event);
            replay::entity::FireStopEvent stop;
            stop.timeMs = 0;
            stop.start = static_cast<std::uint32_t>(index);
            Ctx().clip.fireStops.Add(stop);
            g_poseV2FireOwners[live.entry] = static_cast<std::uint32_t>(index);
            ++remnantCount;
        }
        // The fires of the engine's list the detour never saw, at time 0
        // (RecordPoseV2UntrackedFires).
        const std::uint64_t rebuiltBefore = g_poseV2FireRebuilt;
        const std::uint64_t ptfxBefore = g_poseV2FirePtfxOwned;
        const std::uint64_t skippedBefore = g_poseV2FireUntrackedSkipped;
        RecordPoseV2UntrackedFires(0, player);
        // The detector. Fires and remnants the engine holds that the start detour
        // never saw (lit before the observer was installed) cannot enter the clip's
        // initial state; the playback then shows whatever the live world has there.
        std::uint32_t engineFires = 0;
        std::uint32_t engineRemnants = 0;
        const bool counted = replay::fire::EngineLiveCounts(engineFires, engineRemnants);
        Ctx().log << "POSE_V2_FIRE_INITIAL,lit=" << litCount << ",remnants=" << remnantCount << ",tracked_fires=" << fires.size()
            << ",tracked_remnants=" << remnants.size() << ",engine_fires=" << (counted ? static_cast<int>(engineFires) : -1)
            << ",engine_remnants=" << (counted ? static_cast<int>(engineRemnants) : -1) << ",untracked_fires="
            << (counted && engineFires > fires.size() ? engineFires - fires.size() : 0u) << ",untracked_remnants="
            << (counted && engineRemnants > remnants.size() ? engineRemnants - remnants.size() : 0u)
            << ",rebuilt=" << (g_poseV2FireRebuilt - rebuiltBefore) << ",ptfx_owned=" << (g_poseV2FirePtfxOwned - ptfxBefore)
            << ",untracked_left_out=" << (g_poseV2FireUntrackedSkipped - skippedBefore) << '\n';
    }

    void SamplePoseV2FireItems(std::uint32_t nowMs)
    {
        if (g_poseV2FireOwners.empty() || nowMs < g_poseV2FireItemSampleAt)
            return;
        g_poseV2FireItemSampleAt = nowMs + 250;
        static std::vector<replay::fire::AreaItem> items;
        std::string error;
        if (!replay::fire::ReadAreaItems(items, error))
            return;
        for (const replay::fire::AreaItem& item : items)
        {
            if (!item.primary)
                continue;
            const auto found = g_poseV2FireOwners.find(item.owner);
            if (found == g_poseV2FireOwners.end())
                continue;
            replay::entity::FireItemSample sample;
            sample.timeMs = nowMs;
            sample.start = found->second;
            sample.outer = item.outer;
            sample.burnt = item.burnt;
            sample.ember = item.ember;
            sample.glow = item.glow;
            sample.width = item.width;
            Ctx().clip.fireItems.Add(sample);
        }
    }

    // The area the recorded fires cover (their positions), for clearing.
    void PoseV2FireArea()
    {
        const auto& starts = Ctx().clip.fireStarts.Events();
        g_poseV2FireRadius = 0.0f;
        if (starts.empty())
            return;
        float sum[3] = {};
        for (const auto& event : starts)
            for (int axis = 0; axis < 3; ++axis)
                sum[axis] += event.position[axis];
        for (int axis = 0; axis < 3; ++axis)
            g_poseV2FireCenter[axis] = sum[axis] / static_cast<float>(starts.size());
        float farthest = 0.0f;
        for (const auto& event : starts)
        {
            const float dx = event.position[0] - g_poseV2FireCenter[0];
            const float dy = event.position[1] - g_poseV2FireCenter[1];
            const float dz = event.position[2] - g_poseV2FireCenter[2];
            farthest = (std::max)(farthest, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        g_poseV2FireRadius = farthest + 20.0f;
    }
    namespace
    {

    // The entities whose engine fire record the rebuild keeps at playhead
    // timeMs (FireRecordKeptAt over the recorded fires on each). Puppets by id;
    // world objects by the recorded model and position (the same lookup the fire
    // start uses). A puppet without a fire start on it but with a FREC track
    // keeps its record when the recorded state has one with the fire out (flag
    // 0x04).
    void PoseV2FireRecordsKept(std::uint32_t timeMs, std::vector<const void*>& kept)
    {
        kept.clear();
        const auto& starts = Ctx().clip.fireStarts.Events();
        std::vector<std::uint32_t> stopAt(starts.size(), 0xFFFFFFFFu);
        for (const auto& stop : Ctx().clip.fireStops.Events())
            if (stop.start < stopAt.size() && stop.timeMs < stopAt[stop.start])
                stopAt[stop.start] = stop.timeMs;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> spans;
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (!puppet.clone || !replay::entities::Exists(puppet.clone))
                continue;
            spans.clear();
            for (std::size_t index = 0; index < starts.size(); ++index)
                if (starts[index].entityA == puppet.packet.id || starts[index].entityB == puppet.packet.id)
                    spans.push_back({starts[index].timeMs, stopAt[index]});
            bool keep = false;
            if (!spans.empty())
                keep = replay::FireRecordKeptAt(spans, timeMs);
            else if (replay::FireCharKept(Ctx().clip.fireChars, puppet.packet.id.Packed()))
            {
                for (const replay::FireCharTrack& track : Ctx().clip.fireChars)
                    if (track.id == puppet.packet.id.Packed())
                        if (const replay::FireCharState* state = replay::FireCharAt(track, timeMs))
                            keep = state->present && (state->flags & 0x04) != 0;
            }
            if (keep)
                kept.push_back(replay::entities::AddressOf(puppet.clone));
        }
        // World objects: grouped by the object the lookup finds.
        std::vector<std::pair<Entity, std::vector<std::pair<std::uint32_t, std::uint32_t>>>> world;
        for (std::size_t index = 0; index < starts.size(); ++index)
        {
            const replay::entity::FireStartEvent& event = starts[index];
            for (int side = 0; side < 2; ++side)
            {
                const bool isA = side == 0;
                if ((isA ? event.entityA : event.entityB).Valid())
                    continue;
                const std::uint32_t model = isA ? event.entityAModel : event.entityBModel;
                if (!model)
                    continue;
                const Entity object = FindEntityByModelNear(model, isA ? event.entityAPos : event.entityBPos, 3.0f);
                if (!object)
                    continue;
                auto found = std::find_if(world.begin(), world.end(), [&](const auto& entry) { return entry.first == object; });
                if (found == world.end())
                {
                    world.push_back({object, {}});
                    found = world.end() - 1;
                }
                found->second.push_back({event.timeMs, stopAt[index]});
            }
        }
        for (const auto& [object, objectSpans] : world)
            if (replay::FireRecordKeptAt(objectSpans, timeMs))
                kept.push_back(replay::entities::AddressOf(object));
    }
    }

    // Ends our fires and every fire and remnant in the fire area
    // (STOP_FIRE_IN_RANGE).
    void ClearPoseV2Fires(bool instant)
    {
        g_poseV2FireEndedQueue.clear();
        for (OurFire& ours : g_poseV2FiresOurs)
        {
            if (!ours.alive)
                continue;
            std::string error;
            replay::fire::StopFire(ours.fire, instant, 0, error);
            ours.alive = false;
        }
        if (g_poseV2FireRadius > 0.0f)
        {
            FIRE::STOP_FIRE_IN_RANGE(g_poseV2FireCenter[0], g_poseV2FireCenter[1], g_poseV2FireCenter[2], g_poseV2FireRadius);
            std::string error;
            g_poseV2FireRemnantsCleared += static_cast<std::uint64_t>(
                replay::fire::ClearRemnants(g_poseV2FireCenter, g_poseV2FireRadius, error));
            // The burnt ground the recording (or an earlier play) left. The same
            // engine clear takes every entity fire record in the box, the puppets' burnt
            // look with them; the records of puppets with a FREC track are shielded
            // through it. Which records survive depends on the playhead only
            // (FireRecordKeptAt): the entity burnt by then and no fire of it still
            // burning (that one is started again by the rebuild, with a fresh record).
            // Puppets and the world objects the recorded fires burnt (props, a deck a
            // molotov burns on) alike. Off for the start and end of a playback
            // (clearAtMs UINT32_MAX).
            std::vector<const void*> kept;
            if (g_poseV2FireClearAtMs != 0xFFFFFFFFu)
                PoseV2FireRecordsKept(g_poseV2FireClearAtMs, kept);
            std::vector<replay::burn::CharShield> shields;
            const int shielded = kept.empty() ? 0 : replay::burn::ShieldChars(kept, shields);
            std::string burntError;
            if (!replay::fire::ClearBurntGround(g_poseV2FireCenter, g_poseV2FireRadius, burntError))
                g_poseV2FireLastError = "burnt_" + burntError;
            replay::burn::UnshieldChars(shields);
            g_poseV2FireCharShielded += static_cast<std::uint64_t>(shielded);
            // FREC writes the recorded state on its next pass.
            for (replay::playback::Puppet& puppet : Ctx().puppets.list)
                puppet.fireCharCheckAt = 0;
        }
    }
    namespace
    {

    replay::fire::StartRetry& PoseV2FireRetry(std::uint32_t index)
    {
        if (g_poseV2FireRetry.size() < Ctx().clip.fireStarts.Events().size())
            g_poseV2FireRetry.resize(Ctx().clip.fireStarts.Events().size());
        return g_poseV2FireRetry[index];
    }

    // A start that did not happen waits for a bounded retry (fire_observer.h
    // StartRetry), with a capped evidence line.
    void FailPoseV2FireStart(std::uint32_t index, std::uint32_t timeMs, const char* reason)
    {
        replay::fire::StartRetry& retry = PoseV2FireRetry(index);
        const bool gaveUp = replay::fire::NoteStartFailed(retry, GetTickCount64());
        if (gaveUp)
            ++g_poseV2FireGaveUp;
        else if (std::find(g_poseV2FirePending.begin(), g_poseV2FirePending.end(), index) == g_poseV2FirePending.end())
            g_poseV2FirePending.push_back(index);
        if (g_poseV2FireSkipLogs < 24)
        {
            ++g_poseV2FireSkipLogs;
            const replay::entity::FireStartEvent& event = Ctx().clip.fireStarts.Events()[index];
            Ctx().log << "POSE_V2_PLAYBACK_FIRE_SKIP,index=" << index << ",type=" << event.type << ",reason=" << reason
                << ",entity_a=" << (event.entityA.Valid() ? replay::entity::IdText(event.entityA) : std::string("none"))
                << ",entity_b=" << (event.entityB.Valid() ? replay::entity::IdText(event.entityB) : std::string("none"))
                << ",failures=" << retry.failures << ",gave_up=" << (retry.gaveUp ? 1 : 0) << ",event_ms=" << event.timeMs
                << ",playhead_ms=" << timeMs << '\n';
        }
    }

    // Returns whether the fire started. Never calls the engine with an entity
    // it cannot resolve (a recorded entity whose puppet is missing or not ready
    // waits for a bounded retry instead of going in as null), and never with the
    // recorded parent fire pointer.
    bool StartPoseV2Fire(std::uint32_t index, std::uint32_t timeMs)
    {
        const replay::entity::FireStartEvent& event = Ctx().clip.fireStarts.Events()[index];
        if (PoseV2FireRetry(index).gaveUp)
            return false;
        // A start with a live fire of ours is not started twice, whichever list
        // (retry, ended queue, reconcile) asks; not a failure (no retry is queued
        // for it).
        if (replay::fire::LiveStart(g_poseV2FiresOurs, index))
        {
            ++g_poseV2FireDuplicates;
            return false;
        }
        alignas(16) std::uint8_t desc[0x120] = {};
        std::memcpy(desc, event.desc, sizeof(desc));
        // Entities become their puppets (none when not recorded: the engine start
        // takes a null +0x40, it tests it before every use); no culprit (no
        // crime, honour or wanted level); no parent
        // line-of-sight test.
        bool unresolved = false;
        for (const auto& [offset, id] : {std::make_pair(replay::fire::kDescEntityA, event.entityA),
                 std::make_pair(replay::fire::kDescEntityB, event.entityB)})
        {
            std::uint64_t pointer = 0;
            if (id.Valid())
            {
                if (const replay::playback::Puppet* puppet = Ctx().puppets.Find(id))
                    if (puppet->clone && replay::entities::Exists(puppet->clone))
                        pointer = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet->clone));
                if (!pointer)
                    unresolved = true;
            }
            else
            {
                // The same world object by model and position; not found, the engine gets
                // null.
                const bool isA = offset == replay::fire::kDescEntityA;
                const std::uint32_t model = isA ? event.entityAModel : event.entityBModel;
                if (model)
                {
                    if (const Entity world = FindEntityByModelNear(model, isA ? event.entityAPos : event.entityBPos, 3.0f))
                    {
                        pointer = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(world));
                        ++g_poseV2FireWorldFound;
                    }
                    else
                        ++g_poseV2FireWorldMissing;
                }
            }
            std::memcpy(desc + offset, &pointer, sizeof(pointer));
        }
        if (unresolved)
        {
            ++g_poseV2FireEntityWaits;
            FailPoseV2FireStart(index, timeMs, "entity_not_ready");
            return false;
        }
        const std::uint64_t none = 0;
        std::memcpy(desc + replay::fire::kDescCulprit, &none, sizeof(none));
        // Older clips can still carry the recorded parent (+0xF0).
        if (replay::fire::ClearDescParent(desc))
            ++g_poseV2FireParentCleared;
        desc[0x115] &= static_cast<std::uint8_t>(~1u);
        std::string error;
        void* fire = replay::fire::StartFire(desc, error);
        if (!fire)
        {
            ++g_poseV2FirePlayRefused;
            g_poseV2FireLastError = error;
            FailPoseV2FireStart(index, timeMs, error.c_str());
            return false;
        }
        replay::fire::NoteStartOk(PoseV2FireRetry(index));
        ++g_poseV2FirePlayed;
        g_poseV2FiresOurs.push_back({index, fire, true});
        g_poseV2FiresOurs.back().generation = replay::fire::FireGeneration(fire);
        WriteOurFire(g_poseV2FiresOurs.back(), replay::fire::kFireRngOffset, &event.rng, sizeof(event.rng));
        if (g_poseV2FirePlayed <= 12)
            Ctx().log << "POSE_V2_PLAYBACK_FIRE,index=" << index << ",slot=" << replay::fire::SlotOf(fire) << ",type=" << event.type
                << ",event_ms=" << event.timeMs << ",playhead_ms=" << timeMs << '\n';
        return true;
    }

    // A recorded fire started as the state at the playhead (after a jump, or
    // a retry): its ground state is restored once the engine made it, its age
    // written, and it is ended right after when it had ended by then. Returns
    // whether it started.
    bool StartPoseV2FireState(std::uint32_t index, std::uint32_t timeMs, bool alive, std::uint8_t endInstant, std::uint8_t endArg3)
    {
        if (!StartPoseV2Fire(index, timeMs))
            return false;
        OurFire& ours = g_poseV2FiresOurs.back();
        ours.restore = true;
        ours.endAfter = !alive;
        ours.endInstant = endInstant;
        ours.endArg3 = endArg3;
        // The fire's age at the playhead (+0x188, seconds): its update
        // derives the flame strength from it, so it does not restart small.
        const auto& origin = Ctx().clip.fireStarts.Events()[index];
        const float age = origin.initialAge + static_cast<float>(timeMs > origin.timeMs ? timeMs - origin.timeMs : 0) / 1000.0f;
        WriteOurFire(ours, replay::fire::kFireAgeOffset, &age, sizeof(age));
        ours.restoreMs = timeMs;
        ours.restoreSince = GetTickCount64();
        return true;
    }

    // The recorded stop of a start (first one), for the state at a playhead:
    // alive when not stopped by then.
    void PoseV2FireStopOf(std::uint32_t index, std::uint32_t timeMs, bool& alive, std::uint8_t& instant, std::uint8_t& arg3)
    {
        alive = true;
        instant = 0;
        arg3 = 0;
        for (const auto& stop : Ctx().clip.fireStops.Events())
            if (stop.start == index)
            {
                alive = timeMs < stop.timeMs;
                instant = stop.instant;
                arg3 = stop.arg3;
                return;
            }
    }
    }

    // The survivors of a clear, read on a later tick (in the tick of the stop
    // they are not settled yet).
    void SchedulePoseV2FireCheck(const char* tag, const float* center, float radius)
    {
        if (!g_poseV2FireReady || radius <= 0.0f)
            return;
        g_poseV2FireCheckAt = GetTickCount64() + 300;
        g_poseV2FireCheckTag = tag;
        std::memcpy(g_poseV2FireCheckCenter, center, sizeof(g_poseV2FireCheckCenter));
        g_poseV2FireCheckRadius = radius;
    }
    void CheckPoseV2FireClear()
    {
        if (!g_poseV2FireCheckAt || GetTickCount64() < g_poseV2FireCheckAt)
            return;
        g_poseV2FireCheckAt = 0;
        static std::vector<replay::fire::EngineFire> live;
        const float* c = g_poseV2FireCheckCenter;
        const int native = FIRE::GET_NUMBER_OF_FIRES_IN_RANGE(c[0], c[1], c[2], g_poseV2FireCheckRadius);
        if (!replay::fire::ReadActiveFires(live))
        {
            Ctx().log << "POSE_V2_PLAYBACK_FIRE_CLEAR_CHECK,tag=" << g_poseV2FireCheckTag << ",error=unreadable,native_count=" << native << '\n';
            return;
        }
        int counts[5] = {};
        int inRange = 0;
        int lines = 0;
        for (const replay::fire::EngineFire& fire : live)
        {
            if (replay::fire::FireDistance(replay::fire::FirePos(fire.state).v, c) >= g_poseV2FireCheckRadius)
                continue;
            ++inRange;
            const OurFire* our = nullptr;
            for (const OurFire& ours : g_poseV2FiresOurs)
                if (ours.alive && reinterpret_cast<std::uint64_t>(ours.fire) == fire.fire)
                    our = &ours;
            const replay::fire::ClearVerdict verdict =
                replay::fire::ClearVerdictFor(fire.state, false, our != nullptr);
            ++counts[static_cast<int>(verdict)];
            if (lines++ < 16)
            {
                const replay::fire::FirePos at(fire.state);
                const float* p = at.v;
                Ctx().log << "POSE_V2_PLAYBACK_FIRE_SURVIVOR,tag=" << g_poseV2FireCheckTag << ",slot=" << fire.slot << ",type="
                    << replay::fire::FireField<std::uint32_t>(fire.state, replay::fire::kFireType) << ",pos=" << p[0] << ";" << p[1]
                    << ";" << p[2] << ",flags=0x" << std::hex << replay::fire::FireField<std::uint32_t>(fire.state, replay::fire::kFireFlags)
                    << std::dec << ",class=" << replay::fire::ClearVerdictName(verdict) << ",start="
                    << (our ? static_cast<int>(our->start) : -1) << ",tracked=" << (fire.tracked ? 1 : 0)
                    << ",persistent=" << (replay::fire::PersistentFire(fire.state) ? 1 : 0) << '\n';
            }
        }
        Ctx().log << "POSE_V2_PLAYBACK_FIRE_CLEAR_CHECK,tag=" << g_poseV2FireCheckTag << ",center=" << c[0] << ";" << c[1] << ";" << c[2]
            << ",radius=" << g_poseV2FireCheckRadius << ",live_in_range=" << inRange
            << ",ours=" << counts[static_cast<int>(replay::fire::ClearVerdict::Ours)]
            << ",adopted=" << counts[static_cast<int>(replay::fire::ClearVerdict::Protected)]
            << ",ptfx_owned=" << counts[static_cast<int>(replay::fire::ClearVerdict::PtfxOwned)]
            << ",stop_immune=" << counts[static_cast<int>(replay::fire::ClearVerdict::StopImmune)]
            << ",other=" << counts[static_cast<int>(replay::fire::ClearVerdict::Stop)]
            << ",native_count=" << native << '\n';
    }
    namespace
    {

    // The starts waiting for a retry, tried when their delay is over, while
    // they are due at the playhead and not already ours.
    void RetryPoseV2FireStarts(std::uint32_t timeMs)
    {
        if (g_poseV2FirePending.empty())
            return;
        const std::uint64_t now = GetTickCount64();
        static std::vector<std::uint32_t> keep;
        keep.clear();
        const std::vector<std::uint32_t> pending = g_poseV2FirePending;
        g_poseV2FirePending.clear();
        for (const std::uint32_t index : pending)
        {
            replay::fire::StartRetry& retry = PoseV2FireRetry(index);
            if (retry.gaveUp)
                continue;
            const bool ours = std::any_of(g_poseV2FiresOurs.begin(), g_poseV2FiresOurs.end(),
                [&](const OurFire& fire) { return fire.start == index; });
            if (ours)
                continue;
            if (Ctx().clip.fireStarts.Events()[index].timeMs > timeMs || !replay::fire::StartAllowed(retry, now))
            {
                keep.push_back(index);
                continue;
            }
            bool alive = true;
            std::uint8_t instant = 0;
            std::uint8_t arg3 = 0;
            PoseV2FireStopOf(index, timeMs, alive, instant, arg3);
            ++g_poseV2FireRetries;
            StartPoseV2FireState(index, timeMs, alive, instant, arg3); // a failure queues it again (bounded)
        }
        for (const std::uint32_t index : keep)
            if (std::find(g_poseV2FirePending.begin(), g_poseV2FirePending.end(), index) == g_poseV2FirePending.end())
                g_poseV2FirePending.push_back(index);
    }

    // An ended start is started again (and shut down right after) only for
    // what it leaves: a recorded ground sample by the playhead, or a map fire
    // (type 12), whose shutdown leaves the smouldering remnant. An ended entity
    // fire without a sample leaves nothing a restart brings back (the charred
    // bit has its own track) and would only take a slot.
    constexpr std::uint32_t kPoseV2FireTypeMap = 12;
    bool PoseV2FireEndedNeedsRestart(std::uint32_t index, std::uint32_t timeMs)
    {
        if (static_cast<std::uint32_t>(Ctx().clip.fireStarts.Events()[index].type) == kPoseV2FireTypeMap)
            return true;
        const std::size_t starts = Ctx().clip.fireStarts.Events().size();
        if (g_poseV2FireFirstSampleMs.size() != starts || g_poseV2FireFirstSampleOf != Ctx().clip.fireItems.Events().size())
        {
            g_poseV2FireFirstSampleMs.assign(starts, 0xFFFFFFFFu);
            for (const auto& sample : Ctx().clip.fireItems.Events())
                if (sample.start < starts)
                    g_poseV2FireFirstSampleMs[sample.start] = (std::min)(g_poseV2FireFirstSampleMs[sample.start], sample.timeMs);
            g_poseV2FireFirstSampleOf = Ctx().clip.fireItems.Events().size();
        }
        return g_poseV2FireFirstSampleMs[index] <= timeMs;
    }

    // The engine has 128 fire slots (fire_observer.h); a few stay free for
    // fires the replay does not own.
    constexpr std::uint32_t kPoseV2FireSlotBudget = replay::fire::kFireCount - 8;
    constexpr int kPoseV2FireEndedPerFrame = 16;
    std::uint32_t PoseV2FiresLive()
    {
        return static_cast<std::uint32_t>(std::count_if(g_poseV2FiresOurs.begin(), g_poseV2FiresOurs.end(),
            [](const OurFire& ours) { return ours.alive; }));
    }

    // The ended starts waiting for a slot. Each one is started as the state at
    // the playhead (ground state restored, then shut down by
    // RestorePoseV2FireItems, which frees its slot), a few per frame and only
    // while the live fires leave room, after the burning ones.
    void PumpPoseV2FireEndedQueue(std::uint32_t timeMs)
    {
        if (g_poseV2FireEndedQueue.empty())
            return;
        const std::uint64_t now = GetTickCount64();
        std::uint32_t live = PoseV2FiresLive();
        int started = 0;
        std::size_t used = 0;
        static std::vector<std::uint32_t> later;
        later.clear();
        while (used < g_poseV2FireEndedQueue.size() && started < kPoseV2FireEndedPerFrame && live < kPoseV2FireSlotBudget)
        {
            const std::uint32_t index = g_poseV2FireEndedQueue[used++];
            if (Ctx().clip.fireStarts.Events()[index].timeMs > timeMs || PoseV2FireRetry(index).gaveUp)
                continue;
            if (!replay::fire::StartAllowed(PoseV2FireRetry(index), now))
            {
                later.push_back(index);
                continue;
            }
            bool alive = true;
            std::uint8_t instant = 0;
            std::uint8_t arg3 = 0;
            PoseV2FireStopOf(index, timeMs, alive, instant, arg3);
            // A failure queues it for RetryPoseV2FireStarts (bounded).
            if (StartPoseV2FireState(index, timeMs, alive, instant, arg3))
            {
                ++started;
                ++live;
            }
        }
        g_poseV2FireEndedQueue.erase(g_poseV2FireEndedQueue.begin(), g_poseV2FireEndedQueue.begin() + used);
        g_poseV2FireEndedQueue.insert(g_poseV2FireEndedQueue.end(), later.begin(), later.end());
        if (live >= kPoseV2FireSlotBudget && !g_poseV2FireEndedQueue.empty() && g_poseV2FireSlotLogs < 16)
        {
            ++g_poseV2FireSlotLogs;
            Ctx().log << "POSE_V2_PLAYBACK_FIRE_SLOTS,live=" << live << ",budget=" << kPoseV2FireSlotBudget
                << ",ended_waiting=" << g_poseV2FireEndedQueue.size() << ",playhead_ms=" << timeMs << '\n';
        }
    }

    // After a jump: every fire started by the playhead is started again; its
    // area item gets the ground state recorded at the playhead, and the ones
    // that had ended are shut down right after (their char stays). The burning
    // ones first; the ended ones through the paced queue
    // (PumpPoseV2FireEndedQueue), and only those that leave something.
    void ReconcilePoseV2Fires(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.fires_reconcile");
        std::vector<std::uint32_t> stopAt(Ctx().clip.fireStarts.Events().size(), 0xFFFFFFFFu);
        std::vector<std::uint8_t> stopInstant(Ctx().clip.fireStarts.Events().size(), 0);
        std::vector<std::uint8_t> stopArg3(Ctx().clip.fireStarts.Events().size(), 0);
        for (const auto& stop : Ctx().clip.fireStops.Events())
            if (stop.start < stopAt.size())
            {
                stopAt[stop.start] = stop.timeMs;
                stopInstant[stop.start] = stop.instant;
                stopArg3[stop.start] = stop.arg3;
            }
        // Nothing to do when the same fires are started and alive or ended as
        // wanted (a scrub crosses many frames with the same set). Starts that gave
        // up (fire_observer.h StartRetry) are not wanted any more in this playback.
        std::vector<std::pair<std::uint32_t, bool>> wanted;
        for (std::uint32_t index = 0; index < Ctx().clip.fireStarts.Events().size(); ++index)
        {
            if (Ctx().clip.fireStarts.Events()[index].timeMs > timeMs || PoseV2FireRetry(index).gaveUp)
                continue;
            const bool alive = timeMs < stopAt[index];
            if (!alive && !PoseV2FireEndedNeedsRestart(index, timeMs))
                continue;
            wanted.push_back({index, alive});
        }
        // What the replay asked for (not whether the engine already ended a fire
        // by itself: that is not a reason to rebuild everything). The ended starts
        // still in the queue count as had. An ended fire counts only when it is
        // wanted (one that leaves ground behind): otherwise a fire stopped in normal
        // play would make the sets differ and every scrub would rebuild all the
        // fires, clearing the burnt ground.
        const auto wantedEnded = [&](std::uint32_t index) {
            return std::find(wanted.begin(), wanted.end(), std::make_pair(index, false)) != wanted.end();
        };
        std::vector<std::pair<std::uint32_t, bool>> have;
        for (const OurFire& ours : g_poseV2FiresOurs)
        {
            const bool alive = !ours.endAfter && !ours.stopped;
            if (alive || wantedEnded(ours.start))
                have.push_back({ours.start, alive});
        }
        for (const std::uint32_t index : g_poseV2FireEndedQueue)
            if (wantedEnded(index))
                have.push_back({index, false});
        std::sort(have.begin(), have.end());
        if (have == wanted)
            return;
        // Only some wanted starts are missing (refused, waiting for their entity,
        // or newly due): start those that may be tried now, leave the others
        // burning. Otherwise a refused start never matches and every scrub tick
        // ends and restarts all the fires.
        static std::vector<std::uint32_t> missing;
        if (replay::fire::OnlyMissing(have, wanted, missing))
        {
            const std::uint64_t now = GetTickCount64();
            int started = 0;
            for (const std::uint32_t index : missing)
            {
                if (timeMs >= stopAt[index])
                {
                    // Ended by the playhead: paced by the queue (once; a failed start waits in
                    // the retry list). QueueStartOnce: one list per start.
                    if (replay::fire::QueueStartOnce(g_poseV2FireEndedQueue, g_poseV2FirePending, index))
                        ++g_poseV2FireEndedQueued;
                    continue;
                }
                if (!replay::fire::StartAllowed(PoseV2FireRetry(index), now))
                {
                    replay::fire::QueueStartOnce(g_poseV2FirePending, g_poseV2FireEndedQueue, index);
                    continue;
                }
                if (PoseV2FireRetry(index).failures)
                    ++g_poseV2FireRetries;
                started += StartPoseV2FireState(index, timeMs, timeMs < stopAt[index], stopInstant[index], stopArg3[index]) ? 1 : 0;
            }
            ++g_poseV2FireIncremental;
            if (started && g_poseV2FireReconcileLogs < 64)
            {
                ++g_poseV2FireReconcileLogs;
                Ctx().log << "POSE_V2_PLAYBACK_FIRE_RECONCILE,mode=missing,started=" << started << ",missing=" << missing.size()
                    << ",playhead_ms=" << timeMs << '\n';
            }
            PumpPoseV2FireEndedQueue(timeMs);
            return;
        }
        g_poseV2FireClearAtMs = timeMs;
        ClearPoseV2Fires(true);
        g_poseV2FireClearAtMs = 0xFFFFFFFFu;
        g_poseV2FiresOurs.clear();
        // The rebuild owns every start: a start waiting for a retry that this
        // playhead no longer wants is dropped, and one still wanted stays in the
        // retry list only (not also in the ended queue, where both would start it).
        g_poseV2FirePending.erase(std::remove_if(g_poseV2FirePending.begin(), g_poseV2FirePending.end(),
            [&](std::uint32_t index) {
                return std::none_of(wanted.begin(), wanted.end(), [&](const auto& want) { return want.first == index; });
            }), g_poseV2FirePending.end());
        int started = 0;
        int queued = 0;
        for (const auto& [index, alive] : wanted)
        {
            if (!alive)
            {
                // After every burning one, paced by the slots.
                if (replay::fire::QueueStartOnce(g_poseV2FireEndedQueue, g_poseV2FirePending, index))
                {
                    ++g_poseV2FireEndedQueued;
                    ++queued;
                }
                continue;
            }
            if (!replay::fire::StartAllowed(PoseV2FireRetry(index), GetTickCount64()))
            {
                // Waiting for its retry delay: tried by RetryPoseV2FireStarts.
                replay::fire::QueueStartOnce(g_poseV2FirePending, g_poseV2FireEndedQueue, index);
                continue;
            }
            started += StartPoseV2FireState(index, timeMs, alive, stopInstant[index], stopArg3[index]) ? 1 : 0;
        }
        ++g_poseV2FireReconciles;
        if (g_poseV2FireReconcileLogs < 64)
        {
            ++g_poseV2FireReconcileLogs;
            Ctx().log << "POSE_V2_PLAYBACK_FIRE_RECONCILE,mode=rebuild,started=" << started << ",wanted=" << wanted.size()
                << ",alive=" << std::count_if(wanted.begin(), wanted.end(), [](const auto& entry) { return entry.second; })
                << ",ended_queued=" << queued << ",playhead_ms=" << timeMs << '\n';
        }
        PumpPoseV2FireEndedQueue(timeMs);
    }

    // Writes the recorded ground state on the items of fires restarted by a
    // jump, once the engine has made them (the frame after the start).
    void RestorePoseV2FireItems()
    {
        const ULONGLONG now = GetTickCount64();
        for (OurFire& ours : g_poseV2FiresOurs)
        {
            if (!ours.restore || !ours.alive)
                continue;
            const replay::entity::FireItemSample* sample = nullptr;
            for (const auto& candidate : Ctx().clip.fireItems.Events())
            {
                if (candidate.timeMs > ours.restoreMs)
                    break;
                if (candidate.start == ours.start)
                    sample = &candidate;
            }
            bool done = sample == nullptr;
            if (sample)
            {
                std::string error;
                std::vector<replay::fire::AreaItemWrite> one(1);
                one[0].owner = reinterpret_cast<std::uint64_t>(ours.fire);
                one[0].outer = sample->outer;
                one[0].burnt = sample->burnt;
                one[0].width = sample->width;
                one[0].ember = ours.endAfter ? sample->ember : -1.0f;
                one[0].glow = ours.endAfter ? sample->glow : -1.0f;
                done = replay::fire::WriteAreaItems(one, error) && one[0].done;
                if (done)
                    ++g_poseV2FireRestored;
                else if (now - ours.restoreSince > 1000)
                {
                    ++g_poseV2FireRestoreMissed;
                    done = true;
                }
            }
            if (!done)
                continue;
            ours.restore = false;
            if (ours.endAfter)
            {
                // The recorded way it ended (not instant: an instant stop
                // drops the burnt disc with the fire).
                std::string error;
                replay::fire::StopFire(ours.fire, ours.endInstant != 0, ours.endArg3, error);
                ours.alive = false;
            }
        }
    }

    // Paused or scrubbing: the world keeps running but the playhead does
    // not, so our live fires are held at the playhead every frame (age and
    // ground state), and any the engine ended meanwhile brings a reconcile.
    void HoldPoseV2Fires(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.fires_hold");
        bool lost = false;
        static std::vector<replay::fire::AreaItemWrite> writes;
        writes.clear();
        // While the playhead moves (a scrub), the live fires take their age at the
        // playhead (+0x188, the flame strength follows it). Without it a fire
        // restarted by a reconcile keeps aging with the wall clock while the
        // playhead goes back, and can burn out by life at a playhead where it still
        // burns. A still playhead writes nothing.
        const bool moved = timeMs != g_poseV2FireHoldLastMs;
        g_poseV2FireHoldLastMs = timeMs;
        if (moved && !g_poseV2FireHoldWrites)
            for (OurFire& ours : g_poseV2FiresOurs)
            {
                if (!ours.alive || ours.endAfter)
                    continue;
                const auto& event = Ctx().clip.fireStarts.Events()[ours.start];
                const float age = event.initialAge + (timeMs > event.timeMs ? static_cast<float>(timeMs - event.timeMs) / 1000.0f : 0.0f);
                WriteOurFire(ours, replay::fire::kFireAgeOffset, &age, sizeof(age));
            }
        for (OurFire& ours : g_poseV2FiresOurs)
        {
            if (ours.endAfter)
                continue;
            if (!ours.alive)
            {
                bool shouldBurn = true;
                for (const auto& stop : Ctx().clip.fireStops.Events())
                    if (stop.start == ours.start && stop.timeMs <= timeMs)
                        shouldBurn = false;
                if (shouldBurn)
                    lost = true;
                continue;
            }
            // No per-frame state writes while paused; the engine keeps drawing the
            // restored fires.
            if (!g_poseV2FireHoldWrites)
                continue;
            const auto& event = Ctx().clip.fireStarts.Events()[ours.start];
            const float age = event.initialAge + (timeMs > event.timeMs ? static_cast<float>(timeMs - event.timeMs) / 1000.0f : 0.0f);
            WriteOurFire(ours, replay::fire::kFireAgeOffset, &age, sizeof(age));
            const replay::entity::FireItemSample* sample = nullptr;
            for (const auto& candidate : Ctx().clip.fireItems.Events())
            {
                if (candidate.timeMs > timeMs)
                    break;
                if (candidate.start == ours.start)
                    sample = &candidate;
            }
            if (sample)
            {
                replay::fire::AreaItemWrite write;
                write.owner = reinterpret_cast<std::uint64_t>(ours.fire);
                write.outer = sample->outer;
                write.burnt = sample->burnt;
                write.width = sample->width;
                writes.push_back(write);
            }
        }
        if (!writes.empty())
        {
            std::string error;
            replay::fire::WriteAreaItems(writes, error);
        }
        if (lost)
        {
            // Only the fires the engine ended before their recorded stop
            // come back (with their state at the playhead).
            std::vector<std::uint32_t> restart;
            for (OurFire& ours : g_poseV2FiresOurs)
            {
                if (ours.alive || ours.endAfter || ours.stopped)
                    continue;
                bool shouldBurn = true;
                for (const auto& stop : Ctx().clip.fireStops.Events())
                    if (stop.start == ours.start && stop.timeMs <= timeMs)
                        shouldBurn = false;
                if (shouldBurn)
                {
                    restart.push_back(ours.start);
                    ours.stopped = true; // replaced below
                }
            }
            g_poseV2FiresOurs.erase(std::remove_if(g_poseV2FiresOurs.begin(), g_poseV2FiresOurs.end(),
                [&](const OurFire& ours) { return !ours.alive && ours.stopped && !ours.endAfter &&
                    std::find(restart.begin(), restart.end(), ours.start) != restart.end(); }), g_poseV2FiresOurs.end());
            // Through the bounded retry (a refused restart waits).
            const std::uint64_t now = GetTickCount64();
            for (const std::uint32_t index : restart)
            {
                if (!replay::fire::StartAllowed(PoseV2FireRetry(index), now))
                {
                    if (!PoseV2FireRetry(index).gaveUp &&
                        std::find(g_poseV2FirePending.begin(), g_poseV2FirePending.end(), index) == g_poseV2FirePending.end())
                        g_poseV2FirePending.push_back(index);
                    continue;
                }
                StartPoseV2FireState(index, timeMs, true, 0, 0);
            }
        }
    }

    // Evidence: the puppets our live fires were started on (the recorded
    // entityA/B, now peds, vehicles, weapons or props) and whether the engine
    // reports them on fire.
    void LogPoseV2FireEntities(std::uint32_t timeMs)
    {
        if (g_poseV2FireEntityLogs >= 48 || timeMs < g_poseV2FireEntityNextMs)
            return;
        g_poseV2FireEntityNextMs = timeMs + 1000;
        for (const OurFire& ours : g_poseV2FiresOurs)
        {
            if (!ours.alive || g_poseV2FireEntityLogs >= 48)
                continue;
            const replay::entity::FireStartEvent& event = Ctx().clip.fireStarts.Events()[ours.start];
            for (const replay::entity::ReplayId id : {event.entityA, event.entityB})
            {
                if (!id.Valid() || g_poseV2FireEntityLogs >= 48)
                    continue;
                const replay::playback::Puppet* puppet = Ctx().puppets.Find(id);
                const bool exists = puppet && puppet->clone && replay::entities::Exists(puppet->clone);
                ++g_poseV2FireEntityLogs;
                Ctx().log << "POSE_V2_PLAYBACK_FIRE_ENTITY,index=" << ours.start << ",id=" << replay::entity::IdText(id)
                    << ",kind=" << (puppet ? replay::entity::KindText(puppet->packet.kind) : "none")
                    << ",puppet=" << (exists ? 1 : 0)
                    << ",on_fire=" << (exists && FIRE::IS_ENTITY_ON_FIRE(puppet->clone) ? 1 : 0)
                    << ",playhead_ms=" << timeMs << '\n';
            }
        }
    }
    }

    void PlayPoseV2Fires(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.fires");
        if (!g_poseV2FirePlaying)
            return;
        // The engine ends fires by itself too (burnt out): ours that it
        // stopped are not ours any more (their slot is reused).
        static std::vector<replay::fire::Call> calls;
        replay::fire::CollectSince(g_poseV2FirePlayCursor, calls);
        static std::vector<std::uint64_t> ourRemnants;
        if (g_poseV2FiresOurs.empty())
            ourRemnants.clear();
        for (const replay::fire::Call& call : calls)
        {
            if (call.kind == replay::fire::CallKind::RemnantAdd && call.remnant >= 0)
            {
                for (const OurFire& ours : g_poseV2FiresOurs)
                    if (reinterpret_cast<std::uint64_t>(ours.fire) == call.fire)
                        ourRemnants.push_back(replay::fire::RemnantAddress(call.remnant));
                continue;
            }
            if (call.kind != replay::fire::CallKind::Stop)
                continue;
            for (OurFire& ours : g_poseV2FiresOurs)
                if (ours.alive && reinterpret_cast<std::uint64_t>(ours.fire) == call.fire)
                {
                    ours.alive = false;
                    ++g_poseV2FireEngineEnded;
                    if (g_poseV2FireEngineEnded <= 15)
                    {
                        // Evidence: how long the engine let it burn, against
                        // the recording (and the state it ended with).
                        std::uint32_t recordedStop = 0;
                        for (const auto& stop : Ctx().clip.fireStops.Events())
                            if (stop.start == ours.start)
                                recordedStop = stop.timeMs;
                        const auto& event = Ctx().clip.fireStarts.Events()[ours.start];
                        float life = 0.0f;
                        float strength = 0.0f;
                        std::uint32_t flags = 0;
                        std::memcpy(&life, call.state + 0x188, sizeof(life));
                        std::memcpy(&strength, call.state + 0xA8, sizeof(strength));
                        std::memcpy(&flags, call.state + 0xE8, sizeof(flags));
                        Ctx().log << "POSE_V2_PLAYBACK_FIRE_ENDED,index=" << ours.start << ",played_ms="
                            << (g_poseV2FireLastPlayhead > event.timeMs ? g_poseV2FireLastPlayhead - event.timeMs : 0)
                            << ",recorded_ms=" << (recordedStop > event.timeMs ? recordedStop - event.timeMs : 0)
                            << ",life=" << life << ",strength=" << strength << ",flags=0x" << std::hex << flags << std::dec
                            << ",instant=" << static_cast<unsigned>(call.instant) << ",caller=0x" << std::hex << call.caller << std::dec << '\n';
                    }
                }
        }
        RestorePoseV2FireItems();
        LogPoseV2FireEntities(timeMs);
        // Cheap scrub: the fires stay held where the scrub began (the state is
        // left as it was during a scrub); the playhead they were at stays the last
        // one seen, so the release reconciles once at the new playhead.
        if (g_poseV2StateDeferred)
        {
            HoldPoseV2Fires(g_poseV2FireLastPlayhead);
            return;
        }
        const std::uint32_t last = g_poseV2FireLastPlayhead;
        g_poseV2FireLastPlayhead = timeMs;
        // A real move of the playhead that normal play does not cover
        // (jump, rewind, scrub step); a paused playhead is not a jump.
        const bool jumped = timeMs != last && (!Ctx().hooks->NormalPlay() || timeMs < last ||
            timeMs - last > replay::entity::ForwardEventCursor::kMaxStepMs);
        if (jumped)
        {
            ReconcilePoseV2Fires(timeMs);
            g_poseV2FireStartCursorPlay.Reset(timeMs);
            g_poseV2FireStopCursorPlay.Reset(timeMs);
        }
        RetryPoseV2FireStarts(timeMs);
        PumpPoseV2FireEndedQueue(timeMs);
        {
            static std::vector<std::uint64_t> owners;
            owners.clear();
            for (const OurFire& ours : g_poseV2FiresOurs)
                if (ours.alive)
                    owners.push_back(reinterpret_cast<std::uint64_t>(ours.fire));
            owners.insert(owners.end(), ourRemnants.begin(), ourRemnants.end());
            replay::fire::SetFrozenOwners(owners);
            // A zero time step on our fires and items while paused leaves nothing
            // drawn (the area update builds the draw lists), so the freeze stays off;
            // the hold below keeps the state.
            replay::fire::SetFreeze(false);
        }
        if (!Ctx().hooks->NormalPlay())
        {
            HoldPoseV2Fires(timeMs);
            return;
        }
        if (jumped)
            return;
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
        static std::vector<std::size_t> due;
        if (g_poseV2FireStartCursorPlay.Advance(timeMs, Ctx().hooks->NormalPlay(), fromMs, toMs))
        {
            Ctx().clip.fireStarts.Range(fromMs, toMs, due);
            for (const std::size_t index : due)
            {
                // A fire that entered the clip already burning (late entry) starts with
                // its age.
                const replay::entity::FireStartEvent& event = Ctx().clip.fireStarts.Events()[index];
                if (StartPoseV2Fire(static_cast<std::uint32_t>(index), timeMs) && event.initial && event.initialAge > 0.0f)
                {
                    const float age = event.initialAge + static_cast<float>(timeMs > event.timeMs ? timeMs - event.timeMs : 0) / 1000.0f;
                    WriteOurFire(g_poseV2FiresOurs.back(), replay::fire::kFireAgeOffset, &age, sizeof(age));
                }
            }
        }
        if (g_poseV2FireStopCursorPlay.Advance(timeMs, Ctx().hooks->NormalPlay(), fromMs, toMs))
        {
            Ctx().clip.fireStops.Range(fromMs, toMs, due);
            for (const std::size_t index : due)
            {
                const auto& stop = Ctx().clip.fireStops.Events()[index];
                for (OurFire& ours : g_poseV2FiresOurs)
                {
                    if (ours.start == stop.start)
                        ours.stopped = true;
                    if (ours.alive && ours.start == stop.start)
                    {
                        std::string error;
                        if (replay::fire::StopFire(ours.fire, stop.instant != 0, stop.arg3, error))
                            ++g_poseV2FireStopped;
                        ours.alive = false;
                    }
                }
            }
        }
    }
}
