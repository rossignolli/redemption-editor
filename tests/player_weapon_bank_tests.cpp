// Which weapon the hidden live player holds during a replay so the engine has
// the bank of the recorded player's next shot loaded (player_weapon_bank_core.h).
#include "player_weapon_bank_core.h"
#include "shot_audio_delay.h" // POSE_V2_SHOT_PLAYER_BANK
#include "log_policy.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
    using namespace replay::weaponbank;

    int g_bankFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_bankFailures;
    }

    void TestWanted()
    {
        const std::vector<Shot> none;
        Check(Wanted(none, 0) == 0, "no shot: no preference");

        const std::vector<Shot> shots = {{2000, 0xA}, {4000, 0xA}, {16000, 0xB}, {28000, 0xC}};
        Check(Wanted(shots, 0) == 0xA, "before the first shot: its weapon, early");
        Check(Wanted(shots, 2000) == 0xA, "at a shot: its weapon");
        Check(Wanted(shots, 5400) == 0xA, "within the hold after a shot: still its weapon");
        Check(Wanted(shots, 5600) == 0xB, "after the hold: the next shot's weapon, however far");
        Check(Wanted(shots, 29400) == 0xC, "last shot's hold");
        Check(Wanted(shots, 29600) == 0, "after the last shot: no preference");

        const std::vector<Shot> unknown = {{1000, 0}, {3000, 0xD}};
        Check(Wanted(unknown, 0) == 0xD, "a shot without a weapon hash is skipped");
        Check(Wanted(shots, 5600, 0) == 0xB, "hold 0");

        // A tester's report (a gun switch shortly before a shot): the
        // next shot within the lead takes the weapon from the held one.
        const std::vector<Shot> quick = {{1000, 0xA}, {1300, 0xA}, {1800, 0xB}, {2400, 0xA}};
        Check(Wanted(quick, 1100) == 0xA, "a shot of the held weapon still to come keeps it");
        Check(Wanted(quick, 1301) == 0xB, "after the last shot of A, B (500 ms ahead) before A's hold ends");
        Check(Wanted(quick, 1301, kHoldMs, 0) == 0xA, "lead 0: the old behaviour (A held past B's shot)");
        Check(Wanted(quick, 1801) == 0xA, "and back to A ahead of its shot");
        Check(Wanted(quick, 2401) == 0xA, "the last shot's hold");
        const std::vector<Shot> far = {{1000, 0xA}, {6000, 0xB}};
        Check(Wanted(far, 2000) == 0xA, "the next shot beyond the lead: the held weapon keeps its hold");
        Check(Wanted(far, 2600) == 0xB, "then the next at any distance");
        Check(Wanted(far, 2000, kHoldMs, 4000) == 0xB, "a longer lead reaches it");
        const std::vector<Shot> same = {{1000, 0xA}, {1000, 0xB}};
        Check(Wanted(same, 1000) == 0xA, "two shots at the same time: the first");
    }

    // What is done when the wanted weapon is not in the hand.
    void TestDecide()
    {
        Probe carried;
        carried.owned = true;
        carried.onPerson = true;
        EquipState state;
        Decision d = Decide(state, 0, carried, 0);
        Check(d.action == Action::None && !d.switched, "no wanted weapon: nothing");

        d = Decide(state, 0xA, carried, 1000);
        Check(d.switched && d.action == Action::Equip && !d.failed, "carried: a plain equip");
        d = Decide(state, 0xA, carried, 1200);
        Check(d.action == Action::None && !d.switched, "within the retry delay: wait");
        d = Decide(state, 0xA, carried, 1500);
        Check(d.action == Action::Equip && !d.failed, "second request");
        d = Decide(state, 0xA, carried, 2000);
        Check(d.action == Action::GiveCopy && d.failed, "two requests without the weapon in hand: equip_failed, a copy");
        Probe withCopy = carried;
        withCopy.copyGiven = true;
        d = Decide(state, 0xA, withCopy, 2500);
        Check(d.action == Action::Equip && !d.failed, "a copy already given: equip again, failure said once");
        withCopy.held = 0xA;
        d = Decide(state, 0xA, withCopy, 2600);
        Check(d.action == Action::None && state.attempts == 0, "in hand: done");

        // Stored on the horse: owned, not on him. Never a plain equip (it
        // would take it off the horse): a copy at once.
        Probe horse;
        horse.owned = true;
        horse.held = 0xA;
        d = Decide(state, 0xB, horse, 3000);
        Check(d.switched && d.action == Action::GiveCopy && !d.failed, "stored on the horse: a copy at once");
        horse.copyGiven = true;
        d = Decide(state, 0xB, horse, 3500);
        Check(d.action == Action::Equip, "the copy given and not in hand yet: equip it");

        // Never owned: given, as before.
        Probe none;
        EquipState fresh;
        d = Decide(fresh, 0xC, none, 0);
        Check(d.action == Action::GiveNew, "never owned: given");
        Probe givenNew;
        givenNew.owned = true;
        givenNew.onPerson = true;
        givenNew.copyGiven = true; // ours anyway: no second copy
        Decide(fresh, 0xC, givenNew, 500);
        d = Decide(fresh, 0xC, givenNew, 1000);
        Check(d.failed && d.action == Action::Equip, "given new and not in hand: equip_failed, no copy of our own gun");

        // A new wanted weapon starts its own count.
        d = Decide(fresh, 0xD, carried, 1100);
        Check(d.switched && d.action == Action::Equip && fresh.attempts == 1 && !fresh.failed, "a new weapon resets the count");
        Check(std::string(ActionName(Action::GiveCopy)) == "copy", "action names");
    }

    // The loadout check at the end.
    void TestLoadout()
    {
        Loadout before;
        before.held = 0xA;
        before.horseHolster = 0xB;
        before.attach[0] = 0xA;
        before.attach[9] = 0xE;
        before.weapons = {{0xA, true, true, 24}, {0xB, true, false, 30}};
        Loadout after = before;
        Check(LoadoutDiff(before, after).empty(), "same loadout: no diff");
        after.attach[0] = 0xB; // the hand follows the held weapon
        Check(LoadoutDiff(before, after).empty(), "hand attach points are not listed apart");
        after.held = 0xB;
        after.attach[9] = 0;
        after.horseHolster = 0;
        after.weapons = {{0xA, true, true, 24}, {0xB, true, true, 29}};
        Check(LoadoutDiff(before, after) == "held:0xA>0xB;ap9:0xE>0x0;horse:0xB>0x0;0xB.person:0>1;0xB.ammo:30>29",
            "every difference, no commas");
        after = before;
        after.weapons = {{0xA, false, false, 0}};
        Check(LoadoutDiff(before, after) == "0xA.owned:1>0;0xA.person:1>0;0xA.ammo:24>0", "a weapon lost; one missing after is skipped");

        const std::vector<Shot> shots = {{1, 0xB}, {2, 0}, {3, 0xA}, {4, 0xB}, {5, 0xC}};
        const std::vector<std::uint32_t> weapons = LoadoutWeapons(shots, 0xA);
        Check(weapons == std::vector<std::uint32_t>({0xA, 0xB, 0xC}), "held first, every shot weapon once");
        Check(LoadoutWeapons({}, 0).empty(), "nothing");
    }

    // The player's shots with their bank absent are counted and flagged; an
    // NPC's absent bank is not.
    void TestPlayerBankFlag()
    {
        namespace sd = replay::shotdelay;
        const auto shot = [](bool player, sd::Bank bank, sd::Outcome outcome) {
            sd::Shot s;
            s.fire.player = player;
            s.created = outcome != sd::Outcome::Unmatched;
            s.bank = bank;
            s.outcome = outcome;
            return s;
        };
        sd::Tally t;
        Check(std::string(sd::PlayerBankVerdict(t)) == "no_player_shots", "no player shot");
        t.Add(shot(true, sd::Bank::Loaded, sd::Outcome::Started));
        t.Add(shot(false, sd::Bank::Absent, sd::Outcome::Started));
        Check(std::string(sd::PlayerBankVerdict(t)) == "ok", "an NPC's absent bank is not the player's");
        t.Add(shot(true, sd::Bank::Absent, sd::Outcome::Started));
        t.Add(shot(true, sd::Bank::Absent, sd::Outcome::Unmatched));
        Check(t.playerShots == 3 && t.playerCreated == 2 && t.playerAbsent == 1, "player tallies");
        Check(std::string(sd::PlayerBankVerdict(t)) == "bank_absent", "flagged");
        sd::Tally merged;
        merged.Merge(t);
        Check(merged.playerAbsent == 1 && merged.playerShots == 3, "merge keeps the player counts");
        Check(sd::PlayerBankLine("play", t) == "POSE_V2_SHOT_PLAYER_BANK,phase=play,player_shots=3,created=2,bank_absent=1,verdict=bank_absent\n",
            "the summary line");
        // Both kept with Debug=0 (a QA report): the triage reads them together.
        Check(replay::log::EssentialLine("POSE_V2_SHOT_PLAYER_BANK,phase=play,bank_absent=1"), "bank line kept");
        Check(replay::log::EssentialLine("PLAYER_WEAPON_BANK,event=end,retries=3"), "weapon bank line kept");
    }
}

int RunPlayerWeaponBankTests()
{
    g_bankFailures = 0;
    TestWanted();
    TestDecide();
    TestLoadout();
    TestPlayerBankFlag();
    if (!g_bankFailures)
        std::printf("Player weapon bank tests passed.\n");
    return g_bankFailures;
}
