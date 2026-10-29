#pragma once

// The generic sound effect recorder and player, behind RedemptionEditor.ini
// SoundGeneric=1, the default (0: nothing of this module runs, the taps in the detours are
// one relaxed load each).
//
// With SoundGeneric=1 the load installs the sound port detours the probe
// already has (soundfx_probe.cpp: one hook per port feeds the census, the probe
// and this recorder), and while a recording runs the detours push events into
// two lock-free rings that the script thread drains every recording tick into
// the clip's Track (Recording::soundGeneric, section SGEN). A SOUND_GENERIC line
// goes to the log every 60 s and at the stop; ReplayDebug\<clip>.sound-generic.txt
// is written when the clip is saved (tools/sound_generic_report.py reads it).
// The pure part (events, rings, packets, sampler, budget, report):
// sound_generic_core.h.
//
// Ini keys (section [RedemptionEditor]):
//   SoundGeneric=1            1: record, and play the packets in the replay
//   SoundGenericUpdateHz=20   samples per second per sound and per setting
//   SoundGenericMaxMBPerMin=8 clip bytes the track may add per minute; past
//                             it the adjust rate drops, then the adjusts,
//                             then the creates and ends (Governor)
//   SoundGenericPlay=1        0: the packets are counted, nothing plays
//   SoundGenericPreloadMs=500 a sound that waited for a load in the recording
//                             is created and prepared this early (0: off)
//   SoundGenericPreloadDelayMs=100 the wait that marks such a sound
//   SoundGenericHoldMs=150    the wait after a packet's time for the puppet
//                             to make the sound itself (then it is not played)
//   SoundGenericSlots=64      sounds the player holds at once (8 to 256)
//   SoundGenericTake=         family:taker;... (sound_owner_table.h): who
//                             plays a family: generic, specific or puppet
//   SoundGenericMode=packets  the puppets' own sounds are refused at the
//                             creation ports and every recorded packet of a
//                             replay entity plays from the clip (exceptions:
//                             speech, steps, melee, shots, explosions);
//                             legacy: the older per-track rules
//   SoundGenericWeaponOffShot=1 a packet on a weapon object's audio that no
//                             weapon code asked for and that is away from
//                             every recorded shot of that weapon (a script
//                             sound on the object, e.g. a campfire's sizzle)
//                             is played by this player; 0: every weapon
//                             audio packet is left to the shot track
//   SoundGenericVehicleBlock=1 a puppet vehicle's or train car's own create of
//                             a sound whose recorded packet is alive then is
//                             refused at the census creators (packets mode,
//                             vehicle_sound_rule.h); 0: its own copies play
//                             over the packets

