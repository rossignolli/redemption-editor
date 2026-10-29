#include "skeleton_resolve.h"
#include "entity_cache.h"
#include "skeleton_walk.h"

#include "main.h"
#include "natives.h"

#include <cstdio>
#include <cstring>

namespace replay::skeleton
{
    struct RawBoneTransform
    {
        float value[12];
    };

    // RDR2 metapeds can expose more than 1,024 transforms (the tested player
    // skeleton reports 1,041 through the same +0x108 field used by the native
    // bone lookup). Keep a generous finite ceiling so corrupt pointers still
    // fail before any pose-buffer size arithmetic or traversal.
    constexpr std::uint32_t kMaxResolvedBoneCount = 4096;

    // The pointer walk, guarded by SEH. VirtualQuery is not used: it reports
    // the extent of the whole region the address falls in, which costs
    // milliseconds on the large engine heaps, and the entity registry
    // resolves every recorded entity at the scan cadence.
    // The getter at vtable+0xB8 has three bodies (skeleton_walk.h); the
    // generic one (0x65B6FC, every object vtable) is not called but read,
    // field by field (rung 2): fragment = bit 6 of +0xB0 ? [+0x38] : null,
    // exactly what its 21 bytes do, so an object's walk reaches the fallback.
    bool GuardedPoseSkeletonWalk(ResolvedPoseSkeleton& result)
    {
        __try
        {
            BYTE* vtable = *reinterpret_cast<BYTE**>(result.entity);
            BYTE* fragmentGetter = *reinterpret_cast<BYTE**>(vtable + 0xB8);
            result.getterKind = static_cast<std::uint8_t>(
                walk::ClassifyGetter(fragmentGetter, walk::kGetterProbeBytes));
            switch (static_cast<walk::GetterKind>(result.getterKind))
            {
            case walk::GetterKind::Ped:
            case walk::GetterKind::Vehicle:
            {
                using FragmentGetter = BYTE* (__fastcall*)(BYTE* entity);
                result.fragment =
                    reinterpret_cast<FragmentGetter>(fragmentGetter)(result.entity);
                break;
            }
            case walk::GetterKind::Generic:
            {
                const std::uint32_t flags =
                    *reinterpret_cast<std::uint32_t*>(result.entity + walk::kEntityFlagsOffset);
                result.fragment = walk::GenericIsFragment(flags)
                    ? *reinterpret_cast<BYTE**>(result.entity + walk::kEntityPhysicsInstOffset)
                    : nullptr;
                break;
            }
            default:
                return false;
            }
            if (result.fragment)
            {
                result.fragmentOwner = *reinterpret_cast<BYTE**>(result.fragment + 0x128);
                if (result.fragmentOwner)
                    result.skeleton = *reinterpret_cast<BYTE**>(result.fragmentOwner + 0x160);
                result.usedFragmentPath = result.skeleton != nullptr;
            }
            if (!result.skeleton)
            {
                BYTE* fallbackOwner = *reinterpret_cast<BYTE**>(result.entity + 0xA8);
                if (fallbackOwner)
                    result.skeleton = *reinterpret_cast<BYTE**>(fallbackOwner + 0x40);
            }
            if (!result.skeleton)
                return false;
            result.poseBuffer = *reinterpret_cast<BYTE**>(result.skeleton + 0x00);
            result.worldTransform = *reinterpret_cast<BYTE**>(result.skeleton + 0x08);
            result.boneCount = *reinterpret_cast<std::uint32_t*>(result.skeleton + 0x108);
            if (!result.poseBuffer || result.boneCount == 0 ||
                result.boneCount > kMaxResolvedBoneCount)
            {
                return false;
            }
            // Touch the first and last transform, like the checked resolver.
            const volatile BYTE first = result.poseBuffer[0];
            const volatile BYTE last = result.poseBuffer[
                static_cast<std::size_t>(result.boneCount * 2u) * sizeof(RawBoneTransform) - 1u];
            (void)first;
            (void)last;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool FastResolvePedPoseSkeleton(Ped ped, ResolvedPoseSkeleton& result)
    {
        result = {};
        result.entity = replay::entities::AddressOf(ped); // memory, no ScriptHook call
        if (result.entity && GuardedPoseSkeletonWalk(result))
            return true;
        result = {};
        return false;
    }

    // Evidence for an entity whose skeleton walk failed (skeleton_walk.h):
    // which getter body, every pointer of both paths and whether the
    // model's drawable has skeleton data at all (what CreateSkeleton,
    // 0xE34A18, builds the skeleton from). The generic getter is read like
    // the resolver does; the ped getter is not called here.
    bool GuardedDescribeWalk(BYTE* entity, walk::Evidence& evidence)
    {
        __try
        {
            BYTE* vtable = *reinterpret_cast<BYTE**>(entity);
            BYTE* fragmentGetter = *reinterpret_cast<BYTE**>(vtable + 0xB8);
            std::memcpy(evidence.getter, fragmentGetter, sizeof(evidence.getter));
            evidence.kind = walk::ClassifyGetter(fragmentGetter, walk::kGetterProbeBytes);
            evidence.entityType = *(entity + walk::kEntityTypeOffset);
            BYTE* fragment = nullptr;
            if (evidence.kind == walk::GetterKind::Generic)
            {
                const std::uint32_t flags = *reinterpret_cast<std::uint32_t*>(entity + walk::kEntityFlagsOffset);
                evidence.isFragment = walk::GenericIsFragment(flags) ? 1 : 0;
                fragment = evidence.isFragment
                    ? *reinterpret_cast<BYTE**>(entity + walk::kEntityPhysicsInstOffset)
                    : nullptr;
                evidence.fragment = fragment ? 1 : 0;
            }
            else if (evidence.kind == walk::GetterKind::Vehicle)
            {
                fragment = *reinterpret_cast<BYTE**>(entity + 0x938);
                evidence.fragment = fragment ? 1 : 0;
            }
            BYTE* skeleton = nullptr;
            if (fragment)
            {
                BYTE* cache = *reinterpret_cast<BYTE**>(fragment + 0x128);
                evidence.fragmentCache = cache ? 1 : 0;
                skeleton = cache ? *reinterpret_cast<BYTE**>(cache + 0x160) : nullptr;
                evidence.fragmentSkel = skeleton ? 1 : 0;
            }
            BYTE* owner = *reinterpret_cast<BYTE**>(entity + 0xA8);
            evidence.owner = owner ? 1 : 0;
            BYTE* ownerSkeleton = owner ? *reinterpret_cast<BYTE**>(owner + 0x40) : nullptr;
            evidence.ownerSkel = ownerSkeleton ? 1 : 0;
            if (!skeleton)
                skeleton = ownerSkeleton;
            BYTE* drawHandler = *reinterpret_cast<BYTE**>(entity + walk::kEntityDrawHandlerOffset);
            BYTE* drawable = drawHandler
                ? *reinterpret_cast<BYTE**>(drawHandler + walk::kDrawHandlerDrawableOffset)
                : nullptr;
            evidence.skeletonData = drawable
                ? (*reinterpret_cast<BYTE**>(drawable + walk::kDrawableSkeletonDataOffset) ? 1 : 0)
                : 0;
            if (skeleton)
                evidence.bones = *reinterpret_cast<std::uint32_t*>(skeleton + 0x108);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::string DescribePoseSkeletonWalk(Entity entity)
    {
        BYTE* address = getScriptHandleBaseAddress(entity);
        if (!address)
            return "no_address";
        walk::Evidence evidence;
        if (!GuardedDescribeWalk(address, evidence))
            return "walk_fault|" + walk::Format(evidence);
        return walk::Format(evidence);
    }
}
