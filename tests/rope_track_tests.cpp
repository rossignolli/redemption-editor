#include "rope_track.h"
#include "reins_guard.h"

#include <cmath>
#include <cstdio>

namespace
{
    using namespace replay::ropes;
    using replay::entity::ReplayId;

    int g_ropeFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_ropeFailures;
    }

    RopeSlotEvent Slot(std::uint32_t timeMs, std::uint32_t slot, RopeHolder holder, ReplayId id, std::int32_t bone)
    {
        RopeSlotEvent event;
        event.timeMs = timeMs;
        event.slot = slot;
        event.holder = holder;
        event.id = id;
        event.bone = bone;
        return event;
    }

    void TestSpanAndVertices()
    {
        RopeRecord record;
        record.vertices = 2;
        Check(!InSpan(record, 0), "no samples: never in span");
        Check(!WantsVertices(record, 3, 0, 33), "other vertex count is not sampled");
        Check(WantsVertices(record, 2, 0, 33), "first sample is due");
        AppendVertices(record, 100, {0.f, 0.f, 0.f, 1.f, 1.f, 1.f});
        Check(!WantsVertices(record, 2, 120, 33), "rate gate holds inside the interval");
        Check(WantsVertices(record, 2, 133, 33), "rate gate opens at the interval");
        AppendVertices(record, 133, {0.f, 0.f, 0.f, 2.f, 2.f, 2.f});
        Check(record.times.size() == 2 && record.positions.size() == 12, "samples appended");
        Check(!InSpan(record, 99) && InSpan(record, 100) && InSpan(record, 120) && InSpan(record, 133) &&
                !InSpan(record, 134),
            "span is [first, last] sample");
    }

    void TestHeld()
    {
        RopeRecord record;
        Check(NoteHeld(record, 0, false), "first held state counts");
        Check(!NoteHeld(record, 33, false), "same state is not stored");
        Check(NoteHeld(record, 500, true), "change stored");
        Check(NoteHeld(record, 900, false), "change back stored");
        Check(!HeldAt(record, 0) && !HeldAt(record, 499) && HeldAt(record, 500) && HeldAt(record, 899) &&
                !HeldAt(record, 900) && !HeldAt(record, 5000),
            "held follows the last change at or before the time");
    }

    void TestSlots()
    {
        RopeRecord record;
        const ReplayId horse{1, 1};
        const ReplayId rider{2, 1};
        Check(NoteSlot(record, Slot(0, 4, RopeHolder::Recorded, horse, 7434)) == SlotNote::First,
            "first state of a slot");
        Check(NoteSlot(record, Slot(33, 4, RopeHolder::Recorded, horse, 7434)) == SlotNote::Unchanged,
            "same entity and bone: nothing stored");
        Check(NoteSlot(record, Slot(0, 6, RopeHolder::Empty, {}, 11300)) == SlotNote::First,
            "another slot starts on its own");
        Check(NoteSlot(record, Slot(1000, 4, RopeHolder::Recorded, rider, 509)) == SlotNote::Changed,
            "reins move to the rider's hand");
        Check(NoteSlot(record, Slot(2000, 4, RopeHolder::Other, {}, 509)) == SlotNote::Changed,
            "an entity outside the recording is a change too");
        Check(record.slotEvents.size() == 4 && record.slotState.size() == 7, "events and per-slot state kept");

        Check(SlotStateAt(record, 4, 0)->id == horse, "slot 4 starts on the horse");
        Check(SlotStateAt(record, 4, 999)->id == horse, "still the horse before the change");
        Check(SlotStateAt(record, 4, 1000)->id == rider && SlotStateAt(record, 4, 1000)->bone == 509,
            "rider from the change on");
        Check(SlotStateAt(record, 4, 2500)->holder == RopeHolder::Other, "latest state wins");
        Check(SlotStateAt(record, 6, 0)->holder == RopeHolder::Empty, "empty slot state");
        Check(SlotStateAt(record, 5, 5000) == nullptr, "slot without events");
    }

    // An entry the engine writes back is given up after three writes that
    // did not hold; a new recorded state tries again.
    void TestSlotGuard()
    {
        SlotGuard guard;
        Check(DecideSlotWrite(guard, 100, true) == SlotWrite::Hold, "entry already right: nothing written");
        Check(DecideSlotWrite(guard, 100, false) == SlotWrite::Write, "first write");
        Check(DecideSlotWrite(guard, 100, false) == SlotWrite::Write, "reverted once: written again");
        Check(DecideSlotWrite(guard, 100, false) == SlotWrite::Write, "reverted twice: written again");
        Check(DecideSlotWrite(guard, 100, false) == SlotWrite::Yield, "reverted three times: left to the engine");
        Check(DecideSlotWrite(guard, 100, false) == SlotWrite::Yielded, "stays with the engine");
        Check(DecideSlotWrite(guard, 250, false) == SlotWrite::Write, "a new recorded state is written");
        Check(DecideSlotWrite(guard, 250, true) == SlotWrite::Hold, "it held");
        Check(DecideSlotWrite(guard, 250, false) == SlotWrite::Write, "changed later: written again");
        Check(DecideSlotWrite(guard, 250, true) == SlotWrite::Hold, "held again");
        Check(DecideSlotWrite(guard, 250, false) == SlotWrite::Write &&
                DecideSlotWrite(guard, 250, false) == SlotWrite::Write,
            "a revert after a hold starts a new count");
    }

    // Reins of a galloping horse: the recorded window goes into the followed
    // engine rope only 1:1 and only inside the recorded samples; the hand
    // evidence is the nearest vertex.
    void TestRecordedShape()
    {
        RopeRecord record;
        record.vertices = 3;
        Check(DecideRecordedShape(record, 0, 3) == ShapeApply::NoSample, "no samples: nothing to apply");
        AppendVertices(record, 100, {0, 0, 0, 1, 0, 0, 2, 0, 0});
        AppendVertices(record, 200, {0, 0, 0, 1, 1, 0, 2, 0, 0});
        Check(DecideRecordedShape(record, 50, 3) == ShapeApply::NoSample, "before the first sample");
        Check(DecideRecordedShape(record, 250, 3) == ShapeApply::NoSample, "after the last sample");
        Check(DecideRecordedShape(record, 100, 3) == ShapeApply::Apply, "first sample: applied (the first frame)");
        Check(DecideRecordedShape(record, 150, 3) == ShapeApply::Apply, "between samples: applied");
        Check(DecideRecordedShape(record, 150, 2) == ShapeApply::WindowMismatch,
            "the engine rope draws another window: not applied");
        RopeRecord windowed;
        windowed.vertices = 5;
        AppendVertices(windowed, 0, {0, 0, 0, 4, 0, 0});
        Check(DecideRecordedShape(windowed, 0, 5) == ShapeApply::WindowMismatch,
            "a recorded window resampled to the record (lasso pay-out) is not set 1:1");
        const float hand[3] = {1, 0.4f, 0};
        std::vector<float> xyz;
        Check(VerticesAt(record, 150, xyz) && std::fabs(NearestVertexDistance(xyz, hand) - 0.1f) < 1e-5f,
            "nearest vertex to the hand (the middle vertex, blended)");
        Check(NearestVertexDistance({}, hand) < 0.0f, "no vertices: no distance");
    }

    void TestOwnRopeGeometry()
    {
        RopeRecord record;
        record.vertices = 3;
        AppendVertices(record, 100, {0, 0, 0, 1, 0, 0, 2, 0, 0});
        AppendVertices(record, 200, {0, 0, 0, 0, 1, 0, 0, 2, 0});
        Check(record.windowCounts.size() == 2 && record.windowCounts[0] == 3, "each sample keeps its window count");
        Check(WindowCountAt(record, 50) == 0 && WindowCountAt(record, 150) == 3, "window count at the playhead");
        std::vector<float> xyz;
        Check(!VerticesAt(record, 50, xyz) && !VerticesAt(record, 250, xyz), "no vertices outside the samples");
        Check(VerticesAt(record, 100, xyz) && xyz[6] == 2.0f && xyz[7] == 0.0f, "a sample time gives the sample");
        Check(VerticesAt(record, 150, xyz) && xyz[6] == 1.0f && xyz[7] == 1.0f, "between samples the vertices blend");
        Check(VerticesAt(record, 200, xyz) && xyz[7] == 2.0f, "the last sample is reachable");

        std::vector<float> out;
        Resample({0, 0, 0, 2, 0, 0}, 3, out);
        Check(out.size() == 9 && out[3] == 1.0f && out[6] == 2.0f, "resampling spreads along the polyline");
        Resample({0, 0, 0, 1, 0, 0, 2, 0, 0, 3, 0, 0, 4, 0, 0}, 2, out);
        Check(out.size() == 6 && out[0] == 0.0f && out[3] == 4.0f, "resampling keeps both ends");

        // A circle sampled at 8 vertices, resampled to 64: the spline stays
        // near the circle (0.96 at the clamped ends) where straight chords
        // would sag to 0.92.
        std::vector<float> circle;
        for (int n = 0; n < 9; ++n)
        {
            const float a = 2.0f * 3.14159265f * static_cast<float>(n % 8) / 8.0f;
            circle.push_back(std::cos(a));
            circle.push_back(std::sin(a));
            circle.push_back(0.0f);
        }
        ResampleSmooth(circle, 64, out);
        float low = 2.0f;
        for (std::size_t v = 0; v < 64; ++v)
            low = (std::min)(low, std::sqrt(out[v * 3] * out[v * 3] + out[v * 3 + 1] * out[v * 3 + 1]));
        Check(out.size() == 192 && low > 0.95f && out[0] == 1.0f && std::fabs(out[189] - 1.0f) < 1e-5f,
            "smooth resampling keeps the curve through the vertices and both ends");
        ResampleSmooth({0, 0, 0, 2, 0, 0}, 3, out);
        Check(out.size() == 9 && out[3] == 1.0f, "two vertices resample linearly");

        Check(Extent({0, 0, 0, 0.1f, 0, 0, 0, 0.2f, 0}) < 0.3f, "a coiled rope has a small extent");
        Check(Extent({0, 0, 0, 3, 4, 0}) == 5.0f, "extent is the farthest vertex from the first");
        Check(MaxPathLength(record, 1.5f) == 2.0f, "path length is the longest sample polyline");

        // Layout seen live: anchor, stale, the paid-out run, stale.
        std::vector<float> lasso = {0, 0, 0, -700, -1900, 0, -700, -1899, 0, 0.5f, 0, 0, 1, 0, 0, 1.5f, 0, 0,
            -700, -1898, 0, -700, -1897, 0, -700, -1896, 0, -700, -1895, 0};
        Check(KeepAnchoredRun(lasso, 1.5f) == 4 && lasso[3] == 0.5f && lasso[9] == 1.5f,
            "the run touching the anchor is kept, stale chains are dropped");
        std::vector<float> reversed = {0, 0, 0, 2, 0, 0, 1, 0, 0};
        Check(KeepAnchoredRun(reversed, 1.5f) == 3 && reversed[3] == 1.0f && reversed[6] == 2.0f,
            "a run touching the anchor at its far end is reversed");
        std::vector<float> whole = {0, 0, 0, 1, 0, 0, 2, 0, 0};
        Check(KeepAnchoredRun(whole, 1.5f) == 3, "a continuous rope keeps every vertex");
        // A window shorter than the record is resampled to the record's count.
        RopeRecord windowed;
        windowed.vertices = 5;
        AppendVertices(windowed, 0, {0, 0, 0, 4, 0, 0});
        Check(windowed.positions.size() == 15 && std::fabs(windowed.positions[6] - 2.0f) < 1e-5f && windowed.windowCounts[0] == 2,
            "a short window is spread over the record's vertices");
        std::vector<float> lone = {0, 0, 0, 500, 0, 0, 501, 0, 0};
        Check(KeepAnchoredRun(lone, 1.5f) == 1, "no chain near the anchor keeps only the anchor");
        RopeRecord stale;
        stale.vertices = 3;
        AppendVertices(stale, 0, {0, 0, 0, 1, 0, 0, 900, 0, 0});
        Check(MaxPathLength(stale, 1.5f) == 1.0f, "path length ignores stale vertices");
    }
}

