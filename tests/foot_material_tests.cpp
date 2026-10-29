// The standing material of the feet (foot_material.h): slot layout, the
// write decision and the pairing of recorded foot-downs with the movement
// update samples.
#include "foot_material.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
    int g_footFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_footFailures;
    }

    using namespace replay::footmaterial;

    void TestLayout()
    {
        // 0x318D1D4: (foot + 9) << 6; constructor 0x3178EB4 from +0x240.
        Check(SlotOffset(0) == 0x240 && SlotOffset(1) == 0x280 && SlotOffset(3) == 0x300, "slot of each foot");
        for (std::uint8_t foot = 0; foot < kFeet; ++foot)
            Check(SlotOffset(foot) == ((static_cast<std::uintptr_t>(foot) + 9) << 6), "slot matches the engine's index");
        Check(kSlotHash == 8 && kSlotRef == 0xC && kQueueCount == 0xB0 && kQueue == 0x70, "fields");
        Check(!ValidRef(0) && !ValidRef(0xFFFFFFFFu) && ValidRef(0x2f36cbu), "valid refs");
    }

    void TestNeedsWrite()
    {
        Material recorded{0x1234u, 0x2f36cbu};
        Material other{0x5678u, 0x2f7034u};
        Check(NeedsWrite(0xA000, other, 0xB000, recorded), "another container: write");
        Check(!NeedsWrite(0xB000, recorded, 0xB000, recorded), "same slot: no write");
        Check(NeedsWrite(0xB000, other, 0xB000, recorded), "same container pointer, other ref: write");
        Check(!NeedsWrite(0xA000, other, 0, recorded), "no container found: no write");
        Check(!NeedsWrite(0xA000, other, 0xB000, Material{}), "no recorded material: no write");
    }

    Sample MakeSample(std::uint64_t timeUs, std::uint64_t footAudio, std::uint8_t foot, std::uint32_t ref)
    {
        Sample sample;
        sample.timeUs = timeUs;
        sample.footAudio = footAudio;
        sample.foot = foot;
        sample.material = Material{ref ^ 0xABCDu, ref};
        return sample;
    }

    Pairer::Pending MakePending(std::uint64_t timeUs, std::uint64_t footAudio, std::uint8_t foot, std::uint64_t token)
    {
        Pairer::Pending pending;
        pending.timeUs = timeUs;
        pending.footAudio = footAudio;
        pending.foot = foot;
        pending.token = token;
        pending.check = token * 7;
        return pending;
    }

    void TestPairing()
    {
        Pairer pairer;
        std::vector<Pairer::Match> matches;
        // Two horses stepping together; each sample follows its foot-down.
        pairer.AddPending(MakePending(1000, 0xA, 0, 1));
        pairer.AddPending(MakePending(1001, 0xB, 0, 2));
        pairer.AddPending(MakePending(1500, 0xA, 1, 3));
        pairer.AddSample(MakeSample(1010, 0xB, 0, 0x2f7034u));
        pairer.AddSample(MakeSample(1012, 0xA, 0, 0x2f36cbu));
        pairer.Pair(matches);
        Check(matches.size() == 2, "two foot-downs matched");
        Check(matches.size() == 2 && matches[0].token == 1 && matches[0].material.ref == 0x2f36cbu &&
                matches[0].check == 7,
            "horse A foot 0 gets its own sample");
        Check(matches.size() == 2 && matches[1].token == 2 && matches[1].material.ref == 0x2f7034u,
            "horse B foot 0 gets its own sample");
        Check(pairer.PendingCount() == 1, "foot 1 still waits");
        // Its sample comes on the next tick.
        pairer.AddSample(MakeSample(1520, 0xA, 1, 0x2f767fu));
        pairer.Pair(matches);
        Check(matches.size() == 1 && matches[0].token == 3 && matches[0].material.ref == 0x2f767fu,
            "a sample one tick later still pairs");
        Check(pairer.PendingCount() == 0 && pairer.SampleCount() == 0, "all used");
    }

    void TestSampleBeforeFootDown()
    {
        Pairer pairer;
        std::vector<Pairer::Match> matches;
        // A sample of an earlier, unrecorded step never matches a later foot-down.
        pairer.AddSample(MakeSample(900, 0xA, 0, 0x111u));
        pairer.AddPending(MakePending(1000, 0xA, 0, 1));
        pairer.Pair(matches);
        Check(matches.empty() && pairer.PendingCount() == 1, "an older sample is not this step's");
        pairer.AddSample(MakeSample(1030, 0xA, 0, 0x222u));
        pairer.AddSample(MakeSample(1060, 0xA, 0, 0x333u));
        pairer.Pair(matches);
        Check(matches.size() == 1 && matches[0].material.ref == 0x222u, "the first sample after the foot-down");
        // The left over samples expire.
        Check(pairer.Expire(1000000, 500000) == 0 && pairer.SampleCount() == 0, "old samples go with Expire");
    }

    void TestExpire()
    {
        Pairer pairer;
        std::vector<Pairer::Match> matches;
        pairer.AddPending(MakePending(1000, 0xA, 2, 1));
        pairer.AddPending(MakePending(400000, 0xA, 3, 2));
        Check(pairer.Expire(600000, 500000) == 1 && pairer.PendingCount() == 1, "the old foot-down expires");
        Check(pairer.Expired() == 1, "expired counted");
        pairer.AddSample(MakeSample(400100, 0xA, 3, 0x444u));
        pairer.Pair(matches);
        Check(matches.size() == 1 && matches[0].token == 2, "the recent one still pairs");
        // Order in time: the same foot stepping twice before the update ran.
        pairer.AddPending(MakePending(2000000, 0xC, 0, 10));
        pairer.AddPending(MakePending(2000500, 0xC, 0, 11));
        pairer.AddSample(MakeSample(2001000, 0xC, 0, 0x555u));
        pairer.AddSample(MakeSample(2001000, 0xC, 0, 0x555u));
        pairer.Pair(matches);
        Check(matches.size() == 2 && matches[0].token == 10 && matches[1].token == 11, "two queued steps, two samples");
    }

    void TestCapacity()
    {
        Pairer pairer;
        bool full = false;
        for (std::size_t n = 0; n < Pairer::kMaxPending + 3; ++n)
            full = !pairer.AddPending(MakePending(n, 0xD, 0, n)) || full;
        Check(full && pairer.PendingCount() == Pairer::kMaxPending, "pending list bounded");
        for (std::size_t n = 0; n < Pairer::kMaxSamples + 10; ++n)
            pairer.AddSample(MakeSample(n, 0xE, 0, 1));
        Check(pairer.SampleCount() == Pairer::kMaxSamples, "samples bounded");
    }

    // The per-foot mix slot.
    void TestFootMix()
    {
        // 0x317AAD0 writes +0x390/+0x3B0/+0x3C0 + 4 * foot; 0x318D52C reads them.
        Check(MixVolumeOffset(0) == 0x390 && MixVolumeOffset(3) == 0x39C, "mix volume per foot");
        Check(MixHighPassOffset(1) == 0x3B4 && MixLowPassOffset(2) == 0x3C8, "mix filters per foot");
        // The reset 0x319580C: 0 dB, 0 Hz, 24000 Hz.
        FootMix reset{true, 0.0f, 0, 24000};
        FootMix hit{true, -4.07f, 150, 5000};
        Check(NeedsMixWrite(reset, hit), "reset slot, recorded hit: write");
        Check(!NeedsMixWrite(hit, hit), "same mix: no write");
        Check(!NeedsMixWrite(reset, FootMix{}), "unknown mix: never write");
        FootMix filterOnly = hit;
        filterOnly.highPass = 300;
        Check(NeedsMixWrite(hit, filterOnly), "only the high-pass differs: write");
        Check(PlausibleMix(-6.27f, 200, 24000) && !PlausibleMix(1e30f, 0, 24000) && !PlausibleMix(0.0f, 0, 0xFFFFFFFFu),
            "plausible slot values only");
        Check(EncodeMixFilter(24000) == 24000 && EncodeMixFilter(0x10000u) == 0xFFFF, "filter as u16");
    }
}

