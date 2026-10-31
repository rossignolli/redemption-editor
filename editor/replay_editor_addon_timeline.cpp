// replay_editor_addon_timeline.cpp: the playback HUD and the edit screen: the edit panel, the
// timeline and the camera overlay of the edit screen.
// Split from replay_editor_addon.cpp (moved code); shared internals in replay_editor_addon_internal.h.

#include "replay_editor_addon_internal.h"

namespace
{

    // ---------------- Playback HUD ----------------
    // Minimal HUD while a clip plays and the editor is closed: the red
    // progress bar with the cursor, the time and the keys.
    // The clip's restricted-camera spans as thin bands under a timeline bar
    // (cutscene, script camera, no control), the recording cuts as small ticks,
    // a legend of the kinds present and a tooltip.
    const char* SpanName(std::uint32_t kind)
    {
        return kind == 0 ? TR("span.cutscene", "Cutscene") : kind == 1 ? TR("span.script_camera", "Script camera") : TR("span.no_control", "No control");
    }
    const ImU32 kSpanColors[] = {IM_COL32(176, 120, 226, 230), IM_COL32(92, 152, 222, 230), IM_COL32(150, 143, 131, 230)};

    // Returns where its legend ends (legendX without one).
    float DrawSpanBands(ImDrawList* draw, float left, float width, float barBottom, float legendX, float legendY,
        double viewStart = 0.0, double viewSpan = 0.0)
    {
        UI_PROBE_SCOPE("SpanBands");
        const double duration = viewSpan > 0.0 ? viewSpan : g_snapshot.durationMs ? static_cast<double>(g_snapshot.durationMs) : 1.0;
        const ImVec2 mouse = ImGui::GetMousePos();
        bool present[3] = {};
        std::string tip;
        for (std::uint32_t n = 0; n < g_snapshot.spanCount && n < kMaxSpansInSnapshot; ++n)
        {
            const ClipSpan& span = g_snapshot.spans[n];
            if (span.kind > 2 || span.endMs <= span.startMs)
                continue;
            present[span.kind] = true;
            const float x0 = std::clamp(left + static_cast<float>((span.startMs - viewStart) / duration) * width, left, left + width);
            const float x1 = std::clamp(std::max(x0 + 2.0f, left + static_cast<float>((span.endMs - viewStart) / duration) * width), left, left + width);
            const float y0 = barBottom + 2.0f + span.kind * 3.0f;
            draw->AddRectFilled(P(x0, y0), P(x1, y0 + 2.0f), kSpanColors[span.kind]);
            if (mouse.x >= S(x0) && mouse.x <= S(x1) && mouse.y >= S(barBottom - 12.0f) && mouse.y <= S(barBottom + 14.0f))
            {
                char at[16]{}, to[16]{};
                FormatClock(span.startMs, at, sizeof(at), true);
                FormatClock(span.endMs, to, sizeof(to), true);
                tip += (tip.empty() ? "" : "\n") + TRF("span.tip", "{kind}: {from} to {to}", {{"kind", SpanName(span.kind)}, {"from", at}, {"to", to}});
            }
        }
        for (std::uint32_t n = 0; n < g_snapshot.cutCount && n < kMaxCutsInSnapshot; ++n)
        {
            const float x = left + static_cast<float>((g_snapshot.cuts[n] - viewStart) / duration) * width;
            if (x < left || x > left + width)
                continue;
            draw->AddLine(P(x, barBottom - 8.0f), P(x, barBottom + 2.0f), IM_COL32(236, 230, 218, 200), S(1.0f));
            if (std::fabs(mouse.x - S(x)) <= S(3.0f) && mouse.y >= S(barBottom - 12.0f) && mouse.y <= S(barBottom + 14.0f))
                tip += (tip.empty() ? "" : "\n") + std::string(TR("span.cut", "Cut: the recording paused here"));
        }
        if (!tip.empty())
            ImGui::SetTooltip("%s", tip.c_str());
        float x = legendX;
        for (int kind = 0; kind < 3; ++kind)
        {
            if (!present[kind])
                continue;
            draw->AddRectFilled(P(x, legendY + 7.0f), P(x + 14.0f, legendY + 10.0f), kSpanColors[kind]);
            Text(draw, x + 18.0f, legendY, 14.0f, kMuted, SpanName(static_cast<std::uint32_t>(kind)));
            x += 30.0f + TextWidth(14.0f, SpanName(static_cast<std::uint32_t>(kind))) / g_scale;
        }
        return x;
    }

    // The width DrawSpanBands' legend takes.
    float SpanLegendWidth()
    {
        bool present[3] = {};
        for (std::uint32_t n = 0; n < g_snapshot.spanCount && n < kMaxSpansInSnapshot; ++n)
        {
            const ClipSpan& span = g_snapshot.spans[n];
            if (span.kind <= 2 && span.endMs > span.startMs)
                present[span.kind] = true;
        }
        float width = 0.0f;
        for (int kind = 0; kind < 3; ++kind)
            if (present[kind])
                width += 30.0f + TextWidth(14.0f, SpanName(static_cast<std::uint32_t>(kind))) / g_scale;
        return width;
    }
}

    void DrawPlaybackHud(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("PlaybackHud");
        const float left = 120.0f;
        const float right = g_vw - 120.0f;
        const float y = g_vh - g_footH - 44.0f; // over the key bar
        // The project playing as a whole shows the project's time.
        const bool project = ProjectFlag(ProjectPreviewing);
        const std::uint64_t hudNow = project ? g_snapshot.projectPlayheadMs : g_snapshot.playheadMs;
        const std::uint64_t hudTotal = project ? g_snapshot.projectTotalMs : g_snapshot.durationMs;
        const float amount = hudTotal ? std::clamp(static_cast<float>(hudNow) / hudTotal, 0.0f, 1.0f) : 0.0f;
        const float x = left + (right - left) * amount;
        if (TimelineArtReady()) // the painted bar and crown
        {
            TornStrip(draw, ui_art::Art::BarTorn, left, left, right, y - 3.0f, y + 9.0f, kTrackTint);
            TornStrip(draw, ui_art::Art::BarRed, left, left, x, y - 3.0f, y + 9.0f, IM_COL32(255, 255, 255, 255));
            PlayheadIcon(draw, x, y + 11.0f, 26.0f);
        }
        else
        {
            draw->AddRectFilled(P(left, y), P(right, y + 6.0f), IM_COL32(236, 230, 218, 70));
            draw->AddRectFilled(P(left, y), P(x, y + 6.0f), kRed);
            draw->AddCircle(P(x, y - 6.0f), S(7.0f), kText, 0, S(2.0f));
            draw->AddTriangleFilled(P(x - 4.0f, y + 6.0f), P(x + 4.0f, y + 6.0f), P(x, y), kText);
        }
        char now[16]{};
        char total[16]{};
        FormatClock(hudNow, now, sizeof(now), false);
        FormatClock(hudTotal, total, sizeof(total), false);
        char time[48]{};
        std::snprintf(time, sizeof(time), "%s / %s", now, total);
        Text(draw, left, y + 16.0f, 16.0f, kText, time);
        if (project && g_snapshot.projectActiveClip < g_snapshot.projectClipCount)
        {
            // The project, the clip playing and its place.
            const std::string where = TRF("hud.project_clip", "{project}  -  clip {at} of {count}: {clip}",
                {{"project", g_snapshot.projectName}, {"at", std::to_string(g_snapshot.projectActiveClip + 1)},
                    {"count", std::to_string(g_snapshot.projectClipCount)}, {"clip", ClipLabel(g_snapshot.projectClips[g_snapshot.projectActiveClip].file)}});
            TextFit(draw, left, y - 34.0f, 16.0f, kText, where.c_str(), right - left);
            for (std::uint32_t n = 1; n < g_snapshot.projectClipCount; ++n)
            {
                // The cuts between the clips, on the bar.
                std::uint64_t at = 0;
                for (std::uint32_t k = 0; k < n; ++k)
                    at += g_snapshot.projectClips[k].outMs - g_snapshot.projectClips[k].inMs;
                const float cx = left + (right - left) * std::clamp(static_cast<float>(at) / std::max<std::uint64_t>(1, hudTotal), 0.0f, 1.0f);
                if (!SeamMark(draw, cx, y - 6.0f, y + 12.0f, (right - left) / static_cast<float>(g_snapshot.projectClipCount)))
                    draw->AddLine(P(cx, y - 3.0f), P(cx, y + 9.0f), IM_COL32(236, 230, 218, 160), S(1.0f));
                if (n > 64)
                    break;
            }
        }
        char speed[32]{};
        std::snprintf(speed, sizeof(speed), "%u%%", g_snapshot.speedPercent);
        if (g_snapshot.speedPercent != 100)
            Text(draw, left + 130.0f, y + 16.0f, 16.0f, kGold, speed);
        const bool busy = PlaybackBusy(); // the world's load behind the black: the badge (DrawLoadingBadge)
        const char* state = busy ? TR("hud.loading", "Loading") : Flag(SnapshotPaused) ? TR("hud.paused", "Paused") : nullptr;
        if (state)
            Text(draw, left + 200.0f, y + 16.0f, 16.0f, kMuted, state);
        if (busy) // the turning cylinder before "Loading"
            LoadingCue(draw, left + 188.0f, y + 16.0f + LabelHeight(16.0f) * 0.5f, 18.0f);
        DrawSpanBands(draw, left, right - left, y + 6.0f, left + 330.0f, y + 18.0f);
        // The keys are on the key bar under it (PlaybackTips).
    }

    // True while the marker menu has the keyboard (the edit screen's other
    // keys stand back).
    bool g_markerMenuActive = false;

