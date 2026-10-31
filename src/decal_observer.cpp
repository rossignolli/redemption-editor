#include "decal_observer.h"
#include "clip_sites.h" // kDecalWrapper, kDecalGenericCall
#include "decal_producer_gate.h"
#include "engine_calls.h"
#include "game_sig.h"
#include "guarded_memory.h"
#include "mod_clock.h"
#include "mod_identity.h"
#include "profiler.h"

#include <unordered_map>
#include <windows.h>

#include <MinHook.h>
#include "hook_batch.h"
#include "event_ring.h"
#include "module_lifetime.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace replay::decal
{
    namespace
    {
        namespace sig = replay::sig;

        // The rage decal core (manager, entity, settings, id, flag) -> id or 0.
        const sig::Signature kDecalCore{"decal_core",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 57 41 56 41 57 48 83 EC 20 4C 8B F2 41 8B F1 33 D2 49 8B E8 "
            "38 15 ?? ?? ?? ?? 48 8B F9 75 20 38 15 ?? ?? ?? ?? 74 08 38 15 ?? ?? ?? ?? 74 10 38 15 ?? ?? ?? ?? 75 08 "
            "38 15 ?? ?? ?? ?? 74 0A 38 54 24 60",
            0x2C2EF08};
        // Its take of a request ring entry (decal_add_guard.h): at most 0x400 in
        // flight, the cursor at manager+0xD75D0, the ring at manager+0xB5D0, entries
        // of 0x330, the settings copied in, the entity ref at entry+0x310.
        const sig::Signature kDecalCoreRing{"decal_core_ring", kDecalCore, sig::Offset{0x58},
            "41 B8 00 04 00 00 44 39 81 D8 75 0D 00 0F 83 ?? ?? ?? ?? 8B 81 D0 75 0D 00 48 81 C1 D0 B5 00 00 FF C0 89 81 "
            "00 C0 0C 00 41 3B C0 75 08 89 91 00 C0 0C 00 8B C2 48 98 41 BF 01 00 00 00 44 01 B9 08 C0 0C 00 48 8B D5 48 "
            "69 D8 30 03 00 00 48 03 D9 48 8B CB E8 ?? ?? ?? ?? 48 8D 8B 10 03 00 00",
            0x2C2EF60};
        // The generic wrapper and its call of the core (kDecalWrapper,
        // kDecalGenericCall) are in clip_sites.h: a clip names the call.
        // The pending ring's worker: an entry's settings info (+0x88), its entity
        // (+0x318) and the info's +0x72, read with no null check.
        const sig::Signature kDecalWorkerInfo{"decal_worker_info",
            "48 8B 83 88 00 00 00 48 8B BB 18 03 00 00 80 78 72 00 74 5C", 0x2C42D0C};
        // The game's decal add (weapon impacts, explosions, blood, ADD_DECAL) and
        // its load of the rage decal manager (a pointer) for the wrapper.
        const sig::Signature kDecalGameAdd{"decal_game_add",
            "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 48 83 EC 30 33 ED 49 8B D8", 0x125F9EC};
        const sig::Signature kDecalGameAddManager{"decal_game_add_manager", kDecalGameAdd, sig::Offset{0x150},
            "48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 5C 24 40", 0x125FB3C};
        const sig::Signature kDecalManager{"decal_manager", kDecalGameAddManager, sig::Rel32{0x03, 0x07}, "", 0x5B1F940};
        // The removal by id (manager, id).
        const sig::Signature kDecalRemove{"decal_remove",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 48 89 78 20 41 56 48 83 EC 20 44 8B F2 48 8D 99 60 39 00 00",
            0x2C49FCC};
        using RemoveFn = void(__fastcall*)(void*, int);
        // The projected decal add (13 arguments).
        const sig::Signature kDecalProjAdd{"decal_proj_add",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 41 54 41 55 41 56 41 57 48 8D A8 18 FD FF FF 48 81 EC B0 03 00 "
            "00 80 B9 92 90 01 00 00 45 8B F1 0F 29 70 B8 49 8B F8",
            0x140CF14};
        // Its reads of the stack arguments (engine_calls.h): a build that
        // changed them fails here.
        const sig::Signature kDecalProjAddArg5{"decal_proj_add_arg5", kDecalProjAdd, sig::Offset{0x131},
            "4C 8B BD 10 03 00 00", 0x140D045};
        const sig::Signature kDecalProjAddArg7{"decal_proj_add_arg7", kDecalProjAdd, sig::Offset{0x195},
            "4C 8B A5 20 03 00 00", 0x140D0A9};
        const sig::Signature kDecalProjAddArg6{"decal_proj_add_arg6", kDecalProjAdd, sig::Offset{0x1A0},
            "4C 8B AD 18 03 00 00", 0x140D0B4};
        const sig::Signature kDecalProjAddArg9{"decal_proj_add_arg9", kDecalProjAdd, sig::Offset{0x237},
            "0F B7 85 30 03 00 00", 0x140D14B};
        const sig::Signature kDecalProjAddArg8{"decal_proj_add_arg8", kDecalProjAdd, sig::Offset{0x254},
            "F3 0F 10 85 28 03 00 00", 0x140D168};
        const sig::Signature kDecalProjAddArg13{"decal_proj_add_arg13", kDecalProjAdd, sig::Offset{0x2A2},
            "48 8B 85 50 03 00 00", 0x140D1B6};
        const sig::Signature kDecalProjAddArg11{"decal_proj_add_arg11", kDecalProjAdd, sig::Offset{0x4F3},
            "83 BD 40 03 00 00 00", 0x140D407};
        const sig::Signature kDecalProjAddArg12{"decal_proj_add_arg12", kDecalProjAdd, sig::Offset{0x53F},
            "8B 9D 48 03 00 00", 0x140D453};
        const sig::Signature* const kDecalProjAddArgs[] = {&kDecalProjAddArg5, &kDecalProjAddArg7, &kDecalProjAddArg6,
            &kDecalProjAddArg9, &kDecalProjAddArg8, &kDecalProjAddArg13, &kDecalProjAddArg11, &kDecalProjAddArg12};
        // The rage decal callback that mirrors a decal on an entity: it hands the
        // projected decal manager (the object itself) to the projected add.
        const sig::Signature kDecalRageMirror{"decal_rage_mirror",
            "45 84 C9 0F 84 ?? ?? ?? ?? 48 8B C4 48 89 58 08 48 89 70 10 55 57 41 54", 0x124AC84};
        const sig::Signature kDecalRageProjManager{"decal_rage_proj_manager", kDecalRageMirror, sig::Offset{0x153},
            "48 8D 0D ?? ?? ?? ?? 83 4C 24 58 FF 45 8B CF 83 4C 24 50 FF 4D 8B C6 C0 E8 06", 0x124ADD7};
        const sig::Signature kDecalProjManager{"decal_proj_manager", kDecalRageProjManager, sig::Rel32{0x03, 0x07}, "",
            0x4F15040};
        const sig::Signature kDecalRageMirrorCall{"decal_rage_mirror_call", kDecalRageMirror, sig::Offset{0x1B3},
            "E8 ?? ?? ?? ?? 4C 8D 9C 24 20 01 00 00", 0x124AE37};
        // The blood pool add, and its call of the game's decal add.
        const sig::Signature kDecalPoolAdd{"decal_pool_add",
            "48 8B C4 48 89 58 10 48 89 70 18 48 89 48 08 55 57 41 54 41 55", 0x125CCF4};
        const sig::Signature kDecalPoolAddCall{"decal_pool_add_call", kDecalPoolAdd, sig::Offset{0x639},
            "E8 ?? ?? ?? ?? EB 02 33 C0", 0x125D32D};
        using ProjAddFn = void*(__fastcall*)(void*, const std::uint32_t*, void*, int, const float*, const float*, const float*,
            float, std::uint16_t, std::uint8_t, int, int, void*);
        // The alive test (manager, id) -> bool: the slot's generation word at
        // manager+0x948 against the id's low word.
        const sig::Signature kDecalAlive{"decal_alive",
            "85 D2 74 1A 8D 82 00 00 FF FF 44 8B C0 49 C1 E8 10 66 42 39 94 41 48 09 00 00", 0x2C3DD04};
        // The info lookup (table, hash*, index) -> record, and the game's add path
        // asking it with the info table: a null result or a record whose qword
        // +0x130 is 0 is refused before the add.
        const sig::Signature kDecalInfoLookup{"decal_info_lookup",
            "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 41 8B F8 48 83 C1 18 E8 ?? ?? ?? ??",
            0x2C3A3D8};
        const sig::Signature kDecalAddPath{"decal_add_path",
            "48 8B C4 48 89 58 10 48 89 78 18 55 41 54 41 55 41 56 41 57 48 8D A8 C8 FE FF FF", 0x125B97C};
        const sig::Signature kDecalInfoUse{"decal_info_use", kDecalAddPath, sig::Offset{0xF7},
            "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 0F 84 ?? ?? ?? ?? 48 83 B8 30 01 00 00 00 0F 84 ?? ?? "
            "?? ??",
            0x125BA73};
        const sig::Signature kDecalInfoTable{"decal_info_table", kDecalInfoUse, sig::Rel32{0x03, 0x07}, "", 0x3CE4F00};

        using CoreFn = int(__fastcall*)(void*, void*, void*, int, std::uint8_t);

        constexpr std::uint32_t kRingSize = 1024;
        using Slot = replay::ring::Slot<AddCall>; // event_ring.h
        Slot g_ring[kRingSize];
        std::atomic<std::uint64_t> g_next{0};
        std::atomic<CoreFn> g_original{nullptr};
        std::atomic<bool> g_enabled{false};
        std::atomic<std::int32_t> g_inFlight{0};
        void* g_target = nullptr;
        bool g_installed = false;
        std::uintptr_t g_base = 0;
        std::uintptr_t g_imageEnd = 0;
        LARGE_INTEGER g_frequency{};
        std::uintptr_t g_wrapper = 0;
        constexpr std::uint32_t kRemoveRing = 1024;
        using RemoveSlot = replay::ring::Slot<RemoveCall>; // event_ring.h
        RemoveSlot g_removes[kRemoveRing];
        std::atomic<std::uint64_t> g_nextRemove{0};
        std::atomic<RemoveFn> g_originalRemove{nullptr};
        void* g_removeTarget = nullptr;
        constexpr std::uint32_t kProjRing = 1024;
        using ProjSlot = replay::ring::Slot<ProjCall>; // event_ring.h
        ProjSlot g_proj[kProjRing];
        std::atomic<std::uint64_t> g_nextProj{0};
        std::atomic<ProjAddFn> g_originalProj{nullptr};
        void* g_projTarget = nullptr;
        // The deferred decal job's drain (queue) -> count drained. It walks the
        // 0x80-byte requests of its queue and adds each through the game's add
        // path, the game's decal add, the wrapper and the core, on a worker of the
        // task scheduler. It is the engine's producer into the request ring the
        // replay also produces into; the gate below keeps the two apart.
        const sig::Signature kDecalDrain{"decal_drain",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 57 48 83 EC 20 33 F6 48 8D A9 30 80 01 00 40 88 B1 18 80 01 "
            "00",
            0x1431DD4};
        using DrainFn = int(__fastcall*)(void*);
        std::atomic<DrainFn> g_originalDrain{nullptr};
        void* g_drainTarget = nullptr;
        ProducerGate g_gate;
        std::atomic<bool> g_gateEnabled{true}; // DecalGate=0 in the ini turns it off (A/B only)

        std::uint64_t QpcUs()
        {
            return replay::clock::NowUs(); // mod_clock.h (no 21-day overflow)
        }

        bool GuardedRead(std::uint8_t* out, const void* source, std::size_t size)
        {
            return replay::mem::Copy(out, source, size); // guarded_memory.h
        }

        // Set while the replay itself adds a decal (same thread, the detour
        // runs inside the call).
        thread_local bool t_replayAdd = false;
        // The same for a projected add (POSE_V2_PROJ_PLAYBACK_ENGINE
        // tells the engine's own projected adds from the replay's).
        thread_local bool t_replayProj = false;

        int __fastcall CoreDetour(void* manager, void* entity, void* settings, int id, std::uint8_t flag)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.decal_add");
            struct InFlight
            {
                InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            const CoreFn original = g_original.load(std::memory_order_acquire);
            const int result = original ? original(manager, entity, settings, id, flag) : 0;
            if (!g_enabled.load(std::memory_order_relaxed))
                return result;
            void* frames[kCallerDepth + 1] = {};
            const USHORT captured = RtlCaptureStackBackTrace(1, kCallerDepth + 1, frames, nullptr);
            replay::ring::Writer<AddCall> slot(g_ring, kRingSize, g_next);
            AddCall& call = slot.value;
            call.timeUs = QpcUs();
            call.entity = reinterpret_cast<std::uint64_t>(entity);
            call.id = static_cast<std::uint32_t>(id);
            call.result = static_cast<std::uint32_t>(result);
            call.flag = flag;
            call.threadId = GetCurrentThreadId();
            call.byReplay = t_replayAdd;
            for (std::uint32_t depth = 0; depth < kCallerDepth; ++depth)
            {
                const std::uintptr_t address = depth < captured ? reinterpret_cast<std::uintptr_t>(frames[depth]) : 0;
                call.callers[depth] = address >= g_base && address < g_imageEnd
                    ? static_cast<std::uint32_t>(address - g_base)
                    : 0;
            }
            if (!GuardedRead(call.settings, settings, kSettingsBytes))
                std::memset(call.settings, 0, kSettingsBytes);
            slot.Publish();
            return result;
        }
    }

    namespace
    {
        void* __fastcall ProjDetour(void* manager, const std::uint32_t* type, void* entity, int arg4, const float* position,
            const float* direction, const float* side, float size, std::uint16_t arg9, std::uint8_t arg10, int arg11, int arg12,
            void* owner)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            struct InFlight
            {
                InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            if (g_enabled.load(std::memory_order_relaxed))
            {
                void* frames[2] = {};
                const USHORT captured = RtlCaptureStackBackTrace(1, 2, frames, nullptr);
                const std::uintptr_t address = captured > 0 ? reinterpret_cast<std::uintptr_t>(frames[0]) : 0;
                replay::ring::Writer<ProjCall> slot(g_proj, kProjRing, g_nextProj);
                ProjCall& call = slot.value;
                call.timeUs = QpcUs();
                call.typeHash = 0;
                GuardedRead(reinterpret_cast<std::uint8_t*>(&call.typeHash), type, 4);
                call.entity = reinterpret_cast<std::uint64_t>(entity);
                call.arg4 = arg4;
                GuardedRead(reinterpret_cast<std::uint8_t*>(call.position), position, 16);
                GuardedRead(reinterpret_cast<std::uint8_t*>(call.direction), direction, 16);
                GuardedRead(reinterpret_cast<std::uint8_t*>(call.side), side, 16);
                call.size = size;
                call.arg9 = arg9;
                call.arg10 = arg10;
                call.arg11 = arg11;
                call.arg12 = arg12;
                call.owner = reinterpret_cast<std::uint64_t>(owner);
                call.caller = address >= g_base && address < g_imageEnd ? static_cast<std::uint32_t>(address - g_base) : 0;
                call.byReplay = t_replayProj;
                slot.Publish();
            }
            const ProjAddFn original = g_originalProj.load(std::memory_order_acquire);
            return original ? original(manager, type, entity, arg4, position, direction, side, size, arg9, arg10, arg11, arg12, owner)
                            : nullptr;
        }

        void __fastcall RemoveDetour(void* manager, int id)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            struct InFlight
            {
                InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            if (g_enabled.load(std::memory_order_relaxed))
            {
                void* frames[2] = {};
                const USHORT captured = RtlCaptureStackBackTrace(1, 2, frames, nullptr);
                const std::uintptr_t address = captured > 0 ? reinterpret_cast<std::uintptr_t>(frames[0]) : 0;
                replay::ring::Writer<RemoveCall> slot(g_removes, kRemoveRing, g_nextRemove);
                slot.value.timeUs = QpcUs();
                slot.value.id = static_cast<std::uint32_t>(id);
                slot.value.caller = address >= g_base && address < g_imageEnd ? static_cast<std::uint32_t>(address - g_base) : 0;
                slot.Publish();
            }
            const RemoveFn original = g_originalRemove.load(std::memory_order_acquire);
            if (original)
                original(manager, id);
        }
    }

    namespace
    {
        // The engine's drain holds the
        // producer gate for its whole pass; the replay's adds hold it around
        // the core (AddDecal, AddDecalWithId). Same thread as the engine's
        // producer, same ring, never both at once.
        int __fastcall DrainDetour(void* queue)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.decal_drain");
            struct InFlight
            {
                InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            const DrainFn original = g_originalDrain.load(std::memory_order_acquire);
            if (!original)
                return 0;
            if (!g_gateEnabled.load(std::memory_order_relaxed))
                return original(queue);
            GateScope gate(g_gate, GetCurrentThreadId());
            return original(queue);
        }

        void ReadGateIni()
        {
            char directory[MAX_PATH]{};
            GetCurrentDirectoryA(MAX_PATH, directory);
            const std::string ini = replay::identity::IniPathIn(directory);
            g_gateEnabled.store(replay::identity::IniInt("DecalGate", 1, ini.c_str()) != 0, std::memory_order_relaxed);
        }
    }

    bool DrainGateInstalled()
    {
        return g_drainTarget != nullptr && g_gateEnabled.load(std::memory_order_relaxed);
    }

    GateCounters GateStats()
    {
        return g_gate.Counters();
    }

    bool Install(std::string& error)
    {
        if (g_installed)
            return true;
        QueryPerformanceFrequency(&g_frequency);
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        g_base = reinterpret_cast<std::uintptr_t>(base);
        g_imageEnd = g_base + nt->OptionalHeader.SizeOfImage;
        void* target = reinterpret_cast<void*>(sig::Find(kDecalCore));
        if (!target)
        {
            error = "decal_core_signature_mismatch";
            return false;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&CoreDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            return false;
        }
        g_original.store(reinterpret_cast<CoreFn>(trampoline), std::memory_order_release);
        status = replay::hooks::Enable(target);
        if (status != MH_OK)
        {
            MH_RemoveHook(target);
            g_original.store(nullptr, std::memory_order_release);
            error = std::string("mh_enable_") + MH_StatusToString(status);
            return false;
        }
        g_target = target;
        g_installed = true;
        void* removeTarget = reinterpret_cast<void*>(sig::Find(kDecalRemove));
        if (removeTarget)
        {
            void* removeTrampoline = nullptr;
            if (MH_CreateHook(removeTarget, reinterpret_cast<void*>(&RemoveDetour), &removeTrampoline) == MH_OK && removeTrampoline)
            {
                g_originalRemove.store(reinterpret_cast<RemoveFn>(removeTrampoline), std::memory_order_release);
                if (replay::hooks::Enable(removeTarget) == MH_OK)
                    g_removeTarget = removeTarget;
                else
                {
                    MH_RemoveHook(removeTarget);
                    g_originalRemove.store(nullptr, std::memory_order_release);
                }
            }
        }
        if (!g_removeTarget)
            error = "decal_remove_observer_unavailable";
        void* projTarget = reinterpret_cast<void*>(sig::Find(kDecalProjAdd));
        if (projTarget)
        {
            void* projTrampoline = nullptr;
            if (MH_CreateHook(projTarget, reinterpret_cast<void*>(&ProjDetour), &projTrampoline) == MH_OK && projTrampoline)
            {
                g_originalProj.store(reinterpret_cast<ProjAddFn>(projTrampoline), std::memory_order_release);
                if (replay::hooks::Enable(projTarget) == MH_OK)
                    g_projTarget = projTarget;
                else
                {
                    MH_RemoveHook(projTarget);
                    g_originalProj.store(nullptr, std::memory_order_release);
                }
            }
        }
        if (!g_projTarget && error.empty())
            error = "proj_observer_unavailable";
        // The drain of the deferred decal job, so the replay's adds never run
        // beside the engine's.
        ReadGateIni();
        void* drainTarget = reinterpret_cast<void*>(sig::Find(kDecalDrain));
        if (drainTarget)
        {
            void* drainTrampoline = nullptr;
            if (MH_CreateHook(drainTarget, reinterpret_cast<void*>(&DrainDetour), &drainTrampoline) == MH_OK && drainTrampoline)
            {
                g_originalDrain.store(reinterpret_cast<DrainFn>(drainTrampoline), std::memory_order_release);
                if (replay::hooks::Enable(drainTarget) == MH_OK)
                    g_drainTarget = drainTarget;
                else
                {
                    MH_RemoveHook(drainTarget);
                    g_originalDrain.store(nullptr, std::memory_order_release);
                }
            }
        }
        if (!g_drainTarget && error.empty())
            error = "decal_drain_gate_unavailable";
        return true;
    }

    std::uint64_t TotalProj()
    {
        return g_nextProj.load(std::memory_order_relaxed);
    }

    void CollectProjSince(std::uint64_t& cursor, std::vector<ProjCall>& out)
    {
        out.clear();
        replay::ring::ForEachSince(g_proj, kProjRing, g_nextProj, cursor, [&](const ProjCall& call) { out.push_back(call); });
    }

    namespace
    {
        bool GuardedProjAdd(ProjAddFn fn, void* manager, const ProjCall& call, void* entity, std::uint32_t& code)
        {
            __try
            {
                alignas(16) float position[4];
                alignas(16) float direction[4];
                alignas(16) float side[4];
                std::memcpy(position, call.position, sizeof(position));
                std::memcpy(direction, call.direction, sizeof(direction));
                std::memcpy(side, call.side, sizeof(side));
                const std::uint32_t type = call.typeHash;
                t_replayProj = true;
                void* added = nullptr;
                const bool called = engine::Call(engine::kDecalProjAdd, reinterpret_cast<std::uintptr_t>(fn), added, manager,
                    &type, entity, call.arg4, position, direction, side, call.size, call.arg9, call.arg10, call.arg11,
                    call.arg12, reinterpret_cast<void*>(call.owner));
                t_replayProj = false;
                return called || engine::Failed(code);
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                t_replayProj = false;
                return false;
            }
        }
    }

    bool AddProjDecal(const ProjCall& call, void* entity, std::string& error)
    {
        ProjAddFn fn = g_originalProj.load(std::memory_order_acquire);
        if (!fn)
        {
            error = "proj_not_hooked";
            return false;
        }
        for (const sig::Signature* site : kDecalProjAddArgs)
        {
            if (!sig::Find(*site))
            {
                error = "proj_add_signature_mismatch";
                return false;
            }
        }
        void* manager = reinterpret_cast<void*>(sig::Find(kDecalProjManager));
        if (!manager)
        {
            error = "proj_manager_signature_mismatch";
            return false;
        }
        std::uint32_t code = 0;
        if (!GuardedProjAdd(fn, manager, call, entity, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        return true;
    }

    std::uint64_t TotalRemoves()
    {
        return g_nextRemove.load(std::memory_order_relaxed);
    }

    void CollectRemovesSince(std::uint64_t& cursor, std::vector<RemoveCall>& out)
    {
        out.clear();
        replay::ring::ForEachSince(g_removes, kRemoveRing, g_nextRemove, cursor, [&](const RemoveCall& call) { out.push_back(call); });
    }

    namespace
    {
        bool GuardedRemove(std::uintptr_t remove, void* manager, int id, std::uint32_t& code)
        {
            // Through the trampoline when observed (no event of ours).
            const RemoveFn fn = g_originalRemove.load(std::memory_order_acquire);
            return engine::Call(engine::kDecalRemove, fn ? reinterpret_cast<std::uintptr_t>(fn) : remove, manager, id) ||
                engine::Failed(code);
        }
    }

    namespace
    {
        bool ManagerPointer(std::uint64_t& manager, std::string& error)
        {
            const std::uintptr_t at = sig::Find(kDecalManager);
            std::uint8_t pointer[8] = {};
            if (!at || !GuardedRead(pointer, reinterpret_cast<const void*>(at), 8))
            {
                error = "decal_manager_unreadable";
                return false;
            }
            std::memcpy(&manager, pointer, 8);
            if (!manager)
                error = "decal_manager_null";
            return manager != 0;
        }

        // The checks before a decal reaches the core (decal_add_guard.h). Script
        // thread only (both adds).
        guard::Refusals g_guardRefusals;
        RingStale g_lastRingStale;

        // What the checks read and the field the replay writes into a settings
        // copy (the info, +0x88) are in the bytes of these: the core's ring take,
        // the worker's reads, the add path's info test.
        bool LayoutProven(std::string& error)
        {
            static const bool proven = sig::Find(kDecalCoreRing) && sig::Find(kDecalWorkerInfo) && sig::Find(kDecalInfoUse);
            if (!proven)
                error = "decal_layout_signature_mismatch";
            return proven;
        }

        bool CheckInfo(const std::uint8_t* settings, std::string& error)
        {
            std::uint64_t info = 0;
            std::memcpy(&info, settings + kSettingsInfo, sizeof(info));
            std::uint64_t data = 0;
            const bool read = info >= guard::kLowestPointer &&
                GuardedRead(reinterpret_cast<std::uint8_t*>(&data), reinterpret_cast<const void*>(info + guard::kInfoDataOffset),
                    sizeof(data));
            const guard::InfoVerdict verdict = guard::JudgeInfo(info, read, data);
            if (verdict == guard::InfoVerdict::Ok)
                return true;
            guard::Count(g_guardRefusals, verdict);
            error = guard::InfoVerdictText(verdict);
            return false;
        }

        // The id the core will count on: in the table and alive (a dead id
        // bumps a free-list link, one past the table any manager field).
        bool CheckId(std::uint64_t manager, std::uint32_t id, std::string& error)
        {
            const std::int32_t slot = guard::IdSlot(id);
            if (slot < 0)
            {
                ++g_guardRefusals.idOutOfRange;
                error = "id_out_of_range";
                return false;
            }
            std::uint16_t generation = 0;
            if (!GuardedRead(reinterpret_cast<std::uint8_t*>(&generation),
                    reinterpret_cast<const void*>(manager + guard::kIdGenerationOffset + static_cast<std::uint64_t>(slot) * 2),
                    sizeof(generation)) ||
                !guard::IdAlive(id, generation))
            {
                ++g_guardRefusals.idDead;
                error = "id_not_alive";
                return false;
            }
            return true;
        }

        // The ring entry the core takes next: its previous entity is unlinked
        // first (the ref assign's unlink writes [previous+8]); a value that is
        // not a readable pointer is refused. Under the producer gate the cursor
        // does not move between this and the add.
        bool CheckRingEntry(std::uint64_t manager, std::string& error)
        {
            std::uint32_t cursor = 0;
            std::uint64_t previous = 0;
            std::uint32_t index = 0;
            bool read = GuardedRead(reinterpret_cast<std::uint8_t*>(&cursor),
                reinterpret_cast<const void*>(manager + guard::kRingCursorOffset), sizeof(cursor));
            if (read)
            {
                index = guard::NextRingIndex(cursor);
                read = GuardedRead(reinterpret_cast<std::uint8_t*>(&previous),
                    reinterpret_cast<const void*>(guard::RingEntityAddress(static_cast<std::uintptr_t>(manager), index)), sizeof(previous));
            }
            std::uint8_t probe[16] = {};
            const bool readable = previous >= guard::kLowestPointer &&
                GuardedRead(probe, reinterpret_cast<const void*>(previous), sizeof(probe));
            if (read && guard::RingRefPlausible(previous, readable))
                return true;
            ++g_guardRefusals.ringStale;
            g_lastRingStale.index = index;
            g_lastRingStale.previous = previous;
            g_lastRingStale.readFailed = !read;
            error = read ? "ring_stale_ref" : "ring_unreadable";
            return false;
        }

        bool GuardedCoreAdd(CoreFn fn, void* manager, void* entity, std::uint8_t* settings, int id, std::uint8_t flag, int& result,
            std::uint32_t& code)
        {
            return engine::Call(engine::kDecalCore, reinterpret_cast<std::uintptr_t>(fn), result, manager, entity, settings, id,
                       flag) ||
                engine::Failed(code);
        }

        bool GuardedAlive(std::uintptr_t fn, void* manager, int id, bool& alive, std::uint32_t& code)
        {
            return engine::Call(engine::kDecalAlive, fn, alive, manager, id) || engine::Failed(code);
        }
    }

    bool AddDecalWithId(void* entity, const std::uint8_t settings[kSettingsBytes], std::uint32_t id, std::uint8_t flag,
        std::string& error)
    {
        const CoreFn core = g_original.load(std::memory_order_acquire);
        if (!core)
        {
            error = "decal_core_not_hooked";
            return false;
        }
        if (!LayoutProven(error))
            return false;
        // The id check reads the generation words the alive test reads.
        if (!sig::Find(kDecalAlive))
        {
            error = "decal_alive_signature_mismatch";
            return false;
        }
        if (!CheckInfo(settings, error))
            return false;
        std::uint64_t manager = 0;
        if (!ManagerPointer(manager, error))
            return false;
        alignas(16) std::uint8_t copy[kSettingsBytes];
        std::memcpy(copy, settings, kSettingsBytes);
        int result = 0;
        std::uint32_t code = 0;
        bool called = false;
        bool refused = false;
        if (g_gateEnabled.load(std::memory_order_relaxed))
        {
            // Never beside the engine's drain.
            GateScope gate(g_gate, GetCurrentThreadId());
            // Id and ring entry checked under the gate.
            refused = !CheckId(manager, id, error) || !CheckRingEntry(manager, error);
            if (!refused)
                called = GuardedCoreAdd(core, reinterpret_cast<void*>(manager), entity, copy, static_cast<int>(id), flag, result, code);
        }
        else
        {
            refused = !CheckId(manager, id, error) || !CheckRingEntry(manager, error);
            if (!refused)
                called = GuardedCoreAdd(core, reinterpret_cast<void*>(manager), entity, copy, static_cast<int>(id), flag, result, code);
        }
        if (refused)
            return false;
        if (!called)
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        if (!result)
            error = "refused";
        return result != 0;
    }

    bool IsDecalAlive(std::uint32_t id, std::string& error)
    {
        const std::uintptr_t aliveTest = sig::Find(kDecalAlive);
        if (!aliveTest)
        {
            error = "decal_alive_signature_mismatch";
            return false;
        }
        std::uint64_t manager = 0;
        if (!ManagerPointer(manager, error))
            return false;
        bool alive = false;
        std::uint32_t code = 0;
        if (!GuardedAlive(aliveTest, reinterpret_cast<void*>(manager), static_cast<int>(id), alive, code))
        {
            error = "exception";
            return false;
        }
        return alive;
    }

    bool RemoveDecal(std::uint32_t id, std::string& error)
    {
        const std::uintptr_t remove = sig::Find(kDecalRemove);
        if (!g_removeTarget && !remove)
        {
            error = "decal_remove_signature_mismatch";
            return false;
        }
        const std::uintptr_t managerAt = sig::Find(kDecalManager);
        std::uint8_t pointer[8] = {};
        if (!managerAt || !GuardedRead(pointer, reinterpret_cast<const void*>(managerAt), 8))
        {
            error = "decal_manager_unreadable";
            return false;
        }
        std::uint64_t manager = 0;
        std::memcpy(&manager, pointer, 8);
        std::uint32_t code = 0;
        if (!manager || !GuardedRemove(remove, reinterpret_cast<void*>(manager), static_cast<int>(id), code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = manager ? text : "decal_manager_null";
            return false;
        }
        return true;
    }

    void SetEnabled(bool enabled)
    {
        g_enabled.store(enabled, std::memory_order_relaxed);
    }

    std::uint64_t NowUs()
    {
        return replay::clock::NowUs(); // mod_clock.h (no 21-day overflow)
    }

    std::uint64_t TotalCalls()
    {
        return g_next.load(std::memory_order_relaxed);
    }

    void CollectSince(std::uint64_t& cursor, std::vector<AddCall>& out)
    {
        out.clear();
        replay::ring::ForEachSince(g_ring, kRingSize, g_next, cursor, [&](const AddCall& call) { out.push_back(call); });
    }

    namespace
    {
        constexpr std::size_t kInfoRecordBytes = 0x160;
        std::uintptr_t g_infoLookup = 0;
        std::uintptr_t g_infoTable = 0;
        bool g_infoLookupChecked = false;
        std::unordered_map<std::uint64_t, InfoKey> g_infoKeys;

        template <typename T>
        bool ReadValue(std::uintptr_t address, T& value)
        {
            return GuardedRead(reinterpret_cast<std::uint8_t*>(&value), reinterpret_cast<const void*>(address), sizeof(T));
        }

        bool GuardedLookup(std::uintptr_t fn, void* table, const std::uint32_t* hash, int index, void*& out,
            std::uint32_t& code)
        {
            return engine::Call(engine::kDecalInfoLookup, fn, out, table, hash, index) || engine::Failed(code);
        }
    }

    InfoKey InfoKeyOf(std::uint64_t info)
    {
        InfoKey key;
        if (!info)
            return key;
        const auto cached = g_infoKeys.find(info);
        if (cached != g_infoKeys.end())
            return cached->second;
        const std::uintptr_t table = sig::Find(kDecalInfoTable);
        if (!table)
            return key;
        // Map at table+0x18: pairs at [+0x20] ({u32 hash, pad, u64 value}),
        // u16 count at +0x28; value = {u64 base, u16 count} of 0x160 B records.
        std::uint64_t pairs = 0;
        std::uint16_t count = 0;
        if (ReadValue(table + 0x20, pairs) && ReadValue(table + 0x28, count) && pairs && count <= 8192)
        {
            for (std::uint16_t n = 0; n < count && !key.known; ++n)
            {
                std::uint32_t hash = 0;
                std::uint64_t value = 0;
                std::uint64_t base = 0;
                std::uint16_t records = 0;
                if (!ReadValue(pairs + n * 16ull, hash) || !ReadValue(pairs + n * 16ull + 8, value) || !value ||
                    !ReadValue(value, base) || !ReadValue(value + 8, records) || !base)
                    continue;
                if (info >= base && info < base + records * kInfoRecordBytes && (info - base) % kInfoRecordBytes == 0)
                {
                    key.known = true;
                    key.hash = hash;
                    key.index = static_cast<std::int32_t>((info - base) / kInfoRecordBytes);
                }
            }
        }
        g_infoKeys[info] = key;
        return key;
    }

    void ClearInfoKeys() { g_infoKeys.clear(); }

    std::uint64_t InfoPointerOf(const InfoKey& key, std::string& error)
    {
        if (!key.known)
        {
            error = "info_key_unknown";
            return 0;
        }
        if (!g_infoLookupChecked)
        {
            g_infoLookupChecked = true;
            g_infoTable = sig::Find(kDecalInfoTable);
            if (g_infoTable)
                g_infoLookup = sig::Find(kDecalInfoLookup);
        }
        if (!g_infoLookup)
        {
            error = "info_lookup_signature_mismatch";
            return 0;
        }
        void* out = nullptr;
        std::uint32_t code = 0;
        const std::uint32_t hash = key.hash;
        if (!GuardedLookup(g_infoLookup, reinterpret_cast<void*>(g_infoTable), &hash, key.index, out, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return 0;
        }
        if (!out)
            error = "info_not_found";
        return reinterpret_cast<std::uint64_t>(out);
    }

    bool EnsurePlayer(std::string& error)
    {
        if (g_wrapper)
            return true;
        const std::uintptr_t wrapper = sig::Find(kDecalWrapper);
        if (!wrapper)
        {
            error = "decal_wrapper_signature_mismatch";
            return false;
        }
        g_wrapper = wrapper;
        return true;
    }

    namespace
    {
        bool GuardedAdd(void* manager, void* entity, std::uint8_t* settings, std::uint8_t flag, int& id, std::uint32_t& code)
        {
            t_replayAdd = true;
            const bool called = engine::Call(engine::kDecalWrapper, g_wrapper, id, manager, entity, settings, flag);
            t_replayAdd = false;
            return called || engine::Failed(code);
        }
    }

    bool IsPedEntity(std::uint64_t entity)
    {
        std::uint8_t type = 0;
        return entity && GuardedRead(&type, reinterpret_cast<const void*>(entity + 0x30), 1) && type == 4;
    }

    std::uint32_t AddDecal(void* entity, const std::uint8_t settings[kSettingsBytes], std::uint8_t flag, std::string& error)
    {
        if (!EnsurePlayer(error))
            return 0;
        if (!LayoutProven(error))
            return 0;
        if (!CheckInfo(settings, error))
            return 0;
        const std::uintptr_t managerAt = sig::Find(kDecalManager);
        std::uint8_t pointer[8] = {};
        if (!managerAt || !GuardedRead(pointer, reinterpret_cast<const void*>(managerAt), 8))
        {
            error = "decal_manager_unreadable";
            return 0;
        }
        std::uint64_t manager = 0;
        std::memcpy(&manager, pointer, 8);
        if (!manager)
        {
            error = "decal_manager_null";
            return 0;
        }
        alignas(16) std::uint8_t copy[kSettingsBytes];
        std::memcpy(copy, settings, kSettingsBytes);
        int id = 0;
        std::uint32_t code = 0;
        bool called = false;
        bool refused = false;
        if (g_gateEnabled.load(std::memory_order_relaxed))
        {
            // Never beside the engine's drain.
            GateScope gate(g_gate, GetCurrentThreadId());
            // The ring entry checked under the gate.
            refused = !CheckRingEntry(manager, error);
            if (!refused)
                called = GuardedAdd(reinterpret_cast<void*>(manager), entity, copy, flag, id, code);
        }
        else
        {
            refused = !CheckRingEntry(manager, error);
            if (!refused)
                called = GuardedAdd(reinterpret_cast<void*>(manager), entity, copy, flag, id, code);
        }
        if (refused)
            return 0;
        if (!called)
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return 0;
        }
        if (id == 0)
            error = "refused";
        return static_cast<std::uint32_t>(id);
    }

    guard::Refusals GuardRefusals() { return g_guardRefusals; }
    RingStale LastRingStale() { return g_lastRingStale; }

    namespace
    {
        // The RVA the return of the call at `call` (5 bytes) has, or `missing`
        // (above every RVA) when the game lacks the call.
        std::uint32_t ReturnRva(const sig::Signature& call, std::uint32_t missing)
        {
            const std::uintptr_t at = sig::Find(call);
            const std::uintptr_t base = sig::ImageBase();
            return at && base ? static_cast<std::uint32_t>(at + 5 - base) : missing;
        }

        struct ReturnRvas
        {
            std::uint32_t generic;
            std::uint32_t rageMirror;
            std::uint32_t poolAdd;
        };

        // Once, a value of its own for each site the game lacks.
        const ReturnRvas& Returns()
        {
            static const ReturnRvas returns{ReturnRva(kDecalGenericCall, 0xFFFFFFFFu),
                ReturnRva(kDecalRageMirrorCall, 0xFFFFFFFEu), ReturnRva(kDecalPoolAddCall, 0xFFFFFFFDu)};
            return returns;
        }
    }

    std::uint32_t GenericReturn() { return Returns().generic; }
    std::uint32_t RageMirrorProjReturn() { return Returns().rageMirror; }
    std::uint32_t PoolAddReturn() { return Returns().poolAdd; }

    void Shutdown()
    {
        g_enabled.store(false, std::memory_order_relaxed);
        if (g_drainTarget)
        {
            MH_DisableHook(g_drainTarget);
            MH_RemoveHook(g_drainTarget);
            g_drainTarget = nullptr;
        }
        if (g_projTarget)
        {
            MH_DisableHook(g_projTarget);
            MH_RemoveHook(g_projTarget);
            g_projTarget = nullptr;
        }
        if (g_removeTarget)
        {
            MH_DisableHook(g_removeTarget);
            MH_RemoveHook(g_removeTarget);
            g_removeTarget = nullptr;
        }
        if (g_installed && g_target)
        {
            MH_DisableHook(g_target);
            MH_RemoveHook(g_target);
            g_installed = false;
        }
        for (int wait = 0; wait < 200 && g_inFlight.load(std::memory_order_seq_cst) > 0; ++wait)
            Sleep(1);
        g_original.store(nullptr, std::memory_order_release);
        g_originalRemove.store(nullptr, std::memory_order_release);
        g_originalProj.store(nullptr, std::memory_order_release);
        g_originalDrain.store(nullptr, std::memory_order_release);
    }
}
