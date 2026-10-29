#pragma once

// The scrub and jump model, pure so the core tests cover it.
//
// - Moving the playhead never waits; stopping it waits once. A scrub (the
//   keys, a timeline drag) only moves pose, life and camera; the release
//   rebuilds the rest once and the Settle below holds the time until the
//   puppets, their models and that rebuild are ready (N stable ticks, with a
//   stall limit of 6.6 s).
// - A far jump (outside the model window around the playhead, the camera
//   moved more than 100 m, no collision loaded where it landed, or puppets
//   of the target without their model) also freezes the last picture with a
//   spinner (the editor add-on draws it) until the settle ends.
// - A model window of +/-4 s around the playhead is requested ahead, and a
//   model nobody touched for 5 s is let go (ModelPreload).
// - Puppet creation has a budget per frame: larger behind the frozen
//   picture; in normal play a due puppet with its model loaded holds the
//   time a frame instead of letting the playhead run without it.
// - A timeline drag is a scrub until the mouse is released (DragGate).
// - ScrubCost sums what a scrub cost (the SCRUB_COST line).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace replay::settle
{
    struct Config
    {
        std::uint32_t stableTicks = 5;     // frames of a streamed world before it counts as loaded
        std::uint64_t timeoutMs = 6600;    // a settle that never loads is given up after this
        std::uint64_t promoteAfterMs = 400; // a near settle still waiting after this freezes too
        float farCameraMeters = 100.0f;    // a camera jump longer than this freezes the picture
        float collisionCheckMeters = 1.0f; // moved more than this: the collision there must be loaded
        std::uint64_t windowMs = 4000;     // models preloaded this far ahead, each side
        std::uint64_t releaseMs = 5000;    // 1.25 x 4 s: a preload nobody renewed is let go
        std::size_t maxPreloads = 32;      // kept low: a RDR2 ped model weighs a lot
        std::uint64_t dragIdleMs = 500;    // a drag whose end never came settles after this
        std::uint32_t maxCreationHoldFrames = 30; // normal play holds a frame for creation at most this many in a row
    };

    // ---- Far jump -------------------------------------------------------

    enum FarBits : std::uint32_t
    {
        kFarNone = 0,
        kFarOutsideWindow = 1u << 0, // the target is outside the model window
        kFarCamera = 1u << 1,        // the camera moved more than farCameraMeters
        kFarNoCollision = 1u << 2,   // moved and the collision there is not loaded
        kFarModels = 1u << 3,        // puppets alive at the target without their model
        kFarCaller = 1u << 4,        // the caller asked for the frozen picture (start, end)
        kFarSlow = 1u << 5,          // a near settle that took longer than promoteAfterMs
    };

    struct JumpFacts
    {
        bool fine = false;             // a frame step: never far by the camera
        std::uint64_t windowCentreMs = 0; // where the model window stands (the playhead before the jump without it)
        std::uint64_t targetMs = 0;
        bool cameraValid = false;      // both the old and the new camera are known
        float cameraMovedMeters = 0.0f;
        bool collisionLoaded = true;   // HAS_COLLISION_LOADED_AROUND_POSITION at the new camera
        std::uint32_t puppetsWithoutModel = 0;
        bool callerWantsFreeze = false;
    };

    inline std::uint32_t FarBitsOf(const JumpFacts& facts, const Config& config = {})
    {
        std::uint32_t bits = kFarNone;
        const std::uint64_t delta = facts.targetMs > facts.windowCentreMs ? facts.targetMs - facts.windowCentreMs
                                                                          : facts.windowCentreMs - facts.targetMs;
        if (delta > config.windowMs)
            bits |= kFarOutsideWindow;
        if (facts.puppetsWithoutModel > 0)
            bits |= kFarModels;
        if (!facts.fine && facts.cameraValid)
        {
            if (facts.cameraMovedMeters > config.farCameraMeters)
                bits |= kFarCamera;
            else if (facts.cameraMovedMeters > config.collisionCheckMeters && !facts.collisionLoaded)
                bits |= kFarNoCollision;
        }
        if (facts.callerWantsFreeze && !facts.fine)
            bits |= kFarCaller;
        return bits;
    }

    // "outside_window+camera" (none: "near").
    inline std::string FarReasons(std::uint32_t bits)
    {
        static const char* const kNames[] = {"outside_window", "camera_far", "no_collision", "models", "caller", "slow"};
        std::string text;
        for (std::uint32_t bit = 0; bit < 6; ++bit)
        {
            if (!(bits & (1u << bit)))
                continue;
            if (!text.empty())
                text += '+';
            text += kNames[bit];
        }
        return text.empty() ? std::string("near") : text;
    }

    // A playhead move that is a jump for the settle (not a scrub tick): any
    // move while paused (a seek, a marker, a step), and in play a move back
    // or longer than the event cursors' normal step (a loop, a trim, a seek
    // while playing). Normal play moves at most kMaxNormalStepMs a frame.
    constexpr std::uint64_t kJumpStepMs = 250; // ForwardEventCursor::kMaxStepMs
    inline bool IsJumpMove(std::uint64_t before, std::uint64_t after, bool paused, bool scrubbing)
    {
        if (scrubbing || before == after)
            return false;
        if (paused)
            return true;
        return after < before || after - before > kJumpStepMs;
    }

    // ---- Settle ---------------------------------------------------------

    struct Readiness
    {
        std::uint32_t building = 0;      // puppets alive at the playhead not built and shown yet
        std::uint32_t modelsMissing = 0; // of those, the ones whose model is not loaded
        bool stateDeferred = false;      // the cheap scrub's state rebuild has not run yet
        bool statePending = false;       // a reconcile still going on (decal budget, gore retries)
        bool sceneLoaded = true;         // the load scene or the collision at the camera
    };

    inline bool IsReady(const Readiness& readiness)
    {
        return readiness.building == 0 && readiness.modelsMissing == 0 && !readiness.stateDeferred &&
            !readiness.statePending && readiness.sceneLoaded;
    }

    // What was still missing (for the JUMP_LOAD line): "puppets+scene".
    inline std::string Missing(const Readiness& readiness)
    {
        std::string text;
        const auto add = [&](const char* name) {
            if (!text.empty())
                text += '+';
            text += name;
        };
        if (readiness.building)
            add("puppets");
        if (readiness.modelsMissing)
            add("models");
        if (readiness.stateDeferred)
            add("state_deferred");
        if (readiness.statePending)
            add("state_pending");
        if (!readiness.sceneLoaded)
            add("scene");
        return text.empty() ? std::string("none") : text;
    }

    struct Verdict
    {
        bool done = false;
        bool timedOut = false;
        bool promoted = false; // became far this tick (kFarSlow)
    };

    // The wait after a scrub release or a jump. Time does not move while it is
    // active (the transport's hold); a scrub cancels it (pre-caching stops at
    // scrub start); a new jump retargets it (the stable count and the stall limit
    // start again, as a jump restarts the preparation).
    class Settle
    {
    public:
        void Begin(const char* reason, std::uint64_t now, std::uint64_t targetMs, std::uint32_t farBits)
        {
            if (m_active)
            {
                ++m_retargets;
                m_farBits |= farBits; // a frozen picture stays frozen until the end
            }
            else
            {
                m_retargets = 0;
                m_farBits = farBits;
                m_firstStartedAt = now;
            }
            m_active = true;
            m_reason = reason ? reason : "jump";
            m_startedAt = now;
            m_targetMs = targetMs;
            m_stable = 0;
            m_ticks = 0;
        }

        Verdict Update(std::uint64_t now, const Readiness& readiness, const Config& config = {})
        {
            Verdict verdict;
            if (!m_active)
                return verdict;
            now = std::max(now, m_startedAt);
            ++m_ticks;
            m_last = readiness;
            m_stable = IsReady(readiness) ? m_stable + 1 : 0;
            const std::uint64_t elapsed = now - m_startedAt;
            if (m_stable >= config.stableTicks)
                verdict.done = true;
            else if (elapsed >= config.timeoutMs)
                verdict.done = verdict.timedOut = true;
            else if (!m_farBits && elapsed >= config.promoteAfterMs)
            {
                m_farBits |= kFarSlow;
                verdict.promoted = true;
            }
            if (verdict.done)
            {
                m_active = false;
                m_endedElapsedMs = now - m_firstStartedAt;
            }
            return verdict;
        }

        // Far reasons found later in the jump's tick (the camera, known
        // once the replay camera evaluated the target).
        void AddFar(std::uint32_t bits)
        {
            if (m_active)
                m_farBits |= bits;
        }

        void Cancel(std::uint64_t now)
        {
            if (m_active)
                m_endedElapsedMs = now > m_firstStartedAt ? now - m_firstStartedAt : 0;
            m_active = false;
        }

        bool Active() const { return m_active; }
        bool Far() const { return m_active && m_farBits != 0; }
        std::uint32_t FarBits() const { return m_farBits; }
        const char* Reason() const { return m_reason; }
        std::uint64_t TargetMs() const { return m_targetMs; }
        std::uint32_t Retargets() const { return m_retargets; }
        std::uint32_t Ticks() const { return m_ticks; }
        std::uint32_t StableTicks() const { return m_stable; }
        const Readiness& LastReadiness() const { return m_last; }
        std::uint64_t ElapsedMs(std::uint64_t now) const { return now > m_firstStartedAt ? now - m_firstStartedAt : 0; }
        // The whole wait (first jump to done), valid after Update ended it.
        std::uint64_t EndedElapsedMs() const { return m_endedElapsedMs; }

    private:
        bool m_active = false;
        const char* m_reason = "jump";
        std::uint64_t m_startedAt = 0;
        std::uint64_t m_firstStartedAt = 0;
        std::uint64_t m_targetMs = 0;
        std::uint32_t m_farBits = 0;
        std::uint32_t m_stable = 0;
        std::uint32_t m_ticks = 0;
        std::uint32_t m_retargets = 0;
        std::uint64_t m_endedElapsedMs = 0;
        Readiness m_last;
    };

    // ---- Model window ---------------------------------------------------

    // Models alive within +/- windowMs of the playhead are requested ahead
    // (4 s each side); a model the window has not
    // touched for releaseMs is let go (1.25 x 4 s). The
    // models the puppets requested themselves belong to the playback and are
    // never released here (they go at the end of the playback).
    class ModelPreload
    {
    public:
        struct Entry
        {
            std::uint32_t model = 0;
            std::uint64_t touchedAt = 0;
        };

        // `wanted`: the models alive in the window now (duplicates allowed);
        // `owned`: the models the playback requested for its puppets.
        // `request`: the models to request this time (the window's, capped);
        // `release`: the models to let go (SET_MODEL_AS_NO_LONGER_NEEDED).
        void Update(std::uint64_t now, const std::vector<std::uint32_t>& wanted, const std::vector<std::uint32_t>& owned,
            std::vector<std::uint32_t>& request, std::vector<std::uint32_t>& release, const Config& config = {})
        {
            request.clear();
            release.clear();
            const auto isOwned = [&](std::uint32_t model) {
                return std::find(owned.begin(), owned.end(), model) != owned.end();
            };
            // Owned by the playback now: forgotten here, never released here.
            m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                [&](const Entry& entry) { return isOwned(entry.model); }),
                m_entries.end());
            for (const std::uint32_t model : wanted)
            {
                if (!model || isOwned(model) || std::find(request.begin(), request.end(), model) != request.end())
                    continue;
                auto found = std::find_if(m_entries.begin(), m_entries.end(),
                    [&](const Entry& entry) { return entry.model == model; });
                if (found == m_entries.end())
                {
                    if (m_entries.size() >= config.maxPreloads)
                    {
                        ++m_capped;
                        continue;
                    }
                    m_entries.push_back({model, now});
                    ++m_requested;
                }
                else
                    found->touchedAt = now;
                request.push_back(model);
            }
            for (std::size_t index = m_entries.size(); index-- > 0;)
            {
                if (now - std::min(now, m_entries[index].touchedAt) < config.releaseMs)
                    continue;
                release.push_back(m_entries[index].model);
                m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(index));
                ++m_released;
            }
        }

        // Stop: everything held goes.
        void Clear(std::vector<std::uint32_t>& release)
        {
            release.clear();
            for (const Entry& entry : m_entries)
                release.push_back(entry.model);
            m_released += m_entries.size();
            m_entries.clear();
        }

        bool Holds(std::uint32_t model) const
        {
            return std::any_of(m_entries.begin(), m_entries.end(), [&](const Entry& entry) { return entry.model == model; });
        }
        std::size_t Count() const { return m_entries.size(); }
        std::uint64_t Requested() const { return m_requested; }
        std::uint64_t Released() const { return m_released; }
        std::uint64_t Capped() const { return m_capped; }

    private:
        std::vector<Entry> m_entries;
        std::uint64_t m_requested = 0;
        std::uint64_t m_released = 0;
        std::uint64_t m_capped = 0;
    };

    // ---- Creation budget ------------------------------------------------

    constexpr std::uint32_t kCreatesNormal = 4;   // kMaxCreatesPerFrame: CREATE_PED is synchronous and costly
    constexpr std::uint32_t kCreatesSettle = 6;   // settling with the picture live
    constexpr std::uint32_t kCreatesFrozen = 12;  // behind the frozen picture (the hitch does not show)

    inline std::uint32_t CreateBudget(bool settling, bool frozen)
    {
        return frozen ? kCreatesFrozen : settling ? kCreatesSettle : kCreatesNormal;
    }

    // In normal play a pending create abandons the frame before the time moves: a
    // puppet alive at the playhead whose model is loaded but that the budget left
    // uncreated holds the time this frame. Bounded (heldFrames in a row) so a
    // create the engine keeps refusing never stops the playback; a model still
    // loading never holds (kModelTimeoutMs covers it).
    inline bool HoldForCreation(bool normalPlay, std::uint32_t dueWithModel, std::uint32_t heldFrames, const Config& config = {})
    {
        return normalPlay && dueWithModel > 0 && heldFrames < config.maxCreationHoldFrames;
    }

    // ---- Timeline drag --------------------------------------------------

    // Seek kinds of the editor's SeekAbsolute (editor_protocol.h
    // SeekDragging, SeekDragEnd); 0 is a plain seek (a click, a marker).
    enum class SeekKind : std::uint8_t
    {
        Plain = 0,
        Dragging = 1,
        DragEnd = 2,
    };

    // A drag moves the playhead cheaply (a scrub) and loads once, where the
    // mouse is released; a drag end that never comes (an editor closed
    // mid-drag) ends it after dragIdleMs.
    class DragGate
    {
    public:
        // Returns true when this seek settles (plain or drag end).
        bool OnSeek(std::uint64_t now, SeekKind kind)
        {
            if (kind == SeekKind::Dragging)
            {
                m_dragging = true;
                m_lastAt = now;
                return false;
            }
            m_dragging = false;
            return true;
        }
        bool Dragging() const { return m_dragging; }
        // True once when a drag went idle without its end (the caller settles).
        bool TakeIdleEnd(std::uint64_t now, const Config& config = {})
        {
            if (!m_dragging || now < m_lastAt || now - m_lastAt < config.dragIdleMs)
                return false;
            m_dragging = false;
            return true;
        }
        void Reset()
        {
            m_dragging = false;
        }

    private:
        bool m_dragging = false;
        std::uint64_t m_lastAt = 0;
    };

    // ---- Scrub cost -----------------------------------------------------

    struct CostCounters
    {
        std::uint64_t decodes = 0;         // pose frames decoded (pose_stream DecodedFrameCount)
        std::uint64_t checkpointRestores = 0;
        std::uint64_t decalReconciles = 0;
        std::uint64_t bloodReconciles = 0; // per ped
        std::uint64_t goreReconciles = 0;  // per ped
        std::uint64_t fireReconciles = 0;
        std::uint64_t creates = 0;         // puppets created
        std::uint64_t vehicleRebuilds = 0; // vehicles deleted to be built again (windows, parts, frags, burnt)
        std::uint64_t deferredTicks = 0;   // ticks the state rebuild was deferred (cheap scrub)
    };

    inline std::uint64_t Since(std::uint64_t now, std::uint64_t before) { return now >= before ? now - before : now; }

    // One scrub (keys or a drag): its ticks and what they did, and the
    // release tick (where the deferred rebuild runs once).
    class ScrubCost
    {
    public:
        void Begin(const char* kind, int direction, std::uint64_t fromMs, const CostCounters& counters)
        {
            *this = ScrubCost{};
            m_active = true;
            m_kind = kind ? kind : "scrub";
            m_direction = direction;
            m_fromMs = fromMs;
            m_start = counters;
        }
        void Tick(double tickUs, std::uint32_t puppets)
        {
            if (!m_active)
                return;
            ++m_ticks;
            m_totalUs += tickUs;
            m_maxUs = std::max(m_maxUs, tickUs);
            m_maxPuppets = std::max(m_maxPuppets, puppets);
        }
        bool Active() const { return m_active; }
        std::uint32_t Ticks() const { return m_ticks; }

        // The release tick's time and the counters after it: the line.
        std::string Finish(std::uint64_t toMs, double releaseUs, const CostCounters& counters, bool deferred)
        {
            m_active = false;
            const CostCounters& a = m_start;
            char line[512];
            std::snprintf(line, sizeof(line),
                "SCRUB_COST,kind=%s,direction=%d,from_ms=%llu,to_ms=%llu,ticks=%u,avg_ms=%.2f,max_ms=%.2f,release_ms=%.2f,"
                "puppets=%u,decodes=%llu,checkpoint_restores=%llu,decal_reconciles=%llu,blood_reconciles=%llu,"
                "gore_reconciles=%llu,fire_reconciles=%llu,creates=%llu,vehicle_rebuilds=%llu,deferred=%d,deferred_ticks=%llu\n",
                m_kind, m_direction, static_cast<unsigned long long>(m_fromMs), static_cast<unsigned long long>(toMs), m_ticks,
                m_ticks ? m_totalUs / 1000.0 / m_ticks : 0.0, m_maxUs / 1000.0, releaseUs / 1000.0, m_maxPuppets,
                static_cast<unsigned long long>(Since(counters.decodes, a.decodes)),
                static_cast<unsigned long long>(Since(counters.checkpointRestores, a.checkpointRestores)),
                static_cast<unsigned long long>(Since(counters.decalReconciles, a.decalReconciles)),
                static_cast<unsigned long long>(Since(counters.bloodReconciles, a.bloodReconciles)),
                static_cast<unsigned long long>(Since(counters.goreReconciles, a.goreReconciles)),
                static_cast<unsigned long long>(Since(counters.fireReconciles, a.fireReconciles)),
                static_cast<unsigned long long>(Since(counters.creates, a.creates)),
                static_cast<unsigned long long>(Since(counters.vehicleRebuilds, a.vehicleRebuilds)), deferred ? 1 : 0,
                static_cast<unsigned long long>(Since(counters.deferredTicks, a.deferredTicks)));
            return line;
        }

    private:
        bool m_active = false;
        const char* m_kind = "scrub";
        int m_direction = 0;
        std::uint64_t m_fromMs = 0;
        std::uint32_t m_ticks = 0;
        double m_totalUs = 0.0;
        double m_maxUs = 0.0;
        std::uint32_t m_maxPuppets = 0;
        CostCounters m_start;
    };
}
