// replay_editor_addon_help.cpp: the key tips of every screen, the key bar and the help screen.
// Split from replay_editor_addon.cpp (moved code); shared internals in replay_editor_addon_internal.h.

#include "replay_editor_addon_internal.h"

namespace
{

    // A list for the bar (the tips of the state, `when` true) or for the F1
    // help (every tip with a help row, whatever the state).
    struct TipList
    {
        KeyTips& out;
        bool help;
        void Add(bool when, const char* key, const char* label, const char* helpKey = nullptr, const char* helpText = nullptr, bool on = true)
        {
            if (help ? helpKey != nullptr : (when && key != nullptr))
                out.push_back({key, label, helpKey, helpText ? helpText : label, on});
        }
    };

    // Shift+F1 on the bar while marks wait ("Send report (3)"); on the F1 help
    // always, with marks on.
    void ReportTip(TipList& t)
    {
        static char label[160];
        const std::uint32_t pending = qa_shots::PendingMarks();
        replay::editor::uiqa::FormatReportKey(label, sizeof(label), pending);
        const char* key = KeyText(Action::Report);
        t.Add(pending > 0, key, label, qa_shots::MarksEnabled() ? key : nullptr,
            TR("help.save_the_qa_report", "Save the QA report (one zip, every mark and its clips)"));
    }

    // The clip editor (DrawEdit, DrawEditPanel, DrawEditTimeline). With a
    // marker's menu open the arrows and Enter are the menu's.
    void EditTips(TipList& t, bool menu)
    {
        const bool root = g_markerMenu == MarkerMenu::Root;
        // The remappable keys from the [Keys] table (KeyText, PairText).
        const char* frame = PairText(Action::FrameBack, Action::FrameForward);
        t.Add(true, fixedkey::Play, TR("keys.play_pause", "Play/Pause"), JoinText(fixedkey::Play, KeyText(Action::PlayPause)), TR("help.play_pause", "Play, pause"));
        // Left and Right change the side panel row under the mouse (ValueRow)
        // instead of stepping a frame.
        t.Add(!menu && !g_valueRowHovered, "Left Right", TR("keys.frame", "Frame"), JoinText("Left Right", frame), TR("help.one_frame", "One frame"));
        t.Add(!menu && g_valueRowHovered, "Left Right", TR("keys.change", "Change"), TR("keys.cap.left_right_on_a_row", "Left Right on a row"), TR("help.change_the_setting_under_the_mouse", "Change the setting under the mouse"));
        t.Add(menu, frame, TR("keys.frame", "Frame"));
        if (Keys().Bound(Action::Rewind) || Keys().Bound(Action::FastForward))
        {
            const char* rewind = PairText(Action::Rewind, Action::FastForward);
            t.Add(!menu, rewind, TR("keys.rewind_ff", "Rewind/FF"), rewind, TR("help.rewind_fast_forward_hold", "Rewind, fast forward (hold)"));
        }
        t.Add(menu, "Up Down", TR("keys.row", "Row"), "Up Down, Left Right", TR("help.marker_menu_row_value", "Marker menu: row, value"));
        t.Add(menu, "Left Right", TR("keys.value", "Value"));
        t.Add(menu, "PgUp PgDn", TR("keys.big_step", "Big step"), "PageUp PageDown", TR("help.value_big_steps", "Value: big steps"));
        t.Add(menu, fixedkey::Select, TR("keys.select", "Select"));
        t.Add(!menu, fixedkey::Select, TR("keys.add_marker", "Add marker"), fixedkey::Select, TR("help.add_a_marker_open_it", "Add a marker / open it"));
        t.Add(true, "Tab Ctrl+Tab", TR("keys.markers", "Markers"), "Tab / Ctrl+Tab", TR("help.next_previous_marker", "Next / previous marker"));
        t.Add(!menu, TR("keys.cap.click_mark", "Click mark"), TR("keys.select_drag", "Select, drag"), TR("keys.cap.click_a_marker", "Click a marker"), TR("help.select_it_drag_move", "Select it (drag: move)"));
        t.Add(menu, "Del", TR("keys.delete_marker", "Delete marker"), "Del", TR("help.delete_the_marker", "Delete the marker"));
        t.Add(menu, "Ctrl+C Ctrl+V", TR("keys.copy_paste", "Copy/Paste"), "Ctrl+C / Ctrl+V", TR("help.copy_paste_marker", "Copy / paste marker"));
        t.Add(!menu, "Ctrl+Left Right", TR("keys.start_end", "Start/End"), "Ctrl+Left / Ctrl+Right", TR("help.clip_start_end", "Clip start / end"));
        t.Add(!menu, TR("keys.cap.wheel_on_bar", "Wheel on bar"), TR("keys.frames", "Frames"), TR("keys.cap.wheel_on_the_timeline", "Wheel on the timeline"), TR("help.frames_shift_ten", "Frames (Shift: ten)"));
        t.Add(!menu, TR("keys.cap.ctrl_wheel", "Ctrl+Wheel"), TR("keys.zoom", "Zoom"), TR("keys.cap.ctrl_wheel_on_it", "Ctrl + wheel on it"), TR("help.zoom_the_timeline", "Zoom the timeline"));
        const char* speed = PairText(Action::SpeedDown, Action::SpeedUp);
        t.Add(!menu, speed, TR("keys.speed", "Speed"), speed, TR("help.playback_speed", "Playback speed"));
        t.Add(true, "Ctrl+Z Ctrl+Y", TR("keys.undo_redo", "Undo/Redo"), "Ctrl+Z / Ctrl+Y", TR("help.undo_redo", "Undo / redo"), Flag(SnapshotCanUndo) || Flag(SnapshotCanRedo));
        t.Add(true, fixedkey::Save, TR("keys.save", "Save"), fixedkey::Save, TR("help.save_the_clip", "Save the clip"));
        t.Add(!menu, fixedkey::Export, TR("keys.export", "Export"), fixedkey::Export, TR("help.export_video_render_export", "Export video (render export)"));
        t.Add(!menu, "T", TR("keys.thumbnail", "Thumbnail"), "T", TR("help.thumbnail_from_this_frame", "Thumbnail from this frame"), g_snapshot.currentClip[0] != 0);
        t.Add(!menu, "P", TR("keys.photo", "Photo"), "P", TR("help.photo_of_this_frame", "Photo of this frame"));
        const char* freeCam = KeyText(Action::FreeCam);
        const char* hide = KeyText(Action::HideUi);
        const char* stop = KeyText(Action::Stop);
        const char* mark = KeyText(Action::MarkProblem);
        const char* editor = KeyText(Action::Editor);
        const char* markHelp = KeyFill(TR("help.mark_problem", "Mark a problem (QA report: {key_report})"));
        t.Add(!menu, freeCam, Flag(SnapshotFreeCamEnabled) ? TR("keys.free_cam_off", "Free cam off") : TR("keys.free_cam", "Free cam"), freeCam, TR("help.free_camera", "Free camera"));
        t.Add(true, hide, Flag(SnapshotFreeCamEnabled) ? TR("keys.hide_ui_fly", "Hide UI, fly") : TR("keys.hide_ui", "Hide UI"), hide, TR("help.hide_the_editor_and_the_playback", "Hide the editor and the playback bar"));
        t.Add(!menu, stop, TR("keys.stop", "Stop"), stop, TR("help.stop_the_clip", "Stop the clip"));
        t.Add(!menu && qa_shots::MarksEnabled(), mark, TR("keys.mark_problem", "Mark problem"), mark, markHelp);
        if (!menu)
            ReportTip(t);
        t.Add(!menu, editor, TR("keys.close", "Close"), editor, TR("help.close_the_editor", "Close the editor"));
        t.Add(true, fixedkey::Help, TR("keys.all_keys", "All keys"));
        t.Add(menu, fixedkey::Back, root ? TR("keys.close_menu", "Close menu") : TR("keys.back", "Back"));
        t.Add(!menu, fixedkey::Back, ProjectFlag(ProjectEditingClip) ? TR("keys.project", "Project") : TR("keys.menu", "Menu"), fixedkey::Back, TR("help.back", "Back"));
    }

