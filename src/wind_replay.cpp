#include <intrin.h>

#include "wind_replay.h"

#include "guarded_memory.h"
#include "hook_batch.h"
#include "module_lifetime.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstring>

#pragma intrinsic(_ReturnAddress)

// Every read goes through replay::mem (SEH, range filtered) after the code
// sites of wind_replay_core.h matched once and the weather entity's vtable
// is the constructor's; the detour is installed only after the gate's
// prologue and the same sites matched.

namespace replay::windreplay
{
    namespace
    {
        Config g_cfg;

        enum class Proof : std::uint8_t { Unknown, Proven, Failed };
        Proof g_proof = Proof::Unknown;
        const char* g_failedSite = nullptr;
        std::uint32_t g_failedRva = 0;
        bool g_proofLogged = false;
        std::uintptr_t g_base = 0;

        // ---------------------------------------------------------- detour

        using GateFn = bool(__fastcall*)(void* streamer, const void* pos, std::uint32_t mask);
        std::atomic<GateFn> g_original{nullptr};
        void* g_target = nullptr;
        std::atomic<int> g_inFlight{0};
        std::atomic<std::uintptr_t> g_detourBase{0};

        // Published by the script thread, read by the detour: the position
        // first, the time last (release); a read between two publishes may
        // mix two ticks' coordinates (centimetres apart).
        std::atomic<float> g_sceneX{0.0f};
        std::atomic<float> g_sceneY{0.0f};
        std::atomic<float> g_sceneZ{0.0f};
        std::atomic<std::uint64_t> g_sceneAtMs{0};

        std::atomic<std::uint64_t> g_sceneCalls{0};
        std::atomic<std::uint64_t> g_sceneUnloaded{0};
        std::atomic<std::uint64_t> g_liveCalls{0};
        std::atomic<std::uint64_t> g_liveUnloaded{0};

        bool __fastcall GateDetour(void* streamer, const void* pos, std::uint32_t mask)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            g_inFlight.fetch_add(1, std::memory_order_seq_cst);
            const GateFn original = g_original.load(std::memory_order_acquire);
            bool result = false;
            if (original)
            {
                const std::uintptr_t ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
                const std::uintptr_t base = g_detourBase.load(std::memory_order_relaxed);
                const std::uint32_t retRva =
                    base && ret > base && ret - base < 0x80000000ull ? static_cast<std::uint32_t>(ret - base) : 0;
                if (retRva != kWeatherGateReturnRva)
                    result = original(streamer, pos, mask);
                else if (AskScene(retRva, g_sceneAtMs.load(std::memory_order_acquire), GetTickCount64()))
                {
                    alignas(16) float scene[4] = {g_sceneX.load(std::memory_order_relaxed),
                        g_sceneY.load(std::memory_order_relaxed), g_sceneZ.load(std::memory_order_relaxed), 0.0f};
                    result = original(streamer, scene, mask);
                    g_sceneCalls.fetch_add(1, std::memory_order_relaxed);
                    if (!result)
                        g_sceneUnloaded.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    result = original(streamer, pos, mask);
                    g_liveCalls.fetch_add(1, std::memory_order_relaxed);
                    if (!result)
                        g_liveUnloaded.fetch_add(1, std::memory_order_relaxed);
                }
            }
            g_inFlight.fetch_sub(1, std::memory_order_seq_cst);
            return result;
        }

        GateCounts TakeGate()
        {
            GateCounts g;
            g.sceneCalls = g_sceneCalls.exchange(0, std::memory_order_relaxed);
            g.sceneUnloaded = g_sceneUnloaded.exchange(0, std::memory_order_relaxed);
            g.liveCalls = g_liveCalls.exchange(0, std::memory_order_relaxed);
            g.liveUnloaded = g_liveUnloaded.exchange(0, std::memory_order_relaxed);
            return g;
        }

        // ---------------------------------------------------------- proof

