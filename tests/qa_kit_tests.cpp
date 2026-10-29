// The pure parts of "mark problem", the rolling health buffer, the report's
// zip and system info, and the QA screenshots' pixels (with their CPU cost on
// this machine).
#include "qa_bundle.h"
#include "qa_health.h"
#include "qa_image.h"
#include "qa_jpeg.h"
#include "qa_marks.h"
#include "qa_report.h"
#include "qa_zip.h"
#include "log_policy.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    int g_qaFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: qa kit: %s\n", message);
        ++g_qaFailures;
    }

    using namespace replay::qa;

    std::string Env(const char* name, const char* fallback)
    {
        char value[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableA(name, value, MAX_PATH);
        return length > 0 && length < MAX_PATH ? std::string(value) : std::string(fallback);
    }

    void WriteBytes(const std::string& path, const std::vector<std::uint8_t>& bytes)
    {
        FILE* file = nullptr;
        if (fopen_s(&file, path.c_str(), "wb") == 0 && file)
        {
            std::fwrite(bytes.data(), 1, bytes.size(), file);
            std::fclose(file);
        }
    }

    void TestField()
    {
        Check(Field("POSE_JERK,xid=5,id=7:1,kind=stall", "id") == "7:1", "field: a key inside another key is not it");
        Check(Field("TAG,a=1,b=22", "b") == "22", "field: the last one");
        Check(Field("TAG,a=1,b=22", "c").empty(), "field: absent is empty");
        Check(Field("id=3,b=1", "id").empty(), "field: the tag position is never a key");
    }

    void TestClassify()
    {
        Anomaly a;
        Check(ClassifyLine("PUPPET_MISSING,id=12:3,kind=ped,model=0xabc,reason=unposed,camera_m=12,missing_ms=600,entry_ms=100,playhead_ms=5000",
                  1, a) &&
                a.kind == Kind::PuppetUnposed && a.entity == "12:3" && a.playheadMs == 5000 &&
                a.detail.find("reason=unposed") != std::string::npos,
            "missing puppet, unposed, with its playhead");
        Check(ClassifyLine("PUPPET_MISSING,id=1:1,kind=horse,model=0x1,reason=no_puppet,camera_m=3,missing_ms=600,playhead_ms=10", 0, a) &&
                a.kind == Kind::PuppetGone,
            "no puppet is a despawn pop");
        Check(ClassifyLine("PUPPET_MISSING,id=1:1,kind=horse,model=0x1,reason=arm_refused,camera_m=3,missing_ms=600,playhead_ms=10", 0, a) &&
                a.kind == Kind::ArmRefused,
            "arm refused through the watch");
        Check(ClassifyLine("PUPPET_MISSING,id=1:1,kind=ped,model=0x1,reason=model_loading,camera_m=3,missing_ms=600,playhead_ms=10", 0, a) &&
                a.kind == Kind::PuppetHidden,
            "other reasons are hidden");
        Check(!ClassifyLine("PUPPET_MISSING_END,id=1:1,kind=ped,reason=dressing,missing_ms=900,suspended=0,playhead_ms=10", 0, a),
            "the end of an episode is not an anomaly");
        Check(ClassifyLine("POSE_V2_PLAYBACK_ARM,id=4:1,status=failed,mode=refused,error=no_record,slot=-1,clone_bones=710,track_bones=705", 7777, a) &&
                a.kind == Kind::ArmRefused && a.playheadMs == 7777 && a.detail.find("clone_bones=710") != std::string::npos,
            "arm refused: the playhead of the tick when the line has none");
        Check(ClassifyLine("POSE_V2_PLAYBACK_ARM,id=4:1,status=armed,mode=mapped,error=none,slot=3,clone_bones=710,track_bones=705,matched=690,rest=20,dropped=15", 1, a) &&
                a.kind == Kind::ArmMapped,
            "arm through the skeleton map");
        Check(!ClassifyLine("POSE_V2_PLAYBACK_ARM,id=4:1,status=armed,mode=exact,error=none,slot=3", 1, a), "an exact arm is fine");
        Check(ClassifyLine("POSE_V2_ARM_REFUSED_HIDDEN,id=9:2,kind=horse,model=0x1,error=no_map,clone_bones=710,track_bones=705,in_track=1,playhead_ms=41000", 0, a) &&
                a.kind == Kind::ArmRefused && a.playheadMs == 41000,
            "refused and hidden");
        Check(ClassifyLine("POSE_V2_UNPOSED_HIDDEN,id=9:2,playhead_ms=5", 0, a) && a.kind == Kind::PuppetUnposed, "unposed hidden");
        Check(ClassifyLine("PUPPET_POP_IN,id=3:1,kind=ped,model=0x2,camera_m=12,on_screen=1,entry_ms=4000,playhead_ms=4100", 0, a) &&
                a.kind == Kind::PopIn && a.detail.find("on_screen=1") != std::string::npos,
            "pop in");
        Check(ClassifyLine("POSE_V2_CREATE_REFUSED,id=3:1,kind=ped,model=0x2,attempts=3,model_loaded=1", 5, a) && a.kind == Kind::CreateRefused,
            "create refused");
        Check(ClassifyLine("POSE_JERK,id=2:1,kind=engine_moved,playhead_ms=1200,magnitude_m=1.500,engine_m=1.4,mount=none,mount_m=0.000,"
                           "speed_mps=3.0,engine_velocity_mps=0.00,wall_step_ms=16.7,play_step_ms=17,lost_ms=0.0,causes=physics",
                  0, a) &&
                a.kind == Kind::PoseJump && a.playheadMs == 1200 && a.detail.find("magnitude_m=1.500") != std::string::npos,
            "pose jump");
        Check(ClassifyLine("POSE_V2_PLAYBACK_SPEECH,ped=1:1,context=0xAB,voice=0xCD,variation=1,status=refused:voice:pinned:line:none,"
                           "event_ms=2900,request_ms=2950,playhead_ms=3000",
                  0, a) &&
                a.kind == Kind::SpeechRefused && a.entity == "1:1" && a.detail.find("status=refused:voice") != std::string::npos,
            "speech refused");
        Check(!ClassifyLine("POSE_V2_PLAYBACK_SPEECH,ped=1:1,status=played:voice:pinned:line:ok,playhead_ms=3000", 0, a), "speech played");
        Check(ClassifyLine("POSE_V2_ANIMAL_VOCAL,ped=5:1,context=0x1,requested=0x1,voice=0x2,variation=0,direct_line=0x0,stop_reason=4,age_ms=10,"
                           "status=ok,lines=1->1,created=0,event_ms=10",
                  77, a) &&
                a.kind == Kind::AnimalVocalMissing && a.playheadMs == 77,
            "animal vocal not created");
        Check(!ClassifyLine("POSE_V2_ANIMAL_VOCAL,ped=5:1,created=1,event_ms=10", 77, a), "animal vocal created");
        Check(ClassifyLine("POSE_V2_PLAYBACK_SOUND,ped=2:1,kind=0,ref=0x1,status=not_queued:empty,event_ms=1,playhead_ms=2", 0, a) &&
                a.kind == Kind::SoundRefused,
            "sound not queued");
        Check(!ClassifyLine("POSE_V2_PLAYBACK_SOUND,ped=2:1,kind=0,ref=0x1,status=queued,event_ms=1,playhead_ms=2", 0, a), "sound queued");
        Check(ClassifyLine("POSE_V2_PLAYBACK_FIRE_SKIP,index=1,type=2,reason=no_entity,entity_a=none,entity_b=none,failures=3,gave_up=1,"
                           "event_ms=5,playhead_ms=6",
                  0, a) &&
                a.kind == Kind::FxRefused && a.entity.empty() && a.detail.find("what=PLAYBACK_FIRE_SKIP") != std::string::npos,
            "fire skipped, no entity");
        Check(ClassifyLine("POSE_V2_FX_REGISTERED_ATTACH_MISS,hash=0x12,id=4:4,puppet=none,state=-1,error=x,start_ms=3", 9, a) &&
                a.kind == Kind::FxRefused && a.entity == "4:4",
            "registered fx without its puppet");
        Check(ClassifyLine("POSE_V2_ENTITY_SOUND,phase=play,id=0:1,ref=0x80000e002f4361,ped_audio=1,train_audio=0,status=refused_by_engine,"
                           "crossed=1,event_ms=45541,playhead_ms=45670",
                  0, a) &&
                a.kind == Kind::EntitySoundRefused && a.entity == "0:1" && a.playheadMs == 45670 &&
                a.detail.find("status=refused_by_engine") != std::string::npos,
            "entity sound refused");
        Check(!ClassifyLine("POSE_V2_ENTITY_SOUND,phase=play,id=0:1,ref=0x1,ped_audio=1,train_audio=0,status=playing,crossed=1,event_ms=1,playhead_ms=2", 0, a),
            "entity sound playing");
        Check(!ClassifyLine("POSE_V2_ENTITY_SOUND,phase=play,id=0:1,ref=0x1,ped_audio=1,train_audio=0,status=too_far,crossed=1,event_ms=1,playhead_ms=2", 0, a),
            "entity sound too far is not a refusal");
        Check(!ClassifyLine("POSE_V2_ENTITY_SOUND,phase=record_end,id=0:1,how=stop,start_ms=2712,end_ms=3792", 0, a), "entity sound record end");
        Check(ClassifyLine("POSE_V2_PLAYBACK_WRITE_CHECK,id=33:274,state=changed,bones=262,local_bones=0,object_bones=6,first_bone=96,last_bone=101,"
                           "worst_bone=101,max_m=0.280,max_rad=0.000,record=0x8000001b,playhead_ms=52836",
                  0, a) &&
                a.kind == Kind::PoseOverwritten && a.entity == "33:274" && a.playheadMs == 52836 &&
                a.detail == "object_bones=6;local_bones=0;first_bone=96;last_bone=101;max_m=0.280",
            "pose overwritten by the engine after the write");
        Check(!ClassifyLine("POSE_V2_PLAYBACK_WRITE_CHECK,id=33:274,state=clean,bones=262,local_bones=0,object_bones=0,playhead_ms=50630", 0, a),
            "a clean write check");
        Check(std::string(KindText(Kind::GenericRefused)) == "generic_refused" && std::string(KindText(Kind::EntitySoundSilent)) == "entity_sound_silent" &&
                CountsText(Counts()).find("pose_overwritten=0") != std::string::npos,
            "the new kinds are named and counted");
        Check(std::string(KindText(Kind::AppearanceMismatch)) == "appearance_mismatch" && std::string(KindText(Kind::PropMisplaced)) == "prop_misplaced" &&
                CountsText(Counts()).find("prop_misplaced=0") != std::string::npos,
            "the press-time kinds are named and counted");
        Check(ClassifyLine("POSE_V2_ROPE_STRAY,engine_id=1391,type=14,camera_m=6,holder=player,entity=0x7ff6a0,playhead_ms=5120", 0, a) &&
                a.kind == Kind::RopeStray && a.entity == "1391" && a.playheadMs == 5120 &&
                a.detail == "type=14;camera_m=6;holder=player;entity=0x7ff6a0",
            "task 19: a world rope drawn near the camera");
        Check(std::string(KindText(Kind::RopeStray)) == "rope_stray" && CountsText(Counts()).find("rope_stray=0") != std::string::npos,
            "rope_stray is named and counted");
        Check(!ClassifyLine("PLAYBACK_STOP,playhead_ms=5", 0, a), "other lines are not anomalies");
        Check(!ClassifyLine("", 0, a) && !ClassifyLine("=== Redemption Editor loaded ===", 0, a), "empty and header lines");
    }

    Anomaly Make(std::uint64_t wall, std::uint32_t playhead, Kind kind)
    {
        Anomaly a;
        a.wallMs = wall;
        a.playheadMs = playhead;
        a.kind = kind;
        return a;
    }

    void TestRing()
    {
        HealthRing ring(4);
        ring.Push(Make(1000, 500, Kind::Hitch));
        ring.Push(Make(2000, 1500, Kind::PopIn));
        ring.Push(Make(3000, 2500, Kind::PoseJump));
        ring.Push(Make(4000, 3500, Kind::PoseJump));
        ring.Push(Make(5000, 4500, Kind::SoundRefused));
        Check(ring.Size() == 4 && ring.Overflowed() == 1, "ring: capacity drops the oldest");
        Check(ring.Totals().total == 5 && ring.Totals().perKind[static_cast<int>(Kind::PoseJump)] == 2, "ring: the counts keep everything");
        const std::vector<Anomaly> window = ring.Window(3000, 1000, 1000);
        Check(window.size() == 2 && window[0].playheadMs == 2500 && window[1].playheadMs == 3500, "ring: the playhead window");
        {
            // A window pressed after a scrub takes every pass
            // in the ring; the same line of two passes is kept once, from
            // the latest pass, and the window comes in playhead order.
            HealthRing passes(8);
            Anomaly first = Make(1000, 2500, Kind::SpeechRefused);
            first.entity = "0:1";
            first.detail = "status=refused";
            first.pass = 0;
            passes.Push(first);
            Anomaly other = Make(1500, 2400, Kind::PoseJump);
            other.pass = 0;
            passes.Push(other);
            Anomaly again = first;
            again.wallMs = 90000;
            again.pass = 2;
            passes.Push(again);
            const std::vector<Anomaly> merged = passes.Window(3000, 1000, 1000);
            Check(merged.size() == 2 && merged[0].playheadMs == 2400 && merged[1].playheadMs == 2500 && merged[1].pass == 2 &&
                    merged[1].wallMs == 90000,
                "ring: passes merged, the duplicate kept once from the latest pass");
            Anomaly differs = first;
            differs.detail = "status=mismatch";
            passes.Push(differs);
            Check(passes.Window(3000, 1000, 1000).size() == 3, "ring: a different detail is another anomaly");
        }
        ring.Prune(7500, 3000);
        Check(ring.Size() == 1, "ring: prune by wall age");
        Check(CountsText(ring.Totals()).find("pose_jump=2") != std::string::npos &&
                CountsText(ring.Totals()).find("hitch=1") != std::string::npos,
            "ring: the counts text");
        ring.Reset();
        Check(ring.Size() == 0 && ring.Totals().total == 0, "ring: reset");
        Check(ring.Window(100, kBeforeMs, kAfterMs).empty(), "ring: an empty window");
        HealthRing low;
        low.Push(Make(100, 50, Kind::Hitch));
        Check(low.Window(1000, kBeforeMs, kAfterMs).size() == 1, "ring: a window at the clip's start clamps at 0");
    }

    void TestWindow()
    {
        Check(MaxWaitMs(1.0) == 3000 && MaxWaitMs(4.0) == 2500 && MaxWaitMs(0.05) == 12000, "max wait bounds");
        MarkWindow window;
        window.playheadMs = 10000;
        window.pressWall = 50000;
        window.rate = 1.0;
        Check(!WindowClosed(window, 50500, 10500, true), "window: open half a second later");
        Check(WindowClosed(window, 51900, 12000, true), "window: closed when the playhead passed 2 s");
        Check(WindowClosed(window, 50100, 10100, false), "window: closed when the playback stops");
        Check(!WindowClosed(window, 52000, 10000, true) && WindowClosed(window, 53000, 10000, true), "window: paused waits, then closes");
    }

    void TestMarkJson()
    {
        Mark mark;
        mark.id = 3;
        mark.folder = "mark-20261005-210304-03";
        mark.version = "1.0.0-dev";
        mark.clip = "042-Valentine.rpclip";
        mark.playheadMs = 12345;
        mark.note = "horse \"floats\"\nthen pops";
        mark.noteSet = true;
        BodyInView body;
        body.id = "4:1";
        body.kind = "horse";
        body.model = 0xABCDEF01;
        body.distanceM = 12.25f;
        mark.bodies.push_back(body);
        // A ped checked at the press, with a dropped hat.
        BodyInView ped;
        ped.id = "69:280";
        ped.kind = "ped";
        ped.model = 0x54D8BE8E;
        ped.distanceM = 12.7f;
        ped.boneCount = 959;
        ped.armed = true;
        ped.armedBones = 959;
        ped.record = 0x8000001c;
        ped.armedRecord = 0x8000001c;
        ped.layersChecked = true;
        ped.layersRecorded = 10;
        ped.layersPresent = 9;
        ped.pedLayers = 10;
        PropInView hat;
        hat.id = "72:345";
        hat.origin = "hat";
        hat.model = 0x30762E1A;
        hat.state = "ready";
        hat.clone = true;
        hat.knockRequested = true;
        hat.placeErrorM = 0.31f;
        hat.kind = "prop";
        hat.link = "owner";
        ped.props.push_back(hat);
        // A held object without a weapon is listed too, with its kind and link.
        PropInView note;
        note.id = "18:585";
        note.kind = "weapon";
        note.link = "weapon_point";
        note.origin = "held_object";
        note.model = 0x04D58C8E;
        note.state = "ready";
        note.clone = true;
        ped.props.push_back(note);
        mark.bodies.push_back(ped);
        mark.pass = 3;
        Anomaly anomaly = Make(1, 11000, Kind::ArmRefused);
        anomaly.entity = "4:1";
        anomaly.pass = 2;
        mark.anomalies.push_back(anomaly);
        const std::string json = MarkJson(mark);
        Check(json.find("\"pass\": 3,") != std::string::npos && json.find("\"offset_ms\":-1345,\"pass\":2,") != std::string::npos,
            "json: the pass at the press and the anomaly's pass");
        Check(json.find("\"bones\": 0, \"layers\": null, \"bones_armed\": 0, \"record\": \"0x0\", \"armed_record\": \"0x0\", \"props\": []}") != std::string::npos,
            "json: a body not checked says so");
        Check(json.find("\"bones\": 959, \"layers\": {\"recorded\": 10, \"present\": 9, \"ped\": 10}, \"bones_armed\": 959, \"record\": \"0x8000001c\", "
                        "\"armed_record\": \"0x8000001c\", \"props\": [{\"id\": \"72:345\", \"kind\": \"prop\", \"link\": \"owner\", "
                        "\"origin\": \"hat\", \"model\": \"0x30762E1A\", \"weapon\": \"0x00000000\", \"state\": \"ready\", "
                        "\"clone\": true, \"knock_requested\": true, \"place_error_m\": 0.31}, ") != std::string::npos,
            "json: the body's layers, records and its dropped hat");
        Check(json.find("{\"id\": \"18:585\", \"kind\": \"weapon\", \"link\": \"weapon_point\", \"origin\": \"held_object\", "
                        "\"model\": \"0x04D58C8E\"") != std::string::npos,
            "json: a held object without a weapon is one of the body's props, with its kind (M1)");
        Check(json.find("\"playhead\": \"0:12.345\"") != std::string::npos, "json: the playhead as text");
        Check(json.find("horse \\\"floats\\\"\\nthen pops") != std::string::npos, "json: the note escaped");
        Check(json.find("\"model\": \"0xABCDEF01\"") != std::string::npos, "json: the model hash");
        Check(json.find("\"offset_ms\":-1345") != std::string::npos, "json: the anomaly's offset from the press");
        Check(json.find("\"kind\":\"arm_refused\"") != std::string::npos, "json: the anomaly's kind");
        // The sounds block, unknown players.
        Check(json.find("\"sounds\": {\"from_ms\": 4345, \"to_ms\": 14345,\n    \"generic\": null,\n    \"entity_sounds\": null}") != std::string::npos ||
                json.find("\"generic\": null") != std::string::npos,
            "json: the sounds block says what it does not know");
        mark.sounds.genericKnown = true;
        mark.sounds.fromMs = 4345;
        mark.sounds.toMs = 14345;
        mark.sounds.packets = 3;
        SoundRow family;
        family.family = "entity_sound";
        family.taker = "specific";
        family.packets = 3;
        family.skipped = 3;
        mark.sounds.families.push_back(family);
        SoundRow owner = family;
        owner.owner = "0:1";
        mark.sounds.owners.push_back(owner);
        SoundFailure failure;
        failure.timeMs = 12000;
        failure.owner = "0:1";
        failure.family = "owned";
        failure.taker = "generic";
        failure.set = 0x2f4361;
        failure.outcome = "refused";
        mark.sounds.failures.push_back(failure);
        mark.sounds.entityKnown = true;
        mark.sounds.entityTotals.rows = 72;
        mark.sounds.entityTotals.passed = 72;
        EntitySoundRow row;
        row.owner = "0:1";
        row.rows = 72;
        row.passed = 72;
        row.lastStatus = "no_slot";
        mark.sounds.entityOwners.push_back(row);
        const std::string withSounds = MarkJson(mark);
        Check(withSounds.find("{\"family\":\"entity_sound\",\"taker\":\"specific\",\"packets\":3,\"played\":0,\"refused\":0,\"yielded\":0,\"missed\":0,\"jumped\":0,\"skipped\":3,\"not_reached\":0,\"doubles\":0}") != std::string::npos,
            "json: a family row");
        Check(withSounds.find("{\"owner\":\"0:1\",\"family\":\"entity_sound\"") != std::string::npos, "json: an owner row");
        Check(withSounds.find("{\"time_ms\":12000,\"offset_ms\":-345,\"owner\":\"0:1\",\"family\":\"owned\",\"taker\":\"generic\",\"set\":\"0x2f4361\",\"outcome\":\"refused\"}") != std::string::npos,
            "json: a failure with its offset from the press");
        Check(withSounds.find("\"totals\": {\"rows\":72,\"played\":0,\"refused\":0,\"too_far\":0,\"by_puppet\":0,\"passed\":72,\"to_generic\":0,\"untried\":0,\"last_status\":\"\"}") != std::string::npos &&
                withSounds.find("{\"owner\":\"0:1\",\"rows\":72,") != std::string::npos && withSounds.find("\"last_status\":\"no_slot\"") != std::string::npos,
            "json: the entity sound rows");
        int depth = 0, low = 0;
        bool inString = false;
        for (std::size_t n = 0; n < json.size(); ++n)
        {
            const char c = json[n];
            if (inString)
            {
                if (c == '\\')
                    ++n;
                else if (c == '"')
                    inString = false;
                continue;
            }
            if (c == '"')
                inString = true;
            else if (c == '{' || c == '[')
                ++depth;
            else if (c == '}' || c == ']')
                low = (std::min)(low, --depth);
        }
        Check(depth == 0 && low == 0 && !inString, "json: balanced");
        {
            // The same balance with the sounds block filled.
            int d2 = 0, low2 = 0;
            bool str2 = false;
            for (std::size_t n = 0; n < withSounds.size(); ++n)
            {
                const char c = withSounds[n];
                if (str2)
                {
                    if (c == '\\')
                        ++n;
                    else if (c == '"')
                        str2 = false;
                    continue;
                }
                if (c == '"')
                    str2 = true;
                else if (c == '{' || c == '[')
                    ++d2;
                else if (c == '}' || c == ']')
                    low2 = (std::min)(low2, --d2);
            }
            Check(d2 == 0 && low2 == 0 && !str2, "json: balanced with the sounds block");
        }
        Check(JsonEscape(std::string("a\x01" "b")) == "a\\u0001b", "json: control characters");
    }

    // The report header's marks line from the folders moved.
    void TestMarksLine()
    {
        using namespace replay::qareport;
        Check(MarksText({}).empty(), "marks line: none");
        Check(MarksText({"mark-a", "mark-b"}) == "2 (mark-a;mark-b)", "marks line: two");
        const std::string text = "Redemption Editor QA report\r\nversion: x\r\nmarks: none\r\nThe clips named by the marks...\r\n\r\nfiles:\r\n";
        const std::string replaced = ReplaceMarksLine(text, MarksText({"mark-20261006-110243-01", "mark-20261006-110249-02", "mark-20261006-110622-03"}));
        Check(replaced.find("\r\nmarks: 3 (mark-20261006-110243-01;mark-20261006-110249-02;mark-20261006-110622-03)\r\nThe clips") != std::string::npos &&
                replaced.find("marks: none") == std::string::npos,
            "marks line: replaced in place, the line ending kept");
        Check(ReplaceMarksLine(text, "") == text, "marks line: none stays none");
        Check(ReplaceMarksLine("version: x\r\n", "1 (m)") == "version: x\r\nmarks: 1 (m)\r\n", "marks line: added when absent");
        Check(ReplaceMarksLine("remarks: 5\r\nmarks: none", "1 (m)") == "remarks: 5\r\nmarks: 1 (m)", "marks line: only at a line start, no newline after");
        // The clips a mark named that were gone (011 deleted before the report).
        Check(AddMissingClipsLine(text, {}) == text, "missing clips: nothing when every clip was there");
        const std::string missing = AddMissingClipsLine(text, {"011-GrizzliesWest.rpclip"});
        Check(missing.find("\r\nmarks: none\r\nmissing_clips: 1 (011-GrizzliesWest.rpclip; named by a mark or open, not in ReplayClips "
                           "when the report was made)\r\nThe clips") != std::string::npos,
            "missing clips: the line right after the marks line");
        Check(AddMissingClipsLine("version: x\r\n", {"a.rpclip", "b.rpclip"}).find("missing_clips: 2 (a.rpclip;b.rpclip;") != std::string::npos,
            "missing clips: added at the end without a marks line");
    }

    // From QA: only the player's marks of this session, still on disk,
    // count for the reminders; an earlier session's folders are "older".
    void TestPendingMarks()
    {
        const std::vector<std::string> none;
        PendingMarks c = CountPendingMarks(none, none);
        Check(c.session == 0 && c.older == 0, "pending: nothing anywhere");
        // A stray folder of an earlier session (a game closed before Shift+F1).
        c = CountPendingMarks(none, {"mark-20261020-101010-01"});
        Check(c.session == 0 && c.older == 1, "pending: an earlier session's mark is not this session's");
        // This session's press, with the older folder still there.
        c = CountPendingMarks({"mark-20261029-120000-01"}, {"mark-20261020-101010-01", "mark-20261029-120000-01"});
        Check(c.session == 1 && c.older == 1, "pending: one pressed now, one older");
        // Two pressed now; the first id of a session can share its number with
        // an older folder, never its whole name (the time is in it).
        c = CountPendingMarks({"mark-20261029-120000-01", "mark-20261029-120005-02"},
            {"mark-20261020-101010-01", "mark-20261029-120000-01", "mark-20261029-120005-02"});
        Check(c.session == 2 && c.older == 1, "pending: two pressed now");
        // The report moved them: gone from disk, so not pending even before
        // the session's list is cleared.
        c = CountPendingMarks({"mark-20261029-120000-01"}, none);
        Check(c.session == 0 && c.older == 0, "pending: the report took the marks");
        // A press whose folder could not be made is not waiting either.
        c = CountPendingMarks({"mark-20261029-120000-01"}, {"mark-20261020-101010-01"});
        Check(c.session == 0 && c.older == 1, "pending: a press without a folder");
    }

    void TestNames()
    {
        Check(TimeText(62345) == "1:02.345", "time text");
        Check(MarkFolderName(7, 2026, 10, 5, 21, 3, 4) == "mark-20261005-210304-07", "mark folder");
        Check(ClipStem("042-Valentine.rpclip") == "042-Valentine", "clip stem");
        Check(SafeStem("a b/c..d") == "a_b_c__d", "safe stem");
        Check(ZipName("report-20261005-210304", "042-Valentine.rpclip") == "report-20261005-210304-042-Valentine.zip", "zip name");
        Check(ZipName("report-20261005-210304", "") == "report-20261005-210304.zip", "zip name without a clip");
        Check(NvidiaDriverVersion("32.0.15.6094") == "560.94", "nvidia 560.94");
        Check(NvidiaDriverVersion("31.0.15.3623") == "536.23", "nvidia 536.23");
        Check(NvidiaDriverVersion("32.0.15.7602") == "576.02", "nvidia 576.02");
        Check(NvidiaDriverVersion("31.0.101.5186").size() > 0, "an Intel version is still four numbers");
        Check(NvidiaDriverVersion("abc").empty() && NvidiaDriverVersion("1.2.3").empty(), "nvidia: not a version");
        Check(GameBuildMatches("1.0.1491.50") == 1, "game build 1491.50");
        Check(GameBuildMatches("1.0.1491.16") == 0, "another game build");
        Check(GameBuildMatches("none") == -1 && GameBuildMatches("1.0.1491") == -1, "unreadable game build");
        Check(ListedModFile("ScriptHookRDR2.dll") && ListedModFile("RedemptionEditor.asi") && ListedModFile("ReplayProbe.asi") && ListedModFile("dinput8.dll") &&
                ListedModFile("RedemptionEditor.addon64") && ListedModFile("ReplayEditor.addon64"),
            "mods listed");
        Check(!ListedModFile("bink2w64.dll") && !ListedModFile("oo2core_5_win64.dll") && !ListedModFile("steam_api64.dll") &&
                !ListedModFile("readme.txt"),
            "the game's own files are not");
        // The loaded mod environment line.
        const std::string env = EnvModsLine({"vfs.asi", "scripts\\Rampage.asi"}, {"graphics-hook64.dll"}, true, false, true);
        Check(env == "ENV_MODS,count=2,outside_count=1,vfs=1,lml=0,mod_manager=1,mods=vfs.asi|scripts\\Rampage.asi,"
                     "outside=graphics-hook64.dll",
            "env mods: one line, the loaded mods and the overlays");
        Check(EnvModsLine({}, {}, false, false, false) == "ENV_MODS,count=0,outside_count=0,vfs=0,lml=0,mod_manager=0,mods=,outside=",
            "env mods: none");
        Check(replay::log::EssentialLine(env + "\n"), "env mods: the line survives Debug=0");
        // The live query runs in the test process too (no game folder mods there).
        Check(replay::qa::bundle::EnvModsLine().rfind("ENV_MODS,count=", 0) == 0, "env mods: the live query");
    }

    void TestZip()
    {
        using namespace replay::qa::zip;
        const char* digits = "123456789";
        Check(Crc32Update(0, reinterpret_cast<const std::uint8_t*>(digits), 9) == 0xCBF43926u, "crc32 check value");
        std::uint16_t date = 0, time = 0;
        DosDateTime(2026, 10, 5, 21, 3, 4, date, time);
        Check(date == (((2026 - 1980) << 9) | (10 << 5) | 5) && time == ((21 << 11) | (3 << 5) | 2), "dos date and time");

        MemorySink sink;
        {
            Writer writer(sink);
            const std::string a = "hello report\r\n";
            Check(writer.Add("report-x\\report.txt", a.data(), a.size(), true, date, time), "zip: stored entry (no zlib)");
            Check(writer.Begin("report-x/marks/m1/mark.json", false, date, time), "zip: streamed entry");
            const std::string b = "{\"id\": 1}";
            Check(writer.Write(reinterpret_cast<const std::uint8_t*>(b.data()), 4) &&
                    writer.Write(reinterpret_cast<const std::uint8_t*>(b.data() + 4), b.size() - 4) && writer.End(),
                "zip: chunks");
            Check(writer.Add("report-x/empty.txt", "", 0, false, date, time), "zip: empty entry");
            Check(writer.Finish() && writer.Error().empty(), "zip: finish");
        }
        std::vector<ListedEntry> entries;
        Check(ListArchive(sink.bytes.data(), sink.bytes.size(), entries) && entries.size() == 3, "zip: the central directory reads back");
        if (entries.size() == 3)
        {
            Check(entries[0].name == "report-x/report.txt", "zip: backslashes become slashes");
            Check(entries[0].method == 0 && entries[0].raw == 14 && entries[0].packed == 14, "zip: stored sizes");
            Check(entries[1].crc == Crc32Update(0, reinterpret_cast<const std::uint8_t*>("{\"id\": 1}"), 9), "zip: crc of a chunked entry");
            Check(entries[2].raw == 0, "zip: the empty entry");
        }
        {
            MemorySink bad;
            Writer writer(bad);
            Check(!writer.Write(reinterpret_cast<const std::uint8_t*>("x"), 1) && writer.Error() == "no_entry", "zip: write without an entry fails");
        }
        // Files for a check with another reader (python -m zipfile -t).
        const std::string folder = Env("REPLAYPOC_QA_ZIP_OUT", "");
        if (!folder.empty() && GetFileAttributesA(folder.c_str()) != INVALID_FILE_ATTRIBUTES)
            WriteBytes(folder + "\\qa-zip-test-stored.zip", sink.bytes);
        // Deflate with the staged zlib (the video export's z.dll), when there.
        const std::string zlibPath = Env("REPLAYPOC_ZLIB", "external\\ffmpeg\\installed\\x64-windows-release\\bin\\z.dll");
        HMODULE zlib = LoadLibraryExA(zlibPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!zlib)
        {
            std::printf("qa kit: zlib not found (%s), deflate test skipped\n", zlibPath.c_str());
            return;
        }
        ZlibApi api;
        api.deflateInit2_ = reinterpret_cast<decltype(api.deflateInit2_)>(GetProcAddress(zlib, "deflateInit2_"));
        api.deflate = reinterpret_cast<decltype(api.deflate)>(GetProcAddress(zlib, "deflate"));
        api.deflateEnd = reinterpret_cast<decltype(api.deflateEnd)>(GetProcAddress(zlib, "deflateEnd"));
        Check(api.Ready(), "zlib exports");
        if (api.Ready())
        {
            std::string log;
            for (int n = 0; n < 20000; ++n)
                log += "POSE_V2_PLAYBACK_SOUND,ped=2:" + std::to_string(n % 50) + ",kind=0,status=queued,playhead_ms=" + std::to_string(n * 33) + "\n";
            std::vector<std::uint8_t> big(3u << 20);
            std::uint32_t seed = 1;
            for (std::uint8_t& byte : big)
            {
                seed = seed * 1664525u + 1013904223u;
                byte = static_cast<std::uint8_t>(seed >> 24);
            }
            MemorySink packed;
            Writer writer(packed, &api, 6);
            const auto t0 = std::chrono::steady_clock::now();
            Check(writer.Add("r/RedemptionEditor.log", log.data(), log.size(), true, date, time), "zip: deflated entry");
            Check(writer.Add("r/random.bin", big.data(), big.size(), true, date, time), "zip: deflated incompressible entry");
            Check(writer.Add("r/stored.jpg", big.data(), 1000, false, date, time), "zip: stored beside deflated");
            Check(writer.Finish() && writer.Deflated() == 2, "zip: deflated finish");
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::vector<ListedEntry> list;
            Check(ListArchive(packed.bytes.data(), packed.bytes.size(), list) && list.size() == 3, "zip: deflated reads back");
            if (list.size() == 3)
            {
                Check(list[0].method == 8 && list[0].packed < list[0].raw / 5 && list[0].raw == log.size(), "zip: the log shrinks");
                Check(list[0].crc == Crc32Update(0, reinterpret_cast<const std::uint8_t*>(log.data()), log.size()), "zip: crc of the raw data");
                Check(list[2].method == 0, "zip: the jpg is stored");
            }
            std::printf("qa kit: zip deflate %.1f MB in %.0f ms (log %zu -> %u bytes)\n", (log.size() + big.size()) / 1048576.0, ms,
                log.size(), list.empty() ? 0u : list[0].packed);
            if (GetFileAttributesA(folder.c_str()) != INVALID_FILE_ATTRIBUTES)
                WriteBytes(folder + "\\qa-zip-test-deflate.zip", packed.bytes);
        }
        FreeLibrary(zlib);
    }

    void TestImage()
    {
        using namespace replay::qa::image;
        std::uint32_t w = 0, h = 0;
        RingSize(3840, 2160, 480, w, h);
        Check(w == 852 && h == 480, "ring size 4K (853.3 rounded, even)");
        RingSize(2560, 1080, 480, w, h);
        Check(w == 1138 && h == 480, "ring size ultrawide");
        RingSize(1280, 401, 480, w, h);
        Check(w == 1280 && h == 400, "ring size reduced: even, not scaled up");
        // One pixel of each kind: red 255, green 128, blue 0.
        std::uint8_t rgba[4] = {255, 128, 0, 255}, bgra[4] = {0, 128, 255, 255};
        std::uint32_t rgb10 = 1023u | (514u << 10) | (0u << 20);
        std::uint8_t r10[4];
        std::memcpy(r10, &rgb10, 4);
        std::uint16_t half[4] = {0x3C00, 0x3800, 0x0000, 0x3C00}; // 1.0, 0.5, 0
        std::vector<std::uint8_t> out;
        ToBgr(rgba, 4, 1, 1, Pixel::Rgba8, 1, 1, out);
        Check(out[0] == 0 && out[1] == 128 && out[2] == 255, "rgba8 to bgr");
        ToBgr(bgra, 4, 1, 1, Pixel::Bgra8, 1, 1, out);
        Check(out[0] == 0 && out[1] == 128 && out[2] == 255, "bgra8 to bgr");
        ToBgr(r10, 4, 1, 1, Pixel::Rgb10, 1, 1, out);
        Check(out[0] == 0 && out[1] == 128 && out[2] == 255, "rgb10 to bgr");
        ToBgr(reinterpret_cast<std::uint8_t*>(half), 8, 1, 1, Pixel::Half, 1, 1, out);
        Check(out[0] == 0 && out[1] == 128 && out[2] == 255, "half to bgr");
        // 4x4: left half white, right half black, reduced to 2x2.
        std::vector<std::uint8_t> board(4 * 4 * 4, 0);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 2; ++x)
                std::memset(&board[(y * 4 + x) * 4], 255, 4);
        ToBgr(board.data(), 16, 4, 4, Pixel::Bgra8, 2, 2, out);
        Check(out.size() == 12 && out[0] == 255 && out[3] == 0 && out[6] == 255 && out[9] == 0, "reduce keeps the halves");

        // Cost on this machine (the editor runs these on a worker thread).
        const std::uint32_t W = 3840, H = 2160;
        std::vector<std::uint8_t> frame(static_cast<std::size_t>(W) * H * 4);
        for (std::size_t n = 0; n < frame.size(); ++n)
            frame[n] = static_cast<std::uint8_t>((n * 2654435761u) >> 13);
        RingSize(W, H, 480, w, h);
        auto t0 = std::chrono::steady_clock::now();
        ToBgr(frame.data(), W * 4, W, H, Pixel::Bgra8, w, h, out);
        const double reduce = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::vector<std::uint8_t> reduced(static_cast<std::size_t>(w) * h * 4);
        for (std::size_t n = 0; n < reduced.size(); ++n)
            reduced[n] = frame[n];
        t0 = std::chrono::steady_clock::now();
        std::vector<std::uint8_t> smallBgr;
        ToBgr(reduced.data(), w * 4, w, h, Pixel::Bgra8, w, h, smallBgr);
        const double convert = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        t0 = std::chrono::steady_clock::now();
        std::vector<std::uint8_t> fullBgr;
        ToBgr(frame.data(), W * 4, W, H, Pixel::Bgra8, W, H, fullBgr);
        const double full = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("qa kit: cpu reduce 4K->%ux%u %.2f ms, convert %ux%u %.2f ms, convert 4K %.1f ms\n", w, h, reduce, w, h, convert, full);

        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        const std::wstring ringPath = std::wstring(temp) + L"replaypoc-qa-ring.jpg";
        const std::wstring fullPath = std::wstring(temp) + L"replaypoc-qa-full.jpg";
        std::string error;
        t0 = std::chrono::steady_clock::now();
        const bool ringOk = replay::qa::jpeg::WriteBgr(ringPath, out.data(), w, h, 0.8f, error);
        const double ringMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        Check(ringOk, "jpeg: ring picture written");
        t0 = std::chrono::steady_clock::now();
        const bool fullOk = replay::qa::jpeg::WriteBgr(fullPath, fullBgr.data(), W, H, 0.9f, error);
        const double fullMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        Check(fullOk, "jpeg: full picture written");
        WIN32_FILE_ATTRIBUTE_DATA a{}, b{};
        GetFileAttributesExW(ringPath.c_str(), GetFileExInfoStandard, &a);
        GetFileAttributesExW(fullPath.c_str(), GetFileExInfoStandard, &b);
        std::printf("qa kit: jpeg %ux%u %.1f ms (%lu bytes), 4K %.1f ms (%lu bytes)%s%s\n", w, h, ringMs, a.nFileSizeLow, fullMs, b.nFileSizeLow,
            error.empty() ? "" : " error=", error.c_str());
        DeleteFileW(ringPath.c_str());
        DeleteFileW(fullPath.c_str());
    }
}

