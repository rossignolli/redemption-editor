// ReplayClipTool: reads a clip file with the mod's own pure code
// (no game) and writes what a camera director needs as JSON: the entities
// with their trajectories (position, heading, speed, body box, head), what
// they are attached to, the timed events (shots with where they landed,
// speech, melee, body impacts, wounds, explosions, fires, breaks), values
// derived from those (who went down, distances between entities, the busy
// moments) and the recorded game camera.
//
//   ReplayClipTool export <clip.rpclip> <out.json> [--step-ms 100]
//   ReplayClipTool trim <in.rpclip> <out.rpclip> <fromMs> <toMs>
//   ReplayClipTool trim <in.rpclip> <out.rpclip> last <seconds>
//   ReplayClipTool set-camera <clip.rpclip> <markers.txt> [--out <copy.rpclip>]
//   ReplayClipTool project-camera <project.rpproj> <clip.rpclip> <markers.txt> [--name <name>] [--keep-trim]
//   ReplayClipTool project-info <project.rpproj>
//   ReplayClipTool mount <clip.rpclip> <rider> <mount> <fromMs> <toMs> [--step-ms 16] [--raw]
//   ReplayClipTool pairs <clip.rpclip> [--step-ms 100]
//   ReplayClipTool events <clip.rpclip> [id]
//   ReplayClipTool sections <clip.rpclip>   (the sections, the load verdict, the sections left out)
//   ReplayClipTool signature <clip.rpclip>... [--id slot:instance]   (skeleton signatures)
//   ReplayClipTool hats <clip.rpclip> [--step-ms 100]             (each record's hat chain and its top bone over time)
//   ReplayClipTool objects <clip.rpclip> [id | scenery]   (weapons, props, world objects, breaks, scenery and its children; OBJECT_LOOK and SCENERY_LOOK)
//   ReplayClipTool weapon-look <clip.rpclip> [id]   (each weapon's components by name and its look track, wear and engine component list per state)
//   ReplayClipTool never-animated <clip.rpclip>... [--ref slot:instance]   (bodies recorded without a pose change)
//   ReplayClipTool frozen-poses <clip.rpclip> [--max-pct 2] [--max-peds 0] [--max-dist 40]   (walking peds whose pose repeats)
//
// set-camera replaces the camera markers (see SetCamera for the format);
// with --out the clip is written there and the input is left alone.
// project-camera (for the director MCP): the same markers into a project
// (replay_project.h): the clip's entry in the project (added at the end when
// missing; the project is created when the file does not exist) takes them
// as its edit markers, and its trim becomes the first and last marker
// (TakeMarkers) unless --keep-trim fits them to the trim it has. The clip
// file is only read.
//
// The export also writes the place
// (town, district, character, weather by name, the player's model), marks
// the player entity, who rides which horse (jerk::MountPairs, the playback's
// own rule), and a timeline block: time scale changes, the cinematic slow
// motion spans (kill cam), Dead Eye and Eagle Eye spans, the recording cuts,
// the gate spans (cutscene, script camera, controls off) and the game
// camera cuts.
//
// trim cuts the clip to a window with recording_window.h (the clip starts
// at the lifecycle block holding fromMs), writes it with the input's game
// build identity, loads the output again and prints a summary.
//
// Bones are stored by index without names: the head is estimated as the
// highest bone of the entity's first sampled frame, followed by index.

#include "camera_spline.h" // the Spline keys
#include "camera_marker_shake.h" // the shake in the markers, shake-migration
#include "clip_compress.h"
#include "clip_file.h"
#include "clip_pack.h"        // pack-verify, pack-load
#include "entity_transform.h"
#include "pose_jerk.h"
#include "recording_window.h"
#include "replay_project.h"
#include "sound_owner_table.h" // the generic packets' family and taker
#include "sound_generic_player_core.h" // InitVariableIndex
#include "skeleton_map.h"       // signature
#include "skeleton_signature.h" // signature
#include "speech_identity.h"     // speech, speech-log
#include "speech_record_match.h" // speech-log replays the recorder
#include "object_check.h"        // objects
#include "weapon_components.h"   // weapon-look
#include "weapon_look_core.h"   // weapon-look
#include "hat_chain.h"           // hats
#include "tpose_check.h"         // never-animated
#include "mix_scene_pool_core.h"  // mix
#include "speech_duck_core.h"     // mix
#include "shot_fx_audit_core.h"   // shots
#include "weapon_bones_core.h"    // weapon-bones
#include "fx_motion_rule.h"       // events (derived call gaps)
#include "frozen_pose_check.h"    // frozen-poses
#include "muscle_watch.h"         // muscle-rows
#include "shader_vars_core.h"      // shader-vars

#include <Windows.h>
#include <psapi.h> // the memory peak (pack-verify, pack-load)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using namespace replay;

    constexpr float kPi = 3.14159265358979323846f;

    struct Sample
    {
        std::uint32_t t = 0;
        float pos[3] = {};      // entity origin
        float heading = 0.0f;   // degrees, 0 faces +Y (rotation order 2 yaw)
        float speed = 0.0f;     // m/s, from the previous sample
        float head[3] = {};     // estimated head bone, world
        float boxMin[3] = {};   // all bones, world
        float boxMax[3] = {};
        bool valid = false;
    };

    struct EntityOut
    {
        entity::CreatePacket create;
        std::uint32_t firstMs = ~0u;
        std::uint32_t lastMs = 0;
        std::vector<Sample> samples;
        float height = 0.0f;  // median body box height
        float radius = 0.0f;  // median body box radius on the ground plane
        int headBone = -1;
        std::uint32_t headRecord = 0;
    };

    std::string Hex(std::uint32_t value)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "0x%08X", value);
        return text;
    }

    std::string Id(entity::ReplayId id) { return entity::IdText(id); }

    std::string F(float v, int digits = 2)
    {
        if (!std::isfinite(v))
            return "null";
        char text[32];
        std::snprintf(text, sizeof(text), "%.*f", digits, v);
        return text;
    }

    std::string V3(const float* v, int digits = 2)
    {
        return "[" + F(v[0], digits) + "," + F(v[1], digits) + "," + F(v[2], digits) + "]";
    }

    // The entity's world transform at the sample (the mod's own decode:
    // the position carries a world cell in matrix[15]).
    bool SampleWorld(const pose::PoseSample& s, pose::WorldTransform& world)
    {
        pose::WorldTransform a{};
        if (!pose::DecodeWorldMatrix(s.worldA, a))
            return false;
        pose::WorldTransform b{};
        world = pose::DecodeWorldMatrix(s.worldB, b) ? pose::InterpolateWorld(a, b, s.t) : a;
        return true;
    }

    void ToWorld(const pose::WorldTransform& world, const float* local, float* out)
    {
        const pose::Vec3 r = pose::Rotate(world.rotation, {local[0], local[1], local[2]});
        out[0] = world.position.x + r.x;
        out[1] = world.position.y + r.y;
        out[2] = world.position.z + r.z;
    }

    float Distance(const float* a, const float* b)
    {
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    int Export(const std::wstring& clipPath, const std::wstring& outPath, std::uint32_t stepMs, const std::string& clipName)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        // Image offsets are rebased on load; any base works for reading.
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        if (stepMs == 0)
            stepMs = durationMs <= 120000 ? 100u : durationMs <= 300000 ? 200u : 250u;

        std::map<std::uint32_t, std::unique_ptr<pose::PoseTrack>> tracks;
        for (const pose::PoseRecordExport& record : poses)
        {
            auto track = std::make_unique<pose::PoseTrack>();
            std::string importError;
            if (track->Import(record.track, importError))
                tracks[record.recordId] = std::move(track);
            else
                std::fprintf(stderr, "record %08X: %s\n", record.recordId, importError.c_str());
        }

        // Entities: every create packet, alive range from the lifecycle.
        std::map<std::uint32_t, EntityOut> entities;
        for (const entity::CreatePacket& create : recording.created)
            entities[create.id.Packed()].create = create;
        std::vector<entity::CreatePacket> alive;
        std::vector<entity::Attachment> attachments;
        struct AttachSpan { entity::Attachment a; std::uint32_t from; std::uint32_t to; };
        std::vector<AttachSpan> attachSpans;
        const std::uint32_t duration = static_cast<std::uint32_t>(durationMs);
        for (std::uint32_t t = 0; t <= duration; t += stepMs)
        {
            recording.lifecycle.AliveAt(t, alive);
            for (const entity::CreatePacket& create : alive)
            {
                EntityOut& e = entities[create.id.Packed()];
                if (e.create.id.Packed() == 0)
                    e.create = create;
                e.firstMs = std::min(e.firstMs, t);
                e.lastMs = std::max(e.lastMs, t);
            }
            recording.lifecycle.AttachmentsAt(t, attachments);
            for (const entity::Attachment& a : attachments)
            {
                auto it = std::find_if(attachSpans.begin(), attachSpans.end(), [&](const AttachSpan& s) {
                    return s.a == a && s.to + stepMs >= t;
                });
                if (it == attachSpans.end())
                    attachSpans.push_back({a, t, t});
                else
                    it->to = t;
            }
        }

        // Trajectories.
        pose::PoseSample poseSample;
        for (auto& [packed, e] : entities)
        {
            const entity::EntityKind kind = e.create.kind;
            if (kind == entity::EntityKind::Weapon)
                continue;
            Sample previous;
            for (std::uint32_t t = 0; t <= duration; t += stepMs)
            {
                const entity::PoseSegment* segment = recording.segments.At(e.create.id, t);
                const std::uint32_t recordId = segment ? segment->recordId : packed;
                auto found = tracks.find(recordId);
                Sample s;
                s.t = t;
                pose::WorldTransform world{};
                if (found != tracks.end() && found->second->Sample(t, poseSample) && SampleWorld(poseSample, world))
                {
                    s.pos[0] = world.position.x; s.pos[1] = world.position.y; s.pos[2] = world.position.z;
                    pose::Vec3 right{}, forward{}, up{};
                    pose::BasisFromQuat(world.rotation, right, forward, up);
                    s.heading = std::atan2(-forward.x, forward.y) * 180.0f / kPi;
                    const auto& objects = poseSample.objects;
                    if (!objects.empty())
                    {
                        if (e.headBone < 0 || e.headRecord != recordId ||
                            e.headBone >= static_cast<int>(objects.size()))
                        {
                            // Highest bone of this record's first sample.
                            float best = -1e9f;
                            for (std::size_t n = 0; n < objects.size(); ++n)
                            {
                                if (objects[n].translation[2] > best)
                                {
                                    best = objects[n].translation[2];
                                    e.headBone = static_cast<int>(n);
                                }
                            }
                            e.headRecord = recordId;
                        }
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            s.boxMin[axis] = 1e9f;
                            s.boxMax[axis] = -1e9f;
                        }
                        for (std::size_t n = 0; n < objects.size(); ++n)
                        {
                            float w[3];
                            ToWorld(world, objects[n].translation, w);
                            for (int axis = 0; axis < 3; ++axis)
                            {
                                s.boxMin[axis] = std::min(s.boxMin[axis], w[axis]);
                                s.boxMax[axis] = std::max(s.boxMax[axis], w[axis]);
                            }
                            if (static_cast<int>(n) == e.headBone)
                                std::memcpy(s.head, w, sizeof(w));
                        }
                    }
                    else
                    {
                        std::memcpy(s.head, s.pos, sizeof(s.pos));
                        std::memcpy(s.boxMin, s.pos, sizeof(s.pos));
                        std::memcpy(s.boxMax, s.pos, sizeof(s.pos));
                    }
                    s.valid = true;
                    if (previous.valid && t > previous.t)
                        s.speed = Distance(s.pos, previous.pos) * 1000.0f / static_cast<float>(t - previous.t);
                    previous = s;
                }
                e.samples.push_back(s);
            }
        }

        // Body size: medians over the valid samples. A "ped" under 0.8 m is
        // an animal (bird, fish, dog); humans stand about 1.7 m.
        for (auto& [packed, e] : entities)
        {
            std::vector<float> heights, radii;
            for (const Sample& s : e.samples)
            {
                if (!s.valid)
                    continue;
                heights.push_back(s.boxMax[2] - s.boxMin[2]);
                radii.push_back(std::max(std::max(s.boxMax[0] - s.pos[0], s.pos[0] - s.boxMin[0]),
                    std::max(s.boxMax[1] - s.pos[1], s.pos[1] - s.boxMin[1])));
            }
            if (heights.empty())
                continue;
            std::nth_element(heights.begin(), heights.begin() + heights.size() / 2, heights.end());
            std::nth_element(radii.begin(), radii.begin() + radii.size() / 2, radii.end());
            e.height = heights[heights.size() / 2];
            e.radius = radii[radii.size() / 2];
        }
        const auto isSmall = [&](const EntityOut& e) {
            return e.create.kind == entity::EntityKind::Ped && e.height < 0.8f;
        };

        const auto sampleAt = [&](std::uint32_t packed, std::uint32_t t) -> const Sample* {
            auto it = entities.find(packed);
            if (it == entities.end() || it->second.samples.empty())
                return nullptr;
            const std::size_t index = std::min<std::size_t>(t / stepMs, it->second.samples.size() - 1);
            const Sample& s = it->second.samples[index];
            return s.valid ? &s : nullptr;
        };
        // Entity whose body box (grown by 0.5 m) holds a point at t.
        const auto entityAt = [&](const float* p, std::uint32_t t, std::uint32_t skip) -> std::string {
            float best = 1e9f;
            std::string id;
            for (auto& [packed, e] : entities)
            {
                if (packed == skip || e.create.kind == entity::EntityKind::Weapon)
                    continue;
                const Sample* s = sampleAt(packed, t);
                if (!s)
                    continue;
                bool inside = true;
                for (int axis = 0; axis < 3; ++axis)
                    inside = inside && p[axis] >= s->boxMin[axis] - 0.5f && p[axis] <= s->boxMax[axis] + 0.5f;
                const float d = Distance(p, s->pos);
                if (inside && d < best)
                {
                    best = d;
                    id = Id(e.create.id);
                }
            }
            return id;
        };

        std::ostringstream o;
        o << "{\n\"clip\":{\"file\":\"" << clipName << "\",\"durationMs\":" << durationMs << ",\"stepMs\":" << stepMs;
        const Environment& env = recording.environment;
        if (env.recorded)
            o << ",\"clock\":{\"startS\":" << env.clockStartS << ",\"endS\":" << env.clockEndS << "},\"weather\":{\"from\":\""
              << Hex(env.weatherFrom) << "\",\"to\":\"" << Hex(env.weatherTo) << "\",\"mix\":" << F(env.weatherMix) << "}";
        if (recording.hasArea)
            o << ",\"area\":{\"min\":" << V3(recording.areaMin) << ",\"max\":" << V3(recording.areaMax) << "}";
        if (env.recorded)
            o << ",\"environmentSamples\":" << env.track.size();
        const Place& place = recording.place;
        if (place.recorded)
        {
            const auto text = [](const char* raw, std::size_t size) {
                std::string out;
                for (std::size_t n = 0; n < size && raw[n]; ++n)
                {
                    const char c = raw[n];
                    if (c == '"' || c == '\\')
                        out += '\\';
                    if (static_cast<unsigned char>(c) >= 0x20)
                        out += c;
                }
                return out;
            };
            o << ",\"place\":{\"town\":\"" << text(place.town, sizeof(place.town)) << "\",\"district\":\""
              << text(place.district, sizeof(place.district)) << "\",\"state\":\"" << text(place.state, sizeof(place.state))
              << "\",\"character\":\"" << text(place.character, sizeof(place.character)) << "\",\"weather\":\""
              << text(place.weather, sizeof(place.weather)) << "\",\"playerModel\":\"" << Hex(place.playerModel) << "\"}";
        }
        o << "},\n";

        // The player: the ped of the place's player model alive longest
        // (a clip without the place: the first ped created).
        std::uint32_t playerPacked = 0;
        {
            std::uint32_t bestSpan = 0;
            for (auto& [packed, e] : entities)
            {
                if (e.create.kind != entity::EntityKind::Ped || e.firstMs == ~0u)
                    continue;
                if (place.recorded && place.playerModel && e.create.modelHash != place.playerModel)
                    continue;
                const std::uint32_t span = e.lastMs - e.firstMs + 1;
                if (!place.recorded || !place.playerModel)
                {
                    if (!playerPacked || e.create.timeMs < entities[playerPacked].create.timeMs)
                        playerPacked = packed;
                    continue;
                }
                if (span > bestSpan)
                {
                    bestSpan = span;
                    playerPacked = packed;
                }
            }
        }

        o << "\"units\":\"metres, degrees (heading 0 faces +Y, counter-clockwise), ms\",\n";
        o << "\"sampleFields\":[\"t\",\"x\",\"y\",\"z\",\"heading\",\"speed\",\"headX\",\"headY\",\"headZ\",\"boxMinZ\",\"boxMaxZ\",\"boxRadiusXY\"],\n";
        o << "\"entities\":[\n";
        bool first = true;
        for (auto& [packed, e] : entities)
        {
            o << (first ? "" : ",\n") << "{\"id\":\"" << Id(e.create.id) << "\",\"kind\":\"" << entity::KindText(e.create.kind)
              << "\",\"model\":\"" << Hex(e.create.modelHash) << "\"";
            first = false;
            if (e.create.weaponHash)
                o << ",\"weapon\":\"" << Hex(e.create.weaponHash) << "\"";
            if (e.create.owner.Valid())
                o << ",\"owner\":\"" << Id(e.create.owner) << "\"";
            o << ",\"height\":" << F(e.height) << ",\"radius\":" << F(e.radius);
            if (isSmall(e))
                o << ",\"small\":true";
            if (packed == playerPacked && playerPacked)
                o << ",\"player\":true";
            o << ",\"createdMs\":" << e.create.timeMs;
            if (e.firstMs != ~0u)
                o << ",\"aliveMs\":[" << e.firstMs << "," << e.lastMs << "]";
            o << ",\"samples\":[";
            bool firstSample = true;
            for (const Sample& s : e.samples)
            {
                if (!s.valid)
                    continue;
                const float rx = std::max(s.boxMax[0] - s.pos[0], s.pos[0] - s.boxMin[0]);
                const float ry = std::max(s.boxMax[1] - s.pos[1], s.pos[1] - s.boxMin[1]);
                o << (firstSample ? "" : ",") << "[" << s.t << "," << F(s.pos[0]) << "," << F(s.pos[1]) << "," << F(s.pos[2])
                  << "," << F(s.heading, 0) << "," << F(s.speed) << "," << F(s.head[0]) << "," << F(s.head[1]) << ","
                  << F(s.head[2]) << "," << F(s.boxMin[2]) << "," << F(s.boxMax[2]) << "," << F(std::max(rx, ry)) << "]";
                firstSample = false;
            }
            o << "]}";
        }
        o << "\n],\n";

        o << "\"attachments\":[";
        first = true;
        for (const AttachSpan& span : attachSpans)
        {
            const char* kind = span.a.kind == entity::AttachmentKind::VehicleSeat ? "vehicle_seat"
                : span.a.kind == entity::AttachmentKind::DraftHarness ? "draft_harness" : "weapon_point";
            o << (first ? "" : ",") << "{\"child\":\"" << Id(span.a.child) << "\",\"parent\":\"" << Id(span.a.parent)
              << "\",\"kind\":\"" << kind << "\",\"index\":" << span.a.index << ",\"fromMs\":" << span.from << ",\"toMs\":" << span.to << "}";
            first = false;
        }
        o << "],\n";

        // Riders and mounts: the playback's own pairing rule
        // (jerk::MountPairs) on the recorded transforms at each step; a
        // span per rider and horse.
        {
            struct RideSpan { std::uint32_t rider; std::uint32_t mount; std::uint32_t from; std::uint32_t to; };
            std::vector<RideSpan> rides;
            jerk::MountPairs pairs;
            std::vector<jerk::Body> bodies;
            for (std::uint32_t t = 0; t <= duration; t += stepMs)
            {
                recording.lifecycle.AliveAt(t, alive);
                bodies.clear();
                for (const entity::CreatePacket& c : alive)
                {
                    if (c.kind != entity::EntityKind::Ped && c.kind != entity::EntityKind::Horse)
                        continue;
                    const entity::PoseSegment* segment = recording.segments.At(c.id, t);
                    auto track = tracks.find(segment ? segment->recordId : c.id.Packed());
                    pose::WorldTransform world{};
                    if (track == tracks.end() || !track->second->Sample(t, poseSample) || !SampleWorld(poseSample, world))
                        continue;
                    jerk::Body body;
                    body.packed = c.id.Packed();
                    body.horse = c.kind == entity::EntityKind::Horse;
                    body.world = world;
                    bodies.push_back(body);
                }
                pairs.Update(bodies);
                for (const auto& [rider, mount] : pairs.Map())
                {
                    auto it = std::find_if(rides.begin(), rides.end(), [&](const RideSpan& r) {
                        return r.rider == rider && r.mount == mount && r.to + stepMs >= t;
                    });
                    if (it == rides.end())
                        rides.push_back({rider, mount, t, t});
                    else
                        it->to = t;
                }
            }
            o << "\"riding\":[";
            first = true;
            for (const RideSpan& r : rides)
            {
                o << (first ? "" : ",") << "{\"rider\":\"" << Id(entity::ReplayId::FromPacked(r.rider)) << "\",\"mount\":\""
                  << Id(entity::ReplayId::FromPacked(r.mount)) << "\",\"fromMs\":" << r.from << ",\"toMs\":" << r.to << "}";
                first = false;
            }
            o << "],\n";
        }

        // Events, one list in time order.
        struct Ev { std::uint32_t t; std::string json; };
        std::vector<Ev> events;
        for (const entity::ShotEvent& shot : recording.shots.Events())
        {
            std::string json = "{\"t\":" + std::to_string(shot.timeMs) + ",\"type\":\"shot\",\"shooter\":\"" + Id(shot.shooter) +
                "\",\"weapon\":\"" + Hex(shot.weaponHash) + "\"" +
                (shot.weapon.Valid() ? ",\"weaponEntity\":\"" + Id(shot.weapon) + "\"" : std::string()) + ",\"muzzle\":" + V3(&shot.muzzle.x) + ",\"direction\":" + V3(&shot.direction.x, 3);
            if (shot.hasImpact)
            {
                json += ",\"impact\":" + V3(&shot.impact.x);
                const std::string hit = entityAt(&shot.impact.x, shot.timeMs, shot.shooter.Packed());
                if (!hit.empty())
                    json += ",\"hit\":\"" + hit + "\"";
            }
            events.push_back({shot.timeMs, json + "}"});
        }
        for (const entity::SpeechEvent& line : recording.speechLines.Events())
        {
            std::string json = "{\"t\":" + std::to_string(line.timeMs) + ",\"type\":\"speech\",\"speaker\":\"" + Id(line.speaker) +
                "\",\"voice\":\"" + Hex(line.voiceHash) + "\",\"context\":\"" + Hex(line.contextPartialHash) + "\",\"variation\":" + std::to_string(line.variation);
            if (line.stopMs > line.timeMs)
                json += ",\"endMs\":" + std::to_string(line.stopMs);
            events.push_back({line.timeMs, json + "}"});
        }
        for (const entity::MeleeEvent& hit : recording.meleeHits.Events())
            events.push_back({hit.timeMs, "{\"t\":" + std::to_string(hit.timeMs) + ",\"type\":\"melee\",\"entity\":\"" + Id(hit.entity) + "\"}"});
        // An invalid id (none or not recorded) is left out.
        for (const entity::BodyImpactEvent& impact : recording.bodyImpacts.Events())
            events.push_back({impact.timeMs, "{\"t\":" + std::to_string(impact.timeMs) + ",\"type\":\"body_impact\",\"entity\":\"" +
                Id(impact.entity) + "\"" + (impact.other.Valid() ? ",\"other\":\"" + Id(impact.other) + "\"" : std::string()) +
                ",\"position\":" + V3(impact.position) + "}"});
        for (const entity::GoreHitEvent& wound : recording.goreHits.Events())
            events.push_back({wound.timeMs, "{\"t\":" + std::to_string(wound.timeMs) + ",\"type\":\"wound\",\"entity\":\"" +
                Id(wound.entity) + "\"" + (wound.inflictor.Valid() ? ",\"by\":\"" + Id(wound.inflictor) + "\"" : std::string()) + "}"});
        for (const entity::ExplosionAudioEvent& blast : recording.explosions.Events())
            events.push_back({blast.timeMs, "{\"t\":" + std::to_string(blast.timeMs) + ",\"type\":\"explosion\",\"position\":" + V3(blast.position) + "}"});
        for (const entity::FireStartEvent& fire : recording.fireStarts.Events())
            events.push_back({fire.timeMs, "{\"t\":" + std::to_string(fire.timeMs) + ",\"type\":\"fire\",\"position\":" + V3(fire.position) + "}"});
        for (const FragBreak& piece : recording.fragBreaks)
            events.push_back({piece.timeMs, "{\"t\":" + std::to_string(piece.timeMs) + ",\"type\":\"break\",\"object\":\"" + Id(piece.id) +
                "\",\"group\":" + std::to_string(piece.group) + "}"});
        std::stable_sort(events.begin(), events.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
        o << "\"events\":[\n";
        for (std::size_t n = 0; n < events.size(); ++n)
            o << (n ? ",\n" : "") << events[n].json;
        o << "\n],\n";

        // Footsteps: per entity counts (the rhythm of a walk or a run).
        std::map<std::uint32_t, std::uint32_t> steps;
        for (const entity::FootstepEvent& step : recording.footsteps.Events())
            ++steps[step.entity.Packed()];

        // Derived: down (the head under 0.6 m above the body's lowest bone for 1 s).
        o << "\"derived\":{\n\"down\":[";
        first = true;
        for (auto& [packed, e] : entities)
        {
            if (e.create.kind != entity::EntityKind::Ped || isSmall(e))
                continue;
            std::uint32_t lowSince = 0;
            bool low = false, reported = false;
            for (const Sample& s : e.samples)
            {
                const bool isLow = s.valid && (s.head[2] - s.boxMin[2]) < 0.6f;
                if (isLow && !low)
                    lowSince = s.t;
                low = isLow;
                if (low && !reported && s.t - lowSince >= 1000)
                {
                    o << (first ? "" : ",") << "{\"entity\":\"" << Id(e.create.id) << "\",\"fromMs\":" << lowSince << "}";
                    first = false;
                    reported = true;
                }
                if (!low)
                    reported = false;
            }
        }
        o << "],\n\"footsteps\":{";
        first = true;
        for (auto& [packed, count] : steps)
        {
            o << (first ? "" : ",") << "\"" << Id(entity::ReplayId::FromPacked(packed)) << "\":" << count;
            first = false;
        }
        o << "},\n";

        // Distances between the peds, horses and vehicles every 500 ms.
        std::vector<std::uint32_t> bodies;
        for (auto& [packed, e] : entities)
            if ((e.create.kind == entity::EntityKind::Ped && !isSmall(e)) || e.create.kind == entity::EntityKind::Horse ||
                e.create.kind == entity::EntityKind::Vehicle)
                bodies.push_back(packed);
        o << "\"distanceStepMs\":500,\"distances\":[";
        first = true;
        for (std::size_t i = 0; i < bodies.size(); ++i)
            for (std::size_t j = i + 1; j < bodies.size(); ++j)
            {
                std::string row;
                float closest = 1e9f;
                for (std::uint32_t t = 0; t <= duration; t += 500)
                {
                    const Sample* a = sampleAt(bodies[i], t);
                    const Sample* b = sampleAt(bodies[j], t);
                    row += (row.empty() ? "" : ",") + (a && b ? F(Distance(a->pos, b->pos), 1) : std::string("null"));
                    if (a && b)
                        closest = std::min(closest, Distance(a->pos, b->pos));
                }
                if (closest > 25.0f)
                    continue; // never close: noise for framing
                o << (first ? "" : ",\n") << "{\"a\":\"" << Id(entity::ReplayId::FromPacked(bodies[i])) << "\",\"b\":\""
                  << Id(entity::ReplayId::FromPacked(bodies[j])) << "\",\"closest\":" << F(closest, 1) << ",\"m\":[" << row << "]}";
                first = false;
            }
        o << "],\n";

        // Activity per second: events and movement, with the busiest entity.
        o << "\"activity\":[";
        for (std::uint32_t second = 0; second * 1000 <= duration; ++second)
        {
            const std::uint32_t t0 = second * 1000, t1 = t0 + 1000;
            std::map<std::string, float> score;
            float total = 0.0f;
            for (const Ev& ev : events)
                if (ev.t >= t0 && ev.t < t1)
                    total += 1.0f;
            for (const entity::ShotEvent& shot : recording.shots.Events())
                if (shot.timeMs >= t0 && shot.timeMs < t1)
                    score[Id(shot.shooter)] += 3.0f;
            for (const entity::SpeechEvent& line : recording.speechLines.Events())
                if (line.timeMs >= t0 && line.timeMs < t1)
                    score[Id(line.speaker)] += 2.0f;
            for (const entity::GoreHitEvent& wound : recording.goreHits.Events())
                if (wound.timeMs >= t0 && wound.timeMs < t1)
                    score[Id(wound.entity)] += 3.0f;
            for (const entity::MeleeEvent& hit : recording.meleeHits.Events())
                if (hit.timeMs >= t0 && hit.timeMs < t1)
                    score[Id(hit.entity)] += 2.0f;
            for (std::uint32_t packed : bodies)
            {
                const Sample* s = sampleAt(packed, t0 + 500);
                if (s)
                    score[Id(entity::ReplayId::FromPacked(packed))] += std::min(s->speed, 8.0f) * 0.25f;
            }
            std::string focus;
            float best = 0.0f;
            for (auto& [id, value] : score)
                if (value > best) { best = value; focus = id; }
            o << (second ? "," : "") << "{\"s\":" << second << ",\"events\":" << F(total, 0) << ",\"focus\":\"" << focus
              << "\",\"focusScore\":" << F(best, 1) << "}";
        }
        o << "]\n},\n";

        // Timeline: what changes the pace of the clip.
        {
            o << "\"timeline\":{\"timeScale\":[";
            first = true;
            for (const timescale::Sample& sample : recording.timeScale.Samples())
            {
                o << (first ? "" : ",") << "[" << sample.timeMs << "," << F(sample.scale, 3) << "]";
                first = false;
            }
            o << "],\"slowMotion\":[";
            first = true;
            for (const camera::TimeSpan& span : camera::CinematicSlowSpans(recording.gameCamera, recording.timeScale))
            {
                o << (first ? "" : ",") << "{\"fromMs\":" << span.startMs << ",\"toMs\":" << span.endMs << "}";
                first = false;
            }
            o << "],\"abilities\":[";
            first = true;
            for (const AbilitySpan& span : recording.abilities)
            {
                o << (first ? "" : ",") << "{\"kind\":\"" << (span.kind == AbilityKind::EagleEye ? "eagle_eye" : "dead_eye")
                  << "\",\"fromMs\":" << span.startMs << ",\"toMs\":" << span.endMs << "}";
                first = false;
            }
            o << "],\"cuts\":[";
            first = true;
            for (std::uint32_t cut : recording.cuts)
            {
                o << (first ? "" : ",") << cut;
                first = false;
            }
            o << "],\"gateSpans\":[";
            first = true;
            for (const gate::Span& span : recording.spans)
            {
                const char* kind = span.kind == gate::SpanKind::Cutscene ? "cutscene"
                    : span.kind == gate::SpanKind::ScriptCamera ? "script_camera" : "controls_off";
                o << (first ? "" : ",") << "{\"kind\":\"" << kind << "\",\"fromMs\":" << span.startMs << ",\"toMs\":" << span.endMs << "}";
                first = false;
            }
            o << "],\"gameCameraCuts\":[";
            first = true;
            for (std::size_t n = 1; n < recording.gameCamera.size(); ++n)
            {
                if (!camera::IsGameCameraCut(recording.gameCamera[n - 1], recording.gameCamera[n]))
                    continue;
                o << (first ? "" : ",") << recording.gameCamera[n].timeMs;
                first = false;
            }
            o << "]},\n";
        }

        // The recorded game camera every 250 ms.
        o << "\"gameCameraFields\":[\"t\",\"x\",\"y\",\"z\",\"pitch\",\"roll\",\"yaw\",\"fov\"],\n\"gameCamera\":[";
        first = true;
        for (std::uint32_t t = 0; t <= duration; t += 250)
        {
            editor::CameraState state{};
            if (!camera::EvaluateGame(recording.gameCamera, t, state))
                break;
            o << (first ? "" : ",") << "[" << t << "," << F(state.position.x) << "," << F(state.position.y) << "," << F(state.position.z)
              << "," << F(state.rotation.x, 1) << "," << F(state.rotation.y, 1) << "," << F(state.rotation.z, 1) << "," << F(state.fov, 1) << "]";
            first = false;
        }
        o << "]\n}\n";

        std::ofstream out(outPath, std::ios::binary);
        out << o.str();
        std::printf("CLIP_TOOL,export=ok,entities=%zu,events=%zu,duration_ms=%llu\n", entities.size(), events.size(),
            static_cast<unsigned long long>(durationMs));
        return out ? 0 : 3;
    }
}

