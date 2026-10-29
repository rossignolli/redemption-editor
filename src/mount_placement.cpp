// The mod side of the rider follow at the ped pre-render (mount_placement.h): the detour, the table of replay pairs
// the playback publishes every tick, and the guarded engine reads and writes.
// The pure part (decode, encode, the follow plan, the totals) is
// mount_placement_core.cpp.
//
// Pair table protocol (the capture's g_detoursInFlight pattern):
// - Publish (script thread, after the tick placed every puppet) writes the
//   entries, then stores the count (seq_cst).
// - The detour raises g_inFlight, loads the count again (seq_cst) and only
//   then reads an entry; it lowers g_inFlight when done with it.
// - BeginTick and EndPlayback (script thread, before any puppet can be
//   deleted) store a zero count, wait for g_inFlight to reach zero and only
//   then read the records back and let the entries be rewritten. So a
//   detour never holds a puppet address the playback deleted, and never
//   reads an entry being rewritten. A wait that times out (it never should:
//   the pre-render jobs and the script tick are not expected to overlap,
//   see calls_during_tick) skips this tick's records and publish. It also
//   makes BeginTick return false, and the playback tick then deletes no puppet (the lifecycle deletes wait
//   for the next tick); EndPlayback waits up to a second before the stop
//   deletes every puppet.
//
// The table holds every placed puppet under FrameEnd (riders, other peds and horses, vehicles),
// up to kMaxEntries, found by address through an open-addressed index the
// publish builds while the count is zero. Same protocol, same drain.

#include "mount_placement.h"

#include "entity_cache.h"
#include "guarded_memory.h"
#include "hook_batch.h"
#include "module_lifetime.h"
#include "pose_capture.h"
#include "profiler.h"
#include "replay_playback.h"
#include "replay_puppets.h"
#include "skeleton_resolve.h"
#include "skeleton_walk.h"

#include <Windows.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ostream>
#include <vector>

namespace replay::mount
{
    namespace
    {
        // Every placed puppet (the ped pool has 150 places, plus
        // horses are peds, plus the vehicle puppets).
        constexpr std::uint32_t kMaxEntries = 256;
        constexpr std::uint32_t kIndexBits = 9;
        constexpr std::uint32_t kIndexSize = 1u << kIndexBits; // twice the entries: short probes
        // Pose writes at the pre-render per frame (a whole pose copy each).
        // The finalizer runs before the script tick publishes, so at the
        // pre-render nearly every placed puppet's slot still holds the
        // previous publish: the write here is what draws this tick's pose.
        // A cap of 24 left up to half the entries of a busy camp a tick
        // behind their neighbours; 64 covers the scenes measured, at about
        // 14 us a call.
        constexpr std::uint32_t kStaleBudget = 64;
        constexpr ULONGLONG kDrainMs = 20;
        constexpr ULONGLONG kEndDrainMs = 1000; // before the stop deletes every puppet
        const FollowThresholds kLimits{};
        const PlaceThresholds kPlaceLimits{};

        struct Entry
        {
            std::atomic<std::uint64_t> rider{0}; // the puppet's engine address (the lookup key; the rider for a pair)
            PuppetRole role = PuppetRole::Rider;
            int overrideSlot = -1;                // its pose override slot (peds and horses), -1 none
            std::uint64_t appliesAtPublish = 0;   // that slot's write count at the publish
            std::uint64_t mount = 0;              // the mount puppet's CPed
            // Their skeleton parent matrices (0: not resolved).
            // riderParent, riderPacked and riderPlaced are the puppet's own
            // for every role.
            std::uint64_t riderParent = 0;
            std::uint64_t mountParent = 0;
            std::uint32_t riderPacked = 0;
            std::uint32_t mountPacked = 0;
            std::uint32_t playheadMs = 0;
            std::uint64_t publishedAt = 0;        // replay::prof::Now()
            pose::WorldTransform mountPlaced{};
            pose::WorldTransform riderPlaced{};
            RigidOffset offset{};
            // Written by the detour (one pre-render of this rider at a time),
            // read back by the script thread after the drain.
            FrameRecord record{};
            PuppetRecord puppet{}; // the end-of-frame write's record

