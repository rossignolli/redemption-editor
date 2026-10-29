#include "pose_jerk.h"

#include "playback_time.h"

#include <algorithm>
#include <cmath>

namespace replay::jerk
{
    namespace
    {
        float Length(const pose::Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
        pose::Vec3 Sub(const pose::Vec3& a, const pose::Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
        float Distance(const pose::Vec3& a, const pose::Vec3& b) { return Length(Sub(a, b)); }

        bool InBox(const pose::Vec3& p, const SaddleBox& box)
        {
            return std::fabs(p.x) <= box.halfWidth && p.y >= box.backY && p.y <= box.frontY && p.z >= box.lowZ &&
                p.z <= box.highZ;
        }
    }

    pose::Vec3 InMountSpace(const pose::WorldTransform& mount, const pose::Vec3& riderOrigin)
    {
        return pose::Rotate(pose::Conjugate(mount.rotation), Sub(riderOrigin, mount.position));
    }

    bool InSaddle(const pose::WorldTransform& mount, const pose::Vec3& riderOrigin, const SaddleBox& box)
    {
        return InBox(InMountSpace(mount, riderOrigin), box);
    }

    void MountPairs::Update(const std::vector<Body>& bodies)
    {
        std::unordered_map<std::uint32_t, std::uint32_t> next;
        for (const Body& rider : bodies)
        {
            if (rider.horse)
                continue;
            const auto kept = m_mountOf.find(rider.packed);
            std::uint32_t chosen = 0;
            if (kept != m_mountOf.end())
            {
                for (const Body& mount : bodies)
                {
                    if (mount.horse && mount.packed == kept->second &&
                        InSaddle(mount.world, rider.world.position, kSaddleLeave))
                    {
                        chosen = mount.packed;
                        break;
                    }
                }
            }
            if (!chosen)
            {
                float best = 1e9f;
                for (const Body& mount : bodies)
                {
                    if (!mount.horse || !InSaddle(mount.world, rider.world.position, kSaddleEnter))
                        continue;
                    const float d = Distance(mount.world.position, rider.world.position);
                    if (d < best)
                    {
                        best = d;
                        chosen = mount.packed;
                    }
                }
            }
            if (chosen)
            {
                next[rider.packed] = chosen;
                if (kept == m_mountOf.end() || kept->second != chosen)
                    ++m_enters;
            }
        }
        for (const auto& [rider, mount] : m_mountOf)
        {
            const auto now = next.find(rider);
            if (now == next.end() || now->second != mount)
                ++m_leaves;
        }
        m_mountOf.swap(next);
        const std::unordered_map<std::uint32_t, std::uint32_t>& previousMountOf = next; // the swap left the old map here

        // The seats. Per mount, its riders ordered front to back
        // (+Y in the mount's space; the packed id breaks a tie): the first
        // sits in the saddle (-1), the others behind take 0, 1, ...
        m_previousSeatOf.swap(m_seatOf); // kept for the leave line and the seat changes
        m_seatOf.clear();
        m_seatChanges.clear();
        struct Seated
        {
            std::uint32_t mount;
            float forward;
            std::uint32_t rider;
        };
        std::vector<Seated> seated;
        seated.reserve(m_mountOf.size());
        for (const Body& rider : bodies)
        {
            const auto paired = m_mountOf.find(rider.packed);
            if (rider.horse || paired == m_mountOf.end())
                continue;
            for (const Body& mount : bodies)
            {
                if (mount.horse && mount.packed == paired->second)
                {
                    seated.push_back({mount.packed, InMountSpace(mount.world, rider.world.position).y, rider.packed});
                    break;
                }
            }
        }
        std::sort(seated.begin(), seated.end(), [](const Seated& a, const Seated& b) {
            if (a.mount != b.mount)
                return a.mount < b.mount;
            if (a.forward != b.forward)
                return a.forward > b.forward;
            return a.rider < b.rider;
        });
        bool shared = false;
        for (std::size_t n = 0; n < seated.size();)
        {
            std::size_t end = n;
            while (end < seated.size() && seated[end].mount == seated[n].mount)
                ++end;
            for (std::size_t k = n; k < end; ++k)
                m_seatOf[seated[k].rider] = kSaddleSeat + static_cast<int>(k - n);
            const auto riders = static_cast<std::uint32_t>(end - n);
            m_maxRiders = std::max(m_maxRiders, riders);
            shared = shared || riders >= 2;
            n = end;
        }
        m_sharedUpdates += shared ? 1 : 0;
        // A rider on the same mount as before whose seat
        // moved (another rider got on in front of him or left it). An enter
        // or a leave is logged by the caller from the map itself.
        for (const auto& [rider, seat] : m_seatOf)
        {
            const auto was = m_previousSeatOf.find(rider);
            const auto wasMount = previousMountOf.find(rider);
            const auto isMount = m_mountOf.find(rider);
            if (was != m_previousSeatOf.end() && was->second != seat && wasMount != previousMountOf.end() &&
                isMount != m_mountOf.end() && wasMount->second == isMount->second)
                m_seatChanges.push_back(rider);
        }
        std::sort(m_seatChanges.begin(), m_seatChanges.end());
    }

    std::uint32_t MountPairs::MountOf(std::uint32_t rider) const
    {
        const auto found = m_mountOf.find(rider);
        return found == m_mountOf.end() ? 0 : found->second;
    }

    std::uint32_t MountPairs::RidersOf(std::uint32_t mount) const
    {
        std::uint32_t riders = 0;
        if (!mount)
            return 0;
        for (const auto& [rider, ridden] : m_mountOf)
            riders += ridden == mount ? 1 : 0;
        return riders;
    }

    int MountPairs::SeatOf(std::uint32_t rider) const
    {
        const auto found = m_seatOf.find(rider);
        return found == m_seatOf.end() ? kSaddleSeat : found->second;
    }

    int MountPairs::PreviousSeatOf(std::uint32_t rider) const
    {
        const auto found = m_previousSeatOf.find(rider);
        return found == m_previousSeatOf.end() ? kSaddleSeat : found->second;
    }

    std::string CauseText(std::uint32_t causes)
    {
        static const char* const kNames[kCauseBits] = {"slow_tick", "capped", "held", "physics", "not_written",
            "segment", "created", "mounted", "frozen", "attached", "prerender"};
        std::string text;
        for (int bit = 0; bit < kCauseBits; ++bit)
        {
            if (!(causes & (1u << bit)))
                continue;
            if (!text.empty())
                text += '+';
            text += kNames[bit];
        }
        return text.empty() ? "none" : text;
    }

    const char* KindText(Kind kind)
    {
        switch (kind)
        {
        case Kind::EngineMoved: return "engine_moved";
        case Kind::MountOffset: return "mount_offset";
        case Kind::TimeStep: return "time_step";
        case Kind::Stall: return "stall";
        case Kind::PoseStale: return "pose_stale";
        default: return "?";
        }
    }

    void Detector::Reset()
    {
        m_states.clear();
        m_puppets.clear();
        m_summary = {};
        m_tick = {};
        m_tickSerial = 0;
        m_hasLast = false;
        m_lastPlayheadMs = 0;
        m_lastWallMs = 0.0;
    }

    TickInfo Detector::BeginTick(std::uint32_t playheadMs, double wallMs, double rate, bool normalPlay)
    {
        ++m_tickSerial;
        m_tick = {};
        const bool follows = normalPlay && m_hasLast && playheadMs >= m_lastPlayheadMs && wallMs >= m_lastWallMs;
        if (follows)
        {
            m_tick.valid = true;
            m_tick.wallStepMs = static_cast<float>(wallMs - m_lastWallMs);
            m_tick.playStepMs = static_cast<float>(playheadMs - m_lastPlayheadMs);
            m_tick.expectedStepMs = static_cast<float>((wallMs - m_lastWallMs) * rate);
            const float lost = m_tick.expectedStepMs - m_tick.playStepMs;
            m_tick.lostMs = std::fabs(lost) > m_limits.timeSlackMs ? lost : 0.0f;
            if (m_tick.wallStepMs >= m_limits.stallMs)
                m_tick.causes |= kCauseSlowTick;
            if (m_tick.expectedStepMs > static_cast<float>(transport::kMaxNormalStepMs) + m_limits.timeSlackMs)
                m_tick.causes |= kCauseCapped;
            if (m_tick.playStepMs == 0.0f && m_tick.wallStepMs > 0.0f)
                m_tick.causes |= kCauseHeld;
            ++m_summary.ticks;
            m_summary.slowTicks += (m_tick.causes & kCauseSlowTick) ? 1 : 0;
            m_summary.cappedTicks += (m_tick.causes & kCauseCapped) ? 1 : 0;
            m_summary.heldTicks += (m_tick.causes & kCauseHeld) ? 1 : 0;
        }
        if (!normalPlay || (m_hasLast && playheadMs < m_lastPlayheadMs))
        {
            // Paused, scrubbing or sent back: nothing carries over.
            for (auto& [packed, state] : m_states)
                state.has = false;
        }
        m_hasLast = normalPlay;
        m_lastPlayheadMs = playheadMs;
        m_lastWallMs = wallMs;
        return m_tick;
    }

    void Detector::Record(const Finding& finding, std::vector<Finding>& out)
    {
        out.push_back(finding);
        ++m_summary.jerks;
        ++m_summary.byKind[static_cast<int>(finding.kind)];
        for (int bit = 0; bit < kCauseBits; ++bit)
            if (finding.causes & (1u << bit))
                ++m_summary.byCause[bit];
        if (finding.magnitudeM > m_summary.worstM)
        {
            m_summary.worstM = finding.magnitudeM;
            m_summary.worstPacked = finding.packed;
            m_summary.worstMs = finding.playheadMs;
            m_summary.worstKind = finding.kind;
        }
        PuppetSummary& puppet = m_puppets[finding.packed];
        puppet.packed = finding.packed;
        ++puppet.jerks;
        ++puppet.byKind[static_cast<int>(finding.kind)];
        if (finding.magnitudeM > puppet.worstM)
        {
            puppet.worstM = finding.magnitudeM;
            puppet.worstMs = finding.playheadMs;
            puppet.worstKind = finding.kind;
        }
    }

    void Detector::Observe(const Observation& o, std::vector<Finding>& out)
    {
        State& state = m_states[o.packed];
        state.hasDisplacement = false;
        const bool consecutive = m_tick.valid && state.has && state.lastTick + 1 == m_tickSerial;
        std::uint32_t causes = o.causes | m_tick.causes | (o.mount ? kCauseMounted : 0u);
        if (consecutive)
        {
            ++m_summary.observations;
            // Recorded speed over the tick (kept through a held tick).
            if (o.playheadMs > state.playheadMs)
                state.speedMps = Distance(o.recorded, state.recorded) * 1000.0f /
                    static_cast<float>(o.playheadMs - state.playheadMs);
            // The skeleton pose: nothing written since the last tick means
            // the frame in between rendered the pose of two ticks ago.
            const bool stale = o.hasOverride && state.hasOverride && o.overrideWrites == state.overrideWrites;
            if (stale)
                causes |= kCauseNotWritten;
            if (stale && state.roots >= 2 && o.hasRoot)
            {
                const float missed = Distance(state.rootPrev, state.rootPrev2);
                if (missed >= m_limits.poseStaleM)
                {
                    Finding f;
                    f.packed = o.packed;
                    f.kind = Kind::PoseStale;
                    f.magnitudeM = missed;
                    f.speedMps = state.speedMps;
                    f.mount = o.mount;
                    f.causes = causes;
                    f.playheadMs = o.playheadMs;
                    Record(f, out);
                }
            }
            // Where the engine left the clone since our last placement.
            if (o.placed && o.hasEngine)
            {
                state.displacement = Sub(o.engine, o.placedAt);
                state.hasDisplacement = true;
                const float moved = Length(state.displacement);
                // A rider the pre-render carried with his mount moved by the
                // mount's push, which the mount's own finding reports. Put back
                // by the end-of-frame placement before the frame is drawn:
                // counted apart, not a jerk anyone saw.
                if (moved >= m_limits.engineMovedM && !(causes & kCausePreRender) && o.frameEndRefuseM > 0.0f &&
                    moved <= o.frameEndRefuseM)
                    ++m_summary.placedBack;
                else if (moved >= m_limits.engineMovedM && !(causes & kCausePreRender))
                {
                    Finding f;
                    f.packed = o.packed;
                    f.kind = Kind::EngineMoved;
                    f.magnitudeM = moved;
                    f.engineM = moved;
                    f.speedMps = state.speedMps;
                    f.mount = o.mount;
                    f.causes = causes;
                    f.playheadMs = o.playheadMs;
                    Record(f, out);
                }
            }
        }
        state.has = true;
        state.lastTick = m_tickSerial;
        state.recorded = o.recorded;
        state.playheadMs = o.playheadMs;
        state.overrideWrites = o.overrideWrites;
        state.hasOverride = o.hasOverride;
        state.mount = o.mount;
        state.causes = causes;
        state.frameEndRefuseM = o.frameEndRefuseM;
        if (o.hasRoot)
        {
            state.rootPrev2 = state.rootPrev;
            state.rootPrev = o.rootObject;
            state.roots = consecutive ? std::min(state.roots + 1, 2) : 1;
        }
        else
            state.roots = 0;
    }

    void Detector::RecordPair(const PairGroupInput& worst, std::uint32_t riders, std::vector<Finding>& out)
    {
        const State& rider = m_states.at(worst.rider); // both are in this tick's pair list
        const State& mount = m_states.at(worst.mount);
        Finding f;
        f.packed = worst.rider;
        f.kind = Kind::MountOffset;
        f.magnitudeM = worst.relativeM;
        f.engineM = worst.riderMovedM;
        f.mountM = worst.relativeM;
        f.speedMps = rider.speedMps;
        f.mount = worst.mount;
        f.causes = rider.causes | mount.causes;
        f.playheadMs = rider.playheadMs;
        f.riders = riders;
        m_summary.mountOffsetRiders += riders;
        Record(f, out);
    }

    void Detector::EndTick(std::vector<Finding>& out)
    {
        if (!m_tick.valid)
            return;
        // The pair: the rider against its mount, both from this tick.
        // Every rider of a mount is a pair of its own (a passenger behind the
        // saddle too); the offsets are grouped below so that a mount's move is
        // one finding, not one per rider.
        const State* fastest = nullptr;
        std::uint32_t fastestPacked = 0;
        m_pairScratch.clear();
        for (const auto& [packed, state] : m_states)
        {
            if (!state.has || state.lastTick != m_tickSerial)
                continue;
            // A tie (two riders of one horse) goes to the lower
            // packed id, not to the map's order.
            if (!fastest || state.speedMps > fastest->speedMps ||
                (state.speedMps == fastest->speedMps && packed < fastestPacked))
            {
                fastest = &state;
                fastestPacked = packed;
            }
            if (!state.mount || !state.hasDisplacement)
                continue;
            const auto mount = m_states.find(state.mount);
            if (mount == m_states.end() || mount->second.lastTick != m_tickSerial || !mount->second.hasDisplacement)
                continue;
            const float relative = Distance(state.displacement, mount->second.displacement);
            if (relative < m_limits.mountOffsetM)
                continue;
            PairGroupInput offset;
            offset.rider = packed;
            offset.mount = state.mount;
            offset.relativeM = relative;
            offset.riderMovedM = Length(state.displacement);
            // As with engine_moved, an offset whose two sides the end-of-frame
            // placement puts back before the frame is drawn was never on
            // screen.
            const float mountMoved = Length(mount->second.displacement);
            offset.placedBack = state.frameEndRefuseM > 0.0f && mount->second.frameEndRefuseM > 0.0f &&
                offset.riderMovedM <= state.frameEndRefuseM && mountMoved <= mount->second.frameEndRefuseM;
            m_pairScratch.push_back(offset);
        }
        GroupPairOffsets(
            m_pairScratch, m_limits.mountOffsetM,
            [&](std::size_t worst, std::uint32_t riders) { RecordPair(m_pairScratch[worst], riders, out); },
            [&]() { ++m_summary.pairPlacedBack; });
        if (!fastest)
            return;
        // The playhead against the frame: one finding per tick, on the
        // fastest puppet (every puppet had the same step).
        Finding f;
        f.packed = fastestPacked;
        f.speedMps = fastest->speedMps;
        f.mount = fastest->mount;
        f.causes = m_tick.causes | (fastest->causes & (kCausePhysics | kCauseMounted));
        f.playheadMs = fastest->playheadMs;
        const float lostM = fastest->speedMps * std::fabs(m_tick.lostMs) / 1000.0f;
        if (m_tick.lostMs != 0.0f && lostM >= m_limits.timeStepM)
        {
            f.kind = Kind::TimeStep;
            f.magnitudeM = lostM;
            Record(f, out);
        }
        else if ((m_tick.causes & kCauseSlowTick) != 0)
        {
            const float stallM = fastest->speedMps * m_tick.wallStepMs / 1000.0f;
            if (stallM >= m_limits.timeStepM)
            {
                f.kind = Kind::Stall;
                f.magnitudeM = stallM;
                Record(f, out);
            }
        }
    }

    std::vector<PuppetSummary> Detector::Worst(std::size_t count) const
    {
        std::vector<PuppetSummary> list;
        list.reserve(m_puppets.size());
        for (const auto& [packed, puppet] : m_puppets)
            list.push_back(puppet);
        std::sort(list.begin(), list.end(), [](const PuppetSummary& a, const PuppetSummary& b) {
            return a.jerks != b.jerks ? a.jerks > b.jerks : a.worstM > b.worstM;
        });
        if (list.size() > count)
            list.resize(count);
        return list;
    }
}
