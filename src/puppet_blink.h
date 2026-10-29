#pragma once

// The puppet blink detector.
//
// A blink is any tick of normal play where a body (ped or horse) that the
// recording says is on screen is not drawn as recorded:
//   hidden   the puppet exists and is hidden by the mod (the unposed hide, no
//            sample at the playhead, an arm given up);
//   unposed  it is shown with no pose override armed (its bind pose would draw);
//   bind     it is shown and armed, but the frame got no pose write (the detour
//            refused the write on a bone count mismatch, or nothing wrote it);
//   missing  the recording has it alive and visible, and there is no puppet
//            (still loading, dressing, rebuilt, failed).
// Should be visible: alive at the playhead, recorded visible (PVIS: a cutscene
// or a mission hide is recorded hidden and never counts), inside its pose
// track, not intentionally absent (the never animated hide, a puppet waiting
// for a later segment).
//
// Before a body was shown at all in the current stretch of normal play (the
// playback start, after a pause, a scrub, a seek settle or a jump) it gets
// Rule::graceMs of playhead to appear: a late first appearance is the puppet
// watch's business (PUPPET_MISSING), not a blink. Once shown, a single tick
// counts.
//
// Episodes: consecutive ticks of one body with a blink, of the kind and cause
// they began with. Each one closes into a PUPPET_BLINK line (capped by the
// caller); the playback's PUPPET_BLINK_SUMMARY has the totals by kind, the
// counted episodes and the worst ones. An episode counts (fails a bench run)
// when it was on screen and within radiusM of the camera for at least
// minFrames ticks (noWriteMinFrames for a frame with no write at all: a
// finalizer skipped once shows the previous pose, not the bind pose; the LOD
// force makes it write again on the next frame). Far or off screen episodes
// are logged and do not count.
//
// Pure: no engine access; unit tested in ReplayCoreTests (puppet_blink_tests.cpp).

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace replay::blink
{
    enum class Kind : std::uint8_t
    {
        None = 0,
        Hidden,
        Unposed,
        Bind,
        Missing,
        Count,
    };

    inline const char* KindText(Kind kind)
    {
        switch (kind)
        {
        case Kind::None:
            return "none";
        case Kind::Hidden:
            return "hidden";
        case Kind::Unposed:
            return "unposed";
        case Kind::Bind:
            return "bind";
        case Kind::Missing:
            return "missing";
        default:
            return "?";
        }
    }

    enum class Cause : std::uint8_t
    {
        Other = 0,
        Create,       // never posed in this stretch (first arm)
        Rearm,        // posed before: the arm was released (rebuild, re-dress, skeleton change)
        Segment,      // a pose segment switch released the arm (segment_switch.h hole) or changed the record
        LayerRetry,   // a layer application left a bone count no record has; applied again
        PrefixHold,   // a gore rebuild held on the record's first bones (prefix_hold.h)
        ArmRefused,   // arming gave up (no record and no skeleton map for its bones)
        NoSample,     // no pose sample at the playhead for the record it plays
        BoneMismatch, // the detour refused the write: the skeleton has another bone count
        NoWrite,      // no write at all reached the skeleton this frame
        ModelLoading, // missing: the model is not in memory
        CreateWait,   // missing: the create budget or a retry
        Dressing,     // missing: created, appearance still being built
        Failed,       // missing: gave up
        NoPuppet,     // missing: no clone (or its entity is gone)
        Count,
    };

    inline const char* CauseText(Cause cause)
    {
        switch (cause)
        {
        case Cause::Other:
            return "other";
        case Cause::Create:
            return "create";
        case Cause::Rearm:
            return "rearm";
        case Cause::Segment:
            return "segment";
        case Cause::LayerRetry:
            return "layer_retry";
        case Cause::PrefixHold:
            return "prefix_hold";
        case Cause::ArmRefused:
            return "arm_refused";
        case Cause::NoSample:
            return "no_sample";
        case Cause::BoneMismatch:
            return "bone_mismatch";
        case Cause::NoWrite:
            return "no_write";
        case Cause::ModelLoading:
            return "model_loading";
        case Cause::CreateWait:
            return "create_wait";
        case Cause::Dressing:
            return "dressing";
        case Cause::Failed:
            return "failed";
        case Cause::NoPuppet:
            return "no_puppet";
        default:
            return "?";
        }
    }

    struct Rule
    {
        float radiusM = 60.0f;              // nearer than this (and on screen) counts
        std::uint32_t minFrames = 1;        // a counted episode lasts at least this many ticks
        std::uint32_t noWriteMinFrames = 2; // ... a no_write one at least this many
        std::uint32_t graceMs = 1500;       // playhead a never shown body gets to appear
        std::uint32_t whereEveryFrames = 15; // camera distance and screen re-read every N ticks of an episode
    };

    // One body this tick, as the playback sees it.
    struct Frame
    {
        // Should it be drawn?
        bool alive = false;           // in the lifecycle at the playhead
        bool recordedVisible = false; // PVIS at the playhead
        bool inTrack = false;         // inside its pose track (or a later segment's)
        bool intentional = false;     // the never animated hide, waiting for a segment or a parent
        // What the mod has.
        bool ready = false;           // a Ready puppet with a live clone
        bool shown = false;           // the mod set it visible (after this tick's decision)
        bool armed = false;           // a pose override slot
        // The frame since the last tick: the write counters of the same slot
        // while the body was shown and armed then too, and the playhead moved.
        bool writesKnown = false;
        std::uint64_t writes = 0;
        std::uint64_t mismatches = 0;
    };

    inline bool ShouldShow(const Frame& f)
    {
        return f.alive && f.recordedVisible && f.inTrack && !f.intentional;
    }

    // The kind of this tick (no grace applied; see Tracker::Judge).
    inline Kind Classify(const Frame& f)
    {
        if (!ShouldShow(f))
            return Kind::None;
        if (!f.ready)
            return Kind::Missing;
        if (!f.shown)
            return Kind::Hidden;
        if (!f.armed)
            return Kind::Unposed;
        if (f.writesKnown && f.writes == 0)
            return Kind::Bind;
        return Kind::None;
    }

    // Why a hidden or unposed body has no pose, from the puppet's state.
    struct ArmState
    {
        bool sampled = true;
        bool armRefused = false;
        bool prefixHold = false;
        bool layerRetry = false;
        bool segmentHole = false;
        bool recordChanged = false;
        bool lostToChange = false;
        bool shownBefore = false;
    };

    inline Cause ArmCause(const ArmState& s)
    {
        if (!s.sampled)
            return Cause::NoSample;
        if (s.armRefused)
            return Cause::ArmRefused;
        if (s.prefixHold)
            return Cause::PrefixHold;
        if (s.segmentHole || s.recordChanged)
            return Cause::Segment;
        if (s.layerRetry)
            return Cause::LayerRetry;
        if (s.lostToChange || s.shownBefore)
            return Cause::Rearm;
        return Cause::Create;
    }

    inline Cause BindCause(const Frame& f)
    {
        return f.mismatches > 0 ? Cause::BoneMismatch : Cause::NoWrite;
    }

    // The write counters of a puppet's override slot, kept from one tick to
    // the next (WriteBaseline::Delta gives the frame in between).
    struct WriteBaseline
    {
        int slot = -1;
        bool shown = false;
        std::uint64_t writes = 0;
        std::uint64_t mismatches = 0;

        // True (with the deltas) when the last tick had the same slot, the
        // body shown, and the counters did not go back (a slot reset).
        bool Delta(int nowSlot, std::uint64_t nowWrites, std::uint64_t nowMismatches, std::uint64_t& dWrites,
            std::uint64_t& dMismatches) const
        {
            if (slot < 0 || nowSlot != slot || !shown || nowWrites < writes || nowMismatches < mismatches)
                return false;
            dWrites = nowWrites - writes;
            dMismatches = nowMismatches - mismatches;
            return true;
        }

        void Keep(int nowSlot, bool nowShown, std::uint64_t nowWrites, std::uint64_t nowMismatches)
        {
            slot = nowSlot;
            shown = nowShown;
            writes = nowWrites;
            mismatches = nowMismatches;
        }
    };

    struct Where
    {
        float cameraM = -1.0f; // < 0: unknown
        bool onScreen = false;
    };

    struct Episode
    {
        std::uint32_t id = 0;
        std::uint32_t model = 0;
        Kind kind = Kind::None;
        Cause cause = Cause::Other;
        std::uint32_t fromMs = 0;  // playhead of its first tick
        std::uint32_t toMs = 0;    // playhead of its last tick
        std::uint32_t frames = 0;  // ticks
        std::uint64_t wallMs = 0;  // wall time from its first tick to the tick it ended
        float cameraM = -1.0f;     // nearest known
        bool onScreen = false;     // on screen in any tick read
        bool counts = false;       // on screen, near, long enough (Rule)
        bool cut = false;          // ended by a pause, a scrub, a jump or the playback's end
    };

    inline bool Counts(const Episode& e, const Rule& rule)
    {
        const std::uint32_t minFrames = e.cause == Cause::NoWrite ? rule.noWriteMinFrames : rule.minFrames;
        return e.onScreen && e.cameraM >= 0.0f && e.cameraM <= rule.radiusM && e.frames >= minFrames;
    }

    struct Summary
    {
        std::uint64_t ticks = 0;  // ticks judged
        std::uint64_t judged = 0; // body ticks judged
        std::uint32_t episodes = 0;
        std::uint32_t onScreenEpisodes = 0;
        std::uint32_t counted = 0;
        std::uint64_t frames = 0;         // blink ticks, every episode
        std::uint64_t countedFrames = 0;  // blink ticks of counted episodes
        std::uint32_t perKind[static_cast<int>(Kind::Count)] = {};
        std::uint32_t countedPerKind[static_cast<int>(Kind::Count)] = {};
        std::uint32_t perCause[static_cast<int>(Cause::Count)] = {};
        std::uint32_t suspends = 0;
        std::vector<Episode> worst; // counted first, then the longest
    };

    class Tracker
    {
    public:
        static constexpr std::size_t kWorst = 4;

        explicit Tracker(const Rule& rule = {}) : m_rule(rule) {}

        const Rule& Settings() const { return m_rule; }

        void Reset()
        {
            m_open.clear();
            m_shown.clear();
            m_summary = {};
            m_active = false;
            m_stretchMs = 0;
        }

        bool Active() const { return m_active; }

        // A tick of normal play begins (the first after a suspend starts a
        // stretch at this playhead).
        void BeginTick(std::uint32_t playheadMs)
        {
            if (!m_active)
            {
                m_active = true;
                m_stretchMs = playheadMs;
            }
            ++m_summary.ticks;
            for (Entry& entry : m_open)
                entry.noted = false;
        }

        // This tick's kind for a body after the grace: a body never shown in
        // this stretch is not judged until it had graceMs of playhead since
        // the stretch began or it entered the recording, whichever is later.
        Kind Judge(std::uint32_t id, const Frame& frame, std::uint32_t playheadMs, std::uint32_t entryMs)
        {
            ++m_summary.judged;
            const Kind kind = Classify(frame);
            if (kind == Kind::None)
            {
                if (frame.ready && frame.shown)
                    MarkShown(id);
                return Kind::None;
            }
            if (ShownBefore(id))
                return kind;
            const std::uint32_t since = (std::max)(m_stretchMs, entryMs);
            return playheadMs >= since && playheadMs - since >= m_rule.graceMs ? kind : Kind::None;
        }

        bool ShownBefore(std::uint32_t id) const
        {
            return std::binary_search(m_shown.begin(), m_shown.end(), id);
        }

        // Is the episode of `id` new or due for a camera read (the caller
        // reads Where only then)?
        bool WantsWhere(std::uint32_t id, Kind kind) const
        {
            const Entry* entry = Find(id);
            if (!entry || entry->episode.kind != kind)
                return true;
            return m_rule.whereEveryFrames == 0 || entry->episode.frames % m_rule.whereEveryFrames == 0;
        }

        // A blink tick of `id` (kind != None). `where` is read when WantsWhere
        // said so (pass nullptr otherwise). Another kind than the open
        // episode's ends it and begins a new one.
        void Note(std::uint32_t id, std::uint32_t model, Kind kind, Cause cause, std::uint32_t playheadMs,
            std::uint64_t wallMs, const Where* where, std::vector<Episode>& closed)
        {
            Entry* entry = Find(id);
            if (entry && entry->episode.kind != kind)
            {
                Close(*entry, wallMs, false, closed);
                Erase(id);
                entry = nullptr;
            }
            if (!entry)
            {
                Entry fresh;
                fresh.episode.id = id;
                fresh.episode.model = model;
                fresh.episode.kind = kind;
                fresh.episode.cause = cause;
                fresh.episode.fromMs = playheadMs;
                fresh.since = wallMs;
                m_open.push_back(fresh);
                entry = &m_open.back();
            }
            entry->noted = true;
            Episode& e = entry->episode;
            e.toMs = playheadMs;
            ++e.frames;
            if (where)
            {
                if (where->cameraM >= 0.0f && (e.cameraM < 0.0f || where->cameraM < e.cameraM))
                    e.cameraM = where->cameraM;
                e.onScreen = e.onScreen || where->onScreen;
            }
        }

        // A body without a blink this tick (its open episode ends).
        void Clear(std::uint32_t id, std::uint64_t wallMs, std::vector<Episode>& closed)
        {
            if (m_open.empty())
                return;
            if (Entry* entry = Find(id))
            {
                Close(*entry, wallMs, false, closed);
                Erase(id);
            }
        }

        // After the bodies of a tick: episodes of bodies not noted (deleted,
        // no longer alive) end.
        void EndTick(std::uint64_t wallMs, std::vector<Episode>& closed)
        {
            for (std::size_t index = m_open.size(); index-- > 0;)
            {
                if (m_open[index].noted)
                    continue;
                Close(m_open[index], wallMs, false, closed);
                m_open.erase(m_open.begin() + static_cast<std::ptrdiff_t>(index));
            }
        }

        // Pause, scrub, settle, jump, the playback's end: open episodes end
        // (cut) and the next tick starts a new stretch (every body gets its
        // grace again).
        void Suspend(std::uint64_t wallMs, std::vector<Episode>& closed)
        {
            if (m_active)
                ++m_summary.suspends;
            for (Entry& entry : m_open)
                Close(entry, wallMs, true, closed);
            m_open.clear();
            m_shown.clear();
            m_active = false;
        }

        const Summary& Stats() const { return m_summary; }
        std::size_t Open() const { return m_open.size(); }

    private:
        struct Entry
        {
            Episode episode;
            std::uint64_t since = 0;
            bool noted = false;
        };

        Entry* Find(std::uint32_t id)
        {
            for (Entry& entry : m_open)
                if (entry.episode.id == id)
                    return &entry;
            return nullptr;
        }
        const Entry* Find(std::uint32_t id) const
        {
            for (const Entry& entry : m_open)
                if (entry.episode.id == id)
                    return &entry;
            return nullptr;
        }
        void Erase(std::uint32_t id)
        {
            m_open.erase(std::remove_if(m_open.begin(), m_open.end(),
                             [&](const Entry& entry) { return entry.episode.id == id; }),
                m_open.end());
        }

        void MarkShown(std::uint32_t id)
        {
            const auto at = std::lower_bound(m_shown.begin(), m_shown.end(), id);
            if (at == m_shown.end() || *at != id)
                m_shown.insert(at, id);
        }

        void Close(Entry& entry, std::uint64_t wallMs, bool cut, std::vector<Episode>& closed)
        {
            Episode& e = entry.episode;
            e.wallMs = wallMs >= entry.since ? wallMs - entry.since : 0;
            e.cut = cut;
            e.counts = Counts(e, m_rule);
            Summary& s = m_summary;
            ++s.episodes;
            s.frames += e.frames;
            ++s.perKind[static_cast<int>(e.kind)];
            ++s.perCause[static_cast<int>(e.cause)];
            if (e.onScreen)
                ++s.onScreenEpisodes;
            if (e.counts)
            {
                ++s.counted;
                s.countedFrames += e.frames;
                ++s.countedPerKind[static_cast<int>(e.kind)];
            }
            // The worst: counted first, then more frames.
            const auto worse = [](const Episode& a, const Episode& b) {
                return a.counts != b.counts ? a.counts : a.frames > b.frames;
            };
            s.worst.insert(std::upper_bound(s.worst.begin(), s.worst.end(), e, worse), e);
            if (s.worst.size() > kWorst)
                s.worst.pop_back();
            closed.push_back(e);
        }

        Rule m_rule;
        std::vector<Entry> m_open;
        std::vector<std::uint32_t> m_shown; // sorted ids shown in this stretch
        Summary m_summary;
        bool m_active = false;
        std::uint32_t m_stretchMs = 0;
    };

    // The summary's worst field: id/kind/cause/frames/on|off[/counted] joined by '|'
    // ("none" when empty). `idText` turns a packed id into text.
    template <typename IdText>
    std::string WorstText(const std::vector<Episode>& worst, IdText idText)
    {
        if (worst.empty())
            return "none";
        std::string out;
        for (const Episode& e : worst)
        {
            if (!out.empty())
                out += '|';
            out += idText(e.id);
            out += '/';
            out += KindText(e.kind);
            out += '/';
            out += CauseText(e.cause);
            out += '/';
            out += std::to_string(e.frames);
            out += e.onScreen ? "/on" : "/off";
            if (e.counts)
                out += "/counted";
        }
        return out;
    }
}
