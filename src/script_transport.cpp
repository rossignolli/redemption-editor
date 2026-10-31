// script_transport.cpp: transport: start playback, scrub, trim, marker effects, settle, UpdatePlayback, camera keys, marker camera edit, the overlay and status.
// Split from script.cpp (moved code); shared internals in script_internal.h.

#include "../script_internal.h"


    // Transport state for the overlay: play, pause, or the scrub in course.
    const char* TransportLabel()
    {
        if (g_transport.Scrubbing())
        {
            const replay::transport::ScrubState& scrub = g_transport.Scrub();
            if (scrub.fine)
                return scrub.direction < 0 ? "STEP-" : "STEP+";
            return scrub.direction < 0 ? "REW2x" : "FWD2x";
        }
        if (g_transport.Paused())
            return "PAUSE";
        static char label[16];
        if (g_playbackSpeedIndex == kNormalSpeedIndex)
            return "PLAY ";
        std::snprintf(label, sizeof(label), "PLAY %d%%", static_cast<int>(kPlaybackSpeeds[g_playbackSpeedIndex] * 100.0 + 0.5));
        return label;
    }

    void DrawOverlay(const ProbeFrame& f)
    {
        REPLAY_PROFILE_ZONE("tick.overlay");
        char line[256]{};
        float y = 0.025f;

        DrawText(replay::build::kDisplayName, 0.018f, y, 0.38f, 235, 195, 95, 235);
        y += 0.028f;

        std::snprintf(line, sizeof(line), "Ped %d | Pos %.2f  %.2f  %.2f | FOV %.1f | Mount %d",
            f.playerPed, f.position.x, f.position.y, f.position.z, f.cameraFov,
            f.mount ? f.mount : f.mountTarget);
        DrawText(line, 0.018f, y, 0.30f, 245, 245, 245, 225);
        y += 0.023f;

        std::snprintf(line, sizeof(line), "Recorded %u entities | puppets %u | timeline %.2f s",
            static_cast<unsigned int>(PoseV2RecordedCount()),
            static_cast<unsigned int>(PoseV2PuppetCount()),
            static_cast<double>(PlaybackDurationMs()) / 1000.0);
        DrawText(line, 0.018f, y, 0.30f, 245, 245, 245, 225);
        y += 0.023f;

        // Profiler: script tick p95 and the three largest zones of the last
        // report window (PROFILE_* lines in the log every 5 s).
        {
            const std::string perf = replay::prof::LastWindowLine();
            if (!perf.empty())
            {
                DrawText(perf.c_str(), 0.018f, y, 0.28f, 180, 230, 180, 225);
                y += 0.023f;
            }
        }
        y += 0.004f;

        DrawText(replay::keys::Fill("{key_editor} EDITOR | {key_record} REC | {key_stop} PLAY | {key_play_pause} pause | {key_frame_back} "
                                    "{key_frame_forward} scrub",
                     replay::keys::Current())
                     .c_str(),
            0.018f, y, 0.28f, 180, 190, 200, 210);
    }

    void DrawStatus(ULONGLONG now)
    {
        REPLAY_PROFILE_ZONE("tick.status");
        char text[224]{};

        if (g_recording)
        {
            const double elapsedSeconds = static_cast<double>(
                now >= g_recordingStartedAt
                    ? now - g_recordingStartedAt
                    : 0) / 1000.0;
            if (g_recordKind == RecordKind::Buffer)
            {
                const std::uint32_t nowMs = replay::pose::capture::NowMs();
                std::snprintf(text, sizeof(text), "BUFFER %us/%us%s | %u entities",
                    (nowMs - g_bufferHeadMs) / 1000u, g_recConfig.bufferSeconds, g_manualInBuffer ? " | REC" : "",
                    static_cast<unsigned int>(PoseV2RecordedCount()));
            }
            else
                std::snprintf(text, sizeof(text), "REC %05.2fs | %u entities", elapsedSeconds,
                    static_cast<unsigned int>(PoseV2RecordedCount()));
            DrawText(text, 0.650f, 0.035f, 0.34f, 240, 45, 45, 245);
        }
        else if (g_transport.Playing())
        {
            const double elapsedSeconds =
                static_cast<double>(g_transport.PlayheadMs()) / 1000.0;
            const double durationSeconds =
                static_cast<double>(PlaybackDurationMs()) / 1000.0;
            std::snprintf(
                text,
                sizeof(text),
                "%s %05.2f/%05.2fs | %u puppets",
                TransportLabel(),
                elapsedSeconds,
                durationSeconds,
                static_cast<unsigned int>(PoseV2PuppetCount()));
            DrawText(
                text,
                0.580f,
                0.035f,
                0.34f,
                g_transport.Paused() ? 245 : 80,
                g_transport.Paused() ? 195 : 210,
                g_transport.Paused() ? 75 : 245,
                245);

            {
                char cameraText[192]{};
                const std::string speedKeys = replay::keys::Fill("{key_speed_down} {key_speed_up}", replay::keys::Current());
                std::snprintf(cameraText, sizeof(cameraText), "camera: %u markers, track %s | Ins marker | Del delete | End on/off | %s speed",
                    static_cast<unsigned>(g_cameraTrack.Keyframes().size()), g_cameraTrackEnabled && !g_cameraTrackManualOverride ? "on" : "free",
                    speedKeys.c_str());
                DrawText(cameraText, 0.620f, 0.092f, 0.25f, 200, 220, 240, 230);
            }
            DrawText(
                replay::keys::Fill("{key_stop} stop | {key_play_pause} pause | {key_frame_back} {key_frame_forward} scrub (Shift fine) | "
                                   "{key_free_cam} freecam",
                    replay::keys::Current())
                    .c_str(),
                0.620f,
                0.066f,
                0.27f,
                195,
                205,
                215,
                225);
            if (g_freeCamEnabled)
            {
                DrawText(
                    "FREECAM mouse | WASD/QE | Shift fast | PgUp/PgDn FOV",
                    0.600f,
                    0.092f,
                    0.28f,
                    90,
                    220,
                    250,
                    235);
            }
        }
        else if (g_overlayEnabled)
        {
            DrawText(
                replay::keys::Fill("READY - {key_record} record | {key_stop} play | {key_play_pause} pause | {key_frame_back} {key_frame_forward} "
                                   "scrub | {key_free_cam} free camera | F9 overlay",
                    replay::keys::Current())
                    .c_str(),
                0.625f,
                0.035f,
                0.30f,
                135,
                225,
                155,
                225);
        }
    }
namespace
{
    void ApplyMarkerEffects(bool active);
    void ApplyAbilityEffect(bool playing);
}
    MarkerHistory g_markerHistory;
namespace
{
    constexpr std::size_t kMarkerHistoryDepth = 64;
}

    // `before`, the markers as they were before the change (the caller changed
    // them and compared: only a real change is a step).
    void PushMarkerHistory(std::uint32_t type, std::uint32_t marker, const replay::camera::Track* before = nullptr)
    {
        MarkerHistory& history = g_markerHistory;
        const ULONGLONG now = GetTickCount64();
        // Only a drag (moves) and a held arrow (one option) merge: two adds or
        // two deletes are two steps.
        const bool mergeable = type == static_cast<std::uint32_t>(replay::editor::CommandType::MoveCameraKeyframe) || (type & 0x10000u);
        const bool run = mergeable && type == history.lastType && marker == history.lastMarker && now - history.lastAt < 1000 &&
            !history.undo.empty();
        history.lastType = type;
        history.lastMarker = marker;
        history.lastAt = now;
        if (run)
            return;
        history.undo.push_back(before ? *before : g_cameraTrack);
        if (history.undo.size() > kMarkerHistoryDepth)
            history.undo.erase(history.undo.begin());
        history.redo.clear();
    }
    // All the clips of the folder, as on disk and uncompressed.
    std::uint64_t g_clipsDiskBytes = 0;
    std::uint64_t g_clipsBytes = 0;
    std::uint32_t g_compressedClips = 0;

    bool StartPlayback(ULONGLONG now)
    {
        (void)now; // unused; tools/audit_retail_only_architecture.py finds the transport by this signature
        replay::prof::ResetSession();
        // A loaded clip plays as loaded (the buffer stayed off since the load); the
        // buffer resumes after this playback stops.
        g_loadedClipHoldsBuffer = false;
        // The buffer plays what it holds: stopped without a file, cut to its window in
        // memory.
        if (g_recording && g_recordKind == RecordKind::Buffer)
            StopBufferForPlayback();
        if (g_transport.DurationMs() == 0)
        {
            ShowObjective(
                "Redemption Editor: record a clip before opening the timeline");
            return false;
        }

        if (g_recording)
            StopRecording();

        g_clipStartTogglesBefore = replay::world::CurrentPressure().iplToggles;
        StopPlayback();
        if (!TakePoseFinalizer())
            return false;
        WorldModelEnter(); // before the transport and before any puppet
        g_transport.Start(PlaybackClockMs());
        BeginClipWorld();
        replay::doors::BeginPlayback(g_clip, LogFile(), g_world.cleared); // the closed theater keeps door ratios
        // The kill cam stretches of the clip (camera_track.h).
        g_cinematicSpans = replay::camera::CinematicSlowSpans(g_clip.gameCamera, g_clip.timeScale);
        g_cinematicSpanLogs = 0;
        g_cinematicSpanShown = nullptr;
        {
            std::ofstream& spanLog = LogFile();
            spanLog << "CINEMATIC_SLOW_SPANS,count=" << g_cinematicSpans.size() << ",spans=";
            for (std::size_t index = 0; index < g_cinematicSpans.size() && index < 16; ++index)
                spanLog << (index ? ";" : "") << g_cinematicSpans[index].startMs << '-' << g_cinematicSpans[index].endMs;
            spanLog << '\n';
        }

        std::ofstream& log = LogFile();
        log << "PLAYBACK_CAPTURE_MODE"
            << ",actor_backend=pose_track_v2"
            << ",task_native_actor_calls=0"
            << ",weapon_actor_calls=0"
            << '\n';
        replay::drawstore::LogSlot("playback_start", log);
        log.flush();
        // The world readiness log of this start.
        g_clipWorldLines = 0;
        g_clipWorldLastLogMs = 0;
        g_clipWorldLastBits = ~0u;
        replay::playback::Start();
        LogStreamPressure("start", "immediate", GetTickCount64());
        ShowObjective("Redemption Editor: preparing entities...");
        return true;
    }
