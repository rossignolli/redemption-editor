// script_camera.cpp: the replay camera: marker targets and bones, placement, lens and depth of field, cuts, prefetch, the world focus.
// Split from script.cpp (moved code); shared internals in script_internal.h.

#include "../script_internal.h"

namespace
{
    bool g_camLent = false;
    std::uint64_t g_camLends = 0;
    bool g_replayCamPlacedThisTick = false; // UpdateReplayCamera placed the replay camera this tick
    bool g_replayCamKeyedThisTick = false;  // and it followed the camera markers

    // A camera target's puppet: where it stands and where it faces.
    bool TargetState(std::uint32_t target, Vector3& position, float& heading)
    {
        const replay::playback::Puppet* puppet = g_puppets.Find(replay::entity::ReplayId::FromPacked(target));
        if (!target || !puppet || !puppet->clone || !ENTITY::DOES_ENTITY_EXIST(puppet->clone))
            return false;
        position = ENTITY::GET_ENTITY_COORDS(puppet->clone, TRUE, FALSE);
        heading = ENTITY::GET_ENTITY_HEADING(puppet->clone);
        return true;
    }
}

    // A Follow keyframe keeps the offset it had from its target at its own
    // time: the command seeks there, and the offset is read once the
    // puppets stand at that time (after the playback update of the tick).
    std::uint32_t g_pendingFollowKeyframe = 0;

    void ResolvePendingFollow()
    {
        if (!g_pendingFollowKeyframe)
            return;
        const replay::camera::Keyframe* key = g_cameraTrack.Find(g_pendingFollowKeyframe);
        Vector3 at{};
        float heading = 0.0f;
        if (key && !replay::playback::Preparing() && TargetState(key->target, at, heading))
        {
            const replay::editor::Vec3 world{key->pose.position.x - at.x, key->pose.position.y - at.y, key->pose.position.z - at.z};
            const replay::editor::Vec3 offset = replay::camera::ToHeadingFrame(world, heading);
            g_cameraTrack.SetMode(key->id, replay::editor::CameraMode::Follow, key->target, offset);
            LogFile() << "CAMERA_KEY,action=follow,id=" << key->id << ",offset=" << offset.x << '/' << offset.y << '/' << offset.z << '\n';
            g_pendingFollowKeyframe = 0;
        }
        else if (!key)
            g_pendingFollowKeyframe = 0;
    }
namespace
{

    // Camera markers: the evaluator is pure (camera_track); the engine gives it the
    // puppets.
    constexpr int kHeadBone = 21030; // SKEL_Head

    // The live puppet read by natives (the fallback).
    bool LiveMarkerEntity(const replay::playback::Puppet* puppet, replay::camera::EntityState& out)
    {
        if (!puppet || !puppet->clone || !ENTITY::DOES_ENTITY_EXIST(puppet->clone))
            return false;
        const Vector3 p = ENTITY::GET_ENTITY_COORDS(puppet->clone, TRUE, FALSE);
        const Vector3 r = ENTITY::GET_ENTITY_ROTATION(puppet->clone, 2);
        out.position = {p.x, p.y, p.z};
        out.pitch = r.x;
        out.roll = r.y;
        out.heading = r.z;
        out.hasHead = false;
        if (puppet->packet.kind == replay::entity::EntityKind::Ped || puppet->packet.kind == replay::entity::EntityKind::Horse)
        {
            const Vector3 head = PED::GET_PED_BONE_COORDS(puppet->clone, kHeadBone, 0.0f, 0.0f, 0.0f);
            if (head.x != 0.0f || head.y != 0.0f || head.z != 0.0f)
            {
                out.hasHead = true;
                out.head = {head.x, head.y, head.z};
            }
        }
        return true;
    }

    // The camera follows the target's RECORDED root at the asked time
    // (replay_playback.h RecordedBodyAt), and its head from the recorded pose.
    // LiveMarkerEntity: the live puppet. The head bone index is the
    // engine's (GET_PED_BONE_INDEX on the puppet) and is checked against the live
    // head at the playhead: three misses over 0.5 m with no match turn it off for
    // that entity and record (the head then rides the recorded root with the live
    // offset).
    struct CamTargetHead
    {
        std::uint32_t id = 0;
        std::uint32_t record = 0;
        int bone = -1;      // -1: not asked yet; -2: no head bone
        std::uint8_t good = 0;
        std::uint8_t bad = 0;
        bool off = false;
    };
    std::vector<CamTargetHead> g_camTargetHeads;
    struct CamTargetCache
    {
        std::uint32_t id = 0;
        std::uint64_t timeMs = 0;
        replay::camera::EntityState state;
    };
    CamTargetCache g_camTargetCache[8];
    std::uint32_t g_camTargetCacheNext = 0;
    // Per CAMERA_CUT_RATE second.
    std::uint32_t g_camTargetRecordedHits = 0;
    std::uint32_t g_camTargetLiveHits = 0;
    std::uint32_t g_camTargetDriftOver5cm = 0;
    float g_camTargetDriftMaxM = 0.0f;
    replay::log::LogLimit g_camTargetLogs;

    // The bone a marker is attached to or looks at. A named bone becomes the
    // skeleton index once per puppet (GET_ENTITY_BONE_INDEX_BY_NAME on its clone,
    // the index of the recorded pose's object space bones, as the head above); the
    // bone at a time is the recorded root times the recorded bone
    // (camera::EntityFromBone), the live clone only without a record there. Caches
    // keyed on the clone handle: a puppet built again asks again.
    struct CamBoneIndex
    {
        std::uint32_t id = 0;
        Entity clone = 0;
        std::uint32_t ref = 0;
        int index = -1;
    };
    std::vector<CamBoneIndex> g_camBoneIndex;
    struct CamBoneCache
    {
        std::uint32_t id = 0;
        std::uint32_t ref = 0;
        std::uint64_t timeMs = 0;
        replay::camera::EntityState state;
    };
    CamBoneCache g_camBoneCache[8];
    std::uint32_t g_camBoneCacheNext = 0;
    struct CamBoneNote
    {
        std::uint32_t id = 0;
        std::uint32_t ref = 0;
        std::uint8_t checks = 0;
        bool missingLogged = false;
    };
    std::vector<CamBoneNote> g_camBoneNotes;
    std::vector<CamTargetBones> g_camTargetBones;
    // Per CAMERA_CUT_RATE second.
    std::uint32_t g_camBoneRecorded = 0;
    std::uint32_t g_camBoneLive = 0;
    std::uint32_t g_camBoneMissing = 0;
    replay::log::LogLimit g_camBoneLogs;

    void ForgetCamTargets()
    {
        g_camBoneIndex.clear();
        for (CamBoneCache& entry : g_camBoneCache)
            entry = CamBoneCache{};
        g_camBoneNotes.clear();
        g_camTargetBones.clear();
        g_camTargetHeads.clear();
        for (CamTargetCache& entry : g_camTargetCache)
            entry = CamTargetCache{};
        g_camTargetCacheNext = 0;
    }

    CamTargetHead& CamTargetHeadFor(std::uint32_t id, std::uint32_t record)
    {
        for (CamTargetHead& head : g_camTargetHeads)
            if (head.id == id && head.record == record)
                return head;
        if (g_camTargetHeads.size() >= 256)
            g_camTargetHeads.erase(g_camTargetHeads.begin());
        g_camTargetHeads.push_back(CamTargetHead{id, record});
        return g_camTargetHeads.back();
    }
}

    bool MarkerEntity(std::uint32_t id, std::uint64_t timeMs, replay::camera::EntityState& out)
    {
        const replay::playback::Puppet* puppet = id ? g_puppets.Find(replay::entity::ReplayId::FromPacked(id)) : nullptr;
        if (!id || !puppet)
            return false;
        for (const CamTargetCache& entry : g_camTargetCache)
        {
            if (entry.id == id && entry.timeMs == timeMs)
            {
                out = entry.state;
                ++g_camTargetRecordedHits;
                return true;
            }
        }
        const bool body = puppet->packet.kind == replay::entity::EntityKind::Ped ||
            puppet->packet.kind == replay::entity::EntityKind::Horse;
        const bool clone = puppet->clone && ENTITY::DOES_ENTITY_EXIST(puppet->clone);
        // The head bone of the record at that time (asked once per record).
        replay::playback::RecordedBody recorded;
        // Before the record's first sample the first one (holdFirst), as for the bones.
        if (!replay::playback::RecordedBodyAt(id, static_cast<std::uint32_t>(std::min<std::uint64_t>(timeMs, 0xFFFFFFFFull)),
                -1, recorded, true))
        {
            ++g_camTargetLiveHits;
            return LiveMarkerEntity(puppet, out); // no pose record there: the live puppet
        }
        CamTargetHead* head = nullptr;
        if (body)
        {
            head = &CamTargetHeadFor(id, recorded.recordId);
            if (head->bone == -1 && clone)
            {
                const int bone = PED::GET_PED_BONE_INDEX(puppet->clone, kHeadBone);
                head->bone = bone >= 0 ? bone : -2;
            }
            replay::playback::RecordedBody withHead;
            if (head->bone >= 0 && !head->off &&
                replay::playback::RecordedBodyAt(id, static_cast<std::uint32_t>(timeMs), head->bone, withHead, true))
                recorded = withHead;
        }
        const replay::editor::Vec3 position{recorded.position[0], recorded.position[1], recorded.position[2]};
        const replay::editor::Vec3 headAt{recorded.head[0], recorded.head[1], recorded.head[2]};
        out = replay::camera::EntityFromRecorded(position, recorded.rotation, recorded.hasHead ? &headAt : nullptr);
        // At the playhead the live puppet is there to compare with (the drift the
        // camera does not take) and to check the head bone.
        replay::camera::EntityState live{};
        const bool atPlayhead = timeMs == g_transport.PlayheadMs() && clone && LiveMarkerEntity(puppet, live);
        if (atPlayhead)
        {
            const float dx = live.position.x - position.x, dy = live.position.y - position.y, dz = live.position.z - position.z;
            const float drift = std::sqrt(dx * dx + dy * dy + dz * dz);
            g_camTargetDriftMaxM = std::max(g_camTargetDriftMaxM, drift);
            if (drift > 0.05f)
                ++g_camTargetDriftOver5cm;
            if (head && recorded.hasHead && live.hasHead && head->good < 3 && head->bad < 3)
            {
                const float hx = live.head.x - headAt.x, hy = live.head.y - headAt.y, hz = live.head.z - headAt.z;
                const float gap = std::sqrt(hx * hx + hy * hy + hz * hz);
                if (gap <= 0.5f + drift)
                    ++head->good;
                else if (++head->bad >= 3 && head->good == 0)
                {
                    head->off = true;
                    for (CamTargetCache& entry : g_camTargetCache)
                        if (entry.id == id)
                            entry = CamTargetCache{};
                }
                if ((head->good == 3 || head->off) && g_camTargetLogs.Take(64))
                    LogFile() << "CAMERA_TARGET,phase=head_check,id=" << id << ",record=0x" << std::hex << recorded.recordId
                              << std::dec << ",bone=" << head->bone << ",result=" << (head->off ? "off" : "ok")
                              << ",gap_m=" << gap << ",drift_m=" << drift << ",playhead_ms=" << timeMs << '\n';
            }
        }
        if (body && (!out.hasHead || (head && head->off)))
        {
            // No recorded head: the live head's offset from the live root,
            // carried by the recorded root (its heading frame).
            replay::camera::EntityState offsetFrom{};
            const bool haveLive = atPlayhead ? true : (clone && LiveMarkerEntity(puppet, offsetFrom));
            const replay::camera::EntityState& source = atPlayhead ? live : offsetFrom;
            if (haveLive && source.hasHead)
            {
                const replay::editor::Vec3 rel = replay::camera::ToHeadingFrame(
                    {source.head.x - source.position.x, source.head.y - source.position.y, source.head.z - source.position.z},
                    source.heading);
                const replay::editor::Vec3 back = replay::camera::FromHeadingFrame(rel, out.heading);
                out.hasHead = true;
                out.head = {position.x + back.x, position.y + back.y, position.z + back.z};
            }
        }
        ++g_camTargetRecordedHits;
        CamTargetCache& slot = g_camTargetCache[g_camTargetCacheNext++ % 8];
        slot.id = id;
        slot.timeMs = timeMs;
        slot.state = out;
        return true;
    }
namespace
{

    CamBoneNote& CamBoneNoteFor(std::uint32_t id, std::uint32_t ref)
    {
        for (CamBoneNote& note : g_camBoneNotes)
            if (note.id == id && note.ref == ref)
                return note;
        if (g_camBoneNotes.size() >= 256)
            g_camBoneNotes.erase(g_camBoneNotes.begin());
        g_camBoneNotes.push_back(CamBoneNote{id, ref});
        return g_camBoneNotes.back();
    }