    // Edit Camera (DrawEditCameraOverlay; the core flies the camera): the
    // free camera's keys, or the orbit's round a Look At target.
    void EditCameraTips(TipList& t, bool orbit)
    {
        t.Add(!orbit, fixedkey::Fly, TR("keys.move", "Move"), "W A S D", TR("help.move", "Move"));
        t.Add(!orbit, "Q E", TR("keys.down_up", "Down Up"), "Q E", TR("help.down_up", "Down, up"));
        t.Add(!orbit, TR("keys.cap.mouse", "Mouse"), TR("keys.look", "Look"), TR("keys.cap.mouse", "Mouse"), TR("help.look", "Look"));
        t.Add(orbit, "A D", TR("keys.orbit", "Orbit"));
        t.Add(orbit, "W S", TR("keys.over_under", "Over Under"));
        t.Add(orbit, "Up Down", TR("keys.distance", "Distance"));
        t.Add(orbit, "Q E", TR("keys.height", "Height"));
        t.Add(orbit, TR("keys.cap.mouse", "Mouse"), TR("keys.framing", "Framing"));
        t.Add(true, "Z C", TR("keys.roll", "Roll"), "Z C", TR("help.roll", "Roll"));
        t.Add(true, "PgUp PgDn", TR("keys.zoom", "Zoom"), "PageUp PageDown", TR("help.zoom", "Zoom"));
        t.Add(!orbit, "Shift Ctrl", TR("keys.fast_slow", "Fast Slow"), "Shift / Ctrl", TR("help.fast_slow", "Fast / slow"));
        t.Add(!orbit, TR("keys.cap.wheel", "Wheel"), TR("keys.speed", "Speed"), TR("keys.cap.wheel", "Wheel"), TR("help.speed", "Speed"));
        t.Add(true, fixedkey::Select, TR("keys.keep", "Keep"), fixedkey::Select, TR("help.keep_the_view", "Keep the view"));
        t.Add(true, fixedkey::Cancel, TR("keys.cancel", "Cancel"), JoinText(fixedkey::Cancel, fixedkey::Back), TR("help.cancel", "Cancel"));
    }

