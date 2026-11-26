#include "cutscene_audio.h"

#include "natives.h"
#include "storage_paths.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <Windows.h>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

namespace replay::cutsceneaudio
{
    namespace
    {
        constexpr wchar_t kAlias[] = L"RedemptionEditorCutsceneAudio";
        // The archive stream API resolves its string arguments through the
        // game's lower-case JOAAT hash. Many cutscene AWC entries only expose
        // the resulting hash, so the metadata table stores printable strings
        // that resolve to the same hashes.
        constexpr UINT64 kLoadStream = 0x1F1F957154EC51DFull;
        constexpr UINT64 kGetCreatedStreamId = 0x0556C784FA056628ull;
        constexpr UINT64 kPlayStreamFrontend = 0x58FCE43488F9F5F4ull;
        constexpr UINT64 kStopStream = 0xA4718A1419D18151ull;

        struct AudioAsset
        {
            const char* scene;
            const char* container;
            std::uint32_t streamHash;
            std::uint32_t durationMs;
            const char* streamName;
            const char* soundSetName;
        };

        constexpr AudioAsset kAudioAssets[] = {
#include "cutscene_audio_assets.inc"
        };

        std::string g_key;
        std::string g_finishedKey;
        std::uint64_t g_finishedAtSourceMs = 0;
        std::uint32_t g_sourceStartMs = 0;
        std::uint32_t g_finishedSourceStartMs = 0;
        const AudioAsset* g_asset = nullptr;
        std::uint32_t g_durationMs = 0;
        std::uint64_t g_playingSourceMs = 0;
        ULONGLONG g_startedAt = 0;
        int g_streamId = -1;
        bool g_open = false;
        bool g_running = false;
        bool g_paused = false;
        bool g_scrubStopped = false;
        bool g_finishedAtEnd = false;
        bool g_loadFailureLogged = false;
        bool g_streamLoadRequested = false;
        bool g_playbackSkipped = false;
        ULONGLONG g_loadStartedAt = 0;
        ULONGLONG g_streamLoadStartedAt = 0;
        ULONGLONG g_nextLoadAttemptAt = 0;
        std::uint64_t g_requestedSourceMs = 0;
        bool g_mciAudio = false;
        std::wstring g_tempAudioPath;

        bool SceneIdsEqual(const char* left, const char* right)
        {
            if (!left || !right)
                return false;

            while (*left && *right)
            {
                unsigned char leftChar = static_cast<unsigned char>(*left++);
                unsigned char rightChar = static_cast<unsigned char>(*right++);
                if (leftChar >= 'a' && leftChar <= 'z')
                    leftChar = static_cast<unsigned char>(leftChar - 'a' + 'A');
                if (rightChar >= 'a' && rightChar <= 'z')
                    rightChar = static_cast<unsigned char>(rightChar - 'a' + 'A');
                if (leftChar != rightChar)
                    return false;
            }
            return *left == *right;
        }

        const AudioAsset* FindAudioAsset(const char* scene)
        {
            if (!scene || !scene[0])
                return nullptr;
            for (const AudioAsset& asset : kAudioAssets)
                if (SceneIdsEqual(asset.scene, scene))
                    return &asset;
            return nullptr;
        }

        bool IsAudioFile(const std::wstring& path)
        {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
        }