            // Everything but the key (the publish stores it).
            void Clear()
            {
                role = PuppetRole::Rider;
                overrideSlot = -1;
                appliesAtPublish = 0;
                mount = 0;
                riderParent = 0;
                mountParent = 0;
                riderPacked = 0;
                mountPacked = 0;
                playheadMs = 0;
                publishedAt = 0;
                mountPlaced = {};
                riderPlaced = {};
                offset = {};
                record = FrameRecord{};
                puppet = PuppetRecord{};
            }
        };

        Entry g_entries[kMaxEntries];
        // Address -> entry index + 1 (0 empty), rebuilt by the
        // publish while g_count is zero (the seq_cst count store publishes it).
        std::uint16_t g_index[kIndexSize];
        std::atomic<std::uint32_t> g_count{0};
        std::atomic<bool> g_frameEnd{true};
        std::atomic<std::uint32_t> g_staleThisTick{0};
        // The stale test is made before
        // the original pre-render; these say whether the finalizer then wrote
        // the slot inside it (the test was early) or not (truly stale).
        std::atomic<std::uint64_t> g_staleAtEntry{0};
        std::atomic<std::uint64_t> g_staleWrittenInside{0};
        std::atomic<std::uint64_t> g_staleStillAfter{0};
        std::atomic<std::uint64_t> g_freshAtEntry{0};
        bool g_physicsFilterNote = false;
        std::atomic<std::int32_t> g_inFlight{0};
        std::atomic<bool> g_enabled{false};
        std::atomic<bool> g_inTick{false};
        std::atomic<std::uint64_t> g_callsDuringTick{0};
        std::uint64_t g_pedVtable = 0; // base + kPedVtableRva, set before the hook goes on

        using PreRenderFn = std::uint64_t(__fastcall*)(void*, std::uint64_t, std::uint64_t, std::uint64_t);
        std::atomic<PreRenderFn> g_original{nullptr};
        void* g_target = nullptr;
        const char* g_hookState = "not_installed";

        // Script thread only.
        Totals g_totals;
        PuppetTotals g_puppetTotals;
        std::vector<std::uint32_t> g_carried; // riders the last pre-render moved (packed)
        bool g_publishBlocked = false;        // the last drain timed out

