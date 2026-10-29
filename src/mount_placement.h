#pragma once

// A rider puppet follows where its mount puppet really is in the drawn frame
// (MountPreRender in the ini, default 0), and every puppet is written back to
// its placement at the end of the frame (FrameEnd).
//
// The galloping horse puppet is a physics body (for the plants); the engine
// moves it 4 to 11 cm off our placement while the frozen rider stays where
// the recording put him. As a replay editor usually does, the child follows
// its parent in the same frame and the entities are written at the very end
// of the frame so nothing overrides them.
//
// Mechanism: a detour on the ped pre-render, the last per-entity step
// before the frame is drawn. At a rider puppet's pre-render, before the
// original runs, the rider's transform is set to the mount puppet's
// transform at that moment composed with the recorded rider-in-mount
// offset of this tick (child follows parent). A horse that was not moved
// gives the recorded rider placement back (no write); a pushed horse
// carries its rider along. Only replay pairs the playback published this
// tick are touched; every other ped goes straight to the original.
//
// Engine layout (build 1491.50):
// - Ped vtable 0x342EB80: the only .rdata vtable whose +0xB8 slot is the
//   ped fragment getter (skeleton_walk.h kPedGetter); it shares the entity
//   and physical slots with the vehicle vtables. Horses use it too (their
//   skeleton walk classifies as ped).
// - Slot +0x258 = 0xCC9698 (.pdata start, no direct caller, only that
//   vtable slot points at it): the same slot as the vehicle pre-render
//   (0x14B5E00 / 0x14B5B84 -> 0x15AD298).
//   It is called by the pre-render list loops 0x5C2D60, 0x5C2E70 and
//   0x5C2F48 (one call per listed entity, the argument says whether the
//   entity was visible in the main viewport this frame), which run inside
//   the job 0x5C2C3C over six entity lists. The body reads the entity's
//   final position (+0x70, cells) for its lighting probe. No position
//   encoder (0x68C654, 0x179DF4) is reached from it in five levels of
//   direct calls (virtual calls not followed): it does not move the ped.
// - Entity matrix +0x40 right, +0x50 forward, +0x60 up, +0x70 position
//   remainder with the 32 m cell indices (int16 x, y) in the w lane:
//   GET_ENTITY_MATRIX 0xF9744C and the position encoder 0x68C654 (cell =
//   truncation of position / 32 toward zero, z kept whole).
// - A ped's skeleton is the fragment one (fragment_path=1); its parent
//   matrix is [cacheEntry+0xE8]+0x40 (0x29727E8), the physics instance
//   matrix, written by 0x179DF4 with the same layout. So a ped has two
//   copies of its transform; both are written here.
// - The order "physics, then script, then pre-render, then draw" (pre-render
//   the last step of the update) is a hypothesis for RDR2. The evidence
//   fields below show whether the horse is already off its placement at the
//   pre-render (the jolt is drawn and this follow removes it) or only later
//   (the jolt is never drawn and the next-tick detector counts something the
//   screen did not show).
//
// Rungs: 5 (detour with a 32-byte prologue signature, the vtable slot and
// the ped getter checked at load, forwards every call, acts only on replay
// riders) and 3 (guarded writes of the rider puppet's two matrices, offline
// layout). The detour never calls a native; the script thread publishes the
// pairs and reads the evidence back.
//
// The pure part (matrix decode and encode with cells, the rigid offset, the
// follow plan, the evidence totals and log lines) is mount_placement_core.cpp,
// which ReplayCoreTests compiles too; the
// detour, the pair table and the engine reads and writes are
// mount_placement.cpp (ReplayProbe).

#include "pose_math.h"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>

namespace replay::playback
{
    struct Puppets;
}

