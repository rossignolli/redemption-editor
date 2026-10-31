#include "game_listener.h"
#include "game_listener_core.h"

#include "engine_calls.h"
#include "game_sig.h"
#include "guarded_memory.h"

#include <windows.h>

#include "main.h"
#include "natives.h"

#include <cmath>
#include <cstring>
#include <ostream>

// game_listener.h. Every engine access goes through replay::mem (SEH, range
// filtered) after the code sites of game_listener_core.h were all found by
// signature; one missing disables the module.
//
// LSTN v2 records every volume point and the microphone hash; the microphone
// is put back through the engine's own override hash +0x780
// (ListenerMicReplay), so the engine builds the recorded microphone's points
// itself (the bit 0x100 path puts every point on point 0 and loses the second
// point near the player).

namespace replay::gamelistener
{
    namespace
    {
        bool g_logEverySecond = false;
        bool g_micReplay = true;
        // A marker camera within this many meters of the recorded camera counts
        // as following it.
        constexpr float kCamMatchM = 1.0f;

        enum class Proof : std::uint8_t { Unknown, Proven, Failed };
        Proof g_proof = Proof::Unknown;
        std::uintptr_t g_env = 0;
        std::uintptr_t g_table = 0;  // the microphone table
        std::uintptr_t g_lookup = 0; // the metadata lookup by hash

        bool g_lastStateValid = false;
        std::uint64_t g_nextLineMs = 0;
        std::uint32_t g_lines = 0;
        std::uint32_t g_recordedSamples = 0;
        std::uint32_t g_recordFailures = 0;
        constexpr std::uint32_t kLineCap = 3600;

        // The microphone.
        enum class CallProof : std::uint8_t { Unknown, Proven, Failed };
        CallProof g_callProof = CallProof::Unknown;
        std::uint32_t g_micOurs = 0;          // the hash the replay wrote in +0x780 and still owns
        std::uint32_t g_micLearned = 0;       // the live camera map hash read in this session (derived fallback)
        std::uint32_t g_micWrites = 0;
        std::uint32_t g_micRestores = 0;
        std::uint32_t g_micForgets = 0;
        std::uint32_t g_micWriteFailures = 0;
        std::uint32_t g_micTicks = 0;
        std::uint32_t g_micLines = 0;
        constexpr std::uint32_t kMicLineCap = 400;
        std::uint32_t g_stateLines = 0;
        constexpr std::uint32_t kStateLineCap = 4000;
        std::uint32_t g_lastMicLogged = 0xFFFFFFFFu;
        std::uintptr_t g_lastInUseLogged = 0;
        const char* g_lastMicWhy = "";
        MicWho g_lastMicWho = MicWho::None;
        bool g_lastFollows = false;
        std::uint8_t g_lastPointsRec = 0xFF;
        std::uint32_t g_lastPointsLive = 0xFFFFFFFFu;

        bool Prove(std::ostream& log)
        {
            if (g_proof != Proof::Unknown)
                return g_proof == Proof::Proven;
            g_proof = Proof::Proven;
            for (const sig::Signature* site : kSites)
            {
                if (!sig::Find(*site))
                {
                    // Not found in this game: no address holds it (readable=0).
                    g_proof = Proof::Failed;
                    log << "GAME_LISTENER,phase=unproven,site=" << site->name << ",rva=0x" << std::hex << site->rva << std::dec
                        << ",readable=0\n";
                    break;
                }
            }
            if (g_proof == Proof::Proven)
            {
                g_env = sig::Find(kEnvironment);
                g_table = sig::Find(kMicTable);
                log << "GAME_LISTENER,phase=proven,sites=" << (sizeof(kSites) / sizeof(kSites[0])) << ",env_rva=0x" << std::hex
                    << (g_env - sig::ImageBase()) << std::dec << ",record_v2=1,mic_replay=" << (g_micReplay ? 1 : 0)
                    << ",cam_match_m=" << kCamMatchM
                    << '\n';
            }
            return g_proof == Proof::Proven;
        }

