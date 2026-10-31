#pragma once

// A game address a clip keeps, named by the signature that holds it: the
// 32-bit hash of the signature's name and the distance from its start. The
// same place has the same id on every game build where the signature is
// found, so a clip saved on one build points at the same code or object on
// another (docs/signatures.md).
//
// A SiteMap is one build's ranges (RVAs): a signature's own bytes, a whole
// function (from its signature to the end of its last instruction) or a
// static object. Pure; unit tested in tests/game_sig_site_tests.cpp.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay::sig
{
    // FNV-1a 32 of the name; never 0 (0 names no signature).
    inline std::uint32_t NameHash(const char* name)
    {
        std::uint32_t hash = 0x811C9DC5u;
        for (const char* p = name; p && *p; ++p)
        {
            hash ^= static_cast<std::uint8_t>(*p);
            hash *= 0x01000193u;
        }
        return hash ? hash : 1u;
    }

    // name 0: a raw RVA in `offset` (no range holds it), valid only on the
    // build it was taken on. {0, 0} is no address.
    struct SiteId
    {
        std::uint32_t name = 0;
        std::uint32_t offset = 0;
    };

    // The u64 a clip writes: the name in the high dword.
    inline std::uint64_t Pack(SiteId id) { return (static_cast<std::uint64_t>(id.name) << 32) | id.offset; }
    inline SiteId Unpack(std::uint64_t packed)
    {
        return {static_cast<std::uint32_t>(packed >> 32), static_cast<std::uint32_t>(packed)};
    }

    // An address of another build that no range names: not 0 (an unknown
    // caller is recorded as 0) and equal to no site (a missing one is
    // 0xFFFFFFFF, fx_observer.h kNoSite).
    constexpr std::uint32_t kForeignRva = 0xFFFFFFF0u;

    // [begin, last], both inclusive: a call's return address is one past its
    // bytes.
    struct SiteRange
    {
        std::uint32_t name = 0;
        std::uint32_t begin = 0;
        std::uint32_t last = 0;
    };

    class SiteMap
    {
    public:
        // A range not found on this build (begin 0) is left out.
        void Add(std::uint32_t name, std::uint32_t begin, std::uint32_t last)
        {
            if (name && begin && last >= begin)
                m_ranges.push_back({name, begin, last});
        }

        const std::vector<SiteRange>& Ranges() const { return m_ranges; }

        // The range holding `rva` that starts last (then the shortest, then
        // the lower name): a call inside a function is named by the call. An
        // RVA no range holds is a raw id; 0 stays {0, 0}.
        SiteId SiteOf(std::uint32_t rva) const
        {
            const SiteRange* best = nullptr;
            for (const SiteRange& r : m_ranges)
            {
                if (rva < r.begin || rva > r.last)
                    continue;
                if (!best || r.begin > best->begin || (r.begin == best->begin && r.last < best->last) ||
                    (r.begin == best->begin && r.last == best->last && r.name < best->name))
                    best = &r;
            }
            if (!best)
                return {0, rva};
            return {best->name, rva - best->begin};
        }

        // Back to an RVA of this map's build. A raw id keeps its RVA when
        // `sameBuild` (the clip was taken on this build), else kForeignRva; a
        // name this build has not found is kForeignRva too, and so is an
        // offset past the range (the function shrank: the place is gone).
        std::uint32_t RvaOf(SiteId id, bool sameBuild) const
        {
            if (!id.name)
                return sameBuild || !id.offset ? id.offset : kForeignRva;
            for (const SiteRange& r : m_ranges)
                if (r.name == id.name)
                    return id.offset <= r.last - r.begin ? r.begin + id.offset : kForeignRva;
            return kForeignRva;
        }

    private:
        std::vector<SiteRange> m_ranges;
    };
}
