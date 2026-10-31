// replay_editor_addon_markers.cpp: the camera markers' panel: the marker menu and its rows (position,
// tools, free camera, shake, spline, attach, look at, depth of field), typing into a row, environment rows.
// Split from replay_editor_addon.cpp (moved code); shared internals in replay_editor_addon_internal.h.

#include "replay_editor_addon_internal.h"

    // ---------------- Edit ----------------
    std::uint32_t g_selectedKeyframeId = 0;
    std::uint32_t g_draggingKeyframeId = 0;
    std::uint64_t g_lastDragTimeMs = UINT64_MAX;
    std::uint64_t g_dragKeyStartMs = 0; // the dragged marker's time at the click
    float g_dragStartX = 0.0f;          // the click, screen pixels
    bool g_dragMoved = false;
    bool g_draggingPlayhead = false;

    const CameraKeyframe* FindKeyframe(std::uint32_t id)
    {
        for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
            if (g_snapshot.cameraKeyframes[n].id == id)
                return &g_snapshot.cameraKeyframes[n];
        return nullptr;
    }

namespace
{

    // "Cut" is the Hold easing: the view jumps at the keyframe.
    const char* kEasings[] = {"Linear", "Smooth", "Ease in", "Ease out", "Cut"};
    const char* kCameraModes[] = {"Free", "Game", "Target", "Follow"};
}

    const char* TargetLabel(std::uint32_t id)
    {
        for (std::uint32_t n = 0; n < g_snapshot.targetCount && n < kMaxTargetsInSnapshot; ++n)
        {
            if (g_snapshot.targets[n].id == id)
                return g_snapshot.targets[n].label;
        }
        return id ? TR("target.not_in_clip", "(not in clip)") : TR("common.none", "None");
    }

namespace
{

    // A keyframe added here is selected once the core lists it: the one
    // nearest the playhead of the request, after the count grew.
    std::uint32_t g_keyTotalBeforeAdd = 0;
    std::uint64_t g_addAtMs = 0;
}
    bool g_selectAdded = false;

namespace
{
    ULONGLONG g_addAskedAt = 0; // an add the core did not make is said so, not left armed
}

    // A short message on the edit screen's status line.
    const char* g_editMessage = "";
    ULONGLONG g_editMessageUntil = 0;
    ULONGLONG g_saveAskedAt = 0; // Ctrl+S waiting for its end
    bool g_saveSeen = false;
    void EditMessage(const char* text, ULONGLONG forMs)
    {
        g_editMessage = text;
        g_editMessageUntil = GetTickCount64() + forMs;
    }

    void AddKeyframeHere()
    {
        // A marker already at the playhead (within two frames) is opened instead
        // (the core refuses a second one there).
        for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
        {
            const CameraKeyframe& key = g_snapshot.cameraKeyframes[n];
            const std::uint64_t d = key.timeMs > g_snapshot.playheadMs ? key.timeMs - g_snapshot.playheadMs : g_snapshot.playheadMs - key.timeMs;
            if (d <= 66)
            {
                g_selectedKeyframeId = key.id;
                PlayUi(UiSound::NavDown);
                Command seek{}; // onto it, so its menu stays open
                seek.timeMs = key.timeMs;
                Send(CommandType::SeekAbsolute, &seek);
                return;
            }
        }
        if (Send(CommandType::CaptureCameraKeyframe))
        {
            PlayUi(UiSound::Select);
            g_keyTotalBeforeAdd = g_snapshot.cameraKeyframeTotal;
            g_addAtMs = g_snapshot.playheadMs;
            g_selectAdded = true;
            g_addAskedAt = GetTickCount64();
        }
    }

    void SelectAddedKeyframe()
    {
        // The core refused the add (512 markers, no room between two close ones):
        // say so, and do not leave the selection armed for the next change of the
        // count (an undo, another clip's bounds).
        if (g_selectAdded && g_snapshot.cameraKeyframeTotal <= g_keyTotalBeforeAdd && GetTickCount64() - g_addAskedAt > 1500)
        {
            g_selectAdded = false;
            PlayUi(UiSound::Error);
            EditMessage(TR("edit.message.no_room", "No room for a marker here"));
            return;
        }
        if (!g_selectAdded || g_snapshot.cameraKeyframeTotal <= g_keyTotalBeforeAdd)
            return;
        g_selectAdded = false;
        std::uint64_t best = ~0ull;
        for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
        {
            const CameraKeyframe& key = g_snapshot.cameraKeyframes[n];
            const std::uint64_t distance = key.timeMs > g_addAtMs ? key.timeMs - g_addAtMs : g_addAtMs - key.timeMs;
            if (distance < best)
            {
                best = distance;
                g_selectedKeyframeId = key.id;
            }
        }
    }

namespace
{

    const char* kPaths[] = {"Straight", "Curved"};
    const int kSpeeds[] = {5, 20, 35, 50, 100, 125, 150, 175, 200};
}
    int g_menuReturnFocus = 0; // the root row a submenu was opened from
    MarkerMenu g_markerMenu = MarkerMenu::Root;
    int g_menuFocus = 0;
    std::uint32_t g_menuMarker = 0;

    // The icons' column before a panel row's label (0 without the tools' icons).
    float EditIconLead()
    {
        using ui_art::Art;
        return AnyArt({Art::ToolCamera, Art::ToolDof, Art::ToolEffects, Art::ToolShake, Art::ToolWeather}) ? 28.0f : 0.0f;
    }

namespace
{

    // The Spline tuning's values last sent (a marker's, by id; the defaults, key
    // 1), the row the Spline Defaults were opened from, and the typed number over
    // a row.
    replay::editor::input::PendingValue<SplineTuning> g_pendingSpline;
    // A marker's bones, attach pose numbers, near clip and DOF numbers last
    // sent.
    replay::editor::input::PendingValue<CameraMarkerExtra> g_pendingExtra;
}
    // A marker's shake last sent.
    replay::editor::input::PendingValue<MarkerShake> g_pendingShake;

namespace
{
    replay::editor::input::PendingValue<SplineTuning> g_pendingSplineDefaults;
}
    int g_splineMenuReturn = 0;     // the Camera menu's Spline Options row
    int g_splineDefaultsReturn = 0; // the Spline menu's Spline Defaults row
    RowTyping g_rowTyping;
    // A typed box had the keys this frame (DrawEdit's keys skip it).
    bool g_fxTypingFrame = false;

namespace
{

    CameraKeyframeOptions OptionsOf(const CameraKeyframe& k)
    {
        CameraKeyframeOptions o{};
        o.cameraMode = k.cameraMode;
        o.lookAtTarget = k.lookAtTarget;
        o.attachTarget = k.attachTarget;
        o.focusTarget = k.focusTarget;
        o.lookAtOffset = k.lookAtOffset;
        o.lookAtRoll = k.lookAtRoll;
        o.presetDistanceScale = k.presetDistanceScale;
        o.fov = k.camera.fov;
        o.dofIntensity = k.dofIntensity;
        o.focusDistance = k.focusDistance;
        o.markerType = k.markerType;
        o.blend = k.blend;
        o.ease = k.ease;
        o.attachType = k.attachType;
        o.shake = k.shake;
        o.shakeIntensity = k.shakeIntensity;
        o.shakeSpeedIndex = k.shakeSpeedIndex;
        o.shakeMotion = k.shakeMotion;
        o.speedIndex = k.speedIndex;
        o.dof = k.dof;
        o.focus = k.focus;
        o.transition = k.transition;
        o.transitionMs = k.transitionMs;
        o.filter = k.filter;
        o.filterIntensity = k.filterIntensity;
        o.saturation = k.saturation;
        o.contrast = k.contrast;
        o.vignette = k.vignette;
        o.brightness = k.brightness;
        return o;
    }

    // The values last sent, stepped from while the snapshot has not brought them
    // back (editor_input.h): otherwise a held arrow or a quick double press on a
    // marker option, the Time row or the playback speed sends the same value
    // twice and the second press is lost.
    replay::editor::input::PendingValue<CameraKeyframeOptions> g_pendingOptions;
    replay::editor::input::PendingValue<std::uint64_t> g_pendingMoveTime;
    replay::editor::input::PendingValue<std::uint32_t> g_pendingSpeed;
}

    // Any other change to a marker (paste, reset, undo, a drag) makes the
    // snapshot the only truth again.
    void ForgetPendingMarkerValues()
    {
        g_pendingOptions.Clear();
        g_pendingMoveTime.Clear();
        g_pendingSpline.Clear();
        g_pendingExtra.Clear();
        g_pendingShake.Clear();
    }

namespace
{

    void SendOptions(const CameraKeyframe& k, std::uint32_t mask, const std::function<void(CameraKeyframeOptions&)>& change)
    {
        Command command{};
        command.keyframeId = k.id;
        const ULONGLONG now = GetTickCount64();
        command.options = g_pendingOptions.Base(k.id, OptionsOf(k), now);
        change(command.options);
        command.options.mask = mask;
        if (Send(CommandType::SetCameraKeyframeOptions, &command))
            g_pendingOptions.Sent(k.id, command.options, now);
    }
}

    // The playback speed preset one step from the one last asked.
    void StepPlaybackSpeed(int step)
    {
        const int count = static_cast<int>(sizeof(kSpeeds) / sizeof(kSpeeds[0]));
        const ULONGLONG now = GetTickCount64();
        const int from = static_cast<int>(g_pendingSpeed.Base(0, g_snapshot.speedIndex, now));
        Command command{};
        command.enabled = static_cast<std::uint32_t>(std::clamp(from + step, 0, count - 1));
        if (static_cast<int>(command.enabled) == from)
        {
            PlayUi(UiSound::Error); // already the slowest or the fastest
            return;
        }
        PlayUiStep(step);
        if (Send(CommandType::SetPlaybackSpeed, &command))
            g_pendingSpeed.Sent(0, command.enabled, now);
    }

namespace
{
    replay::editor::input::PendingValue<std::uint64_t> g_pendingSeek;
}

    void SeekBy(std::int64_t deltaMs)
    {
        const ULONGLONG now = GetTickCount64();
        const std::int64_t from = static_cast<std::int64_t>(g_pendingSeek.Base(0, g_snapshot.playheadMs, now));
        const std::int64_t end = static_cast<std::int64_t>(g_snapshot.durationMs);
        Command seek{};
        seek.timeMs = static_cast<std::uint64_t>(std::clamp<std::int64_t>(from + deltaMs, 0, end));
        if (static_cast<std::int64_t>(seek.timeMs) == from)
            return; // at the start or the end
        if (Send(CommandType::SeekAbsolute, &seek))
            g_pendingSeek.Sent(0, seek.timeMs, now);
    }

    // Delete (the menu's row and the Delete key): the clip's start and end stay
    // (an error sound, the core would refuse them too), and the focus goes to the
    // nearest marker left instead of closing the menu.
    void DeleteMarker(std::uint32_t id)
    {
        const std::uint32_t count = g_snapshot.cameraKeyframeCount;
        std::uint32_t index = count;
        for (std::uint32_t n = 0; n < count; ++n)
            if (g_snapshot.cameraKeyframes[n].id == id)
                index = n;
        if (index >= count)
            return;
        if (count >= 2 && (index == 0 || index + 1 == count))
        {
            PlayUi(UiSound::Error);
            return;
        }
        Command c{};
        c.keyframeId = id;
        if (!Send(CommandType::DeleteCameraKeyframe, &c))
            return;
        ForgetPendingMarkerValues();
        PlayUi(UiSound::Back);
        const std::uint64_t at = g_snapshot.cameraKeyframes[index].timeMs;
        std::uint64_t nearest = ~0ull;
        g_selectedKeyframeId = 0;
        for (std::uint32_t n = 0; n < count; ++n)
        {
            if (n == index)
                continue;
            const std::uint64_t t = g_snapshot.cameraKeyframes[n].timeMs;
            const std::uint64_t distance = t > at ? t - at : at - t;
            if (distance < nearest)
            {
                nearest = distance;
                g_selectedKeyframeId = g_snapshot.cameraKeyframes[n].id;
            }
        }
        // The playhead goes to the marker selected.
        if (const CameraKeyframe* next = FindKeyframe(g_selectedKeyframeId))
        {
            Command seek{};
            seek.timeMs = next->timeMs;
            Send(CommandType::SeekAbsolute, &seek);
        }
    }

namespace
{

