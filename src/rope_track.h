#pragma once

// Rope tracks, pure data.
//
// Every engine rope attached to a recorded entity (reins, lead rope, wagon
// traces, lasso) is a record: its type and vertex count, the entities each
// attachment entry held over time (as replay ids and bones), the held flag
// changes, and its vertices at the capture rate. Engine rope ids are never
// reused within a recording. The engine side (snapshot, attachment setter,
// logging) is replay_ropes; this file only knows the data.

#include "entity_lifecycle.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace replay::ropes
{
    // A recorded rope keeps its record, and its history, for the whole
    // recording: nothing gives a record away (the old cap of 128 records
    // handed a far rope's slot to a near one, its history gone from the
    // clip, and refused new ropes; a town filled it in 8 s). Engine rope ids
    // are never reused, so the records grow with every rope that passed; the
    // recording's byte budget bounds them (replay_buffer.h ApproxBytes counts
    // RopeRecordsBytes) and the buffer's ring frees the ropes it left
    // (DropRopeRecordsBefore). What is bounded is the ropes alive at once: the
    // engine's rope manager holds 128 (0xC26100: cmp [mgr+0xC], 0x80), so a
    // new rope is refused only with that many records live (RopeRecordFits).
    constexpr std::size_t kEngineRopePoolCapacity = 128;
    // The read limit of a clip (a damaged count fails the read). The old
    // clips hold at most 128.
    constexpr std::size_t kMaxRopeRecordsInFile = 1u << 20;
    constexpr std::uint32_t kMaxRopeVertices = 1024;

    struct RopeAttach
    {
        entity::ReplayId id; // invalid: not a recorded entity
        std::int32_t bone = -1;
    };

    // One attachment entry's entity (an attach or detach event): a recorded
    // entity, nothing, or an entity outside the
    // recording (left as the puppet rope has it).
    enum class RopeHolder : std::uint8_t
    {
        Recorded = 0,
        Empty = 1,
        Other = 2,
    };

    struct RopeSlotEvent
    {
        std::uint32_t timeMs = 0;
        std::uint32_t slot = 0;
        RopeHolder holder = RopeHolder::Empty;
        entity::ReplayId id;
        std::int32_t bone = -1;
    };

    struct RopeRecord
    {
        std::uint32_t engineId = 0;
        std::uint32_t type = 0;
        std::uint32_t vertices = 0;
        std::vector<RopeAttach> attachments;  // when it entered
        entity::ReplayId nearPed;             // the ped whose hand held it when it entered (evidence)
        std::vector<RopeSlotEvent> slotEvents; // per slot, in time order
        std::vector<RopeSlotEvent> slotState;  // recording: last per slot
        // Held flag changes (time, on), the first one when it entered.
        std::vector<std::pair<std::uint32_t, bool>> heldEvents;
        std::vector<std::uint32_t> times;
        // Per sample, the rope's drawn window resampled to `vertices`
        // points (times.size() * vertices * 3) and the window's own
        // vertex count (the engine's drawn count at that time).
        std::vector<float> positions;
        std::vector<std::uint32_t> windowCounts;
        bool playbackLogged = false;
        std::uint32_t nextStateLogMs = 0; // recording evidence pacing
        // The last recording frame the engine rope was seen with a recorded
        // entity in an entry (or, a rope recorded for the ped next to it,
        // seen at all). Recording only, not saved.
        std::uint32_t lastSeenMs = 0;
        // The distance of the rope's nearest end to the recording's focus (the player or the gameplay
        // camera, the nearest of the two) at the last frame it was in the
        // engine's list; -1 when unknown. Recording only, not saved.
        float focusDistance = -1.0f;
        // Vertex samples not stored because the rope stood still (the last
        // sample's time moved instead, AppendOrExtendVertices). Evidence.
        std::uint32_t stillSamples = 0;
    };

    // Playback queries.
    bool InSpan(const RopeRecord& record, std::uint32_t timeMs);
    bool HeldAt(const RopeRecord& record, std::uint32_t timeMs);
    // The slot's last event at or before timeMs, null when none yet.
    const RopeSlotEvent* SlotStateAt(const RopeRecord& record, std::uint32_t slot, std::uint32_t timeMs);

    // Playback guard for one attachment entry of a followed engine rope.
    // Some entries are written back by the engine every frame (the reins of
    // a wagon: its own harness code owns them). After kSlotRevertLimit
    // writes in a row that
    // did not hold, the entry is left to the engine until the recorded
    // state changes (a new event, or the playhead moving to another one).
    constexpr std::uint32_t kSlotRevertLimit = 3;
    struct SlotGuard
    {
        std::uint32_t engineId = 0;
        std::uint32_t slot = 0;
        std::uint32_t stateTimeMs = 0xFFFFFFFFu; // the recorded event being played
        bool wrote = false;
        std::uint32_t reverts = 0;
        bool yielded = false;
    };
    enum class SlotWrite : std::uint8_t
    {
        Hold,    // the entry already has the recorded entity
        Write,   // write it
        Yield,   // the engine undid the last writes: stop now (log once)
        Yielded, // left to the engine for this recorded state
    };
    // `matches`: the entry holds the recorded entity now.
    SlotWrite DecideSlotWrite(SlotGuard& guard, std::uint32_t stateTimeMs, bool matches);

    // Playback check of the recorded pins held by another entity than the
    // rope's own (slot 0's): a rider's or a driver's hands, a wagon. Each
    // followed rope tick, before
    // the write: does the engine entry hold the recorded entity? An engine
    // update that puts the pin back every frame (the draft reins rest mode)
    // shows as a low held share and yields.
    struct PinTally
    {
        std::uint64_t slotTicks = 0;
        std::uint64_t held = 0;
        std::uint64_t engineOther = 0;
        std::uint64_t yields = 0; // recorded states left to the engine (three writes undone)
        void Note(bool matches, SlotWrite decision)
        {
            ++slotTicks;
            if (matches)
                ++held;
            else
                ++engineOther;
            if (decision == SlotWrite::Yield)
                ++yields;
        }
        double HeldPct() const { return slotTicks ? 100.0 * static_cast<double>(held) / static_cast<double>(slotTicks) : 0.0; }
        // "none", "engine_owned" (a recorded pin was yielded), "engine_fights"
        // (held under 90% of the ticks), "ok".
        const char* Verdict() const
        {
            if (!slotTicks)
                return "none";
            if (yields)
                return "engine_owned";
            return HeldPct() >= 90.0 ? "ok" : "engine_fights";
        }
    };

    // The recorded shape on a followed engine rope. The entries alone do not
    // always say who holds the rope (a rider's reins can be held through
    // another engine path), while the recorded vertices always have it in
    // the hand. So playback writes the recorded window into the puppet's
    // rope every frame, in play, pause and scrub, and the recorded entries
    // still go through the setter so the engine's own pins agree.
    enum class ShapeApply : std::uint8_t
    {
        Apply,          // same vertex count on both sides: set 1:1
        NoSample,       // outside the recorded samples
        WindowMismatch, // a drawn window of another size (a lasso): left to the entries
    };
    ShapeApply DecideRecordedShape(const RopeRecord& record, std::uint32_t timeMs, std::uint32_t targetWindow);
    // Evidence: the distance of the nearest vertex to a point (a hand).
    float NearestVertexDistance(const std::vector<float>& xyz, const float point[3]);

    // The replay buffer frees the records of ropes that left its ring, like
    // the pose chunks and the fx frames (replay_buffer.h TrimBefore): a
    // record lives while its rope is in the ring, so the records do not grow
    // with every horse and wagon that passed since the buffer started. Only a
    // rope whose last activity is before the ring's head goes: what the ring
    // keeps is never cut.
    struct RopeTrimStats
    {
        std::size_t records = 0; // records freed (their rope gone before the base)
        std::size_t samples = 0; // vertex samples dropped from the records kept
        std::size_t events = 0;  // held and entry events dropped from them
    };
    // The record's last activity: its last vertex sample, held or entry
    // event, or lastSeenMs.
    std::uint32_t LastActivityMs(const RopeRecord& record);
    // Frees the records whose last activity is before keepFrom (the buffer's
    // exportable base: a saved window drops them too, recording_window.cpp)
    // and trims the others to what a cut at keepFrom needs: the vertex
    // sample, the held state and each entry's state in force there, and
    // everything after.
    RopeTrimStats DropRopeRecordsBefore(std::vector<RopeRecord>& records, std::uint32_t keepFrom);

    // ---- Which ropes take the records

    // A rope enters the records when an attachment entry holds a recorded
    // entity, or when an end of it is in a recorded ped's hand (a lasso
    // before it catches anything). The hand, not the ped's position, so a
    // clothes line or fence rope a ped walks by is not recorded.
    constexpr float kRopeHandRadiusM = 1.5f;
    // Fallback when the hand cannot be read (no hand bone): a radius around
    // the ped.
    constexpr float kRopePedRadiusM = 3.0f;
    // A record not seen for this long is no longer live (RopeRecordFits).
    constexpr std::uint32_t kRopeStaleMs = 2000;
    // Two samples of the drawn window within this (per vertex, metres) are
    // the same pose (AppendOrExtendVertices).
    constexpr float kRopeStillM = 0.005f;

    struct RopeCandidate
    {
        bool holdsRecorded = false; // an attachment entry holds a recorded entity
        bool inHand = false;        // an end within kRopeHandRadiusM of a recorded ped's hand
        bool nearPedNoHand = false; // no hand to test: within kRopePedRadiusM of a recorded ped
    };
    inline bool RopeRelevant(const RopeCandidate& candidate)
    {
        return candidate.holdsRecorded || candidate.inHand || candidate.nearPedNoHand;
    }

    // The smallest distance of the two ends to the focus points (count 0 to
    // 2); -1 when there is no focus.
    float RopeFocusDistance(const float first[3], const float last[3], const float (*focus)[3], std::size_t focusCount);

    // The records live at timeMs: seen within kRopeStaleMs (lastSeenMs).
    std::size_t LiveRopeRecords(const std::vector<RopeRecord>& records, std::uint32_t timeMs);
    // A new rope takes a record unless the engine's whole rope pool is already
    // live in the records (kEngineRopePoolCapacity). No record is ever given
    // away for it.
    bool RopeRecordFits(const std::vector<RopeRecord>& records, std::uint32_t timeMs);
    // What the records hold in memory (the replay buffer's byte budget).
    std::size_t RopeRecordsBytes(const std::vector<RopeRecord>& records);

    // A record held only by a ped's hand (nearPed and no recorded entry)
    // is seen while an end stays in that hand (or, without a hand, near
    // the ped); a record with a recorded entry, while the entry holds it.
    bool RopeStillSeen(bool holdsRecorded, bool nearPedRecord, bool inHand);

    // Recording. NoteHeld stores a change of the held flag (the first sample
    // counts) and says whether it did.
    bool NoteHeld(RopeRecord& record, std::uint32_t timeMs, bool held);
    // NoteSlot stores the slot's entity/bone when it differs from the last
    // state: First for the slot's first state, Changed for a later change
    // (the evidence-worthy one), Unchanged when nothing was stored.
    enum class SlotNote : std::uint8_t
    {
        Unchanged,
        First,
        Changed,
    };
    SlotNote NoteSlot(RopeRecord& record, const RopeSlotEvent& state);
    // Whether a vertex sample is due: same vertex count as the record and
    // the capture interval elapsed since the last one.
    bool WantsVertices(const RopeRecord& record, std::uint32_t ropeVertices,
        std::uint32_t timeMs, std::uint32_t intervalMs);
    // Stores one sample: the drawn window (x/y/z packed, any count >= 2)
    // resampled to the record's vertex count.
    void AppendVertices(RopeRecord& record, std::uint32_t timeMs, const std::vector<float>& window);
    // The same, but a rope standing still (the
    // reins of a hitched horse, the traces of a parked wagon: most of a
    // town's ropes) does not grow its samples: when the new window is within
    // kRopeStillM of the last sample, and the last one already repeats the
    // one before it, the last sample's time moves to timeMs (the blend
    // between two equal samples is the same pose, VerticesAt and the trim
    // see the same span). Returns true when a sample was stored.
    bool AppendOrExtendVertices(RopeRecord& record, std::uint32_t timeMs, const std::vector<float>& window);
    // The window vertex count of the sample at or before timeMs (0 when none).
    std::uint32_t WindowCountAt(const RopeRecord& record, std::uint32_t timeMs);

    // Playback of a rope with no engine counterpart (a thrown lasso): the
    // recorded vertices at the playhead, linearly blended between the two
    // samples around it (x/y/z packed). False outside the samples.
    bool VerticesAt(const RopeRecord& record, std::uint32_t timeMs, std::vector<float>& xyz);
    // The same polyline with `count` vertices, spaced along the index.
    void Resample(const std::vector<float>& xyz, std::uint32_t count, std::vector<float>& out);
    // The same, on the Catmull-Rom spline through the vertices (the engine
    // draws a rope as a smooth curve through its vertices, so a denser rope
    // set on straight chords looks polygonal; this keeps the curve).
    void ResampleSmooth(const std::vector<float>& xyz, std::uint32_t count, std::vector<float>& out);
    // Vertices in use (evidence): the longest chain (neighbour steps up to
    // maxStep) with an end within maxStep of vertex 0, ordered from that
    // end, after vertex 0 itself. Returns the kept vertex count and
    // rewrites xyz; only vertex 0 when no chain touches it.
    std::uint32_t KeepAnchoredRun(std::vector<float>& xyz, float maxStep);
    // Largest distance of a vertex from the first one (a coiled rope is small).
    float Extent(const std::vector<float>& xyz);
    // Longest in-use polyline length over all samples (the rope's working length).
    float MaxPathLength(const RopeRecord& record, float maxStep);

    // Engine ropes the recorder looked at and did not record (the reins of
    // every unrecorded horse in a town). Evaluating them costs an id lookup
    // per entry and three natives per ped, so each is evaluated once, again
    // when its attachments change (`key`: the entries' entities and bones),
    // and all of them every kRecheckMs (a recorded ped walking up to a rope,
    // or a rope entering its hand, is seen within that).
    class RejectedRopes
    {
    public:
        static constexpr std::uint32_t kRecheckMs = 250;

        void Reset()
        {
            m_ropes.clear();
            m_nextRecheckMs = 0;
        }
        // Once per recording frame, before the ropes: true when every
        // rejected rope is evaluated again this frame (the list is dropped).
        bool BeginFrame(std::uint32_t timeMs)
        {
            if (timeMs < m_nextRecheckMs && m_nextRecheckMs - timeMs <= kRecheckMs)
                return false;
            m_nextRecheckMs = timeMs + kRecheckMs;
            m_ropes.clear();
            return true;
        }
        // Rejected before with the same attachments: skip it this frame.
        bool Skip(std::uint32_t engineId, std::uint64_t key) const
        {
            for (const auto& [id, seen] : m_ropes)
            {
                if (id == engineId)
                    return seen == key;
            }
            return false;
        }
        void Reject(std::uint32_t engineId, std::uint64_t key)
        {
            for (auto& [id, seen] : m_ropes)
            {
                if (id == engineId)
                {
                    seen = key;
                    return;
                }
            }
            if (m_ropes.size() < kMaxRejected)
                m_ropes.push_back({engineId, key});
        }
        std::size_t Size() const { return m_ropes.size(); }

    private:
        static constexpr std::size_t kMaxRejected = 512;
        std::vector<std::pair<std::uint32_t, std::uint64_t>> m_ropes;
        std::uint32_t m_nextRecheckMs = 0;
    };

    // Key of a rope's attachment entries (FNV-1a over entity and bone).
    inline std::uint64_t AttachKey(const std::uint64_t* entities, const std::int32_t* bones, std::uint32_t count)
    {
        std::uint64_t hash = 1469598103934665603ull;
        const auto mix = [&hash](std::uint64_t value) {
            for (int byte = 0; byte < 8; ++byte)
            {
                hash ^= (value >> (byte * 8)) & 0xFF;
                hash *= 1099511628211ull;
            }
        };
        mix(count);
        for (std::uint32_t n = 0; n < count; ++n)
        {
            mix(entities[n]);
            mix(static_cast<std::uint32_t>(bones[n]));
        }
        return hash;
    }
}