namespace
{
    bool g_backConsumed = false;
}

    // The marker selection the panel last saw and when (a selection the playhead
    // left closes 700 ms after it was made).
    std::uint32_t g_seenSelection = 0;
    ULONGLONG g_selectedAt = 0;

namespace
{
    // Where the seek onto the selected first marker stopped, when the core could not
    // reach it (UINT64_MAX: not needed). The core's earliest playhead is the clip's
    // opening hold (replay_transport.h SetStartMs, ApplyClipTrim): a first marker
    // before it (a project clip's start at its in point 0, a clip's markers saved
    // with another hold) is sought to that hold, and the playhead never stands on
    // the marker itself.
    std::uint64_t g_selectionLandMs = UINT64_MAX;
    // The longest opening hold (replay_playback.cpp: the census over 100 ms, the
    // first poses within 1000 ms of the first create), with room.
    constexpr std::uint64_t kOpeningHoldMaxMs = 2000;

    void DrawEditPanel(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("EditPanel");
        g_valueRowHovered = false;
        g_fxTypingFrame = false;
        // Another clip (or the buffer played): the selection, its menu and an add
        // waiting to be selected belonged to the one before (marker ids start at 1
        // in every clip, so a stale id would open the new clip's marker of that
        // number).
        {
            // By the core's load sequence: a rename of the open clip or the first save
            // of a buffer clip changes its name, not the clip, and keeps the marker's
            // menu open.
            static std::uint32_t seenLoad = ~0u;
            if (seenLoad != g_snapshot.clipLoadSequence)
            {
                seenLoad = g_snapshot.clipLoadSequence;
                g_selectedKeyframeId = 0;
                g_markerMenu = MarkerMenu::Root;
                g_menuFocus = 0;
                g_selectAdded = false;
                ForgetPendingMarkerValues();
            }
        }
        SelectAddedKeyframe();
        // The marker's menu closes when the playhead leaves the marker (play, a
        // click on the bar, a scrub, a frame step): Enter adds again and the arrows
        // step frames. A selection just made waits for its seek (the snapshot lags
        // it).
        {
            const ULONGLONG now = GetTickCount64();
            if (g_selectedKeyframeId != g_seenSelection)
            {
                g_seenSelection = g_selectedKeyframeId;
                g_selectedAt = now;
                g_selectionLandMs = UINT64_MAX;
            }
            if (const CameraKeyframe* key = FindKeyframe(g_selectedKeyframeId))
            {
                const std::uint64_t playhead = g_snapshot.playheadMs;
                std::uint64_t d = key->timeMs > playhead ? key->timeMs - playhead : playhead - key->timeMs;
                const bool running = !Flag(SnapshotPaused);
                const bool settled = g_draggingKeyframeId == 0 && now - g_selectedAt > 700;
                // The first marker before the opening hold: the paused playhead its seek
                // left at the hold (after the marker, before the second one) stands for
                // the marker, until the playhead leaves that spot.
                const CameraKeyframe* keys = g_snapshot.cameraKeyframes;
                const std::uint32_t count = g_snapshot.cameraKeyframeCount;
                if (settled && !running && d > 66 && g_selectionLandMs == UINT64_MAX && count > 0 && key == &keys[0] &&
                    playhead > key->timeMs && playhead - key->timeMs <= kOpeningHoldMaxMs && (count < 2 || playhead < keys[1].timeMs))
                {
                    g_selectionLandMs = playhead;
                }
                if (g_selectionLandMs != UINT64_MAX)
                    d = std::min(d, g_selectionLandMs > playhead ? g_selectionLandMs - playhead : playhead - g_selectionLandMs);
                if (settled && (running || d > 66))
                {
                    g_selectedKeyframeId = 0;
                    g_seenSelection = 0;
                    g_selectionLandMs = UINT64_MAX;
                }
            }
        }
        const bool playing = Flag(SnapshotPlaying);
        const CameraKeyframe* selected = FindKeyframe(g_selectedKeyframeId);
        int index = -1;
        for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
            if (selected && g_snapshot.cameraKeyframes[n].id == selected->id)
                index = static_cast<int>(n);
        // Only when the marker selection changes. With no marker selected the panel
        // can hold the free camera's menu, and resetting every frame would take the
        // focus back to the first row.
        if ((selected ? selected->id : 0u) != g_menuMarker)
        {
            g_rowTyping.on = false;
            g_markerMenu = MarkerMenu::Root;
            g_menuFocus = 0;
            g_menuMarker = selected ? selected->id : 0;
        }
        if (selected)
        {
            g_freeCamMenuOpen = false; // a marker's menu takes the panel
            g_splineDefaultsOpen = false;
        }
        if (!g_freeCamMenuOpen)
            g_splineDefaultsOpen = false; // it lives inside the free camera's menu
        g_markerMenuActive = selected != nullptr || g_freeCamMenuOpen;

        std::vector<MenuRow> rows;
        std::string title;
        if (!selected && g_freeCamMenuOpen)
        {
            rows = BuildFreeCamRows();
            title = Flag(SnapshotFreeCamEnabled) ? TR("edit.title.freecam_on", "FREE CAMERA  (on)") : TR("edit.title.freecam_off", "FREE CAMERA  (off)");
        }
        if (!selected && g_splineDefaultsOpen)
        {
            rows = BuildSplineDefaultRows();
            title = TR("edit.title.spline_defaults", "SPLINE DEFAULTS");
        }
        if (selected)
        {
            rows = BuildMarkerRows(*selected, index);
            const int count = static_cast<int>(g_snapshot.cameraKeyframeCount);
            char at[24]{};
            FormatClock(selected->timeMs, at, sizeof(at), true);
            const char* name = index == 0 ? TR("edit.title.start", "START") : index == count - 1 && count > 1 ? TR("edit.title.end", "END")
                                                                                                            : TR("edit.title.marker", "MARKER");
            const char* sub = g_markerMenu == MarkerMenu::Camera ? TR("edit.title.camera", "  CAMERA") : g_markerMenu == MarkerMenu::Dof ? TR("edit.title.dof", "  DEPTH OF FIELD")
                : g_markerMenu == MarkerMenu::Effects ? TR("edit.title.effects", "  EFFECTS") : g_markerMenu == MarkerMenu::Position ? TR("edit.title.position", "  POSITION")
                : g_markerMenu == MarkerMenu::Tools ? TR("edit.title.move", "  MOVE") : g_markerMenu == MarkerMenu::Spline ? TR("edit.title.spline", "  SPLINE")
                : g_markerMenu == MarkerMenu::SplineDefaults ? TR("edit.title.spline_defaults_sub", "  SPLINE DEFAULTS") : g_markerMenu == MarkerMenu::Shake ? TR("edit.title.shake", "  SHAKE")
                : g_markerMenu == MarkerMenu::ShakeAdvanced ? TR("edit.title.shake_advanced", "  SHAKE ADVANCED") : "";
            title = TRF("edit.title.marker_line", "{name} {n}  {time}{menu}", {{"name", name}, {"n", std::to_string(index + 1)}, {"time", at}, {"menu", sub}});
        }
        // Render export: the Export button; the ENVIRONMENT rows. A long list
        // scrolls with the focus (the rows the screen holds).
        const int fits = std::max(4, static_cast<int>((g_vh - g_footH - 140.0f - 260.0f) / 38.0f));
        const bool homePanel = !selected && !g_freeCamMenuOpen;
        // The home panel's own height: four rows from 128, the environment, the note
        // and the help paragraph measured, the buttons and the panel's foot under
        // them (at small resolutions the readable minimum makes them taller); the
        // panel kPanelPad around the rows.
        const char* const kEditNote =
            KeyFill(TR("edit.note", "{key_select} adds a marker at the playhead, with the settings of the marker before it (the free camera's view "
            "when it is on). Click a gold mark or press {key_next_marker} to open its menu."));
        const float buttonsAt = std::max(100.0f, 4.0f + ParagraphHeight(16.0f, kEditNote, kEditW) + 8.0f);
        float height = homePanel ? 128.0f + 4 * 38.0f + EnvironmentPanelH() + 12.0f + buttonsAt + 74.0f + 14.0f - 20.0f
            : 150.0f + std::clamp<std::size_t>(rows.size(), 4, static_cast<std::size_t>(fits)) * 38.0f + 90.0f;
        // A typed number's refusal takes a line under its row.
        const bool typeError = g_rowTyping.on && g_rowTyping.error[0];
        if (!homePanel && typeError)
            height += kTypeErrorH;
        if (!homePanel && !rows.empty())
        {
            // The rows' menus fit their content as drawn below: the rows from 140, the
            // divider, the focused row's help paragraph from 18 under the last row, and
            // the panel's foot under it (the panel starts at 20).
            const std::size_t shownRows = std::min(rows.size(), static_cast<std::size_t>(fits));
            const std::string& help = rows[std::clamp<std::size_t>(static_cast<std::size_t>(std::max(0, g_menuFocus)), 0, rows.size() - 1)].help;
            height = 140.0f + static_cast<float>(shownRows) * 38.0f + (typeError ? kTypeErrorH : 0.0f) + 18.0f +
                ParagraphHeight(15.0f, help.c_str(), kEditW) + kPanelPadBottom - 20.0f;
        }
        const float panelLeft = kEditX - kPanelPad, panelRight = kEditX + kEditW + kPanelPad;
        InkPanel(draw, panelLeft, 20.0f, panelRight, 20.0f + height, kPanel, kInkEdge, false);
        TitleBanner(draw, panelLeft, 36.0f, 0.0f, 50.0f, 44.0f, panelLeft + 14.0f, 42.0f, 30.0f, "REDEMPTION EDITOR");
        OrnamentDivider(draw, 40.0f, 96.0f, 300.0f);

        if (homePanel)
        {
            SectionTitle(draw, 40.0f, 106.0f, 300.0f, TR("edit.camera", "CAMERA"));
            float y = 128.0f;
            const float lead = EditIconLead();
            if (ValueRow(draw, "##free", y, TR("edit.free_camera", "Free camera"), Flag(SnapshotFreeCamEnabled) ? TR("common.on_cap", "On") : TR("common.off_cap", "Off"), playing, false,
                    lead, ui_art::Art::ToolCamera) != 0)
                Send(CommandType::ToggleFreeCam);
            y += 38.0f;
            // Drone, follow, Walk, the world markers...
            if (ValueRow(draw, "##freeopts", y, TR("edit.free_camera_options", "Free camera options"),
                    (std::string(g_snapshot.freeCamMotion ? TR("freecam.flight.drone", "Drone") : TR("freecam.flight.classic", "Classic")) + "  >").c_str(), true, false, lead) != 0)
            {
                PlayUi(UiSound::Select);
                g_freeCamMenuOpen = true;
                g_menuFocus = 0;
            }
            y += 38.0f;
            if (ValueRow(draw, "##track", y, TR("edit.markers", "Markers"), Flag(SnapshotCameraTrackEnabled) ? TR("common.on_cap", "On") : TR("common.off_cap", "Off"), playing, false, lead) != 0)
            {
                Command command{};
                command.enabled = Flag(SnapshotCameraTrackEnabled) ? 0u : 1u;
                Send(CommandType::SetCameraTrackEnabled, &command);
            }
            y += 38.0f;
            char speed[16]{};
            std::snprintf(speed, sizeof(speed), "%u%%", g_snapshot.speedPercent);
            if (const int step = ValueRow(draw, "##speed", y, TR("marker.speed", "Speed"), speed, playing, true, lead))
                StepPlaybackSpeed(step);
            y += 38.0f;
            y = DrawEnvironmentRows(draw, y, playing);
            y += 12.0f;
            OrnamentDivider(draw, 40.0f, y - 10.0f, 300.0f);
            Paragraph(draw, 40.0f, y + 4.0f, 16.0f, kMuted, kEditNote, kEditW);
            ImGui::SetCursorScreenPos(P(40.0f, y + buttonsAt));
            if (ItemClicked(ImGui::InvisibleButton("##addkey", ImVec2(S(220.0f), S(32.0f)))))
                AddKeyframeHere();
            Brush(draw, 32.0f, y + buttonsAt, 220.0f, 32.0f, ImGui::IsItemHovered() ? kRed : kRedDim);
            Text(draw, 40.0f, y + buttonsAt + 6.0f, 18.0f, kText, TR("edit.add_marker", "Add marker here"));
            // Render export: the export dialog.
            ImGui::SetCursorScreenPos(P(40.0f, y + buttonsAt + 42.0f));
            if (ItemClicked(ImGui::InvisibleButton("##export", ImVec2(S(220.0f), S(32.0f)))))
            {
                PlayUi(UiSound::Select);
                g_screen = Screen::Export;
                exportui::g_back = Screen::Edit;
                if (exportui::g_range == 2)
                    exportui::g_range = 1;
            }
            Brush(draw, 32.0f, y + buttonsAt + 42.0f, 220.0f, 32.0f, ImGui::IsItemHovered() ? kRed : kRedDim);
            const float exportLead = ui_art::Get(ui_art::Art::MenuExport) ? 28.0f : 0.0f;
            MenuIcon(draw, ui_art::Art::MenuExport, 50.0f, y + buttonsAt + 58.0f, 22.0f, true);
            const char* const exportLabel = KeyFill(TR("edit.export", "Export video ({key_export})"));
            // Past the icon a long label is set smaller to stay on the button.
            if (exportLead > 0.0f)
                RowTextFit(draw, 40.0f + exportLead, y + buttonsAt + 48.0f, 18.0f, kText, exportLabel, 220.0f - exportLead - 4.0f);
            else
                Text(draw, 40.0f, y + buttonsAt + 48.0f, 18.0f, kText, exportLabel);
            return;
        }

        RowTextFit(draw, 40.0f, 108.0f, 18.0f, kGold, title.c_str(), kEditW);
        const int count = static_cast<int>(rows.size());
        // A Spline row's typed number (the row it was started on).
        const bool rowTyping = g_rowTyping.on && count > 0 && g_menuFocus < count && rows[g_menuFocus].typed &&
            rows[g_menuFocus].label == g_rowTyping.label;
        if (!rowTyping)
            g_rowTyping.on = false;
        const bool typing = rowTyping; // the typed number
        if (!typing)
            NavigateList(g_menuFocus, count);
        g_menuFocus = std::clamp(g_menuFocus, 0, std::max(0, count - 1));
        const int first = count > fits ? std::clamp(g_menuFocus - fits / 2, 0, count - fits) : 0;
        // A menu with tools' icons (the marker's root) sets every label past their column.
        const bool menuIcons = std::any_of(rows.begin(), rows.end(), [](const MenuRow& row) { return row.icon != ui_art::Art::Count; });
        const float lead = menuIcons ? EditIconLead() : 0.0f;
        float y = 140.0f;
        for (int n = first; n < std::min(count, first + fits); ++n)
        {
            bool clickedEnter = false;
            bool pointed = false; // the mouse moved onto the row (a menu that does not scroll)
            const int step = typing ? 0 : MenuRowDraw(draw, n, y, rows[n], n == g_menuFocus, clickedEnter, lead, count <= fits ? &pointed : nullptr);
            if (pointed)
                g_menuFocus = n;
            if (typing)
            {
                MenuRowDraw(draw, n, y, rows[n], n == g_menuFocus, clickedEnter, lead);
                clickedEnter = false;
                if (n == g_menuFocus && rowTyping)
                    DrawRowTypeBox(draw, y, rows[n]);
                // The refusal under the box takes a line of its own.
                if (n == g_menuFocus && rowTyping && g_rowTyping.error[0] != 0)
                    y += kTypeErrorH;
            }
            if (step != 0 || clickedEnter)
                g_menuFocus = n;
            if (step != 0 && rows[n].step)
            {
                PlayUiStep(step);
                rows[n].step(step);
            }
            if (clickedEnter && rows[n].enter)
            {
                PlayUi(UiSound::Select);
                rows[n].enter();
            }
            y += 38.0f;
        }
        if (rowTyping)
        {
            // The box has the keys (Enter, Esc, the digits).
        }
        else if (count > 0 && rows[g_menuFocus].enabled)
        {
            const MenuRow& row = rows[g_menuFocus];
            int step = 0;
            const bool ctrl = ImGui::GetIO().KeyCtrl; // Ctrl+arrows are the clip's start and end
            if (!ctrl && KeyPressed(ImGuiKey_LeftArrow))
                step = -1;
            if (!ctrl && KeyPressed(ImGuiKey_RightArrow))
                step = 1;
            if (KeyPressed(ImGuiKey_PageUp))
                step = 5;
            if (KeyPressed(ImGuiKey_PageDown))
                step = -5;
            if (row.label == TR("marker.time", "Time") && (step == 5 || step == -5))
                step *= 2; // ten frames
            if (step != 0 && row.kind == 0 && row.step)
            {
                PlayUiStep(step);
                row.step(step);
            }
            if (Confirmed())
            {
                const bool runs = static_cast<bool>(row.enter) || static_cast<bool>(row.step);
                if (runs && row.typed)
                {
                    PlayUi(UiSound::Select);
                    StartRowTyping(row);
                }
                else if (runs && row.enter)
                {
                    PlayUi(UiSound::Select);
                    row.enter();
                }
                else if (runs && row.step)
                {
                    PlayUiStep(1);
                    row.step(1);
                }
            }
        }
        else if (count > 0 && (Confirmed() ||
                                  (!ImGui::GetIO().KeyCtrl && (KeyPressed(ImGuiKey_LeftArrow, false) || KeyPressed(ImGuiKey_RightArrow, false)))))
            PlayUi(UiSound::Error); // the arrows on a greyed row too
        OrnamentDivider(draw, 40.0f, y + 6.0f, 300.0f);
        if (count > 0)
            Paragraph(draw, 40.0f, y + 18.0f, 15.0f, kMuted, rows[g_menuFocus].help.c_str(), 300.0f);
        if (!rowTyping && Back())
        {
            PlayUi(UiSound::Back);
            if (g_splineDefaultsOpen && !selected)
            {
                g_splineDefaultsOpen = false; // back to the free camera's menu, on its row
                g_menuFocus = static_cast<int>(BuildFreeCamRows().size()) - 1;
            }
            else if (g_freeCamMenuOpen && !selected)
                g_freeCamMenuOpen = false;
            else if (g_markerMenu == MarkerMenu::SplineDefaults)
            {
                g_markerMenu = MarkerMenu::Spline;
                g_menuFocus = g_splineDefaultsReturn;
            }
            else if (g_markerMenu == MarkerMenu::Spline)
            {
                g_markerMenu = MarkerMenu::Camera;
                g_menuFocus = g_splineMenuReturn;
            }
            else if (g_markerMenu == MarkerMenu::ShakeAdvanced)
            {
                g_markerMenu = MarkerMenu::Shake;
                g_menuFocus = g_shakeAdvancedReturn;
            }
            else if (g_markerMenu != MarkerMenu::Root)
            {
                g_menuFocus = g_markerMenu == MarkerMenu::Camera ? 0 : g_markerMenu == MarkerMenu::Dof ? 1
                    : g_markerMenu == MarkerMenu::Effects ? 2 : g_menuReturnFocus;
                g_markerMenu = MarkerMenu::Root;
            }
            else
                g_selectedKeyframeId = 0; // the menu closes; Esc again goes home
            g_backConsumed = true;
        }
    }
}

    // The timeline's zoom (Ctrl + wheel over it): the window of the clip shown,
    // centred on the playhead.
    double g_timelineZoom = 1.0;

