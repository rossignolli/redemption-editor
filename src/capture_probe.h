#pragma once

// Research read probes of the capture side. Evidence only: behind
// RedemptionEditor.ini CaptureProbe=1 (default 0: nothing of this module is
// installed and Tick returns at once).
//
// With CaptureProbe=1 the load installs one read-only detour per proved port
// (30 ports, signature checked against build 1491.50; a port whose bytes
// differ stays off and the report names it) and the script thread polls
// game state by native and by guarded read (each global behind a byte check
// of the instruction that proved it). ReplayDebug\capture-probe-<date-
// time>.txt is written every 60 s and at the unload (Ctrl+R). Nothing is
// recorded into a clip and no argument or result is changed.
// tools/capture_probe_report.py reads the report. The tables and the report
// text: capture_probe_core.h.

#include "capture_probe_core.h"

#include <cstdint>
#include <string>

namespace replay::captureprobe
{
    struct TickInput
    {
        Mode mode = Mode::Idle;
    };

#if defined(REDEMPTION_DEV)
    // CaptureProbe of the ini in the working directory (read once).
    bool Enabled();
    // Installs the detours and checks the read sites (call inside the load's
    // hook batch, after the sound probe). False with "off" when the ini key
    // is 0; else `summary` lists what was installed and what was not.
    bool Install(std::string& summary);

    // Script thread, once per tick: the polls that are due, the frame count
    // and the report every 60 s.
    void Tick(const TickInput& input);
    // Module unload, after every detour drained: the final report, then the
    // hooks and the tables go.
    void Shutdown();

    // The render target recorder (render_target.h) owns the detours of C29 and C30 when it is installed (two hooks cannot share a
    // target); it reports each call here, so the probe's counts stay. A
    // no-op while the probe is off.
    void NoteRenderTargetRegister(std::uint64_t name, std::uint8_t flag, bool accepted, void* returnAddress);
    void NoteRenderTargetLink(std::uint32_t model, void* returnAddress);
#else
    // A research probe, built only in the developer build (REDEMPTION_DEV);
    // the public build has these no-ops, which is what the probe does with
    // its key at 0 (the default).
    inline bool Enabled() { return false; }
    inline bool Install(std::string& summary)
    {
        summary = "off";
        return false;
    }
    inline void Tick(const TickInput&) {}
    inline void Shutdown() {}
    inline void NoteRenderTargetRegister(std::uint64_t, std::uint8_t, bool, void*) {}
    inline void NoteRenderTargetLink(std::uint32_t, void*) {}
#endif
}
