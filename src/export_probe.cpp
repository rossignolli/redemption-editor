// Render export: the probe side state machine (export_probe.h). On unless
// RedemptionEditor.ini [Export] Enabled=0.
//
//   Requested  seek (paused) to the start, wait for the seek settle, lock
//              the game timer (refused without it), mute the game's sound
//   Arming     time held until the add-on is ready (textures, encoder)
//   Preroll    the world runs at the export step, the playhead stands at
//              the start, tag drawn (latency calibration), not encoded
//   Capture    one tick per motion blur sample, the playhead and the world
//              both at the sample's time
//   Flush      tag drawn again, the last pictures reach the present
//   Finishing  timer and sound back; the add-on drains the encoder
//   AudioSeek / AudioPass / AudioDone (Settings::audio == RealtimePass):
//              the range plays again at 1x on the wall clock while the
//              add-on captures the game's audio; it muxes and sets Done.
//              The pass starts only once the add-on acknowledged its
//              capture (export_channel.h AudioAck).

#include "export_probe.h"

#include "export_audio_mute.h"
#include "export_channel.h"
#include "export_schedule.h"
#include "export_timer.h"
#include "mod_identity.h" // [Export] layered with the player's settings file

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace replay::exportprobe
{
    namespace
    {
        namespace ec = replay::exportchannel;
        namespace ex = replay::exporting;

        // render_export::ExportError (editor/render_export/export_types.h).
        constexpr std::uint32_t kErrorChannelMissing = 1;
        constexpr std::uint32_t kErrorTimerNotLocked = 2;
        constexpr std::uint32_t kErrorCancelled = 12;
        constexpr std::uint32_t kErrorAudioNotReady = 16;

        constexpr std::uint32_t kAddonReadyTimeoutTicks = 900; // ~15 s at 60 fps

        bool g_enabled = false;
        bool g_segmentRequest = false; // RequestSegment is calling Request
        HANDLE g_mapping = nullptr;
        ec::Channel* g_channel = nullptr;
        ex::Clock g_clock;
        ex::Tick g_tick;
        bool g_tickValid = false;
        bool g_seekPending = false;
        std::uint64_t g_baseMs = 0;
        std::uint64_t g_wallMs = 0;
        std::uint32_t g_requestSeq = 0;
        std::uint32_t g_timerLead = 1; // [Export] TimerLead
        std::uint32_t g_waitTicks = 0;
        std::int64_t g_publishedStepNs = 0; // the step the next game timer update uses
        std::int64_t g_previousTickStepNs = 0;
        std::uint64_t g_captureStartQpc = 0;
        std::uint64_t g_checkLogs = 0;
        std::uint64_t g_audioDeadlineQpc = 0;
        std::uint64_t g_audioAckWaitQpc = 0; // the settled AudioSeek began waiting for the capture (0: not yet)
        std::uint64_t g_lastProgressFrame = 0;
        ec::State g_seenState = ec::State::Off;
        // The blur's samples per frame in slow motion, and the world scale of
        // the previous tick (timer_check).
        float g_previousWorldScale = 1.0f;
        float g_scaleAhead = 1.0f;          // the value the current cap came from
        std::uint32_t g_spanSamples = 0;    // samples of the frames of the current span (0: none yet)
        std::uint64_t g_spanStartFrame = 0;
        std::uint64_t g_blurReducedFrames = 0;
        std::uint32_t g_blurMinSamples = 0;
        std::uint64_t g_blurSpans = 0;
        std::uint64_t g_frameBelowMinimum = 0;
        std::uint64_t g_clampedTicks = 0;

        std::int64_t ClampMinNs()
        {
            const replay::exporttimer::Stats stats = replay::exporttimer::Snapshot();
            return stats.clampMin > 0.0f ? static_cast<std::int64_t>(std::llround(static_cast<double>(stats.clampMin) * 1e9)) : 0;
        }

        std::uint64_t Qpc()
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            return static_cast<std::uint64_t>(now.QuadPart);
        }

        double QpcSeconds(std::uint64_t ticks)
        {
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            return static_cast<double>(ticks) / static_cast<double>(frequency.QuadPart);
        }

        ec::State State()
        {
            return g_channel ? static_cast<ec::State>(g_channel->state.load()) : ec::State::Off;
        }

        void SetState(ec::State state)
        {
            if (g_channel)
                g_channel->state.store(static_cast<std::uint32_t>(state));
        }

        bool Idle(ec::State state)
        {
            return state == ec::State::Off || state == ec::State::Done || state == ec::State::Failed || state == ec::State::Cancelled;
        }

        void StopClock()
        {
            replay::exporttimer::Release();
            g_clock = ex::Clock{};
            g_tickValid = false;
            g_seekPending = false;
            g_publishedStepNs = 0;
            if (g_channel)
                g_channel->timerLocked.store(0);
        }

        void Fail(std::uint32_t error, const char* reason, std::ostream& log)
        {
            StopClock();
            replay::exportmute::Restore();
            if (g_channel)
            {
                g_channel->error.store(error);
                SetState(ec::State::Failed);
            }
            log << "RENDER_EXPORT,phase=fail,error=" << error << ",reason=" << reason << '\n';
        }

        void LogStages(std::ostream& log)
        {
            if (!g_channel)
                return;
            static const char* const names[ec::kStageCount] = {"gpu_copy", "gpu_accumulate", "gpu_resolve", "gpu_readback",
                "cpu_map", "queue_wait", "frame_wall", "ring_wait"};
            for (std::uint32_t i = 0; i < ec::kStageCount; ++i)
                log << ',' << names[i] << "_us=" << g_channel->stageUs[i].load();
            log << ",queue=" << g_channel->queueDepth.load() << ",encoded=" << g_channel->framesEncoded.load()
                << ",bytes=" << g_channel->bytesWritten.load();
        }

        void LogTimerStats(std::ostream& log)
        {
            const replay::exporttimer::Stats stats = replay::exporttimer::Snapshot();
            log << ",timer_updates=" << stats.lockedUpdates << ",timer_error_ticks=" << stats.errorTicks
                << ",timer_write_failures=" << stats.writeFailures << ",clamp_min=" << stats.clampMin
                << ",clamp_max=" << stats.clampMax << ",period=" << stats.period;
        }

        // The step of the tick after g_tick (TimerLead=1: the timer updates
        // at the start of the frame, before the scripts run, so the step of
        // tick n+1 must be in place when tick n ends).
        std::int64_t PeekNextStepNs()
        {
            ex::Clock copy = g_clock;
            const ex::Tick next = copy.Next();
            return next.phase == ex::Phase::Done || next.phase == ex::Phase::Idle ? 0 : next.stepNs;
        }

        void PublishStep(std::int64_t stepNs)
        {
            g_publishedStepNs = stepNs;
            replay::exporttimer::SetStepNs(stepNs);
        }

        void CheckFrameTime(const TickInput& input, std::ostream& log)
        {
            // GET_FRAME_TIME read in this tick is the game set's step of this
            // frame's timer update: the planned step times the warp (min with
            // the clamp). The first tick ran on the real clock.
            if (g_tick.serial == 0 || !g_channel)
                return;
            const std::int64_t planned = g_timerLead ? g_tick.stepNs : g_previousTickStepNs;
            const replay::exporttimer::Stats stats = replay::exporttimer::Snapshot();
            // The engine's minimum step counts (a step clamped up to it hides
            // under the 0.2 ms tolerance at 100 %), and on the tick the world
            // scale changed either scale is right (export_schedule.h
            // CheckTimerStep).
            const ex::TimerCheck check = ex::CheckTimerStep(static_cast<double>(planned) * 1e-9, static_cast<double>(input.worldScale),
                static_cast<double>(g_previousWorldScale), static_cast<double>(input.frameTime), static_cast<double>(stats.clampMin));
            const bool mismatch = check != ex::TimerCheck::Match;
            if (mismatch)
                g_channel->frameTimeMismatches.fetch_add(1);
            if (check == ex::TimerCheck::Clamped)
                ++g_clampedTicks;
            g_channel->timerErrorTicks.store(stats.errorTicks);
            if (g_checkLogs < 40 || (mismatch && g_channel->frameTimeMismatches.load() <= 50) || g_tick.serial % 240 == 0)
            {
                ++g_checkLogs;
                log << "RENDER_EXPORT,phase=timer_check,serial=" << g_tick.serial << ",tick_phase=" << static_cast<std::uint32_t>(g_tick.phase)
                    << ",frame=" << g_tick.frame << ",sample=" << g_tick.sample << ",step_ns=" << g_tick.stepNs
                    << ",prev_step_ns=" << g_previousTickStepNs << ",lead=" << g_timerLead << ",frame_time=" << input.frameTime
                    << ",world_scale=" << input.worldScale << ",system_step=" << stats.lastSystemStep << ",game_step=" << stats.lastGameStep
                    << ",error_ticks=" << stats.errorTicks << ",mismatch=" << (mismatch ? 1 : 0)
                    << ",check=" << (check == ex::TimerCheck::Clamped ? "clamped" : mismatch ? "mismatch" : "match")
                    << ",samples=" << g_tick.samples << ",prev_world_scale=" << g_previousWorldScale << '\n';
            }
        }
    }

    void Configure(const std::string& iniPath)
    {
        g_enabled = replay::identity::ProfileIntA("Export", "Enabled", 1, iniPath.c_str()) != 0;
#if defined(REDEMPTION_DEV) // a developer key; the public build keeps the default (1)
        g_timerLead = replay::identity::ProfileIntA("Export", "TimerLead", 1, iniPath.c_str()) != 0 ? 1u : 0u;
#else
        g_timerLead = 1u;
#endif
        {
            const std::size_t slash = iniPath.find_last_of("\\/");
            replay::exportmute::Configure(slash == std::string::npos ? std::string(".") : iniPath.substr(0, slash));
        }
        if (!g_enabled || g_channel)
            return;
        wchar_t name[64];
        ec::Name(name, GetCurrentProcessId());
        const std::uint64_t size = sizeof(ec::Channel);
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(size >> 32),
            static_cast<DWORD>(size & 0xFFFFFFFFu), name);
        if (!mapping)
        {
            g_enabled = false;
            return;
        }
        void* view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(ec::Channel));
        if (!view)
        {
            CloseHandle(mapping);
            g_enabled = false;
            return;
        }
        g_mapping = mapping;
        // A hot reload finds the old mapping: made again (an export does not
        // survive a reload; the add-on sees Off and drops its state).
        g_channel = new (view) ec::Channel();
        g_channel->bytes = static_cast<std::uint32_t>(sizeof(ec::Channel));
    }

    bool Enabled() { return g_enabled && g_channel; }

    bool Active() { return g_clock.Active(); }

    bool ShowsExporting() { return Enabled() && !Idle(State()); }

    bool HidesHud()
    {
        if (!Enabled())
            return false;
        const ec::State state = State();
        return state == ec::State::Requested || state == ec::State::Arming || state == ec::State::Preroll ||
            state == ec::State::Capture || state == ec::State::Flush;
    }

    void Request(std::uint32_t requestSeq, std::ostream& log, OutputFramesFn outputFrames, std::uint64_t earliestMs)
    {
        if (!Enabled())
        {
            log << "RENDER_EXPORT,phase=request,status=disabled ([Export] Enabled=0)\n";
            return;
        }
        // Between two pieces of a project export the next one may come.
        if (!Idle(State()) && State() != ec::State::SegmentDone)
        {
            log << "RENDER_EXPORT,phase=request,status=busy\n";
            return;
        }
        ec::Settings& settings = g_channel->settings;
        // The opening hold: the range starts where the playhead can stand.
        if (earliestMs > settings.startMs && earliestMs < settings.endMs)
        {
            log << "RENDER_EXPORT,phase=request,start_ms=" << settings.startMs << ",opening_hold_ms=" << earliestMs << '\n';
            settings.startMs = earliestMs;
        }
        if (!ex::ValidRate(settings.rate) || settings.endMs <= settings.startMs)
        {
            log << "RENDER_EXPORT,phase=request,status=bad_settings\n";
            SetState(ec::State::Failed);
            return;
        }
        g_requestSeq = requestSeq;
        if (!g_segmentRequest)
        {
            // One range of the clip open, not a piece of a project.
            g_channel->segmentIndex.store(0);
            g_channel->segmentCount.store(0);
            g_channel->projectFramesBefore.store(0);
            g_channel->projectFramesTotal.store(0);
        }
        g_waitTicks = 0;
        g_checkLogs = 0;
        g_lastProgressFrame = 0;
        g_captureStartQpc = 0;
        g_channel->addonReady.store(0);
        g_channel->latencyPresents.store(~0u);
        g_channel->framesEncoded.store(0);
        g_channel->framesAccumulated.store(0);
        g_channel->probeSerial.store(0);
        g_channel->audioStartQpc.store(0);
        g_channel->audioEndQpc.store(0);
        g_channel->audioCapture.store(static_cast<std::uint32_t>(ec::AudioAck::Pending));
        g_channel->frameTimeMismatches.store(0);
        g_channel->timerErrorTicks.store(0);
        // The exact frame count, at the speeds the span plays at (a 50 % slow
        // motion doubles it): the capture ends on it (Clock maxFrames).
        const std::uint64_t framesAt1x = (settings.endMs - settings.startMs) * settings.rate.num / (1000ull * settings.rate.den);
        const std::uint64_t frames = outputFrames ? outputFrames(settings.startMs, settings.endMs, settings.rate) : framesAt1x;
        g_channel->framesTotalHint.store(frames ? frames : framesAt1x);
        g_seekPending = true;
        SetState(ec::State::Requested);
        log << "RENDER_EXPORT,phase=request,start_ms=" << settings.startMs << ",end_ms=" << settings.endMs
            << ",fps=" << settings.rate.num << '/' << settings.rate.den << ",samples=" << settings.samples
            << ",shutter=" << settings.shutterDegrees << ",preset=" << static_cast<std::uint32_t>(settings.preset)
            << ",audio=" << static_cast<std::uint32_t>(settings.audio) << ",letterbox=" << settings.letterboxAspect
            << ",grain=" << settings.grain << ",lead=" << g_timerLead << ",frames=" << g_channel->framesTotalHint.load()
            << ",frames_at_1x=" << framesAt1x << '\n';
    }

    bool RequestSegment(std::uint32_t requestSeq, const Segment& segment, std::ostream& log, OutputFramesFn outputFrames)
    {
        if (!Enabled() || (!Idle(State()) && State() != ec::State::SegmentDone) || segment.endMs <= segment.startMs || segment.count == 0)
        {
            log << "RENDER_EXPORT,phase=segment_request,status=refused,index=" << segment.index << ",count=" << segment.count << '\n';
            return false;
        }
        static std::uint32_t thumbnailAsked = 1;
        if (segment.index == 0)
            thumbnailAsked = g_channel->settings.thumbnail; // the editor's, kept for the middle piece
        g_channel->settings.startMs = segment.startMs;
        g_channel->settings.endMs = segment.endMs;
        g_channel->settings.thumbnail = segment.thumbnail ? thumbnailAsked : 0u;
        g_channel->segmentIndex.store(segment.index);
        g_channel->segmentCount.store(segment.count);
        g_channel->projectFramesBefore.store(segment.framesBefore);
        g_channel->cancel.store(0);
        g_channel->error.store(0);
        log << "RENDER_EXPORT,phase=segment,index=" << segment.index << ",count=" << segment.count << ",start_ms=" << segment.startMs
            << ",end_ms=" << segment.endMs << ",frames_before=" << segment.framesBefore << '\n';
        g_segmentRequest = true;
        Request(requestSeq, log, outputFrames, segment.earliestMs);
        g_segmentRequest = false;
        if (State() != ec::State::Requested)
            return false;
        // The whole's estimate: the pieces done, this one's count, the rest planned.
        g_channel->projectFramesTotal.store(segment.framesBefore + g_channel->framesTotalHint.load() + segment.framesAfter);
        return true;
    }

    void BeginProject(std::uint32_t count, std::uint64_t framesPlanned, std::ostream& log)
    {
        if (!Enabled())
            return;
        g_channel->segmentIndex.store(0);
        g_channel->segmentCount.store(count);
        g_channel->projectFramesBefore.store(0);
        g_channel->projectFramesTotal.store(framesPlanned);
        g_channel->framesEncoded.store(0);
        g_channel->framesTotalHint.store(0);
        g_channel->error.store(0);
        g_channel->cancel.store(0);
        SetState(ec::State::SegmentDone);
        log << "RENDER_EXPORT,phase=project,pieces=" << count << ",frames_planned=" << framesPlanned << '\n';
    }

    void ProjectProgress(std::uint32_t nextIndex, std::uint64_t framesBefore)
    {
        if (!Enabled())
            return;
        g_channel->segmentIndex.store(nextIndex);
        g_channel->projectFramesBefore.store(framesBefore);
    }

    std::uint32_t StateValue() { return Enabled() ? static_cast<std::uint32_t>(State()) : 0u; }

    std::uint64_t FramesEncoded() { return Enabled() ? g_channel->framesEncoded.load() : 0; }

    void FailProject(std::uint32_t error, const char* reason, std::ostream& log)
    {
        if (!Enabled())
            return;
        StopClock();
        replay::exportmute::Restore();
        g_channel->error.store(error);
        SetState(ec::State::Failed);
        log << "RENDER_EXPORT,phase=project_fail,error=" << error << ",reason=" << (reason ? reason : "?") << '\n';
    }

    void Cancel(const char* reason, std::ostream& log)
    {
        if (!Enabled() || Idle(State()))
            return;
        StopClock();
        replay::exportmute::Restore();
        g_channel->error.store(kErrorCancelled);
        SetState(ec::State::Cancelled);
        log << "RENDER_EXPORT,phase=cancel,reason=" << (reason ? reason : "?") << '\n';
    }

    TickPlan BeginTick(const TickInput& input, std::ostream& log)
    {
        TickPlan plan;
        if (!Enabled())
            return plan;
        const ec::State state = State();
        if (state != g_seenState)
        {
            // The add-on ends the export (Done or Failed after the finalize):
            // one line for the bench with what it reported.
            if (Idle(state) && !Idle(g_seenState))
                log << "RENDER_EXPORT,phase=end,state=" << static_cast<std::uint32_t>(state) << ",error=" << g_channel->error.load()
                    << ",frames_encoded=" << g_channel->framesEncoded.load() << ",bytes=" << g_channel->bytesWritten.load()
                    << ",latency=" << static_cast<std::int32_t>(g_channel->latencyPresents.load())
                    << ",mismatches=" << g_channel->frameTimeMismatches.load() << '\n';
            g_seenState = state;
        }
        if (Idle(state))
            return plan;
        if (g_channel->cancel.load())
        {
            Cancel(g_channel->error.load() ? "addon_failed" : "editor", log);
            plan.pause = true;
            return plan;
        }
        const ec::Settings& settings = g_channel->settings;
        plan.hideHud = HidesHud();
        switch (state)
        {
        case ec::State::Requested:
            plan.holdTime = true;
            if (g_seekPending)
            {
                // The script seeks (paused) and the seek settle
                // (seek_settle.h) loads the scene around the camera; the
                // preroll then runs the world with the playhead still.
                plan.seekToStart = true;
                plan.startMs = settings.startMs < input.durationMs ? settings.startMs : 0;
                g_seekPending = false;
                return plan;
            }
            if (input.settling)
                return plan;
            {
                std::string error;
                if (!replay::exporttimer::Lock(error))
                {
                    Fail(kErrorTimerNotLocked, error.c_str(), log);
                    return plan;
                }
                const replay::exporttimer::Stats stats = replay::exporttimer::Snapshot();
                log << "RENDER_EXPORT,phase=timer_locked,period=" << stats.period << ",clamp_min=" << stats.clampMin
                    << ",clamp_max=" << stats.clampMax << ",lead=" << g_timerLead << '\n';
                const double minStepS = static_cast<double>(stats.clampMin);
                const double sampleStepS = static_cast<double>(ex::FrameStartNs(settings.rate, 1)) * 1e-9 *
                    (settings.shutterDegrees / 360.0) / static_cast<double>(settings.samples ? settings.samples : 1);
                // The samples are reduced per frame (EndTick), never the world
                // sped up. At 100 % already:
                const std::uint32_t atNormal = ex::SamplesForWorldScale(settings.rate, {settings.samples, settings.shutterDegrees}, 0, 1.0,
                    static_cast<std::int64_t>(std::llround(minStepS * 1e9)));
                if (minStepS > 0.0 && atNormal < ex::ClampSamples(settings.samples))
                    log << "RENDER_EXPORT,phase=warning,reason=blur_samples_reduced,at_speed=1,samples=" << atNormal
                        << ",asked=" << settings.samples << ",sample_step_s=" << sampleStepS << ",clamp_min_s=" << minStepS
                        << " (each sample's world step stays at or above the engine's minimum)\n";
            }
            {
                int sessions = 0;
                const bool muted = replay::exportmute::Mute(sessions);
                log << "RENDER_EXPORT,phase=mute,muted=" << (muted ? 1 : 0) << ",sessions=" << sessions << '\n';
            }
            g_channel->timerLocked.store(1);
            g_waitTicks = 0;
            SetState(ec::State::Arming);
            log << "RENDER_EXPORT,phase=arming,playhead_ms=" << input.playheadMs << '\n';
            return plan;
        case ec::State::Arming:
            plan.holdTime = true;
            if (!g_channel->addonReady.load())
            {
                if (++g_waitTicks > kAddonReadyTimeoutTicks)
                    Fail(kErrorChannelMissing, "addon_not_ready", log);
                return plan;
            }
            {
                ex::Settings clock;
                clock.rate = settings.rate;
                clock.shutter = {settings.samples, settings.shutterDegrees};
                clock.prerollFrames = settings.prerollFrames;
                clock.flushTicks = 8;
                // The exact end: the span's frame count (the playhead reaches
                // END one or two frames late, 481 and 482 frames for 8 s at 60
                // fps). The playhead check in EndTick stays for a clip shorter
                // than the span.
                clock.maxFrames = g_channel->framesTotalHint.load();
                g_clock.Begin(clock);
                g_baseMs = g_wallMs;
                g_previousTickStepNs = 0;
                g_previousWorldScale = input.worldScale;
                g_scaleAhead = input.worldScale;
                g_spanSamples = 0;
                g_spanStartFrame = 0;
                g_blurReducedFrames = 0;
                g_blurMinSamples = ex::ClampSamples(settings.samples);
                g_blurSpans = 0;
                g_frameBelowMinimum = 0;
                g_clampedTicks = 0;
                // The first frame's samples, from the scale applied now.
                g_clock.SetSampleCap(ex::SamplesForWorldScale(settings.rate, {settings.samples, settings.shutterDegrees}, 0,
                    static_cast<double>(input.worldScale), ClampMinNs()));
            }
            plan.resume = true;
            SetState(settings.prerollFrames ? ec::State::Preroll : ec::State::Capture);
            log << "RENDER_EXPORT,phase=start\n";
            [[fallthrough]];
        case ec::State::Preroll:
        case ec::State::Capture:
        case ec::State::Flush:
        {
            g_previousTickStepNs = g_tickValid ? g_tick.stepNs : 0;
            g_tick = g_clock.Next();
            g_tickValid = g_tick.phase != ex::Phase::Done && g_tick.phase != ex::Phase::Idle;
            if (!g_tickValid)
            {
                // The clock is done: the add-on drains its ring and the
                // encoder; the game timer goes back to the wall clock now.
                const double wall = g_captureStartQpc ? QpcSeconds(Qpc() - g_captureStartQpc) : 0.0;
                const std::uint64_t frames = g_clock.FramesEmitted();
                log << "RENDER_EXPORT,phase=video_done,frames=" << frames << ",wall_s=" << wall
                    << ",ms_per_frame=" << (frames ? wall * 1000.0 / static_cast<double>(frames) : 0.0)
                    << ",mismatches=" << g_channel->frameTimeMismatches.load() << ",clamped_ticks=" << g_clampedTicks
                    << ",blur_spans=" << g_blurSpans << ",blur_reduced_frames=" << g_blurReducedFrames
                    << ",blur_min_samples=" << g_blurMinSamples << ",blur_asked=" << settings.samples
                    << ",frames_below_minimum=" << g_frameBelowMinimum;
                LogTimerStats(log);
                LogStages(log);
                log << '\n';
                StopClock();
                replay::exportmute::Restore();
                plan.pause = true;
                if (settings.audio == ec::AudioMode::RealtimePass)
                {
                    g_seekPending = true;
                    g_audioAckWaitQpc = 0;
                    SetState(ec::State::AudioSeek);
                    log << "RENDER_EXPORT,phase=audio_seek\n";
                }
                else
                {
                    SetState(ec::State::Finishing);
                    log << "RENDER_EXPORT,phase=finishing,frames=" << frames << '\n';
                }
                return plan;
            }
            if (g_timerLead == 0)
                PublishStep(g_tick.stepNs);
            else if (g_tick.serial == 0)
                PublishStep(g_tick.stepNs); // this frame's update already ran on the real clock
            if (g_tick.phase == ex::Phase::Capture && !g_captureStartQpc)
            {
                g_captureStartQpc = Qpc();
                g_channel->wallStartQpc.store(g_captureStartQpc);
            }
            CheckFrameTime(input, log);
            g_previousWorldScale = input.worldScale;
            if (g_tick.phase == ex::Phase::Capture && g_tick.firstOfFrame)
            {
                // One line per span of frames with the same blur samples (a
                // slow motion span and its end).
                const std::uint32_t asked = ex::ClampSamples(settings.samples);
                const std::uint64_t frame = static_cast<std::uint64_t>(g_tick.frame);
                if (g_tick.samples < asked)
                {
                    ++g_blurReducedFrames;
                    g_blurMinSamples = std::min(g_blurMinSamples, g_tick.samples);
                }
                const std::int64_t minNs = ClampMinNs();
                if (ex::FrameStepBelowMinimum(settings.rate, g_tick.frame, g_scaleAhead, minNs))
                    ++g_frameBelowMinimum;
                if (g_tick.samples != g_spanSamples && (g_spanSamples != 0 || g_tick.samples < asked))
                {
                    if (g_tick.samples < asked)
                        ++g_blurSpans;
                    log << "RENDER_EXPORT,phase=blur_samples,frame=" << frame << ",video_s=" << static_cast<double>(g_tick.timeNs) * 1e-9
                        << ",playhead_ms=" << input.playheadMs << ",world_scale_ahead=" << g_scaleAhead << ",samples=" << g_tick.samples
                        << ",asked=" << asked << ",previous=" << (g_spanSamples ? g_spanSamples : asked)
                        << ",previous_frames=" << (frame - g_spanStartFrame) << ",clamp_min_s=" << static_cast<double>(minNs) * 1e-9
                        << ",reason=" << (g_tick.samples < asked ? (g_scaleAhead < 1.0f ? "slow_motion" : "engine_minimum") : "restored")
                        << (ex::FrameStepBelowMinimum(settings.rate, g_tick.frame, g_scaleAhead, minNs) ? ",frame_step_below_minimum=1" : "")
                        << '\n';
                    g_spanStartFrame = frame;
                }
                g_spanSamples = g_tick.samples;
            }
            SetState(g_tick.phase == ex::Phase::Preroll ? ec::State::Preroll
                     : g_tick.phase == ex::Phase::Capture ? ec::State::Capture
                                                           : ec::State::Flush);
            plan.active = true;
            plan.drawTag = g_tick.phase != ex::Phase::Capture;
            plan.tagSerial = g_tick.serial;
            // The playhead stands at the start through the preroll and the
            // first sample (the world warms up around it), and at the end
            // through the flush.
            plan.holdTime = g_tick.phase != ex::Phase::Capture || (g_tick.frame == 0 && g_tick.firstOfFrame);
            if (g_tick.phase == ex::Phase::Capture && g_tick.lastOfFrame && g_tick.frame + 1 >= static_cast<std::int64_t>(g_lastProgressFrame + 60))
            {
                g_lastProgressFrame = static_cast<std::uint64_t>(g_tick.frame + 1);
                const double wall = QpcSeconds(Qpc() - g_captureStartQpc);
                log << "RENDER_EXPORT,phase=progress,frame=" << (g_tick.frame + 1) << ",of=" << g_channel->framesTotalHint.load()
                    << ",ticks=" << g_tick.serial << ",ms_per_frame=" << wall * 1000.0 / static_cast<double>(g_tick.frame + 1)
                    << ",latency=" << static_cast<std::int32_t>(g_channel->latencyPresents.load());
                LogStages(log);
                log << '\n';
            }
            return plan;
        }
        case ec::State::AudioSeek:
            plan.holdTime = true;
            if (g_seekPending)
            {
                plan.seekToStart = true;
                plan.startMs = settings.startMs < input.durationMs ? settings.startMs : 0;
                g_seekPending = false;
                return plan;
            }
            if (input.settling)
                return plan;
            {
                // The capture must run before the playhead moves (else the
                // leading sound is lost, or the WAV starts after the pass and
                // is dropped): held until the add-on answers.
                if (!g_audioAckWaitQpc)
                    g_audioAckWaitQpc = Qpc();
                const std::uint64_t waitedMs = static_cast<std::uint64_t>(QpcSeconds(Qpc() - g_audioAckWaitQpc) * 1000.0);
                switch (ec::AudioSeekStepFor(g_channel->audioCapture.load(), waitedMs))
                {
                case ec::AudioSeekStep::Hold:
                    return plan;
                case ec::AudioSeekStep::Fail:
                    Fail(kErrorAudioNotReady, "audio_capture_not_ready", log);
                    plan.pause = true;
                    return plan;
                case ec::AudioSeekStep::SkipAudio:
                    // The add-on logged why; the video is kept without sound.
                    g_channel->audioEndQpc.store(Qpc());
                    plan.pause = true;
                    SetState(ec::State::AudioDone);
                    log << "RENDER_EXPORT,phase=audio_skipped,reason=capture_failed,waited_ms=" << waitedMs << '\n';
                    return plan;
                case ec::AudioSeekStep::Play:
                    log << "RENDER_EXPORT,phase=audio_ready,waited_ms=" << waitedMs << '\n';
                    break;
                }
            }
            // The playhead leaves the start on this tick's transport update,
            // on the wall clock at 1x.
            plan.holdTime = false;
            plan.resume = true;
            g_channel->audioStartQpc.store(Qpc());
            {
                LARGE_INTEGER frequency{};
                QueryPerformanceFrequency(&frequency);
                // The pass plays the span at its speeds in real time: the
                // video's length, twice, and 10 s.
                const std::uint64_t videoMs = g_channel->framesTotalHint.load() * 1000ull * settings.rate.den / settings.rate.num;
                g_audioDeadlineQpc = Qpc() + static_cast<std::uint64_t>(frequency.QuadPart) * (videoMs * 2 / 1000 + 10);
            }
            SetState(ec::State::AudioPass);
            log << "RENDER_EXPORT,phase=audio_pass,start_ms=" << settings.startMs << ",end_ms=" << settings.endMs << '\n';
            return plan;
        case ec::State::AudioPass:
            if (input.playheadMs >= settings.endMs || Qpc() > g_audioDeadlineQpc)
            {
                g_channel->audioEndQpc.store(Qpc());
                plan.pause = true;
                SetState(ec::State::AudioDone);
                log << "RENDER_EXPORT,phase=audio_done,playhead_ms=" << input.playheadMs
                    << ",audio_s=" << QpcSeconds(g_channel->audioEndQpc.load() - g_channel->audioStartQpc.load())
                    << ",deadline=" << (input.playheadMs >= settings.endMs ? 0 : 1) << '\n';
            }
            return plan;
        default:
            return plan;
        }
    }

    ex::FrameRate ExportRate()
    {
        return g_channel && ex::ValidRate(g_channel->settings.rate) ? g_channel->settings.rate : ex::FrameRate{};
    }

    std::uint64_t TransportNowMs(std::uint64_t wallClockMs)
    {
        g_wallMs = wallClockMs;
        if (!g_tickValid)
            return wallClockMs;
        return g_clock.TransportNowMs(g_baseMs);
    }

    void EndTick(std::uint64_t playheadMs, float worldScaleAhead, std::ostream& log)
    {
        (void)log;
        if (!g_tickValid || !g_channel)
            return;
        // The samples of a frame that starts on the next tick, from the lowest
        // world scale it can meet (the clock reads the cap only at a frame's
        // first sample, so the peek below and the next tick agree).
        {
            const ec::Settings& settings = g_channel->settings;
            g_scaleAhead = worldScaleAhead > 0.0f ? worldScaleAhead : 1.0f;
            g_clock.SetSampleCap(ex::SamplesForWorldScale(settings.rate, {settings.samples, settings.shutterDegrees}, g_tick.frame + 1,
                static_cast<double>(g_scaleAhead), ClampMinNs()));
        }
        const bool tagged = g_tick.phase != ex::Phase::Capture;
        ec::WriteStamp(*g_channel, g_tick, static_cast<std::uint32_t>(playheadMs), tagged);
        if (g_tick.phase == ex::Phase::Capture && playheadMs >= g_channel->settings.endMs)
            g_clock.Finish(); // the frame being sampled completes, then flush
        if (g_timerLead)
            PublishStep(PeekNextStepNs());
    }

    bool BenchRequest(const void* settings, std::uint32_t settingsBytes, std::ostream& log)
    {
        if (!Enabled() || !Idle(State()) || settingsBytes != sizeof(ec::Settings))
        {
            log << "RENDER_EXPORT,phase=bench_request,status=" << (!Enabled() ? "disabled" : settingsBytes != sizeof(ec::Settings) ? "layout" : "busy") << '\n';
            return false;
        }
        std::memcpy(&g_channel->settings, settings, sizeof(ec::Settings));
        g_channel->cancel.store(0);
        g_channel->error.store(0);
        const std::uint32_t seq = g_channel->requestSeq.fetch_add(1) + 1;
        Request(seq, log);
        return State() == ec::State::Requested;
    }

    int BenchPoll()
    {
        if (!Enabled())
            return -1;
        const ec::State state = State();
        if (state == ec::State::Done)
            return 1;
        if (state == ec::State::Failed || state == ec::State::Cancelled || state == ec::State::Off)
            return -1;
        return 0;
    }

    void BenchCancel(std::ostream& log)
    {
        if (Enabled() && g_channel)
            g_channel->cancel.store(1);
        Cancel("bench_timeout", log);
    }

    void Shutdown()
    {
        StopClock();
        replay::exportmute::Restore();
        replay::exporttimer::Shutdown();
        if (g_channel)
        {
            if (!Idle(State()))
                SetState(ec::State::Cancelled);
            UnmapViewOfFile(g_channel);
        }
        if (g_mapping)
            CloseHandle(g_mapping);
        g_channel = nullptr;
        g_mapping = nullptr;
    }
}
