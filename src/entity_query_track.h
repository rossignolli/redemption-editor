#pragma once

// The engine's answers to the owner queries of the dynamic entity sounds
// (entity_query.h), kept while recording (clip section EQRY) and given back to
// the same queries of the puppets in the replay.
//
// A dynamic entity sound asks its owner's audio entity a chain of names when
// it plays; the answer comes from the owner's live state (the engine mount
// link, the task 0x8A, a held item's list, the attached boat or vehicle),
// none of which a puppet has, so the puppet's answer is the null sound and
// the sound ends 2 to 3 frames later. Here the state is the engine's own
// answer: per recorded owner and chain, every change of the 64-bit answer with
// its recording time (the chain itself once). The replay returns the answer in
// force for that owner and chain at the playhead (kLeadMs ahead, for a sound
// started a tick early): exactly what the engine computed live, null answers
// included (a sound that died live dies again). An owner and chain the
// recording never queried is left to the engine (and the mount relation).

#include "entity_query.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

namespace replay::entityquery
{
    struct Chain
    {
        std::uint32_t hash = 0; // ChainHash(n, refs)
        std::uint8_t n = 0;     // 1..kMaxChain
        std::uint32_t refs[kMaxChain] = {};
    };

    struct AnswerEvent
    {
        std::uint32_t timeMs = 0;
        std::uint32_t owner = 0;   // packed replay id
        std::uint16_t chain = 0;   // index into Track::chains
        std::uint64_t answer = 0;  // the 64-bit sound ref the engine wrote
    };

    constexpr std::size_t kChainBytesMin = 4 + 1 + 4;       // hash, n, one ref
    constexpr std::size_t kAnswerEventBytes = 4 + 4 + 2 + 8;
    constexpr std::size_t kMaxChains = 4096;
    constexpr std::size_t kMaxEvents = 200000;
    constexpr std::uint32_t kLeadMs = 50;

    // FNV-1a over the length and the names (the bytes, little endian).
    inline std::uint32_t ChainHash(std::uint32_t n, const std::uint32_t* refs)
    {
        std::uint32_t h = 2166136261u;
        const auto mix = [&h](std::uint32_t value) {
            for (int i = 0; i < 4; ++i)
            {
                h ^= (value >> (i * 8)) & 0xFFu;
                h *= 16777619u;
            }
        };
        mix(n);
        for (std::uint32_t i = 0; i < n && i < kMaxChain; ++i)
            mix(refs[i]);
        return h;
    }

    struct Track
    {
        std::vector<Chain> chains;
        std::vector<AnswerEvent> events; // time order
        // Recording only (not saved): queries left out.
        std::uint32_t droppedLong = 0; // a chain longer than kMaxChain
        std::uint32_t droppedFull = 0; // past kMaxChains or kMaxEvents

        void Clear()
        {
            chains.clear();
            events.clear();
            droppedLong = 0;
            droppedFull = 0;
            m_byHash.clear();
            m_last.clear();
        }

        bool Empty() const { return events.empty(); }
        std::size_t ApproxBytes() const { return chains.size() * sizeof(Chain) + events.size() * sizeof(AnswerEvent); }

        // The chain's index, added when new; -1 when too long or full.
        int ChainIndex(std::uint32_t n, const std::uint32_t* refs)
        {
            if (n == 0 || n > kMaxChain)
                return -1;
            const std::uint32_t hash = ChainHash(n, refs);
            const auto found = m_byHash.find(hash);
            if (found != m_byHash.end())
            {
                for (const int index : found->second)
                {
                    const Chain& c = chains[static_cast<std::size_t>(index)];
                    if (c.n == n && std::equal(refs, refs + n, c.refs))
                        return index;
                }
            }
            if (chains.size() >= kMaxChains)
                return -1;
            Chain c;
            c.hash = hash;
            c.n = static_cast<std::uint8_t>(n);
            std::copy(refs, refs + n, c.refs);
            chains.push_back(c);
            const int index = static_cast<int>(chains.size() - 1);
            m_byHash[hash].push_back(index);
            return index;
        }

