#include "timecycle_state.h"
#include "guarded_memory.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

// See timecycle_state.h for the code each signature covers. The bytes are the
// build 1491.50 dump's; the rip-relative and call operands are checked against
// the documented targets as well.
namespace replay::tcstate
{
    namespace
    {
        struct RipOperand
        {
            std::size_t dispOffset; // where the disp32 is, from the site
            std::size_t insnEnd;    // where the instruction ends, from the site
            std::uintptr_t targetRva;
        };

        constexpr std::uintptr_t kSetModifierHandlerRva = 0xF8F4E8;
        constexpr std::uint8_t kSetModifierHandler[] = {
            0x48, 0x8B, 0x49, 0x10, 0x48, 0x8B, 0x09, 0xE9, 0x64, 0x32, 0x01, 0x00, 0x48, 0x8B, 0x41, 0x10,
            0xF3, 0x0F, 0x10, 0x00, 0xF3, 0x0F, 0x11, 0x05, 0xF0, 0xA0, 0xB0, 0x03, 0xC3,
        };
        constexpr std::uintptr_t kSetModifierRva = 0xFA2758;
        constexpr std::uint8_t kSetModifier[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD1, 0x33, 0xC9, 0xE8, 0x1C, 0x75, 0x66, 0x01,
            0x45, 0x33, 0xC9, 0x89, 0x44, 0x24, 0x38, 0x4C, 0x8D, 0x05, 0x72, 0x1E, 0x4B, 0x02, 0x48, 0x8D,
            0x54, 0x24, 0x38, 0x48, 0x8D, 0x0D, 0xFE, 0x46, 0xAF, 0x03, 0xE8, 0xF1, 0xC5, 0xB8, 0x01, 0x83,
            0x0D, 0x6A, 0x6E, 0xAF, 0x03, 0xFF, 0x8B, 0x1D, 0xB8, 0x2D, 0xCE, 0x02, 0x89, 0x05, 0x56, 0x6E,
            0xAF, 0x03,
        };
        constexpr std::uintptr_t kModifierIndexRva = 0x2B2ED78;
        constexpr std::uint8_t kModifierIndex[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x02, 0x48, 0x8B, 0xD9, 0x48, 0x8D, 0x54, 0x24, 0x30,
            0x89, 0x44, 0x24, 0x30, 0xE8, 0x8B, 0xFF, 0xFF, 0xFF, 0x44, 0x0F, 0xB7, 0x4B, 0x48, 0x33, 0xD2,
            0x66, 0x41, 0x3B, 0xD1, 0x73, 0x16, 0x4C, 0x8B, 0x43, 0x40, 0x0F, 0xB7, 0xCA, 0x49, 0x39, 0x04,
            0xC8, 0x74, 0x12, 0x66, 0xFF, 0xC2, 0x66, 0x41, 0x3B, 0xD1, 0x72, 0xEE, 0x83, 0xC8, 0xFF, 0x48,
            0x83, 0xC4, 0x20, 0x5B, 0xC3, 0x0F, 0xB7, 0xC2, 0xEB, 0xF5,
        };
        constexpr std::uintptr_t kModifierAliasRva = 0x2B2ED1C;
        constexpr std::uint8_t kModifierAlias[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x8B, 0x02, 0x33, 0xDB, 0x45, 0x85, 0xC0, 0x75, 0x04,
            0x33, 0xC0, 0xEB, 0x41, 0x44, 0x89, 0x44, 0x24, 0x38, 0x39, 0x59, 0x38, 0x74, 0x1E, 0x8B, 0xC3,
            0x7E, 0x1A, 0x8B, 0xD0, 0x44, 0x39, 0x44, 0xD1, 0x18, 0x74, 0x09, 0xFF, 0xC0, 0x3B, 0x41, 0x38,
            0x7C, 0xF0, 0xEB, 0x08, 0x8B, 0x44, 0xD1, 0x1C, 0x89, 0x44, 0x24, 0x38, 0x48, 0x83, 0xC1, 0x50,
            0x48, 0x8D, 0x54, 0x24, 0x38, 0xE8, 0xAA, 0x14, 0x92, 0xFD, 0x48, 0x85, 0xC0, 0x74, 0x03, 0x48,
            0x8B, 0x18, 0x48, 0x8B, 0xC3, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3,
        };
        constexpr std::uintptr_t kSortedMapFindRva = 0x450210;
        constexpr std::uint8_t kSortedMapFind[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x7C, 0x24, 0x10, 0x33, 0xFF, 0x4C, 0x8B, 0xD2, 0x66,
            0x39, 0x79, 0x10, 0x76, 0x5D, 0x4C, 0x8B, 0x59, 0x08, 0x0F, 0xB7, 0x49, 0x10, 0x4D, 0x8B, 0xC3,
            0x44, 0x8B, 0xC9, 0x48, 0x85, 0xC9, 0x74, 0x30, 0x8B, 0x1A, 0x49, 0x8B, 0xD1, 0x48, 0xD1, 0xFA,
            0x48, 0x8B, 0xC2, 0x48, 0x03, 0xC0, 0x41, 0x39, 0x1C, 0xC0, 0x73, 0x14, 0x4D, 0x8D, 0x04, 0xC0,
            0x48, 0x83, 0xC8, 0xFF, 0x48, 0x2B, 0xC2, 0x49, 0x83, 0xC0, 0x10, 0x4C, 0x03, 0xC8, 0xEB, 0x03,
            0x4C, 0x8B, 0xCA, 0x4D, 0x85, 0xC9, 0x7F, 0xD2, 0x48, 0xC1, 0xE1, 0x04, 0x49, 0x03, 0xCB, 0x4C,
            0x3B, 0xC1, 0x74, 0x0E, 0x41, 0x8B, 0x02, 0x41, 0x39, 0x00, 0x75, 0x06, 0x49, 0x8D, 0x40, 0x08,
            0xEB, 0x02, 0x33, 0xC0, 0x48, 0x8B, 0x5C, 0x24, 0x08, 0x48, 0x8B, 0x7C, 0x24, 0x10, 0xC3,
        };
        constexpr std::uintptr_t kModifierUpdateRva = 0xF22BC8;
        constexpr std::uint8_t kModifierUpdate[] = {
            0xF3, 0x0F, 0x10, 0x05, 0x6C, 0x0D, 0xA9, 0x04, 0xF3, 0x0F, 0x59, 0x83, 0x80, 0x27, 0x00, 0x00,
            0xF3, 0x0F, 0x58, 0x83, 0x7C, 0x27, 0x00, 0x00, 0xF3, 0x0F, 0x11, 0x83, 0x7C, 0x27, 0x00, 0x00,
            0xEB, 0x08, 0xF3, 0x0F, 0x10, 0x83, 0x7C, 0x27, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x8B, 0x74, 0x27,
            0x00, 0x00, 0x0F, 0x2F, 0xC1, 0x72, 0x12, 0x83, 0x8B, 0x78, 0x27, 0x00, 0x00, 0xFF, 0x89, 0x83,
            0x70, 0x27, 0x00, 0x00, 0xE9, 0x5B, 0x01, 0x00, 0x00, 0x8B, 0x8B, 0x70, 0x27, 0x00, 0x00, 0x85,
            0xC9, 0x78, 0x24, 0x8B, 0xC1, 0xF3, 0x0F, 0x5C, 0xC8, 0x48, 0x8B, 0x4B, 0x40, 0x48, 0x8B, 0x0C,
            0xC1, 0x0F, 0x28, 0xD1, 0xE8, 0x53, 0x4B, 0xC0, 0x01, 0xF3, 0x0F, 0x10, 0x83, 0x7C, 0x27, 0x00,
            0x00, 0x8B, 0x83, 0x78, 0x27, 0x00, 0x00, 0x8B, 0xC8, 0x0F, 0x28, 0xC8, 0xE9, 0x26, 0x01, 0x00,
            0x00, 0x8B, 0x83, 0x70, 0x27, 0x00, 0x00, 0x85, 0xC0, 0x0F, 0x88, 0xA1, 0x00, 0x00, 0x00, 0x8B,
            0xC8, 0x48, 0x8B, 0x43, 0x40, 0x48, 0x8B, 0x0C, 0xC8, 0x40, 0x38, 0xB3, 0x84, 0x27, 0x00, 0x00,
        };
        constexpr std::uintptr_t kClearHandlerRva = 0xF8AC84;
        constexpr std::uint8_t kClearHandler[] = {
            0xE9, 0xE7, 0x8A, 0x00, 0x00,
        };
        constexpr std::uintptr_t kClearModifierRva = 0xF93770;
        constexpr std::uint8_t kClearModifier[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0xF3, 0x0F, 0x10, 0x05, 0x26, 0x90,
            0x37, 0x02, 0x83, 0xC8, 0xFF, 0x8B, 0x3D, 0xC1, 0x1D, 0xCF, 0x02, 0x33, 0xDB, 0xF3, 0x0F, 0x11,
            0x05, 0x5F, 0x5E, 0xB0, 0x03, 0x89, 0x05, 0x55, 0x5E, 0xB0, 0x03, 0x89, 0x05, 0x57, 0x5E, 0xB0,
            0x03, 0x88, 0x1D, 0x5D, 0x5E, 0xB0, 0x03,
        };
        constexpr std::uintptr_t kSetVariationHandlerRva = 0x1047A2C;
        constexpr std::uint8_t kSetVariationHandler[] = {
            0x48, 0x8B, 0x49, 0x10, 0x48, 0x8B, 0x51, 0x08, 0x48, 0x8B, 0x09, 0xE9, 0x20, 0x1A, 0x01, 0x00,
        };
        constexpr std::uintptr_t kSetVariationImplRva = 0x105945C;
        constexpr std::uint8_t kSetVariationImpl[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xD1,
            0x48, 0x8D, 0x0D, 0x6D, 0x8E, 0x51, 0x03, 0xE8, 0xA0, 0x04, 0x6E, 0xFF, 0x4C, 0x8B, 0xC3, 0x48,
            0x8D, 0x0D, 0x5E, 0x8E, 0x51, 0x03, 0x8B, 0xD0, 0x8B, 0xF8, 0xE8, 0x65, 0x09, 0x6E, 0xFF, 0x85,
            0xC0, 0x78, 0x20, 0x85, 0xFF, 0x78, 0x1C, 0x3B, 0x3D, 0x47, 0x8E, 0x51, 0x03, 0x73, 0x14, 0x45,
            0x33, 0xC9, 0x48, 0x8D, 0x0D, 0x3B, 0x8E, 0x51, 0x03, 0x44, 0x8B, 0xC0, 0x8B, 0xD7, 0xE8, 0x3D,
            0xE7, 0x6F, 0xFF, 0x48, 0x8B, 0x5C, 0x24, 0x30, 0x48, 0x83, 0xC4, 0x20, 0x5F, 0xC3,
        };
        constexpr std::uintptr_t kTypeByNameRva = 0x739918;
        constexpr std::uint8_t kTypeByName[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x33, 0xC9, 0xE8, 0x5C, 0x03, 0xED, 0x01,
            0x44, 0x8B, 0x03, 0x33, 0xD2, 0x45, 0x85, 0xC0, 0x74, 0x16, 0x48, 0x8D, 0x4B, 0x10, 0x39, 0x01,
            0x74, 0x17, 0xFF, 0xC2, 0x48, 0x81, 0xC1, 0xE0, 0x01, 0x00, 0x00, 0x41, 0x3B, 0xD0, 0x72, 0xEE,
            0x83, 0xC8, 0xFF, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3, 0x8B, 0xC2, 0xEB, 0xF6,
        };
        constexpr std::uintptr_t kVariationByNameRva = 0x739DF0;
        constexpr std::uint8_t kVariationByName[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xDA, 0x48, 0x8B, 0xF9, 0x49,
            0x8B, 0xD0, 0x33, 0xC9, 0xE8, 0x7B, 0xFE, 0xEC, 0x01, 0x44, 0x8B, 0xC0, 0x8B, 0xD3, 0x48, 0x8B,
            0xCF, 0x48, 0x8B, 0x5C, 0x24, 0x30, 0x48, 0x83, 0xC4, 0x20, 0x5F, 0xE9, 0x94, 0xFF, 0xFF, 0xFF,
        };
        constexpr std::uintptr_t kVariationByKeyRva = 0x739DB4;
        constexpr std::uint8_t kVariationByKey[] = {
            0x8B, 0xC2, 0x33, 0xD2, 0x4C, 0x69, 0xC8, 0xE0, 0x01, 0x00, 0x00, 0x45, 0x0F, 0xB7, 0x94, 0x09,
            0xD0, 0x01, 0x00, 0x00, 0x45, 0x85, 0xD2, 0x74, 0x1B, 0x4D, 0x8B, 0x8C, 0x09, 0xC8, 0x01, 0x00,
            0x00, 0x8B, 0xC2, 0x48, 0x6B, 0xC8, 0x44, 0x46, 0x39, 0x04, 0x09, 0x74, 0x0B, 0xFF, 0xC2, 0x41,
            0x3B, 0xD2, 0x72, 0xED, 0x83, 0xC8, 0xFF, 0xC3,
        };
        constexpr std::uintptr_t kSetVariationRva = 0x757BEC;
        constexpr std::uint8_t kSetVariation[] = {
            0x40, 0x53, 0x4C, 0x8B, 0xD1, 0x44, 0x8B, 0xDA, 0x33, 0xC9, 0x45, 0x85, 0xC0, 0x79, 0x0F, 0x45,
            0x8A, 0xC1, 0x41, 0x8B, 0xD3, 0x49, 0x8B, 0xCA, 0x5B, 0xE9, 0xDE, 0x46, 0xFD, 0xFF, 0x49, 0x69,
            0xD3, 0xE0, 0x01, 0x00, 0x00, 0x49, 0x8B, 0xC3, 0x42, 0x0F, 0xB7, 0x84, 0x12, 0xD0, 0x01, 0x00,
            0x00, 0x66, 0x85, 0xC0, 0x74, 0x66, 0x0F, 0xB7, 0xC0, 0x44, 0x3B, 0xC0, 0x7D, 0x5E, 0x49, 0x63,
            0x9A, 0xB8, 0x3E, 0x00, 0x00, 0x85, 0xDB, 0x7E, 0x17, 0x48, 0x8B, 0xC1, 0x45, 0x39, 0x9C, 0x82,
            0x38, 0x3E, 0x00, 0x00, 0x74, 0x0D, 0xFF, 0xC1, 0x48, 0xFF, 0xC0, 0x48, 0x3B, 0xC3, 0x7C, 0xEC,
            0x83, 0xC9, 0xFF, 0x83, 0xF9, 0xFF, 0x74, 0x1D, 0x8D, 0x43, 0xFF, 0x48, 0x63, 0xC9, 0x41, 0x89,
            0x82, 0xB8, 0x3E, 0x00, 0x00, 0x41, 0x8B, 0x84, 0x9A, 0x34, 0x3E, 0x00, 0x00, 0x41, 0x89, 0x84,
            0x8A, 0x38, 0x3E, 0x00, 0x00, 0x41, 0x8B, 0xC0, 0x48, 0x6B, 0xC8, 0x44, 0x4A, 0x8B, 0x84, 0x12,
            0xC8, 0x01, 0x00, 0x00, 0x8B, 0x0C, 0x01, 0x42, 0x89, 0x4C, 0x12, 0x14, 0x5B, 0xC3,
        };
        constexpr std::uintptr_t kClearVariationRva = 0x72C2E8;
        constexpr std::uint8_t kClearVariation[] = {
            0x33, 0xC0, 0x44, 0x8B, 0xCA, 0x45, 0x84, 0xC0, 0x74, 0x10, 0x49, 0x69, 0xD1, 0xE0, 0x01, 0x00,
            0x00, 0x8B, 0x44, 0x0A, 0x10, 0x89, 0x44, 0x0A, 0x14, 0xC3, 0x4C, 0x63, 0x91, 0xB8, 0x3E, 0x00,
            0x00, 0x45, 0x85, 0xD2, 0x7E, 0x17, 0x48, 0x8B, 0xD0, 0x44, 0x39, 0x8C, 0x91, 0x38, 0x3E, 0x00,
            0x00, 0x74, 0x0D, 0xFF, 0xC0, 0x48, 0xFF, 0xC2, 0x49, 0x3B, 0xD2, 0x7C, 0xEC, 0x83, 0xC8, 0xFF,
            0x83, 0xF8, 0xFF, 0x75, 0x12, 0x41, 0x8D, 0x42, 0x01, 0x89, 0x81, 0xB8, 0x3E, 0x00, 0x00, 0x46,
            0x89, 0x8C, 0x91, 0x38, 0x3E, 0x00, 0x00, 0xC3,
        };

