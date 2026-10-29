// The pure rules of the vehicle pieces (src/vehicle_pieces.h): no prop of
// a vehicle's model outside the piece path (a duplicate wagon), the groups
// the vehicle's own deletion leaves to its pieces, and when the wheel and
// door natives wait for a piece.
#include "vehicle_pieces.h"

#include <cstdio>
#include <vector>

namespace
{
    using namespace replay;
    using vehicle_pieces::PieceState;

    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    entity::ReplayId Id(std::uint16_t slot, std::uint16_t instance)
    {
        entity::ReplayId id;
        id.slot = slot;
        id.instance = instance;
        return id;
    }

    PieceState Piece(entity::ReplayId owner, std::uint16_t group, std::uint32_t createMs, bool alive, bool made, bool failed)
    {
        PieceState piece;
        piece.owner = owner;
        piece.group = group;
        piece.createMs = createMs;
        piece.alive = alive;
        piece.made = made;
        piece.failed = failed;
        return piece;
    }

    void TestPropModel()
    {
        using entity::PropOrigin;
        Check(vehicle_pieces::PropModelAllowed(PropOrigin::Projectile, false), "model: a projectile of an object model");
        Check(!vehicle_pieces::PropModelAllowed(PropOrigin::Projectile, true),
            "model: a projectile successor of a vehicle model is refused (the duplicate wagon)");
        Check(!vehicle_pieces::PropModelAllowed(PropOrigin::WorldPiece, true), "model: no world piece of a vehicle model");
        Check(!vehicle_pieces::PropModelAllowed(PropOrigin::GorePiece, true), "model: no gore piece of a vehicle model");
        Check(vehicle_pieces::PropModelAllowed(PropOrigin::VehiclePiece, true), "model: a vehicle piece of its vehicle's model");

        const entity::ReplayId wagon = Id(56, 1052);
        entity::CreatePacket packet;
        packet.kind = entity::EntityKind::Prop;
        packet.propOrigin = PropOrigin::VehiclePiece;
        packet.owner = wagon;
        packet.boneCount = 97;
        Check(vehicle_pieces::IsPieceOf(packet, wagon) && vehicle_pieces::GroupOf(packet) == 97, "piece: of its vehicle, its group");
        Check(!vehicle_pieces::IsPieceOf(packet, Id(57, 1053)), "piece: not of another vehicle");
        packet.propOrigin = PropOrigin::WorldPiece;
        Check(!vehicle_pieces::IsPieceOf(packet, wagon), "piece: a world piece is not a vehicle piece");
    }

    void TestReservedRoots()
    {
        const entity::ReplayId wagon = Id(56, 1052);
        const entity::ReplayId coach = Id(57, 1053);
        const std::vector<PieceState> pieces = {
            Piece(wagon, 97, 5000, true, true, false),    // made, alive
            Piece(wagon, 120, 5040, false, false, false), // not born at 5020
            Piece(wagon, 33, 4000, false, false, false),  // span jumped over / ended
            Piece(wagon, 44, 4000, true, false, true),    // failed
            Piece(wagon, 97, 5100, true, false, false),   // same root twice
            Piece(coach, 12, 1000, true, false, false),   // another vehicle
        };
        std::uint16_t roots[8] = {};
        const int count = vehicle_pieces::ReservedRoots(pieces, wagon, 5020, roots, 8);
        Check(count == 2 && roots[0] == 97 && roots[1] == 120,
            "roots: alive and unborn pieces, once each; not failed, ended or another vehicle's");
        const int later = vehicle_pieces::ReservedRoots(pieces, wagon, 6000, roots, 8);
        Check(later == 1 && roots[0] == 97, "roots: a piece past its birth with no puppet leaves its groups");
        Check(vehicle_pieces::ReservedRoots(pieces, wagon, 5020, roots, 1) == 1, "roots: capacity respected");
    }

    void TestDeferNatives()
    {
        const entity::ReplayId wagon = Id(56, 1052);
        std::vector<PieceState> pieces = {Piece(wagon, 97, 5000, true, false, false)};
        Check(vehicle_pieces::DeferPartNatives(pieces, wagon, 5000), "defer: a due piece not made yet");
        Check(!vehicle_pieces::DeferPartNatives(pieces, wagon, 4999), "defer: not before the piece is due");
        Check(!vehicle_pieces::DeferPartNatives(pieces, Id(57, 1053), 6000), "defer: only the piece's vehicle");
        pieces[0].made = true;
        Check(!vehicle_pieces::DeferPartNatives(pieces, wagon, 6000), "defer: not once the piece is made");
        pieces[0].made = false;
        pieces[0].failed = true;
        Check(!vehicle_pieces::DeferPartNatives(pieces, wagon, 6000), "defer: not for a failed piece (no endless wait)");
        pieces[0].failed = false;
        pieces[0].alive = false;
        Check(!vehicle_pieces::DeferPartNatives(pieces, wagon, 6000), "defer: not for a piece with no puppet");
    }

    // The bench's wagon (the dynamite bench): pieces 109, 110 and 112 were born
    // 137 ms after their breaks, and the groups above them deleted in the same
    // tick took them along (already_gone): an unmade piece's group is broken off
    // at its own break.
    void TestUnmadePieceGroup()
    {
        const entity::ReplayId wagon = Id(0, 1);
        std::vector<PieceState> pieces = {Piece(wagon, 109, 18370, false, false, false), Piece(wagon, 124, 18370, true, true, false),
            Piece(wagon, 44, 18370, true, false, true), Piece(wagon, vehicle_pieces::WheelPieceGroup(4), 18370, false, false, false)};
        Check(vehicle_pieces::IsUnmadePieceGroup(pieces, wagon, 109), "unmade: a piece not born yet keeps its group");
        Check(!vehicle_pieces::IsUnmadePieceGroup(pieces, wagon, 124), "unmade: not a piece already made");
        Check(!vehicle_pieces::IsUnmadePieceGroup(pieces, wagon, 44), "unmade: not a failed piece");
        Check(!vehicle_pieces::IsUnmadePieceGroup(pieces, wagon, 97), "unmade: not a group without a piece");
        Check(!vehicle_pieces::IsUnmadePieceGroup(pieces, Id(1, 2), 109), "unmade: only the piece's vehicle");
        Check(!vehicle_pieces::IsUnmadePieceGroup(pieces, wagon, vehicle_pieces::WheelPieceGroup(4)),
            "unmade: a wheel piece has no frag group");
    }
}

int RunVehiclePiecesTests()
{
    g_failures = 0;
    TestPropModel();
    TestReservedRoots();
    TestDeferNatives();
    TestUnmadePieceGroup();
    if (g_failures == 0)
        std::printf("vehicle pieces tests passed\n");
    return g_failures;
}
