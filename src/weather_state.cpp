#include "weather_state.h"
#include "game_sig.h"
#include "guarded_memory.h"

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <initializer_list>

namespace replay::weatherstate
{
    namespace
    {
        namespace sig = replay::sig;

        constexpr std::uintptr_t kTypesOffset = 0x10;     // type i at W + 0x10 + i*0x1E0, hash first
        constexpr std::uintptr_t kTypeStride = 0x1E0;
        constexpr std::uintptr_t kPreviousOffset = 0x3F24;
        constexpr std::uintptr_t kTargetOffset = 0x3F38;
        constexpr std::uintptr_t kTotalOffset = 0x3F3C;
        constexpr std::uintptr_t kLeftOffset = 0x3F40;
        constexpr std::uintptr_t kProgressOffset = 0x3F44;
        constexpr std::uintptr_t kRainOffset = 0x4160;
        constexpr std::uintptr_t kSnowOffset = 0x4164;
        constexpr std::uintptr_t kFlagOffset = 0x4170;
        constexpr std::uintptr_t kWindSpeedOffset = 0x60;
        constexpr std::uintptr_t kWindSpeedSetOffset = 0x68;
        constexpr std::uintptr_t kWindDirectionOffset = 0x70;
        constexpr std::uintptr_t kWindDirectionSetOffset = 0x80;
        // The table holds the game's weather types (22 in the list of
        // env_override.h); a count past this is not the table.
        constexpr std::uint32_t kMaxTypes = 64;