namespace
{
    // set-camera: replaces the clip's camera markers with the ones in a
    // plain text file. Two line formats (not mixed in one file):
    //
    // Markers (CAMR v3), one per line, named keys in any order:
    //   t=1500 mode=free pos=x,y,z rot=pitch,roll,yaw fov=45 blend=linear ease=1
    //   look=12:3 look_off=0.5,0 look_roll=0 attach=12:3 mount=default
    //   rel_pos=x,y,z rel_rot=pitch,roll,heading preset_dist=0.5
    //   shake=handheld shake_i=5 shake_speed=100 dof=default dof_i=50
    //   focus=auto focus_d=10 focus_target=12:3 marker=edit speed=100 curve=0
    //   mode: free game front_low rear right_side left_side overhead (default game)
    //   blend: none linear smooth spline; mount: rigid default flexible
    //   shake: none handheld drunk ground_vibration air_turbulence explosion walking running
    //          horseback steadicam wagon
    //   dof: default custom none; focus: auto manual target
    //   marker: edit anchor_on anchor; speed and shake_speed: 5 20 35 50 100 125 150 175 200
    //   shake_motion: off speed still
    //   Spline tuning (the fields left out come from the editor's
    //   defaults): pacing=natural|continuous|segment alpha=0..1|uniform|
    //   centripetal|chordal ease_in=0..1 ease_out=0..1 weight=0..1
    //   sp_pos=0|1 sp_rot=0|1 sp_fov=0|1 (any of them =global: the default)
    //   blend: a marker without blend= is Spline
    //   shake=custom with shake_amp shake_hz shake_spdamp shake_spdhz shake_rot
    //   shake_pos shake_ref shake_still shake_seed
    //   The shake keys above are the legacy marker shake, folded into the
    //   marker's own shake on the way in (as
    //   an old clip's, with a SHAKE_MIGRATION line); the marker's own keys
    //   shake_transition=cut|blend shake_gain= shake_rate= shake_realtime=0|1
    //   shake_restart=0|1 go straight to it.
    // Shake events (camera_effects.h), one per line, each
    // one parameter of the shake (Simple Camera's model):
    //   fx t=43900 kind=amp value=1.5 mode=snap
    //   kind: onoff preset amp freq spdamp spdfreq rot pos still randomize
    //   profile (preset and profile also take a name: handheld, earthquake...)
    //   The legacy whole-shake lines still read and are converted:
    //   fx t=43900 shake=explosion shake_i=8 shake_speed=100 motion=off mode=snap
    //   Every fx line is folded into the markers (an Anchor added where a
    //   change falls between markers).
    //   targets: a packed id "slot:instance" or "-" for none
    // A line "track legacy_blend_any=1" sets the track flag. Markers go in
    // through the marker rules (spacing, 512 at most, the last marker rule).
    //
    // Old (15 columns, CAMR v2 style), converted like a v2 clip:
    //   timeMs x y z pitch roll yaw fov easing path mode target offX offY offZ
    // easing 0 linear 1 smooth 2 in 3 out 4 cut; path 0 straight 1 curved;
    // mode 0 free 1 game 2 target 3 follow; target a packed id "slot:instance".
    bool ParseTarget(const std::string& text, std::uint32_t& packed)
    {
        packed = 0;
        if (text == "-" || text == "none" || text.empty())
            return true;
        unsigned slot = 0, instance = 0;
        if (sscanf_s(text.c_str(), "%u:%u", &slot, &instance) != 2)
            return false;
        packed = entity::ReplayId{static_cast<std::uint16_t>(slot), static_cast<std::uint16_t>(instance)}.Packed();
        return true;
    }

    bool ParseFloats(const std::string& text, float* out, int count)
    {
        std::istringstream in(text);
        for (int n = 0; n < count; ++n)
        {
            if (n > 0 && in.get() != ',')
                return false;
            if (!(in >> out[n]))
                return false;
        }
        return in.peek() == std::char_traits<char>::eof();
    }

    template <typename E>
    bool ParseName(const std::string& text, std::initializer_list<std::pair<const char*, E>> names, E& out)
    {
        for (const auto& [name, value] : names)
        {
            if (text == name)
            {
                out = value;
                return true;
            }
        }
        return false;
    }

    bool ParseSpeed(const std::string& text, std::uint8_t& index)
    {
        const unsigned long percent = std::strtoul(text.c_str(), nullptr, 10);
        for (std::uint32_t n = 0; n < editor::kMarkerSpeedCount; ++n)
        {
            if (editor::kMarkerSpeedPercent[n] == percent)
            {
                index = static_cast<std::uint8_t>(n);
                return true;
            }
        }
        return false;
    }

    // One marker line; false with `error` naming the key.
    bool ParseMarkerLine(const std::string& line, camera::Keyframe& marker, std::string& error)
    {
        using namespace editor;
        std::istringstream fields(line);
        std::string token;
        bool hasTime = false;
        editor::Vec3 rotation{};
        while (fields >> token)
        {
            const std::size_t equals = token.find('=');
            if (equals == std::string::npos)
            {
                error = "expected key=value, got '" + token + "'";
                return false;
            }
            const std::string key = token.substr(0, equals);
            const std::string value = token.substr(equals + 1);
            bool ok = true;
            float v[3] = {};
            if (key == "t")
            {
                marker.timeMs = std::strtoull(value.c_str(), nullptr, 10);
                hasTime = true;
            }
            else if (key == "mode")
                ok = ParseName<CameraMode>(value, {{"free", CameraMode::Free}, {"game", CameraMode::Game},
                    {"front_low", CameraMode::FrontLow}, {"rear", CameraMode::Rear}, {"right_side", CameraMode::RightSide},
                    {"left_side", CameraMode::LeftSide}, {"overhead", CameraMode::Overhead}}, marker.cameraMode);
            else if (key == "pos")
            {
                ok = ParseFloats(value, v, 3);
                marker.pose.position = {v[0], v[1], v[2]};
            }
            else if (key == "rot")
            {
                ok = ParseFloats(value, v, 3);
                rotation = {v[0], v[1], v[2]};
            }
            else if (key == "fov")
            {
                ok = ParseFloats(value, v, 1) && v[0] >= kMarkerFovMin && v[0] <= kMarkerFovMax;
                marker.pose.fov = v[0];
            }
            else if (key == "blend")
                ok = ParseName<Blend>(value, {{"none", Blend::None}, {"linear", Blend::Linear}, {"smooth", Blend::Smooth}, {"spline", Blend::Spline}},
                    marker.blend);
            else if (key == "ease")
            {
                ok = value == "0" || value == "1";
                marker.ease = value == "1";
            }
            else if (key == "look")
                ok = ParseTarget(value, marker.lookAtTarget);
            else if (key == "look_off")
            {
                ok = ParseFloats(value, v, 2) && std::fabs(v[0]) <= 1.0f && std::fabs(v[1]) <= 1.0f;
                marker.lookAtOffset = {v[0], v[1]};
            }
            else if (key == "look_roll")
            {
                ok = ParseFloats(value, v, 1);
                marker.lookAtRoll = v[0];
            }
            else if (key == "attach")
                ok = ParseTarget(value, marker.attachTarget);
            else if (key == "mount")
                ok = ParseName<AttachType>(value, {{"rigid", AttachType::Rigid}, {"default", AttachType::Default},
                    {"flexible", AttachType::Flexible}}, marker.attachType);
            else if (key == "rel_pos")
            {
                ok = ParseFloats(value, v, 3);
                marker.relPosition = {v[0], v[1], v[2]};
            }
            else if (key == "rel_rot")
            {
                ok = ParseFloats(value, v, 3);
                marker.relRotation = {v[0], v[1], v[2]};
            }
            else if (key == "preset_dist")
            {
                ok = ParseFloats(value, v, 1) && v[0] >= 0.0f && v[0] <= 1.0f;
                marker.presetDistanceScale = v[0];
            }
            else if (key == "shake")
                ok = ParseName<ShakeType>(value, {{"none", ShakeType::None}, {"handheld", ShakeType::Handheld},
                    {"drunk", ShakeType::Drunk}, {"ground_vibration", ShakeType::GroundVibration},
                    {"air_turbulence", ShakeType::AirTurbulence}, {"explosion", ShakeType::Explosion},
                    {"walking", ShakeType::Walking}, {"running", ShakeType::Running}, {"horseback", ShakeType::Horseback},
                    {"steadicam", ShakeType::Steadicam}, {"wagon", ShakeType::Wagon}, {"custom", ShakeType::Custom}}, marker.legacyShake.type);
            else if (key == "shake_i")
            {
                const unsigned long i = std::strtoul(value.c_str(), nullptr, 10);
                ok = i >= 1 && i <= 10;
                marker.legacyShake.intensity = static_cast<std::uint8_t>(i);
            }
            else if (key == "shake_speed")
                ok = ParseSpeed(value, marker.legacyShake.speedIndex);
            else if (key == "shake_motion")
                ok = ParseName<editor::ShakeMotion>(value, {{"off", editor::ShakeMotion::Off}, {"speed", editor::ShakeMotion::Speed},
                    {"still", editor::ShakeMotion::SpeedStill}}, marker.legacyShake.motion);
            else if (key == "shake_transition" || key == "shake_gain" || key == "shake_rate" || key == "shake_realtime" ||
                key == "shake_restart")
            {
                // The marker's own shake (the legacy keys above fill the
                // legacy shake, folded on the way in).
                bool known = false;
                std::string fieldError;
                ok = camera::mshake::ParseField(key, value, marker.shake, known, fieldError) && known;
            }
            else if (key.rfind("shake_", 0) == 0 && key != "shake_i" && key != "shake_speed")
            {
                // A Custom shake's numbers.
                bool known = false;
                std::string fieldError;
                ok = camera::shake::ParseCustomField(key, value, marker.legacyShake.custom, known, fieldError) && known;
            }
            else if (key == "speed")
                ok = ParseSpeed(value, marker.speedIndex);
            else if (key == "dof")
                ok = ParseName<DofMode>(value, {{"default", DofMode::Default}, {"custom", DofMode::Custom},
                    {"none", DofMode::None}}, marker.dof);
            else if (key == "dof_i")
            {
                ok = ParseFloats(value, v, 1) && v[0] >= 0.0f && v[0] <= 100.0f;
                marker.dofIntensity = v[0];
            }
            else if (key == "focus")
                ok = ParseName<FocusMode>(value, {{"auto", FocusMode::Auto}, {"manual", FocusMode::Manual},
                    {"target", FocusMode::Target}}, marker.focus);
            else if (key == "focus_d")
            {
                ok = ParseFloats(value, v, 1) && v[0] >= 0.5f && v[0] <= 1000.0f;
                marker.focusDistance = v[0];
            }
            else if (key == "focus_target")
                ok = ParseTarget(value, marker.focusTarget);
            else if (key == "marker")
                ok = ParseName<MarkerType>(value, {{"edit", MarkerType::Edit}, {"anchor_on", MarkerType::EditAndAnchor},
                    {"anchor", MarkerType::Anchor}}, marker.markerType);
            else if (key == "fade" || key == "fade_ms" || key == "filter" || key == "filter_i" || key == "exposure" ||
                key == "contrast")
            {
                // The clip's fades and the marker effects.
                const long n = std::strtol(value.c_str(), nullptr, 10);
                if (key == "fade")
                {
                    ok = n == 0 || n == 1;
                    marker.transition = static_cast<std::uint8_t>(n);
                }
                else if (key == "fade_ms")
                {
                    ok = n >= 100 && n <= 10000;
                    marker.transitionMs = static_cast<std::uint16_t>(n);
                }
                else if (key == "filter")
                {
                    ok = n >= 0 && n < static_cast<long>(editor::kMarkerFilterCount);
                    marker.filter = static_cast<std::uint8_t>(n);
                }
                else if (key == "filter_i")
                {
                    ok = n >= 0 && n <= 100;
                    marker.filterIntensity = static_cast<std::uint8_t>(n);
                }
                else
                {
                    ok = n >= -100 && n <= 100;
                    (key == "exposure" ? marker.brightness : marker.contrast) = static_cast<std::int8_t>(n);
                }
            }
            else if (key == "curve")
            {
                ok = value == "0" || value == "1";
                marker.legacyCurve = value == "1";
            }
            else
            {
                // The Spline keys (camera_spline.h ParseField).
                bool known = false;
                std::string fieldError;
                ok = camera::spline::ParseField(key, value, marker.spline, known, fieldError);
                if (!known)
                {
                    error = "unknown key '" + key + "'";
                    return false;
                }
            }
            if (!ok)
            {
                error = "bad value for '" + key + "': " + value;
                return false;
            }
        }
        if (!hasTime)
        {
            error = "missing t=";
            return false;
        }
        marker.pose.orientation = camera::Track::FromEulerDegrees(rotation);
        return true;
    }

    // The marker file to a track (the format above); 0 or the exit code.
    int ReadMarkerFile(const std::wstring& keysPath, std::uint64_t durationMs, camera::Track& next, bool& legacyLines)
    {
        std::string error;
        std::ifstream in(keysPath);
        if (!in)
        {
            std::fprintf(stderr, "cannot read the keyframe file\n");
            return 2;
        }
        std::vector<std::pair<int, camera::Keyframe>> markers;
        std::vector<std::pair<int, camera::fx::ShakeEvent>> shakeEvents;
        std::vector<camera::fx::LegacyEvent> legacyShake; // the legacy lines, converted together
        int legacyShakeLine = 0;
        legacyLines = false;
        bool legacyBlendAnyMode = false;
        std::string line;
        int lineNumber = 0;
        while (std::getline(in, line))
        {
            ++lineNumber;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty() || line[0] == '#')
                continue;
            if (line.rfind("fx ", 0) == 0)
            {
                std::istringstream tokens(line.substr(3));
                std::vector<std::string> fields;
                for (std::string token; tokens >> token;)
                    fields.push_back(token);
                if (camera::fx::IsLegacyLine(fields))
                {
                    camera::fx::LegacyEvent old;
                    if (!camera::fx::ParseLegacyFields(fields, old, error))
                    {
                        std::fprintf(stderr, "line %d: %s\n", lineNumber, error.c_str());
                        return 2;
                    }
                    old.timeMs = std::min<std::uint64_t>(old.timeMs, durationMs);
                    legacyShake.push_back(old);
                    legacyShakeLine = legacyShakeLine ? legacyShakeLine : lineNumber;
                    continue;
                }
                camera::fx::ShakeEvent event;
                if (!camera::fx::ParseEventFields(fields, event, error))
                {
                    std::fprintf(stderr, "line %d: %s\n", lineNumber, error.c_str());
                    return 2;
                }
                event.timeMs = std::min<std::uint64_t>(event.timeMs, durationMs);
                shakeEvents.emplace_back(lineNumber, event);
                continue;
            }
            if (line.rfind("track ", 0) == 0)
            {
                if (line.find("legacy_blend_any=1") != std::string::npos)
                    legacyBlendAnyMode = true;
                else if (line.find("legacy_blend_any=0") == std::string::npos)
                {
                    std::fprintf(stderr, "line %d: expected 'track legacy_blend_any=0|1'\n", lineNumber);
                    return 2;
                }
                continue;
            }
            const bool named = line.find('=') != std::string::npos;
            if ((named && legacyLines) || (!named && !markers.empty()))
            {
                std::fprintf(stderr, "line %d: marker and 15 column lines mixed\n", lineNumber);
                return 2;
            }
            if (named)
            {
                camera::Keyframe marker;
                if (!ParseMarkerLine(line, marker, error))
                {
                    std::fprintf(stderr, "line %d: %s\n", lineNumber, error.c_str());
                    return 2;
                }
                marker.timeMs = std::min<std::uint64_t>(marker.timeMs, durationMs);
                markers.emplace_back(lineNumber, marker);
                continue;
            }
            legacyLines = true;
            std::istringstream fields(line);
            unsigned long long timeMs = 0;
            editor::CameraState state{};
            unsigned easing = 1, path = 1, mode = 0;
            std::string target;
            editor::Vec3 offset{};
            if (!(fields >> timeMs >> state.position.x >> state.position.y >> state.position.z >> state.rotation.x >>
                    state.rotation.y >> state.rotation.z >> state.fov >> easing >> path >> mode >> target >> offset.x >>
                    offset.y >> offset.z) || easing > 4 || path > 1 || mode > 3)
            {
                std::fprintf(stderr, "line %d: expected 15 fields\n", lineNumber);
                return 2;
            }
            std::uint32_t packed = 0;
            ParseTarget(target, packed);
            const std::uint32_t id = next.AddOrReplace(std::min<std::uint64_t>(timeMs, durationMs), state,
                static_cast<editor::Easing>(easing), static_cast<editor::Path>(path));
            next.SetMode(id, static_cast<editor::CameraMode>(mode), packed, offset);
        }
        if (legacyLines)
        {
            // The same conversion as a v2 clip (blend moved to the source).
            next.ConvertLegacy();
        }
        else
        {
            std::stable_sort(markers.begin(), markers.end(),
                [](const auto& a, const auto& b) { return a.second.timeMs < b.second.timeMs; });
            for (const auto& [number, marker] : markers)
            {
                if (next.AddMarker(marker) == 0)
                {
                    std::fprintf(stderr, "line %d: no room at %llu ms (spacing or 512 markers)\n", number,
                        static_cast<unsigned long long>(marker.timeMs));
                    return 2;
                }
            }
            camera::ApplyLastMarkerRule(next);
            next.SetLegacyBlendAnyMode(legacyBlendAnyMode);
            next.DeriveLegacy();
        }
        if (!legacyShake.empty())
        {
            std::stable_sort(legacyShake.begin(), legacyShake.end(),
                [](const camera::fx::LegacyEvent& a, const camera::fx::LegacyEvent& b) { return a.timeMs < b.timeMs; });
            for (const camera::fx::ShakeEvent& event : camera::fx::ConvertLegacy(legacyShake))
                shakeEvents.emplace_back(legacyShakeLine, event);
        }
        for (const auto& [number, event] : shakeEvents)
        {
            if (next.LegacyEffects().Add(event) == 0)
            {
                std::fprintf(stderr, "line %d: no room for the shake event at %llu ms (1024 at most)\n", number,
                    static_cast<unsigned long long>(event.timeMs));
                return 2;
            }
        }
        // The "fx" lines and the legacy shake keys are folded into the
        // markers (the clip keeps no events track),
        // with the proof of it (no scene: the markers' own camera).
        if (next.HasLegacyShake())
        {
            camera::mshake::Migrate(next);
            const camera::mshake::Proof proof = camera::mshake::Verify(*next.Migration().before, next, camera::Scene{}, durationMs, 0);
            std::printf("%s\n", camera::mshake::FormatLine(next.Migration(), proof).c_str());
            next.Migration().before.reset();
        }
        return 0;
    }

    bool ReadBuildIdentity(const std::vector<std::uint8_t>& bytes, std::uint32_t& timestamp, std::uint64_t& imageSize);

    // A clip file's plain bytes (a compressed clip is decompressed; Decode
    // and the META reader take the plain clip only).
    bool ReadPlainClip(const std::wstring& path, std::vector<std::uint8_t>& plain, std::string& error)
    {
        std::vector<std::uint8_t> raw;
        {
            std::ifstream file(path, std::ios::binary);
            raw.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        if (raw.empty())
        {
            error = "cannot read the clip file";
            return false;
        }
        if (!clipcompress::IsCompressed(raw.data(), raw.size()))
        {
            plain.swap(raw);
            return true;
        }
        return clipcompress::Decompress(raw.data(), raw.size(), plain, error);
    }

    // `outPath` empty: the clip is written over.
    int SetCamera(const std::wstring& clipPath, const std::wstring& keysPath, const std::wstring& outPath)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track track;
        std::uint64_t durationMs = 0;
        std::string error;
        constexpr std::uintptr_t kBase = 0x140000000ull;
        constexpr std::size_t kSize = 0x10000000ull;
        std::vector<std::uint8_t> raw;
        clipfile::SetBuildIdentity(0, 0);
        if (!ReadPlainClip(clipPath, raw, error) || !clipfile::Decode(raw.data(), raw.size(), kBase, kSize, recording, poses, track, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        camera::Track next;
        bool legacyLines = false;
        if (const int code = ReadMarkerFile(keysPath, durationMs, next, legacyLines))
            return code;
        // The written clip keeps the game build identity of its source.
        std::uint32_t timestamp = 0;
        std::uint64_t imageSize = 0;
        ReadBuildIdentity(raw, timestamp, imageSize);
        clipfile::SetBuildIdentity(timestamp, imageSize);
        if (!clipfile::Save(recording, poses, next, durationMs, kBase, kSize, outPath.empty() ? clipPath : outPath, error))
        {
            std::fprintf(stderr, "save failed: %s\n", error.c_str());
            return 3;
        }
        std::printf("CLIP_TOOL,set_camera=ok,keyframes=%zu,format=%s\n", next.Keyframes().size(),
            legacyLines ? "columns" : "markers");
        return 0;
    }

    // project-info: a project's clips (file, trim, markers) to check what
    // project-camera wrote.
    int ProjectInfo(const std::wstring& projectPath)
    {
        project::Project proj;
        std::string error;
        if (!project::LoadFile(projectPath, proj, error))
        {
            std::fprintf(stderr, "project load failed: %s\n", error.c_str());
            return 2;
        }
        std::printf("PROJECT name=%s clips=%zu total_ms=%llu\n", proj.name.c_str(), proj.clips.size(),
            static_cast<unsigned long long>(project::TotalMs(proj)));
        for (std::size_t n = 0; n < proj.clips.size(); ++n)
        {
            const project::Clip& clip = proj.clips[n];
            std::printf("CLIP %zu file=%s raw_ms=%llu in=%llu out=%llu has_markers=%d markers=%zu\n", n, clip.file.c_str(),
                static_cast<unsigned long long>(clip.rawDurationMs), static_cast<unsigned long long>(clip.inMs),
                static_cast<unsigned long long>(clip.outMs), clip.hasMarkers ? 1 : 0, clip.markers.Keyframes().size());
            for (const camera::Keyframe& k : clip.markers.Keyframes())
                std::printf("  t=%llu mode=%d pos=%.2f,%.2f,%.2f fov=%.1f blend=%d look=%s attach=%s\n",
                    static_cast<unsigned long long>(k.timeMs), static_cast<int>(k.cameraMode), k.pose.position.x,
                    k.pose.position.y, k.pose.position.z, k.pose.fov, static_cast<int>(k.blend),
                    k.lookAtTarget ? Id(entity::ReplayId::FromPacked(k.lookAtTarget)).c_str() : "-",
                    k.attachTarget ? Id(entity::ReplayId::FromPacked(k.attachTarget)).c_str() : "-");
            for (const camera::Keyframe& k : clip.markers.Keyframes())
                if (k.shake.on || k.origin) // the marker's shake
                    std::printf("  shake t=%llu origin=%u %s\n", static_cast<unsigned long long>(k.timeMs),
                        static_cast<unsigned>(k.origin), camera::mshake::FormatFields(k.shake).c_str());
            for (const camera::Keyframe& k : clip.markers.Keyframes())
                if (k.spline.overrides != 0) // the marker's own Spline tuning
                    std::printf("  spline t=%llu %s\n", static_cast<unsigned long long>(k.timeMs), camera::spline::Describe(k.spline).c_str());
        }
        return 0;
    }

    // project-camera: the markers into the clip's entry of a project.
    int ProjectCamera(const std::wstring& projectPath, const std::wstring& clipPath, const std::wstring& keysPath,
        const std::string& name, bool keepTrim)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track track;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, track, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        camera::Track next;
        bool legacyLines = false;
        if (const int code = ReadMarkerFile(keysPath, durationMs, next, legacyLines))
            return code;
        if (legacyLines)
        {
            std::fprintf(stderr, "a project takes markers (CAMR v3 lines), not the 15 column format\n");
            return 2;
        }
        project::Project proj;
        const bool exists = std::ifstream(projectPath, std::ios::binary).good();
        if (exists && !project::LoadFile(projectPath, proj, error))
        {
            std::fprintf(stderr, "project load failed: %s\n", error.c_str());
            return 2;
        }
        if (!exists)
        {
            if (!project::ValidName(name))
            {
                std::fprintf(stderr, "bad project name '%s' (letters, digits, spaces, - and _, 48 bytes)\n", name.c_str());
                return 2;
            }
            proj.name = name;
        }
        const std::wstring fileWide = clipPath.substr(clipPath.find_last_of(L"\\/") + 1);
        std::string file;
        for (const wchar_t c : fileWide)
            file += static_cast<char>(c < 0x80 ? c : L'_');
        std::size_t index = proj.clips.size();
        for (std::size_t n = 0; n < proj.clips.size(); ++n)
        {
            if (proj.clips[n].file == file)
            {
                index = n;
                break;
            }
        }
        std::string why;
        if (index == proj.clips.size())
        {
            project::Clip clip;
            if (!project::MakeClip(file, durationMs, clip) || !project::AddClip(proj, index, clip, &why))
            {
                std::fprintf(stderr, "cannot add the clip to the project: %s\n", why.c_str());
                return 3;
            }
        }
        project::Clip& clip = proj.clips[index];
        if (keepTrim)
        {
            clip.markers = project::FitMarkers(next, clip.inMs, clip.outMs);
            clip.hasMarkers = true;
        }
        else if (!project::TakeMarkers(clip, next))
        {
            std::fprintf(stderr, "the markers cannot be the clip's trim (two at least, %llu ms at least, inside the clip)\n",
                static_cast<unsigned long long>(project::kMinClipMs));
            return 3;
        }
        if (!project::SaveFile(projectPath, proj, error))
        {
            std::fprintf(stderr, "project save failed: %s\n", error.c_str());
            return 3;
        }
        std::printf("CLIP_TOOL,project_camera=ok,clip_index=%zu,clips=%zu,markers=%zu,in_ms=%llu,out_ms=%llu,project_ms=%llu\n",
            index, proj.clips.size(), clip.markers.Keyframes().size(), static_cast<unsigned long long>(clip.inMs),
            static_cast<unsigned long long>(clip.outMs), static_cast<unsigned long long>(project::TotalMs(proj)));
        return 0;
    }
}

namespace
{
    // The game build identity in a clip's META section (u32 PE timestamp,
    // u64 image size), so a trimmed clip keeps the one of its source.
    bool ReadBuildIdentity(const std::vector<std::uint8_t>& bytes, std::uint32_t& timestamp, std::uint64_t& imageSize)
    {
        constexpr std::size_t kHeader = 8 + 4 + 8;
        constexpr std::size_t kSectionHeader = 4 + 2 + 8;
        std::size_t offset = kHeader;
        while (offset + kSectionHeader <= bytes.size())
        {
            std::uint32_t tag = 0;
            std::uint64_t length = 0;
            std::memcpy(&tag, bytes.data() + offset, sizeof(tag));
            std::memcpy(&length, bytes.data() + offset + 6, sizeof(length));
            offset += kSectionHeader;
            if (length > bytes.size() - offset)
                return false;
            if (tag == 0x4154454Du && length >= 12) // "META"
            {
                std::memcpy(&timestamp, bytes.data() + offset, sizeof(timestamp));
                std::memcpy(&imageSize, bytes.data() + offset + 4, sizeof(imageSize));
                return true;
            }
            offset += static_cast<std::size_t>(length) + sizeof(std::uint64_t);
        }
        return false;
    }

    std::uint64_t FileBytes(const std::wstring& path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        return file ? static_cast<std::uint64_t>(file.tellg()) : 0;
    }