namespace
{
    double g_viewStartMs = 0.0;
    double g_viewSpanMs = 1.0;

    std::uint64_t TimeAt(float mouseX, float left, float width)
    {
        const float t = width > 1.0f ? std::clamp((mouseX - S(left)) / S(width), 0.0f, 1.0f) : 0.0f;
        return static_cast<std::uint64_t>(g_viewStartMs + t * g_viewSpanMs + 0.5);
    }

    // The edit timeline: the playback bar, thin and red, with the camera
    // keyframes on it; a diamond where a shot starts (a cut) or a key stands
    // apart, a tick for the keys inside a directed shot. Click or drag the bar
    // to seek, a mark to select or move.
    void DrawEditTimeline(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("EditTimeline");
        const float left = 40.0f;
        const float right = g_vw - 40.0f;
        const float width = right - left;
        // Over the key bar.
        const float y = g_vh - g_footH - 54.0f; // the bar
        // A soft band behind the bar keeps it readable over the game.
        draw->AddRectFilledMultiColor(P(0.0f, y - 72.0f), P(g_vw, g_vh), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
            IM_COL32(8, 7, 6, 200), IM_COL32(8, 7, 6, 200));

        const double clipMs = g_snapshot.durationMs ? static_cast<double>(g_snapshot.durationMs) : 1.0;
        // A new clip starts unzoomed.
        static std::uint32_t zoomClip = ~0u; // per clip load, not per name
        if (zoomClip != g_snapshot.clipLoadSequence)
        {
            zoomClip = g_snapshot.clipLoadSequence;
            g_timelineZoom = 1.0;
        }
        g_viewSpanMs = clipMs / g_timelineZoom;
        // A drag the timeline never saw end (F2, Esc, H or Edit Camera while the
        // button was held) would hold the window and keep the selection from
        // closing.
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            // This runs before the release check below, which would clear the drag on
            // the release frame first: the drag's end (kSeekDragEnd) is sent here, once.
            if (g_draggingPlayhead && g_lastDragTimeMs != UINT64_MAX)
            {
                Command command{};
                command.timeMs = g_lastDragTimeMs;
                command.enabled = kSeekDragEnd;
                Send(CommandType::SeekAbsolute, &command);
            }
            g_draggingKeyframeId = 0;
            g_draggingPlayhead = false;
        }
        // The window holds while the mouse drags (re-centred on every seek, the same
        // mouse x would mean a later time each frame).
        if (!g_draggingPlayhead && !g_draggingKeyframeId)
            g_viewStartMs = std::clamp(static_cast<double>(g_snapshot.playheadMs) - g_viewSpanMs * 0.5, 0.0, clipMs - g_viewSpanMs);
        const double duration = g_viewSpanMs; // below: times map through the window (TimeX)
        const auto TimeX = [&](double timeMs) { return left + static_cast<float>((timeMs - g_viewStartMs) / duration) * width; };
        const float amount = std::clamp(static_cast<float>((g_snapshot.playheadMs - g_viewStartMs) / duration), 0.0f, 1.0f);
        const float playX = left + amount * width;
        // The painted timeline: a torn track, the played part a torn red strip, the
        // marks on the bar as icons, the playhead a watch crown. Without its art,
        // the flat timeline.
        const bool painted = TimelineArtReady();
        if (painted)
        {
            TornStrip(draw, ui_art::Art::BarTorn, left, left, right, y + kTrackTop, y + kTrackBottom, kTrackTint);
            TornStrip(draw, ui_art::Art::BarRed, left, left, playX, y + kTrackTop, y + kTrackBottom, IM_COL32(255, 255, 255, 255));
            // The crown is drawn once the marks are laid out (below): it stands over
            // the selected marker's ring when the playhead is on it.
        }
        else
        {
            draw->AddRectFilled(P(left, y), P(right, y + 5.0f), IM_COL32(236, 230, 218, 70));
            draw->AddRectFilled(P(left, y), P(playX, y + 5.0f), kRed);
            // The playhead: a line through the bar and a small head under it.
            // Drawn before the marks, so its ring never covers a marker added at the
            // playhead.
            draw->AddLine(P(playX, y - 6.0f), P(playX, y + 9.0f), kText, S(2.0f));
            draw->AddTriangleFilled(P(playX - 5.0f, y + 15.0f), P(playX + 5.0f, y + 15.0f), P(playX, y + 8.0f), kText);
        }

