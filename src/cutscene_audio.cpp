#include "cutscene_audio.h"

#include "storage_paths.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <ostream>
#include <string>
#include <utility>

#include <Windows.h>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

namespace replay::cutsceneaudio
{
    namespace
    {
        constexpr wchar_t kAlias[] = L"RedemptionEditorCutsceneAudio";
        constexpr std::uint32_t kArthurPlayerModel = 0x0D7114C9u; // player_zero
        constexpr std::uint32_t kJohnPlayerModel = 0x00B69710u;   // player_three
        std::string g_key;
        std::string g_finishedKey;
        std::uint64_t g_finishedAtSourceMs = 0;
        std::uint32_t g_sourceStartMs = 0;
        std::uint32_t g_finishedSourceStartMs = 0;
        std::wstring g_tempAudioPath;
        std::uint32_t g_durationMs = 0;
        std::uint64_t g_playingSourceMs = 0;
        ULONGLONG g_startedAt = 0;
        bool g_open = false;
        bool g_running = false;
        bool g_paused = false;
        bool g_scrubStopped = false;
        bool g_finishedAtEnd = false;

        std::uint64_t AudioEndMs(const gate::Span& cutscene, const std::vector<gate::Span>& spans,
            bool* usesControlsOff = nullptr)
        {
            if (usesControlsOff)
                *usesControlsOff = false;

            std::uint64_t endMs = cutscene.endMs;
            if (cutscene.endMs <= cutscene.startMs)
                return endMs;

            constexpr std::uint64_t kControlsOffStartToleranceMs = 2000;
            for (const gate::Span& span : spans)
            {
                if (span.kind != gate::SpanKind::ControlsOff || span.endMs <= span.startMs ||
                    span.startMs >= cutscene.endMs || span.endMs <= cutscene.startMs ||
                    span.endMs <= endMs)
                    continue;

                const std::uint64_t startDifference = span.startMs > cutscene.startMs
                    ? static_cast<std::uint64_t>(span.startMs) - cutscene.startMs
                    : static_cast<std::uint64_t>(cutscene.startMs) - span.startMs;
                if (startDifference > kControlsOffStartToleranceMs)
                    continue;

                endMs = span.endMs;
                if (usesControlsOff)
                    *usesControlsOff = true;
            }
            return endMs;
        }

        bool IsAudioFile(const std::wstring& path)
        {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
        }

        std::wstring AudioPath(const gate::Span& span, const wchar_t* file)
        {
            const std::wstring mission(span.mission, span.mission + std::char_traits<char>::length(span.mission));
            const std::wstring scene(span.cutscene, span.cutscene + std::char_traits<char>::length(span.cutscene));
            const std::wstring missionPath = replay::storage::Find(
                std::wstring(L"CutsceneAudio\\") + mission + L"\\" + scene + L"\\" + file);
            if (IsAudioFile(missionPath))
                return missionPath;

            // Some mastered containers are named for the scene and are shared
            // across mission variants. This flat scene folder lets one export
            // serve every recorded mission ID that names that same cutscene.
            const std::wstring scenePath = replay::storage::Find(
                std::wstring(L"CutsceneAudio\\") + scene + L"\\" + file);
            if (IsAudioFile(scenePath))
                return scenePath;
            return missionPath;
        }

        const wchar_t* PlayerTrackFile(std::uint32_t playerModel, const char*& trackName)
        {
            if (playerModel == kArthurPlayerModel)
            {
                trackName = "arthur";
                return L"arthur.wav";
            }
            if (playerModel == kJohnPlayerModel)
            {
                trackName = "john";
                return L"john.wav";
            }
            trackName = "main";
            return nullptr;
        }

        MCIERROR Send(const std::wstring& command, wchar_t* result = nullptr, std::size_t resultCount = 0)
        {
            if (result && resultCount)
                result[0] = L'\0';
            return mciSendStringW(command.c_str(), result, result ? static_cast<UINT>(resultCount) : 0, nullptr);
        }