    int Trim(const std::wstring& inPath, const std::wstring& outPath, bool last, std::uint32_t a, std::uint32_t b)
    {
        constexpr std::uintptr_t kBase = 0x140000000ull;
        constexpr std::size_t kSize = 0x10000000ull;
        std::vector<std::uint8_t> raw;
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        clipfile::SetBuildIdentity(0, 0);
        if (!ReadPlainClip(inPath, raw, error) || !clipfile::Decode(raw.data(), raw.size(), kBase, kSize, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        std::uint32_t fromMs = a;
        std::uint32_t toMs = b;
        if (last)
        {
            toMs = static_cast<std::uint32_t>(durationMs);
            const std::uint64_t span = static_cast<std::uint64_t>(a) * 1000u;
            fromMs = span >= durationMs ? 0u : static_cast<std::uint32_t>(durationMs - span);
        }

        Recording cut;
        std::vector<pose::PoseRecordExport> cutPoses;
        window::Result result;
        if (!window::Cut(recording, poses, durationMs, fromMs, toMs, cut, cutPoses, result))
        {
            std::fprintf(stderr, "trim failed: %s\n", result.error.c_str());
            return 3;
        }
        camera::Track cutCamera;
        window::CutCamera(cameraTrack, result.baseMs, result.endMs, cutCamera);

        std::uint32_t timestamp = 0;
        std::uint64_t imageSize = 0;
        ReadBuildIdentity(raw, timestamp, imageSize);
        clipfile::SetBuildIdentity(timestamp, imageSize);
        if (!clipfile::Save(cut, cutPoses, cutCamera, result.durationMs, kBase, kSize, outPath, error))
        {
            std::fprintf(stderr, "save failed: %s\n", error.c_str());
            return 3;
        }

        // Load the output again (the loader decodes every pose chunk).
        Recording reloaded;
        std::vector<pose::PoseRecordExport> reloadedPoses;
        camera::Track reloadedCamera;
        std::uint64_t reloadedMs = 0;
        if (!clipfile::Load(outPath, kBase, kSize, reloaded, reloadedPoses, reloadedCamera, reloadedMs, error))
        {
            std::fprintf(stderr, "reload failed: %s\n", error.c_str());
            return 4;
        }
        int poseFailures = 0;
        for (const pose::PoseRecordExport& record : reloadedPoses)
        {
            pose::PoseTrack track;
            std::string importError;
            if (!track.Import(record.track, importError))
                ++poseFailures;
        }
        const std::vector<window::KindCount> check = window::Tally(reloaded, reloadedPoses);
        bool same = check.size() == result.kinds.size();
        for (std::size_t n = 0; same && n < check.size(); ++n)
            same = check[n].before == result.kinds[n].after;

        std::printf("CLIP_TOOL_TRIM,from_ms=%u,to_ms=%u,base_ms=%u,end_ms=%u,duration_before_ms=%llu,duration_after_ms=%llu,"
                    "bytes_before=%llu,bytes_after=%llu,entities_before=%llu,entities_after=%llu,decal_pairs_dropped=%llu,"
                    "pose_frames_clamped=%llu,pose_frames_truncated=%llu,reload=ok,pose_import_failures=%d,counts_match=%d,"
                    "clock_s=%u-%u,build=%08X\n",
            fromMs, toMs, result.baseMs, result.endMs, static_cast<unsigned long long>(durationMs),
            static_cast<unsigned long long>(reloadedMs), static_cast<unsigned long long>(FileBytes(inPath)),
            static_cast<unsigned long long>(FileBytes(outPath)),
            static_cast<unsigned long long>(recording.created.size()), static_cast<unsigned long long>(reloaded.created.size()),
            static_cast<unsigned long long>(result.decalPairsDropped),
            static_cast<unsigned long long>(result.poseFramesClamped),
            static_cast<unsigned long long>(result.poseFramesTruncated), poseFailures, same ? 1 : 0,
            reloaded.environment.clockStartS, reloaded.environment.clockEndS, timestamp);
        for (const window::KindCount& kind : result.kinds)
        {
            if (kind.before || kind.after)
                std::printf("  %-20s %10llu -> %10llu\n", kind.name, static_cast<unsigned long long>(kind.before),
                    static_cast<unsigned long long>(kind.after));
        }
        return (poseFailures == 0 && same) ? 0 : 5;
    }

    // One entity's bones in a window (e.g. a ped released from a lasso whose
    // body box blows up while its head stays on the body), printed when a
    // bone's object-space translation lies farther than minM from the
    // entity origin on the ground plane. Per line: time,
    // bone index, layout parent, distance, object translation, local
    // translation. Says which bones leave the body and whether their local
    // carries the offset (a stale or world-driven local) or only the parent
    // chain does.
    int Bones(const std::wstring& clipPath, const std::string& idText, std::uint32_t fromMs, std::uint32_t toMs,
        std::uint32_t stepMs, float minM)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        std::uint32_t packed = 0;
        if (!ParseTarget(idText, packed) || !packed)
        {
            std::fprintf(stderr, "bad entity id: %s (slot:instance)\n", idText.c_str());
            return 1;
        }
        const entity::CreatePacket* create = nullptr;
        for (const entity::CreatePacket& candidate : recording.created)
            if (candidate.id.Packed() == packed)
                create = &candidate;
        if (!create)
        {
            std::fprintf(stderr, "entity %s not in the clip\n", idText.c_str());
            return 3;
        }
        std::map<std::uint32_t, std::unique_ptr<pose::PoseTrack>> tracks;
        for (const pose::PoseRecordExport& record : poses)
        {
            auto track = std::make_unique<pose::PoseTrack>();
            std::string importError;
            if (track->Import(record.track, importError))
                tracks[record.recordId] = std::move(track);
        }
        pose::PoseSample sample;
        for (std::uint32_t t = fromMs; t <= toMs; t += stepMs)
        {
            const entity::PoseSegment* segment = recording.segments.At(create->id, t);
            const std::uint32_t recordId = segment ? segment->recordId : packed;
            auto found = tracks.find(recordId);
            if (found == tracks.end() || !found->second->Sample(t, sample))
            {
                std::printf("t=%u no_sample record=%08X\n", t, recordId);
                continue;
            }
            const pose::SkeletonLayout& layout = found->second->Layout();
            std::uint32_t farBones = 0;
            for (std::size_t n = 0; n < sample.objects.size(); ++n)
            {
                const float* o = sample.objects[n].translation;
                const float d = std::sqrt(o[0] * o[0] + o[1] * o[1]);
                if (!(d > minM))
                    continue;
                ++farBones;
                const float* l = n < sample.locals.size() ? sample.locals[n].translation : o;
                std::printf("t=%u bone=%zu parent=%d dist=%.2f obj=%.2f;%.2f;%.2f local=%.2f;%.2f;%.2f frames=%u/%u\n", t, n,
                    n < layout.parent.size() ? layout.parent[n] : -99, d, o[0], o[1], o[2], l[0], l[1], l[2], sample.frameTimeA,
                    sample.frameTimeB);
            }
            std::printf("t=%u far_bones=%u of %zu\n", t, farBones, sample.objects.size());
        }
        return 0;
    }

    // shader-vars: the recorded shader variables (SHVR) of every horse (or
    // one id): points, holes, unset lanes and dips (a sample whose mean
    // leaves both neighbours, which agree: the muscles relaxing for one
    // sample); with --series, every point as time:set:mean.
    int ShaderVars(const std::wstring& clipPath, const std::string& filter, bool series)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        namespace sv = replay::shadervars;
        std::printf("SHADER_VARS,duration_ms=%llu,owners=%zu,points=%zu\n", static_cast<unsigned long long>(durationMs),
            recording.shaderVars.series.size(), recording.shaderVars.Points());
        for (const sv::Series& s : recording.shaderVars.series)
        {
            const std::string id = entity::IdText(entity::ReplayId::FromPacked(s.owner));
            if (!filter.empty() && filter != id)
                continue;
            for (std::size_t bi = 0; bi < s.blocks.size(); ++bi)
            {
                const sv::Block& b = s.blocks[bi];
                std::vector<float> mean(b.times.size(), 0.0f);
                std::vector<std::uint32_t> set(b.times.size(), 0);
                std::uint32_t partial = 0, empty = 0, gaps100 = 0, maxGap = 0, dips = 0, allZero = 0;
                for (std::size_t i = 0; i < b.times.size(); ++i)
                {
                    float sum = 0.0f;
                    for (std::uint32_t k = 0; k < b.lanes; ++k)
                    {
                        const std::uint32_t v = sv::FromHalf(b.values[i * b.lanes + k]);
                        if (sv::IsSet(v))
                        {
                            ++set[i];
                            sum += sv::FloatOf(v);
                        }
                    }
                    mean[i] = set[i] ? sum / static_cast<float>(set[i]) : -1.0f;
                    partial += set[i] && set[i] < b.lanes ? 1u : 0u;
                    empty += set[i] == 0 ? 1u : 0u;
                    allZero += set[i] && sum == 0.0f ? 1u : 0u;
                    if (i)
                    {
                        const std::uint32_t gap = b.times[i] - b.times[i - 1];
                        maxGap = (std::max)(maxGap, gap);
                        gaps100 += gap > 100 ? 1u : 0u;
                    }
                }
                std::string dipText;
                for (std::size_t i = 1; i + 1 < b.times.size(); ++i)
                {
                    const float a = mean[i - 1], m = mean[i], c = mean[i + 1];
                    if (a < 0 || m < 0 || c < 0)
                        continue;
                    if (std::fabs(a - c) < 0.05f && std::fabs(m - a) > 0.1f && std::fabs(m - c) > 0.1f)
                    {
                        ++dips;
                        if (dips <= 12)
                        {
                            char item[64];
                            std::snprintf(item, sizeof(item), "%s%u:%.2f>%.2f", dipText.empty() ? "" : ";", b.times[i], a, m);
                            dipText += item;
                        }
                    }
                }
                std::printf("SHADER_VARS_OWNER,id=%s,block=%zu,dofs=%zu,lanes=%u,points=%zu,from_ms=%u,to_ms=%u,max_gap_ms=%u,"
                            "gaps_over_100ms=%u,partial=%u,empty=%u,all_zero=%u,dips=%u,dip_at=%s\n",
                    id.c_str(), bi, b.dofs.size(), b.lanes, b.times.size(), b.times.front(), b.times.back(), maxGap, gaps100,
                    partial, empty, allZero, dips, dipText.empty() ? "-" : dipText.c_str());
                if (series)
                {
                    std::string line;
                    for (std::size_t i = 0; i < b.times.size(); ++i)
                    {
                        char item[48];
                        std::snprintf(item, sizeof(item), "%s%u:%u:%.2f", line.empty() ? "" : " ", b.times[i], set[i], mean[i]);
                        line += item;
                    }
                    std::printf("SHADER_VARS_SERIES,id=%s,block=%zu,%s\n", id.c_str(), bi, line.c_str());
                }
            }
        }
        return 0;
    }

    // muscle-rows: the recorded bone rows of every horse (or one id) that go
    // A, B, A over three consecutive raw samples (muscle_watch.h RowWatch): a
    // flicker recorded into the clip. One line per record, the rows that
    // alternate most, and a total.
    int MuscleRows(const std::wstring& clipPath, const std::string& filter, std::uint32_t stepMs)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        std::map<std::uint32_t, std::unique_ptr<pose::PoseTrack>> tracks;
        for (const pose::PoseRecordExport& record : poses)
        {
            auto track = std::make_unique<pose::PoseTrack>();
            std::string importError;
            if (track->Import(record.track, importError))
                tracks[record.recordId] = std::move(track);
        }
        struct PerRecord
        {
            std::string id;
            musclewatch::RowWatch watch;
            std::uint32_t lastFrame = 0xFFFFFFFFu;
            std::uint32_t events = 0;
        };
        std::map<std::uint32_t, PerRecord> records;
        pose::PoseSample sample;
        std::uint32_t horses = 0;
        for (const entity::CreatePacket& create : recording.created)
        {
            if (create.kind != entity::EntityKind::Horse || (!filter.empty() && Id(create.id) != filter))
                continue;
            ++horses;
            for (std::uint64_t t = create.timeMs; t <= durationMs; t += stepMs)
            {
                const entity::PoseSegment* segment = recording.segments.At(create.id, static_cast<std::uint32_t>(t));
                const std::uint32_t recordId = segment ? segment->recordId : create.id.Packed();
                auto found = tracks.find(recordId);
                if (found == tracks.end() || !found->second->Sample(static_cast<std::uint32_t>(t), sample))
                    continue;
                PerRecord& per = records[recordId];
                if (per.id.empty())
                    per.id = Id(create.id);
                const std::uint32_t frame = sample.frameTimeA;
                if (frame == per.lastFrame)
                    continue;
                per.lastFrame = frame;
                if (!found->second->Sample(frame, sample)) // the raw frame, not a blend
                    continue;
                per.events += musclewatch::ObserveRows(per.watch, sample.locals);
            }
        }
        std::uint64_t samples = 0, events = 0, rowsAlternating = 0;
        for (const auto& [recordId, per] : records)
        {
            std::vector<std::pair<std::uint32_t, std::size_t>> top;
            for (std::size_t n = 0; n < per.watch.alternations.size(); ++n)
                if (per.watch.alternations[n])
                    top.push_back({per.watch.alternations[n], n});
            std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
            std::string text;
            for (std::size_t n = 0; n < top.size() && n < 24; ++n)
            {
                char item[48];
                const std::size_t row = top[n].second;
                std::snprintf(item, sizeof(item), "%s%zu%c:%u/%u", text.empty() ? "" : "|", row / 3, "trs"[row % 3], top[n].first,
                    per.watch.changes[row]);
                text += item;
            }
            std::printf("MUSCLE_ROWS,id=%s,record=0x%08x,samples=%u,rows=%zu,rows_alternating=%zu,events=%u,top=%s\n",
                per.id.c_str(), recordId, per.watch.samples, per.watch.alternations.size(), top.size(), per.events,
                text.empty() ? "-" : text.c_str());
            samples += per.watch.samples;
            events += per.events;
            rowsAlternating += top.size();
        }
        std::printf("MUSCLE_ROWS_TOTAL,horses=%u,records=%zu,samples=%llu,rows_alternating=%llu,events=%llu\n", horses,
            records.size(), static_cast<unsigned long long>(samples), static_cast<unsigned long long>(rowsAlternating),
            static_cast<unsigned long long>(events));
        return 0;
    }
}


namespace
{
    // The pose records of the real weapons (create kind weapon with a
    // weapon hash): did the
    // recorded locals of the weapon's own bones move (lever, cylinder,
    // hammer), how often, which bone, and around which shots. A track with
    // no moving frame is the "static" record (wrong bone, wrong space, root
    // only); one that moves is what the play must show.
    // What a clip has of each weapon's look: the components of the create
    // (by name from the public list, "?" when the list has no name), and
    // the look track (WCND): wear per state and, from version 2, the
    // engine's component list per state with what changed.
    int WeaponLook(const std::wstring& clipPath, const std::string& filter)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        const auto nameOf = [](std::uint32_t hash) -> const char* {
            for (const weapon::Component& component : weapon::kComponents)
                if (component.hash == hash)
                    return component.name;
            return "?";
        };
        std::uint32_t weapons = 0, tracks = 0, statesWithList = 0, componentChanges = 0, unnamed = 0;
        for (const entity::CreatePacket& create : recording.created)
        {
            if (!create.weaponHash || (!filter.empty() && Id(create.id) != filter))
                continue;
            ++weapons;
            std::printf("WEAPON,id=%s,kind=%s,weapon=0x%08x,model=0x%08x,create_ms=%u,components=%zu\n", Id(create.id).c_str(),
                create.kind == entity::EntityKind::Weapon ? "weapon" : "prop", create.weaponHash, create.modelHash, create.timeMs,
                create.weaponComponents.size());
            for (const std::uint32_t hash : create.weaponComponents)
                std::printf("  COMPONENT,id=%s,hash=0x%08x,name=%s\n", Id(create.id).c_str(), hash, nameOf(hash));
        }
        for (const WeaponConditionTrack& track : recording.weaponConditions)
        {
            const std::string id = Id(entity::ReplayId::FromPacked(track.recordId));
            if (!filter.empty() && id != filter)
                continue;
            ++tracks;
            const std::vector<std::uint32_t>* previous = nullptr;
            for (const auto& state : track.states)
            {
                const auto& look = state.second;
                const bool list = (look.flags & (weaponlook::kFlagComponents | weaponlook::kFlagComponentsNames)) != 0;
                statesWithList += list ? 1u : 0u;
                std::printf("CONDITION,id=%s,t=%u,dirt=%.3f,soot=%.3f,damage=%.3f,degradation=%.3f,permanent=%.3f,e4=%.3f,"
                            "flags=0x%02x,components_from=%s,components=%zu\n",
                    id.c_str(), state.first, look.dirt, look.soot, look.damage, look.degradation, look.permanent, look.wearE4,
                    look.flags,
                    (look.flags & weaponlook::kFlagComponents) ? "engine"
                        : (look.flags & weaponlook::kFlagComponentsNames) ? "names" : "none",
                    look.components.size());
                if (!list)
                    continue;
                if (!previous)
                {
                    for (const std::uint32_t hash : look.components)
                    {
                        const char* name = nameOf(hash);
                        unnamed += name[0] == '?' ? 1u : 0u;
                        std::printf("  LOOK_COMPONENT,id=%s,t=%u,hash=0x%08x,name=%s\n", id.c_str(), state.first, hash, name);
                    }
                }
                else if (*previous != look.components)
                {
                    ++componentChanges;
                    std::vector<std::uint32_t> gone, added;
                    weaponlook::Diff(*previous, look.components, gone, added);
                    for (const std::uint32_t hash : added)
                        std::printf("  LOOK_CHANGE,id=%s,t=%u,added=0x%08x,name=%s\n", id.c_str(), state.first, hash, nameOf(hash));
                    for (const std::uint32_t hash : gone)
                        std::printf("  LOOK_CHANGE,id=%s,t=%u,removed=0x%08x,name=%s\n", id.c_str(), state.first, hash, nameOf(hash));
                }
                previous = &look.components;
            }
        }
        std::printf("SUMMARY,weapons=%u,look_tracks=%u,states_with_components=%u,component_changes=%u,unnamed_components=%u\n",
            weapons, tracks, statesWithList, componentChanges, unnamed);
        return 0;
    }

    int WeaponBones(const std::wstring& clipPath, const std::string& filter, std::uint32_t stepMs, bool perBone, int seriesBone)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        std::uint32_t weapons = 0, animated = 0, still = 0, unposed = 0;
        for (const entity::CreatePacket& create : recording.created)
        {
            if (create.kind != entity::EntityKind::Weapon || !create.weaponHash)
                continue;
            if (!filter.empty() && Id(create.id) != filter)
                continue;
            const pose::PoseRecordExport* record = nullptr;
            for (const pose::PoseRecordExport& candidate : poses)
                if (candidate.recordId == create.id.Packed())
                    record = &candidate;
            if (!record)
            {
                ++unposed;
                continue;
            }
            ++weapons;
            pose::PoseTrack track;
            std::string importError;
            if (!track.Import(record->track, importError))
            {
                std::printf("WEAPON_POSE,id=%s,status=import_failed,error=%s\n", Id(create.id).c_str(), importError.c_str());
                continue;
            }
            const std::uint32_t first = record->track.stats.firstTimeMs;
            const std::uint32_t last = record->track.stats.lastTimeMs;
            pose::PoseSample sample, previous, start;
            bool havePrevious = false;
            std::uint32_t frames = 0, moving = 0, movingSpans = 0;
            bool wasMoving = false;
            replay::weaponbones::PoseDiff worstFromStart;
            std::map<int, std::uint32_t> movedBones; // bone -> moving frames
            std::vector<std::uint32_t> spanStarts;
            // --bones: per bone, the largest turn from the
            // first frame (and its axis), the largest step, moving frames, the
            // translation range, local and object space.
            struct BoneRange
            {
                float maxRad = 0.0f, axis[3] = {0, 0, 0}, maxStepRad = 0.0f, maxM = 0.0f, objMaxRad = 0.0f;
                std::uint32_t moving = 0;
            };
            std::vector<BoneRange> ranges;
            for (std::uint32_t t = first; t <= last; t += stepMs)
            {
                if (!track.Sample(t, sample) || sample.locals.size() < 2)
                    continue;
                ++frames;
                if (!havePrevious)
                {
                    start = sample;
                    previous = sample;
                    havePrevious = true;
                    continue;
                }
                const std::size_t bones = (std::min)(sample.locals.size(), previous.locals.size());
                const replay::weaponbones::PoseDiff step =
                    replay::weaponbones::DiffLocals(previous.locals.data(), sample.locals.data(), bones);
                const replay::weaponbones::PoseDiff fromStart = replay::weaponbones::DiffLocals(
                    start.locals.data(), sample.locals.data(), (std::min)(bones, start.locals.size()));
                if (fromStart.maxRad > worstFromStart.maxRad || fromStart.maxM > worstFromStart.maxM)
                    worstFromStart = fromStart;
                if (seriesBone >= 0 && static_cast<std::size_t>(seriesBone) < bones)
                {
                    // --series <bone>: the bone's turn from the
                    // first frame, local and object, and the two frames sampled.
                    const std::size_t n = static_cast<std::size_t>(seriesBone);
                    std::printf("WEAPON_POSE_SERIES,id=%s,bone=%d,t=%u,local_deg=%.1f,step_deg=%.1f,obj_deg=%.1f,frames=%u/%u\n",
                        Id(create.id).c_str(), seriesBone, t,
                        replay::weaponbones::QuatAngle(start.locals[n].rotation, sample.locals[n].rotation) * 57.29578f,
                        replay::weaponbones::QuatAngle(previous.locals[n].rotation, sample.locals[n].rotation) * 57.29578f,
                        n < sample.objects.size() && n < start.objects.size()
                            ? replay::weaponbones::QuatAngle(start.objects[n].rotation, sample.objects[n].rotation) * 57.29578f
                            : -1.0f,
                        sample.frameTimeA, sample.frameTimeB);
                }
                if (perBone)
                {
                    ranges.resize(bones);
                    for (std::size_t n = 0; n < bones && n < start.locals.size(); ++n)
                    {
                        const pose::Quat& a = start.locals[n].rotation;
                        const pose::Quat& b = sample.locals[n].rotation;
                        const float rad = replay::weaponbones::QuatAngle(a, b);
                        const float stepRad = replay::weaponbones::QuatAngle(previous.locals[n].rotation, b);
                        BoneRange& r = ranges[n];
                        if (rad > r.maxRad)
                        {
                            r.maxRad = rad;
                            // axis of conj(a) * b
                            const float x = a.w * b.x - a.x * b.w - a.y * b.z + a.z * b.y;
                            const float y = a.w * b.y + a.x * b.z - a.y * b.w - a.z * b.x;
                            const float z = a.w * b.z - a.x * b.y + a.y * b.x - a.z * b.w;
                            const float len = std::sqrt(x * x + y * y + z * z);
                            if (len > 1e-6f)
                            {
                                r.axis[0] = x / len;
                                r.axis[1] = y / len;
                                r.axis[2] = z / len;
                            }
                        }
                        r.maxStepRad = (std::max)(r.maxStepRad, stepRad);
                        r.moving += stepRad > replay::weaponbones::kMovedRad ? 1u : 0u;
                        const float dx = sample.locals[n].translation[0] - start.locals[n].translation[0];
                        const float dy = sample.locals[n].translation[1] - start.locals[n].translation[1];
                        const float dz = sample.locals[n].translation[2] - start.locals[n].translation[2];
                        r.maxM = (std::max)(r.maxM, std::sqrt(dx * dx + dy * dy + dz * dz));
                        if (n < sample.objects.size() && n < start.objects.size())
                            r.objMaxRad = (std::max)(r.objMaxRad,
                                replay::weaponbones::QuatAngle(start.objects[n].rotation, sample.objects[n].rotation));
                    }
                }
                const bool isMoving = step.moved > 0;
                if (isMoving)
                {
                    ++moving;
                    ++movedBones[step.worstBone];
                    if (!wasMoving)
                    {
                        ++movingSpans;
                        if (spanStarts.size() < 12)
                            spanStarts.push_back(t);
                    }
                }
                wasMoving = isMoving;
                previous = sample;
            }
            std::uint32_t shots = 0;
            for (const entity::ShotEvent& shot : recording.shots.Events())
                shots += shot.weapon == create.id ? 1u : 0u;
            std::string topBones;
            for (const auto& [bone, count] : movedBones)
            {
                if (count * 20u < moving && movedBones.size() > 4)
                    continue;
                topBones += (topBones.empty() ? "" : " ") + std::to_string(bone) + ":" + std::to_string(count);
            }
            std::string starts;
            for (std::uint32_t t : spanStarts)
                starts += (starts.empty() ? "" : " ") + std::to_string(t);
            if (perBone)
            {
                const pose::SkeletonLayout& layout = track.Layout();
                for (std::size_t n = 0; n < ranges.size(); ++n)
                {
                    const BoneRange& r = ranges[n];
                    std::printf("WEAPON_POSE_BONE,id=%s,bone=%zu,parent=%d,max_deg=%.1f,axis=%.2f;%.2f;%.2f,max_step_deg=%.1f,"
                                "moving_frames=%u,max_mm=%.1f,obj_max_deg=%.1f,start_t=%.3f;%.3f;%.3f\n",
                        Id(create.id).c_str(), n, n < layout.parent.size() ? layout.parent[n] : -99, r.maxRad * 57.29578f,
                        r.axis[0], r.axis[1], r.axis[2], r.maxStepRad * 57.29578f, r.moving, r.maxM * 1000.0f,
                        r.objMaxRad * 57.29578f, start.locals[n].translation[0], start.locals[n].translation[1],
                        start.locals[n].translation[2]);
                }
            }
            const bool isAnimated = moving > 0;
            animated += isAnimated ? 1u : 0u;
            still += isAnimated ? 0u : 1u;
            std::printf("WEAPON_POSE,id=%s,weapon=0x%08x,model=0x%08x,owner=%s,bones=%u,track_ms=%u-%u,frames=%u,"
                        "moving_frames=%u,moving_spans=%u,span_starts=%s,max_deg_from_start=%.1f,max_mm_from_start=%.1f,"
                        "worst_bone=%d,bones_by_worst=%s,shots=%u,verdict=%s\n",
                Id(create.id).c_str(), create.weaponHash, create.modelHash,
                create.owner.Valid() ? Id(create.owner).c_str() : "none", record->boneCount, first, last, frames, moving,
                movingSpans, starts.empty() ? "-" : starts.c_str(), worstFromStart.maxRad * 57.29578f,
                worstFromStart.maxM * 1000.0f, worstFromStart.worstBone, topBones.empty() ? "-" : topBones.c_str(), shots,
                isAnimated ? "animated" : "static");
        }
        std::printf("WEAPON_POSE_TOTAL,weapons_posed=%u,animated=%u,static=%u,weapons_without_pose=%u,step_ms=%u\n", weapons,
            animated, still, unposed, stepMs);
        return 0;
    }
}

namespace
{
    // Every pose record's hat chain (hat_chain.h) and the local x of its top
    // bone sampled over the record (1 cm buckets): the value the animation
    // fits on screen and the bind value it leaves off screen. `high` is the
    // share of samples at the chain's highest value (the floating hat when
    // it is the bind).

