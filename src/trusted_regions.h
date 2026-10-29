#pragma once

// The readable regions the blood code trusts without asking VirtualQuery
// again (ped_blood.cpp Readable). One query on the ped and damage
// component pools costs 4 to 21 ms, a frame over the vsync, so a fixed
// time to live would make a blood record pay it every few seconds. A
// region in use stays trusted while it keeps being used (each hit pushes
// its expiry), so the pools are asked once per playback; a region not used
// for the time to live is asked again. The cache is dropped at every
// playback start and end (ForgetReadableRegions).
//
// Pure: the clock is the caller's (unit tested).

#include <cstddef>
#include <cstdint>

namespace replay::mem
{
    template <std::size_t Slots>
    class TrustedRegions
    {
    public:
        // True when [first, last) lies in a trusted region; the hit keeps
        // that region trusted until now + ttl.
        bool Contains(std::uintptr_t first, std::uintptr_t last, std::uint64_t now, std::uint64_t ttl)
        {
            for (Region& region : m_regions)
            {
                if (region.expires > now && first >= region.start && last <= region.end)
                {
                    region.expires = now + ttl;
                    ++m_hits;
                    return true;
                }
            }
            return false;
        }

        // A region just seen committed, readable and without guard pages.
        void Add(std::uintptr_t start, std::uintptr_t end, std::uint64_t now, std::uint64_t ttl)
        {
            Region& slot = m_regions[m_next++ % Slots];
            slot.start = start;
            slot.end = end;
            slot.expires = now + ttl;
        }

        void Clear()
        {
            for (Region& region : m_regions)
                region = Region{};
        }

        std::uint64_t Hits() const { return m_hits; }

    private:
        struct Region
        {
            std::uintptr_t start = 0;
            std::uintptr_t end = 0;
            std::uint64_t expires = 0;
        };
        Region m_regions[Slots];
        std::uint32_t m_next = 0;
        std::uint64_t m_hits = 0;
    };
}
