#include <intrin.h>

#include "speech_duck.h"

#include "guarded_memory.h"
#include "hook_batch.h"
#include "mix_scene_pool_core.h"
#include "module_lifetime.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

// Implementation of speech_duck.h. The reads go through replay::mem after the sites of
// speech_duck_core.h matched once; the detour is installed only after the
// driver's prologue and the same sites matched, and its writes run under SEH
// (a fault stops writing for the session and is counted).

namespace replay::speechduck
{
    namespace
    {
        using U = std::uint64_t;

        Config g_cfg;
        std::uintptr_t g_base = 0;

        enum class Proof : std::uint8_t { Unknown, Proven, Failed };
        Proof g_proof = Proof::Unknown;
        const char* g_failedSite = nullptr;
        std::uint32_t g_failedRva = 0;
        bool g_proofLogged = false;

        // ---------------------------------------------------------- detour

        using DriverFn = U(__fastcall*)(U, U, U, U);
        std::atomic<DriverFn> g_original{nullptr};
        void* g_target = nullptr;
        std::atomic<int> g_inFlight{0};

        constexpr std::uint64_t kFreshMs = 1000;
        std::atomic<bool> g_active{false};
        std::atomic<bool> g_faulted{false};
        std::atomic<std::uint64_t> g_publishedAt{0};
        std::atomic<float> g_level{0.0f};
        std::atomic<std::uint32_t> g_flags{0};
        std::atomic<bool> g_level2Known{false};
        std::atomic<float> g_level2{0.0f};
        std::atomic<std::uint64_t> g_calls{0};
        std::atomic<std::uint64_t> g_writes{0};
        std::atomic<std::uint64_t> g_faults{0};

        // SEH: no object with a destructor in here.
        void Inject()
        {
            __try
            {
                const float level = g_level.load(std::memory_order_relaxed);
                *reinterpret_cast<volatile float*>(g_base + kLevelRva) = level;
                if (level > 0.0f)
                {
                    const std::uint32_t flags = g_flags.load(std::memory_order_relaxed);
                    volatile std::uint8_t* bytes = reinterpret_cast<volatile std::uint8_t*>(g_base + kFlagsRva);
                    for (std::uint32_t n = 0; n < 4; ++n)
                        bytes[n] = static_cast<std::uint8_t>((flags >> n) & 1u);
                }
                if (g_level2Known.load(std::memory_order_relaxed))
                    *reinterpret_cast<volatile float*>(g_base + kLevel2Rva) = g_level2.load(std::memory_order_relaxed);
                g_writes.fetch_add(1, std::memory_order_relaxed);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_faults.fetch_add(1, std::memory_order_relaxed);
                g_faulted.store(true, std::memory_order_release);
                g_active.store(false, std::memory_order_release);
            }
        }

        U __fastcall DriverDetour(U a1, U a2, U a3, U a4)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            g_inFlight.fetch_add(1, std::memory_order_seq_cst);
            g_calls.fetch_add(1, std::memory_order_relaxed);
            if (g_active.load(std::memory_order_acquire))
            {
                const std::uint64_t at = g_publishedAt.load(std::memory_order_acquire);
                const std::uint64_t now = GetTickCount64();
                if (at && now >= at && now - at <= kFreshMs)
                    Inject();
            }
            U result = 0;
            if (const DriverFn original = g_original.load(std::memory_order_acquire))
                result = original(a1, a2, a3, a4);
            g_inFlight.fetch_sub(1, std::memory_order_seq_cst);
            return result;
        }