        // The first site that does not match, null when all do (and the
        // weather entity holds its vtable).
        const char* CheckSites(std::uint32_t& failedRva)
        {
            if (!g_base)
                g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            if (!g_base)
            {
                failedRva = 0;
                return "module";
            }
            for (const Site& site : kSites)
            {
                std::uint8_t code[sizeof(site.bytes)]{};
                if (!mem::Copy(code, g_base + site.rva, site.size) || !SiteMatches(site, code))
                {
                    failedRva = site.rva;
                    return site.name;
                }
            }
            std::uint64_t vt = 0;
            if (!mem::Read(g_base + kWeatherRva, vt) || vt != g_base + kWeatherVtableRva)
            {
                failedRva = kWeatherRva;
                return "weather_entity_vtable";
            }
            // The pre-update is the entity's vtable slot +0x28: the call site belongs
            // to the weather entity.
            std::uint64_t preUpdate = 0;
            if (!mem::Read(g_base + kWeatherVtableRva + 0x28, preUpdate) || preUpdate != g_base + kPreUpdateRva)
            {
                failedRva = kWeatherVtableRva + 0x28;
                return "weather_preupdate_slot";
            }
            return nullptr;
        }

        bool Prove(std::ostream* log)
        {
            if (g_proof == Proof::Unknown)
            {
                g_failedSite = CheckSites(g_failedRva);
                g_proof = g_failedSite ? Proof::Failed : Proof::Proven;
            }
            if (log && !g_proofLogged)
            {
                g_proofLogged = true;
                if (g_proof == Proof::Proven)
                    *log << "WIND_REPLAY,phase=proven,sites=" << kSiteCount << ",weather=0x" << std::hex << kWeatherRva
                         << ",gate=0x" << kGateRva << ",streamer=0x" << kMapStreamerRva << std::dec << ",mask=" << kGateMask
                         << ",fix=" << (g_cfg.fix ? 1 : 0) << ",detour=" << (g_target ? 1 : 0) << '\n';
                else
                    *log << "WIND_REPLAY,phase=unproven,site=" << (g_failedSite ? g_failedSite : "?") << ",rva=0x" << std::hex
                         << g_failedRva << std::dec << ",note=disabled\n";
            }
            return g_proof == Proof::Proven;
        }

        // ---------------------------------------------------------- reads

        struct Reading
        {
            bool ok = false;
            float strength = 0.0f;
            std::uint32_t lines[2] = {};
            bool linesOk = false;
            std::uint32_t stamp = 0;
            bool stampOk = false;
            std::uint32_t wx[2] = {};
        };

        bool WeatherHashOf(std::uint32_t index, std::uint32_t& hash)
        {
            hash = 0;
            return index < kMaxWeatherTypes &&
                mem::Read(g_base + kWeatherTableRva + index * kWeatherTypeStride + kWeatherTypeHash, hash);
        }

        Reading ReadNow()
        {
            Reading r;
            const std::uintptr_t w = g_base + kWeatherRva;
            r.ok = mem::Read(w + kWindStrength, r.strength) && std::isfinite(r.strength);
            if (!r.ok)
                return r;
            std::uint32_t i0 = 0, i1 = 0;
            r.linesOk = mem::Read(w + kWindLinePrev, i0) && mem::Read(w + kWindLineNext, i1) && WeatherHashOf(i0, r.lines[0]) &&
                WeatherHashOf(i1, r.lines[1]);
            if (!r.linesOk)
                r.lines[0] = r.lines[1] = 0;
            r.stampOk = mem::Read(w + kSimpleStamp, r.stamp);
            std::uint32_t p = 0, n = 0;
            if (mem::Read(g_base + kWeatherPrevRva, p))
                WeatherHashOf(p, r.wx[0]);
            if (mem::Read(g_base + kWeatherNextRva, n))
                WeatherHashOf(n, r.wx[1]);
            return r;
        }

