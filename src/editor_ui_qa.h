#pragma once

// The editor add-on's rules that need no ReShade nor ImGui, kept pure so
// tests/editor_ui_qa_tests.cpp can pin them.
//
// Hide every bar: H hides the playback bar, the key bar and the add-on's
// notices while a clip plays, with the editor closed as well as in the
// clip editor. The replay keys are the core's (script.cpp
// OnKeyboardMessage) whenever the add-on is not interactive, so they keep
// working hidden. One key, a sound, no text on screen.
//
// The report key: the add-on says after each mark that the Report key
// (Shift+F1 by default) sends the report, counts the marks not sent yet on
// the key bar, and reminds once when a playback stops with marks waiting.
//
// The home menu: Save Replay Buffer is listed only when it can save, and the
// selection keeps its item when an item comes or goes.
//
// The menus' mouse and keys (MenuPointer): the pointer takes the selection
// only when it moves, the keys always win.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "editor_keys.h" // the report key the [Keys] table holds
#include "i18n.h"        // the notices in the player's language

namespace replay::editor::uiqa
{
    // ---------------- Hide the UI ----------------

    struct HideKeyContext
    {
        bool editScreen = false;    // the clip editor (Screen::Edit)
        bool closedScreen = false;  // the editor closed (Screen::Closed)
        bool playing = false;       // a clip or the project plays
        bool debug = false;         // Debug=1: the core's text HUD, no playback bar
        bool noteOpen = false;      // the QA note box has the keyboard
        bool editingCamera = false; // Edit Camera: the free camera's keys
    };

    // Whether H toggles the hidden UI this frame. The clip editor (never in
    // Edit Camera); the editor closed only while a clip plays with the
    // playback bar on screen.
    inline bool HideKeyApplies(const HideKeyContext& c)
    {
        if (c.noteOpen || c.editingCamera)
            return false;
        if (c.editScreen)
            return true;
        return c.closedScreen && c.playing && !c.debug;
    }

    // The hidden flag and when it comes back by itself. F2 or E (opening the
    // editor asks for its UI) call Show; the end of the playback shows the
    // UI again once nothing has played for kShowAfterStopMs (a project's
    // next clip, which opens in under that, keeps it hidden).
    struct HideState
    {
        static constexpr std::uint64_t kShowAfterStopMs = 1500;
        bool hidden = false;
        std::uint64_t stoppedAt = 0; // 0: playing (or never played)

        void Toggle() { hidden = !hidden; }
        void Show() { hidden = false; }

        void Tick(std::uint64_t now, bool playing)
        {
            if (playing)
            {
                stoppedAt = 0;
                return;
            }
            if (!stoppedAt)
                stoppedAt = now ? now : 1;
            if (now >= stoppedAt && now - stoppedAt >= kShowAfterStopMs)
                hidden = false;
        }

        // Hidden where it means something: the clip editor or the editor
        // closed, with a clip playing.
        bool HiddenOn(bool editScreen, bool closedScreen, bool playing) const
        {
            return hidden && playing && (editScreen || closedScreen);
        }
    };

    // ---------------- The painted art's mips ----------------

    // How many levels of the art's mip chain (chainLevels, level 0 the full
    // size) go to the GPU. Vulkan gets level 0 only: ReShade's Vulkan
    // update_texture_region with box == nullptr (create_resource's initial
    // data) takes its extent from level 0 for every level, so it reads
    // 2^level times past each smaller level's data (garbled mips, and a
    // crash in ReShade64.dll when the read reaches a guard page, at every
    // start and swapchain recreate). D3D12 keeps the whole chain.
    inline std::uint16_t ArtUploadLevels(bool vulkan, std::size_t chainLevels)
    {
        if (chainLevels == 0)
            return 0;
        return vulkan ? 1 : static_cast<std::uint16_t>(chainLevels);
    }

    // ---------------- The home menu ----------------

    // Save Replay Buffer is listed while the replay buffer runs and holds a
    // second at least (Snapshot: SnapshotBufferRunning, bufferHeldMs); with
    // the buffer off, or nothing in it yet, the item is not there.
    constexpr std::uint32_t kSaveBufferMinMs = 1000;
    inline bool SaveBufferShown(bool bufferRunning, std::uint32_t heldMs) { return bufferRunning && heldMs >= kSaveBufferMinMs; }

    // The selection's index in `after` (the items' ids now) for `index` in
    // `before` (the ids last frame): the same item where it is still listed,
    // else the first item after it that is (it takes the vanished one's
    // place), else the last.
    inline int KeepSelection(const std::vector<int>& before, int index, const std::vector<int>& after)
    {
        if (after.empty())
            return 0;
        if (index < 0 || index >= static_cast<int>(before.size()))
            return index < 0 ? 0 : static_cast<int>(after.size()) - 1;
        for (std::size_t from = static_cast<std::size_t>(index); from < before.size(); ++from)
            for (std::size_t n = 0; n < after.size(); ++n)
                if (after[n] == before[from])
                    return static_cast<int>(n);
        return static_cast<int>(after.size()) - 1;
    }