namespace replay::mount
{
    // ------------------------------------------------------------------
    // Offline layout (build 1491.50).
    constexpr std::uintptr_t kPedPreRenderRva = 0xCC9698;
    constexpr std::uint8_t kPedPreRenderSignature[] = {
        0x48, 0x8B, 0xC4,                         // mov rax, rsp
        0x48, 0x89, 0x58, 0x10,                   // mov [rax+10h], rbx
        0x55, 0x56, 0x57,                         // push rbp, rsi, rdi
        0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, // push r12..r15
        0x48, 0x8D, 0x68, 0xA1,                   // lea rbp, [rax-5Fh]
        0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00, // sub rsp, 0B0h
        0x4C, 0x8B, 0x05,                         // mov r8, [rip+...] (the displacement is not compared)
    };
    constexpr std::uintptr_t kPedVtableRva = 0x342EB80;
    constexpr std::size_t kPreRenderSlot = 0x258;
    constexpr std::size_t kFragmentGetterSlot = 0xB8;
    constexpr std::size_t kEntityMatrixOffset = 0x40; // 64 bytes: four float4 rows
    constexpr std::size_t kMatrixBytes = 64;

    // ------------------------------------------------------------------
    // Pure part (mount_placement_core.cpp).

    // An engine matrix (entity +0x40 or a physics instance +0x40) decoded:
    // the basis rows and the position split the way the engine stores it.
    struct EngineFrame
    {
        pose::Vec3 right;
        pose::Vec3 forward;
        pose::Vec3 up;
        std::int16_t cellX = 0;
        std::int16_t cellY = 0;
        pose::Vec3 local; // x, y relative to the cell origin; z absolute
    };

    // False when the rows are not a rotation (lengths off 1 by 0.01 or more)
    // or a value is not finite.
    bool DecodeEngineMatrix(const float matrix[16], EngineFrame& frame);
    // World position in double (cell * 32 + local).
    void AbsolutePosition(const EngineFrame& frame, double out[3]);
    // The position relative to the origin of another cell (exact for the
    // cells around it: the cell difference times 32 is exact in float).
    pose::Vec3 LocalIn(const EngineFrame& frame, std::int16_t cellX, std::int16_t cellY);
    // Writes the basis rows and the position into `matrix`, keeping every w
    // lane (the position row's w holds the cell indices: the position is
    // written relative to the matrix's own cells). `position` is relative to
    // the origin of cell (cellX, cellY).
    void EncodeKeepingCells(const pose::Vec3& right, const pose::Vec3& forward, const pose::Vec3& up,
        const pose::Vec3& position, std::int16_t cellX, std::int16_t cellY, float matrix[16]);
    // The rows and the quaternion of the same rotation.
    pose::Quat FrameRotation(const EngineFrame& frame);

    // The child's transform in its parent's space, as basis rows and a
    // position (from the two recorded transforms of the same tick).
    struct RigidOffset
    {
        pose::Vec3 right{1.0f, 0.0f, 0.0f};
        pose::Vec3 forward{0.0f, 1.0f, 0.0f};
        pose::Vec3 up{0.0f, 0.0f, 1.0f};
        pose::Vec3 position;
    };
    RigidOffset OffsetFromTransforms(const pose::WorldTransform& parent, const pose::WorldTransform& child);

    struct FollowThresholds
    {
        float writeM = 0.002f;          // the rider is moved only past 2 mm
        float writeRad = 0.00175f;      // or 0.1 degree
        float refuseM = 1.5f;           // a correction this large is a wrong pair: refused
        float movedM = 0.01f;           // evidence: off its placement by 1 cm
        float joltM = 0.04f;            // the detector's mount_offset threshold
        float engineMovedM = 0.05f;     // the detector's engine_moved threshold
    };

    struct FollowInput
    {
        EngineFrame mount;              // the mount now (its skeleton parent matrix)
        EngineFrame rider;              // the rider now (its entity matrix)
        pose::WorldTransform mountPlaced; // where this tick placed them
        pose::WorldTransform riderPlaced;
        RigidOffset offset;             // rider in mount space, this tick's recording
        bool enabled = false;           // MountPreRender=1
    };

