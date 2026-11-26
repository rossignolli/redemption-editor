// script_editor.cpp: the editor: clip loads, projects and their export, the editor commands and the snapshot.
// Split from script.cpp (moved code); shared internals in script_internal.h.

#include "../script_internal.h"

namespace
{
    int AuthoredLightBoneIndex(Entity entity, std::uint32_t ref);
    Vector3 RotateAuthoredLightVector(const Vector3& rotation, std::uint32_t mount, float x, float y, float z);
    Vector3 UnrotateAuthoredLightVector(const Vector3& rotation, std::uint32_t mount, float x, float y, float z);
    bool AuthoredLightWorldPosition(const replay::editor::AuthoredLight& light, Entity parent, replay::editor::Vec3& world);
    bool AuthoredLightLocalPosition(const replay::editor::AuthoredLight& light, Entity parent,
        const replay::editor::Vec3& world, replay::editor::Vec3& local);
    bool CachedAuthoredLightWorldPosition(std::uint32_t id, std::uint32_t target, std::uint32_t markerId,
        replay::editor::Vec3& world);

    float WrapLightDegrees(float value)
    {
        // Keep the slider's explicit 360-degree endpoint so it doesn't snap
        // back to zero while editing. Values outside the range still wrap.
        if (std::isfinite(value) && value >= 0.0f && value <= 360.0f)
            return value;
        value = std::fmod(value, 360.0f);
        return value < 0.0f ? value + 360.0f : value;
    }

    std::uint32_t NextAuthoredLightId(const std::vector<replay::editor::AuthoredLight>& lights)
    {
        std::uint32_t next = 1;
        for (const replay::editor::AuthoredLight& light : lights)
            if (light.id >= next && light.id != 0xFFFFFFFFu)
                next = light.id + 1;
        return next ? next : 1;
    }

    std::uint64_t g_loggedEditorBridgeEpoch = 0;
    bool g_editorQueueOverflowed = false;
    bool g_refreshTargetCatalog = false;
    // The editor's window over the sorted list (the snapshot holds 64 clips; the
    // folder holds hundreds) and its sort.
    std::uint32_t g_clipWindowFirst = 0;
    ULONGLONG g_loadFailedUntil = 0; // SnapshotLoadFailed
    ULONGLONG g_renameFailedUntil = 0; // SnapshotRenameFailed

    // The editor's menu sounds (editor_protocol.h UiSound), played as the game's
    // shop menus play them. A sound asked while the script thread stood still (the
    // pause menu) is dropped, not played late.
    void PlayEditorUiSound(const replay::editor::Command& command)
    {
        constexpr ULONGLONG kLateMs = 300;
        const char* name = replay::editor::UiSoundName(static_cast<replay::editor::UiSound>(command.enabled));
        const ULONGLONG now = GetTickCount64();
        if (!name || command.timeMs > now || now - command.timeMs > kLateMs)
            return;
        AUDIO::PLAY_SOUND_FRONTEND(name, replay::editor::kUiSoundSet, TRUE, 0);
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            LogFile() << "EDITOR_UI_SOUND,first=" << name << ",set=" << replay::editor::kUiSoundSet << '\n';
        }
    }

    // A speed change pushes the following markers out and could push the clip's
    // end marker past the clip: the end is held at the clip's duration.
    void HoldEndMarkerInClip()
    {
        const std::vector<replay::camera::Keyframe>& keys = g_cameraTrack.Keyframes();
        if (keys.size() < 2 || g_cameraTrack.LegacyBlendAnyMode() || !g_transport.Playing())
            return;
        const std::uint64_t duration = PlaybackDurationMs();
        if (keys.back().timeMs <= duration || keys[keys.size() - 2].timeMs >= duration)
            return;
        if (replay::camera::Keyframe* end = g_cameraTrack.FindMutable(keys.back().id))
        {
            LogFile() << "CAMERA_MARKER,action=end_held,from_ms=" << end->timeMs << ",to_ms=" << duration << '\n';
            end->timeMs = duration;
        }
    }

    // The limit a marker command's spacing cascade must fit in (the clip, as
    // HoldEndMarkerInClip holds it).
    std::uint64_t MarkerTimeLimit()
    {
        return g_transport.Playing() && !g_cameraTrack.LegacyBlendAnyMode() ? PlaybackDurationMs()
                                                                          : replay::camera::kNoTimeLimit;
    }

    // A clip of the folder into playback (the gallery's Enter).
    void LoadClipNow(const std::wstring& path, ULONGLONG now, ClipRead* preloaded = nullptr)
    {
        std::string error;
        if (ReadClipFile(path, error, preloaded))
            StartPlayback(now);
        else
        {
            ShowObjective(("Redemption Editor: could not load " + Narrow(path) + ": " + error).c_str());
            g_loadFailedUntil = GetTickCount64() + 5000; // the editor's alert
        }
    }

    // A load waiting for the save of the clip left.
    void ServicePendingLoad(ULONGLONG now)
    {
        if (g_pendingLoadPath.empty())
            return;
        FinishPendingSave(false);
        if (SavePending())
            return;
        // The file is read on a thread; this tick only starts it, a later one
        // publishes it. A file with a save not written yet opens from memory at once
        // (ReadClipFile).
        if (g_clipRead && g_clipRead->path != g_pendingLoadPath)
            CancelClipRead(); // another clip was asked for meanwhile
        if (!g_clipRead)
        {
            if (!g_clipSaves.Holds(g_pendingLoadPath))
            {
                StartClipRead(g_pendingLoadPath);
                return;
            }
        }
        else if (!g_clipRead->done.load())
            return;
        const std::wstring path = std::move(g_pendingLoadPath);
        g_pendingLoadPath.clear();
        std::unique_ptr<ClipRead> read = std::move(g_clipRead);
        if (read)
            read->worker.join();
        LoadClipNow(path, now, read.get());
    }

    // A seek asked by the editor (the timeline, a marker, a project clip): paused
    // there, with the drag semantics of editor_protocol.h (kSeekPlain,
    // kSeekDragging, kSeekDragEnd).
    void SeekPlayheadTo(std::uint64_t target, std::uint32_t seekKind, ULONGLONG now)
    {
        if (!g_transport.Paused())
            SetPlaybackPaused(true, now);
        // A timeline drag moves cheaply and loads once at its end.
        const replay::settle::SeekKind kind = seekKind == replay::editor::kSeekDragging ? replay::settle::SeekKind::Dragging
            : seekKind == replay::editor::kSeekDragEnd                                   ? replay::settle::SeekKind::DragEnd
                                                                                         : replay::settle::SeekKind::Plain;
        g_seekDrag.OnSeek(now, kind);
        g_seekReason = kind == replay::settle::SeekKind::Dragging ? nullptr
            : kind == replay::settle::SeekKind::DragEnd             ? "drag_end"
                                                                     : "seek";
        g_seekFine = false;
        g_transport.Seek(std::min<std::uint64_t>(target, PlaybackDurationMs()));
    }

    // ---- Projects ----
    // A montage: an ordered list of clips, each a clip file of ReplayClips with its
    // own trim and edit markers (src/replay_project.h). The script thread owns the
    // open project. A project clip plays as any clip (its file loaded through the
    // gallery's path, read on a thread), with the project's markers in place of
    // the file's; marker edits go to the project, never to the clip file. Playing
    // the project as a whole (Preview) goes on to the next clip at each clip's end
    // marker: the same file (two pieces of a cut) only moves the playhead and
    // swaps the markers; another file was read ahead while the clip before it
    // played, and is published there (the puppets are built again).
    const wchar_t* kProjectFolder = L"ReplayProjects";
    constexpr std::uint64_t kProjectPreloadLeadMs = 8000; // read the next file this long before the cut
    enum class ProjectMode
    {
        None,    // no project clip in playback
        Edit,    // one clip open in the clip editor: stops at its end marker
        Preview  // the whole project: on to the next clip at each end marker
    };
    replay::project::Project g_project;
}
    bool g_projectOpen = false;
    std::wstring g_projectPath;
    bool g_projectDirty = false;
namespace
{
    ProjectMode g_projectMode = ProjectMode::None;
    int g_projectActive = -1;             // the project clip in playback
    std::uint32_t g_projectAppliedLoad = 0; // g_clipLoadSequence when its markers went in
    std::vector<bool> g_projectMissing;   // per clip: its file is not in ReplayClips
}
    ProjectPendingOpen g_projectPending;
namespace
{
    // Once the opened clip's puppets stand: the playhead there, played or paused.
    struct ProjectSeekAfterReady
    {
        bool active = false;
        std::uint64_t clipMs = 0;
        bool play = false;
    };
    ProjectSeekAfterReady g_projectSeekAfter;
    std::uint64_t g_projectDragMs = UINT64_MAX; // a drag over a clip of another file (no load until it ends)
    int g_projectAdvanceTo = -1; // the next clip to open (asked at a clip's end, done after the editor's commands)
    std::uint64_t g_projectSequence = 1;
    std::uint64_t g_projectListSequence = 0;
    std::vector<replay::editor::ProjectInfo> g_projectList;
    std::string g_projectMessage;
    std::uint32_t g_projectMessageSequence = 0;
    ULONGLONG g_projectSaveFailedUntil = 0;

    std::wstring Widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

    // A project file where it is (the base folder first, then the game
    // folder), else the base path a new one takes.
    std::wstring ProjectFilePath(const std::string& file) { return replay::storage::Find(std::wstring(kProjectFolder) + L"\\" + Widen(file)); }

    std::wstring ProjectClipPath(const replay::project::Clip& clip) { return FindClipFile(Widen(clip.file)); }

    void ProjectSay(const std::string& text)
    {
        g_projectMessage = text;
        ++g_projectMessageSequence;
        LogFile() << "PROJECT_MESSAGE,text=" << text << '\n';
    }

    void RefreshProjectMissing()
    {
        g_projectMissing.assign(g_project.clips.size(), false);
        for (std::size_t n = 0; n < g_project.clips.size(); ++n)
            g_projectMissing[n] = GetFileAttributesW(ProjectClipPath(g_project.clips[n]).c_str()) == INVALID_FILE_ATTRIBUTES;
    }

    // `files`: the clip list changed (which files exist is checked again);
    // a marker edit only touches one clip's markers.
    void ProjectChanged(bool files = true)
    {
        g_projectDirty = true;
        ++g_projectSequence;
        if (files)
            RefreshProjectMissing();
    }
}

    // The projects of every read root (storage_paths.h), a name in two of them once.
    void RebuildProjectList()
    {
        replay::storage::Folder(kProjectFolder);
        std::vector<std::pair<std::uint64_t, replay::editor::ProjectInfo>> found;
        std::vector<std::wstring> folders;
        for (const replay::storage::Entry& entry : replay::storage::List(kProjectFolder, L"*.rpproj", &folders))
        {
            replay::editor::ProjectInfo info{};
            const std::string file = Narrow(entry.name);
            std::snprintf(info.file, sizeof(info.file), "%s", file.c_str());
            info.modified = entry.stamp;
            replay::project::Summary summary;
            if (replay::project::ReadSummary(folders[entry.root] + L"\\" + entry.name, summary))
            {
                std::snprintf(info.name, sizeof(info.name), "%s", summary.name.c_str());
                info.totalMs = summary.totalMs;
                info.clipCount = summary.clipCount;
                info.readable = summary.readable ? 1u : 0u;
            }
            if (file.size() < sizeof(info.file))
                found.emplace_back(info.modified, info);
        }
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        g_projectList.clear();
        for (std::size_t n = 0; n < found.size() && n < replay::editor::kMaxProjectsInSnapshot; ++n)
            g_projectList.push_back(found[n].second);
        ++g_projectListSequence;
    }

    bool SaveProject(const char* why)
    {
        if (!g_projectOpen)
            return false;
        replay::storage::Folder(kProjectFolder);
        // A project opened from a game folder that takes no writes is saved to
        // the base folder (it is listed from there from now on).
        if (!replay::storage::Current().gameWritable)
            g_projectPath = replay::storage::WritablePath(g_projectPath);
        std::string error;
        const ULONGLONG startedAt = GetTickCount64();
        const bool ok = replay::project::SaveFile(g_projectPath, g_project, error);
        LogFile() << "PROJECT_SAVE,why=" << why << ",path=" << Narrow(g_projectPath) << ",clips=" << g_project.clips.size()
                  << ",total_ms=" << replay::project::TotalMs(g_project) << ",ok=" << (ok ? 1 : 0)
                  << ",ms=" << (GetTickCount64() - startedAt) << ",error=" << (ok ? "none" : error.c_str()) << '\n';
        if (ok)
        {
            g_projectDirty = false;
            ++g_projectSequence;
            RebuildProjectList();
        }
        else
        {
            g_projectSaveFailedUntil = GetTickCount64() + 5000;
            ProjectSay(TRF("project.msg.save_failed", "The project could not be saved: {error}.", {{"error", error}}));
        }
        return ok;
    }

    bool ProjectOwnsPlayback()
    {
        return g_projectOpen && g_projectActive >= 0 && g_projectActive < static_cast<int>(g_project.clips.size()) &&
            g_transport.Playing() && g_projectAppliedLoad == g_clipLoadSequence;
    }

    // Declared before ApplyClipEnvironment.
    std::uint64_t EnvironmentOriginMs()
    {
        if (!ProjectOwnsPlayback())
            return 0;
        return g_project.clips[static_cast<std::size_t>(g_projectActive)].inMs;
    }
namespace
{

    // The clip's environment settings changed in the editor: a project clip keeps
    // them in the project, a gallery clip in its file (saved with the markers:
    // Ctrl+S, the autosave on leaving).
    void EnvironmentEdited(const char* field)
    {
        const replay::EnvironmentOverride& edit = g_clip.environment.edit;
        if (ProjectOwnsPlayback())
        {
            replay::project::Clip& clip = g_project.clips[static_cast<std::size_t>(g_projectActive)];
            clip.environment = edit;
            clip.hasEnvironment = true;
            ProjectChanged(false);
        }
        else
            g_clipEdited = true;
        LogFile() << "ENV_EDIT,field=" << field << ",clock_min=" << edit.clockMinutes << ",weather=" << WeatherName(edit.weather)
                  << ",blend_s=" << edit.blendS << ",clouds_live=" << static_cast<int>(edit.cloudsLive)
                  << ",project=" << (ProjectOwnsPlayback() ? 1 : 0) << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
    }
}

    // The markers of the clip in playback back into the project (an edit of
    // them, a move of its start or end marker: the trim follows).
    void SyncProjectMarkers()
    {
        if (!ProjectOwnsPlayback())
            return;
        replay::project::Clip& clip = g_project.clips[static_cast<std::size_t>(g_projectActive)];
        if (clip.hasMarkers && replay::camera::SameMarkers(clip.markers, g_cameraTrack))
            return;
        if (replay::project::TakeMarkers(clip, g_cameraTrack))
        {
            ProjectChanged(false);
            LogFile() << "PROJECT_MARKERS,clip=" << g_projectActive << ",markers=" << g_cameraTrack.Keyframes().size()
                      << ",in_ms=" << clip.inMs << ",out_ms=" << clip.outMs << '\n';
        }
        g_clipEdited = false; // never the clip file's
    }

    void CancelProjectPreload()
    {
        if (!g_projectPreload)
            return;
        LogFile() << "PROJECT_PRELOAD,action=drop,path=" << Narrow(g_projectPreload->path)
                  << ",done=" << (g_projectPreload->done.load() ? 1 : 0) << '\n';
        g_projectPreloadDropped.push_back(std::move(g_projectPreload));
    }

    // The dropped read aheads whose thread has ended (each tick); `wait`
    // (unload) waits for all of them.
    void ReapProjectPreloads(bool wait)
    {
        for (auto it = g_projectPreloadDropped.begin(); it != g_projectPreloadDropped.end();)
        {
            if (wait || (*it)->done.load())
            {
                (*it)->worker.join();
                it = g_projectPreloadDropped.erase(it);
            }
            else
                ++it;
        }
    }
namespace
{

    // The project lets go of the playback (another clip opened from the
    // gallery, the clip stopped, a recording): the clip that plays, if any,
    // is an ordinary clip from now on.
    void ProjectLeavePlayback(const char* why)
    {
        if (g_projectActive < 0 && !g_projectPending.active && g_projectMode == ProjectMode::None)
            return;
        LogFile() << "PROJECT_PLAYBACK,action=leave,why=" << why << ",clip=" << g_projectActive << '\n';
        g_projectActive = -1;
        g_projectMode = ProjectMode::None;
        g_projectPending = {};
        g_projectSeekAfter = {};
        g_projectDragMs = UINT64_MAX;
        g_projectAdvanceTo = -1;
        CancelProjectPreload();
        ++g_projectSequence;
    }
}

    void ProjectBeforePlaybackStop()
    {
        SyncProjectMarkers();
    }
namespace
{

    // The project clip's markers into the playback (its file is the clip in
    // memory). A clip never opened takes the file's own markers fitted to
    // its trim, with Game Camera bounds where it has none.
    void ApplyProjectMarkers(int index)
    {
        replay::project::Clip& clip = g_project.clips[static_cast<std::size_t>(index)];
        if (!clip.hasMarkers)
        {
            const replay::camera::Track fitted = replay::project::FitMarkers(g_cameraTrack, clip.inMs, clip.outMs,
                GameBoundMarker(clip.inMs), GameBoundMarker(clip.outMs));
            const std::size_t fileMarkers = g_cameraTrack.Keyframes().size();
            if (replay::project::TakeMarkers(clip, fitted))
                ProjectChanged(false);
            LogFile() << "PROJECT_MARKERS,clip=" << index << ",action=from_file,file_markers=" << fileMarkers
                      << ",markers=" << clip.markers.Keyframes().size() << '\n';
        }
        // The environment settings as the markers: a clip never opened takes the clip
        // file's own (ENVO), then the project's apply.
        if (!clip.hasEnvironment)
        {
            clip.environment = g_clip.environment.edit;
            clip.hasEnvironment = true;
            if (clip.environment.Any())
                ProjectChanged(false);
        }
        g_clip.environment.edit = clip.environment;
        if (g_editingMarker)
            LeaveMarkerCameraEdit(false);
        g_cameraTrack = clip.markers;
        g_cameraTrackEnabled = true;
        g_cameraTrackManualOverride = false;
        g_cameraSpring.Reset();
        g_markerHistory = {};
        g_clipEdited = false;
        g_projectActive = index;
        g_projectAppliedLoad = g_clipLoadSequence;
        ++g_projectSequence;
    }

    // Opens a project clip at its own time `clipMs`, paused or playing.
    void OpenProjectClip(int index, std::uint64_t clipMs, bool play, ProjectMode mode, ULONGLONG now)
    {
        if (!g_projectOpen || index < 0 || index >= static_cast<int>(g_project.clips.size()))
            return;
        const replay::project::Clip& clip = g_project.clips[static_cast<std::size_t>(index)];
        const std::wstring path = ProjectClipPath(clip);
        if (index < static_cast<int>(g_projectMissing.size()) && g_projectMissing[static_cast<std::size_t>(index)])
        {
            ProjectSay(TRF("project.msg.clip_gone", "The clip {file} is not in ReplayClips any more.", {{"file", clip.file}}));
            return;
        }
        if (g_recording && g_recordKind != RecordKind::Buffer)
        {
            ProjectSay(replay::keys::Fill(TR("project.msg.stop_recording", "Stop the recording first ({key_record})."), replay::keys::Current()));
            return;
        }
        clipMs = std::clamp(clipMs, clip.inMs, clip.outMs);
        SyncProjectMarkers(); // the clip left keeps its edits
        g_projectMode = mode;
        g_projectDragMs = UINT64_MAX;
        // The same file in playback (two pieces of a cut, the same clip
        // again): only the markers and the playhead change.
        if (ProjectOwnsPlayback() && !g_projectPending.active && g_currentClipPath == path)
        {
            const bool sameClip = index == g_projectActive;
            if (!sameClip)
                ApplyProjectMarkers(index);
            const std::uint64_t at = g_transport.PlayheadMs();
            const std::uint64_t gap = at > clipMs ? at - clipMs : clipMs - at;
            // A cut between two pieces in play: no seek at all (the
            // playhead is already there; a seek would hold the frame).
            if (gap > 50 || !play)
                SeekPlayheadTo(clipMs, replay::editor::kSeekPlain, now);
            SetPlaybackPaused(!play, now);
            LogFile() << "PROJECT_PLAYBACK,action=open,clip=" << index << ",same_file=1,clip_ms=" << clipMs << ",play=" << play
                      << ",mode=" << static_cast<int>(mode) << '\n';
            return;
        }
        // Another file: the gallery's load path (read on a thread, then
        // published); the read ahead is taken when it is this file.
        if (g_projectPreload && g_projectPreload->path == path)
        {
            CancelClipRead();
            g_clipRead = std::move(g_projectPreload);
            replay::clipcompress::worker::SetOpenClip(path);
        }
        else
            CancelProjectPreload();
        if (g_transport.Playing())
            StopPlayback();
        g_projectActive = -1;
        g_projectPending = {true, index, clipMs, play, path};
        g_projectSeekAfter = {};
        g_pendingLoadPath = path;
        ++g_projectSequence;
        LogFile() << "PROJECT_PLAYBACK,action=open,clip=" << index << ",same_file=0,clip_ms=" << clipMs << ",play=" << play
                  << ",mode=" << static_cast<int>(mode) << ",preloaded=" << (g_clipRead && g_clipRead->path == path ? 1 : 0) << '\n';
    }
}

    // ApplyClipTrim at a clip's end marker in normal play: true when the
    // project goes on to its next clip (the playback must not pause).
    bool ProjectReachedClipEnd(ULONGLONG now)
    {
        if (g_projectMode != ProjectMode::Preview || !ProjectOwnsPlayback())
            return false;
        const int next = g_projectActive + 1;
        if (next >= static_cast<int>(g_project.clips.size()))
        {
            LogFile() << "PROJECT_PLAYBACK,action=end,clips=" << g_project.clips.size() << '\n';
            return false; // the project's end: paused there
        }
        // Two pieces of one file that meet (a cut): the markers change and
        // the playback goes on without a seek, right here.
        const replay::project::Clip& clip = g_project.clips[static_cast<std::size_t>(next)];
        const std::uint64_t at = g_transport.PlayheadMs();
        // (the playhead passes the end by up to a frame times the speed)
        if (g_currentClipPath == ProjectClipPath(clip) && (at > clip.inMs ? at - clip.inMs : clip.inMs - at) <= 150)
        {
            ApplyProjectMarkers(next);
            LogFile() << "PROJECT_PLAYBACK,action=next,clip=" << next << ",same_file=1,seamless=1\n";
            return true;
        }
        // Anything else waits for the next tick, after the editor's commands
        // (a seek or a clip load inside the transport's own update would
        // break the frame it is computing): paused at the end meanwhile.
        SetPlaybackPaused(true, now);
        g_projectAdvanceTo = next;
        LogFile() << "PROJECT_PLAYBACK,action=next,clip=" << next << ",same_file=0\n";
        return true;
    }

    void CloseProject(const char* why)
    {
        if (!g_projectOpen)
            return;
        SyncProjectMarkers();
        const bool owned = ProjectOwnsPlayback() || g_projectPending.active;
        ProjectLeavePlayback(why);
        if (owned && g_transport.Playing())
            StopPlayback();
        if (owned)
            g_pendingLoadPath.clear();
        if (g_projectDirty)
            SaveProject(why); // never lost (a clip's markers are saved when left too)
        LogFile() << "PROJECT_CLOSE,why=" << why << ",path=" << Narrow(g_projectPath) << '\n';
        g_project = {};
        g_projectOpen = false;
        g_projectPath.clear();
        g_projectDirty = false;
        g_projectMissing.clear();
        ++g_projectSequence;
    }
