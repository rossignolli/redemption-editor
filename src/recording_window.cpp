#include "recording_window.h"
#include "speech_resume.h"
#include "decal_state.h"
#include "world_state.h"
#include "object_check.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <type_traits>
#include <utility>

// Private clip members (lifecycle blocks, segment counter, transform and
// time scale samples, rope records, camera keyframes) are reached the same
// way clip_file.cpp reaches them: explicit template instantiation names a
// private member without access checking. Only this file uses these tags,
// and only to copy a whole member in or out.
namespace replay::window::access
{
    template <typename Tag, auto Member>
    struct Steal
    {
        template <typename Object, typename Visit>
        friend void With(Tag, Object& object, Visit&& visit)
        {
            visit(object.*Member);
        }
    };

#define REPLAY_WINDOW_MEMBER(TagName, Class, Member)                \
    struct TagName                                                  \
    {                                                               \
        template <typename Object, typename Visit>                  \
        friend void With(TagName, Object& object, Visit&& visit);   \
    };                                                              \
    template struct Steal<TagName, &Class::Member>;

    REPLAY_WINDOW_MEMBER(LifeBlockMs, entity::LifecycleTrack, m_blockMs)
    REPLAY_WINDOW_MEMBER(LifeLive, entity::LifecycleTrack, m_live)
    REPLAY_WINDOW_MEMBER(LifeLiveAttachments, entity::LifecycleTrack, m_liveAttachments)
    REPLAY_WINDOW_MEMBER(LifeBlocks, entity::LifecycleTrack, m_blocks)
    REPLAY_WINDOW_MEMBER(LifeStats, entity::LifecycleTrack, m_stats)
    REPLAY_WINDOW_MEMBER(SegmentsNext, entity::PoseSegmentTrack, m_next)
    REPLAY_WINDOW_MEMBER(RelativeInterval, entity::RelativeTrack, m_intervalMs)
    REPLAY_WINDOW_MEMBER(RelativeSamples, entity::RelativeTrack, m_samples)
    REPLAY_WINDOW_MEMBER(RelativeDropped, entity::RelativeTrack, m_dropped)
    REPLAY_WINDOW_MEMBER(RopeRecords, ropes::Ropes, m_records)
    REPLAY_WINDOW_MEMBER(TimeScaleSamples, timescale::Track, m_samples)
    REPLAY_WINDOW_MEMBER(CameraKeyframes, camera::Track, keyframes_)
    REPLAY_WINDOW_MEMBER(CameraNextId, camera::Track, nextId_)

#undef REPLAY_WINDOW_MEMBER

    template <typename T, typename Tag, typename Object>
    T& Ref(Object& object)
    {
        T* member = nullptr;
        With(Tag{}, object, [&](T& value) { member = &value; });
        return *member;
    }
}

namespace replay::window
{
    namespace
    {
        using entity::ReplayId;

        // The recorder's initial decals sit at 1 ms and every forward cursor
        // plays (from, to]: state a forward cursor plays is folded here.
        constexpr std::uint32_t kFoldForwardMs = 1;

        struct Window
        {
            std::uint32_t base = 0;
            std::uint32_t end = 0;

            // Output time of an input time at or after base; before base: 0.
            std::uint32_t Clamp(std::uint32_t timeMs) const { return timeMs < base ? 0u : timeMs - base; }
            // Output time of a state event folded to `fold` (no earlier).
            std::uint32_t Fold(std::uint32_t timeMs, std::uint32_t fold) const
            {
                const std::uint32_t shifted = Clamp(timeMs);
                return shifted < fold ? fold : shifted;
            }
            // How much later (ms) an input event is placed by Fold.
            std::uint32_t FoldGap(std::uint32_t timeMs, std::uint32_t fold) const
            {
                const std::uint32_t placed = base + Fold(timeMs, fold);
                return placed > timeMs ? placed - timeMs : 0u;
            }
        };

        using KeptSet = std::set<std::uint32_t>;

        bool Kept(const KeptSet& kept, ReplayId id) { return kept.count(id.Packed()) != 0; }
        // An invalid id (a world decal, no entity) always stays.
        bool KeptOrNone(const KeptSet& kept, ReplayId id) { return !id.Valid() || Kept(kept, id); }

        template <typename Event>
        using Events = entity::TimedEvents<Event>;

        // One-shot: only the events inside [base, end], shifted.
        template <typename Event>
        void CutOneShot(const Events<Event>& in, Events<Event>& out, const Window& w)
        {
            out.Begin();
            for (const Event& event : in.Events())
            {
                if (event.timeMs > w.end)
                    break;
                if (event.timeMs < w.base)
                    continue;
                Event copy = event;
                copy.timeMs = event.timeMs - w.base;
                out.Add(copy);
            }
        }

        // State: the events before base are folded to `fold` in their order
        // (`keep` filters, `adjust` sees the copy and the fold gap in ms).
        template <typename Event, typename Keep, typename Adjust>
        void CutState(const Events<Event>& in, Events<Event>& out, const Window& w, std::uint32_t fold,
            Keep keep, Adjust adjust)
        {
            out.Begin();
            const std::vector<Event>& events = in.Events();
            for (std::size_t index = 0; index < events.size(); ++index)
            {
                const Event& event = events[index];
                if (event.timeMs > w.end)
                    break;
                if (!keep(index, event))
                    continue;
                Event copy = event;
                copy.timeMs = w.Fold(event.timeMs, fold);
                adjust(copy, w.FoldGap(event.timeMs, fold));
                out.Add(copy);
            }
        }

        // Continuous samples: from the last one at or before base (placed
        // at 0) up to end. Empty when none is left.
        template <typename Sample, typename TimeOf>
        std::pair<std::size_t, std::size_t> SampleSpan(const std::vector<Sample>& samples, const Window& w, TimeOf timeOf)
        {
            std::size_t first = 0;
            for (std::size_t index = 0; index < samples.size(); ++index)
            {
                if (timeOf(samples[index]) <= w.base)
                    first = index;
                else
                    break;
            }
            std::size_t last = first;
            while (last < samples.size() && timeOf(samples[last]) <= w.end)
                ++last;
            return {first, last};
        }

        template <typename Sample, typename TimeRef>
        std::vector<Sample> CutSamples(const std::vector<Sample>& samples, const Window& w, TimeRef timeRef)
        {
            const auto span = SampleSpan(samples, w, [&](const Sample& s) { return timeRef(const_cast<Sample&>(s)); });
            std::vector<Sample> out(samples.begin() + static_cast<std::ptrdiff_t>(span.first),
                samples.begin() + static_cast<std::ptrdiff_t>(span.second));
            for (Sample& sample : out)
                timeRef(sample) = w.Clamp(timeRef(sample));
            return out;
        }

        // Last-wins state per key: the last event at or before base per key
        // (at 0, in their order) and the ones in (base, end], shifted.
        template <typename Item, typename TimeRef, typename KeyOf>
        std::vector<Item> CutLastPerKey(const std::vector<Item>& items, const Window& w, TimeRef timeRef, KeyOf keyOf)
        {
            std::map<std::uint64_t, std::size_t> lastBefore;
            for (std::size_t index = 0; index < items.size(); ++index)
            {
                if (timeRef(const_cast<Item&>(items[index])) <= w.base)
                    lastBefore[keyOf(items[index])] = index;
            }
            std::vector<Item> out;
            for (std::size_t index = 0; index < items.size(); ++index)
            {
                const std::uint32_t time = timeRef(const_cast<Item&>(items[index]));
                if (time > w.end)
                    continue;
                if (time <= w.base)
                {
                    const auto found = lastBefore.find(keyOf(items[index]));
                    if (found == lastBefore.end() || found->second != index)
                        continue;
                }
                Item copy = items[index];
                timeRef(copy) = w.Clamp(time);
                out.push_back(std::move(copy));
            }
            // The folded ones (all at 0) first; stable keeps their order.
            std::stable_sort(out.begin(), out.end(), [&](const Item& a, const Item& b) {
                return timeRef(const_cast<Item&>(a)) < timeRef(const_cast<Item&>(b));
            });
            return out;
        }

