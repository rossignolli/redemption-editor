// Closed theater (world_theatre.h): the engine functions' signatures and sites
// against the offline dump's addresses, the census reasons and the spawn label copy.
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
        Check(SiteTarget(kPedFlushSite) == 0xC03C84, "sites: RemoveAllPedsWithException calls FlushPedQueues 0xC03C84");
        Check(SiteTarget(kPedDestroySite) == 0x73381C, "sites: and ends with a jmp to the deferred destruction 0x73381C");
        Check(SiteTarget(kVehicleReuseSite) == 0x130DD84, "sites: RemoveAllVehs calls FlushVehicleReusePool 0x130DD84");
        Check(SiteTarget(kEmitterManagerSite) == 0x5C505C0, "sites: the static emitter manager is 0x5C505C0");
        Check(SiteTarget(kPerscharManagerSite) == 0x4A35800, "sites: the perschar manager pointer is 0x4A35800");
        Check(SiteTarget(kSpeechManagerSite) == 0x5BB9750, "sites: the speech manager is 0x5BB9750");
        for (const Site* site : {&kPedFlushSite, &kPedDestroySite, &kVehicleReuseSite, &kEmitterManagerSite,
                 &kPerscharManagerSite, &kSpeechManagerSite})
        {
            Check(SiteMatches(*site, site->bytes, site->size), "sites: a site matches its own bytes");
            std::uint8_t other[8]{};
            std::memcpy(other, site->bytes, site->size);
            other[site->size - 1] ^= 1;
            Check(!SiteMatches(*site, other, site->size), "sites: one byte off does not match");
        }
    }

    void TestSignatures()
    {
        const Signature* all[] = {&kSpawnFunnelSignature, &kMixSceneSignature, &kRemoveAllVehsSignature,
            &kFlushVehicleReuseSignature, &kRemoveAllPedsSignature, &kStaticEmittersSignature, &kSpeechRulesSignature};
        const std::uint32_t rvas[] = {0xBF23D4, 0x2F19018, 0x134E95C, 0x130DD84, 0xC0F83C, 0x312F7A4, 0x2F972C4};
        for (std::size_t n = 0; n < std::size(all); ++n)
        {
            Check(all[n]->rva == rvas[n], "signatures: the research's RVAs");
            Check(SignatureMatches(*all[n], all[n]->bytes, sizeof(all[n]->bytes)), "signatures: match their bytes");
            Check(!SignatureMatches(*all[n], all[n]->bytes, 16), "signatures: 24 bytes are needed");
        }
        // The two generic 16-byte prologues differ past byte 16.
        Check(std::memcmp(kRemoveAllPedsSignature.bytes, kSpeechRulesSignature.bytes, 16) == 0 &&
                std::memcmp(kRemoveAllPedsSignature.bytes, kSpeechRulesSignature.bytes, 24) != 0,
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
