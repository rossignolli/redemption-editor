// src/editor_ui_qa.h: when H hides the replay UI and when it comes back; the
// report key's texts and the reminder when a playback stops with marks waiting.
#include "editor_ui_qa.h"
#include "qa_marks.h"

#include <string>
#include <vector>

#include <cstdio>
#include <cstring>

namespace
{
    using namespace replay::editor::uiqa;

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

    void TestHideKey()
    {
        HideKeyContext c;
        c.closedScreen = true;
        c.playing = true;
        Check(HideKeyApplies(c), "H works with the editor closed while a clip plays");
        c.playing = false;
        Check(!HideKeyApplies(c), "H does nothing with the editor closed and nothing playing (the horse whistle)");
        c.playing = true;
        c.debug = true;
        Check(!HideKeyApplies(c), "H does nothing in debug mode (no playback bar)");
        c.debug = false;
        c.noteOpen = true;
        Check(!HideKeyApplies(c), "H types in the QA note box");

        HideKeyContext edit;
        edit.editScreen = true;
        edit.playing = true;
        Check(HideKeyApplies(edit), "H works in the clip editor as before");
        edit.editingCamera = true;
        Check(!HideKeyApplies(edit), "H does nothing in Edit Camera");

        HideKeyContext home;
        home.playing = true;
        Check(!HideKeyApplies(home), "H does nothing on the menu screens");
    }

    void TestHideState()
    {
        HideState s;
        s.Tick(1000, true);
        s.Toggle();
        Check(s.HiddenOn(false, true, true), "hidden with the editor closed while playing");
        Check(s.HiddenOn(true, false, true), "hidden in the clip editor while playing");
        Check(!s.HiddenOn(false, false, true), "a menu screen is never hidden");
        // A project's next clip: a short gap keeps it hidden.
        s.Tick(2000, false);
        s.Tick(2000 + HideState::kShowAfterStopMs - 1, false);
        Check(s.hidden, "a gap shorter than the reset keeps the UI hidden");
        Check(!s.HiddenOn(false, true, false), "nothing is hidden while nothing plays");
        s.Tick(2600, true);
        Check(s.hidden, "the next clip starts hidden");
        // The end of the playback.
        s.Tick(5000, false);
        s.Tick(5000 + HideState::kShowAfterStopMs, false);
        Check(!s.hidden, "the UI comes back after the playback ends (the next F7 starts with the bar)");
        s.Toggle();
        s.Show();
        Check(!s.hidden, "F2 or E show the UI");
        s.Toggle();
        s.Toggle();
        Check(!s.hidden, "H again shows the UI");
    }

    void TestTexts()
    {
        char text[160];
        FormatMarkNotice(text, sizeof(text), 3);
        Check(std::strcmp(text, "Problem marked (3 waiting). Shift+F1 saves the report when you are done.") == 0, "mark notice with a count");
        FormatMarkNotice(text, sizeof(text), 0);
        Check(std::strcmp(text, "Problem marked. Shift+F1 saves the report when you are done.") == 0, "mark notice without a count");
        FormatStopReminder(text, sizeof(text), 1);
        Check(std::strcmp(text, "1 problem mark not sent yet. Press Shift+F1 to save the report.") == 0, "reminder, one mark");
        FormatStopReminder(text, sizeof(text), 12);
        Check(std::strcmp(text, "12 problem marks not sent yet. Press Shift+F1 to save the report.") == 0, "reminder, marks");
        FormatReportKey(text, sizeof(text), 3);
        Check(std::strcmp(text, "Send report (3)") == 0, "key bar label");
        char small[8];
        FormatStopReminder(small, sizeof(small), 3);
        Check(small[sizeof(small) - 1] == '\0' || std::strlen(small) < sizeof(small), "a short buffer stays terminated");
    }