namespace
{
    void TestMovementTerm()
    {
        MovementTermState s;
        s.enabled = s.puppetFoot = true;
        s.recordedPlayerMounted = true;
        Check(MovementTermCorrection(s, -2.0f) == 2.0f, "companion horse, player rode: term back");
        s.recordedPlayerMounted = false;
        Check(MovementTermCorrection(s, -2.0f) == 0.0f, "player on foot in the recording too: nothing");
        s.ownerIsPlayerPuppet = true;
        Check(MovementTermCorrection(s, -2.0f) == 2.0f, "the player's puppet: term back");
        s.riderIsPlayer = true;
        Check(MovementTermCorrection(s, -2.0f) == 0.0f, "the engine already gave 0 dB (rider answers player)");
        s.riderIsPlayer = false;
        s.livePlayerMounted = true;
        Check(MovementTermCorrection(s, -2.0f) == 0.0f, "live player mounted: the engine gave 0 dB");
        s.livePlayerMounted = false;
        s.puppetFoot = false;
        Check(MovementTermCorrection(s, -2.0f) == 0.0f, "not a puppet: never");
        s.puppetFoot = true;
        s.enabled = false;
        Check(MovementTermCorrection(s, -2.0f) == 0.0f, "key off: never");
    }
}

int RunFootMaterialTests()
{
    TestFootMix();
    TestMovementTerm();
    {
        // FootDrive: the slot volume carries the recorded base.
        FootDrive drive;
        drive.baseKnown = true;
        drive.base = -2.15f;
        const float v = SlotVolumeWithBase(-4.0f, drive, 0.0f, true);
        Check(v < -6.149f && v > -6.151f, "slot + recorded base - puppet base");
        Check(SlotVolumeWithBase(-4.0f, drive, 0.0f, false) == -4.0f, "base off: the slot alone");
        Check(SlotVolumeWithBase(-4.0f, FootDrive{}, 0.0f, true) == -4.0f, "base unknown: the slot alone");
        Check(ImpactOffset(0) == 0x160 && ImpactOffset(3) == 0x160 + 3 * 0x90, "impact per foot (0x318D93D)");
    }
    TestLayout();
    TestNeedsWrite();
    TestPairing();
    TestSampleBeforeFootDown();
    TestExpire();
    TestCapacity();
    if (g_footFailures == 0)
        std::printf("Foot material tests passed.\n");
    return g_footFailures;
}
