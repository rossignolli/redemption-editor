#pragma once

// Pose skeleton resolver for peds, vehicles and objects: walks the engine
// entity to the crSkeleton the pose finalizer writes (fragment path, with the
// owner fallback), SEH guarded and signature checked. Used by the recorder and
// by playback when arming the pose override.

#include "types.h"

#include <cstdint>
#include <string>

namespace replay::skeleton
{
    struct ResolvedPoseSkeleton
    {
        BYTE* entity = nullptr;
        BYTE* fragment = nullptr;
        BYTE* fragmentOwner = nullptr;
        BYTE* skeleton = nullptr;
        BYTE* poseBuffer = nullptr;
        BYTE* worldTransform = nullptr;
        std::uint32_t boneCount = 0;
        bool usedFragmentPath = false;
        // walk::GetterKind of the vtable+0xB8 body (skeleton_walk.h).
        std::uint8_t getterKind = 0;
    };

    // Fast path: no VirtualQuery, the walk itself is guarded.
    bool FastResolvePedPoseSkeleton(Ped ped, ResolvedPoseSkeleton& result);
    // The walk alone, from result.entity (set by the caller): no ScriptHook
    // call, SEH guarded, so a detour can run it.
    bool GuardedPoseSkeletonWalk(ResolvedPoseSkeleton& result);
    // Evidence for an entity whose skeleton walk failed.
    std::string DescribePoseSkeletonWalk(Entity entity);
}
