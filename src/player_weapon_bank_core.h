#pragma once

// The player's weapon bank during a replay. Live, the weapon audio plays the
// local player's shot from the player's equipped-weapon wave slots
// (0x5B82B70, read at 0x2EFAB77 in the fire player 0x2EFA7E0 when the shooter
// answers "player"), which the engine fills with the bank of the weapon the
// player has equipped. In a replay the player's puppet answers "player"
// (detour 0x2F02F8C) but the slots hold the banks of the hidden live player's
// weapons: a shot whose bank is not there loses its body (-11 to -16 dB,
// more under 160 Hz). So the hidden live player equips the weapon of the
// recorded player's next shot, ahead of it, and the engine loads its bank.
// This part is pure: which weapon the live player should hold at a playhead,
// what to do when it is not in his hand, and whether his loadout came back.

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace replay::weaponbank
{
    struct Shot
    {
        std::uint32_t timeMs = 0;
        std::uint32_t weaponHash = 0;
    };

    // A shot keeps its weapon for this long after it (the echo and the late
    // layers of the shot read the same slots).
    constexpr std::uint32_t kHoldMs = 1500;
    // A tester's report (a gun switch shortly before a shot): a shot this close ahead
    // takes the weapon even while the previous shot's hold runs. Before, a
    // switch less than kHoldMs before a shot of another weapon equipped it
    // only when the hold ran out, after that shot. The early switch does not
    // cut the held shot: the slots keep the last banks equipped (about three,
    // measured in game), not only the current one.
    constexpr std::uint32_t kLeadMs = 3000;

    // The weapon the live player should hold at timeMs: the one of the next
    // shot when it is within leadMs, else the one of the player's latest shot
    // still sounding (within holdMs), else the next one at any distance ahead,
    // so the bank loads early. 0: no shot left (keep whatever is held).
    // `shots` sorted by time, the player's shots only.
    inline std::uint32_t Wanted(const std::vector<Shot>& shots, std::uint32_t timeMs, std::uint32_t holdMs = kHoldMs,
        std::uint32_t leadMs = kLeadMs)
    {
        const Shot* held = nullptr;
        const Shot* next = nullptr;
        for (const Shot& shot : shots)
        {
            if (!shot.weaponHash)
                continue;
            if (shot.timeMs >= timeMs)
            {
                next = &shot;
                break;
            }
            if (static_cast<std::uint64_t>(shot.timeMs) + holdMs >= timeMs)
                held = &shot;
        }
        if (next && (!held || next->timeMs - timeMs <= leadMs))
            return next->weaponHash;
        return held ? held->weaponHash : 0;
    }

    // What the live player has of the wanted weapon, read before each try.
    struct Probe
    {
        std::uint32_t held = 0;  // the weapon in his hand
        bool owned = false;      // HAS_PED_GOT_WEAPON, the horse's inventory included
        bool onPerson = false;   // on him (player inventory only, or carried): not stored on the horse
        bool copyGiven = false;  // a temporary copy of it was given in this playback (or it was given new: ours anyway)
    };

    enum class Action
    {
        None,     // in hand, or the last request is still within its retry delay
        Equip,    // SET_CURRENT_PED_WEAPON on what he carries
        GiveNew,  // he never had it: given, taken back by hash at the end
        GiveCopy, // he has it off his person (stored on the horse) or the equip
                  // failed: a second copy forced into the hand, taken back by
                  // its inventory GUID at the end, the stored one untouched
    };

    // The requests for one wanted weapon.
    struct EquipState
    {
        std::uint32_t weapon = 0;
        std::uint32_t attempts = 0;  // requests sent for it
        std::uint64_t nextTryMs = 0;
        bool failed = false;         // equip_failed said for it
    };

    // A request is checked this long after it (the switch takes a few frames).
    constexpr std::uint64_t kRetryMs = 500;
    // Plain equip requests before a weapon he carries counts as failed and
    // gets a copy (a playback started while mounted, a slot the engine does
    // not switch to).
    constexpr std::uint32_t kEquipTries = 2;

    struct Decision
    {
        Action action = Action::None;
        bool switched = false; // a new wanted weapon (else a retry of the same)
        bool failed = false;   // the requests sent did not bring it to the hand: say equip_failed (once per weapon)
    };

    // One step for the wanted weapon. Never equips a weapon stored off his
    // person (that would take it off the horse): a copy instead.
    inline Decision Decide(EquipState& state, std::uint32_t wanted, const Probe& probe, std::uint64_t nowMs)
    {
        Decision d;
        if (!wanted)
            return d;
        if (wanted != state.weapon)
        {
            state = EquipState{};
            state.weapon = wanted;
            d.switched = true;
        }
        if (probe.held == wanted)
        {
            state.attempts = 0;
            return d;
        }
        if (state.attempts && nowMs < state.nextTryMs)
            return d;
        const bool late = state.attempts >= kEquipTries;
        if (late && !state.failed)
        {
            state.failed = true;
            d.failed = true;
        }
        ++state.attempts;
        state.nextTryMs = nowMs + kRetryMs;
        if (!probe.owned)
            d.action = Action::GiveNew;
        else if (!probe.copyGiven && (!probe.onPerson || late))
            d.action = Action::GiveCopy;
        else
            d.action = Action::Equip;
        return d;
    }

    constexpr const char* ActionName(Action action)
    {
        return action == Action::Equip ? "equip" : action == Action::GiveNew ? "give" : action == Action::GiveCopy ? "copy" : "none";
    }

    // The live player's loadout before the first change and after the
    // restore: the weapon in hand, what sits at each attach point (holsters,
    // slings), the horse's holster, and per weapon the playback touched
    // whether he owns it, whether it is on him, and its ammo.
    constexpr std::size_t kAttachPoints = 29; // MAX_WEAPON_ATTACH_POINTS

    struct WeaponFacts
    {
        std::uint32_t weapon = 0;
        bool owned = false;
        bool onPerson = false;
        int ammo = 0;
    };

    struct Loadout
    {
        std::uint32_t held = 0;
        std::uint32_t horseHolster = 0;
        std::array<std::uint32_t, kAttachPoints> attach{};
        std::vector<WeaponFacts> weapons;
    };

    // What differs, "" when nothing: held:0xA>0xB;ap9:0xA>0xB;horse:..;
    // 0xW.owned:1>0;0xW.person:..;0xW.ammo:12>0. The hand attach points (0, 1)
    // follow the held weapon and are not listed apart. No commas (a log field).
    inline std::string LoadoutDiff(const Loadout& before, const Loadout& after)
    {
        std::string out;
        char text[96]{};
        const auto add = [&]() {
            if (!out.empty())
                out += ';';
            out += text;
        };
        if (before.held != after.held)
        {
            std::snprintf(text, sizeof(text), "held:0x%X>0x%X", before.held, after.held);
            add();
        }
        for (std::size_t ap = 2; ap < kAttachPoints; ++ap)
        {
            if (before.attach[ap] == after.attach[ap])
                continue;
            std::snprintf(text, sizeof(text), "ap%u:0x%X>0x%X", static_cast<unsigned>(ap), before.attach[ap], after.attach[ap]);
            add();
        }
        if (before.horseHolster != after.horseHolster)
        {
            std::snprintf(text, sizeof(text), "horse:0x%X>0x%X", before.horseHolster, after.horseHolster);
            add();
        }
        for (const WeaponFacts& was : before.weapons)
        {
            const WeaponFacts* now = nullptr;
            for (const WeaponFacts& w : after.weapons)
                if (w.weapon == was.weapon)
                    now = &w;
            if (!now)
                continue;
            if (was.owned != now->owned)
            {
                std::snprintf(text, sizeof(text), "0x%X.owned:%d>%d", was.weapon, was.owned ? 1 : 0, now->owned ? 1 : 0);
                add();
            }
            if (was.onPerson != now->onPerson)
            {
                std::snprintf(text, sizeof(text), "0x%X.person:%d>%d", was.weapon, was.onPerson ? 1 : 0, now->onPerson ? 1 : 0);
                add();
            }
            if (was.ammo != now->ammo)
            {
                std::snprintf(text, sizeof(text), "0x%X.ammo:%d>%d", was.weapon, was.ammo, now->ammo);
                add();
            }
        }
        return out;
    }

    // The weapons to describe in a loadout: the held one and every weapon
    // of the player's shots, once each, no zero.
    inline std::vector<std::uint32_t> LoadoutWeapons(const std::vector<Shot>& shots, std::uint32_t held)
    {
        std::vector<std::uint32_t> out;
        const auto add = [&](std::uint32_t weapon) {
            if (!weapon)
                return;
            for (const std::uint32_t w : out)
                if (w == weapon)
                    return;
            out.push_back(weapon);
        };
        add(held);
        for (const Shot& shot : shots)
            add(shot.weaponHash);
        return out;
    }
}
