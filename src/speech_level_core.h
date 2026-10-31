#pragma once

// The speech level probe, pure part. Read only: for every speech line the engine plays (state
// 5) on a recorded ped (recording) or a puppet and the live player
// (playback), the request fields that build its volume, the volume the
// engine gives it and the speaker's audio mix group, next to the mixer
// scenes active then (mix_scene_core.h). It decides between the two causes
// of the "ghost camp voices": a mixer
// scene that ducked the line live (same line volume both passes, a
// conversation scene active only in the recording, the speaker not one of
// its participants) or the line's own fields (different line volume).
//
// Guarded reads of the speech entity's lines and their data (dump 1491.50);
// type 2 lines feed five layers, so they have no single volume and are
// logged as "na".
// Research: docs/research/speech-line-request.md

#include <cmath>
#include <cstdint>
#include <map>
#include <string>

namespace replay::speech::level
{
    constexpr std::uint32_t kDataSmoother = 0x148;
    constexpr std::uint32_t kSmootherValue = 0x4;
    constexpr std::uint32_t kSmootherFlags = 0xC;
    constexpr float kSilentDb = -100.0f;

    inline float LinearToDb(float linear)
    {
        if (!(linear > 0.00001f) || !std::isfinite(linear))
            return kSilentDb;
        return 20.0f * std::log10(linear);
    }

    // Samples per line: at first sight in state 5, then every kEveryMs while
    // it plays, kMaxPerLine at most.
    constexpr std::uint32_t kEveryMs = 1000;
    constexpr std::uint32_t kMaxPerLine = 4;

    class Schedule
    {
    public:
        void Reset() { m_lines.clear(); }
        // True when the line is due a sample at wall time nowMs.
        bool Due(std::uint64_t line, std::uint64_t nowMs)
        {
            Entry& entry = m_lines[line];
            if (entry.count >= kMaxPerLine)
                return false;
            if (entry.count && nowMs < entry.nextMs)
                return false;
            ++entry.count;
            entry.nextMs = nowMs + kEveryMs;
            return true;
        }
        // Lines no longer seen are forgotten (bounded map).
        void Forget(std::size_t keepAtMost)
        {
            if (m_lines.size() > keepAtMost)
                m_lines.clear();
        }

    private:
        struct Entry
        {
            std::uint32_t count = 0;
            std::uint64_t nextMs = 0;
        };
        std::map<std::uint64_t, Entry> m_lines;
    };

    // Per pass and source: lines seen and the mean of the type 1 volumes.
    struct Tally
    {
        struct Row
        {
            std::uint64_t samples = 0;
            std::uint64_t volumeSamples = 0;
            double sumDb = 0.0;
            std::uint64_t ducked = 0; // a conversation scene was active and the speaker not a participant
        };
        std::map<std::uint32_t, Row> bySource;

        void Note(std::uint32_t source, bool hasVolume, float db, bool ducked)
        {
            Row& row = bySource[source];
            ++row.samples;
            if (hasVolume)
            {
                ++row.volumeSamples;
                row.sumDb += db;
            }
            row.ducked += ducked ? 1 : 0;
        }
        void Clear() { bySource.clear(); }

        // "src3:n=12,mean_db=-6.1,ducked=10|src2:..." ("none" when empty).
        std::string Text() const
        {
            std::string text;
            char item[96]{};
            for (const auto& entry : bySource)
            {
                const Row& row = entry.second;
                if (row.volumeSamples)
                    std::snprintf(item, sizeof(item), "%ssrc%u:n=%llu,mean_db=%.1f,ducked=%llu", text.empty() ? "" : "|",
                        entry.first, static_cast<unsigned long long>(row.samples), row.sumDb / row.volumeSamples,
                        static_cast<unsigned long long>(row.ducked));
                else
                    std::snprintf(item, sizeof(item), "%ssrc%u:n=%llu,mean_db=na,ducked=%llu", text.empty() ? "" : "|",
                        entry.first, static_cast<unsigned long long>(row.samples), static_cast<unsigned long long>(row.ducked));
                text += item;
            }
            return text.empty() ? std::string("none") : text;
        }
    };
}