        // ---------------------------------------------------------- proof

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
                bool same = mem::Copy(code, g_base + site.rva, site.size);
                for (std::uint8_t n = 0; same && n < site.size; ++n)
                    same = code[n] == site.bytes[n];
                if (!same || (site.ripAt >= 0 && SiteTarget(site) != site.target))
                {
                    failedRva = site.rva;
                    return site.name;
                }
            }
            // The duck's start site (the driver's own call into the pool).
            const mixscene::pool::Driver* duck = mixscene::pool::DriverByHash(kDuckScene);
            if (!duck || !mixscene::pool::DriverConsistent(*duck))
            {
                failedRva = duck ? duck->callRva : 0;
                return "duck_start_table";
            }
            std::uint8_t call[5]{};
            if (!mem::Copy(call, g_base + duck->callRva, sizeof(call)) || std::memcmp(call, duck->callBytes, sizeof(call)) != 0)
            {
                failedRva = duck->callRva;
                return "duck_start_call";
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
                    *log << "SPEECH_DUCK,phase=proven,sites=" << kSiteCount << ",driver=0x" << std::hex << kDriverRva
                         << ",level=0x" << kLevelRva << ",flags=0x" << kFlagsRva << std::dec
                         << ",replay=" << (g_cfg.replay ? 1 : 0) << ",detour=" << (g_target ? 1 : 0) << '\n';
                else
                    *log << "SPEECH_DUCK,phase=unproven,site=" << (g_failedSite ? g_failedSite : "?") << ",rva=0x" << std::hex
                         << g_failedRva << std::dec << ",note=disabled\n";
            }
            return g_proof == Proof::Proven;
        }

        // ---------------------------------------------------------- reads

        struct State
        {
            bool read = false;
            std::uint64_t scene = 0;
            bool sceneRead = false;
            float vars[7] = {};
            bool has[7] = {};
            std::uint64_t scene2 = 0;
            bool scene2Read = false;
            bool hasLevel2Var = false;
            float level2Var = 0.0f;
            float level = 0.0f;
            float level2 = 0.0f;
            std::uint32_t flags = 0;
            std::uint8_t mute = 0;
            std::uint32_t mode = 0;
            std::int32_t block = 0;
            std::uint8_t skip = 0;
        };

        bool ReadSceneCopy(std::uint64_t scene, std::uint8_t (&copy)[mixscene::pool::kSceneSize])
        {
            return scene && mem::Copy(copy, scene, sizeof(copy));
        }

        State ReadState()
        {
            State s;
            s.read = mem::Read(g_base + kLevelRva, s.level) && mem::Read(g_base + kLevel2Rva, s.level2) &&
                mem::Read(g_base + kFlagsRva, s.flags) && mem::Read(g_base + kGateMuteRva, s.mute) &&
                mem::Read(g_base + kGateModeRva, s.mode) && mem::Read(g_base + kGateBlockRva, s.block) &&
                mem::Read(g_base + kGateSkipRva, s.skip);
            static std::uint8_t copy[mixscene::pool::kSceneSize];
            if (mem::Read(g_base + kSceneRefRva, s.scene) && ReadSceneCopy(s.scene, copy))
            {
                s.sceneRead = true;
                for (std::size_t n = 0; n < 7; ++n)
                    s.has[n] = mixscene::pool::VarOf(copy, kDuckVars[n], s.vars[n]);
            }
            if (mem::Read(g_base + kScene2RefRva, s.scene2) && ReadSceneCopy(s.scene2, copy))
            {
                s.scene2Read = true;
                s.hasLevel2Var = mixscene::pool::VarOf(copy, kVarLevel2, s.level2Var);
            }
            return s;
        }

        std::string VarsText(const State& s)
        {
            if (!s.sceneRead)
                return "none";
            static const char* kNames[7] = {"apply", "curve", "edc", "edd", "ede_sfx", "edf", "weather"};
            std::string text;
            char b[40];
            for (std::size_t n = 0; n < 7; ++n)
            {
                if (s.has[n])
                    std::snprintf(b, sizeof(b), "%s%s:%.3f", text.empty() ? "" : ";", kNames[n], static_cast<double>(s.vars[n]));
                else
                    std::snprintf(b, sizeof(b), "%s%s:none", text.empty() ? "" : ";", kNames[n]);
                text += b;
            }
            return text;
        }

        void StateLine(const char* phase, std::uint32_t clipMs, const State& s, std::ostream& log)
        {
            char b[200];
            std::snprintf(b, sizeof(b),
                "SPEECH_DUCK,phase=%s,event=window,clip_ms=%u,read=%d,scene=0x%llx,scene2=0x%llx,level2_var=%s,gates=mute:%u;"
                "mode:%u;block:%d;skip:%u,flags_now=0x%08x,",
                phase, clipMs, s.read ? 1 : 0, static_cast<unsigned long long>(s.scene), static_cast<unsigned long long>(s.scene2),
                s.hasLevel2Var ? std::to_string(s.level2Var).c_str() : "none", static_cast<unsigned>(s.mute),
                static_cast<unsigned>(s.mode), static_cast<int>(s.block), static_cast<unsigned>(s.skip), s.flags);
            log << b << "vars=" << VarsText(s);
        }

        // ---------------------------------------------------------- state

        std::uint64_t g_nextRecWall = 0;
        bool g_playing = false;
        Source g_source = Source::None;
        Input g_last;
        bool g_haveLast = false;
        std::uint64_t g_nextPlayWall = 0;
        std::uint64_t g_changes = 0;
        std::uint64_t g_lastWrites = 0;
        const char* g_idleWhy = nullptr;

        void Idle(const char* why, std::ostream& log)
        {
            const bool was = g_active.exchange(false, std::memory_order_acq_rel);
            if (was || g_idleWhy != why)
                log << "SPEECH_DUCK,phase=play,event=idle,why=" << why << '\n';
            g_idleWhy = why;
        }
    }

    void Configure(const Config& config) { g_cfg = config; }
    const Config& GetConfig() { return g_cfg; }

    bool Install(std::string& error)
    {
        if (g_target)
            return true;
        if (!g_cfg.replay)
        {
            error = "off";
            return false;
        }
        g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        void* target = reinterpret_cast<std::uint8_t*>(g_base) + kDriverRva;
        if (!mem::Matches(target, kDriverSignature, sizeof(kDriverSignature)))
        {
            error = "driver_signature_mismatch";
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
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&DriverDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            return false;
        }
        g_original.store(reinterpret_cast<DriverFn>(trampoline), std::memory_order_release);
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
        g_active.store(false, std::memory_order_release);
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

    void Record(const Recording& clip, std::uint32_t timeMs, bool clockRunning, std::ostream& log)
    {
        (void)clip;
        if (!g_cfg.log || !clockRunning)
            return;
        if (!g_base)
            g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (!Prove(&log))
            return;
        const std::uint64_t wall = GetTickCount64();
        if (wall < g_nextRecWall)
            return;
        g_nextRecWall = wall + 1000;
        StateLine("rec", timeMs, ReadState(), log);
        log << '\n';
    }

    void BeginPlayback(const Recording& clip, std::ostream& log)
    {
        g_playing = true;
        g_active.store(false, std::memory_order_release);
        g_faulted.store(false, std::memory_order_release);
        g_haveLast = false;
        g_changes = 0;
        g_nextPlayWall = 0;
        g_idleWhy = nullptr;
        g_lastWrites = g_writes.load(std::memory_order_relaxed);
        if (!g_base)
            g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const bool proven = Prove(&log);
        const mixscene::Track& track = clip.mixScenes;
        std::size_t conversations = 0, ducks = 0, ducks2 = 0;
        for (const mixscene::Instance& i : track.instances)
        {
            conversations += i.kind == mixscene::Kind::Conversation ? 1 : 0;
            ducks += i.hash == kDuckScene ? 1 : 0;
            ducks2 += i.hash == kDuck2Scene ? 1 : 0;
        }
        g_source = Plan(track, 0, g_cfg.derivedFlags).source;
        log << "SPEECH_DUCK,phase=play,event=begin,replay=" << (g_cfg.replay ? 1 : 0) << ",installed=" << (g_target ? 1 : 0)
            << ",proven=" << (proven ? 1 : 0) << ",source=" << SourceName(g_source)
            << ",mixs_flags=" << track.flags << ",derived_flags=" << FlagsText(g_cfg.derivedFlags)
            << ",conversations=" << conversations << ",duck_instances=" << ducks << ",duck2_instances=" << ducks2 << '\n';
    }

    void Play(const Recording& clip, std::uint32_t timeMs, bool owning, std::ostream& log)
    {
        if (!g_playing)
            return;
        if (!g_cfg.replay)
            return Idle("off", log);
        if (!g_target)
            return Idle("not_installed", log);
        if (g_source == Source::None)
            return Idle("no_section", log);
        if (!owning)
            return Idle("not_owning", log);
        if (g_faulted.load(std::memory_order_acquire))
            return Idle("fault", log);
        const Input in = Plan(clip.mixScenes, timeMs, g_cfg.derivedFlags);
        g_level.store(in.level, std::memory_order_relaxed);
        g_flags.store(in.flags, std::memory_order_relaxed);
        g_level2.store(in.level2, std::memory_order_relaxed);
        g_level2Known.store(in.level2Known, std::memory_order_relaxed);
        g_publishedAt.store(GetTickCount64(), std::memory_order_release);
        g_active.store(true, std::memory_order_release);
        g_idleWhy = nullptr;
        if (!g_haveLast || !SameInput(in, g_last))
        {
            const bool on = in.level > 0.0f;
            const bool wasOn = g_haveLast && g_last.level > 0.0f;
            ++g_changes;
            if (g_changes <= 200)
                log << "SPEECH_DUCK,phase=play,action=" << (on && !wasOn ? "start" : (!on && wasOn ? "stop" : "vars"))
                    << ",clip_ms=" << timeMs << ",source=" << SourceName(in.source) << ",level=" << in.level
                    << ",flags=" << FlagsText(in.flags) << ",level2=" << (in.level2Known ? std::to_string(in.level2) : std::string("live"))
                    << ",instance=" << in.instance << '\n';
            g_last = in;
            g_haveLast = true;
        }
        const std::uint64_t wall = GetTickCount64();
        if (g_cfg.log && wall >= g_nextPlayWall)
        {
            g_nextPlayWall = wall + 1000;
            const std::uint64_t writes = g_writes.load(std::memory_order_relaxed);
            StateLine("play", timeMs, ReadState(), log);
            log << ",replay_level=" << in.level << ",replay_flags=" << FlagsText(in.flags) << ",writes=" << (writes - g_lastWrites)
                << ",calls=" << g_calls.load(std::memory_order_relaxed) << '\n';
            g_lastWrites = writes;
        }
    }

    void EndPlayback(const Recording& clip, std::ostream& log)
    {
        (void)clip;
        if (!g_playing)
            return;
        g_active.store(false, std::memory_order_release);
        g_playing = false;
        log << "SPEECH_DUCK,phase=play,event=end,source=" << SourceName(g_source) << ",changes=" << g_changes
            << ",writes=" << g_writes.load() << ",faults=" << g_faults.load() << '\n';
    }

    void ForgetPlayback()
    {
        g_active.store(false, std::memory_order_release);
        g_playing = false;
    }
}
