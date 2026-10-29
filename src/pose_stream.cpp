#include "pose_stream.h"

#include "profiler.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <emmintrin.h>

namespace replay::pose
{
    namespace
    {
        constexpr std::size_t kRecordHeaderBytes =
            sizeof(std::uint32_t) * 2 + sizeof(std::uint16_t) * 2 + sizeof(float) * 16;

        // The largest
        // entry: channel, mask, a 64-bit rotation, a float translation, w and
        // scale (rot32 and rot64 exclude each other, so do trans16 and
        // transFloat). A record holds at most one entry per channel.
        constexpr std::size_t kMaxEntryBytes = sizeof(std::uint16_t) + sizeof(std::uint8_t) +
            sizeof(std::uint64_t) + sizeof(float) * 3 + sizeof(float) + sizeof(float) * 4;
        static_assert(kMaxEntryBytes == 43, "the entry layout of the header comment");

        // Writes at `at` and moves it past the value. The record scratch has
        // room for the worst case (PoseEncoder::Begin), so nothing grows per
        // field.
        template <typename T>
        void Put(std::uint8_t*& at, const T& value)
        {
            std::memcpy(at, &value, sizeof(T));
            at += sizeof(T);
        }

        // How many changed channels ahead of the one being
        // encoded the two-pass delta frame prefetches. Measured offline on
        // the recorded clips, every track interleaved with the caches
        // flushed between rounds: 2 to 8 ahead took 9 to 12% off the
        // encoder, 16 nothing; with warm caches 4 ahead cost 9%.
        constexpr std::uint32_t kPrefetchAhead = 4;

        // The lines a channel's delta work reads: its
        // emitted bone (48 bytes, which may straddle two lines), its emitted
        // code and, for a bone channel, the layout's high-precision byte.
        // A prefetch never faults, whatever the address.
        inline void PrefetchChannelState(const BoneQts* emitted, const std::uint64_t* codes,
            const std::uint8_t* highPrecision, std::uint32_t boneCount, std::uint32_t channel)
        {
            const char* bone = reinterpret_cast<const char*>(emitted + channel);
            _mm_prefetch(bone, _MM_HINT_T0);
            _mm_prefetch(bone + sizeof(BoneQts) - 1, _MM_HINT_T0);
            _mm_prefetch(reinterpret_cast<const char*>(codes + channel), _MM_HINT_T0);
            if (channel < boneCount)
                _mm_prefetch(reinterpret_cast<const char*>(highPrecision + channel), _MM_HINT_T0);
        }

        template <typename T>
        bool Get(const std::uint8_t* data, std::size_t size, std::size_t& offset, T& value)
        {
            if (size < offset || size - offset < sizeof(T))
                return false;
            std::memcpy(&value, data + offset, sizeof(T));
            offset += sizeof(T);
            return true;
        }

        bool SameBits(const float* a, const float* b, std::size_t count)
        {
            return std::memcmp(a, b, sizeof(float) * count) == 0;
        }

        // The 48 bytes of two bone records are equal.
        inline bool SameBone(const BoneQts& a, const BoneQts& b)
        {
            const auto* pa = reinterpret_cast<const __m128i*>(&a);
            const auto* pb = reinterpret_cast<const __m128i*>(&b);
            const __m128i e0 = _mm_cmpeq_epi8(_mm_loadu_si128(pa), _mm_loadu_si128(pb));
            const __m128i e1 = _mm_cmpeq_epi8(_mm_loadu_si128(pa + 1), _mm_loadu_si128(pb + 1));
            const __m128i e2 = _mm_cmpeq_epi8(_mm_loadu_si128(pa + 2), _mm_loadu_si128(pb + 2));
            return _mm_movemask_epi8(_mm_and_si128(_mm_and_si128(e0, e1), e2)) == 0xFFFF;
        }

        // SetShortcuts (set before a recording, read by the worker).
        std::atomic<bool> g_rawSkip{true};
        std::atomic<bool> g_cheapTests{true};

        // ComposeObjects (pose_skeleton_layout.cpp) with the SSE
        // Compose, bit for bit the same result.
        void ComposeObjectsSimd(
            const SkeletonLayout& layout,
            const BoneQts* locals,
            const BoneQts* unresolvedObjects,
            BoneQts* objects)
        {
            for (std::size_t index = 0; index < layout.unresolvedBones.size(); ++index)
                objects[layout.unresolvedBones[index]] = unresolvedObjects[index];
            for (const std::uint32_t bone : layout.evaluationOrder)
            {
                const std::int32_t parent = layout.parent[bone];
                if (parent == kRootParent)
                    objects[bone] = locals[bone];
                else if (parent >= 0)
                    ComposeSimd(objects[static_cast<std::uint32_t>(parent)], locals[bone], layout.order,
                        layout.inheritScale, objects[bone]);
            }
        }
    }

    void PoseEncoder::Begin(const SkeletonLayout& layout, const StreamConfig& config)
    {
        m_layout = &layout;
        m_config = config;
        m_emitted.assign(layout.ChannelCount(), BoneQts{});
        m_emittedCode.assign(layout.ChannelCount(), ~0ull);
        m_anchorEmitted.clear();
        m_anchorCode.clear();
        m_hasAnchor = false;
        m_lastInput.assign(layout.ChannelCount(), BoneQts{});
        m_hasLastInput = false;
        m_angleTest = MakeAngleTest(config.rotationEpsilon);
    }

    void PoseEncoder::Release()
    {
        // Swapped with
        // empty vectors, so the memory really goes (clear keeps it). Begin
        // assigns every vector again and resets both flags.
        m_layout = nullptr;
        std::vector<BoneQts>().swap(m_lastInput);
        m_hasLastInput = false;
        std::vector<BoneQts>().swap(m_emitted);
        std::vector<std::uint64_t>().swap(m_emittedCode);
        std::vector<BoneQts>().swap(m_anchorEmitted);
        std::vector<std::uint64_t>().swap(m_anchorCode);
        m_hasAnchor = false;
    }

    std::size_t PoseEncoder::StateBytes() const
    {
        return (m_lastInput.capacity() + m_emitted.capacity() + m_anchorEmitted.capacity()) * sizeof(BoneQts) +
            (m_emittedCode.capacity() + m_anchorCode.capacity()) * sizeof(std::uint64_t);
    }

    void PoseEncoder::SetShortcuts(bool rawSkip, bool cheapTests)
    {
        g_rawSkip.store(rawSkip, std::memory_order_relaxed);
        g_cheapTests.store(cheapTests, std::memory_order_relaxed);
    }

    const BoneQts& PoseEncoder::Source(const FrameInput& input, std::uint32_t channel) const
    {
        if (channel < m_layout->boneCount)
            return input.locals[channel];
        return input.objects[m_layout->unresolvedBones[channel - m_layout->boneCount]];
    }

