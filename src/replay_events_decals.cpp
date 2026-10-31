// replay_events_decals.cpp: decals recorded and played back, the projected decals, the recorded decals removed.
// Split from replay_events.cpp (moved code); shared internals in replay_events_internal.h.

#include "replay_events_internal.h"

namespace replay::events
{
    namespace
    {

    // The producer gate's meetings as they happen (the end-of-playback line
    // never comes after a crash): one line at most per second, only when waits
    // or timeouts grew.
    void LogPoseV2DecalGate()
    {
        static std::uint64_t loggedWaits = 0, loggedTimeouts = 0, lastLogMs = 0;
        const replay::decal::GateCounters gate = replay::decal::GateStats();
        if (gate.waits == loggedWaits && gate.timeouts == loggedTimeouts)
            return;
        const std::uint64_t nowMs = GetTickCount64();
        if (nowMs - lastLogMs < 1000)
            return;
        lastLogMs = nowMs;
        loggedWaits = gate.waits;
        loggedTimeouts = gate.timeouts;
        Ctx().log << "POSE_V2_DECAL_GATE,entries=" << gate.entries << ",waits=" << gate.waits << ",wait_us_max=" << gate.waitUsMax
            << ",wait_us_total=" << gate.waitUsTotal << ",timeouts=" << gate.timeouts << ",reentries=" << gate.reentries
            << ",playhead_ms=" << Ctx().transport.PlayheadMs() << ",engine_writes=0" << '\n';
    }
    }

    void MonitorPoseV2Decals(bool acceptAdds)
    {
        if (!g_poseV2DecalObserverReady)
            return;
        LogPoseV2DecalGate();
        static std::vector<replay::decal::AddCall> adds;
        replay::decal::CollectSince(g_poseV2MonitorAddCursor, adds);
        if (acceptAdds)
            for (const replay::decal::AddCall& call : adds)
                if (call.result)
                    g_poseV2DecalMonitor[call.result] = call;
        static std::vector<replay::decal::RemoveCall> removes;
        replay::decal::CollectRemovesSince(g_poseV2MonitorRemoveCursor, removes);
        for (const replay::decal::RemoveCall& call : removes)
            g_poseV2DecalMonitor.erase(call.id);
        // Validity, a few ids per tick (the engine drops old decals itself).
        if (g_poseV2MonitorCheckAt >= g_poseV2MonitorCheck.size())
        {
            g_poseV2MonitorCheck.clear();
            for (const auto& [id, call] : g_poseV2DecalMonitor)
                g_poseV2MonitorCheck.push_back(id);
            g_poseV2MonitorCheckAt = 0;
        }
        for (int n = 0; n < 64 && g_poseV2MonitorCheckAt < g_poseV2MonitorCheck.size(); ++n, ++g_poseV2MonitorCheckAt)
        {
            const std::uint32_t id = g_poseV2MonitorCheck[g_poseV2MonitorCheckAt];
            std::string error;
            if (!replay::decal::IsDecalAlive(id, error))
                g_poseV2DecalMonitor.erase(id);
        }
        while (g_poseV2DecalMonitor.size() > kDecalMonitorMax)
        {
            auto oldest = g_poseV2DecalMonitor.begin();
            for (auto it = g_poseV2DecalMonitor.begin(); it != g_poseV2DecalMonitor.end(); ++it)
                if (it->second.timeUs < oldest->second.timeUs)
                    oldest = it;
            g_poseV2DecalMonitor.erase(oldest);
        }
    }
    namespace
    {

    // The monitor's decals near the player become the recording's time-0
    // events (first recording tick).
    void RecordPoseV2InitialDecals()
    {
        const Vector3 player = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE, FALSE);
        std::vector<const replay::decal::AddCall*> alive;
        for (const auto& [id, call] : g_poseV2DecalMonitor)
        {
            float pos[3];
            std::memcpy(pos, call.settings, sizeof(pos));
            const float dx = pos[0] - player.x;
            const float dy = pos[1] - player.y;
            if (dx * dx + dy * dy <= 150.0f * 150.0f)
                alive.push_back(&call);
        }
        std::sort(alive.begin(), alive.end(),
            [](const replay::decal::AddCall* a, const replay::decal::AddCall* b) { return a->timeUs < b->timeUs; });
        // Each is taken as old as it is at record start (fade in 0, life and pool
        // growth advanced, decal_state.h); one past its life is left out.
        const std::uint64_t now = replay::decal::NowUs();
        // Initial decals whose info pointer is not in the info table any more
        // (the monitor kept them across a load, or the engine id was reused); they
        // are refused at playback (POSE_V2_DECAL_NULL_INFO), counted and sampled
        // here.
        std::uint64_t infoUnknown = 0;
        std::ostringstream unknownSample;
        for (const replay::decal::AddCall* call : alive)
        {
            {
                std::uint64_t info = 0;
                std::memcpy(&info, call->settings + replay::decal::kSettingsInfo, sizeof(info));
                if (!replay::decal::InfoKeyOf(info).known)
                {
                    if (infoUnknown < 4)
                    {
                        float pos[3];
                        std::memcpy(pos, call->settings, sizeof(pos));
                        unknownSample << (infoUnknown ? "|" : "") << call->result << ":"
                            << (now > call->timeUs ? (now - call->timeUs) / 1000 : 0) << "ms:" << static_cast<int>(pos[0]) << "/"
                            << static_cast<int>(pos[1]) << "/" << static_cast<int>(pos[2]);
                    }
                    ++infoUnknown;
                }
            }
            replay::decal::AddCall aged = *call;
            const float ageSeconds = now > call->timeUs ? static_cast<float>(now - call->timeUs) / 1000000.0f : 0.0f;
            if (ageSeconds > 0.0f)
            {
                const float lifeBefore = replay::decal::SettingsFloat(aged.settings, replay::decal::kSettingsLife);
                const float growthBefore = replay::decal::SettingsFloat(aged.settings, replay::decal::kSettingsGrowth);
                const bool keep = replay::decal::AgeSettings(aged.settings, ageSeconds);
                if (g_poseV2DecalAgeLogs < 16)
                {
                    ++g_poseV2DecalAgeLogs;
                    Ctx().log << "POSE_V2_DECAL_AGED,phase=initial,id=" << call->result << ",age_s=" << ageSeconds
                        << ",life=" << lifeBefore << "->" << replay::decal::SettingsFloat(aged.settings, replay::decal::kSettingsLife)
                        << ",growth=" << growthBefore << "->" << replay::decal::SettingsFloat(aged.settings, replay::decal::kSettingsGrowth)
                        << ",growth_time=" << replay::decal::SettingsFloat(aged.settings, replay::decal::kSettingsGrowthTime)
                        << ",kept=" << keep << '\n';
                }
                if (!keep)
                {
                    ++g_poseV2DecalsExpiredInitial;
                    continue;
                }
                ++g_poseV2DecalsAgedInitial;
            }
            AddPoseV2DecalEvent(aged, 1, true); // 1 ms: the play cursor takes (from, to]
            ++g_poseV2DecalInitial;
        }
        Ctx().log << "POSE_V2_DECAL_INITIAL,alive=" << g_poseV2DecalMonitor.size() << ",taken=" << g_poseV2DecalInitial
            << ",aged=" << g_poseV2DecalsAgedInitial << ",expired=" << g_poseV2DecalsExpiredInitial << ",info_unknown=" << infoUnknown
            << ",sample=" << (infoUnknown ? unknownSample.str() : std::string("none")) << ",monitor_session=" << g_poseV2DecalMonitorSession
            << '\n';
    }