    replay::camera::bones::Family CamFamilyOf(const replay::playback::Puppet& puppet, bool clone)
    {
        using replay::camera::bones::Family;
        switch (puppet.packet.kind)
        {
        case replay::entity::EntityKind::Ped:
            return clone && !PED::IS_PED_HUMAN(puppet.clone) ? Family::Animal : Family::Human;
        case replay::entity::EntityKind::Horse: return Family::Horse;
        case replay::entity::EntityKind::Weapon: return Family::Weapon;
        case replay::entity::EntityKind::Vehicle:
        case replay::entity::EntityKind::TrainCar: return Family::Vehicle;
        default: return Family::None;
        }
    }

    // The skeleton index of a named bone on this clone (first engine name
    // found), -1 when it has none of them.
    int NamedBoneIndex(Entity clone, std::uint32_t named)
    {
        if (named >= replay::camera::bones::kNamedCount)
            return -1;
        for (const char* name : replay::camera::bones::kNamed[named].names)
        {
            if (!name)
                break;
            const int index = ENTITY::GET_ENTITY_BONE_INDEX_BY_NAME(clone, name);
            if (index >= 0)
                return index;
        }
        return -1;
    }
}

    const CamTargetBones& TargetBonesFor(const replay::playback::Puppet& puppet)
    {
        const std::uint32_t id = puppet.packet.id.Packed();
        const bool clone = puppet.clone && ENTITY::DOES_ENTITY_EXIST(puppet.clone);
        const ULONGLONG now = GetTickCount64();
        CamTargetBones* entry = nullptr;
        for (CamTargetBones& known : g_camTargetBones)
            if (known.id == id)
                entry = &known;
        if (entry && entry->clone == (clone ? puppet.clone : 0) && (entry->count || now - entry->countAskedAt < 1000))
            return *entry;
        if (!entry)
        {
            if (g_camTargetBones.size() >= 256)
                g_camTargetBones.erase(g_camTargetBones.begin());
            g_camTargetBones.push_back(CamTargetBones{});
            entry = &g_camTargetBones.back();
        }
        const bool sameClone = entry->id == id && entry->clone == (clone ? puppet.clone : 0);
        entry->id = id;
        if (!sameClone)
        {
            entry->clone = clone ? puppet.clone : 0;
            const replay::camera::bones::Family family = CamFamilyOf(puppet, clone);
            entry->family = static_cast<std::uint32_t>(family);
            entry->mask = 0;
            int found[replay::camera::bones::kNamedCount];
            for (std::uint32_t n = 0; n < replay::camera::bones::kNamedCount; ++n)
            {
                found[n] = clone && replay::camera::bones::kNamed[n].family == family ? NamedBoneIndex(puppet.clone, n) : -1;
                if (found[n] >= 0)
                    entry->mask |= 1ull << n;
            }
            // What each weapon model answered for the engine's weapon bone names (the
            // engine hashes the name and finds its tag on this skeleton), once per model:
            // the evidence of the names.
            static std::vector<std::uint32_t> loggedModels;
            if (clone && family == replay::camera::bones::Family::Weapon &&
                std::find(loggedModels.begin(), loggedModels.end(), puppet.packet.modelHash) == loggedModels.end() &&
                loggedModels.size() < 128)
            {
                loggedModels.push_back(puppet.packet.modelHash);
                replay::playback::RecordedBody counted;
                const bool recorded = replay::playback::RecordedBodyAt(id,
                    static_cast<std::uint32_t>(std::min<std::uint64_t>(g_transport.PlayheadMs(), 0xFFFFFFFFull)), -1, counted);
                std::ostringstream names;
                std::uint32_t named = 0;
                for (std::uint32_t n = 0; n < replay::camera::bones::kNamedCount; ++n)
                    if (found[n] >= 0)
                    {
                        names << (named++ ? ";" : "") << replay::camera::bones::kNamed[n].names[0] << '=' << found[n];
                    }
                LogFile() << "WEAPON_BONE_NAMES,id=" << id << ",model=0x" << std::hex << puppet.packet.modelHash << ",weapon=0x"
                          << puppet.packet.weaponHash << std::dec << ",bones=" << (recorded ? counted.boneCount : 0)
                          << ",named=" << named << ",names=" << (named ? names.str() : std::string("-")) << '\n';
            }
        }
        replay::playback::RecordedBody body;
        entry->count = replay::playback::RecordedBodyAt(id,
            static_cast<std::uint32_t>(std::min<std::uint64_t>(g_transport.PlayheadMs(), 0xFFFFFFFFull)), -1, body) ? body.boneCount : 0;
        entry->countAskedAt = now;
        return *entry;
    }
namespace
{

    int CamBoneIndexFor(const replay::playback::Puppet& puppet, bool clone, std::uint32_t ref)
    {
        namespace bones = replay::camera::bones;
        if (bones::ModeOf(ref) == bones::kModeIndex)
            return static_cast<int>(bones::ValueOf(ref));
        if (bones::ModeOf(ref) != bones::kModeNamed || !clone)
            return -1;
        const std::uint32_t id = puppet.packet.id.Packed();
        for (const CamBoneIndex& known : g_camBoneIndex)
            if (known.id == id && known.clone == puppet.clone && known.ref == ref)
                return known.index;
        if (g_camBoneIndex.size() >= 512)
            g_camBoneIndex.erase(g_camBoneIndex.begin());
        const int index = NamedBoneIndex(puppet.clone, bones::ValueOf(ref));
        g_camBoneIndex.push_back(CamBoneIndex{id, puppet.clone, ref, index});
        return index;
    }

    bool MarkerBone(std::uint32_t id, std::uint32_t ref, std::uint64_t timeMs, replay::camera::EntityState& out)
    {
        namespace bones = replay::camera::bones;
        const replay::playback::Puppet* puppet = id ? g_puppets.Find(replay::entity::ReplayId::FromPacked(id)) : nullptr;
        if (!puppet || !ref || !bones::Valid(ref))
            return false;
        for (const CamBoneCache& entry : g_camBoneCache)
            if (entry.id == id && entry.ref == ref && entry.timeMs == timeMs)
            {
                out = entry.state;
                return true;
            }
        const bool clone = puppet->clone && ENTITY::DOES_ENTITY_EXIST(puppet->clone);
        const int index = CamBoneIndexFor(*puppet, clone, ref);
        const std::uint32_t at = static_cast<std::uint32_t>(std::min<std::uint64_t>(timeMs, 0xFFFFFFFFull));
        replay::playback::RecordedBody body;
        bool found = false;
        const char* source = "missing";
        if (index >= 0 && replay::playback::RecordedBodyAt(id, at, index, body, true) &&
            static_cast<std::uint32_t>(index) < body.boneCount)
        {
            // Before the record's first sample the camera holds the first one; the live
            // branch below would mix the clone's entity rotation in.
            if (body.heldFirst && g_camBoneLogs.Take(64))
                LogFile() << "CAMERA_BONE,phase=hold_first,id=" << id << ",ref=0x" << std::hex << ref << std::dec << ",index=" << index
                          << ",time_ms=" << timeMs << '\n';
            const replay::editor::Vec3 root{body.position[0], body.position[1], body.position[2]};
            out = replay::camera::EntityFromBone(root, body.rotation, body.boneTranslation, body.boneRotation);
            found = std::isfinite(out.position.x) && std::isfinite(out.position.y) && std::isfinite(out.position.z);
            source = "recorded";
            if (found)
                ++g_camBoneRecorded;
        }
        else if (index >= 0 && clone)
        {
            // No pose record there: the live clone, and only for a named bone (its index
            // came from this clone's own skeleton). An index picked from the recorded list
            // is never asked of the engine: the clone's skeleton may have fewer bones than
            // the record and the native's bounds are not proven.
            const bool safe = bones::ModeOf(ref) == bones::kModeNamed;
            const Vector3 p = safe ? ENTITY::GET_WORLD_POSITION_OF_ENTITY_BONE(puppet->clone, index) : Vector3{};
            if (safe && (p.x != 0.0f || p.y != 0.0f || p.z != 0.0f))
            {
                const Vector3 r = ENTITY::GET_ENTITY_ROTATION(puppet->clone, 2);
                out = replay::camera::EntityState{};
                out.position = {p.x, p.y, p.z};
                out.pitch = r.x; // the clone's rotation: no bone rotation native in RDR2
                out.roll = r.y;
                out.heading = r.z;
                out.hasHead = true;
                out.head = out.position;
                found = true;
                source = "live";
                ++g_camBoneLive;
            }
        }
        CamBoneNote& note = CamBoneNoteFor(id, ref);
        if (!found)
        {
            ++g_camBoneMissing;
            if (!note.missingLogged && g_camBoneLogs.Take(64))
            {
                note.missingLogged = true;
                LogFile() << "CAMERA_BONE,phase=missing,id=" << id << ",ref=0x" << std::hex << ref << std::dec << ",index=" << index
                          << ",clone=" << (clone ? 1 : 0) << ",bones=" << body.boneCount << ",time_ms=" << timeMs << '\n';
            }
            return false;
        }
        // At the playhead, the recorded bone against the clone's (the bone
        // index and the object space read are checked, three times).
        if (clone && note.checks < 3 && timeMs == g_transport.PlayheadMs() && std::strcmp(source, "recorded") == 0 &&
            bones::ModeOf(ref) == bones::kModeNamed) // the same rule: an index from this clone only
        {
            const Vector3 live = ENTITY::GET_WORLD_POSITION_OF_ENTITY_BONE(puppet->clone, index);
            if (live.x != 0.0f || live.y != 0.0f || live.z != 0.0f)
            {
                ++note.checks;
                const float dx = live.x - out.position.x, dy = live.y - out.position.y, dz = live.z - out.position.z;
                if (g_camBoneLogs.Take(64))
                    LogFile() << "CAMERA_BONE,phase=check,id=" << id << ",ref=0x" << std::hex << ref << std::dec << ",index=" << index
                              << ",gap_m=" << std::sqrt(dx * dx + dy * dy + dz * dz) << ",bones=" << body.boneCount
                              << ",playhead_ms=" << timeMs << '\n';
            }
        }
        CamBoneCache& slot = g_camBoneCache[g_camBoneCacheNext++ % 8];
        slot.id = id;
        slot.ref = ref;
        slot.timeMs = timeMs;
        slot.state = out;
        return true;
    }
}

    // The recording's main character: Arthur's or John's puppet.
    std::uint32_t MainCharacterId()
    {
        static const Hash arthur = MISC::GET_HASH_KEY("player_zero");
        static const Hash john = MISC::GET_HASH_KEY("player_three");
        for (const replay::playback::Puppet& puppet : g_puppets.list)
        {
            if (puppet.packet.kind == replay::entity::EntityKind::Ped &&
                (puppet.packet.modelHash == arthur || puppet.packet.modelHash == john))
                return puppet.packet.id.Packed();
        }
        return 0;
    }
namespace
{

    // The Custom shake's phase memo (a clip change drops it; the markers and
    // events are in its signature).
    replay::camera::ShakePhaseCache g_shakePhaseCache;
}
    // Ticks to wait before folding a legacy shake left in the track (the bench's
    // marker and event steps come one a tick; TickShakeMigration).
    int g_shakeFoldHold = 0;

    // The Spline camera weight applies while the clip plays forward (normal play,
    // not in Edit Camera) or exports; paused, scrubbed or framing a marker the
    // camera sits where the markers are (a view saved from a weighted frame would
    // get the weight again on play).
    bool SplineWeightActive()
    {
        if (replay::exportprobe::Active())
            return true;
        return g_transport.Playing() && !g_transport.Paused() && replay::playback::NormalPlay() && g_editingMarker == 0;
    }

    replay::camera::Scene MarkerScene()
    {
        static std::uint32_t phaseClip = ~0u;
        if (phaseClip != g_clipLoadSequence)
        {
            phaseClip = g_clipLoadSequence;
            g_shakePhaseCache.Clear();
        }
        replay::camera::Scene scene;
        scene.shakePhase = &g_shakePhaseCache;
        scene.splineDefaults = g_splineDefaults;
        scene.splineWeight = SplineWeightActive();
        scene.lookup = MarkerEntity;
        scene.boneLookup = MarkerBone;
        scene.game = &g_clip.gameCamera;
        int width = 0, height = 0;
        GRAPHICS::GET_SCREEN_RESOLUTION(&width, &height);
        scene.aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 16.0f / 9.0f;
        scene.mainCharacter = MainCharacterId();
        // No blend or game camera interpolation across a cut; LockCutsceneCamera gives
        // a recorded cutscene to the game camera.
        scene.cuts = &g_clip.cuts;
        if (g_recConfig.lockCutsceneCamera)
            scene.gameOnly = [](std::uint64_t timeMs) {
                return replay::gate::SpanAt(g_clip.spans, replay::gate::SpanKind::Cutscene, timeMs) != nullptr;
            };
        return scene;
    }

    replay::camera::SpringState g_cameraSpring;
namespace
{

