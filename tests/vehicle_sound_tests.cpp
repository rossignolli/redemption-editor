// The vehicle and train car sounds of the generic player
// (vehicle_sound_rule.h), with the numbers of two recorded cases (a wagon
// 0:1243 and its sound 0x1056365 every 33 to 50 ms; train cars 10 to 14, the
// locomotive 0:725).
#include "sound_check_core.h"
#include "sound_generic_core.h"
#include "sound_generic_player_core.h"
#include "sound_owner_table.h"
#include "sound_slot_policy.h"
#include "vehicle_sound_rule.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    using namespace replay::soundgeneric;
    using namespace replay::soundgeneric::player;
    namespace owner = replay::soundowner;
    namespace slots = replay::soundslots;
    namespace vs = replay::vehiclesound;
    using replay::census::OwnerClass;

    int g_vehicleSoundFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_vehicleSoundFailures;
    }

    bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

    constexpr std::uint32_t kWagon = 1243u;                 // 0:1243
    constexpr std::uint32_t kLocomotive = 725u;             // 0:725
    constexpr std::uint32_t kCar10 = (10u << 16) | 735u;    // 10:735
    constexpr std::uint64_t kWagonSound = 0x1056365ull;     // id_kind=hash
    constexpr std::uint32_t kWagonCaller = 0x25167F1u;      // the audio entity wrapper's return

    slots::SlotView Used(std::uint64_t capKey, float distance, std::uint32_t startedMs)
    {
        slots::SlotView slot;
        slot.used = true;
        slot.owner = static_cast<std::uint32_t>(capKey);
        slot.capKey = capKey;
        slot.distance = distance;
        slot.startedMs = startedMs;
        return slot;
    }

    void TestLimits()
    {
        const vs::SlotLimits ped = vs::LimitsFor(OwnerClass::Ped, false, 6);
        Check(ped.cap == slots::kPerOwnerCap && ped.maxDistanceM == slots::kMaxDistanceM, "limits: a ped keeps 6 and 120 m");
        const vs::SlotLimits wagon = vs::LimitsFor(OwnerClass::Vehicle, false, 6);
        Check(wagon.cap == vs::kVehicleOwnerCap && wagon.maxDistanceM == slots::kMaxDistanceM, "limits: a vehicle gets 16 and 120 m");
        const vs::SlotLimits car = vs::LimitsFor(OwnerClass::Train, false, 6);
        Check(car.cap == vs::kVehicleOwnerCap && car.maxDistanceM == vs::kTrainMaxDistanceM, "limits: a train car gets 16 and 400 m");
        const vs::SlotLimits world = vs::LimitsFor(OwnerClass::World, true, 4);
        Check(world.cap == 4 && world.maxDistanceM == slots::kMaxDistanceM, "limits: a world key keeps the world cap");
        Check(vs::LimitsFor(OwnerClass::Train, true, 4).cap == 4, "limits: a world key wins over the class");
    }

    // 135: the wagon holds 13 recorded sounds at once; with the ped cap of 6
    // the 7th stole its own oldest (199 of the 200 SOUND_GENERIC_STEAL lines).
    void TestWagonCap()
    {
        std::vector<slots::SlotView> view(64);
        for (std::uint32_t n = 0; n < 13; ++n)
            view[n] = Used(kWagon, 3.8f, 3300 + n * 40);
        const vs::SlotLimits wagon = vs::LimitsFor(OwnerClass::Vehicle, false, 6);
        const slots::Choice now = slots::ChooseByKey(view.data(), view.size(), kWagon, 3.8f, wagon.cap, wagon.maxDistanceM);
        Check(now.reason == slots::Reason::Free && !now.Steals(), "135: the wagon's 14th sound takes a free slot");
        const slots::Choice before = slots::ChooseByKey(view.data(), view.size(), kWagon, 3.8f);
        Check(before.reason == slots::Reason::OwnerCap, "135: with the ped cap the same sound stole the wagon's oldest");
        for (std::uint32_t n = 13; n < vs::kVehicleOwnerCap; ++n)
            view[n] = Used(kWagon, 3.8f, 3300 + n * 40);
        const slots::Choice full = slots::ChooseByKey(view.data(), view.size(), kWagon, 3.8f, wagon.cap, wagon.maxDistanceM);
        Check(full.reason == slots::Reason::OwnerCap && full.slot == 0, "135: past 16 the wagon's oldest gives its slot");
    }

    // 186: the locomotive at 151 m (too_far_max_m) and a car at 300 m with
    // the camera on the train; a ped at the same distance stays refused.
    void TestTrainDistance()
    {
        std::vector<slots::SlotView> view(64);
        const vs::SlotLimits car = vs::LimitsFor(OwnerClass::Train, false, 6);
        Check(slots::ChooseByKey(view.data(), view.size(), kLocomotive, 151.0f, car.cap, car.maxDistanceM).reason == slots::Reason::Free,
            "186: the locomotive at 151 m plays");
        Check(slots::ChooseByKey(view.data(), view.size(), kCar10, 300.0f, car.cap, car.maxDistanceM).reason == slots::Reason::Free,
            "186: a car 300 m down the consist plays");
        Check(slots::ChooseByKey(view.data(), view.size(), kCar10, 401.0f, car.cap, car.maxDistanceM).reason == slots::Reason::TooFar,
            "186: past 400 m a car is refused");
        const vs::SlotLimits ped = vs::LimitsFor(OwnerClass::Ped, false, 6);
        Check(slots::ChooseByKey(view.data(), view.size(), 5u, 151.0f, ped.cap, ped.maxDistanceM).reason == slots::Reason::TooFar,
            "186: a ped at 151 m is still refused");
    }

    // The wagon's recorded lives: 0x1056365 every 40 ms living 30 ms (the
    // first 12 s of 135 had 161 of them between 3.8 s and 10 s).
    owner::PacketLives WagonLives()
    {
        owner::PacketLives lives;
        for (std::uint32_t t = 3883; t < 10000; t += 40)
            lives.Add(kWagon, kWagonSound, kWagonSound, t, t + 30);
        lives.Seal();
        return lives;
    }

    void TestOwnCreateRule()
    {
        const owner::PacketLives lives = WagonLives();
        const owner::LifeVerdict during = lives.Judge(kWagon, kWagonSound, 4125);
        Check(during == owner::LifeVerdict::Recorded, "135: the wagon's own 0x1056365 at 4.125 s is the recording's");
        Check(vs::RefuseVehicleOwnCreate(true, true, true, false, false, true, during, false),
            "135: the puppet wagon's own copy is refused");
        Check(!vs::RefuseVehicleOwnCreate(false, true, true, false, false, true, during, false), "rule: SoundGenericVehicleBlock=0 passes it");
        Check(!vs::RefuseVehicleOwnCreate(true, false, true, false, false, true, during, false), "rule: legacy mode passes it");
        Check(!vs::RefuseVehicleOwnCreate(true, true, false, false, false, true, during, false), "rule: not a puppet vehicle passes");
        Check(!vs::RefuseVehicleOwnCreate(true, true, true, true, false, true, during, false), "rule: the replay's own create passes");
        Check(!vs::RefuseVehicleOwnCreate(true, true, true, false, true, true, during, false),
            "rule: a create without a persistent reference (a queue's birth) passes");
        Check(!vs::RefuseVehicleOwnCreate(true, true, true, false, false, false, during, false), "rule: no lives published passes");
        const owner::LifeVerdict other = lives.Judge(kWagon, 0x117c4f9ull, 4125);
        Check(other == owner::LifeVerdict::Passed && !vs::RefuseVehicleOwnCreate(true, true, true, false, false, true, other, false),
            "rule: a sound the wagon never recorded then is left to the puppet");
        Check(vs::RefuseVehicleOwnCreate(true, true, true, false, false, true, other, true), "rule: SoundGenericBlock=all refuses it too");
        Check(vs::RefuseVehicleOwnCreate(true, true, true, false, false, true, owner::LifeVerdict::Alternative, false),
            "rule: another set at a recorded entry is refused");
        Check(vs::OwnCopiesRefused(OwnerClass::Vehicle, true) && vs::OwnCopiesRefused(OwnerClass::Train, true) &&
                  !vs::OwnCopiesRefused(OwnerClass::Vehicle, false) && vs::OwnCopiesRefused(OwnerClass::Horse, false) &&
                  !vs::OwnCopiesRefused(OwnerClass::Object, true),
            "hold: vehicles start at their time only with the block");
    }

    // The player: the wagon's packets with the puppet's own copy in the census
    // 20 ms after each (the double), with and without the block.
    struct FakeEngine : Engine
    {
        replay::census::OwnCreates own;
        std::uint64_t next = 1;
        std::vector<std::uint32_t> createdAt;
        std::uint32_t nowMs = 0;
        bool Create(const CreatePacket&, bool, std::uint64_t& handle, std::string& status, bool& retry) override
        {
            retry = false;
            handle = next++;
            createdAt.push_back(nowMs);
            status = "playing";
            return true;
        }
        bool Prepare(std::uint64_t) override { return true; }
        bool Play(std::uint64_t) override { return true; }
        void Stop(std::uint64_t, EndReason) override {}
        bool Alive(std::uint64_t) override { return true; }
        void Adjust(std::uint64_t, const AdjustPacket&) override {}
        bool MadeByOther(const CreatePacket& packet, std::uint32_t now) override
        {
            return own.Take(packet.owner, owner::PacketSoundKey(packet.stableId, packet.rawRef), packet.timeMs, now, 300);
        }
    };

    Track WagonTrack()
    {
        Track track;
        std::uint32_t index = 0;
        for (std::uint32_t t = 3883; t < 4800; t += 40)
        {
            CreatePacket p;
            p.timeMs = t;
            p.index = index++;
            p.rawRef = kWagonSound;
            const StableId id = StableIdOf(kWagonSound, true);
            p.stableId = id.id;
            p.idKind = id.kind;
            p.owner = kWagon;
            p.ownerClass = OwnerClass::Vehicle;
            p.caller = kWagonCaller;
            p.hasParams = true;
            p.hasPosition = true;
            track.creates.push_back(p);
            EndPacket end;
            end.index = p.index;
            end.timeMs = t + 30;
            end.reason = EndReason::Release;
            track.ends.push_back(end);
        }
        track.nextIndex = index;
        return track;
    }

    struct WagonRun
    {
        std::uint32_t packets = 0;
        std::uint32_t played = 0;
        std::uint32_t yielded = 0;
        std::uint32_t firstLagMs = 0xFFFFFFFFu;
        std::string total;
    };

    WagonRun PlayWagon(bool vehicleBlock, bool puppetCopies)
    {
        const Track track = WagonTrack();
        player::Config config;
        config.packets = true;
        config.enabled = true;
        config.vehicleBlock = vehicleBlock;
        owner::Overrides overrides;
        overrides.packets = true;
        owner::Cover cover;
        Player player;
        player.Begin(track, config, cover, overrides, 3800);
        FakeEngine engine;
        // The puppet's own copies: 20 ms after each recorded time; the block leaves none.
        for (std::uint32_t now = 3800; now <= 5200; now += 16)
        {
            engine.nowMs = now;
            if (puppetCopies)
                for (const CreatePacket& c : track.creates)
                    if (c.timeMs + 20 > now - 16 && c.timeMs + 20 <= now)
                        engine.own.Add(kWagon, owner::SoundKey(kWagonSound), c.timeMs + 20);
            player.Tick(now, true, engine);
        }
        WagonRun run;
        vs::VehicleTally tally;
        player.TallyVehicles(tally);
        const auto it = tally.Rows().find(kWagon);
        if (it != tally.Rows().end())
        {
            run.packets = it->second.packets;
            run.played = it->second.played;
            run.yielded = it->second.yielded;
        }
        if (!engine.createdAt.empty())
            run.firstLagMs = engine.createdAt.front() - track.creates.front().timeMs;
        run.total = vs::FormatVehicleTotal(tally, vehicleBlock);
        return run;
    }

    void TestPlayerHold()
    {
        const WagonRun before = PlayWagon(false, true);
        Check(before.packets == 23 && before.yielded > 0, "135 before: the wagon's packets yield to the puppet's copies");
        Check(before.firstLagMs >= 150 || before.played == 0, "135 before: a vehicle packet waits the anti-double hold");
        const WagonRun after = PlayWagon(true, false);
        Check(after.packets == 23 && after.played == 23 && after.yielded == 0, "135 after: every recorded packet plays once");
        Check(after.firstLagMs < 20, "135 after: a vehicle packet starts at its time (no hold)");
        Check(Has(after.total, "SOUND_GENERIC_VEHICLE_TOTAL,vehicle_block=1,vehicles=1,train_cars=0,packets=23,played=23"),
            "135 after: the total line");
        Check(Has(after.total, "verdict=ok"), "135 after: no double");
    }

    void TestTally()
    {
        Check(vs::Doubled(52, 104) && vs::Doubled(3, 60) && !vs::Doubled(2, 10) && !vs::Doubled(3, 61) && !vs::Doubled(0, 0),
            "verdict: 3 doubles and 5% of the played");
        vs::VehicleTally tally;
        for (std::uint32_t n = 0; n < 104; ++n)
            tally.NotePlan(kWagon, OwnerClass::Vehicle, true, false, false, false, n < 52);
        for (std::uint32_t n = 0; n < 49; ++n)
            tally.NotePlan(kWagon, OwnerClass::Vehicle, false, true, false, false, false);
        tally.NotePlan(5u, OwnerClass::Horse, true, false, false, false, true); // not a vehicle: ignored
        tally.NoteLateDouble(kWagon, OwnerClass::Vehicle);
        tally.NoteOwn(kWagon, OwnerClass::Vehicle, true);
        tally.NoteOwn(kWagon, OwnerClass::Vehicle, false);
        tally.NoteStealLost(kCar10, OwnerClass::Train);
        tally.NoteTooFar(kLocomotive, OwnerClass::Train);
        tally.NoteTooFar(kLocomotive, OwnerClass::Ped); // a ped's: ignored
        Check(tally.Rows().size() == 3, "tally: the wagon, car 10 and the locomotive");
        const std::vector<std::string> lines = vs::FormatVehicleLines(tally, 24);
        Check(lines.size() == 3 && Has(lines[0],
                  "SOUND_GENERIC_VEHICLE,owner=0:1243,cls=vehicle,packets=153,played=104,yielded=49,missed=0,refused=0,doubles=52,"
                  "late_doubles=1,own_refused=1,own_passed=1,steals_lost=0,too_far=0,verdict=doubled"),
            "tally: the recorded wagon of 135 comes out doubled");
        Check(Has(lines[1], "cls=train") && Has(lines[2], "cls=train"), "tally: the train rows");
        const std::string total = vs::FormatVehicleTotal(tally, false);
        Check(Has(total, "vehicle_block=0,vehicles=1,train_cars=2,packets=153,played=104,yielded=49,missed=0,doubles=52,late_doubles=1,"
                         "own_refused=1,own_passed=1,steals_lost=1,too_far=1,doubled_vehicles=1,verdict=doubled"),
            "tally: the total line names the defect");
        tally.ClearPlans();
        Check(tally.Rows().at(kWagon).packets == 0 && tally.Rows().at(kWagon).lateDoubles == 1,
            "tally: a recount keeps the engine side's counts");
        Check(vs::FormatVehicleLines(tally, 1).size() == 1, "tally: the line limit");
    }

    void TestClassSlots()
    {
        vs::ClassSlotTally tally;
        ++tally.Of(OwnerClass::Train).placed;
        ++tally.Of(OwnerClass::Train).capSteals;
        tally.NoteTooFar(OwnerClass::Train, 451.0f, true);
        tally.NoteTooFar(OwnerClass::Train, 420.0f, false);
        ++tally.Of(OwnerClass::Ped).fartherVictims;
        const std::vector<std::string> lines = vs::FormatClassSlotLines(tally, 6);
        Check(lines.size() == 2, "class slots: only the classes with an outcome");
        Check(lines.size() == 2 &&
                  lines[0] == "SOUND_GENERIC_CLASS_SLOTS,class=ped,cap=6,max_m=120,placed=0,cap_steals=0,farther_victims=1,"
                              "farther_takers=0,too_far=0,too_far_packets=0,too_far_max_m=0" &&
                  lines[1] == "SOUND_GENERIC_CLASS_SLOTS,class=train,cap=16,max_m=400,placed=1,cap_steals=1,farther_victims=0,"
                              "farther_takers=0,too_far=2,too_far_packets=1,too_far_max_m=451",
            "class slots: the lines");
        tally.Clear();
        Check(vs::FormatClassSlotLines(tally, 6).empty(), "class slots: cleared");
    }
}

int RunVehicleSoundTests()
{
    TestLimits();
    TestWagonCap();
    TestTrainDistance();
    TestOwnCreateRule();
    TestPlayerHold();
    TestTally();
    TestClassSlots();
    return g_vehicleSoundFailures;
}
