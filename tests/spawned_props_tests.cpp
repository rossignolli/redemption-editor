// Props: an object refused for distance or a full watch is looked at again,
// nearest first, and an expired one only once re-armed. A wagon wheel that
// comes off is a vehicle piece of its own (the wheel object of its slot, read
// from the vehicle's draft data), the vehicle puppet keeps the wheels its
// pieces took, and a plain object puppet of a wheel model takes a wheel object
// a vehicle puppet gave.
#include "object_pose.h"
#include "vehicle_pieces.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <vector>

namespace
{
    int g_spawnedPropsFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_spawnedPropsFailures;
    }

    void TestSpawnRecheck()
    {
        using namespace replay::object_pose::spawn;
        Check(RecheckDue(1000, 1000) && RecheckDue(1001, 1000) && !RecheckDue(999, 1000),
            "recheck: due at its time, not before");
        Check(RecheckDue(5u, 0xFFFFFFF0u), "recheck: due across the clock wrap");
        Check(WithinReach(0.0f) && WithinReach(kNearM) && !WithinReach(kNearM + 0.01f) && !WithinReach(-1.0f),
            "recheck: within reach is 0..kNearM, none is not");
        // The saw: refused at 8 m (the carpenter not recorded yet), armed; the
        // carpenter recorded at 0.7 m: watched.
        Check(!WithinReach(8.0f) && WithinReach(0.7f), "saw: refused far, watched once the carpenter is near");
        // An expired bench by a standing ped is not rewatched while that ped
        // stays; it is when it moved, when nobody is near, or for another ped.
        Check(!Rearm(false, false, 1.0f, true), "rearm: same ped still near, the object still: no");
        Check(Rearm(false, true, 1.0f, true), "rearm: it moved");
        Check(Rearm(false, false, 7.0f, true) && Rearm(false, false, -1.0f, true), "rearm: nobody within reach");
        Check(Rearm(false, false, 1.0f, false), "rearm: another ped is the nearest");
        Check(Rearm(true, false, 1.0f, true), "rearm: armed stays armed");
        // A still watch with nobody near leaves after kLeaveAfterMs.
        Check(!LeaveWatch(kLeaveAfterMs - 1, 10.0f), "leave: not before kLeaveAfterMs");
        Check(LeaveWatch(kLeaveAfterMs, 10.0f) && LeaveWatch(kLeaveAfterMs, -1.0f), "leave: nobody within kLeaveM");
        Check(!LeaveWatch(60000, kLeaveM) && !LeaveWatch(60000, 2.0f), "leave: a ped within kLeaveM keeps it");
        // A full watch: the farthest still entry gives its place to a nearer
        // candidate (margin kEvictMarginM); none when all are as near.
        const float watched[] = {1.0f, 2.9f, -1.0f, 2.0f};
        Check(EvictFor(0.4f, watched, 4) == 2, "evict: an entry with no ped near goes first");
        const float near[] = {1.0f, 2.9f, 2.0f};
        Check(EvictFor(0.4f, near, 3) == 1, "evict: the farthest entry");
        Check(EvictFor(2.5f, near, 3) == -1, "evict: not for a candidate within the margin");
        Check(EvictFor(0.4f, near, 0) == -1, "evict: an empty list");
    }

    replay::entity::ReplayId Id(std::uint16_t slot, std::uint16_t instance)
    {
        replay::entity::ReplayId id;
        id.slot = slot;
        id.instance = instance;
        return id;
    }

    void TestWheelPieceGroups()
    {
        using namespace replay::vehicle_pieces;
        Check(WheelPieceGroup(4) == 0x8004 && IsWheelPiece(WheelPieceGroup(4)) && WheelOfPiece(WheelPieceGroup(4)) == 4,
            "wheel: the group keeps the index under the flag");
        Check(!IsWheelPiece(1) && !IsWheelPiece(0x3FF), "wheel: no fragment group is a wheel piece");
        const replay::entity::ReplayId wagon = Id(0, 1080);
        std::vector<PieceState> pieces;
        PieceState wheel;
        wheel.owner = wagon;
        wheel.group = WheelPieceGroup(4);
        wheel.createMs = 31168;
        pieces.push_back(wheel);
        PieceState board;
        board.owner = wagon;
        board.group = 7;
        board.createMs = 20000;
        pieces.push_back(board);
        PieceState other = wheel;
        other.owner = Id(1, 1081);
        other.group = WheelPieceGroup(0);
        pieces.push_back(other);
        Check(WheelPieceBits(pieces, wagon, 31167) == 0, "wheel: before the piece's birth the wheel is on");
        Check(WheelPieceBits(pieces, wagon, 31168) == 0x10, "wheel: from the piece's birth wheel 4 is off");
        Check(WheelPieceBits(pieces, wagon, 70000) == 0x10, "wheel: it stays off once the piece is picked up");
        Check(PartsWanted(0x0, 0x10) == 0x10 && PartsWanted(0x41, 0x10) == 0x51,
            "wheel: the wanted parts add the pieces' wheels to the recorded ones");
        // The vehicle's own group deletion: a wheel piece reserves no group.
        std::uint16_t reserved[4]{};
        const int count = ReservedRoots(pieces, wagon, 31168, reserved, 4);
        bool wheelReserved = false;
        for (int n = 0; n < count; ++n)
            wheelReserved = wheelReserved || reserved[n] == WheelPieceGroup(4) || reserved[n] == 4;
        Check(!wheelReserved, "wheel: no frag group is reserved for a wheel piece");
    }

    // A fake process memory: 8-byte cells by address.
    struct FakeMemory
    {
        std::map<std::uint64_t, std::uint64_t> cells;
        std::map<std::uint64_t, std::uint32_t> words;
        bool Read(std::uint64_t address, void* out, std::size_t size) const
        {
            if (size == 8)
            {
                const auto it = cells.find(address);
                if (it == cells.end())
                    return false;
                std::memcpy(out, &it->second, 8);
                return true;
            }
            if (size == 4)
            {
                const auto it = words.find(address);
                if (it == words.end())
                    return false;
                std::memcpy(out, &it->second, 4);
                return true;
            }
            return false;
        }
    };

    void TestWheelObjectRead()
    {
        using namespace replay::vehicle_pieces;
        const std::uint64_t vehicle = 0x1B320000000ull;
        const std::uint64_t draft = 0x1B330000000ull;
        const std::uint64_t info = 0x7FF720000000ull;
        const std::uint64_t wheelObject = 0x1B322068F50ull; // the 134 wheel's address in its fx lines
        FakeMemory memory;
        memory.cells[vehicle + kVehicleDraftOffset] = draft;
        memory.cells[draft] = vehicle;
        memory.cells[draft + kDraftModelInfoOffset] = info;
        memory.words[info + kModelInfoFrontWheelOffset] = 0x11111111u;
        memory.words[info + kModelInfoRearWheelOffset] = 0x06A01A3Eu; // wagon02x_wheel_lr
        for (int slot = 0; slot < kMaxWheelSlots; ++slot)
            memory.cells[draft + kDraftWheelObjectsOffset + 8u * static_cast<std::uint64_t>(slot)] = 0;
        memory.cells[draft + kDraftWheelObjectsOffset + 8u * 2u] = wheelObject;
        const auto read = [&](std::uint64_t address, void* out, std::size_t size) { return memory.Read(address, out, size); };
        WheelRead out;
        Check(*ReadWheelObject(read, vehicle, 2, out) == '\0' && out.object == wheelObject && out.model == 0x06A01A3Eu &&
                  out.draft == draft,
            "wheel read: slot 2 (index 4, the left rear) gives its object and the rear model");
        Check(*ReadWheelObject(read, vehicle, 1, out) == '\0' && out.object == 0 && out.model == 0x11111111u,
            "wheel read: slots 0 and 1 take the front model, an empty slot has no object");
        Check(std::strcmp(ReadWheelObject(read, vehicle, 10, out), "bad_slot") == 0 &&
                  std::strcmp(ReadWheelObject(read, vehicle, -1, out), "bad_slot") == 0,
            "wheel read: a slot out of range");
        Check(std::strcmp(ReadWheelObject(read, 0, 2, out), "no_vehicle") == 0, "wheel read: no vehicle");
        memory.cells[draft] = vehicle + 0x10;
        Check(std::strcmp(ReadWheelObject(read, vehicle, 2, out), "draft_not_vehicle") == 0,
            "wheel read: draft data that does not point back to the vehicle is refused");
        memory.cells[draft] = vehicle;
        memory.cells[vehicle + kVehicleDraftOffset] = 0;
        Check(std::strcmp(ReadWheelObject(read, vehicle, 2, out), "no_draft") == 0, "wheel read: a vehicle without draft data");
        memory.cells.erase(vehicle + kVehicleDraftOffset);
        Check(std::strcmp(ReadWheelObject(read, vehicle, 2, out), "read_draft") == 0, "wheel read: an unreadable vehicle");
    }

    void TestWheelLayout()
    {
        using namespace replay::vehicle_pieces;
        // The signatures as the dump has them (build 1491.50).
        std::map<std::uintptr_t, std::vector<std::uint8_t>> code;
        code[kWheelBreakReadRva] = std::vector<std::uint8_t>(std::begin(kWheelBreakReadSignature), std::end(kWheelBreakReadSignature));
        code[kWheelMakerStoreRva] = std::vector<std::uint8_t>(std::begin(kWheelMakerStoreSignature), std::end(kWheelMakerStoreSignature));
        code[kWheelMakerFrontRva] = std::vector<std::uint8_t>(std::begin(kWheelMakerFrontSignature), std::end(kWheelMakerFrontSignature));
        code[kWheelMakerRearRva] = std::vector<std::uint8_t>(std::begin(kWheelMakerRearSignature), std::end(kWheelMakerRearSignature));
        code[kWheelMakerPairRva] = std::vector<std::uint8_t>(std::begin(kWheelMakerPairSignature), std::end(kWheelMakerPairSignature));
        const auto matches = [&](std::uintptr_t rva, const std::uint8_t* bytes, std::size_t size) {
            const auto it = code.find(rva);
            return it != code.end() && it->second.size() == size && std::memcmp(it->second.data(), bytes, size) == 0;
        };
        Check(WheelLayoutProven(matches), "wheel layout: proven on the dump's bytes");
        Check(kWheelBreakReadSignature[3] == 0xE8 && kWheelBreakReadSignature[4] == 0x0E &&
                  kWheelBreakReadSignature[11] == 0x38 && kWheelBreakReadSignature[12] == 0x0A,
            "wheel layout: the break reads +0xEE8 then +0xA38");
        Check(kWheelMakerFrontSignature[10] == 0xB0 && kWheelMakerRearSignature[6] == 0xB4,
            "wheel layout: the maker reads the models at +0x5B0 and +0x5B4");
        code[kWheelMakerStoreRva][0] = 0x90;
        Check(!WheelLayoutProven(matches), "wheel layout: one byte off refuses it");
    }

    void TestWheelKeepAndCopy()
    {
        using namespace replay::vehicle_pieces;
        Check(KeepWheelSpare(true, false) && KeepWheelSpare(false, true) && !KeepWheelSpare(false, false),
            "wheel keep: for its piece or an object puppet of its model, else deleted as before");
        Check(WheelCopyStep(false, true, 0) == WheelCopy::Plain, "wheel copy: not a wheel model: plain");
        Check(WheelCopyStep(true, true, 0) == WheelCopy::Adopt, "wheel copy: a free wheel object: adopted");
        Check(WheelCopyStep(true, false, 0) == WheelCopy::Wait && WheelCopyStep(true, false, kWheelCopyWaitMs - 1) == WheelCopy::Wait,
            "wheel copy: waits for a wheel object a while");
        Check(WheelCopyStep(true, false, kWheelCopyWaitMs) == WheelCopy::Plain, "wheel copy: plain after the wait");
    }

    // A recorded child that is one of its vehicle's own wheels (1171-Blackwater:
    // 62:192, 65:193, 99:194, 108:196 of the wagon 1:2, model 0x5F27ED25).
    constexpr std::uint32_t kFrontWheel = 0xC62E96C1u;
    constexpr std::uint32_t kRearWheel = 0x644CD51Bu;

    // The playback of one vehicle as the playhead sees it: the part track's
    // wheel bits and the wheel children attached to the recorded vehicle.
    struct WheelChildSpan
    {
        std::uint32_t createMs, endMs; // attached to the vehicle in [createMs, endMs)
        std::uint32_t model;
        float x, y;
        bool continues; // the record goes on as a loose prop of the same object after endMs
    };
    struct PartChange
    {
        std::uint32_t atMs;
        std::uint16_t mask;
    };
    struct WheelPlayState
    {
        std::uint16_t wanted = 0; // parts the vehicle puppet has off
        int drawn[6] = {};        // per wheel: puppet wheel + recorded objects drawn
    };
    WheelPlayState WheelPlayAt(const std::vector<WheelChildSpan>& children, const std::vector<PartChange>& parts,
        const replay::vehicle_pieces::WheelModels& models, std::uint32_t timeMs)
    {
        using namespace replay::vehicle_pieces;
        std::uint16_t recorded = 0;
        for (const PartChange& change : parts)
            if (change.atMs <= timeMs)
                recorded = change.mask;
        std::uint16_t kept = 0;
        for (const WheelChildSpan& child : children)
            if (IsVehicleWheelChild(child.model, models) && timeMs >= child.createMs && timeMs < child.endMs)
                kept = static_cast<std::uint16_t>(kept | WheelChildBits(child.model, models, child.x, child.y));
        WheelPlayState state;
        state.wanted = PartsWanted(recorded, 0, kept);
        for (int wheel = 0; wheel < kWheels; ++wheel)
            state.drawn[wheel] = (state.wanted & (1u << wheel)) ? 0 : 1;
        for (const WheelChildSpan& child : children)
        {
            // An attached wheel child has no object; its loose continuation does.
            const bool loose = child.continues && timeMs >= child.endMs;
            if (!loose)
                continue;
            const std::uint16_t bits = WheelChildBits(child.model, models, child.x, child.y);
            for (int wheel = 0; wheel < kWheels; ++wheel)
                if ((bits & (1u << wheel)) && (state.wanted & (1u << wheel)))
                {
                    ++state.drawn[wheel];
                    break;
                }
        }
        return state;
    }

    void TestVehicleWheelChildren()
    {
        using namespace replay::vehicle_pieces;
        // The model info of the wagon's model: its wheel models at any distance.
        // A far puppet has no wheel objects (empty slots, no draft data): the
        // models are read all the same.
        const std::uint64_t vehicle = 0x1B320000000ull;
        const std::uint64_t info = 0x7FF720000000ull;
        FakeMemory memory;
        memory.cells[vehicle + kEntityModelInfoOffset] = info;
        memory.words[info + kModelInfoFrontWheelOffset] = kFrontWheel;
        memory.words[info + kModelInfoRearWheelOffset] = kRearWheel;
        const auto read = [&](std::uint64_t address, void* out, std::size_t size) { return memory.Read(address, out, size); };
        WheelModels models;
        Check(ReadVehicleWheelModels(read, vehicle, models) && models.front == kFrontWheel && models.rear == kRearWheel,
            "wheel child: the wheel models from the vehicle's model info, no wheel object or draft data needed (far vehicle)");
        Check(!ReadVehicleWheelModels(read, 0, models), "wheel child: no vehicle");
        memory.cells.erase(vehicle + kEntityModelInfoOffset);
        Check(!ReadVehicleWheelModels(read, vehicle, models) && models.front == 0 && models.rear == 0,
            "wheel child: an unreadable vehicle gives no models (the old path)");
        memory.cells[vehicle + kEntityModelInfoOffset] = info;
        memory.words.erase(info + kModelInfoRearWheelOffset);
        Check(!ReadVehicleWheelModels(read, vehicle, models), "wheel child: an unreadable model info gives no models");
        memory.words[info + kModelInfoRearWheelOffset] = kRearWheel;
        Check(ReadVehicleWheelModels(read, vehicle, models), "wheel child: models read again");

        Check(IsVehicleWheelChild(kFrontWheel, models) && IsVehicleWheelChild(kRearWheel, models),
            "wheel child: a child of either wheel model is one of the vehicle's wheels");
        Check(!IsVehicleWheelChild(0xC5446145u, models) && !IsVehicleWheelChild(0, models),
            "wheel child: other cargo (187:189) is not: the record draws it as before");
        Check(!IsVehicleWheelChild(kFrontWheel, WheelModels{}), "wheel child: a vehicle without wheel models has none");

        // The recorded places of the four 1171 wheels and the slots the recording read.
        Check(WheelChildBits(kFrontWheel, models, -0.987f, 1.23f) == 0x01, "wheel bits: 62:192 is wheel 0 (left front)");
        Check(WheelChildBits(kFrontWheel, models, 0.987f, 1.23f) == 0x02, "wheel bits: 65:193 is wheel 1 (right front)");
        Check(WheelChildBits(kRearWheel, models, -1.03f, -1.23f) == 0x14, "wheel bits: 99:194 is wheel 4 (left rear; or left middle)");
        Check(WheelChildBits(kRearWheel, models, 1.03f, -1.23f) == 0x28, "wheel bits: 108:196 is wheel 5 (right rear; or right middle)");
        const WheelModels same{kRearWheel, kRearWheel};
        Check(WheelChildBits(kRearWheel, same, -1.0f, 1.2f) == 0x01 && WheelChildBits(kRearWheel, same, -1.0f, -1.2f) == 0x14,
            "wheel bits: one model on both axles: ahead is the front");

        std::map<std::uintptr_t, std::vector<std::uint8_t>> code;
        code[kTintSetterModelInfoRva] =
            std::vector<std::uint8_t>(std::begin(kTintSetterModelInfoSignature), std::end(kTintSetterModelInfoSignature));
        code[kWheelBreakReadRva] = std::vector<std::uint8_t>(std::begin(kWheelBreakReadSignature), std::end(kWheelBreakReadSignature));
        code[kWheelMakerStoreRva] = std::vector<std::uint8_t>(std::begin(kWheelMakerStoreSignature), std::end(kWheelMakerStoreSignature));
        code[kWheelMakerFrontRva] = std::vector<std::uint8_t>(std::begin(kWheelMakerFrontSignature), std::end(kWheelMakerFrontSignature));
        code[kWheelMakerRearRva] = std::vector<std::uint8_t>(std::begin(kWheelMakerRearSignature), std::end(kWheelMakerRearSignature));
        code[kWheelMakerPairRva] = std::vector<std::uint8_t>(std::begin(kWheelMakerPairSignature), std::end(kWheelMakerPairSignature));
        const auto matches = [&](std::uintptr_t rva, const std::uint8_t* bytes, std::size_t size) {
            const auto it = code.find(rva);
            return it != code.end() && it->second.size() == size && std::memcmp(it->second.data(), bytes, size) == 0;
        };
        Check(ModelInfoLayoutProven(matches), "model info layout: proven on the dump's bytes");
        Check(kTintSetterModelInfoSignature[3] == 0x20, "model info layout: the tint setter reads [entity+0x20]");
        code[kTintSetterModelInfoRva][3] = 0x28;
        Check(!ModelInfoLayoutProven(matches), "model info layout: one byte off refuses it");

        Check(PartsWanted(0x31, 0, 0x3F) == 0 && PartsWanted(0x31, 0, 0) == 0x31 && PartsWanted(0x231, 0, 0x14) == 0x221,
            "parts wanted: the kept wheels' bits are not applied, doors and other wheels are");
        Check(PartsWanted(0x10, 0x10, 0x14) == 0x10, "parts wanted: a recorded wheel piece of the wheel still takes it");
        Check(PartsWanted(0x30, 0, 0, 0x30) == 0 && PartsWanted(0x231, 0, 0x01, 0x30) == 0x200,
            "parts wanted: a wheel whose child left the vehicle (the bench's rear wheels) never goes through the part native");
        Check(PartsWanted(0x10, 0x10, 0, 0x10) == 0x10, "parts wanted: a recorded wheel piece still takes its wheel");

        // 1171: the four children attached from 1.0-1.4 s to the end (74.4 s),
        // the part track: wheel 4 off at 66675, wheels 0 and 5 at 66939.
        const std::vector<WheelChildSpan> wagon = {
            {1009, 74400, kFrontWheel, -0.987f, 1.23f, false},
            {1009, 74400, kFrontWheel, 0.987f, 1.23f, false},
            {1225, 74400, kRearWheel, -1.03f, -1.23f, false},
            {1439, 74400, kRearWheel, 1.03f, -1.23f, false},
        };
        const std::vector<PartChange> parts = {{43, 0x00}, {66675, 0x10 | 0x200}, {66939, 0x31 | 0x200}};
        const int wheels[] = {0, 1, 4, 5};
        const auto one = [&](std::uint32_t timeMs) {
            const WheelPlayState state = WheelPlayAt(wagon, parts, models, timeMs);
            bool ok = true;
            for (int wheel : wheels)
                ok = ok && state.drawn[wheel] == 1;
            return ok;
        };
        bool everyTick = true;
        std::uint16_t first = WheelPlayAt(wagon, parts, models, 0).wanted;
        bool flipped = false;
        for (std::uint32_t t = 0; t < 74400; t += 16)
        {
            everyTick = everyTick && one(t);
            const std::uint16_t wanted = WheelPlayAt(wagon, parts, models, t).wanted;
            flipped = flipped || ((wanted ^ first) & 0x3F) != 0;
        }
        Check(everyTick, "1171: every wheel drawn exactly once at every tick (the puppet's, none recorded)");
        Check(!flipped, "1171: the puppet never loses a wheel its attached child stands for, over the detach ticks too");
        Check(WheelPlayAt(wagon, parts, models, 70000).wanted == 0x200, "1171: the door still comes off at the explosion");
        // Before the children's create (a scrub back to 0.5 s), across it, back
        // over the detach and after a rebuild of the puppet: the state is the
        // playhead's alone.
        const std::uint32_t scrub[] = {67000, 500, 1100, 1300, 66700, 66000, 68000, 1009, 1008, 74399};
        bool scrubbed = true;
        for (std::uint32_t t : scrub)
            scrubbed = scrubbed && one(t) && (WheelPlayAt(wagon, parts, models, t).wanted & 0x3F) == 0;
        Check(scrubbed, "1171: any scrub (before the create, across it, across the detach) or a rebuilt puppet (no state kept): exactly one wheel each");

        // A wheel that really came off: its child left the vehicle at 30000 and
        // goes on as a loose prop; the part track takes the wheel at the same time.
        const std::vector<WheelChildSpan> fallen = {{1000, 30000, kRearWheel, -1.0f, -1.2f, true}};
        const std::vector<PartChange> fallenParts = {{0, 0}, {30000, 0x10}};
        const WheelPlayState before = WheelPlayAt(fallen, fallenParts, models, 29984);
        const WheelPlayState after = WheelPlayAt(fallen, fallenParts, models, 30000);
        Check(before.drawn[4] == 1 && (before.wanted & 0x10) == 0, "fallen wheel: on the puppet until the record leaves the vehicle");
        Check(after.drawn[4] == 1 && (after.wanted & 0x10) != 0, "fallen wheel: then only the loose record (the puppet's goes off)");
        Check(WheelPlayAt(fallen, fallenParts, models, 29984).wanted == before.wanted,
            "fallen wheel: scrubbed back over the detach, the puppet has its wheel again (rebuilt whole)");
    }
}

int RunSpawnedPropsTests()
{
    g_spawnedPropsFailures = 0;
    TestSpawnRecheck();
    TestWheelPieceGroups();
    TestWheelObjectRead();
    TestWheelLayout();
    TestWheelKeepAndCopy();
    TestVehicleWheelChildren();
    if (g_spawnedPropsFailures == 0)
        std::printf("spawned props tests passed\n");
    return g_spawnedPropsFailures;
}
