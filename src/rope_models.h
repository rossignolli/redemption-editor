#pragma once

// The models of the engine's rope types. The rope type table's init 0x6D7984
// registers each one (mov edx, hash; lea r8, name; call 0x2607AF8); an object
// of one of these models is a rope's own entity (one ROPE_REINS by every
// horse), not a prop: the rope track (ROPE) replays the rope. Recorded as
// props they would never load in playback (model_timeout) and would hide the
// original late.
//
// Pure, unit tested in tests/scenery_speech_tests.cpp.

#include <cstddef>
#include <cstdint>

namespace replay::ropemodels
{
    struct RopeModel
    {
        std::uint32_t hash;
        const char* name;
    };

    // In the order of 0x6D7984 (build 1491.50).
    constexpr RopeModel kRopeModels[] = {
        {0xD61D9FF3u, "ROPE_THIN"},
        {0x35DB353Du, "ROPE_DEFAULT"},
        {0x3A2305CAu, "ROPE_DEFAULT_32"},
        {0x50924062u, "ROPE_DEFAULT_WIRE"},
        {0x9F2D0F01u, "ROPE_THIN_WIRE_32"},
        {0x0B4EBCE9u, "ROPE_REINS"},
        {0x772C00ACu, "ROPE_THIN_4"},
        {0x5F022193u, "ROPE_CHAIN"},
        {0xC757A15Au, "ROPE_FISHING_LINE"},
        {0x94819C76u, "ROPE_VINE"},
        {0xE2271E5Bu, "PHYSICS_ROPE_FISHING_LINE_2"},
        {0x7B70508Bu, "ROPE_CHAIN_THICK"},
        {0x87D0C329u, "ROPE_LOGWAGON"},
        {0xAD1E2E8Eu, "ROPE_CLOTHSLINE"},
        {0x25DDA17Du, "ROPE_REINS_ROPE"},
        {0x43AB24BAu, "ROPE_HANG_LIGHT"},
        {0x6242E8AAu, "ROPE_BALLOON"},
        {0x01A63462u, "ROPE_THIN_4_MP"},
        {0x364E71BBu, "ROPE_LASSO_DLC"},
    };
    constexpr std::size_t kRopeModelCount = sizeof(kRopeModels) / sizeof(kRopeModels[0]);

    // The name of a rope model, nullptr for any other model.
    inline const char* RopeModelName(std::uint32_t model)
    {
        for (const RopeModel& rope : kRopeModels)
            if (rope.hash == model)
                return rope.name;
        return nullptr;
    }

    inline bool IsRopeModel(std::uint32_t model) { return RopeModelName(model) != nullptr; }
}
