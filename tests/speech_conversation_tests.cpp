// The pure parts of the conversation lines asked again the engine's way: the
// sound set read from the line's data, its ref from the line's recorded
// sounds (an older clip), the Ask writes (duck byte and set), the ref the
// replay resolves, the per-line text and the playback's counts. The rows
// come from one recorded conversation.
#include "speech_conversation.h"
#include "speech_player_route.h"  // NamedVoiceOnPlayer
#include "speech_record_match.h"  // lines asked before the recording

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    using namespace replay::speech;

    int g_speechConversationFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_speechConversationFailures;
    }

    // The fields of an ESND row SetRefFromSounds reads.
    struct Id
    {
        unsigned packed = 0;
        bool operator==(const Id& other) const { return packed == other.packed; }
    };
    struct Row
    {
        Id entity;
        std::uint32_t startMs = 0;
        std::uint64_t soundRef = 0;
        std::uint32_t caller = 0;
        bool pedAudio = true;
        bool byHash = false;
    };

    Id Ped(unsigned slot, unsigned instance) { return Id{(slot << 16) | instance}; }

    request::Fields ScriptLine(std::uint32_t bits)
    {
        request::Fields f;
        f.read = true;
        f.value[request::D8] = 2;
        f.value[request::Source] = 2;
        f.value[request::LineBits] = bits;
        return f;
    }

    void TestDataBlock()
    {
        // data +0xE8: object, +0xF0 hash, +0xF4 ref (0x2FA9327 copies params +0xC0..+0xCF).
        std::uint8_t block[request::kDataSetBytes] = {};
        const std::uint64_t object = 0x25012345678ull;
        const std::uint32_t hash = 0x1234ABCDu;
        const std::uint32_t ref = 0x438F79u;
        std::memcpy(block, &object, 8);
        std::memcpy(block + 8, &hash, 4);
        std::memcpy(block + 0xC, &ref, 4);
        request::Fields f;
        Check(request::SetFromDataBlock(block, sizeof(block), f) && f.setRead && f.setHash == hash && f.setRef == ref,
            "data block: hash and ref");
        request::Fields shortRead;
        Check(!request::SetFromDataBlock(block, sizeof(block) - 1, shortRead) && !shortRead.setRead, "data block: short read");
        // The line block read keeps the set (it comes from the data).
        std::uint8_t line[request::kLineBlockBytes] = {};
        Check(request::FromLineBlock(line, sizeof(line), f) && f.read && f.setRead && f.setRef == ref,
            "line block: keeps the set");
        request::Fields a = f;
        request::Fields b = f;
        b.setRef = 0x436465u;
        Check(request::SameFields(a, a) && !request::SameFields(a, b), "same fields: the set counts");
        Check(request::DiffText(2, a, 2, b) == "set:0x438F79>0x436465", "diff text: the set ref");
        request::Fields unreadSet = b;
        unreadSet.setRead = false;
        Check(request::DiffText(2, a, 2, unreadSet) == "none", "diff text: no set side, no set diff");
    }

    void TestChildRefs()
    {
        using namespace conversation;
        Check(SetRefOfChild(0x0080000300438F79ull) == 0x438F79u, "child ref: layer 3 of set 0x438F79");
        Check(SetRefOfChild(0x0080000400436465ull) == 0x436465u, "child ref: wave of set 0x436465");
        Check(SetRefOfChild(0x00000000002F49ACull) == 0, "child ref: a direct ref is not a set's child");
        Check(SetRefOfChild(0x01800003004366E8ull) == 0, "child ref: other high bits");
        Check(SetRefOfChild(0x00800003FFFFFFFFull) == 0 && SetRefOfChild(0x0080000300000000ull) == 0, "child ref: no set ref");
        Check(ValidSetRef(0x438F79u) && !ValidSetRef(0) && !ValidSetRef(0xFFFFFFFFu), "valid ref");
    }

    void TestSounds1149()
    {
        using namespace conversation;
        // The recorded ESND around the lines of 7:8 (41162) and 4:5 (39377).
        std::vector<Row> rows;
        rows.push_back({Ped(4, 5), 39327, 0x0080000500436465ull, kWaveSoundCaller});
        rows.push_back({Ped(4, 5), 39377, 0x0080000400436465ull, kLayerSoundCaller});
        rows.push_back({Ped(7, 8), 39578, 0x0080000400438F79ull, kWaveSoundCaller});
        rows.push_back({Ped(7, 8), 41162, 0x0080000300438F79ull, kLayerSoundCaller});
        rows.push_back({Ped(7, 8), 41162, 0x0080000200438F79ull, kLayerSoundCaller});
        rows.push_back({Ped(7, 8), 41300, 0x00000000002F49ACull, 0x2588C2Fu}); // a step, not speech
        std::uint32_t at = 0;
        Check(SetRefFromSounds(rows, Ped(7, 8), 2, 41162, at) == 0x438F79u && at == 41162, "sounds: 7:8 line at 41162");
        Check(SetRefFromSounds(rows, Ped(4, 5), 2, 39378, at) == 0x436465u && at == 39377, "sounds: 4:5 AI line");
        Check(SetRefFromSounds(rows, Ped(1, 2), 2, 41162, at) == 0, "sounds: another speaker");
        Check(SetRefFromSounds(rows, Ped(7, 8), 2, 30000, at) == 0, "sounds: nothing near the start");
        // A line not timed by its start: the sounds come up to 2.5 s later.
        Check(SetRefFromSounds(rows, Ped(7, 8), 2, 39000, at) == 0x438F79u && at == 41162, "sounds: after the request");
        // Only the layers prove the line's set (the wave may come from a default set).
        Check(SetRefFromSounds(rows, Ped(7, 8), 2, 39578, at) == 0x438F79u && at == 41162, "sounds: type 2, not the wave");
        Check(SetRefFromSounds(rows, Ped(7, 8), 1, 39578, at) == 0, "sounds: a type 1 line gets no set");
        Check(SetRefFromSounds(rows, Ped(7, 8), 3, 41162, at) == 0, "sounds: a type 3 line gets no set");
        Check(SetRefFromSounds(rows, Ped(4, 5), 0, 39378, at) == 0x436465u && at == 39377, "sounds: unknown type, the layers");
        std::vector<Row> waveOnly = {{Ped(7, 8), 41162, 0x0080000400438F79ull, kWaveSoundCaller}};
        Check(SetRefFromSounds(waveOnly, Ped(7, 8), 2, 41162, at) == 0, "sounds: a type 2 line without layers has no set");
        Check(!SetFromSoundsAllowed(1) && SetFromSoundsAllowed(2) && SetFromSoundsAllowed(0) && !SetFromSoundsAllowed(3),
            "sounds: allowed by type");
        std::vector<Row> notPed = {{Ped(7, 8), 41162, 0x0080000300438F79ull, kLayerSoundCaller, false, false}};
        Check(SetRefFromSounds(notPed, Ped(7, 8), 2, 41162, at) == 0, "sounds: only the ped audio rows");
        std::vector<Row> byHash = {{Ped(7, 8), 41162, 0x0080000300438F79ull, kLayerSoundCaller, true, true}};
        Check(SetRefFromSounds(byHash, Ped(7, 8), 2, 41162, at) == 0, "sounds: a by-hash row is not a ref");
        // Two lines: the nearest one.
        std::vector<Row> two = {{Ped(7, 8), 48500, 0x0080000300436465ull, kLayerSoundCaller},
            {Ped(7, 8), 48646, 0x0080000300438F79ull, kLayerSoundCaller}};
        Check(SetRefFromSounds(two, Ped(7, 8), 2, 48646, at) == 0x438F79u, "sounds: the nearest row wins");
    }

    void TestApply()
    {
        using namespace conversation;
        // 7:8 scripted line: bits 0x02480000, duck bit clear.
        request::Fields script = ScriptLine(0x02480000u);
        request::Ask off;
        Apply(false, script, 0x438F79u, off);
        Check(off.Empty(), "apply: key off writes nothing");
        request::Ask fromSounds;
        Apply(true, script, 0x438F79u, fromSounds);
        Check(AskedDuck(fromSounds) == 0 && fromSounds.setFrom == request::SetFrom::EntitySounds &&
                  fromSounds.setRef == 0x438F79u && fromSounds.setHash == 0,
            "apply: old clip, duck 0 and the set of the line's sounds");
        // 4:5 AI line: bits 0x02688000, duck bit set (the ctor's own value).
        request::Ask ai;
        Apply(true, ScriptLine(0x02688000u), 0, ai);
        Check(AskedDuck(ai) == 1 && ai.setFrom == request::SetFrom::None, "apply: duck 1 kept, no set without one");
        // SPRQ v2: the recorded set wins over the sounds.
        request::Fields v2 = script;
        v2.setRead = true;
        v2.setHash = 0xCAFEF00Du;
        v2.setRef = 0x438F79u;
        request::Ask recorded;
        Apply(true, v2, 0x436465u, recorded);
        Check(recorded.setFrom == request::SetFrom::Recorded && recorded.setHash == 0xCAFEF00Du && recorded.setRef == 0x438F79u,
            "apply: the recorded set");
        // A line recorded with no set (both 0) stays with the context.
        request::Fields none = script;
        none.setRead = true;
        request::Ask leftAlone;
        Apply(true, none, 0x438F79u, leftAlone);
        Check(leftAlone.setFrom == request::SetFrom::None && AskedDuck(leftAlone) == 0, "apply: recorded without a set");
        // An old clip without request fields: no duck byte, the set of the sounds.
        request::Fields unread;
        request::Ask noFields;
        Apply(true, unread, 0x438F79u, noFields);
        Check(AskedDuck(noFields) == -1 && noFields.setFrom == request::SetFrom::EntitySounds, "apply: no fields read");
        // The script path's own duck write is the same byte (no second write).
        request::Ask both;
        scriptpath::Apply(true, false, script, both);
        Apply(true, script, 0x438F79u, both);
        Check(both.byteCount == 1 && AskedDuck(both) == 0, "apply: one duck byte with the script path");
    }

    void TestResolve()
    {
        using namespace conversation;
        request::Ask ask;
        ask.setFrom = request::SetFrom::Recorded;
        ask.setHash = 0xCAFEF00Du;
        ask.setRef = 0x438F79u;
        bool byHash = false;
        Check(RefToResolve(ask, true, 0x438F79u, byHash) == 0x438F79u && byHash, "resolve: by hash");
        // A hash this session does not have: the recorded ref is of another
        // layout and is not tried.
        Check(RefToResolve(ask, true, 0xFFFFFFFFu, byHash) == 0 && !byHash, "resolve: hash not found, no ref");
        Check(RefToResolve(ask, false, 0, byHash) == 0x438F79u && !byHash, "resolve: hash not looked up");
        request::Ask sounds;
        sounds.setFrom = request::SetFrom::EntitySounds;
        sounds.setRef = 0x438F79u;
        Check(RefToResolve(sounds, false, 0, byHash) == 0x438F79u && !byHash, "resolve: the sounds' ref");
        request::Ask nothing;
        nothing.setFrom = request::SetFrom::Recorded;
        nothing.setHash = 0x11u;
        Check(RefToResolve(nothing, true, 0xFFFFFFFFu, byHash) == 0, "resolve: no ref at all");
        Check(std::strcmp(SetStatusName(SetStatus::ByHash), "by_hash") == 0 &&
                  std::strcmp(SetStatusName(SetStatus::BadObject), "bad_object") == 0 &&
                  std::strcmp(SetFromName(request::SetFrom::EntitySounds), "entity_sounds") == 0,
            "names");
    }

    // The table check of a set found by a recorded ref: the
    // count and the name hashes as 0x49F6E4 reads them.
    void PutEntry(std::uint8_t* entries, std::uint32_t n, std::uint32_t hash)
    {
        std::memcpy(entries + static_cast<std::size_t>(n) * conversation::kSetEntryBytes, &hash, sizeof(hash));
    }

    void TestSetTable()
    {
        using namespace conversation;
        std::uint8_t entries[5 * kSetEntryBytes] = {};
        // The five layer names of a type 2 line (0x2FB3E6C), sorted.
        const std::uint32_t names[5] = {0x0FDF5A1Cu, 0x52F58009u, 0x623B8117u, 0x75E4276Du, 0xBEE1D782u};
        for (std::uint32_t n = 0; n < 5; ++n)
            PutEntry(entries, n, names[n]);
        Check(SetTableAscending(entries, 5), "set table: ascending");
        Check(SetTableAscending(entries, 1), "set table: one entry");
        Check(!SetTableAscending(entries, 0), "set table: empty");
        Check(!SetTableAscending(nullptr, 5), "set table: no bytes");
        Check(!SetTableAscending(entries, kSetMaxEntries + 1), "set table: count past the cap");
        PutEntry(entries, 3, 0x10000000u); // below entry 2
        Check(!SetTableAscending(entries, 5), "set table: out of order");
        PutEntry(entries, 3, 0x623B8117u); // equal to entry 2: still searchable
        Check(SetTableAscending(entries, 5), "set table: equal neighbours");
        Check(kSetCountOffset == 5 && kSetEntriesOffset == 9 && kSetEntryBytes == 12, "set table: layout (0x252B154, 0x252A8F4)");
    }

    void TestTally()
    {
        using namespace conversation;
        request::Fields script = ScriptLine(0x02480000u);
        request::Ask ask;
        Apply(true, script, 0x438F79u, ask);
        // Asked as an ambient line: bits 0x02688080 (duck set), set 0x436465.
        request::Fields before = ScriptLine(0x02688080u);
        before.setRead = true;
        before.setRef = 0x436465u;
        // Asked on the script path: duck clear, set 0x438F79.
        request::Fields after = ScriptLine(0x02480000u);
        after.setRead = true;
        after.setRef = 0x438F79u;
        Check(DuckText(script, ask, after) == "0>0>0" && DuckText(script, ask, before) == "0>0>1", "duck text");
        request::Ask none;
        request::Fields unread;
        Check(DuckText(unread, none, unread) == "->->-", "duck text: nothing known");
        Tally t;
        t.Note(ask, SetStatus::ByRef, after, 0x438F79u);
        t.Note(ask, SetStatus::ByRef, before, 0x438F79u);
        t.Note(none, SetStatus::NotAsked, unread, 0);
        Check(t.lines == 3 && t.duckAsked == 2 && t.duckCleared == 2 && t.duckKept == 1, "tally: duck");
        Check(t.setSounds == 2 && t.setNone == 1 && t.setKept == 1 && t.setOther == 1 && t.setUnread == 0, "tally: set");
        Check(t.StatusText() == "by_ref:2", "tally: statuses");
        // By hash the written ref is this session's (another chunk index
        // than the recorded ref): the line that has it is kept.
        request::Fields v2 = ScriptLine(0x02480000u);
        v2.setRead = true;
        v2.setHash = 0xCAFEF00Du;
        v2.setRef = 0x01438F79u;
        request::Ask byHash;
        Apply(true, v2, 0, byHash);
        request::Fields got = ScriptLine(0x02480000u);
        got.setRead = true;
        got.setRef = 0x438F79u;
        Tally h;
        h.Note(byHash, SetStatus::ByHash, got, 0x438F79u);
        Check(h.setRecorded == 1 && h.setKept == 1 && h.setOther == 0, "tally: kept against the written ref");
        h.Note(byHash, SetStatus::BadObject, before, 0);
        Check(h.setOther == 1 && h.StatusText() == "by_hash:1|bad_object:1", "tally: bad object");
    }
}

