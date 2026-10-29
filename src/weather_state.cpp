#include "weather_state.h"
#include "guarded_memory.h"

#include <windows.h>

#include <cmath>
#include <cstddef>

namespace replay::weatherstate
{
    namespace
    {
        constexpr std::uintptr_t kManagerRva = 0x45722E0; // W
        constexpr std::uintptr_t kWindRva = 0x457CC70;    // V
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

        constexpr std::uintptr_t kMsPerSecondRva = 0x330F44C; // .rdata 1000.0f
        constexpr std::uintptr_t kZeroRva = 0x330F380;        // .rdata 0.0f
        constexpr std::uintptr_t kDegToRadRva = 0x3311368;    // .rdata pi/180
        constexpr std::uintptr_t kFinalizeRva = 0x733F34;
        constexpr std::uintptr_t kWindSpeedSetterRva = 0x75676C;
        constexpr std::uintptr_t kWindVectorRva = 0x73A01C;

        // GET_CURR_WEATHER_STATE handler.
        constexpr std::uint8_t kCurrMov[] = {0x8B, 0x05};
        constexpr std::uint8_t kCurrLea[] = {0x4C, 0x8D, 0x15};
        constexpr std::uint8_t kCurrStride[] = {0x48, 0x8B, 0x49, 0x10, 0x48, 0x69, 0xD0, 0xE0, 0x01, 0x00, 0x00};
        constexpr std::uint8_t kCurrHash[] = {0x42, 0x8B, 0x44, 0x12, 0x10};
        // The transition setter.
        constexpr std::uint8_t kSetHead[] = {0x83, 0xFA, 0xFF, 0x74, 0x08, 0x3B, 0x15};
        constexpr std::uint8_t kSetScale[] = {0x73, 0x2C, 0xF3, 0x0F, 0x59, 0x15};
        constexpr std::uint8_t kSetFields[] = {0x83, 0xA1, 0x44, 0x3F, 0x00, 0x00, 0x00, 0x89, 0x91, 0x38, 0x3F, 0x00, 0x00,
            0x44, 0x88, 0x89, 0x70, 0x41, 0x00, 0x00, 0xF3, 0x0F, 0x11, 0x91, 0x3C, 0x3F, 0x00, 0x00, 0xF3, 0x0F, 0x11, 0x91,
            0x40, 0x3F, 0x00, 0x00, 0xC3};
        // The per-frame update.
        constexpr std::uint8_t kUpdateHead[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8A, 0xC2, 0x32, 0xDB, 0x8B, 0x91, 0x38, 0x3F, 0x00, 0x00, 0x83, 0xFA, 0xFF};
        constexpr std::uint8_t kUpdateLeft[] = {0xF3, 0x0F, 0x10, 0x81, 0x40, 0x3F, 0x00, 0x00};
        constexpr std::uint8_t kUpdateFinal[] = {0x8A, 0x81, 0x70, 0x41, 0x00, 0x00, 0x48, 0x8D, 0x0D};
        constexpr std::uint8_t kUpdateCall[] = {
            0xF6, 0xD8, 0x45, 0x1B, 0xC0, 0x41, 0x83, 0xE0, 0x04, 0x41, 0x83, 0xC0, 0x02, 0xE8};
        constexpr std::uint8_t kUpdateClear[] = {0x83, 0x0D};
        // SET_RAIN and _SET_SNOW_LEVEL handlers, and the level readers.
        constexpr std::uint8_t kLevelHandler[] = {0x48, 0x8B, 0x41, 0x10, 0xF3, 0x0F, 0x10, 0x00, 0xF3, 0x0F, 0x11, 0x05};
        constexpr std::uint8_t kRainReader[] = {
            0xF3, 0x0F, 0x10, 0x81, 0x60, 0x41, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0x0F, 0x2F, 0x05};
        constexpr std::uint8_t kSnowReader[] = {0x0F, 0x10, 0x81, 0x64, 0x41, 0x00, 0x00, 0x4C, 0x8B, 0xC9, 0x0F, 0x2F, 0x05};
        // SET_WIND_SPEED.
        constexpr std::uint8_t kComiss[] = {0x0F, 0x2F, 0x05};
        constexpr std::uint8_t kSpeedLea[] = {0x72, 0x12, 0x45, 0x33, 0xC0, 0x48, 0x8D, 0x0D};
        constexpr std::uint8_t kSpeedJmp[] = {0x0F, 0x28, 0xC8, 0xE9};
        constexpr std::uint8_t kMovByte[] = {0xC6, 0x05};
        constexpr std::uint8_t kSpeedSetter[] = {
            0x41, 0x8B, 0xC0, 0xF3, 0x0F, 0x11, 0x4C, 0x81, 0x60, 0xC6, 0x44, 0x08, 0x68, 0x01, 0xC3};
        // SET_WIND_DIRECTION.
        constexpr std::uint8_t kDirectionHead[] = {0x48, 0x83, 0xEC, 0x38, 0x0F, 0x2F, 0x05};
        constexpr std::uint8_t kDirectionLea[] = {
            0x72, 0x28, 0x0F, 0x28, 0xD0, 0x48, 0x8D, 0x54, 0x24, 0x20, 0x48, 0x8D, 0x0D};
        constexpr std::uint8_t kCall[] = {0xE8};
        constexpr std::uint8_t kDirectionStore[] = {0x0F, 0x10, 0x00, 0xF3, 0x0F, 0x7F, 0x05};
        constexpr std::uint8_t kVectorHead[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xF3, 0x0F, 0x59, 0x15};

