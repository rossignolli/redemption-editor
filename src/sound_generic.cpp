#include "sound_generic.h"

#include "mod_clock.h"
#include "mod_identity.h"
#include "pose_capture.h"
#include "profiler.h"
#include "sound_capture.h" // ResolveAudioBase
#include "sound_census.h"
#include "version.generated.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>

// Generic sounds, see sound_generic.h. The
// detours live in soundfx_probe.cpp (settings, stop, release), sound_census.cpp
// (both creators) and audio_observer.cpp (the two queues); each calls one tap
// below when Capturing(). Everything else is the script thread's.

namespace replay::soundgeneric
{
    namespace
    {
        constexpr std::uint32_t kLifeRing = 8192;     // creates, enqueues, stops, releases: ~400 a second
        constexpr std::uint32_t kAdjustRing = 65536;  // settings: 10,000 a second, peaks of 13,000 in one frame
        constexpr wchar_t kReportFolder[] = L"ReplayDebug";
        constexpr double kLogEveryS = 60.0;

        using LifeRing = EventRing<LifeEvent, kLifeRing>;
        using AdjustRing = EventRing<AdjustEvent, kAdjustRing>;

        int g_iniRead = 0;
        bool g_enabled = false;
        Config g_config;
        LifeRing* g_life = nullptr;
        AdjustRing* g_adjust = nullptr;
        std::uintptr_t g_base = 0;
        std::uintptr_t g_imageEnd = 0;

        // Script thread.
        Recorder g_recorder;
        std::uint64_t g_lastLogUs = 0;
        ClockSampler g_recClock; // SOUND_CLOCK while recording
        std::uint64_t g_recordingGeneration = 0;
        std::shared_ptr<const Stats> g_lastStats;

        std::uint32_t ToRva(const void* address)
        {
            const auto value = reinterpret_cast<std::uintptr_t>(address);
            return value >= g_base && value < g_imageEnd ? static_cast<std::uint32_t>(value - g_base) : 0;
        }

