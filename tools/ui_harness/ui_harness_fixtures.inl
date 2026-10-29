// The UI harness's fixtures. Included at the end of replay_editor_addon.cpp when
// REDEMPTION_UI_HARNESS is defined (ReplayEditorUiTests.vcxproj only): this
// code sees the editor's own state (its anonymous namespace) and plays the
// core, publishing a Snapshot through the shared transport as the core does.
// Every fixture starts from the same clean base (Base) on every frame, so a
// fixture is the same picture whatever ran before it.

#include "../tools/ui_harness/ui_harness.h"
#include "qa_shared.h"

#include <cfloat>
#include <functional>

namespace ui_harness
{
    namespace
    {
        // The core's side of the transport and the QA block.
        Snapshot g_fix{};
        replay::qa::Shared g_qaShared{};
        bool g_offline = false;
        std::uint32_t g_fixtureSequence = 1;
        int g_runScale = replay::editor::uiscale::kDefault; // --scale: the interface scale of every fixture

        struct Fixture
        {
            const char* name;
            const char* about;
            std::function<void()> setup;
        };

        // ---------------- Texts that stress the layout ----------------
        // The longest names the editor takes: a clip renamed in the gallery
        // (its box holds 47 characters, then .rpclip), a project name (47,
        // DrawProjectName), a region, the targets' labels (23).
        const char* const kLongClip = "Saint Denis bank robbery final shootout take 12.rpclip";
        const char* const kLongClip2 = "Blackwater ferry escape with Dutch and boys v3.rpclip";
        const char* const kLongProject = "Van der Linde gang final chapter director cut 2";
        const char* const kLongRegion = "Saint Denis, Bayou Nwa, Lemoyne, United States";
        const char* const kTargetLabels[] = {"Arthur Morgan (player)", "Dutch van der Linde", "Hosea Matthews", "John Marston",
            "Horse: The Count", "Stagecoach driver 1", "Lenny Summers", "Sadie Adler", "Micah Bell", "Javier Escuella",
            "O'Driscoll gunman 14", "Pinkerton agent Milton"};

        void Copy(char* to, std::size_t size, const char* text) { std::snprintf(to, size, "%s", text); }

        std::uint32_t Ms(double seconds) { return static_cast<std::uint32_t>(seconds * 1000.0 + 0.5); }

        // ---------------- The snapshot ----------------
        void BaseSnapshot()
        {
            Snapshot& s = g_fix;
            std::memset(&s, 0, sizeof(s));
            s.flags = SnapshotCoreReady;
            s.speedIndex = 4;
            s.speedPercent = 100;
            s.bufferSeconds = 60;
            s.bufferMB = 128;
            s.compressDays = 7;
            s.compressState = 1;
            s.envClockMinutes = -1;
            s.projectActiveClip = ~0u;
            s.splineDefaults = DefaultSplineTuning();
            for (std::uint32_t n = 0; n < kMaxCameraKeyframesInSnapshot; ++n)
            {
                s.cameraKeyframeShake[n] = DefaultMarkerShake();
                s.cameraKeyframeSpline[n] = DefaultSplineTuning();
            }
            s.clipLoadSequence = g_fixtureSequence;
            s.clipListSequence = g_fixtureSequence; // the clips folder read (a count known)
            s.camera.fov = kMarkerFovDefault;
            s.clipsDiskBytes = 18ull * 1024 * 1024 * 1024 + 123456789ull;
            s.clipsBytes = 41ull * 1024 * 1024 * 1024;
            s.clipTotal = 0;
        }

        void Playing(std::uint32_t durationMs, std::uint32_t playheadMs, bool paused, const char* clip = kLongClip)
        {
            g_fix.flags |= SnapshotPlaying | SnapshotClipFromDisk | SnapshotCameraTrackEnabled;
            if (paused)
                g_fix.flags |= SnapshotPaused;
            g_fix.durationMs = durationMs;
            g_fix.playheadMs = playheadMs;
            Copy(g_fix.currentClip, sizeof(g_fix.currentClip), clip);
            g_fix.envFlags = EnvironmentRecorded | EnvironmentSkyTrack | EnvironmentEditable;
        }

        CameraKeyframe& AddKeyframe(std::uint32_t timeMs)
        {
            Snapshot& s = g_fix;
            CameraKeyframe& k = s.cameraKeyframes[s.cameraKeyframeCount];
            k = CameraKeyframe{};
            k.id = s.cameraKeyframeCount + 1;
            k.timeMs = timeMs;
            k.camera.fov = kMarkerFovDefault;
            k.cameraMode = CameraMode::Free;
            k.markerType = MarkerType::Edit;
            k.blend = Blend::Smooth;
            k.shakeIntensity = 5;
            k.shakeSpeedIndex = static_cast<std::uint8_t>(kMarkerSpeedDefaultIndex);
            k.speedIndex = static_cast<std::uint8_t>(kMarkerSpeedDefaultIndex);
            k.presetDistanceScale = 0.5f;
            k.dofIntensity = 50.0f;
            k.focusDistance = 5.0f;
            ++s.cameraKeyframeCount;
            s.cameraKeyframeTotal = s.cameraKeyframeCount;
            return k;
        }

        // `count` markers spread over the clip (the first at 0, the last at its end).
        void Keyframes(std::uint32_t count)
        {
            const std::uint64_t duration = std::max<std::uint64_t>(g_fix.durationMs, 1000);
            for (std::uint32_t n = 0; n < count && n < kMaxCameraKeyframesInSnapshot; ++n)
                AddKeyframe(static_cast<std::uint32_t>(count > 1 ? duration * n / (count - 1) : 0));
        }

        void Targets(std::uint32_t count)
        {
            Snapshot& s = g_fix;
            s.targetCount = std::min<std::uint32_t>(count, kMaxTargetsInSnapshot);
            for (std::uint32_t n = 0; n < s.targetCount; ++n)
            {
                s.targets[n].id = 0x01000000u + n + 1;
                s.targets[n].kind = n == 4 ? 2u : 1u;
                Copy(s.targets[n].label, sizeof(s.targets[n].label), kTargetLabels[n % (sizeof(kTargetLabels) / sizeof(kTargetLabels[0]))]);
                s.targetFamily[n] = n == 4 ? 2u : 1u;
                s.targetBoneMask[n] = 0xFFFFFFFFu;
                s.targetBoneMaskHigh[n] = 0xFFFFFFFFu;
                s.targetBoneCount[n] = 120;
            }
        }

        void SpansAndCuts()
        {
            Snapshot& s = g_fix;
            const std::uint32_t d = static_cast<std::uint32_t>(s.durationMs);
            const std::uint32_t kinds[] = {0, 1, 2, 0, 1, 2};
            s.spanCount = 6;
            s.spanTotal = 6;
            for (std::uint32_t n = 0; n < 6; ++n)
            {
                s.spans[n].startMs = d / 7 * (n + 1);
                s.spans[n].endMs = s.spans[n].startMs + d / 20;
                s.spans[n].kind = kinds[n];
            }
            s.cutCount = 10;
            for (std::uint32_t n = 0; n < 10; ++n)
                s.cuts[n] = d / 11 * (n + 1);
        }

        // The shake in the markers: two markers in three shake (a profile or
        // the noise with its numbers; Blend on some, the other flags on others).
        void MarkerShakes()
        {
            Snapshot& s = g_fix;
            for (std::uint32_t n = 0; n < s.cameraKeyframeCount; ++n)
            {
                MarkerShake& m = s.cameraKeyframeShake[n];
                m = DefaultMarkerShake();
                if (n % 3 == 2)
                    continue;
                m.on = 1;
                m.profile = static_cast<std::uint8_t>(n % 3 == 0 ? 1 + n % 10 : 0);
                m.gain = (n % 10 + 2) / 6.0f * ((n % 10 + 2) / 6.0f);
                m.transition = static_cast<std::uint8_t>(n % 2);
                m.blendMask = n % 4 == 1 ? ShakeBlendGain : ShakeBlendAll;
                m.flags = static_cast<std::uint8_t>(n % 5 == 0 ? ShakeFlagRealTime | ShakeFlagFadeOut : 0);
            }
        }

        ClipInfo& AddClip(const char* name, double seconds)
        {
            Snapshot& s = g_fix;
            ClipInfo& c = s.clips[s.clipCount];
            c = ClipInfo{};
            Copy(c.name, sizeof(c.name), name);
            c.durationMs = Ms(seconds);
            c.bytes = static_cast<std::uint64_t>(seconds * 2.1 * 1024 * 1024);
            c.formatVersion = 42;
            c.loadable = 1;
            c.hasClock = 1;
            c.clockStartS = 17 * 3600 + 3 * 60;
            Copy(c.weather, sizeof(c.weather), "Thunderstorm");
            Copy(c.region, sizeof(c.region), "Valentine, Heartlands, New Hanover");
            Copy(c.character, sizeof(c.character), "Arthur");
            c.chapter = 2;
            c.camp = 1;
            c.listIndex = s.clipCount;
            ++s.clipCount;
            return c;
        }

        // A gallery of `total` clips (the window holds the first 64).
        void Gallery(std::uint32_t total)
        {
            Snapshot& s = g_fix;
            static const char* const names[] = {"clip-20261024-170346.rpclip", "042-SaintDenis-p2.rpclip", nullptr, "137-StrawberryBigValley.rpclip",
                nullptr, "clip-20261023-091502.rpclip", "211-BlackwaterNewAustin-p12.rpclip", "Horse race.rpclip"};
            for (std::uint32_t n = 0; n < total && n < kMaxClipsInSnapshot; ++n)
            {
                const char* name = names[n % 8] ? names[n % 8] : (n % 16 == 2 ? kLongClip : kLongClip2);
                char unique[kClipNameCapacity];
                if (n >= 8 && names[n % 8])
                {
                    // Unique numbered names beyond the first eight.
                    std::snprintf(unique, sizeof(unique), "%03u-RhodesScarlettMeadows.rpclip", n);
                    name = unique;
                }
                ClipInfo& c = AddClip(name, n == 3 ? 3905.0 : 12.0 + n * 37.5);
                c.compressed = (n % 3 == 1) ? 1u : 0u;
                c.clipBytes = c.compressed ? c.bytes * 3 : 0;
                c.loadable = (n == 6) ? 0u : 1u;
                if (n == 2)
                    Copy(c.region, sizeof(c.region), kLongRegion);
            }
            s.clipTotal = total;
            s.clipListSequence = g_fixtureSequence;
        }

