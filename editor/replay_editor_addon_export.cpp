// replay_editor_addon_export.cpp: the export dialog: its choices, the queue and the resume offer.
// Split from replay_editor_addon.cpp (moved code); shared internals in replay_editor_addon_internal.h.

#include "replay_editor_addon_internal.h"

namespace exportui
{
        using render_export::ExportController;

        // A choice's text: TR(key, label) where it is drawn.
        struct Choice
        {
            const char* key;
            const char* label;
            float value;
        };
        const ec::VideoPreset kPresets[] = {ec::VideoPreset::H264Nvenc, ec::VideoPreset::HevcNvenc, ec::VideoPreset::Av1Nvenc,
            ec::VideoPreset::PreviewH264, ec::VideoPreset::ProRes422Hq, ec::VideoPreset::DnxhrHqx, ec::VideoPreset::LosslessFfv1,
            ec::VideoPreset::LosslessNvencHevc, ec::VideoPreset::PngSequence};
        const replay::exporting::FrameRate kRates[] = {{24, 1}, {25, 1}, {30, 1}, {50, 1}, {60, 1}, {120, 1}, {30000, 1001}, {60000, 1001}};
        const char* const kRateLabels[] = {"24", "25", "30", "50", "60", "120", "29.97", "59.94"};
        const std::uint32_t kSamples[] = {1, 8, 16, 32};
        const float kShutters[] = {90.0f, 180.0f, 270.0f, 360.0f};
        const Choice kLetterbox[] = {{TR_NOOP("common.off_cap", "Off"), 0.0f}, {"", "2.39:1", 2.39f}, {"", "2.00:1", 2.0f}, {"", "1.85:1", 1.85f}};
        const Choice kGrain[] = {{TR_NOOP("common.off_cap", "Off"), 0.0f}, {TR_NOOP("export.grain.light", "Light"), 1.5f},
            {TR_NOOP("export.grain.medium", "Medium"), 3.0f}, {TR_NOOP("export.grain.heavy", "Heavy"), 5.0f}};

        int g_index = 0;
        int g_preset = 0;
        int g_rate = 4;
        int g_range = 1; // 0 whole clip, 1 start to end markers, 2 whole project (a project with clips open)
        Screen g_back = Screen::Edit; // where Esc goes (the project screen opens it on the whole project)
        int g_samples = 0;
        int g_shutter = 1;
        int g_letterbox = 0;
        int g_grain = 0;
        bool g_lut = false;
        bool g_audio = true;
        bool g_chapters = true;
        std::wstring g_lutPath; // [Export] Lut
        bool g_configLoaded = false;
        // The unfinished export found when Export was pressed: asked before it is queued.
        struct Offer
        {
            bool active = false;
            Pending pending;
            render_export::ResumeOffer offer;
            bool startNow = false;
            int index = 0;
        };
        Offer g_offer;
        std::vector<Pending> g_queue;
        std::string g_lastLabel;
        std::uint64_t g_requestedAt = 0;
        std::string g_note;
        std::uint64_t g_noteUntil = 0;

        bool TrimRange(std::uint64_t& start, std::uint64_t& end)
        {
            const std::uint32_t count = g_snapshot.cameraKeyframeCount;
            if (count < 2)
                return false;
            start = g_snapshot.cameraKeyframes[0].timeMs;
            end = g_snapshot.cameraKeyframes[count - 1].timeMs;
            return end > start;
        }

        // The open project has clips: the "Whole project" range is offered.
        bool ProjectRange() { return ProjectIsOpen() && g_snapshot.projectClipCount > 0; }

        // A project clip whose file is gone (the export is refused before it starts).
        int MissingProjectClip()
        {
            const std::uint32_t count = std::min(g_snapshot.projectClipCount, kMaxProjectClipsInSnapshot);
            for (std::uint32_t n = 0; n < count; ++n)
                if (g_snapshot.projectClips[n].flags & ProjectClipMissing)
                    return static_cast<int>(n);
            return -1;
        }

        // What a clip range exports, beyond the dialog's settings: the clip, its
        // markers (time, speed, camera) and the playback speed. A segmented
        // export continues only when this is unchanged.
        std::string ExportIdentity()
        {
            std::string markers = std::to_string(g_snapshot.speedPercent);
            char item[160];
            for (std::uint32_t k = 0; k < g_snapshot.cameraKeyframeCount; ++k)
            {
                const CameraKeyframe& key = g_snapshot.cameraKeyframes[k];
                std::snprintf(item, sizeof(item), ";%llu,%u,%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f", static_cast<unsigned long long>(key.timeMs),
                    static_cast<unsigned>(key.speedIndex), static_cast<unsigned>(key.markerType), static_cast<unsigned>(key.cameraMode),
                    key.camera.position.x, key.camera.position.y, key.camera.position.z, key.camera.rotation.x, key.camera.rotation.y,
                    key.camera.rotation.z, key.camera.fov);
                markers += item;
            }
            const std::string clip(g_snapshot.currentClip, strnlen(g_snapshot.currentClip, sizeof(g_snapshot.currentClip)));
            return clip + "|" + render_export::segments::HashText(markers);
        }