    EncodeStats PoseEncoder::Encode(
        const FrameInput& input,
        KeyframeKind kind,
        std::vector<std::uint8_t>& out)
    {
        EncodeStats stats;
        if (kind == KeyframeKind::Relative && !m_hasAnchor)
            kind = KeyframeKind::Full;
        const bool keyframe = kind == KeyframeKind::Full;
        if (keyframe)
        {
            std::fill(m_emitted.begin(), m_emitted.end(), BoneQts{});
            std::fill(m_emittedCode.begin(), m_emittedCode.end(), ~0ull);
        }
        else if (kind == KeyframeKind::Relative)
        {
            m_emitted = m_anchorEmitted;
            m_emittedCode = m_anchorCode;
        }

        const std::uint32_t channels = m_layout->ChannelCount();
        // The record is written into a scratch buffer and appended to `out`
        // in one insert, the same bytes in the same order.
        // The record scratch and the changed list
        // are per thread, not per encoder: held per record they were ~50 KB
        // for a 1050-bone ped, zero-filled at every Begin and cold on every
        // frame. Encode never re-enters on one thread and only reads what it
        // wrote this call (the record up to `at`, the list up to its count),
        // so the stale contents of a reused buffer never reach the output.
        // They only grow, to the largest layout this thread encoded.
        static thread_local std::vector<std::uint8_t> t_record;
        static thread_local std::vector<std::uint32_t> t_changed;
        const std::size_t largestRecord = kRecordHeaderBytes + kMaxEntryBytes * channels;
        if (t_record.size() < largestRecord)
            t_record.resize(largestRecord);
        if (t_changed.size() < channels)
            t_changed.resize(channels);
        std::uint8_t* const record = t_record.data();
        std::uint8_t* at = record;
        Put<std::uint32_t>(at, 0); // patched below
        Put<std::uint32_t>(at, input.timeMs);
        Put<std::uint16_t>(at, static_cast<std::uint16_t>(
            input.flags | (keyframe ? kFrameKeyframe : 0) |
            (kind == KeyframeKind::Relative ? kFrameRelativeKeyframe : 0)));
        std::uint8_t* const countAt = at;
        Put<std::uint16_t>(at, 0); // patched below
        float world[16] = {};
        if (input.world)
            std::memcpy(world, input.world, sizeof(world));
        for (const float value : world)
            Put<float>(at, value);

        // Only a delta frame may skip on equal input bytes; a
        // keyframe resets the state (full) or restores the anchor's
        // (relative), after which the previous frame's input proves nothing.
        const bool rawSkipOn = g_rawSkip.load(std::memory_order_relaxed) && m_lastInput.size() == channels;
        const bool rawSkip = rawSkipOn && kind == KeyframeKind::None && m_hasLastInput;
        const bool cheap = g_cheapTests.load(std::memory_order_relaxed);
        if (kind == KeyframeKind::None)
            stats.deltaChannels = channels;
        // A delta frame
        // that skips on equal input bytes runs in two passes. Pass 1 compares
        // every channel's input with the previous one (SSE, a linear walk of
        // the input and m_lastInput, which the hardware prefetches), keeps
        // the new input of each channel that changed and lists it, in
        // channel order. Pass 2 is the channel loop below over that list, in
        // the same increasing order, prefetching the state of the channel
        // kPrefetchAhead entries ahead: the changed channels are a few
        // percent, scattered, and their m_emitted and m_emittedCode lines
        // are cold after the worker visited every other record. A channel's
        // work reads and writes only that channel's state, so the entries,
        // their order and the stats are the one-pass ones. Keyframes and the
        // no-skip path walk every channel through the same loop.
        const std::uint32_t* changed = nullptr;
        std::uint32_t count = channels;
        const std::uint32_t boneCount = m_layout->boneCount;
        if (rawSkip)
        {
            std::uint32_t* const list = t_changed.data();
            BoneQts* const last = m_lastInput.data();
            std::uint32_t changedCount = 0;
            for (std::uint32_t channel = 0; channel < boneCount; ++channel)
            {
                const BoneQts& current = input.locals[channel];
                if (SameBone(last[channel], current))
                    continue;
                last[channel] = current;
                list[changedCount++] = channel;
            }
            const std::uint32_t* const unresolved = m_layout->unresolvedBones.data();
            for (std::uint32_t channel = boneCount; channel < channels; ++channel)
            {
                const BoneQts& current = input.objects[unresolved[channel - boneCount]];
                if (SameBone(last[channel], current))
                    continue;
                last[channel] = current;
                list[changedCount++] = channel;
            }
            stats.rawSameChannels = channels - changedCount;
            changed = list;
            count = changedCount;
            for (std::uint32_t index = 0; index < count && index < kPrefetchAhead; ++index)
            {
                PrefetchChannelState(m_emitted.data(), m_emittedCode.data(), m_layout->highPrecision.data(),
                    boneCount, changed[index]);
            }
        }
        // A keyframe (and a delta frame without a previous
        // input) keeps every channel's input for the next frame.
        const bool keepInput = rawSkipOn && !rawSkip;
        for (std::uint32_t index = 0; index < count; ++index)
        {
            const std::uint32_t channel = changed ? changed[index] : index;
            if (changed && index + kPrefetchAhead < count)
            {
                PrefetchChannelState(m_emitted.data(), m_emittedCode.data(), m_layout->highPrecision.data(),
                    boneCount, changed[index + kPrefetchAhead]);
            }
            const BoneQts& current = Source(input, channel);
            if (keepInput)
                m_lastInput[channel] = current;
            BoneQts& emitted = m_emitted[channel];
            const bool highPrecision =
                channel >= m_layout->boneCount || m_layout->highPrecision[channel] != 0;

            std::uint8_t mask = 0;
            // The rotation is emitted when its code differs
            // from the last emitted one AND it turned more than epsilon from
            // the emitted rotation. Both are pure, so the order does not
            // matter: the angle (cheap test, exact near the threshold)
            // comes first and the quantization only runs when it passes or
            // on a keyframe (the code is only used to emit).
            std::uint64_t code = 0;
            bool rotate = keyframe;
            if (!cheap)
            {
                // Quantize first, then the exact angle.
                code = highPrecision ? PackQuat64(current.rotation) : PackQuat32(current.rotation);
                rotate = keyframe ||
                    (code != m_emittedCode[channel] &&
                        AngleBetween(emitted.rotation, current.rotation) > m_config.rotationEpsilon);
            }
            else if (!keyframe && AngleExceeds(m_angleTest, emitted.rotation, current.rotation))
            {
                code = highPrecision ? PackQuat64(current.rotation) : PackQuat32(current.rotation);
                rotate = code != m_emittedCode[channel];
            }
            else if (keyframe)
            {
                code = highPrecision ? PackQuat64(current.rotation) : PackQuat32(current.rotation);
            }
            if (rotate)
                mask |= highPrecision ? kEntryRot64 : kEntryRot32;

            std::int16_t packedTranslation[3] = {};
            if (keyframe ||
                (cheap ? TranslationExceeds(emitted, current, m_config.translationEpsilon)
                       : TranslationDistance(emitted, current) > m_config.translationEpsilon))
            {
                mask |= PackTranslation16(
                            current.translation,
                            m_config.translationRange,
                            packedTranslation)
                    ? kEntryTrans16
                    : kEntryTransFloat;
            }
            if (!SameBits(&emitted.translation[3], &current.translation[3], 1))
                mask |= kEntryTransW;
            if (!SameBits(emitted.scale, current.scale, 4))
                mask |= kEntryScale;

            if (mask == 0)
                continue;

            Put<std::uint16_t>(at, static_cast<std::uint16_t>(channel));
            Put<std::uint8_t>(at, mask);
            if (mask & kEntryRot32)
            {
                const std::uint32_t packed = static_cast<std::uint32_t>(code);
                Put(at, packed);
                emitted.rotation = UnpackQuat32(packed);
                m_emittedCode[channel] = code;
            }
            if (mask & kEntryRot64)
            {
                Put(at, code);
                emitted.rotation = UnpackQuat64(code);
                m_emittedCode[channel] = code;
            }
            if (mask & kEntryTrans16)
            {
                for (const std::int16_t value : packedTranslation)
                    Put(at, value);
                UnpackTranslation16(
                    packedTranslation, m_config.translationRange, emitted.translation);
            }
            if (mask & kEntryTransFloat)
            {
                for (int axis = 0; axis < 3; ++axis)
                {
                    Put(at, current.translation[axis]);
                    emitted.translation[axis] = current.translation[axis];
                }
            }
            if (mask & kEntryTransW)
            {
                Put(at, current.translation[3]);
                emitted.translation[3] = current.translation[3];
            }
            if (mask & kEntryScale)
            {
                for (int axis = 0; axis < 4; ++axis)
                {
                    Put(at, current.scale[axis]);
                    emitted.scale[axis] = current.scale[axis];
                }
            }
            ++stats.entries;
        }

        m_hasLastInput = rawSkipOn; // off: nothing was kept
        stats.rawSameFrame = rawSkip && channels > 0 && stats.rawSameChannels == channels;

        const std::uint32_t recordSize = static_cast<std::uint32_t>(at - record);
        const std::uint16_t entryCount = static_cast<std::uint16_t>(stats.entries);
        std::memcpy(record, &recordSize, sizeof(recordSize));
        std::memcpy(countAt, &entryCount, sizeof(entryCount));
        out.insert(out.end(), record, at); // the whole record at once
        stats.bytes = recordSize;
        if (keyframe)
        {
            m_anchorEmitted = m_emitted;
            m_anchorCode = m_emittedCode;
            m_hasAnchor = true;
        }
        return stats;
    }