        std::wstring AudioPath(const gate::Span& span, const AudioAsset& asset)
        {
            char fileName[32]{};
            std::snprintf(fileName, sizeof(fileName), "0x%08X.wav", asset.streamHash);
            const std::wstring root = L"CutsceneAudio\\";
            const std::wstring mission = replay::storage::Widen(span.mission);
            const std::wstring scene = replay::storage::Widen(span.cutscene);
            const std::wstring container = replay::storage::Widen(asset.container);
            std::wstring plainContainer = container;
            constexpr wchar_t kMasterSuffix[] = L"_MASTERED_ONLY";
            if (plainContainer.size() >= sizeof(kMasterSuffix) / sizeof(kMasterSuffix[0]) - 1 &&
                plainContainer.compare(plainContainer.size() - (sizeof(kMasterSuffix) / sizeof(kMasterSuffix[0]) - 1),
                    sizeof(kMasterSuffix) / sizeof(kMasterSuffix[0]) - 1, kMasterSuffix) == 0)
            {
                plainContainer.resize(plainContainer.size() - (sizeof(kMasterSuffix) / sizeof(kMasterSuffix[0]) - 1));
            }

            const std::wstring hashFile = replay::storage::Widen(fileName);
            const std::wstring candidates[] = {
                root + mission + L"\\" + scene + L"\\main.wav",
                root + scene + L"\\main.wav",
                root + container + L"\\" + hashFile,
                root + plainContainer + L"\\" + hashFile,
                root + plainContainer + L"\\main.wav",
            };
            for (const std::wstring& candidate : candidates)
            {
                const std::wstring path = replay::storage::Find(candidate);
                if (IsAudioFile(path))
                    return path;
            }
            return replay::storage::Find(candidates[0]);
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
            wchar_t message[256]{};
            if (!mciGetErrorStringW(error, message, static_cast<UINT>(std::size(message))))
                return "mci_error_" + std::to_string(error);
            std::string result;
            for (const wchar_t c : message)
                if (c)
                    result += c < 128 ? static_cast<char>(c) : '?';
            return result;
        }

        bool NumberResult(const std::wstring& command, std::uint32_t& value)
        {
            wchar_t result[64]{};
            const MCIERROR error = Send(command, result, std::size(result));
            if (error != 0 || !result[0])
                return false;
            wchar_t* end = nullptr;
            const unsigned long parsed = std::wcstoul(result, &end, 10);
            if (end == result || parsed > UINT32_MAX)
                return false;
            value = static_cast<std::uint32_t>(parsed);
            return true;
        }

        std::uint32_t SceneAudioStartCorrectionMs(const gate::Span& span)
        {
            // This master track has a leading silent section before dialogue.
            // Keep the recorded archive offset and avoid starting it out of sync.
            if (SceneIdsEqual(span.mission, "RDST2") && SceneIdsEqual(span.cutscene, "RDST2_RSC_1"))
                return 4800;
            return 0;
        }

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

        void StopDevice()
        {
            if (g_mciAudio)
            {
                if (g_open)
                    Send(std::wstring(L"stop ") + kAlias);
            }
            else if (g_streamId >= 0)
            {
                invoke<Void>(kStopStream, g_streamId);
                g_streamId = -1;
                g_streamLoadRequested = false;
            }
            g_running = false;
        }

        void CloseAudio()
        {
            StopDevice();
            if (g_mciAudio && g_open)
                Send(std::wstring(L"close ") + kAlias);
            if (!g_tempAudioPath.empty())
            {
                DeleteFileW(g_tempAudioPath.c_str());
                g_tempAudioPath.clear();
            }
            g_open = false;
            g_mciAudio = false;
            g_paused = false;
            g_scrubStopped = false;
            g_loadFailureLogged = false;
            g_streamLoadRequested = false;
            g_playbackSkipped = false;
            g_loadStartedAt = 0;
            g_streamLoadStartedAt = 0;
            g_nextLoadAttemptAt = 0;
            g_requestedSourceMs = 0;
            g_asset = nullptr;
            g_durationMs = 0;
        }