    struct FollowPlan
    {
        double mountMovedM = 0.0;       // the mount off its placement now
        float mountTurnedRad = 0.0f;
        double riderMovedM = 0.0;       // the rider off his placement now (before any write)
        double joltM = 0.0;             // the rider's displacement against the mount's: what this frame draws
        double correctionM = 0.0;       // the follow target against the rider now
        float correctionRad = 0.0f;
        double joltAfterM = 0.0;        // the same jolt after the write (the rider at the target)
        bool write = false;
        bool refused = false;           // enabled, past writeM, but past refuseM
        // The target, in the rider's cells.
        pose::Vec3 right;
        pose::Vec3 forward;
        pose::Vec3 up;
        pose::Vec3 position;
    };
    FollowPlan PlanFollow(const FollowInput& input, const FollowThresholds& limits = {});

    // A horse with a
    // passenger behind the saddle has two riders; each is a pair of its own
    // in the table (its own recorded offset in the mount's space), so the
    // follow and the end-of-frame placement treat the passenger exactly as
    // the rider. This counts, from the mounts of the riders published this
    // tick (engine addresses, 0 ignored), the riders whose mount carries
    // another one too, and the most riders one mount carries.
    std::uint32_t SharedMountRiders(const std::uint64_t* mounts, std::size_t count, std::uint32_t& ridersPerMount);

    // The rider's skeleton parent matrix is
    // published as a pointer; the detour walks the chain again from the ped
    // it received and writes the parent only while it is the same one (a
    // skeleton rebuilt after the publish leaves the published matrix stale).
    // None: nothing published (no separate parent to write).
    enum class ParentCheck : std::uint8_t
    {
        None,
        Same,
        Stale, // the walk failed or found another matrix: no parent write
    };
    ParentCheck CheckParent(std::uint64_t published, bool walked, std::uint64_t current);

    // One rider pre-render as the script thread reads it back.
    struct FrameRecord
    {
        std::uint32_t rider = 0;        // packed ReplayIds
        std::uint32_t mount = 0;
        std::uint32_t playheadMs = 0;
        std::uint32_t calls = 0;        // pre-render calls for the rider since the publish
        bool readFail = false;
        bool writeFail = false;
        bool wrote = false;
        bool duringTick = false;        // the call came while the playback tick ran
        bool parentStale = false;       // ParentCheck::Stale, parent not written
        std::int64_t ageUs = -1;        // publish to pre-render, microseconds
        double mountEntityParentM = 0.0; // the mount's entity matrix against its skeleton parent
        FollowPlan plan;
    };

    // Totals for the playback and the evidence lines (capped).
    class Totals
    {
    public:
        static constexpr std::uint32_t kLineCap = 48;
        void Reset() { *this = Totals{}; }
        void NoteTick(std::uint32_t pairs);
        // Of this tick's pairs, the riders that share their mount
        // with another rider, and the most riders on one mount.
        void NoteShared(std::uint32_t sharedRiders, std::uint32_t ridersPerMount);
        void NoteDrain(bool waited, bool timedOut);
        void NoteCallsDuringTick(std::uint64_t calls) { m_callsDuringTick += calls; }
        // Adds one record; appends its evidence line to `line` (empty when
        // the record is ordinary or the cap is reached).
        void Add(const FrameRecord& record, const FollowThresholds& limits, std::string& line);
        std::string Summary(bool enabled, const char* hook) const;
        std::uint64_t Ticks() const { return m_ticks; }
        std::uint64_t Repositions() const { return m_repositions; }

