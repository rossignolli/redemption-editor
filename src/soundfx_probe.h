#pragma once

// Read probes of the sound and particle ports. Evidence only: behind
// RedemptionEditor.ini SoundFxProbe=1 (default 0: no detour of this module is
// installed and the taps below are one relaxed load each).
//
// With SoundFxProbe=1 the load installs one read-only detour per probed port
// (signature checked against build 1491.50; a port whose bytes differ stays
// off and the report names it), the detours the mod already has call the
// taps below, and ReplayDebug\soundfx-probe-<date-time>.txt is written every
// 60 s and at the unload (Ctrl+R). Nothing is recorded into a clip and no
// argument or result is changed. tools/soundfx_probe_report.py reads the
// report. The counters, tables and the report text: soundfx_probe_core.h.
//
// The detours here are the one hook per port of the mod. With SoundGeneric=1
// the ports the generic sound recorder needs (S13, S15, S16 to S19, S21 to
// S24, S38) are installed even with SoundFxProbe=0, and every detour feeds the
// probe (when on) and the recorder (while a recording runs) from the same call.

#include "sound_generic.h"
#include "soundfx_probe_core.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace replay::soundfx
{
    namespace detail
    {
        inline std::atomic<bool> g_active{false};
    }

    // True between a successful Install and the unload. The taps are called
    // only behind this.
    inline bool Active() noexcept { return detail::g_active.load(std::memory_order_relaxed); }

    // SoundFxProbe of the ini in the working directory (read once).
    bool Enabled();
    // Installs the detours (call inside the load's hook batch, after the
    // audio, census and fx observers). False with "off" when the ini key is
    // 0 and SoundGeneric is 0 too; else `summary` lists what was installed
    // and what was not. With SoundGeneric=1 alone, only the recorder's ports
    // are hooked and no probe table exists.
    bool Install(std::string& summary);
    // Script thread, once per tick: the frame count, the calls of the frame,
    // the queue's expiry and the report every 60 s.
    void Tick(Mode mode);
    // Module unload, after every detour drained: the final report, then the
    // hooks and the tables go.
    void Shutdown();
    // The trampoline of a port the probe hooked (0 when it did not): code of
    // the mod that checks a port's bytes before calling it would see the
    // hook's jump, and its own calls should not be counted as the game's.
    void* OriginalFor(std::uintptr_t rva);
    // QueryPerformanceCounter ticks.
    std::uint64_t Ticks();

    // ---- taps (call only when Active()) ----

    // Both creators (sound_census.cpp Push). `params`: a copy of the init
    // params (0x140 bytes) or null; `callers`: the census row's frames (RVA).
    void NoteCreated(std::uint8_t core, std::uint64_t ref, std::uint64_t audio, std::uint64_t sound,
        const std::uint8_t* params, const std::uint32_t* callers, std::uint32_t callerCount);
    // S03, before the original (the params are still the caller's) and after.
    void NoteEnqueueRequest(std::uint64_t audio, std::uint64_t ref, std::uint64_t params, void* returnAddress);
    void NoteEnqueueResult(std::uint64_t audio, std::uint64_t ref, bool accepted);
    // S04, S06 and the params constructor: counted only.
    void NoteCall(Port port, void* returnAddress);
    // S14.
    void NoteForget(std::uint64_t sound, void* returnAddress);
    // S05: the whole call, so a sound the creator makes inside it is known
    // as the speech's when the caller is one of the speech's two sites.
    void EnterByRef(std::uint64_t audio, std::uint64_t ref, std::uint64_t params, void* returnAddress);
    void LeaveByRef();
    // The same speech mark without the probe (the generic
    // recorder skips the speech's sounds); true when the caller is a
    // speech site (then LeaveSpeechOnly must follow).
    bool EnterSpeechOnly(void* returnAddress) noexcept;
    void LeaveSpeechOnly() noexcept;
    // This thread is inside a speech site's persistent-by-ref call.
    bool InsideSpeech() noexcept;
    struct ByRefScope
    {
        ByRefScope(std::uint64_t audio, std::uint64_t ref, std::uint64_t params, void* returnAddress) noexcept
            : m_entered(Active()), m_speech(false)
        {
            if (m_entered)
                EnterByRef(audio, ref, params, returnAddress);
            else if (replay::soundgeneric::Capturing())
                m_speech = EnterSpeechOnly(returnAddress);
        }
        ~ByRefScope() noexcept
        {
            if (m_entered)
                LeaveByRef();
            else if (m_speech)
                LeaveSpeechOnly();
        }
        ByRefScope(const ByRefScope&) = delete;
        ByRefScope& operator=(const ByRefScope&) = delete;

    private:
        bool m_entered;
        bool m_speech;
    };
    // P02: `startTicks` taken at the detour's entry, `engineTicks` the time
    // inside the engine's function.
    void NoteRegistered(std::uint64_t startTicks, std::uint64_t engineTicks, std::uint32_t effectHash, void* instance,
        void* returnAddress);
}
