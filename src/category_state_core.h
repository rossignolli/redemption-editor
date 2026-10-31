#pragma once

// The state of the audio engine's sound categories recorded and put back in
// the replay (clip section CATS): record the engine state the mix depends on
// and restore it, applied to the category stage of every voice. Pure part (no
// game); unit tested in tests/category_state_tests.cpp.
//
// Why: mixer scenes the replay session has and the recording did not (and the
// other way round) move the category stage (d_cat) by about +2 to +6 dB.
//
// The mechanism: the recording keeps, for every static category (by hash),
// the values the controller manager wrote (first sight and every change, at
// most once per kMinIntervalMs per category); the playback, while the replay
// owns the world, puts the recorded volume (or every field) into the category
// right before the resolver reads it (a forwarding detour of the resolver), so
// the resolved value is the recording's whatever scenes the session has. The
// sliders, the music categories and BASE stay live (CategoryStateKeepLive).
// Evidence: offline dump; the signatures below hold the bytes.
// Research: docs/research/audio-mix-scenes.md

#include "game_sig_core.h"

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
    // ---- engine layout ----

    // The resolver's prologue up to the first read of +0x48 (61 bytes): mov
    // rax,rsp; four saves; push r14; sub rsp,20h; movzx r11d,[rcx+0B0h]; mov
    // sil,r8b; movzx ebp,dx; mov r9,rcx; test r8b,r8b; je; movss xmm0,[rcx];
    // jmp; xorps; addss xmm0,[rcx+48h]. Its read of the resolved table, its
    // store at the category's own index and its call for the children.
    inline const sig::Signature kResolver{"category_resolver",
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 48 89 78 20 41 56 48 83 EC 20 44 0F B7 99 B0 00 00 00 41 8A F0 0F "
        "B7 EA 4C 8B C9 45 84 C0 74 06 F3 0F 10 01 EB 03 0F 57 C0 F3 0F 58 41 48",
        0x251C310};
    inline const sig::Signature kResolvedTableRead{"category_resolved_table_read", kResolver, sig::Offset{0x3D},
        "48 8B 15 ?? ?? ?? ??", 0x251C34D};
    inline const sig::Signature kResolvedTable{"category_resolved_table", kResolvedTableRead, sig::Rel32{0x03, 0x07},
        "", 0x5338488};
    inline const sig::Signature kResolvedIndexStore{"category_resolved_index_store", kResolver, sig::Offset{0x64},
        "49 0F BF 81 B2 00 00 00 48 8D 0C C0 F3 0F 11 04 CA", 0x251C374};
    inline const sig::Signature kCallChildren{"category_call_children", kResolver, sig::Offset{0x6B8}, "E8 ?? ?? ?? ??",
        0x251C9C8};
    inline const sig::Signature kCallChildrenTarget{"category_call_children_target", kCallChildren,
        sig::Rel32{0x01, 0x05},
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 48 89 78 20 41 56 48 83 EC 20 44 0F B7 99 B0 00 00 00 41 8A F0 0F "
        "B7 EA 4C 8B C9 45 84 C0 74 06 F3 0F 10 01 EB 03 0F 57 C0 F3 0F 58 41 48",
        0x251C310};
    // The resolve of every hierarchy: the pool's allocated bits, the call for
    // the root and for each dynamic hierarchy.
    inline const sig::Signature kResolveAll{"category_resolve_all",
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8D B1 C8 00 00 00", 0x2539654};
    inline const sig::Signature kPoolBitsSite{"category_pool_bits", kResolveAll, sig::Offset{0x37}, "48 8B 41 38",
        0x253968B};
    inline const sig::Signature kCallRoot{"category_call_root", kResolveAll, sig::Offset{0x59}, "E8 ?? ?? ?? ??",
        0x25396AD};
    inline const sig::Signature kCallRootTarget{"category_call_root_target", kCallRoot, sig::Rel32{0x01, 0x05},
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 48 89 78 20 41 56 48 83 EC 20 44 0F B7 99 B0 00 00 00 41 8A F0 0F "
        "B7 EA 4C 8B C9 45 84 C0 74 06 F3 0F 10 01 EB 03 0F 57 C0 F3 0F 58 41 48",
        0x251C310};
    inline const sig::Signature kCallDynamic{"category_call_dynamic", kResolveAll, sig::Offset{0xC0}, "E8 ?? ?? ?? ??",
        0x2539714};
    inline const sig::Signature kCallDynamicTarget{"category_call_dynamic_target", kCallDynamic, sig::Rel32{0x01, 0x05},
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 48 89 78 20 41 56 48 83 EC 20 44 0F B7 99 B0 00 00 00 41 8A F0 0F "
        "B7 EA 4C 8B C9 45 84 C0 74 06 F3 0F 10 01 EB 03 0F 57 C0 F3 0F 58 41 48",
        0x251C310};
    // The controller manager: the static count, the category pool, its store
    // of the volume and of the other fields (CategoryStateFields=all writes
    // those).
    inline const sig::Signature kControllerManager{"category_controller_manager",
        "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 54 41 56 41 57 48 83 EC 40 44 8B 35 ?? ?? ?? ??",
        0x25394C8};
    inline const sig::Signature kStaticCountRead{"category_static_count_read", kControllerManager, sig::Offset{0x1D},
        "44 8B 35 ?? ?? ?? ??", 0x25394E5};
    inline const sig::Signature kStaticCount{"category_static_count", kStaticCountRead, sig::Rel32{0x03, 0x07}, "",
        0x5338630};
    inline const sig::Signature kCategoryPoolRead{"category_pool_read", kControllerManager, sig::Offset{0x45},
        "4C 8B 15 ?? ?? ?? ??", 0x253950D};
    inline const sig::Signature kCategoryPool{"category_pool", kCategoryPoolRead, sig::Rel32{0x03, 0x07}, "",
        0x5338490};
    inline const sig::Signature kControllerVolumeStore{"category_controller_volume_store", kControllerManager,
        sig::Offset{0xF2}, "F3 0F 11 43 48", 0x25395BA};
    inline const sig::Signature kControllerFieldsStore{"category_controller_fields_store", kControllerManager,
        sig::Offset{0xF7},
        "F3 0F 10 44 24 28 F3 0F 11 43 54 F3 0F 11 4B 4C 66 89 83 88 00 00 00 0F B7 44 24 2E 66 89 83 8A 00 00 00 0F "
        "B7 44 24 30 66 89 83 8C 00 00 00",
        0x25395BF};
    // A controller's creation: the category map, its lookup by hash, the
    // pool index of a category.
    inline const sig::Signature kControllerCreate{"category_controller_create",
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 48 8D B1 40 08 00 00", 0x2526258};
    inline const sig::Signature kMapObjectLea{"category_map_object", kControllerCreate, sig::Offset{0x40},
        "48 8D 0D ?? ?? ?? ??", 0x2526298};
    inline const sig::Signature kCategoryMap{"category_map", kMapObjectLea, sig::Rel32{0x03, 0x07}, "", 0x5338590};
    inline const sig::Signature kMapLookupCall{"category_map_lookup_call", kControllerCreate, sig::Offset{0x47},
        "E8 ?? ?? ?? ??", 0x252629F};
    inline const sig::Signature kMapLookup{"category_map_lookup", kMapLookupCall, sig::Rel32{0x01, 0x05},
        "48 89 5C 24 08 44 8B 81 A4 00 00 00", 0x252A3D0};
    inline const sig::Signature kMapCountSite{"category_map_count", kMapLookup, sig::Offset{0x05},
        "44 8B 81 A4 00 00 00", 0x252A3D5};
    inline const sig::Signature kMapEntriesSite{"category_map_entries", kMapLookup, sig::Offset{0x25},
        "48 8B 99 A8 00 00 00", 0x252A3F5};
    inline const sig::Signature kMapHashSite{"category_map_hash", kMapLookup, sig::Offset{0x3C}, "44 3B 5C C3 08",
        0x252A40C};
    inline const sig::Signature kMapPointerSite{"category_map_pointer", kMapLookup, sig::Offset{0x59}, "4C 8B 14 CB",
        0x252A429};
    inline const sig::Signature kPoolBaseSite{"category_pool_base", kControllerCreate, sig::Offset{0xF5}, "4C 2B 59 08",
        0x252634D};
    inline const sig::Signature kPoolStrideSite{"category_pool_stride", kControllerCreate, sig::Offset{0xFC},
        "48 F7 71 20", 0x2526354};

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

    // A code site as the log names it: the instruction, what its call or rip
    // operand reaches, and what that must be too (the resolver its calls
    // reach).
    struct Site
    {
        const char* name;
        const sig::Signature& at;
        const sig::Signature* reach;
        const sig::Signature* same;
    };

    inline const Site kSites[] = {
        {"resolved_table", kResolvedTableRead, &kResolvedTable, nullptr},
        {"resolved_index_store", kResolvedIndexStore, nullptr, nullptr},
        {"call_root", kCallRoot, &kCallRootTarget, &kResolver},
        {"call_dynamic", kCallDynamic, &kCallDynamicTarget, &kResolver},
        {"call_children", kCallChildren, &kCallChildrenTarget, &kResolver},
        {"static_count", kStaticCountRead, &kStaticCount, nullptr},
        {"category_pool", kCategoryPoolRead, &kCategoryPool, nullptr},
        {"controller_volume_store", kControllerVolumeStore, nullptr, nullptr},
        {"map_object", kMapObjectLea, &kCategoryMap, nullptr},
        {"map_lookup_call", kMapLookupCall, &kMapLookup, nullptr},
        {"map_count", kMapCountSite, nullptr, nullptr},
        {"map_entries", kMapEntriesSite, nullptr, nullptr},
        {"map_hash", kMapHashSite, nullptr, nullptr},
        {"map_pointer", kMapPointerSite, nullptr, nullptr},
        {"pool_base", kPoolBaseSite, nullptr, nullptr},
        {"pool_stride", kPoolStrideSite, nullptr, nullptr},
        {"pool_bits", kPoolBitsSite, nullptr, nullptr},
    };
    constexpr std::size_t kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

    // ---- the values ----

    // What the controller manager writes into a category.
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

    // SFX_SLIDER, MUSIC_SLIDER, INTERACTIVE_MUSIC, MUSIC_ONESHOT (the
    // controllers made by code) and BASE (the fades of loads and the pause).
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