    void TestStopReminder()
    {
        StopReminder r;
        Check(!r.Update(0, false, 3), "no reminder before any playback");
        Check(!r.Update(100, true, 3), "no reminder while playing");
        Check(!r.Update(200, false, 3), "no reminder at the stop itself");
        Check(!r.Update(200 + StopReminder::kSettleMs - 1, false, 3), "no reminder inside the settle");
        Check(r.Update(200 + StopReminder::kSettleMs, false, 3), "the reminder after the settle");
        Check(!r.Update(5000, false, 3), "the reminder only once per stop");
        // A clip opening after another: the gap is shorter than the settle.
        Check(!r.Update(6000, true, 3), "playing again");
        Check(!r.Update(6100, false, 3), "a short gap");
        Check(!r.Update(6500, true, 3), "the next clip plays: no reminder");
        // No marks waiting.
        Check(!r.Update(7000, false, 0), "stop without marks");
        Check(!r.Update(9000, false, 0), "no reminder without marks");
    }

    // From QA ("I keep getting the notification that problems have
    // been marked but I haven't reported anything"): the reminder follows the
    // player's own marks of this session (qa_marks.h CountPendingMarks), so
    // marks an earlier session left on disk never bring it back.
    void TestReminderOnlyForSessionMarks()
    {
        using replay::qa::CountPendingMarks;
        const std::vector<std::string> older = {"mark-20261020-101010-01", "mark-20261021-090000-01"};
        StopReminder r;
        // A new session: two stray folders, nothing pressed.
        std::uint32_t pending = CountPendingMarks({}, older).session;
        Check(pending == 0, "older folders: nothing pending");
        r.Update(0, true, pending);
        r.Update(100, false, pending);
        Check(!r.Update(100 + StopReminder::kSettleMs, false, pending), "older folders: no reminder after a playback");
        // The player presses Shift+F2 once.
        std::vector<std::string> disk = older;
        disk.push_back("mark-20261029-120000-01");
        pending = CountPendingMarks({"mark-20261029-120000-01"}, disk).session;
        Check(pending == 1, "one mark pressed now");
        r.Update(2000, true, pending);
        r.Update(3000, false, pending);
        Check(r.Update(3000 + StopReminder::kSettleMs, false, pending), "the player's mark: the reminder");
        char text[200];
        FormatStopReminder(text, sizeof(text), pending);
        Check(std::strcmp(text, "1 problem mark not sent yet. Press Shift+F1 to save the report.") == 0, "the reminder says how many and the key");
        // Shift+F1: the report takes every folder, the session's list clears.
        pending = CountPendingMarks({}, {}).session;
        r.Update(6000, true, pending);
        r.Update(7000, false, pending);
        Check(!r.Update(7000 + StopReminder::kSettleMs, false, pending), "after the report: no reminder");
        // The key bar: "Send report (N)" only with the session's marks
        // (ReportTip shows it when pending > 0).
        Check(CountPendingMarks({}, older).session == 0, "older folders: no Send report on the key bar");
    }

    void TestArtUploadLevels()
    {
        Check(ArtUploadLevels(true, 11) == 1, "Vulkan: the art goes up with level 0 only");
        Check(ArtUploadLevels(true, 1) == 1, "Vulkan: a 1x1 art keeps its one level");
        Check(ArtUploadLevels(false, 11) == 11, "D3D12: the whole mip chain");
        Check(ArtUploadLevels(false, 1) == 1, "D3D12: a 1x1 art keeps its one level");
        Check(ArtUploadLevels(true, 0) == 0 && ArtUploadLevels(false, 0) == 0, "no chain, no levels");
        // The row art (1024x64): eleven levels, one on Vulkan.
        Check(ArtUploadLevels(true, 11) == 1 && ArtUploadLevels(false, 11) == 11, "the row art: level 0 on Vulkan, the chain elsewhere");
    }