namespace
{

    // Scrub keys held this frame: -1 backward, +1 forward, 0 none or both.
    int ScrubDirectionHeld()
    {
        if (const int bench = replay::bench::ScrubOverride(nullptr))
            return bench;
        const bool back = InterlockedCompareExchange(&g_scrubBackHeld, 0, 0) != 0;
        const bool forward = InterlockedCompareExchange(&g_scrubForwardHeld, 0, 0) != 0;
        if (back == forward)
            return 0;
        return back ? -1 : 1;
    }

    void LogScrub(const char* phase, const replay::transport::ScrubState& scrub, ULONGLONG now)
    {
        std::ofstream& log = LogFile();
        log << "PLAYBACK_SCRUB"
            << ",phase=" << phase
            << ",mode=" << (scrub.fine ? "fine" : "speed")
            << ",direction=" << scrub.direction
            << ",from_ms=" << scrub.fromMs
            << ",to_ms=" << scrub.toMs
            << ",frames=" << scrub.frames
            << ",wall_ms=" << (now >= scrub.startedAt ? now - scrub.startedAt : 0ULL) // start: startedAt can be the next tick
            << ",boundary=" << (scrub.atBoundary ? 1 : 0)
            << '\n';
        log.flush();
    }

    // The playhead moves in the transport; this reports what it decided. While
    // scrubbing every subsystem samples the new time on this frame and nothing
    // one-shot plays (PoseV2NormalPlay is false); a scrub ends paused where it
    // stopped.
    // ---- Clip trim, marker speed, effects, marker snap ----
    // The first and last markers are the clip's start and end: a clip opened
    // without markers gets both (Game Camera), the playback stays between them,
    // and they cannot be deleted.
    std::uint64_t g_clipHoldMs = 0;
}
    bool g_trimReady = false;
namespace
{

    const replay::camera::Keyframe* ActiveMarkerAt(std::uint64_t timeMs)
    {
        const replay::camera::Keyframe* active = nullptr;
        for (const replay::camera::Keyframe& key : g_cameraTrack.Keyframes())
        {
            if (key.timeMs > timeMs)
                break;
            if (key.markerType != replay::editor::MarkerType::Anchor)
                active = &key;
        }
        return active;
    }
}

    bool TrimActive()
    {
        return g_cameraTrackEnabled && g_cameraTrack.Keyframes().size() >= 2 && !g_cameraTrack.LegacyBlendAnyMode();
    }

    // A clip bound (start or end marker) at timeMs: Game Camera, with the recorded
    // game camera at that time as its pose (a marker that inherits it, or a type
    // change to Free, starts from that view instead of the world origin). The
    // clip's own bounds below and a project clip's bounds (FitMarkers templates)
    // are built this way.
    replay::camera::Keyframe GameBoundMarker(std::uint64_t timeMs)
    {
        replay::camera::Keyframe key;
        key.cameraMode = replay::editor::CameraMode::Game;
        key.timeMs = timeMs;
        replay::editor::CameraState game{};
        if (replay::camera::EvaluateGame(g_clip.gameCamera, timeMs, game, &g_clip.cuts))
            key.pose = {game.position, replay::camera::Track::FromEulerDegrees(game.rotation), game.fov};
        return key;
    }
namespace
{

    void ApplyClipTrim(ULONGLONG now)
    {
        if (!g_trimReady && !replay::playback::Preparing())
        {
            g_trimReady = true;
            g_clipHoldMs = g_transport.StartMs();
            // Also a track with one marker (added while the clip was still loading, before
            // the bounds came: otherwise the bounds never come and deleting it leaves no
            // camera track at all).
            if (g_cameraTrack.Keyframes().size() < 2 && !g_cameraTrack.LegacyBlendAnyMode() &&
                g_transport.DurationMs() > g_clipHoldMs + 100)
            {
                // The bounds carry the game camera at their time as their pose: a marker that
                // inherits it, or a type change to Free, starts from that view instead of the
                // world origin.
                const replay::camera::Keyframe start = GameBoundMarker(g_clipHoldMs);
                const replay::camera::Keyframe end = GameBoundMarker(g_transport.DurationMs());
                // A marker already there keeps its place (AddMarker refuses
                // one too close to it).
                g_cameraTrack.AddMarker(start);
                g_cameraTrack.AddMarker(end);
                replay::camera::ApplyLastMarkerRule(g_cameraTrack);
                g_cameraTrackEnabled = true;
                LogFile() << "CAMERA_MARKER,action=clip_bounds,start_ms=" << start.timeMs << ",end_ms=" << end.timeMs << '\n';
            }
        }
        if (!g_trimReady || !TrimActive())
            return;
        const std::vector<replay::camera::Keyframe>& keys = g_cameraTrack.Keyframes();
        const std::uint64_t start = std::max<std::uint64_t>(g_clipHoldMs, keys.front().timeMs);
        const std::uint64_t end = std::max<std::uint64_t>(start, keys.back().timeMs);
        g_transport.SetStartMs(start);
        if (g_transport.PlayheadMs() + 1 < start)
            g_transport.Seek(start);
        // Nor past the end marker (a paused seek, Ctrl+Right or a scrub going on past
        // it: the playback stays between the clip's start and end markers).
        if (end > start && g_transport.PlayheadMs() > end && (g_transport.Paused() || g_transport.Scrubbing()))
            g_transport.Seek(end);
        if (!g_transport.Paused() && !g_transport.Scrubbing() && g_transport.PlayheadMs() >= end && end > start)
        {
            // A project playing as a whole goes on to its next clip.
            if (ProjectReachedClipEnd(now))
                return;
            SetPlaybackPaused(true, now);
            g_transport.Seek(end);
            g_seekReason = "trim_end";
        }
    }

    // Dead Eye in the replay (off by default): the span at the playhead plays the
    // game's screen effect, and the slow motion is kept (the playhead is not sped
    // up to game speed there).
    // What renders at the playhead is the recorded game camera: no freecam, no
    // Edit Camera, and no marker in effect or a Game-mode one.
    bool GameCameraShownAt(std::uint64_t timeMs)
    {
        if (g_freeCamEnabled || g_editingMarker)
            return false;
        if (!g_cameraTrackEnabled || g_cameraTrack.Empty())
            return true;
        const replay::camera::SegmentPlan plan = replay::camera::Plan(g_cameraTrack, timeMs);
        const std::vector<replay::camera::Keyframe>& keys = g_cameraTrack.Keyframes();
        if (!plan.valid || plan.current < 0 || plan.current >= static_cast<int>(keys.size()))
            return true;
        return keys[plan.current].mode == replay::editor::CameraMode::Game && !plan.blending;
    }

    const replay::AbilitySpan* AbilityAt(std::uint64_t timeMs)
    {
        for (const replay::AbilitySpan& span : g_clip.abilities)
            if (timeMs >= span.startMs && timeMs <= span.endMs)
                return &span;
        return nullptr;
    }

    std::string g_abilityPostFx;
}
    AbilityAudio g_abilityAudio;
namespace
{
    constexpr Hash kDeadEyeScene = 0x2233F49Au;

