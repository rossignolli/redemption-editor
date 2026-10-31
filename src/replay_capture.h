#pragma once

// Recorder core: which engine entities enter the recording (proximity scan
// with per-class capacity, pinned player/mount/vehicle and dependents),
// their registration (skeleton resolve, appearance, pose record), pose
// segments when a skeleton changes, retirement, the draft harness / seat /
// weapon point attachment packets, and the per-frame capture tick that
// drives the subsystems (objects, ropes, events). Works on the shared
// Context; the script keeps the wall clock, the transport and the HUD.

#include "replay_context.h"

#include <cstdint>

namespace replay::recorder
{
    void Attach(Context& context);
    void Start();
    // One recording frame; `mount` is the mount the player is fully on (0
    // when none), pinned like the player.
    void Update(Ped mount);
    void Stop(std::uint64_t elapsedMs);

    // The engine animates a ped's hat fit, face and hat brim only while the
    // ped is on screen (the face only near the camera), so a ped recorded
    // behind the camera has the hat 10 cm high and a frozen face (the
    // always-prerender, forced animation update and LOD multiplier natives
    // did not fix it). animVisible (on by default) sets the
    // engine's own "animate as if visible" byte of every recorded ped and
    // animal (anim_visible.h) while it is recorded.
    struct FullAnimConfig
    {
        bool animVisible = true;
        // The recorded peds' anim directors get LOD 0 and visible through the
        // detour of the LOD setter (anim_lod_hook.h): covers distance too, and
        // touches nothing but the animation.
        bool animLod0 = true;
    };
    void SetFullAnim(const FullAnimConfig& config);
}