namespace
{

    // A plain project file name of the folder ("Chase.rpproj").
    bool PlainProjectFile(const std::string& file)
    {
        const std::string ext = replay::project::kExtension;
        return file.size() > ext.size() && file.size() < replay::editor::kClipNameCapacity &&
            file.compare(file.size() - ext.size(), ext.size(), ext) == 0 && file.find_first_of("\\/:*?\"<>|") == std::string::npos &&
            file.find("..") == std::string::npos;
    }

    std::string CommandName(const replay::editor::Command& command)
    {
        return std::string(command.name, strnlen(command.name, sizeof(command.name)));
    }

    // The project's clip index from a command, refusing one that is not there.
    bool ProjectIndexOf(const replay::editor::Command& command, std::size_t& index)
    {
        index = command.keyframeId;
        if (!g_projectOpen || index >= g_project.clips.size())
        {
            ProjectSay(TR("project.msg.no_clip", "No clip there."));
            return false;
        }
        return true;
    }

    // The clip in playback is the one asked for, and where its playhead is.
    bool ProjectActiveAt(std::size_t index, std::uint64_t& clipMs)
    {
        if (!ProjectOwnsPlayback() || g_projectActive != static_cast<int>(index))
        {
            ProjectSay(replay::keys::Fill(TR("project.msg.open_first", "Open that clip first ({key_select}), then put the playhead where it goes."),
                replay::keys::Current()));
            return false;
        }
        clipMs = g_transport.PlayheadMs();
        return true;
    }

    void HandleProjectCommand(replay::editor::Command& command, ULONGLONG now)
    {
        using replay::editor::CommandType;
        std::string why;
        switch (command.type)
        {
        case CommandType::ProjectNew:
        {
            std::string name = replay::project::CleanName(CommandName(command));
            replay::storage::Folder(kProjectFolder);
            if (name.empty())
            {
                for (int n = 1; n < 10000; ++n)
                {
                    name = "Project " + std::to_string(n);
                    if (GetFileAttributesW(ProjectFilePath(name + replay::project::kExtension).c_str()) == INVALID_FILE_ATTRIBUTES)
                        break;
                }
            }
            if (!replay::project::ValidName(name))
            {
                ProjectSay(TR("project.msg.name_chars", "Letters, numbers, spaces, - and _ only."));
                break;
            }
            const std::wstring path = ProjectFilePath(name + replay::project::kExtension);
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                ProjectSay(TR("project.msg.name_exists", "A project with that name exists."));
                break;
            }
            CloseProject("new");
            g_project = {};
            g_project.name = name;
            g_projectPath = path;
            g_projectOpen = true;
            g_projectDirty = false;
            g_projectMissing.clear();
            ++g_projectSequence;
            SaveProject("new");
            LogFile() << "PROJECT_NEW,name=" << name << '\n';
            break;
        }
        case CommandType::ProjectOpen:
        {
            const std::string file = CommandName(command);
            if (!PlainProjectFile(file))
                break;
            const std::wstring path = ProjectFilePath(file);
            if (g_projectOpen && path == g_projectPath)
                break; // already open
            replay::project::Project loaded;
            std::string error;
            if (!replay::project::LoadFile(path, loaded, error))
            {
                ProjectSay(TRF("project.msg.open_failed", "The project could not be opened: {error}.", {{"error", error}}));
                LogFile() << "PROJECT_OPEN,path=" << file << ",ok=0,error=" << error << '\n';
                break;
            }
            CloseProject("open_other");
            g_project = std::move(loaded);
            g_projectPath = path;
            g_projectOpen = true;
            g_projectDirty = false;
            RefreshProjectMissing();
            ++g_projectSequence;
            LogFile() << "PROJECT_OPEN,path=" << file << ",ok=1,clips=" << g_project.clips.size()
                      << ",total_ms=" << replay::project::TotalMs(g_project) << '\n';
            break;
        }
        case CommandType::ProjectSave:
            SyncProjectMarkers();
            SaveProject("editor");
            break;
        case CommandType::ProjectClose:
            CloseProject("editor");
            break;
        case CommandType::ProjectRename:
        {
            // "file|new name". The name is cleaned (CleanName: the characters a
            // file cannot hold become spaces); the project is written under its new
            // file first and the old file removed only once that worked, so a
            // failed rename leaves the project as it was. Another project's file is
            // never replaced.
            const std::string both = CommandName(command);
            const std::size_t bar = both.find('|');
            if (bar == std::string::npos)
                break;
            const std::string file = both.substr(0, bar);
            const std::string name = replay::project::CleanName(both.substr(bar + 1));
            if (!PlainProjectFile(file))
                break;
            if (!replay::project::ValidName(name))
            {
                ProjectSay(TR("project.msg.name_chars", "Letters, numbers, spaces, - and _ only."));
                break;
            }
            const std::wstring from = ProjectFilePath(file);
            if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                RebuildProjectList();
                break;
            }
            std::vector<std::string> files; // every read root's
            for (const replay::storage::Entry& entry : replay::storage::List(kProjectFolder, L"*"))
                files.push_back(Narrow(entry.name));
            const replay::project::RenameCheck check = replay::project::CheckRename(file, name, files);
            if (check == replay::project::RenameCheck::Exists)
            {
                ProjectSay(TR("project.msg.name_exists", "A project with that name exists."));
                LogFile() << "PROJECT_RENAME,from=" << file << ",to=" << name << ",ok=0,error=exists\n";
                break;
            }
            const bool open = g_projectOpen && from == g_projectPath;
            replay::project::Project project;
            std::string error;
            bool ok = true;
            if (open)
            {
                SyncProjectMarkers();
                project = g_project;
            }
            else
                ok = replay::project::LoadFile(from, project, error);
            const bool sameFile = check == replay::project::RenameCheck::Same || check == replay::project::RenameCheck::CaseOnly;
            const std::wstring to = sameFile ? from : ProjectFilePath(replay::project::NameFile(name));
            if (ok && check == replay::project::RenameCheck::Same && project.name == name)
            {
                RebuildProjectList();
                break; // nothing to change
            }
            if (ok)
            {
                project.name = name;
                ok = replay::project::SaveFile(to, project, error);
            }
            std::wstring renamedPath = to;
            if (ok && check == replay::project::RenameCheck::CaseOnly)
            {
                // The same file: only its capitals change (the content is saved).
                const std::wstring cased = ProjectFilePath(replay::project::NameFile(name));
                if (MoveFileExW(from.c_str(), cased.c_str(), 0))
                    renamedPath = cased;
            }
            else if (ok && !sameFile && !DeleteFileW(from.c_str()))
                LogFile() << "PROJECT_RENAME,old_file_kept=" << file << ",error=" << GetLastError() << '\n'; // two copies, none lost
            if (ok && open)
            {
                g_project.name = name;
                g_projectPath = renamedPath;
                g_projectDirty = false;
                ++g_projectSequence;
            }
            if (!ok)
                ProjectSay(TRF("project.msg.rename_failed", "The project could not be renamed: {error}.", {{"error", error}}));
            LogFile() << "PROJECT_RENAME,from=" << file << ",to=" << name << ",open=" << (open ? 1 : 0) << ",ok=" << (ok ? 1 : 0)
                      << ",error=" << (ok ? "none" : error.c_str()) << '\n';
            RebuildProjectList();
            break;
        }
        case CommandType::ProjectDelete:
        {
            const std::string file = CommandName(command);
            if (!PlainProjectFile(file))
                break;
            const std::wstring path = ProjectFilePath(file);
            if (g_projectOpen && path == g_projectPath)
            {
                g_projectDirty = false; // deleted, not saved back
                CloseProject("deleted");
            }
            const bool deleted = DeleteFileW(path.c_str()) != FALSE;
            LogFile() << "PROJECT_DELETE,path=" << file << ",ok=" << (deleted ? 1 : 0) << '\n';
            RebuildProjectList();
            break;
        }
        case CommandType::RefreshProjects:
            RebuildProjectList();
            break;
        case CommandType::ProjectAddClip:
        {
            if (!g_projectOpen)
                break;
            const std::string file = CommandName(command);
            std::uint64_t durationMs = 0;
            bool loadable = false;
            for (const replay::editor::ClipInfo& info : g_editorClipsByDate)
                if (file == info.name)
                {
                    durationMs = info.durationMs;
                    loadable = info.loadable != 0;
                }
            replay::project::Clip clip;
            if (!loadable || !replay::project::MakeClip(file, durationMs, clip))
            {
                ProjectSay(TR("project.msg.add_refused", "That clip cannot be added (another version, or too short)."));
                break;
            }
            const std::size_t at = command.keyframeId == ~0u ? g_project.clips.size()
                                                             : std::min<std::size_t>(command.keyframeId, g_project.clips.size());
            if (!replay::project::AddClip(g_project, at, clip, &why))
            {
                ProjectSay(why);
                break;
            }
            if (g_projectActive >= static_cast<int>(at))
                ++g_projectActive;
            if (g_projectPending.active && g_projectPending.index >= static_cast<int>(at))
                ++g_projectPending.index;
            ProjectChanged();
            LogFile() << "PROJECT_EDIT,action=add,clip=" << file << ",at=" << at << ",clips=" << g_project.clips.size() << '\n';
            break;
        }
        case CommandType::ProjectMoveClip:
        {
            std::size_t from = 0;
            if (!ProjectIndexOf(command, from))
                break;
            const std::size_t to = std::min<std::size_t>(command.target, g_project.clips.size() - 1);
            SyncProjectMarkers();
            if (!replay::project::MoveClip(g_project, from, to, &why))
            {
                if (!why.empty())
                    ProjectSay(why);
                break;
            }
            const auto follow = [&](int& i) {
                if (i < 0)
                    return;
                const std::size_t at = static_cast<std::size_t>(i);
                if (at == from)
                    i = static_cast<int>(to);
                else if (from < at && at <= to)
                    --i;
                else if (to <= at && at < from)
                    ++i;
            };
            follow(g_projectActive);
            follow(g_projectPending.index);
            ProjectChanged();
            LogFile() << "PROJECT_EDIT,action=move,from=" << from << ",to=" << to << '\n';
            break;
        }
        case CommandType::ProjectDuplicateClip:
        {
            std::size_t index = 0;
            if (!ProjectIndexOf(command, index))
                break;
            SyncProjectMarkers();
            if (!replay::project::DuplicateClip(g_project, index, &why))
            {
                ProjectSay(why);
                break;
            }
            if (g_projectActive > static_cast<int>(index))
                ++g_projectActive;
            if (g_projectPending.active && g_projectPending.index > static_cast<int>(index))
                ++g_projectPending.index;
            ProjectChanged();
            LogFile() << "PROJECT_EDIT,action=duplicate,clip=" << index << '\n';
            break;
        }
        case CommandType::ProjectDeleteClip:
        {
            std::size_t index = 0;
            if (!ProjectIndexOf(command, index))
                break;
            const bool active = g_projectActive == static_cast<int>(index) ||
                (g_projectPending.active && g_projectPending.index == static_cast<int>(index));
            if (active)
            {
                ProjectLeavePlayback("clip_deleted");
                if (g_transport.Playing())
                {
                    g_clipEdited = false; // its markers go with it
                    StopPlayback();
                }
                g_pendingLoadPath.clear();
            }
            replay::project::DeleteClip(g_project, index, &why);
            if (g_projectActive > static_cast<int>(index))
                --g_projectActive;
            if (g_projectPending.active && g_projectPending.index > static_cast<int>(index))
                --g_projectPending.index;
            ProjectChanged();
            LogFile() << "PROJECT_EDIT,action=delete,clip=" << index << ",clips=" << g_project.clips.size() << '\n';
            break;
        }
        case CommandType::ProjectSplitClip:
        {
            std::size_t index = 0;
            std::uint64_t at = 0;
            if (!ProjectIndexOf(command, index) || !ProjectActiveAt(index, at))
                break;
            SyncProjectMarkers();
            if (!replay::project::SplitClip(g_project, index, at, &why))
            {
                ProjectSay(why);
                break;
            }
            ProjectChanged();
            ApplyProjectMarkers(static_cast<int>(index)); // the first piece's markers now
            LogFile() << "PROJECT_EDIT,action=split,clip=" << index << ",at_ms=" << at << ",clips=" << g_project.clips.size() << '\n';
            break;
        }
        case CommandType::ProjectTrimClip:
        {
            std::size_t index = 0;
            std::uint64_t at = 0;
            if (!ProjectIndexOf(command, index) || !ProjectActiveAt(index, at))
                break;
            SyncProjectMarkers();
            const replay::project::Clip& clip = g_project.clips[index];
            const std::uint64_t in = command.direction < 0 ? at : clip.inMs;
            const std::uint64_t out = command.direction < 0 ? clip.outMs : at;
            if (!replay::project::SetTrim(g_project, index, in, out, &why))
            {
                if (!why.empty())
                    ProjectSay(why);
                break;
            }
            ProjectChanged();
            ApplyProjectMarkers(static_cast<int>(index));
            LogFile() << "PROJECT_EDIT,action=trim,clip=" << index << ",in_ms=" << g_project.clips[index].inMs
                      << ",out_ms=" << g_project.clips[index].outMs << '\n';
            break;
        }
        case CommandType::ProjectEditClip:
        {
            std::size_t index = 0;
            if (!ProjectIndexOf(command, index))
                break;
            const replay::project::Clip& clip = g_project.clips[index];
            const std::uint64_t at = ProjectOwnsPlayback() && g_projectActive == static_cast<int>(index) ? g_transport.PlayheadMs()
                                                                                                         : clip.inMs;
            OpenProjectClip(static_cast<int>(index), at, false, ProjectMode::Edit, now);
            break;
        }
        case CommandType::ProjectSeek:
        case CommandType::ProjectPlay:
        {
            if (!g_projectOpen || g_project.clips.empty())
                break;
            const bool play = command.type == CommandType::ProjectPlay;
            if (play && command.enabled == 0)
            {
                if (g_transport.Playing())
                    SetPlaybackPaused(true, now);
                break;
            }
            std::size_t index = 0;
            std::uint64_t clipMs = 0;
            if (play && command.timeMs == ~0ull)
            {
                // From where it is: the clip in playback goes on, or the
                // project starts over from its end, or from the start.
                if (ProjectOwnsPlayback())
                {
                    g_projectMode = ProjectMode::Preview;
                    const bool atProjectEnd = g_projectActive + 1 == static_cast<int>(g_project.clips.size()) &&
                        g_transport.PlayheadMs() + 1 >= g_project.clips.back().outMs;
                    if (!atProjectEnd)
                    {
                        SetPlaybackPaused(false, now);
                        break;
                    }
                }
                OpenProjectClip(0, g_project.clips.front().inMs, true, ProjectMode::Preview, now);
                break;
            }
            replay::project::Locate(g_project, command.timeMs, index, clipMs);
            const std::uint32_t kind = play ? replay::editor::kSeekPlain : command.enabled;
            const bool sameFile = ProjectOwnsPlayback() && !g_projectPending.active &&
                g_currentClipPath == ProjectClipPath(g_project.clips[index]);
            if (!play && kind == replay::editor::kSeekDragging)
            {
                // A drag: cheap moves over the file in memory; over another
                // file nothing loads until the drag ends.
                if (sameFile)
                {
                    g_projectDragMs = UINT64_MAX;
                    if (static_cast<int>(index) != g_projectActive)
                        ApplyProjectMarkers(static_cast<int>(index));
                    g_projectMode = ProjectMode::Preview;
                    SeekPlayheadTo(clipMs, kind, now);
                }
                else
                    g_projectDragMs = command.timeMs;
                break;
            }
            if (sameFile && !play)
            {
                g_projectDragMs = UINT64_MAX;
                if (static_cast<int>(index) != g_projectActive)
                    ApplyProjectMarkers(static_cast<int>(index));
                g_projectMode = ProjectMode::Preview;
                SeekPlayheadTo(clipMs, kind, now);
                break;
            }
            OpenProjectClip(static_cast<int>(index), clipMs, play, ProjectMode::Preview, now);
            break;
        }
        default:
            break;
        }
    }
}

    // Each tick after the editor's commands: the project clip that opened,
    // the seek once its puppets stand, the read ahead of the next file and
    // the markers edited back into the project.
    void UpdateProject(ULONGLONG now)
    {
        ReapProjectPreloads(false);
        if (!g_projectOpen)
            return;
        // The next clip asked for at a clip's end (ProjectReachedClipEnd).
        if (g_projectAdvanceTo >= 0)
        {
            const int next = g_projectAdvanceTo;
            g_projectAdvanceTo = -1;
            if (ProjectOwnsPlayback() && g_projectMode == ProjectMode::Preview && next < static_cast<int>(g_project.clips.size()))
                OpenProjectClip(next, g_project.clips[static_cast<std::size_t>(next)].inMs, true, ProjectMode::Preview, now);
        }
        // A project clip being opened: published (or failed) once the
        // gallery's load path has run.
        if (g_projectPending.active && g_pendingLoadPath.empty() && !g_clipRead)
        {
            const ProjectPendingOpen pending = g_projectPending;
            g_projectPending = {};
            if (g_transport.Playing() && g_currentClipPath == pending.path && pending.index >= 0 &&
                pending.index < static_cast<int>(g_project.clips.size()))
            {
                ApplyProjectMarkers(pending.index);
                g_projectSeekAfter = {true, pending.clipMs, pending.play};
            }
            else
            {
                ProjectSay(TR("project.msg.clip_open_failed", "The clip could not be opened."));
                ProjectLeavePlayback("open_failed");
            }
        }
        // The project lost the playback (stopped, another clip, a recording).
        if (g_projectActive >= 0 && !g_projectPending.active && !ProjectOwnsPlayback())
            ProjectLeavePlayback(g_transport.Playing() ? "other_clip" : "stopped");
        if (g_projectSeekAfter.active && ProjectOwnsPlayback() && g_trimReady && !replay::playback::Preparing())
        {
            const ProjectSeekAfterReady seek = g_projectSeekAfter;
            g_projectSeekAfter = {};
            const std::uint64_t at = g_transport.PlayheadMs();
            if ((at > seek.clipMs ? at - seek.clipMs : seek.clipMs - at) > 50 || !seek.play)
                SeekPlayheadTo(seek.clipMs, replay::editor::kSeekPlain, now);
            SetPlaybackPaused(!seek.play, now);
        }
        SyncProjectMarkers();
        // The next clip's file read ahead while this one plays.
        if (g_projectMode == ProjectMode::Preview && ProjectOwnsPlayback() && !g_transport.Paused() && !g_projectPreload &&
            !g_clipRead && g_projectActive + 1 < static_cast<int>(g_project.clips.size()))
        {
            const replay::project::Clip& clip = g_project.clips[static_cast<std::size_t>(g_projectActive)];
            const replay::project::Clip& next = g_project.clips[static_cast<std::size_t>(g_projectActive) + 1];
            const std::wstring nextPath = ProjectClipPath(next);
            const std::uint64_t at = g_transport.PlayheadMs();
            if (nextPath != g_currentClipPath && at + kProjectPreloadLeadMs >= clip.outMs &&
                !g_projectMissing[static_cast<std::size_t>(g_projectActive) + 1])
                g_projectPreload = BeginClipRead(nextPath, false);
        }
        // A read ahead not wanted any more (the order changed, a pause, a seek back)
        // stays until it is taken or another one is needed; one that names a clip not
        // next any more goes.
        if (g_projectPreload && g_projectPreload->done.load())
        {
            const bool wanted = ProjectOwnsPlayback() && g_projectActive + 1 < static_cast<int>(g_project.clips.size()) &&
                ProjectClipPath(g_project.clips[static_cast<std::size_t>(g_projectActive) + 1]) == g_projectPreload->path;
            if (!wanted)
                CancelProjectPreload();
        }
    }
namespace
{

    // ---- Whole project export ----
    // The export dialog's "Whole project" range (StartExport with enabled 1):
    // the project's clips in their order (replay_project.h ExportPlan), each
    // opened as the clip editor opens it (its markers and environment settings
    // from the project) and, once its world stands, its range exported as one
    // piece (exportprobe::RequestSegment: the editor's settings, the clip's in
    // and out, from its opening hold). The add-on keeps each piece's video and
    // sound and joins them after the last one (render_export.cpp). A cancel, a
    // failure or a clip that does not open ends it there (the add-on deletes
    // the pieces).
    constexpr std::uint32_t kExportErrorProjectClipMissing = 18; // render_export::ExportError
    constexpr std::uint32_t kExportErrorProjectClipLoad = 19;
    constexpr ULONGLONG kProjectExportOpenTimeoutMs = 120000;
    constexpr ULONGLONG kProjectExportOpenGraceMs = 2000;
    struct ProjectExportRun
    {
        enum class Step
        {
            Open,   // open the piece's clip
            Wait,   // its file read, its world and puppets standing
            Export  // its piece requested; SegmentDone moves on
        };
        bool active = false;
        std::uint32_t requestSeq = 0;
        std::vector<replay::project::ExportSegment> plan;
        std::vector<replay::projectexport::SegmentFrames> frames; // the plan's (markers' speeds)
        std::size_t index = 0;
        Step step = Step::Open;
        ULONGLONG stepAt = 0;
        std::uint64_t framesBefore = 0; // the pieces done, as encoded
        std::size_t thumbnail = 0;
    };
    ProjectExportRun g_projectExport;

    void EndProjectExport(const char* why)
    {
        if (!g_projectExport.active)
            return;
        LogFile() << "PROJECT_EXPORT,phase=end,why=" << why << ",pieces_done=" << g_projectExport.index
                  << ",of=" << g_projectExport.plan.size() << ",frames=" << g_projectExport.framesBefore << '\n';
        g_projectExport = {};
    }

    void FailProjectExport(std::uint32_t error, const char* why)
    {
        replay::exportprobe::FailProject(error, why, LogFile());
        EndProjectExport(why);
    }

