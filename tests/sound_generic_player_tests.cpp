// The generic sound player's pure part: the owner table (sound_owner_table.h:
// families, takers, the ini overrides, the cover of the clip's tracks, the
// caller ranges) and the player (sound_generic_player_core.h: the plans, the
// hold and the anti-double rule, the preload agenda, the adjusts and ends at
// the playhead, pause, scrub and jump, the double detector, the log line and
// the sound-check section) against a fake engine.
#include "sound_generic_player_core.h"
#include "sound_owner_table.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using namespace replay::soundgeneric;
    using namespace replay::soundgeneric::player;
    namespace owner = replay::soundowner;
    using replay::census::AudioBase;
    using replay::census::OwnerClass;
    using replay::census::kNoOwner;

    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

    constexpr std::uint64_t kHoofRef = 0x008000040029934aull; // the wet hoof set, variant 4, flag bit 55
    constexpr std::uint64_t kSetA = 0x0080000300123456ull;
    constexpr std::uint64_t kSetB = 0x0080000300654321ull;
    constexpr std::uint32_t kPed = 0x00070002u;   // packed replay id
    constexpr std::uint32_t kHorse = 0x00080001u;
    constexpr std::uint32_t kGameCaller = 0x2EFA7E0; // a game site in no known range

    // ------------------------------------------------------------ fake engine

    struct FakeEngine : Engine
    {
        struct Call
        {
            std::string what;
            std::uint64_t handle;
            std::uint32_t index; // the packet's index for creates
        };
        std::vector<Call> calls;
        std::vector<std::pair<std::uint32_t, float>> adjustValues; // time order, (kind, value[0])
        std::vector<bool> alive;      // by handle
        std::vector<bool> playing;    // by handle
        bool refuse = false;          // Create fails for good
        bool retry = false;           // Create fails, may work next tick
        bool refusePlay = false;
        bool keep = false; // KeepAcrossJump's answer for a live handle
        std::vector<std::pair<std::uint32_t, std::uint64_t>> others; // (owner, refLow) another source made
        bool KeepAcrossJump(std::uint64_t handle) override { return keep && Alive(handle); }

        bool Create(const CreatePacket& packet, bool play, std::uint64_t& handle, std::string& status, bool& retryOut) override
        {
            retryOut = false;
            if (retry)
            {
                status = "no_puppet";
                retryOut = true;
                calls.push_back({"create_retry", kNoHandle, packet.index});
                return false;
            }
            if (refuse)
            {
                status = "refused";
                calls.push_back({"create_refused", kNoHandle, packet.index});
                return false;
            }
            handle = alive.size();
            alive.push_back(true);
            playing.push_back(play);
            calls.push_back({play ? "create_play" : "create", handle, packet.index});
            status = play ? "playing" : "created";
            return true;
        }
        bool Prepare(std::uint64_t handle) override
        {
            calls.push_back({"prepare", handle, 0});
            return true;
        }
        bool Play(std::uint64_t handle) override
        {
            calls.push_back({"play", handle, 0});
            if (refusePlay)
                return false;
            playing[static_cast<std::size_t>(handle)] = true;
            return true;
        }
        void Stop(std::uint64_t handle, EndReason reason) override
        {
            calls.push_back({reason == EndReason::Stop ? "stop" : reason == EndReason::StopAndForget ? "forget" : "release",
                handle, 0});
            if (reason != EndReason::Stop)
                alive[static_cast<std::size_t>(handle)] = false;
        }
        // A recorded release forgets the handle and leaves the sound to the
        // engine (it is not stopped here).
        void Forget(std::uint64_t handle) override { calls.push_back({"release", handle, 0}); }
        bool Alive(std::uint64_t handle) override
        {
            return handle < alive.size() && alive[static_cast<std::size_t>(handle)];
        }
        void Adjust(std::uint64_t handle, const AdjustPacket& packet) override
        {
            calls.push_back({std::string("adjust_") + AdjustName(packet.kind), handle, packet.index});
            adjustValues.emplace_back(static_cast<std::uint32_t>(packet.kind), packet.value[0]);
        }
        bool MadeByOther(const CreatePacket& packet, std::uint32_t) override
        {
            for (const auto& other : others)
                if (other.first == packet.owner && other.second == replay::census::RefLow(packet.stableId))
                    return true;
            return false;
        }

        std::size_t Count(const char* what) const
        {
            std::size_t n = 0;
            for (const Call& call : calls)
                if (call.what == what)
                    ++n;
            return n;
        }
        bool Last(const char* what) const { return !calls.empty() && calls.back().what == what; }
        void Clear() { calls.clear(); }
        // The engine ends a sound by itself (a one-shot over).
        void EndByItself(std::uint64_t handle) { alive[static_cast<std::size_t>(handle)] = false; }
    };

    // ------------------------------------------------------------ track helpers

    CreatePacket Create(Track& track, std::uint32_t timeMs, std::uint32_t ownerId, std::uint64_t ref,
        std::uint32_t caller = kGameCaller, std::uint16_t delayMs = 0)
    {
        CreatePacket p;
        p.timeMs = timeMs;
        p.index = track.nextIndex++;
        p.stableId = ref & 0x00FFFFFFFFFFull;
        p.idKind = IdKind::RefSet;
        p.rawRef = ref;
        p.owner = ownerId;
        p.ownerClass = ownerId == kNoOwner ? OwnerClass::World : OwnerClass::Ped;
        p.source = Source::Queue;
        p.hasPosition = ownerId == kNoOwner;
        p.hasParams = true;
        p.createDelayMs = delayMs;
        p.caller = caller;
        p.params.volume = -6.0f;
        track.creates.push_back(p);
        return p;
    }

    void Adjust(Track& track, std::uint32_t timeMs, std::uint32_t index, Adjust kind, float value)
    {
        AdjustPacket a;
        a.timeMs = timeMs;
        a.index = index;
        a.kind = kind;
        a.value[0] = value;
        track.adjusts.push_back(a);
    }

    void End(Track& track, std::uint32_t timeMs, std::uint32_t index, EndReason reason)
    {
        EndPacket e;
        e.timeMs = timeMs;
        e.index = index;
        e.reason = reason;
        track.ends.push_back(e);
    }

    player::Config TestConfig()
    {
        player::Config c;
        c.preloadMs = 500;
        c.preloadDelayMs = 100;
        c.holdMs = 150;
        c.loopMs = 1000;
        c.maxStepMs = 250;
        c.oneShotWindowMs = 500;
        c.doubleToleranceMs = 250;
        c.enabled = true;
        return c;
    }

    // Normal play from `fromMs` to `toMs` in steps of `stepMs`.
    void Run(Player& player, FakeEngine& engine, std::uint32_t fromMs, std::uint32_t toMs, std::uint32_t stepMs = 50)
    {
        for (std::uint32_t t = fromMs; t <= toMs; t += stepMs)
            player.Tick(t, true, engine);
    }

    // ------------------------------------------------------------ owner table

    void TestOwnerTableFamilies()
    {
        owner::Cover cover;
        owner::PacketKey key;
        key.owner = kPed;
        key.cls = OwnerClass::Ped;
        key.stableId = kSetA & 0x00FFFFFFFFFFull;
        key.caller = kGameCaller;
        key.timeMs = 5000;
        Check(owner::FamilyOf(key, cover) == owner::Family::Owned, "table: an owned sound of no known system is owned");
        key.owner = kNoOwner;
        key.cls = OwnerClass::World;
        Check(owner::FamilyOf(key, cover) == owner::Family::World, "table: no owner is world");
        key.owner = kPed;
        key.cls = OwnerClass::Ped;
        // The cover first: the entity sound track carries the same sound.
        // The same set and entry (the flag bit apart: another bank); another
        // entry of the set is another sound (the hoof step 0x800005002f4361 is
        // not the skid's 0x80000e002f4361).
        cover.Add(kPed, 0x00000005ull << 32 | (kSetA & 0xFFFFFFFFull), 5300, owner::Family::EntitySound);
        Check(owner::FamilyOf(key, cover) == owner::Family::Owned, "table: another entry of the set is not covered");
        cover.Add(kPed, kSetA & 0x00FFFFFFFFFFull, 5300, owner::Family::EntitySound);
        Check(owner::FamilyOf(key, cover) == owner::Family::EntitySound, "table: the cover names the track (same set and entry, other bank, 300 ms)");
        key.timeMs = 5800;
        Check(owner::FamilyOf(key, cover) == owner::Family::Owned, "table: the cover's tolerance is 400 ms (500 ms is outside)");
        cover.Clear();
        // The wet hoof set before the caller.
        key.stableId = kHoofRef & 0x00FFFFFFFFFFull;
        key.caller = 0x0318CEC0; // inside the step layer helper
        Check(owner::FamilyOf(key, cover) == owner::Family::HorseStep, "table: the wet hoof set is the horse step family");
        // The caller ranges.
        key.stableId = kSetA & 0x00FFFFFFFFFFull;
        Check(owner::FamilyOf(key, cover) == owner::Family::Footstep, "table: the step layer helper is a footstep");
        key.caller = 0x0318DAFE; // the main step sound's return
        Check(owner::FamilyOf(key, cover) == owner::Family::Footstep, "table: the step function is a footstep");
        key.caller = 0x0318A2A1; // inside the melee hit entry
        Check(owner::FamilyOf(key, cover) == owner::Family::Melee, "table: the melee hit entry");
        key.caller = 0x0318EEB1; // inside the body impact entry
        Check(owner::FamilyOf(key, cover) == owner::Family::Melee, "table: the body impact entry");
        key.caller = 0x03193E6F; // inside the movement update
        Check(owner::FamilyOf(key, cover) == owner::Family::Movement, "table: the movement update");
        key.caller = 0x02588FF0;
        Check(owner::FamilyOf(key, cover) == owner::Family::Collision, "table: the collision entry");
        key.caller = 0x02FA0E00;
        Check(owner::FamilyOf(key, cover) == owner::Family::Speech, "table: the speech line");
        key.caller = 0x0318C150 - 1;
        Check(owner::FamilyOf(key, cover) == owner::Family::Owned, "table: one byte before a range is outside");
        key.caller = 0x0318C4C4;
        Check(owner::FamilyOf(key, cover) == owner::Family::Owned, "table: the end of a range is outside");
        // The weapon audio base after the caller.
        key.caller = kGameCaller;
        key.audioKnown = true;
        key.audioBase = AudioBase::WeaponAudio;
        Check(owner::FamilyOf(key, cover) == owner::Family::Gunshot, "table: a sound on the weapon's audio is a shot");
        key.audioKnown = false;
        Check(owner::FamilyOf(key, cover) == owner::Family::Owned, "table: an unknown audio base says nothing");
        key.audioKnown = true;
        key.audioBase = AudioBase::PedHolder;
        Check(owner::FamilyOf(key, cover) == owner::Family::Owned, "table: the ped holder says nothing");
        // The ranges themselves: ordered inside, no overlap.
        bool sane = true;
        for (std::size_t a = 0; a < owner::kCallerRangeCount; ++a)
        {
            const owner::CallerRange& x = owner::kCallerRanges[a];
            sane = sane && x.begin < x.end;
            for (std::size_t b = a + 1; b < owner::kCallerRangeCount; ++b)
            {
                const owner::CallerRange& y = owner::kCallerRanges[b];
                sane = sane && (x.end <= y.begin || y.end <= x.begin);
            }
        }
        Check(sane, "table: the caller ranges are ordered and do not overlap");
        Check(owner::kCallerRangeCount == 12, "table: twelve caller ranges");
    }

    void TestOwnerTableTakers()
    {
        using owner::Family;
        using owner::Taker;
        Check(owner::DefaultTaker(Family::World) == Taker::Generic && owner::DefaultTaker(Family::Owned) == Taker::Generic,
            "takers: world and owned go to the generic player");
        Check(owner::DefaultTaker(Family::Movement) == Taker::Puppet, "takers: the movement foley is the puppet's");
        const Family specific[] = {Family::Speech, Family::Footstep, Family::HorseStep, Family::Melee, Family::Gunshot,
            Family::Explosion, Family::Collision, Family::EntitySound, Family::SoundRequest, Family::Captured};
        bool all = true;
        for (const Family family : specific)
            all = all && owner::DefaultTaker(family) == Taker::Specific;
        Check(all, "takers: every family with a track keeps it");
        // Names round trip.
        bool names = true;
        for (std::uint8_t n = 0; n < owner::kFamilyCount; ++n)
        {
            Family family = Family::World;
            names = names && owner::FamilyOfName(owner::FamilyName(static_cast<Family>(n)), family) &&
                family == static_cast<Family>(n);
        }
        for (std::uint8_t n = 0; n < owner::kTakerCount; ++n)
        {
            Taker taker = Taker::Generic;
            names = names && owner::TakerOfName(owner::TakerName(static_cast<Taker>(n)), taker) && taker == static_cast<Taker>(n);
        }
        Check(names, "takers: family and taker names round trip");
        // The ini overrides.
        const owner::Overrides o = owner::ParseOverrides("horse_step:generic; movement : generic,gunshot:puppet;bogus:generic;melee:nobody;;");
        Check(o.applied == 3 && o.rejected == "bogus:generic;melee:nobody", "overrides: three applied, two rejected");
        Check(owner::TakerOf(Family::HorseStep, o) == Taker::Generic && owner::TakerOf(Family::Movement, o) == Taker::Generic &&
                owner::TakerOf(Family::Gunshot, o) == Taker::Puppet && owner::TakerOf(Family::Melee, o) == Taker::Specific &&
                owner::TakerOf(Family::World, o) == Taker::Generic,
            "overrides: an overridden family takes the ini, the rest the default");
        const owner::Overrides none = owner::ParseOverrides("");
        Check(none.applied == 0 && none.rejected.empty(), "overrides: an empty key changes nothing");
        const std::string table = owner::FormatTable(o);
        Check(Has(table, "world:generic;owned:generic;speech:specific;footstep:specific;horse_step:generic(ini);movement:generic(ini);melee:specific;gunshot:puppet(ini);explosion:specific;collision:specific;entity_sound:specific;sound_request:specific;captured:specific"),
            "overrides: the table line marks the ini takers");
    }

    // ------------------------------------------------------------ player

    void TestPlayerNormalPlay()
    {
        Track track;
        const CreatePacket a = Create(track, 1000, kPed, kSetA);
        Adjust(track, 1000, a.index, Adjust::Volume, -3.0f);   // the initial state (before the hold ends)
        Adjust(track, 1300, a.index, Adjust::Pitch, 100.0f);
        Adjust(track, 1600, a.index, Adjust::Position, 1.0f);
        End(track, 1800, a.index, EndReason::Stop);
        End(track, 2200, a.index, EndReason::Release);
        const CreatePacket b = Create(track, 1200, kNoOwner, kSetB); // a world one-shot, no release: ends by itself
        FakeEngine engine;
        Player player;
        player.Begin(track, TestConfig(), owner::Cover(), owner::Overrides(), 0);
        Check(player.Stats().packets == 2 && player.Stats().generic == 2, "play: two packets, both generic");
        Run(player, engine, 0, 1100);
        // Packets up to kQueueLeadMaxMs ahead are admitted (b at 1200 too), none started.
        Check(engine.Count("create_play") == 0 && player.Pending() == 2, "play: the hold waits 150 ms after the packet's time");
        player.Tick(1150, true, engine);
        Check(engine.Count("create_play") == 1 && engine.calls[0].index == a.index && engine.Count("adjust_volume") == 1,
            "play: the start at hold end, with the adjusts of the time already gone applied");
        Check(player.Live() == 1 && player.Stats().played == 1, "play: one live sound, one played");
        Run(player, engine, 1200, 1700);
        Check(engine.Count("create_play") == 2 && engine.Count("adjust_pitch") == 1 && engine.Count("adjust_position") == 1,
            "play: the second packet started, the adjusts applied at the playhead");
        Check(player.Stats().adjusts == 3, "play: three adjusts applied");
        Run(player, engine, 1750, 1850);
        Check(engine.Count("stop") == 1 && player.Stats().stops == 1 && player.Live() == 2,
            "play: the stop packet calls S13 and the sound keeps its slot");
        engine.EndByItself(1); // the one-shot b is over in the engine
        Run(player, engine, 1900, 2250);
        Check(engine.Count("release") == 1 && player.Stats().releases == 1, "play: the release packet hands the live sound to the engine");
        Check(player.Stats().endedByEngine == 0 && player.Live() == 0, "play: b ended by itself without ends, nothing counted");
        Check(player.Plans()[0].state == SoundState::Ended && player.Plans()[1].state == SoundState::Ended,
            "play: both plans ended");
        player.End(engine);
        Check(!player.Active() && engine.Count("forget") == 0, "play: the end stops nothing that is over");
        const std::string line = FormatPlayLogLine(player.Stats(), "stop", 2250, 0, 0);
        Check(line.rfind("SOUND_GENERIC_PLAY,phase=stop,playhead_ms=2250,packets=2,generic=2,played=2,", 0) == 0 &&
                Has(line, ",adjusts=3,stops=1,forgets=0,releases=1,ended_by_engine=0,doubles=0,stop_all=0,"),
            "play: the log line");
    }

    // Finding 2 of the release review: the plans find their adjusts and ends
    // through a sparse index (one entry per create), not a table of nextIndex
    // entries: the session's indices of a trimmed clip far apart and up to
    // kMaxNextIndex, packets of a create the trim dropped ignored.
    void TestPlayerSparseIndices()
    {
        Track track;
        Create(track, 1000, kPed, kSetA);
        Create(track, 1200, kNoOwner, kSetB);
        track.creates[0].index = 7;
        track.creates[1].index = kMaxNextIndex - 1;
        track.nextIndex = kMaxNextIndex;
        Adjust(track, 1100, 7, Adjust::Volume, -3.0f);
        Adjust(track, 1150, 12345, Adjust::Volume, -9.0f); // its create was trimmed away
        Adjust(track, 1300, kMaxNextIndex - 1, Adjust::Pitch, 50.0f);
        Adjust(track, 1400, 7, Adjust::Pitch, 100.0f);
        End(track, 1500, 12345, EndReason::Release);
        End(track, 1800, 7, EndReason::Release);
        End(track, 2000, kMaxNextIndex - 1, EndReason::Stop);
        Player player;
        player.Begin(track, TestConfig(), owner::Cover(), owner::Overrides(), 0);
        const std::vector<SoundPlan>& plans = player.Plans();
        Check(plans.size() == 2 && plans[0].adjusts == std::vector<std::uint32_t>{0, 3} &&
                  plans[1].adjusts == std::vector<std::uint32_t>{2},
            "sparse: each plan has its own adjusts, the orphan one goes nowhere");
        Check(plans.size() == 2 && plans[0].ends == std::vector<std::uint32_t>{1} && plans[0].released &&
                  plans[0].endMs == 1800 && plans[1].ends == std::vector<std::uint32_t>{2} && !plans[1].released,
            "sparse: each plan has its own ends, the orphan release goes nowhere");
        FakeEngine engine;
        Run(player, engine, 0, 2100);
        Check(engine.Count("create_play") == 2 && engine.Count("release") == 1 && engine.Count("stop") == 1,
            "sparse: both play and end as recorded");
    }

    void TestPlayerEndedByEngine()
    {
        Track track;
        const CreatePacket a = Create(track, 1000, kPed, kSetA);
        End(track, 3000, a.index, EndReason::Release);
        FakeEngine engine;
        Player player;
        player.Begin(track, TestConfig(), owner::Cover(), owner::Overrides(), 0);
        Run(player, engine, 0, 1500);
        Check(player.Live() == 1, "engine end: live");
        engine.EndByItself(0);
        player.Tick(1550, true, engine);
        Check(player.Live() == 0 && player.Stats().endedByEngine == 1, "engine end: counted when the release is still to come");
        const CreatePacket b = Create(track, 4000, kPed, kSetB);
        End(track, 4300, b.index, EndReason::StopAndForget);
        End(track, 4400, b.index, EndReason::Release);
        player.Begin(track, TestConfig(), owner::Cover(), owner::Overrides(), 3900);
        engine.Clear();
        Run(player, engine, 3900, 4500);
        Check(engine.Count("forget") == 1 && player.Stats().forgets == 1 && player.Stats().releases == 0,
            "engine end: stop-and-forget frees the slot, the release after it is not needed");
    }

    void TestPlayerTakers()
    {
        Track track;
        Create(track, 1000, kPed, kSetA, 0x0318DAFE); // footstep: specific
        Create(track, 1000, kPed, kSetB, 0x03193E6F); // movement: puppet
        Create(track, 1000, kHorse, kHoofRef);        // horse step: specific by default, generic by the ini below
        FakeEngine engine;
        Player player;
        player.Begin(track, TestConfig(), owner::Cover(), owner::Overrides(), 0);
        Check(player.Stats().generic == 0, "takers: nothing generic by default");
        Run(player, engine, 0, 1400);
        Check(engine.calls.empty() && player.Stats().skippedSpecific == 2 && player.Stats().skippedPuppet == 1,
            "takers: specific and puppet packets are counted, not played");
        Check(player.Stats().families[static_cast<int>(owner::Family::Footstep)].skipped == 1 &&
                player.Stats().families[static_cast<int>(owner::Family::Movement)].skipped == 1 &&
                player.Stats().families[static_cast<int>(owner::Family::HorseStep)].skipped == 1,
            "takers: per family");
        player.Begin(track, TestConfig(), owner::Cover(), owner::ParseOverrides("horse_step:generic"), 0);
        Check(player.Stats().generic == 1, "takers: the ini hands the horse step to the generic");
        Run(player, engine, 0, 1400);
        Check(engine.Count("create_play") == 1 && engine.calls[0].index == 2, "takers: the horse step played");
        player::Config off = TestConfig();
        off.enabled = false;
        player.Begin(track, off, owner::Cover(), owner::ParseOverrides("horse_step:generic"), 0);
        engine.Clear();
        Run(player, engine, 0, 1400);
        Check(engine.calls.empty() && player.Stats().skippedByReason[static_cast<int>(SkipReason::Disabled)] == 3,
            "takers: SoundGenericPlay=0 counts everything as disabled");
    }

    void TestPlayerAntiDouble()
    {
        Track track;
        Create(track, 1000, kPed, kSetA);
        Create(track, 1000, kHorse, kSetA); // same set, another owner: not the same key
        FakeEngine engine;
        engine.others.push_back({kPed, static_cast<std::uint64_t>(replay::census::RefLow(kSetA))});
        Player player;
        player.Begin(track, TestConfig(), owner::Cover(), owner::Overrides(), 0);
        Run(player, engine, 0, 1400);
        Check(engine.Count("create_play") == 1 && engine.calls[0].index == 1, "anti-double: the ped's packet yields, the horse's plays");
        Check(player.Stats().yielded == 1 && player.Stats().played == 1, "anti-double: counted as yielded");
        // The double detector: the other source first, then the start.
        DoubleDetector d;
        d.Begin(250);
        const std::uint64_t key = DoubleDetector::KeyOf(kPed, kSetA);
        d.NoteOther(key, 1000);
        Check(d.NoteStart(key, 1200), "doubles: a start 200 ms after another source's sound is a double");
        Check(!d.NoteStart(key, 1600), "doubles: 600 ms after is not");
        Check(!d.NoteStart(DoubleDetector::KeyOf(kHorse, kSetA), 1000), "doubles: another owner is not");
        // The start first, then the other source: counted once.
        d.Begin(250);
        Check(!d.NoteStart(key, 2000), "doubles: a start with nobody before is clean");
        Check(d.NoteOther(key, 2100), "doubles: the other source after the start counts");
        Check(!d.NoteOther(key, 2150), "doubles: one double per start");
        d.NoteOther(key, 5000);
        Check(d.Others() == 1 && d.Starts() == 0, "doubles: the stale rows are pruned");
        // Stats: a start while another source made it.
        FakeEngine engine2;
        Player player2;
        Track track2;
        Create(track2, 1000, kPed, kSetA);
        player2.Begin(track2, TestConfig(), owner::Cover(), owner::Overrides(), 0);
        player2.Doubles().NoteOther(key, 1100);
        Run(player2, engine2, 0, 1400);
        Check(player2.Stats().played == 1 && player2.Stats().doubles == 1 &&
                player2.Stats().families[static_cast<int>(owner::Family::Owned)].doubles == 1,
            "doubles: a start next to another source's sound is counted as a double");
    }

    void TestPlayerRefusedAndMissed()
    {
        Track track;
        const CreatePacket a = Create(track, 1000, kPed, kSetA);
        End(track, 1200, a.index, EndReason::Release); // short
        FakeEngine engine;
        engine.refuse = true;
        Player player;
        player.Begin(track, TestConfig(), owner::Cover(), owner::Overrides(), 0);
        Run(player, engine, 0, 1400);
        Check(engine.Count("create_refused") == 1 && player.Stats().refused == 1 && player.Pending() == 0,
            "refused: one try, counted");
        // Retry: tried every tick while the window lasts (a puppet still coming).
        engine = FakeEngine{};
        engine.retry = true;
        player.Begin(track, TestConfig(), owner::Cover(), owner::Overrides(), 0);
        Run(player, engine, 0, 1600);
        Check(engine.Count("create_retry") == 10 && player.Pending() == 1, "retry: tried every tick, still pending");
        Run(player, engine, 1650, 1700);
        Check(player.Pending() == 0 && player.Stats().missed == 1 && player.Stats().refused == 0,
            "retry: the window (hold + 500 ms) went by, counted as missed");
        // The puppet arrives in time.
        engine = FakeEngine{};
        engine.retry = true;
        player.Begin(track, TestConfig(), owner::Cover(), owner::Overrides(), 0);
        Run(player, engine, 0, 1300);
        engine.retry = false;
        player.Tick(1350, true, engine);
        Check(engine.Count("create_play") == 1 && player.Stats().played == 1, "retry: the puppet arrived, played");
    }

    void TestPlayerPreload()
    {
        Track track;
        const CreatePacket a = Create(track, 2000, kPed, kSetA, kGameCaller, 300); // waited 300 ms in the recording
        Adjust(track, 2000, a.index, Adjust::Volume, -3.0f);
        End(track, 5000, a.index, EndReason::Release);
        Create(track, 2000, kPed, kSetB, kGameCaller, 20); // waited 20 ms: no preload
        FakeEngine engine;
        Player player;
        player.Begin(track, TestConfig(), owner::Cover(), owner::Overrides(), 0);
        Check(player.Plans()[0].preload && !player.Plans()[1].preload, "preload: the agenda marks the packet that waited");
        Run(player, engine, 0, 1450);
        Check(engine.calls.empty(), "preload: nothing before the lead (500 ms)");
        player.Tick(1500, true, engine);
        Check(engine.Count("create") == 1 && engine.Count("prepare") == 1 && engine.calls[0].index == a.index &&
                player.Plans()[0].state == SoundState::Preloaded && player.Stats().preloaded == 1,
            "preload: created and prepared 500 ms early");
        Run(player, engine, 1550, 2100);
        Check(engine.Count("play") == 0, "preload: not played before its hold ends");
        player.Tick(2150, true, engine);
        Check(engine.Count("play") == 1 && engine.Count("create_play") == 1 && engine.Count("adjust_volume") == 1,
            "preload: S11 on the prepared sound at its time; the other packet created and played at once");
        Check(player.Stats().played == 2 && player.Stats().preloaded == 1, "preload: both played, one of them preloaded");
        // A preloaded sound the puppet made itself is dropped.
        engine = FakeEngine{};
        engine.others.push_back({kPed, static_cast<std::uint64_t>(replay::census::RefLow(kSetA))});
        player.Begin(track, TestConfig(), owner::Cover(), owner::Overrides(), 0);
        Run(player, engine, 0, 2200);
        Check(engine.Count("create") == 1 && engine.Count("forget") == 1 && engine.Count("play") == 0 &&
                player.Stats().yielded == 1,
            "preload: the prepared sound is dropped when the puppet made it");
        // Preload off.
        player::Config noPreload = TestConfig();
        noPreload.preloadMs = 0;
        engine = FakeEngine{};
        player.Begin(track, noPreload, owner::Cover(), owner::Overrides(), 0);
        Run(player, engine, 0, 2200);
        Check(engine.Count("create") == 0 && engine.Count("create_play") == 2, "preload: SoundGenericPreloadMs=0 turns it off");
    }

    void TestPlayerPauseAndJump()
    {
        Track track;
        const CreatePacket loop = Create(track, 1000, kPed, kSetA); // long: 1000 to 4000
        Adjust(track, 1500, loop.index, Adjust::Volume, -10.0f);
        Adjust(track, 2500, loop.index, Adjust::Volume, -20.0f);
        End(track, 4000, loop.index, EndReason::Release);
        const CreatePacket shot = Create(track, 2000, kPed, kSetB); // short: 2000 to 2300
        End(track, 2300, shot.index, EndReason::Release);
        const CreatePacket never = Create(track, 3000, kHorse, kSetB); // never released: long by rule
        FakeEngine engine;
        Player player;
        player.Begin(track, TestConfig(), owner::Cover(), owner::Overrides(), 0);
        Run(player, engine, 0, 2200);
        Check(player.Live() == 2 && engine.Count("adjust_volume") == 1, "pause: the loop and the shot live");
        // Pause: everything stopped once.
        player.Tick(2200, false, engine);
        player.Tick(2200, false, engine);
        player.Tick(2200, false, engine);
        Check(engine.Count("forget") == 2 && player.Live() == 0 && player.Stats().stopAll == 1,
            "pause: both stopped, counted once");
        // Resume at the same playhead: the loop comes back with its state, the shot is gone.
        engine.Clear();
        player.Tick(2250, true, engine);
        Check(engine.Count("create_play") == 1 && engine.calls[0].index == loop.index && engine.Count("adjust_volume") == 1,
            "resume: the loop is created again with its latest recorded setting");
        Check(player.Stats().resumed == 1 && player.Stats().played == 2 && player.Stats().jumped == 0 &&
                player.Plans()[1].state == SoundState::Skipped && player.Plans()[1].skip == SkipReason::Jumped,
            "resume: counted as resumed (not played again); the shot's chance is gone, its first outcome (played) stays");
        Run(player, engine, 2300, 2600);
        Check(engine.Count("adjust_volume") == 2, "resume: the adjust at 2500 applied in its time");
        // A jump forward to 3500: the loop stops and comes back, the shot is over, the never-released one is alive.
        engine.Clear();
        player.Tick(3500, true, engine); // a step of 900 ms is a jump
        Check(engine.Count("forget") == 1 && player.Live() == 0, "jump: the live sound stopped");
        player.Tick(3550, true, engine);
        Check(engine.Count("create_play") == 2 && player.Live() == 2 && player.Stats().resumed == 3 && player.Stats().played == 3,
            "jump: the loop and the never-released sound come back (the latter's first outcome is this start)");
        Check(player.Stats().jumped == 0 && player.Plans()[1].state == SoundState::Ended,
            "jump: the shot over before the playhead is not a skip");
        // A jump back to 1200: only the loop is alive there.
        engine.Clear();
        player.Tick(1200, true, engine);
        Check(engine.Count("forget") == 2 && player.Live() == 0, "jump back: everything stopped");
        player.Tick(1250, true, engine);
        Check(engine.Count("create_play") == 1 && engine.calls.back().index == loop.index && player.Live() == 1,
            "jump back: the loop comes back, the rest is ahead again");
        Run(player, engine, 1300, 2200);
        Check(engine.Count("create_play") == 2 && player.Live() == 2, "jump back: the shot plays again at its time");
        Check(player.Stats().played == 3 && player.Stats().packets == 3, "jump back: the stats count each packet once");
        // Scrub (not normal play) then the end.
        player.Tick(2300, false, engine);
        Check(player.Live() == 0, "scrub: stopped");
        player.End(engine);
        Check(!player.Active(), "end: inactive");
    }

    // SoundGenericKeepLoops: a jump in normal play keeps the live loops whose
    // recorded life covers the new playhead; the short sounds and the loops
    // whose life does not cover it stop; pause still stops everything.
    void TestPlayerKeepLoops()
    {
        Track track;
        const CreatePacket loop = Create(track, 1000, kPed, kSetA); // long: 1000 to 6000
        Adjust(track, 1500, loop.index, Adjust::Volume, -10.0f);
        Adjust(track, 2500, loop.index, Adjust::Volume, -20.0f);
        End(track, 6000, loop.index, EndReason::Release);
        const CreatePacket shot = Create(track, 2000, kPed, kSetB); // short: 2000 to 2300
        End(track, 2300, shot.index, EndReason::Release);
        const CreatePacket late = Create(track, 3000, kHorse, kSetB); // long, never released, starts at 3000
        (void)late;
        FakeEngine engine;
        engine.keep = true;
        Player player;
        player::Config config = TestConfig();
        config.keepLoops = true;
        player.Begin(track, config, owner::Cover(), owner::Overrides(), 0);
        Run(player, engine, 0, 3200);
        Check(player.Live() == 2, "keep: the loop and the late one live at 3200");
        // Jump back to 2700 in normal play (a pass wrap or a seek while playing).
        engine.Clear();
        player.Tick(2700, true, engine);
        Check(engine.Count("forget") == 1 && player.Live() == 1 && player.Stats().keptLoops == 1,
            "keep: the loop covering 2700 goes on, the one starting at 3000 stops");
        player.Tick(2750, true, engine);
        Check(engine.Count("create_play") == 0, "keep: the loop is not created again");
        Check(engine.Count("adjust_volume") == 2, "keep: its adjusts walked again up to the playhead (state at 2750)");
        Run(player, engine, 2800, 3300);
        Check(engine.Count("create_play") == 1 && player.Live() == 2, "keep: the late one starts again at its time");
        // A jump past the loop's end: nothing covers it, it stops.
        engine.Clear();
        player.Tick(7000, true, engine);
        Check(engine.Count("forget") == 1 && player.Live() == 1 && player.Stats().keptLoops == 2,
            "keep: the never-released one covers 7000 and goes on, the loop over at 6000 stops");
        // The engine side says no (another puppet after the jump): stopped and created again.
        engine.keep = false;
        engine.Clear();
        player.Tick(3500, true, engine);
        Check(engine.Count("forget") == 1 && player.Live() == 0, "keep: refused by the engine side, stopped");
        player.Tick(3550, true, engine);
        Check(engine.Count("create_play") == 2, "keep: then the loop and the late one created again (resumed)");
        // Pause: everything stops, kept or not.
        engine.keep = true;
        engine.Clear();
        player.Tick(3550, false, engine);
        Check(engine.Count("forget") == 2 && player.Live() == 0, "keep: pause stops everything");
        // Off: the jump stops every live sound.
        engine = FakeEngine{};
        engine.keep = true;
        player.Begin(track, TestConfig(), owner::Cover(), owner::Overrides(), 0);
        Run(player, engine, 0, 3200);
        engine.Clear();
        player.Tick(2700, true, engine);
        Check(engine.Count("forget") == 2 && player.Stats().keptLoops == 0, "keep off: the jump stops every live sound");
        Check(Has(FormatPlayLogLine(player.Stats(), "play", 2700, 0, 0), ",kept_loops=0"), "keep: the log line carries kept_loops");
    }

    void TestPlayerUnsortedTrack()
    {
        Track track;
        const CreatePacket late = Create(track, 3000, kPed, kSetA);
        const CreatePacket early = Create(track, 1000, kPed, kSetB);
        FakeEngine engine;
        Player player;
        player.Begin(track, TestConfig(), owner::Cover(), owner::Overrides(), 0);
        Check(player.Plans()[0].create == early.index && player.Plans()[1].create == late.index,
            "unsorted: the plans are in time order");
        Run(player, engine, 0, 1400);
        Check(engine.Count("create_play") == 1 && engine.calls[0].index == early.index, "unsorted: the early one plays first");
        Run(player, engine, 1450, 3400);
        Check(engine.Count("create_play") == 2 && engine.calls.back().index == late.index, "unsorted: then the late one");
    }

    void TestPlayerSection()
    {
        Track track;
        Create(track, 1000, kPed, kSetA);
        Create(track, 1000, kPed, kSetB, 0x0318DAFE);
        Create(track, 1000, kNoOwner, kSetB);
        FakeEngine engine;
        Player player;
        player.Begin(track, TestConfig(), owner::Cover(), owner::Overrides(), 0);
        Run(player, engine, 0, 1400);
        const std::string section = FormatSection(player.Stats(), owner::ParseOverrides("melee:generic"), "S10:ok;S13:ok");
        Check(section.rfind("== generic sound packets (SoundGeneric=1) ==\n", 0) == 0, "section: the header");
        Check(Has(section, "Packets recorded 3, given to the generic player 2, played 2 (0 preloaded, 0 resumed after a jump), refused by the engine 0, yielded to another source 0, missed their window 0, skipped by a jump 0,"),
            "section: the totals");
        Check(Has(section, "Taker table (SoundGenericMode=legacy): world:generic;owned:generic;speech:specific;footstep:specific;horse_step:specific;movement:puppet;melee:generic(ini);"),
            "section: the taker table");
        Check(Has(section, "Engine ports: S10:ok;S13:ok\n"), "section: the ports");
        // The unplaced column after refused.
        Check(Has(section, "world          generic          1       1         0       0        0       0       0      0       0\n") &&
                Has(section, "owned          generic          1       1         0       0        0       0       0      0       0\n") &&
                Has(section, "footstep       specific         1       0         0       0        0       1       0      0       0\n"),
            "section: one row per family with packets");
        Check(!Has(section, "\nmelee "), "section: a family without packets has no row");
        Check(Has(section, "How to read:"), "section: the how-to-read");
    }

    // The QA mark's window over the player's books: the latest outcome per
    // packet, kept across a pause; by family with its taker, by owner, the
    // failures.
    void TestPlayerWindowReport()
    {
        Track track;
        const CreatePacket a = Create(track, 1000, kHorse, 0x80000e002f4361ull); // generic (owned), plays
        const CreatePacket b = Create(track, 1200, kHorse, 0x80000e002f4361ull); // generic, refused
        const CreatePacket c = Create(track, 1400, kPed, 0x80000e00111111ull, 0x0318E6F0u); // footstep: specific track
        const CreatePacket d = Create(track, 1600, kHorse, 0x80000e002f5642ull); // yields to the puppet
        const CreatePacket e = Create(track, 9000, kHorse, 0x80000e002f5642ull); // not reached
        (void)a; (void)c; (void)e;
        End(track, 1100, 0, EndReason::Release);
        End(track, 1300, 1, EndReason::Release);
        End(track, 1500, 2, EndReason::Release);
        End(track, 1700, 3, EndReason::Release);
        FakeEngine engine;
        Player player;
        owner::Cover cover;
        player.Begin(track, TestConfig(), cover, owner::Overrides{}, 0);
        Run(player, engine, 0, 1150);
        engine.refuse = true;
        Run(player, engine, 1200, 1500);
        engine.refuse = false;
        engine.others.push_back({kHorse, replay::census::RefLow(d.stableId)});
        Run(player, engine, 1550, 2000);
        Check(player.Stats().played == 1 && player.Stats().refused == 1 && player.Stats().yielded == 1 && player.Stats().skippedSpecific == 1,
            "window: the four outcomes");
        // A pause resets the plans' states; the outcomes stay.
        player.Tick(2500, false, engine);
        player.Tick(2550, true, engine);
        const WindowReport w = player.Window(500, 2600, owner::Overrides{});
        Check(w.packets == 4 && w.counts[static_cast<int>(Outcome::Played)] == 1 && w.counts[static_cast<int>(Outcome::Refused)] == 1 &&
                w.counts[static_cast<int>(Outcome::Yielded)] == 1 && w.counts[static_cast<int>(Outcome::Specific)] == 1,
            "window: four packets in the window with their last outcome (kept across the pause)");
        Check(w.families.size() == 2, "window: two families with packets");
        bool owned = false, foot = false;
        for (const WindowFamily& f : w.families)
        {
            owned = owned || (f.family == owner::Family::Owned && f.taker == owner::Taker::Generic && f.packets == 3 &&
                                 f.counts[static_cast<int>(Outcome::Refused)] == 1);
            foot = foot || (f.family == owner::Family::Footstep && f.taker == owner::Taker::Specific && f.packets == 1 &&
                               f.counts[static_cast<int>(Outcome::Specific)] == 1);
        }
        Check(owned && foot, "window: the family rows carry the taker (who owned the sound)");
        Check(w.owners.size() == 2 && w.owners[0].owner == kHorse && w.owners[0].packets == 3 && w.owners[1].owner == kPed,
            "window: owner rows, most packets first");
        Check(w.failures.size() == 1 && w.failures[0].timeMs == 1200 && w.failures[0].owner == kHorse &&
                w.failures[0].outcome == Outcome::Refused && w.failures[0].set == 0x2f4361u && !w.failures[0].doubled,
            "window: the refused packet is the one failure (yielded and specific are not failures)");
        const WindowReport later = player.Window(8000, 10000, owner::Overrides{});
        Check(later.packets == 1 && later.counts[static_cast<int>(Outcome::None)] == 1 && later.failures.empty(),
            "window: a packet not reached counts as none");
        Check(player.Window(3000, 4000, owner::Overrides{}).packets == 0, "window: an empty window");
        Check(std::string(OutcomeName(Outcome::Missed)) == "missed" && OutcomeOfSkip(SkipReason::Jumped) == Outcome::Jumped &&
                OutcomeOfSkip(SkipReason::None) == Outcome::None,
            "window: outcome names and the skip mapping");
        (void)b;
    }
    // SoundGenericExport=0 stops the player while an export owns the clock;
    // the export starts with the playback already running.
    void TestPlayerExportOff()
    {
        Check(player::PlayAllowed(true, false, false) && !player::PlayAllowed(true, false, true) &&
                player::PlayAllowed(true, true, true) && player::PlayAllowed(true, true, false) &&
                !player::PlayAllowed(false, true, false) && !player::PlayAllowed(false, false, true),
            "export off: SoundGenericExport=0 stops the player only while exporting, SoundGenericPlay=0 always");
        Track track;
        Create(track, 1000, kHorse, kHoofRef); // before the export: plays
        Create(track, 3000, kHorse, kHoofRef); // due while the export runs: disabled
        Create(track, 5000, kHorse, kHoofRef); // after the export: plays
        FakeEngine engine;
        Player player;
        player.Begin(track, TestConfig(), owner::Cover(), owner::ParseOverrides("horse_step:generic"), 0);
        Run(player, engine, 0, 2000);
        Check(engine.Count("create_play") == 1 && engine.Count("forget") == 0, "export off: the first packet played before the export");
        Check(!player.SetEnabled(true, engine), "export off: no change when already on");
        Check(player.SetEnabled(false, engine), "export off: the export's start turns the player off");
        Check(engine.Count("forget") == 1 && player.Stats().stopAll == 1, "export off: the live sound stopped at once");
        Run(player, engine, 2000, 4000);
        Check(engine.Count("create_play") == 1 && player.Stats().skippedByReason[static_cast<int>(SkipReason::Disabled)] == 1,
            "export off: the packet due during the export is counted as disabled, not played");
        Check(!player.SetEnabled(false, engine), "export off: no change while still exporting");
        Check(player.SetEnabled(true, engine), "export off: the export's end turns the player on again");
        Run(player, engine, 4000, 6000);
        Check(engine.Count("create_play") == 2 && engine.Count("forget") == 1, "export off: the packet after the export played");
    }

    // SoundGenericMode=packets. The horse's skid: a new sound every 16 ms with
    // adjusts, a stop and forget 16 ms later and the release later still,
    // carried by ESND rows (the cover names the family entity_sound).
    void TestPlayerPacketsMode()
    {
        constexpr std::uint64_t kSkid = 0x0080000e002f4361ull;
        constexpr std::uint32_t kSkidCaller = 0x0251688D;
        const auto build = [&](Track& track, owner::Cover& cover) {
            for (std::uint32_t n = 0; n < 3; ++n)
            {
                const std::uint32_t t = 45541 + 16 * n;
                const CreatePacket c = Create(track, t, kHorse, kSkid, kSkidCaller);
                cover.Add(kHorse, kSkid, t, owner::Family::EntitySound);
                Adjust(track, t, c.index, Adjust::Volume, -3.0f);
                Adjust(track, t, c.index, Adjust::Pitch, 120.0f);
                Adjust(track, t + 8, c.index, Adjust::Volume, -6.0f);
                End(track, t + 16, c.index, EndReason::StopAndForget);
                End(track, t + 40, c.index, EndReason::Release);
            }
            Create(track, 46000, kNoOwner, kSetA);                  // a world sound: keeps the hold
            Create(track, 46100, kHorse, kSetB, 0x0318DAFE);        // a step: the footstep track's
            // The owner is the horse (its step keeps its track; a person's
            // step is a complement of the step call).
            track.creates.back().ownerClass = OwnerClass::Horse;
        };
        {
            Track track;
            owner::Cover cover;
            build(track, cover);
            FakeEngine engine;
            Player player;
            player::Config config = TestConfig();
            config.packets = true;
            owner::Overrides overrides;
            overrides.packets = true;
            // The step at 46.1 s has its recorded foot event (the step track
            // remakes it); without one it would be the generic player's.
            owner::FootWindows feet;
            feet.Add(kHorse, 46080, 0);
            feet.Seal();
            player.Begin(track, config, cover, overrides, 45400, &feet);
            Check(player.Stats().fromTracks == 3, "packets: the three skid packets come from the ESND track's family");
            Run(player, engine, 45400, 45592, 16); // ticks 45544, 45560, 45576, 45592
            Check(engine.Count("create_play") == 3, "packets: every skid sound starts at the first tick of its time (no hold)");
            Run(player, engine, 45608, 46504, 16);
            Check(engine.Count("forget") == 3, "packets: each skid sound gets its recorded stop and forget");
            Check(engine.Count("adjust_volume") == 6 && engine.Count("adjust_pitch") == 3, "packets: the recorded adjusts are applied");
            Check(engine.Count("create_play") == 4, "packets: the world sound plays (after its hold); the step is left to its track");
            Check(player.Stats().skippedSpecific == 1, "packets: the footstep family keeps its track");
            Check(Has(FormatSection(player.Stats(), overrides, ""), "Taker table (SoundGenericMode=packets): world:generic;owned:generic;speech:specific;footstep:specific;horse_step:specific;movement:generic;melee:specific;gunshot:specific;explosion:specific;collision:generic;entity_sound:generic;sound_request:generic;captured:generic"),
                "packets: the table in the report");
            Check(Has(FormatSection(player.Stats(), overrides, ""), "played here with their recorded adjusts: 3 packets."),
                "packets: the report says what came from the tracks");
        }
        {
            Track track;
            owner::Cover cover;
            build(track, cover);
            FakeEngine engine;
            Player player;
            player.Begin(track, TestConfig(), cover, owner::Overrides{}, 45400);
            Run(player, engine, 45400, 46500, 16);
            Check(engine.Count("create_play") == 1 && player.Stats().fromTracks == 0,
                "legacy: the skid stays on the ESND track, only the world sound plays");
        }
    }
}

