// Render export: the add-on side controller (export_controller.h). Nothing
// here runs unless RedemptionEditor.ini [Export] Enabled=1 and the probe's
// channel exists. The FFmpeg DLLs load the first time the export dialog
// opens (or the first export starts), never at the core's load.

#include "export_controller.h"
#include "i18n.h" // the dialog's problem lines in the player's language
#include "mod_identity.h" // RedemptionEditor.ini (the old ReplayProbe.ini is read too)
#include "thread_names.h"

#include <ShlObj.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

namespace render_export
{
    using namespace reshade::api;
    namespace ec = replay::exportchannel;
    namespace ex = replay::exporting;

    namespace
    {
        // The game window during an export (src/export_hold.h): a close
        // request (WM_CLOSE sent or posted, as taskkill and the close button
        // send it, WM_SYSCOMMAND SC_CLOSE, Alt+F4), counted into the channel
        // from the window's own thread before the game handles it; the probe
        // cancels the export. Two thread hooks (sent and posted messages),
        // set at the first Arm, removed at Shutdown; they only count.
        std::atomic<ec::Channel*> g_hookChannel{nullptr};
        std::atomic<int> g_inHook{0};
        std::atomic<HWND> g_hookWindow{nullptr};
        HHOOK g_callHook = nullptr;
        HHOOK g_messageHook = nullptr;

        // Inside a hook procedure (g_inHook held by the caller).
        void CountClose()
        {
            if (ec::Channel* channel = g_hookChannel.load())
                channel->closeRequests.fetch_add(1);
        }

        bool CloseMessage(UINT message, WPARAM wParam)
        {
            return message == WM_CLOSE || (message == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_CLOSE);
        }

        LRESULT CALLBACK CallWindowHook(int code, WPARAM wParam, LPARAM lParam)
        {
            g_inHook.fetch_add(1);
            if (code == HC_ACTION && lParam)
            {
                const auto* message = reinterpret_cast<const CWPSTRUCT*>(lParam);
                if (CloseMessage(message->message, message->wParam))
                    CountClose();
            }
            const LRESULT result = CallNextHookEx(nullptr, code, wParam, lParam);
            g_inHook.fetch_sub(1);
            return result;
        }

        LRESULT CALLBACK GetMessageHook(int code, WPARAM wParam, LPARAM lParam)
        {
            g_inHook.fetch_add(1);
            if (code == HC_ACTION && wParam == PM_REMOVE && lParam)
            {
                const auto* message = reinterpret_cast<const MSG*>(lParam);
                if (CloseMessage(message->message, message->wParam) ||
                    (message->message == WM_SYSKEYDOWN && message->wParam == VK_F4 && (message->lParam & (1 << 29))))
                    CountClose();
            }
            const LRESULT result = CallNextHookEx(nullptr, code, wParam, lParam);
            g_inHook.fetch_sub(1);
            return result;
        }

        // Empty when hooked (or already), else why not.
        std::string HookWindow(HWND window, ec::Channel* channel)
        {
            g_hookChannel.store(channel);
            if (!window)
                return "no_window";
            if (g_hookWindow.load() == window && g_callHook && g_messageHook)
                return {};
            if (g_callHook)
                UnhookWindowsHookEx(g_callHook);
            if (g_messageHook)
                UnhookWindowsHookEx(g_messageHook);
            g_callHook = g_messageHook = nullptr;
            g_hookWindow.store(window);
            const DWORD thread = GetWindowThreadProcessId(window, nullptr);
            g_callHook = SetWindowsHookExW(WH_CALLWNDPROC, CallWindowHook, nullptr, thread);
            g_messageHook = SetWindowsHookExW(WH_GETMESSAGE, GetMessageHook, nullptr, thread);
            if (g_callHook && g_messageHook)
                return {};
            return "hook_failed_" + std::to_string(GetLastError());
        }

        // Shutdown: the hooks off, then no hook is left inside a count before
        // the channel is unmapped.
        void UnhookWindow()
        {
            if (g_callHook)
                UnhookWindowsHookEx(g_callHook);
            if (g_messageHook)
                UnhookWindowsHookEx(g_messageHook);
            g_callHook = g_messageHook = nullptr;
            g_hookWindow.store(nullptr);
            g_hookChannel.store(nullptr);
            while (g_inHook.load() != 0)
                Sleep(0);
            Sleep(1); // a procedure past its count returns out of this module
        }

        std::wstring IniPath()
        {
            wchar_t folder[MAX_PATH]{};
            GetCurrentDirectoryW(MAX_PATH, folder);
            return replay::identity::IniPathInW(folder);
        }

        std::string Utf8(const std::wstring& text)
        {
            if (text.empty())
                return {};
            const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
            return out;
        }

        bool Idle(ec::State state)
        {
            return state == ec::State::Off || state == ec::State::Done || state == ec::State::Failed || state == ec::State::Cancelled;
        }

        double SecondsSince(const LARGE_INTEGER& start)
        {
            LARGE_INTEGER now{}, frequency{};
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            return static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);
        }

        // "C:\x\clip.mp4" -> "C:\x\clip.video.mp4" (the video pass's file).
        std::wstring TempVideoPath(const std::wstring& final)
        {
            const std::size_t dot = final.find_last_of(L'.');
            const std::size_t slash = final.find_last_of(L"\\/");
            if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
                return final + L".video.mkv";
            return final.substr(0, dot) + L".video" + final.substr(dot);
        }