    // A shake migration in the log, once, as SHAKE_MIGRATION (its counts in
    // both builds). The proof (the legacy track and the migrated one compared at
    // every 30 Hz frame of the clip with this clip's scene: the shake and the
    // camera path) runs in the developer build only: its cost is the declared
    // duration's (a small legacy clip may declare 49 days), the public build
    // logs proof=skipped. The track kept for it is let go first, so a failed
    // proof (out of memory) is not tried again every tick.
    void LogShakeMigration(const char* source)
    {
        replay::camera::ShakeMigrationReport& report = g_cameraTrack.Migration();
        if (!report.before)
            return;
        const std::shared_ptr<const replay::camera::Track> before = std::move(report.before);
        report.before.reset();
        const ULONGLONG startedAt = GetTickCount64();
        replay::camera::mshake::Proof proof;
        proof.skipped = true;
        const char* proofError = nullptr;
        if constexpr (replay::identity::kDevBuild)
        {
            const std::uint64_t duration = std::max<std::uint64_t>(g_transport.DurationMs(),
                g_cameraTrack.Keyframes().empty() ? 0 : g_cameraTrack.Keyframes().back().timeMs);
            try
            {
                proof = replay::camera::mshake::Verify(*before, g_cameraTrack, MarkerScene(), duration, 0);
            }
            catch (const std::exception&)
            {
                proof = {};
                proof.skipped = true;
                proofError = "no_memory";
            }
        }
        LogFile() << replay::camera::mshake::FormatLine(report, proof) << (proofError ? ",proof_error=" : "")
                  << (proofError ? proofError : "") << ",source=" << source
                  << ",verify_ms=" << (GetTickCount64() - startedAt) << ",clip=" << Narrow(g_currentClipPath) << '\n';
    }
}

    // Once a tick: a migrated clip's proof; a legacy shake still waiting in the
    // track (the bench's steps, a director's line) folded once no marker or event
    // step came for two ticks.
    void TickShakeMigration()
    {
        if (g_shakeFoldHold > 0)
        {
            --g_shakeFoldHold;
            return;
        }
        if (g_cameraTrack.HasLegacyShake())
        {
            replay::camera::mshake::Migrate(g_cameraTrack);
            g_shakePhaseCache.Clear();
            LogShakeMigration("fold");
            return;
        }
        if (g_cameraTrack.Migration().before)
            LogShakeMigration("load");
    }

    bool EvaluateCameraTrack(std::uint64_t timeMs, replay::editor::CameraState& state, float dtSeconds,
        replay::camera::SegmentPlan* plan)
    {
        if (g_cameraTrack.Empty())
            return false;
        return replay::camera::EvaluateMarkers(g_cameraTrack, timeMs, MarkerScene(), state, &g_cameraSpring, dtSeconds, plan);
    }
namespace
{

    // Every camera of the editor is placed through here (src/camera_cut.h). Params
    // mode: one SET_CAM_PARAMS whose cut flag is set only on a real cut (the TAA
    // keeps its history); legacy mode (ReplayCameraCutMode=legacy, or the first
    // placements did not take): SET_CAM_COORD + SET_CAM_ROT + SET_CAM_FOV, a cut
    // every frame.
    bool g_camCutParamsFailed = false;
    bool g_camCutVerifiedLogged = false;
    replay::camcut::Tracker g_replayCamCut;
}
    replay::camcut::Tracker g_freeCamCut;
namespace
{
    std::uint32_t g_camPlacedCutsThisTick = 0;
    std::uint32_t g_camCutLogs = 0;
    std::uint32_t g_camDofRecreations = 0; // ForgetReplayCamDof with a camera made (CAMERA_CUT_RATE)
    std::uint32_t g_lastPlaceWhy = 0;       // PlaceCam's last cut decision (camcut::CutWhy)

    // The camera object behind a handle through the engine's own lookup 0x1739DC
    // (rung 4, its body compared once), its cut bits cleared after SET_CAM_COORD /
    // SET_CAM_ROT when the placement is not a real cut (rung 3: a plain field of
    // the replay's own camera, written only after the object is shown to be that
    // camera: the position just set at +0x50 and the position bit up). Any
    // failure: the plain natives for good, logged.
    bool g_camBitsChecked = false;
    bool g_camBitsUsable = false;
    bool g_camBitsFailed = false;
    bool g_camBitsVerifiedLogged = false;
    std::uint32_t g_camBitsCleared = 0;     // placements whose bits were cleared (this second)
    std::uint32_t g_camBitsSeenBefore = 0;  // ... whose camera still had cut bits from the frame before
    std::uint32_t g_camBitsClean = 0;       // clean clears in a row (verification)
    void* CallCamLookup(std::uintptr_t function, int handle, std::uint32_t& code)
    {
        void* camera = nullptr;
        if (!replay::engine::Call(replay::engine::kCameraLookup, function, camera, handle))
            replay::engine::Failed(code);
        return camera;
    }

    std::uintptr_t CamObject(Cam cam, std::uint32_t& code)
    {
        const std::uintptr_t lookup = replay::sig::Find(replay::camcut::kCameraLookup);
        if (!g_camBitsChecked)
        {
            g_camBitsChecked = true;
            // The lookup, and SET_CAM_COORD's store of the cut bit on what it
            // returns (the word the replay clears).
            g_camBitsUsable = lookup && replay::sig::Find(replay::camcut::kCameraCoordCutBit);
            LogFile() << "CAMERA_CUT_MODE,phase=bits_config,lookup=" << (g_camBitsUsable ? "ok" : "signature_mismatch")
                      << ",rva=0x" << std::hex << (lookup ? lookup - replay::sig::ImageBase() : 0) << std::dec << '\n';
        }
        if (!g_camBitsUsable)
            return 0;
        return reinterpret_cast<std::uintptr_t>(CallCamLookup(lookup, cam, code));
    }

    void CamBitsFallback(const char* why, Cam cam, std::uint32_t code)
    {
        g_camBitsFailed = true;
        LogFile() << "CAMERA_CUT_MODE,phase=fallback,mode=legacy,why=" << why << ",cam=" << cam << ",code=0x" << std::hex
                  << code << std::dec << '\n';
    }

    // After SET_CAM_COORD + SET_CAM_ROT on `cam` at (px, py, pz): the cut
    // bits off unless `cut`. Also counts a camera that still had the bits
    // from the frame before (they were not the engine's to clear).
    void ClearCamCutBits(Cam cam, float px, float py, float pz, bool cut)
    {
        std::uint32_t code = 0;
        const std::uintptr_t object = CamObject(cam, code);
        if (!object)
        {
            CamBitsFallback(g_camBitsUsable ? (code ? "lookup_exception" : "lookup_null") : "lookup_signature", cam, code);
            return;
        }
        float position[3]{};
        std::uint16_t flags = 0;
        if (!replay::mem::Copy(position, object + replay::camcut::kCamPositionOffset, sizeof(position)) ||
            !replay::mem::Read(object + replay::camcut::kCamFlagsOffset, flags))
        {
            CamBitsFallback("read_fault", cam, 0);
            return;
        }
        const float want[3] = {px, py, pz};
        if (!replay::camcut::CamObjectIsOurs(position, want, flags))
        {
            CamBitsFallback("not_our_camera", cam, flags);
            return;
        }
        if (cut)
            return; // a real cut keeps its bits
        const std::uint16_t cleared = replay::camcut::ClearedCutBits(flags);
        std::uint16_t back = 0xFFFF;
        if (!replay::mem::Put(object + replay::camcut::kCamFlagsOffset, &cleared, sizeof(cleared)) ||
            !replay::mem::Read(object + replay::camcut::kCamFlagsOffset, back) || back != cleared)
        {
            CamBitsFallback("write_fault", cam, back);
            return;
        }
        ++g_camBitsCleared;
        if (++g_camBitsClean == 3 && !g_camBitsVerifiedLogged)
        {
            g_camBitsVerifiedLogged = true;
            LogFile() << "CAMERA_CUT_MODE,phase=verified,mode=bits,flags_before=0x" << std::hex << flags << ",flags_after=0x"
                      << back << std::dec << '\n';
        }
    }
}
    PlacedView g_lastPlacedView;

    void PlaceCam(Cam cam, replay::camcut::Tracker& tracker, const char* which, float px, float py, float pz, float rx,
        float ry, float rz, float fov, float realDt, bool forceCut)
    {
        replay::camcut::Placement placement;
        placement.camera = cam;
        placement.position[0] = px; placement.position[1] = py; placement.position[2] = pz;
        placement.rotation[0] = rx; placement.rotation[1] = ry; placement.rotation[2] = rz;
        placement.fov = fov;
        placement.realDt = realDt;
        placement.forceCut = forceCut;
        const std::uint32_t why = replay::camcut::Decide(tracker, placement);
        g_lastPlaceWhy = why; // the shot's memory (focus) starts over on a cut
        // The view the world overlay is projected with.
        g_lastPlacedView.valid = true;
        g_lastPlacedView.view = {{px, py, pz}, {rx, ry, rz}, fov};
        // The cut bits mode (camera_cut.h Mode::Bits); SET_CAM_PARAMS once the bits
        // could not be cleared, the plain natives once that did not take either.
        if (!g_camBitsFailed)
        {
            {
                // The camera's word before this placement: cut bits left from
                // the frame before mean the engine did not clear them.
                std::uint32_t code = 0;
                const std::uintptr_t object = CamObject(cam, code);
                std::uint16_t before = 0;
                if (object && replay::mem::Read(object + replay::camcut::kCamFlagsOffset, before) &&
                    (before & replay::camcut::kCutBits))
                    ++g_camBitsSeenBefore;
            }
            CAM::SET_CAM_COORD(cam, px, py, pz);
            CAM::SET_CAM_ROT(cam, rx, ry, rz, 2);
            CAM::SET_CAM_FOV(cam, fov);
            const bool cut = why != replay::camcut::kWhyNone;
            ClearCamCutBits(cam, px, py, pz, cut);
            if (cut || g_camBitsFailed)
                ++g_camPlacedCutsThisTick;
            if (cut && replay::log::Verbose() && g_camCutLogs < 200)
            {
                ++g_camCutLogs;
                LogFile() << "CAMERA_CUT,cam=" << which << ",why=0x" << std::hex << why << std::dec
                          << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
            }
            return;
        }
        if (g_camCutParamsFailed)
        {
            CAM::SET_CAM_COORD(cam, px, py, pz);
            CAM::SET_CAM_ROT(cam, rx, ry, rz, 2);
            CAM::SET_CAM_FOV(cam, fov);
            ++g_camPlacedCutsThisTick;
            return;
        }
        const bool cut = why != replay::camcut::kWhyNone;
        CAM::SET_CAM_PARAMS(cam, px, py, pz, rx, ry, rz, fov, 0, 0, 0, 2, cut ? 1 : 0, 0);
        if (cut)
        {
            ++g_camPlacedCutsThisTick;
            if (replay::log::Verbose() && g_camCutLogs < 200)
            {
                ++g_camCutLogs;
                LogFile() << "CAMERA_CUT,cam=" << which << ",why=0x" << std::hex << why << std::dec
                          << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
            }
        }
        if (tracker.placements <= 3)
        {
            // SET_CAM_PARAMS applies the rotation only when [cam+0x538] == 3 and the
            // position only when [cam+0x498] == 0 (camera_cut.h): read back, and the plain
            // natives for good when it did not take.
            const Vector3 gotPos = CAM::GET_CAM_COORD(cam);
            const Vector3 gotRot = CAM::GET_CAM_ROT(cam, 2);
            const float got[3] = {gotPos.x, gotPos.y, gotPos.z};
            const float gotR[3] = {gotRot.x, gotRot.y, gotRot.z};
            if (!replay::camcut::Took(placement.position, placement.rotation, got, gotR))
            {
                g_camCutParamsFailed = true;
                // The fields SET_CAM_PARAMS tests (camera_cut.h), read off the camera object
                // for the evidence.
                std::uint64_t attach = ~0ull, point = ~0ull;
                std::uint32_t camMode = ~0u;
                std::uint8_t frameBit = 0xFF;
                {
                    std::uint32_t code = 0;
                    const std::uintptr_t object = CamObject(cam, code);
                    if (object)
                    {
                        replay::mem::Read(object + replay::camcut::kCamAttachOffset, attach);
                        replay::mem::Read(object + replay::camcut::kCamPointOffset, point);
                        replay::mem::Read(object + replay::camcut::kCamModeOffset, camMode);
                        replay::mem::Read(object + replay::camcut::kCamFrameOffset, frameBit);
                    }
                }
                char line[420]{};
                std::snprintf(line, sizeof(line),
                    "CAMERA_CUT_MODE,phase=fallback,mode=%s,why=params_not_applied,cam=%s,placement=%u,"
                    "want=%.2f;%.2f;%.2f;%.1f;%.1f;%.1f,got=%.2f;%.2f;%.2f;%.1f;%.1f;%.1f,cam_498=0x%llx,cam_520=0x%llx,"
                    "cam_538=%d,cam_4b4=0x%x\n",
                    g_camBitsFailed ? "legacy" : "bits", which, tracker.placements, px, py, pz, rx, ry, rz, got[0], got[1],
                    got[2], gotR[0], gotR[1], gotR[2], static_cast<unsigned long long>(attach),
                    static_cast<unsigned long long>(point), static_cast<int>(camMode), static_cast<unsigned>(frameBit));
                LogFile() << line;
                CAM::SET_CAM_COORD(cam, px, py, pz);
                CAM::SET_CAM_ROT(cam, rx, ry, rz, 2);
                CAM::SET_CAM_FOV(cam, fov);
            }
            else if (tracker.placements == 3 && !g_camCutVerifiedLogged)
            {
                g_camCutVerifiedLogged = true;
                LogFile() << "CAMERA_CUT_MODE,phase=verified,mode=params,cam=" << which << '\n';
            }
        }
    }

    replay::camcut::MarkerFrame MarkerFrameOf(bool keyed, const replay::camera::SegmentPlan& plan)
    {
        replay::camcut::MarkerFrame frame;
        frame.valid = keyed && plan.valid;
        frame.current = plan.current;
        frame.next = plan.next;
        frame.blending = plan.blending;
        return frame;
    }
