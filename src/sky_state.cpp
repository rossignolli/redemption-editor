#include "sky_state.h"
#include "guarded_memory.h"

#include <windows.h>

#include <cstring>

namespace replay::skystate
{
    namespace
    {
        // A site: the bytes at an RVA, and the rip-relative operand(s) in
        // them that must reach the documented RVA.
        struct RipOperand
        {
            std::size_t dispOffset; // where the disp32 is, from the site
            std::size_t insnEnd;    // where the instruction ends, from the site
            std::uintptr_t targetRva;
        };

        constexpr std::uintptr_t kNoiseSetterRva = 0x5DE0A0;
        constexpr std::uint8_t kNoiseSetter[] = {0x0F, 0x10, 0x01, 0x0F, 0x29, 0x05};
        constexpr std::uintptr_t kPositionSetterRva = 0x5DE08C;
        constexpr std::uint8_t kPositionSetter[] = {0x0F, 0x10, 0x01, 0x0F, 0x29, 0x05};
        constexpr std::uintptr_t kLayerSetterRva = 0x5DE074;
        constexpr std::uint8_t kLayerSetter[] = {0x0F, 0x10, 0x01, 0x48, 0x63, 0xC2, 0x48, 0x8D, 0x15};
        constexpr std::uintptr_t kDriftSetterRva = 0x5DDFD8;
        constexpr std::uint8_t kDriftSetter[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xF3, 0x0F, 0x10, 0x0D};
        constexpr std::uintptr_t kRegionSetterRva = 0x5DE0AC;
        constexpr std::uint8_t kRegionSetter[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x8B, 0x05};
        constexpr std::uintptr_t kSkyUpdateRva = 0x5F8640;
        constexpr std::uint8_t kSkyUpdate[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48, 0x89, 0x78, 0x18, 0x55};
        constexpr std::uintptr_t kPositionPeriodSiteRva = 0x5F8CCB; // movss xmm6, [rip]
        constexpr std::uintptr_t kNoisePeriodSiteRva = 0x5F8DF5;    // movss xmm6, [rip]
        constexpr std::uint8_t kMovssXmm6[] = {0xF3, 0x0F, 0x10, 0x35};
        constexpr std::uintptr_t kUnpinnedSiteRva = 0x5F8C46; // lea rcx, [rip]
        constexpr std::uint8_t kLeaRcx[] = {0x48, 0x8D, 0x0D};

        constexpr std::uintptr_t kNoiseRva = 0x4078F60;
        constexpr std::uintptr_t kPositionRva = 0x3F29D10;
        constexpr std::uintptr_t kLayersRva = 0x4054160;
        constexpr std::uintptr_t kDriftRva = 0x4054180;
        constexpr std::uintptr_t kRegionRva = 0x3F28FB0;
        constexpr std::uintptr_t kUnitRva = 0x330C7A8; // .rdata 1.0f
        constexpr std::uintptr_t kRegionMaxRva = 0x5A86EA0;
        constexpr std::uintptr_t kRegionMinRva = 0x5A86E94;
        constexpr std::uintptr_t kPositionPeriodRva = 0x39AA79C;
        constexpr std::uintptr_t kNoisePeriodRva = 0x39AA77C;
        constexpr std::uintptr_t kUnpinnedRva = 0x40651E0;

        bool g_checked = false;
        bool g_ok = false;
        std::string g_error;
        std::uintptr_t g_base = 0;