        void ProjectClips(std::uint32_t count, bool active)
        {
            Snapshot& s = g_fix;
            s.projectClipCount = std::min<std::uint32_t>(count, kMaxProjectClipsInSnapshot);
            std::uint64_t total = 0;
            for (std::uint32_t n = 0; n < s.projectClipCount; ++n)
            {
                ProjectClipInfo& c = s.projectClips[n];
                c = ProjectClipInfo{};
                Copy(c.file, sizeof(c.file), n % 3 == 0 ? kLongClip : n % 3 == 1 ? "042-SaintDenis-p2.rpclip" : "clip-20261024-170346.rpclip");
                c.rawMs = Ms(30.0 + (n % 5) * 20.0);
                c.inMs = Ms(2.0);
                c.outMs = c.rawMs - Ms(3.0);
                c.flags = (n == 1 ? ProjectClipMissing : 0u) | (active && n == 0 ? ProjectClipActive : 0u);
                total += c.outMs - c.inMs;
            }
            s.projectTotalMs = total;
            s.projectFlags |= ProjectOpenFlag;
            Copy(s.projectName, sizeof(s.projectName), kLongProject);
            Copy(s.projectFile, sizeof(s.projectFile), "project-20261024-170346.rpproj");
            if (active)
                s.projectActiveClip = 0;
        }

        void ProjectList(std::uint32_t count)
        {
            Snapshot& s = g_fix;
            s.projectListCount = std::min<std::uint32_t>(count, kMaxProjectsInSnapshot);
            for (std::uint32_t n = 0; n < s.projectListCount; ++n)
            {
                ProjectInfo& p = s.projects[n];
                p = ProjectInfo{};
                std::snprintf(p.file, sizeof(p.file), "project-20261024-1703%02u-a-very-long-project-file-name.rpproj", n % 60);
                if (n % 4 != 3)
                    Copy(p.name, sizeof(p.name), n % 2 ? kLongProject : "Valentine saloon brawl");
                p.totalMs = Ms(95.0 + n * 61.0);
                p.clipCount = 3 + n * 7;
                p.readable = n == 2 ? 0u : 1u;
            }
            s.projectListSequence = g_fixtureSequence;
        }

        // A short list with plain names (the rename screens): Chase, Rhodes,
        // Night camp, Blackwater; Chase selected.
        void RenameProjectList()
        {
            ProjectList(4);
            const char* const names[] = {"Chase", "Rhodes", "Night camp", "Blackwater"};
            for (int n = 0; n < 4; ++n)
            {
                Copy(g_fix.projects[n].name, sizeof(g_fix.projects[n].name), names[n]);
                std::snprintf(g_fix.projects[n].file, sizeof(g_fix.projects[n].file), "%s.rpproj", names[n]);
                g_fix.projects[n].readable = 1u;
            }
            g_projectsIndex = 1;
        }

        // ---------------- The editor's state ----------------
        void BaseEditor()
        {
            g_screen = Screen::Closed;
            g_uiScalePercent = g_runScale; // as InterfaceScale read from the ini
            g_uiScaleRead = true;
            g_helpOpen = false;
            g_helpSwallow = false;
            g_hide = replay::editor::uiqa::HideState{};
            g_homeIndex = 0; // Recorded Clips, the first row
            g_settingsIndex = 0;
            g_settingsScroll = 0.0f;
            g_settingsScrolledTo = -1;
            g_galleryIndex = 0;
            g_gallerySort = 0;
            g_galleryPicking = false;
            g_compressingClip.clear();
            g_projectsIndex = 0;
            g_projectsSelectFile.clear();
            g_projectIndex = 0;
            g_projectMessageUntil = 0;
            g_projectMessageShown.clear();
            g_projectOpenAsked = 0;
            g_editAfterOpen = -1;
            g_dragBlock = -1;
            g_projectScrubbing = false;
            g_editingProjectClip = false;
            g_selectedKeyframeId = 0;
            g_seenSelection = 0; // every frame a fresh selection: the 700 ms close never runs on a slow frame
            g_draggingKeyframeId = 0;
            g_draggingPlayhead = false;
            g_selectAdded = false;
            g_menuMarker = 0;
            g_markerMenu = MarkerMenu::Root;
            g_menuFocus = 0;
            g_pendingShake.Clear();
            g_freeCamMenuOpen = false;
            g_splineDefaultsOpen = false;
            g_rowTyping = RowTyping{};
            g_timelineZoom = 1.0;
            g_editMessage = "";
            g_editMessageUntil = 0;
            g_toastUntil = 0;
            g_recBrief = replay::recindicator::BriefTimer{};
            g_recEnv = replay::recindicator::Environment{};
            g_loadName.clear();
            g_openingAsked = false;
            g_openingSavesRecording = false;
            g_worldNoticeUntil = 0;
            g_worldNoticeArmed = false;
            g_qaNoticeUntil = 0;
            g_qaNoticeText[0] = '\0';
            g_crashNotice = CrashNoticeState{};
            g_crashNotice.checked = true;
            g_storageNotice = StorageNoticeState{};
            g_storageNotice.checked = true;
            g_renameFocus = false;
            g_renameError = "";
            g_harnessMouse = HarnessMouse{}; // no mouse: no cursor (the cursor/ fixtures set one)
            g_menuPointer = replay::editor::uiqa::MenuPointer{}; // a resting mouse unless the fixture moves it
            g_projectNameFocus = false;
            g_projectNameError = "";
            exportui::g_index = 0;
            exportui::g_preset = 0;
            exportui::g_rate = 4;
            exportui::g_range = 1;
            exportui::g_back = Screen::Edit;
            exportui::g_samples = 0;
            exportui::g_queue.clear();
            exportui::g_note.clear();
            exportui::g_noteUntil = 0;
            exportui::g_configLoaded = true;
            exportui::g_lutPath.clear();
            qa_shots::g_state.noteOpen = false;
            qa_shots::g_state.noteFocus = false;
            g_qaShared.marksEnabled = 0;
            g_qaShared.pendingMarks = 0;
            g_offline = false;
            exportstub::Get() = exportstub::State{};
            render_export::ExportController::Get().LoadConfig();
        }

        ULONGLONG Later() { return GetTickCount64() + 600000; }

        // The clip editor on a playing clip with markers, spans and targets.
        void EditBase(std::uint32_t markers = 6)
        {
            Playing(Ms(95.4), Ms(38.2), true);
            Keyframes(markers);
            Targets(12);
            SpansAndCuts();
            MarkerShakes();
            g_screen = Screen::Edit;
        }

        // Marker `index` selected with its menu `menu` (the playhead on it).
        void SelectMarker(std::uint32_t index, MarkerMenu menu, int focus = 0)
        {
            CameraKeyframe& k = g_fix.cameraKeyframes[index];
            k.lookAtTarget = g_fix.targets[0].id;
            k.attachTarget = g_fix.targets[4].id;
            k.focusTarget = g_fix.targets[11].id;
            k.cameraMode = CameraMode::Free;
            k.shake = ShakeType::GroundVibration;
            // The marker's shake, every field set.
            MarkerShake& m = g_fix.cameraKeyframeShake[index];
            m = DefaultMarkerShake();
            m.on = 1;
            m.profile = static_cast<std::uint8_t>(ShakeType::GroundVibration);
            m.gain = 3.36f;
            m.hz = 27.5f;
            m.transition = 1;
            m.blendMask = ShakeBlendGain;
            m.flags = ShakeFlagRealTime | ShakeFlagFadeOut | ShakeFlagRestart;
            m.seed = 65535;
            m.rate = 2.0f;
            m.speedRef = 100.0f;
            k.dof = DofMode::Custom;
            k.focus = FocusMode::Target;
            k.filter = 3;
            k.filterIntensity = 80;
            k.transition = 1;
            k.transitionMs = 1500;
            g_fix.playheadMs = k.timeMs;
            g_selectedKeyframeId = k.id;
            g_menuMarker = k.id;
            g_markerMenu = menu;
            g_menuFocus = focus;
        }

        void KeyBarStress()
        {
            g_qaShared.marksEnabled = 1;
            g_qaShared.pendingMarks = 12;
            g_fix.flags |= SnapshotFreeCamEnabled | SnapshotCanUndo;
            g_fix.freeCamMotion = 1;
            g_fix.freeCamTarget = 0x01000001u;
        }

        // The longest keys the [Keys] table takes: two keys each, with Ctrl and Shift.
        void LongKeys(bool on)
        {
            if (!on)
            {
                replay::keys::Current() = replay::keys::DefaultTable();
                return;
            }
            static const replay::keys::Table table = replay::keys::Load([](const char* key, bool& present) {
                present = true;
                static int n = 0;
                const int fkey = 13 + (n % 12);
                ++n;
                char text[64];
                std::snprintf(text, sizeof(text), "Ctrl+Shift+F%d, Ctrl+Num%d", fkey, (n % 9) + 1);
                (void)key;
                return std::string(text);
            }).table;
            replay::keys::Current() = table;
        }

        // The default table with a long report key (and mark key): the notices'
        // widest text, the key bar left as it is.
        replay::keys::Table LongReportKey()
        {
            replay::keys::Table table = replay::keys::DefaultTable();
            table.Of(Action::Report)[0] = {replay::keys::vk::F(12), true, true};
            table.Of(Action::MarkProblem)[0] = {replay::keys::vk::F(11), true, true};
            return table;
        }

        // A player's remapped [Keys]: every text naming one of these actions shows
        // the key set here, in any language.
        void RemappedKeys()
        {
            static const replay::keys::Table table = replay::keys::Load([](const char* key, bool& present) {
                static const char* const kIni[][2] = {{"Report", "Ctrl+F11"}, {"MarkProblem", "Ctrl+M"}, {"Record", "J"}, {"SaveBuffer", "Shift+J"},
                    {"Editor", "Ctrl+F2"}, {"Stop", "Num7"}, {"HideUi", "Ctrl+H"}};
                for (const auto& entry : kIni)
                {
                    if (std::strcmp(entry[0], key) == 0)
                    {
                        present = true;
                        return std::string(entry[1]);
                    }
                }
                present = false;
                return std::string();
            }).table;
            replay::keys::Current() = table;
        }

