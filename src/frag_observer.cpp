#include "frag_observer.h"
#include "engine_sigs.h"
#include "engine_calls.h"
#include "game_sig.h"
#include "guarded_memory.h"
#include "mod_clock.h"

#include <windows.h>

#include <MinHook.h>
#include "hook_batch.h"
#include "module_lifetime.h"
#include "event_ring.h"

#include <atomic>
#include <cstring>

namespace replay::frag
{
    using enginesig::kFragInstVtable;
    using enginesig::kVehicleBreakOff;

    namespace
    {
        namespace sig = replay::sig;

        // The parts-broke-off callback (parent inst, groups, piece inst).
        const sig::Signature kFragPartsBrokeOff{"frag_parts_broke_off",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC 98 "
            "00 00 00 33 DB",
            0x6EAE68};
        using PartsBrokeOffFn = void(__fastcall*)(void* parent, const void* groups, void* pieceInst);

        constexpr std::uint64_t kRing = 256;
        using Slot = replay::ring::Slot<BreakCall>; // event_ring.h
        Slot g_ring[kRing];
        std::atomic<std::uint64_t> g_next{0};
        std::atomic<PartsBrokeOffFn> g_original{nullptr};
        void* g_target = nullptr;
        std::atomic<int> g_inFlight{0};
        LARGE_INTEGER g_frequency{};

        bool GuardedCopy(void* out, const void* source, std::size_t size)
        {
            return replay::mem::Copy(out, source, size); // guarded_memory.h
        }

