// The signature rules (game_sig_core.h): the pattern text, the indexed pair,
// the match at a buffer, the one-pass search of many patterns (unique,
// missing, more than once, at the edges, from a second section), the rel32
// arithmetic, the list the signatures join and where a derived one lands.
#include "game_sig_core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using namespace replay::sig;

    int g_sigFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_sigFailures;
    }

    const Signature kTestFirst{"test_first", "48 8B 05 ?? ?? ?? ?? 81 E1", 0x1000};
    const Signature kTestSecond{"test_second", "E8 ?? ?? ?? ??", 0x2000};
    const Signature kTestCallee{"test_callee", kTestFirst, Rel32{1, 5}, "8B 89 ?? ??", 0x3000};
    const Signature kTestGlobal{"test_global", kTestCallee, Rel32{3, 7}, "", 0x4000};
    const Signature kTestSite{"test_site", kTestFirst, Offset{0x10}, "C3", 0x1010};
    const Signature kTestSlot{"test_slot", kTestGlobal, Pointer{0x120}, "48 8B C4", 0x5000};
    const Signature kTestObject{"test_object", kTestGlobal, Instance{}, "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 17 00 00 00", 0x6000};
    const Signature kTestFileObject{"test_file_object", kTestGlobal, Instance{true}, "", 0x7000};

    void TestParse()
    {
        Pattern p;
        Check(Parse("48 8B 05 ?? ?? ?? ?? 81 E1", p) && p.size == 9, "a pattern with wildcards parses");
        Check(p.bytes[0] == 0x48 && p.bytes[2] == 0x05 && p.wild[3] && p.wild[6] && !p.wild[7] && p.bytes[8] == 0xE1,
            "bytes and the wildcard mask");
        Check(p.hasKey && p.key == 7, "the indexed pair is the rarest one (81 E1, not 48 8B)");
        Check(Parse("48 8b ? 05", p) && p.size == 4 && p.wild[2] && p.bytes[1] == 0x8B, "a single ? and lower case");
        Check(Parse("E8 ?? ?? ?? ??", p) && p.size == 5 && !p.hasKey, "one fixed byte: checked at an address, not searched");
        Check(!Parse("48 8", p), "a half byte is refused");
        Check(!Parse("48 XY", p), "a non hex byte is refused");
        Check(!Parse("488D", p), "bytes without a separator are refused");
        Check(!Parse("?A", p), "a wildcard glued to a byte is refused");
        Check(!Parse("", p), "an empty pattern is refused");
        Check(!Parse(nullptr, p), "no text is refused");
        Check(!Parse("?? ??", p), "only wildcards are refused");
        std::string longest;
        for (std::size_t n = 0; n < kMaxPatternBytes; ++n)
            longest += n ? " 90" : "90";
        Check(Parse(longest.c_str(), p) && p.size == kMaxPatternBytes, "the longest pattern parses");
        Check(!Parse((longest + " 90").c_str(), p), "a pattern past the capacity is refused");
    }

    void TestMatch()
    {
        Pattern p;
        Parse("48 8D ?? 0D", p);
        const std::uint8_t code[] = {0x48, 0x8D, 0x77, 0x0D, 0xC3};
        Check(MatchesAt(p, code, sizeof(code)), "the wildcard takes any byte");
        const std::uint8_t other[] = {0x48, 0x8D, 0x77, 0x0E};
        Check(!MatchesAt(p, other, sizeof(other)), "a different fixed byte does not match");
        Check(!MatchesAt(p, code, 3), "a pattern longer than the code does not match");
        Check(!MatchesAt(p, nullptr, 8), "no code does not match");
    }

    void TestHooked()
    {
        const std::string hooked = "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 48 8B D9";
        Check(HookTolerant(hooked.c_str()), "14 leading wildcards: a hooked function's signature");
        Check(!HookTolerant("?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 41 48 8B D9"), "13 are not enough");
        Check(!HookTolerant("?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ??"), "nothing proven after them");
        Check(!HookTolerant("E8 ?? ?? ?? ??"), "an ordinary pattern");
        // MinHook's jmp rel32 and a 14-byte jmp [rip], then the bytes kept.
        const std::uint8_t minHook[] = {0xE9, 1, 2, 3, 4, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x48, 0x8B, 0xD9};
        const std::uint8_t absolute[] = {0xFF, 0x25, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0x48, 0x8B, 0xD9};
        const std::uint8_t plain[] = {0x48, 0x89, 0x5C, 0x24, 0x08};
        Pattern p;
        Check(Parse(hooked.c_str(), p) && MatchesAt(p, minHook, sizeof(minHook)) && MatchesAt(p, absolute, sizeof(absolute)),
            "a hooked function still matches");
        Check(StartsWithJump(minHook, sizeof(minHook)) && StartsWithJump(absolute, sizeof(absolute)) &&
                !StartsWithJump(plain, sizeof(plain)) && !StartsWithJump(minHook, 1),
            "the detour's jump is seen");
    }

    void TestSearch()
    {
        // 300 bytes of filler with the patterns placed in it.
        std::vector<std::uint8_t> code(300, 0xCC);
        const std::uint8_t unique[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xDA};
        std::memcpy(code.data() + 100, unique, sizeof(unique));
        const std::uint8_t twice[] = {0x0F, 0x28, 0xC1, 0xF3, 0x0F, 0x59};
        std::memcpy(code.data() + 10, twice, sizeof(twice));
        std::memcpy(code.data() + 200, twice, sizeof(twice));
        const std::uint8_t atEnd[] = {0x5C, 0x24, 0x08, 0x57};
        std::memcpy(code.data() + code.size() - sizeof(atEnd), atEnd, sizeof(atEnd));
        const std::uint8_t atStart[] = {0x4D, 0x8B, 0xC8};
        std::memcpy(code.data(), atStart, sizeof(atStart));
        // A call whose rel32 changes between builds.
        const std::uint8_t call[] = {0xE8, 0x11, 0x22, 0x33, 0x44, 0x84, 0xC0, 0x74, 0x10};
        std::memcpy(code.data() + 150, call, sizeof(call));

        Pattern pUnique, pTwice, pEnd, pStart, pCall, pMissing, pNoKey;
        Parse("40 53 48 83 EC 20 8B DA", pUnique);
        Parse("0F 28 C1 F3 0F 59", pTwice);
        Parse("5C 24 08 57", pEnd);
        Parse("4D 8B C8", pStart);
        Parse("?? ?? ?? ?? ?? 84 C0 74 10", pCall); // the key pair is past the wildcards
        Parse("C1 F3 0F 5A", pMissing);
        Parse("CC ?? ?? ?? ??", pNoKey);
        Pattern pCallRel;
        Parse("E8 ?? ?? ?? ?? 84 C0 74 10", pCallRel);
        const Pattern* list[] = {&pUnique, &pTwice, &pEnd, &pStart, &pCall, &pMissing, &pNoKey, &pCallRel};
        const ScanIndex index(list, sizeof(list) / sizeof(list[0]));
        Check(index.Count() == 8, "every pattern is in the index");
        Hits hits[8];
        index.Scan(code.data(), code.size(), 0x1000, hits);
        Check(hits[0].count == 1 && hits[0].first == 0x1000 + 100, "a unique pattern: once, at its offset plus the origin");
        Check(hits[1].count == 2 && hits[1].first == 0x1000 + 10, "twice: counted as more than one, the first kept");
        Check(hits[2].count == 1 && hits[2].first == 0x1000 + code.size() - sizeof(atEnd), "a match ending at the last byte");
        Check(hits[3].count == 1 && hits[3].first == 0x1000, "a match at the first byte");
        Check(hits[4].count == 1 && hits[4].first == 0x1000 + 150, "leading wildcards: the match starts before its key");
        Check(hits[5].count == 0, "a pattern not in the code is missing");
        Check(hits[6].count == 0, "a pattern without an indexed pair is never searched");
        Check(hits[7].count == 1 && hits[7].first == 0x1000 + 150, "the call matches with any rel32");

        // A second section adds to the same hits (the unique one now twice).
        std::vector<std::uint8_t> second(64, 0x90);
        std::memcpy(second.data() + 20, unique, sizeof(unique));
        index.Scan(second.data(), second.size(), 0x8000, hits);
        Check(hits[0].count == 2 && hits[0].first == 0x1000 + 100, "a match in another section makes it ambiguous");
        Check(hits[5].count == 0, "still missing after the second section");

        // Overlapping matches count.
        const std::uint8_t run[] = {0xAB, 0xAB, 0xAB};
        Pattern pRun;
        Parse("AB AB", pRun);
        const Pattern* one[] = {&pRun};
        const ScanIndex runIndex(one, 1);
        Hits runHits[1];
        runIndex.Scan(run, sizeof(run), 0, runHits);
        Check(runHits[0].count == 2 && runHits[0].first == 0, "overlapping matches are two matches");

        // Nothing to scan.
        Hits none[8];
        index.Scan(nullptr, 100, 0, none);
        index.Scan(code.data(), 1, 0, none);
        Check(none[0].count == 0 && none[3].count == 0, "no data or one byte finds nothing");
    }

    void TestRel32()
    {
        // lea rcx, [rip + 0x5A12425] at 0xE8284 (7 bytes).
        Check(Rel32Target(0xE8284, 7, 0x5A12425) == 0xE8284 + 7 + 0x5A12425, "a positive displacement");
        Check(Rel32Target(0xAA904E, 7, -0xB729) == 0xA9D92C, "a negative displacement");
        Check(Rel32Target(0x7FF600001000, 5, 0x10) == 0x7FF600001015, "an absolute address in the game");
    }

    void TestDerived()
    {
        // call +0x20 at the parent's start (rel32 at 1, instruction of 5 bytes).
        Check(DerivedAddress(kTestCallee, 0x7FF600001000, 0x20) == 0x7FF600001025, "a call lands after its instruction plus the rel32");
        Check(DerivedAddress(kTestCallee, 0x7FF600001000, -0x1005) == 0x7FF600000000, "a call backwards");
        // mov rax, [rip + x] (disp at 3, 7 bytes).
        Check(DerivedAddress(kTestGlobal, 0x7FF600002000, 0x100) == 0x7FF600002107, "a global from a RIP-relative operand");
        Check(DerivedAddress(kTestSite, 0x7FF600001000, 12345) == 0x7FF600001010, "a site ignores the displacement");
        Check(std::strcmp(ResultName(Result::Derived), "derived") == 0, "the log name of a derived result");
    }

    void TestList()
    {
        bool first = false, second = false;
        int count = 0;
        for (const Signature* s = FirstSignature(); s; s = s->link)
        {
            ++count;
            first = first || s == &kTestFirst;
            second = second || s == &kTestSecond;
        }
        Check(first && second && count >= 8, "every signature joins the list at construction");
        Check(kTestFirst.result.load() == Result::Pending && kTestFirst.address.load() == 0, "a new signature is pending");
        Check(kTestFirst.via == Via::Search && !kTestFirst.parent, "an anchor is searched and has no parent");
        Check(kTestCallee.via == Via::Rel32 && kTestCallee.parent == &kTestFirst && kTestCallee.at == 1 && kTestCallee.end == 5,
            "a callee: its parent and where the rel32 is");
        Check(kTestGlobal.parent == &kTestCallee && kTestGlobal.pattern[0] == 0, "a global: reached from a derived one, no pattern");
        Check(kTestSite.via == Via::Offset && kTestSite.at == 0x10, "a site: a distance inside its parent");
        Check(kTestSlot.via == Via::Pointer && kTestSlot.parent == &kTestGlobal && kTestSlot.at == 0x120,
            "a vtable slot: the pointer at a distance inside its parent");
        Check(kTestObject.via == Via::Instance && kTestObject.parent == &kTestGlobal && kTestObject.at == 0,
            "a static object found by its vtable");
        Check(kTestFileObject.via == Via::Instance && kTestFileObject.at == 1,
            "a static object the exe file holds (constant initialized), not one built at start");
        Pattern object;
        Check(Parse(kTestObject.pattern, object) && object.size == 16 && object.wild[0] && object.wild[7] && !object.wild[12] &&
                  object.bytes[12] == 0x17,
            "an object's pattern: the vtable pointer as ??, the fields it checks fixed");
        Check(std::strcmp(ResultName(Result::Ambiguous), "ambiguous") == 0 && std::strcmp(ResultName(Result::Hint), "hint") == 0,
            "the log names of the results");
    }
}

int RunGameSigTests()
{
    TestParse();
    TestMatch();
    TestHooked();
    TestSearch();
    TestRel32();
    TestDerived();
    TestList();
    if (g_sigFailures == 0)
        std::printf("Game signature tests passed.\n");
    return g_sigFailures;
}