    // The camera list order: the presets, the game camera, free.
    const CameraMode kCameraCycle[] = {CameraMode::FrontLow, CameraMode::Rear, CameraMode::RightSide, CameraMode::LeftSide,
        CameraMode::Overhead, CameraMode::Game, CameraMode::Free};
    const char* CameraName(CameraMode mode)
    {
        switch (mode)
        {
        case CameraMode::Game: return TR("camera.game", "Game Camera");
        case CameraMode::FrontLow: return TR("camera.front_low", "Front Low");
        case CameraMode::Rear: return TR("camera.rear", "Rear");
        case CameraMode::RightSide: return TR("camera.right_side", "Right Side");
        case CameraMode::LeftSide: return TR("camera.left_side", "Left Side");
        case CameraMode::Overhead: return TR("camera.overhead", "Overhead");
        default: return TR("camera.free", "Free Camera");
        }
    }
    bool IsPreset(CameraMode mode) { return mode >= CameraMode::FrontLow && mode <= CameraMode::Overhead; }
    // A list of choice names (TR_NOOP entries) and one of them in the player's language.
    struct NameText
    {
        const char* key;
        const char* english;
    };
    template <std::size_t N>
    const char* NameOf(const NameText (&names)[N], int index)
    {
        const NameText& name = names[std::clamp(index, 0, static_cast<int>(N) - 1)];
        return TR(name.key, name.english);
    }
    const NameText kBlendNames[] = {{TR_NOOP("marker.blend.none", "No Blend")}, {TR_NOOP("marker.blend.linear", "Linear")},
        {TR_NOOP("marker.blend.smooth", "Smooth")}, {TR_NOOP("marker.blend.spline", "Spline")}};
    const NameText kAttachNames[] = {{TR_NOOP("marker.mount.rigid", "Rigid")}, {TR_NOOP("marker.mount.default", "Default")},
        {TR_NOOP("marker.mount.flexible", "Flexible")}};
    const NameText kShakeNames[] = {{TR_NOOP("common.none", "None")}, {TR_NOOP("shake.handheld", "Handheld")}, {TR_NOOP("shake.drunk", "Drunk")},
        {TR_NOOP("shake.ground", "Ground Vibration")}, {TR_NOOP("shake.air", "Air Turbulence")}, {TR_NOOP("shake.explosion", "Explosion")},
        {TR_NOOP("shake.walking", "Walking")}, {TR_NOOP("shake.running", "Running")}, {TR_NOOP("shake.horseback", "Horseback")},
        {TR_NOOP("shake.steadicam", "Steadicam")}, {TR_NOOP("shake.wagon", "Wagon")}, {TR_NOOP("shake.custom", "Custom")},
        {TR_NOOP("shake.gunshot", "Gunshot")}};
    const NameText kDofNames[] = {{TR_NOOP("marker.dof.default", "Default")}, {TR_NOOP("marker.dof.custom", "Custom")}, {TR_NOOP("common.none", "None")}};
    const NameText kFocusNames[] = {{TR_NOOP("marker.focus.auto", "Auto")}, {TR_NOOP("marker.focus.manual", "Manual")}, {TR_NOOP("marker.focus.target", "Target")}};
    const NameText kAnchorNames[] = {{TR_NOOP("common.off_cap", "Off")}, {TR_NOOP("common.on_cap", "On")}, {TR_NOOP("marker.anchor.exclusive", "Exclusive")}};

    template <typename E>
    E Cycle(E value, int step, int count)
    {
        return static_cast<E>((static_cast<int>(value) + (step > 0 ? 1 : -1) + count) % count);
    }

    // The next target (or None) after `id`, +1 or -1.
    std::uint32_t StepTargetOrNone(std::uint32_t id, int step)
    {
        const int count = static_cast<int>(std::min(g_snapshot.targetCount, kMaxTargetsInSnapshot));
        int index = -1; // -1 = None
        for (int n = 0; n < count; ++n)
            if (g_snapshot.targets[n].id == id)
                index = n;
        index = (index + 1 + (step > 0 ? 1 : -1) + count + 1) % (count + 1) - 1;
        return index < 0 ? 0 : g_snapshot.targets[index].id;
    }

    std::string FormatValue(const char* format, double value)
    {
        char text[48]{};
        std::snprintf(text, sizeof(text), format, value);
        return text;
    }

    // ---------------- Camera tools ----------------
    // The marker's numbers (Position), the move's tools (Move Tools) and the
    // free camera's options with the world markers (FREE CAMERA).

    struct PendingPose
    {
        std::uint32_t id = 0;
        CameraState camera{};
        ULONGLONG at = 0;
    };
    PendingPose g_pendingPose;

    // The pose through UpdateCameraKeyframe, stepped from the value last
    // sent while the snapshot has not brought it back (a held arrow).
    void SendPose(const CameraKeyframe& k, const std::function<void(CameraState&)>& change)
    {
        const ULONGLONG now = GetTickCount64();
        CameraState camera = g_pendingPose.id == k.id && now - g_pendingPose.at < 700 ? g_pendingPose.camera : k.camera;
        change(camera);
        Command command{};
        command.keyframeId = k.id;
        command.camera = camera;
        command.easing = k.easing;
        command.path = k.path;
        if (Send(CommandType::UpdateCameraKeyframe, &command))
            g_pendingPose = {k.id, camera, now};
    }

    float WrapAngle(float degrees)
    {
        while (degrees > 180.0f)
            degrees -= 360.0f;
        while (degrees <= -180.0f)
            degrees += 360.0f;
        return degrees;
    }

    std::vector<MenuRow> BuildPositionRows(const CameraKeyframe& k)
    {
        std::vector<MenuRow> rows;
        const auto metres = [&](const char* label, float Vec3::*axis) {
            MenuRow row{label, FormatValue("%.2f m", k.camera.position.*axis), true, 0};
            row.step = [k, axis](int step) {
                SendPose(k, [&](CameraState& c) { c.position.*axis += static_cast<float>(step) * 0.1f; });
            };
            row.help = KeyFill(TR("position.metres.help", "0.1 m a step ({key_page_up}/{key_page_down}: 0.5 m)."));
            rows.push_back(row);
        };
        metres("X", &Vec3::x);
        metres("Y", &Vec3::y);
        metres(TR("position.z", "Z (height)"), &Vec3::z);
        MenuRow pitch{TR("position.pitch", "Pitch"), FormatValue("%.0f", k.camera.rotation.x), true, 0};
        pitch.step = [k](int step) { SendPose(k, [&](CameraState& c) { c.rotation.x = std::clamp(c.rotation.x + step, -89.0f, 89.0f); }); };
        pitch.help = TR("position.pitch.help", "Up and down, one degree a step.");
        rows.push_back(pitch);
        MenuRow heading{TR("position.heading", "Heading"), FormatValue("%.0f", k.camera.rotation.z), true, 0};
        heading.step = [k](int step) { SendPose(k, [&](CameraState& c) { c.rotation.z = WrapAngle(c.rotation.z - step); }); };
        heading.help = TR("position.heading.help", "Left and right, one degree a step.");
        rows.push_back(heading);
        MenuRow roll{TR("position.roll", "Roll"), FormatValue("%.0f", k.camera.rotation.y), true, 0};
        roll.step = [k](int step) { SendPose(k, [&](CameraState& c) { c.rotation.y = WrapAngle(c.rotation.y + step); }); };
        roll.help = TR("position.roll.help", "Tilt the picture, one degree a step.");
        rows.push_back(roll);
        return rows;
    }

    int g_stretchPercent = 100;
    std::uint32_t g_lockTarget = 0;
    AttachType g_lockType = AttachType::Default;

    std::vector<MenuRow> BuildToolRows(const CameraKeyframe& k)
    {
        std::vector<MenuRow> rows;
        const bool playing = Flag(SnapshotPlaying);
        auto action = [&](const char* label, std::function<void()> run, const char* help, bool enabled = true) {
            MenuRow row{label, "", enabled && playing, 2};
            row.enter = std::move(run);
            row.help = help;
            rows.push_back(row);
        };
        const auto send = [](CommandType type, std::uint32_t id, std::uint32_t enabled, std::uint32_t target = 0) {
            Command c{};
            c.keyframeId = id;
            c.enabled = enabled;
            c.target = target;
            c.options.attachType = g_lockType;
            ForgetPendingMarkerValues();
            Send(type, &c);
        };
        action(TR("tools.duplicate", "Duplicate"), [k, send] { send(CommandType::DuplicateCameraKeyframe, k.id, 0); },
            TR("tools.duplicate.help", "A copy halfway to the next marker (2 s on after the last): a new point to shape the curve."));
        action(TR("tools.even_speed", "Even Out Speed"), [k, send] { send(CommandType::DistributeCameraKeyframes, k.id, 1); },
            TR("tools.even_speed.help", "The move's inner markers retimed by distance: the camera travels at one speed. The first and last keep their times."));
        action(TR("tools.even_times", "Even Out Times"), [k, send] { send(CommandType::DistributeCameraKeyframes, k.id, 0); },
            TR("tools.even_times.help", "The move's inner markers at equal times between its first and last."));
        MenuRow stretch{TR("tools.stretch", "Stretch Move"), FormatValue("%.0f%%", g_stretchPercent), playing, 0};
        stretch.step = [](int step) { g_stretchPercent = std::clamp(g_stretchPercent + (step > 0 ? 5 : -5) * (std::abs(step) > 1 ? 5 : 1), 25, 400); };
        stretch.enter = [k, send] {
            if (g_stretchPercent != 100)
                send(CommandType::StretchCameraKeyframes, k.id, static_cast<std::uint32_t>(g_stretchPercent));
            g_stretchPercent = 100;
        };
        stretch.help = KeyFill(TR("tools.stretch.help", "{key_select} applies: the move plays over this share of its time (over 100% slower). The action is "
                       "not touched; markers after the move move only when the spacing needs it."));
        rows.push_back(stretch);
        if (!g_lockTarget && g_snapshot.targetCount > 0)
            g_lockTarget = k.attachTarget ? k.attachTarget : g_snapshot.targets[0].id;
        MenuRow target{TR("tools.lock_target", "Lock Target"), TargetLabel(g_lockTarget), playing, 0};
        target.step = [](int step) { g_lockTarget = StepTargetOrNone(g_lockTarget, step); };
        target.help = TR("tools.lock_target.help", "The character, horse or vehicle the move locks to.");
        rows.push_back(target);
        MenuRow mount{TR("tools.lock_mount", "Lock Mount"), NameOf(kAttachNames, std::clamp(static_cast<int>(g_lockType), 0, 2)), playing, 0};
        mount.step = [](int step) { g_lockType = Cycle(g_lockType, step, 3); };
        mount.help = TR("tools.lock_mount.help", "Rigid turns with the target, Default follows its heading, Flexible only its position.");
        rows.push_back(mount);
        action(TR("tools.lock_move", "Lock This Move"), [k, send] { send(CommandType::LockCameraKeyframes, k.id, 0, g_lockTarget); },
            TR("tools.lock_move.help", "Every marker of the move rides the target, each keeping the view it shows now."), g_lockTarget != 0);
        action(TR("tools.lock_all", "Lock Every Marker"), [k, send] { send(CommandType::LockCameraKeyframes, k.id, 1, g_lockTarget); },
            TR("tools.lock_all.help", "Every free marker of the clip rides the target, each keeping its view."), g_lockTarget != 0);
        action(TR("tools.unlock", "Unlock This Move"), [k, send] { send(CommandType::LockCameraKeyframes, k.id, 0, 0); },
            TR("tools.unlock.help", "The move's markers drop their target and stay where they show now."));
        return rows;
    }
}

    bool g_freeCamMenuOpen = false;
    bool g_splineDefaultsOpen = false; // the Spline Defaults, opened from the free camera's menu

namespace
{
    struct PendingFreeCam
    {
        float value = 0.0f;
        ULONGLONG at = 0;
    };
    PendingFreeCam g_pendingFreeCam[static_cast<std::size_t>(FreeCamField::Count)];

    float FreeCamShown(FreeCamField field)
    {
        const std::size_t index = static_cast<std::size_t>(field);
        const PendingFreeCam& pending = g_pendingFreeCam[index];
        return pending.at && GetTickCount64() - pending.at < 700 ? pending.value : g_snapshot.freeCamValues[index];
    }

    void SendFreeCam(FreeCamField field, float value)
    {
        Command c{};
        c.keyframeId = static_cast<std::uint32_t>(field);
        c.camera.fov = value;
        if (Send(CommandType::SetFreeCamOption, &c))
            g_pendingFreeCam[static_cast<std::size_t>(field)] = {value, GetTickCount64()};
    }

    void SendFreeCamAction(FreeCamActionId action, std::uint32_t target = 0)
    {
        Command c{};
        c.keyframeId = static_cast<std::uint32_t>(action);
        c.target = target;
        Send(CommandType::FreeCamAction, &c);
    }

