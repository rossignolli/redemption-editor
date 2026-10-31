#include "entity_pools.h"

#include "engine_calls.h"
#include "game_sig.h"

#include <windows.h>

#include <sstream>

namespace replay::pools
{
    namespace
    {
        // Pool pointer globals (see entity_pools.h): the encoded value, the
        // flag byte before it and the key after it, each found where the
        // engine's own decoders read it.
        constexpr Cipher kPedCipher{2, 3};
        constexpr Cipher kVehicleCipher{1, 4};
        constexpr Cipher kObjectCipher{2, 2};
        constexpr Cipher kGuidCipher{2, 3};
        // Entry sizes: CPed 0x380 (the "Peds" pool creation, the only
        // allocation); CObject 0xCD0 (its creation, allocations 0x7A0..0xCD0);
        // vehicles: the size is the largest vehicle class, read at start
        // (allocations 0x10E0..0x1B80); the guid entry 0x10.
        constexpr std::int32_t kPedEntry = 0x380;
        constexpr std::int32_t kObjectEntry = 0xCD0;
        constexpr std::int32_t kVehicleEntryMin = 0x10E0;
        constexpr std::int32_t kVehicleEntryMax = 0x4000;
        constexpr std::int32_t kGuidEntry = 0x10;
        constexpr std::size_t kGuidTarget = 0x08;