    const char* AbilitySoundSet(const replay::AbilitySpan& span)
    {
        if (span.kind == replay::AbilityKind::EagleEye)
            return "EAGLE_EYE_SOUNDS";
        static const Hash john = MISC::GET_HASH_KEY("player_three");
        return g_clip.place.playerModel == john ? "DEADEYE_NEUTRAL_02_SOUNDS" : "DEADEYE_NEUTRAL_01_SOUNDS";
    }
}

    void StopAbilityAudio(bool withExit)
    {
        AbilityAudio& audio = g_abilityAudio;
        if (!audio.span)
            return;
        if (audio.bed >= 0)
        {
            AUDIO::_STOP_SOUND_WITH_ID(audio.bed);
            AUDIO::RELEASE_SOUND_ID(audio.bed);
        }
        if (audio.scene)
        {
            if (audio.span->kind == replay::AbilityKind::EagleEye)
                AUDIO::STOP_AUDIO_SCENE("EAGLE_EYE_SCENE");
            else
                AUDIO::_0x6AB944DF68B512D3(static_cast<Any>(kDeadEyeScene));
        }
        if (withExit)
            AUDIO::PLAY_SOUND_FRONTEND("exit", audio.soundSet, TRUE, 0);
        LogFile() << "ABILITY_AUDIO,action=stop,exit=" << (withExit ? 1 : 0) << '\n';
        audio = {};
    }
namespace
{

    void UpdateAbilityAudio(const replay::AbilitySpan* span)
    {
        const bool normal = span && replay::playback::NormalPlay();
        AbilityAudio& audio = g_abilityAudio;
        if (audio.span && audio.span != span)
            StopAbilityAudio(span == nullptr && replay::playback::NormalPlay()); // left the span by playing on
        if (!normal)
        {
            if (audio.span)
                StopAbilityAudio(false);
            return;
        }
        if (!audio.span)
        {
            audio.span = span;
            audio.soundSet = AbilitySoundSet(*span);
            const BOOL prepared = AUDIO::PREPARE_SOUNDSET(audio.soundSet, FALSE);
            AUDIO::PLAY_SOUND_FRONTEND("enter", audio.soundSet, TRUE, 0);
            if (span->kind == replay::AbilityKind::EagleEye)
                audio.scene = AUDIO::START_AUDIO_SCENE("EAGLE_EYE_SCENE") != FALSE;
            else
            {
                AUDIO::_0xDC2F83A0612CA34D(static_cast<Any>(kDeadEyeScene));
                audio.scene = true;
                audio.bed = AUDIO::GET_SOUND_ID();
                AUDIO::_PLAY_SOUND_FRONTEND_WITH_SOUND_ID(audio.bed, "BED", audio.soundSet, TRUE);
            }
            LogFile() << "ABILITY_AUDIO,action=start,set=" << audio.soundSet << ",prepared=" << (prepared ? 1 : 0)
                      << ",bed=" << audio.bed << '\n';
        }
        if (audio.bed >= 0)
            AUDIO::_SET_VARIABLE_ON_SOUND_WITH_ID(audio.bed, "deadeye_time",
                static_cast<float>(g_transport.PlayheadMs() - span->startMs) / 1000.0f);
    }

    void ApplyAbilityEffect(bool playing)
    {
        if (!playing)
            StopAbilityAudio(false);
        const replay::AbilitySpan* span = playing && g_recConfig.replayDeadEye ? AbilityAt(g_transport.PlayheadMs()) : nullptr;
        UpdateAbilityAudio(span);
        const char* fx = !span ? "" : span->kind == replay::AbilityKind::DeadEye ? "DEADEYE" : "EagleEye";
        if (g_abilityPostFx == fx)
            return;
        if (!g_abilityPostFx.empty())
            GRAPHICS::ANIMPOSTFX_STOP(g_abilityPostFx.c_str());
        if (*fx)
            GRAPHICS::ANIMPOSTFX_PLAY(fx);
        LogFile() << "ABILITY_PLAY,effect=" << (*fx ? fx : "none") << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
        g_abilityPostFx = fx;
    }
}

    // The marker's speed (5 to 200 %) from it to the next one.
    double MarkerSpeedAt(std::uint64_t timeMs)
    {
        if (!g_cameraTrackEnabled)
            return 1.0;
        const replay::camera::Keyframe* active = ActiveMarkerAt(timeMs);
        return active ? replay::camera::SpeedFactor(active->speedIndex) : 1.0;
    }
namespace
{

    // Effects (the game's own photo mode): the filter is one of its animated post
    // effects with a strength.
    //
    // Exposure and contrast (build 1491.50 and the camera_photomode script):
    //  - _CHANGE_PHOTO_MODE_EXPOSURE / _CONTRAST ADD their argument to the photo
    //    mode value (the handlers are `value += arg`; the photo mode script steps
    //    them by +-0.1 and +-0.0125).
    //  - the post fx update applies them only while the byte that
    //    _0xF5793BB386E1FF9C sets is on (the photo mode sets it to 1 while open,
    //    0 on exit) and a photo mode state predicate holds; outside the photo mode
    //    that predicate is the flag _0x9F6D859C80708B26 (p0 value, p1 0) sets.
    //    Exposure also needs the exposure lock byte at 0
    //    (_SET_PHOTO_MODE_EXPOSURE_LOCKED(FALSE)); it then REPLACES the auto
    //    exposure, clamped to -16..16; contrast is clamped to 0.6..2.
    //  - _UPDATE_PHOTO_MODE_EXPOSURE re-seeds from the exposure already applied,
    //    not from the auto exposure, so calling it every frame and adding the
    //    offset on top runs away to the 16 clamp. The photo mode script seeds the
    //    exposure ONCE (_UPDATE_PHOTO_MODE_EXPOSURE and a read, on the lock toggle
    //    and the first HUD), then only steps it: +-0.1 a press; the HUD shows
    //    ceil(e * 10) + 161 and stops raising at 240, so its top is 7.9; below,
    //    only the engine clamp (-16) holds.
    //  - contrast: +-0.0125 a frame held (0.025 with the keyboard); the HUD shows
    //    ceil(c * 10) - 6 clamped to 0..14, so 0.6 to 2.0.
    // Our -100..100 maps to that range around the value found when grading
    // started: 0 is that value, +100 the photo mode top, -100 its bottom. Every
    // frame the change passed is the delta from the value read to the target (the
    // natives add), never the offset again. An exposure 0 marker drives the
    // exposure back to the seed and gives the exposure back to the auto exposure
    // (lock byte on); the seed is taken again only when an exposure starts from 0
    // (the auto exposure is running then).
    std::string g_effectPostFx;
    constexpr float kGradeExposureMin = -16.0f;
    constexpr float kGradeExposureMax = 7.9f;
    constexpr float kGradeContrastMin = 0.6f;
    constexpr float kGradeContrastMax = 2.0f;
    struct MarkerGrade
    {
        bool engaged = false;
        bool exposureOwned = false; // the seed below is valid and in use
        float baseContrast = 1.0f;
        float baseExposure = 0.0f;
        std::int8_t contrast = 0;
        std::int8_t exposure = 0;
        std::uint32_t logs = 0;
        std::uint32_t settleFrames = 0; // frames left before the settled log
    };
    MarkerGrade g_markerGrade;

    // Marker percent (-100..100) to a value around a base inside [lo, hi].
    float GradeTarget(float base, std::int8_t percent, float lo, float hi)
    {
        const float p = std::clamp(static_cast<float>(percent), -100.0f, 100.0f) / 100.0f;
        const float b = std::clamp(base, lo, hi);
        return p >= 0.0f ? b + p * (hi - b) : b + p * (b - lo);
    }

    void ReleaseMarkerGrade(const char* why)
    {
        MarkerGrade& grade = g_markerGrade;
        if (grade.engaged)
        {
            const float contrastNow = GRAPHICS::_GET_PHOTO_MODE_CONTRAST();
            GRAPHICS::_CHANGE_PHOTO_MODE_CONTRAST(grade.baseContrast - contrastNow);
            if (grade.exposureOwned)
                GRAPHICS::_CHANGE_PHOTO_MODE_EXPOSURE(grade.baseExposure - GRAPHICS::_GET_PHOTO_MODE_EXPOSURE());
            LogFile() << "MARKER_EFFECTS,grade=off,why=" << why << ",contrast_base=" << grade.baseContrast
                      << ",contrast_after=" << GRAPHICS::_GET_PHOTO_MODE_CONTRAST()
                      << ",exposure_base=" << grade.baseExposure
                      << ",exposure_after=" << GRAPHICS::_GET_PHOTO_MODE_EXPOSURE() << '\n';
        }
        // Also when not engaged: a grade of a module instance before a
        // Ctrl+R is not in this instance's state. The lock byte back to 0,
        // the value the photo mode script starts with (bLocal_11 false).
        GRAPHICS::_SET_PHOTO_MODE_EXPOSURE_LOCKED(FALSE);
        GRAPHICS::_0xF5793BB386E1FF9C(FALSE);
        GRAPHICS::_0x9F6D859C80708B26(FALSE, FALSE);
        grade.engaged = false;
        grade.exposureOwned = false;
        grade.contrast = grade.exposure = 0;
        grade.settleFrames = 0;
    }

