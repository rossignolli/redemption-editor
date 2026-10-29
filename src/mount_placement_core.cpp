// The pure part of the rider follow at the ped pre-render (mount_placement.h).
// No Windows, no engine reads: the mod (mount_placement.cpp) and
// ReplayCoreTests (tests/mount_placement_tests.cpp) run this same code.

#include "mount_placement.h"

#include "entity_lifecycle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace replay::mount
{
    namespace
    {
        // Finite on the bits: the ASI builds with /fp:fast, which may fold
        // std::isfinite or x == x away (as pose_fidelity_core.cpp).
        bool FiniteBits(float value)
        {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            return (bits & 0x7F800000u) != 0x7F800000u;
        }

        bool FiniteDouble(double value)
        {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            return (bits & 0x7FF0000000000000ull) != 0x7FF0000000000000ull;
        }

        bool UnitRow(const pose::Vec3& row)
        {
            const float lengthSquared = row.x * row.x + row.y * row.y + row.z * row.z;
            return std::fabs(lengthSquared - 1.0f) < 0.02f; // the length within about 0.01 of 1
        }

        // R * v, R with the columns right, forward, up (the frame's basis).
        pose::Vec3 Apply(const pose::Vec3& right, const pose::Vec3& forward, const pose::Vec3& up, const pose::Vec3& v)
        {
            return {
                v.x * right.x + v.y * forward.x + v.z * up.x,
                v.x * right.y + v.y * forward.y + v.z * up.y,
                v.x * right.z + v.y * forward.z + v.z * up.z,
            };
        }

        double Length3(double x, double y, double z) { return std::sqrt(x * x + y * y + z * z); }

        void Unpack(float lane, std::int16_t& cellX, std::int16_t& cellY)
        {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &lane, sizeof(bits));
            cellX = static_cast<std::int16_t>(bits & 0xFFFFu);
            cellY = static_cast<std::int16_t>(bits >> 16);
        }

        const char* PhaseText(const FrameRecord& record, const FollowThresholds& limits)
        {
            if (record.readFail)
                return "read_fail";
            if (record.writeFail)
                return "write_fail";
            if (record.plan.refused)
                return "refused";
            if (record.wrote && record.plan.correctionM >= limits.movedM)
                return "reposition";
            return "moved";
        }
    }

    bool DecodeEngineMatrix(const float matrix[16], EngineFrame& frame)
    {
        for (int index = 0; index < 15; ++index)
        {
            if ((index & 3) == 3)
                continue; // the rows' w lanes are not part of the transform
            if (!FiniteBits(matrix[index]))
                return false;
        }
        frame.right = {matrix[0], matrix[1], matrix[2]};
        frame.forward = {matrix[4], matrix[5], matrix[6]};
        frame.up = {matrix[8], matrix[9], matrix[10]};
        if (!UnitRow(frame.right) || !UnitRow(frame.forward) || !UnitRow(frame.up))
            return false;
        Unpack(matrix[15], frame.cellX, frame.cellY);
        frame.local = {matrix[12], matrix[13], matrix[14]};
        return true;
    }

    void AbsolutePosition(const EngineFrame& frame, double out[3])
    {
        out[0] = static_cast<double>(frame.cellX) * static_cast<double>(pose::kWorldCellSize) + frame.local.x;
        out[1] = static_cast<double>(frame.cellY) * static_cast<double>(pose::kWorldCellSize) + frame.local.y;
        out[2] = frame.local.z;
    }

    pose::Vec3 LocalIn(const EngineFrame& frame, std::int16_t cellX, std::int16_t cellY)
    {
        const float dx = static_cast<float>(static_cast<int>(frame.cellX) - cellX) * pose::kWorldCellSize;
        const float dy = static_cast<float>(static_cast<int>(frame.cellY) - cellY) * pose::kWorldCellSize;
        return {frame.local.x + dx, frame.local.y + dy, frame.local.z};
    }

    void EncodeKeepingCells(const pose::Vec3& right, const pose::Vec3& forward, const pose::Vec3& up,
        const pose::Vec3& position, std::int16_t cellX, std::int16_t cellY, float matrix[16])
    {
        std::int16_t ownX = 0;
        std::int16_t ownY = 0;
        Unpack(matrix[15], ownX, ownY);
        const float dx = static_cast<float>(static_cast<int>(cellX) - ownX) * pose::kWorldCellSize;
        const float dy = static_cast<float>(static_cast<int>(cellY) - ownY) * pose::kWorldCellSize;
        matrix[0] = right.x;
        matrix[1] = right.y;
        matrix[2] = right.z;
        matrix[4] = forward.x;
        matrix[5] = forward.y;
        matrix[6] = forward.z;
        matrix[8] = up.x;
        matrix[9] = up.y;
        matrix[10] = up.z;
        matrix[12] = position.x + dx;
        matrix[13] = position.y + dy;
        matrix[14] = position.z;
        // matrix[3], [7], [11] and the cells in [15] stay as they were.
    }

    pose::Quat FrameRotation(const EngineFrame& frame)
    {
        return pose::QuatFromBasis(frame.right, frame.forward, frame.up);
    }

    RigidOffset OffsetFromTransforms(const pose::WorldTransform& parent, const pose::WorldTransform& child)
    {
        const pose::Quat inverse = pose::Conjugate(pose::Normalize(parent.rotation));
        RigidOffset offset;
        const pose::Quat rotation = pose::Normalize(pose::Multiply(inverse, pose::Normalize(child.rotation)));
        pose::BasisFromQuat(rotation, offset.right, offset.forward, offset.up);
        offset.position = pose::Rotate(inverse, {child.position.x - parent.position.x,
            child.position.y - parent.position.y, child.position.z - parent.position.z});
        return offset;
    }

    FollowPlan PlanFollow(const FollowInput& input, const FollowThresholds& limits)
    {
        FollowPlan plan;
        double mountAt[3]{};
        double riderAt[3]{};
        AbsolutePosition(input.mount, mountAt);
        AbsolutePosition(input.rider, riderAt);
        const double mountMove[3] = {mountAt[0] - input.mountPlaced.position.x, mountAt[1] - input.mountPlaced.position.y,
            mountAt[2] - input.mountPlaced.position.z};
        const double riderMove[3] = {riderAt[0] - input.riderPlaced.position.x, riderAt[1] - input.riderPlaced.position.y,
            riderAt[2] - input.riderPlaced.position.z};
        plan.mountMovedM = Length3(mountMove[0], mountMove[1], mountMove[2]);
        plan.mountTurnedRad = pose::AngleBetween(FrameRotation(input.mount), pose::Normalize(input.mountPlaced.rotation));
        plan.riderMovedM = Length3(riderMove[0], riderMove[1], riderMove[2]);
        plan.joltM = Length3(riderMove[0] - mountMove[0], riderMove[1] - mountMove[1], riderMove[2] - mountMove[2]);

        // The follow target: the mount now composed with the offset, in the
        // rider's cells (child follows parent).
        const EngineFrame& mount = input.mount;
        const RigidOffset& offset = input.offset;
        plan.right = Apply(mount.right, mount.forward, mount.up, offset.right);
        plan.forward = Apply(mount.right, mount.forward, mount.up, offset.forward);
        plan.up = Apply(mount.right, mount.forward, mount.up, offset.up);
        const pose::Vec3 mountLocal = LocalIn(mount, input.rider.cellX, input.rider.cellY);
        const pose::Vec3 lever = Apply(mount.right, mount.forward, mount.up, offset.position);
        plan.position = {mountLocal.x + lever.x, mountLocal.y + lever.y, mountLocal.z + lever.z};

        const double cx = static_cast<double>(plan.position.x) - input.rider.local.x;
        const double cy = static_cast<double>(plan.position.y) - input.rider.local.y;
        const double cz = static_cast<double>(plan.position.z) - input.rider.local.z;
        plan.correctionM = Length3(cx, cy, cz);
        plan.correctionRad = pose::AngleBetween(pose::QuatFromBasis(plan.right, plan.forward, plan.up), FrameRotation(input.rider));

        bool finite = FiniteDouble(plan.correctionM) && FiniteBits(plan.correctionRad);
        for (const pose::Vec3* v : {&plan.right, &plan.forward, &plan.up, &plan.position})
            finite = finite && FiniteBits(v->x) && FiniteBits(v->y) && FiniteBits(v->z);
        const bool past = finite && (plan.correctionM > limits.writeM || plan.correctionRad > limits.writeRad);
        plan.refused = input.enabled && past && plan.correctionM > limits.refuseM;
        plan.write = input.enabled && past && !plan.refused;
        if (plan.write)
        {
            // The rider's displacement once at the target, against the mount's.
            const double after[3] = {riderMove[0] + cx, riderMove[1] + cy, riderMove[2] + cz};
            plan.joltAfterM = Length3(after[0] - mountMove[0], after[1] - mountMove[1], after[2] - mountMove[2]);
        }
        else
            plan.joltAfterM = plan.joltM;
        return plan;
    }

    ParentCheck CheckParent(std::uint64_t published, bool walked, std::uint64_t current)
    {
        if (!published)
            return ParentCheck::None;
        return walked && current == published ? ParentCheck::Same : ParentCheck::Stale;
    }

    void Totals::NoteTick(std::uint32_t pairs)
    {
        ++m_ticks;
        m_pairs += pairs;
    }

    void Totals::NoteDrain(bool waited, bool timedOut)
    {
        m_drainWaits += waited ? 1 : 0;
        m_drainTimeouts += timedOut ? 1 : 0;
    }

    void Totals::Add(const FrameRecord& record, const FollowThresholds& limits, std::string& line)
    {
        line.clear();
        if (record.calls == 0)
        {
            ++m_missed; // published, but the rider had no pre-render (off screen, not listed)
            return;
        }
        ++m_riderCalls;
        m_multi += record.calls > 1 ? 1 : 0;
        bool notable = false;
        // A rider whose skeleton parent changed since the publish (its parent
        // matrix left alone).
        if (record.parentStale)
        {
            ++m_parentStale;
            notable = true;
        }
        if (record.readFail)
        {
            ++m_readFail;
            notable = true;
        }
        else
        {
            const FollowPlan& plan = record.plan;
            if (record.ageUs >= 0)
            {
                m_ageMinUs = m_ageMinUs < 0 ? record.ageUs : (std::min)(m_ageMinUs, record.ageUs);
                m_ageMaxUs = (std::max)(m_ageMaxUs, record.ageUs);
                m_ageSumUs += record.ageUs;
                ++m_ageCount;
            }
            m_duringTick += record.duringTick ? 1 : 0;
            if (plan.mountMovedM >= limits.movedM)
            {
                ++m_mountMoved1cm;
                m_mountSumM += plan.mountMovedM;
                notable = true;
            }
            m_mountMoved5cm += plan.mountMovedM >= limits.engineMovedM ? 1 : 0;
            m_mountMaxM = (std::max)(m_mountMaxM, plan.mountMovedM);
            if (plan.riderMovedM >= limits.movedM)
            {
                ++m_riderMoved1cm;
                notable = true;
            }
            if (plan.joltM >= limits.joltM)
            {
                ++m_drawnJolt;
                notable = true;
            }
            m_drawnJoltMaxM = (std::max)(m_drawnJoltMaxM, plan.joltM);
            m_entityParentDiffer += record.mountEntityParentM >= limits.movedM ? 1 : 0;
            if (record.wrote)
            {
                ++m_repositions;
                m_appliedSumM += plan.correctionM;
                m_appliedMaxM = (std::max)(m_appliedMaxM, plan.correctionM);
                m_appliedMaxRad = (std::max)(m_appliedMaxRad, plan.correctionRad);
                notable = notable || plan.correctionM >= limits.movedM;
            }
            m_leftJolt += plan.joltAfterM >= limits.joltM ? 1 : 0;
            if (plan.refused)
            {
                ++m_refused;
                notable = true;
            }
            if (record.writeFail)
            {
                ++m_writeFail;
                notable = true;
            }
        }
        if (!notable || m_lines >= kLineCap)
            return;
        ++m_lines;
        const FollowPlan& plan = record.plan;
        char text[480]{};
        std::snprintf(text, sizeof(text),
            "POSE_V2_MOUNT_PRERENDER,phase=%s,rider=%s,mount=%s,playhead_ms=%u,mount_m=%.3f,mount_deg=%.2f,rider_m=%.3f,"
            "jolt_m=%.3f,applied_m=%.3f,applied_deg=%.2f,jolt_after_m=%.3f,entity_parent_m=%.3f,calls=%u,age_us=%lld,"
            "during_tick=%d,parent_stale=%d\n",
            PhaseText(record, limits), entity::IdText(entity::ReplayId::FromPacked(record.rider)).c_str(),
            entity::IdText(entity::ReplayId::FromPacked(record.mount)).c_str(), record.playheadMs, plan.mountMovedM,
            plan.mountTurnedRad * 57.29578f, plan.riderMovedM, plan.joltM, record.wrote ? plan.correctionM : 0.0,
            record.wrote ? plan.correctionRad * 57.29578f : 0.0f, plan.joltAfterM, record.mountEntityParentM, record.calls,
            static_cast<long long>(record.ageUs), record.duringTick ? 1 : 0, record.parentStale ? 1 : 0);
        line = text;
    }

    std::uint32_t SharedMountRiders(const std::uint64_t* mounts, std::size_t count, std::uint32_t& ridersPerMount)
    {
        // A handful of riders per tick: the quadratic count needs no allocation.
        std::uint32_t shared = 0;
        ridersPerMount = 0;
        for (std::size_t n = 0; n < count; ++n)
        {
            if (!mounts[n])
                continue;
            std::uint32_t same = 0;
            for (std::size_t k = 0; k < count; ++k)
                same += mounts[k] == mounts[n] ? 1 : 0;
            ridersPerMount = (std::max)(ridersPerMount, same);
            shared += same >= 2 ? 1 : 0;
        }
        return shared;
    }

    void Totals::NoteShared(std::uint32_t sharedRiders, std::uint32_t ridersPerMount)
    {
        m_sharedRiders += sharedRiders;
        m_maxRidersPerMount = (std::max)(m_maxRidersPerMount, ridersPerMount);
    }

    std::string Totals::Summary(bool enabled, const char* hook) const
    {
        char text[1024]{};
        std::snprintf(text, sizeof(text),
            "POSE_V2_MOUNT_PRERENDER_SUMMARY,enabled=%d,hook=%s,ticks=%llu,pairs=%llu,rider_calls=%llu,missed=%llu,"
            "multi_calls=%llu,mount_moved_1cm=%llu,mount_moved_5cm=%llu,mount_max_m=%.3f,mount_mean_m=%.3f,"
            "rider_moved_1cm=%llu,drawn_jolt_4cm=%llu,drawn_jolt_max_m=%.3f,repositions=%llu,applied_max_m=%.3f,"
            "applied_mean_m=%.3f,applied_max_deg=%.2f,left_jolt_4cm=%llu,refused=%llu,read_fail=%llu,write_fail=%llu,"
            "entity_parent_differ=%llu,during_tick=%llu,calls_during_tick=%llu,age_min_us=%lld,age_mean_us=%lld,"
            "age_max_us=%lld,drain_waits=%llu,drain_timeouts=%llu,shared_mount_riders=%llu,max_riders_per_mount=%u,"
            "parent_stale=%llu,lines=%u\n",
            enabled ? 1 : 0, hook ? hook : "none", static_cast<unsigned long long>(m_ticks),
            static_cast<unsigned long long>(m_pairs), static_cast<unsigned long long>(m_riderCalls),
            static_cast<unsigned long long>(m_missed), static_cast<unsigned long long>(m_multi),
            static_cast<unsigned long long>(m_mountMoved1cm), static_cast<unsigned long long>(m_mountMoved5cm),
            m_mountMaxM, m_mountMoved1cm ? m_mountSumM / static_cast<double>(m_mountMoved1cm) : 0.0,
            static_cast<unsigned long long>(m_riderMoved1cm), static_cast<unsigned long long>(m_drawnJolt), m_drawnJoltMaxM,
            static_cast<unsigned long long>(m_repositions), m_appliedMaxM,
            m_repositions ? m_appliedSumM / static_cast<double>(m_repositions) : 0.0, m_appliedMaxRad * 57.29578f,
            static_cast<unsigned long long>(m_leftJolt), static_cast<unsigned long long>(m_refused),
            static_cast<unsigned long long>(m_readFail), static_cast<unsigned long long>(m_writeFail),
            static_cast<unsigned long long>(m_entityParentDiffer), static_cast<unsigned long long>(m_duringTick),
            static_cast<unsigned long long>(m_callsDuringTick), static_cast<long long>(m_ageMinUs),
            static_cast<long long>(m_ageCount ? m_ageSumUs / static_cast<std::int64_t>(m_ageCount) : -1),
            static_cast<long long>(m_ageMaxUs), static_cast<unsigned long long>(m_drainWaits),
            static_cast<unsigned long long>(m_drainTimeouts), static_cast<unsigned long long>(m_sharedRiders),
            m_maxRidersPerMount, static_cast<unsigned long long>(m_parentStale), m_lines);
        return text;
    }
    // ------------------------------------------------------------------
    // Every puppet at the end of the frame.

    PlacePlan PlanPlace(const EngineFrame& now, const pose::WorldTransform& placed, bool enabled,
        const PlaceThresholds& limits)
    {
        PlacePlan plan;
        double at[3]{};
        AbsolutePosition(now, at);
        const double dx = at[0] - placed.position.x;
        const double dy = at[1] - placed.position.y;
        const double dz = at[2] - placed.position.z;
        plan.movedM = Length3(dx, dy, dz);
        const pose::Quat rotation = pose::Normalize(placed.rotation);
        plan.turnedRad = pose::AngleBetween(FrameRotation(now), rotation);
        pose::BasisFromQuat(rotation, plan.right, plan.forward, plan.up);
        // The placement relative to now's cell origin (double first: the
        // world position is up to a few thousand metres, the remainder is
        // what the engine keeps in float).
        plan.position = {
            static_cast<float>(static_cast<double>(placed.position.x) -
                static_cast<double>(now.cellX) * static_cast<double>(pose::kWorldCellSize)),
            static_cast<float>(static_cast<double>(placed.position.y) -
                static_cast<double>(now.cellY) * static_cast<double>(pose::kWorldCellSize)),
            placed.position.z,
        };
        bool finite = FiniteDouble(plan.movedM) && FiniteBits(plan.turnedRad);
        for (const pose::Vec3* v : {&plan.right, &plan.forward, &plan.up, &plan.position})
            finite = finite && FiniteBits(v->x) && FiniteBits(v->y) && FiniteBits(v->z);
        const bool past = finite && (plan.movedM > limits.writeM || plan.turnedRad > limits.writeRad);
        plan.refused = enabled && past && plan.movedM > limits.refuseM;
        plan.write = enabled && past && !plan.refused;
        return plan;
    }

    void PuppetTotals::NoteTick(std::uint32_t entries)
    {
        ++m_ticks;
        m_entries += entries;
    }

    void PuppetTotals::Add(const PuppetRecord& record, const PlaceThresholds& limits, std::string& line)
    {
        line.clear();
        if (record.calls == 0)
        {
            ++m_missed; // published, no pre-render this frame (not in the lists: off screen, culled)
            return;
        }
        const bool vehicle = record.role == PuppetRole::Vehicle;
        ++m_calls;
        m_vehicleCalls += vehicle ? 1 : 0;
        m_multi += record.calls > 1 ? 1 : 0;
        bool notable = false;
        if (record.parentStale)
            ++m_parentStale;
        if (record.staleWrote)
            ++m_staleWrites;
        if (record.staleBudget)
            ++m_staleBudget;
        if (record.readFail)
        {
            ++m_readFail;
            notable = true;
        }
        else
        {
            const PlacePlan& plan = record.plan;
            if (plan.movedM >= limits.movedM)
            {
                ++m_moved1cm;
                m_vehicleMoved1cm += vehicle ? 1 : 0;
            }
            if (plan.movedM >= limits.engineMovedM)
            {
                ++m_moved5cm;
                notable = true;
            }
            m_movedMaxM = (std::max)(m_movedMaxM, plan.movedM);
            if (record.wrote)
            {
                ++m_writes;
                m_vehicleWrites += vehicle ? 1 : 0;
            }
            if (plan.refused)
            {
                ++m_refused;
                notable = true;
            }
            if (record.writeFail)
            {
                ++m_writeFail;
                notable = true;
            }
        }
        if (!notable || m_lines >= kLineCap)
            return;
        ++m_lines;
        const char* phase = record.readFail ? "read_fail"
            : record.writeFail             ? "write_fail"
            : record.plan.refused          ? "refused"
            : record.wrote                 ? "placed_back"
                                           : "moved";
        const char* role = record.role == PuppetRole::Rider ? "rider" : vehicle ? "vehicle" : "puppet";
        char text[320]{};
        std::snprintf(text, sizeof(text),
            "POSE_V2_PRERENDER,phase=%s,role=%s,id=%s,playhead_ms=%u,moved_m=%.3f,turned_deg=%.2f,calls=%u,"
            "stale_pose=%d,parent_stale=%d\n",
            phase, role, entity::IdText(entity::ReplayId::FromPacked(record.packed)).c_str(), record.playheadMs,
            record.plan.movedM, record.plan.turnedRad * 57.29578f, record.calls, record.staleWrote ? 1 : 0,
            record.parentStale ? 1 : 0);
        line = text;
    }

    std::string PuppetTotals::Summary(bool enabled, bool physicsFilter) const
    {
        char text[768]{};
        std::snprintf(text, sizeof(text),
            "POSE_V2_PRERENDER_SUMMARY,frame_end=%d,physics_filter=%d,ticks=%llu,entries=%llu,calls=%llu,missed=%llu,"
            "multi_calls=%llu,moved_1cm=%llu,moved_5cm=%llu,moved_max_m=%.3f,placed_back=%llu,refused=%llu,"
            "read_fail=%llu,write_fail=%llu,parent_stale=%llu,stale_pose_writes=%llu,stale_budget_skips=%llu,"
            "vehicle_calls=%llu,vehicle_moved_1cm=%llu,vehicle_writes=%llu,lines=%u\n",
            enabled ? 1 : 0, physicsFilter ? 1 : 0, static_cast<unsigned long long>(m_ticks),
            static_cast<unsigned long long>(m_entries), static_cast<unsigned long long>(m_calls),
            static_cast<unsigned long long>(m_missed), static_cast<unsigned long long>(m_multi),
            static_cast<unsigned long long>(m_moved1cm), static_cast<unsigned long long>(m_moved5cm), m_movedMaxM,
            static_cast<unsigned long long>(m_writes), static_cast<unsigned long long>(m_refused),
            static_cast<unsigned long long>(m_readFail), static_cast<unsigned long long>(m_writeFail),
            static_cast<unsigned long long>(m_parentStale), static_cast<unsigned long long>(m_staleWrites),
            static_cast<unsigned long long>(m_staleBudget), static_cast<unsigned long long>(m_vehicleCalls),
            static_cast<unsigned long long>(m_vehicleMoved1cm), static_cast<unsigned long long>(m_vehicleWrites),
            m_lines);
        return text;
    }
}
