// pose_capture_override.cpp: the pose override's public side (arm, publish, apply, check, disarm)
// and the finalizer hook's life (acquire, remove, warm up, service, shutdown).
// Split from pose_capture.cpp (moved code); shared internals in pose_capture_internal.h.

#include "pose_capture_internal.h"

namespace replay::pose::capture
{

    bool ArmOverride(
        std::uint32_t slotIndex,
        void* skeleton,
        std::uint32_t boneCount,
        std::string& error)
    {
        if (slotIndex >= kMaxOverrides || !skeleton || boneCount == 0 || boneCount > 4096)
        {
            error = "invalid_override";
            return false;
        }
        // Through the skeleton hints (script thread, their writer), then the
        // slot; only the capture slots in use.
        const std::uint32_t captureEnd = g_armedScanEnd.load(std::memory_order_acquire);
        for (std::uint32_t index = 0; index < captureEnd && index < kLiveTargetSlots; ++index)
        {
            if (g_armedRecordSkeleton[index].load(std::memory_order_acquire) != skeleton)
                continue;
            const Record* record = g_armed[index].load(std::memory_order_acquire);
            if (record && record->skeleton == skeleton)
            {
                error = "skeleton_is_capture_target";
                return false;
            }
        }
        if (!EnsureHook(error))
            return false;
        // The override writes where the finalizer writes: the pose buffer
        // [skeleton] (locals, then objects) of [skeleton+0x108] bones, both
        // read by the finalizer's own code.
        static const bool layout = sig::Find(tpose::kFinalizerLocals) && sig::Find(tpose::kFinalizerBones);
        if (!layout)
        {
            error = "pose_layout_signature_mismatch";
            return false;
        }

        Override& slot = g_overrides[slotIndex];
        DisarmOverride(slotIndex);
        AcquireSRWLockExclusive(&slot.lock);
        slot.boneCount = boneCount;
        slot.poseLocals.assign(boneCount, BoneQts{});
        slot.poseObjects.assign(boneCount, BoneQts{});
        slot.hasData = false;
        slot.altBoneCount = 0;
        slot.altPose.clear();
        slot.altHasData = false;
        slot.poseHashValid = false;
        ReleaseSRWLockExclusive(&slot.lock);
        slot.applies.store(0);
        slot.altApplies.store(0);
        slot.boneCountMismatch.store(0);
        slot.writeFaults.store(0);
        slot.publishes.store(0);
        slot.hasWrite.store(false);
        slot.late.store(false);
        SetLateBit(slotIndex, false);
        slot.latePending.store(false);
        slot.preRenderSeen.store(false);
        slot.lateWrites.store(0);
        if (slot.rebuildGuard.exchange(false))
            g_guardedSlots.fetch_sub(1, std::memory_order_acq_rel);
        slot.guardWritesThisTick.store(0);
        slot.guardApplies.store(0);
        slot.prefixBones.store(0);
        slot.prefixApplies.store(0);
        slot.skippedOriginal.store(0);
        slot.preRenderWrites.store(0);  // not kept from the slot's last arm
        slot.weaponPassWrites.store(0);
        slot.staleApplies.store(0);
        slot.scriptWrites = 0;
        slot.scriptSurvived = 0;
        slot.scriptOverwritten = 0;
        slot.appliesAtPublish = 0;
        slot.writeChecks = 0;
        slot.writeChanged = 0;
        // The lookups' scan end covers the slot before it is published.
        g_overrideScanEnd.store(replay::capacity::RaisedScanEnd(g_overrideScanEnd.load(std::memory_order_relaxed), slotIndex),
            std::memory_order_seq_cst);
        slot.armed.store(static_cast<std::uint8_t*>(skeleton), std::memory_order_release);
        g_armedSkeleton[slotIndex].store(static_cast<std::uint8_t*>(skeleton), std::memory_order_release);
        return true;
    }

    namespace
    {

