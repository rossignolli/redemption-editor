#include "sky_state.h"
#include "engine_sigs.h"
#include "game_sig.h"
#include "guarded_memory.h"

#include <windows.h>

#include <cstring>
#include <initializer_list>

namespace replay::skystate
{
    using enginesig::kSkyUpdate;

    namespace
    {
        namespace sig = replay::sig;

        // The cloud natives' setters, each storing to its global. The noise
        // and position setters have the same bytes: each is reached from its
        // native, found by its hash where the natives are registered (lea
        // rdx, handler; mov rcx, hash), whose handler packs the vector and
        // calls the setter.
        const sig::Signature kNoiseNative{"sky_noise_native",
            "48 8D 15 ?? ?? ?? ?? 48 B9 32 2F 45 01 DF 66 79 FE E8 ?? ?? ?? ??", 0xFAD212};
        const sig::Signature kNoiseHandler{"sky_noise_handler", kNoiseNative, sig::Rel32{0x03, 0x07},
            "48 83 EC 38 48 8B 41 10 48 8D 4C 24 20 F3 0F 10 08 F3 0F 10 40 10 0F 28 D9 F3 0F 10 50 08 0F 14 D8 0F 14 "
            "D1 0F 14 DA 0F 29 5C 24 20 E8 ?? ?? ?? ??",
            0xF8E578};
        const sig::Signature kNoiseSetter{"sky_noise_setter", kNoiseHandler, sig::Rel32{0x2D, 0x31},
            "0F 10 01 0F 29 05 ?? ?? ?? ?? C3", 0x5DE0A0};
        const sig::Signature kNoise{"sky_noise", kNoiseSetter, sig::Rel32{0x06, 0x0A}, "", 0x4078F60};
        const sig::Signature kPositionNative{"sky_position_native",
            "48 8D 15 ?? ?? ?? ?? 48 B9 80 74 25 93 7B 76 C1 10 E8 ?? ?? ?? ??", 0xFAD239};
        const sig::Signature kPositionHandler{"sky_position_handler", kPositionNative, sig::Rel32{0x03, 0x07},
            "48 83 EC 38 48 8B 41 10 48 8D 4C 24 20 F3 0F 10 08 F3 0F 10 40 10 0F 28 D9 F3 0F 10 50 08 0F 14 D8 0F 14 "
            "D1 0F 14 DA 0F 29 5C 24 20 E8 ?? ?? ?? ??",
            0xF8E530};
        const sig::Signature kPositionSetter{"sky_position_setter", kPositionHandler, sig::Rel32{0x2D, 0x31},
            "0F 10 01 0F 29 05 ?? ?? ?? ?? C3", 0x5DE08C};
        const sig::Signature kPosition{"sky_position", kPositionSetter, sig::Rel32{0x06, 0x0A}, "", 0x3F29D10};
        const sig::Signature kLayerSetter{"sky_layer_setter", "0F 10 01 48 63 C2 48 8D 15 ?? ?? ?? ??", 0x5DE074};
        const sig::Signature kLayers{"sky_layers", kLayerSetter, sig::Rel32{0x09, 0x0D}, "", 0x4054160};
        // fmod(x, the 1.0 of the .rdata) into the drift global.
        const sig::Signature kDriftSetter{"sky_drift_setter",
            "40 53 48 83 EC 20 F3 0F 10 0D ?? ?? ?? ?? 48 8B D9 F3 0F 10 01 E8 ?? ?? ?? ?? F3 0F 10 0D ?? ?? ?? ?? F3 "
            "0F 11 05 ?? ?? ?? ??",
            0x5DDFD8};
        const sig::Signature kUnit{"sky_unit", kDriftSetter, sig::Rel32{0x0A, 0x0E}, "", 0x330C7A8};
        const sig::Signature kDrift{"sky_drift", kDriftSetter, sig::Rel32{0x26, 0x2A}, "", 0x4054180};
        // fmod(x, max(1, max - min)) into the region global.
        const sig::Signature kRegionSetter{"sky_region_setter",
            "40 53 48 83 EC 30 8B 05 ?? ?? ?? ?? 48 8B D9 2B 05 ?? ?? ?? ?? B9 01 00 00 00 3B C1 0F 29 74 24 20 F3 0F "
            "10 03 0F 4C C1 66 0F 6E F0 0F 5B F6 0F 28 CE E8 ?? ?? ?? ?? F3 0F 11 05 ?? ?? ?? ??",
            0x5DE0AC};
        const sig::Signature kRegionMax{"sky_region_max", kRegionSetter, sig::Rel32{0x08, 0x0C}, "", 0x5A86EA0};
        const sig::Signature kRegionMin{"sky_region_min", kRegionSetter, sig::Rel32{0x11, 0x15}, "", 0x5A86E94};
        const sig::Signature kRegion{"sky_region", kRegionSetter, sig::Rel32{0x3B, 0x3F}, "", 0x3F28FB0};
        // The sky update, and in it the reads of the two periods (movss
        // xmm6, [rip]) and of the unpinned global (lea rcx, [rip]).
        const sig::Signature kPositionPeriodRead{"sky_position_period_read", kSkyUpdate, sig::Offset{0x68B},
            "F3 0F 10 35 ?? ?? ?? ??", 0x5F8CCB};
        const sig::Signature kPositionPeriod{"sky_position_period", kPositionPeriodRead, sig::Rel32{0x04, 0x08}, "",
            0x39AA79C};
        const sig::Signature kNoisePeriodRead{"sky_noise_period_read", kSkyUpdate, sig::Offset{0x7B5},
            "F3 0F 10 35 ?? ?? ?? ??", 0x5F8DF5};
        const sig::Signature kNoisePeriod{"sky_noise_period", kNoisePeriodRead, sig::Rel32{0x04, 0x08}, "", 0x39AA77C};
        const sig::Signature kUnpinnedRead{"sky_unpinned_read", kSkyUpdate, sig::Offset{0x606},
            "48 8D 0D ?? ?? ?? ??", 0x5F8C46};
        const sig::Signature kUnpinned{"sky_unpinned", kUnpinnedRead, sig::Rel32{0x03, 0x07}, "", 0x40651E0};