    // --fix: the playback fix (hatchain::FixSample) simulated on every
    // sample: the hats it lowers (the hat group's first bone, model space),
    // where the fitted value came from, and, on the samples it leaves alone,
    // how far the subtree composed again from the recorded locals is from
    // the recorded objects (the compose rule checked against the engine's).
    int HatsFix(const std::wstring& clipPath, std::uint32_t stepMs)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        std::map<std::uint32_t, const entity::CreatePacket*> creates;
        for (const entity::CreatePacket& create : recording.created)
            creates[create.id.Packed()] = &create;
        struct Item
        {
            const pose::PoseRecordExport* record = nullptr;
            std::unique_ptr<pose::PoseTrack> track;
            hatchain::PuppetFix fix;
            entity::ReplayId owner;
            std::uint32_t first = 0, last = 0;
        };
        std::vector<Item> items;
        pose::PoseSample sample;
        for (const pose::PoseRecordExport& record : poses)
        {
            Item item;
            item.record = &record;
            item.track = std::make_unique<pose::PoseTrack>();
            std::string importError;
            if (!item.track->Import(record.track, importError))
                continue;
            item.first = 0xFFFFFFFFu;
            for (const pose::TrackChunkExport& chunk : record.track.chunks)
                for (const std::uint32_t t : chunk.frameTimes)
                {
                    item.first = (std::min)(item.first, t);
                    item.last = (std::max)(item.last, t);
                }
            if (item.first > item.last || !item.track->Sample(item.first, sample))
                continue;
            item.owner = (record.recordId & entity::PoseSegmentTrack::kRecordBit) ? recording.segments.OwnerOf(record.recordId)
                                                                                     : entity::ReplayId::FromPacked(record.recordId);
            const auto created = creates.find(item.owner.Packed());
            const pose::SkeletonLayout& layout = item.track->LayoutAt(item.first);
            std::vector<std::int32_t> parents(layout.parent.begin(), layout.parent.end());
            std::vector<std::array<float, 3>> translations(sample.locals.size());
            for (std::size_t n = 0; n < sample.locals.size(); ++n)
                translations[n] = {sample.locals[n].translation[0], sample.locals[n].translation[1], sample.locals[n].translation[2]};
            item.fix = hatchain::BuildFix(created != creates.end() ? created->second->modelHash : 0u, parents, translations,
                layout.order, layout.inheritScale);
            if (item.fix.armed)
                items.push_back(std::move(item));
        }
        hatchain::Fitted fitted;
        // Pass 1 (the playback learns as it goes; here the whole clip): the
        // models' fitted values.
        for (Item& item : items)
            for (std::uint32_t t = item.first; t <= item.last; t += stepMs * 5)
                if (item.track->Sample(t, sample))
                    for (const hatchain::PuppetFix::Item& chain : item.fix.items)
                        if (chain.top < sample.locals.size() && sample.locals[chain.top].translation[0] <= hatchain::kFitMaxX)
                            fitted.emplace(item.fix.model, sample.locals[chain.top].translation[0]);
        std::uint64_t totalFixed = 0, totalSamples = 0;
        float worstCompose = 0.0f;
        for (Item& item : items)
        {
            std::uint64_t samples = 0, fixedSamples = 0;
            double dropSum = 0.0;
            float composeError = 0.0f;
            std::uint32_t worstT = 0, worstBone = 0;
            std::map<std::string, std::uint32_t> sources;
            hatchain::Fitted local = fitted;
            const pose::SkeletonLayout* layoutNow = &item.track->LayoutAt(item.first);
            std::uint32_t rebuilt = 0;
            for (std::uint32_t t = item.first; t <= item.last; t += stepMs)
            {
                if (!item.track->Sample(t, sample))
                    continue;
                // A layout change mid-record (a hat knocked off, a layer
                // change): the fix is built again from the new layout, as
                // playback does when the layout at the playhead changes.
                const pose::SkeletonLayout* layoutAt = &item.track->LayoutAt(t);
                if (layoutAt != layoutNow)
                {
                    layoutNow = layoutAt;
                    std::vector<std::int32_t> parents(layoutAt->parent.begin(), layoutAt->parent.end());
                    std::vector<std::array<float, 3>> translations(sample.locals.size());
                    for (std::size_t n = 0; n < sample.locals.size(); ++n)
                        translations[n] = {sample.locals[n].translation[0], sample.locals[n].translation[1], sample.locals[n].translation[2]};
                    const std::vector<hatchain::PuppetFix::Item> old = item.fix.items;
                    item.fix = hatchain::BuildFix(item.fix.model, parents, translations, layoutAt->order, layoutAt->inheritScale);
                    for (hatchain::PuppetFix::Item& chain : item.fix.items)
                        for (const hatchain::PuppetFix::Item& before : old)
                            if (before.top == chain.top)
                                chain.fitted = before.fitted;
                    ++rebuilt;
                }
                ++samples;
                std::vector<pose::BoneQts> locals = sample.locals;
                std::vector<pose::BoneQts> objects = sample.objects;
                const std::vector<pose::BoneQts> before = objects;
                const std::uint32_t changed = hatchain::FixSample(item.fix, local, locals, objects);
                for (const hatchain::PuppetFix::Item& chain : item.fix.items)
                {
                    const std::uint16_t hatRoot = chain.hatRoot;
                    // How much closer the hat came to the head (model space:
                    // a lying or bowed head turns the chain away from z).
                    if (changed && hatRoot && hatRoot < objects.size() && chain.head < objects.size())
                        dropSum += pose::TranslationDistance(before[hatRoot], before[chain.head]) -
                            pose::TranslationDistance(objects[hatRoot], objects[chain.head]);
                    if (!changed)
                    {
                        // The compose rule against the recorded objects.
                        std::vector<pose::BoneQts> again = before;
                        for (const std::uint16_t bone : chain.subtree)
                        {
                            const std::int32_t parent = bone < item.fix.parents.size() ? item.fix.parents[bone] : -1;
                            if (parent < 0 || static_cast<std::size_t>(parent) >= again.size() || bone >= again.size())
                                continue;
                            again[bone] = pose::Compose(again[static_cast<std::size_t>(parent)], locals[bone], item.fix.order,
                                item.fix.inheritScale);
                            const float d = pose::TranslationDistance(again[bone], before[bone]);
                            if (d > composeError)
                            {
                                worstT = t;
                                worstBone = bone;
                            }
                            composeError = (std::max)(composeError, d);
                        }
                    }
                }
                if (changed)
                {
                    ++fixedSamples;
                    ++sources[hatchain::FitSourceName(item.fix.lastSource)];
                }
            }
            std::string sourceText;
            for (const auto& [name, count] : sources)
                sourceText += (sourceText.empty() ? "" : ";") + name + ":" + std::to_string(count);
            std::printf("HAT_FIX,record=%08X,entity=%s,model=0x%08x,chains=%zu,samples=%llu,fixed=%llu,sources=%s,"
                        "hat_closer_m=%.3f,compose_error_m=%.4f,worst_t=%u,worst_bone=%u,layouts_rebuilt=%u,"
                        "handled=%llu,kept_objects=%llu\n",
                item.record->recordId, Id(item.owner).c_str(), item.fix.model, item.fix.items.size(),
                static_cast<unsigned long long>(samples), static_cast<unsigned long long>(fixedSamples),
                sourceText.empty() ? "none" : sourceText.c_str(), fixedSamples ? dropSum / fixedSamples : 0.0, composeError,
                worstT, worstBone, rebuilt, static_cast<unsigned long long>(item.fix.handled),
                static_cast<unsigned long long>(item.fix.kept));
            totalFixed += fixedSamples;
            totalSamples += samples;
            worstCompose = (std::max)(worstCompose, composeError);
        }
        std::printf("HATS_FIX_SUMMARY,records_with_hat=%zu,samples=%llu,fixed=%llu,worst_compose_error_m=%.4f\n", items.size(),
            static_cast<unsigned long long>(totalSamples), static_cast<unsigned long long>(totalFixed), worstCompose);
        return 0;
    }

    int Hats(const std::wstring& clipPath, std::uint32_t stepMs)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        std::map<std::uint32_t, const entity::CreatePacket*> creates;
        for (const entity::CreatePacket& create : recording.created)
            creates[create.id.Packed()] = &create;
        std::uint32_t records = 0, withChain = 0, samples = 0, high = 0;
        pose::PoseSample sample;
        for (const pose::PoseRecordExport& record : poses)
        {
            pose::PoseTrack track;
            std::string importError;
            if (!track.Import(record.track, importError))
                continue;
            std::uint32_t first = 0xFFFFFFFFu, last = 0;
            for (const pose::TrackChunkExport& chunk : record.track.chunks)
                for (const std::uint32_t t : chunk.frameTimes)
                {
                    first = (std::min)(first, t);
                    last = (std::max)(last, t);
                }
            if (first > last || !track.Sample(first, sample))
                continue;
            ++records;
            const pose::SkeletonLayout& layout = track.LayoutAt(first);
            std::vector<std::int32_t> parents(layout.parent.begin(), layout.parent.end());
            std::vector<std::array<float, 3>> translations(sample.locals.size());
            for (std::size_t n = 0; n < sample.locals.size(); ++n)
                translations[n] = {sample.locals[n].translation[0], sample.locals[n].translation[1], sample.locals[n].translation[2]};
            const std::vector<hatchain::Chain> chains = hatchain::FindChains(parents, translations);
            if (chains.empty())
                continue;
            ++withChain;
            const entity::ReplayId owner = (record.recordId & entity::PoseSegmentTrack::kRecordBit)
                ? recording.segments.OwnerOf(record.recordId)
                : entity::ReplayId::FromPacked(record.recordId);
            const auto created = creates.find(owner.Packed());
            for (const hatchain::Chain& chain : chains)
            {
                std::map<int, std::uint32_t> buckets;
                std::uint32_t n = 0;
                for (std::uint32_t t = first; t <= last; t += stepMs)
                {
                    if (!track.Sample(t, sample) || chain.top >= sample.locals.size())
                        continue;
                    ++buckets[static_cast<int>(std::lround(sample.locals[chain.top].translation[0] * 100.0f))];
                    ++n;
                }
                std::string values;
                for (const auto& [cm, count] : buckets)
                    values += (values.empty() ? "" : ";") + std::to_string(cm / 100.0f).substr(0, 4) + ":" + std::to_string(count);
                const std::uint32_t atHigh = buckets.empty() ? 0u : buckets.rbegin()->second;
                std::string bones;
                for (auto bone = chain.bones.rbegin(); bone != chain.bones.rend(); ++bone)
                    bones += (bones.empty() ? "" : "-") + std::to_string(*bone);
                std::printf("HAT,record=%08X,entity=%s,model=0x%08x,bones=%zu,head=%u,chain=%s,top=%u,carries_group=%d,"
                            "span_ms=%u-%u,samples=%u,values=%s,high_pct=%.1f\n",
                    record.recordId, Id(owner).c_str(), created != creates.end() ? created->second->modelHash : 0u,
                    sample.locals.size(), chain.head, bones.c_str(), chain.top, chain.carriesGroup ? 1 : 0, first, last, n,
                    values.c_str(), n ? 100.0 * atHigh / n : 0.0);
                if (buckets.size() > 1)
                {
                    samples += n;
                    high += atHigh;
                }
            }
        }
        std::printf("HATS_SUMMARY,records=%u,with_chain=%u,samples_of_changing_chains=%u,at_high=%u\n", records, withChain,
            samples, high);
        return 0;
    }

    // A rider and its mount in a window (a rider jerking on its horse).

    // RAW: every recorded frame of both records (time, gap, flags, origin,
    // root bone, how far each moved and turned since the frame before, and
    // whether IsRebase takes the pair for a rebase, which makes playback
    // hold the earlier frame). PLAY: both sampled at the same playhead, as
    // playback does, with the rider's origin and root in the mount's space
    // and how far that offset moved since the step before; a hold (t = 0
    // between two different frames) is marked.
    struct MountFrameInfo
    {
        std::uint32_t t = 0;
        std::uint16_t flags = 0;
        bool hasWorld = false;
        pose::WorldTransform world{};
        pose::Vec3 root{};
        pose::Quat rootRotation{};
    };

    std::vector<MountFrameInfo> RawFrames(pose::PoseTrack& track, const pose::TrackExport& exported,
        std::uint32_t fromMs, std::uint32_t toMs)
    {
        std::vector<MountFrameInfo> frames;
        pose::PoseSample sample;
        for (const pose::TrackChunkExport& chunk : exported.chunks)
        {
            for (std::size_t n = 0; n < chunk.frameTimes.size(); ++n)
            {
                const std::uint32_t t = chunk.frameTimes[n];
                if (t < fromMs || t > toMs)
                    continue;
                MountFrameInfo info;
                info.t = t;
                const std::size_t offset = chunk.frameOffsets[n];
                if (offset + 12 <= chunk.bytes.size())
                    std::memcpy(&info.flags, chunk.bytes.data() + offset + 8, sizeof(info.flags));
                if (track.Sample(t, sample) && pose::DecodeWorldMatrix(sample.worldA, info.world) &&
                    !sample.objects.empty())
                {
                    info.hasWorld = true;
                    info.root = pose::RootWorldPosition(info.world, sample.objects[0]);
                    info.rootRotation = pose::Normalize(pose::Multiply(info.world.rotation, sample.objects[0].rotation));
                }
                frames.push_back(info);
            }
        }
        std::sort(frames.begin(), frames.end(), [](const MountFrameInfo& a, const MountFrameInfo& b) { return a.t < b.t; });
        return frames;
    }

    float Dist3(const pose::Vec3& a, const pose::Vec3& b)
    {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    pose::Vec3 ToLocal(const pose::WorldTransform& parent, const pose::Vec3& p)
    {
        const pose::Vec3 d{p.x - parent.position.x, p.y - parent.position.y, p.z - parent.position.z};
        return pose::Rotate(pose::Conjugate(parent.rotation), d);
    }

    int Mount(const std::wstring& clipPath, const std::string& riderText, const std::string& mountText,
        std::uint32_t fromMs, std::uint32_t toMs, std::uint32_t stepMs, bool rawOnly)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        std::uint32_t riderPacked = 0, mountPacked = 0;
        if (!ParseTarget(riderText, riderPacked) || !ParseTarget(mountText, mountPacked) || !riderPacked || !mountPacked)
        {
            std::fprintf(stderr, "bad entity id (slot:instance)\n");
            return 1;
        }
        std::map<std::uint32_t, std::unique_ptr<pose::PoseTrack>> tracks;
        std::map<std::uint32_t, const pose::TrackExport*> exports;
        for (const pose::PoseRecordExport& record : poses)
        {
            auto track = std::make_unique<pose::PoseTrack>();
            std::string importError;
            if (track->Import(record.track, importError))
            {
                tracks[record.recordId] = std::move(track);
                exports[record.recordId] = &record.track;
            }
        }
        const auto findCreate = [&](std::uint32_t packed) -> const entity::CreatePacket* {
            for (const entity::CreatePacket& c : recording.created)
                if (c.id.Packed() == packed)
                    return &c;
            return nullptr;
        };
        const entity::CreatePacket* riderCreate = findCreate(riderPacked);
        const entity::CreatePacket* mountCreate = findCreate(mountPacked);
        if (!riderCreate || !mountCreate)
        {
            std::fprintf(stderr, "entity not in the clip\n");
            return 3;
        }
        const auto recordAt = [&](const entity::CreatePacket* c, std::uint32_t t) {
            const entity::PoseSegment* segment = recording.segments.At(c->id, t);
            return segment ? segment->recordId : c->id.Packed();
        };

        // RAW frames of every record either entity uses in the window.
        for (const entity::CreatePacket* c : {riderCreate, mountCreate})
        {
            std::vector<std::uint32_t> records;
            for (std::uint32_t t = fromMs; t <= toMs; t += 10)
            {
                const std::uint32_t r = recordAt(c, t);
                if (std::find(records.begin(), records.end(), r) == records.end())
                    records.push_back(r);
            }
            for (std::uint32_t r : records)
            {
                auto track = tracks.find(r);
                if (track == tracks.end())
                {
                    std::printf("RAW id=%s record=%08X missing\n", Id(c->id).c_str(), r);
                    continue;
                }
                const std::vector<MountFrameInfo> frames = RawFrames(*track->second, *exports[r], fromMs, toMs);
                std::uint32_t maxGap = 0, rebases = 0, flagged = 0;
                for (std::size_t n = 0; n < frames.size(); ++n)
                {
                    const MountFrameInfo& f = frames[n];
                    const MountFrameInfo* p = n ? &frames[n - 1] : nullptr;
                    float originMove = 0, rootMove = 0, entityTurn = 0, rootTurn = 0;
                    bool rebase = false;
                    const std::uint32_t gap = p ? f.t - p->t : 0;
                    if (p && p->hasWorld && f.hasWorld)
                    {
                        originMove = Dist3(p->world.position, f.world.position);
                        rootMove = Dist3(p->root, f.root);
                        entityTurn = pose::AngleBetween(p->world.rotation, f.world.rotation) * 180.0f / kPi;
                        rootTurn = pose::AngleBetween(p->rootRotation, f.rootRotation) * 180.0f / kPi;
                        pose::RebaseProbe a, b;
                        a.world = p->world; a.root = p->root; a.rootRotation = p->rootRotation; a.hasRootRotation = true; a.timeMs = p->t;
                        b.world = f.world; b.root = f.root; b.rootRotation = f.rootRotation; b.hasRootRotation = true; b.timeMs = f.t;
                        rebase = pose::IsRebase(a, b);
                    }
                    maxGap = std::max(maxGap, gap);
                    rebases += rebase ? 1 : 0;
                    flagged += (f.flags & pose::kFrameNoInterpolation) ? 1 : 0;
                    std::printf("RAW id=%s record=%08X t=%u gap=%u flags=%c%c%c pos=%.3f;%.3f;%.3f root=%.3f;%.3f;%.3f "
                                "origin_move=%.3f root_move=%.3f excess=%.3f turn=%.1f root_turn=%.1f rebase=%d\n",
                        Id(c->id).c_str(), r, f.t, gap, (f.flags & pose::kFrameKeyframe) ? 'K' : '-',
                        (f.flags & pose::kFrameNoInterpolation) ? 'N' : '-', (f.flags & pose::kFrameRelativeKeyframe) ? 'R' : '-',
                        f.world.position.x, f.world.position.y, f.world.position.z, f.root.x, f.root.y, f.root.z,
                        originMove, rootMove, originMove - rootMove, entityTurn, rootTurn, rebase ? 1 : 0);
                }
                std::printf("RAW_SUMMARY id=%s record=%08X frames=%zu max_gap=%u rebase_pairs=%u flagged_no_interp=%u\n",
                    Id(c->id).c_str(), r, frames.size(), maxGap, rebases, flagged);
            }
        }
        if (rawOnly)
            return 0;

        // Recorded game camera samples in the window (its own capture times).
        {
            std::uint32_t previousT = 0;
            for (const camera::GameSample& g : recording.gameCamera)
            {
                if (g.timeMs < fromMs || g.timeMs > toMs)
                    continue;
                std::printf("CAM t=%u gap=%u pos=%.3f;%.3f;%.3f rot=%.2f;%.2f;%.2f fov=%.1f\n", g.timeMs,
                    previousT ? g.timeMs - previousT : 0, g.position.x, g.position.y, g.position.z, g.rotation.x,
                    g.rotation.y, g.rotation.z, g.fov);
                previousT = g.timeMs;
            }
        }

        // PLAY: both sampled at the same playhead.
        pose::PoseSample riderSample, mountSample;
        pose::Vec3 camRel1{}, camRel2{};
        int camRelCount = 0;
        bool havePrevious = false;
        pose::Vec3 previousOffset{}, previousRootOffset{};
        float worstStep = 0.0f;
        std::uint32_t worstAt = 0;
        for (std::uint32_t t = fromMs; t <= toMs; t += stepMs)
        {
            auto riderTrack = tracks.find(recordAt(riderCreate, t));
            auto mountTrack = tracks.find(recordAt(mountCreate, t));
            pose::WorldTransform riderWorld{}, mountWorld{};
            if (riderTrack == tracks.end() || mountTrack == tracks.end() ||
                !riderTrack->second->Sample(t, riderSample) || !mountTrack->second->Sample(t, mountSample) ||
                !SampleWorld(riderSample, riderWorld) || !SampleWorld(mountSample, mountWorld))
            {
                std::printf("PLAY t=%u no_sample\n", t);
                havePrevious = false;
                continue;
            }
            const pose::Vec3 offset = ToLocal(mountWorld, riderWorld.position);
            const pose::Vec3 root = riderSample.objects.empty()
                ? riderWorld.position
                : pose::RootWorldPosition(riderWorld, riderSample.objects[0]);
            const pose::Vec3 rootOffset = ToLocal(mountWorld, root);
            // Highest rider bone above its origin (world up): the playback
            // "upright" test reads the head against the origin.
            float top = -1e9f;
            int topBone = -1;
            for (std::size_t n = 0; n < riderSample.objects.size(); ++n)
            {
                const pose::BoneQts& bone = riderSample.objects[n];
                const pose::Vec3 w = pose::Rotate(riderWorld.rotation, {bone.translation[0], bone.translation[1], bone.translation[2]});
                if (w.z > top)
                {
                    top = w.z;
                    topBone = static_cast<int>(n);
                }
            }
            const bool riderHold = riderSample.t == 0.0f && riderSample.frameTimeB > riderSample.frameTimeA &&
                t > riderSample.frameTimeA;
            const bool mountHold = mountSample.t == 0.0f && mountSample.frameTimeB > mountSample.frameTimeA &&
                t > mountSample.frameTimeA;
            const float step = havePrevious ? Dist3(offset, previousOffset) : 0.0f;
            const float rootStep = havePrevious ? Dist3(rootOffset, previousRootOffset) : 0.0f;
            if (rootStep > worstStep)
            {
                worstStep = rootStep;
                worstAt = t;
            }
            // Rider root seen from the recorded game camera: the second
            // difference of (root - camera) per step is the on-screen jerk.
            float camAccelCm = -1.0f;
            editor::CameraState cam{};
            if (camera::EvaluateGame(recording.gameCamera, t, cam))
            {
                const pose::Vec3 rel{root.x - cam.position.x, root.y - cam.position.y, root.z - cam.position.z};
                if (camRelCount >= 2)
                {
                    const pose::Vec3 a{rel.x - 2 * camRel1.x + camRel2.x, rel.y - 2 * camRel1.y + camRel2.y,
                        rel.z - 2 * camRel1.z + camRel2.z};
                    camAccelCm = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z) * 100.0f;
                }
                camRel2 = camRel1;
                camRel1 = rel;
                ++camRelCount;
            }
            else
                camRelCount = 0;
            std::printf("PLAY t=%u rider=%u/%u@%.2f%s mount=%u/%u@%.2f%s offset=%.3f;%.3f;%.3f root_offset=%.3f;%.3f;%.3f "
                        "offset_step_cm=%.1f root_step_cm=%.1f cam_accel_cm=%.2f top_m=%.2f top_bone=%d mount_pos=%.2f;%.2f;%.2f\n",
                t, riderSample.frameTimeA, riderSample.frameTimeB, riderSample.t, riderHold ? " HOLD" : "",
                mountSample.frameTimeA, mountSample.frameTimeB, mountSample.t, mountHold ? " HOLD" : "", offset.x, offset.y,
                offset.z, rootOffset.x, rootOffset.y, rootOffset.z, step * 100.0f, rootStep * 100.0f, camAccelCm, top, topBone, mountWorld.position.x,
                mountWorld.position.y, mountWorld.position.z);
            previousOffset = offset;
            previousRootOffset = rootOffset;
            havePrevious = true;
        }
        std::printf("PLAY_SUMMARY worst_root_step_cm=%.1f at=%u\n", worstStep * 100.0f, worstAt);
        return 0;
    }
}

namespace
{
    // The rider and mount pairs playback would make
    // (replay::jerk::MountPairs) over the whole clip, from the recorded
    // transforms at every step: enter and leave events with the rider's
    // seat in the mount's space. Checks the saddle box against real clips.
    int Pairs(const std::wstring& clipPath, std::uint32_t stepMs)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        std::map<std::uint32_t, std::unique_ptr<pose::PoseTrack>> tracks;
        for (const pose::PoseRecordExport& record : poses)
        {
            auto track = std::make_unique<pose::PoseTrack>();
            std::string importError;
            if (track->Import(record.track, importError))
                tracks[record.recordId] = std::move(track);
        }
        jerk::MountPairs pairs;
        std::vector<entity::CreatePacket> alive;
        std::vector<jerk::Body> bodies;
        pose::PoseSample sample;
        std::map<std::uint32_t, std::uint32_t> previous;
        std::uint64_t pairedSteps = 0;
        for (std::uint32_t t = 0; t <= durationMs; t += stepMs)
        {
            recording.lifecycle.AliveAt(t, alive);
            bodies.clear();
            for (const entity::CreatePacket& c : alive)
            {
                if (c.kind != entity::EntityKind::Ped && c.kind != entity::EntityKind::Horse)
                    continue;
                const entity::PoseSegment* segment = recording.segments.At(c.id, t);
                auto track = tracks.find(segment ? segment->recordId : c.id.Packed());
                pose::WorldTransform world{};
                if (track == tracks.end() || !track->second->Sample(t, sample) || !SampleWorld(sample, world))
                    continue;
                jerk::Body body;
                body.packed = c.id.Packed();
                body.horse = c.kind == entity::EntityKind::Horse;
                body.world = world;
                bodies.push_back(body);
            }
            pairs.Update(bodies);
            std::map<std::uint32_t, std::uint32_t> now(pairs.Map().begin(), pairs.Map().end());
            pairedSteps += now.size();
            const auto seat = [&](std::uint32_t rider, std::uint32_t mount) {
                pose::Vec3 local{};
                const jerk::Body* r = nullptr;
                const jerk::Body* m = nullptr;
                for (const jerk::Body& b : bodies)
                {
                    if (b.packed == rider) r = &b;
                    if (b.packed == mount) m = &b;
                }
                if (r && m)
                    local = jerk::InMountSpace(m->world, r->world.position);
                return local;
            };
            for (const auto& [rider, mount] : now)
            {
                const auto was = previous.find(rider);
                if (was == previous.end() || was->second != mount)
                {
                    const pose::Vec3 local = seat(rider, mount);
                    std::printf("PAIR t=%u phase=enter rider=%s mount=%s seat=%.2f;%.2f;%.2f\n", t,
                        Id(entity::ReplayId::FromPacked(rider)).c_str(), Id(entity::ReplayId::FromPacked(mount)).c_str(),
                        local.x, local.y, local.z);
                }
            }
            for (const auto& [rider, mount] : previous)
            {
                if (pairs.MountOf(rider) != mount)
                {
                    const pose::Vec3 local = seat(rider, mount);
                    std::printf("PAIR t=%u phase=leave rider=%s mount=%s seat=%.2f;%.2f;%.2f\n", t,
                        Id(entity::ReplayId::FromPacked(rider)).c_str(), Id(entity::ReplayId::FromPacked(mount)).c_str(),
                        local.x, local.y, local.z);
                }
            }
            previous.swap(now);
        }
        std::printf("PAIRS_SUMMARY enters=%llu leaves=%llu paired_steps=%llu step_ms=%u\n",
            static_cast<unsigned long long>(pairs.Enters()), static_cast<unsigned long long>(pairs.Leaves()),
            static_cast<unsigned long long>(pairedSteps), stepMs);
        return 0;
    }
}

namespace
{
    // Ropes: every rope record of a clip, its attachment and held events, and per
    // sample where the drawn window's two ends are: the recorded bone
    // (ped or horse, any index) nearest to each end and its distance. Says
    // whether the recorded vertices already have an end in a hand while
    // the attachment entries say nobody holds it.
    // Events: the timed audio and particle events
    // per entity, to compare a clip with what its playback did. Lines:
    // FOOT (footsteps per entity), SND (sound events per entity and kind),
    // ESND (entity sounds), FX (one-shot effects), RGFX (registered tracks
    // with their attach timeline). `filter` keeps one entity id ("1:71").
    // Sounds: the clip's sound census (SCEN) per owner and ref, and the
    // census-level captured sounds (GSND).
    // Sections: the clip's sections as the
    // file holds them (tag, name, version, payload bytes), then the load's
    // verdict with the optional sections the reader left out
    // (CLIP_SECTION_SKIPPED in the game's log) and the generic sound track's
    // counts. Exit 0: the clip opens; 2: it does not.
    // Signature: the skeleton signatures of the bodies of one or more clips, with the
    // mod's own comparison (skeleton_signature.h) and skeleton map.
    //
    // Every layout of every ped and horse pose record is put in a group by
    // model and bone count; inside a group the layouts are clustered by
    // signature (S1, S2, ...: compared bone for bone, a bone left out when
    // either side has it in object space or ambiguous). For each layout:
    // the cluster, the verdict of the arm against every other cluster of
    // its group (what the detector says when the puppet's skeleton is that
    // other signature), the skeleton map it would fall back to, and the
    // appearance in force when the layout starts (layers, fingerprint of the
    // ordered list). A record with several layouts also gets the bones its
    // later layouts demoted (the engine rebuilt them mid-recording).
    struct SignatureLayout
    {
        std::string clip;
        std::string entity;
        std::uint32_t model = 0;
        entity::EntityKind kind = entity::EntityKind::Ped;
        std::uint32_t recordId = 0;
        std::uint32_t layoutIndex = 0;
        std::uint32_t firstMs = 0;
        std::uint32_t lastMs = 0;
        pose::SkeletonLayout layout;
        std::uint32_t demoted = 0;
        const char* unusable = nullptr;
        std::size_t layers = 0;
        std::uint64_t appearancePrint = 0;
        std::size_t createLayers = 0;          // the create packet's (the puppet is dressed from it first)
        std::uint64_t createPrint = 0;
        int cluster = -1;
    };

    std::uint64_t OrderedAppearancePrint(const entity::Appearance& appearance)
    {
        std::uint64_t hash = 0xCBF29CE484222325ull;
        for (const entity::MetaPedAsset& asset : appearance.assets)
        {
            for (const std::uint32_t value : {asset.drawable, asset.albedo, asset.normal, asset.material})
            {
                for (int byte = 0; byte < 4; ++byte)
                {
                    hash ^= (value >> (byte * 8)) & 0xFF;
                    hash *= 0x100000001B3ull;
                }
            }
        }
        return hash;
    }

    // `a` against `b` as the arm compares a record with a puppet, both
    // sides' object-space and ambiguous bones left out.
    pose::SignatureCompare CompareLayouts(const pose::SkeletonLayout& a, const pose::SkeletonLayout& b)
    {
        std::vector<std::int32_t> parents = a.parent;
        std::vector<std::uint8_t> ambiguous(parents.size(), 0);
        std::vector<std::uint16_t> other(b.parent.size(), 0xFFFFu);
        for (std::size_t bone = 0; bone < parents.size() && bone < b.parent.size(); ++bone)
        {
            const bool ambA = bone < a.ambiguous.size() && a.ambiguous[bone];
            const bool ambB = bone < b.ambiguous.size() && b.ambiguous[bone];
            ambiguous[bone] = ambA || ambB ? 1 : 0;
            if (b.parent[bone] == pose::kUnresolvedParent)
                parents[bone] = pose::kUnresolvedParent;
            if (b.parent[bone] >= 0)
                other[bone] = static_cast<std::uint16_t>(b.parent[bone]);
        }
        return pose::CompareSkeletonSignature(parents, ambiguous, other.data(), static_cast<std::uint32_t>(other.size()));
    }

    int Signature(const std::vector<std::wstring>& clips, const std::string& filter)
    {
        std::vector<SignatureLayout> all;
        for (const std::wstring& clipPath : clips)
        {
            Recording recording;
            std::vector<pose::PoseRecordExport> poses;
            camera::Track cameraTrack;
            std::uint64_t durationMs = 0;
            std::string error;
            if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
            {
                std::fprintf(stderr, "load failed: %s\n", error.c_str());
                return 2;
            }
            const std::wstring nameWide = clipPath.substr(clipPath.find_last_of(L"\\/") + 1);
            std::string name;
            for (const wchar_t c : nameWide)
                name.push_back(c < 0x80 ? static_cast<char>(c) : '?');
            for (const pose::PoseRecordExport& record : poses)
            {
                entity::ReplayId owner = recording.segments.OwnerOf(record.recordId);
                const entity::PoseSegment* segment = nullptr;
                for (const entity::PoseSegment& candidate : recording.segments.Segments())
                    if (candidate.recordId == record.recordId)
                        segment = &candidate;
                if (!owner.Valid())
                    owner = entity::ReplayId::FromPacked(record.recordId);
                const entity::CreatePacket* create = nullptr;
                for (const entity::CreatePacket& candidate : recording.created)
                    if (candidate.id == owner)
                        create = &candidate;
                if (!create || (create->kind != entity::EntityKind::Ped && create->kind != entity::EntityKind::Horse))
                    continue;
                const std::string id = Id(owner);
                if (!filter.empty() && id != filter)
                    continue;
                const AppearanceTrack* changes = nullptr;
                for (const AppearanceTrack& track : recording.appearances)
                    if (track.id == owner.Packed())
                        changes = &track;
                std::vector<std::uint32_t> demotedBefore;
                for (std::size_t chunk = 0; chunk < record.track.chunks.size(); ++chunk)
                {
                    const std::uint32_t index = record.track.chunks[chunk].layout;
                    if (index >= record.track.layouts.size() || record.track.chunks[chunk].frameTimes.empty())
                        continue;
                    // One entry per layout run (consecutive chunks of one layout).
                    if (chunk > 0 && record.track.chunks[chunk - 1].layout == index)
                    {
                        all.back().lastMs = record.track.chunks[chunk].frameTimes.back();
                        continue;
                    }
                    SignatureLayout entry;
                    entry.clip = name;
                    entry.entity = id;
                    entry.model = create->modelHash;
                    entry.kind = create->kind;
                    entry.recordId = record.recordId;
                    entry.layoutIndex = index;
                    entry.firstMs = record.track.chunks[chunk].frameTimes.front();
                    entry.lastMs = record.track.chunks[chunk].frameTimes.back();
                    entry.layout = record.track.layouts[index];
                    for (const std::int32_t parent : entry.layout.parent)
                        entry.demoted += parent == pose::kUnresolvedParent ? 1u : 0u;
                    entry.unusable = pose::SignatureUnusable(entry.layout.parent, entry.layout.ambiguous);
                    const entity::Appearance* worn = segment ? &segment->appearance : &create->appearance;
                    if (changes)
                        for (const AppearanceChange& change : changes->changes)
                            if (change.timeMs <= entry.firstMs && (!segment || change.timeMs >= segment->startMs))
                                worn = &change.appearance;
                    entry.layers = worn->assets.size();
                    entry.createLayers = create->appearance.assets.size();
                    entry.createPrint = OrderedAppearancePrint(create->appearance);
                    entry.appearancePrint = OrderedAppearancePrint(*worn);
                    all.push_back(std::move(entry));
                }
            }
        }
        // Clusters per model and bone count.
        struct Cluster
        {
            std::uint32_t model = 0;
            std::uint32_t bones = 0;
            int label = 0;
            std::size_t representative = 0;
            std::uint32_t members = 0;
        };
        std::vector<Cluster> clusters;
        std::map<std::pair<std::uint32_t, std::uint32_t>, int> labels;
        for (std::size_t index = 0; index < all.size(); ++index)
        {
            SignatureLayout& entry = all[index];
            if (entry.unusable)
                continue;
            // The closest signature (fewest differing parents): a layout
            // with a demoted block agrees with both sides of it.
            std::uint32_t best = ~0u;
            for (std::size_t c = 0; c < clusters.size(); ++c)
            {
                if (clusters[c].model != entry.model || clusters[c].bones != entry.layout.boneCount)
                    continue;
                const pose::SignatureCompare compare = CompareLayouts(entry.layout, all[clusters[c].representative].layout);
                if (compare.verdict == pose::SignatureVerdict::Same && compare.differ < best)
                {
                    best = compare.differ;
                    entry.cluster = static_cast<int>(c);
                }
            }
            if (entry.cluster >= 0)
                ++clusters[static_cast<std::size_t>(entry.cluster)].members;
            if (entry.cluster < 0)
            {
                Cluster cluster;
                cluster.model = entry.model;
                cluster.bones = entry.layout.boneCount;
                cluster.label = ++labels[{entry.model, entry.layout.boneCount}];
                cluster.representative = index;
                cluster.members = 1;
                entry.cluster = static_cast<int>(clusters.size());
                clusters.push_back(cluster);
            }
        }
        std::uint32_t multi = 0;
        for (const auto& [key, count] : labels)
            multi += count > 1 ? 1u : 0u;
        std::printf("SIGNATURE_SUMMARY,layouts=%zu,clusters=%zu,groups=%zu,groups_with_more_than_one_signature=%u,"
                    "differ_limit=%u,ambiguous_limit=%u\n",
            all.size(), clusters.size(), labels.size(), multi, pose::kSignatureDifferLimit, pose::kSignatureAmbiguousLimit);
        for (const Cluster& cluster : clusters)
        {
            if (labels[{cluster.model, cluster.bones}] < 2)
                continue;
            const SignatureLayout& rep = all[cluster.representative];
            std::printf("CLUSTER model=%s bones=%u sig=S%d members=%u first=%s:%s\n", Hex(cluster.model).c_str(), cluster.bones,
                cluster.label, cluster.members, rep.clip.c_str(), rep.entity.c_str());
        }
        std::string lastRecord;
        std::vector<std::int32_t> previous;
        std::vector<std::uint8_t> resolvedBefore; // bones some earlier layout of the record parented
        for (const SignatureLayout& entry : all)
        {
            const std::string recordKey = entry.clip + "#" + std::to_string(entry.recordId);
            const bool multiSignature = labels[{entry.model, entry.layout.boneCount}] > 1;
            std::uint32_t newlyDemoted = 0;
            std::int32_t firstDemoted = -1;
            std::int32_t lastDemoted = -1;
            if (recordKey == lastRecord && previous.size() == entry.layout.parent.size())
            {
                for (std::size_t bone = 0; bone < previous.size(); ++bone)
                {
                    if (entry.layout.parent[bone] == pose::kUnresolvedParent && previous[bone] != pose::kUnresolvedParent)
                    {
                        ++newlyDemoted;
                        if (firstDemoted < 0)
                            firstDemoted = static_cast<std::int32_t>(bone);
                        lastDemoted = static_cast<std::int32_t>(bone);
                    }
                }
            }
            const bool layoutChange = recordKey == lastRecord;
            // PoseTrack::RebuiltBonesAt, from the export.
            if (!layoutChange)
                resolvedBefore.assign(entry.layout.parent.size(), 0);
            std::vector<std::uint8_t> rebuilt(entry.layout.parent.size(), 0);
            for (std::size_t bone = 0; bone < rebuilt.size() && bone < resolvedBefore.size(); ++bone)
                rebuilt[bone] = entry.layout.parent[bone] == pose::kUnresolvedParent && resolvedBefore[bone] ? 1 : 0;
            const std::uint32_t rebuiltCount = pose::CountFlags(rebuilt);
            for (std::size_t bone = 0; bone < resolvedBefore.size() && bone < entry.layout.parent.size(); ++bone)
                resolvedBefore[bone] = resolvedBefore[bone] || entry.layout.parent[bone] != pose::kUnresolvedParent ? 1 : 0;
            lastRecord = recordKey;
            previous = entry.layout.parent;
            if (!multiSignature && !layoutChange && filter.empty())
                continue; // one signature for its model: nothing to tell
            std::printf("LAYOUT clip=%s id=%s kind=%s model=%s record=0x%X layout=%u span=%u..%u bones=%u demoted=%u "
                        "ambiguous=%u usable=%s sig=%s layers=%zu appearance=%016llX create_layers=%zu create_appearance=%016llX",
                entry.clip.c_str(), entry.entity.c_str(), entity::KindText(entry.kind), Hex(entry.model).c_str(), entry.recordId,
                entry.layoutIndex, entry.firstMs, entry.lastMs, entry.layout.boneCount, entry.demoted,
                static_cast<unsigned>(std::count(entry.layout.ambiguous.begin(), entry.layout.ambiguous.end(), 1)),
                entry.unusable ? entry.unusable : "yes",
                entry.cluster >= 0 ? ("S" + std::to_string(clusters[static_cast<std::size_t>(entry.cluster)].label)).c_str() : "-",
                entry.layers, static_cast<unsigned long long>(entry.appearancePrint), entry.createLayers,
                static_cast<unsigned long long>(entry.createPrint));
            if (layoutChange)
                std::printf(" layout_change=1 newly_demoted=%u demoted_bones=%d..%d rebuilt=%u", newlyDemoted, firstDemoted,
                    lastDemoted, rebuiltCount);
            std::printf("\n");
            if (rebuiltCount > pose::kSignatureDifferLimit)
            {
                // The arm on this layout, whatever the puppet: the block goes
                // to the puppet's own pose (BuildRebuiltMap on the record's
                // first layout as the puppet's table).
                std::vector<std::int32_t> first(entry.layout.parent);
                for (std::size_t bone = 0; bone < first.size(); ++bone)
                    if (first[bone] < 0)
                        first[bone] = -1;
                const pose::SkeletonMap map = pose::BuildRebuiltMap(first.data(), static_cast<std::uint32_t>(first.size()), rebuilt);
                std::printf("  ARM verdict=differ anomaly=skeleton_signature_mismatch why=record_rebuilt rebuilt=%u action=%s "
                            "matched=%u rest=%u\n",
                    rebuiltCount, map.mode == pose::SkeletonMapMode::Refused ? "refused" : "mapped", map.matched, map.rest);
            }
            if (!filter.empty())
            {
                // --id: the parent table itself (offline analysis).
                std::printf("  PARENTS");
                for (const std::int32_t parent : entry.layout.parent)
                    std::printf(" %d", parent);
                std::printf("\n");
            }
            if (entry.unusable)
                continue;
            for (const Cluster& cluster : clusters)
            {
                if (cluster.model != entry.model || cluster.bones != entry.layout.boneCount)
                    continue;
                const SignatureLayout& other = all[cluster.representative];
                // The arm: this layout is the record, the other signature the puppet.
                const pose::SignatureCompare compare = CompareLayouts(entry.layout, other.layout);
                std::vector<std::int32_t> target(other.layout.parent.begin(), other.layout.parent.end());
                const pose::SkeletonMap map = pose::BuildSkeletonMap(entry.layout.parent.data(),
                    static_cast<std::uint32_t>(entry.layout.parent.size()), target.data(), static_cast<std::uint32_t>(target.size()));
                std::printf("  ARM puppet=S%d verdict=%s%s compared=%u differ=%u first_bone=%d last_bone=%d action=%s map=%s "
                            "matched=%u rest=%u groups=%u/%u reason=%s\n",
                    cluster.label, pose::SignatureVerdictText(compare.verdict),
                    compare.verdict == pose::SignatureVerdict::Differ ? " anomaly=skeleton_signature_mismatch" : "",
                    compare.compared, compare.differ, compare.firstDiff, compare.lastDiff,
                    compare.verdict != pose::SignatureVerdict::Differ ? "exact"
                        : map.mode == pose::SkeletonMapMode::Refused ? "refused"
                                                                     : "mapped",
                    pose::SkeletonMapModeText(map.mode), map.matched, map.rest, map.targetGroups, map.sourceGroups,
                    map.refuseReason);
            }
        }
        // Which ordered appearance came with which signature (the puppet is
        // dressed from the recorded list, and the engine orders the groups

        // the same way for the same list).
        std::map<std::pair<std::uint64_t, std::uint32_t>, std::vector<int>> byAppearance;
        for (const SignatureLayout& entry : all)
        {
            if (entry.cluster < 0 || labels[{entry.model, entry.layout.boneCount}] < 2)
                continue;
            std::vector<int>& seen = byAppearance[{entry.appearancePrint, entry.model}];
            const int label = clusters[static_cast<std::size_t>(entry.cluster)].label;
            if (std::find(seen.begin(), seen.end(), label) == seen.end())
                seen.push_back(label);
        }
        for (const auto& [key, seen] : byAppearance)
        {
            std::printf("APPEARANCE model=%s appearance=%016llX sigs=", Hex(key.second).c_str(),
                static_cast<unsigned long long>(key.first));
            for (std::size_t n = 0; n < seen.size(); ++n)
                std::printf("%sS%d", n ? "," : "", seen[n]);
            std::printf("\n");
        }
        return 0;
    }

