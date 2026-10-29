#pragma once

// The generic sound player, pure part (unit tested in
// tests/sound_generic_player_tests.cpp).
//
// The SGEN packets the recorder keeps (sound_generic_core.h: create, adjust,
// end) are played back the way a replay editor plays its tracked sound
// packets: a create packet recreates the sound on its owner's puppet (or at
// its position) with the recorded init params, the adjust packets are
// applied at the playhead through the same engine ports the game used, and
// the end packet stops it. What is engine is behind the Engine interface
// (sound_generic_player.cpp); everything else (the plan per sound, the step
// rule, the jump and pause rule, the preload agenda, the owner table, the
// double detector, the report) is here and runs in the tests with a fake
// engine.
//
// The rules (the entity sound and capture tracks have the same ones):
// - a tick is a step when normal play moved the playhead forward by at most
//   kMaxStepMs; anything else (pause, scrub, a jump, the first tick) stops
//   every playing sound and the next step resumes from the playhead state:
//   a sound alive at the playhead and at least loopMs long (or never
//   released in the clip) is created again with its latest recorded
//   settings; a short one whose time went by is left out (its chance is gone);
// - a packet starts holdMs after its time, the time to see whether the
//   puppet (or the live world) makes the same sound by itself (Engine::
//   MadeByOther: the anti-double rule); a packet that needed loading in the
//   recording (createDelayMs at or above preloadDelayMs) is created
//   preloadMs early and prepared (S10), then played at its time (S11);
// - the owner table (sound_owner_table.h) says per family whether the
//   generic player, a specific track or the puppet alone plays; only the
//   generic ones are played here, the others are counted;
// - slow motion needs nothing per sound: the world's time scale carries the
//   engine's own slow-motion audio, as the other tracks rely on; the
//   export with the locked clock goes through the same Tick.
//
// A sound is its set and entry for the anti-double rule and the double
// detector (soundowner::SoundKey); in packets mode a step packet away from
// every recorded foot event of its owner is the generic player's (the step
// track never remakes it); every packet of an owner whose own sounds the
// ports refuse is tallied per sound and per pass (SoundTally: played, not
// played and why, ended by the engine at once), and what fell short becomes
// a finding of the sound check and of the notice at Stop; a packet window
// can be traced line by line (SoundGenericTrace).

#include "sound_generic_core.h"
#include "sound_owner_table.h"
#include "vehicle_sound_rule.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <utility>
#include <string>
#include <vector>

namespace replay::soundgeneric::player
{
    struct Config
    {
        std::uint32_t preloadMs = 500;       // SoundGenericPreloadMs: create and prepare this early
        std::uint32_t preloadDelayMs = 100;  // SoundGenericPreloadDelayMs: packets that waited this long get the preload (0: all)
        std::uint32_t holdMs = 150;          // SoundGenericHoldMs: the anti-double wait after the packet's time
        std::uint32_t loopMs = 1000;         // a sound this long is created again mid-span after a jump
        std::uint32_t maxStepMs = 250;       // ForwardEventCursor::kMaxStepMs
        std::uint32_t oneShotWindowMs = 500; // a short sound starts within this of its time or not at all
        std::uint32_t doubleToleranceMs = 250; // census::kWindowMs: the report's tolerance
        bool enabled = true;                 // SoundGenericPlay (0: everything counted, nothing played)
        // SoundGenericMode=packets. A packet of a replay entity starts at its
        // time (no hold): the puppets' own sounds are refused at the ports,
        // nothing to wait for.
        bool packets = false;
        // SoundGenericSkip, the ports the player does not call (SkipKey bits).
        std::uint32_t skipMask = 0;
        // SoundGenericEntityDrivenSkip, SkipKey bits applied only to the
        // packets whose sound updates with its entity (params flag 0x04) and
        // is created with it (not detached by entity mode a): the puppet's
        // audio entity drives those every frame. detachedEntityMode: entity
        // mode a is on (its families are created without the flag).
        std::uint32_t entityDrivenMask = 0;
        bool detachedEntityMode = false;
        // SoundGenericKeepLoops. Without it a jump or a pass wrap during normal
        // play stops every live sound and creates the long ones alive at the new
        // playhead again (loops born again at every pass). On: a live long sound
        // whose recorded life covers the new playhead (started, no end packet at
        // or before it) and that the engine side can keep (Engine::KeepAcrossJump:
        // same puppet, not a queued request) goes on playing; its adjusts are
        // walked again from the start (the state at the new playhead). Pause and
        // scrub still stop everything.
        bool keepLoops = false;
        // SoundGenericVehicleBlock (vehicle_sound_rule.h). The creators refuse a
        // puppet vehicle's or train car's own copy of a recorded sound, so their
        // packets start at their time like the peds' (no anti-double hold).
        bool vehicleBlock = false;
    };

    // SoundGenericSkip's items: the nine adjust ports (S16 to S38, by Adjust)
    // and the three recorded ends.
    constexpr std::uint8_t kSkipStop = kAdjustKinds;
    constexpr std::uint8_t kSkipForget = kAdjustKinds + 1;
    constexpr std::uint8_t kSkipRelease = kAdjustKinds + 2;
    constexpr std::uint8_t kSkipKeys = kAdjustKinds + 3;

    inline const char* SkipKeyName(std::uint8_t key)
    {
        if (key < kAdjustKinds)
            return AdjustPort(static_cast<Adjust>(key));
        return key == kSkipStop ? "stop" : key == kSkipForget ? "forget" : key == kSkipRelease ? "release" : "?";
    }

    // "S16;S18;stop;forget" (case and spaces ignored, unknown items dropped).
    inline std::uint32_t ParseSkip(const std::string& text)
    {
        std::uint32_t mask = 0;
        std::size_t at = 0;
        while (at <= text.size())
        {
            std::size_t end = text.find_first_of(";,", at);
            if (end == std::string::npos)
                end = text.size();
            std::string item;
            for (std::size_t i = at; i < end; ++i)
                if (text[i] != ' ' && text[i] != '\t')
                    item += static_cast<char>(text[i] >= 'A' && text[i] <= 'Z' ? text[i] - 'A' + 'a' : text[i]);
            for (std::uint8_t key = 0; key < kSkipKeys && !item.empty(); ++key)
            {
                std::string name = SkipKeyName(key);
                for (char& c : name)
                    c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
                if (name == item)
                    mask |= 1u << key;
            }
            at = end + 1;
        }
        return mask;
    }

    // Whether the player plays now. SoundGenericPlay=0 never plays;
    // SoundGenericExport=0 (the default) keeps it off while an export owns the
    // transport clock, so an export runs without the generic player's sounds.
    inline bool PlayAllowed(bool play, bool playInExport, bool exporting)
    {
        return play && (playInExport || !exporting);
    }

    enum class SoundState : std::uint8_t
    {
        Idle = 0,      // not yet due, or due and waiting (hold, puppet, slot)
        Preloaded = 1, // created and prepared, not playing
        Playing = 2,
        Ended = 3,     // stopped or released
        Skipped = 4,   // never played (see SkipReason)
    };

    enum class SkipReason : std::uint8_t
    {
        None = 0,
        Specific = 1, // the family's specific track plays it
        Puppet = 2,   // the family's taker is the puppet
        Yielded = 3,  // another source made it around its time
        Refused = 4,  // the engine refused the create (or the play)
        Missed = 5,   // its window went by (no puppet, no slot)
        Jumped = 6,   // a short sound whose time a jump or scrub passed
        Disabled = 7, // SoundGenericPlay=0
        // A sound of no recorded entity and with no position (a 2D sound: the
        // interface, frontend, the 2D beds of the ambience). Nothing to place
        // it on; the live world makes its own.
        Unplaced = 8,
    };
    constexpr std::uint8_t kSkipReasonCount = 9;

    inline const char* SkipName(SkipReason reason)
    {
        static const char* const kNames[kSkipReasonCount] = {"none", "specific", "puppet", "yielded", "refused", "missed",
            "jumped", "disabled", "unplaced"};
        return static_cast<std::uint8_t>(reason) < kSkipReasonCount ? kNames[static_cast<std::uint8_t>(reason)] : "?";
    }

    constexpr std::uint32_t kNeverMs = 0xFFFFFFFFu;
    constexpr std::uint64_t kNoHandle = 0xFFFFFFFFFFFFFFFFull;

    // A packet's outcome as the QA mark reports it: played, or the skip
    // reason (SkipReason, same order after Played).
    enum class Outcome : std::uint8_t
    {
        None = 0,     // not reached yet
        Played = 1,
        Specific = 2, // the family's specific track plays it (the generic skipped it)
        Puppet = 3,
        Yielded = 4,
        Refused = 5,
        Missed = 6,
        Jumped = 7,
        Disabled = 8,
        Unplaced = 9, // a 2D sound of no entity, left to the live world
    };
    constexpr std::uint8_t kOutcomeCount = 10;

    inline const char* OutcomeName(Outcome outcome)
    {
        static const char* const kNames[kOutcomeCount] = {"none", "played", "specific", "puppet", "yielded", "refused", "missed",
            "jumped", "disabled", "unplaced"};
        return static_cast<std::uint8_t>(outcome) < kOutcomeCount ? kNames[static_cast<std::uint8_t>(outcome)] : "?";
    }

    inline Outcome OutcomeOfSkip(SkipReason reason)
    {
        return reason == SkipReason::None ? Outcome::None : static_cast<Outcome>(static_cast<std::uint8_t>(reason) + 1);
    }

    // One sound of the clip: its create packet, its adjusts and ends, what
    // the owner table decided, and its playback state.
    struct SoundPlan
    {
        std::uint32_t create = 0;  // index into Track::creates
        std::uint32_t startMs = 0;
        std::uint32_t endMs = kNeverMs; // the release (end of the sound), kNeverMs when the clip ends first
        bool released = false;
        bool preload = false;
        soundowner::Family family = soundowner::Family::Owned;
        soundowner::Taker taker = soundowner::Taker::Generic;
        std::vector<std::uint32_t> adjusts; // indices into Track::adjusts, time order
        std::vector<std::uint32_t> ends;    // indices into Track::ends, time order
        // The create's own sample of a setting (first) and the value the owner
        // set in that frame, recorded one throttle interval later (second): the
        // second plays from the start.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> startValues;
        // Playback state.
        SoundState state = SoundState::Idle;
        SkipReason skip = SkipReason::None;
        std::uint64_t handle = kNoHandle;
        std::uint32_t stateMs = 0;
        std::size_t adjustCursor = 0;
        std::size_t endCursor = 0;
        bool stopped = false;  // a stop packet (S13) was applied, the release is still to come
        bool resumed = false;  // created again mid-span after a jump
        bool preloadedOnce = false;
        bool counted = false;  // its first outcome (played or skipped) is in the stats
        // What happened to the packet on the latest pass over its time, kept
        // across the resets of a pause, scrub or jump (Resume clears `state`
        // and `skip`): the QA mark's window reads it (WindowReport).
        Outcome lastOutcome = Outcome::None;
        bool doubled = false;  // a start of it was a double (DoubleDetector)
        // Packets mode, a packet of a ped or horse: started at its time,
        // without the hold (the puppet's own is refused).
        bool noHold = false;
        // Packets mode, how late it started against its recorded time; its
        // adjusts and ends come that much later, so a late start keeps the
        // recorded life (a short sound whose stop and forget falls inside the
        // hold or a tick's lateness is not stopped as it starts).
        std::uint32_t shiftMs = 0;
        // Packets mode, a step packet away from every recorded foot event of
        // its owner (the step track does not remake it): the generic player's.
        bool offStep = false;
        // A weapon audio packet the shot track does not remake
        // (soundowner::OffShot): owned, the generic player's.
        bool offShot = false;
        // Packets mode, a step or melee packet of a person that its track
        // replays through the engine (the recorded foot event, the recorded
        // hit): the generic player plays it when the track's engine call did
        // not make the same sound again (the anti-double rule after the hold),
        // so what the call leaves out (a set the puppet picks differently, a
        // layer it never plays) plays from the recording.
        bool complement = false;
        bool complementSeen = false;     // its time was reached on tick complementTick
        std::uint64_t complementTick = 0;
        bool entityDriven = false; // updates with its entity (flag 0x04, kept)
        bool seenLater = false; // a later tick saw it alive (not ended by the engine at once)
        bool traced = false;    // inside the SoundGenericTrace window
        // Mode c, the queue's sound not born yet: its adjusts and ends wait,
        // and count from the birth (a sound born late keeps its recorded life;
        // never stopped before it is born).
        bool awaitingBirth = false;
        std::int32_t bornDelayMs = 0; // birth tick minus recorded time (mode c)
        bool kept = false; // kept playing across a jump (SoundGenericKeepLoops)

        bool Long(std::uint32_t loopMs) const { return !released || endMs - startMs >= loopMs; }
    };

    struct FamilyStats
    {
        std::uint32_t packets = 0;
        std::uint32_t played = 0;
        std::uint32_t preloaded = 0;
        std::uint32_t refused = 0;
        std::uint32_t skipped = 0; // specific, puppet, disabled
        std::uint32_t yielded = 0;
        std::uint32_t missed = 0;
        std::uint32_t jumped = 0;
        std::uint32_t doubles = 0;
        std::uint32_t unplaced = 0; // 2D sounds of no entity
    };

