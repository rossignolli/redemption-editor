#pragma once

// Fragment breaks: a fragment object that loses groups (a crate splitting, a
// fence board flying off) calls its virtual PartsBrokeOff 0x6EAE68(parent
// entity, groups bitset 0x80 bytes, new inst) after the piece exists. A
// read-only detour, signature-checked, forwards first and records the parent
// and the groups into a ring the script thread reads. It never calls natives.
// Build 1491.50.

#include <cstdint>
#include <string>
#include <vector>

namespace replay::frag
{
    struct BreakCall
    {
        std::uint64_t timeUs = 0;
        std::uint64_t parent = 0;       // the fragment inst
        std::uint64_t parentEntity = 0; // its entity (inst user data), 0 when not an entity
        std::uint64_t pieceInst = 0;
        // The piece's entity (the new inst's user data, as for the parent), 0
        // when not set yet at the callback.
        std::uint64_t pieceEntity = 0;
        std::uint8_t groups[0x80] = {};
        bool copied = false;
    };
    bool Install(std::string& error);
    void Shutdown();
    std::uint64_t Total();
    void CollectSince(std::uint64_t& cursor, std::vector<BreakCall>& out);
    std::uint64_t NowUs();

    // A recorded fragment group of a vehicle taken off a vehicle puppet
    // through the engine's own DeleteAboveGroup 0x296AD24 (fragInst*, u16
    // group), the call its wheel code reaches through DeleteAbove 0x296ACCC
    // (0x1579816) and whose PartsBrokeOff 0x6EAE68 already keeps the vehicle's
    // wheel and door flags in step. The group and its subtree go, no piece is
    // made. Script thread only.
    enum class VehicleGroupResult : int
    {
        Deleted = 0,        // the group is gone now
        AlreadyGone = 1,    // nothing to do
        WheelOrDoor = 2,    // a wheel's or door's group (or under one): left to the natives
        Root = 3,           // group 0 / the root: never
        OutOfRange = 4,     // not a group of this fragType at this LOD
        NoInst = 5,         // no fragInst, wrong vtable or not owned by this vehicle
        Unsupported = 6,    // signature mismatch: the path is off for the session
        Fault = 7,          // a guarded read or the call raised
        NotRemoved = 8,     // called, the group is still there (refused by the engine)
        PieceGroup = 9,     // a recorded piece's group (or under one): the piece takes it off
    };
    constexpr int kVehicleGroupResults = 10;
    // `vehicle` is the entity address (getScriptHandleBaseAddress).
    // `reserved`: root groups of the vehicle's recorded pieces, left to
    // BreakOffVehicleGroup.
    VehicleGroupResult DeleteVehicleGroup(std::uint64_t vehicle, std::uint16_t group,
        const std::uint16_t* reserved = nullptr, int reservedCount = 0);
    // The recorded piece made again the way the engine made it: the group (and
    // what is still under it) breaks off the vehicle puppet as a new object
    // through BreakOffAboveGroup 0x2965F2C (inst, group, 0, 0), the call
    // BreakOffAbove 0x2965ECC makes for a wheel (0x15796C0). Same checks as
    // DeleteVehicleGroup (wheels and doors included: a piece may be one);
    // `pieceEntity` is the new object's address. Deleted means the piece exists
    // now.
    VehicleGroupResult BreakOffVehicleGroup(std::uint64_t vehicle, std::uint16_t group, std::uint64_t& pieceEntity);
    // The entity of a fragment inst (its user data), guarded; 0 when none.
    std::uint64_t EntityOfInst(std::uint64_t inst);
    // The lowest group of a recorded bitset (a piece's root: a parent's
    // index is always lower than its children's); -1 when empty.
    int LowestGroup(const std::uint8_t (&groups)[0x80]);
    // Checked once: the call and the offsets it relies on (build 1491.50).
    bool VehicleGroupsSupported();
    const char* VehicleGroupResultText(VehicleGroupResult result);

    // Object fragments (a crate's loose board, frag_piece.h). Script thread,
    // guarded reads, checked once against build 1491.50: the object path of
    // BREAK_OBJECT_FRAGMENT_CHILD (0xE30560: the is-fragment bit 6 of
    // [obj+0xB0], fragInst [obj+0x38]), the parent of a group (u16 [group+6],
    // 0x2974FE6), the cache entry [inst+0x128], its root group (u16
    // [cache+0x180], 0x2974F63: the inst's groups are the ones above it) and
    // its slots (s16 [cache+0x148], -1 gone, 0x29651B5).
    constexpr int kMaxObjectGroups = 0x80 * 8;
    struct ObjectFragState
    {
        int groups = 0;     // of the fragType at the inst's LOD
        bool cached = false; // a cache entry exists (root and gone are read from it)
        int root = 0;       // the cache entry's root group (0 when not cached)
        std::int16_t parents[kMaxObjectGroups] = {};
        std::uint8_t gone[kMaxObjectGroups / 8] = {}; // bit g: slot -1
    };
    bool ObjectGroupsSupported();
    // `object` is the entity address. False when it is not a fragment object
    // (type 5, the is-fragment bit, fragInst vtable, inst owned by it), the
    // offsets are unsupported or a read faulted.
    bool ReadObjectFrag(std::uint64_t object, ObjectFragState& out);
    // The group (and what is still under it) breaks off the object as a new
    // object through BreakOffAboveGroup 0x2965F2C (inst, group, 0, 0), the
    // call BreakOffAbove 0x2965ECC makes for BREAK_OBJECT_FRAGMENT_CHILD
    // without deletion (0xE3060E). Never the root (group 0 or at or below
    // the cache root). Deleted means the piece exists now; `pieceEntity` is
    // its address.
    VehicleGroupResult BreakOffObjectGroup(std::uint64_t object, std::uint16_t group, std::uint64_t& pieceEntity);
}
