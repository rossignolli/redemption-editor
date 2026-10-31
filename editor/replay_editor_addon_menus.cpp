// replay_editor_addon_menus.cpp: the home screen, the clip load and opening, the notices (recording,
// world, QA, crash, storage), the clip gallery, the rename and QA note dialogs.
// Split from replay_editor_addon.cpp (moved code); shared internals in replay_editor_addon_internal.h.

#include "replay_editor_addon_internal.h"

    void DrawHome(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("Home");
        // The pause menu's layout: a dark column on the left edge, the title on a
        // red brush, the items in light text with the selected one on the brush, and
        // a description panel beside them.
        const float columnWidth = 540.0f;
        // Solid under the items, then fading out into the game (no hard edge).
        draw->AddRectFilled(P(0.0f, 0.0f), P(columnWidth - 60.0f, g_vh), IM_COL32(10, 9, 8, 225));
        draw->AddRectFilledMultiColor(P(columnWidth - 60.0f, 0.0f), P(columnWidth + 220.0f, g_vh), IM_COL32(10, 9, 8, 225),
            IM_COL32(10, 9, 8, 0), IM_COL32(10, 9, 8, 0), IM_COL32(10, 9, 8, 225));
        const float top = std::max(60.0f, g_vh * 0.16f);
        TitleBanner(draw, 50.0f, top, 420.0f, 70.0f, 62.0f, 72.0f, top + 10.0f, 44.0f, "REDEMPTION EDITOR");
        OrnamentDivider(draw, 60.0f, top + 104.0f, 400.0f);

        // The items by what they do (a project adds Close Project).
        enum class Do
        {
            Edit,
            Projects,
            Clips,
            Record,
            SaveBuffer,
            Settings,
            CloseProject,
            Back
        };
        struct Item
        {
            Do what;
            const char* label;
            bool enabled;
            ui_art::Art icon = ui_art::Art::Count; // none
        };
        // With a clip loaded the first item edits it (the edit screen's Esc
        // comes back here), and Back to Game closes the clip.
        // With a project open it edits the project; without one and no clip, it
        // starts a new project.
        const bool clipOpen = Flag(SnapshotPlaying);
        const bool projectOpen = ProjectIsOpen();
        // The always-on replay buffer (RedemptionEditor.ini ReplayBuffer=1) has two
        // modes: the ring of the last seconds, saved on demand, and a manual
        // recording marked inside it.
        const bool buffer = Flag(SnapshotBufferRunning);
        const bool manual = Flag(SnapshotBufferManual);
        // A recording started with F4 (the buffer off) shows the item as a stop.
        const bool recordingAlone = !buffer && Flag(SnapshotRecording);
        std::vector<Item> items;
        items.push_back({Do::Clips, TR("home.recorded_clips", "Recorded Clips"), true, ui_art::Art::MenuClips});
        items.push_back({Do::Edit, projectOpen ? TR("home.edit_project", "Edit Project") : clipOpen ? TR("home.edit_clip", "Edit Clip") : TR("home.new_project", "New Project"), true,
            projectOpen ? ui_art::Art::MenuEditProject : clipOpen ? ui_art::Art::MenuEditClip : ui_art::Art::MenuNewProject});
        items.push_back({Do::Projects, TR("home.projects", "Projects"), true, ui_art::Art::MenuProjects});
        items.push_back({Do::Record, buffer ? (manual ? TR("home.stop_recording", "Stop Recording") : TR("home.start_recording", "Start Recording")) : recordingAlone ? TR("home.stop_recording", "Stop Recording") : TR("home.record_now", "Record Now"), true, ui_art::Art::MenuRecord});
        // Save Replay Buffer only while the buffer runs and holds something to
        // save (src/editor_ui_qa.h): hidden, Up and Down skip it and the items
        // below it move up.
        if (replay::editor::uiqa::SaveBufferShown(buffer, g_snapshot.bufferHeldMs))
            items.push_back({Do::SaveBuffer, TR("home.save_buffer", "Save Replay Buffer"), true, ui_art::Art::MenuSaveBuffer});
        items.push_back({Do::Settings, TR("home.replay_settings", "Replay Settings"), true, ui_art::Art::MenuSettings});
        if (projectOpen)
            items.push_back({Do::CloseProject, TR("home.close_project", "Close Project"), true, ui_art::Art::MenuCloseProject});
        // Last, under the boxed rows: Back to Game (a plain line, not a box).
        items.push_back({Do::Back, TR("home.back_to_game", "Back to Game"), true});
        const int count = static_cast<int>(items.size());
        // The selection keeps its item when Close Project or Save Replay Buffer
        // comes or goes (the item after a vanished one takes its place).
        static std::vector<int> seenItems;
        std::vector<int> itemIds;
        for (const Item& item : items)
            itemIds.push_back(static_cast<int>(item.what));
        if (!seenItems.empty() && seenItems != itemIds)
            g_homeIndex = replay::editor::uiqa::KeepSelection(seenItems, g_homeIndex, itemIds);
        seenItems = itemIds;
        g_homeIndex = std::clamp(g_homeIndex, 0, count - 1);
        NavigateList(g_homeIndex, count);
        int activated = -1;
        const float itemsTop = top + 126.0f;
        // Painted, the items' icons in a column before the labels.
        const float iconLead = AnyArt({ui_art::Art::MenuNewProject, ui_art::Art::MenuProjects, ui_art::Art::MenuClips, ui_art::Art::MenuRecord,
                                   ui_art::Art::MenuSaveBuffer, ui_art::Art::MenuSettings}) ? 40.0f : 0.0f;
        // Every item but the last in its row box, as the pause menu's settings list.
        const int boxed = count - 1;
        for (int n = 0; n < boxed; ++n)
        {
            const float y = itemsTop + n * 50.0f;
            bool clicked = false, pointed = false;
            ImGui::PushID(n);
            const bool doubled = ListItem(draw, "##home", 80.0f, y, 380.0f, 44.0f, n == g_homeIndex, clicked, &pointed);
            ImGui::PopID();
            if (clicked || pointed)
                g_homeIndex = n;
            if (doubled || (clicked && items[n].enabled))
                activated = n;
            const ImU32 color = !items[n].enabled ? IM_COL32(236, 230, 218, 70) : kText;
            MenuIcon(draw, items[n].icon, 88.0f + 14.0f, y + 22.0f, 28.0f, n == g_homeIndex, items[n].enabled);
            Text(draw, 88.0f + iconLead, y + 8.0f, 30.0f, color, items[n].label);
        }
        {
            // Back to Game: a plain line below the boxes, set apart; selected,
            // a red ink stroke under it (grey under the mouse). Selected and
            // clicked as a row.
            const int n = boxed;
            const char* const label = items[n].label;
            const float size = 24.0f;
            const float x = 66.0f, y = itemsTop + boxed * 50.0f + 16.0f;
            const float width = std::min(394.0f, TextWidth(size, label) / g_scale);
            const float height = LabelHeight(size);
            ImGui::SetCursorScreenPos(P(x - 6.0f, y - 4.0f));
            ImGui::InvisibleButton("##homeback", ImVec2(S(width + 12.0f), S(height + 12.0f)));
            const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left) || (ImGui::IsItemHovered() && MissedClick());
            const bool pointed = !clicked && n != g_homeIndex && ImGui::IsItemHovered() && g_menuPointer.Moved();
            const bool hovered = HoverShown(ImGui::IsItemHovered());
            if ((clicked || pointed) && n != g_homeIndex)
                PlayUi(UiSound::NavDown);
            if (clicked || pointed)
                g_homeIndex = n;
            if (clicked)
                activated = n;
            const bool selected = n == g_homeIndex;
            if (selected || hovered)
                draw->AddRectFilled(P(x, y + height + 2.0f), P(x + width, y + height + 4.5f), selected ? kRed : kRowInkGrey);
            TextFit(draw, x, y, size, selected || hovered ? kText : IM_COL32(236, 230, 218, 190), label, 394.0f);
        }
        if (Confirmed())
            activated = g_homeIndex;

        // What the selected item does, beside the items.
        const char* heading = items[g_homeIndex].label;
        const char* body = "";
        char detail[160]{};
        switch (items[g_homeIndex].what)
        {
        case Do::Edit:
            if (projectOpen)
            {
                body = TR("home.edit_project.body", "Back to the open project: its clips in order, cut, moved, trimmed and played as one video.");
                char total[16]{};
                FormatClock(g_snapshot.projectTotalMs, total, sizeof(total), false);
                std::snprintf(detail, sizeof(detail), "%s", TRF("home.project.detail", "{name}  -  {count} clips  -  {length}{unsaved}",
                    {{"name", g_snapshot.projectName}, {"count", std::to_string(g_snapshot.projectClipCount)}, {"length", total},
                        {"unsaved", ProjectFlag(ProjectDirty) ? TR("home.project.unsaved", "  -  unsaved") : ""}}).c_str());
            }
            else
                body = clipOpen ? TR("home.edit_clip.body", "Back to the clip that is open: cameras, speed and the timeline.")
                                : TR("home.new_project.body", "Start a new project: put clips in order, cut and trim them, and play them as one video.");
            break;
        case Do::CloseProject:
            body = TR("home.close_project.body", "Close the project. Its changes are saved first; it stays in Projects.");
            break;
        case Do::Projects:
            body = TR("home.projects.body", "Open, rename or delete your projects. Each one is saved to the ReplayProjects folder.");
            std::snprintf(detail, sizeof(detail), "%s", TRF("home.projects.count", "{count} projects", {{"count", std::to_string(g_snapshot.projectListCount)}}).c_str());
            break;
        case Do::Clips:
        {
            body = TR("home.clips.body", "Browse, play and delete your recorded clips. Every recording is saved to the ReplayClips folder.");
            if (!ClipListKnown())
            {
                std::snprintf(detail, sizeof(detail), "%s", TR("home.clips.reading", "Reading clips..."));
                break;
            }
            std::uint64_t bytes = 0;
            bytes = g_snapshot.clipsDiskBytes; // the whole folder (the list is a window)
            char size[32]{};
            FormatSize(bytes, size, sizeof(size));
            std::snprintf(detail, sizeof(detail), "%s", TRF("home.clips.count", "{count} clips  -  {size} on disk",
                {{"count", std::to_string(g_snapshot.clipTotal)}, {"size", size}}).c_str());
            break;
        }
        case Do::Record:
            if (buffer)
                body = KeyFill(manual ? TR("home.record.mark_saves.body", "Save everything from the mark to now as a clip ({key_record}). The replay buffer keeps running.")
                                      : TR("home.record.mark.body", "Mark the start of a recording ({key_record}); the second {key_record} saves from the mark. Long recordings are saved in parts."));
            else if (recordingAlone)
                body = KeyFill(TR("home.record.stop.body", "Stop the recording and save it as a clip ({key_record})."));
            else
                body = KeyFill(clipOpen ? TR("home.record.close_clip.body", "Close the open clip and start recording now. Stop with {key_record}; long recordings are saved in parts.")
                                        : TR("home.record.now.body", "Start recording now. Stop with {key_record}; the clip is saved when it stops. Long recordings are saved in parts."));
            break;
        case Do::SaveBuffer:
            if (buffer)
            {
                body = KeyFill(TR("home.save_buffer.body", "Save what the replay buffer holds as a clip ({key_save_buffer}). Recording goes on in the background."));
                // The real window against the length set (busy places fill the memory
                // before it).
                const std::uint32_t held = g_snapshot.bufferHeldMs / 1000u;
                if (g_snapshot.bufferSeconds && held + 2u < g_snapshot.bufferSeconds)
                    std::snprintf(detail, sizeof(detail), "%s", TRF("home.buffer.holds_of", "The buffer holds {held} of {seconds} seconds (memory {mb} MB)",
                        {{"held", std::to_string(held)}, {"seconds", std::to_string(g_snapshot.bufferSeconds)},
                            {"mb", std::to_string(g_snapshot.bufferMB)}}).c_str());
                else
                    std::snprintf(detail, sizeof(detail), "%s", TRF("home.buffer.holds", "The buffer holds {held} seconds", {{"held", std::to_string(held)}}).c_str());
            }
            else
                body = TR("home.buffer_off.body", "The replay buffer is off. Turn it on in Replay Settings.");
            break;
        case Do::Settings:
            body = KeyFill(TR("home.settings.body", "The replay buffer: the game records all the time and you save the last seconds ({key_save_buffer}). Turn it on and set its length and memory."));
            std::snprintf(detail, sizeof(detail), "%s", TRF("home.settings.detail", "Buffer {state}  -  {seconds} s  -  {mb} MB",
                {{"state", g_snapshot.bufferEnabled ? TR("common.on", "on") : TR("common.off", "off")},
                    {"seconds", std::to_string(g_snapshot.bufferSeconds)}, {"mb", std::to_string(g_snapshot.bufferMB)}}).c_str());
            break;
        default: body = clipOpen ? TR("home.back.close_clip.body", "Close the clip and the editor and return to the game.") : TR("home.back.body", "Close the editor and return to the game."); break;
        }
        const float panelX = columnWidth + 30.0f;
        const float panelTop = itemsTop - 6.0f;
        DescriptionPanel(draw, panelX, panelTop, 200.0f, heading, body, detail, nullptr, // measured
            items[g_homeIndex].what == Do::Clips && !ClipListKnown());

        if (Back())
        {
            PlayUi(UiSound::Back);
            g_screen = clipOpen ? PlaybackScreen() : Screen::Closed;
            return;
        }
        if (activated >= 0 && !items[activated].enabled)
            PlayUi(UiSound::Error);
        if (activated < 0 || !items[activated].enabled)
            return;
        PlayUi(UiSound::Select);
        switch (items[activated].what)
        {
        case Do::Edit:
            if (projectOpen)
                g_screen = Screen::Project;
            else if (clipOpen)
                g_screen = Screen::Edit;
            else
            {
                // A new project, named "Project N" (R renames it in Projects).
                SendNamed(CommandType::ProjectNew, "");
                g_projectOpenAsked = GetTickCount64();
                g_screen = Screen::Project;
            }
            break;
        case Do::CloseProject:
            Send(CommandType::ProjectClose);
            break;
        case Do::Projects:
            Send(CommandType::RefreshProjects);
            g_screen = Screen::Projects;
            break;
        case Do::Clips:
            g_galleryPicking = false;
            RefreshClipList();
            g_screen = Screen::Gallery;
            break;
        case Do::Record:
            Send(CommandType::ToggleRecording);
            g_screen = Screen::Closed;
            break;
        case Do::SaveBuffer:
            Send(CommandType::SaveBuffer);
            g_screen = Screen::Closed;
            break;
        case Do::Settings:
            g_screen = Screen::Settings;
            break;
        case Do::Back:
            if (clipOpen)
                Send(CommandType::Stop);
            g_screen = Screen::Closed;
            break;
        default:
            break;
        }
    }

    // The clip's fades: the first marker's Fade In and the last marker's Fade
    // Out, a black layer over the picture during playback.
    void DrawClipFade(ImDrawList* draw)
    {
        if (!g_hasSnapshot || !Flag(SnapshotPlaying) || g_snapshot.cameraKeyframeCount < 2)
            return;
        const CameraKeyframe& first = g_snapshot.cameraKeyframes[0];
        const CameraKeyframe& last = g_snapshot.cameraKeyframes[g_snapshot.cameraKeyframeCount - 1];
        const double t = static_cast<double>(g_snapshot.playheadMs);
        float alpha = 0.0f;
        if (first.transition && first.transitionMs && t < first.timeMs + first.transitionMs)
            alpha = std::max(alpha, 1.0f - static_cast<float>((t - first.timeMs) / first.transitionMs));
        if (last.transition && last.transitionMs && t > static_cast<double>(last.timeMs) - last.transitionMs)
            alpha = std::max(alpha, static_cast<float>((t - (static_cast<double>(last.timeMs) - last.transitionMs)) / last.transitionMs));
        alpha = std::clamp(alpha, 0.0f, 1.0f);
        if (alpha > 0.0f)
            draw->AddRectFilled(ImVec2(0.0f, 0.0f), ImGui::GetIO().DisplaySize, IM_COL32(0, 0, 0, static_cast<int>(alpha * 255.0f)));
    }

    // ---------------- Recording notices ----------------
    // Shown when the replay is recording, when a clip is being saved and when it
    // was saved. Top right, small, in the pause menu's style; hidden while a clip
    // plays and in the pause menu.
    ULONGLONG g_toastUntil = 0;
    std::uint32_t g_toastClipMs = 0;

    // Opening a clip. The load is sent two frames after the gallery's Enter, so
    // the "OPENING CLIP" notice is on screen before the core's synchronous read;
    // with unsaved changes the core first waits for their save on its thread
    // (SnapshotOpeningClip) instead of freezing the game, and the notice says
    // "SAVING CHANGES".
    std::string g_loadName;
    int g_loadWaitFrames = 0;
    bool g_openingAsked = false;