namespace
{
    LensState g_replayLens;
}
    LensState g_freeLens;
    // When the near clip or the DOF sent last changed (ApplyNearClip,
    // NoteDofReadback): UpdateWorldFreeze thaws the photo mode freeze for it.
    ULONGLONG g_lensChangedAt = 0;
    bool g_lensHeldLogged = false;

    // The photo mode freeze's state (rung 2, read only).
    // _REQUEST_PHOTO_MODE_FREEZE 0x7C709C01D43D94CD is the wrapper 0xF63B4C: mov
    // rcx, [rip + d] (the singleton pointer at RVA 0x3F02448), xor edx, edx, jmp
    // 0x71E260 (state 0 -> 1, the request). 0x73FFBC is the engine's "frozen" test
    // (state - 2 <= 1: states 2 and 3), and the camera code returns early on it
    // (0x44B619, 0x44B66E, 0x44B6D0). -1 when the signature or the read fails.
    int PhotoFreezeState()
    {
        static bool checked = false;
        static std::uintptr_t global = 0;
        if (!checked)
        {
            checked = true;
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            constexpr std::uintptr_t kWrapperRva = 0xF63B4C;
            constexpr std::uintptr_t kGlobalRva = 0x3F02448;
            std::uint8_t code[12]{};
            std::int32_t disp = 0;
            const bool read = replay::mem::Copy(code, base + kWrapperRva, sizeof(code));
            if (read)
                std::memcpy(&disp, code + 3, sizeof(disp));
            const bool ok = read && code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x0D && code[7] == 0x33 && code[8] == 0xD2 &&
                code[9] == 0xE9 && kWrapperRva + 7 + static_cast<std::intptr_t>(disp) == kGlobalRva;
            global = ok ? base + kGlobalRva : 0;
            LogFile() << "CAMERA_FREEZE_PROBE,lookup=" << (ok ? "ok" : read ? "signature_mismatch" : "read_fault") << ",rva=0x" << std::hex
                      << kGlobalRva << std::dec << '\n';
        }
        std::uint64_t object = 0;
        std::uint32_t state = 0;
        if (!global || !replay::mem::Read(global, object) || !object || !replay::mem::Read(static_cast<std::uintptr_t>(object), state))
            return -1;
        return static_cast<int>(state);
    }
    void NewLensCamera(LensState& lens)
    {
        lens.nearSent = -1.0f;
        lens.focusSent = -1.0f;
        lens.focusOverwriteRun = 0;
        lens.pauseForDistance = false;
        lens.dof = false;
        lens.engineFocus = false;
        lens.pausedBefore = -1;
    }
    bool g_replayCamDof = false;
    // The camera's autofocus was unpaused (_PAUSE_CAMERA_FOCUS false, the engine
    // focus).
    bool g_replayCamEngineFocus = false;
namespace
{
    float g_replayCamBlur = 0.0f; // this frame's motion blur strength
    replay::log::LogLimit g_markerFocusLogs; // MARKER_FOCUS lines (capped per session)
    // The shot's focus memory (hysteresis, held distance, distance spring), reset
    // on every cut of the replay camera.
    replay::camera::FocusMemory g_camFocusMemory;
    // Bit 0x4000 of [cam+0xEC] read just before the autofocus was let in
    // (0 or 1), -1 when it could not be read (then the fallback recreates).
    int g_replayCamFocusPausedBefore = -1;
    bool g_camDofClearFailed = false;          // the clear did not null the block: the recreate path for good
    std::uint32_t g_camDofClearChecks = 0;
    std::uint32_t g_camDofClears = 0;          // DOF dropped by the engine's clear (no camera made)
    std::uint32_t g_camFocusRestores = 0;      // autofocus left by the pause bit (no camera made)
    std::uint32_t g_camDofBlendFrames = 0;     // ticks with the DOF interpolated inside a blend
    std::uint32_t g_camFocusEngineFlips = 0;   // inside a shot: entity <-> engine autofocus flips
    std::uint32_t g_camFocusSourceChanges = 0; // inside a shot: any change of focus source
    replay::log::LogLimit g_camDofLogs;
    constexpr std::uint32_t kCamDofBlockOffset = 0x470; // qword, the DOF object (null: none)

    // A fresh camera without the DOF block (the block stays on a camera once set).
    // The new camera takes over in the same frame while the old one still renders
    // (otherwise the script cameras stop rendering for a frame and the gameplay
    // camera flashes on each switch); false when it cannot be made (then the next
    // tick creates one).
    bool ForgetReplayCamDof(const replay::editor::CameraState& state)
    {
        const Cam fresh = CAM::CREATE_CAM_WITH_PARAMS("DEFAULT_SCRIPTED_CAMERA", state.position.x, state.position.y,
            state.position.z, state.rotation.x, state.rotation.y, state.rotation.z, state.fov, FALSE, 2);
        const bool made = fresh && CAM::DOES_CAM_EXIST(fresh);
        if (made && g_replayCamRendering)
        {
            CAM::SET_CAM_NEAR_CLIP(fresh, 0.1f);
            CAM::SET_CAM_ACTIVE(fresh, TRUE);
        }
        if (made)
        {
            // The camera that renders this frame is the fresh one: it gets the placement,
            // with the cut (a new camera starts its springs over), in this same tick.
            PlaceCam(fresh, g_replayCamCut, "replay_dof", state.position.x, state.position.y, state.position.z,
                state.rotation.x, state.rotation.y, state.rotation.z, state.fov, 0.0f, true);
            ++g_camDofRecreations;
            g_camFocusMemory.ResetShot(); // a new camera is a cut
        }
        else if (g_replayCamRendering)
            CAM::RENDER_SCRIPT_CAMS(FALSE, FALSE, 0, TRUE, TRUE, 0);
        CAM::DESTROY_CAM(g_replayCam, FALSE);
        g_replayCam = made ? fresh : 0;
        g_replayCamRendering = made && g_replayCamRendering;
        NewLensCamera(g_replayLens);
        g_replayCamDof = false;
        g_replayCamEngineFocus = false;
        g_replayCamFocusPausedBefore = -1;
        return made;
    }
}

    // The camera's word at +0xEC (rung 2, the plan B lookup); false when the
    // lookup or the read failed.
    bool ReadCamFlags(Cam cam, std::uint16_t& flags)
    {
        std::uint32_t code = 0;
        const std::uintptr_t object = CamObject(cam, code);
        return object && replay::mem::Read(object + replay::camcut::kCamFlagsOffset, flags);
    }
namespace
{

    // DOF back to Default on the same camera. The first clears of the session read
    // [cam+0x470] back (rung 2): still set means the clear did not take, and the
    // camera is recreated from then on.
    bool DropReplayCamDof(const replay::editor::CameraState& state)
    {
        if (g_camDofClearFailed)
            return ForgetReplayCamDof(state);
        std::uint64_t before = ~0ull, after = ~0ull;
        std::uintptr_t object = 0;
        if (g_camDofClearChecks < 3)
        {
            std::uint32_t code = 0;
            object = CamObject(g_replayCam, code);
            if (object)
                replay::mem::Read(object + kCamDofBlockOffset, before);
        }
        CAM::_0x1FC6C727D30FFDDE(g_replayCam);
        if (object)
        {
            ++g_camDofClearChecks;
            const bool read = replay::mem::Read(object + kCamDofBlockOffset, after);
            const bool took = read && after == 0;
            if (g_camDofLogs.Take(16))
                LogFile() << "CAMERA_DOF,phase=clear_check,block_before=0x" << std::hex << before << ",block_after=0x" << after
                          << std::dec << ",result=" << (took ? "ok" : "not_cleared") << '\n';
            if (!took)
            {
                g_camDofClearFailed = true;
                return ForgetReplayCamDof(state);
            }
        }
        ++g_camDofClears;
        g_replayCamDof = false;
        return true;
    }

    // Out of the engine's autofocus on the same camera: the pause bit back to what
    // it was before the autofocus (read back). Unknown or not taken: the camera is
    // recreated.
    bool LeaveEngineFocus(const replay::editor::CameraState& state)
    {
        if (g_replayCamFocusPausedBefore >= 0)
        {
            const bool paused = g_replayCamFocusPausedBefore != 0;
            CAM::_PAUSE_CAMERA_FOCUS(g_replayCam, paused ? TRUE : FALSE);
            std::uint16_t flags = 0;
            if (ReadCamFlags(g_replayCam, flags) && ((flags & kCamFocusPausedBit) != 0) == paused)
            {
                ++g_camFocusRestores;
                g_replayCamEngineFocus = false;
                return true;
            }
        }
        if (g_camDofLogs.Take(16))
            LogFile() << "CAMERA_DOF,phase=fallback,why=" << (g_replayCamFocusPausedBefore < 0 ? "focus_bit_unknown" : "focus_bit_not_restored")
                      << '\n';
        return ForgetReplayCamDof(state);
    }

    // Near clip and DOF sent to a script camera and read back from the camera
    // object (rung 2, the cut bits lookup CamObject; build 1491.50, no write):
    //   [cam+0x84]  near clip (SET_CAM_NEAR_CLIP 0xF64210 > 0xF753A8: maxss
    //               with 0.01, movss [rbx+0x84]);
    //   [cam+0x470] the DOF block (0xF7583C > 0x4210DC): +0x20 f-stop,
    //               +0x34 lens mm, +0x28 "use the f-stop" (0x176678 takes
    //               +0x20 when set), +0x38 "the lens drives the FOV" (0x428370:
    //               FOV = 2 atan(12 / lens));
    //   [cam+0x54C] focus distance (_SET_CAM_FOCUS_DISTANCE 0xF641E0 > 0xF75258).
    // Each tick reads the value before writing: one that is not what the replay
    // wrote the tick before was changed by the engine in between (counted,
    // logged). The focus distance overwritten three ticks in a row pauses the
    // camera's autofocus (_PAUSE_CAMERA_FOCUS, bit 0x4000 of [cam+0xEC], read
    // back) while a distance is asked.
    constexpr std::uint32_t kCamNearClipOffset = 0x84;
    constexpr std::uint32_t kCamFocusDistanceOffset = 0x54C;
    constexpr std::uint32_t kDofFStopOffset = 0x20;
    constexpr std::uint32_t kDofLensOffset = 0x34;
    constexpr std::uint32_t kDofUseFStopOffset = 0x28;
    constexpr std::uint32_t kDofLensFovOffset = 0x38;
}
    replay::editor::CameraLiveInfo g_cameraLive{};
    replay::log::LogLimit g_camLensLogs;
namespace
{

    bool ReadCamFloat(Cam cam, std::uint32_t offset, float& out)
    {
        std::uint32_t code = 0;
        const std::uintptr_t object = CamObject(cam, code);
        return object && replay::mem::Read(object + offset, out) && std::isfinite(out);
    }

    struct DofReadback
    {
        bool ok = false;
        float fstop = 0.0f;
        float lens = 0.0f;
        std::uint8_t useFStop = 0;
        std::uint8_t lensFov = 0;
        float focus = 0.0f;
        bool focusOk = false;
    };