        double Distance(const EngineFrame& a, const EngineFrame& b)
        {
            double pa[3]{};
            double pb[3]{};
            AbsolutePosition(a, pa);
            AbsolutePosition(b, pb);
            const double dx = pa[0] - pb[0];
            const double dy = pa[1] - pb[1];
            const double dz = pa[2] - pb[2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        // One rider's pre-render: read the mount and the rider (guarded),
        // plan the follow, write the rider's two matrices when enabled.
        void Follow(Entry& entry, std::uint8_t* ped)
        {
            FrameRecord& record = entry.record;
            ++record.calls;
            record.rider = entry.riderPacked;
            record.mount = entry.mountPacked;
            record.playheadMs = entry.playheadMs;
            record.readFail = false;
            record.writeFail = false;
            record.wrote = false;
            record.duringTick = g_inTick.load(std::memory_order_relaxed);
            const std::uint64_t now = replay::prof::Now();
            const std::uint64_t perSecond = replay::prof::TicksPerSecond();
            record.ageUs = now >= entry.publishedAt && perSecond
                ? static_cast<std::int64_t>((now - entry.publishedAt) * 1000000ull / perSecond)
                : -1;

            const std::uintptr_t riderMatrix = reinterpret_cast<std::uintptr_t>(ped) + kEntityMatrixOffset;
            const std::uintptr_t mountMatrix = static_cast<std::uintptr_t>(entry.mount) + kEntityMatrixOffset;
            // The parent matrix pointer is the publish's; the chain is walked again from the ped this call
            // received (the same guarded walk and offsets as Publish: the
            // ped fragment getter, fragment +0x128, +0x160, skeleton +0x08;
            // no native) and the parent is read and written only while it is
            // the published one. A skeleton rebuilt since the publish
            // (appearance, ragdoll) leaves the parent alone, counted.
            ParentCheck parent = ParentCheck::None;
            if (entry.riderParent)
            {
                skeleton::ResolvedPoseSkeleton current{};
                current.entity = ped;
                const bool walked = skeleton::GuardedPoseSkeletonWalk(current);
                parent = CheckParent(entry.riderParent, walked, reinterpret_cast<std::uint64_t>(current.worldTransform));
            }
            record.parentStale = parent == ParentCheck::Stale;
            const bool separateParent = parent == ParentCheck::Same && entry.riderParent != riderMatrix;
            std::uint64_t mountVtable = 0;
            float mountEntity[16]{};
            float mountParent[16]{};
            float riderEntity[16]{};
            float riderParent[16]{};
            // The mount must still be a ped (the table is withdrawn before any
            // puppet is deleted; this is the second guard).
            if (!replay::mem::Read(static_cast<std::uintptr_t>(entry.mount), mountVtable) || mountVtable != g_pedVtable ||
                !replay::mem::Copy(mountEntity, mountMatrix, kMatrixBytes) ||
                !replay::mem::Copy(mountParent, entry.mountParent ? static_cast<std::uintptr_t>(entry.mountParent) : mountMatrix,
                    kMatrixBytes) ||
                !replay::mem::Copy(riderEntity, riderMatrix, kMatrixBytes) ||
                (separateParent && !replay::mem::Copy(riderParent, static_cast<std::uintptr_t>(entry.riderParent), kMatrixBytes)))
            {
                record.readFail = true;
                return;
            }
            FollowInput input;
            EngineFrame mountEntityFrame;
            EngineFrame riderParentFrame;
            if (!DecodeEngineMatrix(mountParent, input.mount) || !DecodeEngineMatrix(mountEntity, mountEntityFrame) ||
                !DecodeEngineMatrix(riderEntity, input.rider) ||
                (separateParent && !DecodeEngineMatrix(riderParent, riderParentFrame)))
            {
                record.readFail = true;
                return;
            }
            record.mountEntityParentM = Distance(mountEntityFrame, input.mount);
            input.mountPlaced = entry.mountPlaced;
            input.riderPlaced = entry.riderPlaced;
            input.offset = entry.offset;
            // Under FrameEnd the mount is set back to its own
            // placement at its own pre-render (in an order we do not know),
            // so following the pushed mount would put the jolt back: the
            // rider goes to his placement (PlacePuppet) and this only measures.
            input.enabled = g_enabled.load(std::memory_order_relaxed) && !g_frameEnd.load(std::memory_order_relaxed);
            record.plan = PlanFollow(input, kLimits);
            if (!record.plan.write)
                return;
            // The rider puppet's entity matrix, then its skeleton
            // parent (the physics instance matrix), every w lane kept.
            float out[16]{};
            std::memcpy(out, riderEntity, sizeof(out));
            EncodeKeepingCells(record.plan.right, record.plan.forward, record.plan.up, record.plan.position,
                input.rider.cellX, input.rider.cellY, out);
            if (!replay::mem::Put(riderMatrix, out, kMatrixBytes))
            {
                record.writeFail = true;
                return;
            }
            if (separateParent)
            {
                std::memcpy(out, riderParent, sizeof(out));
                EncodeKeepingCells(record.plan.right, record.plan.forward, record.plan.up, record.plan.position,
                    input.rider.cellX, input.rider.cellY, out);
                if (!replay::mem::Put(static_cast<std::uintptr_t>(entry.riderParent), out, kMatrixBytes))
                {
                    record.writeFail = true;
                    return;
                }
            }
            record.wrote = true;
        }

        std::uint32_t Slot(std::uint64_t address)
        {
            return static_cast<std::uint32_t>(((address >> 4) * 0x9E3779B97F4A7C15ull) >> (64 - kIndexBits));
        }

        // The entry of an engine address, or null (detour side: after the
        // count load, so the index is the publish's).
        Entry* Lookup(std::uint64_t address, std::uint32_t count)
        {
            std::uint32_t at = Slot(address);
            for (std::uint32_t probe = 0; probe < kIndexSize; ++probe, at = (at + 1) & (kIndexSize - 1))
            {
                const std::uint16_t value = g_index[at];
                if (!value)
                    return nullptr;
                if (value <= count && g_entries[value - 1].rider.load(std::memory_order_relaxed) == address)
                    return &g_entries[value - 1];
            }
            return nullptr;
        }

        // Read the entity matrix (and the skeleton parent when it is a
        // separate, still current copy), plan, write both back to this
        // tick's placement. Layout in mount_placement.h.
        void PlaceMatrices(Entry& entry, std::uint8_t* entity, bool walkParent)
        {
            PuppetRecord& record = entry.puppet;
            const std::uintptr_t entityMatrix = reinterpret_cast<std::uintptr_t>(entity) + kEntityMatrixOffset;
            ParentCheck parent = ParentCheck::None;
            if (walkParent && entry.riderParent)
            {
                skeleton::ResolvedPoseSkeleton current{};
                current.entity = entity;
                const bool walked = skeleton::GuardedPoseSkeletonWalk(current);
                parent = CheckParent(entry.riderParent, walked, reinterpret_cast<std::uint64_t>(current.worldTransform));
            }
            record.parentStale = parent == ParentCheck::Stale;
            const bool separateParent = parent == ParentCheck::Same && entry.riderParent != entityMatrix;
            float entityNow[16]{};
            float parentNow[16]{};
            EngineFrame frame;
            EngineFrame parentFrame;
            if (!replay::mem::Copy(entityNow, entityMatrix, kMatrixBytes) || !DecodeEngineMatrix(entityNow, frame) ||
                (separateParent &&
                    (!replay::mem::Copy(parentNow, static_cast<std::uintptr_t>(entry.riderParent), kMatrixBytes) ||
                        !DecodeEngineMatrix(parentNow, parentFrame))))
            {
                record.readFail = true;
                return;
            }
            record.plan = PlanPlace(frame, entry.riderPlaced, true, kPlaceLimits);
            // The physics copy may be off while the entity matrix is not
            // (the instance is what the next physics step and the skeleton
            // read): planned on its own, written with the same target.
            bool parentWrite = false;
            if (separateParent)
            {
                const PlacePlan parentPlan = PlanPlace(parentFrame, entry.riderPlaced, true, kPlaceLimits);
                if (parentPlan.movedM > record.plan.movedM)
                {
                    record.plan.movedM = parentPlan.movedM;
                    record.plan.turnedRad = (std::max)(record.plan.turnedRad, parentPlan.turnedRad);
                }
                parentWrite = parentPlan.write;
                record.plan.refused = record.plan.refused || parentPlan.refused;
            }
            if (record.plan.refused || (!record.plan.write && !parentWrite))
                return;
            float out[16]{};
            std::memcpy(out, entityNow, sizeof(out));
            EncodeKeepingCells(record.plan.right, record.plan.forward, record.plan.up, record.plan.position, frame.cellX,
                frame.cellY, out);
            if (!replay::mem::Put(entityMatrix, out, kMatrixBytes))
            {
                record.writeFail = true;
                return;
            }
            if (separateParent)
            {
                std::memcpy(out, parentNow, sizeof(out));
                EncodeKeepingCells(record.plan.right, record.plan.forward, record.plan.up, record.plan.position,
                    frame.cellX, frame.cellY, out);
                if (!replay::mem::Put(static_cast<std::uintptr_t>(entry.riderParent), out, kMatrixBytes))
                {
                    record.writeFail = true;
                    return;
                }
            }
            record.wrote = true;
        }

        void BeginPuppetRecord(Entry& entry)
        {
            PuppetRecord& record = entry.puppet;
            ++record.calls;
            record.packed = entry.riderPacked;
            record.playheadMs = entry.playheadMs;
            record.role = entry.role;
            record.readFail = false;
            record.writeFail = false;
            record.wrote = false;
            record.parentStale = false;
            record.plan = PlacePlan{};
        }

        // A ped or horse puppet's pre-render, before the
        // original: the published pose when the finalizer skipped it this
        // frame, then the matrices back to the placement.
        void PlacePuppet(Entry& entry, std::uint8_t* ped, int& staleSlot, std::uint64_t& staleCount)
        {
            BeginPuppetRecord(entry);
            PuppetRecord& record = entry.puppet;
            if (entry.overrideSlot >= 0)
            {
                const auto slot = static_cast<std::uint32_t>(entry.overrideSlot);
                if (PoseStale(entry.appliesAtPublish, pose::capture::OverrideApplyCount(slot)))
                {
                    g_staleAtEntry.fetch_add(1, std::memory_order_relaxed);
                    staleSlot = entry.overrideSlot;
                    staleCount = pose::capture::OverrideApplyCount(slot);
                    if (g_staleThisTick.fetch_add(1, std::memory_order_relaxed) < kStaleBudget)
                        record.staleWrote = pose::capture::ApplyOverrideIfStale(slot, entry.appliesAtPublish) || record.staleWrote;
                    else
                        record.staleBudget = true;
                    staleCount = pose::capture::OverrideApplyCount(slot); // after our own write
                }
                else
                    g_freshAtEntry.fetch_add(1, std::memory_order_relaxed);
            }
            PlaceMatrices(entry, ped, true);
        }

        void PreRenderEntry(std::uint8_t* ped, int& staleSlot, std::uint64_t& staleCount)
        {
            g_inFlight.fetch_add(1, std::memory_order_seq_cst);
            const std::uint32_t count = (std::min)(g_count.load(std::memory_order_seq_cst), kMaxEntries);
            Entry* entry = count ? Lookup(reinterpret_cast<std::uint64_t>(ped), count) : nullptr;
            if (entry && entry->role != PuppetRole::Vehicle)
            {
                REPLAY_PROFILE_ZONE("detour.ped_prerender");
                if (entry->role == PuppetRole::Rider)
                    Follow(*entry, ped);
                if (g_frameEnd.load(std::memory_order_relaxed))
                    PlacePuppet(*entry, ped, staleSlot, staleCount);
            }
            g_inFlight.fetch_sub(1, std::memory_order_seq_cst);
        }

        // CPed pre-render (vtable 0x342EB80 slot 0x258), every ped of the
        // pre-render lists, every frame: one count load when no pair is
        // published, the original always.
        std::uint64_t __fastcall PedPreRenderDetour(void* ped, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4)
        {
            replay::lifetime::DetourScope lifetime; // first: the whole call, the trampoline included
            const PreRenderFn original = g_original.load(std::memory_order_acquire);
            int staleSlot = -1;
            std::uint64_t staleCount = 0;
            if (ped && g_count.load(std::memory_order_acquire) != 0)
                PreRenderEntry(static_cast<std::uint8_t*>(ped), staleSlot, staleCount);
            else if (g_inTick.load(std::memory_order_relaxed))
                g_callsDuringTick.fetch_add(1, std::memory_order_relaxed);
            const std::uint64_t result = original ? original(ped, a2, a3, a4) : 0;
            if (staleSlot >= 0) // did the finalizer write it inside the pre-render?
            {
                if (pose::capture::OverrideApplyCount(static_cast<std::uint32_t>(staleSlot)) != staleCount)
                    g_staleWrittenInside.fetch_add(1, std::memory_order_relaxed);
                else
                    g_staleStillAfter.fetch_add(1, std::memory_order_relaxed);
            }
            return result;
        }

        bool Placed(const playback::Puppet& puppet)
        {
            return puppet.state == playback::PuppetState::Ready && puppet.clone && puppet.visibleState == 1 &&
                puppet.hasWorld && puppet.placed;
        }

        // The table withdrawn, the detours drained, the records read back.
        // False when the wait timed out.
        bool Withdraw(std::ostream& log, ULONGLONG timeoutMs)
        {
            const std::uint32_t had = (std::min)(g_count.exchange(0, std::memory_order_seq_cst), kMaxEntries);
            bool waited = false;
            bool timedOut = false;
            if (g_inFlight.load(std::memory_order_seq_cst) != 0)
            {
                waited = true;
                const ULONGLONG deadline = GetTickCount64() + timeoutMs;
                std::uint32_t spins = 0;
                while (g_inFlight.load(std::memory_order_seq_cst) != 0)
                {
                    if (GetTickCount64() >= deadline)
                    {
                        timedOut = true;
                        break;
                    }
                    if (++spins < 64)
                        YieldProcessor();
                    else
                        Sleep(timeoutMs > kDrainMs ? 1 : 0); // the long wait sleeps, as DrainDetours
                }
            }
            g_totals.NoteDrain(waited, timedOut);
            g_totals.NoteCallsDuringTick(g_callsDuringTick.exchange(0, std::memory_order_relaxed));
            g_carried.clear();
            g_publishBlocked = timedOut;
            if (timedOut)
                return false; // a detour may still be on an entry: neither read nor rewritten this tick
            std::string line;
            const bool frameEnd = g_frameEnd.load(std::memory_order_relaxed);
            for (std::uint32_t index = 0; index < had; ++index)
            {
                const Entry& entry = g_entries[index];
                if (entry.role == PuppetRole::Rider)
                {
                    g_totals.Add(entry.record, kLimits, line);
                    if (!line.empty())
                        log << line;
                    if (entry.record.wrote)
                        g_carried.push_back(entry.riderPacked);
                }
                if (!frameEnd)
                    continue;
                g_puppetTotals.Add(entry.puppet, kPlaceLimits, line);
                if (!line.empty())
                    log << line;
            }
            return true;
        }
    }

    void SetEnabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }
    bool Enabled() { return g_enabled.load(std::memory_order_relaxed); }
    // Set at load (WriteLogHeader), before any playback.
    void SetFrameEnd(bool on) { g_frameEnd.store(on, std::memory_order_relaxed); }
    bool FrameEnd() { return g_frameEnd.load(std::memory_order_relaxed); }
    void SetPhysicsFilterNote(bool on) { g_physicsFilterNote = on; }