        // PublishOverride and PublishOverrideSample share this; `store` puts the
        // pose into the slot vectors (copy or swap).
        template <typename Store>
        bool PublishPose(std::uint32_t slotIndex, const BoneQts* locals, const BoneQts* objects,
            std::uint32_t boneCount, Store&& store)
        {
            Override& slot = g_overrides[slotIndex];
            // The hash of what every write of this pose puts in the
            // buffer, taken once here and outside the lock; only the late
            // (vehicle) slots use it on the detour side, the others hash on
            // demand.
            const bool late = slot.late.load(std::memory_order_acquire);
            std::uint64_t hash = 0;
            if (late)
            {
                PoseHasher hasher;
                hasher.Add(locals, sizeof(BoneQts) * boneCount);
                hasher.Add(objects, sizeof(BoneQts) * boneCount);
                hash = hasher.Finish();
            }
            bool published = false;
            AcquireSRWLockExclusive(&slot.lock);
            if (slot.armed.load(std::memory_order_acquire) && boneCount == slot.boneCount)
            {
                store(slot);
                slot.hasData = true;
                slot.poseHashValid = late;
                slot.poseHash = hash;
                // The detour writes under the shared lock, so every apply
                // counted after this one used the pose just stored.
                slot.appliesAtPublish = slot.applies.load(std::memory_order_acquire);
                slot.guardWritesThisTick.store(0, std::memory_order_relaxed); // once per tick
                published = true;
            }
            ReleaseSRWLockExclusive(&slot.lock);
            if (published)
            {
                slot.publishes.fetch_add(1, std::memory_order_relaxed);
                if (late)
                    slot.latePending.store(true, std::memory_order_release); // one late write per publication
            }
            return published;
        }
    }

    bool PublishOverride(
        std::uint32_t slotIndex,
        const BoneQts* locals,
        const BoneQts* objects,
        std::uint32_t boneCount)
    {
        if (slotIndex >= kMaxOverrides || !locals || !objects)
            return false;
        return PublishPose(slotIndex, locals, objects, boneCount, [&](Override& slot) {
            std::memcpy(slot.poseLocals.data(), locals, sizeof(BoneQts) * boneCount);
            std::memcpy(slot.poseObjects.data(), objects, sizeof(BoneQts) * boneCount);
        });
    }

    bool PublishOverrideSample(std::uint32_t slotIndex, PoseSample& sample, std::uint32_t boneCount)
    {
        if (slotIndex >= kMaxOverrides || sample.locals.size() < boneCount || sample.objects.size() < boneCount)
            return false;
        if (sample.locals.size() != boneCount || sample.objects.size() != boneCount)
            return PublishOverride(slotIndex, sample.locals.data(), sample.objects.data(), boneCount);
        return PublishPose(slotIndex, sample.locals.data(), sample.objects.data(), boneCount, [&](Override& slot) {
            // The slot's vectors hold boneCount entries (ArmOverride), as the
            // sample's: the detours see one whole pose or the other.
            slot.poseLocals.swap(sample.locals);
            slot.poseObjects.swap(sample.objects);
        });
    }

    bool PublishOverrideAlternate(
        std::uint32_t slotIndex,
        const BoneQts* locals,
        const BoneQts* objects,
        std::uint32_t boneCount)
    {
        if (slotIndex >= kMaxOverrides || !locals || !objects || boneCount == 0 || boneCount > 4096)
            return false;
        Override& slot = g_overrides[slotIndex];
        bool published = false;
        AcquireSRWLockExclusive(&slot.lock);
        if (slot.armed.load(std::memory_order_acquire) && boneCount != slot.boneCount)
        {
            if (slot.altBoneCount != boneCount)
            {
                slot.altBoneCount = boneCount;
                slot.altPose.assign(static_cast<std::size_t>(boneCount) * 2u, BoneQts{});
            }
            std::memcpy(slot.altPose.data(), locals, sizeof(BoneQts) * boneCount);
            std::memcpy(slot.altPose.data() + boneCount, objects, sizeof(BoneQts) * boneCount);
            slot.altHasData = true;
            published = true;
        }
        ReleaseSRWLockExclusive(&slot.lock);
        return published;
    }

    void ClearOverrideAlternate(std::uint32_t slotIndex)
    {
        if (slotIndex >= kMaxOverrides)
            return;
        Override& slot = g_overrides[slotIndex];
        AcquireSRWLockExclusive(&slot.lock);
        slot.altHasData = false;
        ReleaseSRWLockExclusive(&slot.lock);
    }