        constexpr std::uintptr_t kTimecycleRva = 0x4A96E80; // T
        constexpr std::uintptr_t kWeatherRva = 0x45722E0;   // W
        constexpr std::uintptr_t kUnitRva = 0x330C7A8;      // .rdata 1.0f (the cleared strength)

        // T's fields (timecycle_track.h).
        constexpr std::uintptr_t kModifierPointers = 0x40; // u64 array
        constexpr std::uintptr_t kModifierCount = 0x48;    // u16
        constexpr std::uintptr_t kMapData = 0x58;          // map at 0x50: data at +0x8
        constexpr std::uintptr_t kMapCount = 0x60;         // u16 at map +0x10
        constexpr std::uintptr_t kFields = 0x2770;         // index, strength, transition, progress, speed, fade byte
        constexpr std::size_t kFieldBytes = 0x15;
        // W's weather types.
        constexpr std::uintptr_t kTypeStride = 0x1E0;
        constexpr std::uintptr_t kTypeHash = 0x10;
        constexpr std::uintptr_t kTypeVariationTable = 0x1C8;
        constexpr std::uintptr_t kTypeVariationCount = 0x1D0; // u16
        constexpr std::uintptr_t kVariationStride = 0x44;
        // Sanity caps on what is copied (a garbage count reads nothing).
        constexpr std::uint32_t kMaxTypes = 64;
        constexpr std::uint32_t kMaxModifiers = 16384;
        constexpr std::uint32_t kMaxTypeVariations = 256;

