#pragma once

// The speed the ped audio of a puppet reads.
//
// Each ped audio holder owns a PED_VARIABLES block (ped_audio_state.h, part
// b) that the sounds of that ped read by name. UpdatePedVariables 0x31A3ABC
// writes its "speed" (0x8BAE9C31) every audio update from the movement
// component (row +0x88 +0x5DC, read at 0x31A3EF4; the component writes it at
// 0x8EC5D1 from a smoothed |physical velocity - ground velocity|) through the
// block setter (0x31A3F7A: mov edx,8BAE9C31h; mov rcx,rbx; call 0x258A9B4).
// The name appears nowhere else in the executable: only the sound metadata
// reads it. A puppet is frozen and moved by the replay, so its component
// keeps 0: every puppet's sounds would read speed 0 where the recording read
// the ped's speed.
//
// As a replay editor usually does it (each ped's velocity in its packet, given
// to the ped's audio entity at playback), the recording keeps each recorded
// ped's (and horse's) block speed 4 times a second (section PSPD), and the
// playback writes it into the puppet's block right after the engine's own
// update (PedSpeedReplay, a detour on 0x31A3ABC that forwards every call). A
// clip without the section gives the puppet the speed of its recorded motion
// (its displacement over clip time).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace replay::pedspeed
{
    // The block slot UpdatePedVariables fills from the movement speed. "speed" is
    // the mod's label from that source, not a proven name: joaat("speed") is
    // 0xF997622B.
    constexpr std::uint32_t kSpeedVariable = 0x8BAE9C31u; // in PED_VARIABLES (0x31A3F7A)
    constexpr float kMaxSpeed = 150.0f;                   // m/s: anything past this is not a ped's speed
    constexpr std::uint32_t kSamplePeriodMs = 250;        // the recording's scan (PED_VARS)
    constexpr std::uint32_t kHoldMs = 750;                // a value holds this long past its point
    constexpr std::size_t kMaxOwners = 4096;
    constexpr std::size_t kMaxPoints = 1u << 20; // per owner (72 hours at 4 a second)
    constexpr std::size_t kSeriesHeaderBytes = 4 + 8; // owner, point count (the clip file's counts are 8 bytes)
    constexpr std::size_t kPointBytes = 4 + 4;        // time, speed

    inline bool Plausible(float v) { return std::isfinite(v) && v >= 0.0f && v <= kMaxSpeed; }

    struct Point
    {
        std::uint32_t timeMs = 0;
        float speed = 0.0f;
    };

    struct Series
    {
        std::uint32_t owner = 0; // packed replay id
        std::vector<Point> points; // time ascending
    };

    inline float Lerp(const Point& a, const Point& b, std::uint32_t timeMs)
    {
        if (b.timeMs <= a.timeMs)
            return b.speed;
        const float f = static_cast<float>(timeMs - a.timeMs) / static_cast<float>(b.timeMs - a.timeMs);
        return a.speed + (b.speed - a.speed) * f;
    }

    // The recording's part (Recording::pedSpeed, clip section PSPD).
    struct Track
    {
        std::vector<Series> series; // owner ascending

        void Clear() { series.clear(); }
        bool Empty() const { return series.empty(); }

        std::size_t Points() const
        {
            std::size_t n = 0;
            for (const Series& s : series)
                n += s.points.size();
            return n;
        }

        std::size_t ApproxBytes() const
        {
            std::size_t bytes = series.capacity() * sizeof(Series);
            for (const Series& s : series)
                bytes += s.points.capacity() * sizeof(Point);
            return bytes;
        }

        const Series* Find(std::uint32_t owner) const
        {
            const auto it = std::lower_bound(series.begin(), series.end(), owner,
                [](const Series& s, std::uint32_t id) { return s.owner < id; });
            return it != series.end() && it->owner == owner ? &*it : nullptr;
        }

        // Appends in time order (a clock that went back drops the newer tail,
        // the way a rewound recording overwrites). False: not kept.
        bool Note(std::uint32_t owner, std::uint32_t timeMs, float speed)
        {
            if (!Plausible(speed))
                return false;
            auto it = std::lower_bound(series.begin(), series.end(), owner,
                [](const Series& s, std::uint32_t id) { return s.owner < id; });
            if (it == series.end() || it->owner != owner)
            {
                if (series.size() >= kMaxOwners)
                    return false;
                Series s;
                s.owner = owner;
                it = series.insert(it, std::move(s));
            }
            std::vector<Point>& points = it->points;
            while (!points.empty() && points.back().timeMs >= timeMs)
                points.pop_back();
            if (points.size() >= kMaxPoints)
                return false;
            points.push_back({timeMs, speed});
            return true;
        }

        // The ring: points before keepFromMs go, except the last one of each owner
        // (the value in force at keepFromMs). An owner whose last point is more than
        // kHoldMs before keepFromMs goes whole: At() and CutInto() never use it again
        // from keepFromMs on, and its place under kMaxOwners is given back (otherwise,
        // in a long ring session every ped that ever passed would keep one, and past
        // kMaxOwners no new ped would be recorded).
        void DropBefore(std::uint32_t keepFromMs)
        {
            for (Series& s : series)
            {
                if (!s.points.empty() &&
                    static_cast<std::uint64_t>(s.points.back().timeMs) + kHoldMs < static_cast<std::uint64_t>(keepFromMs))
                {
                    s.points.clear();
                    s.points.shrink_to_fit();
                    continue;
                }
                auto first = std::lower_bound(s.points.begin(), s.points.end(), keepFromMs,
                    [](const Point& p, std::uint32_t t) { return p.timeMs < t; });
                if (first != s.points.begin())
                    --first;
                s.points.erase(s.points.begin(), first);
            }
            series.erase(std::remove_if(series.begin(), series.end(), [](const Series& s) { return s.points.empty(); }),
                series.end());
        }

        // A saved window [baseMs, endMs]: the kept owners' points inside it,
        // shifted to the window, and the value in force at baseMs at 0.
        template <typename Kept>
        void CutInto(Track& out, std::uint32_t baseMs, std::uint32_t endMs, Kept kept) const
        {
            out.Clear();
            for (const Series& s : series)
            {
                if (!kept(s.owner))
                    continue;
                Series o;
                o.owner = s.owner;
                const Point* before = nullptr;
                for (const Point& p : s.points)
                {
                    if (p.timeMs < baseMs)
                    {
                        before = &p;
                        continue;
                    }
                    if (p.timeMs > endMs)
                        break;
                    if (o.points.empty() && before && p.timeMs > baseMs && p.timeMs - before->timeMs <= 2 * kHoldMs)
                        o.points.push_back({0, Lerp(*before, p, baseMs)});
                    o.points.push_back({p.timeMs - baseMs, p.speed});
                }
                if (o.points.empty() && before && baseMs - before->timeMs <= kHoldMs)
                    o.points.push_back({0, before->speed});
                if (!o.points.empty())
                    out.series.push_back(std::move(o));
            }
        }

        // A loaded section: owners ascending and unique, times ascending,
        // values plausible.
        bool Valid() const
        {
            if (series.size() > kMaxOwners)
                return false;
            for (std::size_t n = 0; n < series.size(); ++n)
            {
                const Series& s = series[n];
                if (s.points.empty() || s.points.size() > kMaxPoints || (n && series[n - 1].owner >= s.owner))
                    return false;
                for (std::size_t i = 0; i < s.points.size(); ++i)
                {
                    if (!Plausible(s.points[i].speed) || (i && s.points[i - 1].timeMs >= s.points[i].timeMs))
                        return false;
                }
            }
            return true;
        }

        // The recorded value at timeMs: linear between the points around it;
        // before the first and after the last only within kHoldMs; a hole
        // longer than 2 * kHoldMs (the owner was not scanned) is not bridged.
        bool At(std::uint32_t owner, std::uint32_t timeMs, float& out) const
        {
            const Series* s = Find(owner);
            if (!s || s->points.empty())
                return false;
            const std::vector<Point>& points = s->points;
            const auto next = std::upper_bound(points.begin(), points.end(), timeMs,
                [](std::uint32_t t, const Point& p) { return t < p.timeMs; });
            if (next == points.begin())
            {
                if (points.front().timeMs - timeMs > kHoldMs)
                    return false;
                out = points.front().speed;
                return true;
            }
            const Point& a = *std::prev(next);
            if (next == points.end())
            {
                if (timeMs - a.timeMs > kHoldMs)
                    return false;
                out = a.speed;
                return true;
            }
            if (next->timeMs - a.timeMs > 2 * kHoldMs)
            {
                if (timeMs - a.timeMs <= kHoldMs)
                    out = a.speed;
                else if (next->timeMs - timeMs <= kHoldMs)
                    out = next->speed;
                else
                    return false;
                return true;
            }
            out = Lerp(a, *next, timeMs);
            return true;
        }
    };

    // A clip without the section: the speed of the puppet's recorded motion. The
    // replay puts the puppet where the recording had it at the playhead, so its
    // displacement over clip time is the recorded velocity (the engine's own value
    // is |velocity - ground velocity| through the smoother 0x2521410, whose rates
    // (0x39E9B60, 0x39E9B78: 1.0 per ms, no code writes them) do not smooth at ped
    // speeds; a ped on a moving platform reads less live than this gives).
    // A warp of the puppet (a recorded teleport, a horse called to its rider, a
    // puppet put in place) is not a speed: a step of more than kWarpMetres at more
    // than kWarpSpeed starts over, so up to kWindowMs of 20 to 150 m/s never
    // reaches the sounds that read speed.
    struct MotionEstimator
    {
        static constexpr std::size_t kCapacity = 32;
        static constexpr std::uint32_t kWindowMs = 250;   // the span used, at most
        static constexpr std::uint32_t kMinSpanMs = 100;  // and at least
        static constexpr std::uint32_t kResetGapMs = 1000; // a jump of the playhead starts over
        static constexpr float kWarpMetres = 2.0f;         // one step farther than this
        static constexpr float kWarpSpeed = 50.0f;         // and faster than this (m/s) is a warp
        struct Sample
        {
            std::uint32_t clipMs = 0;
            float x = 0.0f, y = 0.0f, z = 0.0f;
        };
        Sample ring[kCapacity];
        std::size_t count = 0;
        std::size_t head = 0; // the newest

        void Reset() { count = 0; }

        static bool IsWarp(const Sample& last, std::uint32_t clipMs, float x, float y, float z)
        {
            const float dx = x - last.x;
            const float dy = y - last.y;
            const float dz = z - last.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            return d > kWarpMetres && d * 1000.0f > kWarpSpeed * static_cast<float>(clipMs - last.clipMs);
        }

        void Note(std::uint32_t clipMs, float x, float y, float z)
        {
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            {
                Reset();
                return;
            }
            if (count)
            {
                const Sample& last = ring[head];
                if (clipMs < last.clipMs || clipMs - last.clipMs > kResetGapMs)
                    Reset();
                else if (clipMs == last.clipMs)
                {
                    ring[head] = Sample{clipMs, x, y, z};
                    return;
                }
                else if (IsWarp(last, clipMs, x, y, z))
                    Reset();
            }
            head = count ? (head + 1) % kCapacity : 0;
            ring[head] = Sample{clipMs, x, y, z};
            if (count < kCapacity)
                ++count;
        }

        // m/s over the longest span within kWindowMs (at least kMinSpanMs).
        bool Speed(float& out) const
        {
            if (count < 2)
                return false;
            const Sample& newest = ring[head];
            const Sample* oldest = nullptr;
            for (std::size_t i = 1; i < count; ++i)
            {
                const Sample& s = ring[(head + kCapacity - i) % kCapacity];
                const std::uint32_t span = newest.clipMs - s.clipMs;
                if (span > kWindowMs)
                    break;
                if (span >= kMinSpanMs)
                    oldest = &s;
            }
            if (!oldest)
                return false;
            const float dx = newest.x - oldest->x;
            const float dy = newest.y - oldest->y;
            const float dz = newest.z - oldest->z;
            const float v = std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0f / static_cast<float>(newest.clipMs - oldest->clipMs);
            if (!Plausible(v))
                return false;
            out = v;
            return true;
        }
    };

    // The script thread's side of the table the detour reads: a holder keeps
    // its slot while it stays published (only its speed changes), a holder
    // that leaves is cleared before anything else is written there, and a
    // new one gets its speed before its holder. Apply calls
    //   clear(slot)              the slot's holder goes to 0
    //   speed(slot, value)       a staying holder's new speed
    //   set(slot, holder, value) a new holder: speed first, then holder
    // and returns the new end (one past the last used slot).
    template <std::uint32_t Slots>
    struct SlotTable
    {
        std::uintptr_t holder[Slots] = {};
        std::uint32_t end = 0;

        template <typename Valid, typename Clear, typename Speed, typename Set>
        std::uint32_t Apply(const std::uintptr_t* holders, const float* speeds, std::size_t n, Valid valid, Clear clear,
            Speed speed, Set set)
        {
            bool keep[Slots] = {};
            std::vector<std::size_t> fresh;
            for (std::size_t k = 0; k < n; ++k)
            {
                if (!holders[k] || !Plausible(speeds[k]) || !valid(holders[k]))
                    continue;
                std::uint32_t slot = Slots;
                for (std::uint32_t i = 0; i < end; ++i)
                    if (holder[i] == holders[k])
                    {
                        slot = i;
                        break;
                    }
                if (slot == Slots)
                {
                    bool dup = false;
                    for (const std::size_t f : fresh)
                        dup = dup || holders[f] == holders[k];
                    if (!dup)
                        fresh.push_back(k);
                    continue;
                }
                if (keep[slot])
                    continue; // the same holder twice: the first wins
                keep[slot] = true;
                speed(slot, speeds[k]);
            }
            for (std::uint32_t i = 0; i < end; ++i)
            {
                if (keep[i] || !holder[i])
                    continue;
                clear(i);
                holder[i] = 0;
            }
            for (const std::size_t k : fresh)
            {
                std::uint32_t slot = Slots;
                for (std::uint32_t i = 0; i < Slots; ++i)
                    if (!holder[i] && !keep[i])
                    {
                        slot = i;
                        break;
                    }
                if (slot == Slots)
                    break; // full: the rest keep the engine's value
                keep[slot] = true;
                holder[slot] = holders[k];
                set(slot, holders[k], speeds[k]);
                if (slot + 1 > end)
                    end = slot + 1;
            }
            while (end && !holder[end - 1])
                --end;
            return end;
        }

        // The slot of a published holder (Slots: none).
        std::uint32_t SlotOf(std::uintptr_t h) const
        {
            for (std::uint32_t i = 0; i < end; ++i)
                if (h && holder[i] == h)
                    return i;
            return Slots;
        }
    };

    enum class Source : std::uint8_t
    {
        None = 0,
        Section = 1, // the recorded block value (PSPD)
        Derived = 2, // the puppet's recorded motion
    };

    inline const char* SourceName(Source s)
    {
        return s == Source::Section ? "section" : s == Source::Derived ? "derived" : "none";
    }

    struct Target
    {
        Source source = Source::None;
        float speed = 0.0f;
        bool recKnown = false;
        float rec = 0.0f;
        bool derivedKnown = false;
        float derived = 0.0f;
    };

    // What the puppet's block should hold: the recorded value when the clip
    // has one for this owner at this time, else the recorded motion's.
    inline Target Choose(const Track& track, std::uint32_t owner, std::uint32_t timeMs, const MotionEstimator& motion)
    {
        Target t;
        t.recKnown = track.At(owner, timeMs, t.rec);
        t.derivedKnown = motion.Speed(t.derived);
        if (t.recKnown)
        {
            t.source = Source::Section;
            t.speed = t.rec;
        }
        else if (t.derivedKnown)
        {
            t.source = Source::Derived;
            t.speed = t.derived;
        }
        return t;
    }

    // The playback's evidence: one line per owner every kBlockMs of clip time.
    constexpr std::uint32_t kBlockMs = 5000;
    constexpr std::uint32_t kLineCap = 240; // lines per playback

    inline std::string Fmt2(bool known, float v)
    {
        if (!known)
            return "none";
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f", v);
        return text;
    }

    // engine: the engine's own value the detour saw before writing (the
    // puppet's frozen component); block: the block read back afterwards.
    inline std::string OwnerLine(std::uint32_t clipMs, const std::string& owner, const char* kind, const Target& t,
        bool engineKnown, float engine, bool blockKnown, float block, bool published)
    {
        return "PED_SPEED_REPLAY,phase=play,clip_ms=" + std::to_string(clipMs) + ",owner=" + owner + ",kind=" + kind +
            ",source=" + SourceName(t.source) + ",published=" + (published ? "1" : "0") +
            ",target=" + Fmt2(t.source != Source::None, t.speed) + ",rec=" + Fmt2(t.recKnown, t.rec) +
            ",derived=" + Fmt2(t.derivedKnown, t.derived) + ",engine=" + Fmt2(engineKnown, engine) +
            ",block=" + Fmt2(blockKnown, block);
    }

    struct Totals
    {
        std::uint64_t puppetTicks = 0; // puppet x tick in normal play
        std::uint64_t fromSection = 0;
        std::uint64_t fromDerived = 0;
        std::uint64_t none = 0;
        std::uint32_t lines = 0;
        // Evidence lines: the block read back against the target.
        std::uint32_t checked = 0;
        std::uint32_t agree = 0;     // within kAgreeAbs
        double sumEngine = 0.0;      // the engine's own value
        std::uint32_t engineSeen = 0;
        double sumTarget = 0.0;
        // Both the recorded and the derived value known (fallback check).
        std::uint32_t both = 0;
        double sumAbsDerivedErr = 0.0;

        static constexpr float kAgreeAbs = 0.05f;

        void AddTarget(const Target& t)
        {
            ++puppetTicks;
            if (t.source == Source::Section)
                ++fromSection;
            else if (t.source == Source::Derived)
                ++fromDerived;
            else
                ++none;
        }

        void AddCheck(const Target& t, bool engineKnown, float engine, bool blockKnown, float block)
        {
            if (engineKnown)
            {
                ++engineSeen;
                sumEngine += engine;
            }
            if (t.source != Source::None && blockKnown)
            {
                ++checked;
                sumTarget += t.speed;
                if (std::fabs(block - t.speed) <= kAgreeAbs)
                    ++agree;
            }
            if (t.recKnown && t.derivedKnown)
            {
                ++both;
                sumAbsDerivedErr += std::fabs(t.derived - t.rec);
            }
        }
    };

    struct DetourCounts
    {
        std::uint64_t calls = 0;  // UpdatePedVariables calls
        std::uint64_t hits = 0;   // ... of a published puppet holder
        std::uint64_t writes = 0; // speed written into its block
        std::uint64_t failed = 0; // block or slot not found / unreadable
    };

    inline std::string TotalLine(bool enabled, bool installed, const std::string& error, bool section,
        std::size_t recOwners, std::size_t recPoints, const Totals& t, const DetourCounts& d)
    {
        char text[640];
        std::snprintf(text, sizeof(text),
            "PED_SPEED_REPLAY_TOTAL,enabled=%d,installed=%d,error=%s,section=%s,rec_owners=%zu,rec_points=%zu,"
            "puppet_ticks=%llu,from_section=%llu,from_derived=%llu,none=%llu,checked=%u,block_agree=%u/%u,"
            "target_mean=%.2f,engine_mean=%s,derived_err_mean=%s,detour_calls=%llu,detour_hits=%llu,"
            "detour_writes=%llu,detour_failed=%llu,lines=%u",
            enabled ? 1 : 0, installed ? 1 : 0, error.empty() ? "none" : error.c_str(), section ? "PSPD" : "none",
            recOwners, recPoints, static_cast<unsigned long long>(t.puppetTicks),
            static_cast<unsigned long long>(t.fromSection), static_cast<unsigned long long>(t.fromDerived),
            static_cast<unsigned long long>(t.none), t.checked, t.agree, t.checked,
            t.checked ? t.sumTarget / t.checked : 0.0,
            t.engineSeen ? Fmt2(true, static_cast<float>(t.sumEngine / t.engineSeen)).c_str() : "none",
            t.both ? Fmt2(true, static_cast<float>(t.sumAbsDerivedErr / t.both)).c_str() : "none",
            static_cast<unsigned long long>(d.calls), static_cast<unsigned long long>(d.hits),
            static_cast<unsigned long long>(d.writes), static_cast<unsigned long long>(d.failed), t.lines);
        return text;
    }

    inline std::string RecordLine(bool enabled, std::size_t owners, std::size_t points, std::uint32_t noted,
        std::uint32_t refused)
    {
        char text[200];
        std::snprintf(text, sizeof(text), "PED_SPEED_RECORD_TOTAL,enabled=%d,owners=%zu,points=%zu,noted=%u,refused=%u",
            enabled ? 1 : 0, owners, points, noted, refused);
        return text;
    }
}
