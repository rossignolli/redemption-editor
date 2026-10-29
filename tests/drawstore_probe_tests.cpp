// The DRAWSTORE_SLOT rules (drawstore_probe_core.h). The signature patterns
// parse, the masked match, the rel32 arithmetic (on bytes of the 1491.50
// dump), the header bounds, the entry decode (free slot, fields at their
// offsets), the census of the entries with flag bit 0, the load callback
// verdict and the line's text.
#include "drawstore_probe_core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using namespace replay::drawstore;

    int g_dsFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_dsFailures;
    }

    void TestPatterns()
    {
        std::uint8_t bytes[8] = {};
        bool wild[8] = {};
        std::size_t count = 0;
        Check(ParsePattern("48 8D ?? 0d", bytes, wild, sizeof(bytes), count) && count == 4, "a pattern with a wildcard parses");
        Check(bytes[0] == 0x48 && bytes[1] == 0x8D && wild[2] && !wild[3] && bytes[3] == 0x0D, "bytes and the wildcard mask");
        Check(!ParsePattern("48 8", bytes, wild, sizeof(bytes), count), "a half byte is refused");
        Check(!ParsePattern("48 XY", bytes, wild, sizeof(bytes), count), "a non hex byte is refused");
        Check(!ParsePattern("488D", bytes, wild, sizeof(bytes), count), "bytes without a separator are refused");
        Check(!ParsePattern("", bytes, wild, sizeof(bytes), count), "an empty pattern is refused");
        Check(!ParsePattern("01 02 03 04 05 06 07 08 09", bytes, wild, sizeof(bytes), count), "a pattern past the capacity is refused");

        const std::uint8_t code[] = {0x48, 0x8D, 0x05, 0x63, 0x8F, 0xAE, 0x00, 0x48, 0x89, 0x03};
        Check(PatternMatches("48 8D 05 ?? ?? ?? ?? 48 89 03", code, sizeof(code)), "the vtable write matches");
        Check(!PatternMatches("48 8D 05 ?? ?? ?? ?? 48 89 07", code, sizeof(code)), "a different byte does not");
        Check(!PatternMatches("48 8D 05 ?? ?? ?? ?? 48 89 03 C3", code, sizeof(code)), "a pattern longer than the code does not");

        for (const Signature& s : kSignatures)
        {
            std::uint8_t want[96] = {};
            bool w[96] = {};
            Check(ParsePattern(s.bytes, want, w, sizeof(want), count), s.name);
        }
    }

    void TestRel32()
    {
        // 1491.50: lea rcx, [rip + 0x5A12425] at 0xE8284 (7 bytes) reaches the store.
        Check(Rel32Target(0xE8284, 7, 0x5A12425) == kStoreRva, "the static ctor's lea reaches the store");
        // call 0x2B85BF8 at 0xE828B (e8 68 d9 a9 02).
        Check(Rel32Target(0xE828B, 5, 0x02A9D968) == 0x2B85BF8, "the static ctor's call reaches the store ctor");
        // A negative displacement.
        Check(Rel32Target(0xAA904E, 7, -0xB729) == 0xA9D92C, "a negative displacement");
        bool loadObject = false;
        for (const Rel32Check& r : kRel32Checks)
            loadObject = loadObject || r.target == kStoreRva + kStoreLoadObject;
        Check(loadObject, "the load callback object is checked against the store's +0xA0");
    }

    void TestHeader()
    {
        StoreHeader h;
        h.entries = 0x20000000000ull;
        h.flags = 0x20000100000ull;
        h.count = 0x10000;
        h.entrySize = 0x28;
        Check(HeaderSane(h), "a plausible header");
        StoreHeader bad = h;
        bad.count = 0;
        Check(!HeaderSane(bad), "no entries");
        bad = h;
        bad.count = kMaxEntries + 1;
        Check(!HeaderSane(bad), "too many entries");
        bad = h;
        bad.entrySize = 0x20;
        Check(!HeaderSane(bad), "an entry too small for the flags");
        bad = h;
        bad.entries = 0x1000;
        Check(!HeaderSane(bad), "entries in the null region");
    }

    std::vector<std::uint8_t> MakeEntry(std::uint64_t object, std::uint32_t name, std::int32_t parent, std::uint16_t flags)
    {
        std::vector<std::uint8_t> e(0x28, 0);
        std::memcpy(e.data() + kEntryObject, &object, 8);
        const std::uint32_t raw = 7;
        std::memcpy(e.data() + kEntryRaw08, &raw, 4);
        std::memcpy(e.data() + kEntryName, &name, 4);
        std::memcpy(e.data() + kEntryParentTxd, &parent, 4);
        std::memcpy(e.data() + kEntryFlags, &flags, 2);
        return e;
    }

    void TestEntry()
    {
        const auto bytes = MakeEntry(0x1E946570000ull, 0xA1B2C3D4u, 812, 0x3);
        const Entry e = DecodeEntry(0x01, bytes.data());
        Check(e.used && e.object == 0x1E946570000ull && e.name == 0xA1B2C3D4u && e.parentTxd == 812 && e.flags == 0x3 && e.raw08 == 7,
            "the fields at their offsets");
        const Entry freeSlot = DecodeEntry(kSlotFree, bytes.data());
        Check(!freeSlot.used && freeSlot.object == 0 && freeSlot.flags == 0, "a free slot (0x80) has nothing");
    }

    void TestCensus()
    {
        // 10 entries: 0 free, 1 loaded plain, 2..9 with bit 0 (odd ones loaded).
        const std::uint32_t size = 0x28;
        std::vector<std::uint8_t> flags(10, 0);
        std::vector<std::uint8_t> entries(10 * size, 0);
        flags[0] = kSlotFree;
        auto put = [&](int n, std::uint64_t object, std::uint32_t name, std::uint16_t f) {
            const auto e = MakeEntry(object, name, -1, f);
            std::memcpy(entries.data() + n * size, e.data(), size);
        };
        put(0, 0x1000000ull, 0x11, 0x1); // free: ignored even with the bit
        put(1, 0x2000000ull, 0x22, 0x2);
        for (int n = 2; n < 10; ++n)
            put(n, (n & 1) ? 0x3000000ull + n : 0, 0x100u + n, 0x1);
        Census c;
        AddToCensus(c, 0, flags.data(), entries.data(), 5, size);
        AddToCensus(c, 5, flags.data() + 5, entries.data() + 5 * size, 5, size);
        Check(c.scanned == 10 && c.used == 9, "scanned and used");
        Check(c.loaded == 5, "loaded: entry 1 and the odd ones");
        Check(c.wholeDelete == 8 && c.wholeDeleteLoaded == 4, "bit 0 counted on used entries only");
        Check(c.listed == kFirstListed && c.first[0].slot == 2 && c.first[0].name == 0x102 && !c.first[0].loaded &&
                c.first[1].slot == 3 && c.first[1].loaded && c.first[kFirstListed - 1].slot == 7,
            "the first ones listed in slot order, across the chunk boundary");

        std::string text;
        c.complete = true;
        AppendCensus(text, c);
        Check(text.find(",census=complete,scanned=10,used=9,loaded=5,whole_delete=8,whole_delete_loaded=4,first=0x2:0x00000102:0;0x3:")
                == 0,
            "the census text");
        Census empty;
        std::string none;
        AppendCensus(none, empty);
        Check(none.find("first=-") != std::string::npos && none.find("census=partial") != std::string::npos, "an empty census");
    }

    void TestCallback()
    {
        const std::uint64_t base = 0x7FF71E410000ull;
        Check(ClassifyCallback(true, 0, base + kGameLoadCallbackRva, base) == Callback::Game, "the game's callback");
        Check(ClassifyCallback(true, 0, base + 0x1234, base) == Callback::Other, "another function");
        Check(ClassifyCallback(true, 0x12345678, base + kGameLoadCallbackRva, base) == Callback::Other, "an object set");
        Check(ClassifyCallback(false, 0, 0, base) == Callback::Unread, "unread");
        Check(AddressText(base + 0xAB1510, base, 0x7397000) == "RDR2+0xAB1510", "an address in the image");
        Check(AddressText(0x7FFC00001000ull, base, 0x7397000) == "0x7FFC00001000", "an address outside it");
    }

    void TestChain()
    {
        SlotChain s;
        std::string out;
        AppendChain(out, s);
        Check(out == ",slot_in_range=0", "a slot past the count");
        s.inRange = true;
        out.clear();
        AppendChain(out, s);
        Check(out == ",entry=unreadable", "an unreadable entry");
        s.entryRead = true;
        s.entry.used = true;
        s.entry.object = 0x1E946570000ull;
        s.entry.name = 0xABCDEF01u;
        s.entry.parentTxd = -1;
        s.entry.flags = 0x1;
        s.drawableRead = 1;
        s.drawableVtableOk = true;
        s.shaderGroup = 0x1E946572EB0ull;
        s.shaderGroupRead = 1;
        s.shaderGroupVtableOk = true;
        s.dictionary = 0x1E946573000ull;
        s.dictionaryRead = 1;
        s.dictionaryVtableOk = true;
        s.refs = 1;
        s.parent = 0x22200000000ull;
        s.parentRead = 0;
        out.clear();
        AppendChain(out, s);
        Check(out.find(",used=1,name=0xABCDEF01,parent_txd=-1,flags=0x0001,whole_delete=1,") == 0, "the entry text");
        Check(out.find(",object_read=read,object_vt=drawable,shader_group=0x1E946572EB0,shader_group_read=read,shader_group_vt=ok") !=
                std::string::npos,
            "the drawable and its shader group");
        Check(out.find(",txd_refs=1,txd_page_node=0x0,txd_parent=0x22200000000,parent_read=unreadable,parent_vt=-,parent_refs=0") !=
                std::string::npos,
            "the dictionary and an unreadable parent");
        Check(std::strchr(out.c_str(), '%') == nullptr, "no format character in the line (the crash handler prints it with %s)");
    }
}

int RunDrawStoreProbeTests()
{
    TestPatterns();
    TestRel32();
    TestHeader();
    TestEntry();
    TestCensus();
    TestCallback();
    TestChain();
    if (g_dsFailures == 0)
        std::printf("DrawableStore probe tests passed.\n");
    return g_dsFailures;
}