namespace
{
    std::uint32_t g_openingSeq = 0;
    ULONGLONG g_openingAt = 0;
}
    // A load that saves the recording first says "SAVING RECORDING" instead of
    // "SAVING CHANGES".
    bool g_openingSavesRecording = false;

    void RequestLoad(const std::string& name)
    {
        g_loadName = name;
        g_loadWaitFrames = 1;
        g_openingAsked = true;
        g_openingSeq = g_snapshot.clipLoadSequence;
        g_openingAt = GetTickCount64();
        g_openingSavesRecording = false;
    }

    void SendRequestedLoad()
    {
        if (g_loadName.empty() || g_loadWaitFrames-- > 0)
            return;
        SendNamed(CommandType::LoadClip, g_loadName.c_str());
        g_loadName.clear();
    }

namespace
{

    bool OpeningVisible()
    {
        if (!g_loadName.empty() || (g_hasSnapshot && Flag(SnapshotOpeningClip)))
            return true;
        if (!g_openingAsked)
            return false;
        if (!g_hasSnapshot || g_snapshot.clipLoadSequence != g_openingSeq || Flag(SnapshotLoadFailed) ||
            GetTickCount64() - g_openingAt > 20000)
        {
            g_openingAsked = false;
            return false;
        }
        return true;
    }