    void ApplyOverrideNow(std::uint32_t slotIndex)
    {
        if (slotIndex >= kMaxOverrides)
            return;
        Override& slot = g_overrides[slotIndex];
        std::uint8_t* skeleton = slot.armed.load(std::memory_order_acquire);
        if (!skeleton)
            return;
        const std::size_t bytes = static_cast<std::size_t>(slot.boneCount) * 2u * sizeof(BoneQts);
        std::uint64_t hash = 0;
        if (slot.hasWrite.load(std::memory_order_acquire) && GuardedPoseHash(skeleton, bytes, hash))
        {
            if (hash == slot.lastWriteHash.load(std::memory_order_acquire))
                ++slot.scriptSurvived;
            else
                ++slot.scriptOverwritten;
        }
        // The written pose's hash from the publication, not a second
        // read of the buffer.
        if (ApplyOverrideFromDetour(slot, skeleton, &hash))
        {
            slot.lastWriteHash.store(hash, std::memory_order_release);
            slot.hasWrite.store(true, std::memory_order_release);
            ++slot.scriptWrites;
        }
    }

    // The weapon system's per-frame pass writes the weapon object's own bones
    // (the local of the turning part, then the object matrices of the
    // component bone pairs) after the script tick; the
    // slot's pose is written again right after that pass, on the thread that
    // runs it (see WeaponPassDetour in weapon_observer.cpp). No natives, the
    // slot checked under its lock (ApplyOverrideFromDetour).
    bool ApplyOverrideAfterWeaponPass(std::uint32_t slotIndex)
    {
        if (slotIndex >= kMaxOverrides)
            return false;
        Override& slot = g_overrides[slotIndex];
        std::uint8_t* skeleton = slot.armed.load(std::memory_order_acquire);
        if (!skeleton)
            return false;
        std::uint64_t hash = 0;
        if (!ApplyOverrideFromDetour(slot, skeleton, &hash))
            return false;
        slot.lastWriteHash.store(hash, std::memory_order_release);
        slot.hasWrite.store(true, std::memory_order_release);
        slot.weaponPassWrites.fetch_add(1, std::memory_order_relaxed);
        // The finalizers' late writes then only follow a publication.
        slot.preRenderSeen.store(true, std::memory_order_release);
        return true;
    }

    std::uint64_t OverrideApplyCount(std::uint32_t slotIndex)
    {
        if (slotIndex >= kMaxOverrides)
            return 0;
        const Override& slot = g_overrides[slotIndex];
        return slot.applies.load(std::memory_order_acquire) + slot.altApplies.load(std::memory_order_acquire);
    }

