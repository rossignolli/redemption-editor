// replay_editor_addon_projects.cpp: the projects list, the project name screen and the project
// screen with its clip blocks.
// Split from replay_editor_addon.cpp (moved code); shared internals in replay_editor_addon_internal.h.

#include "replay_editor_addon_internal.h"

    // ---------------- Projects ----------------
    // The project screens: the project files, and the open project's timeline,
    // where the clips stand in order as blocks as wide as their trimmed length.
    // The core owns the project (script.cpp, Projects); the editor shows the
    // snapshot and sends commands, as everywhere else.
    int g_projectsIndex = 0;

namespace
{
    std::uint64_t g_projectsSeen = 0;
}
    std::uint32_t g_projectMessageSeen = 0;
    ULONGLONG g_projectMessageUntil = 0;
    std::string g_projectMessageShown;
    int g_editAfterOpen = -1;          // Enter on a clip: the clip editor once it has opened
    ULONGLONG g_editAfterOpenAt = 0;
    int g_dragBlock = -1;              // a block held with the mouse (moved where it is let go)

namespace
{
    float g_dragBlockFromX = 0.0f;
    bool g_dragBlockMoved = false;
}
    bool g_projectScrubbing = false;   // the project's time bar dragged

namespace
{
    std::uint64_t g_projectLastSeek = UINT64_MAX;
}
    // The name screen: a new project or a project renamed.
    bool g_projectNameNew = true;
    std::string g_projectNameFile;
    char g_projectNameText[64]{}; // cut to kMaxNameBytes by CleanName
    std::string g_projectsSelectFile; // the file the Projects list selects when it comes back (a project renamed)
    bool g_projectNameFocus = false;
    const char* g_projectNameError = "";

namespace
{
    Screen g_projectNameBack = Screen::Projects;
}

    void ShowProjectName(bool isNew, const char* file, const char* name, Screen back)
    {
        g_projectNameNew = isNew;
        g_projectNameFile = file ? file : "";
        std::snprintf(g_projectNameText, sizeof(g_projectNameText), "%s", name ? name : "");
        g_projectNameFocus = true;
        g_projectNameError = "";
        g_projectNameBack = back;
        g_screen = Screen::ProjectName;
    }

namespace
{

    // A key hint that is a button too (the Projects panel, the project's header):
    // its key cap and label, a click does what the key does. True when clicked.
    float KeyActionWidth(const char* key, const char* label) { return KeyHintWidth(key, label) - 22.0f; }

    bool KeyAction(ImDrawList* draw, const char* id, float x, float y, const char* key, const char* label, bool on = true)
    {
        const float width = KeyActionWidth(key, label);
        ImGui::SetCursorScreenPos(P(x - 6.0f, y - 4.0f));
        const bool clicked = ItemClicked(ImGui::InvisibleButton(id, ImVec2(S(width + 12.0f), S(28.0f))));
        if (on && HoverShown(ImGui::IsItemHovered()))
            draw->AddRect(P(x - 6.0f, y - 4.0f), P(x + width + 6.0f, y + 24.0f), IM_COL32(236, 230, 218, 90), S(3.0f), 0, S(1.0f));
        KeyHint(draw, x, y, key, label, on);
        return clicked && on;
    }

