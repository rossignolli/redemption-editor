#pragma once

// Memory regions already asked, for code that asks VirtualQuery about many
// addresses at once (the clip pointer audit at the end of a recording). One
// VirtualQuery answers for its whole region (committed or not), so the
// pointers of one heap cost one query instead of one each.
//
// Pure: the query is the caller's (unit tested with a fake one). Meant for
// one pass on one thread; the regions may change afterwards, so a cache is
// not kept between passes.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay::mem
{
    class RegionCache
    {
    public:
        // `query(address, start, end, committed)` fills the region holding
        // `address`; false when it cannot tell (the address counts as not
        // committed and nothing is cached).
        template <typename Query>
        bool Committed(std::uintptr_t address, Query&& query)
        {
            for (const Region& region : m_regions)
            {
                if (address >= region.start && address < region.end)
                {
                    ++m_hits;
                    return region.committed;
                }
            }
            Region region;
            ++m_queries;
            if (!query(address, region.start, region.end, region.committed) || region.end <= region.start ||
                address < region.start || address >= region.end)
            {
                return false;
            }
            if (m_regions.size() < kMaxRegions)
                m_regions.push_back(region);
            return region.committed;
        }

        std::uint32_t Queries() const { return m_queries; }
        std::uint32_t Hits() const { return m_hits; }

    private:
        struct Region
        {
            std::uintptr_t start = 0;
            std::uintptr_t end = 0;
            bool committed = false;
        };
        static constexpr std::size_t kMaxRegions = 512;
        std::vector<Region> m_regions;
        std::uint32_t m_queries = 0;
        std::uint32_t m_hits = 0;
    };
}
