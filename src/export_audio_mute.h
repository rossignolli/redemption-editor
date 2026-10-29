#pragma once

// Render export: the game's sound muted during the video pass (the export runs
// slower than real time, so the game's audio comes out slow and chopped: nobody
// wants to hear it, and it is not the audio of the video, which comes from the
// audio pass).
//
// Not engine access: the Windows audio session of this process is muted
// (what the Volume Mixer does), on every active render device, and the
// previous mute state is restored. Windows may keep a session's mute across
// launches, so a marker file is written while muted and the next
// Configure (a load after a crash or a kill) unmutes and deletes it.
// Script fiber only.

#include <string>

namespace replay::exportmute
{
    // The marker's folder (the game folder, next to RedemptionEditor.ini); also
    // clears a mute left by a previous run.
    void Configure(const std::string& folder);
    // Mutes this process's sessions; false when none was found (logged by
    // the caller, the export goes on: the mute is a comfort).
    bool Mute(int& sessions);
    void Restore();
    bool Muted();
}
