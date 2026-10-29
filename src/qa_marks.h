#pragma once

// QA kit: a "mark problem" (Shift+F2 during a
// replay playback) and what goes with it into the QA report.
//
//   ReplayReports\marks\mark-YYYYMMDD-HHMMSS-NN\
//     mark.json     this file's MarkJson: the playhead, the clip, the camera,
//                   the bodies near the camera, the anomalies of the window
//                   (qa_health.h: 8 s before, 2 s after) and the tester's note
//     ring-NN.jpg   the editor's small screenshots of the 4 s before and the
//                   2 s after the press (editor/qa_shots.h)
//     full.jpg      the frame at the press, full resolution
//
// The next report (Shift+F1) moves every mark folder into its own folder
// and zips it. Also here: the pure parts of the report's system info and
// file names.
//
// Pure: unit tested in ReplayCoreTests.

#include "qa_health.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace replay::qa
{
    inline constexpr const char* kMarksFolder = "ReplayReports\\marks";
    // The game build every offline address of the mod comes from.
    inline constexpr std::uint32_t kGameBuildMajor = 1491;
    inline constexpr std::uint32_t kGameBuildMinor = 50;

    struct CameraInfo
    {
        float position[3] = {};
        float rotation[3] = {}; // degrees, rotation order 2 (x pitch, y roll, z heading)
        float target[3] = {};   // 10 m ahead
        float fov = 0.0f;
        bool freeCam = false;
        bool editingMarker = false;
    };

    // A prop that came off
    // a body in view (a dropped hat, a weapon, a gore piece), recorded
    // against applied at the press: built or not, and how far its clone
    // stands from the last recorded transform placed on it.
    struct PropInView
    {
        std::string id;          // "slot:instance"
        // hat, weapon, gore_piece, projectile, world_object, world_piece,
        // vehicle_piece (a prop); held_weapon,
        // held_object (a weapon-kind puppet without a weapon: a note, a pelt
        // carried by a horse), or the kind of any other attached puppet.
        std::string origin;
        std::string kind;        // the puppet's kind: prop, weapon, ped, horse, vehicle...
        std::string link;        // owner (a prop that came off it), weapon_point, draft_harness, vehicle_seat
        std::uint32_t weaponHash = 0; // weapon-kind puppets: the weapon (0 a plain object)
        std::uint32_t model = 0;
        std::string state;       // loading, dressing, ready, failed
        bool clone = false;      // the clone exists in the engine
        bool knockRequested = false; // a hat: the owner puppet was asked to drop it
        float placeErrorM = -1.0f;   // clone against the last placed transform (-1 unknown)
    };

    struct BodyInView
    {
        std::string id;        // "slot:instance"
        std::string kind;      // ped, horse, vehicle...
        std::uint32_t model = 0;
        std::string label;     // "Arthur", "" otherwise
        float distanceM = 0.0f;
        bool onScreen = false;
        std::string state;     // loading, dressing, ready, failed
        int visible = -1;      // the puppet's visible state (-1 unknown)
        bool armed = false;    // pose override armed (bodies)
        bool mapped = false;   // armed through the skeleton map
        std::string armError;  // why an arm was refused ("" none)
        std::uint32_t boneCount = 0;    // the pose segment's bones at the playhead
        // Recorded against applied. The metaped layers of the
        // create packet found on the clone (entity_appearance.h Compare; a
        // ped only), the clone's own layer count, the skeleton armed and the
        // records (segment at the playhead against the one armed).
        bool layersChecked = false;
        std::uint32_t layersRecorded = 0;
        std::uint32_t layersPresent = 0;
        std::uint32_t pedLayers = 0;
        std::uint32_t armedBones = 0;
        std::uint32_t record = 0;
        std::uint32_t armedRecord = 0;
        std::vector<PropInView> props; // the props that came off this body
    };

    // What the sound
    // players did with the sounds of the mark's window (8 s before the
    // press, 2 s after), filled at the window's close from the generic
    // sound player (sound_generic_player_core.h Player::Window) and the
    // entity sound track (replay_events.h EntitySoundWindowOf). Pure data
    // here (the JSON); the counts are plain so this header stays free of
    // the players.
    struct SoundRow
    {
        std::string key;      // the family, or "owner family"
        std::string owner;    // "slot:instance" ("" for a family row)
        std::string family;
        std::string taker;    // who the owner table says plays it: generic, specific, puppet
        std::uint32_t packets = 0;
        std::uint32_t played = 0;
        std::uint32_t refused = 0;
        std::uint32_t yielded = 0;
        std::uint32_t missed = 0;
        std::uint32_t jumped = 0;
        std::uint32_t skipped = 0;   // left to the specific track or the puppet
        std::uint32_t notReached = 0;
        std::uint32_t doubles = 0;
    };

    struct SoundFailure
    {
        std::uint32_t timeMs = 0;
        std::string owner;
        std::string family;
        std::string taker;
        std::uint32_t set = 0;
        std::string outcome; // refused, missed, double
    };

    struct EntitySoundRow
    {
        std::string owner; // "" for the totals
        std::uint32_t rows = 0, played = 0, refused = 0, tooFar = 0, byPuppet = 0, passed = 0, untried = 0;
        std::uint32_t toGeneric = 0; // carried by a generic packet (the generic block reports them)
        std::string lastStatus;
    };

    struct SoundWindow
    {
        bool genericKnown = false;     // the generic player ran (SoundGeneric=1)
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
        std::uint32_t packets = 0;
        std::vector<SoundRow> families;
        std::vector<SoundRow> owners;
        std::vector<SoundFailure> failures;
        bool entityKnown = false;      // the entity sound track was read
        EntitySoundRow entityTotals;
        std::vector<EntitySoundRow> entityOwners;
    };

    struct Mark
    {
        std::uint32_t id = 0;
        std::string folder;        // mark-YYYYMMDD-HHMMSS-NN
        std::string wallTime;      // "YYYY-MM-DD HH:MM:SS.mmm" (local)
        std::uint64_t pressWall = 0;
        std::uint32_t playheadMs = 0;
        std::uint32_t durationMs = 0;
        double rate = 1.0;
        bool paused = false;
        bool scrubbing = false;
        bool editorOpen = false;
        std::string clip;          // file name in ReplayClips ("" when the clip is the buffer's, unsaved)
        std::string version;
        CameraInfo camera;
        std::vector<BodyInView> bodies;
        std::uint32_t pass = 0;    // the pass over the clip at the press (qa_health.h Anomaly::pass)
        std::vector<Anomaly> pressChecks; // what CollectBodies found at the press (merged into anomalies at the close)
        bool windowClosed = false;
        std::uint32_t windowEndPlayheadMs = 0;
        std::vector<Anomaly> anomalies;
        SoundWindow sounds;
        std::string note;
        bool noteSet = false;      // the tester answered (Enter), with or without text
        bool noteDone = false;     // the note box closed (Enter or Esc)
        std::uint32_t ringShots = 0;
        bool fullShot = false;
        std::string shotsError;
        bool shotsDone = false;
    };

    inline std::string JsonEscape(std::string_view text)
    {
        std::string out;
        out.reserve(text.size() + 2);
        for (const char c : text)
        {
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char hex[8];
                    std::snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += hex;
                }
                else
                    out += c;
            }
        }
        return out;
    }

    inline std::string JsonString(std::string_view text) { return "\"" + JsonEscape(text) + "\""; }

    inline std::string JsonFloat(float value, int decimals = 2)
    {
        if (!(value == value) || value > 1e9f || value < -1e9f)
            return "null";
        char text[48];
        std::snprintf(text, sizeof(text), "%.*f", decimals, static_cast<double>(value));
        return text;
    }

    // "1:02.345" (minutes:seconds.milliseconds).
    inline std::string TimeText(std::uint64_t ms)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%llu:%02llu.%03llu", static_cast<unsigned long long>(ms / 60000),
            static_cast<unsigned long long>((ms / 1000) % 60), static_cast<unsigned long long>(ms % 1000));
        return text;
    }

    inline std::string MarkFolderName(std::uint32_t id, unsigned year, unsigned month, unsigned day, unsigned hour, unsigned minute,
        unsigned second)
    {
        char text[48];
        std::snprintf(text, sizeof(text), "mark-%04u%02u%02u-%02u%02u%02u-%02u", year % 10000u, month % 100u, day % 100u, hour % 100u,
            minute % 100u, second % 100u, id % 100u);
        return text;
    }

    // The marks the reminders count. Only the marks the player pressed in
    // this session (Shift+F2) and not yet in a report: `session`, those of
    // `sessionFolders` still in ReplayReports\marks (`diskFolders`; a
    // report moves them out). Folders of an earlier session (a game closed
    // before Shift+F1, a crash, a reload) are `older`: the next report still
    // takes them, the log says how many, and no notice ever counts them (QA
    // report: one stray mark left on disk said "problem marks not sent
    // yet" at every load and after every playback, session after session).
    // The automatic detectors (QA_HEALTH, PUPPET_BLINK, frozen poses, crash
    // and export notices) write log lines and report files, never a mark
    // folder, so they are in neither count.
    struct PendingMarks
    {
        std::uint32_t session = 0;
        std::uint32_t older = 0;
    };

    inline PendingMarks CountPendingMarks(const std::vector<std::string>& sessionFolders, const std::vector<std::string>& diskFolders)
    {
        PendingMarks counts;
        for (const std::string& disk : diskFolders)
        {
            bool mine = false;
            for (const std::string& folder : sessionFolders)
                mine = mine || folder == disk;
            if (mine)
                ++counts.session;
            else
                ++counts.older;
        }
        return counts;
    }

    // A file name part from free text: letters, digits, '-' and '_' kept,
    // the rest '_', at most `limit` characters ("" stays "").
    inline std::string SafeStem(std::string_view text, std::size_t limit = 40)
    {
        std::string out;
        for (const char c : text)
        {
            if (out.size() >= limit)
                break;
            const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
            out += keep ? c : '_';
        }
        return out;
    }

    // The clip's file name without ".rpclip".
    inline std::string ClipStem(std::string_view clip)
    {
        const std::size_t dot = clip.rfind(".rpclip");
        return std::string(dot == std::string_view::npos ? clip : clip.substr(0, dot));
    }

    // "report-20261005-210304-042-Valentine.zip" ("report-...zip" without a clip).
    inline std::string ZipName(std::string_view reportFolder, std::string_view clip)
    {
        const std::string stem = SafeStem(ClipStem(clip));
        return std::string(reportFolder) + (stem.empty() ? "" : "-" + stem) + ".zip";
    }

    inline std::string AnomalyJson(const Anomaly& anomaly, std::uint32_t pressPlayheadMs)
    {
        const long long offset = static_cast<long long>(anomaly.playheadMs) - static_cast<long long>(pressPlayheadMs);
        return std::string("{\"kind\":") + JsonString(KindText(anomaly.kind)) + ",\"playhead_ms\":" + std::to_string(anomaly.playheadMs) +
            ",\"offset_ms\":" + std::to_string(offset) + ",\"pass\":" + std::to_string(anomaly.pass) + ",\"entity\":" +
            JsonString(anomaly.entity) + ",\"detail\":" + JsonString(anomaly.detail) + "}";
    }

    // A body's recorded-against-applied block and its props.
    inline std::string PropJson(const PropInView& p)
    {
        char model[16];
        std::snprintf(model, sizeof(model), "0x%08X", p.model);
        char weapon[16];
        std::snprintf(weapon, sizeof(weapon), "0x%08X", p.weaponHash);
        return "{\"id\": " + JsonString(p.id) + ", \"kind\": " + JsonString(p.kind) + ", \"link\": " + JsonString(p.link) +
            ", \"origin\": " + JsonString(p.origin) + ", \"model\": " + JsonString(model) +
            ", \"weapon\": " + JsonString(weapon) +
            ", \"state\": " + JsonString(p.state) + ", \"clone\": " + (p.clone ? "true" : "false") + ", \"knock_requested\": " +
            (p.knockRequested ? "true" : "false") + ", \"place_error_m\": " + (p.placeErrorM < 0.0f ? std::string("null") : JsonFloat(p.placeErrorM)) + "}";
    }

    inline std::string BodyCheckJson(const BodyInView& b)
    {
        char record[16], armed[16];
        std::snprintf(record, sizeof(record), "0x%x", b.record);
        std::snprintf(armed, sizeof(armed), "0x%x", b.armedRecord);
        std::string j = ", \"layers\": ";
        if (!b.layersChecked)
            j += "null";
        else
            j += "{\"recorded\": " + std::to_string(b.layersRecorded) + ", \"present\": " + std::to_string(b.layersPresent) + ", \"ped\": " +
                std::to_string(b.pedLayers) + "}";
        j += ", \"bones_armed\": " + std::to_string(b.armedBones) + ", \"record\": " + JsonString(record) + ", \"armed_record\": " +
            JsonString(armed) + ", \"props\": [";
        for (std::size_t n = 0; n < b.props.size(); ++n)
            j += (n ? ", " : "") + PropJson(b.props[n]);
        j += "]";
        return j;
    }

    inline std::string SoundRowJson(const SoundRow& r)
    {
        std::string j = "{";
        if (!r.owner.empty())
            j += "\"owner\":" + JsonString(r.owner) + ",";
        j += "\"family\":" + JsonString(r.family) + ",\"taker\":" + JsonString(r.taker) + ",\"packets\":" + std::to_string(r.packets) +
            ",\"played\":" + std::to_string(r.played) + ",\"refused\":" + std::to_string(r.refused) + ",\"yielded\":" +
            std::to_string(r.yielded) + ",\"missed\":" + std::to_string(r.missed) + ",\"jumped\":" + std::to_string(r.jumped) +
            ",\"skipped\":" + std::to_string(r.skipped) + ",\"not_reached\":" + std::to_string(r.notReached) + ",\"doubles\":" +
            std::to_string(r.doubles) + "}";
        return j;
    }

    inline std::string EntitySoundRowJson(const EntitySoundRow& r)
    {
        std::string j = "{";
        if (!r.owner.empty())
            j += "\"owner\":" + JsonString(r.owner) + ",";
        j += "\"rows\":" + std::to_string(r.rows) + ",\"played\":" + std::to_string(r.played) + ",\"refused\":" + std::to_string(r.refused) +
            ",\"too_far\":" + std::to_string(r.tooFar) + ",\"by_puppet\":" + std::to_string(r.byPuppet) + ",\"passed\":" +
            std::to_string(r.passed) + ",\"to_generic\":" + std::to_string(r.toGeneric) + ",\"untried\":" + std::to_string(r.untried) + ",\"last_status\":" + JsonString(r.lastStatus) + "}";
        return j;
    }

    // The "sounds" block of mark.json: the generic packets and
    // the entity sound rows of the window, with who owned them.
    inline std::string SoundWindowJson(const SoundWindow& w, std::uint32_t pressPlayheadMs)
    {
        std::string j = "{\"from_ms\": " + std::to_string(w.fromMs) + ", \"to_ms\": " + std::to_string(w.toMs) + ",\n";
        j += "    \"generic\": ";
        if (!w.genericKnown)
            j += "null,\n";
        else
        {
            j += "{\"packets\": " + std::to_string(w.packets) + ",\n      \"families\": [";
            for (std::size_t n = 0; n < w.families.size(); ++n)
                j += (n ? ",\n        " : "\n        ") + SoundRowJson(w.families[n]);
            j += w.families.empty() ? "],\n" : "\n      ],\n";
            j += "      \"owners\": [";
            for (std::size_t n = 0; n < w.owners.size(); ++n)
                j += (n ? ",\n        " : "\n        ") + SoundRowJson(w.owners[n]);
            j += w.owners.empty() ? "],\n" : "\n      ],\n";
            j += "      \"failures\": [";
            for (std::size_t n = 0; n < w.failures.size(); ++n)
            {
                const SoundFailure& f = w.failures[n];
                char set[16];
                std::snprintf(set, sizeof(set), "0x%x", f.set);
                const long long offset = static_cast<long long>(f.timeMs) - static_cast<long long>(pressPlayheadMs);
                j += (n ? ",\n        " : "\n        ") + std::string("{\"time_ms\":") + std::to_string(f.timeMs) + ",\"offset_ms\":" +
                    std::to_string(offset) + ",\"owner\":" + JsonString(f.owner) + ",\"family\":" + JsonString(f.family) +
                    ",\"taker\":" + JsonString(f.taker) + ",\"set\":" + JsonString(set) + ",\"outcome\":" + JsonString(f.outcome) + "}";
            }
            j += w.failures.empty() ? "]},\n" : "\n      ]},\n";
        }
        j += "    \"entity_sounds\": ";
        if (!w.entityKnown)
            j += "null}";
        else
        {
            j += "{\"totals\": " + EntitySoundRowJson(w.entityTotals) + ",\n      \"owners\": [";
            for (std::size_t n = 0; n < w.entityOwners.size(); ++n)
                j += (n ? ",\n        " : "\n        ") + EntitySoundRowJson(w.entityOwners[n]);
            j += w.entityOwners.empty() ? "]}}" : "\n      ]}}";
        }
        return j;
    }

    inline std::string MarkJson(const Mark& mark)
    {
        std::string j = "{\n";
        j += "  \"format\": \"redemption-editor-mark/1\",\n";
        j += "  \"id\": " + std::to_string(mark.id) + ",\n";
        j += "  \"folder\": " + JsonString(mark.folder) + ",\n";
        j += "  \"version\": " + JsonString(mark.version) + ",\n";
        j += "  \"wall_time\": " + JsonString(mark.wallTime) + ",\n";
        j += "  \"clip\": " + JsonString(mark.clip) + ",\n";
        j += "  \"playhead_ms\": " + std::to_string(mark.playheadMs) + ",\n";
        j += "  \"playhead\": " + JsonString(TimeText(mark.playheadMs)) + ",\n";
        j += "  \"duration_ms\": " + std::to_string(mark.durationMs) + ",\n";
        j += "  \"rate\": " + JsonFloat(static_cast<float>(mark.rate), 4) + ",\n";
        j += std::string("  \"paused\": ") + (mark.paused ? "true" : "false") + ",\n";
        j += std::string("  \"scrubbing\": ") + (mark.scrubbing ? "true" : "false") + ",\n";
        j += "  \"pass\": " + std::to_string(mark.pass) + ",\n"; // the pass over the clip at the press
        j += std::string("  \"editor_open\": ") + (mark.editorOpen ? "true" : "false") + ",\n";
        j += "  \"window\": {\"before_ms\": " + std::to_string(kBeforeMs) + ", \"after_ms\": " + std::to_string(kAfterMs) +
            ", \"closed\": " + (mark.windowClosed ? "true" : "false") +
            ", \"end_playhead_ms\": " + std::to_string(mark.windowEndPlayheadMs) + "},\n";
        const CameraInfo& c = mark.camera;
        j += "  \"camera\": {\"position\": [" + JsonFloat(c.position[0]) + ", " + JsonFloat(c.position[1]) + ", " +
            JsonFloat(c.position[2]) + "], \"rotation\": [" + JsonFloat(c.rotation[0]) + ", " + JsonFloat(c.rotation[1]) + ", " +
            JsonFloat(c.rotation[2]) + "], \"target\": [" + JsonFloat(c.target[0]) + ", " + JsonFloat(c.target[1]) + ", " +
            JsonFloat(c.target[2]) + "], \"fov\": " + JsonFloat(c.fov) + ", \"free_cam\": " + (c.freeCam ? "true" : "false") +
            ", \"editing_marker\": " + (c.editingMarker ? "true" : "false") + "},\n";
        j += "  \"bodies\": [";
        for (std::size_t n = 0; n < mark.bodies.size(); ++n)
        {
            const BodyInView& b = mark.bodies[n];
            char model[16];
            std::snprintf(model, sizeof(model), "0x%08X", b.model);
            j += n ? ",\n    " : "\n    ";
            j += "{\"id\": " + JsonString(b.id) + ", \"kind\": " + JsonString(b.kind) + ", \"model\": " + JsonString(model) +
                ", \"label\": " + JsonString(b.label) + ", \"distance_m\": " + JsonFloat(b.distanceM, 1) +
                ", \"on_screen\": " + (b.onScreen ? "true" : "false") + ", \"state\": " + JsonString(b.state) +
                ", \"visible\": " + std::to_string(b.visible) + ", \"armed\": " + (b.armed ? "true" : "false") +
                ", \"mapped\": " + (b.mapped ? "true" : "false") + ", \"arm_error\": " + JsonString(b.armError) +
                ", \"bones\": " + std::to_string(b.boneCount) + BodyCheckJson(b) + "}"; // layers, records, props
        }
        j += mark.bodies.empty() ? "],\n" : "\n  ],\n";
        j += "  \"anomalies\": [";
        for (std::size_t n = 0; n < mark.anomalies.size(); ++n)
            j += (n ? ",\n    " : "\n    ") + AnomalyJson(mark.anomalies[n], mark.playheadMs);
        j += mark.anomalies.empty() ? "],\n" : "\n  ],\n";
        j += "  \"sounds\": " + SoundWindowJson(mark.sounds, mark.playheadMs) + ",\n";
        j += "  \"note\": " + JsonString(mark.note) + ",\n";
        j += std::string("  \"note_answered\": ") + (mark.noteSet ? "true" : "false") + ",\n";
        j += "  \"shots\": {\"ring\": " + std::to_string(mark.ringShots) + ", \"full\": " + (mark.fullShot ? "true" : "false") +
            ", \"done\": " + (mark.shotsDone ? "true" : "false") + ", \"error\": " + JsonString(mark.shotsError) + "}\n";
        j += "}\n";
        return j;
    }

    // ---------------- System info (the report's sysinfo.txt) ----------------

    // NVIDIA's driver number from the Windows driver version
    // ("32.0.15.6094" is 560.94: the last five digits of the last two
    // parts). "" when the text is not four numbers.
    inline std::string NvidiaDriverVersion(std::string_view windowsVersion)
    {
        std::string parts[4];
        int index = 0;
        for (const char c : windowsVersion)
        {
            if (c == '.')
            {
                if (++index > 3)
                    return {};
                continue;
            }
            if (c < '0' || c > '9')
                return {};
            parts[index] += c;
        }
        if (index != 3 || parts[2].empty() || parts[3].empty())
            return {};
        std::string last4 = parts[3];
        while (last4.size() < 4)
            last4 = "0" + last4;
        const std::string digits = parts[2] + last4;
        if (digits.size() < 5)
            return {};
        const std::string five = digits.substr(digits.size() - 5);
        return std::to_string(std::stoi(five.substr(0, 3))) + "." + five.substr(3);
    }

    // The game's file version ("1.0.1491.50") against the build of the
    // mod's offline addresses: 1 same build, 0 another, -1 unreadable.
    inline int GameBuildMatches(std::string_view fileVersion)
    {
        std::uint32_t parts[4] = {};
        int index = 0;
        bool digit = false;
        for (const char c : fileVersion)
        {
            if (c == '.')
            {
                if (!digit || ++index > 3)
                    return -1;
                digit = false;
                continue;
            }
            if (c < '0' || c > '9')
                return -1;
            parts[index] = parts[index] * 10 + static_cast<std::uint32_t>(c - '0');
            digit = true;
        }
        if (index != 3 || !digit)
            return -1;
        return parts[2] == kGameBuildMajor && parts[3] == kGameBuildMinor ? 1 : 0;
    }

    inline std::string MegabytesText(std::uint64_t bytes)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(bytes) / (1024.0 * 1024.0));
        return text;
    }

    // A third-party module of the game folder worth listing (the mods): .asi
    // files and DLLs that are not the game's own.
    inline bool ListedModFile(std::string_view name)
    {
        std::string lower;
        for (const char c : name)
            lower += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        const auto ends = [&](std::string_view suffix) {
            return lower.size() >= suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        if (ends(".asi") || ends(".addon64") || ends(".addon"))
            return true;
        if (!ends(".dll"))
            return false;
        // The game's own DLLs (Steam, Rockstar launcher, Bink, Oodle, NVIDIA/AMD upscalers).
        static constexpr std::string_view kGame[] = {"bink2w64.dll", "oo2core_", "steam_api64.dll", "nvngx_dlss", "nvngx_dlssg",
            "amd_fidelityfx", "ffx_", "libxess", "sl.", "nvlowlatency", "socialclub", "nvapi", "icudt", "icuuc"};
        for (const std::string_view prefix : kGame)
        {
            if (lower.compare(0, prefix.size(), prefix) == 0)
                return false;
        }
        return true;
    }

    // The tester's mod environment as loaded, one log line (ENV_MODS) and
    // a sysinfo line.
    // `mods`: the modules loaded from the game folder that ListedModFile
    // keeps; `outside`: modules loaded from outside the game folder and the
    // Windows folder (overlays and hooks: OBS, Steam, Discord); `vfs`: a
    // vfs.asi loaded; `lml`: the game folder has lml\; `modManager`: a
    // ModManager module loaded or in the folder.
    inline std::string EnvModsLine(const std::vector<std::string>& mods, const std::vector<std::string>& outside, bool vfs,
        bool lml, bool modManager)
    {
        std::string line = "ENV_MODS,count=" + std::to_string(mods.size()) + ",outside_count=" + std::to_string(outside.size()) +
            ",vfs=" + (vfs ? "1" : "0") + ",lml=" + (lml ? "1" : "0") + ",mod_manager=" + (modManager ? "1" : "0") + ",mods=";
        for (std::size_t n = 0; n < mods.size(); ++n)
            line += (n ? "|" : "") + mods[n];
        line += ",outside=";
        for (std::size_t n = 0; n < outside.size(); ++n)
            line += (n ? "|" : "") + outside[n];
        for (char& c : line)
            if (c == '\n' || c == '\r')
                c = ' ';
        return line;
    }
}