    void TestHomeMenu()
    {
        Check(!SaveBufferShown(false, 0), "the buffer off: no Save Replay Buffer");
        Check(!SaveBufferShown(false, 60000), "the buffer off (a stale count): no Save Replay Buffer");
        Check(!SaveBufferShown(true, 0), "the buffer on, nothing held yet: no Save Replay Buffer");
        Check(!SaveBufferShown(true, 999), "under a second held: nothing to save yet");
        Check(SaveBufferShown(true, 1000) && SaveBufferShown(true, 41000), "the buffer holding a second or more: listed");
        // Ids as the home's: 0 Edit, 1 Projects, 2 Clips, 3 Record, 4 Save, 5 Settings, 7 Back; 6 Close Project.
        // In the home's order: Recorded Clips first, then Edit, Projects, Record,
        // Save, Settings, Close Project (a project open) and Back to Game.
        const std::vector<int> off = {2, 0, 1, 3, 5, 7};
        const std::vector<int> on = {2, 0, 1, 3, 4, 5, 7};
        const std::vector<int> project = {2, 0, 1, 3, 4, 5, 6, 7};
        Check(KeepSelection(off, 0, on) == 0, "Save appears: Recorded Clips stays selected");
        Check(KeepSelection(off, 4, on) == 5, "Save appears: Replay Settings keeps the selection (moves down)");
        Check(KeepSelection(off, 5, on) == 6, "Save appears: Back to Game keeps the selection");
        Check(KeepSelection(on, 5, off) == 4, "Save goes: Replay Settings keeps the selection (moves up)");
        Check(KeepSelection(on, 4, off) == 4, "Save goes while selected: the item after it (Replay Settings) takes it");
        Check(KeepSelection(on, 0, project) == 0, "a project opens: Recorded Clips, the first item, stays");
        Check(KeepSelection(on, 1, project) == 1, "a project opens: the Edit row keeps the selection");
        Check(KeepSelection(on, 6, project) == 7, "a project opens: Back to Game keeps the selection (Close Project above it)");
        Check(KeepSelection(project, 6, on) == 6, "Close Project goes while selected: Back to Game takes it");
        Check(KeepSelection(on, 9, off) == 5 && KeepSelection(on, -1, off) == 0, "out of range: clamped");
        Check(KeepSelection(on, 3, {}) == 0, "no items: 0");
    }

    // The menus' mouse and keys: a mouse resting over a row never takes the
    // selection back from Up and Down (a regression: the arrows
    // looked dead under a resting mouse).
    void TestMenuPointer()
    {
        MenuPointer p;
        p.BeginFrame(true, 300.0f, 370.0f, false);
        Check(!p.Moved() && !p.HoverShown(), "the first frame after opening: no move, no hover outline");
        for (int frame = 0; frame < 10; ++frame)
            p.BeginFrame(true, 300.0f, 370.0f, false);
        Check(!p.Moved() && !p.HoverShown(), "a mouse resting over a row: never a move");
        p.BeginFrame(true, 300.0f, 370.0f, true);
        Check(!p.Moved() && !p.HoverShown(), "Down under a resting mouse: the key wins");
        p.BeginFrame(true, 300.2f, 370.3f, false);
        Check(!p.Moved(), "a sub-pixel jitter is the mouse resting");
        p.BeginFrame(true, 320.0f, 371.0f, false);
        Check(p.Moved() && p.HoverShown(), "the mouse moves: the row under it takes the selection, its outline shows");
        p.BeginFrame(true, 320.0f, 371.0f, false);
        Check(!p.Moved() && p.HoverShown(), "it rests again: no new move, the outline stays");
        p.BeginFrame(true, 340.0f, 400.0f, true);
        Check(!p.Moved() && !p.HoverShown(), "a key in the same frame as a move: the key wins");
        p.BeginFrame(false, 0.0f, 0.0f, false);
        p.BeginFrame(true, 900.0f, 900.0f, false);
        Check(!p.Moved(), "the mouse back from nowhere (no position): no move");
        p.BeginFrame(true, 910.0f, 900.0f, false);
        Check(p.Moved(), "then it moves");
        p.Reset();
        p.BeginFrame(true, 10.0f, 10.0f, false);
        Check(!p.Moved() && !p.HoverShown(), "after Reset (the editor reopened): where the mouse is now is no move");
    }
}

int RunEditorUiQaTests()
{
    TestHideKey();
    TestHideState();
    TestTexts();
    TestStopReminder();
    TestReminderOnlyForSessionMarks();
    TestArtUploadLevels();
    TestHomeMenu();
    TestMenuPointer();
    std::printf("editor_ui_qa_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}
