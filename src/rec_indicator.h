#pragma once

// The recording indicator (Replay Settings "Recording indicator",
// RedemptionEditor.ini RecordingIndicator): how the recording notices show
// while the replay records, for players who capture their screen.
//   normal  the notices as they always were, in the game frame (the default).
//   brief   the REC (or REPLAY) row for kBriefMs when a recording starts,
//           stops or saves a clip, then hidden. The "Recording paused" and
//           saving notices as in normal.
//   hidden  the recording notices (paused, REC or REPLAY, saving, saved) in a
//           small window over the game that screen capture leaves out
//           (editor/rec_indicator_window.h); brief when that window cannot
//           show over the game (Choose).
//   off     no REC, REPLAY or paused notice; the saving notices stay.
// The core owns the value (script.cpp SetRecordingIndicator, saved to the ini;
// Snapshot::recordingIndicator); the editor draws it.
//
// Pure (no Windows header).

#include <cstdint>

namespace replay::recindicator
{
    enum class Mode : std::uint32_t
    {
        Normal = 0,
        Brief = 1,
        Hidden = 2,
        Off = 3
    };
    constexpr std::uint32_t kModeCount = 4;

    // How long the brief mode shows the row after an event.
    constexpr std::uint64_t kBriefMs = 3000;

    // WDA_EXCLUDEFROMCAPTURE needs Windows 10 2004 (build 19041); before it the
    // flag acts as WDA_MONITOR (a black box in the capture).
    constexpr std::uint32_t kExcludeFromCaptureBuild = 19041;

    // The editor lays out on 1080 units of the display height.
    constexpr float kLayoutHeight = 1080.0f;

    inline Mode FromValue(std::uint32_t value) { return value < kModeCount ? static_cast<Mode>(value) : Mode::Normal; }

    // The ini's words.
    inline const char* IniValue(Mode mode)
    {
        switch (mode)
        {
        case Mode::Brief: return "brief";
        case Mode::Hidden: return "hidden";
        case Mode::Off: return "off";
        default: return "normal";
        }
    }

    // A word of IniValue in any case, or its number; anything else is normal.
    inline Mode Parse(const char* text)
    {
        if (!text)
            return Mode::Normal;
        if (text[0] >= '0' && text[0] <= '9' && text[1] == '\0')
            return FromValue(static_cast<std::uint32_t>(text[0] - '0'));
        for (std::uint32_t n = 0; n < kModeCount; ++n)
        {
            const char* word = IniValue(static_cast<Mode>(n));
            std::uint32_t at = 0;
            while (text[at] && word[at] && (text[at] | 0x20) == word[at])
                ++at;
            if (!text[at] && !word[at])
                return static_cast<Mode>(n);
        }
        return Mode::Normal;
    }

    // The Replay Settings row: the arrows stop at both ends, a click (wrap)
    // goes forward and wraps.
    inline Mode Step(Mode mode, int direction, bool wrap)
    {
        const int at = static_cast<int>(mode) + direction;
        const int last = static_cast<int>(kModeCount) - 1;
        if (wrap && at > last)
            return Mode::Normal;
        return static_cast<Mode>(at < 0 ? 0 : at > last ? last : at);
    }

    // Why the hidden mode shows as brief.
    enum class Fallback : std::uint32_t
    {
        None = 0,
        OldWindows = 1,          // older than Windows 10 2004
        ExclusiveFullscreen = 2, // the game owns its monitor: no window shows over it
        AffinityFailed = 3,      // SetWindowDisplayAffinity refused the window
        WindowFailed = 4         // the window or its drawing could not be made
    };

    inline const char* FallbackName(Fallback why)
    {
        switch (why)
        {
        case Fallback::OldWindows: return "old_windows";
        case Fallback::ExclusiveFullscreen: return "exclusive_fullscreen";
        case Fallback::AffinityFailed: return "affinity_failed";
        case Fallback::WindowFailed: return "window_failed";
        default: return "none";
        }
    }

    struct Environment
    {
        bool windowsSupports = true;
        bool exclusiveFullscreen = false;
        bool affinityFailed = false;
        bool windowFailed = false;
    };

    struct Choice
    {
        Mode mode = Mode::Normal; // what is drawn
        Fallback why = Fallback::None;
    };

