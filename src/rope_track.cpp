#include "rope_track.h"

#include <cmath>

#include <algorithm>
#include <initializer_list>

namespace replay::ropes
{
    bool InSpan(const RopeRecord& record, std::uint32_t timeMs)
    {
        return !record.times.empty() && timeMs >= record.times.front() && timeMs <= record.times.back();
    }

    bool HeldAt(const RopeRecord& record, std::uint32_t timeMs)
    {
        bool held = false;
        for (const auto& event : record.heldEvents)
        {
            if (event.first <= timeMs)
                held = event.second;
        }
        return held;
    }

    const RopeSlotEvent* SlotStateAt(const RopeRecord& record, std::uint32_t slot, std::uint32_t timeMs)
    {
        const RopeSlotEvent* state = nullptr;
        for (const RopeSlotEvent& event : record.slotEvents)
        {
            if (event.slot == slot && event.timeMs <= timeMs)
                state = &event;
        }
        return state;
    }

    SlotWrite DecideSlotWrite(SlotGuard& guard, std::uint32_t stateTimeMs, bool matches)
    {
        if (guard.stateTimeMs != stateTimeMs)
        {
            guard.stateTimeMs = stateTimeMs;
            guard.wrote = false;
            guard.reverts = 0;
            guard.yielded = false;
        }
        if (matches)
        {
            // It held: a later write starts a new count.
            guard.wrote = false;
            guard.reverts = 0;
            return SlotWrite::Hold;
        }
        if (guard.yielded)
            return SlotWrite::Yielded;
        if (guard.wrote && ++guard.reverts >= kSlotRevertLimit)
        {
            guard.yielded = true;
            return SlotWrite::Yield;
        }
        guard.wrote = true;
        return SlotWrite::Write;
    }

    ShapeApply DecideRecordedShape(const RopeRecord& record, std::uint32_t timeMs, std::uint32_t targetWindow)
    {
        const std::size_t stride = static_cast<std::size_t>(record.vertices) * 3;
        if (!InSpan(record, timeMs) || stride == 0 || record.positions.size() < record.times.size() * stride)
            return ShapeApply::NoSample;
        // Both windows must be the record's own count: a resampled window
        // (the lasso's pay-out) is not the engine rope's vertex order.
        if (targetWindow != record.vertices || WindowCountAt(record, timeMs) != record.vertices)
            return ShapeApply::WindowMismatch;
        return ShapeApply::Apply;
    }

