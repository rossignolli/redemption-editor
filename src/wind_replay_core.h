#pragma once

// The audio wind in the replay, pure part.
//
// Engine facts (build 1491.50):
// - The weather audio entity W (0x5C90070, vtable 0x383D3B0) pre-updates
//   through its vtable slot +0x28, 0x31B8638. It reads the local player
//   (the obfuscated cells 0x4A08D58/60/68), his position (+0x70), and asks
//   the map data box streamer 0x5A8A000 whether the world is loaded about
//   that position: 0x2B98AF8(streamer, &pos, 0x78), call at 0x31B8710. On
//   false (0x31B8717 je 0x31B8A0A), or with the fade 0x173E80 >= 0.9, or
//   [0x5C0FBE0] != 0, it takes the SIMPLE path: W+0x50A4 = time (0x31B8A0C)
//   and the wind update 0x31CF078 with the fifth argument 1 (0x31B8A35),
//   which sets the speaker levels of the wind slots to {1,1,1,1} and never
//   calls 0x31CF2C0, the function that swaps the wind slots to the wind
//   sounds of the current weather when it changes (it compares the weather
//   index each row was started with, W+0x5094 / W+0x5098, absolute
//   0x5C95104 / 0x5C95108, with [0x4576204] / [0x4576208] at 0x31CF393 and
//   stores the new one at 0x31CF3F9).
// - In a mission replay the live player stays hidden where he was (the
//   follow is skipped) while the streaming focus goes to the replay camera,
//   possibly hundreds of metres away: the streamer unloads his area, the
//   gate answers false and the replay's wind freezes on the slots started
//   in its first frames, at full level on the four speakers, whatever the
//   recorded weather does next.
// - wind_str is not an input: it is the curve (block+0x3560, hash
//   0x87AA362D) of the peak meter of the "Wind" submix (0x31CCBC6 reads the
//   submix 0x252C054(0x5342260); index +0x11C7 written at 0x25303FE for the
//   string "Wind"), smoothed by 0.05 and stored at 0x31CCC36: the measured
//   loudness of the wind bus itself.
//
// The fix: while the replay owns the world, the weather gate's question is
// asked about the recorded scene instead of the hidden live player. A detour
// of 0x2B98AF8 (prologue checked) forwards every call, and only for the call
// from the weather pre-update (return address 0x31B8715) during playback it
// passes the replay's streaming focus (live, the player is the engine's focus
// entity), else the rendered camera, else the trail focus puppet. Same
// streamer, same mask: the engine answers whether the world is loaded there
// and computes the wind as it did live. No wind value is written.
//
// Evidence (clip section WNDA, optional): the recording keeps, 4 times a
// second, the audio wind strength (W+0x4F44), the weather each wind row plays
// (W+0x5094/W+0x5098 through the weather table) and whether the simple path
// ran (W+0x50A4 moved). The playback reads the same and logs the recorded
// value against the one the engine computed (WIND_REPLAY, every 5 s of clip
// time, and WIND_REPLAY_TOTAL). A clip without the section still gets the
// fix; its lines say rec=none.
//
// Pure: unit tested in wind_replay_tests.cpp.

