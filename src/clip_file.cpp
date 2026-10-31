#include "clip_file_internal.h"

namespace replay::clipfile::access
{

// Several clip members keep their state private and have no bulk setter
// (the lifecycle blocks, the id allocator, the rope records, a transform
// track's samples, the time scale samples, the camera keyframes). Instead
// of adding accessors to those headers, this file names those members
// through explicit template instantiation, where the language does not apply
// access checking. Only this file reads or writes them, and only to copy a
// whole member in or out.
    // With(Tag{}, object, visit) calls visit(object.member).
    template <typename Tag, auto Member>
    struct Steal
    {
        template <typename Object, typename Visit>
        friend void With(Tag, Object& object, Visit&& visit)
        {
            visit(object.*Member);
        }
    };

#define REPLAY_CLIP_MEMBER(TagName, Class, Member)                  \
    struct TagName                                                  \
    {                                                               \
        template <typename Object, typename Visit>                  \
        friend void With(TagName, Object& object, Visit&& visit);   \
    };                                                              \
    template struct Steal<TagName, &Class::Member>;

    REPLAY_CLIP_MEMBER(IdsSlots, entity::IdAllocator, m_slotInstance)
    REPLAY_CLIP_MEMBER(IdsNext, entity::IdAllocator, m_nextInstance)
    REPLAY_CLIP_MEMBER(IdsInUse, entity::IdAllocator, m_inUse)
    REPLAY_CLIP_MEMBER(LifeBlockMs, entity::LifecycleTrack, m_blockMs)
    REPLAY_CLIP_MEMBER(LifeLive, entity::LifecycleTrack, m_live)
    REPLAY_CLIP_MEMBER(LifeLiveAttachments, entity::LifecycleTrack, m_liveAttachments)
    REPLAY_CLIP_MEMBER(LifeBlocks, entity::LifecycleTrack, m_blocks)
    REPLAY_CLIP_MEMBER(LifeStats, entity::LifecycleTrack, m_stats)
    REPLAY_CLIP_MEMBER(SegmentsNext, entity::PoseSegmentTrack, m_next)
    REPLAY_CLIP_MEMBER(RelativeInterval, entity::RelativeTrack, m_intervalMs)
    REPLAY_CLIP_MEMBER(RelativeSamples, entity::RelativeTrack, m_samples)
    REPLAY_CLIP_MEMBER(RelativeDropped, entity::RelativeTrack, m_dropped)
    REPLAY_CLIP_MEMBER(RopeRecords, ropes::Ropes, m_records)
    REPLAY_CLIP_MEMBER(TimeScaleSamples, timescale::Track, m_samples)
    REPLAY_CLIP_MEMBER(CameraKeyframes, camera::Track, keyframes_)
    REPLAY_CLIP_MEMBER(CameraNextId, camera::Track, nextId_)

#undef REPLAY_CLIP_MEMBER

    // The member as a T& (for members of a public type; the compiler
    // checks the type).
    template <typename T, typename Tag, typename Object>
    T& Ref(Object& object)
    {
        T* member = nullptr;
        With(Tag{}, object, [&](T& value) { member = &value; });
        return *member;
    }
}

namespace replay::clipfile
{

    namespace
    {

        constexpr std::uint32_t Tag(const char (&text)[5])
        {
            return static_cast<std::uint32_t>(static_cast<std::uint8_t>(text[0])) |
                (static_cast<std::uint32_t>(static_cast<std::uint8_t>(text[1])) << 8) |
                (static_cast<std::uint32_t>(static_cast<std::uint8_t>(text[2])) << 16) |
                (static_cast<std::uint32_t>(static_cast<std::uint8_t>(text[3])) << 24);
        }

        std::string TagText(std::uint32_t tag)
        {
            std::string text;
            for (int shift = 0; shift < 32; shift += 8)
            {
                const char c = static_cast<char>((tag >> shift) & 0xFFu);
                text += (c >= 0x20 && c < 0x7F) ? c : '?';
            }
            return text;
        }

        // Header: magic, format version, duration.
        constexpr std::size_t kHeaderBytes = sizeof(kMagic) + sizeof(std::uint32_t) + sizeof(std::uint64_t);
        // Section header: tag, version, payload length.
        constexpr std::size_t kSectionHeaderBytes = sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint64_t);

