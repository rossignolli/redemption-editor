// QA kit: "mark problem" and the rolling health buffer, the core side. See
// qa_kit.h.
//
// Engine access: natives only, on the script thread: the rendered
// camera, the puppets' coordinates and their screen projection at the
// press. Nothing is written to the engine.

#include "qa_kit.h"
#include "i18n.h" // the notices in the player's language
#include "mod_identity.h" // RedemptionEditor.ini (old ReplayProbe.ini read too)

#include "engine_hud.h"
#include "qa_bundle.h"
#include "entity_appearance.h" // recorded layers against the clone's
#include "entity_cache.h"
#include "log_format.h"
#include "qa_health.h"
#include "editor_keys.h" // the keys the notices name
#include "qa_marks.h"
#include "qa_shared.h"
#include "replay_events.h" // the entity sound window
#include "replay_puppets.h"
#include "entity_lifecycle.h" // the attachments at the press
#include "sound_generic.h" // the generic sound window
#include "version.generated.h"
#include "main.h"
#include "natives.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <mutex>

namespace replay::qa::kit
{
    namespace
    {
        std::ostream* g_log = nullptr;
        bool g_enabled = true;
        Shared* g_shared = nullptr;
        std::uint64_t g_sharedRetryAt = 0;

        // The health ring: written by the log tap (any thread), read by the
        // script thread.
        std::mutex g_ringLock;
        HealthRing g_ring;
        std::atomic<bool> g_tapOn{false};
        std::atomic<std::uint32_t> g_playheadNow{0};

        std::atomic<LONG> g_markRequested{0};
        std::atomic<bool> g_cancel{false};

        // The playback being watched.
        bool g_watching = false;
        std::string g_watchClip;
        std::uint64_t g_watchStartedAt = 0;
        std::uint64_t g_lastTickAt = 0;
        std::uint32_t g_marksThisPlayback = 0;
        std::atomic<std::uint64_t> g_markPresses{0};
        std::uint64_t g_statsSequenceAtStart = 0;

        std::vector<Mark> g_marks; // this session's marks not yet reported
        std::uint32_t g_nextMarkId = 1;
        LONG64 g_noteSeen = 0;
        LONG64 g_markRequestSeen = -1;
        LONG64 g_reportRequestSeen = -1;
        std::string g_exeVersion;
        bool g_buildNoticePending = false;
        std::uint64_t g_configuredAt = 0;
        std::uint64_t g_lastMarkAt = 0;
        LONG64 g_shotsSeen = 0;
        // The pass over the clip (one more at every jump
        // back, or forward by more than a step): the ring's entries and the
        // marks carry it (qa_health.h Anomaly::pass).
        std::atomic<std::uint32_t> g_pass{0};
        std::uint64_t g_passPlayhead = 0;
        bool g_passPlayheadKnown = false;
        constexpr std::uint64_t kPassJumpMs = 2000;

        std::ostream& Log()
        {
            static std::ostream null(nullptr);
            return g_log ? *g_log : null;
        }

        std::wstring Widen(const std::string& text)
        {
            if (text.empty())
                return {};
            const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring out(static_cast<std::size_t>(size), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
            return out;
        }

        std::string Narrow(const std::wstring& text)
        {
            if (text.empty())
                return {};
            const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
            return out;
        }

        Shared* SharedBlock(std::uint64_t now)
        {
            if (!g_shared && now >= g_sharedRetryAt)
            {
                g_sharedRetryAt = now + 2000;
                g_shared = OpenShared();
            }
            return g_shared;
        }

        // The marks waiting for a report, published for the add-on's key bar
        // and reminders: only this session's presses still on disk
        // (qa_marks.h CountPendingMarks). Folders of an earlier session go
        // into the next report too (its MoveMarks takes every
        // ReplayReports\marks\mark-*) but are only logged. Script thread
        // only; a few dozen entries at most.
        std::uint64_t g_pendingCountAt = 0;
        std::uint32_t g_pendingMarks = 0;
        std::uint32_t g_olderMarks = 0;
        bool g_loadReminderPending = true;

        std::vector<std::string> ListMarkFolders()
        {
            std::vector<std::string> out;
            WIN32_FIND_DATAW data{};
            HANDLE find = FindFirstFileW(replay::storage::Path(L"ReplayReports\\marks\\mark-*").c_str(), &data); // the base folder's (storage_paths.h)
            if (find == INVALID_HANDLE_VALUE)
                return out;
            do
            {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    out.push_back(Narrow(data.cFileName));
            } while (FindNextFileW(find, &data));
            FindClose(find);
            return out;
        }

        // `pressed`: a mark being made, not in g_marks yet.
        void PublishPendingMarks(std::uint64_t now, const std::string& pressed = {})
        {
            g_pendingCountAt = now;
            PendingMarks counts;
            if (g_enabled)
            {
                std::vector<std::string> session;
                for (const Mark& mark : g_marks)
                    session.push_back(mark.folder);
                if (!pressed.empty())
                    session.push_back(pressed);
                counts = CountPendingMarks(session, ListMarkFolders());
            }
            g_pendingMarks = counts.session;
            g_olderMarks = counts.older;
            if (g_shared)
                InterlockedExchange(&g_shared->pendingMarks, static_cast<LONG>(g_pendingMarks));
        }

        bool AddonAlive(std::uint64_t now)
        {
            if (!g_shared)
                return false;
            const std::uint64_t seen = static_cast<std::uint64_t>(ReadSequence(&g_shared->addonFrameAt));
            return seen && now >= seen && now - seen < 3000;
        }

        bool WriteText(const std::wstring& path, const std::string& text)
        {
            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;
            DWORD wrote = 0;
            const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr) && wrote == text.size();
            CloseHandle(file);
            return ok;
        }

