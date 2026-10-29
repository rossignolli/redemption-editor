// The player's horse audio sites and the choice of the mounts that ride with
// the player, the blood code's trusted regions (a region in use stays
// trusted), and the effect sites the replay keeps to itself. The puppets'
// own persistent sounds by entity, call site and entry, the replayed ped
// audio loops that follow their puppet, and the horse loop evidence. Short
// ped audio sounds keep their recorded length (the horse's braking slide)
// and the animal vocalization retry's entry.
#include "fx_observer.h"
#include "horse_audio_evidence.h"
#include "mount_audio.h"
#include "puppet_audio_rule.h"
#include "speech_identity.h"
#include "trusted_regions.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
    int g_gallopFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_gallopFailures;
    }

    void TestMountAudioSites()
    {
        using namespace replay::mountaudio;
        Check(kSiteCount == 13, "thirteen ped audio callers");
        Check(kCounterSlots == kSiteCount + 1, "one counter more for the other callers");
        // Every site is a return address in the ped audio (0x317A000 to
        // 0x31A3000), listed once.
        for (std::size_t index = 0; index < kSiteCount; ++index)
        {
            Check(kSites[index] >= 0x317A000 && kSites[index] < 0x31A3000, "a site lies in the ped audio");
            Check(SiteSlot(kSites[index]) == index, "a site has its own slot");
        }
        // The movement sounds (0x31941F9 + 5) and the footstep variant
        // (0x317C0EB + 5) are among them.
        Check(IsAudioSite(0x31941FE), "the movement sound flag site");
        Check(IsAudioSite(0x317C0F0), "the footstep variant site");
        // The three gameplay callers (call + 5) keep the engine's answer.
        Check(!IsAudioSite(0x899B12) && SiteSlot(0x899B12) == kSiteCount, "gameplay caller 0x899B0D is other");
        Check(!IsAudioSite(0xD9DC81), "gameplay caller 0xD9DC7C is other");
        Check(!IsAudioSite(0x2191005), "gameplay caller 0x2191000 is other");
        // The call address itself is not a return site.
        Check(!IsAudioSite(0x31941F9), "a call address is not its return address");
    }

    void TestPlayerMounts()
    {
        using replay::mountaudio::PlayerMounts;
        // The player 2:72 rides 1:71; 20:159 rides 15:160.
        const auto pack = [](std::uint32_t slot, std::uint32_t instance) { return (instance << 16) | slot; };
        std::map<std::uint32_t, std::uint32_t> mountOf = {{pack(2, 72), pack(1, 71)}, {pack(20, 159), pack(15, 160)}};
        const auto lookup = [&](std::uint32_t rider) {
            const auto found = mountOf.find(rider);
            return found == mountOf.end() ? 0u : found->second;
        };
        std::vector<std::uint32_t> mounts = PlayerMounts(std::vector<std::uint32_t>{pack(2, 72)}, lookup);
        Check(mounts.size() == 1 && mounts[0] == pack(1, 71), "the player's horse rides with the player");
        mounts = PlayerMounts(std::vector<std::uint32_t>{pack(20, 159)}, lookup);
        Check(mounts.size() == 1 && mounts[0] == pack(15, 160), "only the riders given count");
        mountOf.erase(pack(2, 72)); // he got off at 49663 ms
        mounts = PlayerMounts(std::vector<std::uint32_t>{pack(2, 72)}, lookup);
        Check(mounts.empty(), "a player on foot has no horse");
        // Two player puppets on one horse (a model fallback with Arthur twice):
        // the horse once.
        mountOf[pack(2, 72)] = pack(1, 71);
        mountOf[pack(3, 80)] = pack(1, 71);
        mounts = PlayerMounts(std::vector<std::uint32_t>{pack(2, 72), pack(3, 80)}, lookup);
        Check(mounts.size() == 1, "a horse is published once");
        Check(PlayerMounts(std::vector<std::uint32_t>{}, lookup).empty(), "no player puppet, no horse");
    }

    void TestTrustedRegions()
    {
        replay::mem::TrustedRegions<2> regions;
        const std::uint64_t ttl = 5000;
        Check(!regions.Contains(0x1000, 0x1010, 0, ttl), "nothing is trusted at first");
        regions.Add(0x1000, 0x9000, 0, ttl);
        Check(regions.Contains(0x1000, 0x1010, 100, ttl), "a range inside the region is trusted");
        Check(!regions.Contains(0x8FF8, 0x9008, 100, ttl), "a range crossing the end is not");
        // A region kept in use stays trusted (forgetting it after a fixed
        // time costs a 7 to 9 ms query every few seconds).
        std::uint64_t now = 100;
        bool always = true;
        for (int step = 0; step < 60; ++step, now += 1000)
            always = always && regions.Contains(0x2000, 0x2020, now, ttl);
        Check(always, "a region used every second stays trusted for a minute");
        Check(regions.Hits() == 61, "every use is a hit");
        // Not used for the time to live: asked again.
        Check(!regions.Contains(0x2000, 0x2020, now + ttl, ttl), "an idle region expires");
        // Two slots: a third region replaces the oldest.
        regions.Add(0x10000, 0x20000, now, ttl);
        regions.Add(0x30000, 0x40000, now, ttl);
        Check(regions.Contains(0x10000, 0x10008, now, ttl) && regions.Contains(0x30000, 0x30008, now, ttl),
            "the two newest regions are trusted");
        regions.Clear();
        Check(!regions.Contains(0x10000, 0x10008, now, ttl), "Clear forgets every region (playback start and end)");
    }

    void TestReplayEffectSites()
    {
        // The sites must not be any other site the replay plays or filters.
        Check(replay::fx::kFootVfxSite == 0x145320F, "the foot vfx one-shot site");
        Check(replay::fx::kCameraAmbientFxSite == 0x1469DC1, "the camera ambient registered site");
        Check(replay::fx::kFootVfxSite != replay::fx::kMuzzleFlashSite &&
                replay::fx::kFootVfxSite != replay::fx::kBulletImpactSite,
            "the weapon sites are not the foot site");
    }

    // The sound entry of a persistent ref (puppet_audio_rule.h).
    void TestSoundEntryKey()
    {
        using namespace replay::puppetaudio;
        // The tack loop, the horse's foley and skid.
        const std::uint64_t tack = 0x800003000e18baull;
        const std::uint64_t foley = 0x800004004366e8ull;
        const std::uint64_t skid = 0x80000e002f7034ull;
        Check(IsSetEntryRef(tack, false) && IsSetEntryRef(skid, false), "0x8000NN00 refs are set entries");
        Check(SoundEntryKey(tack, false) == ((2ull << 62) | 3u), "the tack loop is entry 3");
        Check(SoundEntryKey(skid, false) == ((2ull << 62) | 0x0Eu), "the skid loop is entry 0x0e");
        Check(SoundEntryKey(0x80002a002f7034ull, false) == ((2ull << 62) | 0x2Au), "the index has 20 bits");
        // The same entry NN of another set is the
        // same sound (the puppet's outfit picked the other set).
        Check(SoundEntryKey(0x80000400438196ull, false) == SoundEntryKey(0x80000400436f49ull, false),
            "same entry of two sets, one key");
        Check(SoundEntryKey(foley, false) != SoundEntryKey(tack, false), "different entries, different keys");
        // A direct metadata ref (high dword 0xFFFFF) and a hash key whole.
        const std::uint64_t direct = 0xfffff0118b7a2ull;
        Check(!IsSetEntryRef(direct, false), "a direct ref is not a set entry");
        Check(SoundEntryKey(direct, false) == direct, "a direct ref keys by its value");
        Check(SoundEntryKey(0xfffff0118b7caull, false) != SoundEntryKey(direct, false), "two direct refs differ");
        Check(!IsSetEntryRef(0x2739ab2dull, true), "a hash is not a set entry");
        Check(SoundEntryKey(0x2739ab2dull, true) == ((1ull << 62) | 0x2739ab2du), "a hash keys tagged");
        Check(SoundEntryKey(0x2739ab2dull, true) != SoundEntryKey(0x2739ab2dull, false), "hash and ref never meet");
        Check(!IsSetEntryRef(0x7f800003000e18baull, false), "other high bits: not a set entry");
        // The key changes with each of the three parts.
        const std::uint64_t key = OwnPersistentKey(0x00010D9Bu, 0x2588C2F, SoundEntryKey(skid, false));
        Check(key != OwnPersistentKey(0x00020D9Bu, 0x2588C2F, SoundEntryKey(skid, false)), "the entity is in the key");
        Check(key != OwnPersistentKey(0x00010D9Bu, 0x2FA10CF, SoundEntryKey(skid, false)), "the call site is in the key");
        Check(key != OwnPersistentKey(0x00010D9Bu, 0x2588C2F, SoundEntryKey(tack, false)), "the entry is in the key");
        Check(key == OwnPersistentKey(0x00010D9Bu, 0x2588C2F, SoundEntryKey(0x80000e002f5642ull, false)),
            "the skid loop of another surface set is the same sound");
    }

    // A recorded clip replayed with the rule by entry.
    void TestOwnPersistentByEntry()
    {
        using namespace replay::puppetaudio;
        const auto pack = [](std::uint32_t slot, std::uint32_t instance) { return (slot << 16) | instance; };
        const std::uint32_t horse = pack(1, 3489);
        const std::uint32_t dog = pack(12, 3500);
        // The ESND pedAudio sounds of the clip (entity, caller, ref).
        struct Recorded { std::uint32_t entity; std::uint32_t caller; std::uint64_t ref; };
        const Recorded clip[] = {
            {horse, 0x2FA10CF, 0x800004004366e8ull}, {horse, 0x2588C2F, 0x80000e002f7034ull},
            {horse, 0x2588C2F, 0x80000e002f5642ull}, {dog, 0x2FA10CF, 0x80000400436739ull}};
        std::vector<std::uint64_t> keys;
        std::vector<std::uint32_t> callers;
        for (const Recorded& sound : clip)
        {
            keys.push_back(OwnPersistentKey(sound.entity, sound.caller, SoundEntryKey(sound.ref, false)));
            callers.push_back(sound.caller);
        }
        // Published sorted and without repeats (SetOwnPersistentKeys): the
        // two skid refs are one key, so four sounds give three keys.
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        Check(keys.size() == 3, "four recorded sounds, three keys");
        const auto carried = [&](bool byEntry, std::uint32_t owner, std::uint32_t caller, std::uint64_t ref, bool byHash) {
            return OwnPersistentCarried(byEntry, owner, caller, ref, byHash, keys.data(), keys.size(), callers.data(),
                callers.size());
        };
        // The puppet horse's tack loop: refused by the call site alone, left
        // to the puppet by entry.
        Check(carried(false, horse, 0x2588C2F, 0x800003000e18baull, false), "the wrapper's call site alone refused it");
        Check(!carried(true, horse, 0x2588C2F, 0x800003000e18baull, false), "by entry: the clip never carried the tack loop");
        Check(!RefuseOwnPersistent(true, false, false, true, false, carried(true, horse, 0x2588C2F, 0x800003000e18baull, false)),
            "the puppet's tack loop plays");
        // The duplicates the call site rule refuses stay refused.
        Check(carried(true, horse, 0x2FA10CF, 0x80000400436f49ull, false), "the foley entry of another set is carried");
        Check(carried(true, horse, 0x2588C2F, 0x80000e002f87cbull, false), "the skid loop of another surface is carried");
        Check(RefuseOwnPersistent(true, false, false, true, false, carried(true, horse, 0x2FA10CF, 0x80000400436f49ull, false)),
            "the puppet's own foley is refused");
        // Another puppet's sound is its own: not carried by the horse's.
        Check(!carried(true, pack(5, 10), 0x2FA10CF, 0x800004004366e8ull, false), "another entity's sound is not carried");
        Check(carried(true, dog, 0x2FA10CF, 0x80000400436aaaull, false), "the dog's foley entry is carried for the dog");
        // The hidden player and his mount keep the call site rule.
        Check(carried(true, kNoOwner, 0x2588C2F, 0x800003000e18baull, false), "hidden: the call site rule");
        Check(!carried(true, kNoOwner, 0x318E828, 0x800003000e18baull, false), "hidden: a call site the clip lacks");
        // No call site (a call from outside the image): never carried.
        Check(!carried(true, horse, 0, 0x800004004366e8ull, false), "no call site, not carried");
        Check(!carried(false, horse, 0, 0x800004004366e8ull, false), "no call site, not carried (call site rule)");
        // A hash and a ref with the same low dword are different sounds.
        std::vector<std::uint64_t> hashKeys = {OwnPersistentKey(horse, 0x25887C9, SoundEntryKey(0x0d0e4full, true))};
        Check(SortedKeyCarried(OwnPersistentKey(horse, 0x25887C9, SoundEntryKey(0x0d0e4full, true)), hashKeys.data(), 1),
            "the same hash is carried");
        Check(!SortedKeyCarried(OwnPersistentKey(horse, 0x25887C9, SoundEntryKey(0x0d0e4full, false)), hashKeys.data(), 1),
            "a ref is not the hash");
        Check(!SortedKeyCarried(1, nullptr, 0), "an empty list carries nothing");
        // A big fight: 600 peds' entries, every one found, none extra.
        std::vector<std::uint64_t> many;
        for (std::uint32_t ped = 0; ped < 120; ++ped)
            for (std::uint32_t entry = 2; entry <= 6; ++entry)
                many.push_back(OwnPersistentKey(pack(ped, 7000 + ped), 0x2FA10CF,
                    SoundEntryKey((0x800000ull | entry) << 32 | 0x436f49u, false)));
        std::sort(many.begin(), many.end());
        bool all = true;
        for (std::uint32_t ped = 0; ped < 120; ++ped)
            for (std::uint32_t entry = 2; entry <= 6; ++entry)
                all = all && SortedKeyCarried(OwnPersistentKey(pack(ped, 7000 + ped), 0x2FA10CF,
                    SoundEntryKey((0x800000ull | entry) << 32 | 0x438196u, false)), many.data(), many.size());
        Check(all, "600 keys: each ped's entries of another set are found");
        Check(!SortedKeyCarried(OwnPersistentKey(pack(0, 7000), 0x2FA10CF, SoundEntryKey(0x800007000e18baull, false)),
                  many.data(), many.size()),
            "an entry no ped recorded is not found");
    }

    // Positioned ped audio loops follow their puppet.
    void TestPedAudioFollowsPuppet()
    {
        using replay::puppetaudio::PedAudioFollowsPuppet;
        Check(PedAudioFollowsPuppet(false, true, false, true), "a sound that is not ped audio follows (as before)");
        Check(PedAudioFollowsPuppet(true, false, false, true), "an unpositioned ped audio sound follows (as before)");
        Check(!PedAudioFollowsPuppet(true, true, false, true), "the shell keeps its recorded point");
        Check(PedAudioFollowsPuppet(true, true, true, true), "a positioned loop follows the puppet");
        Check(!PedAudioFollowsPuppet(true, true, true, false), "PedAudioLoopFollowsPuppet=0: the old behaviour");
        Check(PedAudioFollowsPuppet(true, false, true, false), "an unpositioned loop follows either way");
    }

    // The horse loop fields and the step params text
    // (horse_audio_evidence.h).
    void TestHorseLoopEvidence()
    {
        using namespace replay::horseaudio;
        Check(std::strcmp(FieldName(kExt0030), "x0030") == 0 && std::strcmp(FieldName(kExt0048), "x0048") == 0,
            "saddle weapon loop slots");
        Check(std::strcmp(FieldName(kFoot0596), "f0596") == 0, "tack loop switch");
        Check(std::strcmp(FieldName(kFoot04D8), "f04d8") == 0 && std::strcmp(FieldName(kFoot04E0), "f04e0") == 0,
            "tack loop slots");
        Check(std::strcmp(FieldName(kFoot0480), "f0480") == 0 && std::strcmp(FieldName(kFoot05A5), "f05a5") == 0,
            "urge loop slot and request");
        Check(kFoot05A5 + 1 == kFieldCount, "the urge request is the last field");
        std::set<std::string> names;
        for (std::size_t field = 0; field < kFieldCount; ++field)
            names.insert(FieldName(field));
        Check(names.size() == kFieldCount, "every field name once (f0480 is not the slot hash x0480)");
        // The live horse with its tack loop, the puppet without: one diff.
        State record;
        State play;
        record.ok = play.ok = true;
        for (std::size_t field = 0; field < kFieldCount; ++field)
            record.read[field] = play.read[field] = 1;
        record.values[kFoot04D8] = 1;
        std::size_t count = 0;
        Check(DiffStates(record, play, count) == "f04d8:0x1>0x0" && count == 1, "the missing tack loop shows");
        // Step params: the volume, the count and the variables.
        std::uint8_t params[0x140] = {};
        const float volume = -1.97f;
        std::memcpy(params + kParamsVolumeOffset, &volume, sizeof(volume));
        const std::uint32_t two = 2;
        std::memcpy(params + kParamsVariableCount, &two, sizeof(two));
        const std::uint32_t hashes[2] = {0xb3d66f04u, 0xa53f2e56u};
        const float values[2] = {0.5f, 1.0f};
        for (int n = 0; n < 2; ++n)
        {
            std::memcpy(params + kParamsVariables + 8 * n, &hashes[n], 4);
            std::memcpy(params + kParamsVariables + 8 * n + 4, &values[n], 4);
        }
        Check(FormatStepParams(params, sizeof(params)) == "vol=-1.97,nvars=2,vars=0xb3d66f04:0.50|0xa53f2e56:1.00",
            "step params text");
        Check(FormatStepParams(nullptr, 0) == "vol=na,nvars=na,vars=none", "no params");
        Check(FormatStepParams(params, kParamsBytesNeeded - 1) == "vol=na,nvars=na,vars=none", "short params");
        const std::uint32_t none = 0;
        std::memcpy(params + kParamsVariableCount, &none, sizeof(none));
        Check(FormatStepParams(params, sizeof(params)) == "vol=-1.97,nvars=0,vars=none", "no variables");
        const std::uint32_t many = 40;
        std::memcpy(params + kParamsVariableCount, &many, sizeof(many));
        const std::string text = FormatStepParams(params, sizeof(params));
        std::size_t items = 1;
        for (const char c : text)
            items += c == '|' ? 1 : 0;
        Check(text.find("nvars=40,") != std::string::npos && items == kParamsMaxVariables, "at most 16 listed");
        // The step's filters.
        const std::uint32_t lowPass = 5000, highPass = 150;
        std::memcpy(params + kParamsLowPassOffset, &lowPass, 4);
        std::memcpy(params + kParamsHighPassOffset, &highPass, 4);
        Check(FormatStepFilters(params, sizeof(params)) == "lpf=5000,hpf=150", "step filters text");
        Check(FormatStepFilters(nullptr, 0) == "lpf=na,hpf=na", "no params, no filters");
        Check(FormatStepFilters(params, kParamsHighPassOffset + 3) == "lpf=na,hpf=na", "short params, no filters");
    }
}

