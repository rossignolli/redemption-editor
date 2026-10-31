// The site ids (game_sig_site.h) and the clip's map of them (clip_sites.h):
// an address and its id both ways, the range that names it, the 1491.50 map
// as declared, and a clip saved on that build and read on another one
// (every signature moved) keeping its fx sites, sound families, wrappers,
// static audio objects and vtables.
#include "clip_file.h"
#include "clip_sites.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
    namespace sig = replay::sig;
    namespace clipsites = replay::clipsites;
    namespace soundowner = replay::soundowner;
    namespace soundgeneric = replay::soundgeneric;
    namespace census = replay::census;

    int g_siteFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_siteFailures;
    }

    std::uint32_t PatternSize(const sig::Signature& s)
    {
        sig::Pattern p;
        return sig::Parse(s.pattern, p) ? static_cast<std::uint32_t>(p.size) : 0u;
    }

    void TestIds()
    {
        Check(sig::NameHash("") == 0x811C9DC5u, "name hash: FNV-1a 32 of the empty name");
        Check(sig::NameHash("a") == 0xE40C292Cu, "name hash: FNV-1a 32 of \"a\"");
        Check(sig::NameHash(nullptr) != 0, "name hash: never 0");
        const sig::SiteId id{0x12345678u, 0x9Au};
        const sig::SiteId back = sig::Unpack(sig::Pack(id));
        Check(sig::Pack(id) == 0x123456780000009Aull && back.name == id.name && back.offset == id.offset,
            "pack: the name in the high dword, both ways");
    }

    void TestMap()
    {
        sig::SiteMap map;
        map.Add(1, 0x1000, 0x1FFF); // a function
        map.Add(2, 0x1200, 0x1205); // a call inside it (E8 + 4, its return one past)
        map.Add(3, 0x5000, 0x5000); // a static object
        map.Add(4, 0, 0x10);        // not found: left out
        Check(map.Ranges().size() == 3, "map: a range not found is left out");
        sig::SiteId id = map.SiteOf(0x1205);
        Check(id.name == 2 && id.offset == 5, "map: a call's return address is named by the call");
        id = map.SiteOf(0x1206);
        Check(id.name == 1 && id.offset == 0x206, "map: past the call, the function");
        id = map.SiteOf(0x5000);
        Check(id.name == 3 && id.offset == 0, "map: a static object");
        id = map.SiteOf(0x5008);
        Check(id.name == 0 && id.offset == 0x5008, "map: an address no range holds is raw");
        id = map.SiteOf(0);
        Check(id.name == 0 && id.offset == 0, "map: 0 stays none");
        Check(map.RvaOf({2, 5}, false) == 0x1205 && map.RvaOf({1, 0x206}, false) == 0x1206, "map: back to the RVA");
        Check(map.RvaOf({1, 0x5000}, false) == sig::kForeignRva, "map: an offset past a shorter function is foreign");
        Check(map.RvaOf({9, 0}, true) == sig::kForeignRva, "map: a name this build has not is foreign");
        Check(map.RvaOf({0, 0x5008}, true) == 0x5008, "map: a raw id on its own build keeps its RVA");
        Check(map.RvaOf({0, 0x5008}, false) == sig::kForeignRva, "map: a raw id on another build is foreign");
        Check(map.RvaOf({0, 0}, false) == 0, "map: none stays 0 on any build");
    }

    // The 1491.50 map is the signatures as declared: unique names, each range
    // where its signature (and tail) says, every address back to itself.
    void TestKnownMap()
    {
        const sig::SiteMap& known = clipsites::KnownSiteMap();
        const std::size_t expected = sizeof(clipsites::kSpans) / sizeof(clipsites::kSpans[0]) +
            soundowner::kCallerRangeCount + census::kImageAudioCount + 11; // 11 static objects' vtables
        Check(known.Ranges().size() == expected, "known map: every declared range is there");
        std::set<std::uint32_t> names;
        for (const sig::SiteRange& r : known.Ranges())
            names.insert(r.name);
        Check(names.size() == known.Ranges().size(), "known map: the name hashes are unique");
        bool roundTrip = true;
        for (const sig::SiteRange& r : known.Ranges())
            for (const std::uint32_t rva : {r.begin, r.begin + (r.last - r.begin) / 2, r.last})
                roundTrip = roundTrip && known.RvaOf(known.SiteOf(rva), true) == rva;
        Check(roundTrip, "known map: every address in a range comes back to itself");
        const sig::SiteId muzzle = known.SiteOf(replay::weaponfx::kWeaponMuzzleCall.rva + 5);
        Check(muzzle.name == sig::NameHash(replay::weaponfx::kWeaponMuzzleCall.name) && muzzle.offset == 5,
            "known map: the muzzle site is its call + 5");
        const soundowner::CallerRanges callers = soundowner::KnownCallerRanges();
        bool sameRanges = true;
        for (std::size_t n = 0; n < soundowner::kCallerRangeCount; ++n)
        {
            const sig::SiteId first = known.SiteOf(callers.range[n].begin);
            sameRanges = sameRanges && first.name == sig::NameHash(soundowner::kCallerFunctions[n].start->name) &&
                first.offset == 0 && known.SiteOf(callers.range[n].end - 1).name != 0;
        }
        Check(sameRanges, "known map: the sound owner's functions are the table's ranges");
        const sig::SiteId audio = known.SiteOf(census::kImageAudio01.rva);
        Check(audio.name == sig::NameHash(census::kImageAudio01.name) && audio.offset == 0,
            "known map: a static audio object by its signature");
        const sig::SiteId vtable = known.SiteOf(clipsites::kPedAudioA.rva);
        Check(vtable.name == sig::NameHash(clipsites::kPedAudioA.name) && vtable.offset == 0,
            "known map: a ped audio vtable by its signature");
    }

    // Another build: every signature moves, each function by its own amount
    // (a permutation of 64 KB slots), its sites with it; a run of two
    // functions (the wrappers, the creators) stays together.
    struct OtherBuild
    {
        std::map<const sig::Signature*, std::int64_t> shift;

        static const sig::Signature* Home(const sig::Signature& s)
        {
            const sig::Signature* home = &s;
            while (home->via == sig::Via::Offset && home->parent)
                home = home->parent;
            return home;
        }

        OtherBuild()
        {
            std::vector<const sig::Signature*> homes;
            const auto note = [&](const sig::Signature* s) {
                if (s && std::find(homes.begin(), homes.end(), Home(*s)) == homes.end())
                    homes.push_back(Home(*s));
            };
            for (const clipsites::Span& span : clipsites::kSpans)
            {
                note(span.sig);
                note(span.tail);
            }
            for (const soundowner::CallerFunction& f : soundowner::kCallerFunctions)
            {
                note(f.start);
                note(f.tail);
            }
            for (const census::ImageAudioSignatures& s : census::kImageAudioSignatures)
            {
                note(s.object);
                note(s.vtable);
            }
            std::sort(homes.begin(), homes.end(), [](const sig::Signature* a, const sig::Signature* b) {
                return sig::NameHash(a->name) < sig::NameHash(b->name);
            });
            for (std::size_t n = 0; n < homes.size(); ++n)
                shift[homes[n]] = static_cast<std::int64_t>(0x01000000 + n * 0x10000) - homes[n]->rva;
            for (const clipsites::Span& span : clipsites::kSpans)
                if (span.tail && Home(*span.tail) != Home(*span.sig))
                    shift[Home(*span.tail)] = shift[Home(*span.sig)];
        }

        std::uint32_t RvaOf(const sig::Signature& s) const
        {
            return static_cast<std::uint32_t>(s.rva + shift.at(Home(s)));
        }

        sig::SiteMap Map() const
        {
            return clipsites::BuildSiteMap([this](const sig::Signature& s) { return RvaOf(s); });
        }

        soundowner::CallerRanges CallerRanges() const
        {
            soundowner::CallerRanges ranges;
            for (std::size_t n = 0; n < soundowner::kCallerRangeCount; ++n)
            {
                const soundowner::CallerFunction& f = soundowner::kCallerFunctions[n];
                ranges.range[n] = {RvaOf(*f.start), RvaOf(*f.tail) + PatternSize(*f.tail), f.family};
            }
            return ranges;
        }

        soundgeneric::WrapperCode WrapperCode() const
        {
            const auto end = [this](const sig::Signature& tail) { return RvaOf(tail) + PatternSize(tail); };
            soundgeneric::WrapperCode code;
            code.creators = {RvaOf(soundgeneric::kCreatorEntries), end(soundgeneric::kCreatorATail)};
            code.wrappers = {RvaOf(soundgeneric::kWrappers), end(soundgeneric::kQueueDrainTail)};
            code.otherDrain = {RvaOf(soundgeneric::kOtherDrain), end(soundgeneric::kOtherDrainTail)};
            code.drain = {RvaOf(soundgeneric::kQueueDrain), end(soundgeneric::kQueueDrainTail)};
            return code;
        }
    };

    // 1.5 of the plan: what the playback decides from a recorded address is
    // the same when the clip is read on another build.
    void TestOtherBuild()
    {
        const sig::SiteMap& known = clipsites::KnownSiteMap();
        const OtherBuild other;
        const sig::SiteMap moved = other.Map();
        Check(moved.Ranges().size() == known.Ranges().size(), "other build: every range found");
        const auto carry = [&](std::uint32_t rva) { return moved.RvaOf(known.SiteOf(rva), false); };

        // The fx, decal, blood and speech call sites: the same site there.
        bool sites = true;
        for (const clipsites::Span& span : clipsites::kSpans)
            if (!span.tail && std::strlen(span.sig->pattern) && span.sig->pattern[0] == 'E')
                sites = sites && carry(span.sig->rva + 5) == other.RvaOf(*span.sig) + 5;
        Check(sites, "other build: every call site lands on that build's site (call + 5)");

        // The sound owner's families of callers in, at the edges of and
        // outside the functions.
        const soundowner::CallerRanges knownRanges = soundowner::KnownCallerRanges();
        const soundowner::CallerRanges movedRanges = other.CallerRanges();
        bool families = true;
        std::size_t named = 0;
        for (const soundowner::CallerRange& r : knownRanges.range)
        {
            for (const std::uint32_t rva : {r.begin, r.begin + 1, (r.begin + r.end) / 2, r.end - 1, r.end + 0x40000})
            {
                soundowner::Family a = soundowner::Family::World;
                soundowner::Family b = soundowner::Family::World;
                const bool inA = soundowner::FamilyOfCaller(rva, knownRanges, a);
                const bool inB = soundowner::FamilyOfCaller(carry(rva), movedRanges, b);
                families = families && inA == inB && a == b;
                named += inA ? 1 : 0;
            }
        }
        Check(families && named == 4 * soundowner::kCallerRangeCount,
            "other build: a caller's sound family is the same (FamilyOfCaller)");

        // The audio entity's wrappers: a caller inside them stays inside.
        const soundgeneric::WrapperCode knownCode = soundgeneric::KnownWrapperCode();
        const soundgeneric::WrapperCode movedCode = other.WrapperCode();
        bool wrappers = true;
        for (const soundgeneric::CodeRange& r : {knownCode.creators, knownCode.wrappers, knownCode.otherDrain, knownCode.drain})
            for (const std::uint32_t rva : {r.begin, (r.begin + r.end) / 2, r.end - 1})
                wrappers = wrappers && soundgeneric::InAudioEntityWrappers(rva, knownCode) &&
                    soundgeneric::InAudioEntityWrappers(carry(rva), movedCode);
        Check(wrappers, "other build: a caller in the audio entity's wrappers stays in them");

        // The static audio objects with their vtables, and the audio entity
        // vtables.
        census::ImageAudioTable movedTable{};
        for (std::size_t n = 0; n < census::kImageAudioCount; ++n)
        {
            const census::ImageAudioSignatures& s = census::kImageAudioSignatures[n];
            movedTable.entry[n] = {other.RvaOf(*s.object), s.vtable ? other.RvaOf(*s.vtable) : 0u};
        }
        const census::ImageAudioTable knownTable = census::KnownImageAudioTable();
        bool objects = true;
        for (const census::ImageAudio& entry : knownTable.entry)
        {
            std::uint32_t vtable = 0;
            objects = objects && census::KnownImageAudio(carry(entry.rva), vtable, movedTable) &&
                vtable == (entry.vtableRva ? carry(entry.vtableRva) : 0u);
        }
        Check(objects, "other build: a static audio object is that build's, with its vtable");
        bool vtables = true;
        for (const sig::Signature* s : {&clipsites::kPedAudioA, &clipsites::kPedAudioB, &clipsites::kPedAudioC,
                 &clipsites::kVehicleAudioA, &clipsites::kVehicleAudioB, &clipsites::kEntityAudioA})
            vtables = vtables && carry(s->rva) == other.RvaOf(*s);
        Check(vtables, "other build: an audio entity vtable is that build's");
        Check(carry(0x5000) == sig::kForeignRva && carry(0) == 0,
            "other build: an address no signature names is foreign, none stays none");
    }

    // A clip saved on the 1491.50 exe and read on the other build: the fields
    // the playback compares come back as that build's addresses.
    void TestClipOnOtherBuild()
    {
        namespace clipfile = replay::clipfile;
        namespace entity = replay::entity;
        constexpr std::uintptr_t kBase = 0x140000000ull;
        constexpr std::size_t kImageSize = 0x10000000;
        const OtherBuild other;
        const std::uint32_t muzzle = replay::weaponfx::kWeaponMuzzleCall.rva + 5;
        const std::uint32_t step = replay::enginesig::kAudioStepSound.rva + 0x100;
        const std::uint32_t object = census::kImageAudio04.rva;

        replay::Recording rec;
        entity::FxEvent fx;
        fx.timeMs = 10;
        fx.callerSite = muzzle;
        rec.fx.Add(fx);
        entity::BloodEvent blood;
        blood.timeMs = 20;
        blood.caller = replay::blood::kBloodEvolveCall.rva + 5;
        rec.blood.Add(blood);
        entity::SoundEvent sound;
        sound.timeMs = 30;
        sound.globalAudio = kBase + object;
        rec.sounds.Add(sound);
        replay::soundgeneric::CreatePacket create;
        create.timeMs = 40;
        create.index = 0;
        create.caller = step;
        create.audioKnown = true;
        create.audioBase = census::AudioBase::Image;
        create.audioOffset = static_cast<std::int32_t>(object);
        create.vtableRva = census::kImageAudio04Vtable.rva;
        rec.soundGeneric.nextIndex = 1;
        rec.soundGeneric.creates.push_back(create);
        replay::soundgeneric::CreatePacket ped = create;
        ped.timeMs = 41;
        ped.index = 1;
        ped.caller = 0x123458; // no signature names it: raw
        ped.audioBase = census::AudioBase::PedHolder;
        ped.audioOffset = 0x10;
        ped.vtableRva = clipsites::kPedAudioA.rva;
        rec.soundGeneric.nextIndex = 2;
        rec.soundGeneric.creates.push_back(ped);

        std::vector<replay::pose::PoseRecordExport> poses;
        replay::camera::Track camera;
        std::vector<std::uint8_t> bytes;
        std::string error;
        clipfile::SetSiteMap(clipsites::KnownSiteMap());
        clipfile::SetBuildIdentity(0x65EEC92Eu, 0x7397600u); // saved on 1491.50
        Check(clipfile::Encode(rec, poses, camera, 1000, kBase, kImageSize, bytes, error), "clip on another build: encode");

        // The same build: every field as it was.
        replay::Recording same;
        std::vector<replay::pose::PoseRecordExport> samePoses;
        replay::camera::Track sameCamera;
        std::uint64_t durationMs = 0;
        Check(clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, same, samePoses, sameCamera, durationMs, error),
            ("clip on the same build: decode " + error).c_str());
        Check(same.fx.Events().size() == 1 && same.fx.Events()[0].callerSite == muzzle &&
                same.blood.Events().size() == 1 && same.blood.Events()[0].caller == blood.caller &&
                same.sounds.Events().size() == 1 && same.sounds.Events()[0].globalAudio == kBase + object &&
                same.soundGeneric.creates.size() == 2 && same.soundGeneric.creates[0].caller == step &&
                same.soundGeneric.creates[0].audioOffset == static_cast<std::int32_t>(object) &&
                same.soundGeneric.creates[0].vtableRva == census::kImageAudio04Vtable.rva &&
                same.soundGeneric.creates[1].caller == 0x123458 && same.soundGeneric.creates[1].audioOffset == 0x10 &&
                same.soundGeneric.creates[1].vtableRva == clipsites::kPedAudioA.rva,
            "clip on the same build: the addresses come back as they were");

        // The other build (its own PE identity: META no longer refuses it).
        clipfile::SetSiteMap(other.Map());
        clipfile::SetBuildIdentity(0x70000000u, 0x7400000u);
        replay::Recording moved;
        std::vector<replay::pose::PoseRecordExport> movedPoses;
        replay::camera::Track movedCamera;
        const bool decoded =
            clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, moved, movedPoses, movedCamera, durationMs, error);
        clipfile::SetSiteMap(clipsites::KnownSiteMap());
        clipfile::SetBuildIdentity(0, 0);
        Check(decoded, ("clip on another build: decode " + error).c_str());
        if (!decoded || moved.soundGeneric.creates.size() != 2 || moved.fx.Events().empty() || moved.blood.Events().empty() ||
            moved.sounds.Events().empty())
        {
            Check(false, "clip on another build: the events are there");
            return;
        }
        Check(moved.fx.Events()[0].callerSite == other.RvaOf(replay::weaponfx::kWeaponMuzzleCall) + 5,
            "clip on another build: the fx site is that build's muzzle site");
        Check(moved.blood.Events()[0].caller == other.RvaOf(replay::blood::kBloodEvolveCall) + 5,
            "clip on another build: the blood caller is that build's evolve");
        Check(moved.sounds.Events()[0].globalAudio == kBase + other.RvaOf(census::kImageAudio04),
            "clip on another build: the static audio is that build's object");
        const replay::soundgeneric::CreatePacket& a = moved.soundGeneric.creates[0];
        soundowner::Family family = soundowner::Family::World;
        Check(soundowner::FamilyOfCaller(a.caller, other.CallerRanges(), family) && family == soundowner::Family::Footstep,
            "clip on another build: the generic sound's caller is still the step's (footstep family)");
        Check(a.audioOffset == static_cast<std::int32_t>(other.RvaOf(census::kImageAudio04)) &&
                a.vtableRva == other.RvaOf(census::kImageAudio04Vtable) && a.audioKnown,
            "clip on another build: the generic sound's static object and vtable are that build's");
        const replay::soundgeneric::CreatePacket& b = moved.soundGeneric.creates[1];
        Check(b.vtableRva == other.RvaOf(clipsites::kPedAudioA) && b.audioOffset == 0x10,
            "clip on another build: a ped audio vtable is that build's, its offset kept");
        Check(b.caller == sig::kForeignRva, "clip on another build: a raw caller (no signature names it) is foreign");
    }

    // `bytes` with the section `name` written as an older build did: version
    // `version`, the u64 site id at each of `ids` (payload offsets) narrowed to
    // the u32 RVA it held, its hash recomputed.
    std::vector<std::uint8_t> WithOldSection(const std::vector<std::uint8_t>& bytes, const char* name, std::uint16_t version,
        std::vector<std::size_t> ids, std::uint32_t rva)
    {
        std::uint32_t tag = 0;
        for (const replay::clipfile::SectionInfo& info : replay::clipfile::Sections())
            if (std::strcmp(info.name, name) == 0)
                tag = info.tag;
        std::size_t at = 8 + 4 + 8;
        while (at + 14 <= bytes.size())
        {
            std::uint32_t found = 0;
            std::uint64_t length = 0;
            std::memcpy(&found, bytes.data() + at, 4);
            std::memcpy(&length, bytes.data() + at + 6, 8);
            if (found != tag)
            {
                at += 14 + static_cast<std::size_t>(length) + 8;
                continue;
            }
            std::vector<std::uint8_t> payload(bytes.begin() + static_cast<std::ptrdiff_t>(at + 14),
                bytes.begin() + static_cast<std::ptrdiff_t>(at + 14 + length));
            std::sort(ids.rbegin(), ids.rend());
            for (const std::size_t id : ids)
            {
                payload.erase(payload.begin() + static_cast<std::ptrdiff_t>(id), payload.begin() + static_cast<std::ptrdiff_t>(id + 8));
                payload.insert(payload.begin() + static_cast<std::ptrdiff_t>(id), reinterpret_cast<const std::uint8_t*>(&rva),
                    reinterpret_cast<const std::uint8_t*>(&rva) + 4);
            }
            std::vector<std::uint8_t> out(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(at + 4));
            const std::uint64_t newLength = payload.size();
            std::uint64_t hash = 0xCBF29CE484222325ull;
            for (const std::uint8_t b : payload)
                hash = (hash ^ b) * 0x100000001B3ull;
            out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&version), reinterpret_cast<const std::uint8_t*>(&version) + 2);
            out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&newLength), reinterpret_cast<const std::uint8_t*>(&newLength) + 8);
            out.insert(out.end(), payload.begin(), payload.end());
            out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&hash), reinterpret_cast<const std::uint8_t*>(&hash) + 8);
            out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at + 14 + length + 8), bytes.end());
            return out;
        }
        return {};
    }

    // A clip written before the site ids holds 1491.50 RVAs: read on that exe
    // it is unchanged, read on another build its named addresses move.
    void TestOldClip()
    {
        namespace clipfile = replay::clipfile;
        constexpr std::uintptr_t kBase = 0x140000000ull;
        constexpr std::size_t kImageSize = 0x10000000;
        const std::uint32_t muzzle = replay::weaponfx::kWeaponMuzzleCall.rva + 5;
        replay::Recording rec;
        replay::entity::FxEvent fx;
        fx.timeMs = 10;
        fx.callerSite = muzzle;
        rec.fx.Add(fx);
        std::vector<replay::pose::PoseRecordExport> poses;
        replay::camera::Track camera;
        std::vector<std::uint8_t> bytes;
        std::string error;
        clipfile::SetSiteMap(clipsites::KnownSiteMap());
        Check(clipfile::Encode(rec, poses, camera, 1000, kBase, kImageSize, bytes, error), "old clip: encode");
        // FXEV v1: the site (after the count 8, time 4, effect 4, asset 4) a u32.
        const std::vector<std::uint8_t> old = WithOldSection(bytes, "fx", 1, {8 + 12}, muzzle);
        Check(!old.empty() && old.size() + 4 == bytes.size(), "old clip: an FXEV v1 payload built");
        replay::Recording onKnown;
        std::vector<replay::pose::PoseRecordExport> knownPoses;
        replay::camera::Track knownCamera;
        std::uint64_t durationMs = 0;
        Check(clipfile::Decode(old.data(), old.size(), kBase, kImageSize, onKnown, knownPoses, knownCamera, durationMs, error) &&
                onKnown.fx.Events().size() == 1 && onKnown.fx.Events()[0].callerSite == muzzle,
            "old clip: FXEV v1 on the 1491.50 exe keeps its RVA");
        const OtherBuild other;
        clipfile::SetSiteMap(other.Map());
        replay::Recording onOther;
        const bool decoded =
            clipfile::Decode(old.data(), old.size(), kBase, kImageSize, onOther, knownPoses, knownCamera, durationMs, error);
        clipfile::SetSiteMap(clipsites::KnownSiteMap());
        Check(decoded && onOther.fx.Events().size() == 1 &&
                onOther.fx.Events()[0].callerSite == other.RvaOf(replay::weaponfx::kWeaponMuzzleCall) + 5,
            "old clip: FXEV v1 on another build is that build's muzzle site");

        const sig::SiteMap& known = clipsites::KnownSiteMap();
        const sig::SiteMap moved = other.Map();
        const std::uint32_t site = replay::events::kFireAreaFxCall.rva + 5;
        Check(known.RvaOf(known.SiteOf(site), true) == site && known.RvaOf(known.SiteOf(0x123458), true) == 0x123458,
            "old clip: on the 1491.50 exe every RVA stays");
        Check(moved.RvaOf(known.SiteOf(site), true) == other.RvaOf(replay::events::kFireAreaFxCall) + 5,
            "old clip: on another build a named RVA moves with its signature");
    }

    // The names of the clip sites are part of the clip format: a clip keeps
    // the hash of the name (game_sig_site.h), so a renamed or removed site
    // makes the saved clips point nowhere. This list is frozen: a new site is
    // added to it, an existing one is never renamed (keep the old name in the
    // signature) nor removed while clips may hold it.
    const char* const kFrozenSiteNames[] = {
        "audio_foot_helper_a",
        "audio_foot_helper_b",
        "audio_melee_body_impact",
        "audio_melee_impact",
        "audio_persistent_create",
        "audio_step_create",
        "audio_step_layers",
        "audio_step_other",
        "audio_step_sound",
        "blood_bullet_call",
        "blood_evolve_call",
        "blood_pack_call",
        "census_image_audio_01",
        "census_image_audio_01_vtable",
        "census_image_audio_02",
        "census_image_audio_02_vtable",
        "census_image_audio_03",
        "census_image_audio_03_vtable",
        "census_image_audio_04",
        "census_image_audio_04_vtable",
        "census_image_audio_05",
        "census_image_audio_06",
        "census_image_audio_06_vtable",
        "census_image_audio_07",
        "census_image_audio_07_vtable",
        "census_image_audio_08",
        "census_image_audio_08_vtable",
        "census_image_audio_09",
        "census_image_audio_10",
        "census_image_audio_10_vtable",
        "census_image_audio_11",
        "census_image_audio_12",
        "census_image_audio_12_vtable",
        "census_image_audio_13",
        "census_image_audio_13_vtable",
        "clip_entity_audio_a_vtable",
        "clip_ped_audio_a_vtable",
        "clip_ped_audio_b_vtable",
        "clip_ped_audio_c_vtable",
        "clip_vehicle_audio_a_vtable",
        "clip_vehicle_audio_b_vtable",
        "decal_generic_call",
        "events_fire_area_fx_call",
        "events_fire_entity_flames_fx_call",
        "events_fire_entity_smoke_fx_call",
        "events_fire_limb_flames_fx_call",
        "events_fire_smoulder_fx_call",
        "events_weapon_fx_a_call",
        "events_weapon_fx_b_call",
        "foot_event_movement_update",
        "fx_body_hit_call",
        "fx_bullet_impact_call",
        "fx_camera_ambient_call",
        "fx_foot_vfx_call",
        "fx_hoof_dust_registered_call",
        "fx_registry_restart_call",
        "fx_script_loop_keep_alive_call",
        "fx_script_loop_start_call",
        "generic_creator_entries",
        "generic_other_drain",
        "generic_queue_drain",
        "generic_wrappers",
        "mount_audio_caller_07",
        "speech_layer_sound",
        "speech_layer_sound_call",
        "speech_wave_sound_call",
        "weapon_fx_muzzle_effect_call",
        "weapon_fx_shell_registered_call",
        "weather_entity_vtable",
    };

    void TestFrozenSiteNames()
    {
        std::set<std::string> current;
        const auto note = [&](const sig::Signature* s) {
            if (s)
                current.insert(s->name);
        };
        for (const clipsites::Span& span : clipsites::kSpans)
            note(span.sig);
        for (const soundowner::CallerFunction& f : soundowner::kCallerFunctions)
            note(f.start);
        for (const census::ImageAudioSignatures& s : census::kImageAudioSignatures)
        {
            note(s.object);
            note(s.vtable);
        }
        std::set<std::string> frozen(std::begin(kFrozenSiteNames), std::end(kFrozenSiteNames));
        for (const std::string& name : frozen)
            if (!current.count(name))
                std::fprintf(stderr, "  site name gone (clips hold it): %s\n", name.c_str());
        for (const std::string& name : current)
            if (!frozen.count(name))
                std::fprintf(stderr, "  site name new (add it to kFrozenSiteNames): %s\n", name.c_str());
        Check(current == frozen, "frozen site names: every clip site name kept, every new one listed");
    }
}

int RunGameSigSiteTests()
{
    TestIds();
    TestMap();
    TestKnownMap();
    TestOtherBuild();
    TestClipOnOtherBuild();
    TestOldClip();
    TestFrozenSiteNames();
    if (g_siteFailures == 0)
        std::printf("Game site id tests passed.\n");
    return g_siteFailures;
}