    void SendOverlay(OverlayField field, bool on)
    {
        Command c{};
        c.keyframeId = static_cast<std::uint32_t>(field);
        c.enabled = on ? 1u : 0u;
        Send(CommandType::SetCameraOverlay, &c);
    }
}

    std::vector<MenuRow> BuildFreeCamRows()
    {
        std::vector<MenuRow> rows;
        const bool flying = Flag(SnapshotFreeCamEnabled);
        const auto toggle = [&](const char* label, FreeCamField field, const char* on, const char* off, const char* help) {
            const bool value = FreeCamShown(field) >= 0.5f;
            MenuRow row{label, value ? on : off, true, 0};
            row.step = [field](int) { SendFreeCam(field, FreeCamShown(field) >= 0.5f ? 0.0f : 1.0f); };
            row.help = help;
            rows.push_back(row);
        };
        const auto number = [&](const char* label, FreeCamField field, const char* format, float delta, float lo, float hi,
                                const char* help) {
            MenuRow row{label, FormatValue(format, FreeCamShown(field)), true, 0};
            row.step = [field, delta, lo, hi](int step) {
                SendFreeCam(field, std::clamp(FreeCamShown(field) + static_cast<float>(step) * delta, lo, hi));
            };
            row.help = help;
            rows.push_back(row);
        };
        const bool drone = FreeCamShown(FreeCamField::Motion) >= 0.5f;
        toggle(TR("freecam.flight", "Flight"), FreeCamField::Motion, TR("freecam.flight.drone", "Drone"), TR("freecam.flight.classic", "Classic"), "");
        rows.back().help = KeyFill(TR("freecam.flight.help", "Drone: the camera has weight (it speeds up, coasts and banks into turns). Classic: it goes where you push "
                                      "it. ({key_drone})"));
        toggle(TR("freecam.acrobatic", "Acrobatic"), FreeCamField::Acrobatic, TR("common.on_cap", "On"), TR("common.off_cap", "Off"), "");
        rows.back().help = KeyFill(TR("freecam.acrobatic.help", "Turns about the camera's own axes: full rolls and loops. ({key_acrobatic})"));
        toggle(TR("freecam.walk", "Walk"), FreeCamField::Walk, TR("common.on_cap", "On"), TR("common.off_cap", "Off"), "");
        rows.back().help = KeyFill(TR("freecam.walk.help", "The camera keeps to the ground at eye height. ({key_walk})"));
        if (FreeCamShown(FreeCamField::Walk) >= 0.5f)
            number(TR("freecam.eye_height", "Eye Height"), FreeCamField::WalkHeight, "%.1f m", 0.1f, 0.3f, 5.0f, TR("freecam.eye_height.help", "Above the ground, 0.1 m a step."));
        toggle(TR("freecam.collision", "Collision"), FreeCamField::Collision, TR("common.on_cap", "On"), TR("common.off_cap", "Off"), "");
        rows.back().help = KeyFill(TR("freecam.collision.help", "The camera stops at walls and the ground. ({key_collision})"));
        toggle(TR("freecam.altitude", "Altitude"), FreeCamField::LockAltitude, TR("freecam.altitude.locked", "Locked"), TR("freecam.altitude.free", "Free"), "");
        rows.back().help = KeyFill(TR("freecam.altitude.help", "Locked: moving forward keeps the height ({key_fly_down} and {key_fly_up} still go down and "
                                      "up). ({key_altitude})"));
        MenuRow target{TR("freecam.target", "Target"), g_snapshot.freeCamTarget ? TargetLabel(g_snapshot.freeCamTarget) : TR("common.none", "None"), true, 0};
        target.step = [](int step) { SendFreeCamAction(FreeCamActionId::SetTarget, StepTargetOrNone(g_snapshot.freeCamTarget, step)); };
        target.help = KeyFill(TR("freecam.target.help", "Who the camera follows, and whose recorded path is drawn. ({key_pick}) picks the one under the crosshair."));
        rows.push_back(target);
        const char* const kFollowNames[] = {TR("common.off_cap", "Off"), TR("freecam.follow.rigid", "Rigid"), TR("freecam.follow.position", "Position Only")};
        MenuRow follow{TR("freecam.follow", "Follow"), kFollowNames[std::min<std::uint32_t>(g_snapshot.freeCamFollow, 2)], g_snapshot.freeCamTarget != 0, 0};
        follow.step = [](int step) {
            SendFreeCam(FreeCamField::Follow, static_cast<float>((g_snapshot.freeCamFollow + (step > 0 ? 1 : 2)) % 3));
        };
        follow.help = KeyFill(TR("freecam.follow.help", "Rigid: the camera rides the target and turns with it. Position Only: it moves with it and keeps its own view. ({key_follow})"));
        rows.push_back(follow);
        auto action = [&](const char* label, FreeCamActionId id, std::string help, bool enabled) {
            MenuRow row{label, "", enabled, 2};
            row.enter = [id] { SendFreeCamAction(id); };
            row.help = std::move(help);
            rows.push_back(row);
        };
        action(TR("freecam.pick_aimed", "Target Under Crosshair"), FreeCamActionId::PickAimed, KeyFill(TR("freecam.pick_aimed.help", "The character, horse or vehicle at the centre of the view (free camera on). ({key_pick})")), flying);
        action(TR("freecam.pick_nearest", "Nearest Target"), FreeCamActionId::PickNearest, KeyFill(TR("freecam.pick_nearest.help", "The nearest one to the camera. ({key_pick_nearest})")), flying);
        action(TR("freecam.level", "Level Horizon"), FreeCamActionId::LevelHorizon, KeyFill(TR("freecam.level.help", "Roll back to level. ({key_level})")), flying);
        action(TR("freecam.behind", "Behind The Character"), FreeCamActionId::ShotBehindMain, KeyFill(TR("freecam.behind.help", "The camera behind the target, or the main character. ({key_behind})")), flying);
        action((g_snapshot.freeCamFlags & FreeCamLocked) ? TR("freecam.release", "Release Camera") : TR("freecam.hold", "Hold Camera Still"), FreeCamActionId::ToggleLock,
            KeyFill(TR("freecam.hold.help", "The camera ignores the keys and the mouse (it still follows its target). ({key_hold})")), flying);
        if (drone)
        {
            number(TR("freecam.drag", "Drag"), FreeCamField::DroneDrag, "%.1f", 0.2f, 0.2f, 12.0f, TR("freecam.drag.help", "How fast it coasts to a stop (higher: sooner)."));
            number(TR("freecam.acceleration", "Acceleration"), FreeCamField::DroneAcceleration, "%.0f", 1.0f, 2.0f, 60.0f, TR("freecam.acceleration.help", "How hard it pushes (top speed is acceleration x 2 / drag)."));
            number(TR("freecam.gravity", "Gravity"), FreeCamField::DroneGravity, "%.1f", 0.5f, 0.0f, 20.0f, TR("freecam.gravity.help", "A pull down while it does not climb (0: none)."));
            number(TR("freecam.banking", "Banking"), FreeCamField::DroneBanking, "%.0f", 1.0f, 0.0f, 45.0f, TR("freecam.banking.help", "How much it rolls into a turn."));
            number(TR("freecam.turn_smoothing", "Turn Smoothing"), FreeCamField::DroneRotSmoothing, "%.0f", 1.0f, 1.0f, 30.0f, TR("freecam.turn_smoothing.help", "Lower: heavier, slower turns."));
            number(TR("freecam.zoom_smoothing", "Zoom Smoothing"), FreeCamField::DroneFovSmoothing, "%.0f", 1.0f, 0.0f, 20.0f, TR("freecam.zoom_smoothing.help", "Lower: softer zoom (0: none)."));
        }
        number(TR("freecam.look", "Look Sensitivity"), FreeCamField::LookSensitivity, "%.0f", 1.0f, 2.0f, 30.0f, TR("freecam.look.help", "The mouse's turn."));
        // The flying free camera's near clip (Edit Camera uses the marker's).
        number(TR("freecam.near_clip", "Near Clip"), FreeCamField::NearClip, "%.2f m", 0.01f, 0.01f, 1.0f,
            TR("freecam.near_clip.help", "How close the camera gets before things are cut. Lower gets closer; under about 0.05 m far edges can flicker. "
            "Edit Camera uses the marker's Near Clip."));
        MenuRow reset{TR("freecam.reset", "Reset Free Camera"), "", true, 2};
        reset.enter = [] { SendFreeCam(FreeCamField::ResetDefaults, 0.0f); };
        reset.help = TR("freecam.reset.help", "Every option above back to its default.");
        rows.push_back(reset);
        const auto overlay = [&](const char* label, OverlayField field, std::uint32_t flag, const char* help) {
            const bool on = (g_snapshot.overlayFlags & flag) != 0;
            MenuRow row{label, on ? TR("common.on_cap", "On") : TR("common.off_cap", "Off"), true, 0};
            row.step = [field, on](int) { SendOverlay(field, !on); };
            row.help = help;
            rows.push_back(row);
        };
        overlay(TR("overlay.world", "World Markers"), OverlayField::Enabled, OverlayEnabled, "");
        rows.back().help = KeyFill(TR("overlay.world.help", "The markers, their look arrows and the camera's path drawn in the world (never in an export). ({key_overlay})"));
        overlay(TR("overlay.while_playing", "Markers While Playing"), OverlayField::WhilePlaying, OverlayWhilePlaying,
            TR("overlay.while_playing.help", "Off: shown when paused, in Edit Camera and with the free camera. On: also, faint, while the clip plays."));
        overlay(TR("overlay.time_labels", "Time Labels"), OverlayField::TimeLabels, OverlayTimeLabels, TR("overlay.time_labels.help", "The seconds along the path and the target's trail."));
        overlay(TR("overlay.trail", "Target Trail"), OverlayField::TargetTrail, OverlayTargetTrail, TR("overlay.trail.help", "The free camera target's recorded path, 4 s back and 8 s ahead."));
        // The Spline blend's defaults (also from a marker's Spline Options).
        MenuRow spline{TR("spline.defaults", "Spline Defaults"), ">", true, 1};
        spline.enter = [] {
            g_splineDefaultsOpen = true;
            g_menuFocus = 0;
        };
        spline.help = TR("spline.defaults.help", "Speed profile, curve shape, ease, camera weight and channels every Spline marker uses unless it sets its own.");
        rows.push_back(spline);
        return rows;
    }

    // ---------------- The marker's shake ----------------
    // The shake lives in the marker (older shake events are folded into the
    // markers): Shake (off, a realistic profile or Simple Camera's noise),
    // Intensity, Frequency, Transition (Cut or Blend to the next marker) and the
    // Advanced numbers. The values last sent show while the snapshot lags
    // (PendingValue, as the other marker rows).
    int g_shakeAdvancedReturn = 0; // the Shake menu's Advanced row

namespace
{

    MarkerShake MarkerShakeOf(const CameraKeyframe& k)
    {
        for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
            if (g_snapshot.cameraKeyframes[n].id == k.id)
                return g_pendingShake.Base(k.id, g_snapshot.cameraKeyframeShake[n], GetTickCount64());
        return DefaultMarkerShake();
    }

    void SendMarkerShake(const CameraKeyframe& k, const std::function<void(MarkerShake&)>& edit)
    {
        MarkerShake s = MarkerShakeOf(k);
        edit(s);
        Command command{};
        command.keyframeId = k.id;
        const ULONGLONG now = GetTickCount64();
        command.options = g_pendingOptions.Base(k.id, OptionsOf(k), now);
        command.options.mask = CameraOptionShake;
        command.shake = s;
        if (Send(CommandType::SetCameraKeyframeOptions, &command))
            g_pendingShake.Sent(k.id, s, now);
    }

    // Intensity 1..10 and its gain ((i + 1) / 6)^2.
    float ShakeGainOf(int intensity)
    {
        const float g = (static_cast<float>(std::clamp(intensity, 1, 10)) + 1.0f) / 6.0f;
        return g * g;
    }
    int ShakeIntensityOf(float gain)
    {
        return std::clamp(static_cast<int>(std::floor(6.0f * std::sqrt(std::max(0.0f, gain)) - 1.0f + 0.5f)), 1, 10);
    }

    // The Shake row: Off, the eleven profiles (Gunshot after Explosion, the
    // two one shots together), Simple Camera's four presets (its noise with
    // their numbers) and Simple Custom (the noise with the marker's own
    // numbers).
    constexpr std::uint8_t kShakeProfileOrder[] = {1, 2, 3, 4, 5, 12, 6, 7, 8, 9, 10};
    constexpr int kShakeProfileChoices = static_cast<int>(sizeof(kShakeProfileOrder));
    constexpr int kShakePresetFirst = 1 + kShakeProfileChoices;
    constexpr int kShakeChoiceCount = kShakePresetFirst + 4 + 1;
    const NameText kShakePresetNames[] = {{TR_NOOP("shake.simple_subtle", "Simple: Subtle")}, {TR_NOOP("shake.simple_handheld", "Simple: Handheld")},
        {TR_NOOP("shake.simple_vehicle", "Simple: Vehicle")}, {TR_NOOP("shake.simple_earthquake", "Simple: Earthquake")}};
    int ShakeChoice(const MarkerShake& s)
    {
        if (!s.on)
            return 0;
        for (int n = 0; n < kShakeProfileChoices; ++n)
            if (s.profile != 0 && s.profile == kShakeProfileOrder[n])
                return 1 + n;
        for (int p = 0; p < 4; ++p)
        {
            const replay::camera::fx::PresetValues& v = replay::camera::fx::kPresets[p + 1];
            if (s.amp == v.amp && s.hz == v.hz && s.speedAmp == v.speedAmp && s.speedHz == v.speedHz)
                return kShakePresetFirst + p;
        }
        return kShakePresetFirst + 4;
    }
    const char* ShakeChoiceName(int choice)
    {
        if (choice <= 0)
            return TR("common.off_cap", "Off");
        if (choice < kShakePresetFirst)
            return NameOf(kShakeNames, kShakeProfileOrder[choice - 1]);
        if (choice < kShakePresetFirst + 4)
            return NameOf(kShakePresetNames, choice - kShakePresetFirst);
        return TR("shake.simple_custom", "Simple: Custom");
    }
    void ApplyShakeChoice(MarkerShake& s, int choice)
    {
        if (choice <= 0)
        {
            s.on = 0;
            return;
        }
        s.on = 1;
        if (choice < kShakePresetFirst)
        {
            // A profile at its own speed (4 Hz) and size (Amp 1).
            s.profile = kShakeProfileOrder[choice - 1];
            s.amp = 1.0f;
            s.hz = 4.0f;
            s.speedHz = 0.0f;
            return;
        }
        s.profile = 0;
        if (choice < kShakePresetFirst + 4)
        {
            const replay::camera::fx::PresetValues& v = replay::camera::fx::kPresets[choice - kShakePresetFirst + 1];
            s.amp = v.amp;
            s.hz = v.hz;
            s.speedAmp = v.speedAmp;
            s.speedHz = v.speedHz;
        }
    }
    std::string ShakeSummary(const MarkerShake& s)
    {
        if (!s.on)
            return TR("common.off_cap", "Off");
        return std::string(ShakeChoiceName(ShakeChoice(s))) + " " + std::to_string(ShakeIntensityOf(s.gain));
    }