    // The project files the snapshot lists (the name screen refuses a taken
    // name before it sends it; the core checks again).
    std::vector<std::string> ListedProjectFiles()
    {
        std::vector<std::string> files;
        const std::uint32_t count = std::min(g_snapshot.projectListCount, kMaxProjectsInSnapshot);
        for (std::uint32_t n = 0; n < count; ++n)
            files.emplace_back(g_snapshot.projects[n].file);
        return files;
    }
}

    void DrawProjectName(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("ProjectName");
        draw->AddRectFilled(ImVec2(0.0f, 0.0f), ImGui::GetIO().DisplaySize, IM_COL32(0, 0, 0, 215));
        const std::string fileLine = TRF("rename.file", "File: {file}", {{"file", g_projectNameFile}});
        const char* const kHelp = TR("rename.project.help", "Letters, numbers, spaces, - and _. The file in ReplayProjects takes the name.");
        const NameBoxPlaces at = PlaceNameBox(g_projectNameNew ? nullptr : fileLine.c_str(), kHelp);
        // What the typed name gives, as the core makes it (CleanName): the file it
        // is saved as when that differs from what was typed, a taken name at once.
        std::string typed(g_projectNameText);
        while (!typed.empty() && typed.back() == ' ')
            typed.pop_back();
        while (!typed.empty() && typed.front() == ' ')
            typed.erase(typed.begin());
        const std::string clean = replay::project::CleanName(typed);
        const replay::project::RenameCheck check =
            replay::project::CheckRename(g_projectNameNew ? std::string() : g_projectNameFile, clean, ListedProjectFiles());
        std::string note;
        if (check == replay::project::RenameCheck::Exists)
            note = TR("project.msg.name_exists", "A project with that name exists.");
        else if (!clean.empty() && clean != typed)
            note = TRF("rename.project.saved_as", "Saved as: {file}", {{"file", replay::project::NameFile(clean)}});
        const char* const shownError = g_projectNameError[0] ? g_projectNameError : note.c_str();
        DialogPanel(draw, (shownError[0] ? at.errorY + ParagraphHeight(15.0f, shownError, 400.0f)
                                         : at.helpY + ParagraphHeight(15.0f, kHelp, 400.0f)) + kPanelPadBottom);
        const char* title = g_projectNameNew ? TR("rename.project.new", "New project") : TR("rename.project.title", "Rename project");
        const float width = TextWidth(40.0f, title) / g_scale;
        Text(draw, 640.0f - width / 2.0f, 210.0f, 40.0f, kText, title);
        draw->AddLine(P(440.0f, 268.0f), P(840.0f, 268.0f), kRed, S(3.0f));
        if (!g_projectNameNew)
            Paragraph(draw, 440.0f, 292.0f, 17.0f, kMuted, fileLine.c_str(), 400.0f);
        ImGui::SetCursorScreenPos(P(440.0f, at.inputY));
        ImGui::PushItemWidth(S(400.0f));
        if (g_projectNameFocus)
        {
            ImGui::SetKeyboardFocusHere();
            g_projectNameFocus = false;
        }
        const bool entered = ImGui::InputText("##projectname", g_projectNameText, sizeof(g_projectNameText),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll, nullptr, nullptr) || MissedTap(VK_RETURN);
        ImGui::PopItemWidth();
        Paragraph(draw, 440.0f, at.helpY, 15.0f, kMuted, kHelp, 400.0f);
        if (shownError[0])
            Paragraph(draw, 440.0f, at.errorY, 15.0f, kGold, shownError, 400.0f);
        if (ImGui::IsItemEdited())
            g_projectNameError = ""; // typing again clears the last refusal
        if (entered)
        {
            // Characters a name cannot hold become spaces (CleanName); a taken
            // name is refused here and never overwrites the other project.
            const bool ok = check != replay::project::RenameCheck::Empty && check != replay::project::RenameCheck::Exists;
            g_projectNameError = clean.empty() ? NameEmptyError()
                : check == replay::project::RenameCheck::Exists ? TR("project.msg.name_exists", "A project with that name exists.") : "";
            if (ok)
            {
                bool sent = true;
                if (g_projectNameNew)
                {
                    sent = SendNamed(CommandType::ProjectNew, clean.c_str());
                    g_projectOpenAsked = GetTickCount64();
                }
                else if (check != replay::project::RenameCheck::Same)
                {
                    sent = SendNamed(CommandType::ProjectRename, (g_projectNameFile + "|" + clean).c_str());
                    g_projectsSelectFile = replay::project::NameFile(clean); // the list keeps it selected
                }
                PlayUi(sent ? UiSound::Select : UiSound::Error);
                g_screen = g_projectNameNew ? Screen::Project : g_projectNameBack;
            }
            else
                PlayUi(UiSound::Error);
        }
        else if (!g_helpOpen && !g_helpSwallow && KeyPressed(ImGuiKey_Escape))
        {
            PlayUi(UiSound::Back);
            g_screen = g_projectNameBack;
        }
    }

    void DrawProjects(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("Projects");
        MenuColumn(draw, 660.0f);
        TitleBanner(draw, 50.0f, 20.0f, 420.0f, 70.0f, 62.0f, 72.0f, 30.0f, 44.0f, "REDEMPTION EDITOR");
        SectionTitle(draw, 80.0f, 100.0f, 560.0f, TR("projects.title", "PROJECTS"));
        // Row 0 makes a new project; the files follow, newest first.
        const int files = static_cast<int>(std::min(g_snapshot.projectListCount, kMaxProjectsInSnapshot));
        const int count = files + 1;
        if (g_snapshot.projectListSequence != g_projectsSeen)
        {
            g_projectsSeen = g_snapshot.projectListSequence;
            g_projectsIndex = std::clamp(g_projectsIndex, 0, count - 1);
            // A renamed project moves (the list is newest first): it stays selected.
            for (int n = 0; n < files && !g_projectsSelectFile.empty(); ++n)
                if (replay::project::SameFile(g_snapshot.projects[n].file, g_projectsSelectFile))
                {
                    g_projectsIndex = n + 1;
                    g_projectsSelectFile.clear();
                }
        }
        NavigateList(g_projectsIndex, count);
        constexpr int kRows = 11;
        const int first = std::clamp(g_projectsIndex - kRows / 2, 0, std::max(0, count - kRows));
        for (int row = 0; row < kRows && first + row < count; ++row)
        {
            const int n = first + row;
            const float y = 140.0f + row * 40.0f;
            bool clicked = false, pointed = false; // the mouse selects in a list that fits (see the gallery)
            ImGui::PushID(n);
            const bool doubled = ListItem(draw, "##project", 80.0f, y, 560.0f, 36.0f, n == g_projectsIndex, clicked, count <= kRows ? &pointed : nullptr);
            ImGui::PopID();
            if (clicked || pointed)
                g_projectsIndex = n;
            if (doubled)
                g_projectsIndex = n;
            if (n == 0)
            {
                Text(draw, 88.0f, y + 7.0f, 20.0f, kText, TR("projects.new_row", "+  New Project"));
                if (doubled)
                {
                    PlayUi(UiSound::Select);
                    ShowProjectName(true, nullptr, "", Screen::Projects);
                    return;
                }
                continue;
            }
            const ProjectInfo& info = g_snapshot.projects[n - 1];
            const bool openNow = ProjectIsOpen() && std::strcmp(info.file, g_snapshot.projectFile) == 0;
            std::string label = info.name[0] ? info.name : info.file;
            if (openNow)
                label += TR("gallery.open_now", "   (open)");
            char duration[16]{};
            FormatClock(info.totalMs, duration, sizeof(duration), false);
            const char* lengthText = info.readable ? duration : TR("gallery.old", "old");
            const float lengthX = LengthColumnX(lengthText);
            TextFit(draw, 88.0f, y + 7.0f, 20.0f, info.readable ? kText : kMuted, label.c_str(), std::min(490.0f, lengthX - 98.0f)); // before the length
            Text(draw, lengthX, y + 7.0f, 20.0f, info.readable ? kText : kMuted, lengthText);
            if (doubled)
            {
                PlayUi(info.readable ? UiSound::Select : UiSound::Error);
                if (info.readable)
                {
                    SendNamed(CommandType::ProjectOpen, info.file);
                    g_projectOpenAsked = GetTickCount64();
                    g_projectIndex = 0;
                    g_screen = Screen::Project;
                    return;
                }
            }
        }
        // Details of the selected project, measured first so the panel grows to them
        // (the file name, which has no space, wraps at any character; a long name is
        // cut with "...").
        {
            const float cx = kDetailLeft + kPanelPad;
            const float cw = kDetailRight - kDetailLeft - 2.0f * kPanelPad;
            const float valueW = cx + cw - kDetailValueX;
            const char* const kNewProject =
                TR("projects.new.body", "A project puts clips in order, like the Rockstar Editor's: cut them, move them, trim each one with its "
                "start and end markers, give each its own cameras, and play them all as one video.");
            const char* const kOld = TR("projects.old", "Saved by another version of the editor: it cannot be opened.");
            const ProjectInfo* info = g_projectsIndex > 0 ? &g_snapshot.projects[g_projectsIndex - 1] : nullptr;
            const auto details = [&](ImDrawList* d) {
                float y = 216.0f;
                if (!info)
                    return y + (d ? Paragraph(d, cx, y, 17.0f, kText, kNewProject, cw) : ParagraphHeight(17.0f, kNewProject, cw));
                char duration[16]{};
                FormatClock(info->totalMs, duration, sizeof(duration), true);
                const std::string line = TRF("projects.length.value", "{count} clips, {length}", {{"count", std::to_string(info->clipCount)}, {"length", duration}});
                y = 220.0f;
                if (d)
                {
                    Text(d, cx, y, 15.0f, kMuted, TR("projects.length", "Length"));
                    TextFit(d, kDetailValueX, y, 15.0f, kText, line.c_str(), valueW);
                }
                y = std::max(244.0f, y + LabelHeight(15.0f) + 4.0f);
                if (d)
                    Text(d, cx, y, 15.0f, kMuted, TR("gallery.detail.file", "File"));
                y += std::max(22.0f, (d ? Paragraph(d, kDetailValueX, y, 15.0f, kText, info->file, valueW)
                                        : ParagraphHeight(15.0f, info->file, valueW)) + 4.0f);
                if (!info->readable)
                {
                    y = std::max(290.0f, y + 8.0f);
                    y += d ? Paragraph(d, cx, y, 15.0f, kGold, kOld, cw) : ParagraphHeight(15.0f, kOld, cw);
                }
                return y;
            };
            // A project's actions under its details, as buttons with their keys
            // (the key bar has them too): R renames it, Del deletes it.
            const char* const renameLabel = TR("keys.rename", "Rename");
            const char* const deleteLabel = TR("keys.delete", "Delete");
            const float actionsY = details(nullptr) + 18.0f;
            const float bottom = std::max(420.0f, (info ? actionsY + 20.0f : details(nullptr)) + kPanelPad);
            InkPanel(draw, kDetailLeft, 140.0f, kDetailRight, bottom, kPanel);
            TextFit(draw, cx, 160.0f, 26.0f, kText, info ? (info->name[0] ? info->name : info->file) : TR("home.new_project", "New Project"), cw);
            OrnamentDivider(draw, cx, 200.0f, cw);
            details(draw);
            if (info)
            {
                const ProjectInfo& chosen = *info;
                const float deleteX = cx + KeyActionWidth("R", renameLabel) + 22.0f;
                if (KeyAction(draw, "##projectrename", cx, actionsY, "R", renameLabel))
                {
                    PlayUi(UiSound::Select);
                    ShowProjectName(false, chosen.file, chosen.name, Screen::Projects);
                    return;
                }
                if (KeyAction(draw, "##projectdelete", deleteX, actionsY, "Del", deleteLabel))
                {
                    PlayUi(UiSound::Select);
                    const bool openNow = ProjectIsOpen() && std::strcmp(chosen.file, g_snapshot.projectFile) == 0;
                    ShowDeleteProjectAlert(chosen.file, chosen.name[0] ? chosen.name : chosen.file, openNow);
                    return;
                }
            }
        }
        if (Confirmed())
        {
            if (g_projectsIndex == 0)
            {
                PlayUi(UiSound::Select);
                ShowProjectName(true, nullptr, "", Screen::Projects);
                return;
            }
            const ProjectInfo& info = g_snapshot.projects[g_projectsIndex - 1];
            if (!info.readable)
                PlayUi(UiSound::Error);
            else
            {
                PlayUi(UiSound::Select);
                SendNamed(CommandType::ProjectOpen, info.file);
                g_projectOpenAsked = GetTickCount64();
                g_projectIndex = 0;
                g_screen = Screen::Project;
                return;
            }
        }
        if (g_projectsIndex > 0)
        {
            const ProjectInfo& info = g_snapshot.projects[g_projectsIndex - 1];
            if (KeyPressed(ImGuiKey_R, false))
            {
                PlayUi(UiSound::Select);
                ShowProjectName(false, info.file, info.name, Screen::Projects);
                return;
            }
            if (KeyPressed(ImGuiKey_Delete, false))
            {
                PlayUi(UiSound::Select);
                const bool openNow = ProjectIsOpen() && std::strcmp(info.file, g_snapshot.projectFile) == 0;
                ShowDeleteProjectAlert(info.file, info.name[0] ? info.name : info.file, openNow);
                return;
            }
        }
        if (Back())
        {
            PlayUi(UiSound::Back);
            g_screen = Screen::Home;
        }
    }