    void VehiclePreRender(void* vehicle)
    {
        // Called inside the vehicle pre-render detour (its DetourScope is
        // already held): one count load when nothing is published.
        if (!vehicle || g_count.load(std::memory_order_acquire) == 0)
            return;
        g_inFlight.fetch_add(1, std::memory_order_seq_cst);
        const std::uint32_t count = (std::min)(g_count.load(std::memory_order_seq_cst), kMaxEntries);
        Entry* entry = count ? Lookup(reinterpret_cast<std::uint64_t>(vehicle), count) : nullptr;
        if (entry && entry->role == PuppetRole::Vehicle)
        {
            REPLAY_PROFILE_ZONE("detour.vehicle_place");
            BeginPuppetRecord(*entry);
            // The entity matrix only: a vehicle's skeleton parent is its
            // fragment's, whose layout is not proven offline (left alone).
            PlaceMatrices(*entry, static_cast<std::uint8_t*>(vehicle), false);
        }
        g_inFlight.fetch_sub(1, std::memory_order_seq_cst);
    }
    bool Installed() { return g_target != nullptr; }

    bool Install(std::string& error)
    {
        if (g_target)
            return true;
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const std::uintptr_t target = base + kPedPreRenderRva;
        std::uint8_t prologue[sizeof(kPedPreRenderSignature)] = {};
        if (!replay::mem::Copy(prologue, target, sizeof(prologue)) ||
            std::memcmp(prologue, kPedPreRenderSignature, sizeof(prologue)) != 0)
        {
            error = "ped_prerender_signature_mismatch";
            g_hookState = "signature_mismatch";
            return false;
        }
        // The CPed vtable's pre-render slot holds it, and the same vtable's
        // fragment getter is the ped one (the vtable the horse check uses).
        std::uint64_t slot = 0;
        std::uint64_t getter = 0;
        std::uint8_t getterBytes[sizeof(skeleton::walk::kPedGetter)] = {};
        if (!replay::mem::Read(base + kPedVtableRva + kPreRenderSlot, slot) || slot != target)
        {
            error = "ped_vtable_slot_mismatch";
            g_hookState = "vtable_mismatch";
            return false;
        }
        if (!replay::mem::Read(base + kPedVtableRva + kFragmentGetterSlot, getter) ||
            !replay::mem::Copy(getterBytes, static_cast<std::uintptr_t>(getter), sizeof(getterBytes)) ||
            std::memcmp(getterBytes, skeleton::walk::kPedGetter, sizeof(getterBytes)) != 0)
        {
            error = "ped_vtable_getter_mismatch";
            g_hookState = "vtable_mismatch";
            return false;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            g_hookState = "minhook_failed";
            return false;
        }
        void* trampoline = nullptr;
        void* const targetPointer = reinterpret_cast<void*>(target);
        MH_STATUS status = MH_CreateHook(targetPointer, reinterpret_cast<void*>(&PedPreRenderDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            g_hookState = "minhook_failed";
            return false;
        }
        g_pedVtable = base + kPedVtableRva;
        g_original.store(reinterpret_cast<PreRenderFn>(trampoline), std::memory_order_release);
        status = replay::hooks::Enable(targetPointer);
        if (status != MH_OK)
        {
            MH_RemoveHook(targetPointer);
            g_original.store(nullptr, std::memory_order_release);
            error = std::string("mh_enable_") + MH_StatusToString(status);
            g_hookState = "minhook_failed";
            return false;
        }
        g_target = targetPointer;
        g_hookState = "installed";
        return true;
    }

    void Shutdown()
    {
        // Every hook is already off and the detours drained (OnModuleDetach).
        g_count.store(0, std::memory_order_seq_cst);
        if (g_target)
        {
            MH_DisableHook(g_target);
            MH_RemoveHook(g_target);
            g_target = nullptr;
        }
        g_original.store(nullptr, std::memory_order_release);
        g_hookState = "removed";
    }

    bool BeginTick(std::ostream& log)
    {
        const bool drained = Withdraw(log, kDrainMs);
        g_inTick.store(true, std::memory_order_relaxed);
        return drained;
    }

    // The tick ends here, not at Publish.
    void EndTick() { g_inTick.store(false, std::memory_order_relaxed); }

    void Publish(const playback::Puppets& puppets, std::uint32_t playheadMs, bool allowed)
    {
        const bool frameEnd = g_frameEnd.load(std::memory_order_relaxed);
        if (!g_target || g_publishBlocked || (!allowed && !frameEnd))
        {
            g_totals.NoteTick(0);
            if (frameEnd)
                g_puppetTotals.NoteTick(0);
            return;
        }
        // The table is withdrawn (BeginTick) and drained: the entries and
        // the index are ours.
        std::memset(g_index, 0, sizeof(g_index));
        std::uint32_t count = 0;
        std::uint32_t pairs = 0;
        const std::uint64_t now = replay::prof::Now();
        const auto add = [&](std::uint64_t address) -> Entry* {
            if (count >= kMaxEntries || !address)
                return nullptr;
            std::uint32_t at = Slot(address);
            while (g_index[at])
            {
                if (g_entries[g_index[at] - 1].rider.load(std::memory_order_relaxed) == address)
                    return nullptr; // the same engine entity twice (never expected): the first entry stays
                at = (at + 1) & (kIndexSize - 1);
            }
            Entry& entry = g_entries[count];
            g_index[at] = static_cast<std::uint16_t>(++count);
            entry.Clear();
            entry.rider.store(address, std::memory_order_relaxed);
            entry.playheadMs = playheadMs;
            entry.publishedAt = now;
            return &entry;
        };
        // Riders first, as rider and mount pairs (with FrameEnd off, only
        // when `allowed`).
        for (const playback::Puppet& rider : puppets.list)
        {
            if (rider.packet.kind != entity::EntityKind::Ped || !Placed(rider))
                continue;
            const std::uint32_t mountId = playback::MountOf(rider.packet.id.Packed());
            if (!mountId)
                continue;
            const playback::Puppet* mount = puppets.Find(entity::ReplayId::FromPacked(mountId));
            if (!mount || mount->packet.kind != entity::EntityKind::Horse || !Placed(*mount))
                continue;
            const std::uint64_t riderAddress = reinterpret_cast<std::uint64_t>(entities::AddressOf(rider.clone));
            const std::uint64_t mountAddress = reinterpret_cast<std::uint64_t>(entities::AddressOf(mount->clone));
            if (!riderAddress || !mountAddress)
                continue;
            Entry* entry = add(riderAddress);
            if (!entry)
                continue;
            skeleton::ResolvedPoseSkeleton riderSkeleton;
            skeleton::ResolvedPoseSkeleton mountSkeleton;
            const bool riderResolved = skeleton::FastResolvePedPoseSkeleton(rider.clone, riderSkeleton);
            const bool mountResolved = skeleton::FastResolvePedPoseSkeleton(mount->clone, mountSkeleton);
            entry->role = PuppetRole::Rider;
            entry->mount = mountAddress;
            entry->riderParent = riderResolved ? reinterpret_cast<std::uint64_t>(riderSkeleton.worldTransform) : 0;
            entry->mountParent = mountResolved ? reinterpret_cast<std::uint64_t>(mountSkeleton.worldTransform) : 0;
            entry->riderPacked = rider.packet.id.Packed();
            entry->mountPacked = mountId;
            entry->mountPlaced = mount->placedWorld;
            entry->riderPlaced = rider.placedWorld;
            entry->offset = OffsetFromTransforms(mount->placedWorld, rider.placedWorld);
            entry->overrideSlot = rider.overrideSlot;
            ++pairs;
        }
        // Every other placed puppet. An attached one (a seat,
        // a draft harness) is placed by the engine attachment from its
        // parent and stays out.
        if (frameEnd)
        {
            for (const playback::Puppet& puppet : puppets.list)
            {
                const entity::EntityKind kind = puppet.packet.kind;
                if ((kind != entity::EntityKind::Ped && kind != entity::EntityKind::Horse &&
                        kind != entity::EntityKind::Vehicle) ||
                    !Placed(puppet) || puppet.attached)
                    continue;
                const std::uint64_t address = reinterpret_cast<std::uint64_t>(entities::AddressOf(puppet.clone));
                Entry* entry = add(address); // a rider already in the table is refused here
                if (!entry)
                    continue;
                entry->riderPacked = puppet.packet.id.Packed();
                entry->riderPlaced = puppet.placedWorld;
                if (kind == entity::EntityKind::Vehicle)
                {
                    entry->role = PuppetRole::Vehicle;
                    continue;
                }
                entry->role = PuppetRole::Puppet;
                entry->overrideSlot = puppet.overrideSlot;
                skeleton::ResolvedPoseSkeleton resolved;
                if (skeleton::FastResolvePedPoseSkeleton(puppet.clone, resolved))
                    entry->riderParent = reinterpret_cast<std::uint64_t>(resolved.worldTransform);
            }
            // The write counts last, after every entry: the pre-render
            // compares against what the finalizer had done by now.
            for (std::uint32_t index = 0; index < count; ++index)
            {
                Entry& entry = g_entries[index];
                if (entry.overrideSlot >= 0)
                    entry.appliesAtPublish = pose::capture::OverrideApplyCount(static_cast<std::uint32_t>(entry.overrideSlot));
            }
            g_staleThisTick.store(0, std::memory_order_relaxed);
            g_puppetTotals.NoteTick(count);
        }
        g_totals.NoteTick(pairs);
        {
            // A passenger is a pair of its own (the riders loop
            // above takes every rider of a mount); the evidence of how many
            // shared one this tick. The riders are the first `pairs` entries.
            std::uint64_t mounts[kMaxEntries];
            for (std::uint32_t index = 0; index < pairs; ++index)
                mounts[index] = g_entries[index].mount;
            std::uint32_t perMount = 0;
            const std::uint32_t shared = SharedMountRiders(mounts, pairs, perMount);
            g_totals.NoteShared(shared, perMount);
        }
        g_count.store(count, std::memory_order_seq_cst);
    }

    void EndPlayback(std::ostream& log)
    {
        g_inTick.store(false, std::memory_order_relaxed);
        // The stop deletes every puppet next,
        // so the wait is a long one (a timeout is in drain_timeouts).
        Withdraw(log, kEndDrainMs);
        g_publishBlocked = false;
        if (g_totals.Ticks() != 0)
        {
            log << g_totals.Summary(Enabled(), g_hookState);
            log.flush();
        }
        if (g_puppetTotals.Ticks() != 0)
        {
            log << g_puppetTotals.Summary(FrameEnd(), g_physicsFilterNote);
            log.flush();
        }
        // Where the stale test stands against the finalizer.
        log << "MOUNT_PLACE_STALE_TIMING,stale_at_entry=" << g_staleAtEntry.exchange(0)
            << ",written_inside_prerender=" << g_staleWrittenInside.exchange(0)
            << ",still_stale_after=" << g_staleStillAfter.exchange(0)
            << ",fresh_at_entry=" << g_freshAtEntry.exchange(0) << '\n';
        g_totals.Reset();
        g_puppetTotals.Reset();
        g_carried.clear();
    }

    bool CarriedLastFrame(std::uint32_t packedRider)
    {
        return std::find(g_carried.begin(), g_carried.end(), packedRider) != g_carried.end();
    }
}