        void ReadIni(const char* path)
        {
            // On by default, a support switch (ini_public.h kSupportSwitches; it installs the sound port detours).
            g_enabled = replay::identity::IniInt("SoundGeneric", 1, path) != 0;
            const int hz = static_cast<int>(replay::identity::DevIniInt("SoundGenericUpdateHz", 20, path));
            g_config.updateHz = static_cast<std::uint32_t>(hz < 1 ? 1 : hz > 120 ? 120 : hz);
            const int mb = static_cast<int>(replay::identity::DevIniInt("SoundGenericMaxMBPerMin", 8, path));
            g_config.maxMBPerMin = mb < 0 ? 0.0 : static_cast<double>(mb);
            // The player's keys.
            g_config.play = replay::identity::DevIniInt("SoundGenericPlay", 1, path) != 0;
            g_config.playInExport = replay::identity::DevIniInt("SoundGenericExport", 0, path) != 0;
            const int preload = static_cast<int>(replay::identity::DevIniInt("SoundGenericPreloadMs", 500, path));
            g_config.preloadMs = static_cast<std::uint32_t>(preload < 0 ? 0 : preload > 5000 ? 5000 : preload);
            const int delay = static_cast<int>(replay::identity::DevIniInt("SoundGenericPreloadDelayMs", 100, path));
            g_config.preloadDelayMs = static_cast<std::uint32_t>(delay < 0 ? 0 : delay);
            const int hold = static_cast<int>(replay::identity::DevIniInt("SoundGenericHoldMs", 150, path));
            g_config.holdMs = static_cast<std::uint32_t>(hold < 0 ? 0 : hold > 2000 ? 2000 : hold);
            const int slots = static_cast<int>(replay::identity::DevIniInt("SoundGenericSlots", 64, path));
            g_config.slots = static_cast<std::uint32_t>(slots < 8 ? 8 : slots > 256 ? 256 : slots);
            char take[512]{};
            replay::identity::DevIniString("SoundGenericTake", "", take, sizeof(take), path);
            g_config.take = take;
            // Always packets, blocked by packet (the mode parsers stay for their tests).
            g_config.packets = true;
            g_config.blockAll = false;
            g_config.stepSetGate = replay::identity::DevIniInt("SoundStepSetGate", 1, path) != 0;
            char trace[96]{};
            replay::identity::DevIniString("SoundGenericTrace", "", trace, sizeof(trace), path);
            g_config.trace = trace;
            g_config.envGroups = replay::identity::DevIniInt("SoundGenericEnvGroup", 1, path) != 0;
            g_config.queueEnvGroups = replay::identity::DevIniInt("SoundQueueEnvGroup", 1, path) != 0;
            g_config.weaponHolderEnv = replay::identity::IniInt("ShotEnvGroupReplay", 1, path) != 0;
            // SoundGenericEntityMode: a, b or c.
            char entityMode[32]{};
            replay::identity::DevIniString("SoundGenericEntityMode", "c", entityMode, sizeof(entityMode), path);
            g_config.entityMode = static_cast<std::uint8_t>(EntityModeOfText(entityMode));
            char skip[256]{};
            replay::identity::DevIniString("SoundGenericSkip", "release", skip, sizeof(skip), path);
            g_config.skip = skip;
            g_config.clockLog = replay::identity::DevIniInt("SoundClock", 1, path) != 0;
            g_config.directParams = static_cast<int>(replay::identity::DevIniInt("SoundGenericDirectParams", 0, path));
            char mute[96]{};
            replay::identity::DevIniString("SoundGenericMute", "", mute, sizeof(mute), path);
            g_config.mute = mute;
            char driven[128]{};
            replay::identity::DevIniString("SoundGenericEntityDrivenSkip", kEntityDrivenSkipDefault, driven, sizeof(driven), path);
            g_config.entityDrivenSkip = driven;
            g_config.playerRoute = replay::identity::DevIniInt("SoundGenericPlayerRoute", 0, path) != 0;
            SetOriginSkip(replay::identity::DevIniInt("SoundGenericOriginSkip", 1, path) != 0);
            g_config.weaponOffShot = replay::identity::DevIniInt("SoundGenericWeaponOffShot", 1, path) != 0;
            // Creator B's packets by ref; loops kept across a jump.
            g_config.coreBByRef = replay::identity::DevIniInt("SoundGenericCoreBByRef", 1, path) != 0;
            g_config.keepLoops = replay::identity::DevIniInt("SoundGenericKeepLoops", 1, path) != 0;
            // The world packets' cap per sound.
            g_config.worldCapPerSound = replay::identity::DevIniInt("SoundGenericWorldCap", 1, path) != 0;
            const int worldCap = static_cast<int>(replay::identity::DevIniInt("SoundGenericWorldCapCount", 6, path));
            g_config.worldCapCount = static_cast<std::uint32_t>(worldCap < 0 ? 0 : worldCap > 64 ? 64 : worldCap);
            // The puppet vehicles' own copies refused at the creators.
            g_config.vehicleBlock = replay::identity::DevIniInt("SoundGenericVehicleBlock", 1, path) != 0;
            g_iniRead = 1;
        }

        void ReadIniOnce()
        {
            if (g_iniRead)
                return;
            char directory[MAX_PATH]{};
            GetCurrentDirectoryA(MAX_PATH, directory);
            const std::string ini = replay::identity::IniPathIn(directory);
            ReadIni(ini.c_str());
        }

        std::string Narrow(const std::wstring& text)
        {
            std::string out;
            for (const wchar_t c : text)
                out += c < 128 ? static_cast<char>(c) : '?';
            return out;
        }

        std::string LocalTimeText()
        {
            SYSTEMTIME time{};
            GetLocalTime(&time);
            char text[32];
            std::snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d:%02d", time.wYear, time.wMonth, time.wDay, time.wHour,
                time.wMinute, time.wSecond);
            return text;
        }

        // The clip time of an event stamped at `timeUs` (QPC), as the other
        // tracks do (a pause between the event and now is not counted).
        std::uint32_t ClipMsOf(std::uint64_t timeUs, std::uint64_t nowUs, std::uint32_t nowMs)
        {
            const std::uint64_t ageMs = nowUs > timeUs ? (nowUs - timeUs) / 1000 : 0;
            return replay::pose::capture::RecordMsAgo(ageMs, nowMs);
        }

