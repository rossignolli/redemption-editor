#pragma once

// The clip pointer audit (POSE_V2_CLIP_POINTER_AUDIT, Debug=1 evidence,
// replay_events.cpp) in two parts. At the recording stop the script thread
// only copies the plausible 8-byte words of the clip's raw engine blobs
// (Input, a few microseconds); a worker classifies them against the address
// space afterwards (Classify). In the stop tick the audit would cost up to
// 133 ms in fist fights: VirtualQuery answers with the run of pages of
// identical state from the queried page on, so a melee info or melee
// definition pointer into the game's heap costs a walk over that whole run.
//
// A word counts when the page it points at is committed, and as image or
// other by the image range. One query answers for its whole run
// (region_cache.h). The words are asked in ascending address order: the first
// query of a run covers every later word in it. The found and blob maps are
// keyed (kind, offset) and kind, so their text does not depend on the order.
//
// Pure: the query is the caller's (unit tested with a fake address space,
// tests/stop_path_tests.cpp).

#include "region_cache.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace replay::ptraudit
{
    // A word worth asking about: user space, 8-aligned, past the first 64 KB.
    inline bool Plausible(std::uint64_t value)
    {
        return !(value < 0x10000 || value > 0x7FFFFFFEFFFFull || (value & 7));
    }

    // The words of the clip's blobs, copied on the script thread.
    class Input
    {
    public:
        struct Word
        {
            std::uint64_t value = 0;
            std::uint32_t kind = 0;   // index into Kinds()
            std::uint32_t offset = 0; // byte offset in its blob
        };

        // Every 8-byte word of one blob, counted under `kind`.
        void Scan(const char* kind, const std::uint8_t* data, std::size_t bytes)
        {
            const std::uint32_t index = KindIndex(kind);
            ++m_blobs[m_kinds[index]];
            for (std::size_t offset = 0; offset + 8 <= bytes; offset += 8)
            {
                std::uint64_t value = 0;
                std::memcpy(&value, data + offset, sizeof(value));
                if (Plausible(value))
                    m_words.push_back({value, index, static_cast<std::uint32_t>(offset)});
            }
        }

        // One 8-byte value, as a blob of its own.
        void ScanValue(const char* kind, std::uint64_t value)
        {
            Scan(kind, reinterpret_cast<const std::uint8_t*>(&value), sizeof(value));
        }

        const std::vector<Word>& Words() const { return m_words; }
        const std::vector<std::string>& Kinds() const { return m_kinds; }
        const std::map<std::string, std::size_t>& Blobs() const { return m_blobs; }

    private:
        std::uint32_t KindIndex(const char* kind)
        {
            for (std::size_t index = 0; index < m_kinds.size(); ++index)
            {
                if (m_kinds[index] == kind)
                    return static_cast<std::uint32_t>(index);
            }
            m_kinds.emplace_back(kind);
            return static_cast<std::uint32_t>(m_kinds.size() - 1);
        }

        std::vector<std::string> m_kinds;
        std::map<std::string, std::size_t> m_blobs;
        std::vector<Word> m_words;
    };

    struct Result
    {
        std::string blobs;    // "kind=count;" per kind, by name
        std::string pointers; // "kind+0xoffset=Ni/Mo;" per word slot that held a committed address, or "none"
        std::uint32_t queries = 0;
        std::uint32_t regionHits = 0;
    };

    // `query(address, start, end, committed)` as RegionCache takes it (the
    // real one: VirtualQuery, BaseAddress to BaseAddress + RegionSize).
    template <typename Query>
    Result Classify(const Input& input, std::uintptr_t imageBase, std::uintptr_t imageEnd, Query&& query)
    {
        struct Hits
        {
            std::uint32_t image = 0;
            std::uint32_t other = 0;
        };
        const std::vector<Input::Word>& words = input.Words();
        std::vector<std::uint32_t> order(words.size());
        for (std::size_t index = 0; index < order.size(); ++index)
            order[index] = static_cast<std::uint32_t>(index);
        std::stable_sort(order.begin(), order.end(),
            [&words](std::uint32_t a, std::uint32_t b) { return words[a].value < words[b].value; });
        replay::mem::RegionCache regions;
        std::map<std::pair<std::string, std::size_t>, Hits> found;
        for (const std::uint32_t index : order)
        {
            const Input::Word& word = words[index];
            if (!regions.Committed(static_cast<std::uintptr_t>(word.value), query))
                continue;
            Hits& hits = found[{input.Kinds()[word.kind], static_cast<std::size_t>(word.offset)}];
            if (word.value >= imageBase && word.value < imageEnd)
                ++hits.image;
            else
                ++hits.other;
        }
        // The audit's text.
        std::ostringstream text;
        for (const auto& [key, hits] : found)
            text << key.first << "+0x" << std::hex << key.second << std::dec << "=" << hits.image << "i/" << hits.other << "o;";
        std::ostringstream counts;
        for (const auto& [kind, count] : input.Blobs())
            counts << kind << "=" << count << ";";
        Result result;
        result.blobs = counts.str();
        result.pointers = found.empty() ? std::string("none") : text.str();
        result.queries = regions.Queries();
        result.regionHits = regions.Hits();
        return result;
    }
}
