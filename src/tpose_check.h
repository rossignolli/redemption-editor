#pragma once

// A body recorded or drawn in its bind pose, the T-pose. A ped the engine keeps
// hidden and still (a mission's spare copy of a character) is never animated:
// the finalizer never runs on it, so the clip holds the skeleton as it was
// built, and the replay would draw it T-posed.
//
// 1. Never animated, from the clip alone (TrackSignature): the pose stream
//    keeps, after a chunk's keyframe, only the channels that moved past the
//    encoder's epsilon (pose_stream.h). A track whose frames after the first
//    carry no pose entry at all (no delta entry, no relative keyframe entry,
//    no second full keyframe) never changed pose over the record. With at
//    least kMinFrames frames over kMinSpanMs it is "never animated". The
//    world matrix may still move (a frozen ped carried by script); it does
//    not count. A still body is not always a T-pose (a corpse whose ragdoll
//    went to sleep before the recording is just as still), so the hide also
//    asks for the bind pose (2).
// 2. The bind pose, from the engine: the pose finalizer 0x266C294 takes the
//    model's own rows where the animation left a lane unwritten. Its loop
//    (build 1491.50):
//      0x266C2BC mov r10,[rcx]         the pose buffer: locals, then objects
//      0x266C2B1 mov r14d,[rcx+0D0h]   groups of the skeleton (rcx)
//      0x266C2E9 mov eax,[rcx+108h]    bones of the skeleton
//      0x266C2B8 lea rsi,[rcx+10h]     one skeleton data pointer per group
//      0x266C334 mov rax,[rsi]         the group's skeleton data
//      0x266C343 mov rbx,[rax+28h]     its bone records
//      0x266C347 movzx edi,word [rax+5Eh]  its bone count
//      0x266C350 movups [rbx], [rbx+10h], [rbx+20h]: rotation, translation
//                and scale of the bone (paired with the rotation, translation
//                and scale lanes of the source, pcmpeqd against all ones)
//      0x266C630 add r13,50h           next bone record (0x50 bytes)
//      0x266C67C add rsi,8             next group; locals advance by its bones
//    ReadBindRotations walks the same chain through a guarded read. A pose
//    compared with it bone by bone (rotation only: translations are the
//    model's proportions) is the bind pose when at most a handful of bones
//    turned past kPosedRad (IsBindPose).
// 3. The T-pose watch (TPoseWatch): what the puppet draws, its skeleton's
//    locals read back from the engine buffer, compared with that bind pose
//    every kCheckEveryMs while it is shown. kConfirmChecks bind checks in a
//    row open an episode (one POSE_V2_TPOSE line, the QA kind "tpose"); a
//    posed check closes it. Every T-pose cause shows there (a track recorded
//    in the bind pose, a rebuilt skeleton nobody writes, a lost override).
// 4. The watch only logs. What the engine leaves in an off screen body's buffer
//    is not what the viewer sees, and a seek or a pause rewrites every pose;
//    so the watch (WatchGate) does not judge while the replay prepares or the
//    screen is not faded in (the clip load wait), for TPoseSettleMs after a
//    pause, a resume, a scrub or a jump of the playhead, nor a body the engine
//    does not draw (TPoseOnScreenOnly, IS_ENTITY_ON_SCREEN). A skipped check
//    restarts the body's run.
//
// Pure: no engine access (the reads go through a callable); unit tested in
// tests/scenery_speech_tests.cpp.