        std::vector<Fixture> MakeFixtures()
        {
            std::vector<Fixture> f;
            // ---------------- Home ----------------
            f.push_back({"home/idle", "home screen, nothing open, Recorded Clips selected", [] { g_screen = Screen::Home; }});
            for (int item = 0; item < 7; ++item)
            {
                static const char* const names[] = {"home/buffer-item-0", "home/buffer-item-1", "home/buffer-item-2", "home/buffer-item-3",
                    "home/buffer-item-4", "home/buffer-item-5", "home/buffer-item-6"};
                f.push_back({names[item], "home with the replay buffer recording a manual mark: each item's description", [item] {
                    g_screen = Screen::Home;
                    g_fix.flags |= SnapshotBufferRunning | SnapshotBufferManual | SnapshotRecording;
                    g_fix.bufferEnabled = 1;
                    g_fix.bufferHeldMs = Ms(41.0);
                    g_fix.bufferSeconds = 300;
                    g_fix.bufferMB = 1024;
                    g_fix.recordingMs = Ms(754.0);
                    g_fix.clipTotal = 312;
                    g_fix.projectListCount = 64;
                    g_homeIndex = item;
                }});
            }
            f.push_back({"home/clips-reading", "home as the editor opens: the clips folder still being read (no count yet)", [] {
                g_screen = Screen::Home;
                g_fix.clipListSequence = 0;
                g_fix.clipTotal = 0;
                g_fix.flags |= SnapshotClipListLoading;
            }});
            f.push_back({"home/clips-known", "home once the clips folder is read: 813 clips and their size", [] {
                g_screen = Screen::Home;
                Gallery(64);
                g_fix.clipTotal = 813;
            }});
            f.push_back({"home/project-open", "home with a long-named project open (Edit Project, its detail line)", [] {
                g_screen = Screen::Home;
                ProjectClips(120, false);
                g_fix.projectFlags |= ProjectDirty;
                g_homeIndex = 1; // Recorded Clips, then Edit Project
            }});
            f.push_back({"home/project-close", "home with a project open, Close Project selected", [] {
                g_screen = Screen::Home;
                ProjectClips(3, false);
                g_homeIndex = 5; // Clips, Edit Project, Projects, Record, Replay Settings, Close Project
            }});
            // The rows' boxes (RowBox): Save Replay Buffer only when it can
            // save, Back to Game a plain line under the boxes.
            f.push_back({"home/buffer-off", "home with the replay buffer off: no Save Replay Buffer, Replay Settings selected (buffer off)", [] {
                g_screen = Screen::Home;
                g_fix.bufferEnabled = 0;
                g_homeIndex = 4; // Recorded Clips, New Project, Projects, Record, Replay Settings
            }});
            f.push_back({"home/buffer-empty", "home with the replay buffer just started (nothing held): no Save Replay Buffer yet", [] {
                g_screen = Screen::Home;
                g_fix.flags |= SnapshotBufferRunning;
                g_fix.bufferEnabled = 1;
                g_fix.bufferHeldMs = 0;
                g_homeIndex = 4;
            }});
            f.push_back({"home/hover", "home: Recorded Clips selected (red ink), the mouse resting over Projects: no second outline", [] {
                g_screen = Screen::Home;
                g_fix.flags |= SnapshotBufferRunning;
                g_fix.bufferEnabled = 1;
                g_fix.bufferHeldMs = Ms(41.0);
                g_homeIndex = 0;
                // Projects is the third row (DrawHome: the rows from the title + 126, 50 apart).
                const float itemsTop = std::max(60.0f, g_vh * 0.16f) + 126.0f;
                g_harnessMouse = HarnessMouse{true, 300.0f, itemsTop + 2 * 50.0f + 22.0f};
            }});
            f.push_back({"home/back-selected", "home with Back to Game selected: the plain line under the boxes, its red stroke", [] {
                g_screen = Screen::Home;
                g_homeIndex = 5;
            }});
            f.push_back({"home/back-hover", "home with the mouse resting over Back to Game, Recorded Clips selected (one highlight)", [] {
                g_screen = Screen::Home;
                g_homeIndex = 0;
                // Back to Game: 16 units under the fifth box (the buffer off: five boxed rows).
                const float itemsTop = std::max(60.0f, g_vh * 0.16f) + 126.0f;
                g_harnessMouse = HarnessMouse{true, 120.0f, itemsTop + 5 * 50.0f + 28.0f};
            }});
            f.push_back({"home/hover-moved", "home: the mouse just moved onto Projects, which takes the selection (RDR2's menus)", [] {
                g_screen = Screen::Home;
                g_homeIndex = 0;
                const float itemsTop = std::max(60.0f, g_vh * 0.16f) + 126.0f;
                g_harnessMouse = HarnessMouse{true, 300.0f, itemsTop + 2 * 50.0f + 22.0f, false, true};
            }});
            // ---------------- Replay Settings ----------------
            for (int row = 0; row < 12; ++row)
            {
                static const char* const names[] = {"settings/row-0", "settings/row-1", "settings/row-2", "settings/row-3", "settings/row-4",
                    "settings/row-5", "settings/row-6", "settings/row-7", "settings/row-8", "settings/row-9", "settings/row-10", "settings/row-11"};
                f.push_back({names[row], "Replay Settings in its four groups (Recording, Playback, Storage, Interface), each row selected (its help; the disk use on rows 4 to 6)", [row] {
                    g_screen = Screen::Settings;
                    g_fix.bufferEnabled = 1;
                    g_fix.bufferSeconds = 300;
                    g_fix.bufferMB = 1024;
                    g_fix.compressEnabled = 1;
                    g_fix.compressDays = 45;
                    g_fix.compressState = 2;
                    g_fix.compressDone = 117;
                    g_fix.compressTotal = 1204;
                    g_fix.clipTotal = 1312;
                    g_fix.compressedClips = 1088;
                    g_fix.flags |= SnapshotReplayDeadEye | SnapshotLockCutsceneCamera | SnapshotFreezeOnPause;
                    g_settingsIndex = row;
                }});
            }
            // The recording indicator's row (src/rec_indicator.h), each value.
            for (std::uint32_t mode = 0; mode < replay::recindicator::kModeCount; ++mode)
            {
                static const char* const names[] = {"settings/rec-indicator-normal", "settings/rec-indicator-brief", "settings/rec-indicator-hidden",
                    "settings/rec-indicator-off"};
                f.push_back({names[mode], "Replay Settings, the Recording indicator row with each value (its help)", [mode] {
                    g_screen = Screen::Settings;
                    g_fix.recordingIndicator = mode;
                    g_settingsIndex = 10;
                }});
            }
            f.push_back({"settings/rec-indicator-fallback", "Replay Settings, Hidden from capture shown as Brief (exclusive fullscreen): the note", [] {
                g_screen = Screen::Settings;
                g_fix.recordingIndicator = static_cast<std::uint32_t>(replay::recindicator::Mode::Hidden);
                g_recEnv.exclusiveFullscreen = true;
                g_settingsIndex = 10;
            }});
            // The Interface scale row (src/ui_scale.h) at each step: the
            // settings drawn at that scale, the row selected (its help).
            for (int step = 0; step < replay::editor::uiscale::kStepCount; ++step)
            {
                static const char* const names[] = {"settings/interface-scale-0", "settings/interface-scale-1", "settings/interface-scale-2",
                    "settings/interface-scale-3", "settings/interface-scale-4", "settings/interface-scale-5", "settings/interface-scale-6",
                    "settings/interface-scale-7"};
                static_assert(replay::editor::uiscale::kStepCount <= 8, "a name per step");
                f.push_back({names[step], "Replay Settings at each Interface scale step, the row selected (its help)", [step] {
                    g_screen = Screen::Settings;
                    g_uiScalePercent = replay::editor::uiscale::kSteps[step];
                    g_settingsIndex = 11;
                }});
            }
            f.push_back({"settings/clips-reading", "Replay Settings, Compress clips selected while the clips folder is read", [] {
                g_screen = Screen::Settings;
                g_fix.clipListSequence = 0;
                g_fix.flags |= SnapshotClipListLoading;
                g_settingsIndex = 4;
            }});
            // ---------------- Gallery ----------------
            f.push_back({"gallery/empty", "no clips yet", [] { g_screen = Screen::Gallery; }});
            f.push_back({"gallery/loading", "the folder being read, an empty list", [] {
                g_screen = Screen::Gallery;
                g_fix.flags |= SnapshotClipListLoading;
            }});
            f.push_back({"gallery/many-long", "300 clips, long names, the long one selected and open, zip badges, a region that wraps", [] {
                g_screen = Screen::Gallery;
                Gallery(300);
                Playing(Ms(95.4), Ms(12.0), true, kLongClip);
                g_galleryIndex = 2;
            }});
            f.push_back({"gallery/hover-moved", "300 clips (a list that scrolls): the mouse moved over a row keeps the selection, grey ink on it", [] {
                g_screen = Screen::Gallery;
                Gallery(300);
                g_galleryIndex = 2;
                // The fifth row (DrawGallery: rows from 180, 40 apart, on the left stage).
                g_harnessMouse = HarnessMouse{true, 300.0f, (g_vh - 720.0f) * 0.5f + 180.0f + 4 * 40.0f + 18.0f, false, true};
            }});
            f.push_back({"gallery/hour-clip", "a clip over an hour (the duration column), compressed, loading list", [] {
                g_screen = Screen::Gallery;
                Gallery(64);
                g_fix.flags |= SnapshotClipListLoading;
                g_galleryIndex = 3;
            }});
            f.push_back({"gallery/old-clip", "a clip of another version selected (old, cannot open)", [] {
                g_screen = Screen::Gallery;
                Gallery(12);
                g_galleryIndex = 6;
            }});
            f.push_back({"gallery/picking", "adding a clip to the project (another title, the Enter label)", [] {
                g_screen = Screen::Gallery;
                Gallery(40);
                ProjectClips(4, false);
                g_galleryPicking = true;
                g_galleryIndex = 9;
            }});
            // ---------------- Alert ----------------
            f.push_back({"alert/save-failed", "the longest alert body (a clip that could not be saved)", [] {
                ShowSaveFailedAlert(Screen::Home); // the editor's own words
            }});
            f.push_back({"alert/delete-open-clip", "deleting the open, edited clip with the longest name (its picture above)", [] {
                Playing(Ms(95.4), Ms(12.0), true, kLongClip);
                g_fix.flags |= SnapshotClipEdited;
                ShowDeleteClipAlert(kLongClip, true);
            }});
            f.push_back({"alert/delete-project", "deleting the open project with the longest name", [] {
                ProjectClips(3, false);
                ShowDeleteProjectAlert("project.rpproj", kLongProject, true);
            }});
            // ---------------- Rename and the project's name ----------------
            f.push_back({"rename/long-error", "renaming the longest clip, the error line under the box", [] {
                g_screen = Screen::Rename;
                g_renameFrom = kLongClip;
                Copy(g_renameText, sizeof(g_renameText), "Saint Denis bank robbery final shootout take 12");
                g_renameError = NameCharsError();
            }});
            f.push_back({"projectname/new", "a new project's name box", [] {
                g_screen = Screen::ProjectName;
                g_projectNameNew = true;
                g_projectNameFile.clear();
                Copy(g_projectNameText, sizeof(g_projectNameText), "Project 12");
            }});
            f.push_back({"projectname/rename-error", "renaming a project with the longest file and name, the error line", [] {
                g_screen = Screen::ProjectName;
                g_projectNameNew = false;
                g_projectNameFile = "project-20261024-170346-a-very-long-project-file-name.rpproj";
                Copy(g_projectNameText, sizeof(g_projectNameText), kLongProject);
                g_projectNameError = NameCharsError();
            }});
            f.push_back({"projectname/rename-saved-as", "renaming a project to a name with characters a file cannot hold: the file it gives", [] {
                RenameProjectList();
                ShowProjectName(false, "Chase.rpproj", "Chase: part 2?", Screen::Projects);
            }});
            f.push_back({"projectname/rename-taken", "renaming a project to another project's name: refused at once", [] {
                RenameProjectList();
                ShowProjectName(false, "Chase.rpproj", "rhodes", Screen::Projects);
            }});
            // ---------------- Projects ----------------
            f.push_back({"projects/rename-list", "a short list, Chase selected: its Rename and Delete buttons under the details", [] {
                g_screen = Screen::Projects;
                RenameProjectList();
            }});
            f.push_back({"projects/new-row", "the project files, New Project selected", [] {
                g_screen = Screen::Projects;
                ProjectList(64);
            }});
            f.push_back({"projects/long-names", "64 projects, the long-named open one selected", [] {
                g_screen = Screen::Projects;
                ProjectList(64);
                ProjectClips(3, false);
                Copy(g_fix.projectFile, sizeof(g_fix.projectFile), g_fix.projects[0].file);
                g_projectsIndex = 2;
            }});
            f.push_back({"projects/unreadable", "a project of another version selected", [] {
                g_screen = Screen::Projects;
                ProjectList(8);
                g_projectsIndex = 3;
            }});
            // ---------------- The project's timeline ----------------
            f.push_back({"project/waiting", "a project asked for, not open yet", [] {
                g_screen = Screen::Project;
                g_projectOpenAsked = GetTickCount64();
            }});
            f.push_back({"project/empty", "an open project without clips", [] {
                g_screen = Screen::Project;
                g_fix.projectFlags |= ProjectOpenFlag;
                Copy(g_fix.projectName, sizeof(g_fix.projectName), kLongProject);
            }});
            f.push_back({"project/three-clips", "three clips, long names, a missing one, a message", [] {
                g_screen = Screen::Project;
                ProjectClips(3, false);
                g_fix.projectFlags |= ProjectDirty;
                g_projectMessageShown = TR("project.message.play_first", "Play or open the selected clip first; the cut and the trim go where its playhead is.");
                g_projectMessageUntil = Later();
                g_projectMessageSeen = g_fix.projectMessageSequence;
            }});
            f.push_back({"project/many-clips", "120 clips (thin blocks), playing, the first active", [] {
                g_screen = Screen::Project;
                ProjectClips(120, true);
                Playing(Ms(48.0), Ms(20.0), false, kLongClip);
                g_fix.projectFlags |= ProjectPreviewing;
                g_fix.projectPlayheadMs = Ms(20.0);
                g_projectIndex = 0;
            }});
            f.push_back({"project/save-failed", "the save failed line", [] {
                g_screen = Screen::Project;
                ProjectClips(5, false);
                g_fix.projectFlags |= ProjectSaveFailed;
                g_projectIndex = 4;
            }});
            // ---------------- The clip editor ----------------
            f.push_back({"edit/home-panel", "the edit panel without a marker: camera, environment, buttons", [] { EditBase(); }});
            f.push_back({"edit/home-panel-live", "the panel with live weather and clock", [] {
                EditBase();
                g_fix.envClockMinutes = replay::envoverride::kClockLive;
                g_fix.envWeather = replay::envoverride::kWeatherLive;
                g_fix.envBlendS = 30;
            }});
            f.push_back({"edit/status-long", "the status line: long clip name, zoom, a long message, unsaved, the spans legend", [] {
                EditBase();
                g_fix.flags |= SnapshotClipEdited;
                g_timelineZoom = 4.5;
                g_editMessage = TR("edit.message.photo", "Photo saved (ReShade screenshot folder)"); // the longest message
                g_editMessageUntil = Later();
            }});
            f.push_back({"edit/project-clip", "a project's clip in the editor (its status names the project)", [] {
                EditBase();
                ProjectClips(120, true);
                g_fix.projectFlags |= ProjectEditingClip;
                g_fix.flags |= SnapshotClipEdited;
            }});
            {
                struct MenuCase
                {
                    const char* name;
                    MarkerMenu menu;
                    std::uint32_t marker;
                };
                static const MenuCase menus[] = {{"edit/marker-root", MarkerMenu::Root, 2}, {"edit/marker-start", MarkerMenu::Root, 0},
                    {"edit/marker-end", MarkerMenu::Root, 5}, {"edit/marker-camera", MarkerMenu::Camera, 2}, {"edit/marker-dof", MarkerMenu::Dof, 2},
                    {"edit/marker-effects", MarkerMenu::Effects, 2}, {"edit/marker-position", MarkerMenu::Position, 2},
                    {"edit/marker-tools", MarkerMenu::Tools, 2}, {"edit/marker-spline", MarkerMenu::Spline, 2},
                    {"edit/marker-spline-defaults", MarkerMenu::SplineDefaults, 2},
                    {"edit/marker-shake", MarkerMenu::Shake, 2}, {"edit/marker-shake-advanced", MarkerMenu::ShakeAdvanced, 2}};
                for (const MenuCase& m : menus)
                    f.push_back({m.name, "a marker's menu (targets with long labels, every option set)", [m] {
                        EditBase();
                        SelectMarker(m.marker, m.menu);
                    }});
            }
            f.push_back({"edit/playhead-on-marker", "a key marker selected under the playhead, its shake on, in the red part (the crown over its ring)", [] {
                EditBase();
                SelectMarker(3, MarkerMenu::Root);
            }});
            f.push_back({"edit/playhead-near-marker", "the playhead a few units before the selected marker (the crown easing up)", [] {
                EditBase();
                SelectMarker(3, MarkerMenu::Root);
                g_fix.playheadMs = g_fix.cameraKeyframes[3].timeMs - Ms(0.6);
            }});
            f.push_back({"edit/playhead-on-end", "the end bracket selected under the playhead (the number beside the crown at the bar's end)", [] {
                EditBase();
                SelectMarker(5, MarkerMenu::Root);
            }});
            f.push_back({"edit/marker-menu-scrolled", "the camera menu focused on its last row (the list scrolls)", [] {
                EditBase();
                SelectMarker(2, MarkerMenu::Camera, 60);
            }});
            f.push_back({"edit/row-typing", "a Spline row's typed number with its error line", [] {
                EditBase();
                SelectMarker(2, MarkerMenu::Spline);
                const std::vector<MenuRow> rows = BuildSplineRows(g_fix.cameraKeyframes[2]);
                for (std::size_t n = 0; n < rows.size(); ++n)
                    if (rows[n].typed)
                    {
                        g_menuFocus = static_cast<int>(n);
                        g_rowTyping.on = true;
                        g_rowTyping.label = rows[n].label;
                        Copy(g_rowTyping.text, sizeof(g_rowTyping.text), "12345.678");
                        // The editor's own refusal (DrawRowTypeBox), for this row's range.
                        static std::string why;
                        why = TRF("typing.range", "A number from {min} to {max}.", {{"min", replay::i18n::Printf("%g", rows[n].typeMin)},
                            {"max", replay::i18n::Printf("%g", rows[n].typeMax)}});
                        g_rowTyping.error = why.c_str();
                        break;
                    }
            }});
            // The shake in the marker's menu.
            f.push_back({"edit/marker-shake-off", "a marker's Shake menu with the shake off (the rows greyed)", [] {
                EditBase();
                SelectMarker(2, MarkerMenu::Shake);
                g_fix.cameraKeyframeShake[2].on = 0;
            }});
            f.push_back({"edit/marker-shake-advanced-last", "the Shake Advanced menu focused on its last row", [] {
                EditBase();
                SelectMarker(2, MarkerMenu::ShakeAdvanced, 60);
            }});
            f.push_back({"edit/marker-shake-anchor", "an Anchor the shake migration added, selected (its Shake row open)", [] {
                EditBase();
                // Marker 3 as the migration adds one: an Anchor with a shake.
                CameraKeyframe& a = g_fix.cameraKeyframes[3];
                a.markerType = MarkerType::Anchor;
                a.markerOrigin = 1;
                MarkerShake& m = g_fix.cameraKeyframeShake[3];
                m = DefaultMarkerShake();
                m.on = 1;
                m.amp = 1.5f;
                m.hz = 10.0f;
                m.speedAmp = 0.0f;
                m.speedHz = 0.0f;
                g_fix.playheadMs = a.timeMs;
                g_selectedKeyframeId = a.id;
                g_menuMarker = a.id;
                g_markerMenu = MarkerMenu::Root;
                g_menuFocus = 3;
            }});
            f.push_back({"edit/freecam-menu", "the free camera's options", [] {
                EditBase();
                KeyBarStress();
                g_freeCamMenuOpen = true;
                g_menuFocus = 3;
            }});
            f.push_back({"edit/freecam-spline-defaults", "the Spline Defaults from the free camera's menu", [] {
                EditBase();
                g_freeCamMenuOpen = true;
                g_splineDefaultsOpen = true;
            }});
            f.push_back({"edit/timeline-crowded", "512 markers with their shake badges, zoom x32, a marker selected (its number)", [] {
                Playing(Ms(3600.0), Ms(1800.0), true, kLongClip);
                Keyframes(kMaxCameraKeyframesInSnapshot);
                Targets(4);
                SpansAndCuts();
                MarkerShakes();
                g_screen = Screen::Edit;
                g_timelineZoom = 32.0;
                SelectMarker(256, MarkerMenu::Root);
            }});
            f.push_back({"edit/timeline-512", "512 markers on a 10 minute clip, unzoomed (lanes and ticks), one selected", [] {
                Playing(Ms(600.0), Ms(300.0), true, kLongClip);
                Keyframes(kMaxCameraKeyframesInSnapshot);
                Targets(4);
                SpansAndCuts();
                MarkerShakes();
                g_screen = Screen::Edit;
                SelectMarker(255, MarkerMenu::Root);
            }});
            f.push_back({"edit/world-notice", "the world (mission, chapter) notice beside the edit panel", [] {
                EditBase();
                replay::world::NoticeText(2, replay::world::kChapterEpilogue2, 0, false, g_worldNoticeText, sizeof(g_worldNoticeText));
                g_worldNoticeUntil = Later();
            }});
            f.push_back({"edit/camera", "Edit Camera (the free camera's keys on the bar)", [] {
                EditBase();
                g_fix.flags |= SnapshotEditingCamera;
                g_fix.editingMarker = 3;
                g_fix.camera.fov = 9.0f;
            }});
            f.push_back({"edit/camera-orbit", "Edit Camera orbiting a Look At target", [] {
                EditBase();
                g_fix.cameraKeyframes[2].lookAtTarget = 0x01000001u;
                g_fix.flags |= SnapshotEditingCamera;
                g_fix.editingMarker = 3;
            }});
            // ---------------- Export ----------------
            f.push_back({"export/off", "the export off in the ini", [] {
                EditBase();
                g_screen = Screen::Export;
            }});
            f.push_back({"export/settings", "the dialog: every row, motion blur over slow motion markers", [] {
                EditBase();
                for (std::uint32_t n = 1; n < g_fix.cameraKeyframeCount - 1; ++n)
                    g_fix.cameraKeyframes[n].speedIndex = static_cast<std::uint8_t>(n % 2 ? 0 : 1);
                g_screen = Screen::Export;
                exportstub::Get().enabled = true;
                exportstub::Get().status.channel = true;
                exportstub::Get().backend = "NVENC";
                render_export::ExportController::Get().LoadConfig();
                exportui::g_samples = 3;
                exportui::g_lutPath = L"C:\\LUTs\\film.cube";
                exportui::g_index = 2;
            }});
            f.push_back({"export/h264", "the dialog on its first row (H.264, no blur notes): the panel fitted to its paragraph", [] {
                EditBase();
                g_screen = Screen::Export;
                exportstub::Get().enabled = true;
                exportstub::Get().status.channel = true;
                render_export::ExportController::Get().LoadConfig();
                exportui::g_index = 0;
            }});
            f.push_back({"export/settings-problem", "no encoder: the problem paragraph, the CPU backend, a finished export's path", [] {
                EditBase();
                g_screen = Screen::Export;
                exportstub::State& st = exportstub::Get();
                st.enabled = true;
                st.status.channel = true;
                st.backend = "CPU";
                st.allPresets = false;
                // The controller's words (render_export.cpp EncoderProblem) with a long loader error.
                st.encoderProblem = TRF("export.problem.ffmpeg", "The FFmpeg libraries did not load ({error}). Reinstall the RedemptionEditor folder "
                    "from the release zip.", {{"error", "avcodec-61.dll not found in C:\\Program Files (x86)\\Steam\\steamapps\\common\\Red Dead "
                    "Redemption 2\\RedemptionEditor\\dlls"}});
                st.status.state = replay::exportchannel::State::Done;
                st.status.detail =
                    "C:\\Users\\Player\\Documents\\Rockstar Games\\Red Dead Redemption 2\\Videos\\Redemption Editor\\Saint Denis bank robbery final "
                    "shootout take 12_20261024-170346_h264.mp4";
                render_export::ExportController::Get().LoadConfig();
                exportui::g_preset = 4;
                exportui::g_index = 11;
            }});
            f.push_back({"export/running", "an export running (debug: the stage timings), two more queued", [] {
                EditBase();
                g_fix.flags |= SnapshotDebugMode | SnapshotExporting;
                g_screen = Screen::Export;
                exportstub::State& st = exportstub::Get();
                st.enabled = true;
                st.busy = true;
                st.status.channel = true;
                st.status.state = replay::exportchannel::State::AudioSeek;
                st.status.framesEncoded = 123456;
                st.status.framesTotalHint = 345600;
                st.status.secondsElapsed = 3725.0;
                st.status.secondsLeft = 6600.0;
                st.status.queueDepth = 14;
                st.status.latencyPresents = 3;
                render_export::ExportController::Get().LoadConfig();
                exportui::g_queue.resize(2);
                // As exportui::Build words a queued export.
                exportui::g_lastLabel = TRF("export.job", "{format}, {rate} fps{blur}", {{"format", "Apple ProRes 422 HQ (10 bit)"}, {"rate", "59.94"},
                    {"blur", TR("export.job.blur", ", motion blur")}}) + " [NVENC]";
            }});
            f.push_back({"export/project", "the dialog on a whole project (Range row: Whole project, its total)", [] {
                EditBase();
                ProjectClips(3, true);
                g_screen = Screen::Export;
                exportstub::Get().enabled = true;
                exportstub::Get().status.channel = true;
                render_export::ExportController::Get().LoadConfig();
                exportui::g_range = 2;
                exportui::g_back = Screen::Project;
                exportui::g_index = 2;
            }});
            f.push_back({"export/project-running", "a whole project exporting: segment 2 of 3, the whole's percent", [] {
                EditBase();
                ProjectClips(3, true);
                g_fix.flags |= SnapshotExporting;
                g_screen = Screen::Export;
                exportstub::State& st = exportstub::Get();
                st.enabled = true;
                st.busy = true;
                st.status.channel = true;
                st.status.state = replay::exportchannel::State::Capture;
                st.status.framesEncoded = 123456;
                st.status.framesTotalHint = 345600;
                st.status.segmentIndex = 1;
                st.status.segmentCount = 3;
                st.status.projectFramesBefore = 234567;
                st.status.projectFramesTotal = 987654;
                st.status.secondsElapsed = 3725.0;
                st.status.secondsLeft = 6600.0;
                render_export::ExportController::Get().LoadConfig();
                exportui::g_lastLabel = TRF("export.job", "{format}, {rate} fps{blur}", {{"format", "Apple ProRes 422 HQ (10 bit)"}, {"rate", "59.94"},
                    {"blur", TR("export.job.blur", ", motion blur")}}) + " [NVENC]";
            }});
            f.push_back({"export/project-opening", "a whole project between two pieces: the next clip opening", [] {
                EditBase();
                ProjectClips(3, true);
                g_fix.flags |= SnapshotExporting;
                g_screen = Screen::Export;
                exportstub::State& st = exportstub::Get();
                st.enabled = true;
                st.busy = true;
                st.status.channel = true;
                st.status.state = replay::exportchannel::State::SegmentDone;
                st.status.framesEncoded = 0;
                st.status.segmentIndex = 2;
                st.status.segmentCount = 3;
                st.status.projectFramesBefore = 678901;
                st.status.projectFramesTotal = 987654;
                render_export::ExportController::Get().LoadConfig();
                exportui::g_lastLabel = TRF("export.job", "{format}, {rate} fps{blur}", {{"format", "H.264 (NVENC)"}, {"rate", "60"}, {"blur", ""}});
            }});
            f.push_back({"export/note", "a refused export's note above the key bar", [] {
                EditBase();
                g_screen = Screen::Export;
                exportstub::Get().enabled = true;
                exportstub::Get().status.channel = true;
                render_export::ExportController::Get().LoadConfig();
                exportui::g_note = TR("export.note.reshade", "This ReShade version is not supported");
                exportui::g_noteUntil = Later();
                exportui::g_index = 10;
            }});
            // ---------------- The editor closed: playback bar, notices ----------------
            f.push_back({"hud/clip", "the playback bar: paused, 50 percent, every span kind, cuts", [] {
                Playing(Ms(3725.0), Ms(1234.0), true, kLongClip);
                Keyframes(4);
                SpansAndCuts();
                g_fix.speedIndex = 3;
                g_fix.speedPercent = 50;
            }});
            f.push_back({"hud/loading", "the playback bar while the puppets are built", [] {
                Playing(Ms(95.0), Ms(0.0), false, kLongClip);
                g_fix.flags |= SnapshotPreparing;
                g_fix.speedPercent = 175;
                SpansAndCuts();
            }});
            // ---------------- Loading: the cylinder with its caption ----------------
            f.push_back({"loading/clip-wait", "a clip's world loading behind the black: the cue bottom right, above the playback bar", [] {
                Playing(Ms(95.0), Ms(0.0), false, kLongClip);
                g_fix.flags |= SnapshotPreparing | SnapshotLoadingWorld;
                SpansAndCuts();
            }});
            f.push_back({"loading/clip-wait-debug", "the same with Debug=1 (no playback bar): the cue bottom right over the key bar", [] {
                Playing(Ms(95.0), Ms(0.0), false, kLongClip);
                g_fix.flags |= SnapshotPreparing | SnapshotLoadingWorld | SnapshotDebugMode;
            }});
            f.push_back({"loading/world-exit", "the world loading after a clip closed (not playing): \"Loading...\" with the cylinder", [] {
                g_fix.flags |= SnapshotLoadingWorld;
            }});
            f.push_back({"loading/clip-wait-home", "a clip loading with the home menu open: the cue bottom right", [] {
                g_screen = Screen::Home;
                Playing(Ms(95.0), Ms(0.0), false, kLongClip);
                g_fix.flags |= SnapshotPreparing | SnapshotLoadingWorld;
            }});
            f.push_back({"loading/scrub-far", "a far jump in the clip editor: the frozen picture, the cue on the status line (no lone spinner)", [] {
                EditBase();
                g_fix.flags |= SnapshotPreparing;
                g_fix.flags &= ~SnapshotPaused;
                g_fix.seekLoadState = SeekLoadFrozen;
                g_fix.seekLoadElapsedMs = 900;
            }});
            f.push_back({"loading/scrub-settle", "a near settle past 250 ms in the clip editor (not Preparing): the cue on the status line", [] {
                EditBase();
                g_fix.seekLoadState = SeekLoadSettling;
                g_fix.seekLoadElapsedMs = 600;
            }});
            f.push_back({"loading/scrub-far-hud", "a far jump with the editor closed: the cue on the playback bar's status", [] {
                Playing(Ms(95.0), Ms(40.0), false, kLongClip);
                g_fix.flags |= SnapshotPreparing;
                g_fix.seekLoadState = SeekLoadFrozen;
                SpansAndCuts();
            }});
            f.push_back({"loading/scrub-far-debug", "a far jump with Debug=1 (no playback bar): the cue bottom right with its caption", [] {
                Playing(Ms(95.0), Ms(40.0), false, kLongClip);
                g_fix.flags |= SnapshotPreparing | SnapshotDebugMode;
                g_fix.seekLoadState = SeekLoadFrozen;
            }});
            f.push_back({"hud/project", "the project playing as a whole (its name and clip on the bar)", [] {
                Playing(Ms(48.0), Ms(20.0), false, kLongClip);
                ProjectClips(64, true);
                g_fix.projectFlags |= ProjectPreviewing;
                g_fix.projectPlayheadMs = Ms(20.0);
            }});
            f.push_back({"hud/freecam-keys", "the key bar with the most tips (free camera, a target, QA marks waiting)", [] {
                Playing(Ms(95.0), Ms(20.0), false, kLongClip);
                KeyBarStress();
            }});
            f.push_back({"hud/long-keys", "the key bar with the longest remapped keys", [] {
                Playing(Ms(95.0), Ms(20.0), false, kLongClip);
                KeyBarStress();
                LongKeys(true);
            }});
            f.push_back({"edit/long-keys", "the clip editor's key bar with the longest remapped keys", [] {
                EditBase();
                KeyBarStress();
                LongKeys(true);
            }});
            f.push_back({"notices/recording", "the recording notices stacked: paused gate, REC, saving", [] {
                g_fix.flags |= SnapshotRecording | SnapshotSavingClip;
                g_fix.recordingMs = Ms(3725.0);
                g_fix.recordPauseReasons = 2;
            }});
            // The recording indicator's modes (src/rec_indicator.h): brief 1 s and 5 s
            // after the recording started (shown, then gone), hidden (nothing in the
            // frame: the rows are in the indicator's window), off with a save.
            for (int at = 0; at < 2; ++at)
            {
                static const char* const names[] = {"notices/rec-brief-1s", "notices/rec-brief-5s"};
                f.push_back({names[at], "brief: a recording started 1 s ago (REC shown) and 5 s ago (REC gone)", [at] {
                    g_fix.flags |= SnapshotRecording;
                    g_fix.recordingMs = Ms(at ? 5.0 : 1.0);
                    g_fix.recordingIndicator = static_cast<std::uint32_t>(replay::recindicator::Mode::Brief);
                    replay::recindicator::BriefTimer& brief = g_recBrief;
                    brief.started = true;
                    brief.seen = replay::recindicator::Kind::Rec;
                    brief.savedSequence = g_fix.savedClipSequence;
                    brief.lastMs = g_fix.recordingMs;
                    brief.until = GetTickCount64() - (at ? 5000 : 1000) + replay::recindicator::kBriefMs;
                }});
            }
            f.push_back({"notices/rec-brief-paused", "brief past its 3 s with the gate held: the paused notice stays", [] {
                g_fix.flags |= SnapshotRecording;
                g_fix.recordingMs = Ms(3725.0);
                g_fix.recordPauseReasons = 2;
                g_fix.recordingIndicator = static_cast<std::uint32_t>(replay::recindicator::Mode::Brief);
                g_recBrief.started = true;
                g_recBrief.seen = replay::recindicator::Kind::Rec;
                g_recBrief.savedSequence = g_fix.savedClipSequence;
                g_recBrief.until = 1;
            }});
            f.push_back({"notices/rec-hidden", "hidden from capture: the recording notices in their own window, none in the frame", [] {
                g_fix.flags |= SnapshotRecording | SnapshotSavingClip;
                g_fix.recordingMs = Ms(3725.0);
                g_fix.recordPauseReasons = 2;
                g_fix.recordingIndicator = static_cast<std::uint32_t>(replay::recindicator::Mode::Hidden);
            }});
            f.push_back({"notices/rec-off-saving", "off: no REC nor paused notice, the saving notice stays", [] {
                g_fix.flags |= SnapshotRecording | SnapshotSavingClip;
                g_fix.recordingMs = Ms(3725.0);
                g_fix.recordPauseReasons = 2;
                g_fix.recordingIndicator = static_cast<std::uint32_t>(replay::recindicator::Mode::Off);
            }});
            f.push_back({"notices/buffer-saved", "the replay buffer tag and the clip saved notice", [] {
                g_fix.flags |= SnapshotBufferRunning;
                g_toastUntil = Later();
                g_toastClipMs = Ms(3599.0);
            }});
            f.push_back({"notices/opening", "opening a clip while the recording is saved first", [] {
                g_fix.flags |= SnapshotRecording | SnapshotSavingClip;
                g_fix.recordingMs = Ms(61.0);
                g_loadName = kLongClip;
                g_loadWaitFrames = 100000;
                g_openingSavesRecording = true;
            }});
            f.push_back({"notices/world", "the world (mission, chapter) notice over the playback bar", [] {
                Playing(Ms(95.0), Ms(20.0), false, kLongClip);
                replay::world::NoticeText(2, replay::world::kChapterEpilogue2, 0, true, g_worldNoticeText, sizeof(g_worldNoticeText));
                g_worldNoticeUntil = Later();
            }});
            f.push_back({"notices/qa", "the QA notice top right (long report key)", [] {
                Playing(Ms(95.0), Ms(20.0), false, kLongClip);
                KeyBarStress();
                replay::editor::uiqa::FormatStopReminder(g_qaNoticeText, sizeof(g_qaNoticeText), 12, LongReportKey());
                g_qaNoticeUntil = Later();
            }});
            // Nowhere to save (src/storage_paths.h): a Program Files game folder and
            // both fallbacks refused.
            f.push_back({"notices/storage-failed", "the notice when no folder takes writes: long Program Files and OneDrive paths", [] {
                g_storageNotice.gameDetail =
                    "C:\\Program Files\\Rockstar Games\\Red Dead Redemption 2\\: denied (create error 5)";
                g_storageNotice.fallbackDetail =
                    "C:\\Users\\Player\\OneDrive\\Documents\\Rockstar Games\\Red Dead Redemption 2\\Redemption Editor\\: denied "
                    "(create error 5); C:\\Users\\Player\\AppData\\Local\\RedemptionEditor\\: denied (create error 5)";
                g_storageNotice.until = Later();
            }});
            // The notice after a crash (src/crash_notice.h).
            f.push_back({"notices/crash", "the crash notice at a game start: two crashes, a long report key, a long game folder", [] {
                replay::crashnotice::Decision decision;
                decision.show = true;
                decision.crashes = 2;
                decision.stamp = "20261024-231507";
                g_crashNotice.decision = decision;
                replay::keys::Current() = LongReportKey();
                g_crashNotice.reportsFolder = "D:\\SteamLibrary\\steamapps\\common\\Red Dead Redemption 2\\ReplayReports";
                g_crashNotice.includeOpenClip = false;
                g_crashNotice.until = Later();
            }});
            f.push_back({"notices/crash-busy", "the crash notice with the QA notice, the recording notices and the open clip asked", [] {
                g_fix.flags |= SnapshotRecording | SnapshotBufferRunning;
                g_fix.recordingMs = Ms(42.0);
                KeyBarStress();
                replay::editor::uiqa::FormatStopReminder(g_qaNoticeText, sizeof(g_qaNoticeText), 12, LongReportKey());
                g_qaNoticeUntil = Later();
                replay::crashnotice::Decision decision;
                decision.show = true;
                decision.crashes = 1;
                decision.stamp = "20261025-010203";
                g_crashNotice.decision = decision;
                g_crashNotice.reportsFolder = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Red Dead Redemption 2\\ReplayReports";
                g_crashNotice.includeOpenClip = true;
                g_crashNotice.until = Later();
            }});
            f.push_back({"qa/note-box", "the QA note box over the playback with a long note", [] {
                Playing(Ms(3725.0), Ms(1234.5), false, kLongClip);
                KeyBarStress();
                qa_shots::g_state.noteOpen = true;
                qa_shots::g_state.notePlayheadMs = Ms(1234.5);
                Copy(qa_shots::g_state.note, sizeof(qa_shots::g_state.note),
                    "the horse froze in the middle of the street and then Arthur vanished when the camera cut to the marker");
            }});
            // ---------------- Help ----------------
            f.push_back({"help/edit", "the key help over the clip editor (QA marks on)", [] {
                EditBase();
                KeyBarStress();
                g_helpOpen = true;
            }});
            f.push_back({"help/project", "the key help over the project", [] {
                g_screen = Screen::Project;
                ProjectClips(3, false);
                g_helpOpen = true;
            }});
            f.push_back({"help/long-keys", "the key help with the longest remapped keys", [] {
                EditBase();
                KeyBarStress();
                LongKeys(true);
                g_helpOpen = true;
            }});
            // ---------------- Remapped keys ----------------
            // The texts that name a key show the [Keys] table's (RemappedKeys:
            // Report Ctrl+F11, MarkProblem Ctrl+M, Record J, SaveBuffer Shift+J).
            f.push_back({"keys/remap-help", "the key help over the clip editor with remapped keys", [] {
                EditBase();
                KeyBarStress();
                RemappedKeys();
                g_helpOpen = true;
            }});
            f.push_back({"keys/remap-crash", "the crash notice with remapped report and mark keys", [] {
                RemappedKeys();
                replay::crashnotice::Decision decision;
                decision.show = true;
                decision.crashes = 1;
                decision.stamp = "20261026-101500";
                g_crashNotice.decision = decision;
                g_crashNotice.reportsFolder = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Red Dead Redemption 2\\ReplayReports";
                g_crashNotice.until = Later();
            }});
            f.push_back({"keys/remap-notice", "the QA mark notice and the key bar with a remapped report key", [] {
                Playing(Ms(95.0), Ms(20.0), false, kLongClip);
                KeyBarStress();
                RemappedKeys();
                replay::editor::uiqa::FormatMarkNotice(g_qaNoticeText, sizeof(g_qaNoticeText), 3, replay::keys::Current());
                g_qaNoticeUntil = Later();
            }});
            f.push_back({"keys/remap-home", "home with Save Buffer selected and remapped record keys", [] {
                g_screen = Screen::Home;
                g_fix.flags |= SnapshotBufferRunning;
                g_fix.bufferEnabled = 1;
                g_fix.bufferHeldMs = Ms(41.0);
                g_fix.bufferSeconds = 300;
                g_fix.bufferMB = 1024;
                RemappedKeys();
                g_homeIndex = 4;
            }});
            // ---------------- The cursor ----------------
            // The mouse as ReShade 6.8 serves it (src/editor_cursor.h): the
            // editor draws its cursor, the painted ink arrow (the drawn one
            // with --flat), its tip on the mouse.
            f.push_back({"cursor/home", "the painted cursor over the home menu (ReShade 6.8: the editor draws it)", [] {
                g_screen = Screen::Home;
                g_harnessMouse = HarnessMouse{true, 260.0f, 420.0f};
            }});
            f.push_back({"cursor/home-reshade66", "the painted cursor over the home menu under ReShade 6.6 / 6.7 (the write reaches ImGui; its arrow hidden)", [] {
                g_screen = Screen::Home;
                g_harnessMouse = HarnessMouse{true, 260.0f, 420.0f, true};
            }});
            f.push_back({"cursor/edit", "the painted cursor over the clip editor's timeline, a marker selected", [] {
                EditBase();
                SelectMarker(2, MarkerMenu::Root, 0);
                g_harnessMouse = HarnessMouse{true, 900.0f, 960.0f};
            }});
            f.push_back({"cursor/edge", "the mouse at the picture's bottom right corner (the cursor clamped, cut by the edge)", [] {
                g_screen = Screen::Home;
                g_harnessMouse = HarnessMouse{true, 5000.0f, 5000.0f};
            }});
            // ---------------- No core ----------------
            f.push_back({"waiting/no-core", "the editor open before the core answers", [] {
                g_screen = Screen::Home;
                g_offline = true;
            }});
            return f;
        }