    // The appearance recorded per ped and horse:
    // the create packet, every pose segment, the APPC changes and the full
    // metaped state (MPED). With an id, every layer; without, one line per
    // entity with the counts and the verdict appearance_not_recorded.
    void PrintAppearance(const char* what, std::uint32_t timeMs, const entity::Appearance& a, bool layers)
    {
        std::printf("  %s t=%u outfit=0x%08x layers=%zu\n", what, timeMs, a.outfit, a.assets.size());
        if (!layers)
            return;
        for (std::size_t n = 0; n < a.assets.size(); ++n)
        {
            const entity::MetaPedAsset& x = a.assets[n];
            std::printf("    [%2zu] drawable=0x%08x albedo=0x%08x normal=0x%08x material=0x%08x palette=0x%08x tints=%d/%d/%d\n",
                n, x.drawable, x.albedo, x.normal, x.material, x.palette, x.tint0, x.tint1, x.tint2);
        }
    }

    int AppearanceOf(const std::wstring& clipPath, const std::string& filter)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        const bool detail = !filter.empty();
        std::uint32_t entities = 0, notRecorded = 0;
        for (const entity::CreatePacket& create : recording.created)
        {
            if (create.kind != entity::EntityKind::Ped && create.kind != entity::EntityKind::Horse)
                continue;
            if (detail && Id(create.id) != filter)
                continue;
            ++entities;
            std::size_t segments = 0, maxLayers = create.appearance.assets.size();
            for (const entity::PoseSegment& segment : recording.segments.Segments())
                if (segment.entity == create.id)
                {
                    ++segments;
                    maxLayers = (std::max)(maxLayers, segment.appearance.assets.size());
                }
            const AppearanceTrack* track = FindAppearanceTrack(recording.appearances, create.id.Packed());
            if (track)
                for (const AppearanceChange& change : track->changes)
                    maxLayers = (std::max)(maxLayers, change.appearance.assets.size());
            const metaped::Track* full = metaped::FindTrack(recording.metapeds, create.id.Packed());
            // What the playback dresses the puppet with at its entry (the
            // lookahead covers a ped the engine still built), and the QA
            // rule appearance_not_recorded on it.
            const DressAppearance dress = DressAt(track, recording.segments.Segments(), create.id, nullptr,
                create.appearance, create.timeMs, kDressLookaheadMs);
            metaped::NotRecordedFacts facts;
            facts.layers = dress.appearance ? dress.appearance->assets.size() : 0;
            facts.outfit = dress.appearance ? dress.appearance->outfit : 0;
            facts.clipHasMetaped = !recording.metapeds.empty();
            facts.entityHasMetaped = full && !full->states.empty();
            const char* why = metaped::NotRecordedWhy(facts);
            if (why)
                ++notRecorded;
            char verdict[128] = "";
            if (why)
                std::snprintf(verdict, sizeof(verdict), " verdict=appearance_not_recorded why=%s", why);
            else if (dress.lookaheadMs)
                std::snprintf(verdict, sizeof(verdict), " dressed_from=%s lookahead_ms=%u", dress.from, dress.lookaheadMs);
            // The shop items of the first state (MPED v2), "unread" in
            // older clips.
            std::string shop = "unread";
            if (full && !full->states.empty() && (full->states.front().flags & metaped::kHasShopItems))
                shop = std::to_string(full->states.front().shopItems.size());
            std::printf("ENTITY id=%s kind=%s model=0x%08x create_ms=%u create_layers=%zu outfit=0x%08x segments=%zu "
                        "changes=%zu max_layers=%zu metaped_states=%zu shop_items=%s%s\n",
                Id(create.id).c_str(), create.kind == entity::EntityKind::Horse ? "horse" : "ped", create.modelHash,
                create.timeMs, create.appearance.assets.size(), create.appearance.outfit, segments,
                track ? track->changes.size() : std::size_t(0), maxLayers, full ? full->states.size() : std::size_t(0),
                shop.c_str(), verdict);
            if (!detail)
                continue;
            PrintAppearance("create", create.timeMs, create.appearance, true);
            for (const entity::PoseSegment& segment : recording.segments.Segments())
                if (segment.entity == create.id)
                {
                    char what[64];
                    std::snprintf(what, sizeof(what), "segment record=0x%08x bones=%u", segment.recordId, segment.boneCount);
                    PrintAppearance(what, segment.startMs, segment.appearance, true);
                }
            if (track)
                for (const AppearanceChange& change : track->changes)
                    PrintAppearance("change", change.timeMs, change.appearance, true);
            if (full)
                for (const metaped::State& state : full->states)
                    std::printf("%s", metaped::Describe(state, "  ").c_str());
        }
        std::printf("SUMMARY entities=%u appearance_not_recorded=%u metaped_tracks=%zu\n", entities, notRecorded,
            recording.metapeds.size());
        return 0;
    }

    // Every entity's pose records in order (the create record, then each
    // segment's) with the span each track covers. The verdict column says
    // which kinds follow later records (a vehicle with more than one record
    // must not stay hidden as no_pose once its first record ended).
    int SegmentsOf(const std::wstring& clipPath, const std::string& filter)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        // Each record's span, and its bytes against the track's byte budget
        // with the chunks the budget dropped from its start (a record that
        // hit the budget lost its oldest frames: QA 224, the player).
        struct Span
        {
            std::uint32_t first = 0, last = 0;
            std::uint64_t bytes = 0, budget = 0, dropped = 0;
        };
        std::map<std::uint32_t, Span> spans;
        for (const pose::PoseRecordExport& record : poses)
        {
            pose::PoseTrack track;
            std::string importError;
            if (track.Import(record.track, importError) && track.Stats().frames > 0)
                spans[record.recordId] = {track.Stats().firstTimeMs, track.Stats().lastTimeMs,
                    static_cast<std::uint64_t>(record.track.stats.bytes),
                    static_cast<std::uint64_t>(record.track.config.byteBudget),
                    static_cast<std::uint64_t>(record.track.stats.droppedChunks)};
        }
        const auto span = [&](std::uint32_t recordId) {
            char text[128];
            const auto found = spans.find(recordId);
            if (found == spans.end())
                std::snprintf(text, sizeof(text), "no_track");
            else
                std::snprintf(text, sizeof(text), "%u-%u bytes=%llu budget=%llu dropped_chunks=%llu",
                    found->second.first, found->second.last, static_cast<unsigned long long>(found->second.bytes),
                    static_cast<unsigned long long>(found->second.budget),
                    static_cast<unsigned long long>(found->second.dropped));
            return std::string(text);
        };
        std::uint32_t multi = 0, vehicles = 0;
        for (const entity::CreatePacket& create : recording.created)
        {
            if (!filter.empty() && Id(create.id) != filter)
                continue;
            std::vector<const entity::PoseSegment*> mine;
            for (const entity::PoseSegment& segment : recording.segments.Segments())
                if (segment.entity == create.id)
                    mine.push_back(&segment);
            if (mine.empty() && filter.empty())
                continue;
            ++multi;
            const bool vehicle = create.kind == entity::EntityKind::Vehicle;
            vehicles += vehicle ? 1u : 0u;
            std::printf("ENTITY id=%s kind=%s model=0x%08x create_ms=%u bones=%u records=%zu create_record=%s%s\n",
                Id(create.id).c_str(), entity::KindText(create.kind), create.modelHash, create.timeMs, create.boneCount,
                mine.size() + 1, span(create.id.Packed()).c_str(), vehicle && !mine.empty() ? " vehicle_multi_record=1" : "");
            for (const entity::PoseSegment* segment : mine)
                std::printf("  SEGMENT start_ms=%u record=0x%08x bones=%u span=%s\n", segment->startMs, segment->recordId,
                    segment->boneCount, span(segment->recordId).c_str());
        }
        std::printf("SUMMARY entities_with_segments=%u vehicles=%u\n", multi, vehicles);
        return 0;
    }

    // Every ped and horse record's (tpose_check.h)
    // pose stream read for the never animated signature (no pose entry after
    // the first frame over the whole record, ReadSignature), with its recorded
    // visibility (PVIS) when the clip has it. The bind pose is only in the
    // game; as a proxy, each record's first frame (and the middle one) is
    // compared with a reference record of the same bone count (--ref, by
    // default the clip's first never animated record): the bones turned past
    // 5 degrees from it (ref_posed). Records recorded in the bind pose agree
    // with each other; an animated body turns dozens of bones away.
    int NeverAnimatedOf(const std::wstring& clipPath, const std::string& refText)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        std::uint32_t refPacked = 0;
        if (!ParseTarget(refText, refPacked))
        {
            std::fprintf(stderr, "bad entity id: %s (slot:instance)\n", refText.c_str());
            return 1;
        }
        std::map<std::uint32_t, const entity::CreatePacket*> creates;
        for (const entity::CreatePacket& create : recording.created)
            creates[create.id.Packed()] = &create;
        // A record is its entity's (packed id) or a pose segment's.
        std::map<std::uint32_t, entity::ReplayId> owner;
        for (const entity::PoseSegment& segment : recording.segments.Segments())
            owner[segment.recordId] = segment.entity;
        std::map<std::uint32_t, const PedVisibility*> visibility;
        for (const PedVisibility& row : recording.pedVisibility)
            visibility[row.id.Packed()] = &row;

        struct Row
        {
            std::uint32_t recordId = 0;
            const entity::CreatePacket* create = nullptr;
            tpose::TrackSignature signature;
            std::unique_ptr<pose::PoseTrack> track;
            std::vector<pose::BoneQts> first;
            std::vector<pose::BoneQts> middle;
            float at[3] = {};   // the entity's world position at its first frame
            float rootZ = 0.0f; // the highest bone above the origin (m): about 1.7 standing, low lying
        };
        std::vector<Row> rows;
        for (const pose::PoseRecordExport& record : poses)
        {
            const auto ownerFound = owner.find(record.recordId);
            const std::uint32_t entityPacked = ownerFound != owner.end() ? ownerFound->second.Packed() : record.recordId;
            const auto createFound = creates.find(entityPacked);
            if (createFound == creates.end() || (createFound->second->kind != entity::EntityKind::Ped &&
                                                    createFound->second->kind != entity::EntityKind::Horse))
                continue;
            Row row;
            row.recordId = record.recordId;
            row.create = createFound->second;
            row.signature = tpose::ReadSignature(record.track);
            row.track = std::make_unique<pose::PoseTrack>();
            std::string importError;
            if (row.track->Import(record.track, importError) && row.signature.frames > 0)
            {
                pose::PoseSample sample;
                if (row.track->Sample(row.signature.firstMs, sample))
                {
                    row.first = sample.locals;
                    pose::WorldTransform world{};
                    if (SampleWorld(sample, world))
                    {
                        row.at[0] = world.position.x;
                        row.at[1] = world.position.y;
                        row.at[2] = world.position.z;
                        float top = -100.0f;
                        for (const pose::BoneQts& bone : sample.objects)
                        {
                            float p[3];
                            ToWorld(world, bone.translation, p);
                            top = (std::max)(top, p[2] - world.position.z);
                        }
                        row.rootZ = top;
                    }
                }
                if (row.track->Sample(row.signature.firstMs + tpose::SpanMs(row.signature) / 2u, sample))
                    row.middle = sample.locals;
            }
            rows.push_back(std::move(row));
        }
        const Row* reference = nullptr;
        for (const Row& row : rows)
        {
            if (refPacked ? row.create->id.Packed() == refPacked && row.recordId == refPacked
                          : tpose::NeverAnimated(row.signature) && !row.first.empty())
            {
                reference = &row;
                break;
            }
        }
        std::vector<pose::Quat> refRotations;
        if (reference)
            for (const pose::BoneQts& bone : reference->first)
                refRotations.push_back(bone.rotation);
        std::printf("NEVER_ANIMATED_REF,id=%s,record=0x%08x,bones=%zu\n",
            reference ? Id(reference->create->id).c_str() : "none", reference ? reference->recordId : 0u, refRotations.size());

        std::uint32_t never = 0, shortStill = 0, animated = 0, unreadable = 0;
        std::string neverIds;
        for (const Row& row : rows)
        {
            const tpose::TrackSignature& s = row.signature;
            const char* verdict = tpose::SignatureVerdict(s);
            if (tpose::NeverAnimated(s))
            {
                ++never;
                neverIds += (neverIds.empty() ? "" : ";") + Id(row.create->id);
            }
            else if (!s.parsed)
                ++unreadable;
            else if (s.poseFrames > 0)
                ++animated;
            else
                ++shortStill;
            // The fewest bones away from the reference over the record (every
            // 200 ms from 500 ms after its first frame, which often still
            // holds bind rows: pose_capture.cpp kUnwrittenWarmupSamples): how
            // close an animated body ever comes to it.
            std::string refMin = "-";
            if (!refRotations.empty() && row.create->boneCount == refRotations.size() && row.track)
            {
                std::uint32_t fewest = 0xFFFFFFFFu, fewestMs = 0, compared = 0;
                pose::PoseSample sample;
                for (std::uint32_t t = s.firstMs + 500; t <= s.lastMs && s.frames; t += 200)
                {
                    if (!row.track->Sample(t, sample) || sample.locals.size() != refRotations.size())
                        continue;
                    const tpose::BindMatch m = tpose::CompareLocalsWithBind(sample.locals, refRotations);
                    if (m.posed < fewest)
                    {
                        fewest = m.posed;
                        fewestMs = t;
                        compared = m.compared;
                    }
                }
                if (fewest != 0xFFFFFFFFu)
                {
                    char text[64];
                    std::snprintf(text, sizeof(text), "%u/%u@%u", fewest, compared, fewestMs);
                    refMin = text;
                }
            }
            const auto vis = visibility.find(row.create->id.Packed());
            char pvis[48];
            if (vis == visibility.end())
                std::snprintf(pvis, sizeof(pvis), "none");
            else
                std::snprintf(pvis, sizeof(pvis), "%u,pvis_flips=%zu", vis->second->visible, vis->second->flipsMs.size());
            const auto refText2 = [&](const std::vector<pose::BoneQts>& locals) {
                if (refRotations.empty() || locals.size() != refRotations.size())
                    return std::string("-");
                const tpose::BindMatch m = tpose::CompareLocalsWithBind(locals, refRotations);
                char text[64];
                std::snprintf(text, sizeof(text), "%u/%u,max_deg=%.1f", m.posed, m.compared, m.maxRad * 180.0f / kPi);
                return std::string(text);
            };
            char firstPose[16];
            if (s.firstPoseMs == 0xFFFFFFFFu)
                std::snprintf(firstPose, sizeof(firstPose), "-");
            else
                std::snprintf(firstPose, sizeof(firstPose), "%u", s.firstPoseMs);
            std::printf("NEVER_ANIMATED_RECORD,id=%s,record=0x%08x,kind=%s,model=0x%08x,bones=%u,frames=%llu,first_ms=%u,"
                        "span_ms=%u,pose_frames=%llu,first_pose_ms=%s,full_kf=%llu,rel_kf=%llu,world_frames=%llu,pvis=%s,"
                        "verdict=%s,at=%.2f;%.2f;%.2f,top_m=%.2f,ref_posed_first=%s,ref_posed_mid=%s,ref_posed_min=%s\n",
                Id(row.create->id).c_str(), row.recordId, entity::KindText(row.create->kind), row.create->modelHash,
                row.create->boneCount, static_cast<unsigned long long>(s.frames), s.firstMs, tpose::SpanMs(s),
                static_cast<unsigned long long>(s.poseFrames), firstPose, static_cast<unsigned long long>(s.fullKeyframes),
                static_cast<unsigned long long>(s.relativeKeyframes), static_cast<unsigned long long>(s.worldFrames), pvis,
                verdict, row.at[0], row.at[1], row.at[2], row.rootZ, refText2(row.first).c_str(), refText2(row.middle).c_str(),
                refMin.c_str());
        }
        std::printf("NEVER_ANIMATED_SUMMARY,body_records=%zu,never_animated=%u,still_short=%u,animated=%u,unreadable=%u,"
                    "pvis=%d,ids=%s\n",
            rows.size(), never, shortStill, animated, unreadable, recording.pedVisibility.empty() ? 0 : 1,
            neverIds.empty() ? "-" : neverIds.c_str());
        return 0;
    }

    int SectionsOf(const std::wstring& clipPath)
    {
        std::vector<std::uint8_t> raw;
        std::string error;
        if (!ReadPlainClip(clipPath, raw, error))
        {
            std::fprintf(stderr, "read failed: %s\n", error.c_str());
            return 2;
        }
        const std::vector<clipfile::SectionInfo>& known = clipfile::Sections();
        std::size_t offset = sizeof(clipfile::kMagic) + 4 + 8;
        std::uint32_t format = 0;
        std::uint64_t fileDuration = 0;
        if (raw.size() >= offset)
        {
            std::memcpy(&format, raw.data() + sizeof(clipfile::kMagic), 4);
            std::memcpy(&fileDuration, raw.data() + sizeof(clipfile::kMagic) + 4, 8);
        }
        std::printf("FILE,bytes=%zu,format=%u,duration_ms=%llu\n", raw.size(), format, static_cast<unsigned long long>(fileDuration));
        while (offset + 14 <= raw.size())
        {
            std::uint32_t tag = 0;
            std::uint16_t version = 0;
            std::uint64_t length = 0;
            std::memcpy(&tag, raw.data() + offset, 4);
            std::memcpy(&version, raw.data() + offset + 4, 2);
            std::memcpy(&length, raw.data() + offset + 6, 8);
            const char* name = "?";
            for (const clipfile::SectionInfo& info : known)
                if (info.tag == tag)
                    name = info.name;
            for (const clipfile::RetiredSectionInfo& retired : clipfile::RetiredSections())
                if (retired.tag == tag)
                    name = retired.name;
            char text[5] = {};
            std::memcpy(text, &tag, 4);
            for (char& c : text)
                if (c != 0 && (c < 0x20 || c >= 0x7F))
                    c = '?';
            std::printf("SECTION,tag=%s,name=%s,version=%u,bytes=%llu,at=%zu\n", text, name, version,
                static_cast<unsigned long long>(length), offset + 14);
            // A retired section (WGPU, GPU water) is not read by the loader;
            // listed as ignored.
            for (const clipfile::RetiredSectionInfo& retired : clipfile::RetiredSections())
                if (retired.tag == tag)
                    std::printf("SECTION_IGNORED,tag=%s,name=%s,bytes=%llu,why=%s\n", text, retired.name,
                        static_cast<unsigned long long>(length), retired.why);
            if (length > raw.size() - offset - 14)
            {
                std::printf("SECTION_TRUNCATED,tag=%s\n", text);
                break;
            }
            offset += 14 + static_cast<std::size_t>(length) + 8;
        }
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::vector<clipfile::SkippedSection> skipped;
        const bool ok = clipfile::Decode(raw.data(), raw.size(), 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack,
            durationMs, error, &skipped);
        for (const clipfile::SkippedSection& section : skipped)
            std::printf("CLIP_SECTION_SKIPPED,section=%s,reason=%s\n", section.section.c_str(), section.reason.c_str());
        const soundgeneric::Track& generic = recording.soundGeneric;
        std::printf("CLIP_LOAD,ok=%d,duration_ms=%llu,entities=%zu,poses=%zu,skipped=%zu,sound_generic_next=%u,"
                    "sound_generic_creates=%zu,sound_generic_adjusts=%zu,sound_generic_ends=%zu,error=%s\n",
            ok ? 1 : 0, static_cast<unsigned long long>(durationMs), recording.created.size(), poses.size(), skipped.size(),
            generic.nextIndex, generic.creates.size(), generic.adjusts.size(), generic.ends.size(), ok ? "none" : error.c_str());
        // The markers' shake as loaded (an old clip's
        // marker shake and events folded in).
        for (const camera::Keyframe& k : cameraTrack.Keyframes())
            if (k.shake.on || k.origin)
                std::printf("CLIP_SHAKE,id=%u,t=%llu,origin=%u,%s\n", k.id, static_cast<unsigned long long>(k.timeMs),
                    static_cast<unsigned>(k.origin), camera::mshake::FormatFields(k.shake).c_str());
        if (cameraTrack.Migration().ran)
            std::printf("CLIP_SHAKE_MIGRATION,events=%u,legacy_markers=%u,markers_added=%u\n", cameraTrack.Migration().events,
                cameraTrack.Migration().legacyMarkers, cameraTrack.Migration().markersAdded);
        return ok ? 0 : 2;
    }

    int Sounds(const std::wstring& clipPath)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        const census::Summary& summary = recording.soundCensus;
        std::printf("SCEN,keys=%zu,cells=%zu,covered_ms=%zu,overflow=%llu\n", summary.keys.size(), summary.cells.size(),
            summary.covered.size() * census::kWindowMs, static_cast<unsigned long long>(summary.overflow));
        std::vector<std::uint64_t> totals(summary.keys.size(), 0);
        std::vector<std::uint32_t> first(summary.keys.size(), UINT32_MAX);
        for (const census::SummaryCell& cell : summary.cells)
        {
            if (cell.key >= totals.size())
                continue;
            totals[cell.key] += cell.count;
            first[cell.key] = (std::min)(first[cell.key], cell.window * census::kWindowMs);
        }
        for (std::size_t k = 0; k < summary.keys.size(); ++k)
        {
            const census::SummaryKey& key = summary.keys[k];
            std::printf("SCEN_KEY,owner=%s,cls=%s,ref=0x%llx,count=%llu,first_ms=%u,c=%s\n", census::IdText(key.owner).c_str(),
                census::ClassToken(key.cls), static_cast<unsigned long long>(key.ref),
                static_cast<unsigned long long>(totals[k]), first[k], census::PathText(key.path).c_str());
        }
        for (const census::GeneralSound& sound : recording.generalSounds)
            std::printf("GSND,id=%s,cls=%s,ref=0x%llx,start=%u,end=%u,ended=%d,one_shot=%d,preexisting=%d,follows=%d,"
                        "audio=%s+0x%x,field=0x%x,vtable=0x%x,params=%zu,c=%s\n",
                census::IdText(sound.entity).c_str(), census::ClassToken(sound.cls), static_cast<unsigned long long>(sound.ref),
                sound.startMs, sound.endMs, sound.ended ? 1 : 0, sound.oneShot ? 1 : 0, sound.preexisting ? 1 : 0,
                sound.follows ? 1 : 0, census::BaseToken(sound.base), static_cast<unsigned>(sound.offset),
                static_cast<unsigned>(sound.field), sound.vtableRva, sound.params.size(), census::PathText(sound.path).c_str());
        // The static audio entities the clip names without a vtable (SNDS
        // globalAudio, ESND audioRva): playback plays them only when they are
        // census::kImageAudio (audio::TrustedImageAudio). unknown > 0: sounds
        // this build refuses.
        std::map<std::pair<std::string, std::uint64_t>, std::uint32_t> named;
        for (const entity::SoundEvent& sound : recording.sounds.Events())
            if (sound.globalAudio)
                ++named[{"snds", sound.globalAudio - 0x140000000ull}];
        for (const EntitySound& sound : recording.entitySounds)
            if (!sound.audioOnEntity)
                ++named[{"esnd", sound.audioRva}];
        std::uint32_t unknown = 0;
        for (const auto& [key, count] : named)
        {
            std::uint32_t vtable = 0;
            const bool known = census::KnownImageAudio(key.second, vtable);
            unknown += known ? 0 : count;
            std::printf("IMAGE_AUDIO,section=%s,rva=0x%llx,count=%u,known=%d,vtable=0x%x\n", key.first.c_str(),
                static_cast<unsigned long long>(key.second), count, known ? 1 : 0, vtable);
        }
        std::printf("IMAGE_AUDIO_TOTAL,named=%zu,unknown=%u\n", named.size(), unknown);
        return 0;
    }

    // Generic: the generic sound
    // packets (SGEN) of a clip, one line per create packet with the family
    // and the taker the owner table gives it (the same cover the player
    // builds from the clip's own tracks, sound_generic_player.cpp
    // BeginPlayback), the ends and the adjust count; optional owner and
    // time window filters. The offline view of "who owned this sound".
    // ParamsDiff: the dwords of a params copy that differ from what the
    // constructor 0x487E34 leaves, past the position and the init variables.
    std::string ValuesText(const soundgeneric::InitParams& values);
    std::string ParamsDiff(const std::uint8_t* p, std::size_t size)
    {
        soundgeneric::InitParams values;
        soundgeneric::ReadValueDwords(p, size, values);
        return ValuesText(values);
    }

    std::string ValuesText(const soundgeneric::InitParams& values)
    {
        if (!values.valuesKnown)
            return "unknown";
        std::string out;
        for (std::size_t n = 0; n < values.valueCount && n < soundgeneric::kValueDwords; ++n)
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "%s%03x=%08x", out.empty() ? "" : ";",
                static_cast<unsigned>(values.values[n].index) * 4u, values.values[n].raw);
            out += text;
        }
        return out.empty() ? "ctor" : out;
    }

    // Queries (entity_query_track.h): the recorded answers
    // of the owner queries (section EQRY): the chains with their known head
    // names, then every change of an answer per owner and chain.
    // Mix: the clip's mixer
    // scenes (MIXS: instances with their caller, kind, times and variables,
    // the flags), what the speech duck of the replay would get from them, and
    // the category state (CATS: per category its samples and volume range).
    int Mix(const std::wstring& clipPath)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        const mixscene::Track& mix = recording.mixScenes;
        std::printf("MIXS,instances=%zu,groups=%zu,vars=%zu,lost_stops=%u,flags=%u,duration_ms=%llu\n", mix.instances.size(),
            mix.groups.size(), mix.vars.size(), mix.lostStops, mix.flags, static_cast<unsigned long long>(durationMs));
        for (const mixscene::Instance& i : mix.instances)
        {
            std::size_t vars = 0;
            for (const mixscene::VarSample& v : mix.vars)
                vars += v.id == i.id ? 1 : 0;
            const mixscene::pool::Driver* driver = mixscene::pool::DriverByHash(i.hash);
            std::printf("MIXS_INSTANCE,id=%u,hash=0x%08x,caller=0x%x,kind=%s,start_ms=%u,stop_ms=%s,tags=%u,var_samples=%zu,driver=%s\n",
                i.id, i.hash, i.caller, mixscene::KindName(i.kind), i.startMs,
                i.stopMs == mixscene::kOpen ? "open" : std::to_string(i.stopMs).c_str(), i.tagCount, vars,
                driver ? driver->name : "none");
        }
        // The speech duck's inputs once a second (SpeechDuckDerivedFlags default).
        std::string duck;
        speechduck::Input last;
        bool have = false;
        for (std::uint64_t t = 0; t <= durationMs; t += 250)
        {
            const speechduck::Input in = speechduck::Plan(mix, static_cast<std::uint32_t>(t), speechduck::kDefaultDerivedFlags);
            if (have && speechduck::SameInput(in, last))
                continue;
            char item[96];
            std::snprintf(item, sizeof(item), "%s%llu:%s:%.2f:%s", duck.empty() ? "" : "|", static_cast<unsigned long long>(t),
                speechduck::SourceName(in.source), static_cast<double>(in.level), speechduck::FlagsText(in.flags).c_str());
            duck += item;
            last = in;
            have = true;
        }
        std::printf("SPEECH_DUCK_PLAN,changes=%s\n", duck.empty() ? "none" : duck.c_str());
        const catstate::Track& cats = recording.categoryState;
        std::printf("CATS,section=%s,samples=%zu,categories=%zu,static=%u,map=%u\n", cats.Empty() ? "none" : "CATS",
            cats.samples.size(), cats.Categories(), cats.staticCount, cats.mapCount);
        catstate::PlayIndex index;
        index.Build(cats);
        for (const catstate::Series& series : index.series)
        {
            float lo = series.points.front().second.volume, hi = lo;
            for (const auto& p : series.points)
            {
                lo = (std::min)(lo, p.second.volume);
                hi = (std::max)(hi, p.second.volume);
            }
            if (series.points.size() > 1 || lo != 0.0f)
                std::printf("CATS_CATEGORY,hash=0x%08x,samples=%zu,first_db=%.2f,min_db=%.2f,max_db=%.2f\n", series.hash,
                    series.points.size(), static_cast<double>(series.points.front().second.volume), static_cast<double>(lo),
                    static_cast<double>(hi));
        }
        return 0;
    }

    int Queries(const std::wstring& clipPath)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        const entityquery::Track& track = recording.entityQueries;
        const auto headName = [](std::uint32_t head) -> const char* {
            switch (head)
            {
            case 0xA07A1994u: return "mount";
            case 0x72277E17u: return "map_material";
            case 0x6DA26E2Fu: return "upper_clothing";
            case 0x8C90DADAu: return "lower_clothing";
            case 0xE68A4A98u: return "weapon";
            case 0x6910F770u: return "boat";
            case 0x442CD97Au: return "task_or_held_item";
            default: return "";
            }
        };
        std::printf("EQRY,chains=%zu,answers=%zu\n", track.chains.size(), track.events.size());
        for (std::size_t i = 0; i < track.chains.size(); ++i)
        {
            const entityquery::Chain& c = track.chains[i];
            std::string refs;
            char part[16]{};
            for (std::uint8_t n = 0; n < c.n; ++n)
            {
                std::snprintf(part, sizeof(part), "%s0x%08x", n ? ">" : "", c.refs[n]);
                refs += part;
            }
            std::printf("EQRY_CHAIN,index=%zu,n=%u,head=%s,chain=%s\n", i, c.n, headName(c.refs[0]), refs.c_str());
        }
        for (const entityquery::AnswerEvent& e : track.events)
            std::printf("EQRY_ANSWER,t=%u,owner=%u:%u,chain=%u,answer=0x%llx\n", e.timeMs, e.owner >> 16, e.owner & 0xFFFFu,
                e.chain, static_cast<unsigned long long>(e.answer));
        return 0;
    }

    // Render targets (render_target_core.h): the RTGT section, one line per
    // event, then the state at each event's instant and the UI app spans
    // that playback 2 would relaunch.
    int RenderTargets(const std::wstring& clipPath)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        const rendertarget::Track& track = recording.renderTargets;
        std::printf("RTGT,events=%zu,dropped=%u,duration_ms=%llu\n", track.events.size(), track.dropped,
            static_cast<unsigned long long>(durationMs));
        for (const rendertarget::Event& e : track.events)
        {
            const rendertarget::State state = rendertarget::StateAt(track, e.timeMs);
            const bool draws = e.kind == rendertarget::kAppOpen && rendertarget::AppDrawsTarget(track, e.hash, e.timeMs);
            std::printf("RTGT_EVENT,t=%u,kind=%s,name=%s,hash=0x%08x,entry=0x%08x,flag=%u,accepted=%u,initial=%u,"
                        "targets=%zu,links=%zu,apps=%zu%s\n",
                e.timeMs, rendertarget::KindName(e.kind), e.name[0] ? e.name : "-", e.hash, e.entry, e.flag, e.accepted,
                e.initial, state.targets.size(), state.links.size(), state.apps.size(),
                e.kind == rendertarget::kAppOpen ? (draws ? ",draws_target=1" : ",draws_target=0") : "");
        }
        return 0;
    }

    int Generic(const std::wstring& clipPath, const std::string& filter, std::uint32_t fromMs, std::uint32_t toMs)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        std::uint32_t packed = 0;
        if (!filter.empty() && (!ParseTarget(filter, packed) || !packed))
        {
            std::fprintf(stderr, "bad entity id: %s (slot:instance)\n", filter.c_str());
            return 1;
        }
        soundowner::Cover cover;
        for (const EntitySound& sound : recording.entitySounds)
            cover.Add(sound.entity.Packed(), sound.soundRef, sound.startMs, soundowner::Family::EntitySound);
        for (const entity::SoundEvent& sound : recording.sounds.Events())
            cover.Add(sound.entity.Packed(), sound.soundRef, sound.timeMs, soundowner::Family::SoundRequest);
        for (const CollisionSound& sound : recording.collisionSounds)
            cover.Add(sound.entity.Packed(), sound.soundRef, sound.startMs, soundowner::Family::Collision);
        for (const census::GeneralSound& sound : recording.generalSounds)
            cover.Add(sound.entity, sound.ref, sound.startMs, soundowner::Family::Captured);
        // The recorded shots by weapon (the
        // player's rule for weapon audio packets, soundowner::OffShot).
        soundowner::ShotWindows shots;
        for (const entity::ShotEvent& shot : recording.shots.Shots())
            shots.Add(shot.weapon.Packed(), shot.timeMs);
        shots.Seal();
        const soundgeneric::Track& track = recording.soundGeneric;
        std::vector<std::uint32_t> adjustCount(track.nextIndex, 0);
        for (const soundgeneric::AdjustPacket& a : track.adjusts)
            if (a.index < adjustCount.size())
                ++adjustCount[a.index];
        std::vector<std::string> endText(track.nextIndex);
        for (const soundgeneric::EndPacket& e : track.ends)
        {
            if (e.index >= endText.size())
                continue;
            endText[e.index] += (endText[e.index].empty() ? "" : ";") +
                std::string(e.reason == soundgeneric::EndReason::Release ? "release" : e.reason == soundgeneric::EndReason::Stop ? "stop" : "forget") +
                "@" + std::to_string(e.timeMs);
        }
        std::printf("SGEN_TRACK,creates=%zu,adjusts=%zu,ends=%zu,cover_rows=%zu,filter=%s,from_ms=%u,to_ms=%u\n", track.creates.size(),
            track.adjusts.size(), track.ends.size(), cover.Size(), filter.empty() ? "none" : filter.c_str(), fromMs, toMs);
        std::uint32_t perFamily[soundowner::kFamilyCount] = {};
        std::uint32_t listed = 0;
        for (const soundgeneric::CreatePacket& c : track.creates)
        {
            if (c.timeMs < fromMs || c.timeMs > toMs)
                continue;
            if (packed && c.owner != packed)
                continue;
            soundowner::PacketKey key;
            key.owner = c.owner;
            key.cls = c.ownerClass;
            key.stableId = c.stableId;
            key.caller = c.caller;
            key.timeMs = c.timeMs;
            key.audioKnown = c.audioKnown;
            key.audioBase = c.audioBase;
            key.callerInWrappers = soundgeneric::InAudioEntityWrappers(c.caller);
            const soundowner::Family family = soundowner::FamilyOf(key, cover, &shots);
            ++perFamily[static_cast<std::uint8_t>(family)];
            ++listed;
            std::printf("SGEN,t=%u,index=%u,id=0x%llx,id_kind=%s,raw=0x%llx,owner=%s,cls=%s,caller=0x%x,family=%s,taker=%s,"
                        "source=%s,delay_ms=%u,preexisting=%d,pos=%.2f;%.2f;%.2f,vol=%.2f,audio=%d,adjusts=%u,ends=%s\n",
                c.timeMs, c.index, static_cast<unsigned long long>(c.stableId),
                c.idKind == soundgeneric::IdKind::RefSet ? "refset" : c.idKind == soundgeneric::IdKind::Hash ? "hash" : "none",
                static_cast<unsigned long long>(c.rawRef), census::IdText(c.owner).c_str(), census::ClassToken(c.ownerClass), c.caller,
                soundowner::FamilyName(family), soundowner::TakerName(soundowner::DefaultTaker(family)),
                c.source == soundgeneric::Source::Queue ? "queue" : c.source == soundgeneric::Source::Deferred ? "deferred" : "direct",
                c.createDelayMs, c.preexisting ? 1 : 0, c.params.position[0], c.params.position[1], c.params.position[2],
                c.params.volume, c.audioKnown ? 1 : 0, c.index < adjustCount.size() ? adjustCount[c.index] : 0u,
                c.index < endText.size() && !endText[c.index].empty() ? endText[c.index].c_str() : "none");
            // The flags byte (+0x13A; 0x04 = the audio entity
            // updates the sound every frame) and the owner's audio entity.
            std::printf("SGEN_AUDIO,index=%u,flags=0x%02x,base=%u,field=0x%x,offset=0x%x,vtable=0x%x,env=%d,values=%s\n", c.index,
                static_cast<unsigned>(c.params.flags), static_cast<unsigned>(c.audioBase), static_cast<unsigned>(c.audioField),
                static_cast<unsigned>(c.audioOffset), c.vtableRva, c.params.hasEnvGroup ? 1 : 0, ValuesText(c.params).c_str());
            // The variables of the packet, the three of the
            // init params and every S38 adjust (hash and value), one line.
            std::string vars;
            for (std::size_t n = 0; n < c.params.variableCount && n < soundgeneric::kVariables; ++n)
            {
                char text[48]{};
                std::snprintf(text, sizeof(text), "%s0x%08x=%.3f", vars.empty() ? "" : ";", c.params.variables[n].hash,
                    c.params.variables[n].value);
                vars += text;
            }
            std::string adjusted;
            for (const soundgeneric::AdjustPacket& a : track.adjusts)
            {
                if (a.index != c.index || a.kind != soundgeneric::Adjust::Variable)
                    continue;
                char text[64]{};
                std::snprintf(text, sizeof(text), "%s%u:0x%08x=%.3f", adjusted.empty() ? "" : ";", a.timeMs, a.hash, a.value[0]);
                adjusted += text;
            }
            // Every non-position adjust of the packet (kind@time=value), one line.
            std::string others;
            for (const soundgeneric::AdjustPacket& a : track.adjusts)
            {
                if (a.index != c.index || a.kind == soundgeneric::Adjust::Variable || soundgeneric::IsVector(a.kind))
                    continue;
                char text[64]{};
                std::snprintf(text, sizeof(text), "%s%s@%u=%.3f", others.empty() ? "" : ";", soundgeneric::AdjustName(a.kind), a.timeMs,
                    a.value[0]);
                others += text;
            }
            if (!others.empty())
                std::printf("SGEN_ADJ,index=%u,%s\n", c.index, others.c_str());
            if (!vars.empty() || !adjusted.empty())
                std::printf("SGEN_VAR,index=%u,init=%s,s38=%s\n", c.index, vars.empty() ? "none" : vars.c_str(),
                    adjusted.empty() ? "none" : adjusted.c_str());
        }
        // The init variables (params +0x10, 16 pairs, count
        // +0x90) of the sound requests (SNDS) in the window, from the full
        // params the request track keeps.
        for (const entity::SoundEvent& sound : recording.sounds.Events())
        {
            if (sound.timeMs < fromMs || sound.timeMs > toMs || (packed && sound.entity.Packed() != packed) ||
                sound.params.size() < 0x94)
                continue;
            std::int32_t count = 0;
            std::memcpy(&count, sound.params.data() + 0x90, 4);
            std::string vars;
            for (std::int32_t n = 0; n < count && n < 16; ++n)
            {
                std::uint32_t hash = 0;
                float value = 0.0f;
                std::memcpy(&hash, sound.params.data() + 0x10 + n * 8, 4);
                std::memcpy(&value, sound.params.data() + 0x14 + n * 8, 4);
                char text[48]{};
                std::snprintf(text, sizeof(text), "%s0x%08x=%.3f", vars.empty() ? "" : ";", hash, value);
                vars += text;
            }
            std::printf("SNDS_VAR,t=%u,owner=%s,ref=0x%llx,kind=%u,count=%d,vars=%s\n", sound.timeMs,
                census::IdText(sound.entity.Packed()).c_str(), static_cast<unsigned long long>(sound.soundRef), sound.kind, count,
                vars.empty() ? "none" : vars.c_str());
            std::printf("SNDS_PARAMS,t=%u,ref=0x%llx,diff=%s\n", sound.timeMs, static_cast<unsigned long long>(sound.soundRef),
                ParamsDiff(sound.params.data(), sound.params.size()).c_str());
        }
        for (const EntitySound& sound : recording.entitySounds)
        {
            if (sound.startMs < fromMs || sound.startMs > toMs || (packed && sound.entity.Packed() != packed) ||
                sound.params.size() < 0x94)
                continue;
            std::int32_t count = 0;
            std::memcpy(&count, sound.params.data() + 0x90, 4);
            std::string vars;
            for (std::int32_t n = 0; n < count && n < 16; ++n)
            {
                std::uint32_t hash = 0;
                float value = 0.0f;
                std::memcpy(&hash, sound.params.data() + 0x10 + n * 8, 4);
                std::memcpy(&value, sound.params.data() + 0x14 + n * 8, 4);
                char text[48]{};
                std::snprintf(text, sizeof(text), "%s0x%08x=%.3f", vars.empty() ? "" : ";", hash, value);
                vars += text;
            }
            std::printf("ESND_VAR,t=%u,owner=%s,ref=0x%llx,count=%d,vars=%s\n", sound.startMs,
                census::IdText(sound.entity.Packed()).c_str(), static_cast<unsigned long long>(sound.soundRef), count,
                vars.empty() ? "none" : vars.c_str());
            std::printf("ESND_PARAMS,t=%u,ref=0x%llx,diff=%s\n", sound.startMs, static_cast<unsigned long long>(sound.soundRef),
                ParamsDiff(sound.params.data(), sound.params.size()).c_str());
        }
        {
            // What the player finds for the listed creates
            // (the packet's own init variables, else the other tracks' rows).
            soundgeneric::player::InitVariableIndex index;
            for (const entity::SoundEvent& sound : recording.sounds.Events())
                if (!sound.params.empty())
                    index.Add(sound.entity.Packed(), sound.soundRef, sound.timeMs, sound.params.data(), sound.params.size(), 1);
            for (const EntitySound& sound : recording.entitySounds)
                if (!sound.params.empty() && !sound.byHash)
                    index.Add(sound.entity.Packed(), sound.soundRef, sound.startMs, sound.params.data(), sound.params.size(), 2);
            for (const census::GeneralSound& sound : recording.generalSounds)
                if (!sound.params.empty())
                    index.Add(sound.entity, sound.ref, sound.startMs, sound.params.data(), sound.params.size(), 3);
            index.Seal();
            std::uint32_t own = 0, found[4] = {}, none = 0;
            for (const soundgeneric::CreatePacket& c : track.creates)
            {
                if (c.timeMs < fromMs || c.timeMs > toMs || (packed && c.owner != packed))
                    continue;
                soundgeneric::InitParams vars;
                std::uint8_t source = 0;
                if (c.params.initVariableCount)
                    ++own;
                else if (index.Find(c.owner, c.rawRef, c.timeMs, 25, vars, source))
                    ++found[source & 3];
                else
                    ++none;
            }
            std::printf("SGEN_INIT,rows=%zu,packet=%u,request=%u,entity_sound=%u,capture=%u,none=%u\n", index.Size(), own,
                found[1], found[2], found[3], none);
        }
        std::printf("SGEN_FAMILIES,listed=%u", listed);
        for (std::uint8_t n = 0; n < soundowner::kFamilyCount; ++n)
            if (perFamily[n])
                std::printf(",%s=%u(%s)", soundowner::FamilyName(static_cast<soundowner::Family>(n)), perFamily[n],
                    soundowner::TakerName(soundowner::DefaultTaker(static_cast<soundowner::Family>(n))));
        std::printf("\n");
        return 0;
    }

    // Vehicles: the recorded vehicles and train
    // cars: kind, model, consist place, look, pose bones and the door tracks
    // (VDOR): per door the samples, the first opening and the widest ratio.
    int Vehicles(const std::wstring& clipPath)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        std::uint32_t vehicles = 0, cars = 0, posedCars = 0, withDoors = 0, opening = 0;
        for (const entity::CreatePacket& create : recording.created)
        {
            if (create.kind != entity::EntityKind::Vehicle && create.kind != entity::EntityKind::TrainCar)
                continue;
            const bool car = create.kind == entity::EntityKind::TrainCar;
            (car ? cars : vehicles) += 1;
            posedCars += car && create.boneCount >= 2 ? 1u : 0u;
            std::printf("VEHICLE,id=%s,kind=%s,model=0x%08X,create_ms=%u,bones=%u", entity::IdText(create.id).c_str(),
                car ? "train_car" : "vehicle", static_cast<unsigned>(create.modelHash), create.timeMs, create.boneCount);
            if (const trains::TrainCar* info = recording.trains.Find(create.id.Packed()))
                std::printf(",index=%u,cars=%u,type=%u,head=%s,propset=0x%X,light=0x%X", info->index, info->cars,
                    info->vehicleType, entity::IdText(entity::ReplayId::FromPacked(info->head)).c_str(), info->propSet,
                    info->lightPropSet);
            for (const VehicleLook& look : recording.vehicleLooks)
                if (look.id == create.id)
                    std::printf(",look_propset=0x%X,look_light=0x%X,extras=0x%X,extras_on=0x%X,tint=%d,livery=%d", look.propSet,
                        look.lightPropSet, look.extrasExist, look.extrasOn, look.tint, look.livery);
            const vehdoors::DoorTrack* doors = nullptr;
            for (const vehdoors::DoorTrack& track : recording.vehicleDoors)
                if (track.id == create.id.Packed())
                    doors = &track;
            if (!doors)
            {
                std::printf(",doors=none\n");
                continue;
            }
            ++withDoors;
            opening += vehdoors::EverOpen(*doors) ? 1u : 0u;
            std::printf(",door_samples=%zu\n", doors->samples.size());
            for (std::uint8_t channel = 0; channel < vehdoors::kChannels; ++channel)
            {
                std::uint32_t samples = 0, firstOpenMs = 0;
                bool opened = false;
                float widest = 0.0f, first = -1.0f;
                for (const vehdoors::DoorSample& sample : doors->samples)
                {
                    if (sample.door != channel)
                        continue;
                    ++samples;
                    if (first < 0.0f)
                        first = sample.ratio;
                    widest = (std::max)(widest, sample.ratio);
                    if (!opened && sample.ratio >= vehdoors::kStoreStep)
                    {
                        opened = true;
                        firstOpenMs = sample.timeMs;
                    }
                }
                if (!samples)
                    continue;
                std::printf("  DOOR,id=%s,door=%s,samples=%u,first=%.3f,widest=%.3f,first_open_ms=%s\n",
                    entity::IdText(create.id).c_str(),
                    channel == vehdoors::kLatchChannel ? "latch" : std::to_string(channel).c_str(), samples, first, widest,
                    opened ? std::to_string(firstOpenMs).c_str() : "-");
            }
        }
        std::printf("SUMMARY vehicles=%u train_cars=%u posed_cars=%u with_doors=%u doors_opening=%u door_tracks=%zu\n",
            vehicles, cars, posedCars, withDoors, opening, recording.vehicleDoors.size());
        return 0;
    }

    // Shots: the shot track's weapon effects audited (shared claims, impacts on
    // a body, pellets) and what the legacy rule (ShotFxLegacyDedupe) plays.
    int Shots(const std::wstring& clipPath)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        bool weaponEvents = false;
        std::vector<std::uint32_t> bodyTimes;
        std::map<std::uint32_t, std::uint32_t> bySite;
        for (const entity::FxEvent& fx : recording.fx.Events())
        {
            if (fx.callerSite == replay::shotfx::kMuzzleSite)
                weaponEvents = true;
            if (fx.callerSite == replay::shotfx::kBodySite)
                bodyTimes.push_back(fx.timeMs);
            if (fx.callerSite == replay::shotfx::kMuzzleSite || fx.callerSite == replay::shotfx::kImpactSite ||
                fx.callerSite == replay::shotfx::kBodySite || fx.callerSite == 0x146098D || fx.callerSite == 0x1467AEC)
                ++bySite[fx.callerSite];
        }
        std::vector<replay::shotfx::Shot> shots;
        for (const entity::ShotEvent& shot : recording.shots.Shots())
        {
            replay::shotfx::Shot row;
            row.timeMs = shot.timeMs;
            row.shooter = shot.shooter.Packed();
            row.player = shot.shooterPlayer == 2;
            row.muzzleFx = shot.muzzleFx;
            row.impactFx = shot.impactFx;
            row.hasImpact = shot.hasImpact;
            shots.push_back(row);
        }
        replay::shotfx::Audit audit;
        audit.weaponEvents = weaponEvents;
        const std::vector<replay::shotfx::Decision> decisions = replay::shotfx::Decide(shots, bodyTimes, !weaponEvents, audit);
        for (std::size_t n = 0; n < shots.size(); ++n)
        {
            const replay::shotfx::Shot& shot = shots[n];
            const replay::shotfx::Decision& d = decisions[n];
            std::printf("SHOT,t=%u,shooter=%s,player=%d,muzzle=0x%08x,impact=0x%08x,has_impact=%d,shared_muzzle=%d,shared_impact=%d,"
                        "on_body=%d,pellet=%d,legacy_muzzle=%s,legacy_impact=%s\n",
                shot.timeMs, Id(entity::ReplayId::FromPacked(shot.shooter)).c_str(), shot.player ? 1 : 0, shot.muzzleFx, shot.impactFx,
                shot.hasImpact ? 1 : 0, d.sharedMuzzle ? 1 : 0, d.sharedImpact ? 1 : 0, d.onBody ? 1 : 0, d.pellet ? 1 : 0,
                weaponEvents ? "events" : d.playMuzzle ? "play" : "skip", weaponEvents ? "events" : d.playImpact ? "play" : "skip");
        }
        for (const auto& [site, count] : bySite)
            std::printf("FX_WEAPON_SITE,site=0x%x,events=%u\n", site, count);
        std::string name;
        for (const wchar_t c : clipPath.substr(clipPath.find_last_of(L"\\/") == std::wstring::npos ? 0 : clipPath.find_last_of(L"\\/") + 1))
            name.push_back(c > 0x20 && c < 0x7F ? static_cast<char>(c) : '_');
        std::printf("%s\n", audit.Line(name).c_str());
        return 0;
    }

    int Events(const std::wstring& clipPath, const std::string& filter)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        const auto keep = [&](entity::ReplayId id) { return filter.empty() || Id(id) == filter; };
        std::map<std::string, std::pair<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>>> feet;
        for (const entity::FootstepEvent& step : recording.footsteps.Events())
        {
            auto& entry = feet[Id(step.entity)];
            if (!entry.first++)
                entry.second.first = step.timeMs;
            entry.second.second = step.timeMs;
            if (!filter.empty() && keep(step.entity))
                std::printf("FOOT_EVENT,id=%s,t=%u,event=%d,foot=%u,arg3=%u,arg5=%u,flag=%u,arg7=%u,arg8=%u\n",
                    Id(step.entity).c_str(), step.timeMs, step.eventId, step.foot, step.arg3, step.arg5, step.flag,
                    step.arg7, step.arg8);
        }
        for (const auto& [id, entry] : feet)
            if (filter.empty() || id == filter)
                std::printf("FOOT,id=%s,count=%u,first_ms=%u,last_ms=%u\n", id.c_str(), entry.first, entry.second.first,
                    entry.second.second);
        // The secondary
        // steps (section FSEV) per entity, with their types; with a filter,
        // each event.
        {
            struct Tally
            {
                std::uint32_t count = 0, first = 0, last = 0;
                replay::footevent::WindowCounts types;
            };
            std::map<std::string, Tally> secondary;
            for (const replay::footevent::Event& e : recording.footEvents.events)
            {
                const entity::ReplayId id = entity::ReplayId::FromPacked(e.owner);
                Tally& t = secondary[Id(id)];
                if (!t.count++)
                    t.first = e.timeMs;
                t.last = e.timeMs;
                t.types.AddType(e.type);
                if (!filter.empty() && keep(id))
                    std::printf("FSEV_EVENT,id=%s,t=%u,type=%d,foot=%u,src=%s,material=0x%08x/0x%08x,impact=%s\n",
                        Id(id).c_str(), e.timeMs, static_cast<int>(e.type), e.foot,
                        replay::footevent::OriginName(static_cast<replay::footevent::Origin>(e.origin)), e.materialHash,
                        e.materialRef, (e.flags & replay::footevent::kFlagImpact) ? std::to_string(e.impact).c_str() : "none");
            }
            for (const auto& [id, t] : secondary)
                if (filter.empty() || id == filter)
                    std::printf("FSEV,id=%s,count=%u,first_ms=%u,last_ms=%u,types=%s\n", id.c_str(), t.count, t.first, t.last,
                        replay::footevent::TypesText(t.types).c_str());
            if (filter.empty())
                std::printf("FSEV_TOTAL,section=%s,events=%zu,owners=%zu\n", recording.footEvents.Empty() ? "none" : "FSEV",
                    recording.footEvents.events.size(), recording.footEvents.Owners());
        }
        std::map<std::string, std::uint32_t> sounds;
        for (const entity::SoundEvent& sound : recording.sounds.Events())
        {
            if (!keep(sound.entity))
                continue;
            char key[96]{};
            std::snprintf(key, sizeof(key), "%s,kind=%u,ref=0x%llx", Id(sound.entity).c_str(), sound.kind,
                static_cast<unsigned long long>(sound.soundRef));
            ++sounds[key];
        }
        for (const auto& [key, count] : sounds)
            std::printf("SND,id=%s,count=%u\n", key.c_str(), count);
        for (const EntitySound& sound : recording.entitySounds)
            if (keep(sound.entity))
                std::printf("ESND,id=%s,start=%u,end=%u,ref=0x%llx,by_hash=%d,ped_audio=%d,one_shot=%d,offset=0x%x,caller=0x%x\n",
                    Id(sound.entity).c_str(), sound.startMs, sound.endMs, static_cast<unsigned long long>(sound.soundRef),
                    sound.byHash ? 1 : 0, sound.pedAudio ? 1 : 0, sound.oneShot ? 1 : 0, sound.audioOffset, sound.caller);
        for (const entity::FxEvent& fx : recording.fx.Events())
            if (keep(fx.entity))
            {
                // Zoom, the first evolutions and the settings (FXSS).
                std::printf("FX,t=%u,hash=0x%08x,caller=0x%x,id=%s,bone=%d,pos=%.2f;%.2f;%.2f,flags=0x%x,zoom=%.3f,evo=%u/%u/%u/%u",
                    fx.timeMs, fx.effectHash, fx.callerSite, fx.entity.Valid() ? Id(fx.entity).c_str() : "world", fx.bone,
                    fx.matrix[12], fx.matrix[13], fx.matrix[14], fx.flags, fx.zoom, fx.evolutions[0], fx.evolutions[1],
                    fx.evolutions[2], fx.evolutions[3]);
                if (fx.hasSettings)
                    std::printf(",settings=0x%x,tele=%.2f,lod=%.3f,aux39c=%.3f,cull=%u,alpha=%.3f,tint=%.3f;%.3f;%.3f,ev5cc=0x%x\n",
                        fx.settings.mask, fx.settings.tele3A8, fx.settings.lod3A0, fx.settings.aux39C, fx.settings.cull3BC,
                        fx.settings.alpha38C, fx.settings.tint380[0], fx.settings.tint380[1], fx.settings.tint380[2], fx.settings.ev5CC);
                else
                    std::printf(",settings=none\n");
            }
        for (const entity::RegisteredFxTrack& track : recording.registeredFx)
        {
            bool mine = keep(track.attachId);
            for (const entity::RegisteredFxAttach& change : track.attachTimeline)
                mine = mine || keep(change.id);
            if (!mine)
                continue;
            std::printf("RGFX,hash=0x%08x,caller=0x%x,start=%u,end=%u,frames=%zu,attached=%d,id=%s,bone=%d,model=0x%x,timeline=",
                track.effectHash, track.callerSite, track.startMs, track.endMs, track.frames.size(), track.attached ? 1 : 0,
                track.attachId.Valid() ? Id(track.attachId).c_str() : "none", track.attachBone, track.attachModel);
            for (const entity::RegisteredFxAttach& change : track.attachTimeline)
                std::printf("%u@%s/%d;", change.timeMs, Id(change.id).c_str(), change.bone);
            // RGSS: settings changes and call gaps, or the
            // gaps the playback derives from the frame times of a clip without them.
            std::vector<std::uint32_t> times;
            for (const entity::RegisteredFxFrame& frame : track.frames)
                times.push_back(frame.timeMs);
            const std::size_t derived = track.callGaps.empty() && track.attached && track.attachId.Valid()
                ? replay::fxmotion::DeriveCallGaps(times).size() : 0; // as the playback (puppet tracks only)
            std::printf(",settings=%zu,gaps=%zu,derived_gaps=%zu", track.settings.size(), track.callGaps.size(), derived);
            if (!track.settings.empty())
                std::printf(",tele=%.2f", track.settings.front().settings.tele3A8);
            std::printf("\n");
        }
        return 0;
    }

    // Objects (object_check.h): the clip's objects as
    // the playback sees them: every weapon and prop create (origin, weapon,
    // components, bones, track span and where it starts and ends), the
    // world objects with their stand-in origin, the recorded breaks per
    // object, the scenery (objects of the scene that are not of the map) and
    // the verdicts a QA would otherwise find by eye: weapon_plain (a weapon
    // object rebuilt as its bare model: the knife without its grip) and
    // frag_unplayed (a break recorded on something that is not a world
    // object prop, which the playback never applies).
    int Objects(const std::wstring& clipPath, const std::string& filter)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        const std::map<std::uint32_t, std::uint32_t> weaponModels = objcheck::WeaponModels(recording.created);
        std::map<std::uint32_t, std::uint32_t> poseBones;
        for (const pose::PoseRecordExport& record : poses)
            poseBones[record.recordId] = record.boneCount;
        std::map<std::uint32_t, std::uint32_t> breaksOf;
        for (const FragBreak& e : recording.fragBreaks)
            ++breaksOf[e.id.Packed()];
        std::uint32_t weaponPlain = 0, fragUnplayed = 0, objects = 0;
        for (const entity::CreatePacket& create : recording.created)
        {
            if (create.kind != entity::EntityKind::Weapon && create.kind != entity::EntityKind::Prop)
                continue;
            if (!filter.empty() && Id(create.id) != filter)
                continue;
            ++objects;
            const bool plain = objcheck::WeaponPlain(create, weaponModels);
            weaponPlain += plain ? 1u : 0u;
            std::string span = "none";
            if (entity::RelativeTrack* track = recording.WeaponTrackOf(create.id.Packed()))
            {
                std::uint32_t first = 0, last = 0;
                entity::TransformSample a, b;
                if (track->Span(first, last) && track->Sample(first, a) && track->Sample(last, b))
                {
                    char text[192];
                    std::snprintf(text, sizeof(text), "%u..%u,samples=%zu,from=%.2f;%.2f;%.2f,to=%.2f;%.2f;%.2f", first, last,
                        track->Size(), a.relative.position.x, a.relative.position.y, a.relative.position.z,
                        b.relative.position.x, b.relative.position.y, b.relative.position.z);
                    span = text;
                }
            }
            const auto bones = poseBones.find(create.id.Packed());
            const auto breaks = breaksOf.find(create.id.Packed());
            std::printf("OBJECT,id=%s,kind=%s,origin=%s,model=0x%08x,owner=%s,create_ms=%u,weapon=0x%08x,components=%zu,"
                        "create_bones=%u,pose_bones=%u,breaks=%u,track=%s%s\n",
                Id(create.id).c_str(), create.kind == entity::EntityKind::Weapon ? "weapon" : "prop",
                create.kind == entity::EntityKind::Prop ? objcheck::OriginText(create.propOrigin) : "attached",
                create.modelHash, create.owner.Valid() ? Id(create.owner).c_str() : "none", create.timeMs, create.weaponHash,
                create.weaponComponents.size(), create.boneCount, bones == poseBones.end() ? 0u : bones->second,
                breaks == breaksOf.end() ? 0u : breaks->second, span.c_str(),
                plain ? ",verdict=weapon_plain" : "");
        }
        for (const WorldObjectOrigin& origin : recording.worldObjects)
            if (filter.empty() || Id(origin.id) == filter)
                std::printf("WORLD_OBJECT,id=%s,model=0x%08x,pos=%.2f;%.2f;%.2f,rot=%.1f;%.1f;%.1f\n", Id(origin.id).c_str(),
                    origin.model, origin.position[0], origin.position[1], origin.position[2], origin.rotation[0],
                    origin.rotation[1], origin.rotation[2]);
        for (const FragBreak& e : recording.fragBreaks)
        {
            const entity::CreatePacket* create = recording.FindCreate(e.id.Packed());
            const bool worldProp = create && create->kind == entity::EntityKind::Prop &&
                create->propOrigin == entity::PropOrigin::WorldObject;
            const bool vehicle = create && create->kind == entity::EntityKind::Vehicle;
            const bool unplayed = !worldProp && !vehicle;
            fragUnplayed += unplayed ? 1u : 0u;
            if (filter.empty() || Id(e.id) == filter)
                std::printf("FRAG,t=%u,id=%s,group=%u,on=%s%s\n", e.timeMs, Id(e.id).c_str(), e.group,
                    worldProp ? "world_object" : vehicle ? "vehicle" : create ? "other" : "unknown",
                    unplayed ? ",verdict=frag_unplayed" : "");
        }
        std::map<std::uint32_t, std::uint32_t> owners;
        std::uint32_t taken = 0, gone = 0, late = 0;
        std::uint32_t byRule[objcheck::kSceneryRuleCount]{}; // SCNY v3
        for (const SceneryObject& object : recording.scenery)
        {
            ++owners[object.owner];
            ++byRule[object.rule < objcheck::kSceneryRuleCount ? object.rule : 0];
            taken += object.prop.Valid() ? 1u : 0u;
            gone += object.goneMs != objcheck::kNeverGone ? 1u : 0u;
            late += object.appearMs > 0 ? 1u : 0u;
            if (filter == "scenery")
                std::printf("SCENERY,model=0x%08x,pos=%.2f;%.2f;%.2f,appear_ms=%u,gone_ms=%d,prop=%s,owner=%u,mission=%u,"
                            "visible=%u,visibility_flips=%zu,rule=%s,ipl=0x%08x\n", // SCNY v3
                    object.model, object.position[0], object.position[1], object.position[2], object.appearMs,
                    object.goneMs == objcheck::kNeverGone ? -1 : static_cast<int>(object.goneMs),
                    object.prop.Valid() ? Id(object.prop).c_str() : "none", object.owner, object.mission, object.visible,
                    object.visibleFlipsMs.size(),
                    objcheck::SceneryRuleText(static_cast<objcheck::SceneryRule>(
                        object.rule < objcheck::kSceneryRuleCount ? object.rule : 0)),
                    object.ipl);
        }
        // The scenery's children (lining, loot in a chest).
        std::uint32_t childrenLate = 0, childrenGone = 0, childrenFramed = 0;
        for (const SceneryChild& child : recording.sceneryChildren)
        {
            childrenLate += child.appearMs > 0 ? 1u : 0u;
            childrenGone += child.goneMs != objcheck::kNeverGone ? 1u : 0u;
            childrenFramed += child.hasFrame ? 1u : 0u;
            if (filter == "scenery")
            {
                const std::uint32_t parentModel =
                    child.parent < recording.scenery.size() ? recording.scenery[child.parent].model : 0u;
                std::printf("SCENERY_CHILD,model=0x%08x,parent=%u,parent_model=0x%08x,frame=%u,bone=%d,offset=%.3f;%.3f;%.3f,"
                            "pos=%.2f;%.2f;%.2f,appear_ms=%u,gone_ms=%d,visible=%u,visibility_flips=%zu\n",
                    child.model, child.parent, parentModel, child.hasFrame, child.frameBone, child.relativePosition[0],
                    child.relativePosition[1], child.relativePosition[2], child.position[0], child.position[1],
                    child.position[2], child.appearMs, child.goneMs == objcheck::kNeverGone ? -1 : static_cast<int>(child.goneMs),
                    child.visible, child.visibleFlipsMs.size());
            }
        }
        // The custom texture (document text)
        // and tint changes per object (OLOK), and per scenery entry.
        const auto lookLine = [](const char* tag, const std::string& id, const ObjectLookTrack& track) {
            for (const auto& [timeMs, look] : track.states)
                std::printf("%s,id=%s,t=%u,txd=0x%08x,p2=0x%08x,p3=0x%08x,tint=%u\n", tag, id.c_str(), timeMs, look.txd, look.p2,
                    look.p3, static_cast<unsigned>(look.tint));
        };
        for (const ObjectLookTrack& track : recording.objectLooks)
        {
            const std::string id = Id(entity::ReplayId::FromPacked(track.id));
            if (filter.empty() || id == filter)
                lookLine("OBJECT_LOOK", id, track);
        }
        if (filter == "scenery")
            for (const ObjectLookTrack& track : recording.sceneryObjectLooks)
                lookLine("SCENERY_LOOK", "scenery:" + std::to_string(track.id), track);
        std::printf("SUMMARY,objects=%u,world_objects=%zu,frag_breaks=%zu,scenery=%zu,scenery_taken=%u,scenery_gone=%u,"
                    "scenery_late=%u,owners=",
            objects, recording.worldObjects.size(), recording.fragBreaks.size(), recording.scenery.size(), taken, gone, late);
        for (const auto& [owner, count] : owners)
            std::printf("%u:%u|", owner, count);
        std::printf(",weapon_plain=%u,frag_unplayed=%u,scenery_children=%zu,children_late=%u,children_gone=%u,children_framed=%u,"
                    "look_tracks=%zu,scenery_look_tracks=%zu,scenery_by_rule=",
            weaponPlain, fragUnplayed, recording.sceneryChildren.size(), childrenLate, childrenGone, childrenFramed,
            recording.objectLooks.size(), recording.sceneryObjectLooks.size());
        for (std::size_t rule = 0; rule < objcheck::kSceneryRuleCount; ++rule)
            if (byRule[rule])
                std::printf("%s:%u|", objcheck::SceneryRuleText(static_cast<objcheck::SceneryRule>(rule)), byRule[rule]);
        std::printf("\n");
        return 0;
    }

    int Ropes(const std::wstring& clipPath, std::uint32_t stepMs, bool allVertices)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        std::map<std::uint32_t, std::unique_ptr<pose::PoseTrack>> tracks;
        for (const pose::PoseRecordExport& record : poses)
        {
            auto track = std::make_unique<pose::PoseTrack>();
            std::string importError;
            if (track->Import(record.track, importError))
                tracks[record.recordId] = std::move(track);
        }
        struct Nearest
        {
            std::string id = "none";
            int bone = -1;
            float d = 1e9f;
        };
        std::vector<entity::CreatePacket> alive;
        pose::PoseSample sample;
        std::vector<float> xyz;
        for (const ropes::RopeRecord& record : recording.ropes.Records())
        {
            std::printf("ROPE engine_id=%u type=%u vertices=%u samples=%zu near_ped=%s attached=", record.engineId,
                record.type, record.vertices, record.times.size(),
                record.nearPed.Valid() ? Id(record.nearPed).c_str() : "none");
            for (const ropes::RopeAttach& a : record.attachments)
                std::printf("%s@%d ", a.id.Valid() ? Id(a.id).c_str() : "none", a.bone);
            std::printf("\n");
            for (const ropes::RopeSlotEvent& e : record.slotEvents)
            {
                std::printf("  SLOT t=%u slot=%u holder=%s bone=%d\n", e.timeMs, e.slot,
                    e.holder == ropes::RopeHolder::Recorded ? Id(e.id).c_str()
                        : e.holder == ropes::RopeHolder::Empty ? "none" : "other", e.bone);
            }
            for (const auto& h : record.heldEvents)
                std::printf("  HELD t=%u on=%d\n", h.first, h.second ? 1 : 0);
            if (record.times.empty())
                continue;
            std::uint32_t nextMs = 0;
            for (std::size_t n = 0; n < record.times.size(); ++n)
            {
                const std::uint32_t t = record.times[n];
                if (t < nextMs)
                    continue;
                nextMs = t + stepMs;
                const float* v = &record.positions[n * record.vertices * 3];
                // First, middle and last vertex of the drawn window.
                const float* ends[3] = {v, v + (record.vertices / 2) * 3, v + (record.vertices - 1) * 3};
                Nearest nearest[3];
                // --vertices: every vertex of the window (pins show as a
                // vertex sitting on a bone, under a centimetre).
                std::vector<Nearest> perVertex(allVertices ? record.vertices : 0);
                recording.lifecycle.AliveAt(t, alive);
                for (const entity::CreatePacket& create : alive)
                {
                    if (create.kind != entity::EntityKind::Ped && create.kind != entity::EntityKind::Horse)
                        continue;
                    const entity::PoseSegment* segment = recording.segments.At(create.id, t);
                    const std::uint32_t recordId = segment ? segment->recordId : create.id.Packed();
                    auto found = tracks.find(recordId);
                    pose::WorldTransform world{};
                    if (found == tracks.end() || !found->second->Sample(t, sample) || !SampleWorld(sample, world))
                        continue;
                    for (std::size_t b = 0; b < sample.objects.size(); ++b)
                    {
                        float w[3];
                        ToWorld(world, sample.objects[b].translation, w);
                        for (int end = 0; end < 3; ++end)
                        {
                            const float d = Distance(w, ends[end]);
                            if (d < nearest[end].d)
                                nearest[end] = {Id(create.id), static_cast<int>(b), d};
                        }
                        for (std::size_t vi = 0; vi < perVertex.size(); ++vi)
                        {
                            const float d = Distance(w, v + vi * 3);
                            if (d < perVertex[vi].d)
                                perVertex[vi] = {Id(create.id), static_cast<int>(b), d};
                        }
                    }
                }
                const std::uint32_t window = n < record.windowCounts.size() ? record.windowCounts[n] : 0;
                std::printf("  T t=%u window=%u held=%d", t, window, ropes::HeldAt(record, t) ? 1 : 0);
                const char* names[3] = {"first", "mid", "last"};
                for (int end = 0; end < 3; ++end)
                {
                    std::printf(" %s=%.2f;%.2f;%.2f near=%s#%d@%.3f", names[end], ends[end][0], ends[end][1], ends[end][2],
                        nearest[end].id.c_str(), nearest[end].bone, nearest[end].d);
                }
                std::printf("\n");
                for (std::size_t vi = 0; vi < perVertex.size(); ++vi)
                {
                    std::printf("    V%zu %s#%d@%.3f\n", vi, perVertex[vi].id.c_str(), perVertex[vi].bone, perVertex[vi].d);
                }
            }
        }
        return 0;
    }

    // The clip's speech lines per ped, with the audible start, the request
    // and the gap between them (a gap above 5 s is flagged: likely a ghost
    // line; a scripted conversation may also wait).
    int Speech(const std::wstring& clipPath, const std::string& filter)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        std::map<std::string, std::vector<const entity::SpeechEvent*>> byPed;
        for (const entity::SpeechEvent& line : recording.speechLines.Events())
            if (filter.empty() || Id(line.speaker) == filter)
                byPed[Id(line.speaker)].push_back(&line);
        std::size_t total = 0, suspect = 0, byStart = 0;
        for (const auto& [ped, lines] : byPed)
        {
            std::size_t pedSuspect = 0;
            for (const entity::SpeechEvent* line : lines)
                pedSuspect += speech::rec::SuspectStartGap(line->coreMs, line->timeMs, line->timedByStart) ? 1 : 0;
            std::printf("PED,id=%s,lines=%zu,suspect=%zu\n", ped.c_str(), lines.size(), pedSuspect);
            for (const entity::SpeechEvent* line : lines)
            {
                const bool flag = speech::rec::SuspectStartGap(line->coreMs, line->timeMs, line->timedByStart);
                std::printf("  LINE,start_ms=%u,core_ms=%u,gap_ms=%d,timed_by=%s,voice=0x%08x,context=0x%08x,variation=%d,"
                            "requested=0x%08x,path=%s,stop_ms=%u,stop_reason=%u,end_ms=%u,started_before_ms=%u%s\n",
                    line->timeMs, line->coreMs,
                    static_cast<int>(line->timeMs) - static_cast<int>(line->coreMs),
                    line->timedByStart ? "start" : "core", line->voiceHash, speech::Finalize(line->contextPartialHash),
                    line->variation, speech::Finalize(line->requestedContextPartialHash), speech::PathName(line->path),
                    line->stopMs, line->stopReason, line->endMs, line->startedBeforeMs, flag ? ",suspect=long_wait" : "");
            }
            total += lines.size();
            suspect += pedSuspect;
            for (const entity::SpeechEvent* line : lines)
                byStart += line->timedByStart ? 1 : 0;
        }
        std::printf("SPEECH_TOTAL,lines=%zu,peds=%zu,timed_by_start=%zu,suspect_long_wait=%zu,suspect_gap_ms=%u\n", total,
            byPed.size(), byStart, suspect, speech::rec::kSuspectStartGapMs);
        return 0;
    }

    // speech-log: replays the recording phase of a session log (Debug=1)
    // through the recorder's matcher, with the legacy rule (Legacy) and the
    // current one (Identity), and prints the lines each records. Inputs, in
    // log order (the order the recorder applied them): POSE_V2_SPEECH_EVENT
    // (a line of a recorded ped created), POSE_V2_SPEECH_ID,phase=record with
    // a start timing (an audible start at an address), POSE_V2_SPEECH_UNHEARD
    // with a stop reason and POSE_V2_SPEECH_STOP,phase=record (stops). The
    // identity of a start is the one of the last line created at its address
    // (the line living there); a line is a ghost when the identity recorded
    // differs from it. Creations of peds the clip does not record are not in
    // the log, so a reuse by such a ped is invisible here (the mod's start
    // check covers it).
    struct LogLine
    {
        std::string ped;
        speech::rec::LineKey key;
        std::uint32_t coreMs = 0;
        std::uint32_t startMs = 0;
        bool ghost = false;
    };

    std::string Field(const std::string& line, const char* key)
    {
        const std::string pattern = std::string(",") + key + "=";
        const std::size_t at = line.find(pattern);
        if (at == std::string::npos)
            return std::string();
        const std::size_t from = at + pattern.size();
        const std::size_t to = line.find(',', from);
        return line.substr(from, to == std::string::npos ? std::string::npos : to - from);
    }

    std::uint64_t FieldU(const std::string& line, const char* key)
    {
        const std::string value = Field(line, key);
        return value.empty() ? 0 : std::strtoull(value.c_str(), nullptr, 0);
    }

    std::string LineText(const LogLine& line)
    {
        char text[160];
        std::snprintf(text, sizeof(text), "ped=%s,context=0x%08x,variation=%d,core_ms=%u,start_ms=%u,gap_ms=%d", line.ped.c_str(),
            line.key.context, line.key.variation, line.coreMs, line.startMs,
            static_cast<int>(line.startMs) - static_cast<int>(line.coreMs));
        return text;
    }

    struct LogReplay
    {
        speech::rec::LineMatcher<LogLine> matcher;
        std::vector<LogLine> recorded;
        std::uint64_t unheard = 0;
        explicit LogReplay(speech::rec::Policy policy) : matcher(policy) {}
    };

    int SpeechLog(const std::wstring& logPath, bool listAll)
    {
        std::ifstream in(logPath, std::ios::binary);
        if (!in)
        {
            std::fprintf(stderr, "cannot open the log\n");
            return 2;
        }
        int segment = 0;
        int failures = 0;
        LogReplay legacy(speech::rec::Policy::Legacy);
        LogReplay current(speech::rec::Policy::Identity);
        std::vector<LogLine> logged; // what the session actually recorded (POSE_V2_SPEECH_ID,phase=record)
        std::map<std::uint64_t, speech::rec::LineKey> live; // last line created at each address
        std::string text;
        const auto finish = [&](const std::string& totalLine) {
            ++segment;
            for (LogReplay* replay : {&legacy, &current})
                replay->unheard += replay->matcher.TakePendings().size();
            const auto ghosts = [](const std::vector<LogLine>& lines) {
                std::size_t n = 0;
                for (const LogLine& line : lines)
                    n += line.ghost ? 1 : 0;
                return n;
            };
            const auto key = [](const LogLine& line) {
                return line.ped + "|" + std::to_string(line.key.context) + "|" + std::to_string(line.key.variation) + "|" +
                    std::to_string(line.startMs);
            };
            std::map<std::string, const LogLine*> before, after;
            for (const LogLine& line : logged)
                before[key(line)] = &line;
            for (const LogLine& line : current.recorded)
                after[key(line)] = &line;
            std::size_t legacySame = 0;
            {
                std::map<std::string, int> a, b;
                for (const LogLine& line : logged)
                    ++a[key(line)];
                for (const LogLine& line : legacy.recorded)
                    ++b[key(line)];
                legacySame = a == b ? 1 : 0;
            }
            std::printf("SEGMENT,n=%d,%s\n", segment, totalLine.c_str());
            std::printf("BEFORE,recorded=%zu,ghosts=%zu (from the log)\n", logged.size(), ghosts(logged));
            std::printf("LEGACY_SIM,recorded=%zu,ghosts=%zu,matches_log=%zu\n", legacy.recorded.size(), ghosts(legacy.recorded),
                legacySame);
            const speech::rec::Counters& c = current.matcher.Count();
            std::size_t removed = 0, added = 0;
            for (const auto& [k, line] : before)
                if (!after.count(k))
                {
                    ++removed;
                    std::printf("  REMOVED,%s%s\n", LineText(*line).c_str(), line->ghost ? ",ghost=1" : "");
                }
            for (const auto& [k, line] : after)
                if (!before.count(k))
                {
                    ++added;
                    std::printf("  RECOVERED,%s\n", LineText(*line).c_str());
                }
            std::printf("AFTER,recorded=%zu,ghosts=%zu,removed=%zu,recovered=%zu,reused_dropped=%llu,reused_started=%llu,"
                        "start_mismatch=%llu,unheard=%llu\n",
                current.recorded.size(), ghosts(current.recorded), removed, added,
                static_cast<unsigned long long>(c.reusedPending), static_cast<unsigned long long>(c.reusedStarted),
                static_cast<unsigned long long>(c.startMismatch), static_cast<unsigned long long>(current.unheard));
            if (listAll)
                for (const LogLine& line : current.recorded)
                    std::printf("  LINE,%s\n", LineText(line).c_str());
            if (!legacySame)
                ++failures;
            legacy = LogReplay(speech::rec::Policy::Legacy);
            current = LogReplay(speech::rec::Policy::Identity);
            logged.clear();
            live.clear();
        };
        while (std::getline(in, text))
        {
            if (!text.empty() && text.back() == '\r')
                text.pop_back();
            if (text.rfind("POSE_V2_SPEECH_EVENT,", 0) == 0)
            {
                if (Field(text, "timing") != "wait_start")
                    continue;
                const std::uint64_t address = FieldU(text, "line");
                LogLine line;
                line.ped = Field(text, "ped");
                line.key.voice = static_cast<std::uint32_t>(FieldU(text, "voice"));
                line.key.context = static_cast<std::uint32_t>(FieldU(text, "context"));
                line.key.variation = static_cast<std::int32_t>(FieldU(text, "variation"));
                line.coreMs = static_cast<std::uint32_t>(FieldU(text, "core_ms"));
                live[address] = line.key;
                for (LogReplay* replay : {&legacy, &current})
                {
                    std::vector<speech::rec::LineMatcher<LogLine>::Drop> drops;
                    std::vector<speech::rec::LineMatcher<LogLine>::StartedDrop> startedDrops;
                    replay->matcher.Created(address, drops, startedDrops);
                    replay->unheard += drops.size();
                    replay->matcher.AddPending(address, line.key, line);
                }
            }
            else if (text.rfind("POSE_V2_SPEECH_ID,phase=record,", 0) == 0)
            {
                const std::string timing = Field(text, "timing");
                if (timing.rfind("start", 0) != 0)
                    continue;
                const std::uint64_t address = FieldU(text, "line");
                const std::uint32_t startMs = static_cast<std::uint32_t>(FieldU(text, "t_start_ms"));
                LogLine was;
                was.ped = Field(text, "ped");
                was.key.voice = static_cast<std::uint32_t>(FieldU(text, "voice"));
                was.key.context = static_cast<std::uint32_t>(FieldU(text, "context"));
                was.key.variation = static_cast<std::int32_t>(FieldU(text, "variation"));
                was.coreMs = static_cast<std::uint32_t>(FieldU(text, "t_core_ms"));
                was.startMs = startMs;
                const auto alive = live.find(address);
                const bool known = alive != live.end();
                const speech::rec::LineKey startKey = known ? alive->second : speech::rec::LineKey{};
                was.ghost = known && !speech::rec::SameLine(startKey, was.key);
                logged.push_back(was);
                for (LogReplay* replay : {&legacy, &current})
                {
                    speech::rec::LineMatcher<LogLine>::Entry entry;
                    std::vector<speech::rec::LineMatcher<LogLine>::Drop> drops;
                    if (replay->matcher.Start(address, known, startKey, entry, drops) == speech::rec::StartResult::Matched)
                    {
                        LogLine line = entry.value;
                        line.startMs = startMs;
                        line.ghost = known && !speech::rec::SameLine(startKey, line.key);
                        replay->recorded.push_back(line);
                        replay->matcher.Started(address, replay->recorded.size() - 1);
                    }
                    replay->unheard += drops.size();
                }
            }
            else if (text.rfind("POSE_V2_SPEECH_UNHEARD,", 0) == 0 || text.rfind("POSE_V2_SPEECH_STOP,phase=record,", 0) == 0)
            {
                if (text.find(",at=") != std::string::npos)
                    continue; // end of recording or a rule of the new recorder, not a stop
                const std::string reason = Field(text, text.rfind("POSE_V2_SPEECH_UNHEARD,", 0) == 0 ? "stop_reason" : "reason");
                if (reason.empty() || reason == "none" || reason == "reused" || reason == "start_mismatch")
                    continue;
                const std::uint64_t address = FieldU(text, "line");
                for (LogReplay* replay : {&legacy, &current})
                {
                    speech::rec::LineMatcher<LogLine>::Entry entry;
                    std::size_t index = 0;
                    if (replay->matcher.StopPending(address, entry))
                        ++replay->unheard;
                    else
                        replay->matcher.StopStarted(address, index);
                }
            }
            else if (text.rfind("POSE_V2_SPEECH_RECORD_TOTAL,", 0) == 0)
                finish(text);
        }
        if (segment == 0)
        {
            std::fprintf(stderr, "no POSE_V2_SPEECH_RECORD_TOTAL in the log\n");
            return 3;
        }
        if (failures)
            std::fprintf(stderr, "the Legacy replay does not reproduce the log in %d segment(s)\n", failures);
        return failures ? 4 : 0;
    }
}