        // Rung 4: the metadata lookup by hash (kMetadataLookup, found once: not
        // found leaves the hash unread, MicSource::None). The camera map is not
        // called: its answer is a metadata reference, not a hash. The hash of
        // the metadata in use is read from the table's entries (rung 2) and
        // proven by the forward lookup.
        bool ProveCalls(std::ostream& log)
        {
            if (g_callProof != CallProof::Unknown)
                return g_callProof == CallProof::Proven;
            g_lookup = g_table ? sig::Find(kMetadataLookup) : 0;
            const bool lookup = g_lookup != 0;
            g_callProof = lookup ? CallProof::Proven : CallProof::Failed;
            log << "LISTENER_MIC,phase=calls,lookup=" << (lookup ? 1 : 0) << ",map_called=0\n";
            return g_callProof == CallProof::Proven;
        }

        bool CallLookup(std::uint32_t hash, std::uintptr_t& out, std::uint32_t& code)
        {
            out = 0;
            return engine::Call(engine::kListenerMetadataLookup, g_lookup, out, g_table, hash, kMicMetadataType) ||
                engine::Failed(code);
        }

        // The hash of a metadata object, from
        // the (offset, hash) entries of the chunk that holds it (layout in
        // game_listener_core.h, sites meta_*). Cached per pointer: the scan
        // runs once per microphone.
        struct HashCache
        {
            std::uintptr_t meta = 0;
            std::uint32_t hash = 0;
            const char* why = "";
        };
        HashCache g_hashCache[8];
        std::uint32_t g_hashCacheNext = 0;
        std::uint32_t g_hashScans = 0;
        std::uintptr_t g_lastUnknownLogged = 0;

        std::uint32_t ScanHashOf(std::uintptr_t meta, const char*& why)
        {
            const std::uintptr_t table = g_table;
            std::uint64_t chunks = 0;
            std::uint16_t count = 0;
            if (!mem::Read(table, chunks) || !mem::Read(table + 8, count) || !chunks || count > kMetaMaxChunks)
            {
                why = "table_unread";
                return 0;
            }
            static std::vector<MetaEntry> block;
            for (std::uint32_t c = 0; c < count; ++c)
            {
                const std::uintptr_t chunk = static_cast<std::uintptr_t>(chunks) + c * kMetaChunkStride;
                std::uint8_t loaded = 0;
                std::uint64_t data = 0, entries = 0;
                std::uint32_t size = 0, n = 0;
                if (!mem::Read(chunk + kMetaLoaded, loaded) || !loaded || !mem::Read(chunk + kMetaChunkData, data) ||
                    !mem::Read(chunk + kMetaChunkSize, size) || !data || meta < data || meta - data >= size)
                    continue;
                if (!mem::Read(chunk + kMetaEntryCount, n) || !mem::Read(chunk + kMetaEntries, entries) || !entries ||
                    n > kMetaMaxEntries)
                {
                    why = "entries_unread";
                    return 0;
                }
                const std::uint32_t offset = static_cast<std::uint32_t>(meta - data);
                constexpr std::uint32_t kBlock = 4096;
                block.resize(kBlock);
                for (std::uint32_t at = 0; at < n; at += kBlock)
                {
                    const std::uint32_t take = n - at < kBlock ? n - at : kBlock;
                    if (!mem::Copy(block.data(), static_cast<std::uintptr_t>(entries) + at * sizeof(MetaEntry),
                            take * sizeof(MetaEntry)))
                    {
                        why = "entries_unread";
                        return 0;
                    }
                    if (const std::uint32_t hash = HashOfOffset(block.data(), take, offset))
                    {
                        why = "entry";
                        return hash;
                    }
                }
                why = "no_entry";
                return 0;
            }
            why = "no_chunk";
            return 0;
        }