    // The recorded decals still alive are checked, 64 per tick round robin
    // (the monitor's check, IsDecalAlive); one the engine let expire becomes a
    // removal with expired=1 at that time, so its chain gets removedMs and a
    // reconcile does not bring it back. A failed check (signature, manager) is
    // not a death.
    void SweepPoseV2RecordedDecals(std::uint32_t nowMs)
    {
        if (g_poseV2RecordedAliveIds.empty())
            return;
        if (g_poseV2RecordedAliveAt >= g_poseV2RecordedAliveIds.size())
            g_poseV2RecordedAliveAt = 0;
        std::vector<std::uint32_t> dead;
        for (int n = 0; n < 64 && n < static_cast<int>(g_poseV2RecordedAliveIds.size()); ++n)
        {
            if (g_poseV2RecordedAliveAt >= g_poseV2RecordedAliveIds.size())
                g_poseV2RecordedAliveAt = 0;
            const std::uint32_t id = g_poseV2RecordedAliveIds[g_poseV2RecordedAliveAt++];
            std::string error;
            if (!replay::decal::IsDecalAlive(id, error) && error.empty())
                dead.push_back(id);
        }
        for (const std::uint32_t id : dead)
        {
            replay::entity::DecalRemoveEvent event;
            event.timeMs = nowMs;
            event.engineId = id;
            event.expired = 1;
            Ctx().clip.decalRemoves.Add(event);
            ++g_poseV2DecalsExpired;
            if (g_poseV2DecalExpiredLogs < 16)
            {
                ++g_poseV2DecalExpiredLogs;
                Ctx().log << "POSE_V2_DECAL_EXPIRED,id=" << id << ",time_ms=" << nowMs << ",tracked=" << g_poseV2RecordedAliveIds.size()
                    << '\n';
            }
            g_poseV2RecordedAliveSet.erase(id);
        }
        if (!dead.empty())
        {
            g_poseV2RecordedAliveIds.erase(std::remove_if(g_poseV2RecordedAliveIds.begin(), g_poseV2RecordedAliveIds.end(),
                [](std::uint32_t id) { return g_poseV2RecordedAliveSet.count(id) == 0; }), g_poseV2RecordedAliveIds.end());
            g_poseV2RecordedAliveAt = 0;
        }
    }