        bool g_checked = false;
        bool g_ok = false;
        std::string g_error;
        std::uintptr_t g_base = 0;

        bool Site(std::uintptr_t rva, const std::uint8_t* bytes, std::size_t size, const RipOperand* operands, std::size_t count)
        {
            const std::uintptr_t site = g_base + rva;
            if (!mem::Matches(reinterpret_cast<const void*>(site), bytes, size))
                return false;
            for (std::size_t n = 0; n < count; ++n)
            {
                std::int32_t disp = 0;
                if (!mem::Read(site + operands[n].dispOffset, disp))
                    return false;
                const std::uintptr_t target =
                    site + operands[n].insnEnd + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(disp));
                if (target != g_base + operands[n].targetRva)
                    return false;
            }
            return true;
        }

        bool Check(std::string& error)
        {
            g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            constexpr std::uintptr_t T = kTimecycleRva;
            constexpr std::uintptr_t W = kWeatherRva;
            const RipOperand setHandler[] = {{0x08, 0x0C, kSetModifierRva}, {0x18, 0x1C, T + 0x2774}};
            const RipOperand setModifier[] = {{0x26, 0x2A, T}, {0x2B, 0x2F, kModifierIndexRva}, {0x31, 0x36, T + 0x2778}, {0x3E, 0x42, T + 0x2770}};
            const RipOperand modifierIndex[] = {{0x15, 0x19, kModifierAliasRva}};
            const RipOperand modifierAlias[] = {{0x46, 0x4A, kSortedMapFindRva}};
            const RipOperand clearHandler[] = {{0x01, 0x05, kClearModifierRva}};
            const RipOperand clearModifier[] = {
                {0x0E, 0x12, kUnitRva}, {0x21, 0x25, T + 0x2774}, {0x27, 0x2B, T + 0x2770}, {0x2D, 0x31, T + 0x2778}, {0x33, 0x37, T + 0x2784}};
            const RipOperand variationHandler[] = {{0x0C, 0x10, kSetVariationImplRva}};
            const RipOperand variationImpl[] = {{0x13, 0x17, W}, {0x18, 0x1C, kTypeByNameRva}, {0x22, 0x26, W}, {0x2B, 0x2F, kVariationByNameRva},
                {0x39, 0x3D, W}, {0x45, 0x49, W}, {0x4F, 0x53, kSetVariationRva}};
            const RipOperand variationByName[] = {{0x2C, 0x30, kVariationByKeyRva}};
            const RipOperand setVariation[] = {{0x1A, 0x1E, kClearVariationRva}};
            if (!Site(kSetModifierHandlerRva, kSetModifierHandler, sizeof(kSetModifierHandler), setHandler, 2))
                error = "set_modifier_handler_mismatch";
            else if (!Site(kSetModifierRva, kSetModifier, sizeof(kSetModifier), setModifier, 4))
                error = "set_modifier_mismatch";
            else if (!Site(kModifierIndexRva, kModifierIndex, sizeof(kModifierIndex), modifierIndex, 1))
                error = "modifier_index_mismatch";
            else if (!Site(kModifierAliasRva, kModifierAlias, sizeof(kModifierAlias), modifierAlias, 1))
                error = "modifier_alias_mismatch";
            else if (!Site(kSortedMapFindRva, kSortedMapFind, sizeof(kSortedMapFind), nullptr, 0))
                error = "sorted_map_mismatch";
            else if (!Site(kModifierUpdateRva, kModifierUpdate, sizeof(kModifierUpdate), nullptr, 0))
                error = "modifier_update_mismatch";
            else if (!Site(kClearHandlerRva, kClearHandler, sizeof(kClearHandler), clearHandler, 1))
                error = "clear_handler_mismatch";
            else if (!Site(kClearModifierRva, kClearModifier, sizeof(kClearModifier), clearModifier, 5))
                error = "clear_modifier_mismatch";
            else if (!Site(kSetVariationHandlerRva, kSetVariationHandler, sizeof(kSetVariationHandler), variationHandler, 1))
                error = "set_variation_handler_mismatch";
            else if (!Site(kSetVariationImplRva, kSetVariationImpl, sizeof(kSetVariationImpl), variationImpl, 7))
                error = "set_variation_impl_mismatch";
            else if (!Site(kTypeByNameRva, kTypeByName, sizeof(kTypeByName), nullptr, 0))
                error = "type_by_name_mismatch";
            else if (!Site(kVariationByNameRva, kVariationByName, sizeof(kVariationByName), variationByName, 1))
                error = "variation_by_name_mismatch";
            else if (!Site(kVariationByKeyRva, kVariationByKey, sizeof(kVariationByKey), nullptr, 0))
                error = "variation_by_key_mismatch";
            else if (!Site(kSetVariationRva, kSetVariation, sizeof(kSetVariation), setVariation, 1))
                error = "set_variation_mismatch";
            else if (!Site(kClearVariationRva, kClearVariation, sizeof(kClearVariation), nullptr, 0))
                error = "clear_variation_mismatch";
            else
                return true;
            return false;
        }