        std::uint32_t HashOfMetadata(std::uintptr_t meta, const char*& why)
        {
            for (const HashCache& c : g_hashCache)
                if (c.meta == meta)
                {
                    why = c.why;
                    return c.hash;
                }
            ++g_hashScans;
            std::uint32_t hash = ScanHashOf(meta, why);
            if (hash)
            {
                // Proof: the engine's own lookup by this hash gives the same object
                // (a hash also defined in a later chunk would give another one).
                std::uintptr_t back = 0;
                std::uint32_t code = 0;
                if (!CallLookup(hash, back, code) || back != meta)
                {
                    why = "lookup_differs";
                    hash = 0;
                }
            }
            HashCache& slot = g_hashCache[g_hashCacheNext++ % (sizeof(g_hashCache) / sizeof(g_hashCache[0]))];
            slot.meta = meta;
            slot.hash = hash;
            slot.why = why;
            return hash;
        }

        struct EngineListener
        {
            bool ok = false;
            std::uint8_t current = 0xFF;
            std::uint32_t points = 0;
            float pos[3] = {};
            std::uint32_t flags = 0;
            std::uint32_t micOverride = 0;
            std::uint32_t crossfade = 0;
            std::uintptr_t micInUse = 0;
            float override[3] = {};
            std::uint8_t stored = 0;
            Point point[kMaxRecordedPoints];
        };

        EngineListener ReadEngine()
        {
            EngineListener e;
            if (!g_env)
                return e;
            std::uint8_t current = 0xFF;
            if (!mem::Read(g_env + kCurrentIndex, current) || current >= kListenerCount)
                return e;
            const std::uintptr_t listener = g_env + ListenerOffset(current);
            std::uint32_t points = 0;
            float pos[4]{};
            float over[4]{};
            std::uint32_t flags = 0, mic = 0, crossfade = 0;
            std::uint64_t inUse = 0;
            if (!mem::Read(listener + kVolumePointCount, points) || !mem::Copy(pos, listener + kVolumePointPos, sizeof(pos)) ||
                !mem::Read(g_env + kFlags, flags) || !mem::Read(g_env + kMicOverrideHash, mic) ||
                !mem::Copy(over, g_env + kVolumeOverrideMatrix + 0x30, sizeof(over)))
                return e;
            mem::Read(g_env + kMicCrossfade, crossfade);
            mem::Read(listener + kMicInUse, inUse);
            e.ok = true;
            e.current = current;
            e.points = points;
            std::memcpy(e.pos, pos, sizeof(e.pos));
            std::memcpy(e.override, over, sizeof(e.override));
            e.flags = flags;
            e.micOverride = mic;
            e.crossfade = crossfade;
            e.micInUse = static_cast<std::uintptr_t>(inUse);
            const std::uint32_t stored = points <= kMaxVolumePoints ? (points < kMaxRecordedPoints ? points : kMaxRecordedPoints) : 0;
            for (std::uint32_t j = 0; j < stored; ++j)
            {
                const std::uintptr_t slot = listener + SlotOffset(j);
                float p[4]{}, w[4]{}, f[4]{};
                if (!mem::Copy(p, slot + 0x40, sizeof(p)) || !mem::Copy(w, slot + kSlotWeight, sizeof(w)) ||
                    !mem::Copy(f, slot + kSlotF70, sizeof(f)))
                    break;
                std::memcpy(e.point[j].pos, p, sizeof(e.point[j].pos));
                e.point[j].weight = w[0];
                e.point[j].f70 = f[0];
                e.stored = static_cast<std::uint8_t>(j + 1);
            }
            return e;
        }