        std::wstring ShortPath(const std::wstring& path)
        {
            const DWORD needed = GetShortPathNameW(path.c_str(), nullptr, 0);
            if (!needed)
                return path;
            std::wstring shortened(needed, L'\0');
            const DWORD length = GetShortPathNameW(path.c_str(), shortened.data(), needed);
            if (!length || length >= needed)
                return path;
            shortened.resize(length);
            return shortened;
        }

        bool MakeShortTempCopy(const std::wstring& source, std::wstring& copy)
        {
            wchar_t directory[MAX_PATH]{};
            const DWORD directoryLength = GetTempPathW(MAX_PATH, directory);
            if (!directoryLength || directoryLength >= MAX_PATH)
                return false;
            wchar_t temporary[MAX_PATH]{};
            if (!GetTempFileNameW(directory, L"REA", 0, temporary))
                return false;
            DeleteFileW(temporary);

            std::wstring wavPath = temporary;
            const std::size_t extension = wavPath.find_last_of(L'.');
            if (extension != std::wstring::npos)
                wavPath.resize(extension);
            wavPath += L".wav";
            if (!CopyFileW(source.c_str(), wavPath.c_str(), TRUE))
                return false;
            copy = std::move(wavPath);
            return true;
        }

        std::string ErrorText(MCIERROR error)
        {
            wchar_t text[256]{};
            if (!mciGetErrorStringW(error, text, static_cast<UINT>(std::size(text))))
                return "mci_error_" + std::to_string(error);
            std::string out;
            for (const wchar_t c : text)
                if (c)
                    out += c < 128 ? static_cast<char>(c) : '?';
            return out;
        }

        bool NumberResult(const std::wstring& command, std::uint32_t& value)
        {
            wchar_t text[64]{};
            const MCIERROR error = Send(command, text, std::size(text));
            if (error != 0 || !text[0])
                return false;
            wchar_t* end = nullptr;
            const unsigned long parsed = std::wcstoul(text, &end, 10);
            if (end == text || parsed > UINT32_MAX)
                return false;
            value = static_cast<std::uint32_t>(parsed);
            return true;
        }

        void CloseAudio()
        {
            if (g_open)
            {
                Send(std::wstring(L"stop ") + kAlias);
                Send(std::wstring(L"close ") + kAlias);
            }
            g_open = false;
            g_running = false;
            g_paused = false;
            g_scrubStopped = false;
            g_durationMs = 0;
            if (!g_tempAudioPath.empty())
            {
                DeleteFileW(g_tempAudioPath.c_str());
                g_tempAudioPath.clear();
            }
        }

        std::uint32_t SceneAudioStartCorrectionMs(const gate::Span& span);