namespace
{
    bool WriteFileText(const std::wstring& path, const std::string& text)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        DWORD wrote = 0;
        const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr) && wrote == text.size();
        CloseHandle(file);
        return ok;
    }

    // The report's extension end to end in a scratch game folder (the
    // working directory, as in the game): a mark folder, a plain clip, a
    // dump; the zip written, the folder removed, the mark moved.
    void TestBundle()
    {
        wchar_t temp[MAX_PATH]{}, before[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        GetCurrentDirectoryW(MAX_PATH, before);
        const std::wstring root = std::wstring(temp) + L"replaypoc-qa-bundle-" + std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(root.c_str(), nullptr);
        if (!SetCurrentDirectoryW(root.c_str()))
        {
            Check(false, "bundle: scratch folder");
            return;
        }
        CreateDirectoryW(L"ReplayReports", nullptr);
        CreateDirectoryW(L"ReplayReports\\marks", nullptr);
        CreateDirectoryW(L"ReplayReports\\marks\\mark-20261005-210304-01", nullptr);
        CreateDirectoryW(L"ReplayReports\\report-20261005-210400", nullptr);
        CreateDirectoryW(L"ReplayClips", nullptr);
        CreateDirectoryW(L"crashes", nullptr);
        Mark mark;
        mark.id = 1;
        mark.folder = "mark-20261005-210304-01";
        mark.clip = "042-Valentine.rpclip";
        Check(WriteFileText(L"ReplayReports\\marks\\mark-20261005-210304-01\\mark.json", MarkJson(mark)), "bundle: mark.json");
        Check(WriteFileText(L"ReplayReports\\marks\\mark-20261005-210304-01\\ring-00_t-3.5s.jpg", std::string("\xff\xd8\xff\xe0", 4)), "bundle: jpg");
        Check(WriteFileText(L"ReplayReports\\report-20261005-210400\\RedemptionEditor.log", std::string(200000, 'L')), "bundle: log");
        std::string clip = "RPOCCLIP";
        for (int n = 0; n < 50000; ++n)
            clip += "pose" + std::to_string(n % 97);
        Check(WriteFileText(L"ReplayClips\\042-Valentine.rpclip", clip), "bundle: clip");
        Check(WriteFileText(L"crashes\\crash-1.dmp.txt", "exception 0xC0000005\n"), "bundle: dump");
        // The clip's sound reports and the report.txt marks line.
        CreateDirectoryW(L"ReplayDebug", nullptr);
        Check(WriteFileText(L"ReplayDebug\\042-Valentine.sound-check.txt", "== generic sound packets (SoundGeneric=1) ==\n"), "bundle: sound-check");
        Check(WriteFileText(L"ReplayReports\\report-20261005-210400\\report.txt", "Redemption Editor QA report\r\nversion: test\r\nmarks: none\r\n\r\nfiles:\r\n"),
            "bundle: report.txt");

        replay::qa::bundle::Input input;
        input.reportFolder = L"ReplayReports\\report-20261005-210400";
        input.reportName = "report-20261005-210400";
        input.clips = {"042-Valentine.rpclip", "missing.rpclip", "..\\evil.rpclip"};
        input.zipClip = "042-Valentine.rpclip";
        input.crashFolder = root + L"\\crashes";
        input.asiVersion = "test";
        input.reportText = "Redemption Editor QA report\r\nversion: test\r\nmarks: none\r\n\r\nfiles:\r\n";
        std::atomic<bool> cancel{false};
        input.cancel = &cancel;
        replay::qa::bundle::Result result;
        const auto t0 = std::chrono::steady_clock::now();
        replay::qa::bundle::Extend(input, result);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        Check(result.error.empty() && result.zipError.empty(), "bundle: no error");
        Check(result.zipped && result.zipPath == L"ReplayReports\\report-20261005-210400-042-Valentine.zip", "bundle: the zip's name");
        Check(result.marks == 1 && result.clips == 1 && result.clipsCompressed == 1 && result.dumps == 1, "bundle: marks, clip, dump");
        Check(result.markFolders.size() == 1 && result.markFolders[0] == "mark-20261005-210304-01" && result.debugFiles == 1,
            "bundle: the mark folders by name, the sound report copied");
        Check(GetFileAttributesW(L"ReplayReports\\report-20261005-210400") == INVALID_FILE_ATTRIBUTES, "bundle: the folder goes once zipped");
        Check(GetFileAttributesW(L"ReplayReports\\marks\\mark-20261005-210304-01") == INVALID_FILE_ATTRIBUTES, "bundle: the mark moved");
        std::vector<std::uint8_t> zipBytes;
        {
            HANDLE file = CreateFileW(result.zipPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                zipBytes.resize(GetFileSize(file, nullptr));
                DWORD read = 0;
                ReadFile(file, zipBytes.data(), static_cast<DWORD>(zipBytes.size()), &read, nullptr);
                CloseHandle(file);
            }
        }
        std::vector<zip::ListedEntry> entries;
        Check(zip::ListArchive(zipBytes.data(), zipBytes.size(), entries), "bundle: the zip reads back");
        bool hasMark = false, hasClip = false, hasSys = false, hasMarks = false, hasDump = false, hasDebug = false, hasReport = false;
        for (const zip::ListedEntry& entry : entries)
        {
            hasDebug = hasDebug || entry.name == "report-20261005-210400/debug/042-Valentine.sound-check.txt";
            hasReport = hasReport || entry.name == "report-20261005-210400/report.txt";
            hasMark = hasMark || entry.name == "report-20261005-210400/marks/mark-20261005-210304-01/mark.json";
            hasMarks = hasMarks || entry.name == "report-20261005-210400/marks.json";
            hasSys = hasSys || entry.name == "report-20261005-210400/sysinfo.txt";
            hasDump = hasDump || entry.name == "report-20261005-210400/crashes/crash-1.dmp.txt";
            if (entry.name == "report-20261005-210400/clips/042-Valentine.rpclip")
            {
                hasClip = true;
                Check(entry.method == 0 && entry.raw < clip.size(), "bundle: the clip compressed (zstd), stored in the zip");
            }
        }
        Check(hasMark && hasMarks && hasSys && hasClip && hasDump, "bundle: every part in the zip");
        Check(hasDebug && hasReport, "bundle: the debug report and report.txt in the zip");
        std::printf("qa kit: bundle %zu entries, %.1f KB, zlib=%s, %.0f ms, game exe %s\n", entries.size(), zipBytes.size() / 1024.0,
            result.zlib.c_str(), ms, result.gameExeVersion.c_str());
        const std::string folder = Env("REPLAYPOC_QA_ZIP_OUT", "");
        if (!folder.empty() && GetFileAttributesA(folder.c_str()) != INVALID_FILE_ATTRIBUTES)
            WriteBytes(folder + "\\qa-bundle-test.zip", zipBytes);
        DeleteFileW(result.zipPath.c_str());
        DeleteFileW(L"ReplayClips\\042-Valentine.rpclip");
        DeleteFileW(L"crashes\\crash-1.dmp.txt");
        DeleteFileW(L"ReplayDebug\\042-Valentine.sound-check.txt");
        RemoveDirectoryW(L"ReplayDebug");
        RemoveDirectoryW(L"ReplayClips");
        RemoveDirectoryW(L"crashes");
        RemoveDirectoryW(L"ReplayReports\\marks");
        RemoveDirectoryW(L"ReplayReports");
        SetCurrentDirectoryW(before);
        RemoveDirectoryW(root.c_str());
    }
}

int RunQaKitTests()
{
    TestBundle();
    TestField();
    TestClassify();
    TestRing();
    TestWindow();
    TestMarkJson();
    TestMarksLine();
    TestNames();
    TestPendingMarks();
    TestZip();
    TestImage();
    if (g_qaFailures == 0)
        std::puts("qa kit tests: ok");
    return g_qaFailures;
}
