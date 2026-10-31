#include "timecycle_state.h"
#include "engine_calls.h"
#include "game_sig.h"
#include "guarded_memory.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <vector>

// See timecycle_state.h for the code each signature covers. The patterns are
// the bytes the module checked (address bytes masked); the rip-relative and
// call operands it compared are signatures reached from them, and every
// reference to T or W must land on the same object.
namespace replay::tcstate
{
    namespace
    {
        namespace sig = replay::sig;

        // SET_TIMECYCLE_MODIFIER's handler, found by its native hash where the
        // natives are registered (lea rdx, handler; mov rcx, hash): a jump to
        // the setter. SET_TIMECYCLE_MODIFIER_STRENGTH's handler, found the same
        // way: movss [T+0x2774], xmm0.
        const sig::Signature kSetModifierNative{"timecycle_set_modifier_native",
            "48 8D 15 ?? ?? ?? ?? 48 B9 A7 2D A8 5E 2A 72 08 FA E8 ?? ?? ?? ??", 0xFAC630};
        const sig::Signature kSetModifierHandler{"timecycle_set_modifier_handler", kSetModifierNative,
            sig::Rel32{0x03, 0x07}, "48 8B 49 10 48 8B 09 E9 ?? ?? ?? ??", 0xF8F4E8};
        const sig::Signature kStrengthNative{"timecycle_strength_native",
            "48 8D 15 ?? ?? ?? ?? 48 B9 37 3F 4C C5 9C 4C B7 FD E8 ?? ?? ?? ??", 0xFAC657};
        const sig::Signature kStrengthHandler{"timecycle_strength_handler", kStrengthNative, sig::Rel32{0x03, 0x07},
            "48 8B 41 10 F3 0F 10 00 F3 0F 11 05 ?? ?? ?? ?? C3", 0xF8F4F4};
        const sig::Signature kHandlerStrength{"timecycle_handler_strength", kStrengthHandler, sig::Rel32{0x0C, 0x10}, "",
            0x4A995F4};
        // The setter: lea rcx, [T]; the index of the name's hash; the transition
        // -1 (T+0x2778) and the index (T+0x2770).
        const sig::Signature kSetModifier{"timecycle_set_modifier", kSetModifierHandler, sig::Rel32{0x08, 0x0C},
            "40 53 48 83 EC 20 48 8B D1 33 C9 E8 ?? ?? ?? ?? 45 33 C9 89 44 24 38 4C 8D 05 ?? ?? ?? ?? 48 8D 54 24 38 "
            "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 0D ?? ?? ?? ?? FF 8B 1D ?? ?? ?? ?? 89 05 ?? ?? ?? ??",
            0xFA2758};
        const sig::Signature kTimecycle{"timecycle_object", kSetModifier, sig::Rel32{0x26, 0x2A}, "", 0x4A96E80}; // T
        const sig::Signature kSetModifierTransition{"timecycle_set_modifier_transition", kSetModifier,
            sig::Rel32{0x31, 0x36}, "", 0x4A995F8};
        const sig::Signature kSetModifierIndex{"timecycle_set_modifier_index", kSetModifier, sig::Rel32{0x3E, 0x42},
            "", 0x4A995F0};
        // The index of a hash: the alias (T+0x18, count T+0x38), then the map at
        // T+0x50 through the sorted map's find.
        const sig::Signature kModifierIndex{"timecycle_modifier_index", kSetModifier, sig::Rel32{0x2B, 0x2F},
            "40 53 48 83 EC 20 8B 02 48 8B D9 48 8D 54 24 30 89 44 24 30 E8 ?? ?? ?? ?? 44 0F B7 4B 48 33 D2 66 41 3B "
            "D1 73 16 4C 8B 43 40 0F B7 CA 49 39 04 C8 74 12 66 FF C2 66 41 3B D1 72 EE 83 C8 FF 48 83 C4 20 5B C3 0F "
            "B7 C2 EB F5",
            0x2B2ED78};
        const sig::Signature kModifierAlias{"timecycle_modifier_alias", kModifierIndex, sig::Rel32{0x15, 0x19},
            "40 53 48 83 EC 20 44 8B 02 33 DB 45 85 C0 75 04 33 C0 EB 41 44 89 44 24 38 39 59 38 74 1E 8B C3 7E 1A 8B "
            "D0 44 39 44 D1 18 74 09 FF C0 3B 41 38 7C F0 EB 08 8B 44 D1 1C 89 44 24 38 48 83 C1 50 48 8D 54 24 38 E8 "
            "?? ?? ?? ?? 48 85 C0 74 03 48 8B 18 48 8B C3 48 83 C4 20 5B C3",
            0x2B2ED1C};
        const sig::Signature kSortedMapFind{"timecycle_sorted_map_find", kModifierAlias, sig::Rel32{0x46, 0x4A},
            "48 89 5C 24 08 48 89 7C 24 10 33 FF 4C 8B D2 66 39 79 10 76 5D 4C 8B 59 08 0F B7 49 10 4D 8B C3 44 8B C9 "
            "48 85 C9 74 30 8B 1A 49 8B D1 48 D1 FA 48 8B C2 48 03 C0 41 39 1C C0 73 14 4D 8D 04 C0 48 83 C8 FF 48 2B "
            "C2 49 83 C0 10 4C 03 C8 EB 03 4C 8B CA 4D 85 C9 7F D2 48 C1 E1 04 49 03 CB 4C 3B C1 74 0E 41 8B 02 41 39 "
            "00 75 06 49 8D 40 08 EB 02 33 C0 48 8B 5C 24 08 48 8B 7C 24 10 C3",
            0x450210};
        // The per frame update of the fields (rbx is T): +0x2780 * frame time into
        // +0x277C, the end at +0x2774, the index +0x2770, the fade byte +0x2784.
        const sig::Signature kModifierUpdate{"timecycle_modifier_update",
            "F3 0F 10 05 ?? ?? ?? ?? F3 0F 59 83 80 27 00 00 F3 0F 58 83 7C 27 00 00 F3 0F 11 83 7C 27 00 00 EB 08 F3 "
            "0F 10 83 7C 27 00 00 F3 0F 10 8B 74 27 00 00 0F 2F C1 72 12 83 8B 78 27 00 00 FF 89 83 70 27 00 00 E9 ?? "
            "?? ?? ?? 8B 8B 70 27 00 00 85 C9 78 24 8B C1 F3 0F 5C C8 48 8B 4B 40 48 8B 0C C1 0F 28 D1 E8 ?? ?? ?? ?? "
            "F3 0F 10 83 7C 27 00 00 8B 83 78 27 00 00 8B C8 0F 28 C8 E9 ?? ?? ?? ?? 8B 83 70 27 00 00 85 C0 0F 88 ?? "
            "?? ?? ?? 8B C8 48 8B 43 40 48 8B 0C C8 40 38 B3 84 27 00 00",
            0xF22BC8};
        // CLEAR_TIMECYCLE_MODIFIER, found by its native hash where the natives are
        // registered (lea rdx, handler; mov rcx, hash): its handler jumps to the
        // clear, which writes the 1.0 of the .rdata as the strength and -1 to the
        // index and the transition, and 0 to the fade byte.
        const sig::Signature kClearNative{"timecycle_clear_native",
            "48 8D 15 ?? ?? ?? ?? 48 B9 FB 91 34 D6 F2 4A 3F 0E E8 ?? ?? ?? ??", 0xFAC6CC};
        const sig::Signature kClearHandler{"timecycle_clear_handler", kClearNative, sig::Rel32{0x03, 0x07},
            "E9 ?? ?? ?? ??", 0xF8AC84};
        const sig::Signature kClearModifier{"timecycle_clear_modifier", kClearHandler, sig::Rel32{0x01, 0x05},
            "48 89 5C 24 08 57 48 83 EC 20 F3 0F 10 05 ?? ?? ?? ?? 83 C8 FF 8B 3D ?? ?? ?? ?? 33 DB F3 0F 11 05 ?? ?? "
            "?? ?? 89 05 ?? ?? ?? ?? 89 05 ?? ?? ?? ?? 88 1D ?? ?? ?? ??",
            0xF93770};
        const sig::Signature kUnit{"timecycle_unit", kClearModifier, sig::Rel32{0x0E, 0x12}, "", 0x330C7A8};
        const sig::Signature kClearStrength{"timecycle_clear_strength", kClearModifier, sig::Rel32{0x21, 0x25}, "",
            0x4A995F4};
        const sig::Signature kClearIndex{"timecycle_clear_index", kClearModifier, sig::Rel32{0x27, 0x2B}, "",
            0x4A995F0};
        const sig::Signature kClearTransition{"timecycle_clear_transition", kClearModifier, sig::Rel32{0x2D, 0x31}, "",
            0x4A995F8};
        const sig::Signature kClearFade{"timecycle_clear_fade", kClearModifier, sig::Rel32{0x33, 0x37}, "", 0x4A99604};
        // _SET_WEATHER_VARIATION, found by its native hash: the handler jumps to
        // the implementation, which uses lea rcx, [W] four times: the type by
        // hash, the variation by name (then by key), and the setter (W, type,
        // variation, 0).
        const sig::Signature kVariationNative{"timecycle_variation_native",
            "48 8D 15 ?? ?? ?? ?? 48 B9 48 AF 7C AF 9B 77 73 33 E8 ?? ?? ?? ??", 0x1062C2A};
        const sig::Signature kSetVariationHandler{"timecycle_set_variation_handler", kVariationNative,
            sig::Rel32{0x03, 0x07}, "48 8B 49 10 48 8B 51 08 48 8B 09 E9 ?? ?? ?? ??", 0x1047A2C};
        const sig::Signature kSetVariationImpl{"timecycle_set_variation_impl", kSetVariationHandler,
            sig::Rel32{0x0C, 0x10},
            "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B D1 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 4C 8B C3 48 8D 0D ?? "
            "?? ?? ?? 8B D0 8B F8 E8 ?? ?? ?? ?? 85 C0 78 20 85 FF 78 1C 3B 3D ?? ?? ?? ?? 73 14 45 33 C9 48 8D 0D ?? "
            "?? ?? ?? 44 8B C0 8B D7 E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 83 C4 20 5F C3",
            0x105945C};
        const sig::Signature kWeather{"timecycle_weather", kSetVariationImpl, sig::Rel32{0x13, 0x17}, "",
            0x45722E0}; // W
        const sig::Signature kTypeByName{"timecycle_type_by_name", kSetVariationImpl, sig::Rel32{0x18, 0x1C},
            "40 53 48 83 EC 20 48 8B D9 33 C9 E8 ?? ?? ?? ?? 44 8B 03 33 D2 45 85 C0 74 16 48 8D 4B 10 39 01 74 17 FF "
            "C2 48 81 C1 E0 01 00 00 41 3B D0 72 EE 83 C8 FF 48 83 C4 20 5B C3 8B C2 EB F6",
            0x739918};
        const sig::Signature kWeatherByName{"timecycle_weather_by_name", kSetVariationImpl, sig::Rel32{0x22, 0x26}, "",
            0x45722E0};
        const sig::Signature kVariationByName{"timecycle_variation_by_name", kSetVariationImpl, sig::Rel32{0x2B, 0x2F},
            "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 49 8B D0 33 C9 E8 ?? ?? ?? ?? 44 8B C0 8B D3 48 8B CF 48 8B "
            "5C 24 30 48 83 C4 20 5F E9 ?? ?? ?? ??",
            0x739DF0};
        const sig::Signature kWeatherCount{"timecycle_weather_count", kSetVariationImpl, sig::Rel32{0x39, 0x3D}, "",
            0x45722E0};
        const sig::Signature kWeatherSet{"timecycle_weather_set", kSetVariationImpl, sig::Rel32{0x45, 0x49}, "",
            0x45722E0};
        const sig::Signature kSetVariation{"timecycle_set_variation", kSetVariationImpl, sig::Rel32{0x4F, 0x53},
            "40 53 4C 8B D1 44 8B DA 33 C9 45 85 C0 79 0F 45 8A C1 41 8B D3 49 8B CA 5B E9 ?? ?? ?? ?? 49 69 D3 E0 01 "
            "00 00 49 8B C3 42 0F B7 84 12 D0 01 00 00 66 85 C0 74 66 0F B7 C0 44 3B C0 7D 5E 49 63 9A B8 3E 00 00 85 "
            "DB 7E 17 48 8B C1 45 39 9C 82 38 3E 00 00 74 0D FF C1 48 FF C0 48 3B C3 7C EC 83 C9 FF 83 F9 FF 74 1D 8D "
            "43 FF 48 63 C9 41 89 82 B8 3E 00 00 41 8B 84 9A 34 3E 00 00 41 89 84 8A 38 3E 00 00 41 8B C0 48 6B C8 44 "
            "4A 8B 84 12 C8 01 00 00 8B 0C 01 42 89 4C 12 14 5B C3",
            0x757BEC};
        const sig::Signature kVariationByKey{"timecycle_variation_by_key", kVariationByName, sig::Rel32{0x2C, 0x30},
            "8B C2 33 D2 4C 69 C8 E0 01 00 00 45 0F B7 94 09 D0 01 00 00 45 85 D2 74 1B 4D 8B 8C 09 C8 01 00 00 8B C2 "
            "48 6B C8 44 46 39 04 09 74 0B FF C2 41 3B D2 72 ED 83 C8 FF C3",
            0x739DB4};
        const sig::Signature kClearVariation{"timecycle_clear_variation", kSetVariation, sig::Rel32{0x1A, 0x1E},
            "33 C0 44 8B CA 45 84 C0 74 10 49 69 D1 E0 01 00 00 8B 44 0A 10 89 44 0A 14 C3 4C 63 91 B8 3E 00 00 45 85 "
            "D2 7E 17 48 8B D0 44 39 8C 91 38 3E 00 00 74 0D FF C0 48 FF C2 49 3B D2 7C EC 83 C8 FF 83 F8 FF 75 12 41 "
            "8D 42 01 89 81 B8 3E 00 00 46 89 8C 91 38 3E 00 00 C3",
            0x72C2E8};