        // The engine's pool switch: reference type 1 vehicle, 2 ped, 3 object,
        // 4 pickup; each case decodes its pool (its flag byte, key and value
        // globals; the rotations are the ciphers) and tail-calls GetAt. Its
        // 321 bytes are the switch and its tail (the vehicle case on).
        const sig::Signature kSwitch{"pools_switch",
            "81 79 08 FF FF FF 7F 4C 8B C9 0F 84 ?? ?? ?? ?? 0F B6 51 10 45 33 C0 85 D2 0F 84 ?? ?? ?? ?? 83 EA 01 0F "
            "84 ?? ?? ?? ?? 83 EA 01 0F 84 ?? ?? ?? ?? 83 EA 01 74 45 83 FA 01 0F 85 ?? ?? ?? ?? 8A 05 ?? ?? ?? ?? 84 "
            "C0 0F 84 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 15 ?? ?? ?? ?? 48 C1 C9 02 48 C1 C1 20 8B C1 83 E0 1F 83 "
            "C0 02 48 33 D1 8A C8 8A 05 ?? ?? ?? ?? E9 ?? ?? ?? ?? 8A 05 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 48 8B 0D "
            "?? ?? ?? ?? 48 8B 15 ?? ?? ?? ?? 48 C1 C9 02 48 C1 C1 20 8B C1 83 E0 1F 83 C0 02 48 33 D1 8A C8 8A 05 ?? "
            "?? ?? ?? EB 6E 8A 05 ?? ?? ?? ?? 84 C0 74 75 48 8B 0D ?? ?? ?? ?? 48 8B 15 ?? ?? ?? ?? 48 C1 C1 20 48 C1 "
            "C9 02 48 33 D1 8B C1 83 E0 1F 83 C0 03 8A C8 8A 05 ?? ?? ?? ?? 48 D3 C2 48 C1 C2 20 EB 39",
            0x1D55D8};
        const sig::Signature kSwitchTail{"pools_switch_tail", kSwitch, sig::Offset{0xF0},
            "8A 05 ?? ?? ?? ?? 84 C0 74 39 48 8B 0D ?? ?? ?? ?? 48 8B 15 ?? ?? ?? ?? 48 D1 C9 48 C1 C1 20 8B C1 83 E0 "
            "1F 83 C0 04 48 33 D1 8A C8 8A 05 ?? ?? ?? ?? 48 C1 C2 20 48 D3 C2 84 C0 74 06 4C 8B C2 49 F7 D0 41 8B 51 "
            "08 49 8B C8 E9 ?? ?? ?? ?? 33 C0",
            0x1D56C8};
        // Each case's globals: the flag byte (mov al), the key (mov rcx) and the
        // value (mov rdx).
        const sig::Signature kObjectPresent{"pools_object_present", kSwitch, sig::Rel32{0x7D, 0x81}, "", 0x4643278};
        const sig::Signature kObjectKey{"pools_object_key", kSwitch, sig::Rel32{0x8C, 0x90}, "", 0x4643288};
        const sig::Signature kObjectPool{"pools_object_pool", kSwitch, sig::Rel32{0x93, 0x97}, "", 0x4643280};
        const sig::Signature kPedPresent{"pools_ped_present", kSwitch, sig::Rel32{0xB6, 0xBA}, "", 0x4A11F68};
        const sig::Signature kPedKey{"pools_ped_key", kSwitch, sig::Rel32{0xC1, 0xC5}, "", 0x4A11F78};
        const sig::Signature kPedPool{"pools_ped_pool", kSwitch, sig::Rel32{0xC8, 0xCC}, "", 0x4A11F70};
        const sig::Signature kVehiclePresent{"pools_vehicle_present", kSwitchTail, sig::Rel32{0x02, 0x06}, "",
            0x500D848};
        const sig::Signature kVehicleKey{"pools_vehicle_key", kSwitchTail, sig::Rel32{0x0D, 0x11}, "", 0x500D858};
        const sig::Signature kVehiclePool{"pools_vehicle_pool", kSwitchTail, sig::Rel32{0x14, 0x18}, "", 0x500D850};
        // fwBasePool::GetAt, the switch's tail call: flags +0x10 compared with
        // the reference's low byte, storage +0x08 + index * entry size +0x1C.
        const sig::Signature kGetAt{"pools_get_at", kSwitchTail, sig::Rel32{0x4B, 0x4F},
            "48 8B 41 10 44 8B CA 41 C1 F9 08 41 38 14 01 75 0C 8B 41 1C 41 0F AF C1 48 03 41 08 C3 33 C0 C3",
            0x2B09388};
        // fwBasePool::Delete: the slot's flag gets 0x80 (free), the used count
        // at +0x28 drops in its low 30 bits.
        const sig::Signature kDelete{"pools_delete",
            "4C 8B D9 48 85 D2 74 6E 4C 63 49 1C 4C 8B 51 08 49 2B D2 48 8B C2 48 99 49 F7 F9 44 0F AF C8 4C 8B C0 49 "
            "63 C9 42 83 0C 11 FF 41 83 7B 20 FF 75 04 45 89 43 20 49 8D 4B 24 83 39 FF 75 05 48 8B C1 EB 0D 41 8B 43 "
            "1C 0F AF 01 48 98 49 03 43 08 44 89 00 49 8B 43 10 44 89 01 49 63 C8 80 0C 08 80 41 8B 43 28 8D 48 FF 33 "
            "C8 81 E1 FF FF FF 3F 33 C8 41 89 4B 28 C3",
            0x2B079E0};
        // IS_ENTITY_A_PED / _A_VEHICLE / _AN_OBJECT cores: the type byte +0x30
        // against 4, 3 and 5, each one by itself.
        const sig::Signature kTypePedCore{"pools_entity_type_ped",
            "40 53 48 83 EC 20 33 DB E8 ?? ?? ?? ?? 48 85 C0 74 07 80 78 30 04 0F 94 C3 8A C3 48 83 C4 20 5B C3",
            0xF9A458};
        const sig::Signature kTypeVehicleCore{"pools_entity_type_vehicle",
            "40 53 48 83 EC 20 33 DB E8 ?? ?? ?? ?? 48 85 C0 74 07 80 78 30 03 0F 94 C3 8A C3 48 83 C4 20 5B C3",
            0xF9A47C};
        const sig::Signature kTypeObjectCore{"pools_entity_type_object",
            "40 53 48 83 EC 20 33 DB E8 ?? ?? ?? ?? 48 85 C0 74 07 80 78 30 05 0F 94 C3 8A C3 48 83 C4 20 5B C3",
            0xF9A4A0};
        // GET_ENTITY_COORDS' reader: movdqu [rax+0x70], the high words widened,
        // times the step constant {32, 32, 0, 0}, added to +0x70.
        const sig::Signature kPositionReader{"pools_position_reader",
            "40 53 48 83 EC 20 48 8B DA 41 8A D0 E8 ?? ?? ?? ?? 0F 57 C0 0F 11 03 48 85 C0 74 24", 0xFA4D10};
        const sig::Signature kPosition{"pools_position", kPositionReader, sig::Offset{0x1C},
            "F3 0F 6F 40 70 66 0F 69 C0 66 0F 72 E0 10 0F 5B C0 66 0F 70 C8 EE 0F 59 0D ?? ?? ?? ?? 0F 58 48 70 0F 11 "
            "0B",
            0xFA4D2C};
        const sig::Signature kStepConstant{"pools_position_step", kPosition, sig::Rel32{0x19, 0x1D}, "", 0x330F370};
        // The natives' entity-to-handle path: a native's call of the jump to
        // the guid maker, which looks the entity's script guid extension up
        // (entity +0x10), allocates one in the guid pool when missing (the
        // guid pool decoder is in its bytes) and returns (guid slot << 8) | its
        // flag byte (kGuidHandle).
        const sig::Signature kHandleNative{"pools_handle_native",
            "48 83 EC 28 48 8B 0D ?? ?? ?? ?? 33 C0 48 85 C9 74 05 E8 ?? ?? ?? ?? 48 83 C4 28 C3", 0xFD423C};
        const sig::Signature kGuidJump{"pools_guid_jump", kHandleNative, sig::Rel32{0x13, 0x17}, "E9 ?? ?? ?? ??",
            0xEE5C3C};
        const sig::Signature kCreateGuid{"pools_create_guid", kGuidJump, sig::Rel32{0x01, 0x05},
            "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 8B 15 ?? ?? ?? ?? 48 8B F1 48 83 C1 10 33 FF "
            "32 DB E8 ?? ?? ?? ?? 48 85 C0 75 65 8A 05 ?? ?? ?? ?? 84 C0 75 05 45 33 C0 EB 37 48 8B 0D ?? ?? ?? ?? 4C "
            "8B 05 ?? ?? ?? ?? 48 C1 C1 20 48 C1 C9 02 4C 33 C1 8B C1 83 E0 1F 83 C0 03 8A C8 8A 05 ?? ?? ?? ?? 49 D3 "
            "C0 49 C1 C0 20 84 C0 74 C7 49 F7 D0",
            0x2B07598};
        const sig::Signature kGuidPresent{"pools_guid_present", kCreateGuid, sig::Rel32{0x31, 0x35}, "", 0x59B3FE0};
        const sig::Signature kGuidKey{"pools_guid_key", kCreateGuid, sig::Rel32{0x41, 0x45}, "", 0x59B3FF0};
        const sig::Signature kGuidPool{"pools_guid_pool", kCreateGuid, sig::Rel32{0x48, 0x4C}, "", 0x59B3FE8};
        const sig::Signature kGuidHandle{"pools_guid_handle", kCreateGuid, sig::Offset{0xF1},
            "49 63 49 1C 49 8B C2 49 2B 41 08 48 99 48 F7 F9 4C 8B C0 48 63 C8 49 8B 41 10 41 C1 E0 08 0F B6 3C 08 41 "
            "03 F8",
            0x2B07689};