namespace
{

    // The blocks of the project's timeline: each clip as wide as its
    // trimmed length, at least a few pixels, fitted to the width.
    struct ProjectBlock
    {
        float x0 = 0.0f;
        float x1 = 0.0f;
        std::uint64_t startMs = 0;
        std::uint64_t durationMs = 0;
    };

    std::vector<ProjectBlock> LayoutProjectBlocks(float left, float width)
    {
        const std::uint32_t count = std::min(g_snapshot.projectClipCount, kMaxProjectClipsInSnapshot);
        std::vector<ProjectBlock> blocks(count);
        if (count == 0)
            return blocks;
        const double total = std::max<double>(1.0, static_cast<double>(g_snapshot.projectTotalMs));
        const float minWidth = std::min(26.0f, width / static_cast<float>(count));
        float sum = 0.0f;
        std::uint64_t start = 0;
        for (std::uint32_t n = 0; n < count; ++n)
        {
            const ProjectClipInfo& clip = g_snapshot.projectClips[n];
            blocks[n].startMs = start;
            blocks[n].durationMs = clip.outMs > clip.inMs ? clip.outMs - clip.inMs : 0;
            start += blocks[n].durationMs;
            blocks[n].x1 = std::max(minWidth, static_cast<float>(blocks[n].durationMs / total) * width);
            sum += blocks[n].x1;
        }
        const float scale = sum > 0.0f ? width / sum : 1.0f;
        float x = left;
        for (ProjectBlock& block : blocks)
        {
            const float w = block.x1 * scale;
            block.x0 = x;
            block.x1 = x + w;
            x += w;
        }
        return blocks;
    }