        bool OpenAudio(const gate::Span& span, const std::vector<gate::Span>& spans,
            const std::string& key, std::ostream& log)
        {
            CloseAudio();
            g_asset = FindAudioAsset(span.cutscene);
            if (!g_asset)
            {
                log << "CUTSCENE_AUDIO,phase=resolve,scene=" << key
                    << ",archive=RDR2,ok=0,error=archive_track_not_mapped\n";
                return false;
            }

            const std::wstring wavPath = AudioPath(span, *g_asset);
            if (IsAudioFile(wavPath))
            {
                std::wstring openPath = ShortPath(wavPath);
                MCIERROR error = Send(std::wstring(L"open \"") + openPath + L"\" type waveaudio alias " + kAlias);
                if (error != 0)
                {
                    std::wstring temporaryCopy;
                    if (MakeShortTempCopy(wavPath, temporaryCopy))
                    {
                        openPath = ShortPath(temporaryCopy);
                        error = Send(std::wstring(L"open \"") + openPath + L"\" type waveaudio alias " + kAlias);
                        if (error == 0)
                            g_tempAudioPath = std::move(temporaryCopy);
                        else
                            DeleteFileW(temporaryCopy.c_str());
                    }
                }
                if (error == 0)
                {
                    g_mciAudio = true;
                    g_open = true;
                    error = Send(std::wstring(L"set ") + kAlias + L" time format milliseconds");
                    if (error != 0 || !NumberResult(std::wstring(L"status ") + kAlias + L" length", g_durationMs) || !g_durationMs)
                    {
                        log << "CUTSCENE_AUDIO,phase=configure,scene=" << key << ",source=local_wav,ok=0,error="
                            << (error ? ErrorText(error) : "length_unavailable") << '\n';
                        CloseAudio();
                        return false;
                    }
                }
                else
                {
                    log << "CUTSCENE_AUDIO,phase=open,scene=" << key << ",source=local_wav,ok=0,error="
                        << ErrorText(error) << '\n';
                }
            }

            if (!g_mciAudio)
                g_durationMs = g_asset->durationMs;
            std::uint32_t spanDurationMs = 0;
            bool usesControlsOffEnd = false;
            const std::uint64_t audioEndMs = AudioEndMs(span, spans, &usesControlsOffEnd);
            std::uint64_t spanDuration = 0;
            if (audioEndMs > span.startMs)
            {
                spanDuration = audioEndMs - span.startMs;
                spanDurationMs = static_cast<std::uint32_t>((std::min<std::uint64_t>)(spanDuration, UINT32_MAX));
            }
            // Align the end of the master to the recorded cutscene/no-control boundary.
            const std::uint64_t sourceStart = spanDuration && spanDuration <= g_durationMs
                ? static_cast<std::uint64_t>(g_durationMs) - spanDuration
                : static_cast<std::uint64_t>(span.audioOffsetMs) + SceneAudioStartCorrectionMs(span);
            g_sourceStartMs = static_cast<std::uint32_t>((std::min<std::uint64_t>)(sourceStart, UINT32_MAX));

            g_open = true;
            g_loadStartedAt = GetTickCount64();
            g_streamLoadRequested = false;
            g_playbackSkipped = false;
            g_streamLoadStartedAt = 0;
            g_nextLoadAttemptAt = 0;
            g_requestedSourceMs = 0;
            g_key = key;
            log << "CUTSCENE_AUDIO,phase=resolve,scene=" << key << ",source="
                << (g_mciAudio ? "local_wav" : "RDR2_archive") << ",ok=1,container="
                << g_asset->container << ",track_hash=0x" << std::hex << g_asset->streamHash << std::dec
                << ",duration_ms=" << g_durationMs << ",span_ms=" << spanDurationMs
                << ",source_start_ms=" << g_sourceStartMs
                << ",end_ms=" << audioEndMs
                << ",alignment=" << (spanDuration && spanDuration <= g_durationMs ? "cutscene_end" : "source_offset")
                << ",end=" << (usesControlsOffEnd ? "controls_off" : "cutscene")
                << '\n';
            return true;
        }