        bool OpenAudio(const gate::Span& span, const std::vector<gate::Span>& spans,
            std::uint32_t playerModel, const std::string& key, std::ostream& log)
        {
            CloseAudio();
            const char* trackName = "main";
            const wchar_t* variantFile = PlayerTrackFile(playerModel, trackName);
            std::wstring path = variantFile ? AudioPath(span, variantFile) : std::wstring();
            if (!IsAudioFile(path))
            {
                trackName = "main";
                path = AudioPath(span, L"main.wav");
            }
            if (!IsAudioFile(path))
            {
                log << "CUTSCENE_AUDIO,phase=resolve,scene=" << key << ",ok=0,error="
                    << "main_wav_missing" << ",path=";
                for (const wchar_t c : path)
                    log << (c < 128 ? static_cast<char>(c) : '?');
                log << '\n';
                return false;
            }

            std::wstring openPath = ShortPath(path);
            MCIERROR error = Send(std::wstring(L"open \"") + openPath + L"\" type waveaudio alias " + kAlias);
            if (error != 0)
            {
                std::wstring temporaryCopy;
                if (MakeShortTempCopy(path, temporaryCopy))
                {
                    openPath = ShortPath(temporaryCopy);
                    error = Send(std::wstring(L"open \"") + openPath + L"\" type waveaudio alias " + kAlias);
                    if (error == 0)
                        g_tempAudioPath = std::move(temporaryCopy);
                    else
                        DeleteFileW(temporaryCopy.c_str());
                }
            }
            if (error != 0)
            {
                log << "CUTSCENE_AUDIO,phase=open,scene=" << key << ",ok=0,error=" << ErrorText(error) << '\n';
                return false;
            }
            g_open = true;
            error = Send(std::wstring(L"set ") + kAlias + L" time format milliseconds");
            if (error != 0 || !NumberResult(std::wstring(L"status ") + kAlias + L" length", g_durationMs) || !g_durationMs)
            {
                log << "CUTSCENE_AUDIO,phase=configure,scene=" << key << ",ok=0,error="
                    << (error ? ErrorText(error) : "length_unavailable") << '\n';
                CloseAudio();
                return false;
            }
            std::uint32_t spanDurationMs = 0;
            bool endAligned = false;
            bool usesControlsOffEnd = false;
            const std::uint64_t audioEndMs = AudioEndMs(span, spans, &usesControlsOffEnd);
            if (audioEndMs > span.startMs)
            {
                const std::uint64_t spanDuration = audioEndMs - span.startMs;
                spanDurationMs = static_cast<std::uint32_t>((std::min<std::uint64_t>)(spanDuration, UINT32_MAX));
                if (spanDuration <= g_durationMs)
                {
                    // Play the master at its natural speed and put its end on
                    // the no-control boundary associated with this cutscene.
                    g_sourceStartMs = static_cast<std::uint32_t>(g_durationMs - spanDuration);
                    endAligned = true;
                }
            }
            if (!endAligned)
            {
                const std::uint64_t sourceStart = static_cast<std::uint64_t>(span.audioOffsetMs) +
                    SceneAudioStartCorrectionMs(span);
                g_sourceStartMs = static_cast<std::uint32_t>((std::min<std::uint64_t>)(sourceStart, UINT32_MAX));
            }
            g_key = key;
            log << "CUTSCENE_AUDIO,phase=open,scene=" << key << ",ok=1,track=" << trackName
                << ",duration_ms=" << g_durationMs << ",span_ms=" << spanDurationMs
                << ",source_start_ms=" << g_sourceStartMs
                << ",end_ms=" << audioEndMs
                << ",alignment=" << (endAligned ? (usesControlsOffEnd ? "controls_off_end" : "cutscene_end") : "source") << '\n';
            return true;
        }

        void StopDevice()
        {
            if (g_open)
                Send(std::wstring(L"stop ") + kAlias);
        }

        bool PlayAt(std::uint64_t sourceMs, const std::string& key, std::ostream& log)
        {
            if (!g_open || sourceMs >= g_durationMs)
                return false;
            const MCIERROR error = Send(std::wstring(L"play ") + kAlias + L" from " + std::to_wstring(sourceMs));
            if (error != 0)
            {
                log << "CUTSCENE_AUDIO,phase=play,scene=" << key << ",ok=0,error=" << ErrorText(error) << '\n';
                return false;
            }
            g_running = true;
            g_paused = false;
            g_scrubStopped = false;
            g_playingSourceMs = sourceMs;
            g_startedAt = GetTickCount64();
            return true;
        }

        void PauseAudio()
        {
            if (g_open)
                Send(std::wstring(L"pause ") + kAlias);
            g_running = false;
            g_paused = true;
        }

        const gate::Span* LatestCutsceneAt(const std::vector<gate::Span>& spans, std::uint64_t playheadMs)
        {
            const gate::Span* latest = nullptr;
            for (const gate::Span& span : spans)
            {
                if (span.kind != gate::SpanKind::Cutscene || !span.mission[0] || !span.cutscene[0] ||
                    playheadMs < span.startMs)
                    continue;
                const std::uint64_t audioEndMs = AudioEndMs(span, spans);
                if (audioEndMs > span.startMs && playheadMs >= audioEndMs)
                    continue;
                if (!latest || span.startMs >= latest->startMs)
                    latest = &span;
            }
            return latest;
        }

        std::uint32_t SceneAudioStartCorrectionMs(const gate::Span& span)
        {
            // Report 050 confirms that this master track's first dialogue is
            // about 4.8 seconds after the recorded cutscene marker because of
            // the track's leading silence. Skip that silent lead-in on replay.
            if (std::strcmp(span.mission, "RDST2") == 0 && std::strcmp(span.cutscene, "RDST2_RSC_1") == 0)
                return 4800;
            return 0;
        }
    }