        // The audio entity's base from the owner, and its vtable.
        void ResolveAudio(std::uint64_t audio, const replay::census::Owner& found, OwnerInfo& owner)
        {
            replay::census::AudioBase base = replay::census::AudioBase::Image;
            std::uint16_t field = 0;
            std::int32_t offset = 0;
            if (!audio || !replay::capture::ResolveAudioBase(audio, found, base, field, offset))
                return;
            std::uint64_t vtable = 0;
            if (!replay::census::ReadQword(audio, vtable) || vtable < replay::census::ImageBase() ||
                vtable >= replay::census::ImageEnd() || (vtable & 7u) != 0)
                return;
            owner.audioKnown = true;
            owner.audioBase = base;
            owner.audioField = field;
            owner.audioOffset = offset;
            owner.vtableRva = static_cast<std::uint32_t>(vtable - replay::census::ImageBase());
        }

        void Drain(Context& context, bool paused)
        {
            if (!g_life || !g_adjust || !g_recorder.Active())
                return;
            REPLAY_PROFILE_ZONE("rec.sound_generic");
            const std::uint64_t started = replay::clock::NowUs();
            const std::uint32_t nowMs = replay::pose::capture::NowMs();
            Stats& stats = g_recorder.StatsMutable();
            std::uint64_t events = 0;
            std::uint64_t dropped = 0;
            if (paused)
            {
                // The gate: nothing is recorded while the clock is paused.
                stats.discardedPaused += g_life->Pending() + g_adjust->Pending();
                g_life->Skip();
                g_adjust->Skip();
            }
            else
            {
                // The life ring first: a sound's create before its adjusts.
                events += g_life->Drain([&](const LifeEvent& event) {
                    OwnerInfo owner;
                    if (event.kind == LifeKind::Create && !event.refused)
                    {
                        replay::census::Owner found;
                        const bool recorded = replay::census::OwnerOf(event.audio, event.tracker, found) &&
                            found.kind == replay::census::OwnerKind::Recorded;
                        if (recorded)
                        {
                            owner.found = true;
                            owner.id = found.id;
                            owner.cls = found.cls;
                        }
                        else
                            found = replay::census::Owner{}; // no owner: only a static audio entity resolves
                        // The audio entity from the owner
                        // (the capture's rule) and its vtable, for the player.
                        ResolveAudio(event.audio, found, owner);
                    }
                    g_recorder.Life(event, ClipMsOf(event.timeUs, started, nowMs), owner);
                }, dropped);
                stats.lifeDropped += dropped;
                dropped = 0;
                events += g_adjust->Drain([&](const AdjustEvent& event) {
                    g_recorder.AdjustCall(event, ClipMsOf(event.timeUs, started, nowMs));
                }, dropped);
                stats.adjustDropped += dropped;
            }
            g_recorder.Tick(started, nowMs);
            stats.lifePushed = g_life->Pushed();
            stats.adjustPushed = g_adjust->Pushed();
            const double us = static_cast<double>(replay::clock::NowUs() - started);
            ++stats.drainFrames;
            stats.drainUsTotal += us;
            stats.drainUsMax = (std::max)(stats.drainUsMax, us);
            stats.drainEventsMax = (std::max)(stats.drainEventsMax, events);
            if (started >= g_lastLogUs + static_cast<std::uint64_t>(kLogEveryS * 1000000.0))
            {
                g_lastLogUs = started;
                context.log << FormatLogLine(g_recorder.StatsNow(), "record", g_recorder.Live(), g_recorder.Pending()) << '\n';
            }
        }
    }

    void Configure(const std::string& iniPath) { ReadIni(iniPath.c_str()); }

    bool Enabled()
    {
        ReadIniOnce();
        return g_enabled;
    }

    Config Configured()
    {
        ReadIniOnce();
        return g_config;
    }

    bool Install(std::string& summary)
    {
        if (!Enabled())
        {
            summary = "off";
            return false;
        }
        if (g_life && g_adjust)
        {
            summary = "installed";
            return true;
        }
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        g_base = reinterpret_cast<std::uintptr_t>(base);
        g_imageEnd = g_base + nt->OptionalHeader.SizeOfImage;
        try
        {
            if (!g_life)
                g_life = new LifeRing();
            if (!g_adjust)
                g_adjust = new AdjustRing();
        }
        catch (const std::bad_alloc&)
        {
            delete g_life;
            delete g_adjust;
            g_life = nullptr;
            g_adjust = nullptr;
            summary = "no_memory_for_rings";
            return false;
        }
        summary = "installed,update_hz=" + std::to_string(g_config.updateHz) + ",max_mb_per_min=" +
            std::to_string(static_cast<int>(g_config.maxMBPerMin)) + ",life_ring=" + std::to_string(kLifeRing) +
            ",adjust_ring=" + std::to_string(kAdjustRing);
        return true;
    }

    void Shutdown()
    {
        // The unload disabled every hook and drained every detour before
        // this (module_lifetime.h): no tap pushes any more.
        detail::g_capturing.store(false, std::memory_order_release);
        g_recorder.End();
        delete g_life;
        delete g_adjust;
        g_life = nullptr;
        g_adjust = nullptr;
    }

    void BeginRecording(Context& context)
    {
        if (!g_life || !g_adjust)
            return;
        const std::uint64_t now = replay::clock::NowUs();
        // What the detours pushed between recordings (nothing: the flag was
        // off) or what the previous drain left is not this recording's.
        g_life->Skip();
        g_adjust->Skip();
        context.clip.soundGeneric.Clear();
        g_recorder.Begin(g_config, &context.clip.soundGeneric, now);
        g_lastLogUs = now;
        g_recClock.Reset();
        ++g_recordingGeneration;
        detail::g_capturing.store(true, std::memory_order_release);
    }

    void Record(Context& context)
    {
        if (!g_recorder.Active())
            return;
        // The track lives in the clip; a buffer that started over without
        // BeginRecording keeps the recorder on the same track object.
        Drain(context, replay::pose::capture::IsClockPaused());
        // The recording clock against QPC, for the bench's audio capture.
        if (g_config.clockLog)
        {
            ClockSample samples[2];
            const std::size_t n = g_recClock.Tick(replay::pose::capture::NowMs(), replay::clock::NowUs(),
                !replay::pose::capture::IsClockPaused(), samples);
            for (std::size_t i = 0; i < n; ++i)
                context.log << FormatClockLine("record", samples[i]) << '\n';
        }
    }

    namespace
    {
        std::atomic<std::uint64_t> g_variableRefused{0}; // S38 calls the engine refused while recording
    }

    void EndRecording(Context& context)
    {
        if (!g_recorder.Active())
            return;
        detail::g_capturing.store(false, std::memory_order_release);
        Drain(context, replay::pose::capture::IsClockPaused());
        g_recorder.End();
        g_lastStats = std::make_shared<const Stats>(g_recorder.StatsNow());
        context.log << FormatLogLine(*g_lastStats, "stop", g_lastStats->liveAtStop, g_recorder.Pending()) << '\n';
        // The S38 calls the engine refused (no packet), since the module loaded.
        context.log << "SOUND_GENERIC_VARIABLE_REFUSED,calls=" << g_variableRefused.load(std::memory_order_relaxed) << '\n';
    }

    // ---- taps ----

    void OnEnqueued(Source source, std::uint64_t audio, std::uint64_t ref, std::uint64_t timeUs, void* const* frames,
        std::uint32_t count)
    {
        LifeRing* ring = g_life;
        if (!ring)
            return;
        LifeEvent event;
        event.timeUs = timeUs ? timeUs : replay::clock::NowUs();
        event.kind = LifeKind::Enqueue;
        event.source = source;
        event.ref = ref;
        event.audio = audio;
        for (std::uint32_t n = 0; n < count && n < kCallerFrames; ++n)
            event.callers[n] = ToRva(frames[n]);
        ring->Push(event);
    }

    void OnCreated(std::uint8_t core, std::uint64_t ref, std::uint64_t audio, std::uint64_t sound, const std::uint8_t* params,
        const std::uint32_t* callers, std::uint32_t callerCount, bool mine, bool speech)
    {
        LifeRing* ring = g_life;
        if (!ring)
            return;
        LifeEvent event;
        event.timeUs = replay::clock::NowUs();
        event.kind = LifeKind::Create;
        event.core = core;
        event.refused = sound == 0;
        event.speech = speech;
        event.mine = mine;
        event.ref = ref;
        event.audio = audio;
        event.sound = sound;
        event.hasParams = params != nullptr;
        if (params)
        {
            event.params = ReadInitParams(params);
            std::memcpy(&event.tracker, params + kParamsTracker, 8);
        }
        // The census row's frame 0 is its own detour: the frames above it.
        std::uint32_t at = 0;
        for (std::uint32_t n = 1; n < callerCount && at < kCallerFrames; ++n)
            event.callers[at++] = callers[n];
        ring->Push(event);
    }

    void OnStop(std::uint64_t sound, bool forget)
    {
        LifeRing* ring = g_life;
        if (!ring || !sound)
            return;
        LifeEvent event;
        event.timeUs = replay::clock::NowUs();
        event.kind = forget ? LifeKind::StopAndForget : LifeKind::Stop;
        event.sound = sound;
        ring->Push(event);
    }

    void OnRelease(std::uint64_t sound)
    {
        LifeRing* ring = g_life;
        if (!ring || !sound)
            return;
        LifeEvent event;
        event.timeUs = replay::clock::NowUs();
        event.kind = LifeKind::Release;
        event.sound = sound;
        ring->Push(event);
    }

    void OnVariableRefused() { g_variableRefused.fetch_add(1, std::memory_order_relaxed); }

    void OnAdjust(Adjust kind, std::uint64_t sound, const float* value, std::uint32_t hash)
    {
        AdjustRing* ring = g_adjust;
        if (!ring || !sound || !value)
            return;
        AdjustEvent event;
        event.timeUs = replay::clock::NowUs();
        event.sound = sound;
        event.hash = hash;
        event.kind = kind;
        if (IsVector(kind))
            std::memcpy(event.value, value, sizeof(event.value));
        else
            event.value[0] = value[0];
        ring->Push(event);
    }

    // ---- report ----

    std::shared_ptr<const Stats> StatsSnapshot()
    {
        if (g_recorder.Active())
            return std::make_shared<const Stats>(g_recorder.StatsNow());
        return g_lastStats;
    }

    bool WriteReport(const Recording& clip, const Stats& stats, const std::wstring& clipPath, std::uint32_t durationMs,
        std::wstring& reportPath, std::string& error)
    {
        const Track& track = clip.soundGeneric;
        if (track.Empty() && stats.createsSeen == 0)
        {
            error = "empty";
            return false;
        }
        const std::size_t slash = clipPath.find_last_of(L"\\/");
        const std::wstring file = clipPath.substr(slash == std::wstring::npos ? 0 : slash + 1);
        const std::size_t dot = file.find_last_of(L'.');
        const std::wstring stem = dot != std::wstring::npos ? file.substr(0, dot) : file;
        reportPath = replay::storage::Path(kReportFolder) + L"\\" + (stem.empty() ? L"no-file" : stem) + L".sound-generic.txt";
        ReportHeader header;
        header.clip = Narrow(file);
        header.version = replay::build::kVersion;
        header.written = LocalTimeText();
        header.durationMs = durationMs;
        for (const census::SummaryCell& cell : clip.soundCensus.cells)
        {
            header.censusSounds += cell.count;
            header.censusRefused += cell.refused;
        }
        const std::string text = FormatReport(track, stats, header);
        replay::storage::Folder(kReportFolder); // the base folder (storage_paths.h)
        const std::wstring temporary = reportPath + L".tmp";
        HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            error = "create_failed_" + std::to_string(GetLastError());
            return false;
        }
        DWORD written = 0;
        const bool ok = WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE &&
            written == text.size();
        CloseHandle(handle);
        if (!ok || !MoveFileExW(temporary.c_str(), reportPath.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            DeleteFileW(temporary.c_str());
            error = ok ? "rename_failed" : "write_failed";
            return false;
        }
        error.clear();
        return true;
    }
}