        // T's fields (timecycle_track.h).
        constexpr std::uintptr_t kModifierPointers = 0x40; // u64 array
        constexpr std::uintptr_t kModifierCount = 0x48;    // u16
        constexpr std::uintptr_t kMapData = 0x58;          // map at 0x50: data at +0x8
        constexpr std::uintptr_t kMapCount = 0x60;         // u16 at map +0x10
        constexpr std::uintptr_t kFields = 0x2770;         // index, strength, transition, progress, speed, fade byte
        constexpr std::uintptr_t kIndexField = 0x2770;
        constexpr std::uintptr_t kStrengthField = 0x2774;
        constexpr std::uintptr_t kTransitionField = 0x2778;
        constexpr std::uintptr_t kFadeField = 0x2784;
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
        std::uintptr_t g_timecycle = 0;    // T
        std::uintptr_t g_weather = 0;      // W
        std::uintptr_t g_setVariation = 0; // the engine's setter

        bool Found(std::initializer_list<const sig::Signature*> list)
        {
            for (const sig::Signature* s : list)
                if (!sig::Find(*s))
                    return false;
            return true;
        }

        // A reference that must land on `address` (T or W plus a field): the
        // code agrees on the global it uses.
        bool At(const sig::Signature& s, std::uintptr_t address)
        {
            return address && sig::Find(s) == address;
        }

