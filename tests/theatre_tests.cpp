// Closed theater (world_theatre.h): the engine functions' signatures and sites
// on the offline dump's addresses, the census reasons and the spawn label copy.
#include "world_theatre.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>

namespace
{
    using namespace replay::theatre;

    int g_theatreFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_theatreFailures;
    }

    void TestSites()
    {
        using replay::sig::Signature;
        Check(kPedFlushQueues.rva == 0xC03C84 && kPedFlushQueues.parent == &kPedFlushCall &&
                kPedFlushCall.parent == &kRemoveAllPeds,
            "sites: RemoveAllPedsWithException calls FlushPedQueues 0xC03C84");
        Check(kPedDeferredDestroy.rva == 0x73381C && kPedDeferredDestroy.parent == &kPedDestroyJump &&
                kPedDestroyJump.parent == &kRemoveAllPeds,
            "sites: and ends with a jmp to the deferred destruction 0x73381C");
        Check(kFlushVehicleReuse.rva == 0x130DD84 && kFlushVehicleReuse.parent == &kVehicleReuseCall &&
                kVehicleReuseCall.parent == &kRemoveAllVehs,
            "sites: RemoveAllVehs calls FlushVehicleReusePool 0x130DD84");
        Check(kStaticEmitterManager.rva == 0x5C505C0 && kStaticEmitterManager.parent == &kEmitterReset &&
                kStaticEmitters.parent == &kEmitterReset,
            "sites: the static emitter manager is 0x5C505C0, passed to the reset");
        Check(kPerscharManager.rva == 0x4A35800, "sites: the perschar manager pointer is 0x4A35800");
        Check(kSpeechManager.rva == 0x5BB9750 && kAmbientRelease.parent == kSpeechManager.parent,
            "sites: the speech manager is 0x5BB9750, passed to the release");
        // A call or jump site: its opcode, the rel32 masked.
        for (const Signature* site :
            {&kPedFlushCall, &kPedDestroyJump, &kVehicleReuseCall, &replay::enginesig::kConversationSceneCall})
        {
            replay::sig::Pattern pattern;
            Check(replay::sig::Parse(site->pattern, pattern) && pattern.size == 5 && !pattern.wild[0] && pattern.wild[1] &&
                    pattern.wild[4],
                "sites: an opcode and its rel32");
        }
    }

    void TestSignatures()
    {
        using replay::sig::Signature;
        const Signature* all[] = {&kSpawnFunnel, &replay::enginesig::kMixerStartScene, &kRemoveAllVehs, &kFlushVehicleReuse,
            &kRemoveAllPeds, &kStaticEmitters, &kSpeechRules};
        const std::uint32_t rvas[] = {0xBF23D4, 0x2F19018, 0x134E95C, 0x130DD84, 0xC0F83C, 0x312F7A4, 0x2F972C4};
        for (std::size_t n = 0; n < std::size(all); ++n)
        {
            replay::sig::Pattern pattern;
            Check(all[n]->rva == rvas[n], "signatures: the research's RVAs");
            Check(replay::sig::Parse(all[n]->pattern, pattern) && pattern.size >= 24, "signatures: 24 bytes at least");
        }
        // The two generic 16-byte prologues differ past byte 16 (the rules'
        // first 14 bytes are ??: it is hooked).
        Check(std::strncmp(kRemoveAllPeds.pattern + 14 * 3, kSpeechRules.pattern + 14 * 3, 2 * 3) == 0 &&
                std::strncmp(kRemoveAllPeds.pattern + 14 * 3, kSpeechRules.pattern + 14 * 3, 10 * 3) != 0,
            "signatures: the shared prologue is told apart by the next bytes");
        Check(kRemovePedsForceAllRandomAndMission == 4, "peds: mode 4 is ForceAllRandomAndMission");
    }

    void TestCensus()
    {
        PedFacts facts;
        Check(ClassifyLeft(facts) == LeftReason::Other, "census: nothing known is other");
        facts.dead = true;
        Check(ClassifyLeft(facts) == LeftReason::Dead, "census: dead");
        facts.onMount = true;
        Check(ClassifyLeft(facts) == LeftReason::Rider, "census: a rider before dead");
        facts.scripted = true;
        Check(ClassifyLeft(facts) == LeftReason::Scripted, "census: a script's entity before a rider");
        facts.mission = true;
        Check(ClassifyLeft(facts) == LeftReason::Mission, "census: mission before scripted");
        facts.perschar = true;
        Check(ClassifyLeft(facts) == LeftReason::Perschar, "census: a persistent character first of the vetoes");
        facts.pendingDestroy = true;
        Check(ClassifyLeft(facts) == LeftReason::PendingDestroy, "census: a ped waiting for destruction is not alive");
        facts.player = true;
        Check(ClassifyLeft(facts) == LeftReason::Player, "census: the player");
        Check(std::strcmp(LeftReasonName(LeftReason::PlayerGroup), "player_group") == 0, "census: names");
    }

    void TestLabel()
    {
        char out[16]{};
        CopyLabel("Scenario Mgr 1", 14, out, sizeof(out));
        Check(std::strcmp(out, "Scenario Mgr 1") == 0, "label: copied");
        CopyLabel("a,b\nc", 5, out, sizeof(out));
        Check(std::strcmp(out, "a_b_c") == 0, "label: commas and line ends never reach the log line");
        CopyLabel("CPedPopulation::AddAmbientPedToPopulation", 40, out, sizeof(out));
        Check(std::strlen(out) == sizeof(out) - 1, "label: cut to the buffer");
        CopyLabel(nullptr, 10, out, sizeof(out));
        Check(out[0] == '\0', "label: no text");
    }
}

int RunTheatreTests()
{
    TestSites();
    TestSignatures();
    TestCensus();
    TestLabel();
    if (!g_theatreFailures)
        std::printf("theatre tests passed\n");
    return g_theatreFailures;
}