        void Range(std::uint64_t& start, std::uint64_t& end)
        {
            if (g_range == 2 && !ProjectRange())
                g_range = 0;
            else if (g_range != 2 && !Flag(SnapshotPlaying) && ProjectRange())
                g_range = 2; // no clip open: the project is what there is to export
            start = 0;
            end = g_range == 2 ? g_snapshot.projectTotalMs : g_snapshot.durationMs;
            if (g_range == 1)
                TrimRange(start, end);
        }

        // The range row's next value: whole clip, start to end markers (when the
        // clip has them), whole project (when a project with clips is open);
        // without a clip open only the project.
        int NextRange(int range, int direction)
        {
            std::uint64_t a = 0, b = 0;
            const bool clip = Flag(SnapshotPlaying);
            const bool available[3] = {clip, clip && TrimRange(a, b), ProjectRange()};
            for (int k = 1; k <= 3; ++k)
            {
                const int candidate = ((range + direction * k) % 3 + 3) % 3;
                if (available[candidate])
                    return candidate;
            }
            return 0;
        }

        std::wstring Widen(const char* text)
        {
            wchar_t out[256]{};
            MultiByteToWideChar(CP_UTF8, 0, text, -1, out, 255);
            return out;
        }

        Pending Build()
        {
            Pending pending;
            ec::Settings& s = pending.settings;
            Range(s.startMs, s.endMs);
            s.rate = kRates[g_rate];
            s.samples = kSamples[g_samples];
            s.shutterDegrees = kShutters[g_shutter];
            s.prerollFrames = 30;
            s.preset = kPresets[g_preset];
            s.audio = g_audio && s.preset != ec::VideoPreset::PngSequence ? ec::AudioMode::RealtimePass : ec::AudioMode::None;
            s.letterboxAspect = kLetterbox[g_letterbox].value;
            s.grain = kGrain[g_grain].value;
            s.thumbnail = 1;
            s.chapters = g_chapters ? 1u : 0u;
            if (g_lut && !g_lutPath.empty())
                wcsncpy_s(s.lutPath, g_lutPath.c_str(), _TRUNCATE);
            std::wstring clip = g_snapshot.currentClip[0] ? Widen(g_snapshot.currentClip) : L"replay-buffer";
            const std::size_t dot = clip.rfind(L".rpclip");
            if (dot != std::wstring::npos)
                clip = clip.substr(0, dot);
            pending.project = g_range == 2;
            if (!pending.project)
                pending.identity = ExportIdentity();
            if (pending.project)
            {
                // Named after the project; one chapter per clip (the add-on's).
                clip = g_snapshot.projectName[0] ? Widen(g_snapshot.projectName) : L"project";
                const std::uint32_t count = std::min(g_snapshot.projectClipCount, kMaxProjectClipsInSnapshot);
                for (std::uint32_t n = 0; n < count; ++n)
                    pending.titles.emplace_back(g_snapshot.projectClips[n].file, strnlen(g_snapshot.projectClips[n].file, sizeof(g_snapshot.projectClips[n].file)));
            }
            SYSTEMTIME now{};
            GetLocalTime(&now);
            const std::wstring path = render_export::DefaultOutputPath(ExportController::Get().Folder(), clip, s.preset, now.wYear, now.wMonth,
                now.wDay, now.wHour, now.wMinute, now.wSecond);
            wcsncpy_s(s.outputPath, path.c_str(), _TRUNCATE);
            // Markers inside the range become chapters ("Start" at 0).
            pending.chapters.push_back({0, "Start"});
            for (std::uint32_t k = 0; k < g_snapshot.cameraKeyframeCount && !pending.project; ++k)
            {
                const std::uint64_t t = g_snapshot.cameraKeyframes[k].timeMs;
                if (t > s.startMs + 500 && t + 500 < s.endMs)
                    pending.chapters.push_back({t - s.startMs, "Marker " + std::to_string(k + 1)});
            }
            const render_export::PresetInfo info = render_export::InfoOf(s.preset);
            pending.label = TRF("export.job", "{format}, {rate} fps{blur}", {{"format", TR(info.labelKey, info.label)}, {"rate", kRateLabels[g_rate]},
                {"blur", s.samples > 1 ? TR("export.job.blur", ", motion blur") : ""}});
            const std::string backend = ExportController::Get().BackendLabel(s.preset);
            if (!backend.empty())
                pending.label += " [" + backend + "]";
            return pending;
        }