    DofReadback ReadCamDof(Cam cam)
    {
        DofReadback out;
        std::uint32_t code = 0;
        const std::uintptr_t object = CamObject(cam, code);
        if (!object)
            return out;
        std::uint64_t block = 0;
        if (replay::mem::Read(object + kCamDofBlockOffset, block) && block)
            out.ok = replay::mem::Read(block + kDofFStopOffset, out.fstop) && replay::mem::Read(block + kDofLensOffset, out.lens) &&
                replay::mem::Read(block + kDofUseFStopOffset, out.useFStop) && replay::mem::Read(block + kDofLensFovOffset, out.lensFov);
        out.focusOk = replay::mem::Read(object + kCamFocusDistanceOffset, out.focus);
        return out;
    }
}

    // The near clip on `cam` (held to 0.01..1 m), read back.
    void ApplyNearClip(Cam cam, LensState& lens, float value, const char* which)
    {
        value = std::clamp(std::isfinite(value) ? value : replay::editor::kNearClipDefault, replay::editor::kNearClipMin,
            replay::editor::kNearClipMax);
        float before = 0.0f;
        if (lens.nearSent > 0.0f && ReadCamFloat(cam, kCamNearClipOffset, before) &&
            std::fabs(before - std::max(lens.nearSent, replay::editor::kNearClipMin)) > 1e-4f)
        {
            ++lens.nearOverwrites;
            if (g_camLensLogs.Take(400))
                LogFile() << "CAMERA_NEARCLIP,phase=overwritten,cam=" << which << ",sent=" << lens.nearSent << ",read=" << before
                          << ",overwrites=" << lens.nearOverwrites << '\n';
        }
        CAM::SET_CAM_NEAR_CLIP(cam, value);
        float after = 0.0f;
        const bool read = ReadCamFloat(cam, kCamNearClipOffset, after);
        if (lens.nearSent > 0.0f && std::fabs(value - lens.nearSent) > 1e-4f)
            g_lensChangedAt = GetTickCount64(); // shown through the freeze
        if (std::fabs(value - lens.nearSent) > 1e-4f && g_camLensLogs.Take(400))
            LogFile() << "CAMERA_NEARCLIP,cam=" << which << ",set=" << value << ",read=" << (read ? after : -1.0f)
                      << ",match=" << (read && std::fabs(after - value) < 1e-4f ? 1 : 0) << ",overwrites=" << lens.nearOverwrites
                      << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
        lens.nearSent = value;
        g_cameraLive.nearSet = value;
        g_cameraLive.nearRead = read ? after : 0.0f;
        if (read)
            g_cameraLive.flags |= replay::editor::CameraLiveNearRead;
    }

    // The DOF block the way the game's DEFAULT_SCRIPTED_CAMERA scripts send it
    // (shop_barber, property_use_core: slot 6 on, 7 to 9 off): the f-stop used as
    // given (+0x28), the lens free of the FOV (+0x38 off: the FOV stays the
    // camera's), so the lens can be a focal length multiplier on the FOV's lens.
    // With slots 8 and 9 on (the camera kit's block) the lens drives the FOV, so it
    // must be the FOV's own lens (a 29 mm at FOV 45: f/3.3 keeps almost everything
    // sharp).
    float SendDofBlock(Cam cam, const replay::camera::FocusPlan& focus, float fov)
    {
        const float lens = replay::camera::DofLensMm(fov, focus.intensity, focus.lensMm);
        alignas(8) std::uint8_t block[10 * 8]{};
        const float floats[6] = {focus.fstop, 1.0f, 64.0f, lens, std::min(6.0f, lens), std::max(120.0f, lens)};
        for (int n = 0; n < 6; ++n)
            std::memcpy(block + n * 8, &floats[n], 4);
        const std::int32_t flags[4] = {1, 0, 0, 0};
        for (int n = 0; n < 4; ++n)
            std::memcpy(block + (6 + n) * 8, &flags[n], 4);
        CAM::_SET_CAM_DOF_PARAMS(cam, reinterpret_cast<Any*>(block));
        return lens;
    }

    // The focus distance, after checking whether the engine changed the
    // last one (its autofocus); paused for good on this camera once it did
    // three ticks in a row.
    void SendFocusDistance(Cam cam, LensState& lens, float distance, const char* which)
    {
        float before = 0.0f;
        if (lens.focusSent > 0.0f && ReadCamFloat(cam, kCamFocusDistanceOffset, before))
        {
            if (std::fabs(before - lens.focusSent) > std::max(0.01f, 0.01f * lens.focusSent))
            {
                ++lens.focusOverwrites;
                ++lens.focusOverwriteRun;
                if (g_camLensLogs.Take(400))
                    LogFile() << "CAMERA_DOF,phase=focus_overwritten,cam=" << which << ",sent=" << lens.focusSent << ",read=" << before
                              << ",run=" << lens.focusOverwriteRun << ",overwrites=" << lens.focusOverwrites << '\n';
            }
            else
                lens.focusOverwriteRun = 0;
        }
        if (!lens.pauseForDistance && lens.focusOverwriteRun >= 3)
        {
            lens.pauseForDistance = true;
            if (g_camLensLogs.Take(400))
                LogFile() << "CAMERA_DOF,phase=pause_for_distance,cam=" << which << '\n';
        }
        if (lens.pauseForDistance)
        {
            std::uint16_t flags = 0;
            if (ReadCamFlags(cam, flags) && (flags & kCamFocusPausedBit) == 0)
                CAM::_PAUSE_CAMERA_FOCUS(cam, TRUE);
            g_cameraLive.flags |= replay::editor::CameraLiveFocusPaused;
        }
        CAM::_SET_CAM_FOCUS_DISTANCE(cam, distance);
        lens.focusSent = distance;
        if (lens.focusOverwriteRun)
            g_cameraLive.flags |= replay::editor::CameraLiveFocusOverwritten;
    }

    // What the camera holds now, into the editor's live info and, on a
    // change, the log (requested against read back).
    void NoteDofReadback(Cam cam, const char* which, const replay::camera::FocusPlan& focus, float lens, float focusSent,
        std::uint32_t markerId)
    {
        const DofReadback back = ReadCamDof(cam);
        g_cameraLive.flags |= replay::editor::CameraLiveDofActive | (back.ok ? replay::editor::CameraLiveDofRead : 0u);
        g_cameraLive.focusSource = static_cast<std::uint32_t>(focus.source);
        g_cameraLive.dofFStopSet = focus.fstop;
        g_cameraLive.dofFStopRead = back.ok ? back.fstop : 0.0f;
        g_cameraLive.dofLensSet = lens;
        g_cameraLive.dofLensRead = back.ok ? back.lens : 0.0f;
        g_cameraLive.focusSet = focusSent;
        g_cameraLive.focusRead = back.focusOk ? back.focus : 0.0f;
        replay::camera::InFocusRange(focus.fstop, lens, focusSent > 0.0f ? focusSent : focus.distance, g_cameraLive.inFocusNear,
            g_cameraLive.inFocusFar);
        static std::uint32_t lastMarker = ~0u;
        static replay::camera::FocusSource lastSource = replay::camera::FocusSource::Default;
        static float lastFStop = -1.0f, lastLens = -1.0f, lastFocus = -1.0f;
        static const char* lastWhich = "";
        const bool changed = markerId != lastMarker || focus.source != lastSource || which != lastWhich ||
            std::fabs(focus.fstop - lastFStop) > 0.05f * std::max(0.1f, lastFStop) || std::fabs(lens - lastLens) > 0.5f ||
            std::fabs(focusSent - lastFocus) > 0.1f * std::max(0.2f, lastFocus);
        const int freezeState = PhotoFreezeState();
        if (freezeState == 2 || freezeState == 3)
            g_cameraLive.flags |= replay::editor::CameraLiveFrozen;
        // Any change of the rendered camera's DOF is shown through the freeze, not
        // only the same marker's change: a marker whose DOF was Default would read its
        // first change to Custom as a marker switch and the frozen frame would stay.
        if (changed)
            g_lensChangedAt = GetTickCount64();
        if (!changed)
            return;
        // The last values follow every change (not only the logged ones: a
        // full log cap must not read as a change every tick).
        lastMarker = markerId;
        lastSource = focus.source;
        lastFStop = focus.fstop;
        lastLens = lens;
        lastFocus = focusSent;
        lastWhich = which;
        if (!g_camLensLogs.Take(400))
            return;
        LogFile() << "CAMERA_DOF,phase=readback,cam=" << which << ",marker=" << markerId
                  << ",source=" << replay::camera::FocusSourceName(focus.source) << ",fstop_set=" << focus.fstop
                  << ",fstop_read=" << (back.ok ? back.fstop : -1.0f) << ",lens_set=" << lens << ",lens_read=" << (back.ok ? back.lens : -1.0f)
                  << ",use_fstop=" << static_cast<int>(back.useFStop) << ",lens_fov=" << static_cast<int>(back.lensFov)
                  << ",focus_set=" << focusSent << ",focus_read=" << (back.focusOk ? back.focus : -1.0f)
                  << ",in_focus_near_m=" << g_cameraLive.inFocusNear << ",in_focus_far_m=" << g_cameraLive.inFocusFar
                  << ",block=" << (back.ok ? 1 : 0) << ",freeze_state=" << freezeState << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
    }
namespace
{

    // PlanFocus picks the entity (Target, else the main character; Auto: Look At,
    // attach, main character on screen) and the focus plane depth along the view
    // axis to its head; the distance is sent every frame on an unpaused camera
    // (pausing the focus before _SET_CAM_FOCUS_DISTANCE is not a measured
    // combination); the engine autofocus only when no entity is there. The DOF of
    // the instant is BlendFocus (interpolated inside a blend, hysteresis on the
    // main character, the shot's last distance held) and the distance goes through
    // a spring; the memory starts over on a cut (`cut`). Nothing here makes a
    // camera unless a fallback says so (CAMERA_DOF).
    void ApplyMarkerDof(const replay::camera::SegmentPlan& plan, const replay::editor::CameraState& state,
        std::uint64_t playheadMs, bool cut, float dtSeconds)
    {
        const std::vector<replay::camera::Keyframe>& keys = g_cameraTrack.Keyframes();
        const replay::camera::Keyframe* marker =
            plan.valid && plan.dofIndex >= 0 && plan.dofIndex < static_cast<int>(keys.size()) ? &keys[plan.dofIndex] : nullptr;
        if (cut)
            g_camFocusMemory.ResetShot();
        const replay::camera::BlendedFocus mixed = replay::camera::BlendFocus(g_cameraTrack, plan, state, MarkerScene(),
            playheadMs, &g_camFocusMemory, g_camFocusConfig);
        const replay::camera::FocusPlan focus = mixed.plan;
        const bool blended = mixed.blended;
        if (blended)
            ++g_camDofBlendFrames;
        {
            // Evidence: changes of focus source inside a shot.
            static replay::camera::FocusSource shotSource = replay::camera::FocusSource::Default;
            using replay::camera::FocusSource;
            if (!cut && focus.source != shotSource && shotSource != FocusSource::Default &&
                focus.source != FocusSource::Default)
            {
                ++g_camFocusSourceChanges;
                if ((shotSource == FocusSource::Engine) != (focus.source == FocusSource::Engine))
                    ++g_camFocusEngineFlips;
            }
            shotSource = focus.source;
        }
        if (focus.source == replay::camera::FocusSource::Default)
        {
            g_replayLens.focusSent = -1.0f; // nothing of ours to compare with
            if (g_replayCamDof)
            {
                g_lensChangedAt = GetTickCount64(); // Custom back to Default, shown through the freeze too
                DropReplayCamDof(state);
            }
            return;
        }
        const bool engineFocus = focus.source == replay::camera::FocusSource::Engine;
        if (g_replayCamEngineFocus && !engineFocus)
        {
            if (!LeaveEngineFocus(state))
                return; // recreated next tick, then focused by distance
            if (!g_replayCam)
                return;
        }
        // The shop scripts' block, the lens from the marker (SendDofBlock).
        const float lens = SendDofBlock(g_replayCam, focus, state.fov);
        float sentDistance = focus.distance;
        if (engineFocus)
        {
            if (!g_replayCamEngineFocus)
            {
                std::uint16_t word = 0;
                g_replayCamFocusPausedBefore = ReadCamFlags(g_replayCam, word) ? ((word & kCamFocusPausedBit) ? 1 : 0) : -1;
                CAM::_PAUSE_CAMERA_FOCUS(g_replayCam, FALSE);
            }
            g_replayCamEngineFocus = true;
            g_replayLens.focusSent = -1.0f;
        }
        else if (focus.source != replay::camera::FocusSource::Sharp)
        {
            if (g_camFocusConfig.smoothOmega > 0.0f)
                sentDistance = replay::camera::SmoothFocusDistance(g_camFocusMemory, focus.distance, cut ? 0.0f : dtSeconds,
                    g_camFocusConfig.smoothOmega);
            SendFocusDistance(g_replayCam, g_replayLens, sentDistance, "replay"); // checked, read back
        }
        else
            g_replayLens.focusSent = -1.0f;
        CAM::ALLOW_MOTION_BLUR_DECAY(g_replayCam, FALSE);
        g_replayCamDof = true;
        NoteDofReadback(g_replayCam, "replay", focus, lens, engineFocus || focus.source == replay::camera::FocusSource::Sharp ? 0.0f : sentDistance,
            marker ? marker->id : 0);

        // One line when the source, the entity or the marker changes, or the
        // distance moves more than 10 %, at most every 250 ms; capped.
        static replay::camera::FocusSource lastSource = replay::camera::FocusSource::Default;
        static std::uint32_t lastEntity = ~0u, lastMarker = ~0u;
        static float lastDistance = -1.0f;
        static ULONGLONG lastAt = 0;
        const ULONGLONG now = GetTickCount64();
        const std::uint32_t markerId = marker ? marker->id : 0;
        const bool changed = focus.source != lastSource || focus.entity != lastEntity || markerId != lastMarker;
        const bool moved = lastDistance > 0.0f && std::fabs(focus.distance - lastDistance) > 0.1f * lastDistance && now - lastAt >= 250;
        // Camera evidence (up to 4 lines a second while a clip plays), Debug=1 only.
        if ((changed || moved) && replay::log::Verbose() && g_markerFocusLogs.Take(400))
        {
            LogFile() << "MARKER_FOCUS,marker=" << markerId << ",source=" << replay::camera::FocusSourceName(focus.source)
                      << ",engine_autofocus=" << (engineFocus ? 1 : 0) << ",entity=" << focus.entity
                      << ",focus_depth_m=" << (engineFocus ? 0.0f : focus.distance) << ",sent_m=" << (engineFocus ? 0.0f : sentDistance)
                      << ",target_distance_m=" << focus.straightMeters << ",fstop=" << focus.fstop << ",lens_mm=" << lens
                      << ",blended=" << (blended ? 1 : 0) << ",phase=" << plan.phase
                      << ",intensity=" << (marker ? marker->dofIntensity : 0.0f)
                      << ",focus_mode=" << (marker ? static_cast<int>(marker->focus) : -1)
                      << ",focus_target=" << (marker ? marker->focusTarget : 0)
                      << ",look_at=" << (marker ? marker->lookAtTarget : 0) << ",attach=" << (marker ? marker->attachTarget : 0)
                      << ",missing=" << (focus.targetMissing ? 1 : 0) << ",playhead_ms=" << playheadMs << '\n';
            lastSource = focus.source;
            lastEntity = focus.entity;
            lastMarker = markerId;
            lastDistance = focus.distance;
            lastAt = now;
        }
    }