    void ApplyMarkerGrade(std::int8_t contrast, std::int8_t exposure)
    {
        MarkerGrade& grade = g_markerGrade;
        if (contrast == 0 && exposure == 0)
        {
            if (grade.engaged)
                ReleaseMarkerGrade("marker");
            return;
        }
        if (!grade.engaged)
        {
            const float found = GRAPHICS::_GET_PHOTO_MODE_CONTRAST();
            grade.baseContrast = found >= kGradeContrastMin && found <= kGradeContrastMax ? found : 1.0f;
            grade.engaged = true;
            grade.exposureOwned = false;
            grade.contrast = grade.exposure = 0;
        }
        // Seed the exposure once, before the flags replace the auto one
        // (the photo mode's own order: update, read, then step).
        if (exposure != 0 && !grade.exposureOwned)
        {
            GRAPHICS::_UPDATE_PHOTO_MODE_EXPOSURE();
            grade.baseExposure = std::clamp(GRAPHICS::_GET_PHOTO_MODE_EXPOSURE(), kGradeExposureMin, kGradeExposureMax);
            grade.exposureOwned = true;
        }
        GRAPHICS::_0xF5793BB386E1FF9C(TRUE);
        GRAPHICS::_0x9F6D859C80708B26(TRUE, FALSE);
        // Exposure: lock byte 0 replaces the auto exposure with ours; on an
        // exposure 0 marker the auto exposure runs again (lock on) and our
        // value is also driven back to the seed, so it is 0 either way.
        GRAPHICS::_SET_PHOTO_MODE_EXPOSURE_LOCKED(exposure != 0 ? FALSE : TRUE);
        const float exposureTarget = grade.exposureOwned
            ? GradeTarget(grade.baseExposure, exposure, kGradeExposureMin, kGradeExposureMax) : 0.0f;
        if (grade.exposureOwned)
        {
            const float delta = exposureTarget - GRAPHICS::_GET_PHOTO_MODE_EXPOSURE();
            if (std::fabs(delta) > 0.0005f)
                GRAPHICS::_CHANGE_PHOTO_MODE_EXPOSURE(delta);
            if (exposure == 0 && std::fabs(delta) <= 0.0005f)
                grade.exposureOwned = false; // back at the seed: the next exposure seeds again
        }
        const float contrastTarget = GradeTarget(grade.baseContrast, contrast, kGradeContrastMin, kGradeContrastMax);
        const float contrastNow = GRAPHICS::_GET_PHOTO_MODE_CONTRAST();
        if (std::fabs(contrastTarget - contrastNow) > 0.0005f)
            GRAPHICS::_CHANGE_PHOTO_MODE_CONTRAST(contrastTarget - contrastNow);
        const bool changed = contrast != grade.contrast || exposure != grade.exposure;
        if (changed)
        {
            grade.contrast = contrast;
            grade.exposure = exposure;
            grade.settleFrames = 30;
        }
        // One line on the change and one 30 frames later (the values read
        // must equal the targets and stay there, no drift).
        const bool settled = !changed && grade.settleFrames > 0 && --grade.settleFrames == 0;
        if ((changed || settled) && grade.logs < 48)
        {
            ++grade.logs;
            LogFile() << "MARKER_EFFECTS,grade=on,at=" << (changed ? "change" : "settled")
                      << ",contrast=" << static_cast<int>(contrast)
                      << ",exposure=" << static_cast<int>(exposure) << ",contrast_base=" << grade.baseContrast
                      << ",contrast_target=" << contrastTarget
                      << ",contrast_read=" << GRAPHICS::_GET_PHOTO_MODE_CONTRAST()
                      << ",exposure_base=" << grade.baseExposure
                      << ",exposure_owned=" << (grade.exposureOwned ? 1 : 0)
                      << ",exposure_target=" << exposureTarget
                      << ",exposure_read=" << GRAPHICS::_GET_PHOTO_MODE_EXPOSURE()
                      << ",exposure_lock=" << (exposure != 0 ? 0 : 1) << '\n';
        }
    }

    // The clip's fade in and fade out: a black full screen rectangle drawn every
    // frame with the fade amount at the playhead, so a scrub or a still shows
    // exactly the fade of that instant.
    float g_fadeDrawn = 0.0f;

    void DrawMarkerFade(bool active)
    {
        const float amount = active && g_cameraTrackEnabled
            ? replay::camera::FadeAmount(g_cameraTrack, g_transport.PlayheadMs()) : 0.0f;
        const int alpha = static_cast<int>(amount * 255.0f + 0.5f);
        if (alpha > 0)
            GRAPHICS::DRAW_RECT(0.5f, 0.5f, 1.0f, 1.0f, 0, 0, 0, alpha, FALSE, TRUE);
        // Evidence at the ends of a fade (not per frame).
        const bool edge = (amount >= 0.999f) != (g_fadeDrawn >= 0.999f) || (amount > 0.0f) != (g_fadeDrawn > 0.0f);
        if (edge)
            LogFile() << "MARKER_EFFECTS,fade=" << amount << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
        g_fadeDrawn = amount;
    }

    void ApplyMarkerEffects(bool active)
    {
        const replay::camera::Keyframe* marker = active && g_cameraTrackEnabled ? ActiveMarkerAt(g_transport.PlayheadMs()) : nullptr;
        const std::uint32_t filter = marker ? marker->filter : 0;
        const char* fx = filter < replay::editor::kMarkerFilterCount ? replay::editor::kMarkerFilterPostFx[filter] : "";
        if (g_effectPostFx != fx)
        {
            if (!g_effectPostFx.empty())
                GRAPHICS::ANIMPOSTFX_STOP(g_effectPostFx.c_str());
            if (*fx)
                GRAPHICS::ANIMPOSTFX_PLAY(fx);
            g_effectPostFx = fx;
            LogFile() << "MARKER_EFFECTS,filter=" << (*fx ? fx : "none") << '\n';
        }
        if (*fx)
            GRAPHICS::_ANIMPOSTFX_SET_STRENGTH(fx, marker->filterIntensity / 100.0f);
        // Exposure and contrast are off: turning the photo mode grade on outside photo
        // mode blows the image to white. Saved values stay in the clip, unused.
        constexpr bool kMarkerGradeEnabled = false;
        if (marker && kMarkerGradeEnabled)
            ApplyMarkerGrade(marker->contrast, marker->brightness);
        else if (g_markerGrade.engaged)
            ReleaseMarkerGrade(active ? "no_marker" : "stop");
        DrawMarkerFade(active);
    }
}

    // Every screen effect the replay starts is stopped when a playback stops or
    // starts and when the mod unloads: the marker filter and the Dead Eye / Eagle
    // Eye post effect it tracks, the photo mode exposure and contrast, and any
    // photo mode filter still running (only the replay plays those outside the
    // game's photo mode; one started by a module instance before a Ctrl+R is not
    // in this instance's state). The game's own effects (damage, low health) are
    // left alone.
    void StopOwnScreenEffects(const char* why)
    {
        ApplyMarkerEffects(false);
        ReleaseMarkerGrade(why); // the photo mode grading flags too
        ApplyAbilityEffect(false);
        std::uint32_t swept = 0;
        for (std::uint32_t index = 1; index < replay::editor::kMarkerFilterCount; ++index)
        {
            const char* name = replay::editor::kMarkerFilterPostFx[index];
            if (GRAPHICS::ANIMPOSTFX_IS_RUNNING(name))
            {
                GRAPHICS::ANIMPOSTFX_STOP(name);
                ++swept;
            }
        }
        if (swept)
            LogFile() << "MARKER_EFFECTS,action=swept,why=" << why << ",stopped=" << swept << '\n';
    }
namespace
{

    // Fine scrub stops on a marker it crosses (a snap): the first marker in the
    // scrub's direction (going back, the latest one below the old playhead), and
    // only on the fine scrub gesture.
    void SnapScrubToMarker(std::uint64_t before)
    {
        if (!g_transport.Scrubbing() || !g_transport.Scrub().fine)
            return;
        if (const replay::camera::Keyframe* key =
                replay::camera::FirstMarkerCrossed(g_cameraTrack, before, g_transport.PlayheadMs()))
            g_transport.Seek(key->timeMs);
    }

    // ---- seek settle (seek_settle.h) ----

    // The "jump" load scene the settle started goes with it.
    void EndSettleJumpScene()
    {
        if (g_settleJumpScene && g_clipWorld.loadPhase && std::strcmp(g_clipWorld.loadPhase, "jump") == 0)
        {
            StopClipLoadScene();
            g_clipWorld.loadPhase = nullptr;
            g_clipWorld.jumpMeters = 0.0f;
        }
        g_settleJumpScene = false;
    }