        // Interaction area: the bar and the marks above it (the stacked
        // lanes included).
        ImGui::SetCursorScreenPos(P(left, y - 48.0f));
        ImGui::InvisibleButton("##timeline", ImVec2(S(width), S(60.0f)));
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 mouse = ImGui::GetMousePos();

        // Markers too close to share a spot stack in lanes above the bar instead of
        // drawing over each other (a tick on the bar when all the lanes are taken);
        // the selected one is drawn last, larger, ringed, with a stem down to the
        // bar and its number over it. Painted, the first lane is the bar's middle
        // (the icons sit on the bar); the hit circles move with them, the same size.
        const float markY = painted ? y + (kTrackTop + kTrackBottom) * 0.5f : y - 11.0f;
        constexpr int kLanes = 3;
        constexpr float kLaneStep = 13.0f;
        constexpr float kMarkGap = 13.0f; // two diamonds side by side
        struct MarkLayout
        {
            float x = 0.0f;
            float y = 0.0f;
            bool tick = false; // no lane left: a tick on the bar
            bool hidden = false; // outside the zoomed window
        };
        const std::uint32_t keyCount = g_snapshot.cameraKeyframeCount;
        std::vector<MarkLayout> marks(keyCount);
        float laneEnd[kLanes];
        for (float& end : laneEnd)
            end = -1.0e9f;
        const float endX = keyCount >= 2
            ? TimeX(static_cast<double>(g_snapshot.cameraKeyframes[keyCount - 1].timeMs)) : 1.0e9f;
        std::uint32_t hoveredKey = 0;
        float best = S(painted ? 11.0f : 9.0f); // painted, about the icon's size
        for (std::uint32_t n = 0; n < keyCount; ++n)
        {
            const CameraKeyframe& key = g_snapshot.cameraKeyframes[n];
            MarkLayout& mark = marks[n];
            mark.x = TimeX(static_cast<double>(key.timeMs));
            mark.hidden = mark.x < left - 1.0f || mark.x > right + 1.0f;
            if (mark.hidden)
                continue;
            const bool bound = keyCount >= 2 && (n == 0 || n + 1 == keyCount);
            int lane = 0;
            if (!bound)
            {
                // Lane 0 is also taken next to the end bracket.
                while (lane < kLanes && (mark.x - laneEnd[lane] < kMarkGap || (lane == 0 && endX - mark.x < kMarkGap)))
                    ++lane;
            }
            if (lane < kLanes)
            {
                laneEnd[lane] = mark.x;
                mark.y = markY - lane * kLaneStep;
            }
            else
            {
                mark.tick = key.id != g_selectedKeyframeId;
                mark.y = markY; // the selected one is drawn over the others
            }
            if (!mark.tick)
            {
                const float d = std::hypot(mouse.x - S(mark.x), mouse.y - S(mark.y));
                if (d < best)
                {
                    best = d;
                    hoveredKey = key.id;
                }
            }
        }
        // The painted playhead. On the selected marker (the playhead goes there when
        // a marker is selected) the crown would sit on the marker's ring: it rises
        // instead, its needle's tip on the ring's top, a little smaller to stay clear
        // of the shake lane, and the marker's number goes beside the crown. It eases
        // up over the last few units as the playhead nears the marker. Drawing only:
        // the hit areas and the drag are the bar's and the marks', as before.
        float crownNumberX = -1.0f, crownNumberY = 0.0f;
        if (painted)
        {
            float lift = 0.0f;
            for (std::uint32_t n = 0; n < keyCount; ++n)
            {
                const MarkLayout& mark = marks[n];
                if (g_snapshot.cameraKeyframes[n].id != g_selectedKeyframeId || mark.hidden || mark.tick)
                    continue;
                const bool bound = keyCount >= 2 && (n == 0 || n + 1 == keyCount);
                if (bound || mark.y == markY) // on the bar (a marker up a lane has its stem)
                    lift = std::clamp((17.0f - std::fabs(mark.x - playX)) / 8.0f, 0.0f, 1.0f);
                break;
            }
            const float tipY = (y + kTrackBottom + 2.0f) + lift * ((markY - 13.0f) - (y + kTrackBottom + 2.0f));
            const float crownH = 38.0f - lift * 3.0f;
            PlayheadIcon(draw, playX, tipY, crownH);
            if (lift > 0.5f)
            {
                // The crown's ring: rows 1 to 58 of the art (centre 105 rows over the tip), 56 of its 136 columns.
                crownNumberX = playX + crownH * 28.0f / kPlayheadRows + 5.0f;
                crownNumberY = tipY - crownH * 105.0f / kPlayheadRows - LabelHeight(14.0f) * 0.5f;
            }
        }
        const auto drawMark = [&](std::uint32_t n) {
            const CameraKeyframe& key = g_snapshot.cameraKeyframes[n];
            const MarkLayout& mark = marks[n];
            const float x = mark.x;
            const float my = mark.y;
            if (mark.hidden)
                return;
            if (mark.tick)
            {
                if (painted) // the pin, small, on the bar
                    OutlinedIcon(draw, ui_art::Art::IconKeyTick, x, markY, 14.0f, IM_COL32(222, 222, 222, 200));
                else
                    draw->AddLine(P(x, y - 6.0f), P(x, y - 1.0f), IM_COL32(233, 178, 64, 170), S(1.0f));
                return;
            }
            const bool selected = key.id == g_selectedKeyframeId;
            const bool hot = hovered && key.id == hoveredKey;
            const float r = selected ? 7.0f : hot ? 6.0f : 5.0f;
            if (painted)
            {
                DrawPaintedMark(draw, n, x, my, y, selected, hot, crownNumberX, crownNumberY);
                return;
            }
            const ImU32 color = selected ? IM_COL32(255, 229, 142, 255) : hot ? IM_COL32(245, 205, 110, 255) : kGold;
            const bool bound = keyCount >= 2 && (n == 0 || n + 1 == keyCount);
            if (selected && !bound)
                draw->AddLine(P(x, my + r), P(x, y), IM_COL32(255, 229, 142, 200), S(1.5f));
            if (bound)
            {
                // The clip's start and end: a cut bracket, [ and ].
                const float dir = n == 0 ? 1.0f : -1.0f;
                draw->AddLine(P(x, markY - 9.0f), P(x, y + 5.0f), color, S(selected ? 4.0f : 3.0f));
                draw->AddLine(P(x, markY - 9.0f), P(x + dir * 6.0f, markY - 9.0f), color, S(3.0f));
                draw->AddLine(P(x, y + 5.0f), P(x + dir * 6.0f, y + 5.0f), color, S(3.0f));
            }
            else if (key.markerType == MarkerType::Anchor)
                draw->AddLine(P(x, my - r), P(x, my + r), color, S(2.0f));
            else
                draw->AddQuadFilled(P(x, my - r), P(x + r, my), P(x, my + r), P(x - r, my), color);
            if (selected)
            {
                const float ringY = bound ? markY - 9.0f : my;
                draw->AddCircle(P(x, ringY), S(r + 5.0f), kText, 0, S(1.5f));
                char number[8]{};
                std::snprintf(number, sizeof(number), "%u", n + 1);
                Text(draw, x - TextWidth(14.0f, number) / g_scale * 0.5f, ringY - r - 24.0f, 14.0f, kText, number);
            }
        };