// Clip names and paths are ASCII in practice; anything else becomes '?'.
std::string Narrow(const std::wstring& text)
{
    std::string out;
    for (const wchar_t c : text)
        out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return out;
}

namespace
{
    // shake-migration: an old clip's marker shake and shake events folded
    // into the markers as the editor does on load, and the proof that
    // nothing plays differently: SHAKE_MIGRATION with the largest shake and
    // camera path difference over every 30 Hz frame. Only the camera and
    // shake sections are read (an old clip whose other sections this build
    // refuses still shows its shake); the game camera comes along when the
    // whole clip loads. Read only: the clip is never written.
    int ShakeMigration(const std::vector<std::wstring>& clips, bool verbose)
    {
        int failures = 0;
        for (const std::wstring& path : clips)
        {
            std::vector<std::uint8_t> plain;
            std::string error;
            const std::string name = Narrow(path.substr(path.find_last_of(L"\\/") + 1));
            if (!ReadPlainClip(path, plain, error) || plain.size() < 20)
            {
                std::printf("SHAKE_MIGRATION_CLIP,clip=%s,ok=0,error=%s\n", name.c_str(), error.c_str());
                ++failures;
                continue;
            }
            std::uint64_t durationMs = 0;
            std::memcpy(&durationMs, plain.data() + 12, sizeof(durationMs));
            const std::uint8_t* camr = nullptr;
            const std::uint8_t* cfxe = nullptr;
            std::size_t camrBytes = 0, cfxeBytes = 0;
            std::uint16_t camrVersion = 0, cfxeVersion = 0;
            for (std::size_t offset = 20; offset + 14 <= plain.size();)
            {
                std::uint32_t tag = 0;
                std::uint16_t version = 0;
                std::uint64_t length = 0;
                std::memcpy(&tag, plain.data() + offset, 4);
                std::memcpy(&version, plain.data() + offset + 4, 2);
                std::memcpy(&length, plain.data() + offset + 6, 8);
                offset += 14;
                if (length > plain.size() - offset)
                    break;
                if (tag == 0x524D4143u) // CAMR
                {
                    camr = plain.data() + offset;
                    camrBytes = static_cast<std::size_t>(length);
                    camrVersion = version;
                }
                else if (tag == 0x45584643u) // CFXE
                {
                    cfxe = plain.data() + offset;
                    cfxeBytes = static_cast<std::size_t>(length);
                    cfxeVersion = version;
                }
                offset += static_cast<std::size_t>(length) + 8;
            }
            camera::Track track;
            if (!camr || !clipfile::DecodeCameraTrack(camr, camrBytes, camrVersion, track, error) ||
                (cfxe && cfxeBytes && !clipfile::DecodeShakeEvents(cfxe, cfxeBytes, cfxeVersion, track, error)))
            {
                std::printf("SHAKE_MIGRATION_CLIP,clip=%s,ok=0,camr=v%u,error=%s\n", name.c_str(), camrVersion, error.c_str());
                ++failures;
                continue;
            }
            // The game camera and the cuts when the whole clip loads here.
            Recording recording;
            std::vector<pose::PoseRecordExport> poses;
            camera::Track whole;
            std::uint64_t wholeMs = 0;
            std::string wholeError;
            const bool loaded = clipfile::Decode(plain.data(), plain.size(), 0x140000000ull, 0x10000000ull, recording, poses, whole,
                wholeMs, wholeError);
            camera::Scene scene;
            if (loaded)
            {
                scene.game = &recording.gameCamera;
                scene.cuts = &recording.cuts;
            }
            const std::size_t markersBefore = track.Keyframes().size();
            const bool hadLegacy = track.HasLegacyShake();
            camera::mshake::Migrate(track);
            camera::mshake::Proof proof;
            if (track.Migration().before)
                proof = camera::mshake::Verify(*track.Migration().before, track, scene, durationMs, 0);
            std::printf("SHAKE_MIGRATION_CLIP,clip=%s,ok=1,camr=v%u,cfxe=v%u,duration_ms=%llu,markers=%zu,legacy=%d,scene=%s\n",
                name.c_str(), camrVersion, cfxeVersion, static_cast<unsigned long long>(durationMs), markersBefore, hadLegacy ? 1 : 0,
                loaded ? "game_camera" : "markers_only");
            if (hadLegacy)
            {
                std::printf("%s,clip=%s\n", camera::mshake::FormatLine(track.Migration(), proof).c_str(), name.c_str());
                if (!proof.Exact())
                    ++failures;
            }
            if (verbose)
                for (const camera::Keyframe& k : track.Keyframes())
                    std::printf("  t=%llu type=%d origin=%u %s\n", static_cast<unsigned long long>(k.timeMs),
                        static_cast<int>(k.markerType), static_cast<unsigned>(k.origin), camera::mshake::FormatFields(k.shake).c_str());
        }
        return failures ? 4 : 0;
    }
}