        bool Finite(const float p[3]) { return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]); }

        bool PointValid(const EngineListener& e)
        {
            return e.ok && e.points > 0 && e.points <= kMaxVolumePoints && Finite(e.pos) &&
                (e.pos[0] != 0.0f || e.pos[1] != 0.0f || e.pos[2] != 0.0f);
        }

        // The microphone the live engine uses, as the microphone chooser
        // chooses it: the override hash; else the camera map's hash when its
        // metadata is the one in use; else the default when it is in use.
        MicSource ResolveMic(const EngineListener& e, std::uint32_t& hash, std::ostream& log)
        {
            hash = 0;
            if (!e.ok)
                return MicSource::None;
            if (e.micOverride)
            {
                hash = e.micOverride;
                return MicSource::Override;
            }
            if (!ProveCalls(log) || !e.micInUse)
                return MicSource::None;
            // The microphone in use (desc+0x300)
            // whatever path chose it (camera map, second map, default), by its
            // hash in the metadata table.
            const char* why = "";
            const std::uint32_t inUse = HashOfMetadata(e.micInUse, why);
            if (!inUse)
            {
                if (e.micInUse != g_lastUnknownLogged && g_micLines++ < kMicLineCap)
                    log << "LISTENER_MIC,phase=hash_unknown,in_use=0x" << std::hex << e.micInUse << std::dec << ",why=" << why
                        << ",scans=" << g_hashScans << '\n';
                g_lastUnknownLogged = e.micInUse;
                return MicSource::None;
            }
            hash = inUse;
            return inUse == kMicDefaultHash ? MicSource::Default : MicSource::CameraMap;
        }

        void MicLine(std::ostream& log, const char* phase, std::uint32_t clipMs, const EngineListener& e, std::uint32_t hash,
            MicSource source, const char* who)
        {
            if (g_micLines >= kMicLineCap)
                return;
            ++g_micLines;
            log << "LISTENER_MIC,phase=" << phase << ",clip_ms=" << clipMs << ",hash=0x" << std::hex << hash
                << ",source=" << MicSourceName(source) << ",who=" << who << ",in_use=0x" << e.micInUse
                << ",override=0x" << e.micOverride << std::dec << ",crossfade=" << static_cast<std::int32_t>(e.crossfade)
                << ",points=" << e.points;
            for (std::uint8_t j = 0; j < e.stored; ++j)
                log << ",p" << static_cast<int>(j) << '=' << e.point[j].pos[0] << ';' << e.point[j].pos[1] << ';'
                    << e.point[j].pos[2] << ",w" << static_cast<int>(j) << '=' << e.point[j].weight << ",f70_"
                    << static_cast<int>(j) << '=' << e.point[j].f70;
            log << '\n';
        }

        // The microphone override the way the engine's scene
        // writer does it, crossfade first (0: at once), then the hash; given
        // back the way the scene zeroers do it.
        bool WriteMic(std::uint32_t hash)
        {
            const std::uint32_t crossfade = 0;
            return mem::Put(g_env + kMicCrossfade, &crossfade, sizeof(crossfade)) &&
                mem::Put(g_env + kMicOverrideHash, &hash, sizeof(hash));
        }

        bool RestoreMic()
        {
            std::uint32_t now = 0;
            if (!mem::Read(g_env + kMicOverrideHash, now))
                return false;
            if (now != g_micOurs)
                return true; // somebody else's now: not ours to clear
            const std::uint32_t crossfade = 0, none = 0;
            return mem::Put(g_env + kMicCrossfade, &crossfade, sizeof(crossfade)) &&
                mem::Put(g_env + kMicOverrideHash, &none, sizeof(none));
        }

        void Line(std::ostream& log, const char* phase, std::uint32_t clipMs, const EngineListener& e, const float* target,
            const char* why)
        {
            const Vector3 cam = CAM::GET_FINAL_RENDERED_CAM_COORD();
            const float camPos[3] = {cam.x, cam.y, cam.z};
            const Ped player = PLAYER::PLAYER_PED_ID();
            float playerPos[3] = {};
            const bool havePlayer = player && ENTITY::DOES_ENTITY_EXIST(player);
            if (havePlayer)
            {
                const Vector3 p = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
                playerPos[0] = p.x;
                playerPos[1] = p.y;
                playerPos[2] = p.z;
            }
            log << "LISTENER,phase=" << phase << ",clip_ms=" << clipMs << ",ok=" << (e.ok ? 1 : 0)
                << ",cur=" << static_cast<int>(e.current) << ",points=" << e.points << ",at=" << e.pos[0] << ';' << e.pos[1]
                << ';' << e.pos[2] << ",d_cam=" << Distance(e.pos, camPos)
                << ",d_player=" << (havePlayer ? Distance(e.pos, playerPos) : -1.0f)
                << ",override_bit=" << ((e.flags & kVolumeOverrideBit) ? 1 : 0) << ",mic_override=0x" << std::hex
                << e.micOverride << ",mic_in_use=0x" << e.micInUse << std::dec;
            // The second point, where the live game puts it.
            if (e.stored >= 2)
                log << ",pos1=" << e.point[1].pos[0] << ';' << e.point[1].pos[1] << ';' << e.point[1].pos[2]
                    << ",pos1_d_cam=" << Distance(e.point[1].pos, camPos)
                    << ",pos1_d_player=" << (havePlayer ? Distance(e.point[1].pos, playerPos) : -1.0f);
            else
                log << ",pos1=na";
            if (target)
                log << ",target=" << target[0] << ';' << target[1] << ';' << target[2] << ",target_err=" << Distance(e.pos, target);
            if (why)
                log << ",why=" << why << ",mic_ours=0x" << std::hex << g_micOurs << std::dec;
            log << '\n';
        }
    }

    void Configure(bool logEverySecond)
    {
        g_logEverySecond = logEverySecond;
    }

    void ConfigureMic(bool micReplay)
    {
        g_micReplay = micReplay;
    }

    float CamMatchM() { return kCamMatchM; }

    void Record(Recording& clip, std::uint32_t timeMs, bool clockRunning, std::ostream& log)
    {
        if (!clockRunning)
            return;
        if (!Prove(log))
            return;
        const bool due = SampleDue(clip.gameListener, timeMs);
        const ULONGLONG now = GetTickCount64();
        const bool line = g_logEverySecond && now >= g_nextLineMs && g_lines < kLineCap;
        if (!due && !line)
            return;
        const EngineListener e = ReadEngine();
        if (due)
        {
            if (PointValid(e))
            {
                Sample s;
                s.timeMs = timeMs;
                std::memcpy(s.pos, e.pos, sizeof(s.pos));
                s.points = static_cast<std::uint8_t>(e.points);
                s.flags = (e.flags & kVolumeOverrideBit) ? kSampleOverride : 0;
                {
                    // LSTN v2: every point and the microphone.
                    s.stored = e.stored;
                    for (std::uint8_t j = 0; j < e.stored; ++j)
                        s.point[j] = e.point[j];
                    std::uint32_t hash = 0;
                    s.micSource = ResolveMic(e, hash, log);
                    s.micHash = hash;
                    // The derived fallback of a clip without hashes (who=derived) is used
                    // only on 2-point samples, so it is learned only from a 2-point
                    // microphone.
                    if ((s.micSource == MicSource::CameraMap || s.micSource == MicSource::Default) && e.points >= 2)
                        g_micLearned = hash;
                    if (hash != g_lastMicLogged || e.micInUse != g_lastInUseLogged)
                    {
                        g_lastMicLogged = hash;
                        g_lastInUseLogged = e.micInUse;
                        MicLine(log, "rec", timeMs, e, hash, s.micSource, "live");
                    }
                }
                Append(clip.gameListener, s);
                ++g_recordedSamples;
            }
            else if (g_recordFailures++ < 8)
            {
                log << "GAME_LISTENER,phase=record_skip,clip_ms=" << timeMs << ",ok=" << (e.ok ? 1 : 0)
                    << ",points=" << e.points << '\n';
            }
        }
        if (line)
        {
            g_nextLineMs = now + 1000;
            ++g_lines;
            Line(log, "rec", timeMs, e, nullptr, nullptr);
        }
    }

    void Play(const Recording& clip, std::uint32_t playheadMs, bool recordedGameCamera, std::ostream& log, float camErrM,
        const char* source)
    {
        if (!g_micReplay && !g_logEverySecond && !g_micOurs)
            return;
        const bool proven = Prove(log);
        EngineListener e = proven ? ReadEngine() : EngineListener{};

        // The microphone first; while it is ours the bit 0x100
        // path stays off (it puts every point on point 0).
        const std::uint32_t fallback = g_micLearned; // the hash of the 2-point microphone read live
        const MicPlan plan = proven ? MicAt(clip.gameListener, playheadMs, fallback) : MicPlan{};
        MicInputs mi;
        mi.enabled = g_micReplay;
        mi.proven = proven && e.ok;
        mi.follows = recordedGameCamera;
        mi.plan = plan.hash;
        mi.current = e.micOverride;
        mi.ours = g_micOurs;
        const char* micWhy = "";
        const MicAction action = DecideMic(mi, micWhy);
        bool micWriteFailed = false;
        switch (action)
        {
        case MicAction::Write:
            if (WriteMic(plan.hash))
            {
                g_micOurs = plan.hash;
                ++g_micWrites;
            }
            else
            {
                micWriteFailed = true;
                ++g_micWriteFailures;
            }
            break;
        case MicAction::Restore:
            if (RestoreMic())
                ++g_micRestores;
            g_micOurs = 0;
            break;
        case MicAction::Forget:
            g_micOurs = 0;
            ++g_micForgets;
            break;
        default:
            break;
        }
        if (g_micOurs)
            ++g_micTicks;

        float target[3] = {};
        const bool haveTarget = proven && At(clip.gameListener, playheadMs, target) && Finite(target);
        const bool stateChanged = !g_lastStateValid || std::strcmp(micWhy, g_lastMicWhy) != 0 ||
            plan.who != g_lastMicWho || recordedGameCamera != g_lastFollows || plan.pointsRec != g_lastPointsRec ||
            e.points != g_lastPointsLive || micWriteFailed;
        if (stateChanged && g_stateLines < kStateLineCap)
        {
            log << "GAME_LISTENER,phase=state,clip_ms=" << playheadMs << ",track="
                << clip.gameListener.samples.size() << ",recorded_camera=" << (recordedGameCamera ? 1 : 0)
                << ",source=" << (source ? source : "-") << ",cam_err_m=" << camErrM
                << ",points_rec=" << static_cast<int>(plan.pointsRec) << ",points_live=" << e.points
                << ",mic_why=" << micWhy << ",mic_action=" << MicActionName(action) << ",mic_who=" << MicWhoName(plan.who)
                << ",mic_hash=0x" << std::hex << plan.hash << ",mic_ours=0x" << g_micOurs << ",mic_now=0x" << e.micOverride
                << std::dec << ",mic_written=" << g_micWrites << ",write_failed=" << (micWriteFailed ? 1 : 0) << '\n';
            ++g_stateLines;
            g_lastStateValid = true;
            g_lastMicWhy = micWhy;
            g_lastMicWho = plan.who;
            g_lastFollows = recordedGameCamera;
            g_lastPointsRec = plan.pointsRec;
            g_lastPointsLive = e.points;
        }
        if (action == MicAction::Write && !micWriteFailed)
        {
            // The microphone the engine builds from the next frame on (the
            // points are read again on the next line).
            MicLine(log, "play", playheadMs, e, plan.hash, MicSource::Override, MicWhoName(plan.who));
        }
        const ULONGLONG now = GetTickCount64();
        if (g_logEverySecond && now >= g_nextLineMs && g_lines < kLineCap)
        {
            g_nextLineMs = now + 1000;
            ++g_lines;
            Line(log, "play", playheadMs, e, haveTarget ? target : nullptr, nullptr);
        }
    }

    void Release(const char* why, std::ostream& log)
    {
        bool micRestored = false;
        if (g_micOurs && g_proof == Proof::Proven)
            micRestored = RestoreMic();
        if (g_micTicks || g_micWrites)
            log << "GAME_LISTENER,phase=release,why=" << why << ",mic_ours=0x" << std::hex
                << g_micOurs << std::dec << ",mic_restored=" << (micRestored ? 1 : 0) << ",mic_ticks=" << g_micTicks
                << ",mic_writes=" << g_micWrites << ",mic_restores=" << g_micRestores << ",mic_forgets=" << g_micForgets
                << ",mic_write_failures=" << g_micWriteFailures << ",mic_learned=0x" << std::hex << g_micLearned << std::dec
                << '\n';
        g_lastStateValid = false;
        g_micOurs = 0;
        g_micTicks = 0;
        g_micWrites = 0;
        g_micRestores = 0;
        g_micForgets = 0;
        g_micWriteFailures = 0;
        g_lastMicWho = MicWho::None;
        g_lastMicWhy = "";
        g_stateLines = 0;
    }
}
