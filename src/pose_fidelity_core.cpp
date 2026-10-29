// The pure part of the pose fidelity judge (pose_fidelity.h). No Windows, no
// engine reads: the mod (pose_fidelity.cpp) and ReplayCoreTests
// (tests/pose_fidelity_tests.cpp) run this same code.

#include "pose_fidelity.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace replay::pose::fidelity
{
    namespace
    {
        // Finite on the bits (the exponent is not all ones): the ASI builds
        // with /fp:fast, which may fold std::isfinite or x == x away.
        bool FiniteBits(float value)
        {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            return (bits & 0x7F800000u) != 0x7F800000u;
        }

        // Object space is metres around the entity; a key bone farther than
        // this is garbage, and the squared distances stay finite.
        constexpr float kMaxObjectCoordinate = 10000.0f;

        bool UsableCoordinate(float value)
        {
            return FiniteBits(value) && value <= kMaxObjectCoordinate && value >= -kMaxObjectCoordinate;
        }

        float SquaredDistance(const BoneQts& a, const BoneQts& b)
        {
            const float dx = a.translation[0] - b.translation[0];
            const float dy = a.translation[1] - b.translation[1];
            const float dz = a.translation[2] - b.translation[2];
            return dx * dx + dy * dy + dz * dz;
        }

        // An error as logged: one that is not a finite number, or above
        // kNonFiniteError (garbage), counts as kNonFiniteError. Tested on the
        // bits first (/fp:fast), so the comparison only sees finite values.
        float SanitizedError(double value)
        {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            if ((bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull ||
                value > static_cast<double>(kNonFiniteError))
            {
                return kNonFiniteError;
            }
            return static_cast<float>(value);
        }

        void Accumulate(KindSummary& summary, const RecordSamples& record, const SampleError& error,
            std::uint32_t timeMs)
        {
            ++summary.samples;
            summary.bones += error.bones;
            for (std::uint32_t index = 0; index < error.bones; ++index)
            {
                summary.position.Add(error.positionMm[index]);
                summary.rotation.Add(error.rotationMrad[index]);
                if (error.positionMm[index] > summary.worstMm)
                {
                    summary.worstMm = error.positionMm[index];
                    summary.worstRecord = record.recordId;
                    summary.worstMs = timeMs;
                    summary.worstBone = static_cast<std::int32_t>(error.bone[index]);
                }
            }
            if (error.hasOrigin)
                summary.origin.Add(error.originMm);
        }

        void CountRecord(KindSummary& summary, const RecordResult& result)
        {
            ++summary.records;
            summary.outside += result.outside;
            summary.skipped += result.skipped;
            summary.dropped += result.dropped;
        }
    }

    const char* KindText(Kind kind)
    {
        switch (kind)
        {
        case Kind::Ped:
            return "ped";
        case Kind::Horse:
            return "horse";
        case Kind::Vehicle:
            return "vehicle";
        default:
            return "other";
        }
    }

    bool UsablePosition(const BoneQts& bone)
    {
        return UsableCoordinate(bone.translation[0]) && UsableCoordinate(bone.translation[1]) &&
            UsableCoordinate(bone.translation[2]);
    }

    std::vector<std::uint32_t> ChooseKeyBones(const BoneQts* objects, std::uint32_t boneCount, std::uint32_t count)
    {
        std::vector<std::uint32_t> chosen;
        if (!objects || boneCount == 0 || count == 0)
            return chosen;
        // Squared distance of every candidate to the nearest chosen bone; -1
        // for bones that are not candidates (unusable, or already chosen).
        std::vector<float> nearest(boneCount, -1.0f);
        std::uint32_t next = boneCount;
        for (std::uint32_t bone = 0; bone < boneCount; ++bone)
        {
            if (!UsablePosition(objects[bone]))
                continue;
            nearest[bone] = (std::numeric_limits<float>::max)();
            if (next == boneCount)
                next = bone; // bone 0, or the first usable one
        }
        while (next < boneCount && chosen.size() < count)
        {
            chosen.push_back(next);
            nearest[next] = -1.0f;
            const BoneQts& picked = objects[next];
            next = boneCount;
            float farthest = -1.0f;
            for (std::uint32_t bone = 0; bone < boneCount; ++bone)
            {
                if (nearest[bone] < 0.0f)
                    continue;
                const float distance = SquaredDistance(objects[bone], picked);
                if (distance < nearest[bone])
                    nearest[bone] = distance;
                if (nearest[bone] > farthest) // strict: a tie keeps the lowest index
                {
                    farthest = nearest[bone];
                    next = bone;
                }
            }
        }
        return chosen;
    }

    void FillTruth(TruthSample& truth, const std::uint32_t* keyBones, std::uint32_t keyCount,
        const BoneQts* objects)
    {
        keyCount = (std::min)(keyCount, kKeyBones);
        for (std::uint32_t key = 0; key < keyCount; ++key)
        {
            const BoneQts& bone = objects[keyBones[key]];
            truth.position[key] = {bone.translation[0], bone.translation[1], bone.translation[2]};
            truth.rotation[key] = bone.rotation;
        }
    }

    void FillTruthFromKeys(TruthSample& truth, const BoneQts* keyObjects, std::uint32_t keyCount)
    {
        keyCount = (std::min)(keyCount, kKeyBones);
        for (std::uint32_t key = 0; key < keyCount; ++key)
        {
            const BoneQts& bone = keyObjects[key];
            truth.position[key] = {bone.translation[0], bone.translation[1], bone.translation[2]};
            truth.rotation[key] = bone.rotation;
        }
    }

    bool TruthUsable(const TruthSample& truth, std::uint32_t keyCount)
    {
        keyCount = (std::min)(keyCount, kKeyBones);
        for (std::uint32_t key = 0; key < keyCount; ++key)
        {
            const Vec3& p = truth.position[key];
            const Quat& q = truth.rotation[key];
            if (!FiniteBits(p.x) || !FiniteBits(p.y) || !FiniteBits(p.z) || !FiniteBits(q.x) || !FiniteBits(q.y) ||
                !FiniteBits(q.z) || !FiniteBits(q.w))
            {
                return false;
            }
        }
        return true;
    }

    bool PlacementOf(const PoseSample& sample, WorldTransform& world)
    {
        // The same as replay_playback.cpp SampleWorldTransform (the puppet's
        // placement for this sample).
        WorldTransform a{};
        if (!DecodeWorldMatrix(sample.worldA, a))
            return false;
        WorldTransform b{};
        world = DecodeWorldMatrix(sample.worldB, b) ? InterpolateWorld(a, b, sample.t) : a;
        return true;
    }

    float PositionErrorMm(const Vec3& truth, const BoneQts& recorded)
    {
        const double dx = static_cast<double>(recorded.translation[0]) - static_cast<double>(truth.x);
        const double dy = static_cast<double>(recorded.translation[1]) - static_cast<double>(truth.y);
        const double dz = static_cast<double>(recorded.translation[2]) - static_cast<double>(truth.z);
        return SanitizedError(std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0);
    }

    float RotationErrorMrad(const Quat& truth, const BoneQts& recorded)
    {
        return SanitizedError(static_cast<double>(AngleBetween(truth, recorded.rotation)) * 1000.0);
    }

    bool Compare(const TruthSample& truth, const std::uint32_t* keyBones, std::uint32_t keyCount,
        const PoseSample& sample, SampleError& out)
    {
        out = SampleError{};
        keyCount = (std::min)(keyCount, kKeyBones);
        const std::size_t bones = sample.objects.size();
        for (std::uint32_t key = 0; key < keyCount; ++key)
        {
            const std::uint32_t bone = keyBones[key];
            if (bone >= bones)
                continue;
            const BoneQts& recorded = sample.objects[bone];
            out.bone[out.bones] = bone;
            out.positionMm[out.bones] = PositionErrorMm(truth.position[key], recorded);
            out.rotationMrad[out.bones] = RotationErrorMrad(truth.rotation[key], recorded);
            ++out.bones;
        }
        WorldTransform placed{};
        WorldTransform engine{};
        if (PlacementOf(sample, placed) && DecodeWorldMatrix(truth.world, engine))
        {
            const double dx = static_cast<double>(placed.position.x) - static_cast<double>(engine.position.x);
            const double dy = static_cast<double>(placed.position.y) - static_cast<double>(engine.position.y);
            const double dz = static_cast<double>(placed.position.z) - static_cast<double>(engine.position.z);
            out.originMm = SanitizedError(std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0);
            out.hasOrigin = true;
        }
        return out.bones > 0;
    }

    Distribution::Distribution() : m_bins(kBins + 1u, 0u) {}

    void Distribution::Reset()
    {
        std::fill(m_bins.begin(), m_bins.end(), 0u);
        m_count = 0;
        m_sum = 0.0;
        m_max = 0.0f;
        m_over10 = 0;
        m_over50 = 0;
        m_over200 = 0;
    }

    void Distribution::Add(float value)
    {
        if (!FiniteBits(value))
            value = kNonFiniteError;
        else if (value < 0.0f)
            value = 0.0f;
        ++m_count;
        m_sum += static_cast<double>(value);
        if (value > m_max)
            m_max = value;
        m_over10 += value > 10.0f ? 1u : 0u;
        m_over50 += value > 50.0f ? 1u : 0u;
        m_over200 += value > 200.0f ? 1u : 0u;
        std::uint32_t bin = kBins; // overflow
        if (value < kBinWidth * static_cast<float>(kBins))
            bin = (std::min)(static_cast<std::uint32_t>(value / kBinWidth), kBins - 1u);
        ++m_bins[bin];
    }

    float Distribution::Percentile(double p) const
    {
        const std::uint64_t rank = NearestRank(p, m_count);
        if (rank == 0)
            return 0.0f;
        std::uint64_t cumulative = 0;
        for (std::uint32_t bin = 0; bin < kBins; ++bin)
        {
            cumulative += m_bins[bin];
            if (cumulative >= rank)
                return (std::min)(static_cast<float>(bin + 1u) * kBinWidth, m_max);
        }
        return m_max; // the overflow bin
    }

    std::uint64_t NearestRank(double p, std::uint64_t count)
    {
        if (count == 0)
            return 0;
        if (!(p > 0.0))
            return 1;
        if (p >= 1.0)
            return count;
        // The tolerance keeps an exact product (0.95 * 20) from rounding up
        // a rank through its binary representation.
        const double scaled = std::ceil(p * static_cast<double>(count) - 1e-9);
        const std::uint64_t rank = scaled < 1.0 ? 1u : static_cast<std::uint64_t>(scaled);
        return (std::min)(rank, count);
    }

    std::uint32_t StaggerMs(std::uint32_t recordId, std::uint32_t intervalMs)
    {
        if (intervalMs == 0)
            return 0;
        std::uint32_t hash = recordId * 0x9E3779B1u;
        hash ^= hash >> 15;
        return hash % intervalMs;
    }

    std::uint32_t IntervalMs(std::uint32_t hz)
    {
        return 1000u / std::clamp(hz, kMinHz, kMaxHz);
    }

    void Store::Reset()
    {
        // Swapped with empty containers: the memory goes, not only the size.
        std::vector<std::unique_ptr<RecordSamples>>().swap(m_records);
        std::unordered_map<std::uint32_t, RecordSamples*>().swap(m_index);
        m_bytes = 0;
        m_nextTrimMs = 0;
    }

    RecordSamples& Store::Record(std::uint32_t recordId)
    {
        const auto found = m_index.find(recordId);
        if (found != m_index.end())
            return *found->second;
        m_records.push_back(std::make_unique<RecordSamples>());
        RecordSamples& record = *m_records.back();
        record.recordId = recordId;
        m_index.emplace(recordId, &record);
        return record;
    }

    const RecordSamples* Store::Find(std::uint32_t recordId) const
    {
        const auto found = m_index.find(recordId);
        return found == m_index.end() ? nullptr : found->second;
    }

    bool Store::Due(std::uint32_t recordId, std::uint32_t nowMs, std::uint32_t intervalMs)
    {
        const bool known = m_index.find(recordId) != m_index.end();
        RecordSamples& record = Record(recordId);
        if (!known)
            record.nextDueMs = nowMs + StaggerMs(recordId, intervalMs);
        record.lastActiveMs = nowMs;
        if (nowMs < record.nextDueMs)
            return false;
        // One interval on keeps the stagger; a record that fell behind (a
        // long tick, the pause) starts again from now.
        record.nextDueMs += intervalMs;
        if (record.nextDueMs <= nowMs)
            record.nextDueMs = nowMs + intervalMs;
        return true;
    }

    bool Store::HasRoom(const RecordSamples& record) const
    {
        const std::vector<TruthSample>& samples = record.samples;
        if (samples.size() < samples.capacity() || (record.head > 0 && record.head * 2u >= samples.size()))
            return true; // reserved room, or the front Trim dropped (reused)
        return m_capBytes > m_bytes && (m_capBytes - m_bytes) / sizeof(TruthSample) > 0;
    }

    bool Store::Append(RecordSamples& record, const TruthSample& sample)
    {
        std::vector<TruthSample>& samples = record.samples;
        // Buffer mode: the reads Trim dropped make room before the vector grows.
        if (record.head > 0 && samples.size() == samples.capacity() && record.head * 2u >= samples.size())
        {
            samples.erase(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(record.head));
            record.head = 0;
        }
        if (samples.size() == samples.capacity())
        {
            // The cap counts what is reserved, so the growth is decided here.
            const std::size_t have = samples.capacity();
            std::size_t want = have < 16u ? 16u : have + have / 2u;
            const std::size_t room = m_capBytes > m_bytes ? (m_capBytes - m_bytes) / sizeof(TruthSample) : 0u;
            if (want - have > room)
                want = have + room;
            if (want == have)
            {
                ++record.dropped;
                return false;
            }
            samples.reserve(want);
            m_bytes += (samples.capacity() - have) * sizeof(TruthSample);
        }
        samples.push_back(sample);
        return true;
    }

    void Store::Trim(std::uint32_t nowMs)
    {
        if (!m_buffer || nowMs <= kBufferKeepMs)
            return;
        const std::uint32_t oldest = nowMs - kBufferKeepMs;
        bool forget = false;
        for (const std::unique_ptr<RecordSamples>& record : m_records)
        {
            while (record->head < record->samples.size() && record->samples[record->head].timeMs < oldest)
                ++record->head;
            if (record->head > 0 && record->head == record->samples.size())
            {
                record->samples.clear(); // the capacity stays reserved (counted)
                record->head = 0;
            }
            forget = forget || (record->Held() == 0 && record->lastActiveMs < oldest);
        }
        if (!forget)
            return;
        // Records nobody asked for within the window (the entity left):
        // released, the others keep their order.
        std::vector<std::unique_ptr<RecordSamples>> kept;
        kept.reserve(m_records.size());
        for (std::unique_ptr<RecordSamples>& record : m_records)
        {
            if (record->Held() == 0 && record->lastActiveMs < oldest)
            {
                m_bytes -= record->samples.capacity() * sizeof(TruthSample);
                m_index.erase(record->recordId);
                continue;
            }
            kept.push_back(std::move(record));
        }
        m_records.swap(kept);
    }

    void Store::TrimOnClock(std::uint32_t nowMs)
    {
        if (!m_buffer || nowMs < m_nextTrimMs)
            return;
        Trim(nowMs);
        m_nextTrimMs = nowMs + kBufferTrimEveryMs;
    }

    void EvaluateStore(const Store& store, TrackSource& source, Report& report)
    {
        Distribution recordPosition;
        Distribution recordRotation;
        PoseSample sample;
        std::vector<std::size_t> order;
        for (const std::unique_ptr<RecordSamples>& pointer : store.Records())
        {
            const RecordSamples& record = *pointer;
            if (record.Held() == 0 && record.skipped == 0 && record.dropped == 0)
                continue; // scheduled, never read
            RecordResult result;
            result.recordId = record.recordId;
            result.kind = record.kind;
            result.skipped = record.skipped;
            result.dropped = record.dropped;
            KindSummary& kind = report.kinds[static_cast<std::size_t>(record.kind) % kKindCount];

            // In time order, so each track is sampled forward (its cursor
            // moves on instead of decoding again).
            order.clear();
            for (std::size_t index = record.head; index < record.samples.size(); ++index)
                order.push_back(index);
            std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                return record.samples[a].timeMs < record.samples[b].timeMs;
            });
            std::uint32_t firstMs = 0;
            std::uint32_t lastMs = 0;
            const bool hasTrack = !order.empty() && source.Span(record.recordId, firstMs, lastMs);
            const std::uint32_t keyCount = static_cast<std::uint32_t>(record.keyBones.size());
            recordPosition.Reset();
            recordRotation.Reset();
            float worstMm = -1.0f;
            for (const std::size_t index : order)
            {
                const TruthSample& truth = record.samples[index];
                SampleError error;
                if (!hasTrack || truth.timeMs < firstMs || truth.timeMs > lastMs ||
                    !source.Sample(record.recordId, truth.timeMs, sample) ||
                    !Compare(truth, record.keyBones.data(), keyCount, sample, error))
                {
                    ++result.outside;
                    continue;
                }
                ++result.samples;
                result.bones += error.bones;
                for (std::uint32_t bone = 0; bone < error.bones; ++bone)
                {
                    recordPosition.Add(error.positionMm[bone]);
                    recordRotation.Add(error.rotationMrad[bone]);
                    if (error.positionMm[bone] > worstMm)
                    {
                        worstMm = error.positionMm[bone];
                        result.worstMs = truth.timeMs;
                        result.worstBone = static_cast<std::int32_t>(error.bone[bone]);
                    }
                }
                if (error.hasOrigin && error.originMm > result.originMaxMm)
                    result.originMaxMm = error.originMm;
                Accumulate(kind, record, error, truth.timeMs);
                Accumulate(report.all, record, error, truth.timeMs);
            }
            result.posMeanMm = recordPosition.Mean();
            result.posP95Mm = recordPosition.Percentile(0.95);
            result.posMaxMm = recordPosition.Max();
            result.rotP95Mrad = recordRotation.Percentile(0.95);
            CountRecord(kind, result);
            CountRecord(report.all, result);
            report.records.push_back(result);
        }
    }

    // pose_fidelity.h ExportedTracks.
    ExportedTracks::ExportedTracks(std::vector<PoseRecordExport> records) : m_records(std::move(records))
    {
        m_index.reserve(m_records.size());
        for (std::size_t index = 0; index < m_records.size(); ++index)
            m_index.emplace(m_records[index].recordId, index); // the first of an id stays
    }

    PoseTrack* ExportedTracks::TrackOf(std::uint32_t recordId)
    {
        if (m_hasCurrent && m_currentId == recordId)
            return m_current.get();
        m_current.reset(); // the previous record's track and caches go
        m_hasCurrent = true;
        m_currentId = recordId;
        const auto found = m_index.find(recordId);
        if (found == m_index.end())
            return nullptr;
        auto track = std::make_unique<PoseTrack>();
        std::string error;
        if (!track->Import(m_records[found->second].track, error))
        {
            ++m_importFailures;
            m_lastImportError = error;
            return nullptr;
        }
        m_current = std::move(track);
        return m_current.get();
    }

    // As capture::TrackSpan and capture::SampleTrack on the record.
    bool ExportedTracks::Span(std::uint32_t recordId, std::uint32_t& firstMs, std::uint32_t& lastMs)
    {
        const PoseTrack* track = TrackOf(recordId);
        if (!track || track->Stats().frames == 0)
            return false;
        firstMs = track->Stats().firstTimeMs;
        lastMs = track->Stats().lastTimeMs;
        return true;
    }

    bool ExportedTracks::Sample(std::uint32_t recordId, std::uint32_t timeMs, PoseSample& sample)
    {
        PoseTrack* track = TrackOf(recordId);
        return track && track->Sample(timeMs, sample);
    }

    std::string ReportText(const Report& report, std::uint32_t hz, double evalMs, bool perRecord)
    {
        std::string text;
        if (perRecord)
        {
            for (const RecordResult& record : report.records)
                text.append(RecordLine(record)).push_back('\n');
        }
        text.append(SummaryLine("all", report.all, hz, evalMs)).push_back('\n');
        for (std::size_t kind = 0; kind < kKindCount; ++kind)
        {
            if (report.kinds[kind].records != 0)
                text.append(SummaryLine(KindText(static_cast<Kind>(kind)), report.kinds[kind], hz, evalMs)).push_back('\n');
        }
        return text;
    }

    std::string RecordLine(const RecordResult& record)
    {
        char text[384]{};
        std::snprintf(text, sizeof(text),
            "POSE_FIDELITY,record=0x%x,kind=%s,samples=%llu,pos_mean_mm=%.2f,pos_p95_mm=%.2f,pos_max_mm=%.2f,"
            "rot_p95_mrad=%.2f,origin_max_mm=%.2f,worst_ms=%u,worst_bone=%d",
            record.recordId, KindText(record.kind), static_cast<unsigned long long>(record.samples),
            record.posMeanMm, static_cast<double>(record.posP95Mm), static_cast<double>(record.posMaxMm),
            static_cast<double>(record.rotP95Mrad), static_cast<double>(record.originMaxMm), record.worstMs,
            record.worstBone);
        return text;
    }

    std::string SummaryLine(const char* kind, const KindSummary& summary, std::uint32_t hz, double evalMs)
    {
        char text[1024]{};
        std::snprintf(text, sizeof(text),
            "POSE_FIDELITY_SUMMARY,kind=%s,records=%llu,samples=%llu,bones=%llu,outside=%llu,skipped=%llu,"
            "dropped=%llu,pos_mean_mm=%.2f,pos_p50_mm=%.2f,pos_p95_mm=%.2f,pos_p99_mm=%.2f,pos_max_mm=%.2f,"
            "pos_over_10mm=%llu,pos_over_50mm=%llu,pos_over_200mm=%llu,rot_p95_mrad=%.2f,rot_max_mrad=%.2f,"
            "origin_mean_mm=%.2f,origin_p95_mm=%.2f,origin_max_mm=%.2f,worst_record=0x%x,worst_ms=%u,"
            "worst_bone=%d,hz=%u,eval_ms=%.1f",
            kind, static_cast<unsigned long long>(summary.records), static_cast<unsigned long long>(summary.samples),
            static_cast<unsigned long long>(summary.bones), static_cast<unsigned long long>(summary.outside),
            static_cast<unsigned long long>(summary.skipped), static_cast<unsigned long long>(summary.dropped),
            summary.position.Mean(), static_cast<double>(summary.position.Percentile(0.50)),
            static_cast<double>(summary.position.Percentile(0.95)),
            static_cast<double>(summary.position.Percentile(0.99)), static_cast<double>(summary.position.Max()),
            static_cast<unsigned long long>(summary.position.Over10()),
            static_cast<unsigned long long>(summary.position.Over50()),
            static_cast<unsigned long long>(summary.position.Over200()),
            static_cast<double>(summary.rotation.Percentile(0.95)), static_cast<double>(summary.rotation.Max()),
            summary.origin.Mean(), static_cast<double>(summary.origin.Percentile(0.95)),
            static_cast<double>(summary.origin.Max()), summary.worstRecord, summary.worstMs, summary.worstBone, hz,
            evalMs);
        return text;
    }
}