namespace
{
    // ---- a recorded conversation asked before F4. Recording log: the line
    // already playing (7:8, 0xa393699f) adopted at 71 ms, the batch of nine
    // lines created at core_ms 56598 and started one by one; before the
    // recording the same kind of batch held the lines of the first 35 s, none
    // recorded.
    using namespace replay::speech;
    using rec::AdoptWhy;
    using rec::EarlyLine;
    using rec::EarlyLines;
    using rec::LineKey;
    using rec::StartResult;

    LineKey Key(std::uint32_t voice, std::uint32_t context, std::int32_t variation)
    {
        LineKey key;
        key.voice = voice;
        key.context = context;
        key.variation = variation;
        return key;
    }

    EarlyLine Early(std::uint64_t line, const LineKey& key, std::uint64_t createdUs)
    {
        EarlyLine early;
        early.line = line;
        early.key = key;
        early.createdUs = createdUs;
        early.requestedContext = key.context;
        early.path = 0x2FA12B0u; // direct
        return early;
    }

    void TestEarlyLines()
    {
        const std::uint32_t arthur = 0xE1B02E5Bu;
        const std::uint32_t colter = 0x7F30A0EEu;
        EarlyLines early;
        // The batch, created before F4 (contexts of the recorded batch).
        early.Created(Early(0x250220AD410ull, Key(arthur, 0x7507E3BDu, 1), 1000));
        early.Created(Early(0x250220AC910ull, Key(colter, 0x06B48718u, 1), 1000));
        early.Created(Early(0x250220AD6D0ull, Key(arthur, 0x584A2A42u, 1), 1000));
        early.Created(Early(0x250220AA550ull, Key(colter, 0xA393699Fu, 1), 1000));
        // The first line started before F4: the playing adoption takes it.
        early.Drop(0x250220AA550ull);
        Check(early.Lines().size() == 3, "early: three lines wait for their start at F4");
        EarlyLine got;
        Check(early.Take(0x250220AD410ull, Key(arthur, 0x7507E3BDu, 1), got) && got.createdUs == 1000 &&
                got.requestedContext == 0x7507E3BDu && got.path == 0x2FA12B0u,
            "early: Arthur's start during the recording is a queued line");
        Check(!early.Take(0x250220AD410ull, Key(arthur, 0x7507E3BDu, 1), got), "early: taken once");
        // Another identity at a held address: the address was reused unseen.
        Check(!early.Take(0x250220AC910ull, Key(colter, 0x642441F6u, 1), got) && early.Lines().size() == 1,
            "early: another identity at the address is not the held line, dropped");
        // A creation during the recording at a held address releases it.
        early.Created(Early(0x250220AD6D0ull, Key(arthur, 0x3D1EF3ECu, 1), 2000));
        Check(early.Lines().size() == 1 && early.Lines()[0].key.context == 0x3D1EF3ECu, "early: rule 1 replaces");
        early.Drop(0x250220AD6D0ull);
        Check(early.Lines().empty(), "early: a stop drops");
        early.Created(Early(0, Key(arthur, 1, 1), 1));
        Check(early.Lines().empty(), "early: no line, nothing held");
        for (std::uint64_t n = 0; n < EarlyLines::kMax + 5; ++n)
            early.Created(Early(0x1000 + n * 0x10, Key(colter, static_cast<std::uint32_t>(n + 1), 1), n));
        Check(early.Lines().size() == EarlyLines::kMax && early.Lines().front().createdUs == 5, "early: bounded, oldest out");
        early.Clear();
        Check(early.Lines().empty(), "early: clear");
    }