#include "env_override.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace replay::windreplay
{
    // ------------------------------------------------------------ layout

    // The gate: "is the world loaded about this position" of a box streamer
    // (builds the box pos +- [streamer+0x5C] and asks every class of the mask,
    // 0x2B98B3C; 45 callers in the build, two with this streamer and mask
    // from the audio: 0x31B8710 the weather pre-update, 0x31E7174 another
    // audio update 0x31E70D0, left alone).
    constexpr std::uint32_t kGateRva = 0x2B98AF8;
    constexpr std::uint8_t kGateSignature[] = {
        0x48, 0x83, 0xEC, 0x48,                   // sub rsp,0x48
        0xF3, 0x0F, 0x6F, 0x12,                   // movdqu xmm2,[rdx]   (only x, y, z used: w becomes the radius)
        0x48, 0x8D, 0x54, 0x24, 0x20,             // lea rdx,[rsp+0x20]
        0xF3, 0x0F, 0x10, 0x41, 0x5C};            // movss xmm0,[rcx+0x5C]
    constexpr std::uint32_t kWeatherGateCallRva = 0x31B8710;
    constexpr std::uint32_t kWeatherGateReturnRva = 0x31B8715;
    constexpr std::uint32_t kWeatherSimpleRva = 0x31B8A0A;
    constexpr std::uint32_t kOtherGateReturnRva = 0x31E7179; // 0x31E70D0 (not changed)
    constexpr std::uint32_t kMapStreamerRva = 0x5A8A000;
    constexpr std::uint32_t kGateMask = 0x78;
    constexpr std::uint32_t kPreUpdateRva = 0x31B8638; // vtable 0x383D3B0 slot +0x28

    constexpr std::uint32_t kWeatherRva = 0x5C90070;
    constexpr std::uint32_t kWeatherVtableRva = 0x383D3B0;
    constexpr std::uint32_t kWindStrength = 0x4F44;    // W+0x1980+0x35C4 (0x31B874B, 0x31CCC36)
    constexpr std::uint32_t kSimpleStamp = 0x50A4;     // u32 time, written only by the simple path (0x31B8A0C)
    constexpr std::uint32_t kWindLinePrev = 0x5094;    // u32 weather index the row 0 slots were started with
    constexpr std::uint32_t kWindLineNext = 0x5098;    // row 1
    constexpr std::uint32_t kWeatherPrevRva = 0x4576204;
    constexpr std::uint32_t kWeatherNextRva = 0x4576208;
    constexpr std::uint32_t kWeatherTableRva = 0x45722E0;
    constexpr std::uint32_t kWeatherTypeStride = 0x1E0;
    constexpr std::uint32_t kWeatherTypeHash = 0x10;
    constexpr std::uint32_t kMaxWeatherTypes = 256;

    // ------------------------------------------------------------ sites

    // Code bytes of build 1491.50 checked once before the first read and
    // before the detour (a mismatch turns the feature off and logs
    // WIND_REPLAY,phase=unproven,site=...). The gate's own prologue is
    // checked by the install (kGateSignature): once hooked its first bytes
    // are the detour's jump, so it is not in this table.
    struct Site
    {
        const char* name;
        std::uint32_t rva;
        std::uint8_t size;
        std::uint8_t bytes[20];
    };
    constexpr Site kSites[] = {
        // the weather pre-update's gate: mask, &pos, streamer, call, test, je simple
        {"gate_mask", 0x31B86F5, 6, {0x41, 0xB8, 0x78, 0x00, 0x00, 0x00}},
        {"gate_pos", 0x31B86FB, 11, {0x66, 0x0F, 0x7F, 0x74, 0x24, 0x40, 0x48, 0x8D, 0x54, 0x24, 0x40}},
        {"gate_call", 0x31B8709, 20,
            {0x48, 0x8D, 0x0D, 0xF0, 0x18, 0x8D, 0x02, 0xE8, 0xE3, 0x03, 0x9E, 0xFF, 0x84, 0xC0, 0x0F, 0x84, 0xED, 0x02, 0x00, 0x00}},
        // the simple path: the stamp and the update's fifth argument 1
        {"simple_stamp", 0x31B8A0A, 8, {0x8B, 0xD5, 0x89, 0xAE, 0xA4, 0x50, 0x00, 0x00}},
        {"simple_update", 0x31B8A35, 5, {0xC6, 0x44, 0x24, 0x20, 0x01}},
        // the wind block and the smoothed strength
        {"wind_block", 0x31B874B, 7, {0x4C, 0x8D, 0xAE, 0x80, 0x19, 0x00, 0x00}},
        {"wind_strength", 0x31CCC36, 8, {0xF3, 0x0F, 0x11, 0x97, 0xC4, 0x35, 0x00, 0x00}},
        // the wind rows' weather index (0x31CF2C0)
        {"line_next", 0x31CF33C, 7, {0x48, 0x8D, 0x05, 0xC5, 0x5D, 0xAC, 0x02}},
        {"line_next_index", 0x31CF343, 6, {0x8B, 0x1D, 0xBF, 0x6E, 0x3A, 0x01}},
        {"line_prev", 0x31CF349, 7, {0x48, 0x8D, 0x0D, 0xB4, 0x5D, 0xAC, 0x02}},
        {"line_prev_index", 0x31CF364, 7, {0x0F, 0x44, 0x1D, 0x99, 0x6E, 0x3A, 0x01}},
        {"line_compare", 0x31CF393, 2, {0x39, 0x19}},
        {"line_store", 0x31CF3F9, 2, {0x89, 0x18}},
        // the weather entity (static object, its constructor's vtable) and the weather table
        {"weather_init", 0x112D90, 7, {0x48, 0x8D, 0x0D, 0xD9, 0xD2, 0xB7, 0x05}},
        {"weather_vtable", 0x112D9C, 7, {0x48, 0x8D, 0x05, 0x0D, 0xA6, 0x72, 0x03}},
        {"weather_next", 0x31B8737, 6, {0x8B, 0x05, 0xCB, 0xDA, 0x3B, 0x01}},
        {"weather_table", 0x31B873D, 7, {0x48, 0x8D, 0x15, 0x9C, 0x9B, 0x3B, 0x01}},
        {"weather_prev", 0x31B8744, 7, {0x44, 0x8B, 0x35, 0xB9, 0xDA, 0x3B, 0x01}},
        {"weather_stride", 0x31B8752, 7, {0x49, 0x69, 0xCE, 0xE0, 0x01, 0x00, 0x00}},
        {"weather_hash", 0x31B8759, 4, {0x8B, 0x5C, 0x11, 0x10}},
    };
    constexpr std::size_t kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

    constexpr std::size_t SiteIndex(const char* name)
    {
        for (std::size_t n = 0; n < kSiteCount; ++n)
        {
            const char* a = kSites[n].name;
            const char* b = name;
            while (*a && *a == *b)
            {
                ++a;
                ++b;
            }
            if (*a == *b)
                return n;
        }
        return kSiteCount;
    }
    constexpr std::int32_t Rel32(const Site& s, std::uint32_t at)
    {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(s.bytes[at]) | (static_cast<std::uint32_t>(s.bytes[at + 1]) << 8) |
            (static_cast<std::uint32_t>(s.bytes[at + 2]) << 16) | (static_cast<std::uint32_t>(s.bytes[at + 3]) << 24));
    }
    // The rip-relative target of the instruction that starts at `at` in the
    // site, `len` bytes long, its displacement at `disp`.
    constexpr std::uint32_t RipOf(const char* name, std::uint32_t at, std::uint32_t len, std::uint32_t disp)
    {
        return static_cast<std::uint32_t>(static_cast<std::int64_t>(kSites[SiteIndex(name)].rva) + at + len +
            Rel32(kSites[SiteIndex(name)], at + disp));
    }
    static_assert(SiteIndex("no_such_site") == kSiteCount, "site lookup");
    static_assert(RipOf("gate_call", 0, 7, 3) == kMapStreamerRva, "lea rcx, streamer");
    static_assert(RipOf("gate_call", 7, 5, 1) == kGateRva, "call the gate");
    static_assert(kSites[SiteIndex("gate_call")].rva + 12 == kWeatherGateReturnRva, "the call's return address");
    static_assert(kSites[SiteIndex("gate_call")].rva + 7 == kWeatherGateCallRva, "the call");
    static_assert(RipOf("gate_call", 14, 6, 2) == kWeatherSimpleRva, "je to the simple path");
    static_assert(kSites[SiteIndex("simple_stamp")].rva == kWeatherSimpleRva, "the simple path starts with the stamp");
    static_assert(kSites[SiteIndex("simple_stamp")].bytes[4] == (kSimpleStamp & 0xFF) &&
            kSites[SiteIndex("simple_stamp")].bytes[5] == (kSimpleStamp >> 8),
        "mov [rsi+0x50A4],ebp");
    static_assert(kSites[SiteIndex("gate_mask")].bytes[2] == kGateMask, "mov r8d,0x78");
    static_assert(0x1980 + 0x35C4 == kWindStrength, "block + strength");
    static_assert(RipOf("line_next", 0, 7, 3) == kWeatherRva + kWindLineNext, "row 1 index (0x5C95108)");
    static_assert(RipOf("line_prev", 0, 7, 3) == kWeatherRva + kWindLinePrev, "row 0 index (0x5C95104)");
    static_assert(RipOf("line_next_index", 0, 6, 2) == kWeatherNextRva, "next weather index");
    static_assert(RipOf("line_prev_index", 0, 7, 3) == kWeatherPrevRva, "previous weather index");
    static_assert(RipOf("weather_init", 0, 7, 3) == kWeatherRva, "weather entity");
    static_assert(RipOf("weather_vtable", 0, 7, 3) == kWeatherVtableRva, "weather vtable");
    static_assert(RipOf("weather_next", 0, 6, 2) == kWeatherNextRva, "weather next");
    static_assert(RipOf("weather_prev", 0, 7, 3) == kWeatherPrevRva, "weather prev");
    static_assert(RipOf("weather_table", 0, 7, 3) == kWeatherTableRva, "weather table");

    inline bool SiteMatches(const Site& site, const std::uint8_t* code)
    {
        if (!code)
            return false;
        for (std::uint8_t n = 0; n < site.size; ++n)
            if (code[n] != site.bytes[n])
                return false;
        return true;
    }

    // ------------------------------------------------------------ the gate

    // Whether the detour asks about the replay's scene: only the weather
    // pre-update's call, only while a fresh scene is published.
    constexpr std::uint64_t kSceneStaleMs = 3000;
    inline bool SceneFresh(std::uint64_t publishedAtMs, std::uint64_t nowMs)
    {
        return publishedAtMs != 0 && nowMs >= publishedAtMs && nowMs - publishedAtMs <= kSceneStaleMs;
    }
    inline bool AskScene(std::uint32_t returnRva, std::uint64_t publishedAtMs, std::uint64_t nowMs)
    {
        return returnRva == kWeatherGateReturnRva && SceneFresh(publishedAtMs, nowMs);
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