    std::vector<MenuRow> BuildShakeRows(const CameraKeyframe& k)
    {
        std::vector<MenuRow> rows;
        const MarkerShake s = MarkerShakeOf(k);
        MenuRow kind{TR("shake.row", "Shake"), ShakeChoiceName(ShakeChoice(s)), true, 0};
        kind.step = [k](int step) {
            SendMarkerShake(k, [step](MarkerShake& v) {
                const int next = (ShakeChoice(v) + (step > 0 ? 1 : -1) + kShakeChoiceCount) % kShakeChoiceCount;
                ApplyShakeChoice(v, next);
            });
        };
        kind.help = TR("shake.row.help", "The camera shake from this marker to the next one: off, a realistic profile (Handheld to Wagon), or "
                    "Simple Camera's noise (its presets, or Custom with the numbers in Advanced). Explosion and Gunshot hit once, at this "
                    "marker (Restart Here fires them again).");
        rows.push_back(kind);
        MenuRow intensity{TR("shake.intensity", "Intensity"), FormatValue("%.0f", ShakeIntensityOf(s.gain)), s.on != 0, 0};
        intensity.step = [k](int step) {
            SendMarkerShake(k, [step](MarkerShake& v) { v.gain = ShakeGainOf(ShakeIntensityOf(v.gain) + (step > 0 ? 1 : -1)); });
        };
        intensity.help = TR("shake.intensity.help", "1 to 10 (5: the shake as tuned).");
        rows.push_back(intensity);
        char hz[48]{};
        if (s.profile != 0)
            std::snprintf(hz, sizeof(hz), "%.1f Hz  (%.0f%%)", s.hz, s.hz / 4.0f * 100.0f);
        else
            std::snprintf(hz, sizeof(hz), "%.1f Hz", s.hz);
        MenuRow frequency{TR("shake.frequency", "Frequency"), hz, s.on != 0, 0};
        frequency.step = [k](int step) {
            SendMarkerShake(k, [step](MarkerShake& v) { v.hz = std::clamp(v.hz + static_cast<float>(step) * 0.1f, 0.0f, 30.0f); });
        };
        frequency.help = KeyFill(TR("shake.frequency.help", "How fast it shakes ({key_page_up}/{key_page_down}: half a hertz). A profile plays at its own speed at 4 Hz."));
        rows.push_back(frequency);
        const bool blend = s.transition == static_cast<std::uint8_t>(ShakeTransition::Blend) && s.blendMask != 0;
        MenuRow transition{TR("shake.transition", "Transition"), !blend ? TR("shake.transition.cut", "Cut") : s.blendMask == ShakeBlendAll ? TR("shake.transition.blend", "Blend") : TR("shake.transition.part", "Blend (part)"), s.on != 0, 0};
        transition.step = [k](int) {
            SendMarkerShake(k, [](MarkerShake& v) {
                const bool on = v.transition == static_cast<std::uint8_t>(ShakeTransition::Blend) && v.blendMask != 0;
                v.transition = static_cast<std::uint8_t>(on ? ShakeTransition::Cut : ShakeTransition::Blend);
                v.blendMask = ShakeBlendAll;
            });
        };
        transition.help = TR("shake.transition.help", "Cut: this shake holds until the next marker. Blend: its numbers go smoothly to the next marker's "
                          "(Simple Camera's Ramp). Part: an old clip blended only some.");
        rows.push_back(transition);
        MenuRow advanced{TR("shake.advanced", "Advanced"), ">", s.on != 0, 1};
        advanced.enter = [n = rows.size()] {
            g_shakeAdvancedReturn = static_cast<int>(n);
            g_markerMenu = MarkerMenu::ShakeAdvanced;
            g_menuFocus = 0;
        };
        advanced.help = TR("shake.advanced.help", "Amplitude, the speed couplings, the weights, Stop When Still, the pattern, the clock.");
        rows.push_back(advanced);
        return rows;
    }

    std::vector<MenuRow> BuildShakeAdvancedRows(const CameraKeyframe& k)
    {
        std::vector<MenuRow> rows;
        const MarkerShake s = MarkerShakeOf(k);
        const auto number = [&](const char* label, const char* format, float value, float delta, float lo, float hi,
                                float MarkerShake::*field, const char* help) {
            MenuRow row{label, FormatValue(format, value), true, 0};
            row.step = [k, delta, lo, hi, field](int step) {
                SendMarkerShake(k, [&](MarkerShake& v) { v.*field = std::clamp(v.*field + static_cast<float>(step) * delta, lo, hi); });
            };
            row.help = help;
            rows.push_back(row);
        };
        number(TR("shake.amplitude", "Amplitude"), "%.2f", s.amp, 0.01f, 0.0f, 5.0f, &MarkerShake::amp,
            TR("shake.amplitude.help", "The noise: rotation up to amplitude x 1 degree, position amplitude x 5 cm. A profile: its size (1 as tuned)."));
        number(TR("shake.speed_amplitude", "Speed > Amplitude"), "%.2f", s.speedAmp, 0.05f, 0.0f, 2.0f, &MarkerShake::speedAmp,
            TR("shake.speed_amplitude.help", "Stronger as the camera moves faster: at the reference speed, x (1 + this)."));
        number(TR("shake.speed_frequency", "Speed > Frequency"), "%.2f", s.speedHz, 0.05f, 0.0f, 2.0f, &MarkerShake::speedHz,
            TR("shake.speed_frequency.help", "Faster as the camera moves faster: at the reference speed, frequency x (1 + this)."));
        number(TR("shake.reference_speed", "Reference Speed"), "%.0f m/s", s.speedRef, 1.0f, 1.0f, 100.0f, &MarkerShake::speedRef,
            TR("shake.reference_speed.help", "The camera speed counted as full for the two couplings."));
        number(TR("shake.rotation_weight", "Rotation Weight"), "%.2f", s.rotWeight, 0.05f, 0.0f, 2.0f, &MarkerShake::rotWeight,
            TR("shake.rotation_weight.help", "How much the shake turns the camera (0: none)."));
        number(TR("shake.position_weight", "Position Weight"), "%.2f", s.posWeight, 0.05f, 0.0f, 2.0f, &MarkerShake::posWeight,
            TR("shake.position_weight.help", "How much the shake moves the camera (0: none)."));
        const auto toggle = [&](const char* label, bool on, const std::function<void(MarkerShake&)>& flip, const char* help) {
            MenuRow row{label, on ? TR("common.on_cap", "On") : TR("common.off_cap", "Off"), true, 0};
            row.step = [k, flip](int) { SendMarkerShake(k, flip); };
            row.help = help;
            rows.push_back(row);
        };
        toggle(TR("shake.still", "Stop When Still"), s.still != 0, [](MarkerShake& v) { v.still = v.still ? 0 : 1; },
            TR("shake.still.help", "Fade the shake out while the camera stands still (back in when it moves)."));
        MenuRow pattern{TR("shake.pattern", "Pattern"), FormatValue("%.0f", s.seed), true, 0};
        pattern.step = [k](int step) {
            SendMarkerShake(k, [step](MarkerShake& v) { v.seed = static_cast<std::uint16_t>(static_cast<int>(v.seed) + step); });
        };
        pattern.help = TR("shake.pattern.help", "Another noise pattern with the same numbers (Simple Camera's Randomize, the same on every play).");
        rows.push_back(pattern);
        MenuRow speed{TR("shake.speed", "Shake Speed"), FormatValue("%.0f%%", s.rate * 100.0f), true, 0};
        speed.step = [k](int step) {
            SendMarkerShake(k, [step](MarkerShake& v) {
                // The marker speed presets (5 % to 200 %), from the one nearest.
                const int count = static_cast<int>(kMarkerSpeedCount);
                int at = 0;
                for (int n = 0; n < count; ++n)
                    if (std::fabs(kMarkerSpeedPercent[n] / 100.0f - v.rate) < std::fabs(kMarkerSpeedPercent[at] / 100.0f - v.rate))
                        at = n;
                at = std::clamp(at + (step > 0 ? 1 : -1), 0, count - 1);
                v.rate = static_cast<float>(kMarkerSpeedPercent[at]) / 100.0f;
            });
        };
        speed.help = TR("shake.speed.help", "The shake's clock, 5 % to 200 %.");
        rows.push_back(speed);
        toggle(TR("shake.real_time", "Real Time"), (s.flags & ShakeFlagRealTime) != 0,
            [](MarkerShake& v) { v.flags = static_cast<std::uint8_t>(v.flags ^ ShakeFlagRealTime); },
            TR("shake.real_time.help", "On: the shake keeps real time in this marker's slow motion. Off: it slows down with the clip."));
        toggle(TR("shake.fade_out", "Fade Out"), (s.flags & ShakeFlagFadeOut) != 0,
            [](MarkerShake& v) { v.flags = static_cast<std::uint8_t>(v.flags ^ ShakeFlagFadeOut); },
            TR("shake.fade_out.help", "With Cut: the shake fades out over the last quarter second before the next marker."));
        toggle(TR("shake.restart", "Restart Here"), (s.flags & ShakeFlagRestart) != 0,
            [](MarkerShake& v) {
                v.flags = static_cast<std::uint8_t>(v.flags ^ ShakeFlagRestart);
                v.startCycles = 0.0;
                v.startFadeMs = 0.0;
            },
            TR("shake.restart.help", "The pattern and its fade in start again at this marker (otherwise the shake goes on from the marker before)."));
        for (MenuRow& row : rows)
            row.enabled = s.on != 0;
        return rows;
    }

    // ---------------- The Spline tuning ----------------
    // A marker's own fields (the rest from the Spline Defaults) and the defaults
    // themselves. Numbers on the arrows (0.05 a step, PageUp and PageDown five),
    // Enter types them; a marker's row goes left past 0 to the default again.
    // Shown values in brackets come from the defaults.
    SplineTuning MarkerSpline(const CameraKeyframe& k)
    {
        for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
            if (g_snapshot.cameraKeyframes[n].id == k.id)
                return g_pendingSpline.Base(k.id, g_snapshot.cameraKeyframeSpline[n], GetTickCount64());
        return DefaultSplineTuning();
    }

    SplineTuning SplineDefaultsShown()
    {
        return g_pendingSplineDefaults.Base(1, g_snapshot.splineDefaults, GetTickCount64());
    }

    void SendMarkerSpline(const CameraKeyframe& k, const std::function<void(SplineTuning&)>& edit)
    {
        SplineTuning t = MarkerSpline(k);
        edit(t);
        Command command{};
        command.keyframeId = k.id;
        const ULONGLONG now = GetTickCount64();
        command.options = g_pendingOptions.Base(k.id, OptionsOf(k), now);
        command.options.mask = CameraOptionSpline;
        command.spline = t;
        if (Send(CommandType::SetCameraKeyframeOptions, &command))
            g_pendingSpline.Sent(k.id, t, now);
    }