        std::wstring MarkFolderPath(const Mark& mark)
        {
            return replay::storage::Path(L"ReplayReports\\marks\\") + Widen(mark.folder);
        }

        void SaveMark(const Mark& mark)
        {
            if (!WriteText(MarkFolderPath(mark) + L"\\mark.json", MarkJson(mark)))
                Log() << "REPORT_MARK,phase=json_failed,id=" << mark.id << ",error=" << GetLastError() << '\n';
        }

        const char* StateText(playback::PuppetState state)
        {
            switch (state)
            {
            case playback::PuppetState::Loading: return "loading";
            case playback::PuppetState::Dressing: return "dressing";
            case playback::PuppetState::Ready: return "ready";
            case playback::PuppetState::Failed: return "failed";
            default: return "?";
            }
        }

        constexpr std::uint32_t kPropsPerBody = 32;

        const char* AttachmentText(entity::AttachmentKind kind)
        {
            switch (kind)
            {
            case entity::AttachmentKind::DraftHarness: return "draft_harness";
            case entity::AttachmentKind::VehicleSeat: return "vehicle_seat";
            case entity::AttachmentKind::WeaponPoint: return "weapon_point";
            default: return "attached";
            }
        }

        const char* PropOriginText(entity::PropOrigin origin)
        {
            switch (origin)
            {
            case entity::PropOrigin::DroppedHat: return "hat";
            case entity::PropOrigin::DroppedWeapon: return "weapon";
            case entity::PropOrigin::GorePiece: return "gore_piece";
            case entity::PropOrigin::Projectile: return "projectile";
            case entity::PropOrigin::WorldObject: return "world_object";
            case entity::PropOrigin::WorldPiece: return "world_piece";
            case entity::PropOrigin::VehiclePiece: return "vehicle_piece";
            default: return "?";
            }
        }