    // StartExport with enabled 1 (the editor wrote the settings in the
    // channel first, as for any export).
    void StartProjectExport(std::uint32_t requestSeq, ULONGLONG now)
    {
        if (g_projectExport.active)
        {
            LogFile() << "PROJECT_EXPORT,phase=request,status=busy\n";
            return;
        }
        if (!replay::exportprobe::Enabled())
            return;
        const std::uint32_t state = replay::exportprobe::StateValue();
        const auto idle = [](std::uint32_t s) {
            using St = replay::exportchannel::State;
            return s == static_cast<std::uint32_t>(St::Off) || s == static_cast<std::uint32_t>(St::Done) ||
                s == static_cast<std::uint32_t>(St::Failed) || s == static_cast<std::uint32_t>(St::Cancelled);
        };
        if (!idle(state))
        {
            LogFile() << "PROJECT_EXPORT,phase=request,status=busy,state=" << state << '\n';
            return;
        }
        if (!g_projectOpen || g_project.clips.empty())
        {
            ProjectSay(TR("project.msg.export_empty", "Add clips to the project before exporting it."));
            replay::exportprobe::FailProject(kExportErrorProjectClipLoad, "no_project", LogFile());
            return;
        }
        SyncProjectMarkers();
        RefreshProjectMissing();
        for (std::size_t n = 0; n < g_project.clips.size(); ++n)
        {
            if (!g_projectMissing[n])
                continue;
            // Said before anything is exported (nothing to delete afterwards).
            ProjectSay(TRF("project.msg.export_missing", "The clip {file} is not in ReplayClips: the project cannot be exported.",
                {{"file", g_project.clips[n].file}}));
            replay::exportprobe::FailProject(kExportErrorProjectClipMissing, "clip_missing", LogFile());
            LogFile() << "PROJECT_EXPORT,phase=refused,clip=" << n << ",file=" << g_project.clips[n].file << '\n';
            return;
        }
        if (g_recording && g_recordKind != RecordKind::Buffer)
        {
            ProjectSay(replay::keys::Fill(TR("project.msg.stop_recording", "Stop the recording first ({key_record})."), replay::keys::Current()));
            replay::exportprobe::FailProject(kExportErrorProjectClipLoad, "recording", LogFile());
            return;
        }
        ProjectExportRun run;
        run.active = true;
        run.requestSeq = requestSeq;
        run.plan = replay::project::ExportPlan(g_project);
        std::vector<replay::projectexport::SegmentInput> inputs;
        for (const replay::project::ExportSegment& segment : run.plan)
        {
            replay::projectexport::SegmentInput input;
            input.startMs = segment.inMs;
            input.endMs = segment.outMs;
            // A clip never opened in the project takes its file's markers at
            // its first open: counted at 100 % until then.
            if (segment.ownMarkers)
                for (const replay::camera::Keyframe& key : segment.markers.Keyframes())
                    if (key.markerType != replay::editor::MarkerType::Anchor)
                        input.speeds.push_back({key.timeMs, static_cast<double>(replay::camera::SpeedFactor(key.speedIndex))});
            inputs.push_back(std::move(input));
        }
        run.frames = replay::projectexport::PlanFrames(inputs, replay::exportprobe::ExportRate());
        run.thumbnail = replay::projectexport::ThumbnailSegment(run.frames);
        run.step = ProjectExportRun::Step::Open;
        run.stepAt = now;
        replay::exportprobe::BeginProject(static_cast<std::uint32_t>(run.plan.size()), replay::projectexport::TotalFrames(run.frames), LogFile());
        LogFile() << "PROJECT_EXPORT,phase=start,clips=" << run.plan.size() << ",frames_planned=" << replay::projectexport::TotalFrames(run.frames)
                  << ",thumbnail_piece=" << run.thumbnail << ",project=" << g_project.name << '\n';
        g_projectExport = std::move(run);
    }
}

    // Each tick after the editor's commands and UpdateProject.
    void UpdateProjectExport(ULONGLONG now)
    {
        ProjectExportRun& run = g_projectExport;
        if (!run.active)
            return;
        using St = replay::exportchannel::State;
        const St state = static_cast<St>(replay::exportprobe::StateValue());
        if (run.step == ProjectExportRun::Step::Export)
        {
            if (state != St::SegmentDone)
            {
                if (state == St::Done || state == St::Failed || state == St::Cancelled || state == St::Off)
                {
                    if (state == St::Done)
                    {
                        run.framesBefore += replay::exportprobe::FramesEncoded();
                        ++run.index;
                    }
                    EndProjectExport(state == St::Done ? "done" : state == St::Cancelled ? "cancelled" : "failed");
                }
                return; // the piece runs
            }
            const std::uint64_t frames = replay::exportprobe::FramesEncoded();
            LogFile() << "PROJECT_EXPORT,phase=piece_done,piece=" << run.index << ",frames=" << frames
                      << ",planned=" << (run.index < run.frames.size() ? run.frames[run.index].frames : 0) << '\n';
            run.framesBefore += frames;
            ++run.index;
            replay::exportprobe::ProjectProgress(static_cast<std::uint32_t>(run.index), run.framesBefore);
            run.step = ProjectExportRun::Step::Open;
            run.stepAt = now;
            if (run.index >= run.plan.size())
            {
                FailProjectExport(kExportErrorProjectClipLoad, "past_last_piece"); // the last piece ends in Done
                return;
            }
        }
        else if (state != St::SegmentDone)
        {
            // Cancelled (or failed) between two pieces.
            EndProjectExport(state == St::Cancelled ? "cancelled" : "stopped");
            return;
        }
        if (!g_projectOpen || g_project.clips.size() != run.plan.size() || run.index >= g_project.clips.size())
        {
            FailProjectExport(kExportErrorProjectClipLoad, "project_changed");
            return;
        }
        const replay::project::Clip& clip = g_project.clips[run.index];
        if (run.step == ProjectExportRun::Step::Open)
        {
            if (run.index < g_projectMissing.size() && g_projectMissing[run.index])
            {
                ProjectSay(TRF("project.msg.export_missing", "The clip {file} is not in ReplayClips: the project cannot be exported.",
                    {{"file", clip.file}}));
                FailProjectExport(kExportErrorProjectClipMissing, "clip_missing");
                return;
            }
            LogFile() << "PROJECT_EXPORT,phase=open,piece=" << run.index << ",file=" << clip.file << ",in_ms=" << clip.inMs
                      << ",out_ms=" << clip.outMs << '\n';
            OpenProjectClip(static_cast<int>(run.index), clip.inMs, false, ProjectMode::Edit, now);
            run.step = ProjectExportRun::Step::Wait;
            run.stepAt = now;
            return;
        }
        // Wait: the clip in playback is this piece's, read, its world loaded,
        // its puppets built, the seek to its in point settled (the probe then
        // seeks to the range's start and waits for the settle again).
        const bool open = ProjectOwnsPlayback() && g_projectActive == static_cast<int>(run.index);
        const bool loading = g_projectPending.active || g_projectSeekAfter.active || ClipReadPending() || !g_pendingLoadPath.empty() ||
            ClipLoadWaiting() || replay::playback::Preparing() || !g_trimReady || g_settle.Active();
        if (!open || loading)
        {
            const ULONGLONG waited = now - run.stepAt;
            if ((!open && !g_projectPending.active && waited > kProjectExportOpenGraceMs) || waited > kProjectExportOpenTimeoutMs)
            {
                ProjectSay(TRF("project.msg.export_open_failed", "The clip {file} could not be opened: the project export stopped.",
                    {{"file", clip.file}}));
                FailProjectExport(kExportErrorProjectClipLoad, waited > kProjectExportOpenTimeoutMs ? "clip_open_timeout" : "clip_open_failed");
            }
            return;
        }
        replay::exportprobe::Segment segment;
        segment.index = static_cast<std::uint32_t>(run.index);
        segment.count = static_cast<std::uint32_t>(run.plan.size());
        segment.startMs = clip.inMs;
        segment.endMs = clip.outMs;
        segment.earliestMs = g_transport.StartMs();
        segment.framesBefore = run.framesBefore;
        for (std::size_t k = run.index + 1; k < run.frames.size(); ++k)
            segment.framesAfter += run.frames[k].frames;
        segment.thumbnail = run.index == run.thumbnail;
        if (!replay::exportprobe::RequestSegment(run.requestSeq, segment, LogFile(), ExportOutputFrames))
        {
            FailProjectExport(kExportErrorProjectClipLoad, "piece_refused");
            return;
        }
        run.step = ProjectExportRun::Step::Export;
        run.stepAt = now;
    }
namespace
{

