// The whole metaped look (metaped_state.h), the dressing lookahead
// (appearance_track.h DressAt), the skeleton change re-armed in its tick
// (segment_switch.h), the camera's cut bits (camera_cut.h) and the
// appearance_not_recorded anomaly of the QA kit.
#include "appearance_track.h"
#include "camera_cut.h"
#include "metaped_state.h"
#include "qa_health.h"
#pragma warning(push)
#pragma warning(disable : 4324) // inc/types.h Vector3 alignas padding
#include "replay_puppets.h"
#pragma warning(pop)
#include "prefix_hold.h"
#include "segment_switch.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    using replay::metaped::Expression;

    void TestStateAt()
    {
        using namespace replay::metaped;
        Track track;
        track.id = 0x00540072u;
        State a;
        a.timeMs = 4325;
        a.source = Source::Segment;
        State b;
        b.timeMs = 39618;
        b.source = Source::Segment;
        track.states = {a, b};
        Wanted w = StateAt(&track, 4277, 2000);
        Check(w.state == &track.states[0] && w.lookaheadMs == 48 && w.key == 1,
            "metaped: before the first state, the first within the lookahead (84:114, 48 ms)");
        w = StateAt(&track, 1000, 2000);
        Check(!w.state, "metaped: nothing within the lookahead");
        w = StateAt(&track, 20000, 2000);
        Check(w.state == &track.states[0] && w.key == 1 && w.lookaheadMs == 0, "metaped: the latest at or before");
        w = StateAt(&track, 39618, 2000);
        Check(w.state == &track.states[1] && w.key == 2, "metaped: a state at the playhead is in force");
        Check(!StateAt(nullptr, 0, 2000).state, "metaped: no track");
    }

    void TestExpressions()
    {
        using namespace replay::metaped;
        const std::vector<Expression> first = {{0x0010, 0.5f}, {0x0020, 0.1f}, {0x0030, -0.2f}, {0x0040, 0.9f}};
        const std::vector<Expression> second = {{0x0010, 0.5f}, {0x0020, 0.4f}, {0x0040, 0.9f}, {0x0050, 1.0f}};
        std::vector<Expression> stable;
        StableExpressions(first, second, 1e-4f, stable);
        Check(stable.size() == 2 && stable[0].id == 0x0010 && stable[1].id == 0x0040,
            "metaped: only the expressions equal in both reads stay (the animated face leaves)");
        const std::vector<Expression> puppet = {{0x0010, 0.5f}, {0x0040, 0.1f}};
        const Diff d = Compare(first, puppet, 1e-3f);
        Check(d.recorded == 4 && d.present == 2 && d.differ == 1 && d.missing == 2 && d.worstId == 0x0040 && !d.Exact(),
            "metaped: recorded against the puppet");
        std::vector<Expression> set;
        ToApply(first, puppet, 1e-4f, set);
        Check(set.size() == 3 && set[0].id == 0x0020 && set[1].id == 0x0030 && set[2].id == 0x0040,
            "metaped: only what the puppet does not have is set");
        ToApply(first, {}, 1e-4f, set);
        Check(set.size() == 4, "metaped: an unread puppet gets all of them");
        Check(ExpressionsPrint(first) == ExpressionsPrint(first) && ExpressionsPrint(first) != ExpressionsPrint(second),
            "metaped: print");
        std::vector<Expression> unsorted = {{0x30, 1.0f}, {0x10, 2.0f}};
        SortById(unsorted);
        Check(unsorted[0].id == 0x10 && FindExpression(unsorted, 0x30) && !FindExpression(unsorted, 0x20),
            "metaped: sorted lookup");
    }

    void PutDof(std::vector<std::uint8_t>& table, std::uint16_t offset, std::uint16_t id, std::uint8_t track)
    {
        const std::size_t at = table.size();
        table.resize(at + replay::metaped::kDofBytes, 0);
        std::memcpy(&table[at], &offset, 2);
        std::memcpy(&table[at + 2], &id, 2);
        table[at + 4] = track;
    }

    void TestParse()
    {
        using namespace replay::metaped;
        // The engine's frame data: sorted by (track << 16 | id); bone tracks
        // around the expression track 0x40 (layout in metaped_state.h).
        std::vector<std::uint8_t> table;
        PutDof(table, 0, 0x0001, 0x00);   // a bone translation
        PutDof(table, 16, 0x0002, 0x01);  // a bone rotation
        PutDof(table, 32, 0x1111, 0x40);  // expression
        PutDof(table, 36, 0x2222, 0x40);  // expression not set
        PutDof(table, 40, 0x3333, 0x40);  // expression
        PutDof(table, 44, 0x0005, 0x41);  // another track
        const std::uint32_t count = static_cast<std::uint32_t>(table.size() / kDofBytes);
        Check(ExpressionBufferBytes(table.data(), count) == 44, "parse: the buffer the expressions reach");
        std::vector<std::uint8_t> buffer(48, 0);
        const float v1 = 0.75f, v3 = -1.0f;
        const std::uint32_t notSet = kNotSet;
        std::memcpy(&buffer[32], &v1, 4);
        std::memcpy(&buffer[36], &notSet, 4);
        std::memcpy(&buffer[40], &v3, 4);
        std::vector<Expression> out;
        std::uint32_t skipped = 0;
        Check(ParseExpressions(table.data(), count, buffer.data(), 44, out, skipped) && out.size() == 2 &&
                out[0].id == 0x1111 && out[0].value == 0.75f && out[1].id == 0x3333 && out[1].value == -1.0f && skipped == 1,
            "parse: the expression track's values, the unset one skipped");
        Check(!ParseExpressions(table.data(), count, buffer.data(), 40, out, skipped), "parse: an offset past the buffer refused");
        std::vector<std::uint8_t> swapped;
        PutDof(swapped, 0, 0x3333, 0x40);
        PutDof(swapped, 4, 0x1111, 0x40);
        Check(!ParseExpressions(swapped.data(), 2, buffer.data(), buffer.size(), out, skipped),
            "parse: a table out of order is not the layout");
        Check(!ParseExpressions(table.data(), kMaxDofs + 1, buffer.data(), buffer.size(), out, skipped), "parse: count bound");
    }

    void TestNotRecorded()
    {
        using namespace replay::metaped;
        NotRecordedFacts f;
        Check(NotRecordedWhy(f) && std::string(NotRecordedWhy(f)) == "no_layers", "qa rule: nothing to wear");
        f.layers = 11;
        Check(!NotRecordedWhy(f), "qa rule: layers, a clip without the metaped section (old clip) is fine");
        f.clipHasMetaped = true;
        Check(std::string(NotRecordedWhy(f)) == "no_metaped_state", "qa rule: a new clip without the look of this body");
        f.entityHasMetaped = true;
        Check(!NotRecordedWhy(f), "qa rule: layers and look");
        f.layers = 0;
        f.outfit = 0x13C4805Au;
        Check(!NotRecordedWhy(f), "qa rule: an outfit alone dresses");
        Check(ScaleNotDefault(0.9f) && !ScaleNotDefault(1.0f) && !ScaleValid(0.0f) && !ScaleValid(9.0f), "metaped: scale");
    }

    void TestDressAt()
    {
        using namespace replay;
        entity::ReplayId id{84, 114};
        entity::Appearance create; // 705 bones, 0 layers: the engine still built the ped
        std::vector<entity::PoseSegment> segments(2);
        segments[0].entity = id;
        segments[0].startMs = 4325;
        segments[0].boneCount = 959;
        segments[0].appearance.assets.resize(11);
        segments[1].entity = id;
        segments[1].startMs = 39618;
        segments[1].boneCount = 942;
        segments[1].appearance.assets.resize(10);
        DressAppearance d = DressAt(nullptr, segments, id, nullptr, create, 4277, kDressLookaheadMs);
        Check(!d.Empty() && d.appearance == &segments[0].appearance && d.lookaheadMs == 48 &&
                std::string(d.from) == "lookahead_segment",
            "dress: 84:114 is dressed with the segment 48 ms later, not left empty");
        d = DressAt(nullptr, segments, id, nullptr, create, 1000, kDressLookaheadMs);
        Check(d.Empty() && std::string(d.from) == "none", "dress: no layers within the lookahead");
        d = DressAt(nullptr, segments, id, &segments[0], create, 20000, kDressLookaheadMs);
        Check(d.appearance == &segments[0].appearance && d.lookaheadMs == 0 && std::string(d.from) == "segment",
            "dress: the segment in force");
        entity::Appearance dressed;
        dressed.assets.resize(9);
        d = DressAt(nullptr, segments, id, nullptr, dressed, 4277, kDressLookaheadMs);
        Check(d.appearance == &dressed && std::string(d.from) == "create", "dress: a create packet with layers is worn");
        entity::ReplayId other{17, 201};
        d = DressAt(nullptr, segments, other, nullptr, create, 4277, kDressLookaheadMs);
        Check(d.Empty(), "dress: another entity's segments are not taken");
    }

    // Mid-clip puppets dressed from their own create packet: a pointer into the
    // puppet list would go stale when another puppet is queued or erased while
    // they dress. The copy survives both.
    void TestWearSurvivesTheList()
    {
        using namespace replay;
        std::vector<playback::Puppet> list(1);
        list[0].packet.id = entity::ReplayId{82, 102};
        list[0].packet.appearance.assets.resize(1);
        list[0].packet.appearance.assets[0].drawable = 0x2B1B02CAu;
        list.push_back(playback::Puppet{});
        list[1].packet.id = entity::ReplayId{16, 539};
        list[1].packet.appearance.assets.resize(10);
        playback::SetWear(list[1], list[1].packet.appearance); // dressed from its create packet
        Check(playback::Wear(list[1]).assets.size() == 10, "wear: dressed with the create packet's 10 layers");
        for (int i = 0; i < 64; ++i)
            list.push_back(playback::Puppet{}); // queued while it dresses: the vector grows
        list.erase(list.begin()); // 82:102 deleted by the lifecycle: 16:539 moves down
        const playback::Puppet& moved = list[0];
        Check(moved.packet.id == (entity::ReplayId{16, 539}) && playback::Wear(moved).assets.size() == 10,
            "wear: the layers judged at the end of Dressing are the ones it was dressed with");
        playback::Puppet fresh;
        fresh.packet.appearance.assets.resize(3);
        Check(&playback::Wear(fresh) == &fresh.packet.appearance, "wear: before dressing, the create packet");
        playback::SetWear(fresh, fresh.wear); // self assignment is harmless
        Check(fresh.hasWear && fresh.wear.assets.empty(), "wear: set from its own copy");
    }

    // Arthur: create packet 21 layers, a change of 23 layers folded to t=0 by the
    // window cut. The puppet is dressed with the change (its skeleton is the
    // recorded one, signature S1), and the QA comparison takes the same: 23 of 23,
    // not 7 of 21.
    void TestArthurChangeAtZero()
    {
        using namespace replay;
        entity::ReplayId arthur{0, 1386};
        entity::Appearance create;
        create.assets.resize(21);
        AppearanceTrack track;
        track.id = arthur.Packed();
        AppearanceChange change;
        change.timeMs = 0;
        change.appearance.assets.resize(23);
        track.changes.push_back(change);
        const std::vector<entity::PoseSegment> segments;
        DressAppearance d = DressAt(&track, segments, arthur, nullptr, create, 0, kDressLookaheadMs);
        Check(d.appearance == &track.changes[0].appearance && d.key == 1 && std::string(d.from) == "change",
            "dress: Arthur at 0 wears the change folded to 0, not the create packet");
        d = DressAt(&track, segments, arthur, nullptr, create, 7209, kDressLookaheadMs);
        Check(d.appearance == &track.changes[0].appearance, "dress: and at the mark (7.2 s)");
        playback::Puppet puppet;
        puppet.packet.appearance = create;
        playback::SetWear(puppet, *d.appearance);
        Check(playback::Wear(puppet).assets.size() == 23, "qa: the mark compares with the 23 layers worn");
    }

    void TestSegmentSwitch()
    {
        using namespace replay::segswitch;
        // 84:114 at 39.6 s: the hat knocked off, the skeleton rebuilt in
        // place from 959 to 942 bones; the 942 record exists.
        Facts knock;
        knock.armed = true;
        knock.resolved = true;
        knock.samePointer = true;
        knock.armedBones = 959;
        knock.cloneBones = 942;
        knock.armedRecord = 0x80000008u;
        knock.segmentRecord = 0x80000008u; // the switch waits for the knock
        knock.segmentBones = 959;
        knock.recordForClone = 0x80000014u;
        Decision d = Decide(knock);
        Check(d.action == Action::RearmNow && d.record == 0x80000014u && std::string(d.why) == "skeleton_changed",
            "switch: a rebuilt skeleton is armed in the tick with the record of its bones");
        Facts none = knock;
        none.recordForClone = 0;
        Check(Decide(none).action == Action::Release, "switch: no record for the bones: the old path (release)");
        Facts same = knock;
        same.cloneBones = 959;
        Check(Decide(same).action == Action::Keep, "switch: nothing changed");
        Facts arrived = same;
        arrived.segmentRecord = 0x80000020u; // a segment with the same bones
        d = Decide(arrived);
        Check(d.action == Action::RearmNow && d.record == 0x80000020u && std::string(d.why) == "segment_record",
            "switch: a segment with the skeleton's bones swaps the record in the tick");
        Facts wait = same;
        wait.segmentRecord = 0x80000014u;
        wait.segmentBones = 942;
        d = Decide(wait);
        Check(d.action == Action::Keep && std::string(d.why) == "wait_rebuild",
            "switch: a segment the skeleton has not reached keeps posing the armed record");
        Facts unresolved = knock;
        unresolved.resolved = false;
        Check(Decide(unresolved).action == Action::Release, "switch: an unread skeleton is released");
        Facts idle;
        Check(Decide(idle).action == Action::Keep, "switch: not armed: the arm path");
        Facts moved = knock;
        moved.samePointer = false;
        moved.cloneBones = 959;
        Check(Decide(moved).action == Action::RearmNow, "switch: another skeleton object with a record is re-armed");

        Check(SwitchKeepsArm(true, false, 0x80000014u, 942, 0x80000014u, 942), "switch: the segment the arm already poses");
        Check(!SwitchKeepsArm(true, true, 0x80000014u, 942, 0x80000014u, 942), "switch: a mapped arm keeps the old path");
        Check(!SwitchKeepsArm(true, false, 0x80000008u, 959, 0x80000014u, 942), "switch: another record is not kept");
        Check(SwitchHoldsArm(true, false) && !SwitchHoldsArm(false, false) && !SwitchHoldsArm(true, true),
            "switch: an exact arm holds instead of being released");

        // The two paths over the 84:114 ticks (the knock seen in tick 1): releasing and
        // arming two ticks later leaves stale ticks; the current path has no tick
        // without a pose.
        Gap old;
        old.Change(Action::Release);
        old.Tick(false);
        old.Tick(false);
        old.Tick(true);
        Check(old.holeTicks == 2 && old.worstHole == 2 && old.released == 1, "switch: the old path, two ticks without a pose");
        Gap now;
        now.Change(Decide(knock).action);
        now.Tick(true);
        now.Tick(true);
        Check(now.holeTicks == 0 && now.rearmedInTick == 1 && now.switches == 1, "switch: re-armed in the tick, no hole");
    }

    // A skeleton layout of `body` bones in two chains off bone 0 (the root)
    // and then `pieces` groups of `pieceBones` hanging off bone 10 of the first
    // chain (as the layer groups of a metaped hang off the body).
    std::vector<std::int32_t> Layout(std::uint32_t body, std::uint32_t pieces, std::uint32_t pieceBones)
    {
        std::vector<std::int32_t> parents;
        const std::uint32_t second = body / 2; // the second chain's first bone
        for (std::uint32_t bone = 0; bone < body; ++bone)
            parents.push_back(bone == 0 ? -2 : bone == second ? 0 : static_cast<std::int32_t>(bone - 1));
        for (std::uint32_t piece = 0; piece < pieces; ++piece)
            for (std::uint32_t k = 0; k < pieceBones; ++k)
                parents.push_back(k == 0 ? 10 : static_cast<std::int32_t>(parents.size() - 1));
        return parents;
    }

    void TestPrefixHold()
    {
        using namespace replay::prefixhold;
        using namespace replay::segswitch;
        // 1272-Blackwater, 42:43 shot in the head at 5.6 s: armed on the 998-bone
        // record, the skeleton rebuilt in place to 705 (the body group alone) for
        // three ticks, then 857 (the record after the shot).
        const std::vector<std::int32_t> record = Layout(705, 7, 41); // 705 + 287 = 992
        Check(BodyBones(record.data(), static_cast<std::uint32_t>(record.size())) == 705,
            "prefix: the body group of a layout is its first group");
        Check(BodyBones(nullptr, 0) == 0, "prefix: no layout, no body group");

        Check(Writable(705, 998, 705), "prefix: a skeleton shrunk to the body group is written");
        Check(Writable(600, 998, 705), "prefix: fewer bones than the body group too");
        Check(!Writable(857, 998, 705), "prefix: more bones than the body group are not");
        Check(!Writable(998, 998, 705) && !Writable(1000, 998, 1000), "prefix: only fewer bones than the record");
        Check(!Writable(705, 998, 0) && !Writable(0, 998, 705), "prefix: no body group or no bones: off");

        Facts gore;
        gore.armed = true;
        gore.resolved = true;
        gore.samePointer = true;
        gore.armedBones = 998;
        gore.cloneBones = 705;
        gore.armedRecord = 0x2a002bu;
        gore.segmentRecord = 0x2a002bu;
        gore.segmentBones = 998;
        gore.recordForClone = 0; // no record has 705 bones
        gore.bodyBones = 705;
        Decision d = Decide(gore);
        Check(d.action == Action::HoldPrefix && std::string(d.why) == "prefix_hold",
            "prefix: the gore rebuild keeps the arm (the record's first bones), not released");
        Facts later = gore;
        later.segmentRecord = 0x80000004u; // the switch to the 857 segment, skeleton still 705
        later.segmentBones = 857;
        Check(Decide(later).action == Action::HoldPrefix, "prefix: held across the segment switch");
        Facts done = later;
        done.cloneBones = 857;
        done.recordForClone = 0x80000004u;
        d = Decide(done);
        Check(d.action == Action::RearmNow && d.record == 0x80000004u, "prefix: the 857 record arms in the tick it fits");
        Facts other = gore;
        other.samePointer = false;
        Check(Decide(other).action == Action::Release, "prefix: another skeleton object is not held");
        Facts noBody = gore;
        noBody.bodyBones = 0;
        Check(Decide(noBody).action == Action::Release, "prefix: no body group known: the old path");
        Facts grown = gore;
        grown.cloneBones = 1010;
        Check(Decide(grown).action == Action::Release, "prefix: a skeleton that grew is not a prefix");

        // The puppet's table against the record's first bones (engine tables
        // are uint16, bone 0 any value).
        std::vector<std::uint16_t> clone;
        for (std::uint32_t bone = 0; bone < 705; ++bone)
            clone.push_back(bone == 0 ? 0xFFFFu : static_cast<std::uint16_t>(record[bone]));
        Check(SameFront(record.data(), static_cast<std::uint32_t>(record.size()), clone.data(), 705),
            "prefix: the body group's table is the record's front");
        std::vector<std::int32_t> demoted = record;
        demoted[300] = replay::pose::kUnresolvedParent; // a bone recorded in object space
        Check(SameFront(demoted.data(), static_cast<std::uint32_t>(demoted.size()), clone.data(), 705),
            "prefix: a bone kept in object space vouches for nothing");
        std::vector<std::uint16_t> moved = clone;
        moved[400] = 12; // the record has 399
        Check(!SameFront(record.data(), static_cast<std::uint32_t>(record.size()), moved.data(), 705),
            "prefix: another parent is another skeleton");
        Check(!SameFront(record.data(), 10, clone.data(), 705) && !SameFront(nullptr, 0, clone.data(), 705),
            "prefix: more bones than the record, or no record: not a front");

        // The hold over the 42:43 ticks (ms of GetTickCount64).
        Hold hold;
        Check(!hold.Active() && !hold.Holds(1000), "prefix: no hold");
        Check(hold.Note(705, 1000) && hold.Active() && hold.ticks == 1, "prefix: the first tick begins the hold");
        Check(hold.Holds(1016), "prefix: held while the table is not read");
        Check(!hold.Note(705, 1033) && hold.ticks == 2, "prefix: the next tick goes on with it");
        hold.verdict = Verdict::Same;
        Check(hold.Holds(1050), "prefix: held once the front is verified");
        Check(!hold.Holds(1000 + kMaxMs + 1) && hold.Expired(1000 + kMaxMs + 1), "prefix: not past its bound");
        Check(hold.Note(690, 1066) && hold.verdict == Verdict::Pending && hold.ticks == 1,
            "prefix: another count while held is compared again");
        hold.verdict = Verdict::Differ;
        Check(!hold.Holds(1070), "prefix: a front that differs is released");
        hold.verdict = Verdict::Unread;
        Check(!hold.Holds(1070), "prefix: a table that cannot be read is released");
        hold.End();
        Check(!hold.Active() && hold.bones == 0, "prefix: ended");
        Hold zero;
        Check(zero.Note(705, 0) && zero.Active(), "prefix: a hold begun at tick count 0 is still active");

        // Over the ticks: the old path hid the body three ticks (released at the
        // 705 rebuild, armed on the 857 record later); held, no tick without a pose.
        Gap old;
        old.Change(Decide(noBody).action);
        old.Tick(false);
        old.Tick(false);
        old.Tick(false);
        old.Tick(true);
        Check(old.holeTicks == 3 && old.released == 1, "prefix: the old path, three ticks hidden");
        Gap held;
        held.Change(Decide(gore).action);
        held.Tick(true);
        held.Tick(true);
        held.Change(Decide(done).action);
        held.Tick(true);
        Check(held.holeTicks == 0 && held.rearmedInTick == 2 && held.released == 0, "prefix: held, then armed: no hole");
    }

    void TestCameraBits()
    {
        using namespace replay::camcut;
        Check(ModeFromIni(nullptr) == Mode::Bits && ModeFromIni("") == Mode::Bits && ModeFromIni("BITS") == Mode::Bits &&
                ModeFromIni("clearbits") == Mode::Bits && ModeFromIni("Params") == Mode::Params &&
                ModeFromIni("legacy") == Mode::Legacy,
            "camera: plan B is the default, params and legacy by name");
        Check(std::string(ModeName(Mode::Bits)) == "bits", "camera: the mode's name");
        Check(ClearedCutBits(0x00C3) == 0x0003 && ClearedCutBits(0x0040) == 0 && ClearedCutBits(0x0100) == 0x0100,
            "camera: only the cut bits go");
        const float want[3] = {-2380.77f, -2376.20f, 62.71f};
        const float same[3] = {-2380.77f, -2376.20f, 62.71f};
        const float other[3] = {-2369.63f, -2388.54f, 63.53f};
        Check(CamObjectIsOurs(same, want, 0x00C0) && !CamObjectIsOurs(other, want, 0x00C0) && !CamObjectIsOurs(same, want, 0x0080),
            "camera: the object written is the camera just placed (position and the position bit)");
        Check(sizeof(kCamLookup) == 103 && kCamLookup[0] == 0x48 && kCamLookup[102] == 0xC3, "camera: the lookup's body");
    }

    void TestQaKind()
    {
        replay::qa::Anomaly a;
        Check(replay::qa::ClassifyLine("POSE_V2_APPEARANCE_NOT_RECORDED,id=112:146,kind=ped,model=0xf91b281f,why=no_layers,"
                                       "create_layers=0,dressed_from=none,metaped_states=0,playhead_ms=7143",
                  0, a) &&
                a.kind == replay::qa::Kind::AppearanceNotRecorded && a.entity == "112:146" && a.playheadMs == 7143 &&
                a.detail.find("why=no_layers") != std::string::npos,
            "qa: POSE_V2_APPEARANCE_NOT_RECORDED is appearance_not_recorded");
        Check(std::string(replay::qa::KindText(replay::qa::Kind::AppearanceNotRecorded)) == "appearance_not_recorded",
            "qa: the kind's name");
    }
}