        std::uintptr_t TimecycleAt(std::uintptr_t offset) { return g_base + kTimecycleRva + offset; }
        std::uintptr_t WeatherAt(std::uintptr_t offset) { return g_base + kWeatherRva + offset; }

        // The modifier array and map, copied again when their header moved
        // (pointer or count): both are built at load, so normally once.
        struct Tables
        {
            std::uint64_t pointersAt = 0;
            std::uint16_t pointerCount = 0;
            std::uint64_t mapAt = 0;
            std::uint16_t mapCount = 0;
            bool valid = false;
            std::vector<std::uint64_t> pointers;
            std::vector<tctrack::MapEntry> map;
        };
        Tables g_tables;

        bool RefreshTables()
        {
            std::uint64_t pointersAt = 0, mapAt = 0;
            std::uint16_t pointerCount = 0, mapCount = 0;
            if (!mem::Read(TimecycleAt(kModifierPointers), pointersAt) || !mem::Read(TimecycleAt(kModifierCount), pointerCount) ||
                !mem::Read(TimecycleAt(kMapData), mapAt) || !mem::Read(TimecycleAt(kMapCount), mapCount))
                return false;
            Tables& t = g_tables;
            if (t.valid && t.pointersAt == pointersAt && t.pointerCount == pointerCount && t.mapAt == mapAt && t.mapCount == mapCount)
                return true;
            t.valid = false;
            t.pointersAt = pointersAt;
            t.pointerCount = pointerCount;
            t.mapAt = mapAt;
            t.mapCount = mapCount;
            if (pointerCount > kMaxModifiers || mapCount > kMaxModifiers)
                return false;
            t.pointers.assign(pointerCount, 0);
            if (pointerCount && !mem::Copy(t.pointers.data(), static_cast<std::uintptr_t>(pointersAt), pointerCount * sizeof(std::uint64_t)))
                return false;
            std::vector<std::uint8_t> raw(static_cast<std::size_t>(mapCount) * 16);
            if (mapCount && !mem::Copy(raw.data(), static_cast<std::uintptr_t>(mapAt), raw.size()))
                return false;
            t.map.resize(mapCount);
            for (std::size_t n = 0; n < mapCount; ++n)
            {
                std::memcpy(&t.map[n].key, raw.data() + n * 16, sizeof(std::uint32_t));
                std::memcpy(&t.map[n].pointer, raw.data() + n * 16 + 8, sizeof(std::uint64_t));
            }
            t.valid = true;
            return true;
        }