    // ---------------- The menus' mouse and keys ----------------

    // The mouse over a menu list, as RDR2's menus take it. The row under the
    // pointer becomes the selection only on a frame the pointer moved: a
    // mouse resting over the list never takes the selection back from the
    // keys. A navigation key always wins: its frame ignores the pointer, and
    // the grey outline of the row under the mouse stays hidden until the
    // mouse moves again (one highlight, the selection's, while the keys
    // drive). A click selects whatever the pointer did before.
    class MenuPointer
    {
    public:
        // A move smaller than this (pixels) is the mouse resting.
        static constexpr float kMoveEpsilon = 0.5f;

        // Once a frame, before the menus: the mouse (valid false: none) and
        // whether a navigation key was pressed this frame.
        void BeginFrame(bool valid, float x, float y, bool navigationKey)
        {
            m_moved = false;
            if (valid && m_have)
            {
                const float dx = x - m_x, dy = y - m_y;
                m_moved = dx > kMoveEpsilon || dx < -kMoveEpsilon || dy > kMoveEpsilon || dy < -kMoveEpsilon;
            }
            m_have = valid;
            if (valid)
            {
                m_x = x;
                m_y = y;
            }
            if (navigationKey)
            {
                m_moved = false;
                m_live = false;
            }
            else if (m_moved)
                m_live = true;
        }

        // The editor opened: where the mouse is now is no move (it came there
        // while the game had it), and the keys start with the highlight.
        void Reset()
        {
            m_have = false;
            m_moved = false;
            m_live = false;
        }

        // The pointer moved this frame (and no key was pressed): the row under
        // it takes the selection.
        bool Moved() const { return m_moved; }
        // The mouse moved since the last navigation key: the row under it may
        // show its grey outline.
        bool HoverShown() const { return m_live; }

    private:
        bool m_have = false;
        float m_x = 0.0f;
        float m_y = 0.0f;
        bool m_moved = false;
        bool m_live = false;
    };

    // ---------------- The report key ----------------

    // After a mark's note box closes: "Problem marked (3 waiting). Shift+F1
    // saves the report when you are done." `pending` is the core's count of
    // mark folders not sent yet (0: unknown, the count is left out). `keys`:
    // the [Keys] table the report key is read from.
    inline void FormatMarkNotice(char* out, std::size_t size, std::uint32_t pending, const replay::keys::Table& keys = replay::keys::Current())
    {
        if (!out || !size)
            return;
        if (pending)
            replay::i18n::CopyTo(out, size, replay::keys::Format(TR("qa.marked_waiting", "Problem marked ({count} waiting). {key_report} saves the report when you are done."),
                keys, {{"count", std::to_string(pending)}}));
        else
            replay::i18n::CopyTo(out, size, replay::keys::Fill(TR("qa.marked", "Problem marked. {key_report} saves the report when you are done."), keys));
    }

    // When a playback stops with marks waiting.
    inline void FormatStopReminder(char* out, std::size_t size, std::uint32_t pending, const replay::keys::Table& keys = replay::keys::Current())
    {
        if (!out || !size)
            return;
        if (pending == 1)
            replay::i18n::CopyTo(out, size, replay::keys::Fill(TR("qa.reminder.one", "1 problem mark not sent yet. Press {key_report} to save the report."), keys));
        else
            replay::i18n::CopyTo(out, size, replay::keys::Format(TR("qa.reminder", "{count} problem marks not sent yet. Press {key_report} to save the report."),
                keys, {{"count", std::to_string(pending)}}));
    }

    // The key bar's word for Shift+F1 with marks waiting: "Send report (3)".
    inline void FormatReportKey(char* out, std::size_t size, std::uint32_t pending)
    {
        if (!out || !size)
            return;
        replay::i18n::CopyTo(out, size, TRF("qa.send_report", "Send report ({count})", {{"count", std::to_string(pending)}}));
    }

    // The reminder when a playback stops: once per stop, after nothing has
    // played for kSettleMs (a clip opening after another, a project's next
    // clip, does not count as a stop), and only with marks waiting.
    struct StopReminder
    {
        static constexpr std::uint64_t kSettleMs = 1000;
        bool wasPlaying = false;
        bool armed = false;
        std::uint64_t stoppedAt = 0;

        // True once, the frame the reminder is due.
        bool Update(std::uint64_t now, bool playing, std::uint32_t pending)
        {
            if (playing)
            {
                wasPlaying = true;
                armed = false;
                return false;
            }
            if (wasPlaying)
            {
                wasPlaying = false;
                armed = true;
                stoppedAt = now;
            }
            if (!armed || now < stoppedAt || now - stoppedAt < kSettleMs)
                return false;
            armed = false;
            return pending > 0;
        }
    };
}
