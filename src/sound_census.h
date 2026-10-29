#pragma once

// Engine sound census: read-only detours on the two sound instance creators
// (sound_census_core.h has the evidence).
//
// On by default in a cheap mode. Every recording counts the sounds per owner, ref and caller
// path per 250 ms window into the clip (Recording::soundCensus), every
// playback counts the same way, and at the playback's end the two are
// compared (sound_check_core.h Diff): ReplayDebug\<clip>.sound-check.txt
// (a debug folder in the game folder, not next to the clips) and
// SOUND_MISSING_SUMMARY. RedemptionEditor.ini:
//   SoundCheck      the cheap census and the report; default 1 with
//                   Debug=1, 0 without. Off, the hooks stay
//                   only for SoundCapture and nothing is counted
//   SoundCensus=0   1: also one SOUND_CENSUS log line per sound (for the
//                   developer tools that compare runs)
//   SoundCapture=1  the census-level capture (sound_capture.h): 1 vehicles,
//                   train cars and objects; 2 every recorded owner; 0 off

#include "replay_context.h"
#include "sound_census_core.h"

#include <string>
#include <vector>

namespace replay::census
{
    // Reads the ini in the working directory (once).
    bool Enabled();
    bool FullLog();
    // The census comparison and its report (SoundCheck).
    bool CheckEnabled();
    int CaptureMode();
    // Hooks both creators (signature-checked; call inside the load's hook
    // batch). False with the reason when a signature differs or the census
    // is off; the mod runs the same either way.
    bool Install(std::string& error);
    // Script thread, once per recording or playback tick: drains what the
    // detours saw since the last tick, finds each row's owner and clip time
    // (interpolated between this tick and the previous one), counts it and,
    // with SoundCensus=1, logs it.
    void Tick(Context& context, const char* phase, double clipMs, bool normal);
    // Script thread, every frame: when no recording or playback ticked for a
    // while, closes the running phase (SOUND_CENSUS_TOTAL) and keeps the
    // live persistent sounds up to date (the capture adopts the ones alive
    // when their owner enters a recording).
    void Idle(Context& context);
    // Module unload, after the hooks were disabled and drained.
    void Shutdown();

    // A playback starts and ends (events::BeginPlayback,
    // EndPlayback). The end compares and writes the report.
    void BeginPlayback(Context& context);
    // `summary` (horse_sound_report.h) goes under the report's
    // RESUMO line and `section` before its first section (speech and steps
    // outcomes the census cannot see: a refused line creates no sound).
    void EndPlayback(Context& context, const std::string& captureLine, const std::string& summary = std::string(),
        const std::string& section = std::string());
    // Findings of another source for the next EndPlayback's notice (the generic
    // player's sounds that fell short: the census sees a create of the
    // player as played whether or not it is ever heard).
    void NoteExtraFindings(std::vector<Finding> findings);
    // A script restart with a playback open: the
    // playback's census ends without its report (the clip is dropped).
    void ForgetPlaybackAfterRestart();
    // script.cpp: where the open clip is (the report goes next to it).
    void SetClipPathSource(std::wstring (*source)());
    // The clip path the reports are named after (the fx check).
    std::wstring CurrentClipPath();

    // Sounds this thread creates while one is alive are the mod's own
    // (marked `mine`: never a puppet's own create for the anti-double rule).
    struct ModScope
    {
        ModScope();
        ~ModScope();
        ModScope(const ModScope&) = delete;
        ModScope& operator=(const ModScope&) = delete;
    };

    // The rows of the last Tick, owner resolved (owner.kind None: no owner).
    struct TickRow
    {
        Row row;
        Owner owner;
        double clipMs = 0.0;
        bool normal = false;
        bool alive = false; // *out held the sound when drained (a persistent sound)
    };
    const std::vector<TickRow>& Rows();
    // The player's recorded id the owner table uses
    // (the last recording's, or the clip census's player at a playback).
    std::uint32_t RecordedPlayerId();
    // The recorded player's speech lines are said by the hidden live player in this
    // playback (SpeechPlayerOnPlayer): their sounds count as his.
    void SetPlayerSpeechProxy(bool on);
    const char* Phase();

    // Persistent sounds alive now (made since the mod loaded, slot still
    // holding them), for the capture's adoption of sounds older than the
    // recording.
    struct LiveSound
    {
        std::uint64_t sound = 0;
        std::uint64_t slot = 0;
        Row row; // the create (its params included)
    };
    const std::vector<LiveSound>& Live();
    // The owner of an audio entity or tracker in the current table.
    bool OwnerOf(std::uint64_t audio, std::uint64_t tracker, Owner& out);
    // Guarded reads for the capture (same floor as the detours).
    bool ReadQword(std::uint64_t address, std::uint64_t& value);
    bool ReadBytes(std::uint64_t address, void* out, std::size_t size);
    std::uint64_t ImageBase();
    std::uint64_t ImageEnd();

    // Cost evidence (microseconds, script thread) for the summary line.
    struct Cost
    {
        std::uint64_t ticks = 0;
        double tickUsTotal = 0.0;
        double tickUsMax = 0.0;
        std::uint64_t rows = 0;
    };
}