    void TrackPoseV2RecordedDecal(std::uint32_t id)
    {
        if (id && g_poseV2RecordedAliveSet.insert(id).second)
            g_poseV2RecordedAliveIds.push_back(id);
    }
    }

    void UntrackPoseV2RecordedDecal(std::uint32_t id)
    {
        if (g_poseV2RecordedAliveSet.erase(id))
        {
            g_poseV2RecordedAliveIds.erase(std::remove(g_poseV2RecordedAliveIds.begin(), g_poseV2RecordedAliveIds.end(), id),
                g_poseV2RecordedAliveIds.end());
            if (g_poseV2RecordedAliveAt > g_poseV2RecordedAliveIds.size())
                g_poseV2RecordedAliveAt = 0;
        }
    }

    void RecordPoseV2Decals()
    {
        REPLAY_PROFILE_ZONE("rec.decals");
        if (g_poseV2DecalInitialPending)
        {
            g_poseV2DecalInitialPending = false;
            RecordPoseV2InitialDecals();
        }
        static std::vector<replay::decal::AddCall> calls;
        replay::decal::CollectSince(g_poseV2DecalCursor, calls);
        const std::uint64_t now = replay::decal::NowUs();
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        for (const replay::decal::AddCall& call : calls)
        {
            if (!call.result)
            {
                ++g_poseV2DecalsRefused;
                continue;
            }
            const std::uint64_t ageMs = now > call.timeUs ? (now - call.timeUs) / 1000 : 0;
            AddPoseV2DecalEvent(call, replay::pose::capture::RecordMsAgo(ageMs, nowMs), false);
        }
        RecordPoseV2DecalExtras(now, nowMs);
        SweepPoseV2RecordedDecals(nowMs);
    }

    // One observed decal add as a recorded event. initial: a decal alive
    // before record start (the monitor's state), played as a plain add at 0.
    void AddPoseV2DecalEvent(const replay::decal::AddCall& call, std::uint32_t timeMs, bool initial)
    {
        {
            if (call.callers[0] != replay::decal::GenericReturn() && !initial)
                ++g_poseV2DecalsInternal;
            replay::entity::DecalEvent event;
            event.timeMs = timeMs;
            event.callerReturn = initial ? replay::decal::GenericReturn() : call.callers[0];
            event.engineId = call.result;
            event.requestId = call.id;
            event.flag = call.flag;
            std::memcpy(event.settings, call.settings, sizeof(event.settings));
            {
                std::uint64_t info = 0;
                std::memcpy(&info, event.settings + replay::decal::kSettingsInfo, sizeof(info));
                const replay::decal::InfoKey key = replay::decal::InfoKeyOf(info);
                event.infoKnown = key.known;
                event.infoHash = key.hash;
                event.infoIndex = key.index;
                if (info && !key.known)
                    ++g_poseV2DecalsInfoUnknown;
                std::memset(event.settings + replay::decal::kSettingsInfo, 0, sizeof(info));
            }
            event.attached = (call.settings[replay::decal::kSettingsAttachFlags] & 1) != 0 && call.entity != 0;
            if (event.attached)
                ++g_poseV2DecalsAttached;
            // The entity is kept whenever the add named one, attached or not: the
            // engine keeps it with the rage decal and paints the projected mirror on it.
            // A recorded ped, vehicle, weapon or prop by id, else a world object by
            // model and position.
            if (call.entity != 0)
            {
                event.entity = RecordedIdAtAddress(call.entity);
                if (!event.entity.Valid())
                {
                    // Every add on an entity looks it up; the pool scan is cached per game
                    // tick.
                    static std::unordered_map<std::uint64_t, Entity> handleCache;
                    static ULONGLONG handleCacheAt = 0;
                    const ULONGLONG tick = GetTickCount64();
                    if (tick != handleCacheAt)
                    {
                        handleCache.clear();
                        handleCacheAt = tick;
                    }
                    const auto cached = handleCache.find(call.entity);
                    const Entity handle = cached != handleCache.end() ? cached->second
                                                                      : (handleCache[call.entity] = HandleAtAddress(call.entity));
                    if (handle)
                    {
                        event.attachModel = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(handle));
                        const Vector3 at = ENTITY::GET_ENTITY_COORDS(handle, TRUE, FALSE);
                        event.attachPos[0] = at.x;
                        event.attachPos[1] = at.y;
                        event.attachPos[2] = at.z;
                    }
                }
                event.hasEntity = event.entity.Valid() || event.attachModel != 0;
                if (event.hasEntity && !event.attached)
                {
                    ++g_poseV2DecalsWithEntity;
                    if (g_poseV2DecalEntityLogs < 12)
                    {
                        ++g_poseV2DecalEntityLogs;
                        std::uint16_t settingsEntityId = 0;
                        std::memcpy(&settingsEntityId, call.settings + replay::decal::kSettingsEntityId, sizeof(settingsEntityId));
                        Ctx().log << "POSE_V2_DECAL_ENTITY,phase=record,time_ms=" << timeMs << ",id=" << call.result << std::hex
                            << ",entity=0x" << call.entity << ",model=0x" << event.attachModel << ",settings_b8=0x" << settingsEntityId
                            << ",caller=0x" << call.callers[1] << "/0x" << call.callers[2] << std::dec
                            << ",rec=" << replay::entity::IdText(event.entity) << '\n';
                    }
                }
            }
            // Evidence: the settings of a blood pool add (decal_observer.h PoolAddReturn) as recorded:
            // life, fade in, growth, growth time, growth end.
            if (!initial && g_poseV2PoolSamples < 16 &&
                (call.callers[0] == replay::decal::PoolAddReturn() || call.callers[1] == replay::decal::PoolAddReturn() ||
                    call.callers[2] == replay::decal::PoolAddReturn()))
            {
                ++g_poseV2PoolSamples;
                float dims[4];
                std::memcpy(dims, call.settings + 0x30, sizeof(dims));
                Ctx().log << "POSE_V2_POOL_SAMPLE,time_ms=" << timeMs << ",id=" << call.result << ",req=" << call.id
                    << ",life44=" << replay::decal::SettingsFloat(call.settings, replay::decal::kSettingsLife)
                    << ",fade48=" << replay::decal::SettingsFloat(call.settings, replay::decal::kSettingsFadeIn)
                    << ",fade4c=" << replay::decal::SettingsFloat(call.settings, 0x4C)
                    << ",growth50=" << replay::decal::SettingsFloat(call.settings, replay::decal::kSettingsGrowth)
                    << ",growth54=" << replay::decal::SettingsFloat(call.settings, replay::decal::kSettingsGrowthTime)
                    << ",end60=" << replay::decal::SettingsFloat(call.settings, replay::decal::kSettingsGrowthEnd)
                    << ",dims=" << dims[0] << ";" << dims[1] << ";" << dims[2] << ";" << dims[3] << '\n';
            }
            if (g_poseV2DecalSamples++ < 24)
            {
                float pos[3];
                float dims[4];
                std::memcpy(pos, call.settings, sizeof(pos));
                std::memcpy(dims, call.settings + 0x30, sizeof(dims));
                Ctx().log << "POSE_V2_DECAL_SAMPLE,time_ms=" << event.timeMs << std::hex << ",caller=0x" << call.callers[0]
                    << "/0x" << call.callers[1] << "/0x" << call.callers[2] << ",entity=0x" << call.entity << ",model=0x" << event.attachModel
                    << std::dec << ",id=" << call.result << ",req=" << call.id << ",flag=" << int(call.flag) << ",attached=" << int(event.attached)
                    << ",rec=" << replay::entity::IdText(event.entity) << ",pos=" << pos[0] << ";" << pos[1] << ";" << pos[2]
                    << ",dims=" << dims[0] << ";" << dims[1] << ";" << dims[2] << ";" << dims[3] << '\n';
            }
            {
                float pos[3];
                std::memcpy(pos, call.settings, sizeof(pos));
                if (!event.attached)
                    Ctx().clip.ExtendArea(pos[0], pos[1], pos[2]);
            }
            // A replayable add is followed until it expires.
            if (event.callerReturn == replay::decal::GenericReturn())
                TrackPoseV2RecordedDecal(event.engineId);
            Ctx().clip.decals.Add(event);
        }
    }

    void RecordPoseV2DecalExtras(std::uint64_t now, std::uint32_t nowMs)
    {
        static std::vector<replay::decal::ProjCall> projs;
        replay::decal::CollectProjSince(g_poseV2ProjCursor, projs);
        for (const replay::decal::ProjCall& call : projs)
        {
            // The projection a blood record makes: the blood track applies
            // the record again and the engine projects it itself.
            if (g_poseV2BloodObserverReady && call.caller == replay::blood::FinalizeProjReturn())
            {
                ++g_poseV2ProjFromBlood;
                continue;
            }
            // The mirror of a rage decal on an entity (RageMirrorProjReturn, owner
            // always 0): the rage decal is recorded with its entity and the engine
            // paints the mirror again at playback.
            if (call.caller == replay::decal::RageMirrorProjReturn())
            {
                ++g_poseV2ProjFromRage;
                if (g_poseV2ProjFromRageLogs < 8)
                {
                    ++g_poseV2ProjFromRageLogs;
                    Ctx().log << "POSE_V2_PROJ_FROM_RAGE,type=0x" << std::hex << call.typeHash << ",entity=0x" << call.entity
                        << ",owner=0x" << call.owner << std::dec << ",pos=" << call.position[0] << ";" << call.position[1] << ";"
                        << call.position[2] << ",size=" << call.size << '\n';
                }
                continue;
            }
            replay::entity::ProjDecalEvent event;
            const std::uint64_t ageMs = now > call.timeUs ? (now - call.timeUs) / 1000 : 0;
            event.timeMs = replay::pose::capture::RecordMsAgo(ageMs, nowMs);
            for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
                if (reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(entity.handle)) == call.entity)
                    event.entity = entity.id;
            if (!event.entity.Valid())
            {
                if (const Entity handle = HandleAtAddress(call.entity))
                {
                    event.attachModel = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(handle));
                    const Vector3 at = ENTITY::GET_ENTITY_COORDS(handle, TRUE, FALSE);
                    event.attachPos[0] = at.x;
                    event.attachPos[1] = at.y;
                    event.attachPos[2] = at.z;
                }
                else
                    ++g_poseV2ProjNoEntity;
            }
            event.typeHash = call.typeHash;
            event.arg4 = call.arg4;
            std::memcpy(event.position, call.position, sizeof(event.position));
            std::memcpy(event.direction, call.direction, sizeof(event.direction));
            std::memcpy(event.side, call.side, sizeof(event.side));
            event.size = call.size;
            event.arg9 = call.arg9;
            event.arg10 = call.arg10;
            event.arg11 = call.arg11;
            event.arg12 = call.arg12;
            {
                const replay::decal::InfoKey key = replay::decal::InfoKeyOf(call.owner);
                event.infoKnown = key.known;
                event.infoHash = key.hash;
                event.infoIndex = key.index;
            }
            if (g_poseV2ProjSamples++ < 12)
                Ctx().log << "POSE_V2_PROJ_DECAL_SAMPLE,time_ms=" << event.timeMs << std::hex << ",type=0x" << call.typeHash
                    << ",entity=0x" << call.entity << ",model=0x" << event.attachModel << ",owner=0x" << call.owner << ",caller=0x"
                    << call.caller << std::dec << ",rec=" << replay::entity::IdText(event.entity) << ",arg4=" << call.arg4
                    << ",pos=" << call.position[0] << ";" << call.position[1] << ";" << call.position[2] << ",dir=" << call.direction[0]
                    << ";" << call.direction[1] << ";" << call.direction[2] << ",size=" << call.size << ",a9=" << call.arg9
                    << ",a10=" << int(call.arg10) << ",a11=" << call.arg11 << ",a12=" << call.arg12 << '\n';
            Ctx().clip.projDecals.Add(event);
        }
        static std::vector<replay::decal::RemoveCall> removes;
        replay::decal::CollectRemovesSince(g_poseV2DecalRemoveCursor, removes);
        for (const replay::decal::RemoveCall& call : removes)
        {
            replay::entity::DecalRemoveEvent event;
            const std::uint64_t ageMs = now > call.timeUs ? (now - call.timeUs) / 1000 : 0;
            event.timeMs = replay::pose::capture::RecordMsAgo(ageMs, nowMs);
            event.engineId = call.id;
            Ctx().clip.decalRemoves.Add(event);
            UntrackPoseV2RecordedDecal(call.id); // removed by a call, not expired
        }
    }
    std::vector<DecalChain> g_poseV2DecalChains;
    // Regrows (a decal in place re-added only to change its size): done,
    // refused by the core (the old decal stays), chains that gave up after
    // kRegrowMaxRefusals, and ones left for the next tick by the per-tick
    // budget.
    std::uint64_t g_poseV2DecalRegrows = 0;
    std::uint64_t g_poseV2DecalRegrowRefused = 0;
    std::uint64_t g_poseV2DecalRegrowGaveUp = 0;
    std::uint64_t g_poseV2DecalRegrowDeferred = 0;
    std::uint32_t g_poseV2DecalRegrowLogs = 0;
    std::string g_poseV2DecalRegrowLastError;
    namespace
    {
    constexpr int kPoseV2DecalRegrowsPerTick = 32;
    std::vector<std::size_t> g_poseV2DecalChainOf; // event index -> chain
    }
    bool g_poseV2DecalChainsBuilt = false;
    std::uint32_t g_poseV2DecalLastPlayhead = 0;
    // A reconcile that hit its add or regrow budget runs again on the next
    // ticks (a single reconcile on scrub release must reach the state the
    // per-tick reconciles of the scrub reached).
    bool g_poseV2DecalReconcileIncomplete = false;
    // Cheap scrub: while set, fire, blood, gore, decal and trail state are
    // not rebuilt per tick; the first tick after it rebuilds them once at the
    // playhead.
    bool g_poseV2StateDeferred = false;
    std::uint64_t g_poseV2DeferredTicks = 0;
    namespace
    {
    bool g_poseV2TrailResetPending = false; // a scrub moved the puppets
    }
    std::uint64_t g_poseV2DecalReconciles = 0;
    std::uint64_t g_poseV2DecalStateRemoved = 0;
    std::uint64_t g_poseV2DecalStateRestored = 0;
    namespace
    {

    void BuildPoseV2DecalChains()
    {
        g_poseV2DecalChains.clear();
        const auto& events = Ctx().clip.decals.Events();
        g_poseV2DecalChainOf.assign(events.size(), static_cast<std::size_t>(-1));
        std::map<std::uint32_t, std::size_t> byEngineId;
        std::map<std::uint32_t, std::vector<std::pair<std::uint32_t, std::size_t>>> owners; // engine id -> (add ms, chain)
        std::vector<std::size_t> order(events.size());
        for (std::size_t n = 0; n < order.size(); ++n)
            order[n] = n;
        std::stable_sort(order.begin(), order.end(),
            [&](std::size_t a, std::size_t b) { return events[a].timeMs < events[b].timeMs; });
        for (const std::size_t index : order)
        {
            const replay::entity::DecalEvent& event = events[index];
            if (event.callerReturn != replay::decal::GenericReturn())
                continue; // internal re-adds are not replayed
            std::size_t chain = static_cast<std::size_t>(-1);
            const auto known = event.requestId ? byEngineId.find(event.requestId) : byEngineId.end();
            if (known != byEngineId.end())
                chain = known->second;
            else
            {
                chain = g_poseV2DecalChains.size();
                g_poseV2DecalChains.emplace_back();
            }
            g_poseV2DecalChains[chain].events.push_back(index);
            g_poseV2DecalChainOf[index] = chain;
            if (event.engineId)
            {
                byEngineId[event.engineId] = chain;
                owners[event.engineId].push_back({event.timeMs, chain});
            }
        }
        // A removal goes to the chain that held the engine id at its time (the
        // engine reuses ids; with the expiry removals a removal of an earlier decal
        // must not end a later one with that id). A removal before any add of its
        // id matches nothing.
        std::uint64_t expired = 0;
        for (const replay::entity::DecalRemoveEvent& remove : Ctx().clip.decalRemoves.Events())
        {
            const auto found = owners.find(remove.engineId);
            if (found == owners.end())
                continue;
            std::size_t chain = static_cast<std::size_t>(-1);
            for (const auto& [addMs, owner] : found->second)
            {
                if (addMs > remove.timeMs)
                    break;
                chain = owner;
            }
            if (chain == static_cast<std::size_t>(-1))
                continue;
            if (remove.timeMs < g_poseV2DecalChains[chain].removedMs)
                g_poseV2DecalChains[chain].removedMs = remove.timeMs;
            expired += remove.expired;
        }
        g_poseV2DecalChainsBuilt = true;
        Ctx().log << "POSE_V2_PLAYBACK_DECAL_CHAINS,chains=" << g_poseV2DecalChains.size()
            << ",removes=" << Ctx().clip.decalRemoves.Events().size() << ",expired_removes=" << expired << '\n';
    }

    // Adds (or re-adds with `ours`) one recorded decal event; the new id or 0.
    // ageMs > 0 (a reconcile after a jump) plays a copy aged to the playhead
    // (decal_state.h); `expired` says it had expired by then and nothing was
    // added. Forward play passes 0 and adds the settings as recorded.
    // regrow=true: a re-add of a decal in place only to change its size; a
    // refusal leaves the old decal visible, so it is counted as a refused regrow
    // (bounded retry in the reconcile), not as a failed decal.
    std::uint32_t ApplyPoseV2DecalEvent(std::size_t index, std::uint32_t ours, std::uint32_t ageMs = 0, bool* expired = nullptr,
        bool regrow = false)
    {
        const replay::entity::DecalEvent& event = Ctx().clip.decals.Events()[index];
        std::uint8_t settings[replay::decal::kSettingsBytes];
        std::memcpy(settings, event.settings, sizeof(settings));
        if (expired)
            *expired = false;
        if (ageMs)
        {
            const float lifeBefore = replay::decal::SettingsFloat(settings, replay::decal::kSettingsLife);
            const float growthBefore = replay::decal::SettingsFloat(settings, replay::decal::kSettingsGrowth);
            const bool alive = replay::decal::AgeSettings(settings, static_cast<float>(ageMs) / 1000.0f);
            if (g_poseV2DecalAgeLogs < 32)
            {
                ++g_poseV2DecalAgeLogs;
                Ctx().log << "POSE_V2_DECAL_AGED,phase=play,id=" << event.engineId << ",age_ms=" << ageMs << ",life=" << lifeBefore
                    << "->" << replay::decal::SettingsFloat(settings, replay::decal::kSettingsLife) << ",growth=" << growthBefore << "->"
                    << replay::decal::SettingsFloat(settings, replay::decal::kSettingsGrowth)
                    << ",growth_time=" << replay::decal::SettingsFloat(settings, replay::decal::kSettingsGrowthTime)
                    << ",kept=" << alive << '\n';
            }
            if (!alive)
            {
                ++g_poseV2DecalsExpiredPlay;
                if (expired)
                    *expired = true;
                return 0;
            }
            ++g_poseV2DecalsAgedPlay;
        }
        {
            std::uint64_t info = 0;
            if (!event.infoKnown)
            {
                // Never added with info 0: the engine's ring worker would read [0+0x72].
                // The game never adds a decal without its info record.
                ++g_poseV2DecalsNullInfo;
                if (g_poseV2DecalNullInfoLogs < 16)
                {
                    ++g_poseV2DecalNullInfoLogs;
                    float pos[3];
                    std::memcpy(pos, event.settings, sizeof(pos));
                    Ctx().log << "POSE_V2_DECAL_NULL_INFO,id=" << event.engineId << ",time_ms=" << event.timeMs
                        << ",initial=" << (event.timeMs <= 1 ? 1 : 0) << ",reason=info_unknown_at_record,x=" << pos[0] << ",y=" << pos[1]
                        << ",z=" << pos[2] << ",replace=" << (ours != 0) << ",added=0\n";
                }
                return 0;
            }
            {
                replay::decal::InfoKey key;
                key.known = true;
                key.hash = event.infoHash;
                key.index = event.infoIndex;
                std::string infoError;
                info = replay::decal::InfoPointerOf(key, infoError);
                if (!info)
                {
                    ++g_poseV2DecalsNoInfo;
                    g_poseV2DecalLastError = infoError;
                    return 0;
                }
            }
            std::memcpy(settings + replay::decal::kSettingsInfo, &info, sizeof(info));
        }
        void* entity = nullptr;
        if (event.attached)
        {
            Entity target = 0;
            if (event.entity.Valid())
            {
                const replay::playback::Puppet* puppet = Ctx().puppets.Find(event.entity);
                if (puppet && puppet->clone && replay::entities::Exists(puppet->clone))
                    target = puppet->clone;
            }
            if (!target && event.attachModel && !event.entity.Valid())
                target = FindEntityByModelNear(event.attachModel, event.attachPos, 3.0f);
            if (!target)
            {
                ++g_poseV2DecalsNoEntity;
                return 0;
            }
            // The settings keep the recorded +0xB8 (decal_observer.h
            // kSettingsEntityId: the game computes it from the impact record).
            entity = replay::entities::AddressOf(target);
        }
        else if (event.hasEntity)
        {
            // The rage decal goes back with the entity it named (the puppet, or the
            // world object found again) though not attached: the engine keeps it with
            // the decal (entry +0x318) and its mirror loop paints the projected copy (a
            // bullet mark on a deck). Not found: played without.
            Entity target = 0;
            if (event.entity.Valid())
            {
                const replay::playback::Puppet* puppet = Ctx().puppets.Find(event.entity);
                if (puppet && puppet->clone && replay::entities::Exists(puppet->clone))
                    target = puppet->clone;
            }
            if (!target && event.attachModel && !event.entity.Valid())
                target = FindEntityByModelNear(event.attachModel, event.attachPos, 3.0f);
            if (target)
            {
                entity = replay::entities::AddressOf(target);
                ++g_poseV2DecalsEntityPassed;
                // The settings keep the recorded +0xB8, as on an attached decal.
                if (g_poseV2DecalEntityLogs < 24)
                {
                    ++g_poseV2DecalEntityLogs;
                    std::uint16_t recordedId = 0;
                    std::memcpy(&recordedId, settings + replay::decal::kSettingsEntityId, sizeof(recordedId));
                    Ctx().log << "POSE_V2_DECAL_ENTITY,phase=play,id=" << event.engineId << ",rec=" << replay::entity::IdText(event.entity)
                        << std::hex << ",model=0x" << event.attachModel << ",entity=0x" << reinterpret_cast<std::uint64_t>(entity)
                        << ",settings_b8=0x" << recordedId << std::dec << ",replace=" << (ours != 0) << '\n';
                }
            }
            else
                ++g_poseV2DecalsEntityMissing;
        }
        std::string error;
        // A refusal of the core's guard (decal_add_guard.h), logged with its
        // reason, capped.
        const auto logGuard = [&](const char* path) {
            if (g_poseV2DecalGuardLogs >= 24)
                return;
            const bool guardReason = error.rfind("info_", 0) == 0 || error.rfind("id_", 0) == 0 || error.rfind("ring_", 0) == 0;
            if (!guardReason)
                return;
            ++g_poseV2DecalGuardLogs;
            Ctx().log << (error.rfind("info_", 0) == 0 ? "POSE_V2_DECAL_NULL_INFO" : "POSE_V2_DECAL_GUARD") << ",id=" << event.engineId
                << ",time_ms=" << event.timeMs << ",path=" << path << ",reason=" << error << ",ours=" << ours;
            if (error.rfind("ring_", 0) == 0)
            {
                // The entry the core would have taken and its previous entity.
                const replay::decal::RingStale stale = replay::decal::LastRingStale();
                Ctx().log << std::hex << ",ring_index=0x" << stale.index << ",ring_previous=0x" << stale.previous << std::dec;
            }
            Ctx().log << ",added=0\n";
        };
        if (ours)
        {
            if (replay::decal::AddDecalWithId(entity, settings, ours, event.flag, error))
            {
                ++g_poseV2DecalsPlayed;
                ++g_poseV2DecalsReplaced;
                g_poseV2DecalIds[event.engineId] = ours;
                return ours;
            }
            logGuard("replace");
            if (error == "id_not_alive" || error == "id_out_of_range")
            {
                // The engine let our decal go (its id freed or reused); a replace by that
                // id would count on a free slot. A new decal instead, with a new id.
                ++g_poseV2DecalsDeadIdReadd;
                error.clear();
                ours = 0;
            }
            else if (regrow)
            {
                ++g_poseV2DecalRegrowRefused;
                g_poseV2DecalRegrowLastError = error;
                return 0;
            }
            if (ours)
            {
                ++g_poseV2DecalsFailed;
                g_poseV2DecalLastError = error;
                return 0;
            }
        }
        if (const std::uint32_t added = replay::decal::AddDecal(entity, settings, event.flag, error))
        {
            ++g_poseV2DecalsPlayed;
            g_poseV2DecalIds[event.engineId] = added;
            return added;
        }
        logGuard("add");
        ++g_poseV2DecalsFailed;
        g_poseV2DecalLastError = error;
        return 0;
    }
    }
    std::vector<PoseV2DecalRemoveRetry> g_poseV2DecalRemoveRetries;
    std::uint64_t g_poseV2DecalRemoveRetried = 0; // removes done again on an id still alive
    namespace
    {
    constexpr std::uint64_t kPoseV2DecalRemoveRetryWindowMs = 2000;
    constexpr std::uint64_t kPoseV2DecalRemoveRetryStepMs = 150;
    constexpr std::size_t kPoseV2DecalRemoveRetryMax = 4096;

    bool RemovePoseV2Decal(std::uint32_t id, std::string& error)
    {
        const bool removed = replay::decal::RemoveDecal(id, error);
        if (removed && g_poseV2DecalRemoveRetries.size() < kPoseV2DecalRemoveRetryMax)
        {
            const std::uint64_t now = GetTickCount64();
            g_poseV2DecalRemoveRetries.push_back({id, now, now + kPoseV2DecalRemoveRetryStepMs, 0});
        }
        return removed;
    }

    void RetryPoseV2DecalRemoves()
    {
        if (g_poseV2DecalRemoveRetries.empty())
            return;
        const std::uint64_t now = GetTickCount64();
        std::unordered_set<std::uint32_t> ours;
        for (const DecalChain& chain : g_poseV2DecalChains)
            if (chain.ours)
                ours.insert(chain.ours);
        for (const auto& entry : g_poseV2DecalIds)
            ours.insert(entry.second);
        std::size_t kept = 0;
        for (PoseV2DecalRemoveRetry& retry : g_poseV2DecalRemoveRetries)
        {
            if (ours.count(retry.id) || now - retry.firstMs > kPoseV2DecalRemoveRetryWindowMs)
                continue;
            if (now >= retry.nextMs)
            {
                std::string error;
                if (replay::decal::IsDecalAlive(retry.id, error) && replay::decal::RemoveDecal(retry.id, error))
                    ++g_poseV2DecalRemoveRetried;
                ++retry.tries;
                retry.nextMs = now + kPoseV2DecalRemoveRetryStepMs * (retry.tries + 1);
            }
            g_poseV2DecalRemoveRetries[kept++] = retry;
        }
        g_poseV2DecalRemoveRetries.resize(kept);
    }

    void ReconcilePoseV2Decals(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.decals_reconcile");
        ++g_poseV2DecalReconciles;
        g_poseV2DecalReconcileIncomplete = false;
        const auto& events = Ctx().clip.decals.Events();
        int applied = 0;
        int regrows = 0;
        const std::uint64_t nowTick = GetTickCount64();
        for (DecalChain& chain : g_poseV2DecalChains)
        {
            std::size_t desired = static_cast<std::size_t>(-1);
            for (const std::size_t index : chain.events)
            {
                if (events[index].timeMs > timeMs)
                    break;
                desired = index;
            }
            if (desired != static_cast<std::size_t>(-1) && chain.removedMs <= timeMs)
                desired = static_cast<std::size_t>(-1);
            // A pool still growing (settings +0x54 > 0) at the new playhead or at the
            // last one is put back at the size of the moment even when its event did not
            // change: only when the aged size really changes (RegrowNeeded), at most
            // kPoseV2DecalRegrowsPerTick per tick, and a refused one waits and gives up
            // after a few (decal_state.h RegrowRetry).
            bool regrow = false;
            if (desired == chain.applied && desired != static_cast<std::size_t>(-1) && chain.ours && timeMs != chain.appliedAtMs &&
                replay::decal::RegrowAllowed(chain.regrow, nowTick))
            {
                const std::uint8_t* settings = events[desired].settings;
                const float ageNow = static_cast<float>(timeMs - events[desired].timeMs) / 1000.0f;
                const float ageThen = chain.appliedAtMs > events[desired].timeMs
                    ? static_cast<float>(chain.appliedAtMs - events[desired].timeMs) / 1000.0f
                    : 0.0f;
                regrow = replay::decal::RegrowNeeded(settings, ageNow, ageThen);
            }
            if (desired == chain.applied && !regrow)
                continue;
            if (regrow && regrows >= kPoseV2DecalRegrowsPerTick)
            {
                ++g_poseV2DecalRegrowDeferred;
                g_poseV2DecalReconcileIncomplete = true;
                continue;
            }
            if (desired == static_cast<std::size_t>(-1))
            {
                if (chain.ours)
                {
                    std::string error;
                    RemovePoseV2Decal(chain.ours, error);
                    for (auto it = g_poseV2DecalIds.begin(); it != g_poseV2DecalIds.end();)
                        it = it->second == chain.ours ? g_poseV2DecalIds.erase(it) : std::next(it);
                    ++g_poseV2DecalStateRemoved;
                }
                chain.ours = 0;
                chain.applied = desired;
                continue;
            }
            // A bounded number of adds per tick: a long jump spreads them. Aged to
            // the playhead; a decal past its life by then is removed like a chain with
            // no event in force (outside the add budget; a later jump back tries it
            // again).
            const std::uint32_t ageMs = timeMs - events[desired].timeMs;
            bool expired = false;
            if (ageMs)
            {
                std::uint8_t probe[replay::decal::kSettingsBytes];
                std::memcpy(probe, events[desired].settings, sizeof(probe));
                expired = !replay::decal::AgeSettings(probe, static_cast<float>(ageMs) / 1000.0f);
                if (expired)
                    ++g_poseV2DecalsExpiredPlay;
            }
            if (!expired)
            {
                if (applied >= 256)
                {
                    g_poseV2DecalReconcileIncomplete = true;
                    continue;
                }
                ++applied;
            }
            if (regrow && !expired)
                ++regrows;
            const std::uint32_t ours = expired ? 0 : ApplyPoseV2DecalEvent(desired, chain.ours, ageMs, &expired, regrow);
            if (regrow && !expired && !ours)
            {
                // The old decal stays; try again later, bounded.
                if (replay::decal::NoteRegrowRefused(chain.regrow, nowTick))
                {
                    ++g_poseV2DecalRegrowGaveUp;
                    if (g_poseV2DecalRegrowLogs < 16)
                    {
                        ++g_poseV2DecalRegrowLogs;
                        Ctx().log << "POSE_V2_DECAL_REGROW_GIVEUP,id=" << events[desired].engineId << ",refusals=" << chain.regrow.refusals
                            << ",error=" << g_poseV2DecalRegrowLastError << ",playhead_ms=" << timeMs << '\n';
                    }
                }
                continue;
            }
            if (expired)
            {
                if (chain.ours)
                {
                    std::string error;
                    RemovePoseV2Decal(chain.ours, error);
                    for (auto it = g_poseV2DecalIds.begin(); it != g_poseV2DecalIds.end();)
                        it = it->second == chain.ours ? g_poseV2DecalIds.erase(it) : std::next(it);
                    ++g_poseV2DecalStateRemoved;
                }
                chain.ours = 0;
                chain.applied = static_cast<std::size_t>(-1);
                continue;
            }
            if (ours)
            {
                if (chain.ours)
                    ++g_poseV2DecalStateRestored;
                if (regrow)
                {
                    ++g_poseV2DecalRegrows;
                    if (g_poseV2DecalRegrowLogs < 16)
                    {
                        ++g_poseV2DecalRegrowLogs;
                        const std::uint8_t* settings = events[desired].settings;
                        Ctx().log << "POSE_V2_DECAL_REGROW,id=" << events[desired].engineId << ",age_ms=" << ageMs
                            << ",growth50=" << replay::decal::SettingsFloat(settings, replay::decal::kSettingsGrowth)
                            << ",growth54=" << replay::decal::SettingsFloat(settings, replay::decal::kSettingsGrowthTime)
                            << ",end60=" << replay::decal::SettingsFloat(settings, replay::decal::kSettingsGrowthEnd)
                            << ",playhead_ms=" << timeMs << '\n';
                    }
                }
                else
                    replay::decal::ResetRegrow(chain.regrow); // a new event in force: regrows allowed again
                chain.ours = ours;
                chain.applied = desired;
                chain.appliedAtMs = timeMs;
            }
        }
    }
    }

    // During playback the replay's decals come only from the recording
    // (replay entities generate no effects): a decal the engine adds by itself
    // on the world or an object (a bleeding puppet's drips duplicating the
    // recorded ones) is removed at once. Decals on peds stay: the engine
    // rebuilds a body's blood from the replayed records.
    std::uint64_t g_poseV2EngineDecalCursor = 0;
    std::uint64_t g_poseV2EngineDecalsRemoved = 0;
    std::uint64_t g_poseV2EngineDecalsOnPeds = 0;
    namespace
    {
    void RemovePoseV2EngineDecals()
    {
        static std::vector<replay::decal::AddCall> adds;
        replay::decal::CollectSince(g_poseV2EngineDecalCursor, adds);
        for (const replay::decal::AddCall& call : adds)
        {
            if (!call.result || call.byReplay)
                continue;
            if (replay::decal::IsPedEntity(call.entity))
            {
                ++g_poseV2EngineDecalsOnPeds;
                continue;
            }
            std::string error;
            if (RemovePoseV2Decal(call.result, error))
                ++g_poseV2EngineDecalsRemoved;
        }
    }
    }

    void PlayPoseV2Decals(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.decals");
        if (!g_poseV2FxReplayEnabled)
            return;
        RemovePoseV2EngineDecals();
        RetryPoseV2DecalRemoves();
        if (!g_poseV2DecalChainsBuilt)
            BuildPoseV2DecalChains();
        // Cheap scrub: the decal state (and the trail map) wait for the release,
        // where the playhead differs from the last one seen and the reconcile below
        // runs once. The cursor keeps its old playhead.
        if (g_poseV2StateDeferred && !g_poseV2DecalInitialPlayPending)
            return;
        // A reconcile that ran out of its budget goes on at the same playhead.
        if (g_poseV2DecalReconcileIncomplete && timeMs == g_poseV2DecalLastPlayhead)
            ReconcilePoseV2Decals(timeMs);
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
        static std::vector<std::size_t> due;
        const std::uint32_t lastMs = g_poseV2DecalLastPlayhead;
        g_poseV2DecalLastPlayhead = timeMs;
        if (g_poseV2DecalInitialPlayPending)
        {
            // The initial state (decals alive before record start, at 1 ms)
            // is applied on the first playback tick, whatever the cursor says.
            g_poseV2DecalInitialPlayPending = false;
            Ctx().clip.decals.Range(0, 1, due);
            Ctx().log << "POSE_V2_PLAYBACK_DECAL_INITIAL,events=" << due.size() << ",playhead_ms=" << timeMs << '\n';
        }
        else if (!g_poseV2DecalCursorPlay.Advance(timeMs, Ctx().hooks->NormalPlay(), fromMs, toMs))
        {
            // Not a normal forward step: a jump or scrub (or a pause, when the
            // playhead did not move). Any move reconciles the whole state.
            if (timeMs != lastMs)
                ReconcilePoseV2Decals(timeMs);
            // Backward: footprints made after the new playhead go (the trail map
            // is reset; the puppets lay their trails again as they move).
            static ULONGLONG lastTrailReset = 0;
            if (timeMs + 50 < lastMs && GetTickCount64() - lastTrailReset > 500)
            {
                GRAPHICS::_0xEC3F7F24EEEB3BA3();
                lastTrailReset = GetTickCount64();
            }
            // The puppets keep laying tracks while the scrub moves them (backward
            // too), and the 500 ms lock lets the last ones stay: one more reset when
            // normal play resumes.
            if (timeMs != lastMs)
                g_poseV2TrailResetPending = true;
            return;
        }
        else
        {
            if (g_poseV2TrailResetPending)
            {
                g_poseV2TrailResetPending = false;
                GRAPHICS::_0xEC3F7F24EEEB3BA3();
                Ctx().log << "POSE_V2_TRAIL_RESET,reason=scrub_end,playhead_ms=" << timeMs << '\n';
            }
            Ctx().clip.decals.Range(fromMs, toMs, due);
        }
        for (const std::size_t index : due)
        {
            const replay::entity::DecalEvent& event = Ctx().clip.decals.Events()[index];
            if (event.callerReturn != replay::decal::GenericReturn())
            {
                ++g_poseV2DecalsSkippedInternal;
                continue;
            }
            const std::size_t chainIndex = index < g_poseV2DecalChainOf.size() ? g_poseV2DecalChainOf[index] : static_cast<std::size_t>(-1);
            DecalChain* chain = chainIndex < g_poseV2DecalChains.size() ? &g_poseV2DecalChains[chainIndex] : nullptr;
            // A re-add of a known decal replaces ours (the growing pool).
            std::uint32_t existing = chain ? chain->ours : 0;
            if (!existing)
            {
                const auto known = g_poseV2DecalIds.find(event.requestId);
                if (known != g_poseV2DecalIds.end())
                    existing = known->second;
            }
            const std::uint32_t ours = ApplyPoseV2DecalEvent(index, existing);
            if (ours && chain)
            {
                chain->ours = ours;
                chain->applied = index;
                chain->appliedAtMs = event.timeMs;
                replay::decal::ResetRegrow(chain->regrow);
            }
        }
    }
    namespace
    {

    // Evidence: the projected adds the engine makes by itself during
    // playback (the rage mirror of a decal the replay put back with its entity,
    // RageMirrorProjReturn), capped.
    void ObservePoseV2ProjEngine(std::uint32_t timeMs)
    {
        static std::vector<replay::decal::ProjCall> calls;
        replay::decal::CollectProjSince(g_poseV2ProjEngineCursor, calls);
        for (const replay::decal::ProjCall& call : calls)
        {
            if (call.byReplay)
                continue;
            ++g_poseV2ProjEngineSeen;
            if (call.caller == replay::decal::RageMirrorProjReturn())
                ++g_poseV2ProjEngineMirrors;
            if (g_poseV2ProjEngineLogs < 24)
            {
                ++g_poseV2ProjEngineLogs;
                Ctx().log << "POSE_V2_PROJ_PLAYBACK_ENGINE,playhead_ms=" << timeMs << std::hex << ",caller=0x" << call.caller
                    << ",entity=0x" << call.entity << ",type=0x" << call.typeHash << ",owner=0x" << call.owner << std::dec
                    << ",mirror=" << (call.caller == replay::decal::RageMirrorProjReturn()) << ",pos=" << call.position[0] << ";"
                    << call.position[1] << ";" << call.position[2] << ",size=" << call.size << '\n';
            }
        }
    }
    }

    void PlayPoseV2ProjDecals(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.proj_decals");
        ObservePoseV2ProjEngine(timeMs);
        if (!g_poseV2FxReplayEnabled)
            return;
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
        if (!g_poseV2ProjCursorPlay.Advance(timeMs, Ctx().hooks->NormalPlay(), fromMs, toMs))
            return;
        static std::vector<std::size_t> due;
        Ctx().clip.projDecals.Range(fromMs, toMs, due);
        for (const std::size_t index : due)
        {
            const replay::entity::ProjDecalEvent& event = Ctx().clip.projDecals.Events()[index];
            Entity target = 0;
            if (event.entity.Valid())
            {
                const replay::playback::Puppet* puppet = Ctx().puppets.Find(event.entity);
                if (puppet && puppet->clone && replay::entities::Exists(puppet->clone))
                    target = puppet->clone;
            }
            if (!target && event.attachModel)
                target = FindEntityByModelNear(event.attachModel, event.attachPos, 3.0f);
            if (!target)
            {
                ++g_poseV2ProjMissing;
                continue;
            }
            replay::decal::ProjCall call;
            call.typeHash = event.typeHash;
            call.arg4 = event.arg4;
            std::memcpy(call.position, event.position, sizeof(call.position));
            std::memcpy(call.direction, event.direction, sizeof(call.direction));
            std::memcpy(call.side, event.side, sizeof(call.side));
            call.size = event.size;
            call.arg9 = event.arg9;
            call.arg10 = event.arg10;
            call.arg11 = event.arg11;
            call.arg12 = event.arg12;
            // The 13th argument is an owner (a blood record or null), not a decal
            // info. An event without a known key (the rage mirrors, owner 0) is played
            // with a null owner, the value the engine's own mirror passes.
            if (event.infoKnown)
            {
                replay::decal::InfoKey key;
                key.known = true;
                key.hash = event.infoHash;
                key.index = event.infoIndex;
                std::string infoError;
                call.owner = replay::decal::InfoPointerOf(key, infoError);
                if (!call.owner)
                {
                    ++g_poseV2ProjNoInfo;
                    g_poseV2ProjLastError = infoError;
                    continue;
                }
            }
            else
            {
                call.owner = 0;
                ++g_poseV2ProjNullOwner;
            }
            std::string error;
            if (replay::decal::AddProjDecal(call, replay::entities::AddressOf(target), error))
                ++g_poseV2ProjPlayed;
            else
            {
                ++g_poseV2ProjFailed;
                g_poseV2ProjLastError = error;
            }
        }
    }

    // Every limb a recorded ped loses (the engine gore bit, read through the
    // native), as an event at the time it appeared.
    // The recorded entity at an engine address (invalid when none):
    // peds, vehicles, recorded weapons and props, train cars (a fire on a crate,
    // on an arrow, on a dropped lantern keeps its entity; the fx attach changes
    // resolve through here).
    std::uint32_t g_poseV2ObjectAtAddressLogs = 0;

    void PlayPoseV2DecalRemoves(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.decal_removes");
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
        if (!g_poseV2DecalRemoveCursorPlay.Advance(timeMs, Ctx().hooks->NormalPlay(), fromMs, toMs))
            return;
        static std::vector<std::size_t> due;
        Ctx().clip.decalRemoves.Range(fromMs, toMs, due);
        for (const std::size_t index : due)
        {
            const replay::entity::DecalRemoveEvent& event = Ctx().clip.decalRemoves.Events()[index];
            const auto found = g_poseV2DecalIds.find(event.engineId);
            if (found == g_poseV2DecalIds.end())
            {
                ++g_poseV2DecalRemovesUnmapped;
                continue;
            }
            std::string error;
            if (RemovePoseV2Decal(found->second, error))
                ++g_poseV2DecalsRemoved;
            for (DecalChain& chain : g_poseV2DecalChains)
                if (chain.ours == found->second)
                {
                    chain.ours = 0;
                    chain.applied = static_cast<std::size_t>(-1);
                }
            g_poseV2DecalIds.erase(found);
        }
    }

    std::uint32_t RemoveRecordedDecals()
    {
        std::set<std::uint32_t> ids;
        for (const replay::entity::DecalEvent& event : Ctx().clip.decals.Events())
            ids.insert(event.engineId);
        std::uint32_t removed = 0;
        for (const std::uint32_t id : ids)
        {
            std::string error;
            if (replay::decal::IsDecalAlive(id, error) && replay::decal::RemoveDecal(id, error))
                ++removed;
        }
        return removed;
    }

    std::uint32_t RecordedDecalsAlive(std::uint32_t& checked)
    {
        std::set<std::uint32_t> ids;
        for (const replay::entity::DecalEvent& event : Ctx().clip.decals.Events())
            ids.insert(event.engineId);
        checked = static_cast<std::uint32_t>(ids.size());
        std::uint32_t alive = 0;
        for (const std::uint32_t id : ids)
        {
            std::string error;
            if (replay::decal::IsDecalAlive(id, error))
                ++alive;
        }
        return alive;
    }
}