namespace
{
    // The buffer must not keep a record for every rope since the session
    // started (the records would grow with every horse that passed). The trim
    // frees the ropes gone before the ring's base and keeps the state in
    // force there.
    void TestBufferFreesRecords()
    {
        const ReplayId horse{3, 30};
        // A filled cap of 64 (the cap is now 128; the trim test keeps this case).
        constexpr std::uint32_t kFilled = 64;
        std::vector<RopeRecord> records;
        for (std::uint32_t n = 0; n < kFilled; ++n)
        {
            RopeRecord record;
            record.engineId = n + 1;
            record.vertices = 2;
            NoteHeld(record, n * 1000, false);
            NoteSlot(record, Slot(n * 1000, 0, RopeHolder::Recorded, horse, 7));
            AppendVertices(record, n * 1000, {0.f, 0.f, 0.f, 1.f, 0.f, 0.f});
            record.lastSeenMs = n * 1000 + 500; // gone half a second after its sample
            records.push_back(record);
        }
        Check(records.size() == kFilled, "the old cap is full");
        // A live rope: old samples and events, still seen now.
        RopeRecord& live = records[0];
        live.lastSeenMs = 120000;
        AppendVertices(live, 10000, {0.f, 0.f, 0.f, 2.f, 0.f, 0.f});
        AppendVertices(live, 50000, {0.f, 0.f, 0.f, 3.f, 0.f, 0.f});
        AppendVertices(live, 70000, {0.f, 0.f, 0.f, 4.f, 0.f, 0.f});
        NoteHeld(live, 20000, true);
        NoteHeld(live, 80000, false);
        NoteSlot(live, Slot(30000, 0, RopeHolder::Empty, {}, -1));
        NoteSlot(live, Slot(40000, 2, RopeHolder::Recorded, horse, 9));
        NoteSlot(live, Slot(90000, 0, RopeHolder::Recorded, horse, 7));
        std::vector<float> before;
        Check(VerticesAt(live, 60000, before), "live rope sampled before the trim");
        const RopeRecord liveCopy = live;

        const RopeTrimStats stats = DropRopeRecordsBefore(records, 60000);
        Check(stats.records == kFilled - 5, "ropes gone before the base are freed (59 of 64)");
        Check(records.size() < kFilled, "the cap has room again");
        bool liveKept = false;
        for (const RopeRecord& record : records)
        {
            Check(LastActivityMs(record) >= 60000, "every kept record reaches the base");
            liveKept = liveKept || record.engineId == 1;
        }
        Check(liveKept, "a rope still seen keeps its record");
        const RopeRecord* trimmed = nullptr;
        for (const RopeRecord& record : records)
            if (record.engineId == 1)
                trimmed = &record;
        if (!trimmed)
            return;
        Check(trimmed->times.size() == 2 && trimmed->times.front() == 50000, "the sample in force at the base stays");
        Check(trimmed->positions.size() == trimmed->times.size() * 6 && trimmed->windowCounts.size() == trimmed->times.size(),
            "positions and window counts follow the samples");
        std::vector<float> after;
        Check(VerticesAt(*trimmed, 60000, after) && after == before, "the pose at the base is unchanged");
        for (std::uint32_t t : {60000u, 79999u, 80000u, 120000u})
            Check(HeldAt(*trimmed, t) == HeldAt(liveCopy, t), "held state after the base is unchanged");
        for (std::uint32_t slot : {0u, 2u})
            for (std::uint32_t t : {60000u, 89999u, 90000u})
            {
                const RopeSlotEvent* a = SlotStateAt(*trimmed, slot, t);
                const RopeSlotEvent* b = SlotStateAt(liveCopy, slot, t);
                Check(a && b && a->holder == b->holder && a->bone == b->bone, "entry state after the base is unchanged");
            }
        Check(stats.samples == 2 && stats.events > 0, "the trim counts what it dropped");
        // Freed again: nothing left to drop at the same base.
        const RopeTrimStats again = DropRopeRecordsBefore(records, 60000);
        Check(again.records == 0 && again.samples == 0 && again.events == 0, "the trim is idempotent");
    }
}