        // Recorded against applied for one body at the press. The metaped
        // layers the puppet must wear at the playhead against the clone's (a
        // hat is a layer), the skeleton armed against the
        // segment's record, and the props that came off the body (a dropped
        // hat, a weapon, a gore piece): built or not, and how far the clone
        // is from the transform last placed on it. A body within
        // kCheckRangeM gets an anomaly for a mismatch (the press's own
        // anomalies, merged into the window at its close).
        constexpr float kCheckRangeM = 40.0f;
        constexpr float kPropPlaceToleranceM = 0.25f;
        void CheckBody(const TickState& state, const playback::Puppet& puppet, float distance, Mark& mark, BodyInView& body)
        {
            const bool inRange = distance <= kCheckRangeM; // not "near": a Windows macro
            body.armedBones = puppet.armedBones;
            body.record = puppet.recordId;
            body.armedRecord = puppet.armedRecordId;
            const bool pedLike = puppet.packet.kind == entity::EntityKind::Ped || puppet.packet.kind == entity::EntityKind::Horse;
            if (pedLike && puppet.clone && replay::entities::Exists(puppet.clone) && puppet.state == playback::PuppetState::Ready)
            {
                // Against what the puppet must wear at the playhead (segment,
                // change), not the create packet: a change folded to 0 holds all
                // of its layers.
                const entity::appearance::Match match =
                    entity::appearance::Compare(puppet.clone, playback::Wear(puppet));
                body.layersChecked = true;
                body.layersRecorded = match.recorded;
                body.layersPresent = match.present;
                body.pedLayers = match.pedLayers;
            }
            const bool layersMissing = body.layersChecked && body.layersPresent < body.layersRecorded;
            const bool bonesDiffer = body.armed && body.armedBones && body.boneCount && body.armedBones != body.boneCount;
            if (inRange && (layersMissing || bonesDiffer))
            {
                Anomaly anomaly;
                anomaly.wallMs = mark.pressWall;
                anomaly.playheadMs = mark.playheadMs;
                anomaly.pass = mark.pass;
                anomaly.kind = Kind::AppearanceMismatch;
                anomaly.entity = body.id;
                char records[48];
                std::snprintf(records, sizeof(records), "record=0x%x;armed_record=0x%x", body.record, body.armedRecord);
                anomaly.detail = "layers=" + std::to_string(body.layersPresent) + "/" + std::to_string(body.layersRecorded) +
                    ";bones=" + std::to_string(body.armedBones) + "/" + std::to_string(body.boneCount) + ";" + records;
                mark.pressChecks.push_back(anomaly);
            }
            if (!state.puppets)
                return;
            // Every puppet attached to the body, of any kind, not only the
            // props that came off it: a held object without a weapon and the
            // cargo on a horse are weapon-kind puppets with a WeaponPoint
            // attachment.
            static std::vector<entity::Attachment> attachments;
            attachments.clear();
            if (state.lifecycle)
                state.lifecycle->AttachmentsAt(static_cast<std::uint32_t>(state.playheadMs), attachments);
            const auto linkOf = [&](const playback::Puppet& other) -> const char* {
                if (other.packet.kind == entity::EntityKind::Prop && other.packet.owner == puppet.packet.id)
                    return "owner";
                for (const entity::Attachment& attachment : attachments)
                    if (attachment.child == other.packet.id && attachment.parent == puppet.packet.id)
                        return AttachmentText(attachment.kind);
                if (other.attached && other.attachment.parent == puppet.packet.id)
                    return AttachmentText(other.attachment.kind);
                return nullptr;
            };
            std::uint32_t listed = 0;
            for (const playback::Puppet& prop : state.puppets->list)
            {
                if (&prop == &puppet)
                    continue;
                const char* link = linkOf(prop);
                if (!link)
                    continue;
                if (++listed > kPropsPerBody)
                    break;
                PropInView view;
                view.id = entity::IdText(prop.packet.id);
                view.kind = entity::KindText(prop.packet.kind);
                view.link = link;
                view.weaponHash = prop.packet.kind == entity::EntityKind::Weapon ? prop.packet.weaponHash : 0;
                view.origin = prop.packet.kind == entity::EntityKind::Prop ? PropOriginText(prop.packet.propOrigin)
                    : prop.packet.kind == entity::EntityKind::Weapon       ? (view.weaponHash ? "held_weapon" : "held_object")
                                                                           : view.kind;
                view.model = prop.packet.modelHash;
                view.state = StateText(prop.state);
                view.knockRequested = prop.knockRequested;
                view.clone = prop.clone && replay::entities::Exists(prop.clone);
                // The place error and its anomaly: props only (placed by
                // their world transform; an attached puppet follows its parent).
                if (prop.packet.kind != entity::EntityKind::Prop)
                {
                    body.props.push_back(std::move(view));
                    continue;
                }
                if (view.clone && prop.hasWorld)
                {
                    const Vector3 at = ENTITY::GET_ENTITY_COORDS(prop.clone, TRUE, FALSE);
                    const float dx = at.x - prop.lastWorld.position.x, dy = at.y - prop.lastWorld.position.y,
                                dz = at.z - prop.lastWorld.position.z;
                    view.placeErrorM = std::sqrt(dx * dx + dy * dy + dz * dz);
                }
                const bool notBuilt = prop.state == playback::PuppetState::Failed ||
                    (prop.state == playback::PuppetState::Ready && !view.clone);
                const bool misplaced = view.placeErrorM > kPropPlaceToleranceM;
                if (inRange && (notBuilt || misplaced))
                {
                    Anomaly anomaly;
                    anomaly.wallMs = mark.pressWall;
                    anomaly.playheadMs = mark.playheadMs;
                    anomaly.pass = mark.pass;
                    anomaly.kind = Kind::PropMisplaced;
                    anomaly.entity = view.id;
                    char error[32];
                    std::snprintf(error, sizeof(error), "%.2f", view.placeErrorM);
                    anomaly.detail = "owner=" + body.id + ";origin=" + view.origin + ";state=" + view.state + ";clone=" +
                        (view.clone ? "1" : "0") + ";place_error_m=" + (view.placeErrorM < 0.0f ? std::string("none") : error);
                    mark.pressChecks.push_back(anomaly);
                }
                body.props.push_back(std::move(view));
            }
        }

        // The bodies near the camera at the press: every ped, horse,
        // vehicle and train car puppet within 100 m (shown or not: a
        // missing one is often the problem), nearest first, at most 48.
        void CollectBodies(const TickState& state, const Vector3& camera, Mark& mark)
        {
            if (!state.puppets)
                return;
            static const Hash arthur = MISC::GET_HASH_KEY("player_zero");
            static const Hash john = MISC::GET_HASH_KEY("player_three");
            for (const playback::Puppet& puppet : state.puppets->list)
            {
                const entity::EntityKind kind = puppet.packet.kind;
                if (kind != entity::EntityKind::Ped && kind != entity::EntityKind::Horse && kind != entity::EntityKind::Vehicle &&
                    kind != entity::EntityKind::TrainCar)
                    continue;
                Vector3 at{};
                bool have = false;
                if (puppet.clone && replay::entities::Exists(puppet.clone))
                {
                    at = ENTITY::GET_ENTITY_COORDS(puppet.clone, TRUE, FALSE);
                    have = true;
                }
                else if (puppet.hasWorld)
                {
                    at.x = puppet.lastWorld.position.x;
                    at.y = puppet.lastWorld.position.y;
                    at.z = puppet.lastWorld.position.z;
                    have = true;
                }
                if (!have)
                    continue;
                const float dx = at.x - camera.x, dy = at.y - camera.y, dz = at.z - camera.z;
                const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (distance > 100.0f)
                    continue;
                BodyInView body;
                body.id = entity::IdText(puppet.packet.id);
                body.kind = entity::KindText(kind);
                body.model = puppet.packet.modelHash;
                if (kind == entity::EntityKind::Ped && puppet.packet.modelHash == arthur)
                    body.label = "Arthur";
                else if (kind == entity::EntityKind::Ped && puppet.packet.modelHash == john)
                    body.label = "John";
                body.distanceM = distance;
                float sx = 0.0f, sy = 0.0f;
                body.onScreen = GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD(at.x, at.y, at.z, &sx, &sy) != FALSE;
                body.state = StateText(puppet.state);
                body.visible = puppet.visibleState;
                body.armed = puppet.overrideSlot >= 0;
                body.mapped = puppet.mapped;
                body.armError = puppet.armError;
                body.boneCount = puppet.boneCount;
                CheckBody(state, puppet, distance, mark, body);
                mark.bodies.push_back(std::move(body));
            }
            std::sort(mark.bodies.begin(), mark.bodies.end(),
                [](const BodyInView& a, const BodyInView& b) { return a.distanceM < b.distanceM; });
            if (mark.bodies.size() > 48)
                mark.bodies.resize(48);
        }