    bool ApplyOverrideIfStale(std::uint32_t slotIndex, std::uint64_t appliesAtPublish)
    {
        if (slotIndex >= kMaxOverrides)
            return false;
        Override& slot = g_overrides[slotIndex];
        std::uint8_t* skeleton = slot.armed.load(std::memory_order_acquire);
        if (!skeleton || OverrideApplyCount(slotIndex) != appliesAtPublish)
            return false;
        std::uint64_t hash = 0;
        if (!ApplyOverrideFromDetour(slot, skeleton, &hash)) // armed checked again under the lock
            return false;
        slot.lastWriteHash.store(hash, std::memory_order_release);
        slot.hasWrite.store(true, std::memory_order_release);
        slot.staleApplies.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    OverrideWriteCheck CheckOverrideWrite(std::uint32_t slotIndex)
    {
        OverrideWriteCheck result;
        if (slotIndex >= kMaxOverrides)
            return result;
        Override& slot = g_overrides[slotIndex];
        std::uint8_t* skeleton = slot.armed.load(std::memory_order_acquire);
        if (!skeleton)
            return result;
        static std::vector<BoneQts> now; // the buffer as the frame left it
        AcquireSRWLockShared(&slot.lock);
        const std::uint64_t before = slot.applies.load(std::memory_order_acquire);
        if (slot.hasData && before > slot.appliesAtPublish)
        {
            std::uint8_t* poseBuffer = nullptr;
            std::uint8_t* world = nullptr;
            std::uint32_t liveBones = 0;
            now.resize(static_cast<std::size_t>(slot.boneCount) * 2u);
            const bool ok = GuardedReadSkeleton(skeleton, poseBuffer, world, liveBones) && poseBuffer &&
                liveBones == slot.boneCount &&
                GuardedCopy(now.data(), poseBuffer, now.size() * sizeof(BoneQts)) &&
                slot.applies.load(std::memory_order_acquire) == before;
            if (ok)
            {
                result.checked = true;
                result.boneCount = slot.boneCount;
                // The published pose in one array for the compare (every few seconds
                // per puppet, not per tick).
                static std::vector<BoneQts> published;
                published.assign(slot.poseLocals.begin(), slot.poseLocals.end());
                published.insert(published.end(), slot.poseObjects.begin(), slot.poseObjects.end());
                result.change = CompareWrittenPose(published.data(), now.data(), slot.boneCount,
                    kLateTranslation, kLateRotation);
                ++slot.writeChecks;
                if (result.change.localBones + result.change.objectBones > 0)
                    ++slot.writeChanged;
            }
        }
        ReleaseSRWLockShared(&slot.lock);
        return result;
    }

    // The drawn check of a script-written slot (an attached object the
    // finalizer never visits). Script thread, at the start of the tick,
    // before the publish: the buffer then holds what the last frame drew
    // (hypothesis: the update ends before the next script tick) and the slot
    // still the pose written last tick. Read only; counted in
    // writeChecks/writeChanged by NoteOverrideDrawn, for
    // POSE_V2_PLAYBACK_DELETE.
    bool ReadOverrideDrawn(std::uint32_t slotIndex, std::vector<BoneQts>& published, std::vector<BoneQts>& drawn)
    {
        published.clear();
        drawn.clear();
        if (slotIndex >= kMaxOverrides)
            return false;
        Override& slot = g_overrides[slotIndex];
        std::uint8_t* skeleton = slot.armed.load(std::memory_order_acquire);
        if (!skeleton || !slot.hasWrite.load(std::memory_order_acquire))
            return false;
        bool ok = false;
        AcquireSRWLockShared(&slot.lock);
        if (slot.hasData && slot.boneCount && slot.poseLocals.size() == slot.boneCount)
        {
            // Locals then object matrices, as the engine buffer holds them
            // (only the locals when the slot has no objects published).
            const bool objects = slot.poseObjects.size() == slot.boneCount;
            std::uint8_t* poseBuffer = nullptr;
            std::uint8_t* world = nullptr;
            std::uint32_t liveBones = 0;
            drawn.resize(static_cast<std::size_t>(slot.boneCount) * (objects ? 2u : 1u));
            ok = GuardedReadSkeleton(skeleton, poseBuffer, world, liveBones) && poseBuffer &&
                liveBones == slot.boneCount &&
                GuardedCopy(drawn.data(), poseBuffer, drawn.size() * sizeof(BoneQts));
            if (ok)
            {
                published.assign(slot.poseLocals.begin(), slot.poseLocals.end());
                if (objects)
                    published.insert(published.end(), slot.poseObjects.begin(), slot.poseObjects.end());
            }
        }
        ReleaseSRWLockShared(&slot.lock);
        if (!ok)
            drawn.clear();
        return ok;
    }

    void NoteOverrideDrawn(std::uint32_t slotIndex, bool changed)
    {
        if (slotIndex >= kMaxOverrides)
            return;
        Override& slot = g_overrides[slotIndex];
        // Script thread only (CheckOverrideWrite is the other writer, also
        // the script thread), like the script* counters.
        ++slot.writeChecks;
        if (changed)
            ++slot.writeChanged;
    }

    void SetOverrideRebuildGuard(std::uint32_t slotIndex, bool on)
    {
        if (slotIndex >= kMaxOverrides)
            return;
        Override& slot = g_overrides[slotIndex];
        if (slot.rebuildGuard.exchange(on, std::memory_order_acq_rel) != on)
        {
            if (on)
                g_guardedSlots.fetch_add(1, std::memory_order_acq_rel);
            else
                g_guardedSlots.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    void SetOverridePrefixBones(std::uint32_t slotIndex, std::uint32_t bodyBones)
    {
        if (slotIndex >= kMaxOverrides)
            return;
        g_overrides[slotIndex].prefixBones.store(bodyBones, std::memory_order_release);
    }

    void SetOverrideLate(std::uint32_t slotIndex, bool late)
    {
        if (slotIndex >= kMaxOverrides)
            return;
        g_overrides[slotIndex].late.store(late, std::memory_order_release);
        SetLateBit(slotIndex, late);
    }

    void DisarmOverride(std::uint32_t slotIndex)
    {
        if (slotIndex >= kMaxOverrides)
            return;
        Override& slot = g_overrides[slotIndex];
        g_armedSkeleton[slotIndex].store(nullptr, std::memory_order_release);
        slot.armed.store(nullptr, std::memory_order_release);
        slot.late.store(false, std::memory_order_release);
        SetLateBit(slotIndex, false);
        slot.latePending.store(false, std::memory_order_release);
        slot.prefixBones.store(0, std::memory_order_release);
        if (slot.rebuildGuard.exchange(false, std::memory_order_acq_rel))
            g_guardedSlots.fetch_sub(1, std::memory_order_acq_rel);
        // Wait out any detour still inside the shared section.
        AcquireSRWLockExclusive(&slot.lock);
        slot.hasData = false;
        slot.altHasData = false;
        ReleaseSRWLockExclusive(&slot.lock);
    }

    void DisarmOverrides()
    {
        for (std::uint32_t slot = 0; slot < kMaxOverrides; ++slot)
            DisarmOverride(slot);
        // Every slot is free (each disarm waited out the detours inside its
        // slot), the lookups scan nothing until the next ArmOverride raises it.
        g_overrideScanEnd.store(0, std::memory_order_seq_cst);
    }

    OverrideStats OverrideSnapshot(std::uint32_t slotIndex)
    {
        OverrideStats stats;
        if (slotIndex >= kMaxOverrides)
            return stats;
        const Override& slot = g_overrides[slotIndex];
        stats.applies = slot.applies.load();
        stats.altApplies = slot.altApplies.load();
        stats.boneCountMismatch = slot.boneCountMismatch.load();
        stats.writeFaults = slot.writeFaults.load();
        stats.publishes = slot.publishes.load();
        stats.scriptWrites = slot.scriptWrites;
        stats.scriptSurvived = slot.scriptSurvived;
        stats.scriptOverwritten = slot.scriptOverwritten;
        stats.lateWrites = slot.lateWrites.load();
        stats.preRenderWrites = slot.preRenderWrites.load();
        stats.skippedOriginal = slot.skippedOriginal.load();
        stats.writeChecks = slot.writeChecks;
        stats.writeChanged = slot.writeChanged;
        stats.guardApplies = slot.guardApplies.load();
        stats.prefixApplies = slot.prefixApplies.load();
        stats.staleApplies = slot.staleApplies.load();
        stats.weaponPassWrites = slot.weaponPassWrites.load();
        return stats;
    }

    bool AcquireFinalizer(std::string& error)
    {
        return TakeFinalizer(error);
    }

    namespace
    {

        // Removes the detour once nothing is armed: new calls go to the
        // original as soon as the jump is gone, calls already inside finish
        // on the trampoline, which is freed only after they drained.
        bool RemoveFinalizerHook()
        {
            if (g_armedCount.load(std::memory_order_acquire) != 0)
                return false;
            for (const Override& slot : g_overrides)
            {
                if (slot.armed.load(std::memory_order_acquire))
                    return false;
            }
            if (g_hookInstalled && g_hookTarget)
            {
                MH_DisableHook(g_hookTarget);
                // The whole detour, not only its copy, counts as in flight (a call
                // inside the trampoline or an override return must be waited for).
                // Twice around the grace for a call that jumped in just before the
                // jump went away; still busy: the hook goes back on and the removal
                // waits for the next tick. The count is the sum of the stripes,
                // which is enough here for the reason given in module_lifetime.h
                // (StripedCounter::Sum).
                const ULONGLONG deadline = GetTickCount64() + 500;
                bool drained = false;
                for (int pass = 0; pass < 2; ++pass)
                {
                    while (g_finalizerCalls.Sum() != 0 && GetTickCount64() < deadline)
                        Sleep(1);
                    drained = g_finalizerCalls.Sum() == 0;
                    if (!drained)
                        break;
                    if (pass == 0)
                        Sleep(20);
                }
                if (!drained)
                {
                    MH_EnableHook(g_hookTarget);
                    return false;
                }
                MH_RemoveHook(g_hookTarget);
                g_hookInstalled = false;
                g_hookTarget = nullptr;
                g_original.store(nullptr, std::memory_order_release);
            }
            return true;
        }
    }

    bool WarmUpFinalizer(std::string& error)
    {
        // The load is where the pose buffer reserve starts filling, off the
        // script thread, before the first recording's first scan.
        StartReserveFill();
        return EnsureHook(error);
    }

    bool WarmUpVehiclePreRender(std::string& error)
    {
        if (g_vehicleHookTarget)
            return true;
        auto* target = reinterpret_cast<std::uint8_t*>(sig::Find(kVehiclePreRender));
        if (!target)
        {
            error = "vehicle_prerender_signature_mismatch";
            return false;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&VehiclePreRenderDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            return false;
        }
        g_vehicleOriginal.store(reinterpret_cast<VehiclePreRenderFn>(trampoline), std::memory_order_release);
        status = replay::hooks::Enable(target);
        if (status != MH_OK)
        {
            MH_RemoveHook(target);
            g_vehicleOriginal.store(nullptr, std::memory_order_release);
            error = std::string("mh_enable_") + MH_StatusToString(status);
            return false;
        }
        g_vehicleHookTarget = target;
        return true;
    }

    void ReleaseFinalizer()
    {
        // The detour and the slot stay; ServiceFinalizer gives them to the
        // Pose Editor when it asks.
    }

    FinalizerEvent ServiceFinalizer(bool busy, std::string& error)
    {
        if (g_ownerHeld)
        {
            if (busy || g_owner.Wanted() != rdr2::finalizer_owner::kPoseEditor)
                return FinalizerEvent::None;
            if (!RemoveFinalizerHook())
                return FinalizerEvent::None;
            g_owner.Release(rdr2::finalizer_owner::kReplayPoC);
            g_ownerHeld = false;
            return FinalizerEvent::Yielded;
        }
        // Claim it back once the Pose Editor let it go and nobody waits.
        if (!g_owner.Open() || g_owner.Holder() != rdr2::finalizer_owner::kFree ||
            g_owner.Wanted() != rdr2::finalizer_owner::kFree)
        {
            return FinalizerEvent::None;
        }
        if (!EnsureHook(error))
            return FinalizerEvent::Failed;
        return FinalizerEvent::Reclaimed;
    }

    bool FinalizerWanted()
    {
        return g_owner.Open() && g_owner.Wanted() == rdr2::finalizer_owner::kPoseEditor;
    }

    void Shutdown()
    {
        DisarmAllRecords();
        DisarmOverrides();
        if (g_hookInstalled && g_hookTarget)
        {
            MH_DisableHook(g_hookTarget);
            MH_RemoveHook(g_hookTarget);
            g_hookInstalled = false;
        }
        if (g_vehicleHookTarget)
        {
            MH_DisableHook(g_vehicleHookTarget);
            MH_RemoveHook(g_vehicleHookTarget);
            g_vehicleHookTarget = nullptr;
        }
        StopForUnload(g_worker, g_workerRunning, g_wake, g_workerExited, "pose_worker");
        StopForUnload(g_discoveryThread, g_discoveryRunning, g_discoveryWake, g_discoveryExited, "pose_discovery");
        // The reserve's load-time fill, if it still runs (it stops between two
        // buffers).
        StopForUnload(g_reserveThread, g_reserveRunning, nullptr, g_reserveExited, "pose_reserve");
        g_original.store(nullptr, std::memory_order_release);
        if (g_ownerHeld)
            g_owner.Release(rdr2::finalizer_owner::kReplayPoC);
        g_ownerHeld = false;
        g_owner.Close();
        if (g_wake)
        {
            CloseHandle(g_wake);
            g_wake = nullptr;
        }
        if (g_discoveryWake)
        {
            CloseHandle(g_discoveryWake);
            g_discoveryWake = nullptr;
        }
    }
}
