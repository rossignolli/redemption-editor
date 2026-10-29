#pragma once

// Which world positions may grow the clip area, and the repair of an area
// that an invalid position stretched.
//
// Foot dust puffs (site 0x145320F, flags 0x200) can be recorded as world
// effects at (0; -0.03; 0): their instance has a parent matrix of zeros
// (mode 1, the parent not written yet), so the composition keeps the local
// offset as if it were the world matrix. The area then grows from the
// scene to the map origin, and the playback start clear (fires, particles,
// decals, pickups) is centred hundreds of metres away from the scene.
//
// A local offset is never a world position: the recording drops a mode 1
// placement whose parent is all zeros (FxWorldUnresolved) and the area takes no
// position near the map origin or outside the map. A clip saved with a
// stretched area keeps it; RepairArea rebuilds it at playback start from the
// positions the clip holds (world effects, decals not attached, registered
// effects, the game camera), each one checked the same way.
//
// Pure (no game); unit tested in tests/sound_particles_tests.cpp.

#include <cmath>
#include <cstdint>

namespace replay::cliparea
{
    // A position this close to the map origin (horizontal distance) is a
    // local offset taken for a world position. The RDR2 map spans about
    // -7000..+4000 m; nothing the replay records sits within 5 m of (0, 0).
    constexpr float kOriginRejectM = 5.0f;
    constexpr float kMapLimitM = 16000.0f;
    constexpr float kHeightLimitM = 5000.0f;

    inline bool PositionValid(float x, float y, float z)
    {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            return false;
        if (std::fabs(x) > kMapLimitM || std::fabs(y) > kMapLimitM || std::fabs(z) > kHeightLimitM)
            return false;
        return x * x + y * y >= kOriginRejectM * kOriginRejectM;
    }

    // An effect placed by a parent matrix (mode 1) whose parent is all
    // zeros: the offset is local and the world position is unknown.
    inline bool FxWorldUnresolved(std::uint8_t mode, const float parent[16])
    {
        if (mode != 1)
            return false;
        for (int n = 0; n < 16; ++n)
            if (parent[n] != 0.0f)
                return false;
        return true;
    }

    // An axis-aligned box of valid positions.
    struct Area
    {
        bool has = false;
        float min[3] = {};
        float max[3] = {};
        std::uint32_t rejected = 0; // positions refused by PositionValid

        // False (and counted) when the position is refused.
        bool Extend(float x, float y, float z)
        {
            if (!PositionValid(x, y, z))
            {
                ++rejected;
                return false;
            }
            const float p[3] = {x, y, z};
            if (!has)
            {
                for (int n = 0; n < 3; ++n)
                    min[n] = max[n] = p[n];
                has = true;
                return true;
            }
            for (int n = 0; n < 3; ++n)
            {
                min[n] = p[n] < min[n] ? p[n] : min[n];
                max[n] = p[n] > max[n] ? p[n] : max[n];
            }
            return true;
        }
    };

    // A saved area that reaches the map origin was stretched by an invalid
    // position.
    inline bool AreaReachesOrigin(bool has, const float min[3], const float max[3])
    {
        if (!has)
            return false;
        return min[0] <= kOriginRejectM && max[0] >= -kOriginRejectM && min[1] <= kOriginRejectM &&
            max[1] >= -kOriginRejectM;
    }

    // Rebuilds the area from the clip's positions: `each(extend)` calls
    // extend(x, y, z) for every position the clip holds. Returns true when
    // the new area has at least one position.
    template <typename Each>
    Area RebuildArea(Each&& each)
    {
        Area area;
        each([&](float x, float y, float z) { area.Extend(x, y, z); });
        return area;
    }
}