    void ComposeEngineObjects(const std::uint16_t* parents, std::uint32_t boneCount, float rootScale,
        const BoneQts* locals, BoneQts* objects)
    {
        // Same composition as the capture worker (pose_capture.cpp).
        objects[0] = locals[0];
        for (int axis = 0; axis < 3; ++axis)
        {
            objects[0].translation[axis] *= rootScale;
            objects[0].scale[axis] *= rootScale;
        }
        objects[0].scale[3] *= rootScale;
        for (std::uint32_t bone = 1; bone < boneCount; ++bone)
        {
            if (parents[bone] < bone)
                objects[bone] = Compose(objects[parents[bone]], locals[bone], CompositionOrder::ParentThenLocal, true);
            else
                objects[bone] = locals[bone];
        }
    }

    bool IsExactEngineLayout(const SkeletonLayout& layout, const std::uint16_t* parents, std::uint32_t boneCount,
        float rootScale)
    {
        if (!parents || boneCount == 0 || layout.boneCount != boneCount || layout.parent.size() != boneCount ||
            layout.order != CompositionOrder::ParentThenLocal || !layout.inheritScale)
        {
            return false;
        }
        // The discovery's test (pose_capture.cpp DiscoveryMain); a scale
        // within 1e-6 of 1 but not 1 would make bone 0's object differ from
        // its local by a hair, so such a table is not taken as exact.
        const bool rootScaled = std::fabs(rootScale - 1.0f) > 1e-6f;
        if (!rootScaled && rootScale != 1.0f)
            return false;
        if (layout.parent[0] != (rootScaled ? kUnresolvedParent : kRootParent))
            return false;
        for (std::uint32_t bone = 1; bone < boneCount; ++bone)
        {
            const std::int32_t expected = parents[bone] < bone ? static_cast<std::int32_t>(parents[bone]) : kUnresolvedParent;
            if (layout.parent[bone] != expected)
                return false;
        }
        return true;
    }

    void EngineComposeBones(const SkeletonLayout& layout, const std::uint16_t* parents, std::uint32_t boneCount,
        std::vector<std::uint32_t>& bones)
    {
        bones.clear();
        if (!parents || boneCount == 0)
            return;
        std::vector<std::uint8_t> needed(boneCount, 0);
        auto mark = [&](std::uint32_t bone) {
            // The bone and its ancestors, as ComposeEngineObjects reaches
            // them (a parent that is not an earlier bone ends the chain).
            while (bone < boneCount && !needed[bone])
            {
                needed[bone] = 1;
                if (bone == 0 || parents[bone] >= bone)
                    break;
                bone = parents[bone];
            }
        };
        mark(0);
        for (const std::uint32_t bone : layout.unresolvedBones)
            mark(bone);
        for (std::uint32_t bone = 0; bone < boneCount; ++bone)
        {
            if (needed[bone])
                bones.push_back(bone);
        }
    }