    void LogSettleEnd(const char* phase, ULONGLONG now, bool timedOut)
    {
        if (!g_jumpLoadLogs.Take(kJumpLoadLogCap))
            return;
        const replay::settle::Readiness& last = g_settle.LastReadiness();
        LogFile() << "JUMP_LOAD,phase=" << phase << ",reason=" << g_settle.Reason() << ",far="
                  << replay::settle::FarReasons(g_settle.FarBits()) << ",wait_ms="
                  << (std::strcmp(phase, "end") == 0 ? g_settle.EndedElapsedMs() : g_settle.ElapsedMs(now))
                  << ",timed_out=" << (timedOut ? 1 : 0) << ",ticks=" << g_settle.Ticks() << ",retargets=" << g_settle.Retargets()
                  << ",missing=" << replay::settle::Missing(last) << ",building=" << last.building
                  << ",models_missing=" << last.modelsMissing << ",streaming_requests=" << STREAMING::GET_NUMBER_OF_STREAMING_REQUESTS()
                  << ",target_ms=" << g_settle.TargetMs() << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
    }

    // A scrub or a drag cancels the wait (and its preloads).
    void CancelSettle(ULONGLONG now, const char* why)
    {
        if (!g_settle.Active())
            return;
        g_settle.Cancel(now);
        LogSettleEnd(why, now, false);
        EndSettleJumpScene();
    }

    void BeginSettle(ULONGLONG now, const char* reason, bool fine, std::uint64_t fromMs, std::uint64_t toMs)
    {
        replay::settle::JumpFacts facts;
        facts.fine = fine;
        facts.windowCentreMs = g_preloadCentreValid ? g_preloadCentreMs : fromMs;
        facts.targetMs = toMs;
        facts.puppetsWithoutModel = replay::playback::AliveWithoutModel(static_cast<std::uint32_t>(toMs));
        const std::uint32_t bits = replay::settle::FarBitsOf(facts, g_settleConfig);
        const bool wasFar = g_settle.Far();
        g_settle.Begin(reason, now, toMs, bits);
        if (!wasFar && g_settle.Far())
            ++g_seekLoadSequence;
        // The camera part of the test runs in this tick's UpdateReplayCamera,
        // against the camera rendered before the jump.
        g_settleCameraCheck = !fine;
        g_settleCameraFrom = g_renderedCamera.position;
        g_settleCameraFromValid = g_renderedCamera.valid;
        g_nextPreloadAt = 0; // the model window moves to the target now
        if (g_jumpLoadLogs.Take(kJumpLoadLogCap))
        {
            LogFile() << "JUMP_LOAD,phase=begin,reason=" << reason << ",fine=" << (fine ? 1 : 0) << ",far="
                      << replay::settle::FarReasons(g_settle.FarBits()) << ",from_ms=" << fromMs << ",to_ms=" << toMs
                      << ",window_centre_ms=" << facts.windowCentreMs << ",without_model=" << facts.puppetsWithoutModel
                      << ",retargets=" << g_settle.Retargets() << '\n';
        }
    }

    // Before the transport moves: a scrub cancels the settle, the settle holds the
    // time, and normal play holds a frame for a due puppet whose model is loaded
    // (bounded).
    void SettleBeforeTransport(ULONGLONG now, replay::transport::Input& input)
    {
        const bool scrubInput = input.scrubDirection != 0;
        if (scrubInput)
            CancelSettle(now, "cancelled_scrub");
        bool creationHold = false;
        const std::uint32_t due = replay::playback::GetSettleState().dueWithModel;
        if (due == 0)
            g_creationHoldSpent = false;
        if (!g_creationHoldSpent && !g_settle.Active() && !scrubInput && !g_transport.Paused() && !g_transport.Scrubbing() &&
            !replay::playback::Preparing() && !ClipLoadWaiting())
        {
            creationHold = replay::settle::HoldForCreation(replay::playback::NormalPlay(), due, g_creationHeldFrames, g_settleConfig);
        }
        if (creationHold)
        {
            ++g_creationHeldFrames;
            ++g_creationHolds;
        }
        else if (g_creationHeldFrames)
        {
            // Still due after the bound: the engine keeps refusing; play on
            // and hold again only after nothing was due.
            g_creationHoldSpent = due > 0;
            if (g_creationHoldLogs.Take(64))
            {
                LogFile() << "CREATION_HOLD,frames=" << g_creationHeldFrames << ",total=" << g_creationHolds
                          << ",still_due=" << due << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
            }
            g_creationHeldFrames = 0;
        }
        input.hold = replay::playback::Preparing() || ClipLoadWaiting() || (g_settle.Active() && !scrubInput) || creationHold;
    }

    // After the transport moved (and the seeks of this tick's commands): a
    // scrub release, a drag end or a jump starts the settle; the cheap scrub
    // flag for this tick's playback.
    void SettleAfterTransport(ULONGLONG now, std::uint32_t events)
    {
        const std::uint64_t playhead = g_transport.PlayheadMs();
        const std::uint64_t before = g_settleLastPlayhead;
        g_settleLastPlayhead = playhead;
        const char* reason = g_seekReason;
        bool fine = g_seekFine;
        g_seekReason = nullptr;
        g_seekFine = false;
        const bool opening = replay::playback::Preparing() ||
            (ClipLoadWaiting() && std::strcmp(g_clipWorld.loadPhase, "jump") != 0); // the clip's own load covers it
        bool begin = false;
        if (opening)
            CancelSettle(now, "cancelled_open"); // a clip (re)opening waits on its own
        if (before == ~0ull || opening)
            begin = false;
        else if ((events & replay::transport::kScrubEnded) && !g_transport.Scrubbing()) // not a direction change
        {
            reason = "scrub_release";
            fine = g_transport.EndedScrub().fine;
            begin = true;
        }
        else if (g_seekDrag.TakeIdleEnd(now, g_settleConfig))
        {
            reason = "drag_idle";
            begin = true;
        }
        else if (reason && std::strcmp(reason, "drag_end") == 0)
            begin = true; // the release may land on the last dragged time
        else if (!g_seekDrag.Dragging() &&
            replay::settle::IsJumpMove(before, playhead, g_transport.Paused(), g_transport.Scrubbing()))
        {
            if (!reason)
                reason = "jump";
            begin = true;
        }
        if (g_seekDrag.Dragging())
            CancelSettle(now, "cancelled_drag");
        if (begin)
            BeginSettle(now, reason, fine, before, playhead);
        // Cheap scrub: while the playhead is scrubbed or dragged, and while
        // the settle still builds puppets (the state rebuild runs once they
        // exist, so it reaches every puppet of the target).
        const bool building = g_settle.Active() && replay::playback::GetSettleState().building > 0;
        replay::playback::SetCheapScrub(g_transport.Scrubbing() || g_seekDrag.Dragging() || building);
    }
}

    replay::settle::CostCounters CostCountersNow()
    {
        replay::settle::CostCounters counters;
        counters.decodes = replay::pose::DecodedFrameCount();
        counters.checkpointRestores = replay::pose::CheckpointRestoreCount();
        const replay::events::StateCounters state = replay::events::Counters();
        counters.decalReconciles = state.decalReconciles;
        counters.bloodReconciles = state.bloodReconciles;
        counters.goreReconciles = state.goreReconciles;
        counters.fireReconciles = state.fireReconciles;
        counters.deferredTicks = state.deferredTicks;
        const replay::playback::Counters playback = replay::playback::GetCounters();
        counters.creates = playback.creates;
        counters.vehicleRebuilds = playback.vehicleRebuilds;
        return counters;
    }

    // Once per tick after the camera and the clip world: the model window,
    // the settle's readiness and its end, the create budget.
    void UpdateSeekSettle(ULONGLONG now)
    {
        REPLAY_PROFILE_ZONE("tick.seek_settle");
        if (!g_transport.Playing())
        {
            if (g_settle.Active())
                CancelSettle(now, "cancelled_stop");
            g_settleLastPlayhead = ~0ull;
            g_preloadCentreValid = false;
            g_nextPreloadAt = 0;
            g_seekDrag.Reset();
            g_creationHeldFrames = 0;
            g_creationHoldSpent = false;
            return;
        }
        const std::uint32_t playhead = static_cast<std::uint32_t>(g_transport.PlayheadMs());
        if (now >= g_nextPreloadAt && !replay::playback::Preparing())
        {
            g_nextPreloadAt = now + 250;
            replay::playback::PreloadModels(playhead, now, static_cast<std::uint32_t>(g_settleConfig.windowMs));
            g_preloadCentreMs = playhead;
            g_preloadCentreValid = true;
        }
        if (g_settle.Active())
        {
            const replay::playback::SettleState state = replay::playback::GetSettleState();
            replay::settle::Readiness readiness;
            readiness.building = state.building;
            readiness.modelsMissing = state.modelsMissing;
            readiness.stateDeferred = replay::events::StateDeferred();
            readiness.statePending = replay::events::StatePending();
            readiness.sceneLoaded = !(g_clipWorld.loadPhase && std::strcmp(g_clipWorld.loadPhase, "jump") == 0);
            const replay::settle::Verdict verdict = g_settle.Update(now, readiness, g_settleConfig);
            if (verdict.promoted)
            {
                ++g_seekLoadSequence;
                LogSettleEnd("promoted", now, false);
            }
            if (verdict.done)
            {
                LogSettleEnd("end", now, verdict.timedOut);
                EndSettleJumpScene();
            }
        }
        replay::playback::SetCreateBudget(replay::settle::CreateBudget(g_settle.Active(), g_settle.Far()));
        replay::events::SetStateBudget(g_settle.Active(), g_settle.Far());
        replay::playback::SetWatchHeld(g_settle.Active() || ClipLoadWaiting());
    }

    // One SCRUB_COST line per scrub (keys or a drag), at the tick after it.
    void UpdateScrubCost(double tickUs)
    {
        const bool cheap = g_transport.Playing() && (g_transport.Scrubbing() || g_seekDrag.Dragging());
        if (cheap)
        {
            if (!g_scrubCost.Active())
            {
                const bool drag = g_seekDrag.Dragging();
                const char* kind = drag ? "drag" : g_transport.Scrub().fine ? "fine" : "keys";
                const int direction = drag ? (g_transport.PlayheadMs() >= g_tickStartPlayhead ? 1 : -1) : g_transport.Scrub().direction;
                g_scrubCost.Begin(kind, direction, g_tickStartPlayhead, g_tickStartCounters);
            }
            g_scrubCost.Tick(tickUs, replay::playback::GetSettleState().puppets);
            return;
        }
        if (!g_scrubCost.Active())
            return;
        const std::string line = g_scrubCost.Finish(g_transport.PlayheadMs(), tickUs, CostCountersNow(), replay::playback::CheapScrub());
        if (g_scrubCostLogs.Take(200))
        {
            LogFile() << line;
        }
    }