namespace
{
    // SoundGenericEntityMode's pure part.
    void TestEntityMode()
    {
        Check(EntityModeOfText("a") == EntityMode::Detached && EntityModeOfText("") == EntityMode::Detached &&
                EntityModeOfText("zz") == EntityMode::Detached,
            "entity mode: a, empty and unknown are detached");
        Check(EntityModeOfText(" B ") == EntityMode::Override && EntityModeOfText("override") == EntityMode::Override,
            "entity mode: b is override");
        Check(EntityModeOfText("c\r\n") == EntityMode::GameQueue && EntityModeOfText("Game_Queue") == EntityMode::GameQueue,
            "entity mode: c is the game queue");
        Check(std::strcmp(EntityModeLetter(EntityMode::GameQueue), "c") == 0 &&
                std::strcmp(EntityModeName(EntityMode::Override), "override") == 0,
            "entity mode: letters and names");
        Check(EntityModeApplies(owner::Family::EntitySound, kHorse) && EntityModeApplies(owner::Family::SoundRequest, kHorse),
            "entity mode: ESND and SNDS packets of an owner");
        Check(!EntityModeApplies(owner::Family::EntitySound, kNoOwner) && !EntityModeApplies(owner::Family::Collision, kHorse) &&
                !EntityModeApplies(owner::Family::Owned, kHorse),
            "entity mode: not the world's, not the other families");
        Check(DetachedFlags(0x24) == 0x20 && DetachedFlags(0x20) == 0x20, "entity mode a: only the flag 0x04 goes");

        InitParams params;
        params.volume = -3.0f;
        params.pitch = 120;
        params.variableCount = 1;
        params.variables[0] = Variable{0x11111111u, 1.0f};
        InitParams vars;
        vars.initVariableCount = 2;
        vars.initVariables[0] = Variable{0x48fe569du, 5.5f}; // the skid's speed
        vars.initVariables[1] = Variable{0x11111111u, 2.0f};
        OverrideState state;
        state.Seed(params, &vars);
        Check(state.VariableCount() == 2 && state.volume == -3.0f && state.pitch == 120,
            "override: seeded with the create's variables (one hash once), volume and pitch");
        Check(state.VariableAt(1).value == 1.0f, "override: the params' variable after the init one");
        {
            // A +0x10C dword that is a NaN as a float never becomes a
            // variable value.
            InitParams nan = params;
            const std::uint32_t raw = 0xFFFD933Fu;
            std::memcpy(&nan.variables[0].value, &raw, 4);
            OverrideState nanState;
            nanState.Seed(nan, nullptr);
            Check(nanState.VariableCount() == 0, "override: a NaN dword of +0x10C is not seeded as a variable");
        }
        AdjustPacket a;
        a.kind = Adjust::Variable;
        a.hash = 0x48fe569du;
        a.value[0] = 0.25f;
        state.Note(a);
        a.kind = Adjust::Volume;
        a.value[0] = -12.0f;
        state.Note(a);
        a.kind = Adjust::Pitch;
        a.value[0] = -49.6f;
        state.Note(a);
        a.kind = Adjust::Position;
        a.value[0] = 99.0f;
        state.Note(a);
        a.kind = Adjust::Variable;
        a.hash = 0x22222222u;
        a.value[0] = 7.0f;
        state.Note(a);
        Check(state.VariableCount() == 3 && state.VariableAt(0).value == 0.25f && state.VariableAt(2).value == 7.0f,
            "override: S38 updates the latest value, a new hash is added");
        Check(state.volume == -12.0f && state.pitch == -50, "override: the last S18 and S19 (rounded)");
        a.hash = 0;
        state.Note(a);
        Check(state.VariableCount() == 3, "override: a null hash is not kept");
        for (std::uint32_t n = 0; n < 64; ++n)
        {
            a.hash = 0x30000000u + n;
            state.Note(a);
        }
        Check(state.VariableCount() == OverrideState::kMax, "override: bounded");
    }
    // SoundGenericSkip: the skipped ports are not called and are counted per
    // item; a skipped end keeps the sound live.
    void TestPlayerSkip()
    {
        Check(player::ParseSkip("") == 0 && player::ParseSkip("s18 ; S38,stop;release;bogus") ==
                    ((1u << 2) | (1u << 8) | (1u << player::kSkipStop) | (1u << player::kSkipRelease)),
            "skip: the items parsed, case and spaces ignored, unknown dropped");
        Track track;
        const CreatePacket a = Create(track, 1000, kPed, kSetA);
        Adjust(track, 1300, a.index, Adjust::Volume, -3.0f);
        Adjust(track, 1400, a.index, Adjust::Pitch, 100.0f);
        End(track, 1800, a.index, EndReason::Stop);
        End(track, 2200, a.index, EndReason::Release);
        FakeEngine engine;
        Player player;
        player::Config c = TestConfig();
        c.skipMask = player::ParseSkip("S18;stop;release");
        player.Begin(track, c, owner::Cover(), owner::Overrides(), 0);
        Run(player, engine, 0, 2400);
        Check(engine.Count("create_play") == 1 && engine.Count("adjust_volume") == 0 && engine.Count("adjust_pitch") == 1,
            "skip: S18 not called, S19 called");
        Check(engine.Count("stop") == 0 && engine.Count("release") == 0 && player.Live() == 1,
            "skip: the stop and the release not called, the sound stays live");
        const player::PlayStats& s = player.Stats();
        Check(s.skippedByKey[2] == 1 && s.skippedByKey[player::kSkipStop] == 1 && s.skippedByKey[player::kSkipRelease] == 1 &&
                s.adjusts == 1 && s.stops == 0 && s.releases == 0,
            "skip: counted per item");
        player.End(engine);
    }

