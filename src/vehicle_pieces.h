#pragma once

// Pieces that break off a recorded vehicle's fragment (a wheel, a door, a
// board, what an explosion or a fire knocks off). The engine makes each one an
// object of the vehicle's model that keeps only the groups that came off; it
// is recorded as an entity of its own (object create with its frag parent,
// transform per frame) and the world's originals are hidden while it plays.
// Here a piece is a prop with PropOrigin::VehiclePiece (owner: the vehicle,
// boneCount: the group), rebuilt by breaking the same group off the vehicle
// puppet. Pure rules shared by capture and playback
// (tests/vehicle_pieces_tests.cpp).

#include "entity_lifecycle.h"

#include <cstdint>
#include <vector>

namespace replay::vehicle_pieces
{
    // A prop of a vehicle's model is a piece of a vehicle: it is only ever a
    // VehiclePiece. Any other origin (a projectile's "successor", a world piece)
    // would make CREATE_OBJECT build the whole vehicle as an object in playback (a
    // duplicate wagon after the dynamite).
    inline bool PropModelAllowed(entity::PropOrigin origin, bool vehicleModel)
    {
        return !vehicleModel || origin == entity::PropOrigin::VehiclePiece;
    }

    inline bool IsPieceOf(const entity::CreatePacket& packet, entity::ReplayId vehicle)
    {
        return packet.kind == entity::EntityKind::Prop && packet.propOrigin == entity::PropOrigin::VehiclePiece &&
            packet.owner == vehicle;
    }

    inline std::uint16_t GroupOf(const entity::CreatePacket& packet)
    {
        return static_cast<std::uint16_t>(packet.boneCount);
    }

    // What playback knows of a piece puppet.
    struct PieceState
    {
        entity::ReplayId owner;
        std::uint16_t group = 0;
        std::uint32_t createMs = 0;
        bool alive = false;  // it has a puppet at the playhead (its span)
        bool made = false;   // broken off the vehicle puppet and placed
        bool failed = false; // could not be made (the group was gone)
    };

    // The roots the vehicle's own group deletion leaves to its pieces: a
    // piece of it that is alive at the playhead or not born yet (the break's
    // groups are due a tick before its object was found). A failed piece,
    // or one whose whole span the playhead jumped over, leaves its groups to
    // the deletion, so the vehicle never keeps a part that came off.
    inline int ReservedRoots(const std::vector<PieceState>& pieces, entity::ReplayId vehicle,
        std::uint32_t playheadMs, std::uint16_t* out, int capacity)
    {
        int count = 0;
        for (const PieceState& piece : pieces)
        {
            // A wheel piece (kWheelPieceFlag below) holds no frag group: its wheel is the
            // natives'.
            if (!(piece.owner == vehicle) || piece.failed || count >= capacity ||
                !(piece.alive || piece.createMs > playheadMs) || (piece.group & 0x8000u) != 0)
                continue;
            bool seen = false;
            for (int n = 0; n < count; ++n)
                seen = seen || out[n] == piece.group;
            if (!seen)
                out[count++] = piece.group;
        }
        return count;
    }

    // A group of `vehicle` that a recorded piece breaks off and that piece is not
    // made yet (its group is still on the vehicle puppet): broken off at the
    // group's own break event, before any group above it is deleted.
    inline bool IsUnmadePieceGroup(const std::vector<PieceState>& pieces, entity::ReplayId vehicle, std::uint16_t group)
    {
        for (const PieceState& piece : pieces)
            if (piece.owner == vehicle && piece.group == group && (piece.group & 0x8000u) == 0 && !piece.made &&
                !piece.failed)
                return true;
        return false;
    }

    // The wheel and door natives (VPRT) wait while a piece of the vehicle is
    // due at the playhead and not made yet: the native would take the wheel
    // off first (as a piece it deletes) and the recorded piece could not
    // break it off any more. Only a piece alive at the playhead counts (one
    // whose object the game removed later has no puppet to wait for).
    inline bool DeferPartNatives(const std::vector<PieceState>& pieces, entity::ReplayId vehicle,
        std::uint32_t playheadMs)
    {
        for (const PieceState& piece : pieces)
            if (piece.owner == vehicle && piece.alive && !piece.made && !piece.failed && piece.createMs <= playheadMs)
                return true;
        return false;
    }