    void Stop(std::ostream* log)
    {
        if (log && (!g_key.empty() || !g_finishedKey.empty() || g_open || g_running || g_paused))
            *log << "CUTSCENE_AUDIO,phase=stop,scene=" << g_key << '\n';
        CloseAudio();
        g_key.clear();
        g_finishedKey.clear();
        g_finishedAtSourceMs = 0;
        g_sourceStartMs = 0;
        g_finishedSourceStartMs = 0;
        g_finishedAtEnd = false;
    }

    void Update(const std::vector<gate::Span>& spans, std::uint32_t playerModel,
        std::uint64_t playheadMs, bool playing, bool paused, bool scrubbing,
        double rate, std::ostream& log)
    {
        if (!playing)
        {
            if (!g_key.empty() || !g_finishedKey.empty() || g_open)
                Stop(&log);
            return;
        }

        const gate::Span* span = LatestCutsceneAt(spans, playheadMs);
        if (!span)
        {
            if (!g_key.empty() || !g_finishedKey.empty() || g_open)
                Stop(&log);
            return;
        }
        char keyBuffer[160]{};
        std::snprintf(keyBuffer, sizeof(keyBuffer), "%s/%s@%u", span->mission, span->cutscene, span->startMs);
        const std::string key = keyBuffer;
        const std::uint64_t elapsedMs = playheadMs - span->startMs;
        const std::uint64_t fallbackStartMs = static_cast<std::uint64_t>(span->audioOffsetMs) +
            SceneAudioStartCorrectionMs(*span);
        const std::uint64_t sourceStartMs = key == g_key ? g_sourceStartMs :
            (key == g_finishedKey ? g_finishedSourceStartMs : fallbackStartMs);
        std::uint64_t sourceMs = sourceStartMs + elapsedMs;

        if (g_finishedKey == key && g_finishedAtEnd && sourceMs < g_finishedAtSourceMs)
        {
            g_finishedKey.clear();
            g_finishedAtEnd = false;
        }
        if (key != g_key)
        {
            CloseAudio();
            g_key.clear();
            if (g_finishedKey == key)
                return;
            if (!OpenAudio(*span, spans, playerModel, key, log))
            {
                g_finishedKey = key;
                g_finishedAtEnd = false;
                return;
            }
            sourceMs = static_cast<std::uint64_t>(g_sourceStartMs) + elapsedMs;
        }
        if (g_finishedKey == key)
            return;
        if (sourceMs >= g_durationMs)
        {
            g_finishedSourceStartMs = g_sourceStartMs;
            StopDevice();
            CloseAudio();
            g_key.clear();
            g_finishedKey = key;
            g_finishedAtSourceMs = sourceMs;
            g_finishedAtEnd = true;
            log << "CUTSCENE_AUDIO,phase=end,scene=" << key << ",source_ms=" << sourceMs << '\n';
            return;
        }

        const bool rateSupported = rate >= 0.995 && rate <= 1.005;
        if (scrubbing || !rateSupported)
        {
            if (g_running)
                StopDevice();
            g_running = false;
            g_paused = false;
            g_scrubStopped = true;
            return;
        }
        if (paused)
        {
            if (g_running)
                PauseAudio();
            return;
        }

        const ULONGLONG now = GetTickCount64();
        if (g_running)
        {
            const std::uint64_t elapsed = now >= g_startedAt ? now - g_startedAt : 0;
            const std::uint64_t expected = g_playingSourceMs + elapsed;
            const std::uint64_t drift = expected > sourceMs ? expected - sourceMs : sourceMs - expected;
            if (drift > 350)
            {
                StopDevice();
                PlayAt(sourceMs, key, log);
                log << "CUTSCENE_AUDIO,phase=resync,scene=" << key << ",source_ms=" << sourceMs
                    << ",drift_ms=" << drift << '\n';
            }
            return;
        }

        if (PlayAt(sourceMs, key, log))
            log << "CUTSCENE_AUDIO,phase=play,scene=" << key << ",source_ms=" << sourceMs
                << ",resume=" << (g_paused || g_scrubStopped ? 1 : 0) << '\n';
    }
}
