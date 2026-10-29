#pragma once

// The state of the audio engine's sound categories recorded and put back in
// the replay (clip section CATS): record the engine state the mix depends on
// and restore it, applied to the category stage of every voice. Pure part (no
// game); unit tested in tests/category_state_tests.cpp.
//
// Engine layout (build 1491.50):
//   - the resolved volume of category i (table [0x5338488], stride 0x48, +0
//     in dB, what the volume probe calls cat_db) is written by the resolver
//     0x251C310(category, u16, use meta) on the mixer side: meta (+0, only
//     with the third argument set) plus the controllers' sum (+0x48, read at
//     0x251C348) plus the parent's resolved value (+0xB0 the parent's index,
//     unless +0xE4 bit 0), stored at the category's own index (+0xB2,
//     0x251C374..0x251C380). It is called for the root by 0x2539654 at
//     0x25396AD (use meta 1), for each dynamic hierarchy at 0x2539714 and
//     recursively for the children at 0x251C9C8, so a parent's value is
//     final before its children read it;
//   - the controllers' sum of every static category ([0x5338630] of them, the
//     category pool [0x5338490]: +8 base, +0x10 count, +0x20 stride, +0x38
//     the allocated bits) is written by the controller manager 0x25394C8
//     (manager [0x5338518], game side, its own lock +0x840): +0x48 volume dB,
//     +0x4C and +0x54 (multipliers), +0x88 pitch, +0x8A low pass, +0x8C high
//     pass (0x25395BA..0x25395E7), and 0, 1, 1, 0, 20000, 0 for a category
//     without a controller (0x25395F0..0x253960C). A controller comes from a
//     mixer scene's patch (0x3278B6C, 0x32756F4 -> 0x2526258) or from code:
//     the sliders SFX_SLIDER and MUSIC_SLIDER and the music categories
//     INTERACTIVE_MUSIC and MUSIC_ONESHOT (0x2EFF0DC -> 0x2526258);
//   - a category by hash: the map at 0x5338590 (+0xA4 count, +0xA8 entries of
//     16 bytes {category pointer, hash}, sorted by hash, 0x252A3D0); the
//     pool index of a category is (pointer - base) / stride (0x252634D).
// Without this, the category stage (d_cat) differs from the recording by
// about +4 dB on scripted speech and +2 to +6 dB on ambient speech, ambience
// and weather, from mixer scenes the replay session has and the recording did
// not (and the other way round): the conversation duck, the hidden live
// player's state scenes, duplicated permanent scenes.
//
// The mechanism: the recording keeps, for every static category (by hash),
// the values the controller manager wrote (first sight and every change, at
// most once per kMinIntervalMs per category); the playback, while the replay
// owns the world, puts the recorded volume (or every field) into the
// category right before the resolver reads it (a detour of 0x251C310 that
// forwards every call), so the resolved value is the recording's whatever
// scenes the session has. The sliders, the music categories and BASE stay
// live (CategoryStateKeepLive).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace replay::catstate
{
    // ---- engine layout (build 1491.50) ----
    constexpr std::uint32_t kResolverRva = 0x251C310;      // (category, u16, use meta)
    constexpr std::uint32_t kResolvedTableRva = 0x5338488; // [ ]: resolved values, stride 0x48
    constexpr std::uint32_t kStaticCountRva = 0x5338630;   // u32: static categories
    constexpr std::uint32_t kCategoryPoolRva = 0x5338490;  // [ ]: the category pool
    constexpr std::uint32_t kCategoryMapRva = 0x5338590;   // the map object (hash -> category)
    constexpr std::uint32_t kMapLookupRva = 0x252A3D0;

    constexpr std::uint32_t kResolvedStride = 0x48;
    constexpr std::uint32_t kCatMeta = 0x00;     // f32 dB (metadata)
    constexpr std::uint32_t kCatVolume = 0x48;   // f32 dB (controllers' sum)
    constexpr std::uint32_t kCatRolloff = 0x4C;  // f32
    constexpr std::uint32_t kCatX54 = 0x54;      // f32
    constexpr std::uint32_t kCatPitch = 0x88;    // s16 cents
    constexpr std::uint32_t kCatLpf = 0x8A;      // u16 Hz
    constexpr std::uint32_t kCatHpf = 0x8C;      // u16 Hz
    constexpr std::uint32_t kCatParent = 0xB0;   // s16 (-1 none)
    constexpr std::uint32_t kCatIndex = 0xB2;    // s16 (its own index)
    constexpr std::uint32_t kCatFlags = 0xE4;    // bit 0: the parent's volume is not added
    constexpr std::uint32_t kCatBlockFrom = kCatVolume;
    constexpr std::uint32_t kCatBlockSize = kCatHpf + 2 - kCatVolume; // 0x48..0x8D
    constexpr std::uint32_t kMapCount = 0xA4;
    constexpr std::uint32_t kMapEntries = 0xA8;
    constexpr std::uint32_t kMapEntryStride = 16; // {pointer, hash at +8}
    constexpr std::uint32_t kPoolBase = 0x08;
    constexpr std::uint32_t kPoolCount = 0x10;
    constexpr std::uint32_t kPoolStride = 0x20;
    constexpr std::uint32_t kPoolBits = 0x38;

    constexpr std::size_t kMaxCategories = 4096; // indices the snapshot covers (static count above: refused)
    constexpr std::size_t kMaxMapEntries = 8192;

    // The resolver's prologue up to the first read of +0x48 (61 bytes):
    // mov rax,rsp; four saves; push r14; sub rsp,20h; movzx r11d,[rcx+0B0h];
    // mov sil,r8b; movzx ebp,dx; mov r9,rcx; test r8b,r8b; je; movss
    // xmm0,[rcx]; jmp; xorps; addss xmm0,[rcx+48h].
    constexpr std::uint8_t kResolverSignature[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10,
        0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x0F, 0xB7, 0x99, 0xB0,
        0x00, 0x00, 0x00, 0x41, 0x8A, 0xF0, 0x0F, 0xB7, 0xEA, 0x4C, 0x8B, 0xC9, 0x45, 0x84, 0xC0, 0x74, 0x06, 0xF3, 0x0F,
        0x10, 0x01, 0xEB, 0x03, 0x0F, 0x57, 0xC0, 0xF3, 0x0F, 0x58, 0x41, 0x48};

    // A code site: its bytes, and for a rip-relative operand or a call the
    // offset of the disp32 inside the bytes, where that instruction ends and
    // the target it must reach (ripAt < 0: bytes only).
    struct Site
    {
        const char* name;
        std::uint32_t rva;
        std::uint8_t size;
        std::uint8_t bytes[17];
        std::int8_t ripAt;
        std::uint8_t insnEnd;
        std::uint32_t target;
    };

    inline std::uint32_t SiteTarget(const Site& site)
    {
        if (site.ripAt < 0)
            return 0;
        const std::uint8_t* d = site.bytes + site.ripAt;
        const std::int32_t disp = static_cast<std::int32_t>(static_cast<std::uint32_t>(d[0]) |
            (static_cast<std::uint32_t>(d[1]) << 8) | (static_cast<std::uint32_t>(d[2]) << 16) |
            (static_cast<std::uint32_t>(d[3]) << 24));
        return static_cast<std::uint32_t>(static_cast<std::int64_t>(site.rva) + site.insnEnd + disp);
    }

    inline bool SiteMatches(const Site& site, const std::uint8_t* code)
    {
        for (std::uint8_t n = 0; n < site.size; ++n)
            if (code[n] != site.bytes[n])
                return false;
        return true;
    }

    constexpr Site kSites[] = {
        {"resolved_table", 0x251C34D, 7, {0x48, 0x8B, 0x15, 0x34, 0xC1, 0xE1, 0x02}, 3, 7, kResolvedTableRva},
        {"resolved_index_store", 0x251C374, 17,
            {0x49, 0x0F, 0xBF, 0x81, 0xB2, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x0C, 0xC0, 0xF3, 0x0F, 0x11, 0x04, 0xCA}, -1, 0, 0},
        {"call_root", 0x25396AD, 5, {0xE8, 0x5E, 0x2C, 0xFE, 0xFF}, 1, 5, kResolverRva},
        {"call_dynamic", 0x2539714, 5, {0xE8, 0xF7, 0x2B, 0xFE, 0xFF}, 1, 5, kResolverRva},
        {"call_children", 0x251C9C8, 5, {0xE8, 0x43, 0xF9, 0xFF, 0xFF}, 1, 5, kResolverRva},
        {"static_count", 0x25394E5, 7, {0x44, 0x8B, 0x35, 0x44, 0xF1, 0xDF, 0x02}, 3, 7, kStaticCountRva},
        {"category_pool", 0x253950D, 7, {0x4C, 0x8B, 0x15, 0x7C, 0xEF, 0xDF, 0x02}, 3, 7, kCategoryPoolRva},
        {"controller_volume_store", 0x25395BA, 5, {0xF3, 0x0F, 0x11, 0x43, 0x48}, -1, 0, 0},
        {"map_object", 0x2526298, 7, {0x48, 0x8D, 0x0D, 0xF1, 0x22, 0xE1, 0x02}, 3, 7, kCategoryMapRva},
        {"map_lookup_call", 0x252629F, 5, {0xE8, 0x2C, 0x41, 0x00, 0x00}, 1, 5, kMapLookupRva},
        {"map_count", 0x252A3D5, 7, {0x44, 0x8B, 0x81, 0xA4, 0x00, 0x00, 0x00}, -1, 0, 0},
        {"map_entries", 0x252A3F5, 7, {0x48, 0x8B, 0x99, 0xA8, 0x00, 0x00, 0x00}, -1, 0, 0},
        {"map_hash", 0x252A40C, 5, {0x44, 0x3B, 0x5C, 0xC3, 0x08}, -1, 0, 0},
        {"map_pointer", 0x252A429, 4, {0x4C, 0x8B, 0x14, 0xCB}, -1, 0, 0},
        {"pool_base", 0x252634D, 4, {0x4C, 0x2B, 0x59, 0x08}, -1, 0, 0},
        {"pool_stride", 0x2526354, 4, {0x48, 0xF7, 0x71, 0x20}, -1, 0, 0},
        {"pool_bits", 0x253968B, 4, {0x48, 0x8B, 0x41, 0x38}, -1, 0, 0},
    };
    constexpr std::size_t kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

    // ---- the values ----

    // What the controller manager writes into a category (0x25394C8).
    struct Values
    {
        float volume = 0.0f;  // +0x48 dB
        float rolloff = 1.0f; // +0x4C
        float x54 = 1.0f;     // +0x54
        std::int16_t pitch = 0;
        std::uint16_t lpf = 20000;
        std::uint16_t hpf = 0;
    };

    inline bool SameValues(const Values& a, const Values& b)
    {
        return a.volume == b.volume && a.rolloff == b.rolloff && a.x54 == b.x54 && a.pitch == b.pitch && a.lpf == b.lpf &&
            a.hpf == b.hpf;
    }

    inline bool PlausibleValues(const Values& v)
    {
        return std::isfinite(v.volume) && std::isfinite(v.rolloff) && std::isfinite(v.x54) && v.volume >= -200.0f &&
            v.volume <= 100.0f;
    }

    // A filter moved by more than 1 % of the larger value.
    inline bool FilterMoved(std::uint16_t a, std::uint16_t b)
    {
        const int big = (std::max)(static_cast<int>((std::max)(a, b)), 1);
        return std::abs(static_cast<int>(a) - static_cast<int>(b)) * 100 > big;
    }

    // The recorder's change test: volume beyond 0.05 dB, a multiplier beyond
    // 0.01, any pitch change, a filter beyond 1 %.
    constexpr float kVolumeStepDb = 0.05f;
    constexpr float kFactorStep = 0.01f;
    inline bool Changed(const Values& a, const Values& b)
    {
        return std::fabs(a.volume - b.volume) > kVolumeStepDb || std::fabs(a.rolloff - b.rolloff) > kFactorStep ||
            std::fabs(a.x54 - b.x54) > kFactorStep || a.pitch != b.pitch || FilterMoved(a.lpf, b.lpf) ||
            FilterMoved(a.hpf, b.hpf);
    }

    struct Sample
    {
        std::uint32_t timeMs = 0;
        std::uint32_t hash = 0;
        Values v;
    };
    constexpr std::size_t kSampleBytes = 4 + 4 + 4 + 4 + 4 + 2 + 2 + 2;
    constexpr std::size_t kMaxSamples = 4000000;
    constexpr std::uint32_t kMinIntervalMs = 50; // per category
    constexpr std::uint32_t kScanIntervalMs = 50; // the recording reads the categories at most 20 times a second

    // The recording's part (Recording::categoryState, clip section CATS).
    struct Track
    {
        std::uint32_t staticCount = 0; // the session's static categories when recorded
        std::uint32_t mapCount = 0;    // entries of the hash map
        std::vector<Sample> samples;   // time ascending

        void Clear()
        {
            staticCount = 0;
            mapCount = 0;
            samples.clear();
        }
        bool Empty() const { return samples.empty(); }
        std::size_t ApproxBytes() const { return samples.capacity() * sizeof(Sample); }

        std::size_t Categories() const
        {
            std::vector<std::uint32_t> seen;
            for (const Sample& s : samples)
                seen.push_back(s.hash);
            std::sort(seen.begin(), seen.end());
            return static_cast<std::size_t>(std::unique(seen.begin(), seen.end()) - seen.begin());
        }

        bool Valid() const
        {
            if (samples.size() > kMaxSamples)
                return false;
            std::uint32_t last = 0;
            for (const Sample& s : samples)
            {
                if (s.timeMs < last || !s.hash || !PlausibleValues(s.v))
                    return false;
                last = s.timeMs;
            }
            return true;
        }

        // The ring drops what is older than keepFromMs; per category the
        // value in force at keepFromMs stays, moved to keepFromMs.
        // The samples are in time order (Offer only appends, a clock that
        // went back pops the tail), so the dropped part is a prefix and the
        // moved values go in front of the rest, already in order: no sort of
        // the whole ring, which would stall the script thread as it grows.
        void DropBefore(std::uint32_t keepFromMs)
        {
            const auto firstKept = std::lower_bound(samples.begin(), samples.end(), keepFromMs,
                [](const Sample& s, std::uint32_t t) { return s.timeMs < t; });
            if (firstKept == samples.begin())
                return;
            std::map<std::uint32_t, Sample> last; // hash -> its last sample before keepFromMs
            for (auto it = samples.begin(); it != firstKept; ++it)
                last[it->hash] = *it;
            std::vector<Sample> moved;
            moved.reserve(last.size());
            for (const auto& entry : last)
            {
                Sample m = entry.second;
                m.timeMs = keepFromMs;
                moved.push_back(m);
            }
            samples.erase(samples.begin(), firstKept);
            samples.insert(samples.begin(), moved.begin(), moved.end());
        }

        // The window [baseMs, endMs] as a clip of its own: each category's
        // value in force at baseMs at 0, then the changes inside the window.
        void CutInto(Track& out, std::uint32_t baseMs, std::uint32_t endMs) const
        {
            out.Clear();
            out.staticCount = staticCount;
            out.mapCount = mapCount;
            std::map<std::uint32_t, Values> at;
            for (const Sample& s : samples)
                if (s.timeMs <= baseMs)
                    at[s.hash] = s.v;
            for (const auto& entry : at)
                out.samples.push_back(Sample{0, entry.first, entry.second});
            for (const Sample& s : samples)
                if (s.timeMs > baseMs && s.timeMs <= endMs)
                    out.samples.push_back(Sample{s.timeMs - baseMs, s.hash, s.v});
        }
    };

    // ---- the recorder's bookkeeping ----

    struct Recorder
    {
        struct Last
        {
            Values v;
            std::uint32_t storedMs = 0;
        };
        std::map<std::uint32_t, Last> last; // hash -> last stored

        void Reset() { last.clear(); }

        // Stores the category's values when first seen or changed (beyond
        // the steps of Changed, at most once per kMinIntervalMs; a change
        // inside the interval is stored at the first offer after it, since
        // the test is against the last stored value). A clock that went back
        // drops the newer tail (a rewound recording overwrites).
        bool Offer(Track& track, std::uint32_t hash, const Values& v, std::uint32_t nowMs)
        {
            if (!hash || !PlausibleValues(v) || track.samples.size() >= kMaxSamples)
                return false;
            if (!track.samples.empty() && track.samples.back().timeMs > nowMs)
            {
                while (!track.samples.empty() && track.samples.back().timeMs > nowMs)
                    track.samples.pop_back();
                last.clear(); // every category is stored again from here
            }
            const auto found = last.find(hash);
            if (found != last.end())
            {
                if (!Changed(found->second.v, v))
                    return false;
                if (nowMs >= found->second.storedMs && nowMs - found->second.storedMs < kMinIntervalMs)
                    return false;
            }
            track.samples.push_back(Sample{nowMs, hash, v});
            last[hash] = Last{v, nowMs};
            return true;
        }
    };

    // ---- the playback's index ----

    struct Series
    {
        std::uint32_t hash = 0;
        std::vector<std::pair<std::uint32_t, Values>> points; // time ascending
    };

    // The recorded values by category, built once per playback.
    struct PlayIndex
    {
        std::vector<Series> series; // by hash

        void Build(const Track& track)
        {
            series.clear();
            std::map<std::uint32_t, std::size_t> at;
            for (const Sample& s : track.samples)
            {
                auto found = at.find(s.hash);
                if (found == at.end())
                {
                    found = at.emplace(s.hash, series.size()).first;
                    series.push_back(Series{s.hash, {}});
                }
                series[found->second].points.emplace_back(s.timeMs, s.v);
            }
        }

        // The value in force at timeMs (the first one before the first
        // sample: a cut clip starts every category at 0).
        static const Values* At(const Series& s, std::uint32_t timeMs)
        {
            if (s.points.empty())
                return nullptr;
            auto it = std::upper_bound(s.points.begin(), s.points.end(), timeMs,
                [](std::uint32_t t, const std::pair<std::uint32_t, Values>& p) { return t < p.first; });
            if (it == s.points.begin())
                return &s.points.front().second;
            return &(it - 1)->second;
        }
    };

    // ---- the keep-live list ----

    // SFX_SLIDER, MUSIC_SLIDER, INTERACTIVE_MUSIC, MUSIC_ONESHOT (the code
    // controllers of 0x2EFF0DC) and BASE (the fades of loads and the pause).
    constexpr std::uint32_t kDefaultKeepLive[] = {0xBAD598C7u, 0xA4D158B0u, 0x5DB47EC6u, 0x20230050u, 0x44E21C90u};
    constexpr const char* kDefaultKeepLiveText = "0xBAD598C7,0xA4D158B0,0x5DB47EC6,0x20230050,0x44E21C90";
    // The categories of the volume probe's classes and their parents
    // (speech 75, ambient speech 66, ambience 160 161 11, weather 170 169 172,
    // horse 69 158 71, shots 51, world 65 150).
    constexpr const char* kDefaultWatchText = "75,66,160,161,11,170,169,172,69,158,71,51,65,150";

    inline bool KeepLive(const std::vector<std::uint32_t>& keepLive, std::uint32_t hash)
    {
        return std::find(keepLive.begin(), keepLive.end(), hash) != keepLive.end();
    }

    // "volume" (default) or "all".
    inline bool ParseFieldsAll(const char* text)
    {
        if (!text)
            return false;
        std::string t(text);
        for (char& c : t)
            c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        return t == "all";
    }

    // ---- the evidence ----

    // One window of the playback (CATEGORY_STATE event=window).
    struct Window
    {
        std::uint64_t calls = 0;   // resolver calls while imposing
        std::uint64_t imposed = 0; // categories written
        std::uint64_t raced = 0;   // resolved value not the imposed one (the game side wrote in between)
        std::uint64_t faults = 0;
        float maxDeltaDb = 0.0f;   // largest |recorded - live controllers| among the imposed
        std::uint32_t maxDeltaIndex = 0;
    };

    inline std::string Fmt2(float v)
    {
        char b[32];
        std::snprintf(b, sizeof(b), "%.2f", static_cast<double>(v));
        return b;
    }

    inline std::string WindowLine(std::uint32_t clipMs, const Window& w, std::size_t published, std::size_t keepLive)
    {
        char b[320];
        std::snprintf(b, sizeof(b),
            "CATEGORY_STATE,phase=play,event=window,clip_ms=%u,published=%zu,keep_live=%zu,calls=%llu,imposed=%llu,"
            "raced=%llu,faults=%llu,max_delta_db=%s,max_delta_cat=%u",
            clipMs, published, keepLive, static_cast<unsigned long long>(w.calls), static_cast<unsigned long long>(w.imposed),
            static_cast<unsigned long long>(w.raced), static_cast<unsigned long long>(w.faults), Fmt2(w.maxDeltaDb).c_str(),
            w.maxDeltaIndex);
        return b;
    }
}