        bool PlayAt(std::uint64_t sourceMs, const std::string& key, std::ostream& log)
        {
            if (!g_open || !g_asset || sourceMs >= g_durationMs || g_playbackSkipped)
                return false;

            if (g_mciAudio)
            {
                const MCIERROR error = Send(std::wstring(L"play ") + kAlias + L" from " + std::to_wstring(sourceMs));
                if (error != 0)
                {
                    log << "CUTSCENE_AUDIO,phase=play,scene=" << key << ",source=local_wav,ok=0,error="
                        << ErrorText(error) << '\n';
                    return false;
                }
                g_running = true;
                g_paused = false;
                g_scrubStopped = false;
                g_playingSourceMs = sourceMs;
                g_startedAt = GetTickCount64();
                return true;
            }

            if (!g_streamLoadRequested)
            {
                if (sourceMs > 1000)
                {
                    log << "CUTSCENE_AUDIO,phase=load,scene=" << key
                        << ",archive=RDR2,ok=0,error=stream_seek_unsupported\n";
                    g_loadFailureLogged = true;
                    g_playbackSkipped = true;
                    return false;
                }

                const ULONGLONG now = GetTickCount64();
                if (now < g_nextLoadAttemptAt)
                    return false;

                const BOOL loaded = invoke<BOOL>(kLoadStream,
                    g_asset->streamName, g_asset->soundSetName);
                if (!loaded)
                {
                    if (!g_loadFailureLogged)
                    {
                        log << "CUTSCENE_AUDIO,phase=load,scene=" << key
                            << ",archive=RDR2,ok=0,error=stream_not_ready\n";
                        g_loadFailureLogged = true;
                    }
                    g_nextLoadAttemptAt = now + 500;
                    return false;
                }

                g_streamLoadRequested = true;
                g_streamLoadStartedAt = now;
                g_requestedSourceMs = sourceMs;
            }

            const int streamId = invoke<int>(kGetCreatedStreamId,
                g_asset->streamName, g_asset->soundSetName);
            if (streamId < 0)
            {
                if (!g_loadFailureLogged)
                {
                    log << "CUTSCENE_AUDIO,phase=load,scene=" << key
                        << ",archive=RDR2,ok=0,error=stream_id_pending\n";
                    g_loadFailureLogged = true;
                }
                return false;
            }

            const ULONGLONG now = GetTickCount64();
            if ((now >= g_streamLoadStartedAt && now - g_streamLoadStartedAt > 1000) ||
                sourceMs > g_requestedSourceMs + 1000)
            {
                invoke<Void>(kStopStream, streamId);
                g_streamLoadRequested = false;
                g_playbackSkipped = true;
                log << "CUTSCENE_AUDIO,phase=load,scene=" << key
                    << ",archive=RDR2,ok=0,error=stream_started_too_late\n";
                return false;
            }

            invoke<Void>(kPlayStreamFrontend, streamId);
            g_streamId = streamId;
            g_running = true;
            g_paused = false;
            g_scrubStopped = false;
            g_loadFailureLogged = false;
            g_streamLoadRequested = true;
            g_playingSourceMs = sourceMs;
            g_startedAt = now;
            return true;
        }

        void PauseAudio()
        {
            StopDevice();
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
        (void)playerModel; // The archive master track already contains the full mix.
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
        const std::uint64_t sourceStartMs = key == g_key ? g_sourceStartMs :
            (key == g_finishedKey ? g_finishedSourceStartMs : 0);
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
            if (!OpenAudio(*span, spans, key, log))
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
            if (g_streamLoadRequested && !g_running)
                g_playbackSkipped = true;
            g_paused = false;
            g_scrubStopped = true;
            return;
        }
        if (paused)
        {
            if (g_running)
                PauseAudio();
            if (g_streamLoadRequested && !g_running)
                g_playbackSkipped = true;
            return;
        }

        const ULONGLONG now = GetTickCount64();
        if (g_running)
        {
            const std::uint64_t elapsed = now >= g_startedAt ? now - g_startedAt : 0;
            const std::uint64_t expected = g_playingSourceMs + elapsed;
            const std::uint64_t drift = expected > sourceMs ? expected - sourceMs : sourceMs - expected;
            if (drift > (g_mciAudio ? 350u : 1000u))
            {
                StopDevice();
                if (g_mciAudio)
                {
                    PlayAt(sourceMs, key, log);
                    log << "CUTSCENE_AUDIO,phase=resync,scene=" << key << ",source=local_wav,source_ms="
                        << sourceMs << ",drift_ms=" << drift << '\n';
                }
                else
                {
                    g_playbackSkipped = true;
                    log << "CUTSCENE_AUDIO,phase=stop,scene=" << key << ",reason=stream_seek_unsupported"
                        << ",source_ms=" << sourceMs << ",drift_ms=" << drift << '\n';
                }
            }
            return;
        }

        if (!PlayAt(sourceMs, key, log) && !g_playbackSkipped && g_loadStartedAt && now >= g_loadStartedAt &&
            now - g_loadStartedAt > 5000)
        {
            log << "CUTSCENE_AUDIO,phase=load,scene=" << key
                << ",archive=RDR2,ok=0,error=stream_load_timeout\n";
            CloseAudio();
            g_key.clear();
            g_finishedKey = key;
            g_finishedAtEnd = false;
        }
        else if (g_running)
        {
            log << "CUTSCENE_AUDIO,phase=play,scene=" << key << ",source_ms=" << sourceMs
                << ",resume=" << (g_paused || g_scrubStopped ? 1 : 0) << '\n';
        }
    }
}
