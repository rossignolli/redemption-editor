#include "entity_pools.h"

#include <windows.h>

#include <sstream>

namespace replay::pools
{
    namespace
    {
        // Build 1491.50 (see entity_pools.h). Pool pointer globals: the
        // value; the flag byte is at -8 and the key at +8.
        constexpr std::uintptr_t kPedPoolRva = 0x4A11F70;
        constexpr std::uintptr_t kVehiclePoolRva = 0x500D850;
        constexpr std::uintptr_t kObjectPoolRva = 0x4643280;
        constexpr std::uintptr_t kGuidPoolRva = 0x59B3FE8;
        constexpr Cipher kPedCipher{2, 3};
        constexpr Cipher kVehicleCipher{1, 4};
        constexpr Cipher kObjectCipher{2, 2};
        constexpr Cipher kGuidCipher{2, 3};
        // Entry sizes: CPed 0x380 (the "Peds" pool creation 0xC098E5, the only
        // allocation 0xBFE2FA); CObject 0xCD0 (creation 0x81BC09, allocations
        // 0x7A0..0xCD0); vehicles: the size is the largest vehicle class,
        // read at start (0x3B01B60; allocations 0x10E0..0x1B80); the guid
        // entry 0x10.
        constexpr std::int32_t kPedEntry = 0x380;
        constexpr std::int32_t kObjectEntry = 0xCD0;
        constexpr std::int32_t kVehicleEntryMin = 0x10E0;
        constexpr std::int32_t kVehicleEntryMax = 0x4000;
        constexpr std::int32_t kGuidEntry = 0x10;
        constexpr std::size_t kGuidTarget = 0x08;