    // SoundGenericEntityDrivenSkip only touches the packets whose sound
    // updates with its entity (params flag 0x04) and keeps it.
    void TestEntityDrivenSkip()
    {
        Track track;
        const CreatePacket driven = Create(track, 1000, kNoOwner, kSetA);
        track.creates.back().params.flags = 0x24;
        const CreatePacket plain = Create(track, 1000, kNoOwner, kSetB);
        track.creates.back().params.flags = 0x20;
        for (const CreatePacket* c : {&driven, &plain})
        {
            Adjust(track, 1300, c->index, Adjust::Volume, -20.0f);
            Adjust(track, 1300, c->index, Adjust::Pitch, 50.0f);
            End(track, 1600, c->index, EndReason::Release);
        }
        player::Config config = TestConfig();
        config.entityDrivenMask = player::ParseSkip("S18,stop,forget,release");
        FakeEngine engine;
        Player player;
        player.Begin(track, config, owner::Cover(), owner::Overrides(), 0);
        Check(player.Stats().entityDriven == 1, "entity driven: one packet keeps the update by its entity");
        Run(player, engine, 0, 2000);
        std::size_t drivenVolume = 0, plainVolume = 0, drivenPitch = 0, drivenRelease = 0, plainRelease = 0;
        std::uint64_t drivenHandle = kNoHandle;
        for (const auto& call : engine.calls)
            if (call.what == "create_play" && call.index == driven.index)
                drivenHandle = call.handle;
        for (const auto& call : engine.calls)
        {
            const bool isDriven = call.what.rfind("adjust_", 0) == 0 ? call.index == driven.index : call.handle == drivenHandle;
            if (call.what == "adjust_volume")
                ++(isDriven ? drivenVolume : plainVolume);
            if (call.what == "adjust_pitch" && isDriven)
                ++drivenPitch;
            if (call.what == "release")
                ++(isDriven ? drivenRelease : plainRelease);
        }
        Check(drivenVolume == 0 && plainVolume == 1, "entity driven: the recorded volume is left to the entity, the plain sound gets it");
        Check(drivenPitch == 1, "entity driven: the other adjusts still apply");
        Check(drivenRelease == 0 && plainRelease == 1, "entity driven: the recorded end is left to the entity and the safety net");
    }

