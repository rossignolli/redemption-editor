#pragma once

// The clip's map state in the replay (IPL groups and interior entity sets):
// the recording keeps the active candidate IPLs (ipl_candidates.h) and the
// entity sets of the interiors near the scene, at the start and whenever they
// change; the playback backs up what it will touch, applies the clip's state
// at the playhead inside the clip's area (with the screen dark in product
// mode) and gives the backup back on exit. The chapter (the camp global) is
// recorded for a notice when the clip is played in another chapter.
// Natives only (rung 1) plus one guarded read of a script global (rung 2).
// The rules are pure in world_state.h.

#include "replay_recording.h"
#include "world_state.h"

#include <cstdint>
#include <ostream>

namespace replay::world
{
    // ---------------- Recording ----------------

    // Full scan of the candidates, their bounding spheres, the camp and the
    // interiors at the player and the camera. After Recording::Begin.
    void BeginRecording(Recording& clip, std::ostream& log);
    // Every tick of a recording while its clock runs (the recording gate):
    // a new pass every 2 s, spread over ticks (a slice of candidates per
    // tick) so the cost per tick stays small.
    void UpdateRecording(Recording& clip, std::uint32_t nowMs, const float (&player)[3], const float (&camera)[3],
        std::ostream& log);
    void EndRecording(const Recording& clip, std::ostream& log);

    // ---------------- Playback ----------------

    struct PlaybackConfig
    {
        bool restore = true;   // RedemptionEditor.ini RestoreWorldState=1
        bool inMission = false; // RestoreWorldStateInMission=1: apply while a mission runs
    };

    // What the editor shows on load (world_notice.h).
    struct Notice
    {
        std::uint32_t sequence = 0; // grows with each playback start that has something to say
        std::uint32_t kind = kNoticeNone;
        std::uint8_t recordedChapter = kChapterUnknown;
        std::uint8_t currentChapter = kChapterUnknown;
        std::uint32_t differences = 0; // IPLs and entity sets that differ in the clip's area
        std::uint32_t applied = 0;
    };

    // Before the clip load scene: the backup and the state at playhead 0.
    void BeginPlayback(const Recording& clip, const PlaybackConfig& config, std::ostream& log);
    // Every tick of a playback: the state at the playhead when a change of
    // the clip is crossed or skipped (a seek), the game undoing an applied
    // IPL (logged, applied again), interiors that streamed in later.
    void UpdatePlayback(const Recording& clip, std::uint32_t playheadMs, std::uint64_t nowMs, std::ostream& log);
    // The interiors whose entity sets were changed are ready (the load wait).
    bool Ready();
    // The clip's map state was looked at for the current playback (BeginPlayback
    // ran, applied or dry; false again after EndPlayback): the map dressing stands
    // decide after it.
    bool PlaybackApplied();
    // The IPL churn of the clip starts: every REQUEST/REMOVE_IPL_HASH the replay
    // issued (monotonic), the last playback start's apply and the last restore
    // (the previous clip's IPLs given back).
    struct Pressure
    {
        std::uint64_t iplToggles = 0;
        std::uint64_t applies = 0;
        std::uint32_t lastApplyOn = 0;
        std::uint32_t lastApplyOff = 0;
        std::uint32_t lastApplySets = 0;
        std::uint64_t lastApplyAtMs = 0; // GetTickCount64 of the last apply, 0: never
        std::uint32_t lastRestoreIpls = 0;
    };
    Pressure CurrentPressure();
    // Gives the backup back (screen still dark in product mode).
    void EndPlayback(std::ostream& log);
    // A script restart with a playback open. The IPLs are the map's (names, not
    // handles), but the backup is applied by the path made for a reload: the
    // backup file stays and ServiceRestoreLeft gives it back once no loading screen
    // is up. Here the in-memory playback state is only dropped (true if one was open).
    bool ForgetPlaybackAfterRestart(std::ostream& log);
    const Notice& CurrentNotice();

    // After a reload or a crash: the backup file left by a playback. Called
    // every tick; runs once, when no loading screen is up.
    void ServiceRestoreLeft(std::ostream& log);

    // ---------------- Bench ----------------

    // world_probe: the cost of a full candidate scan, the active count and
    // fingerprint, the camp global and the camp IPLs, the interior at the
    // player and its entity sets.
    void Probe(std::ostream& log);
}