    // CAMERA_FOLLOW, twice a second while a marker with an attach or Look At target
    // plays (and on each marker change), capped. follow_err_m: the camera against
    // the marker's own attach pose at this time (0 when it follows exactly; a
    // blend or a shake moves it on purpose, see blending and smooth_lag_m). rel:
    // camera minus target in the target's heading frame (constant while an attach
    // Default follows). frame_err: where the Look At target sits on screen against
    // the marker's framing. dt_game is GET_FRAME_TIME, dt_real the frame, dt_clip
    // what the springs took.
    replay::log::LogLimit g_cameraFollowLogs; // per session

    void LogCameraFollow(const replay::camera::SegmentPlan& plan, const replay::editor::CameraState& state,
        std::uint64_t playheadMs, float realDt, float clipDt)
    {
        const std::vector<replay::camera::Keyframe>& keys = g_cameraTrack.Keyframes();
        if (!plan.valid || plan.current < 0 || plan.current >= static_cast<int>(keys.size()) || g_cameraFollowLogs.Seen() >= 400)
            return;
        const replay::camera::Keyframe& marker = keys[plan.current];
        const std::uint32_t target = marker.attachTarget ? marker.attachTarget : marker.lookAtTarget;
        if (!target || marker.cameraMode != replay::camera::CameraMode::Free)
            return;
        static std::uint32_t lastMarker = 0;
        static ULONGLONG lastAt = 0;
        const ULONGLONG now = GetTickCount64();
        if (marker.id == lastMarker && now - lastAt < 500)
            return;
        lastMarker = marker.id;
        lastAt = now;
        (void)g_cameraFollowLogs.Take(400);
        const replay::camera::Scene scene = MarkerScene();
        replay::camera::EntityState entity{};
        bool boneMissing = false; // the attach (else Look At) target through its bone
        const bool found = marker.attachTarget ? replay::camera::LookupAttach(marker, playheadMs, scene, entity, &boneMissing)
                                               : replay::camera::LookupLookAt(marker, playheadMs, scene, entity, &boneMissing);
        const replay::editor::Vec3 aim = entity.hasHead ? entity.head
            : replay::editor::Vec3{entity.position.x, entity.position.y, entity.position.z + replay::camera::kLookAtAimHeight};
        const float ax = aim.x - state.position.x, ay = aim.y - state.position.y, az = aim.z - state.position.z;
        const replay::editor::Vec3 rel = replay::camera::ToHeadingFrame(
            {state.position.x - entity.position.x, state.position.y - entity.position.y, state.position.z - entity.position.z},
            entity.heading);
        const replay::camera::LivePose own = replay::camera::Resolve(marker, playheadMs, scene);
        const float ex = state.position.x - own.pose.position.x, ey = state.position.y - own.pose.position.y,
                    ez = state.position.z - own.pose.position.z;
        float frameErr = -1.0f;
        if (found && marker.lookAtTarget)
        {
            replay::editor::Vec2 screen{};
            if (replay::camera::ProjectToScreen(state, aim, scene.aspect, screen))
                frameErr = std::sqrt((screen.x - marker.lookAtOffset.x) * (screen.x - marker.lookAtOffset.x) +
                    (screen.y - marker.lookAtOffset.y) * (screen.y - marker.lookAtOffset.y));
        }
        LogFile() << "CAMERA_FOLLOW,marker=" << marker.id << ",target=" << target << ",found=" << (found ? 1 : 0)
                  << ",attach=" << marker.attachTarget << ",attach_type=" << static_cast<int>(marker.attachType)
                  << ",look_at=" << marker.lookAtTarget << ",attach_bone=0x" << std::hex << marker.attachBone
                  << ",look_bone=0x" << marker.lookAtBone << std::dec << ",bone_missing=" << (boneMissing ? 1 : 0)
                  << ",blend=" << static_cast<int>(marker.blend)
                  << ",blending=" << (plan.blending ? 1 : 0) << ",smooth=" << (plan.smooth ? 1 : 0)
                  << ",smooth_anchor=" << plan.smoothAnchor << ",smooth_lag_m=" << plan.smoothLagMeters
                  << ",follow_err_m=" << std::sqrt(ex * ex + ey * ey + ez * ez)
                  << ",target_m=" << std::sqrt(ax * ax + ay * ay + az * az)
                  << ",rel=" << rel.x << '/' << rel.y << '/' << rel.z << ",frame_err=" << frameErr
                  << ",speed_pct=" << static_cast<int>(MarkerSpeedAt(playheadMs) * 100.0 + 0.5)
                  << ",rate=" << g_transport.Rate() << ",time_scale=" << g_worldTimeScaleApplied
                  << ",dt_game=" << MISC::GET_FRAME_TIME() << ",dt_real=" << realDt << ",dt_clip=" << clipDt
                  << ",shake=" << static_cast<int>(replay::camera::mshake::SummaryType(replay::camera::mshake::HeldAt(g_cameraTrack, playheadMs)))
                  << ",playhead_ms=" << playheadMs << '\n';
    }

    // Once a tick: the final rendered frame's cut bits (rung 2, guarded
    // read at the address the signature of 0x174AE8 gives), one
    // CAMERA_CUT_RATE line a second while a clip is open (every 10 s
    // outside it, the game's own baseline).
    std::uintptr_t g_camCutFlags = 0; // the final frame's flags (camcut::kCameraFinalFrameFlags)
    bool g_camCutFlagsChecked = false;
    replay::camcut::Rate g_camCutRate;
    std::uint32_t g_camCutGameSeconds = 0;
}

    void UpdateCameraCutRate(ULONGLONG now)
    {
        if (!g_camCutFlagsChecked)
        {
            g_camCutFlagsChecked = true;
            // Where the final frame's cut test reads the flags.
            g_camCutFlags = replay::sig::Find(replay::camcut::kCameraFinalFrameFlags);
            LogFile() << "CAMERA_CUT_MODE,phase=config,mode=" << replay::camcut::ModeName(replay::camcut::Mode::Bits)
                      << ",rate_probe=" << (g_camCutFlags ? "ok" : "signature_mismatch") << ",flags_rva=0x" << std::hex
                      << (g_camCutFlags ? g_camCutFlags - replay::sig::ImageBase() : 0) << std::dec << '\n';
        }
        const std::uint32_t placed = g_camPlacedCutsThisTick;
        g_camPlacedCutsThisTick = 0;
        if (!g_camCutFlags)
            return;
        std::uint16_t flags = 0;
        if (!replay::mem::Read(g_camCutFlags, flags))
            return;
        if (!g_camCutRate.Add(now, (flags & replay::camcut::kCutBits) != 0, placed != 0))
            return;
        const std::uint32_t bitsCleared = g_camBitsCleared, bitsBefore = g_camBitsSeenBefore;
        g_camBitsCleared = 0;
        g_camBitsSeenBefore = 0;
        const bool open = g_transport.Playing();
        if (!open && ++g_camCutGameSeconds % 10 != 0)
            return;
        const char* mode = !open ? "game" : g_freeCamEnabled ? "freecam" : g_transport.Paused() ? "pause" : "play";
        const char* cutMode = g_camBitsFailed ? "legacy_fallback" : replay::camcut::ModeName(replay::camcut::Mode::Bits);
        LogFile() << "CAMERA_CUT_RATE,mode=" << mode << ",cut_mode=" << cutMode
                  << ",frames=" << g_camCutRate.frames << ",cut_frames=" << g_camCutRate.cuts
                  << ",placed_cuts=" << g_camCutRate.placedCuts << ",bits_cleared=" << bitsCleared
                  << ",bits_left_before=" << bitsBefore << ",dof_recreations=" << g_camDofRecreations
                  // Session totals, but the target drift (this second).
                  << ",dof_clears=" << g_camDofClears << ",focus_restores=" << g_camFocusRestores
                  << ",dof_blend_frames=" << g_camDofBlendFrames << ",focus_engine_flips=" << g_camFocusEngineFlips
                  << ",focus_source_changes=" << g_camFocusSourceChanges << ",target_recorded=" << g_camTargetRecordedHits
                  << ",target_live=" << g_camTargetLiveHits << ",target_drift_max_m=" << g_camTargetDriftMaxM
                  << ",target_drift_over_5cm=" << g_camTargetDriftOver5cm << ",smooth_reframes=" << g_cameraSpring.reframes
                  << ",smooth_resets=" << g_cameraSpring.resets << ",prefetch_started=" << g_camPrefetchStarted
                  << ",prefetch_loaded_at_cut=" << g_camPrefetchLoadedAtCut
                  << ",prefetch_not_loaded_at_cut=" << g_camPrefetchNotLoadedAtCut
                  << ",prefetch_cancelled=" << g_camPrefetchCancelled
                  // Bone lookups (this second) and engine overwrites (session).
                  << ",bone_recorded=" << g_camBoneRecorded << ",bone_live=" << g_camBoneLive << ",bone_missing=" << g_camBoneMissing
                  << ",near_overwrites=" << (g_replayLens.nearOverwrites + g_freeLens.nearOverwrites)
                  << ",focus_overwrites=" << (g_replayLens.focusOverwrites + g_freeLens.focusOverwrites)
                  << ",playhead_ms=" << (open ? g_transport.PlayheadMs() : 0) << '\n';
        g_camTargetDriftMaxM = 0.0f;
        g_camTargetDriftOver5cm = 0;
        g_camBoneRecorded = g_camBoneLive = g_camBoneMissing = 0;
    }
namespace
{

