#pragma once

// QA kit: the rolling health buffer.
//
// While a clip plays, the anomalies the playback already reports in its log
// lines (the missing puppet detector, the pose arm, the pop-in judge, the
// jerk detector, the speech, vocal, sound and fx players) are also kept in
// a small ring with their playhead, wall time and entity, whatever the
// Debug setting: the log tap (script.cpp, QuietFilter) hands every line to
// ClassifyLine before the Debug=0 filter drops it. Frame hitches come from
// the script tick. A "mark problem" (Shift+F2) copies the anomalies of its
// window (8 s of playhead before the press, 2 s after) into the mark, and
// each playback ends with one QA_HEALTH_SUMMARY line (counts per kind).
//
// Pure: no engine access; unit tested in ReplayCoreTests (qa_kit_tests.cpp).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace replay::qa
{
    // The mark's window, in playhead milliseconds around the press.
    inline constexpr std::uint32_t kBeforeMs = 8000;
    inline constexpr std::uint32_t kAfterMs = 2000;
    // The ring keeps the playback's last kRingCapacity anomalies, whatever
    // their age: a mark pressed after a scrub still finds the pass that
    // played its window.
    inline constexpr std::size_t kRingCapacity = 2048;
    // A script tick longer than this while playing is a hitch.
    inline constexpr std::uint32_t kHitchMs = 100;

    enum class Kind : std::uint8_t
    {
        PuppetHidden = 0,   // PUPPET_MISSING (model loading, create wait, dressing, failed, no pose, invisible...)
        PuppetUnposed,      // PUPPET_MISSING reason=unposed, POSE_V2_UNPOSED_HIDDEN (an episode past the rule)
        ArmRefused,         // POSE_V2_PLAYBACK_ARM mode=refused, POSE_V2_ARM_REFUSED_HIDDEN, PUPPET_MISSING reason=arm_refused
        ArmMapped,          // POSE_V2_PLAYBACK_ARM mode=mapped (posed through the skeleton map: worth a look)
        PuppetGone,         // PUPPET_MISSING reason=no_puppet: alive in the clip, no puppet (a despawn pop)
        PopIn,              // PUPPET_POP_IN: shown late, close to the camera (a spawn pop)
        CreateRefused,      // POSE_V2_CREATE_REFUSED: the engine refused the puppet
        PoseJump,           // POSE_JERK: a big jump of a body between two ticks
        SpeechRefused,      // POSE_V2_PLAYBACK_SPEECH status refused or mismatch
        AnimalVocalMissing, // POSE_V2_ANIMAL_VOCAL created=0
        SoundRefused,       // POSE_V2_PLAYBACK_SOUND with a failure status (footsteps, impacts, weapons)
        FxRefused,          // fx and fire the playback could not place
        Hitch,              // a script tick over kHitchMs while playing
        // Sound and pose checks.
        GenericRefused,     // a generic sound packet (SGEN) the engine refused in the mark's window
        GenericMissed,      // a generic packet whose window went by (no puppet, no slot)
        GenericDouble,      // a generic packet played while another source made the same sound
        EntitySoundRefused, // an entity sound row (ESND) the engine refused (POSE_V2_ENTITY_SOUND status, or the window)
        EntitySoundSilent,  // ESND rows of one owner in the window: none played, none the puppet's, none refused
        PoseOverwritten,    // POSE_V2_PLAYBACK_WRITE_CHECK state=changed: the engine moved bones after the pose was written
        // Checks made at the press on the bodies in view (qa_kit.cpp
        // CollectBodies), recorded against applied.
        AppearanceMismatch, // a body's recorded metaped layers not all on its puppet, or the armed skeleton is not the segment's
        PropMisplaced,      // a dropped prop (hat, weapon, piece) of a body in view not built, or not where its track puts it
        SpeechSilent,       // POSE_V2_SPEECH_SILENT: a line the replay created that never became audible
        // music_stop.h: the game's score still on past the fade of the replay's
        // music stop (MUSIC_STOP phase=restop).
        MusicInReplay,
        // A world rope (not the replay's: the reins of the player's real horse)
        // drawn near the camera during the playback, after the world clear hid it.
        RopeStray,          // POSE_V2_ROPE_STRAY
        // skeleton_signature.h: a body's skeleton with the record's bone count
        // and other parents (groups in another order: the hat above the head,
        // the face stretched), armed through the skeleton map or refused.
        SkeletonSignatureMismatch, // POSE_V2_SKELETON_SIGNATURE verdict=differ
        // puppet_lod.h: a shown puppet on screen whose pose was not written for
        // 2 ticks in a row while playing (the engine's animation LOD; no native
        // tells the draw LOD itself).
        PuppetLowLod,       // POSE_V2_PUPPET_LOW_LOD
        // metaped_state.h NotRecordedWhy: a body whose puppet had nothing
        // recorded to wear (no layer and no outfit at its entry or within the
        // dressing lookahead), or, in a clip with the metaped section, no metaped
        // look for it.
        AppearanceNotRecorded, // POSE_V2_APPEARANCE_NOT_RECORDED
        // speech_resume.h Cut: a replayed line that was audible and left before
        // its end, not stopped by the replay.
        SpeechCut,          // POSE_V2_SPEECH_CUT
        // object_check.h: a world object, scenery object or weapon object the
        // playback shows wrong: the original drawn next to the replay's copy, a
        // scenery stand-in missing, a weapon object as its bare model, a break
        // never played, an animated object recorded without its pose.
        ObjectWrong,        // POSE_V2_OBJECT_CHECK
        // tpose_check.h: a shown body drawn in its model's bind pose (the T-pose
        // watch), or a never animated record shown because its bind pose could
        // not be read.
        TPose,              // POSE_V2_TPOSE, POSE_V2_NEVER_ANIMATED verdict=show_unproven
        // A finding of the fx check at the playback's stop (recreate_loop,
        // relocate_every_frame, motion_lost, restart_mismatch, dup_world,
        // owner_late, blink, settings_diff, weapon_shared, weapon_on_body,
        // placement_far).
        FxCheckFlag,        // FX_CHECK_FLAG
        Count,
    };

    inline const char* KindText(Kind kind)
    {
        switch (kind)
        {
        case Kind::PuppetHidden: return "puppet_hidden";
        case Kind::PuppetUnposed: return "puppet_unposed";
        case Kind::ArmRefused: return "arm_refused";
        case Kind::ArmMapped: return "arm_mapped";
        case Kind::PuppetGone: return "puppet_gone";
        case Kind::PopIn: return "pop_in";
        case Kind::CreateRefused: return "create_refused";
        case Kind::PoseJump: return "pose_jump";
        case Kind::SpeechRefused: return "speech_refused";
        case Kind::AnimalVocalMissing: return "animal_vocal_missing";
        case Kind::SoundRefused: return "sound_refused";
        case Kind::FxRefused: return "fx_refused";
        case Kind::Hitch: return "hitch";
        case Kind::GenericRefused: return "generic_refused";
        case Kind::GenericMissed: return "generic_missed";
        case Kind::GenericDouble: return "generic_double";
        case Kind::EntitySoundRefused: return "entity_sound_refused";
        case Kind::EntitySoundSilent: return "entity_sound_silent";
        case Kind::PoseOverwritten: return "pose_overwritten";
        case Kind::AppearanceMismatch: return "appearance_mismatch";
        case Kind::PropMisplaced: return "prop_misplaced";
        case Kind::SpeechSilent: return "speech_silent";
        case Kind::MusicInReplay: return "music_in_replay";
        case Kind::RopeStray: return "rope_stray";
        case Kind::SkeletonSignatureMismatch: return "skeleton_signature_mismatch";
        case Kind::PuppetLowLod: return "puppet_low_lod";
        case Kind::AppearanceNotRecorded: return "appearance_not_recorded";
        case Kind::SpeechCut: return "speech_cut";
        case Kind::ObjectWrong: return "object_wrong";
        case Kind::TPose: return "tpose";
        case Kind::FxCheckFlag: return "fx_check_flag";
        default: return "?";
        }
    }

    struct Anomaly
    {
        std::uint64_t wallMs = 0;     // GetTickCount64 when seen
        std::uint32_t playheadMs = 0;
        // The pass over the clip it was seen in (qa_kit.cpp: one more at every
        // jump back or forward). A mark pressed paused after a scrub can have
        // its window's last pass a minute old, so windows are chosen by pass,
        // not by wall time.
        std::uint32_t pass = 0;
        Kind kind = Kind::PuppetHidden;
        std::string entity;           // "slot:instance" as the log prints it ("" none)
        std::string detail;           // short "key=value;key=value"
    };

    // The value of `key` in a "TAG,a=1,b=2" line ("" when absent). The key
    // must follow a ',' (the tag is never a key).
    inline std::string_view Field(std::string_view line, std::string_view key)
    {
        std::size_t from = 0;
        while (true)
        {
            const std::size_t at = line.find(key, from);
            if (at == std::string_view::npos)
                return {};
            const std::size_t end = at + key.size();
            if (at > 0 && line[at - 1] == ',' && end < line.size() && line[end] == '=')
            {
                std::size_t stop = line.find_first_of(",\r\n", end + 1);
                if (stop == std::string_view::npos)
                    stop = line.size();
                return line.substr(end + 1, stop - end - 1);
            }
            from = at + 1;
        }
    }

    inline bool ParseU32(std::string_view text, std::uint32_t& value)
    {
        if (text.empty() || text.size() > 10)
            return false;
        std::uint64_t v = 0;
        for (const char c : text)
        {
            if (c < '0' || c > '9')
                return false;
            v = v * 10 + static_cast<std::uint64_t>(c - '0');
        }
        if (v > 0xFFFFFFFFull)
            return false;
        value = static_cast<std::uint32_t>(v);
        return true;
    }

    inline bool StartsWith(std::string_view text, std::string_view prefix)
    {
        return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
    }

    // A failure status of the sound player (audio_observer.cpp and the
    // footstep, impact and weapon players): anything that says the sound
    // did not go to the engine. "queued", "duplicate", "already" are not.
    inline bool SoundStatusFailed(std::string_view status)
    {
        static constexpr std::string_view kFailed[] = {"no_", "not_", "bad_", "write_fault", "vtable_", "too_far", "slot_other",
            "signature_", "shooter_gone", "queue_full", "owner_other", "hash_mismatch", "foot_vtable", "neighbor_ped_audio",
            "refused", "fail"};
        for (const std::string_view prefix : kFailed)
        {
            if (StartsWith(status, prefix))
                return true;
        }
        return status.find("audio_unregistered") != std::string_view::npos || status.find("fail") != std::string_view::npos;
    }

    // One log line of the playback to an anomaly. `playheadNow` is used when
    // the line has no playhead_ms. False for every other line (the caller
    // fills wallMs).
    inline bool ClassifyLine(std::string_view line, std::uint32_t playheadNow, Anomaly& out)
    {
        if (line.empty() || line[0] < 'A' || line[0] > 'Z')
            return false;
        const std::size_t comma = line.find(',');
        if (comma == std::string_view::npos)
            return false;
        const std::string_view tag = line.substr(0, comma);
        const auto fill = [&](Kind kind, std::string_view entityKey, std::string detail) {
            out.kind = kind;
            out.entity = std::string(Field(line, entityKey));
            out.detail = std::move(detail);
            std::uint32_t playhead = playheadNow;
            if (!ParseU32(Field(line, "playhead_ms"), playhead))
                playhead = playheadNow;
            out.playheadMs = playhead;
            return true;
        };
        const auto pair = [&](std::string_view key) {
            const std::string_view value = Field(line, key);
            return value.empty() ? std::string() : std::string(key) + "=" + std::string(value);
        };
        const auto join = [](std::string a, const std::string& b) {
            if (a.empty())
                return b;
            return b.empty() ? a : a + ";" + b;
        };
        if (tag == "PUPPET_MISSING")
        {
            const std::string_view reason = Field(line, "reason");
            const Kind kind = reason == "unposed" ? Kind::PuppetUnposed
                : reason == "arm_refused"         ? Kind::ArmRefused
                : reason == "no_puppet"           ? Kind::PuppetGone
                                                  : Kind::PuppetHidden;
            return fill(kind, "id", join(join(join(pair("reason"), pair("kind")), pair("model")), pair("camera_m")));
        }
        // One line per episode past the rule (puppet_watch.h UnposedRule).
        if (tag == "POSE_V2_UNPOSED_HIDDEN")
            return fill(Kind::PuppetUnposed, "id",
                join(join(join(join("hidden_unposed", pair("cause")), pair("ms")), pair("ticks")), pair("on_screen")));
        if (tag == "POSE_V2_PLAYBACK_ARM")
        {
            const std::string_view mode = Field(line, "mode");
            if (mode == "refused")
                return fill(Kind::ArmRefused, "id", join(join(pair("error"), pair("clone_bones")), pair("track_bones")));
            if (mode == "mapped")
                return fill(Kind::ArmMapped, "id", join(join(join(pair("matched"), pair("rest")), pair("dropped")), pair("clone_bones")));
            return false;
        }
        if (tag == "POSE_V2_ARM_REFUSED_HIDDEN")
            return fill(Kind::ArmRefused, "id", join(join(join(pair("kind"), pair("model")), pair("error")), "hidden=1"));
        if (tag == "PUPPET_POP_IN")
            return fill(Kind::PopIn, "id", join(join(join(pair("kind"), pair("model")), pair("camera_m")), pair("on_screen")));
        if (tag == "POSE_V2_CREATE_REFUSED")
            return fill(Kind::CreateRefused, "id", join(join(pair("kind"), pair("model")), pair("attempts")));
        if (tag == "POSE_JERK")
            return fill(Kind::PoseJump, "id", join(join(join(pair("kind"), pair("magnitude_m")), pair("causes")), pair("lost_ms")));
        if (tag == "POSE_V2_PLAYBACK_SPEECH")
        {
            const std::string_view status = Field(line, "status");
            if (!StartsWith(status, "refused") && !StartsWith(status, "mismatch"))
                return false;
            const std::size_t colon = status.find(':', status.find(':') + 1);
            return fill(Kind::SpeechRefused, "ped",
                join(join("status=" + std::string(status.substr(0, colon)), pair("context")), pair("voice")));
        }
        // Accepted and created, then killed while loading or never started
        // (speech_identity.h JudgeLine).
        if (tag == "POSE_V2_SPEECH_SILENT")
            return fill(Kind::SpeechSilent, "ped",
                join(join(join(join(join(join(join(join(pair("kind"), pair("why")), pair("cause")), pair("voice_status")),
                    pair("path")), pair("stop_reason")), pair("stop_state")), pair("context")), pair("voice")));
        // An audible line cut before its end.
        if (tag == "POSE_V2_SPEECH_CUT")
            return fill(Kind::SpeechCut, "ped",
                join(join(join(join(join(pair("kind"), pair("reason")), pair("heard_ms")), pair("expected_ms")),
                    pair("path")), pair("context")));
        if (tag == "POSE_V2_ANIMAL_VOCAL")
        {
            if (Field(line, "created") != "0")
                return false;
            return fill(Kind::AnimalVocalMissing, "ped", join(join(pair("context"), pair("status")), pair("voice")));
        }
        if (tag == "POSE_V2_PLAYBACK_SOUND")
        {
            const std::string_view status = Field(line, "status");
            if (!SoundStatusFailed(status))
                return false;
            return fill(Kind::SoundRefused, "ped", join(join(pair("kind"), pair("ref")), pair("status")));
        }
        // The entity sound track's per-row line (the first 40 of a playback)
        // with a failure status; "too_far" is not heard there, not a refusal
        // (replay_events.cpp).
        if (tag == "POSE_V2_ENTITY_SOUND")
        {
            if (Field(line, "phase") != "play")
                return false;
            const std::string_view status = Field(line, "status");
            if (status == "too_far" || !SoundStatusFailed(status))
                return false;
            return fill(Kind::EntitySoundRefused, "id", join(join(pair("ref"), pair("status")), pair("event_ms")));
        }
        // The end-of-frame check of the written pose (replay_playback.cpp
        // CheckPoseV2Write): bones the engine moved after the pose was applied
        // (a prop or layer the engine re-poses).
        if (tag == "POSE_V2_PLAYBACK_WRITE_CHECK")
        {
            if (Field(line, "state") != "changed")
                return false;
            return fill(Kind::PoseOverwritten, "id",
                join(join(join(join(pair("object_bones"), pair("local_bones")), pair("first_bone")), pair("last_bone")), pair("max_m")));
        }
        // A world rope drawn near the camera after the world clear hid it
        // (replay_playback.cpp HidePoseV2WorldRopes); the entity is the engine
        // rope id.
        // The arm or a recheck found another skeleton (replay_playback.cpp
        // GatePoseV2Signature, RecheckPoseV2Signature).
        if (tag == "POSE_V2_SKELETON_SIGNATURE")
        {
            if (Field(line, "verdict") != "differ")
                return false;
            return fill(Kind::SkeletonSignatureMismatch, "id",
                join(join(join(join(join(join(pair("phase"), pair("action")), pair("differ")), pair("first_bone")),
                    pair("last_bone")), pair("bones")), pair("model")));
        }
        if (tag == "POSE_V2_APPEARANCE_NOT_RECORDED")
            return fill(Kind::AppearanceNotRecorded, "id",
                join(join(join(join(pair("why"), pair("kind")), pair("model")), pair("create_layers")), pair("dressed_from")));
        if (tag == "POSE_V2_PUPPET_LOW_LOD")
            return fill(Kind::PuppetLowLod, "id", join(join(join(pair("kind"), pair("camera_m")), pair("stale_ticks")), pair("forced")));
        if (tag == "POSE_V2_OBJECT_CHECK")
            return fill(Kind::ObjectWrong, "id", join(join(join(pair("verdict"), pair("why")), pair("model")), pair("phase")));
        if (tag == "POSE_V2_TPOSE")
            return fill(Kind::TPose, "id", join(join(join(join(pair("kind"), pair("posed")), pair("compared")), pair("never_animated")),
                pair("on_screen")));
        if (tag == "FX_CHECK_FLAG")
            return fill(Kind::FxCheckFlag, "hash", join(join(pair("kind"), pair("value")), pair("limit")));
        if (tag == "POSE_V2_NEVER_ANIMATED")
        {
            if (Field(line, "verdict") != "show_unproven")
                return false;
            return fill(Kind::TPose, "id", join(join(join("never_animated=1", pair("kind")), pair("model")), pair("bind_error")));
        }
        if (tag == "POSE_V2_ROPE_STRAY")
            return fill(Kind::RopeStray, "engine_id", join(join(join(pair("type"), pair("camera_m")), pair("holder")), pair("entity")));
        if (tag == "POSE_V2_FX_REGISTERED_ATTACH_MISS" || tag == "POSE_V2_FX_NO_WORLD_POSITION" ||
            tag == "POSE_V2_FIRE_STATE_REFUSED" || tag == "POSE_V2_PLAYBACK_FIRE_SKIP")
        {
            const std::string_view entityKey = !Field(line, "id").empty() ? "id" : "entity_a";
            const bool ok = fill(Kind::FxRefused, entityKey,
                join(join(join("what=" + std::string(tag.substr(8)), pair("hash")), pair("reason")), pair("error")));
            if (out.entity == "none")
                out.entity.clear();
            return ok;
        }
        // Music the stop at the replay's entry did not silence.
        if (tag == "MUSIC_STOP")
        {
            if (Field(line, "phase") != "restop")
                return false;
            return fill(Kind::MusicInReplay, "", join(join(pair("why"), pair("t_ms")), pair("event_ok")));
        }
        return false;
    }

    struct Counts
    {
        std::uint32_t perKind[static_cast<int>(Kind::Count)] = {};
        std::uint32_t total = 0;
    };

    inline std::string CountsText(const Counts& counts)
    {
        std::string text;
        for (int kind = 0; kind < static_cast<int>(Kind::Count); ++kind)
        {
            text += text.empty() ? "" : ",";
            text += KindText(static_cast<Kind>(kind));
            text += '=';
            text += std::to_string(counts.perKind[kind]);
        }
        return text;
    }

    class HealthRing
    {
    public:
        explicit HealthRing(std::size_t capacity = kRingCapacity) : m_capacity(capacity ? capacity : 1) {}

        void Push(const Anomaly& anomaly)
        {
            if (m_entries.size() >= m_capacity)
            {
                m_entries.pop_front();
                ++m_overflowed;
            }
            m_entries.push_back(anomaly);
            const int kind = static_cast<int>(anomaly.kind);
            if (kind >= 0 && kind < static_cast<int>(Kind::Count))
                ++m_counts.perKind[kind];
            ++m_counts.total;
        }

        // Drops what is older than retainMs (wall).
        void Prune(std::uint64_t nowWall, std::uint64_t retainMs)
        {
            while (!m_entries.empty() && nowWall > m_entries.front().wallMs && nowWall - m_entries.front().wallMs > retainMs)
                m_entries.pop_front();
        }

        // The anomalies of a mark's window: playhead in [center - before,
        // center + after], from every pass still in the ring (the press often
        // comes paused, after a scrub, long after the pass that played the
        // window). The same line seen in two passes (same kind, entity,
        // playhead and detail) is kept once, from the latest pass; in playhead
        // order.
        std::vector<Anomaly> Window(std::uint32_t center, std::uint32_t before, std::uint32_t after) const
        {
            std::vector<Anomaly> out;
            const std::uint32_t from = center > before ? center - before : 0;
            const std::uint64_t to = static_cast<std::uint64_t>(center) + after;
            for (const Anomaly& anomaly : m_entries)
            {
                if (anomaly.playheadMs < from || anomaly.playheadMs > to)
                    continue;
                bool seen = false;
                for (Anomaly& kept : out)
                {
                    if (kept.kind != anomaly.kind || kept.playheadMs != anomaly.playheadMs || kept.entity != anomaly.entity ||
                        kept.detail != anomaly.detail)
                        continue;
                    seen = true;
                    if (anomaly.pass >= kept.pass)
                    {
                        kept.pass = anomaly.pass;
                        kept.wallMs = anomaly.wallMs;
                    }
                    break;
                }
                if (!seen)
                    out.push_back(anomaly);
            }
            std::stable_sort(out.begin(), out.end(), [](const Anomaly& a, const Anomaly& b) { return a.playheadMs < b.playheadMs; });
            return out;
        }

        // A playback starts: its counts from zero (the ring is cleared too).
        void Reset()
        {
            m_entries.clear();
            m_counts = {};
            m_overflowed = 0;
        }

        const Counts& Totals() const { return m_counts; }
        std::size_t Size() const { return m_entries.size(); }
        std::uint64_t Overflowed() const { return m_overflowed; }

    private:
        std::size_t m_capacity;
        std::deque<Anomaly> m_entries;
        Counts m_counts;
        std::uint64_t m_overflowed = 0;
    };

    // A mark waits for the 2 s after its press. Closed when the playhead
    // passed press + after, the playback stopped, or the wall time ran out
    // (paused, slowed down: after / rate + 1 s, between 2.5 s and 12 s).
    struct MarkWindow
    {
        std::uint32_t playheadMs = 0;
        std::uint64_t pressWall = 0;
        double rate = 1.0;
    };

    inline std::uint64_t MaxWaitMs(double rate)
    {
        const double r = rate < 0.05 ? 0.05 : rate;
        const double ms = kAfterMs / r + 1000.0;
        return static_cast<std::uint64_t>(ms < 2500.0 ? 2500.0 : (ms > 12000.0 ? 12000.0 : ms));
    }

    inline bool WindowClosed(const MarkWindow& window, std::uint64_t nowWall, std::uint64_t playheadMs, bool playing)
    {
        if (!playing)
            return true;
        if (playheadMs >= static_cast<std::uint64_t>(window.playheadMs) + kAfterMs)
            return true;
        return nowWall >= window.pressWall + MaxWaitMs(window.rate);
    }
}