        bool g_checked = false;
        bool g_ok = false;
        std::string g_error;
        std::uintptr_t g_base = 0;
        std::uintptr_t g_createGuid = 0;

        struct PoolSite
        {
            const sig::Signature* present; // the flag byte (0: no pool)
            const sig::Signature* value;   // the encoded pointer
            const sig::Signature* key;
            Cipher cipher;
            std::int32_t minEntry;
            std::int32_t maxEntry;
            std::uint8_t type;
            const char* name;
        };
        const PoolSite kSites[kKinds] = {
            {&kPedPresent, &kPedPool, &kPedKey, kPedCipher, kPedEntry, kPedEntry, kTypePed, "peds"},
            {&kVehiclePresent, &kVehiclePool, &kVehicleKey, kVehicleCipher, kVehicleEntryMin, kVehicleEntryMax,
                kTypeVehicle, "vehicles"},
            {&kObjectPresent, &kObjectPool, &kObjectKey, kObjectCipher, kObjectEntry, kObjectEntry, kTypeObject,
                "objects"},
        };
        const PoolSite kGuidSite{&kGuidPresent, &kGuidPool, &kGuidKey, kGuidCipher, kGuidEntry, kGuidEntry, 0, "guids"};

        bool GuardedCopy(void* out, std::uintptr_t from, std::size_t size)
        {
            __try
            {
                std::memcpy(out, reinterpret_cast<const void*>(from), size);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool GuardedImageSize(std::uintptr_t base, std::uintptr_t& size)
        {
            __try
            {
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
                size = nt->OptionalHeader.SizeOfImage;
                return dos->e_magic == IMAGE_DOS_SIGNATURE && nt->Signature == IMAGE_NT_SIGNATURE;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // One entity's type byte and position bytes.
        bool GuardedEntity(std::uint64_t address, std::uint8_t& type, std::uint8_t* position)
        {
            __try
            {
                type = *reinterpret_cast<const std::uint8_t*>(address + kEntityType);
                std::memcpy(position, reinterpret_cast<const void*>(address + kEntityPosition), 16);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool GuardedQword(std::uint64_t address, std::uint64_t& value)
        {
            __try
            {
                value = *reinterpret_cast<const std::uint64_t*>(address);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        std::int32_t GuardedCreateGuid(std::uint64_t entity)
        {
            std::int32_t guid = 0;
            return engine::Call(engine::kCreateGuid, g_createGuid, guid, reinterpret_cast<void*>(entity)) ? guid : 0;
        }

        // The pool's address now (0 when the engine has none or the read
        // failed) and its header.
        bool DecodePool(const PoolSite& site, std::uint64_t& pool, PoolHeader& header)
        {
            pool = 0;
            header = {};
            if (!g_base)
                return false;
            std::uint8_t present = 0;
            std::uint64_t value = 0;
            std::uint64_t key = 0;
            const std::uintptr_t presentAt = sig::Find(*site.present);
            const std::uintptr_t valueAt = sig::Find(*site.value);
            const std::uintptr_t keyAt = sig::Find(*site.key);
            if (!presentAt || !valueAt || !keyAt || !GuardedCopy(&present, presentAt, 1) || !present ||
                !GuardedCopy(&value, valueAt, 8) || !GuardedCopy(&key, keyAt, 8))
                return false;
            pool = DecodePointer(value, key, site.cipher);
            std::uint8_t bytes[kHeaderBytes]{};
            if (!pool || !GuardedCopy(bytes, static_cast<std::uintptr_t>(pool), sizeof(bytes)))
                return false;
            header = ParseHeader(bytes);
            return true;
        }

        PoolState StateOf(const PoolSite& site, PoolHeader* headerOut = nullptr)
        {
            PoolState state;
            std::uint64_t pool = 0;
            PoolHeader header;
            if (!DecodePool(site, pool, header))
                return state;
            state.size = header.size;
            state.entrySize = header.entrySize;
            state.used = header.used;
            state.status = ValidateHeader(header, site.minEntry, site.maxEntry);
            state.ok = state.status == HeaderStatus::Ok;
            if (headerOut)
                *headerOut = header;
            return state;
        }

        // The flag bytes of a pool (a copy: the walk reads them many times).
        bool CopyFlags(const PoolHeader& header, std::vector<std::uint8_t>& flags)
        {
            flags.resize(static_cast<std::size_t>(header.size));
            return GuardedCopy(flags.data(), static_cast<std::uintptr_t>(header.flags), flags.size());
        }

        // Existing handles, sorted by entity (the guid pool walked read-only).
        int SnapshotGuids(std::vector<GuidEntry>& guids, PoolState* stateOut)
        {
            PoolHeader header;
            const PoolState state = StateOf(kGuidSite, &header);
            if (stateOut)
                *stateOut = state;
            static std::vector<std::uint8_t> flags;
            if (!state.ok || !CopyFlags(header, flags))
            {
                guids.clear();
                return 0;
            }
            guids.resize(static_cast<std::size_t>(header.size));
            const int count = WalkGuids(header, flags.data(), static_cast<std::int32_t>(flags.size()),
                [](std::uint64_t entry, std::uint64_t& target) { return GuardedQword(entry + kGuidTarget, target); },
                guids.data(), static_cast<int>(guids.size()));
            guids.resize(static_cast<std::size_t>(count));
            return count;
        }

        // The slot still holds the entity the walk found.
        bool StillLive(Kind kind, const Entry& entry)
        {
            std::uint64_t pool = 0;
            PoolHeader header;
            const PoolSite& site = kSites[static_cast<std::size_t>(kind)];
            if (!DecodePool(site, pool, header) || ValidateHeader(header, site.minEntry, site.maxEntry) != HeaderStatus::Ok)
                return false;
            const std::int32_t index = entry.ref >> 8;
            if (index < 0 || index >= header.size)
                return false;
            if (header.storage + static_cast<std::uint64_t>(index) * static_cast<std::uint64_t>(header.entrySize) != entry.address)
                return false;
            std::uint8_t flag = kFlagFree;
            return GuardedCopy(&flag, static_cast<std::uintptr_t>(header.flags + static_cast<std::uint64_t>(index)), 1) &&
                flag == static_cast<std::uint8_t>(entry.ref & 0xFF) && SlotUsed(flag);
        }

        double NowUs()
        {
            static LARGE_INTEGER frequency{};
            if (!frequency.QuadPart)
                QueryPerformanceFrequency(&frequency);
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            return static_cast<double>(now.QuadPart) * 1000000.0 / static_cast<double>(frequency.QuadPart);
        }
    }

    bool Check(std::string& error)
    {
        if (g_checked)
        {
            error = g_error;
            return g_ok;
        }
        g_checked = true;
        g_base = sig::ImageBase();
        std::uintptr_t imageSize = 0;
        const auto fail = [&](const char* what) {
            g_error = what;
            error = g_error;
            g_ok = false;
            return false;
        };
        if (!g_base || !GuardedImageSize(g_base, imageSize))
            return fail("image");
        struct Required
        {
            const sig::Signature* signature;
            const char* name;
        };
        // The globals the pools are read from come with the code that pins
        // them (the switch's cases, the guid maker).
        const Required required[] = {
            {&kSwitch, "pool_switch_signature_mismatch"},
            {&kSwitchTail, "pool_switch_signature_mismatch"},
            {&kPedPresent, "pool_switch_signature_mismatch"},
            {&kPedPool, "pool_switch_signature_mismatch"},
            {&kPedKey, "pool_switch_signature_mismatch"},
            {&kVehiclePresent, "pool_switch_signature_mismatch"},
            {&kVehiclePool, "pool_switch_signature_mismatch"},
            {&kVehicleKey, "pool_switch_signature_mismatch"},
            {&kObjectPresent, "pool_switch_signature_mismatch"},
            {&kObjectPool, "pool_switch_signature_mismatch"},
            {&kObjectKey, "pool_switch_signature_mismatch"},
            {&kGetAt, "get_at_signature_mismatch"},
            {&kDelete, "delete_signature_mismatch"},
            {&kTypePedCore, "type_signature_mismatch"},
            {&kTypeVehicleCore, "type_signature_mismatch"},
            {&kTypeObjectCore, "type_signature_mismatch"},
            {&kPositionReader, "position_signature_mismatch"},
            {&kPosition, "position_signature_mismatch"},
            {&kStepConstant, "position_signature_mismatch"},
            {&kHandleNative, "guid_jump_signature_mismatch"},
            {&kGuidJump, "guid_jump_signature_mismatch"},
            {&kCreateGuid, "create_guid_signature_mismatch"},
            {&kGuidPresent, "create_guid_signature_mismatch"},
            {&kGuidPool, "create_guid_signature_mismatch"},
            {&kGuidKey, "create_guid_signature_mismatch"},
            {&kGuidHandle, "guid_handle_signature_mismatch"},
        };
        for (const Required& r : required)
        {
            if (!sig::Find(*r.signature))
                return fail(r.name);
        }
        float step[4]{};
        if (!GuardedCopy(step, sig::Find(kStepConstant), sizeof(step)) || step[0] != kPositionStep ||
            step[1] != kPositionStep || step[2] != 0.0f || step[3] != 0.0f)
            return fail("position_step_mismatch");
        // The pools are decoded and validated on every use (a pool that does
        // not decode to a header of its class makes that call return -1),
        // so only the code is checked here.
        g_createGuid = sig::Find(kCreateGuid);
        g_ok = true;
        g_error.clear();
        error.clear();
        return true;
    }

    bool Available()
    {
        std::string error;
        return Check(error);
    }


    PoolState State(Kind kind)
    {
        if (!g_base || kind == Kind::Count)
            return {};
        return StateOf(kSites[static_cast<std::size_t>(kind)]);
    }

    PoolState GuidState()
    {
        if (!g_base)
            return {};
        return StateOf(kGuidSite);
    }

    std::uint32_t GuidRoom()
    {
        const PoolState state = GuidState();
        if (!state.ok || state.size <= 0)
            return 0;
        const std::uint32_t size = static_cast<std::uint32_t>(state.size);
        const std::uint32_t keep = state.used + GuidReserve(state.size);
        return size > keep ? size - keep : 0u;
    }

    int Read(Kind kind, Entry* out, int capacity, WalkStats* stats)
    {
        if (stats)
            *stats = {};
        if (!Available() || kind == Kind::Count)
            return 0;
        const PoolSite& site = kSites[static_cast<std::size_t>(kind)];
        PoolHeader header;
        const PoolState state = StateOf(site, &header);
        static std::vector<std::uint8_t> flags;
        if (!state.ok || !CopyFlags(header, flags))
            return 0;
        return WalkPool(header, flags.data(), static_cast<std::int32_t>(flags.size()), GuardedEntity, site.type, out,
            capacity, stats);
    }

    std::int32_t HandleOf(Kind kind, const Entry& entry, bool create)
    {
        if (!Available() || !entry.address || !StillLive(kind, entry))
            return 0;
        static std::vector<GuidEntry> guids;
        PoolState guidState;
        SnapshotGuids(guids, &guidState);
        if (const std::int32_t existing = FindGuid(guids.data(), static_cast<int>(guids.size()), entry.address))
            return existing;
        if (!create || !guidState.ok ||
            static_cast<std::uint32_t>(guidState.size) <= guidState.used + GuidReserve(guidState.size))
            return 0;
        return GuardedCreateGuid(entry.address);
    }

    int ListHandles(Kind kind, const Position* centers, int centerCount, float radius, float existingRadius,
        int createBudget, int* handles, int capacity, ListStats* stats)
    {
        ListStats local;
        ListStats& s = stats ? *stats : local;
        s = {};
        if (!Available() || kind == Kind::Count)
            return -1;
        const double startedUs = NowUs();
        const PoolSite& site = kSites[static_cast<std::size_t>(kind)];
        PoolHeader header;
        const PoolState state = StateOf(site, &header);
        static std::vector<std::uint8_t> flags;
        static std::vector<Entry> entries;
        if (!state.ok || !CopyFlags(header, flags))
            return -1;
        entries.resize(static_cast<std::size_t>(header.size));
        const int count = WalkPool(header, flags.data(), static_cast<std::int32_t>(flags.size()), GuardedEntity,
            site.type, entries.data(), static_cast<int>(entries.size()));
        s.live = static_cast<std::uint32_t>(count);

        static std::vector<GuidEntry> guids;
        PoolState guidState;
        SnapshotGuids(guids, &guidState);
        const int guidCount = static_cast<int>(guids.size());
        static std::vector<int> order;
        static std::vector<float> distance2;
        PlanHandles(entries.data(), count, centers, centerCount, radius, existingRadius, createBudget,
            [&](const Entry& entry) { return FindGuid(guids.data(), guidCount, entry.address) != 0; }, order, distance2,
            &s.skippedFar, &s.skippedBudget);

        std::uint32_t guidUsed = guidState.used;
        const std::uint32_t guidSize = guidState.ok ? static_cast<std::uint32_t>(guidState.size) : 0u;
        int listed = 0;
        for (const int index : order)
        {
            if (listed >= capacity)
                break;
            const Entry& entry = entries[static_cast<std::size_t>(index)];
            std::int32_t handle = FindGuid(guids.data(), guidCount, entry.address);
            if (handle)
            {
                ++s.existing;
            }
            else if (guidSize > guidUsed + GuidReserve(guidState.size))
            {
                // The slot was read this call and no engine code ran since
                // (this loop only allocates guids), so it still holds it.
                handle = GuardedCreateGuid(entry.address);
                if (handle)
                {
                    ++s.created;
                    ++guidUsed;
                }
            }
            if (!handle)
            {
                ++s.failed;
                continue;
            }
            handles[listed++] = handle;
        }
        s.listed = static_cast<std::uint32_t>(listed);
        s.guidSize = guidSize;
        s.guidFree = guidSize > guidUsed ? guidSize - guidUsed : 0u;
        s.us = NowUs() - startedUs;
        return listed;
    }

    bool GuidHeader(PoolHeader& header)
    {
        header = {};
        if (!Available())
            return false;
        const PoolState state = StateOf(kGuidSite, &header);
        return state.ok;
    }

    bool EntryAt(std::uint64_t address, Kind& kind, Entry& entry)
    {
        entry = {};
        if (!Available() || !address)
            return false;
        std::uint8_t type = 0;
        std::uint8_t position[16]{};
        if (!GuardedEntity(address, type, position))
            return false;
        std::size_t site = kKinds;
        for (std::size_t n = 0; n < kKinds; ++n)
            if (kSites[n].type == type)
                site = n;
        if (site == kKinds)
            return false;
        std::uint64_t pool = 0;
        PoolHeader header;
        if (!DecodePool(kSites[site], pool, header) ||
            ValidateHeader(header, kSites[site].minEntry, kSites[site].maxEntry) != HeaderStatus::Ok ||
            address < header.storage)
            return false;
        const std::uint64_t offset = address - header.storage;
        const std::uint64_t entrySize = static_cast<std::uint64_t>(header.entrySize);
        if (offset % entrySize != 0 || offset / entrySize >= static_cast<std::uint64_t>(header.size))
            return false;
        const std::int32_t index = static_cast<std::int32_t>(offset / entrySize);
        std::uint8_t flag = kFlagFree;
        if (!GuardedCopy(&flag, static_cast<std::uintptr_t>(header.flags + static_cast<std::uint64_t>(index)), 1) ||
            !SlotUsed(flag))
            return false;
        kind = static_cast<Kind>(site);
        entry.address = address;
        entry.ref = PoolRef(index, flag);
        entry.type = type;
        entry.position = DecodePosition(position);
        return true;
    }

    std::string Describe()
    {
        std::ostringstream out;
        std::string error;
        const bool ok = Check(error);
        out << "ENTITY_POOLS,status=" << (ok ? "ok" : "unavailable");
        if (!ok)
            out << ",error=" << error;
        for (std::size_t n = 0; n < kKinds; ++n)
        {
            const PoolState state = g_base ? StateOf(kSites[n]) : PoolState{};
            out << ',' << kSites[n].name << '=' << state.used << '/' << state.size << ",entry_" << kSites[n].name
                << "=0x" << std::hex << state.entrySize << std::dec;
        }
        const PoolState guids = g_base ? StateOf(kGuidSite) : PoolState{};
        out << ",guids=" << guids.used << '/' << guids.size;
        return out.str();
    }
}