    // A clip or the project playing with the editor closed (the playback bar):
    // the keys go to the game and the core (script.cpp, OnKeyboard: K , . [ ] F7
    // F11), F2, E and H to the editor. H hides the bar and this key bar (hidden,
    // no bar is drawn: the F1 help explains).
    void PlaybackTips(TipList& t)
    {
        const bool freeCam = Flag(SnapshotFreeCamEnabled);
        // The keys of the [Keys] table.
        const char* play = KeyText(Action::PlayPause);
        const char* frame = PairText(Action::FrameBack, Action::FrameForward);
        const char* speed = PairText(Action::SpeedDown, Action::SpeedUp);
        const char* freeCamKey = KeyText(Action::FreeCam);
        t.Add(true, play, TR("keys.play_pause", "Play/Pause"), play, TR("help.pause_play", "Pause, play"));
        t.Add(true, frame, TR("keys.frame", "Frame"), frame, TR("help.one_frame_hold_rewind_fast_forward", "One frame (hold: rewind, fast forward)"));
        if (Keys().Bound(Action::Rewind) || Keys().Bound(Action::FastForward))
        {
            const char* rewind = PairText(Action::Rewind, Action::FastForward);
            t.Add(true, rewind, TR("keys.rewind_ff", "Rewind/FF"), rewind, TR("help.rewind_fast_forward_hold", "Rewind, fast forward (hold)"));
        }
        t.Add(true, speed, TR("keys.speed", "Speed"), speed, TR("help.playback_speed", "Playback speed"));
        t.Add(true, freeCamKey, freeCam ? TR("keys.free_cam_off", "Free cam off") : TR("keys.free_cam", "Free cam"), freeCamKey, TR("help.free_camera", "Free camera"));
        t.Add(freeCam, fixedkey::Fly, TR("keys.move", "Move"));
        t.Add(freeCam, "Q E", TR("keys.down_up", "Down Up"));
        t.Add(freeCam, TR("keys.cap.mouse", "Mouse"), TR("keys.look", "Look"));
        t.Add(freeCam, TR("keys.cap.wheel", "Wheel"), TR("keys.cam_speed", "Cam speed"));
        // Drone, the target, follow, the world markers.
        {
            const char* drone = KeyText(Action::FreeCamDrone);
            const char* pick = KeyText(Action::FreeCamPick);
            const char* follow = KeyText(Action::FreeCamFollow);
            const char* overlay = KeyText(Action::CameraOverlay);
            t.Add(freeCam, drone, g_snapshot.freeCamMotion ? TR("keys.classic", "Classic") : TR("keys.drone", "Drone"), drone, TR("help.free_camera_drone_flight_on_off", "Free camera: Drone flight on/off"));
            t.Add(freeCam, pick, TR("keys.target", "Target"), pick, TR("help.free_camera_the_target_under_the", "Free camera: the target under the crosshair"));
            t.Add(false, nullptr, nullptr, KeyText(Action::FreeCamPickNearest), TR("help.free_camera_the_nearest_target", "Free camera: the nearest target"));
            t.Add(freeCam && g_snapshot.freeCamTarget != 0, follow, TR("keys.follow", "Follow"), follow, TR("help.free_camera_follow_the_target_rigid", "Free camera: follow the target (rigid, position only, off)"));
            t.Add(false, nullptr, nullptr, KeyText(Action::FreeCamAcrobatic), TR("help.free_camera_acrobatic_rotation_on_off", "Free camera: acrobatic rotation on/off"));
            t.Add(false, nullptr, nullptr, KeyText(Action::FreeCamWalk), TR("help.free_camera_walk_on_off", "Free camera: Walk on/off"));
            t.Add(false, nullptr, nullptr, KeyText(Action::FreeCamCollision), TR("help.free_camera_collision_on_off", "Free camera: collision on/off"));
            t.Add(false, nullptr, nullptr, KeyText(Action::FreeCamLockAltitude), TR("help.free_camera_keep_the_altitude", "Free camera: keep the altitude"));
            t.Add(false, nullptr, nullptr, KeyText(Action::FreeCamLevel), TR("help.free_camera_level_the_horizon", "Free camera: level the horizon"));
            t.Add(false, nullptr, nullptr, KeyText(Action::FreeCamBehind), TR("help.free_camera_behind_the_character", "Free camera: behind the character"));
            t.Add(false, nullptr, nullptr, KeyText(Action::FreeCamLock), TR("help.free_camera_hold_still", "Free camera: hold still"));
            t.Add(true, overlay, TR("keys.markers", "Markers"), overlay, TR("help.camera_markers_and_path_in_the", "Camera markers and path in the world on/off"));
        }
        const char* hide = KeyText(Action::HideUi);
        const char* stop = KeyText(Action::Stop);
        const char* mark = KeyText(Action::MarkProblem);
        t.Add(true, hide, TR("keys.hide_ui", "Hide UI"), hide, TR("help.hide_the_replay_ui_the_keys", "Hide the replay UI (the keys keep working)"));
        // E only without the free camera (it flies up there).
        t.Add(true, freeCam ? KeyText(Action::Editor) : KeyText(Action::Editor, fixedkey::Edit), TR("keys.edit", "Edit"), KeyText(Action::Editor, fixedkey::Edit),
            TR("help.open_the_editor", "Open the editor"));
        t.Add(true, stop, TR("keys.stop", "Stop"), stop, TR("help.play_stop_the_last_clip", "Play / stop the last clip"));
        t.Add(qa_shots::MarksEnabled(), mark, TR("keys.mark_problem", "Mark problem"), mark,
            KeyFill(TR("help.mark_problem", "Mark a problem (QA report: {key_report})")));
        ReportTip(t);
        // F4 and Shift+F4 work with the editor closed only.
        t.Add(false, nullptr, nullptr, KeyText(Action::Record), TR("help.record_mark_a_recording", "Record / mark a recording"));
        t.Add(false, nullptr, nullptr, KeyText(Action::SaveBuffer), TR("help.save_the_replay_buffer", "Save the replay buffer"));
    }

