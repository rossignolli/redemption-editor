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
    // behind the camera has the hat 10 cm high and a frozen face.
    // prerender/forceAnim/lodMultiplier (off by default): the recorded humans
    // within radiusM of the camera get SET_ENTITY_ALWAYS_PRERENDER,
    // FORCE_PED_AI_AND_ANIMATION_UPDATE every tick and a LOD multiplier,
    // given back when they leave the radius or the recording. None of the
    // three keeps the face, hat fit or brim computed off screen (the LOD
    // multiplier fixes distance only). animVisible (on by default) sets the
    // engine's own "animate as if visible" byte of every recorded ped and
    // animal (anim_visible.h) while it is recorded, whatever the radius.
    struct FullAnimConfig
    {
        bool prerender = false;
        bool forceAnim = false;
        std::uint32_t lodMultiplier = 0; // 0: not touched
        float radiusM = 40.0f;
        bool animVisible = true;
        // The recorded peds' anim directors get LOD 0 and visible through the
        // detour of 0x2BD410C (anim_lod_hook.h): covers distance too, and
        // touches nothing but the animation.
        bool animLod0 = true;
    };
    void SetFullAnim(const FullAnimConfig& config);
}