        // Blend bands: a thin gold line from a marker that blends to the next one
        // (straight for Linear, a wave for Smooth); a cut has none.
        for (std::uint32_t n = 0; n + 1 < g_snapshot.cameraKeyframeCount; ++n)
        {
            const CameraKeyframe& key = g_snapshot.cameraKeyframes[n];
            if (key.blend == Blend::None || key.markerType == MarkerType::Anchor)
                continue;
            const float x0 = std::clamp(TimeX(static_cast<double>(key.timeMs)), left, right);
            const float x1 = std::clamp(TimeX(static_cast<double>(g_snapshot.cameraKeyframes[n + 1].timeMs)), left, right);
            const float by = painted ? y + kTrackTop - 5.0f : y - 20.0f; // just over the torn bar
            if (key.blend == Blend::Linear)
                draw->AddLine(P(x0, by), P(x1, by), IM_COL32(233, 178, 64, 200), S(2.0f));
            else
            {
                ImVec2 last = P(x0, by);
                for (float x = x0 + 3.0f; x <= x1; x += 3.0f)
                {
                    const ImVec2 point = P(x, by + std::sin((x - x0) * 0.35f) * 1.6f);
                    draw->AddLine(last, point, IM_COL32(233, 178, 64, 200), S(2.0f));
                    last = point;
                }
            }
        }