namespace
{
    void TestStopAtRecordedLength()
    {
        using replay::puppetaudio::StopAtRecordedLength;
        constexpr std::uint32_t kLoop = 1000;
        // Horse 0:833: 0x80000e002f4361 created at
        // 8029 ms and stopped by the game at 8059 ms (record_end how=stop).
        Check(!StopAtRecordedLength(true, true, false, true, 8029, 8059, kLoop, 8038, 8055),
            "the slide grain plays while younger than its recorded 30 ms");
        Check(StopAtRecordedLength(true, true, false, true, 8029, 8059, kLoop, 8038, 8072),
            "the slide grain stops once it played its recorded 30 ms");
        Check(StopAtRecordedLength(true, true, false, true, 8029, 8059, kLoop, 8038, 8068),
            "the stop counts from the playhead it started at (exactly the length)");
        Check(!StopAtRecordedLength(true, true, false, true, 8029, 8059, kLoop, 8100, 8120),
            "a late start keeps the whole length");
        Check(!StopAtRecordedLength(false, true, false, true, 8029, 8059, kLoop, 8038, 9000),
            "PedAudioKeepRecordedLength=0: left to the engine");
        Check(!StopAtRecordedLength(true, true, true, true, 8029, 8059, kLoop, 8038, 9000),
            "a one-shot is never cut");
        Check(!StopAtRecordedLength(true, false, false, true, 8029, 8059, kLoop, 8038, 9000),
            "an object loop keeps its own rule (not own audio)");
        Check(!StopAtRecordedLength(true, true, false, false, 8029, 0, kLoop, 8038, 9000),
            "a sound that never ended in the recording is not cut");
        Check(!StopAtRecordedLength(true, true, false, true, 6199, 10446, kLoop, 6209, 9000),
            "a loop (1 s or more) keeps the end-of-span rule");
        Check(!StopAtRecordedLength(true, true, false, true, 100, 50, kLoop, 100, 9000), "a bad span is not cut");
        Check(!StopAtRecordedLength(true, true, false, true, 8029, 8059, kLoop, 8038, 8000),
            "a playhead before the start (a seek) is left to the seek path");
        Check(StopAtRecordedLength(true, true, false, true, 500, 500, kLoop, 510, 510),
            "a zero-length sound stops on the next check");
        // The pelt sounds (how=gone, about 600 ms):
        // the stop comes when the recorded (polled) length is over.
        Check(!StopAtRecordedLength(true, true, false, true, 14895, 15613, kLoop, 14901, 15600),
            "a sound that ended by itself plays its measured length");
        Check(StopAtRecordedLength(true, true, false, true, 14895, 15613, kLoop, 14901, 15620),
            "and is stopped after it");
    }