        // Where each global is in this game (Check).
        struct Globals
        {
            std::uintptr_t noise = 0, position = 0, layers = 0, drift = 0, region = 0, unit = 0, regionMax = 0,
                           regionMin = 0, positionPeriod = 0, noisePeriod = 0, unpinned = 0;
        };
        Globals g_at;

        bool g_checked = false;
        bool g_ok = false;
        std::string g_error;

        bool Found(std::initializer_list<const sig::Signature*> list)
        {
            for (const sig::Signature* s : list)
                if (!sig::Find(*s))
                    return false;
            return true;
        }

        bool Check(std::string& error)
        {
            if (!Found({&kNoiseNative, &kNoiseHandler, &kNoiseSetter, &kNoise}))
                error = "noise_setter_mismatch";
            else if (!Found({&kPositionNative, &kPositionHandler, &kPositionSetter, &kPosition}))
                error = "position_setter_mismatch";
            else if (!Found({&kLayerSetter, &kLayers}))
                error = "layer_setter_mismatch";
            else if (!Found({&kDriftSetter, &kUnit, &kDrift}))
                error = "drift_setter_mismatch";
            else if (!Found({&kRegionSetter, &kRegionMax, &kRegionMin, &kRegion}))
                error = "region_setter_mismatch";
            else if (!Found({&kSkyUpdate}))
                error = "sky_update_mismatch";
            else if (!Found({&kPositionPeriodRead, &kPositionPeriod}))
                error = "position_period_mismatch";
            else if (!Found({&kNoisePeriodRead, &kNoisePeriod}))
                error = "noise_period_mismatch";
            else if (!Found({&kUnpinnedRead, &kUnpinned}))
                error = "unpinned_site_mismatch";
            else
            {
                g_at.noise = sig::Find(kNoise);
                g_at.position = sig::Find(kPosition);
                g_at.layers = sig::Find(kLayers);
                g_at.drift = sig::Find(kDrift);
                g_at.region = sig::Find(kRegion);
                g_at.unit = sig::Find(kUnit);
                g_at.regionMax = sig::Find(kRegionMax);
                g_at.regionMin = sig::Find(kRegionMin);
                g_at.positionPeriod = sig::Find(kPositionPeriod);
                g_at.noisePeriod = sig::Find(kNoisePeriod);
                g_at.unpinned = sig::Find(kUnpinned);
                return true;
            }
            return false;
        }

        bool ReadFloats(std::uintptr_t address, float* out, std::size_t count)
        {
            return mem::Copy(out, address, count * sizeof(float));
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
        if (!ReadFloats(g_at.noise, noise, 3) || !ReadFloats(g_at.position, position, 3) ||
            !ReadFloats(g_at.layers, layers, 8) || !ReadFloats(g_at.drift, drift, 2) || !ReadFloats(g_at.region, region, 2) ||
            !ReadFloats(g_at.unpinned, unpinned, 3))
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
        if (!ReadFloats(g_at.noisePeriod, &noise, 1) || !ReadFloats(g_at.positionPeriod, &position, 1) ||
            !ReadFloats(g_at.unit, &unit, 1) || !mem::Read(g_at.regionMax, regionMax) ||
            !mem::Read(g_at.regionMin, regionMin))
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