        // GET_CURR_WEATHER_STATE handler: mov eax, [W+0x3F24] (the previous
        // index); lea r10, [W]; the stride 0x1E0 and the hash at +0x10.
        const sig::Signature kCurrState{"weather_curr_state",
            "8B 05 ?? ?? ?? ?? 4C 8D 15 ?? ?? ?? ?? 48 8B 49 10 48 69 D0 E0 01 00 00 4C 8B 41 08 4C 8B 49 10 48 8B 09 "
            "42 8B 44 12 10",
            0x1042D0C};
        const sig::Signature kManager{"weather_manager", kCurrState, sig::Rel32{0x09, 0x0D}, "", 0x45722E0}; // W
        const sig::Signature kCurrPrevious{"weather_curr_previous", kCurrState, sig::Rel32{0x02, 0x06}, "", 0x4576204};
        // The transition setter: cmp edx, [W] (the type count); time * the
        // 1000.0 of the .rdata; then the five fields, as WriteTransition writes them.
        const sig::Signature kSetter{"weather_transition_setter",
            "83 FA FF 74 08 3B 15 ?? ?? ?? ?? 73 2C F3 0F 59 15 ?? ?? ?? ?? 83 A1 44 3F 00 00 00 89 91 38 3F 00 00 44 "
            "88 89 70 41 00 00 F3 0F 11 91 3C 3F 00 00 F3 0F 11 91 40 3F 00 00 C3",
            0x7340BC};
        const sig::Signature kSetterCount{"weather_setter_count", kSetter, sig::Rel32{0x07, 0x0B}, "", 0x45722E0};
        const sig::Signature kMsPerSecond{"weather_ms_per_second", kSetter, sig::Rel32{0x11, 0x15}, "", 0x330F44C};
        // The per-frame update: at progress 1 it calls the finalizer with
        // lea rcx, [W] and clears the target (or [W+0x3F38], -1).
        const sig::Signature kUpdate{"weather_transition_update",
            "40 53 48 83 EC 20 8A C2 32 DB 8B 91 38 3F 00 00 83 FA FF 0F 84 ?? ?? ?? ?? F3 0F 10 0D ?? ?? ?? ?? 0F 57 "
            "DB F3 0F 59 0D ?? ?? ?? ?? F3 0F 10 81 40 3F 00 00 F3 0F 5C C1 F3 0F 10 0D ?? ?? ?? ?? F3 0F 11 81 40 3F "
            "00 00 F3 0F 10 15 ?? ?? ?? ?? F3 0F 5E 15 ?? ?? ?? ?? 0F 2F D3 72 07 F3 0F 5D D1 0F 28 DA 0F 28 C1 F3 0F "
            "5C C3 0F 2E C1 F3 0F 11 81 44 3F 00 00 75 47 3A C3 74 41 8A 81 70 41 00 00 48 8D 0D ?? ?? ?? ?? F6 D8 45 "
            "1B C0 41 83 E0 04 41 83 C0 02 E8 ?? ?? ?? ?? 83 25 ?? ?? ?? ?? 00 83 0D ?? ?? ?? ?? FF",
            0x75FFEC};
        const sig::Signature kUpdateManager{"weather_update_manager", kUpdate, sig::Rel32{0x85, 0x89}, "", 0x45722E0};
        const sig::Signature kFinalize{"weather_finalize", kUpdate, sig::Rel32{0x97, 0x9B},
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 20 3B 15 ?? ?? ?? ??", 0x733F34};
        const sig::Signature kUpdateTarget{"weather_update_target", kUpdate, sig::Rel32{0xA4, 0xA9}, "", 0x4576218};
        // SET_RAIN and _SET_SNOW_LEVEL handlers (movss [W+0x4160] and
        // [W+0x4164], xmm0), found by their native hashes where the natives
        // are registered (lea rdx, handler; mov rcx, hash), and the level
        // readers: the override counts when it is >= the 0.0 of the .rdata.
        const sig::Signature kRainNative{"weather_rain_native",
            "48 8D 15 ?? ?? ?? ?? 48 B9 D6 0F 83 26 05 FC 3D 19 E8 ?? ?? ?? ??", 0x1062D89};
        const sig::Signature kRainHandler{"weather_rain_handler", kRainNative, sig::Rel32{0x03, 0x07},
            "48 8B 41 10 F3 0F 10 00 F3 0F 11 05 ?? ?? ?? ?? C3", 0x10477E8};
        const sig::Signature kRainLevel{"weather_rain_level", kRainHandler, sig::Rel32{0x0C, 0x10}, "", 0x4576440};
        const sig::Signature kSnowNative{"weather_snow_native",
            "48 8D 15 ?? ?? ?? ?? 48 B9 40 CA C5 0E E8 E7 BE F6 E8 ?? ?? ?? ??", 0x1062DFE};
        const sig::Signature kSnowHandler{"weather_snow_handler", kSnowNative, sig::Rel32{0x03, 0x07},
            "48 8B 41 10 F3 0F 10 00 F3 0F 11 05 ?? ?? ?? ?? C3", 0x1047868};
        const sig::Signature kSnowLevel{"weather_snow_level", kSnowHandler, sig::Rel32{0x0C, 0x10}, "", 0x4576444};
        const sig::Signature kRainReader{"weather_rain_reader",
            "48 89 5C 24 08 57 48 83 EC 30 F3 0F 10 81 60 41 00 00 48 8B D9 0F 2F 05 ?? ?? ?? ??", 0x760EAC};
        const sig::Signature kZero{"weather_zero", kRainReader, sig::Rel32{0x18, 0x1C}, "", 0x330F380};
        const sig::Signature kSnowReader{"weather_snow_reader",
            "48 83 EC 38 F3 0F 10 81 64 41 00 00 4C 8B C9 0F 2F 05 ?? ?? ?? ??", 0x762C30};
        const sig::Signature kSnowZero{"weather_snow_zero", kSnowReader, sig::Rel32{0x12, 0x16}, "", 0x330F380};
        // SET_WIND_SPEED: below 0.0 it clears the flag byte (mov [V+0x68], 0),
        // else lea rcx, [V] and a jump to the setter (speed at V+0x60, flag V+0x68).
        const sig::Signature kSpeedHandler{"weather_wind_speed_handler",
            "0F 2F 05 ?? ?? ?? ?? 72 12 45 33 C0 48 8D 0D ?? ?? ?? ?? 0F 28 C8 E9 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 00",
            0x1059500};
        const sig::Signature kSpeedZero{"weather_wind_speed_zero", kSpeedHandler, sig::Rel32{0x03, 0x07}, "",
            0x330F380};
        const sig::Signature kWind{"weather_wind", kSpeedHandler, sig::Rel32{0x0F, 0x13}, "", 0x457CC70}; // V
        const sig::Signature kSpeedSetter{"weather_wind_speed_setter", kSpeedHandler, sig::Rel32{0x17, 0x1B},
            "41 8B C0 F3 0F 11 4C 81 60 C6 44 08 68 01 C3", 0x75676C};
        const sig::Signature kSpeedSet{"weather_wind_speed_set", kSpeedHandler, sig::Rel32{0x1D, 0x22}, "", 0x457CCD8};
        // SET_WIND_DIRECTION: below 0.0 it clears [V+0x80]; else lea rcx, [V],
        // the vector of the degrees (* pi/180 of the .rdata) stored at V+0x70
        // and the flag byte V+0x80.
        const sig::Signature kDirectionHandler{"weather_wind_direction_handler",
            "48 83 EC 38 0F 2F 05 ?? ?? ?? ?? 72 28 0F 28 D0 48 8D 54 24 20 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F 10 "
            "00 F3 0F 7F 05 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 01",
            0x10594BC};
        const sig::Signature kDirectionZero{"weather_wind_direction_zero", kDirectionHandler, sig::Rel32{0x07, 0x0B},
            "", 0x330F380};
        const sig::Signature kDirectionWind{"weather_wind_direction_wind", kDirectionHandler, sig::Rel32{0x18, 0x1C},
            "", 0x457CC70};
        const sig::Signature kWindVector{"weather_wind_vector", kDirectionHandler, sig::Rel32{0x1D, 0x21},
            "40 53 48 83 EC 20 F3 0F 59 15 ?? ?? ?? ??", 0x73A01C};
        const sig::Signature kDegToRad{"weather_deg_to_rad", kWindVector, sig::Rel32{0x0A, 0x0E}, "", 0x3311368};
        const sig::Signature kDirectionValue{"weather_wind_direction_value", kDirectionHandler, sig::Rel32{0x28, 0x2C},
            "", 0x457CCE0};
        const sig::Signature kDirectionSet{"weather_wind_direction_set", kDirectionHandler, sig::Rel32{0x2E, 0x33}, "",
            0x457CCF0};

        bool g_checked = false;
        bool g_ok = false;
        std::string g_error;
        std::uintptr_t g_manager = 0; // W
        std::uintptr_t g_wind = 0;    // V

        bool Found(std::initializer_list<const sig::Signature*> list)
        {
            for (const sig::Signature* s : list)
                if (!sig::Find(*s))
                    return false;
            return true;
        }

        // A reference that must land on `address` (W or V plus a field): the
        // code agrees on the global it uses.
        bool At(const sig::Signature& s, std::uintptr_t address)
        {
            return address && sig::Find(s) == address;
        }

        bool Constant(const sig::Signature& s, float wanted)
        {
            float value = 0.0f;
            const std::uintptr_t address = sig::Find(s);
            return address && mem::Read(address, value) &&
                std::fabs(value - wanted) <= 1.0e-6f * (1.0f + std::fabs(wanted));
        }

        bool Check(std::string& error)
        {
            const std::uintptr_t w = sig::Find(kManager);
            const std::uintptr_t v = sig::Find(kWind);
            const std::uintptr_t zero = sig::Find(kZero);
            if (!w || !At(kCurrPrevious, w + kPreviousOffset))
                error = "curr_state_mismatch";
            else if (!At(kSetterCount, w) || !Constant(kMsPerSecond, 1000.0f))
                error = "transition_setter_mismatch";
            else if (!At(kUpdateManager, w) || !Found({&kFinalize}) || !At(kUpdateTarget, w + kTargetOffset))
                error = "transition_update_mismatch";
            else if (!At(kRainLevel, w + kRainOffset) || !At(kSnowLevel, w + kSnowOffset) || !At(kSnowZero, zero) ||
                     !Constant(kZero, 0.0f))
                error = "rain_snow_mismatch";
            else if (!v || !At(kSpeedZero, zero) || !Found({&kSpeedSetter}) || !At(kSpeedSet, v + kWindSpeedSetOffset))
                error = "wind_speed_mismatch";
            else if (!At(kDirectionZero, zero) || !At(kDirectionWind, v) || !Found({&kWindVector}) ||
                     !At(kDirectionValue, v + kWindDirectionOffset) || !At(kDirectionSet, v + kWindDirectionSetOffset) ||
                     !Constant(kDegToRad, 3.14159265f / 180.0f))
                error = "wind_direction_mismatch";
            else
            {
                g_manager = w;
                g_wind = v;
                return true;
            }
            return false;
        }

        std::uintptr_t Manager(std::uintptr_t offset) { return g_manager + offset; }
        std::uintptr_t Wind(std::uintptr_t offset) { return g_wind + offset; }

        bool TypeCount(std::uint32_t& count)
        {
            return mem::Read(Manager(0), count) && count > 0 && count <= kMaxTypes;
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

    bool Ready() { return g_ok; }

    bool ReadTransition(Transition& out)
    {
        if (!g_ok)
            return false;
        std::uint32_t count = 0;
        if (!TypeCount(count) || !mem::Read(Manager(kTargetOffset), out.index) || !mem::Read(Manager(kTotalOffset), out.totalMs) ||
            !mem::Read(Manager(kLeftOffset), out.leftMs) || !mem::Read(Manager(kProgressOffset), out.progress) ||
            !mem::Read(Manager(kFlagOffset), out.flag))
            return false;
        out.hash = 0;
        if (out.index == -1)
            return true;
        if (out.index < 0 || static_cast<std::uint32_t>(out.index) >= count)
            return false;
        return mem::Read(Manager(kTypesOffset + static_cast<std::uintptr_t>(out.index) * kTypeStride), out.hash);
    }

    bool WriteTransition(const Transition& in)
    {
        if (!g_ok)
            return false;
        std::uint32_t count = 0;
        if (!TypeCount(count) || in.index < -1 || (in.index >= 0 && static_cast<std::uint32_t>(in.index) >= count))
            return false;
        if (!std::isfinite(in.totalMs) || !std::isfinite(in.leftMs) || !std::isfinite(in.progress))
            return false;
        // The target index is the field the
        // update tests first (kUpdate: -1 returns at once), so it goes last
        // when a transition is written and first when it is cleared. Should
        // the update ever run between two of these writes (the thread is a
        // hypothesis, see the header), it never sees a target with the
        // previous total 0 (0/0 is NaN, the comiss falls through to progress
        // 1 and the engine would finalize: the dry switch).
        if (in.index == -1)
            return mem::Put(Manager(kTargetOffset), &in.index, sizeof(in.index)) &&
                mem::Put(Manager(kProgressOffset), &in.progress, sizeof(in.progress)) &&
                mem::Put(Manager(kFlagOffset), &in.flag, sizeof(in.flag)) &&
                mem::Put(Manager(kTotalOffset), &in.totalMs, sizeof(in.totalMs)) &&
                mem::Put(Manager(kLeftOffset), &in.leftMs, sizeof(in.leftMs));
        return mem::Put(Manager(kTotalOffset), &in.totalMs, sizeof(in.totalMs)) &&
            mem::Put(Manager(kLeftOffset), &in.leftMs, sizeof(in.leftMs)) &&
            mem::Put(Manager(kProgressOffset), &in.progress, sizeof(in.progress)) &&
            mem::Put(Manager(kFlagOffset), &in.flag, sizeof(in.flag)) &&
            mem::Put(Manager(kTargetOffset), &in.index, sizeof(in.index));
    }

    std::int32_t IndexOf(std::uint32_t hash)
    {
        std::uint32_t count = 0;
        if (!g_ok || !hash || !TypeCount(count))
            return -1;
        for (std::uint32_t n = 0; n < count; ++n)
        {
            std::uint32_t typeHash = 0;
            if (!mem::Read(Manager(kTypesOffset + static_cast<std::uintptr_t>(n) * kTypeStride), typeHash))
                return -1;
            if (typeHash == hash)
                return static_cast<std::int32_t>(n);
        }
        return -1;
    }

    bool ReadOverrides(Overrides& out)
    {
        if (!g_ok)
            return false;
        std::uint8_t speedSet = 0, directionSet = 0;
        float direction[2] = {};
        if (!mem::Read(Manager(kRainOffset), out.rain) || !mem::Read(Manager(kSnowOffset), out.snow) ||
            !mem::Read(Wind(kWindSpeedOffset), out.windSpeed) || !mem::Read(Wind(kWindSpeedSetOffset), speedSet) ||
            !mem::Copy(direction, Wind(kWindDirectionOffset), sizeof(direction)) || !mem::Read(Wind(kWindDirectionSetOffset), directionSet))
            return false;
        out.windSpeedSet = speedSet != 0;
        out.windDirectionSet = directionSet != 0;
        out.windX = direction[0];
        out.windY = direction[1];
        return std::isfinite(out.rain) && std::isfinite(out.snow) && std::isfinite(out.windSpeed) && std::isfinite(out.windX) &&
            std::isfinite(out.windY);
    }
}
