// The clip area and the
// fx placements of no world position, the puppet one-shot rule, the merge
// of a sound requested every frame, and the registered effect rules.
#include "clip_area.h"
#include "puppet_audio_rule.h"
#include "registered_fx_rule.h"
#include "sound_check_core.h"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace
{
    int g_soundParticleFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_soundParticleFailures;
    }

    void TestPositionValid()
    {
        using replay::cliparea::PositionValid;
        // A recorded scene and its six puffs.
        Check(PositionValid(-1395.0f, -330.0f, 95.0f), "a scene position is valid");
        Check(!PositionValid(0.0f, -0.03f, 0.0f), "the puff at the map origin is refused");
        Check(!PositionValid(3.0f, 2.0f, 120.0f), "within 5 m of the origin is refused at any height");
        Check(PositionValid(6.0f, 0.0f, 50.0f), "6 m from the origin is a position");
        Check(!PositionValid(std::numeric_limits<float>::quiet_NaN(), 10.0f, 10.0f), "NaN is refused");
        Check(!PositionValid(std::numeric_limits<float>::infinity(), 10.0f, 10.0f), "infinity is refused");
        Check(!PositionValid(20000.0f, 10.0f, 10.0f), "outside the map is refused");
        Check(!PositionValid(100.0f, 10.0f, 9000.0f), "far above the map is refused");
    }

    void TestFxWorldUnresolved()
    {
        using replay::cliparea::FxWorldUnresolved;
        float zero[16] = {};
        float parent[16] = {};
        parent[0] = parent[5] = parent[10] = parent[15] = 1.0f;
        parent[12] = -1100.0f;
        Check(FxWorldUnresolved(1, zero), "mode 1 with a parent of zeros has no world position");
        Check(!FxWorldUnresolved(1, parent), "mode 1 with a parent is placed");
        Check(!FxWorldUnresolved(0, zero), "world mode keeps its matrix");
    }

    void TestAreaExtend()
    {
        // The recording side: the six puffs do not stretch the area.
        replay::cliparea::Area area;
        area.Extend(-1601.0f, -556.0f, 80.0f);
        area.Extend(-1138.0f, -104.0f, 120.0f);
        for (int n = 0; n < 6; ++n)
            area.Extend(0.0f, -0.03f, 0.0f);
        Check(area.has && area.rejected == 6, "six origin puffs refused");
        Check(area.max[0] == -1138.0f && area.max[1] == -104.0f, "the area keeps the scene's bounds");
        Check(!replay::cliparea::AreaReachesOrigin(area.has, area.min, area.max), "the scene area does not reach the origin");
        // The saved area of the clip (max 0.0;-0.03;193.89) needs the repair.
        const float savedMin[3] = {-1601.0f, -556.0f, 0.0f};
        const float savedMax[3] = {0.0f, -0.03f, 193.89f};
        Check(replay::cliparea::AreaReachesOrigin(true, savedMin, savedMax), "the stretched area is repaired");
        Check(!replay::cliparea::AreaReachesOrigin(false, savedMin, savedMax), "no area, no repair");
        // The repair rebuilds from the clip's positions, refusing the bad ones.
        const replay::cliparea::Area rebuilt = replay::cliparea::RebuildArea([](auto&& extend) {
            extend(-1400.0f, -300.0f, 90.0f);
            extend(0.0f, -0.03f, 0.0f);
            extend(-1200.0f, -200.0f, 100.0f);
        });
        Check(rebuilt.has && rebuilt.rejected == 1, "the repair refuses the origin");
        Check(rebuilt.min[0] == -1400.0f && rebuilt.max[0] == -1200.0f, "the repaired area is the scene");
        const replay::cliparea::Area empty = replay::cliparea::RebuildArea([](auto&& extend) { extend(0.0f, 0.0f, 0.0f); });
        Check(!empty.has, "nothing valid: no area (no clear in the wrong place)");
    }

    void TestPuppetOneShotRule()
    {
        using namespace replay::puppetaudio;
        const std::uint64_t holders[] = {0x20000000ull, 0x30000000ull};
        const std::uint64_t span = 0x4820;
        Check(InSpans(0x20000000ull, holders, 2, span), "the holder itself");
        Check(InSpans(0x20000000ull + 0x481F, holders, 2, span), "the last byte of the span");
        Check(!InSpans(0x20000000ull + 0x4820, holders, 2, span), "past the span");
        Check(!InSpans(0x1FFFFFF8ull, holders, 2, span), "below the holder");
        Check(!InSpans(0, holders, 2, span), "no audio entity");
        Check(!InSpans(0x30000010ull, holders, 1, span), "only the published count");
        // The rule: refused only when active, on a puppet, not the replay's.
        Check(RefuseOwnOneShot(true, false, false, true), "a puppet's own one-shot is refused");
        Check(!RefuseOwnOneShot(true, true, false, true), "the replay's own sound passes (ReplayEmitScope)");
        Check(!RefuseOwnOneShot(true, false, true, true), "a sound inside a replayed footstep or melee passes");
        Check(!RefuseOwnOneShot(true, false, false, false), "a world ped's sound passes");
        Check(!RefuseOwnOneShot(false, false, false, true), "no clip playing: nothing refused");
    }

    // The weapon one-shots and
    // the puppets' own persistent ped audio sounds.
    void TestPuppetExtraRules()
    {
        using namespace replay::puppetaudio;
        // Weapon one-shots: the recorder's span from the weapon object (audio at +0x100).
        const std::uint64_t weapons[] = {0x40000000ull};
        Check(InSpans(0x40000100ull, weapons, 1, kWeaponOneShotSpan), "weapon audio at CWeapon + 0x100");
        Check(InSpans(0x400001F8ull, weapons, 1, kWeaponOneShotSpan), "weapon audio inside the recorder's span");
        Check(!InSpans(0x40000200ull, weapons, 1, kWeaponOneShotSpan), "past the weapon span");
        // Call sites the ESND track carries (0x2fa10cf, 0x2fa0dcc).
        const std::uint32_t callers[] = {0x2FA10CFu, 0x2FA0DCCu};
        Check(CallerCarried(0x2FA10CFu, callers, 2), "a carried call site");
        Check(!CallerCarried(0x2588C2Fu, callers, 2), "a call site the recording never saw");
        Check(!CallerCarried(0x2FA0DCCu, callers, 1), "only the published count");
        Check(!CallerCarried(0, callers, 2), "no call site");
        // The persistent rule.
        Check(RefuseOwnPersistent(true, false, false, true, false, true), "a puppet's own foley from a carried call site");
        Check(!RefuseOwnPersistent(true, false, false, true, true, true), "a sound with a tracker is not ESND ped audio");
        Check(!RefuseOwnPersistent(true, false, false, true, false, false), "a call site the clip does not carry");
        Check(!RefuseOwnPersistent(true, true, false, true, false, true), "the replay's own passes");
        Check(!RefuseOwnPersistent(true, false, true, true, false, true), "inside a replayed footstep or melee passes");
        Check(!RefuseOwnPersistent(true, false, false, false, false, true), "a world ped passes");
        Check(!RefuseOwnPersistent(false, false, false, true, false, true), "switched off: nothing refused");
        // The hold.
        Check(PedAudioHoldMs(true, true, false, 120) == 120, "ped audio waits for the puppet");
        Check(PedAudioHoldMs(true, true, true, 120) == 0, "ped audio starts at its time while the own are refused");
        Check(PedAudioHoldMs(false, true, true, 120) == 120, "a train car's own audio keeps its hold");
        Check(PedAudioHoldMs(false, false, true, 120) == 0, "an object sound has no hold");
    }

    replay::census::GeneralSound TrainSound(std::uint32_t startMs, std::uint64_t ref = 0x11735be)
    {
        replay::census::GeneralSound sound;
        sound.entity = (231u << 16) | 0u;
        sound.cls = replay::census::OwnerClass::Train;
        sound.ref = ref;
        sound.startMs = startMs;
        sound.endMs = startMs + 500;
        sound.oneShot = true;
        sound.ended = true;
        sound.base = replay::census::AudioBase::VehicleAudio;
        sound.offset = 0x40;
        sound.vtableRva = 0x3000000;
        return sound;
    }

    void TestCoalesceRepeatedOneShots()
    {
        using namespace replay::census;
        // One request every 16 ms from 241 to 741 ms, then another source
        // in between.
        std::vector<GeneralSound> sounds;
        for (std::uint32_t t = 241; t <= 741; t += 16)
            sounds.push_back(TrainSound(t));
        const std::size_t perFrame = sounds.size();
        sounds.push_back(TrainSound(400, 0x999)); // another ref, alone
        GeneralSound loop = TrainSound(300, 0x777);
        loop.oneShot = false;
        loop.ended = false;
        sounds.push_back(loop); // a loop is never merged
        const CoalesceStats stats = CoalesceRepeatedOneShots(sounds);
        Check(stats.runs == 1 && stats.merged == perFrame, "the per-frame requests are one run");
        Check(sounds.size() == 3, "one merged sound, the lone one-shot and the loop");
        const GeneralSound* merged = nullptr;
        for (const GeneralSound& sound : sounds)
            if (sound.ref == 0x11735be)
                merged = &sound;
        Check(merged && !merged->oneShot && merged->ended, "the run plays as one kept sound");
        Check(merged && merged->startMs == 241 && merged->endMs == 241 + 16 * ((741 - 241) / 16) + kRepeatGapMs,
            "from the first request to the last plus the gap");
        Check(sounds.front().startMs <= sounds.back().startMs, "the result is in start order");
        // Two requests are not a run; a gap over 100 ms breaks it.
        std::vector<GeneralSound> few = {TrainSound(0), TrainSound(16)};
        Check(CoalesceRepeatedOneShots(few).runs == 0 && few.size() == 2 && few[0].oneShot, "two one-shots stay");
        std::vector<GeneralSound> spaced = {TrainSound(0), TrainSound(200), TrainSound(400), TrainSound(600)};
        Check(CoalesceRepeatedOneShots(spaced).runs == 0 && spaced.size() == 4, "one-shots 200 ms apart stay");
        // Another owner or audio entity is another source.
        std::vector<GeneralSound> owners;
        for (std::uint32_t t = 0; t < 100; t += 16)
        {
            owners.push_back(TrainSound(t));
            GeneralSound other = TrainSound(t);
            other.offset = 0x80;
            owners.push_back(other);
        }
        const CoalesceStats twoRuns = CoalesceRepeatedOneShots(owners);
        Check(twoRuns.runs == 2 && owners.size() == 2, "each audio entity has its own run");
    }

    void TestRegisteredFxRules()
    {
        using namespace replay::regfx;
        Check(PlayMode(true, false) == Mode::Normal, "normal play");
        Check(PlayMode(false, true) == Mode::Clear, "a scrub clears");
        Check(PlayMode(false, false) == Mode::Hold, "a pause holds");
        Check(MayCreate(Mode::Normal, false, false), "a world track is created in normal play");
        Check(MayCreate(Mode::Normal, true, true), "an attached track with its entity");
        Check(!MayCreate(Mode::Normal, true, false), "an attached track without its entity is not shown alone");
        Check(!MayCreate(Mode::Hold, false, true), "nothing is created in a pause");
        Check(!MayCreate(Mode::Clear, false, true), "nothing is created in a scrub");

        Identity created;
        created.rule = 0x7FF600001000ull;
        created.hash = 0x99b2905;
        created.state = kStatePlaying;
        created.position[0] = -1200.0f;
        Identity now = created;
        Check(StopAllowed(true, created, now), "our playing instance is stopped");
        now.state = kStateStopping;
        Check(!StopAllowed(true, created, now), "an instance already stopping is left alone");
        now = created;
        now.rule = 0x7FF600002000ull;
        Check(!StopAllowed(true, created, now), "another effect at the address is not ours");
        Check(!StopAllowed(false, created, created), "unreadable: no call");

        Identity atEnd = created;
        atEnd.state = kStateStopping;
        Identity later = atEnd;
        later.state = kStatePlaying;
        Check(ClassifyAfterEnd(true, atEnd, later, false) == AfterEnd::AliveOurs, "still playing where we left it");
        later.state = kStateFinished;
        Check(ClassifyAfterEnd(true, atEnd, later, false) == AfterEnd::Finishing, "finishing");
        later.state = kStatePlaying;
        Check(ClassifyAfterEnd(true, atEnd, later, true) == AfterEnd::Reused, "taken by one of our live tracks");
        later.position[0] += 10.0f;
        Check(ClassifyAfterEnd(true, atEnd, later, false) == AfterEnd::Reused, "moved 10 m: another tenant");
        later = atEnd;
        later.hash = 0x45faf2c0;
        Check(ClassifyAfterEnd(true, atEnd, later, false) == AfterEnd::Reused, "another effect hash");
        later = atEnd;
        later.state = 0;
        Check(ClassifyAfterEnd(true, atEnd, later, false) == AfterEnd::Reused, "a pooled instance set up again");
        Check(ClassifyAfterEnd(false, atEnd, later, false) == AfterEnd::Unreadable, "unreadable");
        Check(EndOnLostOwner(true) && !EndOnLostOwner(false), "an effect ends with a lost actor, stays on a map object");
    }

    // The owner of a registered track, with the numbers of a recorded clip.
    void TestRegisteredFxOwnerRules()
    {
        using namespace replay::regfx;
        // 0xF00DD858 on a map entity, last call at 24742 ms; the hoof puff
        // that took the address attached to the horse's bone 365 at 24842 ms.
        const std::uint64_t lastCallUs = 24742000ull;
        Check(!AttachBelongsToKey(false, 24842000ull, lastCallUs), "an attach 100 ms after the last call is a new tenant's");
        Check(!AttachBelongsToKey(false, 17957000ull, 17807000ull), "the 0xAAD875AB case: 150 ms after the last call");
        Check(AttachBelongsToKey(false, lastCallUs + 14, lastCallUs), "the site's own attach right after the get (dt_us 14)");
        Check(AttachBelongsToKey(false, lastCallUs + kKeyAttachWindowUs, lastCallUs), "the window's edge");
        Check(!AttachBelongsToKey(false, lastCallUs + kKeyAttachWindowUs + 1, lastCallUs), "past the window");
        Check(AttachBelongsToKey(true, lastCallUs + 80000, lastCallUs), "a key called in this tick still holds its instance");
        Check(AttachBelongsToKey(false, lastCallUs - 5000, lastCallUs), "an attach before the last call");

        // The recorded parent of the map effect against the horse puppet.
        float parent[16] = {};
        parent[12] = -1600.0f;
        parent[13] = -2800.0f;
        parent[14] = -3.3f;
        const float horse[3] = {-1950.0f, -2500.0f, 56.0f};
        Check(!ParentAtOwner(parent, horse), "a parent 460 m from the puppet: recorded on another entity");
        // A hoof dust on the horse: its parent is the bone, a metre away.
        parent[12] = -1949.2f;
        parent[13] = -2500.6f;
        parent[14] = 55.1f;
        Check(ParentAtOwner(parent, horse), "a parent at the puppet's bone");
        parent[12] = horse[0] + kOwnerParentMaxM;
        parent[13] = horse[1];
        parent[14] = horse[2];
        Check(ParentAtOwner(parent, horse), "the limit is inclusive");
        parent[12] = horse[0] + kOwnerParentMaxM + 0.5f;
        Check(!ParentAtOwner(parent, horse), "past the limit");
        float zero[16] = {};
        Check(ParentAtOwner(zero, horse), "a parent of zeros is unknown");
        float bad[16] = {};
        bad[12] = std::numeric_limits<float>::quiet_NaN();
        bad[13] = 1.0f;
        Check(ParentAtOwner(bad, horse), "a parent not finite is unknown");
        bad[12] = std::numeric_limits<float>::infinity();
        Check(ParentAtOwner(bad, horse), "an infinite parent is unknown");
    }
}

int RunSoundParticlesTests()
{
    TestPositionValid();
    TestFxWorldUnresolved();
    TestAreaExtend();
    TestPuppetOneShotRule();
    TestPuppetExtraRules();
    TestCoalesceRepeatedOneShots();
    TestRegisteredFxRules();
    TestRegisteredFxOwnerRules();
    if (g_soundParticleFailures == 0)
        std::printf("Sound and particle tests passed.\n");
    return g_soundParticleFailures;
}