        std::vector<Fixture>& All()
        {
            static std::vector<Fixture> fixtures = MakeFixtures();
            return fixtures;
        }
    }

    std::vector<FixtureInfo> Fixtures()
    {
        std::vector<FixtureInfo> out;
        for (const Fixture& f : All())
            out.push_back({f.name, f.about});
        return out;
    }

    void Begin(const void* imguiTable)
    {
        EditorCoreStart(static_cast<const imgui_function_table*>(imguiTable));
        EnsureTransport();
        if (g_transport)
            InitializeSharedTransport(*g_transport, GetCurrentProcessId(), 1);
        g_qaShared.version = replay::qa::kSharedVersion;
        g_qaShared.structSize = sizeof(replay::qa::Shared);
        qa_shots::g_state.shared = &g_qaShared;
    }

    void Prepare(std::size_t index)
    {
        static std::size_t last = ~std::size_t(0);
        if (index != last)
        {
            last = index;
            ++g_fixtureSequence; // a new clip for the editor's per-clip state
        }
        BaseSnapshot();
        BaseEditor();
        LongKeys(false);
        All()[index].setup();
        if (!g_transport)
            return;
        // The editor's commands: read and dropped, as a core with nothing to do.
        Command command{};
        while (TryPopSharedCommand(*g_transport, command, GetCurrentProcessId()))
        {
        }
        InterlockedExchange(&g_transport->online, g_offline ? 0 : 1);
        if (!g_offline)
            PublishSharedSnapshot(*g_transport, g_fix, GetCurrentProcessId());
    }