    // Where a recording notice goes this frame.
    enum class NoticeSink
    {
        None,
        Frame,
        Window
    };

    struct RecordingRows
    {
        NoticeSink paused = NoticeSink::None;    // "Recording paused: ..."
        NoticeSink indicator = NoticeSink::None; // REC and its clock, or REPLAY
        NoticeSink saving = NoticeSink::None;    // SAVING CLIP or CLIP SAVED
        recind::Kind kind = recind::Kind::None;  // the indicator's row
        std::uint32_t ms = 0;                    // its clock
        bool live = true;                        // false: brief, a stopped recording's row (its clock held)
    };

    RecordingRows PlanRecordingRows()
    {
        RecordingRows rows;
        const recind::Mode mode = g_recChoice.mode;
        const NoticeSink sink = mode == recind::Mode::Hidden ? NoticeSink::Window : mode == recind::Mode::Off ? NoticeSink::None : NoticeSink::Frame;
        if (Flag(SnapshotRecording) && g_snapshot.recordPauseReasons)
            rows.paused = sink;
        if (mode == recind::Mode::Brief)
        {
            if (g_recBrief.Visible(GetTickCount64()))
            {
                rows.kind = g_recBrief.Shown();
                rows.live = !g_recBrief.Stopped();
                rows.ms = rows.live ? g_snapshot.recordingMs : g_recBrief.endedMs;
                rows.indicator = NoticeSink::Frame;
            }
        }
        else if (sink != NoticeSink::None)
        {
            rows.kind = IndicatorKind();
            // Normal: the buffer's row only for a few seconds when it starts.
            if (mode == recind::Mode::Normal && rows.kind == recind::Kind::Replay && !g_replayOn.Visible(GetTickCount64()))
                rows.kind = recind::Kind::None;
            rows.ms = g_snapshot.recordingMs;
            rows.indicator = sink;
        }
        if (Flag(SnapshotSavingClip) || GetTickCount64() < g_toastUntil)
            rows.saving = mode == recind::Mode::Hidden ? NoticeSink::Window : NoticeSink::Frame;
        return rows;
    }
}

    bool RecordingNoticeVisible()
    {
        if (OpeningVisible())
            return true;
        if (!g_hasSnapshot || Flag(SnapshotPlaying) || Flag(SnapshotGamePaused))
            return false;
        const RecordingRows rows = PlanRecordingRows();
        return rows.paused != NoticeSink::None || (rows.indicator != NoticeSink::None && rows.kind != recind::Kind::None) ||
            rows.saving != NoticeSink::None;
    }

namespace
{

    // A text as the title font draws it (capitals), for the indicator's window.
    std::wstring WideCapitals(const char* text)
    {
        std::wstring out;
        for (const char* p = text; p && *p;)
        {
            std::uint32_t c = replay::i18n::ToUpper(replay::i18n::Decode(p));
            if (c >= 0x10000)
            {
                c -= 0x10000;
                out.push_back(static_cast<wchar_t>(0xD800 + (c >> 10)));
                out.push_back(static_cast<wchar_t>(0xDC00 + (c & 0x3FF)));
            }
            else
                out.push_back(static_cast<wchar_t>(c));
        }
        return out;
    }

    // `busy`: the value is a text spinner, drawn as the turning cylinder when
    // its art is there.
    void Notice(ImDrawList* draw, float y, ImU32 dot, bool pulse, const char* label, const char* value, float x = 40.0f, bool busy = false)
    {
        UI_PROBE_SCOPE("Notice");
        constexpr float kCue = 26.0f;
        const bool cue = busy && ui_art::Get(ui_art::Art::Loading);
        if (cue)
            value = "";
        // Wider when the label needs it ("Recording paused: ...").
        const float needed = 54.0f + TextWidth(22.0f, label) / g_scale +
            (value && value[0] ? TextWidth(22.0f, value) / g_scale + 28.0f : cue ? kCue + 28.0f : 0.0f);
        const float width = std::max(250.0f, needed);
        // x: bottom left by default; beside the edit panel there.
        InkPanel(draw, x, y, x + width, y + 40.0f, IM_COL32(10, 9, 8, 200), 6.0f); // a thin ink edge
        float alpha = 1.0f;
        if (pulse)
            alpha = 0.55f + 0.45f * std::sin(static_cast<float>(GetTickCount64() % 1200) / 1200.0f * 6.2832f);
        const ImU32 color = (dot & 0x00FFFFFFu) | (static_cast<ImU32>(alpha * 255.0f) << 24);
        draw->AddCircleFilled(P(x + 22.0f, y + 20.0f), S(7.0f), color, 20);
        Text(draw, x + 40.0f, y + 8.0f, 22.0f, kText, label);
        if (value && value[0])
            Text(draw, x + width - TextWidth(22.0f, value) / g_scale - 14.0f, y + 8.0f, 22.0f, kMuted, value);
        if (cue)
            LoadingCue(draw, x + width - 14.0f - kCue * 0.5f, y + 20.0f, kCue);
    }

    // One recording notice in the game frame or in the indicator's window
    // (each moves the stack up), or none.
    void RecordingRow(ImDrawList* draw, NoticeSink sink, float& y, ImU32 dot, bool pulse, const char* label, const char* value, bool busy = false)
    {
        if (sink == NoticeSink::Frame)
            Notice(draw, y, dot, pulse, label, value, 40.0f, busy);
        else if (sink == NoticeSink::Window)
        {
            rec_window::Row row;
            row.label = WideCapitals(label);
            row.value = busy ? std::wstring() : WideCapitals(value);
            row.spinner = busy;
            row.dot = ((dot & 0xFFu) << 16) | (dot & 0xFF00u) | ((dot >> 16) & 0xFFu); // IM_COL32 is 0xAABBGGRR
            row.pulse = pulse;
            row.y = y;
            g_recRows.x = 40.0f;
            g_recRows.uiScale = g_uiScaleFactor;
            g_recRows.rows.push_back(row);
        }
        else
            return;
        y -= 48.0f;
    }
}

    // ---------------- The loading cue ----------------
    // The visual identity's loading cue (the art brief:
    // "a small spinning emblem bottom right with a caps label"): the turning
    // cylinder after its caption on an ink plate, bottom right above the key
    // bar (and above the playback bar when that shows). It replaces the core's
    // plain game text "Loading replay..." (the core leaves it out while the
    // editor says it draws the cue, CommandType::SetLoadingCue) and the far
    // seek's dots, which stood alone at the right edge.

    // A seek waits on the world with a cue: the far jump's frozen picture, or a
    // near settle past seek_freeze's threshold (as its spinner was).
    bool SeekCueWanted()
    {
        if (!g_hasSnapshot || !Flag(SnapshotPlaying))
            return false;
        return g_snapshot.seekLoadState == SeekLoadFrozen ||
            (g_snapshot.seekLoadState == SeekLoadSettling && g_snapshot.seekLoadElapsedMs >= seek_freeze::kSettlingSpinnerMs);
    }

    // The playback bars' state "Loading" with its cylinder: the puppets built,
    // a far jump, a seek waiting (not the world's load behind the black: the
    // badge says that one).
    bool PlaybackBusy() { return g_hasSnapshot && (Flag(SnapshotPreparing) || SeekCueWanted()) && !Flag(SnapshotLoadingWorld); }

namespace
{

