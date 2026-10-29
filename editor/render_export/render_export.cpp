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

    void ExportController::LoadConfig()
    {
        const std::wstring ini = IniPath();
        m_enabled = replay::identity::ProfileIntW(L"Export", L"Enabled", 1, ini.c_str()) != 0; // on by default
        // AssumeLatency, Slots, Ffmpeg, Backend and FfmpegDlls are developer keys;
        // the public build keeps their defaults without reading them.
#if defined(REDEMPTION_DEV)
        const int latency = static_cast<int>(replay::identity::ProfileIntW(L"Export", L"AssumeLatency", -1, ini.c_str()));
        m_slots = std::clamp<std::uint32_t>(replay::identity::ProfileIntW(L"Export", L"Slots", 6, ini.c_str()), 2, 16);
#else
        const int latency = -1;
        m_slots = 6;
#endif
        m_assumedLatency = latency >= 0 ? static_cast<std::uint32_t>(latency) : ~0u;
        m_hdr10 = replay::identity::ProfileIntW(L"Export", L"Hdr10", 0, ini.c_str()) != 0;
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
#if defined(REDEMPTION_DEV)
        replay::identity::ProfileStringW(L"Export", L"Ffmpeg", L"", text, MAX_PATH, ini.c_str());
#endif
        m_ffmpegConfigured = text[0] != 0;
        m_ffmpeg = m_ffmpegConfigured ? text : L"ffmpeg.exe";
        text[0] = 0;
#if defined(REDEMPTION_DEV)
        replay::identity::ProfileStringW(L"Export", L"Backend", L"libav", text, MAX_PATH, ini.c_str());
#else
        wcscpy_s(text, L"libav");
#endif
        m_backendLibav = _wcsicmp(text, L"ffmpeg") != 0 && _wcsicmp(text, L"ffmpeg.exe") != 0;
        {
            std::lock_guard<std::mutex> lock(m_probeMutex);
            m_useFfmpegExe = !m_backendLibav || (m_probe.done && !m_probe.runtimeLoaded && m_ffmpegConfigured);
        }
        text[0] = 0;
        replay::identity::ProfileStringW(L"Export", L"Encoder", L"auto", text, MAX_PATH, ini.c_str());
        ParseBackendChoice(text, m_automaticBackend, m_forcedBackend);
        text[0] = 0;
#if defined(REDEMPTION_DEV)
        replay::identity::ProfileStringW(L"Export", L"FfmpegDlls", L"", text, MAX_PATH, ini.c_str());
#endif
        m_dllFolder = text[0] ? std::wstring(text) : DefaultFfmpegDllFolder();
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
        if (!m_backendLibav)
            return false;
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
            if (!loaded && m_ffmpegConfigured)
                m_useFfmpegExe = true; // no DLLs, but an ffmpeg.exe is configured: the fallback
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
        if (!m_probe.done || m_useFfmpegExe)
            return -1;
        return ChooseVariant(preset, m_probe, m_automaticBackend, m_forcedBackend);
    }

    bool ExportController::PresetAvailable(ec::VideoPreset preset) const
    {
        {
            std::lock_guard<std::mutex> lock(m_probeMutex);
            if (m_useFfmpegExe)
                return InfoOf(preset).available;
            if (!m_probe.done)
                return true; // not known yet
        }
        return VariantFor(preset) >= 0;
    }

    std::string ExportController::BackendLabel(ec::VideoPreset preset) const
    {
        {
            std::lock_guard<std::mutex> lock(m_probeMutex);
            if (m_useFfmpegExe)
                return "ffmpeg.exe";
        }
        const int variant = VariantFor(preset);
        return variant < 0 ? std::string() : std::string(BackendName(AllVariants()[static_cast<std::size_t>(variant)].backend));
    }

    std::string ExportController::EncoderProblem() const
    {
        std::lock_guard<std::mutex> lock(m_probeMutex);
        if (m_useFfmpegExe || !m_probe.done || m_probe.runtimeLoaded)
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

    bool ExportController::RequestExport(const ec::Settings& settings, const std::vector<Chapter>& chapters, std::uint32_t& requestSeq)
    {
        if (!m_enabled || !OpenChannel() || Busy())
            return false;
        if (m_finisher.joinable())
            m_finisher.join();
        m_channel->settings = settings;
        m_channel->cancel.store(0);
        m_channel->error.store(0);
        m_chapters = chapters;
        requestSeq = m_channel->requestSeq.fetch_add(1) + 1;
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
        if (m_armed && m_startQpc.QuadPart)
        {
            status.secondsElapsed = SecondsSince(m_startQpc);
            if (status.framesEncoded > 10 && status.framesTotalHint > status.framesEncoded)
                status.secondsLeft = status.secondsElapsed / static_cast<double>(status.framesEncoded) *
                    static_cast<double>(status.framesTotalHint - status.framesEncoded);
        }
        std::lock_guard<std::mutex> lock(m_statusMutex);
        status.detail = m_detail;
        return status;
    }

    void ExportController::Fail(ExportError error, const std::string& detail)
    {
        Log("RENDER_EXPORT,phase=fail,error=" + std::to_string(static_cast<unsigned>(error)) + ",detail=" + detail);
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_detail = detail;
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
            const std::size_t slash = m_job.finalOutput.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
                SHCreateDirectoryExW(nullptr, m_job.finalOutput.substr(0, slash).c_str(), nullptr);
            if (m_settings.preset == ec::VideoPreset::PngSequence)
                SHCreateDirectoryExW(nullptr, m_job.finalOutput.c_str(), nullptr);
        }
        m_job.output = m_settings.preset == ec::VideoPreset::PngSequence ? m_job.finalOutput : TempVideoPath(m_job.finalOutput);
        if (!segmentCount || segmentIndex == 0)
            DeleteFileW(m_logPath.c_str());
        if (segmentCount)
        {
            // Deleted with the pieces should this one never finish.
            m_project.stray = {m_job.output, WithoutExtension(m_job.finalOutput) + L".audio.wav"};
            Log("RENDER_EXPORT,phase=project_piece,piece=" + std::to_string(segmentIndex + 1) + ",of=" + std::to_string(segmentCount) +
                ",start_ms=" + std::to_string(m_settings.startMs) + ",end_ms=" + std::to_string(m_settings.endMs) + ",file=" + Utf8(m_job.output));
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
        config.slots = m_slots;
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
        bool useFfmpegExe = false;
        {
            std::lock_guard<std::mutex> lock(m_probeMutex);
            useFfmpegExe = m_useFfmpegExe;
        }
        if (!useFfmpegExe && !EnsureRuntime())
        {
            useFfmpegExe = m_ffmpegConfigured; // the documented fallback
            if (!useFfmpegExe)
            {
                m_gpu.End(dev);
                std::lock_guard<std::mutex> lock(m_probeMutex);
                Fail(ExportError::EncoderStart, "ffmpeg_dlls_missing (" + m_runtimeError + ")");
                return;
            }
        }
        if (useFfmpegExe)
        {
            m_encoder = std::make_unique<FfmpegPipeSink>();
        }
        else
        {
            m_job.variant = VariantFor(m_settings.preset);
            m_job.automaticBackend = m_automaticBackend;
            m_job.forcedBackend = static_cast<std::uint32_t>(m_forcedBackend);
            if (m_settings.thumbnail && m_settings.preset != ec::VideoPreset::PngSequence)
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
            m_encoder = std::make_unique<LibavSink>();
        }
        m_encoderLogged = false;
        if (!m_encoder->Open(m_job))
        {
            m_gpu.End(dev);
            Fail(ExportError::EncoderStart, useFfmpegExe ? "ffmpeg_did_not_start (" + Utf8(m_ffmpeg) + ")" : std::string("ffmpeg_runtime_missing"));
            return;
        }
        m_presentIndex = 0;
        m_latency = ~0u;
        m_latencyVotes = 0;
        m_latencyCandidate = ~0u;
        m_lastSerial = ~0ull;
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
        char line[512];
        std::snprintf(line, sizeof(line),
            "RENDER_EXPORT,phase=armed,size=%ux%u,format=%u,color_space=%u,layout=%u,samples=%u,shutter=%.1f,fps=%u/%u,preset=%u,bars=%u+%u,grain=%.2f,slots=%u,frame_bytes=%llu",
            m_width, m_height, static_cast<unsigned>(desc.texture.format), static_cast<unsigned>(space), static_cast<unsigned>(m_job.layout),
            m_settings.samples, m_settings.shutterDegrees, m_settings.rate.num, m_settings.rate.den, static_cast<unsigned>(m_settings.preset),
            config.barTop, config.barBottom, m_settings.grain, m_slots, static_cast<unsigned long long>(m_gpu.FrameBytes()));
        Log(line);
        if (useFfmpegExe)
            Log("RENDER_EXPORT,phase=ffmpeg,args=" + Utf8(BuildFfmpegArgs(m_job)));
        else
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
            m_channel->framesEncoded.store(m_encoder->FramesWritten());
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
            // buffer): the ini's value or 1.
            m_latency = m_assumedLatency != ~0u ? m_assumedLatency : 1u;
            m_channel->latencyPresents.store(m_latency);
            Log("RENDER_EXPORT,phase=latency,presents=" + std::to_string(m_latency) + ",source=" +
                (m_assumedLatency != ~0u ? "ini" : "default"));
        }
        if (m_latency == ~0u || m_presentIndex < m_latency)
            return;
        // The picture on screen now belongs to the tick `latency` presents ago.
        const std::uint64_t serial = m_presentIndex - m_latency;
        ec::TickStamp stamp;
        if (!ec::ReadStamp(*m_channel, serial, stamp))
        {
            if (m_channel->probeSerial.load() > serial + ec::kTickRing)
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
        if (state == ec::State::Preroll || state == ec::State::Capture || state == ec::State::Flush)
        {
            Capture(runtime, state);
            ++m_presentIndex;
        }
        if (!m_armed)
            return;
        DrainFrames(runtime, false);
        PublishTimings();
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
        if (m_settings.audio == ec::AudioMode::RealtimePass && state() == ec::State::AudioSeek && !m_stopFinisher.load())
        {
            auto capture = std::make_unique<ProcessLoopbackCapture>();
            std::string error;
            if (!capture->Start(GetCurrentProcessId(), WithoutExtension(m_job.finalOutput) + L".audio.wav", error)) // up to 5 s
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
        if (m_encoder)
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
                    m_job.audioWav = WithoutExtension(m_job.finalOutput) + L".audio.wav";
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
        // Chapters from the markers (a FFMETADATA file for ffmpeg.exe;
        // libavformat takes them directly).
        const bool chapters = !piece && ok && m_settings.chapters && !m_chapters.empty() && m_settings.preset != ec::VideoPreset::PngSequence;
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
        if (!piece && ok && m_settings.preset != ec::VideoPreset::PngSequence)
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
            const std::wstring& file = piece ? m_project.finalOutput : m_job.finalOutput;
            char line[512];
            std::snprintf(line, sizeof(line), "RENDER_EXPORT,phase=%s,frames=%llu,bytes=%llu,file=%s,detail=%s", ok ? "done" : "failed",
                static_cast<unsigned long long>(channel->framesEncoded.load()),
                static_cast<unsigned long long>(FileBytes(file)), Utf8(file).c_str(), ok ? doneDetail.c_str() : detail.c_str());
            Log(line);
        }
        if (piece)
            m_project = {}; // joined (its pieces deleted) or dropped above
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
        if (m_encoder)
            m_encoder->Close();
        m_encoder.reset();
        DropProjectParts("shutdown"); // a project export cut short: its pieces go
        // The GPU may still hold the ring: wait for it before freeing (the
        // export is over; the flush this implies does not matter now).
        if (m_gpu.Ready() && runtime)
            runtime->get_command_queue()->wait_idle();
        m_gpu.End(runtime ? runtime->get_device() : nullptr);
        if (m_channel)
            UnmapViewOfFile(m_channel);
        if (m_mapping)
            CloseHandle(m_mapping);
        m_channel = nullptr;
        m_mapping = nullptr;
        // The core may unload now (hot swap); the FFmpeg DLLs stay in memory
        // and must not call back into it.
        ReleaseFfmpegLog();
    }
}