        std::uint32_t ReadU32(const std::uint8_t* data)
        {
            std::uint32_t value = 0;
            std::memcpy(&value, data, sizeof(value));
            return value;
        }

        void WriteU32(std::uint8_t* data, std::uint32_t value) { std::memcpy(data, &value, sizeof(value)); }

        // Pose record header: u32 recordSize, u32 timeMs, u16 flags, ...
        constexpr std::size_t kRecordTimeOffset = sizeof(std::uint32_t);
        constexpr std::size_t kRecordHeaderMin = sizeof(std::uint32_t) * 2 + sizeof(std::uint16_t) * 2;

        bool RewriteRecordTime(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t expected,
            std::uint32_t timeMs)
        {
            if (offset + kRecordHeaderMin > bytes.size())
                return false;
            const std::uint32_t size = ReadU32(bytes.data() + offset);
            if (size < kRecordHeaderMin || offset + size > bytes.size())
                return false;
            if (ReadU32(bytes.data() + offset + kRecordTimeOffset) != expected)
                return false;
            WriteU32(bytes.data() + offset + kRecordTimeOffset, timeMs);
            return true;
        }

        // One pose record cut to the window; false when the record is
        // malformed, `keep` false when nothing is left of it.
        bool CutPoseRecord(const pose::PoseRecordExport& in, const Window& w, pose::PoseRecordExport& out, bool& keep,
            Result& result)
        {
            keep = false;
            const std::vector<pose::TrackChunkExport>& chunks = in.track.chunks;
            if (chunks.empty() || chunks.back().frameTimes.empty() || chunks.back().frameTimes.back() < w.base)
                return true; // ended before the window
            std::size_t first = 0;
            for (std::size_t index = 0; index < chunks.size(); ++index)
            {
                if (!chunks[index].frameTimes.empty() && chunks[index].frameTimes.front() <= w.base)
                    first = index;
            }
            out = pose::PoseRecordExport{};
            out.recordId = in.recordId;
            out.boneCount = in.boneCount;
            out.track.config = in.track.config;
            out.track.layouts = in.track.layouts;
            out.track.currentLayout = in.track.currentLayout;
            std::map<std::int32_t, std::int32_t> anchorMap;
            pose::TrackStats& stats = out.track.stats;
            stats = pose::TrackStats{};
            for (std::size_t index = first; index < chunks.size(); ++index)
            {
                const pose::TrackChunkExport& source = chunks[index];
                if (source.frameTimes.empty() || source.frameTimes.size() != source.frameOffsets.size())
                {
                    result.error = "pose record " + std::to_string(in.recordId) + ": bad chunk";
                    return false;
                }
                if (source.frameTimes.front() > w.end)
                    break;
                pose::TrackChunkExport chunk = source;
                // Frames after end: cut the tail at the first one.
                std::size_t count = chunk.frameTimes.size();
                while (count > 0 && chunk.frameTimes[count - 1] > w.end)
                    --count;
                if (count < chunk.frameTimes.size())
                {
                    result.poseFramesTruncated += chunk.frameTimes.size() - count;
                    chunk.bytes.resize(chunk.frameOffsets[count]);
                    chunk.frameTimes.resize(count);
                    chunk.frameOffsets.resize(count);
                }
                for (std::size_t frame = 0; frame < chunk.frameTimes.size(); ++frame)
                {
                    const std::uint32_t original = chunk.frameTimes[frame];
                    const std::uint32_t shifted = w.Clamp(original);
                    if (original < w.base)
                        ++result.poseFramesClamped;
                    if (!RewriteRecordTime(chunk.bytes.Mutable(), chunk.frameOffsets[frame], original, shifted))
                    {
                        result.error = "pose record " + std::to_string(in.recordId) + ": frame header";
                        return false;
                    }
                    chunk.frameTimes[frame] = shifted;
                }
                if (chunk.anchor >= 0)
                {
                    const auto found = anchorMap.find(chunk.anchor);
                    if (found != anchorMap.end())
                        chunk.anchor = found->second;
                    else
                    {
                        if (static_cast<std::size_t>(chunk.anchor) >= in.track.anchors.size())
                        {
                            result.error = "pose record " + std::to_string(in.recordId) + ": anchor index";
                            return false;
                        }
                        std::vector<std::uint8_t> anchor = in.track.anchors[static_cast<std::size_t>(chunk.anchor)];
                        if (anchor.size() < kRecordHeaderMin ||
                            !RewriteRecordTime(anchor, 0, ReadU32(anchor.data() + kRecordTimeOffset),
                                w.Clamp(ReadU32(anchor.data() + kRecordTimeOffset))))
                        {
                            result.error = "pose record " + std::to_string(in.recordId) + ": anchor header";
                            return false;
                        }
                        const std::int32_t next = static_cast<std::int32_t>(out.track.anchors.size());
                        anchorMap[chunk.anchor] = next;
                        out.track.anchors.push_back(std::move(anchor));
                        chunk.anchor = next;
                    }
                }
                // Stats as the track would count these chunks.
                stats.frames += chunk.frameTimes.size();
                ++stats.keyframes;
                const std::uint32_t keySize = ReadU32(chunk.bytes.data());
                if (chunk.anchor >= 0)
                {
                    ++stats.relativeKeyframes;
                    stats.relativeKeyframeBytes += keySize;
                }
                else
                    stats.fullKeyframeBytes += keySize;
                stats.bytes += chunk.bytes.size();
                out.track.chunks.push_back(std::move(chunk));
            }
            if (out.track.chunks.empty())
                return true;
            stats.droppedChunks = in.track.stats.droppedChunks + (chunks.size() - out.track.chunks.size());
            stats.firstTimeMs = out.track.chunks.front().frameTimes.front();
            stats.lastTimeMs = out.track.chunks.back().frameTimes.back();
            keep = true;
            return true;
        }

        // The pose segment each segment record belongs to, and whether it
        // survives: dropped with its entity, when a later segment of the
        // same entity starts at or before base, or when it starts after end.
        void CutSegments(const Recording& in, const Window& w, const KeptSet& kept, Recording& out,
            std::set<std::uint32_t>& droppedRecords)
        {
            const std::vector<entity::PoseSegment>& segments = in.segments.Segments();
            out.segments.Begin();
            for (std::size_t index = 0; index < segments.size(); ++index)
            {
                const entity::PoseSegment& segment = segments[index];
                bool superseded = false;
                for (std::size_t later = index + 1; later < segments.size(); ++later)
                {
                    if (segments[later].entity == segment.entity && segments[later].startMs <= w.base)
                        superseded = true;
                }
                if (!Kept(kept, segment.entity) || superseded || segment.startMs > w.end)
                {
                    droppedRecords.insert(segment.recordId);
                    continue;
                }
                entity::PoseSegment copy = segment;
                copy.startMs = w.Clamp(segment.startMs);
                out.segments.Add(copy);
            }
            auto& segmentsIn = const_cast<entity::PoseSegmentTrack&>(in.segments);
            access::Ref<std::uint32_t, access::SegmentsNext>(out.segments) =
                access::Ref<std::uint32_t, access::SegmentsNext>(segmentsIn);
        }

        void RebaseCreate(entity::CreatePacket& packet, const Window& w) { packet.timeMs = w.Clamp(packet.timeMs); }

