#include "frag_observer.h"
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
    namespace
    {
        constexpr std::uintptr_t kPartsBrokeOffRva = 0x6EAE68;
        constexpr std::uint8_t kPartsBrokeOffSignature[] = {
            0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x55};
        using PartsBrokeOffFn = void(__fastcall*)(void* parent, const void* groups, void* pieceInst);

        constexpr std::uint64_t kRing = 256;
        using Slot = replay::ring::Slot<BreakCall>; // event_ring.h
        Slot g_ring[kRing];
        std::atomic<std::uint64_t> g_next{0};
        std::atomic<PartsBrokeOffFn> g_original{nullptr};
        void* g_target = nullptr;
        std::atomic<int> g_inFlight{0};
        LARGE_INTEGER g_frequency{};

        bool GuardedCompare(const void* target, const std::uint8_t* signature, std::size_t size)
        {
            return replay::mem::Matches(target, signature, size); // guarded_memory.h
        }

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
            // +0xA8 bit 0 flag is set (insertion callback 0x6E38AC).
            std::uint64_t user = 0;
            std::uint8_t flags = 0;
            if (parent && GuardedCopy(&user, static_cast<const std::uint8_t*>(parent) + 0xA0, 8) &&
                GuardedCopy(&flags, static_cast<const std::uint8_t*>(parent) + 0xA8, 1) && (flags & 1))
                call.parentEntity = user & ~0xFull;
            call.pieceInst = reinterpret_cast<std::uint64_t>(pieceInst);
            // The piece's entity the same way (0x15796CE reads it
            // right after BreakOffAbove returns).
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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        void* target = base + kPartsBrokeOffRva;
        if (!GuardedCompare(target, kPartsBrokeOffSignature, sizeof(kPartsBrokeOffSignature)))
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
        constexpr std::uintptr_t kDeleteAboveGroupRva = 0x296AD24;
        // 24 bytes: DeleteGroups 0x296AF00 shares the first 16.
        constexpr std::uint8_t kDeleteAboveGroupSignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48,
            0x89, 0x78, 0x18, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
        using DeleteAboveGroupFn = void(__fastcall*)(void* inst, std::uint16_t group);
        // BreakOffAboveGroup(inst, group, ptr, flag) -> the new
        // piece's inst (0: group out of range or already gone, no free cache
        // entry). It checks group < numGroups itself (0x2965F96), not the root.
        constexpr std::uintptr_t kBreakOffAboveGroupRva = 0x2965F2C;
        constexpr std::uint8_t kBreakOffAboveGroupSignature[] = {
            0x40, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8D, 0x6C,
            0x24, 0x20, 0x48, 0x8B, 0x81, 0x38, 0x01, 0x00, 0x00, 0x45, 0x8A, 0xF9};
        using BreakOffAboveGroupFn = void*(__fastcall*)(void* inst, std::uint16_t group, void* data, bool flag);
        int g_breakOffState = -1; // -1 unchecked, 0 off, 1 on
        constexpr std::uintptr_t kFragInstVtableRva = 0x33CBDB8;
        // The offsets read below, as the engine itself uses them.
        struct OffsetSite
        {
            std::uintptr_t rva;
            std::uint8_t bytes[11];
            std::size_t size;
        };
        const OffsetSite kOffsetSites[] = {
            {0x15794F8, {0x48, 0x8B, 0xA9, 0x38, 0x09, 0x00, 0x00}, 7},       // mov rbp,[rcx+0x938]: vehicle fragInst
            {0x6EB0D4, {0x8B, 0x8F, 0xD8, 0x0B, 0x00, 0x00}, 6},             // door count [veh+0xBD8]
            {0x6EB0E8, {0x4C, 0x6B, 0xC0, 0x70, 0x4C, 0x03, 0x87, 0xD0, 0x0B, 0x00, 0x00}, 11}, // doors [veh+0xBD0], 0x70 each
            {0x6EB0F8, {0x66, 0x45, 0x39, 0x60, 0x08}, 5},                   // door child u16 +8
            {0x6EB120, {0x8B, 0x8F, 0xC0, 0x0B, 0x00, 0x00}, 6},             // wheel count [veh+0xBC0]
            {0x6EB135, {0x48, 0x8B, 0x87, 0xB8, 0x0B, 0x00, 0x00}, 7},       // wheels [veh+0xBB8]
            {0x6EB145, {0x66, 0x44, 0x39, 0xA2, 0x30, 0x01, 0x00, 0x00}, 8}, // wheel child u16 +0x130
        };
        int g_vehicleGroupsState = -1; // -1 unchecked, 0 off, 1 on

        struct VehicleFragView
        {
            std::uint64_t inst = 0;
            int result = -1; // -1: go on and call
        };

        // Every read the call depends on, guarded. No objects with a
        // destructor here (__try).
        void GuardedVehicleFragView(std::uint64_t vehicle, std::uintptr_t base, std::uint16_t group, VehicleFragView& view,
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
                if (!inst || *reinterpret_cast<const std::uint64_t*>(inst) != base + kFragInstVtableRva ||
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

        bool GuardedDeleteAboveGroup(std::uintptr_t base, std::uint64_t inst, std::uint16_t group)
        {
            __try
            {
                reinterpret_cast<DeleteAboveGroupFn>(base + kDeleteAboveGroupRva)(reinterpret_cast<void*>(inst), group);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        void* GuardedBreakOffAboveGroup(std::uintptr_t base, std::uint64_t inst, std::uint16_t group, bool& faulted)
        {
            __try
            {
                return reinterpret_cast<BreakOffAboveGroupFn>(base + kBreakOffAboveGroupRva)(
                    reinterpret_cast<void*>(inst), group, nullptr, false);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                faulted = true;
                return nullptr;
            }
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
            const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
            bool ok = GuardedCompare(base + kDeleteAboveGroupRva, kDeleteAboveGroupSignature, sizeof(kDeleteAboveGroupSignature));
            for (const OffsetSite& site : kOffsetSites)
                ok = ok && GuardedCompare(base + site.rva, site.bytes, site.size);
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
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        VehicleFragView view;
        GuardedVehicleFragView(vehicle, base, group, view, true, reserved, reservedCount);
        if (view.result >= 0)
            return static_cast<VehicleGroupResult>(view.result);
        if (!GuardedDeleteAboveGroup(base, view.inst, group))
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
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (g_breakOffState < 0)
            g_breakOffState = GuardedCompare(reinterpret_cast<const void*>(base + kBreakOffAboveGroupRva),
                kBreakOffAboveGroupSignature, sizeof(kBreakOffAboveGroupSignature)) ? 1 : 0;
        if (g_breakOffState != 1)
            return VehicleGroupResult::Unsupported;
        if (!vehicle)
            return VehicleGroupResult::NoInst;
        VehicleFragView view;
        GuardedVehicleFragView(vehicle, base, group, view, false, nullptr, 0);
        if (view.result >= 0)
            return static_cast<VehicleGroupResult>(view.result);
        bool faulted = false;
        void* piece = GuardedBreakOffAboveGroup(base, view.inst, group, faulted);
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
        struct ObjectSite
        {
            std::uintptr_t rva;
            std::uint8_t bytes[12];
            std::size_t size;
        };
        const ObjectSite kObjectSites[] = {
            // mov eax,[rcx+0xB0]; mov sil,r8b; shr eax,6: bIsFrag (0xE30560, the
            // object path of BREAK_OBJECT_FRAGMENT_CHILD)
            {0xE30574, {0x8B, 0x81, 0xB0, 0x00, 0x00, 0x00, 0x41, 0x8A, 0xF0, 0xC1, 0xE8, 0x06}, 12},
            {0xE30587, {0x48, 0x8B, 0x59, 0x38}, 4},                         // mov rbx,[rcx+0x38]: the fragInst
            {0x2974F57, {0x48, 0x8B, 0x83, 0x28, 0x01, 0x00, 0x00}, 7},      // mov rax,[rbx+0x128]: cache entry
            {0x2974F63, {0x44, 0x0F, 0xB7, 0xB8, 0x80, 0x01, 0x00, 0x00}, 8}, // movzx r15d,word [rax+0x180]: root group
            {0x2974F70, {0x0F, 0xB7, 0xBE, 0x40, 0x01, 0x00, 0x00}, 7},      // movzx edi,word [rsi+0x140]: group count
            {0x2974F9B, {0x48, 0x8B, 0x86, 0xC8, 0x00, 0x00, 0x00}, 7},      // mov rax,[rsi+0xC8]: group table
            {0x2974FE6, {0x41, 0x0F, 0xB7, 0x50, 0x06}, 5},                  // movzx edx,word [r8+6]: parent group
            {0x29651B5, {0x49, 0x8B, 0x86, 0x48, 0x01, 0x00, 0x00}, 7},      // mov rax,[r14+0x148]: slots
        };
        int g_objectGroupsState = -1; // -1 unchecked, 0 off, 1 on

        // 0 ok, 1 not a fragment object, 2 fault. No objects with a
        // destructor here (__try).
        int GuardedReadObjectFrag(std::uint64_t object, std::uintptr_t base, ObjectFragState& out, std::uint64_t& instOut)
        {
            __try
            {
                if (*reinterpret_cast<const std::uint8_t*>(object + 0x30) != 5 ||
                    !((*reinterpret_cast<const std::uint32_t*>(object + 0xB0) >> 6) & 1))
                    return 1;
                const std::uint64_t inst = *reinterpret_cast<const std::uint64_t*>(object + 0x38);
                if (!inst || *reinterpret_cast<const std::uint64_t*>(inst) != base + kFragInstVtableRva ||
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
            const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
            bool ok = true;
            for (const ObjectSite& site : kObjectSites)
                ok = ok && GuardedCompare(base + site.rva, site.bytes, site.size);
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
        return GuardedReadObjectFrag(object, reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)), out, inst) == 0;
    }

    VehicleGroupResult BreakOffObjectGroup(std::uint64_t object, std::uint16_t group, std::uint64_t& pieceEntity)
    {
        pieceEntity = 0;
        if (!ObjectGroupsSupported())
            return VehicleGroupResult::Unsupported;
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (g_breakOffState < 0)
            g_breakOffState = GuardedCompare(reinterpret_cast<const void*>(base + kBreakOffAboveGroupRva),
                kBreakOffAboveGroupSignature, sizeof(kBreakOffAboveGroupSignature)) ? 1 : 0;
        if (g_breakOffState != 1)
            return VehicleGroupResult::Unsupported;
        if (!object)
            return VehicleGroupResult::NoInst;
        // 2 KiB of parents: static, script thread only.
        static ObjectFragState state;
        std::memset(state.gone, 0, sizeof(state.gone));
        std::uint64_t inst = 0;
        const int read = GuardedReadObjectFrag(object, base, state, inst);
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
        void* piece = GuardedBreakOffAboveGroup(base, inst, group, faulted);
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