    // ---------------- Camera attach, bones and near clip ----------------
    // The marker's bones, attach pose numbers, near clip and DOF numbers.
    CameraMarkerExtra MarkerExtra(const CameraKeyframe& k)
    {
        for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount && n < kMaxCameraKeyframesInSnapshot; ++n)
            if (g_snapshot.cameraKeyframes[n].id == k.id)
                return g_pendingExtra.Base(k.id, g_snapshot.cameraKeyframeExtra[n], GetTickCount64());
        return CameraMarkerExtra{};
    }

    void SendMarkerExtra(const CameraKeyframe& k, std::uint32_t fields, const std::function<void(CameraMarkerExtra&)>& edit)
    {
        CameraMarkerExtra e = MarkerExtra(k);
        edit(e);
        e.fields = fields;
        Command command{};
        command.keyframeId = k.id;
        const ULONGLONG now = GetTickCount64();
        command.options = g_pendingOptions.Base(k.id, OptionsOf(k), now);
        command.options.mask = CameraOptionExtra;
        command.extra = e;
        if (Send(CommandType::SetCameraKeyframeOptions, &command))
        {
            e.fields = 0;
            g_pendingExtra.Sent(k.id, e, now);
        }
    }

    int TargetSlot(std::uint32_t id)
    {
        for (std::uint32_t n = 0; n < g_snapshot.targetCount && n < kMaxTargetsInSnapshot; ++n)
            if (g_snapshot.targets[n].id == id)
                return static_cast<int>(n);
        return -1;
    }

    std::string BoneLabel(std::uint32_t ref)
    {
        namespace bones = replay::camera::bones;
        if (ref == 0)
            return TR("bone.root", "Root");
        if (bones::ModeOf(ref) == bones::kModeNamed && bones::ValueOf(ref) < bones::kNamedCount)
            return TR(bones::kNamed[bones::ValueOf(ref)].key, bones::kNamed[bones::ValueOf(ref)].label);
        return TRF("bone.index", "Bone {index}", {{"index", std::to_string(bones::ValueOf(ref))}});
    }

    // The Attach Bone or Look At Bone row of a target.
    void AddBoneRow(std::vector<MenuRow>& rows, const char* label, const CameraKeyframe& k, std::uint32_t target, bool attach)
    {
        namespace bones = replay::camera::bones;
        const int slot = TargetSlot(target);
        const std::uint64_t mask = slot >= 0 ? (static_cast<std::uint64_t>(g_snapshot.targetBoneMaskHigh[slot]) << 32) | g_snapshot.targetBoneMask[slot] : 0;
        const std::uint32_t count = slot >= 0 ? g_snapshot.targetBoneCount[slot] : 0;
        const CameraMarkerExtra e = MarkerExtra(k);
        const std::uint32_t ref = attach ? e.attachBone : e.lookAtBone;
        const std::uint32_t field = attach ? CameraExtraAttachBone : CameraExtraLookAtBone;
        MenuRow row{label, BoneLabel(ref), mask != 0 || count != 0 || ref != 0, 0};
        row.step = [k, attach, field, mask, count](int step) {
            SendMarkerExtra(k, field, [&](CameraMarkerExtra& x) {
                std::uint32_t& r = attach ? x.attachBone : x.lookAtBone;
                r = bones::Step(r, step, mask, count);
            });
        };
        if (count)
        {
            row.typed = [k, attach, field, count](double typedValue) {
                const long index = std::lround(typedValue);
                if (index < 0 || static_cast<std::uint32_t>(index) >= count)
                    return;
                SendMarkerExtra(k, field, [&](CameraMarkerExtra& x) {
                    (attach ? x.attachBone : x.lookAtBone) = bones::Index(static_cast<std::uint32_t>(index));
                });
            };
            row.typeStart = bones::ModeOf(ref) == bones::kModeIndex ? FormatValue("%.0f", bones::ValueOf(ref)) : "0";
            row.typeMin = 0.0f;
            row.typeMax = static_cast<float>(count - 1);
        }
        const bones::Family family = slot >= 0 ? static_cast<bones::Family>(g_snapshot.targetFamily[slot]) : bones::Family::None;
        const char* familyText = family == bones::Family::Human ? TR("bone.family.human", "human")
            : family == bones::Family::Horse ? TR("bone.family.horse", "horse")
            : family == bones::Family::Animal ? TR("bone.family.animal", "animal")
            : family == bones::Family::Weapon ? TR("bone.family.weapon", "weapon")
            : family == bones::Family::Vehicle ? TR("bone.family.vehicle", "vehicle") : TR("bone.family.none", "none");
        row.help = std::string(attach ? TR("bone.attach.help", "The camera rides this bone (Root: the whole entity). ") : TR("bone.look.help", "The camera looks at this bone. ")) +
            TRF("bone.list.help", "The named bones of the {family} come first, then every bone of its skeleton by index", {{"family", familyText}}) +
            (count ? replay::keys::Format(TR("bone.count.help", " ({count} bones; {key_page_up}/{key_page_down}: five at a time; {key_select} types an index)."),
                         Keys(), {{"count", std::to_string(count)}})
                   : std::string(TR("bone.no_skeleton.help", " (skeleton not recorded here: named bones only).")));
        rows.push_back(row);
    }

    // A number of the attach pose (metres or degrees in the attach frame).
    void AddPoseRow(std::vector<MenuRow>& rows, const char* label, const CameraKeyframe& k, bool rotation, int axis, const char* help)
    {
        const CameraMarkerExtra e = MarkerExtra(k);
        const Vec3& v = rotation ? e.relRotation : e.relPosition;
        const float value = axis == 0 ? v.x : axis == 1 ? v.y : v.z;
        const float delta = rotation ? 1.0f : 0.05f;
        MenuRow row{label, FormatValue(rotation ? "%.1f deg" : "%.2f m", value), true, 0};
        const std::uint32_t field = rotation ? CameraExtraRelRotation : CameraExtraRelPosition;
        row.step = [k, rotation, axis, delta, field](int step) {
            SendMarkerExtra(k, field, [&](CameraMarkerExtra& x) {
                Vec3& w = rotation ? x.relRotation : x.relPosition;
                float& c = axis == 0 ? w.x : axis == 1 ? w.y : w.z;
                c = std::round((c + static_cast<float>(step) * delta) * 100.0f) / 100.0f;
            });
        };
        row.typed = [k, rotation, axis, field](double typedValue) {
            SendMarkerExtra(k, field, [&](CameraMarkerExtra& x) {
                Vec3& w = rotation ? x.relRotation : x.relPosition;
                (axis == 0 ? w.x : axis == 1 ? w.y : w.z) = static_cast<float>(typedValue);
            });
        };
        row.typeStart = FormatValue("%.2f", value);
        row.typeMin = rotation ? -180.0f : -100.0f;
        row.typeMax = rotation ? 180.0f : 100.0f;
        row.help = std::string(help) +
            KeyFill(rotation ? TR("pose.rotation.step", " 1 degree a step ({key_page_up}/{key_page_down}: five).")
                             : TR("pose.position.step", " 5 cm a step ({key_page_up}/{key_page_down}: 25 cm).")) +
            KeyFill(TR("pose.types", " {key_select} types it."));
        rows.push_back(row);
    }

    // The engine's lens for a FOV (12 mm over tan(fov / 2)) times a focal length
    // multiplier (2.0 to 2.5 by intensity): camera::DofLensMm.
    float AutoDofLens(float fov, float intensity)
    {
        const float f = std::clamp(fov, 5.0f, 130.0f);
        const float base = 12.0f / std::tan(f * 0.5f * 3.14159265f / 180.0f);
        return std::clamp(base * (2.0f + 0.5f * std::clamp(intensity, 0.0f, 100.0f) / 100.0f), kDofLensMin, kDofLensMax);
    }

    float IntensityFStop(float intensity)
    {
        return 22.0f * std::pow(0.5f / 22.0f, std::clamp(intensity, 0.0f, 100.0f) / 100.0f);
    }

    void SendSplineDefaults(const std::function<void(SplineTuning&)>& edit)
    {
        SplineTuning t = SplineDefaultsShown();
        edit(t);
        t.overrides = 0;
        Command command{};
        command.spline = t;
        if (Send(CommandType::SetSplineDefaults, &command))
            g_pendingSplineDefaults.Sent(1, t, GetTickCount64());
    }

    const NameText kSplinePacingNames[] = {{TR_NOOP("spline.profile.natural", "Natural")}, {TR_NOOP("spline.profile.continuous", "Continuous")},
        {TR_NOOP("spline.profile.per_segment", "Per Segment")}};

    std::string SplineNumber(float value)
    {
        return FormatValue("%.2f", value);
    }

    // The value rows of a tuning. `own`: the tuning shown (a marker's or the
    // defaults); `defaults`: what a marker's field falls back to; `perMarker`:
    // the rows have the default state; `apply`: sends a change.
    void AddSplineRows(std::vector<MenuRow>& rows, const SplineTuning& own, const SplineTuning& defaults, bool perMarker,
        const std::function<void(const std::function<void(SplineTuning&)>&)>& apply)
    {
        const auto overridden = [&](std::uint8_t bit) { return !perMarker || (own.overrides & bit) != 0; };
        const auto bracket = [](const std::string& text, bool mine) { return mine ? text : "(" + text + ")"; };
        const SplinePacing pacing = static_cast<SplinePacing>(overridden(SplineOverridePacing) ? own.pacing : defaults.pacing);

        // Speed Profile: (default), Natural, Continuous, Per Segment.
        {
            const bool mine = overridden(SplineOverridePacing);
            const int shownPacing = std::min<int>(mine ? own.pacing : defaults.pacing, kSplinePacingCount - 1);
            MenuRow row{TR("spline.profile", "Speed Profile"), bracket(NameOf(kSplinePacingNames, shownPacing), mine), true, 0};
            row.step = [apply, perMarker](int step) {
                apply([step, perMarker](SplineTuning& t) {
                    const int states = perMarker ? kSplinePacingCount + 1 : kSplinePacingCount;
                    int at = perMarker ? ((t.overrides & SplineOverridePacing) ? t.pacing + 1 : 0) : t.pacing;
                    at = (at + (step > 0 ? 1 : states - 1)) % states;
                    if (perMarker && at == 0)
                        t.overrides = static_cast<std::uint8_t>(t.overrides & ~SplineOverridePacing);
                    else
                    {
                        t.pacing = static_cast<std::uint8_t>(perMarker ? at - 1 : at);
                        t.overrides = static_cast<std::uint8_t>(t.overrides | SplineOverridePacing);
                    }
                });
            };
            row.help = TR("spline.profile.help", "Natural: the curve over the marker times (it slows into tight bends). Continuous: one speed through the whole "
                       "move, every marker reached on time. Per Segment: an even speed inside each segment.");
            if (perMarker)
                row.help += TR("spline.brackets", " In brackets: the Spline Defaults.");
            rows.push_back(row);
        }

        const auto number = [&](const char* label, float SplineTuning::*field, std::uint8_t bit, std::string help) {
            const bool mine = overridden(bit);
            const float value = mine ? own.*field : defaults.*field;
            MenuRow row{label, bracket(SplineNumber(value), mine), true, 0};
            row.step = [apply, field, bit, perMarker](int step) {
                apply([&](SplineTuning& t) {
                    const bool set = !perMarker || (t.overrides & bit) != 0;
                    if (!set)
                    {
                        // From the default: the first step starts at its value.
                        t.overrides = static_cast<std::uint8_t>(t.overrides | bit);
                        t.*field = std::clamp(SplineDefaultsShown().*field + static_cast<float>(step) * 0.05f, 0.0f, 1.0f);
                        return;
                    }
                    if (perMarker && step < 0 && t.*field <= 0.0001f)
                    {
                        t.overrides = static_cast<std::uint8_t>(t.overrides & ~bit); // left past 0: the default again
                        return;
                    }
                    t.*field = std::clamp(std::round((t.*field + static_cast<float>(step) * 0.05f) * 100.0f) / 100.0f, 0.0f, 1.0f);
                });
            };
            row.typed = [apply, field, bit](double typedValue) {
                apply([&](SplineTuning& t) {
                    t.*field = std::clamp(static_cast<float>(typedValue), 0.0f, 1.0f);
                    t.overrides = static_cast<std::uint8_t>(t.overrides | bit);
                });
            };
            row.typeStart = SplineNumber(value);
            row.typeMin = 0.0f;
            row.typeMax = 1.0f;
            if (perMarker)
                help += TR("spline.left_past_zero", " Left past 0: back to the default.");
            row.help = help + KeyFill(TR("spline.types", " {key_select} types a number."));
            rows.push_back(row);
        };

        {
            const float alpha = overridden(SplineOverrideAlpha) ? own.alpha : defaults.alpha;
            std::string help = TR("spline.curve_shape.help", "The Catmull-Rom spacing: 0 Uniform (wide turns), 0.5 Centripetal, 1 Chordal (holds short legs tight).");
            if (pacing == SplinePacing::Natural)
                help += TR("spline.curve_shape.natural", " No effect with the Natural profile (a curve over the marker times has no spacing to choose).");
            else if (alpha < 0.001f)
                help += TR("spline.curve_shape.uniform", " Now: Uniform.");
            else if (std::fabs(alpha - 0.5f) < 0.001f)
                help += TR("spline.curve_shape.centripetal", " Now: Centripetal.");
            else if (alpha > 0.999f)
                help += TR("spline.curve_shape.chordal", " Now: Chordal.");
            number(TR("spline.curve_shape", "Curve Shape"), &SplineTuning::alpha, SplineOverrideAlpha, help);
        }
        number(TR("spline.ease_in", "Ease In"), &SplineTuning::easeIn, SplineOverrideEaseIn,
            TR("spline.ease_in.help", "The share of the segment spent speeding up from rest (it still arrives on time). Position, rotation, zoom and "
            "framing ease together."));
        number(TR("spline.ease_out", "Ease Out"), &SplineTuning::easeOut, SplineOverrideEaseOut,
            TR("spline.ease_out.help", "The share of the segment spent slowing down to rest at the next marker. Ease In plus Ease Out over 1 are scaled down."));
        number(TR("spline.weight", "Camera Weight"), &SplineTuning::weight, SplineOverrideWeight,
            TR("spline.weight.help", "Pulls the curve toward a B-spline: the camera cuts inside the corners like it has mass, never past a marker. "
            "Only while playing and exporting; paused, the camera sits on the markers."));

        const auto channel = [&](const char* label, std::uint8_t channelBit, std::uint8_t bit, const char* help) {
            const bool mine = overridden(bit);
            const bool on = ((mine ? own.channels : defaults.channels) & channelBit) != 0;
            MenuRow row{label, bracket(on ? TR("common.on_cap", "On") : TR("common.off_cap", "Off"), mine), true, 0};
            row.step = [apply, channelBit, bit, perMarker](int step) {
                apply([&](SplineTuning& t) {
                    // A marker: (default), On, Off; the defaults: On, Off.
                    const int states = perMarker ? 3 : 2;
                    const bool set = !perMarker || (t.overrides & bit) != 0;
                    const bool isOn = (t.channels & channelBit) != 0;
                    int at = perMarker ? (set ? (isOn ? 1 : 2) : 0) : (isOn ? 0 : 1);
                    at = (at + (step > 0 ? 1 : states - 1)) % states;
                    const bool wantOn = perMarker ? at == 1 : at == 0;
                    if (perMarker && at == 0)
                    {
                        t.overrides = static_cast<std::uint8_t>(t.overrides & ~bit);
                        return;
                    }
                    t.channels = static_cast<std::uint8_t>(wantOn ? (t.channels | channelBit) : (t.channels & ~channelBit));
                    t.overrides = static_cast<std::uint8_t>(t.overrides | bit);
                });
            };
            row.help = help;
            rows.push_back(row);
        };
        channel(TR("spline.position", "Spline Position"), SplineChannelPosition, SplineOverridePosition,
            TR("spline.position.help", "Off: the camera's position goes straight between the markers (with the ease)."));
        channel(TR("spline.rotation", "Spline Rotation"), SplineChannelRotation, SplineOverrideRotation,
            TR("spline.rotation.help", "Off: the rotation turns straight between the markers (with the ease)."));
        channel(TR("spline.zoom", "Spline Zoom"), SplineChannelFov, SplineOverrideFov,
            TR("spline.zoom.help", "Off: the field of view changes straight. On: a curve that never passes its target and comes back."));
    }
}

    std::vector<MenuRow> BuildSplineDefaultRows()
    {
        std::vector<MenuRow> rows;
        const SplineTuning d = SplineDefaultsShown();
        AddSplineRows(rows, d, d, false, [](const std::function<void(SplineTuning&)>& edit) { SendSplineDefaults(edit); });
        MenuRow reset{TR("spline.reset_defaults", "Reset Defaults"), "", true, 2};
        reset.enter = [] { SendSplineDefaults([](SplineTuning& t) { t = DefaultSplineTuning(); }); };
        reset.help = TR("spline.reset_defaults.help", "Natural, Curve Shape 1.00, no ease, no weight, every channel on. Saved in RedemptionEditor.ini.");
        rows.push_back(reset);
        return rows;
    }

    std::vector<MenuRow> BuildSplineRows(const CameraKeyframe& k)
    {
        std::vector<MenuRow> rows;
        const SplineTuning own = MarkerSpline(k);
        AddSplineRows(rows, own, SplineDefaultsShown(), true,
            [k](const std::function<void(SplineTuning&)>& edit) { SendMarkerSpline(k, edit); });
        MenuRow reset{TR("spline.use_defaults", "Use Defaults"), "", own.overrides != 0, 2};
        reset.enter = [k] { SendMarkerSpline(k, [](SplineTuning& t) { t.overrides = 0; }); };
        reset.help = TR("spline.use_defaults.help", "Every row of this marker back to the Spline Defaults.");
        rows.push_back(reset);
        MenuRow defaults{TR("spline.defaults", "Spline Defaults"), ">", true, 1};
        defaults.enter = [count = rows.size()] {
            g_splineDefaultsReturn = static_cast<int>(count);
            g_markerMenu = MarkerMenu::SplineDefaults;
            g_menuFocus = 0;
        };
        defaults.help = TR("spline.defaults.marker_help", "The values every marker uses unless it sets its own (saved in RedemptionEditor.ini).");
        rows.push_back(defaults);
        return rows;
    }

    // A typed number over the focused row (the Spline rows): Enter sends it, Esc
    // leaves the row as it was.
    void StartRowTyping(const MenuRow& row)
    {
        g_rowTyping.on = true;
        g_rowTyping.label = row.label;
        g_rowTyping.focus = true;
        g_rowTyping.error = "";
        std::snprintf(g_rowTyping.text, sizeof(g_rowTyping.text), "%s", row.typeStart.c_str());
    }

    void DrawRowTypeBox(ImDrawList* draw, float y, const MenuRow& row)
    {
        UI_PROBE_SCOPE("RowTypeBox");
        g_fxTypingFrame = true;
        const float x = 40.0f + 300.0f * 0.40f;
        const float width = 300.0f * 0.60f - 6.0f;
        draw->AddRectFilled(P(x - 4.0f, y + 2.0f), P(x + width + 2.0f, y + 32.0f), IM_COL32(10, 9, 8, 235));
        ImGui::SetCursorScreenPos(P(x, y + 4.0f));
        ImGui::PushItemWidth(S(width));
        if (g_rowTyping.focus)
        {
            ImGui::SetKeyboardFocusHere();
            g_rowTyping.focus = false;
        }
        const bool entered = ImGui::InputText("##rowtype", g_rowTyping.text, sizeof(g_rowTyping.text),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CharsDecimal, nullptr,
            nullptr) || MissedTap(VK_RETURN);
        ImGui::PopItemWidth();
        if (g_rowTyping.error[0])
            TextFit(draw, kEditX, y + 36.0f, 14.0f, kGold, g_rowTyping.error, kEditW);
        if (entered)
        {
            std::string text(g_rowTyping.text);
            std::replace(text.begin(), text.end(), ',', '.');
            char* end = nullptr;
            const double number = std::strtod(text.c_str(), &end);
            if (end == text.c_str() || *end || !std::isfinite(number) || number < row.typeMin || number > row.typeMax)
            {
                static std::string why;
                why = TRF("typing.range", "A number from {min} to {max}.", {{"min", replay::i18n::Printf("%g", row.typeMin)}, {"max", replay::i18n::Printf("%g", row.typeMax)}});
                g_rowTyping.error = why.c_str();
                PlayUi(UiSound::Error);
                return;
            }
            if (row.typed)
                row.typed(number);
            PlayUi(UiSound::Select);
            g_rowTyping.on = false;
        }
        else if (!g_helpOpen && !g_helpSwallow && KeyPressed(ImGuiKey_Escape, false))
        {
            PlayUi(UiSound::Back);
            g_rowTyping.on = false;
        }
    }