namespace
{

    // The world time scale and the playhead rate UpdatePlayback below applies at
    // `playheadMs` (playing, not scrubbing: an export never scrubs). Keep the two
    // in step.
    float PlaybackWorldScaleAt(std::uint64_t playheadMs, double& rate)
    {
        const double speed = kPlaybackSpeeds[g_playbackSpeedIndex] * MarkerSpeedAt(playheadMs);
        const float recordedScale = g_clip.timeScale.At(static_cast<std::uint32_t>(playheadMs));
        const bool deadEyeSlow = g_recConfig.replayDeadEye && AbilityAt(playheadMs) != nullptr;
        const replay::camera::TimeSpan* cinematic = replay::camera::SpanAt(g_cinematicSpans, playheadMs);
        const bool slowAbility = deadEyeSlow || (cinematic && GameCameraShownAt(playheadMs));
        rate = slowAbility ? speed : speed / (recordedScale > 0.0f ? recordedScale : 1.0f);
        float world = speed < 1.0 ? static_cast<float>(speed) : 1.0f;
        if (slowAbility)
            world = std::min(world, recordedScale);
        return world;
    }
}

    // The export's output frames (exportprobe::Request): the span at the rate
    // UpdatePlayback gives each place.
    std::uint64_t ExportOutputFrames(std::uint64_t startMs, std::uint64_t endMs, replay::exporting::FrameRate rate)
    {
        const auto rateAt = [](std::uint64_t ms) {
            double playheadRate = 1.0;
            PlaybackWorldScaleAt(ms, playheadRate);
            return playheadRate;
        };
        return replay::exporting::SpanOutputFrames(startMs, endMs, rate, rateAt, 50000000ull);
    }
namespace
{