    // The start values of a sound in a clip recorded with the older recorder.
    // The recorder samples each setting at most every 1/SoundGenericUpdateHz
    // (50 ms): the creator's own call with the params' default (volume 0 dB)
    // took the sample at the create and the value the owner set in the same
    // frame waited for the next tick, 50 to 67 ms later, so the replay played
    // that first stretch, often the whole life, at 0 dB. A setting whose first
    // sample is at the create (within kStartValueAtMs) and whose next sample
    // comes within kStartValueWindowMs, with another value, starts at that
    // next value: pairs (first, next) of indices into track.adjusts. The
    // positions are left out (the player places the sound itself).
    constexpr std::uint32_t kStartValueAtMs = 2;
    constexpr std::uint32_t kStartValueWindowMs = 100;
    inline std::vector<std::pair<std::uint32_t, std::uint32_t>> StartValues(const Track& track, std::uint32_t createMs,
        const std::vector<std::uint32_t>& adjusts)
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
        struct Seen
        {
            Adjust kind;
            std::uint32_t hash;
            std::uint32_t first;
            bool open;
        };
        std::vector<Seen> seen;
        for (const std::uint32_t idx : adjusts)
        {
            if (idx >= track.adjusts.size())
                continue;
            const AdjustPacket& a = track.adjusts[idx];
            if (a.kind == Adjust::Position || a.kind == Adjust::PositionNoDoppler)
                continue;
            if (a.timeMs < createMs)
                continue;
            if (a.timeMs > createMs + kStartValueWindowMs)
                break; // time order
            Seen* entry = nullptr;
            for (Seen& e : seen)
                if (e.kind == a.kind && e.hash == a.hash)
                    entry = &e;
            const bool atCreate = a.timeMs <= createMs + kStartValueAtMs;
            if (!entry)
            {
                seen.push_back(Seen{a.kind, a.hash, idx, atCreate});
                continue;
            }
            if (!entry->open)
                continue;
            if (atCreate)
            {
                entry->first = idx; // several in the create frame: the last one is the start
                continue;
            }
            entry->open = false;
            const AdjustPacket& first = track.adjusts[entry->first];
            if (std::memcmp(first.value, a.value, sizeof(a.value)) != 0)
                out.emplace_back(entry->first, idx);
        }
        return out;
    }

    struct PlayStats
    {
        std::uint32_t packets = 0;      // create packets in the clip
        std::uint32_t generic = 0;      // taker generic
        std::uint32_t played = 0;       // starts that the engine accepted
        std::uint32_t preloaded = 0;    // created early and prepared
        std::uint32_t resumed = 0;      // created again mid-span after a jump
        std::uint32_t refused = 0;
        std::uint32_t yielded = 0;
        std::uint32_t missed = 0;
        std::uint32_t jumped = 0;
        std::uint32_t skippedSpecific = 0;
        std::uint32_t skippedPuppet = 0;
        std::uint32_t adjusts = 0;      // adjust packets applied
        std::uint32_t stops = 0;        // S13
        std::uint32_t forgets = 0;      // S14 from a stop-and-forget packet
        std::uint32_t releases = 0;     // release packets: the sound was still alive (forgotten, not stopped)
        std::uint32_t endedByEngine = 0; // the engine ended the sound before its release packet
        std::uint32_t stopAll = 0;      // pause, scrub or jump stops
        std::uint32_t keptLoops = 0;    // long sounds kept playing across a jump
        std::uint32_t doubles = 0;      // played while another source made the same sound (DoubleDetector)
        std::uint32_t livePeak = 0;
        std::uint32_t skippedByReason[kSkipReasonCount] = {};
        FamilyStats families[soundowner::kFamilyCount];
        // 2D sounds of no entity (SkipReason::Unplaced), and the engine's
        // refusals by the engine side's status ("not_created:audio_ok",
        // "exception_..."), one per packet.
        std::uint32_t unplaced = 0;
        std::map<std::string, std::uint32_t> refusedStatus;
        // Packets mode: generic packets of a family a track plays otherwise
        // (entity sound, sound request, collision, capture, movement), played
        // here with their adjusts.
        std::uint32_t fromTracks = 0;
        std::uint32_t entityDriven = 0; // generic packets their entity updates (flag 0x04 kept)
        // Packets mode, step packets away from every recorded foot event of
        // their owner, played here; and the sounds the engine ended before a
        // later tick saw them (created, never heard).
        std::uint32_t offStep = 0;
        std::uint32_t offShot = 0; // weapon audio packets away from every shot
        // Step and melee packets of persons given to the generic player as the
        // complement of their track's engine call, and per pass what they came
        // to (played: the call had not made it; yielded: the call made it,
        // nothing played twice).
        std::uint32_t complement = 0;
        std::uint32_t complementPlayed = 0;
        std::uint32_t complementYielded = 0;
        std::uint32_t complementMeleePlayed = 0;
        // Of complementYielded, the ones the call made in another entry of
        // the same set (playing them would double the call's own sound).
        std::uint32_t complementYieldedSet = 0;
        std::uint32_t startValues = 0; // create samples played with the value set in that frame
        std::uint32_t endedAtOnce = 0;
        // Ended by the engine before half the life they had live (EndedEarly).
        std::uint32_t endedEarly = 0;
        // Per SoundGenericSkip item, the calls the player did not make.
        std::uint32_t skippedByKey[kSkipKeys] = {};
        // Mode c starts asked ahead of their time.
        std::uint32_t ledStarts = 0;
    };

    // Per owner and sound, what each pass did with the packets of an owner
    // whose own sounds the ports refuse (noHold: nothing else makes them).
    // The census sees a create of the generic player as "played" whether or
    // not it is ever heard: this is what the player itself knows.
    struct SoundTallyKey
    {
        std::uint32_t owner = census::kNoOwner;
        std::uint64_t sound = 0; // soundowner::SoundKey
        bool operator<(const SoundTallyKey& o) const { return owner != o.owner ? owner < o.owner : sound < o.sound; }
    };

    struct SoundTallyEntry
    {
        census::OwnerClass cls = census::OwnerClass::World;
        soundowner::Family family = soundowner::Family::Owned;
        std::uint64_t ref = 0;     // the first packet's raw ref (the report's ref)
        std::uint32_t taken = 0;   // due in a pass
        std::uint32_t played = 0;
        std::uint32_t yielded = 0;
        std::uint32_t refused = 0;
        std::uint32_t missed = 0;
        std::uint32_t endedAtOnce = 0; // created, ended by the engine before a later tick
        std::uint32_t endedEarly = 0;  // ended by the engine before half its recorded life
        std::uint32_t offStep = 0;     // of them, step packets away from a foot event
        std::uint32_t firstShortMs = 0xFFFFFFFFu; // clip time of the first that fell short
    };

    using SoundTally = std::map<SoundTallyKey, SoundTallyEntry>;

    // A step or melee packet of a person the track's engine call did not
    // make again, played from the recording (the report's and the log's
    // evidence).
    struct ComplementEvidence
    {
        std::uint32_t owner = census::kNoOwner;
        std::uint64_t ref = 0;
        std::uint32_t timeMs = 0;  // the packet's recorded time
        std::uint32_t lateMs = 0;  // how late it started (the hold and the ticks' wait)
        soundowner::Family family = soundowner::Family::Footstep;
        std::uint32_t caller = 0;  // the family caller (the engine function that made it live)
    };
    constexpr std::size_t kComplementEvidenceMax = 48;
    // The ticks after its time a complement waits for the census of its
    // track's call (the call of tick N is drained at the start of N + 1).
    constexpr std::uint64_t kComplementTicks = 2;

    // A tally row fell short: half or more of the packets due (2 or more)
    // did not play, or half or more of the played (2 or more) were ended by
    // the engine before a later tick, or half or more of the played were
    // ended by the engine before half the life they had live.
    inline bool TallyShort(const SoundTallyEntry& e)
    {
        const std::uint32_t notPlayed = e.yielded + e.refused + e.missed;
        return (notPlayed >= 2 && notPlayed * 2 >= e.taken) || (e.endedAtOnce >= 2 && e.endedAtOnce * 2 >= e.played) ||
            (e.endedEarly >= 1 && e.endedEarly * 2 >= e.played);
    }

    // A sound the engine ended well before the life it had live. The
    // recorded end is its release with no stop or stop-and-forget before it
    // (a one-shot over by itself, not one the game stopped), that life is at
    // least kEarlyMinLifeMs, and the replayed one lived less than half of it
    // with no stop of ours.
    constexpr std::uint32_t kEarlyMinLifeMs = 200;

    inline bool EndedEarly(std::uint32_t recordedLifeMs, std::uint32_t livedMs)
    {
        return recordedLifeMs >= kEarlyMinLifeMs && static_cast<std::uint64_t>(livedMs) * 2 < recordedLifeMs;
    }

    // SoundGenericTrace=owner@fromMs-toMs (owner "0:1", or
    // "*" for every owner). Empty: off.
    struct TraceWindow
    {
        bool on = false;
        bool anyOwner = false;
        std::uint32_t owner = census::kNoOwner;
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
        std::uint32_t maxLines = 2000; // the adjusts are traced too
        bool Holds(std::uint32_t packetOwner, std::uint32_t timeMs) const
        {
            return on && (anyOwner || packetOwner == owner) && timeMs >= fromMs && timeMs <= toMs;
        }
    };

    inline TraceWindow ParseTraceWindow(const std::string& text)
    {
        TraceWindow out;
        std::string t;
        for (const char c : text)
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
                t += c;
        const std::size_t at = t.find('@');
        const std::size_t dash = t.find('-', at == std::string::npos ? 0 : at);
        if (at == std::string::npos || dash == std::string::npos)
            return out;
        const std::string who = t.substr(0, at);
        if (who == "*")
            out.anyOwner = true;
        else
        {
            const std::size_t colon = who.find(':');
            if (colon == std::string::npos)
                return out;
            char* end = nullptr;
            const unsigned long slot = std::strtoul(who.c_str(), &end, 10);
            if (end != who.c_str() + colon)
                return out;
            const unsigned long instance = std::strtoul(who.c_str() + colon + 1, &end, 10);
            if (*end || slot > 0xFFFFu || instance > 0xFFFFu)
                return out;
            out.owner = static_cast<std::uint32_t>((slot << 16) | instance);
        }
        char* end = nullptr;
        const unsigned long from = std::strtoul(t.c_str() + at + 1, &end, 10);
        if (end != t.c_str() + dash)
            return out;
        const unsigned long to = std::strtoul(t.c_str() + dash + 1, &end, 10);
        if (*end || to < from)
            return out;
        out.fromMs = static_cast<std::uint32_t>(from);
        out.toMs = static_cast<std::uint32_t>(to);
        out.on = true;
        return out;
    }

    // What the packets of a playhead window came to (the QA mark's "sounds"
    // block): per family with the taker that owns it, per owner, and the
    // packets that failed.
    struct WindowFamily
    {
        soundowner::Family family = soundowner::Family::World;
        soundowner::Taker taker = soundowner::Taker::Generic;
        std::uint32_t packets = 0;
        std::uint32_t counts[kOutcomeCount] = {}; // by Outcome
        std::uint32_t doubles = 0;
    };

    struct WindowOwner
    {
        std::uint32_t owner = census::kNoOwner;
        census::OwnerClass cls = census::OwnerClass::World;
        soundowner::Family family = soundowner::Family::World;
        soundowner::Taker taker = soundowner::Taker::Generic;
        std::uint32_t packets = 0;
        std::uint32_t counts[kOutcomeCount] = {};
        std::uint32_t doubles = 0;
    };

    struct WindowFailure
    {
        std::uint32_t timeMs = 0;
        std::uint32_t owner = census::kNoOwner;
        soundowner::Family family = soundowner::Family::World;
        soundowner::Taker taker = soundowner::Taker::Generic;
        std::uint32_t set = 0; // the low 32 bits of the stable id (the sound set)
        Outcome outcome = Outcome::None;
        bool doubled = false;
    };

    struct WindowReport
    {
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
        std::uint32_t packets = 0;
        std::uint32_t counts[kOutcomeCount] = {};
        std::uint32_t doubles = 0;
        std::vector<WindowFamily> families; // only families with packets, in family order
        std::vector<WindowOwner> owners;    // (owner, family) rows, most packets first, at most kWindowOwners
        std::vector<WindowFailure> failures; // refused, missed and doubled packets in time order, at most kWindowFailures
    };
    constexpr std::size_t kWindowOwners = 16;
    constexpr std::size_t kWindowFailures = 64;

    // The init variables of a clip recorded before SGEN v3, from the full
    // params the other tracks keep (sound requests, entity sounds, the
    // capture): the row of the same owner and raw ref nearest in time,
    // within toleranceMs. The packet's own (SGEN v3) come first.
    class InitVariableIndex
    {
    public:
        void Clear() { m_rows.clear(); m_sealed = false; }
        void Add(std::uint32_t owner, std::uint64_t ref, std::uint32_t timeMs, const std::uint8_t* params, std::size_t size,
            std::uint8_t source)
        {
            if (!ref || !params)
                return;
            Row row;
            ReadInitVariables(params, size, row.vars);
            ReadValueDwords(params, size, row.vars); // the value dwords too
            if (!row.vars.initVariableCount && !row.vars.valuesKnown)
                return;
            row.owner = owner;
            row.ref = ref;
            row.timeMs = timeMs;
            row.source = source;
            m_rows.push_back(row);
            m_sealed = false;
        }
        void Seal()
        {
            std::sort(m_rows.begin(), m_rows.end(), [](const Row& a, const Row& b) {
                return a.owner != b.owner ? a.owner < b.owner : a.ref != b.ref ? a.ref < b.ref : a.timeMs < b.timeMs;
            });
            m_sealed = true;
        }
        std::size_t Size() const { return m_rows.size(); }
        // The row's variables into `out` (InitParams' init variables) and
        // its source; false when none is near enough.
        bool Find(std::uint32_t owner, std::uint64_t ref, std::uint32_t timeMs, std::uint32_t toleranceMs, InitParams& out,
            std::uint8_t& source) const
        {
            if (!m_sealed || !ref)
                return false;
            Row key;
            key.owner = owner;
            key.ref = ref;
            key.timeMs = timeMs > toleranceMs ? timeMs - toleranceMs : 0;
            auto it = std::lower_bound(m_rows.begin(), m_rows.end(), key, [](const Row& a, const Row& b) {
                return a.owner != b.owner ? a.owner < b.owner : a.ref != b.ref ? a.ref < b.ref : a.timeMs < b.timeMs;
            });
            const Row* best = nullptr;
            std::uint32_t bestGap = 0xFFFFFFFFu;
            for (; it != m_rows.end() && it->owner == owner && it->ref == ref && it->timeMs <= timeMs + toleranceMs; ++it)
            {
                const std::uint32_t gap = it->timeMs > timeMs ? it->timeMs - timeMs : timeMs - it->timeMs;
                if (gap < bestGap)
                {
                    bestGap = gap;
                    best = &*it;
                }
            }
            if (!best)
                return false;
            out.initVariableCount = best->vars.initVariableCount;
            for (std::size_t n = 0; n < kInitVariables; ++n)
                out.initVariables[n] = best->vars.initVariables[n];
            out.valuesKnown = best->vars.valuesKnown;
            out.valueCount = best->vars.valueCount;
            for (std::size_t n = 0; n < kValueDwords; ++n)
                out.values[n] = best->vars.values[n];
            source = best->source;
            return true;
        }

    private:
        struct Row
        {
            std::uint32_t owner = census::kNoOwner;
            std::uint64_t ref = 0;
            std::uint32_t timeMs = 0;
            std::uint8_t source = 0;
            InitParams vars;
        };
        std::vector<Row> m_rows;
        bool m_sealed = false;
    };

    // The packets SoundGenericEntityMode changes: the sounds of an owner the
    // ESND and SNDS families carry (where the continuous sounds bound to the
    // animal are); the rest of the generic player stays as it was.
    inline bool EntityModeApplies(soundowner::Family family, std::uint32_t owner)
    {
        return owner != census::kNoOwner &&
            (family == soundowner::Family::EntitySound || family == soundowner::Family::SoundRequest);
    }

    // Mode a: the params' flags without "update with the entity".
    inline std::uint8_t DetachedFlags(std::uint8_t flags)
    {
        return static_cast<std::uint8_t>(flags & ~kParamsFlagUpdateEntity);
    }

    // The packets mode c asks of the game's queue (an owner's ESND or SNDS
    // sound named by more than its hash); the rest is the direct create.
    inline bool QueuedPacket(EntityMode configured, soundowner::Family family, const CreatePacket& packet)
    {
        return configured == EntityMode::GameQueue && EntityModeApplies(family, packet.owner) &&
            packet.idKind != IdKind::Hash && !(packet.idKind == IdKind::None && packet.core != 0);
    }

    // Mode c, the queue's latency between the request and the birth of its
    // sound (the recording has it per packet, 25 to 35 ms on average). The
    // player asks the queue this much ahead, a moving average per family of
    // what it measured, at most kQueueLeadMaxMs, so the sound is born at its
    // recorded time.
    constexpr std::uint32_t kQueueLeadMaxMs = 150;
    class QueueLead
    {
    public:
        void Reset()
        {
            for (Family& f : m_families)
                f = Family{};
        }
        void Note(soundowner::Family family, double latencyMs)
        {
            const std::uint8_t n = static_cast<std::uint8_t>(family);
            if (n >= soundowner::kFamilyCount || !(latencyMs >= 0.0))
                return;
            Family& f = m_families[n];
            f.averageMs = f.samples ? f.averageMs + (latencyMs - f.averageMs) * kWeight : latencyMs;
            ++f.samples;
            f.totalMs += latencyMs;
            if (latencyMs > f.maxMs)
                f.maxMs = latencyMs;
        }
        std::uint32_t LeadMs(soundowner::Family family) const
        {
            const std::uint8_t n = static_cast<std::uint8_t>(family);
            if (n >= soundowner::kFamilyCount || !m_families[n].samples)
                return 0;
            const double ms = m_families[n].averageMs + 0.5;
            return ms >= kQueueLeadMaxMs ? kQueueLeadMaxMs : static_cast<std::uint32_t>(ms);
        }
        std::uint32_t Samples(soundowner::Family family) const
        {
            const std::uint8_t n = static_cast<std::uint8_t>(family);
            return n < soundowner::kFamilyCount ? m_families[n].samples : 0;
        }
        double MeanMs(soundowner::Family family) const
        {
            const std::uint8_t n = static_cast<std::uint8_t>(family);
            return n < soundowner::kFamilyCount && m_families[n].samples ? m_families[n].totalMs / m_families[n].samples : 0.0;
        }
        double MaxMs(soundowner::Family family) const
        {
            const std::uint8_t n = static_cast<std::uint8_t>(family);
            return n < soundowner::kFamilyCount ? m_families[n].maxMs : 0.0;
        }

    private:
        static constexpr double kWeight = 0.25; // the latest births weigh a quarter
        struct Family
        {
            std::uint32_t samples = 0;
            double averageMs = 0.0;
            double totalMs = 0.0;
            double maxMs = 0.0;
        };
        Family m_families[soundowner::kFamilyCount];
    };

    // Mode b's state of one sound: the latest recorded value of each
    // variable (the create's init and params variables, then each S38 in
    // order), the last volume (S18) and pitch (S19); the engine writes them
    // back every tick.
    class OverrideState
    {
    public:
        static constexpr std::size_t kMax = kInitVariables + kVariables + 8;

        void Clear() { *this = OverrideState{}; }

        void Seed(const InitParams& params, const InitParams* initVariables)
        {
            Clear();
            volume = params.volume;
            pitch = params.pitch;
            if (initVariables)
                for (std::size_t n = 0; n < initVariables->initVariableCount && n < kInitVariables; ++n)
                    Set(initVariables->initVariables[n].hash, initVariables->initVariables[n].value);
            // +0x10C holds raw dwords (possibly a NaN as a float); one that is not
            // a number never becomes a variable value.
            for (std::size_t n = 0; n < params.variableCount && n < kVariables; ++n)
                if (std::isfinite(params.variables[n].value))
                    Set(params.variables[n].hash, params.variables[n].value);
        }

        void Note(const AdjustPacket& a)
        {
            switch (a.kind)
            {
            case Adjust::Volume: volume = a.value[0]; break;
            case Adjust::Pitch:
                pitch = static_cast<std::int16_t>(a.value[0] < -32768.0f ? -32768 : a.value[0] > 32767.0f ? 32767
                    : static_cast<int>(a.value[0] < 0.0f ? a.value[0] - 0.5f : a.value[0] + 0.5f));
                break;
            case Adjust::Variable: Set(a.hash, a.value[0]); break;
            default: break;
            }
        }

        std::size_t VariableCount() const { return m_count; }
        const Variable& VariableAt(std::size_t n) const { return m_vars[n]; }
        float volume = 0.0f;
        std::int16_t pitch = 0;

    private:
        void Set(std::uint32_t hash, float value)
        {
            if (!hash)
                return;
            for (std::size_t n = 0; n < m_count; ++n)
                if (m_vars[n].hash == hash)
                {
                    m_vars[n].value = value;
                    return;
                }
            if (m_count < kMax)
                m_vars[m_count++] = Variable{hash, value};
        }

        Variable m_vars[kMax];
        std::size_t m_count = 0;
    };

    // ------------------------------------------------------------ linked sounds
    //
    // Mode c's sound comes from the game's queue with no slot the engine
    // nulls when it frees the sound, so a raw pointer kept until a later stop
    // may point at freed memory. A sound is held only through a reference the
    // engine clears: here the reference is the sound's identity
    // (audio::SoundIdentity: the requested settings block that points back at
    // it), taken at the birth and checked before every stop, adjust or
    // liveness answer. A sound whose identity no longer holds is gone: nothing
    // is called on it, it is counted (SOUND_GENERIC_STALE) and forgotten. A
    // reset forgets without a call.
    struct SoundLinks
    {
        virtual ~SoundLinks() = default;
        // The identity of a live sound, 0 when freed or reused.
        virtual std::uint64_t Identity(std::uint64_t sound) const = 0;
        // S13 on a sound whose identity was just checked.
        virtual bool Stop(std::uint64_t sound) = 0;
    };

    struct LinkedSound
    {
        std::uint64_t sound = 0;
        std::uint64_t identity = 0; // 0 with a sound: gone (born dead or seen gone)
    };

    struct StaleCounters
    {
        std::uint64_t linked = 0;         // births with an identity
        std::uint64_t bornGone = 0;       // births already gone when drained
        std::uint64_t seenGone = 0;       // identity broke while held (the engine ended it)
        std::uint64_t stops = 0;          // S13 on a checked sound
        std::uint64_t stopSkipped = 0;    // stop asked on a gone sound: no call
        std::uint64_t netStops = 0;       // the safety net's S13
        std::uint64_t netSkipped = 0;     // ... on gone sounds: no call
        std::uint64_t unboundSkipped = 0; // a birth nobody waits for, already gone: no call
        std::uint64_t adjustSkipped = 0;  // an adjust on a gone sound: no call
        std::uint64_t forgotten = 0;      // dropped by a reset without a call
        // The engine calls that would have been made on a freed sound.
        std::uint64_t Stale() const { return stopSkipped + netSkipped + unboundSkipped + adjustSkipped; }
    };

    inline void Link(LinkedSound& link, std::uint64_t sound, const SoundLinks& links, StaleCounters& counters)
    {
        link.sound = sound;
        link.identity = sound ? links.Identity(sound) : 0;
        if (!sound)
            return;
        ++(link.identity ? counters.linked : counters.bornGone);
    }

    // The sound while it is still the one linked, else 0 (no engine call).
    inline std::uint64_t LiveSound(const LinkedSound& link, const SoundLinks& links)
    {
        if (!link.sound || !link.identity)
            return 0;
        return links.Identity(link.sound) == link.identity ? link.sound : 0;
    }

    // LiveSound, and a broken identity is remembered and counted once.
    inline std::uint64_t CheckLinked(LinkedSound& link, const SoundLinks& links, StaleCounters& counters)
    {
        const std::uint64_t sound = LiveSound(link, links);
        if (!sound && link.sound && link.identity)
        {
            link.identity = 0;
            ++counters.seenGone;
        }
        return sound;
    }

    // Stops the linked sound if it is still the same, and lets go of it
    // either way (a second stop finds nothing).
    inline bool StopLinked(LinkedSound& link, SoundLinks& links, StaleCounters& counters)
    {
        bool stopped = false;
        if (const std::uint64_t sound = LiveSound(link, links))
        {
            stopped = links.Stop(sound);
            if (stopped)
                ++counters.stops;
        }
        else if (link.sound)
            ++counters.stopSkipped;
        link = LinkedSound{};
        return stopped;
    }

    // The safety net's sounds (bound or forgotten by their packet): stopped
    // at the end only while each is still the same sound.
    class LinkedSet
    {
    public:
        void Insert(const LinkedSound& link)
        {
            if (link.sound && link.identity)
                m_live[link.sound] = link.identity;
        }
        void Erase(std::uint64_t sound) { m_live.erase(sound); }
        std::size_t Size() const { return m_live.size(); }
        void StopAll(SoundLinks& links, StaleCounters& counters)
        {
            for (const auto& entry : m_live)
            {
                LinkedSound link{entry.first, entry.second};
                if (LiveSound(link, links))
                {
                    if (links.Stop(entry.first))
                        ++counters.netStops;
                }
                else
                    ++counters.netSkipped;
            }
            m_live.clear();
        }
        // A reset (save loaded, scripts restarted): no engine call.
        void Forget(StaleCounters& counters)
        {
            counters.forgotten += m_live.size();
            m_live.clear();
        }

    private:
        std::map<std::uint64_t, std::uint64_t> m_live;
    };

    // A birth no request waits for: stopped now if it is still alive.
    inline bool StopUnbound(std::uint64_t sound, SoundLinks& links, StaleCounters& counters)
    {
        if (!sound)
            return false;
        if (!links.Identity(sound))
        {
            ++counters.unboundSkipped;
            return false;
        }
        const bool stopped = links.Stop(sound);
        if (stopped)
            ++counters.netStops;
        return stopped;
    }

    inline std::string StaleLine(const char* why, const StaleCounters& c)
    {
        char text[320]{};
        std::snprintf(text, sizeof(text),
            "SOUND_GENERIC_STALE,why=%s,stale=%llu,linked=%llu,born_gone=%llu,seen_gone=%llu,stops=%llu,stop_skipped=%llu,"
            "net_stops=%llu,net_skipped=%llu,unbound_skipped=%llu,adjust_skipped=%llu,forgotten=%llu",
            why, static_cast<unsigned long long>(c.Stale()), static_cast<unsigned long long>(c.linked),
            static_cast<unsigned long long>(c.bornGone), static_cast<unsigned long long>(c.seenGone),
            static_cast<unsigned long long>(c.stops), static_cast<unsigned long long>(c.stopSkipped),
            static_cast<unsigned long long>(c.netStops), static_cast<unsigned long long>(c.netSkipped),
            static_cast<unsigned long long>(c.unboundSkipped), static_cast<unsigned long long>(c.adjustSkipped),
            static_cast<unsigned long long>(c.forgotten));
        return text;
    }

    // What the engine side provides (sound_generic_player.cpp); a fake in the tests.
    struct Engine
    {
        virtual ~Engine() = default;
        // Creates the packet's sound on its owner (or at its position) with
        // its init params; plays it when `play`. True with the handle. False
        // with `retry` when it may work next tick (puppet still coming, no
        // slot yet), else refused for good (`status` says why).
        virtual bool Create(const CreatePacket& packet, bool play, std::uint64_t& handle, std::string& status, bool& retry) = 0;
        // The recorded life of the sound the next Create makes (kNeverMs: no
        // recorded end, a loop), for the slot policy.
        virtual void NoteNextLife(std::uint32_t lifeMs) { (void)lifeMs; }
        // S10 on a created sound (prepare, may load).
        virtual bool Prepare(std::uint64_t handle) = 0;
        // S11 on a created sound (prepare and play).
        virtual bool Play(std::uint64_t handle) = 0;
        // S13 for Stop (the sound keeps its slot until the engine ends it),
        // S14 for StopAndForget and Release (the slot is free afterwards).
        virtual void Stop(std::uint64_t handle, EndReason reason) = 0;
        // The engine still holds the sound (false once it ended by itself).
        virtual bool Alive(std::uint64_t handle) = 0;
        virtual void Adjust(std::uint64_t handle, const AdjustPacket& packet) = 0;
        // The verdict of the last Adjust ("ok" or why not; empty when the
        // engine side does not say), and what the create of a live handle
        // carried (its init variables and where they came from), for
        // SoundGenericTrace.
        virtual const char* AdjustResult() const { return ""; }
        virtual std::string CreateDetail(std::uint64_t) const { return std::string(); }
        // The family of the packet the next Create makes
        // (SoundGenericEntityMode applies to two of them), and how a live handle
        // ended or is ending (mode, life, the engine side's reason) for the
        // trace's end lines.
        virtual void SetCreateFamily(soundowner::Family) {}
        virtual std::string EndDetail(std::uint64_t) const { return std::string(); }
        // The safety net of StopAll (pause, scrub, jump, the end): every sound
        // the engine side made through the game's queue (mode c) stops, bound to
        // a packet or not.
        virtual void StopAllQueued() {}
        // A recorded release (S15) is the engine giving the slot back, not a
        // stop: the player forgets the handle and the engine side lets the sound
        // end by itself (the safety net still stops it). And mode c's lead: how
        // much ahead of the packet's time the queue is asked (0 when the packet
        // is not queued).
        virtual void Forget(std::uint64_t) {}
        // Mode c's request whose sound the queue has not made yet.
        virtual bool AwaitsBirth(std::uint64_t) const { return false; }
        virtual std::uint32_t LeadMs(const CreatePacket&, soundowner::Family) const { return 0; }
        // The anti-double rule: another source (the puppet's own animation,
        // the live world, another track) made this owner's sound around
        // the packet's time.
        virtual bool MadeByOther(const CreatePacket& packet, std::uint32_t nowMs) = 0;
        // For a complement only, after MadeByOther said no: the owner made a
        // sound of the packet's sound set in another entry around its time (the
        // track's call made that layer with the puppet's own choice).
        virtual bool MadeSameSetByOther(const CreatePacket& packet, std::uint32_t nowMs)
        {
            (void)packet;
            (void)nowMs;
            return false;
        }
        // SoundGenericKeepLoops: whether a live sound may go on across a jump
        // (the engine side: still held, made on the same puppet that owns the
        // packet now, not a queued request).
        virtual bool KeepAcrossJump(std::uint64_t) { return false; }
    };

    // A packet with no recorded owner and no position has nothing to be
    // placed on (SkipReason::Unplaced).
    inline bool UnplacedPacket(const CreatePacket& packet)
    {
        return packet.owner == census::kNoOwner && !packet.hasPosition;
    }

    // ------------------------------------------------------------ doubles

    // Starts of the generic player against the sounds other sources made:
    // the same key (owner, sound set) within the tolerance is a double. One
    // double per start; both orders (the other first or the start first).
    class DoubleDetector
    {
    public:
        void Begin(std::uint32_t toleranceMs)
        {
            m_tolerance = toleranceMs;
            m_starts.clear();
            m_others.clear();
        }

        // The sound is its set and entry (soundowner::SoundKey, 40 bits), mixed
        // with the owner.
        static std::uint64_t KeyOf(std::uint32_t owner, std::uint64_t ref)
        {
            return (static_cast<std::uint64_t>(owner) * 0x9E3779B97F4A7C15ull) ^ soundowner::SoundKey(ref);
        }

        // Who made the other copy (the census row's creator frames, module
        // relative), for the double's evidence line.
        static constexpr std::size_t kSourceFrames = 3;
        struct Source
        {
            std::uint32_t frames[kSourceFrames] = {};
            std::uint32_t timeMs = 0;
            bool mine = false; // made inside a mod scope
            std::uint64_t audio = 0;   // the audio entity it was made on
            std::uint8_t base = 0;     // the owner range's audio base (census::AudioBase)
            std::uint16_t field = 0;
        };

        // A start of the generic player: true when another source already
        // made the same sound within the tolerance (`source`: that one).
        bool NoteStart(std::uint64_t key, std::uint32_t timeMs, Source* source = nullptr)
        {
            Prune(timeMs);
            bool doubled = false;
            for (const Entry& other : m_others)
            {
                if (other.key == key && Near(other.timeMs, timeMs))
                {
                    doubled = true;
                    if (source)
                        *source = other.source;
                    break;
                }
            }
            m_starts.push_back({key, timeMs, doubled, {}});
            return doubled;
        }

        // A sound another source made: true when a start of the generic
        // player within the tolerance was not yet counted as a double.
        bool NoteOther(std::uint64_t key, std::uint32_t timeMs, const Source& source = {})
        {
            Prune(timeMs);
            m_others.push_back({key, timeMs, false, source});
            for (Entry& start : m_starts)
            {
                if (!start.doubled && start.key == key && Near(start.timeMs, timeMs))
                {
                    start.doubled = true;
                    return true;
                }
            }
            return false;
        }

        std::size_t Starts() const { return m_starts.size(); }
        std::size_t Others() const { return m_others.size(); }

    private:
        struct Entry
        {
            std::uint64_t key;
            std::uint32_t timeMs;
            bool doubled;
            Source source;
        };

        bool Near(std::uint32_t a, std::uint32_t b) const
        {
            return (a > b ? a - b : b - a) <= m_tolerance;
        }

        void Prune(std::uint32_t nowMs)
        {
            // A jump back makes everything "newer than now": dropped too.
            const auto stale = [&](const Entry& e) { return e.timeMs > nowMs + m_tolerance || nowMs - e.timeMs > m_tolerance * 4; };
            m_starts.erase(std::remove_if(m_starts.begin(), m_starts.end(), stale), m_starts.end());
            m_others.erase(std::remove_if(m_others.begin(), m_others.end(), stale), m_others.end());
        }

        std::uint32_t m_tolerance = 250;
        std::vector<Entry> m_starts;
        std::vector<Entry> m_others;
    };

    // ------------------------------------------------------------ player

    class Player
    {
    public:
        // Builds the plans from the track: the owner table per packet
        // (cover from the clip's tracks, the ini overrides), the ends and
        // adjusts per sound, the preload flag. `lastMs`: the playhead the
        // playback starts at (the first tick is a resume from there).
        // `feet`, the recorded foot events (packets mode: a step packet outside
        // them is the generic player's; null: none); `trace`, the
        // SoundGenericTrace window.
        void Begin(const Track& track, const Config& config, const soundowner::Cover& cover,
            const soundowner::Overrides& overrides, std::uint32_t lastMs, const soundowner::FootWindows* feet = nullptr,
            const TraceWindow& trace = TraceWindow(), const soundowner::ShotWindows* shots = nullptr)
        {
            m_track = &track;
            m_config = config;
            m_stats = PlayStats{};
            m_tally.clear();
            m_trace = trace;
            m_traceLines.clear();
            m_traceDropped = 0;
            m_traceWritten = 0;
            m_plans.clear();
            m_plans.resize(track.creates.size());
            for (std::uint32_t n = 0; n < track.creates.size(); ++n)
            {
                const CreatePacket& c = track.creates[n];
                SoundPlan& plan = m_plans[n];
                plan.create = n;
                plan.startMs = c.timeMs;
                plan.preload = config.preloadMs > 0 && c.createDelayMs >= config.preloadDelayMs;
                soundowner::PacketKey key;
                key.owner = c.owner;
                key.cls = c.ownerClass;
                key.stableId = c.stableId;
                key.rawRef = c.rawRef;
                key.caller = c.caller;
                key.timeMs = c.timeMs;
                key.audioKnown = c.audioKnown;
                key.audioBase = c.audioBase;
                // `shots`, the recorded shots (a weapon audio packet of no named caller
                // away from them is not the shot track's; null: no shot filter).
                key.callerInWrappers = InAudioEntityWrappers(c.caller);
                plan.family = soundowner::FamilyOf(key, cover, shots);
                plan.taker = soundowner::TakerOf(plan.family, overrides);
                if (plan.family == soundowner::Family::Owned && soundowner::OffShot(key, shots))
                {
                    plan.offShot = true;
                    ++m_stats.offShot;
                }
                // Packets mode, the step exception covers only
                // what the step track remakes (a packet near a recorded foot
                // event of its owner); a family the ini set keeps the ini's taker.
                if (config.packets && soundowner::StepFamily(plan.family) && plan.taker == soundowner::Taker::Specific &&
                    !overrides.set[static_cast<std::uint8_t>(plan.family)] && !(feet && feet->Covers(c.owner, c.timeMs)))
                {
                    plan.taker = soundowner::Taker::Generic;
                    plan.offStep = true;
                    ++m_stats.offStep;
                }
                // The rest of a person's step and melee
                // packets, the complement of the track's engine call.
                if (config.packets && soundowner::ComplementFamily(plan.family, c.ownerClass) &&
                    plan.taker == soundowner::Taker::Specific && !overrides.set[static_cast<std::uint8_t>(plan.family)])
                {
                    plan.taker = soundowner::Taker::Generic;
                    plan.complement = true;
                    plan.preload = false; // decided after the call, never prepared ahead
                    ++m_stats.complement;
                }
                plan.traced = m_trace.Holds(c.owner, c.timeMs);
                plan.entityDriven = (c.params.flags & kParamsFlagUpdateEntity) != 0 &&
                    !(config.detachedEntityMode && EntityModeApplies(plan.family, c.owner));
                if (plan.entityDriven && plan.taker == soundowner::Taker::Generic)
                    ++m_stats.entityDriven;
                // Packets mode.
                if (config.packets && plan.taker == soundowner::Taker::Generic && c.owner != census::kNoOwner)
                {
                    // The ports refuse the own sounds of peds and horses
                    // only (their audio holders): an object's or a weapon's
                    // own sound may still come (a vehicle's too with
                    // SoundGenericVehicleBlock=0), so those keep the hold
                    // and its anti-double wait.
                    // A complement keeps the hold (its track's call comes first, the
                    // anti-double rule decides); the vehicles' and train cars' own copies
                    // are refused with SoundGenericVehicleBlock.
                    plan.noHold = !plan.complement && vehiclesound::OwnCopiesRefused(c.ownerClass, config.vehicleBlock);
                    if (!plan.offStep && !plan.complement && soundowner::DefaultTaker(plan.family) != soundowner::Taker::Generic)
                        ++m_stats.fromTracks;
                }
                if (!config.enabled)
                    plan.taker = soundowner::Taker::Puppet; // counted as disabled below
                ++m_stats.packets;
                ++m_stats.families[static_cast<std::uint8_t>(plan.family)].packets;
                if (plan.taker == soundowner::Taker::Generic)
                    ++m_stats.generic;
            }
            // Time order (the clip reader enforces it; a track still in
            // memory may not): the due, preload and resume walks rely on it.
            std::stable_sort(m_plans.begin(), m_plans.end(),
                [](const SoundPlan& a, const SoundPlan& b) { return a.startMs < b.startMs; });
            // Packet index -> plan, sparse: one entry per create (the indices
            // are the session's, a trimmed clip's can be far apart), never a
            // table of nextIndex entries. The last create of an index wins, as
            // a dense table filled in plan order did.
            m_byIndex.clear();
            m_byIndex.reserve(m_plans.size());
            for (std::uint32_t n = 0; n < m_plans.size(); ++n)
            {
                const CreatePacket& c = track.creates[m_plans[n].create];
                if (c.index < track.nextIndex)
                    m_byIndex.emplace_back(c.index, n);
            }
            std::stable_sort(m_byIndex.begin(), m_byIndex.end(),
                [](const std::pair<std::uint32_t, std::uint32_t>& a, const std::pair<std::uint32_t, std::uint32_t>& b) {
                    return a.first < b.first;
                });
            const auto planOf = [this](std::uint32_t index) {
                const auto after = std::upper_bound(m_byIndex.begin(), m_byIndex.end(), index,
                    [](std::uint32_t value, const std::pair<std::uint32_t, std::uint32_t>& entry) { return value < entry.first; });
                return after == m_byIndex.begin() || std::prev(after)->first != index ? kNoPlan : std::prev(after)->second;
            };
            for (std::uint32_t n = 0; n < track.adjusts.size(); ++n)
            {
                const std::uint32_t plan = planOf(track.adjusts[n].index);
                if (plan != kNoPlan)
                    m_plans[plan].adjusts.push_back(n);
            }
            for (SoundPlan& plan : m_plans)
                plan.startValues = StartValues(track, plan.startMs, plan.adjusts);
            for (std::uint32_t n = 0; n < track.ends.size(); ++n)
            {
                const EndPacket& e = track.ends[n];
                const std::uint32_t planIndex = planOf(e.index);
                if (planIndex == kNoPlan)
                    continue;
                SoundPlan& plan = m_plans[planIndex];
                plan.ends.push_back(n);
                if (e.reason == EndReason::Release && !plan.released)
                {
                    plan.released = true;
                    plan.endMs = e.timeMs < plan.startMs ? plan.startMs : e.timeMs;
                }
            }
            m_doubles.Begin(config.doubleToleranceMs);
            m_doubleEvidence.clear();
            m_doubleEvidenceTaken = 0;
            m_lastMs = lastMs;
            m_resume = true;
            m_next = 0;
            m_preloadNext = 0;
            m_pending.clear();
            m_live.clear();
            m_complementEvidence.clear();
            m_began = true;
        }

        bool Active() const { return m_began && m_track != nullptr; }
        // The first complements played (owner, sound, time).
        const std::vector<ComplementEvidence>& Complements() const { return m_complementEvidence; }
        const PlayStats& Stats() const { return m_stats; }
        PlayStats& StatsMutable() { return m_stats; }
        const std::vector<SoundPlan>& Plans() const { return m_plans; }
        std::size_t Live() const { return m_live.size(); }

        // The generic packets of the vehicles and train cars into `tally` (its
        // plan counts are redone): how the last pass ended (played, yielded,
        // missed, refused) and the doubles (another source's copy first).
        void TallyVehicles(vehiclesound::VehicleTally& tally) const
        {
            tally.ClearPlans();
            if (!m_track)
                return;
            for (const SoundPlan& plan : m_plans)
            {
                if (plan.create >= m_track->creates.size() || plan.taker != soundowner::Taker::Generic)
                    continue;
                const CreatePacket& c = m_track->creates[plan.create];
                tally.NotePlan(c.owner, c.ownerClass, plan.lastOutcome == Outcome::Played, plan.lastOutcome == Outcome::Yielded,
                    plan.lastOutcome == Outcome::Missed, plan.lastOutcome == Outcome::Refused, plan.doubled);
            }
        }

        // The packets whose time is in [fromMs, toMs] and what the latest pass
        // did with them (SoundPlan::lastOutcome), by family (with the taker that
        // owns the family: who was supposed to play it), by (owner, family), and
        // the failures (refused, missed, doubled) one by one. A packet not
        // reached yet counts as "none".
        WindowReport Window(std::uint32_t fromMs, std::uint32_t toMs, const soundowner::Overrides& overrides) const
        {
            WindowReport out;
            out.fromMs = fromMs;
            out.toMs = toMs;
            WindowFamily families[soundowner::kFamilyCount];
            for (std::uint8_t n = 0; n < soundowner::kFamilyCount; ++n)
            {
                families[n].family = static_cast<soundowner::Family>(n);
                families[n].taker = soundowner::TakerOf(families[n].family, overrides);
            }
            if (!m_track)
                return out;
            for (const SoundPlan& plan : m_plans)
            {
                if (plan.startMs < fromMs)
                    continue;
                if (plan.startMs > toMs)
                    break; // time order
                const CreatePacket& c = m_track->creates[plan.create];
                const std::uint8_t outcome = static_cast<std::uint8_t>(plan.lastOutcome);
                ++out.packets;
                ++out.counts[outcome];
                out.doubles += plan.doubled ? 1 : 0;
                WindowFamily& family = families[static_cast<std::uint8_t>(plan.family)];
                ++family.packets;
                ++family.counts[outcome];
                family.doubles += plan.doubled ? 1 : 0;
                WindowOwner* row = nullptr;
                for (WindowOwner& candidate : out.owners)
                {
                    if (candidate.owner == c.owner && candidate.family == plan.family)
                    {
                        row = &candidate;
                        break;
                    }
                }
                if (!row)
                {
                    WindowOwner fresh;
                    fresh.owner = c.owner;
                    fresh.cls = c.ownerClass;
                    fresh.family = plan.family;
                    fresh.taker = plan.taker;
                    out.owners.push_back(fresh);
                    row = &out.owners.back();
                }
                ++row->packets;
                ++row->counts[outcome];
                row->doubles += plan.doubled ? 1 : 0;
                const bool failed = plan.lastOutcome == Outcome::Refused || plan.lastOutcome == Outcome::Missed || plan.doubled;
                if (failed && out.failures.size() < kWindowFailures)
                {
                    WindowFailure failure;
                    failure.timeMs = plan.startMs;
                    failure.owner = c.owner;
                    failure.family = plan.family;
                    failure.taker = plan.taker;
                    failure.set = static_cast<std::uint32_t>(census::RefLow(c.stableId));
                    failure.outcome = plan.lastOutcome;
                    failure.doubled = plan.doubled;
                    out.failures.push_back(failure);
                }
            }
            for (const WindowFamily& family : families)
                if (family.packets)
                    out.families.push_back(family);
            std::stable_sort(out.owners.begin(), out.owners.end(),
                [](const WindowOwner& a, const WindowOwner& b) { return a.packets > b.packets; });
            if (out.owners.size() > kWindowOwners)
                out.owners.resize(kWindowOwners);
            return out;
        }
        std::size_t Pending() const { return m_pending.size(); }
        DoubleDetector& Doubles() { return m_doubles; }
        // The doubles with the other copy's creator.
        struct DoubleEvidence
        {
            std::uint32_t timeMs = 0;
            std::uint32_t owner = 0;
            std::uint64_t ref = 0;
            soundowner::Family family = soundowner::Family::Owned;
            DoubleDetector::Source other;
        };
        static constexpr std::size_t kDoubleEvidenceMax = 80;
        std::vector<DoubleEvidence> TakeDoubleEvidence()
        {
            std::vector<DoubleEvidence> out;
            out.swap(m_doubleEvidence);
            m_doubleEvidenceTaken += out.size();
            return out;
        }
        // The per-sound tally of this playback, and the trace
        // lines written since the last call (the engine side logs them).
        const SoundTally& Tally() const { return m_tally; }
        std::vector<std::string> TakeTrace()
        {
            std::vector<std::string> out;
            out.swap(m_traceLines);
            return out;
        }
        std::uint32_t TraceDropped() const { return m_traceDropped; }
        const Config& Configured() const { return m_config; }

        // Stops every playing or preloaded sound (pause, scrub, jump, the end).
        void StopAll(Engine& engine)
        {
            engine.StopAllQueued(); // first, then the plans (their handles stay answered)
            if (m_live.empty() && m_pending.empty())
                return; // nothing to stop (a paused playback ticks every frame)
            for (const std::uint32_t n : m_live)
            {
                SoundPlan& plan = m_plans[n];
                if (plan.handle != kNoHandle)
                    engine.Stop(plan.handle, EndReason::StopAndForget);
                plan.handle = kNoHandle;
                plan.state = SoundState::Idle;
                plan.kept = false;
            }
            m_live.clear();
            for (const std::uint32_t n : m_pending)
            {
                SoundPlan& plan = m_plans[n];
                if (plan.state == SoundState::Preloaded && plan.handle != kNoHandle)
                    engine.Stop(plan.handle, EndReason::StopAndForget);
                plan.handle = kNoHandle;
                plan.state = SoundState::Idle;
            }
            m_pending.clear();
            ++m_stats.stopAll;
        }

        // Turns the player off or on in the middle of a playback (an export
        // starts with the playback already running). Off: the live and preloaded
        // sounds stop now and every packet due from here is counted as disabled,
        // as with SoundGenericPlay=0. On again: the packets due from here play.
        // True when the state changed.
        bool SetEnabled(bool enabled, Engine& engine)
        {
            if (m_config.enabled == enabled)
                return false;
            m_config.enabled = enabled;
            if (!enabled)
                StopAll(engine);
            return true;
        }

        // One playback tick at `nowMs`.
        void Tick(std::uint32_t nowMs, bool normalPlay, Engine& engine)
        {
            if (!Active())
                return;
            const std::uint32_t lastMs = m_lastMs;
            const bool step = normalPlay && nowMs >= lastMs && nowMs - lastMs <= m_config.maxStepMs;
            m_lastMs = nowMs;
            ++m_ticks; // the complement's wait
            if (!step)
            {
                // A jump in normal play keeps the loops that cover the new playhead.
                if (normalPlay && m_config.keepLoops)
                    StopAllButCoveringLoops(nowMs, engine);
                else
                    StopAll(engine);
                m_resume = true;
                return;
            }
            if (m_resume)
                Resume(lastMs, nowMs);
            // New packets due (their time inside this step).
            // Mode c's lead admits them up to kQueueLeadMaxMs early (Start waits for each one's own lead).
            while (m_next < m_plans.size() && m_plans[m_next].startMs <= nowMs + kQueueLeadMaxMs)
            {
                SoundPlan& plan = m_plans[m_next];
                if (plan.state == SoundState::Idle || plan.state == SoundState::Preloaded)
                {
                    if (plan.state == SoundState::Idle)
                        plan.stateMs = nowMs;
                    if (std::find(m_pending.begin(), m_pending.end(), m_next) == m_pending.end())
                        m_pending.push_back(static_cast<std::uint32_t>(m_next));
                }
                ++m_next;
            }
            Preload(nowMs, engine);
            Start(nowMs, engine);
            Drive(nowMs, engine);
            m_stats.livePeak = (std::max)(m_stats.livePeak, static_cast<std::uint32_t>(m_live.size()));
        }

        // A script restart (a save loaded) with the playback open: the live and
        // pending plans let go without the engine (their handles are the old
        // world's), the player inactive.
        void Abandon()
        {
            for (const std::uint32_t n : m_live)
            {
                m_plans[n].handle = kNoHandle;
                m_plans[n].state = SoundState::Idle;
            }
            for (const std::uint32_t n : m_pending)
            {
                m_plans[n].handle = kNoHandle;
                m_plans[n].state = SoundState::Idle;
            }
            m_live.clear();
            m_pending.clear();
            m_began = false;
        }

        // The end of the playback: everything stopped, the stats closed.
        void End(Engine& engine)
        {
            if (!Active())
                return;
            StopAll(engine);
            m_began = false;
        }

        // Whether a live plan may go on at
        // `nowMs` after a jump (pure: the recorded life covers it).
        bool CoversAfterJump(const SoundPlan& plan, std::uint32_t nowMs) const
        {
            if (plan.state != SoundState::Playing || plan.handle == kNoHandle || plan.stopped || plan.awaitingBirth ||
                !plan.Long(m_config.loopMs) || plan.startMs > nowMs || (plan.released && plan.endMs <= nowMs))
                return false;
            if (m_track)
                for (const std::uint32_t e : plan.ends)
                    if (e < m_track->ends.size() && m_track->ends[e].timeMs <= nowMs)
                        return false; // a stop or forget at or before the new playhead
            return true;
        }

    private:
        static constexpr std::uint32_t kNoPlan = 0xFFFFFFFFu;

        // StopAll but the live long sounds whose
        // recorded life covers the new playhead and that the engine keeps.
        void StopAllButCoveringLoops(std::uint32_t nowMs, Engine& engine)
        {
            std::vector<std::uint32_t> kept;
            for (std::size_t l = 0; l < m_live.size();)
            {
                SoundPlan& plan = m_plans[m_live[l]];
                if (CoversAfterJump(plan, nowMs) && engine.KeepAcrossJump(plan.handle))
                {
                    plan.kept = true;
                    kept.push_back(m_live[l]);
                    m_live.erase(m_live.begin() + static_cast<std::ptrdiff_t>(l));
                    continue;
                }
                ++l;
            }
            StopAll(engine);
            m_live = kept;
            m_stats.keptLoops += static_cast<std::uint32_t>(kept.size());
        }

        // One outcome per packet in the stats: the first one (a scrub walks
        // the plans again; the resumed starts are counted apart).
        void Skip(SoundPlan& plan, SkipReason reason)
        {
            plan.state = SoundState::Skipped;
            plan.skip = reason;
            plan.lastOutcome = OutcomeOfSkip(reason); // kept across resumes
            Note(plan, OutcomeOfSkip(reason), m_lastMs); // every pass
            if (plan.counted)
                return;
            plan.counted = true;
            ++m_stats.skippedByReason[static_cast<std::uint8_t>(reason)];
            FamilyStats& family = m_stats.families[static_cast<std::uint8_t>(plan.family)];
            switch (reason)
            {
            case SkipReason::Specific: ++m_stats.skippedSpecific; ++family.skipped; break;
            case SkipReason::Puppet: ++m_stats.skippedPuppet; ++family.skipped; break;
            case SkipReason::Disabled: ++family.skipped; break;
            case SkipReason::Yielded: ++m_stats.yielded; ++family.yielded; break;
            case SkipReason::Refused: ++m_stats.refused; ++family.refused; break;
            case SkipReason::Missed: ++m_stats.missed; ++family.missed; break;
            case SkipReason::Jumped: ++m_stats.jumped; ++family.jumped; break;
            case SkipReason::Unplaced: ++m_stats.unplaced; ++family.unplaced; break;
            case SkipReason::None: break;
            }
        }

        // The first step after a pause, scrub, jump or the start: the
        // sounds alive at the playhead come back with their state; short
        // ones whose time went by are left out.
        void Resume(std::uint32_t lastMs, std::uint32_t nowMs)
        {
            m_resume = false;
            m_pending.clear();
            m_next = 0;
            m_preloadNext = 0;
            for (SoundPlan& plan : m_plans)
            {
                // A loop kept across the jump
                // stays live; its adjusts are walked again up to the playhead.
                if (plan.kept && plan.state == SoundState::Playing && plan.handle != kNoHandle)
                {
                    plan.resumed = true;
                    plan.adjustCursor = 0;
                    plan.endCursor = 0;
                    plan.shiftMs = 0;
                    continue;
                }
                plan.kept = false;
                plan.state = SoundState::Idle;
                plan.skip = SkipReason::None;
                plan.handle = kNoHandle;
                plan.stopped = false;
                plan.resumed = false;
                plan.adjustCursor = 0;
                plan.endCursor = 0;
                plan.shiftMs = 0;
                plan.complementSeen = false; // a new pass waits for its own call again
            }
            for (std::uint32_t n = 0; n < m_plans.size(); ++n)
            {
                SoundPlan& plan = m_plans[n];
                if (plan.startMs > lastMs)
                    break; // the rest is ahead: the normal path from here
                m_next = n + 1;
                m_preloadNext = n + 1;
                if (plan.kept) // still live in m_live
                {
                    plan.kept = false;
                    continue;
                }
                // Behind the playhead.
                const bool alive = plan.endMs > nowMs;
                if (alive && plan.Long(m_config.loopMs))
                {
                    plan.resumed = true;
                    plan.stateMs = nowMs;
                    m_pending.push_back(n);
                }
                else if (plan.taker == soundowner::Taker::Generic && alive)
                    Skip(plan, SkipReason::Jumped);
                else if (plan.taker == soundowner::Taker::Generic)
                    plan.state = SoundState::Ended; // over before the playhead: nothing to do
                else
                    SkipForTaker(plan);
            }
        }

        bool SkipForTaker(SoundPlan& plan)
        {
            if (!m_config.enabled)
            {
                Skip(plan, SkipReason::Disabled);
                return true;
            }
            if (plan.taker == soundowner::Taker::Specific)
            {
                Skip(plan, SkipReason::Specific);
                return true;
            }
            if (plan.taker == soundowner::Taker::Puppet)
            {
                Skip(plan, SkipReason::Puppet);
                return true;
            }
            return false;
        }

        // Sounds that needed loading are created early and prepared.
        void Preload(std::uint32_t nowMs, Engine& engine)
        {
            while (m_preloadNext < m_plans.size())
            {
                SoundPlan& plan = m_plans[m_preloadNext];
                const std::uint32_t lead = plan.startMs > m_config.preloadMs ? plan.startMs - m_config.preloadMs : 0;
                if (lead > nowMs)
                    break;
                const std::uint32_t n = static_cast<std::uint32_t>(m_preloadNext++);
                if (!plan.preload || plan.state != SoundState::Idle || plan.startMs <= nowMs)
                    continue; // not a preload, or already due (the normal path)
                if (SkipForTaker(plan))
                    continue;
                if (UnplacedPacket(m_track->creates[plan.create]))
                {
                    Skip(plan, SkipReason::Unplaced);
                    continue;
                }
                std::string status;
                bool retry = false;
                std::uint64_t handle = kNoHandle;
                engine.SetCreateFamily(plan.family);
                engine.NoteNextLife(plan.released ? plan.endMs - plan.startMs : kNeverMs);
                if (engine.Create(m_track->creates[plan.create], false, handle, status, retry))
                {
                    plan.handle = handle;
                    plan.state = SoundState::Preloaded;
                    plan.preloadedOnce = true;
                    engine.Prepare(handle);
                    ++m_stats.preloaded;
                    ++m_stats.families[static_cast<std::uint8_t>(plan.family)].preloaded;
                    if (std::find(m_pending.begin(), m_pending.end(), n) == m_pending.end())
                        m_pending.push_back(n);
                }
                else if (!retry)
                    Refuse(plan, status);
                // retry: the normal path tries again at its time
            }
        }

        // The due packets: hold, the anti-double rule, the start.
        void Start(std::uint32_t nowMs, Engine& engine)
        {
            for (std::size_t p = 0; p < m_pending.size();)
            {
                const std::uint32_t n = m_pending[p];
                SoundPlan& plan = m_plans[n];
                const CreatePacket& packet = m_track->creates[plan.create];
                if (plan.state != SoundState::Idle && plan.state != SoundState::Preloaded)
                {
                    m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(p));
                    continue;
                }
                // Mode c is asked ahead by the
                // queue's measured latency, so its sound is born on time.
                const std::uint32_t leadMs = plan.resumed ? 0u : (std::min)(engine.LeadMs(packet, plan.family), kQueueLeadMaxMs);
                const std::uint32_t aheadMs = nowMs + leadMs;
                if (plan.startMs > aheadMs)
                {
                    ++p; // preloaded or not yet due
                    continue;
                }
                if (SkipForTaker(plan))
                {
                    m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(p));
                    continue;
                }
                if (UnplacedPacket(packet))
                {
                    Skip(plan, SkipReason::Unplaced);
                    m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(p));
                    continue;
                }
                // The hold: a resumed sound starts at once (its time is long gone).
                const std::uint32_t dueMs = plan.resumed ? plan.stateMs : plan.startMs + (plan.noHold ? 0u : m_config.holdMs);
                if (aheadMs < dueMs)
                {
                    ++p;
                    continue;
                }
                // A complement is decided two ticks after
                // its time was reached at the earliest: the track's call of
                // that tick (the step before this player, the melee hit after
                // it) reaches the census on the next one, and a slow frame
                // does not shorten the wait the hold gives.
                if (plan.complement && !plan.resumed)
                {
                    // The playhead itself, not the queue's lead: the track's
                    // call happens at the recorded time, never ahead of it.
                    if (nowMs < dueMs)
                    {
                        ++p;
                        continue;
                    }
                    if (!plan.complementSeen)
                    {
                        plan.complementSeen = true;
                        plan.complementTick = m_ticks;
                    }
                    if (m_ticks < plan.complementTick + kComplementTicks)
                    {
                        ++p;
                        continue;
                    }
                }
                // Its window: a short sound within oneShotWindowMs of its due
                // time, a long one while it lasts.
                const std::uint32_t windowEnd = plan.Long(m_config.loopMs)
                    ? plan.endMs
                    : (std::max)(plan.endMs, dueMs + m_config.oneShotWindowMs);
                if (nowMs >= windowEnd && windowEnd != kNeverMs)
                {
                    if (plan.state == SoundState::Preloaded && plan.handle != kNoHandle)
                        engine.Stop(plan.handle, EndReason::StopAndForget);
                    plan.handle = kNoHandle;
                    Skip(plan, SkipReason::Missed);
                    m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(p));
                    continue;
                }
                // The engine side learns this packet's recorded life before the
                // anti-double rule too (a long world copy that yields is watched with its
                // live twin).
                engine.NoteNextLife(plan.released ? plan.endMs - plan.startMs : kNeverMs);
                bool yieldNow = engine.MadeByOther(packet, nowMs);
                bool sameSet = false;
                if (!yieldNow && plan.complement)
                {
                    sameSet = engine.MadeSameSetByOther(packet, nowMs);
                    yieldNow = sameSet;
                }
                if (yieldNow)
                {
                    if (plan.state == SoundState::Preloaded && plan.handle != kNoHandle)
                        engine.Stop(plan.handle, EndReason::StopAndForget);
                    plan.handle = kNoHandle;
                    if (plan.complement) // the track's call made it
                        ++m_stats.complementYielded;
                    if (sameSet) // in another entry of the set
                        ++m_stats.complementYieldedSet;
                    Skip(plan, SkipReason::Yielded);
                    m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(p));
                    continue;
                }
                bool ok = false;
                bool retry = false;
                std::string refusal = "play_failed";
                if (plan.state == SoundState::Preloaded)
                {
                    ok = engine.Play(plan.handle);
                    if (!ok)
                    {
                        engine.Stop(plan.handle, EndReason::StopAndForget);
                        plan.handle = kNoHandle;
                    }
                }
                else
                {
                    std::string status;
                    std::uint64_t handle = kNoHandle;
                    engine.SetCreateFamily(plan.family);
                    engine.NoteNextLife(plan.released ? plan.endMs - plan.startMs : kNeverMs);
                    ok = engine.Create(packet, true, handle, status, retry);
                    if (ok)
                        plan.handle = handle;
                    else
                        refusal = status;
                }
                if (ok)
                {
                    plan.state = SoundState::Playing;
                    plan.stateMs = nowMs;
                    // A late start keeps the recorded life; a sound resumed mid-span keeps
                    // the recorded times. Packets mode only.
                    plan.shiftMs = m_config.packets && !plan.resumed && aheadMs > plan.startMs ? aheadMs - plan.startMs : 0;
                    if (leadMs)
                        ++m_stats.ledStarts;
                    plan.awaitingBirth = engine.AwaitsBirth(plan.handle);
                    plan.bornDelayMs = 0;
                    plan.lastOutcome = Outcome::Played;
                    plan.seenLater = false;
                    if (plan.complement) // the track's call had not made it
                    {
                        ++m_stats.complementPlayed;
                        if (plan.family == soundowner::Family::Melee)
                            ++m_stats.complementMeleePlayed;
                        if (m_complementEvidence.size() < kComplementEvidenceMax)
                        {
                            ComplementEvidence e;
                            e.owner = packet.owner;
                            e.ref = packet.rawRef ? packet.rawRef : packet.stableId;
                            e.timeMs = plan.startMs;
                            e.lateMs = nowMs > plan.startMs ? nowMs - plan.startMs : 0;
                            e.family = plan.family;
                            e.caller = packet.caller;
                            m_complementEvidence.push_back(e);
                        }
                    }
                    Note(plan, Outcome::Played, nowMs);
                    if (plan.traced)
                        Trace(plan, "create", engine.CreateDetail(plan.handle).c_str(), nowMs);
                    FamilyStats& family = m_stats.families[static_cast<std::uint8_t>(plan.family)];
                    if (!plan.counted)
                    {
                        plan.counted = true;
                        ++m_stats.played;
                        ++family.played;
                    }
                    if (plan.resumed)
                        ++m_stats.resumed;
                    DoubleDetector::Source other;
                    if (m_doubles.NoteStart(DoubleDetector::KeyOf(packet.owner, packet.stableId), nowMs, &other))
                    {
                        ++m_stats.doubles;
                        ++family.doubles;
                        plan.doubled = true;
                        // The evidence of the first ones.
                        if (m_doubleEvidenceTaken + m_doubleEvidence.size() < kDoubleEvidenceMax)
                        {
                            DoubleEvidence e;
                            e.timeMs = nowMs;
                            e.owner = packet.owner;
                            e.ref = packet.rawRef ? packet.rawRef : packet.stableId;
                            e.family = plan.family;
                            e.other = other;
                            m_doubleEvidence.push_back(e);
                        }
                    }
                    // The settings of the time already gone (the hold, or
                    // the whole span of a resumed sound) are applied now
                    // (mode c's once its sound is born).
                    if (!plan.awaitingBirth)
                        ApplyAdjusts(plan, nowMs, engine);
                    m_live.push_back(n);
                    m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(p));
                    continue;
                }
                if (retry)
                {
                    ++p; // next tick, while its window lasts
                    continue;
                }
                Refuse(plan, refusal);
                m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(p));
            }
        }

        // A refusal, with the engine side's status counted once per packet.
        void Refuse(SoundPlan& plan, const std::string& status)
        {
            if (plan.traced)
                Trace(plan, "refused_status", status.c_str(), m_lastMs);
            if (!plan.counted)
                ++m_stats.refusedStatus[status.empty() ? std::string("unknown") : status];
            Skip(plan, SkipReason::Refused);
        }

        // SoundGenericSkip, plus SoundGenericEntityDrivenSkip
        // for the packets their entity drives.
        std::uint32_t SkipMaskOf(const SoundPlan& plan) const
        {
            return m_config.skipMask | (plan.entityDriven ? m_config.entityDrivenMask : 0u);
        }

        void ApplyAdjusts(SoundPlan& plan, std::uint32_t nowMs, Engine& engine)
        {
            while (plan.adjustCursor < plan.adjusts.size())
            {
                const AdjustPacket& due = m_track->adjusts[plan.adjusts[plan.adjustCursor]];
                if (static_cast<std::uint64_t>(due.timeMs) + plan.shiftMs > nowMs)
                    break;
                // StartValues: the create's sample
                // plays with the value the owner set in that frame.
                const AdjustPacket* applied = &due;
                for (const auto& start : plan.startValues)
                    if (start.first == plan.adjusts[plan.adjustCursor])
                    {
                        applied = &m_track->adjusts[start.second];
                        ++m_stats.startValues;
                        break;
                    }
                const AdjustPacket& a = *applied;
                ++plan.adjustCursor;
                const std::uint8_t key = static_cast<std::uint8_t>(a.kind);
                if (plan.handle != kNoHandle && key < kAdjustKinds && (SkipMaskOf(plan) & (1u << key)))
                {
                    ++m_stats.skippedByKey[key];
                    if (plan.traced)
                        Trace(plan, "adjust", AdjustDetail(a, "skipped").c_str(), nowMs);
                }
                else if (plan.handle != kNoHandle)
                {
                    engine.Adjust(plan.handle, a);
                    ++m_stats.adjusts;
                    if (plan.traced) // one line per adjust, with the port's verdict
                        Trace(plan, "adjust", AdjustDetail(a, engine.AdjustResult()).c_str(), nowMs);
                }
                else if (plan.traced)
                    Trace(plan, "adjust", AdjustDetail(a, "no_handle").c_str(), nowMs);
            }
        }

        // When the sound was born against its
        // recorded time and, for an end, its recorded time and the real one.
        static std::string BornDetail(const SoundPlan& plan, std::uint32_t nowMs, std::uint32_t recordedMs = kNeverMs)
        {
            char text[128]{};
            if (recordedMs == kNeverMs)
                std::snprintf(text, sizeof(text), "born_delay_ms=%d,shift_ms=%u", plan.bornDelayMs, plan.shiftMs);
            else
                std::snprintf(text, sizeof(text), "born_delay_ms=%d,recorded_ms=%u,applied_ms=%u", plan.bornDelayMs, recordedMs, nowMs);
            return text;
        }

        // "S38:0x48fe569d=5.514@45574:ok" (port, hash, value,
        // recorded time, verdict).
        static std::string AdjustDetail(const AdjustPacket& a, const char* result)
        {
            char text[160]{};
            if (a.kind == Adjust::Variable)
                std::snprintf(text, sizeof(text), "%s:0x%08x=%.3f@%u:%s", AdjustPort(a.kind), a.hash, a.value[0], a.timeMs,
                    result && *result ? result : "unknown");
            else if (IsVector(a.kind))
                std::snprintf(text, sizeof(text), "%s:%.2f;%.2f;%.2f@%u:%s", AdjustPort(a.kind), a.value[0], a.value[1], a.value[2],
                    a.timeMs, result && *result ? result : "unknown");
            else
                std::snprintf(text, sizeof(text), "%s:%.3f@%u:%s", AdjustPort(a.kind), a.value[0], a.timeMs,
                    result && *result ? result : "unknown");
            return text;
        }

        // The playing sounds: adjusts and ends up to the playhead.
        void Drive(std::uint32_t nowMs, Engine& engine)
        {
            for (std::size_t l = 0; l < m_live.size();)
            {
                SoundPlan& plan = m_plans[m_live[l]];
                bool over = false;
                if (plan.handle == kNoHandle || !engine.Alive(plan.handle))
                {
                    // How it ended (the engine side's account).
                    const std::string why = plan.traced && plan.handle != kNoHandle ? engine.EndDetail(plan.handle)
                        : std::string();
                    // The engine ended it by itself (a one-shot over, a
                    // stopped sound released): its ends are not needed.
                    if (!plan.stopped && (plan.endCursor < plan.ends.size()))
                        ++m_stats.endedByEngine;
                    // Gone before a later tick saw it, and
                    // before any end of its own was applied: created, never
                    // heard. A sound whose recorded life ended by itself (the
                    // release first, already due) ended naturally.
                    bool natural = false;
                    if (!plan.ends.empty())
                    {
                        const EndPacket& first = m_track->ends[plan.ends.front()];
                        natural = first.reason == EndReason::Release &&
                            static_cast<std::uint64_t>(first.timeMs) + plan.shiftMs <= nowMs;
                    }
                    std::uint32_t recordedLife = 0;
                    std::uint32_t lived = 0;
                    if (!plan.seenLater && plan.endCursor == 0 && !plan.stopped && !natural)
                    {
                        ++m_stats.endedAtOnce;
                        Note(plan, Outcome::None, nowMs, true, why.c_str());
                    }
                    else if (!natural && EarlyLife(plan, nowMs, recordedLife, lived))
                    {
                        // Heard, then gone well
                        // before its recorded life.
                        ++m_stats.endedEarly;
                        NoteEarly(plan, nowMs, recordedLife, lived, why);
                    }
                    else if (plan.traced)
                        Trace(plan, "ended_by_engine", why.c_str(), nowMs);
                    over = true;
                }
                else
                {
                    if (nowMs > plan.stateMs)
                        plan.seenLater = true;
                    if (plan.awaitingBirth)
                    {
                        // Mode c, nothing before
                        // the birth; then the recorded adjusts and ends count
                        // from it (a resumed sound keeps the recorded times).
                        if (engine.AwaitsBirth(plan.handle))
                        {
                            ++l;
                            continue;
                        }
                        plan.awaitingBirth = false;
                        plan.bornDelayMs = static_cast<std::int32_t>(static_cast<std::int64_t>(nowMs) - plan.startMs);
                        if (!plan.resumed)
                            plan.shiftMs = nowMs > plan.startMs ? nowMs - plan.startMs : 0;
                        if (plan.traced)
                            Trace(plan, "born", BornDetail(plan, nowMs).c_str(), nowMs);
                    }
                    ApplyAdjusts(plan, nowMs, engine);
                    while (plan.endCursor < plan.ends.size() && !over)
                    {
                        const EndPacket& e = m_track->ends[plan.ends[plan.endCursor]];
                        if (static_cast<std::uint64_t>(e.timeMs) + plan.shiftMs > nowMs)
                            break;
                        ++plan.endCursor;
                        // SoundGenericSkip: the
                        // recorded end is not applied; the sound stays live
                        // (the engine ends it, or the safety net of StopAll).
                        const std::uint8_t skipKey = e.reason == EndReason::Stop ? kSkipStop
                            : e.reason == EndReason::StopAndForget ? kSkipForget : kSkipRelease;
                        if (SkipMaskOf(plan) & (1u << skipKey))
                        {
                            ++m_stats.skippedByKey[skipKey];
                            if (plan.traced)
                                Trace(plan, SkipKeyName(skipKey), "skipped", nowMs);
                            continue;
                        }
                        switch (e.reason)
                        {
                        case EndReason::Stop:
                            if (!plan.stopped)
                            {
                                engine.Stop(plan.handle, EndReason::Stop);
                                plan.stopped = true;
                                ++m_stats.stops;
                                if (plan.traced)
                                    Trace(plan, "stop", BornDetail(plan, nowMs, e.timeMs).c_str(), nowMs);
                            }
                            break;
                        case EndReason::StopAndForget:
                        {
                            const std::string why = plan.traced ? engine.EndDetail(plan.handle) : std::string();
                            engine.Stop(plan.handle, EndReason::StopAndForget);
                            ++m_stats.forgets;
                            if (plan.traced)
                                Trace(plan, "forget", (why + "," + BornDetail(plan, nowMs, e.timeMs)).c_str(), nowMs);
                        }
                            over = true;
                            break;
                        case EndReason::Release:
                        {
                            // The release is the engine giving its slot back, not a stop: the
                            // handle is forgotten and the sound ends by itself (a stop here would
                            // cut it short); StopAll's net still stops it.
                            const std::string why = plan.traced ? engine.EndDetail(plan.handle) : std::string();
                            if (engine.Alive(plan.handle))
                            {
                                engine.Forget(plan.handle);
                                ++m_stats.releases;
                            }
                            if (plan.traced)
                                Trace(plan, "release", (why + ",left_to_engine=1").c_str(), nowMs);
                        }
                            over = true;
                            break;
                        }
                    }
                }
                if (over)
                {
                    plan.handle = kNoHandle;
                    plan.state = SoundState::Ended;
                    plan.stateMs = nowMs;
                    m_live.erase(m_live.begin() + static_cast<std::ptrdiff_t>(l));
                    continue;
                }
                ++l;
            }
        }

        // The recorded life of a plan the engine
        // just ended and how long it lived here, when it ended early.
        bool EarlyLife(const SoundPlan& plan, std::uint32_t nowMs, std::uint32_t& recordedLife, std::uint32_t& lived) const
        {
            if (!m_track || plan.stopped || plan.resumed || plan.ends.empty() || plan.handle == kNoHandle)
                return false;
            const EndPacket& first = m_track->ends[plan.ends.front()];
            if (first.reason != EndReason::Release || first.timeMs <= plan.startMs)
                return false;
            recordedLife = first.timeMs - plan.startMs;
            const std::uint64_t bornAt = static_cast<std::uint64_t>(plan.startMs) + plan.shiftMs;
            lived = plan.awaitingBirth || nowMs <= bornAt ? 0u : static_cast<std::uint32_t>(nowMs - bornAt);
            return EndedEarly(recordedLife, lived);
        }

        void NoteEarly(SoundPlan& plan, std::uint32_t nowMs, std::uint32_t recordedLife, std::uint32_t lived, const std::string& why)
        {
            if (plan.traced)
            {
                char detail[96]{};
                std::snprintf(detail, sizeof(detail), "recorded_life_ms=%u,lived_here_ms=%u%s", recordedLife, lived,
                    why.empty() ? "" : ",");
                Trace(plan, "ended_early", (detail + why).c_str(), nowMs);
            }
            if (!m_track || !plan.noHold || plan.taker != soundowner::Taker::Generic)
                return;
            const CreatePacket& c = m_track->creates[plan.create];
            SoundTallyKey key;
            key.owner = c.owner;
            key.sound = soundowner::PacketSoundKey(c.stableId, c.rawRef);
            SoundTallyEntry& e = m_tally[key];
            if (!e.taken && !e.played && !e.endedAtOnce && !e.endedEarly)
            {
                e.cls = c.ownerClass;
                e.family = plan.family;
                e.ref = c.rawRef ? c.rawRef : c.stableId;
            }
            ++e.endedEarly;
            e.firstShortMs = (std::min)(e.firstShortMs, plan.startMs);
        }

        // One outcome of a pass into the per-sound tally (the
        // owners whose own sounds the ports refuse) and the trace.
        void Note(SoundPlan& plan, Outcome outcome, std::uint32_t nowMs, bool endedAtOnce = false, const char* detail = "")
        {
            if (plan.traced)
                Trace(plan, endedAtOnce ? "ended_at_once" : OutcomeName(outcome), detail, nowMs);
            if (!m_track || !plan.noHold || plan.taker != soundowner::Taker::Generic)
                return;
            if (!endedAtOnce && outcome != Outcome::Played && outcome != Outcome::Yielded && outcome != Outcome::Refused &&
                outcome != Outcome::Missed)
                return; // jumped, unplaced: not the player's shortfall
            const CreatePacket& c = m_track->creates[plan.create];
            SoundTallyKey key;
            key.owner = c.owner;
            key.sound = soundowner::PacketSoundKey(c.stableId, c.rawRef);
            SoundTallyEntry& e = m_tally[key];
            if (!e.taken && !e.played && !e.endedAtOnce)
            {
                e.cls = c.ownerClass;
                e.family = plan.family;
                e.ref = c.rawRef ? c.rawRef : c.stableId;
            }
            if (endedAtOnce)
            {
                ++e.endedAtOnce;
                e.firstShortMs = (std::min)(e.firstShortMs, plan.startMs);
                return;
            }
            ++e.taken;
            e.offStep += plan.offStep ? 1 : 0;
            switch (outcome)
            {
            case Outcome::Played: ++e.played; return;
            case Outcome::Yielded: ++e.yielded; break;
            case Outcome::Refused: ++e.refused; break;
            default: ++e.missed; break;
            }
            e.firstShortMs = (std::min)(e.firstShortMs, plan.startMs);
        }

        void Trace(const SoundPlan& plan, const char* event, const char* detail, std::uint32_t nowMs)
        {
            if (!m_track)
                return;
            if (m_traceWritten >= m_trace.maxLines)
            {
                ++m_traceDropped;
                return;
            }
            ++m_traceWritten;
            const CreatePacket& c = m_track->creates[plan.create];
            char line[640]{};
            std::snprintf(line, sizeof(line),
                "SOUND_GENERIC_TRACE,playhead_ms=%u,t=%u,owner=%u:%u,ref=0x%llx,family=%s,taker=%s,off_step=%d,no_hold=%d,event=%s%s%s",
                nowMs, plan.startMs, c.owner >> 16, c.owner & 0xFFFFu, static_cast<unsigned long long>(c.rawRef ? c.rawRef : c.stableId),
                soundowner::FamilyName(plan.family), soundowner::TakerName(plan.taker), plan.offStep ? 1 : 0, plan.noHold ? 1 : 0,
                event, detail && *detail ? ",detail=" : "", detail ? detail : "");
            m_traceLines.push_back(line);
        }

        SoundTally m_tally;
        TraceWindow m_trace;
        std::vector<std::string> m_traceLines;
        std::uint32_t m_traceDropped = 0;
        std::uint32_t m_traceWritten = 0;
        const Track* m_track = nullptr;
        Config m_config;
        PlayStats m_stats;
        std::vector<SoundPlan> m_plans;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> m_byIndex; // (packet index, plan), sorted by index
        std::vector<std::uint32_t> m_pending; // plans due, waiting or preloaded
        std::vector<std::uint32_t> m_live;    // plans playing
        DoubleDetector m_doubles;
        std::vector<DoubleEvidence> m_doubleEvidence;
        std::size_t m_doubleEvidenceTaken = 0;
        std::uint32_t m_lastMs = 0;
        std::size_t m_next = 0;        // first plan not yet due
        std::size_t m_preloadNext = 0; // first plan whose preload lead is not yet reached
        bool m_resume = true;
        bool m_began = false;
        std::uint64_t m_ticks = 0; // Tick calls (the complement's wait)
        std::vector<ComplementEvidence> m_complementEvidence; // the first complements played
    };

    // ------------------------------------------------------------ report

    namespace detail
    {
        inline std::string Num(std::uint64_t value) { return std::to_string(value); }
    }

    // The SOUND_GENERIC_PLAY line (every 60 s in playback and at the end).
    inline std::string FormatPlayLogLine(const PlayStats& s, const char* phase, std::uint32_t playheadMs, std::size_t live,
        std::size_t pending)
    {
        using detail::Num;
        return std::string("SOUND_GENERIC_PLAY,phase=") + phase + ",playhead_ms=" + Num(playheadMs) + ",packets=" +
            Num(s.packets) + ",generic=" + Num(s.generic) + ",played=" + Num(s.played) + ",preloaded=" + Num(s.preloaded) +
            ",resumed=" + Num(s.resumed) + ",refused=" + Num(s.refused) + ",unplaced=" + Num(s.unplaced) + ",yielded=" + Num(s.yielded) + ",missed=" +
            Num(s.missed) + ",jumped=" + Num(s.jumped) + ",specific=" + Num(s.skippedSpecific) + ",puppet=" +
            Num(s.skippedPuppet) + ",adjusts=" + Num(s.adjusts) + ",stops=" + Num(s.stops) + ",forgets=" + Num(s.forgets) +
            ",releases=" + Num(s.releases) + ",ended_by_engine=" + Num(s.endedByEngine) + ",doubles=" + Num(s.doubles) +
            ",stop_all=" + Num(s.stopAll) + ",from_tracks=" + Num(s.fromTracks) + ",entity_driven=" + Num(s.entityDriven) + ",off_step=" + Num(s.offStep) +
            ",ended_at_once=" + Num(s.endedAtOnce) + ",ended_early=" + Num(s.endedEarly) + ",live=" + Num(live) + ",pending=" + Num(pending) + ",live_peak=" + Num(s.livePeak) +
            ",kept_loops=" + Num(s.keptLoops) +
            ",complement=" + Num(s.complement) + ",complement_played=" + Num(s.complementPlayed) +
            ",complement_yielded=" + Num(s.complementYielded) + ",complement_melee_played=" + Num(s.complementMeleePlayed) +
            ",complement_yielded_set=" + Num(s.complementYieldedSet) +
            ",start_values=" + Num(s.startValues);
    }

    // One line per complement played (the first
    // kComplementEvidenceMax): the packet the track's engine call did not
    // make again, with how late it started.
    inline std::string FormatComplementLine(const ComplementEvidence& e)
    {
        char text[224]{};
        std::snprintf(text, sizeof(text),
            "SOUND_GENERIC_COMPLEMENT,owner=%s,family=%s,ref=0x%llx,caller=0x%x,event_ms=%u,late_ms=%u",
            census::IdText(e.owner).c_str(), soundowner::FamilyName(e.family), static_cast<unsigned long long>(e.ref),
            e.caller, e.timeMs, e.lateMs);
        return text;
    }

    // The section of ReplayDebug\<clip>.sound-check.txt: per family, what
    // was recorded against what was played, refused, skipped and doubled.
    inline std::string FormatSection(const PlayStats& s, const soundowner::Overrides& overrides, const std::string& ports)
    {
        using detail::Num;
        std::string out;
        out += "== generic sound packets (SoundGeneric=1) ==\n";
        out += "Packets recorded " + Num(s.packets) + ", given to the generic player " + Num(s.generic) + ", played " +
            Num(s.played) + " (" + Num(s.preloaded) + " preloaded, " + Num(s.resumed) + " resumed after a jump), refused by the engine " +
            Num(s.refused) + ", yielded to another source " + Num(s.yielded) + ", missed their window " + Num(s.missed) +
            ", skipped by a jump " + Num(s.jumped) + ", doubled (another source made the same sound within " +
            "the census window) " + Num(s.doubles) + ".\n";
        out += "Adjusts applied " + Num(s.adjusts) + ", stops " + Num(s.stops) + ", stop-and-forget " + Num(s.forgets) +
            ", releases (forgotten, left to the engine) " + Num(s.releases) + ", mode c starts asked ahead " + Num(s.ledStarts) + ", ended by the engine before the release " + Num(s.endedByEngine) + ".\n";
        out += std::string("Taker table (SoundGenericMode=") + (overrides.packets ? "packets" : "legacy") + "): " +
            soundowner::FormatTable(overrides) + "\n";
        if (!ports.empty())
            out += "Engine ports: " + ports + "\n";
        // What the refusals were, and the 2D sounds
        // of no entity apart (they never reach the engine).
        if (!s.refusedStatus.empty())
        {
            out += "Refused by the engine, by status:";
            bool first = true;
            for (const auto& entry : s.refusedStatus)
            {
                out += std::string(first ? " " : "; ") + entry.first + " " + Num(entry.second);
                first = false;
            }
            out += ".\n";
        }
        if (s.fromTracks)
            out += "Taken from the tracks that replayed them by their ref before (SoundGenericMode=packets: entity sounds, "
                   "sound requests, collisions, capture, movement), played here with their recorded adjusts: " +
                Num(s.fromTracks) + " packets.\n";
        if (s.unplaced)
            out += "Not placed (2D sounds of no entity: interface, frontend, ambience beds; the live world makes its own): " +
                Num(s.unplaced) + ".\n";
        if (s.offStep)
            out += "Step packets away from every recorded foot event of their owner (slide, drag, braking: the step track does "
                   "not remake them), given to this player: " + Num(s.offStep) + ".\n";
        if (s.offShot)
            out += "Weapon object sounds no weapon code asked for and away from every recorded shot of that weapon (a script "
                   "sound on the object, the campfire's sizzle on the meat: the shot track does not remake them), given to this player: " +
                Num(s.offShot) + ".\n";
        if (s.complement)
            out += "Step and melee packets of persons whose track replays the recorded foot event or hit through the engine: " +
                Num(s.complement) + ". Played here because the track's call did not make them again: " +
                Num(s.complementPlayed) + " (melee " + Num(s.complementMeleePlayed) + "); left to the call that made them: " +
                Num(s.complementYielded) + ", of which the call made in another entry of the same sound set: " +
                Num(s.complementYieldedSet) + " (counts per pass).\n";
        if (s.endedAtOnce)
            out += "Created and ended by the engine before the next tick, before their recorded end (never heard): " +
                Num(s.endedAtOnce) + ".\n";
        if (s.endedEarly)
            out += "Ended by the engine before half the life they had live (a one-shot that lived at least 200 ms in the "
                   "recording; a dynamic sound whose owner state the replay lacks): " + Num(s.endedEarly) + ".\n";
        char line[256]{};
        std::snprintf(line, sizeof(line), "%-14s %-9s %8s %7s %9s %7s %8s %7s %7s %6s %7s\n", "family", "taker", "recorded",
            "played", "preloaded", "refused", "unplaced", "skipped", "yielded", "missed", "doubles");
        out += line;
        for (std::uint8_t n = 0; n < soundowner::kFamilyCount; ++n)
        {
            const FamilyStats& f = s.families[n];
            if (!f.packets)
                continue;
            const soundowner::Family family = static_cast<soundowner::Family>(n);
            std::snprintf(line, sizeof(line), "%-14s %-9s %8u %7u %9u %7u %8u %7u %7u %6u %7u\n", soundowner::FamilyName(family),
                soundowner::TakerName(soundowner::TakerOf(family, overrides)), f.packets, f.played, f.preloaded, f.refused,
                f.unplaced, f.skipped + f.jumped, f.yielded, f.missed, f.doubles);
            out += line;
        }
        out += "How to read: \"skipped\" is what the owner table left to a specific track or to the puppet (plus short sounds a jump passed);\n"
               "\"yielded\" is what the puppet or the live world made by itself around the packet's time; \"doubles\" counts sounds this\n"
               "player started while another source made the same sound anyway. SoundGenericTake=family:taker in the ini changes a family.\n";
        return out;
    }

    // ------------------------------------------------------------ per sound

    // The sounds of the owners whose own sounds the ports refuse, per pass, that
    // sounds of the owners whose own sounds the ports refuse, per pass, that
    // fell short (TallyShort): nothing else makes them, so each is a sound
    // the replay lost even when the census saw a create. Empty when none.
    inline std::string FormatTallySection(const SoundTally& tally, std::size_t limit = 30)
    {
        std::string out;
        std::size_t shown = 0;
        std::uint32_t rows = 0;
        for (const auto& entry : tally)
            rows += TallyShort(entry.second) ? 1 : 0;
        if (!rows)
            return out;
        out = "== generic packets that fell short (owners whose own sounds the ports refuse: nothing else makes them) ==\n";
        char line[420]{};
        for (const auto& entry : tally)
        {
            const SoundTallyEntry& e = entry.second;
            if (!TallyShort(e))
                continue;
            if (shown++ >= limit)
            {
                out += "... and " + std::to_string(rows - limit) + " more.\n";
                break;
            }
            std::snprintf(line, sizeof(line),
                "SHORT  owner %u:%u  ref 0x%llx  %s%s  due %u, played %u, yielded %u, refused %u, missed %u, ended by the engine at once %u; from %u.%u s\n",
                entry.first.owner >> 16, entry.first.owner & 0xFFFFu, static_cast<unsigned long long>(e.ref),
                soundowner::FamilyName(e.family), e.offStep ? " (step away from a foot event)" : "", e.taken, e.played, e.yielded,
                e.refused, e.missed, e.endedAtOnce, e.firstShortMs == 0xFFFFFFFFu ? 0 : e.firstShortMs / 1000,
                e.firstShortMs == 0xFFFFFFFFu ? 0 : (e.firstShortMs % 1000) / 100);
            out += line;
            if (e.endedEarly)
            {
                out.pop_back();
                out += "; ended early " + std::to_string(e.endedEarly) + "\n";
            }
        }
        out += "How to read: \"yielded\" means another source made the same sound (set and entry) around its time; with the\n"
               "puppet's own refused that can only be a replayed step, melee or speech. \"ended by the engine at once\" is a sound\n"
               "created and gone before the next tick, before its recorded end; \"ended early\" lived less than half its recorded\n"
               "life. Each row is a MISSING of the notice at Stop.\n\n";
        return out;
    }

    // The same rows as the sound check's findings (MISSING, for the notice).
    inline std::vector<census::Finding> TallyFindings(const SoundTally& tally)
    {
        std::vector<census::Finding> out;
        for (const auto& entry : tally)
        {
            const SoundTallyEntry& e = entry.second;
            if (!TallyShort(e))
                continue;
            census::Finding f;
            f.kind = census::FindingKind::Missing;
            f.owner = entry.first.owner;
            f.cls = e.cls;
            f.ref = e.ref;
            f.recorded = static_cast<double>(e.taken);
            f.played = static_cast<double>(e.played - (std::min)(e.played, e.endedAtOnce + e.endedEarly));
            if (e.firstShortMs != 0xFFFFFFFFu)
                f.spans.push_back({e.firstShortMs, e.firstShortMs + 250});
            out.push_back(f);
        }
        return out;
    }

    // ------------------------------------------------------------ blocked

    // The creates of the engine's own on the replay's puppets in packets mode,
    // for the sound check, apart from the census (a refused one never became a
    // sound). Each is judged at the port against the lives of the clip's
    // packets (soundowner::PacketLives): Recorded (the recording plays it) and
    // Alternative (another set at the entry a recorded packet fills) are
    // refused; Passed (no packet: the puppet makes it) goes through with
    // SoundGenericBlock=packet (the default) and is refused with
    // SoundGenericBlock=all. S03 refuses the puppets' own one-shots whatever
    // the verdict. A persistent wrapper asks again every frame while its ref
    // is empty, so the attempts are also counted as runs (a new run after
    // 250 ms without an attempt of that owner and sound).
    constexpr std::uint32_t kBlockedRunGapMs = 250;

    struct BlockedKey
    {
        std::uint8_t group = 0; // 0 no packet (passed or refused), 1 alternative, 2 recorded, 3 step unmade, 4 unrecorded
        std::uint32_t owner = census::kNoOwner;
        std::uint64_t sound = 0; // soundowner::SoundKey
        // The ones without a packet first, then the unrecorded refusals, then
        // the step unmade ones, then the rest.
        static std::uint8_t Rank(std::uint8_t g) { return g == 0 ? 0 : g == 4 ? 1 : g == 3 ? 2 : static_cast<std::uint8_t>(g + 2); }
        bool operator<(const BlockedKey& o) const
        {
            if (group != o.group)
                return Rank(group) < Rank(o.group);
            if (owner != o.owner)
                return owner < o.owner;
            return sound < o.sound;
        }
    };

    struct BlockedEntry
    {
        std::uint32_t count = 0;   // attempts
        std::uint32_t refused = 0; // of them refused at the port
        std::uint32_t runs = 0;
        std::uint32_t firstMs = 0xFFFFFFFFu;
        std::uint32_t lastMs = 0;
        std::uint32_t ports = 0; // bit per port (3, 4, 5, 6)
        std::uint64_t ref = 0;   // the first one's ref
        std::uint32_t caller = 0;
    };

    class BlockedTally
    {
    public:
        void Clear()
        {
            m_entries.clear();
            m_total = m_refused = m_hidden = m_runsNoPacket = 0;
            for (std::uint32_t& n : m_byPort)
                n = 0;
            for (std::uint32_t& n : m_byVerdict)
                n = 0;
        }
        // One create of a puppet's own at a port: its verdict and whether
        // the port refused it.
        void Note(std::uint32_t owner, std::uint64_t ref, std::uint32_t caller, std::uint8_t port, std::uint32_t clipMs,
            soundowner::LifeVerdict verdict, bool refused)
        {
            ++m_total;
            m_refused += refused ? 1 : 0;
            if (port < 8)
                ++m_byPort[port];
            // the hidden player and his mount: no clip entity, nothing recorded
            // to play (the ports' "no owner" is 0xFFFFFFFF, puppet_audio_rule.h)
            if (owner == census::kNoOwner || owner == 0xFFFFFFFFu)
            {
                ++m_hidden;
                return;
            }
            const std::uint8_t v = static_cast<std::uint8_t>(verdict);
            if (v < soundowner::kLifeVerdictCount)
                ++m_byVerdict[v];
            BlockedKey key;
            key.group = verdict == soundowner::LifeVerdict::Unrecorded ? 4
                : verdict == soundowner::LifeVerdict::Unmade ? 3
                : verdict == soundowner::LifeVerdict::Recorded ? 2
                : verdict == soundowner::LifeVerdict::Alternative ? 1 : 0;
            key.owner = owner;
            key.sound = soundowner::SoundKey(ref);
            BlockedEntry& entry = m_entries[key];
            const bool newRun = !entry.count || clipMs < entry.lastMs || clipMs - entry.lastMs > kBlockedRunGapMs;
            if (!entry.count)
            {
                entry.ref = ref;
                entry.caller = caller;
            }
            if (newRun)
            {
                ++entry.runs;
                if (key.group == 0)
                    ++m_runsNoPacket;
            }
            ++entry.count;
            entry.refused += refused ? 1 : 0;
            entry.firstMs = (std::min)(entry.firstMs, clipMs);
            entry.lastMs = (std::max)(entry.lastMs, clipMs);
            if (port < 32)
                entry.ports |= 1u << port;
        }
        std::uint32_t Total() const { return m_total; }
        std::uint32_t Refused() const { return m_refused; }
        std::uint32_t Passed() const { return m_total - m_refused; }
        std::uint32_t Hidden() const { return m_hidden; }
        std::uint32_t ByVerdict(soundowner::LifeVerdict v) const
        {
            return static_cast<std::uint8_t>(v) < soundowner::kLifeVerdictCount ? m_byVerdict[static_cast<std::uint8_t>(v)] : 0;
        }
        std::uint32_t WithPacket() const
        {
            return ByVerdict(soundowner::LifeVerdict::Recorded) + ByVerdict(soundowner::LifeVerdict::Alternative);
        }
        std::uint32_t WithoutPacket() const { return ByVerdict(soundowner::LifeVerdict::Passed); }
        std::uint32_t RunsWithoutPacket() const { return m_runsNoPacket; }
        std::uint32_t ByPort(std::uint8_t port) const { return port < 8 ? m_byPort[port] : 0; }
        const std::map<BlockedKey, BlockedEntry>& Entries() const { return m_entries; }

    private:
        std::map<BlockedKey, BlockedEntry> m_entries;
        std::uint32_t m_total = 0;
        std::uint32_t m_refused = 0;
        std::uint32_t m_hidden = 0;
        std::uint32_t m_runsNoPacket = 0;
        std::uint32_t m_byPort[8] = {};
        std::uint32_t m_byVerdict[soundowner::kLifeVerdictCount] = {};
    };

    inline std::string BlockedLogLine(const BlockedTally& t, bool blockAll = false)
    {
        char line[560]{};
        // unrecorded= at the end (the parsers key by name); s01, s02 after it,
        // the vehicles' own creates at creator A and B.
        std::snprintf(line, sizeof(line),
            "SOUND_GENERIC_BLOCKED_SUMMARY,block=%s,attempts=%u,refused=%u,passed=%u,recorded=%u,alternative=%u,no_packet=%u,"
            "no_packet_runs=%u,hidden_player=%u,s03=%u,s04=%u,s05=%u,s06=%u,step_unmade=%u,unrecorded=%u,s01=%u,s02=%u",
            blockAll ? "all" : "packet", t.Total(), t.Refused(), t.Passed(), t.ByVerdict(soundowner::LifeVerdict::Recorded),
            t.ByVerdict(soundowner::LifeVerdict::Alternative), t.WithoutPacket(), t.RunsWithoutPacket(), t.Hidden(), t.ByPort(3),
            t.ByPort(4), t.ByPort(5), t.ByPort(6), t.ByVerdict(soundowner::LifeVerdict::Unmade),
            t.ByVerdict(soundowner::LifeVerdict::Unrecorded), t.ByPort(1), t.ByPort(2));
        return line;
    }

    // The sound check's section (empty when the mode is off). The rows
    // without a packet first, at most `limit` lines.
    inline std::string FormatBlockedSection(const BlockedTally& t, bool packets, std::size_t limit = 40, bool blockAll = false)
    {
        if (!packets)
            return std::string();
        std::string out = "== the puppets' own creates (SoundGenericMode=packets, SoundGenericBlock=" +
            std::string(blockAll ? "all" : "packet") + ") ==\n";
        char line[1100]{};
        std::snprintf(line, sizeof(line),
            "Attempts %u (a persistent wrapper asks again every frame while refused): refused %u, passed %u.\n"
            "With the recorded sound in a packet alive then (refused, the clip plays it) %u; another set at the entry a recorded "
            "packet fills (refused, proved double) %u; without a packet %u in %u runs (%s); on the hidden player and his mount %u.\n"
            "By port: S03 create-and-play %u (the puppets' own one-shots, refused), S04 deferred request %u, S05 "
            "persistent by ref %u, S06 persistent by hash %u; a vehicle's or train car's own create at the creators: S01 %u, "
            "S02 %u (SoundGenericVehicleBlock).\n"
            "Inside a replayed step or melee call, a set the owner never made in the clip (refused, SoundStepSetGate) %u.\n"
            "A persistent sound by ref the owner never made in the clip and the ESND track does not carry (refused, "
            "PuppetUnrecordedPersistent) %u.\n",
            t.Total(), t.Refused(), t.Passed(), t.ByVerdict(soundowner::LifeVerdict::Recorded),
            t.ByVerdict(soundowner::LifeVerdict::Alternative), t.WithoutPacket(), t.RunsWithoutPacket(),
            blockAll ? "refused too" : "left to the puppet, except S03", t.Hidden(), t.ByPort(3), t.ByPort(4), t.ByPort(5), t.ByPort(6),
            t.ByPort(1), t.ByPort(2), t.ByVerdict(soundowner::LifeVerdict::Unmade), t.ByVerdict(soundowner::LifeVerdict::Unrecorded));
        out += line;
        std::size_t shown = 0;
        static const char* const kGroup[5] = {"NO PACKET ", "ALTERNATE ", "recorded  ", "STEP UNMADE ", "UNRECORDED "};
        for (const auto& entry : t.Entries())
        {
            if (shown++ >= limit)
            {
                out += "... and " + std::to_string(t.Entries().size() - limit) + " more (owner, sound) rows.\n";
                break;
            }
            const BlockedEntry& e = entry.second;
            std::string ports;
            for (std::uint8_t port = 1; port <= 6; ++port) // S01, S02 the creators
                if (e.ports & (1u << port))
                    ports += (ports.empty() ? "S0" : ",S0") + std::to_string(port);
            std::snprintf(line, sizeof(line),
                "%s owner %u:%u  sound 0x%llx  attempts %u in %u runs, refused %u  (%s, ref 0x%llx at 0x%x)  %u.%u s to %u.%u s\n",
                kGroup[entry.first.group < 5 ? entry.first.group : 0], entry.first.owner >> 16, entry.first.owner & 0xFFFFu,
                static_cast<unsigned long long>(entry.first.sound), e.count, e.runs, e.refused, ports.c_str(),
                static_cast<unsigned long long>(e.ref), e.caller, e.firstMs / 1000, (e.firstMs % 1000) / 100, e.lastMs / 1000,
                (e.lastMs % 1000) / 100);
            out += line;
        }
        out += "How to read: a create the engine made by itself for a replay entity (its animation, AI or audio update). \"recorded\"\n"
               "and \"ALTERNATE\" are refused, the clip plays the recorded sound (the model of the GTA V Rockstar Editor); \"NO PACKET\"\n"
               "is a sound the recording has no packet for (a loop alive before the recording began, or one the recording never made):\n"
               "the puppet makes it unless SoundGenericBlock=all. SoundGenericMode=legacy brings back every own sound of the puppets.\n\n";
        return out;
    }
}
