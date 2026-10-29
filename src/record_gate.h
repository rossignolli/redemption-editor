#pragma once

// Recording gate.
//
// Per tick the recorder reads raw signals (pause menu, cutscene camera,
// script camera, letterbox, player control, fade, loading screen) and
// decides whether the recording clock stops. A stopped clock records
// nothing; the clip goes straight from the instant it stopped to the
// instant it went on (a cut). A replay editor usually stops recording while
// the player has no control and leaves cutscene frames flagged; here the
// cutscene is left out by default (RecordCutscenes=0) and kept on request.
//
// Also here: the restricted-camera spans (cutscene, script camera, no
// control) the editor draws on the timeline, and the cut-aware sampling
// rule playback uses so nothing is interpolated across a cut.
//
// Pure data: no engine or native access, unit tested in ReplayCoreTests.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

namespace replay::gate
{
    // Raw signals of one tick (the RECORD_PAUSE log's bits).
    enum Signal : std::uint32_t
    {
        SignalPauseMenu = 1u << 0,      // HUD::IS_PAUSE_MENU_ACTIVE
        SignalAnimSceneCam = 1u << 1,   // CAM::_IS_ANIM_SCENE_CAM_ACTIVE
        SignalScriptCam = 1u << 2,      // GET_RENDERING_CAM valid and not one of ours
        SignalGameplayCam = 1u << 3,    // IS_GAMEPLAY_CAM_RENDERING
        SignalPlayerAnimScene = 1u << 4, // IS_ENTITY_PLAYING_ANIM_SCENE(player, -1)
        SignalLetterbox = 1u << 5,      // HAS_LETTER_BOX
        SignalControlOff = 1u << 6,     // !IS_PLAYER_CONTROL_ON
        SignalFadedOut = 1u << 7,       // IS_SCREEN_FADED_OUT
        SignalLoading = 1u << 8,        // GET_IS_LOADING_SCREEN_ACTIVE || IS_LOADING_SCREEN_VISIBLE
        SignalCinematicCam = 1u << 9,   // IS_CINEMATIC_CAM_RENDERING (never gates)
        SignalMission = 1u << 10        // GET_MISSION_FLAG (informative only)
    };

    // Why the clock is stopped (bits; several can hold at once).
    enum Reason : std::uint32_t
    {
        ReasonNone = 0,
        ReasonMenu = 1u << 0,
        ReasonCutscene = 1u << 1,
        ReasonFade = 1u << 2,
        ReasonLoading = 1u << 3
    };

    // The raw cutscene rule: the anim scene camera, or a script camera that
    // is not ours rendering over a player in an anim scene, behind a
    // letterbox or without control. The cinematic horse camera is not a
    // script camera and never counts.
    inline bool CutsceneRaw(std::uint32_t signals)
    {
        if (signals & SignalAnimSceneCam)
            return true;
        const bool foreignScriptCam = (signals & SignalScriptCam) && !(signals & SignalGameplayCam);
        return foreignScriptCam &&
            (signals & (SignalPlayerAnimScene | SignalLetterbox | SignalControlOff)) != 0;
    }

    // On after `onTicks` consecutive raw ticks, off after `offTicks`.
    struct Hysteresis
    {
        std::uint32_t onTicks = 3;
        std::uint32_t offTicks = 10;
        bool active = false;
        std::uint32_t run = 0; // consecutive ticks disagreeing with `active`

        // True when `active` changed on this tick.
        bool Update(bool raw)
        {
            if (raw == active)
            {
                run = 0;
                return false;
            }
            ++run;
            if (run < (active ? offTicks : onTicks))
                return false;
            active = raw;
            run = 0;
            return true;
        }
        void Reset()
        {
            active = false;
            run = 0;
        }
    };

    // The gate's decision for one tick.
    struct Decision
    {
        std::uint32_t reasons = ReasonNone; // everything that holds (cutscene even when recorded)
        bool paused = false;                // the clock stops
        bool cutscene = false;              // the cutscene rule after the hysteresis
    };

    struct Gate
    {
        Hysteresis cutscene;

        Decision Update(std::uint32_t signals, bool recordCutscenes)
        {
            cutscene.Update(CutsceneRaw(signals));
            Decision decision;
            decision.cutscene = cutscene.active;
            if (signals & SignalPauseMenu)
                decision.reasons |= ReasonMenu;
            if (signals & SignalFadedOut)
                decision.reasons |= ReasonFade;
            if (signals & SignalLoading)
                decision.reasons |= ReasonLoading;
            if (decision.cutscene)
                decision.reasons |= ReasonCutscene;
            decision.paused = (decision.reasons & (ReasonMenu | ReasonFade | ReasonLoading)) != 0 ||
                (decision.cutscene && !recordCutscenes);
            return decision;
        }
        void Reset() { cutscene.Reset(); }
    };

