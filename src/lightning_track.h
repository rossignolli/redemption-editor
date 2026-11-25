#pragma once

// The clip's lightnings over time: every lightning the weather manager started
// while the recording ran, with its type (directional, cloud or strike) and,
// when it had one, the world position it struck. Playback fires each one again
// through the engine's own port (script_clip_world.cpp), so the flash and its
// thunder are the game's, not a copy. Pure data; the manager's fields live in
// lightning_state.h.
//
// Personal build (see the lightning note in docs/building.md): the game never
// wrote lightnings into a clip, so a clip WITH lightnings carries a section
// (LGTN) an unmodified Redemption Editor refuses; a clip without any stays
// readable by it (the section is omitted).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay
{
    // One lightning as the manager's type field showed it: the first frame of a
    // new non-zero type (a lightning lasts several frames and never changes
    // type while it runs).
    struct LightningStrike
    {
        std::uint32_t timeMs = 0;
        std::uint8_t type = 0; // 1 directional, 2 cloud, 3 strike (the manager's +0x6E00)
        bool positionKnown = false;
        float position[3] = {}; // the manager's +0x6E50, when it held one
    };

    namespace lighttrack
    {
        constexpr std::uint8_t kDirectional = 1;
        constexpr std::uint8_t kCloud = 2;
        constexpr std::uint8_t kStrike = 3;
        // A recording that long holds far fewer; the cap keeps a bad read from
        // growing the clip without end.
        constexpr std::size_t kMaxStrikes = 20000;
        // A position that close to the origin is the field not written yet.
        constexpr float kMinPosition = 0.01f;
        // A playhead jump this far forward (or any backwards jump) is a seek:
        // playback repositions its cursor instead of firing every strike passed.
        constexpr std::uint32_t kSeekMs = 500;

        inline bool Finite(const float* p)
        {
            return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
        }

        inline bool PlausiblePosition(const float* p)
        {
            return Finite(p) && std::fabs(p[0]) + std::fabs(p[1]) + std::fabs(p[2]) > kMinPosition;
        }

        inline bool Valid(const LightningStrike& strike)
        {
            if (strike.type < kDirectional || strike.type > kStrike)
                return false;
            return !strike.positionKnown || Finite(strike.position);
        }

        // Appends one, in time order (the recorder sees them one at a time).
        inline bool Add(std::vector<LightningStrike>& track, const LightningStrike& strike)
        {
            if (!Valid(strike) || track.size() >= kMaxStrikes)
                return false;
            if (!track.empty() && strike.timeMs < track.back().timeMs)
                return false;
            track.push_back(strike);
            return true;
        }

        // A clip cut: the strikes inside [baseMs, endMs], moved to the base.
        inline std::vector<LightningStrike> Cut(const std::vector<LightningStrike>& in, std::uint32_t baseMs,
            std::uint32_t endMs)
        {
            std::vector<LightningStrike> out;
            for (const LightningStrike& strike : in)
            {
                if (strike.timeMs < baseMs || strike.timeMs > endMs)
                    continue;
                LightningStrike shifted = strike;
                shifted.timeMs = strike.timeMs - baseMs;
                out.push_back(shifted);
            }
            return out;
        }

        // The first strike at or after timeMs (the playback cursor after a seek).
        inline std::size_t IndexAt(const std::vector<LightningStrike>& track, std::uint32_t timeMs)
        {
            const auto after = std::lower_bound(track.begin(), track.end(), timeMs,
                [](const LightningStrike& strike, std::uint32_t t) { return strike.timeMs < t; });
            return static_cast<std::size_t>(after - track.begin());
        }
    }
}