namespace
{
    // frozen-poses: walking peds whose recorded pose repeats from one capture
    // frame to the next, split by the recorded game camera's view
    // (frozen_pose_check.h). Every pose track frame of every ped record is
    // decoded at its own time (no resampling); a pose segment's record counts
    // only while it is the one its entity plays. A ped is carried while it
    // sits in a vehicle seat (the lifecycle's attachments) or rides a mount
    // (jerk::MountPairs, the playback's own rule, as export finds it), both
    // every kCarriedStepMs. One FROZEN_POSES_PED line per ped with a counted
    // pair (closest to the camera first; --max-peds keeps the N closest, 0
    // all), then FROZEN_POSES with the totals and the verdict. Exit 4 when
    // the off camera share is over --max-pct.
    int FrozenPoses(const std::wstring& clipPath, float maxPct, std::size_t maxPeds, float maxDistM)
    {
        constexpr std::uint32_t kCarriedStepMs = 100;
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track cameraTrack;
        std::uint64_t durationMs = 0;
        std::string error;
        if (!clipfile::Load(clipPath, 0x140000000ull, 0x10000000ull, recording, poses, cameraTrack, durationMs, error))
        {
            std::fprintf(stderr, "load failed: %s\n", error.c_str());
            return 2;
        }
        std::map<std::uint32_t, const entity::CreatePacket*> created;
        for (const entity::CreatePacket& create : recording.created)
            created[create.id.Packed()] = &create;
        std::map<std::uint32_t, std::unique_ptr<pose::PoseTrack>> tracks;
        for (const pose::PoseRecordExport& record : poses)
        {
            auto track = std::make_unique<pose::PoseTrack>();
            std::string importError;
            if (track->Import(record.track, importError))
                tracks[record.recordId] = std::move(track);
            else
                std::fprintf(stderr, "record %08X: %s\n", record.recordId, importError.c_str());
        }
        pose::PoseSample sample;

        // Carried peds, one flag per kCarriedStepMs.
        std::map<std::uint32_t, std::vector<std::uint8_t>> carried;
        {
            const std::uint32_t duration = static_cast<std::uint32_t>(durationMs);
            const std::size_t steps = duration / kCarriedStepMs + 1;
            const auto mark = [&](std::uint32_t packed, std::uint32_t t) {
                std::vector<std::uint8_t>& flags = carried[packed];
                flags.resize(steps);
                flags[t / kCarriedStepMs] = 1;
            };
            std::vector<entity::CreatePacket> alive;
            std::vector<entity::Attachment> attachments;
            std::vector<jerk::Body> bodies;
            jerk::MountPairs pairs;
            for (std::uint32_t t = 0; t <= duration; t += kCarriedStepMs)
            {
                recording.lifecycle.AttachmentsAt(t, attachments);
                for (const entity::Attachment& a : attachments)
                    if (a.kind == entity::AttachmentKind::VehicleSeat)
                        mark(a.child.Packed(), t);
                recording.lifecycle.AliveAt(t, alive);
                bodies.clear();
                for (const entity::CreatePacket& c : alive)
                {
                    if (c.kind != entity::EntityKind::Ped && c.kind != entity::EntityKind::Horse)
                        continue;
                    const entity::PoseSegment* segment = recording.segments.At(c.id, t);
                    auto track = tracks.find(segment ? segment->recordId : c.id.Packed());
                    pose::WorldTransform world{};
                    if (track == tracks.end() || !track->second->Sample(t, sample) || !SampleWorld(sample, world))
                        continue;
                    jerk::Body body;
                    body.packed = c.id.Packed();
                    body.horse = c.kind == entity::EntityKind::Horse;
                    body.world = world;
                    bodies.push_back(body);
                }
                pairs.Update(bodies);
                for (const auto& [rider, mount] : pairs.Map())
                    mark(rider, t);
            }
        }
        const auto carriedAt = [&](std::uint32_t packed, std::uint32_t t) {
            auto it = carried.find(packed);
            const std::size_t step = t / kCarriedStepMs;
            return it != carried.end() && step < it->second.size() && it->second[step] != 0;
        };

        const auto viewAt = [&](std::uint32_t t) {
            frozenpose::View view;
            editor::CameraState state{};
            if (!camera::EvaluateGame(recording.gameCamera, t, state, &recording.cuts))
                return view;
            view.pos[0] = state.position.x;
            view.pos[1] = state.position.y;
            view.pos[2] = state.position.z;
            view.yawDeg = state.rotation.z;
            view.fovDeg = state.fov;
            view.valid = true;
            return view;
        };

        struct Ped
        {
            entity::ReplayId id;
            std::uint32_t model = 0;
            frozenpose::Counts counts;
        };
        std::map<std::uint32_t, Ped> peds;
        std::uint64_t frames = 0;
        std::vector<pose::BoneQts> previous;
        std::vector<frozenpose::Frame> run;
        for (const pose::PoseRecordExport& record : poses)
        {
            const bool segmentRecord = (record.recordId & entity::PoseSegmentTrack::kRecordBit) != 0;
            const entity::ReplayId owner =
                segmentRecord ? recording.segments.OwnerOf(record.recordId) : entity::ReplayId::FromPacked(record.recordId);
            auto found = created.find(owner.Packed());
            auto track = tracks.find(record.recordId);
            if (found == created.end() || found->second->kind != entity::EntityKind::Ped || track == tracks.end())
                continue;
            Ped& ped = peds[owner.Packed()];
            ped.id = owner;
            ped.model = found->second->modelHash;
            run.clear();
            previous.clear();
            const auto flush = [&] {
                frozenpose::CountPairs(run, viewAt, maxDistM, ped.counts);
                run.clear();
                previous.clear();
            };
            for (const pose::TrackChunkExport& chunk : record.track.chunks)
            {
                for (const std::uint32_t t : chunk.frameTimes)
                {
                    const entity::PoseSegment* segment = recording.segments.At(owner, t);
                    const std::uint32_t active = segment ? segment->recordId : owner.Packed();
                    pose::WorldTransform world{};
                    if (active != record.recordId || !track->second->Sample(t, sample) || sample.frameTimeA != t ||
                        !SampleWorld(sample, world))
                    {
                        flush(); // not this record's frame to play: the run ends here
                        continue;
                    }
                    ++frames;
                    frozenpose::Frame frame;
                    frame.timeMs = t;
                    frame.pos[0] = world.position.x;
                    frame.pos[1] = world.position.y;
                    frame.pos[2] = world.position.z;
                    frame.samePose = previous.size() == sample.objects.size() &&
                        frozenpose::SamePose(previous.data(), sample.objects.data(), previous.size());
                    frame.carried = carriedAt(owner.Packed(), t);
                    previous = sample.objects;
                    run.push_back(frame);
                }
            }
            flush();
        }

        std::vector<const Ped*> counted;
        for (const auto& [packed, ped] : peds)
            if (ped.counts.Pairs())
                counted.push_back(&ped);
        std::sort(counted.begin(), counted.end(), [](const Ped* a, const Ped* b) {
            return a->counts.MeanDistance() < b->counts.MeanDistance();
        });
        if (maxPeds && counted.size() > maxPeds)
            counted.resize(maxPeds);
        frozenpose::Counts total;
        for (const Ped* ped : counted)
        {
            const frozenpose::Counts& c = ped->counts;
            total.Add(c);
            std::printf("FROZEN_POSES_PED,id=%s,model=%s,dist_m=%.1f,on=%u/%u,off=%u/%u,off_pct=%.1f\n", Id(ped->id).c_str(),
                Hex(ped->model).c_str(), c.MeanDistance(), c.frozen[frozenpose::kOn], c.pairs[frozenpose::kOn],
                c.frozen[frozenpose::kOff], c.pairs[frozenpose::kOff], frozenpose::OffPercent(c));
        }
        const std::wstring nameWide = clipPath.substr(clipPath.find_last_of(L"\\/") + 1);
        const bool fails = frozenpose::Fails(total, maxPct);
        std::printf("FROZEN_POSES,clip=%s,peds=%zu,frames=%llu,on=%u/%u,off=%u/%u,on_pct=%.1f,off_pct=%.1f,max_pct=%.1f,"
                    "max_dist_m=%.0f,verdict=%s\n",
            Narrow(nameWide).c_str(), counted.size(), static_cast<unsigned long long>(frames), total.frozen[frozenpose::kOn],
            total.pairs[frozenpose::kOn], total.frozen[frozenpose::kOff], total.pairs[frozenpose::kOff],
            frozenpose::OnPercent(total), frozenpose::OffPercent(total), maxPct, maxDistM, fails ? "fail" : "ok");
        return fails ? 4 : 0;
    }
}

namespace
{
    using namespace replay;