namespace
{

    // A photo mode filter's name ("Game 01", "Modern 01", "Vintage 01").
    std::string FilterName(std::uint32_t index)
    {
        const std::string english = kMarkerFilters[index];
        const std::size_t space = english.rfind(' ');
        if (index == 0 || space == std::string::npos)
            return TR("common.none", "None");
        const std::string number = english.substr(space + 1);
        const std::string family = english.substr(0, space);
        if (family == "Game")
            return TRF("filter.game", "Game {n}", {{"n", number}});
        if (family == "Modern")
            return TRF("filter.modern", "Modern {n}", {{"n", number}});
        return TRF("filter.vintage", "Vintage {n}", {{"n", number}});
    }
}

    std::vector<MenuRow> BuildMarkerRows(const CameraKeyframe& k, int index)
    {
        std::vector<MenuRow> rows;
        const int count = static_cast<int>(g_snapshot.cameraKeyframeCount);
        const bool isLast = index == count - 1 && count > 1;
        const CameraKeyframe* previous = index > 0 ? &g_snapshot.cameraKeyframes[index - 1] : nullptr;
        const bool free = k.cameraMode == CameraMode::Free || k.cameraMode == CameraMode::Target || k.cameraMode == CameraMode::Follow;
        const bool anchorOnly = k.markerType == MarkerType::Anchor;
        if (g_markerMenu == MarkerMenu::Root)
        {
            // LockCutsceneCamera: a marker inside a recorded cutscene is the recorded
            // camera; its options are greyed.
            bool inCutscene = false;
            for (std::uint32_t n = 0; n < g_snapshot.spanCount && n < kMaxSpansInSnapshot; ++n)
                inCutscene = inCutscene || (g_snapshot.spans[n].kind == 0 && k.timeMs >= g_snapshot.spans[n].startMs &&
                    k.timeMs < g_snapshot.spans[n].endMs);
            const bool locked = Flag(SnapshotLockCutsceneCamera) && inCutscene;
            MenuRow camera{TR("marker.camera_options", "Camera Options"), locked ? TR("marker.camera_options.game", "Game") : ">", !anchorOnly && !locked, 1};
            camera.icon = ui_art::Art::ToolCamera;
            camera.enter = [] { g_markerMenu = MarkerMenu::Camera; g_menuFocus = 0; };
            camera.help = locked ? TR("marker.camera_options.locked", "Unavailable during a cutscene (recorded camera)")
                                 : TR("marker.camera_options.help", "The camera for this marker: type, targets, blend, shake, field of view.");
            rows.push_back(camera);
            MenuRow dof{TR("marker.dof", "Depth of Field"), ">", !anchorOnly && !isLast, 1};
            dof.icon = ui_art::Art::ToolDof;
            dof.enter = [] { g_markerMenu = MarkerMenu::Dof; g_menuFocus = 0; };
            dof.help = isLast ? TR("marker.dof.last", "The last marker uses the depth of field of the one before it.") : TR("marker.dof.help", "Focus and background blur from this marker on.");
            rows.push_back(dof);
            MenuRow effects{TR("marker.effects", "Effects"), ">", !anchorOnly, 1};
            effects.icon = ui_art::Art::ToolEffects;
            effects.enter = [] { g_markerMenu = MarkerMenu::Effects; g_menuFocus = 0; };
            effects.help = TR("marker.effects.help", "The game's photo mode filters and fades from this marker on.");
            rows.push_back(effects);
            // The marker's shake (an Anchor the shake migration added holds one too).
            const bool holdsShake = !anchorOnly || k.markerOrigin == 1;
            MenuRow shake{TRF("marker.shake", "Shake ({summary})", {{"summary", ShakeSummary(MarkerShakeOf(k))}}), ">", holdsShake, 1};
            shake.icon = ui_art::Art::ToolShake;
            shake.enter = [] { g_menuReturnFocus = g_menuFocus; g_markerMenu = MarkerMenu::Shake; g_menuFocus = 0; };
            shake.help = k.markerOrigin == 1 ? TR("marker.shake.migrated", "Added when the clip's old shake events were moved into the markers: it holds a "
                                               "change of the shake, the camera passes it by.")
                                             : TR("marker.shake.help", "The camera shake from this marker on (profile, intensity, frequency, transition).");
            rows.push_back(shake);
            // The marker's numbers and the tools for the move it belongs to.
            const bool posable = free && !anchorOnly && !isLast;
            MenuRow position{TR("marker.position", "Position"), k.attachTarget ? TR("marker.position.attached", "Attached") : ">", posable && !k.attachTarget, 1};
            position.enter = [] { g_menuReturnFocus = g_menuFocus; g_markerMenu = MarkerMenu::Position; g_menuFocus = 0; };
            position.help = k.attachTarget ? TR("marker.position.attached_help", "An attached marker is framed in Edit Camera (its position rides the target).")
                                           : TR("marker.position.help", "The marker's position and rotation as numbers.");
            rows.push_back(position);
            MenuRow tools{TR("marker.tools", "Move Tools"), ">", !anchorOnly, 1};
            tools.enter = [] { g_menuReturnFocus = g_menuFocus; g_markerMenu = MarkerMenu::Tools; g_menuFocus = 0; };
            tools.help = TR("marker.tools.help", "Duplicate, even out the speed, stretch the move, lock it to a character.");
            rows.push_back(tools);
            MenuRow speed{TR("marker.speed", "Speed"), FormatValue("%.0f%%", kMarkerSpeedPercent[std::min<std::uint32_t>(k.speedIndex, kMarkerSpeedCount - 1)]), !isLast, 0};
            speed.step = [k](int step) {
                SendOptions(k, CameraOptionSpeed, [&](CameraKeyframeOptions& o) {
                    o.speedIndex = static_cast<std::uint8_t>(std::clamp(static_cast<int>(o.speedIndex) + (step > 0 ? 1 : -1), 0, static_cast<int>(kMarkerSpeedCount) - 1));
                });
            };
            speed.help = TR("marker.speed.help", "Slow motion or fast from this marker to the next one.");
            rows.push_back(speed);
            char at[24]{};
            FormatClock(k.timeMs, at, sizeof(at), true);
            MenuRow time{TR("marker.time", "Time"), at, true, 0};
            time.step = [k](int step) {
                Command command{};
                command.keyframeId = k.id;
                const ULONGLONG now = GetTickCount64();
                const std::uint64_t from = g_pendingMoveTime.Base(k.id, k.timeMs, now);
                const std::int64_t to = static_cast<std::int64_t>(from) + step * 33;
                command.timeMs = static_cast<std::uint64_t>(std::max<std::int64_t>(0, to));
                if (Send(CommandType::MoveCameraKeyframe, &command)) // the core seeks to the time taken
                    g_pendingMoveTime.Sent(k.id, command.timeMs, now);
            };
            time.help = KeyFill(TR("marker.time.help", "Move the marker one frame ({key_page_up}/{key_page_down}: ten)."));
            rows.push_back(time);
            const bool isFirst = index == 0 && count > 1;
            if (isFirst || isLast)
            {
                // The clip's ends: a fade instead of the anchor.
                MenuRow fade{TR("shake.transition", "Transition"), k.transition ? (isFirst ? TR("marker.fade_in", "Fade In") : TR("marker.fade_out", "Fade Out")) : TR("common.none", "None"), true, 0};
                fade.step = [k](int) {
                    SendOptions(k, CameraOptionTransition, [&](CameraKeyframeOptions& o) { o.transition = o.transition ? 0 : 1; });
                };
                fade.help = isFirst ? TR("marker.fade_in.help", "Fade from black at the start of the clip.") : TR("marker.fade_out.help", "Fade to black at the end of the clip.");
                rows.push_back(fade);
                if (k.transition)
                {
                    MenuRow length{TR("marker.fade_length", "Fade Length"), FormatValue("%.1f s", k.transitionMs / 1000.0), true, 0};
                    length.step = [k](int step) {
                        SendOptions(k, CameraOptionTransitionMs, [&](CameraKeyframeOptions& o) {
                            o.transitionMs = static_cast<std::uint16_t>(std::clamp(static_cast<int>(o.transitionMs) + step * 100, 100, 10000));
                        });
                    };
                    length.help = TR("marker.fade_length.help", "0.1 s a step.");
                    rows.push_back(length);
                }
            }
            else if (k.markerType != MarkerType::Edit)
            {
                // Anchor is hidden until it does something (anchors sync to a project's
                // music, which this editor has not). A marker already an anchor still shows
                // the row, so it can be turned Off; the row goes once it is.
                MenuRow anchor{TR("marker.anchor", "Anchor"), NameOf(kAnchorNames, std::clamp(static_cast<int>(k.markerType), 0, 2)), true, 0};
                anchor.step = [k](int step) {
                    SendOptions(k, CameraOptionMarkerType, [&](CameraKeyframeOptions& o) { o.markerType = Cycle(o.markerType, step, 3); });
                };
                anchor.help = TR("marker.anchor.help", "Exclusive: a mark on the timeline only, the camera ignores it.");
                rows.push_back(anchor);
            }
            auto action = [&](const char* label, std::function<void()> run, const char* help) {
                MenuRow row{label, "", true, 2};
                row.enter = std::move(run);
                row.help = help;
                rows.push_back(row);
            };
            action(TR("marker.copy", "Copy"), [k] { Command c{}; c.keyframeId = k.id; Send(CommandType::CopyCameraKeyframe, &c); }, KeyFill(TR("marker.copy.help", "Copy this marker's settings ({key_copy}).")));
            action(TR("marker.paste", "Paste"), [k] { Command c{}; c.keyframeId = k.id; c.enabled = 0; ForgetPendingMarkerValues(); Send(CommandType::PasteCameraKeyframe, &c); }, KeyFill(TR("marker.paste.help", "Paste the copied settings here ({key_paste}).")));
            rows.back().enabled = Flag(SnapshotMarkerCopied); // nothing copied, nothing to paste
            action(TR("marker.reset", "Reset"), [k] { Command c{}; c.keyframeId = k.id; ForgetPendingMarkerValues(); Send(CommandType::ResetCameraKeyframe, &c); }, TR("marker.reset.help", "Back to the default settings."));
            action(TR("marker.paste_effects", "Paste Effects"), [k] { Command c{}; c.keyframeId = k.id; c.enabled = 1; ForgetPendingMarkerValues(); Send(CommandType::PasteCameraKeyframe, &c); }, TR("marker.paste_effects.help", "Paste only the copied effects."));
            rows.back().enabled = Flag(SnapshotMarkerCopied);
            const bool bound = isFirst || isLast;
            action(TR("marker.delete", "Delete"), [k] { DeleteMarker(k.id); },
                bound ? TR("marker.delete.bound", "The clip's start and end stay; move them instead.") : KeyFill(TR("marker.delete.help", "Delete this marker ({key_delete}).")));
            rows.back().enabled = !bound;
            return rows;
        }
        if (g_markerMenu == MarkerMenu::Camera)
        {
            // The last marker's type cannot be changed. It takes the camera of the
            // marker before it: a Free Camera when that one blends into it
            // (ApplyLastMarkerRule), otherwise that marker's camera plays up to the end
            // (Plan skips the last marker), so its type is shown. The core refuses the
            // type on the last marker too.
            const CameraKeyframe* before = nullptr;
            for (int n = index - 1; isLast && n >= 0 && !before; --n)
                if (g_snapshot.cameraKeyframes[n].markerType != MarkerType::Anchor)
                    before = &g_snapshot.cameraKeyframes[n];
            const bool blendedInto = before && before->blend != Blend::None && before->cameraMode == CameraMode::Free;
            const CameraMode shownMode = isLast && before && !blendedInto ? before->cameraMode : k.cameraMode;
            MenuRow type{TR("marker.camera", "Camera"), CameraName(shownMode), !isLast, 0};
            type.step = [k](int step) {
                SendOptions(k, CameraOptionMode, [&](CameraKeyframeOptions& o) {
                    int at = 6;
                    for (int n = 0; n < 7; ++n)
                        if (kCameraCycle[n] == o.cameraMode) // from the value last sent
                            at = n;
                    at = (at + (step > 0 ? 1 : -1) + 7) % 7;
                    o.cameraMode = kCameraCycle[at];
                });
            };
            type.help = isLast ? TR("marker.camera.last", "The last marker keeps the camera of the marker before it.")
                               : TR("marker.camera.help", "Game Camera plays the recorded view; the presets orbit the main character; Free Camera is yours to frame.");
            rows.push_back(type);
            {
                // The near clip plane.
                const float own = MarkerExtra(k).nearClip;
                MenuRow nearRow{TR("freecam.near_clip", "Near Clip"), own > 0.0f ? FormatValue("%.2f m", own) : TRF("marker.near_clip.default", "Default ({value} m)", {{"value", replay::i18n::Printf("%.2f", kNearClipDefault)}}), true, 0};
                nearRow.step = [k](int step) {
                    SendMarkerExtra(k, CameraExtraNearClip, [&](CameraMarkerExtra& x) {
                        const float base = x.nearClip > 0.0f ? x.nearClip : kNearClipDefault;
                        x.nearClip = std::clamp(std::round((base + static_cast<float>(step) * 0.01f) * 100.0f) / 100.0f, kNearClipMin, kNearClipMax);
                    });
                };
                nearRow.typed = [k](double typedValue) {
                    const float v = static_cast<float>(typedValue);
                    SendMarkerExtra(k, CameraExtraNearClip, [&](CameraMarkerExtra& x) {
                        x.nearClip = v <= 0.0f ? 0.0f : std::clamp(v, kNearClipMin, kNearClipMax);
                    });
                };
                nearRow.typeStart = FormatValue("%.2f", own > 0.0f ? own : kNearClipDefault);
                nearRow.typeMin = 0.0f;
                nearRow.typeMax = kNearClipMax;
                const CameraLiveInfo& live = g_snapshot.cameraLive;
                nearRow.help = std::string(KeyFill(TR("marker.near_clip.help", "How close the camera gets before things are cut, in metres (0.01 to 1). Lower "
                                                      "gets closer; under about 0.05 m far edges can flicker. {key_select} types it, 0 is the default."))) +
                    ((live.flags & CameraLiveNearRead) ? TRF("marker.near_clip.reads", " The camera reads {value} m now.", {{"value", replay::i18n::Printf("%.3f", live.nearRead)}}) : std::string());
                rows.push_back(nearRow);
            }
            if (free)
            {
                MenuRow look{TR("marker.look_at", "Look At Target"), TargetLabel(k.lookAtTarget), true, 0};
                look.step = [k](int step) {
                    SendOptions(k, CameraOptionLookAtTarget, [&](CameraKeyframeOptions& o) { o.lookAtTarget = StepTargetOrNone(o.lookAtTarget, step); });
                };
                look.help = TR("marker.look_at.help", "The camera turns to keep this target in frame (framing set in Edit Camera).");
                rows.push_back(look);
                if (k.lookAtTarget)
                    AddBoneRow(rows, TR("marker.look_at_bone", "Look At Bone"), k, k.lookAtTarget, false);
                MenuRow attach{TR("marker.attach", "Attach Target"), TargetLabel(k.attachTarget), true, 0};
                attach.step = [k](int step) {
                    SendOptions(k, CameraOptionAttachTarget, [&](CameraKeyframeOptions& o) { o.attachTarget = StepTargetOrNone(o.attachTarget, step); });
                };
                attach.help = TR("marker.attach.help", "The camera moves with this target.");
                rows.push_back(attach);
                if (k.attachTarget)
                {
                    MenuRow mount{TR("marker.mount", "Mount Type"), NameOf(kAttachNames, std::clamp(static_cast<int>(k.attachType), 0, 2)), true, 0};
                    mount.step = [k](int step) {
                        SendOptions(k, CameraOptionAttachType, [&](CameraKeyframeOptions& o) { o.attachType = Cycle(o.attachType, step, 3); });
                    };
                    mount.help = TR("marker.mount.help", "Rigid turns with the target in every axis, Default follows its heading, Flexible only its position.");
                    rows.push_back(mount);
                    // The bone and the attach pose as numbers.
                    AddBoneRow(rows, TR("marker.attach_bone", "Attach Bone"), k, k.attachTarget, true);
                    AddPoseRow(rows, TR("pose.offset_right", "Offset Right"), k, false, 0, TR("pose.offset_right.help", "The camera's place in the attach frame: + right of the bone or entity."));
                    AddPoseRow(rows, TR("pose.offset_forward", "Offset Forward"), k, false, 1, TR("pose.offset_forward.help", "+ in front of it."));
                    AddPoseRow(rows, TR("pose.offset_up", "Offset Up"), k, false, 2, TR("pose.offset_up.help", "+ above it."));
                    AddPoseRow(rows, TR("position.pitch", "Pitch"), k, true, 0, TR("pose.pitch.help", "The camera's turn in the attach frame: up and down."));
                    AddPoseRow(rows, TR("position.roll", "Roll"), k, true, 1, TR("pose.roll.help", "Tilt around the view."));
                    AddPoseRow(rows, TR("position.heading", "Heading"), k, true, 2, TR("pose.heading.help", "Left and right (a Look At target overrides the turn)."));
                }
            }
            if (free && !isLast)
            {
                MenuRow blend{TR("marker.blend", "Blend"), NameOf(kBlendNames, std::clamp(static_cast<int>(k.blend), 0, static_cast<int>(kBlendCount) - 1)), true, 0};
                blend.step = [k](int step) {
                    SendOptions(k, CameraOptionBlend, [&](CameraKeyframeOptions& o) { o.blend = Cycle(o.blend, step, kBlendCount); });
                };
                blend.help = TR("marker.blend.help", "How the camera goes to the next marker: a cut (the default), a straight line, smoothed, or Spline: one "
                             "curve through the markers that blend into each other, keeping its speed through each marker. Blends only "
                             "between free cameras.");
                rows.push_back(blend);
                if (k.blend == Blend::Spline)
                {
                    // Speed profile, curve shape, ease, weight, channels.
                    const SplineTuning own = MarkerSpline(k);
                    MenuRow spline{own.overrides ? TR("marker.spline_options.own", "Spline Options (own)") : TR("marker.spline_options", "Spline Options"), ">", true, 1};
                    spline.enter = [n = rows.size()] {
                        g_splineMenuReturn = static_cast<int>(n);
                        g_markerMenu = MarkerMenu::Spline;
                        g_menuFocus = 0;
                    };
                    spline.help = TR("marker.spline_options.help", "Speed profile, curve shape, ease in and out, camera weight and channels of the segment from this "
                                  "marker (each one its own or the Spline Defaults).");
                    rows.push_back(spline);
                }
            }
            // The chain ease is for Linear and Smooth; a Spline segment has its own Ease
            // In and Ease Out (Spline Options).
            const auto chainEase = [](Blend blend) { return blend == Blend::Linear || blend == Blend::Smooth; };
            const bool easeRow = (free && chainEase(k.blend)) || (previous && chainEase(previous->blend));
            if (easeRow)
            {
                MenuRow ease{TR("marker.blend_ease", "Blend Ease"), k.ease ? TR("common.on_cap", "On") : TR("common.off_cap", "Off"), true, 0};
                ease.step = [k](int) {
                    SendOptions(k, CameraOptionEase, [&](CameraKeyframeOptions& o) { o.ease = o.ease ? 0 : 1; });
                };
                ease.help = TR("marker.blend_ease.help", "On: a chain of blends starts slow and ends slow. Off: constant speed.");
                rows.push_back(ease);
            }
            // The shake has its own menu (Shake, at the root).
            if (k.cameraMode != CameraMode::Game)
            {
                char fov[32]{};
                std::snprintf(fov, sizeof(fov), "%.0f  (%.2fX)", k.camera.fov, kMarkerFovDefault / std::max(1.0f, k.camera.fov));
                MenuRow zoom{TR("marker.fov", "Field of View"), fov, true, 0};
                zoom.step = [k](int step) {
                    SendOptions(k, CameraOptionFov, [&](CameraKeyframeOptions& o) {
                        o.fov = std::clamp(std::round(o.fov) - static_cast<float>(step) * kMarkerFovStep, kMarkerFovMin, kMarkerFovMax);
                    });
                };
                zoom.help = KeyFill(TR("marker.fov.help", "Right zooms in ({key_page_up}/{key_page_down}: five degrees)."));
                rows.push_back(zoom);
            }
            if (IsPreset(k.cameraMode))
            {
                MenuRow distance{TR("marker.distance", "Distance"), FormatValue("%.2fx", 0.5 + k.presetDistanceScale), true, 0};
                distance.step = [k](int step) {
                    SendOptions(k, CameraOptionPresetDistance, [&](CameraKeyframeOptions& o) {
                        o.presetDistanceScale = std::clamp(o.presetDistanceScale + step * 0.05f, 0.0f, 1.0f);
                    });
                };
                distance.help = TR("marker.distance.help", "Nearer or farther from the character.");
                rows.push_back(distance);
            }
            if (IsPreset(k.cameraMode) && k.cameraMode != CameraMode::Overhead || (free && k.lookAtTarget))
            {
                MenuRow roll{TR("position.roll", "Roll"), FormatValue("%.0f", k.lookAtRoll), true, 0};
                roll.step = [k](int step) {
                    SendOptions(k, CameraOptionLookAtRoll, [&](CameraKeyframeOptions& o) { o.lookAtRoll = std::clamp(o.lookAtRoll + step, -90.0f, 90.0f); });
                };
                roll.help = TR("marker.roll.help", "Tilt the picture.");
                rows.push_back(roll);
            }
            if (free || IsPreset(k.cameraMode))
            {
                MenuRow editCamera{TR("marker.edit_camera", "Edit Camera"), "", true, 2};
                editCamera.enter = [k] {
                    Command c{};
                    c.keyframeId = k.id;
                    c.enabled = 1;
                    Send(CommandType::EditCameraKeyframe, &c);
                };
                editCamera.help = KeyFill(TR("marker.edit_camera.help", "Frame this marker's shot: {key_fly}, mouse, {key_roll_left}/{key_roll_right} roll, "
                                                                        "{key_page_up}/{key_page_down} zoom, {key_select} to keep, {key_cancel} to cancel."));
                rows.push_back(editCamera);
            }
            return rows;
        }
        if (g_markerMenu == MarkerMenu::Shake)
            return BuildShakeRows(k);
        if (g_markerMenu == MarkerMenu::ShakeAdvanced)
            return BuildShakeAdvancedRows(k);
        if (g_markerMenu == MarkerMenu::Position)
            return BuildPositionRows(k);
        if (g_markerMenu == MarkerMenu::Spline)
            return BuildSplineRows(k);
        if (g_markerMenu == MarkerMenu::SplineDefaults)
            return BuildSplineDefaultRows();
        if (g_markerMenu == MarkerMenu::Tools)
            return BuildToolRows(k);
        if (g_markerMenu == MarkerMenu::Effects)
        {
            MenuRow filter{TR("marker.filter", "Filter"), FilterName(std::min<std::uint32_t>(k.filter, kMarkerFilterCount - 1)), true, 0};
            filter.step = [k](int step) {
                SendOptions(k, CameraOptionFilter, [&](CameraKeyframeOptions& o) {
                    const int n = static_cast<int>(kMarkerFilterCount);
                    o.filter = static_cast<std::uint8_t>((static_cast<int>(o.filter) + step % n + n) % n);
                });
            };
            filter.help = KeyFill(TR("marker.filter.help", "The game's photo mode filters ({key_page_up}/{key_page_down}: five at a time)."));
            rows.push_back(filter);
            if (k.filter)
            {
                MenuRow intensity{TR("marker.filter_intensity", "Filter Intensity"), FormatValue("%.0f", k.filterIntensity), true, 0};
                intensity.step = [k](int step) {
                    SendOptions(k, CameraOptionFilterIntensity, [&](CameraKeyframeOptions& o) {
                        o.filterIntensity = static_cast<std::uint8_t>(std::clamp(static_cast<int>(o.filterIntensity) + step, 0, 100));
                    });
                };
                intensity.help = TR("marker.filter_intensity.help", "0 to 100.");
                rows.push_back(intensity);
            }
            // Exposure and contrast are not exposed yet.
            return rows;
        }
        // Depth of Field
        MenuRow mode{TR("marker.dof.mode", "DOF"), NameOf(kDofNames, std::clamp(static_cast<int>(k.dof), 0, 2)), true, 0};
        mode.step = [k](int step) { SendOptions(k, CameraOptionDof, [&](CameraKeyframeOptions& o) { o.dof = Cycle(o.dof, step, 3); }); };
        mode.help = TR("marker.dof.mode.help", "Default: the game's own. Custom: your focus and blur. None: everything sharp.");
        rows.push_back(mode);
        if (k.dof == DofMode::Custom)
        {
            MenuRow intensity{TR("shake.intensity", "Intensity"), FormatValue("%.0f", k.dofIntensity), true, 0};
            intensity.step = [k](int step) {
                SendOptions(k, CameraOptionDofIntensity, [&](CameraKeyframeOptions& o) { o.dofIntensity = std::clamp(o.dofIntensity + step, 0.0f, 100.0f); });
            };
            intensity.help = TR("marker.dof_intensity.help", "How strong the blur is (0 to 100): a shortcut that sets the f-stop and the lens "
                             "while F-Stop and Lens are on Auto.");
            rows.push_back(intensity);
            // The numbers the engine takes.
            const CameraMarkerExtra ex = MarkerExtra(k);
            const float autoFStop = IntensityFStop(k.dofIntensity);
            MenuRow fstop{TR("marker.fstop", "F-Stop"), ex.dofFStop > 0.0f ? FormatValue("f/%.1f", ex.dofFStop) : TRF("marker.fstop.auto", "Auto (f/{value})", {{"value", replay::i18n::Printf("%.1f", autoFStop)}}), true, 0};
            fstop.step = [k, autoFStop](int step) {
                SendMarkerExtra(k, CameraExtraDofFStop, [&](CameraMarkerExtra& x) {
                    const float base = x.dofFStop > 0.0f ? x.dofFStop : autoFStop;
                    x.dofFStop = std::clamp(base * std::pow(2.0f, static_cast<float>(-step) / 6.0f), kDofFStopMin, kDofFStopMax);
                });
            };
            fstop.typed = [k](double typedValue) {
                const float v = static_cast<float>(typedValue);
                SendMarkerExtra(k, CameraExtraDofFStop, [&](CameraMarkerExtra& x) {
                    x.dofFStop = v <= 0.0f ? 0.0f : std::clamp(v, kDofFStopMin, kDofFStopMax);
                });
            };
            fstop.typeStart = FormatValue("%.1f", ex.dofFStop > 0.0f ? ex.dofFStop : autoFStop);
            fstop.typeMin = 0.0f;
            fstop.typeMax = kDofFStopMax;
            fstop.help = KeyFill(TR("marker.fstop.help", "The aperture: lower is more blur (right opens it, a third of a stop a step). {key_select} types it, 0 is Auto."));
            rows.push_back(fstop);
            const float autoLens = AutoDofLens(k.camera.fov, k.dofIntensity);
            MenuRow lens{TR("marker.lens", "Lens"), ex.dofLensMm > 0.0f ? FormatValue("%.0f mm", ex.dofLensMm) : TRF("marker.lens.auto", "Auto ({value} mm)", {{"value", replay::i18n::Printf("%.0f", autoLens)}}), true, 0};
            lens.step = [k, autoLens](int step) {
                SendMarkerExtra(k, CameraExtraDofLens, [&](CameraMarkerExtra& x) {
                    const float base = x.dofLensMm > 0.0f ? x.dofLensMm : autoLens;
                    x.dofLensMm = std::clamp(std::round(base + static_cast<float>(step) * 5.0f), kDofLensMin, kDofLensMax);
                });
            };
            lens.typed = [k](double typedValue) {
                const float v = static_cast<float>(typedValue);
                SendMarkerExtra(k, CameraExtraDofLens, [&](CameraMarkerExtra& x) {
                    x.dofLensMm = v <= 0.0f ? 0.0f : std::clamp(v, kDofLensMin, kDofLensMax);
                });
            };
            lens.typeStart = FormatValue("%.0f", ex.dofLensMm > 0.0f ? ex.dofLensMm : autoLens);
            lens.typeMin = 0.0f;
            lens.typeMax = kDofLensMax;
            lens.help = KeyFill(TR("marker.lens.help", "The focal length the blur is computed with (35 mm film; it does not zoom: the FOV stays the camera's). Longer "
                        "is more blur. Auto: the FOV's lens times 2 to 2.5. 5 mm a step, {key_select} types it, 0 is Auto."));
            rows.push_back(lens);
            {
                const CameraLiveInfo& live = g_snapshot.cameraLive;
                const bool on = (live.flags & CameraLiveDofActive) != 0;
                std::string range = "-";
                if (on && live.inFocusNear > 0.0f)
                    range = live.inFocusFar > 0.0f ? TRF("marker.in_focus.range", "{near} to {far} m", {{"near", replay::i18n::Printf("%.1f", live.inFocusNear)},
                                                         {"far", replay::i18n::Printf("%.1f", live.inFocusFar)}})
                                                   : TRF("marker.in_focus.infinity", "{near} m to infinity", {{"near", replay::i18n::Printf("%.1f", live.inFocusNear)}});
                MenuRow sharp{TR("marker.in_focus", "In Focus"), range, true, 0};
                sharp.help = TR("marker.in_focus.help", "What stays sharp now (near to far), from the f-stop, the lens and the focus distance the camera got.");
                rows.push_back(sharp);
                std::string read = "-";
                if (on && (live.flags & CameraLiveDofRead))
                    read = FormatValue("f/%.1f", live.dofFStopRead) + FormatValue("  %.0f mm", live.dofLensRead) +
                        (live.focusRead > 0.0f ? FormatValue("  %.1f m", live.focusRead) : std::string(TR("marker.camera_reads.auto", "  auto")));
                MenuRow back{TR("marker.camera_reads", "Camera Reads"), read, true, 0};
                back.help = std::string(TR("marker.camera_reads.help", "What the game's camera holds, read back from it this frame.")) +
                    ((live.flags & CameraLiveFocusPaused) ? TR("marker.camera_reads.paused", " The game's autofocus kept moving the focus, so it is paused for this camera.") : "");
                rows.push_back(back);
            }
            MenuRow focus{TR("marker.focus", "Focus"), NameOf(kFocusNames, std::clamp(static_cast<int>(k.focus), 0, 2)), true, 0};
            focus.step = [k](int step) { SendOptions(k, CameraOptionFocus, [&](CameraKeyframeOptions& o) { o.focus = Cycle(o.focus, step, 3); }); };
            // The core's PlanFocus order (Look At, attach, the player on screen, the game's autofocus).
            focus.help = TR("marker.focus.help", "Auto: the Look At target, else the attach target, else the player on screen, else the game's autofocus. Manual: a distance. Target: a character.");
            rows.push_back(focus);
            if (k.focus == FocusMode::Manual)
            {
                MenuRow distance{TR("marker.focus_distance", "Focus Distance"), FormatValue("%.1f m", k.focusDistance), true, 0};
                distance.step = [k](int step) {
                    SendOptions(k, CameraOptionFocusDistance, [&](CameraKeyframeOptions& o) { o.focusDistance = std::clamp(o.focusDistance + step * 0.1f, 0.5f, 1000.0f); });
                };
                // Enter types the metres.
                distance.typed = [k](double typedValue) {
                    const float v = static_cast<float>(typedValue);
                    SendOptions(k, CameraOptionFocusDistance, [&](CameraKeyframeOptions& o) { o.focusDistance = std::clamp(v, 0.5f, 1000.0f); });
                };
                distance.typeStart = FormatValue("%.1f", k.focusDistance);
                distance.typeMin = 0.5f;
                distance.typeMax = 1000.0f;
                distance.help = KeyFill(TR("marker.focus_distance.help", "0.1 m a step ({key_page_up}/{key_page_down}: 0.5 m). {key_select} types it."));
                rows.push_back(distance);
            }
            if (k.focus == FocusMode::Target)
            {
                // No character chosen focuses the player (index 0), as the core does.
                MenuRow target{TR("marker.focus_target", "Focus Target"), k.focusTarget ? TargetLabel(k.focusTarget) : TR("marker.focus_target.player", "Player"), true, 0};
                target.step = [k](int step) {
                    SendOptions(k, CameraOptionFocusTarget, [&](CameraKeyframeOptions& o) { o.focusTarget = StepTargetOrNone(o.focusTarget, step); });
                };
                target.help = TR("marker.focus_target.help", "The character kept sharp.");
                rows.push_back(target);
            }
        }
        return rows;
    }

    // ---------------- Environment ----------------
    // The clip's clock, weather and clouds: "Recorded" (the clip's own, every
    // setting's first value) or an override the core applies at the playhead
    // (env_override.h), during playback and the export only.
    float EnvironmentPanelH()
    {
        return 26.0f + 4 * 38.0f;
    }

