// The player's horse audio evidence (horse_audio_evidence.h has
// the offline evidence). Logging only: guarded reads, script thread.
#include "horse_audio_evidence.h"
#include "horse_audio_session.h"

#include "audio_observer.h"
#include "foot_event_core.h" // HORSE_STEP_PATHS
#include "guarded_memory.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace replay::horseaudio
{
    namespace
    {
        constexpr std::uintptr_t kComponentTableRva = 0x39ECE48;     // the row table every predicate reads
        constexpr std::uintptr_t kComponentIndexBaseRva = 0x39ECE40;
        constexpr std::uint64_t kFootOffset = 0x1560;               // foot audio in the holder (0x8C2B64)
        constexpr std::uint64_t kExtOffset = 0xE00;                 // ped audio +0xE00 object (0x318D9B9)
        constexpr std::uint32_t kStepLogCap = 400;                  // per phase, the rider too
        // The recording keeps every step of a clip (the trot decomposed too);
        // the playback keeps 400.
        constexpr std::uint32_t kStepLogCapRecord = 20000;
        std::uint32_t StepLogCap(const std::string& phase) { return phase == "record" ? kStepLogCapRecord : kStepLogCap; }
        constexpr std::uint32_t kStateLogCap = 30;                  // per phase
        constexpr std::uint32_t kCompareLogCap = 40;                // playback only
        constexpr std::uint32_t kStateIntervalMs = 1000;

        bool g_enabled = false;
        bool g_active = false;
        std::string g_phase;
        std::uint64_t g_playCursor = 0;
        std::uint64_t g_footCursor = 0;
        std::uint32_t g_stepLogs = 0;
        std::uint32_t g_stateLogs = 0;
        std::uint32_t g_compareLogs = 0;
        std::uint32_t g_lastStateMs = 0;
        bool g_haveStateTime = false;
        std::uint64_t g_requests = 0;
        std::uint64_t g_requestsReplayed = 0;
        std::uint64_t g_steps = 0;
        std::uint64_t g_stepsSilenced = 0;
        std::uint64_t g_stateSamples = 0;
        std::uint64_t g_compareSamples = 0;
        std::uint64_t g_compareDiffSamples = 0;
        std::uint64_t g_everDiff = 0;
        std::uint64_t g_noHorseTicks = 0;
        std::map<RefKey, std::uint32_t> g_refs;
        State g_lastLogged;
        bool g_haveLastLogged = false;
        // The live horse while the player rode it (last sample of the last
        // recording), kept for the playback comparison.
        State g_recordState;
        bool g_haveRecordState = false;
        std::string g_recordHorse;
        // The horse's step sounds by path per 5 s of clip time (main step,
        // secondary step with its soundset field, wet layer), every request
        // counted (the step lines above stop at kStepLogCap).
        replay::footevent::PathCounts g_paths;
        std::uint32_t g_pathWindow = 0xFFFFFFFFu;
        std::string g_pathOwner;
        std::uint32_t g_pathLines = 0;
        constexpr std::uint32_t kPathLineCap = 2000;

        void FlushPaths(std::ostream& log)
        {
            if (g_pathWindow != 0xFFFFFFFFu && g_paths.Any() && g_pathLines < kPathLineCap)
            {
                ++g_pathLines;
                log << replay::footevent::PathLine(g_phase.c_str(), g_pathWindow * replay::footevent::kWindowMs, g_pathOwner,
                           g_paths)
                    << '\n';
            }
            g_paths = replay::footevent::PathCounts{};
            g_pathWindow = 0xFFFFFFFFu;
        }

        void NotePath(std::uint32_t timeMs, const std::string& horseId, std::uint32_t caller2, std::uint64_t ref, std::ostream& log)
        {
            const std::uint32_t window = timeMs / replay::footevent::kWindowMs;
            if (window != g_pathWindow || horseId != g_pathOwner)
            {
                FlushPaths(log);
                g_pathWindow = window;
                g_pathOwner = horseId;
            }
            g_paths.Add(caller2, ref);
        }

        std::uint64_t Row(std::uint64_t entity)
        {
            if (!entity)
                return 0;
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            std::uint32_t index = 0;
            std::uint32_t indexBase = 0;
            std::uint64_t table = 0;
            if (!replay::mem::Read(entity + 0x9C, index) || !replay::mem::Read(base + kComponentIndexBaseRva, indexBase) ||
                !replay::mem::Read(base + kComponentTableRva, table) || !table)
                return 0;
            return table + static_cast<std::uint64_t>(static_cast<std::uint32_t>((index & 0x1FFFF) - indexBase)) * 0x148;
        }

        // The row slot as the engine reads it (neg/sbb/and: 0, else & ~1).
        bool Slot(std::uint64_t row, std::size_t slot, std::uint64_t& out)
        {
            out = 0;
            if (!row || !replay::mem::Read(row + slot, out))
                return false;
            out &= ~1ull;
            return true;
        }

        template <typename T>
        void Put(State& state, std::size_t field, std::uint64_t address, bool when = true)
        {
            T value{};
            if (when && address && replay::mem::Read(address, value))
            {
                state.values[field] = static_cast<std::uint64_t>(value);
                state.read[field] = 1;
            }
        }

        void Set(State& state, std::size_t field, std::uint64_t value)
        {
            state.values[field] = value;
            state.read[field] = 1;
        }

        void ResetCounters()
        {
            g_stepLogs = g_stateLogs = g_compareLogs = 0;
            g_haveStateTime = false;
            g_requests = g_requestsReplayed = g_steps = g_stepsSilenced = 0;
            g_stateSamples = g_compareSamples = g_compareDiffSamples = 0;
            g_everDiff = 0;
            g_noHorseTicks = 0;
            g_refs.clear();
            g_haveLastLogged = false;
            g_paths = replay::footevent::PathCounts{};
            g_pathWindow = 0xFFFFFFFFu;
            g_pathOwner.clear();
            g_pathLines = 0;
        }
    }

    void ReadState(void* pedEntity, State& state)
    {
        state = State{};
        const std::uint64_t ped = reinterpret_cast<std::uint64_t>(pedEntity);
        state.ped = ped;
        if (!ped)
            return;
        const std::uint64_t row = Row(ped);
        // Row mask and the slots the predicates use.
        if (row)
        {
            std::uint64_t mask = 0;
            for (std::size_t index = 0; index < kRowSlotCount; ++index)
            {
                std::uint64_t value = 0;
                if (Slot(row, kRowSlots[index], value) && value)
                    mask |= 1ull << index;
            }
            Set(state, kRowMask, mask);
            std::uint64_t own = 0;
            if (Slot(row, 0xB8, own) && own)
                Put<std::uint8_t>(state, kOwnFlags40, own + 0x40);
            std::uint64_t e0 = 0;
            if (Slot(row, 0xE0, e0) && e0)
                Put<std::uint32_t>(state, kRowE0Flags, e0 + 0x400);
            std::uint64_t link = 0;
            if (Slot(row, 0xA8, link))
            {
                if (!link)
                {
                    Set(state, kLinkIsPlayer, LinkAnswer(true, false, 0));
                    Set(state, kLinkE1, 0);
                }
                else
                {
                    // 0x8B69B0: [C+0xB0] entity, its row +0xB8 component +0x40.
                    std::uint64_t rider = 0;
                    std::uint64_t riderFlags = 0;
                    std::uint8_t flags = 0;
                    const bool riderRead = replay::mem::Read(link + 0xB0, rider);
                    bool readable = riderRead;
                    if (riderRead && rider)
                    {
                        readable = Slot(Row(rider), 0xB8, riderFlags) && riderFlags &&
                            replay::mem::Read(riderFlags + 0x40, flags);
                    }
                    Set(state, kLinkIsPlayer, LinkAnswer(readable, riderRead && rider != 0, flags));
                    // 0x8B7498 first steps: E1 = [C+8] & ~0xF, C0 = row(E1) +0x00.
                    std::uint64_t e1 = 0;
                    if (replay::mem::Read(link + 8, e1))
                    {
                        e1 &= ~0xFull;
                        Set(state, kLinkE1, !e1 ? 0 : e1 == ped ? 1 : (rider && e1 == rider) ? 2 : 3);
                        std::uint64_t c0 = 0;
                        if (e1 && Slot(Row(e1), 0x00, c0))
                            Set(state, kLinkC0, c0 ? 1 : 0);
                    }
                    // 0x8B4A88: [C+0xA8] entity has row +0xF0.
                    std::uint64_t other = 0;
                    if (replay::mem::Read(link + 0xA8, other))
                    {
                        std::uint64_t f0 = 0;
                        Set(state, kPred8B4A88, other && Slot(Row(other), 0xF0, f0) && f0 ? 1 : 0);
                    }
                }
            }
        }
        const std::uint64_t holder = replay::audio::PedAudioHolder(pedEntity);
        state.holder = holder;
        if (!holder)
            return;
        const std::uint64_t foot = holder + kFootOffset;
        std::uint64_t footParent = 0;
        std::uint64_t footPed = 0;
        replay::mem::Read(foot + 0x10, footParent);
        replay::mem::Read(foot + 0x18, footPed);
        // The step path reads the ped audio as [foot+0x10] (0x318D9B0).
        const std::uint64_t audio = footParent ? footParent : holder;
        const std::uint64_t ext = audio + kExtOffset;
        std::uint64_t extParent = 0;
        std::uint64_t extPed = 0;
        std::uint64_t extVtable = 0;
        replay::mem::Read(ext + 0x10, extParent);
        replay::mem::Read(ext + 0x18, extPed);
        replay::mem::Read(ext, extVtable);
        const std::uint64_t module = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
        state.extVtable = extVtable > module && extVtable < module + 0x7000000 ? static_cast<std::uint32_t>(extVtable - module) : 0;
        state.layout = (footParent == holder ? 1u : 0u) | (footPed == ped ? 2u : 0u) | (extParent == audio ? 4u : 0u) |
            (extPed == ped ? 8u : 0u);
        state.ok = footPed == ped && extPed == ped;
        Put<std::uint8_t>(state, kAudio4662, audio + 0x4662);
        Put<std::uint32_t>(state, kAudio45E8, audio + 0x45E8);
        std::uint64_t settings = 0;
        if (replay::mem::Read(audio + 0x4608, settings))
        {
            Set(state, kAudio4608, settings);
            Put<std::uint32_t>(state, kAudio4608H10, settings + 0x10, settings != 0);
            Put<std::uint32_t>(state, kAudio4608H14, settings + 0x14, settings != 0);
        }
        Put<std::uint64_t>(state, kAudio1450, audio + 0x1450);
        Put<std::uint8_t>(state, kAudio0774, audio + 0x774);
        Put<std::uint8_t>(state, kExt0700, ext + 0x700);
        std::uint64_t sound = 0;
        if (replay::mem::Read(ext + 0x708, sound))
            Set(state, kExt0708, sound ? 1 : 0);
        for (std::size_t slot = 0; slot <= 10; ++slot)
            Put<std::uint32_t>(state, kExtSlot0 + slot, ext + 0x470 + 4 * slot);
        Put<std::uint64_t>(state, kFoot0420, foot + 0x420);
        Put<std::uint64_t>(state, kFoot0428, foot + 0x428);
        Put<std::uint32_t>(state, kFoot0580, foot + 0x580);
        Put<std::uint8_t>(state, kFoot05A0, foot + 0x5A0);
        Put<std::uint8_t>(state, kFoot05A7, foot + 0x5A7);
        Put<std::uint32_t>(state, kFoot044C, foot + 0x44C);
        Put<std::uint8_t>(state, kFoot0599, foot + 0x599);
        Put<std::uint8_t>(state, kFoot0450, foot + 0x450);
        Put<std::uint8_t>(state, kFoot0024, foot + 0x24);
        // The persistent loops (horse_audio_evidence.h), 1 while
        // the slot holds a sound; the sound itself is not read.
        const auto holds = [&state](std::size_t field, std::uint64_t slot) {
            std::uint64_t value = 0;
            if (replay::mem::Read(slot, value))
                Set(state, field, value ? 1 : 0);
        };
        holds(kExt0030, ext + 0x30);
        holds(kExt0048, ext + 0x48);
        Put<std::uint8_t>(state, kFoot0596, foot + 0x596);
        holds(kFoot04D8, foot + 0x4D8);
        holds(kFoot04E0, foot + 0x4E0);
        holds(kFoot0480, foot + 0x480);
        Put<std::uint8_t>(state, kFoot05A5, foot + 0x5A5);
    }

    void SetEnabled(bool on)
    {
        g_enabled = on;
    }

    bool Enabled()
    {
        return g_enabled;
    }

    void Begin(const char* phase, std::ostream& log)
    {
        if (!g_enabled)
            return;
        g_active = true;
        g_phase = phase;
        ResetCounters();
        replay::audio::SetPlayRequestEvidence(true);
        std::vector<replay::audio::SoundRequest> stale;
        replay::audio::CollectPlayRequestsSince(g_playCursor, stale);
        std::vector<replay::audio::FootstepCall> staleFeet;
        replay::audio::CollectFootstepsSince(g_footCursor, staleFeet);
        if (g_phase == "record")
        {
            g_haveRecordState = false;
            g_recordHorse.clear();
        }
        log << "POSE_V2_HORSE_AUDIO_EVIDENCE,phase=" << g_phase << ",event=begin,record_state="
            << (g_haveRecordState ? 1 : 0) << '\n';
    }

    void Tick(void* horse, void* rider, const std::string& horseId, std::uint32_t timeMs, std::ostream& log)
    {
        if (!g_enabled || !g_active)
            return;
        const std::uint64_t holder = horse ? replay::audio::PedAudioHolder(horse) : 0;
        const std::uint64_t riderHolder = rider ? replay::audio::PedAudioHolder(rider) : 0;
        if (!holder)
            ++g_noHorseTicks;
        // Hoofsteps queued on this horse's foot audio (footstep entry ring).
        static std::vector<replay::audio::FootstepCall> feet;
        replay::audio::CollectFootstepsSince(g_footCursor, feet);
        for (const replay::audio::FootstepCall& step : feet)
        {
            if (!holder || step.footAudio != holder + kFootOffset)
                continue;
            ++g_steps;
            g_stepsSilenced += step.silenced ? 1 : 0;
        }
        // Sounds the engine queued on this horse's ped audio (the step
        // sounds of 0x318D52C among them).
        static std::vector<replay::audio::SoundRequest> requests;
        replay::audio::CollectPlayRequestsSince(g_playCursor, requests);
        bool sampled = false;
        State now;
        for (const replay::audio::SoundRequest& request : requests)
        {
            // The rider's own sounds too (the player live, his puppet in the
            // replay), marked who=rider.
            const bool onHorse = holder && request.audioEntity >= holder && request.audioEntity - holder < replay::audio::kPedAudioSpan;
            const bool onRider = riderHolder && request.audioEntity >= riderHolder &&
                request.audioEntity - riderHolder < replay::audio::kPedAudioSpan;
            if (!onHorse && !onRider)
                continue;
            if (onRider)
            {
                if (g_stepLogs >= StepLogCap(g_phase))
                    continue;
                ++g_stepLogs;
                char head[256];
                std::snprintf(head, sizeof(head),
                    "POSE_V2_HORSE_STEP_AUDIO,phase=%s,horse=%s,who=rider,ref=0x%llx,audio_off=0x%llx,callers=0x%X/0x%X/0x%X/0x%X,time_ms=%u,",
                    g_phase.c_str(), horseId.c_str(), static_cast<unsigned long long>(request.soundRef),
                    static_cast<unsigned long long>(request.audioEntity - riderHolder), request.callers[0], request.callers[1],
                    request.callers[2], request.callers[3], timeMs);
                log << head << FormatStepFilters(request.hasParams ? request.params : nullptr, sizeof(request.params)) << ','
                    << FormatStepParams(request.hasParams ? request.params : nullptr, sizeof(request.params)) << '\n';
                continue;
            }
            ++g_requests;
            g_requestsReplayed += request.inReplayedSystem ? 1 : 0;
            NotePath(timeMs, horseId, request.callers[2], request.soundRef, log);
            const std::uint8_t p138 = request.hasParams ? request.params[0x138] : 0xFF;
            RefKey key;
            key.ref = request.soundRef;
            key.caller = request.callers[1];
            key.p138 = p138;
            ++g_refs[key];
            if (g_stepLogs >= StepLogCap(g_phase))
                continue;
            ++g_stepLogs;
            if (!sampled)
            {
                ReadState(horse, now);
                sampled = true;
            }
            char line[512];
            std::snprintf(line, sizeof(line),
                "POSE_V2_HORSE_STEP_AUDIO,phase=%s,horse=%s,ref=0x%llx,audio_off=0x%llx,callers=0x%X/0x%X/0x%X/0x%X,"
                "replayed=%d,p138=0x%X,link_is_player=%llu,link_e1=%llu,p8b4a88=%llu,a4662=0x%llx,a45e8=%llu,"
                "a1450=0x%llx,x0498=0x%llx,f0428=0x%llx,row_mask=0x%llx,time_ms=%u",
                g_phase.c_str(), horseId.c_str(), static_cast<unsigned long long>(request.soundRef),
                static_cast<unsigned long long>(request.audioEntity - holder), request.callers[0], request.callers[1],
                request.callers[2], request.callers[3], request.inReplayedSystem ? 1 : 0, p138,
                static_cast<unsigned long long>(now.values[kLinkIsPlayer]),
                static_cast<unsigned long long>(now.values[kLinkE1]),
                static_cast<unsigned long long>(now.values[kPred8B4A88]),
                static_cast<unsigned long long>(now.values[kAudio4662]),
                static_cast<unsigned long long>(now.values[kAudio45E8]),
                static_cast<unsigned long long>(now.values[kAudio1450]),
                static_cast<unsigned long long>(now.values[kExtSlot10]),
                static_cast<unsigned long long>(now.values[kFoot0428]),
                static_cast<unsigned long long>(now.values[kRowMask]), timeMs);
            // The step's init params (volume and variables), the foot audio's
            // movement volume terms read by 0x31815C4 (+0x504, written by the
            // foot update 0x31925AC at 0x3192731; +0x500 its input; +0x508 the
            // step's variable scale) and the four foot mix slots (+0x390
            // volume, +0x3B0 high-pass), guarded reads (rung 2).
            {
                const std::uint64_t foot = holder + kFootOffset;
                float f500 = 0.0f, f504 = 0.0f, f508 = 0.0f, f510 = 0.0f, f518 = 0.0f, f51c = 0.0f;
                replay::mem::Read(foot + 0x510, f510); // the movement sounds' base volume (0x3193FE7)
                replay::mem::Read(foot + 0x518, f518);
                replay::mem::Read(foot + 0x51C, f51c);
                float vol[4] = {};
                std::uint32_t hp[4] = {};
                replay::mem::Read(foot + 0x500, f500);
                replay::mem::Read(foot + 0x504, f504);
                replay::mem::Read(foot + 0x508, f508);
                for (int n = 0; n < 4; ++n)
                {
                    replay::mem::Read(foot + 0x390 + 4 * n, vol[n]);
                    replay::mem::Read(foot + 0x3B0 + 4 * n, hp[n]);
                }
                char extra[260];
                std::snprintf(extra, sizeof(extra), ",f510=%.3f,f518=%.3f,f51c=%.3f,f500=%.3f,f504=%.3f,f508=%.3f,slots=%.2f/%u|%.2f/%u|%.2f/%u|%.2f/%u",
                    static_cast<double>(f510), static_cast<double>(f518), static_cast<double>(f51c),
                    static_cast<double>(f500), static_cast<double>(f504), static_cast<double>(f508),
                    static_cast<double>(vol[0]), hp[0], static_cast<double>(vol[1]), hp[1], static_cast<double>(vol[2]), hp[2],
                    static_cast<double>(vol[3]), hp[3]);
                log << line << extra;
            }
            log << ',' << FormatStepFilters(request.hasParams ? request.params : nullptr, sizeof(request.params))
                << ',' << FormatStepParams(request.hasParams ? request.params : nullptr, sizeof(request.params));
            // The whole init params as hex (0x140 bytes) and the sound's flags,
            // for a byte diff of each hit, live horse against puppet.
            if (request.hasParams)
            {
                static const char kHex[] = "0123456789abcdef";
                std::string hex;
                hex.reserve(sizeof(request.params) * 2);
                for (const std::uint8_t byte : request.params)
                {
                    hex.push_back(kHex[byte >> 4]);
                    hex.push_back(kHex[byte & 15]);
                }
                char flags[96];
                std::snprintf(flags, sizeof(flags), ",flags=%llx;%llx;%llx;%llx",
                    static_cast<unsigned long long>(request.flags[0]), static_cast<unsigned long long>(request.flags[1]),
                    static_cast<unsigned long long>(request.flags[2]), static_cast<unsigned long long>(request.flags[3]));
                log << flags << ",params=" << hex;
            }
            log << '\n';
        }
        // The horse's audio state once per second.
        if (!holder || (g_haveStateTime && timeMs - g_lastStateMs < kStateIntervalMs && timeMs >= g_lastStateMs))
            return;
        g_haveStateTime = true;
        g_lastStateMs = timeMs;
        if (!sampled)
            ReadState(horse, now);
        ++g_stateSamples;
        if ((!g_haveLastLogged || !SameState(g_lastLogged, now)) && g_stateLogs < kStateLogCap)
        {
            ++g_stateLogs;
            g_lastLogged = now;
            g_haveLastLogged = true;
            log << "POSE_V2_HORSE_AUDIO_STATE,phase=" << g_phase << ",horse=" << horseId << ",time_ms=" << timeMs << ','
                << FormatState(now) << '\n';
        }
        if (g_phase == "record")
        {
            g_recordState = now;
            g_haveRecordState = true;
            g_recordHorse = horseId;
            return;
        }
        if (!g_haveRecordState)
            return;
        ++g_compareSamples;
        std::size_t count = 0;
        const std::string diff = DiffStates(g_recordState, now, count);
        g_everDiff |= DiffMask(g_recordState, now);
        if (count)
            ++g_compareDiffSamples;
        if (g_compareLogs < kCompareLogCap)
        {
            ++g_compareLogs;
            log << "POSE_V2_HORSE_AUDIO_COMPARE,record_horse=" << g_recordHorse << ",play_horse=" << horseId
                << ",time_ms=" << timeMs << ",diff_fields=" << count << ",diff=" << (count ? diff : std::string("none")) << '\n';
        }
    }

    void End(std::ostream& log)
    {
        if (!g_enabled || !g_active)
            return;
        g_active = false;
        FlushPaths(log);
        replay::audio::SetPlayRequestEvidence(false);
        std::vector<std::pair<RefKey, std::uint32_t>> refs(g_refs.begin(), g_refs.end());
        std::sort(refs.begin(), refs.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string text;
        for (std::size_t index = 0; index < refs.size() && index < 24; ++index)
        {
            char item[96];
            std::snprintf(item, sizeof(item), "%s0x%llx@0x%X/p%X:%u", text.empty() ? "" : "|",
                static_cast<unsigned long long>(refs[index].first.ref), refs[index].first.caller, refs[index].first.p138,
                refs[index].second);
            text += item;
        }
        log << "POSE_V2_HORSE_AUDIO_SUMMARY,phase=" << g_phase << ",requests=" << g_requests
            << ",replayed=" << g_requestsReplayed << ",steps=" << g_steps << ",steps_silenced=" << g_stepsSilenced
            << ",state_samples=" << g_stateSamples << ",compare_samples=" << g_compareSamples
            << ",compare_diff_samples=" << g_compareDiffSamples << ",ever_diff=" << MaskNames(g_everDiff)
            << ",no_horse_ticks=" << g_noHorseTicks << ",distinct=" << refs.size()
            << ",refs=" << (text.empty() ? std::string("none") : text) << '\n';
        log.flush();
    }
}