        void CreateMark(std::uint64_t now, const TickState& state)
        {
            SYSTEMTIME local{};
            GetLocalTime(&local);
            Mark mark;
            mark.id = g_nextMarkId++;
            mark.folder = MarkFolderName(mark.id, local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);
            char wall[40];
            std::snprintf(wall, sizeof(wall), "%04u-%02u-%02u %02u:%02u:%02u.%03u", local.wYear, local.wMonth, local.wDay, local.wHour,
                local.wMinute, local.wSecond, local.wMilliseconds);
            mark.wallTime = wall;
            mark.pressWall = now;
            mark.playheadMs = static_cast<std::uint32_t>(state.playheadMs);
            mark.durationMs = static_cast<std::uint32_t>(state.durationMs);
            mark.rate = state.rate;
            mark.paused = state.paused;
            mark.scrubbing = state.scrubbing;
            mark.editorOpen = state.editorOpen;
            mark.clip = state.clip;
            mark.version = replay::build::kVersion;
            mark.pass = g_pass.load(std::memory_order_relaxed);

            const Vector3 position = CAM::GET_FINAL_RENDERED_CAM_COORD();
            const Vector3 rotation = CAM::GET_FINAL_RENDERED_CAM_ROT(2);
            CameraInfo& camera = mark.camera;
            camera.position[0] = position.x;
            camera.position[1] = position.y;
            camera.position[2] = position.z;
            camera.rotation[0] = rotation.x;
            camera.rotation[1] = rotation.y;
            camera.rotation[2] = rotation.z;
            camera.fov = CAM::GET_FINAL_RENDERED_CAM_FOV();
            constexpr float kDegrees = 3.14159265f / 180.0f;
            const float pitch = rotation.x * kDegrees, heading = rotation.z * kDegrees;
            camera.target[0] = position.x - std::sin(heading) * std::cos(pitch) * 10.0f;
            camera.target[1] = position.y + std::cos(heading) * std::cos(pitch) * 10.0f;
            camera.target[2] = position.z + std::sin(pitch) * 10.0f;
            camera.freeCam = state.freeCam;
            camera.editingMarker = state.editingMarker;
            CollectBodies(state, position, mark);

            replay::storage::Folder(L"ReplayReports\\marks");
            const std::wstring folder = MarkFolderPath(mark);
            const bool made = CreateDirectoryW(folder.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
            SaveMark(mark); // the press, at once: a crash in the next 2 s still leaves it
            PublishPendingMarks(now, mark.folder); // the count is in before the add-on sees the mark

            if (Shared* shared = SharedBlock(now))
            {
                wchar_t full[MAX_PATH * 2]{};
                GetFullPathNameW(folder.c_str(), MAX_PATH * 2, full, nullptr);
                const std::string path = Narrow(full);
                shared->markId = mark.id;
                shared->markPlayheadMs = mark.playheadMs;
                std::snprintf(shared->markFolder, sizeof(shared->markFolder), "%s", path.c_str());
                InterlockedIncrement64(&shared->markSequence);
            }
            ++g_marksThisPlayback;
            g_markPresses.fetch_add(1, std::memory_order_relaxed);
            // The rate and camera with their digits, whatever an earlier line
            // left on the stream (else rate=1 for 1.019, camera=7e+02).
            const replay::logfmt::FormatScope logFormat(Log());
            replay::logfmt::DefaultFloat(Log()) << "REPORT_MARK,phase=press,id=" << mark.id << ",folder=" << mark.folder << ",made=" << (made ? 1 : 0)
                  << ",clip=" << (mark.clip.empty() ? "buffer" : mark.clip) << ",playhead_ms=" << mark.playheadMs
                  << ",rate=" << std::setprecision(4) << std::fixed << mark.rate << ",paused=" << (mark.paused ? 1 : 0) << ",bodies=" << mark.bodies.size()
                  << std::setprecision(1) << ",camera=" << position.x << ";" << position.y << ";" << position.z << ",addon=" << (AddonAlive(now) ? 1 : 0) << '\n';
            // With the add-on alive its own notice names the report key
            // after the note box (this one is hidden during the replay).
            const std::string notice = AddonAlive(now)
                ? replay::keys::Format(TR("qa.marked_at_note", "Problem marked at {time}. Type a short note and press {key_select} ({key_select} alone "
                                                               "skips it). {key_report} saves the report."),
                      replay::keys::Current(), {{"time", TimeText(mark.playheadMs)}})
                : replay::keys::Format(TR("qa.marked_at", "Problem marked at {time}. {key_report} saves the report."), replay::keys::Current(),
                      {{"time", TimeText(mark.playheadMs)}});
            replay::hud::ShowNotice(notice.c_str());
            g_marks.push_back(std::move(mark));
        }

        // The sounds of the window, from the players' own books (no log line needed): the
        // generic packets by family and owner with the taker that owned
        // them, the entity sound rows by owner, and anomalies for what
        // failed (refused, missed, doubled, silent).
        void FillSounds(Mark& mark)
        {
            SoundWindow& w = mark.sounds;
            w.fromMs = mark.playheadMs > kBeforeMs ? mark.playheadMs - kBeforeMs : 0;
            w.toMs = mark.playheadMs + kAfterMs;
            replay::soundgeneric::player::WindowReport generic;
            if (replay::soundgeneric::WindowReport(w.fromMs, w.toMs, generic))
            {
                using replay::soundgeneric::player::Outcome;
                w.genericKnown = true;
                w.packets = generic.packets;
                const auto row = [](const std::uint32_t* counts, std::uint32_t doubles, SoundRow& r) {
                    r.played = counts[static_cast<int>(Outcome::Played)];
                    r.refused = counts[static_cast<int>(Outcome::Refused)];
                    r.yielded = counts[static_cast<int>(Outcome::Yielded)];
                    r.missed = counts[static_cast<int>(Outcome::Missed)];
                    r.jumped = counts[static_cast<int>(Outcome::Jumped)];
                    r.skipped = counts[static_cast<int>(Outcome::Specific)] + counts[static_cast<int>(Outcome::Puppet)] +
                        counts[static_cast<int>(Outcome::Disabled)] +
                        counts[static_cast<int>(Outcome::Unplaced)]; // 2D sounds of no entity
                    r.notReached = counts[static_cast<int>(Outcome::None)];
                    r.doubles = doubles;
                };
                for (const replay::soundgeneric::player::WindowFamily& f : generic.families)
                {
                    SoundRow r;
                    r.family = replay::soundowner::FamilyName(f.family);
                    r.taker = replay::soundowner::TakerName(f.taker);
                    r.packets = f.packets;
                    row(f.counts, f.doubles, r);
                    w.families.push_back(r);
                }
                for (const replay::soundgeneric::player::WindowOwner& o : generic.owners)
                {
                    SoundRow r;
                    r.owner = replay::census::IdText(o.owner);
                    r.family = replay::soundowner::FamilyName(o.family);
                    r.taker = replay::soundowner::TakerName(o.taker);
                    r.packets = o.packets;
                    row(o.counts, o.doubles, r);
                    w.owners.push_back(r);
                }
                for (const replay::soundgeneric::player::WindowFailure& f : generic.failures)
                {
                    SoundFailure failure;
                    failure.timeMs = f.timeMs;
                    failure.owner = replay::census::IdText(f.owner);
                    failure.family = replay::soundowner::FamilyName(f.family);
                    failure.taker = replay::soundowner::TakerName(f.taker);
                    failure.set = f.set;
                    failure.outcome = f.doubled ? "double" : replay::soundgeneric::player::OutcomeName(f.outcome);
                    w.failures.push_back(failure);
                    Anomaly anomaly;
                    anomaly.wallMs = mark.pressWall;
                    anomaly.pass = mark.pass;
                    anomaly.playheadMs = f.timeMs;
                    anomaly.kind = f.doubled ? Kind::GenericDouble : f.outcome == Outcome::Missed ? Kind::GenericMissed : Kind::GenericRefused;
                    anomaly.entity = failure.owner;
                    char set[16];
                    std::snprintf(set, sizeof(set), "0x%x", f.set);
                    anomaly.detail = "family=" + failure.family + ";taker=" + failure.taker + ";set=" + set;
                    mark.anomalies.push_back(anomaly);
                }
            }
            {
                const replay::events::EntitySoundWindow esnd = replay::events::EntitySoundWindowOf(w.fromMs, w.toMs);
                w.entityKnown = true;
                const auto row = [](const auto& src, EntitySoundRow& r) {
                    r.rows = src.rows;
                    r.played = src.played;
                    r.refused = src.refused;
                    r.tooFar = src.tooFar;
                    r.byPuppet = src.byPuppet;
                    r.passed = src.passed;
                    r.untried = src.untried;
                    r.toGeneric = src.toGeneric;
                };
                row(esnd, w.entityTotals);
                for (const replay::events::EntitySoundWindowOwner& o : esnd.owners)
                {
                    EntitySoundRow r;
                    r.owner = replay::census::IdText(o.owner);
                    r.lastStatus = o.lastStatus;
                    row(o, r);
                    w.entityOwners.push_back(r);
                    Anomaly anomaly;
                    anomaly.wallMs = mark.pressWall;
                    anomaly.pass = mark.pass;
                    anomaly.playheadMs = mark.playheadMs;
                    anomaly.entity = r.owner;
                    if (o.refused)
                    {
                        anomaly.kind = Kind::EntitySoundRefused;
                        anomaly.detail = "rows=" + std::to_string(o.rows) + ";refused=" + std::to_string(o.refused) + ";status=" + o.lastStatus;
                        mark.anomalies.push_back(anomaly);
                    }
                    // Rows the player reached (their time passed) and none
                    // played, none the puppet's, none refused, none too far:
                    // the track had them and nothing came out.
                    if (o.rows >= 3 && o.passed == o.rows)
                    {
                        anomaly.kind = Kind::EntitySoundSilent;
                        anomaly.detail = "rows=" + std::to_string(o.rows) + ";passed=" + std::to_string(o.passed);
                        mark.anomalies.push_back(anomaly);
                    }
                }
            }
            std::stable_sort(mark.anomalies.begin(), mark.anomalies.end(),
                [](const Anomaly& a, const Anomaly& b) { return a.playheadMs < b.playheadMs; });
        }

        void CloseWindows(std::uint64_t now, const TickState& state)
        {
            for (Mark& mark : g_marks)
            {
                if (mark.windowClosed)
                    continue;
                MarkWindow window;
                window.playheadMs = mark.playheadMs;
                window.pressWall = mark.pressWall;
                window.rate = mark.rate;
                const bool sameClip = state.clip == mark.clip;
                if (!WindowClosed(window, now, state.playheadMs, state.playing && sameClip))
                    continue;
                {
                    std::lock_guard<std::mutex> lock(g_ringLock);
                    // Every pass still in the ring (the press
                    // often comes paused after a scrub).
                    mark.anomalies = g_ring.Window(mark.playheadMs, kBeforeMs, kAfterMs);
                }
                mark.anomalies.insert(mark.anomalies.end(), mark.pressChecks.begin(), mark.pressChecks.end());
                if (sameClip && state.playing)
                    FillSounds(mark); // the players' books are those of this clip
                std::stable_sort(mark.anomalies.begin(), mark.anomalies.end(),
                    [](const Anomaly& a, const Anomaly& b) { return a.playheadMs < b.playheadMs; });
                mark.windowClosed = true;
                mark.windowEndPlayheadMs = static_cast<std::uint32_t>(state.playheadMs);
                SaveMark(mark);
                Counts counts;
                for (const Anomaly& anomaly : mark.anomalies)
                    ++counts.perKind[static_cast<int>(anomaly.kind)];
                Log() << "REPORT_MARK,phase=window,id=" << mark.id << ",anomalies=" << mark.anomalies.size() << ","
                      << CountsText(counts) << ",end_playhead_ms=" << mark.windowEndPlayheadMs
                      << ",waited_ms=" << (now - mark.pressWall) << '\n';
            }
        }

        Mark* FindMark(std::uint32_t id)
        {
            for (Mark& mark : g_marks)
                if (mark.id == id)
                    return &mark;
            return nullptr;
        }

        void ReadAddonReplies()
        {
            if (!g_shared)
                return;
            const LONG64 notes = ReadSequence(&g_shared->noteSequence);
            if (notes != g_noteSeen)
            {
                g_noteSeen = notes;
                char note[kNoteCapacity]{};
                std::memcpy(note, g_shared->note, sizeof(note) - 1);
                if (Mark* mark = FindMark(g_shared->noteMarkId))
                {
                    mark->note = note;
                    mark->noteSet = g_shared->noteAnswered != 0;
                    mark->noteDone = true;
                    SaveMark(*mark);
                    Log() << "REPORT_MARK,phase=note,id=" << mark->id << ",answered=" << (mark->noteSet ? 1 : 0)
                          << ",chars=" << mark->note.size() << '\n';
                }
            }
            const LONG64 shots = ReadSequence(&g_shared->shotsSequence);
            if (shots != g_shotsSeen)
            {
                g_shotsSeen = shots;
                if (Mark* mark = FindMark(g_shared->shotsMarkId))
                {
                    char error[sizeof(g_shared->shotsError)]{};
                    std::memcpy(error, g_shared->shotsError, sizeof(error) - 1);
                    mark->ringShots = g_shared->shotsRing;
                    mark->fullShot = g_shared->shotsFull != 0;
                    mark->shotsError = error;
                    mark->shotsDone = true;
                    SaveMark(*mark);
                    Log() << "REPORT_MARK,phase=shots,id=" << mark->id << ",ring=" << mark->ringShots
                          << ",full=" << (mark->fullShot ? 1 : 0) << ",error=" << (mark->shotsError.empty() ? "none" : mark->shotsError) << '\n';
                }
            }
        }

        void StartWatch(std::uint64_t now, const TickState& state)
        {
            {
                std::lock_guard<std::mutex> lock(g_ringLock);
                g_ring.Reset();
            }
            g_watching = true;
            g_watchClip = state.clip;
            g_watchStartedAt = now;
            g_pass.store(0, std::memory_order_relaxed);
            g_passPlayheadKnown = false;
            g_marksThisPlayback = 0;
            g_statsSequenceAtStart = g_shared ? static_cast<std::uint64_t>(ReadSequence(&g_shared->statsSequence)) : 0;
        }

        // QA_HEALTH_SUMMARY: one line per playback, with Debug=0 too (the
        // _SUMMARY suffix is essential, log_policy.h).
        void EndWatch(std::uint64_t now)
        {
            if (!g_watching)
                return;
            g_watching = false;
            Counts counts;
            std::size_t overflowed = 0;
            {
                std::lock_guard<std::mutex> lock(g_ringLock);
                counts = g_ring.Totals();
                overflowed = static_cast<std::size_t>(g_ring.Overflowed());
            }
            const replay::logfmt::FormatScope logFormat(Log()); // the averages with their digits
            replay::logfmt::DefaultFloat(Log());
            Log() << "QA_HEALTH_SUMMARY,clip=" << (g_watchClip.empty() ? "buffer" : g_watchClip)
                  << ",wall_ms=" << (now - g_watchStartedAt) << ",marks=" << g_marksThisPlayback << ",anomalies=" << counts.total << ','
                  << CountsText(counts) << ",ring_overflowed=" << overflowed;
            if (g_shared && static_cast<std::uint64_t>(ReadSequence(&g_shared->statsSequence)) != g_statsSequenceAtStart)
            {
                const Shared& s = *g_shared;
                Log() << ",shots_path=" << (s.ringPath == 1 ? "gpu_blit" : s.ringPath == 2 ? "full_copy" : "none")
                      << ",shots_size=" << s.ringWidth << 'x' << s.ringHeight << ",shots_captures=" << s.ringCaptures
                      << ",shots_issue_us_avg=" << s.ringIssueUsAvg << ",shots_issue_us_max=" << s.ringIssueUsMax
                      << ",shots_worker_us_avg=" << s.ringWorkerUsAvg << ",frame_us_avg=" << s.frameUsAvg
                      << ",frame_us_after_capture_avg=" << s.frameUsAfterAvg << ",frame_us_after_capture_max=" << s.frameUsAfterMax;
            }
            else
                Log() << ",shots_path=none";
            Log() << '\n';
            if (g_log)
                g_log->flush();
        }
    }

