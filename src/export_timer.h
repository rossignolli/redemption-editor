#pragma once

// Render export: the game timer locked to the export clock.
//
// Why the playhead alone is not enough: the puppets follow the playhead, but
// the rest of the world (particles, fire, water, cloth, wind, ambient
// animation, the camera's own smoothing) advances by the game's frame step,
// which rage::fwTimer takes from QueryPerformanceCounter. While exporting a
// frame may take a second of real time, so that step must be the export
// clock's step, every frame, before the game updates.
//
// How (rung 5 of the engine access ladder, build 1491.50):
//   fwTimer::Update (RVA 0x2B13D3C, prologue signature checked) computes
//     realDelta = float(QPC() - [0x59B3CA8]) * [0x542819C]
//   and then stores a second QPC() into [0x59B3CA8]; every time set is
//   advanced by realDelta (times the warp: Dead Eye, SET_TIME_SCALE and the
//   other slots still apply on top, as in normal play).
//   The detour forwards every call to the original. Only while an export
//   holds the lock, right before forwarding, it writes [0x59B3CA8] =
//   QPC() - ticks(step), so the original's own subtraction yields the
//   export step (plus the few ticks between our QPC read and its own). The
//   delta the engine really used is read back after the call from the
//   system time set (0x59B38F0 +0x0C, warp 1.0, never clamped) and the
//   difference is carried into the next frame (StepPlanner): no drift. The
//   original overwrites [0x59B3CA8] with a real QPC at the end of every
//   call, so nothing fake survives the lock.
// Signature checks: the prologue, the two instruction sites that read and
// store [0x59B3CA8] and the time set helper's store of +0x0C. Any mismatch
// refuses the install (and the export is refused, never recorded on the
// wall clock).
//
// The clamp of the clamped sets ([0x3CC8388] min, [0x3CC838C] max step) is
// read at install and logged: a step below the minimum (high fps times many
// motion blur samples) would make the world run faster than the export;
// the probe compares GET_FRAME_TIME with the planned step every tick and
// logs the mismatches (RENDER_EXPORT,phase=timer_check).
//
// Script fiber calls (Install, SetStepNs, Lock, Release, Stats); the
// detour runs on the game's main thread and never calls natives.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>

namespace replay::exporttimer
{
    // Pure: how many counter ticks each locked frame asks for, so the sum of
    // the deltas the engine really used follows the sum of the export steps
    // exactly (rounded to one tick), whatever the gap between our counter
    // read and the engine's. Unit tested (tests/export_schedule_tests.cpp).
    class StepPlanner
    {
    public:
        void Begin(double ticksPerSecond) noexcept
        {
            m_ticksPerSecond = ticksPerSecond > 0.0 ? ticksPerSecond : 1.0;
            m_wantNs = 0;
            m_appliedTicks = 0;
            m_frames = 0;
        }

        // The ticks to subtract from "now" for a frame of `stepNs`.
        std::int64_t Issue(std::int64_t stepNs) noexcept
        {
            if (stepNs < 0)
                stepNs = 0;
            m_wantNs += stepNs;
            const std::int64_t target = TicksOf(m_wantNs);
            std::int64_t ticks = target - m_appliedTicks;
            // Never a negative or zero delta (the engine divides by the step).
            if (ticks < 1)
                ticks = 1;
            m_lastIssued = ticks;
            return ticks;
        }

        // The ticks the engine really used for that frame (read back).
        void Applied(std::int64_t ticks) noexcept
        {
            m_appliedTicks += ticks;
            ++m_frames;
        }

        std::int64_t ErrorTicks() const noexcept { return m_appliedTicks - TicksOf(m_wantNs); }
        std::int64_t LastIssued() const noexcept { return m_lastIssued; }
        std::uint64_t Frames() const noexcept { return m_frames; }
        double TicksPerSecond() const noexcept { return m_ticksPerSecond; }

        std::int64_t TicksOf(std::int64_t ns) const noexcept
        {
            return static_cast<std::int64_t>(std::llround(static_cast<double>(ns) * m_ticksPerSecond / 1e9));
        }

    private:
        double m_ticksPerSecond = 1.0;
        std::int64_t m_wantNs = 0;
        std::int64_t m_appliedTicks = 0;
        std::int64_t m_lastIssued = 0;
        std::uint64_t m_frames = 0;
    };

    // What the detour saw (copied for the log; script fiber).
    struct Stats
    {
        bool installed = false;
        bool locked = false;
        std::uint64_t updates = 0;        // fwTimer::Update calls since install
        std::uint64_t lockedUpdates = 0;  // of which with the export step
        std::int64_t lastStepNs = 0;      // the step asked for the last locked update
        float lastSystemStep = 0.0f;      // seconds the engine used (system set, unscaled)
        float lastGameStep = 0.0f;        // seconds of the game set (warp and clamp applied)
        std::int64_t errorTicks = 0;      // cumulative applied - wanted
        std::uint64_t writeFailures = 0;
        float clampMin = 0.0f;            // [0x3CC8388]
        float clampMax = 0.0f;            // [0x3CC838C]
        float period = 0.0f;              // [0x542819C] seconds per counter tick
    };

    // Finds and hooks the timer; false (with the reason) when the build does
    // not match. Idempotent.
    bool Install(std::string& error);
    bool Installed();
    // Starts following the export clock (the next update uses the step set
    // by SetStepNs).
    bool Lock(std::string& error);
    // The step the next fwTimer::Update simulates (ns of real time; the
    // game applies its warp on top).
    void SetStepNs(std::int64_t stepNs);
    // Back to the wall clock (export end, cancel, unload, hot reload).
    void Release();
    bool Locked();
    Stats Snapshot();
    // Unhooks (module unload): Release first.
    void Shutdown();
}