    // The reason shown and logged first: menu, then loading, fade, cutscene.
    inline const char* ReasonName(std::uint32_t reasons)
    {
        if (reasons & ReasonMenu)
            return "menu";
        if (reasons & ReasonLoading)
            return "loading";
        if (reasons & ReasonFade)
            return "fade";
        if (reasons & ReasonCutscene)
            return "cutscene";
        return "none";
    }

    // A pause makes a cut unless only the pause menu held it: the world
    // stands still under the menu, so the frames on both sides join.
    inline bool PauseMakesCut(std::uint32_t reasonsDuringPause)
    {
        return (reasonsDuringPause & ~static_cast<std::uint32_t>(ReasonMenu)) != 0;
    }

    // ------------------------------------------------------------------
    // Restricted-camera spans (clip section SPAN).

    enum class SpanKind : std::uint8_t
    {
        Cutscene = 0,     // only exists when cutscenes are recorded
        ScriptCamera = 1, // a script camera that is not ours rendered
        ControlsOff = 2   // the player had no control
    };
    constexpr std::uint8_t kSpanKindCount = 3;

    struct Span
    {
        std::uint32_t startMs = 0;
        std::uint32_t endMs = 0; // equal to start while still open
        SpanKind kind = SpanKind::Cutscene;
        bool open = false;       // recording only
    };

    // Opens a span of `kind` when `active` turns on and closes it when it
    // turns off, at `nowMs`. An open span's end follows the clock. A span
    // that closes with no length is dropped.
    inline void UpdateSpan(std::vector<Span>& spans, SpanKind kind, bool active, std::uint32_t nowMs)
    {
        Span* open = nullptr;
        for (Span& span : spans)
            if (span.open && span.kind == kind)
                open = &span;
        if (active && !open)
        {
            spans.push_back({nowMs, nowMs, kind, true});
            return;
        }
        if (!open)
            return;
        open->endMs = nowMs > open->endMs ? nowMs : open->endMs;
        if (active)
            return;
        open->open = false;
        if (open->endMs == open->startMs)
            spans.erase(spans.begin() + (open - spans.data()));
    }

    // Closes every open span at `nowMs` (recording stop).
    inline void CloseSpans(std::vector<Span>& spans, std::uint32_t nowMs)
    {
        for (const SpanKind kind : {SpanKind::Cutscene, SpanKind::ScriptCamera, SpanKind::ControlsOff})
            UpdateSpan(spans, kind, false, nowMs);
    }

    // The span of `kind` holding `timeMs` (start inclusive, end exclusive), or null.
    inline const Span* SpanAt(const std::vector<Span>& spans, SpanKind kind, std::uint64_t timeMs)
    {
        for (const Span& span : spans)
            if (span.kind == kind && timeMs >= span.startMs && timeMs < span.endMs)
                return &span;
        return nullptr;
    }

    // ------------------------------------------------------------------
    // Cuts (clip section CUTS): the instants the clock went on after a
    // pause that was not only the menu. Sorted ascending.

    // The first cut c with a < c <= b, or false. Frames at or before a cut
    // were recorded before it; frames after it, after the pause.
    inline bool CutBetween(const std::vector<std::uint32_t>& cuts, std::uint32_t a, std::uint32_t b, std::uint32_t& cut)
    {
        for (const std::uint32_t c : cuts)
        {
            if (c > b)
                return false;
            if (c > a)
            {
                cut = c;
                return true;
            }
        }
        return false;
    }

    // The blend factor between samples at a and b for the playhead t, with
    // `t01` the plain interpolation factor: across a cut the sample on the
    // playhead's side of it, whole (0 before the cut, 1 from the cut on).
    inline float CutAwareFactor(const std::vector<std::uint32_t>* cuts, std::uint32_t a, std::uint32_t b,
        std::uint64_t t, float t01)
    {
        std::uint32_t cut = 0;
        if (!cuts || cuts->empty() || !CutBetween(*cuts, a, b, cut))
            return t01;
        return t < cut ? 0.0f : 1.0f;
    }

    // Adds a cut (kept sorted, no duplicates).
    inline void AddCut(std::vector<std::uint32_t>& cuts, std::uint32_t atMs)
    {
        if (!cuts.empty() && cuts.back() >= atMs)
        {
            if (cuts.back() == atMs)
                return;
            for (std::size_t n = 0; n < cuts.size(); ++n)
                if (cuts[n] == atMs)
                    return;
            cuts.push_back(atMs);
            for (std::size_t n = cuts.size() - 1; n > 0 && cuts[n - 1] > cuts[n]; --n)
                std::swap(cuts[n - 1], cuts[n]);
            return;
        }
        cuts.push_back(atMs);
    }
}
