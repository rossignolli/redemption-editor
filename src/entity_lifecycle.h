#pragma once

// Generic entity lifecycle, following the Rockstar Editor model.
//
// Every recorded entity gets a ReplayId (slot + instance). The slot is reused
// once the entity is gone; the instance comes from a global counter, so a
// reused slot never aliases an earlier entity. The recording carries create
// and delete packets, and every block (kBlockMs) starts with a full create of
// every live entity, so any block can be read on its own: which entities
// exist at a given time is answered from that block alone, without scanning
// the clip from the start. A delete carries a copy of the entity's create, so
// rewinding over a delete can recreate the entity.
//
// Pure data: no engine or native access, unit tested in ReplayCoreTests.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace replay::entity
{
    struct ReplayId
    {
        std::uint16_t slot = 0xFFFF;
        std::uint16_t instance = 0;

        bool Valid() const { return slot != 0xFFFF; }
        std::uint32_t Packed() const
        {
            return (static_cast<std::uint32_t>(slot) << 16) | instance;
        }
        static ReplayId FromPacked(std::uint32_t packed)
        {
            return {static_cast<std::uint16_t>(packed >> 16),
                static_cast<std::uint16_t>(packed & 0xFFFFu)};
        }
        bool operator==(const ReplayId& other) const
        {
            return slot == other.slot && instance == other.instance;
        }
        bool operator!=(const ReplayId& other) const { return !(*this == other); }
    };

    // "slot:instance", the id as the logs print it.
    inline std::string IdText(ReplayId id)
    {
        char text[16]{};
        std::snprintf(text, sizeof(text), "%u:%u",
            static_cast<unsigned int>(id.slot), static_cast<unsigned int>(id.instance));
        return text;
    }

    enum class EntityKind : std::uint8_t
    {
        Ped = 0,
        Horse = 1,
        Vehicle = 2,
        // Object without a recorded skeleton (a weapon), placed by its
        // transform in its attach parent's space.
        Weapon = 3,
        // Object that came off a recorded ped (a knocked off hat), placed
        // by its world transform.
        Prop = 4,
        // A car of a train (train_consist.h): no skeleton, placed
        // by its recorded world transform (a rigid track at the capture
        // rate), built as a vehicle puppet or, refused, as an object.
        TrainCar = 5,
    };

    inline const char* KindText(EntityKind kind)
    {
        switch (kind)
        {
        case EntityKind::Horse:
            return "horse";
        case EntityKind::Vehicle:
            return "vehicle";
        case EntityKind::Weapon:
            return "weapon";
        case EntityKind::Prop:
            return "prop";
        case EntityKind::TrainCar:
            return "train_car";
        default:
            return "ped";
        }
    }

    // What a prop entity was before it came off its owner; decides how
    // playback builds it.
    enum class PropOrigin : std::uint8_t
    {
        DroppedHat = 0,    // rebuilt by the owner puppet dropping its layer
        DroppedWeapon = 1, // rebuilt as the weapon object (weaponHash/model)
        GorePiece = 2,     // a severed limb: the engine's own piece adopted, or the model
        Projectile = 3,    // a free object a recorded ped shot (an arrow): its model
        WorldObject = 4,   // an object of the world that moved or broke (a crate, a fence): its model
        WorldPiece = 5,    // a new object of a WorldObject's model next to it (a broken-off piece)
        // A piece that broke off a recorded vehicle's fragment (a
        // wheel, a door, a board: an object of the vehicle's model holding
        // only its groups). Rebuilt by breaking the same group off the
        // vehicle puppet; owner is the vehicle, boneCount the group.
        VehiclePiece = 6,
    };

    // One metaped layer as the engine reports it: enough to rebuild the
    // same outfit on a new ped of the same model.
    struct MetaPedAsset
    {
        std::uint32_t drawable = 0;
        std::uint32_t albedo = 0;
        std::uint32_t normal = 0;
        std::uint32_t material = 0;
        std::uint32_t palette = 0;
        std::int32_t tint0 = 0;
        std::int32_t tint1 = 0;
        std::int32_t tint2 = 0;
    };

    // The create packet carries the model and its variation data, so
    // playback builds the entity without the recorded one.
    struct Appearance
    {
        std::uint32_t outfit = 0;
        std::vector<MetaPedAsset> assets;
        // Vehicles.
        std::int32_t vehicleTint = -1;
        std::int32_t vehicleLivery = -1;
    };

    struct CreatePacket
    {
        ReplayId id;
        std::uint32_t timeMs = 0; // when the entity entered the recording
        EntityKind kind = EntityKind::Ped;
        std::uint32_t modelHash = 0;
        // Session handle the entity was recorded from (logs only).
        std::int32_t sourceHandle = 0;
        // VehiclePiece props keep here the vehicle fragment group
        // the piece broke off at (its root; no bones are recorded for props).
        std::uint32_t boneCount = 0;
        // Weapon objects: the weapon the object belongs to, 0 for a plain
        // object of modelHash.
        std::uint32_t weaponHash = 0;
        // Weapon objects: the components the object had (straps, barrels,
        // finishes), which the weapon system does not put on a new object.
        std::vector<std::uint32_t> weaponComponents;
        // Props: the entity it came off, and what it was there.
        ReplayId owner;
        PropOrigin propOrigin = PropOrigin::DroppedHat;
        // Peds: their metaped layers. Props: the layers of the object
        // itself (a hat is a generic prop model dressed by its layer).
        Appearance appearance;
    };

    // Recorded attachment between two entities (attach/detach packets). A
    // child has at most one parent at a time.
    enum class AttachmentKind : std::uint8_t
    {
        DraftHarness = 1, // index = harness slot on the vehicle
        VehicleSeat = 2,  // index = seat (-1 driver)
        WeaponPoint = 3,  // index = weapon attach point on the ped
    };

    struct Attachment
    {
        ReplayId child;
        ReplayId parent;
        AttachmentKind kind = AttachmentKind::DraftHarness;
        std::int32_t index = 0;

        bool operator==(const Attachment& other) const
        {
            return child == other.child && parent == other.parent &&
                kind == other.kind && index == other.index;
        }
    };

    // Lowest free slot first; the instance counter is global and never 0.
    class IdAllocator
    {
    public:
        explicit IdAllocator(std::uint16_t maxSlots = 1024);
        void Reset();
        // Invalid id when every slot is in use.
        ReplayId Allocate();
        void Release(ReplayId id);
        std::uint32_t InUse() const { return m_inUse; }

    private:
        std::vector<std::uint16_t> m_slotInstance; // 0 = free
        std::uint16_t m_nextInstance = 1;
        std::uint32_t m_inUse = 0;
    };

    struct LifecycleStats
    {
        std::uint64_t blocks = 0;
        std::uint64_t creates = 0;
        std::uint64_t deletes = 0;
        std::uint64_t fullCreates = 0;
        std::uint64_t attaches = 0;
        std::uint64_t detaches = 0;
        std::uint64_t fullAttachments = 0;
    };

    class LifecycleTrack
    {
    public:
        static constexpr std::uint32_t kBlockMs = 2000;

        void Begin(std::uint32_t blockMs = kBlockMs);
        // Opens every block up to timeMs, each one starting with a full create
        // of the entities live at that moment. Times must not go backwards.
        void Advance(std::uint32_t timeMs);
        bool Create(const CreatePacket& packet);
        bool Delete(ReplayId id, std::uint32_t timeMs);

        // Entities alive at timeMs: the covering block's full creates plus
        // its creates/deletes up to timeMs. Empty before the first block.
        void AliveAt(std::uint32_t timeMs, std::vector<CreatePacket>& out) const;
        // What AliveAt(timeMs) depends on (the
        // covering block and how many of its events are at or before
        // timeMs). Two times with the same stamp have the same AliveAt, so
        // playback copies the packets (two vectors each) only when it moves.
        // kNoAliveStamp before the first block.
        static constexpr std::uint64_t kNoAliveStamp = ~0ull;
        std::uint64_t AliveStamp(std::uint32_t timeMs) const;
        const std::vector<CreatePacket>& Live() const { return m_live; }
        // The model of every entity alive at some time in
        // [fromMs, toMs] (the model window of the seek settle; weapons, which
        // stream as weapon assets, are left out). Duplicates removed.
        void ModelsBetween(std::uint32_t fromMs, std::uint32_t toMs, std::vector<std::uint32_t>& out) const;
        // The packed id of every entity the kept blocks name (their full
        // creates and events) or that is live now: sorted, each once. What
        // the buffer keeps per entity outside the lifecycle (the visibility
        // rows) goes with an id not in it.
        void KeptIds(std::vector<std::uint32_t>& out) const;

        // Attachments follow the same block rule: each block starts with the
        // full attachment state. Attach replaces the child's current
        // attachment; deleting an entity detaches everything it takes part in.
        bool Attach(const Attachment& attachment, std::uint32_t timeMs);
        bool Detach(ReplayId child, std::uint32_t timeMs);
        void AttachmentsAt(std::uint32_t timeMs, std::vector<Attachment>& out) const;
        const std::vector<Attachment>& LiveAttachments() const { return m_liveAttachments; }
        const LifecycleStats& Stats() const { return m_stats; }
        std::size_t BlockCount() const { return m_blocks.size(); }
        // Replay buffer: drops the blocks that end at or before
        // timeMs (the one holding timeMs stays: its full creates are the
        // state there). Each block repeats the full create of every live
        // entity (appearance included): an hour of 100 entities is ~185k
        // copies. Returns the blocks dropped.
        std::size_t DropBefore(std::uint32_t timeMs);
        // The start of the oldest block kept (0 when
        // there is none): the earliest base a window cut can still have.
        std::uint32_t FirstBlockMs() const { return m_blocks.empty() ? 0 : m_blocks.front().startMs; }
        // Rough bytes the blocks hold.
        std::size_t ApproxBytes() const;

    private:
        struct Event
        {
            bool create = false;
            std::uint32_t timeMs = 0;
            CreatePacket packet; // delete: copy of the entity's create
        };

        struct AttachEvent
        {
            bool attach = false;
            std::uint32_t timeMs = 0;
            Attachment attachment;
        };

        struct Block
        {
            std::uint32_t startMs = 0;
            std::vector<CreatePacket> fullCreates;
            std::vector<Event> events;
            std::vector<Attachment> fullAttachments;
            std::vector<AttachEvent> attachEvents;
        };

        const Block* BlockAt(std::uint32_t timeMs) const;

        std::uint32_t m_blockMs = kBlockMs;
        std::vector<CreatePacket> m_live;
        std::vector<Attachment> m_liveAttachments;
        std::deque<Block> m_blocks;
        LifecycleStats m_stats;
    };
}