namespace
{
    // Feet under the boots: the shop items read per component (one per item), the
    // choice of the dress path, the read back.
    void TestShopItems()
    {
        using namespace replay::metaped;
        std::vector<ShopItem> items;
        Check(AddShopItem(items, 0xBEEF0001u, 0, false) && AddShopItem(items, 0xBEEF0002u, 0xCAFE0001u, true),
            "shop: two items");
        Check(!AddShopItem(items, 0xBEEF0001u, 0x1234u, false) && !AddShopItem(items, 0, 0, false) && items.size() == 2 &&
                items[0].state == 0 && items[1].state == 0xCAFE0001u && items[1].flags == kShopItemStatus,
            "shop: the components of one item are one item (the first read kept); 0 is no item");
        std::vector<ShopItem> many;
        for (std::uint32_t n = 1; n <= kMaxShopItems + 5; ++n)
            AddShopItem(many, n, 0, false);
        Check(many.size() == kMaxShopItems, "shop: bounded");

        DressFacts f;
        f.hasState = true;
        f.stateHasItems = true;
        f.printMatches = true;
        DressChoice c = ChooseDress(f);
        Check(c.path == DressPath::ShopItems && std::string(c.why) == "ok" && std::string(DressPathText(c.path)) == "shop_items",
            "shop: Arthur with shop items recorded for the layers worn is dressed by them");
        DressFacts g = f;
        g.printMatches = false;
        Check(ChooseDress(g).path == DressPath::Layers && std::string(ChooseDress(g).why) == "print_differs",
            "shop: items recorded with other layers (an alternate, a lookahead) are not worn");
        g = f;
        g.shopRefused = true;
        Check(std::string(ChooseDress(g).why) == "shop_refused", "shop: a puppet whose shop dress failed keeps the layers");
        g = f;
        g.stateHasItems = false;
        Check(std::string(ChooseDress(g).why) == "no_shop_items", "shop: a body without shop items (NPC by layers)");
        g = f;
        g.hasState = false;
        Check(std::string(ChooseDress(g).why) == "no_state", "shop: a clip before MPED");
        g = f;
        g.wearEmpty = true;
        Check(ChooseDress(g).path == DressPath::Layers && std::string(ChooseDress(g).why) == "no_layers",
            "shop: nothing to wear stays the layer path (categories emptied)");

        const std::vector<ShopItem> recorded = {{0x10u, 0u, 0u}, {0x20u, 0x99u, 0u}, {0x30u, 0x77u, 0u}};
        ShopDiff d = CompareShopItems(recorded, recorded);
        Check(d.Exact() && d.matched == 3 && d.recorded == 3 && d.puppet == 3, "shop: read back as recorded");
        const std::vector<ShopItem> puppet = {{0x20u, 0x98u, 0u}, {0x10u, 0u, 0u}, {0x40u, 0u, 0u}};
        d = CompareShopItems(recorded, puppet);
        Check(!d.Exact() && d.matched == 1 && d.stateDiffer == 1 && d.firstDiffer == 0x20u && d.missing == 1 &&
                d.firstMissing == 0x30u && d.extra == 1,
            "shop: the pants untucked (state), the boots missing, an item of the puppet's own");
        d = CompareShopItems({}, puppet);
        Check(d.recorded == 0 && d.extra == 3 && !d.Exact(), "shop: a puppet with items nobody recorded");

        State st;
        st.flags = kHasShopItems;
        st.shopItems = recorded;
        const std::string text = Describe(st, "  ");
        Check(text.find("shop_items=3") != std::string::npos && text.find("0x00000020:0x00000099") != std::string::npos,
            "shop: ClipTool text");
        State unread;
        Check(Describe(unread, "").find("shop_items=unread") != std::string::npos, "shop: a clip before them says unread");
    }
}

int RunAppearanceTests()
{
    TestStateAt();
    TestExpressions();
    TestParse();
    TestNotRecorded();
    TestDressAt();
    TestWearSurvivesTheList();
    TestArthurChangeAtZero();
    TestSegmentSwitch();
    TestPrefixHold();
    TestCameraBits();
    TestQaKind();
    TestShopItems();
    if (g_failures == 0)
        std::puts("appearance, segment switch and camera bits tests passed");
    return g_failures;
}