        bool g_checked = false;
        bool g_ok = false;
        std::string g_error;
        std::uintptr_t g_base = 0;

        template <std::size_t N>
        bool Bytes(std::uintptr_t rva, const std::uint8_t (&bytes)[N])
        {
            return mem::Matches(reinterpret_cast<const void*>(g_base + rva), bytes, N);
        }

        // A rip-relative operand (or a call's rel32): the disp32 at dispRva,
        // the instruction ending at endRva, must reach targetRva.
        bool Rip(std::uintptr_t dispRva, std::uintptr_t endRva, std::uintptr_t targetRva)
        {
            std::int32_t disp = 0;
            if (!mem::Read(g_base + dispRva, disp))
                return false;
            return g_base + endRva + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(disp)) == g_base + targetRva;
        }

        bool Constant(std::uintptr_t rva, float wanted)
        {
            float value = 0.0f;
            return mem::Read(g_base + rva, value) && std::fabs(value - wanted) <= 1.0e-6f * (1.0f + std::fabs(wanted));
        }

        bool Check(std::string& error)
        {
            g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            const std::uintptr_t w = kManagerRva;
            const std::uintptr_t v = kWindRva;
            if (!(Bytes(0x1042D0C, kCurrMov) && Rip(0x1042D0E, 0x1042D12, w + kPreviousOffset) && Bytes(0x1042D12, kCurrLea) &&
                    Rip(0x1042D15, 0x1042D19, w) && Bytes(0x1042D19, kCurrStride) && Bytes(0x1042D2F, kCurrHash)))
                error = "curr_state_mismatch";
            else if (!(Bytes(0x7340BC, kSetHead) && Rip(0x7340C3, 0x7340C7, w) && Bytes(0x7340C7, kSetScale) &&
                         Rip(0x7340CD, 0x7340D1, kMsPerSecondRva) && Bytes(0x7340D1, kSetFields) &&
                         Constant(kMsPerSecondRva, 1000.0f)))
                error = "transition_setter_mismatch";
            else if (!(Bytes(0x75FFEC, kUpdateHead) && Bytes(0x760018, kUpdateLeft) && Bytes(0x760068, kUpdateFinal) &&
                         Rip(0x760071, 0x760075, w) && Bytes(0x760075, kUpdateCall) && Rip(0x760083, 0x760087, kFinalizeRva) &&
                         Bytes(0x76008E, kUpdateClear) && Rip(0x760090, 0x760095, w + kTargetOffset)))
                error = "transition_update_mismatch";
            else if (!(Bytes(0x10477E8, kLevelHandler) && Rip(0x10477F4, 0x10477F8, w + kRainOffset) &&
                         Bytes(0x1047868, kLevelHandler) && Rip(0x1047874, 0x1047878, w + kSnowOffset) &&
                         Bytes(0x760EB6, kRainReader) && Rip(0x760EC4, 0x760EC8, kZeroRva) && Bytes(0x762C35, kSnowReader) &&
                         Rip(0x762C42, 0x762C46, kZeroRva) && Constant(kZeroRva, 0.0f)))
                error = "rain_snow_mismatch";
            else if (!(Bytes(0x1059500, kComiss) && Rip(0x1059503, 0x1059507, kZeroRva) && Bytes(0x1059507, kSpeedLea) &&
                         Rip(0x105950F, 0x1059513, v) && Bytes(0x1059513, kSpeedJmp) &&
                         Rip(0x1059517, 0x105951B, kWindSpeedSetterRva) && Bytes(0x105951B, kMovByte) &&
                         Rip(0x105951D, 0x1059522, v + kWindSpeedSetOffset) && Bytes(kWindSpeedSetterRva, kSpeedSetter)))
                error = "wind_speed_mismatch";
            else if (!(Bytes(0x10594BC, kDirectionHead) && Rip(0x10594C3, 0x10594C7, kZeroRva) &&
                         Bytes(0x10594C7, kDirectionLea) && Rip(0x10594D4, 0x10594D8, v) && Bytes(0x10594D8, kCall) &&
                         Rip(0x10594D9, 0x10594DD, kWindVectorRva) && Bytes(0x10594DD, kDirectionStore) &&
                         Rip(0x10594E4, 0x10594E8, v + kWindDirectionOffset) && Bytes(0x10594E8, kMovByte) &&
                         Rip(0x10594EA, 0x10594EF, v + kWindDirectionSetOffset) && Bytes(kWindVectorRva, kVectorHead) &&
                         Rip(0x73A026, 0x73A02A, kDegToRadRva) && Constant(kDegToRadRva, 3.14159265f / 180.0f)))
                error = "wind_direction_mismatch";
            else
                return true;
            return false;
        }

        std::uintptr_t Manager(std::uintptr_t offset) { return g_base + kManagerRva + offset; }
        std::uintptr_t Wind(std::uintptr_t offset) { return g_base + kWindRva + offset; }

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
        // update tests first (0x75FFEC: -1 returns at once), so it goes last
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