    void TestAdoptsStart()
    {
        Check(rec::AdoptsStart(StartResult::NoPending, false, true, 5), "adopt: a start the recorder held nothing for");
        Check(!rec::AdoptsStart(StartResult::Matched, false, true, 5), "adopt: a matched start is the pending's");
        Check(!rec::AdoptsStart(StartResult::Mismatch, false, true, 5), "adopt: a mismatch keeps the prompt ghost rule");
        Check(!rec::AdoptsStart(StartResult::NoPending, true, true, 5), "adopt: a line already started is not new");
        Check(!rec::AdoptsStart(StartResult::NoPending, false, false, 5), "adopt: identity unread");
        Check(!rec::AdoptsStart(StartResult::NoPending, false, true, 4), "adopt: not playing after the start");
        Check(std::strcmp(rec::AdoptWhyName(AdoptWhy::Playing), "playing") == 0 &&
                std::strcmp(rec::AdoptWhyName(AdoptWhy::Queued), "queued") == 0 &&
                std::strcmp(rec::AdoptWhyName(AdoptWhy::Unrequested), "unrequested") == 0,
            "adopt: names");
        // The matcher knows a started line (a second start there is not new).
        rec::LineMatcher<int> matcher;
        std::vector<rec::LineMatcher<int>::Drop> drops;
        std::vector<rec::LineMatcher<int>::StartedDrop> startedDrops;
        matcher.Created(0xA0, drops, startedDrops);
        matcher.AddPending(0xA0, Key(1, 2, 3), 7);
        rec::LineMatcher<int>::Entry entry;
        Check(matcher.Start(0xB0, true, Key(1, 2, 3), entry, drops) == StartResult::NoPending, "matcher: no pending");
        Check(matcher.Start(0xA0, true, Key(1, 2, 3), entry, drops) == StartResult::Matched && entry.value == 7,
            "matcher: matched");
        Check(!matcher.IsStarted(0xA0), "matcher: not started before Started");
        matcher.Started(0xA0, 4);
        Check(matcher.IsStarted(0xA0) && !matcher.IsStarted(0xB0) && !matcher.IsStarted(0), "matcher: started");
        std::size_t index = 0;
        Check(matcher.StopStarted(0xA0, index) && index == 4 && !matcher.IsStarted(0xA0), "matcher: stopped");
    }

