// src/editor_cursor.h: the editor shows a cursor
// whenever its UI wants the mouse, whichever ReShade serves it, and hands
// the mouse back to the camera when it does not.
#include "editor_cursor.h"

#include <cstdio>
#include <cstring>

namespace
{
    using namespace replay::editor::cursor;

    int g_failures = 0;
    int g_checks = 0;

    void Check(bool condition, const char* message)
    {
        ++g_checks;
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    Frame Interactive(bool afterWrite)
    {
        Frame f;
        f.interactive = true;
        f.cursorAfterWrite = afterWrite;
        f.mouseValid = true;
        f.x = 1900.0f;
        f.y = 1000.0f;
        f.width = 3840.0f;
        f.height = 2160.0f;
        f.reason = Reason::Screen;
        return f;
    }

    void TestReShade68()
    {
        // ReShade 6.8 (a compatibility table): the write is lost, the editor draws.
        const Decision d = Decide(Interactive(false), IoWrites::Unknown);
        Check(d.owner == Owner::Editor, "the editor owns the mouse");
        Check(d.writes == IoWrites::Lost, "the lost write is measured");
        Check(d.drawn == Drawn::Editor, "the editor draws its arrow when ImGui will not");
        Check(d.drawX == 1900.0f && d.drawY == 1000.0f, "the arrow at the mouse");
    }

    void TestReShade66()
    {
        // ReShade 6.6 (the same ImGui): the write reaches, ImGui draws, no double cursor.
        const Decision d = Decide(Interactive(true), IoWrites::Unknown);
        Check(d.writes == IoWrites::Reach, "the write reaches");
        Check(d.drawn == Drawn::ImGui, "ImGui draws its cursor, the editor does not");
    }

    void TestReShadeWindow()
    {
        // ReShade's window open over the editor: ImGui draws (ReShade's flag), no double.
        Frame f = Interactive(true);
        f.reshadeCursor = true;
        Decision d = Decide(f, IoWrites::Lost);
        Check(d.drawn == Drawn::ImGui, "ReShade's window draws the cursor");
        Check(d.writes == IoWrites::Lost, "the measure is kept while ReShade's window hides it");
        // ReShade's window alone (editor closed): ReShade owns the mouse.
        f.interactive = false;
        d = Decide(f, IoWrites::Lost);
        Check(d.owner == Owner::ReShade, "ReShade's window owns the mouse");
        Check(d.drawn == Drawn::ImGui, "ReShade draws its cursor");
    }

    void TestGame()
    {
        // Editor closed, Edit Camera or the UI hidden: the camera has the mouse, no cursor.
        const Reason reasons[] = {Reason::Closed, Reason::EditCamera, Reason::UiHidden};
        for (Reason r : reasons)
        {
            Frame f = Interactive(false);
            f.interactive = false;
            f.reason = r;
            const Decision d = Decide(f, IoWrites::Lost);
            Check(d.owner == Owner::Game, "the game owns the mouse when the editor does not want it");
            Check(d.drawn == Drawn::None, "no cursor over the free camera");
        }
    }

    void TestOutside()
    {
        // A second (virtual) monitor: the mouse left the picture. The arrow
        // stays at the edge, also when ImGui draws (its cursor would be off it).
        Frame f = Interactive(true);
        f.x = 4500.0f;
        f.y = -20.0f;
        Decision d = Decide(f, IoWrites::Unknown);
        Check(!d.inside, "outside the picture");
        Check(d.drawn == Drawn::Editor, "the editor's arrow at the edge");
        Check(d.drawX == 3839.0f && d.drawY == 0.0f, "clamped to the picture");
        // No position at all: nothing to draw.
        f.mouseValid = false;
        d = Decide(f, IoWrites::Unknown);
        Check(d.drawn == Drawn::ImGui, "no position: only ImGui's (which draws nothing either)");
        f.cursorAfterWrite = false;
        d = Decide(f, IoWrites::Unknown);
        Check(d.drawn == Drawn::None, "no position, lost write: nothing");
    }

    bool Near(float a, float b) { return a - b < 0.001f && b - a < 0.001f; }

    void TestInkRect()
    {
        // The painted cursor: 32 px tall at 1080 lines, its tip (texel 5, 0 of
        // 128) on the mouse, scaled like the UI at 1440 and 2160 lines.
        Rect r = InkRect(100.0f, 50.0f, 1.0f, 128, 128);
        Check(Near(r.y1 - r.y0, 32.0f) && Near(r.x1 - r.x0, 32.0f), "32 px at 1080p");
        Check(Near(r.x0 + kInkTipU * (r.x1 - r.x0), 100.0f) && Near(r.y0, 50.0f), "the tip on the mouse at 1080p");
        Check(Near(r.x0, 100.0f - 1.25f), "the tip 1.25 px right of the picture's left edge at 1080p");
        r = InkRect(100.0f, 50.0f, 1440.0f / 1080.0f, 128, 128);
        Check(Near(r.y1 - r.y0, 32.0f * 1440.0f / 1080.0f), "scaled at 1440p");
        Check(Near(r.x0 + kInkTipU * (r.x1 - r.x0), 100.0f) && Near(r.y0, 50.0f), "the tip on the mouse at 1440p");
        r = InkRect(3839.0f, 2159.0f, 2.0f, 128, 128);
        Check(Near(r.y1 - r.y0, 64.0f) && Near(r.x0, 3839.0f - 2.5f) && Near(r.y0, 2159.0f), "64 px at 4K, the tip on the mouse");
        r = InkRect(10.0f, 10.0f, 720.0f / 1080.0f, 128, 128);
        Check(Near(r.y1 - r.y0, 32.0f), "never below its 1080p size");
        r = InkRect(0.0f, 0.0f, 1.0f, 256, 512);
        Check(Near(r.x1 - r.x0, 16.0f) && Near(r.y1 - r.y0, 32.0f), "another PNG keeps its aspect");
        Check(Near(r.x0, -kInkTipU * 16.0f), "and its tip as a fraction");
    }

    void TestStateLog()
    {
        StateLog log;
        char line[320];
        const Frame f = Interactive(false);
        const Decision d = Decide(f, IoWrites::Unknown);
        Check(log.Update(f, d, "1.92.5", 19222, line, sizeof(line)), "the first state is logged");
        Check(std::strstr(line, "CURSOR_STATE,owner=editor,drawn=editor,why=screen,reshade_window=0,imgui_io=lost,mouse=inside") == line,
            "the line says who owns and who draws");
        Check(std::strstr(line, "reshade_imgui=1.92.5,addon_imgui=19222") != nullptr, "the two ImGui versions");
        Frame moved = f;
        moved.x = 10.0f;
        Check(!log.Update(moved, Decide(moved, IoWrites::Lost), "1.92.5", 19222, line, sizeof(line)), "a move alone is not a change");
        Frame closed = f;
        closed.interactive = false;
        closed.reason = Reason::Closed;
        Check(log.Update(closed, Decide(closed, IoWrites::Lost), "1.92.5", 19222, line, sizeof(line)), "closing is a change");
        Check(std::strstr(line, "owner=game,drawn=none,why=closed") != nullptr, "the game has the mouse");
        // The cap: a flapping state stops at kMaxLines, the last says capped=1.
        StateLog capped;
        bool sawCapped = false;
        for (std::uint32_t i = 0; i < StateLog::kMaxLines + 50; ++i)
        {
            const Frame& g = (i % 2) ? f : closed;
            if (capped.Update(g, Decide(g, IoWrites::Lost), "1.92.5", 19222, line, sizeof(line)) && std::strstr(line, "capped=1"))
                sawCapped = true;
        }
        Check(capped.Lines() == StateLog::kMaxLines, "the lines stop at the cap");
        Check(capped.Changes() == StateLog::kMaxLines + 50, "the changes are still counted");
        Check(sawCapped, "the last line says capped=1");
    }

    Frame WithInk(bool afterWrite)
    {
        Frame f = Interactive(afterWrite);
        f.inkReady = true;
        return f;
    }

    void TestInkEverywhere()
    {
        // ReShade 6.6 / 6.7 (the write reaches): the user's log said
        // drawn=imgui, the small default arrow. With the ink art the editor
        // draws its painted cursor and hides ImGui's: one cursor.
        Decision d = Decide(WithInk(true), IoWrites::Unknown);
        Check(d.owner == Owner::Editor, "6.6: the editor owns the mouse");
        Check(d.writes == IoWrites::Reach, "6.6: the write still measured as reaching");
        Check(d.drawn == Drawn::Ink, "6.6: the ink cursor, not ImGui's");
        Check(d.hideImGui, "6.6: ImGui's cursor hidden (no double cursor)");
        Check(d.drawX == 1900.0f && d.drawY == 1000.0f, "6.6: the ink at the mouse");
        // ReShade 6.8 (the write is lost): the ink cursor too.
        d = Decide(WithInk(false), IoWrites::Unknown);
        Check(d.writes == IoWrites::Lost && d.drawn == Drawn::Ink && d.hideImGui, "6.8: the ink cursor");
        // Outside the picture (another monitor), either ReShade: at the edge.
        Frame f = WithInk(true);
        f.x = 4500.0f;
        f.y = -20.0f;
        d = Decide(f, IoWrites::Reach);
        Check(!d.inside && d.drawn == Drawn::Ink, "outside: the ink cursor");
        Check(d.drawX == 3839.0f && d.drawY == 0.0f, "outside: clamped to the picture");
        // ReShade's own window open: its cursor, untouched.
        f = WithInk(true);
        f.reshadeCursor = true;
        d = Decide(f, IoWrites::Reach);
        Check(d.drawn == Drawn::ImGui && !d.hideImGui, "ReShade's window: ReShade's cursor, not hidden");
        f.interactive = false;
        d = Decide(f, IoWrites::Reach);
        Check(d.owner == Owner::ReShade && d.drawn == Drawn::ImGui && !d.hideImGui, "ReShade's window alone: its cursor");
        // The game has the mouse (closed, Edit Camera, UI hidden): nothing.
        f = WithInk(true);
        f.interactive = false;
        f.reason = Reason::EditCamera;
        d = Decide(f, IoWrites::Reach);
        Check(d.owner == Owner::Game && d.drawn == Drawn::None && !d.hideImGui, "the game's mouse: no cursor, ImGui left alone");
        // No position: nothing to draw, ImGui left as before.
        f = WithInk(true);
        f.mouseValid = false;
        d = Decide(f, IoWrites::Unknown);
        Check(d.drawn == Drawn::ImGui && !d.hideImGui, "no position: as before");
        // The art missing: today's rules (ImGui's on 6.6, the drawn arrow on 6.8).
        Check(Decide(Interactive(true), IoWrites::Unknown).drawn == Drawn::ImGui, "no art, 6.6: ImGui's");
        Check(Decide(Interactive(false), IoWrites::Unknown).drawn == Drawn::Editor, "no art, 6.8: the drawn arrow");
        Check(!Decide(Interactive(false), IoWrites::Unknown).hideImGui, "no art: nothing hidden");
        // The log line says drawn=ink.
        StateLog log;
        char line[320];
        const Frame g = WithInk(true);
        Check(log.Update(g, Decide(g, IoWrites::Unknown), "1.92.2", 19222, line, sizeof(line)), "the ink state is logged");
        Check(std::strstr(line, "CURSOR_STATE,owner=editor,drawn=ink,why=screen,reshade_window=0,imgui_io=reach,mouse=inside") == line,
            "the line says drawn=ink");
    }
}

int RunEditorCursorTests()
{
    TestReShade68();
    TestReShade66();
    TestReShadeWindow();
    TestGame();
    TestOutside();
    TestInkRect();
    TestStateLog();
    TestInkEverywhere();
    std::printf("editor_cursor_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}