namespace
{
    // The controller's put-back on the saddle is skipped only for the ropes
    // the playback follows (rope 307 followed as engine rope 6111 at 117197 ms).
    void TestReinsGuard()
    {
        using replay::reins::SkipRestore;
        const std::uint64_t followed[] = {0x24698a45000ull, 0x24698a5b000ull};
        Check(SkipRestore(0x24698a45000ull, followed, 2), "followed reins: put-back skipped");
        Check(!SkipRestore(0x24698a99000ull, followed, 2), "world reins: put-back forwarded");
        Check(!SkipRestore(0, followed, 2), "no rope: forwarded");
        Check(!SkipRestore(0x24698a45000ull, followed, 0), "outside playback (empty set): forwarded");
    }
}

namespace
{
    // Live ropes in a town (hitched horses and parked wagons, all attached to
    // recorded entities) can fill the records. A town's worth of ropes is
    // simulated here: which ones take the records and what a full cap does.
    RopeRecord Far(std::uint32_t id, float distance, std::uint32_t lastSeenMs)
    {
        RopeRecord record;
        record.engineId = id;
        record.vertices = 2;
        record.focusDistance = distance;
        record.lastSeenMs = lastSeenMs;
        return record;
    }

    void TestRopePriority()
    {
        // Relevance.
        RopeCandidate world;
        Check(!RopeRelevant(world), "relevance: a world rope with no recorded entity and no hand is not recorded");
        RopeCandidate attached;
        attached.holdsRecorded = true;
        Check(RopeRelevant(attached), "relevance: a rope on a recorded horse or wagon is");
        RopeCandidate lasso;
        lasso.inHand = true;
        Check(RopeRelevant(lasso), "relevance: a rope in a recorded ped's hand is");
        // Seen: a rope recorded for a hand counts while in the hand.
        Check(RopeStillSeen(true, false, false), "seen: an entry holds a recorded entity");
        Check(RopeStillSeen(false, true, true), "seen: the lasso still in the hand");
        Check(!RopeStillSeen(false, true, false), "seen: a world rope the ped walked away from is not (the trim frees it)");
        // Focus distance: the nearest end to the nearest focus.
        const float first[3] = {100.f, 0.f, 0.f};
        const float last[3] = {10.f, 0.f, 0.f};
        const float focus[2][3] = {{0.f, 0.f, 0.f}, {0.f, 4.f, 0.f}};
        Check(std::fabs(RopeFocusDistance(first, last, focus, 2) - 10.f) < 0.01f, "focus: nearest end, nearest point");
        Check(RopeFocusDistance(first, last, focus, 0) < 0.f, "focus: none known");

        // A town: 150 hitched horses (20 to 243 m), then 6 wagons (10 ropes
        // each) driving past. Every rope takes a record and keeps it: the old
        // cap of 128 gave far records away (their history gone from the clip)
        // and refused the rest. Only the ropes alive at once are bounded, by
        // the engine's rope pool.
        Check(kEngineRopePoolCapacity == 128 && kMaxRopeRecordsInFile >= 128, "pool: 128 alive at once, the read takes old and new clips");
        std::vector<RopeRecord> session;
        std::uint32_t refused = 0;
        const auto offer = [&](std::uint32_t id, float distance, std::uint32_t t) {
            for (RopeRecord& record : session)
                if (record.engineId == id)
                {
                    record.focusDistance = distance;
                    record.lastSeenMs = t;
                    return true;
                }
            if (!RopeRecordFits(session, t))
            {
                ++refused;
                return false;
            }
            session.push_back(Far(id, distance, t));
            AppendVertices(session.back(), t, {0.f, 0.f, 0.f, 1.f, 0.f, 0.f});
            return true;
        };
        // The town's horses come and go (at most 100 alive at once).
        for (std::uint32_t n = 0; n < 150; ++n)
            offer(1000 + n, 20.f + static_cast<float>(n) * 1.5f, n < 100 ? 0 : 3000);
        bool wagonsRecorded = true;
        for (std::uint32_t wagon = 0; wagon < 6; ++wagon)
            for (std::uint32_t rope = 0; rope < 10; ++rope)
                wagonsRecorded = offer(5000 + wagon * 10 + rope, 8.f, 6000 + wagon * 1000) && wagonsRecorded;
        Check(wagonsRecorded && refused == 0, "town: every rope is recorded");
        Check(session.size() == 210, "town: 210 records, past the old cap of 128");
        bool historyKept = true;
        for (std::uint32_t n = 0; n < 150; ++n)
            historyKept = historyKept && session[n].engineId == 1000 + n && session[n].times.size() == 1;
        Check(historyKept, "town: no record is given away, every first sample is still there");
        Check(LiveRopeRecords(session, 12500) == 10, "town: only the last wagon's ropes are live at the end");
        // The pool: with 128 records live a new rope is refused (the engine
        // cannot hold more), and nothing is erased for it.
        std::vector<RopeRecord> full;
        for (std::uint32_t n = 0; n < kEngineRopePoolCapacity; ++n)
            full.push_back(Far(n + 1, 5.f, 10000));
        full.push_back(Far(900, 5.f, 1000)); // an old rope, gone
        Check(LiveRopeRecords(full, 10500) == kEngineRopePoolCapacity && !RopeRecordFits(full, 10500),
            "pool: the whole pool live refuses a new rope");
        Check(full.size() == kEngineRopePoolCapacity + 1, "pool: and erases nothing");
        Check(RopeRecordFits(full, 12500), "pool: once they are stale a new rope takes a record");
        Check(RopeRecordFits(std::vector<RopeRecord>(kEngineRopePoolCapacity - 1, Far(1, 5.f, 10000)), 10000),
            "pool: under the pool always fits");
        // The bytes the budget counts: every sample.
        RopeRecord sampled = Far(1, 5.f, 0);
        sampled.vertices = 2;
        AppendVertices(sampled, 0, {0.f, 0.f, 0.f, 1.f, 0.f, 0.f});
        AppendVertices(sampled, 33, {0.f, 0.f, 0.f, 2.f, 0.f, 0.f});
        const std::vector<RopeRecord> one{sampled};
        Check(RopeRecordsBytes(one) >= sizeof(RopeRecord) + 2 * 6 * sizeof(float) + 2 * 2 * sizeof(std::uint32_t),
            "bytes: the record and its samples");
    }