    // The project's timeline (DrawProject). The cut and the trims work on
    // the selected clip while it is open and playing: muted otherwise.
    // The Projects list (DrawProjects): on the bar, and on the F1 help over it.
    void ProjectsTips(TipList& t)
    {
        const bool file = g_projectsIndex > 0;
        const bool readable = file && g_projectsIndex - 1 < static_cast<int>(kMaxProjectsInSnapshot) &&
            g_snapshot.projects[g_projectsIndex - 1].readable;
        t.Add(true, "Up Down", TR("keys.select", "Select"), "Up Down", TR("help.select_a_project", "Select a project"));
        t.Add(true, fixedkey::Select, file ? TR("keys.open", "Open") : TR("keys.new_project", "New project"), fixedkey::Select,
            TR("help.open_it_or_make_a_new_one", "Open it (on New Project: make one)"), !file || readable);
        t.Add(file, "R", TR("keys.rename", "Rename"), "R", TR("help.rename_the_project", "Rename the project (its file too)"));
        t.Add(file, "Del", TR("keys.delete", "Delete"), "Del", TR("help.delete_the_project", "Delete the project"));
        t.Add(true, fixedkey::Help, TR("keys.all_keys", "All keys"));
        t.Add(true, fixedkey::Back, TR("keys.back", "Back"));
    }

    void ProjectTips(TipList& t)
    {
        const int count = static_cast<int>(std::min(g_snapshot.projectClipCount, kMaxProjectClipsInSnapshot));
        const bool playing = Flag(SnapshotPlaying);
        const bool any = ProjectIsOpen() && count > 0;
        const int active = g_snapshot.projectActiveClip < static_cast<std::uint32_t>(count) ? static_cast<int>(g_snapshot.projectActiveClip) : -1;
        const bool atPlayhead = active >= 0 && active == g_projectIndex && playing && !ProjectFlag(ProjectLoadingClip);
        const bool previewing = ProjectFlag(ProjectPreviewing) && playing;
        t.Add(any, fixedkey::Play, previewing && !Flag(SnapshotPaused) ? TR("keys.pause", "Pause") : TR("keys.play", "Play"), JoinText(fixedkey::Play, KeyText(Action::PlayPause)),
            TR("help.play_the_project_pause", "Play the project, pause"));
        t.Add(any, "Left Right", TR("keys.select", "Select"), "Left Right", TR("help.select_a_clip", "Select a clip"));
        t.Add(any, fixedkey::Select, TR("keys.edit_clip", "Edit clip"), KeyFill(TR("keys.cap.enter_double_click", "{key_select} / double click")), TR("help.edit_the_clip_trim_cameras", "Edit the clip (trim, cameras)"));
        t.Add(ProjectIsOpen(), fixedkey::AddClip, TR("keys.add_clip", "Add clip"), fixedkey::AddClip, TR("help.add_a_clip_after_it", "Add a clip after it"));
        t.Add(any, "Ctrl+Left Right", TR("keys.move", "Move"), "Ctrl+Left / Ctrl+Right", TR("help.move_it_back_forward", "Move it back / forward"), count > 1);
        t.Add(any && count > 1, TR("keys.cap.drag_clip", "Drag clip"), TR("keys.move", "Move"), TR("keys.cap.drag_a_clip", "Drag a clip"), TR("help.move_it_there", "Move it there"));
        t.Add(any, "X", TR("keys.cut", "Cut"), "X", TR("help.cut_it_at_the_playhead", "Cut it at the playhead"), atPlayhead);
        t.Add(any, "I O", TR("keys.trim", "Trim"), "I / O", TR("help.its_start_end_to_the_playhead", "Its start / end to the playhead"), atPlayhead);
        t.Add(any, "D", TR("keys.duplicate", "Duplicate"), "D", TR("help.duplicate_it", "Duplicate it"));
        t.Add(any, "Del", TR("keys.remove", "Remove"), "Del", TR("help.remove_it", "Remove it"));
        t.Add(any, TR("keys.cap.click_bar", "Click bar"), TR("keys.seek", "Seek"), TR("keys.cap.click_drag_the_bar", "Click / drag the bar"), TR("help.project_time", "Project time"));
        const char* frame = PairText(Action::FrameBack, Action::FrameForward);
        const char* stop = KeyText(Action::Stop);
        const char* editor = KeyText(Action::Editor);
        t.Add(any, frame, TR("keys.frame", "Frame"), frame, TR("help.one_frame", "One frame"), playing);
        t.Add(ProjectIsOpen(), fixedkey::Save, TR("keys.save", "Save"), fixedkey::Save, TR("help.save_the_project", "Save the project"));
        t.Add(ProjectIsOpen(), "R", TR("keys.rename", "Rename"), "R", TR("help.rename_the_project", "Rename the project (its file too)"));
        t.Add(any, fixedkey::Export, TR("keys.export", "Export"), fixedkey::Export, TR("help.export_the_whole_project", "Export the whole project as one video"));
        t.Add(any, stop, TR("keys.stop", "Stop"), stop, TR("help.stop", "Stop"), playing);
        t.Add(true, editor, TR("keys.close", "Close"), editor, TR("help.close_the_editor", "Close the editor"));
        t.Add(true, fixedkey::Help, TR("keys.all_keys", "All keys"));
        t.Add(true, fixedkey::Back, TR("keys.back", "Back"), fixedkey::Back, TR("help.back", "Back"));
    }