    void UpdateCamPrefetch(bool keyed, const replay::camera::SegmentPlan& plan, const replay::editor::CameraState& camera,
        std::uint64_t playheadMs, bool normalPlay)
    {
        const bool busy = g_clipWorld.loadSceneActive || g_exitLoad.active || g_settle.Active() || ClipLoadWaiting();
        const ULONGLONG now = GetTickCount64();
        if (g_camPrefetch.active)
        {
            if (busy || !normalPlay || !keyed)
            {
                StopCamPrefetch(busy ? "load_busy" : !normalPlay ? "not_playing" : "no_markers");
                return;
            }
            const bool loaded = STREAMING::IS_LOAD_SCENE_LOADED() != FALSE;
            if (!g_camPrefetch.cutSeen && playheadMs >= g_camPrefetch.cutMs)
            {
                g_camPrefetch.cutSeen = true;
                g_camPrefetch.cutAt = now;
                ++(loaded ? g_camPrefetchLoadedAtCut : g_camPrefetchNotLoadedAtCut);
                if (g_camPrefetchLogs.Take(200))
                    LogFile() << "CAMERA_PREFETCH,phase=cut,cut_ms=" << g_camPrefetch.cutMs << ",loaded=" << (loaded ? 1 : 0)
                              << ",lead_real_ms=" << (now - g_camPrefetch.startedAt) << ",meters=" << g_camPrefetch.meters
                              << ",rendered_scene_loaded=" << (STREAMING::IS_RENDERED_SCENE_LOADED() ? 1 : 0)
                              << ",playhead_ms=" << playheadMs << '\n';
            }
            if (g_camPrefetch.cutSeen && (loaded || now - g_camPrefetch.cutAt > 2000))
                StopCamPrefetch(loaded ? "loaded" : "timeout");
            return;
        }
        if (!normalPlay)
            g_camPrefetchDoneCutMs = ~0ull; // a seek or a pause: the same cut may be served again
        if (busy || !normalPlay || !keyed || STREAMING::IS_LOAD_SCENE_ACTIVE())
            return;
        const replay::camera::PrefetchTarget target = replay::camera::PlanPrefetch(g_cameraTrack, plan, playheadMs, camera,
            MarkerScene(), replay::camera::kPrefetchLeadMs, replay::camera::kPrefetchMinMeters);
        if (!target.want || target.cutMs == g_camPrefetchDoneCutMs)
            return;
        const float pitch = target.view.rotation.x * 3.14159265f / 180.0f;
        const float heading = target.view.rotation.z * 3.14159265f / 180.0f;
        g_camPrefetchDoneCutMs = target.cutMs;
        if (!STREAMING::LOAD_SCENE_START(target.view.position.x, target.view.position.y, target.view.position.z,
                -std::sin(heading) * std::cos(pitch), std::cos(heading) * std::cos(pitch), std::sin(pitch), 500.0f, 4))
        {
            if (g_camPrefetchLogs.Take(200))
                LogFile() << "CAMERA_PREFETCH,phase=refused,cut_ms=" << target.cutMs << ",meters=" << target.meters
                          << ",playhead_ms=" << playheadMs << '\n';
            return;
        }
        g_camPrefetch.active = true;
        g_camPrefetch.cutMs = target.cutMs;
        g_camPrefetch.meters = target.meters;
        g_camPrefetch.startedAt = now;
        ++g_camPrefetchStarted;
        if (g_camPrefetchLogs.Take(200))
            LogFile() << "CAMERA_PREFETCH,phase=start,cut_ms=" << target.cutMs << ",meters=" << target.meters
                      << ",at=" << target.view.position.x << '/' << target.view.position.y << '/' << target.view.position.z
                      << ",playhead_ms=" << playheadMs << '\n';
    }

    // The detector for the corner at a marker. Each time normal play crosses a
    // marker inside a move (the segment before and after both blend), one
    // CAMERA_MARKER_PASS line: the camera's speed (metres per clip second) on the
    // tick before and the tick after the marker, the jump between them and how far
    // the direction turned. A Linear chain turns a corner there; a Spline one
    // should not.
    replay::log::LogLimit g_markerPassLogs;
    replay::log::LogLimit g_cameraFxLogs; // CAMERA_FX