    void FillProjectSnapshot(replay::editor::Snapshot& snapshot)
    {
        snapshot.projectActiveClip = ~0u;
        snapshot.projectListCount = static_cast<std::uint32_t>(g_projectList.size());
        for (std::size_t n = 0; n < g_projectList.size(); ++n)
            snapshot.projects[n] = g_projectList[n];
        snapshot.projectListSequence = g_projectListSequence;
        snapshot.projectSequence = g_projectSequence;
        snapshot.projectMessageSequence = g_projectMessageSequence;
        // Cut on a character's start (a translated message is UTF-8).
        const std::string message = g_projectMessage.substr(0, replay::i18n::Boundary(g_projectMessage, sizeof(snapshot.projectMessage) - 1));
        std::snprintf(snapshot.projectMessage, sizeof(snapshot.projectMessage), "%s", message.c_str());
        if (GetTickCount64() < g_projectSaveFailedUntil)
            snapshot.projectFlags |= replay::editor::ProjectSaveFailed;
        if (!g_projectOpen)
            return;
        snapshot.projectFlags |= replay::editor::ProjectOpenFlag;
        if (g_projectDirty)
            snapshot.projectFlags |= replay::editor::ProjectDirty;
        const bool owns = ProjectOwnsPlayback();
        if (owns && g_projectMode == ProjectMode::Preview)
            snapshot.projectFlags |= replay::editor::ProjectPreviewing;
        if (owns && g_projectMode == ProjectMode::Edit)
            snapshot.projectFlags |= replay::editor::ProjectEditingClip;
        if (g_projectPending.active || g_projectSeekAfter.active)
            snapshot.projectFlags |= replay::editor::ProjectLoadingClip;
        if (g_projectPreload && g_projectPreload->done.load() && g_projectPreload->ok)
            snapshot.projectFlags |= replay::editor::ProjectClipPreloaded;
        std::snprintf(snapshot.projectName, sizeof(snapshot.projectName), "%s", g_project.name.c_str());
        const std::wstring file = g_projectPath.substr(g_projectPath.find_last_of(L'\\') + 1);
        std::snprintf(snapshot.projectFile, sizeof(snapshot.projectFile), "%s", Narrow(file).c_str());
        snapshot.projectTotalMs = replay::project::TotalMs(g_project);
        snapshot.projectClipCount = static_cast<std::uint32_t>(
            std::min<std::size_t>(g_project.clips.size(), replay::editor::kMaxProjectClipsInSnapshot));
        for (std::uint32_t n = 0; n < snapshot.projectClipCount; ++n)
        {
            const replay::project::Clip& clip = g_project.clips[n];
            replay::editor::ProjectClipInfo& info = snapshot.projectClips[n];
            std::snprintf(info.file, sizeof(info.file), "%s", clip.file.c_str());
            info.inMs = static_cast<std::uint32_t>(clip.inMs);
            info.outMs = static_cast<std::uint32_t>(clip.outMs);
            info.rawMs = static_cast<std::uint32_t>(clip.rawDurationMs);
            info.markerCount = static_cast<std::uint32_t>(clip.markers.Keyframes().size());
            if (n < g_projectMissing.size() && g_projectMissing[n])
                info.flags |= replay::editor::ProjectClipMissing;
            if (clip.hasMarkers)
                info.flags |= replay::editor::ProjectClipOwnMarkers;
        }
        const int shown = owns ? g_projectActive : g_projectPending.active ? g_projectPending.index : -1;
        if (shown >= 0 && shown < static_cast<int>(snapshot.projectClipCount))
        {
            snapshot.projectActiveClip = static_cast<std::uint32_t>(shown);
            snapshot.projectClips[shown].flags |= replay::editor::ProjectClipActive;
        }
        if (g_projectDragMs != UINT64_MAX)
            snapshot.projectPlayheadMs = g_projectDragMs;
        else if (owns)
            snapshot.projectPlayheadMs = replay::project::ToProjectMs(g_project, static_cast<std::size_t>(g_projectActive), g_transport.PlayheadMs());
        else if (g_projectPending.active)
            snapshot.projectPlayheadMs = replay::project::ToProjectMs(g_project, static_cast<std::size_t>(g_projectPending.index), g_projectPending.clipMs);
        // A project clip's markers changed and not saved yet: the clip
        // editor's "unsaved changes" (Ctrl+S saves the project).
        if (owns && g_projectDirty)
            snapshot.flags |= replay::editor::SnapshotClipEdited;
    }
}

    void ProcessEditorCommands(ULONGLONG now)
    {
        ServicePendingLoad(now);
        REPLAY_PROFILE_ZONE("tick.editor_commands");
        replay::editor::Command command{};
        std::uint32_t processed = 0;
        while (processed < 128 && replay::editor::TryPopCommand(command))
        {
            ++processed;
            // Menu sounds, one per key press, stay out of the log.
            if (command.type == replay::editor::CommandType::PlayUiSound)
            {
                PlayEditorUiSound(command);
                continue;
            }
            // The loading cue's keep-alive (every 2 s) too: logged when it starts or stops.
            if (command.type == replay::editor::CommandType::SetLoadingCue)
            {
                const bool was = EditorDrawsLoadingCue();
                g_editorLoadingCueAt = command.enabled ? std::max<ULONGLONG>(GetTickCount64(), 1) : 0;
                if (was != EditorDrawsLoadingCue())
                    LogFile() << "EDITOR_LOADING_CUE,editor_draws=" << (EditorDrawsLoadingCue() ? 1 : 0) << '\n';
                continue;
            }
            if (command.type == replay::editor::CommandType::LightAdd ||
                command.type == replay::editor::CommandType::LightUpdate ||
                command.type == replay::editor::CommandType::LightDelete ||
                command.type == replay::editor::CommandType::LightClear ||
                command.type == replay::editor::CommandType::LightCopyMarker ||
                command.type == replay::editor::CommandType::LightRotateMarker)
            {
                using replay::editor::CommandType;
                replay::camera::Track markersBefore;
                if (g_transport.Playing())
                    markersBefore = g_cameraTrack;
                std::vector<replay::editor::AuthoredLight>& lights = g_cameraTrack.AuthoredLights();
                bool changed = false;
                const auto normalize = [](replay::editor::AuthoredLight& light) {
                    light.range = std::clamp(light.range, 0.5f, 250.0f);
                    light.intensity = std::clamp(light.intensity, 0.0f, 300.0f);
                    light.aimPitch = std::clamp(light.aimPitch, -180.0f, 180.0f);
                    light.aimYaw = WrapLightDegrees(light.aimYaw);
                    light.beamAngle = std::clamp(light.beamAngle, 0.0f, 360.0f);
                    light.aimRoll = WrapLightDegrees(light.aimRoll);
                    light.colorR = std::min(light.colorR, 255u);
                    light.colorG = std::min(light.colorG, 255u);
                    light.colorB = std::min(light.colorB, 255u);
                    light.enabled = light.enabled ? 1u : 0u;
                    light.coneEnabled = light.coneEnabled ? 1u : 0u;
                    light.reflectorEnabled = light.reflectorEnabled ? 1u : 0u;
                    light.reflectorHidden = light.reflectorHidden ? 1u : 0u;
                    light.blend = static_cast<std::uint32_t>(replay::editor::Blend::Spline);
                    if (light.attachType > 2u)
                        light.attachType = static_cast<std::uint32_t>(replay::editor::AttachType::Default);
                    if (light.blend > 3u)
                        light.blend = static_cast<std::uint32_t>(replay::editor::Blend::None);
                    if (!replay::camera::bones::Valid(light.attachedBone))
                        light.attachedBone = 0;
                    light.spline = replay::camera::spline::Clamped(light.spline);
                    light.name[sizeof(light.name) - 1] = '\0';
                };
                if (command.type == CommandType::LightAdd && lights.size() < replay::editor::kMaxAuthoredLights)
                {
                    replay::editor::AuthoredLight light = command.authoredLight;
                    if (std::isfinite(light.position.x) && std::isfinite(light.position.y) && std::isfinite(light.position.z) &&
                        std::isfinite(light.range) && std::isfinite(light.intensity) &&
                        std::isfinite(light.aimPitch) && std::isfinite(light.aimYaw) && std::isfinite(light.beamAngle) &&
                        std::isfinite(light.aimRoll) &&
                        (light.markerId == 0 || g_cameraTrack.Find(light.markerId)))
                    {
                        normalize(light);
                        if (!light.id)
                            light.id = NextAuthoredLightId(lights);
                        if (!light.name[0])
                            std::snprintf(light.name, sizeof(light.name), "Light %02u", static_cast<unsigned>(lights.size() + 1));
                        lights.push_back(light);
                        changed = true;
                    }
                }
                else if (command.type == CommandType::LightUpdate && command.keyframeId < lights.size())
                {
                    replay::editor::AuthoredLight light = command.authoredLight;
                    if (std::isfinite(light.position.x) && std::isfinite(light.position.y) && std::isfinite(light.position.z) &&
                        std::isfinite(light.range) && std::isfinite(light.intensity) &&
                        std::isfinite(light.aimPitch) && std::isfinite(light.aimYaw) && std::isfinite(light.beamAngle) &&
                        std::isfinite(light.aimRoll) &&
                        (light.markerId == 0 || g_cameraTrack.Find(light.markerId)))
                    {
                        normalize(light);
                        const replay::editor::AuthoredLight& previous = lights[command.keyframeId];
                        light.id = previous.id;
                        if (!light.name[0])
                            std::snprintf(light.name, sizeof(light.name), "Light %02u", static_cast<unsigned>(command.keyframeId + 1));
                        if (light.attachedTarget != previous.attachedTarget)
                        {
                            const auto targetHandle = [](std::uint32_t packed) -> Entity {
                                const replay::playback::Puppet* puppet = packed
                                    ? g_puppets.Find(replay::entity::ReplayId::FromPacked(packed)) : nullptr;
                                return puppet && puppet->packet.kind == replay::entity::EntityKind::Ped ? puppet->clone : 0;
                            };
                            const Entity oldParent = targetHandle(previous.attachedTarget);
                            const Entity newParent = targetHandle(light.attachedTarget);
                            replay::editor::Vec3 world{};
                            bool canConvert = true;
                            if (previous.attachedTarget)
                            {
                                canConvert = AuthoredLightWorldPosition(previous, oldParent, world) ||
                                    CachedAuthoredLightWorldPosition(previous.id, previous.attachedTarget, previous.markerId, world);
                            }
                            else
                                world = previous.position;

                            if (canConvert && light.attachedTarget)
                            {
                                canConvert = AuthoredLightLocalPosition(light, newParent, world, light.position);
                            }
                            else if (canConvert)
                                light.position = world;

                            if (!canConvert)
                            {
                                // Never store a world-space position as an attachment offset. That
                                // turns a light near (-140, -30, 96) into an offset of hundreds of
                                // metres when the character clone is not available yet.
                                light.attachedTarget = previous.attachedTarget;
                                light.attachedBone = previous.attachedBone;
                                light.attachType = previous.attachType;
                                light.position = previous.position;
                                LogFile() << "LIGHT_ATTACH,action=rejected,target=" << command.authoredLight.attachedTarget
                                    << ",reason=parent_unavailable\n";
                            }
                        }
                        const auto same = [](const replay::editor::AuthoredLight& a, const replay::editor::AuthoredLight& b) {
                            return a.markerId == b.markerId && a.position.x == b.position.x && a.position.y == b.position.y &&
                                a.position.z == b.position.z && a.range == b.range && a.intensity == b.intensity &&
                                a.colorR == b.colorR && a.colorG == b.colorG && a.colorB == b.colorB && a.enabled == b.enabled &&
                                a.aimPitch == b.aimPitch && a.aimYaw == b.aimYaw && a.beamAngle == b.beamAngle &&
                                a.coneEnabled == b.coneEnabled && a.aimRoll == b.aimRoll && a.reflectorEnabled == b.reflectorEnabled &&
                                a.reflectorHidden == b.reflectorHidden && a.attachedTarget == b.attachedTarget &&
                                a.lookAtTarget == b.lookAtTarget &&
                                a.id == b.id && a.attachedBone == b.attachedBone && a.attachType == b.attachType && a.blend == b.blend &&
                                replay::camera::spline::Same(a.spline, b.spline) &&
                                std::strncmp(a.name, b.name, sizeof(a.name)) == 0;
                        };
                        changed = !same(lights[command.keyframeId], light);
                        lights[command.keyframeId] = light;
                    }
                }
                else if (command.type == CommandType::LightDelete && command.keyframeId < lights.size())
                {
                    lights.erase(lights.begin() + command.keyframeId);
                    changed = true;
                }
                else if (command.type == CommandType::LightClear)
                {
                    changed = !lights.empty();
                    lights.clear();
                }
                else if (command.type == CommandType::LightCopyMarker && command.keyframeId != command.target &&
                    g_cameraTrack.Find(command.keyframeId) && g_cameraTrack.Find(command.target))
                {
                    std::vector<replay::editor::AuthoredLight> sourceLights;
                    std::size_t destinationCount = 0;
                    for (const replay::editor::AuthoredLight& light : lights)
                    {
                        if (light.markerId == command.keyframeId)
                            sourceLights.push_back(light);
                        if (light.markerId == command.target)
                            ++destinationCount;
                    }
                    const std::size_t capacityAfterReplace = replay::editor::kMaxAuthoredLights - (lights.size() - destinationCount);
                    if (sourceLights.size() <= capacityAfterReplace)
                    {
                        const auto sameSettings = [](const replay::editor::AuthoredLight& a, const replay::editor::AuthoredLight& b) {
                            return a.position.x == b.position.x && a.position.y == b.position.y && a.position.z == b.position.z &&
                                a.range == b.range && a.intensity == b.intensity && a.colorR == b.colorR &&
                                a.colorG == b.colorG && a.colorB == b.colorB && a.enabled == b.enabled &&
                                a.aimPitch == b.aimPitch && a.aimYaw == b.aimYaw && a.beamAngle == b.beamAngle &&
                                a.coneEnabled == b.coneEnabled && a.aimRoll == b.aimRoll && a.reflectorEnabled == b.reflectorEnabled &&
                                a.reflectorHidden == b.reflectorHidden && a.attachedTarget == b.attachedTarget &&
                                a.lookAtTarget == b.lookAtTarget && a.id == b.id &&
                                a.attachedBone == b.attachedBone && a.attachType == b.attachType && a.blend == b.blend &&
                                replay::camera::spline::Same(a.spline, b.spline) &&
                                std::strncmp(a.name, b.name, sizeof(a.name)) == 0;
                        };
                        std::vector<replay::editor::AuthoredLight> oldDestination;
                        for (const replay::editor::AuthoredLight& light : lights)
                            if (light.markerId == command.target)
                                oldDestination.push_back(light);
                        bool same = oldDestination.size() == sourceLights.size();
                        for (std::size_t n = 0; same && n < sourceLights.size(); ++n)
                            same = sameSettings(oldDestination[n], sourceLights[n]);
                        if (!same)
                        {
                            lights.erase(std::remove_if(lights.begin(), lights.end(), [&](const replay::editor::AuthoredLight& light) {
                                return light.markerId == command.target;
                            }), lights.end());
                            for (replay::editor::AuthoredLight light : sourceLights)
                            {
                                light.markerId = command.target;
                                lights.push_back(light);
                            }
                            changed = true;
                        }
                    }
                }
                else if (command.type == CommandType::LightRotateMarker && g_cameraTrack.Find(command.keyframeId))
                {
                    constexpr float kDegreesToRadians = 0.01745329251994329577f;
                    constexpr float kRadiansToDegrees = 57.295779513082320876f;
                    const float pitch = command.camera.rotation.x * kDegreesToRadians;
                    const float roll = command.camera.rotation.y * kDegreesToRadians;
                    const float yaw = command.camera.rotation.z * kDegreesToRadians;
                    const auto rotate = [&](float x, float y, float z) {
                        // Apply roll, then pitch, then yaw to the whole rig.
                        const float x1 = std::cos(roll) * x + std::sin(roll) * z;
                        const float y1 = y;
                        const float z1 = -std::sin(roll) * x + std::cos(roll) * z;
                        const float x2 = x1;
                        const float y2 = std::cos(pitch) * y1 - std::sin(pitch) * z1;
                        const float z2 = std::sin(pitch) * y1 + std::cos(pitch) * z1;
                        return Vector3{std::cos(yaw) * x2 - std::sin(yaw) * y2,
                            std::sin(yaw) * x2 + std::cos(yaw) * y2, z2};
                    };
                    struct RigPose
                    {
                        std::size_t index = 0;
                        Entity parent = 0;
                        Vector3 parentRotation{};
                        replay::editor::Vec3 world{};
                    };
                    std::vector<RigPose> poses;
                    bool resolvedAll = true;
                    double centerX = 0.0, centerY = 0.0, centerZ = 0.0;
                    for (std::size_t i = 0; i < lights.size(); ++i)
                    {
                        const replay::editor::AuthoredLight& light = lights[i];
                        if (light.markerId != command.keyframeId)
                            continue;
                        RigPose pose{};
                        pose.index = i;
                        if (light.attachedTarget)
                        {
                            const replay::playback::Puppet* puppet = g_puppets.Find(
                                replay::entity::ReplayId::FromPacked(light.attachedTarget));
                            if (!puppet || !puppet->clone || !ENTITY::DOES_ENTITY_EXIST(puppet->clone) ||
                                !AuthoredLightWorldPosition(light, puppet->clone, pose.world))
                            {
                                resolvedAll = false;
                                break;
                            }
                            pose.parent = puppet->clone;
                            pose.parentRotation = ENTITY::GET_ENTITY_ROTATION(pose.parent, 2);
                        }
                        else
                            pose.world = light.position;
                        centerX += pose.world.x;
                        centerY += pose.world.y;
                        centerZ += pose.world.z;
                        poses.push_back(pose);
                    }
                    if (resolvedAll && !poses.empty())
                    {
                        const float inverseCount = 1.0f / static_cast<float>(poses.size());
                        const Vector3 center{static_cast<float>(centerX) * inverseCount,
                            static_cast<float>(centerY) * inverseCount, static_cast<float>(centerZ) * inverseCount};
                        std::vector<replay::editor::AuthoredLight> rotated;
                        rotated.reserve(poses.size());
                        for (const RigPose& pose : poses)
                        {
                            replay::editor::AuthoredLight light = lights[pose.index];
                            const Vector3 relative{pose.world.x - center.x, pose.world.y - center.y, pose.world.z - center.z};
                            const Vector3 rotatedRelative = rotate(relative.x, relative.y, relative.z);
                            const replay::editor::Vec3 world{center.x + rotatedRelative.x,
                                center.y + rotatedRelative.y, center.z + rotatedRelative.z};
                            if (pose.parent)
                            {
                                if (!AuthoredLightLocalPosition(light, pose.parent, world, light.position))
                                {
                                    resolvedAll = false;
                                    break;
                                }
                            }
                            else
                                light.position = world;

                            const float lightPitch = light.aimPitch * kDegreesToRadians;
                            const float lightYaw = light.aimYaw * kDegreesToRadians;
                            const float lightRoll = light.aimRoll * kDegreesToRadians;
                            const float localRightX = std::cos(lightYaw), localRightY = std::sin(lightYaw);
                            const float localUpX = std::sin(lightYaw) * std::sin(lightPitch);
                            const float localUpY = -std::cos(lightYaw) * std::sin(lightPitch);
                            const float localUpZ = std::cos(lightPitch);
                            Vector3 forward{-std::sin(lightYaw) * std::cos(lightPitch),
                                std::cos(lightYaw) * std::cos(lightPitch), std::sin(lightPitch)};
                            Vector3 up{localUpX * std::cos(lightRoll) - localRightX * std::sin(lightRoll),
                                localUpY * std::cos(lightRoll) - localRightY * std::sin(lightRoll),
                                localUpZ * std::cos(lightRoll)};
                            if (pose.parent)
                            {
                                forward = RotateAuthoredLightVector(pose.parentRotation, light.attachType, forward.x, forward.y, forward.z);
                                up = RotateAuthoredLightVector(pose.parentRotation, light.attachType, up.x, up.y, up.z);
                            }
                            forward = rotate(forward.x, forward.y, forward.z);
                            up = rotate(up.x, up.y, up.z);
                            if (pose.parent)
                            {
                                forward = UnrotateAuthoredLightVector(pose.parentRotation, light.attachType, forward.x, forward.y, forward.z);
                                up = UnrotateAuthoredLightVector(pose.parentRotation, light.attachType, up.x, up.y, up.z);
                            }
                            const float horizontal = std::sqrt(forward.x * forward.x + forward.y * forward.y);
                            const float nextPitch = std::atan2(forward.z, horizontal);
                            const float nextYaw = std::atan2(-forward.x, forward.y);
                            const float rightX = std::cos(nextYaw), rightY = std::sin(nextYaw);
                            const float baseUpX = std::sin(nextYaw) * std::sin(nextPitch);
                            const float baseUpY = -std::cos(nextYaw) * std::sin(nextPitch);
                            const float baseUpZ = std::cos(nextPitch);
                            const float alongRight = up.x * rightX + up.y * rightY;
                            const float alongUp = up.x * baseUpX + up.y * baseUpY + up.z * baseUpZ;
                            light.aimPitch = nextPitch * kRadiansToDegrees;
                            light.aimYaw = WrapLightDegrees(nextYaw * kRadiansToDegrees);
                            light.aimRoll = WrapLightDegrees(std::atan2(-alongRight, alongUp) * kRadiansToDegrees);
                            rotated.push_back(light);
                        }
                        if (resolvedAll)
                        {
                            for (std::size_t i = 0; i < poses.size(); ++i)
                                lights[poses[i].index] = rotated[i];
                            changed = true;
                        }
                    }
                    if (!resolvedAll)
                        LogFile() << "LIGHT_RIG_ROTATE,ok=0,marker=" << command.keyframeId << ",reason=attachment_unavailable\n";
                }
                if (changed && g_transport.Playing() && !replay::camera::SameMarkers(markersBefore, g_cameraTrack))
                {
                    g_clipEdited = true;
                    PushMarkerHistory(static_cast<std::uint32_t>(command.type), command.keyframeId, &markersBefore);
                }
                continue;
            }
            // The fields too.
            LogFile() << "EDITOR_COMMAND,type=" << static_cast<std::uint32_t>(command.type) << ",marker=" << command.keyframeId
                      << ",time_ms=" << command.timeMs << ",enabled=" << command.enabled << ",direction=" << command.direction
                      << ",mask=0x" << std::hex << command.options.mask << std::dec << '\n';
            // The commands that change the clip's markers. The markers as they were,
            // compared after the command: an undo step and "unsaved changes" only when it
            // changed them (a refused add, a move to the same time, a value at its limit
            // change nothing).
            bool markerCommand = false;
            std::uint32_t historyKind = 0;
            replay::camera::Track markersBefore;
            switch (command.type)
            {
            case replay::editor::CommandType::CaptureCameraKeyframe:
            case replay::editor::CommandType::DeleteCameraKeyframe:
            case replay::editor::CommandType::MoveCameraKeyframe:
            case replay::editor::CommandType::UpdateCameraKeyframe:
            case replay::editor::CommandType::SetCameraKeyframeOptions:
            case replay::editor::CommandType::ResetCameraKeyframe:
            case replay::editor::CommandType::PasteCameraKeyframe:
            case replay::editor::CommandType::SetCameraKeyframeMode:
            case replay::editor::CommandType::DuplicateCameraKeyframe:
            case replay::editor::CommandType::DistributeCameraKeyframes:
            case replay::editor::CommandType::StretchCameraKeyframes:
            case replay::editor::CommandType::LockCameraKeyframes:
                // (Edit Camera's keep is counted in LeaveMarkerCameraEdit.)
                if (g_transport.Playing())
                {
                    markerCommand = true;
                    // A step of the undo history; an option's runs merge per option (the mask).
                    historyKind = command.type == replay::editor::CommandType::SetCameraKeyframeOptions
                        ? 0x10000u | command.options.mask : static_cast<std::uint32_t>(command.type);
                    markersBefore = g_cameraTrack;
                }
                break;
            default:
                break;
            }
            switch (command.type)
            {
            case replay::editor::CommandType::SaveOpenClip:
                // A project's clip: Ctrl+S saves the project.
                if (ProjectOwnsPlayback())
                {
                    SyncProjectMarkers();
                    SaveProject("editor_clip");
                    break;
                }
                // Ctrl+S on a clip with its own file and no changes does not write it again (a
                // compressed clip would go back to full size); a held save of it (or of
                // another clip) is retried now, as a save asked for does. A buffer clip with
                // no file yet is saved.
                if (g_transport.Playing() && !g_clipEdited && !g_memoryClipPath.empty())
                {
                    g_clipSaves.RetryNow();
                    LogFile() << "CLIP_EDIT_SAVE,why=editor,skipped=no_changes,path=" << Narrow(g_memoryClipPath)
                              << ",held=" << g_clipSaves.HeldCount() << '\n';
                    break;
                }
                SaveEditedClip("editor");
                break;

            // The markers' undo and redo.
            case replay::editor::CommandType::UndoMarkers:
            case replay::editor::CommandType::RedoMarkers:
            {
                const bool undo = command.type == replay::editor::CommandType::UndoMarkers;
                std::vector<replay::camera::Track>& from = undo ? g_markerHistory.undo : g_markerHistory.redo;
                std::vector<replay::camera::Track>& to = undo ? g_markerHistory.redo : g_markerHistory.undo;
                if (from.empty() || !g_transport.Playing())
                    break;
                if (g_editingMarker)
                    LeaveMarkerCameraEdit(false);
                to.push_back(g_cameraTrack);
                g_cameraTrack = std::move(from.back());
                from.pop_back();
                g_markerHistory.lastType = 0;
                g_cameraTrackManualOverride = false;
                g_cameraSpring.Reset();
                g_clipEdited = true;
                LogFile() << "CAMERA_MARKER,action=" << (undo ? "undo" : "redo") << ",markers=" << g_cameraTrack.Keyframes().size()
                          << ",undo_left=" << g_markerHistory.undo.size() << ",redo_left=" << g_markerHistory.redo.size() << '\n';
                break;
            }

            case replay::editor::CommandType::ToggleRecording:
                PressRecord();
                break;

            case replay::editor::CommandType::PlayPause:
                if (!g_transport.Playing())
                    StartPlayback(now);
                else
                {
                    // Play from the end of the trimmed clip starts it over.
                    if (g_transport.Paused() && TrimActive() && g_transport.PlayheadMs() + 1 >= g_cameraTrack.Keyframes().back().timeMs)
                        g_transport.Seek(g_transport.StartMs());
                    SetPlaybackPaused(!g_transport.Paused(), now);
                }
                break;

            case replay::editor::CommandType::Stop:
                if (g_recording)
                    StopRecording();
                if (g_transport.Playing())
                    StopPlayback();
                ProjectLeavePlayback("stop");
                break;

            case replay::editor::CommandType::SeekAbsolute:
            case replay::editor::CommandType::StepFrame:
            {
                if (g_recording || PlaybackDurationMs() == 0)
                    break;
                if (!g_transport.Playing() && !StartPlayback(now))
                    break;
                SetPlaybackPaused(true, now);
                std::uint64_t target = command.timeMs;
                if (command.type == replay::editor::CommandType::StepFrame)
                {
                    const std::uint64_t at = g_transport.PlayheadMs();
                    target = command.direction < 0 ? (at > 33 ? at - 33 : 0) : at + 33;
                    // A frame step is a fine scrub (never far by the camera, only by missing
                    // models).
                    g_seekReason = "step";
                    g_seekFine = true;
                    g_seekDrag.OnSeek(now, replay::settle::SeekKind::Plain);
                }
                else
                {
                    // A timeline drag moves cheaply and loads once at its end (editor_protocol.h
                    // kSeekDragging, kSeekDragEnd); SeekPlayheadTo (the project's seeks too).
                    SeekPlayheadTo(target, command.enabled, now);
                    break;
                }
                g_transport.Seek(std::min<std::uint64_t>(target, PlaybackDurationMs()));
                break;
            }

            case replay::editor::CommandType::SetPlaybackSpeed:
            {
                const int wanted = static_cast<int>(std::min<std::uint32_t>(command.enabled,
                    static_cast<std::uint32_t>(std::size(kPlaybackSpeeds)) - 1));
                g_playbackSpeedIndex = wanted;
                LogFile() << "PLAYBACK_SPEED,percent=" << static_cast<int>(kPlaybackSpeeds[wanted] * 100.0 + 0.5)
                          << ",source=editor\n";
                break;
            }

            case replay::editor::CommandType::RefreshClips:
            {
                g_clipWindowFirst = static_cast<std::uint32_t>(command.timeMs);
                const std::uint32_t sort = command.enabled == 1 ? 1u : 0u;
                // Direction 1 is the gallery's page move: the list read before, re-sorted if
                // needed (no re-read of every clip's header on the script thread).
                if (command.direction == 1 && (!g_editorClipsByDate.empty() || ClipListLoading()))
                {
                    // A sort change re-sorts the list read (no disk).
                    if (sort != g_clipSort)
                    {
                        g_clipSort = sort;
                        ApplyClipSort();
                    }
                    else
                        ++g_editorClipListSequence;
                    break;
                }
                g_clipSort = sort;
                RebuildEditorClipList();
                break;
            }

            case replay::editor::CommandType::SetEditorOpen:
                g_editorCoversScreen = command.enabled != 0;
                break;

            case replay::editor::CommandType::SetLightEditorState:
                g_lightEditorOpen = command.enabled != 0;
                g_lightGizmosVisible = (command.target & 1u) != 0;
                break;

            case replay::editor::CommandType::RefreshTargets:
                g_refreshTargetCatalog = true;
                break;

            // A clip renamed from the gallery (R); its thumbnail too, and the open clip's
            // paths when it is the one renamed.
            case replay::editor::CommandType::RenameClip:
            {
                char both[replay::editor::kClipNameCapacity]{};
                std::memcpy(both, command.name, sizeof(both) - 1);
                const std::string text(both);
                const std::size_t bar = text.find('|');
                if (bar == std::string::npos)
                    break;
                const std::string from = text.substr(0, bar);
                std::string to = text.substr(bar + 1);
                if (to.size() < 7 || to.substr(to.size() - 7) != ".rpclip")
                    to += ".rpclip";
                const auto plain = [](const std::string& name) {
                    return name.size() > 7 && name.find_first_of("\\/:*?\"<>|") == std::string::npos && name.find("..") == std::string::npos;
                };
                if (!plain(from) || !plain(to) || from == to)
                {
                    g_renameFailedUntil = from == to ? 0 : GetTickCount64() + 5000;
                    break;
                }
                FinishPendingSave(true); // not while its file is written
                // Renamed where it is (the base folder, or the game folder); never to a
                // name another read root already shows.
                const std::wstring fromPath = FindClipFile(std::wstring(from.begin(), from.end()));
                const std::wstring folder = fromPath.substr(0, fromPath.find_last_of(L'\\') + 1);
                const std::wstring toPath = folder + std::wstring(to.begin(), to.end());
                const bool taken = replay::storage::Exists(FindClipFile(std::wstring(to.begin(), to.end())));
                // Nor over a name a save not written yet holds (MoveFileEx refuses only a file
                // on disk).
                const bool moved = !taken && !g_clipSaves.Holds(toPath) && MoveFileExW(fromPath.c_str(), toPath.c_str(), 0) != FALSE;
                if (taken)
                    SetLastError(ERROR_ALREADY_EXISTS);
                if (moved)
                {
                    g_clipSaves.Rename(fromPath, toPath); // its held save follows it
                    // The thumbnail where the editor wrote it (the base folder, or next to the clip).
                    const auto stem = [](const std::string& clip) { return std::wstring(clip.begin(), clip.end() - 7); };
                    const std::wstring thumbFrom = FindClipFile(stem(from) + L".bmp");
                    MoveFileExW(thumbFrom.c_str(), (thumbFrom.substr(0, thumbFrom.find_last_of(L'\\') + 1) + stem(to) + L".bmp").c_str(),
                        MOVEFILE_REPLACE_EXISTING);
                    if (g_currentClipPath == fromPath)
                        g_currentClipPath = toPath;
                    if (g_memoryClipPath == fromPath)
                    {
                        g_memoryClipPath = toPath;
                        replay::clipcompress::worker::SetOpenClip(toPath);
                    }
                    // The open project names the clip by its file.
                    if (g_projectOpen)
                    {
                        std::size_t renamed = 0;
                        for (replay::project::Clip& clip : g_project.clips)
                            if (clip.file == from)
                            {
                                clip.file = to;
                                ++renamed;
                            }
                        if (renamed)
                            ProjectChanged();
                    }
                    RebuildEditorClipList();
                }
                const DWORD renameError = moved ? 0 : GetLastError();
                // The editor is told when the name was taken or the file in use (it has
                // already gone back to the list).
                g_renameFailedUntil = moved ? 0 : GetTickCount64() + 5000;
                LogFile() << "CLIP_RENAME,from=" << from << ",to=" << to << ",ok=" << (moved ? 1 : 0) << ",error="
                          << renameError << '\n';
                break;
            }

            case replay::editor::CommandType::LoadClip:
            case replay::editor::CommandType::DeleteClip:
            {
                char name[replay::editor::kClipNameCapacity]{};
                std::memcpy(name, command.name, sizeof(name) - 1);
                const std::string text(name);
                // A plain file name of the folder, nothing else.
                if (text.empty() || text.find_first_of("\\/:") != std::string::npos || text.find("..") != std::string::npos)
                    break;
                const std::wstring path = FindClipFile(std::wstring(text.begin(), text.end()));
                if (command.type == replay::editor::CommandType::DeleteClip)
                {
                    if (g_transport.Playing() && path == g_currentClipPath)
                    {
                        g_clipEdited = false; // deleted, not saved back (the autosave brought it back)
                        StopPlayback();
                    }
                    // The player deletes it: its saves not written yet go too (one being written,
                    // once done).
                    g_clipSaves.Drop(path);
                    FinishPendingSave(true);
                    const bool deleted = DeleteFileW(path.c_str()) != FALSE;
                    // Its thumbnail (the editor writes <clip>.bmp in the base folder; an
                    // older one sits next to the clip).
                    const std::size_t dot = text.rfind(".rpclip");
                    if (deleted && dot != std::string::npos)
                        for (const std::wstring& clips : replay::storage::FolderRoots(kClipFolder))
                            DeleteFileW((clips + L"\\" + std::wstring(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(dot)) + L".bmp").c_str());
                    LogFile() << "CLIP_DELETE,path=" << text << ",ok=" << deleted << '\n';
                    RebuildEditorClipList();
                    if (g_projectOpen)
                    {
                        RefreshProjectMissing(); // its project clips say "missing"
                        ++g_projectSequence;
                    }
                    break;
                }
                // A gallery clip opened over a project's clip: the project keeps its markers
                // and lets go of the playback.
                if (g_projectActive >= 0 || g_projectPending.active)
                {
                    SyncProjectMarkers();
                    ProjectLeavePlayback("gallery");
                }
                // Leaving a clip with changes saves it on a thread: the clip stops now (its
                // save starts) and the load runs once the save is done (ServicePendingLoad),
                // the editor showing "Opening clip" meanwhile; the file is read on a thread.
                // A recording in progress is saved first, as F4 would: a manual one stops and
                // is saved as a clip; the F4 mark inside the buffer saves the window from the
                // mark to now. Both go through the save queue, and the load waits for the
                // write in flight (ServicePendingLoad), so the chosen clip opens after it.
                if (g_recording && g_recordKind == RecordKind::Manual)
                {
                    LogFile() << "CLIP_LOAD_SAVES_RECORDING,kind=manual,path=" << text << '\n';
                    StopRecording(true);
                }
                else if (g_recording && g_recordKind == RecordKind::Buffer && g_manualInBuffer)
                {
                    const std::uint32_t nowMs = replay::pose::capture::NowMs();
                    LogFile() << "CLIP_LOAD_SAVES_RECORDING,kind=buffer_mark,from_ms=" << g_manualStartMs << ",to_ms=" << nowMs
                              << ",path=" << text << '\n';
                    g_manualInBuffer = false;
                    // The chain's final save is the next part, as at the second F4.
                    g_manualPart = replay::buffercov::FinalPartNumber(g_manualPart);
                    SaveRecordingWindow(g_manualStartMs, nowMs, "manual");
                    g_manualPart = 0;
                }
                if (!g_recording || g_recordKind == RecordKind::Buffer)
                {
                    if (g_transport.Playing())
                        StopPlayback();
                    g_pendingLoadPath = path;
                    LogFile() << "CLIP_LOAD_DEFERRED,path=" << text << ",saving=" << (SavePending() ? 1 : 0) << '\n';
                    break;
                }
                LoadClipNow(path, now);
                break;
            }

            case replay::editor::CommandType::ReservedSetPlaybackDirection:
            {
                std::ofstream& log = LogFile();
                log << "EDITOR_COMMAND_REJECTED,type="
                    << static_cast<std::uint32_t>(command.type)
                    << ",reason=forward_only_transport\n";
                log.flush();
                break;
            }

            case replay::editor::CommandType::ToggleFreeCam:
                if (g_editingMarker)
                    LeaveMarkerCameraEdit(true);
                else if (g_freeCamEnabled)
                    StopFreeCam();
                else
                    StartFreeCam(now);
                break;

            case replay::editor::CommandType::CaptureCameraKeyframe:
                if (g_transport.Playing())
                    AddMarkerAtPlayhead();
                break;

            case replay::editor::CommandType::DeleteCameraKeyframe:
                // The clip's start and end markers stay (they cut the clip).
                if (replay::camera::IsClipBound(g_cameraTrack, command.keyframeId))
                {
                    replay::hud::ShowNotice(TR("hud.marker_bound_delete", "Redemption Editor: the start and end markers cut the clip; move them instead."));
                    break;
                }
                // A marker in Edit Camera is left (cancelled) before it goes; the camera goes
                // back to the track at once.
                if (g_editingMarker == command.keyframeId)
                    LeaveMarkerCameraEdit(false);
                if (g_cameraTrack.RemoveMarker(command.keyframeId))
                {
                    replay::camera::ApplyLastMarkerRule(g_cameraTrack);
                    g_cameraTrackManualOverride = false;
                    g_cameraSpring.Reset();
                    LogFile() << "CAMERA_MARKER,action=delete,id=" << command.keyframeId
                              << ",left=" << g_cameraTrack.Keyframes().size() << '\n';
                }
                break;

            case replay::editor::CommandType::MoveCameraKeyframe:
            {
                // Held between its neighbours (ClampMoveTime): CanPlace alone would let a drag
                // jump over markers and reorder them, the clip's start and end included.
                if (g_editingMarker == command.keyframeId)
                    break;
                const std::uint64_t wanted = std::min(command.timeMs, PlaybackDurationMs());
                const std::uint64_t to = replay::camera::ClampMoveTime(g_cameraTrack, command.keyframeId, wanted);
                if (replay::camera::CanPlace(g_cameraTrack, to, command.keyframeId) &&
                    g_cameraTrack.MoveMarker(command.keyframeId, to))
                {
                    replay::camera::ApplyLastMarkerRule(g_cameraTrack);
                    g_cameraTrackManualOverride = false;
                    g_cameraSpring.Reset();
                    // The playhead goes with the marker, to the time it took (not the time asked,
                    // which is off the marker when the move was clamped).
                    if (g_transport.Paused())
                        g_transport.Seek(to);
                }
                break;
            }

            case replay::editor::CommandType::SetCameraKeyframeOptions:
            {
                // The last marker's camera type is not the player's; it follows the marker
                // before it (ApplyLastMarkerRule, Plan). The editor greys the row; a type sent
                // anyway is dropped here.
                if ((command.options.mask & replay::editor::CameraOptionMode) != 0 &&
                    replay::camera::IsLastMarker(g_cameraTrack, command.keyframeId))
                {
                    command.options.mask &= ~static_cast<std::uint32_t>(replay::editor::CameraOptionMode);
                    LogFile() << "CAMERA_MARKER,action=refused,reason=last_marker_type,id=" << command.keyframeId << '\n';
                    if (command.options.mask == 0)
                        break;
                }
                // A marker that becomes a Free camera starts from the view it showed (the
                // recorded game camera, the preset around the character). Its stored pose is
                // only a placeholder (the world origin for a Game Camera marker).
                replay::camera::LivePose shown{};
                bool toFree = false;
                // A new attach target or mount type keeps the view the marker showed and stores
                // the pose relative to the target from it (the target matrix is recorded
                // again). Left as it was, the relative pose would be {0,0,0} for a marker
                // never attached (the camera inside the character) or one taken in another
                // mount's frame (a jump to another side).
                bool reattach = false;
                if (const replay::camera::Keyframe* key = g_cameraTrack.Find(command.keyframeId))
                {
                    toFree = (command.options.mask & replay::editor::CameraOptionMode) != 0 &&
                        command.options.cameraMode == replay::editor::CameraMode::Free &&
                        key->cameraMode != replay::editor::CameraMode::Free;
                    reattach = !toFree && key->cameraMode == replay::editor::CameraMode::Free &&
                        (command.options.mask & (replay::editor::CameraOptionAttachTarget | replay::editor::CameraOptionAttachType)) != 0;
                    // A new attach bone keeps the view too (the pose stored relative to the bone),
                    // unless the command sets the attach pose numbers itself.
                    const std::uint32_t poseFields = replay::editor::CameraExtraRelPosition | replay::editor::CameraExtraRelRotation;
                    if (!toFree && key->cameraMode == replay::editor::CameraMode::Free &&
                        (command.options.mask & replay::editor::CameraOptionExtra) != 0 &&
                        (command.extra.fields & replay::editor::CameraExtraAttachBone) != 0 && (command.extra.fields & poseFields) == 0)
                        reattach = true;
                    if (toFree || reattach)
                        shown = replay::camera::Resolve(*key, key->timeMs, MarkerScene());
                }
                if (g_cameraTrack.SetOptions(command.keyframeId, command.options, MarkerTimeLimit()))
                {
                    // The bones, attach pose numbers, near clip and DOF numbers (validated; a value
                    // out of range changes nothing of them and is logged).
                    if ((command.options.mask & replay::editor::CameraOptionExtra) != 0)
                        if (replay::camera::Keyframe* key = g_cameraTrack.FindMutable(command.keyframeId))
                        {
                            const bool taken = replay::camera::SetMarkerExtra(*key, command.extra);
                            LogFile() << "CAMERA_MARKER,action=extra,id=" << key->id << ",fields=0x" << std::hex << command.extra.fields
                                      << ",attach_bone=0x" << key->attachBone << ",look_bone=0x" << key->lookAtBone << std::dec
                                      << ",near_clip=" << key->nearClip << ",fstop=" << key->dofFStop << ",lens_mm=" << key->dofLensMm
                                      << ",rel=" << key->relPosition.x << '/' << key->relPosition.y << '/' << key->relPosition.z
                                      << ",rel_rot=" << key->relRotation.x << '/' << key->relRotation.y << '/' << key->relRotation.z
                                      << ",taken=" << (taken ? 1 : 0) << '\n';
                        }
                    if (reattach)
                    {
                        replay::camera::Keyframe* key = g_cameraTrack.FindMutable(command.keyframeId);
                        replay::camera::EntityState target{};
                        if (key && key->attachTarget && replay::camera::LookupAttach(*key, key->timeMs, MarkerScene(), target))
                        {
                            const replay::editor::CameraState view{shown.pose.position,
                                replay::camera::Track::ToEulerDegrees(shown.pose.orientation), shown.pose.fov};
                            replay::camera::CaptureAttachPose(*key, view, target);
                            LogFile() << "CAMERA_MARKER,action=attach_pose,id=" << key->id << ",target=" << key->attachTarget
                                      << ",type=" << static_cast<int>(key->attachType) << ",rel=" << key->relPosition.x << '/'
                                      << key->relPosition.y << '/' << key->relPosition.z
                                      << ",playhead_off_ms=" << static_cast<long long>(g_transport.PlayheadMs()) - static_cast<long long>(key->timeMs) << '\n';
                        }
                    }
                    if (toFree)
                    {
                        if (replay::camera::Keyframe* key = g_cameraTrack.FindMutable(command.keyframeId))
                        {
                            // SetMarkerView stores the view so the marker shows it back (attach and Look
                            // At too).
                            const replay::editor::CameraState view{shown.pose.position,
                                replay::camera::Track::ToEulerDegrees(shown.pose.orientation), shown.pose.fov};
                            replay::camera::SetMarkerView(*key, view, MarkerScene());
                        }
                    }
                    replay::camera::ApplyLastMarkerRule(g_cameraTrack);
                    g_cameraSpring.Reset();
                    // The marker's whole shake.
                    if ((command.options.mask & replay::editor::CameraOptionShake) != 0)
                        if (replay::camera::Keyframe* key = g_cameraTrack.FindMutable(command.keyframeId))
                        {
                            const bool set = replay::camera::SetShake(*key, command.shake);
                            LogFile() << "CAMERA_SHAKE,action=set,id=" << key->id << ",done=" << (set ? 1 : 0) << ','
                                      << replay::camera::mshake::FormatFields(key->shake) << '\n';
                        }
                    // The marker's Spline tuning.
                    if ((command.options.mask & replay::editor::CameraOptionSpline) != 0)
                        if (replay::camera::Keyframe* key = g_cameraTrack.FindMutable(command.keyframeId))
                            if (replay::camera::SetSplineTuning(*key, command.spline))
                                LogFile() << "CAMERA_SPLINE,marker,id=" << key->id << ",overrides="
                                          << static_cast<int>(key->spline.overrides) << ','
                                          << replay::camera::spline::Describe(key->spline) << '\n';
                    LogFile() << "CAMERA_MARKER,action=options,id=" << command.keyframeId << ",mask=0x" << std::hex
                              << command.options.mask << std::dec << (toFree ? ",pose=shown" : "") << '\n';
                }
                break;
            }

            case replay::editor::CommandType::ResetCameraKeyframe:
                if (g_cameraTrack.ResetMarker(command.keyframeId, MarkerTimeLimit()))
                {
                    replay::camera::ApplyLastMarkerRule(g_cameraTrack);
                    g_cameraSpring.Reset();
                    LogFile() << "CAMERA_MARKER,action=reset,id=" << command.keyframeId << '\n';
                }
                break;

            case replay::editor::CommandType::CopyCameraKeyframe:
                if (const replay::camera::Keyframe* key = g_cameraTrack.Find(command.keyframeId))
                {
                    g_copiedMarker = *key;
                    g_hasCopiedMarker = true;
                    LogFile() << "CAMERA_MARKER,action=copy,id=" << key->id << '\n';
                }
                break;

            case replay::editor::CommandType::PasteCameraKeyframe:
                if (g_hasCopiedMarker && g_cameraTrack.PasteMarker(command.keyframeId, g_copiedMarker,
                                             static_cast<replay::editor::CopyContext>(command.enabled), MarkerTimeLimit()))
                {
                    replay::camera::ApplyLastMarkerRule(g_cameraTrack);
                    g_cameraSpring.Reset();
                    LogFile() << "CAMERA_MARKER,action=paste,id=" << command.keyframeId << ",context=" << command.enabled << '\n';
                }
                break;

            case replay::editor::CommandType::EditCameraKeyframe:
                if (command.enabled)
                    EnterMarkerCameraEdit(command.keyframeId, now);
                else
                    LeaveMarkerCameraEdit(command.target == 0); // target 1: cancel
                break;

            case replay::editor::CommandType::UpdateCameraKeyframe:
                if (IsValidEasing(command.easing) && IsValidPath(command.path))
                {
                    replay::editor::CameraState camera = command.camera;
                    camera.fov = std::max(10.0f, std::min(120.0f, camera.fov));
                    if (g_cameraTrack.Update(
                        command.keyframeId,
                        camera,
                        command.easing,
                        command.path))
                    {
                        CaptureMarkerRelations(command.keyframeId, camera);
                        g_cameraTrackManualOverride = false;
                    }
                }
                break;

            // Marker tools.
            case replay::editor::CommandType::DuplicateCameraKeyframe:
            {
                if (g_editingMarker)
                    break;
                const std::uint32_t added = replay::camera::DuplicateMarker(g_cameraTrack, command.keyframeId, MarkerTimeLimit());
                if (added)
                {
                    g_cameraSpring.Reset();
                    g_cameraTrackManualOverride = false;
                    if (const replay::camera::Keyframe* key = g_cameraTrack.Find(added))
                    {
                        if (g_transport.Paused())
                            g_transport.Seek(key->timeMs);
                    }
                }
                else
                    replay::hud::ShowNotice(TR("hud.marker_no_room", "Redemption Editor: no room for a copy there."));
                LogFile() << "CAMERA_MARKER,action=duplicate,id=" << command.keyframeId << ",added=" << added << '\n';
                break;
            }

            case replay::editor::CommandType::DistributeCameraKeyframes:
            {
                const bool byDistance = command.enabled != 0;
                const bool done = !g_editingMarker &&
                    replay::camera::DistributeMove(g_cameraTrack, command.keyframeId, byDistance, MarkerScene());
                if (done)
                {
                    g_cameraSpring.Reset();
                    g_cameraTrackManualOverride = false;
                }
                else
                    replay::hud::ShowNotice(TR("hud.move_three", "Redemption Editor: a move needs three markers that blend into each other."));
                LogFile() << "CAMERA_MARKER,action=distribute,id=" << command.keyframeId << ",by_distance=" << (byDistance ? 1 : 0)
                          << ",done=" << (done ? 1 : 0) << '\n';
                break;
            }

            case replay::editor::CommandType::StretchCameraKeyframes:
            {
                const bool done = !g_editingMarker &&
                    replay::camera::StretchMove(g_cameraTrack, command.keyframeId, command.enabled, MarkerTimeLimit(), MarkerScene());
                if (done)
                {
                    g_cameraSpring.Reset();
                    g_cameraTrackManualOverride = false;
                }
                else
                    replay::hud::ShowNotice(TR("hud.move_fit", "Redemption Editor: the move does not fit (the clip's end, or its end marker)."));
                LogFile() << "CAMERA_MARKER,action=stretch,id=" << command.keyframeId << ",percent=" << command.enabled
                          << ",done=" << (done ? 1 : 0) << '\n';
                break;
            }

            case replay::editor::CommandType::LockCameraKeyframes:
            {
                if (g_editingMarker)
                    break;
                const replay::editor::AttachType type =
                    static_cast<std::uint8_t>(command.options.attachType) <= static_cast<std::uint8_t>(replay::editor::AttachType::Flexible)
                    ? command.options.attachType : replay::editor::AttachType::Default;
                const int changed = replay::camera::LockMarkers(g_cameraTrack, command.keyframeId, command.enabled != 0,
                    command.target, type, MarkerScene());
                if (changed)
                {
                    g_cameraSpring.Reset();
                    g_cameraTrackManualOverride = false;
                }
                LogFile() << "CAMERA_MARKER,action=" << (command.target ? "lock" : "bake") << ",id=" << command.keyframeId
                          << ",target=" << command.target << ",type=" << static_cast<int>(type) << ",every=" << command.enabled
                          << ",changed=" << changed << '\n';
                break;
            }

            case replay::editor::CommandType::SetFreeCamOption:
            {
                const auto field = static_cast<replay::editor::FreeCamField>(command.keyframeId);
                if (SetFreeCamValue(field, command.camera.fov))
                {
                    if (field == replay::editor::FreeCamField::Motion || field == replay::editor::FreeCamField::Acrobatic)
                        g_fcState.Reset();
                    if (field != replay::editor::FreeCamField::Follow)
                        SaveFreeCamOptions();
                    LogFreeCamConfig("editor");
                }
                break;
            }

            case replay::editor::CommandType::FreeCamAction:
            {
                static const LONG kBits[] = {kFcPick, kFcPickNearest, kFcClearTarget, kFcFollow, kFcLevel, kFcBehind, kFcLock};
                if (command.keyframeId == static_cast<std::uint32_t>(replay::editor::FreeCamActionId::SetTarget))
                    SetFreeCamTarget(command.target, "editor_list");
                else if (command.keyframeId < sizeof(kBits) / sizeof(kBits[0]))
                    InterlockedOr(&g_fcActionBits, kBits[command.keyframeId]);
                break;
            }

            case replay::editor::CommandType::SetCameraOverlay:
            {
                const bool on = command.enabled != 0;
                switch (static_cast<replay::editor::OverlayField>(command.keyframeId))
                {
                case replay::editor::OverlayField::Enabled: g_camOverlayEnabled = on; break;
                case replay::editor::OverlayField::WhilePlaying: g_camOverlayWhilePlaying = on; break;
                case replay::editor::OverlayField::TimeLabels: g_camOverlayTimeLabels = on; break;
                case replay::editor::OverlayField::TargetTrail: g_camOverlayTrail = on; break;
                default: break;
                }
                SaveOverlayOptions();
                LogFreeCamConfig("overlay");
                break;
            }

            case replay::editor::CommandType::SetSplineDefaults:
            {
                // The Spline defaults (overrides ignored), saved to the ini.
                const replay::editor::SplineTuning& t = command.spline;
                if (std::isfinite(t.alpha) && std::isfinite(t.easeIn) && std::isfinite(t.easeOut) && std::isfinite(t.weight) &&
                    t.pacing < replay::editor::kSplinePacingCount)
                {
                    replay::editor::SplineTuning d = replay::camera::spline::Clamped(t);
                    d.overrides = 0;
                    g_splineDefaults = d;
                    SaveSplineDefaults();
                    LogSplineDefaults("editor");
                }
                break;
            }

            case replay::editor::CommandType::EditorUiNote:
            {
                // The editor's own note.
                char note[sizeof(command.name) + 1]{};
                std::memcpy(note, command.name, sizeof(command.name));
                for (char& ch : note)
                    if (ch == '\n' || ch == '\r')
                        ch = ' ';
                LogFile() << "CAMERA_FX_UI," << note << ",playhead_ms=" << g_transport.PlayheadMs() << '\n';
                break;
            }


            case replay::editor::CommandType::SaveBuffer:
                SaveBuffer();
                break;

            case replay::editor::CommandType::SetReplayDeadEye:
            {
                g_recConfig.replayDeadEye = command.enabled != 0;
                char workingDirectory[MAX_PATH]{};
                GetCurrentDirectoryA(MAX_PATH, workingDirectory);
                replay::identity::IniWrite("ReplayDeadEye", g_recConfig.replayDeadEye ? "1" : "0",
                    replay::identity::IniPathIn(workingDirectory).c_str());
                LogFile() << "RECORDING_CONFIG,replay_dead_eye=" << (g_recConfig.replayDeadEye ? 1 : 0) << ",from=editor\n";
                break;
            }

            case replay::editor::CommandType::SetReplayBuffer:
                SetReplayBuffer(command.enabled != 0, static_cast<std::uint32_t>(command.timeMs), command.keyframeId);
                break;

            // The recording gate's settings, saved to the ini.
            case replay::editor::CommandType::SetRecordCutscenes:
            case replay::editor::CommandType::SetLockCutsceneCamera:
            {
                const bool record = command.type == replay::editor::CommandType::SetRecordCutscenes;
                bool& value = record ? g_recConfig.recordCutscenes : g_recConfig.lockCutsceneCamera;
                value = command.enabled != 0;
                char workingDirectory[MAX_PATH]{};
                GetCurrentDirectoryA(MAX_PATH, workingDirectory);
                replay::identity::IniWrite(record ? "RecordCutscenes" : "LockCutsceneCamera", value ? "1" : "0",
                    replay::identity::IniPathIn(workingDirectory).c_str());
                LogFile() << "RECORDING_CONFIG," << (record ? "record_cutscenes=" : "lock_cutscene_camera=") << (value ? 1 : 0)
                          << ",from=editor\n";
                break;
            }

            // Render export: the settings are
            // in the export channel; refused unless [Export] Enabled=1.
            // enabled 1: the whole project, one clip after the other.
            case replay::editor::CommandType::StartExport:
                if (command.enabled == 1)
                    StartProjectExport(command.keyframeId, now);
                else
                    replay::exportprobe::Request(command.keyframeId, LogFile(), ExportOutputFrames, g_transport.Playing() ? g_transport.StartMs() : 0);
                break;

            case replay::editor::CommandType::CancelExport:
                replay::exportprobe::Cancel("editor", LogFile());
                break;

            // An environment setting of the clip in playback, one step (Recorded is each
            // one's first value).
            case replay::editor::CommandType::SetEnvironment:
            {
                if (!g_transport.Playing() || !g_clip.environment.recorded)
                    break;
                replay::EnvironmentOverride& edit = g_clip.environment.edit;
                const replay::EnvironmentOverride before = edit;
                const int step = command.direction < 0 ? -1 : 1;
                const char* field = "none";
                switch (static_cast<replay::editor::EnvironmentField>(command.keyframeId))
                {
                case replay::editor::EnvironmentField::Clock:
                    field = "clock";
                    edit.clockMinutes = replay::envoverride::StepClock(edit.clockMinutes,
                        RecordedEnvironmentAt(EnvironmentOriginMs()).clockS, step);
                    break;
                case replay::editor::EnvironmentField::Weather:
                    field = "weather";
                    edit.weather = replay::envoverride::StepWeather(edit.weather, step);
                    break;
                case replay::editor::EnvironmentField::Blend:
                    field = "blend";
                    edit.blendS = replay::envoverride::StepBlend(edit.blendS, step);
                    break;
                case replay::editor::EnvironmentField::Clouds:
                    field = "clouds";
                    edit.cloudsLive = edit.cloudsLive ? 0 : 1;
                    break;
                case replay::editor::EnvironmentField::ResetAll:
                    field = "reset";
                    edit = replay::EnvironmentOverride{};
                    break;
                default:
                    break;
                }
                if (edit != before)
                    EnvironmentEdited(field);
                break;
            }

            // Replay Settings' "Freeze world when paused".
            case replay::editor::CommandType::SetFreezeOnPause:
            {
                g_recConfig.freezeWorldOnPause = command.enabled != 0;
                char workingDirectory[MAX_PATH]{};
                GetCurrentDirectoryA(MAX_PATH, workingDirectory);
                replay::identity::IniWrite("FreezeWorldOnPause", g_recConfig.freezeWorldOnPause ? "1" : "0",
                    replay::identity::IniPathIn(workingDirectory).c_str());
                LogFile() << "RECORDING_CONFIG,freeze_world_on_pause=" << (g_recConfig.freezeWorldOnPause ? 1 : 0) << ",from=editor\n";
                break;
            }

            // Replay Settings' "Recording indicator".
            case replay::editor::CommandType::SetRecordingIndicator:
            {
                g_recConfig.recordingIndicator = replay::recindicator::FromValue(command.enabled);
                char workingDirectory[MAX_PATH]{};
                GetCurrentDirectoryA(MAX_PATH, workingDirectory);
                replay::identity::IniWrite("RecordingIndicator", replay::recindicator::IniValue(g_recConfig.recordingIndicator),
                    replay::identity::IniPathIn(workingDirectory).c_str());
                LogFile() << "RECORDING_CONFIG,recording_indicator=" << replay::recindicator::IniValue(g_recConfig.recordingIndicator)
                          << ",from=editor\n";
                break;
            }

            // Replay Settings' "Pose detail" (pose_deadband_smooth.h): the
            // tracks begun from now on use it.
            case replay::editor::CommandType::SetPoseDetail:
            {
                g_recConfig.poseDetail = command.enabled == static_cast<std::uint32_t>(replay::pose::smooth::Detail::Maximum)
                    ? replay::pose::smooth::Detail::Maximum
                    : replay::pose::smooth::Detail::Standard;
                if (!g_recConfig.poseCodecCoarse)
                    replay::pose::smooth::SetDetail(g_recConfig.poseDetail);
                char workingDirectory[MAX_PATH]{};
                GetCurrentDirectoryA(MAX_PATH, workingDirectory);
                replay::identity::IniWrite("PoseDetail", replay::pose::smooth::DetailText(g_recConfig.poseDetail),
                    replay::identity::IniPathIn(workingDirectory).c_str());
                LogFile() << "RECORDING_CONFIG,pose_detail=" << replay::pose::smooth::DetailText(g_recConfig.poseDetail)
                          << ",coarse_dev=" << (g_recConfig.poseCodecCoarse ? 1 : 0) << ",from=editor\n";
                break;
            }

            case replay::editor::CommandType::SetClipCompression:
                SetClipCompression(command.enabled != 0, command.keyframeId);
                break;

            case replay::editor::CommandType::CompressClip:
            {
                const std::string name(command.name, strnlen(command.name, sizeof(command.name)));
                replay::clipcompress::worker::RequestFile(std::wstring(name.begin(), name.end()));
                LogFile() << "CLIP_COMPRESS_REQUEST,file=" << name << ",from=editor\n";
                break;
            }

            case replay::editor::CommandType::CompressClipsNow:
                replay::clipcompress::worker::RequestPass();
                LogFile() << "CLIP_COMPRESS_REQUEST,days=" << g_recConfig.compressAfterDays << ",from=editor\n";
                break;

            case replay::editor::CommandType::SetCameraKeyframeMode:
            {
                const replay::camera::Keyframe* key = g_cameraTrack.Find(command.keyframeId);
                if (!key || static_cast<std::uint32_t>(command.mode) > 3)
                    break;
                const bool targeted = command.mode == replay::editor::CameraMode::Target ||
                    command.mode == replay::editor::CameraMode::Follow;
                const std::uint32_t target = targeted ? command.target : 0;
                g_cameraTrack.SetMode(key->id, command.mode, target, {});
                g_pendingFollowKeyframe = 0;
                if (command.mode == replay::editor::CameraMode::Follow && g_transport.Playing())
                {
                    SetPlaybackPaused(true, now);
                    g_transport.Seek(std::min<std::uint64_t>(key->timeMs, PlaybackDurationMs()));
                    g_pendingFollowKeyframe = key->id;
                }
                LogFile() << "CAMERA_KEY,action=mode,id=" << key->id << ",mode=" << static_cast<std::uint32_t>(command.mode)
                          << ",target=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(target)) << '\n';
                break;
            }

            case replay::editor::CommandType::SetCameraTrackEnabled:
                g_cameraTrackEnabled = command.enabled != 0;
                g_cameraTrackManualOverride = !g_cameraTrackEnabled;
                break;

            // Projects.
            case replay::editor::CommandType::ProjectNew:
            case replay::editor::CommandType::ProjectOpen:
            case replay::editor::CommandType::ProjectSave:
            case replay::editor::CommandType::ProjectClose:
            case replay::editor::CommandType::ProjectRename:
            case replay::editor::CommandType::ProjectDelete:
            case replay::editor::CommandType::RefreshProjects:
            case replay::editor::CommandType::ProjectAddClip:
            case replay::editor::CommandType::ProjectMoveClip:
            case replay::editor::CommandType::ProjectDuplicateClip:
            case replay::editor::CommandType::ProjectDeleteClip:
            case replay::editor::CommandType::ProjectSplitClip:
            case replay::editor::CommandType::ProjectTrimClip:
            case replay::editor::CommandType::ProjectEditClip:
            case replay::editor::CommandType::ProjectSeek:
            case replay::editor::CommandType::ProjectPlay:
                HandleProjectCommand(command, now);
                break;

            case replay::editor::CommandType::None:
            default:
                break;
            }
            if (markerCommand && !replay::camera::SameMarkers(markersBefore, g_cameraTrack))
            {
                g_clipEdited = true;
                PushMarkerHistory(historyKind, command.keyframeId, &markersBefore);
            }
            else if (markerCommand && command.type != replay::editor::CommandType::MoveCameraKeyframe)
                LogFile() << "CAMERA_MARKER,action=unchanged,type=" << static_cast<std::uint32_t>(command.type) << ",id="
                          << command.keyframeId << '\n';
        }
        if (processed > 0)
            HoldEndMarkerInClip();
    }

    void PublishEditorSnapshot()
    {
        REPLAY_PROFILE_ZONE("tick.editor_snapshot");
        // Static (the snapshot passes 130 KB with the project; the script thread's
        // stack would hold it, and a copy, every tick).
        static replay::editor::Snapshot snapshot;
        std::memset(&snapshot, 0, sizeof(snapshot));
        snapshot.flags = replay::editor::SnapshotCoreReady;
        if (g_recording) snapshot.flags |= replay::editor::SnapshotRecording;
        if (g_transport.Playing()) snapshot.flags |= replay::editor::SnapshotPlaying;
        if (g_transport.Paused()) snapshot.flags |= replay::editor::SnapshotPaused;
        if (g_transport.Scrubbing()) snapshot.flags |= replay::editor::SnapshotScrubbing;
        if (g_freeCamEnabled) snapshot.flags |= replay::editor::SnapshotFreeCamEnabled;
        if (g_debugMode) snapshot.flags |= replay::editor::SnapshotDebugMode;
        snapshot.bufferHeldMs = 0;
        snapshot.recordingMs = 0;
        if (g_recording && g_recordKind == RecordKind::Manual)
            snapshot.recordingMs = replay::pose::capture::NowMs();
        else if (g_recording && g_manualInBuffer)
            snapshot.recordingMs = replay::pose::capture::NowMs() - g_manualStartMs;
        snapshot.savedClipMs = g_savedClipMs;
        const std::vector<replay::editor::AuthoredLight>& authoredLights = g_cameraTrack.AuthoredLights();
        snapshot.authoredLightCount = static_cast<std::uint32_t>(std::min<std::size_t>(authoredLights.size(), replay::editor::kMaxAuthoredLights));
        snapshot.authoredLightSelected = 0;
        std::copy_n(authoredLights.begin(), snapshot.authoredLightCount, snapshot.authoredLights);
        snapshot.sessionResetSequence = g_sessionResets + g_safeExitEditorCloses; // the add-on closes when it grows (session reset or safe exit)
        snapshot.editingMarker = g_editingMarker;
        if (g_editingMarker)
            snapshot.flags |= replay::editor::SnapshotEditingCamera;
        if (g_clipEdited)
            snapshot.flags |= replay::editor::SnapshotClipEdited;
        if (g_hasCopiedMarker)
            snapshot.flags |= replay::editor::SnapshotMarkerCopied;
        if (!g_markerHistory.undo.empty())
            snapshot.flags |= replay::editor::SnapshotCanUndo;
        if (!g_markerHistory.redo.empty())
            snapshot.flags |= replay::editor::SnapshotCanRedo;
        if (GetTickCount64() < g_loadFailedUntil)
            snapshot.flags |= replay::editor::SnapshotLoadFailed;
        if (GetTickCount64() < g_renameFailedUntil)
            snapshot.flags |= replay::editor::SnapshotRenameFailed;
        if (ClipListLoading())
            snapshot.flags |= replay::editor::SnapshotClipListLoading;
        if (!g_pendingLoadPath.empty())
            snapshot.flags |= replay::editor::SnapshotOpeningClip;
        snapshot.clipLoadSequence = g_clipLoadSequence;
        snapshot.gameLanguage = g_gameLanguage;
        // The seek settle; frozen: the editor holds the picture it took before the
        // jump, with a spinner.
        snapshot.seekLoadState = !g_settle.Active() ? replay::editor::SeekLoadNone
            : g_settle.Far()                        ? replay::editor::SeekLoadFrozen
                                                    : replay::editor::SeekLoadSettling;
        snapshot.seekLoadSequence = g_seekLoadSequence;
        snapshot.seekLoadFarBits = g_settle.Active() ? g_settle.FarBits() : 0;
        snapshot.seekLoadElapsedMs = g_settle.Active() ? static_cast<std::uint32_t>(g_settle.ElapsedMs(GetTickCount64())) : 0;
        snapshot.seekLoadTargetMs = g_settle.Active() ? g_settle.TargetMs() : 0;
        {
            const std::string working = Narrow(replay::clipcompress::worker::FileInWork());
            std::snprintf(snapshot.compressingClip, sizeof(snapshot.compressingClip), "%s", working.c_str());
        }
        if (g_pendingSave)
            snapshot.flags |= replay::editor::SnapshotSavingClip;
        if (g_clipSaves.HeldCount() > 0)
            snapshot.flags |= replay::editor::SnapshotSaveFailed;
        if (replay::exportprobe::ShowsExporting())
            snapshot.flags |= replay::editor::SnapshotExporting; // render export sketch
        snapshot.bufferEnabled = g_recConfig.buffer ? 1u : 0u;
        if (g_recConfig.replayDeadEye)
            snapshot.flags |= replay::editor::SnapshotReplayDeadEye;
        // The recording gate.
        if (g_recConfig.recordCutscenes)
            snapshot.flags |= replay::editor::SnapshotRecordCutscenes;
        if (g_recConfig.lockCutsceneCamera)
            snapshot.flags |= replay::editor::SnapshotLockCutsceneCamera;
        if (g_recConfig.freezeWorldOnPause)
            snapshot.flags |= replay::editor::SnapshotFreezeOnPause;
        snapshot.recordingIndicator = static_cast<std::uint32_t>(g_recConfig.recordingIndicator);
        snapshot.poseDetail = g_recConfig.poseCodecCoarse ? 0u : static_cast<std::uint32_t>(g_recConfig.poseDetail);
        snapshot.recordPauseReasons = g_recording && replay::pose::capture::IsClockPaused() ? g_gateReasons : 0u;
        snapshot.spanTotal = static_cast<std::uint32_t>(g_clip.spans.size());
        snapshot.spanCount = std::min(snapshot.spanTotal, replay::editor::kMaxSpansInSnapshot);
        for (std::uint32_t index = 0; index < snapshot.spanCount; ++index)
        {
            const replay::gate::Span& span = g_clip.spans[index];
            snapshot.spans[index] = {span.startMs, span.endMs, static_cast<std::uint32_t>(span.kind), 0u};
        }
        snapshot.cutCount = static_cast<std::uint32_t>(std::min<std::size_t>(g_clip.cuts.size(), replay::editor::kMaxCutsInSnapshot));
        for (std::uint32_t index = 0; index < snapshot.cutCount; ++index)
            snapshot.cuts[index] = g_clip.cuts[index];
        // The world notice of the playback start.
        {
            const replay::world::Notice& notice = replay::world::CurrentNotice();
            snapshot.worldNoticeSequence = notice.sequence;
            snapshot.worldNoticeKind = notice.kind;
            snapshot.worldChapterRecorded = notice.recordedChapter;
            snapshot.worldChapterNow = notice.currentChapter;
            snapshot.worldDifferences = notice.differences;
            snapshot.worldApplied = notice.applied;
        }
        snapshot.bufferSeconds = g_recConfig.bufferSeconds;
        snapshot.bufferMB = g_recConfig.bufferMB;
        {
            const replay::clipcompress::worker::Status compress = replay::clipcompress::worker::CurrentStatus();
            snapshot.compressEnabled = g_recConfig.compressClips ? 1u : 0u;
            snapshot.compressDays = g_recConfig.compressAfterDays;
            snapshot.compressState = static_cast<std::uint32_t>(compress.state);
            snapshot.compressDone = compress.done;
            snapshot.compressTotal = compress.total;
            snapshot.compressedClips = g_compressedClips;
            snapshot.clipsDiskBytes = g_clipsDiskBytes;
            snapshot.clipsBytes = g_clipsBytes;
        }
        if (HUD::IS_PAUSE_MENU_ACTIVE())
            snapshot.flags |= replay::editor::SnapshotGamePaused;
        if (g_recording && g_recordKind == RecordKind::Buffer)
        {
            snapshot.flags |= replay::editor::SnapshotBufferRunning;
            if (g_manualInBuffer)
                snapshot.flags |= replay::editor::SnapshotBufferManual;
            const std::uint32_t nowMs = replay::pose::capture::NowMs();
            snapshot.bufferHeldMs = nowMs > g_bufferHeadMs ? nowMs - g_bufferHeadMs : 0;
        }
        if (g_cameraTrackEnabled) snapshot.flags |= replay::editor::SnapshotCameraTrackEnabled;
        if (g_cameraTrackManualOverride)
            snapshot.flags |= replay::editor::SnapshotCameraTrackManualOverride;

        g_editorQueueOverflowed =
            g_editorQueueOverflowed || replay::editor::ConsumeQueueOverflowed();
        if (g_editorQueueOverflowed)
            snapshot.flags |= replay::editor::SnapshotCommandQueueOverflowed;

        // Nominal 30 Hz frames of the timeline, for the editor's counter.
        snapshot.playbackFrameIndex = static_cast<std::uint32_t>(g_transport.PlayheadMs() / 33u);
        snapshot.playbackFrameCount = static_cast<std::uint32_t>(PlaybackDurationMs() / 33u + 1u);
        snapshot.playheadMs = g_transport.PlayheadMs();
        snapshot.durationMs = PlaybackDurationMs();
        if (g_clip.fromDisk) snapshot.flags |= replay::editor::SnapshotClipFromDisk;
        if (replay::playback::Preparing() || ClipLoadWaiting() || g_settle.Far()) snapshot.flags |= replay::editor::SnapshotPreparing; // a far jump
        if (ClipLoadWaiting() || g_exitLoad.active) snapshot.flags |= replay::editor::SnapshotLoadingWorld; // the editor's loading cue
        snapshot.speedIndex = static_cast<std::uint32_t>(g_playbackSpeedIndex);
        snapshot.speedPercent = static_cast<std::uint32_t>(kPlaybackSpeeds[g_playbackSpeedIndex] * 100.0 + 0.5);
        snapshot.clipTotal = static_cast<std::uint32_t>(g_editorClips.size());
        {
            const std::uint32_t first = std::min(g_clipWindowFirst,
                snapshot.clipTotal > replay::editor::kMaxClipsInSnapshot ? snapshot.clipTotal - replay::editor::kMaxClipsInSnapshot : 0u);
            snapshot.clipCount = std::min<std::uint32_t>(snapshot.clipTotal - first, replay::editor::kMaxClipsInSnapshot);
            for (std::uint32_t index = 0; index < snapshot.clipCount; ++index)
                snapshot.clips[index] = g_editorClips[first + index];
        }
        snapshot.clipListSequence = g_editorClipListSequence;
        snapshot.savedClipSequence = g_savedClipSequence;
        std::snprintf(snapshot.savedClip, sizeof(snapshot.savedClip), "%s", g_savedClipName.c_str());
        if (!g_currentClipPath.empty())
        {
            const std::string current = Narrow(g_currentClipPath.substr(g_currentClipPath.find_last_of(L'\\') + 1));
            std::snprintf(snapshot.currentClip, sizeof(snapshot.currentClip), "%s", current.c_str());
        }
        snapshot.camera = CurrentEditorCameraState();
        FillProjectSnapshot(snapshot);
        // The environment settings of the clip in playback.
        snapshot.envClockMinutes = -1;
        if (g_transport.Playing())
        {
            const replay::Environment& environment = g_clip.environment;
            if (environment.recorded)
                snapshot.envFlags |= replay::editor::EnvironmentRecorded | replay::editor::EnvironmentEditable;
            if (!environment.sky.empty())
                snapshot.envFlags |= replay::editor::EnvironmentSkyTrack;
            snapshot.envClockMinutes = environment.edit.clockMinutes;
            snapshot.envWeather = environment.edit.weather;
            snapshot.envBlendS = environment.edit.blendS;
            snapshot.envCloudsLive = environment.edit.cloudsLive;
            snapshot.envRecordedStartS = RecordedEnvironmentAt(EnvironmentOriginMs()).clockS;
        }

        // The free camera's options and the overlay.
        snapshot.freeCamFlags = (g_fcSettings.acrobatic ? replay::editor::FreeCamAcrobatic : 0u) |
            (g_fcSettings.walk ? replay::editor::FreeCamWalk : 0u) | (g_fcSettings.lockAltitude ? replay::editor::FreeCamLockAltitude : 0u) |
            (g_fcSettings.collision ? replay::editor::FreeCamCollision : 0u) | (g_fcLocked ? replay::editor::FreeCamLocked : 0u) |
            (g_fcPadActive ? replay::editor::FreeCamPad : 0u) | (g_fcTargetLost ? replay::editor::FreeCamTargetLost : 0u);
        snapshot.freeCamMotion = g_fcSettings.motion == replay::freecam::Motion::Drone ? 1u : 0u;
        snapshot.freeCamFollow = static_cast<std::uint32_t>(g_fcFollow);
        snapshot.freeCamTarget = g_fcTarget;
        for (std::size_t index = 0; index < static_cast<std::size_t>(replay::editor::FreeCamField::Count); ++index)
            snapshot.freeCamValues[index] = FreeCamValue(static_cast<replay::editor::FreeCamField>(index));
        // The shake in force at the playhead (what a new marker starts from).
        snapshot.shakeAtPlayhead = replay::camera::mshake::HeldAt(g_cameraTrack, g_transport.PlayheadMs());
        snapshot.overlayFlags = (g_camOverlayEnabled ? replay::editor::OverlayEnabled : 0u) |
            (g_camOverlayWhilePlaying ? replay::editor::OverlayWhilePlaying : 0u) |
            (g_camOverlayTimeLabels ? replay::editor::OverlayTimeLabels : 0u) | (g_camOverlayTrail ? replay::editor::OverlayTargetTrail : 0u);

        const std::vector<replay::camera::Keyframe>& keyframes = g_cameraTrack.Keyframes();
        snapshot.cameraKeyframeTotal = static_cast<std::uint32_t>(keyframes.size());
        snapshot.cameraKeyframeCount = std::min(
            snapshot.cameraKeyframeTotal,
            replay::editor::kMaxCameraKeyframesInSnapshot);
        for (std::uint32_t index = 0; index < snapshot.cameraKeyframeCount; ++index)
        {
            replay::camera::FillSnapshotKeyframe(keyframes[index], snapshot.cameraKeyframes[index]);
            snapshot.cameraKeyframeShake[index] = keyframes[index].shake;
            snapshot.cameraKeyframeSpline[index] = keyframes[index].spline;
            replay::camera::FillSnapshotExtra(keyframes[index], snapshot.cameraKeyframeExtra[index]);
        }
        snapshot.cameraLive = g_cameraLive; // what the rendered camera got this tick
        snapshot.splineDefaults = g_splineDefaults;
        snapshot.splineWeightActive = SplineWeightActive() ? 1u : 0u;

        // Keyframe targets are the entities alive at this playhead, taken from
        // the clip's lifecycle track rather than the asynchronously loaded puppet
        // list. The latter can briefly omit a valid character while a seek settles.
        // Names use the game's model name (a_m_m_valtownfolk_01, player_zero; the
        // hash in hex when unknown), with the main character first.
        snapshot.targetCount = 0;
        {
            const std::uint32_t targetTime = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                g_transport.PlayheadMs(), 0xFFFFFFFFull));
            static std::vector<replay::entity::CreatePacket> alivePackets;
            static std::uint32_t targetClipSequence = ~0u;
            static std::uint64_t targetAliveStamp = replay::entity::LifecycleTrack::kNoAliveStamp;
            static std::size_t targetAliveBlocks = static_cast<std::size_t>(-1);
            static std::size_t targetCreatedCount = static_cast<std::size_t>(-1);
            const std::uint64_t aliveStamp = g_clip.lifecycle.AliveStamp(targetTime);
            const std::size_t aliveBlocks = g_clip.lifecycle.BlockCount();
            const std::size_t createdCount = g_clip.created.size();
            if (g_refreshTargetCatalog || targetClipSequence != g_clipLoadSequence || targetAliveStamp != aliveStamp ||
                targetAliveBlocks != aliveBlocks || targetCreatedCount != createdCount)
            {
                targetClipSequence = g_clipLoadSequence;
                targetAliveStamp = aliveStamp;
                targetAliveBlocks = aliveBlocks;
                targetCreatedCount = createdCount;
                g_refreshTargetCatalog = false;
                g_clip.lifecycle.AliveAt(targetTime, alivePackets);
            }
            const std::uint32_t liveMain = MainCharacterId();
            static const Hash arthur = MISC::GET_HASH_KEY("player_zero");
            static const Hash john = MISC::GET_HASH_KEY("player_three");
            std::vector<const replay::entity::CreatePacket*> ordered;
            ordered.reserve(alivePackets.size());
            for (const replay::entity::CreatePacket& packet : alivePackets)
                ordered.push_back(&packet);
            std::sort(ordered.begin(), ordered.end(), [](const replay::entity::CreatePacket* a, const replay::entity::CreatePacket* b) {
                return a->id.Packed() < b->id.Packed();
            });
            ordered.erase(std::unique(ordered.begin(), ordered.end(), [](const replay::entity::CreatePacket* a,
                const replay::entity::CreatePacket* b) { return a->id.Packed() == b->id.Packed(); }), ordered.end());
            const auto listed = [](replay::entity::EntityKind kind) {
                return kind == replay::entity::EntityKind::Ped || kind == replay::entity::EntityKind::Horse ||
                    kind == replay::entity::EntityKind::Vehicle || kind == replay::entity::EntityKind::Weapon;
            };
            std::unordered_set<std::uint32_t> activeIds;
            activeIds.reserve(ordered.size());
            for (const replay::entity::CreatePacket* packet : ordered)
                activeIds.insert(packet->id.Packed());
            std::uint32_t main = liveMain;
            if (!main)
                for (const replay::entity::CreatePacket* packet : ordered)
                    if (packet->modelHash == arthur || packet->modelHash == john)
                    {
                        main = packet->id.Packed();
                        break;
                    }
            // Include the whole clip's create catalog after the live entities, so
            // targets which enter later remain selectable. Active entries stay first.
            std::vector<const replay::entity::CreatePacket*> catalog;
            catalog.reserve(std::min<std::size_t>(replay::editor::kMaxTargetsInSnapshot,
                ordered.size() + g_clip.created.size()));
            std::unordered_set<std::uint32_t> catalogIds;
            catalogIds.reserve(catalog.capacity());
            // Entities of the same model get a number (#1, #2); a model seen once none.
            std::unordered_map<std::uint32_t, int> sameModel, numbered;
            // A third pass lists the weapons (attach to a muzzle or a grip), after the
            // characters, horses, animals and vehicles.
            for (int pass = 0; pass < 3; ++pass)
                for (const replay::entity::CreatePacket* packet : ordered)
                {
                    if (catalog.size() >= replay::editor::kMaxTargetsInSnapshot)
                        break;
                    const replay::entity::EntityKind kind = packet->kind;
                    const bool weapon = kind == replay::entity::EntityKind::Weapon;
                    if (!listed(kind))
                        continue;
                    if (weapon != (pass == 2))
                        continue;
                    const bool isMain = packet->id.Packed() == main;
                    if (pass < 2 && (pass == 0) != isMain)
                        continue;
                    if (catalog.size() >= replay::editor::kMaxTargetsInSnapshot)
                        break;
                    if (!catalogIds.insert(packet->id.Packed()).second)
                        continue;
                    catalog.push_back(packet);
                }
            std::vector<const replay::entity::CreatePacket*> createdOrdered;
            createdOrdered.reserve(g_clip.created.size());
            for (const replay::entity::CreatePacket& packet : g_clip.created)
                if (listed(packet.kind) && catalogIds.find(packet.id.Packed()) == catalogIds.end())
                    createdOrdered.push_back(&packet);
            std::sort(createdOrdered.begin(), createdOrdered.end(), [](const replay::entity::CreatePacket* a,
                const replay::entity::CreatePacket* b) { return a->id.Packed() < b->id.Packed(); });
            for (const replay::entity::CreatePacket* packet : createdOrdered)
            {
                if (catalog.size() >= replay::editor::kMaxTargetsInSnapshot)
                    break;
                if (catalogIds.insert(packet->id.Packed()).second)
                    catalog.push_back(packet);
            }
            for (const replay::entity::CreatePacket* packet : catalog)
                ++sameModel[packet->modelHash];
            for (const replay::entity::CreatePacket* packet : catalog)
            {
                if (snapshot.targetCount >= replay::editor::kMaxTargetsInSnapshot)
                    break;
                    const std::uint32_t slot = snapshot.targetCount;
                    replay::playback::Puppet metadata{};
                    metadata.packet = *packet;
                    const CamTargetBones& bones = TargetBonesFor(metadata);
                    snapshot.targetFamily[slot] = bones.family;
                    snapshot.targetBoneMask[slot] = static_cast<std::uint32_t>(bones.mask);
                    snapshot.targetBoneMaskHigh[slot] = static_cast<std::uint32_t>(bones.mask >> 32);
                    snapshot.targetBoneCount[slot] = bones.count;
                    replay::editor::TargetInfo& info = snapshot.targets[snapshot.targetCount++];
                    info.id = packet->id.Packed();
                    info.kind = static_cast<std::uint32_t>(packet->kind);
                    info.active = activeIds.find(info.id) != activeIds.end() ? 1u : 0u;
                    const std::uint32_t model = packet->modelHash;
                    replay::models::Label(model, sameModel[model] > 1 ? ++numbered[model] : 0, info.label, sizeof(info.label));
            }
        }

        if (g_recording)
        {
            std::snprintf(
                snapshot.status,
                sizeof(snapshot.status),
                "REC | %u entities | %.1f s",
                static_cast<unsigned int>(PoseV2RecordedCount()),
                static_cast<double>(GetTickCount64() - g_recordingStartedAt) / 1000.0);
        }
        else if (g_transport.Playing())
        {
            std::snprintf(
                snapshot.status,
                sizeof(snapshot.status),
                "%s | %u puppets | %u keyframes",
                TransportLabel(),
                static_cast<unsigned int>(PoseV2PuppetCount()),
                snapshot.cameraKeyframeTotal);
        }
        else
        {
            std::snprintf(
                snapshot.status,
                sizeof(snapshot.status),
                "%.1f s recorded | PoseTrack v2",
                static_cast<double>(PlaybackDurationMs()) / 1000.0);
        }

        replay::editor::Publish(snapshot);
        const std::uint64_t bridgeEpoch = replay::editor::OnlineEpoch();
        if (bridgeEpoch != 0 && bridgeEpoch != g_loggedEditorBridgeEpoch)
        {
            g_loggedEditorBridgeEpoch = bridgeEpoch;
            std::ofstream& log = LogFile();
            log << "EDITOR_BRIDGE,status=online"
                << ",transport=shared_memory"
                << ",transport_version="
                << replay::editor::kSharedTransportVersion
                << ",api_version=" << replay::editor::kApiVersion
                << ",epoch=" << bridgeEpoch
                << '\n';
            log.flush();
        }
    }