    // StartValues. A wagon sound (0x44546c index 73) created at 3105 with the
    // create's volume sample 0 dB, the -88.6 dB the owner set in that frame
    // recorded at 3171 (the throttle); stop-and-forget at 3155. Without this
    // the copy plays its whole life at 0 dB. A ramp's later samples and a
    // sample that is not at the create are left alone.
    void TestStartValues()
    {
        Track track;
        const CreatePacket blip = Create(track, 3105, kNoOwner, kSetA);
        Adjust(track, 3105, blip.index, Adjust::Volume, 0.0f);
        Adjust(track, 3105, blip.index, Adjust::Pitch, 0.0f);
        Adjust(track, 3105, blip.index, Adjust::LowPass, 20000.0f);
        End(track, 3155, blip.index, EndReason::StopAndForget);
        Adjust(track, 3171, blip.index, Adjust::Volume, -88.642f);
        Adjust(track, 3171, blip.index, Adjust::Pitch, -144.0f);
        End(track, 3173, blip.index, EndReason::Release);
        const CreatePacket ramp = Create(track, 4000, kNoOwner, kSetB);
        Adjust(track, 4000, ramp.index, Adjust::Volume, -17.0f);
        Adjust(track, 4050, ramp.index, Adjust::Volume, -13.0f);
        Adjust(track, 4120, ramp.index, Adjust::Volume, -12.0f); // beyond the window: at its time
        Adjust(track, 4060, ramp.index, Adjust::Pitch, 10.0f);  // no sample at the create: at its time
        const CreatePacket same = Create(track, 5000, kNoOwner, kSetA);
        Adjust(track, 5000, same.index, Adjust::Volume, -3.0f);
        Adjust(track, 5050, same.index, Adjust::Volume, -3.0f);

        std::vector<std::uint32_t> blipAdjusts, rampAdjusts, sameAdjusts;
        for (std::uint32_t n = 0; n < track.adjusts.size(); ++n)
            (track.adjusts[n].index == blip.index ? blipAdjusts : track.adjusts[n].index == ramp.index ? rampAdjusts : sameAdjusts).push_back(n);
        // The track's adjusts are in time order per sound in a recording; sort the ramp's.
        std::sort(rampAdjusts.begin(), rampAdjusts.end(),
            [&](std::uint32_t a, std::uint32_t b) { return track.adjusts[a].timeMs < track.adjusts[b].timeMs; });
        const auto b = StartValues(track, 3105, blipAdjusts);
        Check(b.size() == 2, "start values: the blip's volume and pitch start with the frame's value");
        if (b.size() == 2)
            Check(track.adjusts[b[0].second].value[0] == -88.642f && track.adjusts[b[1].second].value[0] == -144.0f,
                "start values: the values recorded one interval later");
        const auto r = StartValues(track, 4000, rampAdjusts);
        Check(r.size() == 1 && track.adjusts[r[0].second].value[0] == -13.0f, "start values: only the ramp's first step, not the later ones nor a setting without a create sample");
        Check(StartValues(track, 5000, sameAdjusts).empty(), "start values: the same value again changes nothing");

        // The player: the blip's create plays at -88.6 dB, never at 0 dB.
        player::Config config = TestConfig();
        FakeEngine engine;
        Player player;
        Track only;
        const CreatePacket c = Create(only, 3105, kNoOwner, kSetA);
        Adjust(only, 3105, c.index, Adjust::Volume, 0.0f);
        End(only, 3155, c.index, EndReason::StopAndForget);
        Adjust(only, 3171, c.index, Adjust::Volume, -88.642f);
        End(only, 3173, c.index, EndReason::Release);
        player.Begin(only, config, owner::Cover(), owner::Overrides(), 0);
        Run(player, engine, 3000, 3400);
        bool sawZero = false, sawLow = false;
        for (const auto& v : engine.adjustValues)
            if (v.first == static_cast<std::uint32_t>(Adjust::Volume))
            {
                sawZero = sawZero || v.second == 0.0f;
                sawLow = sawLow || v.second < -80.0f;
            }
        Check(sawLow && !sawZero, "start values: the player gives the copy the frame's -88.6 dB from its create (before: 0 dB until the forget)");
        Check(player.Stats().startValues == 1, "start values: counted (start_values=)");
    }

