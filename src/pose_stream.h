#pragma once

// PoseTrack v2 stream: compact per-frame pose records grouped in chunks.
//
// Each chunk starts with a keyframe, so any chunk can be decoded on its own
// (seek and rewind never scan the whole clip). The first chunk of a layout
// carries a full keyframe (every channel), the anchor; the periodic chunks
// after it start with a relative keyframe: the channels that differ from the
// anchor's decoded state. A chunk keeps a shared copy of its anchor
// record and the decoder applies it before the chunk's own frames.
// Between keyframes a frame only carries the channels that moved more than
// the configured epsilon since the value the decoder already holds, which
// keeps the error bounded and prevents drift. The oldest chunk is dropped
// when the byte budget is exceeded, like a rolling replay buffer.
//
// Frame record (little endian, packed):
//   u32 recordSize, u32 timeMs, u16 flags, u16 entryCount, f32 world[16]
//   entryCount x { u16 channel, u8 mask, payload in mask bit order }
// Mask bits: rot32 (u32), rot64 (u64), trans16 (3 x s16),
//   transFloat (3 x f32), transW (f32), scale (4 x f32).

#include "pose_skeleton_layout.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace replay::pose
{
    enum FrameFlags : std::uint16_t
    {
        kFrameKeyframe = 0x0001,
        // The pose must not be interpolated into this frame (teleport, cut).
        kFrameNoInterpolation = 0x0002,
        // A chunk's first frame, encoded against its anchor keyframe's
        // state instead of zero: the decoder applies the anchor first, then
        // this record as a delta.
        kFrameRelativeKeyframe = 0x0004,
    };

    enum class KeyframeKind : std::uint8_t
    {
        None = 0,     // delta frame
        Full = 1,     // every channel, resets the state (anchor)
        Relative = 2, // delta against the anchor's state
    };

    enum EntryMask : std::uint8_t
    {
        kEntryRot32 = 0x01,
        kEntryRot64 = 0x02,
        kEntryTrans16 = 0x04,
        kEntryTransFloat = 0x08,
        kEntryTransW = 0x10,
        kEntryScale = 0x20,
    };

    struct StreamConfig
    {
        float rotationEpsilon = 0.002f;     // radians
        float translationEpsilon = 0.0005f; // metres
        float translationRange = 4.0f;      // s16 range, metres
        std::uint32_t keyframeIntervalMs = 2000;
        std::size_t byteBudget = 32u * 1024u * 1024u;
    };

    struct FrameInput
    {
        std::uint32_t timeMs = 0;
        std::uint16_t flags = 0;
        const float* world = nullptr;     // 16 floats, stored verbatim
        const BoneQts* locals = nullptr;  // layout.boneCount entries
        const BoneQts* objects = nullptr; // layout.boneCount entries
    };

    struct EncodeStats
    {
        std::uint32_t entries = 0;
        std::uint32_t bytes = 0;
        // In a delta frame, the
        // channels whose input had the same 48 bytes as in the previous
        // frame (skipped: the decision cannot change), of `deltaChannels`;
        // rawSameFrame when that was every channel.
        std::uint32_t deltaChannels = 0;
        std::uint32_t rawSameChannels = 0;
        bool rawSameFrame = false;
    };

    class PoseEncoder
    {
    public:
        void Begin(const SkeletonLayout& layout, const StreamConfig& config);
        // Appends one record to `out`. A full keyframe resets the channel
        // state and becomes the anchor; a relative one restores the anchor's
        // state first and encodes what differs from it (falls back to a
        // full keyframe when there is no anchor).
        EncodeStats Encode(const FrameInput& input, KeyframeKind kind, std::vector<std::uint8_t>& out);
        bool HasAnchor() const { return m_hasAnchor; }
        // Frees the channel state (PoseTrack::Seal); Begin again before the
        // next Encode, which then encodes exactly what a fresh encoder would.
        // StateBytes: the bytes that state holds (capacity); the per-thread
        // scratch of Encode is not counted.
        void Release();
        std::size_t StateBytes() const;
        // The shortcuts (skip a channel whose input bytes did not change;
        // angle before quantization, cheap angle and distance tests), on by
        // default. Off does the full work (PoseCaptureShortcuts=0, and the
        // tests' benchmarks, which time each alone). The output is the same
        // either way.
        static void SetShortcuts(bool rawSkip, bool cheapTests);

    private:
        const BoneQts& Source(const FrameInput& input, std::uint32_t channel) const;

        const SkeletonLayout* m_layout = nullptr;
        StreamConfig m_config;
        // Every channel's input of the previous Encode (valid while
        // m_hasLastInput; Begin clears it). A delta frame skips a channel
        // whose input has the same bytes: with the same input and the state
        // that input left, the encoder never emits. Not after a relative
        // keyframe's restore of the anchor state, which only keyframes do.
        std::vector<BoneQts> m_lastInput;
        bool m_hasLastInput = false;
        AngleTest m_angleTest;
        std::vector<BoneQts> m_emitted;
        // Last packed rotation per channel: re-quantizing an unchanged
        // rotation to the same code must never emit an entry.
        std::vector<std::uint64_t> m_emittedCode;
        // The state right after the anchor keyframe.
        std::vector<BoneQts> m_anchorEmitted;
        std::vector<std::uint64_t> m_anchorCode;
        bool m_hasAnchor = false;
        // The record scratch and the changed-channel list are per-thread
        // buffers in Encode.
    };

    class PoseDecoder
    {
    public:
        void Begin(const SkeletonLayout& layout, const StreamConfig& config);
        // Applies one record. Fails on malformed data or on a delta frame
        // before any keyframe.
        // The record is checked whole first and then decoded in
        // place (no copy of every channel per record); a malformed record
        // still leaves the state untouched. `touched`, when given, gets the
        // channels the record wrote, in record order.
        bool Apply(const std::uint8_t* data, std::size_t size, std::size_t& consumed,
            std::vector<std::uint32_t>* touched = nullptr);

        std::uint32_t TimeMs() const { return m_timeMs; }
        std::uint16_t Flags() const { return m_flags; }
        const float* World() const { return m_world; }
        const std::vector<BoneQts>& Channels() const { return m_channels; }

        // The decoded state alone (no scratch), for the backward
        // scrub checkpoints of PoseTrack. Restoring a saved state gives
        // exactly the decoder that saved it (same layout, after Begin).
        struct State
        {
            std::vector<BoneQts> channels;
            std::uint32_t timeMs = 0;
            std::uint16_t flags = 0;
            float world[16] = {};
            bool hasKeyframe = false;
        };
        void SaveState(State& out) const;
        void RestoreState(const State& state);

    private:
        const SkeletonLayout* m_layout = nullptr;
        StreamConfig m_config;
        std::vector<BoneQts> m_channels;
        std::uint32_t m_timeMs = 0;
        std::uint16_t m_flags = 0;
        float m_world[16] = {};
        bool m_hasKeyframe = false;
    };

    struct PoseSample
    {
        std::vector<BoneQts> locals;
        std::vector<BoneQts> objects;
        float worldA[16] = {};
        float worldB[16] = {};
        float t = 0.0f; // blend from worldA to worldB
        std::uint32_t frameTimeA = 0;
        std::uint32_t frameTimeB = 0;
    };

    // Frame flag decision at record time. A replay editor usually gates
    // interpolation on a per-frame warp flag from the engine; here it is
    // derived from the recorded data: between consecutive
    // frames the entity origin moved while the root bone's world position
    // did not follow (a ragdoll exit or a warp re-bases the entity matrix
    // and the bones compensate, the mesh stays put), or the entity
    // teleported. The later frame is then flagged kFrameNoInterpolation, so
    // playback holds the earlier frame instead of blending a matrix and a
    // pose that only agree at the end points.
    // The heading rebase too. In cover the engine turns the entity
    // matrix about 180 degrees in one frame while the root bone turns back by
    // the same amount (the mesh stays put, the origin barely moves).
    // Blended per bone, the object space turning 180
    // degrees folds the skeleton (bones 2.5 m off at mid-frame). The entity
    // turning this much more than the root's world rotation is a rebase.
    struct RebaseProbe
    {
        WorldTransform world;
        Vec3 root; // root bone (index 0) position in world space
        Quat rootRotation{}; // root bone rotation in world space
        bool hasRootRotation = false;
        std::uint32_t timeMs = 0;
    };
    constexpr float kRebaseOriginExcessM = 0.25f; // origin moved this much more than the root
    constexpr float kTeleportDistanceM = 3.0f;    // origin moved this much in one frame
    constexpr std::uint32_t kRebaseMaxGapMs = 100; // only between close frames
    constexpr float kRebaseRotationExcessRad = 0.785f; // entity turned 45 degrees more than the root
    Vec3 RootWorldPosition(const WorldTransform& world, const BoneQts& rootObject);
    RebaseProbe MakeRebaseProbe(const WorldTransform& world, const BoneQts& rootObject, std::uint32_t timeMs);
    bool IsRebase(const RebaseProbe& previous, const RebaseProbe& current);

    // The pure part of the capture worker (pose_capture.cpp), here so the
    // tests run the same code as the mod.
    //
    // The objects of a locals-only frame, composed with the engine parent
    // table as the finalizer does it (q = qp*ql, t = tp + rot(qp, tl*sp),
    // s = sl*sp; bone 0 is its local scaled by the skeleton's root scale; a
    // bone whose parent is not an earlier bone keeps its local).
    void ComposeEngineObjects(const std::uint16_t* parents, std::uint32_t boneCount, float rootScale,
        const BoneQts* locals, BoneQts* objects);
    // The layout is the one BuildLayoutFromParents makes from this table
    // (ParentThenLocal, inherited scale, each bone's parent the table's
    // earlier bone or object space, bone 0 the root, or in object space when
    // the root scale is not 1), and the root scale is exactly 1 or clearly
    // not (|s - 1| > 1e-6). Then every parented bone's object in a frame
    // composed by ComposeEngineObjects IS the layout's expected object (the
    // same Compose with the same arguments), so FindFailingBones can only
    // come back empty on such a frame and the worker skips it (and the
    // periodic composition checks) for its own frames.
    bool IsExactEngineLayout(const SkeletonLayout& layout, const std::uint16_t* parents, std::uint32_t boneCount,
        float rootScale);
    // The bones the encoder and the rebase probe read from the objects
    // of such a frame: bone 0 (the probe), every object-space channel's
    // bone, and their ancestors in the engine table; ascending (a parent
    // before its children).
    void EngineComposeBones(const SkeletonLayout& layout, const std::uint16_t* parents, std::uint32_t boneCount,
        std::vector<std::uint32_t>& bones);
    // ComposeEngineObjects for those bones only (the same values for them;
    // the other objects are left as they are and must not be read).
    void ComposeEngineObjectsSubset(const std::uint16_t* parents, std::uint32_t boneCount, float rootScale,
        const std::vector<std::uint32_t>& bones, const BoneQts* locals, BoneQts* objects);

    // CompareLatePose (pose_skeleton_layout.h) when the detour
    // also kept the finalizer's objects. Bones whose bytes are the
    // finalizer's are not compared: a local with the same bytes is at
    // distance 0 and angle 0 (float noise at most, far under any tolerance),
    // and an object test whose inputs (own local and object, parent's
    // object) are all the finalizer's is the finalizer's own composition,
    // which matches ours within 1e-7. The result is CompareLatePose's, bit for bit
    // (tests/pose_capture_tests.cpp), for any end-of-frame buffer over a
    // finalizer output that follows that rule. `equalBlocks` tells the
    // caller no byte differed (the common case: one memcmp per block).
    LatePoseChange CompareLatePoseChanged(
        const BoneQts* finalizerLocals,
        const BoneQts* finalizerObjects,
        const BoneQts* lateLocals,
        const BoneQts* lateObjects,
        const std::uint16_t* engineParents,
        std::uint32_t boneCount,
        float rootScale,
        float translationTolerance,
        float rotationTolerance,
        std::vector<BoneQts>& scratch,
        std::vector<std::uint8_t>& changedMask,
        bool& equalBlocks);

    // Pose frames decoded by every PoseTrack since the process started (the
    // SCRUB_COST line takes the difference over a scrub). These counters and
    // SampleWorkCounters count the calling thread's decodes and samples only.
    std::uint64_t DecodedFrameCount();
    // Decodes that started from a scrub checkpoint, all tracks.
    std::uint64_t CheckpointRestoreCount();
    // What the sampler did since the process started, all tracks
    // (script thread): frame pairs prepared for a blend, of those the ones
    // built from A plus one record, the bones of those pairs, the bones
    // their record reached (composed again) and the bones interpolated per
    // pair (locals and objects).
    struct SampleWork
    {
        std::uint64_t pairs = 0;
        std::uint64_t incrementalPairs = 0;
        std::uint64_t pairBones = 0;
        std::uint64_t reachedBones = 0;
        std::uint64_t blendLocals = 0;
        std::uint64_t blendObjects = 0;
        std::uint64_t nextAdvanced = 0; // next frame reached from the frame before A
        std::uint64_t nextCopied = 0;   // next frame from a copy of the cursor
    };
    SampleWork SampleWorkCounters();

    struct TrackStats
    {
        std::uint64_t frames = 0;
        std::uint64_t keyframes = 0;
        // Of the keyframes, those encoded against their anchor, and the
        // bytes of full and relative keyframe records.
        std::uint64_t relativeKeyframes = 0;
        std::uint64_t fullKeyframeBytes = 0;
        std::uint64_t relativeKeyframeBytes = 0;
        std::uint64_t bytes = 0;
        std::uint64_t droppedChunks = 0;
        std::uint32_t firstTimeMs = 0;
        std::uint32_t lastTimeMs = 0;
    };

    // A track's recorded data without its playback caches, for the clip
    // file (clip_file.h): the layouts and anchor records the chunks share,
    // by index, and the chunks as recorded.
    // A chunk's bytes are shared between the track and its exports (save,
    // buffer save, import) and copied only when one side writes, so
    // ExportRecords does not copy every track under g_mutex on the script
    // thread. Reading is const only (data, size, begin, end): a read never
    // copies. Writers say so with Mutable(), which copies the bytes first
    // when someone else holds them.
    class SharedBytes
    {
    public:
        SharedBytes() = default;
        SharedBytes(const std::vector<std::uint8_t>& bytes) : m_bytes(std::make_shared<std::vector<std::uint8_t>>(bytes)) {}
        SharedBytes(std::vector<std::uint8_t>&& bytes)
            : m_bytes(std::make_shared<std::vector<std::uint8_t>>(std::move(bytes)))
        {
        }
        SharedBytes& operator=(const std::vector<std::uint8_t>& bytes)
        {
            m_bytes = std::make_shared<std::vector<std::uint8_t>>(bytes);
            return *this;
        }
        SharedBytes& operator=(std::vector<std::uint8_t>&& bytes)
        {
            m_bytes = std::make_shared<std::vector<std::uint8_t>>(std::move(bytes));
            return *this;
        }

        const std::vector<std::uint8_t>& Get() const { return m_bytes ? *m_bytes : Empty(); }
        operator const std::vector<std::uint8_t>&() const { return Get(); }
        const std::uint8_t* data() const { return Get().data(); }
        std::size_t size() const { return Get().size(); }
        bool empty() const { return Get().empty(); }
        std::vector<std::uint8_t>::const_iterator begin() const { return Get().begin(); }
        std::vector<std::uint8_t>::const_iterator end() const { return Get().end(); }
        std::uint8_t operator[](std::size_t index) const { return Get()[index]; }
        void resize(std::size_t size) { Mutable().resize(size); }

        // The bytes for writing, unshared first.
        std::vector<std::uint8_t>& Mutable()
        {
            if (!m_bytes)
                m_bytes = std::make_shared<std::vector<std::uint8_t>>();
            else if (m_bytes.use_count() > 1)
                m_bytes = std::make_shared<std::vector<std::uint8_t>>(*m_bytes);
            return *m_bytes;
        }
        bool SharesWith(const SharedBytes& other) const { return m_bytes && m_bytes == other.m_bytes; }

        friend bool operator==(const SharedBytes& a, const SharedBytes& b) { return a.Get() == b.Get(); }
        friend bool operator==(const SharedBytes& a, const std::vector<std::uint8_t>& b) { return a.Get() == b; }
        friend bool operator==(const std::vector<std::uint8_t>& a, const SharedBytes& b) { return a == b.Get(); }
        friend bool operator!=(const SharedBytes& a, const SharedBytes& b) { return !(a == b); }
        friend bool operator!=(const SharedBytes& a, const std::vector<std::uint8_t>& b) { return !(a == b); }
        friend bool operator!=(const std::vector<std::uint8_t>& a, const SharedBytes& b) { return !(a == b); }

    private:
        static const std::vector<std::uint8_t>& Empty()
        {
            static const std::vector<std::uint8_t> empty;
            return empty;
        }
        std::shared_ptr<std::vector<std::uint8_t>> m_bytes;
    };

    struct TrackChunkExport
    {
        std::uint32_t layout = 0; // index into TrackExport::layouts
        std::int32_t anchor = -1; // index into TrackExport::anchors, -1: none
        SharedBytes bytes;
        std::vector<std::uint32_t> frameTimes;
        std::vector<std::uint32_t> frameOffsets;
    };

    struct TrackExport
    {
        StreamConfig config;
        std::vector<SkeletonLayout> layouts;
        std::vector<std::vector<std::uint8_t>> anchors;
        std::vector<TrackChunkExport> chunks;
        std::uint32_t currentLayout = 0; // the layout new frames would use
        TrackStats stats;
    };

    // One pose record of a recording (the packed ReplayId of its entity, or
    // a pose segment record) with its track.
    struct PoseRecordExport
    {
        std::uint32_t recordId = 0;
        std::uint32_t boneCount = 0;
        TrackExport track;
    };

    class PoseTrack
    {
    public:
        void Begin(const SkeletonLayout& layout, const StreamConfig& config);
        // Switches the layout for subsequent frames. The next Append starts a
        // new chunk with a keyframe; earlier chunks keep their own layout.
        void UpdateLayout(const SkeletonLayout& layout);
        EncodeStats Append(const FrameInput& input);
        // Pose at an arbitrary time inside [first, last]. Channels are
        // interpolated between the surrounding frames unless the later frame
        // is flagged kFrameNoInterpolation.
        bool Sample(std::uint32_t timeMs, PoseSample& sample);

        const TrackStats& Stats() const { return m_stats; }
        // Replay buffer: drops the leading chunks that end before
        // timeMs (the chunk holding timeMs stays: it starts with the
        // keyframe the playhead decodes from). Returns the chunks dropped.
        std::size_t DropBefore(std::uint32_t timeMs);
        std::size_t Bytes() const { return m_totalBytes; }
        const SkeletonLayout& Layout() const { return *m_layout; }
        // The layout of the chunk
        // that plays timeMs (the last chunk starting at or before it, the
        // first one before the track), as Sample picks it; the current
        // layout when the track has no chunk.
        const SkeletonLayout& LayoutAt(std::uint32_t timeMs) const;
        // The bones in object space in LayoutAt(timeMs) that an earlier
        // chunk's layout had parented: the capture demoted them mid-recording
        // (the engine rebuilt that part of the skeleton under the same bone
        // count). One flag per bone; returns how many.
        std::uint32_t RebuiltBonesAt(std::uint32_t timeMs, std::vector<std::uint8_t>& rebuilt) const;
        // Samples that held frame A over an unflagged rebase.
        std::uint64_t UnflaggedRebaseHolds() const { return m_unflaggedRebaseHolds; }
        // Decodes that started from a scrub checkpoint instead of
        // the chunk keyframe, and the bytes the checkpoints hold now.
        std::uint64_t CheckpointRestores() const { return m_checkpointRestores; }
        std::size_t CheckpointBytes() const;
        // Every PoseTrack's scrub checkpoints on (the default) or off (tests
        // compare the output of both).
        static void SetCheckpointsEnabled(bool enabled);

        // Clip file: the recorded chunks with their layouts and anchors.
        void Export(TrackExport& out) const;
        // Replaces the track with an exported one (playback only: a later
        // Append starts a new chunk with a full keyframe). Every chunk is
        // decoded once to check it; on failure the track is left as it was
        // and `error` names the problem. The imported
        // track is sealed (Seal) and its decoders hold nothing until the
        // first Sample.
        bool Import(const TrackExport& in, std::string& error);

        // The track is done recording. Frees what only appending needs, the encoder
        // state (PoseEncoder::Release) and the track's own reference to the
        // current anchor (every chunk encoded against it keeps its own), and
        // the playback caches Sample rebuilds on demand (decoders,
        // materialized poses, scrub checkpoints, pair scratch), with their
        // serials reset so no cache is believed valid. The chunks with their
        // layouts and anchors, the current layout and the stats stay, so
        // Export, Sample, Stats, Bytes and DropBefore give exactly what they
        // gave before.
        // Like every PoseTrack call it needs the track to itself (the
        // capture calls it under g_mutex). It is correct at any time and
        // more than once; it pays once nothing appends any more: a retired
        // record whose ring is drained and released, with no frame pending
        // and no discovery or re-parent job queued or running, or every
        // record after StopAll stopped the worker and the discovery thread.
        // An Append after Seal still works (it never happens in the mod):
        // the encoder starts over on the current layout and the frame opens
        // a new chunk with a full keyframe, a new anchor, as after
        // UpdateLayout; the track is no longer sealed. Sample does not
        // unseal: it only rebuilds its caches.
        void Seal();
        bool Sealed() const { return m_sealed; }
        // The bytes (capacity) the encoder state and the
        // playback caches hold, for the tests and the evidence lines.
        std::size_t EncoderBytes() const { return m_encoder.StateBytes(); }
        std::size_t PlaybackCacheBytes() const;

    private:
        struct Chunk
        {
            std::shared_ptr<const SkeletonLayout> layout;
            // The full keyframe record this chunk's relative keyframe is
            // encoded against; null when the chunk starts with a full one.
            std::shared_ptr<const std::vector<std::uint8_t>> anchor;
            SharedBytes bytes; // shared with exports, copied on write
            std::vector<std::uint32_t> frameTimes;
            std::vector<std::uint32_t> frameOffsets;
        };

        bool DecodeTo(std::size_t chunkIndex, std::size_t frameIndex, PoseDecoder& decoder, std::size_t& cursorFrame);
        void EnforceBudget();

        std::shared_ptr<const SkeletonLayout> m_layout;
        bool m_forceKeyframe = false;
        // Seal ran and no Append, UpdateLayout or Begin
        // since; the encoder holds no state.
        bool m_sealed = false;
        // The current anchor record (the last full keyframe of this layout).
        std::shared_ptr<const std::vector<std::uint8_t>> m_anchor;
        StreamConfig m_config;
        PoseEncoder m_encoder;
        std::deque<Chunk> m_chunks;
        std::size_t m_totalBytes = 0;
        TrackStats m_stats;

        // Sequential playback cache: decoder positioned at a frame.
        // The decoders hold no channels until DecodeTo
        // Begins them (m_next may also be a copy of a positioned m_cursor);
        // Begin, Import and Seal leave both empty (ReleasePlaybackCaches).
        PoseDecoder m_cursor;
        std::uint64_t m_cursorChunkSerial = ~0ull;
        std::size_t m_cursorFrame = 0;
        std::uint64_t m_firstChunkSerial = 0;
        // Decoder at the frame after the cursor (kept between calls, so a
        // playhead that moves into it takes it over instead of decoding
        // again).
        PoseDecoder m_next;
        std::uint64_t m_nextChunkSerial = ~0ull;
        std::size_t m_nextFrame = 0;
        std::uint64_t m_unflaggedRebaseHolds = 0;

        // Backward scrub without decoding from the chunk keyframe every tick.
        // The cursor decoder cannot go back, so a playhead that moves
        // backward would decode the chunk from its keyframe to the frame (up
        // to 60 frames per puppet per tick at 30 Hz with 2 s chunks).
        // A decode that starts over keeps the decoder state every
        // kCheckpointStride frames of that chunk; the next one starts from
        // the nearest checkpoint at or before its frame (at most
        // kCheckpointStride - 1 frames more). Only the cursor's chunk is
        // kept; moving on two chunks frees them.
        static constexpr std::size_t kCheckpointStride = 8;
        std::uint64_t m_checkpointSerial = ~0ull;
        std::vector<PoseDecoder::State> m_checkpoints; // index k: the state after frame k * stride
        std::vector<std::uint8_t> m_checkpointValid;
        std::uint64_t m_checkpointRestores = 0;
        void ResetCheckpoints(bool release);

        // Materialized (composed objects) poses of the cursor and next
        // frames: at 60 fps a 30 Hz frame is sampled twice, and the next
        // frame becomes the cursor frame one call later. The
        // locals are the decoder's own first channels (no copy).
        struct Materialized
        {
            std::uint64_t serial = ~0ull;
            std::size_t frame = 0;
            std::vector<BoneQts> objects;
        };
        Materialized m_poseA;
        Materialized m_poseB;
        void Materialize(const SkeletonLayout& layout, const PoseDecoder& decoder,
            std::uint64_t serial, std::size_t frame, Materialized& out);

        // Sampling without copies (bit for bit the copying output,
        // tests/pose_sample_tests.cpp).
        // The frame m_next's decoder state holds, whatever the cache says
        // (after a swap it is the old cursor frame): a next frame one ahead
        // of it in the same chunk is reached by applying two records to it
        // instead of copying every channel of the cursor.
        std::uint64_t m_nextStateSerial = ~0ull;
        std::size_t m_nextStateFrame = 0;
        // The channels the next frame's record wrote over the cursor frame
        // (valid for m_touchedSerial / m_touchedFrame): only the bones they
        // reach are composed again for frame B and compared for the blend.
        std::vector<std::uint32_t> m_touched;
        std::uint64_t m_touchedSerial = ~0ull;
        std::size_t m_touchedFrame = 0;
        std::vector<std::uint8_t> m_dirty;
        // What depends only on the pair of frames, not on t: the bones whose
        // locals and objects differ between A and B, and the rebase hold.
        struct Pair
        {
            std::uint64_t serialA = ~0ull;
            std::size_t frameA = 0;
            std::uint64_t serialB = ~0ull;
            std::size_t frameB = 0;
            std::vector<std::uint32_t> locals;
            std::vector<std::uint32_t> objects;
            bool rebase = false;
            bool sameBones = true;
        };
        Pair m_pair;
        void PreparePair(const SkeletonLayout& layoutA, const SkeletonLayout& layoutB,
            std::uint64_t serialA, std::size_t frameA, std::uint64_t serialB, std::size_t frameB);
        // Frees every playback cache above and resets

        // its serial (Begin, Seal).
        void ReleasePlaybackCaches();
    };
}