        bool Site(std::uintptr_t rva, const std::uint8_t* head, std::size_t headSize, const RipOperand* operands,
            std::size_t count)
        {
            const std::uintptr_t site = g_base + rva;
            if (!mem::Matches(reinterpret_cast<const void*>(site), head, headSize))
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
            const RipOperand noise[] = {{6, 10, kNoiseRva}};
            const RipOperand position[] = {{6, 10, kPositionRva}};
            const RipOperand layer[] = {{9, 13, kLayersRva}};
            const RipOperand drift[] = {{0x0A, 0x0E, kUnitRva}, {0x26, 0x2A, kDriftRva}};
            const RipOperand region[] = {{0x08, 0x0C, kRegionMaxRva}, {0x11, 0x15, kRegionMinRva}, {0x3B, 0x3F, kRegionRva}};
            const RipOperand positionPeriod[] = {{4, 8, kPositionPeriodRva}};
            const RipOperand noisePeriod[] = {{4, 8, kNoisePeriodRva}};
            const RipOperand unpinned[] = {{3, 7, kUnpinnedRva}};
            if (!Site(kNoiseSetterRva, kNoiseSetter, sizeof(kNoiseSetter), noise, 1))
                error = "noise_setter_mismatch";
            else if (!Site(kPositionSetterRva, kPositionSetter, sizeof(kPositionSetter), position, 1))
                error = "position_setter_mismatch";
            else if (!Site(kLayerSetterRva, kLayerSetter, sizeof(kLayerSetter), layer, 1))
                error = "layer_setter_mismatch";
            else if (!Site(kDriftSetterRva, kDriftSetter, sizeof(kDriftSetter), drift, 2))
                error = "drift_setter_mismatch";
            else if (!Site(kRegionSetterRva, kRegionSetter, sizeof(kRegionSetter), region, 3))
                error = "region_setter_mismatch";
            else if (!Site(kSkyUpdateRva, kSkyUpdate, sizeof(kSkyUpdate), nullptr, 0))
                error = "sky_update_mismatch";
            else if (!Site(kPositionPeriodSiteRva, kMovssXmm6, sizeof(kMovssXmm6), positionPeriod, 1))
                error = "position_period_mismatch";
            else if (!Site(kNoisePeriodSiteRva, kMovssXmm6, sizeof(kMovssXmm6), noisePeriod, 1))
                error = "noise_period_mismatch";
            else if (!Site(kUnpinnedSiteRva, kLeaRcx, sizeof(kLeaRcx), unpinned, 1))
                error = "unpinned_site_mismatch";
            else
                return true;
            return false;
        }

        bool ReadFloats(std::uintptr_t rva, float* out, std::size_t count)
        {
            return mem::Copy(out, g_base + rva, count * sizeof(float));
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

    bool Read(SkySample& out)
    {
        if (!g_ok)
            return false;
        float noise[3], position[3], layers[8], drift[2], region[2], unpinned[3];
        if (!ReadFloats(kNoiseRva, noise, 3) || !ReadFloats(kPositionRva, position, 3) || !ReadFloats(kLayersRva, layers, 8) ||
            !ReadFloats(kDriftRva, drift, 2) || !ReadFloats(kRegionRva, region, 2) || !ReadFloats(kUnpinnedRva, unpinned, 3))
            return false;
        using namespace sky;
        out.value[kNoiseX] = noise[0];
        out.value[kNoiseY] = noise[1];
        out.value[kNoiseZ] = noise[2];
        out.value[kPositionX] = position[0];
        out.value[kPositionY] = position[1];
        out.value[kPositionZ] = position[2];
        out.value[kLayer0X] = layers[0];
        out.value[kLayer0Y] = layers[1];
        out.value[kLayer1X] = layers[4];
        out.value[kLayer1Y] = layers[5];
        out.value[kDriftX] = drift[0];
        out.value[kDriftY] = drift[1];
        out.value[kRegionX] = region[0];
        out.value[kRegionY] = region[1];
        std::memcpy(out.unpinned, unpinned, sizeof(unpinned));
        return skytrack::Finite(out);
    }

    bool ReadPeriods(SkyPeriods& out)
    {
        if (!g_ok)
            return false;
        float noise = 0.0f, position = 0.0f, unit = 0.0f;
        std::int32_t regionMax = 0, regionMin = 0;
        if (!ReadFloats(kNoisePeriodRva, &noise, 1) || !ReadFloats(kPositionPeriodRva, &position, 1) ||
            !ReadFloats(kUnitRva, &unit, 1) || !mem::Read(g_base + kRegionMaxRva, regionMax) ||
            !mem::Read(g_base + kRegionMinRva, regionMin))
            return false;
        // As the region setter: max(1, max - min), as a float.
        const std::int64_t span = static_cast<std::int64_t>(regionMax) - regionMin;
        out.value[sky::kPeriodNoise] = noise;
        out.value[sky::kPeriodPosition] = position;
        out.value[sky::kPeriodLayer] = unit;
        out.value[sky::kPeriodDrift] = unit;
        out.value[sky::kPeriodRegion] = static_cast<float>(span < 1 ? 1 : span);
        // A period that is not a positive number is unknown (0): the
        // channel is held between samples instead of interpolated.
        for (float& v : out.value)
            if (!(v > 0.0f) || !(v < 1.0e9f))
                v = 0.0f;
        return true;
    }
}
