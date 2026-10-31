#pragma once

// Transform track of an attached entity without a skeleton (a weapon
// object), in its parent's space.
//
// An attached object is sampled relative to the parent entity, with both
// matrices read on the script thread in the same frame, so the child stays
// consistent with its parent at playback whatever frame the engine updated
// them in. Playback composes the sample with the parent puppet's current
// transform.
//
// Pure data: no engine or native access, unit tested in ReplayCoreTests.

#include "pose_math.h"
#include "record_gate.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay::entity
{
    // child expressed in parent space, and back.
    pose::WorldTransform RelativeTransform(
        const pose::WorldTransform& parent,
        const pose::WorldTransform& child);
    pose::WorldTransform ComposeTransform(
        const pose::WorldTransform& parent,
        const pose::WorldTransform& relative);

    // Attach through a bone of the child (the engine attachment extension's
    // child bone, weapon_observer.h AttachFrame): the engine puts the child's
    // bone at parent bone * offset, so the child's origin sits at
    // parent bone * offset * inverse(child bone). ATTACH_ENTITY_TO_ENTITY has
    // no child bone, so the recording folds it into the offset: the result
    // is the offset of the child's origin from the parent bone.
    // childBoneObject: the child bone in the child's object space (its
    // skeleton's object matrix).
    pose::WorldTransform FoldChildBone(
        const pose::WorldTransform& offset,
        const pose::WorldTransform& childBoneObject);

    // An attach frame against the recording. The frame puts the child at
    // parentBoneObject * frameOffset in the parent's object space (the
    // parent's recorded pose at the playhead); the recording saw it at
    // recordedRelative (the weapon track, also in the parent's frame). A
    // frame that misses by more than a hand's grip is not the attach the
    // original had (a clip recorded before the child bone was read, or an
    // attach kind the frame does not describe): playback keeps the recorded
    // relative transform instead, as the reference editor places an
    // attached object by its recorded matrix.
    struct FrameAgreement
    {
        float angleDeg = 0.0f;
        float distanceM = 0.0f;
    };
    FrameAgreement AttachFrameAgreement(
        const pose::WorldTransform& parentBoneObject,
        const pose::WorldTransform& frameOffset,
        const pose::WorldTransform& recordedRelative);
    // Attach checks of the Klan1 clip (31/10/2026, engine against recorded):
    // weapons in a hand and in holsters within ~1 degree (median) and 1-2 cm,
    // a repeater on its shoulder strap 0.4 degrees but 4.4 cm (the strap
    // update moves it off the bare frame); the crowd torch read without its
    // child bone 84 degrees and 5 cm. The distance bound leaves the straps
    // alone; the angle tells the torch.
    constexpr float kAttachFrameMaxDeg = 10.0f;
    constexpr float kAttachFrameMaxM = 0.10f;
    inline bool AttachFrameAgrees(const FrameAgreement& agreement)
    {
        return agreement.angleDeg <= kAttachFrameMaxDeg && agreement.distanceM <= kAttachFrameMaxM;
    }

    // Seat evidence. A seated puppet rides its recorded pose (the engine seat
    // placement lands the ped at the vehicle for a frame and then holds it off
    // the recorded pose). The error logged is the distance between the child's
    // recorded world position now and where the engine seat would place it: the
    // parent's world now composed with the child-in-parent transform once seated
    // (sampled a little ahead).
    float PlacementErrorM(
        const pose::WorldTransform& parent,
        const pose::WorldTransform& seatRelative,
        const pose::WorldTransform& child);
    // How far ahead of the playhead the seated relation is sampled.
    constexpr std::uint32_t kSeatSteadyAheadMs = 500;

    struct TransformSample
    {
        std::uint32_t timeMs = 0;
        pose::WorldTransform relative;
        bool visible = true;
    };

    class RelativeTrack
    {
    public:
        static constexpr std::size_t kMaxSamples = 60u * 60u * 30u; // 1 h at 30 Hz

        explicit RelativeTrack(std::uint32_t sampleHz = 30);
        // Rate gated: returns false when the sample is too close to the last
        // one, goes back in time or the track is full.
        bool Append(const TransformSample& sample);
        // Interpolated sample at timeMs, clamped to the span. Visibility is
        // the one of the earlier sample. False when empty. With `cuts`
        // (Recording::cuts), two samples on either side of a cut are not
        // blended: the one on the playhead's side is taken whole.
        bool Sample(std::uint32_t timeMs, TransformSample& out, const std::vector<std::uint32_t>* cuts = nullptr) const;
        bool Span(std::uint32_t& firstMs, std::uint32_t& lastMs) const;
        std::size_t Size() const { return m_samples.size(); }
        std::uint64_t Dropped() const { return m_dropped; }
        // Replay buffer: drops the samples before timeMs but the last one of
        // them (the pose in force at timeMs).
        std::size_t DropBefore(std::uint32_t timeMs)
        {
            std::size_t keep = 0;
            while (keep + 1 < m_samples.size() && m_samples[keep + 1].timeMs <= timeMs)
                ++keep;
            if (keep)
                m_samples.erase(m_samples.begin(), m_samples.begin() + static_cast<std::ptrdiff_t>(keep));
            return keep;
        }

    private:
        std::uint32_t m_intervalMs = 33;
        std::vector<TransformSample> m_samples;
        std::uint64_t m_dropped = 0;
    };
}