    // The export dialog (DrawExport): its keys, from its code. While an export
    // runs only Del (cancel) works; with the export off only Esc. The bar, like
    // the dialog, is drawn after the export's capture point
    // (ExportController::OnOverlay), so it never reaches the video.
    void ExportTips(TipList& t)
    {
        render_export::ExportController& controller = render_export::ExportController::Get();
        if (!controller.Enabled() || !controller.Status().channel)
        {
            t.Add(true, fixedkey::Back, TR("keys.back", "Back"), fixedkey::Back, TR("help.back", "Back"));
            return;
        }
        if (controller.Busy())
        {
            t.Add(true, "Del", TR("keys.cancel_export", "Cancel export"), "Del", TR("help.cancel_the_export_and_the_queue", "Cancel the export (and the queue)"));
            return;
        }
        const int index = exportui::g_index;
        const bool clip = Flag(SnapshotPlaying) || exportui::g_range == 2; // an export needs the clip open (a project opens its own)
        t.Add(true, "Up Down", TR("keys.select", "Select"), "Up Down", TR("help.select_a_setting", "Select a setting"));
        t.Add(index < 10, "Left Right", TR("keys.change", "Change"), "Left Right", TR("help.change_the_setting", "Change the setting"));
        t.Add(true, KeyFill(TR("keys.cap.enter_click", "{key_select} / Click")), index < 10 ? TR("keys.next_value", "Next value") : index == 10 ? TR("keys.export", "Export") : TR("keys.add_to_queue", "Add to queue"), KeyFill(TR("keys.cap.enter_click_help", "{key_select} / click")),
            TR("help.next_value_export", "Next value; on Export now / Add to queue: do it"), index < 10 || clip);
        t.Add(true, "A", TR("keys.add_to_queue", "Add to queue"), "A", TR("help.add_to_the_export_queue", "Add to the export queue"), clip);
        t.Add(true, fixedkey::Help, TR("keys.all_keys", "All keys"));
        if (exportui::g_back == Screen::Project)
            t.Add(true, fixedkey::Back, TR("keys.back", "Back"), fixedkey::Back, TR("help.back_to_the_project", "Back to the project"));
        else
            t.Add(true, fixedkey::Back, TR("keys.back", "Back"), fixedkey::Back, TR("help.back_to_the_clip_editor", "Back to the clip editor"));
    }
}

    // The bar's tips for what is on screen now.
    void ScreenTips(KeyTips& out)
    {
        TipList t{out, false};
        const bool playing = g_hasSnapshot && Flag(SnapshotPlaying);
        const bool debug = g_hasSnapshot && Flag(SnapshotDebugMode);
        if (qa_shots::NoteOpen()) // the note box
        {
            t.Add(true, fixedkey::Select, TR("keys.save_note", "Save note"));
            t.Add(true, fixedkey::Back, TR("keys.no_note", "No note"));
            return;
        }
        if (g_screen == Screen::Closed)
        {
            // Nothing over the game unless a clip plays (its playback bar).
            if (playing && !debug && !UiHidden())
                PlaybackTips(t);
            return;
        }
        if (!g_hasSnapshot)
        {
            t.Add(true, KeyText(Action::Editor), TR("keys.close", "Close"));
            return;
        }
        if (g_screen == Screen::Edit && Flag(SnapshotEditingCamera))
        {
            const CameraKeyframe* key = FindKeyframe(g_snapshot.editingMarker);
            EditCameraTips(t, key && key->lookAtTarget && key->cameraMode == CameraMode::Free);
            return;
        }
        // Hidden, no key bar (H brings it back).
        if (g_screen == Screen::Edit && UiHidden())
            return;
        if (g_helpOpen)
        {
            t.Add(true, JoinText(fixedkey::Help, fixedkey::Back), TR("keys.close_help", "Close help"));
            return;
        }
        switch (g_screen)
        {
        case Screen::Home:
            t.Add(true, "Up Down", TR("keys.select", "Select"));
            t.Add(true, fixedkey::Select, TR("keys.open", "Open"));
            ReportTip(t);
            t.Add(true, KeyText(Action::Editor), TR("keys.close", "Close"));
            t.Add(true, fixedkey::Help, TR("keys.all_keys", "All keys"));
            t.Add(true, fixedkey::Back, playing ? TR("keys.back_to_clip", "Back to clip") : TR("keys.back_to_game", "Back to game"));
            break;
        case Screen::Settings:
            t.Add(true, "Up Down", TR("keys.select", "Select"));
            t.Add(true, "Left Right", TR("keys.change", "Change"));
            t.Add(true, KeyFill(TR("keys.cap.enter_click", "{key_select} / Click")), TR("keys.next_value", "Next value"));
            t.Add(true, fixedkey::Help, TR("keys.all_keys", "All keys"));
            t.Add(true, fixedkey::Back, TR("keys.back", "Back"));
            break;
        case Screen::Gallery:
        {
            const ClipInfo* clip = GalleryClip(g_galleryIndex);
            const bool open = clip && playing && std::strcmp(clip->name, g_snapshot.currentClip) == 0;
            t.Add(g_snapshot.clipTotal > 1, "Up Down", TR("keys.select", "Select"));
            if (g_galleryPicking)
                t.Add(clip != nullptr, fixedkey::Select, TR("keys.add_to_project", "Add to project"), nullptr, nullptr, clip && clip->loadable);
            else
            {
                t.Add(clip != nullptr, fixedkey::Select, open ? TR("keys.edit", "Edit") : TR("keys.play", "Play"), nullptr, nullptr, clip && clip->loadable);
                t.Add(clip != nullptr, "R", TR("keys.rename", "Rename"));
                t.Add(clip != nullptr, "C", TR("keys.compress", "Compress"), nullptr, nullptr, clip && !clip->compressed && !open && clip->loadable);
                t.Add(clip != nullptr, "Del", TR("keys.delete", "Delete"));
            }
            t.Add(true, "Tab", TR("keys.sort", "Sort"));
            t.Add(true, fixedkey::Help, TR("keys.all_keys", "All keys"));
            t.Add(true, fixedkey::Back, g_galleryPicking ? TR("keys.project", "Project") : TR("keys.back", "Back"));
            break;
        }
        case Screen::Projects:
            ProjectsTips(t);
            break;
        case Screen::Project:
            ProjectTips(t);
            break;
        case Screen::Edit:
            EditTips(t, g_markerMenuActive);
            break;
        case Screen::Alert:
            if (g_alert.action != CommandType::None)
            {
                t.Add(true, fixedkey::Select, g_alert.confirmLabel.c_str());
                t.Add(true, fixedkey::Back, TR("keys.cancel", "Cancel"));
            }
            else
                t.Add(true, fixedkey::Select, TR("keys.ok", "OK"));
            break;
        case Screen::Rename:
        case Screen::ProjectName:
            t.Add(true, fixedkey::Select, TR("keys.save", "Save"));
            t.Add(true, fixedkey::Back, TR("keys.cancel", "Cancel"));
            break;
        case Screen::Export:
            ExportTips(t);
            break;
        default:
            break;
        }
    }

