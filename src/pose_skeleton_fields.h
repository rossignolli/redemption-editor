#pragma once

// The skeleton fields pose_capture.cpp reads (kSkeletonPoseBuffer,
// kSkeletonWorldMatrix and kSkeletonBoneCount there), proven offline and read
// on every capture. The pose fidelity judge (pose_fidelity.cpp) reads the same
// three fields of the same skeletons, so they live here instead of as new
// numbers; pose_capture.cpp keeps its own copy (same values).

#include <cstddef>

namespace replay::pose::skeleton_fields
{
    // Pose buffer pointer: boneCount locals, then boneCount objects (BoneQts).
    constexpr std::size_t kSkeletonPoseBuffer = 0x00;
    // Parent (world) matrix pointer: 16 floats, DecodeWorldMatrix's layout.
    constexpr std::size_t kSkeletonWorldMatrix = 0x08;
    // Bone count (u32).
    constexpr std::size_t kSkeletonBoneCount = 0x108;
}
