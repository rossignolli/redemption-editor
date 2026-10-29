#pragma once

// The sound categories' controller values recorded and put back in the replay,
// engine side (the cause, the evidence and the layout are in
// category_state_core.h).
//
// - Recording (CategoryStateRecord=1): every static category's values (the
//   controllers' sum the controller manager wrote: volume, the two
//   multipliers, pitch, the filters), read by hash through the category map
//   at most 20 times a second, stored on first sight and on change (clip
//   section CATS).
// - Playback (CategoryStateReplay=1): while the replay owns the world and the
//   clip has CATS, a detour of the resolver 0x251C310 (prologue and sites
//   checked, installed with the other hooks at load, forwards every call)
//   writes the recorded volume (CategoryStateFields=all: every field) into
//   the category right before the resolver reads it. The values at the
//   playhead are prepared on the script thread and published through a
//   two-buffer seqlock; the detour never calls natives, never allocates and
//   never waits. The categories of CategoryStateKeepLive (the sliders, the
//   music, BASE) and the dynamic hierarchies stay live.
// - Evidence: CATEGORY_STATE lines (begin, a window every 500 ms, end) and,
//   with CategoryStateLog=1, CATEGORY_CTL lines for the watched categories
//   (CategoryStateWatch, indices) in recording and playback.
//
// Engine access: rung 2 reads of the category map and pool (sites checked
// once), rung 3 writes of the categories' +0x48 (and fields) from the detour
// only (rung 5: the closed theatre, only while the replay owns the world);
// Shutdown stops imposing, disables the hook, waits for the calls in flight
// and only then removes it.

#include "category_state_core.h"
#include "replay_recording.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace replay::catstate
{
    struct Config
    {
        bool record = true;                 // CategoryStateRecord
        bool replay = true;                 // CategoryStateReplay
        bool fieldsAll = false;             // CategoryStateFields=all
        std::vector<std::uint32_t> keepLive; // CategoryStateKeepLive (hashes)
        bool log = false;                   // CategoryStateLog
        std::vector<std::uint32_t> watch;   // CategoryStateWatch (category indices)
    };
    void Configure(const Config& config);
    const Config& GetConfig();

    // Load time, inside the hook batch. False with the reason ("off" when
    // CategoryStateReplay=0).
    bool Install(std::string& error);
    bool Installed();
    void Shutdown();

    // Script thread.
    void Record(Recording& clip, std::uint32_t timeMs, bool clockRunning, std::ostream& log);
    void BeginPlayback(const Recording& clip, std::ostream& log);
    // Every playback tick (any play state: a seek or a pause shows the
    // recorded mix too); `owning`: the replay owns the world now.
    void Play(const Recording& clip, std::uint32_t timeMs, bool owning, std::ostream& log);
    void EndPlayback(const Recording& clip, std::ostream& log);
    // A script restart with a playback open (the playback's end does not run): the
    // detour stops imposing at once, no engine write (the controller manager
    // rewrites every category).
    void ForgetPlayback();
}
