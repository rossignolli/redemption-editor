#pragma once

// Named render targets and the UI apps that draw into them, engine side (the
// rules and the track are render_target_core.h).
//
// Recording (RenderTargetRecord, default 1): detours on the engine's
// register, link and release of named render targets and on the UI app
// launch (each signature checked; a mismatch leaves that port off and
// logs it). The detours forward
// every call unchanged and queue (name, flag, answer, model, app, entry) in
// a ring; the script thread drains it every tick (Monitor), keeps the live
// state (a recording that starts with a target registered gets it at 0) and
// files the events with the recording's clock. UI apps open and close are
// polled with IS_UIAPP_ACTIVE_BY_HASH on the script thread (SHOP_BROWSING
// plus every app the launch detour saw), the entry
// taken from the launch detour. Clip section RTGT (optional).
//
// Playback (RenderTargetPlayback, default 1 = relink): every tick the clip's
// state at the playhead is compared with what the replay applied and the
// difference is made with natives on the script thread (register, link,
// release; the UI apps are not relaunched).
// Targets or apps the world already has are left alone (never released or
// closed by the replay). Everything the replay made is undone at the end.
//
// Log lines: RT_HOOKS, RT_EVENT, RT_RECORD_BEGIN/TOTAL, RT_PLAY_BEGIN,
// RT_PLAY (each native made), RT_PLAY_TOTAL, BENCH_RT.

#include "render_target_core.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace replay::rendertarget
{
    struct Settings
    {
        bool record = true;                 // RenderTargetRecord
        Mode mode = Mode::Relink;           // RenderTargetPlayback (0 off, 1 relink)
    };
    void Configure(const Settings& settings);
    const Settings& Configured();

    // Installs the four detours (inside the load's hook batch, before the
    // capture probe: C29 and C30 of the probe ride these detours). False
    // with "off" when RenderTargetRecord=0.
    bool Install(std::string& summary);
    bool Installed();
    // The capture probe's ports C29 and C30 are these detours' targets.
    bool OwnsProbePorts();
    void Shutdown();

    // Script thread, every tick: drains the detours' ring into the live
    // state and, while recording, into the track (time nowMs); polls the UI
    // apps while recording.
    void Monitor(bool recording, std::uint32_t nowMs, Track& track, std::ostream& log);
    void BeginRecording(Track& track, std::ostream& log);
    void EndRecording(const Track& track, std::ostream& log);

    void BeginPlayback(const Track& track, std::ostream& log);
    void Play(const Track& track, std::uint32_t timeMs, std::ostream& log);
    void EndPlayback(std::ostream& log);
    // A script restart with a playback open: what the replay made is
    // forgotten without a native (the old script owned it).
    void ForgetAfterRestart();

    // Bench op "render_target": register NAME [flag] | link MODEL |
    // release NAME | app_open APP [ENTRY] | app_close APP | log NAME [MODEL].
    // Live natives (not marked as the replay's own: a recording files them).
    bool BenchOp(const std::vector<std::string>& args, std::string& detail);
}