    void Overlay(reshade::api::effect_runtime* runtime) { EditorCoreOverlay(runtime); }

    // ---------------- Interaction scripts ----------------
    namespace
    {
        struct Script
        {
            int width = 1920;
            int height = 1080;
            const std::function<void(const InteractionInput&)>* frame = nullptr;
            float mouseX = 0.0f, mouseY = 0.0f; // units
            std::string failure;
            std::vector<Command> sent; // the editor's commands, in order

            float Scale() const
            {
                return static_cast<float>(height) / 1080.0f *
                    replay::editor::uiscale::Effective(g_uiScalePercent, static_cast<float>(width), static_cast<float>(height));
            }

            // One frame: the core's side as Prepare's (the snapshot published,
            // the editor's commands read and dropped), then the editor's frame.
            void Frame(std::vector<int> keys = {}, bool mouseDown = false)
            {
                if (g_transport)
                {
                    Command command{};
                    while (TryPopSharedCommand(*g_transport, command, GetCurrentProcessId()))
                        sent.push_back(command);
                    PublishSharedSnapshot(*g_transport, g_fix, GetCurrentProcessId());
                }
                for (int& look : g_rowLooks)
                    look = 0;
                g_rowBoxes.clear();
                InteractionInput input;
                input.keysDown = std::move(keys);
                input.mouseDown = mouseDown;
                input.mouseX = mouseX == -FLT_MAX ? -FLT_MAX : mouseX * Scale();
                input.mouseY = mouseY == -FLT_MAX ? -FLT_MAX : mouseY * Scale();
                (*frame)(input);
            }

