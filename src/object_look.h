#pragma once

// The custom texture (document text) and tint of the recorded objects
// (object_look_core.h).
//
// Recording (guarded reads of offline-proven fields): every
// recorded weapon object (the held note is one: OBJECT_1, weapon_plain), prop
// and scenery object is read at its first sample and every 500 ms (object
// +0x420/+0x424/+0x428, entity +0xB8 bits 3..8), only after IS_ENTITY_AN_OBJECT
// and with the setter's and the tint writer's code signature checked once (a
// mismatch disables it, logged). Changes go into Recording::objectLooks /
// sceneryObjectLooks (section OLOK).
//
// Playback (natives on the script thread): on the weapon and prop
// puppets and the scenery and world object stand-ins with a track, the look at
// the playhead: REQUEST_STREAMED_TXD, wait HAS_STREAMED_TXD_LOADED, then
// SET_CUSTOM_TEXTURES_ON_OBJECT and SET_OBJECT_TINT_INDEX; the TXDs asked are
// released at the playback's end. The detector object_look_mismatch reads the
// object again 0.5 s and 2 s after a set and on a QA mark.
//
// Ini ObjectLook (1 by default): records and plays. ObjectLookEvidence (0 by
// default): every read logged (OBJECT_LOOK_READ, first read and changes per
// object), with ObjectLook=0 the evidence-only path (nothing recorded or set).

#include "object_look_core.h"
#include "replay_context.h"

#include <cstdint>

namespace replay::objlook
{
    void Attach(Context& context);
    void SetRecordAndPlay(bool on);
    void SetEvidence(bool on);
    bool RecordAndPlay();
    bool Evidence();
    // The code signatures checked (once) and the feature on.
    bool Active();

    // The fields of an object now (IS_ENTITY_AN_OBJECT, guarded read; for
    // the evidence). false: not an object, unreadable, or the build check
    // failed.
    bool ReadLook(Object object, ObjectLook& look, std::uint32_t& tintWord);

    // Recording side.
    void BeginRecording();
    void EndRecording();
    // One recorded weapon object or prop (key: packed id); throttled by
    // the caller's checked/checkMs pair (kLookReadMs).
    void SampleObject(std::uint32_t key, Object object, std::uint32_t timeMs, bool& checked, std::uint32_t& checkMs);
    // One scenery entry while it stands (key: its index), throttled inside.
    void SampleScenery(std::uint32_t index, Object object, std::uint32_t timeMs);

    // Playback side.
    void BeginPlayback();
    void EndPlayback();
    // A script restart with a playback open: forgotten, no native.
    void ForgetAfterRestart();
    // The weapon and prop puppets with a track.
    void PlayPuppets(std::uint32_t timeMs);
    // A stand-in: `track` is an index into sceneryObjectLooks (scenery) or
    // the prop's packed id (a world object's stand-in, before the prop takes
    // it over; the prop's track held at its first state). -1 / 0: none.
    int SceneryTrackOf(std::uint32_t entry);
    void PlayStand(Object standIn, bool scenery, std::uint32_t key, std::uint32_t model, std::uint32_t timeMs);
}