        bool Check(std::string& error)
        {
            const std::uintptr_t t = sig::Find(kTimecycle);
            const std::uintptr_t w = sig::Find(kWeather);
            if (!Found({&kSetModifierHandler, &kHandlerStrength}))
                error = "set_modifier_handler_mismatch";
            else if (!t || !At(kHandlerStrength, t + kStrengthField) ||
                     !At(kSetModifierTransition, t + kTransitionField) || !At(kSetModifierIndex, t + kIndexField))
                error = "set_modifier_mismatch";
            else if (!Found({&kModifierIndex}))
                error = "modifier_index_mismatch";
            else if (!Found({&kModifierAlias}))
                error = "modifier_alias_mismatch";
            else if (!Found({&kSortedMapFind}))
                error = "sorted_map_mismatch";
            else if (!Found({&kModifierUpdate}))
                error = "modifier_update_mismatch";
            else if (!Found({&kClearNative, &kClearHandler}))
                error = "clear_handler_mismatch";
            else if (!Found({&kClearModifier, &kUnit}) || !At(kClearStrength, t + kStrengthField) ||
                     !At(kClearIndex, t + kIndexField) || !At(kClearTransition, t + kTransitionField) ||
                     !At(kClearFade, t + kFadeField))
                error = "clear_modifier_mismatch";
            else if (!Found({&kVariationNative, &kSetVariationHandler}))
                error = "set_variation_handler_mismatch";
            else if (!w || !At(kWeatherByName, w) || !At(kWeatherCount, w) || !At(kWeatherSet, w))
                error = "set_variation_impl_mismatch";
            else if (!Found({&kTypeByName}))
                error = "type_by_name_mismatch";
            else if (!Found({&kVariationByName}))
                error = "variation_by_name_mismatch";
            else if (!Found({&kVariationByKey}))
                error = "variation_by_key_mismatch";
            else if (!Found({&kSetVariation}))
                error = "set_variation_mismatch";
            else if (!Found({&kClearVariation}))
                error = "clear_variation_mismatch";
            else
            {
                g_timecycle = t;
                g_weather = w;
                g_setVariation = sig::Find(kSetVariation);
                return true;
            }
            return false;
        }

        std::uintptr_t TimecycleAt(std::uintptr_t offset) { return g_timecycle + offset; }
        std::uintptr_t WeatherAt(std::uintptr_t offset) { return g_weather + offset; }

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

        bool GuardedSetVariation(std::uintptr_t setter, std::uintptr_t weather, std::uint32_t type,
            std::int32_t variation, std::uint8_t immediate)
        {
            return engine::Call(engine::kTimecycleSetVariation, setter, reinterpret_cast<void*>(weather), type, variation,
                immediate);
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
        // unchecked (the per frame update: [T+0x40][index]).
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
                if (GuardedSetVariation(g_setVariation, g_weather, i, -1, 1))
                    ++result.cleared;
                else
                    ++result.faults;
                break;
            case tctrack::VariationStep::Set:
                if (GuardedSetVariation(g_setVariation, g_weather, i, index, 0))
                    ++result.set;
                else
                    ++result.faults;
                break;
            }
        }
        return result;
    }
}