    // The lowest world scale over the next two video frames of an export:
    // the playhead walked forward in 1 ms steps of video time at the rate of
    // each place (a marker's slow motion that starts inside the next frame
    // already lowers that frame's motion blur samples).
    float ExportWorldScaleAhead(std::uint64_t playheadMs, float applied)
    {
        if (!replay::exportprobe::Active())
            return applied;
        const replay::exporting::FrameRate rate = replay::exportprobe::ExportRate();
        const double windowMs = 2000.0 * rate.den / (rate.num ? rate.num : 60);
        double playhead = static_cast<double>(playheadMs);
        const double end = static_cast<double>(PlaybackDurationMs());
        float lowest = applied;
        for (double t = 0.0; t <= windowMs && playhead <= end; t += 1.0)
        {
            double playRate = 1.0;
            lowest = std::min(lowest, PlaybackWorldScaleAt(static_cast<std::uint64_t>(playhead), playRate));
            playhead += playRate;
        }
        return lowest;
    }
}

    void UpdatePlayback(ULONGLONG now)
    {
        REPLAY_PROFILE_ZONE("tick.playback");
        if (!g_transport.Playing())
            return;

        replay::transport::Input input;
        input.hold = replay::playback::Preparing() || ClipLoadWaiting(); // the world loads
        input.scrubDirection = g_editingMarker ? 0 : ScrubDirectionHeld(); // Edit Camera stays on its marker
        bool benchFine = false;
        input.fineScrub = InterlockedCompareExchange(&g_fastMove, 0, 0) != 0 ||
            (replay::bench::ScrubOverride(&benchFine) != 0 && benchFine);
        SettleBeforeTransport(now, input);
        // Render export: while an export runs, the export clock drives the transport,
        // one tick per game frame, whatever the wall clock does. Inert unless
        // RedemptionEditor.ini [Export] Enabled=1 and the editor asked for an export.
        replay::exportprobe::TickInput exportInput;
        exportInput.playheadMs = g_transport.PlayheadMs();
        exportInput.durationMs = g_transport.DurationMs();
        exportInput.settling = g_settle.Active() || ClipLoadWaiting();
        exportInput.frameTime = MISC::GET_FRAME_TIME();
        exportInput.worldScale = g_worldTimeScaleApplied;
        exportInput.transportLostMs = g_transport.LostMs();
        if (replay::exportprobe::WantsScreenSignals())
        {
            // The hold (src/export_hold.h has each native's evidence): no
            // frame is taken while the game shows a menu or a frontend screen.
            namespace eh = replay::exporthold;
            if (HUD::IS_PAUSE_MENU_ACTIVE())
                exportInput.screenSignals |= eh::SignalMenu;
            if (UISTICKYFEED::_UI_STICKY_FEED_IS_ALERT_SCREEN_ACTIVE())
                exportInput.screenSignals |= eh::SignalAlert;
            if (DLC::GET_IS_LOADING_SCREEN_ACTIVE() || SCRIPTS::IS_LOADING_SCREEN_VISIBLE())
                exportInput.screenSignals |= eh::SignalLoading;
        }
        const replay::exportprobe::TickPlan exportPlan = replay::exportprobe::BeginTick(exportInput, LogFile());
        if (exportPlan.seekToStart)
        {
            SetPlaybackPaused(true, now);
            g_seekReason = "export";
            g_seekFine = false;
            g_transport.Seek(std::min<std::uint64_t>(exportPlan.startMs, PlaybackDurationMs()));
        }
        if (exportPlan.resume)
            SetPlaybackPaused(false, now); // the first export ticks hold time, so the transport takes the export clock
        if (exportPlan.pause)
            SetPlaybackPaused(true, now);
        if (exportPlan.holdTime)
            input.hold = true;
        if (exportPlan.active)
            input.scrubDirection = 0;
        // The game HUD is hidden below (HidesHud joins the playing test),
        // the debug drawing is gated by HidesHud, and the text notices are
        // suppressed: nothing of the mod or the game is burnt into the video.
        replay::hud::SetSuppressed(exportPlan.hideHud);
        if (exportPlan.drawTag)
        {
            // Latency tag (export_channel.h): drawn in preroll and flush
            // pictures only, which are never encoded.
            namespace ec = replay::exportchannel;
            for (std::uint32_t cell = 0; cell < ec::kTagCells; ++cell)
            {
                const int v = ec::TagCellWhite(exportPlan.tagSerial, cell) ? 255 : 0;
                GRAPHICS::DRAW_RECT(ec::TagCellCentreX(cell), ec::TagCellCentreY(), ec::kTagCellWidth, ec::kTagCellHeight, v, v, v, 255, FALSE, TRUE);
            }
        }
        // Time compensation: where the game ran slowed while recording
        // (Dead Eye, weapon wheel), the playhead runs 1/scale faster so the
        // stretch plays at game speed.
        {
            const LONG step = InterlockedExchange(&g_speedStepRequested, 0);
            if (step)
            {
                const int wanted = std::max(0, std::min(static_cast<int>(std::size(kPlaybackSpeeds)) - 1, g_playbackSpeedIndex + static_cast<int>(step)));
                if (wanted != g_playbackSpeedIndex)
                {
                    g_playbackSpeedIndex = wanted;
                    char text[64]{};
                    std::snprintf(text, sizeof(text), "Redemption Editor: speed %d%%", static_cast<int>(kPlaybackSpeeds[wanted] * 100.0 + 0.5));
                    ShowObjective(text);
                    LogFile() << "PLAYBACK_SPEED,percent=" << static_cast<int>(kPlaybackSpeeds[wanted] * 100.0 + 0.5)
                              << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
                }
            }
        }
        ApplyClipTrim(now);
        const double speed = kPlaybackSpeeds[g_playbackSpeedIndex] * MarkerSpeedAt(g_transport.PlayheadMs());
        const float recordedScale = g_clip.timeScale.At(static_cast<std::uint32_t>(g_transport.PlayheadMs()));
        const bool deadEyeSlow = g_recConfig.replayDeadEye && AbilityAt(g_transport.PlayheadMs()) != nullptr;
        // The kill cam keeps its slow motion while the recorded game camera shows it
        // (the time compensation would turn 1.9 s of kill cam into 0.23 s: rate
        // 1/0.061). Under a free or preset marker the stretch plays at game speed.
        const replay::camera::TimeSpan* cinematic = replay::camera::SpanAt(g_cinematicSpans, g_transport.PlayheadMs());
        const bool cinematicSlow = cinematic && GameCameraShownAt(g_transport.PlayheadMs());
        if (cinematic != g_cinematicSpanShown)
        {
            g_cinematicSpanShown = cinematic;
            if (cinematic && g_cinematicSpanLogs < 32)
            {
                ++g_cinematicSpanLogs;
                LogFile() << "CINEMATIC_SLOW,phase=enter,start_ms=" << cinematic->startMs << ",end_ms=" << cinematic->endMs
                          << ",game_camera=" << (cinematicSlow ? 1 : 0) << ",recorded_scale=" << recordedScale
                          << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
            }
        }
        const bool slowAbility = deadEyeSlow || cinematicSlow;
        // With Dead Eye on in the replay its slow motion stays; otherwise the
        // stretch plays at game speed (the time compensation).
        g_transport.SetRate(slowAbility ? speed : speed / recordedScale);
        {
            float world = (speed < 1.0 && !g_transport.Paused() && !g_transport.Scrubbing()) ? static_cast<float>(speed) : 1.0f;
            if (slowAbility && !g_transport.Paused() && !g_transport.Scrubbing())
                world = std::min(world, recordedScale); // sounds and particles slow down with it
            if (world != g_worldTimeScaleApplied)
            {
                MISC::SET_TIME_SCALE(world);
                g_worldTimeScaleApplied = world;
            }
        }
        const std::uint64_t playheadBefore = g_transport.PlayheadMs();
        const std::uint32_t events = g_transport.Update(replay::exportprobe::TransportNowMs(PlaybackClockMs()), input);
        replay::exportprobe::EndTick(g_transport.PlayheadMs(), ExportWorldScaleAhead(g_transport.PlayheadMs(), g_worldTimeScaleApplied),
            LogFile()); // render export: the tick's stamp, the next step (the next frame's blur samples)
        SnapScrubToMarker(playheadBefore);
        SettleAfterTransport(now, events);
        ApplyMarkerEffects(true);
        ApplyAbilityEffect(true);
        if (events == 0)
            return;

        using namespace replay::transport;
        if (events & kScrubEnded)
            LogScrub("end", g_transport.EndedScrub(), now);
        if (events & kPaused)
            LogTransportPaused(true);
        if (events & kScrubStarted)
        {
            const ScrubState& scrub = g_transport.Scrub();
            LogScrub("start", scrub, now);
            ShowObjective(scrub.fine
                ? (scrub.direction < 0 ? "Redemption Editor: fine scrub backward" : "Redemption Editor: fine scrub forward")
                : (scrub.direction < 0 ? "Redemption Editor: scrub 2x backward" : "Redemption Editor: scrub 2x forward"));
        }
        if (events & kReachedStart)
            ShowObjective("Redemption Editor: start of the timeline");
        if (events & kReachedEnd)
            ShowObjective("Redemption Editor: end of the timeline");
    }
    replay::editor::CameraState CurrentEditorCameraState()
    {
        replay::editor::CameraState camera{};
        if (g_freeCamEnabled)
        {
            camera.position = {g_freeCamPosition.x, g_freeCamPosition.y, g_freeCamPosition.z};
            camera.rotation = {g_freeCamRotation.x, g_freeCamRotation.y, g_freeCamRotation.z};
            camera.fov = g_freeCamFov;
            return camera;
        }

        const Vector3 position = CAM::GET_FINAL_RENDERED_CAM_COORD();
        const Vector3 rotation = CAM::GET_FINAL_RENDERED_CAM_ROT(2);
        camera.position = {position.x, position.y, position.z};
        camera.rotation = {rotation.x, rotation.y, rotation.z};
        camera.fov = CAM::GET_FINAL_RENDERED_CAM_FOV();
        return camera;
    }

    bool IsValidEasing(replay::editor::Easing easing)
    {
        return easing >= replay::editor::Easing::Linear &&
            easing <= replay::editor::Easing::Hold;
    }

    bool IsValidPath(replay::editor::Path path)
    {
        return path >= replay::editor::Path::Linear &&
            path <= replay::editor::Path::Smooth;
    }

    // Insert and Delete outside the editor's menus (the editor closed or hidden,
    // Edit Camera), through the marker API, as the editor's commands (the keyframe
    // API, AddOrReplace and Remove, would turn the track into a converted v2 clip
    // and could delete the clip's start or end marker). Nothing while a marker is
    // in Edit Camera.
    void ProcessCameraKeys()
    {
        if (InterlockedExchange(&g_cameraKeyAddRequested, 0) && !g_editingMarker)
        {
            if (g_transport.Playing())
            {
                const replay::camera::Track before = g_cameraTrack; // a step only when added
                AddMarkerAtPlayhead();
                if (!replay::camera::SameMarkers(before, g_cameraTrack))
                {
                    PushMarkerHistory(static_cast<std::uint32_t>(replay::editor::CommandType::CaptureCameraKeyframe), 0, &before);
                    g_clipEdited = true;
                }
                char text[96]{};
                std::snprintf(text, sizeof(text), "Camera: marker at %.2fs (%u markers)",
                    g_transport.PlayheadMs() / 1000.0, static_cast<unsigned>(g_cameraTrack.Keyframes().size()));
                ShowObjective(text);
            }
            else
                ShowObjective(replay::keys::Fill("Camera: play the replay ({key_stop}) to add markers", replay::keys::Current()).c_str());
        }
        if (InterlockedExchange(&g_cameraKeyDeleteRequested, 0) && !g_editingMarker)
        {
            const auto& keys = g_cameraTrack.Keyframes();
            const bool bounds = TrimActive();
            const std::uint64_t playhead = g_transport.PlayheadMs();
            const auto distance = [&](std::uint64_t t) { return t > playhead ? t - playhead : playhead - t; };
            const replay::camera::Keyframe* nearest = nullptr;
            for (std::size_t index = 0; index < keys.size(); ++index)
            {
                if (bounds && (index == 0 || index + 1 == keys.size()))
                    continue; // the clip's start and end stay
                if (!nearest || distance(keys[index].timeMs) < distance(nearest->timeMs))
                    nearest = &keys[index];
            }
            if (nearest)
            {
                const replay::camera::Track before = g_cameraTrack; // a step only when removed
                const std::uint64_t removedAt = nearest->timeMs;
                const std::uint32_t removedId = nearest->id;
                if (g_cameraTrack.RemoveMarker(removedId))
                {
                    PushMarkerHistory(static_cast<std::uint32_t>(replay::editor::CommandType::DeleteCameraKeyframe), removedId, &before);
                    g_clipEdited = true;
                    replay::camera::ApplyLastMarkerRule(g_cameraTrack);
                    g_cameraTrackManualOverride = false;
                    g_cameraSpring.Reset();
                }
                char text[96]{};
                std::snprintf(text, sizeof(text), "Camera: marker at %.2fs deleted (%u markers)", removedAt / 1000.0,
                    static_cast<unsigned>(g_cameraTrack.Keyframes().size()));
                ShowObjective(text);
                LogFile() << "CAMERA_MARKER,action=delete,id=" << removedId << ",from=key,left=" << g_cameraTrack.Keyframes().size()
                          << '\n';
            }
        }
        if (InterlockedExchange(&g_cameraTrackToggleRequested, 0))
        {
            g_cameraTrackEnabled = !g_cameraTrackEnabled;
            g_cameraTrackManualOverride = !g_cameraTrackEnabled;
            ShowObjective(g_cameraTrackEnabled ? "Camera: track on" : "Camera: track off (free camera)");
            LogFile() << "CAMERA_KEY,action=toggle,enabled=" << g_cameraTrackEnabled << '\n';
        }
    }

    // ---- camera markers: add, edit camera ----
    replay::camera::Keyframe g_copiedMarker;
    bool g_hasCopiedMarker = false;
    std::uint32_t g_editingMarker = 0; // "Edit Camera": the free camera frames this marker

    // A marker's attach pose and Look At framing from a world camera: what
    // "Use this view" and leaving Edit Camera store.
    void CaptureMarkerRelations(std::uint32_t id, const replay::editor::CameraState& camera)
    {
        replay::camera::Keyframe* key = g_cameraTrack.FindMutable(id);
        if (!key)
            return;
        const std::uint64_t at = key->timeMs;
        replay::camera::EntityState target{};
        const replay::camera::Scene scene = MarkerScene(); // through the bones
        if (replay::camera::LookupAttach(*key, at, scene, target))
            replay::camera::CaptureAttachPose(*key, camera, target);
        if (replay::camera::LookupLookAt(*key, at, scene, target))
        {
            const replay::editor::Vec3 point = target.hasHead ? target.head
                : replay::editor::Vec3{target.position.x, target.position.y, target.position.z + replay::camera::kLookAtAimHeight};
            replay::editor::Vec2 screen{};
            if (replay::camera::ProjectToScreen(camera, point, MarkerScene().aspect, screen))
            {
                key->lookAtOffset = {std::clamp(screen.x, -1.0f, 1.0f), std::clamp(screen.y, -1.0f, 1.0f)};
                key->lookAtRoll = camera.rotation.y;
            }
        }
    }

    // Add: at the playhead, inheriting from the marker before it; the view is the
    // free camera's when it is on, otherwise the frame being rendered.
    void AddMarkerAtPlayhead()
    {
        const std::uint64_t at = g_transport.PlayheadMs();
        // The start and end markers bound every other marker.
        if (!replay::camera::WithinClipBounds(g_cameraTrack, at))
        {
            replay::hud::ShowNotice(TR("hud.marker_bounds", "Redemption Editor: markers go between the clip's start and end markers."));
            return;
        }
        if (!replay::camera::CanPlace(g_cameraTrack, at))
        {
            replay::hud::ShowNotice(TR("hud.marker_close", "Redemption Editor: too close to another marker."));
            return;
        }
        replay::camera::Keyframe fresh;
        fresh.timeMs = at;
        const replay::camera::Keyframe* previous = nullptr;
        for (const replay::camera::Keyframe& key : g_cameraTrack.Keyframes())
            if (key.timeMs <= at)
                previous = &key;
        if (previous)
            replay::camera::InheritFrom(fresh, *previous, replay::camera::InsideBlend(g_cameraTrack, at));
        // The shake in force there (a marker added inside a blend holds the blended
        // numbers: nothing changes).
        fresh.shake = replay::camera::mshake::HeldAt(g_cameraTrack, at);
        replay::editor::CameraState view{};
        if (g_freeCamEnabled)
        {
            view = CurrentEditorCameraState();
            fresh.cameraMode = replay::editor::CameraMode::Free;
        }
        else
        {
            const Vector3 p = CAM::GET_FINAL_RENDERED_CAM_COORD();
            const Vector3 r = CAM::GET_FINAL_RENDERED_CAM_ROT(2);
            view = {{p.x, p.y, p.z}, {r.x, r.y, r.z}, CAM::GET_FINAL_RENDERED_CAM_FOV()};
        }
        // The pose is always the frame shown at the playhead. InheritFrom copies no
        // pose from a Game Camera marker (the clip's start), so the new marker would
        // keep the default pose, the world origin, and turning it into a Free camera
        // (or Edit Camera, whose orbit clamps the distance to 150 m) would send the
        // camera far away. A preset's stored pose is a placeholder too; a Free marker
        // before it already took the rendered frame.
        fresh.pose = {view.position, replay::camera::Track::FromEulerDegrees(view.rotation),
            std::clamp(view.fov, replay::editor::kMarkerFovMin, replay::editor::kMarkerFovMax)};
        const std::uint32_t id = g_cameraTrack.AddMarker(fresh);
        if (!id)
            return;
        // Always (the freecam off too): the view stored so the marker shows it back
        // with its attach and Look At.
        if (replay::camera::Keyframe* added = g_cameraTrack.FindMutable(id))
            replay::camera::SetMarkerView(*added, view, MarkerScene());
        replay::camera::ApplyLastMarkerRule(g_cameraTrack);
        g_cameraTrackEnabled = true;
        g_cameraTrackManualOverride = false;
        LogFile() << "CAMERA_MARKER,action=add,id=" << id << ",at_ms=" << at << ",mode="
                  << static_cast<std::uint32_t>(fresh.cameraMode) << ",inherited=" << (previous ? previous->id : 0) << '\n';
    }

    void LeaveMarkerCameraEdit(bool keep)
    {
        if (!g_editingMarker)
            return;
        if (keep && g_freeCamEnabled)
        {
            const replay::editor::CameraState view = CurrentEditorCameraState();
            // Every way out that keeps the view (Enter, F11) is an edit: an undo step and
            // unsaved changes.
            if (g_cameraTrack.Find(g_editingMarker))
            {
                PushMarkerHistory(static_cast<std::uint32_t>(replay::editor::CommandType::EditCameraKeyframe), g_editingMarker);
                g_markerHistory.lastType = 0; // never merged with the next change
                g_clipEdited = true;
            }
            if (replay::camera::Keyframe* key = g_cameraTrack.FindMutable(g_editingMarker))
            {
                // The pose only: Track::Update would also re-derive this marker's and the
                // previous one's blend from the legacy easing and turn the whole track into a
                // converted v2 clip (legacyBlendAnyMode: blends between any camera types, the
                // clip's start and end no longer protected nor trimming, saved in the clip).
                replay::camera::SetMarkerView(*key, view, MarkerScene());
                if (g_orbit.marker == key->id)
                {
                    // The orbit's framing and roll as set, not re-derived.
                    if (replay::camera::Keyframe* stored = g_cameraTrack.FindMutable(key->id))
                    {
                        stored->lookAtOffset = g_orbit.offset;
                        stored->lookAtRoll = g_orbit.roll;
                    }
                }
                replay::camera::ApplyLastMarkerRule(g_cameraTrack); // the end follows the new framing now
                LogFile() << "CAMERA_MARKER,action=edit_done,id=" << key->id << '\n';
            }
        }
        g_editingMarker = 0;
        g_orbit.marker = 0;
        // The free camera as it was before Edit Camera (a free camera the player had
        // on stays on).
        if (!g_freeCamBeforeEdit)
            StopFreeCam();
        g_cameraTrackManualOverride = false;
        g_cameraSpring.Reset();
    }

    // "Edit Camera": paused on the marker, the free camera starts from the
    // marker's view; leaving stores the view (and the attach and framing).
    void EnterMarkerCameraEdit(std::uint32_t id, ULONGLONG now)
    {
        const replay::camera::Keyframe* key = g_cameraTrack.Find(id);
        if (!key || !g_transport.Playing())
            return;
        LeaveMarkerCameraEdit(true);
        SetPlaybackPaused(true, now);
        g_transport.Seek(std::min<std::uint64_t>(key->timeMs, PlaybackDurationMs()));
        // The puppets still stand at the old playhead: this view is a first
        // guess; UpdateFreeCam resolves it again once they stand at the
        // marker's time (an attach or Look At target moved in between).
        g_editResolvePending = true;
        g_freeCamBeforeEdit = g_freeCamEnabled;
        replay::editor::CameraState view{};
        const replay::camera::LivePose live = EditStartPose(*key);
        view.position = live.pose.position;
        view.rotation = replay::camera::Track::ToEulerDegrees(live.pose.orientation);
        view.fov = live.pose.fov;
        if (!g_freeCamEnabled && !StartFreeCam(now))
            return;
        g_freeCamPosition = {view.position.x, view.position.y, view.position.z};
        g_freeCamRotation = {view.rotation.x, view.rotation.y, view.rotation.z};
        g_freeCamFov = view.fov;
        // The marker's view, roll included, and nothing else: an acrobatic or Drone
        // flight before this (its orientation, banking, turn rates, momentum) does
        // not carry over.
        g_freeCamVelocity = {};
        ResetFreeCamToView();
        g_cameraTrackManualOverride = true; // the free camera does not snap back to the track
        g_editingMarker = id;
        LogFile() << "CAMERA_MARKER,action=edit,id=" << id << ",level=1,roll=" << view.rotation.y << '\n';
    }
