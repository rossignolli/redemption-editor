#pragma once
// The world's streaming focus follows the camera that renders, whatever it is,
// while the replay owns the world (one replay camera, the free camera is a mode
// of it, and the world streams around what is on screen). Without it a free
// camera after a view lent to another mod's camera (CLEAR_FOCUS) leaves the
// focus on the hidden live player hundreds of metres away and the puppets'
// hooves are never created.
//
// Pure logic: which camera owns the focus, when the owner changed, when the
// one per second FOCUS line is due. No natives here.
#include <cstdint>

namespace replay::worldfocus
{
    enum class Source : std::uint8_t
    {
        None = 0, // the replay does not own the focus (not playing, or WorldFocus=0 and nothing set)
        Replay,   // the replay camera on the recorded game camera
        Marker,   // the replay camera on the camera markers
        FreeCam,  // the replay's free camera
        Edit,     // the free camera in Edit Camera (a marker being edited)
        Lent,     // another mod's script camera renders (the view lent to it)
        Game,     // the live gameplay camera (a clip without a game camera)
    };

    inline const char* SourceName(Source s)
    {
        switch (s)
        {
        case Source::Replay: return "replay";
        case Source::Marker: return "marker";
        case Source::FreeCam: return "freecam";
        case Source::Edit: return "edit";
        case Source::Lent: return "lent";
        case Source::Game: return "game";
        default: return "none";
        }
    }

    // What the script thread knows this tick. renderingCam is
    // CAM::GET_RENDERING_CAM() (0 or -1: the gameplay camera).
    struct Inputs
    {
        bool playing = false;       // the transport has a clip (playing or paused): the replay owns the world
        int renderingCam = 0;
        int replayCam = 0;          // 0 when none
        int freeCam = 0;            // 0 when none
        bool freeCamEnabled = false;
        bool editingMarker = false;
        bool keyed = false;         // the replay camera follows the markers
        bool replayCamPlaced = false; // UpdateReplayCamera placed the replay camera this tick
    };

    // The camera that renders decides, not the replay's mode flags: a view
    // lent to another mod is "lent" even with the free camera on, and the free
    // camera is "freecam" even when a stale lend flag is still up.
    inline Source Choose(const Inputs& in)
    {
        if (!in.playing)
            return Source::None;
        const bool scriptCam = in.renderingCam != 0 && in.renderingCam != -1;
        if (scriptCam && in.freeCam && in.renderingCam == in.freeCam)
            return in.editingMarker ? Source::Edit : Source::FreeCam;
        if (scriptCam && in.replayCam && in.renderingCam == in.replayCam)
            return in.keyed ? Source::Marker : Source::Replay;
        if (scriptCam)
            return Source::Lent;
        // The engine has not switched yet (RENDER_SCRIPT_CAMS takes effect on
        // the next frame): the camera the replay just activated owns it.
        if (in.freeCamEnabled && in.freeCam)
            return in.editingMarker ? Source::Edit : Source::FreeCam;
        if (in.replayCamPlaced && in.replayCam)
            return in.keyed ? Source::Marker : Source::Replay;
        return Source::Game;
    }

    // Where the focus position comes from: the replay's own cameras give the
    // pose they placed this tick (GET_FINAL_RENDERED_CAM_COORD is one frame
    // late); another camera only has the rendered coordinate.
    inline bool UsesOwnPose(Source s)
    {
        return s == Source::Replay || s == Source::Marker || s == Source::FreeCam || s == Source::Edit;
    }

    struct Tracker
    {
        Source last = Source::None;
        bool owned = false;            // the replay set the focus at least once since the playback started
        std::uint64_t nextSampleMs = 0;
        std::uint32_t changes = 0;
        std::uint32_t samples = 0;
    };

    // True when the owner changed (a FOCUS line is due).
    inline bool Update(Tracker& t, Source now)
    {
        if (now == t.last)
            return false;
        t.last = now;
        ++t.changes;
        return true;
    }

    // The per second FOCUS_SAMPLE line (capped per playback).
    inline bool SampleDue(Tracker& t, std::uint64_t nowMs, std::uint32_t cap = 1800, std::uint64_t periodMs = 1000)
    {
        if (t.samples >= cap || nowMs < t.nextSampleMs)
            return false;
        t.nextSampleMs = nowMs + periodMs;
        ++t.samples;
        return true;
    }

    inline void Reset(Tracker& t) { t = Tracker{}; }
}