#include "replay_context.h"
#include "sound_generic_core.h"
#include "sound_generic_player_core.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace replay::soundgeneric
{
    namespace detail
    {
        inline std::atomic<bool> g_capturing{false};
    }

    // True while a recording runs with SoundGeneric=1: the only thing a
    // detour reads before it pushes.
    inline bool Capturing() noexcept { return detail::g_capturing.load(std::memory_order_relaxed); }

    // The ini keys (read once).
    void Configure(const std::string& iniPath);
    // SoundGenericOriginSkip (1): a position adjust at the map origin is not applied
    // (the player's own sound requests while mounted).
    void SetOriginSkip(bool on);
    bool Enabled();
    Config Configured();
    // The rings (before the hooks are enabled). False with the reason when
    // off or when the rings could not be made.
    bool Install(std::string& summary);
    // Module unload, after every detour drained.
    void Shutdown();

    // Script thread: a recording starts (the tables and the stats start
    // over; the detours start pushing), its tick (after the census tick:
    // the drain) and its stop (the last drain, the detours stop pushing).
    void BeginRecording(Context& context);
    void Record(Context& context);
    void EndRecording(Context& context);

    // ---- taps (call only when Capturing()) ----

    // S03 (Queue) or S04 (Deferred) accepted a request; `frames` are the
    // return addresses above the detour (nearest first), `count` of them.
    void OnEnqueued(Source source, std::uint64_t audio, std::uint64_t ref, std::uint64_t timeUs, void* const* frames,
        std::uint32_t count);
    // Both creators (the census detour): `params` is the census's copy of
    // the init params or null, `callers` the census row's frames (RVA).
    void OnCreated(std::uint8_t core, std::uint64_t ref, std::uint64_t audio, std::uint64_t sound, const std::uint8_t* params,
        const std::uint32_t* callers, std::uint32_t callerCount, bool mine, bool speech);
    // S13 (forget false) and S14 (forget true).
    void OnStop(std::uint64_t sound, bool forget);
    // S15 (not from a creator's failure path), the slot's sound read before the release.
    void OnRelease(std::uint64_t sound);
    // A setting port: `sound` is [block]; `value` 3 floats (position) or 1; `hash` for Variable.
    void OnAdjust(Adjust kind, std::uint64_t sound, const float* value, std::uint32_t hash);
    // An S38 call the engine refused while recording (its sound's tree exports no
    // such variable): not a packet, only counted.
    void OnVariableRefused();

    // ---- player (sound_generic_player.cpp) ----

    // Script thread: a playback starts (the plans from the clip's track, the
    // owner table from the clip's other tracks and the ini), its tick
    // (after the census tick and the capture: the step rule, the starts,
    // the adjusts, the ends; every tick, paused or scrubbing too, as the
    // capture) and its end (everything stopped; returns the section for
    // ReplayDebug\<clip>.sound-check.txt, empty when the track is empty).
    void BeginPlayback(Context& context, std::uint32_t firstMs);
    void Play(Context& context, std::uint32_t timeMs);
    std::string EndPlayback(Context& context);
    // A script restart with a playback open: the player's plans dropped without
    // the engine, the queued sounds (raw pointers) forgotten, the weak slots
    // stopped by the guarded path.
    std::uint32_t ForgetPlaybackAfterRestart(Context& context);
    // The packets whose time is in [fromMs, toMs] of the playback that runs and
    // what the latest pass did with them, by family (with its taker), by owner,
    // and the failures (a QA mark's window). False when no playback runs.
    bool WindowReport(std::uint32_t fromMs, std::uint32_t toMs, player::WindowReport& out);
    // SoundGenericMode=packets while the player plays (on, SoundGenericExport in an
    // export, a track): the ports refuse the puppets' own sounds
    // (audio::SetBlockPuppetSounds) and the tracks that replay a sound by its ref
    // leave a row a packet of this player carries (sound_owner_table.h
    // PacketIndex): the ESND row by its index in the clip's entitySounds, the
    // others by owner, ref and time.
    bool PacketsMode();
    // A top sound this player made (script thread; the volume probe's src).
    bool IsPlayerSound(std::uint64_t sound);
    bool EntitySoundHandedOff(std::size_t row);
    bool PacketCovers(std::uint32_t owner, std::uint64_t ref, std::uint32_t timeMs);
    // The sounds of the puppets the last EndPlayback found short (nothing else
    // makes them: not played, or created and ended by the engine at once), as
    // MISSING findings for the notice at Stop.
    std::vector<census::Finding> LastFindings();
    // The melee packets of the last (or current) playback the melee track's call
    // did not make again, played from the recording (per pass).
    std::uint32_t MeleeComplementsPlayed();

    // ---- report ----

    // The stats of the current or last recording (a copy for the save thread).
    std::shared_ptr<const Stats> StatsSnapshot();
    // ReplayDebug\<clip stem>.sound-generic.txt from the clip's track and
    // `stats`; pure apart from the file write (any thread). False with the
    // reason when nothing was written (an empty track writes nothing).
    bool WriteReport(const Recording& clip, const Stats& stats, const std::wstring& clipPath, std::uint32_t durationMs,
        std::wstring& reportPath, std::string& error);
}