        bool UnpackFields(const std::uint8_t* raw, tctrack::ModifierFields& out)
        {
            std::memcpy(&out.index, raw + 0x0, 4);
            std::memcpy(&out.strength, raw + 0x4, 4);
            std::memcpy(&out.transitionIndex, raw + 0x8, 4);
            std::memcpy(&out.progress, raw + 0xC, 4);
            std::memcpy(&out.speed, raw + 0x10, 4);
            out.fadeOut = raw[0x14];
            return true;
        }

        using SetVariationFn = void(__fastcall*)(void* weather, std::uint32_t type, std::int32_t variation, std::uint8_t immediate);
        bool GuardedSetVariation(std::uintptr_t base, std::uint32_t type, std::int32_t variation, std::uint8_t immediate)
        {
            __try
            {
                reinterpret_cast<SetVariationFn>(base + kSetVariationRva)(reinterpret_cast<void*>(base + kWeatherRva), type, variation, immediate);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool TypeCount(std::uint32_t& count)
        {
            if (!mem::Read(WeatherAt(0), count))
                return false;
            if (count > kMaxTypes)
                count = kMaxTypes;
            return true;
        }

        // A type's hash and its active variation key.
        bool TypeState(std::uint32_t type, std::uint32_t& hash, std::uint32_t& active)
        {
            std::uint32_t pair[2] = {};
            if (!mem::Copy(pair, WeatherAt(type * kTypeStride + kTypeHash), sizeof(pair)))
                return false;
            hash = pair[0];
            active = pair[1];
            return true;
        }

        bool VariationKeys(std::uint32_t type, std::vector<std::uint32_t>& keys)
        {
            keys.clear();
            std::uint64_t table = 0;
            std::uint16_t count = 0;
            if (!mem::Read(WeatherAt(type * kTypeStride + kTypeVariationTable), table) ||
                !mem::Read(WeatherAt(type * kTypeStride + kTypeVariationCount), count))
                return false;
            if (count > kMaxTypeVariations)
                return false;
            keys.resize(count);
            for (std::uint16_t n = 0; n < count; ++n)
                if (!mem::Read(static_cast<std::uintptr_t>(table) + n * kVariationStride, keys[n]))
                    return false;
            return true;
        }
    }

    bool Resolve(std::string& error)
    {
        if (!g_checked)
        {
            g_checked = true;
            g_ok = Check(g_error);
        }
        error = g_error;
        return g_ok;
    }

    bool ReadFields(tctrack::ModifierFields& out)
    {
        if (!g_ok)
            return false;
        std::uint8_t raw[kFieldBytes] = {};
        if (!mem::Copy(raw, TimecycleAt(kFields), sizeof(raw)))
            return false;
        return UnpackFields(raw, out);
    }

    bool Read(TimecycleSample& out, bool* overflow)
    {
        if (overflow)
            *overflow = false;
        tctrack::ModifierFields f;
        if (!ReadFields(f) || !RefreshTables())
            return false;
        out = TimecycleSample{};
        out.modifierIndex = f.index < 0 ? -1 : f.index;
        out.transitionIndex = f.transitionIndex < 0 ? -1 : f.transitionIndex;
        out.modifierKey = tctrack::KeyOfIndex(g_tables.pointers, g_tables.map, out.modifierIndex);
        out.transitionKey = tctrack::KeyOfIndex(g_tables.pointers, g_tables.map, out.transitionIndex);
        // An index the tables do not name is recorded as none (its key would
        // mean nothing in another session).
        if (!out.modifierKey)
            out.modifierIndex = -1;
        if (!out.transitionKey)
            out.transitionIndex = -1;
        out.strength = std::isfinite(f.strength) ? f.strength : tctrack::kDefaultStrength;
        out.progress = std::isfinite(f.progress) ? f.progress : 0.0f;
        out.speed = std::isfinite(f.speed) ? f.speed : 0.0f;
        out.fadeOut = f.fadeOut ? 1 : 0;
        std::uint32_t types = 0;
        if (!TypeCount(types))
            return false;
        for (std::uint32_t i = 0; i < types; ++i)
        {
            std::uint32_t hash = 0, active = 0;
            if (!TypeState(i, hash, active))
                return false;
            if (!tctrack::AddVariation(out, hash, active) && overflow)
                *overflow = true;
        }
        return tctrack::Valid(out);
    }

    std::int32_t IndexOf(std::uint32_t key)
    {
        if (!g_ok || !key || !RefreshTables())
            return -1;
        return tctrack::IndexOfKey(g_tables.pointers, g_tables.map, key);
    }

    std::size_t ModifierCount()
    {
        if (!g_ok || !RefreshTables())
            return 0;
        return g_tables.pointers.size();
    }

    bool WriteFields(const tctrack::ModifierFields& f)
    {
        if (!g_ok)
            return false;
        // A written index must be inside the array the engine indexes
        // unchecked (0xF22B60: [T+0x40][index]).
        const std::size_t count = ModifierCount();
        if ((f.index >= 0 && static_cast<std::size_t>(f.index) >= count) ||
            (f.transitionIndex >= 0 && static_cast<std::size_t>(f.transitionIndex) >= count))
            return false;
        std::uint8_t raw[kFieldBytes] = {};
        std::memcpy(raw + 0x0, &f.index, 4);
        std::memcpy(raw + 0x4, &f.strength, 4);
        std::memcpy(raw + 0x8, &f.transitionIndex, 4);
        std::memcpy(raw + 0xC, &f.progress, 4);
        std::memcpy(raw + 0x10, &f.speed, 4);
        raw[0x14] = f.fadeOut;
        return mem::Put(TimecycleAt(kFields), raw, sizeof(raw));
    }

    VariationResult ApplyVariations(const TimecycleSample& wanted)
    {
        VariationResult result;
        if (!g_ok)
            return result;
        std::uint32_t types = 0;
        if (!TypeCount(types))
        {
            ++result.faults;
            return result;
        }
        std::vector<std::uint32_t> keys;
        for (std::uint32_t i = 0; i < types; ++i)
        {
            std::uint32_t hash = 0, active = 0;
            if (!TypeState(i, hash, active))
            {
                ++result.faults;
                continue;
            }
            ++result.types;
            const std::uint32_t want = tctrack::VariationOf(wanted, hash);
            std::int32_t index = -1;
            if (!tctrack::IsDefaultVariation(hash, want) && active != want)
            {
                if (!VariationKeys(i, keys))
                {
                    ++result.faults;
                    continue;
                }
                index = tctrack::VariationIndex(keys, want);
            }
            switch (tctrack::PlanVariation(hash, active, want, index))
            {
            case tctrack::VariationStep::None:
                break;
            case tctrack::VariationStep::Missing:
                ++result.missing;
                break;
            case tctrack::VariationStep::Clear:
                if (GuardedSetVariation(g_base, i, -1, 1))
                    ++result.cleared;
                else
                    ++result.faults;
                break;
            case tctrack::VariationStep::Set:
                if (GuardedSetVariation(g_base, i, index, 0))
                    ++result.set;
                else
                    ++result.faults;
                break;
            }
        }
        return result;
    }
}