    // A still rope (most of a town) stores two samples, the last one retimed;
    // the pose at every time is unchanged.
    void TestStillSamples()
    {
        RopeRecord record;
        record.vertices = 2;
        const std::vector<float> a = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f};
        const std::vector<float> b = {0.f, 0.f, 0.f, 1.f, 1.f, 0.f};
        Check(AppendOrExtendVertices(record, 0, a), "still: first sample stored");
        Check(AppendOrExtendVertices(record, 33, a), "still: second sample stored (it proves the rope stands)");
        for (std::uint32_t t = 66; t <= 3300; t += 33)
            Check(!AppendOrExtendVertices(record, t, a), "still: later equal samples move the last time");
        Check(record.times.size() == 2 && record.times.back() == 3300 && record.stillSamples == 99, "still: two samples span 0 to 3300");
        std::vector<float> at;
        Check(VerticesAt(record, 2000, at) && at == a, "still: the pose in the span");
        Check(AppendOrExtendVertices(record, 3333, b), "still: a move is stored");
        Check(VerticesAt(record, 3300, at) && at == a, "still: the pose before the move");
        Check(VerticesAt(record, 3333, at) && std::fabs(at[4] - 1.f) < 1e-4f, "still: the pose after the move");
        Check(AppendOrExtendVertices(record, 3366, b), "still: the first repeat after a move is stored");
        Check(!AppendOrExtendVertices(record, 3399, b), "still: then retimed");
        // The trim inside a still span keeps the sample in force.
        std::vector<RopeRecord> records{record};
        records[0].lastSeenMs = 3399;
        DropRopeRecordsBefore(records, 1000);
        Check(records.size() == 1 && VerticesAt(records[0], 1000, at) && at == a, "still: trimmed inside the span, same pose");
        // A window of another size is a new sample.
        RopeRecord lasso;
        lasso.vertices = 2;
        AppendOrExtendVertices(lasso, 0, a);
        AppendOrExtendVertices(lasso, 33, a);
        Check(AppendOrExtendVertices(lasso, 66, {0.f, 0.f, 0.f, 0.5f, 0.f, 0.f, 1.f, 0.f, 0.f}) && lasso.times.size() == 3,
            "still: another window count is stored");
    }
}

int RunRopeTrackTests()
{
    TestReinsGuard();
    TestBufferFreesRecords();
    TestRopePriority();
    TestStillSamples();
    TestOwnRopeGeometry();
    TestSpanAndVertices();
    TestHeld();
    TestSlots();
    TestSlotGuard();
    TestRecordedShape();
    if (g_ropeFailures == 0)
        std::printf("Rope track tests passed.\n");
    return g_ropeFailures;
}