        std::wstring WithoutExtension(const std::wstring& path)
        {
            const std::size_t dot = path.find_last_of(L'.');
            const std::size_t slash = path.find_last_of(L"\\/");
            return dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash) ? path : path.substr(0, dot);
        }

        // An image sequence's folder: its PNGs, then the folder.
        void DeleteFolder(const std::wstring& folder)
        {
            WIN32_FIND_DATAW entry{};
            HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &entry);
            if (find != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                        DeleteFileW((folder + L"\\" + entry.cFileName).c_str());
                } while (FindNextFileW(find, &entry));
                FindClose(find);
            }
            RemoveDirectoryW(folder.c_str());
        }

        // A piece's file (or an image sequence's folder) and what the export
        // left next to it.
        void DeletePath(const std::wstring& path)
        {
            if (path.empty())
                return;
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
                return;
            if (attributes & FILE_ATTRIBUTE_DIRECTORY)
                DeleteFolder(path);
            else
                DeleteFileW(path.c_str());
            DeleteFileW((path + L".ffmpeg.log").c_str());
        }

        std::wstring Wide(const std::string& text)
        {
            if (text.empty())
                return {};
            const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring out(static_cast<std::size_t>(size), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
            return out;
        }

        // A segmented export's folder: "C:\x\clip.mp4" -> "C:\x\clip.mp4.parts".
        std::wstring PartsFolder(const std::wstring& final) { return final + L".parts"; }

        // ".mp4" (UTF-8), the segments' extension.
        std::string ExtensionOf(const std::wstring& path)
        {
            const std::size_t dot = path.find_last_of(L'.');
            const std::size_t slash = path.find_last_of(L"\\/");
            return dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash) ? std::string(".mkv") : Utf8(path.substr(dot));
        }

        bool ReadSmallFile(const std::wstring& path, std::string& out)
        {
            out.clear();
            HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;
            LARGE_INTEGER size{};
            bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (16 << 20);
            if (ok)
            {
                out.resize(static_cast<std::size_t>(size.QuadPart));
                DWORD read = 0;
                ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size();
            }
            CloseHandle(file);
            return ok;
        }

        bool ReadManifest(const std::wstring& folder, segments::Manifest& manifest, std::string& why)
        {
            std::string text;
            if (!ReadSmallFile(folder + L"\\manifest.txt", text))
            {
                why = "no manifest";
                return false;
            }
            return segments::Parse(text, manifest, why);
        }

        std::uint64_t FileBytes(const std::wstring& path)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
                return 0;
            return (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        }
    }

    ExportController& ExportController::Get()
    {
        static ExportController controller;
        return controller;
    }

    void ExportController::Log(const std::string& line) const
    {
        reshade::log::message(reshade::log::level::info, line.c_str());
        // Next to the video as well (ReShade's log is shared with everything
        // else).
        if (!m_logPath.empty())
        {
            const std::wstring& path = m_logPath;
            HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                const std::string text = line + "\r\n";
                DWORD written = 0;
                WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
                CloseHandle(file);
            }
        }
    }

    void ExportController::LogMemory(const char* reason, std::uint64_t frame, std::uint32_t inflight, std::uint32_t queue)
    {
        MemorySample sample;
        {
            std::lock_guard<std::mutex> lock(m_memoryMutex);
            sample = m_memory.Sample();
            m_memoryPeaks.Add(sample);
        }
        const std::uint64_t of = m_channel ? m_channel->framesTotalHint.load() : 0;
        Log(FormatMemLine(sample, frame, of, inflight, queue, reason));
    }

    void ExportController::LoadConfig()
    {
        const std::wstring ini = IniPath();
        m_enabled = replay::identity::ProfileIntW(L"Export", L"Enabled", 1, ini.c_str()) != 0; // on by default
        m_hdr10 = replay::identity::ProfileIntW(L"Export", L"Hdr10", 0, ini.c_str()) != 0;
        // Segments of this many seconds of video (0: one file, as before).
        m_segmentSeconds = segments::ClampSeconds(static_cast<int>(replay::identity::ProfileIntW(L"Export", L"SegmentSeconds", 60, ini.c_str())));
        {
            // Read as text: GetPrivateProfileInt drops negative values.
            // Default 140 ms: with carbine shots the game's sound came 131 to 148 ms
            // after the muzzle flash frame; with 140 the shots land 3 to 18 ms from
            // their flash, inside one frame at 60 fps.
            wchar_t offset[32]{};
            replay::identity::ProfileStringW(L"Export", L"AudioOffsetMs", L"140", offset, 32, ini.c_str());
            m_audioOffsetMs = _wtoi(offset);
        }
        wchar_t text[MAX_PATH]{};
        replay::identity::ProfileStringW(L"Export", L"Encoder", L"auto", text, MAX_PATH, ini.c_str());
        ParseBackendChoice(text, m_automaticBackend, m_forcedBackend);
        // The FFmpeg DLLs: always <game folder>\RedemptionEditor\dlls.
        m_dllFolder = DefaultFfmpegDllFolder();
        text[0] = 0;
        replay::identity::ProfileStringW(L"Export", L"Folder", L"", text, MAX_PATH, ini.c_str());
        m_folder = text;
        if (m_folder.empty())
        {
            PWSTR documents = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)) && documents)
            {
                m_folder = std::wstring(documents) + L"\\Rockstar Games\\Red Dead Redemption 2\\Videos\\Redemption Editor";
                CoTaskMemFree(documents);
            }
        }
    }

    bool ExportController::EnsureRuntime()
    {
        std::string error;
        if (LoadFfmpegRuntime(m_dllFolder, error))
            return true;
        std::lock_guard<std::mutex> lock(m_probeMutex);
        m_runtimeError = error;
        return false;
    }

    void ExportController::StartProbe()
    {
        std::lock_guard<std::mutex> lock(m_probeMutex);
        if (m_probeStarted)
            return;
        m_probeStarted = true;
        m_probeThread = std::thread([this] {
            replay::threads::Scope threadName("export_probe");
            const bool loaded = EnsureRuntime();
            ProbeResult result = loaded ? ProbeEncoders() : ProbeResult{};
            result.runtimeLoaded = loaded;
            result.done = true;
            std::string line = "RENDER_EXPORT,phase=encoders,ffmpeg=" + (loaded ? FfmpegVersionText() : std::string("missing"));
            if (!loaded)
            {
                std::lock_guard<std::mutex> lock(m_probeMutex);
                result.runtimeError = m_runtimeError;
                line += ",error=" + m_runtimeError;
            }
            for (std::size_t i = 0; i < result.opened.size(); ++i)
            {
                const EncoderVariant& v = AllVariants()[i];
                line += std::string(",") + v.codec + "@" + std::to_string(static_cast<unsigned>(v.preset)) + "=" + (result.opened[i] ? "1" : "0");
            }
            reshade::log::message(reshade::log::level::info, line.c_str());
            std::lock_guard<std::mutex> lock(m_probeMutex);
            m_probe = std::move(result);
        });
    }

    bool ExportController::ProbeDone() const
    {
        std::lock_guard<std::mutex> lock(m_probeMutex);
        return m_probe.done;
    }

    int ExportController::VariantFor(ec::VideoPreset preset) const
    {
        std::lock_guard<std::mutex> lock(m_probeMutex);
        if (!m_probe.done)
            return -1;
        return ChooseVariant(preset, m_probe, m_automaticBackend, m_forcedBackend);
    }

    bool ExportController::PresetAvailable(ec::VideoPreset preset) const
    {
        {
            std::lock_guard<std::mutex> lock(m_probeMutex);
            if (!m_probe.done)
                return true; // not known yet
        }
        return VariantFor(preset) >= 0;
    }

    std::string ExportController::BackendLabel(ec::VideoPreset preset) const
    {
        const int variant = VariantFor(preset);
        return variant < 0 ? std::string() : std::string(BackendName(AllVariants()[static_cast<std::size_t>(variant)].backend));
    }

    std::string ExportController::EncoderProblem() const
    {
        std::lock_guard<std::mutex> lock(m_probeMutex);
        if (!m_probe.done || m_probe.runtimeLoaded)
            return {};
        return TRF("export.problem.ffmpeg", "The FFmpeg libraries did not load ({error}). Reinstall the RedemptionEditor folder from the release zip.",
            {{"error", m_probe.runtimeError}});
    }

    std::string ExportController::ReShadeProblem() const
    {
        std::string version;
        if (DetectReShadeQueryAbi(version) != QueryAbi::Unknown)
            return {};
        return TRF("export.problem.reshade", "Video export does not support ReShade {version}. Install ReShade 6.6.2 to 6.8.x with full add-on support.",
            {{"version", version.empty() ? std::string(TR("export.problem.reshade.unknown", "(version unknown)")) : version}});
    }

    bool ExportController::OpenChannel()
    {
        if (m_channel)
            return true;
        if (++m_openRetry % 60 != 1)
            return false; // once a second at 60 fps
        wchar_t name[64];
        ec::Name(name, GetCurrentProcessId());
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
        if (!mapping)
            return false;
        void* view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(ec::Channel));
        auto* channel = static_cast<ec::Channel*>(view);
        if (!channel || channel->magic != ec::kMagic || channel->version != ec::kVersion ||
            channel->bytes != static_cast<std::uint32_t>(sizeof(ec::Channel)))
        {
            if (view)
                UnmapViewOfFile(view);
            CloseHandle(mapping);
            return false;
        }
        m_mapping = mapping;
        m_channel = channel;
        return true;
    }

    bool ExportController::Busy() const
    {
        if (m_armed || m_finishing)
            return true;
        return m_channel && !Idle(static_cast<ec::State>(m_channel->state.load()));
    }

    bool ExportController::RequestExport(const ec::Settings& settings, const std::vector<Chapter>& chapters, std::uint32_t& requestSeq,
        const std::string& identity, const ResumeOffer* resume)
    {
        if (!m_enabled || !OpenChannel() || Busy())
            return false;
        if (m_finisher.joinable())
            m_finisher.join();
        // A segmented export's manifest keeps what it was asked for: the
        // settings as the dialog gave them and what is exported.
        PendingSegments pending;
        pending.identity = identity;
        pending.settingsHash = segments::SettingsHash(settings, m_segmentSeconds, VariantFor(settings.preset), m_hdr10);
        ec::Settings asked = settings;
        std::uint32_t resumeFlag = 0;
        std::uint64_t resumeStartMs = 0, resumeFrames = 0;
        if (resume)
        {
            // Read again (the dialog's offer may be old): it must still match.
            segments::Manifest manifest;
            segments::ResumePlan plan;
            std::string why;
            if (!ReadManifest(resume->folder, manifest, why) || segments::Check(manifest, pending.settingsHash, identity) != segments::ResumeCheck::Resumable ||
                !segments::PlanResume(manifest, plan))
            {
                Log("RENDER_EXPORT_SEG,phase=resume_refused,reason=" + (why.empty() ? std::string("manifest_changed") : why) + ",folder=" + Utf8(resume->folder));
                return false;
            }
            wcsncpy_s(asked.outputPath, Wide(manifest.finalOutput).c_str(), _TRUNCATE);
            if (manifest.audioDone)
                asked.audio = ec::AudioMode::None; // the audio pass is in the folder already
            pending.resume = true;
            pending.folder = resume->folder;
            resumeFlag = 1;
            resumeStartMs = plan.startMs;
            resumeFrames = plan.videoDone ? 0 : plan.frames;
        }
        m_channel->settings = asked;
        m_channel->resume.store(resumeFlag);
        m_channel->resumeStartMs.store(resumeStartMs);
        m_channel->resumeFrames.store(resumeFrames);
        m_channel->cancel.store(0);
        m_channel->error.store(0);
        m_chapters = chapters;
        requestSeq = m_channel->requestSeq.fetch_add(1) + 1;
        pending.valid = true;
        pending.requestSeq = requestSeq;
        m_pending = pending;
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_detail.clear();
        }
        return true; // the editor now sends CommandType::StartExport
    }

    bool ExportController::RequestProjectExport(const ec::Settings& settings, const std::vector<std::string>& clipTitles, std::uint32_t& requestSeq)
    {
        if (!RequestExport(settings, {}, requestSeq))
            return false;
        m_projectTitles = clipTitles;
        return true; // the editor now sends CommandType::StartExport with enabled 1
    }

    bool ExportController::FindResumable(const ec::Settings& settings, const std::string& identity, ResumeOffer& offer)
    {
        offer = {};
        if (!m_enabled || m_segmentSeconds == 0 || settings.preset == ec::VideoPreset::PngSequence || m_folder.empty())
            return false;
        const std::string hash = segments::SettingsHash(settings, m_segmentSeconds, VariantFor(settings.preset), m_hdr10);
        std::wstring base = m_folder;
        while (!base.empty() && (base.back() == L'\\' || base.back() == L'/'))
            base.pop_back();
        WIN32_FIND_DATAW entry{};
        HANDLE find = FindFirstFileW((base + L"\\*.parts").c_str(), &entry);
        if (find == INVALID_HANDLE_VALUE)
            return false;
        bool found = false;
        do
        {
            if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                continue;
            const std::wstring folder = base + L"\\" + entry.cFileName;
            segments::Manifest manifest;
            std::string why;
            if (!ReadManifest(folder, manifest, why))
                continue; // not ours, or cut short: left alone
            if (_wcsicmp(PartsFolder(Wide(manifest.finalOutput)).c_str(), folder.c_str()) != 0)
                continue; // moved or renamed: its export would write elsewhere
            switch (segments::Check(manifest, hash, identity))
            {
            case segments::ResumeCheck::Nothing:
                // This export again, and the old try never closed a segment.
                DeleteFolder(folder);
                Log("RENDER_EXPORT_SEG,phase=dropped,why=nothing_done,folder=" + Utf8(folder));
                break;
            case segments::ResumeCheck::Resumable:
            {
                const std::uint32_t done = segments::DoneSegments(manifest);
                if (!found || done > offer.done)
                {
                    found = true;
                    offer.folder = folder;
                    offer.finalOutput = Wide(manifest.finalOutput);
                    offer.done = done;
                    offer.count = manifest.SegmentCount();
                    offer.audioDone = manifest.audioDone;
                }
                break;
            }
            default:
                break;
            }
        } while (FindNextFileW(find, &entry));
        FindClose(find);
        if (found)
            Log("RENDER_EXPORT_SEG,phase=resume_offer,done=" + std::to_string(offer.done) + ",of=" + std::to_string(offer.count) +
                ",audio_done=" + (offer.audioDone ? "1" : "0") + ",folder=" + Utf8(offer.folder));
        return found;
    }

    void ExportController::DropResumable(const ResumeOffer& offer)
    {
        if (offer.folder.empty() || Busy())
            return;
        DeleteFolder(offer.folder);
        Log("RENDER_EXPORT_SEG,phase=dropped,why=start_over,folder=" + Utf8(offer.folder));
    }

    void ExportController::Cancel()
    {
        if (m_channel)
            m_channel->cancel.store(1);
    }

    ExportStatus ExportController::Status() const
    {
        ExportStatus status;
        status.channel = m_channel != nullptr;
        if (!m_channel)
            return status;
        status.state = static_cast<ec::State>(m_channel->state.load());
        status.framesEncoded = m_channel->framesEncoded.load();
        status.framesTotalHint = m_channel->framesTotalHint.load();
        status.queueDepth = m_channel->queueDepth.load();
        status.latencyPresents = m_channel->latencyPresents.load();
        status.timerMismatches = m_channel->frameTimeMismatches.load();
        status.error = static_cast<ExportError>(m_channel->error.load());
        status.timings = m_gpu.Timings();
        status.segmentCount = m_channel->segmentCount.load();
        status.segmentIndex = m_channel->segmentIndex.load();
        status.projectFramesBefore = m_channel->projectFramesBefore.load();
        status.projectFramesTotal = m_channel->projectFramesTotal.load();
        status.held = m_channel->hold.load() != 0;
        // A segmented export: the whole video's frames (the channel's count is
        // the video pass's, shorter on a resume), and the ETA from the frames
        // of this run.
        std::uint64_t doneBefore = 0;
        if (const std::uint32_t segmentsTotal = m_statusSegments.load())
        {
            status.fileSegments = segmentsTotal;
            status.fileSegment = std::min(m_statusSegment.load(), segmentsTotal);
            status.framesTotalHint = m_statusTotal.load();
            doneBefore = std::min(m_statusFramesAtStart.load(), status.framesEncoded);
        }
        if (m_armed && m_startQpc.QuadPart)
        {
            status.secondsElapsed = SecondsSince(m_startQpc);
            const std::uint64_t done = status.framesEncoded - doneBefore;
            if (done > 10 && status.framesTotalHint > status.framesEncoded)
                status.secondsLeft = status.secondsElapsed / static_cast<double>(done) * static_cast<double>(status.framesTotalHint - status.framesEncoded);
        }
        std::lock_guard<std::mutex> lock(m_statusMutex);
        status.detail = m_detail;
        return status;
    }

    void ExportController::Fail(ExportError error, const std::string& detail)
    {
        Log("RENDER_EXPORT,phase=fail,error=" + std::to_string(static_cast<unsigned>(error)) + ",detail=" + detail);
        LogMemory("error", m_framesQueued, m_gpu.InFlight(), m_encoder ? m_encoder->QueueDepth() : 0);
        std::string shown = detail;
        if (m_seg.active)
        {
            // The closed segments stay for a resume (the one being written is
            // not in the manifest: it is written again).
            Log("RENDER_EXPORT_SEG,phase=kept,segment=" + std::to_string(m_seg.current + 1) + ",of=" + std::to_string(m_seg.count) +
                ",folder=" + Utf8(m_seg.folder));
            shown += " (the finished segments are kept: export again with the same settings to continue)";
            m_seg.active = false;
            m_statusSegments = 0;
        }
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_detail = shown;
        }
        if (m_channel)
        {
            m_channel->error.store(static_cast<std::uint32_t>(error));
            m_channel->cancel.store(1); // the probe stops the clock, gives the timer and the sound back
        }
        m_armed = false;
        if (m_encoder)
        {
            // Closed on a worker (the trailer of a long file takes a while).
            // Joined before the next one and in Shutdown (never detached: the
            // core may be unloaded).
            if (m_closer.joinable())
                m_closer.join();
            EncoderSink* encoder = m_encoder.release();
            m_closer = std::thread([encoder] {
                replay::threads::Scope threadName("export_closer");
                encoder->Close();
                delete encoder;
            });
        }
        m_teardownPresents = 8; // the GPU may still use the ring
        m_teardownAfterFail = true; // and Teardown waits for it (no completion query covers every command)
    }

    void ExportController::Arm(effect_runtime* runtime)
    {
        if (!m_channel->timerLocked.load())
        {
            // The whole point is a fixed timestep: without it the video would
            // stutter like a screen recording. Refused, not degraded.
            Fail(ExportError::TimerNotLocked, "timer_not_locked");
            return;
        }
        if (m_finisher.joinable())
            m_finisher.join();
        device* dev = runtime->get_device();
        const resource back = runtime->get_current_back_buffer();
        const resource_desc desc = dev->get_resource_desc(back);
        m_settings = m_channel->settings;
        m_width = desc.texture.width;
        m_height = desc.texture.height;
        // The effect runtime has no colour space getter in this ReShade
        // (6.6.2): a 16 bit float back buffer is scRGB (linear), a 10 bit one
        // is HDR10 only when [Export] Hdr10=1 says so (SDR 10 bit otherwise).
        const format typeless = format_to_typeless(desc.texture.format);
        const bool scrgb = typeless == format::r16g16b16a16_typeless;
        const bool hdr10 = !scrgb && typeless == format::r10g10b10a2_typeless && m_hdr10;
        const color_space space = scrgb ? color_space::extended_srgb_linear : hdr10 ? color_space::hdr10_st2084 : color_space::srgb_nonlinear;

        m_job = {};
        m_job.width = m_width;
        m_job.height = m_height;
        m_job.rate = m_settings.rate;
        m_job.preset = m_settings.preset;
        m_job.hdr10 = m_settings.hdrOutput != 0 && hdr10;
        m_job.layout = LayoutFor(m_settings.preset, m_job.hdr10);
        m_job.quality = m_settings.quality;
        m_job.ffmpeg = m_ffmpeg;
        m_job.finalOutput = m_settings.outputPath;
        m_job.lut = m_settings.lutPath;
        if (m_job.finalOutput.empty())
        {
            Fail(ExportError::EncoderStart, "no_output_path");
            return;
        }
        // A whole project's piece: its own file next to the requested one
        // (ProjectPartPath), kept until the last piece joins them all.
        const std::uint32_t segmentCount = m_channel->segmentCount.load();
        const std::uint32_t segmentIndex = m_channel->segmentIndex.load();
        const bool png = m_settings.preset == ec::VideoPreset::PngSequence;
        if (segmentCount)
        {
            if (segmentIndex == 0 || !m_project.active || m_project.finalOutput != m_job.finalOutput)
            {
                if (m_project.active)
                    DropProjectParts("new_project");
                m_project = {};
                m_project.active = true;
                m_project.count = segmentCount;
                m_project.finalOutput = m_job.finalOutput;
                m_project.titles = m_projectTitles;
            }
            m_project.index = segmentIndex;
            m_job.finalOutput = ProjectPartPath(m_project.finalOutput, segmentIndex, png);
        }
        else if (m_project.active)
        {
            DropProjectParts("plain_export");
        }
        m_logPath = WithoutExtension(segmentCount ? m_project.finalOutput : m_job.finalOutput) + L".export.log";
        {
            // The peaks cover the whole export (a project's pieces too).
            std::lock_guard<std::mutex> lock(m_memoryMutex);
            if (!segmentCount || segmentIndex == 0)
                m_memoryPeaks = {};
            const std::string adapter = m_memory.Open(dev->get_native(), dev->get_api() == device_api::d3d12);
            if (!segmentCount || segmentIndex == 0)
                Log(adapter);
        }
        m_memoryEvery = MemLogEveryFrames(m_settings.rate);
        m_memoryNextFrame = m_memoryEvery;
        {
            const std::size_t slash = m_job.finalOutput.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
                SHCreateDirectoryExW(nullptr, m_job.finalOutput.substr(0, slash).c_str(), nullptr);
            if (m_settings.preset == ec::VideoPreset::PngSequence)
                SHCreateDirectoryExW(nullptr, m_job.finalOutput.c_str(), nullptr);
        }
        m_job.output = m_settings.preset == ec::VideoPreset::PngSequence ? m_job.finalOutput : TempVideoPath(m_job.finalOutput);
        // A resume appends to the log of the export it continues.
        if ((!segmentCount || segmentIndex == 0) && !m_channel->resume.load())
            DeleteFileW(m_logPath.c_str());
        if (segmentCount)
        {
            // Deleted with the pieces should this one never finish.
            m_project.stray = {m_job.output, WithoutExtension(m_job.finalOutput) + L".audio.wav"};
            Log("RENDER_EXPORT,phase=project_piece,piece=" + std::to_string(segmentIndex + 1) + ",of=" + std::to_string(segmentCount) +
                ",start_ms=" + std::to_string(m_settings.startMs) + ",end_ms=" + std::to_string(m_settings.endMs) + ",file=" + Utf8(m_job.output));
        }

        if (!EnsureRuntime())
        {
            std::lock_guard<std::mutex> lock(m_probeMutex);
            Fail(ExportError::EncoderStart, "ffmpeg_dlls_missing (" + m_runtimeError + ")");
            return;
        }
        // Segmented export: a range of the clip in the libav backend (a
        // project's pieces and an image sequence are files already).
        if (m_segCloser.joinable())
            m_segCloser.join();
        m_seg = {};
        m_segCloseFailed = false;
        m_statusSegments = 0;
        m_statusSegment = 0;
        if (!segmentCount && !png && m_segmentSeconds > 0)
        {
            std::string why;
            if (!ArmSegments(why))
            {
                Fail(ExportError::ResumeMismatch, why);
                return;
            }
        }
        else if (!segmentCount)
        {
            if (m_channel->resume.load())
            {
                Fail(ExportError::ResumeMismatch, "resume_refused (segmented export is off for this export: [Export] SegmentSeconds, the backend or the "
                    "format; the segments are kept)");
                return;
            }
            Log(std::string("RENDER_EXPORT_SEG,phase=off,reason=") + (m_segmentSeconds == 0 ? "segment_seconds_0" : png ? "image_sequence" : "ffmpeg_exe"));
        }
        else if (m_channel->resume.load())
        {
            Fail(ExportError::ResumeMismatch, "resume_refused (segmented export is off for this export: [Export] SegmentSeconds, the backend or the "
                "format; the segments are kept)");
            return;
        }
        m_pending.valid = false;
        if (m_seg.videoDone)
        {
            // A resume with every segment done: no video pass (no ring, no
            // encoder); the probe goes on to the audio pass or the join.
            m_presentIndex = 0;
            m_framesQueued = 0;
            m_samplesTaken = 0;
            m_startQpc = {};
            m_finishing = false;
            m_teardownPresents = 0;
            m_teardownAfterFail = false;
            m_armed = true;
            m_channel->latencyPresents.store(~0u);
            m_channel->addonReady.store(1);
            Log("RENDER_EXPORT,phase=armed,video=0,reason=resume_segments_done");
            LogMemory("arm", 0, 0, 0);
            return;
        }

        GpuConfig config;
        config.width = m_width;
        config.height = m_height;
        config.backFormat = desc.texture.format;
        config.layout = m_job.layout;
        config.accumulate = m_settings.samples > 1;
        config.transfer = space == color_space::hdr10_st2084 ? 1u : space == color_space::extended_srgb_linear ? 2u : 0u;
        config.bt2020 = m_job.hdr10;
        config.dither = m_job.layout == OutputLayout::Nv12 || m_job.layout == OutputLayout::Rgba8;
        LetterboxRows(m_width, m_height, m_settings.letterboxAspect, config.barTop, config.barBottom);
        config.grain = std::clamp(m_settings.grain, 0.0f, 32.0f) / 255.0f;
        config.slots = kExportSlots;
        {
            std::string version;
            const QueryAbi abi = DetectReShadeQueryAbi(version);
            Log("RENDER_EXPORT,phase=reshade,version=" + (version.empty() ? std::string("unknown") : version) + ",query_abi=" + QueryAbiName(abi));
        }
        std::string error;
        if (!m_gpu.Begin(runtime, config, error))
        {
            const ExportError code = error.rfind("unsupported_reshade", 0) == 0 ? ExportError::UnsupportedReShade
                : error.rfind("unsupported_size", 0) == 0 || error.rfind("unsupported_api", 0) == 0 ? ExportError::UnsupportedFormat
                                                                                                   : ExportError::AccumulatorFailed;
            Fail(code, error);
            return;
        }
        {
            m_job.variant = VariantFor(m_settings.preset);
            m_job.automaticBackend = m_automaticBackend;
            m_job.forcedBackend = static_cast<std::uint32_t>(m_forcedBackend);
            if (m_seg.active)
            {
                // The first segment written (its own thumbnail rule).
                if (!OpenSegment(m_seg.current))
                {
                    m_gpu.End(dev);
                    Fail(ExportError::EncoderStart, "ffmpeg_runtime_missing");
                    return;
                }
            }
            else if (m_settings.thumbnail && m_settings.preset != ec::VideoPreset::PngSequence)
            {
                // The middle frame, as the ffmpeg.exe path's -ss (end - start) / 2.
                m_job.thumbnail = WithoutExtension(m_job.finalOutput) + L".jpg";
                m_job.thumbnailFrame = (m_settings.endMs - m_settings.startMs) * m_settings.rate.num / (2000ull * m_settings.rate.den);
                if (segmentCount)
                {
                    // The piece holding the whole's middle frame (the probe set
                    // the thumbnail on that one only): the project's JPEG.
                    m_job.thumbnail = WithoutExtension(m_project.finalOutput) + L".jpg";
                    const std::uint64_t middle = m_channel->projectFramesTotal.load() / 2;
                    const std::uint64_t before = m_channel->projectFramesBefore.load();
                    const std::uint64_t hint = m_channel->framesTotalHint.load();
                    m_job.thumbnailFrame = middle > before ? std::min(middle - before, hint ? hint - 1 : 0) : 0;
                }
            }
            if (!m_seg.active)
                m_encoder = std::make_unique<LibavSink>();
        }
        m_encoderLogged = false;
        if (!m_seg.active && !m_encoder->Open(m_job))
        {
            m_gpu.End(dev);
            Fail(ExportError::EncoderStart, "ffmpeg_runtime_missing");
            return;
        }
        m_presentIndex = 0;
        m_latency = ~0u;
        m_latencyVotes = 0;
        m_latencyCandidate = ~0u;
        m_lastSerial = ~0ull;
        m_holdEpoch = m_holdAcked = m_channel->holdEpoch.load();
        m_holdSeen = 0;
        m_resyncPresents = 0;
        m_rebaseVote.Reset();
        m_samplesTaken = 0;
        m_framesQueued = 0;
        m_ringWaitUs = 0.0;
        m_queueWaitUs = 0.0;
        m_startQpc = {};
        m_finishing = false;
        m_teardownPresents = 0;
        m_teardownAfterFail = false;
        m_armed = true;
        m_channel->latencyPresents.store(~0u);
        m_channel->addonReady.store(1);
        {
            const std::string hook = HookWindow(static_cast<HWND>(runtime->get_hwnd()), m_channel);
            Log("RENDER_EXPORT,phase=window_hook,status=" + (hook.empty() ? std::string("on") : hook));
        }
        char line[512];
        std::snprintf(line, sizeof(line),
            "RENDER_EXPORT,phase=armed,size=%ux%u,format=%u,color_space=%u,layout=%u,samples=%u,shutter=%.1f,fps=%u/%u,preset=%u,bars=%u+%u,grain=%.2f,slots=%u,frame_bytes=%llu",
            m_width, m_height, static_cast<unsigned>(desc.texture.format), static_cast<unsigned>(space), static_cast<unsigned>(m_job.layout),
            m_settings.samples, m_settings.shutterDegrees, m_settings.rate.num, m_settings.rate.den, static_cast<unsigned>(m_settings.preset),
            config.barTop, config.barBottom, m_settings.grain, kExportSlots, static_cast<unsigned long long>(m_gpu.FrameBytes()));
        Log(line);
        LogMemory("arm", 0, m_gpu.InFlight(), m_encoder->QueueDepth());
        {
            const int v = m_job.variant;
            const std::string pixFmt = v >= 0 ? VariantPixFmt(AllVariants()[static_cast<std::size_t>(v)], m_job.hdr10) : std::string("?");
            Log("RENDER_EXPORT,phase=libav,ffmpeg=" + FfmpegVersionText() + ",variant=" + (v >= 0 ? AllVariants()[static_cast<std::size_t>(v)].codec : "first_that_opens") +
                ",filters=" + VideoFilterChain(m_job.preset, m_job.layout, pixFmt, m_job.hdr10, Utf8(m_job.lut)));
        }
    }

    void ExportController::DrainFrames(effect_runtime* runtime, bool wait)
    {
        LARGE_INTEGER t0{};
        QueryPerformanceCounter(&t0);
        const bool ok = m_gpu.Drain(runtime, wait, [this](std::int64_t frame, std::vector<std::uint8_t>& data) {
            // A segmented export: the first frame of the next segment closes
            // this one's encoder and opens the next's.
            if (m_seg.active && static_cast<std::uint64_t>(frame) + m_seg.firstFrame >= m_seg.currentEnd && !RotateSegment())
            {
                Fail(ExportError::EncoderStart, "segment_open_failed (segment " + std::to_string(m_seg.current + 1) + ")");
                return false;
            }
            VideoFrame video;
            video.index = frame;
            video.width = m_width;
            video.height = m_height;
            video.data.swap(data);
            std::uint32_t waited = 0;
            if (!m_encoder || !m_encoder->Push(video, waited))
            {
                std::string why = "encoder_closed (see the .ffmpeg.log next to the video)";
                if (auto* libav = dynamic_cast<LibavSink*>(m_encoder.get()); libav && !libav->OpenError().empty())
                    why = "no_encoder_opened (" + libav->OpenError() + ")";
                Fail(m_encoder ? m_encoder->Error() : ExportError::EncoderWrite, why);
                return false;
            }
            m_queueWaitUs = m_queueWaitUs * 0.9 + waited * 0.1;
            data = m_encoder->TakeBuffer(static_cast<std::size_t>(m_gpu.FrameBytes()));
            ++m_framesQueued;
            return true;
        });
        if (wait)
            m_ringWaitUs = m_ringWaitUs * 0.9 + SecondsSince(t0) * 1e6 * 0.1;
        if (!ok && m_armed)
        {
            if (m_gpu.DrainTimedOut())
                Fail(ExportError::GpuTimeout, "gpu_timeout (a video frame was not done on the GPU after " + std::to_string(kDrainWaitLimitMs) +
                    " ms, query_abi=" + QueryAbiName(m_gpu.Abi()) + ")");
            else
                Fail(ExportError::EncoderWrite, "readback_map_failed");
        }
    }

    void ExportController::PublishTimings()
    {
        if (!m_channel)
            return;
        const GpuTimings& t = m_gpu.Timings();
        m_channel->stageUs[ec::kStageGpuCopy].store(static_cast<std::uint32_t>(t.copyUs));
        m_channel->stageUs[ec::kStageGpuAccumulate].store(static_cast<std::uint32_t>(t.accumulateUs));
        m_channel->stageUs[ec::kStageGpuResolve].store(static_cast<std::uint32_t>(t.resolveUs));
        m_channel->stageUs[ec::kStageGpuReadback].store(static_cast<std::uint32_t>(t.readbackUs));
        m_channel->stageUs[ec::kStageCpuMap].store(static_cast<std::uint32_t>(t.mapUs));
        m_channel->stageUs[ec::kStageCpuQueueWait].store(static_cast<std::uint32_t>(m_queueWaitUs));
        m_channel->stageUs[ec::kStageRingWait].store(static_cast<std::uint32_t>(m_ringWaitUs));
        if (m_framesQueued && m_startQpc.QuadPart)
            m_channel->stageUs[ec::kStageFrameWall].store(
                static_cast<std::uint32_t>(SecondsSince(m_startQpc) * 1e6 / static_cast<double>(m_framesQueued)));
        if (m_encoder)
        {
            if (!m_encoderLogged)
            {
                const std::string name = m_encoder->EncoderName();
                if (!name.empty())
                {
                    m_encoderLogged = true;
                    Log("RENDER_EXPORT,phase=encoder_open,codec=" + name);
                    // The export's thread ids (the writer and the readback helpers run by
                    // now).
                    Log("RENDER_EXPORT,phase=threads," + replay::threads::MapText());
                }
            }
            m_channel->queueDepth.store(m_encoder->QueueDepth());
            // A segmented export counts the whole video's frames.
            m_channel->framesEncoded.store((m_seg.active ? segments::StartFrame(m_seg.current, m_seg.manifest.seconds, m_settings.rate) : 0) +
                m_encoder->FramesWritten());
            m_channel->bytesWritten.store(m_encoder->BytesWritten());
        }
    }

    void ExportController::Capture(effect_runtime* runtime, ec::State state)
    {
        // Latency: the tags of the preroll and flush pictures.
        if (state == ec::State::Preroll || state == ec::State::Flush)
            m_gpu.ReadTag(runtime, m_presentIndex);
        m_gpu.DrainTags(runtime->get_device(), [this, state](std::uint64_t presentIndex, std::uint32_t tag) {
            const std::uint32_t latency = ex::LatencyFromTag(presentIndex, tag);
            if (m_latency == ~0u)
            {
                m_latencyVotes = latency == m_latencyCandidate ? m_latencyVotes + 1 : 1;
                m_latencyCandidate = latency;
                if (m_latencyVotes >= 3)
                {
                    m_latency = latency;
                    m_channel->latencyPresents.store(latency);
                    Log("RENDER_EXPORT,phase=latency,presents=" + std::to_string(latency) + ",source=tag");
                }
            }
            else if (latency != m_latency && state == ec::State::Flush)
            {
                // A tick without a present (or the reverse) during the
                // capture: frames after it belong to another tick. Kept, logged.
                Log("RENDER_EXPORT,phase=warning,reason=latency_slipped,start=" + std::to_string(m_latency) + ",end=" + std::to_string(latency));
                m_channel->error.store(static_cast<std::uint32_t>(ExportError::LatencySlipped));
            }
        });
        if (m_latency == ~0u && state == ec::State::Capture)
        {
            // No tag read in the preroll (the tag hidden, or a 16 bit back
            // buffer): 1.
            m_latency = 1u;
            m_channel->latencyPresents.store(m_latency);
            Log("RENDER_EXPORT,phase=latency,presents=" + std::to_string(m_latency) + ",source=default");
        }
        if (m_latency == ~0u || m_presentIndex < m_latency)
            return;
        // The picture on screen now belongs to the tick `latency` presents ago.
        const std::uint64_t serial = m_presentIndex - m_latency;
        // Never a tick twice (a resync may move the count back).
        if (m_lastSerial != ~0ull && serial <= m_lastSerial)
            return;
        ec::TickStamp stamp;
        if (!ec::ReadStamp(*m_channel, serial, stamp))
        {
            // In a hold the game may present less than the probe ticks; the
            // resync puts the count back in step.
            if (m_channel->probeSerial.load() > serial + ec::kTickRing && m_channel->hold.load() == 0)
                Fail(ExportError::StampMissing, "stamp_missing");
            return;
        }
        m_lastSerial = serial;
        if (stamp.phase != static_cast<std::uint32_t>(ex::Phase::Capture))
            return; // preroll and flush pictures are never encoded
        if (!m_startQpc.QuadPart)
            QueryPerformanceCounter(&m_startQpc);
        const bool first = (stamp.flags & ec::kFirstOfFrame) != 0;
        const bool last = (stamp.flags & ec::kLastOfFrame) != 0;
        if (m_seg.active)
        {
            const std::uint64_t whole = static_cast<std::uint64_t>(stamp.frame) + m_seg.firstFrame;
            if (first || stamp.samples <= 1)
                RecordResumePoint(whole, stamp.playheadMs);
            if (whole < m_seg.writeFrom)
                return; // a resume's pre-roll: the game renders it, nothing is read back
        }
        if (last && m_gpu.Full())
            DrainFrames(runtime, true); // backpressure: the oldest slot (submitted earlier) completes
        if (!m_armed)
            return;
        std::string error;
        const float weight = stamp.samples > 1 ? stamp.weight : 1.0f;
        if (!m_gpu.Sample(runtime, weight, first || stamp.samples <= 1, last || stamp.samples <= 1, stamp.frame,
                static_cast<std::uint32_t>(stamp.frame), error))
        {
            Fail(error == "size_changed" ? ExportError::SizeChanged : ExportError::AccumulatorFailed, error);
            return;
        }
        ++m_samplesTaken;
        if (last || stamp.samples <= 1)
            m_channel->framesAccumulated.fetch_add(1);
    }

    bool ExportController::Resync(effect_runtime* runtime)
    {
        namespace eh = replay::exporthold;
        // Each resync has its own epoch (written before hold says 2).
        const std::uint32_t epoch = m_channel->holdEpoch.load();
        const std::uint32_t hold = m_channel->hold.load();
        if (hold != 0 && m_holdSeen == 0)
            Log("RENDER_EXPORT,phase=hold_addon,reasons=" + eh::SignalNames(m_channel->holdReasons.load()) + ",present=" + std::to_string(m_presentIndex) +
                ",last_serial=" + std::to_string(static_cast<long long>(m_lastSerial)) + ",probe_serial=" + std::to_string(m_channel->probeSerial.load()) +
                ",frames_accumulated=" + std::to_string(m_channel->framesAccumulated.load()));
        m_holdSeen = hold;
        if (epoch == m_holdAcked)
            return false; // no resync pending: held ticks are Hold stamps, Capture skips them
        if (hold != 2)
        {
            if (hold == 0)
            {
                // The probe went on without the acknowledgement (its own timeout).
                Log("RENDER_EXPORT,phase=warning,reason=hold_resync_unacknowledged,epoch=" + std::to_string(epoch));
                m_holdAcked = epoch;
            }
            return false; // held again before the resync ended
        }
        if (epoch != m_holdEpoch)
        {
            m_holdEpoch = epoch;
            m_resyncPresents = 0;
            m_rebaseVote.Reset();
        }
        if (m_latency == ~0u)
        {
            // Held in the preroll before the latency was measured: the
            // preroll's own calibration does it from the same count.
            m_holdAcked = m_holdEpoch;
            m_channel->holdAck.store(m_holdEpoch);
            Log("RENDER_EXPORT,phase=hold_resync,epoch=" + std::to_string(m_holdEpoch) + ",result=preroll_calibrates");
            return false;
        }
        ++m_resyncPresents;
        m_gpu.ReadTag(runtime, m_presentIndex);
        bool done = false;
        std::int64_t delta = 0;
        m_gpu.DrainTags(runtime->get_device(), [this, &done, &delta](std::uint64_t presentIndex, std::uint32_t tag) {
            if (done)
                return;
            const std::uint64_t probeSerial = m_channel->probeSerial.load();
            if (probeSerial == 0)
                return;
            const std::uint64_t serial = eh::FullSerialOfTag(probeSerial - 1, tag);
            const std::int64_t d = eh::RebaseDelta(presentIndex, serial, m_latency);
            if (m_rebaseVote.Add(d))
            {
                done = true;
                delta = d;
            }
        });
        if (!done && m_resyncPresents < eh::kAddonResyncMaxPresents)
            return true;
        if (done)
            m_presentIndex = static_cast<std::uint64_t>(static_cast<std::int64_t>(m_presentIndex) + delta);
        m_holdAcked = m_holdEpoch;
        m_channel->holdAck.store(m_holdEpoch);
        Log("RENDER_EXPORT,phase=hold_resync,epoch=" + std::to_string(m_holdEpoch) + ",result=" + (done ? "rebased" : "no_tags") +
            ",delta=" + std::to_string(delta) + ",presents=" + std::to_string(m_resyncPresents) + ",present=" + std::to_string(m_presentIndex) +
            ",latency=" + std::to_string(m_latency));
        return true; // this present's tag was read; the stamps from the next one on
    }

    void ExportController::OnOverlay(effect_runtime* runtime)
    {
        if (!m_enabled)
        {
            // The ini may change under a running game (written before a probe
            // reload; the core is not reloaded then).
            if (++m_configRetry % 120 == 0)
                LoadConfig();
            return;
        }
        if (!OpenChannel())
            return;
        const auto state = static_cast<ec::State>(m_channel->state.load());
        if (Idle(state))
        {
            if (m_armed && !m_finishing) // the finisher owns the encoder once it runs
            {
                // Cancelled by the probe or the editor.
                Log("RENDER_EXPORT,phase=stopped,state=" + std::to_string(static_cast<unsigned>(state)));
                Fail(state == ec::State::Cancelled ? ExportError::Cancelled : static_cast<ExportError>(m_channel->error.load()),
                    state == ec::State::Cancelled ? "cancelled" : "failed");
            }
            else if (!m_armed && !m_finishing && m_project.active)
            {
                // A project export cancelled or failed (in a piece or between
                // two): its pieces go (the next overlay after the Fail above,
                // once the encoder's closer is done with the last one).
                DropProjectParts(state == ec::State::Cancelled ? "cancelled" : "failed");
            }
            return;
        }
        if (state == ec::State::Arming && !m_armed && !m_finishing && m_teardownPresents == 0)
        {
            Arm(runtime);
            return;
        }
        if (!m_armed)
            return;
        if (m_finishing)
            return; // the finisher owns the encoder and the audio pass capture now
        if (m_segCloseFailed.exchange(false))
        {
            Fail(ExportError::EncoderExit, "segment_close_failed (see the .ffmpeg.log of the segment in " + Utf8(m_seg.folder) + ")");
            return;
        }
        if (state == ec::State::Preroll || state == ec::State::Capture || state == ec::State::Flush)
        {
            // The hold (src/export_hold.h): Hold ticks are never encoded
            // (their stamps are not Capture); the resync reads tags instead.
            if (!Resync(runtime))
                Capture(runtime, state);
            ++m_presentIndex;
        }
        if (!m_armed)
            return;
        if (m_gpu.Ready())
            DrainFrames(runtime, false);
        PublishTimings();
        if (m_armed && m_framesQueued >= m_memoryNextFrame)
        {
            // Every 2 s of video (rate limited by the output, not the PC).
            LogMemory("periodic", m_framesQueued, m_gpu.InFlight(), m_encoder ? m_encoder->QueueDepth() : 0);
            while (m_memoryNextFrame <= m_framesQueued)
                m_memoryNextFrame += m_memoryEvery;
        }
        // The probe's clock is done: the last frames leave the ring, then the
        // encoder closes and the finalize runs on a worker.
        const bool videoDone = state == ec::State::Finishing || state == ec::State::AudioSeek || state == ec::State::AudioPass ||
            state == ec::State::AudioDone;
        if (videoDone && !m_finishing)
        {
            // The last slots were recorded presents ago (the flush ticks):
            // they complete without a flush. A slot of this very present
            // (never expected) waits for the next overlay.
            for (std::uint32_t before = m_gpu.InFlight(); m_armed && before > 0; before = m_gpu.InFlight())
            {
                DrainFrames(runtime, true);
                if (m_gpu.InFlight() == before)
                    break;
            }
            if (m_armed && m_gpu.InFlight() == 0)
                StartFinish();
        }
    }

    void ExportController::OnPresent(effect_runtime* runtime)
    {
        m_gpu.OnPresent();
        if (m_teardownPresents > 0 && --m_teardownPresents == 0)
            Teardown(runtime);
    }

    void ExportController::Teardown(effect_runtime* runtime)
    {
        // After a failure or a cancel the GPU may still hold commands that use
        // the ring, the scratch and the sum (the samples before a frame's last
        // one carry no completion query, and presents are no proof): the queue
        // goes idle before they are destroyed. One flush, on this exceptional
        // path only (as in Begin and Shutdown); a finished export drained its
        // ring first and frees without it.
        if (m_teardownAfterFail && m_gpu.Ready())
        {
            LARGE_INTEGER t0{};
            QueryPerformanceCounter(&t0);
            runtime->get_command_queue()->wait_idle();
            char line[96];
            std::snprintf(line, sizeof(line), "RENDER_EXPORT,phase=teardown,after=fail,wait_idle_ms=%.2f", SecondsSince(t0) * 1000.0);
            Log(line);
        }
        m_teardownAfterFail = false;
        m_gpu.End(runtime->get_device());
    }

    void ExportController::StartFinish()
    {
        m_finishing = true;
        PublishTimings();
        const double wall = m_startQpc.QuadPart ? SecondsSince(m_startQpc) : 0.0;
        const GpuTimings& t = m_gpu.Timings();
        char line[512];
        std::snprintf(line, sizeof(line),
            "RENDER_EXPORT,phase=video_queued,frames=%llu,samples=%llu,wall_s=%.3f,ms_per_frame=%.3f,gpu_copy_us=%.0f,gpu_accumulate_us=%.0f,gpu_resolve_us=%.0f,gpu_readback_us=%.0f,cpu_map_us=%.0f,queue_wait_us=%.0f,ring_wait_us=%.0f,latency=%u",
            static_cast<unsigned long long>(m_framesQueued), static_cast<unsigned long long>(m_samplesTaken), wall,
            m_framesQueued ? wall * 1000.0 / static_cast<double>(m_framesQueued) : 0.0, t.copyUs, t.accumulateUs, t.resolveUs, t.readbackUs,
            t.mapUs, m_queueWaitUs, m_ringWaitUs, m_latency);
        Log(line);
        LogMemory("finish", m_framesQueued, m_gpu.InFlight(), m_encoder ? m_encoder->QueueDepth() : 0);
        m_teardownPresents = 8; // the ring is drained; buffers freed a few presents later
        m_stopFinisher.store(false);
        m_finisher = std::thread([this] {
            replay::threads::Scope threadName("export_finisher");
            FinishWorker();
        });
    }

    void ExportController::FinishWorker()
    {
        ec::Channel* channel = m_channel;
        auto state = [channel] { return static_cast<ec::State>(channel->state.load()); };
        bool ok = true;
        std::string detail;
        ExportError failure = ExportError::EncoderExit; // the channel's error when !ok
        const bool libav = dynamic_cast<LibavSink*>(m_encoder.get()) != nullptr;
        // The audio pass capture belongs to this thread alone: started, read,
        // stopped and destroyed here (the render thread never touches it). It
        // starts before the encoder closes, so the trailer overlaps the pass as
        // before; the probe holds AudioSeek until this answer.
        std::unique_ptr<ProcessLoopbackCapture> audio;
        // A segmented export keeps its audio pass with its segments.
        const bool segmented = m_seg.active;
        const std::wstring audioPath = segmented ? m_seg.folder + L"\\audio.wav" : WithoutExtension(m_job.finalOutput) + L".audio.wav";
        if (m_settings.audio == ec::AudioMode::RealtimePass && state() == ec::State::AudioSeek && !m_stopFinisher.load())
        {
            auto capture = std::make_unique<ProcessLoopbackCapture>();
            std::string error;
            if (!capture->Start(GetCurrentProcessId(), audioPath, error)) // up to 5 s
            {
                Log("RENDER_EXPORT,phase=warning,reason=audio_capture_failed,detail=" + error + " (the video is kept without sound)");
                channel->audioCapture.store(static_cast<std::uint32_t>(ec::AudioAck::Failed));
            }
            else
            {
                Log("RENDER_EXPORT,phase=audio_capture,started=1");
                audio = std::move(capture);
                channel->audioCapture.store(static_cast<std::uint32_t>(ec::AudioAck::Capturing));
            }
        }
        if (segmented)
        {
            // The segment closer is done with the one before; the last one
            // closes here, short when the clock ended early.
            if (m_segCloser.joinable())
                m_segCloser.join();
            if (m_segCloseFailed.exchange(false))
            {
                ok = false;
                detail = "segment_close_failed (see the .ffmpeg.log of the segment in " + Utf8(m_seg.folder) + ")";
            }
            if (m_encoder)
            {
                const std::uint32_t last = m_seg.current;
                ok = CloseSegment(m_encoder.release(), SegmentRecord(last), true) && ok;
                if (!ok && detail.empty())
                    detail = "encoder_failed (see " + Utf8(SegmentPath(last)) + ".ffmpeg.log)";
            }
        }
        else if (m_encoder)
        {
            ok = m_encoder->Close();
            channel->framesEncoded.store(m_encoder->FramesWritten());
            channel->bytesWritten.store(m_encoder->BytesWritten());
            if (!ok)
                detail = "encoder_failed (see " + Utf8(m_job.output) + ".ffmpeg.log)";
            m_encoder.reset();
        }
        // The audio pass (the probe plays the range again at 1x).
        double audioStartS = 0.0;
        if (ok && m_settings.audio == ec::AudioMode::RealtimePass)
        {
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds((m_settings.endMs - m_settings.startMs) * 3 + 60000);
            AudioWaitEnd end = AudioWaitEnd::Waiting;
            while ((end = AudioWaitStep(state(), std::chrono::steady_clock::now() >= deadline, m_stopFinisher.load())) == AudioWaitEnd::Waiting)
                Sleep(50);
            if (end == AudioWaitEnd::ProbeFailed)
            {
                // The probe gave up waiting for the capture (AudioNotReady): no
                // pass ran. The export fails; the video without sound stays
                // under its temporary name.
                ok = false;
                failure = ExportError::AudioNotReady;
                detail = "audio_capture_not_ready (the video without sound is kept: " + Utf8(m_job.output) + ")";
            }
            else if (end == AudioWaitEnd::Cancelled)
            {
                // Cancelled during the pass: no finalize, so a video without
                // sound never takes the requested name.
                ok = false;
                failure = ExportError::Cancelled;
                detail = "cancelled_during_audio_pass (the video without sound is kept: " + Utf8(m_job.output) + ")";
            }
            else if (end == AudioWaitEnd::Stopped)
            {
                // The editor or the game is closing: no finalize now (a remux
                // would hold the shutdown). The video without sound stays under
                // its temporary name.
                ok = false;
                failure = ExportError::Cancelled;
                detail = "export_stopped_during_audio_pass (the video without sound is kept: " + Utf8(m_job.output) + ")";
            }
            if (audio)
            {
                audio->Stop();
                const std::uint64_t first = audio->FirstQpc100ns();
                const std::uint64_t start = ProcessLoopbackCapture::Qpc100ns(channel->audioStartQpc.load());
                if (state() == ec::State::AudioDone && start > first)
                {
                    const int offsetMs = m_settings.audioOffsetMs != 0 ? m_settings.audioOffsetMs : m_audioOffsetMs;
                    audioStartS = static_cast<double>(start - first) / 1e7 + offsetMs / 1000.0;
                    if (audioStartS < 0.0)
                        audioStartS = 0.0;
                    m_job.audioWav = audioPath;
                }
                char line[256];
                std::snprintf(line, sizeof(line), "RENDER_EXPORT,phase=audio_captured,frames=%llu,packets=%llu,gap_frames=%llu,start_s=%.4f,state=%u",
                    static_cast<unsigned long long>(audio->Frames()), static_cast<unsigned long long>(audio->Packets()),
                    static_cast<unsigned long long>(audio->GapFrames()), audioStartS, static_cast<unsigned>(state()));
                Log(line);
            }
        }
        // A whole project's piece: kept (its video, its sound and where the
        // range starts in it) until the last piece, which joins them all.
        const bool piece = m_project.active;
        std::string doneDetail; // the dialog's text when ok (the file, with a warning if any)
        if (piece)
        {
            audio.reset(); // its WAV closed before it is read or deleted
            if (ok)
            {
                ProjectPart part;
                part.video.path = m_job.output;
                part.video.frames = channel->framesEncoded.load();
                part.wav = m_job.audioWav;
                part.audioStartS = audioStartS;
                part.wavFile = WithoutExtension(m_job.finalOutput) + L".audio.wav";
                m_project.parts.push_back(part);
                m_project.stray.clear();
                Log("RENDER_EXPORT,phase=project_piece_done,piece=" + std::to_string(m_project.index + 1) + ",of=" +
                    std::to_string(m_project.count) + ",frames=" + std::to_string(part.video.frames) + ",audio=" + (part.wav.empty() ? "0" : "1"));
                if (m_project.index + 1 < m_project.count)
                {
                    {
                        std::lock_guard<std::mutex> lock(m_statusMutex);
                        m_detail.clear();
                    }
                    m_armed = false;
                    m_finishing = false;
                    channel->state.store(static_cast<std::uint32_t>(ec::State::SegmentDone)); // the script opens the next clip
                    return;
                }
                ok = FinishProject(libav, detail, doneDetail, failure);
            }
            if (!ok && failure != ExportError::FinalizeFailed)
            {
                DropProjectParts("piece_failed", false);
                detail = detail.substr(0, detail.find(" (")); // no video is kept: the pieces are deleted
            }
        }
        if (segmented)
        {
            audio.reset(); // its WAV closed before the join reads it
            ok = FinishSegments(ok, audioStartS, detail, doneDetail, failure);
        }
        // Chapters from the markers (a FFMETADATA file for ffmpeg.exe;
        // libavformat takes them directly).
        const bool chapters = !piece && !segmented && ok && m_settings.chapters && !m_chapters.empty() &&
            m_settings.preset != ec::VideoPreset::PngSequence;
        if (chapters && !libav)
        {
            m_job.chapters = WithoutExtension(m_job.finalOutput) + L".chapters.txt";
            const std::string meta = BuildChapterMetadata(m_chapters, m_settings.endMs - m_settings.startMs);
            HANDLE file = CreateFileW(m_job.chapters.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                WriteFile(file, meta.data(), static_cast<DWORD>(meta.size()), &written, nullptr);
                CloseHandle(file);
            }
            else
            {
                m_job.chapters.clear();
            }
        }
        // Finalize: remux (audio, chapters) or rename.
        if (!piece && !segmented && ok && m_settings.preset != ec::VideoPreset::PngSequence)
        {
            FinalizeReport report;
            DWORD moveError = 0;
            const std::wstring log = m_job.output + L".ffmpeg.log";
            // A fragmented video pass (MP4, MOV) is always remuxed into a plain file,
            // even without audio or chapters.
            const bool fragmented = FragmentedMuxer(MuxerFor(m_job.output, m_settings.preset));
            if (!m_job.audioWav.empty() || !m_job.chapters.empty() || (libav && chapters) || fragmented)
            {
                bool finalized = false;
                if (libav)
                {
                    std::string error;
                    SetFfmpegLogFile(log);
                    finalized = LibavFinalize(m_job, m_job.output, audioStartS, chapters ? m_chapters : std::vector<Chapter>{},
                        m_settings.endMs - m_settings.startMs, channel->framesEncoded.load(), error);
                    SetFfmpegLogFile(L"");
                    if (!finalized)
                    {
                        DeleteFileW(m_job.finalOutput.c_str());
                        Log("RENDER_EXPORT,phase=warning,reason=finalize_failed,detail=" + error);
                    }
                }
                else
                {
                    finalized = RunFfmpeg(m_ffmpeg, BuildFinalizeArgs(m_job, m_job.output, audioStartS), log, 600000) == 0;
                }
                bool moved = false;
                if (finalized)
                {
                    DeleteFileW(m_job.output.c_str());
                }
                else
                {
                    // The video alone is still there under its temporary name.
                    moved = MoveFileExW(m_job.output.c_str(), m_job.finalOutput.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
                    moveError = moved ? 0 : GetLastError();
                }
                report = ReportFinalize(true, finalized, moved, Utf8(m_job.finalOutput), Utf8(m_job.output));
            }
            else
            {
                const bool moved = MoveFileExW(m_job.output.c_str(), m_job.finalOutput.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
                moveError = moved ? 0 : GetLastError();
                report = ReportFinalize(false, false, moved, Utf8(m_job.finalOutput), Utf8(m_job.output));
            }
            if (!report.ok)
            {
                ok = false;
                failure = ExportError::FinalizeFailed;
                detail = report.detail;
                Log("RENDER_EXPORT,phase=warning,reason=video_not_saved,move_error=" + std::to_string(moveError));
            }
            else
            {
                doneDetail = report.detail;
            }
            if (!m_job.chapters.empty())
                DeleteFileW(m_job.chapters.c_str());
            if (ok && m_settings.thumbnail && !libav) // libav: taken from the middle frame during the encode
            {
                const double middle = static_cast<double>(m_settings.endMs - m_settings.startMs) / 2000.0;
                RunFfmpeg(m_ffmpeg, BuildThumbnailArgs(m_job.finalOutput, middle, WithoutExtension(m_job.finalOutput) + L".jpg"), log, 60000);
            }
        }
        audio.reset(); // stopped (and its WAV closed) on any path
        {
            // After the finalize (it holds the whole file's packets for a while).
            LogMemory("end", channel->framesEncoded.load(), 0, 0);
            MemoryPeaks peaks;
            {
                std::lock_guard<std::mutex> lock(m_memoryMutex);
                peaks = m_memoryPeaks;
            }
            const std::wstring& file = piece ? m_project.finalOutput : m_job.finalOutput;
            char line[512];
            std::snprintf(line, sizeof(line), "RENDER_EXPORT,phase=%s,frames=%llu,bytes=%llu,file=%s,detail=%s", ok ? "done" : "failed",
                static_cast<unsigned long long>(channel->framesEncoded.load()),
                static_cast<unsigned long long>(FileBytes(file)), Utf8(file).c_str(), ok ? doneDetail.c_str() : detail.c_str());
            Log(line + FormatMemPeaks(peaks));
        }
        if (piece)
            m_project = {}; // joined (its pieces deleted) or dropped above
        if (segmented)
        {
            m_seg.active = false;
            m_statusSegments = 0;
        }
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_detail = ok ? (doneDetail.empty() ? Utf8(m_job.finalOutput) : doneDetail) : detail;
        }
        if (!ok)
            channel->error.store(static_cast<std::uint32_t>(failure));
        m_armed = false;
        m_finishing = false;
        channel->state.store(static_cast<std::uint32_t>(ok ? ec::State::Done : ec::State::Failed));
    }

    void ExportController::DropProjectParts(const char* why, bool joinCloser)
    {
        if (!m_project.active)
            return;
        // The closer may still write the last piece's trailer (a Fail): done first.
        if (joinCloser && m_closer.joinable())
            m_closer.join();
        std::size_t files = 0;
        for (const ProjectPart& part : m_project.parts)
        {
            DeletePath(part.video.path);
            DeletePath(part.wavFile);
            files += 2;
        }
        for (const std::wstring& path : m_project.stray)
        {
            DeletePath(path);
            ++files;
        }
        Log(std::string("RENDER_EXPORT,phase=project_dropped,why=") + why + ",pieces=" + std::to_string(m_project.parts.size()) +
            ",of=" + std::to_string(m_project.count) + ",files=" + std::to_string(files));
        m_project = {};
    }

    bool ExportController::FinishProject(bool libav, std::string& detail, std::string& doneDetail, ExportError& failure)
    {
        const std::vector<ProjectPart>& parts = m_project.parts;
        const std::wstring& final = m_project.finalOutput;
        std::vector<std::uint64_t> frames;
        std::uint64_t total = 0;
        for (const ProjectPart& part : parts)
        {
            frames.push_back(part.video.frames);
            total += part.video.frames;
        }
        const ex::FrameRate rate = m_settings.rate;
        const std::uint64_t durationMs = static_cast<std::uint64_t>(replay::projectexport::FrameTimeNs(rate, total) / 1000000);
        Log("RENDER_EXPORT,phase=project_join,pieces=" + std::to_string(parts.size()) + ",frames=" + std::to_string(total) +
            ",duration_ms=" + std::to_string(durationMs) + ",file=" + Utf8(final));
        const auto deletePieces = [&] {
            for (const ProjectPart& part : parts)
            {
                DeletePath(part.video.path);
                DeletePath(part.wavFile);
            }
        };
        if (m_settings.preset == ec::VideoPreset::PngSequence)
        {
            // An image sequence: the pieces' PNGs into the requested folder,
            // numbered on from one piece to the next (image2 starts at 1).
            SHCreateDirectoryExW(nullptr, final.c_str(), nullptr);
            std::uint64_t first = 0;
            bool moved = true;
            for (std::size_t k = 0; k < parts.size() && moved; ++k)
            {
                for (std::uint64_t n = 1; n <= parts[k].video.frames; ++n)
                {
                    wchar_t from[32], to[32];
                    std::swprintf(from, 32, L"\\%06llu.png", static_cast<unsigned long long>(n));
                    std::swprintf(to, 32, L"\\%06llu.png", static_cast<unsigned long long>(first + n));
                    if (!MoveFileExW((parts[k].video.path + from).c_str(), (final + to).c_str(), MOVEFILE_REPLACE_EXISTING))
                    {
                        moved = false;
                        break;
                    }
                }
                first += parts[k].video.frames;
            }
            if (!moved)
            {
                failure = ExportError::FinalizeFailed;
                detail = "finalize_failed: the image sequence could not be gathered in " + Utf8(final) + "; its pieces are kept next to it (.partNN)";
                return false;
            }
            deletePieces();
            doneDetail = Utf8(final);
            return true;
        }
        EncodeJob job = m_job;
        job.finalOutput = final;
        job.output = TempVideoPath(final);
        job.audioWav.clear();
        job.chapters.clear();
        // The sound: every piece's audio pass cut to its frames, one WAV.
        bool anyAudio = false;
        for (const ProjectPart& part : parts)
            anyAudio = anyAudio || !part.wav.empty();
        if (m_settings.audio == ec::AudioMode::RealtimePass && anyAudio)
        {
            WavInfo info;
            std::uint32_t sampleRate = 48000;
            for (const ProjectPart& part : parts)
                if (ReadWavInfo(part.wav, info))
                {
                    sampleRate = info.sampleRate;
                    break;
                }
            const std::wstring wav = WithoutExtension(final) + L".audio.wav";
            std::string error;
            SpliceReport report;
            if (SpliceWav(PlanAudio(parts, rate, sampleRate), wav, error, &report))
            {
                job.audioWav = wav;
                char line[192];
                std::snprintf(line, sizeof(line), "RENDER_EXPORT,phase=project_audio,samples=%llu,silent=%llu,rate=%u",
                    static_cast<unsigned long long>(report.samples), static_cast<unsigned long long>(report.silentSamples), report.sampleRate);
                Log(line);
            }
            else
            {
                Log("RENDER_EXPORT,phase=warning,reason=project_audio_failed,detail=" + error + " (the video is kept without sound)");
            }
        }
        // One chapter per clip.
        std::vector<Chapter> chapters;
        if (m_settings.chapters)
            for (const auto& [ms, title] : PartChapterTimes(m_project.titles, frames, rate))
                chapters.push_back({ms, title});
        const std::wstring log = final + L".ffmpeg.log";
        bool finalized = false;
        std::string error;
        if (libav)
        {
            std::vector<VideoPart> videos;
            for (const ProjectPart& part : parts)
                videos.push_back(part.video);
            SetFfmpegLogFile(log);
            finalized = LibavFinalizeParts(job, videos, 0.0, chapters, durationMs, total, error);
            SetFfmpegLogFile(L"");
        }
        else
        {
            // ffmpeg.exe: the concat demuxer over a list of the pieces.
            const std::wstring list = WithoutExtension(final) + L".concat.txt";
            std::vector<std::string> paths;
            for (const ProjectPart& part : parts)
                paths.push_back(Utf8(part.video.path));
            const std::string text = BuildConcatList(paths);
            HANDLE file = CreateFileW(list.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
                CloseHandle(file);
                if (!chapters.empty())
                {
                    job.chapters = WithoutExtension(final) + L".chapters.txt";
                    const std::string meta = BuildChapterMetadata(chapters, durationMs);
                    HANDLE metaFile = CreateFileW(job.chapters.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (metaFile != INVALID_HANDLE_VALUE)
                    {
                        WriteFile(metaFile, meta.data(), static_cast<DWORD>(meta.size()), &written, nullptr);
                        CloseHandle(metaFile);
                    }
                    else
                        job.chapters.clear();
                }
                finalized = RunFfmpeg(m_ffmpeg, BuildFinalizeArgs(job, list, 0.0, true), log, 600000) == 0;
                if (!job.chapters.empty())
                    DeleteFileW(job.chapters.c_str());
                DeleteFileW(list.c_str());
                if (finalized && m_settings.thumbnail)
                    RunFfmpeg(m_ffmpeg, BuildThumbnailArgs(final, static_cast<double>(durationMs) / 2000.0, WithoutExtension(final) + L".jpg"), log, 60000);
            }
            else
                error = "cannot write the concat list";
        }
        if (!finalized)
        {
            DeleteFileW(final.c_str());
            Log("RENDER_EXPORT,phase=warning,reason=project_join_failed,detail=" + error);
            failure = ExportError::FinalizeFailed;
            // Hours of rendering are not thrown away: the pieces stay.
            detail = "finalize_failed: the project's pieces could not be joined (" + error + "); they are kept next to " + Utf8(final) +
                " (.partNN), see the .ffmpeg.log";
            m_project.parts.clear(); // kept, not dropped
            return false;
        }
        deletePieces();
        doneDetail = Utf8(final);
        return true;
    }

    // ------------------------------------------------------------------
    // Segmented export (export_segments.h)
    // ------------------------------------------------------------------

    std::wstring ExportController::SegmentPath(std::uint32_t k) const
    {
        return m_seg.folder + L"\\" + Wide(segments::SegmentFileName(k, m_seg.manifest.extension));
    }

    bool ExportController::WriteManifest()
    {
        // A temporary file moved over the old one: a crash leaves either.
        const std::string text = segments::Format(m_seg.manifest);
        const std::wstring path = m_seg.folder + L"\\manifest.txt";
        const std::wstring temp = path + L".tmp";
        HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        DWORD written = 0;
        bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
        ok = FlushFileBuffers(file) && ok;
        CloseHandle(file);
        return ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }

    bool ExportController::ArmSegments(std::string& why)
    {
        namespace sg = segments;
        const bool resumeAsked = m_channel->resume.load() != 0;
        const bool pendingHere = m_pending.valid && m_pending.requestSeq == m_channel->requestSeq.load();
        SegmentRun run;
        run.folder = PartsFolder(m_job.finalOutput);
        sg::ResumePlan plan;
        if (resumeAsked)
        {
            std::string reason;
            if (!pendingHere || !m_pending.resume || _wcsicmp(m_pending.folder.c_str(), run.folder.c_str()) != 0)
            {
                why = "resume_lost (the request does not name this parts folder)";
                return false;
            }
            if (!ReadManifest(run.folder, run.manifest, reason))
            {
                why = "resume_manifest (" + reason + ")";
                return false;
            }
            if (sg::Check(run.manifest, m_pending.settingsHash, m_pending.identity) != sg::ResumeCheck::Resumable || !sg::PlanResume(run.manifest, plan))
            {
                why = "resume_manifest_changed";
                return false;
            }
            if (run.manifest.width != m_width || run.manifest.height != m_height)
            {
                why = "resume_size_changed (the segments are " + std::to_string(run.manifest.width) + "x" + std::to_string(run.manifest.height) +
                    ", the game is " + std::to_string(m_width) + "x" + std::to_string(m_height) + ": set that resolution again, or start over)";
                return false;
            }
            const std::uint64_t range = m_channel->rangeFrames.load();
            if (range != run.manifest.totalFrames)
            {
                why = "resume_range_changed (the range has " + std::to_string(range) + " frames now, the segments were made for " +
                    std::to_string(run.manifest.totalFrames) + ": a marker or the speed changed; start over)";
                return false;
            }
            if (m_channel->resumeFrames.load() != (plan.videoDone ? 0 : plan.frames) || (!plan.videoDone && m_channel->resumeStartMs.load() != plan.startMs))
            {
                why = "resume_plan_changed";
                return false;
            }
            // The segments from the first missing one on are written again.
            for (std::size_t k = plan.segment; k < run.manifest.segments.size(); ++k)
                DeleteFileW((run.folder + L"\\" + Wide(run.manifest.segments[k].file)).c_str());
            run.manifest.segments.resize(plan.segment);
            run.resumed = true;
            run.videoDone = plan.videoDone;
            run.firstFrame = plan.firstFrame;
            run.writeFrom = plan.writeFrom;
            run.current = plan.segment;
        }
        else
        {
            if (GetFileAttributesW(run.folder.c_str()) != INVALID_FILE_ATTRIBUTES)
                DeleteFolder(run.folder); // a stale folder under this very name
            sg::Manifest& m = run.manifest;
            m.settingsHash = pendingHere ? m_pending.settingsHash : sg::SettingsHash(m_settings, m_segmentSeconds, VariantFor(m_settings.preset), m_hdr10);
            m.identity = pendingHere ? m_pending.identity : std::string("bench");
            m.finalOutput = Utf8(m_job.finalOutput);
            m.extension = ExtensionOf(m_job.finalOutput);
            m.rate = m_settings.rate;
            m.seconds = m_segmentSeconds;
            m.totalFrames = m_channel->framesTotalHint.load();
            m.width = m_width;
            m.height = m_height;
        }
        if (run.manifest.totalFrames == 0)
        {
            why = "segments_no_frames";
            return false;
        }
        run.total = run.manifest.totalFrames;
        run.count = run.manifest.SegmentCount();
        run.currentEnd = std::min(sg::StartFrame(run.current + 1ull, run.manifest.seconds, m_settings.rate), run.total);
        run.prerollFrames = sg::PrerollFrames(kResumePrerollMs, m_settings.rate);
        run.nextPoint = run.current + 1;
        run.active = true;
        SHCreateDirectoryExW(nullptr, run.folder.c_str(), nullptr);
        m_seg = std::move(run);
        {
            std::lock_guard<std::mutex> lock(m_segMutex);
            if (!WriteManifest())
            {
                why = "segments_folder (the manifest cannot be written in " + Utf8(m_seg.folder) + ")";
                m_seg.active = false;
                return false;
            }
        }
        m_statusTotal = m_seg.total;
        m_statusSegments = m_seg.count;
        m_statusSegment = m_seg.current + 1;
        m_statusFramesAtStart = m_seg.videoDone ? m_seg.total : m_seg.writeFrom;
        char line[256];
        std::snprintf(line, sizeof(line),
            "RENDER_EXPORT_SEG,phase=armed,segments=%u,segment_seconds=%u,total_frames=%llu,resume=%d,first_missing=%u,video_from=%llu,write_from=%llu,"
            "preroll_frames=%llu,resume_start_ms=%llu,video_frames=%llu,folder=",
            m_seg.count, m_seg.manifest.seconds, static_cast<unsigned long long>(m_seg.total), m_seg.resumed ? 1 : 0, m_seg.current + 1,
            static_cast<unsigned long long>(m_seg.firstFrame), static_cast<unsigned long long>(m_seg.writeFrom),
            static_cast<unsigned long long>(m_seg.writeFrom - m_seg.firstFrame), static_cast<unsigned long long>(m_channel->resumeStartMs.load()),
            static_cast<unsigned long long>(m_seg.videoDone ? 0 : m_seg.total - m_seg.firstFrame));
        Log(line + Utf8(m_seg.folder) + ",manifest_audio=" + (m_seg.manifest.audioDone ? "1" : "0"));
        return true;
    }

    bool ExportController::OpenSegment(std::uint32_t k)
    {
        // A fresh encoder per segment: it starts on a keyframe, and the one
        // before gives its memory back when it closes.
        EncodeJob job = m_job;
        job.output = SegmentPath(k);
        job.thumbnail.clear();
        const segments::Bounds b = segments::BoundsOf(k, m_seg.total, m_seg.manifest.seconds, m_settings.rate);
        const std::uint64_t middle = m_seg.total / 2;
        if (m_settings.thumbnail && middle >= b.first && middle < b.first + b.frames)
        {
            job.thumbnail = WithoutExtension(m_job.finalOutput) + L".jpg";
            job.thumbnailFrame = middle - b.first;
        }
        DeleteFileW(job.output.c_str());
        DeleteFileW((job.output + L".ffmpeg.log").c_str());
        auto sink = std::make_unique<LibavSink>();
        if (!sink->Open(job))
            return false;
        m_encoder = std::move(sink);
        m_statusSegment = k + 1;
        char line[200];
        std::snprintf(line, sizeof(line), "RENDER_EXPORT_SEG,phase=open,segment=%u,of=%u,first_frame=%llu,frames=%llu,first_sample=%llu,samples=%llu,file=", k + 1,
            m_seg.count, static_cast<unsigned long long>(b.first), static_cast<unsigned long long>(b.frames), static_cast<unsigned long long>(b.firstSample),
            static_cast<unsigned long long>(b.samples));
        Log(line + Utf8(job.output));
        return true;
    }

    void ExportController::RecordResumePoint(std::uint64_t frame, std::uint64_t playheadMs)
    {
        // Where each later segment's resume will start: the playhead this
        // export has at that frame (a resume seeks there).
        while (m_seg.nextPoint < m_seg.count)
        {
            const std::uint64_t at = segments::ResumeFrame(m_seg.nextPoint, m_seg.prerollFrames, m_seg.manifest.seconds, m_settings.rate, m_seg.firstFrame);
            if (at > frame)
                break;
            if (at == frame)
                m_seg.points[at] = playheadMs;
            ++m_seg.nextPoint;
        }
    }

    segments::Record ExportController::SegmentRecord(std::uint32_t k) const
    {
        const segments::Bounds b = segments::BoundsOf(k, m_seg.total, m_seg.manifest.seconds, m_settings.rate);
        segments::Record r;
        r.index = k;
        r.frames = b.frames;
        r.firstSample = b.firstSample;
        r.samples = b.samples;
        r.file = segments::SegmentFileName(k, m_seg.manifest.extension);
        if (k + 1 < m_seg.count)
        {
            const std::uint64_t at = segments::ResumeFrame(k + 1, m_seg.prerollFrames, m_seg.manifest.seconds, m_settings.rate, m_seg.firstFrame);
            const auto found = m_seg.points.find(at);
            if (found != m_seg.points.end())
            {
                r.hasResume = true;
                r.resumeFrame = at;
                r.resumeMs = found->second;
            }
        }
        return r;
    }

    bool ExportController::RotateSegment()
    {
        if (m_segCloser.joinable())
            m_segCloser.join(); // the one before closed a segment's length ago
        const std::uint32_t k = m_seg.current;
        const segments::Record record = SegmentRecord(k);
        EncoderSink* sink = m_encoder.release();
        m_seg.current = k + 1;
        m_seg.currentEnd = std::min(segments::StartFrame(m_seg.current + 1ull, m_seg.manifest.seconds, m_settings.rate), m_seg.total);
        const bool opened = m_seg.current < m_seg.count && OpenSegment(m_seg.current);
        const std::wstring nextLog = SegmentPath(m_seg.current) + L".ffmpeg.log";
        m_segCloser = std::thread([this, sink, record, nextLog, opened] {
            replay::threads::Scope threadName("export_segment_closer");
            if (!CloseSegment(sink, record, false))
                m_segCloseFailed = true;
            if (opened)
                SetFfmpegLogFile(nextLog); // the close cleared the log the next segment's encoder had set
        });
        return opened;
    }

    bool ExportController::CloseSegment(EncoderSink* sink, segments::Record record, bool last)
    {
        const bool closed = sink->Close();
        const std::uint64_t written = sink->FramesWritten();
        const std::uint64_t bytes = sink->BytesWritten();
        delete sink; // its encoder, filter graph and frame pool go now
        const std::uint64_t expected = record.frames;
        const std::uint64_t first = segments::StartFrame(record.index, m_seg.manifest.seconds, m_settings.rate);
        bool ok = closed;
        bool shortened = false;
        if (written != expected)
        {
            // Only the last segment may end early: the playhead reached the
            // range's end before the planned count (a frame or two).
            if (last && written > 0 && written < expected)
            {
                record.frames = written;
                record.samples = replay::projectexport::AudioSamples(first, written, m_settings.rate, segments::kSampleRate);
                shortened = true;
            }
            else
            {
                ok = false;
            }
        }
        if (ok)
        {
            std::lock_guard<std::mutex> lock(m_segMutex);
            if (shortened)
                m_seg.manifest.totalFrames = first + written;
            m_seg.manifest.segments.push_back(record);
            ok = WriteManifest();
        }
        char line[400];
        std::snprintf(line, sizeof(line),
            "RENDER_EXPORT_SEG,phase=closed,segment=%u,of=%u,frames=%llu,expected=%llu,first_sample=%llu,samples=%llu,bytes=%llu,resume_frame=%lld,"
            "resume_ms=%lld,last=%d,ok=%d,file=",
            record.index + 1, m_seg.count, static_cast<unsigned long long>(written), static_cast<unsigned long long>(expected),
            static_cast<unsigned long long>(record.firstSample), static_cast<unsigned long long>(record.samples), static_cast<unsigned long long>(bytes),
            record.hasResume ? static_cast<long long>(record.resumeFrame) : -1ll, record.hasResume ? static_cast<long long>(record.resumeMs) : -1ll,
            last ? 1 : 0, ok ? 1 : 0);
        Log(line + record.file);
        LogMemory("segment", first + written, 0, 0);
        return ok;
    }

    bool ExportController::FinishSegments(bool ok, double audioStartS, std::string& detail, std::string& doneDetail, ExportError& failure)
    {
        namespace sg = segments;
        const std::wstring folder = m_seg.folder;
        const auto keep = [&](const char* why) {
            Log(std::string("RENDER_EXPORT_SEG,phase=kept,why=") + why + ",folder=" + Utf8(folder));
            detail += " (the finished segments are kept: export again with the same settings to continue)";
        };
        if (!ok)
        {
            keep("export_failed");
            return false;
        }
        sg::Manifest manifest;
        {
            // The audio pass of this run joins the segments in the manifest:
            // a later resume needs neither again.
            std::lock_guard<std::mutex> lock(m_segMutex);
            if (!m_job.audioWav.empty())
            {
                m_seg.manifest.audioDone = true;
                m_seg.manifest.audioFile = "audio.wav";
                m_seg.manifest.audioStartS = audioStartS;
                WriteManifest();
            }
            manifest = m_seg.manifest;
        }
        std::wstring wav;
        double startS = 0.0;
        if (manifest.audioDone)
        {
            wav = folder + L"\\" + Wide(manifest.audioFile);
            startS = manifest.audioStartS;
        }
        else if (m_settings.audio == ec::AudioMode::RealtimePass)
        {
            Log("RENDER_EXPORT_SEG,phase=warning,reason=no_audio_pass (the video is joined without sound)");
        }
        std::vector<sg::JoinPart> plan;
        std::string why;
        if (!sg::PlanJoin(manifest, plan, why))
        {
            failure = ExportError::FinalizeFailed;
            detail = "join_failed: " + why;
            keep("join_plan");
            return false;
        }
        std::vector<VideoPart> videos;
        for (const sg::JoinPart& part : plan)
            videos.push_back({folder + L"\\" + Wide(part.file), part.frames});
        EncodeJob job = m_job;
        job.audioWav = wav;
        job.chapters.clear();
        const ex::FrameRate rate = manifest.rate;
        const std::uint64_t total = manifest.totalFrames;
        const std::int64_t expectedUs = replay::projectexport::FrameTimeNs(rate, total) / 1000;
        const std::uint64_t durationMs = static_cast<std::uint64_t>(expectedUs / 1000);
        const std::vector<Chapter> chapters = m_settings.chapters && !m_chapters.empty() ? m_chapters : std::vector<Chapter>{};
        {
            char line[256];
            std::snprintf(line, sizeof(line), "RENDER_EXPORT_SEG,phase=join,segments=%zu,frames=%llu,duration_ms=%llu,audio=%d,audio_start_s=%.4f,file=",
                plan.size(), static_cast<unsigned long long>(total), static_cast<unsigned long long>(durationMs), wav.empty() ? 0 : 1, startS);
            Log(line + Utf8(m_job.finalOutput));
        }
        FinalizeCounts counts;
        std::string error;
        SetFfmpegLogFile(m_job.finalOutput + L".ffmpeg.log");
        const bool joined = LibavFinalizeParts(job, videos, startS, chapters, durationMs, total, error, &counts);
        SetFfmpegLogFile(L"");
        // Checked twice: the packets copied from each segment, and the file
        // read back (its frame count where the container keeps one, its
        // duration within a frame).
        std::size_t bad = plan.size();
        for (std::size_t k = 0; joined && k < plan.size(); ++k)
            if (k >= counts.partPackets.size() || counts.partPackets[k] != plan[k].frames)
            {
                bad = k;
                break;
            }
        std::uint64_t fileFrames = 0;
        std::int64_t fileUs = 0;
        std::string infoError;
        const bool read = joined && LibavVideoInfo(m_job.finalOutput, fileFrames, fileUs, infoError);
        const std::int64_t frameUs = ex::FrameStartNs(rate, 1) / 1000 + 1;
        const std::int64_t offUs = fileUs > expectedUs ? fileUs - expectedUs : expectedUs - fileUs;
        const std::uint64_t audioExpected = wav.empty() ? 0 : replay::projectexport::AudioSampleAt(total, rate, sg::kSampleRate);
        const bool verified = joined && bad == plan.size() && counts.videoPackets == total && read && (fileFrames == 0 || fileFrames == total) &&
            offUs <= frameUs && (wav.empty() || counts.audioLimit == audioExpected);
        {
            char line[400];
            std::snprintf(line, sizeof(line),
                "RENDER_EXPORT_SEG,phase=%s,frames=%llu,expected=%llu,file_frames=%llu,duration_us=%lld,expected_us=%lld,audio_samples=%llu,"
                "audio_expected=%llu,bad_segment=%d,error=",
                verified ? "joined" : "join_failed", static_cast<unsigned long long>(counts.videoPackets), static_cast<unsigned long long>(total),
                static_cast<unsigned long long>(fileFrames), static_cast<long long>(fileUs), static_cast<long long>(expectedUs),
                static_cast<unsigned long long>(counts.audioSamples), static_cast<unsigned long long>(audioExpected),
                bad < plan.size() ? static_cast<int>(bad) + 1 : 0);
            Log(line + (error.empty() ? infoError : error));
        }
        if (verified && !wav.empty() && counts.audioSamples < audioExpected)
            Log("RENDER_EXPORT_SEG,phase=warning,reason=audio_short,samples=" + std::to_string(counts.audioSamples) + ",expected=" +
                std::to_string(audioExpected) + " (the audio pass ended early: the rest is silent)");
        if (!verified)
        {
            DeleteFileW(m_job.finalOutput.c_str());
            failure = ExportError::FinalizeFailed;
            if (bad < plan.size())
            {
                // That segment and the ones after it are written again by the
                // next try (the manifest keeps the ones before).
                std::lock_guard<std::mutex> lock(m_segMutex);
                for (std::size_t k = bad; k < m_seg.manifest.segments.size(); ++k)
                    DeleteFileW((folder + L"\\" + Wide(m_seg.manifest.segments[k].file)).c_str());
                m_seg.manifest.segments.resize(bad);
                WriteManifest();
                detail = "join_failed: segment " + std::to_string(bad + 1) + " holds " +
                    std::to_string(bad < counts.partPackets.size() ? counts.partPackets[bad] : 0) + " of its " + std::to_string(plan[bad].frames) + " frames";
            }
            else
            {
                detail = "join_failed (" + (error.empty() ? (infoError.empty() ? std::string("the joined file does not match its segments") : infoError) : error) +
                    "; see " + Utf8(m_job.finalOutput) + ".ffmpeg.log)";
            }
            keep("join");
            return false;
        }
        DeleteFolder(folder); // the segments, their logs, the manifest and the audio pass
        Log("RENDER_EXPORT_SEG,phase=cleaned,left=" + std::string(GetFileAttributesW(folder.c_str()) == INVALID_FILE_ATTRIBUTES ? "0" : "1") +
            ",folder=" + Utf8(folder));
        m_channel->framesEncoded.store(total);
        doneDetail = Utf8(m_job.finalOutput);
        return true;
    }

    void ExportController::Shutdown(effect_runtime* runtime)
    {
        if (m_armed && !m_finishing)
            Fail(ExportError::Cancelled, "editor_stopped");
        // A finisher waiting for the audio pass stops waiting now (the probe
        // may never tick again); then it is joined.
        m_stopFinisher.store(true);
        if (m_finisher.joinable())
            m_finisher.join();
        if (m_probeThread.joinable())
            m_probeThread.join();
        if (m_closer.joinable())
            m_closer.join();
        if (m_segCloser.joinable())
            m_segCloser.join();
        if (m_encoder)
            m_encoder->Close();
        m_encoder.reset();
        DropProjectParts("shutdown"); // a project export cut short: its pieces go
        // The GPU may still hold the ring: wait for it before freeing (the
        // export is over; the flush this implies does not matter now).
        if (m_gpu.Ready() && runtime)
            runtime->get_command_queue()->wait_idle();
        m_gpu.End(runtime ? runtime->get_device() : nullptr);
        UnhookWindow(); // before the channel goes
        if (m_channel)
            UnmapViewOfFile(m_channel);
        if (m_mapping)
            CloseHandle(m_mapping);
        m_channel = nullptr;
        m_mapping = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_memoryMutex);
            m_memory.Close();
        }
        // The core may unload now (hot swap); the FFmpeg DLLs stay in memory
        // and must not call back into it.
        ReleaseFfmpegLog();
    }
}