        // The engine's pool switch 0x1D55D8..0x1D5719: reference type 1
        // vehicle, 2 ped, 3 object, 4 pickup; each case decodes its pool
        // (the rip displacements pin the globals above, the rotations the
        // ciphers) and tail-calls GetAt 0x2B09388.
        constexpr std::uintptr_t kSwitchRva = 0x1D55D8;
        constexpr std::uint8_t kSwitchSignature[] = {
            0x81, 0x79, 0x08, 0xFF, 0xFF, 0xFF, 0x7F, 0x4C, 0x8B, 0xC9, 0x0F, 0x84, 0x2F, 0x01, 0x00, 0x00, 0x0F, 0xB6,
            0x51, 0x10, 0x45, 0x33, 0xC0, 0x85, 0xD2, 0x0F, 0x84, 0x20, 0x01, 0x00, 0x00, 0x83, 0xEA, 0x01, 0x0F, 0x84,
            0xC8, 0x00, 0x00, 0x00, 0x83, 0xEA, 0x01, 0x0F, 0x84, 0x83, 0x00, 0x00, 0x00, 0x83, 0xEA, 0x01, 0x74, 0x45,
            0x83, 0xFA, 0x01, 0x0F, 0x85, 0x00, 0x01, 0x00, 0x00, 0x8A, 0x05, 0xDB, 0xD8, 0x36, 0x04, 0x84, 0xC0, 0x0F,
            0x84, 0xE6, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x0D, 0xDC, 0xD8, 0x36, 0x04, 0x48, 0x8B, 0x15, 0xCD, 0xD8, 0x36,
            0x04, 0x48, 0xC1, 0xC9, 0x02, 0x48, 0xC1, 0xC1, 0x20, 0x8B, 0xC1, 0x83, 0xE0, 0x1F, 0x83, 0xC0, 0x02, 0x48,
            0x33, 0xD1, 0x8A, 0xC8, 0x8A, 0x05, 0xAA, 0xD8, 0x36, 0x04, 0xE9, 0xA7, 0x00, 0x00, 0x00, 0x8A, 0x05, 0x1F,
            0xDC, 0x46, 0x04, 0x84, 0xC0, 0x0F, 0x84, 0xAA, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x0D, 0x20, 0xDC, 0x46, 0x04,
            0x48, 0x8B, 0x15, 0x11, 0xDC, 0x46, 0x04, 0x48, 0xC1, 0xC9, 0x02, 0x48, 0xC1, 0xC1, 0x20, 0x8B, 0xC1, 0x83,
            0xE0, 0x1F, 0x83, 0xC0, 0x02, 0x48, 0x33, 0xD1, 0x8A, 0xC8, 0x8A, 0x05, 0xEE, 0xDB, 0x46, 0x04, 0xEB, 0x6E,
            0x8A, 0x05, 0xD6, 0xC8, 0x83, 0x04, 0x84, 0xC0, 0x74, 0x75, 0x48, 0x8B, 0x0D, 0xDB, 0xC8, 0x83, 0x04, 0x48,
            0x8B, 0x15, 0xCC, 0xC8, 0x83, 0x04, 0x48, 0xC1, 0xC1, 0x20, 0x48, 0xC1, 0xC9, 0x02, 0x48, 0x33, 0xD1, 0x8B,
            0xC1, 0x83, 0xE0, 0x1F, 0x83, 0xC0, 0x03, 0x8A, 0xC8, 0x8A, 0x05, 0xA9, 0xC8, 0x83, 0x04, 0x48, 0xD3, 0xC2,
            0x48, 0xC1, 0xC2, 0x20, 0xEB, 0x39, 0x8A, 0x05, 0x7A, 0x81, 0xE3, 0x04, 0x84, 0xC0, 0x74, 0x39, 0x48, 0x8B,
            0x0D, 0x7F, 0x81, 0xE3, 0x04, 0x48, 0x8B, 0x15, 0x70, 0x81, 0xE3, 0x04, 0x48, 0xD1, 0xC9, 0x48, 0xC1, 0xC1,
            0x20, 0x8B, 0xC1, 0x83, 0xE0, 0x1F, 0x83, 0xC0, 0x04, 0x48, 0x33, 0xD1, 0x8A, 0xC8, 0x8A, 0x05, 0x4E, 0x81,
            0xE3, 0x04, 0x48, 0xC1, 0xC2, 0x20, 0x48, 0xD3, 0xC2, 0x84, 0xC0, 0x74, 0x06, 0x4C, 0x8B, 0xC2, 0x49, 0xF7,
            0xD0, 0x41, 0x8B, 0x51, 0x08, 0x49, 0x8B, 0xC8, 0xE9, 0x71, 0x3C, 0x93, 0x02, 0x33, 0xC0};
        // fwBasePool::GetAt 0x2B09388: flags +0x10 compared with the
        // reference's low byte, storage +0x08 + index * entry size +0x1C.
        constexpr std::uintptr_t kGetAtRva = 0x2B09388;
        constexpr std::uint8_t kGetAtSignature[] = {
            0x48, 0x8B, 0x41, 0x10, 0x44, 0x8B, 0xCA, 0x41, 0xC1, 0xF9, 0x08, 0x41, 0x38, 0x14, 0x01, 0x75, 0x0C, 0x8B,
            0x41, 0x1C, 0x41, 0x0F, 0xAF, 0xC1, 0x48, 0x03, 0x41, 0x08, 0xC3, 0x33, 0xC0, 0xC3};
        // fwBasePool::Delete 0x2B079E0: the slot's flag gets 0x80 (free),
        // the used count at +0x28 drops in its low 30 bits.
        constexpr std::uintptr_t kDeleteRva = 0x2B079E0;
        constexpr std::uint8_t kDeleteSignature[] = {
            0x4C, 0x8B, 0xD9, 0x48, 0x85, 0xD2, 0x74, 0x6E, 0x4C, 0x63, 0x49, 0x1C, 0x4C, 0x8B, 0x51, 0x08, 0x49, 0x2B,
            0xD2, 0x48, 0x8B, 0xC2, 0x48, 0x99, 0x49, 0xF7, 0xF9, 0x44, 0x0F, 0xAF, 0xC8, 0x4C, 0x8B, 0xC0, 0x49, 0x63,
            0xC9, 0x42, 0x83, 0x0C, 0x11, 0xFF, 0x41, 0x83, 0x7B, 0x20, 0xFF, 0x75, 0x04, 0x45, 0x89, 0x43, 0x20, 0x49,
            0x8D, 0x4B, 0x24, 0x83, 0x39, 0xFF, 0x75, 0x05, 0x48, 0x8B, 0xC1, 0xEB, 0x0D, 0x41, 0x8B, 0x43, 0x1C, 0x0F,
            0xAF, 0x01, 0x48, 0x98, 0x49, 0x03, 0x43, 0x08, 0x44, 0x89, 0x00, 0x49, 0x8B, 0x43, 0x10, 0x44, 0x89, 0x01,
            0x49, 0x63, 0xC8, 0x80, 0x0C, 0x08, 0x80, 0x41, 0x8B, 0x43, 0x28, 0x8D, 0x48, 0xFF, 0x33, 0xC8, 0x81, 0xE1,
            0xFF, 0xFF, 0xFF, 0x3F, 0x33, 0xC8, 0x41, 0x89, 0x4B, 0x28, 0xC3};
        // IS_ENTITY_A_PED / _A_VEHICLE / _AN_OBJECT cores 0xF9A458..: the
        // type byte +0x30 against 4, 3 and 5.
        constexpr std::uintptr_t kTypesRva = 0xF9A458;
        constexpr std::uint8_t kTypesSignature[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xDB, 0xE8, 0xBF, 0x76, 0x9D, 0xFF, 0x48, 0x85, 0xC0, 0x74, 0x07,
            0x80, 0x78, 0x30, 0x04, 0x0F, 0x94, 0xC3, 0x8A, 0xC3, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3, 0xCC, 0x72, 0x48,
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xDB, 0xE8, 0x9B, 0x76, 0x9D, 0xFF, 0x48, 0x85, 0xC0, 0x74, 0x07,
            0x80, 0x78, 0x30, 0x03, 0x0F, 0x94, 0xC3, 0x8A, 0xC3, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3, 0xCC, 0xEA, 0x41,
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xDB, 0xE8, 0x77, 0x76, 0x9D, 0xFF, 0x48, 0x85, 0xC0, 0x74, 0x07,
            0x80, 0x78, 0x30, 0x05, 0x0F, 0x94, 0xC3, 0x8A, 0xC3, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3};
        // GET_ENTITY_COORDS' reader 0xFA4D2C: movdqu [rax+0x70], the high
        // words widened, times the constant 0x330F370, added to +0x70.
        constexpr std::uintptr_t kPositionRva = 0xFA4D2C;
        constexpr std::uint8_t kPositionSignature[] = {
            0xF3, 0x0F, 0x6F, 0x40, 0x70, 0x66, 0x0F, 0x69, 0xC0, 0x66, 0x0F, 0x72, 0xE0, 0x10, 0x0F, 0x5B, 0xC0, 0x66,
            0x0F, 0x70, 0xC8, 0xEE, 0x0F, 0x59, 0x0D, 0x27, 0xA6, 0x36, 0x02, 0x0F, 0x58, 0x48, 0x70, 0x0F, 0x11, 0x0B};
        constexpr std::uintptr_t kPositionStepRva = 0x330F370;
        // The natives' entity-to-handle path: 0xEE5C3C `jmp 0x2B07598`;
        // 0x2B07598 looks the entity's script guid extension up (entity
        // +0x10), allocates one in the guid pool when missing (the guid
        // pool decoder is in the signature), and returns
        // (guid slot << 8) | its flag byte (0x2B07689..).
        constexpr std::uintptr_t kGuidJumpRva = 0xEE5C3C;
        constexpr std::uint8_t kGuidJumpSignature[] = {0xE9, 0x57, 0x19, 0xC2, 0x01};
        constexpr std::uintptr_t kCreateGuidRva = 0x2B07598;
        constexpr std::uint8_t kCreateGuidSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83,
            0xEC, 0x20, 0x8B, 0x15, 0x12, 0xCB, 0xEA, 0x02, 0x48, 0x8B, 0xF1, 0x48, 0x83, 0xC1, 0x10, 0x33, 0xFF, 0x32,
            0xDB, 0xE8, 0xAE, 0x67, 0x0D, 0x00, 0x48, 0x85, 0xC0, 0x75, 0x65, 0x8A, 0x05, 0x13, 0xCA, 0xEA, 0x02, 0x84,
            0xC0, 0x75, 0x05, 0x45, 0x33, 0xC0, 0xEB, 0x37, 0x48, 0x8B, 0x0D, 0x13, 0xCA, 0xEA, 0x02, 0x4C, 0x8B, 0x05,
            0x04, 0xCA, 0xEA, 0x02, 0x48, 0xC1, 0xC1, 0x20, 0x48, 0xC1, 0xC9, 0x02, 0x4C, 0x33, 0xC1, 0x8B, 0xC1, 0x83,
            0xE0, 0x1F, 0x83, 0xC0, 0x03, 0x8A, 0xC8, 0x8A, 0x05, 0xE1, 0xC9, 0xEA, 0x02, 0x49, 0xD3, 0xC0, 0x49, 0xC1,
            0xC0, 0x20, 0x84, 0xC0, 0x74, 0xC7, 0x49, 0xF7, 0xD0};
        constexpr std::uintptr_t kGuidHandleRva = 0x2B07689;
        constexpr std::uint8_t kGuidHandleSignature[] = {
            0x49, 0x63, 0x49, 0x1C, 0x49, 0x8B, 0xC2, 0x49, 0x2B, 0x41, 0x08, 0x48, 0x99, 0x48, 0xF7, 0xF9, 0x4C, 0x8B,
            0xC0, 0x48, 0x63, 0xC8, 0x49, 0x8B, 0x41, 0x10, 0x41, 0xC1, 0xE0, 0x08, 0x0F, 0xB6, 0x3C, 0x08, 0x41, 0x03,
            0xF8};