    // With the default SoundGenericEntityDrivenSkip an entity driven packet
    // still gets its recorded stop-and-forget (a wagon sound created and
    // forgotten 15 ms later, 64 times; skipped, every copy would live on), and
    // its recorded volume; only the release stays skipped.
    void TestEntityDrivenDefaultKeepsStops()
    {
        Track track;
        std::vector<CreatePacket> blips;
        for (std::uint32_t n = 0; n < 4; ++n)
        {
            blips.push_back(Create(track, 1000 + n * 200, kNoOwner, kSetA));
            track.creates.back().params.flags = 0x24;
            Adjust(track, 1005 + n * 200, blips.back().index, Adjust::Volume, -20.0f);
            End(track, 1015 + n * 200, blips.back().index, EndReason::StopAndForget);
            End(track, 1035 + n * 200, blips.back().index, EndReason::Release);
        }
        player::Config config = TestConfig();
        config.entityDrivenMask = player::ParseSkip(kEntityDrivenSkipDefault);
        FakeEngine engine;
        Player player;
        player.Begin(track, config, owner::Cover(), owner::Overrides(), 0);
        Check(player.Stats().entityDriven == 4, "entity driven default: the four blips update with their entity");
        Run(player, engine, 0, 2000);
        std::size_t forgets = 0, volumes = 0;
        for (const auto& call : engine.calls)
        {
            if (call.what == "forget")
                ++forgets;
            if (call.what == "adjust_volume")
                ++volumes;
        }
        Check(forgets == 4, "entity driven default: every recorded stop-and-forget ends its copy (before: 0, the copies piled up)");
        Check(volumes == 4, "entity driven default: the recorded volume applies (the wagon set 0x44546c to -70 to -90 dB)");
        Check(player.Stats().skippedByKey[kSkipForget] == 0,
            "entity driven default: no forget skipped");
    }