    float ProjectTimeX(const std::vector<ProjectBlock>& blocks, std::uint64_t timeMs)
    {
        if (blocks.empty())
            return 0.0f;
        for (const ProjectBlock& block : blocks)
            if (timeMs < block.startMs + block.durationMs)
                return block.x0 + (block.x1 - block.x0) * static_cast<float>(timeMs - std::min(timeMs, block.startMs)) /
                    static_cast<float>(std::max<std::uint64_t>(1, block.durationMs));
        return blocks.back().x1;
    }

    std::uint64_t ProjectTimeAtX(const std::vector<ProjectBlock>& blocks, float x)
    {
        for (const ProjectBlock& block : blocks)
            if (x < block.x1)
            {
                const float t = std::clamp((x - block.x0) / std::max(1.0f, block.x1 - block.x0), 0.0f, 1.0f);
                return block.startMs + static_cast<std::uint64_t>(t * static_cast<float>(block.durationMs));
            }
        return blocks.empty() ? 0 : blocks.back().startMs + blocks.back().durationMs;
    }

    int ProjectBlockAtX(const std::vector<ProjectBlock>& blocks, float x)
    {
        for (std::size_t n = 0; n < blocks.size(); ++n)
            if (x >= blocks[n].x0 && x < blocks[n].x1)
                return static_cast<int>(n);
        if (!blocks.empty() && x >= blocks.back().x1)
            return static_cast<int>(blocks.size()) - 1;
        return blocks.empty() ? -1 : 0;
    }

