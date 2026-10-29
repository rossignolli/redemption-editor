#pragma once

// Rider and mount consistency, and the playback jerk detector (pure data).
//
// A mounted rider is its own puppet, placed from its own track and a physics
// body (unfrozen, map collision on, gravity off) while it moves upright,
// something the recording never had. A mounted ped keeps
// its standing capsule (it is not on a mount for the engine): with the
// pelvis 1.4 m over the ground the capsule reaches 0.4 m over it, and a
// kerb, a boardwalk edge or a rut pushes the rider out of its placement
// between our placement and the render, while the horse goes on. The
// engine gives a mounted rider no vegetation modifier anyway (0xC2CF14),
// so the physics bought nothing for him.
//
// Pairing: a ped whose recorded origin sits in a horse's saddle box (the
// horse's space, +Y forward) rides it. Entered in a tight box, left out of
// a looser one (no flicker at the edge). Nothing of this is in the clip:
// it is read from the two recorded transforms at the playhead.
//
// Detector: per puppet and tick of normal play, where the engine left the
// clone since our last placement (a physics push, an attachment, anything
// after the script thread), the same for the pair (the rider against its
// mount: the visible jerk), the playhead step against the frame's
// wall-clock step (a creation hold, a capped stall), and the skeleton pose
// not written since the last tick (the engine skipped the puppet's
// finalizer: the frame shows the previous pose). Each finding is a
// POSE_JERK line with the puppet, the time, the size and the causes.