        Sample SampleOf(const Reading& r, std::uint32_t timeMs, bool& stampValid, std::uint32_t& lastStamp)
        {
            Sample s;
            s.timeMs = timeMs;
            s.strength = r.strength;
            if (r.linesOk)
            {
                s.lines[0] = r.lines[0];
                s.lines[1] = r.lines[1];
                s.flags |= kSampleLines;
            }
            if (r.stampOk)
            {
                if (stampValid && r.stamp != lastStamp)
                    s.flags |= kSampleSimple;
                lastStamp = r.stamp;
                stampValid = true;
            }
            return s;
        }

        // ---------------------------------------------------------- state

        // Recording.
        bool g_recStampValid = false;
        std::uint32_t g_recStamp = 0;
        std::uint32_t g_recLogs = 0;
        std::uint32_t g_recFailures = 0;

        // Playback.
        bool g_playing = false;
        bool g_recorded = false;
        Block g_block;
        Totals g_totals;
        GateCounts g_gateTotal;
        bool g_playHave = false;
        std::uint32_t g_playLastMs = 0;
        bool g_playStampValid = false;
        std::uint32_t g_playStamp = 0;
        std::uint32_t g_playLines = 0;
        std::uint32_t g_playFailures = 0;

        // The scene.
        SceneSource g_source = SceneSource::None;
        std::uint32_t g_sceneLogs = 0;