    // A wagon's wheel does not come off as a fragment piece: the break deletes the
    // wheel's groups from the vehicle (FRAG_BREAK where=vehicle, piece=0) and the
    // engine hands out a wheel object of its own (e.g. model wagon02x_wheel_lr
    // 0x06A01A3E), kept per wheel slot in the vehicle's draft data. Build 1491.50:
    // the wheel break 0x1579488 reads the draft data at vehicle +0xEE8 and returns
    // [draft + slot*8 + 0xA38] (0x1579835); the maker 0x1482150 (this = the draft
    // data, [draft] = the vehicle, [draft+8] = its model info) stores the object it
    // creates there (0x14823CB), of the model hash at model info +0x5B0 for slots 0
    // and 1 and +0x5B4 for the others (0x14821B6, 0x14821ED, slot/2 at 0x1482215),
    // and links it to the vehicle (0xF140A4 registers the vehicle in the object's
    // extension). That object draws with the vehicle's colours: its own tint bits
    // (+0xB8) read 0 while it shows the wagon's tint (OBJECT_LOOK tinted=0); the
    // vehicle tint selects colour entries of the vehicle's shader (setter
    // 0x10F567C writes [vehicle+0x28]+0x574/0x575 and 0x51E3F0 maps it through the
    // model's colour table), not an object palette row, so SET_OBJECT_TINT_INDEX
    // on a plain object of the wheel model cannot give it (a plain object of the
    // model draws bright yellow, without its vehicle).
    //
    // So, as a fragment child is created with its frag parent, a wheel piece is
    // recorded as a VehiclePiece whose group is kWheelPieceFlag | wheel index (no
    // frag group is that high: 0x80 bytes of group bits), with its own world track
    // from the moment it leaves, and played as the object _BREAK_OFF_VEHICLE_WHEEL
    // returns from the vehicle puppet (the engine makes it with its parent link,
    // the puppet's tint).
    constexpr std::uint16_t kWheelPieceFlag = 0x8000;
    constexpr int kWheels = 6; // _BREAK_OFF_VEHICLE_WHEEL index 0 LF .. 5 RR

    inline std::uint16_t WheelPieceGroup(int wheel)
    {
        return static_cast<std::uint16_t>(kWheelPieceFlag | (static_cast<unsigned>(wheel) & 0x7u));
    }
    inline bool IsWheelPiece(std::uint16_t group) { return (group & kWheelPieceFlag) != 0; }
    inline int WheelOfPiece(std::uint16_t group) { return static_cast<int>(group & 0x7u); }

    // The wheels of `vehicle` its wheel pieces took off at the playhead (born
    // at or before it, alive or not: a piece picked up later is a held
    // object, the wheel stays off). Bit n: wheel index n (the VPRT bits).
    inline std::uint16_t WheelPieceBits(const std::vector<PieceState>& pieces, entity::ReplayId vehicle, std::uint32_t playheadMs)
    {
        std::uint16_t bits = 0;
        for (const PieceState& piece : pieces)
            if (piece.owner == vehicle && IsWheelPiece(piece.group) && piece.createMs <= playheadMs &&
                WheelOfPiece(piece.group) < kWheels)
                bits = static_cast<std::uint16_t>(bits | (1u << WheelOfPiece(piece.group)));
        return bits;
    }