            // A key pressed and released (two frames), the mouse where it is.
            void Press(ImGuiKey key)
            {
                Frame({static_cast<int>(key)});
                Frame();
            }

            void MoveTo(float x, float y)
            {
                mouseX = x;
                mouseY = y;
                Frame();
            }

            void Click()
            {
                Frame({}, true);
                Frame();
            }

            void Expect(bool ok, const std::string& what)
            {
                if (!ok && failure.empty())
                    failure = what;
            }
        };

        // A fixture's start (Prepare once) and a few frames for the layout.
        bool Start(Script& script, const char* fixture)
        {
            const std::vector<Fixture>& all = All();
            for (std::size_t n = 0; n < all.size(); ++n)
                if (std::strcmp(all[n].name, fixture) == 0)
                {
                    // Twice: a new fixture is a new clip for the editor, whose first
                    // frame drops the last clip's marker selection.
                    Prepare(n);
                    script.Frame();
                    Prepare(n);
                    script.Frame();
                    return true;
                }
            script.failure = std::string("no fixture ") + fixture;
            return false;
        }

        // DrawHome's rows (units): the first row's top, 50 apart, 44 high.
        float HomeRowY(int row) { return std::max(60.0f, g_vh * 0.16f) + 126.0f + row * 50.0f + 22.0f; }
        // The middle of the n-th row box drawn last frame (units), or false.
        bool RowMiddle(std::size_t n, float& x, float& y)
        {
            if (n >= g_rowBoxes.size())
                return false;
            x = (g_rowBoxes[n].x + g_rowBoxes[n].z) * 0.5f;
            y = (g_rowBoxes[n].y + g_rowBoxes[n].w) * 0.5f;
            return true;
        }
        int HoveredLooks() { return g_rowLooks[static_cast<int>(RowLook::Hovered)]; }
        int SelectedLooks() { return g_rowLooks[static_cast<int>(RowLook::Selected)] + g_rowLooks[static_cast<int>(RowLook::SelectedGreyed)]; }
    }