    // The playback bar the editor draws with the editor closed (DrawPlaybackHud).
    bool PlaybackBarShown() { return g_screen == Screen::Closed && Flag(SnapshotPlaying) && !Flag(SnapshotDebugMode) && !UiHidden(); }
}

    // The caption of the badge this frame, or nullptr. The world's load: on
    // every screen but the clip editor (its status line has the cue), and over
    // a hidden interface too (as the core's text was). A seek's wait: only
    // where no playback bar carries it (Debug=1's text HUD, the editor closed).
    const char* LoadingBadgeCaption()
    {
        if (!g_hasSnapshot)
            return nullptr;
        if (Flag(SnapshotLoadingWorld) && (g_screen != Screen::Edit || UiHidden()))
            return Flag(SnapshotPlaying) ? TR("hud.loading_replay", "Loading replay...") : TR("hud.loading_world", "Loading...");
        if (SeekCueWanted() && g_screen == Screen::Closed && Flag(SnapshotDebugMode) && !UiHidden())
            return TR("hud.loading", "Loading");
        return nullptr;
    }

    void DrawLoadingBadge(ImDrawList* draw)
    {
        const char* caption = LoadingBadgeCaption();
        if (!caption)
            return;
        UI_PROBE_SCOPE("LoadingBadge");
        // Screen units whatever stage the screen under it uses.
        const float ox = g_ox, oy = g_oy;
        g_ox = g_oy = 0.0f;
        constexpr float kCue = 30.0f, kH = 44.0f, kPad = 18.0f, kGap = 12.0f;
        const bool cue = ui_art::Get(ui_art::Art::Loading) != 0;
        const float labelW = TextWidth(22.0f, caption) / g_scale;
        const float width = kPad + labelW + (cue ? kGap + kCue : 0.0f) + kPad;
        const float x1 = g_vw - 40.0f;
        // Above the key bar; above the playback bar and its project line too.
        const float bottom = g_vh - g_footH - (PlaybackBarShown() ? 100.0f : 24.0f);
        const float x0 = x1 - width, y0 = bottom - kH;
        InkPanel(draw, x0, y0, x1, bottom, IM_COL32(10, 9, 8, 210), 6.0f);
        Text(draw, x0 + kPad, y0 + (kH - LabelHeight(22.0f)) * 0.5f, 22.0f, kText, caption);
        if (cue)
            LoadingCue(draw, x1 - kPad - kCue * 0.5f, y0 + kH * 0.5f, kCue);
        g_ox = ox;
        g_oy = oy;
    }

    void DrawRecordingNotices(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("RecordingNotices");
        if (!RecordingNoticeVisible())
            return;
        // Bottom left, stacked upwards, above the game's cores.
        float y = g_vh - 160.0f - g_footH; // above the key bar
        static const char* kSpinner[] = {"|", "/", "-", "\\"};
        const bool opening = OpeningVisible();
        if (opening)
        {
            Notice(draw, y, IM_COL32(236, 230, 218, 255), true, Flag(SnapshotSavingClip) ? (g_openingSavesRecording ? TR("notice.saving_recording", "SAVING RECORDING") : TR("notice.saving_changes", "SAVING CHANGES")) : TR("notice.opening_clip", "OPENING CLIP"),
                kSpinner[(GetTickCount64() / 150) % 4], 40.0f, true);
            y -= 48.0f;
            if (Flag(SnapshotPlaying) || Flag(SnapshotGamePaused))
                return; // the clip left still shows: only this notice
        }
        // The recording indicator's mode: each row in the frame, in the window
        // or not shown (normal: all in the frame).
        const RecordingRows rows = PlanRecordingRows();
        char clock[32]{};
        // The recording gate holds (cutscene, fade, loading).
        const std::uint32_t gate = g_snapshot.recordPauseReasons;
        if (rows.paused != NoticeSink::None)
        {
            const char* why = (gate & 1u) ? TR("notice.paused.menu", "menu") : (gate & 8u) ? TR("notice.paused.loading", "loading")
                : (gate & 4u) ? TR("notice.paused.fade", "fade") : TR("notice.paused.cutscene", "cutscene");
            const std::string label = TRF("notice.recording_paused", "Recording paused: {why}", {{"why", why}});
            RecordingRow(draw, rows.paused, y, IM_COL32(236, 190, 70, 255), false, label.c_str(), "");
        }
        if (rows.kind == recind::Kind::Rec)
        {
            FormatClock(rows.ms, clock, sizeof(clock), false);
            RecordingRow(draw, rows.indicator, y, IM_COL32(200, 30, 30, 255), rows.live, TR("notice.rec", "REC"), clock);
        }
        else if (rows.kind == recind::Kind::Replay)
        {
            // No seconds (the held window only wobbles between 60 and 61 once full):
            // the tag alone says the buffer runs.
            RecordingRow(draw, rows.indicator, y, IM_COL32(170, 60, 50, 255), false, TR("notice.replay_on", "REPLAY ON"), "");
        }
        if (rows.saving != NoticeSink::None && Flag(SnapshotSavingClip) && !opening)
        {
            static const char* kSpin[] = {"|", "/", "-", "\\"};
            RecordingRow(draw, rows.saving, y, IM_COL32(236, 230, 218, 255), true, TR("notice.saving_clip", "SAVING CLIP"),
                kSpin[(GetTickCount64() / 150) % 4], true);
        }
        else if (rows.saving != NoticeSink::None && GetTickCount64() < g_toastUntil)
        {
            FormatClock(g_toastClipMs, clock, sizeof(clock), false);
            RecordingRow(draw, rows.saving, y, IM_COL32(120, 170, 90, 255), false, TR("notice.clip_saved", "CLIP SAVED"), clock);
        }
    }

namespace
{

    // ---------------- World notice ----------------
    // The clip was recorded in another chapter or its area's map state
    // differs from the live world: a notice for 8 s once the clip shows
    // (after the load wait), top left, in the recording notices' style.
    std::uint32_t g_worldNoticeSeen = 0;
    bool g_worldNoticeInit = false;
}
    bool g_worldNoticeArmed = false;
    ULONGLONG g_worldNoticeUntil = 0;
    char g_worldNoticeText[512] = {};

    void UpdateWorldNotice()
    {
        if (!g_hasSnapshot)
            return;
        if (!g_worldNoticeInit)
        {
            g_worldNoticeInit = true;
            g_worldNoticeSeen = g_snapshot.worldNoticeSequence;
            return;
        }
        if (g_snapshot.worldNoticeSequence != g_worldNoticeSeen)
        {
            g_worldNoticeSeen = g_snapshot.worldNoticeSequence;
            g_worldNoticeArmed = g_snapshot.worldNoticeKind != replay::world::kNoticeNone &&
                replay::world::NoticeText(static_cast<std::uint8_t>(g_snapshot.worldChapterRecorded),
                    static_cast<std::uint8_t>(g_snapshot.worldChapterNow), g_snapshot.worldDifferences,
                    g_snapshot.worldNoticeKind == replay::world::kNoticeRestored, g_worldNoticeText,
                    sizeof(g_worldNoticeText));
        }
        if (!Flag(SnapshotPlaying))
        {
            g_worldNoticeArmed = false;
            g_worldNoticeUntil = 0;
            return;
        }
        if (g_worldNoticeArmed && !Flag(SnapshotPreparing))
        {
            g_worldNoticeArmed = false;
            g_worldNoticeUntil = GetTickCount64() + 8000;
        }
    }

    bool WorldNoticeVisible()
    {
        return g_hasSnapshot && Flag(SnapshotPlaying) && GetTickCount64() < g_worldNoticeUntil;
    }

    void DrawWorldNotice(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("WorldNotice");
        if (!WorldNoticeVisible())
            return;
        // Right of the edit panel while it shows (so it does not cover the panel's
        // title).
        const float x = g_screen == Screen::Edit && !UiHidden() ? 390.0f : 40.0f;
        Notice(draw, 40.0f, IM_COL32(236, 190, 70, 255), false, g_worldNoticeText, "", x);
    }

    // ---------------- QA notices ----------------
    // The core's notices are the game's objective text, hidden while a replay
    // plays, so the add-on confirms each mark (when its note box closes) and
    // says once when a playback stops with marks waiting. Top right, in the
    // recording notices' style; drawn with the UI hidden too (marking is a QA
    // action) and never during an export (the add-on draws after the export's
    // capture point anyway).
    char g_qaNoticeText[512] = {};
    ULONGLONG g_qaNoticeUntil = 0;

namespace
{
    replay::editor::uiqa::StopReminder g_qaStopReminder;

    void ShowQaNotice(const char* text, ULONGLONG ms)
    {
        std::snprintf(g_qaNoticeText, sizeof(g_qaNoticeText), "%s", text);
        g_qaNoticeUntil = GetTickCount64() + ms;
    }
}

    bool QaNoticeVisible() { return GetTickCount64() < g_qaNoticeUntil && !render_export::ExportController::Get().Busy(); }

namespace
{

    void ShowMarkNotice()
    {
        char text[512];
        replay::editor::uiqa::FormatMarkNotice(text, sizeof(text), qa_shots::PendingMarks(), Keys());
        ShowQaNotice(text, 6000);
    }
}

    void UpdateQaNotices(bool playing)
    {
        const std::uint32_t pending = qa_shots::PendingMarks();
        if (g_qaStopReminder.Update(GetTickCount64(), playing, pending))
        {
            char text[512];
            replay::editor::uiqa::FormatStopReminder(text, sizeof(text), pending, Keys());
            ShowQaNotice(text, 8000);
        }
    }

    void DrawQaNotice(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("QaNotice");
        if (!QaNoticeVisible())
            return;
        const float width = std::max(250.0f, 54.0f + TextWidth(22.0f, g_qaNoticeText) / g_scale);
        Notice(draw, 40.0f, IM_COL32(236, 190, 70, 255), false, g_qaNoticeText, "", std::max(40.0f, g_vw - 40.0f - width));
    }
    CrashNoticeState g_crashNotice;

    bool CrashNoticeVisible() { return GetTickCount64() < g_crashNotice.until && !render_export::ExportController::Get().Busy(); }