namespace
{
    float LightAngleDelta(float from, float to)
    {
        float delta = std::fmod(to - from + 540.0f, 360.0f) - 180.0f;
        if (delta < -180.0f)
            delta += 360.0f;
        return delta;
    }

    float LightCatmull(float p0, float p1, float p2, float p3, float t)
    {
        const float t2 = t * t, t3 = t2 * t;
        return 0.5f * ((2.0f * p1) + (-p0 + p2) * t +
            (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
            (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
    }

    float InterpolateLightValue(float p0, float p1, float p2, float p3, float t, replay::editor::Blend blend,
        bool angle = false)
    {
        if (angle)
        {
            p0 = p1 + LightAngleDelta(p1, p0);
            p2 = p1 + LightAngleDelta(p1, p2);
            p3 = p1 + LightAngleDelta(p1, p3);
        }
        float value = p1;
        if (blend == replay::editor::Blend::Linear || blend == replay::editor::Blend::Smooth)
            value = p1 + (p2 - p1) * t;
        else if (blend == replay::editor::Blend::Spline)
            value = LightCatmull(p0, p1, p2, p3, t);
        return angle ? WrapLightDegrees(value) : value;
    }

    float SampleLightSplineValue(float p0, float p1, float p2, float p3, double hBefore, double h0, double hAfter,
        float phase, const replay::editor::SplineTuning& fromTuning, const replay::editor::SplineTuning& toTuning,
        bool hasPrevious, bool hasAfter, std::uint8_t channel, bool angle, bool monotone)
    {
        if (!hasPrevious)
            p0 = 2.0f * p1 - p2;
        if (!hasAfter)
            p3 = 2.0f * p2 - p1;
        if (angle)
        {
            p0 = p1 + LightAngleDelta(p1, p0);
            p2 = p1 + LightAngleDelta(p1, p2);
            p3 = p1 + LightAngleDelta(p1, p3);
        }
        const auto linear = [&]() { return p1 + (p2 - p1) * phase; };
        if ((fromTuning.channels & channel) == 0 || h0 <= 1e-9)
            return angle ? WrapLightDegrees(linear()) : linear();

        const double d0 = (static_cast<double>(p2) - p1) / h0;
        double m1 = d0, m2 = d0;
        if (monotone)
        {
            const double dBefore = hasPrevious && hBefore > 1e-9 ? (static_cast<double>(p1) - p0) / hBefore : 0.0;
            const double dAfter = hasAfter && hAfter > 1e-9 ? (static_cast<double>(p3) - p2) / hAfter : 0.0;
            m1 = hasPrevious ? replay::camera::spline::MonotoneInner(hBefore, h0, dBefore, d0)
                : hasAfter ? replay::camera::spline::MonotoneEnd(h0, hAfter, d0, dAfter) : d0;
            m2 = hasAfter ? replay::camera::spline::MonotoneInner(h0, hAfter, d0, dAfter)
                : hasPrevious ? replay::camera::spline::MonotoneEnd(h0, hBefore, d0, dBefore) : d0;
        }
        else
        {
            if (hasPrevious && hBefore > 1e-9)
                m1 = (static_cast<double>(p2) - p0) / (hBefore + h0);
            if (hasAfter && hAfter > 1e-9)
                m2 = (static_cast<double>(p3) - p1) / (h0 + hAfter);
            if (!hasPrevious && hasAfter)
                m1 = replay::camera::spline::NaturalEnd(d0, m2);
            else if (hasPrevious && !hasAfter)
                m2 = replay::camera::spline::NaturalEnd(d0, m1);
        }

        const double easedTime = static_cast<double>(phase) * h0;
        double value = replay::camera::spline::Hermite(p1, p2, m1, m2, h0, easedTime / h0);
        const double weight = SplineWeightActive()
            ? replay::camera::spline::WeightBetween(fromTuning.weight, toTuning.weight, phase) : 0.0;
        if (weight > 0.0f)
        {
            // Same time-aware B-spline pull as camera spline markers. The
            // outer knots are mirrored at the ends of a spline run.
            const double k0 = hasPrevious && hBefore > 1e-9 ? -hBefore : -h0;
            const double k1 = 0.0;
            const double k2 = h0;
            const double k3 = hasAfter && hAfter > 1e-9 ? h0 + hAfter : 2.0 * h0;
            const double knots[6] = {2.0 * k0 - k1, k0, k1, k2, k3, 2.0 * k3 - k2};
            double weights[4]{};
            replay::camera::spline::BSplineWeights(knots, easedTime, weights);
            const double curve = weights[0] * p0 + weights[1] * p1 + weights[2] * p2 + weights[3] * p3;
            value += static_cast<double>(weight) * (curve - value);
        }
        const float result = static_cast<float>(value);
        return angle ? WrapLightDegrees(result) : result;
    }

    int AuthoredLightBoneIndex(Entity entity, std::uint32_t ref)
    {
        if (!ref)
            return -1;
        const std::uint32_t mode = replay::camera::bones::ModeOf(ref);
        const std::uint32_t value = replay::camera::bones::ValueOf(ref);
        if (mode == replay::camera::bones::kModeIndex)
            return static_cast<int>(value);
        if (mode == replay::camera::bones::kModeNamed && value < replay::camera::bones::kNamedCount)
            for (const char* name : replay::camera::bones::kNamed[value].names)
            {
                if (!name)
                    break;
                const int index = ENTITY::GET_ENTITY_BONE_INDEX_BY_NAME(entity, const_cast<char*>(name));
                if (index >= 0)
                    return index;
            }
        return -1;
    }

    Vector3 RotateAuthoredLightVector(const Vector3& rotation, std::uint32_t mount, float x, float y, float z)
    {
        float pitch = 0.0f, roll = 0.0f, heading = 0.0f;
        if (mount == static_cast<std::uint32_t>(replay::editor::AttachType::Rigid))
        {
            pitch = rotation.x;
            roll = rotation.y;
            heading = rotation.z;
        }
        else if (mount == static_cast<std::uint32_t>(replay::editor::AttachType::Default))
            heading = rotation.z;
        constexpr float kRad = 0.01745329251994329577f;
        const float p = pitch * kRad, r = roll * kRad, h = heading * kRad;
        const float cp = std::cos(p), sp = std::sin(p);
        const float cr = std::cos(r), sr = std::sin(r);
        const float ch = std::cos(h), sh = std::sin(h);
        return {
            (ch * cr - sh * sp * sr) * x + (-sh * cp) * y + (ch * sr + sh * sp * cr) * z,
            (sh * cr + ch * sp * sr) * x + (ch * cp) * y + (sh * sr - ch * sp * cr) * z,
            (-cp * sr) * x + sp * y + (cp * cr) * z
        };
    }

    Vector3 UnrotateAuthoredLightVector(const Vector3& rotation, std::uint32_t mount, float x, float y, float z)
    {
        float pitch = 0.0f, roll = 0.0f, heading = 0.0f;
        if (mount == static_cast<std::uint32_t>(replay::editor::AttachType::Rigid))
        {
            pitch = rotation.x;
            roll = rotation.y;
            heading = rotation.z;
        }
        else if (mount == static_cast<std::uint32_t>(replay::editor::AttachType::Default))
            heading = rotation.z;
        constexpr float kRad = 0.01745329251994329577f;
        const float p = pitch * kRad, r = roll * kRad, h = heading * kRad;
        const float cp = std::cos(p), sp = std::sin(p);
        const float cr = std::cos(r), sr = std::sin(r);
        const float ch = std::cos(h), sh = std::sin(h);
        const float r00 = ch * cr - sh * sp * sr;
        const float r01 = -sh * cp;
        const float r02 = ch * sr + sh * sp * cr;
        const float r10 = sh * cr + ch * sp * sr;
        const float r11 = ch * cp;
        const float r12 = sh * sr - ch * sp * cr;
        const float r20 = -cp * sr;
        const float r21 = sp;
        const float r22 = cp * cr;
        return {
            r00 * x + r10 * y + r20 * z,
            r01 * x + r11 * y + r21 * z,
            r02 * x + r12 * y + r22 * z
        };
    }

    bool AuthoredLightParentFrame(const replay::editor::AuthoredLight& light, Entity parent,
        Vector3& origin, Vector3& rotation)
    {
        if (!parent || !ENTITY::DOES_ENTITY_EXIST(parent))
            return false;
        origin = ENTITY::GET_ENTITY_COORDS(parent, TRUE, FALSE);
        if (light.attachedBone)
        {
            const int bone = AuthoredLightBoneIndex(parent, light.attachedBone);
            if (bone >= 0)
                origin = ENTITY::GET_WORLD_POSITION_OF_ENTITY_BONE(parent, bone);
        }
        rotation = ENTITY::GET_ENTITY_ROTATION(parent, 2);
        return std::isfinite(origin.x) && std::isfinite(origin.y) && std::isfinite(origin.z) &&
            std::isfinite(rotation.x) && std::isfinite(rotation.y) && std::isfinite(rotation.z);
    }
    bool AuthoredLightWorldPosition(const replay::editor::AuthoredLight& light, Entity parent,
        replay::editor::Vec3& world)
    {
        Vector3 origin{}, rotation{};
        if (!AuthoredLightParentFrame(light, parent, origin, rotation))
            return false;
        const Vector3 offset = RotateAuthoredLightVector(rotation, light.attachType,
            light.position.x, light.position.y, light.position.z);
        world = {origin.x + offset.x, origin.y + offset.y, origin.z + offset.z};
        return std::isfinite(world.x) && std::isfinite(world.y) && std::isfinite(world.z);
    }

    bool AuthoredLightLocalPosition(const replay::editor::AuthoredLight& light, Entity parent,
        const replay::editor::Vec3& world, replay::editor::Vec3& local)
    {
        Vector3 origin{}, rotation{};
        if (!AuthoredLightParentFrame(light, parent, origin, rotation))
            return false;
        const Vector3 offset = UnrotateAuthoredLightVector(rotation, light.attachType,
            world.x - origin.x, world.y - origin.y, world.z - origin.z);
        local = {offset.x, offset.y, offset.z};
        return std::isfinite(local.x) && std::isfinite(local.y) && std::isfinite(local.z);
    }

    struct CachedAuthoredLightAttachment
    {
        std::uint32_t target = 0;
        std::uint32_t markerId = 0;
        replay::editor::Vec3 position{};
    };

    std::unordered_map<std::uint32_t, CachedAuthoredLightAttachment> g_authoredLightAttachmentCache;

    bool CachedAuthoredLightWorldPosition(std::uint32_t id, std::uint32_t target, std::uint32_t markerId,
        replay::editor::Vec3& world)
    {
        const auto cached = g_authoredLightAttachmentCache.find(id);
        if (!id || cached == g_authoredLightAttachmentCache.end() || cached->second.target != target ||
            cached->second.markerId != markerId)
            return false;
        world = cached->second.position;
        return std::isfinite(world.x) && std::isfinite(world.y) && std::isfinite(world.z);
    }

    struct EffectiveAuthoredLight
    {
        replay::editor::Vec3 position{};
        float aimPitch = 0.0f;
        float aimYaw = 0.0f;
        float aimRoll = 0.0f;
    };

    bool ResolveAuthoredLight(const replay::editor::AuthoredLight& light, EffectiveAuthoredLight& effective)
    {
        effective.position = light.position;
        effective.aimPitch = light.aimPitch;
        effective.aimYaw = light.aimYaw;
        effective.aimRoll = light.aimRoll;
        if (light.attachedTarget)
        {
            const replay::playback::Puppet* puppet = g_puppets.Find(
                replay::entity::ReplayId::FromPacked(light.attachedTarget));
            if (!puppet || !puppet->clone || !ENTITY::DOES_ENTITY_EXIST(puppet->clone) ||
                puppet->packet.kind != replay::entity::EntityKind::Ped)
            {
                const auto cached = g_authoredLightAttachmentCache.find(light.id);
                if (light.id && cached != g_authoredLightAttachmentCache.end() &&
                    cached->second.target == light.attachedTarget && cached->second.markerId == light.markerId)
                    effective.position = cached->second.position;
                else
                    effective.position = light.position;
            }
            else
            {
                const Entity parent = puppet->clone;
                Vector3 origin = ENTITY::GET_ENTITY_COORDS(parent, TRUE, FALSE);
                if (light.attachedBone)
                {
                    const int bone = AuthoredLightBoneIndex(parent, light.attachedBone);
                    if (bone >= 0)
                        origin = ENTITY::GET_WORLD_POSITION_OF_ENTITY_BONE(parent, bone);
                }
                const Vector3 parentRotation = ENTITY::GET_ENTITY_ROTATION(parent, 2);
                const Vector3 offset = RotateAuthoredLightVector(parentRotation, light.attachType,
                    light.position.x, light.position.y, light.position.z);
                effective.position = {origin.x + offset.x, origin.y + offset.y, origin.z + offset.z};
                if (light.id)
                {
                    if (g_authoredLightAttachmentCache.size() > replay::editor::kMaxAuthoredLights * 2)
                        g_authoredLightAttachmentCache.clear();
                    g_authoredLightAttachmentCache[light.id] = {light.attachedTarget, light.markerId, effective.position};
                }

                constexpr float kDegreesToRadians = 0.01745329251994329577f;
                constexpr float kRadiansToDegrees = 57.295779513082320876f;
                const float pitch = light.aimPitch * kDegreesToRadians;
                const float yaw = light.aimYaw * kDegreesToRadians;
                const float localX = -std::sin(yaw) * std::cos(pitch);
                const float localY = std::cos(yaw) * std::cos(pitch);
                const float localZ = std::sin(pitch);
                const Vector3 axis = RotateAuthoredLightVector(parentRotation, light.attachType, localX, localY, localZ);
                const float dx = axis.x;
                const float dy = axis.y;
                const float dz = axis.z;
                effective.aimPitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * kRadiansToDegrees;
                effective.aimYaw = std::atan2(-dx, dy) * kRadiansToDegrees;
                const float localRightX = std::cos(yaw), localRightY = std::sin(yaw);
                const float localUpX = std::sin(yaw) * std::sin(pitch), localUpY = -std::cos(yaw) * std::sin(pitch);
                const float roll = light.aimRoll * kDegreesToRadians;
                const Vector3 localUp = RotateAuthoredLightVector(parentRotation, light.attachType,
                    localUpX * std::cos(roll) - localRightX * std::sin(roll),
                    localUpY * std::cos(roll) - localRightY * std::sin(roll), std::cos(pitch) * std::cos(roll));
                const float worldPitch = effective.aimPitch * kDegreesToRadians;
                const float worldYaw = effective.aimYaw * kDegreesToRadians;
                const float rightX = std::cos(worldYaw), rightY = std::sin(worldYaw);
                const float upX = std::sin(worldYaw) * std::sin(worldPitch);
                const float upY = -std::cos(worldYaw) * std::sin(worldPitch);
                const float alongRight = localUp.x * rightX + localUp.y * rightY;
                const float alongUp = localUp.x * upX + localUp.y * upY + localUp.z * std::cos(worldPitch);
                effective.aimRoll = std::atan2(-alongRight, alongUp) * kRadiansToDegrees;
            }
        }
        else if (light.id)
            g_authoredLightAttachmentCache.erase(light.id);

        if (light.lookAtTarget)
        {
            const replay::playback::Puppet* target = g_puppets.Find(
                replay::entity::ReplayId::FromPacked(light.lookAtTarget));
            if (target && target->clone && ENTITY::DOES_ENTITY_EXIST(target->clone) &&
                target->packet.kind == replay::entity::EntityKind::Ped)
            {
                Vector3 point = ENTITY::GET_ENTITY_COORDS(target->clone, TRUE, FALSE);
                const int head = ENTITY::GET_ENTITY_BONE_INDEX_BY_NAME(target->clone, const_cast<char*>("SKEL_Head"));
                if (head >= 0)
                    point = ENTITY::GET_WORLD_POSITION_OF_ENTITY_BONE(target->clone, head);
                const float dx = point.x - effective.position.x;
                const float dy = point.y - effective.position.y;
                const float dz = (head >= 0 ? point.z : point.z + replay::camera::kLookAtAimHeight) - effective.position.z;
                constexpr float kRadiansToDegrees = 57.295779513082320876f;
                effective.aimPitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * kRadiansToDegrees;
                effective.aimYaw = std::atan2(-dx, dy) * kRadiansToDegrees;
            }
        }
        return true;
    }
}

void DrawAuthoredLights()
{
    const std::vector<replay::camera::Keyframe>& markers = g_cameraTrack.Keyframes();
    std::uint64_t lightTimeMs = g_transport.PlayheadMs();
    // The camera and light passes both sample this tick's transport playhead.
    // Advancing the lights by another rendered frame makes marker cuts lead the
    // camera by one frame, so sample the exact playhead here.
    std::uint32_t activeMarkerId = 0;
    std::size_t activeMarkerIndex = markers.size();
    std::size_t previousMarkerIndex = markers.size();
    std::size_t nextMarkerIndex = markers.size();
    std::size_t afterNextMarkerIndex = markers.size();
    for (std::size_t n = 0; n < markers.size(); ++n)
    {
        if (markers[n].timeMs > lightTimeMs)
        {
            if (markers[n].markerType != replay::editor::MarkerType::Anchor && nextMarkerIndex == markers.size())
                nextMarkerIndex = n;
            else if (markers[n].markerType != replay::editor::MarkerType::Anchor && afterNextMarkerIndex == markers.size())
                afterNextMarkerIndex = n;
            continue;
        }
        if (markers[n].markerType == replay::editor::MarkerType::Anchor)
            continue;
        previousMarkerIndex = activeMarkerIndex;
        activeMarkerId = markers[n].id;
        activeMarkerIndex = n;
    }
    const replay::camera::Keyframe* currentMarker = activeMarkerIndex < markers.size() ? &markers[activeMarkerIndex] : nullptr;
    const replay::camera::Keyframe* previousMarker = previousMarkerIndex < markers.size() ? &markers[previousMarkerIndex] : nullptr;
    const replay::camera::Keyframe* nextMarker = nextMarkerIndex < markers.size() ? &markers[nextMarkerIndex] : nullptr;
    const replay::camera::Keyframe* afterNextMarker = afterNextMarkerIndex < markers.size() ? &markers[afterNextMarkerIndex] : nullptr;
    const bool showLightGizmos = g_lightEditorOpen && g_lightGizmosVisible;
        if (showLightGizmos)
    {
        // Keep the source locator visible whenever the user asks for gizmos,
        // including while editing markers without Free Cam enabled.
        constexpr Hash kLightLocatorMarker = 0x94FDAE17;
        const auto drawLocator = [&](float x, float y, float z, float radius, float height,
            int red, int green, int blue, int alpha) {
            GRAPHICS::_DRAW_MARKER(kLightLocatorMarker, x, y, z,
                0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                radius, radius, height,
                red, green, blue, alpha,
                FALSE, FALSE, 2, FALSE, nullptr, nullptr, TRUE);
        };
        for (const replay::editor::AuthoredLight& light : g_cameraTrack.AuthoredLights())
        {
            if (light.markerId != 0 && light.markerId != activeMarkerId)
                continue;
            EffectiveAuthoredLight effective{};
            if (!ResolveAuthoredLight(light, effective))
                continue;

            // A black shell and gold core make the 3D beacon readable against
            // both sunlit and dark scenes. Its center marks the actual source.
            drawLocator(effective.position.x, effective.position.y, effective.position.z,
                0.62f, 1.45f, 8, 7, 5, 245);
            drawLocator(effective.position.x, effective.position.y, effective.position.z,
                0.38f, 1.08f, 255, 220, 32, 255);

            float screenX = 0.0f, screenY = 0.0f;
            if (GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD(effective.position.x, effective.position.y,
                    effective.position.z, &screenX, &screenY) &&
                screenX >= 0.0f && screenX <= 1.0f && screenY >= 0.0f && screenY <= 1.0f)
            {
                // Screen-space crosshair stays crisp even when the world marker
                // is small or blends into the scene.
                GRAPHICS::DRAW_RECT(screenX, screenY, 0.024f, 0.0032f, 8, 7, 5, 245, FALSE, FALSE);
                GRAPHICS::DRAW_RECT(screenX, screenY, 0.0032f, 0.024f, 8, 7, 5, 245, FALSE, FALSE);
                GRAPHICS::DRAW_RECT(screenX, screenY, 0.018f, 0.0014f, 255, 220, 32, 255, FALSE, FALSE);
                GRAPHICS::DRAW_RECT(screenX, screenY, 0.0014f, 0.018f, 255, 220, 32, 255, FALSE, FALSE);
                GRAPHICS::DRAW_RECT(screenX, screenY, 0.0044f, 0.0044f, 8, 7, 5, 255, FALSE, FALSE);
                GRAPHICS::DRAW_RECT(screenX, screenY, 0.0020f, 0.0020f, 255, 255, 235, 255, FALSE, FALSE);
            }

            if ((light.coneEnabled || light.reflectorEnabled) && light.beamAngle < 359.0f)
            {
                constexpr float kDegreesToRadians = 0.01745329251994329577f;
                const float pitch = effective.aimPitch * kDegreesToRadians;
                const float yaw = effective.aimYaw * kDegreesToRadians;
                const float directionX = -std::sin(yaw) * std::cos(pitch);
                const float directionY = std::cos(yaw) * std::cos(pitch);
                const float directionZ = std::sin(pitch);
                const float guideLength = std::clamp(light.range, 1.0f, 18.0f);
                for (int step = 1; step <= 4; ++step)
                {
                    const float distance = guideLength * static_cast<float>(step) / 4.0f;
                    const float size = 0.18f - 0.025f * static_cast<float>(step);
                    drawLocator(effective.position.x + directionX * distance,
                        effective.position.y + directionY * distance,
                        effective.position.z + directionZ * distance,
                        size, size * 1.35f, 255, 105, 28, 235);
                }
            }
        }
    }
    const std::vector<replay::editor::AuthoredLight>& authoredLights = g_cameraTrack.AuthoredLights();
    const auto lightAtMarker = [&](std::uint32_t markerId, const replay::editor::AuthoredLight& reference,
        std::size_t ordinal) -> const replay::editor::AuthoredLight* {
        const replay::editor::AuthoredLight* byId = nullptr;
        const replay::editor::AuthoredLight* byName = nullptr;
        const replay::editor::AuthoredLight* byOrdinal = nullptr;
        std::size_t at = 0;
        for (const replay::editor::AuthoredLight& candidate : authoredLights)
        {
            if (candidate.markerId != markerId)
                continue;
            if (candidate.id == reference.id)
                byId = &candidate;
            if (reference.name[0] && std::strncmp(candidate.name, reference.name, sizeof(reference.name)) == 0)
                byName = &candidate;
            if (at++ == ordinal)
                byOrdinal = &candidate;
        }
        return byId ? byId : byName ? byName : byOrdinal;
    };
    replay::editor::Blend segmentBlend = currentMarker ? currentMarker->blend : replay::editor::Blend::None;
    bool segmentCut = false;
    if (currentMarker && nextMarker)
        for (const std::uint32_t cut : g_clip.cuts)
            if (cut > currentMarker->timeMs && cut <= nextMarker->timeMs)
            {
                segmentCut = true;
                break;
            }
    if (!nextMarker || segmentCut)
        segmentBlend = replay::editor::Blend::None;
    const float rawSegmentPhase = currentMarker && nextMarker && nextMarker->timeMs > currentMarker->timeMs
        ? std::clamp(static_cast<float>(static_cast<double>(lightTimeMs - currentMarker->timeMs) /
            static_cast<double>(nextMarker->timeMs - currentMarker->timeMs)), 0.0f, 1.0f) : 0.0f;
    replay::editor::SplineTuning segmentSplineFrom = replay::editor::DefaultSplineTuning();
    replay::editor::SplineTuning segmentSplineTo = segmentSplineFrom;
    if (currentMarker && nextMarker)
    {
        segmentSplineFrom = replay::camera::spline::Effective(currentMarker->spline, g_splineDefaults);
        segmentSplineTo = replay::camera::spline::Effective(nextMarker->spline, g_splineDefaults);
    }
    const auto phaseForBlend = [&](replay::editor::Blend blend) {
        if (!currentMarker || !nextMarker || blend == replay::editor::Blend::None)
            return rawSegmentPhase;
        if (blend == replay::editor::Blend::Spline)
            return static_cast<float>(replay::camera::spline::Ease(rawSegmentPhase,
                segmentSplineFrom.easeIn, segmentSplineFrom.easeOut));
        float phase = replay::camera::ApplyEase(
            replay::camera::ChainEase(previousMarker, *currentMarker, nextMarker, true), rawSegmentPhase);
        if (blend == replay::editor::Blend::Smooth)
            phase = phase * phase * (3.0f - 2.0f * phase);
        return phase;
    };
    const auto authoredLightOrdinal = [&](std::size_t lightIndex, std::uint32_t markerId) {
        std::size_t ordinal = 0;
        for (std::size_t n = 0; n < lightIndex; ++n)
            ordinal += authoredLights[n].markerId == markerId ? 1u : 0u;
        return ordinal;
    };
    const int clockMinutes = CLOCK::GET_CLOCK_HOURS() * 60 + CLOCK::GET_CLOCK_MINUTES();
    const float daylight = std::clamp((clockMinutes - 330.0f) / 90.0f, 0.0f, 1.0f) *
        std::clamp((1110.0f - clockMinutes) / 90.0f, 0.0f, 1.0f);
    // Sunlit scenes wash out the native point light quickly. Increase the
    // contribution through the daytime arc while leaving night lighting at
    // the same authored intensity.
    // The editor's point-light native contributes little against direct sun.
    // Scale only the daylight arc, leaving night scenes at authored intensity.
    const float daylightGain = 1.0f + 59.0f * daylight;
    for (std::size_t n = 0; n < authoredLights.size() && n < replay::editor::kMaxAuthoredLights; ++n)
    {
        const replay::editor::AuthoredLight& light = authoredLights[n];
        replay::editor::AuthoredLight sampled = light;
        EffectiveAuthoredLight effective{};
        bool resolved = false;
        bool active = false;
        if (light.markerId == 0)
        {
            active = light.enabled && light.intensity > 0.0f;
            resolved = ResolveAuthoredLight(sampled, effective);
        }
        else if (light.markerId == activeMarkerId && currentMarker)
        {
            const std::size_t ordinal = authoredLightOrdinal(n, light.markerId);
            // A camera cut always wins. Otherwise authored lights consistently
            // use spline interpolation, independent of older per-light values.
            const replay::editor::Blend markerBlend = segmentBlend == replay::editor::Blend::None
                ? replay::editor::Blend::None
                : replay::editor::Blend::Spline;
            const bool lightBlending = currentMarker && nextMarker && markerBlend != replay::editor::Blend::None;
            const replay::editor::AuthoredLight* nextState = lightBlending
                ? lightAtMarker(nextMarker->id, light, ordinal) : nullptr;
            const replay::editor::SplineTuning lightSplineFrom = replay::camera::spline::Effective(
                light.spline, segmentSplineFrom);
            const replay::editor::SplineTuning lightSplineTo = nextState
                ? replay::camera::spline::Effective(nextState->spline, segmentSplineTo) : segmentSplineTo;
            const float lightPhase = markerBlend == replay::editor::Blend::Spline
                ? static_cast<float>(replay::camera::spline::Ease(rawSegmentPhase,
                    lightSplineFrom.easeIn, lightSplineFrom.easeOut))
                : phaseForBlend(markerBlend);
            if (nextState && lightBlending && nextMarker->timeMs > currentMarker->timeMs)
            {
                const float amount = lightPhase;
                const auto cutBetween = [&](std::uint64_t from, std::uint64_t to) {
                    for (const std::uint32_t cut : g_clip.cuts)
                        if (cut > from && cut <= to)
                            return true;
                    return false;
                };
                const replay::camera::Keyframe* afterMarker = afterNextMarker;
                const auto effectiveBlendAt = [](const replay::editor::AuthoredLight* state,
                    const replay::camera::Keyframe* marker) {
                    if (!state || !marker || marker->blend == replay::editor::Blend::None)
                        return replay::editor::Blend::None;
                    return replay::editor::Blend::Spline;
                };
                const auto previousCandidate = previousMarker
                    ? lightAtMarker(previousMarker->id, light, ordinal) : nullptr;
                const auto afterCandidate = afterMarker
                    ? lightAtMarker(afterMarker->id, *nextState, ordinal) : nullptr;
                const bool hasPreviousLink = previousMarker && previousCandidate &&
                    effectiveBlendAt(previousCandidate, previousMarker) == replay::editor::Blend::Spline &&
                    !previousMarker->legacyCurve && !cutBetween(previousMarker->timeMs, currentMarker->timeMs);
                const bool hasAfterLink = afterMarker && afterCandidate &&
                    effectiveBlendAt(nextState, nextMarker) == replay::editor::Blend::Spline &&
                    !nextMarker->legacyCurve && !cutBetween(nextMarker->timeMs, afterMarker->timeMs);
                const replay::editor::AuthoredLight* previousState = hasPreviousLink ? previousCandidate : nullptr;
                const replay::editor::AuthoredLight* afterNextState = hasAfterLink ? afterCandidate : nullptr;
                const replay::editor::SplineTuning fromTuning = lightSplineFrom;
                const replay::editor::SplineTuning toTuning = lightSplineTo;
                const double hBefore = hasPreviousLink
                    ? static_cast<double>(currentMarker->timeMs - previousMarker->timeMs) / 1000.0 : 0.0;
                const double h0 = static_cast<double>(nextMarker->timeMs - currentMarker->timeMs) / 1000.0;
                const double hAfter = hasAfterLink
                    ? static_cast<double>(afterMarker->timeMs - nextMarker->timeMs) / 1000.0 : 0.0;
                const auto positionDistance = [](const replay::editor::AuthoredLight* a,
                    const replay::editor::AuthoredLight* b) {
                    if (!a || !b)
                        return 0.0;
                    const double x = static_cast<double>(b->position.x) - a->position.x;
                    const double y = static_cast<double>(b->position.y) - a->position.y;
                    const double z = static_cast<double>(b->position.z) - a->position.z;
                    return std::sqrt(x * x + y * y + z * z);
                };
                double positionHBefore = hBefore, positionH0 = h0, positionHAfter = hAfter;
                if (static_cast<replay::editor::SplinePacing>(fromTuning.pacing) != replay::editor::SplinePacing::Natural)
                {
                    const auto spacing = [&](double distance) {
                        return std::pow(std::max(1e-3, distance), static_cast<double>(fromTuning.alpha));
                    };
                    positionHBefore = hasPreviousLink ? spacing(positionDistance(previousState, &light)) : 0.0;
                    positionH0 = spacing(positionDistance(&light, nextState));
                    positionHAfter = hasAfterLink ? spacing(positionDistance(nextState, afterNextState)) : 0.0;
                }
                const auto getScalar = [&](const replay::editor::AuthoredLight* state, float replay::editor::AuthoredLight::*field) {
                    return state ? state->*field : light.*field;
                };
                const auto sample = [&](float p0, float p1, float p2, float p3, std::uint8_t channel,
                    bool angle, bool monotone, float samplePhase, double sampleHBefore, double sampleH0, double sampleHAfter) {
                    if (markerBlend == replay::editor::Blend::Spline)
                        return SampleLightSplineValue(p0, p1, p2, p3, sampleHBefore, sampleH0, sampleHAfter, samplePhase,
                            fromTuning, toTuning, previousState != nullptr, afterNextState != nullptr,
                            channel, angle, monotone);
                    return InterpolateLightValue(p0, p1, p2, p3, samplePhase, markerBlend, angle);
                };
                const auto interpolate = [&](float replay::editor::AuthoredLight::*field,
                    std::uint8_t channel = replay::editor::SplineChannelFov, bool angle = false, bool monotone = false) {
                    return sample(getScalar(previousState, field), light.*field, nextState->*field,
                        getScalar(afterNextState, field), channel, angle, monotone, amount, hBefore, h0, hAfter);
                };
                const auto positionValue = [&](const replay::editor::AuthoredLight* state, int axis) {
                    const replay::editor::AuthoredLight& value = state ? *state : light;
                    return axis == 0 ? value.position.x : axis == 1 ? value.position.y : value.position.z;
                };
                const auto colorValue = [&](const replay::editor::AuthoredLight* state, int channel) {
                    const replay::editor::AuthoredLight& value = state ? *state : light;
                    return static_cast<float>(channel == 0 ? value.colorR : channel == 1 ? value.colorG : value.colorB);
                };
                const auto sampleAxis = [&](int axis, float phase) {
                    return sample(positionValue(previousState, axis), positionValue(&light, axis),
                        positionValue(nextState, axis), positionValue(afterNextState, axis), replay::editor::SplineChannelPosition,
                        false, false, phase, positionHBefore, positionH0, positionHAfter);
                };
                const auto samplePosition = [&](float phase) {
                    return std::array<float, 3>{sampleAxis(0, phase), sampleAxis(1, phase), sampleAxis(2, phase)};
                };
                float positionPhase = amount;
                if (markerBlend == replay::editor::Blend::Spline &&
                    static_cast<replay::editor::SplinePacing>(fromTuning.pacing) != replay::editor::SplinePacing::Natural)
                {
                    constexpr int kArcSteps = 32;
                    std::array<double, kArcSteps + 1> arc{};
                    std::array<std::array<float, 3>, kArcSteps + 1> points{};
                    for (int step = 0; step <= kArcSteps; ++step)
                    {
                        points[step] = samplePosition(static_cast<float>(step) / kArcSteps);
                        if (step > 0)
                        {
                            const double dx = static_cast<double>(points[step][0]) - points[step - 1][0];
                            const double dy = static_cast<double>(points[step][1]) - points[step - 1][1];
                            const double dz = static_cast<double>(points[step][2]) - points[step - 1][2];
                            arc[step] = arc[step - 1] + std::sqrt(dx * dx + dy * dy + dz * dz);
                        }
                    }
                    const double length = arc[kArcSteps];
                    double distance = amount * length;
                    if (static_cast<replay::editor::SplinePacing>(fromTuning.pacing) == replay::editor::SplinePacing::Continuous && length > 1e-9)
                    {
                        const double beforeLength = hasPreviousLink ? positionDistance(previousState, &light) : 0.0;
                        const double afterLength = hasAfterLink ? positionDistance(nextState, afterNextState) : 0.0;
                        const double d0 = length / std::max(1e-6, h0);
                        const double dBefore = hasPreviousLink && hBefore > 1e-9 ? beforeLength / hBefore : 0.0;
                        const double dAfter = hasAfterLink && hAfter > 1e-9 ? afterLength / hAfter : 0.0;
                        const double slopeStart = hasPreviousLink ? replay::camera::spline::MonotoneInner(hBefore, h0, dBefore, d0)
                            : hasAfterLink ? replay::camera::spline::MonotoneEnd(h0, hAfter, d0, dAfter) : d0;
                        const double slopeEnd = hasAfterLink ? replay::camera::spline::MonotoneInner(h0, hAfter, d0, dAfter)
                            : hasPreviousLink ? replay::camera::spline::MonotoneEnd(h0, hBefore, d0, dBefore) : d0;
                        distance = replay::camera::spline::Hermite(0.0, length, slopeStart, slopeEnd, h0, amount * h0);
                    }
                    distance = std::clamp(distance, 0.0, length);
                    if (length > 1e-9)
                    {
                        const auto upper = std::lower_bound(arc.begin(), arc.end(), distance);
                        const int hi = static_cast<int>(upper - arc.begin());
                        const int lo = std::max(0, hi - 1);
                        const double span = arc[hi] - arc[lo];
                        const float fraction = span > 1e-9 ? static_cast<float>((distance - arc[lo]) / span) : 0.0f;
                        positionPhase = (static_cast<float>(lo) + fraction) / kArcSteps;
                    }
                }
                const auto sampleColor = [&](int channel) {
                    return sample(colorValue(previousState, channel), colorValue(&light, channel),
                        colorValue(nextState, channel), colorValue(afterNextState, channel), replay::editor::SplineChannelFov,
                        false, true, amount, hBefore, h0, hAfter);
                };
                const auto enabledIntensity = [](const replay::editor::AuthoredLight* state) {
                    return state && state->enabled ? state->intensity : 0.0f;
                };
                const auto intensityAt = [&](const replay::editor::AuthoredLight* state) {
                    return state ? enabledIntensity(state) : enabledIntensity(&light);
                };
                const float p0Intensity = intensityAt(previousState);
                const float p1Intensity = light.enabled ? light.intensity : 0.0f;
                const float p2Intensity = enabledIntensity(nextState);
                const float p3Intensity = afterNextState ? enabledIntensity(afterNextState) : p2Intensity;
                sampled.intensity = sample(p0Intensity, p1Intensity, p2Intensity, p3Intensity,
                    replay::editor::SplineChannelFov, false, true, amount, hBefore, h0, hAfter);
                sampled.range = interpolate(&replay::editor::AuthoredLight::range, replay::editor::SplineChannelFov, false, true);
                sampled.beamAngle = interpolate(&replay::editor::AuthoredLight::beamAngle, replay::editor::SplineChannelFov, false, true);
                sampled.position.x = sampleAxis(0, positionPhase);
                sampled.position.y = sampleAxis(1, positionPhase);
                sampled.position.z = sampleAxis(2, positionPhase);
                sampled.aimPitch = interpolate(&replay::editor::AuthoredLight::aimPitch, replay::editor::SplineChannelRotation, true);
                sampled.aimYaw = interpolate(&replay::editor::AuthoredLight::aimYaw, replay::editor::SplineChannelRotation, true);
                sampled.aimRoll = interpolate(&replay::editor::AuthoredLight::aimRoll, replay::editor::SplineChannelRotation, true);
                sampled.colorR = static_cast<std::uint32_t>(std::clamp(sampleColor(0), 0.0f, 255.0f));
                sampled.colorG = static_cast<std::uint32_t>(std::clamp(sampleColor(1), 0.0f, 255.0f));
                sampled.colorB = static_cast<std::uint32_t>(std::clamp(sampleColor(2), 0.0f, 255.0f));
                sampled.enabled = (light.enabled || (amount > 0.0f && nextState->enabled)) ? 1u : 0u;
                const bool sameAttachment = light.attachedTarget == nextState->attachedTarget &&
                    light.attachedBone == nextState->attachedBone && light.attachType == nextState->attachType &&
                    light.lookAtTarget == nextState->lookAtTarget;
                if (sameAttachment)
                    resolved = ResolveAuthoredLight(sampled, effective);
                else
                {
                    EffectiveAuthoredLight from{}, to{};
                    const bool haveFrom = ResolveAuthoredLight(light, from);
                    const bool haveTo = ResolveAuthoredLight(*nextState, to);
                    resolved = haveFrom && haveTo;
                    if (resolved)
                    {
                        effective.position = {from.position.x + (to.position.x - from.position.x) * amount,
                            from.position.y + (to.position.y - from.position.y) * amount,
                            from.position.z + (to.position.z - from.position.z) * amount};
                        effective.aimPitch = InterpolateLightValue(from.aimPitch, from.aimPitch, to.aimPitch, to.aimPitch,
                            amount, replay::editor::Blend::Linear, true);
                        effective.aimYaw = InterpolateLightValue(from.aimYaw, from.aimYaw, to.aimYaw, to.aimYaw,
                            amount, replay::editor::Blend::Linear, true);
                        effective.aimRoll = InterpolateLightValue(from.aimRoll, from.aimRoll, to.aimRoll, to.aimRoll,
                            amount, replay::editor::Blend::Linear, true);
                    }
                }
                active = sampled.enabled && sampled.intensity > 0.0f;
            }
            else
            {
                if (lightBlending)
                {
                    sampled.intensity = light.enabled ? light.intensity * (1.0f - lightPhase) : 0.0f;
                    sampled.enabled = sampled.intensity > 0.0f ? 1u : 0u;
                }
                active = sampled.enabled && sampled.intensity > 0.0f;
                resolved = ResolveAuthoredLight(sampled, effective);
            }
        }
        if (!active && !resolved)
            continue;
        if (!resolved)
            resolved = ResolveAuthoredLight(sampled, effective);
        if (!active || !resolved)
            continue;
        const int red = static_cast<int>(std::min(sampled.colorR, 255u));
        const int green = static_cast<int>(std::min(sampled.colorG, 255u));
        const int blue = static_cast<int>(std::min(sampled.colorB, 255u));
        // Legacy reflector clips are rendered as beams; new edits use only the
        // point or beam modes. POINT lights stay omnidirectional.
        const bool directional = sampled.coneEnabled || sampled.reflectorEnabled;
        if (!directional || sampled.beamAngle >= 359.0f)
        {
            GRAPHICS::DRAW_LIGHT_WITH_RANGE(effective.position.x, effective.position.y, effective.position.z,
                red, green, blue, sampled.range, sampled.intensity * daylightGain);
            continue;
        }
        // RDR2 exposes only omnidirectional DRAW_LIGHT_WITH_RANGE. Stack five
        // smaller point lights along an aimed axis, increasing their radius
        // toward the end, to suggest a tapered beam. This remains an
        // approximation and can illuminate objects outside the beam.
        constexpr int kBeamSamples = 5;
        constexpr float kDegreesToRadians = 0.01745329251994329577f;
        const float pitch = effective.aimPitch * kDegreesToRadians;
        const float yaw = effective.aimYaw * kDegreesToRadians;
        const float roll = effective.aimRoll * kDegreesToRadians;
        const float directionX = -std::sin(yaw) * std::cos(pitch);
        const float directionY = std::cos(yaw) * std::cos(pitch);
        const float directionZ = std::sin(pitch);
        const float baseRightX = std::cos(yaw);
        const float baseRightY = std::sin(yaw);
        const float baseUpX = std::sin(yaw) * std::sin(pitch);
        const float baseUpY = -std::cos(yaw) * std::sin(pitch);
        const float baseUpZ = std::cos(pitch);
        const float upX = baseUpX * std::cos(roll) - baseRightX * std::sin(roll);
        const float upY = baseUpY * std::cos(roll) - baseRightY * std::sin(roll);
        const float upZ = baseUpZ * std::cos(roll);
        const float step = sampled.range / static_cast<float>(kBeamSamples);
        const float halfAngle = std::clamp(sampled.beamAngle * 0.5f, 0.5f, 89.5f) * kDegreesToRadians;
        const float tangent = std::tan(halfAngle);
        for (int sample = 0; sample < kBeamSamples; ++sample)
        {
            const float distance = step * (static_cast<float>(sample) + 0.5f);
            const float coneRadius = distance * tangent;
            const float overlap = step * 0.18f;
            const float sampleRange = std::clamp(coneRadius + overlap, std::min(1.0f, sampled.range), sampled.range);
            const float centerX = effective.position.x + directionX * distance;
            const float centerY = effective.position.y + directionY * distance;
            const float centerZ = effective.position.z + directionZ * distance;
            const float rollSpread = std::max(coneRadius * 0.45f, step * 0.12f);
            GRAPHICS::DRAW_LIGHT_WITH_RANGE(centerX, centerY, centerZ,
                red, green, blue, sampleRange, sampled.intensity * daylightGain * 0.20f);
            // A narrow fan gives roll a visible effect: it rotates the beam's
            // asymmetric cross-section around its aim axis. The native lights
            // remain point sources, so this is still an approximation.
            GRAPHICS::DRAW_LIGHT_WITH_RANGE(centerX + upX * rollSpread, centerY + upY * rollSpread,
                centerZ + upZ * rollSpread, red, green, blue, sampleRange, sampled.intensity * daylightGain * 0.10f);
            GRAPHICS::DRAW_LIGHT_WITH_RANGE(centerX - upX * rollSpread, centerY - upY * rollSpread,
                centerZ - upZ * rollSpread, red, green, blue, sampleRange, sampled.intensity * daylightGain * 0.10f);
        }
    }
}
