#include "clip_file.h"

#include "camera_marker_shake.h"

#include "camera_spline.h"
#include "clip_compress.h"
#include "world_state.h"
#include "object_check.h" // kMaxVisibilityFlips
#include "frag_piece.h"   // the loose pieces' groups
#include "hat_look.h"     // the scenery hats' layer cap
#include "weapon_look_core.h" // WCND version 2

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

// Several clip members keep their state private and have no bulk setter
// (the lifecycle blocks, the id allocator, the rope records, a transform
// track's samples, the time scale samples, the camera keyframes). Instead
// of adding accessors to those headers, this file names those members
// through explicit template instantiation, where the language does not apply
// access checking. Only this file reads or writes them, and only to copy a
// whole member in or out.
namespace replay::clipfile::access
{
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
        using entity::ReplayId;

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

        struct Image
        {
            std::uintptr_t base = 0;
            std::size_t size = 0;
        };

        // A pointer into the game image as an offset from its base, 0 when
        // it points anywhere else (heap, stack, null).
        std::uint64_t ToRva(std::uint64_t address, const Image& image)
        {
            if (image.base == 0 || address <= image.base || address - image.base >= image.size)
                return 0;
            return address - image.base;
        }

        // Back to an address in this process's image; 0 stays 0 and an
        // offset past the image (another game build) becomes 0.
        std::uint64_t FromRva(std::uint64_t rva, const Image& image)
        {
            if (rva == 0 || image.base == 0 || rva >= image.size)
                return 0;
            return static_cast<std::uint64_t>(image.base) + rva;
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

        // Session pointers inside the raw engine blobs.
        constexpr std::size_t kDecalSettingsInfo = 0x88;   // info record, kept as (hash, index)
        constexpr std::size_t kBloodRecordEntity = 0x78;   // entity, kept as ref78Entity
        constexpr std::size_t kGoreContextPed = 0x00;      // ped, kept as entity
        constexpr std::size_t kGoreContextInflictor = 0x08; // inflictor, kept as inflictor
        constexpr std::size_t kFireDescEntityA = 0x40;     // kept as entityA
        constexpr std::size_t kFireDescEntityB = 0x48;     // kept as entityB
        constexpr std::size_t kFireDescCulprit = 0xC0;
        constexpr std::size_t kFireDescImage = 0xF0;       // pointer into the image: RVA
        // Sound init params: playback copies [0, 0xC8), [0xE8, 0x12C) and
        // [0x12E, 0x140) (the tracker at +0xC8, the wave slot, category and
        // environment group up to +0xE8 and the u16 handle at +0x12C are the
        // engine's), so those are not written. The tail after the handle is
        // kept: it holds value fields with non-zero defaults (ids -1, 0xFF, a
        // flags byte 0x20; ctor 0x487E34) that playback needs (audio_observer.cpp
        // CopyRecordedSoundParams). Only the handle is left out.
        constexpr std::size_t kSoundParamsBytes = 0x140;
        constexpr std::size_t kSoundParamsEngineStart = 0xC8;
        constexpr std::size_t kSoundParamsEngineEnd = 0xE8;
        constexpr std::size_t kSoundParamsHandle = 0x12C;
        constexpr std::size_t kSoundParamsHandleBytes = 2;
        constexpr std::size_t kFireInfoBytes = 0xA0;
        constexpr std::size_t kRegisteredFxMaxPlanes = 16;

        class Writer
        {
        public:
            explicit Writer(std::vector<std::uint8_t>& out) : m_out(out) {}

            template <typename T>
            void Put(T value)
            {
                static_assert(std::is_arithmetic_v<T>, "plain values only");
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
                m_out.insert(m_out.end(), bytes, bytes + sizeof(T));
            }
            void Bool(bool value) { Put<std::uint8_t>(value ? 1u : 0u); }
            void Id(ReplayId id) { Put(id.Packed()); }
            template <typename E>
            void Enum(E value)
            {
                Put(static_cast<std::underlying_type_t<E>>(value));
            }
            template <typename T, std::size_t N>
            void Array(const T (&values)[N])
            {
                for (const T& value : values)
                    Put(value);
            }
            void Count(std::size_t count) { Put(static_cast<std::uint64_t>(count)); }
            template <typename T>
            void Values(const std::vector<T>& values)
            {
                Count(values.size());
                for (const T& value : values)
                    Put(value);
            }
            void Raw(const std::uint8_t* data, std::size_t size) { m_out.insert(m_out.end(), data, data + size); }
            void Blob(const std::vector<std::uint8_t>& bytes)
            {
                Count(bytes.size());
                Raw(bytes.data(), bytes.size());
            }

        private:
            std::vector<std::uint8_t>& m_out;
        };

        class Reader
        {
        public:
            Reader(const std::uint8_t* data, std::size_t size) : m_data(data), m_size(size) {}

            template <typename T>
            T Get()
            {
                static_assert(std::is_arithmetic_v<T>, "plain values only");
                T value{};
                if (!m_ok || m_size - m_pos < sizeof(T))
                {
                    m_ok = false;
                    return value;
                }
                std::memcpy(&value, m_data + m_pos, sizeof(T));
                m_pos += sizeof(T);
                return value;
            }
            bool Bool()
            {
                const std::uint8_t value = Get<std::uint8_t>();
                if (value > 1)
                    m_ok = false;
                return value != 0;
            }
            ReplayId Id() { return ReplayId::FromPacked(Get<std::uint32_t>()); }
            // An enum value within [0, maxValue].
            template <typename E>
            E Enum(std::underlying_type_t<E> maxValue)
            {
                const auto value = Get<std::underlying_type_t<E>>();
                if (value > maxValue)
                    m_ok = false;
                return static_cast<E>(value);
            }
            template <typename T, std::size_t N>
            void Array(T (&values)[N])
            {
                for (T& value : values)
                    value = Get<T>();
            }
            // An element count, refused when the rest of the payload cannot
            // hold that many elements of at least minBytes each.
            std::size_t Count(std::size_t minBytes)
            {
                const std::uint64_t count = Get<std::uint64_t>();
                if (!m_ok)
                    return 0;
                if (minBytes == 0)
                    minBytes = 1;
                if (count > (m_size - m_pos) / minBytes)
                {
                    m_ok = false;
                    return 0;
                }
                return static_cast<std::size_t>(count);
            }
            template <typename T>
            void Values(std::vector<T>& values)
            {
                const std::size_t count = Count(sizeof(T));
                values.resize(count);
                for (T& value : values)
                    value = Get<T>();
            }
            void Raw(std::uint8_t* data, std::size_t size)
            {
                if (!m_ok || m_size - m_pos < size)
                {
                    m_ok = false;
                    return;
                }
                std::memcpy(data, m_data + m_pos, size);
                m_pos += size;
            }
            void Blob(std::vector<std::uint8_t>& bytes)
            {
                const std::size_t count = Count(1);
                bytes.resize(count);
                if (count)
                    Raw(bytes.data(), count);
            }
            void Fail() { m_ok = false; }
            bool Ok() const { return m_ok; }
            bool AtEnd() const { return m_pos == m_size; }
            std::size_t Position() const { return m_pos; } // where a refused payload stopped
            std::size_t Size() const { return m_size; }

        private:
            const std::uint8_t* m_data = nullptr;
            std::size_t m_size = 0;
            std::size_t m_pos = 0;
            bool m_ok = true;
        };

        struct SaveContext
        {
            const Recording& recording;
            const std::vector<pose::PoseRecordExport>& poses;
            const camera::Track& camera;
            Image image;
        };

        struct LoadContext
        {
            Recording& recording;
            std::vector<pose::PoseRecordExport>& poses;
            camera::Track& camera;
            Image image;
            // The version of the section being read (a section with a
            // minVersion reads older payloads, see Section).
            std::uint16_t sectionVersion = 0;
        };

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

        // Timed event tracks: count, then each event.
        template <typename Event, typename WriteOne>
        void WriteEvents(Writer& w, const std::vector<Event>& events, WriteOne writeOne)
        {
            w.Count(events.size());
            for (const Event& event : events)
                writeOne(w, event);
        }

        template <typename Track, typename ReadOne>
        void ReadEvents(Reader& r, Track& track, std::size_t minBytes, ReadOne readOne)
        {
            track.Begin();
            const std::size_t count = r.Count(minBytes);
            std::uint32_t lastMs = 0;
            for (std::size_t index = 0; index < count && r.Ok(); ++index)
            {
                typename std::decay_t<decltype(track.Events())>::value_type event;
                readOne(r, event);
                // Add clamps a late time; a file must already be in order.
                if (event.timeMs < lastMs)
                    r.Fail();
                lastMs = event.timeMs;
                track.Add(event);
            }
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

        // Section "shots" (SHOT). The entity references in the fire info
        // are session pointers (playback writes the puppets there): zeroed.
        // Version 2 adds the shooter's local player flag at the
        // end of each shot (version 1 reads it as unknown).
        constexpr std::uint16_t kShotsVersion = 2;
        void WriteShot(Writer& w, const entity::ShotEvent& shot)
        {
            w.Put(shot.timeMs);
            w.Id(shot.shooter);
            w.Id(shot.weapon);
            w.Put(shot.weaponHash);
            WriteVec3(w, shot.muzzle);
            WriteVec3(w, shot.direction);
            WriteVec3(w, shot.impact);
            w.Bool(shot.hasImpact);
            WriteVec3(w, shot.impactNormal);
            w.Bool(shot.hasNormal);
            w.Put(shot.muzzleFx);
            w.Put(shot.impactFx);
            w.Put(shot.muzzleFlags);
            w.Bool(shot.hasMuzzleFlags);
            std::array<std::uint8_t, kFireInfoBytes> info = shot.fireInfo;
            for (const entity::ShotEvent::FireRefField& ref : shot.fireRefs)
                ZeroBytes(info.data(), info.size(), ref.offset, sizeof(std::uint64_t));
            w.Raw(info.data(), info.size());
            w.Bool(shot.hasFireInfo);
            w.Count(shot.fireRefs.size());
            for (const entity::ShotEvent::FireRefField& ref : shot.fireRefs)
            {
                w.Put(ref.offset);
                w.Enum(ref.role);
                w.Id(ref.target);
                w.Id(ref.via);
                w.Put(ref.viaOffset);
            }
            w.Put(shot.shooterPlayer);
        }
        void ReadShot(Reader& r, entity::ShotEvent& shot, std::uint16_t version)
        {
            shot.timeMs = r.Get<std::uint32_t>();
            shot.shooter = r.Id();
            shot.weapon = r.Id();
            shot.weaponHash = r.Get<std::uint32_t>();
            shot.muzzle = ReadVec3(r);
            shot.direction = ReadVec3(r);
            shot.impact = ReadVec3(r);
            shot.hasImpact = r.Bool();
            shot.impactNormal = ReadVec3(r);
            shot.hasNormal = r.Bool();
            shot.muzzleFx = r.Get<std::uint32_t>();
            shot.impactFx = r.Get<std::uint32_t>();
            shot.muzzleFlags = r.Get<std::uint32_t>();
            shot.hasMuzzleFlags = r.Bool();
            r.Raw(shot.fireInfo.data(), shot.fireInfo.size());
            shot.hasFireInfo = r.Bool();
            shot.fireRefs.resize(r.Count(13));
            for (entity::ShotEvent::FireRefField& ref : shot.fireRefs)
            {
                ref.offset = r.Get<std::uint16_t>();
                ref.role = static_cast<entity::ShotEvent::FireRef>(r.Get<std::uint8_t>());
                if (ref.role < entity::ShotEvent::FireRef::Shooter || ref.role > entity::ShotEvent::FireRef::Unknown ||
                    ref.offset + sizeof(std::uint64_t) > kFireInfoBytes)
                {
                    r.Fail();
                }
                ref.target = r.Id();
                ref.via = r.Id();
                ref.viaOffset = r.Get<std::uint16_t>();
            }
            shot.shooterPlayer = version >= 2 ? r.Get<std::uint8_t>() : 0;
            if (shot.shooterPlayer > 2)
                r.Fail();
        }
        void WriteShots(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.shots.Events(), WriteShot); }
        void ReadShots(Reader& r, LoadContext& c)
        {
            ReadEvents(r, c.recording.shots, 240,
                [&](Reader& reader, entity::ShotEvent& shot) { ReadShot(reader, shot, c.sectionVersion); });
        }

        // Section "speechLines" (SPCH). voiceObject is a session pointer: 0.
        // Version 2 adds endMs (the natural end, 0 when unknown); a version 1
        // reads it as 0. Version 3 adds startedBeforeMs (a
        // line adopted already playing); older versions read it as 0.
        constexpr std::uint16_t kSpeechVersion = 3;
        constexpr std::uint16_t kSpeechMinVersion = 1;
        void WriteSpeech(Writer& w, const entity::SpeechEvent& e)
        {
            w.Put(e.timeMs);
            w.Id(e.speaker);
            w.Put(e.voiceHash);
            w.Put(e.contextPartialHash);
            w.Put(e.variation);
            w.Put(e.requestedContextPartialHash);
            w.Put(e.voiceObjectType);
            w.Put(e.voiceObjectPartial);
            w.Put(e.dataType);
            w.Put(e.path);
            w.Put(e.coreMs);
            w.Bool(e.timedByStart);
            w.Put(e.stopMs);
            w.Put(e.stopReason);
            w.Put(e.endMs);
            w.Put(e.startedBeforeMs);
        }
        void ReadSpeech(Reader& r, entity::SpeechEvent& e, std::uint16_t version)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.speaker = r.Id();
            e.voiceHash = r.Get<std::uint32_t>();
            e.contextPartialHash = r.Get<std::uint32_t>();
            e.variation = r.Get<std::int32_t>();
            e.requestedContextPartialHash = r.Get<std::uint32_t>();
            e.voiceObject = 0;
            e.voiceObjectType = r.Get<std::uint8_t>();
            e.voiceObjectPartial = r.Get<std::uint32_t>();
            e.dataType = r.Get<std::uint32_t>();
            e.path = r.Get<std::uint32_t>();
            e.coreMs = r.Get<std::uint32_t>();
            e.timedByStart = r.Bool();
            e.stopMs = r.Get<std::uint32_t>();
            e.stopReason = r.Get<std::uint32_t>();
            e.endMs = version >= 2 ? r.Get<std::uint32_t>() : 0;
            e.startedBeforeMs = version >= 3 ? r.Get<std::uint32_t>() : 0;
        }
        void WriteSpeechLines(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.speechLines.Events(), WriteSpeech); }
        void ReadSpeechLines(Reader& r, LoadContext& c)
        {
            ReadEvents(r, c.recording.speechLines, 50,
                [&](Reader& reader, entity::SpeechEvent& e) { ReadSpeech(reader, e, c.sectionVersion); });
        }

        // Section "speech requests" (SPRQ, speech_request.h), optional: per speech line, in the order of the
        // speechLines section (written before it in the file), whether the
        // request fields were read and their values. Left out when no line
        // has them; a clip without it plays its lines with the ctor's fields.
        // v2 (speech_conversation.h): each row
        // also has the line's sound set (read flag, name hash, ref); a v1
        // row has none (the playback then takes the set from the line's own
        // recorded sounds, ESND).
        constexpr std::uint16_t kSpeechRequestsVersion = 2;
        bool OmitSpeechRequests(const SaveContext& c)
        {
            for (const entity::SpeechEvent& e : c.recording.speechLines.Events())
                if (e.request.read || e.request.setRead)
                    return false;
            return true;
        }
        void WriteSpeechRequests(Writer& w, const SaveContext& c)
        {
            const auto& events = c.recording.speechLines.Events();
            w.Put(static_cast<std::uint16_t>(speech::request::kFieldCount));
            w.Count(events.size());
            for (const entity::SpeechEvent& e : events)
            {
                w.Bool(e.request.read);
                for (std::size_t f = 0; f < speech::request::kFieldCount; ++f)
                    w.Put(e.request.value[f]);
                w.Bool(e.request.setRead); // v2
                w.Put(e.request.setHash);
                w.Put(e.request.setRef);
            }
        }
        void ReadSpeechRequests(Reader& r, LoadContext& c)
        {
            const bool withSet = c.sectionVersion >= 2;
            const std::uint16_t fields = r.Get<std::uint16_t>();
            const std::size_t count = r.Count(1 + 4u * fields + (withSet ? 9u : 0u));
            // One row per speech line, the same field count as this build.
            if (!r.Ok() || fields != speech::request::kFieldCount || count != c.recording.speechLines.Events().size())
            {
                r.Fail();
                return;
            }
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                speech::request::Fields& request = c.recording.speechLines.At(n).request;
                request.read = r.Bool();
                for (std::size_t f = 0; f < speech::request::kFieldCount; ++f)
                    request.value[f] = r.Get<std::uint32_t>();
                request.setRead = false;
                request.setHash = request.setRef = 0;
                if (withSet)
                {
                    request.setRead = r.Bool();
                    request.setHash = r.Get<std::uint32_t>();
                    request.setRef = r.Get<std::uint32_t>();
                }
            }
        }
        void ResetSpeechRequests(LoadContext& c)
        {
            const std::size_t count = c.recording.speechLines.Events().size();
            for (std::size_t n = 0; n < count; ++n)
                c.recording.speechLines.At(n).request = speech::request::Fields{};
        }

        // Section "footsteps" (FOOT). Version 2 adds the step container of
        // the foot (materialHash, materialRef); a version 1 payload loads them
        // as 0 (the puppet's own surface). Version 3 adds the foot's wetness
        // (one byte, 0 unknown). Version 4 adds the foot's mix (known byte,
        // volume f32, high-pass and low-pass u16). Version 5 adds the drive
        // (known bits, base f32, impact f32). Version 6 adds the ground layers
        // (known byte, five u16 thousandths) and the audio LOD byte.
        constexpr std::uint16_t kFootstepsVersion = 6;
        constexpr std::uint16_t kFootstepsMinVersion = 1;
        void WriteFootstep(Writer& w, const entity::FootstepEvent& e)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Put(e.eventId);
            w.Put(e.arg3);
            w.Put(e.foot);
            w.Put(e.arg5);
            w.Put(e.flag);
            w.Put(e.arg8);
            w.Put(e.arg7);
            w.Put(e.materialHash);
            w.Put(e.materialRef);
            w.Put(e.wetness);
            w.Put(e.mixKnown);
            w.Put(e.mixVolume);
            w.Put(e.mixHighPass);
            w.Put(e.mixLowPass);
            w.Put(e.driveKnown);
            w.Put(e.driveBase);
            w.Put(e.driveImpact);
            w.Put(e.layersKnown);
            for (const std::uint16_t layer : e.layer)
                w.Put(layer);
            w.Put(e.audioLod);
        }
        void ReadFootstep(Reader& r, entity::FootstepEvent& e, std::uint16_t version)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            e.eventId = r.Get<std::int32_t>();
            e.arg3 = r.Get<std::uint32_t>();
            e.foot = r.Get<std::uint8_t>();
            e.arg5 = r.Get<std::uint8_t>();
            e.flag = r.Get<std::uint8_t>();
            e.arg8 = r.Get<std::uint8_t>();
            e.arg7 = r.Get<std::uint32_t>();
            e.materialHash = version >= 2 ? r.Get<std::uint32_t>() : 0u;
            e.materialRef = version >= 2 ? r.Get<std::uint32_t>() : 0u;
            e.wetness = version >= 3 ? r.Get<std::uint8_t>() : std::uint8_t{0};
            if (version >= 4)
            {
                e.mixKnown = r.Get<std::uint8_t>();
                e.mixVolume = r.Get<float>();
                e.mixHighPass = r.Get<std::uint16_t>();
                e.mixLowPass = r.Get<std::uint16_t>();
            }
            if (version >= 5)
            {
                e.driveKnown = r.Get<std::uint8_t>();
                e.driveBase = r.Get<float>();
                e.driveImpact = r.Get<float>();
            }
            if (version >= 6)
            {
                e.layersKnown = r.Get<std::uint8_t>();
                for (std::uint16_t& layer : e.layer)
                    layer = r.Get<std::uint16_t>();
                e.audioLod = r.Get<std::uint8_t>();
            }
        }
        void WriteFootsteps(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.footsteps.Events(), WriteFootstep); }
        void ReadFootsteps(Reader& r, LoadContext& c)
        {
            ReadEvents(r, c.recording.footsteps, 24,
                [&](Reader& reader, entity::FootstepEvent& step) { ReadFootstep(reader, step, c.sectionVersion); });
        }

        // Section "explosions" (EXPL).
        constexpr std::uint16_t kExplosionsVersion = 1;
        void WriteExplosion(Writer& w, const entity::ExplosionAudioEvent& e)
        {
            w.Put(e.timeMs);
            w.Bool(e.hasInstance);
            w.Put(e.tag);
            w.Put(e.overrideRef);
            w.Array(e.position);
            w.Put(e.underwater);
            w.Put(e.b31);
            w.Put(e.b32);
            w.Put(e.interior);
            w.Put(e.caller);
        }
        void ReadExplosion(Reader& r, entity::ExplosionAudioEvent& e)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.hasInstance = r.Bool();
            e.tag = r.Get<std::int32_t>();
            e.overrideRef = r.Get<std::uint32_t>();
            r.Array(e.position);
            e.underwater = r.Get<std::uint8_t>();
            e.b31 = r.Get<std::uint8_t>();
            e.b32 = r.Get<std::uint8_t>();
            e.interior = r.Get<std::uint32_t>();
            e.caller = r.Get<std::uint32_t>();
        }
        void WriteExplosions(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.explosions.Events(), WriteExplosion); }
        void ReadExplosions(Reader& r, LoadContext& c) { ReadEvents(r, c.recording.explosions, 36, ReadExplosion); }

        // Section "surfaces" (SURF).
        constexpr std::uint16_t kSurfacesVersion = 1;
        void WriteSurface(Writer& w, const entity::SurfaceEvent& e)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Array(e.wet);
            w.Put(e.wetFlags);
            w.Array(e.dirt);
        }
        void ReadSurface(Reader& r, entity::SurfaceEvent& e)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            r.Array(e.wet);
            e.wetFlags = r.Get<std::uint8_t>();
            r.Array(e.dirt);
        }
        void WriteSurfaces(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.surfaces.Events(), WriteSurface); }
        void ReadSurfaces(Reader& r, LoadContext& c) { ReadEvents(r, c.recording.surfaces, 37, ReadSurface); }

        // Section "fx" (FXEV): one-shot particle effects.
        constexpr std::uint16_t kFxVersion = 1;
        void WriteFx(Writer& w, const entity::FxEvent& e)
        {
            w.Put(e.timeMs);
            w.Put(e.effectHash);
            w.Put(e.assetHash);
            w.Put(e.callerSite);
            w.Id(e.entity);
            w.Put(e.bone);
            w.Put(e.zoom);
            w.Put(e.flags);
            w.Array(e.matrix);
            w.Array(e.evolutions);
        }
        void ReadFx(Reader& r, entity::FxEvent& e)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.effectHash = r.Get<std::uint32_t>();
            e.assetHash = r.Get<std::uint32_t>();
            e.callerSite = r.Get<std::uint32_t>();
            e.entity = r.Id();
            e.bone = r.Get<std::int32_t>();
            e.zoom = r.Get<float>();
            e.flags = r.Get<std::uint32_t>();
            r.Array(e.matrix);
            r.Array(e.evolutions);
        }
        void WriteFxTrack(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.fx.Events(), WriteFx); }
        void ReadFxTrack(Reader& r, LoadContext& c) { ReadEvents(r, c.recording.fx, 112, ReadFx); }

        // Section "registeredFx" (RGFX): continuous particle effects.
        constexpr std::uint16_t kRegisteredFxVersion = 1;
        void WriteRegisteredFx(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.registeredFx.size());
            for (const entity::RegisteredFxTrack& track : c.recording.registeredFx)
            {
                w.Put(track.effectHash);
                w.Put(track.flag);
                w.Put(track.arg8);
                w.Put(track.arg9);
                w.Put(track.callerSite);
                w.Bool(track.attached);
                w.Id(track.attachId);
                w.Put(track.attachBone);
                w.Put(track.attachModel);
                w.Array(track.attachPos);
                w.Put(track.startMs);
                w.Put(track.endMs);
                w.Count(track.frames.size());
                for (const entity::RegisteredFxFrame& frame : track.frames)
                {
                    w.Put(frame.timeMs);
                    w.Array(frame.matrix);
                    w.Array(frame.parent);
                    w.Array(frame.evolutions);
                    w.Put(frame.zoom);
                    w.Array(frame.aux350);
                    w.Put(frame.flags);
                    w.Put(frame.planeCount);
                    w.Array(frame.planes);
                    w.Array(frame.start310);
                    w.Put(frame.farClip);
                }
            }
        }
        void ReadRegisteredFx(Reader& r, LoadContext& c)
        {
            c.recording.registeredFx.resize(r.Count(48));
            for (entity::RegisteredFxTrack& track : c.recording.registeredFx)
            {
                track.effectHash = r.Get<std::uint32_t>();
                track.flag = r.Get<std::uint8_t>();
                track.arg8 = r.Get<std::uint8_t>();
                track.arg9 = r.Get<std::uint8_t>();
                track.callerSite = r.Get<std::uint32_t>();
                track.attached = r.Bool();
                track.attachId = r.Id();
                track.attachBone = r.Get<std::int32_t>();
                track.attachModel = r.Get<std::uint32_t>();
                r.Array(track.attachPos);
                track.startMs = r.Get<std::uint32_t>();
                track.endMs = r.Get<std::uint32_t>();
                track.frames.resize(r.Count(964));
                std::uint32_t lastMs = 0;
                for (entity::RegisteredFxFrame& frame : track.frames)
                {
                    frame.timeMs = r.Get<std::uint32_t>();
                    r.Array(frame.matrix);
                    r.Array(frame.parent);
                    r.Array(frame.evolutions);
                    frame.zoom = r.Get<float>();
                    r.Array(frame.aux350);
                    frame.flags = r.Get<std::uint32_t>();
                    frame.planeCount = r.Get<std::uint32_t>();
                    r.Array(frame.planes);
                    r.Array(frame.start310);
                    frame.farClip = r.Get<float>();
                    if (frame.planeCount > kRegisteredFxMaxPlanes || frame.timeMs < lastMs)
                        r.Fail();
                    lastMs = frame.timeMs;
                }
            }
        }

        // Section "decals" (DECL). settings +0x88 (the info pointer) is
        // already zero in the recording and forced to zero here.
        // Version 2 adds hasEntity after the settings; a version 1 payload
        // still loads (hasEntity false).
        constexpr std::uint16_t kDecalsVersion = 2;
        constexpr std::uint16_t kDecalsMinVersion = 1;
        void WriteDecal(Writer& w, const entity::DecalEvent& e)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Bool(e.attached);
            w.Put(e.attachModel);
            w.Array(e.attachPos);
            w.Put(e.callerReturn);
            w.Put(e.engineId);
            w.Put(e.requestId);
            w.Put(e.flag);
            w.Bool(e.infoKnown);
            w.Put(e.infoHash);
            w.Put(e.infoIndex);
            std::uint8_t settings[sizeof(e.settings)];
            std::memcpy(settings, e.settings, sizeof(settings));
            ZeroBytes(settings, sizeof(settings), kDecalSettingsInfo, sizeof(std::uint64_t));
            w.Raw(settings, sizeof(settings));
            w.Bool(e.hasEntity); // version 2
        }
        void ReadDecal(Reader& r, entity::DecalEvent& e, std::uint16_t version)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            e.attached = r.Bool();
            e.attachModel = r.Get<std::uint32_t>();
            r.Array(e.attachPos);
            e.callerReturn = r.Get<std::uint32_t>();
            e.engineId = r.Get<std::uint32_t>();
            e.requestId = r.Get<std::uint32_t>();
            e.flag = r.Get<std::uint8_t>();
            e.infoKnown = r.Bool();
            e.infoHash = r.Get<std::uint32_t>();
            e.infoIndex = r.Get<std::int32_t>();
            r.Raw(e.settings, sizeof(e.settings));
            e.hasEntity = version >= 2 ? r.Bool() : false;
        }
        void WriteDecals(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.decals.Events(), WriteDecal); }
        void ReadDecals(Reader& r, LoadContext& c)
        {
            const std::uint16_t version = c.sectionVersion;
            ReadEvents(r, c.recording.decals, 380, [version](Reader& reader, entity::DecalEvent& e) { ReadDecal(reader, e, version); });
        }

        // Section "decalRemoves" (DCRM).
        // Version 2 adds the expired byte; a version 1 payload loads
        // with expired 0.
        constexpr std::uint16_t kDecalRemovesVersion = 2;
        constexpr std::uint16_t kDecalRemovesMinVersion = 1;
        void WriteDecalRemove(Writer& w, const entity::DecalRemoveEvent& e)
        {
            w.Put(e.timeMs);
            w.Put(e.engineId);
            w.Put(e.expired); // version 2
        }
        void ReadDecalRemove(Reader& r, entity::DecalRemoveEvent& e, std::uint16_t version)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.engineId = r.Get<std::uint32_t>();
            e.expired = version >= 2 ? r.Get<std::uint8_t>() : std::uint8_t{0};
            if (e.expired > 1)
                r.Fail();
        }
        void WriteDecalRemoves(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.decalRemoves.Events(), WriteDecalRemove); }
        void ReadDecalRemoves(Reader& r, LoadContext& c)
        {
            const std::uint16_t version = c.sectionVersion;
            ReadEvents(r, c.recording.decalRemoves, 8,
                [version](Reader& reader, entity::DecalRemoveEvent& e) { ReadDecalRemove(reader, e, version); });
        }

        // Section "projDecals" (PDCL).
        constexpr std::uint16_t kProjDecalsVersion = 1;
        void WriteProjDecal(Writer& w, const entity::ProjDecalEvent& e)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Put(e.attachModel);
            w.Array(e.attachPos);
            w.Put(e.typeHash);
            w.Put(e.arg4);
            w.Array(e.position);
            w.Array(e.direction);
            w.Array(e.side);
            w.Put(e.size);
            w.Put(e.arg9);
            w.Put(e.arg10);
            w.Put(e.arg11);
            w.Put(e.arg12);
            w.Bool(e.infoKnown);
            w.Put(e.infoHash);
            w.Put(e.infoIndex);
        }
        void ReadProjDecal(Reader& r, entity::ProjDecalEvent& e)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            e.attachModel = r.Get<std::uint32_t>();
            r.Array(e.attachPos);
            e.typeHash = r.Get<std::uint32_t>();
            e.arg4 = r.Get<std::int32_t>();
            r.Array(e.position);
            r.Array(e.direction);
            r.Array(e.side);
            e.size = r.Get<float>();
            e.arg9 = r.Get<std::uint16_t>();
            e.arg10 = r.Get<std::uint8_t>();
            e.arg11 = r.Get<std::int32_t>();
            e.arg12 = r.Get<std::int32_t>();
            e.infoKnown = r.Bool();
            e.infoHash = r.Get<std::uint32_t>();
            e.infoIndex = r.Get<std::int32_t>();
        }
        void WriteProjDecals(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.projDecals.Events(), WriteProjDecal); }
        void ReadProjDecals(Reader& r, LoadContext& c) { ReadEvents(r, c.recording.projDecals, 100, ReadProjDecal); }

        // Section "gore" (GORE).
        constexpr std::uint16_t kGoreVersion = 1;
        void WriteGore(Writer& w, const entity::GoreEvent& e)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Put(e.limb);
        }
        void ReadGore(Reader& r, entity::GoreEvent& e)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            e.limb = r.Get<std::int32_t>();
        }
        void WriteGoreTrack(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.gore.Events(), WriteGore); }
        void ReadGoreTrack(Reader& r, LoadContext& c) { ReadEvents(r, c.recording.gore, 12, ReadGore); }

        // Section "goreHits" (GHIT). context +0x00 and +0x08 (ped and
        // inflictor) are already zero and forced to zero here.
        constexpr std::uint16_t kGoreHitsVersion = 1;
        void WriteGoreHit(Writer& w, const entity::GoreHitEvent& e)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Id(e.inflictor);
            std::uint8_t context[sizeof(e.context)];
            std::memcpy(context, e.context, sizeof(context));
            ZeroBytes(context, sizeof(context), kGoreContextPed, sizeof(std::uint64_t));
            ZeroBytes(context, sizeof(context), kGoreContextInflictor, sizeof(std::uint64_t));
            w.Raw(context, sizeof(context));
            w.Put(e.result);
            w.Put(e.caller);
        }
        void ReadGoreHit(Reader& r, entity::GoreHitEvent& e)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            e.inflictor = r.Id();
            r.Raw(e.context, sizeof(e.context));
            e.result = r.Get<std::uint8_t>();
            e.caller = r.Get<std::uint32_t>();
        }
        void WriteGoreHits(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.goreHits.Events(), WriteGoreHit); }
        void ReadGoreHits(Reader& r, LoadContext& c) { ReadEvents(r, c.recording.goreHits, 97, ReadGoreHit); }

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

        // Section "blood" (BLOD). record +0x78 (an entity pointer, kept as
        // ref78Entity) is already zero and forced to zero here.
        constexpr std::uint16_t kBloodVersion = 1;
        void WriteBloodEvent(Writer& w, const entity::BloodEvent& e)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Put(e.slot);
            w.Put(e.caller);
            w.Put(e.index);
            w.Put(e.age);
            w.Id(e.ref78Entity);
            std::uint8_t record[sizeof(e.record)];
            std::memcpy(record, e.record, sizeof(record));
            ZeroBytes(record, sizeof(record), kBloodRecordEntity, sizeof(std::uint64_t));
            w.Raw(record, sizeof(record));
        }
        void ReadBloodEvent(Reader& r, entity::BloodEvent& e)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            e.slot = r.Get<std::uint8_t>();
            e.caller = r.Get<std::uint32_t>();
            e.index = r.Get<std::int32_t>();
            e.age = r.Get<float>();
            e.ref78Entity = r.Id();
            r.Raw(e.record, sizeof(e.record));
        }
        void WriteBlood(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.blood.Events(), WriteBloodEvent); }
        void ReadBlood(Reader& r, LoadContext& c) { ReadEvents(r, c.recording.blood, 377, ReadBloodEvent); }

        // Section "fireStarts" (FIRS). desc +0x40, +0x48 (ids) and +0xC0
        // (culprit) are already zero and forced to zero here; +0xF0 points
        // into the game image and is written as an RVA.
        // Version 2: the world object model and position of an unrecorded
        // entity A/B after the descriptor; version 1 reads as none.
        // Version 3: the rebuilt flag (an
        // initial fire read from the engine's fire list); older reads false.
        constexpr std::uint16_t kFireStartsVersion = 3;
        void WriteFireStart(Writer& w, const entity::FireStartEvent& e, const Image& image)
        {
            w.Put(e.timeMs);
            w.Put(e.slot);
            w.Put(e.type);
            w.Put(e.group);
            w.Array(e.position);
            w.Put(e.rng);
            w.Id(e.entityA);
            w.Id(e.entityB);
            w.Put(e.caller);
            w.Bool(e.initial);
            w.Bool(e.initialRemnant);
            w.Put(e.initialAge);
            std::uint8_t desc[sizeof(e.desc)];
            std::memcpy(desc, e.desc, sizeof(desc));
            ZeroBytes(desc, sizeof(desc), kFireDescEntityA, sizeof(std::uint64_t));
            ZeroBytes(desc, sizeof(desc), kFireDescEntityB, sizeof(std::uint64_t));
            ZeroBytes(desc, sizeof(desc), kFireDescCulprit, sizeof(std::uint64_t));
            WriteU64At(desc, kFireDescImage, ToRva(ReadU64At(desc, kFireDescImage), image));
            w.Raw(desc, sizeof(desc));
            w.Put(e.entityAModel);
            w.Array(e.entityAPos);
            w.Put(e.entityBModel);
            w.Array(e.entityBPos);
            w.Bool(e.rebuilt);
        }
        void ReadFireStart(Reader& r, entity::FireStartEvent& e, const Image& image, std::uint16_t version)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.slot = r.Get<std::int32_t>();
            e.type = r.Get<std::uint32_t>();
            e.group = r.Get<std::uint32_t>();
            r.Array(e.position);
            e.rng = r.Get<std::uint64_t>();
            e.entityA = r.Id();
            e.entityB = r.Id();
            e.caller = r.Get<std::uint32_t>();
            e.initial = r.Bool();
            e.initialRemnant = r.Bool();
            e.initialAge = r.Get<float>();
            r.Raw(e.desc, sizeof(e.desc));
            WriteU64At(e.desc, kFireDescImage, FromRva(ReadU64At(e.desc, kFireDescImage), image));
            if (version >= 2)
            {
                e.entityAModel = r.Get<std::uint32_t>();
                r.Array(e.entityAPos);
                e.entityBModel = r.Get<std::uint32_t>();
                r.Array(e.entityBPos);
            }
            if (version >= 3)
                e.rebuilt = r.Bool();
        }
        void WriteFireStarts(Writer& w, const SaveContext& c)
        {
            WriteEvents(w, c.recording.fireStarts.Events(),
                [&](Writer& writer, const entity::FireStartEvent& e) { WriteFireStart(writer, e, c.image); });
        }
        void ReadFireStarts(Reader& r, LoadContext& c)
        {
            ReadEvents(r, c.recording.fireStarts, 340,
                [&](Reader& reader, entity::FireStartEvent& e) { ReadFireStart(reader, e, c.image, c.sectionVersion); });
        }

        // Section "fireStops" (FSTP).
        constexpr std::uint16_t kFireStopsVersion = 1;
        void WriteFireStop(Writer& w, const entity::FireStopEvent& e)
        {
            w.Put(e.timeMs);
            w.Put(e.start);
            w.Put(e.instant);
            w.Put(e.arg3);
        }
        void ReadFireStop(Reader& r, entity::FireStopEvent& e)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.start = r.Get<std::uint32_t>();
            e.instant = r.Get<std::uint8_t>();
            e.arg3 = r.Get<std::uint8_t>();
        }
        void WriteFireStops(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.fireStops.Events(), WriteFireStop); }
        void ReadFireStops(Reader& r, LoadContext& c) { ReadEvents(r, c.recording.fireStops, 10, ReadFireStop); }

        // Section "fireItems" (FITM).
        constexpr std::uint16_t kFireItemsVersion = 1;
        void WriteFireItem(Writer& w, const entity::FireItemSample& e)
        {
            w.Put(e.timeMs);
            w.Put(e.start);
            w.Put(e.outer);
            w.Put(e.burnt);
            w.Put(e.ember);
            w.Put(e.glow);
            w.Put(e.width);
        }
        void ReadFireItem(Reader& r, entity::FireItemSample& e)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.start = r.Get<std::uint32_t>();
            e.outer = r.Get<float>();
            e.burnt = r.Get<float>();
            e.ember = r.Get<float>();
            e.glow = r.Get<float>();
            e.width = r.Get<float>();
        }
        void WriteFireItems(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.fireItems.Events(), WriteFireItem); }
        void ReadFireItems(Reader& r, LoadContext& c) { ReadEvents(r, c.recording.fireItems, 28, ReadFireItem); }

        // Section "meta" (META): the game build (see SetBuildIdentity).
        constexpr std::uint16_t kMetaVersion = 1;
        std::uint32_t g_buildTimestamp = 0;
        std::uint64_t g_buildImageSize = 0;
        void WriteMeta(Writer& w, const SaveContext&)
        {
            w.Put(g_buildTimestamp);
            w.Put(g_buildImageSize);
        }
        void ReadMeta(Reader& r, LoadContext&)
        {
            const std::uint32_t timestamp = r.Get<std::uint32_t>();
            const std::uint64_t size = r.Get<std::uint64_t>();
            // Another game build: its image offsets would point wrong.
            if (g_buildTimestamp && timestamp && (timestamp != g_buildTimestamp || size != g_buildImageSize))
                r.Fail();
        }

        // Section "area" (AREA).
        constexpr std::uint16_t kAreaVersion = 1;
        void WriteArea(Writer& w, const SaveContext& c)
        {
            w.Bool(c.recording.hasArea);
            w.Array(c.recording.areaMin);
            w.Array(c.recording.areaMax);
        }
        void ReadArea(Reader& r, LoadContext& c)
        {
            c.recording.hasArea = r.Bool();
            r.Array(c.recording.areaMin);
            r.Array(c.recording.areaMax);
        }

        // Section "worldObjects" (WOBJ): how the moved or broken world
        // objects stood at the start.
        constexpr std::uint16_t kWorldObjectsVersion = 1;
        void WriteWorldObjects(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.worldObjects.size());
            for (const WorldObjectOrigin& origin : c.recording.worldObjects)
            {
                w.Id(origin.id);
                w.Put(origin.model);
                w.Array(origin.position);
                w.Array(origin.rotation);
            }
        }
        void ReadWorldObjects(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(4 + 4 + 24);
            for (std::size_t n = 0; n < count; ++n)
            {
                WorldObjectOrigin origin;
                origin.id = r.Id();
                origin.model = r.Get<std::uint32_t>();
                r.Array(origin.position);
                r.Array(origin.rotation);
                c.recording.worldObjects.push_back(origin);
            }
        }

        // Section "fragBreaks" (FRAG): fragment groups that came off.
        constexpr std::uint16_t kFragBreaksVersion = 1;
        void WriteFragBreaks(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.fragBreaks.size());
            for (const FragBreak& e : c.recording.fragBreaks)
            {
                w.Put(e.timeMs);
                w.Id(e.id);
                w.Put(e.group);
            }
        }
        void ReadFragBreaks(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(4 + 4 + 2);
            for (std::size_t n = 0; n < count; ++n)
            {
                FragBreak e;
                e.timeMs = r.Get<std::uint32_t>();
                e.id = r.Id();
                e.group = r.Get<std::uint16_t>();
                c.recording.fragBreaks.push_back(e);
            }
        }

        // Section "frag pieces" (FPCE): per
        // loose piece of a fragment object, where its groups came from and
        // the groups it carries (frag_piece.h). Optional: without it a piece
        // plays as the whole object.
        constexpr std::uint16_t kFragPiecesVersion = 1;
        void WriteFragPieces(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.fragPieces.size());
            for (const FragPiece& row : c.recording.fragPieces)
            {
                w.Id(row.id);
                w.Put(row.source);
                w.Values(row.groups);
            }
        }
        void ReadFragPieces(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(4 + 1 + 8 + 2);
            if (count > fragpiece::kMaxRows)
                r.Fail();
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                FragPiece row;
                row.id = r.Id();
                row.source = r.Get<std::uint8_t>();
                r.Values(row.groups);
                if (row.source > fragpiece::kSourceMax || !fragpiece::ValidGroups(row.groups))
                    r.Fail();
                c.recording.fragPieces.push_back(std::move(row));
            }
        }

        // Section "scenery" (SCNY, optional): the objects of the scene that
        // are not of the map, as state (object_check.h).
        // v2: the visibility when recorded and its flips (u8, then a count and
        // u32 times); v1 reads visible.
        // v3: the entry's rule (u8,
        // objcheck::SceneryRule) and a map dressing entry's IPL (u32); v2
        // and v1 read rule 0 (legacy) and no IPL.
        constexpr std::uint16_t kSceneryVersion = 3;
        void WriteScenery(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.scenery.size());
            for (const SceneryObject& object : c.recording.scenery)
            {
                w.Put(object.model);
                w.Array(object.position);
                w.Array(object.rotation);
                w.Put(object.appearMs);
                w.Put(object.goneMs);
                w.Id(object.prop);
                w.Put(object.owner);
                w.Put(object.mission);
                w.Put(object.visible);
                w.Values(object.visibleFlipsMs);
                w.Put(object.rule); // v3
                w.Put(object.ipl);
            }
        }
        void ReadScenery(Reader& r, LoadContext& c)
        {
            const bool v2 = c.sectionVersion >= 2;
            const bool v3 = c.sectionVersion >= 3;
            const std::size_t count = r.Count(4 + 24 + 4 + 4 + 4 + 1 + 1 + (v2 ? 1 + 8 : 0) + (v3 ? 1 + 4 : 0));
            for (std::size_t n = 0; n < count; ++n)
            {
                SceneryObject object;
                object.model = r.Get<std::uint32_t>();
                r.Array(object.position);
                r.Array(object.rotation);
                object.appearMs = r.Get<std::uint32_t>();
                object.goneMs = r.Get<std::uint32_t>();
                object.prop = r.Id();
                object.owner = r.Get<std::uint8_t>();
                object.mission = r.Get<std::uint8_t>();
                if (v2)
                {
                    object.visible = r.Get<std::uint8_t>() != 0 ? 1 : 0;
                    r.Values(object.visibleFlipsMs);
                }
                if (v3)
                {
                    object.rule = r.Get<std::uint8_t>();
                    object.ipl = r.Get<std::uint32_t>();
                    if (object.rule >= objcheck::kSceneryRuleCount)
                        r.Fail();
                }
                c.recording.scenery.push_back(object);
            }
        }

        // Section "scenery children" (SCCH, optional):
        // the objects attached to a scenery object (object_check.h), each
        // naming its parent by its index in SCNY. Written
        // only when the recording has one, so a clip without children stays
        // readable by a build that does not know the section (an unknown
        // section refuses the clip).
        constexpr std::uint16_t kSceneryChildrenVersion = 1;
        void WriteSceneryChildren(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.sceneryChildren.size());
            for (const SceneryChild& child : c.recording.sceneryChildren)
            {
                w.Put(child.model);
                w.Put(child.parent);
                w.Put(child.hasFrame);
                w.Put(child.frameBone);
                w.Array(child.framePosition);
                w.Array(child.frameRotation);
                w.Array(child.relativePosition);
                w.Array(child.relativeRotation);
                w.Array(child.position);
                w.Array(child.rotation);
                w.Put(child.appearMs);
                w.Put(child.goneMs);
                w.Put(child.visible);
                w.Values(child.visibleFlipsMs);
            }
        }
        void ReadSceneryChildren(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(4 + 4 + 1 + 2 + 12 + 16 + 12 + 16 + 12 + 12 + 4 + 4 + 1 + 8);
            if (count > objcheck::kMaxSceneryChildren)
                r.Fail();
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                SceneryChild child;
                child.model = r.Get<std::uint32_t>();
                child.parent = r.Get<std::uint32_t>();
                child.hasFrame = r.Get<std::uint8_t>() != 0 ? 1 : 0;
                child.frameBone = r.Get<std::int16_t>();
                r.Array(child.framePosition);
                r.Array(child.frameRotation);
                r.Array(child.relativePosition);
                r.Array(child.relativeRotation);
                r.Array(child.position);
                r.Array(child.rotation);
                child.appearMs = r.Get<std::uint32_t>();
                child.goneMs = r.Get<std::uint32_t>();
                child.visible = r.Get<std::uint8_t>() != 0 ? 1 : 0;
                r.Values(child.visibleFlipsMs);
                // The frame plausible when kept (a parent that is no scenery
                // entry, SCNY left out, is dropped by ValidateLoaded).
                if (child.visibleFlipsMs.size() > objcheck::kMaxVisibilityFlips ||
                    !std::is_sorted(child.visibleFlipsMs.begin(), child.visibleFlipsMs.end()) ||
                    (child.hasFrame && !objcheck::FramePlausible(child.frameBone, child.framePosition, child.frameRotation)))
                    r.Fail();
                c.recording.sceneryChildren.push_back(std::move(child));
            }
        }
        bool OmitSceneryChildren(const SaveContext& c) { return c.recording.sceneryChildren.empty(); }

        // Section "scenery hats" (SCHT, optional, hat_look.h): the metaped layer of each scenery hat (a hat that fell
        // before the recording started), naming its entry by its index in
        // SCNY: the index, then the layers (the eight values of
        // _SET_META_PED_TAG each). Written only when a scenery entry has one;
        // an index past the scenery (SCNY left out or refused) is dropped.
        constexpr std::uint16_t kSceneryHatsVersion = 1;
        void WriteSceneryHats(Writer& w, const SaveContext& c)
        {
            std::size_t count = 0;
            for (const SceneryObject& object : c.recording.scenery)
                count += object.hatLook.empty() ? 0u : 1u;
            w.Count(count);
            for (std::size_t index = 0; index < c.recording.scenery.size(); ++index)
            {
                const std::vector<entity::MetaPedAsset>& look = c.recording.scenery[index].hatLook;
                if (look.empty())
                    continue;
                w.Put(static_cast<std::uint32_t>(index));
                w.Count(look.size());
                for (const entity::MetaPedAsset& asset : look)
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
            }
        }
        void ReadSceneryHats(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(4 + 8 + 32);
            if (count > objcheck::kMaxScenery)
                r.Fail();
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                const std::uint32_t index = r.Get<std::uint32_t>();
                std::vector<entity::MetaPedAsset> look(r.Count(32));
                if (look.empty() || look.size() > hatlook::kMaxTags)
                    r.Fail();
                for (entity::MetaPedAsset& asset : look)
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
                if (r.Ok() && index < c.recording.scenery.size())
                    c.recording.scenery[index].hatLook = std::move(look);
            }
        }
        bool OmitSceneryHats(const SaveContext& c)
        {
            return std::none_of(c.recording.scenery.begin(), c.recording.scenery.end(),
                [](const SceneryObject& object) { return !object.hatLook.empty(); });
        }
        void ResetSceneryHats(LoadContext& c)
        {
            for (SceneryObject& object : c.recording.scenery)
                object.hatLook.clear();
        }

        // Section "ped visibility" (PVIS):
        // per recorded ped and horse, visible at its first sample and the
        // times it flipped since. Optional: without it every puppet shows.
        constexpr std::uint16_t kPedVisibilityVersion = 1;
        void WritePedVisibility(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.pedVisibility.size());
            for (const PedVisibility& row : c.recording.pedVisibility)
            {
                w.Id(row.id);
                w.Put(row.visible);
                w.Values(row.flipsMs);
            }
        }
        void ReadPedVisibility(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(4 + 1 + 8);
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                PedVisibility row;
                row.id = r.Id();
                row.visible = r.Get<std::uint8_t>() != 0 ? 1 : 0;
                r.Values(row.flipsMs);
                if (row.flipsMs.size() > objcheck::kMaxVisibilityFlips ||
                    !std::is_sorted(row.flipsMs.begin(), row.flipsMs.end()))
                    r.Fail();
                c.recording.pedVisibility.push_back(std::move(row));
            }
        }

        // Section "gameCamera" (GCAM): the rendered camera per captured
        // frame.
        constexpr std::uint16_t kGameCameraVersion = 1;
        void WriteGameCamera(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.gameCamera.size());
            for (const camera::GameSample& e : c.recording.gameCamera)
            {
                w.Put(e.timeMs);
                w.Put(e.position.x);
                w.Put(e.position.y);
                w.Put(e.position.z);
                w.Put(e.rotation.x);
                w.Put(e.rotation.y);
                w.Put(e.rotation.z);
                w.Put(e.fov);
            }
        }
        void ReadGameCamera(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(4 + 7 * 4);
            std::uint32_t lastMs = 0;
            for (std::size_t n = 0; n < count; ++n)
            {
                camera::GameSample e{};
                e.timeMs = r.Get<std::uint32_t>();
                e.position.x = r.Get<float>();
                e.position.y = r.Get<float>();
                e.position.z = r.Get<float>();
                e.rotation.x = r.Get<float>();
                e.rotation.y = r.Get<float>();
                e.rotation.z = r.Get<float>();
                e.fov = r.Get<float>();
                // EvaluateGame expects the samples sorted by time.
                if (e.timeMs < lastMs)
                    r.Fail();
                lastMs = e.timeMs;
                c.recording.gameCamera.push_back(e);
            }
        }

        // Section "environment" (ENVR): clock and weather.
        constexpr std::uint16_t kEnvironmentVersion = 1;
        void WriteEnvironment(Writer& w, const SaveContext& c)
        {
            const Environment& e = c.recording.environment;
            w.Bool(e.recorded);
            w.Put(e.clockStartS);
            w.Put(e.clockEndS);
            w.Put(e.weatherFrom);
            w.Put(e.weatherTo);
            w.Put(e.weatherMix);
        }
        void ReadEnvironment(Reader& r, LoadContext& c)
        {
            Environment& e = c.recording.environment;
            e.recorded = r.Bool();
            e.clockStartS = r.Get<std::uint32_t>();
            e.clockEndS = r.Get<std::uint32_t>();
            e.weatherFrom = r.Get<std::uint32_t>();
            e.weatherTo = r.Get<std::uint32_t>();
            e.weatherMix = r.Get<float>();
            if (e.clockStartS >= 86400u || e.clockEndS >= 86400u)
                r.Fail();
        }

        // Section "environment track" (ENVT, optional): the
        // weather, clock, wind, rain and snow over time
        // (environment_track.h). A clip without it plays the start state.
        // v2 adds the weather manager's transition over time to each sample
        // (target hash, total, time left, flags: bit 0 known, bit 1 the
        // manager's flag byte). A v1 section still loads, its transition
        // unknown (the replay leaves the engine's alone).
        constexpr std::uint16_t kEnvironmentTrackVersion = 2;
        constexpr std::uint16_t kEnvironmentTrackMinVersion = 1;
        constexpr std::uint8_t kTransitionKnownBit = 1;
        constexpr std::uint8_t kTransitionFlagBit = 2;
        void WriteEnvironmentTrack(Writer& w, const SaveContext& c)
        {
            const std::vector<EnvironmentSample>& track = c.recording.environment.track;
            w.Count(track.size());
            for (const EnvironmentSample& s : track)
            {
                w.Put(s.timeMs);
                w.Put(s.weatherFrom);
                w.Put(s.weatherTo);
                w.Put(s.weatherMix);
                w.Put(s.clockS);
                w.Put(s.windSpeed);
                w.Put(s.windDirection);
                w.Put(s.rain);
                w.Put(s.snow);
                w.Put(s.transitionTo); // v2
                w.Put(s.transitionTotalMs);
                w.Put(s.transitionLeftMs);
                const std::uint8_t flags = static_cast<std::uint8_t>((s.transitionKnown ? kTransitionKnownBit : 0) |
                    (s.transitionFlag ? kTransitionFlagBit : 0));
                w.Put(flags);
            }
        }
        void ReadEnvironmentTrack(Reader& r, LoadContext& c)
        {
            const bool v2 = c.sectionVersion >= 2;
            const std::size_t count = r.Count(9 * 4 + (v2 ? 3 * 4 + 1 : 0));
            std::vector<EnvironmentSample>& track = c.recording.environment.track;
            std::uint32_t lastMs = 0;
            for (std::size_t n = 0; n < count; ++n)
            {
                EnvironmentSample s;
                s.timeMs = r.Get<std::uint32_t>();
                s.weatherFrom = r.Get<std::uint32_t>();
                s.weatherTo = r.Get<std::uint32_t>();
                s.weatherMix = r.Get<float>();
                s.clockS = r.Get<std::uint32_t>();
                s.windSpeed = r.Get<float>();
                s.windDirection = r.Get<float>();
                s.rain = r.Get<float>();
                s.snow = r.Get<float>();
                if (v2)
                {
                    s.transitionTo = r.Get<std::uint32_t>();
                    s.transitionTotalMs = r.Get<float>();
                    s.transitionLeftMs = r.Get<float>();
                    const std::uint8_t flags = r.Get<std::uint8_t>();
                    if (flags & ~(kTransitionKnownBit | kTransitionFlagBit))
                        r.Fail();
                    s.transitionKnown = (flags & kTransitionKnownBit) != 0;
                    s.transitionFlag = (flags & kTransitionFlagBit) ? 1 : 0;
                }
                if (!envtrack::Valid(s) || s.timeMs < lastMs)
                    r.Fail();
                lastMs = s.timeMs;
                track.push_back(s);
            }
        }

        // Section "sky track" (SKYT, optional): the cloud accumulators over
        // time and their periods (sky_track.h). A clip without it leaves the
        // clouds to the session.
        constexpr std::uint16_t kSkyTrackVersion = 1;
        void WriteSkyTrack(Writer& w, const SaveContext& c)
        {
            const Environment& e = c.recording.environment;
            for (float period : e.skyPeriods.value)
                w.Put(period);
            w.Count(e.sky.size());
            for (const SkySample& s : e.sky)
            {
                w.Put(s.timeMs);
                for (float v : s.value)
                    w.Put(v);
                for (float v : s.unpinned)
                    w.Put(v);
            }
        }
        void ReadSkyTrack(Reader& r, LoadContext& c)
        {
            Environment& e = c.recording.environment;
            for (float& period : e.skyPeriods.value)
                period = r.Get<float>();
            if (!skytrack::Valid(e.skyPeriods))
                r.Fail();
            const std::size_t count = r.Count(4 + (sky::kChannels + 3) * 4);
            std::uint32_t lastMs = 0;
            for (std::size_t n = 0; n < count; ++n)
            {
                SkySample s;
                s.timeMs = r.Get<std::uint32_t>();
                for (float& v : s.value)
                    v = r.Get<float>();
                for (float& v : s.unpinned)
                    v = r.Get<float>();
                if (!skytrack::Finite(s) || s.timeMs < lastMs)
                    r.Fail();
                lastMs = s.timeMs;
                e.sky.push_back(s);
            }
        }

        // Section "timecycle" (TCMD, timecycle_track.h):
        // the script timecycle modifier (key, index, strength, transition key
        // and index, progress, speed, fade) and the weather types with a
        // variation, over time. Optional: a clip without it leaves the
        // session's modifier and variations alone.
        constexpr std::uint16_t kTimecycleVersion = 1;
        constexpr std::size_t kTimecycleSampleMinBytes = 4 * 8 + 2;
        void WriteTimecycle(Writer& w, const SaveContext& c)
        {
            const std::vector<TimecycleSample>& track = c.recording.environment.timecycle;
            w.Count(track.size());
            for (const TimecycleSample& s : track)
            {
                w.Put(s.timeMs);
                w.Put(s.modifierKey);
                w.Put(s.modifierIndex);
                w.Put(s.strength);
                w.Put(s.transitionKey);
                w.Put(s.transitionIndex);
                w.Put(s.progress);
                w.Put(s.speed);
                w.Put(s.fadeOut);
                w.Put(s.variationCount);
                for (std::size_t n = 0; n < s.variationCount; ++n)
                {
                    w.Put(s.variations[n].type);
                    w.Put(s.variations[n].variation);
                }
            }
        }
        void ReadTimecycle(Reader& r, LoadContext& c)
        {
            std::vector<TimecycleSample>& track = c.recording.environment.timecycle;
            const std::size_t count = r.Count(kTimecycleSampleMinBytes);
            std::uint32_t lastMs = 0;
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                TimecycleSample s;
                s.timeMs = r.Get<std::uint32_t>();
                s.modifierKey = r.Get<std::uint32_t>();
                s.modifierIndex = r.Get<std::int32_t>();
                s.strength = r.Get<float>();
                s.transitionKey = r.Get<std::uint32_t>();
                s.transitionIndex = r.Get<std::int32_t>();
                s.progress = r.Get<float>();
                s.speed = r.Get<float>();
                s.fadeOut = r.Get<std::uint8_t>() != 0 ? 1 : 0;
                s.variationCount = r.Get<std::uint8_t>();
                if (s.variationCount > tctrack::kMaxVariations)
                {
                    r.Fail();
                    return;
                }
                for (std::size_t v = 0; v < s.variationCount; ++v)
                {
                    s.variations[v].type = r.Get<std::uint32_t>();
                    s.variations[v].variation = r.Get<std::uint32_t>();
                }
                if (!tctrack::Valid(s) || s.timeMs < lastMs)
                    r.Fail();
                lastMs = s.timeMs;
                track.push_back(s);
            }
        }

        // Section "moon" (MOON, moon_track.h):
        // the sky object's two moon angles (+0xA80 turn, +0x8E8 orbit) over
        // time, with the drawn sun and moon and the clock date as evidence.
        // Optional: a clip without it plays with the session's moon (which
        // the replay gives back when the clip ends).
        constexpr std::uint16_t kMoonVersion = 1;
        constexpr std::size_t kMoonSampleBytes = 4 + 2 * 8 + 6 * 4 + 2 + 1 + 1;
        void WriteMoon(Writer& w, const SaveContext& c)
        {
            const std::vector<MoonSample>& track = c.recording.environment.moon;
            w.Count(track.size());
            for (const MoonSample& s : track)
            {
                w.Put(s.timeMs);
                w.Put(s.angle);
                w.Put(s.orbit);
                for (float v : s.sun)
                    w.Put(v);
                for (float v : s.moon)
                    w.Put(v);
                w.Put(s.year);
                w.Put(s.month);
                w.Put(s.day);
            }
        }
        void ReadMoon(Reader& r, LoadContext& c)
        {
            std::vector<MoonSample>& track = c.recording.environment.moon;
            const std::size_t count = r.Count(kMoonSampleBytes);
            std::uint32_t lastMs = 0;
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                MoonSample s;
                s.timeMs = r.Get<std::uint32_t>();
                s.angle = r.Get<double>();
                s.orbit = r.Get<double>();
                for (float& v : s.sun)
                    v = r.Get<float>();
                for (float& v : s.moon)
                    v = r.Get<float>();
                s.year = r.Get<std::uint16_t>();
                s.month = r.Get<std::uint8_t>();
                s.day = r.Get<std::uint8_t>();
                if (!moontrack::Valid(s) || s.timeMs < lastMs)
                {
                    r.Fail();
                    return;
                }
                lastMs = s.timeMs;
                track.push_back(s);
            }
        }

        // Section "environment edit" (ENVO, optional): the editor's
        // environment settings of the clip (env_override.h). A clip without it
        // plays everything as recorded.
        // v2: 12 bytes appended for the experimental sun and moon rows (24
        // bytes); 1.0 cut the rows, so they are written as Recorded and
        // skipped when read (env_override.h). v1 (12 bytes) still read.
        constexpr std::uint16_t kEnvironmentEditVersion = 2;
        constexpr std::uint16_t kEnvironmentEditMinVersion = 1;
        void WriteEnvironmentEdit(Writer& w, const SaveContext& c)
        {
            std::uint8_t bytes[envoverride::kBytes];
            envoverride::Pack(c.recording.environment.edit, bytes);
            for (std::uint8_t b : bytes)
                w.Put(b);
        }
        void ReadEnvironmentEdit(Reader& r, LoadContext& c)
        {
            const std::size_t size = c.sectionVersion >= 2 ? envoverride::kBytes : envoverride::kBytesV1;
            std::uint8_t bytes[envoverride::kBytes]{};
            for (std::size_t n = 0; n < size; ++n)
                bytes[n] = r.Get<std::uint8_t>();
            if (!envoverride::Unpack(bytes, size, c.recording.environment.edit))
                r.Fail();
        }

        // Section "place" (PLAC, optional): zones, character,
        // weather name.
        constexpr std::uint16_t kPlaceVersion = 1;
        void WritePlace(Writer& w, const SaveContext& c)
        {
            const Place& p = c.recording.place;
            w.Bool(p.recorded);
            w.Put(p.zoneTown);
            w.Put(p.zoneDistrict);
            w.Put(p.zoneState);
            w.Put(p.playerModel);
            w.Array(p.town);
            w.Array(p.district);
            w.Array(p.state);
            w.Array(p.character);
            w.Array(p.weather);
        }
        void ReadPlace(Reader& r, LoadContext& c)
        {
            Place& p = c.recording.place;
            p.recorded = r.Bool();
            p.zoneTown = r.Get<std::uint32_t>();
            p.zoneDistrict = r.Get<std::uint32_t>();
            p.zoneState = r.Get<std::uint32_t>();
            p.playerModel = r.Get<std::uint32_t>();
            r.Array(p.town);
            r.Array(p.district);
            r.Array(p.state);
            r.Array(p.character);
            r.Array(p.weather);
            // Text fields always end in a terminator.
            p.town[sizeof(p.town) - 1] = p.district[sizeof(p.district) - 1] = p.state[sizeof(p.state) - 1] = '\0';
            p.character[sizeof(p.character) - 1] = p.weather[sizeof(p.weather) - 1] = '\0';
        }

        // Section "world info" (WINF, optional): the chapter, the
        // camp and a fingerprint of the map state at the clip start, small
        // and fixed (20 bytes) so the clip list reads it from the summary.
        constexpr std::uint16_t kWorldInfoVersion = 1;
        void WriteWorldInfo(Writer& w, const SaveContext& c)
        {
            const WorldState& world = c.recording.world;
            w.Bool(world.recorded);
            w.Put(world.camp);
            w.Put(world.chapter);
            w.Put(world.flags);
            w.Put(world::Fingerprint(world));
            w.Put(static_cast<std::uint32_t>(world::IplsOnAt(world, 0).size()));
            w.Put(world.candidateListHash);
            w.Put(world.candidateCount);
        }
        void ReadWorldInfo(Reader& r, LoadContext& c)
        {
            WorldState& world = c.recording.world;
            world.recorded = r.Bool();
            world.camp = r.Get<std::uint8_t>();
            world.chapter = r.Get<std::uint8_t>();
            world.flags = r.Get<std::uint8_t>();
            r.Get<std::uint32_t>(); // fingerprint: derived from the WRLD events
            r.Get<std::uint32_t>(); // active count: likewise
            world.candidateListHash = r.Get<std::uint32_t>();
            world.candidateCount = r.Get<std::uint32_t>();
            if ((world.camp > 8 && world.camp != world::kCampUnknown) || world.chapter > world::kChapterEpilogue2)
                r.Fail();
        }

        // Section "world" (WRLD, optional): the IPL changes (the
        // active candidates at 0, then each change), their bounding spheres,
        // the interiors found near the scene and their entity set changes.
        constexpr std::uint16_t kWorldVersion = 1;
        void WriteWorld(Writer& w, const SaveContext& c)
        {
            const WorldState& world = c.recording.world;
            w.Count(world.ipls.size());
            for (const IplChange& e : world.ipls)
            {
                w.Put(e.timeMs);
                w.Put(e.hash);
                w.Bool(e.active);
            }
            w.Count(world.spheres.size());
            for (const IplSphere& e : world.spheres)
            {
                w.Put(e.hash);
                w.Array(e.center);
                w.Put(e.radius);
            }
            w.Count(world.interiors.size());
            for (const WorldInterior& e : world.interiors)
            {
                w.Put(e.nameHash);
                w.Array(e.position);
            }
            w.Count(world.entitySets.size());
            for (const EntitySetChange& e : world.entitySets)
            {
                w.Put(e.timeMs);
                w.Put(e.interior);
                w.Bool(e.active);
                const std::size_t length = strnlen(e.name, sizeof(e.name) - 1);
                w.Put(static_cast<std::uint8_t>(length));
                w.Raw(reinterpret_cast<const std::uint8_t*>(e.name), length);
            }
        }
        void ReadWorld(Reader& r, LoadContext& c)
        {
            WorldState& world = c.recording.world;
            world.ipls.resize(r.Count(9));
            std::uint32_t lastMs = 0;
            for (IplChange& e : world.ipls)
            {
                e.timeMs = r.Get<std::uint32_t>();
                e.hash = r.Get<std::uint32_t>();
                e.active = r.Bool();
                if (e.timeMs < lastMs)
                    r.Fail();
                lastMs = e.timeMs;
            }
            world.spheres.resize(r.Count(20));
            std::uint32_t lastHash = 0;
            for (std::size_t n = 0; n < world.spheres.size(); ++n)
            {
                IplSphere& e = world.spheres[n];
                e.hash = r.Get<std::uint32_t>();
                r.Array(e.center);
                e.radius = r.Get<float>();
                if (n > 0 && e.hash <= lastHash)
                    r.Fail(); // FindSphere searches by hash
                lastHash = e.hash;
            }
            world.interiors.resize(r.Count(16));
            for (WorldInterior& e : world.interiors)
            {
                e.nameHash = r.Get<std::uint32_t>();
                r.Array(e.position);
            }
            world.entitySets.resize(r.Count(8));
            lastMs = 0;
            for (EntitySetChange& e : world.entitySets)
            {
                e.timeMs = r.Get<std::uint32_t>();
                e.interior = r.Get<std::uint16_t>();
                e.active = r.Bool();
                const std::uint8_t length = r.Get<std::uint8_t>();
                if (length >= sizeof(e.name) || e.interior >= world.interiors.size() || e.timeMs < lastMs)
                {
                    r.Fail();
                    return;
                }
                r.Raw(reinterpret_cast<std::uint8_t*>(e.name), length);
                e.name[length] = '\0';
                lastMs = e.timeMs;
            }
        }

        // Section "abilities" (ABIL, optional): Dead Eye and
        // Eagle Eye spans.
        constexpr std::uint16_t kAbilitiesVersion = 1;
        void WriteAbilities(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.abilities.size());
            for (const AbilitySpan& span : c.recording.abilities)
            {
                w.Put(span.startMs);
                w.Put(span.endMs);
                w.Enum(span.kind);
            }
        }
        void ReadAbilities(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(9);
            for (std::size_t n = 0; n < count; ++n)
            {
                AbilitySpan span;
                span.startMs = r.Get<std::uint32_t>();
                span.endMs = r.Get<std::uint32_t>();
                span.kind = r.Enum<AbilityKind>(static_cast<std::uint32_t>(AbilityKind::EagleEye));
                if (span.endMs < span.startMs)
                    r.Fail();
                c.recording.abilities.push_back(span);
            }
        }

        // Section "weapon conditions" (WCND, optional).
        // Version 2 (weapon_look_core.h) is the whole look per state: also the
        // rust (permanent degradation), the sixth wear channel, the flags and
        // the engine's component list. Version 1 (dirt, soot, damage,
        // degradation) still reads, with flags 0 (the create's components
        // apply).
        constexpr std::uint16_t kWeaponConditionsVersion = 2;
        constexpr std::uint16_t kWeaponConditionsMinVersion = 1;
        void WriteWeaponConditions(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.weaponConditions.size());
            for (const WeaponConditionTrack& track : c.recording.weaponConditions)
            {
                w.Put(track.recordId);
                w.Count(track.states.size());
                for (const auto& [timeMs, state] : track.states)
                {
                    w.Put(timeMs);
                    w.Put(state.dirt);
                    w.Put(state.soot);
                    w.Put(state.damage);
                    w.Put(state.degradation);
                    w.Put(state.permanent);
                    w.Put(state.wearE4);
                    w.Put(state.flags);
                    w.Values(state.components);
                }
            }
        }
        void ReadWeaponConditions(Reader& r, LoadContext& c)
        {
            const bool look = c.sectionVersion >= 2;
            c.recording.weaponConditions.resize(r.Count(8));
            for (WeaponConditionTrack& track : c.recording.weaponConditions)
            {
                track.recordId = r.Get<std::uint32_t>();
                track.states.resize(r.Count(look ? 33 : 20));
                std::uint32_t lastMs = 0;
                for (auto& [timeMs, state] : track.states)
                {
                    timeMs = r.Get<std::uint32_t>();
                    state.dirt = r.Get<float>();
                    state.soot = r.Get<float>();
                    state.damage = r.Get<float>();
                    state.degradation = r.Get<float>();
                    if (look)
                    {
                        state.permanent = r.Get<float>();
                        state.wearE4 = r.Get<float>();
                        state.flags = r.Get<std::uint8_t>();
                        r.Values(state.components);
                        if (state.components.size() > weaponlook::kComponentsMax)
                            r.Fail();
                    }
                    if (timeMs < lastMs)
                        r.Fail();
                    lastMs = timeMs;
                }
            }
        }

        // Section "cuts" (CUTS, optional): the instants the recording clock
        // went on after a cutscene, fade or loading screen. A clip without it
        // loads with none.
        constexpr std::uint16_t kCutsVersion = 1;
        void WriteCuts(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.cuts.size());
            for (const std::uint32_t cut : c.recording.cuts)
                w.Put(cut);
        }
        void ReadCuts(Reader& r, LoadContext& c)
        {
            c.recording.cuts.resize(r.Count(4));
            std::uint32_t last = 0;
            for (std::size_t n = 0; n < c.recording.cuts.size(); ++n)
            {
                const std::uint32_t cut = r.Get<std::uint32_t>();
                if (n > 0 && cut <= last)
                    r.Fail();
                c.recording.cuts[n] = last = cut;
            }
        }

        // Section "spans" (SPAN, optional): the restricted-camera
        // spans (cutscene, script camera, no control).
        constexpr std::uint16_t kSpansVersion = 1;
        void WriteSpans(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.spans.size());
            for (const gate::Span& span : c.recording.spans)
            {
                w.Put(span.startMs);
                w.Put(span.endMs);
                w.Enum(span.kind);
            }
        }
        void ReadSpans(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(9);
            for (std::size_t n = 0; n < count; ++n)
            {
                gate::Span span;
                span.startMs = r.Get<std::uint32_t>();
                span.endMs = r.Get<std::uint32_t>();
                span.kind = r.Enum<gate::SpanKind>(static_cast<std::uint32_t>(gate::SpanKind::ControlsOff));
                if (span.endMs < span.startMs)
                    r.Fail();
                c.recording.spans.push_back(span);
            }
        }

        // Section "scorched" (SCOR, optional): the charred state
        // changes of each recorded ped and horse.
        constexpr std::uint16_t kScorchedVersion = 1;
        void WriteScorched(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.scorched.size());
            for (const ScorchTrack& track : c.recording.scorched)
            {
                w.Put(track.id);
                w.Count(track.states.size());
                for (const auto& [timeMs, on] : track.states)
                {
                    w.Put(timeMs);
                    w.Put(static_cast<std::uint8_t>(on ? 1 : 0));
                }
            }
        }
        void ReadScorched(Reader& r, LoadContext& c)
        {
            c.recording.scorched.resize(r.Count(8));
            for (ScorchTrack& track : c.recording.scorched)
            {
                track.id = r.Get<std::uint32_t>();
                track.states.resize(r.Count(5));
                std::uint32_t lastMs = 0;
                for (auto& [timeMs, on] : track.states)
                {
                    timeMs = r.Get<std::uint32_t>();
                    const std::uint8_t value = r.Get<std::uint8_t>();
                    if (value > 1 || timeMs < lastMs)
                        r.Fail();
                    on = value != 0;
                    lastMs = timeMs;
                }
            }
        }

        // Section "vehicle windows" (VWIN, optional): the
        // broken window mask changes of each recorded vehicle.
        constexpr std::uint16_t kVehicleWindowsVersion = 1;
        void WriteVehicleWindows(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.vehicleWindows.size());
            for (const WindowTrack& track : c.recording.vehicleWindows)
            {
                w.Put(track.id);
                w.Count(track.states.size());
                for (const auto& [timeMs, mask] : track.states)
                {
                    w.Put(timeMs);
                    w.Put(mask);
                }
            }
        }
        void ReadVehicleWindows(Reader& r, LoadContext& c)
        {
            c.recording.vehicleWindows.resize(r.Count(8));
            for (WindowTrack& track : c.recording.vehicleWindows)
            {
                track.id = r.Get<std::uint32_t>();
                track.states.resize(r.Count(5));
                std::uint32_t lastMs = 0;
                for (auto& [timeMs, mask] : track.states)
                {
                    timeMs = r.Get<std::uint32_t>();
                    mask = r.Get<std::uint8_t>();
                    if (mask >= (1u << kVehicleWindows) || timeMs < lastMs)
                        r.Fail();
                    lastMs = timeMs;
                }
            }
        }

        // Section "water gpu" (WGPU) is retired (kRetiredSections below).

        // Section "vehicle burnt" (VBRN, optional): the
        // burnt state changes of each recorded vehicle (ScorchTrack layout).
        constexpr std::uint16_t kVehicleBurntVersion = 1;
        void WriteVehicleBurnt(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.vehicleBurnt.size());
            for (const ScorchTrack& track : c.recording.vehicleBurnt)
            {
                w.Put(track.id);
                w.Count(track.states.size());
                for (const auto& [timeMs, on] : track.states)
                {
                    w.Put(timeMs);
                    w.Put(static_cast<std::uint8_t>(on ? 1 : 0));
                }
            }
        }
        void ReadVehicleBurnt(Reader& r, LoadContext& c)
        {
            c.recording.vehicleBurnt.resize(r.Count(8));
            for (ScorchTrack& track : c.recording.vehicleBurnt)
            {
                track.id = r.Get<std::uint32_t>();
                track.states.resize(r.Count(5));
                std::uint32_t lastMs = 0;
                for (auto& [timeMs, on] : track.states)
                {
                    timeMs = r.Get<std::uint32_t>();
                    const std::uint8_t value = r.Get<std::uint8_t>();
                    if (value > 1 || timeMs < lastMs)
                        r.Fail();
                    on = value != 0;
                    lastMs = timeMs;
                }
            }
        }

        // Section "vehicle parts" (VPRT, optional): the
        // lost wheel and door mask changes of each recorded vehicle (bits
        // 0..5 wheels, 6..15 doors, VehiclePartTrack).
        constexpr std::uint16_t kVehiclePartsVersion = 1;
        void WriteVehicleParts(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.vehicleParts.size());
            for (const VehiclePartTrack& track : c.recording.vehicleParts)
            {
                w.Put(track.id);
                w.Count(track.states.size());
                for (const auto& [timeMs, mask] : track.states)
                {
                    w.Put(timeMs);
                    w.Put(mask);
                }
            }
        }
        void ReadVehicleParts(Reader& r, LoadContext& c)
        {
            c.recording.vehicleParts.resize(r.Count(8));
            for (VehiclePartTrack& track : c.recording.vehicleParts)
            {
                track.id = r.Get<std::uint32_t>();
                track.states.resize(r.Count(6));
                std::uint32_t lastMs = 0;
                for (auto& [timeMs, mask] : track.states)
                {
                    timeMs = r.Get<std::uint32_t>();
                    mask = r.Get<std::uint16_t>();
                    if (timeMs < lastMs)
                        r.Fail();
                    lastMs = timeMs;
                }
            }
        }

        // Section "vehicle doors" (VDOR, optional, left out when empty,
        // vehicle_doors.h): the door ratio changes of
        // each recorded vehicle and train car (script door 0..9, latch 15).
        // Clips without it play the doors as the puppet spawns them (shut).
        constexpr std::uint16_t kVehicleDoorsVersion = 1;
        void WriteVehicleDoors(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.vehicleDoors.size());
            for (const vehdoors::DoorTrack& track : c.recording.vehicleDoors)
            {
                w.Put(track.id);
                w.Count(track.samples.size());
                for (const vehdoors::DoorSample& sample : track.samples)
                {
                    w.Put(sample.timeMs);
                    w.Put(sample.door);
                    w.Put(sample.ratio);
                }
            }
        }
        void ReadVehicleDoors(Reader& r, LoadContext& c)
        {
            c.recording.vehicleDoors.resize(r.Count(8));
            for (vehdoors::DoorTrack& track : c.recording.vehicleDoors)
            {
                track.id = r.Get<std::uint32_t>();
                track.samples.resize(r.Count(9));
                std::uint32_t lastMs = 0;
                for (vehdoors::DoorSample& sample : track.samples)
                {
                    sample.timeMs = r.Get<std::uint32_t>();
                    sample.door = r.Get<std::uint8_t>();
                    sample.ratio = r.Get<float>();
                    if (sample.timeMs < lastMs || sample.door >= vehdoors::kChannels || !(sample.ratio >= 0.0f) ||
                        sample.ratio > 1.0f)
                        r.Fail();
                    lastMs = sample.timeMs;
                }
            }
        }
        bool OmitVehicleDoors(const SaveContext& c)
        {
            return c.recording.vehicleDoors.empty();
        }

        // Section "burn levels" (BLVL, optional): the burn level changes of
        // each recorded vehicle, ped and horse (BurnLevelTrack), and of each
        // recorded weapon object and prop (same payload, version 1). Clips
        // without it play without the level (the fire replayed on a vehicle
        // puppet darkens it; peds and horses stay clean).
        constexpr std::uint16_t kBurnLevelsVersion = 1;
        void WriteBurnTracks(Writer& w, const std::vector<BurnLevelTrack>& tracks)
        {
            w.Count(tracks.size());
            for (const BurnLevelTrack& track : tracks)
            {
                w.Put(track.id);
                w.Count(track.states.size());
                for (const auto& [timeMs, level] : track.states)
                {
                    w.Put(timeMs);
                    w.Put(level);
                }
            }
        }
        void ReadBurnTracks(Reader& r, std::vector<BurnLevelTrack>& tracks)
        {
            tracks.resize(r.Count(8));
            for (BurnLevelTrack& track : tracks)
            {
                track.id = r.Get<std::uint32_t>();
                track.states.resize(r.Count(8));
                std::uint32_t lastMs = 0;
                for (auto& [timeMs, level] : track.states)
                {
                    timeMs = r.Get<std::uint32_t>();
                    level = r.Get<float>();
                    if (timeMs < lastMs || !std::isfinite(level))
                        r.Fail();
                    lastMs = timeMs;
                }
            }
        }
        void WriteBurnLevels(Writer& w, const SaveContext& c) { WriteBurnTracks(w, c.recording.burnLevels); }
        void ReadBurnLevels(Reader& r, LoadContext& c) { ReadBurnTracks(r, c.recording.burnLevels); }

        // Section "scenery burn levels" (SBLV,
        // optional): the burn level changes of each scenery object, the BLVL
        // payload with the entry's index in SCNY as the track id. Written
        // only when the recording has one (a build that does not know the
        // section still opens the clip); a track whose index is no scenery
        // entry is dropped by ValidateLoaded.
        constexpr std::uint16_t kSceneryBurnLevelsVersion = 1;
        void WriteSceneryBurnLevels(Writer& w, const SaveContext& c) { WriteBurnTracks(w, c.recording.sceneryBurnLevels); }
        void ReadSceneryBurnLevels(Reader& r, LoadContext& c) { ReadBurnTracks(r, c.recording.sceneryBurnLevels); }
        bool OmitSceneryBurnLevels(const SaveContext& c) { return c.recording.sceneryBurnLevels.empty(); }

        // Section "object looks" (OLOK, optional, object_look_core.h): the
        // custom texture (document text: txd, p2, p3 hashes) and tint changes
        // of each recorded weapon object and prop (track id: packed id), then
        // of each scenery object (track id: its index in SCNY). Written only
        // when the recording has one (a reader that does not know the section
        // still opens the clip); ValidateLoaded
        // drops a scenery track whose index is no scenery entry and bounds
        // the tint.
        constexpr std::uint16_t kObjectLooksVersion = 1;
        void WriteObjectLookTracks(Writer& w, const std::vector<ObjectLookTrack>& tracks)
        {
            w.Count(tracks.size());
            for (const ObjectLookTrack& track : tracks)
            {
                w.Put(track.id);
                w.Count(track.states.size());
                for (const auto& [timeMs, look] : track.states)
                {
                    w.Put(timeMs);
                    w.Put(look.txd);
                    w.Put(look.p2);
                    w.Put(look.p3);
                    w.Put(look.tint);
                }
            }
        }
        void ReadObjectLookTracks(Reader& r, std::vector<ObjectLookTrack>& tracks)
        {
            tracks.resize(r.Count(8));
            for (ObjectLookTrack& track : tracks)
            {
                track.id = r.Get<std::uint32_t>();
                track.states.resize(r.Count(17));
                std::uint32_t lastMs = 0;
                for (auto& [timeMs, look] : track.states)
                {
                    timeMs = r.Get<std::uint32_t>();
                    look.txd = r.Get<std::uint32_t>();
                    look.p2 = r.Get<std::uint32_t>();
                    look.p3 = r.Get<std::uint32_t>();
                    look.tint = r.Get<std::uint8_t>();
                    if (timeMs < lastMs)
                        r.Fail();
                    lastMs = timeMs;
                }
            }
        }
        void WriteObjectLooks(Writer& w, const SaveContext& c)
        {
            WriteObjectLookTracks(w, c.recording.objectLooks);
            WriteObjectLookTracks(w, c.recording.sceneryObjectLooks);
        }
        void ReadObjectLooks(Reader& r, LoadContext& c)
        {
            ReadObjectLookTracks(r, c.recording.objectLooks);
            ReadObjectLookTracks(r, c.recording.sceneryObjectLooks);
        }
        bool OmitObjectLooks(const SaveContext& c)
        {
            return c.recording.objectLooks.empty() && c.recording.sceneryObjectLooks.empty();
        }
        void ResetObjectLooks(LoadContext& c)
        {
            c.recording.objectLooks.clear();
            c.recording.sceneryObjectLooks.clear();
        }

        // Section "fire chars" (FREC, optional): the fire record (burnt look)
        // changes of each recorded ped, horse and vehicle (FireCharTrack):
        // time, present, flags, volume count and the kept floats of each
        // volume. Clips without it play without the look (the fire replayed
        // on a puppet still burns it in normal play).
        // Version 2: after the floats of each volume its anchor
        // (component i32, sub u16, flags u8, mask u32, direction 3 x f32);
        // version 1 reads without anchors.
        constexpr std::uint16_t kFireCharsVersion = 2;
        constexpr std::uint16_t kFireCharsMinVersion = 1;
        void WriteFireChars(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.fireChars.size());
            for (const FireCharTrack& track : c.recording.fireChars)
            {
                w.Put(track.id);
                w.Count(track.states.size());
                for (const auto& [timeMs, state] : track.states)
                {
                    w.Put(timeMs);
                    w.Put(static_cast<std::uint8_t>(state.present ? 1 : 0));
                    w.Put(state.flags);
                    w.Count(state.volumes.size());
                    for (std::size_t n = 0; n < state.volumes.size(); ++n)
                    {
                        for (float value : state.volumes[n])
                            w.Put(value);
                        const FireCharAnchor anchor = n < state.anchors.size() ? state.anchors[n] : FireCharAnchor{};
                        w.Put(anchor.component);
                        w.Put(anchor.sub);
                        w.Put(anchor.flags);
                        w.Put(anchor.mask);
                        for (float value : anchor.dir)
                            w.Put(value);
                    }
                }
            }
        }
        void ReadFireChars(Reader& r, LoadContext& c)
        {
            c.recording.fireChars.resize(r.Count(8));
            for (FireCharTrack& track : c.recording.fireChars)
            {
                track.id = r.Get<std::uint32_t>();
                track.states.resize(r.Count(6));
                std::uint32_t lastMs = 0;
                for (auto& [timeMs, state] : track.states)
                {
                    timeMs = r.Get<std::uint32_t>();
                    state.present = r.Get<std::uint8_t>() != 0;
                    state.flags = r.Get<std::uint8_t>();
                    const bool anchored = c.sectionVersion >= 2;
                    state.volumes.resize(r.Count(kFireCharFloats * 4 + (anchored ? 23 : 0)));
                    if (state.volumes.size() > 64)
                        r.Fail();
                    if (anchored)
                        state.anchors.resize(state.volumes.size());
                    for (std::size_t n = 0; n < state.volumes.size(); ++n)
                    {
                        for (float& value : state.volumes[n])
                        {
                            value = r.Get<float>();
                            if (!std::isfinite(value))
                                r.Fail();
                        }
                        if (!anchored)
                            continue;
                        FireCharAnchor& anchor = state.anchors[n];
                        anchor.component = r.Get<std::int32_t>();
                        anchor.sub = r.Get<std::uint16_t>();
                        anchor.flags = r.Get<std::uint8_t>();
                        anchor.mask = r.Get<std::uint32_t>();
                        for (float& value : anchor.dir)
                        {
                            value = r.Get<float>();
                            if (!std::isfinite(value))
                                r.Fail();
                        }
                    }
                    if (timeMs < lastMs)
                        r.Fail();
                    lastMs = timeMs;
                }
            }
        }

        // Section "appearance changes" (APPC, optional): the metaped layers a
        // recorded ped or horse took mid-clip without a skeleton change
        // (appearance_track.h). Clips without it wear the create and segment
        // layers only.
        constexpr std::uint16_t kAppearanceChangesVersion = 1;
        void WriteAppearanceChanges(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.appearances.size());
            for (const AppearanceTrack& track : c.recording.appearances)
            {
                w.Put(track.id);
                w.Count(track.changes.size());
                for (const AppearanceChange& change : track.changes)
                {
                    w.Put(change.timeMs);
                    WriteAppearance(w, change.appearance);
                }
            }
        }
        void ReadAppearanceChanges(Reader& r, LoadContext& c)
        {
            c.recording.appearances.resize(r.Count(12));
            for (AppearanceTrack& track : c.recording.appearances)
            {
                track.id = r.Get<std::uint32_t>();
                track.changes.resize(r.Count(24));
                std::uint32_t lastMs = 0;
                for (AppearanceChange& change : track.changes)
                {
                    change.timeMs = r.Get<std::uint32_t>();
                    ReadAppearance(r, change.appearance);
                    if (change.timeMs < lastMs)
                        r.Fail();
                    lastMs = change.timeMs;
                }
            }
        }

        // Section "metaped" (MPED, optional, metaped_state.h): the whole
        // metaped look of each ped and horse (stable expressions, scale,
        // cleanliness, outfit, type) at its entry, at each skeleton change and
        // at each change the poll saw. Clips without it wear the layers only
        // (the puppet's own random face).
        // v2 adds after each state's expressions its shop items (item,
        // wearable state, flags). Written only when a state has one
        // (MetapedSaveVersion), so a clip without them stays v1 for v1
        // readers; v1 is still read (no items).
        constexpr std::uint16_t kMetapedVersion = 2;
        constexpr std::uint16_t kMetapedV1Version = 1;
        constexpr std::uint32_t kMetapedMaxExpressions = 4096;
        bool MetapedHasShopItems(const SaveContext& c)
        {
            for (const metaped::Track& track : c.recording.metapeds)
                for (const metaped::State& state : track.states)
                    if (!state.shopItems.empty())
                        return true;
            return false;
        }
        std::uint16_t MetapedSaveVersion(const SaveContext& c)
        {
            return MetapedHasShopItems(c) ? kMetapedVersion : kMetapedV1Version;
        }
        void WriteMetapeds(Writer& w, const SaveContext& c)
        {
            const bool v2 = MetapedHasShopItems(c);
            w.Count(c.recording.metapeds.size());
            for (const metaped::Track& track : c.recording.metapeds)
            {
                w.Put(track.id);
                w.Count(track.states.size());
                for (const metaped::State& state : track.states)
                {
                    w.Put(state.timeMs);
                    w.Enum(state.source);
                    w.Put(state.flags);
                    w.Put(state.outfit);
                    w.Put(state.metapedType);
                    w.Put(state.layerCount);
                    w.Put(state.layersPrint);
                    w.Put(state.scale);
                    w.Put(state.cleanliness);
                    w.Count(state.expressions.size());
                    for (const metaped::Expression& e : state.expressions)
                    {
                        w.Put(e.id);
                        w.Put(e.value);
                    }
                    if (!v2)
                        continue;
                    w.Count(state.shopItems.size());
                    for (const metaped::ShopItem& item : state.shopItems)
                    {
                        w.Put(item.item);
                        w.Put(item.state);
                        w.Put(item.flags);
                    }
                }
            }
        }
        void ReadMetapeds(Reader& r, LoadContext& c)
        {
            c.recording.metapeds.resize(r.Count(8));
            for (metaped::Track& track : c.recording.metapeds)
            {
                track.id = r.Get<std::uint32_t>();
                track.states.resize(r.Count(41));
                std::uint32_t lastMs = 0;
                for (metaped::State& state : track.states)
                {
                    state.timeMs = r.Get<std::uint32_t>();
                    state.source = r.Enum<metaped::Source>(static_cast<std::uint8_t>(metaped::Source::Late));
                    state.flags = r.Get<std::uint32_t>();
                    state.outfit = r.Get<std::uint32_t>();
                    state.metapedType = r.Get<std::int32_t>();
                    state.layerCount = r.Get<std::uint32_t>();
                    state.layersPrint = r.Get<std::uint64_t>();
                    state.scale = r.Get<float>();
                    state.cleanliness = r.Get<std::int32_t>();
                    const std::size_t count = r.Count(6);
                    if (count > kMetapedMaxExpressions)
                    {
                        r.Fail();
                        return;
                    }
                    state.expressions.resize(count);
                    std::uint16_t lastId = 0;
                    for (std::size_t n = 0; n < count; ++n)
                    {
                        metaped::Expression& e = state.expressions[n];
                        e.id = r.Get<std::uint16_t>();
                        e.value = r.Get<float>();
                        if (!std::isfinite(e.value) || (n && e.id <= lastId))
                            r.Fail();
                        lastId = e.id;
                    }
                    if (c.sectionVersion >= 2)
                    {
                        const std::size_t items = r.Count(9);
                        if (items > metaped::kMaxShopItems)
                        {
                            r.Fail();
                            return;
                        }
                        state.shopItems.resize(items);
                        for (metaped::ShopItem& item : state.shopItems)
                        {
                            item.item = r.Get<std::uint32_t>();
                            item.state = r.Get<std::uint32_t>();
                            item.flags = r.Get<std::uint8_t>();
                            if (!item.item)
                                r.Fail(); // AddShopItem never keeps 0
                        }
                    }
                    if (state.timeMs < lastMs)
                        r.Fail();
                    lastMs = state.timeMs;
                }
            }
        }

        // Section "anim audio" (AANI, optional): the animation
        // audio events of recorded peds and horses (entity_events.h).
        constexpr std::uint16_t kAnimAudioVersion = 1;
        void WriteAnimAudioEvent(Writer& w, const entity::AnimAudioEvent& e)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Put(e.hash);
            w.Put(e.extra);
        }
        void ReadAnimAudioEvent(Reader& r, entity::AnimAudioEvent& e)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            e.hash = r.Get<std::uint32_t>();
            e.extra = r.Get<std::int32_t>();
        }
        void WriteAnimAudio(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.animAudio.Events(), WriteAnimAudioEvent); }
        void ReadAnimAudio(Reader& r, LoadContext& c) { ReadEvents(r, c.recording.animAudio, 16, ReadAnimAudioEvent); }

        // Section "registered fx attaches" (RFXA, optional): the attach
        // changes of the registered fx tracks, by track index (the
        // registeredFx section comes first in the file). A clip without it
        // loads with no attach changes.
        constexpr std::uint16_t kRegisteredFxAttachesVersion = 1;
        void WriteRegisteredFxAttaches(Writer& w, const SaveContext& c)
        {
            const std::vector<entity::RegisteredFxTrack>& tracks = c.recording.registeredFx;
            std::size_t withTimeline = 0;
            for (const entity::RegisteredFxTrack& track : tracks)
                withTimeline += track.attachTimeline.empty() ? 0 : 1;
            w.Count(withTimeline);
            for (std::size_t index = 0; index < tracks.size(); ++index)
            {
                if (tracks[index].attachTimeline.empty())
                    continue;
                w.Put(static_cast<std::uint32_t>(index));
                w.Count(tracks[index].attachTimeline.size());
                for (const entity::RegisteredFxAttach& entry : tracks[index].attachTimeline)
                {
                    w.Put(entry.timeMs);
                    w.Id(entry.id);
                    w.Put(entry.bone);
                }
            }
        }
        void ReadRegisteredFxAttaches(Reader& r, LoadContext& c)
        {
            std::vector<entity::RegisteredFxTrack>& tracks = c.recording.registeredFx;
            const std::size_t count = r.Count(4 + 8);
            std::uint32_t lastIndex = 0;
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                const std::uint32_t index = r.Get<std::uint32_t>();
                // Each track once, in order, and one the registeredFx section has.
                if (index >= tracks.size() || (n > 0 && index <= lastIndex) || !tracks[index].attachTimeline.empty())
                {
                    r.Fail();
                    return;
                }
                lastIndex = index;
                std::vector<entity::RegisteredFxAttach>& timeline = tracks[index].attachTimeline;
                timeline.resize(r.Count(12));
                std::uint32_t lastMs = 0;
                for (entity::RegisteredFxAttach& entry : timeline)
                {
                    entry.timeMs = r.Get<std::uint32_t>();
                    entry.id = r.Id();
                    entry.bone = r.Get<std::int32_t>();
                    if (entry.timeMs < lastMs)
                        r.Fail();
                    lastMs = entry.timeMs;
                }
            }
        }

        // Section "vehicle looks" (VEHL, optional): cargo prop
        // set, lantern and extras per recorded vehicle. Version 2
        // adds the latest tint and livery (a version 1 payload loads them
        // as kLookUnset: the create packet's values apply).
        constexpr std::uint16_t kVehicleLooksVersion = 2;
        constexpr std::uint16_t kVehicleLooksMinVersion = 1;
        void WriteVehicleLooks(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.vehicleLooks.size());
            for (const VehicleLook& look : c.recording.vehicleLooks)
            {
                w.Put(look.id.Packed());
                w.Put(look.propSet);
                w.Put(look.lightPropSet);
                w.Put(look.extrasExist);
                w.Put(look.extrasOn);
                w.Put(look.tint);   // version 2
                w.Put(look.livery); // version 2
            }
        }
        void ReadVehicleLooks(Reader& r, LoadContext& c)
        {
            const std::uint16_t version = c.sectionVersion;
            const std::size_t count = r.Count(version >= 2 ? 28 : 20);
            for (std::size_t n = 0; n < count; ++n)
            {
                VehicleLook look;
                look.id = entity::ReplayId::FromPacked(r.Get<std::uint32_t>());
                look.propSet = r.Get<std::uint32_t>();
                look.lightPropSet = r.Get<std::uint32_t>();
                look.extrasExist = r.Get<std::uint32_t>();
                look.extrasOn = r.Get<std::uint32_t>();
                if (version >= 2)
                {
                    look.tint = r.Get<std::int32_t>();
                    look.livery = r.Get<std::int32_t>();
                }
                c.recording.vehicleLooks.push_back(look);
            }
        }

        // Section "doors" (DOOR, optional): map doors by hash.
        constexpr std::uint16_t kDoorsVersion = 1;
        void WriteDoors(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.doors.size());
            for (const DoorTrack& door : c.recording.doors)
            {
                w.Put(door.hash);
                w.Count(door.samples.size());
                for (const DoorSample& e : door.samples)
                {
                    w.Put(e.timeMs);
                    for (float v : e.rotation)
                        w.Put(v);
                    for (float v : e.position)
                        w.Put(v);
                }
            }
        }
        void ReadDoors(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(8);
            for (std::size_t n = 0; n < count; ++n)
            {
                DoorTrack door;
                door.hash = r.Get<std::uint32_t>();
                const std::size_t samples = r.Count(4 + 6 * 4);
                std::uint32_t lastMs = 0;
                for (std::size_t k = 0; k < samples; ++k)
                {
                    DoorSample e;
                    e.timeMs = r.Get<std::uint32_t>();
                    for (float& v : e.rotation)
                        v = r.Get<float>();
                    for (float& v : e.position)
                        v = r.Get<float>();
                    if (e.timeMs < lastMs)
                        r.Fail();
                    lastMs = e.timeMs;
                    door.samples.push_back(e);
                }
                c.recording.doors.push_back(std::move(door));
            }
        }

        // Section "meleeHits" (MELE). info is a heap pointer (0); its
        // vtable points into the image (RVA). infoHash is the action
        // result's name hash, which playback resolves back to the engine's
        // object (audio_observer.cpp FindActionResult), so the
        // format needs no new field.
        constexpr std::uint16_t kMeleeHitsVersion = 1;
        void WriteMelee(Writer& w, const entity::MeleeEvent& e, const Image& image)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Put(ToRva(e.infoVtable, image));
            w.Put(e.infoHash);
        }
        void ReadMelee(Reader& r, entity::MeleeEvent& e, const Image& image)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            e.info = 0;
            e.infoVtable = FromRva(r.Get<std::uint64_t>(), image);
            e.infoHash = r.Get<std::uint32_t>();
        }
        void WriteMeleeHits(Writer& w, const SaveContext& c)
        {
            WriteEvents(w, c.recording.meleeHits.Events(),
                [&](Writer& writer, const entity::MeleeEvent& e) { WriteMelee(writer, e, c.image); });
        }
        void ReadMeleeHits(Reader& r, LoadContext& c)
        {
            ReadEvents(r, c.recording.meleeHits, 20,
                [&](Reader& reader, entity::MeleeEvent& e) { ReadMelee(reader, e, c.image); });
        }

        // Section "bodyImpacts" (BIMP). definition is a heap pointer (0);
        // its vtable points into the image (RVA).
        constexpr std::uint16_t kBodyImpactsVersion = 1;
        void WriteBodyImpact(Writer& w, const entity::BodyImpactEvent& e, const Image& image)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Id(e.other);
            w.Array(e.position);
            w.Put(e.arg4);
            w.Put(e.arg5);
            w.Put(ToRva(e.definitionVtable, image));
            w.Put(e.definitionHash);
            w.Put(e.arg7);
            w.Put(e.arg10);
        }
        void ReadBodyImpact(Reader& r, entity::BodyImpactEvent& e, const Image& image)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            e.other = r.Id();
            r.Array(e.position);
            e.arg4 = r.Get<std::uint64_t>();
            e.arg5 = r.Get<std::uint64_t>();
            e.definition = 0;
            e.definitionVtable = FromRva(r.Get<std::uint64_t>(), image);
            e.definitionHash = r.Get<std::uint32_t>();
            e.arg7 = r.Get<std::uint64_t>();
            e.arg10 = r.Get<std::uint64_t>();
        }
        void WriteBodyImpacts(Writer& w, const SaveContext& c)
        {
            WriteEvents(w, c.recording.bodyImpacts.Events(),
                [&](Writer& writer, const entity::BodyImpactEvent& e) { WriteBodyImpact(writer, e, c.image); });
        }
        void ReadBodyImpacts(Reader& r, LoadContext& c)
        {
            ReadEvents(r, c.recording.bodyImpacts, 72,
                [&](Reader& reader, entity::BodyImpactEvent& e) { ReadBodyImpact(reader, e, c.image); });
        }

        // Sound params without the engine's fields (see kSoundParams*).
        void WriteSoundParams(Writer& w, const std::vector<std::uint8_t>& params)
        {
            std::vector<std::uint8_t> copy = params;
            if (copy.size() > kSoundParamsEngineStart)
                ZeroBytes(copy.data(), copy.size(), kSoundParamsEngineStart, kSoundParamsEngineEnd - kSoundParamsEngineStart);
            if (copy.size() > kSoundParamsHandle)
                ZeroBytes(copy.data(), copy.size(), kSoundParamsHandle, kSoundParamsHandleBytes);
            w.Blob(copy);
        }
        void ReadSoundParams(Reader& r, std::vector<std::uint8_t>& params)
        {
            r.Blob(params);
            // Playback reads a full params block when there is one.
            if (!params.empty() && params.size() != kSoundParamsBytes)
                r.Fail();
        }

        // Section "sounds" (SNDS). globalAudio is a static audio entity in
        // the image (RVA).
        constexpr std::uint16_t kSoundsVersion = 1;
        void WriteSound(Writer& w, const entity::SoundEvent& e, const Image& image)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Put(e.audioOffset);
            w.Put(e.soundRef);
            WriteSoundParams(w, e.params);
            w.Array(e.flags);
            w.Put(e.kind);
            w.Put(ToRva(e.globalAudio, image));
        }
        void ReadSound(Reader& r, entity::SoundEvent& e, const Image& image)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            e.audioOffset = r.Get<std::uint64_t>();
            e.soundRef = r.Get<std::uint64_t>();
            ReadSoundParams(r, e.params);
            r.Array(e.flags);
            e.kind = r.Get<std::uint8_t>();
            e.globalAudio = FromRva(r.Get<std::uint64_t>(), image);
        }
        void WriteSounds(Writer& w, const SaveContext& c)
        {
            WriteEvents(w, c.recording.sounds.Events(),
                [&](Writer& writer, const entity::SoundEvent& e) { WriteSound(writer, e, c.image); });
        }
        void ReadSounds(Reader& r, LoadContext& c)
        {
            ReadEvents(r, c.recording.sounds, 69,
                [&](Reader& reader, entity::SoundEvent& e) { ReadSound(reader, e, c.image); });
        }

        // Section "collisionSounds" (CSND).
        constexpr std::uint16_t kCollisionSoundsVersion = 1;
        void WriteCollisionSounds(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.collisionSounds.size());
            for (const CollisionSound& sound : c.recording.collisionSounds)
            {
                w.Id(sound.entity);
                w.Put(sound.startMs);
                w.Put(sound.endMs);
                w.Bool(sound.ended);
                w.Put(sound.soundRef);
                WriteSoundParams(w, sound.params);
                w.Count(sound.intensity.size());
                for (const auto& [timeMs, value] : sound.intensity)
                {
                    w.Put(timeMs);
                    w.Put(value);
                }
            }
        }
        void ReadCollisionSounds(Reader& r, LoadContext& c)
        {
            c.recording.collisionSounds.resize(r.Count(37));
            for (CollisionSound& sound : c.recording.collisionSounds)
            {
                sound.entity = r.Id();
                sound.startMs = r.Get<std::uint32_t>();
                sound.endMs = r.Get<std::uint32_t>();
                sound.ended = r.Bool();
                sound.soundRef = r.Get<std::uint64_t>();
                ReadSoundParams(r, sound.params);
                sound.intensity.resize(r.Count(8));
                for (auto& [timeMs, value] : sound.intensity)
                {
                    timeMs = r.Get<std::uint32_t>();
                    value = r.Get<float>();
                }
            }
        }

        // Section "entity sounds" (ESND, optional): sounds tied to a
        // recorded object or vehicle (replay_recording.h EntitySound).
        // Version 2: oneShot after the params; version 1 reads as a
        // persistent sound (all it could hold).
        // Version 3: pedAudio after oneShot (ped audio sounds).
        // Version 4: vehicleAudio after pedAudio (train car sounds).
        constexpr std::uint16_t kEntitySoundsVersion = 4;
        void WriteEntitySounds(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.entitySounds.size());
            for (const EntitySound& sound : c.recording.entitySounds)
            {
                w.Id(sound.entity);
                w.Put(sound.startMs);
                w.Put(sound.endMs);
                w.Bool(sound.ended);
                w.Put(sound.soundRef);
                w.Bool(sound.byHash);
                w.Bool(sound.audioOnEntity);
                w.Put(sound.audioRva);
                w.Put(sound.audioOffset);
                w.Put(sound.trackerOffset);
                w.Put(sound.caller);
                WriteSoundParams(w, sound.params);
                w.Bool(sound.oneShot);
                w.Bool(sound.pedAudio);
                w.Bool(sound.vehicleAudio);
            }
        }
        void ReadEntitySounds(Reader& r, LoadContext& c)
        {
            c.recording.entitySounds.resize(r.Count(47));
            for (EntitySound& sound : c.recording.entitySounds)
            {
                sound.entity = r.Id();
                sound.startMs = r.Get<std::uint32_t>();
                sound.endMs = r.Get<std::uint32_t>();
                sound.ended = r.Bool();
                sound.soundRef = r.Get<std::uint64_t>();
                sound.byHash = r.Bool();
                sound.audioOnEntity = r.Bool();
                sound.audioRva = r.Get<std::uint64_t>();
                sound.audioOffset = r.Get<std::uint32_t>();
                sound.trackerOffset = r.Get<std::uint32_t>();
                sound.caller = r.Get<std::uint32_t>();
                ReadSoundParams(r, sound.params);
                sound.oneShot = c.sectionVersion >= 2 ? r.Bool() : false;
                sound.pedAudio = c.sectionVersion >= 3 ? r.Bool() : false;
                sound.vehicleAudio = c.sectionVersion >= 4 ? r.Bool() : false;
                // The audio entity and tracker offsets
                // become engine pointers in playback: only the ones the
                // recorder can produce are accepted.
                if (!EntitySoundAudioValid(sound, c.image.size))
                    r.Fail();
            }
        }

        // Section "trains" (TRNC, optional): the recorded train cars and the
        // head cars' speed (train_consist.h). The cars' world tracks are
        // weapon tracks (WTRK) keyed by their ids; clips without it have no
        // train car.
        constexpr std::uint16_t kTrainsVersion = 1;
        void WriteTrains(Writer& w, const SaveContext& c)
        {
            const trains::TrainData& data = c.recording.trains;
            w.Count(data.cars.size());
            for (const trains::TrainCar& car : data.cars)
            {
                w.Put(car.id);
                w.Put(car.head);
                w.Put(car.forward);
                w.Put(car.backward);
                w.Put(car.index);
                w.Put(car.cars);
                w.Put(car.flags);
                w.Put(car.vehicleType);
                w.Put(car.track);
                w.Put(car.propSet);
                w.Put(car.lightPropSet);
            }
            w.Count(data.speeds.size());
            for (const trains::SpeedTrack& track : data.speeds)
            {
                w.Put(track.head);
                w.Count(track.samples.size());
                for (const trains::SpeedSample& sample : track.samples)
                {
                    w.Put(sample.timeMs);
                    w.Put(sample.speed);
                    w.Put(sample.forward);
                }
            }
        }
        void ReadTrains(Reader& r, LoadContext& c)
        {
            trains::TrainData& data = c.recording.trains;
            data.Clear();
            data.cars.resize(r.Count(34));
            for (trains::TrainCar& car : data.cars)
            {
                car.id = r.Get<std::uint32_t>();
                car.head = r.Get<std::uint32_t>();
                car.forward = r.Get<std::uint32_t>();
                car.backward = r.Get<std::uint32_t>();
                car.index = r.Get<std::uint16_t>();
                car.cars = r.Get<std::uint16_t>();
                car.flags = r.Get<std::uint8_t>();
                car.vehicleType = r.Get<std::uint8_t>();
                car.track = r.Get<std::int32_t>();
                car.propSet = r.Get<std::uint32_t>();
                car.lightPropSet = r.Get<std::uint32_t>();
                if (!car.id || car.cars > trains::kMaxConsistCars || car.index >= trains::kMaxConsistCars)
                    r.Fail();
            }
            data.speeds.resize(r.Count(12));
            for (trains::SpeedTrack& track : data.speeds)
            {
                track.head = r.Get<std::uint32_t>();
                track.samples.resize(r.Count(9));
                std::uint32_t last = 0;
                for (trains::SpeedSample& sample : track.samples)
                {
                    sample.timeMs = r.Get<std::uint32_t>();
                    sample.speed = r.Get<float>();
                    sample.forward = r.Get<std::uint8_t>();
                    if (sample.timeMs < last || sample.forward > 1 || !std::isfinite(sample.speed))
                        r.Fail();
                    last = sample.timeMs;
                }
            }
        }

        // Section "sound census" (SCEN, optional): the recording's engine
        // sound counts per owner, ref and caller path per 250 ms window
        // (sound_check_core.h), compared with each playback's. Clips without
        // it have none (the report says so).
        constexpr std::uint16_t kSoundCensusVersion = 1;
        void WriteSoundCensus(Writer& w, const SaveContext& c)
        {
            const census::Summary& summary = c.recording.soundCensus;
            w.Put(census::kWindowMs);
            w.Count(summary.keys.size());
            for (const census::SummaryKey& key : summary.keys)
            {
                w.Put(key.owner);
                w.Put(static_cast<std::uint8_t>(key.cls));
                w.Put(key.ref);
                for (const std::uint32_t caller : key.path)
                    w.Put(caller);
            }
            w.Count(summary.cells.size());
            for (const census::SummaryCell& cell : summary.cells)
            {
                w.Put(cell.window);
                w.Put(cell.key);
                w.Put(cell.count);
                w.Put(cell.refused);
            }
            w.Values(summary.covered);
            w.Put(summary.overflow);
        }
        void ReadSoundCensus(Reader& r, LoadContext& c)
        {
            census::Summary& summary = c.recording.soundCensus;
            summary.Clear();
            if (r.Get<std::uint32_t>() != census::kWindowMs)
                r.Fail();
            summary.keys.resize(r.Count(33));
            for (census::SummaryKey& key : summary.keys)
            {
                key.owner = r.Get<std::uint32_t>();
                const std::uint8_t cls = r.Get<std::uint8_t>();
                key.cls = static_cast<census::OwnerClass>(cls);
                key.ref = r.Get<std::uint64_t>();
                for (std::uint32_t& caller : key.path)
                    caller = r.Get<std::uint32_t>();
                if (cls >= census::kOwnerClassCount)
                    r.Fail();
            }
            if (summary.keys.size() > census::kMaxKeys)
                r.Fail();
            summary.cells.resize(r.Count(9));
            std::uint32_t last = 0;
            for (census::SummaryCell& cell : summary.cells)
            {
                cell.window = r.Get<std::uint32_t>();
                cell.key = r.Get<std::uint16_t>();
                cell.count = r.Get<std::uint16_t>();
                cell.refused = r.Get<std::uint8_t>();
                if (cell.key >= summary.keys.size() || cell.window < last)
                    r.Fail();
                last = cell.window;
            }
            r.Values(summary.covered);
            for (std::size_t n = 1; n < summary.covered.size(); ++n)
                if (summary.covered[n] <= summary.covered[n - 1])
                    r.Fail();
            summary.overflow = r.Get<std::uint64_t>();
        }

        // Section "general sounds" (GSND, optional): the sounds of
        // recorded vehicles, train cars and objects the census caught and no
        // other track covers (sound_capture.h). The audio base and offset
        // become an engine pointer in playback: only what the recorder makes
        // is accepted (GeneralSoundValid).
        constexpr std::uint16_t kGeneralSoundsVersion = 1;
        void WriteGeneralSounds(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.generalSounds.size());
            for (const census::GeneralSound& sound : c.recording.generalSounds)
            {
                w.Put(sound.entity);
                w.Put(static_cast<std::uint8_t>(sound.cls));
                w.Put(sound.startMs);
                w.Put(sound.endMs);
                w.Bool(sound.ended);
                w.Bool(sound.oneShot);
                w.Bool(sound.preexisting);
                w.Bool(sound.follows);
                w.Put(sound.ref);
                w.Put(static_cast<std::uint8_t>(sound.base));
                w.Put(sound.field);
                w.Put(sound.offset);
                w.Put(sound.vtableRva);
                for (const std::uint32_t caller : sound.path)
                    w.Put(caller);
                WriteSoundParams(w, sound.params);
            }
        }
        void ReadGeneralSounds(Reader& r, LoadContext& c)
        {
            c.recording.generalSounds.resize(r.Count(56));
            for (census::GeneralSound& sound : c.recording.generalSounds)
            {
                sound.entity = r.Get<std::uint32_t>();
                const std::uint8_t cls = r.Get<std::uint8_t>();
                sound.startMs = r.Get<std::uint32_t>();
                sound.endMs = r.Get<std::uint32_t>();
                sound.ended = r.Bool();
                sound.oneShot = r.Bool();
                sound.preexisting = r.Bool();
                sound.follows = r.Bool();
                sound.ref = r.Get<std::uint64_t>();
                const std::uint8_t base = r.Get<std::uint8_t>();
                sound.field = r.Get<std::uint16_t>();
                sound.offset = r.Get<std::int32_t>();
                sound.vtableRva = r.Get<std::uint32_t>();
                for (std::uint32_t& caller : sound.path)
                    caller = r.Get<std::uint32_t>();
                ReadSoundParams(r, sound.params);
                if (cls >= census::kOwnerClassCount || base >= census::kAudioBaseCount)
                {
                    r.Fail();
                    continue;
                }
                sound.cls = static_cast<census::OwnerClass>(cls);
                sound.base = static_cast<census::AudioBase>(base);
                if (!census::GeneralSoundValid(sound, c.image.size, kSoundParamsBytes))
                    r.Fail();
            }
        }

        // Section "sound generic" (SGEN, optional): the generic sound effect
        // packets (create, adjust, end; sound_generic_core.h). A clip without
        // it loads with an empty track. Every packet is written with the byte
        // count kCreatePacketBytes, AdjustPacketBytes and kEndPacketBytes say
        // (the budget counts the same bytes).
        // Version 2 adds, after the caller path of each create, the audio
        // entity's base, field, offset and vtable RVA (11 bytes); a version 1
        // payload loads with audioKnown false (the player falls back to the
        // static audio).
        // Version 3 adds the init variables of each create packet (params
        // +0x10, count +0x90): a count byte and that many (hash, value).
        // Version 4 adds the value dwords of the params away from the
        // constructor's (a count byte, then index and raw dword) and the bit
        // 0x10 of the bits byte (the game gave an environment group).
        constexpr std::uint16_t kSoundGenericVersion = 4;
        constexpr std::uint16_t kSoundGenericMinVersion = 1;
        void WriteSoundGeneric(Writer& w, const SaveContext& c)
        {
            const soundgeneric::Track& track = c.recording.soundGeneric;
            w.Put(track.nextIndex);
            w.Count(track.creates.size());
            for (const soundgeneric::CreatePacket& p : track.creates)
            {
                w.Put(p.timeMs);
                w.Put(p.index);
                w.Put(p.stableId);
                w.Enum(p.idKind);
                w.Put(p.rawRef);
                w.Put(p.owner);
                w.Enum(p.ownerClass);
                w.Enum(p.source);
                w.Put(p.core);
                w.Put<std::uint8_t>((p.hasPosition ? 1u : 0u) | (p.hasParams ? 2u : 0u) | (p.preexisting ? 4u : 0u) |
                    (p.params.hasTracker ? 8u : 0u) | (p.params.hasEnvGroup ? 0x10u : 0u)); // 0x10: version 4
                w.Put(p.createDelayMs);
                w.Array(p.params.position);
                w.Put(p.params.volume);
                w.Put(p.params.volume2);
                w.Put(p.params.pitch);
                w.Put(p.params.pan);
                w.Put(p.params.flags);
                w.Put(p.params.variableCount);
                w.Put<std::uint8_t>(0); // reserved
                // The slots past variableCount are written as zeros. The game
                // leaves them as they were in the init params (0xFFFFFFFF
                // seen, a NaN as a float), garbage the reader never uses.
                for (std::size_t n = 0; n < soundgeneric::kVariables; ++n)
                {
                    const bool used = n < p.params.variableCount;
                    w.Put(used ? p.params.variables[n].hash : 0u);
                    w.Put(used ? p.params.variables[n].value : 0.0f);
                }
                w.Put(p.caller);
                w.Array(p.path);
                w.Enum(p.audioKnown ? p.audioBase : census::AudioBase::Image); // version 2
                w.Put(p.audioField);
                w.Put(p.audioOffset);
                w.Put(p.audioKnown ? p.vtableRva : 0u);
                const std::uint8_t initCount = p.params.initVariableCount > soundgeneric::kInitVariables
                    ? static_cast<std::uint8_t>(soundgeneric::kInitVariables)
                    : p.params.initVariableCount; // version 3
                w.Put(initCount);
                for (std::uint8_t n = 0; n < initCount; ++n)
                {
                    w.Put(p.params.initVariables[n].hash);
                    w.Put(p.params.initVariables[n].value);
                }
                const std::uint8_t valueCount = p.params.valueCount > soundgeneric::kValueDwords
                    ? static_cast<std::uint8_t>(soundgeneric::kValueDwords)
                    : p.params.valueCount; // version 4
                w.Put(valueCount);
                for (std::uint8_t n = 0; n < valueCount; ++n)
                {
                    w.Put(p.params.values[n].index);
                    w.Put(p.params.values[n].raw);
                }
            }
            w.Count(track.adjusts.size());
            for (const soundgeneric::AdjustPacket& p : track.adjusts)
            {
                w.Put(p.timeMs);
                w.Put(p.index);
                w.Enum(p.kind);
                if (soundgeneric::IsVector(p.kind))
                    w.Array(p.value);
                else if (p.kind == soundgeneric::Adjust::Variable)
                {
                    w.Put(p.hash);
                    w.Put(p.value[0]);
                }
                else
                    w.Put(p.value[0]);
            }
            w.Count(track.ends.size());
            for (const soundgeneric::EndPacket& p : track.ends)
            {
                w.Put(p.timeMs);
                w.Put(p.index);
                w.Enum(p.reason);
            }
        }
        void ReadSoundGeneric(Reader& r, LoadContext& c)
        {
            soundgeneric::Track& track = c.recording.soundGeneric;
            track.Clear();
            track.nextIndex = r.Get<std::uint32_t>();
            // Every table sized by it must stay small (a 4 GiB nextIndex next
            // to one create asked the player for 16 GiB): refused past the
            // bound, the section left out.
            if (track.nextIndex > soundgeneric::kMaxNextIndex)
            {
                r.Fail();
                return;
            }
            const auto finite = [](float v) { return std::isfinite(v); };
            const std::uint16_t version = c.sectionVersion;
            track.creates.resize(r.Count(version >= 4   ? soundgeneric::kCreatePacketBytesV4Min
                    : version >= 3 ? soundgeneric::kCreatePacketBytesV3Min
                    : version >= 2 ? soundgeneric::kCreatePacketBytes
                                   : soundgeneric::kCreatePacketBytesV1));
            std::uint32_t lastMs = 0;
            for (soundgeneric::CreatePacket& p : track.creates)
            {
                p.timeMs = r.Get<std::uint32_t>();
                p.index = r.Get<std::uint32_t>();
                p.stableId = r.Get<std::uint64_t>();
                p.idKind = r.Enum<soundgeneric::IdKind>(soundgeneric::kIdKindCount - 1);
                p.rawRef = r.Get<std::uint64_t>();
                p.owner = r.Get<std::uint32_t>();
                p.ownerClass = r.Enum<census::OwnerClass>(census::kOwnerClassCount - 1);
                p.source = r.Enum<soundgeneric::Source>(soundgeneric::kSourceCount - 1);
                p.core = r.Get<std::uint8_t>();
                const std::uint8_t bits = r.Get<std::uint8_t>();
                p.hasPosition = (bits & 1u) != 0;
                p.hasParams = (bits & 2u) != 0;
                p.preexisting = (bits & 4u) != 0;
                p.params.hasTracker = (bits & 8u) != 0;
                p.params.hasEnvGroup = version >= 4 && (bits & 0x10u) != 0;
                p.createDelayMs = r.Get<std::uint16_t>();
                r.Array(p.params.position);
                p.params.volume = r.Get<float>();
                p.params.volume2 = r.Get<float>();
                p.params.pitch = r.Get<std::int16_t>();
                p.params.pan = r.Get<std::int16_t>();
                p.params.flags = r.Get<std::uint8_t>();
                p.params.variableCount = r.Get<std::uint8_t>();
                r.Get<std::uint8_t>(); // reserved
                for (soundgeneric::Variable& v : p.params.variables)
                {
                    v.hash = r.Get<std::uint32_t>();
                    v.value = r.Get<float>();
                }
                p.caller = r.Get<std::uint32_t>();
                r.Array(p.path);
                if (version >= 2)
                {
                    p.audioBase = r.Enum<census::AudioBase>(census::kAudioBaseCount - 1);
                    p.audioField = r.Get<std::uint16_t>();
                    p.audioOffset = r.Get<std::int32_t>();
                    p.vtableRva = r.Get<std::uint32_t>();
                    p.audioKnown = p.vtableRva != 0;
                }
                else
                    p.audioKnown = false;
                if (version >= 3) // the init variables
                {
                    p.params.initVariableCount = r.Get<std::uint8_t>();
                    if (p.params.initVariableCount > soundgeneric::kInitVariables)
                        r.Fail();
                    for (std::uint8_t n = 0; n < p.params.initVariableCount && r.Ok(); ++n)
                    {
                        p.params.initVariables[n].hash = r.Get<std::uint32_t>();
                        p.params.initVariables[n].value = r.Get<float>();
                        if (!finite(p.params.initVariables[n].value))
                            r.Fail();
                    }
                }
                if (version >= 4) // the value dwords
                {
                    p.params.valueCount = r.Get<std::uint8_t>();
                    if (p.params.valueCount > soundgeneric::kValueDwords)
                        r.Fail();
                    for (std::uint8_t n = 0; n < p.params.valueCount && r.Ok(); ++n)
                    {
                        p.params.values[n].index = r.Get<std::uint8_t>();
                        p.params.values[n].raw = r.Get<std::uint32_t>();
                        if (!soundgeneric::IsValueDwordIndex(p.params.values[n].index))
                            r.Fail();
                    }
                    p.params.valuesKnown = true;
                }
                if (p.index >= track.nextIndex || p.timeMs < lastMs || p.core > 1 || bits > (version >= 4 ? 31 : 15) ||
                    p.params.variableCount > soundgeneric::kVariables || !finite(p.params.position[0]) ||
                    !finite(p.params.position[1]) || !finite(p.params.position[2]) || !finite(p.params.volume) ||
                    !finite(p.params.volume2))
                    r.Fail();
                // The slots past the count (possibly garbage) are zeroed.
                // The slots the count names are not checked as floats:
                // params +0x10C is a list of raw dwords the creators copy,
                // not (hash, value) pairs, and one can be a NaN read as a
                // float (0xFFFD933F seen). The dwords go back as they came.
                for (std::size_t n = 0; n < soundgeneric::kVariables; ++n)
                {
                    if (n >= p.params.variableCount)
                        p.params.variables[n] = soundgeneric::Variable{};
                }
                lastMs = p.timeMs;
                if (!r.Ok())
                    return;
            }
            track.adjusts.resize(r.Count(soundgeneric::kAdjustPacketHeadBytes + 4));
            lastMs = 0;
            for (soundgeneric::AdjustPacket& p : track.adjusts)
            {
                p.timeMs = r.Get<std::uint32_t>();
                p.index = r.Get<std::uint32_t>();
                p.kind = r.Enum<soundgeneric::Adjust>(soundgeneric::kAdjustKinds - 1);
                if (!r.Ok())
                    return;
                if (soundgeneric::IsVector(p.kind))
                    r.Array(p.value);
                else if (p.kind == soundgeneric::Adjust::Variable)
                {
                    p.hash = r.Get<std::uint32_t>();
                    p.value[0] = r.Get<float>();
                }
                else
                    p.value[0] = r.Get<float>();
                if (p.index >= track.nextIndex || p.timeMs < lastMs || !finite(p.value[0]) || !finite(p.value[1]) ||
                    !finite(p.value[2]))
                    r.Fail();
                lastMs = p.timeMs;
                if (!r.Ok())
                    return;
            }
            track.ends.resize(r.Count(soundgeneric::kEndPacketBytes));
            lastMs = 0;
            for (soundgeneric::EndPacket& p : track.ends)
            {
                p.timeMs = r.Get<std::uint32_t>();
                p.index = r.Get<std::uint32_t>();
                p.reason = r.Enum<soundgeneric::EndReason>(soundgeneric::kEndReasonCount - 1);
                if (p.index >= track.nextIndex || p.timeMs < lastMs)
                    r.Fail();
                lastMs = p.timeMs;
                if (!r.Ok())
                    return;
            }
        }

        // Section "mix probe" (MIXP, mix_probe_core.h): per owned sound of the recording,
        // its owner, role, kind, ref, distances to the camera and the live
        // player, the engine distance, the ped audio LOD and the player
        // flags; once a second the camera, player and main character
        // positions and whether the game's music played. Evidence only
        // (nothing plays from it). Written only when the probe recorded
        // something (MixProbe=1), so readers that do not know the section
        // still open a clip without it.
        constexpr std::uint16_t kMixProbeVersion = 1;
        constexpr std::size_t kMixSampleBytes = 4 + 4 + 4 + 8 + 4 + 2 + 2 + 2;
        constexpr std::size_t kMixListenerBytes = 4 + 9 * 4 + 2;
        void WriteMixProbe(Writer& w, const SaveContext& c)
        {
            const mixprobe::Track& track = c.recording.mixProbe;
            w.Put(track.mainId);
            w.Put(track.dropped);
            w.Count(track.samples.size());
            for (const mixprobe::Sample& s : track.samples)
            {
                w.Put(s.timeMs);
                w.Put(s.owner);
                w.Put(s.source);
                w.Put(s.ref);
                w.Put(s.role);
                w.Put(s.kind);
                w.Put(s.lod);
                w.Put(s.flags);
                w.Put(s.dCamDm);
                w.Put(s.dPlayerDm);
                w.Put(s.dListenerDm);
            }
            w.Count(track.listeners.size());
            for (const mixprobe::Listener& l : track.listeners)
            {
                w.Put(l.timeMs);
                for (int n = 0; n < 3; ++n)
                    w.Put(l.cam[n]);
                for (int n = 0; n < 3; ++n)
                    w.Put(l.player[n]);
                for (int n = 0; n < 3; ++n)
                    w.Put(l.main[n]);
                w.Put(l.hasMain);
                w.Put(l.music);
            }
        }
        void ReadMixProbe(Reader& r, LoadContext& c)
        {
            mixprobe::Track& track = c.recording.mixProbe;
            track.Clear();
            track.mainId = r.Get<std::uint32_t>();
            track.dropped = r.Get<std::uint32_t>();
            const std::size_t samples = r.Count(kMixSampleBytes);
            track.samples.reserve(samples);
            for (std::size_t n = 0; n < samples && r.Ok(); ++n)
            {
                mixprobe::Sample s;
                s.timeMs = r.Get<std::uint32_t>();
                s.owner = r.Get<std::uint32_t>();
                s.source = r.Get<std::uint32_t>();
                s.ref = r.Get<std::uint64_t>();
                s.role = r.Get<std::uint8_t>();
                s.kind = r.Get<std::uint8_t>();
                s.lod = r.Get<std::uint8_t>();
                s.flags = r.Get<std::uint8_t>();
                s.dCamDm = r.Get<std::uint16_t>();
                s.dPlayerDm = r.Get<std::uint16_t>();
                s.dListenerDm = r.Get<std::uint16_t>();
                if (s.role >= mixprobe::kRoleCount || s.kind >= mixprobe::kKindCount)
                    r.Fail();
                track.samples.push_back(s);
            }
            const std::size_t listeners = r.Count(kMixListenerBytes);
            track.listeners.reserve(listeners);
            for (std::size_t n = 0; n < listeners && r.Ok(); ++n)
            {
                mixprobe::Listener l;
                l.timeMs = r.Get<std::uint32_t>();
                for (int k = 0; k < 3; ++k)
                    l.cam[k] = r.Get<float>();
                for (int k = 0; k < 3; ++k)
                    l.player[k] = r.Get<float>();
                for (int k = 0; k < 3; ++k)
                    l.main[k] = r.Get<float>();
                l.hasMain = r.Get<std::uint8_t>();
                l.music = r.Get<std::uint8_t>();
                track.listeners.push_back(l);
            }
        }
        bool OmitMixProbe(const SaveContext& c) { return c.recording.mixProbe.Empty(); }

        // Section "render targets" (RTGT, render_target_core.h): the named render targets
        // (register with its flag and answer, link of a model filed with the
        // target's name, release) and the UI apps open (with entry) and
        // closed, in time order; initial = the state at the recording's start.
        // Optional and written only when there is something.
        constexpr std::uint16_t kRenderTargetsVersion = 1;
        constexpr std::size_t kRenderTargetEventBytes = 4 + 4 + 4 + 4 + rendertarget::kNameBytes;
        void WriteRenderTargets(Writer& w, const SaveContext& c)
        {
            const rendertarget::Track& track = c.recording.renderTargets;
            w.Put(track.dropped);
            w.Count(track.events.size());
            for (const rendertarget::Event& e : track.events)
            {
                w.Put(e.timeMs);
                w.Put(e.kind);
                w.Put(e.flag);
                w.Put(e.accepted);
                w.Put(e.initial);
                w.Put(e.hash);
                w.Put(e.entry);
                w.Raw(reinterpret_cast<const std::uint8_t*>(e.name), rendertarget::kNameBytes);
            }
        }
        void ReadRenderTargets(Reader& r, LoadContext& c)
        {
            rendertarget::Track& track = c.recording.renderTargets;
            track.Clear();
            track.dropped = r.Get<std::uint32_t>();
            const std::size_t count = r.Count(kRenderTargetEventBytes);
            track.events.reserve(count);
            std::uint32_t lastMs = 0;
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                rendertarget::Event e;
                e.timeMs = r.Get<std::uint32_t>();
                e.kind = r.Get<std::uint8_t>();
                e.flag = r.Get<std::uint8_t>();
                e.accepted = r.Get<std::uint8_t>();
                e.initial = r.Get<std::uint8_t>();
                e.hash = r.Get<std::uint32_t>();
                e.entry = r.Get<std::uint32_t>();
                r.Raw(reinterpret_cast<std::uint8_t*>(e.name), rendertarget::kNameBytes);
                if (e.kind >= rendertarget::kKindCount || e.flag > 1 || e.accepted > 1 || e.initial > 1 || e.timeMs < lastMs ||
                    !e.NameValid())
                    r.Fail();
                lastMs = e.timeMs;
                track.events.push_back(e);
            }
        }
        bool OmitRenderTargets(const SaveContext& c) { return c.recording.renderTargets.Empty(); }
        // Section "game listener" (LSTN, game_listener_core.h): the live
        // engine volume listener position
        // (point 0 of the current listener description) about 30 times a
        // second, the number of volume points and whether the engine's own
        // override was on. ReplayGameListener=1 puts it back while the replay
        // renders the recorded game camera. Optional: omitted when empty.
        // v2: after the v1 samples, one record per sample with the microphone
        // hash, where it came from and up to 4 volume points (position, slot
        // +0x50 weight, slot +0x70). Saved as v2 only when a sample has v2
        // data (as MIXS v3); v1 still reads (no microphone).
        constexpr std::uint16_t kGameListenerVersion = 2;
        constexpr std::uint16_t kGameListenerV1Version = 1;
        constexpr std::size_t kGameListenerSampleBytes = 4 + 3 * 4 + 1 + 1;
        constexpr std::size_t kGameListenerV2MinBytes = 4 + 1 + 1;
        void WriteGameListener(Writer& w, const SaveContext& c)
        {
            const gamelistener::Track& track = c.recording.gameListener;
            w.Count(track.samples.size());
            for (const gamelistener::Sample& s : track.samples)
            {
                w.Put(s.timeMs);
                for (int n = 0; n < 3; ++n)
                    w.Put(s.pos[n]);
                w.Put(s.points);
                w.Put(s.flags);
            }
            if (!track.AnyV2())
                return;
            w.Count(track.samples.size());
            for (const gamelistener::Sample& s : track.samples)
            {
                w.Put(s.micHash);
                w.Put(static_cast<std::uint8_t>(s.micSource));
                const std::uint8_t stored = std::min<std::uint8_t>(s.stored, gamelistener::kMaxRecordedPoints);
                w.Put(stored);
                for (std::uint8_t j = 0; j < stored; ++j)
                {
                    for (int n = 0; n < 3; ++n)
                        w.Put(s.point[j].pos[n]);
                    w.Put(s.point[j].weight);
                    w.Put(s.point[j].f70);
                }
            }
        }
        std::uint16_t GameListenerSaveVersion(const SaveContext& c)
        {
            return c.recording.gameListener.AnyV2() ? kGameListenerVersion : kGameListenerV1Version;
        }
        void ReadGameListener(Reader& r, LoadContext& c)
        {
            gamelistener::Track& track = c.recording.gameListener;
            track.Clear();
            const std::size_t samples = r.Count(kGameListenerSampleBytes);
            track.samples.reserve(samples);
            std::uint32_t last = 0;
            for (std::size_t n = 0; n < samples && r.Ok(); ++n)
            {
                gamelistener::Sample s;
                s.timeMs = r.Get<std::uint32_t>();
                for (int k = 0; k < 3; ++k)
                    s.pos[k] = r.Get<float>();
                s.points = r.Get<std::uint8_t>();
                s.flags = r.Get<std::uint8_t>();
                if ((n && s.timeMs < last) || !std::isfinite(s.pos[0]) || !std::isfinite(s.pos[1]) || !std::isfinite(s.pos[2]))
                    r.Fail();
                last = s.timeMs;
                track.samples.push_back(s);
            }
            if (c.sectionVersion < 2 || !r.Ok())
                return;
            const std::size_t v2 = r.Count(kGameListenerV2MinBytes);
            if (v2 != track.samples.size())
            {
                r.Fail();
                return;
            }
            for (gamelistener::Sample& s : track.samples)
            {
                if (!r.Ok())
                    break;
                s.micHash = r.Get<std::uint32_t>();
                s.micSource = r.Enum<gamelistener::MicSource>(static_cast<std::uint8_t>(gamelistener::MicSource::Default));
                s.stored = r.Get<std::uint8_t>();
                if (s.stored > gamelistener::kMaxRecordedPoints)
                {
                    r.Fail();
                    break;
                }
                for (std::uint8_t j = 0; j < s.stored; ++j)
                {
                    for (int k = 0; k < 3; ++k)
                        s.point[j].pos[k] = r.Get<float>();
                    s.point[j].weight = r.Get<float>();
                    s.point[j].f70 = r.Get<float>();
                    if (!std::isfinite(s.point[j].pos[0]) || !std::isfinite(s.point[j].pos[1]) ||
                        !std::isfinite(s.point[j].pos[2]))
                        r.Fail();
                }
            }
        }
        bool OmitGameListener(const SaveContext& c) { return c.recording.gameListener.Empty(); }
        // Section "wind audio" (WNDA, wind_replay_core.h): the
        // weather audio's smoothed wind strength (W+0x4F44), the weather of
        // each wind row (W+0x5094/W+0x5098 through the weather table) and
        // whether the simple path ran, 4 times a second. Evidence only: the
        // playback logs the recorded value against the engine's (nothing is
        // applied from it). Optional: omitted when empty; a clip without it
        // plays with rec=none.
        constexpr std::uint16_t kWindAudioVersion = 1;
        constexpr std::size_t kWindAudioSampleBytes = 4 + 4 + 4 + 4 + 1;
        void WriteWindAudio(Writer& w, const SaveContext& c)
        {
            const windreplay::Track& track = c.recording.windAudio;
            w.Count(track.samples.size());
            for (const windreplay::Sample& s : track.samples)
            {
                w.Put(s.timeMs);
                w.Put(s.strength);
                w.Put(s.lines[0]);
                w.Put(s.lines[1]);
                w.Put(s.flags);
            }
        }
        void ReadWindAudio(Reader& r, LoadContext& c)
        {
            windreplay::Track& track = c.recording.windAudio;
            track.Clear();
            const std::size_t samples = r.Count(kWindAudioSampleBytes);
            track.samples.reserve(samples);
            std::uint32_t last = 0;
            for (std::size_t n = 0; n < samples && r.Ok(); ++n)
            {
                windreplay::Sample s;
                s.timeMs = r.Get<std::uint32_t>();
                s.strength = r.Get<float>();
                s.lines[0] = r.Get<std::uint32_t>();
                s.lines[1] = r.Get<std::uint32_t>();
                s.flags = r.Get<std::uint8_t>();
                if ((n && s.timeMs < last) || !std::isfinite(s.strength) || (s.flags & ~windreplay::kSampleFlagsKnown) != 0)
                    r.Fail();
                last = s.timeMs;
                track.samples.push_back(s);
            }
        }
        bool OmitWindAudio(const SaveContext& c) { return c.recording.windAudio.Empty(); }
        // Section "mix scenes" (MIXS, mix_scene_core.h):
        // the mixer scenes of the recording (id, hash, the start's return
        // address, kind, start, stop or open, a conversation scene's
        // participant tags) and each recorded ped's audio mix group changes.
        // Optional and omitted when empty (MixSceneRecord=0 or nothing seen).
        // v2: the scene variables (time, instance id, name hash, value) after
        // the groups; v1 still reads.
        // v3: a trailing u32 of flags (kTrackPoolScanned: the recording read
        // the scene pool, so its direct scenes, the speech duck among them,
        // are complete); v1 and v2 still read (flags 0).
        // A track without flags (MixSceneRecordDirect=0, or a pool that could
        // not be read) is saved as v2, without the trailing field (a newer
        // section version refuses the whole clip in an older reader); only a
        // track with flags needs v3.
        constexpr std::uint16_t kMixScenesVersion = 3;
        constexpr std::uint16_t kMixScenesNoFlagsVersion = 2;
        constexpr std::size_t kMixSceneMinBytes = 4 + 4 + 4 + 1 + 4 + 4 + 1;
        constexpr std::size_t kMixGroupBytes = 4 + 4 + 4;
        constexpr std::size_t kMixVarBytes = 4 + 4 + 4 + 4;
        void WriteMixScenes(Writer& w, const SaveContext& c)
        {
            const mixscene::Track& track = c.recording.mixScenes;
            w.Put(track.lostStops);
            w.Count(track.instances.size());
            for (const mixscene::Instance& i : track.instances)
            {
                w.Put(i.id);
                w.Put(i.hash);
                w.Put(i.caller);
                w.Put(static_cast<std::uint8_t>(i.kind));
                w.Put(i.startMs);
                w.Put(i.stopMs);
                w.Put(i.tagCount);
                for (std::uint8_t n = 0; n < i.tagCount; ++n)
                    w.Put(i.tags[n]);
            }
            w.Count(track.groups.size());
            for (const mixscene::GroupSample& g : track.groups)
            {
                w.Put(g.timeMs);
                w.Put(g.entity);
                w.Put(g.group);
            }
            w.Count(track.vars.size());
            for (const mixscene::VarSample& v : track.vars)
            {
                w.Put(v.timeMs);
                w.Put(v.id);
                w.Put(v.name);
                w.Put(v.value);
            }
            if (track.flags) // v3 (MixScenesSaveVersion)
                w.Put(track.flags);
        }
        std::uint16_t MixScenesSaveVersion(const SaveContext& c)
        {
            return c.recording.mixScenes.flags ? kMixScenesVersion : kMixScenesNoFlagsVersion;
        }
        void ReadMixScenes(Reader& r, LoadContext& c)
        {
            mixscene::Track& track = c.recording.mixScenes;
            track.Clear();
            track.lostStops = r.Get<std::uint32_t>();
            const std::size_t instances = r.Count(kMixSceneMinBytes);
            track.instances.reserve(instances);
            for (std::size_t n = 0; n < instances && r.Ok(); ++n)
            {
                mixscene::Instance i;
                i.id = r.Get<std::uint32_t>();
                i.hash = r.Get<std::uint32_t>();
                i.caller = r.Get<std::uint32_t>();
                i.kind = r.Enum<mixscene::Kind>(mixscene::kKindCount - 1);
                i.startMs = r.Get<std::uint32_t>();
                i.stopMs = r.Get<std::uint32_t>();
                i.tagCount = r.Get<std::uint8_t>();
                if (i.tagCount > mixscene::kMaxTags || (i.stopMs != mixscene::kOpen && i.stopMs < i.startMs))
                {
                    r.Fail();
                    return;
                }
                for (std::uint8_t t = 0; t < i.tagCount; ++t)
                    i.tags[t] = r.Get<std::uint32_t>();
                track.instances.push_back(i);
            }
            const std::size_t groups = r.Count(kMixGroupBytes);
            track.groups.reserve(groups);
            std::uint32_t lastMs = 0;
            for (std::size_t n = 0; n < groups && r.Ok(); ++n)
            {
                mixscene::GroupSample g;
                g.timeMs = r.Get<std::uint32_t>();
                g.entity = r.Get<std::uint32_t>();
                g.group = r.Get<std::uint32_t>();
                if (g.timeMs < lastMs)
                {
                    r.Fail();
                    return;
                }
                lastMs = g.timeMs;
                track.groups.push_back(g);
            }
            if (c.sectionVersion < 2)
                return; // v1: no variables
            const std::size_t vars = r.Count(kMixVarBytes);
            track.vars.reserve(vars);
            lastMs = 0;
            for (std::size_t n = 0; n < vars && r.Ok(); ++n)
            {
                mixscene::VarSample v;
                v.timeMs = r.Get<std::uint32_t>();
                v.id = r.Get<std::uint32_t>();
                v.name = r.Get<std::uint32_t>();
                v.value = r.Get<float>();
                if (v.timeMs < lastMs || !std::isfinite(v.value))
                {
                    r.Fail();
                    return;
                }
                lastMs = v.timeMs;
                track.vars.push_back(v);
            }
            if (c.sectionVersion < 3 || !r.Ok())
                return; // v2: no flags
            track.flags = r.Get<std::uint32_t>();
            if ((track.flags & ~mixscene::kTrackPoolScanned) != 0)
                r.Fail();
        }
        bool OmitMixScenes(const SaveContext& c) { return c.recording.mixScenes.Empty(); }

        // Section "category state" (CATS, category_state_core.h): every static sound category's controller values (hash, volume dB,
        // the two multipliers, pitch, low and high pass) on first sight and
        // on change, after the session's static and map counts. The playback
        // puts them back into the categories (CategoryStateReplay). Optional
        // and omitted when empty: a clip without it plays with the session's
        // categories.
        constexpr std::uint16_t kCategoryStateVersion = 1;
        void WriteCategoryState(Writer& w, const SaveContext& c)
        {
            const catstate::Track& track = c.recording.categoryState;
            w.Put(track.staticCount);
            w.Put(track.mapCount);
            w.Count(track.samples.size());
            for (const catstate::Sample& s : track.samples)
            {
                w.Put(s.timeMs);
                w.Put(s.hash);
                w.Put(s.v.volume);
                w.Put(s.v.rolloff);
                w.Put(s.v.x54);
                w.Put(s.v.pitch);
                w.Put(s.v.lpf);
                w.Put(s.v.hpf);
            }
        }
        void ReadCategoryState(Reader& r, LoadContext& c)
        {
            catstate::Track& track = c.recording.categoryState;
            track.Clear();
            track.staticCount = r.Get<std::uint32_t>();
            track.mapCount = r.Get<std::uint32_t>();
            const std::size_t count = r.Count(catstate::kSampleBytes);
            if (count > catstate::kMaxSamples)
            {
                r.Fail();
                return;
            }
            track.samples.resize(count);
            for (catstate::Sample& s : track.samples)
            {
                s.timeMs = r.Get<std::uint32_t>();
                s.hash = r.Get<std::uint32_t>();
                s.v.volume = r.Get<float>();
                s.v.rolloff = r.Get<float>();
                s.v.x54 = r.Get<float>();
                s.v.pitch = r.Get<std::int16_t>();
                s.v.lpf = r.Get<std::uint16_t>();
                s.v.hpf = r.Get<std::uint16_t>();
            }
            if (r.Ok() && !track.Valid())
                r.Fail();
        }
        bool OmitCategoryState(const SaveContext& c) { return c.recording.categoryState.Empty(); }

        // Section "entity queries" (EQRY, entity_query_track.h): the engine's answers to the
        // owner queries of the dynamic entity sounds of the recorded owners.
        // The chains once (hash, length, names), then the changes of the
        // answer per owner and chain in time order. Optional.
        constexpr std::uint16_t kEntityQueriesVersion = 1;
        void WriteEntityQueries(Writer& w, const SaveContext& c)
        {
            const entityquery::Track& track = c.recording.entityQueries;
            w.Count(track.chains.size());
            for (const entityquery::Chain& chain : track.chains)
            {
                w.Put(chain.hash);
                w.Put(chain.n);
                for (std::uint8_t n = 0; n < chain.n; ++n)
                    w.Put(chain.refs[n]);
            }
            w.Count(track.events.size());
            for (const entityquery::AnswerEvent& e : track.events)
            {
                w.Put(e.timeMs);
                w.Put(e.owner);
                w.Put(e.chain);
                w.Put(e.answer);
            }
        }
        void ReadEntityQueries(Reader& r, LoadContext& c)
        {
            entityquery::Track& track = c.recording.entityQueries;
            track.Clear();
            track.chains.resize(r.Count(entityquery::kChainBytesMin));
            if (track.chains.size() > entityquery::kMaxChains)
                r.Fail();
            for (entityquery::Chain& chain : track.chains)
            {
                if (!r.Ok())
                    return;
                chain.hash = r.Get<std::uint32_t>();
                chain.n = r.Get<std::uint8_t>();
                if (chain.n == 0 || chain.n > entityquery::kMaxChain)
                {
                    r.Fail();
                    return;
                }
                for (std::uint8_t n = 0; n < chain.n; ++n)
                    chain.refs[n] = r.Get<std::uint32_t>();
                if (chain.hash != entityquery::ChainHash(chain.n, chain.refs))
                    r.Fail();
            }
            if (!r.Ok())
                return;
            track.events.resize(r.Count(entityquery::kAnswerEventBytes));
            std::uint32_t lastMs = 0;
            for (entityquery::AnswerEvent& e : track.events)
            {
                e.timeMs = r.Get<std::uint32_t>();
                e.owner = r.Get<std::uint32_t>();
                e.chain = r.Get<std::uint16_t>();
                e.answer = r.Get<std::uint64_t>();
                if (e.chain >= track.chains.size() || e.timeMs < lastMs)
                    r.Fail();
                lastMs = e.timeMs;
                if (!r.Ok())
                    return;
            }
            track.Reindex();
        }

        // Section "foley variants" (FVAR, ped_audio_state.h): the two variant indices the
        // engine drew once for each recorded owner's ped audio (the local
        // player's 0 and 0 included), sorted by owner. Optional: without it
        // only the recorded player's puppet is given the player's indices.
        constexpr std::uint16_t kFoleyVariantsVersion = 1;
        void WriteFoleyVariants(Writer& w, const SaveContext& c)
        {
            const pedaudiostate::VariantTrack& track = c.recording.foleyVariants;
            w.Count(track.rows.size());
            for (const pedaudiostate::VariantRow& row : track.rows)
            {
                w.Put(row.owner);
                w.Put(row.a);
                w.Put(row.b);
                w.Put(row.player);
            }
        }
        void ReadFoleyVariants(Reader& r, LoadContext& c)
        {
            pedaudiostate::VariantTrack& track = c.recording.foleyVariants;
            track.Clear();
            track.rows.resize(r.Count(pedaudiostate::kVariantRowBytes));
            for (pedaudiostate::VariantRow& row : track.rows)
            {
                row.owner = r.Get<std::uint32_t>();
                row.a = r.Get<std::int32_t>();
                row.b = r.Get<std::int32_t>();
                row.player = r.Get<std::uint8_t>();
                if (!r.Ok())
                    return;
            }
            if (!track.Valid())
                r.Fail();
        }

        // Section "ped speed" (PSPD, ped_speed_core.h): each
        // recorded ped's and horse's PED_VARIABLES speed (the variable the
        // sounds of that ped read by name), 4 times a second, by owner. The
        // playback writes it into the puppet's block (PedSpeedReplay).
        // Optional and omitted when empty: a clip without it plays with the
        // speed of the puppets' recorded motion.
        constexpr std::uint16_t kPedSpeedVersion = 1;
        void WritePedSpeed(Writer& w, const SaveContext& c)
        {
            const pedspeed::Track& track = c.recording.pedSpeed;
            w.Count(track.series.size());
            for (const pedspeed::Series& s : track.series)
            {
                w.Put(s.owner);
                w.Count(s.points.size());
                for (const pedspeed::Point& p : s.points)
                {
                    w.Put(p.timeMs);
                    w.Put(p.speed);
                }
            }
        }
        void ReadPedSpeed(Reader& r, LoadContext& c)
        {
            pedspeed::Track& track = c.recording.pedSpeed;
            track.Clear();
            const std::size_t owners = r.Count(pedspeed::kSeriesHeaderBytes);
            if (owners > pedspeed::kMaxOwners)
            {
                r.Fail();
                return;
            }
            track.series.reserve(owners);
            for (std::size_t n = 0; n < owners && r.Ok(); ++n)
            {
                pedspeed::Series s;
                s.owner = r.Get<std::uint32_t>();
                const std::size_t points = r.Count(pedspeed::kPointBytes);
                if (points > pedspeed::kMaxPoints)
                {
                    r.Fail();
                    return;
                }
                s.points.resize(points);
                for (pedspeed::Point& p : s.points)
                {
                    p.timeMs = r.Get<std::uint32_t>();
                    p.speed = r.Get<float>();
                }
                track.series.push_back(std::move(s));
            }
            if (r.Ok() && !track.Valid())
                r.Fail();
        }
        bool OmitPedSpeed(const SaveContext& c) { return c.recording.pedSpeed.Empty(); }

        // Section "shader vars" (SHVR, shader_vars_core.h): each recorded
        // horse's animated shader variables (the muscles under its skin: the
        // values of its creature's shader vars component, track 0x17 and the
        // others the component holds), about 30 times a second, by owner and
        // layout; the values as half floats. The playback writes them into the
        // puppet's component before the shader reads them (MuscleReplay).
        // Optional and omitted when empty: a clip without it plays the
        // puppets' own values (still muscles).
        constexpr std::uint16_t kShaderVarsVersion = 1;
        void WriteShaderVars(Writer& w, const SaveContext& c)
        {
            const shadervars::Track& track = c.recording.shaderVars;
            w.Count(track.series.size());
            for (const shadervars::Series& s : track.series)
            {
                w.Put(s.owner);
                w.Count(s.blocks.size());
                for (const shadervars::Block& b : s.blocks)
                {
                    w.Count(b.dofs.size());
                    for (const shadervars::Dof& dof : b.dofs)
                    {
                        w.Put(dof.track);
                        w.Put(dof.type);
                        w.Put(dof.id);
                    }
                    w.Count(b.times.size());
                    for (const std::uint32_t t : b.times)
                        w.Put(t);
                    for (const std::uint16_t v : b.values)
                        w.Put(v);
                }
            }
        }
        void ReadShaderVars(Reader& r, LoadContext& c)
        {
            shadervars::Track& track = c.recording.shaderVars;
            track.Clear();
            const std::size_t owners = r.Count(4 + 8);
            if (owners > shadervars::kMaxOwners)
            {
                r.Fail();
                return;
            }
            track.series.reserve(owners);
            for (std::size_t n = 0; n < owners && r.Ok(); ++n)
            {
                shadervars::Series s;
                s.owner = r.Get<std::uint32_t>();
                const std::size_t blocks = r.Count(8 + 4 + 8);
                if (blocks > shadervars::kMaxBlocks)
                {
                    r.Fail();
                    return;
                }
                for (std::size_t bi = 0; bi < blocks && r.Ok(); ++bi)
                {
                    shadervars::Block b;
                    const std::size_t dofs = r.Count(4);
                    if (dofs > shadervars::kMaxDofs)
                    {
                        r.Fail();
                        return;
                    }
                    b.dofs.resize(dofs);
                    for (shadervars::Dof& dof : b.dofs)
                    {
                        dof.track = r.Get<std::uint8_t>();
                        dof.type = r.Get<std::uint8_t>();
                        dof.id = r.Get<std::uint16_t>();
                    }
                    b.lanes = shadervars::LanesOf(b.dofs);
                    const std::size_t points = r.Count(4 + 2 * static_cast<std::size_t>(b.lanes ? b.lanes : 1));
                    if (points > shadervars::kMaxPoints || b.lanes > shadervars::kMaxLanes)
                    {
                        r.Fail();
                        return;
                    }
                    b.times.resize(points);
                    for (std::uint32_t& t : b.times)
                        t = r.Get<std::uint32_t>();
                    b.values.resize(points * b.lanes);
                    for (std::uint16_t& v : b.values)
                        v = r.Get<std::uint16_t>();
                    s.blocks.push_back(std::move(b));
                }
                track.series.push_back(std::move(s));
            }
            if (r.Ok() && !track.Valid())
                r.Fail();
        }
        bool OmitShaderVars(const SaveContext& c) { return c.recording.shaderVars.Empty(); }

        // Section "foot events" (FSEV, foot_event_core.h): the secondary steps of each recorded ped and horse (the entries
        // the engine put in the foot audio's second queue: type, foot, origin,
        // the foot slot's container and the foot's rise), queued again on the
        // puppet at the same clip time (FootEventReplay). Optional and omitted
        // when empty (FootEventRecord=0 records none): a clip without it keeps
        // the puppets' own secondary steps.
        constexpr std::uint16_t kFootEventsVersion = 1;
        void WriteFootEvents(Writer& w, const SaveContext& c)
        {
            const footevent::Track& track = c.recording.footEvents;
            w.Count(track.events.size());
            for (const footevent::Event& e : track.events)
            {
                w.Put(e.owner);
                w.Put(e.timeMs);
                w.Put(e.type);
                w.Put(e.foot);
                w.Put(e.origin);
                w.Put(e.flags);
                w.Put(e.materialHash);
                w.Put(e.materialRef);
                w.Put(e.impact);
            }
        }
        void ReadFootEvents(Reader& r, LoadContext& c)
        {
            footevent::Track& track = c.recording.footEvents;
            track.Clear();
            const std::size_t count = r.Count(footevent::kEventBytes);
            if (count > footevent::kMaxEvents)
            {
                r.Fail();
                return;
            }
            track.events.resize(count);
            for (footevent::Event& e : track.events)
            {
                e.owner = r.Get<std::uint32_t>();
                e.timeMs = r.Get<std::uint32_t>();
                e.type = r.Get<std::int8_t>();
                e.foot = r.Get<std::uint8_t>();
                e.origin = r.Get<std::uint8_t>();
                e.flags = r.Get<std::uint8_t>();
                e.materialHash = r.Get<std::uint32_t>();
                e.materialRef = r.Get<std::uint32_t>();
                e.impact = r.Get<float>();
            }
            if (r.Ok() && !track.Valid())
                r.Fail();
        }
        bool OmitFootEvents(const SaveContext& c) { return c.recording.footEvents.Empty(); }
        // Section "foot movement" (FMOV, foot_movement_core.h): the movement volume result of each
        // recorded ped's and horse's step (owner, time, foot, result), given
        // back to the puppet's step (FootMovementReplay). Optional and omitted
        // when empty (FootMovementRecord=0 records none).
        constexpr std::uint16_t kFootMovementVersion = 1;
        void WriteFootMovement(Writer& w, const SaveContext& c)
        {
            const footmovement::Track& track = c.recording.footMovement;
            w.Count(track.samples.size());
            for (const footmovement::Sample& s : track.samples)
            {
                w.Put(s.owner);
                w.Put(s.timeMs);
                w.Put(s.foot);
                w.Put(s.result);
            }
        }
        void ReadFootMovement(Reader& r, LoadContext& c)
        {
            footmovement::Track& track = c.recording.footMovement;
            track.Clear();
            const std::size_t count = r.Count(footmovement::kSampleBytes);
            if (count > footmovement::kMaxSamples)
            {
                r.Fail();
                return;
            }
            track.samples.resize(count);
            for (footmovement::Sample& s : track.samples)
            {
                s.owner = r.Get<std::uint32_t>();
                s.timeMs = r.Get<std::uint32_t>();
                s.foot = r.Get<std::uint8_t>();
                s.result = r.Get<float>();
            }
            if (r.Ok() && !track.Valid())
                r.Fail();
        }
        bool OmitFootMovement(const SaveContext& c) { return c.recording.footMovement.Empty(); }
        // Section "weapon fx intents" (WFXI, weapon_fx_intent_core.h): the weapon
        // system's requests for each shot's muzzle flash and shell (shooter
        // and weapon ids, bone, info hashes and range, the vector argument).
        // Optional and omitted when empty (WeaponFxIntent=0 records none).
        constexpr std::uint16_t kWeaponFxIntentsVersion = 1;
        void WriteWeaponFxIntents(Writer& w, const SaveContext& c)
        {
            const weaponfx::Track& track = c.recording.weaponFxIntents;
            w.Count(track.intents.size());
            for (const weaponfx::Intent& i : track.intents)
            {
                w.Put(i.timeMs);
                w.Put(i.shooter);
                w.Put(i.weapon);
                w.Put(i.kind);
                w.Put(i.bone);
                w.Put(i.argA);
                w.Put(i.fxHash);
                w.Put(i.fxHashAlt);
                w.Put(i.range);
                w.Array(i.vec);
            }
        }
        void ReadWeaponFxIntents(Reader& r, LoadContext& c)
        {
            weaponfx::Track& track = c.recording.weaponFxIntents;
            track.Clear();
            const std::size_t count = r.Count(weaponfx::kIntentBytes);
            if (count > weaponfx::kMaxIntents)
            {
                r.Fail();
                return;
            }
            track.intents.resize(count);
            for (weaponfx::Intent& i : track.intents)
            {
                i.timeMs = r.Get<std::uint32_t>();
                i.shooter = r.Get<std::uint32_t>();
                i.weapon = r.Get<std::uint32_t>();
                i.kind = r.Get<std::uint8_t>();
                i.bone = r.Get<std::int32_t>();
                i.argA = r.Get<std::int32_t>();
                i.fxHash = r.Get<std::uint32_t>();
                i.fxHashAlt = r.Get<std::uint32_t>();
                i.range = r.Get<float>();
                r.Array(i.vec);
            }
            if (r.Ok() && !track.Valid())
                r.Fail();
        }
        bool OmitWeaponFxIntents(const SaveContext& c) { return c.recording.weaponFxIntents.Empty(); }

        // The fx settings. FXSS: per one-shot event of the fx section
        // (by its index there; FXEV comes first in the file) the instance
        // fields the engine set before the start (entity::FxSettings). RGSS:
        // per registered track (index into RGFX) the settings changes and the
        // stretches without a call (callGaps). Both optional and omitted when
        // empty: a clip without them plays with the engine's settings and
        // gaps derived from the frame times.
        constexpr std::uint16_t kFxSettingsVersion = 1;
        constexpr std::uint16_t kRegisteredFxSettingsVersion = 1;
        constexpr std::size_t kFxSettingsBytes = 4 + 11 * 4 + 2 + 4;
        constexpr std::size_t kMaxSettingsChanges = 4096;
        constexpr std::size_t kMaxCallGaps = 65536;
        void PutFxSettings(Writer& w, const entity::FxSettings& s)
        {
            w.Put(s.mask);
            w.Array(s.vel350);
            w.Array(s.tint380);
            w.Put(s.alpha38C);
            w.Put(s.aux39C);
            w.Put(s.lod3A0);
            w.Put(s.tele3A8);
            w.Put(s.cull3BC);
            w.Put(s.ev5CC);
        }
        void GetFxSettings(Reader& r, entity::FxSettings& s)
        {
            s.mask = r.Get<std::uint32_t>();
            if (s.mask & ~entity::FxSettings::kAll)
                r.Fail();
            r.Array(s.vel350);
            r.Array(s.tint380);
            s.alpha38C = r.Get<float>();
            s.aux39C = r.Get<float>();
            s.lod3A0 = r.Get<float>();
            s.tele3A8 = r.Get<float>();
            s.cull3BC = r.Get<std::uint16_t>();
            s.ev5CC = r.Get<std::uint32_t>();
            entity::FxSettingsKeepFinite(s); // no NaN or infinity into an instance
        }
        void WriteFxSettings(Writer& w, const SaveContext& c)
        {
            const std::vector<entity::FxEvent>& events = c.recording.fx.Events();
            std::size_t with = 0;
            for (const entity::FxEvent& event : events)
                with += event.hasSettings ? 1 : 0;
            w.Count(with);
            for (std::size_t index = 0; index < events.size(); ++index)
            {
                if (!events[index].hasSettings)
                    continue;
                w.Put(static_cast<std::uint32_t>(index));
                PutFxSettings(w, events[index].settings);
            }
        }
        void ReadFxSettings(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(4 + kFxSettingsBytes);
            const std::size_t events = c.recording.fx.Events().size();
            std::uint32_t lastIndex = 0;
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                const std::uint32_t index = r.Get<std::uint32_t>();
                // Each event once, in order, one the fx section has.
                if (index >= events || (n > 0 && index <= lastIndex))
                {
                    r.Fail();
                    return;
                }
                lastIndex = index;
                entity::FxEvent& event = c.recording.fx.At(index);
                GetFxSettings(r, event.settings);
                event.hasSettings = r.Ok();
            }
        }
        void ResetFxSettings(LoadContext& c)
        {
            for (std::size_t index = 0; index < c.recording.fx.Events().size(); ++index)
            {
                c.recording.fx.At(index).hasSettings = false;
                c.recording.fx.At(index).settings = {};
            }
        }
        bool OmitFxSettings(const SaveContext& c)
        {
            for (const entity::FxEvent& event : c.recording.fx.Events())
                if (event.hasSettings)
                    return false;
            return true;
        }
        void WriteRegisteredFxSettings(Writer& w, const SaveContext& c)
        {
            const std::vector<entity::RegisteredFxTrack>& tracks = c.recording.registeredFx;
            std::size_t with = 0;
            for (const entity::RegisteredFxTrack& track : tracks)
                with += (track.settings.empty() && track.callGaps.empty()) ? 0 : 1;
            w.Count(with);
            for (std::size_t index = 0; index < tracks.size(); ++index)
            {
                const entity::RegisteredFxTrack& track = tracks[index];
                if (track.settings.empty() && track.callGaps.empty())
                    continue;
                w.Put(static_cast<std::uint32_t>(index));
                w.Count(track.settings.size());
                for (const entity::RegisteredFxSettingsChange& change : track.settings)
                {
                    w.Put(change.timeMs);
                    PutFxSettings(w, change.settings);
                }
                w.Count(track.callGaps.size());
                for (const entity::RegisteredFxGap& gap : track.callGaps)
                {
                    w.Put(gap.fromMs);
                    w.Put(gap.toMs);
                }
            }
        }
        void ReadRegisteredFxSettings(Reader& r, LoadContext& c)
        {
            std::vector<entity::RegisteredFxTrack>& tracks = c.recording.registeredFx;
            const std::size_t count = r.Count(4 + 8 + 8);
            std::uint32_t lastIndex = 0;
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                const std::uint32_t index = r.Get<std::uint32_t>();
                if (index >= tracks.size() || (n > 0 && index <= lastIndex) || !tracks[index].settings.empty() ||
                    !tracks[index].callGaps.empty())
                {
                    r.Fail();
                    return;
                }
                lastIndex = index;
                entity::RegisteredFxTrack& track = tracks[index];
                const std::size_t changes = r.Count(4 + kFxSettingsBytes);
                if (changes > kMaxSettingsChanges)
                {
                    r.Fail();
                    return;
                }
                track.settings.resize(changes);
                std::uint32_t lastMs = 0;
                for (entity::RegisteredFxSettingsChange& change : track.settings)
                {
                    change.timeMs = r.Get<std::uint32_t>();
                    GetFxSettings(r, change.settings);
                    if (change.timeMs < lastMs)
                        r.Fail();
                    lastMs = change.timeMs;
                }
                const std::size_t gaps = r.Count(8);
                if (gaps > kMaxCallGaps)
                {
                    r.Fail();
                    return;
                }
                track.callGaps.resize(gaps);
                std::uint32_t lastTo = 0;
                for (entity::RegisteredFxGap& gap : track.callGaps)
                {
                    gap.fromMs = r.Get<std::uint32_t>();
                    gap.toMs = r.Get<std::uint32_t>();
                    if (gap.toMs <= gap.fromMs || gap.fromMs < lastTo)
                        r.Fail();
                    lastTo = gap.toMs;
                }
            }
        }
        void ResetRegisteredFxSettings(LoadContext& c)
        {
            for (entity::RegisteredFxTrack& track : c.recording.registeredFx)
            {
                track.settings.clear();
                track.callGaps.clear();
            }
        }
        bool OmitRegisteredFxSettings(const SaveContext& c)
        {
            for (const entity::RegisteredFxTrack& track : c.recording.registeredFx)
                if (!track.settings.empty() || !track.callGaps.empty())
                    return false;
            return true;
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
            {Tag("FXEV"), kFxVersion, "fx", WriteFxTrack, ReadFxTrack},
            {Tag("RGFX"), kRegisteredFxVersion, "registeredFx", WriteRegisteredFx, ReadRegisteredFx},
            {Tag("DECL"), kDecalsVersion, "decals", WriteDecals, ReadDecals, false, kDecalsMinVersion},
            {Tag("DCRM"), kDecalRemovesVersion, "decalRemoves", WriteDecalRemoves, ReadDecalRemoves, false,
                kDecalRemovesMinVersion},
            {Tag("PDCL"), kProjDecalsVersion, "projDecals", WriteProjDecals, ReadProjDecals},
            {Tag("GORE"), kGoreVersion, "gore", WriteGoreTrack, ReadGoreTrack},
            {Tag("GHIT"), kGoreHitsVersion, "goreHits", WriteGoreHits, ReadGoreHits},
            {Tag("TSCL"), kTimeScaleVersion, "timeScale", WriteTimeScale, ReadTimeScale},
            {Tag("BLOD"), kBloodVersion, "blood", WriteBlood, ReadBlood},
            {Tag("FIRS"), kFireStartsVersion, "fireStarts", WriteFireStarts, ReadFireStarts, false, 1},
            {Tag("FSTP"), kFireStopsVersion, "fireStops", WriteFireStops, ReadFireStops},
            {Tag("FITM"), kFireItemsVersion, "fireItems", WriteFireItems, ReadFireItems},
            {Tag("META"), kMetaVersion, "meta", WriteMeta, ReadMeta},
            {Tag("AREA"), kAreaVersion, "area", WriteArea, ReadArea},
            {Tag("MELE"), kMeleeHitsVersion, "meleeHits", WriteMeleeHits, ReadMeleeHits},
            {Tag("BIMP"), kBodyImpactsVersion, "bodyImpacts", WriteBodyImpacts, ReadBodyImpacts},
            {Tag("SNDS"), kSoundsVersion, "sounds", WriteSounds, ReadSounds},
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
            {Tag("GSND"), kGeneralSoundsVersion, "general sounds", WriteGeneralSounds, ReadGeneralSounds, true, 0, ResetMember<&Recording::generalSounds>},
            {Tag("SGEN"), kSoundGenericVersion, "sound generic", WriteSoundGeneric, ReadSoundGeneric, true, kSoundGenericMinVersion, ResetMember<&Recording::soundGeneric>}, // v2 the audio base, v3 init variables, v4 value dwords
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