namespace
{

    std::string Utf8(const wchar_t* text)
    {
        char out[MAX_PATH * 3]{};
        WideCharToMultiByte(CP_UTF8, 0, text, -1, out, static_cast<int>(sizeof(out)) - 1, nullptr, nullptr);
        return out;
    }
}

    void CheckCrashNotice()
    {
        if (g_crashNotice.checked || !g_hasSnapshot)
            return;
        g_crashNotice.checked = true;
        wchar_t local[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        if (length == 0 || length >= MAX_PATH - 64)
            return;
        const std::wstring root = std::wstring(local) + L"\\RedemptionEditor";
        std::vector<replay::crashnotice::Dump> dumps;
        WIN32_FIND_DATAW found{};
        HANDLE find = FindFirstFileW((root + L"\\crashes\\*.dmp").c_str(), &found);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    continue;
                replay::crashnotice::Dump dump;
                dump.name = Utf8(found.cFileName);
                dump.written = (static_cast<std::uint64_t>(found.ftLastWriteTime.dwHighDateTime) << 32) | found.ftLastWriteTime.dwLowDateTime;
                dumps.push_back(dump);
            } while (dumps.size() < 256 && FindNextFileW(find, &found));
            FindClose(find);
        }
        if (dumps.empty())
            return;
        const std::wstring ackPath = root + L"\\" + std::wstring(replay::crashnotice::kAckFileName,
            replay::crashnotice::kAckFileName + std::strlen(replay::crashnotice::kAckFileName));
        std::uint64_t acknowledged = 0;
        {
            HANDLE file = CreateFileW(ackPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                char text[64]{};
                DWORD read = 0;
                if (ReadFile(file, text, sizeof(text) - 1, &read, nullptr))
                    acknowledged = replay::crashnotice::ParseAck(std::string(text, read));
                CloseHandle(file);
            }
        }
        FILETIME nowTime{};
        GetSystemTimeAsFileTime(&nowTime);
        const std::uint64_t now = (static_cast<std::uint64_t>(nowTime.dwHighDateTime) << 32) | nowTime.dwLowDateTime;
        const replay::crashnotice::Decision decision = replay::crashnotice::Decide(dumps, GetCurrentProcessId(), acknowledged, now);
        if (!decision.show)
            return;
        wchar_t folder[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, folder);
        const std::wstring ini = replay::identity::IniPathInW(folder);
        const bool includeOpenClip = replay::identity::IniIntW(L"ReportIncludeClips", 0, ini.c_str()) != 0;
        g_crashNotice.decision = decision;
        g_crashNotice.reportsFolder = replay::storage::Utf8(replay::storage::Path(replay::storage::kReports)); // the base folder (storage_paths.h)
        g_crashNotice.includeOpenClip = includeOpenClip;
        g_crashNotice.until = GetTickCount64() + replay::crashnotice::kShowMs;
        // Told: the next start shows it only for a newer crash.
        CreateDirectoryW(root.c_str(), nullptr);
        HANDLE file = CreateFileW(ackPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            const std::string ack = replay::crashnotice::AckText(decision.newest);
            DWORD wrote = 0;
            WriteFile(file, ack.data(), static_cast<DWORD>(ack.size()), &wrote, nullptr);
            CloseHandle(file);
        }
    }
    StorageNoticeState g_storageNotice;

namespace
{
    constexpr ULONGLONG kStorageNoticeMs = 30000;
}

    bool StorageNoticeVisible() { return GetTickCount64() < g_storageNotice.until && !render_export::ExportController::Get().Busy(); }

    void CheckStorageNotice()
    {
        if (g_storageNotice.checked || !g_hasSnapshot)
            return;
        g_storageNotice.checked = true;
        const replay::storage::Layout& layout = replay::storage::Current();
        reshade::log::message(reshade::log::level::info, replay::storage::LogLine().c_str());
        if (!layout.failed)
            return;
        g_storageNotice.gameDetail = layout.gameDetail;
        g_storageNotice.fallbackDetail = layout.fallbackDetail;
        g_storageNotice.until = GetTickCount64() + kStorageNoticeMs;
    }

    void DrawStorageNotice(ImDrawList* draw, float y0)
    {
        UI_PROBE_SCOPE("StorageNotice");
        if (!StorageNoticeVisible())
            return;
        const float width = std::min(900.0f, g_vw - 80.0f);
        const float x0 = (g_vw - width) * 0.5f;
        const float pad = 30.0f;
        const float wrap = width - 2.0f * pad;
        const float bodyY = y0 + pad + 46.0f;
        const std::string title = TR("storage.title", "Redemption Editor cannot save anything");
        const std::string body = TRF("storage.body",
            "Windows refused every folder the editor saves in, so clips, projects, reports and changed settings are not kept. "
            "Game folder: {game}. Fallback folders: {fallback}. Give your Windows account write access to one of them, or move the game "
            "out of Program Files.",
            {{"game", g_storageNotice.gameDetail}, {"fallback", g_storageNotice.fallbackDetail}});
        const float bodyH = ParagraphHeight(20.0f, body.c_str(), wrap);
        InkPanel(draw, x0, y0, x0 + width, bodyY + bodyH + pad, IM_COL32(12, 11, 10, 235));
        Text(draw, x0 + pad, y0 + pad - 4.0f, 28.0f, kText, title.c_str());
        draw->AddLine(P(x0 + pad, y0 + pad + 34.0f), P(x0 + width - pad, y0 + pad + 34.0f), kRed, S(3.0f));
        Paragraph(draw, x0 + pad, bodyY, 20.0f, kText, body.c_str(), wrap);
    }

    void DrawCrashNotice(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("CrashNotice");
        if (!CrashNoticeVisible())
            return;
        const float width = std::min(900.0f, g_vw - 80.0f);
        const float x0 = (g_vw - width) * 0.5f;
        const float y0 = 110.0f;
        const float pad = 30.0f;
        const float wrap = width - 2.0f * pad;
        const float bodyY = y0 + pad + 46.0f;
        const replay::crashnotice::Text text =
            replay::crashnotice::NoticeText(g_crashNotice.decision, Keys(), g_crashNotice.reportsFolder, g_crashNotice.includeOpenClip);
        const float bodyH = ParagraphHeight(20.0f, text.body.c_str(), wrap);
        InkPanel(draw, x0, y0, x0 + width, bodyY + bodyH + pad, IM_COL32(12, 11, 10, 235));
        Text(draw, x0 + pad, y0 + pad - 4.0f, 28.0f, kText, text.title.c_str());
        draw->AddLine(P(x0 + pad, y0 + pad + 34.0f), P(x0 + width - pad, y0 + pad + 34.0f), kRed, S(3.0f));
        Paragraph(draw, x0 + pad, bodyY, 20.0f, kText, text.body.c_str(), wrap);
    }

namespace
{

    void ShowRename(const ClipInfo& clip); // below
}

    // ---------------- Gallery ----------------
    int g_galleryIndex = 0;
    int g_gallerySort = 0; // 0 date, 1 duration
    std::string g_compressingClip; // the clip C asked for

namespace
{
    std::uint64_t g_gallerySequence = 0;

    // The snapshot holds a window of 64 clips of the whole sorted list (the
    // folder can hold hundreds): the core sorts, the window follows the
    // selection.
    std::uint32_t g_requestedWindowFirst = ~0u;
    int g_requestedSort = -1;

    void RequestClipWindow(std::uint32_t first)
    {
        if (first == g_requestedWindowFirst && g_gallerySort == g_requestedSort)
            return;
        Command command{};
        command.timeMs = first;
        command.enabled = static_cast<std::uint32_t>(g_gallerySort);
        command.direction = g_requestedWindowFirst == ~0u ? 0 : 1; // 1: a page move, no re-read
        if (Send(CommandType::RefreshClips, &command))
        {
            g_requestedWindowFirst = first;
            g_requestedSort = g_gallerySort;
        }
    }
}

    // `reread` false (the home screen, which only shows the counts): the list the
    // core keeps (rebuilt after every save, delete, rename and compression), no
    // disk read. Reading the folder's hundreds of headers stalls the game's frame
    // right as the editor opens, and keys pressed during it would be lost. The
    // gallery still reads the folder when it opens (clips copied in by hand).
    void RefreshClipList(bool reread)
    {
        const std::uint32_t first = g_requestedWindowFirst == ~0u ? 0u : g_requestedWindowFirst;
        if (reread)
            g_requestedWindowFirst = ~0u;
        RequestClipWindow(first);
    }

    // The clip at a place of the whole list, when the window holds it.
    const ClipInfo* GalleryClip(int n)
    {
        if (g_snapshot.clipCount == 0 || n < 0)
            return nullptr;
        const std::uint32_t first = g_snapshot.clips[0].listIndex;
        const std::uint32_t at = static_cast<std::uint32_t>(n);
        return at >= first && at < first + g_snapshot.clipCount ? &g_snapshot.clips[at - first] : nullptr;
    }

    int g_projectIndex = 0; // the clip selected on the project's timeline