        void __fastcall PartsBrokeOffDetour(void* parent, const void* groups, void* pieceInst)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            g_inFlight.fetch_add(1, std::memory_order_seq_cst);
            const PartsBrokeOffFn original = g_original.load(std::memory_order_acquire);
            if (original)
                original(parent, groups, pieceInst);
            replay::ring::Writer<BreakCall> slot(g_ring, kRing, g_next);
            BreakCall& call = slot.value;
            call.timeUs = NowUs();
            call.parent = reinterpret_cast<std::uint64_t>(parent);
            // `parent` is the fragment inst (live: its vtable is not an
            // entity's); its entity is the inst's user data +0xA0 when the
            // +0xA8 bit 0 flag is set (the insertion callback).
            std::uint64_t user = 0;
            std::uint8_t flags = 0;
            if (parent && GuardedCopy(&user, static_cast<const std::uint8_t*>(parent) + 0xA0, 8) &&
                GuardedCopy(&flags, static_cast<const std::uint8_t*>(parent) + 0xA8, 1) && (flags & 1))
                call.parentEntity = user & ~0xFull;
            call.pieceInst = reinterpret_cast<std::uint64_t>(pieceInst);
            // The piece's entity the same way (the vehicle's break-off
            // reads it right after BreakOffAbove returns).
            call.pieceEntity = 0;
            if (pieceInst && GuardedCopy(&user, static_cast<const std::uint8_t*>(pieceInst) + 0xA0, 8) &&
                GuardedCopy(&flags, static_cast<const std::uint8_t*>(pieceInst) + 0xA8, 1) && (flags & 1))
                call.pieceEntity = user & ~0xFull;
            call.copied = groups && GuardedCopy(call.groups, groups, sizeof(call.groups));
            slot.Publish();
            g_inFlight.fetch_sub(1, std::memory_order_seq_cst);
        }
    }

    std::uint64_t NowUs()
    {
        return replay::clock::NowUs(); // mod_clock.h (no 21-day overflow)
    }

    bool Install(std::string& error)
    {
        if (g_target)
            return true;
        void* target = reinterpret_cast<void*>(sig::Find(kFragPartsBrokeOff));
        if (!target)
        {
            error = "frag_parts_broke_off_signature_mismatch";
            return false;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&PartsBrokeOffDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            return false;
        }
        g_original.store(reinterpret_cast<PartsBrokeOffFn>(trampoline), std::memory_order_release);
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

    std::uint64_t Total() { return g_next.load(std::memory_order_acquire); }

    void CollectSince(std::uint64_t& cursor, std::vector<BreakCall>& out)
    {
        out.clear();
        replay::ring::ForEachSince(g_ring, kRing, g_next, cursor, [&](const BreakCall& call) { out.push_back(call); });
    }

    void Shutdown()
    {
        if (g_target)
        {
            MH_DisableHook(g_target);
            MH_RemoveHook(g_target);
            g_target = nullptr;
        }
        for (int wait = 0; wait < 200 && g_inFlight.load(std::memory_order_seq_cst) > 0; ++wait)
            Sleep(1);
        g_original.store(nullptr, std::memory_order_release);
    }
    // Vehicle fragment groups in playback (see the header).
    namespace
    {
        // DeleteAboveGroup(inst, group); DeleteGroups shares its first 16 bytes.
        const sig::Signature kFragDeleteAboveGroup{"frag_delete_above_group",
            "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 48 8D A8 38 FF FF FF 48 81 EC A0 "
            "01 00 00 48 8B 01 44 0F B7 FA",
            0x296AD24};
        // BreakOffAboveGroup(inst, group, ptr, flag) -> the new
        // piece's inst (0: group out of range or already gone, no free cache
        // entry). It checks group < numGroups itself, not the root.
        const sig::Signature kFragBreakOffAboveGroup{"frag_break_off_above_group",
            "40 55 41 56 41 57 48 83 EC 20 48 8D 6C 24 20 48 8B 81 38 01 00 00 45 8A F9 48 89 5D 20 44 0F B7 F2 48 89 "
            "75 28 33 DB 48 89 7D 30 48 8B F1 48 8B B8 90 00 00 00 4C 89 65 38 4D 8B E0",
            0x2965F2C};
        int g_breakOffState = -1; // -1 unchecked, 0 off, 1 on
        std::uintptr_t g_breakOffAboveGroup = 0;
        // The offsets read below, as the engine itself uses them: the vehicle's
        // break-off reads its fragInst, the parts-broke-off callback its doors
        // and wheels.
        const sig::Signature kFragVehicleInst{"frag_vehicle_inst", kVehicleBreakOff, sig::Offset{0x70},
            "48 8B A9 38 09 00 00", 0x15794F8}; // mov rbp,[rcx+0x938]: vehicle fragInst
        const sig::Signature kFragDoorCount{"frag_door_count", kFragPartsBrokeOff, sig::Offset{0x26C},
            "8B 8F D8 0B 00 00", 0x6EB0D4}; // door count [veh+0xBD8]
        const sig::Signature kFragDoors{"frag_doors", kFragPartsBrokeOff, sig::Offset{0x280},
            "4C 6B C0 70 4C 03 87 D0 0B 00 00", 0x6EB0E8}; // doors [veh+0xBD0], 0x70 each
        const sig::Signature kFragDoorChild{"frag_door_child", kFragPartsBrokeOff, sig::Offset{0x290}, "66 45 39 60 08",
            0x6EB0F8}; // door child u16 +8
        const sig::Signature kFragWheelCount{"frag_wheel_count", kFragPartsBrokeOff, sig::Offset{0x2B8},
            "8B 8F C0 0B 00 00", 0x6EB120}; // wheel count [veh+0xBC0]
        const sig::Signature kFragWheels{"frag_wheels", kFragPartsBrokeOff, sig::Offset{0x2CD}, "48 8B 87 B8 0B 00 00",
            0x6EB135}; // wheels [veh+0xBB8]
        const sig::Signature kFragWheelChild{"frag_wheel_child", kFragPartsBrokeOff, sig::Offset{0x2DD},
            "66 44 39 A2 30 01 00 00", 0x6EB145}; // wheel child u16 +0x130
        const sig::Signature* const kVehicleSites[] = {&kFragDeleteAboveGroup, &kFragInstVtable, &kFragVehicleInst,
            &kFragDoorCount, &kFragDoors, &kFragDoorChild, &kFragWheelCount, &kFragWheels, &kFragWheelChild};
        int g_vehicleGroupsState = -1; // -1 unchecked, 0 off, 1 on
        std::uintptr_t g_deleteAboveGroup = 0;
        std::uintptr_t g_fragInstVtable = 0;

        struct VehicleFragView
        {
            std::uint64_t inst = 0;
            int result = -1; // -1: go on and call
        };

        // Every read the call depends on, guarded. No objects with a
        // destructor here (__try).
        void GuardedVehicleFragView(std::uint64_t vehicle, std::uintptr_t vtable, std::uint16_t group, VehicleFragView& view,
            bool keepWheelsAndDoors, const std::uint16_t* reserved, int reservedCount)
        {
            __try
            {
                if (*reinterpret_cast<const std::uint8_t*>(vehicle + 0x30) != 3)
                {
                    view.result = static_cast<int>(VehicleGroupResult::NoInst);
                    return;
                }
                const std::uint64_t inst = *reinterpret_cast<const std::uint64_t*>(vehicle + 0x938);
                if (!inst || *reinterpret_cast<const std::uint64_t*>(inst) != vtable ||
                    !(*reinterpret_cast<const std::uint8_t*>(inst + 0xA8) & 1) ||
                    (*reinterpret_cast<const std::uint64_t*>(inst + 0xA0) & ~0xFull) != vehicle)
                {
                    view.result = static_cast<int>(VehicleGroupResult::NoInst);
                    return;
                }
                const std::uint64_t type = *reinterpret_cast<const std::uint64_t*>(inst + 0x138);
                const std::uint64_t lodGroup = type ? *reinterpret_cast<const std::uint64_t*>(type + 0x90) : 0;
                const std::uint32_t lod = *reinterpret_cast<const std::uint32_t*>(inst + 0x178);
                const std::uint64_t pl = lodGroup && lod <= 2 ? *reinterpret_cast<const std::uint64_t*>(lodGroup + 0x10 + 8ull * lod) : 0;
                if (!pl)
                {
                    view.result = static_cast<int>(VehicleGroupResult::NoInst);
                    return;
                }
                const std::uint16_t groups = *reinterpret_cast<const std::uint16_t*>(pl + 0x140);
                const std::int16_t children = *reinterpret_cast<const std::int16_t*>(pl + 0x146);
                const std::uint64_t groupTable = *reinterpret_cast<const std::uint64_t*>(pl + 0xC8);
                const std::uint64_t childToGroup = *reinterpret_cast<const std::uint64_t*>(pl + 0x110);
                if (group == 0)
                {
                    view.result = static_cast<int>(VehicleGroupResult::Root);
                    return;
                }
                if (group >= groups || !groupTable || !childToGroup || children <= 0 ||
                    !*reinterpret_cast<const std::uint64_t*>(groupTable + 8ull * group))
                {
                    view.result = static_cast<int>(VehicleGroupResult::OutOfRange);
                    return;
                }
                const std::uint64_t cache = *reinterpret_cast<const std::uint64_t*>(inst + 0x128);
                if (cache)
                {
                    const std::int16_t root = *reinterpret_cast<const std::int16_t*>(cache + 0x180);
                    if (static_cast<std::int16_t>(group) <= root)
                    {
                        view.result = static_cast<int>(VehicleGroupResult::Root);
                        return;
                    }
                    const std::uint64_t slots = *reinterpret_cast<const std::uint64_t*>(cache + 0x148);
                    if (slots && *reinterpret_cast<const std::int16_t*>(slots + 2ull * group) == -1)
                    {
                        view.result = static_cast<int>(VehicleGroupResult::AlreadyGone);
                        return;
                    }
                }
                // The wheels' and doors' groups (their child's group): those
                // are the natives' (VPRT), with the vehicle's own state.
                // A recorded piece's root is its own (the piece
                // breaks it off); a piece being made takes wheels and doors.
                std::uint16_t kept[64];
                int keptCount = 0;
                std::uint16_t pieces[64];
                int pieceCount = 0;
                for (int n = 0; reserved && n < reservedCount && pieceCount < 64; ++n)
                    pieces[pieceCount++] = reserved[n];
                if (keepWheelsAndDoors)
                {
                    const std::int32_t wheels = *reinterpret_cast<const std::int32_t*>(vehicle + 0xBC0);
                    const std::uint64_t wheelArray = *reinterpret_cast<const std::uint64_t*>(vehicle + 0xBB8);
                    for (std::int32_t n = 0; wheelArray && n < wheels && n < 32 && keptCount < 64; ++n)
                    {
                        const std::uint64_t wheel = *reinterpret_cast<const std::uint64_t*>(wheelArray + 8ull * n);
                        if (!wheel)
                            continue;
                        const std::uint16_t child = *reinterpret_cast<const std::uint16_t*>(wheel + 0x130);
                        if (child < static_cast<std::uint16_t>(children))
                            kept[keptCount++] = *reinterpret_cast<const std::uint16_t*>(childToGroup + 2ull * child);
                    }
                    const std::int32_t doors = *reinterpret_cast<const std::int32_t*>(vehicle + 0xBD8);
                    const std::uint64_t doorArray = *reinterpret_cast<const std::uint64_t*>(vehicle + 0xBD0);
                    for (std::int32_t n = 0; doorArray && n < doors && n < 32 && keptCount < 64; ++n)
                    {
                        const std::uint16_t child = *reinterpret_cast<const std::uint16_t*>(doorArray + 0x70ull * n + 8);
                        if (child < static_cast<std::uint16_t>(children))
                            kept[keptCount++] = *reinterpret_cast<const std::uint16_t*>(childToGroup + 2ull * child);
                    }
                }
                // The group or one of its parents (a parent's index is
                // always lower).
                std::int32_t walk = group;
                for (int step = 0; step < 64 && walk > 0 && walk < groups; ++step)
                {
                    for (int k = 0; k < pieceCount; ++k)
                        if (pieces[k] == walk)
                        {
                            view.result = static_cast<int>(VehicleGroupResult::PieceGroup);
                            return;
                        }
                    for (int k = 0; k < keptCount; ++k)
                        if (kept[k] == walk)
                        {
                            view.result = static_cast<int>(VehicleGroupResult::WheelOrDoor);
                            return;
                        }
                    const std::uint64_t entry = *reinterpret_cast<const std::uint64_t*>(groupTable + 8ull * walk);
                    const std::int16_t parent = entry ? *reinterpret_cast<const std::int16_t*>(entry + 6) : -1;
                    if (parent < 0 || parent >= walk)
                        break;
                    walk = parent;
                }
                view.inst = inst;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                view.result = static_cast<int>(VehicleGroupResult::Fault);
            }
        }

        bool GuardedDeleteAboveGroup(std::uintptr_t fn, std::uint64_t inst, std::uint16_t group)
        {
            return engine::Call(engine::kFragDeleteAboveGroup, fn, reinterpret_cast<void*>(inst), group);
        }

        void* GuardedBreakOffAboveGroup(std::uintptr_t fn, std::uint64_t inst, std::uint16_t group, bool& faulted)
        {
            void* piece = nullptr;
            if (!engine::Call(engine::kFragBreakOffAboveGroup, fn, piece, reinterpret_cast<void*>(inst), group, nullptr, false))
                faulted = true;
            return piece;
        }

        // BreakOffAboveGroup, checked once for both break-off paths.
        bool BreakOffReady()
        {
            if (g_breakOffState < 0)
            {
                g_breakOffAboveGroup = sig::Find(kFragBreakOffAboveGroup);
                g_breakOffState = g_breakOffAboveGroup ? 1 : 0;
            }
            return g_breakOffState == 1;
        }

        // 1 gone, 0 present, -1 unreadable.
        int GuardedInstGroupGone(std::uint64_t inst, std::uint16_t group)
        {
            __try
            {
                const std::uint64_t cache = *reinterpret_cast<const std::uint64_t*>(inst + 0x128);
                if (!cache)
                    return 0;
                const std::uint64_t slots = *reinterpret_cast<const std::uint64_t*>(cache + 0x148);
                return slots && *reinterpret_cast<const std::int16_t*>(slots + 2ull * group) == -1 ? 1 : 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return -1;
            }
        }
    }

    bool VehicleGroupsSupported()
    {
        if (g_vehicleGroupsState < 0)
        {
            bool ok = true;
            for (const sig::Signature* site : kVehicleSites)
                ok = ok && sig::Find(*site) != 0;
            g_deleteAboveGroup = ok ? sig::Find(kFragDeleteAboveGroup) : 0;
            g_fragInstVtable = ok ? sig::Find(kFragInstVtable) : 0;
            g_vehicleGroupsState = ok ? 1 : 0;
        }
        return g_vehicleGroupsState == 1;
    }

    VehicleGroupResult DeleteVehicleGroup(std::uint64_t vehicle, std::uint16_t group,
        const std::uint16_t* reserved, int reservedCount)
    {
        if (!VehicleGroupsSupported())
            return VehicleGroupResult::Unsupported;
        if (!vehicle)
            return VehicleGroupResult::NoInst;
        VehicleFragView view;
        GuardedVehicleFragView(vehicle, g_fragInstVtable, group, view, true, reserved, reservedCount);
        if (view.result >= 0)
            return static_cast<VehicleGroupResult>(view.result);
        if (!GuardedDeleteAboveGroup(g_deleteAboveGroup, view.inst, group))
            return VehicleGroupResult::Fault;
        return GuardedInstGroupGone(view.inst, group) == 1 ? VehicleGroupResult::Deleted : VehicleGroupResult::NotRemoved;
    }

    std::uint64_t EntityOfInst(std::uint64_t inst)
    {
        std::uint64_t user = 0;
        std::uint8_t flags = 0;
        if (inst && GuardedCopy(&user, reinterpret_cast<const std::uint8_t*>(inst) + 0xA0, 8) &&
            GuardedCopy(&flags, reinterpret_cast<const std::uint8_t*>(inst) + 0xA8, 1) && (flags & 1))
            return user & ~0xFull;
        return 0;
    }

    int LowestGroup(const std::uint8_t (&groups)[0x80])
    {
        for (int bit = 0; bit < 0x80 * 8; ++bit)
            if ((groups[bit / 8] >> (bit % 8)) & 1)
                return bit;
        return -1;
    }

    VehicleGroupResult BreakOffVehicleGroup(std::uint64_t vehicle, std::uint16_t group, std::uint64_t& pieceEntity)
    {
        pieceEntity = 0;
        if (!VehicleGroupsSupported())
            return VehicleGroupResult::Unsupported;
        if (!BreakOffReady())
            return VehicleGroupResult::Unsupported;
        if (!vehicle)
            return VehicleGroupResult::NoInst;
        VehicleFragView view;
        GuardedVehicleFragView(vehicle, g_fragInstVtable, group, view, false, nullptr, 0);
        if (view.result >= 0)
            return static_cast<VehicleGroupResult>(view.result);
        bool faulted = false;
        void* piece = GuardedBreakOffAboveGroup(g_breakOffAboveGroup, view.inst, group, faulted);
        if (faulted)
            return VehicleGroupResult::Fault;
        if (!piece)
            return VehicleGroupResult::NotRemoved;
        pieceEntity = EntityOfInst(reinterpret_cast<std::uint64_t>(piece));
        return pieceEntity ? VehicleGroupResult::Deleted : VehicleGroupResult::NotRemoved;
    }

    // Object fragments (see the header).
    namespace
    {
        // The object path of BREAK_OBJECT_FRAGMENT_CHILD.
        const sig::Signature kFragObjectBreakChild{"frag_object_break_child",
            "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 8B 81 B0 00 00 00", 0xE30560};
        // mov eax,[rcx+0xB0]; mov sil,r8b; shr eax,6: bIsFrag
        const sig::Signature kFragObjectIsFrag{"frag_object_is_frag", kFragObjectBreakChild, sig::Offset{0x14},
            "8B 81 B0 00 00 00 41 8A F0 C1 E8 06", 0xE30574};
        const sig::Signature kFragObjectInst{"frag_object_inst", kFragObjectBreakChild, sig::Offset{0x27}, "48 8B 59 38",
            0xE30587}; // mov rbx,[rcx+0x38]: the fragInst
        // The fragInst's break-off of a group's children (it calls
        // BreakOffAboveGroup).
        const sig::Signature kFragInstBreakGroup{"frag_inst_break_group",
            "40 55 57 41 54 41 56 41 57 48 83 EC 60 48 8D 6C 24 30 48 8B 81 38 01 00 00 4C 8B E2 48 89 5D 68", 0x2974EFC};
        const sig::Signature kFragCacheEntry{"frag_cache_entry", kFragInstBreakGroup, sig::Offset{0x5B},
            "48 8B 83 28 01 00 00", 0x2974F57}; // mov rax,[rbx+0x128]: cache entry
        const sig::Signature kFragRootGroup{"frag_root_group", kFragInstBreakGroup, sig::Offset{0x67},
            "44 0F B7 B8 80 01 00 00", 0x2974F63}; // movzx r15d,word [rax+0x180]: root group
        const sig::Signature kFragGroupCount{"frag_group_count", kFragInstBreakGroup, sig::Offset{0x74},
            "0F B7 BE 40 01 00 00", 0x2974F70}; // movzx edi,word [rsi+0x140]: group count
        const sig::Signature kFragGroupTable{"frag_group_table", kFragInstBreakGroup, sig::Offset{0x9F},
            "48 8B 86 C8 00 00 00", 0x2974F9B}; // mov rax,[rsi+0xC8]: group table
        const sig::Signature kFragParentGroup{"frag_parent_group", kFragInstBreakGroup, sig::Offset{0xEA},
            "41 0F B7 50 06", 0x2974FE6}; // movzx edx,word [r8+6]: parent group
        // A fragInst's walk of its groups.
        const sig::Signature kFragInstGroupState{"frag_inst_group_state",
            "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 30 48 8B 81 38 01 00 00", 0x2965078};
        const sig::Signature kFragGroupSlots{"frag_group_slots", kFragInstGroupState, sig::Offset{0x13D},
            "49 8B 86 48 01 00 00", 0x29651B5}; // mov rax,[r14+0x148]: slots
        const sig::Signature* const kObjectSites[] = {&kFragObjectIsFrag, &kFragObjectInst, &kFragCacheEntry,
            &kFragRootGroup, &kFragGroupCount, &kFragGroupTable, &kFragParentGroup, &kFragGroupSlots, &kFragInstVtable};
        int g_objectGroupsState = -1; // -1 unchecked, 0 off, 1 on
        std::uintptr_t g_objectFragVtable = 0;

        // 0 ok, 1 not a fragment object, 2 fault. No objects with a
        // destructor here (__try).
        int GuardedReadObjectFrag(std::uint64_t object, std::uintptr_t vtable, ObjectFragState& out, std::uint64_t& instOut)
        {
            __try
            {
                if (*reinterpret_cast<const std::uint8_t*>(object + 0x30) != 5 ||
                    !((*reinterpret_cast<const std::uint32_t*>(object + 0xB0) >> 6) & 1))
                    return 1;
                const std::uint64_t inst = *reinterpret_cast<const std::uint64_t*>(object + 0x38);
                if (!inst || *reinterpret_cast<const std::uint64_t*>(inst) != vtable ||
                    !(*reinterpret_cast<const std::uint8_t*>(inst + 0xA8) & 1) ||
                    (*reinterpret_cast<const std::uint64_t*>(inst + 0xA0) & ~0xFull) != object)
                    return 1;
                const std::uint64_t type = *reinterpret_cast<const std::uint64_t*>(inst + 0x138);
                const std::uint64_t lodGroup = type ? *reinterpret_cast<const std::uint64_t*>(type + 0x90) : 0;
                const std::uint32_t lod = *reinterpret_cast<const std::uint32_t*>(inst + 0x178);
                const std::uint64_t pl = lodGroup && lod <= 2 ? *reinterpret_cast<const std::uint64_t*>(lodGroup + 0x10 + 8ull * lod) : 0;
                if (!pl)
                    return 1;
                const std::uint16_t groups = *reinterpret_cast<const std::uint16_t*>(pl + 0x140);
                const std::uint64_t groupTable = *reinterpret_cast<const std::uint64_t*>(pl + 0xC8);
                if (!groups || groups > kMaxObjectGroups || !groupTable)
                    return 1;
                out.groups = groups;
                for (int g = 0; g < groups; ++g)
                {
                    const std::uint64_t entry = *reinterpret_cast<const std::uint64_t*>(groupTable + 8ull * g);
                    out.parents[g] = entry ? *reinterpret_cast<const std::int16_t*>(entry + 6) : -1;
                }
                const std::uint64_t cache = *reinterpret_cast<const std::uint64_t*>(inst + 0x128);
                out.cached = cache != 0;
                out.root = 0;
                if (cache)
                {
                    out.root = *reinterpret_cast<const std::uint16_t*>(cache + 0x180);
                    const std::uint64_t slots = *reinterpret_cast<const std::uint64_t*>(cache + 0x148);
                    for (int g = 0; slots && g < groups; ++g)
                        if (*reinterpret_cast<const std::int16_t*>(slots + 2ull * g) == -1)
                            out.gone[g / 8] |= static_cast<std::uint8_t>(1u << (g % 8));
                }
                instOut = inst;
                return 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 2;
            }
        }
    }

    bool ObjectGroupsSupported()
    {
        if (g_objectGroupsState < 0)
        {
            bool ok = true;
            for (const sig::Signature* site : kObjectSites)
                ok = ok && sig::Find(*site) != 0;
            g_objectFragVtable = ok ? sig::Find(kFragInstVtable) : 0;
            g_objectGroupsState = ok ? 1 : 0;
        }
        return g_objectGroupsState == 1;
    }

    bool ReadObjectFrag(std::uint64_t object, ObjectFragState& out)
    {
        out.groups = 0;
        out.cached = false;
        out.root = 0;
        std::memset(out.gone, 0, sizeof(out.gone));
        if (!object || !ObjectGroupsSupported())
            return false;
        std::uint64_t inst = 0;
        return GuardedReadObjectFrag(object, g_objectFragVtable, out, inst) == 0;
    }

    VehicleGroupResult BreakOffObjectGroup(std::uint64_t object, std::uint16_t group, std::uint64_t& pieceEntity)
    {
        pieceEntity = 0;
        if (!ObjectGroupsSupported())
            return VehicleGroupResult::Unsupported;
        if (!BreakOffReady())
            return VehicleGroupResult::Unsupported;
        if (!object)
            return VehicleGroupResult::NoInst;
        // 2 KiB of parents: static, script thread only.
        static ObjectFragState state;
        std::memset(state.gone, 0, sizeof(state.gone));
        std::uint64_t inst = 0;
        const int read = GuardedReadObjectFrag(object, g_objectFragVtable, state, inst);
        if (read == 2)
            return VehicleGroupResult::Fault;
        if (read != 0)
            return VehicleGroupResult::NoInst;
        if (group == 0 || (state.cached && group <= state.root))
            return VehicleGroupResult::Root;
        if (group >= state.groups)
            return VehicleGroupResult::OutOfRange;
        if ((state.gone[group / 8] >> (group % 8)) & 1)
            return VehicleGroupResult::AlreadyGone;
        bool faulted = false;
        void* piece = GuardedBreakOffAboveGroup(g_breakOffAboveGroup, inst, group, faulted);
        if (faulted)
            return VehicleGroupResult::Fault;
        if (!piece)
            return VehicleGroupResult::NotRemoved;
        pieceEntity = EntityOfInst(reinterpret_cast<std::uint64_t>(piece));
        return pieceEntity ? VehicleGroupResult::Deleted : VehicleGroupResult::NotRemoved;
    }

    const char* VehicleGroupResultText(VehicleGroupResult result)
    {
        switch (result)
        {
        case VehicleGroupResult::Deleted: return "deleted";
        case VehicleGroupResult::AlreadyGone: return "already_gone";
        case VehicleGroupResult::WheelOrDoor: return "wheel_or_door";
        case VehicleGroupResult::Root: return "root";
        case VehicleGroupResult::OutOfRange: return "out_of_range";
        case VehicleGroupResult::NoInst: return "no_inst";
        case VehicleGroupResult::Unsupported: return "unsupported";
        case VehicleGroupResult::Fault: return "fault";
        case VehicleGroupResult::NotRemoved: return "not_removed";
        case VehicleGroupResult::PieceGroup: return "piece_group";
        }
        return "?";
    }
}