    void SendProjectIndex(CommandType type, int index, std::int32_t direction = 0, std::uint32_t target = 0)
    {
        Command command{};
        command.keyframeId = static_cast<std::uint32_t>(index);
        command.direction = direction;
        command.target = target;
        Send(type, &command);
    }
}

    void DrawProject(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("Project");
        const ULONGLONG nowTick = GetTickCount64();
        if (!ProjectIsOpen())
        {
            // A project asked for (new, open) shows up within a frame or two.
            InkPanel(draw, 30.0f, 30.0f, 620.0f, 110.0f, kPanel);
            const bool waiting = g_projectOpenAsked && nowTick - g_projectOpenAsked < 3000;
            Text(draw, 54.0f, 54.0f, 22.0f, kText, waiting ? TR("project.opening", "Opening the project...") : TR("project.none", "No project is open."));
            if (waiting)
                LoadingAfter(draw, 54.0f, 54.0f, 22.0f, TR("project.opening", "Opening the project..."), 600.0f);
            if (!waiting && Back())
            {
                PlayUi(UiSound::Back);
                g_screen = Screen::Home;
            }
            if (!waiting)
                g_screen = Screen::Projects;
            return;
        }
        g_projectOpenAsked = 0;
        DrawClipFade(draw);
        const int count = static_cast<int>(std::min(g_snapshot.projectClipCount, kMaxProjectClipsInSnapshot));
        g_projectIndex = count ? std::clamp(g_projectIndex, 0, count - 1) : 0;
        const bool playing = Flag(SnapshotPlaying);
        const bool previewing = ProjectFlag(ProjectPreviewing);
        const bool loading = ProjectFlag(ProjectLoadingClip);
        const int active = g_snapshot.projectActiveClip < static_cast<std::uint32_t>(count) ? static_cast<int>(g_snapshot.projectActiveClip) : -1;

        // The message of a refused edit (the core's words), for 4 s.
        if (g_snapshot.projectMessageSequence != g_projectMessageSeen)
        {
            if (g_projectMessageSeen != 0 || g_snapshot.projectMessageSequence > 0)
            {
                g_projectMessageShown = g_snapshot.projectMessage;
                g_projectMessageUntil = nowTick + 4000;
            }
            g_projectMessageSeen = g_snapshot.projectMessageSequence;
        }

        // Header: the project, its length, what plays.
        const float panelW = 640.0f;
        // The panel kPanelPad left of its lines (46), its brush and title over the
        // ink edge.
        InkPanel(draw, 46.0f - kPanelPad, 30.0f, 30.0f + panelW, 156.0f, kPanel);
        const std::string title = TRF("project.title", "PROJECT  {name}{dirty}", {{"name", g_snapshot.projectName}, {"dirty", ProjectFlag(ProjectDirty) ? " *" : ""}});
        // R renames the project: a button with its key at the header's right end.
        const char* const renameLabel = TR("keys.rename", "Rename");
        const float renameW = KeyActionWidth("R", renameLabel);
        const float renameX = 30.0f + panelW - 16.0f - renameW;
        bool renameAsked = KeyAction(draw, "##projectrename", renameX, 44.0f, "R", renameLabel);
        // A long name is cut with "..." inside the brush, clear of the button.
        const std::string shownTitle = Ellipsized(26.0f, title, std::min(panelW - 66.0f, renameX - 70.0f));
        Brush(draw, 30.0f, 36.0f, std::min(panelW - 20.0f, TextWidth(26.0f, shownTitle.c_str()) / g_scale + 40.0f), 40.0f, kRed);
        Text(draw, 44.0f, 42.0f, 26.0f, kText, shownTitle.c_str());
        char total[16]{}, now[16]{};
        FormatClock(g_snapshot.projectTotalMs, total, sizeof(total), true);
        FormatClock(g_snapshot.projectPlayheadMs, now, sizeof(now), true);
        std::string line = count == 1 ? TRF("project.length.one", "{now} / {total}   -   1 clip", {{"now", now}, {"total", total}})
                                       : TRF("project.length", "{now} / {total}   -   {count} clips", {{"now", now}, {"total", total}, {"count", std::to_string(count)}});
        TextFit(draw, 46.0f, 88.0f, 17.0f, kText, line.c_str(), panelW - 32.0f);
        if (active >= 0)
        {
            const ProjectClipInfo& clip = g_snapshot.projectClips[active];
            const char* doing = loading ? TR("project.active.opening", "Opening")
                : previewing ? (Flag(SnapshotPaused) ? TR("project.active.paused", "Paused at") : TR("project.active.playing", "Playing"))
                             : TR("project.active.editing", "Editing");
            line = TRF("project.active", "{doing} clip {at} of {count}: {clip}",
                {{"doing", doing}, {"at", std::to_string(active + 1)}, {"count", std::to_string(count)}, {"clip", ClipLabel(clip.file)}});
        }
        else
            line = KeyFill(count ? TR("project.hint.play", "{key_play} plays the project from the selected clip.")
                                 : TR("project.hint.empty", "Empty: {key_add_clip} adds a clip from your recorded clips."));
        TextFit(draw, 46.0f, 114.0f, 15.0f, kMuted, line.c_str(), panelW - 32.0f);
        UI_PROBE_BOX("endpanel", P(0.0f, 0.0f), P(0.0f, 0.0f)); // the message under the panel is not in it
        if (nowTick < g_projectMessageUntil)
            Paragraph(draw, 46.0f, 166.0f, 16.0f, kGold, g_projectMessageShown.c_str(), panelW - 30.0f);
        else if (ProjectFlag(ProjectSaveFailed))
            Paragraph(draw, 46.0f, 166.0f, 16.0f, kGold, TR("project.save_failed", "The project could not be saved."), panelW - 30.0f);

        // The timeline: the blocks over the project's time bar.
        const float left = 40.0f;
        const float right = g_vw - 40.0f;
        const float width = right - left;
        // Over the key bar.
        const float blocksTop = g_vh - g_footH - 136.0f;
        const float blocksHeight = 64.0f;
        const float barY = blocksTop + blocksHeight + 14.0f;
        draw->AddRectFilledMultiColor(P(0.0f, blocksTop - 50.0f), P(g_vw, g_vh), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
            IM_COL32(8, 7, 6, 215), IM_COL32(8, 7, 6, 215));
        const std::vector<ProjectBlock> blocks = LayoutProjectBlocks(left, width);
        const ImVec2 mouse = ImGui::GetMousePos();
        const float mouseX = mouse.x / g_scale - g_ox;

        // Blocks: interaction first (the drawing shows the drag).
        ImGui::SetCursorScreenPos(P(left, blocksTop));
        ImGui::InvisibleButton("##projectblocks", ImVec2(S(width), S(blocksHeight)));
        const bool blocksHovered = ImGui::IsItemHovered();
        const int hoveredBlock = blocksHovered ? ProjectBlockAtX(blocks, mouseX) : -1;
        if (blocksHovered && MouseClicked() && hoveredBlock >= 0)
        {
            if (hoveredBlock != g_projectIndex)
                PlayUi(UiSound::NavDown);
            g_projectIndex = hoveredBlock;
            g_dragBlock = hoveredBlock;
            g_dragBlockFromX = mouseX;
            g_dragBlockMoved = false;
        }
        if (blocksHovered && hoveredBlock >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            PlayUi(UiSound::Select);
            SendProjectIndex(CommandType::ProjectEditClip, hoveredBlock);
            g_editAfterOpen = hoveredBlock;
            g_editAfterOpenAt = nowTick;
            g_dragBlock = -1;
        }
        if (g_dragBlock >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left) && std::fabs(mouseX - g_dragBlockFromX) > 8.0f)
            g_dragBlockMoved = true;
        int dropAt = -1;
        if (g_dragBlock >= 0 && g_dragBlockMoved)
            dropAt = ProjectBlockAtX(blocks, mouseX);
        if (g_dragBlock >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (g_dragBlockMoved && dropAt >= 0 && dropAt != g_dragBlock)
            {
                PlayUi(UiSound::Select);
                SendProjectIndex(CommandType::ProjectMoveClip, g_dragBlock, 0, static_cast<std::uint32_t>(dropAt));
                g_projectIndex = dropAt;
            }
            g_dragBlock = -1;
            g_dragBlockMoved = false;
        }

        for (int n = 0; n < count; ++n)
        {
            const ProjectBlock& block = blocks[n];
            const ProjectClipInfo& clip = g_snapshot.projectClips[n];
            const bool selected = n == g_projectIndex;
            const bool missing = (clip.flags & ProjectClipMissing) != 0;
            const bool isActive = (clip.flags & ProjectClipActive) != 0;
            const float x0 = block.x0 + 1.0f;
            const float x1 = std::max(x0 + 2.0f, block.x1 - 1.0f);
            const ImU32 fill = missing ? IM_COL32(60, 20, 18, 220) : isActive ? IM_COL32(58, 50, 38, 240)
                : n == hoveredBlock ? IM_COL32(40, 36, 32, 235) : IM_COL32(24, 22, 20, 230);
            draw->AddRectFilled(P(x0, blocksTop), P(x1, blocksTop + blocksHeight), fill, S(3.0f));
            if (isActive)
                draw->AddRectFilled(P(x0, blocksTop), P(x1, blocksTop + 4.0f), kGold);
            if (selected)
                draw->AddRect(P(x0, blocksTop), P(x1, blocksTop + blocksHeight), kRed, S(3.0f), 0, S(3.0f));
            else
                draw->AddRect(P(x0, blocksTop), P(x1, blocksTop + blocksHeight), kPanelLine, S(3.0f), 0, S(1.0f));
            if (x1 - x0 > 44.0f)
            {
                draw->PushClipRect(P(x0 + 4.0f, blocksTop), P(x1 - 4.0f, blocksTop + blocksHeight), true);
                char number[8]{};
                std::snprintf(number, sizeof(number), "%d", n + 1);
                Text(draw, x0 + 7.0f, blocksTop + 8.0f, 14.0f, kMuted, number);
                Text(draw, x0 + 7.0f + TextWidth(14.0f, number) / g_scale + 8.0f, blocksTop + 8.0f, 15.0f, missing ? kMuted : kText,
                    ClipLabel(clip.file).c_str());
                char length[24]{};
                FormatClock(block.durationMs, length, sizeof(length), true);
                Text(draw, x0 + 7.0f, blocksTop + 36.0f, 14.0f, missing ? kGold : kMuted, missing ? TR("project.clip.missing", "missing") : length);
                draw->PopClipRect();
            }
            // The trim inside the clip file: a thin bar under the block.
            if (clip.rawMs > 0 && x1 - x0 > 12.0f)
            {
                const float a = x0 + (x1 - x0) * static_cast<float>(clip.inMs) / static_cast<float>(clip.rawMs);
                const float b = x0 + (x1 - x0) * static_cast<float>(clip.outMs) / static_cast<float>(clip.rawMs);
                draw->AddRectFilled(P(x0, blocksTop + blocksHeight - 5.0f), P(x1, blocksTop + blocksHeight - 3.0f), IM_COL32(236, 230, 218, 40));
                draw->AddRectFilled(P(a, blocksTop + blocksHeight - 5.0f), P(b, blocksTop + blocksHeight - 3.0f), IM_COL32(236, 230, 218, 150));
            }
        }
        if (g_dragBlock >= 0 && g_dragBlockMoved && dropAt >= 0)
        {
            // Where the held block goes: a gold line at that place.
            const float x = dropAt > g_dragBlock ? blocks[dropAt].x1 : blocks[dropAt].x0;
            draw->AddLine(P(x, blocksTop - 6.0f), P(x, blocksTop + blocksHeight + 6.0f), kGold, S(3.0f));
        }

        // The project's time bar (click or drag to move the playhead; a drag
        // over another clip's file loads it once let go).
        const std::uint64_t playhead = g_projectScrubbing && g_projectLastSeek != UINT64_MAX ? g_projectLastSeek : g_snapshot.projectPlayheadMs;
        const float playX = count ? std::clamp(ProjectTimeX(blocks, playhead), left, right) : left;
        if (TimelineArtReady()) // the torn bar (the blocks keep their look)
        {
            TornStrip(draw, ui_art::Art::BarTorn, left, left, right, barY - 3.0f, barY + 8.0f, kTrackTint);
            TornStrip(draw, ui_art::Art::BarRed, left, left, playX, barY - 3.0f, barY + 8.0f, IM_COL32(255, 255, 255, 255));
        }
        else
        {
            draw->AddRectFilled(P(left, barY), P(right, barY + 5.0f), IM_COL32(236, 230, 218, 70));
            draw->AddRectFilled(P(left, barY), P(playX, barY + 5.0f), kRed);
        }
        for (int n = 1; n < count; ++n)
            if (!SeamMark(draw, blocks[n].x0, barY - 6.0f, barY + 11.0f, width / static_cast<float>(count)))
                draw->AddLine(P(blocks[n].x0, barY - 3.0f), P(blocks[n].x0, barY + 8.0f), IM_COL32(236, 230, 218, 120), S(1.0f));
        if (count)
        {
            draw->AddLine(P(playX, blocksTop - 4.0f), P(playX, barY + 9.0f), kText, S(2.0f));
            draw->AddTriangleFilled(P(playX - 5.0f, barY + 15.0f), P(playX + 5.0f, barY + 15.0f), P(playX, barY + 8.0f), kText);
        }
        ImGui::SetCursorScreenPos(P(left, barY - 8.0f));
        ImGui::InvisibleButton("##projectbar", ImVec2(S(width), S(24.0f)));
        const bool barHovered = ImGui::IsItemHovered();
        if (count && barHovered && MouseClicked())
        {
            g_projectScrubbing = ImGui::IsMouseDown(ImGuiMouseButton_Left);
            g_projectLastSeek = UINT64_MAX;
            if (!g_projectScrubbing)
            {
                Command seek{};
                seek.timeMs = ProjectTimeAtX(blocks, mouseX);
                Send(CommandType::ProjectSeek, &seek);
            }
        }
        if (g_projectScrubbing)
        {
            const std::uint64_t at = ProjectTimeAtX(blocks, mouseX);
            const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
            if (down && at != g_projectLastSeek)
            {
                Command seek{};
                seek.timeMs = at;
                seek.enabled = kSeekDragging;
                Send(CommandType::ProjectSeek, &seek);
                g_projectLastSeek = at;
                const int under = ProjectBlockAtX(blocks, mouseX);
                if (under >= 0)
                    g_projectIndex = under;
            }
            if (!down)
            {
                Command seek{};
                seek.timeMs = g_projectLastSeek != UINT64_MAX ? g_projectLastSeek : at;
                seek.enabled = kSeekDragEnd;
                Send(CommandType::ProjectSeek, &seek);
                g_projectScrubbing = false;
            }
        }

        // Keys.
        const bool ctrl = ImGui::GetIO().KeyCtrl;
        if (count && !ctrl && KeyPressed(ImGuiKey_LeftArrow))
        {
            g_projectIndex = std::max(0, g_projectIndex - 1);
            PlayUi(UiSound::NavLeft);
        }
        if (count && !ctrl && KeyPressed(ImGuiKey_RightArrow))
        {
            g_projectIndex = std::min(count - 1, g_projectIndex + 1);
            PlayUi(UiSound::NavRight);
        }
        // Ctrl+Left / Ctrl+Right: the selected clip one place back or forward.
        if (count && ctrl && (KeyPressed(ImGuiKey_LeftArrow, false) || KeyPressed(ImGuiKey_RightArrow, false)))
        {
            const int step = KeyPressed(ImGuiKey_LeftArrow, false) ? -1 : 1;
            const int to = g_projectIndex + step;
            if (to < 0 || to >= count)
                PlayUi(UiSound::Error);
            else
            {
                SendProjectIndex(CommandType::ProjectMoveClip, g_projectIndex, 0, static_cast<std::uint32_t>(to));
                g_projectIndex = to;
                PlayUi(step < 0 ? UiSound::NavLeft : UiSound::NavRight);
            }
        }
        if (KeyPressed(ImGuiKey_Space, false) || ActionPressed(Action::PlayPause))
        {
            Command command{};
            if (previewing && playing && !Flag(SnapshotPaused))
                command.enabled = 0;
            else if (previewing && playing)
            {
                command.enabled = 1;
                command.timeMs = ~0ull;
            }
            else
            {
                command.enabled = 1;
                command.timeMs = count ? blocks[g_projectIndex].startMs : 0;
            }
            if (count)
                Send(CommandType::ProjectPlay, &command);
            else
                PlayUi(UiSound::Error);
        }
        if (count && !ctrl && Confirmed())
        {
            PlayUi(UiSound::Select);
            SendProjectIndex(CommandType::ProjectEditClip, g_projectIndex);
            g_editAfterOpen = g_projectIndex;
            g_editAfterOpenAt = nowTick;
        }
        if (!ctrl && KeyPressed(ImGuiKey_A, false))
        {
            PlayUi(UiSound::Select);
            g_galleryPicking = true;
            g_pickInsertAt = count ? static_cast<std::uint32_t>(g_projectIndex + 1) : ~0u;
            RefreshClipList();
            g_screen = Screen::Gallery;
            return;
        }
        // The edits on the clip open at the playhead: X cuts it there, I and O
        // trim its start or end to the playhead.
        const bool selectedIsActive = active >= 0 && active == g_projectIndex && playing && !loading;
        if (count && !ctrl && (KeyPressed(ImGuiKey_X, false) || KeyPressed(ImGuiKey_I, false) || KeyPressed(ImGuiKey_O, false)))
        {
            if (!selectedIsActive)
            {
                PlayUi(UiSound::Error);
                g_projectMessageShown = TR("project.message.play_first", "Play or open the selected clip first; the cut and the trim go where its playhead is.");
                g_projectMessageUntil = nowTick + 4000;
            }
            else if (KeyPressed(ImGuiKey_X, false))
            {
                PlayUi(UiSound::Select);
                SendProjectIndex(CommandType::ProjectSplitClip, g_projectIndex);
            }
            else
            {
                PlayUi(UiSound::Select);
                SendProjectIndex(CommandType::ProjectTrimClip, g_projectIndex, KeyPressed(ImGuiKey_I, false) ? -1 : 1);
            }
        }
        if (count && !ctrl && KeyPressed(ImGuiKey_D, false))
        {
            PlayUi(UiSound::Select);
            SendProjectIndex(CommandType::ProjectDuplicateClip, g_projectIndex);
        }
        if (count && KeyPressed(ImGuiKey_Delete, false))
        {
            PlayUi(UiSound::Select);
            ShowAlert(TR("project.remove.title", "Remove clip"), TRF("project.remove.body",
                "Remove clip {at} ({clip}) from the project? The clip file stays in ReplayClips; its trim and markers in this project go.",
                {{"at", std::to_string(g_projectIndex + 1)}, {"clip", ClipLabel(g_snapshot.projectClips[g_projectIndex].file)}}),
                TR("project.remove.confirm", "Remove"), CommandType::ProjectDeleteClip, std::to_string(g_projectIndex), Screen::Project);
            return;
        }
        if (renameAsked || (!ctrl && KeyPressed(ImGuiKey_R, false)))
        {
            PlayUi(UiSound::Select);
            ShowProjectName(false, g_snapshot.projectFile, g_snapshot.projectName, Screen::Project);
            return;
        }
        if (ctrl && KeyPressed(ImGuiKey_S, false))
        {
            PlayUi(UiSound::Select);
            Send(CommandType::ProjectSave);
            g_projectMessageShown = TR("project.message.saved", "Saved.");
            g_projectMessageUntil = nowTick + 2000;
        }
        // Ctrl+E: the export dialog on the whole project (each clip with its
        // trim, markers and settings, one video).
        if (ctrl && count > 0 && KeyPressed(ImGuiKey_E, false))
        {
            PlayUi(UiSound::Select);
            exportui::g_range = 2;
            exportui::g_back = Screen::Project;
            g_screen = Screen::Export;
            return;
        }
        const bool stepBack = ActionPressed(Action::FrameBack, true);
        if (playing && (stepBack || ActionPressed(Action::FrameForward, true)))
        {
            Command command{};
            command.direction = stepBack ? -1 : 1;
            Send(CommandType::StepFrame, &command);
        }
        if (ActionPressed(Action::Stop) && playing)
        {
            PlayUi(UiSound::Back);
            Send(CommandType::Stop);
        }
        if (Back())
        {
            PlayUi(UiSound::Back);
            g_screen = Screen::Home;
            g_homeIndex = kHomeEditRow;
        }
    }