        // The marks over the bands, the selected one last (on top).
        std::uint32_t selectedIndex = keyCount;
        for (std::uint32_t n = 0; n < keyCount; ++n)
        {
            if (g_snapshot.cameraKeyframes[n].id == g_selectedKeyframeId)
                selectedIndex = n;
            else
                drawMark(n);
        }
        if (selectedIndex < keyCount)
            drawMark(selectedIndex);

        // The shake lives in the markers: a small cyan dot over each marker whose
        // shake is on (a ring on an Anchor the shake migration added), a thin line on
        // to the next shake marker while it plays. Drawing only.
        {
            constexpr ImU32 kShakeBadge = IM_COL32(110, 190, 235, 255);
            const float laneY = y - 58.0f;
            float lastX = left;
            bool lastOn = false;
            const auto run = [&](float x0, float x1) {
                x0 = std::clamp(x0, left, right);
                x1 = std::clamp(x1, left, right);
                if (x1 > x0)
                    draw->AddLine(P(x0, laneY), P(x1, laneY), IM_COL32(110, 190, 235, 90), S(1.5f));
            };
            for (std::uint32_t n = 0; n < keyCount; ++n)
            {
                const CameraKeyframe& key = g_snapshot.cameraKeyframes[n];
                if (key.markerType == MarkerType::Anchor && key.markerOrigin != 1)
                    continue; // a plain Anchor holds no shake
                const float x = TimeX(static_cast<double>(key.timeMs));
                if (lastOn)
                    run(lastX, x);
                const bool on = g_snapshot.cameraKeyframeShake[n].on != 0;
                if (on && x >= left - 1.0f && x <= right + 1.0f)
                {
                    // Painted, the spur rowel over the marker (dimmer on an Anchor the migration
                    // added; the selected marker's white on its red halo).
                    if (painted && key.id == g_selectedKeyframeId && ui_art::Get(ui_art::Art::IconShakeSelected))
                        Icon(draw, ui_art::Art::IconShakeSelected, x, laneY, 20.0f, IM_COL32(255, 255, 255, 255));
                    else if (painted)
                        Icon(draw, ui_art::Art::IconShake, x, laneY, 15.0f,
                            key.markerOrigin == 1 ? IM_COL32(222, 222, 222, 150) : kIconTint);
                    else if (key.markerOrigin == 1)
                        draw->AddCircle(P(x, laneY), S(3.5f), kShakeBadge, 0, S(1.5f));
                    else
                        draw->AddCircleFilled(P(x, laneY), S(3.0f), kShakeBadge);
                }
                lastX = x;
                lastOn = on;
            }
            if (lastOn)
                run(lastX, TimeX(static_cast<double>(g_snapshot.durationMs)));
        }

        char now[16]{};
        char total[16]{};
        FormatClock(g_snapshot.playheadMs, now, sizeof(now), true);
        FormatClock(g_snapshot.durationMs, total, sizeof(total), true);
        char time[48]{};
        std::snprintf(time, sizeof(time), "%s / %s", now, total);
        // The status line laid out one after the other, each at its own width; when
        // it and the spans' legend are wider than the bar, the clip's name, then the
        // message, are cut with "..." (at 4:3 the legend would leave the screen).
        struct StatusItem
        {
            ImU32 color;
            std::string text;
            float minWidth;
            bool flexible;
        };
        std::vector<StatusItem> items;
        const auto status = [&](ImU32 color, const char* text, float minWidth, bool flexible = false) {
            items.push_back({color, text, minWidth, flexible});
        };
        status(kText, time, 180.0f);
        // A seek's wait and the world's load too: the clip editor's cue is here (no badge over it).
        const bool busy = Flag(SnapshotPreparing) || SeekCueWanted() || Flag(SnapshotLoadingWorld);
        const char* state = busy ? TR("hud.loading", "Loading") : Flag(SnapshotPaused) ? TR("hud.paused", "Paused") : "";
        status(kMuted, state, 100.0f);
        std::string clip;
        if (ProjectFlag(ProjectEditingClip) && g_snapshot.projectActiveClip < g_snapshot.projectClipCount)
            // A project's clip: which one, of how many.
            clip = TRF("edit.status.project_clip", "{project}: clip {at} of {count}, {clip}", {{"project", g_snapshot.projectName},
                {"at", std::to_string(g_snapshot.projectActiveClip + 1)}, {"count", std::to_string(g_snapshot.projectClipCount)},
                {"clip", ClipLabel(g_snapshot.currentClip)}});
        else
            clip = g_snapshot.currentClip[0] ? ClipLabel(g_snapshot.currentClip) : std::string(TR("edit.status.last_recording", "last recording"));
        status(kMuted, clip.c_str(), 100.0f, true);
        if (g_timelineZoom > 1.01)
            status(kMuted, TRF("edit.status.zoom", "Zoom x{zoom}", {{"zoom", replay::i18n::Printf("%.1f", g_timelineZoom)}}).c_str(), 80.0f);
        // Marker changes not in the file yet (Ctrl+S; leaving the clip saves them
        // too).
        if (GetTickCount64() < g_editMessageUntil)
            status(kGold, g_editMessage, 0.0f, true);
        else if (Flag(SnapshotSavingClip))
            status(kGold, TR("edit.status.saving", "Saving..."), 0.0f);
        else if (Flag(SnapshotClipEdited))
            status(kGold, KeyFill(TR("edit.status.unsaved", "Unsaved changes ({key_save})")), 0.0f);
        {
            const auto itemWidth = [](const StatusItem& item) { return std::max(item.minWidth, TextWidth(16.0f, item.text.c_str()) / g_scale + 24.0f); };
            float used = SpanLegendWidth();
            for (const StatusItem& item : items)
                used += itemWidth(item);
            for (StatusItem& item : items)
            {
                const float excess = used - width;
                if (excess <= 0.0f)
                    break;
                if (!item.flexible)
                    continue;
                const float before = itemWidth(item);
                const float textW = TextWidth(16.0f, item.text.c_str()) / g_scale;
                item.text = Ellipsized(16.0f, item.text, std::max(60.0f, textW - excess));
                used += itemWidth(item) - before;
            }
        }
        float statusX = left;
        for (std::size_t n = 0; n < items.size(); ++n)
        {
            const StatusItem& item = items[n];
            Text(draw, statusX, y + 14.0f, 16.0f, item.color, item.text.c_str());
            // The turning cylinder in the gap before "Loading" (the state, item 1).
            if (n == 1 && busy)
                LoadingCue(draw, statusX - 12.0f, y + 14.0f + LabelHeight(16.0f) * 0.5f, 18.0f);
            statusX += std::max(item.minWidth, TextWidth(16.0f, item.text.c_str()) / g_scale + 24.0f);
        }
        // The spans' legend after the status.
        DrawSpanBands(draw, left, width, y + 5.0f, statusX, y + 14.0f, g_viewStartMs, g_viewSpanMs);

