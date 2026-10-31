// replay_events_blood_gore.cpp: blood and gore: the clip pointer audit, blood recorded and played back, gore hits and limbs.
// Split from replay_events.cpp (moved code); shared internals in replay_events_internal.h.

#include "replay_events_internal.h"

namespace replay::events
{
    namespace
    {
    const char* BloodRef78Class(std::uint64_t value);
    std::uint32_t g_poseV2BloodQueueTarget = 0;    // playhead the queue was planned for
    std::uint32_t g_poseV2BloodQueueOps = 0;       // ops planned in the current drain
    std::uint32_t g_poseV2BloodQueueTicks = 0;
    double g_poseV2BloodQueueMaxTickMs = 0.0;
    double g_poseV2BloodQueueTotalMs = 0.0;
    std::uint64_t g_poseV2BloodQueueStartQpc = 0;
    std::uint64_t g_poseV2BloodQueueQueries = 0;   // VirtualQuery count at the plan
    double g_poseV2BloodQueueQueryMs = 0.0;
    std::uint32_t g_poseV2BloodApplyCalls = 0;     // engine applies, for the tick line
    }
    replay::entity::ReplayId RecordedIdAtAddress(std::uint64_t address)
    {
        if (!address)
            return {};
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
            if (reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(entity.handle)) == address)
                return entity.id;
        const auto found = [&](replay::entity::ReplayId id, const char* kind) {
            if (g_poseV2ObjectAtAddressLogs < 32)
            {
                ++g_poseV2ObjectAtAddressLogs;
                Ctx().log << "POSE_V2_ID_AT_ADDRESS,kind=" << kind << ",id=" << replay::entity::IdText(id) << ",entity=0x"
                    << std::hex << address << std::dec << ",time_ms=" << replay::pose::capture::NowMs() << '\n';
            }
            return id;
        };
        for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
            if (weapon.object && reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(weapon.object)) == address)
                return found(weapon.id, "weapon");
        for (const replay::recorder::RecordedProp& prop : Ctx().recorder.props)
            if (prop.object && reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(prop.object)) == address)
                return found(prop.id, "prop");
        if (const replay::entity::ReplayId car = replay::trains::RecordedIdAt(address); car.Valid())
            return found(car, "train_car");
        return {};
    }

    // Record +0x78 leaves the clip as a replay id (see BloodEvent).
    std::uint64_t g_poseV2BloodRef78[3] = {}; // null, recorded entity, unknown (dropped)
    namespace
    {
    void TakeBloodRef78(replay::entity::BloodEvent& event)
    {
        std::uint64_t value = 0;
        std::memcpy(&value, event.record + 0x78, sizeof(value));
        event.ref78Entity = RecordedIdAtAddress(value);
        ++g_poseV2BloodRef78[!value ? 0 : event.ref78Entity.Valid() ? 1 : 2];
        std::memset(event.record + 0x78, 0, sizeof(value));
    }
    }

    // The fire descriptor's entity pointers (+0x40, +0x48 kept as ids, +0xC0
    // the culprit, never replayed) leave the clip zeroed; playback writes the
    // puppets back (StartPoseV2Fire). The parent fire (+0xF0) and the probe
    // exclusion (+0xF8) too (fire_observer.h kDescParentFire): a slot address of
    // this session, and the cause of a guarded fault in the fire start when
    // replayed.
    void ZeroFireDescPointers(replay::entity::FireStartEvent& event)
    {
        std::memset(event.desc + replay::fire::kDescEntityA, 0, 0x10);
        std::memset(event.desc + replay::fire::kDescCulprit, 0, 0x8);
        replay::fire::ClearDescParent(event.desc);
    }

    // A fire's entity the recording does not have, kept as the world object it
    // is (model and position; objects only: a ped or vehicle outside the
    // recording has no counterpart in the replay). Without it a fire on a
    // world object (a molotov on a stage deck) starts with no entity and shows
    // nothing.
    std::uint32_t g_poseV2FireWorldEntities = 0;
    std::uint32_t g_poseV2FireWorldFound = 0;
    std::uint32_t g_poseV2FireWorldMissing = 0;
    void TakeFireWorldEntity(std::uint64_t address, replay::entity::ReplayId id, std::uint32_t& model, float pos[3])
    {
        model = 0;
        if (!address || id.Valid())
            return;
        const Entity handle = HandleAtAddress(address);
        if (!handle || !ENTITY::IS_ENTITY_AN_OBJECT(handle))
            return;
        model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(handle));
        const Vector3 at = ENTITY::GET_ENTITY_COORDS(handle, TRUE, FALSE);
        pos[0] = at.x;
        pos[1] = at.y;
        pos[2] = at.z;
        ++g_poseV2FireWorldEntities;
    }

    // Clip-on-disk audit: every raw engine blob the clip keeps, scanned for
    // 8-byte values that point at committed memory (the game image or anything
    // else). A clip saved to disk must carry none of the "other". Evidence only
    // (the clip reader validates what a file may name), so it runs with Debug=1.
    // The stop tick only copies the words; the VirtualQuery classification (the
    // melee and impact pointers land in the game's heap, a run of pages the
    // kernel walks page by page) runs on a worker in ascending address order and
    // the line is written on a later tick (clip_pointer_audit.h, deferred_log.h).
    // audit_us is the worker's time; script_us the copy's.
    void AuditPoseV2ClipPointers()
    {
        if (!replay::log::Verbose())
            return;
        const std::uint64_t auditStart = replay::prof::Now();
        replay::ptraudit::Input input;
        for (const replay::entity::BloodEvent& event : Ctx().clip.blood.Events())
            input.Scan("blood_record", event.record, sizeof(event.record));
        for (const replay::entity::GoreHitEvent& event : Ctx().clip.goreHits.Events())
            input.Scan("gore_context", event.context, sizeof(event.context));
        for (const replay::entity::FireStartEvent& event : Ctx().clip.fireStarts.Events())
        {
            input.Scan("fire_desc", event.desc, sizeof(event.desc));
            input.Scan("fire_rng", reinterpret_cast<const std::uint8_t*>(&event.rng), sizeof(event.rng));
        }
        for (const replay::entity::MeleeEvent& event : Ctx().clip.meleeHits.Events())
        {
            input.ScanValue("melee_info", event.info);
            input.ScanValue("melee_info_vtable", event.infoVtable);
        }
        for (const replay::entity::BodyImpactEvent& event : Ctx().clip.bodyImpacts.Events())
        {
            input.ScanValue("impact_arg4", event.arg4);
            input.ScanValue("impact_arg5", event.arg5);
            input.ScanValue("impact_definition", event.definition);
            input.ScanValue("impact_definition_vtable", event.definitionVtable);
            input.ScanValue("impact_arg7", event.arg7);
            input.ScanValue("impact_arg10", event.arg10);
        }
        const std::uintptr_t imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(imageBase + dos->e_lfanew);
        const std::uintptr_t imageEnd = imageBase + nt->OptionalHeader.SizeOfImage;
        const std::uint64_t scriptUs =
            replay::clock::TicksToUs(replay::prof::Now() - auditStart, replay::prof::TicksPerSecond());
        // The worker: its own copy of the words, VirtualQuery only (no
        // engine memory is read, no native is called).
        replay::deferredlog::Log().Submit([words = std::move(input), imageBase, imageEnd, scriptUs]() {
            const std::uint64_t startedUs = replay::clock::NowUs();
            const auto queryRegion = [](std::uintptr_t address, std::uintptr_t& start, std::uintptr_t& end, bool& committed) {
                MEMORY_BASIC_INFORMATION info{};
                if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info))
                    return false;
                start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
                end = start + info.RegionSize;
                committed = info.State == MEM_COMMIT;
                return true;
            };
            const replay::ptraudit::Result result = replay::ptraudit::Classify(words, imageBase, imageEnd, queryRegion);
            std::ostringstream line;
            line << "POSE_V2_CLIP_POINTER_AUDIT,blobs=" << result.blobs << ",pointers=" << result.pointers
                 << ",queries=" << result.queries << ",region_hits=" << result.regionHits
                 << ",audit_us=" << (replay::clock::NowUs() - startedUs) << ",script_us=" << scriptUs
                 << ",words=" << words.Words().size() << '\n';
            return line.str();
        });
    }

    void RecordPoseV2GoreHits()
    {
        static std::vector<replay::gore::EventCall> calls;
        replay::gore::CollectEventsSince(g_poseV2GoreHitCursor, calls);
        if (calls.empty())
            return;
        const std::uint64_t now = replay::gore::NowUs();
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        for (const replay::gore::EventCall& call : calls)
        {
            std::uint64_t ped = 0;
            std::uint64_t inflictor = 0;
            std::memcpy(&ped, call.context + 0x00, sizeof(ped));
            std::memcpy(&inflictor, call.context + 0x08, sizeof(inflictor));
            replay::entity::GoreHitEvent event;
            event.entity = RecordedIdAtAddress(ped);
            if (!event.entity.Valid())
            {
                ++g_poseV2GoreHitsOther;
                continue;
            }
            event.inflictor = RecordedIdAtAddress(inflictor);
            const std::uint64_t ageMs = now > call.timeUs ? (now - call.timeUs) / 1000 : 0;
            event.timeMs = replay::pose::capture::RecordMsAgo(ageMs, nowMs);
            std::memcpy(event.context, call.context, sizeof(event.context));
            // +0x00 ped and +0x08 inflictor travel as ids; playback writes the
            // puppets there (replay::gore::ReplayEvent).
            std::memset(event.context, 0, 0x10);
            event.result = call.result;
            event.caller = call.caller;
            Ctx().clip.goreHits.Add(event);
            for (const replay::recorder::RecordedEntity& recorded : Ctx().recorder.entities)
                if (recorded.id.Packed() == event.entity.Packed() && replay::entities::Exists(recorded.handle))
                {
                    g_poseV2GorePieceCenter = ENTITY::GET_ENTITY_COORDS(recorded.handle, TRUE, FALSE);
                    g_poseV2GorePieceModel = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(recorded.handle));
                    g_poseV2GorePieceUntilMs = nowMs + 2000;
                    g_poseV2GorePieceOwner = recorded.handle;
                }
            std::uint32_t hash = 0;
            std::uint32_t bone = 0;
            std::uint16_t mask = 0;
            float hit[4] = {};
            std::memcpy(&hash, call.context + 0x40, 4);
            std::memcpy(&bone, call.context + 0x44, 4);
            std::memcpy(&mask, call.context + 0x48, 2);
            std::memcpy(hit, call.context + 0x10, sizeof(hit));
            Ctx().log << "POSE_V2_GORE_HIT,id=" << replay::entity::IdText(event.entity)
                << ",inflictor=" << (event.inflictor.Valid() ? replay::entity::IdText(event.inflictor) : std::string("none"))
                << ",inflictor_ptr=" << (inflictor ? 1 : 0) << ",mask=0x" << std::hex << mask << ",hash=0x" << hash
                << ",bone=0x" << bone << ",b4a=0x" << static_cast<unsigned>(call.context[0x4A]) << ",caller=0x" << call.caller
                << std::dec << ",result=" << static_cast<unsigned>(call.result) << ",hit=" << hit[0] << ";" << hit[1] << ";"
                << hit[2] << ",time_ms=" << event.timeMs << '\n';
        }
    }
    namespace
    {

    // The addresses of the recorded peds and horses as of the last blood
    // collection (RecordPoseV2Blood falls back to it).
    void RefreshPoseV2BloodOwnerCache()
    {
        g_poseV2BloodOwnerCache.clear();
        for (const replay::recorder::RecordedEntity& recorded : Ctx().recorder.entities)
        {
            if (recorded.vehicle || !replay::entities::Exists(recorded.handle) || !replay::entities::IsPed(recorded.handle))
                continue;
            if (void* address = replay::entities::AddressOf(recorded.handle))
                g_poseV2BloodOwnerCache.push_back({reinterpret_cast<std::uint64_t>(address), recorded.id});
        }
    }

    // Evidence for a record whose owner is not recorded (e.g. damage pack
    // records). Which world entity it is: a ped of the pool
    // (model, horse, distance to the player, dead, on fire) or none (not a ped:
    // freed, or another kind).
    void LogPoseV2BloodUnmapped(const replay::blood::Call& call)
    {
        if (g_poseV2BloodUnmappedLogs >= 16)
            return;
        ++g_poseV2BloodUnmappedLogs;
        static int peds[1024];
        const int count = (std::min)(worldGetAllPeds(peds, 1024), 1024);
        Ped found = 0;
        for (int n = 0; n < count && !found; ++n)
            if (peds[n] && reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(peds[n])) == call.entity)
                found = peds[n];
        float pos[4] = {};
        std::memcpy(pos, call.record, sizeof(pos));
        std::int32_t info = 0;
        std::memcpy(&info, call.record + 0x108, sizeof(info));
        Ctx().log << "POSE_V2_BLOOD_UNMAPPED,caller=0x" << std::hex << call.caller << ",entity=0x" << call.entity << std::dec
            << ",slot=" << static_cast<unsigned>(call.slot) << ",info=" << info << ",pack=" << static_cast<unsigned>(call.record[0x83]);
        if (found)
        {
            const Hash model = ENTITY::GET_ENTITY_MODEL(found);
            const Vector3 at = ENTITY::GET_ENTITY_COORDS(found, TRUE, FALSE);
            const Vector3 player = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE, FALSE);
            const float dx = at.x - player.x;
            const float dy = at.y - player.y;
            const float dz = at.z - player.z;
            bool rejected = false;
            for (const Ped ped : Ctx().recorder.rejected)
                rejected = rejected || ped == found;
            Ctx().log << ",ped=" << found << ",model=0x" << std::hex << static_cast<std::uint32_t>(model) << std::dec
                << ",horse=" << (PED::_IS_THIS_MODEL_A_HORSE(model) ? 1 : 0) << ",dist_m=" << std::sqrt(dx * dx + dy * dy + dz * dz)
                << ",dead=" << (ENTITY::IS_ENTITY_DEAD(found) ? 1 : 0) << ",on_fire=" << (FIRE::IS_ENTITY_ON_FIRE(found) ? 1 : 0)
                << ",rejected=" << (rejected ? 1 : 0);
        }
        else
            Ctx().log << ",ped=none";
        Ctx().log << ",recorded=" << Ctx().recorder.entities.size() << ",time_ms=" << replay::pose::capture::NowMs() << '\n';
    }
    }

    // Every ped damage record finalized on a recorded entity (evidence and
    // the data the replay applies).
    void RecordPoseV2Blood()
    {
        static std::vector<replay::blood::Call> calls;
        replay::blood::CollectSince(g_poseV2BloodCursor, calls);
        if (calls.empty())
        {
            RefreshPoseV2BloodOwnerCache();
            return;
        }
        const std::uint64_t now = replay::blood::NowUs();
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        for (const replay::blood::Call& call : calls)
        {
            ++g_poseV2BloodByCaller[call.caller];
            if (!call.copied)
            {
                ++g_poseV2BloodNotCopied;
                continue;
            }
            replay::entity::BloodEvent event;
            event.entity = RecordedIdAtAddress(call.entity);
            // A record finalized on an entity the recorder retired before this
            // collection (the finalize ran in an earlier engine frame, the entity was
            // deleted or evicted since): its address as of the last collection still
            // names it.
            bool fromCache = false;
            if (!event.entity.Valid())
                for (const auto& [address, id] : g_poseV2BloodOwnerCache)
                    if (address == call.entity)
                    {
                        event.entity = id;
                        fromCache = true;
                        ++g_poseV2BloodFromCache;
                        break;
                    }
            if (!event.entity.Valid())
            {
                ++g_poseV2BloodUnmapped;
                LogPoseV2BloodUnmapped(call);
                continue;
            }
            // Ped damage records belong on peds (horses are peds). The engine also
            // finalizes records on a burning wagon; the playback refuses them
            // (ReadyPedPuppet) and writing one crashes, so they do not enter the clip.
            if (!fromCache)
            {
                const replay::recorder::RecordedEntity* owner = nullptr;
                for (const replay::recorder::RecordedEntity& recorded : Ctx().recorder.entities)
                    if (recorded.id.Packed() == event.entity.Packed())
                        owner = &recorded;
                if (!owner || owner->vehicle || !replay::entities::Exists(owner->handle) ||
                    !replay::entities::IsPed(owner->handle))
                {
                    ++g_poseV2BloodNotPed;
                    continue;
                }
            }
            const std::uint64_t ageMs = now > call.timeUs ? (now - call.timeUs) / 1000 : 0;
            event.timeMs = replay::pose::capture::RecordMsAgo(ageMs, nowMs);
            event.slot = call.slot;
            event.caller = call.caller;
            event.index = call.index;
            event.age = call.age;
            std::memcpy(event.record, call.record, sizeof(event.record));
            TakeBloodRef78(event);
            Ctx().clip.blood.Add(event);
            if (g_poseV2BloodLogged < 40)
            {
                ++g_poseV2BloodLogged;
                float pos[4] = {};
                std::uint16_t info = 0;
                std::memcpy(pos, call.record, sizeof(pos));
                std::memcpy(&info, call.record + 0x108, sizeof(info));
                Ctx().log << "POSE_V2_BLOOD,id=" << replay::entity::IdText(event.entity) << ",caller=0x" << std::hex
                    << call.caller << std::dec << ",slot=" << static_cast<unsigned>(call.slot) << ",index=" << call.index
                    << ",age=" << call.age << ",pos=" << pos[0] << ";" << pos[1] << ";" << pos[2] << ",scale=" << pos[3]
                    << ",info=" << info << ",ref78_is_owner=" << (*reinterpret_cast<const std::uint64_t*>(call.record + 0x78) == call.entity)
                    << ",ref78=" << BloodRef78Class(*reinterpret_cast<const std::uint64_t*>(call.record + 0x78))
                    << ",flags81=" << static_cast<unsigned>(call.record[0x81]) << "/"
                    << static_cast<unsigned>(call.record[0x82]) << "/" << static_cast<unsigned>(call.record[0x83])
                    << ",time_ms=" << event.timeMs << '\n';
            }
        }
        RefreshPoseV2BloodOwnerCache();
    }

    // What a ped already carries when it enters the recording: its blood
    // records, the gore bits the native does not report (the head), and severed
    // pieces already lying next to it.
    void RecordPoseV2HeldState(std::uint32_t nowMs)
    {
        static int objects[1024];
        int objectCount = -1;
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (entity.vehicle || !g_poseV2HeldStateTaken.insert(entity.id.Packed()).second)
                continue;
            if (!replay::entities::Exists(entity.handle) || !replay::entities::IsPed(entity.handle))
                continue;
            void* ped = replay::entities::AddressOf(entity.handle);
            std::vector<replay::blood::HeldRecord> records;
            std::string error;
            if (g_poseV2BloodObserverReady && replay::blood::ReadRecords(ped, records, error))
            {
                for (const replay::blood::HeldRecord& held : records)
                {
                    replay::entity::BloodEvent event;
                    event.timeMs = nowMs;
                    event.entity = entity.id;
                    event.caller = held.pack ? replay::blood::CallerPack() : replay::blood::CallerBlood();
                    event.age = held.age;
                    std::memcpy(event.record, held.record, sizeof(event.record));
                    TakeBloodRef78(event);
                    Ctx().clip.blood.Add(event);
                    ++g_poseV2HeldBlood;
                }
            }
            std::uint32_t mask = 0;
            if (replay::gore::ReadGoreMask(ped, mask) && mask)
            {
                for (int bit = 0; bit < 15; ++bit)
                {
                    if (!(mask & (1u << bit)))
                        continue;
                    bool native = false;
                    for (const int limb : replay::gore::kLimbs)
                        if (limb == bit)
                            native = true;
                    if (native)
                        continue; // the per-frame native poll records it
                    replay::entity::GoreEvent event;
                    event.timeMs = nowMs;
                    event.entity = entity.id;
                    event.limb = bit;
                    Ctx().clip.gore.Add(event);
                    ++g_poseV2HeldGoreBits;
                }
            }
            if (mask)
            {
                const Vector3 at = ENTITY::GET_ENTITY_COORDS(entity.handle, TRUE, FALSE);
                // The objects within 3 m from the object pool (handles only for those)
                // in place of worldGetAllObjects, which gives a handle to every object of the
                // world; the listing stays as the fallback.
                static int nearObjects[64];
                const float center[3] = {at.x, at.y, at.z};
                int nearCount = -1;
                {
                    REPLAY_PROFILE_ZONE("rec.gore.held_objects");
                    nearCount = replay::entities::ListNear(replay::pools::Kind::Object, center, 3.0f, nearObjects, 64);
                }
                const int* candidates = nearObjects;
                int candidateCount = nearCount;
                if (nearCount < 0)
                {
                    if (objectCount < 0)
                        objectCount = (std::min)(worldGetAllObjects(objects, 1024), 1024);
                    candidates = objects;
                    candidateCount = objectCount;
                }
                for (int n = 0; n < candidateCount; ++n)
                {
                    const Object object = candidates[n];
                    if (!object || !replay::entities::Exists(object) || ENTITY::IS_ENTITY_ATTACHED(object))
                        continue;
                    const std::uint32_t model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object));
                    if (model != 0x4F6FA8E4u && model != 0x5595880Bu)
                        continue;
                    const Vector3 p = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
                    const float dx = p.x - at.x;
                    const float dy = p.y - at.y;
                    const float dz = p.z - at.z;
                    if (dx * dx + dy * dy + dz * dz > 3.0f * 3.0f)
                        continue;
                    if (replay::objects::RegisterGorePiece(object, entity.handle))
                        ++g_poseV2HeldPieces;
                }
            }
            Ctx().log << "POSE_V2_HELD_STATE,id=" << replay::entity::IdText(entity.id) << ",blood=" << records.size()
                << ",gore_mask=0x" << std::hex << mask << std::dec << ",time_ms=" << nowMs << '\n';
        }
    }
    namespace
    {

    // The game's effective time scale this frame (scaled step / real step),
    // skipped on paused frames (the frame counter does not move).
    int g_poseV2LastFrameCount = -1;
    }
    void RecordPoseV2TimeScale()
    {
        const int frame = MISC::GET_FRAME_COUNT();
        if (frame == g_poseV2LastFrameCount)
            return;
        g_poseV2LastFrameCount = frame;
        const float scaled = MISC::GET_FRAME_TIME();
        const float real = MISC::GET_SYSTEM_TIME_STEP();
        // A hitch (e.g. a record start stall) clips the scaled step at 1/15 s
        // while the real one is long: not a slowdown.
        if (real <= 0.0f || real > 0.1f)
            return;
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        if (Ctx().clip.timeScale.Note(nowMs, scaled / real))
        {
            const replay::logfmt::FormatScope logFormat(Ctx().log); // full digits
            replay::logfmt::DefaultFloat(Ctx().log) << "POSE_V2_TIME_SCALE,scale=" << Ctx().clip.timeScale.Samples().back().scale << ",frame_s=" << scaled
                << ",real_s=" << real << ",time_ms=" << nowMs << '\n';
        }
    }
    std::vector<PendingGore> g_poseV2GorePending;
    // Gore is persistent state, not a one-shot: the limb mask (handler bits,
    // 0 head) each puppet has had applied, and the playhead of the last
    // reconcile.
    std::map<std::uint32_t, std::uint16_t> g_poseV2GoreApplied;
    std::uint32_t g_poseV2GoreLastPlayhead = 0;
    std::uint64_t g_poseV2GoreReconciles = 0;
    // A bit the engine refused right after a clear: retried on the next ticks.
    std::uint32_t g_poseV2GoreRetryTicks = 0;
    namespace
    {

    std::uint16_t HitMask(const replay::entity::GoreHitEvent& event)
    {
        std::uint16_t mask = 0;
        std::memcpy(&mask, event.context + 0x48, sizeof(mask));
        return mask;
    }

    // The mask an entity has lost by timeMs in the recording.
    std::uint16_t PoseV2GoreDesired(std::uint32_t entity, std::uint32_t timeMs)
    {
        std::uint16_t mask = 0;
        for (const replay::entity::GoreHitEvent& event : Ctx().clip.goreHits.Events())
            if (event.timeMs <= timeMs && event.result && event.entity.Packed() == entity)
                mask |= HitMask(event);
        for (const replay::entity::GoreEvent& event : Ctx().clip.gore.Events())
            if (event.timeMs <= timeMs && event.entity.Packed() == entity && event.limb >= 0 && event.limb < 16)
                mask |= static_cast<std::uint16_t>(1u << event.limb);
        return mask;
    }
    constexpr std::uint32_t kPoseV2GorePendingMaxMs = 5000;
    }

    // A ped only. A burning carriage has blood records in the clip; its
    // vehicle has no ped damage index at +0x9C, and the blood clear and slot
    // alloc read a garbage component there (C0000005 under the slot alloc).
    // Blood and gore never go to a non-ped.
    std::uint32_t g_poseV2NotPedLogs = 0;
    namespace
    {
    const replay::playback::Puppet* ReadyPedPuppet(replay::entity::ReplayId id)
    {
        const replay::playback::Puppet* puppet = Ctx().puppets.Find(id);
        if (!puppet || !puppet->clone || !replay::entities::Exists(puppet->clone) ||
            puppet->state != replay::playback::PuppetState::Ready)
        {
            return nullptr;
        }
        if (!replay::entities::IsPed(puppet->clone))
        {
            if (g_poseV2NotPedLogs < 8)
            {
                ++g_poseV2NotPedLogs;
                Ctx().log << "POSE_V2_PLAYBACK_NOT_PED,id=" << replay::entity::IdText(id) << ",model=0x" << std::hex
                    << static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(puppet->clone)) << std::dec << '\n';
            }
            return nullptr;
        }
        return puppet;
    }

    // False when the puppet is not there yet.
    bool PlayPoseV2GoreHitAt(std::size_t index, std::uint32_t timeMs)
    {
        const replay::entity::GoreHitEvent& event = Ctx().clip.goreHits.Events()[index];
        const replay::playback::Puppet* puppet = ReadyPedPuppet(event.entity);
        if (!puppet)
            return false;
        void* inflictor = nullptr;
        if (event.inflictor.Valid())
            if (const replay::playback::Puppet* other = Ctx().puppets.Find(event.inflictor))
                if (other->clone && replay::entities::Exists(other->clone))
                    inflictor = replay::entities::AddressOf(other->clone);
        bool handled = false;
        std::string error;
        replay::objects::ReleaseGorePieces(event.entity, event.timeMs);
        const bool ok = replay::gore::ReplayEvent(event.context, replay::entities::AddressOf(puppet->clone), inflictor, handled, error);
        if (ok && handled)
            g_poseV2GoreApplied[event.entity.Packed()] |= HitMask(event);
        if (ok)
            ++g_poseV2GoreHitsPlayed;
        else
        {
            ++g_poseV2GoreHitsFailed;
            g_poseV2GoreLastError = error;
        }
        Ctx().log << "POSE_V2_PLAYBACK_GORE_HIT,id=" << replay::entity::IdText(event.entity) << ",called=" << ok
            << ",handled=" << handled << ",recorded_result=" << static_cast<unsigned>(event.result)
            << ",inflictor=" << (inflictor ? 1 : 0) << ",error=" << (error.empty() ? "none" : error.c_str())
            << ",event_ms=" << event.timeMs << ",playhead_ms=" << timeMs << '\n';
        return true;
    }

    bool PlayPoseV2GoreLimbAt(std::size_t index, std::uint32_t timeMs)
    {
        const replay::entity::GoreEvent& event = Ctx().clip.gore.Events()[index];
        const replay::playback::Puppet* puppet = ReadyPedPuppet(event.entity);
        if (!puppet)
            return false;
        // The gameplay event replayed before already removed the limb (with
        // its stump and wounds); the mask apply is the fallback.
        if (PED::_HAS_PED_TAKEN_GORE_DAMAGE(puppet->clone, event.limb))
        {
            ++g_poseV2GoreMaskSkipped;
            ++g_poseV2GorePlayed;
            Ctx().log << "POSE_V2_PLAYBACK_GORE,id=" << replay::entity::IdText(event.entity) << ",limb=" << event.limb
                << ",applied=skip_already_set,event_ms=" << event.timeMs << ",playhead_ms=" << timeMs << '\n';
            return true;
        }
        std::string error;
        const bool ok = replay::gore::ApplyLimb(replay::entities::AddressOf(puppet->clone), event.limb, error);
        const bool set = PED::_HAS_PED_TAKEN_GORE_DAMAGE(puppet->clone, event.limb) != FALSE;
        if ((ok || set) && event.limb >= 0 && event.limb < 16)
            g_poseV2GoreApplied[event.entity.Packed()] |= static_cast<std::uint16_t>(1u << event.limb);
        if (ok || set)
            ++g_poseV2GorePlayed;
        else
        {
            ++g_poseV2GoreFailed;
            g_poseV2GoreLastError = error;
        }
        Ctx().log << "POSE_V2_PLAYBACK_GORE,id=" << replay::entity::IdText(event.entity) << ",limb=" << event.limb
            << ",applied=" << ok << ",bit_set=" << set << ",error=" << (error.empty() ? "none" : error.c_str())
            << ",event_ms=" << event.timeMs << ",playhead_ms=" << timeMs << '\n';
        return true;
    }

    void PlayPoseV2GorePending(std::uint32_t timeMs)
    {
        for (std::size_t n = 0; n < g_poseV2GorePending.size();)
        {
            const PendingGore pending = g_poseV2GorePending[n];
            const bool done = pending.hit ? PlayPoseV2GoreHitAt(pending.index, timeMs)
                                          : PlayPoseV2GoreLimbAt(pending.index, timeMs);
            if (done || timeMs > pending.eventMs + kPoseV2GorePendingMaxMs || timeMs + 50 < pending.eventMs)
            {
                if (!done)
                {
                    if (pending.hit)
                        ++g_poseV2GoreHitsNoPuppet;
                    else
                        ++g_poseV2GoreNoPuppet;
                }
                g_poseV2GorePending.erase(g_poseV2GorePending.begin() + static_cast<std::ptrdiff_t>(n));
                continue;
            }
            ++n;
        }
    }

    // After a jump, rewind or scrub: every puppet gets the gore of the
    // playhead. More than that clears the ped's gore (native
    // 0x704C908E9C405136, the engine's gore clear) and applies the mask again;
    // less applies what is missing (the engine's own mask replay path).
    // Returns whether a bit was refused (the caller retries a bounded number of
    // ticks; the engine refuses right after a clear).
    bool ReconcilePoseV2Gore(std::uint32_t timeMs, bool logUnchanged = true)
    {
        REPLAY_PROFILE_ZONE("play.gore_reconcile");
        bool retry = false;
        g_poseV2GorePending.clear();
        std::map<std::uint32_t, replay::entity::ReplayId> entities;
        for (const replay::entity::GoreHitEvent& event : Ctx().clip.goreHits.Events())
            entities[event.entity.Packed()] = event.entity;
        for (const replay::entity::GoreEvent& event : Ctx().clip.gore.Events())
            entities[event.entity.Packed()] = event.entity;
        for (const auto& [entity, id] : entities)
        {
            const replay::playback::Puppet* puppet = ReadyPedPuppet(id);
            if (!puppet)
                continue;
            const std::uint16_t desired = PoseV2GoreDesired(entity, timeMs);
            std::uint16_t& applied = g_poseV2GoreApplied[entity];
            if (applied == desired)
                continue;
            const std::uint16_t before = applied;
            bool cleared = false;
            int strayDeleted = 0;
            if (applied & ~desired)
            {
                PED::_0x704C908E9C405136(puppet->clone);
                applied = 0;
                cleared = true;
                // The pieces the engine dropped when these cuts were replayed stay on the
                // ground after the clear; the adopted ones are puppets and keep their own
                // lifetime.
                strayDeleted = replay::objects::DeleteStrayGorePieces(puppet->clone, 4.0f);
            }
            int appliedBits = 0;
            std::string error;
            for (int bit = 0; bit < 15; ++bit)
            {
                if (!(desired & (1u << bit)) || (applied & (1u << bit)))
                    continue;
                std::string bitError;
                // The recorded gameplay event of this limb when there is one
                // (the engine drops a real piece again, which the piece
                // puppet adopts: a gore clear removes the adopted ones);
                // otherwise the engine's mask replay path.
                const replay::entity::GoreHitEvent* hit = nullptr;
                for (const replay::entity::GoreHitEvent& event : Ctx().clip.goreHits.Events())
                    if (event.timeMs <= timeMs && event.result && event.entity.Packed() == entity &&
                        (HitMask(event) & (1u << bit)))
                        hit = &event;
                bool done = false;
                if (hit)
                {
                    bool handled = false;
                    void* inflictor = nullptr;
                    if (hit->inflictor.Valid())
                        if (const replay::playback::Puppet* other = Ctx().puppets.Find(hit->inflictor))
                            if (other->clone && replay::entities::Exists(other->clone))
                                inflictor = replay::entities::AddressOf(other->clone);
                    replay::objects::ReleaseGorePieces(hit->entity, hit->timeMs);
                    done = replay::gore::ReplayEvent(hit->context, replay::entities::AddressOf(puppet->clone), inflictor, handled, bitError) && handled;
                    if (!done && bitError.empty())
                        bitError = "event_not_handled";
                }
                if (!done)
                    done = replay::gore::ApplyLimb(replay::entities::AddressOf(puppet->clone), bit, bitError);
                if (done)
                {
                    ++appliedBits;
                    applied |= static_cast<std::uint16_t>(1u << bit);
                }
                else
                {
                    error = bitError;
                    retry = true;
                }
            }
            ++g_poseV2GoreReconciles;
            if (!logUnchanged && applied == before)
                continue;
            Ctx().log << "POSE_V2_PLAYBACK_GORE_RECONCILE,id=" << replay::entity::IdText(id) << ",had=0x" << std::hex << before
                << ",wanted=0x" << desired << std::dec << ",cleared=" << cleared << ",stray_pieces_deleted=" << strayDeleted
                << ",applied_bits=" << appliedBits
                << ",error=" << (error.empty() ? "none" : error.c_str()) << ",playhead_ms=" << timeMs << '\n';
        }
        return retry;
    }
    }

    void PlayPoseV2GoreHits(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.gore_hits");
        if (g_poseV2StateDeferred)
            return; // cheap scrub, reconciled on release
        const std::uint32_t last = g_poseV2GoreLastPlayhead;
        g_poseV2GoreLastPlayhead = timeMs;
        const bool jumped = !Ctx().hooks->NormalPlay() || timeMs < last ||
            timeMs - last > replay::entity::ForwardEventCursor::kMaxStepMs;
        if (jumped && timeMs != last)
            g_poseV2GoreRetryTicks = ReconcilePoseV2Gore(timeMs) ? 30 : 0;
        else if (g_poseV2GoreRetryTicks > 0)
        {
            --g_poseV2GoreRetryTicks;
            if (!ReconcilePoseV2Gore(timeMs, false))
                g_poseV2GoreRetryTicks = 0;
        }
        PlayPoseV2GorePending(timeMs);
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
        if (!g_poseV2GoreHitCursorPlay.Advance(timeMs, Ctx().hooks->NormalPlay(), fromMs, toMs))
            return;
        static std::vector<std::size_t> due;
        Ctx().clip.goreHits.Range(fromMs, toMs, due);
        for (const std::size_t index : due)
            if (!PlayPoseV2GoreHitAt(index, timeMs))
                g_poseV2GorePending.push_back({true, index, Ctx().clip.goreHits.Events()[index].timeMs});
    }

    void PlayPoseV2Gore(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.gore");
        if (g_poseV2StateDeferred)
            return; // cheap scrub (the cursor follows on release)
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
        if (!g_poseV2GoreCursorPlay.Advance(timeMs, Ctx().hooks->NormalPlay(), fromMs, toMs))
            return;
        static std::vector<std::size_t> due;
        Ctx().clip.gore.Range(fromMs, toMs, due);
        for (const std::size_t index : due)
            if (!PlayPoseV2GoreLimbAt(index, timeMs))
                g_poseV2GorePending.push_back({false, index, Ctx().clip.gore.Events()[index].timeMs});
    }
    namespace
    {

    // Where a record's +0x78 points (clip-on-disk pointer audit): null, the
    // game image (session-stable) or anything else (heap, dies with a save load).
    const char* BloodRef78Class(std::uint64_t value)
    {
        if (!value)
            return "null";
        static std::uintptr_t imageBase = 0;
        static std::uintptr_t imageEnd = 0;
        if (!imageBase)
        {
            imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(imageBase + dos->e_lfanew);
            imageEnd = imageBase + nt->OptionalHeader.SizeOfImage;
        }
        return value >= imageBase && value < imageEnd ? "image" : "other";
    }

    bool ApplyPoseV2Blood(const replay::entity::BloodEvent& event, float age, std::uint32_t timeMs);

    // Every record event plays; the evolution of a record already on the body
    // (the blood evolve's return) through ApplyPoseV2BloodEvolve, the others through the
    // engine's add.
    bool PoseV2BloodReplayable(const replay::entity::BloodEvent& event)
    {
        (void)event;
        return true;
    }

    bool PoseV2BloodEvolve(const replay::entity::BloodEvent& event)
    {
        return event.caller == replay::blood::CallerEvolve();
    }

    // False when it must wait (no puppet, render busy, or the record it
    // evolves not there yet); the pending list retries it for 3 s.
    bool ApplyPoseV2BloodEvolve(const replay::entity::BloodEvent& event, std::uint32_t timeMs)
    {
        const replay::playback::Puppet* puppet = ReadyPedPuppet(event.entity);
        if (!puppet)
            return false;
        ++g_poseV2BloodApplyCalls;
        bool busy = false;
        std::string error;
        const bool ok = replay::blood::ApplyEvolve(replay::entities::AddressOf(puppet->clone), event.record, busy, error);
        if (busy)
        {
            ++g_poseV2BloodBusy;
            return false;
        }
        if (ok)
        {
            if (error == "already_evolved")
                ++g_poseV2BloodEvolvePresent;
            else
                ++g_poseV2BloodEvolved;
        }
        else
        {
            ++g_poseV2BloodEvolveFailed;
            g_poseV2BloodLastError = error;
        }
        if (g_poseV2BloodEvolveLogs < 24)
        {
            ++g_poseV2BloodEvolveLogs;
            std::int32_t info = 0;
            std::int32_t next = 0;
            std::int32_t hits = 0;
            float size = 0.0f;
            std::memcpy(&info, event.record + 0x108, sizeof(info));
            std::memcpy(&next, event.record + 0x10C, sizeof(next));
            std::memcpy(&hits, event.record + 0x110, sizeof(hits));
            std::memcpy(&size, event.record + 0x104, sizeof(size));
            Ctx().log << "POSE_V2_PLAYBACK_BLOOD_EVOLVE,id=" << replay::entity::IdText(event.entity) << ",applied=" << ok
                << ",error=" << (error.empty() ? "none" : error.c_str()) << ",info=" << info << ",next=" << next << ",hits=" << hits
                << ",size=" << size << ",event_ms=" << event.timeMs << ",playhead_ms=" << timeMs << '\n';
        }
        if (!ok && error == "no_base_record")
            return false;
        std::uint32_t& upTo = g_poseV2BloodAppliedUpTo[event.entity.Packed()];
        upTo = (std::max)(upTo, event.timeMs);
        g_poseV2BloodAppliedClone[event.entity.Packed()] = puppet->clone;
        return true;
    }

    bool ApplyPoseV2BloodEvent(const replay::entity::BloodEvent& event, float age, std::uint32_t timeMs)
    {
        return PoseV2BloodEvolve(event) ? ApplyPoseV2BloodEvolve(event, timeMs) : ApplyPoseV2Blood(event, age, timeMs);
    }

    // One recorded record on its puppet; false when it must wait (no
    // puppet yet or the render job holds the sets).
    bool ApplyPoseV2Blood(const replay::entity::BloodEvent& event, float age, std::uint32_t timeMs)
    {
        const replay::playback::Puppet* puppet = ReadyPedPuppet(event.entity);
        if (!puppet)
            return false;
        bool busy = false;
        std::string error;
        std::uint64_t ref78 = 0;
        if (event.ref78Entity.Valid())
            if (const replay::playback::Puppet* named = ReadyPedPuppet(event.ref78Entity))
                ref78 = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(named->clone));
        ++g_poseV2BloodApplyCalls;
        std::uint64_t queriesBefore = 0, hitsBefore = 0;
        double queryMsBefore = 0.0;
        replay::blood::QueryStats(queriesBefore, hitsBefore, queryMsBefore);
        const std::uint64_t applyStart = replay::prof::Now();
        const bool ok = replay::blood::Apply(replay::entities::AddressOf(puppet->clone), event.record, age,
            event.caller == replay::blood::CallerPack(), ref78, busy, error);
        {
            // A slow apply: the engine's slot allocation or its add.
            const double totalMs = static_cast<double>(replay::prof::Now() - applyStart) * 1000.0 /
                static_cast<double>(replay::prof::TicksPerSecond());
            if (totalMs >= 5.0 && g_poseV2BloodSlowLogs < 40) // per playback
            {
                ++g_poseV2BloodSlowLogs;
                double allocMs = 0.0, addMs = 0.0;
                bool allocated = false;
                replay::blood::LastApplyTiming(allocMs, addMs, allocated);
                std::uint64_t queries = 0, hits = 0;
                double queryMs = 0.0;
                replay::blood::QueryStats(queries, hits, queryMs);
                char line[300]{};
                std::snprintf(line, sizeof(line),
                    "POSE_V2_BLOOD_SLOW,id=%s,total_ms=%.1f,alloc_ms=%.1f,add_ms=%.1f,query_ms=%.2f,queries=%llu,allocated=%d,pack=%d,ok=%d,error=%s,playhead_ms=%u\n",
                    replay::entity::IdText(event.entity).c_str(), totalMs, allocMs, addMs, queryMs - queryMsBefore,
                    static_cast<unsigned long long>(queries - queriesBefore), allocated ? 1 : 0,
                    event.caller == replay::blood::CallerPack() ? 1 : 0, ok ? 1 : 0, error.empty() ? "none" : error.c_str(), timeMs);
                Ctx().log << line;
            }
        }
        if (busy)
        {
            ++g_poseV2BloodBusy;
            return false;
        }
        if (ok)
        {
            ++g_poseV2BloodPlayed;
            if (error == "already_present")
                ++g_poseV2BloodPresent;
        }
        else
        {
            ++g_poseV2BloodFailed;
            g_poseV2BloodLastError = error;
        }
        std::uint32_t& upTo = g_poseV2BloodAppliedUpTo[event.entity.Packed()];
        upTo = (std::max)(upTo, event.timeMs);
        g_poseV2BloodAppliedClone[event.entity.Packed()] = puppet->clone;
        if (g_poseV2BloodPlayed + g_poseV2BloodFailed <= 40)
            Ctx().log << "POSE_V2_PLAYBACK_BLOOD,id=" << replay::entity::IdText(event.entity) << ",applied=" << ok
                << ",error=" << (error.empty() ? "none" : error.c_str()) << ",age=" << age << ",event_ms=" << event.timeMs
                << ",playhead_ms=" << timeMs << '\n';
        return true;
    }

    double PoseV2MsSince(std::uint64_t start)
    {
        return static_cast<double>(replay::prof::Now() - start) * 1000.0 / static_cast<double>(replay::prof::TicksPerSecond());
    }

    std::uint64_t PoseV2NowUs()
    {
        return replay::clock::TicksToUs(replay::prof::Now(), replay::prof::TicksPerSecond()); // mod_clock.h
    }

    // After a jump: every puppet is planned to the blood of the playhead,
    // only the difference (replay::budget::PlanPed): forward adds the
    // records after the last applied one; back (or a new puppet) clears and
    // lays every record up to the playhead. The ops run in PlayPoseV2Blood.
    void ReconcilePoseV2Blood(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.blood_reconcile");
        g_poseV2BloodPending.clear();
        const bool wasDraining = !g_poseV2BloodQueue.Empty();
        g_poseV2BloodQueue.Reset();
        const std::vector<replay::entity::BloodEvent>& events = Ctx().clip.blood.Events();
        // Per ped: the last record due and every record index, in clip order.
        struct PedRecords
        {
            replay::entity::ReplayId id;
            bool anyDue = false;
            std::uint32_t lastDue = 0;
            std::vector<std::size_t> indices;
        };
        std::map<std::uint32_t, PedRecords> peds;
        for (std::size_t index = 0; index < events.size(); ++index)
        {
            const replay::entity::BloodEvent& event = events[index];
            PedRecords& ped = peds[event.entity.Packed()];
            ped.id = event.entity;
            if (!PoseV2BloodReplayable(event) || event.timeMs > timeMs)
                continue;
            ped.anyDue = true;
            ped.lastDue = (std::max)(ped.lastDue, event.timeMs);
            ped.indices.push_back(index);
        }
        std::uint32_t planned = 0;
        for (const auto& [packed, ped] : peds)
        {
            const auto found = g_poseV2BloodAppliedUpTo.find(packed);
            const bool hasApplied = found != g_poseV2BloodAppliedUpTo.end();
            const std::uint32_t upTo = hasApplied ? found->second : 0;
            const replay::playback::Puppet* puppet = nullptr;
            const bool mayNeed = (hasApplied && upTo > timeMs) || (ped.anyDue && (!hasApplied || upTo < ped.lastDue));
            if (!mayNeed)
                continue;
            puppet = ReadyPedPuppet(ped.id);
            if (!puppet)
                continue;
            // Forward only (the puppet holds the records up to upTo and none past
            // the playhead): the records in (upTo, playhead] go on in order, as normal
            // play would have laid them.
            const auto appliedClone = g_poseV2BloodAppliedClone.find(packed);
            const bool sameClone = appliedClone != g_poseV2BloodAppliedClone.end() && appliedClone->second == puppet->clone;
            const replay::budget::PedPlan plan =
                replay::budget::PlanPed(hasApplied, upTo, sameClone, ped.anyDue, ped.lastDue, timeMs);
            if (plan == replay::budget::PedPlan::Nothing)
                continue;
            const bool forwardOnly = plan == replay::budget::PedPlan::Forward;
            if (!forwardOnly)
            {
                // The clear op names the ped through its first record.
                g_poseV2BloodQueue.PushClear(packed);
                ++planned;
            }
            std::uint32_t ops = 0;
            for (const std::size_t index : ped.indices)
            {
                if (forwardOnly && events[index].timeMs <= upTo)
                    continue;
                g_poseV2BloodQueue.PushApply(packed, index);
                ++ops;
            }
            planned += ops;
            ++g_poseV2BloodReconciles;
            Ctx().log << "POSE_V2_PLAYBACK_BLOOD_RECONCILE,id=" << replay::entity::IdText(ped.id) << ",queued=" << ops
                << ",forward_only=" << (forwardOnly ? 1 : 0) << ",playhead_ms=" << timeMs << '\n';
        }
        if (g_poseV2BloodQueue.Empty())
            return;
        g_poseV2BloodQueueTarget = timeMs;
        g_poseV2BloodQueueOps = planned;
        g_poseV2BloodQueueTicks = 0;
        g_poseV2BloodQueueMaxTickMs = 0.0;
        g_poseV2BloodQueueTotalMs = 0.0;
        g_poseV2BloodQueueStartQpc = replay::prof::Now();
        double queryMs = 0.0;
        std::uint64_t hits = 0;
        replay::blood::QueryStats(g_poseV2BloodQueueQueries, hits, queryMs);
        g_poseV2BloodQueueQueryMs = queryMs;
        if (g_poseV2BloodQueueLogs < 200)
        {
            ++g_poseV2BloodQueueLogs;
            Ctx().log << "POSE_V2_BLOOD_QUEUE,phase=planned,ops=" << planned << ",replanned=" << (wasDraining ? 1 : 0)
                << ",budget_us=" << g_poseV2StateBudgetUs << ",playhead_ms=" << timeMs << '\n';
        }
    }

    // One op of the queue, the record aged at the playhead now.
    void RunPoseV2BloodOp(const replay::budget::Op& op, std::uint32_t timeMs)
    {
        const std::vector<replay::entity::BloodEvent>& events = Ctx().clip.blood.Events();
        if (op.kind == replay::budget::Op::Kind::Clear)
        {
            // The ped's id from any of its records.
            for (const replay::entity::BloodEvent& event : events)
                if (event.entity.Packed() == op.entity)
                {
                    if (const replay::playback::Puppet* puppet = ReadyPedPuppet(event.entity))
                    {
                        std::string error;
                        replay::blood::Clear(replay::entities::AddressOf(puppet->clone), error);
                    }
                    g_poseV2BloodAppliedUpTo.erase(op.entity);
                    break;
                }
            return;
        }
        if (op.index >= events.size())
            return;
        const replay::entity::BloodEvent& event = events[op.index];
        const float age = event.age + static_cast<float>(timeMs > event.timeMs ? timeMs - event.timeMs : 0) / 1000.0f;
        if (!ApplyPoseV2BloodEvent(event, age, timeMs)) // evolutions in order too
            g_poseV2BloodPending.push_back(op.index);
    }
    }

    void PlayPoseV2Blood(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.blood");
        if (g_poseV2StateDeferred)
            return; // cheap scrub, reconciled on release
        const std::uint64_t tickStart = replay::prof::Now();
        const std::uint32_t appliesBefore = g_poseV2BloodApplyCalls;
        std::uint64_t queriesBefore = 0, hitsBefore = 0;
        double queryMsBefore = 0.0;
        replay::blood::QueryStats(queriesBefore, hitsBefore, queryMsBefore);
        const std::uint32_t last = g_poseV2BloodLastPlayhead;
        g_poseV2BloodLastPlayhead = timeMs;
        const bool jumped = !Ctx().hooks->NormalPlay() || timeMs < last ||
            timeMs - last > replay::entity::ForwardEventCursor::kMaxStepMs;
        if (jumped && timeMs != last)
            ReconcilePoseV2Blood(timeMs);
        // The queue under the tick's budget.
        replay::budget::RunStats run;
        if (!g_poseV2BloodQueue.Empty())
        {
            REPLAY_PROFILE_ZONE("play.blood_queue");
            const std::uint64_t queueStart = replay::prof::Now();
            run = g_poseV2BloodQueue.Run(g_poseV2StateBudgetUs, PoseV2NowUs,
                [timeMs](const replay::budget::Op& op) { RunPoseV2BloodOp(op, timeMs); });
            const double tickMs = PoseV2MsSince(queueStart);
            ++g_poseV2BloodQueueTicks;
            g_poseV2BloodQueueTotalMs += tickMs;
            g_poseV2BloodQueueMaxTickMs = (std::max)(g_poseV2BloodQueueMaxTickMs, tickMs);
            if (g_poseV2BloodQueue.Empty() && g_poseV2BloodQueueLogs < 200)
            {
                ++g_poseV2BloodQueueLogs;
                std::uint64_t queries = 0, hits = 0;
                double queryMs = 0.0;
                replay::blood::QueryStats(queries, hits, queryMs);
                char line[320]{};
                std::snprintf(line, sizeof(line),
                    "POSE_V2_BLOOD_QUEUE,phase=drained,ops=%u,ticks=%u,max_tick_ms=%.2f,work_ms=%.2f,wall_ms=%.1f,queries=%llu,query_ms=%.2f,target_ms=%u,playhead_ms=%u\n",
                    g_poseV2BloodQueueOps, g_poseV2BloodQueueTicks, g_poseV2BloodQueueMaxTickMs, g_poseV2BloodQueueTotalMs,
                    PoseV2MsSince(g_poseV2BloodQueueStartQpc), static_cast<unsigned long long>(queries - g_poseV2BloodQueueQueries),
                    queryMs - g_poseV2BloodQueueQueryMs, g_poseV2BloodQueueTarget, timeMs);
                Ctx().log << line;
            }
        }
        for (std::size_t n = 0; n < g_poseV2BloodPending.size();)
        {
            const replay::entity::BloodEvent& event = Ctx().clip.blood.Events()[g_poseV2BloodPending[n]];
            const float age = event.age + static_cast<float>(timeMs > event.timeMs ? timeMs - event.timeMs : 0) / 1000.0f;
            if (ApplyPoseV2BloodEvent(event, age, timeMs) || timeMs > event.timeMs + 3000 || timeMs + 50 < event.timeMs)
                g_poseV2BloodPending.erase(g_poseV2BloodPending.begin() + static_cast<std::ptrdiff_t>(n));
            else
                ++n;
        }
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
        if (g_poseV2BloodCursorPlay.Advance(timeMs, Ctx().hooks->NormalPlay(), fromMs, toMs))
        {
            static std::vector<std::size_t> due;
            Ctx().clip.blood.Range(fromMs, toMs, due);
            for (const std::size_t index : due)
            {
                const replay::entity::BloodEvent& event = Ctx().clip.blood.Events()[index];
                if (!PoseV2BloodReplayable(event))
                    continue;
                // A ped still in the queue keeps its order.
                if (g_poseV2BloodQueue.Holds(event.entity.Packed()))
                {
                    g_poseV2BloodQueue.PushApply(event.entity.Packed(), index);
                    continue;
                }
                if (!ApplyPoseV2BloodEvent(event, event.age, timeMs))
                    g_poseV2BloodPending.push_back(index);
            }
        }
        // A slow blood tick says where its time went (each apply can stay under
        // the per-apply 5 ms threshold while the tick as a whole stalls).
        const double tickMs = PoseV2MsSince(tickStart);
        if (tickMs >= 20.0 && g_poseV2BloodTickLogs < 40)
        {
            ++g_poseV2BloodTickLogs;
            std::uint64_t queries = 0, hits = 0;
            double queryMs = 0.0;
            replay::blood::QueryStats(queries, hits, queryMs);
            char line[320]{};
            std::snprintf(line, sizeof(line),
                "POSE_V2_BLOOD_TICK,tick_ms=%.1f,applies=%u,queue_ops=%u,queue_clears=%u,queue_left=%zu,budget_us=%u,pending=%zu,queries=%llu,query_ms=%.2f,cache_hits=%llu,playhead_ms=%u\n",
                tickMs, g_poseV2BloodApplyCalls - appliesBefore, run.done, run.clears, g_poseV2BloodQueue.Size(),
                g_poseV2StateBudgetUs, g_poseV2BloodPending.size(), static_cast<unsigned long long>(queries - queriesBefore),
                queryMs - queryMsBefore, static_cast<unsigned long long>(hits - hitsBefore), timeMs);
            Ctx().log << line;
        }
    }
}