#include "pose_math.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace replay::jerk
{
    // ---- Rider and mount -------------------------------------------------

    struct SaddleBox
    {
        float halfWidth;  // |x|
        float backY;      // y from
        float frontY;     // y to
        float lowZ;       // z from
        float highZ;      // z to
    };
    // A rider's origin in the horse's space measured x -0.04 to 0.03,
    // y 0.00 to 0.09, z 0.26 to 0.39 over a gallop. A ped
    // standing next to a horse is 0.6 m or more to the side and at about the
    // horse origin's height (z near 0). A second rider sits behind (y -0.7).
    constexpr SaddleBox kSaddleEnter{0.35f, -0.95f, 0.60f, 0.12f, 0.90f};
    constexpr SaddleBox kSaddleLeave{0.50f, -1.15f, 0.80f, -0.05f, 1.10f};

    // The rider's origin in the mount's space.
    pose::Vec3 InMountSpace(const pose::WorldTransform& mount, const pose::Vec3& riderOrigin);
    bool InSaddle(const pose::WorldTransform& mount, const pose::Vec3& riderOrigin, const SaddleBox& box);

    struct Body
    {
        std::uint32_t packed = 0;
        bool horse = false; // else a ped
        pose::WorldTransform world;
    };

    // Rider to mount pairs, updated once per tick from the recorded
    // transforms the tick placed. A rider keeps its mount while it stays in
    // the loose box; else it takes the nearest horse whose tight box holds it.
    //
    // A horse may carry more than one rider (a passenger sits about y -0.45
    // to -0.58 behind the saddle). The engine has seats: the game scripts put
    // the rider with SET_PED_ONTO_MOUNT(ped, horse, -1, true) and the
    // passenger with seat 0, and TASK_MOUNT_ANIMAL takes the same index after
    // _IS_MOUNT_SEAT_FREE(horse, 0). Both are on the mount for the engine
    // (IS_PED_ON_MOUNT, GET_MOUNT). The map holds one entry per rider, so a
    // passenger is a second rider of the same mount; the seat is read from
    // the geometry the same way
    // (the front-most rider of a horse sits in the saddle, seat -1, the
    // others behind take 0, 1, ...), nothing of it is in the clip.
    class MountPairs
    {
    public:
        static constexpr int kSaddleSeat = -1; // the engine's seat index of the rider in the saddle
        void Clear()
        {
            m_mountOf.clear();
            m_seatOf.clear();
            m_previousSeatOf.clear();
            m_seatChanges.clear();
        }
        void Update(const std::vector<Body>& bodies);
        // 0 when the ped rides nothing.
        std::uint32_t MountOf(std::uint32_t rider) const;
        // The riders a mount carries now (0 when none).
        std::uint32_t RidersOf(std::uint32_t mount) const;
        // The seat of a rider in the engine's numbering (-1 the
        // saddle, 0 the first passenger behind it); kSaddleSeat when the
        // ped rides nothing (ask MountOf first).
        int SeatOf(std::uint32_t rider) const;
        // The seat before the last Update
        // (kSaddleSeat when the ped rode nothing then), for a leave line.
        int PreviousSeatOf(std::uint32_t rider) const;
        // The riders that kept their mount in the last Update but changed
        // seat (a lone rider is -1; another one entering in front of him
        // makes him 0 with no enter of his own).
        const std::vector<std::uint32_t>& SeatChanges() const { return m_seatChanges; }
        std::size_t Size() const { return m_mountOf.size(); }
        const std::unordered_map<std::uint32_t, std::uint32_t>& Map() const { return m_mountOf; }
        std::uint64_t Enters() const { return m_enters; }
        std::uint64_t Leaves() const { return m_leaves; }
        // The most riders one mount carried since the last ResetShared.
        std::uint32_t MaxRiders() const { return m_maxRiders; }
        // Updates where some mount carried two riders or more.
        std::uint64_t SharedUpdates() const { return m_sharedUpdates; }
        // Per playback (the enter and leave counts stay cumulative).
        void ResetShared()
        {
            m_maxRiders = 0;
            m_sharedUpdates = 0;
        }

    private:
        std::unordered_map<std::uint32_t, std::uint32_t> m_mountOf;
        std::unordered_map<std::uint32_t, int> m_seatOf;
        std::unordered_map<std::uint32_t, int> m_previousSeatOf;
        std::vector<std::uint32_t> m_seatChanges;
        std::uint64_t m_enters = 0;
        std::uint64_t m_leaves = 0;
        std::uint32_t m_maxRiders = 0;
        std::uint64_t m_sharedUpdates = 0;
    };

    // ---- Detector --------------------------------------------------------

    enum Cause : std::uint32_t
    {
        kCauseSlowTick = 1u << 0,   // the frame took 50 ms or more
        kCauseCapped = 1u << 1,     // the playhead step was capped (kMaxNormalStepMs)
        kCauseHeld = 1u << 2,       // the playhead did not move (creation hold)
        kCausePhysics = 1u << 3,    // the clone is a physics body (unfrozen)
        kCauseNotWritten = 1u << 4, // no override write since the last tick
        kCauseSegment = 1u << 5,    // pose segment switched this tick
        kCauseCreated = 1u << 6,    // clone built within the last ticks
        kCauseMounted = 1u << 7,    // rider of a mount puppet
        kCauseFrozen = 1u << 8,     // moved although frozen
        kCauseAttached = 1u << 9,   // no longer set; kept so the cause columns stay
        // The last pre-render moved this
        // rider with his mount (mount_placement.h, MountPreRender=1). His
        // displacement is the mount's, not a jerk of his own: it counts in
        // the pair check, never as an engine_moved of the rider.
        kCausePreRender = 1u << 10,
    };
    constexpr int kCauseBits = 11;
    std::string CauseText(std::uint32_t causes);

    enum class Kind : std::uint8_t
    {
        EngineMoved = 0, // the clone was not where we placed it
        MountOffset = 1, // the rider moved against its mount
        TimeStep = 2,    // the playhead step did not follow the frame
        Stall = 3,       // a long frame (the game's, the motion is right)
        PoseStale = 4,   // the frame showed the previous tick's pose
        Count = 5,
    };
    const char* KindText(Kind kind);

    struct Thresholds
    {
        float engineMovedM = 0.05f;
        float mountOffsetM = 0.04f;
        float timeStepM = 0.05f;
        float stallMs = 50.0f;
        float poseStaleM = 0.03f;
        float timeSlackMs = 8.0f; // playhead against wall step, per frame
    };

    struct TickInfo
    {
        bool valid = false;       // a normal play tick after another one
        float wallStepMs = 0.0f;
        float playStepMs = 0.0f;
        float expectedStepMs = 0.0f; // wall step times the rate
        float lostMs = 0.0f;         // expected - play (0 when they agree)
        std::uint32_t causes = 0;    // slow_tick, capped, held
    };

    // One puppet in one tick (the playback fills it before placing).
    struct Observation
    {
        std::uint32_t packed = 0;
        std::uint32_t mount = 0;          // packed id of its mount, 0 none
        bool placed = false;              // placedWorld holds the last placement
        pose::Vec3 placedAt{};            // last placement asked
        bool hasEngine = false;
        pose::Vec3 engine{};              // where the engine left it
        pose::Vec3 recorded{};            // this tick's recorded position
        pose::Vec3 rootObject{};          // this tick's root bone, object space
        bool hasRoot = false;
        std::uint64_t overrideWrites = 0; // applies + alt + guard, cumulative
        bool hasOverride = false;
        std::uint32_t causes = 0;         // physics, segment, created, frozen
        std::uint32_t playheadMs = 0;
        // > 0 when the end-of-frame
        // placement (JitterFix) puts the puppet back before the frame is
        // drawn, up to this far: such an engine move is never seen.
        float frameEndRefuseM = 0.0f;
    };

    struct Finding
    {
        std::uint32_t packed = 0;
        Kind kind = Kind::EngineMoved;
        float magnitudeM = 0.0f;
        float engineM = 0.0f;   // this puppet's engine displacement
        float mountM = -1.0f;   // the pair's relative displacement (-1 none)
        float speedMps = 0.0f;  // recorded speed
        std::uint32_t mount = 0;
        std::uint32_t causes = 0;
        std::uint32_t playheadMs = 0;
        // A mount_offset the mount's own move made (its riders
        // stayed where placed) is one finding for the mount, whatever the
        // number of riders it carries; this is how many riders it shows on
        // (`packed` is the one with the largest offset). 1 otherwise.
        std::uint32_t riders = 1;
    };

    struct PuppetSummary
    {
        std::uint32_t packed = 0;
        std::uint32_t jerks = 0;
        float worstM = 0.0f;
        std::uint32_t worstMs = 0;
        Kind worstKind = Kind::EngineMoved;
        std::uint32_t byKind[static_cast<int>(Kind::Count)] = {};
    };

    struct Summary
    {
        std::uint64_t ticks = 0;          // normal play ticks observed
        std::uint64_t observations = 0;   // puppet ticks
        std::uint64_t jerks = 0;
        std::uint64_t byKind[static_cast<int>(Kind::Count)] = {};
        std::uint64_t byCause[kCauseBits] = {}; // bit index of Cause
        float worstM = 0.0f;
        std::uint32_t worstPacked = 0;
        std::uint32_t worstMs = 0;
        Kind worstKind = Kind::EngineMoved;
        std::uint64_t heldTicks = 0;
        std::uint64_t cappedTicks = 0;
        std::uint64_t slowTicks = 0;
        std::uint64_t placedBack = 0; // engine moves the end-of-frame placement undid (not findings)
        // Pair offsets the end-of-frame placement undid (every side
        // put back before the frame is drawn): one per mount move, not findings.
        std::uint64_t pairPlacedBack = 0;
        // The riders behind the mount_offset findings (a mount move
        // seen on two riders is one finding and two riders here).
        std::uint64_t mountOffsetRiders = 0;
    };

    // The pair offsets of one tick turned into findings.
    // A rider that moved on its own (riderMovedM at the mount_offset
    // threshold or more) is its own finding; the offsets a mount's move
    // made on riders that stayed are grouped per mount, one finding with
    // the largest. A group, or a lone rider, whose every offset the
    // end-of-frame placement undid is one placed back, no finding.
    // `onFinding(worstIndex, riders)` and `onPlacedBack()` are called in a
    // stable order (mount, then rider packed id).
    struct PairGroupInput
    {
        std::uint32_t rider = 0;
        std::uint32_t mount = 0;
        float relativeM = 0.0f;
        float riderMovedM = 0.0f;
        bool placedBack = false;
    };
    template <typename OnFinding, typename OnPlacedBack>
    void GroupPairOffsets(std::vector<PairGroupInput>& offsets, float ownMoveM, OnFinding&& onFinding,
        OnPlacedBack&& onPlacedBack)
    {
        std::sort(offsets.begin(), offsets.end(), [](const PairGroupInput& a, const PairGroupInput& b) {
            return a.mount != b.mount ? a.mount < b.mount : a.rider < b.rider;
        });
        // A rider that moved on its own: one finding each.
        for (std::size_t n = 0; n < offsets.size(); ++n)
        {
            if (offsets[n].riderMovedM < ownMoveM)
                continue;
            if (offsets[n].placedBack)
                onPlacedBack();
            else
                onFinding(n, 1u);
        }
        // The mount's moves, one per mount.
        std::size_t n = 0;
        while (n < offsets.size())
        {
            const std::uint32_t mount = offsets[n].mount;
            std::size_t worst = offsets.size();
            std::uint32_t riders = 0;
            bool allBack = true;
            for (; n < offsets.size() && offsets[n].mount == mount; ++n)
            {
                if (offsets[n].riderMovedM >= ownMoveM)
                    continue;
                ++riders;
                allBack = allBack && offsets[n].placedBack;
                if (worst == offsets.size() || offsets[n].relativeM > offsets[worst].relativeM)
                    worst = n;
            }
            if (!riders)
                continue;
            if (allBack)
                onPlacedBack();
            else
                onFinding(worst, riders);
        }
    }

    class Detector
    {
    public:
        explicit Detector(const Thresholds& thresholds = {}) : m_limits(thresholds) {}
        void Reset();
        // Once per tick, before the puppets. Not normal play (paused,
        // scrub, preparing) or a playhead that went back: no finding this
        // tick, and the puppets' previous state is forgotten.
        TickInfo BeginTick(std::uint32_t playheadMs, double wallMs, double rate, bool normalPlay);
        // Per puppet, before its placement. Findings are appended to `out`.
        void Observe(const Observation& observation, std::vector<Finding>& out);
        // The pair check needs both sides of the tick: after every
        // Observe, once per tick.
        void EndTick(std::vector<Finding>& out);
        const Summary& Totals() const { return m_summary; }
        const TickInfo& LastTick() const { return m_tick; }
        // The puppets with the most jerks, worst first.
        std::vector<PuppetSummary> Worst(std::size_t count) const;

    private:
        struct State
        {
            bool has = false;
            std::uint32_t lastTick = 0;  // tick serial of the last Observe
            pose::Vec3 recorded{};
            std::uint32_t playheadMs = 0;
            float speedMps = 0.0f;
            pose::Vec3 rootPrev{};       // root object of the last two ticks
            pose::Vec3 rootPrev2{};
            int roots = 0;
            std::uint64_t overrideWrites = 0;
            bool hasOverride = false;
            // This tick's engine displacement (for the pair check).
            bool hasDisplacement = false;
            pose::Vec3 displacement{};
            std::uint32_t mount = 0;
            std::uint32_t causes = 0;
            float frameEndRefuseM = 0.0f; // the observation's, for the pair check
        };
        void Record(const Finding& finding, std::vector<Finding>& out);
        void RecordPair(const PairGroupInput& worst, std::uint32_t riders, std::vector<Finding>& out);

        Thresholds m_limits;
        std::unordered_map<std::uint32_t, State> m_states;
        std::unordered_map<std::uint32_t, PuppetSummary> m_puppets;
        Summary m_summary;
        TickInfo m_tick;
        std::uint32_t m_tickSerial = 0;
        bool m_hasLast = false;
        std::uint32_t m_lastPlayheadMs = 0;
        double m_lastWallMs = 0.0;
        std::vector<PairGroupInput> m_pairScratch; // EndTick's, kept to avoid a per-tick allocation
    };
}
