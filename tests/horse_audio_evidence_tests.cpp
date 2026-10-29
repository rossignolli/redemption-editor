// The pure part of the player's horse audio evidence: field names, the
// 0x8B69B0 replica answer, the record/play comparison text.
#include "horse_audio_evidence.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace
{
    int g_horseFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_horseFailures;
    }

    void TestFieldNames()
    {
        using namespace replay::horseaudio;
        std::set<std::string> names;
        for (std::size_t field = 0; field < kFieldCount; ++field)
        {
            const std::string name = FieldName(field);
            Check(name != "?" && !name.empty(), "every field has a name");
            names.insert(name);
        }
        Check(names.size() == kFieldCount, "field names are unique");
        Check(std::strcmp(FieldName(kExtSlot10), "x0498") == 0, "slot 10 is +0x498 (the step settings hash)");
        Check(std::strcmp(FieldName(kRowE0Flags), "row_e0_400") == 0, "the last field is row +0xE0 +0x400");
        Check(std::strcmp(FieldName(kFieldCount), "?") == 0, "out of range");
    }

    void TestLinkAnswer()
    {
        using replay::horseaudio::LinkAnswer;
        Check(LinkAnswer(true, true, 2) == 1, "bit 1 set, bit 0 clear: the local player");
        Check(LinkAnswer(true, true, 3) == 0, "bit 0 set: not the local player");
        Check(LinkAnswer(true, true, 0) == 0, "no player bit");
        Check(LinkAnswer(true, false, 2) == 2, "no linked entity");
        Check(LinkAnswer(false, true, 2) == 3, "unreadable");
    }

    void TestDiff()
    {
        using namespace replay::horseaudio;
        State record;
        State play;
        record.ok = play.ok = true;
        for (std::size_t field = 0; field < kFieldCount; ++field)
        {
            record.read[field] = play.read[field] = 1;
            record.values[field] = play.values[field] = field;
        }
        record.ped = 1;
        play.ped = 2; // identity, not compared
        std::size_t count = 99;
        Check(DiffStates(record, play, count).empty() && count == 0, "same fields: no diff");
        Check(SameState(record, play), "same state");
        Check(DiffMask(record, play) == 0, "empty mask");
        Check(MaskNames(0) == "none", "no names");
        // Hypothesis shape: the live horse answers yes, the puppet's step
        // settings differ.
        record.values[kLinkIsPlayer] = 1;
        play.values[kLinkIsPlayer] = 0;
        play.values[kAudio1450] = 0x1234;
        play.read[kFoot0428] = 0;
        const std::string diff = DiffStates(record, play, count);
        Check(count == 3, "three fields differ");
        Check(diff.find("link_is_player:0x1>0x0") != std::string::npos, "link answer in the diff");
        Check(diff.find("a1450:0xb>0x1234") != std::string::npos, "settings pointer in the diff");
        Check(diff.find("f0428:0x1b>na") != std::string::npos, "an unreadable field differs");
        const std::uint64_t mask = DiffMask(record, play);
        Check(MaskNames(mask) == "link_is_player|a1450|f0428", "mask names in field order");
        Check(!SameState(record, play), "different state");
        const std::string text = FormatState(play);
        Check(text.find("ok=1,layout=0x0,ext_vt=0x0,row_mask=0x0,") == 0, "state text starts with the identity");
        Check(text.find(",f0428=na,") != std::string::npos, "unreadable field shows na");
        Check(text.find(",row_e0_400=0x23") != std::string::npos, "last field");
    }

    void TestRefKeyOrder()
    {
        using replay::horseaudio::RefKey;
        RefKey a{0x80000d002f7ab1ull, 0x318DAFE, 8};
        RefKey b{0x80000d002f7ab1ull, 0x318DAFE, 0};
        RefKey c{0x80000d002f36cbull, 0x318DAFE, 0};
        Check(b < a && !(a < b), "p138 orders last");
        Check(c < b, "ref orders first");
    }
}

int RunHorseAudioEvidenceTests()
{
    TestFieldNames();
    TestLinkAnswer();
    TestDiff();
    TestRefKeyOrder();
    if (g_horseFailures == 0)
        std::printf("Horse audio evidence tests passed.\n");
    return g_horseFailures;
}