    private:
        std::uint64_t m_ticks = 0;
        std::uint64_t m_pairs = 0;
        std::uint64_t m_riderCalls = 0;
        std::uint64_t m_missed = 0;
        std::uint64_t m_multi = 0;
        std::uint64_t m_mountMoved1cm = 0;
        std::uint64_t m_mountMoved5cm = 0;
        double m_mountMaxM = 0.0;
        double m_mountSumM = 0.0;
        std::uint64_t m_riderMoved1cm = 0;
        std::uint64_t m_drawnJolt = 0;
        double m_drawnJoltMaxM = 0.0;
        std::uint64_t m_repositions = 0;
        double m_appliedMaxM = 0.0;
        double m_appliedSumM = 0.0;
        float m_appliedMaxRad = 0.0f;
        std::uint64_t m_leftJolt = 0;
        std::uint64_t m_refused = 0;
        std::uint64_t m_readFail = 0;
        std::uint64_t m_writeFail = 0;
        std::uint64_t m_entityParentDiffer = 0;
        std::uint64_t m_parentStale = 0;
        std::uint64_t m_sharedRiders = 0;
        std::uint32_t m_maxRidersPerMount = 0;
        std::uint64_t m_duringTick = 0;
        std::uint64_t m_callsDuringTick = 0;
        std::int64_t m_ageMinUs = -1;
        std::int64_t m_ageMaxUs = -1;
        std::int64_t m_ageSumUs = 0;
        std::uint64_t m_ageCount = 0;
        std::uint64_t m_drainWaits = 0;
        std::uint64_t m_drainTimeouts = 0;
        std::uint32_t m_lines = 0;
    };

    // ------------------------------------------------------------------
    // Every puppet at the end of the frame, written after the whole update
    // so nothing can override it. The same
    // ped pre-render detour, for every placed ped and horse puppet (not
    // only riders), and the vehicle pre-render base (pose_capture.cpp) for
    // vehicle puppets: the entity matrix (and, for peds, the physics
    // instance matrix the skeleton hangs from) is set back to where this
    // tick placed the puppet when the engine moved it since. Also, for a
    // ped whose pose finalizer did not run since the publish, the
    // published pose is written there (pose_stale).

    struct PlaceThresholds
    {
        float writeM = 0.002f;     // written only past 2 mm
        float writeRad = 0.00175f; // or 0.1 degree
        float refuseM = 25.0f;     // this far off is not a push but a wrong entry: refused, counted
        float movedM = 0.01f;      // evidence: moved by 1 cm
        float engineMovedM = 0.05f; // the detector's engine_moved threshold
    };

    // The placement in the frame's own cells, and whether to write it.
    struct PlacePlan
    {
        double movedM = 0.0;
        float turnedRad = 0.0f;
        bool write = false;
        bool refused = false;
        pose::Vec3 right;
        pose::Vec3 forward;
        pose::Vec3 up;
        pose::Vec3 position; // relative to the origin of now's cell
    };
    PlacePlan PlanPlace(const EngineFrame& now, const pose::WorldTransform& placed, bool enabled,
        const PlaceThresholds& limits = {});

    // The finalizer did not write the slot since the publish took its
    // count: the frame would draw an older pose.
    constexpr bool PoseStale(std::uint64_t appliesAtPublish, std::uint64_t appliesNow)
    {
        return appliesNow == appliesAtPublish;
    }

    enum class PuppetRole : std::uint8_t
    {
        Rider,   // a ped on a horse puppet (the follow evidence, and placed like the others under FrameEnd)
        Puppet,  // any other placed ped or horse puppet (FrameEnd only)
        Vehicle, // a vehicle puppet, at the vehicle pre-render (FrameEnd only)
    };

    // One puppet's pre-render as the script thread reads it back.
    struct PuppetRecord
    {
        std::uint32_t packed = 0;
        std::uint32_t playheadMs = 0;
        std::uint32_t calls = 0;
        PuppetRole role = PuppetRole::Puppet;
        bool readFail = false;
        bool writeFail = false;
        bool wrote = false;
        bool parentStale = false;
        bool staleWrote = false;     // the published pose written here (pose_stale)
        bool staleBudget = false;    // stale, but the frame's write budget was spent
        PlacePlan plan;
    };