namespace
{

    constexpr float kKeyBarRowHeight = 28.0f;
}

    // Where each row of the bar starts in `tips`, filled in order, as many tips
    // a row as fit between the screen's margins (ui_layout_check.h PackRows).
    std::vector<std::size_t> KeyBarRows(const KeyTips& tips)
    {
        std::vector<float> widths(tips.size());
        for (std::size_t n = 0; n < tips.size(); ++n)
            widths[n] = KeyHintWidth(tips[n].key, tips[n].label);
        return replay::editor::uilayout::PackRows(widths, g_vw - 80.0f + 22.0f); // the last tip's gap hangs past the margin
    }

    float KeyBarHeight(std::size_t rows) { return rows ? rows * kKeyBarRowHeight + 14.0f : 0.0f; }

    void DrawKeyBar(ImDrawList* draw, const KeyTips& tips)
    {
        UI_PROBE_SCOPE("KeyBar");
        const std::vector<std::size_t> starts = KeyBarRows(tips);
        g_footH = KeyBarHeight(starts.size());
        if (starts.empty())
            return;
        // Screen units, whatever stage the screen above used.
        const float ox = g_ox, oy = g_oy;
        g_ox = g_oy = 0.0f;
        const float top = g_vh - g_footH;
        draw->AddRectFilled(P(0.0f, top), P(g_vw, g_vh), IM_COL32(8, 7, 6, 170));
        UI_PROBE_BOX("keybar", P(0.0f, top), P(g_vw, g_vh));
        for (std::size_t row = 0; row < starts.size(); ++row)
        {
            const std::size_t end = row + 1 < starts.size() ? starts[row + 1] : tips.size();
            float width = 0.0f;
            for (std::size_t n = starts[row]; n < end; ++n)
                width += KeyHintWidth(tips[n].key, tips[n].label);
            // Right-aligned (the gap after the last tip past the margin).
            float x = std::max(40.0f, g_vw - 40.0f - width + 22.0f);
            const float y = top + 8.0f + row * kKeyBarRowHeight;
            for (std::size_t n = starts[row]; n < end; ++n)
                x += KeyHint(draw, x, y, tips[n].key, tips[n].label, tips[n].on);
        }
        g_ox = ox;
        g_oy = oy;
    }

    // F1: every key of the editor on one panel, the screen shown first. Its rows
    // come from the key bar's lists (EditTips, ProjectTips, EditCameraTips,
    // PlaybackTips); the menus' keys are on their own bar.
    void DrawHelp(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("Help");
        // Laid out from measured text: the rows flow down columns as wide as their
        // longest key and help, as many columns as the screen's height needs; a key
        // or a help too wide for its column wraps; the panel is centred and grows to
        // them. Above 100% Interface scale the text steps down (never below its
        // pixels at 100%) until the columns fit the screen's width.
        struct Section
        {
            const char* title;
            KeyTips keys;
        };
        std::vector<Section> sections;
        const auto add = [&sections](const char* title, const std::function<void(TipList&)>& fill) {
            Section section{title, {}};
            TipList list{section.keys, true};
            fill(list);
            sections.push_back(std::move(section));
        };
        if (g_screen == Screen::Projects)
            add(TR("help.section.projects", "PROJECTS"), [](TipList& t) { ProjectsTips(t); });
        if (g_screen == Screen::Project || g_screen == Screen::Projects)
            add(TR("help.section.project", "PROJECT"), [](TipList& t) { ProjectTips(t); });
        else
            add(TR("help.section.editing", "EDITING A CLIP"), [](TipList& t) { EditTips(t, false); });
        add(TR("help.section.edit_camera", "EDIT CAMERA"), [](TipList& t) { EditCameraTips(t, false); });
        add(TR("help.section.closed", "EDITOR CLOSED"), [](TipList& t) { PlaybackTips(t); });
        // The hide key and where the keys are changed.
        const std::string note = replay::keys::Fill(TR("help.note", "Every screen shows its keys at the foot of the screen. {key_hide} hides every bar while a "
            "clip plays; {key_hide} brings them back. The keys can be changed in RedemptionEditor.ini, section [Keys]."), Keys());

        // Screen units, whatever stage the screen under it used.
        const float ox = g_ox, oy = g_oy;
        g_ox = g_oy = 0.0f;
        const float top = 60.0f;
        const float headerH = 68.0f; // the brush and its title over the rows
        const float footerH = LabelHeight(14.0f) + 8.0f + kPanelPadBottom; // "F1 or Esc closes this help.", clear of the ink band
        const float availH = std::max(200.0f, g_vh - g_footH - 20.0f - top - headerH - footerH - kPanelPad);
        const float availW = g_vw - 80.0f;

        // The rows' text size: 15 units, and above 100% Interface scale (where the
        // screen holds fewer units) a step smaller while the rows do not fit the
        // screen's width, down to the pixels they have at 100% (15 units at 100%,
        // 12 at 125%). The gaps and the key column's cap go with it. At 100% and
        // below the help is laid out as it always was.
        const float sizeFloor = std::min(15.0f, 15.0f / std::max(1.0f, g_uiScaleFactor));
        float size = 15.0f;
        float keyW = 0.0f, columnHelpW = 0.0f, keyGap = 16.0f, columnGap = 40.0f, titleSize = 16.0f, titleH = 26.0f;

        // One entry per title, row and the note, with its height at a help width.
        struct Entry
        {
            int section;
            int row; // -1 the section's title, -2 the note, -3 the title of a section going on from the column before
            float height;
        };
        std::vector<Entry> entries;
        std::vector<std::vector<Entry>> columns;
        float columnW = 0.0f, panelW = 0.0f;
        for (;;)
        {
            const float k = size / 15.0f;
            keyGap = 16.0f * k;
            columnGap = 40.0f * k;
            titleSize = 16.0f * k;
            titleH = 26.0f * k;
            const float maxKeyW = 230.0f * k, minHelpW = 160.0f * k;
            keyW = 0.0f;
            float helpW = 0.0f;
            for (const Section& section : sections)
                for (const KeyTip& tip : section.keys)
                {
                    keyW = std::max(keyW, TextWidth(size, tip.helpKey) / g_scale);
                    helpW = std::max(helpW, TextWidth(size, tip.help) / g_scale);
                }
            keyW = std::min(keyW, maxKeyW);
            columnHelpW = helpW;
            for (int count = 1; count <= 4; ++count)
            {
                columnHelpW = std::max(minHelpW, std::min(helpW, (availW - 2.0f * kPanelPad - static_cast<float>(count - 1) * columnGap) / static_cast<float>(count) - keyW - keyGap));
                entries.clear();
                for (int s = 0; s < static_cast<int>(sections.size()); ++s)
                {
                    entries.push_back({s, -1, titleH});
                    for (int r = 0; r < static_cast<int>(sections[s].keys.size()); ++r)
                    {
                        const KeyTip& tip = sections[s].keys[r];
                        const float h = std::max(TitleWrapped(nullptr, 0.0f, 0.0f, size, 0, tip.helpKey, keyW),
                            TitleWrapped(nullptr, 0.0f, 0.0f, size, 0, tip.help, columnHelpW));
                        entries.push_back({s, r, std::max(22.0f * k, h + 4.0f * k)});
                    }
                    entries.back().height += 12.0f * k; // the gap after a section
                }
                entries.push_back({-1, -2, ParagraphHeight(size, note.c_str(), keyW + keyGap + columnHelpW)});
                // Two packings at this column count: first every section whole (one that
                // does not fit under the last starts the next column, when it fits a
                // column); if that needs more columns, sections split where the column
                // ends, the part in the next column under its title marked (CONT.).
                for (int whole = 1; whole >= 0; --whole)
                {
                    columns.assign(1, {});
                    float used = 0.0f;
                    for (std::size_t n = 0; n < entries.size(); ++n)
                    {
                        // A title stays with its first row (whole: with all its rows).
                        float need = entries[n].height + (entries[n].row == -1 && n + 1 < entries.size() ? entries[n + 1].height : 0.0f);
                        if (whole && entries[n].row == -1)
                        {
                            float sectionH = 0.0f;
                            for (std::size_t m = n; m < entries.size() && entries[m].section == entries[n].section; ++m)
                                sectionH += entries[m].height;
                            if (sectionH <= availH)
                                need = sectionH;
                        }
                        if (used > 0.0f && used + need > availH)
                        {
                            columns.emplace_back();
                            used = 0.0f;
                            // A section going on in the next column: its title again on top, marked.
                            if (entries[n].row >= 0)
                            {
                                columns.back().push_back({entries[n].section, -3, titleH});
                                used += titleH;
                            }
                        }
                        columns.back().push_back(entries[n]);
                        used += entries[n].height;
                    }
                    if (static_cast<int>(columns.size()) <= count)
                        break;
                }
                if (static_cast<int>(columns.size()) <= count)
                    break;
            }
            columnW = keyW + keyGap + columnHelpW;
            const float columnCount = static_cast<float>(columns.size());
            panelW = 2.0f * kPanelPad + columnCount * columnW + (columnCount - 1.0f) * columnGap;
            if (panelW <= availW + 0.5f || size <= sizeFloor)
                break;
            size = std::max(sizeFloor, size - 1.0f);
        }
        float contentH = 0.0f;
        for (const std::vector<Entry>& column : columns)
        {
            float h = 0.0f;
            for (const Entry& entry : column)
                h += entry.height;
            contentH = std::max(contentH, h);
        }
        const float panelH = headerH + contentH + footerH;
        const float x0 = std::max(40.0f, (g_vw - panelW) * 0.5f);
        InkPanel(draw, x0, top, x0 + panelW, top + panelH, IM_COL32(10, 9, 8, 235));
        const char* const title = TR("help.title", "EDITOR KEYS");
        Brush(draw, x0, top + 10.0f, std::max(260.0f, TextWidth(26.0f, title) / g_scale + 40.0f), 40.0f, kRed); // as wide as a long title
        Text(draw, x0 + 16.0f, top + 16.0f, 26.0f, kText, title);
        for (std::size_t c = 0; c < columns.size(); ++c)
        {
            const float x = x0 + kPanelPad + static_cast<float>(c) * (columnW + columnGap);
            float y = top + headerH;
            for (const Entry& entry : columns[c])
            {
                if (entry.row == -2)
                    Paragraph(draw, x, y, size, kMuted, note.c_str(), columnW);
                else if (entry.row == -1)
                    Text(draw, x, y, titleSize, kGold, sections[entry.section].title);
                else if (entry.row == -3)
                    Text(draw, x, y, titleSize, kGold, TRF("help.section.cont", "{title} (CONT.)", {{"title", sections[entry.section].title}}).c_str());
                else
                {
                    const KeyTip& tip = sections[entry.section].keys[entry.row];
                    TitleWrapped(draw, x, y, size, kText, tip.helpKey, keyW);
                    TitleWrapped(draw, x + keyW + keyGap, y, size, kMuted, tip.help, columnHelpW);
                }
                y += entry.height;
            }
        }
        Text(draw, x0 + kPanelPad, top + panelH - footerH + 8.0f, 14.0f, kMuted, KeyFill(TR("help.close", "{key_help} or {key_back} closes this help.")));
        g_ox = ox;
        g_oy = oy;
    }