        std::uint64_t Fnv64(const std::uint8_t* data, std::size_t size)
        {
            std::uint64_t hash = 0xCBF29CE484222325ull;
            for (std::size_t n = 0; n < size; ++n)
            {
                hash ^= data[n];
                hash *= 0x100000001B3ull;
            }
            return hash;
        }
    }

        // A pointer into the game image as an offset from its base, 0 when
        // it points anywhere else (heap, stack, null).
        std::uint64_t ToRva(std::uint64_t address, const Image& image)
        {
            if (image.base == 0 || address <= image.base || address - image.base >= image.size)
                return 0;
            return address - image.base;
        }

    namespace
    {

        // The clip being read was saved on the running game build (META), and
        // the running build is 1.0.1491.50, the build of the clips written
        // before the site ids. Per thread: the clip list reads on a worker.
        thread_local bool t_sameBuild = true;
        thread_local bool t_running1491 = true;
        constexpr std::uint32_t k1491Timestamp = 0x65EEC92Eu;
        constexpr std::uint64_t k1491ImageSize = 0x7397600u;
    }

        // Back to an address in this process's image; 0 stays 0 and an
        // offset past the image becomes 0.
        std::uint64_t FromRva(std::uint64_t rva, const Image& image)
        {
            if (rva == 0 || image.base == 0 || rva >= image.size)
                return 0;
            return static_cast<std::uint64_t>(image.base) + rva;
        }
        // An RVA the clip keeps raw (no site id): 0 on another build.
        std::uint64_t FromRawRva(std::uint64_t rva, const Image& image) { return t_sameBuild ? FromRva(rva, image) : 0; }
        // A caller the clip keeps as a raw RVA (diagnostics, not compared by
        // the playback): foreign on another build.
        std::uint32_t RawCaller(std::uint32_t rva) { return t_sameBuild || !rva ? rva : sig::kForeignRva; }

    namespace
    {

        // The game addresses the playback compares (clip_sites.h) are written
        // as site ids and read back as RVAs of the running game: its map
        // (SetSiteMap), the 1491.50 one when none was set.
        const sig::SiteMap* g_sites = nullptr;
        const sig::SiteMap& Sites() { return g_sites ? *g_sites : clipsites::KnownSiteMap(); }

        // No RVA reaches 4 GB: such a value is written as 0xFFFFFFFF, which the
        // checks on load refuse as they refused it.
        std::uint32_t RvaBits(std::uint64_t rva) { return rva > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(rva); }
    }
        std::uint64_t SiteIdOf(std::uint64_t rva) { return sig::Pack(Sites().SiteOf(RvaBits(rva))); }
        // A raw id is an RVA of the build the clip was taken on: kept on that
        // build, foreign on another.
        std::uint32_t RvaOfSite(std::uint64_t id) { return Sites().RvaOf(sig::Unpack(id), t_sameBuild); }
        // A field written before the site ids: an RVA of the 1491.50 exe (the
        // only one the mod ran on), named by that exe's map; one it does not
        // name is kept only on that exe.
        std::uint32_t RvaOf1491(std::uint64_t rva)
        {
            return Sites().RvaOf(clipsites::KnownSiteMap().SiteOf(RvaBits(rva)), t_running1491);
        }

        void ZeroBytes(std::uint8_t* data, std::size_t size, std::size_t offset, std::size_t count)
        {
            if (offset >= size)
                return;
            if (count > size - offset)
                count = size - offset;
            std::memset(data + offset, 0, count);
        }

        std::uint64_t ReadU64At(const std::uint8_t* data, std::size_t offset)
        {
            std::uint64_t value = 0;
            std::memcpy(&value, data + offset, sizeof(value));
            return value;
        }

        void WriteU64At(std::uint8_t* data, std::size_t offset, std::uint64_t value)
        {
            std::memcpy(data + offset, &value, sizeof(value));
        }

        // Shared pieces.

        void WriteVec3(Writer& w, const pose::Vec3& v)
        {
            w.Put(v.x);
            w.Put(v.y);
            w.Put(v.z);
        }

        pose::Vec3 ReadVec3(Reader& r)
        {
            pose::Vec3 v;
            v.x = r.Get<float>();
            v.y = r.Get<float>();
            v.z = r.Get<float>();
            return v;
        }

        void WriteAppearance(Writer& w, const entity::Appearance& appearance)
        {
            w.Put(appearance.outfit);
            w.Count(appearance.assets.size());
            for (const entity::MetaPedAsset& asset : appearance.assets)
            {
                w.Put(asset.drawable);
                w.Put(asset.albedo);
                w.Put(asset.normal);
                w.Put(asset.material);
                w.Put(asset.palette);
                w.Put(asset.tint0);
                w.Put(asset.tint1);
                w.Put(asset.tint2);
            }
            w.Put(appearance.vehicleTint);
            w.Put(appearance.vehicleLivery);
        }

        void ReadAppearance(Reader& r, entity::Appearance& appearance)
        {
            appearance.outfit = r.Get<std::uint32_t>();
            appearance.assets.resize(r.Count(32));
            for (entity::MetaPedAsset& asset : appearance.assets)
            {
                asset.drawable = r.Get<std::uint32_t>();
                asset.albedo = r.Get<std::uint32_t>();
                asset.normal = r.Get<std::uint32_t>();
                asset.material = r.Get<std::uint32_t>();
                asset.palette = r.Get<std::uint32_t>();
                asset.tint0 = r.Get<std::int32_t>();
                asset.tint1 = r.Get<std::int32_t>();
                asset.tint2 = r.Get<std::int32_t>();
            }
            appearance.vehicleTint = r.Get<std::int32_t>();
            appearance.vehicleLivery = r.Get<std::int32_t>();
        }

    namespace
    {

        // sourceHandle is a script handle of the recording session: not written.
        void WriteCreate(Writer& w, const entity::CreatePacket& packet)
        {
            w.Id(packet.id);
            w.Put(packet.timeMs);
            w.Enum(packet.kind);
            w.Put(packet.modelHash);
            w.Put(packet.boneCount);
            w.Put(packet.weaponHash);
            w.Values(packet.weaponComponents);
            w.Id(packet.owner);
            w.Enum(packet.propOrigin);
            WriteAppearance(w, packet.appearance);
        }

        void ReadCreate(Reader& r, entity::CreatePacket& packet)
        {
            packet.id = r.Id();
            packet.timeMs = r.Get<std::uint32_t>();
            packet.kind = r.Enum<entity::EntityKind>(static_cast<std::uint8_t>(entity::EntityKind::TrainCar));
            packet.modelHash = r.Get<std::uint32_t>();
            packet.sourceHandle = 0;
            packet.boneCount = r.Get<std::uint32_t>();
            packet.weaponHash = r.Get<std::uint32_t>();
            r.Values(packet.weaponComponents);
            packet.owner = r.Id();
            packet.propOrigin = r.Enum<entity::PropOrigin>(static_cast<std::uint8_t>(entity::PropOrigin::VehiclePiece));
            ReadAppearance(r, packet.appearance);
        }

        void WriteCreates(Writer& w, const std::vector<entity::CreatePacket>& packets)
        {
            w.Count(packets.size());
            for (const entity::CreatePacket& packet : packets)
                WriteCreate(w, packet);
        }

        void ReadCreates(Reader& r, std::vector<entity::CreatePacket>& packets)
        {
            packets.resize(r.Count(40));
            for (entity::CreatePacket& packet : packets)
                ReadCreate(r, packet);
        }

        void WriteAttachment(Writer& w, const entity::Attachment& attachment)
        {
            w.Id(attachment.child);
            w.Id(attachment.parent);
            w.Enum(attachment.kind);
            w.Put(attachment.index);
        }

        void ReadAttachment(Reader& r, entity::Attachment& attachment)
        {
            attachment.child = r.Id();
            attachment.parent = r.Id();
            attachment.kind = r.Enum<entity::AttachmentKind>(static_cast<std::uint8_t>(entity::AttachmentKind::WeaponPoint));
            attachment.index = r.Get<std::int32_t>();
        }

        void WriteAttachments(Writer& w, const std::vector<entity::Attachment>& attachments)
        {
            w.Count(attachments.size());
            for (const entity::Attachment& attachment : attachments)
                WriteAttachment(w, attachment);
        }

        void ReadAttachments(Reader& r, std::vector<entity::Attachment>& attachments)
        {
            attachments.resize(r.Count(13));
            for (entity::Attachment& attachment : attachments)
                ReadAttachment(r, attachment);
        }

        // Section "ids" (IDAL): the id allocator.
        constexpr std::uint16_t kIdsVersion = 1;
        void WriteIds(Writer& w, const SaveContext& c)
        {
            auto& ids = const_cast<entity::IdAllocator&>(c.recording.ids);
            w.Values(access::Ref<std::vector<std::uint16_t>, access::IdsSlots>(ids));
            w.Put(access::Ref<std::uint16_t, access::IdsNext>(ids));
            w.Put(access::Ref<std::uint32_t, access::IdsInUse>(ids));
        }
        void ReadIds(Reader& r, LoadContext& c)
        {
            entity::IdAllocator& ids = c.recording.ids;
            auto& slots = access::Ref<std::vector<std::uint16_t>, access::IdsSlots>(ids);
            r.Values(slots);
            access::Ref<std::uint16_t, access::IdsNext>(ids) = r.Get<std::uint16_t>();
            access::Ref<std::uint32_t, access::IdsInUse>(ids) = r.Get<std::uint32_t>();
            if (slots.size() > 0xFFFFu || access::Ref<std::uint32_t, access::IdsInUse>(ids) > slots.size())
                r.Fail();
        }

        // Section "lifecycle" (LIFE): the create/delete and attach blocks.
        constexpr std::uint16_t kLifecycleVersion = 1;
        void WriteLifecycle(Writer& w, const SaveContext& c)
        {
            auto& track = const_cast<entity::LifecycleTrack&>(c.recording.lifecycle);
            w.Put(access::Ref<std::uint32_t, access::LifeBlockMs>(track));
            WriteCreates(w, access::Ref<std::vector<entity::CreatePacket>, access::LifeLive>(track));
            WriteAttachments(w, access::Ref<std::vector<entity::Attachment>, access::LifeLiveAttachments>(track));
            With(access::LifeBlocks{}, track, [&](const auto& blocks) {
                w.Count(blocks.size());
                for (const auto& block : blocks)
                {
                    w.Put(block.startMs);
                    WriteCreates(w, block.fullCreates);
                    w.Count(block.events.size());
                    for (const auto& event : block.events)
                    {
                        w.Bool(event.create);
                        w.Put(event.timeMs);
                        WriteCreate(w, event.packet);
                    }
                    WriteAttachments(w, block.fullAttachments);
                    w.Count(block.attachEvents.size());
                    for (const auto& event : block.attachEvents)
                    {
                        w.Bool(event.attach);
                        w.Put(event.timeMs);
                        WriteAttachment(w, event.attachment);
                    }
                }
            });
            const entity::LifecycleStats& stats = access::Ref<entity::LifecycleStats, access::LifeStats>(track);
            w.Put(stats.blocks);
            w.Put(stats.creates);
            w.Put(stats.deletes);
            w.Put(stats.fullCreates);
            w.Put(stats.attaches);
            w.Put(stats.detaches);
            w.Put(stats.fullAttachments);
        }
        void ReadLifecycle(Reader& r, LoadContext& c)
        {
            entity::LifecycleTrack& track = c.recording.lifecycle;
            track.Begin();
            const std::uint32_t blockMs = r.Get<std::uint32_t>();
            if (blockMs == 0)
                r.Fail();
            access::Ref<std::uint32_t, access::LifeBlockMs>(track) = blockMs;
            ReadCreates(r, access::Ref<std::vector<entity::CreatePacket>, access::LifeLive>(track));
            ReadAttachments(r, access::Ref<std::vector<entity::Attachment>, access::LifeLiveAttachments>(track));
            With(access::LifeBlocks{}, track, [&](auto& blocks) {
                using Block = typename std::decay_t<decltype(blocks)>::value_type;
                const std::size_t count = r.Count(4);
                for (std::size_t index = 0; index < count && r.Ok(); ++index)
                {
                    Block block;
                    block.startMs = r.Get<std::uint32_t>();
                    if (!blocks.empty() && block.startMs <= blocks.back().startMs)
                        r.Fail();
                    ReadCreates(r, block.fullCreates);
                    block.events.resize(r.Count(45));
                    for (auto& event : block.events)
                    {
                        event.create = r.Bool();
                        event.timeMs = r.Get<std::uint32_t>();
                        ReadCreate(r, event.packet);
                    }
                    ReadAttachments(r, block.fullAttachments);
                    block.attachEvents.resize(r.Count(18));
                    for (auto& event : block.attachEvents)
                    {
                        event.attach = r.Bool();
                        event.timeMs = r.Get<std::uint32_t>();
                        ReadAttachment(r, event.attachment);
                    }
                    blocks.push_back(std::move(block));
                }
            });
            entity::LifecycleStats& stats = access::Ref<entity::LifecycleStats, access::LifeStats>(track);
            stats.blocks = r.Get<std::uint64_t>();
            stats.creates = r.Get<std::uint64_t>();
            stats.deletes = r.Get<std::uint64_t>();
            stats.fullCreates = r.Get<std::uint64_t>();
            stats.attaches = r.Get<std::uint64_t>();
            stats.detaches = r.Get<std::uint64_t>();
            stats.fullAttachments = r.Get<std::uint64_t>();
        }

        // Section "segments" (SEGM): pose segments.
        constexpr std::uint16_t kSegmentsVersion = 1;
        void WriteSegments(Writer& w, const SaveContext& c)
        {
            auto& track = const_cast<entity::PoseSegmentTrack&>(c.recording.segments);
            w.Put(access::Ref<std::uint32_t, access::SegmentsNext>(track));
            w.Count(track.Segments().size());
            for (const entity::PoseSegment& segment : track.Segments())
            {
                w.Id(segment.entity);
                w.Put(segment.startMs);
                w.Put(segment.recordId);
                w.Put(segment.boneCount);
                WriteAppearance(w, segment.appearance);
            }
        }
        void ReadSegments(Reader& r, LoadContext& c)
        {
            entity::PoseSegmentTrack& track = c.recording.segments;
            track.Begin();
            const std::uint32_t next = r.Get<std::uint32_t>();
            const std::size_t count = r.Count(24);
            for (std::size_t index = 0; index < count && r.Ok(); ++index)
            {
                entity::PoseSegment segment;
                segment.entity = r.Id();
                segment.startMs = r.Get<std::uint32_t>();
                segment.recordId = r.Get<std::uint32_t>();
                segment.boneCount = r.Get<std::uint32_t>();
                ReadAppearance(r, segment.appearance);
                track.Add(segment);
            }
            access::Ref<std::uint32_t, access::SegmentsNext>(track) = next;
        }

        // Section "created" (CREA): every create packet.
        constexpr std::uint16_t kCreatedVersion = 1;
        void WriteCreated(Writer& w, const SaveContext& c) { WriteCreates(w, c.recording.created); }
        void ReadCreated(Reader& r, LoadContext& c) { ReadCreates(r, c.recording.created); }

        // Section "weaponTracks" (WTRK): transform tracks of weapons and props.
        constexpr std::uint16_t kWeaponTracksVersion = 1;
        void WriteWeaponTracks(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.weaponTracks.size());
            for (const WeaponTrack& entry : c.recording.weaponTracks)
            {
                auto& track = const_cast<entity::RelativeTrack&>(entry.track);
                w.Put(entry.recordId);
                w.Put(access::Ref<std::uint32_t, access::RelativeInterval>(track));
                w.Put(access::Ref<std::uint64_t, access::RelativeDropped>(track));
                const auto& samples = access::Ref<std::vector<entity::TransformSample>, access::RelativeSamples>(track);
                w.Count(samples.size());
                for (const entity::TransformSample& sample : samples)
                {
                    w.Put(sample.timeMs);
                    WriteVec3(w, sample.relative.position);
                    w.Put(sample.relative.rotation.x);
                    w.Put(sample.relative.rotation.y);
                    w.Put(sample.relative.rotation.z);
                    w.Put(sample.relative.rotation.w);
                    w.Bool(sample.visible);
                }
            }
        }
        void ReadWeaponTracks(Reader& r, LoadContext& c)
        {
            c.recording.weaponTracks.clear();
            const std::size_t count = r.Count(24);
            for (std::size_t index = 0; index < count && r.Ok(); ++index)
            {
                WeaponTrack entry;
                entry.recordId = r.Get<std::uint32_t>();
                access::Ref<std::uint32_t, access::RelativeInterval>(entry.track) = r.Get<std::uint32_t>();
                access::Ref<std::uint64_t, access::RelativeDropped>(entry.track) = r.Get<std::uint64_t>();
                auto& samples = access::Ref<std::vector<entity::TransformSample>, access::RelativeSamples>(entry.track);
                samples.resize(r.Count(33));
                for (entity::TransformSample& sample : samples)
                {
                    sample.timeMs = r.Get<std::uint32_t>();
                    sample.relative.position = ReadVec3(r);
                    sample.relative.rotation.x = r.Get<float>();
                    sample.relative.rotation.y = r.Get<float>();
                    sample.relative.rotation.z = r.Get<float>();
                    sample.relative.rotation.w = r.Get<float>();
                    sample.visible = r.Bool();
                }
                for (std::size_t n = 1; n < samples.size(); ++n)
                {
                    if (samples[n].timeMs < samples[n - 1].timeMs)
                        r.Fail();
                }
                c.recording.weaponTracks.push_back(std::move(entry));
            }
        }

        // Section "strapTracks" (STRP).
        constexpr std::uint16_t kStrapTracksVersion = 1;
        void WriteStrapTracks(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.strapTracks.size());
            for (const StrapTrack& track : c.recording.strapTracks)
            {
                w.Put(track.recordId);
                w.Count(track.states.size());
                for (const auto& [timeMs, state] : track.states)
                {
                    w.Put(timeMs);
                    w.Count(state.maskBits.size());
                    for (const auto& [hash, set] : state.maskBits)
                    {
                        w.Put(hash);
                        w.Bool(set);
                    }
                    w.Put(state.flags);
                    w.Put(state.weightA);
                    w.Put(state.weightB);
                    w.Bool(state.hasStraps);
                }
            }
        }
        void ReadStrapTracks(Reader& r, LoadContext& c)
        {
            c.recording.strapTracks.resize(r.Count(12));
            for (StrapTrack& track : c.recording.strapTracks)
            {
                track.recordId = r.Get<std::uint32_t>();
                track.states.resize(r.Count(25));
                for (auto& [timeMs, state] : track.states)
                {
                    timeMs = r.Get<std::uint32_t>();
                    state.maskBits.resize(r.Count(5));
                    for (auto& [hash, set] : state.maskBits)
                    {
                        hash = r.Get<std::uint32_t>();
                        set = r.Bool();
                    }
                    state.flags = r.Get<std::uint32_t>();
                    state.weightA = r.Get<float>();
                    state.weightB = r.Get<float>();
                    state.hasStraps = r.Bool();
                }
            }
        }

        // Section "attachFrames" (ATFR).
        constexpr std::uint16_t kAttachFramesVersion = 1;
        void WriteAttachFrames(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.attachFrames.size());
            for (const AttachFrameTrack& track : c.recording.attachFrames)
            {
                w.Put(track.recordId);
                w.Count(track.frames.size());
                for (const auto& [timeMs, frame] : track.frames)
                {
                    w.Put(timeMs);
                    w.Put(frame.bone);
                    w.Array(frame.position);
                    w.Array(frame.rotation);
                }
            }
        }
        void ReadAttachFrames(Reader& r, LoadContext& c)
        {
            c.recording.attachFrames.resize(r.Count(12));
            for (AttachFrameTrack& track : c.recording.attachFrames)
            {
                track.recordId = r.Get<std::uint32_t>();
                track.frames.resize(r.Count(34));
                for (auto& [timeMs, frame] : track.frames)
                {
                    timeMs = r.Get<std::uint32_t>();
                    frame.bone = r.Get<std::int16_t>();
                    r.Array(frame.position);
                    r.Array(frame.rotation);
                }
            }
        }

        // Section "ropes" (ROPE): rope records. playbackLogged is playback
        // state and starts false after a load.
        constexpr std::uint16_t kRopesVersion = 1;
        void WriteSlotEvents(Writer& w, const std::vector<ropes::RopeSlotEvent>& events)
        {
            w.Count(events.size());
            for (const ropes::RopeSlotEvent& event : events)
            {
                w.Put(event.timeMs);
                w.Put(event.slot);
                w.Enum(event.holder);
                w.Id(event.id);
                w.Put(event.bone);
            }
        }
        void ReadSlotEvents(Reader& r, std::vector<ropes::RopeSlotEvent>& events)
        {
            events.resize(r.Count(17));
            for (ropes::RopeSlotEvent& event : events)
            {
                event.timeMs = r.Get<std::uint32_t>();
                event.slot = r.Get<std::uint32_t>();
                event.holder = r.Enum<ropes::RopeHolder>(static_cast<std::uint8_t>(ropes::RopeHolder::Other));
                event.id = r.Id();
                event.bone = r.Get<std::int32_t>();
            }
        }
        void WriteRopes(Writer& w, const SaveContext& c)
        {
            const std::vector<ropes::RopeRecord>& records = c.recording.ropes.Records();
            w.Count(records.size());
            for (const ropes::RopeRecord& record : records)
            {
                w.Put(record.engineId);
                w.Put(record.type);
                w.Put(record.vertices);
                w.Count(record.attachments.size());
                for (const ropes::RopeAttach& attach : record.attachments)
                {
                    w.Id(attach.id);
                    w.Put(attach.bone);
                }
                w.Id(record.nearPed);
                WriteSlotEvents(w, record.slotEvents);
                WriteSlotEvents(w, record.slotState);
                w.Count(record.heldEvents.size());
                for (const auto& [timeMs, held] : record.heldEvents)
                {
                    w.Put(timeMs);
                    w.Bool(held);
                }
                w.Values(record.times);
                w.Values(record.positions);
                w.Values(record.windowCounts);
                w.Put(record.nextStateLogMs);
            }
        }
        void ReadRopes(Reader& r, LoadContext& c)
        {
            std::vector<ropes::RopeRecord>& records = access::Ref<std::vector<ropes::RopeRecord>, access::RopeRecords>(c.recording.ropes);
            records.clear();
            const std::size_t count = r.Count(60);
            if (count > ropes::kMaxRopeRecordsInFile) // the read limit, above the recording cap
                r.Fail();
            for (std::size_t index = 0; index < count && r.Ok(); ++index)
            {
                ropes::RopeRecord record;
                record.engineId = r.Get<std::uint32_t>();
                record.type = r.Get<std::uint32_t>();
                record.vertices = r.Get<std::uint32_t>();
                record.attachments.resize(r.Count(8));
                for (ropes::RopeAttach& attach : record.attachments)
                {
                    attach.id = r.Id();
                    attach.bone = r.Get<std::int32_t>();
                }
                record.nearPed = r.Id();
                ReadSlotEvents(r, record.slotEvents);
                ReadSlotEvents(r, record.slotState);
                record.heldEvents.resize(r.Count(5));
                for (auto& [timeMs, held] : record.heldEvents)
                {
                    timeMs = r.Get<std::uint32_t>();
                    held = r.Bool();
                }
                r.Values(record.times);
                r.Values(record.positions);
                r.Values(record.windowCounts);
                record.nextStateLogMs = r.Get<std::uint32_t>();
                // Playback indexes the positions by sample and vertex.
                if (record.vertices > ropes::kMaxRopeVertices ||
                    record.positions.size() != record.times.size() * record.vertices * 3u ||
                    record.windowCounts.size() != record.times.size())
                {
                    r.Fail();
                }
                records.push_back(std::move(record));
            }
        }

        // Section "timeScale" (TSCL).
        constexpr std::uint16_t kTimeScaleVersion = 1;
        void WriteTimeScale(Writer& w, const SaveContext& c)
        {
            const std::vector<timescale::Sample>& samples = c.recording.timeScale.Samples();
            w.Count(samples.size());
            for (const timescale::Sample& sample : samples)
            {
                w.Put(sample.timeMs);
                w.Put(sample.scale);
            }
        }
        void ReadTimeScale(Reader& r, LoadContext& c)
        {
            auto& samples = access::Ref<std::vector<timescale::Sample>, access::TimeScaleSamples>(c.recording.timeScale);
            samples.resize(r.Count(8));
            std::uint32_t lastMs = 0;
            for (timescale::Sample& sample : samples)
            {
                sample.timeMs = r.Get<std::uint32_t>();
                sample.scale = r.Get<float>();
                if (sample.timeMs < lastMs || !(sample.scale >= timescale::kMinScale && sample.scale <= 1.0f))
                    r.Fail();
                lastMs = sample.timeMs;
            }
        }

        // Section "pose" (POSE): the pose records with their layouts,
        // anchors and chunks. Each track is decoded once on load.
        constexpr std::uint16_t kPoseVersion = 1;
        void WriteLayout(Writer& w, const pose::SkeletonLayout& layout)
        {
            w.Put(layout.boneCount);
            w.Enum(layout.order);
            w.Bool(layout.inheritScale);
            w.Values(layout.parent);
            w.Values(layout.ambiguous);
            w.Values(layout.highPrecision);
            w.Values(layout.evaluationOrder);
            w.Values(layout.unresolvedBones);
        }
        void ReadLayout(Reader& r, pose::SkeletonLayout& layout)
        {
            layout.boneCount = r.Get<std::uint32_t>();
            layout.order = r.Enum<pose::CompositionOrder>(static_cast<std::uint8_t>(pose::CompositionOrder::LocalThenParent));
            layout.inheritScale = r.Bool();
            r.Values(layout.parent);
            r.Values(layout.ambiguous);
            r.Values(layout.highPrecision);
            r.Values(layout.evaluationOrder);
            r.Values(layout.unresolvedBones);
        }
        void WritePose(Writer& w, const SaveContext& c)
        {
            w.Count(c.poses.size());
            for (const pose::PoseRecordExport& record : c.poses)
            {
                w.Put(record.recordId);
                w.Put(record.boneCount);
                const pose::TrackExport& track = record.track;
                w.Put(track.config.rotationEpsilon);
                w.Put(track.config.translationEpsilon);
                w.Put(track.config.translationRange);
                w.Put(track.config.keyframeIntervalMs);
                w.Put(static_cast<std::uint64_t>(track.config.byteBudget));
                w.Count(track.layouts.size());
                for (const pose::SkeletonLayout& layout : track.layouts)
                    WriteLayout(w, layout);
                w.Count(track.anchors.size());
                for (const std::vector<std::uint8_t>& anchor : track.anchors)
                    w.Blob(anchor);
                w.Count(track.chunks.size());
                for (const pose::TrackChunkExport& chunk : track.chunks)
                {
                    w.Put(chunk.layout);
                    w.Put(chunk.anchor);
                    w.Blob(chunk.bytes);
                    w.Values(chunk.frameTimes);
                    w.Values(chunk.frameOffsets);
                }
                w.Put(track.currentLayout);
                w.Put(track.stats.frames);
                w.Put(track.stats.keyframes);
                w.Put(track.stats.relativeKeyframes);
                w.Put(track.stats.fullKeyframeBytes);
                w.Put(track.stats.relativeKeyframeBytes);
                w.Put(track.stats.bytes);
                w.Put(track.stats.droppedChunks);
                w.Put(track.stats.firstTimeMs);
                w.Put(track.stats.lastTimeMs);
            }
        }
        void ReadPose(Reader& r, LoadContext& c)
        {
            c.poses.clear();
            const std::size_t count = r.Count(40);
            for (std::size_t index = 0; index < count && r.Ok(); ++index)
            {
                pose::PoseRecordExport record;
                record.recordId = r.Get<std::uint32_t>();
                record.boneCount = r.Get<std::uint32_t>();
                pose::TrackExport& track = record.track;
                track.config.rotationEpsilon = r.Get<float>();
                track.config.translationEpsilon = r.Get<float>();
                track.config.translationRange = r.Get<float>();
                track.config.keyframeIntervalMs = r.Get<std::uint32_t>();
                track.config.byteBudget = static_cast<std::size_t>(r.Get<std::uint64_t>());
                track.layouts.resize(r.Count(46));
                for (pose::SkeletonLayout& layout : track.layouts)
                    ReadLayout(r, layout);
                track.anchors.resize(r.Count(8));
                for (std::vector<std::uint8_t>& anchor : track.anchors)
                    r.Blob(anchor);
                track.chunks.resize(r.Count(32));
                for (pose::TrackChunkExport& chunk : track.chunks)
                {
                    chunk.layout = r.Get<std::uint32_t>();
                    chunk.anchor = r.Get<std::int32_t>();
                    r.Blob(chunk.bytes.Mutable());
                    r.Values(chunk.frameTimes);
                    r.Values(chunk.frameOffsets);
                }
                track.currentLayout = r.Get<std::uint32_t>();
                track.stats.frames = r.Get<std::uint64_t>();
                track.stats.keyframes = r.Get<std::uint64_t>();
                track.stats.relativeKeyframes = r.Get<std::uint64_t>();
                track.stats.fullKeyframeBytes = r.Get<std::uint64_t>();
                track.stats.relativeKeyframeBytes = r.Get<std::uint64_t>();
                track.stats.bytes = r.Get<std::uint64_t>();
                track.stats.droppedChunks = r.Get<std::uint64_t>();
                track.stats.firstTimeMs = r.Get<std::uint32_t>();
                track.stats.lastTimeMs = r.Get<std::uint32_t>();
                if (!r.Ok())
                    return;
                // The record must decode here, not at the first playback.
                for (const pose::PoseRecordExport& earlier : c.poses)
                {
                    if (earlier.recordId == record.recordId)
                        r.Fail();
                }
                pose::PoseTrack check;
                std::string error;
                if (!check.Import(track, error))
                    r.Fail();
                c.poses.push_back(std::move(record));
            }
        }

        // Section "camera" (CAMR): camera markers. 2: mode, target and follow
        // offset. 3: the full marker (blend on the source marker, Look At,
        // Attach, presets, shake, DOF, speed) and the track's
        // legacyBlendAnyMode. A v2 payload still loads, converted
        // (camera::ConvertV2ToV3).
        constexpr std::uint16_t kCameraVersion = 9; // 9: the marker's whole shake (MarkerShake) and its origin; the old shake fields go
        // 8: bones, near clip, DOF numbers
        // 7: Spline tuning per marker
        // 6: Custom shake (type 11) and its numbers
        // 5: Spline blend (3), shake with movement
        // 4: transition and effects per marker
        constexpr std::uint16_t kCameraMinVersion = 2;
        // A Custom shake's numbers (32 bytes, CAMR v6 to v8).
        constexpr std::size_t kShakeCustomBytes = 32;
        void PutShakeCustom(Writer& w, const editor::ShakeCustom& c)
        {
            w.Put(c.amp);
            w.Put(c.hz);
            w.Put(c.speedAmp);
            w.Put(c.speedHz);
            w.Put(c.rotWeight);
            w.Put(c.posWeight);
            w.Put(c.speedRef);
            w.Put(c.seed);
            w.Put(c.still);
            w.Put(static_cast<std::uint8_t>(0));
        }
        editor::ShakeCustom GetShakeCustom(Reader& r)
        {
            editor::ShakeCustom c{};
            c.amp = r.Get<float>();
            c.hz = r.Get<float>();
            c.speedAmp = r.Get<float>();
            c.speedHz = r.Get<float>();
            c.rotWeight = r.Get<float>();
            c.posWeight = r.Get<float>();
            c.speedRef = r.Get<float>();
            c.seed = r.Get<std::uint16_t>();
            c.still = r.Get<std::uint8_t>();
            c.reserved = r.Get<std::uint8_t>();
            if (r.Ok() && !camera::shake::ValidCustom(c))
                r.Fail();
            return c;
        }

        // A marker's Spline tuning (20 bytes).
        constexpr std::size_t kSplineTuningBytes = 20;
        void PutSplineTuning(Writer& w, const editor::SplineTuning& t)
        {
            w.Put(t.alpha);
            w.Put(t.easeIn);
            w.Put(t.easeOut);
            w.Put(t.weight);
            w.Put(t.pacing);
            w.Put(t.channels);
            w.Put(t.overrides);
            w.Put(static_cast<std::uint8_t>(0));
        }
        editor::SplineTuning GetSplineTuning(Reader& r)
        {
            editor::SplineTuning t{};
            t.alpha = r.Get<float>();
            t.easeIn = r.Get<float>();
            t.easeOut = r.Get<float>();
            t.weight = r.Get<float>();
            t.pacing = r.Get<std::uint8_t>();
            t.channels = r.Get<std::uint8_t>();
            t.overrides = r.Get<std::uint8_t>();
            t.reserved = r.Get<std::uint8_t>();
            if (r.Ok() && !camera::spline::Valid(t))
                r.Fail();
            t.reserved = 0;
            return t;
        }

        // CAMR v9: a marker's shake (61 bytes; 62 with the origin).
        constexpr std::size_t kMarkerShakeBytes = 4 + 2 + 2 + 1 + 9 * 4 + 2 * 8;
        // Gunshot (ShakeType 12) came after the first CAMR v9 builds, whose
        // reader takes the profiles 0..10 only and refuses the whole clip on
        // another (the camera section is required). So a Gunshot marker is
        // written as Explosion (5) with this seed (a profile never reads its
        // seed; only the noise does): an older build opens the clip and plays
        // an Explosion there (the same one shot at that marker, bigger); this
        // one reads it back as Gunshot. Same section version, same size.
        constexpr std::uint16_t kGunshotSeedTag = 0x6A57;
        void PutMarkerShake(Writer& w, const editor::MarkerShake& s)
        {
            const bool gunshot = s.profile == static_cast<std::uint8_t>(editor::ShakeType::Gunshot);
            w.Put(s.on);
            w.Put(gunshot ? static_cast<std::uint8_t>(editor::ShakeType::Explosion) : s.profile);
            w.Put(s.transition);
            w.Put(s.flags);
            w.Put(s.blendMask);
            w.Put(gunshot ? kGunshotSeedTag : s.seed);
            w.Put(s.still);
            w.Put(s.gain);
            w.Put(s.amp);
            w.Put(s.hz);
            w.Put(s.speedAmp);
            w.Put(s.speedHz);
            w.Put(s.rotWeight);
            w.Put(s.posWeight);
            w.Put(s.speedRef);
            w.Put(s.rate);
            w.Put(s.startCycles);
            w.Put(s.startFadeMs);
        }
        editor::MarkerShake GetMarkerShake(Reader& r)
        {
            editor::MarkerShake s{};
            s.on = r.Get<std::uint8_t>();
            s.profile = r.Get<std::uint8_t>();
            s.transition = r.Get<std::uint8_t>();
            s.flags = r.Get<std::uint8_t>();
            s.blendMask = r.Get<std::uint16_t>();
            s.seed = r.Get<std::uint16_t>();
            s.still = r.Get<std::uint8_t>();
            s.gain = r.Get<float>();
            s.amp = r.Get<float>();
            s.hz = r.Get<float>();
            s.speedAmp = r.Get<float>();
            s.speedHz = r.Get<float>();
            s.rotWeight = r.Get<float>();
            s.posWeight = r.Get<float>();
            s.speedRef = r.Get<float>();
            s.rate = r.Get<float>();
            s.startCycles = r.Get<double>();
            s.startFadeMs = r.Get<double>();
            if (s.profile == static_cast<std::uint8_t>(editor::ShakeType::Explosion) && s.seed == kGunshotSeedTag)
            {
                s.profile = static_cast<std::uint8_t>(editor::ShakeType::Gunshot);
                s.seed = 0;
            }
            if (r.Ok() && (!camera::mshake::Usable(s) || !camera::mshake::Same(s, camera::mshake::Clamped(s))))
                r.Fail();
            return s;
        }

        void WriteCamera(Writer& w, const SaveContext& c)
        {
            auto& track = const_cast<camera::Track&>(c.camera);
            w.Put(access::Ref<std::uint32_t, access::CameraNextId>(track));
            w.Bool(track.LegacyBlendAnyMode());
            const std::vector<camera::Keyframe>& keyframes = track.Keyframes();
            w.Count(keyframes.size());
            for (const camera::Keyframe& keyframe : keyframes)
            {
                w.Put(keyframe.id);
                w.Put(keyframe.timeMs);
                w.Put(keyframe.pose.position.x);
                w.Put(keyframe.pose.position.y);
                w.Put(keyframe.pose.position.z);
                w.Put(keyframe.pose.orientation.w);
                w.Put(keyframe.pose.orientation.x);
                w.Put(keyframe.pose.orientation.y);
                w.Put(keyframe.pose.orientation.z);
                w.Put(keyframe.pose.fov);
                w.Enum(keyframe.cameraMode);
                w.Enum(keyframe.markerType);
                w.Enum(keyframe.blend);
                w.Bool(keyframe.ease);
                w.Put(keyframe.lookAtTarget);
                w.Put(keyframe.lookAtOffset.x);
                w.Put(keyframe.lookAtOffset.y);
                w.Put(keyframe.lookAtRoll);
                w.Put(keyframe.attachTarget);
                w.Enum(keyframe.attachType);
                w.Put(keyframe.relPosition.x);
                w.Put(keyframe.relPosition.y);
                w.Put(keyframe.relPosition.z);
                w.Put(keyframe.relRotation.x);
                w.Put(keyframe.relRotation.y);
                w.Put(keyframe.relRotation.z);
                w.Put(keyframe.presetDistanceScale);
                // v9: the old shake fields (type, intensity, speed) are gone.
                w.Enum(keyframe.dof);
                w.Enum(keyframe.focus);
                w.Put(keyframe.dofIntensity);
                w.Put(keyframe.focusDistance);
                w.Put(keyframe.focusTarget);
                w.Put(keyframe.speedIndex);
                w.Bool(keyframe.legacyCurve);
                w.Put(keyframe.editedMask);
                // v4
                w.Put(keyframe.transition);
                w.Put(keyframe.transitionMs);
                w.Put(keyframe.filter);
                w.Put(keyframe.filterIntensity);
                w.Put(keyframe.saturation);
                w.Put(keyframe.contrast);
                w.Put(keyframe.vignette);
                w.Put(keyframe.brightness);
                // v5 (shake motion) and v6 (Custom numbers): gone in v9.
                // v7
                PutSplineTuning(w, keyframe.spline);
                // v8
                w.Put(keyframe.attachBone);
                w.Put(keyframe.lookAtBone);
                w.Put(keyframe.nearClip);
                w.Put(keyframe.dofFStop);
                w.Put(keyframe.dofLensMm);
                // v9
                PutMarkerShake(w, keyframe.shake);
                w.Put(keyframe.origin);
            }
        }
        // v3 payload bytes per marker (the reader's minimum per count).
        constexpr std::size_t kCameraV3Bytes = 127;
        constexpr std::size_t kCameraV8Bytes = 20; // two bones and three floats
        void ReadCameraV2(Reader& r, camera::Track& track, std::vector<camera::Keyframe>& keyframes)
        {
            keyframes.resize(r.Count(52 + 4 + 4 + 12));
            for (camera::Keyframe& keyframe : keyframes)
            {
                keyframe.id = r.Get<std::uint32_t>();
                keyframe.timeMs = r.Get<std::uint64_t>();
                keyframe.pose.position.x = r.Get<float>();
                keyframe.pose.position.y = r.Get<float>();
                keyframe.pose.position.z = r.Get<float>();
                keyframe.pose.orientation.w = r.Get<float>();
                keyframe.pose.orientation.x = r.Get<float>();
                keyframe.pose.orientation.y = r.Get<float>();
                keyframe.pose.orientation.z = r.Get<float>();
                keyframe.pose.fov = r.Get<float>();
                keyframe.easing = r.Enum<editor::Easing>(static_cast<std::uint32_t>(editor::Easing::Hold));
                keyframe.path = r.Enum<editor::Path>(static_cast<std::uint32_t>(editor::Path::Smooth));
                keyframe.mode = r.Enum<editor::CameraMode>(static_cast<std::uint32_t>(editor::CameraMode::Follow));
                keyframe.target = r.Get<std::uint32_t>();
                keyframe.offset.x = r.Get<float>();
                keyframe.offset.y = r.Get<float>();
                keyframe.offset.z = r.Get<float>();
            }
            // The legacy view is what v2 stored; the model comes from it.
            track.ConvertLegacy();
        }
        void ReadCameraV3(Reader& r, camera::Track& track, std::vector<camera::Keyframe>& keyframes, std::uint16_t version)
        {
            const bool v4 = version >= 4;
            const bool v5 = version >= 5;
            const bool v6 = version >= 6;
            const bool v7 = version >= 7;
            const bool v8 = version >= 8;
            const bool v9 = version >= 9; // MarkerShake, no old shake fields
            track.SetLegacyBlendAnyMode(r.Bool());
            keyframes.resize(r.Count(kCameraV3Bytes - (v9 ? 3 : 0) + (v4 ? 9 : 0) + (v5 && !v9 ? 1 : 0) +
                (v6 && !v9 ? kShakeCustomBytes : 0) + (v7 ? kSplineTuningBytes : 0) + (v8 ? kCameraV8Bytes : 0) +
                (v9 ? kMarkerShakeBytes + 1 : 0)));
            for (camera::Keyframe& keyframe : keyframes)
            {
                keyframe.id = r.Get<std::uint32_t>();
                keyframe.timeMs = r.Get<std::uint64_t>();
                keyframe.pose.position.x = r.Get<float>();
                keyframe.pose.position.y = r.Get<float>();
                keyframe.pose.position.z = r.Get<float>();
                keyframe.pose.orientation.w = r.Get<float>();
                keyframe.pose.orientation.x = r.Get<float>();
                keyframe.pose.orientation.y = r.Get<float>();
                keyframe.pose.orientation.z = r.Get<float>();
                keyframe.pose.fov = r.Get<float>();
                keyframe.cameraMode = r.Enum<editor::CameraMode>(static_cast<std::uint32_t>(editor::CameraMode::Overhead));
                // Target and Follow are legacy only; a v3 marker never has them.
                if (keyframe.cameraMode == editor::CameraMode::Target || keyframe.cameraMode == editor::CameraMode::Follow)
                    r.Fail();
                keyframe.markerType = r.Enum<editor::MarkerType>(static_cast<std::uint8_t>(editor::MarkerType::Anchor));
                // v5: Spline; a v3 or v4 marker never has it.
                keyframe.blend = r.Enum<editor::Blend>(static_cast<std::uint8_t>(v5 ? editor::Blend::Spline : editor::Blend::Smooth));
                keyframe.ease = r.Bool();
                keyframe.lookAtTarget = r.Get<std::uint32_t>();
                keyframe.lookAtOffset.x = r.Get<float>();
                keyframe.lookAtOffset.y = r.Get<float>();
                keyframe.lookAtRoll = r.Get<float>();
                keyframe.attachTarget = r.Get<std::uint32_t>();
                keyframe.attachType = r.Enum<editor::AttachType>(static_cast<std::uint8_t>(editor::AttachType::Flexible));
                keyframe.relPosition.x = r.Get<float>();
                keyframe.relPosition.y = r.Get<float>();
                keyframe.relPosition.z = r.Get<float>();
                keyframe.relRotation.x = r.Get<float>();
                keyframe.relRotation.y = r.Get<float>();
                keyframe.relRotation.z = r.Get<float>();
                keyframe.presetDistanceScale = r.Get<float>();
                // v6: Custom (11); an older marker never has it. v3..v8: the
                // old shake, folded into the marker's shake after the load
                // (camera_marker_shake.h Migrate).
                if (!v9)
                {
                    keyframe.legacyShake.type = r.Enum<editor::ShakeType>(static_cast<std::uint8_t>(
                        v6 ? editor::ShakeType::Custom : editor::ShakeType::Wagon));
                    keyframe.legacyShake.intensity = r.Get<std::uint8_t>();
                    keyframe.legacyShake.speedIndex = r.Get<std::uint8_t>();
                }
                keyframe.dof = r.Enum<editor::DofMode>(static_cast<std::uint8_t>(editor::DofMode::None));
                keyframe.focus = r.Enum<editor::FocusMode>(static_cast<std::uint8_t>(editor::FocusMode::Target));
                keyframe.dofIntensity = r.Get<float>();
                keyframe.focusDistance = r.Get<float>();
                keyframe.focusTarget = r.Get<std::uint32_t>();
                keyframe.speedIndex = r.Get<std::uint8_t>();
                keyframe.legacyCurve = r.Bool();
                keyframe.editedMask = r.Get<std::uint64_t>();
                if (v4)
                {
                    keyframe.transition = r.Get<std::uint8_t>();
                    keyframe.transitionMs = r.Get<std::uint16_t>();
                    keyframe.filter = r.Get<std::uint8_t>();
                    keyframe.filterIntensity = r.Get<std::uint8_t>();
                    keyframe.saturation = r.Get<std::int8_t>();
                    keyframe.contrast = r.Get<std::int8_t>();
                    keyframe.vignette = r.Get<std::int8_t>();
                    keyframe.brightness = r.Get<std::int8_t>();
                    if (keyframe.transition > 1 || keyframe.filter >= editor::kMarkerFilterCount || keyframe.filterIntensity > 100)
                        r.Fail();
                }
                if (v5 && !v9)
                {
                    const std::uint8_t motion = r.Get<std::uint8_t>();
                    if (motion >= editor::kShakeMotionCount)
                        r.Fail();
                    keyframe.legacyShake.motion = static_cast<editor::ShakeMotion>(motion);
                }
                if (v6 && !v9)
                    keyframe.legacyShake.custom = GetShakeCustom(r);
                // v7; an older marker takes every field from the defaults.
                keyframe.spline = v7 ? GetSplineTuning(r) : editor::DefaultSplineTuning();
                // v8; an older marker: root, default near clip, DOF from the intensity.
                if (v8)
                {
                    editor::CameraMarkerExtra extra{};
                    extra.fields = editor::CameraExtraAttachBone | editor::CameraExtraLookAtBone | editor::CameraExtraNearClip |
                        editor::CameraExtraDofFStop | editor::CameraExtraDofLens;
                    extra.attachBone = r.Get<std::uint32_t>();
                    extra.lookAtBone = r.Get<std::uint32_t>();
                    extra.nearClip = r.Get<float>();
                    extra.dofFStop = r.Get<float>();
                    extra.dofLensMm = r.Get<float>();
                    const std::uint64_t mask = keyframe.editedMask;
                    if (r.Ok() && !camera::SetMarkerExtra(keyframe, extra))
                        r.Fail();
                    keyframe.editedMask = mask; // as stored (SetMarkerExtra marks it)
                }
                if (v9)
                {
                    keyframe.shake = GetMarkerShake(r);
                    keyframe.origin = r.Get<std::uint8_t>();
                    if (keyframe.origin > camera::kMarkerOriginShakeMigration)
                        r.Fail();
                }
                if (keyframe.legacyShake.intensity < 1 || keyframe.legacyShake.intensity > 10 ||
                    keyframe.legacyShake.speedIndex >= editor::kMarkerSpeedCount ||
                    keyframe.speedIndex >= editor::kMarkerSpeedCount)
                    r.Fail();
            }
            // The old core still evaluates the legacy view.
            track.DeriveLegacy();
        }
        void ReadCamera(Reader& r, LoadContext& c)
        {
            camera::Track& track = c.camera;
            track.Clear();
            const std::uint32_t nextId = r.Get<std::uint32_t>();
            auto& keyframes = access::Ref<std::vector<camera::Keyframe>, access::CameraKeyframes>(track);
            if (c.sectionVersion == 2)
                ReadCameraV2(r, track, keyframes);
            else
                ReadCameraV3(r, track, keyframes, c.sectionVersion);
            std::uint64_t lastMs = 0;
            for (const camera::Keyframe& keyframe : keyframes)
            {
                // Evaluate expects the keyframes sorted by time and ids below nextId.
                if (keyframe.timeMs < lastMs || keyframe.id >= nextId)
                    r.Fail();
                lastMs = keyframe.timeMs;
            }
            access::Ref<std::uint32_t, access::CameraNextId>(track) = nextId;
        }

        // Section "shake events" (CFXE, optional): the shake events track
        // (camera_effects.h), kept with the markers. A clip without it has none.
        // 3: Simple Camera's model, one parameter per event (kind, mode,
        // value); 2: Custom shake (type 11) and its numbers; 1: a whole shake
        // per event. v1 and v2 are converted on load (camera_effects.h
        // ConvertLegacy).
        constexpr std::uint16_t kShakeEventsVersion = 3;
        constexpr std::size_t kShakeEventBytes = 13;    // v1
        constexpr std::size_t kShakeEventV3Bytes = 14;  // id, time, kind, mode, value
        // The events are folded into the markers on load, so a save never
        // writes this section (OmitShakeEvents); the reader stays for clips
        // that have it.
        bool OmitShakeEvents(const SaveContext& c) { return c.camera.LegacyEffects().Empty(); }
        void WriteShakeEvents(Writer& w, const SaveContext& c)
        {
            const camera::fx::EffectTrack& fx = c.camera.LegacyEffects();
            w.Put(fx.NextId());
            w.Count(fx.Events().size());
            for (const camera::fx::ShakeEvent& e : fx.Events())
            {
                w.Put(e.id);
                w.Put(static_cast<std::uint32_t>(e.timeMs));
                w.Put(static_cast<std::uint8_t>(e.kind));
                w.Put(static_cast<std::uint8_t>(e.mode));
                w.Put(e.value);
            }
        }
        void ReadShakeEvents(Reader& r, LoadContext& c)
        {
            const std::uint32_t nextId = r.Get<std::uint32_t>();
            if (c.sectionVersion >= 3)
            {
                std::vector<camera::fx::ShakeEvent> events(r.Count(kShakeEventV3Bytes));
                for (camera::fx::ShakeEvent& e : events)
                {
                    e.id = r.Get<std::uint32_t>();
                    e.timeMs = r.Get<std::uint32_t>();
                    e.kind = static_cast<camera::fx::Kind>(r.Get<std::uint8_t>());
                    e.mode = static_cast<camera::fx::Mode>(r.Get<std::uint8_t>());
                    e.value = r.Get<float>();
                }
                if (r.Ok() && !c.camera.LegacyEffects().Assign(std::move(events), nextId))
                    r.Fail();
                return;
            }
            const bool v2 = c.sectionVersion >= 2;
            std::vector<camera::fx::LegacyEvent> old(r.Count(kShakeEventBytes + (v2 ? kShakeCustomBytes : 0)));
            for (camera::fx::LegacyEvent& e : old)
            {
                r.Get<std::uint32_t>(); // the old id (the converted events take new ones)
                e.timeMs = r.Get<std::uint32_t>();
                const std::uint8_t shake = r.Get<std::uint8_t>();
                e.shake = static_cast<editor::ShakeType>(shake);
                e.intensity = r.Get<std::uint8_t>();
                e.speedIndex = r.Get<std::uint8_t>();
                const std::uint8_t motion = r.Get<std::uint8_t>();
                e.motion = static_cast<editor::ShakeMotion>(motion);
                const std::uint8_t mode = r.Get<std::uint8_t>();
                e.mode = static_cast<camera::fx::Mode>(mode);
                if (v2)
                    e.custom = GetShakeCustom(r);
                if (shake >= editor::kShakeTypeCount || (!v2 && e.shake == editor::ShakeType::Custom) || e.intensity < 1 ||
                    e.intensity > 10 || e.speedIndex >= editor::kMarkerSpeedCount || motion >= editor::kShakeMotionCount ||
                    mode >= camera::fx::kModeCount)
                    r.Fail();
            }
            if (!r.Ok())
                return;
            (void)nextId;
            std::vector<camera::fx::ShakeEvent> events = camera::fx::ConvertLegacy(old);
            const std::uint32_t converted = static_cast<std::uint32_t>(events.size()) + 1;
            if (!c.camera.LegacyEffects().Assign(std::move(events), converted))
                r.Fail();
        }
        void ResetShakeEvents(LoadContext& c) { c.camera.LegacyEffects().Clear(); }

        struct Section
        {
            std::uint32_t tag;
            std::uint16_t version;
            const char* name;
            void (*write)(Writer&, const SaveContext&);
            void (*read)(Reader&, LoadContext&);
            // A section added after format 4 may be absent: the clip still
            // loads, with that member empty.
            bool optional = false;
            // Every section is strict (one version) except where this is set:
            // the oldest payload version its reader still converts. The
            // camera has it (the directed markers are hand-made work),
            // and decals and decalRemoves (a trailing field each).
            std::uint16_t minVersion = 0;
            // An optional section whose payload the reader refuses is left
            // out instead of refusing the clip: reset puts the member back as
            // a clip without the section leaves it (the reader may have
            // written part of it). Every optional section has one (TestOptionalSectionSkip).
            void (*reset)(LoadContext&) = nullptr;
            // An optional section left out of the file when this says so
            // (nothing to keep), so a reader that does not know the section
            // still opens the clip.
            bool (*omit)(const SaveContext&) = nullptr;
            // The version this save writes when it depends on the content (the
            // payload must match it; null: `version`). Between minVersion
            // and version, so this build reads it back.
            std::uint16_t (*saveVersion)(const SaveContext&) = nullptr;
        };

        // File order. A new clip member gets a section here; a changed one
        // bumps its version.
        // The resets of the optional sections (Section::reset).
        template <auto Member>
        void ResetMember(LoadContext& c)
        {
            c.recording.*Member = {};
        }
        void ResetEnvironmentTrack(LoadContext& c) { c.recording.environment.track.clear(); }
        void ResetSkyTrack(LoadContext& c)
        {
            c.recording.environment.sky.clear();
            c.recording.environment.skyPeriods = {};
        }
        void ResetEnvironmentEdit(LoadContext& c) { c.recording.environment.edit = {}; }
        void ResetTimecycle(LoadContext& c) { c.recording.environment.timecycle.clear(); }
        void ResetMoon(LoadContext& c) { c.recording.environment.moon.clear(); }
        void ResetWorldInfo(LoadContext& c)
        {
            const WorldState fresh;
            WorldState& world = c.recording.world;
            world.recorded = fresh.recorded;
            world.camp = fresh.camp;
            world.chapter = fresh.chapter;
            world.flags = fresh.flags;
            world.candidateListHash = fresh.candidateListHash;
            world.candidateCount = fresh.candidateCount;
        }
        void ResetWorld(LoadContext& c)
        {
            WorldState& world = c.recording.world;
            world.ipls.clear();
            world.spheres.clear();
            world.interiors.clear();
            world.entitySets.clear();
        }
        // The attaches live on the registeredFx records (a required section).
        void ResetRegisteredFxAttaches(LoadContext& c)
        {
            for (entity::RegisteredFxTrack& track : c.recording.registeredFx)
                track.attachTimeline.clear();
        }

        const Section kSections[] = {
            {Tag("IDAL"), kIdsVersion, "ids", WriteIds, ReadIds},
            {Tag("LIFE"), kLifecycleVersion, "lifecycle", WriteLifecycle, ReadLifecycle},
            {Tag("SEGM"), kSegmentsVersion, "segments", WriteSegments, ReadSegments},
            {Tag("CREA"), kCreatedVersion, "created", WriteCreated, ReadCreated},
            {Tag("WTRK"), kWeaponTracksVersion, "weaponTracks", WriteWeaponTracks, ReadWeaponTracks},
            {Tag("STRP"), kStrapTracksVersion, "strapTracks", WriteStrapTracks, ReadStrapTracks},
            {Tag("ATFR"), kAttachFramesVersion, "attachFrames", WriteAttachFrames, ReadAttachFrames},
            {Tag("ROPE"), kRopesVersion, "ropes", WriteRopes, ReadRopes},
            {Tag("SHOT"), kShotsVersion, "shots", WriteShots, ReadShots, false, 1},
            {Tag("SPCH"), kSpeechVersion, "speechLines", WriteSpeechLines, ReadSpeechLines, false, kSpeechMinVersion},
            {Tag("FOOT"), kFootstepsVersion, "footsteps", WriteFootsteps, ReadFootsteps, false, kFootstepsMinVersion},
            {Tag("EXPL"), kExplosionsVersion, "explosions", WriteExplosions, ReadExplosions},
            {Tag("SURF"), kSurfacesVersion, "surfaces", WriteSurfaces, ReadSurfaces},
            {Tag("FXEV"), kFxVersion, "fx", WriteFxTrack, ReadFxTrack, false, kFxMinVersion},
            {Tag("RGFX"), kRegisteredFxVersion, "registeredFx", WriteRegisteredFx, ReadRegisteredFx, false,
                kRegisteredFxMinVersion},
            {Tag("DECL"), kDecalsVersion, "decals", WriteDecals, ReadDecals, false, kDecalsMinVersion},
            {Tag("DCRM"), kDecalRemovesVersion, "decalRemoves", WriteDecalRemoves, ReadDecalRemoves, false,
                kDecalRemovesMinVersion},
            {Tag("PDCL"), kProjDecalsVersion, "projDecals", WriteProjDecals, ReadProjDecals},
            {Tag("GORE"), kGoreVersion, "gore", WriteGoreTrack, ReadGoreTrack},
            {Tag("GHIT"), kGoreHitsVersion, "goreHits", WriteGoreHits, ReadGoreHits},
            {Tag("TSCL"), kTimeScaleVersion, "timeScale", WriteTimeScale, ReadTimeScale},
            {Tag("BLOD"), kBloodVersion, "blood", WriteBlood, ReadBlood, false, kBloodMinVersion},
            {Tag("FIRS"), kFireStartsVersion, "fireStarts", WriteFireStarts, ReadFireStarts, false, 1},
            {Tag("FSTP"), kFireStopsVersion, "fireStops", WriteFireStops, ReadFireStops},
            {Tag("FITM"), kFireItemsVersion, "fireItems", WriteFireItems, ReadFireItems},
            {Tag("META"), kMetaVersion, "meta", WriteMeta, ReadMeta},
            {Tag("AREA"), kAreaVersion, "area", WriteArea, ReadArea},
            {Tag("MELE"), kMeleeHitsVersion, "meleeHits", WriteMeleeHits, ReadMeleeHits},
            {Tag("BIMP"), kBodyImpactsVersion, "bodyImpacts", WriteBodyImpacts, ReadBodyImpacts},
            {Tag("SNDS"), kSoundsVersion, "sounds", WriteSounds, ReadSounds, false, kSoundsMinVersion},
            {Tag("CSND"), kCollisionSoundsVersion, "collisionSounds", WriteCollisionSounds, ReadCollisionSounds},
            {Tag("WOBJ"), kWorldObjectsVersion, "worldObjects", WriteWorldObjects, ReadWorldObjects},
            {Tag("FRAG"), kFragBreaksVersion, "fragBreaks", WriteFragBreaks, ReadFragBreaks},
            {Tag("GCAM"), kGameCameraVersion, "gameCamera", WriteGameCamera, ReadGameCamera},
            {Tag("ENVR"), kEnvironmentVersion, "environment", WriteEnvironment, ReadEnvironment},
            {Tag("PLAC"), kPlaceVersion, "place", WritePlace, ReadPlace, true, 0, ResetMember<&Recording::place>},
            {Tag("ENVT"), kEnvironmentTrackVersion, "environment track", WriteEnvironmentTrack, ReadEnvironmentTrack, true, kEnvironmentTrackMinVersion, ResetEnvironmentTrack}, // v2: the weather transition
            {Tag("SKYT"), kSkyTrackVersion, "sky track", WriteSkyTrack, ReadSkyTrack, true, 0, ResetSkyTrack},
            {Tag("ENVO"), kEnvironmentEditVersion, "environment edit", WriteEnvironmentEdit, ReadEnvironmentEdit, true, kEnvironmentEditMinVersion, ResetEnvironmentEdit},
            {Tag("WINF"), kWorldInfoVersion, "world info", WriteWorldInfo, ReadWorldInfo, true, 0, ResetWorldInfo}, // read by the clip list
            {Tag("DOOR"), kDoorsVersion, "doors", WriteDoors, ReadDoors, true, 0, ResetMember<&Recording::doors>},
            {Tag("WRLD"), kWorldVersion, "world", WriteWorld, ReadWorld, true, 0, ResetWorld},
            {Tag("ABIL"), kAbilitiesVersion, "abilities", WriteAbilities, ReadAbilities, true, 0, ResetMember<&Recording::abilities>},
            {Tag("VEHL"), kVehicleLooksVersion, "vehicle looks", WriteVehicleLooks, ReadVehicleLooks, true, kVehicleLooksMinVersion, ResetMember<&Recording::vehicleLooks>},
            {Tag("WCND"), kWeaponConditionsVersion, "weapon conditions", WriteWeaponConditions, ReadWeaponConditions, true, kWeaponConditionsMinVersion, ResetMember<&Recording::weaponConditions>}, // v2: the whole weapon look
            {Tag("RFXA"), kRegisteredFxAttachesVersion, "registered fx attaches", WriteRegisteredFxAttaches, ReadRegisteredFxAttaches, true, 0, ResetRegisteredFxAttaches},
            {Tag("CUTS"), kCutsVersion, "cuts", WriteCuts, ReadCuts, true, 0, ResetMember<&Recording::cuts>},
            {Tag("SPAN"), kSpansVersion, "spans", WriteSpans, ReadSpans, true, 0, ResetMember<&Recording::spans>},
            {Tag("SCOR"), kScorchedVersion, "scorched", WriteScorched, ReadScorched, true, 0, ResetMember<&Recording::scorched>},
            {Tag("AANI"), kAnimAudioVersion, "anim audio", WriteAnimAudio, ReadAnimAudio, true, 0, ResetMember<&Recording::animAudio>},
            {Tag("VWIN"), kVehicleWindowsVersion, "vehicle windows", WriteVehicleWindows, ReadVehicleWindows, true, 0, ResetMember<&Recording::vehicleWindows>},
            {Tag("APPC"), kAppearanceChangesVersion, "appearance changes", WriteAppearanceChanges, ReadAppearanceChanges, true, 0, ResetMember<&Recording::appearances>},
            {Tag("MPED"), kMetapedVersion, "metaped", WriteMetapeds, ReadMetapeds, true, kMetapedV1Version, ResetMember<&Recording::metapeds>, nullptr, MetapedSaveVersion}, // v2: shop items, written only with them
            {Tag("VPRT"), kVehiclePartsVersion, "vehicle parts", WriteVehicleParts, ReadVehicleParts, true, 0, ResetMember<&Recording::vehicleParts>},
            {Tag("VDOR"), kVehicleDoorsVersion, "vehicle doors", WriteVehicleDoors, ReadVehicleDoors, true, 1, ResetMember<&Recording::vehicleDoors>, OmitVehicleDoors},
            {Tag("VBRN"), kVehicleBurntVersion, "vehicle burnt", WriteVehicleBurnt, ReadVehicleBurnt, true, 0, ResetMember<&Recording::vehicleBurnt>},
            {Tag("BLVL"), kBurnLevelsVersion, "burn levels", WriteBurnLevels, ReadBurnLevels, true, 0, ResetMember<&Recording::burnLevels>},
            {Tag("FREC"), kFireCharsVersion, "fire chars", WriteFireChars, ReadFireChars, true, kFireCharsMinVersion, ResetMember<&Recording::fireChars>},
            {Tag("ESND"), kEntitySoundsVersion, "entity sounds", WriteEntitySounds, ReadEntitySounds, true, 1, ResetMember<&Recording::entitySounds>},
            {Tag("TRNC"), kTrainsVersion, "trains", WriteTrains, ReadTrains, true, 0, ResetMember<&Recording::trains>},
            {Tag("SCEN"), kSoundCensusVersion, "sound census", WriteSoundCensus, ReadSoundCensus, true, 0, ResetMember<&Recording::soundCensus>},
            {Tag("GSND"), kGeneralSoundsVersion, "general sounds", WriteGeneralSounds, ReadGeneralSounds, true, kGeneralSoundsMinVersion, ResetMember<&Recording::generalSounds>},
            {Tag("SGEN"), kSoundGenericVersion, "sound generic", WriteSoundGeneric, ReadSoundGeneric, true, kSoundGenericMinVersion, ResetMember<&Recording::soundGeneric>}, // v2 the audio base, v3 init variables, v4 value dwords, v5 site ids
            {Tag("PVIS"), kPedVisibilityVersion, "ped visibility", WritePedVisibility, ReadPedVisibility, true, 1, ResetMember<&Recording::pedVisibility>},
            {Tag("FPCE"), kFragPiecesVersion, "frag pieces", WriteFragPieces, ReadFragPieces, true, 1, ResetMember<&Recording::fragPieces>},
            {Tag("EQRY"), kEntityQueriesVersion, "entity queries", WriteEntityQueries, ReadEntityQueries, true, 1, ResetMember<&Recording::entityQueries>}, // state sounds
            {Tag("MIXP"), kMixProbeVersion, "mix probe", WriteMixProbe, ReadMixProbe, true, 1, ResetMember<&Recording::mixProbe>, OmitMixProbe}, // evidence only
            {Tag("RTGT"), kRenderTargetsVersion, "render targets", WriteRenderTargets, ReadRenderTargets, true, 1, ResetMember<&Recording::renderTargets>, OmitRenderTargets},
            {Tag("LSTN"), kGameListenerVersion, "game listener", WriteGameListener, ReadGameListener, true, 1, ResetMember<&Recording::gameListener>, OmitGameListener, GameListenerSaveVersion}, // v2: microphone and points, written only with them
            {Tag("WNDA"), kWindAudioVersion, "wind audio", WriteWindAudio, ReadWindAudio, true, 1, ResetMember<&Recording::windAudio>, OmitWindAudio}, // evidence only
            {Tag("MIXS"), kMixScenesVersion, "mix scenes", WriteMixScenes, ReadMixScenes, true, 1, ResetMember<&Recording::mixScenes>, OmitMixScenes, MixScenesSaveVersion}, // v2 variables; v3 flags, written only with flags
            {Tag("CATS"), kCategoryStateVersion, "category state", WriteCategoryState, ReadCategoryState, true, 1, ResetMember<&Recording::categoryState>, OmitCategoryState},
            {Tag("FVAR"), kFoleyVariantsVersion, "foley variants", WriteFoleyVariants, ReadFoleyVariants, true, 1, ResetMember<&Recording::foleyVariants>},
            {Tag("PSPD"), kPedSpeedVersion, "ped speed", WritePedSpeed, ReadPedSpeed, true, 1, ResetMember<&Recording::pedSpeed>, OmitPedSpeed},
            {Tag("SHVR"), kShaderVarsVersion, "shader vars", WriteShaderVars, ReadShaderVars, true, 1, ResetMember<&Recording::shaderVars>, OmitShaderVars},
            {Tag("FSEV"), kFootEventsVersion, "foot events", WriteFootEvents, ReadFootEvents, true, 1, ResetMember<&Recording::footEvents>, OmitFootEvents},
            {Tag("FMOV"), kFootMovementVersion, "foot movement", WriteFootMovement, ReadFootMovement, true, 1, ResetMember<&Recording::footMovement>, OmitFootMovement},
            {Tag("FXSS"), kFxSettingsVersion, "fx settings", WriteFxSettings, ReadFxSettings, true, 1, ResetFxSettings, OmitFxSettings}, // after FXEV (event indices)
            {Tag("RGSS"), kRegisteredFxSettingsVersion, "registered fx settings", WriteRegisteredFxSettings, ReadRegisteredFxSettings, true, 1, ResetRegisteredFxSettings, OmitRegisteredFxSettings}, // after RGFX (track indices)
            {Tag("WFXI"), kWeaponFxIntentsVersion, "weapon fx intents", WriteWeaponFxIntents, ReadWeaponFxIntents, true, 1, ResetMember<&Recording::weaponFxIntents>, OmitWeaponFxIntents},
            {Tag("TCMD"), kTimecycleVersion, "timecycle", WriteTimecycle, ReadTimecycle, true, 1, ResetTimecycle},
            {Tag("MOON"), kMoonVersion, "moon", WriteMoon, ReadMoon, true, 1, ResetMoon},
            {Tag("SCNY"), kSceneryVersion, "scenery", WriteScenery, ReadScenery, true, 1, ResetMember<&Recording::scenery>}, // v1 still read
            {Tag("SCCH"), kSceneryChildrenVersion, "scenery children", WriteSceneryChildren, ReadSceneryChildren, true, 0, ResetMember<&Recording::sceneryChildren>, OmitSceneryChildren}, // after SCNY (parent indices)
            {Tag("SBLV"), kSceneryBurnLevelsVersion, "scenery burn levels", WriteSceneryBurnLevels, ReadSceneryBurnLevels, true, 0, ResetMember<&Recording::sceneryBurnLevels>, OmitSceneryBurnLevels}, // after SCNY (indices)
            {Tag("OLOK"), kObjectLooksVersion, "object looks", WriteObjectLooks, ReadObjectLooks, true, 0, ResetObjectLooks, OmitObjectLooks}, // after SCNY (scenery indices)
            {Tag("SCHT"), kSceneryHatsVersion, "scenery hats", WriteSceneryHats, ReadSceneryHats, true, 0, ResetSceneryHats, OmitSceneryHats}, // after SCNY (entry indices)
            {Tag("SPRQ"), kSpeechRequestsVersion, "speech requests", WriteSpeechRequests, ReadSpeechRequests, true, 1, ResetSpeechRequests, OmitSpeechRequests}, // after SPCH (one row per line); v2 the sound set, v1 still read
            {Tag("POSE"), kPoseVersion, "pose", WritePose, ReadPose},
            {Tag("CAMR"), kCameraVersion, "camera", WriteCamera, ReadCamera, false, kCameraMinVersion},
            {Tag("CFXE"), kShakeEventsVersion, "shake events", WriteShakeEvents, ReadShakeEvents, true, 1, ResetShakeEvents, OmitShakeEvents}, // read only (folded into the markers)
        };
        constexpr std::size_t kSectionCount = sizeof(kSections) / sizeof(kSections[0]);

        // Whether the clip was saved on the running build, from its META,
        // read ahead of the sections that need it (t_sameBuild). A clip
        // without META, or a build identity not set (the tests), counts as
        // the same build; a clip without META is of 1491.50.
        void MetaSameBuild(const std::uint8_t* data, std::size_t size)
        {
            t_running1491 = !g_buildTimestamp || (g_buildTimestamp == k1491Timestamp && g_buildImageSize == k1491ImageSize);
            t_sameBuild = t_running1491;
            for (std::size_t offset = kHeaderBytes; offset < size && size - offset >= kSectionHeaderBytes;)
            {
                Reader header(data + offset, kSectionHeaderBytes);
                const std::uint32_t tag = header.Get<std::uint32_t>();
                header.Get<std::uint16_t>();
                const std::uint64_t length = header.Get<std::uint64_t>();
                offset += kSectionHeaderBytes;
                if (length > size - offset || size - offset - length < sizeof(std::uint64_t))
                    return; // the loop below reports it
                if (tag == Tag("META"))
                {
                    Reader meta(data + offset, static_cast<std::size_t>(length));
                    const std::uint32_t timestamp = meta.Get<std::uint32_t>();
                    const std::uint64_t image = meta.Get<std::uint64_t>();
                    t_sameBuild = !g_buildTimestamp || !timestamp ||
                        (timestamp == g_buildTimestamp && image == g_buildImageSize);
                    return;
                }
                offset += static_cast<std::size_t>(length) + sizeof(std::uint64_t);
            }
        }

        // Sections written for a feature since removed. Decode skips them unread (no reader, no hash check, any
        // version, even twice) and lists them as skipped; a save never
        // writes them. The tag stays reserved: never reuse it.
        struct RetiredSection
        {
            std::uint32_t tag;
            const char* name;
            const char* why;
        };
        const RetiredSection kRetiredSections[] = {
            {Tag("WGPU"), "water gpu", "GPU water recording, removed"},
        };

        const RetiredSection* FindRetired(std::uint32_t tag)
        {
            for (const RetiredSection& section : kRetiredSections)
                if (section.tag == tag)
                    return &section;
            return nullptr;
        }

        std::string Quoted(const char* name) { return std::string("'") + name + "'"; }

        // Checks that need more than one section: indices into another track.
        bool CrossCheck(const Recording& recording, std::string& error)
        {
            const std::size_t starts = recording.fireStarts.Events().size();
            for (const entity::FireStopEvent& stop : recording.fireStops.Events())
            {
                if (stop.start >= starts)
                {
                    error = "section 'fireStops': start index past fireStarts";
                    return false;
                }
            }
            for (const entity::FireItemSample& item : recording.fireItems.Events())
            {
                if (item.start >= starts)
                {
                    error = "section 'fireItems': start index past fireStarts";
                    return false;
                }
            }
            return true;
        }
    }

    bool ValidateLoaded(Recording& recording, camera::Track& camera, std::uint64_t durationMs, std::string& error)
    {
        // A scenery child whose parent is not a scenery
        // entry (the SCNY section left out or refused) is left out with it.
        recording.sceneryChildren.erase(std::remove_if(recording.sceneryChildren.begin(), recording.sceneryChildren.end(),
                                            [&](const SceneryChild& child) { return child.parent >= recording.scenery.size(); }),
            recording.sceneryChildren.end());
        // Timestamps are u32 ms everywhere below the header.
        if (durationMs > 0xFFFFFFFFull)
        {
            error = "duration " + std::to_string(durationMs) + " ms past the timestamps' range";
            return false;
        }
        const auto finite = [](float value) { return std::isfinite(value); };
        const auto finite3 = [&](const editor::Vec3& v) { return finite(v.x) && finite(v.y) && finite(v.z); };
        auto& keyframes = access::Ref<std::vector<camera::Keyframe>, access::CameraKeyframes>(camera);
        const std::uint32_t nextId = access::Ref<std::uint32_t, access::CameraNextId>(camera);
        std::vector<std::uint32_t> ids;
        ids.reserve(keyframes.size());
        for (camera::Keyframe& keyframe : keyframes)
        {
            const camera::Quaternion& q = keyframe.pose.orientation;
            const float norm = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
            if (!finite3(keyframe.pose.position) || !finite(q.w) || !finite(q.x) || !finite(q.y) || !finite(q.z) ||
                !finite(norm) || norm < 1e-6f || !finite(keyframe.pose.fov) || !finite3(keyframe.offset) ||
                !finite(keyframe.lookAtOffset.x) || !finite(keyframe.lookAtOffset.y) || !finite(keyframe.lookAtRoll) ||
                !finite3(keyframe.relPosition) || !finite3(keyframe.relRotation) || !finite(keyframe.presetDistanceScale) ||
                !finite(keyframe.dofIntensity) || !finite(keyframe.focusDistance) || keyframe.timeMs > 0xFFFFFFFFull)
            {
                error = "section 'camera': marker " + std::to_string(keyframe.id) + " has an unusable value";
                return false;
            }
            keyframe.pose.fov = (std::min)(170.0f, (std::max)(1.0f, keyframe.pose.fov));
            if (keyframe.id >= nextId)
            {
                error = "section 'camera': marker id past the next id";
                return false;
            }
            ids.push_back(keyframe.id);
        }
        std::sort(ids.begin(), ids.end());
        if (std::adjacent_find(ids.begin(), ids.end()) != ids.end())
        {
            error = "section 'camera': two markers share an id";
            return false;
        }
        for (const camera::GameSample& sample : recording.gameCamera)
        {
            if (!finite3(sample.position) || !finite3(sample.rotation) || !finite(sample.fov))
            {
                error = "section 'gameCamera': a sample has an unusable value";
                return false;
            }
        }
        for (BurnLevelTrack& track : recording.burnLevels)
            for (auto& state : track.states)
                state.second = (std::min)(1.0f, (std::max)(0.0f, state.second));
        // A scenery burn level track whose
        // index is no scenery entry (SCNY left out or refused) goes; the
        // levels bounded like the entities'.
        recording.sceneryBurnLevels.erase(std::remove_if(recording.sceneryBurnLevels.begin(), recording.sceneryBurnLevels.end(),
                                              [&](const BurnLevelTrack& track) { return track.id >= recording.scenery.size(); }),
            recording.sceneryBurnLevels.end());
        for (BurnLevelTrack& track : recording.sceneryBurnLevels)
            for (auto& state : track.states)
                state.second = (std::min)(1.0f, (std::max)(0.0f, state.second));
        // A scenery look track whose index is no
        // scenery entry goes; every tint bounded to the engine's six bits.
        recording.sceneryObjectLooks.erase(std::remove_if(recording.sceneryObjectLooks.begin(), recording.sceneryObjectLooks.end(),
                                               [&](const ObjectLookTrack& track) { return track.id >= recording.scenery.size(); }),
            recording.sceneryObjectLooks.end());
        for (std::vector<ObjectLookTrack>* tracks : {&recording.objectLooks, &recording.sceneryObjectLooks})
            for (ObjectLookTrack& track : *tracks)
                for (auto& state : track.states)
                    SanitizeObjectLook(state.second);
        error.clear();
        return true;
    }

    // The camera section's payload on its own, for the project file
    // (replay_project.h): each clip of a project keeps its edit markers in
    // the same bytes the clip file uses, with the section version beside them.
    std::uint16_t CameraSectionVersion() { return kCameraVersion; }

    // The shake events alone (a project's clip entry).
    std::uint16_t ShakeEventsSectionVersion() { return kShakeEventsVersion; }

    std::vector<std::uint8_t> EncodeShakeEvents(const camera::Track& track)
    {
        static const Recording kNoRecording;
        static const std::vector<pose::PoseRecordExport> kNoPoses;
        std::vector<std::uint8_t> out;
        Writer writer(out);
        WriteShakeEvents(writer, SaveContext{kNoRecording, kNoPoses, track, Image{}});
        return out;
    }

    bool DecodeShakeEvents(const std::uint8_t* data, std::size_t size, std::uint16_t version, camera::Track& track,
        std::string& error)
    {
        if (version < 1 || version > kShakeEventsVersion)
        {
            error = "shake events: version " + std::to_string(version) + " not readable";
            return false;
        }
        Recording noRecording;
        std::vector<pose::PoseRecordExport> noPoses;
        camera::Track loaded;
        LoadContext context{noRecording, noPoses, loaded, Image{}};
        context.sectionVersion = version;
        Reader reader(data, size);
        ReadShakeEvents(reader, context);
        if (!reader.Ok() || !reader.AtEnd())
        {
            error = "shake events: damaged payload";
            return false;
        }
        track.LegacyEffects() = loaded.LegacyEffects();
        return true;
    }

    std::vector<std::uint8_t> EncodeCameraTrack(const camera::Track& track)
    {
        static const Recording kNoRecording;
        static const std::vector<pose::PoseRecordExport> kNoPoses;
        std::vector<std::uint8_t> out;
        Writer writer(out);
        WriteCamera(writer, SaveContext{kNoRecording, kNoPoses, track, Image{}});
        return out;
    }

    bool DecodeCameraTrack(const std::uint8_t* data, std::size_t size, std::uint16_t version, camera::Track& track,
        std::string& error)
    {
        if (version < kCameraMinVersion || version > kCameraVersion)
        {
            error = "camera markers: version " + std::to_string(version) + " not readable";
            return false;
        }
        Recording noRecording;
        std::vector<pose::PoseRecordExport> noPoses;
        camera::Track loaded;
        LoadContext context{noRecording, noPoses, loaded, Image{}};
        context.sectionVersion = version;
        Reader reader(data, size);
        ReadCamera(reader, context);
        if (!reader.Ok() || !reader.AtEnd())
        {
            error = "camera markers: damaged payload";
            return false;
        }
        // The same checks as a clip's markers (finite pose, ids unique).
        if (!ValidateLoaded(noRecording, loaded, 0, error))
            return false;
        track = std::move(loaded);
        return true;
    }

    void SetBuildIdentity(std::uint32_t peTimestamp, std::uint64_t imageSize)
    {
        g_buildTimestamp = peTimestamp;
        g_buildImageSize = imageSize;
    }

    void SetSiteMap(const sig::SiteMap& map)
    {
        static sig::SiteMap stored;
        stored = map;
        g_sites = &stored;
    }

    const std::vector<RetiredSectionInfo>& RetiredSections()
    {
        static const std::vector<RetiredSectionInfo> sections = [] {
            std::vector<RetiredSectionInfo> out;
            for (const RetiredSection& section : kRetiredSections)
                out.push_back({section.tag, section.name, section.why});
            return out;
        }();
        return sections;
    }

    const std::vector<SectionInfo>& Sections()
    {
        static const std::vector<SectionInfo> sections = [] {
            std::vector<SectionInfo> out;
            for (const Section& section : kSections)
                out.push_back({section.tag, section.version, section.name, section.optional, section.reset != nullptr});
            return out;
        }();
        return sections;
    }

    bool Encode(
        const Recording& recording,
        const std::vector<pose::PoseRecordExport>& poses,
        const camera::Track& camera,
        std::uint64_t durationMs,
        std::uintptr_t imageBase,
        std::size_t imageSize,
        std::vector<std::uint8_t>& out,
        std::string& error)
    {
        out.clear();
        const SaveContext context{recording, poses, camera, Image{imageBase, imageSize}};
        Writer header(out);
        header.Raw(reinterpret_cast<const std::uint8_t*>(kMagic), sizeof(kMagic));
        header.Put(kFormatVersion);
        header.Put(durationMs);
        std::vector<std::uint8_t> payload;
        for (const Section& section : kSections)
        {
            if (section.omit && section.omit(context))
                continue;
            payload.clear();
            Writer writer(payload);
            section.write(writer, context);
            header.Put(section.tag);
            header.Put(section.saveVersion ? section.saveVersion(context) : section.version);
            header.Put(static_cast<std::uint64_t>(payload.size()));
            header.Raw(payload.data(), payload.size());
            header.Put(Fnv64(payload.data(), payload.size()));
        }
        error.clear();
        return true;
    }

    static bool DecodeUnguarded(
        const std::uint8_t* data,
        std::size_t size,
        std::uintptr_t imageBase,
        std::size_t imageSize,
        Recording& recording,
        std::vector<pose::PoseRecordExport>& poses,
        camera::Track& camera,
        std::uint64_t& durationMs,
        std::string& error,
        std::vector<SkippedSection>* skipped)
    {
        if (skipped)
            skipped->clear();
        if (!data || size < kHeaderBytes || std::memcmp(data, kMagic, sizeof(kMagic)) != 0)
        {
            error = "not a clip file (magic)";
            return false;
        }
        Reader header(data + sizeof(kMagic), size - sizeof(kMagic));
        const std::uint32_t format = header.Get<std::uint32_t>();
        const std::uint64_t duration = header.Get<std::uint64_t>();
        if (format != kFormatVersion)
        {
            error = "format version " + std::to_string(format) + ", this build reads " +
                std::to_string(kFormatVersion);
            return false;
        }

        Recording loadedRecording;
        std::vector<pose::PoseRecordExport> loadedPoses;
        camera::Track loadedCamera;
        LoadContext context{loadedRecording, loadedPoses, loadedCamera, Image{imageBase, imageSize}};
        bool seen[kSectionCount] = {};
        MetaSameBuild(data, size);
        std::size_t offset = kHeaderBytes;
        while (offset < size)
        {
            if (size - offset < kSectionHeaderBytes)
            {
                error = "truncated section header at byte " + std::to_string(offset);
                return false;
            }
            Reader sectionHeader(data + offset, kSectionHeaderBytes);
            const std::uint32_t tag = sectionHeader.Get<std::uint32_t>();
            const std::uint16_t version = sectionHeader.Get<std::uint16_t>();
            const std::uint64_t length = sectionHeader.Get<std::uint64_t>();
            offset += kSectionHeaderBytes;

            std::size_t index = 0;
            while (index < kSectionCount && kSections[index].tag != tag)
                ++index;
            if (index == kSectionCount)
            {
                // A retired section is stepped over unread.
                if (const RetiredSection* retired = FindRetired(tag))
                {
                    if (length > size - offset || size - offset - length < sizeof(std::uint64_t))
                    {
                        error = "section " + Quoted(retired->name) + " truncated";
                        return false;
                    }
                    if (skipped)
                        skipped->push_back(SkippedSection{retired->name, std::string("retired_not_read (") + retired->why + ")"});
                    offset += static_cast<std::size_t>(length) + sizeof(std::uint64_t);
                    continue;
                }
                error = "unknown section '" + TagText(tag) + "'";
                return false;
            }
            const Section& section = kSections[index];
            if (seen[index])
            {
                error = "section " + Quoted(section.name) + " appears twice";
                return false;
            }
            seen[index] = true;
            const std::uint16_t oldest = section.minVersion ? section.minVersion : section.version;
            if (version > section.version || version < oldest)
            {
                error = "section " + Quoted(section.name) + " version " + std::to_string(version) +
                    ", this build reads " + std::to_string(section.version);
                return false;
            }
            if (length > size - offset || size - offset - length < sizeof(std::uint64_t))
            {
                error = "section " + Quoted(section.name) + " truncated";
                return false;
            }
            std::uint64_t stored = 0;
            std::memcpy(&stored, data + offset + length, sizeof(stored));
            if (stored != Fnv64(data + offset, static_cast<std::size_t>(length)))
            {
                error = "section " + Quoted(section.name) + " corrupt (hash)";
                return false;
            }
            context.sectionVersion = version;
            Reader reader(data + offset, static_cast<std::size_t>(length));
            section.read(reader, context);
            if (!reader.Ok() || !reader.AtEnd())
            {
                // An optional section the reader refuses is left out: its
                // reset puts the member back as a clip without it, and the
                // clip opens.
                if (section.optional && section.reset)
                {
                    section.reset(context);
                    if (skipped)
                    {
                        std::string reason = !reader.Ok()
                            ? "malformed at payload byte " + std::to_string(reader.Position()) + " of " + std::to_string(reader.Size())
                            : std::to_string(reader.Size() - reader.Position()) + " bytes left over after the payload";
                        skipped->push_back(SkippedSection{section.name, std::move(reason)});
                    }
                    offset += static_cast<std::size_t>(length) + sizeof(std::uint64_t);
                    continue;
                }
                error = "section " + Quoted(section.name) + " malformed";
                return false;
            }
            offset += static_cast<std::size_t>(length) + sizeof(std::uint64_t);
        }
        for (std::size_t index = 0; index < kSectionCount; ++index)
        {
            if (!seen[index] && !kSections[index].optional)
            {
                error = "missing section " + Quoted(kSections[index].name);
                return false;
            }
        }
        if (!CrossCheck(loadedRecording, error))
            return false;
        if (!ValidateLoaded(loadedRecording, loadedCamera, duration, error))
            return false;
        // A window of the buffer saved while the recording ran (before 1.0.0's fix) has
        // its environment track but not the recorded flag: the flag and the clock
        // are set from the track, as the cut now does (EnvironmentFromTrack). A clip
        // without a track stays without its environment.
        if (!loadedRecording.environment.recorded)
            loadedRecording.environment.repairedOnLoad = EnvironmentFromTrack(
                loadedRecording.environment, loadedRecording.environment.track, 0, duration);
        // An old clip's marker shake and
        // shake events go into the markers (the report and the track before
        // stay in camera.Migration() for the proof and SHAKE_MIGRATION).
        camera::mshake::Migrate(loadedCamera);

        recording = std::move(loadedRecording);
        poses = std::move(loadedPoses);
        camera = std::move(loadedCamera);
        durationMs = duration;
        error.clear();
        return true;
    }

    bool Decode(
        const std::uint8_t* data,
        std::size_t size,
        std::uintptr_t imageBase,
        std::size_t imageSize,
        Recording& recording,
        std::vector<pose::PoseRecordExport>& poses,
        camera::Track& camera,
        std::uint64_t& durationMs,
        std::string& error)
    {
        return Decode(data, size, imageBase, imageSize, recording, poses, camera, durationMs, error, nullptr);
    }

    bool Decode(
        const std::uint8_t* data,
        std::size_t size,
        std::uintptr_t imageBase,
        std::size_t imageSize,
        Recording& recording,
        std::vector<pose::PoseRecordExport>& poses,
        camera::Track& camera,
        std::uint64_t& durationMs,
        std::string& error,
        std::vector<SkippedSection>* skipped)
    {
        // The counts are bounded by the payload, but
        // the decoded objects are larger than their bytes: an allocation
        // that fails is a refused clip, the outputs untouched.
        try
        {
            return DecodeUnguarded(data, size, imageBase, imageSize, recording, poses, camera, durationMs, error, skipped);
        }
        catch (const std::bad_alloc&)
        {
            error = "not enough memory to decode the clip";
        }
        catch (const std::length_error&)
        {
            error = "clip too large to decode";
        }
        return false;
    }

    static bool WriteClipBytes(const std::uint8_t* data, std::size_t size, const std::wstring& path, std::string& error)
    {
        const std::filesystem::path target(path);
        std::filesystem::path temporary = target;
        temporary += L".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                error = "cannot create the temporary file";
                return false;
            }
            file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
            file.flush();
            if (!file)
            {
                file.close();
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                error = "write failed";
                return false;
            }
        }
        std::error_code renameError;
        std::filesystem::rename(temporary, target, renameError);
        if (renameError)
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            error = "rename failed: " + renameError.message();
            return false;
        }
        error.clear();
        return true;
    }

    static std::uint32_t MsSince(std::chrono::steady_clock::time_point start)
    {
        return static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    }

    static bool SaveUnguarded(
        const Recording& recording,
        const std::vector<pose::PoseRecordExport>& poses,
        const camera::Track& camera,
        std::uint64_t durationMs,
        std::uintptr_t imageBase,
        std::size_t imageSize,
        const std::wstring& path,
        std::string& error,
        const SaveOptions& options,
        SaveInfo& info)
    {
        auto started = std::chrono::steady_clock::now();
        std::vector<std::uint8_t> bytes;
        if (!Encode(recording, poses, camera, durationMs, imageBase, imageSize, bytes, error))
            return false;
        info.encodeMs = MsSince(started);
        info.clipBytes = bytes.size();
        if (options.pack)
        {
            clipcompress::SavePackOptions pack;
            pack.zstdLevel = options.zstdLevel;
            pack.threads = options.threads;
            pack.availableBytes = options.availableBytes;
            pack.cancel = options.cancel;
            clipcompress::SavePackResult result;
            std::vector<std::uint8_t> packed;
            const bool ok = clipcompress::PackForSave(bytes, pack, packed, result);
            info.packMs = static_cast<std::uint32_t>(result.packMs);
            info.verifyMs = static_cast<std::uint32_t>(result.verifyMs);
            if (ok)
            {
                bytes.clear();
                bytes.shrink_to_fit();
                started = std::chrono::steady_clock::now();
                if (!WriteClipBytes(packed.data(), packed.size(), path, error))
                    return false;
                info.writeMs = MsSince(started);
                info.packed = true;
                info.fileBytes = packed.size();
                return true;
            }
            // Not packed: written plain, as before (the pack may have taken
            // the clip's bytes; they are encoded again).
            info.fallback = result.fallback.empty() ? std::string("pack failed") : result.fallback;
            packed.clear();
            packed.shrink_to_fit();
            if (bytes.empty())
            {
                if (!Encode(recording, poses, camera, durationMs, imageBase, imageSize, bytes, error))
                    return false;
            }
        }
        started = std::chrono::steady_clock::now();
        if (!WriteClipBytes(bytes.data(), bytes.size(), path, error))
            return false;
        info.writeMs = MsSince(started);
        info.fileBytes = bytes.size();
        return true;
    }

    bool Save(
        const Recording& recording,
        const std::vector<pose::PoseRecordExport>& poses,
        const camera::Track& camera,
        std::uint64_t durationMs,
        std::uintptr_t imageBase,
        std::size_t imageSize,
        const std::wstring& path,
        std::string& error)
    {
        return Save(recording, poses, camera, durationMs, imageBase, imageSize, path, error, SaveOptions{}, nullptr);
    }

    bool Save(
        const Recording& recording,
        const std::vector<pose::PoseRecordExport>& poses,
        const camera::Track& camera,
        std::uint64_t durationMs,
        std::uintptr_t imageBase,
        std::size_t imageSize,
        const std::wstring& path,
        std::string& error,
        const SaveOptions& options,
        SaveInfo* info)
    {
        SaveInfo local;
        SaveInfo& out = info ? *info : local;
        out = SaveInfo{};
        // The encoded copy of a large clip may not
        // fit; that is a failed save (the caller keeps the clip), not an
        // exception through the writer thread.
        try
        {
            return SaveUnguarded(recording, poses, camera, durationMs, imageBase, imageSize, path, error, options, out);
        }
        catch (const std::bad_alloc&)
        {
            error = "not enough memory to write the clip";
        }
        catch (const std::length_error&)
        {
            error = "clip too large to write";
        }
        std::error_code ignored;
        std::filesystem::path temporary(path);
        temporary += L".tmp";
        std::filesystem::remove(temporary, ignored);
        return false;
    }

    bool Load(
        const std::wstring& path,
        std::uintptr_t imageBase,
        std::size_t imageSize,
        Recording& recording,
        std::vector<pose::PoseRecordExport>& poses,
        camera::Track& camera,
        std::uint64_t& durationMs,
        std::string& error)
    {
        return Load(path, imageBase, imageSize, recording, poses, camera, durationMs, error, nullptr);
    }

    bool CheckLoadBudget(std::uint64_t fileBytes, std::uint64_t clipBytes, bool compressed, std::uint64_t availableBytes,
        std::string& error, bool packed)
    {
        constexpr std::uint64_t kMb = 1024 * 1024;
        if (fileBytes > clipcompress::kMaxClipBytes || clipBytes > clipcompress::kMaxClipBytes)
        {
            error = "clip too large (" + std::to_string((compressed ? clipBytes : fileBytes) / kMb) + " MB, the limit is " +
                std::to_string(clipcompress::kMaxClipBytes / kMb) + " MB)";
            return false;
        }
        if (availableBytes == 0)
            return true; // unknown: the load's own allocation failures still come back as errors
        // The file's bytes and the decompressed copy exist together (a
        // packed file: the file and its streams, then the streams, up to
        // 1.25 x the clip, and the clip; counted as all three); then the
        // clip's bytes and the decoded clip (the file's buffer is gone).
        const std::uint64_t clip = compressed ? clipBytes : fileBytes;
        const std::uint64_t whileDecompressing = packed ? fileBytes + 2 * clipBytes + clipBytes / 4
            : compressed                               ? fileBytes + clipBytes
                                                       : fileBytes;
        const std::uint64_t whileDecoding = clip + kDecodeFactor * clip;
        const std::uint64_t peak = (std::max)(whileDecompressing, whileDecoding);
        if (peak + kLoadReserveBytes > availableBytes)
        {
            error = "not enough memory to open the clip (needs about " + std::to_string(peak / kMb) + " MB, " +
                std::to_string(availableBytes / kMb) + " MB free)";
            return false;
        }
        return true;
    }

    namespace
    {

        bool LoadChecked(
            const std::wstring& path,
            std::uintptr_t imageBase,
            std::size_t imageSize,
            Recording& recording,
            std::vector<pose::PoseRecordExport>& poses,
            camera::Track& camera,
            std::uint64_t& durationMs,
            std::string& error,
            LoadInfo* info)
        {
            std::vector<std::uint8_t> bytes;
            {
                std::ifstream file(std::filesystem::path(path), std::ios::binary);
                if (!file)
                {
                    error = "cannot open the file";
                    return false;
                }
                // Size and first bytes before the
                // whole file is read (a large file of another kind, or a
                // clip too large for the memory left, is refused here).
                std::error_code sizeError;
                const std::uint64_t fileBytes = std::filesystem::file_size(std::filesystem::path(path), sizeError);
                if (sizeError)
                {
                    error = "read failed";
                    return false;
                }
                std::uint8_t head[32]{};
                const std::size_t headBytes = static_cast<std::size_t>((std::min)(fileBytes, std::uint64_t{sizeof(head)}));
                file.read(reinterpret_cast<char*>(head), static_cast<std::streamsize>(headBytes));
                if (static_cast<std::size_t>(file.gcount()) != headBytes)
                {
                    error = "read failed";
                    return false;
                }
                const bool compressed = clipcompress::IsCompressed(head, headBytes);
                if (!compressed && (headBytes < kHeaderBytes || std::memcmp(head, kMagic, sizeof(kMagic)) != 0))
                {
                    error = "not a clip file (magic)";
                    return false;
                }
                if (!compressed)
                {
                    std::uint32_t format = 0;
                    std::memcpy(&format, head + sizeof(kMagic), sizeof(format));
                    if (format != kFormatVersion)
                    {
                        error = "format version " + std::to_string(format) + ", this build reads " +
                            std::to_string(kFormatVersion);
                        return false;
                    }
                }
                const bool packed = clipcompress::IsPacked(head, headBytes);
                const std::uint64_t clipBytes = compressed ? clipcompress::ContentSize(head, headBytes) : fileBytes;
                if (packed && clipBytes == 0)
                {
                    error = "packed clip: bad header";
                    return false;
                }
                if (!CheckLoadBudget(fileBytes, clipBytes, compressed, clipcompress::AvailableMemoryBytes(), error, packed))
                    return false;
                bytes.resize(static_cast<std::size_t>(fileBytes));
                file.seekg(0);
                file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (file.bad() || static_cast<std::uint64_t>(file.gcount()) != fileBytes)
                {
                    error = "read failed";
                    return false;
                }
            }
            LoadInfo loaded;
            loaded.fileBytes = bytes.size();
            // A compressed clip, decompressed once into memory (the file's
            // buffer released as soon as it is not needed).
            if (clipcompress::IsCompressed(bytes.data(), bytes.size()))
            {
                const auto startedAt = std::chrono::steady_clock::now();
                loaded.packed = clipcompress::IsPacked(bytes.data(), bytes.size());
                std::vector<std::uint8_t> clip;
                if (!clipcompress::DecompressConsume(std::move(bytes), clip, error))
                    return false;
                loaded.compressed = true;
                loaded.decompressMs = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - startedAt).count());
                bytes.swap(clip);
            }
            loaded.clipBytes = bytes.size();
            if (info)
                *info = loaded;
            return Decode(bytes.data(), bytes.size(), imageBase, imageSize, recording, poses, camera, durationMs, error,
                info ? &info->skipped : nullptr); // the optional sections left out
        }
    }

    bool Load(
        const std::wstring& path,
        std::uintptr_t imageBase,
        std::size_t imageSize,
        Recording& recording,
        std::vector<pose::PoseRecordExport>& poses,
        camera::Track& camera,
        std::uint64_t& durationMs,
        std::string& error,
        LoadInfo* info)
    {
        // An allocation the memory left cannot hold
        // is a failed load (the clip open stays), not an exception through
        // the script thread.
        try
        {
            return LoadChecked(path, imageBase, imageSize, recording, poses, camera, durationMs, error, info);
        }
        catch (const std::bad_alloc&)
        {
            error = "not enough memory to open the clip";
        }
        catch (const std::length_error&)
        {
            error = "clip too large";
        }
        return false;
    }

    // Clip names "NNN-Place": see clip_file.h.
    std::string ClipPlaceTag(const char* town, const char* district, const char* state)
    {
        // Latin-1 letters (UTF-8 C3 80 to C3 BF) to ASCII; "" drops one.
        static const char* const kLatin1[64] = {
            "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
            "D", "N", "O", "O", "O", "O", "O", "", "O", "U", "U", "U", "U", "Y", "Th", "ss",
            "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
            "d", "n", "o", "o", "o", "o", "o", "", "o", "u", "u", "u", "u", "y", "th", "y"};
        constexpr std::size_t kMaxTag = 40;
        const char* const sources[3] = {town, district, state};
        for (const char* source : sources)
        {
            if (!source)
                continue;
            std::string tag;
            bool newWord = true;
            const auto add = [&](char c) {
                if (tag.size() >= kMaxTag)
                    return;
                if (newWord && c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 'a' + 'A');
                tag += c;
                newWord = false;
            };
            for (std::size_t n = 0; source[n] && n < 256; ++n)
            {
                const unsigned char c = static_cast<unsigned char>(source[n]);
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                    add(static_cast<char>(c));
                else if (c == 0xC3 && static_cast<unsigned char>(source[n + 1]) >= 0x80 &&
                         static_cast<unsigned char>(source[n + 1]) <= 0xBF)
                {
                    for (const char* ascii = kLatin1[static_cast<unsigned char>(source[++n]) - 0x80]; *ascii; ++ascii)
                        add(*ascii);
                }
                else if (c == '\'' || c >= 0x80)
                    continue; // an apostrophe or another non-ASCII byte: no new word
                else
                    newWord = true;
            }
            if (!tag.empty())
                return tag;
        }
        return "Wild";
    }

    bool ClipNumberOf(const std::wstring& name, std::uint32_t& number)
    {
        const std::size_t slash = name.find_last_of(L"\\/");
        const std::size_t start = slash == std::wstring::npos ? 0 : slash + 1;
        std::size_t at = start;
        std::uint64_t value = 0;
        while (at < name.size() && name[at] >= L'0' && name[at] <= L'9' && at - start < 9)
            value = value * 10 + static_cast<std::uint64_t>(name[at++] - L'0');
        if (at - start < 3 || at >= name.size() || name[at] != L'-')
            return false;
        number = static_cast<std::uint32_t>(value);
        return true;
    }

    std::uint32_t NextClipNumber(const std::vector<std::wstring>& names)
    {
        std::uint32_t highest = 0;
        for (const std::wstring& name : names)
        {
            std::uint32_t number = 0;
            if (ClipNumberOf(name, number))
                highest = (std::max)(highest, number);
        }
        return highest + 1;
    }

    std::wstring NumberedClipBase(std::uint32_t number, const std::string& placeTag, const std::wstring& suffix)
    {
        wchar_t digits[16]{};
        std::swprintf(digits, 16, L"%03u-", number);
        return std::wstring(digits) + std::wstring(placeTag.begin(), placeTag.end()) + suffix;
    }
}
