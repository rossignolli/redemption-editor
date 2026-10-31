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

#include "engine_sigs.h"
#include "entity_lifecycle.h"
#include "game_sig_core.h"

#include <cstdint>
#include <vector>

namespace replay::vehicle_pieces
{
    using enginesig::kVehicleBreakOff;

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

    // A wagon's wheel does not come off as a fragment piece: the break deletes
    // the wheel's groups from the vehicle and the engine hands out a wheel
    // object of its own, linked to the vehicle and drawn with the vehicle's
    // tint. A plain object of the wheel model cannot get that tint (it draws
    // bright yellow), so a wheel piece is recorded as a VehiclePiece whose group
    // is kWheelPieceFlag | wheel index (no frag group is that high: 0x80 bytes of
    // group bits), with its own world track from the moment it leaves, and
    // played as the object _BREAK_OFF_VEHICLE_WHEEL returns from the vehicle
    // puppet (the engine makes it with its parent link, the puppet's tint).
    // Evidence: offline dump.
    // Research: docs/research/vehicles-pieces.md
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

    // The wheel break: mov rcx,[rax+0xEE8]; mov r14,[rcx+r15*8+0xA38].
    inline const sig::Signature kWheelBreakRead{"vehicle_wheel_break_read", kVehicleBreakOff, sig::Offset{0x3AD},
        "48 8B 88 E8 0E 00 00 4E 8B B4 F9 38 0A 00 00", 0x1579835};
    // The wheel maker: the front model (mov rax,[rcx+8]; mov [rbp-0x79],r15d;
    // mov ecx,[rax+0x5B0]), the rear one (mov rax,[rdi+8]; mov ecx,[rax+0x5B4]),
    // which of them (mov eax,r12d; cdq; sub eax,edx; sar eax,1; jne: slot/2 == 0
    // is the front model) and the store (mov rcx,[rdi]; mov dl,[rcx+0x8C];
    // mov [rax+0x8C],dl; lea rdx,[rbp-0x71]; mov [rdi+r12*8+0xA38],rax).
    inline const sig::Signature kWheelMaker{"vehicle_wheel_maker",
        "85 D2 0F 88 ?? ?? ?? ?? 48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9", 0x1482150};
    inline const sig::Signature kWheelMakerFront{"vehicle_wheel_maker_front", kWheelMaker, sig::Offset{0x66},
        "48 8B 41 08 44 89 7D 87 8B 88 B0 05 00 00", 0x14821B6};
    inline const sig::Signature kWheelMakerRear{"vehicle_wheel_maker_rear", kWheelMaker, sig::Offset{0x9D},
        "48 8B 47 08 8B 88 B4 05 00 00", 0x14821ED};
    inline const sig::Signature kWheelMakerPair{"vehicle_wheel_maker_pair", kWheelMaker, sig::Offset{0xC5},
        "41 8B C4 99 2B C2 D1 F8 75 36", 0x1482215};
    inline const sig::Signature kWheelMakerStore{"vehicle_wheel_maker_store", kWheelMaker, sig::Offset{0x268},
        "48 8B 0F 8A 91 8C 00 00 00 88 90 8C 00 00 00 48 8D 55 8F 4A 89 84 E7 38 0A 00 00", 0x14823B8};

    // `found(signature)` -> bool: the signature is in the game.
    template <typename Found>
    inline bool WheelLayoutProven(Found&& found)
    {
        return found(kWheelBreakRead) && found(kWheelMakerStore) && found(kWheelMakerFront) && found(kWheelMakerRear) &&
            found(kWheelMakerPair);
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
    // A wagon's wheel objects hang on the vehicle and get recorded as attached
    // objects of a nearby ped, but they are the vehicle's own parts, made by the
    // engine only near the camera. So such a child is decided from the vehicle's
    // model, never from the live puppet: while it is attached to the recorded
    // vehicle at the playhead it has no object of its own and the puppet keeps
    // that wheel (the part track's wheel bit is not applied). Once the record
    // has left the vehicle, the part track and the loose wheel path apply.
    // The model info is [entity+0x20] (offline: the vehicle tint setter reads
    // it, then its colour count at +0x260).
    // Research: docs/research/vehicles-pieces.md
    constexpr std::uintptr_t kEntityModelInfoOffset = 0x20;
    // The tint setter's read: mov rsi,[rdi+0x20]; test rsi,rsi; je; mov rcx,rsi.
    inline const sig::Signature kTintSetter{"vehicle_tint_setter",
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 8B EA 8B D9 E8 ?? ?? ?? ?? 41 B8 0F 00 00 00",
        0x10F567C};
    inline const sig::Signature kTintSetterModelInfo{"vehicle_tint_setter_model_info", kTintSetter, sig::Offset{0x39},
        "48 8B 77 20 48 85 F6 74 36 48 8B CE", 0x10F56B5};

    // `found(signature)` -> bool: the signature is in the game. The wheel
    // models' offsets are the maker's (WheelLayoutProven).
    template <typename Found>
    inline bool ModelInfoLayoutProven(Found&& found)
    {
        return found(kTintSetterModelInfo) && WheelLayoutProven(found);
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