        // Recording: one query's answer, kept when it is the first of that
        // owner and chain or differs from the last kept one. Times never go
        // back (a query drained late keeps the order).
        bool Note(std::uint32_t timeMs, std::uint32_t owner, std::uint32_t n, const std::uint32_t* refs, std::uint64_t answer)
        {
            if (n > kMaxChain)
            {
                ++droppedLong;
                return false;
            }
            const int index = ChainIndex(n, refs);
            if (index < 0)
            {
                ++droppedFull;
                return false;
            }
            const std::pair<std::uint32_t, std::uint16_t> key{owner, static_cast<std::uint16_t>(index)};
            const auto last = m_last.find(key);
            if (last != m_last.end() && last->second == answer)
                return false;
            if (events.size() >= kMaxEvents)
            {
                ++droppedFull;
                return false;
            }
            AnswerEvent e;
            e.timeMs = events.empty() ? timeMs : (std::max)(timeMs, events.back().timeMs);
            e.owner = owner;
            e.chain = static_cast<std::uint16_t>(index);
            e.answer = answer;
            events.push_back(e);
            m_last[key] = answer;
            return true;
        }

        // After a load or a cut: the lookup tables of ChainIndex and Note.
        void Reindex()
        {
            m_byHash.clear();
            m_last.clear();
            for (std::size_t i = 0; i < chains.size(); ++i)
                m_byHash[chains[i].hash].push_back(static_cast<int>(i));
            for (const AnswerEvent& e : events)
                m_last[{e.owner, e.chain}] = e.answer;
        }

    private:
        std::map<std::uint32_t, std::vector<int>> m_byHash;
        std::map<std::pair<std::uint32_t, std::uint16_t>, std::uint64_t> m_last;
    };

    // The saved window [base, end]: the answer in force at base of each owner
    // and chain moves to 0, the rest shifts by base, later ones go.
    inline void CutInto(const Track& in, Track& out, std::uint32_t base, std::uint32_t end)
    {
        out.Clear();
        out.chains = in.chains;
        std::map<std::pair<std::uint32_t, std::uint16_t>, AnswerEvent> inForce;
        std::vector<AnswerEvent> later;
        for (const AnswerEvent& e : in.events)
        {
            if (e.timeMs > end)
                continue;
            if (e.timeMs < base)
            {
                AnswerEvent copy = e;
                copy.timeMs = 0;
                inForce[{e.owner, e.chain}] = copy;
                continue;
            }
            AnswerEvent copy = e;
            copy.timeMs = e.timeMs - base;
            later.push_back(copy);
        }
        for (const auto& entry : inForce)
            out.events.push_back(entry.second);
        out.events.insert(out.events.end(), later.begin(), later.end());
        out.Reindex();
    }

    // The ring buffer drops what is before keepFrom: the answer in force of
    // each owner and chain stays, at keepFrom. Returns the events dropped.
    inline std::size_t TrimBefore(Track& track, std::uint32_t keepFrom)
    {
        std::map<std::pair<std::uint32_t, std::uint16_t>, AnswerEvent> inForce;
        std::vector<AnswerEvent> kept;
        std::size_t before = 0;
        for (const AnswerEvent& e : track.events)
        {
            if (e.timeMs < keepFrom)
            {
                ++before;
                AnswerEvent copy = e;
                copy.timeMs = keepFrom;
                inForce[{e.owner, e.chain}] = copy;
                continue;
            }
            kept.push_back(e);
        }
        if (!before)
            return 0;
        std::vector<AnswerEvent> events;
        events.reserve(inForce.size() + kept.size());
        for (const auto& entry : inForce)
            events.push_back(entry.second);
        events.insert(events.end(), kept.begin(), kept.end());
        const std::size_t dropped = track.events.size() - events.size();
        track.events.swap(events);
        track.Reindex();
        return dropped;
    }

    // Playback: the answers of one owner and chain in time order.
    struct Timeline
    {
        std::uint32_t owner = 0;
        std::uint16_t chain = 0;
        std::vector<std::pair<std::uint32_t, std::uint64_t>> answers; // (timeMs, answer)

        // The answer in force at t (false before the first).
        bool At(std::uint32_t t, std::uint64_t& answer) const
        {
            const auto after = std::upper_bound(answers.begin(), answers.end(), t,
                [](std::uint32_t time, const std::pair<std::uint32_t, std::uint64_t>& a) { return time < a.first; });
            if (after == answers.begin())
                return false;
            answer = std::prev(after)->second;
            return true;
        }
    };

    // Sorted by owner, then chain.
    inline std::vector<Timeline> Timelines(const Track& track)
    {
        std::map<std::pair<std::uint32_t, std::uint16_t>, Timeline> byKey;
        for (const AnswerEvent& e : track.events)
        {
            if (e.chain >= track.chains.size())
                continue;
            Timeline& t = byKey[{e.owner, e.chain}];
            t.owner = e.owner;
            t.chain = e.chain;
            t.answers.push_back({e.timeMs, e.answer});
        }
        std::vector<Timeline> out;
        out.reserve(byKey.size());
        for (auto& entry : byKey)
            out.push_back(std::move(entry.second));
        return out;
    }
}