    void Configure(const std::string& iniPath, std::ostream& log, int scriptHookGameVersion)
    {
        g_log = &log;
        g_exeVersion = bundle::GameExeVersion();
        const int match = GameBuildMatches(g_exeVersion);
        g_buildNoticePending = match == 0;
        g_configuredAt = GetTickCount64();
        log << "ASI_GAME,exe_version=" << g_exeVersion << ",expected_build=" << kGameBuildMajor << '.' << kGameBuildMinor
            << ",match=" << (match == 1 ? "yes" : match == 0 ? "NO" : "unknown") << ",scripthook_game_version=" << scriptHookGameVersion
            << '\n';
        g_enabled = replay::identity::IniInt("QaMarks", 1, iniPath.c_str()) != 0;
        g_cancel.store(false);
        g_shared = OpenShared();
        if (g_shared)
            InterlockedExchange(&g_shared->marksEnabled, g_enabled ? 1 : 0);
        log << "QA_KIT_OBSERVER,marks=" << (g_enabled ? 1 : 0) << ",shared=" << (g_shared ? 1 : 0) << ",key=shift_f2,window_before_ms="
            << kBeforeMs << ",window_after_ms=" << kAfterMs << ",ring_capacity=" << kRingCapacity << '\n';
    }

    void OnLogLine(std::string_view line)
    {
        if (!g_tapOn.load(std::memory_order_relaxed) || line.empty() || line[0] != 'P')
            return;
        Anomaly anomaly;
        if (!ClassifyLine(line, g_playheadNow.load(std::memory_order_relaxed), anomaly))
            return;
        anomaly.wallMs = GetTickCount64();
        anomaly.pass = g_pass.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_ringLock);
        g_ring.Push(anomaly);
    }

