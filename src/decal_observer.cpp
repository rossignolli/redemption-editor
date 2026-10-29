#include "decal_observer.h"
#include "decal_producer_gate.h"
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
        constexpr std::uintptr_t kCoreRva = 0x2C2EF08;
        constexpr std::uint8_t kCoreSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
            0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20};
        constexpr std::uintptr_t kWrapperRva = 0x2C2F054;
        constexpr std::uint8_t kWrapperSignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48,
            0x89, 0x78, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC};
        constexpr std::uintptr_t kManagerRva = 0x5B1F940; // pointer to the rage decal manager
        constexpr std::uintptr_t kRemoveRva = 0x2C49FCC; // (manager, id)
        constexpr std::uint8_t kRemoveSignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48,
            0x89, 0x78, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC};
        using RemoveFn = void(__fastcall*)(void*, int);
        constexpr std::uintptr_t kProjAddRva = 0x140CF14;
        constexpr std::uint8_t kProjAddSignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x50, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54,
            0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0x18, 0xFD, 0xFF, 0xFF, 0x48, 0x81, 0xEC};
        // 32 bytes; the first 24 appear 29 times in .text, the 32 once.
        constexpr std::uintptr_t kProjManagerRva = 0x4F15040; // the manager object itself
        using ProjAddFn = void*(__fastcall*)(void*, const std::uint32_t*, void*, int, const float*, const float*, const float*,
            float, std::uint16_t, std::uint8_t, int, int, void*);
        constexpr std::uintptr_t kAliveRva = 0x2C3DD04; // (manager, id) -> bool
        constexpr std::uint8_t kAliveSignature[] = {
            0x85, 0xD2, 0x74, 0x1A, 0x8D, 0x82, 0x00, 0x00, 0xFF, 0xFF, 0x44, 0x8B, 0xC0, 0x49, 0xC1, 0xE8,
            0x10, 0x66, 0x42, 0x39, 0x94, 0x41, 0x48, 0x09};
        using AliveFn = bool(__fastcall*)(void*, int);

        using CoreFn = int(__fastcall*)(void*, void*, void*, int, std::uint8_t);
        using WrapperFn = int(__fastcall*)(void*, void*, void*, std::uint8_t);

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
        WrapperFn g_wrapper = nullptr;
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
        // The deferred decal job's drain, 0x1431DD4 (queue) -> count drained. It
        // walks the 0x80-byte requests of the queue at 0x4F50980 and adds each
        // through 0x14357D0 -> 0x125B97C -> 0x125F9EC -> wrapper 0x2C2F054 -> core
        // 0x2C2EF08, on a worker of the task scheduler (job 0xC1F450). It is the
        // engine's producer into the request ring the replay also produces into;
        // the gate below keeps the two apart.
        constexpr std::uintptr_t kDrainRva = 0x1431DD4;
        constexpr std::uint8_t kDrainSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
            0x48, 0x83, 0xEC, 0x20, 0x33, 0xF6, 0x48, 0x8D, 0xA9, 0x30, 0x80, 0x01, 0x00, 0x40, 0x88, 0xB1};
        // 32 bytes, once in .text.
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

        bool GuardedCompare(const void* target, const std::uint8_t* signature, std::size_t size)
        {
            return replay::mem::Matches(target, signature, size); // guarded_memory.h
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
        void* target = base + kCoreRva;
        if (!GuardedCompare(target, kCoreSignature, sizeof(kCoreSignature)))
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
        void* removeTarget = base + kRemoveRva;
        if (GuardedCompare(removeTarget, kRemoveSignature, sizeof(kRemoveSignature)))
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
        void* projTarget = base + kProjAddRva;
        if (GuardedCompare(projTarget, kProjAddSignature, sizeof(kProjAddSignature)))
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
        void* drainTarget = base + kDrainRva;
        if (GuardedCompare(drainTarget, kDrainSignature, sizeof(kDrainSignature)))
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
                fn(manager, &type, entity, call.arg4, position, direction, side, call.size, call.arg9, call.arg10, call.arg11,
                    call.arg12, reinterpret_cast<void*>(call.owner));
                t_replayProj = false;
                return true;
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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        ProjAddFn fn = g_originalProj.load(std::memory_order_acquire);
        if (!fn)
        {
            error = "proj_not_hooked";
            return false;
        }
        std::uint32_t code = 0;
        if (!GuardedProjAdd(fn, base + kProjManagerRva, call, entity, code))
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
        bool GuardedRemove(void* manager, int id, std::uint32_t& code)
        {
            __try
            {
                const RemoveFn fn = g_originalRemove.load(std::memory_order_acquire);
                if (fn)
                    fn(manager, id);
                else
                    reinterpret_cast<RemoveFn>(reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr)) + kRemoveRva)(manager, id);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    namespace
    {
        bool ManagerPointer(std::uint64_t& manager, std::string& error)
        {
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            std::uint8_t pointer[8] = {};
            if (!GuardedRead(pointer, base + kManagerRva, 8))
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
        // first (0x15EDE0 -> 0x15E544 writes [previous+8]); a value that is
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
            __try
            {
                result = fn(manager, entity, settings, id, flag);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool GuardedAlive(AliveFn fn, void* manager, int id, bool& alive, std::uint32_t& code)
        {
            __try
            {
                alive = fn(manager, id);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!GuardedCompare(base + kAliveRva, kAliveSignature, sizeof(kAliveSignature)))
        {
            error = "decal_alive_signature_mismatch";
            return false;
        }
        std::uint64_t manager = 0;
        if (!ManagerPointer(manager, error))
            return false;
        bool alive = false;
        std::uint32_t code = 0;
        if (!GuardedAlive(reinterpret_cast<AliveFn>(base + kAliveRva), reinterpret_cast<void*>(manager), static_cast<int>(id), alive, code))
        {
            error = "exception";
            return false;
        }
        return alive;
    }

    bool RemoveDecal(std::uint32_t id, std::string& error)
    {
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!g_removeTarget && !GuardedCompare(base + kRemoveRva, kRemoveSignature, sizeof(kRemoveSignature)))
        {
            error = "decal_remove_signature_mismatch";
            return false;
        }
        std::uint8_t pointer[8] = {};
        if (!GuardedRead(pointer, base + kManagerRva, 8))
        {
            error = "decal_manager_unreadable";
            return false;
        }
        std::uint64_t manager = 0;
        std::memcpy(&manager, pointer, 8);
        std::uint32_t code = 0;
        if (!manager || !GuardedRemove(reinterpret_cast<void*>(manager), static_cast<int>(id), code))
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
        constexpr std::uintptr_t kInfoTableRva = 0x3CE4F00;
        constexpr std::uintptr_t kInfoLookupRva = 0x2C3A3D8;
        constexpr std::uint8_t kInfoLookupSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20,
            0x48, 0x8B, 0xD9, 0x41, 0x8B, 0xF8, 0x48, 0x83, 0xC1, 0x18,
        };
        constexpr std::size_t kInfoRecordBytes = 0x160;
        using InfoLookupFn = void*(__fastcall*)(void* table, const std::uint32_t* hash, int index);
        InfoLookupFn g_infoLookup = nullptr;
        bool g_infoLookupChecked = false;
        std::unordered_map<std::uint64_t, InfoKey> g_infoKeys;

        template <typename T>
        bool ReadValue(std::uintptr_t address, T& value)
        {
            return GuardedRead(reinterpret_cast<std::uint8_t*>(&value), reinterpret_cast<const void*>(address), sizeof(T));
        }

        bool GuardedLookup(InfoLookupFn fn, void* table, const std::uint32_t* hash, int index, void*& out, std::uint32_t& code)
        {
            __try
            {
                out = fn(table, hash, index);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
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
        const std::uintptr_t table = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) + kInfoTableRva;
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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!g_infoLookupChecked)
        {
            g_infoLookupChecked = true;
            if (GuardedCompare(base + kInfoLookupRva, kInfoLookupSignature, sizeof(kInfoLookupSignature)))
                g_infoLookup = reinterpret_cast<InfoLookupFn>(base + kInfoLookupRva);
        }
        if (!g_infoLookup)
        {
            error = "info_lookup_signature_mismatch";
            return 0;
        }
        void* out = nullptr;
        std::uint32_t code = 0;
        const std::uint32_t hash = key.hash;
        if (!GuardedLookup(g_infoLookup, base + kInfoTableRva, &hash, key.index, out, code))
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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!GuardedCompare(base + kWrapperRva, kWrapperSignature, sizeof(kWrapperSignature)))
        {
            error = "decal_wrapper_signature_mismatch";
            return false;
        }
        g_wrapper = reinterpret_cast<WrapperFn>(base + kWrapperRva);
        return true;
    }

    namespace
    {
        bool GuardedAdd(void* manager, void* entity, std::uint8_t* settings, std::uint8_t flag, int& id, std::uint32_t& code)
        {
            __try
            {
                t_replayAdd = true;
                id = g_wrapper(manager, entity, settings, flag);
                t_replayAdd = false;
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                t_replayAdd = false;
                return false;
            }
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
        if (!CheckInfo(settings, error))
            return 0;
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        std::uint8_t pointer[8] = {};
        if (!GuardedRead(pointer, base + kManagerRva, 8))
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