        // Starts the next queued export when the controller is free.
        void Service()
        {
            ExportController& controller = ExportController::Get();
            // A project export opens its clips itself; a clip range needs its clip.
            if (g_queue.empty() || controller.Busy() || (!g_queue.front().project && !Flag(SnapshotPlaying)))
                return;
            const std::uint64_t now = GetTickCount64();
            if (now < g_requestedAt + 1500)
                return; // the probe's state follows a command a tick later
            Pending next = g_queue.front();
            std::uint32_t requestSeq = 0;
            if (next.project ? !controller.RequestProjectExport(next.settings, next.titles, requestSeq)
                             : !controller.RequestExport(next.settings, next.chapters, requestSeq, next.identity, next.resume ? &next.offer : nullptr))
            {
                if (next.resume && !controller.Busy())
                {
                    // The unfinished export changed since it was offered: not started.
                    g_queue.erase(g_queue.begin());
                    g_note = TR("export.resume.gone", "The unfinished export can no longer be continued");
                    g_noteUntil = GetTickCount64() + 4000;
                }
                return;
            }
            g_queue.erase(g_queue.begin());
            Command command{};
            command.keyframeId = requestSeq;
            command.enabled = next.project ? 1u : 0u; // 1: the whole project, one clip after the other
            Send(CommandType::StartExport, &command);
            g_lastLabel = next.label;
            g_requestedAt = now;
        }

        // Where the motion blur will take fewer samples (the markers' slow motion, or
        // the engine's minimum step at full speed), told before the export starts.
        // The probe decides per frame with the game's own values; this is the same
        // rule on the markers and the playback speed (a recorded Dead Eye or kill cam
        // slow motion is not in the snapshot: the export log names those spans).
        std::vector<replay::exporting::BlurSpan> BlurPlan(std::uint64_t startMs, std::uint64_t endMs)
        {
            std::vector<replay::exporting::MarkerSpeed> markers;
            if (Flag(SnapshotCameraTrackEnabled))
            {
                for (std::uint32_t k = 0; k < g_snapshot.cameraKeyframeCount; ++k)
                {
                    const CameraKeyframe& key = g_snapshot.cameraKeyframes[k];
                    if (key.markerType == MarkerType::Anchor)
                        continue;
                    const std::uint32_t index = std::min<std::uint32_t>(key.speedIndex, kMarkerSpeedCount - 1);
                    markers.push_back({key.timeMs, static_cast<double>(kMarkerSpeedPercent[index]) / 100.0});
                }
            }
            const double global = g_snapshot.speedPercent ? static_cast<double>(g_snapshot.speedPercent) / 100.0 : 1.0;
            const std::vector<replay::exporting::SpeedSpan> spans = replay::exporting::SpeedSpansFromMarkers(markers, startMs, endMs, global);
            return replay::exporting::PlanBlurSpans(kRates[g_rate], {kSamples[g_samples], kShutters[g_shutter]}, spans,
                replay::exporting::kEngineMinStepS);
        }

        // A whole project's progress: the pieces done and the current one's
        // frames against the whole (between two pieces the next one's count is 0).
        double ProjectFraction(const render_export::ExportStatus& status)
        {
            return replay::projectexport::OverallFraction(status.projectFramesBefore,
                status.state == ec::State::SegmentDone ? 0 : status.framesEncoded, status.projectFramesTotal);
        }

        // The resume offer: continue the unfinished export, or start over
        // (its segments deleted). Esc closes it without queueing anything.
        void DrawResumeOffer(ImDrawList* draw, float top)
        {
            ExportController& controller = ExportController::Get();
            const render_export::ResumeOffer& offer = g_offer.offer;
            float y = top + 130.0f;
            Text(draw, 80.0f, y, 26.0f, kGold, TR("export.resume.title", "Unfinished export found"));
            y += 40.0f;
            const std::string text = offer.done >= offer.count
                ? std::string(TR("export.resume.text_joined", "An export of this clip with these settings has all its segments done but was not joined. Continue it, or start over?"))
                : TRF("export.resume.text", "An export of this clip with these settings stopped after {done} of {count} segments. Continue it, or start over?",
                      {{"done", std::to_string(offer.done)}, {"count", std::to_string(offer.count)}});
            y += Paragraph(draw, 80.0f, y, 20.0f, kText, text.c_str(), 440.0f) + 20.0f;
            const std::string rows[2] = {TR("export.resume.continue", "Continue the export"), TR("export.resume.restart", "Start over (its segments are deleted)")};
            NavigateList(g_offer.index, 2);
            bool activate = Confirmed();
            for (int n = 0; n < 2; ++n)
            {
                const float rowY = y + n * 42.0f;
                bool clicked = false, pointed = false;
                ImGui::PushID(200 + n);
                ListItem(draw, "##resume", 80.0f, rowY, 440.0f, 38.0f, n == g_offer.index, clicked, &pointed);
                ImGui::PopID();
                if (pointed)
                    g_offer.index = n;
                if (clicked)
                {
                    g_offer.index = n;
                    activate = true;
                }
                RowTextFit(draw, 88.0f, rowY + 6.0f, 26.0f, kGold, rows[n].c_str(), 424.0f);
            }
            if (activate)
            {
                PlayUi(UiSound::Select);
                Pending pending = g_offer.pending;
                if (g_offer.index == 0)
                {
                    pending.resume = true;
                    pending.offer = offer;
                }
                else
                {
                    controller.DropResumable(offer);
                }
                const bool startNow = g_offer.startNow;
                g_offer = {};
                g_queue.push_back(pending);
                g_requestedAt = 0;
                if (startNow)
                    Service();
                return;
            }
            if (Back())
            {
                PlayUi(UiSound::Back);
                g_offer = {};
            }
        }