    // Mode c's sounds are held by their identity; a freed or reused sound is
    // never stopped, a second stop finds nothing, a reset forgets without a
    // call.
    struct FakeLinks : SoundLinks
    {
        std::map<std::uint64_t, std::uint64_t> live; // sound -> identity (the engine's pool)
        std::vector<std::uint64_t> stops;
        std::uint64_t Identity(std::uint64_t sound) const override
        {
            const auto found = live.find(sound);
            return found == live.end() ? 0 : found->second;
        }
        bool Stop(std::uint64_t sound) override
        {
            stops.push_back(sound);
            return live.count(sound) != 0;
        }
    };

    void TestLinkedSounds()
    {
        constexpr std::uint64_t kA = 0x2D4000001000ull;
        constexpr std::uint64_t kB = 0x2D4000002000ull;
        {
            // The queue's sound ended before the stop.
            FakeLinks links;
            StaleCounters c;
            links.live[kA] = 0x1111;
            LinkedSound link;
            Link(link, kA, links, c);
            Check(link.identity == 0x1111 && c.linked == 1, "linked: identity taken at the birth");
            Check(LiveSound(link, links) == kA, "linked: alive while the identity holds");
            links.live.erase(kA); // the engine ends and frees it
            Check(LiveSound(link, links) == 0, "linked: gone once freed");
            Check(!StopLinked(link, links, c), "released before stop: nothing stopped");
            Check(links.stops.empty(), "released before stop: no engine call");
            Check(c.stopSkipped == 1 && c.Stale() == 1, "released before stop: counted stale");
            Check(link.sound == 0, "released before stop: let go");
        }
        {
            // The address reused by another sound (another settings block).
            FakeLinks links;
            StaleCounters c;
            links.live[kA] = 0x1111;
            LinkedSound link;
            Link(link, kA, links, c);
            links.live[kA] = 0x2222;
            Check(LiveSound(link, links) == 0, "reused address: not our sound");
            StopLinked(link, links, c);
            Check(links.stops.empty() && c.stopSkipped == 1, "reused address: no stop of the new sound");
        }
        {
            // Stop twice.
            FakeLinks links;
            StaleCounters c;
            links.live[kA] = 0x1111;
            LinkedSound link;
            Link(link, kA, links, c);
            Check(StopLinked(link, links, c), "double stop: the first stops");
            Check(!StopLinked(link, links, c), "double stop: the second finds nothing");
            Check(links.stops.size() == 1 && c.stops == 1 && c.stopSkipped == 0, "double stop: one engine call, nothing stale");
        }
        {
            // Stop after a reset (save loaded): the net forgets, no call.
            FakeLinks links;
            StaleCounters c;
            links.live[kA] = 0x1111;
            links.live[kB] = 0x2222;
            LinkedSound a, b;
            Link(a, kA, links, c);
            Link(b, kB, links, c);
            LinkedSet net;
            net.Insert(a);
            net.Insert(b);
            net.Forget(c);
            net.StopAll(links, c);
            Check(links.stops.empty(), "stop after reset: no engine call");
            Check(c.forgotten == 2 && net.Size() == 0, "stop after reset: both forgotten");
        }
        {
            // The safety net stops only the sounds still alive.
            FakeLinks links;
            StaleCounters c;
            links.live[kA] = 0x1111;
            links.live[kB] = 0x2222;
            LinkedSound a, b;
            Link(a, kA, links, c);
            Link(b, kB, links, c);
            LinkedSet net;
            net.Insert(a);
            net.Insert(b);
            links.live.erase(kA);
            net.StopAll(links, c);
            Check(links.stops.size() == 1 && links.stops[0] == kB, "net: only the live sound is stopped");
            Check(c.netStops == 1 && c.netSkipped == 1, "net: one stop, one skipped");
        }
        {
            // Born gone, and an unbound birth already gone.
            FakeLinks links;
            StaleCounters c;
            LinkedSound link;
            Link(link, kA, links, c);
            Check(link.identity == 0 && c.bornGone == 1, "born gone: no identity");
            Check(LiveSound(link, links) == 0, "born gone: never used");
            LinkedSet net;
            net.Insert(link);
            Check(net.Size() == 0, "born gone: not in the net");
            Check(!StopUnbound(kB, links, c) && links.stops.empty() && c.unboundSkipped == 1, "unbound gone: no call");
            links.live[kB] = 0x2222;
            Check(StopUnbound(kB, links, c) && c.netStops == 1, "unbound alive: stopped");
        }
        {
            // Seen gone once while held (the engine ended it): counted once.
            FakeLinks links;
            StaleCounters c;
            links.live[kA] = 0x1111;
            LinkedSound link;
            Link(link, kA, links, c);
            links.live.erase(kA);
            Check(CheckLinked(link, links, c) == 0 && CheckLinked(link, links, c) == 0, "seen gone: zero");
            Check(c.seenGone == 1 && c.Stale() == 0, "seen gone: once, not stale (no call was asked)");
            const std::string line = StaleLine("playback_end", c);
            Check(line.rfind("SOUND_GENERIC_STALE,why=playback_end,stale=0,linked=1,", 0) == 0, "stale line: head");
            Check(Has(line, ",seen_gone=1,") && Has(line, ",forgotten=0"), "stale line: fields");
        }
    }
}

int RunSoundGenericPlayerTests()
{
    TestLinkedSounds();
    TestEntityDrivenSkip();
    TestEntityDrivenDefaultKeepsStops();
    TestStartValues();
    TestPlayerSkip();
    TestEntityMode();
    TestOwnerTableFamilies();
    TestOwnerTableTakers();
    TestPlayerNormalPlay();
    TestPlayerSparseIndices();
    TestPlayerEndedByEngine();
    TestPlayerTakers();
    TestPlayerAntiDouble();
    TestPlayerRefusedAndMissed();
    TestPlayerPreload();
    TestPlayerPauseAndJump();
    TestPlayerKeepLoops();
    TestPlayerUnsortedTrack();
    TestPlayerSection();
    TestPlayerWindowReport();
    TestPlayerExportOff();
    TestPlayerPacketsMode();
    if (g_failures)
        std::fprintf(stderr, "%d generic sound player test(s) failed.\n", g_failures);
    return g_failures;
}