    void ComposeEngineObjectsSubset(const std::uint16_t* parents, std::uint32_t boneCount, float rootScale,
        const std::vector<std::uint32_t>& bones, const BoneQts* locals, BoneQts* objects)
    {
        for (const std::uint32_t bone : bones)
        {
            if (bone >= boneCount)
                continue;
            if (bone == 0)
            {
                objects[0] = locals[0];
                for (int axis = 0; axis < 3; ++axis)
                {
                    objects[0].translation[axis] *= rootScale;
                    objects[0].scale[axis] *= rootScale;
                }
                objects[0].scale[3] *= rootScale;
            }
            else if (parents[bone] < bone)
            {
                objects[bone] = Compose(objects[parents[bone]], locals[bone], CompositionOrder::ParentThenLocal, true);
            }
            else
            {
                objects[bone] = locals[bone];
            }
        }
    }

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
        bool& equalBlocks)
    {
        LatePoseChange change;
        equalBlocks = false;
        if (!finalizerLocals || !finalizerObjects || !lateLocals || !lateObjects || !engineParents || boneCount == 0)
            return change;
        const std::size_t blockBytes = static_cast<std::size_t>(boneCount) * sizeof(BoneQts);
        const bool localsEqual = std::memcmp(finalizerLocals, lateLocals, blockBytes) == 0;
        const bool objectsEqual = std::memcmp(finalizerObjects, lateObjects, blockBytes) == 0;
        if (localsEqual && objectsEqual)
        {
            equalBlocks = true;
            return change;
        }
        // Per bone: bit 0 the local differs, bit 1 the object differs.
        changedMask.assign(boneCount, 0);
        for (std::uint32_t bone = 0; bone < boneCount; ++bone)
        {
            std::uint8_t bits = 0;
            if (!localsEqual && !SameBone(finalizerLocals[bone], lateLocals[bone]))
                bits |= 1u;
            if (!objectsEqual && !SameBone(finalizerObjects[bone], lateObjects[bone]))
                bits |= 2u;
            changedMask[bone] = bits;
        }
        // The rest is CompareLatePose (pose_skeleton_layout.cpp) over the
        // bones that can differ, in the same order.
        float worst = 0.0f;
        auto note = [&](std::uint32_t bone, float translation, float rotation) {
            change.maxTranslation = (std::max)(change.maxTranslation, translation);
            change.maxRotation = (std::max)(change.maxRotation, rotation);
            if (translation >= worst)
            {
                worst = translation;
                change.worstBone = static_cast<std::int32_t>(bone);
            }
        };
        if (!localsEqual)
        {
            for (std::uint32_t bone = 0; bone < boneCount; ++bone)
            {
                if (!(changedMask[bone] & 1u))
                    continue;
                const float translation = TranslationDistance(finalizerLocals[bone], lateLocals[bone]);
                const float rotation = AngleBetween(finalizerLocals[bone].rotation, lateLocals[bone].rotation);
                if (translation > translationTolerance || rotation > rotationTolerance)
                {
                    ++change.localBones;
                    note(bone, translation, rotation);
                }
            }
        }
        scratch.resize(boneCount);
        for (std::uint32_t bone = 0; bone < boneCount; ++bone)
        {
            const bool parented = bone != 0 && engineParents[bone] < bone;
            if (bone != 0 && !parented)
                continue; // not composed by the finalizer either
            const bool inputsChanged = changedMask[bone] != 0 ||
                (parented && (changedMask[engineParents[bone]] & 2u) != 0);
            if (!inputsChanged)
                continue;
            if (bone == 0)
            {
                scratch[0] = lateLocals[0];
                for (int axis = 0; axis < 3; ++axis)
                {
                    scratch[0].translation[axis] *= rootScale;
                    scratch[0].scale[axis] *= rootScale;
                }
                scratch[0].scale[3] *= rootScale;
            }
            else
            {
                scratch[bone] = Compose(lateObjects[engineParents[bone]], lateLocals[bone],
                    CompositionOrder::ParentThenLocal, true);
            }
            const float translation = TranslationDistance(scratch[bone], lateObjects[bone]);
            const float rotation = AngleBetween(scratch[bone].rotation, lateObjects[bone].rotation);
            if (translation > translationTolerance || rotation > rotationTolerance)
            {
                ++change.objectBones;
                note(bone, translation, rotation);
            }
        }
        return change;
    }

    void PoseDecoder::Begin(const SkeletonLayout& layout, const StreamConfig& config)
    {
        m_layout = &layout;
        m_config = config;
        m_channels.assign(layout.ChannelCount(), BoneQts{});
        m_timeMs = 0;
        m_flags = 0;
        std::memset(m_world, 0, sizeof(m_world));
        m_hasKeyframe = false;
    }

    namespace
    {
        // Payload bytes of one entry, in mask bit order (unknown bits carry
        // none, as the decoder always read them).
        std::size_t EntryPayloadBytes(std::uint8_t mask)
        {
            std::size_t bytes = 0;
            if (mask & kEntryRot32)
                bytes += sizeof(std::uint32_t);
            if (mask & kEntryRot64)
                bytes += sizeof(std::uint64_t);
            if (mask & kEntryTrans16)
                bytes += sizeof(std::int16_t) * 3;
            if (mask & kEntryTransFloat)
                bytes += sizeof(float) * 3;
            if (mask & kEntryTransW)
                bytes += sizeof(float);
            if (mask & kEntryScale)
                bytes += sizeof(float) * 4;
            return bytes;
        }

        template <typename T>
        T Read(const std::uint8_t* data, std::size_t& offset)
        {
            T value;
            std::memcpy(&value, data + offset, sizeof(T));
            offset += sizeof(T);
            return value;
        }
    }

    bool PoseDecoder::Apply(
        const std::uint8_t* data,
        std::size_t size,
        std::size_t& consumed,
        std::vector<std::uint32_t>* touched)
    {
        consumed = 0;
        if (!m_layout || !data)
            return false;

        std::size_t offset = 0;
        std::uint32_t recordSize = 0;
        std::uint32_t timeMs = 0;
        std::uint16_t flags = 0;
        std::uint16_t entryCount = 0;
        if (!Get(data, size, offset, recordSize) ||
            recordSize < kRecordHeaderBytes || recordSize > size ||
            !Get(data, recordSize, offset, timeMs) ||
            !Get(data, recordSize, offset, flags) ||
            !Get(data, recordSize, offset, entryCount))
        {
            return false;
        }
        const bool keyframe = (flags & kFrameKeyframe) != 0;
        if (!keyframe && !m_hasKeyframe)
            return false;

        float world[16] = {};
        for (float& value : world)
        {
            if (!Get(data, recordSize, offset, value))
                return false;
        }

        // Check the whole record first (every entry in range and complete,
        // nothing after the last), so the decode below can write the channels
        // in place: a malformed record still changes nothing.
        const std::size_t entriesStart = offset;
        const std::size_t channelCount = m_channels.size();
        for (std::uint16_t entry = 0; entry < entryCount; ++entry)
        {
            std::uint16_t channel = 0;
            std::uint8_t mask = 0;
            if (!Get(data, recordSize, offset, channel) ||
                !Get(data, recordSize, offset, mask) ||
                channel >= channelCount)
            {
                return false;
            }
            const std::size_t payload = EntryPayloadBytes(mask);
            if (recordSize - offset < payload)
                return false;
            offset += payload;
        }
        if (offset != recordSize)
            return false;

        if (keyframe)
            std::fill(m_channels.begin(), m_channels.end(), BoneQts{});
        if (touched)
            touched->clear();
        offset = entriesStart;
        for (std::uint16_t entry = 0; entry < entryCount; ++entry)
        {
            const std::uint16_t channel = Read<std::uint16_t>(data, offset);
            const std::uint8_t mask = Read<std::uint8_t>(data, offset);
            BoneQts& value = m_channels[channel];
            if (mask & kEntryRot32)
                value.rotation = UnpackQuat32(Read<std::uint32_t>(data, offset));
            if (mask & kEntryRot64)
                value.rotation = UnpackQuat64(Read<std::uint64_t>(data, offset));
            if (mask & kEntryTrans16)
            {
                std::int16_t packed[3] = {};
                for (std::int16_t& axis : packed)
                    axis = Read<std::int16_t>(data, offset);
                UnpackTranslation16(packed, m_config.translationRange, value.translation);
            }
            if (mask & kEntryTransFloat)
            {
                for (int axis = 0; axis < 3; ++axis)
                    value.translation[axis] = Read<float>(data, offset);
            }
            if (mask & kEntryTransW)
                value.translation[3] = Read<float>(data, offset);
            if (mask & kEntryScale)
            {
                for (float& axis : value.scale)
                    axis = Read<float>(data, offset);
            }
            if (touched)
                touched->push_back(channel);
        }

        m_timeMs = timeMs;
        m_flags = flags;
        std::memcpy(m_world, world, sizeof(world));
        m_hasKeyframe = true;
        consumed = recordSize;
        return true;
    }

    void PoseDecoder::SaveState(State& out) const
    {
        out.channels.assign(m_channels.begin(), m_channels.end());
        out.timeMs = m_timeMs;
        out.flags = m_flags;
        std::memcpy(out.world, m_world, sizeof(out.world));
        out.hasKeyframe = m_hasKeyframe;
    }

    void PoseDecoder::RestoreState(const State& state)
    {
        m_channels.assign(state.channels.begin(), state.channels.end());
        m_timeMs = state.timeMs;
        m_flags = state.flags;
        std::memcpy(m_world, state.world, sizeof(m_world));
        m_hasKeyframe = state.hasKeyframe;
    }

    namespace
    {
        // The work counters are per thread.
        // The pose fidelity judge samples its own copies of the tracks on a
        // worker thread (pose_fidelity.cpp); the script thread's counters
        // (the seek settle deltas, the tests) stay the script thread's, and
        // two threads never write the same counter.
        thread_local std::uint64_t g_decodedFrames = 0; // the calling thread's
        thread_local std::uint64_t g_checkpointRestoresAll = 0;
        bool g_checkpointsEnabled = true;
    }

    namespace
    {
        thread_local SampleWork g_sampleWork; // the calling thread's
    }

    SampleWork SampleWorkCounters() { return g_sampleWork; }

    std::uint64_t DecodedFrameCount() { return g_decodedFrames; }
    std::uint64_t CheckpointRestoreCount() { return g_checkpointRestoresAll; }

    void PoseTrack::SetCheckpointsEnabled(bool enabled) { g_checkpointsEnabled = enabled; }

    std::size_t PoseTrack::CheckpointBytes() const
    {
        std::size_t bytes = 0;
        for (const PoseDecoder::State& state : m_checkpoints)
            bytes += state.channels.capacity() * sizeof(BoneQts);
        return bytes;
    }

    void PoseTrack::ResetCheckpoints(bool release)
    {
        m_checkpointSerial = ~0ull;
        std::fill(m_checkpointValid.begin(), m_checkpointValid.end(), static_cast<std::uint8_t>(0));
        if (release)
        {
            std::vector<PoseDecoder::State>().swap(m_checkpoints);
            std::vector<std::uint8_t>().swap(m_checkpointValid);
        }
    }

    void PoseTrack::Begin(const SkeletonLayout& layout, const StreamConfig& config)
    {
        m_layout = std::make_shared<const SkeletonLayout>(layout);
        m_forceKeyframe = false;
        m_sealed = false;
        m_anchor.reset();
        m_config = config;
        m_encoder.Begin(*m_layout, m_config);
        m_chunks.clear();
        m_totalBytes = 0;
        m_stats = {};
        m_firstChunkSerial = 0;
        // The decoders are not Begun here (two channel vectors in every
        // track, even one that never plays back): DecodeTo Begins the decoder
        // it decodes into and the next-frame path copies the positioned
        // cursor, so both start empty like the other playback caches.
        ReleasePlaybackCaches();
    }

    void PoseTrack::UpdateLayout(const SkeletonLayout& layout)
    {
        m_layout = std::make_shared<const SkeletonLayout>(layout);
        m_encoder.Begin(*m_layout, m_config);
        m_sealed = false; // the encoder holds state again
        m_forceKeyframe = true;
        m_anchor.reset(); // the channel set changes: the next keyframe is full
    }

    void PoseTrack::Seal()
    {
        // See the
        // header. m_anchor is only read by Append, and an Append after Seal
        // starts a full keyframe (a new anchor) before it would read it; the
        // chunks encoded against it hold their own reference.
        m_encoder.Release();
        m_anchor.reset();
        ReleasePlaybackCaches();
        m_sealed = true;
    }

    void PoseTrack::ReleasePlaybackCaches()
    {
        // Begin's resets, freeing the
        // memory as well: fresh decoders, poses and pair, and empty touched
        // and dirty scratch (only read after a decode fills them).
        ResetCheckpoints(true);
        m_cursor = PoseDecoder{};
        m_next = PoseDecoder{};
        m_cursorChunkSerial = ~0ull;
        m_cursorFrame = 0;
        m_nextChunkSerial = ~0ull;
        m_nextFrame = 0;
        m_poseA = {};
        m_poseB = {};
        m_nextStateSerial = ~0ull;
        m_nextStateFrame = 0;
        std::vector<std::uint32_t>().swap(m_touched);
        m_touchedSerial = ~0ull;
        m_touchedFrame = 0;
        std::vector<std::uint8_t>().swap(m_dirty);
        m_pair = {};
    }

    std::size_t PoseTrack::PlaybackCacheBytes() const
    {
        // Capacity of every cache ReleasePlaybackCaches frees.
        std::size_t bytes = CheckpointBytes() + m_checkpoints.capacity() * sizeof(PoseDecoder::State) +
            m_checkpointValid.capacity();
        bytes += (m_cursor.Channels().capacity() + m_next.Channels().capacity()) * sizeof(BoneQts);
        bytes += (m_poseA.objects.capacity() + m_poseB.objects.capacity()) * sizeof(BoneQts);
        bytes += (m_touched.capacity() + m_pair.locals.capacity() + m_pair.objects.capacity()) * sizeof(std::uint32_t);
        bytes += m_dirty.capacity();
        return bytes;
    }

    EncodeStats PoseTrack::Append(const FrameInput& input)
    {
        // A sealed
        // track appends again from scratch: the encoder starts over on the
        // current layout and this frame opens a new chunk with a full
        // keyframe (the encoder kept nothing to encode a delta or a
        // relative keyframe against), as after UpdateLayout.
        if (m_sealed)
        {
            m_encoder.Begin(*m_layout, m_config);
            m_forceKeyframe = true;
            m_anchor.reset();
            m_sealed = false;
        }
        // Chunks break on the global block grid (multiples of the
        // interval, the lifecycle's 2 s blocks), not every interval from the
        // track's start: every track has a keyframe at each block start, so
        // a clip cut at a block (replay buffer, window cut) starts on one.
        const std::uint32_t interval = m_config.keyframeIntervalMs ? m_config.keyframeIntervalMs : 1;
        const bool keyframe = m_forceKeyframe || m_chunks.empty() ||
            input.timeMs / interval != m_chunks.back().frameTimes.front() / interval;
        KeyframeKind kind = KeyframeKind::None;
        if (keyframe)
        {
            kind = (m_forceKeyframe || m_chunks.empty() || !m_anchor || !m_encoder.HasAnchor())
                ? KeyframeKind::Full
                : KeyframeKind::Relative;
            m_chunks.emplace_back();
            m_chunks.back().layout = m_layout;
            if (kind == KeyframeKind::Relative)
                m_chunks.back().anchor = m_anchor;
            m_forceKeyframe = false;
        }

        Chunk& chunk = m_chunks.back();
        chunk.frameOffsets.push_back(static_cast<std::uint32_t>(chunk.bytes.size()));
        chunk.frameTimes.push_back(input.timeMs);
        const std::size_t recordStart = chunk.bytes.size();
        const EncodeStats encoded = m_encoder.Encode(input, kind, chunk.bytes.Mutable()); // unshared first
        if (keyframe)
        {
            // The encoder may have fallen back to a full keyframe.
            std::uint16_t flags = 0;
            std::memcpy(&flags, chunk.bytes.data() + recordStart + sizeof(std::uint32_t) * 2, sizeof(flags));
            if (flags & kFrameKeyframe)
            {
                chunk.anchor.reset();
                m_anchor = std::make_shared<const std::vector<std::uint8_t>>(
                    chunk.bytes.begin() + static_cast<std::ptrdiff_t>(recordStart), chunk.bytes.end());
                m_stats.fullKeyframeBytes += encoded.bytes;
            }
            else
            {
                ++m_stats.relativeKeyframes;
                m_stats.relativeKeyframeBytes += encoded.bytes;
            }
        }

        m_totalBytes += encoded.bytes;
        if (m_stats.frames == 0)
            m_stats.firstTimeMs = input.timeMs;
        m_stats.lastTimeMs = input.timeMs;
        ++m_stats.frames;
        m_stats.keyframes += keyframe ? 1u : 0u;
        m_stats.bytes = m_totalBytes;
        EnforceBudget();
        return encoded;
    }

    void PoseTrack::EnforceBudget()
    {
        while (m_chunks.size() > 1 && m_totalBytes > m_config.byteBudget)
        {
            m_totalBytes -= m_chunks.front().bytes.size();
            m_chunks.pop_front();
            ++m_firstChunkSerial;
            ++m_stats.droppedChunks;
        }
        m_stats.bytes = m_totalBytes;
        if (!m_chunks.empty())
            m_stats.firstTimeMs = m_chunks.front().frameTimes.front();
    }

    std::size_t PoseTrack::DropBefore(std::uint32_t timeMs)
    {
        std::size_t dropped = 0;
        while (m_chunks.size() > 1 && !m_chunks[1].frameTimes.empty() && m_chunks[1].frameTimes.front() <= timeMs)
        {
            m_totalBytes -= m_chunks.front().bytes.size();
            m_chunks.pop_front();
            ++m_firstChunkSerial;
            ++m_stats.droppedChunks;
            ++dropped;
        }
        m_stats.bytes = m_totalBytes;
        if (!m_chunks.empty() && !m_chunks.front().frameTimes.empty())
            m_stats.firstTimeMs = m_chunks.front().frameTimes.front();
        return dropped;
    }

    bool PoseTrack::DecodeTo(
        std::size_t chunkIndex,
        std::size_t frameIndex,
        PoseDecoder& decoder,
        std::size_t& cursorFrame)
    {
        REPLAY_PROFILE_ZONE("play.pose.decode");
        const Chunk& chunk = m_chunks[chunkIndex];
        const std::uint64_t serial = m_firstChunkSerial + chunkIndex;
        std::size_t start = 0;
        // A cursor decode that starts over (backward, or a jump
        // inside the chunk) starts from the nearest checkpoint and keeps new
        // ones on the way.
        bool keepCheckpoints = false;
        if (&decoder == &m_cursor && m_cursorChunkSerial == serial &&
            cursorFrame <= frameIndex)
        {
            start = cursorFrame + 1;
        }
        else if (&decoder == &m_cursor && g_checkpointsEnabled)
        {
            keepCheckpoints = true;
            if (m_checkpointSerial != serial)
            {
                std::fill(m_checkpointValid.begin(), m_checkpointValid.end(), static_cast<std::uint8_t>(0));
                const std::size_t slots = (chunk.frameTimes.size() + kCheckpointStride - 1) / kCheckpointStride;
                if (m_checkpoints.size() < slots)
                {
                    m_checkpoints.resize(slots);
                    m_checkpointValid.resize(slots, 0);
                }
                m_checkpointSerial = serial;
            }
            decoder.Begin(*chunk.layout, m_config);
            std::size_t slot = frameIndex / kCheckpointStride + 1;
            while (slot-- > 0)
            {
                if (slot < m_checkpointValid.size() && m_checkpointValid[slot])
                    break;
            }
            if (slot != static_cast<std::size_t>(-1))
            {
                decoder.RestoreState(m_checkpoints[slot]);
                start = slot * kCheckpointStride + 1;
                ++m_checkpointRestores;
                ++g_checkpointRestoresAll;
            }
            else if (chunk.anchor)
            {
                std::size_t consumed = 0;
                if (!decoder.Apply(chunk.anchor->data(), chunk.anchor->size(), consumed))
                {
                    m_cursorChunkSerial = ~0ull;
                    return false;
                }
            }
        }
        else
        {
            decoder.Begin(*chunk.layout, m_config);
            if (chunk.anchor)
            {
                std::size_t consumed = 0;
                if (!decoder.Apply(chunk.anchor->data(), chunk.anchor->size(), consumed))
                {
                    if (&decoder == &m_cursor)
                        m_cursorChunkSerial = ~0ull;
                    return false;
                }
            }
        }

        for (std::size_t frame = start; frame <= frameIndex; ++frame)
        {
            const std::size_t offset = chunk.frameOffsets[frame];
            std::size_t consumed = 0;
            if (!decoder.Apply(chunk.bytes.data() + offset, chunk.bytes.size() - offset, consumed))
            {
                if (&decoder == &m_cursor)
                    m_cursorChunkSerial = ~0ull;
                return false;
            }
            ++g_decodedFrames;
            if (keepCheckpoints && frame % kCheckpointStride == 0)
            {
                const std::size_t slot = frame / kCheckpointStride;
                if (slot < m_checkpoints.size() && !m_checkpointValid[slot])
                {
                    decoder.SaveState(m_checkpoints[slot]);
                    m_checkpointValid[slot] = 1;
                }
            }
        }
        if (&decoder == &m_cursor)
            m_cursorChunkSerial = serial;
        cursorFrame = frameIndex;
        return true;
    }

    void PoseTrack::Materialize(
        const SkeletonLayout& layout,
        const PoseDecoder& decoder,
        std::uint64_t serial,
        std::size_t frame,
        Materialized& out)
    {
        if (out.serial == serial && out.frame == frame && out.objects.size() == layout.boneCount)
            return;
        const std::vector<BoneQts>& channels = decoder.Channels();
        out.objects.resize(layout.boneCount);
        ComposeObjectsSimd(
            layout,
            channels.data(),
            channels.data() + layout.boneCount,
            out.objects.data());
        out.serial = serial;
        out.frame = frame;
    }

    // Frame B and what the blend needs from the pair A, B. When B
    // is A plus one delta record (the next frame of the same chunk, the
    // normal play case), B's objects are A's with only the bones that record
    // reaches composed again (its local channels, its object channels and
    // their descendants): an untouched bone has A's local and A's parent
    // object, so composing it again would give A's value bit for bit. Only
    // those bones can differ, so only they are compared; every other bone
    // blends to A's value, exactly what the per-bone memcmp gave.
    void PoseTrack::PreparePair(
        const SkeletonLayout& layoutA,
        const SkeletonLayout& layoutB,
        std::uint64_t serialA,
        std::size_t frameA,
        std::uint64_t serialB,
        std::size_t frameB)
    {
        if (m_pair.serialA == serialA && m_pair.frameA == frameA &&
            m_pair.serialB == serialB && m_pair.frameB == frameB &&
            m_poseB.serial == serialB && m_poseB.frame == frameB)
        {
            return;
        }
        m_pair.serialA = serialA;
        m_pair.frameA = frameA;
        m_pair.serialB = serialB;
        m_pair.frameB = frameB;
        m_pair.locals.clear();
        m_pair.objects.clear();
        m_pair.rebase = false;
        const std::uint32_t bones = layoutA.boneCount;
        m_pair.sameBones = layoutB.boneCount == bones;

        const BoneQts* localsA = m_cursor.Channels().data();
        const BoneQts* localsB = m_next.Channels().data();
        const bool incremental = m_pair.sameBones && &layoutA == &layoutB && serialA == serialB &&
            frameB == frameA + 1 && m_touchedSerial == serialB && m_touchedFrame == frameB &&
            !(m_next.Flags() & kFrameKeyframe) &&
            m_poseA.serial == serialA && m_poseA.frame == frameA && m_poseA.objects.size() == bones &&
            !(m_poseB.serial == serialB && m_poseB.frame == frameB);
        if (incremental)
        {
            const SkeletonLayout& layout = layoutB;
            const std::vector<BoneQts>& channels = m_next.Channels();
            m_poseB.objects.assign(m_poseA.objects.begin(), m_poseA.objects.end());
            BoneQts* objects = m_poseB.objects.data();
            // Bit 0: the local was written; bit 1: the object may differ.
            m_dirty.assign(bones, 0);
            std::uint8_t* dirty = m_dirty.data();
            for (const std::uint32_t channel : m_touched)
            {
                if (channel < bones)
                {
                    dirty[channel] |= 3;
                }
                else
                {
                    const std::uint32_t bone = layout.unresolvedBones[channel - bones];
                    objects[bone] = channels[channel];
                    dirty[bone] |= 2;
                }
            }
            for (const std::uint32_t bone : layout.evaluationOrder)
            {
                const std::int32_t parent = layout.parent[bone];
                if (parent == kRootParent)
                {
                    if (dirty[bone] & 2)
                        objects[bone] = localsB[bone];
                }
                else if (parent >= 0 && ((dirty[bone] | dirty[static_cast<std::uint32_t>(parent)]) & 2))
                {
                    ComposeSimd(objects[static_cast<std::uint32_t>(parent)], localsB[bone], layout.order,
                        layout.inheritScale, objects[bone]);
                    dirty[bone] |= 2;
                }
            }
            m_poseB.serial = serialB;
            m_poseB.frame = frameB;
            ++g_sampleWork.incrementalPairs;
            const BoneQts* objectsA = m_poseA.objects.data();
            for (std::uint32_t bone = 0; bone < bones; ++bone)
            {
                const std::uint8_t flags = dirty[bone];
                if (!flags)
                    continue;
                ++g_sampleWork.reachedBones;
                if ((flags & 1) && std::memcmp(&localsA[bone], &localsB[bone], sizeof(BoneQts)) != 0)
                    m_pair.locals.push_back(bone);
                if ((flags & 2) && std::memcmp(&objectsA[bone], &objects[bone], sizeof(BoneQts)) != 0)
                    m_pair.objects.push_back(bone);
            }
        }
        else
        {
            Materialize(layoutB, m_next, serialB, frameB, m_poseB);
            if (m_pair.sameBones)
            {
                const BoneQts* objectsA = m_poseA.objects.data();
                const BoneQts* objectsB = m_poseB.objects.data();
                for (std::uint32_t bone = 0; bone < bones; ++bone)
                {
                    if (std::memcmp(&localsA[bone], &localsB[bone], sizeof(BoneQts)) != 0)
                        m_pair.locals.push_back(bone);
                    if (std::memcmp(&objectsA[bone], &objectsB[bone], sizeof(BoneQts)) != 0)
                        m_pair.objects.push_back(bone);
                }
            }
        }

        ++g_sampleWork.pairs;
        g_sampleWork.pairBones += bones;
        g_sampleWork.blendLocals += m_pair.locals.size();
        g_sampleWork.blendObjects += m_pair.objects.size();

        // A rebase the recording did not flag (older clips, or any the flag
        // missed): hold frame A, as for a
        // flagged one. The caller blends the matrix with the same t.
        WorldTransform worldA{};
        WorldTransform worldB{};
        m_pair.rebase = !m_poseA.objects.empty() && !m_poseB.objects.empty() &&
            DecodeWorldMatrix(m_cursor.World(), worldA) && DecodeWorldMatrix(m_next.World(), worldB) &&
            IsRebase(MakeRebaseProbe(worldA, m_poseA.objects[0], m_cursor.TimeMs()),
                MakeRebaseProbe(worldB, m_poseB.objects[0], m_next.TimeMs()));
    }

    Vec3 RootWorldPosition(const WorldTransform& world, const BoneQts& rootObject)
    {
        const Vec3 local{rootObject.translation[0], rootObject.translation[1],
            rootObject.translation[2]};
        const Vec3 rotated = Rotate(world.rotation, local);
        return {world.position.x + rotated.x, world.position.y + rotated.y,
            world.position.z + rotated.z};
    }

    bool IsRebase(const RebaseProbe& previous, const RebaseProbe& current)
    {
        if (current.timeMs <= previous.timeMs ||
            current.timeMs - previous.timeMs > kRebaseMaxGapMs)
        {
            return false;
        }
        const auto distance = [](const Vec3& a, const Vec3& b) {
            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            const float dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        const float origin = distance(previous.world.position, current.world.position);
        const float root = distance(previous.root, current.root);
        if (origin >= kTeleportDistanceM || origin - root >= kRebaseOriginExcessM)
            return true;
        // A heading rebase (the entity turned, the root did not).
        if (previous.hasRootRotation && current.hasRootRotation)
        {
            const float entityTurn = AngleBetween(previous.world.rotation, current.world.rotation);
            const float rootTurn = AngleBetween(previous.rootRotation, current.rootRotation);
            if (entityTurn - rootTurn >= kRebaseRotationExcessRad)
                return true;
        }
        return false;
    }

    RebaseProbe MakeRebaseProbe(const WorldTransform& world, const BoneQts& rootObject, std::uint32_t timeMs)
    {
        RebaseProbe probe;
        probe.world = world;
        probe.root = RootWorldPosition(world, rootObject);
        probe.rootRotation = Normalize(Multiply(world.rotation, rootObject.rotation));
        probe.hasRootRotation = true;
        probe.timeMs = timeMs;
        return probe;
    }

    const SkeletonLayout& PoseTrack::LayoutAt(std::uint32_t timeMs) const
    {
        for (std::size_t index = m_chunks.size(); index-- > 0;)
        {
            const Chunk& chunk = m_chunks[index];
            if ((index == 0 || (!chunk.frameTimes.empty() && chunk.frameTimes.front() <= timeMs)) && chunk.layout)
                return *chunk.layout;
        }
        return *m_layout;
    }

    std::uint32_t PoseTrack::RebuiltBonesAt(std::uint32_t timeMs, std::vector<std::uint8_t>& rebuilt) const
    {
        const SkeletonLayout& current = LayoutAt(timeMs);
        rebuilt.assign(current.parent.size(), 0);
        std::uint32_t count = 0;
        const SkeletonLayout* seen = nullptr;
        for (const Chunk& chunk : m_chunks)
        {
            const SkeletonLayout* layout = chunk.layout.get();
            if (!layout || layout == &current)
                break; // the chunks before the one playing timeMs
            if (layout == seen || layout->parent.size() != current.parent.size())
                continue;
            seen = layout;
            for (std::size_t bone = 0; bone < current.parent.size(); ++bone)
            {
                if (!rebuilt[bone] && current.parent[bone] == kUnresolvedParent && layout->parent[bone] != kUnresolvedParent)
                {
                    rebuilt[bone] = 1;
                    ++count;
                }
            }
        }
        return count;
    }

    bool PoseTrack::Sample(std::uint32_t timeMs, PoseSample& sample)
    {
        if (m_chunks.empty())
            return false;
        timeMs = std::clamp(timeMs, m_stats.firstTimeMs, m_stats.lastTimeMs);

        // Last chunk whose first frame is at or before timeMs.
        std::size_t chunkIndex = 0;
        for (std::size_t index = m_chunks.size(); index-- > 0;)
        {
            if (m_chunks[index].frameTimes.front() <= timeMs)
            {
                chunkIndex = index;
                break;
            }
        }
        const Chunk& chunk = m_chunks[chunkIndex];
        const auto upper =
            std::upper_bound(chunk.frameTimes.begin(), chunk.frameTimes.end(), timeMs);
        const std::size_t frameIndex =
            static_cast<std::size_t>(upper - chunk.frameTimes.begin()) - 1u;
        const std::uint64_t serial = m_firstChunkSerial + chunkIndex;
        // The checkpoints of a chunk the playhead left behind by
        // two chunks (normal play went on) are freed.
        if (m_checkpointSerial != ~0ull && serial > m_checkpointSerial + 1)
            ResetCheckpoints(true);

        // Frame A: the cached next frame when the playhead moved into it,
        // else the sequential decode from the cursor or the chunk keyframe.
        if (m_cursorChunkSerial != serial || m_cursorFrame != frameIndex)
        {
            if (m_nextChunkSerial == serial && m_nextFrame == frameIndex)
            {
                // The decoder swapped out keeps the old cursor frame.
                m_nextStateSerial = m_cursorChunkSerial;
                m_nextStateFrame = m_cursorFrame;
                std::swap(m_cursor, m_next);
                std::swap(m_poseA, m_poseB);
                m_cursorChunkSerial = serial;
                m_cursorFrame = frameIndex;
                m_nextChunkSerial = ~0ull;
            }
            else
            {
                m_nextChunkSerial = ~0ull;
                if (!DecodeTo(chunkIndex, frameIndex, m_cursor, m_cursorFrame))
                    return false;
            }
        }

        // Frame after A: next record in this chunk, or the next keyframe.
        bool hasNext = false;
        const SkeletonLayout* nextLayout = chunk.layout.get();
        std::uint64_t nextSerial = serial;
        std::size_t nextFrame = frameIndex + 1;
        if (frameIndex + 1 >= chunk.frameTimes.size())
        {
            nextSerial = serial + 1;
            nextFrame = 0;
        }
        const bool nextExists = frameIndex + 1 < chunk.frameTimes.size() ||
            chunkIndex + 1 < m_chunks.size();
        if (nextExists && m_nextChunkSerial == nextSerial && m_nextFrame == nextFrame)
        {
            hasNext = true;
            if (nextSerial != serial)
                nextLayout = m_chunks[chunkIndex + 1].layout.get();
        }
        else if (frameIndex + 1 < chunk.frameTimes.size())
        {
            REPLAY_PROFILE_ZONE("play.pose.next");
            // The next decoder still holding the frame before A
            // (after the swap) reaches B with A's record and B's; else it
            // starts from a copy of the cursor. Same state either way: a
            // frame's state only depends on the records up to it.
            std::size_t consumed = 0;
            bool fromCursor = true;
            if (frameIndex > 0 && m_nextStateSerial == serial && m_nextStateFrame + 1 == frameIndex)
            {
                const std::size_t own = chunk.frameOffsets[frameIndex];
                fromCursor = !m_next.Apply(chunk.bytes.data() + own, chunk.bytes.size() - own, consumed);
            }
            if (fromCursor)
                m_next = m_cursor;
            ++(fromCursor ? g_sampleWork.nextCopied : g_sampleWork.nextAdvanced);
            const std::size_t offset = chunk.frameOffsets[frameIndex + 1];
            hasNext = m_next.Apply(
                chunk.bytes.data() + offset, chunk.bytes.size() - offset, consumed, &m_touched);
            m_touchedSerial = hasNext ? serial : ~0ull;
            m_touchedFrame = frameIndex + 1;
            m_nextStateSerial = hasNext ? serial : ~0ull;
            m_nextStateFrame = frameIndex + 1;
        }
        else if (chunkIndex + 1 < m_chunks.size())
        {
            std::size_t unused = 0;
            nextLayout = m_chunks[chunkIndex + 1].layout.get();
            hasNext = DecodeTo(chunkIndex + 1, 0, m_next, unused);
            m_nextStateSerial = hasNext ? serial + 1 : ~0ull;
            m_nextStateFrame = 0;
        }
        if (hasNext)
        {
            m_nextChunkSerial = nextSerial;
            m_nextFrame = nextFrame;
        }
        else
        {
            m_nextChunkSerial = ~0ull;
        }

        float t = 0.0f;
        if (hasNext && !(m_next.Flags() & kFrameNoInterpolation) &&
            m_next.TimeMs() > m_cursor.TimeMs())
        {
            t = static_cast<float>(timeMs - m_cursor.TimeMs()) /
                static_cast<float>(m_next.TimeMs() - m_cursor.TimeMs());
        }

        // Each side is rebuilt with its own layout, then blended per bone,
        // so a layout change between chunks never mixes channel meanings.
        {
            REPLAY_PROFILE_ZONE("play.pose.materialize");
            Materialize(*chunk.layout, m_cursor, serial, frameIndex, m_poseA);
            if (t > 0.0f)
            {
                PreparePair(*chunk.layout, *nextLayout, serial, frameIndex, nextSerial, nextFrame);
                if (m_pair.rebase)
                {
                    t = 0.0f;
                    ++m_unflaggedRebaseHolds;
                }
                else if (!m_pair.sameBones)
                {
                    t = 0.0f; // never blend two bone counts
                }
            }
        }

        {
            REPLAY_PROFILE_ZONE("play.pose.blend");
            // A copied once, then only the bones that differ in
            // the pair are interpolated (the others blended to A's value).
            const std::size_t bones = chunk.layout->boneCount;
            sample.locals.resize(bones);
            sample.objects.resize(bones);
            std::memcpy(sample.locals.data(), m_cursor.Channels().data(), bones * sizeof(BoneQts));
            std::memcpy(sample.objects.data(), m_poseA.objects.data(), bones * sizeof(BoneQts));
            if (t > 0.0f)
            {
                const BoneQts* localsA = m_cursor.Channels().data();
                const BoneQts* localsB = m_next.Channels().data();
                for (const std::uint32_t bone : m_pair.locals)
                    InterpolateSimd(localsA[bone], localsB[bone], t, sample.locals[bone]);
                const BoneQts* objectsA = m_poseA.objects.data();
                const BoneQts* objectsB = m_poseB.objects.data();
                for (const std::uint32_t bone : m_pair.objects)
                    InterpolateSimd(objectsA[bone], objectsB[bone], t, sample.objects[bone]);
            }
        }

        std::memcpy(sample.worldA, m_cursor.World(), sizeof(sample.worldA));
        std::memcpy(
            sample.worldB,
            hasNext ? m_next.World() : m_cursor.World(),
            sizeof(sample.worldB));
        sample.t = t;
        sample.frameTimeA = m_cursor.TimeMs();
        sample.frameTimeB = hasNext ? m_next.TimeMs() : m_cursor.TimeMs();
        return true;
    }

    void PoseTrack::Export(TrackExport& out) const
    {
        out = {};
        out.config = m_config;
        out.stats = m_stats;
        std::vector<const SkeletonLayout*> layouts;
        std::vector<const std::vector<std::uint8_t>*> anchors;
        const auto layoutIndex = [&](const SkeletonLayout* layout) {
            for (std::size_t index = 0; index < layouts.size(); ++index)
            {
                if (layouts[index] == layout)
                    return static_cast<std::uint32_t>(index);
            }
            layouts.push_back(layout);
            out.layouts.push_back(*layout);
            return static_cast<std::uint32_t>(layouts.size() - 1);
        };
        for (const Chunk& chunk : m_chunks)
        {
            TrackChunkExport exported;
            exported.layout = layoutIndex(chunk.layout.get());
            if (chunk.anchor)
            {
                std::size_t found = anchors.size();
                for (std::size_t index = 0; index < anchors.size(); ++index)
                {
                    if (anchors[index] == chunk.anchor.get())
                        found = index;
                }
                if (found == anchors.size())
                {
                    anchors.push_back(chunk.anchor.get());
                    out.anchors.push_back(*chunk.anchor);
                }
                exported.anchor = static_cast<std::int32_t>(found);
            }
            exported.bytes = chunk.bytes;
            exported.frameTimes = chunk.frameTimes;
            exported.frameOffsets = chunk.frameOffsets;
            out.chunks.push_back(std::move(exported));
        }
        if (m_layout)
            out.currentLayout = layoutIndex(m_layout.get());
    }

    namespace
    {
        bool LayoutUsable(const SkeletonLayout& layout)
        {
            const std::size_t bones = layout.boneCount;
            if (bones == 0 || bones > 4096 || layout.parent.size() != bones ||
                layout.ambiguous.size() != bones || layout.highPrecision.size() != bones ||
                layout.evaluationOrder.size() != bones || layout.unresolvedBones.size() > bones)
            {
                return false;
            }
            for (const std::int32_t parent : layout.parent)
            {
                if (parent < kUnresolvedParent || parent >= static_cast<std::int32_t>(bones))
                    return false;
            }
            for (const std::uint32_t bone : layout.evaluationOrder)
            {
                if (bone >= bones)
                    return false;
            }
            for (const std::uint32_t bone : layout.unresolvedBones)
            {
                if (bone >= bones)
                    return false;
            }
            return true;
        }
    }

    bool PoseTrack::Import(const TrackExport& in, std::string& error)
    {
        if (in.layouts.empty() || in.currentLayout >= in.layouts.size())
        {
            error = "no_layout";
            return false;
        }
        std::vector<std::shared_ptr<const SkeletonLayout>> layouts;
        for (const SkeletonLayout& layout : in.layouts)
        {
            if (!LayoutUsable(layout))
            {
                error = "bad_layout";
                return false;
            }
            layouts.push_back(std::make_shared<const SkeletonLayout>(layout));
        }
        std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> anchors;
        for (const std::vector<std::uint8_t>& anchor : in.anchors)
            anchors.push_back(std::make_shared<const std::vector<std::uint8_t>>(anchor));

        std::deque<Chunk> chunks;
        std::size_t totalBytes = 0;
        PoseDecoder decoder;
        for (const TrackChunkExport& exported : in.chunks)
        {
            if (exported.layout >= layouts.size() ||
                exported.anchor >= static_cast<std::int32_t>(anchors.size()) || exported.anchor < -1 ||
                exported.frameTimes.empty() || exported.frameTimes.size() != exported.frameOffsets.size())
            {
                error = "bad_chunk";
                return false;
            }
            if (!chunks.empty() && exported.frameTimes.front() < chunks.back().frameTimes.back())
            {
                error = "chunk_time_order";
                return false;
            }
            // Decode the chunk once: every record must parse, start at its
            // offset, end where the next begins and carry its frame time.
            decoder.Begin(*layouts[exported.layout], in.config);
            if (exported.anchor >= 0)
            {
                const std::vector<std::uint8_t>& anchor = *anchors[static_cast<std::size_t>(exported.anchor)];
                std::size_t consumed = 0;
                if (anchor.empty() || !decoder.Apply(anchor.data(), anchor.size(), consumed) ||
                    consumed != anchor.size())
                {
                    error = "bad_anchor";
                    return false;
                }
            }
            for (std::size_t frame = 0; frame < exported.frameOffsets.size(); ++frame)
            {
                const std::size_t offset = exported.frameOffsets[frame];
                const std::size_t end = frame + 1 < exported.frameOffsets.size()
                    ? exported.frameOffsets[frame + 1]
                    : exported.bytes.size();
                std::size_t consumed = 0;
                if (offset >= end || end > exported.bytes.size() ||
                    (frame > 0 && exported.frameTimes[frame] < exported.frameTimes[frame - 1]) ||
                    !decoder.Apply(exported.bytes.data() + offset, end - offset, consumed) ||
                    offset + consumed != end || decoder.TimeMs() != exported.frameTimes[frame])
                {
                    error = "bad_frame";
                    return false;
                }
            }
            Chunk chunk;
            chunk.layout = layouts[exported.layout];
            if (exported.anchor >= 0)
                chunk.anchor = anchors[static_cast<std::size_t>(exported.anchor)];
            chunk.bytes = exported.bytes;
            chunk.frameTimes = exported.frameTimes;
            chunk.frameOffsets = exported.frameOffsets;
            totalBytes += chunk.bytes.size();
            chunks.push_back(std::move(chunk));
        }

        Begin(*layouts[in.currentLayout], in.config);
        m_layout = layouts[in.currentLayout];
        m_chunks = std::move(chunks);
        m_totalBytes = totalBytes;
        m_stats = in.stats;
        m_stats.bytes = m_totalBytes;
        if (!m_chunks.empty())
        {
            m_stats.firstTimeMs = m_chunks.front().frameTimes.front();
            m_stats.lastTimeMs = m_chunks.back().frameTimes.back();
        }
        // Playback only, so sealed: no encoder state and no decoder channels
        // until the first Sample. A later Append Begins the encoder on
        // m_layout and starts a new chunk with a full keyframe.
        Seal();
        return true;
    }
}