    // The parts the vehicle puppet must have off: the recorded state and the
    // wheels its pieces took (a piece may be born a sample before the 4 Hz
    // part track has the wheel off).
    inline std::uint16_t PartsWanted(std::uint16_t recorded, std::uint16_t pieceBits)
    {
        return static_cast<std::uint16_t>(recorded | pieceBits);
    }
    // The same, less the wheels the puppet keeps because a recorded wheel child
    // is still attached at the playhead (VehicleWheelChild below).
    inline std::uint16_t PartsWanted(std::uint16_t recorded, std::uint16_t pieceBits, std::uint16_t keptWheels)
    {
        return static_cast<std::uint16_t>((recorded & ~keptWheels) | pieceBits);
    }
    // Less the wheels whose recorded child left the vehicle (its object went on
    // as a loose prop): that wheel object comes off the puppet as it is, never
    // through the part native (which breaks the wheel up: the loose wheel's own
    // fragment pieces break off it later).
    inline std::uint16_t PartsWanted(std::uint16_t recorded, std::uint16_t pieceBits, std::uint16_t keptWheels,
        std::uint16_t leftWheels)
    {
        return PartsWanted(static_cast<std::uint16_t>(recorded & ~leftWheels), pieceBits, keptWheels);
    }

    // ---- recording: the wheel object of a slot (rung 2, guarded reads) ----
    constexpr std::uintptr_t kVehicleDraftOffset = 0xEE8;
    constexpr std::uintptr_t kDraftModelInfoOffset = 0x8;
    constexpr std::uintptr_t kDraftWheelObjectsOffset = 0xA38;
    constexpr std::uintptr_t kModelInfoFrontWheelOffset = 0x5B0;
    constexpr std::uintptr_t kModelInfoRearWheelOffset = 0x5B4;
    constexpr int kMaxWheelSlots = 10; // the slot count the part track accepts ([vehicle+0xBC0])

    constexpr std::uintptr_t kWheelBreakReadRva = 0x1579835;
    // mov rcx,[rax+0xEE8]; mov r14,[rcx+r15*8+0xA38]
    constexpr std::uint8_t kWheelBreakReadSignature[] = {
        0x48, 0x8B, 0x88, 0xE8, 0x0E, 0x00, 0x00, 0x4E, 0x8B, 0xB4, 0xF9, 0x38, 0x0A, 0x00, 0x00};
    constexpr std::uintptr_t kWheelMakerStoreRva = 0x14823B8;
    // mov rcx,[rdi]; mov dl,[rcx+0x8C]; mov [rax+0x8C],dl; lea rdx,[rbp-0x71]; mov [rdi+r12*8+0xA38],rax
    constexpr std::uint8_t kWheelMakerStoreSignature[] = {0x48, 0x8B, 0x0F, 0x8A, 0x91, 0x8C, 0x00, 0x00, 0x00, 0x88,
        0x90, 0x8C, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x55, 0x8F, 0x4A, 0x89, 0x84, 0xE7, 0x38, 0x0A, 0x00, 0x00};
    constexpr std::uintptr_t kWheelMakerFrontRva = 0x14821B6;
    // mov rax,[rcx+8]; mov [rbp-0x79],r15d; mov ecx,[rax+0x5B0]
    constexpr std::uint8_t kWheelMakerFrontSignature[] = {
        0x48, 0x8B, 0x41, 0x08, 0x44, 0x89, 0x7D, 0x87, 0x8B, 0x88, 0xB0, 0x05, 0x00, 0x00};
    constexpr std::uintptr_t kWheelMakerRearRva = 0x14821ED;
    // mov rax,[rdi+8]; mov ecx,[rax+0x5B4]
    constexpr std::uint8_t kWheelMakerRearSignature[] = {0x48, 0x8B, 0x47, 0x08, 0x8B, 0x88, 0xB4, 0x05, 0x00, 0x00};
    constexpr std::uintptr_t kWheelMakerPairRva = 0x1482215;
    // mov eax,r12d; cdq; sub eax,edx; sar eax,1; jne (slot/2 == 0: the front model)
    constexpr std::uint8_t kWheelMakerPairSignature[] = {0x41, 0x8B, 0xC4, 0x99, 0x2B, 0xC2, 0xD1, 0xF8, 0x75, 0x36};

    // `matches(rva, bytes, size)` -> bool: the code at the module's rva.
    template <typename Matches>
    inline bool WheelLayoutProven(Matches&& matches)
    {
        return matches(kWheelBreakReadRva, kWheelBreakReadSignature, sizeof(kWheelBreakReadSignature)) &&
            matches(kWheelMakerStoreRva, kWheelMakerStoreSignature, sizeof(kWheelMakerStoreSignature)) &&
            matches(kWheelMakerFrontRva, kWheelMakerFrontSignature, sizeof(kWheelMakerFrontSignature)) &&
            matches(kWheelMakerRearRva, kWheelMakerRearSignature, sizeof(kWheelMakerRearSignature)) &&
            matches(kWheelMakerPairRva, kWheelMakerPairSignature, sizeof(kWheelMakerPairSignature));
    }