namespace
{

    void SendEnvironmentStep(EnvironmentField field, int step)
    {
        Command command{};
        command.keyframeId = static_cast<std::uint32_t>(field);
        command.direction = step < 0 ? -1 : 1;
        Send(CommandType::SetEnvironment, &command);
    }
}

    // The section title and its four rows from `y`; returns the y under them.
    float DrawEnvironmentRows(ImDrawList* draw, float y, bool playing)
    {
        UI_PROBE_SCOPE("Environment");
        namespace env = replay::envoverride;
        SectionTitle(draw, 40.0f, y + 4.0f, 300.0f, TR("env.title", "ENVIRONMENT"));
        y += 26.0f;
        const bool editable = playing && (g_snapshot.envFlags & EnvironmentEditable) != 0;
        const float lead = EditIconLead();
        std::string clock = TR("env.recorded", "Recorded");
        if (g_snapshot.envClockMinutes >= 0)
            clock = replay::i18n::Printf("%02d:%02d", g_snapshot.envClockMinutes / 60, g_snapshot.envClockMinutes % 60);
        else if (g_snapshot.envClockMinutes == env::kClockLive) // the game's clock (another mod may set it)
            clock = TR("env.live", "Game (live)");
        if (const int step = ValueRow(draw, "##envclock", y, TR("env.time", "Time of day"), clock.c_str(), editable, true, lead, ui_art::Art::ToolWeather))
            SendEnvironmentStep(EnvironmentField::Clock, step);
        y += 38.0f;
        const bool weatherLive = g_snapshot.envWeather == env::kWeatherLive;
        const int weather = g_snapshot.envWeather && !weatherLive ? env::WeatherIndex(g_snapshot.envWeather) : -1;
        const char* weatherText = weatherLive ? TR("env.live", "Game (live)") : weather >= 0 ? TR(env::kWeathers[weather].key, env::kWeathers[weather].label)
                                                                                     : TR("env.recorded", "Recorded");
        if (const int step = ValueRow(draw, "##envweather", y, TR("gallery.detail.weather", "Weather"), weatherText, editable, true, lead, ui_art::Art::ToolWeather))
            SendEnvironmentStep(EnvironmentField::Weather, step);
        y += 38.0f;
        std::string blend = TR("shake.transition.cut", "Cut");
        if (g_snapshot.envBlendS)
            blend = std::to_string(g_snapshot.envBlendS) + " s";
        // The blend is the override weather's way in: nothing to blend into
        // while the weather is the recorded one or the game's.
        if (const int step = ValueRow(draw, "##envblend", y, TR("env.weather_blend", "Weather blend"), blend.c_str(),
                editable && g_snapshot.envWeather != 0 && !weatherLive, true, lead))
            SendEnvironmentStep(EnvironmentField::Blend, step);
        y += 38.0f;
        // A clip from before the cloud track never pinned them: Live, fixed.
        const bool sky = (g_snapshot.envFlags & EnvironmentSkyTrack) != 0;
        const char* clouds = !sky || g_snapshot.envCloudsLive ? TR("env.clouds.live", "Live") : TR("env.recorded", "Recorded");
        if (const int step = ValueRow(draw, "##envclouds", y, TR("env.clouds", "Clouds"), clouds, editable && sky, false, lead))
            SendEnvironmentStep(EnvironmentField::Clouds, step);
        y += 38.0f;
        return y;
    }