    float NearestVertexDistance(const std::vector<float>& xyz, const float point[3])
    {
        float best = -1.0f;
        for (std::size_t v = 0; v + 2 < xyz.size(); v += 3)
        {
            const float dx = xyz[v] - point[0];
            const float dy = xyz[v + 1] - point[1];
            const float dz = xyz[v + 2] - point[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (best < 0.0f || d < best)
                best = d;
        }
        return best;
    }

    bool NoteHeld(RopeRecord& record, std::uint32_t timeMs, bool held)
    {
        if (!record.heldEvents.empty() && record.heldEvents.back().second == held)
            return false;
        record.heldEvents.push_back({timeMs, held});
        return true;
    }

    SlotNote NoteSlot(RopeRecord& record, const RopeSlotEvent& state)
    {
        if (record.slotState.size() <= state.slot)
            record.slotState.resize(state.slot + 1);
        RopeSlotEvent& last = record.slotState[state.slot];
        const bool first = std::none_of(record.slotEvents.begin(), record.slotEvents.end(),
            [&](const RopeSlotEvent& event) { return event.slot == state.slot; });
        if (!first && last.holder == state.holder && last.id == state.id && last.bone == state.bone)
            return SlotNote::Unchanged;
        last = state;
        record.slotEvents.push_back(state);
        return first ? SlotNote::First : SlotNote::Changed;
    }

    bool WantsVertices(const RopeRecord& record, std::uint32_t ropeVertices,
        std::uint32_t timeMs, std::uint32_t intervalMs)
    {
        if (ropeVertices != record.vertices)
            return false;
        return record.times.empty() || timeMs - record.times.back() >= intervalMs;
    }

    void AppendVertices(RopeRecord& record, std::uint32_t timeMs, const std::vector<float>& window)
    {
        static std::vector<float> resampled;
        ResampleSmooth(window, record.vertices, resampled);
        record.times.push_back(timeMs);
        record.windowCounts.push_back(static_cast<std::uint32_t>(window.size() / 3));
        record.positions.insert(record.positions.end(), resampled.begin(), resampled.end());
    }

    std::uint32_t WindowCountAt(const RopeRecord& record, std::uint32_t timeMs)
    {
        if (record.times.empty() || record.windowCounts.size() != record.times.size())
            return 0;
        const auto after = std::upper_bound(record.times.begin(), record.times.end(), timeMs);
        if (after == record.times.begin())
            return 0;
        return record.windowCounts[static_cast<std::size_t>(after - record.times.begin()) - 1];
    }

    bool VerticesAt(const RopeRecord& record, std::uint32_t timeMs, std::vector<float>& xyz)
    {
        const std::size_t samples = record.times.size();
        const std::size_t stride = static_cast<std::size_t>(record.vertices) * 3;
        if (samples == 0 || stride == 0 || record.positions.size() < samples * stride || !InSpan(record, timeMs))
            return false;
        const auto after = std::lower_bound(record.times.begin(), record.times.end(), timeMs);
        const std::size_t b = static_cast<std::size_t>(after - record.times.begin());
        const std::size_t a = b > 0 && record.times[b] != timeMs ? b - 1 : b;
        const std::uint32_t span = record.times[b] - record.times[a];
        const float t = span ? static_cast<float>(timeMs - record.times[a]) / static_cast<float>(span) : 0.0f;
        xyz.resize(stride);
        const float* pa = &record.positions[a * stride];
        const float* pb = &record.positions[b * stride];
        for (std::size_t n = 0; n < stride; ++n)
            xyz[n] = pa[n] + (pb[n] - pa[n]) * t;
        return true;
    }

    void Resample(const std::vector<float>& xyz, std::uint32_t count, std::vector<float>& out)
    {
        const std::size_t in = xyz.size() / 3;
        out.assign(static_cast<std::size_t>(count) * 3, 0.0f);
        if (in == 0 || count == 0)
            return;
        for (std::uint32_t v = 0; v < count; ++v)
        {
            const float at = count > 1 ? static_cast<float>(v) * static_cast<float>(in - 1) / static_cast<float>(count - 1) : 0.0f;
            const std::size_t i = (std::min)(static_cast<std::size_t>(at), in - 1);
            const std::size_t j = (std::min)(i + 1, in - 1);
            const float f = at - static_cast<float>(i);
            for (int c = 0; c < 3; ++c)
                out[v * 3 + c] = xyz[i * 3 + c] + (xyz[j * 3 + c] - xyz[i * 3 + c]) * f;
        }
    }

    void ResampleSmooth(const std::vector<float>& xyz, std::uint32_t count, std::vector<float>& out)
    {
        const std::size_t in = xyz.size() / 3;
        if (in < 3 || count < 2)
        {
            Resample(xyz, count, out);
            return;
        }
        out.assign(static_cast<std::size_t>(count) * 3, 0.0f);
        const auto at = [&](std::ptrdiff_t index, int c) {
            const std::ptrdiff_t clamped = index < 0 ? 0 : index >= static_cast<std::ptrdiff_t>(in) ? static_cast<std::ptrdiff_t>(in) - 1 : index;
            return xyz[static_cast<std::size_t>(clamped) * 3 + static_cast<std::size_t>(c)];
        };
        for (std::uint32_t v = 0; v < count; ++v)
        {
            const float pos = static_cast<float>(v) * static_cast<float>(in - 1) / static_cast<float>(count - 1);
            const std::ptrdiff_t i = (std::min)(static_cast<std::ptrdiff_t>(pos), static_cast<std::ptrdiff_t>(in) - 2);
            const float t = pos - static_cast<float>(i);
            const float t2 = t * t;
            const float t3 = t2 * t;
            for (int c = 0; c < 3; ++c)
            {
                const float p0 = at(i - 1, c);
                const float p1 = at(i, c);
                const float p2 = at(i + 1, c);
                const float p3 = at(i + 2, c);
                out[v * 3 + static_cast<std::size_t>(c)] = 0.5f *
                    (2.0f * p1 + (p2 - p0) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                        (3.0f * p1 - p0 - 3.0f * p2 + p3) * t3);
            }
        }
    }

    std::uint32_t KeepAnchoredRun(std::vector<float>& xyz, float maxStep)
    {
        const std::size_t count = xyz.size() / 3;
        if (count == 0)
            return 0;
        const auto dist2 = [&](std::size_t a, std::size_t b) {
            const float dx = xyz[a * 3] - xyz[b * 3];
            const float dy = xyz[a * 3 + 1] - xyz[b * 3 + 1];
            const float dz = xyz[a * 3 + 2] - xyz[b * 3 + 2];
            return dx * dx + dy * dy + dz * dz;
        };
        const float limit = maxStep * maxStep;
        std::size_t bestFirst = 0;
        std::size_t bestLast = 0;
        bool bestReversed = false;
        std::size_t bestSize = 0;
        std::size_t first = 1;
        while (first < count)
        {
            std::size_t last = first;
            while (last + 1 < count && dist2(last, last + 1) <= limit)
                ++last;
            const bool nearFirst = dist2(0, first) <= limit;
            const bool nearLast = dist2(0, last) <= limit;
            const std::size_t size = last - first + 1;
            if ((nearFirst || nearLast) && size > bestSize)
            {
                bestFirst = first;
                bestLast = last;
                bestReversed = !nearFirst;
                bestSize = size;
            }
            first = last + 1;
        }
        std::vector<float> kept(xyz.begin(), xyz.begin() + 3);
        for (std::size_t n = 0; n < bestSize; ++n)
        {
            const std::size_t v = bestReversed ? bestLast - n : bestFirst + n;
            kept.insert(kept.end(), xyz.begin() + static_cast<std::ptrdiff_t>(v * 3),
                xyz.begin() + static_cast<std::ptrdiff_t>(v * 3 + 3));
        }
        xyz.swap(kept);
        return static_cast<std::uint32_t>(xyz.size() / 3);
    }

    float Extent(const std::vector<float>& xyz)
    {
        float best = 0.0f;
        for (std::size_t v = 3; v + 2 < xyz.size(); v += 3)
        {
            const float dx = xyz[v] - xyz[0];
            const float dy = xyz[v + 1] - xyz[1];
            const float dz = xyz[v + 2] - xyz[2];
            best = (std::max)(best, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        return best;
    }

    float MaxPathLength(const RopeRecord& record, float maxStep)
    {
        const std::size_t stride = static_cast<std::size_t>(record.vertices) * 3;
        float best = 0.0f;
        std::vector<float> sample;
        for (std::size_t s = 0; stride && (s + 1) * stride <= record.positions.size(); ++s)
        {
            sample.assign(record.positions.begin() + static_cast<std::ptrdiff_t>(s * stride),
                record.positions.begin() + static_cast<std::ptrdiff_t>((s + 1) * stride));
            KeepAnchoredRun(sample, maxStep);
            const float* p = sample.data();
            float length = 0.0f;
            for (std::size_t v = 3; v < sample.size(); v += 3)
            {
                const float dx = p[v] - p[v - 3];
                const float dy = p[v + 1] - p[v - 2];
                const float dz = p[v + 2] - p[v - 1];
                length += std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            best = (std::max)(best, length);
        }
        return best;
    }
    std::uint32_t LastActivityMs(const RopeRecord& record)
    {
        std::uint32_t last = record.lastSeenMs;
        if (!record.times.empty())
            last = (std::max)(last, record.times.back());
        for (const auto& held : record.heldEvents)
            last = (std::max)(last, held.first);
        for (const RopeSlotEvent& slot : record.slotEvents)
            last = (std::max)(last, slot.timeMs);
        return last;
    }

    RopeTrimStats DropRopeRecordsBefore(std::vector<RopeRecord>& records, std::uint32_t keepFrom)
    {
        RopeTrimStats stats;
        const std::size_t before = records.size();
        records.erase(std::remove_if(records.begin(), records.end(),
                          [keepFrom](const RopeRecord& record) { return LastActivityMs(record) < keepFrom; }),
            records.end());
        stats.records = before - records.size();
        for (RopeRecord& record : records)
        {
            // Vertex samples: the one in force at keepFrom stays.
            std::size_t keep = 0;
            while (keep + 1 < record.times.size() && record.times[keep + 1] <= keepFrom)
                ++keep;
            const std::size_t stride = static_cast<std::size_t>(record.vertices) * 3u;
            if (keep && record.positions.size() == record.times.size() * stride &&
                record.windowCounts.size() == record.times.size())
            {
                record.times.erase(record.times.begin(), record.times.begin() + static_cast<std::ptrdiff_t>(keep));
                record.windowCounts.erase(record.windowCounts.begin(),
                    record.windowCounts.begin() + static_cast<std::ptrdiff_t>(keep));
                record.positions.erase(record.positions.begin(),
                    record.positions.begin() + static_cast<std::ptrdiff_t>(keep * stride));
                stats.samples += keep;
            }
            // Held changes: the one in force at keepFrom stays.
            std::size_t heldKeep = 0;
            while (heldKeep + 1 < record.heldEvents.size() && record.heldEvents[heldKeep + 1].first <= keepFrom)
                ++heldKeep;
            if (heldKeep)
            {
                record.heldEvents.erase(record.heldEvents.begin(),
                    record.heldEvents.begin() + static_cast<std::ptrdiff_t>(heldKeep));
                stats.events += heldKeep;
            }
            // Entry events: per entry, the last one at or before keepFrom
            // stays (the events are in time order).
            std::vector<std::uint32_t> lastAtBase;
            for (std::size_t index = 0; index < record.slotEvents.size(); ++index)
            {
                const RopeSlotEvent& event = record.slotEvents[index];
                if (event.timeMs > keepFrom)
                    continue;
                if (event.slot >= lastAtBase.size())
                    lastAtBase.resize(static_cast<std::size_t>(event.slot) + 1u, ~0u);
                lastAtBase[event.slot] = static_cast<std::uint32_t>(index);
            }
            std::size_t write = 0;
            for (std::size_t index = 0; index < record.slotEvents.size(); ++index)
            {
                const RopeSlotEvent& event = record.slotEvents[index];
                const bool drop = event.timeMs <= keepFrom && lastAtBase[event.slot] != static_cast<std::uint32_t>(index);
                if (drop)
                {
                    ++stats.events;
                    continue;
                }
                if (write != index)
                    record.slotEvents[write] = record.slotEvents[index];
                ++write;
            }
            record.slotEvents.resize(write);
        }
        return stats;
    }
}

// Which ropes take the records.
namespace replay::ropes
{
    float RopeFocusDistance(const float first[3], const float last[3], const float (*focus)[3], std::size_t focusCount)
    {
        float best = -1.0f;
        for (std::size_t f = 0; f < focusCount; ++f)
        {
            for (const float* end : {first, last})
            {
                const float dx = end[0] - focus[f][0];
                const float dy = end[1] - focus[f][1];
                const float dz = end[2] - focus[f][2];
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (best < 0.0f || d < best)
                    best = d;
            }
        }
        return best;
    }

    std::size_t LiveRopeRecords(const std::vector<RopeRecord>& records, std::uint32_t timeMs)
    {
        std::size_t live = 0;
        for (const RopeRecord& record : records)
        {
            const std::uint32_t age = timeMs >= record.lastSeenMs ? timeMs - record.lastSeenMs : 0;
            live += age < kRopeStaleMs ? 1u : 0u;
        }
        return live;
    }

    bool RopeRecordFits(const std::vector<RopeRecord>& records, std::uint32_t timeMs)
    {
        // Fewer records than the pool cannot fill it (no walk).
        return records.size() < kEngineRopePoolCapacity || LiveRopeRecords(records, timeMs) < kEngineRopePoolCapacity;
    }

    std::size_t RopeRecordsBytes(const std::vector<RopeRecord>& records)
    {
        std::size_t bytes = records.size() * sizeof(RopeRecord);
        for (const RopeRecord& record : records)
            bytes += record.attachments.size() * sizeof(RopeAttach) + record.slotEvents.size() * sizeof(RopeSlotEvent) +
                record.slotState.size() * sizeof(RopeSlotEvent) + record.heldEvents.size() * sizeof(record.heldEvents[0]) +
                record.times.size() * sizeof(std::uint32_t) + record.positions.size() * sizeof(float) +
                record.windowCounts.size() * sizeof(std::uint32_t);
        return bytes;
    }

    bool RopeStillSeen(bool holdsRecorded, bool nearPedRecord, bool inHand)
    {
        return holdsRecorded || (nearPedRecord && inHand);
    }

    bool AppendOrExtendVertices(RopeRecord& record, std::uint32_t timeMs, const std::vector<float>& window)
    {
        const std::size_t stride = static_cast<std::size_t>(record.vertices) * 3u;
        const std::size_t samples = record.times.size();
        if (samples < 2 || stride == 0 || record.positions.size() != samples * stride ||
            record.windowCounts.size() != samples || record.windowCounts.back() != window.size() / 3 ||
            record.windowCounts[samples - 2] != record.windowCounts.back())
        {
            AppendVertices(record, timeMs, window);
            return true;
        }
        static std::vector<float> resampled;
        ResampleSmooth(window, record.vertices, resampled);
        const float* last = &record.positions[(samples - 1) * stride];
        const float* before = &record.positions[(samples - 2) * stride];
        bool still = resampled.size() == stride;
        for (std::size_t n = 0; still && n < stride; ++n)
            still = std::fabs(resampled[n] - last[n]) <= kRopeStillM && std::fabs(last[n] - before[n]) <= kRopeStillM;
        if (!still)
        {
            record.times.push_back(timeMs);
            record.windowCounts.push_back(static_cast<std::uint32_t>(window.size() / 3));
            record.positions.insert(record.positions.end(), resampled.begin(), resampled.end());
            return true;
        }
        record.times.back() = timeMs;
        ++record.stillSamples;
        return false;
    }
}