    void DetectMarkerPass(bool keyed, const replay::camera::SegmentPlan& plan, const replay::editor::CameraState& state,
        std::uint64_t playheadMs, bool normalPlay)
    {
        static replay::editor::Vec3 lastPosition{};
        static std::uint64_t lastPlayhead = ~0ull;
        static int lastCurrent = -1;
        static bool lastBlending = false;
        static float lastSpeed = 0.0f;
        static replay::editor::Vec3 lastDirection{};
        static bool pending = false;
        static std::uint32_t pendingMarker = 0;
        static float pendingSpeed = 0.0f;
        static replay::editor::Vec3 pendingDirection{};
        static int pendingBlend = 0;
        if (!keyed || !normalPlay || lastPlayhead == ~0ull || playheadMs <= lastPlayhead || playheadMs - lastPlayhead > 200)
        {
            pending = false;
            lastSpeed = 0.0f;
        }
        else
        {
            const float clipDt = static_cast<float>(playheadMs - lastPlayhead) / 1000.0f;
            const float dx = state.position.x - lastPosition.x, dy = state.position.y - lastPosition.y,
                        dz = state.position.z - lastPosition.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float speed = distance / clipDt;
            const replay::editor::Vec3 direction = distance > 1e-4f ? replay::editor::Vec3{dx / distance, dy / distance, dz / distance}
                                                                    : replay::editor::Vec3{};
            if (pending)
            {
                pending = false;
                const float dot = direction.x * pendingDirection.x + direction.y * pendingDirection.y + direction.z * pendingDirection.z;
                const float turn = std::acos(std::clamp(dot, -1.0f, 1.0f)) * 57.29578f;
                const float jump = std::max(speed, pendingSpeed) > 0.05f
                    ? std::fabs(speed - pendingSpeed) / std::max(speed, pendingSpeed) * 100.0f : 0.0f;
                // The marker's Spline pacing and the weight in force.
                replay::editor::SplineTuning tuning = g_splineDefaults;
                if (const replay::camera::Keyframe* key = g_cameraTrack.Find(pendingMarker))
                    tuning = replay::camera::spline::Effective(key->spline, g_splineDefaults);
                if (g_markerPassLogs.Take(400))
                    LogFile() << "CAMERA_MARKER_PASS,marker=" << pendingMarker << ",blend=" << pendingBlend << ",speed_in=" << pendingSpeed
                              << ",speed_out=" << speed << ",jump_pct=" << jump << ",turn_deg=" << turn
                              << ",pacing=" << replay::camera::spline::PacingName(static_cast<replay::editor::SplinePacing>(tuning.pacing))
                              << ",ease=" << tuning.easeIn << '/' << tuning.easeOut
                              << ",weight=" << (SplineWeightActive() ? tuning.weight : 0.0f) << ",playhead_ms=" << playheadMs << '\n';
            }
            if (plan.valid && plan.current >= 0 && plan.current != lastCurrent && lastCurrent >= 0 && lastBlending && plan.blending &&
                plan.current < static_cast<int>(g_cameraTrack.Keyframes().size()))
            {
                pending = true;
                pendingMarker = g_cameraTrack.Keyframes()[plan.current].id;
                pendingSpeed = lastSpeed;
                pendingDirection = lastDirection;
                pendingBlend = static_cast<int>(g_cameraTrack.Keyframes()[plan.current].blend);
            }
            lastSpeed = speed;
            lastDirection = direction;
        }
        lastPosition = state.position;
        lastPlayhead = playheadMs;
        lastCurrent = plan.valid ? plan.current : -1;
        lastBlending = plan.valid && plan.blending;
    }
}

    void StopReplayCamera()
    {
        if (g_replayCam && CAM::DOES_CAM_EXIST(g_replayCam))
        {
            if (g_replayCamRendering && !g_freeCamEnabled)
                CAM::RENDER_SCRIPT_CAMS(FALSE, FALSE, 0, TRUE, TRUE, 0);
            CAM::DESTROY_CAM(g_replayCam, FALSE);
        }
        // The focus stays set while a clip plays (another camera takes it this
        // tick); StopPlayback clears it.
        if (g_replayCam && !g_transport.Playing())
            STREAMING::CLEAR_FOCUS();
        g_renderedCamera.valid = false;
        if (!g_freeCamEnabled)
            g_lastPlacedView.valid = false;
        g_replayCam = 0;
        g_replayCamRendering = false;
        g_replayCamDof = false;
        g_replayCamEngineFocus = false;
        g_replayCamFocusPausedBefore = -1;
        StopCamPrefetch("camera_stopped");
        ForgetCamTargets();
    }

    // Once per tick after the playhead moved. The freecam, when on, owns the
    // view; turning it off hands the view back here on the next tick.
    void UpdateReplayCamera()
    {
        REPLAY_PROFILE_ZONE("tick.replaycam");
        if (!g_transport.Playing())
        {
            StopReplayCamera();
            return;
        }
        if (g_freeCamEnabled)
        {
            g_replayCamRendering = false;
            g_replayCamCut = {}; // back from the freecam: a cut
            g_renderedCamera.valid = false; // the freecam renders
            return;
        }
        replay::editor::CameraState state{};
        const std::uint64_t playheadMs = g_transport.PlayheadMs();
        // The real frame time (the performance counter; the game's GET_FRAME_TIME
        // follows SET_TIME_SCALE, 0.05 under a 5 % marker) and the clip time it played
        // (real x the playhead rate). The Smooth springs take the clip time: the same
        // rounding at every marker speed. The motion blur takes the real time: it is
        // how fast the picture moves on screen.
        static std::uint64_t lastTick = 0;
        const std::uint64_t tickNow = replay::prof::Now();
        float realDt = lastTick && tickNow > lastTick
            ? static_cast<float>(static_cast<double>(tickNow - lastTick) / static_cast<double>(replay::prof::TicksPerSecond()))
            : 0.0f;
        lastTick = tickNow;
        realDt = std::min(realDt, 0.2f); // a stalled frame: the transport's kMaxNormalStepMs
        // The Smooth blend's springs only run in normal forward play:
        // paused, scrubbing or stepping, dt 0 resets them.
        const bool normalPlay = !g_transport.Paused() && replay::playback::NormalPlay();
        const float dt = normalPlay ? replay::camera::ClipStepSeconds(realDt, g_transport.Rate()) : 0.0f;
        const float blurDt = normalPlay ? realDt : 0.0f;
        replay::camera::SegmentPlan plan;
        const bool keyed = g_cameraTrackEnabled && EvaluateCameraTrack(playheadMs, state, dt, &plan);
        if (!keyed && !replay::camera::EvaluateGame(g_clip.gameCamera, playheadMs, state, &g_clip.cuts))
        {
            StopReplayCamera(); // a clip without a game camera: the live game camera
            return;
        }
        if (!g_replayCam || !CAM::DOES_CAM_EXIST(g_replayCam))
        {
            g_replayCam = CAM::CREATE_CAM_WITH_PARAMS("DEFAULT_SCRIPTED_CAMERA",
                state.position.x, state.position.y, state.position.z,
                state.rotation.x, state.rotation.y, state.rotation.z, state.fov, FALSE, 2);
            g_replayCamRendering = false;
            NewLensCamera(g_replayLens);
            g_replayCamDof = false;               // a fresh camera has no block
            g_replayCamEngineFocus = false;
            g_replayCamFocusPausedBefore = -1;
            if (!g_replayCam || !CAM::DOES_CAM_EXIST(g_replayCam))
            {
                g_replayCam = 0;
                return;
            }
            LogFile() << "PLAYBACK_CAMERA,created=1,samples=" << g_clip.gameCamera.size()
                      << ",keyframes=" << g_cameraTrack.Keyframes().size() << '\n';
        }
        // Another mod's script camera renders (camera_cut.h LendView): the replay lends
        // it the view (no placing, no activation, no focus) and takes it back when that
        // camera stops or the replay plays.
        {
            const Cam rendering = CAM::GET_RENDERING_CAM();
            const bool other = rendering != 0 && rendering != -1 && rendering != g_replayCam &&
                (!g_freeCam || rendering != g_freeCam);
            const bool lend = replay::camcut::LendView(kCamRelease, g_transport.Paused(), other);
            if (lend)
            {
                if (!g_camLent)
                {
                    g_camLent = true;
                    ++g_camLends;
                    // The focus follows the lent camera (UpdateWorldFocus); clearing it
                    // would hand the world to the hidden live player, maybe far away.
                    LogFile() << "CAMERA_RELEASE,phase=lend,focus=follows"
                              << ",mode=" << replay::camcut::ReleaseName(kCamRelease)
                              << ",cam=" << rendering << ",paused=" << (g_transport.Paused() ? 1 : 0) << ",playhead_ms=" << playheadMs
                              << ",lends=" << g_camLends << '\n';
                }
                g_replayCamRendering = false; // activated and rendered again when taken back
                g_replayCamCut = {};          // and placed with a cut
                return;
            }
            if (g_camLent)
            {
                g_camLent = false;
                LogFile() << "CAMERA_RELEASE,phase=take_back,other_rendering=" << (other ? 1 : 0)
                          << ",paused=" << (g_transport.Paused() ? 1 : 0) << ",playhead_ms=" << playheadMs << '\n';
            }
        }
        // The rendered camera is the load scene's target. A jump's camera test (in the
        // jump's own tick): the camera moved more than 100 m, or more than 1 m onto
        // ground whose collision is not loaded, makes the jump far: the settle freezes
        // the picture (no black screen) and the load scene streams the new place (the
        // "jump" wait). A frame step never tests the camera.
        {
            const replay::clipload::Vec3 position{state.position.x, state.position.y, state.position.z};
            if (g_settleCameraCheck)
            {
                g_settleCameraCheck = false;
                if (g_clipWorld.active && g_settle.Active() && g_settleCameraFromValid)
                {
                    replay::settle::JumpFacts facts;
                    facts.windowCentreMs = facts.targetMs = playheadMs; // the window was tested at the jump
                    facts.cameraValid = true;
                    facts.cameraMovedMeters = std::sqrt(replay::clipload::DistanceSq(g_settleCameraFrom, position));
                    if (facts.cameraMovedMeters > g_settleConfig.collisionCheckMeters)
                    {
                        STREAMING::REQUEST_COLLISION_AT_COORD(position.x, position.y, position.z);
                        facts.collisionLoaded = ENTITY::HAS_COLLISION_LOADED_AROUND_POSITION(position.x, position.y, position.z) != FALSE;
                    }
                    const std::uint32_t bits = replay::settle::FarBitsOf(facts, g_settleConfig);
                    if (bits)
                    {
                        const bool wasFar = g_settle.Far();
                        g_settle.AddFar(bits);
                        if (!wasFar)
                            ++g_seekLoadSequence;
                        g_clipWorld.jumpMeters = facts.cameraMovedMeters;
                        if (!ClipLoadWaiting())
                        {
                            BeginClipLoadWait("jump", GetTickCount64());
                            g_settleJumpScene = true;
                        }
                        if (g_jumpLoadLogs.Take(kJumpLoadLogCap))
                        {
                            LogFile() << "JUMP_LOAD,phase=camera,reason=" << g_settle.Reason() << ",far="
                                      << replay::settle::FarReasons(g_settle.FarBits()) << ",camera_m=" << facts.cameraMovedMeters
                                      << ",collision=" << (facts.collisionLoaded ? 1 : 0) << ",target_ms=" << playheadMs << '\n';
                        }
                    }
                }
            }
            g_renderedCamera.valid = true;
            g_renderedCamera.position = position;
            g_renderedCamera.rotation = {state.rotation.x, state.rotation.y, state.rotation.z};
            g_renderedCamera.playheadMs = playheadMs;
        }
        // A script camera brings the game's cinematic letterbox; the replay's view has
        // none.
        CAM::SET_WIDESCREEN_BORDERS(FALSE, 0);
        bool placedCut = false;
        {
            // A cut only on a real one (camera_cut.h): the playhead jumped (seek, scrub,
            // frame step), the settle or the load wait holds the picture, the marker
            // changed without a blend, the view switched between markers and the game
            // camera, or a recorded cut was crossed. Paused, the frame converges.
            static std::uint64_t lastPlayhead = ~0ull;
            static replay::camcut::MarkerFrame lastMarker;
            static bool lastKeyed = false;
            bool forceCut = lastPlayhead == ~0ull;
            if (!normalPlay && playheadMs != lastPlayhead)
                forceCut = true;
            if (g_settle.Active() || ClipLoadWaiting())
                forceCut = true;
            if (keyed != lastKeyed)
                forceCut = true;
            // Cut when the frame before was not blending into the marker now current
            // (camera_cut.h MarkerBoundaryCut).
            const replay::camcut::MarkerFrame marker = MarkerFrameOf(keyed, plan);
            if (replay::camcut::MarkerBoundaryCut(lastMarker, marker))
                forceCut = true;
            if (normalPlay && lastPlayhead != ~0ull && playheadMs > lastPlayhead)
                for (const std::uint32_t c : g_clip.cuts)
                    if (c > lastPlayhead && c <= playheadMs)
                        forceCut = true;
            lastPlayhead = playheadMs;
            lastMarker = marker;
            lastKeyed = keyed;
            PlaceCam(g_replayCam, g_replayCamCut, "replay", state.position.x, state.position.y, state.position.z,
                state.rotation.x, state.rotation.y, state.rotation.z, state.fov, realDt, forceCut);
            placedCut = g_lastPlaceWhy != replay::camcut::kWhyNone;
        }
        // Streaming ahead of a cut far away.
        UpdateCamPrefetch(keyed, plan, state, playheadMs, normalPlay);
        // The markers' near clip (default 0.1), read back. The live info starts over
        // each tick (the rendered camera fills it).
        g_cameraLive = {};
        ApplyNearClip(g_replayCam, g_replayLens, keyed ? replay::camera::NearClipAt(g_cameraTrack, plan) : replay::editor::kNearClipDefault,
            "replay");
        {
            // Motion blur from the camera's own speed: still is sharp, a fast move smears,
            // as a real shutter does. Only in normal play (a scrub or a jump is not
            // movement).
            static replay::editor::Vec3 lastPosition{};
            static float blur = 0.0f;
            // Per real second (blurDt): the game's frame time under a 5 % marker would make
            // a slow camera count 20 times faster and smear the slow motion.
            float target = 0.0f;
            if (blurDt > 0.0f)
            {
                const float dx = state.position.x - lastPosition.x, dy = state.position.y - lastPosition.y,
                            dz = state.position.z - lastPosition.z;
                const float speedMs = std::sqrt(dx * dx + dy * dy + dz * dz) / blurDt;
                target = speedMs < 50.0f ? std::clamp((speedMs - 1.0f) / 15.0f, 0.0f, 0.6f) : 0.0f; // a cut is not speed
            }
            blur += (target - blur) * std::min(1.0f, blurDt * 8.0f);
            lastPosition = state.position;
            g_replayCamBlur = blurDt > 0.0f ? blur : 0.0f;
        }
        if (keyed)
            LogCameraFollow(plan, state, playheadMs, realDt, dt);
        DetectMarkerPass(keyed, plan, state, playheadMs, normalPlay);
        {
            // The shake marker in force, the markers' shake folded to the playhead
            // (CAMERA_SHAKE, evidence).
            static int lastShakeMarker = -2;
            const replay::camera::mshake::Resolved r = keyed ? replay::camera::mshake::Resolve(g_cameraTrack, playheadMs)
                                                             : replay::camera::mshake::Resolved{};
            if (r.marker != lastShakeMarker)
            {
                lastShakeMarker = r.marker;
                const std::uint32_t id = r.marker >= 0 ? g_cameraTrack.Keyframes()[r.marker].id : 0u;
                if (g_cameraFxLogs.Take(400))
                    LogFile() << "CAMERA_SHAKE,action=in_force,marker=" << id << ",active=" << (r.active ? 1 : 0)
                              << ",profile=" << replay::camera::fx::ShakeName(static_cast<replay::editor::ShakeType>(r.profile))
                              << ",gain=" << r.gain << ",amp=" << r.custom.amp
                              << ",hz=" << r.custom.hz << ",spdamp=" << r.custom.speedAmp << ",spdfreq=" << r.custom.speedHz
                              << ",rot=" << r.custom.rotWeight << ",pos=" << r.custom.posWeight
                              << ",still=" << static_cast<int>(r.custom.still) << ",seed=" << r.custom.seed
                              << ",blending=" << (r.blending ? 1 : 0) << ",cycles=" << r.cycles << ",playhead_ms=" << playheadMs << '\n';
            }
        }
        ApplyMarkerDof(keyed ? plan : replay::camera::SegmentPlan{}, state, playheadMs, placedCut, dt);
        if (!g_replayCam)
            return; // recreated next tick without the DOF block
        CAM::SET_CAM_MOTION_BLUR_STRENGTH(g_replayCam, g_replayCamBlur); // a camera ApplyMarkerDof replaced
        // The world streams around the camera, not around the player left where the
        // playback started. The focus is set once per tick for every camera by
        // UpdateWorldFocus.
        g_replayCamPlacedThisTick = true;
        g_replayCamKeyedThisTick = keyed;
        if (!g_replayCamRendering)
        {
            CAM::SET_CAM_ACTIVE(g_replayCam, TRUE);
            CAM::RENDER_SCRIPT_CAMS(TRUE, FALSE, 0, TRUE, TRUE, 0);
            g_replayCamRendering = true;
        }
    }

    // Once per tick after UpdateFreeCam and UpdateReplayCamera (world_focus_core.h).
    // While a clip plays (the replay owns the world) the streaming focus is put on
    // the camera that renders, whatever it is: the replay camera, the free camera,
    // Edit Camera, another mod's camera the view was lent to, or the live game
    // camera. Otherwise the focus would stay on the hidden live player, maybe
    // hundreds of metres away, and the puppets' hooves would never be created.
    // Rung 1 (natives). FOCUS on each owner change, FOCUS_SAMPLE once per second
    // (focus to camera distance, also with WorldFocus=0).
    void UpdateWorldFocus(ULONGLONG now)
    {
        using namespace replay::worldfocus;
        Inputs in;
        in.playing = g_transport.Playing();
        in.renderingCam = in.playing ? CAM::GET_RENDERING_CAM() : 0;
        in.replayCam = (g_replayCam && CAM::DOES_CAM_EXIST(g_replayCam)) ? g_replayCam : 0;
        in.freeCam = (g_freeCam && CAM::DOES_CAM_EXIST(g_freeCam)) ? g_freeCam : 0;
        in.freeCamEnabled = g_freeCamEnabled;
        in.editingMarker = g_editingMarker != 0;
        in.keyed = g_replayCamKeyedThisTick;
        in.replayCamPlaced = g_replayCamPlacedThisTick;
        g_replayCamPlacedThisTick = false;
        g_replayCamKeyedThisTick = false;
        if (!in.playing)
            return;
        const Source source = Choose(in);
        // The live listener put back while the replay camera renders the recorded game
        // camera (game_listener.h): a marker camera counts when it stands within
        // ListenerCamMatchM of the recorded game camera at the playhead.
        {
            float camErrM = -1.0f;
            if (source == Source::Marker || source == Source::Replay)
            {
                replay::editor::CameraState recorded{};
                if (replay::camera::EvaluateGame(g_clip.gameCamera, g_transport.PlayheadMs(), recorded, &g_clip.cuts))
                {
                    const Vector3 live = CAM::GET_FINAL_RENDERED_CAM_COORD();
                    const float rx = g_renderedCamera.valid ? g_renderedCamera.position.x : live.x;
                    const float ry = g_renderedCamera.valid ? g_renderedCamera.position.y : live.y;
                    const float rz = g_renderedCamera.valid ? g_renderedCamera.position.z : live.z;
                    const float dx = rx - recorded.position.x, dy = ry - recorded.position.y, dz = rz - recorded.position.z;
                    camErrM = std::sqrt(dx * dx + dy * dy + dz * dz);
                }
            }
            const bool follows = replay::gamelistener::FollowsRecordedCamera(source == Source::Replay,
                source == Source::Marker, camErrM, replay::gamelistener::CamMatchM());
            replay::gamelistener::Play(g_clip, static_cast<std::uint32_t>(g_transport.PlayheadMs()), follows, LogFile(),
                camErrM, replay::worldfocus::SourceName(source));
        }
        const Vector3 rendered = CAM::GET_FINAL_RENDERED_CAM_COORD();
        Vector3 at = rendered;
        if (source == Source::Replay || source == Source::Marker)
        {
            if (g_renderedCamera.valid)
            {
                at.x = g_renderedCamera.position.x;
                at.y = g_renderedCamera.position.y;
                at.z = g_renderedCamera.position.z;
            }
        }
        else if (source == Source::FreeCam || source == Source::Edit)
        {
            at = g_freeCamPosition;
        }
        STREAMING::SET_FOCUS_POS_AND_VEL(at.x, at.y, at.z, 0.0f, 0.0f, 0.0f);
        g_worldFocusAt = at;
        g_worldFocusTracker.owned = true;
        const Ped player = PLAYER::PLAYER_PED_ID();
        const bool playerExists = player && ENTITY::DOES_ENTITY_EXIST(player);
        const Vector3 playerAt = playerExists ? ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE) : Vector3{};
        auto dist = [](const Vector3& a, const Vector3& b) {
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        const bool changed = Update(g_worldFocusTracker, source);
        const bool sample = SampleDue(g_worldFocusTracker, now);
        if (!changed && !sample)
            return;
        const bool playerIsFocus = playerExists && STREAMING::IS_ENTITY_FOCUS(player) != FALSE;
        std::ofstream& log = LogFile();
        log << (changed ? "FOCUS" : "FOCUS_SAMPLE") << ",source=" << SourceName(source)
            << ",set=1,cam=" << in.renderingCam
            << ",at=" << at.x << ';' << at.y << ';' << at.z
            << ",focus_cam_m=" << dist(g_worldFocusAt, rendered)
            << ",player_cam_m=" << (playerExists ? dist(playerAt, rendered) : -1.0f)
            << ",player_is_focus=" << (playerIsFocus ? 1 : 0)
            << ",lent=" << (g_camLent ? 1 : 0) << ",paused=" << (g_transport.Paused() ? 1 : 0)
            << ",playhead_ms=" << g_transport.PlayheadMs();
        if (changed)
            log << ",changes=" << g_worldFocusTracker.changes;
        log << '\n';
    }

    // The focus the replay set does not outlive the clip.
    void ReleaseWorldFocus(const char* why)
    {
        const bool owned = g_worldFocusTracker.owned;
        if (owned)
            STREAMING::CLEAR_FOCUS();
        if (owned || g_worldFocusTracker.changes)
            LogFile() << "FOCUS,source=none,why=" << why << ",cleared=" << (owned ? 1 : 0)
                      << ",changes=" << g_worldFocusTracker.changes << ",samples=" << g_worldFocusTracker.samples << '\n';
        replay::worldfocus::Reset(g_worldFocusTracker);
        g_replayCamPlacedThisTick = false;
        g_replayCamKeyedThisTick = false;
    }