        // Lifecycle blocks from the base block to end; fills the kept ids.
        void CutLifecycle(const Recording& in, const Window& w, Recording& out, KeptSet& kept)
        {
            auto& source = const_cast<entity::LifecycleTrack&>(in.lifecycle);
            const std::uint32_t blockMs = access::Ref<std::uint32_t, access::LifeBlockMs>(source);
            out.lifecycle.Begin(blockMs);
            entity::LifecycleStats stats;
            With(access::LifeBlocks{}, source, [&](const auto& blocks) {
                With(access::LifeBlocks{}, out.lifecycle, [&](auto& outBlocks) {
                    using Block = typename std::decay_t<decltype(outBlocks)>::value_type;
                    for (const auto& block : blocks)
                    {
                        if (block.startMs < w.base || block.startMs > w.end)
                            continue;
                        Block copy;
                        copy.startMs = block.startMs - w.base;
                        copy.fullCreates = block.fullCreates;
                        for (entity::CreatePacket& packet : copy.fullCreates)
                        {
                            RebaseCreate(packet, w);
                            kept.insert(packet.id.Packed());
                        }
                        for (const auto& event : block.events)
                        {
                            if (event.timeMs > w.end)
                                break;
                            auto rebased = event;
                            rebased.timeMs = w.Clamp(event.timeMs);
                            RebaseCreate(rebased.packet, w);
                            kept.insert(rebased.packet.id.Packed());
                            (rebased.create ? stats.creates : stats.deletes) += 1;
                            copy.events.push_back(std::move(rebased));
                        }
                        copy.fullAttachments = block.fullAttachments;
                        for (const auto& event : block.attachEvents)
                        {
                            if (event.timeMs > w.end)
                                break;
                            auto rebased = event;
                            rebased.timeMs = w.Clamp(event.timeMs);
                            (rebased.attach ? stats.attaches : stats.detaches) += 1;
                            copy.attachEvents.push_back(rebased);
                        }
                        ++stats.blocks;
                        stats.fullCreates += copy.fullCreates.size();
                        stats.fullAttachments += copy.fullAttachments.size();
                        outBlocks.push_back(std::move(copy));
                    }
                });
            });
            // What a recorder would hold live at the end of the window.
            const std::uint32_t endOut = w.end - w.base;
            std::vector<entity::CreatePacket> live;
            out.lifecycle.AliveAt(endOut, live);
            std::vector<entity::Attachment> attachments;
            out.lifecycle.AttachmentsAt(endOut, attachments);
            access::Ref<std::vector<entity::CreatePacket>, access::LifeLive>(out.lifecycle) = std::move(live);
            access::Ref<std::vector<entity::Attachment>, access::LifeLiveAttachments>(out.lifecycle) = std::move(attachments);
            access::Ref<entity::LifecycleStats, access::LifeStats>(out.lifecycle) = stats;
        }

        // Decal adds and removes both before base that match for certain
        // (see recording_window.h note b): their engine ids.
        std::set<std::uint32_t> DroppableDecalPairs(const Recording& in, const Window& w)
        {
            std::map<std::uint32_t, std::uint32_t> adds;     // engineId -> count
            std::map<std::uint32_t, std::uint32_t> requests; // requestId -> count
            std::map<std::uint32_t, std::uint32_t> addMs;
            for (const entity::DecalEvent& event : in.decals.Events())
            {
                if (event.engineId)
                {
                    ++adds[event.engineId];
                    addMs[event.engineId] = event.timeMs;
                }
                if (event.requestId)
                    ++requests[event.requestId];
            }
            std::map<std::uint32_t, std::uint32_t> removes;
            std::map<std::uint32_t, std::uint32_t> removeMs;
            for (const entity::DecalRemoveEvent& event : in.decalRemoves.Events())
            {
                ++removes[event.engineId];
                removeMs[event.engineId] = event.timeMs;
            }
            std::set<std::uint32_t> pairs;
            for (const auto& [engineId, count] : adds)
            {
                if (count != 1 || requests.count(engineId) || removes[engineId] != 1)
                    continue;
                const std::uint32_t added = addMs[engineId];
                const std::uint32_t removed = removeMs[engineId];
                if (added < w.base && removed < w.base && added <= removed)
                    pairs.insert(engineId);
            }
            return pairs;
        }

        template <typename Track>
        std::uint64_t Size(const Track& track)
        {
            return track.Events().size();
        }
    }

    std::uint32_t BaseFor(const Recording& recording, std::uint32_t fromMs)
    {
        std::uint32_t base = 0;
        auto& track = const_cast<entity::LifecycleTrack&>(recording.lifecycle);
        With(access::LifeBlocks{}, track, [&](const auto& blocks) {
            for (const auto& block : blocks)
            {
                if (block.startMs > fromMs)
                    break;
                base = block.startMs;
            }
        });
        return base;
    }

    std::vector<KindCount> Tally(const Recording& r, const std::vector<pose::PoseRecordExport>& poses)
    {
        std::uint64_t chunks = 0;
        std::uint64_t frames = 0;
        for (const pose::PoseRecordExport& record : poses)
        {
            chunks += record.track.chunks.size();
            for (const pose::TrackChunkExport& chunk : record.track.chunks)
                frames += chunk.frameTimes.size();
        }
        std::uint64_t weaponSamples = 0;
        for (const WeaponTrack& entry : r.weaponTracks)
            weaponSamples += entry.track.Size();
        std::uint64_t straps = 0;
        for (const StrapTrack& track : r.strapTracks)
            straps += track.states.size();
        std::uint64_t attach = 0;
        for (const AttachFrameTrack& track : r.attachFrames)
            attach += track.frames.size();
        std::uint64_t ropeSamples = 0;
        for (const ropes::RopeRecord& record : r.ropes.Records())
            ropeSamples += record.times.size();
        std::uint64_t rgfxFrames = 0;
        for (const entity::RegisteredFxTrack& track : r.registeredFx)
            rgfxFrames += track.frames.size();
        std::uint64_t appearanceChanges = 0;
        for (const AppearanceTrack& track : r.appearances)
            appearanceChanges += track.changes.size();
        const std::uint64_t blocks = r.lifecycle.BlockCount();
        std::vector<KindCount> out = {
            {"entities", r.created.size()},
            {"blocks", blocks},
            {"segments", r.segments.Segments().size()},
            {"poseRecords", poses.size()},
            {"poseChunks", chunks},
            {"poseFrames", frames},
            {"weaponTracks", r.weaponTracks.size()},
            {"weaponSamples", weaponSamples},
            {"strapStates", straps},
            {"attachFrames", attach},
            {"ropes", r.ropes.Records().size()},
            {"ropeSamples", ropeSamples},
            {"shots", Size(r.shots)},
            {"speech", Size(r.speechLines)},
            {"footsteps", Size(r.footsteps)},
            {"explosions", Size(r.explosions)},
            {"animAudio", Size(r.animAudio)},
            {"surfaces", Size(r.surfaces)},
            {"fx", Size(r.fx)},
            {"registeredFx", r.registeredFx.size()},
            {"registeredFxFrames", rgfxFrames},
            {"decals", Size(r.decals)},
            {"decalRemoves", Size(r.decalRemoves)},
            {"projDecals", Size(r.projDecals)},
            {"gore", Size(r.gore)},
            {"goreHits", Size(r.goreHits)},
            {"timeScale", r.timeScale.Samples().size()},
            {"blood", Size(r.blood)},
            {"fireStarts", Size(r.fireStarts)},
            {"fireStops", Size(r.fireStops)},
            {"fireItems", Size(r.fireItems)},
            {"meleeHits", Size(r.meleeHits)},
            {"bodyImpacts", Size(r.bodyImpacts)},
            {"sounds", Size(r.sounds)},
            {"collisionSounds", r.collisionSounds.size()},
            {"entitySounds", r.entitySounds.size()},
            {"worldObjects", r.worldObjects.size()},
            {"fragBreaks", r.fragBreaks.size()},
            {"fragPieces", r.fragPieces.size()},
            {"scenery", r.scenery.size()},
            {"sceneryChildren", r.sceneryChildren.size()},
            {"gameCamera", r.gameCamera.size()},
            {"worldIpls", r.world.ipls.size()},
            {"worldEntitySets", r.world.entitySets.size()},
            {"appearanceChanges", appearanceChanges},
            {"trainCars", r.trains.cars.size()},
            {"generalSounds", r.generalSounds.size()},
            {"soundCensusCells", r.soundCensus.cells.size()},
            {"soundGenericPackets", r.soundGeneric.Packets()},
            {"mixProbeSamples", r.mixProbe.samples.size()},
            {"renderTargetEvents", r.renderTargets.events.size()},
            {"gameListenerSamples", r.gameListener.samples.size()},
            {"windAudioSamples", r.windAudio.samples.size()},
            {"pedSpeedPoints", r.pedSpeed.Points()},
            {"shaderVarsPoints", r.shaderVars.Points()},
            {"footEvents", r.footEvents.events.size()},
            {"footMovement", r.footMovement.samples.size()},
            {"mixScenes", r.mixScenes.instances.size()},
            {"categoryStateSamples", r.categoryState.samples.size()},
            {"mixGroupSamples", r.mixScenes.groups.size()},
        };
        return out;
    }