        // "Segment 2 of 3: frame 120 of 480" ("Segment 2 of 3: opening the clip..." between pieces).
        std::string ProjectProgressText(const render_export::ExportStatus& status)
        {
            const bool between = status.state == ec::State::SegmentDone;
            // Between two pieces the probe names the one opening.
            const std::uint32_t shown = std::min(status.segmentIndex, status.segmentCount - 1) + 1;
            if (between)
                return TRF("export.segment_opening", "Segment {n} of {count}: opening the clip...",
                    {{"n", std::to_string(shown)}, {"count", std::to_string(status.segmentCount)}});
            return TRF("export.segment_of", "Segment {n} of {count}: frame {done} of {total}",
                {{"n", std::to_string(shown)}, {"count", std::to_string(status.segmentCount)}, {"done", std::to_string(status.framesEncoded)},
                    {"total", std::to_string(status.framesTotalHint)}});
        }

        const char* StateText(ec::State state)
        {
            switch (state)
            {
            case ec::State::Requested: return TR("export.state.requested", "Preparing the scene...");
            case ec::State::Arming: return TR("export.state.arming", "Starting the encoder...");
            case ec::State::Preroll: return TR("export.state.preroll", "Warming up the world...");
            case ec::State::Capture: return TR("export.state.capture", "Exporting video");
            case ec::State::Flush: return TR("export.state.flush", "Last frames...");
            case ec::State::Finishing: return TR("export.state.finishing", "Finishing the file...");
            case ec::State::AudioSeek: return TR("export.state.audio_seek", "Audio pass: seeking...");
            case ec::State::AudioPass: return TR("export.state.audio_pass", "Audio pass (real time)");
            case ec::State::AudioDone: return TR("export.state.audio_done", "Muxing audio...");
            case ec::State::SegmentDone: return TR("export.state.segment_done", "Next clip of the project...");
            case ec::State::Done: return TR("export.state.done", "Done");
            case ec::State::Failed: return TR("export.state.failed", "Failed");
            case ec::State::Cancelled: return TR("export.state.cancelled", "Cancelled");
            default: return TR("export.state.ready", "Ready");
            }
        }
}

    void DrawExport(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("Export");
        using namespace exportui;
        ExportController& controller = ExportController::Get();
        if (!g_configLoaded)
        {
            g_configLoaded = true;
            wchar_t folder[MAX_PATH]{};
            GetCurrentDirectoryW(MAX_PATH, folder);
            const std::wstring ini = replay::identity::IniPathInW(folder);
            wchar_t lut[MAX_PATH]{};
            replay::identity::ProfileStringW(L"Export", L"Lut", L"", lut, MAX_PATH, ini.c_str());
            g_lutPath = lut;
        }
        const float columnWidth = 600.0f;
        draw->AddRectFilled(P(0.0f, 0.0f), P(columnWidth - 60.0f, g_vh), IM_COL32(10, 9, 8, 225));
        draw->AddRectFilledMultiColor(P(columnWidth - 60.0f, 0.0f), P(columnWidth + 220.0f, g_vh), IM_COL32(10, 9, 8, 225),
            IM_COL32(10, 9, 8, 0), IM_COL32(10, 9, 8, 0), IM_COL32(10, 9, 8, 225));
        const float top = std::max(50.0f, g_vh * 0.10f);
        const char* const title = TR("export.title", "EXPORT VIDEO");
        TitleBanner(draw, 50.0f, top, 330.0f, 70.0f, 62.0f, 72.0f, top + 10.0f, 44.0f, title); // as wide as a long title
        OrnamentDivider(draw, 60.0f, top + 100.0f, 460.0f);

        const render_export::ExportStatus status = controller.Status();
        const bool running = controller.Busy();
        if (!controller.Enabled() || !status.channel)
        {
            Paragraph(draw, 80.0f, top + 130.0f, 22.0f, kText,
                TR("export.off", "Video export is off. Set [Export] Enabled=1 in RedemptionEditor.ini, then restart the game."),
                440.0f);
            if (Back())
            {
                PlayUi(UiSound::Back);
                g_screen = g_back;
            }
            return;
        }

        if (running)
        {
            const float y = top + 130.0f;
            // A loading screen: the export stands still until the scene is
            // back (src/export_hold.h).
            const char* const stateText = status.held ? TR("export.state.held", "Paused: loading screen") : StateText(status.state);
            Text(draw, 80.0f, y, 30.0f, kText, stateText);
            LoadingAfter(draw, 80.0f, y, 30.0f, stateText, 520.0f);
            TextFit(draw, 80.0f, y + 44.0f, 20.0f, kMuted, g_lastLabel.c_str(), 440.0f);
            const double total = static_cast<double>(std::max<std::uint64_t>(status.framesTotalHint, 1));
            float fraction = static_cast<float>(std::min(1.0, static_cast<double>(status.framesEncoded) / total));
            const bool project = status.segmentCount > 0;
            float barWidth = 420.0f;
            if (project)
            {
                // A whole project: the bar and its percent (after it) are the whole's.
                fraction = static_cast<float>(ProjectFraction(status));
                barWidth = 350.0f;
                Text(draw, 444.0f, y + 80.0f, 20.0f, kGold, (std::to_string(replay::projectexport::PercentOf(fraction)) + "%").c_str());
            }
            draw->AddRectFilled(P(80.0f, y + 84.0f), P(80.0f + barWidth, y + 100.0f), kTrack);
            draw->AddRectFilled(P(80.0f, y + 84.0f), P(80.0f + barWidth * fraction, y + 100.0f), kRed);
            char line[160];
            if (project)
                TextFit(draw, 80.0f, y + 112.0f, 22.0f, kText, ProjectProgressText(status).c_str(), 440.0f);
            else if (status.fileSegments > 0)
                TextFit(draw, 80.0f, y + 112.0f, 22.0f, kText, TRF("export.segment_of", "Segment {n} of {count}: frame {done} of {total}",
                    {{"n", std::to_string(std::max<std::uint32_t>(status.fileSegment, 1))}, {"count", std::to_string(status.fileSegments)},
                        {"done", std::to_string(status.framesEncoded)}, {"total", std::to_string(status.framesTotalHint)}}).c_str(), 440.0f);
            else
                Text(draw, 80.0f, y + 112.0f, 22.0f, kText, TRF("export.frame_of", "Frame {done} of {total}",
                    {{"done", std::to_string(status.framesEncoded)}, {"total", std::to_string(status.framesTotalHint)}}).c_str());
            char elapsed[24]{}, left[24]{};
            FormatClock(static_cast<std::uint64_t>(status.secondsElapsed * 1000.0), elapsed, sizeof(elapsed), false);
            if (status.secondsLeft >= 0.0)
                FormatClock(static_cast<std::uint64_t>(status.secondsLeft * 1000.0), left, sizeof(left), false);
            Text(draw, 80.0f, y + 144.0f, 22.0f, kText, TRF("export.elapsed", "Elapsed {elapsed}   Time left {left}",
                {{"elapsed", elapsed}, {"left", status.secondsLeft >= 0.0 ? left : "--:--"}}).c_str());
            // The stage timings are for developers (Debug=1 in RedemptionEditor.ini);
            // the product shows progress only.
            if (g_hasSnapshot && Flag(SnapshotDebugMode))
            {
                const render_export::GpuTimings& t = status.timings;
                std::snprintf(line, sizeof(line), "GPU copy %.2f  blur %.2f  convert %.2f  readback %.2f ms", t.copyUs / 1000.0,
                    t.accumulateUs / 1000.0, t.resolveUs / 1000.0, t.readbackUs / 1000.0);
                Text(draw, 80.0f, y + 184.0f, 18.0f, kMuted, line);
                std::snprintf(line, sizeof(line), "CPU map %.2f ms   encoder queue %u   latency %d", t.mapUs / 1000.0, status.queueDepth,
                    status.latencyPresents == ~0u ? -1 : static_cast<int>(status.latencyPresents));
                Text(draw, 80.0f, y + 210.0f, 18.0f, kMuted, line);
            }
            if (!g_queue.empty())
            {
                Text(draw, 80.0f, y + 240.0f, 18.0f, kGold, TRF("export.queue_more", "{count} more in the queue", {{"count", std::to_string(g_queue.size())}}).c_str());
            }
            ImGui::SetCursorScreenPos(P(80.0f, y + 280.0f));
            const bool cancelClicked = ItemClicked(ImGui::InvisibleButton("##exportcancel", ImVec2(S(220.0f), S(36.0f))));
            Brush(draw, 72.0f, y + 280.0f, 220.0f, 36.0f, ImGui::IsItemHovered() ? kRed : kRedDim);
            Text(draw, 84.0f, y + 286.0f, 20.0f, kText, TR("export.cancel", "Cancel export"));
            if (cancelClicked || KeyPressed(ImGuiKey_Delete, false))
            {
                PlayUi(UiSound::Back);
                g_queue.clear();
                controller.Cancel();
                Send(CommandType::CancelExport);
            }
            // Its keys are on the key bar (ExportTips).
            return;
        }

        if (g_offer.active)
        {
            DrawResumeOffer(draw, top);
            return;
        }

        // Settings. The encoders of this PC are probed once (FFmpeg DLLs
        // loaded, every variant opened); the formats without one are skipped.
        controller.StartProbe();
        const bool probed = controller.ProbeDone();
        const int presetCount = static_cast<int>(std::size(kPresets));
        if (probed && !controller.PresetAvailable(kPresets[g_preset]))
        {
            for (int k = 1; k < presetCount; ++k)
            {
                const int candidate = (g_preset + k) % presetCount;
                if (controller.PresetAvailable(kPresets[candidate]))
                {
                    g_preset = candidate;
                    break;
                }
            }
        }
        const render_export::PresetInfo info = render_export::InfoOf(kPresets[g_preset]);
        const std::string backend = controller.BackendLabel(kPresets[g_preset]);
        const bool presetUsable = !probed || controller.PresetAvailable(kPresets[g_preset]);
        std::uint64_t startMs = 0, endMs = 0;
        Range(startMs, endMs);
        std::string rows[12];
        const char* formatLabel = TR(info.labelKey, info.label);
        if (!probed)
            rows[0] = TRF("export.format.checking", "Format: {format} (checking encoders...)", {{"format", formatLabel}});
        else if (!backend.empty())
            rows[0] = TRF("export.format.backend", "Format: {format} ({backend})", {{"format", formatLabel}, {"backend", backend}});
        else
            rows[0] = TRF("export.format", "Format: {format}", {{"format", formatLabel}});
        rows[1] = TRF("export.rate", "Frame rate: {rate} fps", {{"rate", kRateLabels[g_rate]}});
        {
            char a[24]{}, b[24]{};
            FormatClock(startMs, a, sizeof(a), true);
            FormatClock(endMs, b, sizeof(b), true);
            rows[2] = TRF("export.range", "Range: {range} ({start} - {end})",
                {{"range", g_range == 2 ? TR("export.range.project", "Whole project")
                        : g_range == 1  ? TR("export.range.markers", "Start to End markers")
                                        : TR("export.range.whole", "Whole clip")},
                    {"start", a}, {"end", b}});
        }
        if (kSamples[g_samples] > 1)
            rows[3] = TRF("export.blur", "Motion blur: {samples} samples", {{"samples", std::to_string(kSamples[g_samples])}});
        else
            rows[3] = TR("export.blur.off", "Motion blur: Off");
        rows[4] = TRF("export.shutter", "Shutter angle: {angle} deg", {{"angle", replay::i18n::Printf("%.0f", kShutters[g_shutter])}});
        rows[5] = TRF("export.letterbox", "Letterbox: {value}", {{"value", TR(kLetterbox[g_letterbox].key, kLetterbox[g_letterbox].label)}});
        rows[6] = TRF("export.grain", "Film grain: {value}", {{"value", TR(kGrain[g_grain].key, kGrain[g_grain].label)}});
        rows[7] = TRF("export.lut", "LUT: {value}", {{"value", g_lut && !g_lutPath.empty() ? TR("common.on_cap", "On")
                    : g_lutPath.empty() ? TR("export.lut.none", "None ([Export] Lut)") : TR("common.off_cap", "Off")}});
        rows[8] = TRF("export.audio", "Audio: {value}", {{"value", g_audio ? TR("export.audio.pass", "Real-time pass") : TR("export.audio.none", "None")}});
        rows[9] = TRF("export.chapters", "Markers as chapters: {value}", {{"value", g_chapters ? TR("common.on_cap", "On") : TR("common.off_cap", "Off")}});
        rows[10] = TR("export.now", "Export now");
        rows[11] = TRF("export.add_queue", "Add to queue ({count})", {{"count", std::to_string(g_queue.size())}});
        const int count = 12;
        NavigateList(g_index, count);
        int direction = 0;
        if (KeyPressed(ImGuiKey_RightArrow))
            direction = 1;
        if (KeyPressed(ImGuiKey_LeftArrow))
            direction = -1;
        bool activate = Confirmed();
        const float itemsTop = top + 116.0f;
        for (int n = 0; n < count; ++n)
        {
            const float y = itemsTop + n * 42.0f;
            bool clicked = false, pointed = false;
            ImGui::PushID(n);
            ListItem(draw, "##export", 80.0f, y, 440.0f, 38.0f, n == g_index, clicked, &pointed);
            ImGui::PopID();
            if (pointed)
                g_index = n;
            if (clicked)
            {
                g_index = n;
                if (n < 10)
                    direction = 1;
                else
                    activate = true;
            }
            RowTextFit(draw, 88.0f, y + 6.0f, 26.0f, n >= 10 ? kGold : kText, rows[n].c_str(), 424.0f); // inside its row
        }
        if (activate && g_index < 10)
        {
            direction = 1;
            activate = false;
        }
        auto cycle = [direction](int value, int size) { return (value + direction + size) % size; };
        if (direction != 0)
        {
            PlayUi(direction < 0 ? UiSound::NavLeft : UiSound::NavRight);
            switch (g_index)
            {
            case 0:
                // Only the formats with an encoder on this PC.
                for (int k = 0; k < presetCount; ++k)
                {
                    g_preset = cycle(g_preset, presetCount);
                    if (controller.PresetAvailable(kPresets[g_preset]))
                        break;
                }
                break;
            case 1: g_rate = cycle(g_rate, static_cast<int>(std::size(kRates))); break;
            case 2: g_range = NextRange(g_range, direction); break;
            case 3: g_samples = cycle(g_samples, static_cast<int>(std::size(kSamples))); break;
            case 4: g_shutter = cycle(g_shutter, static_cast<int>(std::size(kShutters))); break;
            case 5: g_letterbox = cycle(g_letterbox, static_cast<int>(std::size(kLetterbox))); break;
            case 6: g_grain = cycle(g_grain, static_cast<int>(std::size(kGrain))); break;
            case 7: g_lut = !g_lut && !g_lutPath.empty(); break;
            case 8: g_audio = !g_audio; break;
            case 9: g_chapters = !g_chapters; break;
            default: break;
            }
        }
        const bool addKey = KeyPressed(ImGuiKey_A, false);
        if ((activate && (g_index == 10 || g_index == 11)) || addKey)
        {
            const bool project = g_range == 2;
            const int missing = project ? MissingProjectClip() : -1;
            if (!project && !Flag(SnapshotPlaying))
            {
                PlayUi(UiSound::Error);
                g_note = TR("export.note.open_clip", "Open a clip first"); g_noteUntil = GetTickCount64() + 2500;
            }
            else if (missing >= 0)
            {
                // Said before anything runs (the script refuses it too).
                PlayUi(UiSound::Error);
                g_note = TRF("export.note.clip_missing", "{clip} is not in ReplayClips: the project cannot be exported",
                    {{"clip", ClipLabel(g_snapshot.projectClips[missing].file)}});
                g_noteUntil = GetTickCount64() + 4000;
            }
            else if (!controller.ReShadeProblem().empty())
            {
                PlayUi(UiSound::Error);
                g_note = TR("export.note.reshade", "This ReShade version is not supported"); g_noteUntil = GetTickCount64() + 2500;
            }
            else if (!presetUsable || !controller.EncoderProblem().empty())
            {
                PlayUi(UiSound::Error);
                g_note = TR("export.note.no_encoder", "No encoder for this format"); g_noteUntil = GetTickCount64() + 2500;
            }
            else if (endMs <= startMs)
            {
                PlayUi(UiSound::Error);
                g_note = TR("export.note.empty_range", "The range is empty"); g_noteUntil = GetTickCount64() + 2500;
            }
            else
            {
                PlayUi(UiSound::Select);
                Pending pending = Build();
                render_export::ResumeOffer offer;
                if (!pending.project && controller.FindResumable(pending.settings, pending.identity, offer))
                {
                    // An unfinished segmented export of this very clip and settings: asked first.
                    g_offer.active = true;
                    g_offer.pending = std::move(pending);
                    g_offer.offer = offer;
                    g_offer.startNow = g_index == 10 && !addKey;
                    g_offer.index = 0;
                }
                else
                {
                    g_queue.push_back(std::move(pending));
                    g_requestedAt = 0;
                    if (g_index == 10 && !addKey)
                        Service(); // starts right away
                }
            }
        }

        // Right side: what will come out. Laid out in two passes, measured then
        // drawn: each block stands under the one before and the panel grows to them,
        // so a wrapped line never lands over the next one.
        const float rx = 640.0f;
        const float ry = top + 130.0f;
        constexpr float kInfoW = 480.0f;
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const std::uint32_t width = static_cast<std::uint32_t>(display.x);
        const std::uint32_t height = static_cast<std::uint32_t>(display.y);
        const double seconds = static_cast<double>(endMs - startMs) / 1000.0;
        const bool halved = kPresets[g_preset] == ec::VideoPreset::PreviewH264;
        const std::string resolution = TRF("export.resolution", "Resolution: {width}x{height} (the game's){halved}",
            {{"width", std::to_string(halved ? width / 2 : width)}, {"height", std::to_string(halved ? height / 2 : height)},
                {"halved", halved ? TR("export.resolution.halved", ", halved") : ""}});
        char size[32]{};
        FormatSize(render_export::EstimateBytes(kPresets[g_preset], width, height, kRates[g_rate], seconds), size, sizeof(size));
        const std::string length = TRF("export.length", "Length {seconds} s, about {size}", {{"seconds", replay::i18n::Printf("%.1f", seconds)}, {"size", size}});
        const std::uint32_t renders = kSamples[g_samples];
        const std::string frames = TRF("export.renders", "Game frames per video frame: {count}{slower}",
            {{"count", std::to_string(renders)}, {"slower", renders > 1 ? TR("export.renders.slower", " (slower, smoother motion)") : ""}});
        std::string problem = controller.ReShadeProblem();
        if (problem.empty())
            problem = controller.EncoderProblem();
        std::string encoder;
        if (!backend.empty())
            encoder = TRF("export.encoder", "Encoder: {encoder}", {{"encoder", backend == "CPU" ? std::string(TR("export.encoder.cpu", "CPU (slower)"))
                    : backend == "ffmpeg.exe" ? std::string("ffmpeg.exe") : backend + " (GPU)"}});
        // The blur reduced in slow motion, said before it starts.
        std::vector<std::string> blurLines;
        std::size_t moreSpans = 0;
        if (g_range != 2) // the open clip's markers (a project's other clips are not in the snapshot)
        {
            const std::vector<replay::exporting::BlurSpan> blur = BlurPlan(startMs, endMs);
            const std::uint32_t asked = kSamples[g_samples];
            const std::uint32_t atNormal = replay::exporting::SamplesForWorldScale(kRates[g_rate], {asked, kShutters[g_shutter]}, 0, 1.0,
                static_cast<std::int64_t>(replay::exporting::kEngineMinStepS * 1e9 + 0.5));
            if (atNormal < asked)
                blurLines.push_back(TRF("export.blur.capped", "Motion blur: {samples} samples per frame ({asked} would step below the game's minimum).",
                    {{"samples", std::to_string(atNormal)}, {"asked", std::to_string(asked)}}));
            std::size_t shown = 0;
            for (const replay::exporting::BlurSpan& span : blur)
            {
                if (span.worldScale >= 1.0)
                    continue; // the line above
                if (shown == 3)
                {
                    ++moreSpans;
                    continue;
                }
                ++shown;
                const std::string from = replay::i18n::Printf("%.1f", span.outStartS), to = replay::i18n::Printf("%.1f", span.outEndS);
                if (span.frameBelowMinimum)
                    blurLines.push_back(TRF("export.blur.below", "Slow motion at {from} to {to} s: no blur, and the world runs a little fast there (below the game's minimum step).",
                        {{"from", from}, {"to", to}}));
                else if (span.samples == 1)
                    blurLines.push_back(TRF("export.blur.none", "Slow motion at {from} to {to} s: blur reduced to 1 sample (none).", {{"from", from}, {"to", to}}));
                else
                    blurLines.push_back(TRF("export.blur.reduced", "Slow motion at {from} to {to} s: blur reduced to {samples} samples.",
                        {{"from", from}, {"to", to}, {"samples", std::to_string(span.samples)}}));
            }
        }
        std::string detail;
        if (!status.detail.empty())
            detail = std::string(StateText(status.state)) + ": " + status.detail;
        else if (status.state == ec::State::Failed && status.error == render_export::ExportError::ProjectClipMissing)
            detail = std::string(StateText(status.state)) + ": " +
                TR("export.project.missing", "a clip of the project is not in ReplayClips (see the project screen). Nothing was exported.");
        else if (status.state == ec::State::Failed && status.error == render_export::ExportError::ProjectClipLoad)
            detail = std::string(StateText(status.state)) + ": " +
                TR("export.project.load", "a clip of the project could not be opened (see the project screen). The pieces exported were deleted.");
        const auto layout = [&](ImDrawList* d) {
            float y = ry;
            const auto label = [&](float atLeast, float sizeUnits, ImU32 color, const char* text) {
                y = std::max(y, atLeast);
                y += LabelFit(d, rx, y, sizeUnits, color, text, kInfoW) + 6.0f;
            };
            const auto paragraph = [&](float atLeast, float sizeUnits, ImU32 color, const char* text) {
                y = std::max(y, atLeast);
                y += (d ? Paragraph(d, rx, y, sizeUnits, color, text, kInfoW) : ParagraphHeight(sizeUnits, text, kInfoW)) + 6.0f;
            };
            label(ry, 24.0f, kGold, formatLabel);
            paragraph(ry + 34.0f, 18.0f, kMuted, TR(info.noteKey, info.note));
            label(ry + 80.0f, 20.0f, kText, resolution.c_str());
            label(ry + 110.0f, 20.0f, kText, length.c_str());
            label(ry + 140.0f, 20.0f, kText, frames.c_str());
            if (!problem.empty())
                paragraph(ry + 170.0f, 16.0f, kRed, problem.c_str());
            else if (!presetUsable)
                label(ry + 170.0f, 20.0f, kRed, TR("export.no_encoder_pc", "No encoder for this format on this PC."));
            else if (!encoder.empty())
                label(ry + 170.0f, 20.0f, kText, encoder.c_str());
            paragraph(ry + 200.0f, 16.0f, kMuted,
                TR("export.how", "The game renders frame by frame on a locked clock: nothing stutters, whatever the speed of the PC. The game's sound is "
                "muted during the video pass and recorded in a second, real-time pass."));
            if (!blurLines.empty() || moreSpans)
                y = std::max(y, ry + 270.0f); // the blur notes where they always start
            for (const std::string& blurLine : blurLines)
                paragraph(y, 16.0f, kGold, blurLine.c_str());
            if (moreSpans)
            {
                label(y, 16.0f, kGold, TRF("export.blur.more", "... and {count} more slow motion spans.", {{"count", std::to_string(moreSpans)}}).c_str());
            }
            if (!detail.empty())
                paragraph(std::max(ry + 290.0f, y + 2.0f), 16.0f, status.state == ec::State::Done ? kGold : kRed, detail.c_str());
            return y;
        };
        // Fitted to the content as drawn (the last line's 6-unit gap replaced by the
        // panel's foot), like the other text panels.
        const float panelBottom = layout(nullptr) - 6.0f + kPanelPadBottom;
        InkPanel(draw, rx - kPanelPad, ry - kPanelPad, rx + kInfoW + kPanelPad, panelBottom, kPanel); // taller: the blur notes
        layout(draw);
        UI_PROBE_BOX("endpanel", P(0.0f, 0.0f), P(0.0f, 0.0f));
        if (GetTickCount64() < g_noteUntil)
            Text(draw, 80.0f, g_vh - g_footH - 50.0f, 22.0f, kRed, g_note.c_str());
        if (Back())
        {
            PlayUi(UiSound::Back);
            g_screen = g_back;
        }
    }