    void TestAnimalVocalEntry()
    {
        using namespace replay::speech;
        Check(kAnimalSpeechVtableRva == 0x37FE430 && kAnimalVocalHashSlot == 0x288 && kAnimalVocalHashRva == 0x2FB2B4C,
            "animal vocal entry constants (build 1491.50)");
        Check(kAnimalSpeechVtableRva + kAnimalVocalHashSlot == 0x37FE6B8,
            "the hash slot is the only .rdata qword holding 0x2FB2B4C");
        Check(CheckAnimalVocalEntry(0x37FE430, 0x2FB2B4C) == AnimalVocalEntry::Ok, "the animal speech entity is accepted");
        Check(CheckAnimalVocalEntry(0x37FE428, 0x2FB2B4C) == AnimalVocalEntry::NotAnimalSpeech,
            "the old vtable (a 0x38-byte object) is not an animal speech entity");
        Check(CheckAnimalVocalEntry(0x37FD5F8, 0x11CC10) == AnimalVocalEntry::NotAnimalSpeech,
            "the base (human) speech entity is refused");
        Check(CheckAnimalVocalEntry(0x37FE430, 0x2FB2B70) == AnimalVocalEntry::SlotOther,
            "the name entry (slot +0x290) is not the hash entry");
        Check(CheckAnimalVocalEntry(~0ull, ~0ull) == AnimalVocalEntry::NotAnimalSpeech, "an unreadable vtable is refused");
    }
}

int RunGallopTests()
{
    TestMountAudioSites();
    TestPlayerMounts();
    TestTrustedRegions();
    TestReplayEffectSites();
    TestSoundEntryKey();
    TestOwnPersistentByEntry();
    TestPedAudioFollowsPuppet();
    TestHorseLoopEvidence();
    TestStopAtRecordedLength();
    TestAnimalVocalEntry();
    if (g_gallopFailures == 0)
        std::printf("Gallop tests passed.\n");
    return g_gallopFailures;
}