    struct WheelRead
    {
        std::uint64_t draft = 0;
        std::uint64_t object = 0; // the wheel object's address (0: none in the slot)
        std::uint32_t frontModel = 0;
        std::uint32_t rearModel = 0;
        std::uint32_t model = 0; // the slot's model (front for slots 0 and 1)
    };

    // The wheel object and models of `slot` of the vehicle at `vehicle`.
    // read(address, out, size) -> bool: a guarded read. Returns "" or what
    // failed. The draft data's back pointer must be the vehicle.
    template <typename Read>
    inline const char* ReadWheelObject(Read&& read, std::uint64_t vehicle, int slot, WheelRead& out)
    {
        out = WheelRead{};
        if (!vehicle)
            return "no_vehicle";
        if (slot < 0 || slot >= kMaxWheelSlots)
            return "bad_slot";
        if (!read(vehicle + kVehicleDraftOffset, &out.draft, sizeof(out.draft)))
            return "read_draft";
        if (!out.draft)
            return "no_draft";
        std::uint64_t back = 0;
        if (!read(out.draft, &back, sizeof(back)))
            return "read_back";
        if (back != vehicle)
            return "draft_not_vehicle";
        std::uint64_t info = 0;
        if (!read(out.draft + kDraftModelInfoOffset, &info, sizeof(info)) || !info)
            return "read_model_info";
        if (!read(info + kModelInfoFrontWheelOffset, &out.frontModel, sizeof(out.frontModel)) ||
            !read(info + kModelInfoRearWheelOffset, &out.rearModel, sizeof(out.rearModel)))
            return "read_wheel_models";
        out.model = slot / 2 == 0 ? out.frontModel : out.rearModel;
        if (!read(out.draft + kDraftWheelObjectsOffset + static_cast<std::uint64_t>(slot) * 8u, &out.object,
                sizeof(out.object)))
            return "read_wheel_object";
        return "";
    }

    // ---- playback: the wheel objects made by vehicle puppets ----
    // The wheel _BREAK_OFF_VEHICLE_WHEEL returned is kept (hidden) when the clip
    // has a use for it: a wheel piece of that wheel, or any object puppet of its
    // model (a carried wheel is a held object, then a dropped prop: a fresh object
    // of the model is yellow). Pure decision.
    inline bool KeepWheelSpare(bool wheelPieceOfThisWheel, bool clipObjectOfModel)
    {
        return wheelPieceOfThisWheel || clipObjectOfModel;
    }

    // A fresh object puppet of a wheel model waits this long for a wheel
    // object (another puppet of the model gives its one back at its span's
    // end, the vehicle puppet breaks it off) before it is made plain.
    constexpr std::uint32_t kWheelCopyWaitMs = 1500;
    enum class WheelCopy : std::uint8_t
    {
        Plain = 0, // not a wheel model, or waited long enough: CREATE_OBJECT
        Adopt = 1, // a kept wheel object of the model is free
        Wait = 2,  // a wheel model with no free wheel yet
    };
    inline WheelCopy WheelCopyStep(bool wheelModel, bool spareFree, std::uint32_t waitedMs)
    {
        if (!wheelModel)
            return WheelCopy::Plain;
        if (spareFree)
            return WheelCopy::Adopt;
        return waitedMs < kWheelCopyWaitMs ? WheelCopy::Wait : WheelCopy::Plain;
    }

