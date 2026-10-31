#pragma once

// The audio wind in the replay, pure part.
//
// Why: the weather audio entity asks the map streamer whether the world is
// loaded about the live player. In a replay the live player stays hidden far
// from the replay camera, the answer is false, and the wind freezes at full
// level on the slots started in the first frames.
//
// The fix: a detour of that gate (prologue checked) forwards every call; only
// the call from the weather pre-update (return address kGateCall + 5) during
// playback is asked about the replay's streaming focus instead (else the
// rendered camera, else the trail focus puppet). The engine then computes the
// wind itself; no wind value is written.
//
// Evidence: clip section WNDA (optional, 4 Hz) keeps the wind strength, the
// weather of each wind row and whether the simple path ran; playback logs the
// recorded value against the engine's (WIND_REPLAY, WIND_REPLAY_TOTAL). A
// clip without the section still gets the fix (rec=none).
// Offline dump, build 1491.50; sites found by signature (kSites).
// Research: docs/research/weather-wind-sky-timecycle.md
//
// Pure: unit tested in wind_replay_tests.cpp.

#include "engine_sigs.h"
#include "env_override.h"
#include "game_sig_core.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace replay::windreplay
{
    using enginesig::kWeatherEntityInit;
    using enginesig::kWeatherEntityVtable;

    // ------------------------------------------------------------ layout

    constexpr std::uint32_t kGateMask = 0x78;
    constexpr std::uint32_t kGateCallLength = 5; // the return address: kGateCall + 5
    constexpr std::uint32_t kPreUpdateSlot = 0x28; // in the weather entity's vtable
    constexpr std::uint32_t kWindStrength = 0x4F44;    // W+0x1980+0x35C4 (kWeatherRows' wind block, kStrength)
    constexpr std::uint32_t kSimpleStamp = 0x50A4;     // u32 time, written only by the simple path (kSimple)
    constexpr std::uint32_t kWindLinePrev = 0x5094;    // u32 weather index the row 0 slots were started with
    constexpr std::uint32_t kWindLineNext = 0x5098;    // row 1
    constexpr std::uint32_t kWeatherTypeStride = 0x1E0;
    constexpr std::uint32_t kWeatherTypeHash = 0x10;
    constexpr std::uint32_t kMaxWeatherTypes = 256;

    // ------------------------------------------------------------ sites

    // The code checked once before the first read and before the detour (a
    // mismatch turns the feature off and logs WIND_REPLAY,phase=unproven,
    // site=...). The gate's own prologue is checked by its signature, found
    // before the detour is in (once hooked its first bytes are the detour's
    // jump).
    //
    // The weather pre-update (the weather entity's vtable slot +0x28).
    inline const sig::Signature kPreUpdate{"wind_preupdate",
        "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 41 54 41 55 41 56 41 57 48 81 EC 90 00 00 00 0F 29 70 C8 45 "
        "33 E4",
        0x31B8638};
    // The gate's arguments: mov r8d, 0x78 (the mask); &pos in rdx; lea rcx,
    // [the map data box streamer].
    inline const sig::Signature kGateArgs{"wind_gate_args", kPreUpdate, sig::Offset{0xBD},
        "41 B8 78 00 00 00 66 0F 7F 74 24 40 48 8D 54 24 40 0F 28 F8 48 8D 0D ?? ?? ?? ??", 0x31B86F5};
    inline const sig::Signature kMapStreamer{"wind_map_streamer", kGateArgs, sig::Rel32{0x17, 0x1B}, "", 0x5A8A000};
    // call the gate; test al, al; je to the simple path.
    inline const sig::Signature kGateCall{"wind_gate_call", kPreUpdate, sig::Offset{0xD8},
        "E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ??", 0x31B8710};
    // The gate: "is the world loaded about this position" of a box streamer
    // (builds the box pos +- [streamer+0x5C] and asks every class of the mask;
    // 45 callers in the build, two with this streamer and mask from the
    // audio: the weather pre-update, and another audio update, left alone).
    //   sub rsp,0x48; movdqu xmm2,[rdx] (only x, y, z used: w becomes the
    //   radius); lea rdx,[rsp+0x20]; movss xmm0,[rcx+0x5C]
    inline const sig::Signature kGate{"wind_gate", kGateCall, sig::Rel32{0x01, 0x05},
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 0F 10 41 5C 0F C6 C0 00",
        0x2B98AF8};
    // The simple path: the stamp (mov [rsi+0x50A4], ebp) and the update's
    // fifth argument 1.
    inline const sig::Signature kSimple{"wind_simple", kGateCall, sig::Rel32{0x09, 0x0D},
        "8B D5 89 AE A4 50 00 00", 0x31B8A0A};
    inline const sig::Signature kSimpleUpdate{"wind_simple_update", kSimple, sig::Offset{0x2B},
        "C6 44 24 20 01", 0x31B8A35};
    // The weather of the wind rows: the next index, the weather table, the
    // previous index, the wind block (W+0x1980), the stride and the hash.
    inline const sig::Signature kWeatherRows{"wind_weather_rows", kPreUpdate, sig::Offset{0xFF},
        "8B 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 44 8B 35 ?? ?? ?? ?? 4C 8D AE 80 19 00 00 49 69 CE E0 01 00 00 8B 5C "
        "11 10",
        0x31B8737};
    inline const sig::Signature kWeatherNext{"wind_weather_next", kWeatherRows, sig::Rel32{0x02, 0x06}, "", 0x4576208};
    inline const sig::Signature kWeatherTable{"wind_weather_table", kWeatherRows, sig::Rel32{0x09, 0x0D}, "",
        0x45722E0};
    inline const sig::Signature kWeatherPrev{"wind_weather_prev", kWeatherRows, sig::Rel32{0x10, 0x14}, "", 0x4576204};
    // The smoothed strength: movss [rdi+0x35C4], xmm2 (in the wind block).
    inline const sig::Signature kStrengthUpdate{"wind_strength_update",
        "48 8B C4 48 89 58 08 57 48 81 EC 80 00 00 00 0F 10 01", 0x31CCB68};
    inline const sig::Signature kStrength{"wind_strength", kStrengthUpdate, sig::Offset{0xCE},
        "F3 0F 11 97 C4 35 00 00", 0x31CCC36};
    // The wind rows' weather index: lea rax, [W+0x5098]; mov ebx, [next];
    // lea rcx, [W+0x5094]; cmove ebx, [previous]; cmp [rcx], ebx; and the
    // store mov [rax], ebx.
    inline const sig::Signature kLines{"wind_lines",
        "48 8B C4 48 89 58 18 4C 89 48 20 89 50 10 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 68 FC FF FF", 0x31CF2C0};
    inline const sig::Signature kLineIndices{"wind_line_indices", kLines, sig::Offset{0x7C},
        "48 8D 05 ?? ?? ?? ?? 8B 1D ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ??", 0x31CF33C};
    inline const sig::Signature kLineNext{"wind_line_next", kLineIndices, sig::Rel32{0x03, 0x07}, "", 0x5C95108};
    inline const sig::Signature kLineNextIndex{"wind_line_next_index", kLineIndices, sig::Rel32{0x09, 0x0D}, "",
        0x4576208};
    inline const sig::Signature kLinePrev{"wind_line_prev", kLineIndices, sig::Rel32{0x10, 0x14}, "", 0x5C95104};
    inline const sig::Signature kLinePrevRead{"wind_line_prev_read", kLines, sig::Offset{0xA4},
        "0F 44 1D ?? ?? ?? ??", 0x31CF364};
    inline const sig::Signature kLinePrevIndex{"wind_line_prev_index", kLinePrevRead, sig::Rel32{0x03, 0x07}, "",
        0x4576204};
    inline const sig::Signature kLineCompare{"wind_line_compare", kLines, sig::Offset{0xD3}, "39 19", 0x31CF393};
    inline const sig::Signature kLineStore{"wind_line_store", kLines, sig::Offset{0x139}, "89 18", 0x31CF3F9};
    // The weather entity's initializer: lea rcx, [W] (the static object),
    // lea rax, [its vtable].
    inline const sig::Signature kWeather{"wind_weather", kWeatherEntityInit, sig::Rel32{0x07, 0x0B}, "", 0x5C90070}; // W

    // The checked code by the names the log prints (site=...): each name is
    // a site of the table this module had, now inside the signature that
    // holds its bytes (the streamer's lea and the gate's prologue are not
    // sites: the log prints the one, the install checks the other).
    struct Site
    {
        const char* name;
        const sig::Signature* signature;
    };
    inline const Site kSites[] = {
        // the weather pre-update's gate: mask, &pos, streamer, call, test, je simple
        {"gate_mask", &kGateArgs},
        {"gate_pos", &kGateArgs},
        {"gate_call", &kGateCall},
        // the simple path: the stamp and the update's fifth argument 1
        {"simple_stamp", &kSimple},
        {"simple_update", &kSimpleUpdate},
        // the wind block and the smoothed strength
        {"wind_block", &kWeatherRows},
        {"wind_strength", &kStrength},
        // the wind rows' weather index
        {"line_next", &kLineNext},
        {"line_next_index", &kLineNextIndex},
        {"line_prev", &kLinePrev},
        {"line_prev_index", &kLinePrevIndex},
        {"line_compare", &kLineCompare},
        {"line_store", &kLineStore},
        // the weather entity (static object, its initializer's vtable) and the weather table
        {"weather_init", &kWeather},
        {"weather_vtable", &kWeatherEntityVtable},
        {"weather_next", &kWeatherNext},
        {"weather_table", &kWeatherTable},
        {"weather_prev", &kWeatherPrev},
        {"weather_stride", &kWeatherRows},
        {"weather_hash", &kWeatherRows},
    };
    constexpr std::size_t kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

    // ------------------------------------------------------------ the gate

    // Whether the detour asks about the replay's scene: only the weather
    // pre-update's call, only while a fresh scene is published.
    constexpr std::uint64_t kSceneStaleMs = 3000;
    inline bool SceneFresh(std::uint64_t publishedAtMs, std::uint64_t nowMs)
    {
        return publishedAtMs != 0 && nowMs >= publishedAtMs && nowMs - publishedAtMs <= kSceneStaleMs;
    }
    // `weatherReturn`: the weather pre-update's return address from the gate
    // (kGateCall + kGateCallLength in the game), 0 when not found.
    inline bool AskScene(std::uintptr_t returnAddress, std::uintptr_t weatherReturn, std::uint64_t publishedAtMs,
        std::uint64_t nowMs)
    {
        return weatherReturn && returnAddress == weatherReturn && SceneFresh(publishedAtMs, nowMs);
    }

    // Focus first (the streaming focus the replay set, where the map data is
    // loaded; live, the player is the engine's focus entity), then Camera, then
    // Puppet (the trail focus, up to 250 m from the camera: the gate may say no
    // there).
    enum class SceneSource : std::uint8_t { None = 0, Puppet, Camera, Focus };
    inline const char* SceneSourceName(SceneSource s)
    {
        switch (s)
        {
        case SceneSource::Puppet: return "puppet";
        case SceneSource::Camera: return "camera";
        case SceneSource::Focus: return "focus";
        default: return "none";
        }
    }

    // The detour's counters for one stretch (read and zeroed by the script
    // thread): the weather pre-update's calls answered about the replay's
    // scene and about the live player, and how many said "not loaded" (each
    // one a frame of the simple path, unless the fade or the third test
    // already sent it there).
    struct GateCounts
    {
        std::uint64_t sceneCalls = 0;
        std::uint64_t sceneUnloaded = 0;
        std::uint64_t liveCalls = 0;
        std::uint64_t liveUnloaded = 0;
        void Add(const GateCounts& o)
        {
            sceneCalls += o.sceneCalls;
            sceneUnloaded += o.sceneUnloaded;
            liveCalls += o.liveCalls;
            liveUnloaded += o.liveUnloaded;
        }
    };
    inline std::string GateText(const GateCounts& g)
    {
        char b[96];
        std::snprintf(b, sizeof(b), "scene:%llu/%llu,live:%llu/%llu", static_cast<unsigned long long>(g.sceneCalls),
            static_cast<unsigned long long>(g.sceneUnloaded), static_cast<unsigned long long>(g.liveCalls),
            static_cast<unsigned long long>(g.liveUnloaded));
        return b;
    }

    // ------------------------------------------------------------ the track

    struct Sample
    {
        std::uint32_t timeMs = 0;
        float strength = 0.0f;       // W+0x4F44
        std::uint32_t lines[2] = {}; // weather hash of the wind rows (0 unknown)
        std::uint8_t flags = 0;
    };
    constexpr std::uint8_t kSampleSimple = 1; // W+0x50A4 moved since the previous sample: the simple path ran
    constexpr std::uint8_t kSampleLines = 2;  // lines[] read
    constexpr std::uint8_t kSampleFlagsKnown = kSampleSimple | kSampleLines;

    // The recording's part (Recording::windAudio, clip section WNDA).
    struct Track
    {
        std::vector<Sample> samples; // time ascending

        void Clear() { samples.clear(); }
        bool Empty() const { return samples.empty(); }
        std::size_t ApproxBytes() const { return samples.capacity() * sizeof(Sample); }
        void DropBefore(std::uint32_t keepFromMs)
        {
            samples.erase(std::remove_if(samples.begin(), samples.end(),
                              [keepFromMs](const Sample& s) { return s.timeMs < keepFromMs; }),
                samples.end());
        }
        void CutInto(Track& out, std::uint32_t baseMs, std::uint32_t endMs) const
        {
            out.Clear();
            for (const Sample& s : samples)
            {
                if (s.timeMs < baseMs || s.timeMs > endMs)
                    continue;
                Sample copy = s;
                copy.timeMs -= baseMs;
                out.samples.push_back(copy);
            }
        }
    };

    // One sample per kSamplePeriodMs of clip time, recording and playback
    // (the strength is smoothed by 0.05 per audio frame: 250 ms resolves it).
    constexpr std::uint32_t kSamplePeriodMs = 250;
    // A recorded sample further than this from the playback's is not paired.
    constexpr std::uint32_t kMatchMs = 300;
    // The playback logs one line per block of clip time.
    constexpr std::uint32_t kBlockMs = 5000;
    constexpr std::uint32_t kLineCap = 80; // lines per playback

    inline bool SampleDue(const Track& track, std::uint32_t timeMs)
    {
        return track.samples.empty() || timeMs < track.samples.back().timeMs ||
            timeMs - track.samples.back().timeMs >= kSamplePeriodMs;
    }

    // Appends in time order (a clock that went back drops the newer tail,
    // the way a rewound recording overwrites).
    inline void Append(Track& track, const Sample& s)
    {
        while (!track.samples.empty() && track.samples.back().timeMs >= s.timeMs)
            track.samples.pop_back();
        track.samples.push_back(s);
    }

    // The recorded sample nearest to timeMs, null when none is within kMatchMs.
    inline const Sample* At(const Track& track, std::uint32_t timeMs)
    {
        const std::vector<Sample>& s = track.samples;
        if (s.empty())
            return nullptr;
        auto it = std::lower_bound(s.begin(), s.end(), timeMs, [](const Sample& x, std::uint32_t t) { return x.timeMs < t; });
        const Sample* best = nullptr;
        std::uint32_t bestGap = kMatchMs + 1;
        if (it != s.end())
        {
            best = &*it;
            bestGap = it->timeMs - timeMs;
        }
        if (it != s.begin())
        {
            const Sample& a = *(it - 1);
            const std::uint32_t gap = timeMs - a.timeMs;
            if (gap <= bestGap)
            {
                best = &a;
                bestGap = gap;
            }
        }
        return best && bestGap <= kMatchMs ? best : nullptr;
    }

    // ------------------------------------------------------------ names

    // A weather hash by its game name (GET_HASH_KEY of the type names), or hex.
    inline std::string WeatherText(std::uint32_t hash)
    {
        if (!hash)
            return "none";
        const int index = envoverride::WeatherIndex(hash);
        if (index >= 0)
            return envoverride::kWeathers[index].game;
        char b[16];
        std::snprintf(b, sizeof(b), "0x%08x", hash);
        return b;
    }
    inline std::string LinesText(const std::uint32_t lines[2], bool known)
    {
        if (!known)
            return "none";
        return WeatherText(lines[0]) + ";" + WeatherText(lines[1]);
    }

    // ------------------------------------------------------------ comparison

    // The wind strength is a curve of the wind bus's peak meter (0..1, the
    // engine caps it at 1 for the variable). Two values agree when they are
    // within kAbsTolerance, or within the ratio band for the louder ones.
    // In light snow the live strength is about 0.03 to 0.15, so a looser
    // tolerance (0.06) would pass a replay 2.5 times as loud (+8 dB); where both
    // passes ran the normal path they differ by at most 0.02.
    constexpr float kAbsTolerance = 0.03f;
    constexpr float kRatioLow = 0.75f;
    constexpr float kRatioHigh = 1.0f / 0.75f;
    inline bool Agree(float rec, float play)
    {
        if (!std::isfinite(rec) || !std::isfinite(play))
            return false;
        if (std::fabs(play - rec) <= kAbsTolerance)
            return true;
        if (rec <= 0.0f)
            return false;
        const float r = play / rec;
        return r >= kRatioLow && r <= kRatioHigh;
    }
    // play / rec as dB of amplitude, floored (a convenience for reading).
    inline float RatioDb(float rec, float play)
    {
        const float floor = 0.001f;
        return 20.0f * std::log10((std::max)(play, floor) / (std::max)(rec, floor));
    }

    // One block of playback samples and their recorded pairs.
    struct Block
    {
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
        std::uint32_t samples = 0; // playback samples
        std::uint32_t matched = 0; // of them with a recorded pair
        double play = 0.0;         // sum over all playback samples
        double playMatched = 0.0;  // sum over the paired ones
        double rec = 0.0;          // sum of the pairs
        std::uint32_t simplePlay = 0;
        std::uint32_t simpleRec = 0;
        std::uint32_t linesCompared = 0;
        std::uint32_t linesSame = 0;
        std::uint32_t playLines[2] = {};
        bool playLinesKnown = false;
        std::uint32_t recLines[2] = {};
        bool recLinesKnown = false;
        std::uint32_t wx[2] = {}; // the live weather (prev, next) at the last sample

        bool Empty() const { return samples == 0; }
        void Add(const Sample& p, const Sample* r, const std::uint32_t wxNow[2])
        {
            if (!samples)
                fromMs = p.timeMs;
            toMs = p.timeMs;
            ++samples;
            play += p.strength;
            if (p.flags & kSampleSimple)
                ++simplePlay;
            playLinesKnown = (p.flags & kSampleLines) != 0;
            playLines[0] = p.lines[0];
            playLines[1] = p.lines[1];
            wx[0] = wxNow ? wxNow[0] : 0;
            wx[1] = wxNow ? wxNow[1] : 0;
            if (!r)
                return;
            ++matched;
            playMatched += p.strength;
            rec += r->strength;
            if (r->flags & kSampleSimple)
                ++simpleRec;
            recLinesKnown = (r->flags & kSampleLines) != 0;
            recLines[0] = r->lines[0];
            recLines[1] = r->lines[1];
            if ((p.flags & kSampleLines) && (r->flags & kSampleLines))
            {
                ++linesCompared;
                if (p.lines[0] == r->lines[0] && p.lines[1] == r->lines[1])
                    ++linesSame;
            }
        }
        float PlayMean() const { return samples ? static_cast<float>(play / samples) : 0.0f; }
        float RecMean() const { return matched ? static_cast<float>(rec / matched) : 0.0f; }
        float PlayMatchedMean() const { return matched ? static_cast<float>(playMatched / matched) : 0.0f; }
        bool Agrees() const { return matched && Agree(RecMean(), PlayMatchedMean()); }
    };

    struct Totals
    {
        std::uint32_t samples = 0;
        std::uint32_t matched = 0;
        std::uint32_t blocks = 0;
        std::uint32_t blocksMatched = 0;
        std::uint32_t blocksAgree = 0;
        double play = 0.0;
        double playMatched = 0.0;
        double rec = 0.0;
        double sumAbsDiff = 0.0;      // over the paired samples
        float maxAbsDiff = 0.0f;      // block means
        std::uint32_t maxAtMs = 0;
        float maxRec = 0.0f, maxPlay = 0.0f;
        std::uint32_t simplePlay = 0;
        std::uint32_t simpleRec = 0;
        std::uint32_t linesCompared = 0;
        std::uint32_t linesSame = 0;

        void AddPair(float r, float p) { sumAbsDiff += std::fabs(p - r); }
        void AddBlock(const Block& b)
        {
            if (b.Empty())
                return;
            ++blocks;
            samples += b.samples;
            matched += b.matched;
            play += b.play;
            playMatched += b.playMatched;
            rec += b.rec;
            simplePlay += b.simplePlay;
            simpleRec += b.simpleRec;
            linesCompared += b.linesCompared;
            linesSame += b.linesSame;
            if (!b.matched)
                return;
            ++blocksMatched;
            if (b.Agrees())
                ++blocksAgree;
            const float d = std::fabs(b.PlayMatchedMean() - b.RecMean());
            if (d >= maxAbsDiff)
            {
                maxAbsDiff = d;
                maxAtMs = b.fromMs;
                maxRec = b.RecMean();
                maxPlay = b.PlayMatchedMean();
            }
        }
    };

    inline std::string F3(float v)
    {
        char b[24];
        std::snprintf(b, sizeof(b), "%.3f", std::isfinite(v) ? v : 0.0f);
        return b;
    }
    inline std::string F1(float v)
    {
        char b[24];
        std::snprintf(b, sizeof(b), "%.1f", std::isfinite(v) ? v : 0.0f);
        return b;
    }

    // WIND_REPLAY,phase=play: one block, the recorded value against the one
    // the engine computed in the replay.
    inline std::string BlockLine(const Block& b, bool recorded, const GateCounts& gate, SceneSource source)
    {
        std::string line = "WIND_REPLAY,phase=play,from_ms=" + std::to_string(b.fromMs) + ",to_ms=" + std::to_string(b.toMs) +
            ",samples=" + std::to_string(b.samples);
        if (recorded && b.matched)
        {
            line += ",rec_str=" + F3(b.RecMean()) + ",play_str=" + F3(b.PlayMatchedMean()) +
                ",diff=" + F3(b.PlayMatchedMean() - b.RecMean()) + ",ratio_db=" + F1(RatioDb(b.RecMean(), b.PlayMatchedMean())) +
                ",agree=" + (b.Agrees() ? "1" : "0") + ",paired=" + std::to_string(b.matched);
        }
        else
            line += std::string(",rec_str=none,play_str=") + F3(b.PlayMean());
        line += ",rec_lines=" + (recorded && b.matched ? LinesText(b.recLines, b.recLinesKnown) : std::string("none")) +
            ",play_lines=" + LinesText(b.playLines, b.playLinesKnown) + ",wx=" + WeatherText(b.wx[0]) + ";" + WeatherText(b.wx[1]) +
            ",simple_rec=" + std::to_string(b.simpleRec) + ",simple_play=" + std::to_string(b.simplePlay) +
            ",gate=" + GateText(gate) + ",scene=" + SceneSourceName(source);
        return line;
    }

    inline std::string TotalLine(const Totals& t, bool recorded, std::size_t recordedSamples, bool fix, const GateCounts& gate)
    {
        std::string line = "WIND_REPLAY_TOTAL,fix=" + std::string(fix ? "1" : "0") + ",recorded=" + (recorded ? "1" : "0") +
            ",rec_samples=" + std::to_string(recordedSamples) + ",samples=" + std::to_string(t.samples) +
            ",paired=" + std::to_string(t.matched) + ",blocks=" + std::to_string(t.blocks);
        if (recorded && t.matched)
        {
            line += ",rec_mean=" + F3(static_cast<float>(t.rec / t.matched)) +
                ",play_mean=" + F3(static_cast<float>(t.playMatched / t.matched)) +
                ",mean_abs_diff=" + F3(static_cast<float>(t.sumAbsDiff / t.matched)) +
                ",blocks_agree=" + std::to_string(t.blocksAgree) + "/" + std::to_string(t.blocksMatched) +
                ",worst_block_ms=" + std::to_string(t.maxAtMs) + ",worst_rec=" + F3(t.maxRec) + ",worst_play=" + F3(t.maxPlay);
        }
        else
            line += ",play_mean=" + F3(t.samples ? static_cast<float>(t.play / t.samples) : 0.0f);
        line += ",lines_same=" + std::to_string(t.linesSame) + "/" + std::to_string(t.linesCompared) +
            ",simple_rec=" + std::to_string(t.simpleRec) + ",simple_play=" + std::to_string(t.simplePlay) + ",gate=" + GateText(gate);
        return line;
    }
}