    inline Choice Choose(Mode wanted, const Environment& env)
    {
        if (wanted != Mode::Hidden)
            return {wanted, Fallback::None};
        const Fallback why = !env.windowsSupports ? Fallback::OldWindows
            : env.exclusiveFullscreen            ? Fallback::ExclusiveFullscreen
            : env.affinityFailed                 ? Fallback::AffinityFailed
            : env.windowFailed                   ? Fallback::WindowFailed
                                                 : Fallback::None;
        return why == Fallback::None ? Choice{Mode::Hidden, why} : Choice{Mode::Brief, why};
    }

    inline bool WindowsSupports(std::uint32_t major, std::uint32_t build)
    {
        return major > 10 || (major == 10 && build >= kExcludeFromCaptureBuild);
    }

    struct Rect
    {
        long left = 0, top = 0, right = 0, bottom = 0;
    };

    inline bool CoversMonitor(const Rect& window, const Rect& monitor)
    {
        return monitor.right > monitor.left && monitor.bottom > monitor.top && window.left <= monitor.left && window.top <= monitor.top &&
            window.right >= monitor.right && window.bottom >= monitor.bottom;
    }

    // Exclusive fullscreen: the game window covers its monitor and either its
    // DXGI swap chain is in fullscreen state or the shell reports an exclusive
    // Direct3D full screen application (SHQueryUserNotificationState
    // QUNS_RUNNING_D3D_FULL_SCREEN, what notifications use to stay away). A
    // borderless window covers the monitor too and is neither.
    inline bool ExclusiveFullscreen(bool coversMonitor, bool swapChainFullscreen, bool shellD3dFullScreen)
    {
        return coversMonitor && (swapChainFullscreen || shellD3dFullScreen);
    }

    // The window's scale: the game's client height is 1080 layout units, as the
    // editor's, so the indicator keeps its size relative to the picture at
    // 1080p, 1440p and 4K.
    inline float Scale(long clientHeight) { return clientHeight > 0 ? static_cast<float>(clientHeight) / kLayoutHeight : 0.0f; }

    // The width of a notice row in layout units (replay_editor_addon.cpp
    // Notice), from its label's and value's widths in units.
    inline float RowWidth(float labelWidth, float valueWidth)
    {
        const float needed = 54.0f + labelWidth + (valueWidth > 0.0f ? valueWidth + 28.0f : 0.0f);
        return needed > 250.0f ? needed : 250.0f;
    }

    // What the row shows: a manual recording (REC and its clock) or the
    // replay buffer (REPLAY).
    enum class Kind : std::uint32_t
    {
        None = 0,
        Rec = 1,
        Replay = 2
    };

    // replay_editor_addon.cpp DrawRecordingNotices' rule.
    inline Kind KindOf(bool bufferRunning, bool bufferManual, std::uint32_t recordingMs)
    {
        if (recordingMs > 0 && (!bufferRunning || bufferManual))
            return Kind::Rec;
        return bufferRunning ? Kind::Replay : Kind::None;
    }

    // The brief mode's clock. An event shows the row for kBriefMs: a recording
    // starts (the first frame seen recording counts), stops, turns from the
    // buffer to a manual recording or back, or a clip is saved while it goes
    // on (the buffer's save). After a stop the row of the recording that ended
    // shows, its clock held; the save of a stopped recording is told by the
    // "clip saved" notice instead.
    struct BriefTimer
    {
        bool started = false;
        Kind seen = Kind::None;
        std::uint64_t savedSequence = 0;
        std::uint64_t until = 0;
        Kind ended = Kind::None;
        std::uint32_t endedMs = 0;
        std::uint32_t lastMs = 0;

        void Update(std::uint64_t now, Kind kind, std::uint32_t recordingMs, std::uint64_t savedClipSequence)
        {
            if (!started)
            {
                started = true;
                seen = kind;
                savedSequence = savedClipSequence;
                lastMs = recordingMs;
                if (kind != Kind::None)
                    until = now + kBriefMs;
                return;
            }
            if (kind != seen)
            {
                ended = kind == Kind::None ? seen : Kind::None;
                endedMs = lastMs;
                seen = kind;
                until = now + kBriefMs;
            }
            if (savedClipSequence != savedSequence)
            {
                savedSequence = savedClipSequence;
                if (kind != Kind::None)
                {
                    ended = Kind::None;
                    until = now + kBriefMs;
                }
            }
            if (kind == Kind::Rec)
                lastMs = recordingMs;
        }

        bool Visible(std::uint64_t now) const { return now < until && Shown() != Kind::None; }

        // The row while Visible: the live one, or the one that just stopped.
        Kind Shown() const { return seen != Kind::None ? seen : ended; }
        bool Stopped() const { return seen == Kind::None; }
    };
}