        void Flush(std::ostream& log)
        {
            if (g_block.Empty())
                return;
            const GateCounts gate = TakeGate();
            g_gateTotal.Add(gate);
            g_totals.AddBlock(g_block);
            if (g_playLines < kLineCap)
            {
                ++g_playLines;
                log << BlockLine(g_block, g_recorded, gate, g_source) << '\n';
            }
            g_block = Block{};
        }
    }

    void Configure(const Config& config) { g_cfg = config; }
    const Config& GetConfig() { return g_cfg; }

    bool Install(std::string& error)
    {
        if (g_target)
            return true;
        if (!g_cfg.fix)
        {
            error = "off";
            return false;
        }
        g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        void* target = reinterpret_cast<std::uint8_t*>(g_base) + kGateRva;
        if (!mem::Matches(target, kGateSignature, sizeof(kGateSignature)))
        {
            error = "gate_signature_mismatch";
            return false;
        }
        if (!Prove(nullptr))
        {
            error = std::string("site_") + (g_failedSite ? g_failedSite : "?");
            return false;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&GateDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            return false;
        }
        g_detourBase.store(g_base, std::memory_order_relaxed);
        g_original.store(reinterpret_cast<GateFn>(trampoline), std::memory_order_release);
        status = replay::hooks::Enable(target);
        if (status != MH_OK)
        {
            MH_RemoveHook(target);
            g_original.store(nullptr, std::memory_order_release);
            error = std::string("mh_enable_") + MH_StatusToString(status);
            return false;
        }
        g_target = target;
        return true;
    }

    bool Installed() { return g_target != nullptr; }

    void Shutdown()
    {
        ClearScene();
        // Disable, wait for the calls in flight, then remove. MH_RemoveHook frees
        // the trampoline a call in flight may still be running (the unload drains
        // every detour first, but a drain that timed out would leave this as the
        // only wait).
        if (g_target)
            MH_DisableHook(g_target);
        for (int wait = 0; wait < 200 && g_inFlight.load(std::memory_order_seq_cst) > 0; ++wait)
            Sleep(1);
        if (g_target)
        {
            MH_RemoveHook(g_target);
            g_target = nullptr;
        }
        g_original.store(nullptr, std::memory_order_release);
    }

    void PublishScene(float x, float y, float z, SceneSource source, std::ostream& log)
    {
        if (!g_target || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        {
            ClearScene();
            return;
        }
        g_sceneX.store(x, std::memory_order_relaxed);
        g_sceneY.store(y, std::memory_order_relaxed);
        g_sceneZ.store(z, std::memory_order_relaxed);
        g_sceneAtMs.store(GetTickCount64(), std::memory_order_release);
        if (source != g_source && g_sceneLogs < 16)
        {
            ++g_sceneLogs;
            log << "WIND_REPLAY,phase=scene,source=" << SceneSourceName(source) << ",at=" << x << ';' << y << ';' << z << '\n';
        }
        g_source = source;
    }

    void ClearScene()
    {
        g_sceneAtMs.store(0, std::memory_order_release);
        g_source = SceneSource::None;
    }

    void Record(Recording& clip, std::uint32_t timeMs, bool clockRunning, std::ostream& log)
    {
        if (!g_cfg.log || !clockRunning)
            return;
        if (!Prove(&log))
            return;
        if (!SampleDue(clip.windAudio, timeMs))
            return;
        const Reading r = ReadNow();
        if (!r.ok)
        {
            if (g_recFailures++ < 8)
                log << "WIND_REPLAY,phase=record_skip,clip_ms=" << timeMs << '\n';
            return;
        }
        const bool first = clip.windAudio.Empty();
        // A new recording does not compare the stamp with the previous
        // recording's last sample (a false simple flag at 0).
        if (first)
            g_recStampValid = false;
        const Sample s = SampleOf(r, timeMs, g_recStampValid, g_recStamp);
        Append(clip.windAudio, s);
        if (first && g_recLogs < 8)
        {
            ++g_recLogs;
            log << "WIND_REPLAY,phase=rec,first_ms=" << timeMs << ",str=" << F3(s.strength)
                << ",lines=" << LinesText(s.lines, (s.flags & kSampleLines) != 0) << ",wx=" << WeatherText(r.wx[0]) << ';'
                << WeatherText(r.wx[1]) << '\n';
        }
    }

    void BeginPlayback(const Recording& clip, std::ostream& log)
    {
        g_playing = true;
        g_recorded = !clip.windAudio.Empty();
        g_block = Block{};
        g_totals = Totals{};
        g_gateTotal = GateCounts{};
        g_playHave = false;
        g_playLastMs = 0;
        g_playStampValid = false;
        g_playLines = 0;
        g_playFailures = 0;
        g_sceneLogs = 0;
        TakeGate(); // what came before belongs to the recording or the game
        const bool proven = Prove(&log);
        log << "WIND_REPLAY,phase=begin,fix=" << (g_cfg.fix ? 1 : 0) << ",detour=" << (g_target ? 1 : 0)
            << ",log=" << (g_cfg.log ? 1 : 0) << ",proven=" << (proven ? 1 : 0)
            << ",section=" << (g_recorded ? "WNDA" : "none") << ",rec_samples=" << clip.windAudio.samples.size() << '\n';
    }

    void Play(const Recording& clip, std::uint32_t timeMs, bool normal, std::ostream& log)
    {
        if (!g_playing || !g_cfg.log || !normal)
            return;
        if (!Prove(&log))
            return;
        // A loop or a seek back closes the block (a new stretch of clip time).
        if (g_playHave && timeMs < g_playLastMs)
        {
            Flush(log);
            g_playHave = false;
        }
        if (g_playHave && timeMs - g_playLastMs < kSamplePeriodMs)
            return;
        g_playHave = true;
        g_playLastMs = timeMs;
        const Reading r = ReadNow();
        if (!r.ok)
        {
            if (g_playFailures++ < 8)
                log << "WIND_REPLAY,phase=play_skip,clip_ms=" << timeMs << '\n';
            return;
        }
        const Sample s = SampleOf(r, timeMs, g_playStampValid, g_playStamp);
        const Sample* rec = g_recorded ? At(clip.windAudio, timeMs) : nullptr;
        if (!g_block.Empty() && timeMs - g_block.fromMs >= kBlockMs)
            Flush(log);
        g_block.Add(s, rec, r.wx);
        if (rec)
            g_totals.AddPair(rec->strength, s.strength);
    }

    void EndPlayback(const Recording& clip, std::ostream& log)
    {
        if (!g_playing)
            return;
        Flush(log);
        g_gateTotal.Add(TakeGate());
        log << TotalLine(g_totals, g_recorded, clip.windAudio.samples.size(), g_cfg.fix && g_target, g_gateTotal) << '\n';
        g_playing = false;
    }
}
