// Puppet sound rules: B1 (a puppet's own persistent sound without counterpart
// in the clip, puppet_audio_rule.h + sound_owner_table.h PacketLives), B2 (the
// ground layers and the recorded audio LOD, foot_material.h) and B3 (the ref
// a creator B packet is created again with, sound_generic_core.h).
#include "foot_material.h"
#include "puppet_audio_rule.h"
#include "sound_generic_core.h"
#include "sound_owner_table.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
    int g_puppetSoundFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_puppetSoundFailures;
    }

    // Pigs (packed ids), the vocal set they made in the recording and the pen
    // loop they made only in the replay (0x2588C2F into the by-ref port).
    constexpr std::uint32_t kPig97 = (97u << 16) | 179u;
    constexpr std::uint32_t kBoar = (139u << 16) | 634u;
    constexpr std::uint64_t kPigVocal = 0x00800004004366e8ull;
    constexpr std::uint64_t kPenLoop = 0x0080001d002c0fb4ull;
    constexpr std::uint64_t kPenLoopOtherEntry = 0x00800003002c0fb4ull;

    void TestUnrecordedRule()
    {
        using replay::puppetaudio::RefuseUnrecordedOwnPersistent;
        Check(RefuseUnrecordedOwnPersistent(true, true, true, false, false), "B1: owner never made the set, ESND does not carry it");
        Check(!RefuseUnrecordedOwnPersistent(false, true, true, false, false), "B1: gate off (ini 0)");
        Check(!RefuseUnrecordedOwnPersistent(true, false, true, false, false), "B1: the by-hash port is left out");
        Check(!RefuseUnrecordedOwnPersistent(true, true, false, false, false), "B1: no clip owner (hidden player, no lives)");
        Check(!RefuseUnrecordedOwnPersistent(true, true, true, true, false), "B1: the owner made the set somewhere in the clip");
        Check(!RefuseUnrecordedOwnPersistent(true, true, true, false, true), "B1: the ESND track carries it");

        // The pig's lives: only its vocal (ESND by speech) and nothing of
        // the pen loop; a boar that had the loop recorded.
        replay::soundowner::PacketLives lives;
        lives.Add(kPig97, 0x04004366e8ull, kPigVocal, 17222, 18000);
        lives.Add(kBoar, 0x1d002c0fb4ull, kPenLoop, 1000, 0xFFFFFFFFu);
        lives.Seal();
        Check(lives.Judge(kPig97, kPenLoop, 12705) == replay::soundowner::LifeVerdict::Passed,
            "B1: the port's old verdict for the pen loop is passed (no packet alive)");
        Check(lives.OwnerHasPackets(kPig97) && !lives.OwnerMadeSet(kPig97, kPenLoop),
            "B1: the pig has packets but never the pen loop's set: refused by the new rule");
        Check(lives.OwnerMadeSet(kBoar, kPenLoopOtherEntry),
            "B1: an owner that made the set (any entry, any time) keeps its own create");
        Check(replay::soundowner::LifeVerdictName(replay::soundowner::LifeVerdict::Unrecorded)[0] == 'u' &&
                replay::soundowner::kLifeVerdictCount == 5,
            "B1: the new verdict has a name and a slot in the tallies");
    }

    void TestLayers()
    {
        using namespace replay::footmaterial;
        // 0x318D297: [foot +0x3D0 + 4 * (5 * foot + layer)]; 0x318D7A1 reads
        // layer 1 of the foot as +0x3D4 + 20 * foot.
        Check(LayerOffset(0, 0) == 0x3D0 && LayerOffset(0, 4) == 0x3E0, "B2: layers of foot 0");
        for (std::uint8_t foot = 0; foot < kFeet; ++foot)
            Check(LayerOffset(foot, 1) == 0x3D4 + 20u * foot, "B2: layer 1 matches 0x318D7A1's index");
        Check(LayerOffset(3, 4) + 4 <= 0x420, "B2: the 20 weights end before +0x420 (the gait metadata)");
        Check(EncodeLayer(0.0f) == 0 && EncodeLayer(-1.0f) == 0 && EncodeLayer(0.731f) == 731 && EncodeLayer(1.0f) == 1000,
            "B2: thousandths");
        Check(EncodeLayer(100.0f) == 65535, "B2: clamped to the u16");
        Check(DecodeLayer(731) > 0.7309f && DecodeLayer(731) < 0.7311f, "B2: decode");
        Check(!NeedsLayerWrite(0.731f, DecodeLayer(EncodeLayer(0.731f))), "B2: a round trip is not a write");
        Check(NeedsLayerWrite(0.0f, 0.25f) && NeedsLayerWrite(0.8f, 0.0f), "B2: another weight is a write");
        Check(PlausibleLayer(0.0f) && PlausibleLayer(1.0f) && !PlausibleLayer(-0.5f) && !PlausibleLayer(1e9f),
            "B2: plausible weights only");
        Material m;
        Check(!m.layersKnown && m.layers[0] == 0.0f, "B2: a material starts without layers");
    }

    void TestAudioLod()
    {
        using namespace replay::footmaterial;
        std::uint8_t lod = 9;
        Check(!PuppetAudioLodAt({}, 1000, 2, lod), "B2 LOD: no recorded LOD, the caller keeps 0");
        const std::vector<LodPoint> points = {{1000, 1}, {2000, 2}, {3000, 3}, {4000, 0}};
        Check(PuppetAudioLodAt(points, 500, 2, lod) && lod == 1, "B2 LOD: before the first step, the first step's");
        Check(PuppetAudioLodAt(points, 1000, 2, lod) && lod == 1, "B2 LOD: at a step");
        Check(PuppetAudioLodAt(points, 2500, 2, lod) && lod == 2, "B2 LOD: the latest step at the playhead");
        Check(PuppetAudioLodAt(points, 3500, 2, lod) && lod == 2, "B2 LOD: never above the foot's max LOD");
        Check(PuppetAudioLodAt(points, 3500, kLodUnknown, lod) && lod == 3, "B2 LOD: max unknown, up to 3");
        Check(PuppetAudioLodAt(points, 9000, 2, lod) && lod == 0, "B2 LOD: after the last step, the last one's");
        const std::vector<LodPoint> four = {{0, 4}};
        Check(PuppetAudioLodAt(four, 10, kLodUnknown, lod) && lod == 3, "B2 LOD: 4 (no override) is never asked");
    }

    void TestCreateRef()
    {
        using namespace replay::soundgeneric;
        bool byHash = true;
        // A creator B packet (core 1): the resolved ref.
        Check(CreateRefOf(IdKind::Hash, 0x448716ull, 0x448716ull, 1, true, byHash) == 0x448716ull && !byHash,
            "B3: a creator B packet by ref (the resolved ref, direct form)");
        Check(CreateRefOf(IdKind::Hash, 0x448716ull, 0x448716ull, 1, false, byHash) == 0x448716ull && byHash,
            "B3: SoundGenericCoreBByRef=0, the old by-hash create");
        // Known form: set + entry with the flag bit.
        Check(CreateRefOf(IdKind::RefSet, 0x1d002c0fb4ull, 0x80001d002c0fb4ull, 0, true, byHash) == 0x80001d002c0fb4ull &&
                !byHash,
            "B3: the known form unchanged");
        // Another form recorded at creator A: the raw ref whole (direct refs:
        // 0x2517CB0 passes a high dword whose & 0xF00000 is not 0x800000).
        Check(CreateRefOf(IdKind::None, 0, 0x000fffff0115f8fdull, 0, true, byHash) == 0x000fffff0115f8fdull && !byHash,
            "B3: 0xfffff0115f8fd by ref, whole");
        Check(CreateRefOf(IdKind::None, 0, 0x0100002f0117ce80ull, 0, true, byHash) == 0x0100002f0117ce80ull && !byHash,
            "B3: the bit 56 form by ref, whole");
        Check(((0x0100002f0117ce80ull >> 32) & 0xF00000u) != 0x800000u && ((0x000fffff0115f8fdull >> 32) & 0xF00000u) != 0x800000u &&
                ((0x0080001d002c0fb4ull >> 32) & 0xF00000u) == 0x800000u,
            "B3: which forms the resolver reads as indirect");
        Check(StableIdOf(0x448716ull, true).kind == IdKind::Hash, "B3: what the recorder calls a hash is creator B's ref");
    }
}

int RunPuppetSoundsTests()
{
    TestUnrecordedRule();
    TestLayers();
    TestAudioLod();
    TestCreateRef();
    if (!g_puppetSoundFailures)
        std::printf("puppet sound tests: ok\n");
    return g_puppetSoundFailures;
}