    void RequestMark() { g_markRequested.store(1); }
    std::uint64_t MarkPresses() { return g_markPresses.load(std::memory_order_relaxed); }

    void Tick(std::uint64_t now, const TickState& state)
    {
        Shared* shared = SharedBlock(now);
        const bool watch = state.playing && !state.recording;
        // Another game build: said once, in game (the 1491.50 exe's file
        // version is 1.0.1491.50).
        if (g_buildNoticePending && now - g_configuredAt > 20000 && !state.exporting)
        {
            g_buildNoticePending = false;
            const std::string notice = TRF("qa.build", "Redemption Editor: this game is build {build}; the mod is made for 1.0.1491.50, "
                "so some things may not work.", {{"build", g_exeVersion}});
            replay::hud::ShowNotice(notice.c_str());
        }
        // The pending count every 2 s (the report thread moves the folders
        // away). Once after the load (30 s) the marks an earlier session left
        // are logged, with no notice: the player did not mark them now
        // (they go into the next report all the same).
        if (!g_pendingCountAt || now - g_pendingCountAt >= 2000)
            PublishPendingMarks(now);
        if (g_loadReminderPending && now - g_configuredAt > 30000)
        {
            g_loadReminderPending = false;
            if (g_enabled && g_olderMarks)
                Log() << "QA_PENDING_MARKS,phase=load,session=" << g_pendingMarks << ",older=" << g_olderMarks << ",notice=0\n";
        }
        if (g_watching && (!watch || state.clip != g_watchClip))
            EndWatch(now);
        if (watch && !g_watching)
            StartWatch(now, state);
        g_playheadNow.store(static_cast<std::uint32_t>(state.playheadMs), std::memory_order_relaxed);
        g_tapOn.store(watch, std::memory_order_relaxed);
        // A new pass at a jump back, or forward by more than
        // a step (a seek); plain play and frame steps stay in the pass.
        if (watch)
        {
            if (g_passPlayheadKnown &&
                (state.playheadMs < g_passPlayhead || state.playheadMs > g_passPlayhead + kPassJumpMs))
                g_pass.fetch_add(1, std::memory_order_relaxed);
            g_passPlayhead = state.playheadMs;
            g_passPlayheadKnown = true;
        }
        if (shared)
        {
            InterlockedExchange(&shared->ringAllowed, g_enabled && watch && !state.exporting ? 1 : 0);
            InterlockedExchange(&shared->marksEnabled, g_enabled ? 1 : 0);
        }

        // Hitches: a long script tick in plain play (not a pause, a scrub,
        // a settle or a load, where a long tick is by design).
        if (watch && g_lastTickAt && !state.paused && !state.scrubbing && !state.settling && now > g_lastTickAt &&
            now - g_lastTickAt > kHitchMs)
        {
            Anomaly anomaly;
            anomaly.wallMs = now;
            anomaly.playheadMs = static_cast<std::uint32_t>(state.playheadMs);
            anomaly.kind = Kind::Hitch;
            anomaly.pass = g_pass.load(std::memory_order_relaxed);
            anomaly.detail = "tick_ms=" + std::to_string(now - g_lastTickAt);
            std::lock_guard<std::mutex> lock(g_ringLock);
            g_ring.Push(anomaly);
        }
        g_lastTickAt = watch ? now : 0;
        // No prune by wall age; the ring keeps its last
        // kRingCapacity entries of the playback, so a mark pressed after a
        // scrub still finds the pass that played its window.

        // The add-on's Shift+F2 (the editor has the keyboard).
        if (shared)
        {
            const LONG64 requests = ReadSequence(&shared->markRequestSequence);
            if (g_markRequestSeen >= 0 && requests != g_markRequestSeen)
                g_markRequested.store(1);
            g_markRequestSeen = requests;
        }
        // One mark per key press, whoever saw it (a press seen by both sides
        // while the editor opens or closes).
        if (g_markRequested.load() && g_lastMarkAt && now - g_lastMarkAt < 400)
            g_markRequested.store(0);
        if (g_markRequested.exchange(0))
        {
            g_lastMarkAt = now;
            // Never during an export (the notice would be in the video).
            if (!g_enabled || state.exporting)
            {
            }
            else if (!watch)
                replay::hud::ShowNotice(replay::keys::Fill(TR("qa.mark_needs_replay", "{key_mark} marks a problem while a replay plays ({key_stop}, or a "
                                                                                      "clip in the editor)."),
                    replay::keys::Current()).c_str());
            else if (g_marks.size() >= 99)
                replay::hud::ShowNotice(
                    replay::keys::Fill(TR("qa.marks_full", "99 problem marks are waiting: save the report first ({key_report})."), replay::keys::Current())
                        .c_str());
            else
                CreateMark(now, state);
        }
        CloseWindows(now, state);
        ReadAddonReplies();
    }