    std::vector<InteractionResult> RunInteractions(int width, int height, const std::function<void(const InteractionInput&)>& frame)
    {
        std::vector<InteractionResult> results;
        const auto run = [&](const char* name, const std::function<void(Script&)>& body) {
            Script script;
            script.width = width;
            script.height = height;
            script.frame = &frame;
            script.mouseX = -FLT_MAX; // no mouse until the script puts one
            script.mouseY = -FLT_MAX;
            body(script);
            results.push_back({name, script.failure});
            End();
        };

        // A regression: Up and Down under a mouse resting over the
        // list. The selection moves, and the row under the mouse never shows a
        // second (grey) outline after a key: one highlight, the selection's.
        run("home/keys-under-resting-mouse", [](Script& s) {
            // The mouse over Projects from the menu's first frame (it rests there).
            s.mouseX = 300.0f;
            s.mouseY = HomeRowY(2);
            if (!Start(s, "home/idle"))
                return;
            s.Frame();
            s.Frame();
            s.Expect(g_homeIndex == 0, "a resting mouse took the selection (Recorded Clips expected)");
            s.Press(ImGuiKey_DownArrow);
            s.Expect(g_homeIndex == 1, "Down under a resting mouse: New Project expected, got " + std::to_string(g_homeIndex));
            s.Press(ImGuiKey_DownArrow);
            s.Press(ImGuiKey_DownArrow);
            s.Expect(g_homeIndex == 3, "Down past the row under the mouse: Record expected, got " + std::to_string(g_homeIndex));
            s.Frame();
            s.Expect(HoveredLooks() == 0, "after a key the row under the resting mouse still shows its grey outline (two highlights)");
            s.Expect(SelectedLooks() == 1, "one selected row expected, " + std::to_string(SelectedLooks()) + " drawn");
            s.Press(ImGuiKey_UpArrow);
            s.Expect(g_homeIndex == 2, "Up: Projects (under the mouse) expected, got " + std::to_string(g_homeIndex));
            s.Press(ImGuiKey_UpArrow);
            s.Press(ImGuiKey_UpArrow);
            s.Expect(g_homeIndex == 0, "Up past the row under the mouse: Recorded Clips expected, got " + std::to_string(g_homeIndex));
        });

        // RDR2's menus: the mouse that moves takes the selection; the keys go
        // on from there, and the resting mouse never takes it back.
        run("home/mouse-moves-then-keys", [](Script& s) {
            if (!Start(s, "home/idle"))
                return;
            s.mouseX = 300.0f;
            s.mouseY = HomeRowY(0);
            s.Frame();
            s.MoveTo(300.0f, HomeRowY(2));
            s.Expect(g_homeIndex == 2, "the mouse moved onto Projects: it takes the selection, got " + std::to_string(g_homeIndex));
            s.Frame();
            s.Expect(HoveredLooks() == 0 && SelectedLooks() == 1, "the row under the mouse is the selection: one outline");
            s.Press(ImGuiKey_DownArrow);
            s.Frame();
            s.Frame();
            s.Expect(g_homeIndex == 3, "Down from Projects with the mouse resting on it: Record expected, got " + std::to_string(g_homeIndex));
            s.MoveTo(310.0f, HomeRowY(2) + 4.0f);
            s.Expect(g_homeIndex == 2, "the mouse moves within Projects again: it takes the selection back");
        });

        // A click still selects and opens.
        run("home/click-opens", [](Script& s) {
            if (!Start(s, "home/idle"))
                return;
            s.MoveTo(300.0f, HomeRowY(2));
            s.Click();
            s.Expect(g_screen == Screen::Projects, "a click on Projects opens the projects");
        });

        // Replay Settings: the list's own order under a resting mouse.
        run("settings/keys-under-resting-mouse", [](Script& s) {
            if (!Start(s, "home/idle"))
                return;
            g_screen = Screen::Settings;
            g_settingsIndex = 0;
            s.Frame();
            // The mouse comes to the fourth row and rests there (the keys then).
            float x = 0.0f, y = 0.0f;
            s.Expect(RowMiddle(3, x, y), "Replay Settings drew no fourth row");
            s.MoveTo(x - 40.0f, y);
            s.MoveTo(x, y);
            s.Expect(g_settingsIndex != 0, "the mouse moved onto a row: it takes the selection");
            g_settingsIndex = 0; // the keys from the first row, the mouse resting
            s.Frame();
            s.Frame();
            s.Expect(g_settingsIndex == 0, "a resting mouse took the Replay Settings selection");
            s.Press(ImGuiKey_DownArrow);
            s.Expect(g_settingsIndex == 1, "Down: Length expected, got " + std::to_string(g_settingsIndex));
            s.Press(ImGuiKey_DownArrow);
            s.Expect(g_settingsIndex == 2, "Down: Memory expected, got " + std::to_string(g_settingsIndex));
            s.Frame();
            s.Expect(HoveredLooks() == 0, "after a key the row under the resting mouse shows its grey outline");
        });

        // A marker's menu in the clip editor.
        run("edit/marker-menu-keys-under-resting-mouse", [](Script& s) {
            if (!Start(s, "edit/playhead-on-marker"))
                return;
            const int before = g_menuFocus;
            // The mouse over the fourth row from the start (the menu's rows are the
            // only boxed rows of the panel), resting.
            float x = 0.0f, y = 0.0f;
            s.Expect(RowMiddle(3, x, y), "the marker menu drew no fourth row");
            s.mouseX = x;
            s.mouseY = y;
            g_menuPointer.Reset();
            s.Frame();
            s.Frame();
            s.Expect(g_menuFocus == before, "a resting mouse took the marker menu's focus");
            s.Expect(g_selectedKeyframeId != 0, "the marker's menu closed before the key");
            s.Press(ImGuiKey_DownArrow);
            s.Expect(g_menuFocus == before + 1, "Down in the marker menu: focus " + std::to_string(before + 1) + " expected, got " + std::to_string(g_menuFocus));
            s.Frame();
            s.Expect(HoveredLooks() == 0, "after a key the marker menu's row under the resting mouse shows its grey outline");
        });

        // The first marker before the clip's opening hold (a project clip's start at
        // 0): a click selects it, the core's seek stops at the hold, and the
        // selection stays (it closed itself 700 ms later); it closes once the
        // playhead leaves the hold.
        run("edit/first-marker-before-hold-stays-selected", [](Script& s) {
            if (!Start(s, "edit/playhead-on-marker"))
                return;
            constexpr std::uint64_t kHold = 293; // a hold seen in the logs (CAMERA_MARKER clip_bounds)
            g_selectedKeyframeId = 0;
            g_fix.playheadMs = kHold;
            s.Frame();
            s.Frame();
            const std::uint32_t first = g_fix.cameraKeyframes[0].id;
            s.Expect(g_fix.cameraKeyframes[0].timeMs == 0, "the fixture's first marker is not at 0");
            // The first marker's icon on the bar (DrawEditTimeline: x at the bar's left
            // end, unzoomed; y the mark's lane 0).
            const float barY = g_vh - g_footH - 54.0f;
            s.MoveTo(40.0f, TimelineArtReady() ? barY + (kTrackTop + kTrackBottom) * 0.5f : barY - 11.0f);
            s.Click();
            s.Expect(g_selectedKeyframeId == first, "a click on the first marker did not select it, got " + std::to_string(g_selectedKeyframeId));
            s.Frame();
            // 700 ms on (the close rule's wait), the playhead still at the hold.
            g_selectedAt = GetTickCount64() - 1000;
            for (int n = 0; n < 8; ++n)
                s.Frame();
            s.Expect(g_selectedKeyframeId == first, "the first marker deselected itself with the playhead at the opening hold");
            // A frame step from the hold keeps it (as on any marker); a jump away closes it.
            g_fix.playheadMs = kHold + 33;
            s.Frame();
            s.Expect(g_selectedKeyframeId == first, "a frame step from the hold closed the first marker");
            g_fix.playheadMs = kHold + 1500;
            s.Frame();
            s.Frame();
            s.Expect(g_selectedKeyframeId == 0, "the playhead left the hold and the first marker stayed selected");
            // Any other marker the playhead is not on still closes.
            g_selectedKeyframeId = g_fix.cameraKeyframes[2].id;
            s.Frame();
            g_selectedAt = GetTickCount64() - 1000;
            s.Frame();
            s.Frame();
            s.Expect(g_selectedKeyframeId == 0, "a marker away from the playhead stayed selected");
        });

        // A list that scrolls (300 clips): the moving mouse never selects (the
        // list would scroll under it), the keys do.
        run("gallery/mouse-in-scrolling-list", [](Script& s) {
            if (!Start(s, "gallery/many-long"))
                return;
            const int before = g_galleryIndex;
            const float top = (g_vh - 720.0f) * 0.5f + 180.0f;
            s.mouseX = 300.0f;
            s.mouseY = top + 6 * 40.0f + 18.0f;
            s.Frame();
            s.MoveTo(300.0f, top + 7 * 40.0f + 18.0f);
            s.Frame();
            s.Expect(g_galleryIndex == before, "the moving mouse selected in a list that scrolls");
            s.Expect(HoveredLooks() == 1, "the moved mouse's row shows its grey outline");
            s.Press(ImGuiKey_DownArrow);
            s.Frame();
            s.Expect(g_galleryIndex == before + 1, "Down in the gallery under the mouse");
            s.Expect(HoveredLooks() == 0, "after a key no grey outline under the resting mouse");
        });
        // Renaming a project from the Projects list: R opens the name box on the
        // selected one; a taken name is refused there (nothing sent); a free one
        // sends ProjectRename "file|name", and the list keeps the renamed project
        // selected when it comes back in its new place.
        run("projects/rename-from-list", [](Script& s) {
            if (!Start(s, "projects/rename-list"))
                return;
            s.Press(ImGuiKey_R);
            s.Expect(g_screen == Screen::ProjectName && !g_projectNameNew, "R on a project: the rename box expected");
            s.Expect(g_projectNameFile == "Chase.rpproj" && std::string(g_projectNameText) == "Chase", "the box names Chase: " + g_projectNameFile);
            ShowProjectName(false, "Chase.rpproj", "Rhodes", Screen::Projects);
            s.Frame();
            s.Frame();
            s.sent.clear();
            s.Press(ImGuiKey_Enter);
            s.Expect(g_screen == Screen::ProjectName, "a taken name: the box stays");
            s.Expect(g_projectNameError[0] != 0, "a taken name: its error line");
            for (const Command& c : s.sent)
                s.Expect(c.type != CommandType::ProjectRename, "a taken name was sent");
            ShowProjectName(false, "Chase.rpproj", "Night: ride", Screen::Projects);
            s.Frame();
            s.Frame();
            s.sent.clear();
            s.Press(ImGuiKey_Enter);
            s.Expect(g_screen == Screen::Projects, "a free name: back to the list");
            bool renamed = false;
            for (const Command& c : s.sent)
                renamed = renamed || (c.type == CommandType::ProjectRename && std::strcmp(c.name, "Chase.rpproj|Night ride") == 0);
            s.Expect(renamed, "ProjectRename \"Chase.rpproj|Night ride\" (the cleaned name) expected");
            // The core's new list: the renamed project now third.
            std::snprintf(g_fix.projects[2].file, sizeof(g_fix.projects[2].file), "%s", "Night ride.rpproj");
            Copy(g_fix.projects[2].name, sizeof(g_fix.projects[2].name), "Night ride");
            ++g_fix.projectListSequence;
            s.Frame();
            s.Frame();
            s.Expect(g_projectsIndex == 3, "the renamed project stays selected, got row " + std::to_string(g_projectsIndex));
            s.Press(ImGuiKey_Escape);
        });

        // Renaming the open project from its own screen: R, the box over it,
        // Esc back to the project.
        run("project/rename-key", [](Script& s) {
            if (!Start(s, "project/empty"))
                return;
            s.Press(ImGuiKey_R);
            s.Expect(g_screen == Screen::ProjectName && !g_projectNameNew, "R in the project: the rename box expected");
            s.Expect(g_projectNameFile == g_fix.projectFile, "the box names the open project's file: " + g_projectNameFile);
            s.Press(ImGuiKey_Escape);
            s.Expect(g_screen == Screen::Project, "Esc: back to the project");
        });

        return results;
    }

    bool FontsReady() { return display_font::g_title.Ready() && display_font::g_body.Ready(); }

    std::size_t MissingGlyphs() { return display_font::g_title.MissingGlyphs() + display_font::g_body.MissingGlyphs(); }

    void End()
    {
        LongKeys(false);
        qa_shots::g_state.noteOpen = false;
    }

    void SetScale(int percent) { g_runScale = replay::editor::uiscale::Clamp(percent); }

    bool MousePixels(int width, int height, float& x, float& y)
    {
        if (!g_harnessMouse.on)
            return false;
        // The editor's unit at this display (UpdateLayoutScale); the menus
        // that take a mouse stand at the screen's origin (no stage).
        const float scale = static_cast<float>(height) / 1080.0f *
            replay::editor::uiscale::Effective(g_uiScalePercent, static_cast<float>(width), static_cast<float>(height));
        x = g_harnessMouse.x * scale;
        y = g_harnessMouse.y * scale;
        return true;
    }

    void HideFace(const char* file)
    {
        std::wstring name;
        for (const char* c = file; *c; ++c)
            name += static_cast<wchar_t>(*c);
        display_font::HiddenFaces().push_back(name);
    }
}