    class PuppetTotals
    {
    public:
        static constexpr std::uint32_t kLineCap = 48;
        void Reset() { *this = PuppetTotals{}; }
        void NoteTick(std::uint32_t entries);
        void Add(const PuppetRecord& record, const PlaceThresholds& limits, std::string& line);
        std::string Summary(bool enabled, bool physicsFilter) const;
        std::uint64_t Ticks() const { return m_ticks; }
        std::uint64_t Writes() const { return m_writes; }
        std::uint64_t StaleWrites() const { return m_staleWrites; }
        std::uint64_t Moved5cm() const { return m_moved5cm; }

    private:
        std::uint64_t m_ticks = 0;
        std::uint64_t m_entries = 0;
        std::uint64_t m_calls = 0;
        std::uint64_t m_missed = 0;
        std::uint64_t m_multi = 0;
        std::uint64_t m_moved1cm = 0;
        std::uint64_t m_moved5cm = 0;
        double m_movedMaxM = 0.0;
        std::uint64_t m_writes = 0;
        std::uint64_t m_refused = 0;
        std::uint64_t m_readFail = 0;
        std::uint64_t m_writeFail = 0;
        std::uint64_t m_parentStale = 0;
        std::uint64_t m_staleWrites = 0;
        std::uint64_t m_staleBudget = 0;
        std::uint64_t m_vehicleCalls = 0;
        std::uint64_t m_vehicleMoved1cm = 0;
        std::uint64_t m_vehicleWrites = 0;
        std::uint32_t m_lines = 0;
    };

    // ------------------------------------------------------------------
    // Mod part (mount_placement.cpp, ReplayProbe only).

    // MountPreRender in the ini (default 0): 1 writes the follow; 0 only
    // measures (the same reads, no write).
    void SetEnabled(bool on);
    bool Enabled();
    // FrameEnd in the ini (default: JitterFix, 1). 1 publishes every placed
    // puppet for the end-of-frame write above; 0: riders only,
    // MountPreRender decides.
    void SetFrameEnd(bool on);
    bool FrameEnd();
    // For the summary line (the physics filter is replay_playback's).
    void SetPhysicsFilterNote(bool on);
    // The vehicle pre-render base detour (pose_capture.cpp) calls this
    // before its original: a published vehicle puppet's entity matrix is
    // set back to its placement. Detour context, never a native.
    void VehiclePreRender(void* vehicle);
    // At load, inside the hook batch (hook_batch.h): signature, vtable slot
    // and ped getter checked, then the detour queued. False with `error`.
    bool Install(std::string& error);
    bool Installed();
    // Unload, after every hook was disabled and the detours drained.
    void Shutdown();

    // Playback, script thread. BeginTick first in the playback tick, before
    // any puppet can be deleted: the pairs of the last tick are withdrawn,
    // the detours using them drained, their records read back (evidence
    // lines into `log`). Publish after the tick placed every puppet and
    // updated the rider and mount pairs (`allowed` false: nothing published,
    // the CarriedPhysics A/B). EndPlayback before the puppets go: the same
    // as BeginTick, then the summary line and a reset.
    // Under FrameEnd, Publish also adds every other placed,
    // unattached ped, horse and vehicle puppet, and `allowed` does not
    // withholds the riders (the follow only measures then).
    // BeginTick returns false when the drain
    // timed out (a detour may still hold a pair): the tick must not delete
    // a puppet. EndPlayback waits up to a second (DrainDetours' pattern).
    // EndTick last in the playback tick (the
    // calls_during_tick evidence covers the whole tick, not up to Publish).
    bool BeginTick(std::ostream& log);
    void Publish(const playback::Puppets& puppets, std::uint32_t playheadMs, bool allowed);
    void EndTick();
    void EndPlayback(std::ostream& log);
    // The jerk detector's cause: this rider was moved with his mount at the
    // last pre-render (records read back in this tick's BeginTick).
    bool CarriedLastFrame(std::uint32_t packedRider);
}