    // ---- playback: a recorded child that is one of its vehicle's own wheels ----
    // A wagon's wheel objects hang on the vehicle at its wheel bones; a ped near
    // the wagon gets them recorded as attached objects (POSE_V2_VEHICLE_CHILD,
    // 1171-Blackwater 62:192, 65:193, 99:194, 108:196 of 1:2). They are the
    // vehicle's own parts, made by its wheel maker from its model's wheel models
    // (model info +0x5B0 front, +0x5B4 rear) and drawn with its colours; the
    // vehicle puppet makes its own when the engine wants them (only near the
    // camera: a far puppet has none yet). As the recording keeps a vehicle's
    // wheels with the vehicle, such a child is decided from the vehicle's
    // model, never from the live puppet: it has no object of its own while it is
    // attached to the recorded vehicle at the playhead, and the puppet keeps that
    // wheel (the part track's wheel bit is not applied: in 1171 the wheels went
    // "destroyed" at 66.7 s while their objects stayed on the wagon, drawn, to
    // the end of the recording). Once the record has left the vehicle (its
    // attachment ended), the part track and the loose wheel path apply as before.
    // The model info is [entity+0x20] (offline, build 1491.50: the vehicle tint
    // setter reads it at 0x10F56B5 and its colour count at +0x260 at 0x52CBF4).
    constexpr std::uintptr_t kEntityModelInfoOffset = 0x20;
    constexpr std::uintptr_t kTintSetterModelInfoRva = 0x10F56B5;
    // mov rsi,[rdi+0x20]; test rsi,rsi; je; mov rcx,rsi
    constexpr std::uint8_t kTintSetterModelInfoSignature[] = {0x48, 0x8B, 0x77, 0x20, 0x48, 0x85, 0xF6, 0x74, 0x36, 0x48, 0x8B, 0xCE};

    // `matches(rva, bytes, size)` -> bool: the code at the module's rva. The
    // wheel models' offsets are the maker's (WheelLayoutProven).
    template <typename Matches>
    inline bool ModelInfoLayoutProven(Matches&& matches)
    {
        return matches(kTintSetterModelInfoRva, kTintSetterModelInfoSignature, sizeof(kTintSetterModelInfoSignature)) &&
            WheelLayoutProven(matches);
    }

    struct WheelModels
    {
        std::uint32_t front = 0;
        std::uint32_t rear = 0;
    };

    // The wheel models of the vehicle at `vehicle` from its model info: the same
    // at any distance (no wheel object needed). read(address, out, size) -> bool:
    // a guarded read.
    template <typename Read>
    inline bool ReadVehicleWheelModels(Read&& read, std::uint64_t vehicle, WheelModels& out)
    {
        out = WheelModels{};
        std::uint64_t info = 0;
        if (!vehicle || !read(vehicle + kEntityModelInfoOffset, &info, sizeof(info)) || !info)
            return false;
        if (!read(info + kModelInfoFrontWheelOffset, &out.front, sizeof(out.front)) ||
            !read(info + kModelInfoRearWheelOffset, &out.rear, sizeof(out.rear)))
        {
            out = WheelModels{};
            return false;
        }
        return out.front != 0 || out.rear != 0;
    }

    inline bool IsVehicleWheelChild(std::uint32_t childModel, const WheelModels& models)
    {
        return childModel != 0 && (childModel == models.front || childModel == models.rear);
    }

    // The wheel bits (_BREAK_OFF_VEHICLE_WHEEL / part track index: 0 LF, 1 RF,
    // 2 LM, 3 RM, 4 LR, 5 RR) a wheel child stands for, from its model and its
    // recorded place on the vehicle (left at x < 0: 1171 62:192 at x -0.99 is
    // wheel 0, 99:194 at -1.03 wheel 4, 108:196 at +1.03 wheel 5, as the
    // recording's POSE_V2_VEHICLE_WHEEL_PIECE lines read them from the slots). A
    // rear model wheel is the middle or the rear one of its side; with one model
    // on both axles the front is ahead (y > 0).
    inline std::uint16_t WheelChildBits(std::uint32_t childModel, const WheelModels& models, float x, float y)
    {
        const unsigned right = x > 0.0f ? 1u : 0u;
        const bool front = childModel == models.front && (childModel != models.rear || y > 0.0f);
        if (front)
            return static_cast<std::uint16_t>(1u << right);
        return static_cast<std::uint16_t>((1u << (2u + right)) | (1u << (4u + right)));
    }
}