    void TestNamedVoice()
    {
        // Arthur's conversation lines: voice 0xe1b02e5b, no voice object.
        Check(route::NamedVoiceOnPlayer(true, false, 0xE1B02E5Bu, 0, 0), "named: Arthur's line on the hidden player");
        Check(!route::NamedVoiceOnPlayer(false, false, 0xE1B02E5Bu, 0, 0), "named: the puppet keeps its pinned voice");
        Check(!route::NamedVoiceOnPlayer(true, false, 0xE1B02E5Bu, 0x12345678u, 0x22), "named: an AI line in the holder voice");
        Check(!route::NamedVoiceOnPlayer(true, false, 0xE1B02E5Bu, 0, 0x22), "named: a voice object type alone");
        Check(!route::NamedVoiceOnPlayer(true, true, 0xE1B02E5Bu, 0, 0), "named: not an animal");
        Check(!route::NamedVoiceOnPlayer(true, false, 0, 0, 0), "named: no voice recorded");
    }

    void TestPlainAsk()
    {
        using namespace conversation;
        request::Fields script = ScriptLine(0x02480000u);
        script.setRead = true;
        script.setHash = 0x1234ABCDu;
        script.setRef = 0x438F79u;
        const request::Ask plain = request::BuildAsk(2, script, true, true);
        request::Ask live = plain;
        Apply(true, script, 0, live);
        Check(request::SameAsk(plain, plain), "plain: same ask");
        Check(!request::SameAsk(live, plain), "plain: the conversation fields change the ask");
        request::Ask off = plain;
        Apply(false, script, 0, off);
        Check(request::SameAsk(off, plain), "plain: key off, no retry");
        request::Ask a;
        a.Set(0xDC, 1);
        a.Set(0xE0, 2);
        request::Ask b;
        b.Set(0xE0, 2);
        b.Set(0xDC, 1);
        Check(request::SameAsk(a, b), "plain: write order does not count");
        b.Set(0xE0, 3);
        Check(!request::SameAsk(a, b), "plain: a value differs");
        request::Ask c = a;
        c.SetByte(scriptpath::kParamsDuckByte, 0);
        Check(!request::SameAsk(a, c), "plain: the duck byte counts");
    }
}

int RunSpeechConversationTests()
{
    TestEarlyLines();
    TestAdoptsStart();
    TestNamedVoice();
    TestPlainAsk();
    TestDataBlock();
    TestChildRefs();
    TestSounds1149();
    TestApply();
    TestResolve();
    TestSetTable();
    TestTally();
    if (g_speechConversationFailures == 0)
        std::printf("speech conversation (sound set and duck byte of the conversation lines) tests passed\n");
    return g_speechConversationFailures;
}