#include "pose_math.h"
#include "pose_stream.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace replay::tpose
{
    // ---- 1. never animated (the clip) ----

    // Frames and span a still track needs before it is called never animated
    // (2 s at the 30 Hz capture rate): a body seen for a moment is not judged.
    constexpr std::uint64_t kMinFrames = 60;
    constexpr std::uint32_t kMinSpanMs = 2000;

    // The frame record header (pose_stream.h): u32 size, u32 time, u16 flags,
    // u16 entries, f32 world[16].
    constexpr std::size_t kFrameFlagsAt = 8;
    constexpr std::size_t kFrameEntriesAt = 10;
    constexpr std::size_t kFrameWorldAt = 12;
    constexpr std::size_t kFrameHeaderBytes = 12 + 16 * sizeof(float);

    struct TrackSignature
    {
        std::uint64_t frames = 0;
        // Frames after the first that carry a pose entry, or a full keyframe
        // after the first (a layout change: counted, the state is not decoded).
        std::uint64_t poseFrames = 0;
        std::uint64_t fullKeyframes = 0;
        std::uint64_t relativeKeyframes = 0;
        // Frames whose world matrix differs from the frame before.
        std::uint64_t worldFrames = 0;
        std::uint32_t firstMs = 0;
        std::uint32_t lastMs = 0;
        std::uint32_t firstPoseMs = 0xFFFFFFFFu; // the first frame with a pose entry (none: 0xFFFFFFFF)
        bool parsed = false; // every frame header was read
    };

    inline TrackSignature ReadSignature(const pose::TrackExport& track)
    {
        TrackSignature s;
        bool first = true;
        float lastWorld[16] = {};
        for (const pose::TrackChunkExport& chunk : track.chunks)
        {
            if (chunk.frameOffsets.size() != chunk.frameTimes.size())
                return s;
            const std::uint8_t* bytes = chunk.bytes.data();
            const std::size_t size = chunk.bytes.size();
            for (std::size_t frame = 0; frame < chunk.frameOffsets.size(); ++frame)
            {
                const std::size_t at = chunk.frameOffsets[frame];
                if (at > size || size - at < kFrameHeaderBytes)
                    return s;
                std::uint16_t flags = 0;
                std::uint16_t entries = 0;
                float world[16];
                std::memcpy(&flags, bytes + at + kFrameFlagsAt, sizeof(flags));
                std::memcpy(&entries, bytes + at + kFrameEntriesAt, sizeof(entries));
                std::memcpy(world, bytes + at + kFrameWorldAt, sizeof(world));
                const std::uint32_t timeMs = chunk.frameTimes[frame];
                const bool keyframe = (flags & pose::kFrameKeyframe) != 0;
                const bool relative = (flags & pose::kFrameRelativeKeyframe) != 0;
                if (keyframe && !relative)
                    ++s.fullKeyframes;
                if (relative)
                    ++s.relativeKeyframes;
                if (first)
                {
                    // The first frame sets the state (a full keyframe, or a
                    // relative one against an anchor older than the clip).
                    s.firstMs = timeMs;
                    first = false;
                }
                else
                {
                    if ((keyframe && !relative) || entries > 0)
                    {
                        ++s.poseFrames;
                        if (s.firstPoseMs == 0xFFFFFFFFu)
                            s.firstPoseMs = timeMs;
                    }
                    if (std::memcmp(world, lastWorld, sizeof(world)) != 0)
                        ++s.worldFrames;
                }
                std::memcpy(lastWorld, world, sizeof(world));
                s.lastMs = timeMs;
                ++s.frames;
            }
        }
        s.parsed = true;
        return s;
    }

    inline std::uint32_t SpanMs(const TrackSignature& s)
    {
        return s.frames && s.lastMs >= s.firstMs ? s.lastMs - s.firstMs : 0;
    }

    inline bool NeverAnimated(const TrackSignature& s)
    {
        return s.parsed && s.frames >= kMinFrames && SpanMs(s) >= kMinSpanMs && s.poseFrames == 0;
    }

    // The verdict text of the clip-side check (logs and the clip tool).
    inline const char* SignatureVerdict(const TrackSignature& s)
    {
        if (!s.parsed)
            return "unreadable";
        if (s.poseFrames > 0)
            return "animated";
        return NeverAnimated(s) ? "never_animated" : "still_short";
    }

    // ---- 2. the bind pose (the engine) ----

    constexpr std::size_t kSkeletonPoseBufferOffset = 0x00; // locals (48 bytes a bone), then objects
    constexpr std::size_t kSkeletonGroupDataOffset = 0x10; // one skeleton data pointer per group
    constexpr std::size_t kSkeletonGroupCountOffset = 0xD0; // u32
    constexpr std::size_t kSkeletonBoneCountOffset = 0x108; // u32
    constexpr std::size_t kDataBonesOffset = 0x28;          // the bone records
    constexpr std::size_t kDataBoneCountOffset = 0x5E;      // u16
    constexpr std::size_t kBoneRecordBytes = 0x50;          // rotation, translation, scale, then the bone's own fields
    // The group pointers end where the group count starts.
    constexpr std::uint32_t kMaxGroups = static_cast<std::uint32_t>((kSkeletonGroupCountOffset - kSkeletonGroupDataOffset) / 8);
    constexpr std::uint32_t kMaxBones = 4096; // pose_stream LayoutUsable

    // The code of the finalizer that proves the layout above (build 1491.50).
    constexpr std::uintptr_t kFinalizerBindLoadRva = 0x266C334;
    constexpr std::uint8_t kFinalizerBindLoadSignature[] = {
        0x48, 0x8B, 0x06,             // mov rax,[rsi]
        0x66, 0x0F, 0x6F, 0xC6,       // movdqa xmm0,xmm6
        0x66, 0x0F, 0x6F, 0xD6,       // movdqa xmm2,xmm6
        0x66, 0x0F, 0x6F, 0xDE,       // movdqa xmm3,xmm6
        0x48, 0x8B, 0x58, 0x28,       // mov rbx,[rax+28h]
        0x0F, 0xB7, 0x78, 0x5E,       // movzx edi,word ptr [rax+5Eh]
    };
    constexpr std::uintptr_t kFinalizerBindStrideRva = 0x266C630;
    constexpr std::uint8_t kFinalizerBindStrideSignature[] = {
        0x49, 0x83, 0xC5, 0x50, // add r13,50h
    };
    constexpr std::uintptr_t kFinalizerGroupStepRva = 0x266C67C;
    constexpr std::uint8_t kFinalizerGroupStepSignature[] = {
        0x48, 0x83, 0xC6, 0x08, // add rsi,8
        0x41, 0x3B, 0xD6,       // cmp edx,r14d
    };
    constexpr std::uintptr_t kFinalizerLocalsRva = 0x266C2BC;
    constexpr std::uint8_t kFinalizerLocalsSignature[] = {
        0x4C, 0x8B, 0x11, // mov r10,[rcx]
    };
    constexpr std::uintptr_t kFinalizerBonesRva = 0x266C2E9;
    constexpr std::uint8_t kFinalizerBonesSignature[] = {
        0x8B, 0x81, 0x08, 0x01, 0x00, 0x00, // mov eax,[rcx+108h]
    };

    // `matches(rva, bytes, size)` -> bool: the code at the module's rva.
    template <typename Matches>
    inline bool BindLayoutProven(Matches&& matches)
    {
        return matches(kFinalizerBindLoadRva, kFinalizerBindLoadSignature, sizeof(kFinalizerBindLoadSignature)) &&
            matches(kFinalizerBindStrideRva, kFinalizerBindStrideSignature, sizeof(kFinalizerBindStrideSignature)) &&
            matches(kFinalizerGroupStepRva, kFinalizerGroupStepSignature, sizeof(kFinalizerGroupStepSignature)) &&
            matches(kFinalizerBonesRva, kFinalizerBonesSignature, sizeof(kFinalizerBonesSignature)) &&
            matches(kFinalizerLocalsRva, kFinalizerLocalsSignature, sizeof(kFinalizerLocalsSignature));
    }

    // The bind rotation of every bone of `skeleton`, in the order of its pose
    // buffer (group after group). read(address, out, size) -> bool: a guarded
    // read. Returns "" or what failed.
    template <typename Read>
    inline const char* ReadBindRotations(Read&& read, std::uintptr_t skeleton, std::vector<pose::Quat>& out)
    {
        out.clear();
        if (!skeleton)
            return "no_skeleton";
        std::uint32_t groups = 0;
        std::uint32_t bones = 0;
        if (!read(skeleton + kSkeletonGroupCountOffset, &groups, sizeof(groups)) ||
            !read(skeleton + kSkeletonBoneCountOffset, &bones, sizeof(bones)))
            return "read_skeleton";
        if (groups == 0 || groups > kMaxGroups)
            return "bad_groups";
        if (bones == 0 || bones > kMaxBones)
            return "bad_bones";
        out.reserve(bones);
        std::vector<std::uint8_t> records;
        for (std::uint32_t group = 0; group < groups; ++group)
        {
            std::uintptr_t data = 0;
            std::uint16_t count = 0;
            std::uintptr_t first = 0;
            if (!read(skeleton + kSkeletonGroupDataOffset + 8u * group, &data, sizeof(data)) || !data)
                return "no_group_data";
            if (!read(data + kDataBoneCountOffset, &count, sizeof(count)) ||
                !read(data + kDataBonesOffset, &first, sizeof(first)) || !first)
                return "read_group";
            if (static_cast<std::size_t>(count) > bones - out.size())
                return "count_mismatch";
            records.resize(static_cast<std::size_t>(count) * kBoneRecordBytes);
            if (count && !read(first, records.data(), records.size()))
                return "read_records";
            for (std::uint32_t bone = 0; bone < count; ++bone)
            {
                pose::Quat q;
                std::memcpy(&q, records.data() + static_cast<std::size_t>(bone) * kBoneRecordBytes, sizeof(q));
                out.push_back(q);
            }
        }
        if (out.size() != bones)
        {
            out.clear();
            return "count_mismatch";
        }
        return "";
    }

    // The whole bind local of every bone (rotation, translation and scale, the
    // three lanes the finalizer loads from the bone record at +0x00, +0x10 and
    // +0x20), in the pose buffer's order. A drawer slides: its rest is a
    // translation as much as a rotation (object_check.h RestOf). Returns "" or
    // what failed.
    template <typename Read>
    inline const char* ReadBindLocals(Read&& read, std::uintptr_t skeleton, std::vector<pose::BoneQts>& out)
    {
        out.clear();
        if (!skeleton)
            return "no_skeleton";
        std::uint32_t groups = 0;
        std::uint32_t bones = 0;
        if (!read(skeleton + kSkeletonGroupCountOffset, &groups, sizeof(groups)) ||
            !read(skeleton + kSkeletonBoneCountOffset, &bones, sizeof(bones)))
            return "read_skeleton";
        if (groups == 0 || groups > kMaxGroups)
            return "bad_groups";
        if (bones == 0 || bones > kMaxBones)
            return "bad_bones";
        out.reserve(bones);
        std::vector<std::uint8_t> records;
        for (std::uint32_t group = 0; group < groups; ++group)
        {
            std::uintptr_t data = 0;
            std::uint16_t count = 0;
            std::uintptr_t first = 0;
            if (!read(skeleton + kSkeletonGroupDataOffset + 8u * group, &data, sizeof(data)) || !data)
                return "no_group_data";
            if (!read(data + kDataBoneCountOffset, &count, sizeof(count)) ||
                !read(data + kDataBonesOffset, &first, sizeof(first)) || !first)
                return "read_group";
            if (static_cast<std::size_t>(count) > bones - out.size())
                return "count_mismatch";
            records.resize(static_cast<std::size_t>(count) * kBoneRecordBytes);
            if (count && !read(first, records.data(), records.size()))
                return "read_records";
            for (std::uint32_t bone = 0; bone < count; ++bone)
            {
                pose::BoneQts local;
                std::memcpy(&local, records.data() + static_cast<std::size_t>(bone) * kBoneRecordBytes, sizeof(local));
                out.push_back(local);
            }
        }
        if (out.size() != bones)
        {
            out.clear();
            return "count_mismatch";
        }
        return "";
    }

    // The locals of `skeleton`'s pose buffer (what the frame draws from: the
    // finalizer's output, or the override's write after it). Returns "" or
    // what failed.
    template <typename Read>
    inline const char* ReadDrawnLocals(Read&& read, std::uintptr_t skeleton, std::vector<pose::BoneQts>& out)
    {
        out.clear();
        std::uintptr_t buffer = 0;
        std::uint32_t bones = 0;
        if (!skeleton || !read(skeleton + kSkeletonPoseBufferOffset, &buffer, sizeof(buffer)) ||
            !read(skeleton + kSkeletonBoneCountOffset, &bones, sizeof(bones)))
            return "read_skeleton";
        if (!buffer || bones == 0 || bones > kMaxBones)
            return "bad_buffer";
        out.resize(bones);
        if (!read(buffer, out.data(), out.size() * sizeof(pose::BoneQts)))
        {
            out.clear();
            return "read_buffer";
        }
        return "";
    }

    // A bone turned this far from its bind rotation is posed (5 degrees: the
    // stream's rotation error is a fraction of a degree, an arm hanging down
    // is some 80 degrees from the T).
    constexpr float kPosedRad = 0.0873f;
    // Bones compared before any verdict.
    constexpr std::uint32_t kMinCompared = 16;
    // The bind pose: at most max(kBindPosedFloor, compared / kBindPosedShare)
    // bones posed. An animated body turns its spine, limbs, hands and face:
    // dozens to hundreds of bones.
    constexpr std::uint32_t kBindPosedFloor = 4;
    constexpr std::uint32_t kBindPosedShare = 20;

    struct BindMatch
    {
        std::uint32_t bones = 0;    // bones of the pose
        std::uint32_t compared = 0; // with a usable rotation on both sides
        std::uint32_t posed = 0;    // turned past kPosedRad
        float maxRad = 0.0f;
        std::uint32_t worstBone = 0;
    };

    // The angle between two rotations (radians, 0 to pi), or -1 when either
    // is not a usable quaternion.
    inline float RotationAngle(const pose::Quat& a, const pose::Quat& b)
    {
        const float na = a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w;
        const float nb = b.x * b.x + b.y * b.y + b.z * b.z + b.w * b.w;
        if (!(na > 0.25f && na < 4.0f) || !(nb > 0.25f && nb < 4.0f))
            return -1.0f;
        float dot = (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) / std::sqrt(na * nb);
        dot = std::fabs(dot);
        dot = dot > 1.0f ? 1.0f : dot;
        return 2.0f * std::acos(dot);
    }

    // `rotationAt(bone)` -> pose::Quat of the pose being judged.
    template <typename RotationAt>
    inline BindMatch CompareWithBind(RotationAt&& rotationAt, std::uint32_t bones, const std::vector<pose::Quat>& bind)
    {
        BindMatch m;
        m.bones = bones;
        const std::uint32_t count = (std::min)(bones, static_cast<std::uint32_t>(bind.size()));
        for (std::uint32_t bone = 0; bone < count; ++bone)
        {
            const float angle = RotationAngle(rotationAt(bone), bind[bone]);
            if (angle < 0.0f)
                continue;
            ++m.compared;
            if (angle > kPosedRad)
                ++m.posed;
            if (angle > m.maxRad)
            {
                m.maxRad = angle;
                m.worstBone = bone;
            }
        }
        return m;
    }

    inline BindMatch CompareLocalsWithBind(const std::vector<pose::BoneQts>& locals, const std::vector<pose::Quat>& bind)
    {
        return CompareWithBind([&](std::uint32_t bone) { return locals[bone].rotation; },
            static_cast<std::uint32_t>(locals.size()), bind);
    }

    inline bool IsBindPose(const BindMatch& m)
    {
        return m.compared >= kMinCompared && m.posed <= (std::max)(kBindPosedFloor, m.compared / kBindPosedShare);
    }

    // The hide asks for much less than the watch. A never animated record holds the
    // skeleton as built, every bone at its bind rotation (posed 0 expected). A still
    // corpse whose ragdoll turned only its physics bones (pelvis, spine, neck, head,
    // limbs: some 20 of 600 to 950) can pass the watch's 5 % (47 of 952), and a body
    // that only lies there must never vanish. Hidden: at most max(2, 0.5 % of the
    // bones) posed.
    constexpr std::uint32_t kHideBindPosedFloor = 2;
    constexpr std::uint32_t kHideBindPosedShare = 200;

    inline bool IsHideBindPose(const BindMatch& m)
    {
        return m.compared >= kMinCompared && m.posed <= (std::max)(kHideBindPosedFloor, m.compared / kHideBindPosedShare);
    }

    // ---- the hide of a never animated body recorded in its bind pose ----

    // HideNeverAnimated (ini, default 1): 0 shows them as recorded; 1 hides a
    // body whose record is never animated and whose recorded pose is the
    // bind pose of its puppet's model; 2 also hides a never animated record
    // when the bind pose cannot be read (the T-pose not proven).
    enum class HideMode : std::uint8_t { Off = 0, BindProven = 1, Signature = 2 };

    enum class HideVerdict : std::uint8_t
    {
        Show = 0,          // animated, or still for too short a time
        HideBind,          // never animated, recorded in the bind pose
        ShowPosed,         // never animated, but in another pose (a still corpse)
        HideUnproven,      // never animated, bind unknown, HideMode::Signature
        ShowUnproven,      // never animated, bind unknown
        Off,               // HideNeverAnimated=0
    };

    inline const char* HideVerdictText(HideVerdict v)
    {
        switch (v)
        {
        case HideVerdict::Show: return "show";
        case HideVerdict::HideBind: return "hide_bind";
        case HideVerdict::ShowPosed: return "show_posed";
        case HideVerdict::HideUnproven: return "hide_unproven";
        case HideVerdict::ShowUnproven: return "show_unproven";
        case HideVerdict::Off: return "off";
        default: return "?";
        }
    }

    inline bool Hides(HideVerdict v) { return v == HideVerdict::HideBind || v == HideVerdict::HideUnproven; }

    // `bind`: the comparison of the recorded pose with the puppet's bind pose
    // (null: not read). The bind pose here is IsHideBindPose, not IsBindPose.
    inline HideVerdict Decide(HideMode mode, const TrackSignature& signature, const BindMatch* bind)
    {
        if (!NeverAnimated(signature))
            return HideVerdict::Show;
        if (mode == HideMode::Off)
            return HideVerdict::Off;
        if (bind && bind->compared >= kMinCompared)
            return IsHideBindPose(*bind) ? HideVerdict::HideBind : HideVerdict::ShowPosed;
        return mode == HideMode::Signature ? HideVerdict::HideUnproven : HideVerdict::ShowUnproven;
    }

    // ---- 3. the T-pose watch (what the puppet draws) ----

    constexpr std::uint32_t kCheckEveryMs = 1000;
    constexpr std::uint32_t kConfirmChecks = 2;

    struct WatchReport
    {
        std::uint32_t checks = 0; // bind checks in a row that opened the episode
    };

    // ---- 4. when the watch judges (WatchGate) ----

    constexpr std::uint32_t kSettleMsDefault = 1500;
    // A playhead step past wall time x max(rate, 1) x 2 + this is a jump
    // (the slack covers a slow frame).
    constexpr std::uint32_t kJumpSlackMs = 250;

    enum class WatchSkip : std::uint8_t
    {
        None = 0,
        Preparing = 1, // the playhead holds while the puppets are built
        Hidden = 2,    // the screen is not faded in (the clip load wait draws "Loading replay..." over black)
        Settle = 3,    // within the settle window after a transport event
        OffScreen = 4, // the engine does not draw the body
    };
    constexpr std::uint8_t kWatchSkipCount = 5;

    // One per playback; Tick once per frame with the transport state.
    class WatchGate
    {
    public:
        void Configure(std::uint32_t settleMs, bool onScreenOnly)
        {
            m_settleMs = settleMs;
            m_onScreenOnly = onScreenOnly;
        }
        void Reset()
        {
            m_init = false;
            m_until = 0;
            events = 0;
        }
        // The frame's transport; true when a transport event happened now.
        bool Tick(std::uint64_t nowMs, std::uint32_t playheadMs, bool paused, bool scrubbing, bool preparing, bool hidden,
            double rate)
        {
            bool event = !m_init || paused != m_paused || scrubbing || preparing || hidden;
            if (!event && paused)
                event = playheadMs != m_playheadMs; // a seek while paused
            if (!event && !paused)
            {
                const std::uint64_t wall = nowMs > m_nowMs ? nowMs - m_nowMs : 0;
                const double speed = rate > 1.0 ? rate : 1.0;
                const double allowed = static_cast<double>(wall) * speed * 2.0 + kJumpSlackMs;
                event = playheadMs < m_playheadMs || static_cast<double>(playheadMs - m_playheadMs) > allowed;
            }
            m_init = true;
            m_paused = paused;
            m_playheadMs = playheadMs;
            m_nowMs = nowMs;
            m_preparing = preparing;
            m_hidden = hidden;
            if (event)
            {
                ++events;
                m_until = nowMs + m_settleMs;
            }
            return event;
        }
        // Whether a check of a body now counts (`onScreen`: the engine draws it).
        WatchSkip Judge(std::uint64_t nowMs, bool onScreen) const
        {
            if (m_preparing)
                return WatchSkip::Preparing;
            if (m_hidden)
                return WatchSkip::Hidden;
            if (nowMs < m_until)
                return WatchSkip::Settle;
            if (m_onScreenOnly && !onScreen)
                return WatchSkip::OffScreen;
            return WatchSkip::None;
        }
        std::uint32_t SettleMs() const { return m_settleMs; }
        bool OnScreenOnly() const { return m_onScreenOnly; }
        std::uint64_t events = 0; // transport events seen

    private:
        std::uint32_t m_settleMs = kSettleMsDefault;
        bool m_onScreenOnly = true;
        bool m_init = false;
        bool m_paused = false;
        bool m_preparing = false;
        bool m_hidden = false;
        std::uint32_t m_playheadMs = 0;
        std::uint64_t m_nowMs = 0;
        std::uint64_t m_until = 0;
    };

    struct WatchSummary
    {
        std::uint64_t checks = 0;     // drawn poses compared
        std::uint64_t bindChecks = 0; // of those, in the bind pose
        std::uint64_t unread = 0;     // the drawn pose or the bind pose could not be read
        std::uint32_t episodes = 0;   // POSE_V2_TPOSE lines
        std::uint32_t entities = 0;   // distinct bodies with an episode
        std::uint32_t open = 0;       // episodes still open at the end
        std::uint64_t skipped[kWatchSkipCount] = {}; // checks not judged, per WatchSkip
    };

    class TPoseWatch
    {
    public:
        void Reset()
        {
            m_state.clear();
            m_summary = {};
        }

        // One check of a shown body; true when it opens an episode.
        bool Note(std::uint32_t id, bool bind, WatchReport& report)
        {
            ++m_summary.checks;
            State& state = m_state[id];
            if (!bind)
            {
                state.inRow = 0;
                state.open = false;
                return false;
            }
            ++m_summary.bindChecks;
            ++state.inRow;
            if (state.open || state.inRow < kConfirmChecks)
                return false;
            state.open = true;
            ++m_summary.episodes;
            if (!state.seen)
            {
                state.seen = true;
                ++m_summary.entities;
            }
            report.checks = state.inRow;
            return true;
        }

        void NoteUnread() { ++m_summary.unread; }

        // A check not judged (WatchGate): the body's run starts again; an
        // open episode stays open (the same T-pose seen again is not a new one).
        void Hold(std::uint32_t id, WatchSkip why)
        {
            const std::uint8_t n = static_cast<std::uint8_t>(why);
            if (n < kWatchSkipCount)
                ++m_summary.skipped[n];
            const auto found = m_state.find(id);
            if (found != m_state.end())
                found->second.inRow = 0;
        }

        // The body is no longer shown: its run of checks starts again.
        void Forget(std::uint32_t id)
        {
            const auto found = m_state.find(id);
            if (found != m_state.end())
            {
                found->second.inRow = 0;
                found->second.open = false;
            }
        }

        WatchSummary Summary() const
        {
            WatchSummary s = m_summary;
            s.open = 0;
            for (const auto& entry : m_state)
                s.open += entry.second.open ? 1u : 0u;
            return s;
        }

    private:
        struct State
        {
            std::uint32_t inRow = 0;
            bool open = false;
            bool seen = false;
        };
        std::unordered_map<std::uint32_t, State> m_state;
        WatchSummary m_summary;
    };

    // ---- the recorder's side (POSE_V2_CAPTURE_STOP counters) ----

    // The record the finalizer never visited and whose every script copy had
    // the same bytes as the one before: detour_calls=0, captured >= 2 and
    // raw_same_channels == raw_delta_channels > 0 (raw_same_pct=100).
    inline bool RecordedNeverAnimated(std::uint64_t captured, std::uint64_t detourCalls, std::uint64_t rawDeltaChannels,
        std::uint64_t rawSameChannels)
    {
        return captured >= 2 && detourCalls == 0 && rawDeltaChannels > 0 && rawSameChannels == rawDeltaChannels;
    }
}
