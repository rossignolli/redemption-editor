#pragma once

// Who owns the mouse and who draws the cursor while the editor is on screen.
// Pure rules (no ReShade, no ImGui) so tests/editor_cursor_tests.cpp can pin
// them.
//
// ReShade 6.8 runs ImGui 1.92.5 (19250) and serves an add-on built against
// 1.92.2 (19222, ours) through a compatibility table whose GetIO() returns a
// static copy converted one way from the real IO: a write to
// io.MouseDrawCursor never reaches ReShade's context, so ImGui draws the
// cursor only while ReShade's own window is open. The editor reads the flag
// back from a fresh GetIO() after its write and draws its own arrow when ImGui
// will not.
//
// The painted ink cursor (editor/ui/cursor_ink.png) is the editor's in every
// ReShade: when its art is loaded, the editor draws it whenever it owns the
// mouse (6.6 and 6.7, where the write reaches and ImGui would draw its small
// default arrow, as well as 6.8), and hides ImGui's (io.MouseDrawCursor back
// to false after the measure; a lost write changes nothing), so there is never
// a double cursor. ReShade's own window open: its cursor, untouched. Without
// the art: the rules above (ImGui's where it draws, the drawn arrow where not).

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace replay::editor::cursor
{
    // Who gets the mouse this frame.
    enum class Owner : std::uint8_t
    {
        Game = 0,   // the free camera / the game (mouse look)
        Editor = 1, // the editor's UI (ReShade blocks the game's mouse for it)
        ReShade = 2 // ReShade's own window is open (it blocks the game's mouse)
    };

    // Who draws a cursor this frame.
    enum class Drawn : std::uint8_t
    {
        None = 0,
        ImGui = 1, // ImGui's software cursor (io.MouseDrawCursor in ReShade's context)
        Editor = 2, // the editor's own drawn arrow on the foreground draw list (no ink art)
        Ink = 3     // the editor's painted ink cursor, ImGui's hidden
    };

    // Whether the add-on's writes to ImGuiIO reach ReShade's context.
    enum class IoWrites : std::uint8_t
    {
        Unknown = 0, // not measured yet (ReShade's window was open, or the UI never wanted the mouse)
        Reach = 1,   // same ImGui version as ReShade: GetIO() is the real one
        Lost = 2     // a compatibility table: GetIO() is a copy
    };

    // Why the editor wants (or not) the mouse; for the log only.
    enum class Reason : std::uint8_t
    {
        Closed = 0,     // the editor is closed (the playback bar, the game)
        Screen = 1,     // an editor screen with buttons
        Note = 2,       // the QA note box
        EditCamera = 3, // Edit Camera: the free camera has the mouse
        UiHidden = 4    // H hid the clip editor to fly
    };

    struct Frame
    {
        bool interactive = false;      // the editor's UI wants the mouse (and blocks the game's)
        bool reshadeCursor = false;    // io.MouseDrawCursor as ReShade left it, before the editor's write (its window is open)
        bool cursorAfterWrite = false; // io.MouseDrawCursor read from a fresh GetIO() after the editor wrote true
        bool mouseValid = false;       // io.MousePos is a position (ImGui's -FLT_MAX means none)
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f; // io.DisplaySize
        float height = 0.0f;
        Reason reason = Reason::Closed;
        bool inkReady = false; // the painted cursor's art is loaded (ui_art Art::Cursor)
    };

    struct Decision
    {
        Owner owner = Owner::Game;
        Drawn drawn = Drawn::None;
        IoWrites writes = IoWrites::Unknown;
        bool inside = false; // the mouse is inside the picture
        float drawX = 0.0f;  // where the editor's arrow goes (clamped to the picture)
        float drawY = 0.0f;
        bool hideImGui = false; // the editor writes io.MouseDrawCursor = false after its measure (Ink)
    };

    inline bool Inside(const Frame& f)
    {
        return f.mouseValid && f.x >= 0.0f && f.y >= 0.0f && f.x < f.width && f.y < f.height;
    }

    inline float Clamp(float v, float lo, float hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // `writes` is the last measure (kept while ReShade's window hides it).
    inline Decision Decide(const Frame& f, IoWrites writes)
    {
        Decision d;
        d.writes = writes;
        d.owner = f.interactive ? Owner::Editor : (f.reshadeCursor ? Owner::ReShade : Owner::Game);
        d.inside = Inside(f);
        // Measured only when the editor's write is the one that matters:
        // with ReShade's window open the flag was already true.
        if (f.interactive && !f.reshadeCursor)
            d.writes = f.cursorAfterWrite ? IoWrites::Reach : IoWrites::Lost;
        const bool imguiDraws = f.reshadeCursor || (f.interactive && f.cursorAfterWrite);
        if (f.interactive && !f.reshadeCursor && f.inkReady && f.mouseValid)
        {
            // The painted cursor whatever ImGui would do; ImGui's hidden.
            d.drawn = Drawn::Ink;
            d.hideImGui = true;
        }
        else if (!f.mouseValid)
            d.drawn = imguiDraws ? Drawn::ImGui : Drawn::None; // ImGui draws nothing either without a position
        else if (f.interactive && (!imguiDraws || !d.inside))
            d.drawn = Drawn::Editor; // ImGui will not draw it, or would draw it off the picture (another monitor)
        else
            d.drawn = imguiDraws ? Drawn::ImGui : Drawn::None;
        if (d.drawn == Drawn::Editor || d.drawn == Drawn::Ink)
        {
            d.drawX = Clamp(f.x, 0.0f, f.width > 1.0f ? f.width - 1.0f : 0.0f);
            d.drawY = Clamp(f.y, 0.0f, f.height > 1.0f ? f.height - 1.0f : 0.0f);
        }
        return d;
    }

    inline const char* OwnerName(Owner o)
    {
        switch (o)
        {
        case Owner::Editor: return "editor";
        case Owner::ReShade: return "reshade";
        default: return "game";
        }
    }

    inline const char* DrawnName(Drawn d)
    {
        switch (d)
        {
        case Drawn::ImGui: return "imgui";
        case Drawn::Editor: return "editor";
        case Drawn::Ink: return "ink";
        default: return "none";
        }
    }

    inline const char* WritesName(IoWrites w)
    {
        switch (w)
        {
        case IoWrites::Reach: return "reach";
        case IoWrites::Lost: return "lost";
        default: return "unknown";
        }
    }

    inline const char* ReasonName(Reason r)
    {
        switch (r)
        {
        case Reason::Screen: return "screen";
        case Reason::Note: return "note";
        case Reason::EditCamera: return "edit_camera";
        case Reason::UiHidden: return "ui_hidden";
        default: return "closed";
        }
    }

    // The CURSOR_STATE line: one per change of what it says, capped.
    class StateLog
    {
    public:
        static constexpr std::uint32_t kMaxLines = 200;

        // Writes a line into `out` and returns true when the state changed
        // (and the cap allows it). The last allowed line says capped=1.
        bool Update(const Frame& f, const Decision& d, const char* reshadeImgui, unsigned addonImgui, char* out,
            std::size_t capacity)
        {
            const Key key{d.owner, d.drawn, d.writes, f.reason, f.reshadeCursor, f.mouseValid, d.inside};
            if (m_have && key == m_last)
                return false;
            m_last = key;
            m_have = true;
            ++m_changes;
            if (m_lines >= kMaxLines)
                return false;
            ++m_lines;
            const bool capped = m_lines == kMaxLines;
            std::snprintf(out, capacity,
                "CURSOR_STATE,owner=%s,drawn=%s,why=%s,reshade_window=%d,imgui_io=%s,mouse=%s,display=%.0fx%.0f,"
                "reshade_imgui=%s,addon_imgui=%u,changes=%u%s",
                OwnerName(d.owner), DrawnName(d.drawn), ReasonName(f.reason), f.reshadeCursor ? 1 : 0, WritesName(d.writes),
                !f.mouseValid ? "none" : (d.inside ? "inside" : "outside"), f.width, f.height,
                reshadeImgui ? reshadeImgui : "?", addonImgui, m_changes, capped ? ",capped=1" : "");
            return true;
        }

        std::uint32_t Lines() const { return m_lines; }
        std::uint32_t Changes() const { return m_changes; }

    private:
        struct Key
        {
            Owner owner;
            Drawn drawn;
            IoWrites writes;
            Reason reason;
            bool reshadeWindow;
            bool mouseValid;
            bool inside;
            bool operator==(const Key& o) const
            {
                return owner == o.owner && drawn == o.drawn && writes == o.writes && reason == o.reason &&
                    reshadeWindow == o.reshadeWindow && mouseValid == o.mouseValid && inside == o.inside;
            }
        };
        Key m_last{};
        bool m_have = false;
        std::uint32_t m_lines = 0;
        std::uint32_t m_changes = 0;
    };

    // The painted cursor (editor/ui/cursor_ink.png, ui_art Art::Cursor): a
    // dry-ink arrow, 128x128, its tip at texel (5, 0). Drawn 32 px tall at
    // 1080 lines, scaled like the rest of the UI (never below its 1080p size,
    // like the drawn arrow); the tip texel's corner on the mouse, as a
    // hardware cursor's hotspot. The tip is kept as a fraction of the picture,
    // so a larger or smaller PNG keeps it.
    constexpr float kInkTipU = 5.0f / 128.0f;
    constexpr float kInkTipV = 0.0f / 128.0f;
    constexpr float kInkHeight1080 = 32.0f;

    struct Rect
    {
        float x0 = 0.0f;
        float y0 = 0.0f;
        float x1 = 0.0f;
        float y1 = 0.0f;
    };

    // Where the painted cursor goes for the mouse at (x, y); `uiScale` is the
    // editor's (display height / 1080), `texW` x `texH` the PNG's size.
    inline Rect InkRect(float x, float y, float uiScale, int texW, int texH)
    {
        const float s = uiScale > 1.0f ? uiScale : 1.0f;
        const float h = kInkHeight1080 * s;
        const float w = texH > 0 ? h * static_cast<float>(texW) / static_cast<float>(texH) : h;
        Rect r;
        r.x0 = x - kInkTipU * w;
        r.y0 = y - kInkTipV * h;
        r.x1 = r.x0 + w;
        r.y1 = r.y0 + h;
        return r;
    }

    // The drawn arrow (when the painted one is missing): its outline at 1080p
    // (the tip at 0,0), the classic pointer.
    constexpr int kArrowPoints = 7;
    constexpr float kArrow[kArrowPoints][2] = {
        {0.0f, 0.0f}, {0.0f, 17.0f}, {4.0f, 13.0f}, {7.5f, 20.0f}, {10.0f, 19.0f}, {6.5f, 12.0f}, {12.0f, 12.0f}};
}