    bool Cut(
        const Recording& in,
        const std::vector<pose::PoseRecordExport>& inPoses,
        std::uint64_t durationMs,
        std::uint32_t fromMs,
        std::uint32_t toMs,
        Recording& out,
        std::vector<pose::PoseRecordExport>& outPoses,
        Result& result)
    {
        result = Result{};
        if (&in == &out || &inPoses == &outPoses)
        {
            result.error = "the output must not be the input";
            return false;
        }
        if (durationMs && toMs > durationMs)
            toMs = static_cast<std::uint32_t>(durationMs);
        if (toMs <= fromMs)
        {
            result.error = "empty window";
            return false;
        }
        Window w;
        w.base = BaseFor(in, fromMs);
        w.end = toMs;
        result.baseMs = w.base;
        result.endMs = w.end;
        result.durationMs = w.end - w.base;

        out = Recording();
        outPoses.clear();

        // Entities and lifecycle first: everything keyed by an entity
        // follows the kept set.
        KeptSet kept;
        CutLifecycle(in, w, out, kept);
        out.ids = in.ids;
        for (const entity::CreatePacket& packet : in.created)
        {
            if (!Kept(kept, packet.id))
                continue;
            entity::CreatePacket copy = packet;
            RebaseCreate(copy, w);
            out.created.push_back(std::move(copy));
        }
        std::set<std::uint32_t> droppedSegmentRecords;
        CutSegments(in, w, kept, out, droppedSegmentRecords);

        // Pose records.
        for (const pose::PoseRecordExport& record : inPoses)
        {
            ReplayId owner = ReplayId::FromPacked(record.recordId);
            if (record.recordId & entity::PoseSegmentTrack::kRecordBit)
            {
                owner = in.segments.OwnerOf(record.recordId);
                if (droppedSegmentRecords.count(record.recordId))
                    continue;
            }
            if (!Kept(kept, owner))
                continue;
            pose::PoseRecordExport cut;
            bool keep = false;
            if (!CutPoseRecord(record, w, cut, keep, result))
                return false;
            if (keep)
                outPoses.push_back(std::move(cut));
        }

        // Weapon and prop transform tracks (samples, kept with the entity).
        for (const WeaponTrack& entry : in.weaponTracks)
        {
            if (!Kept(kept, ReplayId::FromPacked(entry.recordId)))
                continue;
            auto& source = const_cast<entity::RelativeTrack&>(entry.track);
            const auto& samples = access::Ref<std::vector<entity::TransformSample>, access::RelativeSamples>(source);
            std::vector<entity::TransformSample> cut =
                CutSamples(samples, w, [](entity::TransformSample& s) -> std::uint32_t& { return s.timeMs; });
            if (cut.empty())
                continue;
            WeaponTrack copy;
            copy.recordId = entry.recordId;
            access::Ref<std::uint32_t, access::RelativeInterval>(copy.track) =
                access::Ref<std::uint32_t, access::RelativeInterval>(source);
            access::Ref<std::uint64_t, access::RelativeDropped>(copy.track) =
                access::Ref<std::uint64_t, access::RelativeDropped>(source);
            access::Ref<std::vector<entity::TransformSample>, access::RelativeSamples>(copy.track) = std::move(cut);
            out.weaponTracks.push_back(std::move(copy));
        }
        for (const StrapTrack& track : in.strapTracks)
        {
            if (!Kept(kept, ReplayId::FromPacked(track.recordId)))
                continue;
            StrapTrack copy;
            copy.recordId = track.recordId;
            using State = std::pair<std::uint32_t, weapon::StrapState>;
            copy.states = CutLastPerKey(track.states, w, [](State& s) -> std::uint32_t& { return s.first; },
                [](const State&) { return std::uint64_t{0}; });
            if (!copy.states.empty())
                out.strapTracks.push_back(std::move(copy));
        }
        for (const WeaponConditionTrack& track : in.weaponConditions)
        {
            if (!Kept(kept, ReplayId::FromPacked(track.recordId)))
                continue;
            WeaponConditionTrack copy;
            copy.recordId = track.recordId;
            using State = std::pair<std::uint32_t, WeaponCondition>;
            copy.states = CutLastPerKey(track.states, w, [](State& s) -> std::uint32_t& { return s.first; },
                [](const State&) { return std::uint64_t{0}; });
            if (!copy.states.empty())
                out.weaponConditions.push_back(std::move(copy));
        }
        // Charred state per ped and horse, the one in force at base folded to 0.
        for (const ScorchTrack& track : in.scorched)
        {
            if (!Kept(kept, ReplayId::FromPacked(track.id)))
                continue;
            ScorchTrack copy;
            copy.id = track.id;
            using State = std::pair<std::uint32_t, bool>;
            copy.states = CutLastPerKey(track.states, w, [](State& s) -> std::uint32_t& { return s.first; },
                [](const State&) { return std::uint64_t{0}; });
            if (!copy.states.empty())
                out.scorched.push_back(std::move(copy));
        }
        // Broken window mask per vehicle, the one in force at base folded to 0.
        for (const WindowTrack& track : in.vehicleWindows)
        {
            if (!Kept(kept, ReplayId::FromPacked(track.id)))
                continue;
            WindowTrack copy;
            copy.id = track.id;
            using State = std::pair<std::uint32_t, std::uint8_t>;
            copy.states = CutLastPerKey(track.states, w, [](State& s) -> std::uint32_t& { return s.first; },
                [](const State&) { return std::uint64_t{0}; });
            if (!copy.states.empty())
                out.vehicleWindows.push_back(std::move(copy));
        }
        // Burnt state per vehicle, the one in force at base folded to 0.
        for (const ScorchTrack& track : in.vehicleBurnt)
        {
            if (!Kept(kept, ReplayId::FromPacked(track.id)))
                continue;
            ScorchTrack copy;
            copy.id = track.id;
            using State = std::pair<std::uint32_t, bool>;
            copy.states = CutLastPerKey(track.states, w, [](State& s) -> std::uint32_t& { return s.first; },
                [](const State&) { return std::uint64_t{0}; });
            if (!copy.states.empty())
                out.vehicleBurnt.push_back(std::move(copy));
        }
        // Burn level per vehicle, ped, horse, weapon object and prop, the one in
        // force at base folded to 0.
        for (const BurnLevelTrack& track : in.burnLevels)
        {
            if (!Kept(kept, ReplayId::FromPacked(track.id)))
                continue;
            BurnLevelTrack copy;
            copy.id = track.id;
            using State = std::pair<std::uint32_t, float>;
            copy.states = CutLastPerKey(track.states, w, [](State& s) -> std::uint32_t& { return s.first; },
                [](const State&) { return std::uint64_t{0}; });
            if (!copy.states.empty())
                out.burnLevels.push_back(std::move(copy));
        }
        // Object looks per weapon object and prop, the one in force at base folded to 0.
        for (const ObjectLookTrack& track : in.objectLooks)
        {
            if (!Kept(kept, ReplayId::FromPacked(track.id)))
                continue;
            ObjectLookTrack copy;
            copy.id = track.id;
            using State = std::pair<std::uint32_t, ObjectLook>;
            copy.states = CutLastPerKey(track.states, w, [](State& s) -> std::uint32_t& { return s.first; },
                [](const State&) { return std::uint64_t{0}; });
            if (!copy.states.empty())
                out.objectLooks.push_back(std::move(copy));
        }
        // Fire records per ped, horse and vehicle, the one in force at base folded to 0.
        for (const FireCharTrack& track : in.fireChars)
        {
            if (!Kept(kept, ReplayId::FromPacked(track.id)))
                continue;
            FireCharTrack copy;
            copy.id = track.id;
            using State = std::pair<std::uint32_t, FireCharState>;
            copy.states = CutLastPerKey(track.states, w, [](State& s) -> std::uint32_t& { return s.first; },
                [](const State&) { return std::uint64_t{0}; });
            if (!copy.states.empty())
                out.fireChars.push_back(std::move(copy));
        }
        // Lost wheel and door mask per vehicle, the one in force at base folded to 0.
        for (const VehiclePartTrack& track : in.vehicleParts)
        {
            if (!Kept(kept, ReplayId::FromPacked(track.id)))
                continue;
            VehiclePartTrack copy;
            copy.id = track.id;
            using State = std::pair<std::uint32_t, std::uint16_t>;
            copy.states = CutLastPerKey(track.states, w, [](State& s) -> std::uint32_t& { return s.first; },
                [](const State&) { return std::uint64_t{0}; });
            if (!copy.states.empty())
                out.vehicleParts.push_back(std::move(copy));
        }
        // Door ratios per vehicle (vehicle_doors.h), the last of each door at or
        // before base folded to 0.
        for (const vehdoors::DoorTrack& track : in.vehicleDoors)
        {
            if (!Kept(kept, ReplayId::FromPacked(track.id)))
                continue;
            vehdoors::DoorTrack copy;
            copy.id = track.id;
            copy.samples = CutLastPerKey(track.samples, w, [](vehdoors::DoorSample& s) -> std::uint32_t& { return s.timeMs; },
                [](const vehdoors::DoorSample& s) { return std::uint64_t{s.door}; });
            if (!copy.samples.empty())
                out.vehicleDoors.push_back(std::move(copy));
        }
        // Appearance changes per ped and horse, the one in force at base folded to 0,
        // but only when it is newer than the pose segment in force at base (also
        // folded to 0): at equal times the change wins (AppearanceAt), so an older
        // change would override the segment's layers.
        for (const AppearanceTrack& track : in.appearances)
        {
            const ReplayId id = ReplayId::FromPacked(track.id);
            if (!Kept(kept, id))
                continue;
            bool hasSegment = false;
            std::uint32_t segmentMs = 0;
            for (const entity::PoseSegment& segment : in.segments.Segments())
            {
                if (segment.entity == id && segment.startMs <= w.base && (!hasSegment || segment.startMs >= segmentMs))
                {
                    hasSegment = true;
                    segmentMs = segment.startMs;
                }
            }
            std::size_t lastBefore = track.changes.size();
            for (std::size_t index = 0; index < track.changes.size(); ++index)
            {
                if (track.changes[index].timeMs <= w.base)
                    lastBefore = index;
            }
            AppearanceTrack copy;
            copy.id = track.id;
            for (std::size_t index = 0; index < track.changes.size(); ++index)
            {
                const AppearanceChange& change = track.changes[index];
                if (change.timeMs > w.end)
                    break;
                if (change.timeMs <= w.base &&
                    (index != lastBefore || (hasSegment && change.timeMs < segmentMs)))
                    continue;
                AppearanceChange shifted = change;
                shifted.timeMs = w.Clamp(change.timeMs);
                copy.changes.push_back(std::move(shifted));
            }
            if (!copy.changes.empty())
                out.appearances.push_back(std::move(copy));
        }
        // The metaped states per ped and horse: the one in force at base folded to 0,
        // the later ones shifted.
        for (const metaped::Track& track : in.metapeds)
        {
            if (!Kept(kept, ReplayId::FromPacked(track.id)))
                continue;
            std::size_t lastBefore = track.states.size();
            for (std::size_t index = 0; index < track.states.size(); ++index)
                if (track.states[index].timeMs <= w.base)
                    lastBefore = index;
            metaped::Track copy;
            copy.id = track.id;
            for (std::size_t index = 0; index < track.states.size(); ++index)
            {
                const metaped::State& state = track.states[index];
                if (state.timeMs > w.end)
                    break;
                if (state.timeMs <= w.base && index != lastBefore)
                    continue;
                metaped::State shifted = state;
                shifted.timeMs = w.Clamp(state.timeMs);
                copy.states.push_back(std::move(shifted));
            }
            if (!copy.states.empty())
                out.metapeds.push_back(std::move(copy));
        }
        for (const AttachFrameTrack& track : in.attachFrames)
        {
            if (!Kept(kept, ReplayId::FromPacked(track.recordId)))
                continue;
            AttachFrameTrack copy;
            copy.recordId = track.recordId;
            using Frame = std::pair<std::uint32_t, weapon::AttachFrame>;
            copy.frames = CutLastPerKey(track.frames, w, [](Frame& f) -> std::uint32_t& { return f.first; },
                [](const Frame&) { return std::uint64_t{0}; });
            if (!copy.frames.empty())
                out.attachFrames.push_back(std::move(copy));
        }

        // Ropes.
        {
            std::vector<ropes::RopeRecord>& records =
                access::Ref<std::vector<ropes::RopeRecord>, access::RopeRecords>(out.ropes);
            records.clear();
            for (const ropes::RopeRecord& record : in.ropes.Records())
            {
                std::uint32_t lastMs = record.times.empty() ? 0 : record.times.back();
                for (const auto& held : record.heldEvents)
                    lastMs = (std::max)(lastMs, held.first);
                for (const ropes::RopeSlotEvent& slot : record.slotEvents)
                    lastMs = (std::max)(lastMs, slot.timeMs);
                if (lastMs < w.base)
                    continue; // ended before the window
                bool startsAfter = true;
                if (!record.times.empty() && record.times.front() <= w.end)
                    startsAfter = false;
                if (!record.heldEvents.empty() && record.heldEvents.front().first <= w.end)
                    startsAfter = false;
                if (!record.slotEvents.empty() && record.slotEvents.front().timeMs <= w.end)
                    startsAfter = false;
                if (startsAfter)
                    continue;
                ropes::RopeRecord copy;
                copy.engineId = record.engineId;
                copy.type = record.type;
                copy.vertices = record.vertices;
                copy.attachments = record.attachments;
                copy.nearPed = record.nearPed;
                const auto span = SampleSpan(record.times, w, [](std::uint32_t t) { return t; });
                const std::size_t stride = static_cast<std::size_t>(record.vertices) * 3u;
                for (std::size_t index = span.first; index < span.second; ++index)
                {
                    copy.times.push_back(w.Clamp(record.times[index]));
                    copy.windowCounts.push_back(record.windowCounts[index]);
                    copy.positions.insert(copy.positions.end(),
                        record.positions.begin() + static_cast<std::ptrdiff_t>(index * stride),
                        record.positions.begin() + static_cast<std::ptrdiff_t>((index + 1) * stride));
                }
                using Held = std::pair<std::uint32_t, bool>;
                copy.heldEvents = CutLastPerKey(record.heldEvents, w, [](Held& h) -> std::uint32_t& { return h.first; },
                    [](const Held&) { return std::uint64_t{0}; });
                copy.slotEvents = CutLastPerKey(record.slotEvents, w,
                    [](ropes::RopeSlotEvent& e) -> std::uint32_t& { return e.timeMs; },
                    [](const ropes::RopeSlotEvent& e) { return static_cast<std::uint64_t>(e.slot); });
                copy.slotState = record.slotState;
                for (ropes::RopeSlotEvent& state : copy.slotState)
                    state.timeMs = w.Clamp((std::min)(state.timeMs, w.end));
                copy.nextStateLogMs = w.Clamp(record.nextStateLogMs);
                records.push_back(std::move(copy));
            }
        }

        // One-shot events.
        CutOneShot(in.shots, out.shots, w);
        CutOneShot(in.speechLines, out.speechLines, w);
        {
            // The speech track's other times: the request and the early stop.
            std::vector<entity::SpeechEvent> lines = out.speechLines.Events();
            out.speechLines.Begin();
            // A line that began before the window and still plays at its base is kept,
            // adopted at 0 with the time it had played (speech_resume.h AdoptAtBase).
            const std::vector<entity::SpeechEvent>& all = in.speechLines.Events();
            for (std::size_t n = 0; n < all.size() && all[n].timeMs < w.base; ++n)
            {
                std::uint32_t next = 0;
                for (std::size_t m = n + 1; m < all.size(); ++m)
                    if (all[m].speaker == all[n].speaker)
                    {
                        next = all[m].timeMs;
                        break;
                    }
                entity::SpeechEvent adopted;
                if (speech::resume::AdoptAtBase(all[n], w.base, next, adopted))
                {
                    if (adopted.stopMs > w.end - w.base)
                        adopted.stopMs = w.end - w.base;
                    out.speechLines.Add(adopted);
                }
            }
            for (entity::SpeechEvent& line : lines)
            {
                line.coreMs = w.Clamp(line.coreMs);
                if (line.stopMs)
                    line.stopMs = (std::max)(w.Clamp(line.stopMs), 1u);
                if (line.endMs) // the natural end, SPCH v2
                    line.endMs = (std::max)(w.Clamp(line.endMs), 1u);
                out.speechLines.Add(line);
            }
        }
        CutOneShot(in.footsteps, out.footsteps, w);
        CutOneShot(in.explosions, out.explosions, w);
        CutOneShot(in.animAudio, out.animAudio, w);
        CutOneShot(in.fx, out.fx, w);
        CutOneShot(in.meleeHits, out.meleeHits, w);
        CutOneShot(in.bodyImpacts, out.bodyImpacts, w);
        CutOneShot(in.sounds, out.sounds, w);

        // Collision sounds: playback starts one only when forward play
        // crosses its start, so one started before base never plays again.
        for (const CollisionSound& sound : in.collisionSounds)
        {
            if (sound.startMs < w.base || sound.startMs > w.end)
                continue;
            CollisionSound copy = sound;
            copy.startMs = sound.startMs - w.base;
            copy.endMs = w.Clamp((std::min)(sound.endMs, w.end));
            copy.intensity.clear();
            for (const auto& sample : sound.intensity)
            {
                if (sample.first > w.end)
                    break;
                copy.intensity.push_back({w.Clamp(sample.first), sample.second});
            }
            out.collisionSounds.push_back(std::move(copy));
        }

        // Entity sounds overlapping the window (a loop that began before base plays
        // from base, as playback restarts a loop mid-span).
        for (const EntitySound& sound : in.entitySounds)
        {
            const std::uint32_t end = sound.ended ? sound.endMs : w.end;
            if (end < w.base || sound.startMs > w.end)
                continue;
            EntitySound copy = sound;
            copy.startMs = w.Clamp((std::max)(sound.startMs, w.base));
            copy.endMs = w.Clamp((std::min)(end, w.end));
            copy.ended = true;
            out.entitySounds.push_back(std::move(copy));
        }

        // The census-level captured sounds, as the entity sounds (a loop alive at base
        // plays from base); the sound census moves with the window (sound_check_core.h).
        for (const census::GeneralSound& sound : in.generalSounds)
        {
            const std::uint32_t end = sound.ended ? sound.endMs : w.end;
            if (end < w.base || sound.startMs > w.end)
                continue;
            if (sound.oneShot && sound.startMs < w.base)
                continue;
            census::GeneralSound copy = sound;
            copy.startMs = w.Clamp((std::max)(sound.startMs, w.base));
            copy.endMs = w.Clamp((std::min)(end, w.end));
            copy.ended = true;
            out.generalSounds.push_back(std::move(copy));
        }
        in.soundCensus.CutInto(out.soundCensus, w.base, w.end);
        // The generic sound packets (a sound alive at base keeps its create at 0,
        // preexisting, with its latest settings).
        soundgeneric::CutInto(in.soundGeneric, out.soundGeneric, w.base, w.end);
        // The owner query answers (the one in force at base moves to 0).
        entityquery::CutInto(in.entityQueries, out.entityQueries, w.base, w.end);
        in.mixProbe.CutInto(out.mixProbe, w.base, w.end);
        // The render targets and UI apps alive at base move to 0.
        out.renderTargets = rendertarget::Cut(in.renderTargets, w.base, w.end);
        in.gameListener.CutInto(out.gameListener, w.base, w.end);
        in.windAudio.CutInto(out.windAudio, w.base, w.end);
        in.mixScenes.CutInto(out.mixScenes, w.base, w.end); // a scene active at base starts at 0
        in.categoryState.CutInto(out.categoryState, w.base, w.end); // the values in force at base move to 0
        // The foley variants of the owners kept.
        pedaudiostate::CutInto(in.foleyVariants, out.foleyVariants,
            [&kept](std::uint32_t owner) { return Kept(kept, ReplayId::FromPacked(owner)); });
        // The kept owners' speeds (the value in force at base moves to 0).
        in.pedSpeed.CutInto(out.pedSpeed, w.base, w.end,
            [&kept](std::uint32_t owner) { return Kept(kept, ReplayId::FromPacked(owner)); });
        // The kept horses' shader variables (the values in force at base move to 0).
        in.shaderVars.CutInto(out.shaderVars, w.base, w.end,
            [&kept](std::uint32_t owner) { return Kept(kept, ReplayId::FromPacked(owner)); });
        // The kept owners' secondary steps inside the window.
        in.footEvents.CutInto(out.footEvents, w.base, w.end,
            [&kept](std::uint32_t owner) { return Kept(kept, ReplayId::FromPacked(owner)); });
        // The kept owners' movement volume results inside the window.
        in.footMovement.CutInto(out.footMovement, w.base, w.end,
            [&kept](std::uint32_t owner) { return Kept(kept, ReplayId::FromPacked(owner)); });

        // State events.
        const auto none = [](auto&, std::uint32_t) {};
        {
            const std::set<std::uint32_t> pairs = DroppableDecalPairs(in, w);
            result.decalPairsDropped = pairs.size();
            // A decal folded forward by `gap` ms is made that much older (decal_state.h
            // AgeSettings, note c); one that has expired by base is dropped. An unattached
            // decal whose entity (the projected mirror's owner) is not kept stays, without it.
            CutState(in.decals, out.decals, w, kFoldForwardMs,
                [&](std::size_t, const entity::DecalEvent& e) {
                    if (e.timeMs < w.base && e.engineId && pairs.count(e.engineId))
                        return false;
                    if (!(!e.attached || KeptOrNone(kept, e.entity)))
                        return false;
                    const std::uint32_t gap = w.FoldGap(e.timeMs, kFoldForwardMs);
                    if (gap)
                    {
                        std::uint8_t aged[sizeof(e.settings)];
                        std::memcpy(aged, e.settings, sizeof(aged));
                        if (!decal::AgeSettings(aged, static_cast<float>(gap) / 1000.0f))
                        {
                            ++result.decalsExpiredDropped;
                            return false;
                        }
                    }
                    return true;
                },
                [&](entity::DecalEvent& e, std::uint32_t gap) {
                    if (gap)
                        decal::AgeSettings(e.settings, static_cast<float>(gap) / 1000.0f);
                    if (!e.attached && e.entity.Valid() && !Kept(kept, e.entity))
                    {
                        e.entity = {};
                        e.hasEntity = e.attachModel != 0;
                    }
                });
            CutState(in.decalRemoves, out.decalRemoves, w, kFoldForwardMs,
                [&](std::size_t, const entity::DecalRemoveEvent& e) {
                    return !(e.timeMs < w.base && pairs.count(e.engineId));
                },
                none);
        }
        CutState(in.projDecals, out.projDecals, w, kFoldForwardMs,
            [&](std::size_t, const entity::ProjDecalEvent& e) { return KeptOrNone(kept, e.entity); }, none);
        {
            // Surfaces: the latest per entity is the state.
            std::map<std::uint32_t, std::size_t> lastBefore;
            const auto& events = in.surfaces.Events();
            for (std::size_t index = 0; index < events.size(); ++index)
            {
                if (events[index].timeMs <= w.base)
                    lastBefore[events[index].entity.Packed()] = index;
            }
            CutState(in.surfaces, out.surfaces, w, kFoldForwardMs,
                [&](std::size_t index, const entity::SurfaceEvent& e) {
                    if (!Kept(kept, e.entity))
                        return false;
                    if (e.timeMs > w.base)
                        return true;
                    const auto found = lastBefore.find(e.entity.Packed());
                    return found != lastBefore.end() && found->second == index;
                },
                none);
        }
        CutState(in.gore, out.gore, w, kFoldForwardMs,
            [&](std::size_t, const entity::GoreEvent& e) { return Kept(kept, e.entity); }, none);
        CutState(in.goreHits, out.goreHits, w, kFoldForwardMs,
            [&](std::size_t, const entity::GoreHitEvent& e) { return Kept(kept, e.entity); }, none);
        CutState(in.blood, out.blood, w, kFoldForwardMs,
            [&](std::size_t, const entity::BloodEvent& e) { return Kept(kept, e.entity); },
            [](entity::BloodEvent& e, std::uint32_t gapMs) { e.age += static_cast<float>(gapMs) / 1000.0f; });
        // Fires: starts are indexed by stops and items, so none is dropped
        // except the tail after end (indices stay).
        CutState(in.fireStarts, out.fireStarts, w, 0u,
            [](std::size_t, const entity::FireStartEvent&) { return true; },
            [&](entity::FireStartEvent& e, std::uint32_t gapMs) {
                if (gapMs)
                {
                    e.initial = true;
                    e.initialAge += static_cast<float>(gapMs) / 1000.0f;
                }
            });
        const std::size_t fireCount = out.fireStarts.Events().size();
        CutState(in.fireStops, out.fireStops, w, 0u,
            [&](std::size_t, const entity::FireStopEvent& e) { return e.start < fireCount; }, none);
        {
            std::vector<entity::FireItemSample> items;
            for (const entity::FireItemSample& item : in.fireItems.Events())
            {
                if (item.start < fireCount)
                    items.push_back(item);
            }
            items = CutLastPerKey(items, w, [](entity::FireItemSample& s) -> std::uint32_t& { return s.timeMs; },
                [](const entity::FireItemSample& s) { return static_cast<std::uint64_t>(s.start); });
            out.fireItems.Begin();
            for (const entity::FireItemSample& item : items)
                out.fireItems.Add(item);
        }
        for (const FragBreak& event : in.fragBreaks)
        {
            if (event.timeMs > w.end || !Kept(kept, event.id))
                continue;
            FragBreak copy = event;
            copy.timeMs = w.Clamp(event.timeMs);
            out.fragBreaks.push_back(copy);
        }
        std::stable_sort(out.fragBreaks.begin(), out.fragBreaks.end(),
            [](const FragBreak& a, const FragBreak& b) { return a.timeMs < b.timeMs; });
        // A kept fragment piece keeps its groups (no time in them).
        for (const FragPiece& row : in.fragPieces)
            if (Kept(kept, row.id))
                out.fragPieces.push_back(row);
        {
            using Sample = timescale::Sample;
            access::Ref<std::vector<Sample>, access::TimeScaleSamples>(out.timeScale) =
                CutLastPerKey(in.timeScale.Samples(), w, [](Sample& s) -> std::uint32_t& { return s.timeMs; },
                    [](const Sample&) { return std::uint64_t{0}; });
        }

        // Continuous tracks.
        for (const entity::RegisteredFxTrack& track : in.registeredFx)
        {
            if (track.endMs < w.base || track.startMs > w.end)
                continue;
            entity::RegisteredFxTrack copy = track;
            copy.frames = CutSamples(track.frames, w, [](entity::RegisteredFxFrame& f) -> std::uint32_t& { return f.timeMs; });
            if (copy.frames.empty())
                continue;
            copy.startMs = w.Clamp(track.startMs);
            copy.endMs = w.Clamp((std::min)(track.endMs, w.end));
            // The attach changes onto kept entities, the one in force at base first (at
            // 0), rebased like the frames.
            std::vector<entity::RegisteredFxAttach> attaches;
            for (const entity::RegisteredFxAttach& entry : track.attachTimeline)
                if (Kept(kept, entry.id))
                    attaches.push_back(entry);
            copy.attachTimeline = CutSamples(attaches, w, [](entity::RegisteredFxAttach& e) -> std::uint32_t& { return e.timeMs; });
            // The settings in force at base first (at 0), and the stretches without a call
            // that reach into the window, cut to it and rebased.
            copy.settings = CutSamples(track.settings, w, [](entity::RegisteredFxSettingsChange& e) -> std::uint32_t& { return e.timeMs; });
            copy.callGaps.clear();
            for (const entity::RegisteredFxGap& gap : track.callGaps)
            {
                if (gap.toMs <= w.base || gap.fromMs >= w.end)
                    continue;
                entity::RegisteredFxGap cut;
                cut.fromMs = w.Clamp((std::max)(gap.fromMs, w.base));
                cut.toMs = w.Clamp((std::min)(gap.toMs, w.end));
                if (cut.toMs > cut.fromMs && (copy.callGaps.empty() || cut.fromMs >= copy.callGaps.back().toMs))
                    copy.callGaps.push_back(cut);
            }
            out.registeredFx.push_back(std::move(copy));
        }
        out.gameCamera = CutSamples(in.gameCamera, w, [](camera::GameSample& s) -> std::uint32_t& { return s.timeMs; });
        // Doors: samples like the camera, the last one before base holds the door's
        // angle at the clip start.
        for (const DoorTrack& door : in.doors)
        {
            DoorTrack copy;
            copy.hash = door.hash;
            copy.samples = CutSamples(door.samples, w, [](DoorSample& s) -> std::uint32_t& { return s.timeMs; });
            if (!copy.samples.empty())
                out.doors.push_back(std::move(copy));
        }

        // Ability spans: the part inside the window.
        for (const AbilitySpan& span : in.abilities)
        {
            if (span.endMs < w.base || span.startMs > w.end)
                continue;
            AbilitySpan copy = span;
            copy.startMs = w.Clamp(span.startMs);
            copy.endMs = w.Clamp((std::min)(span.endMs, w.end));
            copy.open = false;
            out.abilities.push_back(copy);
        }

        // Restricted-camera spans, the part inside the window (like the ability
        // spans), and the cuts after base.
        for (const gate::Span& span : in.spans)
        {
            if (span.endMs < w.base || span.startMs > w.end)
                continue;
            gate::Span copy = span;
            copy.startMs = w.Clamp(span.startMs);
            copy.endMs = w.Clamp((std::min)(span.endMs, w.end));
            copy.open = false;
            if (copy.endMs > copy.startMs)
                out.spans.push_back(copy);
        }
        for (const std::uint32_t cut : in.cuts)
        {
            if (cut > w.base && cut <= w.end)
                out.cuts.push_back(cut - w.base);
        }

        // Vehicle components: for the kept vehicles.
        for (const VehicleLook& look : in.vehicleLooks)
        {
            if (Kept(kept, look.id))
                out.vehicleLooks.push_back(look);
        }
        // Train cars of the kept entities (a neighbour out of the window reads 0) and
        // the head cars' speed, sampled like a track.
        in.trains.CutInto(out.trains, [&](std::uint32_t id) { return Kept(kept, ReplayId::FromPacked(id)); },
            [&](const std::vector<trains::SpeedSample>& samples) {
                return CutSamples(samples, w, [](trains::SpeedSample& s) -> std::uint32_t& { return s.timeMs; });
            });

        // World.
        for (const WorldObjectOrigin& origin : in.worldObjects)
        {
            if (Kept(kept, origin.id))
                out.worldObjects.push_back(origin);
        }
        // The scenery alive in the window, its times moved to it (object_check.h
        // CutLife); a takeover by a prop left out is dropped (the object stays as it
        // stood). The index each kept entry got, for its children.
        std::vector<std::uint32_t> sceneryIndex(in.scenery.size(), 0xFFFFFFFFu);
        for (std::size_t n = 0; n < in.scenery.size(); ++n)
        {
            SceneryObject copy = in.scenery[n];
            if (!objcheck::CutLife(copy.appearMs, copy.goneMs, w.base, w.end))
                continue;
            objcheck::CutVisibility(copy.visible, copy.visibleFlipsMs, w.base, w.end);
            if (copy.prop.Valid() && !Kept(kept, copy.prop))
                copy.prop = ReplayId{};
            sceneryIndex[n] = static_cast<std::uint32_t>(out.scenery.size());
            out.scenery.push_back(copy);
        }
        // The scenery children alive in the window whose parent was kept, their
        // parent renumbered.
        for (const SceneryChild& child : in.sceneryChildren)
        {
            if (child.parent >= sceneryIndex.size() || sceneryIndex[child.parent] == 0xFFFFFFFFu)
                continue;
            SceneryChild copy = child;
            if (!objcheck::CutLife(copy.appearMs, copy.goneMs, w.base, w.end))
                continue;
            objcheck::CutVisibility(copy.visible, copy.visibleFlipsMs, w.base, w.end);
            copy.parent = sceneryIndex[child.parent];
            out.sceneryChildren.push_back(std::move(copy));
        }
        // The scenery burn levels of the kept entries, renumbered, the one in force at
        // base folded to 0.
        for (const BurnLevelTrack& track : in.sceneryBurnLevels)
        {
            if (track.id >= sceneryIndex.size() || sceneryIndex[track.id] == 0xFFFFFFFFu)
                continue;
            BurnLevelTrack copy;
            copy.id = sceneryIndex[track.id];
            using State = std::pair<std::uint32_t, float>;
            copy.states = CutLastPerKey(track.states, w, [](State& s) -> std::uint32_t& { return s.first; },
                [](const State&) { return std::uint64_t{0}; });
            if (!copy.states.empty())
                out.sceneryBurnLevels.push_back(std::move(copy));
        }
        // The scenery looks of the kept entries, renumbered, the one in force at base
        // folded to 0.
        for (const ObjectLookTrack& track : in.sceneryObjectLooks)
        {
            if (track.id >= sceneryIndex.size() || sceneryIndex[track.id] == 0xFFFFFFFFu)
                continue;
            ObjectLookTrack copy;
            copy.id = sceneryIndex[track.id];
            using State = std::pair<std::uint32_t, ObjectLook>;
            copy.states = CutLastPerKey(track.states, w, [](State& s) -> std::uint32_t& { return s.first; },
                [](const State&) { return std::uint64_t{0}; });
            if (!copy.states.empty())
                out.sceneryObjectLooks.push_back(std::move(copy));
        }
        // The ped visibility, the state at base folded in.
        for (const PedVisibility& row : in.pedVisibility)
        {
            if (!Kept(kept, row.id))
                continue;
            PedVisibility copy = row;
            objcheck::CutVisibility(copy.visible, copy.flipsMs, w.base, w.end);
            out.pedVisibility.push_back(std::move(copy));
        }
        out.objectsBefore = in.objectsBefore;
        out.pickupsBefore = in.pickupsBefore;
        out.adoptedObjects = in.adoptedObjects;
        out.hasArea = in.hasArea;
        std::memcpy(out.areaMin, in.areaMin, sizeof(out.areaMin));
        std::memcpy(out.areaMax, in.areaMax, sizeof(out.areaMax));
        out.environment = in.environment;
        if (in.environment.recorded)
        {
            out.environment.clockStartS = ClockAt(in.environment, w.base, durationMs);
            out.environment.clockEndS = ClockAt(in.environment, w.end, durationMs);
        }
        // The environment over time, from the state at the base.
        out.environment.track = envtrack::Cut(in.environment.track, w.base, w.end);
        // A window of a recording still running (the buffer's Shift+F4, a manual
        // recording or part inside the ring): the recorded flag and the stop clock
        // are only set when the recording stops, so the window takes them from its
        // track, as a stopped recording's window would have them. Without this the
        // clip saved had no editable time and weather and played back without its
        // environment (QA 232).
        if (!in.environment.recorded)
            EnvironmentFromTrack(out.environment, in.environment.track, w.base, w.end);
        // The clouds too, from the state at the base.
        out.environment.sky = skytrack::Cut(in.environment.sky, in.environment.skyPeriods, w.base, w.end);
        // The timecycle modifier and the variations, from the state at the base.
        out.environment.timecycle = tctrack::Cut(in.environment.timecycle, w.base, w.end);
        // The moon's angles, from the state at the base.
        out.environment.moon = moontrack::Cut(in.environment.moon, w.base, w.end);
        if (!out.environment.track.empty())
        {
            const EnvironmentSample& first = out.environment.track.front();
            out.environment.weatherFrom = first.weatherFrom;
            out.environment.weatherTo = first.weatherTo;
            out.environment.weatherMix = first.weatherMix;
        }
        out.place = in.place;
        // The map state, last per IPL and per entity set at base folded to 0 (state
        // events), the changes inside the window shifted.
        world::CutWorld(in.world, w.base, w.end, out.world);
        out.fromDisk = in.fromDisk;

        const std::vector<KindCount> before = Tally(in, inPoses);
        const std::vector<KindCount> after = Tally(out, outPoses);
        result.kinds = before;
        for (std::size_t index = 0; index < result.kinds.size() && index < after.size(); ++index)
            result.kinds[index].after = after[index].before;
        return true;
    }

    void CutCamera(const camera::Track& in, std::uint32_t baseMs, std::uint32_t endMs, camera::Track& out)
    {
        auto& source = const_cast<camera::Track&>(in);
        const std::uint32_t nextId = access::Ref<std::uint32_t, access::CameraNextId>(source);
        std::vector<camera::Keyframe> keyframes;
        for (const camera::Keyframe& keyframe : in.Keyframes())
        {
            if (keyframe.timeMs < baseMs || keyframe.timeMs > endMs)
                continue;
            camera::Keyframe copy = keyframe;
            copy.timeMs = keyframe.timeMs - baseMs;
            keyframes.push_back(copy);
        }
        out.Clear();
        out.SetLegacyBlendAnyMode(in.LegacyBlendAnyMode());
        access::Ref<std::vector<camera::Keyframe>, access::CameraKeyframes>(out) = std::move(keyframes);
        access::Ref<std::uint32_t, access::CameraNextId>(out) = nextId;
    }
}
