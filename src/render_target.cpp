#include "render_target.h"

#include "capture_probe.h"
#include "guarded_memory.h"
#include "hook_batch.h"
#include "module_lifetime.h"

#include <windows.h>
#include <intrin.h>

#include <MinHook.h>

#include "main.h"
#include "natives.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace replay::rendertarget
{
    namespace
    {
        // ------------------------------------------------------------ ports

        enum Port : int
        {
            kPortRegister = 0,
            kPortLink,
            kPortRelease,
            kPortLaunch,
            kPortCount
        };

        struct PortInfo
        {
            const char* name;
            std::uint32_t rva; // build 1491.50
            const char* signature;
        };

        // Build 1491.50 (native handler pair "lea rdx, handler / mov rcx, native hash"
        // in the registration table at 0x105D6B4..0x105D788 and 0x10A6396):
        //  register: 0x98AF2BB6F62BD588 -> handler 0x1046C2C -> call 0x1056800 (only caller)
        //  link:     0x2F506B8556242DDB -> handler 0x1045EB4 -> jmp 0x1055280 (only caller)
        //  release:  0x0E692EE61761361F -> handler 0x1046C58 -> call 0x1056880 (only caller);
        //            it reads the script id at 0xEE5A10()+8 and uses the same
        //            manager 0x4077A70 as the register
        //  launch:   0xC1BCF31E975B3195 -> handler 0x106D8D8 -> call 0x109B178(&app, &entry);
        //            5 callers, all launch natives or their helpers
        // Register and link: the same bytes as the capture probe's C29 and C30.
        // Each signature is unique in .text.
        constexpr PortInfo kPorts[kPortCount] = {
            {"register", 0x1056800, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 40 8A F2 48 8B D9 E8 F6"},
            {"link", 0x1055280, "48 8B C4 48 89 58 08 57 48 83 EC 20 48 8D 50 18 C7 40 10 FF"},
            {"release", 0x1056880, "40 53 48 83 EC 20 48 8B D9 E8 82 F1 E8 FF 48 8B D3 48 8D 0D D8 11 02 03 44 8B 40 08"},
            {"uiapp_launch", 0x109B178, "48 83 EC 48 4C 8B C1 B8 02 00 00 00 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 2B 8B 02 4C 8D 4C 24 60"},
        };

        using RegisterFn = std::uint64_t(__fastcall*)(const char*, std::uint64_t, std::uint64_t, std::uint64_t);
        using LinkFn = std::uint64_t(__fastcall*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
        using ReleaseFn = std::uint64_t(__fastcall*)(const char*, std::uint64_t, std::uint64_t, std::uint64_t);
        using LaunchFn = std::uint64_t(__fastcall*)(const std::uint32_t*, const std::uint32_t*, std::uint64_t, std::uint64_t);

        std::atomic<void*> g_originals[kPortCount] = {};
        void* g_targets[kPortCount] = {};
        const char* g_status[kPortCount] = {"not_installed", "not_installed", "not_installed", "not_installed"};
        std::uintptr_t g_base = 0;
        bool g_installTried = false;
        Settings g_settings = [] {
            Settings s;
            s.uiApps = {Joaat("SHOP_BROWSING")};
            return s;
        }();

        // The replay's own natives (playback) are not world events.
        std::atomic<int> g_selfCall{0};
        struct SelfCall
        {
            SelfCall() { g_selfCall.fetch_add(1, std::memory_order_acq_rel); }
            ~SelfCall() { g_selfCall.fetch_sub(1, std::memory_order_acq_rel); }
        };

        // ------------------------------------------------------------ ring

        struct Raw
        {
            std::uint8_t kind = kRegister; // kRegister, kLink, kRelease, or kAppOpen for a launch
            std::uint8_t flag = 0;
            std::uint8_t accepted = 0;
            std::uint32_t hash = 0;  // link: model; launch: app
            std::uint32_t entry = 0; // launch: entry
            std::uint32_t caller = 0; // return address RVA (evidence)
            char name[kNameBytes] = {};
        };
        constexpr std::size_t kRingCap = 256;
        SRWLOCK g_ringLock = SRWLOCK_INIT;
        std::vector<Raw> g_ring; // guarded by g_ringLock
        std::atomic<std::uint32_t> g_ringDropped{0};
        std::atomic<std::uint64_t> g_calls[kPortCount] = {};
        std::atomic<std::uint64_t> g_selfCalls{0};

        std::uint32_t ToRva(void* address)
        {
            const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(address);
            return a >= g_base && a - g_base < 0x10000000ull ? static_cast<std::uint32_t>(a - g_base) : 0;
        }

        // A guarded copy of a C string (at most kNameBytes - 1 characters).
        bool ReadName(const char* address, char (&out)[kNameBytes])
        {
            std::memset(out, 0, sizeof(out));
            if (!address)
                return false;
            for (std::size_t n = 0; n + 1 < kNameBytes; ++n)
            {
                char c = 0;
                if (!replay::mem::Copy(&c, reinterpret_cast<std::uintptr_t>(address) + n, 1))
                    return false;
                out[n] = c;
                if (!c)
                    return true;
            }
            out[kNameBytes - 1] = 0;
            return true; // truncated
        }

        void Push(const Raw& raw)
        {
            AcquireSRWLockExclusive(&g_ringLock);
            if (g_ring.size() < kRingCap)
                g_ring.push_back(raw);
            else
                g_ringDropped.fetch_add(1, std::memory_order_relaxed);
            ReleaseSRWLockExclusive(&g_ringLock);
        }

        // ------------------------------------------------------------ detours

        std::uint64_t __fastcall RegisterDetour(const char* name, std::uint64_t flag, std::uint64_t a2, std::uint64_t a3)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first (audit C01)
            void* const returnAddress = _ReturnAddress();
            const RegisterFn original = reinterpret_cast<RegisterFn>(g_originals[kPortRegister].load(std::memory_order_acquire));
            const std::uint64_t result = original ? original(name, flag, a2, a3) : 0;
            g_calls[kPortRegister].fetch_add(1, std::memory_order_relaxed);
            const bool accepted = static_cast<std::uint8_t>(result) != 0;
            replay::captureprobe::NoteRenderTargetRegister(reinterpret_cast<std::uint64_t>(name),
                static_cast<std::uint8_t>(flag), accepted, returnAddress);
            if (g_selfCall.load(std::memory_order_acquire))
            {
                g_selfCalls.fetch_add(1, std::memory_order_relaxed);
                return result;
            }
            Raw raw;
            raw.kind = kRegister;
            raw.flag = static_cast<std::uint8_t>(flag) ? 1 : 0;
            raw.accepted = accepted ? 1 : 0;
            raw.caller = ToRva(returnAddress);
            ReadName(name, raw.name);
            Push(raw);
            return result;
        }

        std::uint64_t __fastcall LinkDetour(std::uint64_t model, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3)
        {
            replay::lifetime::DetourScope lifetime;
            void* const returnAddress = _ReturnAddress();
            const LinkFn original = reinterpret_cast<LinkFn>(g_originals[kPortLink].load(std::memory_order_acquire));
            const std::uint64_t result = original ? original(model, a1, a2, a3) : 0;
            g_calls[kPortLink].fetch_add(1, std::memory_order_relaxed);
            replay::captureprobe::NoteRenderTargetLink(static_cast<std::uint32_t>(model), returnAddress);
            if (g_selfCall.load(std::memory_order_acquire))
            {
                g_selfCalls.fetch_add(1, std::memory_order_relaxed);
                return result;
            }
            Raw raw;
            raw.kind = kLink;
            raw.hash = static_cast<std::uint32_t>(model);
            raw.caller = ToRva(returnAddress);
            Push(raw);
            return result;
        }

        std::uint64_t __fastcall ReleaseDetour(const char* name, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3)
        {
            replay::lifetime::DetourScope lifetime;
            void* const returnAddress = _ReturnAddress();
            // Read before the call (the engine may free a name it owns; the
            // scripts pass a literal).
            Raw raw;
            raw.kind = kRelease;
            raw.caller = ToRva(returnAddress);
            ReadName(name, raw.name);
            const ReleaseFn original = reinterpret_cast<ReleaseFn>(g_originals[kPortRelease].load(std::memory_order_acquire));
            const std::uint64_t result = original ? original(name, a1, a2, a3) : 0;
            g_calls[kPortRelease].fetch_add(1, std::memory_order_relaxed);
            if (g_selfCall.load(std::memory_order_acquire))
            {
                g_selfCalls.fetch_add(1, std::memory_order_relaxed);
                return result;
            }
            raw.accepted = static_cast<std::uint8_t>(result) != 0 ? 1 : 0;
            Push(raw);
            return result;
        }

        std::uint64_t __fastcall LaunchDetour(const std::uint32_t* app, const std::uint32_t* entry, std::uint64_t a2,
            std::uint64_t a3)
        {
            replay::lifetime::DetourScope lifetime;
            void* const returnAddress = _ReturnAddress();
            Raw raw;
            raw.kind = kAppOpen;
            raw.caller = ToRva(returnAddress);
            const bool appRead = app && replay::mem::Copy(&raw.hash, reinterpret_cast<std::uintptr_t>(app), 4);
            if (entry)
                replay::mem::Copy(&raw.entry, reinterpret_cast<std::uintptr_t>(entry), 4);
            const LaunchFn original = reinterpret_cast<LaunchFn>(g_originals[kPortLaunch].load(std::memory_order_acquire));
            const std::uint64_t result = original ? original(app, entry, a2, a3) : 0;
            g_calls[kPortLaunch].fetch_add(1, std::memory_order_relaxed);
            if (g_selfCall.load(std::memory_order_acquire))
            {
                g_selfCalls.fetch_add(1, std::memory_order_relaxed);
                return result;
            }
            raw.accepted = static_cast<std::uint8_t>(result & 0xFF);
            if (appRead && raw.hash)
                Push(raw);
            return result;
        }

        void* const kDetours[kPortCount] = {
            reinterpret_cast<void*>(&RegisterDetour),
            reinterpret_cast<void*>(&LinkDetour),
            reinterpret_cast<void*>(&ReleaseDetour),
            reinterpret_cast<void*>(&LaunchDetour),
        };

        // ------------------------------------------------------------ install

        bool BytesMatch(std::uintptr_t address, const char* text)
        {
            std::uint8_t code[64] = {};
            std::size_t count = 0;
            const char* p = text;
            std::uint8_t want[64] = {};
            bool wild[64] = {};
            while (*p && count < sizeof(want))
            {
                while (*p == ' ')
                    ++p;
                if (!*p)
                    break;
                if (p[0] == '?' && p[1] == '?')
                    wild[count] = true;
                else
                    want[count] = static_cast<std::uint8_t>(std::strtoul(std::string(p, 2).c_str(), nullptr, 16));
                ++count;
                p += 2;
            }
            if (!count || !replay::mem::Copy(code, address, count))
                return false;
            for (std::size_t n = 0; n < count; ++n)
                if (!wild[n] && code[n] != want[n])
                    return false;
            return true;
        }

        const char* HookPort(int port)
        {
            const std::uintptr_t address = g_base + kPorts[port].rva;
            if (!BytesMatch(address, kPorts[port].signature))
                return "signature_mismatch";
            void* target = reinterpret_cast<void*>(address);
            void* trampoline = nullptr;
            if (MH_CreateHook(target, kDetours[port], &trampoline) != MH_OK || !trampoline)
                return "create_failed";
            g_originals[port].store(trampoline, std::memory_order_release);
            if (replay::hooks::Enable(target) != MH_OK)
            {
                MH_RemoveHook(target);
                g_originals[port].store(nullptr, std::memory_order_release);
                return "enable_failed";
            }
            g_targets[port] = target;
            return "installed";
        }

        // ------------------------------------------------------------ script thread state

        Live g_live;
        AppPoll g_poll;
        bool g_recording = false;
        std::uint32_t g_recordEvents = 0;
        std::uint32_t g_recordApps = 0;

        // Playback: what the replay applied, and what it found already there
        // (never released or closed by it).
        State g_have;
        std::set<std::string> g_foreignTargets;
        std::set<std::uint32_t> g_foreignApps;
        bool g_playing = false;
        bool g_inputBlockLogged = false;
        struct PlayCounters
        {
            std::uint32_t registers = 0, registerRefused = 0, foreign = 0, links = 0, linkMissed = 0, releases = 0;
            std::uint32_t appOpens = 0, appRefused = 0, appForeign = 0, appCloses = 0, inputTicks = 0;
        } g_counters;

        std::string Hex(std::uint32_t value)
        {
            char text[16];
            std::snprintf(text, sizeof(text), "0x%08x", value);
            return text;
        }

        void LogEvent(std::ostream& log, const Event& e, const char* phase, std::uint32_t caller)
        {
            log << "RT_EVENT,phase=" << phase << ",t=" << e.timeMs << ",kind=" << KindName(e.kind)
                << ",name=" << (e.name[0] ? e.name : "-") << ",hash=" << Hex(e.hash) << ",entry=" << Hex(e.entry)
                << ",flag=" << static_cast<int>(e.flag) << ",accepted=" << static_cast<int>(e.accepted)
                << ",initial=" << static_cast<int>(e.initial) << ",caller=" << Hex(caller) << '\n';
        }

        // The frontend actions a UI app reads (catalog paging, accept, back). The free
        // camera reads its pad axes as disabled controls, so they keep working.
        // Whether the UI app's own input path honours these is not proven
        // (RT_INPUT_BLOCK).
        void BlockAppInput()
        {
            static const char* const kActions[] = {
                "INPUT_FRONTEND_ACCEPT", "INPUT_FRONTEND_CANCEL", "INPUT_FRONTEND_LEFT", "INPUT_FRONTEND_RIGHT",
                "INPUT_FRONTEND_UP", "INPUT_FRONTEND_DOWN", "INPUT_FRONTEND_LB", "INPUT_FRONTEND_RB",
                "INPUT_FRONTEND_X", "INPUT_FRONTEND_Y", "INPUT_FRONTEND_LT", "INPUT_FRONTEND_RT",
                "INPUT_GAME_MENU_ACCEPT", "INPUT_GAME_MENU_CANCEL", "INPUT_GAME_MENU_UP", "INPUT_GAME_MENU_DOWN",
                "INPUT_GAME_MENU_LEFT", "INPUT_GAME_MENU_RIGHT", "INPUT_GAME_MENU_TAB_LEFT", "INPUT_GAME_MENU_TAB_RIGHT",
            };
            static Hash hashes[sizeof(kActions) / sizeof(kActions[0])] = {};
            static bool ready = false;
            if (!ready)
            {
                for (std::size_t n = 0; n < sizeof(kActions) / sizeof(kActions[0]); ++n)
                    hashes[n] = Joaat(kActions[n]);
                ready = true;
            }
            for (const Hash hash : hashes)
            {
                PAD::DISABLE_CONTROL_ACTION(0, hash, TRUE);
                PAD::DISABLE_CONTROL_ACTION(2, hash, TRUE);
            }
        }

        void Execute(const Plan& plan, std::uint32_t timeMs, std::ostream& log)
        {
            SelfCall self;
            for (const App& a : plan.close)
            {
                const bool foreign = g_foreignApps.erase(a.app) != 0;
                if (!foreign)
                {
                    UIAPPS::_CLOSE_UIAPP_BY_HASH(a.app);
                    ++g_counters.appCloses;
                }
                g_have.apps.erase(std::remove_if(g_have.apps.begin(), g_have.apps.end(), [&](const App& h) { return h.app == a.app; }),
                    g_have.apps.end());
                log << "RT_PLAY,t=" << timeMs << ",op=app_close,app=" << Hex(a.app) << ",foreign=" << (foreign ? 1 : 0) << '\n';
            }
            for (const std::string& name : plan.release)
            {
                const bool foreign = g_foreignTargets.erase(name) != 0;
                BOOL released = FALSE;
                if (!foreign)
                {
                    released = HUD::RELEASE_NAMED_RENDERTARGET(name.c_str());
                    ++g_counters.releases;
                }
                Event e;
                e.kind = kRelease;
                e.SetName(name.c_str());
                g_have.Apply(e);
                log << "RT_PLAY,t=" << timeMs << ",op=release,name=" << name << ",foreign=" << (foreign ? 1 : 0)
                    << ",released=" << (released ? 1 : 0) << '\n';
            }
            for (const Target& t : plan.registers)
            {
                Event e;
                e.kind = kRegister;
                e.flag = t.flag;
                e.SetName(t.name.c_str());
                if (HUD::IS_NAMED_RENDERTARGET_REGISTERED(t.name.c_str()))
                {
                    // The world has it (a live script): left alone, never released.
                    g_foreignTargets.insert(t.name);
                    g_have.Apply(e);
                    ++g_counters.foreign;
                    log << "RT_PLAY,t=" << timeMs << ",op=register,name=" << t.name << ",status=foreign" << '\n';
                    continue;
                }
                const BOOL ok = HUD::REGISTER_NAMED_RENDERTARGET(t.name.c_str(), t.flag ? TRUE : FALSE);
                const int renderId = ok ? HUD::GET_NAMED_RENDERTARGET_RENDER_ID(t.name.c_str()) : 0;
                if (ok)
                    ++g_counters.registers;
                else
                {
                    // Not retried every tick: filed as foreign (nothing to release).
                    g_foreignTargets.insert(t.name);
                    ++g_counters.registerRefused;
                }
                g_have.Apply(e);
                log << "RT_PLAY,t=" << timeMs << ",op=register,name=" << t.name << ",flag=" << static_cast<int>(t.flag)
                    << ",status=" << (ok ? "registered" : "refused") << ",render_id=" << renderId << '\n';
            }
            for (const Link& l : plan.links)
            {
                Event e;
                e.kind = kLink;
                e.hash = l.model;
                e.SetName(l.target.c_str());
                if (!l.target.empty() && g_foreignTargets.count(l.target))
                {
                    g_have.Apply(e);
                    log << "RT_PLAY,t=" << timeMs << ",op=link,model=" << Hex(l.model) << ",target=" << l.target
                        << ",status=target_foreign" << '\n';
                    continue;
                }
                const bool valid = STREAMING::IS_MODEL_VALID(l.model) != 0;
                if (valid)
                    HUD::LINK_NAMED_RENDERTARGET(l.model);
                const bool linked = valid && HUD::IS_NAMED_RENDERTARGET_LINKED(l.model) != 0;
                if (linked)
                    ++g_counters.links;
                else
                    ++g_counters.linkMissed;
                // Filed either way: a link the engine refused is not retried every tick.
                g_have.links.erase(std::remove_if(g_have.links.begin(), g_have.links.end(), [&](const Link& h) { return h.model == l.model; }),
                    g_have.links.end());
                g_have.Apply(e);
                log << "RT_PLAY,t=" << timeMs << ",op=link,model=" << Hex(l.model) << ",target=" << (l.target.empty() ? "-" : l.target)
                    << ",model_valid=" << (valid ? 1 : 0) << ",linked=" << (linked ? 1 : 0) << '\n';
            }
            for (const App& a : plan.open)
            {
                Event e;
                e.kind = kAppOpen;
                e.hash = a.app;
                e.entry = a.entry;
                if (UIAPPS::IS_UIAPP_ACTIVE_BY_HASH(a.app))
                {
                    g_foreignApps.insert(a.app);
                    g_have.Apply(e);
                    ++g_counters.appForeign;
                    log << "RT_PLAY,t=" << timeMs << ",op=app_open,app=" << Hex(a.app) << ",status=foreign" << '\n';
                    continue;
                }
                const bool can = a.entry ? UIAPPS::CAN_LAUNCH_UIAPP_BY_HASH_WITH_ENTRY(a.app, a.entry) != 0
                                         : UIAPPS::CAN_LAUNCH_UIAPP_BY_HASH(a.app) != 0;
                int result = 0;
                if (can)
                    result = a.entry ? UIAPPS::LAUNCH_UIAPP_BY_HASH_WITH_ENTRY(a.app, a.entry) : UIAPPS::LAUNCH_UIAPP_BY_HASH(a.app);
                if (can)
                    ++g_counters.appOpens;
                else
                {
                    g_foreignApps.insert(a.app); // nothing to close; not retried every tick
                    ++g_counters.appRefused;
                }
                g_have.Apply(e);
                log << "RT_PLAY,t=" << timeMs << ",op=app_open,app=" << Hex(a.app) << ",entry=" << Hex(a.entry)
                    << ",status=" << (can ? "launched" : "refused") << ",result=" << result << '\n';
            }
        }

        void UndoAll(std::uint32_t timeMs, std::ostream& log)
        {
            Plan plan = Reconcile(g_have, State{}, true);
            if (!plan.Empty())
                Execute(plan, timeMs, log);
            g_have = State{};
            g_foreignTargets.clear();
            g_foreignApps.clear();
        }
    }

    void Configure(const Settings& settings) { g_settings = settings; }
    const Settings& Configured() { return g_settings; }

    bool Install(std::string& summary)
    {
        if (g_installTried)
        {
            summary = "already";
            return Installed();
        }
        g_installTried = true;
        if (!g_settings.record)
        {
            summary = "off";
            return false;
        }
        g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            summary = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        g_ring.reserve(kRingCap);
        int installed = 0;
        summary.clear();
        for (int port = 0; port < kPortCount; ++port)
        {
            g_status[port] = HookPort(port);
            installed += std::strcmp(g_status[port], "installed") == 0 ? 1 : 0;
            summary += std::string(port ? "," : "") + kPorts[port].name + "=" + g_status[port];
        }
        summary += ",ports=" + std::to_string(installed) + "/" + std::to_string(kPortCount);
        return installed > 0;
    }

    bool Installed()
    {
        for (int port = 0; port < kPortCount; ++port)
            if (g_targets[port])
                return true;
        return false;
    }

    bool OwnsProbePorts() { return g_targets[kPortRegister] && g_targets[kPortLink]; }

    void Shutdown()
    {
        for (int port = 0; port < kPortCount; ++port)
            if (g_targets[port])
                MH_DisableHook(g_targets[port]);
        // The unload disabled every hook and drained every detour before
        // this (module_lifetime.h, DetourScope); the trampolines go now.
        for (int port = 0; port < kPortCount; ++port)
            if (g_targets[port])
            {
                MH_RemoveHook(g_targets[port]);
                g_targets[port] = nullptr;
                g_originals[port].store(nullptr, std::memory_order_release);
            }
    }

    void Monitor(bool recording, std::uint32_t nowMs, Track& track, std::ostream& log)
    {
        std::vector<Raw> drained;
        AcquireSRWLockExclusive(&g_ringLock);
        drained.swap(g_ring);
        g_ring.reserve(kRingCap);
        ReleaseSRWLockExclusive(&g_ringLock);
        const std::uint32_t dropped = g_ringDropped.exchange(0, std::memory_order_relaxed);
        const bool filing = recording && g_recording;
        if (filing)
            track.dropped += dropped;
        for (const Raw& raw : drained)
        {
            Event e;
            e.timeMs = nowMs;
            e.kind = raw.kind;
            e.flag = raw.flag;
            e.accepted = raw.accepted;
            e.hash = raw.hash;
            e.entry = raw.entry;
            std::memcpy(e.name, raw.name, sizeof(e.name));
            switch (raw.kind)
            {
            case kRegister:
                g_live.Register(raw.name, raw.flag, raw.accepted != 0);
                break;
            case kLink:
                e.SetName(g_live.Link(raw.hash).c_str());
                break;
            case kRelease:
                g_live.Release(raw.name);
                break;
            case kAppOpen:
                // A launch: its entry for the poll (open and close are the poll's).
                g_poll.NoteLaunch(raw.hash, raw.entry);
                log << "RT_EVENT,phase=" << (filing ? "rec" : "live") << ",t=" << nowMs << ",kind=uiapp_launch,app="
                    << Hex(raw.hash) << ",entry=" << Hex(raw.entry) << ",result=" << static_cast<int>(raw.accepted)
                    << ",caller=" << Hex(raw.caller) << '\n';
                continue;
            default:
                continue;
            }
            LogEvent(log, e, filing ? "rec" : "live", raw.caller);
            if (filing)
            {
                track.events.push_back(e);
                ++g_recordEvents;
            }
        }
        if (!filing)
            return;
        for (const std::uint32_t app : g_poll.Apps())
        {
            Event e;
            if (!g_poll.Observe(app, UIAPPS::IS_UIAPP_ACTIVE_BY_HASH(app) != 0, e))
                continue;
            e.timeMs = nowMs;
            LogEvent(log, e, "rec", 0);
            track.events.push_back(e);
            ++g_recordApps;
        }
    }

    void BeginRecording(Track& track, std::ostream& log)
    {
        track.Clear();
        g_recordEvents = 0;
        g_recordApps = 0;
        g_recording = g_settings.record && Installed();
        g_poll.Reset(g_settings.uiApps);
        if (!g_recording)
        {
            log << "RT_RECORD_BEGIN,status=off,record=" << (g_settings.record ? 1 : 0) << ",installed=" << (Installed() ? 1 : 0) << '\n';
            return;
        }
        // The state found at the start (the initial state events).
        track.events = StateEvents(g_live.Targets(), 0);
        for (const Event& e : track.events)
            LogEvent(log, e, "rec", 0);
        for (const std::uint32_t app : g_poll.Apps())
        {
            Event e;
            if (!g_poll.Observe(app, UIAPPS::IS_UIAPP_ACTIVE_BY_HASH(app) != 0, e))
                continue;
            e.initial = 1;
            LogEvent(log, e, "rec", 0);
            track.events.push_back(e);
        }
        log << "RT_RECORD_BEGIN,status=on,initial=" << track.events.size() << ",polled_apps=" << g_poll.Apps().size()
            << ",ports=" << g_status[kPortRegister] << ';' << g_status[kPortLink] << ';' << g_status[kPortRelease] << ';'
            << g_status[kPortLaunch] << '\n';
    }

    void EndRecording(const Track& track, std::ostream& log)
    {
        if (!g_recording)
            return;
        g_recording = false;
        std::uint32_t kinds[kKindCount] = {};
        for (const Event& e : track.events)
            if (e.kind < kKindCount)
                ++kinds[e.kind];
        log << "RT_RECORD_TOTAL,events=" << track.events.size() << ",register=" << kinds[kRegister] << ",link=" << kinds[kLink]
            << ",release=" << kinds[kRelease] << ",app_open=" << kinds[kAppOpen] << ",app_close=" << kinds[kAppClose]
            << ",dropped=" << track.dropped << ",polled_apps=" << g_poll.Apps().size()
            << ",calls=" << g_calls[kPortRegister].load() << ';' << g_calls[kPortLink].load() << ';'
            << g_calls[kPortRelease].load() << ';' << g_calls[kPortLaunch].load() << '\n';
    }

    void BeginPlayback(const Track& track, std::ostream& log)
    {
        g_have = State{};
        g_foreignTargets.clear();
        g_foreignApps.clear();
        g_counters = {};
        g_inputBlockLogged = false;
        g_playing = true;
        log << "RT_PLAY_BEGIN,mode=" << ModeName(g_settings.mode) << ",events=" << track.events.size()
            << ",block_input=" << (g_settings.blockInput ? 1 : 0) << '\n';
    }

    void Play(const Track& track, std::uint32_t timeMs, std::ostream& log)
    {
        if (!g_playing || g_settings.mode == Mode::Off || (track.events.empty() && g_have.Empty()))
            return;
        State want = StateAt(track, timeMs);
        const bool apps = g_settings.mode == Mode::UiApp;
        if (apps)
            want.apps.erase(std::remove_if(want.apps.begin(), want.apps.end(),
                                [&](const App& a) { return !AppDrawsTarget(track, a.app, timeMs); }),
                want.apps.end());
        else
            want.apps.clear();
        const Plan plan = Reconcile(g_have, want, apps);
        if (!plan.Empty())
            Execute(plan, timeMs, log);
        // Input: only while an app the replay launched is open.
        bool ours = false;
        for (const App& a : g_have.apps)
            ours = ours || !g_foreignApps.count(a.app);
        if (ours && g_settings.blockInput)
        {
            BlockAppInput();
            ++g_counters.inputTicks;
            if (!g_inputBlockLogged)
            {
                g_inputBlockLogged = true;
                log << "RT_INPUT_BLOCK,t=" << timeMs << ",groups=0;2,actions=frontend;game_menu" << '\n';
            }
        }
    }

    void EndPlayback(std::ostream& log)
    {
        if (!g_playing)
            return;
        UndoAll(0xFFFFFFFFu, log);
        g_playing = false;
        log << "RT_PLAY_TOTAL,mode=" << ModeName(g_settings.mode) << ",registered=" << g_counters.registers
            << ",register_refused=" << g_counters.registerRefused << ",foreign=" << g_counters.foreign
            << ",linked=" << g_counters.links << ",link_missed=" << g_counters.linkMissed << ",released=" << g_counters.releases
            << ",app_opened=" << g_counters.appOpens << ",app_refused=" << g_counters.appRefused
            << ",app_foreign=" << g_counters.appForeign << ",app_closed=" << g_counters.appCloses
            << ",input_ticks=" << g_counters.inputTicks << ",self_calls=" << g_selfCalls.load() << '\n';
    }

    void ForgetAfterRestart()
    {
        g_have = State{};
        g_foreignTargets.clear();
        g_foreignApps.clear();
        g_playing = false;
    }

    bool BenchOp(const std::vector<std::string>& args, std::string& detail)
    {
        if (args.empty())
        {
            detail = "error=no_args";
            return false;
        }
        const std::string& op = args[0];
        const auto hashOf = [](const std::string& text) -> std::uint32_t {
            if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
                return static_cast<std::uint32_t>(std::strtoul(text.c_str() + 2, nullptr, 16));
            if (!text.empty() && (text[0] == '-' || (text[0] >= '0' && text[0] <= '9')))
                return static_cast<std::uint32_t>(std::strtol(text.c_str(), nullptr, 10));
            return Joaat(text.c_str());
        };
        char line[256];
        if (op == "register" && args.size() >= 2)
        {
            const BOOL flag = args.size() >= 3 && args[2] == "1";
            const BOOL ok = HUD::REGISTER_NAMED_RENDERTARGET(args[1].c_str(), flag);
            std::snprintf(line, sizeof(line), "op=register,name=%s,ok=%d,render_id=%d", args[1].c_str(), ok ? 1 : 0,
                ok ? HUD::GET_NAMED_RENDERTARGET_RENDER_ID(args[1].c_str()) : 0);
        }
        else if (op == "link" && args.size() >= 2)
        {
            const std::uint32_t model = hashOf(args[1]);
            HUD::LINK_NAMED_RENDERTARGET(model);
            std::snprintf(line, sizeof(line), "op=link,model=0x%08x,linked=%d", model, HUD::IS_NAMED_RENDERTARGET_LINKED(model) ? 1 : 0);
        }
        else if (op == "release" && args.size() >= 2)
        {
            const BOOL ok = HUD::RELEASE_NAMED_RENDERTARGET(args[1].c_str());
            std::snprintf(line, sizeof(line), "op=release,name=%s,ok=%d", args[1].c_str(), ok ? 1 : 0);
        }
        else if (op == "app_open" && args.size() >= 2)
        {
            const std::uint32_t app = hashOf(args[1]);
            const std::uint32_t entry = args.size() >= 3 ? hashOf(args[2]) : 0;
            const int result = entry ? UIAPPS::LAUNCH_UIAPP_BY_HASH_WITH_ENTRY(app, entry) : UIAPPS::LAUNCH_UIAPP_BY_HASH(app);
            std::snprintf(line, sizeof(line), "op=app_open,app=0x%08x,entry=0x%08x,result=%d,active=%d", app, entry, result,
                UIAPPS::IS_UIAPP_ACTIVE_BY_HASH(app) ? 1 : 0);
        }
        else if (op == "app_close" && args.size() >= 2)
        {
            const std::uint32_t app = hashOf(args[1]);
            UIAPPS::_CLOSE_UIAPP_BY_HASH(app);
            std::snprintf(line, sizeof(line), "op=app_close,app=0x%08x", app);
        }
        else if (op == "log" && args.size() >= 2)
        {
            const std::uint32_t model = args.size() >= 3 ? hashOf(args[2]) : 0;
            std::snprintf(line, sizeof(line), "op=log,name=%s,registered=%d,render_id=%d,model=0x%08x,linked=%d,shop_browsing=%d",
                args[1].c_str(), HUD::IS_NAMED_RENDERTARGET_REGISTERED(args[1].c_str()) ? 1 : 0,
                HUD::GET_NAMED_RENDERTARGET_RENDER_ID(args[1].c_str()), model,
                model && HUD::IS_NAMED_RENDERTARGET_LINKED(model) ? 1 : 0,
                UIAPPS::IS_UIAPP_ACTIVE_BY_HASH(Joaat("SHOP_BROWSING")) ? 1 : 0);
        }
        else
        {
            detail = "error=usage";
            return false;
        }
        detail = line;
        return true;
    }
}
