#pragma once

// The mixer's scene pool read directly (every scene the engine has, not only
// the starts the mod saw), the scenes the engine starts straight into the pool
// by game state ("drivers"), and the replay's ownership of the hidden live
// player's state scenes. Pure part; unit tested in
// tests/category_state_tests.cpp.
//
// Build 1491.50:
//   - the scene start 0x2F18E40(mixer, settings, &reference, ...) takes a
//     slot of 0x340 bytes from the scene pool (0x2B0FBD0) without checking
//     the hash, constructs it (0x327334C zeroes +0x328), sets the settings
//     (0x32772B2: +0x308) and, with a reference, writes the scene there and
//     the reference's address at +0x310 (0x2F18F85); only the wrapper
//     0x2F19018 writes the hash at +0x328 (0x2F19087). A failed or full pool
//     returns 0 without touching the reference (0x2F18FFE), and every caller
//     releases its temporary list afterwards (0x2F03DAC handles an empty one);
//   - the pool's pointer is kept encoded in three cells: a byte 0x5CAF468
//     (set: valid), 0x5CAF470 and the key 0x5CAF478, decoded as in
//     0x2F18E74..0x2F18EAB and 0x2F1C8B6..0x2F1C8E5 (ror 5, rol 32, xor, rol
//     32, rol by (key & 31) + 1, not); the pool: +8 base, +0x10 the flag
//     bytes (a slot is in use when its flag has bit 7 clear), +0x18 count,
//     +0x1C stride (0x2F1C8E8..0x2F1C916, the update's loop);
//   - a scene is ready to be deleted when its 16 patch slots (+0x70, stride
//     0x10), its 16 slots at +0x170 and its list at +0x270 (count +0x2F0)
//     are all empty (0x3278B0C): a stopped scene clears them (0x327A07C) and
//     its reference (+0x310, and the reference itself);
//   - the drivers: 0x2F2D1EC (the speech duck 0x576520E1 and the second duck
//     0x0E585610), 0x2F216E0 (ten scenes by game state) start their scenes
//     straight into 0x2F18E40, so the hash is never at +0x328; each keeps its
//     scene in a reference of its own, used by that one start site (the ref
//     names the scene). 0x2F297E4 starts 0x24BFC688 and 0x2348B998 through
//     the wrapper by the hidden live player's state (bit 14 of +0x3F50 of
//     the local player's component), and when the start gives no scene it
//     tries again the next frame (0x2F2998E, 0x2F298F1).
// Observed live: the speech duck 0x576520E1 is live exactly in the recorded
// conversations; 0x24BFC688 would follow the hidden player in the replay; and
// a list of open scenes kept from the starts alone misses the session's
// permanent scenes when the hooks warm up late, so the replay would start
// second instances of them.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace replay::mixscene::pool
{
    // ---- the encoded pool pointer ----
    constexpr std::uint32_t kCellValidRva = 0x5CAF468; // byte
    constexpr std::uint32_t kCellValueRva = 0x5CAF470; // u64
    constexpr std::uint32_t kCellKeyRva = 0x5CAF478;   // u64

    inline std::uint64_t Rol(std::uint64_t v, unsigned n)
    {
        n &= 63u;
        return n ? (v << n) | (v >> (64u - n)) : v;
    }
    inline std::uint64_t Ror(std::uint64_t v, unsigned n) { return Rol(v, (64u - (n & 63u)) & 63u); }

    // 0x2F18E74..0x2F18EAB.
    inline std::uint64_t Decode(std::uint64_t value, std::uint64_t key)
    {
        const std::uint64_t k = Rol(Ror(key, 5), 32);
        std::uint64_t d = value ^ k;
        const unsigned shift = (static_cast<std::uint32_t>(k) & 0x1Fu) + 1u;
        d = Rol(d, 32);
        d = Rol(d, shift);
        return ~d;
    }
    // The inverse (tests build a pool cell with it).
    inline std::uint64_t Encode(std::uint64_t pointer, std::uint64_t key)
    {
        const std::uint64_t k = Rol(Ror(key, 5), 32);
        const unsigned shift = (static_cast<std::uint32_t>(k) & 0x1Fu) + 1u;
        std::uint64_t d = ~pointer;
        d = Ror(d, shift);
        d = Ror(d, 32);
        return d ^ k;
    }

    // A code site the scan relies on (bytes, and for a rip operand the
    // disp32's offset, the instruction's end and the target).
    struct CodeSite
    {
        const char* name;
        std::uint32_t rva;
        std::uint8_t size;
        std::uint8_t bytes[33];
        std::int8_t ripAt;
        std::uint8_t insnEnd;
        std::uint32_t target;
    };
    constexpr CodeSite kSites[] = {
        {"pool_valid", 0x2F18E66, 6, {0x8A, 0x05, 0xFC, 0x65, 0xD9, 0x02}, 2, 6, kCellValidRva},
        {"pool_key", 0x2F18E74, 7, {0x48, 0x8B, 0x0D, 0xFD, 0x65, 0xD9, 0x02}, 3, 7, kCellKeyRva},
        {"pool_value", 0x2F18E7B, 7, {0x48, 0x8B, 0x15, 0xEE, 0x65, 0xD9, 0x02}, 3, 7, kCellValueRva},
        // ror rcx,5; rol rcx,20h; xor rdx,rcx; mov eax,ecx; and eax,1Fh; rol rdx,20h; inc eax; mov cl,al;
        // mov al,[valid]; rol rdx,cl
        {"pool_decode", 0x2F18E82, 33, {0x48, 0xC1, 0xC9, 0x05, 0x48, 0xC1, 0xC1, 0x20, 0x48, 0x33, 0xD1, 0x8B, 0xC1, 0x83,
            0xE0, 0x1F, 0x48, 0xC1, 0xC2, 0x20, 0xFF, 0xC0, 0x8A, 0xC8, 0x8A, 0x05, 0xC8, 0x65, 0xD9, 0x02, 0x48, 0xD3, 0xC2},
            -1, 0, 0},
        {"pool_not", 0x2F18EAB, 3, {0x48, 0xF7, 0xD2}, -1, 0, 0},
        {"pool_count", 0x2F1C8E8, 3, {0x8B, 0x73, 0x18}, -1, 0, 0},
        {"pool_flags", 0x2F1C8F0, 4, {0x48, 0x8B, 0x43, 0x10}, -1, 0, 0},
        {"pool_stride", 0x2F1C8FF, 4, {0x0F, 0xAF, 0x43, 0x1C}, -1, 0, 0},
        {"pool_free_bit", 0x2F1C903, 6, {0x81, 0xE7, 0x80, 0x00, 0x00, 0x00}, -1, 0, 0},
        {"pool_base", 0x2F1C916, 4, {0x48, 0x03, 0x53, 0x08}, -1, 0, 0},
        {"scene_reference_store", 0x2F18F85, 10, {0x48, 0x89, 0x1E, 0x48, 0x89, 0xB3, 0x10, 0x03, 0x00, 0x00}, -1, 0, 0},
        {"scene_hash_store", 0x2F19087, 6, {0x89, 0x8A, 0x28, 0x03, 0x00, 0x00}, -1, 0, 0},
        {"scene_settings_store", 0x32772B2, 7, {0x48, 0x89, 0x91, 0x08, 0x03, 0x00, 0x00}, -1, 0, 0},
        {"scene_hash_zeroed", 0x32733CC, 7, {0x4C, 0x89, 0x89, 0x28, 0x03, 0x00, 0x00}, -1, 0, 0},
    };
    constexpr std::size_t kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

    inline std::uint32_t SiteTarget(const CodeSite& site)
    {
        if (site.ripAt < 0)
            return 0;
        const std::uint8_t* d = site.bytes + site.ripAt;
        const std::int32_t disp = static_cast<std::int32_t>(static_cast<std::uint32_t>(d[0]) |
            (static_cast<std::uint32_t>(d[1]) << 8) | (static_cast<std::uint32_t>(d[2]) << 16) |
            (static_cast<std::uint32_t>(d[3]) << 24));
        return static_cast<std::uint32_t>(static_cast<std::int64_t>(site.rva) + site.insnEnd + disp);
    }

    // 0x3278B0C whole (93 bytes, up to its ret): the layout of ReadyToDelete.
    constexpr std::uint32_t kReadyToDeleteRva = 0x3278B0C;
    constexpr std::uint8_t kReadyToDeleteSignature[] = {0x48, 0x8D, 0x41, 0x70, 0x45, 0x33, 0xC9, 0x48, 0x8D, 0x91, 0x70,
        0x01, 0x00, 0x00, 0xEB, 0x09, 0x4C, 0x39, 0x08, 0x75, 0x48, 0x48, 0x83, 0xC0, 0x10, 0x48, 0x3B, 0xC2, 0x75, 0xF2,
        0x41, 0x8B, 0xD1, 0x48, 0x8D, 0x81, 0x70, 0x01, 0x00, 0x00, 0x4C, 0x39, 0x08, 0x75, 0x30, 0xFF, 0xC2, 0x48, 0x83,
        0xC0, 0x10, 0x83, 0xFA, 0x10, 0x72, 0xF0, 0x48, 0x8D, 0x91, 0x70, 0x02, 0x00, 0x00, 0x48, 0x63, 0x89, 0xF0, 0x02,
        0x00, 0x00, 0x4C, 0x8D, 0x04, 0xCA, 0xEB, 0x09, 0x4C, 0x39, 0x0A, 0x75, 0x0C, 0x48, 0x83, 0xC2, 0x08, 0x49, 0x3B,
        0xD0, 0x75, 0xF2, 0xB0, 0x01, 0xC3};

    // ---- the pool and the scene ----
    constexpr std::uint32_t kPoolBase = 0x08;
    constexpr std::uint32_t kPoolFlags = 0x10;
    constexpr std::uint32_t kPoolCount = 0x18;
    constexpr std::uint32_t kPoolStride = 0x1C;
    constexpr std::uint8_t kSlotFree = 0x80;
    constexpr std::uint32_t kMaxSlots = 2048;

    constexpr std::uint32_t kSceneSize = 0x340;
    constexpr std::uint32_t kSceneVars = 0x18;      // 10 pairs {name hash, value}
    constexpr std::uint32_t kSceneVarCount = 0x68;
    constexpr std::uint32_t kScenePatches = 0x70;   // 16 x {patch, group}
    constexpr std::uint32_t kSceneSlots2 = 0x170;   // 16 x 0x10
    constexpr std::uint32_t kSceneList = 0x270;     // qwords, count at +0x2F0
    constexpr std::uint32_t kSceneListCount = 0x2F0;
    constexpr std::uint32_t kSceneSettings = 0x308;
    constexpr std::uint32_t kSceneReference = 0x310;
    constexpr std::uint32_t kSceneOwner = 0x318;
    constexpr std::uint32_t kSceneHash = 0x328;
    constexpr std::uint32_t kApplyVar = 0xE865CDE8u; // joaat("apply")

    inline std::uint64_t Qword(const std::uint8_t* scene, std::uint32_t offset)
    {
        std::uint64_t v = 0;
        std::memcpy(&v, scene + offset, 8);
        return v;
    }
    inline std::uint32_t Dword(const std::uint8_t* scene, std::uint32_t offset)
    {
        std::uint32_t v = 0;
        std::memcpy(&v, scene + offset, 4);
        return v;
    }

    // 0x3278B0C on a copy of the scene (kSceneSize bytes).
    inline bool ReadyToDelete(const std::uint8_t* scene)
    {
        for (std::uint32_t n = 0; n < 16; ++n)
            if (Qword(scene, kScenePatches + n * 0x10))
                return false;
        for (std::uint32_t n = 0; n < 16; ++n)
            if (Qword(scene, kSceneSlots2 + n * 0x10))
                return false;
        const std::int32_t count = static_cast<std::int32_t>(Dword(scene, kSceneListCount));
        for (std::int32_t n = 0; n < count && n < 16; ++n)
            if (Qword(scene, kSceneList + static_cast<std::uint32_t>(n) * 8))
                return false;
        return true;
    }

    inline std::uint32_t Patches(const std::uint8_t* scene)
    {
        std::uint32_t count = 0;
        for (std::uint32_t n = 0; n < 16; ++n)
            count += Qword(scene, kScenePatches + n * 0x10) ? 1u : 0u;
        return count;
    }

    // The value of variable `name` in the scene copy (false when absent).
    inline bool VarOf(const std::uint8_t* scene, std::uint32_t name, float& value)
    {
        const std::int32_t count = static_cast<std::int32_t>(Dword(scene, kSceneVarCount));
        for (std::int32_t n = 0; n < count && n < 10; ++n)
        {
            const std::uint32_t at = kSceneVars + static_cast<std::uint32_t>(n) * 8;
            if (Dword(scene, at) == name)
            {
                std::memcpy(&value, scene + at + 4, 4);
                return true;
            }
        }
        return false;
    }

    // ---- the drivers ----

    enum class Via : std::uint8_t
    {
        Direct,  // straight into 0x2F18E40: no hash at +0x328, named by its reference
        Wrapper, // through 0x2F19018: the start detour sees it (refusable by its return address)
    };

    struct Driver
    {
        const char* name;
        std::uint32_t hash;
        std::uint32_t refRva;
        std::uint32_t callRva;  // the call of the start
        std::uint32_t leaRva;   // lea r8,[reference]
        std::uint32_t hashRva;  // mov edx,hash or mov dword [rbp+x],hash
        std::uint8_t hashSize;
        std::uint8_t hashAt;    // the immediate's offset in hashBytes
        Via via;
        std::uint8_t callBytes[5];
        std::uint8_t leaBytes[7];
        std::uint8_t hashBytes[7];

        std::uint32_t ReturnRva() const { return callRva + 5; }
    };

    constexpr std::uint32_t kPoolStartRva = 0x2F18E40;
    constexpr std::uint32_t kWrapperStartRva = 0x2F19018;

    // 0x467F39D6 (0x2F21C17) is left out: its reference 0x5BA5F30 is also
    // where 0x2F18E40 keeps any exclusive scene (0x2F18FF3), so the ref does
    // not name it.
    constexpr Driver kDrivers[] = {
        {"speech_duck", 0x576520E1u, 0x5BA5FE0, 0x2F2D2A9, 0x2F2D281, 0x2F2D26B, 5, 1, Via::Direct, {0xE8, 0x92, 0xBB, 0xFE, 0xFF}, {0x4C, 0x8D, 0x05, 0x58, 0x8D, 0xC7, 0x02}, {0xBA, 0xE1, 0x20, 0x65, 0x57}},
        {"speech_duck_2", 0x0E585610u, 0x5BA5FE8, 0x2F2D4C0, 0x2F2D498, 0x2F2D482, 5, 1, Via::Direct, {0xE8, 0x7B, 0xB9, 0xFE, 0xFF}, {0x4C, 0x8D, 0x05, 0x49, 0x8B, 0xC7, 0x02}, {0xBA, 0x10, 0x56, 0x58, 0x0E}},
        {"state_3e338f46", 0x3E338F46u, 0x5BA5F08, 0x2F217C2, 0x2F21799, 0x2F2177D, 5, 1, Via::Direct, {0xE8, 0x79, 0x76, 0xFF, 0xFF}, {0x4C, 0x8D, 0x05, 0x68, 0x47, 0xC8, 0x02}, {0xBA, 0x46, 0x8F, 0x33, 0x3E}},
        {"state_592b49d2", 0x592B49D2u, 0x5BA5F38, 0x2F21915, 0x2F218EC, 0x2F218D0, 5, 1, Via::Direct, {0xE8, 0x26, 0x75, 0xFF, 0xFF}, {0x4C, 0x8D, 0x05, 0x45, 0x46, 0xC8, 0x02}, {0xBA, 0xD2, 0x49, 0x2B, 0x59}},
        {"state_5167ffe9", 0x5167FFE9u, 0x5BA6078, 0x2F219A1, 0x2F21978, 0x2F2195D, 5, 1, Via::Direct, {0xE8, 0x9A, 0x74, 0xFF, 0xFF}, {0x4C, 0x8D, 0x05, 0xF9, 0x46, 0xC8, 0x02}, {0xBA, 0xE9, 0xFF, 0x67, 0x51}},
        {"state_e494db3e", 0xE494DB3Eu, 0x5BA6080, 0x2F21A2E, 0x2F21A08, 0x2F219EE, 5, 1, Via::Direct, {0xE8, 0x0D, 0x74, 0xFF, 0xFF}, {0x4C, 0x8D, 0x05, 0x71, 0x46, 0xC8, 0x02}, {0xBA, 0x3E, 0xDB, 0x94, 0xE4}},
        {"state_f36b4f3a", 0xF36B4F3Au, 0x5BA6098, 0x2F21AC7, 0x2F21AA1, 0x2F21A87, 5, 1, Via::Direct, {0xE8, 0x74, 0x73, 0xFF, 0xFF}, {0x4C, 0x8D, 0x05, 0xF0, 0x45, 0xC8, 0x02}, {0xBA, 0x3A, 0x4F, 0x6B, 0xF3}},
        {"state_a496be1f", 0xA496BE1Fu, 0x5BA60A0, 0x2F21B46, 0x2F21B20, 0x2F21B06, 5, 1, Via::Direct, {0xE8, 0xF5, 0x72, 0xFF, 0xFF}, {0x4C, 0x8D, 0x05, 0x79, 0x45, 0xC8, 0x02}, {0xBA, 0x1F, 0xBE, 0x96, 0xA4}},
        {"state_703e9005", 0x703E9005u, 0x5BA5F18, 0x2F21CC6, 0x2F21CA0, 0x2F21C86, 5, 1, Via::Direct, {0xE8, 0x75, 0x71, 0xFF, 0xFF}, {0x4C, 0x8D, 0x05, 0x71, 0x42, 0xC8, 0x02}, {0xBA, 0x05, 0x90, 0x3E, 0x70}},
        {"state_9d7c75d4", 0x9D7C75D4u, 0x5BA6090, 0x2F21D3E, 0x2F21D18, 0x2F21CFE, 5, 1, Via::Direct, {0xE8, 0xFD, 0x70, 0xFF, 0xFF}, {0x4C, 0x8D, 0x05, 0x71, 0x43, 0xC8, 0x02}, {0xBA, 0xD4, 0x75, 0x7C, 0x9D}},
        {"state_ab0410f1", 0xAB0410F1u, 0x5BA5F70, 0x2F21E98, 0x2F21E72, 0x2F21E58, 5, 1, Via::Direct, {0xE8, 0xA3, 0x6F, 0xFF, 0xFF}, {0x4C, 0x8D, 0x05, 0xF7, 0x40, 0xC8, 0x02}, {0xBA, 0xF1, 0x10, 0x04, 0xAB}},
        {"player_state", 0x24BFC688u, 0x5BA6038, 0x2F29980, 0x2F29950, 0x2F29979, 7, 3, Via::Wrapper, {0xE8, 0x93, 0xF6, 0xFE, 0xFF}, {0x4C, 0x8D, 0x05, 0xE1, 0xC6, 0xC7, 0x02}, {0xC7, 0x45, 0x18, 0x88, 0xC6, 0xBF, 0x24}},
        {"player_state_2", 0x2348B998u, 0x5BA6030, 0x2F298E3, 0x2F298B3, 0x2F298DC, 7, 3, Via::Wrapper, {0xE8, 0x30, 0xF7, 0xFE, 0xFF}, {0x4C, 0x8D, 0x05, 0x76, 0xC7, 0xC7, 0x02}, {0xC7, 0x45, 0x10, 0x98, 0xB9, 0x48, 0x23}},
    };
    constexpr std::size_t kDriverCount = sizeof(kDrivers) / sizeof(kDrivers[0]);

    // What a wrapper driver does when the start gives no scene: it tests
    // the reference and tries again the next frame (the refusal's proof).
    struct Tolerance
    {
        std::uint32_t hash;
        std::uint32_t rva;
        std::uint8_t size;
        std::uint8_t bytes[12];
        std::uint32_t refRva; // the rip operand of the first instruction
    };
    constexpr Tolerance kTolerances[] = {
        // cmp [0x5BA6038],rbx; je exit
        {0x24BFC688u, 0x2F2998E, 9, {0x48, 0x39, 0x1D, 0xA3, 0xC6, 0xC7, 0x02, 0x74, 0x5E}, 0x5BA6038},
        // mov rcx,[0x5BA6030]; test rcx,rcx; je on
        {0x2348B998u, 0x2F298F1, 12, {0x48, 0x8B, 0x0D, 0x38, 0xC7, 0xC7, 0x02, 0x48, 0x85, 0xC9, 0x74, 0x3E}, 0x5BA6030},
    };

    inline std::int32_t Rel32(const std::uint8_t* d)
    {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(d[0]) | (static_cast<std::uint32_t>(d[1]) << 8) |
            (static_cast<std::uint32_t>(d[2]) << 16) | (static_cast<std::uint32_t>(d[3]) << 24));
    }
    inline std::uint32_t CallTarget(const Driver& d)
    {
        return static_cast<std::uint32_t>(static_cast<std::int64_t>(d.callRva) + 5 + Rel32(d.callBytes + 1));
    }
    inline std::uint32_t LeaTarget(const Driver& d)
    {
        return static_cast<std::uint32_t>(static_cast<std::int64_t>(d.leaRva) + 7 + Rel32(d.leaBytes + 3));
    }
    inline std::uint32_t HashImmediate(const Driver& d)
    {
        std::uint32_t v = 0;
        std::memcpy(&v, d.hashBytes + d.hashAt, 4);
        return v;
    }
    inline std::uint32_t ToleranceTarget(const Tolerance& t)
    {
        return static_cast<std::uint32_t>(static_cast<std::int64_t>(t.rva) + 7 + Rel32(t.bytes + 3));
    }

    // The table is consistent with itself (the tests and the runtime check
    // before the bytes are compared with the game's).
    inline bool DriverConsistent(const Driver& d)
    {
        const std::uint32_t target = d.via == Via::Direct ? kPoolStartRva : kWrapperStartRva;
        return d.callBytes[0] == 0xE8 && CallTarget(d) == target && LeaTarget(d) == d.refRva && HashImmediate(d) == d.hash;
    }

    inline const Driver* DriverByRef(std::uint32_t refRva)
    {
        for (const Driver& d : kDrivers)
            if (d.refRva == refRva)
                return &d;
        return nullptr;
    }
    inline const Driver* DriverByHash(std::uint32_t hash)
    {
        for (const Driver& d : kDrivers)
            if (d.hash == hash)
                return &d;
        return nullptr;
    }
    inline const Driver* DriverByReturn(std::uint32_t returnRva)
    {
        for (const Driver& d : kDrivers)
            if (d.ReturnRva() == returnRva)
                return &d;
        return nullptr;
    }
    inline bool IsDirectCaller(std::uint32_t callerRva)
    {
        const Driver* d = DriverByReturn(callerRva);
        return d && d->via == Via::Direct;
    }
    // A wrapper driver whose null reference is proven harmless (the replay
    // may refuse its start while it owns the scene).
    inline bool Refusable(const Driver& d)
    {
        if (d.via != Via::Wrapper)
            return false;
        for (const Tolerance& t : kTolerances)
            if (t.hash == d.hash && ToleranceTarget(t) == d.refRva)
                return true;
        return false;
    }

    // ---- a scan ----

    enum class Name : std::uint8_t
    {
        None,    // no hash at +0x328 and no driver reference
        Wrapper, // the hash at +0x328
        Direct,  // a driver's reference
    };
    inline const char* NameText(Name n)
    {
        switch (n)
        {
        case Name::Wrapper: return "wrapper";
        case Name::Direct: return "direct";
        default: return "none";
        }
    }

    struct PoolScene
    {
        std::uint32_t slot = 0;
        std::uint64_t scene = 0;
        std::uint64_t settings = 0;
        std::uint64_t reference = 0; // where the scene is kept (+0x310)
        std::uint64_t owner = 0;
        std::uint32_t hash = 0;      // +0x328, else the driver's
        Name name = Name::None;
        const Driver* driver = nullptr;
        std::uint32_t patches = 0;
        bool hasApply = false;
        float apply = 0.0f;
        bool replayOwned = false;    // a reference of the replay's own
    };

    // Names a scene copy: +0x328 when set, else the driver whose reference
    // (an RVA, 0 when outside the image) holds it.
    inline void NameScene(PoolScene& s, std::uint32_t hashAt328, std::uint32_t referenceRva)
    {
        s.hash = hashAt328;
        s.name = hashAt328 ? Name::Wrapper : Name::None;
        s.driver = referenceRva ? DriverByRef(referenceRva) : nullptr;
        if (!hashAt328 && s.driver)
        {
            s.hash = s.driver->hash;
            s.name = Name::Direct;
        }
    }

    // The hashes of the scenes the engine has (not the replay's own).
    inline std::vector<std::uint32_t> EngineOpen(const std::vector<PoolScene>& scenes)
    {
        std::vector<std::uint32_t> out;
        for (const PoolScene& s : scenes)
            if (!s.replayOwned && s.hash)
                out.push_back(s.hash);
        return out;
    }

    // The same, leaving out too the scenes the replay is running now, by pointer.
    // A script scene the replay starts with START_AUDIO_SCENE (0x31C8820 ->
    // 0x2F19018) keeps the reference of the script thread's slot, not one of the
    // replay's, so replayOwned does not see it: counted as the engine's, the next
    // tick would stop it as "engine_owns" and start it again until kMaxAttempts,
    // then lose it.
    inline std::vector<std::uint32_t> EngineOpen(const std::vector<PoolScene>& scenes, const std::vector<std::uint64_t>& running)
    {
        std::vector<std::uint32_t> out;
        for (const PoolScene& s : scenes)
            if (!s.replayOwned && s.hash && std::find(running.begin(), running.end(), s.scene) == running.end())
                out.push_back(s.hash);
        return out;
    }

    // How many alive scenes carry each hash (duplicates of one hash).
    inline std::size_t Duplicates(const std::vector<PoolScene>& scenes, std::uint32_t hash)
    {
        std::size_t n = 0;
        for (const PoolScene& s : scenes)
            if (s.hash == hash)
                ++n;
        return n;
    }
}