        // The wheel over the timeline steps frames (a notch, one frame; with Shift,
        // ten).
        if (hovered && ImGui::GetIO().MouseWheel != 0.0f && ImGui::GetIO().KeyCtrl)
        {
            // Ctrl + wheel zooms the timeline (x1 to x32).
            g_timelineZoom = std::clamp(g_timelineZoom * (ImGui::GetIO().MouseWheel > 0.0f ? 1.5 : 1.0 / 1.5), 1.0, 32.0);
        }
        else if (hovered && ImGui::GetIO().MouseWheel != 0.0f)
        {
            Command step{};
            step.direction = ImGui::GetIO().MouseWheel > 0.0f ? 1 : -1;
            const int count = ImGui::GetIO().KeyShift ? 10 : 1;
            for (int n = 0; n < count; ++n)
                Send(CommandType::StepFrame, &step);
        }
        if (hovered && MouseClicked())
        {
            // A click that came and went inside one frame: the button is up already, so
            // no drag follows; the bar is sought here, once (a marker is selected and
            // sought below as always).
            if (!hoveredKey && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                Command seek{};
                seek.timeMs = TimeAt(mouse.x, left, width);
                Send(CommandType::SeekAbsolute, &seek);
            }
            if (hoveredKey)
            {
                // Selecting a marker sounds like the menus do.
                PlayUi(UiSound::Select);
                g_selectedKeyframeId = hoveredKey;
                g_draggingKeyframeId = hoveredKey;
                g_lastDragTimeMs = UINT64_MAX;
                // The playhead jumps to the key clicked (paused there).
                if (const CameraKeyframe* key = FindKeyframe(hoveredKey))
                {
                    Command seek{};
                    seek.timeMs = key->timeMs;
                    Send(CommandType::SeekAbsolute, &seek);
                    g_lastDragTimeMs = key->timeMs; // a click without a move does not move the key
                    g_dragKeyStartMs = key->timeMs;
                }
                g_dragStartX = mouse.x;
                g_dragMoved = false;
                ForgetPendingMarkerValues(); // the drag moves it from the snapshot's time
            }
            else
            {
                g_draggingPlayhead = true;
                g_lastDragTimeMs = UINT64_MAX;
            }
        }
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && (g_draggingKeyframeId || g_draggingPlayhead))
        {
            std::uint64_t timeMs = TimeAt(mouse.x, left, width);
            // A marker moves only once the mouse has gone 4 px, and by how far it went,
            // so a click anywhere on the mark (9 px around it) never moves it.
            if (g_draggingKeyframeId)
            {
                if (!g_dragMoved && std::fabs(mouse.x - g_dragStartX) < S(4.0f))
                    timeMs = g_lastDragTimeMs;
                else
                {
                    g_dragMoved = true;
                    const std::int64_t delta = static_cast<std::int64_t>(timeMs) -
                        static_cast<std::int64_t>(TimeAt(g_dragStartX, left, width));
                    timeMs = static_cast<std::uint64_t>(std::clamp<std::int64_t>(
                        static_cast<std::int64_t>(g_dragKeyStartMs) + delta, 0, static_cast<std::int64_t>(g_snapshot.durationMs)));
                }
            }
            if (timeMs != g_lastDragTimeMs)
            {
                Command command{};
                command.timeMs = timeMs;
                if (g_draggingKeyframeId)
                {
                    command.keyframeId = g_draggingKeyframeId;
                    Send(CommandType::MoveCameraKeyframe, &command);
                }
                else
                {
                    // The drag is a scrub (cheap, no load) until the mouse is released.
                    command.enabled = kSeekDragging;
                    Send(CommandType::SeekAbsolute, &command);
                }
                g_lastDragTimeMs = timeMs;
            }
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            // The drag's end loads once, where it stopped.
            if (g_draggingPlayhead && g_lastDragTimeMs != UINT64_MAX)
            {
                Command command{};
                command.timeMs = g_lastDragTimeMs;
                command.enabled = kSeekDragEnd;
                Send(CommandType::SeekAbsolute, &command);
            }
            g_draggingKeyframeId = 0;
            g_draggingPlayhead = false;
        }

        // The keys of what has the keyboard (the marker's menu or the timeline) are
        // in the key bar under the timeline (EditTips).
    }
}

    // Edit Camera: the controls at the foot, a thin cross in the middle, the
    // zoom; Enter keeps the view in the marker, Backspace leaves it as it was.
    bool g_editOverlayArmed = false;

    void DrawEditCameraOverlay(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("EditCamera");
        const float cx = g_vw * 0.5f, cy = g_vh * 0.5f;
        draw->AddLine(P(cx - 14.0f, cy), P(cx + 14.0f, cy), IM_COL32(236, 230, 218, 120), S(1.0f));
        draw->AddLine(P(cx, cy - 14.0f), P(cx, cy + 14.0f), IM_COL32(236, 230, 218, 120), S(1.0f));
        const CameraKeyframe* key = FindKeyframe(g_snapshot.editingMarker);
        const std::string title = TRF("editcam.title", "EDIT CAMERA  {zoom}X ZOOM",
            {{"zoom", replay::i18n::Printf("%.2f", kMarkerFovDefault / std::max(1.0f, g_snapshot.camera.fov))}});
        Brush(draw, 40.0f, 40.0f, TextWidth(26.0f, title.c_str()) / g_scale + 40.0f, 40.0f, kRed);
        Text(draw, 54.0f, 46.0f, 26.0f, kText, title.c_str());
        const bool orbit = key && key->lookAtTarget && key->cameraMode == CameraMode::Free;
        if (orbit)
            Text(draw, 40.0f, 90.0f, 16.0f, kMuted, TR("editcam.orbit", "Orbit around the Look At target; the mouse places it on screen."));
        // Its controls are in the key bar (EditCameraTips). Enter or Backspace only
        // after both were seen up since the overlay came (the Enter that opened Edit
        // Camera, held or repeated, would close it again); Esc cancels too.
        if (!g_runtime->is_key_down(VK_RETURN) && !g_runtime->is_key_down(VK_BACK) && !g_runtime->is_key_down(VK_ESCAPE))
            g_editOverlayArmed = true;
        const bool keep = g_editOverlayArmed && KeyVk(VK_RETURN);
        const bool cancel = g_editOverlayArmed && (KeyVk(VK_BACK) || KeyVk(VK_ESCAPE));
        if (keep || cancel)
        {
            g_editOverlayArmed = false;
            Command command{};
            command.keyframeId = g_snapshot.editingMarker;
            command.enabled = 0;
            command.target = keep ? 0u : 1u;
            Send(CommandType::EditCameraKeyframe, &command);
        }
    }

    void DrawEdit(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("Edit");
        DrawClipFade(draw);
        DrawEditPanel(draw);
        DrawEditTimeline(draw);
        if (g_fxTypingFrame)
            return; // the shake event's typed Value or Time has the keys
        // Ctrl+S saves the clip with its markers.
        if (ImGui::GetIO().KeyCtrl && KeyPressed(ImGuiKey_S, false))
        {
            // A clip with its own file and no changes is not written again (a
            // compressed clip would go back to full size); the core skips it too (and
            // retries a held save). A buffer clip with no file yet is still saved.
            const bool unchanged = Flag(SnapshotPlaying) && g_snapshot.currentClip[0] && !Flag(SnapshotClipEdited);
            if (Send(CommandType::SaveOpenClip))
            {
                PlayUi(UiSound::Select);
                if (unchanged)
                {
                    g_saveAskedAt = 0;
                    EditMessage(TR("edit.message.no_changes", "No changes"));
                }
                else
                {
                    g_saveAskedAt = GetTickCount64();
                    g_saveSeen = false;
                }
            }
        }
        // The save's end is said. A failure keeps "Unsaved changes" and the core's
        // notice; only the success is confirmed here.
        if (g_saveAskedAt)
        {
            const bool saving = Flag(SnapshotSavingClip);
            g_saveSeen = g_saveSeen || saving;
            const ULONGLONG waited = GetTickCount64() - g_saveAskedAt;
            if ((g_saveSeen && !saving) || (!g_saveSeen && waited > 3000))
            {
                g_saveAskedAt = 0;
                if (!Flag(SnapshotClipEdited))
                    EditMessage(TR("edit.message.saved", "Saved"));
            }
            else if (waited > 15000)
                g_saveAskedAt = 0;
        }
        // Ctrl+Z / Ctrl+Y (Ctrl+Shift+Z) undo and redo marker changes.
        if (ImGui::GetIO().KeyCtrl && (KeyPressed(ImGuiKey_Z, true) || KeyPressed(ImGuiKey_Y, true)))
        {
            const bool redo = KeyPressed(ImGuiKey_Y, true) || ImGui::GetIO().KeyShift;
            if (!Flag(redo ? SnapshotCanRedo : SnapshotCanUndo))
                PlayUi(UiSound::Error);
            else if (Send(redo ? CommandType::RedoMarkers : CommandType::UndoMarkers))
            {
                PlayUi(redo ? UiSound::NavRight : UiSound::NavLeft);
                ForgetPendingMarkerValues();
            }
        }
        // Render export: Ctrl+E opens the export dialog.
        if (ImGui::GetIO().KeyCtrl && KeyPressed(ImGuiKey_E, false))
        {
            PlayUi(UiSound::Select);
            g_screen = Screen::Export;
            exportui::g_back = Screen::Edit;
            if (exportui::g_range == 2)
                exportui::g_range = 1; // the clip's range from the clip editor
            return;
        }
        // P: a photo of the frame shown, saved by ReShade to its screenshot folder.
        if (!ImGui::GetIO().KeyCtrl && KeyPressed(ImGuiKey_P, false))
        {
            g_runtime->save_screenshot("RedemptionEditor");
            PlayUi(UiSound::Select);
            EditMessage(TR("edit.message.photo", "Photo saved (ReShade screenshot folder)"));
        }
        // T: the frame shown becomes the clip's thumbnail.
        if (!ImGui::GetIO().KeyCtrl && KeyPressed(ImGuiKey_T, false))
        {
            if (!g_snapshot.currentClip[0])
            {
                // The replay buffer played before it was saved has no file to hold a
                // thumbnail.
                PlayUi(UiSound::Error);
                static std::string saveFirst; // EditMessage keeps the pointer
                saveFirst = replay::keys::Fill(TR("edit.message.save_first", "Save the clip first ({key_save})"), Keys());
                EditMessage(saveFirst.c_str());
            }
            else if (clip_thumbnails::SetFromScreen(g_runtime, g_snapshot.currentClip))
            {
                PlayUi(UiSound::Select);
                EditMessage(TR("edit.message.thumbnail", "Thumbnail set"));
            }
            else
            {
                PlayUi(UiSound::Error);
                EditMessage(TR("edit.message.thumbnail_failed", "Could not take the thumbnail"));
            }
        }
        // Ctrl+Left / Ctrl+Right: the clip's start and end (Home opens the Social
        // Club overlay).
        const bool toStart = ImGui::GetIO().KeyCtrl && KeyPressed(ImGuiKey_LeftArrow, false);
        const bool toEnd = ImGui::GetIO().KeyCtrl && KeyPressed(ImGuiKey_RightArrow, false);
        if (toStart || toEnd)
        {
            Command seek{};
            seek.timeMs = toEnd ? g_snapshot.durationMs : 0;
            Send(CommandType::SeekAbsolute, &seek);
            PlayUi(toEnd ? UiSound::NavRight : UiSound::NavLeft);
        }
        // F7 stops the clip here too, as the playback bar and the help say (with the
        // editor open the key never reaches the core). The [Keys] table's keys
        // (editor_keys.h); Space stays fixed.
        if (ActionPressed(Action::Stop))
        {
            PlayUi(UiSound::Back);
            Send(CommandType::Stop);
        }
        if (KeyPressed(ImGuiKey_Space, false) || ActionPressed(Action::PlayPause)) // no repeat (a held Space would flicker play and pause)
            Send(CommandType::PlayPause);
        if (ActionPressed(Action::FreeCam))
            Send(CommandType::ToggleFreeCam);
        // Held , . keep stepping (the key repeat), as the arrows do.
        const bool stepBack = ActionPressed(Action::FrameBack, true);
        if (stepBack || ActionPressed(Action::FrameForward, true))
        {
            Command command{};
            command.direction = stepBack ? -1 : 1;
            Send(CommandType::StepFrame, &command);
        }
        // Rewind / FastForward (no key by default) seek half a second per repeat
        // while held (about ten times real time).
        const bool rewind = ActionPressed(Action::Rewind, true);
        if (rewind || ActionPressed(Action::FastForward, true))
            SeekBy(rewind ? -kRewindStepMs : kRewindStepMs);
        const bool slower = ActionPressed(Action::SpeedDown);
        if (slower || ActionPressed(Action::SpeedUp))
            StepPlaybackSpeed(slower ? -1 : 1); // from the speed last asked
        // Enter adds a marker at the playhead (with the settings of the marker
        // before it); with a marker's menu open the keys are its.
        if (!g_markerMenuActive && Confirmed())
            AddKeyframeHere();
        // Tab / Ctrl+Tab (or Shift+Tab): the next or previous marker (the playhead
        // goes along). Ctrl too, because Shift+Tab opens the Steam overlay on Steam
        // copies of the game.
        if (KeyPressed(ImGuiKey_Tab) && g_snapshot.cameraKeyframeCount > 0)
        {
            const bool backwards = ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl;
            // From the selected marker when there is one: the snapshot's playhead lags
            // the seek a frame or two, and a quick second Tab would pick the same marker
            // again.
            const CameraKeyframe* from = FindKeyframe(g_selectedKeyframeId);
            const std::uint64_t reference = from ? from->timeMs : g_snapshot.playheadMs;
            const CameraKeyframe* pick = nullptr;
            for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
            {
                const CameraKeyframe& key = g_snapshot.cameraKeyframes[backwards ? g_snapshot.cameraKeyframeCount - 1 - n : n];
                if (backwards ? key.timeMs + 1 < reference : key.timeMs > reference + 1)
                {
                    pick = &key;
                    break;
                }
            }
            if (!pick)
                pick = &g_snapshot.cameraKeyframes[backwards ? g_snapshot.cameraKeyframeCount - 1 : 0];
            g_selectedKeyframeId = pick->id;
            PlayUi(backwards ? UiSound::NavUp : UiSound::NavDown);
            Command seek{};
            seek.timeMs = pick->timeMs;
            Send(CommandType::SeekAbsolute, &seek);
        }
        if (g_markerMenuActive && ImGui::GetIO().KeyCtrl && KeyPressed(ImGuiKey_C))
        {
            Command c{};
            c.keyframeId = g_selectedKeyframeId;
            if (Send(CommandType::CopyCameraKeyframe, &c))
                PlayUi(UiSound::Select);
        }
        if (g_markerMenuActive && ImGui::GetIO().KeyCtrl && KeyPressed(ImGuiKey_V))
        {
            Command c{};
            c.keyframeId = g_selectedKeyframeId;
            ForgetPendingMarkerValues();
            if (!Flag(SnapshotMarkerCopied))
                PlayUi(UiSound::Error);
            else if (Send(CommandType::PasteCameraKeyframe, &c))
                PlayUi(UiSound::Select);
        }
        if (g_markerMenuActive && KeyPressed(ImGuiKey_Delete, false)) // a held Delete would go on deleting the next ones
            DeleteMarker(g_selectedKeyframeId);
        if (!g_markerMenuActive && !g_valueRowHovered && !ImGui::GetIO().KeyCtrl)
        {
            Command command{};
            if (KeyPressed(ImGuiKey_LeftArrow))
            {
                command.direction = -1;
                Send(CommandType::StepFrame, &command);
            }
            if (KeyPressed(ImGuiKey_RightArrow))
            {
                command.direction = 1;
                Send(CommandType::StepFrame, &command);
            }
        }
        // Esc goes to the home screen with the clip still open; F2 closes the
        // editor. An Esc the marker menu used (back a level, close the menu) stays
        // there.
        if (g_backConsumed)
            g_backConsumed = false;
        else if (Back())
        {
            PlayUi(UiSound::Back);
            RefreshClipList(false);
            // A project's clip goes back to the project.
            g_screen = ProjectFlag(ProjectEditingClip) ? Screen::Project : Screen::Home;
            g_homeIndex = kHomeEditRow;
        }
    }