namespace
{

    void PlayClip(const ClipInfo& clip)
    {
        // Picking a clip for the project (A on its timeline).
        if (g_galleryPicking)
        {
            if (!clip.loadable)
            {
                PlayUi(UiSound::Error);
                ShowAlert(TR("alert.title", "Alert"), TR("alert.clip_version.add", "This clip was saved by another version of the replay and cannot be added."), nullptr,
                    CommandType::None, "", Screen::Gallery);
                return;
            }
            Command command{};
            std::snprintf(command.name, sizeof(command.name), "%s", clip.name);
            command.keyframeId = g_pickInsertAt;
            if (Send(CommandType::ProjectAddClip, &command))
            {
                PlayUi(UiSound::Select);
                if (g_pickInsertAt != ~0u)
                    g_projectIndex = static_cast<int>(g_pickInsertAt);
                else
                    g_projectIndex = static_cast<int>(g_snapshot.projectClipCount);
            }
            g_galleryPicking = false;
            g_screen = Screen::Project;
            return;
        }
        if (!clip.loadable)
        {
            PlayUi(UiSound::Error);
            ShowAlert(TR("alert.title", "Alert"), TR("alert.clip_version.open", "This clip was saved by another version of the replay and cannot be opened."), nullptr,
                CommandType::None, "", Screen::Gallery);
            return;
        }
        // Enter on the clip already open goes back to the Edit screen; no reload
        // from disk, so the undo history and the playhead stay.
        if (Flag(SnapshotPlaying) && g_snapshot.currentClip[0] && std::strcmp(clip.name, g_snapshot.currentClip) == 0)
        {
            PlayUi(UiSound::Select);
            g_screen = Screen::Edit;
            return;
        }
        // A recording in progress (F4 alone, or the F4 mark inside the replay
        // buffer) is saved as a clip by the core first, through the save queue, and
        // the chosen clip opens once that save is written.
        const bool savesRecording = Flag(SnapshotRecording) && (!Flag(SnapshotBufferRunning) || Flag(SnapshotBufferManual));
        PlayUi(UiSound::Select);
        RequestLoad(clip.name); // after the notice is on screen
        g_openingSavesRecording = savesRecording;
        g_screen = Screen::Closed;
    }
}

    // The length column of the gallery's and the projects' rows (x 590, the
    // row ending at 640): a longer word of another language ("old") ends at
    // the row's end instead.
    float LengthColumnX(const char* text)
    {
        const float width = TextWidth(20.0f, text) / g_scale;
        return !replay::i18n::Active() || 590.0f + width <= 636.0f ? 590.0f : 636.0f - width;
    }

    void DrawGallery(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("Gallery");
        MenuColumn(draw, 660.0f);
        TitleBanner(draw, 50.0f, 20.0f, 420.0f, 70.0f, 62.0f, 72.0f, 30.0f, 44.0f, "REDEMPTION EDITOR");
        SectionTitle(draw, 80.0f, 100.0f, 560.0f, g_galleryPicking ? TR("gallery.title.pick", "ADD A CLIP TO THE PROJECT") : TR("gallery.title", "RECORDED CLIPS"));

        // Sort row.
        Text(draw, 80.0f, 140.0f, 14.0f, kMuted, TR("gallery.sort_by", "SORT BY"));
        const char* sorts[] = {TR("gallery.sort.date", "Date"), TR("gallery.sort.duration", "Duration")};
        float sx = 160.0f;
        if (replay::i18n::Active()) // after a long "SORT BY" (English keeps its place)
            sx = std::max(sx, 80.0f + TextWidth(14.0f, TR("gallery.sort_by", "SORT BY")) / g_scale + 20.0f);
        for (int n = 0; n < 2; ++n)
        {
            ImGui::SetCursorScreenPos(P(sx - 6.0f, 136.0f));
            ImGui::PushID(100 + n);
            if (ItemClicked(ImGui::InvisibleButton("##sort", ImVec2(S(TextWidth(16.0f, sorts[n]) / g_scale + 12.0f), S(24.0f)))) &&
                g_gallerySort != n)
            {
                g_gallerySort = n;
                g_galleryIndex = 0;
                PlayUiStep(n == 0 ? -1 : 1);
            }
            ImGui::PopID();
            if (n == g_gallerySort)
                Brush(draw, sx - 6.0f, 136.0f, TextWidth(16.0f, sorts[n]) / g_scale + 6.0f, 24.0f, kRed);
            Text(draw, sx, 138.0f, 16.0f, kText, sorts[n]);
            sx += TextWidth(16.0f, sorts[n]) / g_scale + 34.0f;
        }
        if (KeyPressed(ImGuiKey_Tab))
        {
            g_gallerySort = 1 - g_gallerySort;
            g_galleryIndex = 0;
            PlayUiStep(g_gallerySort == 0 ? -1 : 1);
        }

        const int count = static_cast<int>(g_snapshot.clipTotal);
        if (g_snapshot.clipListSequence != g_gallerySequence)
        {
            g_gallerySequence = g_snapshot.clipListSequence;
            g_galleryIndex = std::clamp(g_galleryIndex, 0, std::max(0, count - 1));
        }
        NavigateList(g_galleryIndex, count);
        // The window: centred on the selection, moved when it nears an edge.
        {
            const int window = static_cast<int>(kMaxClipsInSnapshot);
            const int want = std::clamp(g_galleryIndex - window / 2, 0, std::max(0, count - window));
            const int held = g_snapshot.clipCount ? static_cast<int>(g_snapshot.clips[0].listIndex) : 0;
            if (g_gallerySort != g_requestedSort || !GalleryClip(g_galleryIndex) ||
                (std::abs(want - held) >= window / 4 && (held > 0 || want > held)))
                RequestClipWindow(static_cast<std::uint32_t>(want));
        }

        // The list: at most 11 rows, scrolled to keep the selection.
        constexpr int kRows = 11;
        const int first = std::clamp(g_galleryIndex - kRows / 2, 0, std::max(0, count - kRows));
        // The core reads the folder on a thread; the list shown meanwhile is the last
        // one read.
        const bool listLoading = Flag(SnapshotClipListLoading);
        if (count == 0)
        {
            const char* const text = !ClipListKnown() ? TR("gallery.loading", "Loading clips...") : KeyFill(TR("gallery.empty", "No clips yet. Record one from the home screen ({key_record})."));
            Text(draw, 80.0f, 190.0f, 20.0f, kMuted, text);
            if (!ClipListKnown())
                LoadingAfter(draw, 80.0f, 190.0f, 20.0f, text, 640.0f);
        }
        else if (listLoading)
        {
            // Painted, the turning cylinder after the words in place of the text spinner.
            static const char* kSpin[] = {"|", "/", "-", "\\"};
            const bool cue = ui_art::Get(ui_art::Art::Loading) != 0;
            const std::string text = TRF("gallery.loading_spin", "Loading clips {spinner}", {{"spinner", cue ? "" : kSpin[(GetTickCount64() / 150) % 4]}});
            Text(draw, 420.0f, 140.0f, 14.0f, kMuted, text.c_str());
            if (cue)
                LoadingAfter(draw, 420.0f, 140.0f, 14.0f, text.c_str(), 660.0f);
        }
        for (int row = 0; row < kRows && first + row < count; ++row)
        {
            const int n = first + row;
            const float y = 180.0f + row * 40.0f;
            const ClipInfo* listed = GalleryClip(n);
            if (!listed)
            {
                Text(draw, 88.0f, y + 7.0f, 20.0f, kMuted, "...");
                continue;
            }
            const ClipInfo& clip = *listed;
            // The mouse takes the selection only in a list that fits (a longer one
            // scrolls to keep the selection in its middle, under the mouse).
            bool clicked = false, pointed = false;
            ImGui::PushID(n);
            const bool doubled = ListItem(draw, "##clip", 80.0f, y, 560.0f, 36.0f, n == g_galleryIndex, clicked, count <= kRows ? &pointed : nullptr);
            ImGui::PopID();
            if (clicked || pointed)
                g_galleryIndex = n;
            if (doubled)
                PlayClip(clip);
            const bool selected = n == g_galleryIndex;
            const ImU32 color = !clip.loadable ? kMuted : (selected ? kText : kText);
            // Open while it plays.
            const bool openNow = Flag(SnapshotPlaying) && std::strcmp(clip.name, g_snapshot.currentClip) == 0;
            char duration[16]{};
            FormatClock(clip.durationMs, duration, sizeof(duration), false);
            const char* lengthText = clip.loadable ? duration : TR("gallery.old", "old");
            const float lengthX = LengthColumnX(lengthText);
            Text(draw, lengthX, y + 7.0f, 20.0f, color, lengthText);
            // A compressed clip: a gold badge left of the duration, as tall as its text
            // (at least 18 units), and the name cut with "..." before it.
            float labelRight = std::min(580.0f, lengthX - 10.0f);
            if (clip.compressed)
            {
                const float badgeWidth = TextWidth(12.0f, "ZIP") / g_scale + 10.0f;
                const float bx = labelRight + 2.0f - badgeWidth;
                const float badgeH = std::max(18.0f, LabelHeight(12.0f) + 4.0f);
                const float by = y + 18.0f - badgeH * 0.5f;
                draw->AddRect(P(bx, by), P(bx + badgeWidth, by + badgeH), kGold, S(3.0f), 0, S(1.5f));
                Text(draw, bx + 5.0f, by + 2.0f + (badgeH - 18.0f) * 0.5f, 12.0f, kGold, "ZIP");
                labelRight = bx - 8.0f;
            }
            const std::string label = ClipLabel(clip.name) + (openNow ? TR("gallery.open_now", "   (open)") : "");
            TextFit(draw, 88.0f, y + 7.0f, 20.0f, color, label.c_str(), labelRight - 88.0f);
        }
        if (count > kRows)
        {
            Text(draw, 80.0f, 628.0f, 14.0f, kMuted, TRF("gallery.position", "{at} of {count}", {{"at", std::to_string(g_galleryIndex + 1)}, {"count", std::to_string(count)}}).c_str());
        }

        // Details of the selected clip: the rows first, measured, so the panel grows
        // to them.
        std::vector<std::pair<const char*, std::string>> details;
        const ClipInfo* selectedClip = GalleryClip(g_galleryIndex);
        if (selectedClip)
        {
            const ClipInfo& clip = *selectedClip;
            char duration[16]{};
            char size[32]{};
            FormatClock(clip.durationMs, duration, sizeof(duration), true);
            FormatSize(clip.bytes, size, sizeof(size));
            std::string format;
            const char* cannotOpen = clip.loadable ? "" : TR("gallery.size.cannot_open", " (cannot open)");
            if (clip.compressed && clip.clipBytes)
            {
                char full[32]{};
                FormatSize(clip.clipBytes, full, sizeof(full));
                format = TRF("gallery.size.compressed_of", "{size} compressed ({full}), format {version}{cannot}",
                    {{"size", size}, {"full", full}, {"version", std::to_string(clip.formatVersion)}, {"cannot", cannotOpen}});
            }
            else
                format = TRF("gallery.size", "{size}{compressed}, format {version}{cannot}",
                    {{"size", size}, {"compressed", clip.compressed ? TR("gallery.size.compressed", " compressed") : ""},
                        {"version", std::to_string(clip.formatVersion)}, {"cannot", cannotOpen}});
            char clock[16]{};
            if (clip.hasClock)
                std::snprintf(clock, sizeof(clock), "%02u:%02u", clip.clockStartS / 3600, clip.clockStartS / 60 % 60);
            const auto orDash = [](const char* text) { return std::string(text && text[0] ? text : "-"); };
            // How the clip is stored, in words.
            std::string storage;
            if (clip.compressed && clip.clipBytes && clip.bytes)
                storage = TRF("gallery.storage.ratio", "Compressed, {ratio}x smaller",
                    {{"ratio", replay::i18n::Printf("%.1f", static_cast<double>(clip.clipBytes) / static_cast<double>(clip.bytes))}});
            // The file the compressor works on now (not necessarily the clip C asked
            // for).
            else if (!clip.compressed && std::strcmp(g_snapshot.compressingClip, clip.name) == 0)
                storage = TR("gallery.storage.compressing", "Compressing...");
            else if (!clip.compressed && g_compressingClip == clip.name && g_snapshot.compressState >= 2)
                storage = TR("gallery.storage.waiting", "Waiting to be compressed");
            else
                storage = clip.compressed ? TR("gallery.storage.compressed", "Compressed") : TR("gallery.storage.not_compressed", "Not compressed");
            // The chapter the clip was recorded in (and its camp).
            char chapter[160]{};
            replay::world::ChapterText(static_cast<std::uint8_t>(clip.chapter), chapter, sizeof(chapter));
            if (chapter[0] && clip.camp < 9)
            {
                const std::size_t used = std::strlen(chapter);
                std::snprintf(chapter + used, sizeof(chapter) - used, " (%s)",
                    replay::world::CampName(static_cast<std::uint8_t>(clip.camp)));
            }
            details = {
                {TR("gallery.detail.duration", "Duration"), duration},
                {TR("gallery.detail.time", "In-game time"), orDash(clock)},
                {TR("gallery.detail.weather", "Weather"), orDash(replay::envoverride::WeatherDisplay(clip.weather))},
                {TR("gallery.detail.character", "Character"), orDash(clip.character)},
                {TR("gallery.detail.chapter", "Chapter"), orDash(chapter)},
                {TR("gallery.detail.region", "Region"), orDash(clip.region)},
                {TR("gallery.detail.file", "File"), clip.name},
                {TR("gallery.detail.size", "Size"), format},
                {TR("gallery.detail.storage", "Storage"), storage},
            };
        }
        // The values' column from the panel's padding.
        const float detailX = kDetailLeft + kPanelPad;
        const float valueW = kDetailRight - kPanelPad - kDetailValueX;
        float detailsBottom = 600.0f;
        {
            float y = 432.0f;
            for (const auto& [label, value] : details)
                y += std::max(22.0f, ParagraphHeight(15.0f, value.c_str(), valueW) + 4.0f);
            detailsBottom = std::max(detailsBottom, y + kPanelPadBottom);
        }
        InkPanel(draw, kDetailLeft, 140.0f, kDetailRight, detailsBottom, kPanel);
        draw->AddRectFilled(P(725.0f, 165.0f), P(1175.0f, 418.0f), IM_COL32(40, 38, 34, 255));
        const ImTextureID thumbnail = selectedClip ? clip_thumbnails::Thumbnail(g_runtime, selectedClip->name) : 0;
        if (thumbnail)
            // The texture shows upside down on this renderer (the file on disk is
            // upright): sampled with V flipped.
            draw->AddImage(ImTextureRef(thumbnail), P(725.0f, 165.0f), P(1175.0f, 418.0f), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
        else if (selectedClip && clip_thumbnails::Loading(selectedClip->name))
        {
            Text(draw, 900.0f, 282.0f, 16.0f, kMuted, TR("gallery.thumb.loading", "[ loading ]")); // read on a thread
            LoadingCue(draw, 950.0f, 252.0f, 36.0f);
        }
        else
            Text(draw, 880.0f, 282.0f, 16.0f, kMuted, TR("gallery.thumb.none", "[ no thumbnail ]"));
        if (selectedClip)
        {
            const ClipInfo& clip = *selectedClip;
            float y = 432.0f;
            for (const auto& [label, value] : details)
            {
                TextFit(draw, detailX, y, 15.0f, kMuted, label, kDetailValueX - detailX - 8.0f);
                // Long values (a region) wrap inside the panel.
                const float height = Paragraph(draw, kDetailValueX, y, 15.0f, kText, value.c_str(), valueW);
                y += std::max(22.0f, height + 4.0f);
            }
            if (Confirmed())
                PlayClip(clip);
            // R renames the selected clip.
            if (!g_galleryPicking && KeyPressed(ImGuiKey_R, false))
                ShowRename(clip);
            // C compresses the selected clip now (any age; not the open one, which the
            // compressor never touches).
            if (!g_galleryPicking && KeyPressed(ImGuiKey_C, false))
            {
                const bool open = Flag(SnapshotPlaying) && std::strcmp(clip.name, g_snapshot.currentClip) == 0;
                if (clip.compressed || open || !clip.loadable)
                    PlayUi(UiSound::Error);
                else if (SendNamed(CommandType::CompressClip, clip.name))
                {
                    PlayUi(UiSound::Select);
                    g_compressingClip = clip.name;
                }
            }
            if (!g_galleryPicking && KeyPressed(ImGuiKey_Delete, false))
            {
                PlayUi(UiSound::Select);
                // The open clip closes and its unsaved marker changes go with it (the core
                // drops them).
                const bool openNow = Flag(SnapshotPlaying) && std::strcmp(clip.name, g_snapshot.currentClip) == 0;
                ShowDeleteClipAlert(clip.name, openNow);
            }
        }
        else if (Confirmed())
            PlayUi(UiSound::Error);

        if (Back())
        {
            PlayUi(UiSound::Back);
            g_screen = g_galleryPicking ? Screen::Project : Screen::Home;
            g_galleryPicking = false;
        }
    }

    // ---------------- Rename ----------------
    std::string g_renameFrom;
    char g_renameText[48]{};
    bool g_renameFocus = false;
    const char* g_renameError = ""; // why Enter was refused, under the field

namespace
{

    void ShowRename(const ClipInfo& clip)
    {
        g_renameFrom = clip.name;
        std::string stem = clip.name;
        if (stem.size() > 7 && stem.substr(stem.size() - 7) == ".rpclip")
            stem.resize(stem.size() - 7);
        std::snprintf(g_renameText, sizeof(g_renameText), "%s", stem.c_str());
        g_renameFocus = true;
        g_renameError = "";
        g_screen = Screen::Rename;
    }

    // The dialogs (Alert, Rename, New/Rename project): an ink panel round their
    // title, line and text (the content 440 to 840, the title at 210), from 192
    // to `bottom`.
    constexpr float kDialogTop = 192.0f;
}

    void DialogPanel(ImDrawList* draw, float bottom)
    {
        InkPanel(draw, 440.0f - kPanelPad, kDialogTop, 840.0f + kPanelPad, std::max(bottom, kDialogTop + 140.0f), kPanel);
    }

    NameBoxPlaces PlaceNameBox(const char* fileLine, const char* help)
    {
        NameBoxPlaces at{};
        at.inputY = std::max(330.0f, 292.0f + (fileLine ? ParagraphHeight(17.0f, fileLine, 400.0f) : 0.0f) + 8.0f);
        at.helpY = std::max(372.0f, at.inputY + ImGui::GetFrameHeight() / g_scale + 14.0f);
        at.errorY = std::max(400.0f, at.helpY + ParagraphHeight(15.0f, help, 400.0f) + 6.0f);
        return at;
    }

    void DrawRename(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("Rename");
        draw->AddRectFilled(ImVec2(0.0f, 0.0f), ImGui::GetIO().DisplaySize, IM_COL32(0, 0, 0, 215));
        const std::string fileLine = TRF("rename.file", "File: {file}", {{"file", g_renameFrom}});
        const char* const kHelp = TR("rename.clip.help", "Letters, numbers, spaces, - and _. The thumbnail keeps up with it.");
        const NameBoxPlaces at = PlaceNameBox(fileLine.c_str(), kHelp);
        DialogPanel(draw, (g_renameError[0] ? at.errorY + ParagraphHeight(15.0f, g_renameError, 400.0f) : at.helpY + ParagraphHeight(15.0f, kHelp, 400.0f)) +
            kPanelPadBottom);
        const char* const title = TR("rename.clip.title", "Rename clip");
        const float width = TextWidth(40.0f, title) / g_scale;
        Text(draw, 640.0f - width / 2.0f, 210.0f, 40.0f, kText, title);
        draw->AddLine(P(440.0f, 268.0f), P(840.0f, 268.0f), kRed, S(3.0f));
        Paragraph(draw, 440.0f, 292.0f, 17.0f, kMuted, fileLine.c_str(), 400.0f);
        ImGui::SetCursorScreenPos(P(440.0f, at.inputY));
        ImGui::PushItemWidth(S(400.0f));
        if (g_renameFocus)
        {
            ImGui::SetKeyboardFocusHere();
            g_renameFocus = false;
        }
        const bool entered = ImGui::InputText("##rename", g_renameText, sizeof(g_renameText),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll, nullptr, nullptr) ||
            MissedTap(VK_RETURN);
        ImGui::PopItemWidth();
        Paragraph(draw, 440.0f, at.helpY, 15.0f, kMuted, kHelp, 400.0f);
        if (g_renameError[0])
            Paragraph(draw, 440.0f, at.errorY, 15.0f, kGold, g_renameError, 400.0f);
        if (entered)
        {
            std::string name(g_renameText);
            while (!name.empty() && name.back() == ' ')
                name.pop_back(); // a trailing space is not part of the name
            bool ok = !name.empty();
            for (const char c : name)
                ok = ok && (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_');
            const std::string both = g_renameFrom + "|" + name; // the core adds .rpclip
            // The refusals say why, and the same name is no rename.
            g_renameError = name.empty() ? NameEmptyError() : !ok ? NameCharsError() : "";
            if (ok && both.size() >= kClipNameCapacity - 1)
            {
                ok = false;
                g_renameError = TR("rename.error.long", "That name is too long.");
            }
            if (ok && name + ".rpclip" == g_renameFrom)
            {
                PlayUi(UiSound::Back);
                g_screen = Screen::Gallery;
            }
            else if (ok && SendNamed(CommandType::RenameClip, both.c_str()))
            {
                clip_thumbnails::Forget(g_renameFrom);
                PlayUi(UiSound::Select);
                g_screen = Screen::Gallery;
            }
            else
                PlayUi(UiSound::Error);
        }
        else if (!g_helpOpen && !g_helpSwallow && KeyPressed(ImGuiKey_Escape))
        {
            PlayUi(UiSound::Back);
            g_screen = Screen::Gallery;
        }
    }

    // ---------------- QA note ----------------
    // Opened by a "mark problem" (Shift+F2, the core's key) over the playback,
    // which goes on (the 2 s after the press are part of the mark). Enter
    // sends the note, empty or not; Esc sends none. Drawn after the render
    // export's capture point and never during an export (the core refuses
    // a mark then).
    void DrawQaNote(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("QaNote");
        qa_shots::State& qa = qa_shots::g_state;
        // The content measured first and laid out under each other, the box
        // kPanelPad around it and as tall as it needs.
        constexpr float kNoteX = 350.0f, kNoteW = 580.0f;
        const char* const kExample =
            KeyFill(TR("qa.note.example", "For example: \"the horse froze\", \"no voice\", \"he vanished\". {key_select} saves (empty is fine), {key_back} skips."));
        const char* const kReport = KeyFill(TR("qa.note.report", "When you are done testing, press {key_report}: one zip with every mark and its clips."));
        const std::string at = TRF("qa.note.at", "At {time} of the clip. The 8 seconds before and 2 after are saved with it. What went wrong? (optional)",
            {{"time", replay::qa::TimeText(qa.notePlayheadMs)}});
        const float inputY = std::max(214.0f, 156.0f + ParagraphHeight(16.0f, at.c_str(), kNoteW) + 8.0f);
        const float exampleY = std::max(258.0f, inputY + ImGui::GetFrameHeight() / g_scale + 14.0f);
        const float reportY = std::max(300.0f, exampleY + ParagraphHeight(15.0f, kExample, kNoteW) + 8.0f);
        const float bottom = std::max(352.0f, reportY + ParagraphHeight(16.0f, kReport, kNoteW) + kPanelPadBottom);
        const float boxLeft = kNoteX - kPanelPad, boxRight = kNoteX + kNoteW + kPanelPad;
        InkPanel(draw, boxLeft, 96.0f, boxRight, bottom, kPanel); // taller, for the Shift+F1 line
        if (!ui_art::Get(ui_art::Art::EdgeInk))
            draw->AddRect(P(boxLeft, 96.0f), P(boxRight, bottom), kPanelLine, 0.0f, 0, S(1.0f)); // the flat look's outline
        const char* const title = TR("qa.note.title", "PROBLEM MARKED");
        Brush(draw, boxLeft, 106.0f, std::max(290.0f, TextWidth(24.0f, title) / g_scale + 40.0f), 36.0f, kRed); // as wide as a long title
        Text(draw, boxLeft + 16.0f, 110.0f, 24.0f, kText, title);
        Paragraph(draw, kNoteX, 156.0f, 16.0f, kText, at.c_str(), kNoteW);
        ImGui::SetCursorScreenPos(P(kNoteX, inputY));
        ImGui::PushItemWidth(S(kNoteW));
        if (qa.noteFocus)
        {
            ImGui::SetKeyboardFocusHere();
            qa.noteFocus = false;
        }
        const bool entered = ImGui::InputText("##qanote", qa.note, sizeof(qa.note), ImGuiInputTextFlags_EnterReturnsTrue, nullptr, nullptr) ||
            MissedTap(VK_RETURN);
        ImGui::PopItemWidth();
        Paragraph(draw, kNoteX, exampleY, 15.0f, kMuted, kExample, kNoteW);
        // The report key on its own line, in plain text.
        Paragraph(draw, kNoteX, reportY, 16.0f, kText, kReport, kNoteW);
        if (entered)
        {
            qa_shots::SendNote(true);
            PlayUi(UiSound::Select);
            ShowMarkNotice();
        }
        else if (KeyPressed(ImGuiKey_Escape, false))
        {
            qa_shots::SendNote(false);
            PlayUi(UiSound::Back);
            ShowMarkNotice();
        }
    }