    double MsSince(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    // This process's commit and working set peaks, in MB.
    void MemoryPeak(double& commitMb, double& workingSetMb)
    {
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        commitMb = workingSetMb = 0.0;
        if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        {
            commitMb = static_cast<double>(counters.PeakPagefileUsage) / (1024.0 * 1024.0);
            workingSetMb = static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
        }
    }

    // pack-verify <clip> [--threads N] [--level L] [--zstd] [--block MB] [--plain] [--out <file>]
    // The clip (plain, zstd or packed) packed into the packed container,
    // unpacked again (all threads, then one) and compared byte for byte with
    // the clip. Never writes the clip; --out writes the packed file (with
    // the summary frame) for pack-load. Exit 0 identical, 3 different, 1 error.
    int PackVerify(int argc, wchar_t** argv)
    {
        const std::wstring path = argv[2];
        clippack::PackOptions options;
        options.threads = 8;
        std::wstring outPath;
        for (int n = 3; n < argc; ++n)
        {
            const std::wstring arg = argv[n];
            if (arg == L"--threads" && n + 1 < argc)
                options.threads = static_cast<unsigned>(std::wcstoul(argv[++n], nullptr, 10));
            else if (arg == L"--level" && n + 1 < argc)
                options.lzmaLevel = static_cast<int>(std::wcstol(argv[++n], nullptr, 10));
            else if (arg == L"--zstd")
                options.codec = clippack::Codec::Zstd;
            else if (arg == L"--block" && n + 1 < argc)
                options.blockBytes = static_cast<std::size_t>(std::wcstoul(argv[++n], nullptr, 10)) << 20;
            else if (arg == L"--plain")
                options.transform = false;
            else if (arg == L"--out" && n + 1 < argc)
                outPath = argv[++n];
        }
        std::vector<std::uint8_t> file;
        std::string error;
        const auto readStart = std::chrono::steady_clock::now();
        if (!clipcompress::ReadWholeFile(path, file, nullptr, error))
        {
            std::printf("PACK_VERIFY,ok=0,error=read %s\n", error.c_str());
            return 1;
        }
        const char* source = clipcompress::IsPacked(file.data(), file.size()) ? "packed"
            : clipcompress::IsZstd(file.data(), file.size())                   ? "zstd"
            : clipcompress::IsPlainClip(file.data(), file.size())               ? "plain"
                                                                                : "other";
        const std::uint64_t fileBytes = file.size();
        std::vector<std::uint8_t> clip;
        if (clipcompress::IsCompressed(file.data(), file.size()))
        {
            if (!clipcompress::Decompress(file.data(), file.size(), clip, error))
            {
                std::printf("PACK_VERIFY,ok=0,error=decompress %s\n", error.c_str());
                return 1;
            }
        }
        else
            clip.swap(file);
        file.clear();
        file.shrink_to_fit();
        const double readMs = MsSince(readStart);
        if (!clipcompress::IsPlainClip(clip.data(), clip.size()))
        {
            std::printf("PACK_VERIFY,ok=0,error=not a clip\n");
            return 1;
        }

        std::vector<std::uint8_t> packed;
        clippack::PackStats stats;
        const auto packStart = std::chrono::steady_clock::now();
        if (!clippack::Pack(clip.data(), clip.size(), options, packed, error, &stats))
        {
            std::printf("PACK_VERIFY,ok=0,error=pack %s\n", error.c_str());
            return 1;
        }
        const double packMs = MsSince(packStart);
        for (const clippack::StreamStat& stream : stats.streams)
        {
            if (stream.rawBytes == 0)
                continue;
            std::printf("PACK_STREAM,name=%s,raw=%llu,packed=%llu,blocks=%u,ratio=%.2f\n", clippack::StreamName(stream.stream),
                static_cast<unsigned long long>(stream.rawBytes), static_cast<unsigned long long>(stream.packedBytes), stream.blocks,
                stream.packedBytes ? static_cast<double>(stream.rawBytes) / static_cast<double>(stream.packedBytes) : 0.0);
        }

        bool identical = true;
        double unpackMs[2]{};
        clippack::UnpackStats unpackStats[2];
        const unsigned threadRuns[2] = {0, 1}; // the default (the load's), then one thread (the compressor's verify)
        for (int run = 0; run < 2; ++run)
        {
            clippack::UnpackOptions unpack;
            unpack.threads = threadRuns[run];
            std::vector<std::uint8_t> back;
            const auto unpackStart = std::chrono::steady_clock::now();
            const bool ok = clippack::Unpack(packed.data(), packed.size(), unpack, back, error, &unpackStats[run]);
            unpackMs[run] = MsSince(unpackStart);
            if (!ok)
            {
                std::printf("PACK_VERIFY,ok=0,error=unpack %s\n", error.c_str());
                return 1;
            }
            const bool same = back.size() == clip.size() && std::memcmp(back.data(), clip.data(), clip.size()) == 0;
            identical = identical && same;
            if (!same)
            {
                std::size_t at = 0;
                while (at < back.size() && at < clip.size() && back[at] == clip[at])
                    ++at;
                std::printf("PACK_DIFF,threads=%u,first_difference=%zu,size=%zu,expected=%zu\n", unpackStats[run].threads, at,
                    back.size(), clip.size());
            }
        }
        std::vector<std::uint8_t> summary;
        const bool haveSummary = clipcompress::Summary(clip.data(), clip.size(), summary);
        std::vector<std::uint8_t> withSummary = packed;
        if (haveSummary)
            clipcompress::AppendSummaryFrame(summary, withSummary);
        if (!outPath.empty())
        {
            std::ofstream out(outPath, std::ios::binary);
            out.write(reinterpret_cast<const char*>(withSummary.data()), static_cast<std::streamsize>(withSummary.size()));
            if (!out)
            {
                std::printf("PACK_VERIFY,ok=0,error=cannot write %s\n", Narrow(outPath).c_str());
                return 1;
            }
        }
        double commitMb = 0, workingSetMb = 0;
        MemoryPeak(commitMb, workingSetMb);
        const std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
        std::printf("PACK_VERIFY,clip=%s,source=%s,file_bytes=%llu,clip_bytes=%zu,packed_bytes=%zu,ratio=%.3f,vs_file=%.3f,"
                    "identical=%s,pose=%d,rope=%d,pose_skip=%s,rope_skip=%s,read_ms=%.0f,pack_ms=%.0f,transform_ms=%.0f,"
                    "pack_threads=%u,unpack_ms=%.0f,unpack_threads=%u,blocks_ms=%.0f,rebuild_ms=%.0f,unpack1_ms=%.0f,"
                    "blocks=%u,peak_commit_mb=%.0f,peak_ws_mb=%.0f\n",
            Narrow(name).c_str(), source, static_cast<unsigned long long>(fileBytes), clip.size(), withSummary.size(),
            static_cast<double>(clip.size()) / static_cast<double>(withSummary.size()),
            static_cast<double>(fileBytes) / static_cast<double>(withSummary.size()), identical ? "yes" : "no", stats.pose ? 1 : 0,
            stats.rope ? 1 : 0, stats.poseSkip.empty() ? "-" : stats.poseSkip.c_str(), stats.ropeSkip.empty() ? "-" : stats.ropeSkip.c_str(),
            readMs, packMs, stats.transformMs, options.threads, unpackMs[0], unpackStats[0].threads, unpackStats[0].blocksMs,
            unpackStats[0].rebuildMs, unpackMs[1], unpackStats[0].blocks, commitMb, workingSetMb);
        return identical ? 0 : 3;
    }

    // save-pack-verify <clip> [--levels 1,3,6] [--threads 1,4] [--lzma] [--out <file>]
    // The save's pack (clipcompress::PackForSave, what clipfile::Save runs
    // on the save's thread) of the clip (plain, zstd or packed: its clip),
    // for each zstd level and thread count: its time (pack and check), the
    // size, then unpacked again and compared byte for byte. --lzma adds the
    // background compressor's LZMA 9 size for comparison; the plain write
    // and each packed write are timed in the folder of --out (or the temp
    // folder). --out keeps the last packed file. Never writes the clip.
    // Exit 0 all identical, 3 a difference, 1 error.
    int SavePackVerify(int argc, wchar_t** argv)
    {
        const std::wstring path = argv[2];
        std::vector<int> levels = {1, 3, 6};
        std::vector<unsigned> threadCounts = {1, 4};
        bool lzma = false;
        std::wstring outPath;
        const auto parseList = [](const std::wstring& text) {
            std::vector<unsigned> values;
            std::size_t at = 0;
            while (at < text.size())
            {
                const std::size_t comma = text.find(L',', at);
                values.push_back(static_cast<unsigned>(std::wcstoul(text.substr(at, comma - at).c_str(), nullptr, 10)));
                if (comma == std::wstring::npos)
                    break;
                at = comma + 1;
            }
            return values;
        };
        for (int n = 3; n < argc; ++n)
        {
            const std::wstring arg = argv[n];
            if (arg == L"--levels" && n + 1 < argc)
            {
                levels.clear();
                for (unsigned value : parseList(argv[++n]))
                    levels.push_back(static_cast<int>(value));
            }
            else if (arg == L"--threads" && n + 1 < argc)
                threadCounts = parseList(argv[++n]);
            else if (arg == L"--lzma")
                lzma = true;
            else if (arg == L"--out" && n + 1 < argc)
                outPath = argv[++n];
        }
        std::vector<std::uint8_t> file;
        std::string error;
        if (!clipcompress::ReadWholeFile(path, file, nullptr, error))
        {
            std::printf("SAVE_PACK,ok=0,error=read %s\n", error.c_str());
            return 1;
        }
        std::vector<std::uint8_t> clip;
        if (clipcompress::IsCompressed(file.data(), file.size()))
        {
            if (!clipcompress::Decompress(file.data(), file.size(), clip, error))
            {
                std::printf("SAVE_PACK,ok=0,error=decompress %s\n", error.c_str());
                return 1;
            }
        }
        else
            clip.swap(file);
        std::vector<std::uint8_t>().swap(file);
        if (!clipcompress::IsPlainClip(clip.data(), clip.size()))
        {
            std::printf("SAVE_PACK,ok=0,error=not a clip\n");
            return 1;
        }
        const std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
        std::wstring folder;
        if (!outPath.empty())
            folder = outPath.substr(0, outPath.find_last_of(L"\\/") + 1);
        else
        {
            wchar_t temp[MAX_PATH]{};
            GetTempPathW(MAX_PATH, temp);
            folder = temp;
        }
        const std::wstring scratch = folder + L"save-pack-verify.tmp.rpclip";
        const auto timedWrite = [&](const std::vector<std::uint8_t>& bytes) {
            const auto start = std::chrono::steady_clock::now();
            {
                std::ofstream out(scratch, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                out.flush();
            }
            const double ms = MsSince(start);
            DeleteFileW(scratch.c_str());
            return ms;
        };
        const double plainWriteMs = timedWrite(clip);

        std::uint64_t lzmaBytes = 0;
        double lzmaMs = 0.0;
        if (lzma)
        {
            clippack::PackOptions options; // the background compressor's: LZMA 9
            options.threads = 8;
            std::vector<std::uint8_t> packed;
            const auto start = std::chrono::steady_clock::now();
            if (!clippack::Pack(clip.data(), clip.size(), options, packed, error))
            {
                std::printf("SAVE_PACK,ok=0,error=lzma %s\n", error.c_str());
                return 1;
            }
            lzmaMs = MsSince(start);
            std::vector<std::uint8_t> summary;
            if (clipcompress::Summary(clip.data(), clip.size(), summary))
                clipcompress::AppendSummaryFrame(summary, packed);
            lzmaBytes = packed.size();
            std::printf("SAVE_PACK_LZMA,clip=%s,clip_bytes=%zu,lzma_bytes=%llu,ratio=%.3f,ms_8_threads=%.0f\n", Narrow(name).c_str(),
                clip.size(), static_cast<unsigned long long>(lzmaBytes), static_cast<double>(clip.size()) / static_cast<double>(lzmaBytes), lzmaMs);
        }
        bool identical = true;
        std::vector<std::uint8_t> last;
        // The file of each level from the first thread count: the other
        // thread counts must write the same bytes.
        std::vector<std::pair<int, std::uint64_t>> firstHash;
        for (const unsigned threads : threadCounts)
        {
            for (const int level : levels)
            {
                std::vector<std::uint8_t> copy = clip;
                clipcompress::SavePackOptions options;
                options.zstdLevel = level;
                options.threads = threads;
                std::vector<std::uint8_t> packed;
                clipcompress::SavePackResult result;
                const auto start = std::chrono::steady_clock::now();
                const bool ok = clipcompress::PackForSave(copy, options, packed, result);
                const double totalMs = MsSince(start);
                if (!ok)
                {
                    std::printf("SAVE_PACK,clip=%s,level=%d,threads=%u,ok=0,fallback=%s\n", Narrow(name).c_str(), level, threads,
                        result.fallback.c_str());
                    identical = false;
                    continue;
                }
                std::vector<std::uint8_t> back;
                const bool same = clipcompress::Decompress(packed.data(), packed.size(), back, error) && back == clip;
                identical = identical && same;
                std::vector<std::uint8_t>().swap(back);
                const std::uint64_t fileHash = clippack::HashBytes(packed.data(), packed.size());
                bool sameFile = true;
                bool known = false;
                for (const auto& entry : firstHash)
                    if (entry.first == level)
                    {
                        known = true;
                        sameFile = entry.second == fileHash;
                    }
                if (!known)
                    firstHash.emplace_back(level, fileHash);
                identical = identical && sameFile;
                const double writeMs = timedWrite(packed);
                std::printf("SAVE_PACK,clip=%s,level=%d,threads=%u,ok=1,identical=%s,same_file_as_first_threads=%s,clip_bytes=%zu,file_bytes=%zu,vs_raw=%.3f,vs_lzma=%.3f,"
                            "pack_ms=%.0f,transform_ms=%.0f,blocks_ms=%.0f,verify_ms=%.0f,total_ms=%.0f,write_ms=%.0f,plain_write_ms=%.0f,extra_ms=%.0f,pose=%d,rope=%d\n",
                    Narrow(name).c_str(), level, threads, same ? "yes" : "no", sameFile ? "yes" : "no", clip.size(), packed.size(),
                    static_cast<double>(packed.size()) / static_cast<double>(clip.size()),
                    lzmaBytes ? static_cast<double>(packed.size()) / static_cast<double>(lzmaBytes) : 0.0, result.packMs, result.transformMs,
                    result.compressMs, result.verifyMs,
                    totalMs, writeMs, plainWriteMs, totalMs + writeMs - plainWriteMs, result.pose ? 1 : 0, result.rope ? 1 : 0);
                last.swap(packed);
            }
        }
        if (!outPath.empty() && !last.empty())
        {
            std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(last.data()), static_cast<std::streamsize>(last.size()));
            if (!out)
            {
                std::printf("SAVE_PACK,ok=0,error=cannot write %s\n", Narrow(outPath).c_str());
                return 1;
            }
        }
        double commitMb = 0, workingSetMb = 0;
        MemoryPeak(commitMb, workingSetMb);
        std::printf("SAVE_PACK_DONE,clip=%s,identical=%s,peak_commit_mb=%.0f,peak_ws_mb=%.0f\n", Narrow(name).c_str(),
            identical ? "yes" : "no", commitMb, workingSetMb);
        return identical ? 0 : 3;
    }

    // pack-load <file>: the editor's load (clipfile::Load: read, decompress,
    // decode) of a clip file, timed, with this process's memory peak.
    int PackLoad(const std::wstring& path)
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track track;
        std::uint64_t durationMs = 0;
        std::string error;
        clipfile::LoadInfo info;
        constexpr std::uintptr_t kBase = 0x140000000ull;
        constexpr std::size_t kSize = 0x10000000ull;
        const auto start = std::chrono::steady_clock::now();
        const bool ok = clipfile::Load(path, kBase, kSize, recording, poses, track, durationMs, error, &info);
        const double loadMs = MsSince(start);
        double commitMb = 0, workingSetMb = 0;
        MemoryPeak(commitMb, workingSetMb);
        std::printf("PACK_LOAD,ok=%d,error=%s,packed=%d,compressed=%d,file_bytes=%llu,clip_bytes=%llu,decompress_ms=%u,load_ms=%.0f,"
                    "poses=%zu,duration_ms=%llu,peak_commit_mb=%.0f,peak_ws_mb=%.0f\n",
            ok ? 1 : 0, ok ? "none" : error.c_str(), info.packed ? 1 : 0, info.compressed ? 1 : 0,
            static_cast<unsigned long long>(info.fileBytes), static_cast<unsigned long long>(info.clipBytes), info.decompressMs, loadMs,
            poses.size(), static_cast<unsigned long long>(durationMs), commitMb, workingSetMb);
        return ok ? 0 : 1;
    }
}

namespace
{
    // compress-file <file> [--zstd] [--no-upgrade] [--threads N]: the
    // background compressor's CompressFile on that file, IN PLACE (run it
    // on a copy). Exit 0 compressed, 2 skipped, 1 failed.
    int CompressFileCommand(int argc, wchar_t** argv)
    {
        clipcompress::CompressOptions options;
        for (int n = 3; n < argc; ++n)
        {
            const std::wstring arg = argv[n];
            if (arg == L"--zstd")
                options.format = clipcompress::Format::Zstd;
            else if (arg == L"--no-upgrade")
                options.upgrade = false;
            else if (arg == L"--threads" && n + 1 < argc)
                options.threads = static_cast<unsigned>(std::wcstoul(argv[++n], nullptr, 10));
        }
        const clipcompress::FileResult result = clipcompress::CompressFile(argv[2], options, nullptr);
        double commitMb = 0, workingSetMb = 0;
        MemoryPeak(commitMb, workingSetMb);
        std::printf("COMPRESS_FILE,ok=%d,skipped=%d,reason=%s,bytes_in=%llu,bytes_out=%llu,ms=%u,upgraded=%d,fast_packed=%d,plain_pack=%d,"
                    "peak_commit_mb=%.0f,peak_ws_mb=%.0f\n",
            result.ok ? 1 : 0, result.skipped ? 1 : 0, result.reason.empty() ? "-" : result.reason.c_str(),
            static_cast<unsigned long long>(result.bytesIn), static_cast<unsigned long long>(result.bytesOut), result.ms,
            result.upgraded ? 1 : 0, result.fastPacked ? 1 : 0, result.plainPack ? 1 : 0, commitMb, workingSetMb);
        return result.ok ? 0 : result.skipped ? 2 : 1;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc >= 3 && std::wstring(argv[1]) == L"compress-file")
        return CompressFileCommand(argc, argv);
    if (argc >= 3 && std::wstring(argv[1]) == L"pack-verify")
        return PackVerify(argc, argv);
    if (argc >= 3 && std::wstring(argv[1]) == L"save-pack-verify")
        return SavePackVerify(argc, argv);
    if (argc == 3 && std::wstring(argv[1]) == L"pack-load")
        return PackLoad(argv[2]);
    if ((argc == 4 || argc == 6) && std::wstring(argv[1]) == L"set-camera")
        return SetCamera(argv[2], argv[3], argc == 6 && std::wstring(argv[4]) == L"--out" ? std::wstring(argv[5]) : std::wstring());
    if (argc == 3 && std::wstring(argv[1]) == L"project-info")
        return ProjectInfo(argv[2]);
    if (argc >= 5 && std::wstring(argv[1]) == L"project-camera")
    {
        std::string name = "Director";
        bool keepTrim = false;
        for (int n = 5; n < argc; ++n)
        {
            if (std::wstring(argv[n]) == L"--name" && n + 1 < argc)
                name = Narrow(argv[++n]);
            else if (std::wstring(argv[n]) == L"--keep-trim")
                keepTrim = true;
        }
        return ProjectCamera(argv[2], argv[3], argv[4], name, keepTrim);
    }
    if (argc == 6 && std::wstring(argv[1]) == L"trim")
    {
        const bool last = std::wstring(argv[4]) == L"last";
        const unsigned long a = std::wcstoul(argv[last ? 5 : 4], nullptr, 10);
        const unsigned long b = last ? 0ul : std::wcstoul(argv[5], nullptr, 10);
        return Trim(argv[2], argv[3], last, static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b));
    }
    if (argc >= 6 && std::wstring(argv[1]) == L"bones")
    {
        std::uint32_t stepMs = 33;
        float minM = 1.0f;
        for (int n = 6; n + 1 < argc; ++n)
        {
            if (std::wstring(argv[n]) == L"--step-ms")
                stepMs = static_cast<std::uint32_t>(std::max(1, _wtoi(argv[n + 1])));
            else if (std::wstring(argv[n]) == L"--min-m")
                minM = static_cast<float>(_wtof(argv[n + 1]));
        }
        return Bones(argv[2], Narrow(argv[3]), static_cast<std::uint32_t>(std::wcstoul(argv[4], nullptr, 10)),
            static_cast<std::uint32_t>(std::wcstoul(argv[5], nullptr, 10)), stepMs, minM);
    }
    if (argc >= 3 && std::wstring(argv[1]) == L"shader-vars")
    {
        std::string filter;
        bool series = false;
        for (int n = 3; n < argc; ++n)
        {
            if (std::wstring(argv[n]) == L"--id" && n + 1 < argc)
                filter = Narrow(argv[++n]);
            else if (std::wstring(argv[n]) == L"--series")
                series = true;
        }
        return ShaderVars(argv[2], filter, series);
    }
    if (argc >= 3 && std::wstring(argv[1]) == L"muscle-rows")
    {
        std::string filter;
        std::uint32_t stepMs = 5;
        for (int n = 3; n + 1 < argc; ++n)
        {
            if (std::wstring(argv[n]) == L"--id")
                filter = Narrow(argv[n + 1]);
            else if (std::wstring(argv[n]) == L"--step-ms")
                stepMs = static_cast<std::uint32_t>(std::max(1, _wtoi(argv[n + 1])));
        }
        return MuscleRows(argv[2], filter, stepMs);
    }
    if (argc >= 3 && std::wstring(argv[1]) == L"hats")
    {
        std::uint32_t stepMs = 100;
        bool fix = false;
        for (int n = 3; n < argc; ++n)
        {
            if (std::wstring(argv[n]) == L"--step-ms" && n + 1 < argc)
                stepMs = static_cast<std::uint32_t>(std::max(1, _wtoi(argv[n + 1])));
            else if (std::wstring(argv[n]) == L"--fix")
                fix = true;
        }
        return fix ? HatsFix(argv[2], stepMs) : Hats(argv[2], stepMs);
    }
    if (argc >= 3 && std::wstring(argv[1]) == L"pairs")
    {
        std::uint32_t stepMs = 100;
        for (int n = 3; n + 1 < argc; ++n)
            if (std::wstring(argv[n]) == L"--step-ms")
                stepMs = static_cast<std::uint32_t>(std::max(1, _wtoi(argv[n + 1])));
        return Pairs(argv[2], stepMs);
    }
    if (argc >= 7 && std::wstring(argv[1]) == L"mount")
    {
        std::uint32_t stepMs = 16;
        bool rawOnly = false;
        for (int n = 7; n < argc; ++n)
        {
            if (std::wstring(argv[n]) == L"--step-ms" && n + 1 < argc)
                stepMs = static_cast<std::uint32_t>(std::max(1, _wtoi(argv[n + 1])));
            else if (std::wstring(argv[n]) == L"--raw")
                rawOnly = true;
        }
        return Mount(argv[2], Narrow(argv[3]), Narrow(argv[4]), static_cast<std::uint32_t>(std::wcstoul(argv[5], nullptr, 10)),
            static_cast<std::uint32_t>(std::wcstoul(argv[6], nullptr, 10)), stepMs, rawOnly);
    }
    if (argc >= 3 && std::wstring(argv[1]) == L"sounds")
        return Sounds(argv[2]);
    if (argc >= 3 && std::wstring(argv[1]) == L"appearance")
        return AppearanceOf(argv[2], argc >= 4 ? Narrow(argv[3]) : std::string());
    if (argc >= 3 && std::wstring(argv[1]) == L"segments")
        return SegmentsOf(argv[2], argc >= 4 ? Narrow(argv[3]) : std::string());
    if (argc >= 3 && std::wstring(argv[1]) == L"never-animated")
    {
        std::vector<std::wstring> clips;
        std::string ref;
        for (int n = 2; n < argc; ++n)
        {
            if (std::wstring(argv[n]) == L"--ref" && n + 1 < argc)
                ref = Narrow(argv[++n]);
            else
                clips.push_back(argv[n]);
        }
        int result = 0;
        for (const std::wstring& clip : clips)
        {
            std::printf("CLIP,%s\n", Narrow(clip.substr(clip.find_last_of(L"\\/") + 1)).c_str());
            result = (std::max)(result, NeverAnimatedOf(clip, ref));
        }
        return result;
    }
    if (argc >= 3 && std::wstring(argv[1]) == L"shake-migration")
    {
        std::vector<std::wstring> clips;
        bool verbose = false;
        for (int n = 2; n < argc; ++n)
        {
            if (std::wstring(argv[n]) == L"--markers")
                verbose = true;
            else
                clips.push_back(argv[n]);
        }
        return ShakeMigration(clips, verbose);
    }
    if (argc >= 3 && std::wstring(argv[1]) == L"frozen-poses")
    {
        float maxPct = replay::frozenpose::kDefaultMaxPct;
        float maxDistM = replay::frozenpose::kDefaultMaxDistM;
        std::size_t maxPeds = 0;
        for (int n = 3; n + 1 < argc; ++n)
        {
            if (std::wstring(argv[n]) == L"--max-pct")
                maxPct = static_cast<float>(_wtof(argv[++n]));
            else if (std::wstring(argv[n]) == L"--max-peds")
                maxPeds = static_cast<std::size_t>(std::max(0, _wtoi(argv[++n])));
            else if (std::wstring(argv[n]) == L"--max-dist")
                maxDistM = static_cast<float>(_wtof(argv[++n]));
        }
        return FrozenPoses(argv[2], maxPct, maxPeds, maxDistM);
    }
    if (argc == 3 && std::wstring(argv[1]) == L"sections")
        return SectionsOf(argv[2]);
    if (argc >= 3 && std::wstring(argv[1]) == L"speech")
        return Speech(argv[2], argc >= 4 ? Narrow(argv[3]) : std::string());
    if (argc >= 3 && std::wstring(argv[1]) == L"speech-log")
        return SpeechLog(argv[2], argc >= 4 && std::wstring(argv[3]) == L"--lines");
    if (argc == 3 && std::wstring(argv[1]) == L"vehicles")
        return Vehicles(argv[2]);
    if (argc == 3 && std::wstring(argv[1]) == L"shots")
        return Shots(argv[2]);
    if (argc >= 3 && std::wstring(argv[1]) == L"events")
        return Events(argv[2], argc >= 4 ? Narrow(argv[3]) : std::string());
    if (argc == 3 && std::wstring(argv[1]) == L"queries")
        return Queries(argv[2]);
    if (argc >= 3 && std::wstring(argv[1]) == L"weapon-bones")
    {
        std::string filter;
        std::uint32_t stepMs = 33;
        bool perBone = false;
        int seriesBone = -1;
        for (int n = 3; n < argc; ++n)
        {
            if (std::wstring(argv[n]) == L"--series" && n + 1 < argc)
            {
                seriesBone = _wtoi(argv[++n]);
                continue;
            }
            if (std::wstring(argv[n]) == L"--step-ms" && n + 1 < argc)
                stepMs = static_cast<std::uint32_t>(std::max(1, _wtoi(argv[++n])));
            else if (std::wstring(argv[n]) == L"--bones")
                perBone = true;
            else
                filter = Narrow(argv[n]);
        }
        return WeaponBones(argv[2], filter, stepMs, perBone, seriesBone);
    }
    if (argc == 3 && std::wstring(argv[1]) == L"mix")
        return Mix(argv[2]);
    if (argc == 3 && std::wstring(argv[1]) == L"render-targets")
        return RenderTargets(argv[2]);
    if (argc >= 3 && std::wstring(argv[1]) == L"generic")
    {
        std::string filter;
        std::uint32_t fromMs = 0, toMs = 0xFFFFFFFFu;
        int next = 3;
        if (argc > next && std::wcschr(argv[next], L':'))
            filter = Narrow(argv[next++]);
        if (argc > next + 1)
        {
            fromMs = static_cast<std::uint32_t>(std::wcstoul(argv[next], nullptr, 10));
            toMs = static_cast<std::uint32_t>(std::wcstoul(argv[next + 1], nullptr, 10));
        }
        return Generic(argv[2], filter, fromMs, toMs);
    }
    if (argc >= 3 && std::wstring(argv[1]) == L"signature")
    {
        std::vector<std::wstring> clips;
        std::string filter;
        for (int n = 2; n < argc; ++n)
        {
            if (std::wstring(argv[n]) == L"--id" && n + 1 < argc)
                filter = Narrow(argv[++n]);
            else
                clips.push_back(argv[n]);
        }
        return Signature(clips, filter);
    }
    if (argc >= 3 && std::wstring(argv[1]) == L"weapon-look")
        return WeaponLook(argv[2], argc >= 4 ? Narrow(argv[3]) : std::string());
    if (argc >= 3 && std::wstring(argv[1]) == L"objects")
        return Objects(argv[2], argc >= 4 ? Narrow(argv[3]) : std::string());
    if (argc >= 3 && std::wstring(argv[1]) == L"ropes")
    {
        std::uint32_t stepMs = 250;
        bool allVertices = false;
        for (int n = 3; n < argc; ++n)
        {
            if (std::wstring(argv[n]) == L"--step-ms" && n + 1 < argc)
                stepMs = static_cast<std::uint32_t>(std::max(1, _wtoi(argv[n + 1])));
            else if (std::wstring(argv[n]) == L"--vertices")
                allVertices = true;
        }
        return Ropes(argv[2], stepMs, allVertices);
    }
    if (argc < 4 || std::wstring(argv[1]) != L"export")
    {
        std::fprintf(stderr, "usage: ReplayClipTool export <clip.rpclip> <out.json> [--step-ms 100]\n"
                             "       ReplayClipTool set-camera <clip.rpclip> <markers.txt> [--out <copy.rpclip>]\n"
                             "       ReplayClipTool project-camera <project.rpproj> <clip.rpclip> <markers.txt> [--name <name>] [--keep-trim]\n"
                             "       ReplayClipTool project-info <project.rpproj>\n"
                             "       ReplayClipTool pack-verify <clip.rpclip> [--threads 8] [--level 9] [--zstd] [--block 32] [--plain] [--out <packed.rpclip>]\n"
                             "       ReplayClipTool pack-load <clip.rpclip>\n"
                             "       ReplayClipTool save-pack-verify <clip.rpclip> [--levels 1,3,6] [--threads 1,4] [--lzma] [--out <packed.rpclip>]\n"
                             "       ReplayClipTool compress-file <copy.rpclip> [--zstd] [--no-upgrade] [--threads 1]   (in place)\n"
                             "       ReplayClipTool trim <in.rpclip> <out.rpclip> <fromMs> <toMs>\n"
                             "       ReplayClipTool trim <in.rpclip> <out.rpclip> last <seconds>\n"
                             "       ReplayClipTool bones <clip.rpclip> <slot:instance> <fromMs> <toMs> [--step-ms 33] [--min-m 1]\n"
                             "       ReplayClipTool pairs <clip.rpclip> [--step-ms 100]\n"
                             "       ReplayClipTool mount <clip.rpclip> <rider> <mount> <fromMs> <toMs> [--step-ms 16] [--raw]\n"
                             "       ReplayClipTool ropes <clip.rpclip> [--step-ms 250] [--vertices]\n"
                             "       ReplayClipTool objects <clip.rpclip> [id | scenery]\n"
                             "       ReplayClipTool weapon-look <clip.rpclip> [id]\n"
                             "       ReplayClipTool events <clip.rpclip> [id]\n"
                             "       ReplayClipTool shots <clip.rpclip>\n"
                             "       ReplayClipTool vehicles <clip.rpclip>\n"
                             "       ReplayClipTool speech <clip.rpclip> [id]\n"
                             "       ReplayClipTool speech-log <RedemptionEditor.log> [--lines]\n"
                             "       ReplayClipTool generic <clip.rpclip> [id] [fromMs toMs]\n"
                             "       ReplayClipTool queries <clip.rpclip>\n"
                             "       ReplayClipTool mix <clip.rpclip>\n"
                             "       ReplayClipTool render-targets <clip.rpclip>\n"
                             "       ReplayClipTool sounds <clip.rpclip>\n"
                             "       ReplayClipTool sections <clip.rpclip>\n"
                             "       ReplayClipTool shake-migration <clip.rpclip>... [--markers]\n"
                             "       ReplayClipTool segments <clip.rpclip> [id]\n"
                             "       ReplayClipTool appearance <clip.rpclip> [id]\n"
                             "       ReplayClipTool signature <clip.rpclip>... [--id slot:instance]\n"
                             "       ReplayClipTool hats <clip.rpclip> [--step-ms 100] [--fix]\n"
                             "       ReplayClipTool never-animated <clip.rpclip>... [--ref slot:instance]\n"
                             "       ReplayClipTool frozen-poses <clip.rpclip> [--max-pct 2] [--max-peds 0] [--max-dist 40]\n");
        return 1;
    }
    // No --step-ms: 100 ms up to 2 minutes, 200 up to 5, 250 beyond (a long
    // clip with a thousand entities makes hundreds of MB of JSON at 100 ms).

    std::uint32_t stepMs = 0;
    for (int n = 4; n + 1 < argc; ++n)
        if (std::wstring(argv[n]) == L"--step-ms")
            stepMs = static_cast<std::uint32_t>(std::max(20, _wtoi(argv[n + 1])));
    const std::wstring clip = argv[2];
    const std::wstring outWide = argv[3];
    const std::wstring nameWide = clip.substr(clip.find_last_of(L"\\/") + 1);
    return Export(clip, outWide, stepMs, Narrow(nameWide));
}