        using CreateGuidFn = std::int32_t(__fastcall*)(void* entity);

        bool g_checked = false;
        bool g_ok = false;
        bool g_enabled = true;
        std::string g_error;
        std::uintptr_t g_base = 0;
        CreateGuidFn g_createGuid = nullptr;

        struct PoolSite
        {
            std::uintptr_t rva;
            Cipher cipher;
            std::int32_t minEntry;
            std::int32_t maxEntry;
            std::uint8_t type;
            const char* name;
        };
        const PoolSite kSites[kKinds] = {
            {kPedPoolRva, kPedCipher, kPedEntry, kPedEntry, kTypePed, "peds"},
            {kVehiclePoolRva, kVehicleCipher, kVehicleEntryMin, kVehicleEntryMax, kTypeVehicle, "vehicles"},
            {kObjectPoolRva, kObjectCipher, kObjectEntry, kObjectEntry, kTypeObject, "objects"},
        };
        const PoolSite kGuidSite{kGuidPoolRva, kGuidCipher, kGuidEntry, kGuidEntry, 0, "guids"};

        bool GuardedCompare(std::uintptr_t target, const std::uint8_t* signature, std::size_t size)
        {
            __try
            {
                return std::memcmp(reinterpret_cast<const void*>(target), signature, size) == 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

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
            __try
            {
                return g_createGuid(reinterpret_cast<void*>(entity));
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
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
            const std::uintptr_t global = g_base + site.rva;
            if (!GuardedCopy(&present, global - 8, 1) || !present || !GuardedCopy(&value, global, 8) ||
                !GuardedCopy(&key, global + 8, 8))
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
        g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        std::uintptr_t imageSize = 0;
        const auto fail = [&](const char* what) {
            g_error = what;
            error = g_error;
            g_ok = false;
            return false;
        };
        if (!g_base || !GuardedImageSize(g_base, imageSize) || imageSize < kGuidPoolRva + 0x10)
            return fail("image");
        struct Signature
        {
            std::uintptr_t rva;
            const std::uint8_t* bytes;
            std::size_t size;
            const char* name;
        };
        const Signature signatures[] = {
            {kSwitchRva, kSwitchSignature, sizeof(kSwitchSignature), "pool_switch_signature_mismatch"},
            {kGetAtRva, kGetAtSignature, sizeof(kGetAtSignature), "get_at_signature_mismatch"},
            {kDeleteRva, kDeleteSignature, sizeof(kDeleteSignature), "delete_signature_mismatch"},
            {kTypesRva, kTypesSignature, sizeof(kTypesSignature), "type_signature_mismatch"},
            {kPositionRva, kPositionSignature, sizeof(kPositionSignature), "position_signature_mismatch"},
            {kGuidJumpRva, kGuidJumpSignature, sizeof(kGuidJumpSignature), "guid_jump_signature_mismatch"},
            {kCreateGuidRva, kCreateGuidSignature, sizeof(kCreateGuidSignature), "create_guid_signature_mismatch"},
            {kGuidHandleRva, kGuidHandleSignature, sizeof(kGuidHandleSignature), "guid_handle_signature_mismatch"},
        };
        for (const Signature& signature : signatures)
        {
            if (!GuardedCompare(g_base + signature.rva, signature.bytes, signature.size))
                return fail(signature.name);
        }
        float step[4]{};
        if (!GuardedCopy(step, g_base + kPositionStepRva, sizeof(step)) || step[0] != kPositionStep ||
            step[1] != kPositionStep || step[2] != 0.0f || step[3] != 0.0f)
            return fail("position_step_mismatch");
        // The pools are decoded and validated on every use (a pool that does
        // not decode to a header of its class makes that call return -1),
        // so only the code is checked here.
        g_createGuid = reinterpret_cast<CreateGuidFn>(g_base + kCreateGuidRva);
        g_ok = true;
        g_error.clear();
        error.clear();
        return true;
    }

    bool Available()
    {
        if (!g_enabled)
            return false;
        std::string error;
        return Check(error);
    }

    void SetEnabled(bool enabled) { g_enabled = enabled; }
    bool Enabled() { return g_enabled; }

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
        out << "ENTITY_POOLS,status=" << (ok ? "ok" : "unavailable") << ",enabled=" << (g_enabled ? 1 : 0);
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