    bool TakeReportRequest()
    {
        if (!g_shared)
            return false;
        const LONG64 requests = ReadSequence(&g_shared->reportRequestSequence);
        const bool fresh = g_reportRequestSeen >= 0 && requests != g_reportRequestSeen;
        g_reportRequestSeen = requests;
        return fresh;
    }

    bool ReadyForReport(std::uint64_t now)
    {
        const bool addon = AddonAlive(now);
        for (const Mark& mark : g_marks)
        {
            if (!mark.windowClosed)
                return false;
            const std::uint64_t age = now - mark.pressWall;
            if (addon && !mark.noteDone && age < 20000)
                return false;
            if (addon && !mark.shotsDone && age < 12000)
                return false;
        }
        return true;
    }

    std::vector<std::string> PendingMarkFolders()
    {
        std::vector<std::string> out;
        for (const Mark& mark : g_marks)
            out.push_back(mark.folder);
        return out;
    }

    std::vector<std::string> MarkedClips()
    {
        std::vector<std::string> out;
        for (const Mark& mark : g_marks)
            if (!mark.clip.empty() && std::find(out.begin(), out.end(), mark.clip) == out.end())
                out.push_back(mark.clip);
        return out;
    }

    void MarksReported()
    {
        g_marks.clear();
        PublishPendingMarks(GetTickCount64()); // the key bar and the reminders drop them at once
    }

    std::atomic<bool>& CancelFlag() { return g_cancel; }

    void Shutdown()
    {
        g_cancel.store(true);
        g_tapOn.store(false);
        if (g_watching)
            EndWatch(GetTickCount64());
        if (g_shared)
            InterlockedExchange(&g_shared->ringAllowed, 0);
        g_log = nullptr;
    }
}
