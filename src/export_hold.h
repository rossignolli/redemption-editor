#pragma once

// Render export: what the export does when the game shows something that is
// not the replay scene.
//
// A screen that waits for the player (the quit to desktop alert, the pause
// menu) or a close request on the game window CANCELS the export at once,
// keeping the finished segments (as the Cancel button does): the game timer
// goes back to the wall clock and the game runs, so the alert is drawn and
// can be answered, and "export again with the same settings" continues from
// the last good segment. The segment in progress is never closed, so no
// picture of the alert ends in a video. (Holding instead was tried: with the
// timer at the engine's minimum step the frontend never drew the alert and
// nothing answered it.)
//
// A screen that needs no answer (a loading screen) HOLDS the export: the
// probe's clock does not advance, the playhead is held, and the ticks it
// writes are Hold ticks (export_schedule.h Phase::Hold) that the add-on
// never encodes. When the screen has been gone for a while, a short resync
// follows (tagged Hold ticks: the add-on checks its present count against
// the tick serials again, export_channel.h) and the clock goes on from the
// frame and sample it stood at: the frame count is the same as without the
// hold.
//
// Signals (read by the probe on the script fiber, src/script_transport.cpp):
//   menu     HUD::IS_PAUSE_MENU_ACTIVE. Public native; its documentation
//            says it covers the pause menu and the frontend menus, and the
//            recording gate (record_gate.h) already stops on it. Cancels.
//   alert    UISTICKYFEED::_UI_STICKY_FEED_IS_ALERT_SCREEN_ACTIVE. Public
//            native (community native database); seen in game on for the quit
//            to desktop alert (RENDER_EXPORT,phase=held,reason=alert after a
//            WM_CLOSE). Cancels.
//   close    the add-on saw the game window asked to close (WM_CLOSE sent or
//            posted, WM_SYSCOMMAND SC_CLOSE, Alt+F4) through hooks on the
//            window's thread, before the game handles it. Cancels.
//   loading  DLC::GET_IS_LOADING_SCREEN_ACTIVE or IS_LOADING_SCREEN_VISIBLE
//            (public natives, as the recording gate). Holds.
//   paused   GET_FRAME_TIME is 0 while the export timer gives the frame a
//            positive step: the game itself is paused. Unverified, so it
//            only keeps a hold a loading screen started, never starts one.
//
// Pure (unit tested in ReplayCoreTests, tests/export_hold_tests.cpp).

#include "export_schedule.h"

#include <cstdint>
#include <string>

namespace replay::exporthold
{
    enum Signal : std::uint32_t
    {
        SignalMenu = 1u << 0,
        SignalAlert = 1u << 1,
        SignalLoading = 1u << 2,
        SignalPaused = 1u << 3, // keeps a hold, never starts one
        SignalClose = 1u << 4   // edge: a close request since the last tick
    };
    // A screen waiting for the player, or a close request: the export is cancelled.
    constexpr std::uint32_t kCancelSignals = SignalMenu | SignalAlert | SignalClose;
    // The screens read from natives (src/script_transport.cpp).
    constexpr std::uint32_t kScreenSignals = SignalMenu | SignalAlert | SignalLoading;
    // A screen that needs no answer: the export holds.
    constexpr std::uint32_t kHoldSignals = SignalLoading;

    // "menu+alert": the signal bits by name (logs).
    inline std::string SignalNames(std::uint32_t bits)
    {
        static const char* const names[] = {"menu", "alert", "loading", "paused", "close"};
        std::string text;
        for (std::uint32_t i = 0; i < 5; ++i)
        {
            if (!(bits & (1u << i)))
                continue;
            if (!text.empty())
                text += '+';
            text += names[i];
        }
        return text.empty() ? std::string("none") : text;
    }

    // The cancel's reason for the log, or null: the export goes on.
    inline const char* CancelReason(std::uint32_t signals)
    {
        if (signals & SignalClose)
            return "window_close";
        if (signals & SignalAlert)
            return "game_alert";
        if (signals & SignalMenu)
            return "game_menu";
        return nullptr;
    }

    enum class Phase : std::uint8_t
    {
        Run = 0,    // the clock runs
        Held = 1,   // a signal is on (or went off less than releaseTicks ago)
        Resync = 2  // the screen is back; tagged Hold ticks until the add-on is in step again
    };

    struct Config
    {
        std::uint32_t releaseTicks = 10;          // every signal off this many ticks before the resync
        std::uint32_t resyncMinTicks = 8;         // at least this many resync ticks (the menu's fade out)
        std::uint32_t resyncMaxTicks = 240;       // no acknowledgement by then: go on anyway (logged)
    };

    struct Decision
    {
        bool hold = false;          // this tick is a Hold tick
        bool started = false;       // the first tick of a hold
        bool resyncStarted = false; // the first resync tick
        bool releasing = false;     // the last Hold tick: the next tick runs the clock
        bool timedOut = false;      // releasing without the add-on's acknowledgement
        Phase phase = Phase::Run;   // this tick's phase
        std::uint32_t signals = 0;  // the signals that held this tick
    };

    class Machine
    {
    public:
        Machine() = default;
        explicit Machine(const Config& config) : m_config(config) {}

        void Reset()
        {
            m_phase = Phase::Run;
            m_offTicks = 0;
            m_resyncTicks = 0;
            m_holdTicks = 0;
            m_reasons = 0;
        }

        // One tick. `signals`: Signal bits (the cancel signals are the
        // probe's, before this). `acknowledged`: the add-on finished this
        // hold's resync.
        Decision Update(std::uint32_t signals, bool acknowledged)
        {
            const std::uint32_t starters = signals & kHoldSignals;
            const std::uint32_t keepers = starters | (signals & SignalPaused);
            Decision d;
            switch (m_phase)
            {
            case Phase::Run:
                if (!starters)
                    return d;
                m_phase = Phase::Held;
                m_offTicks = 0;
                m_resyncTicks = 0;
                m_holdTicks = 0;
                m_reasons = 0;
                d.started = true;
                d.phase = Phase::Held;
                break;
            case Phase::Held:
                d.phase = Phase::Held;
                if (keepers)
                {
                    m_offTicks = 0;
                }
                else if (++m_offTicks >= m_config.releaseTicks)
                {
                    m_phase = Phase::Resync;
                    m_resyncTicks = 0;
                    d.resyncStarted = true;
                    d.phase = Phase::Resync;
                }
                break;
            case Phase::Resync:
                d.phase = Phase::Resync;
                if (keepers)
                {
                    // The screen came back before the resync ended: held again.
                    m_phase = Phase::Held;
                    m_offTicks = 0;
                    d.phase = Phase::Held;
                    break;
                }
                break;
            }
            if (m_phase == Phase::Resync)
            {
                ++m_resyncTicks;
                const bool ready = acknowledged && m_resyncTicks >= m_config.resyncMinTicks;
                if (ready || m_resyncTicks >= m_config.resyncMaxTicks)
                {
                    d.releasing = true;
                    d.timedOut = !ready;
                    m_phase = Phase::Run;
                }
            }
            m_reasons |= keepers;
            ++m_holdTicks;
            d.hold = true;
            d.signals = keepers;
            return d;
        }

        Phase CurrentPhase() const { return m_phase; }
        std::uint32_t Reasons() const { return m_reasons; }       // every signal of this (or the last) hold
        std::uint64_t HoldTicks() const { return m_holdTicks; }   // its Hold ticks
        std::uint32_t ResyncTicks() const { return m_resyncTicks; }

    private:
        Config m_config;
        Phase m_phase = Phase::Run;
        std::uint32_t m_offTicks = 0;
        std::uint32_t m_resyncTicks = 0;
        std::uint64_t m_holdTicks = 0;
        std::uint32_t m_reasons = 0;
    };

    // The game timer's step during Hold ticks: the engine's minimum (the
    // world moves as little as it can under a loading screen).
    inline std::int64_t HoldStepNs(std::int64_t clampMinNs)
    {
        return clampMinNs > 0 ? clampMinNs : 1000000;
    }

    // ------------------------------------------------------------------
    // Add-on side: the resync. During a hold the game may present more or
    // fewer pictures than the probe ticks, so the add-on's present count can
    // slip against the serials. The tags of the resync ticks give it back: the
    // full serial of a tag (its low kTagBits bits) is the newest written serial
    // with those bits, and the count is moved so that the tagged present maps
    // to that serial at the latency measured in the preroll.

    inline std::uint64_t FullSerialOfTag(std::uint64_t newestSerial, std::uint32_t tag)
    {
        constexpr std::uint64_t mask = (1ull << replay::exporting::kTagBits) - 1ull;
        return newestSerial - ((newestSerial - tag) & mask);
    }

    // The correction to add to the present count.
    inline std::int64_t RebaseDelta(std::uint64_t tagPresent, std::uint64_t serial, std::uint32_t latency)
    {
        return static_cast<std::int64_t>(serial + latency) - static_cast<std::int64_t>(tagPresent);
    }

    // Three tags in a row agreeing on the correction (a tag half covered by a
    // menu fading out reads wrong once).
    struct RebaseVote
    {
        static constexpr std::uint32_t kVotes = 3;
        std::int64_t candidate = 0;
        std::uint32_t votes = 0;

        bool Add(std::int64_t delta)
        {
            votes = votes && delta == candidate ? votes + 1 : 1;
            candidate = delta;
            return votes >= kVotes;
        }
        void Reset()
        {
            candidate = 0;
            votes = 0;
        }
    };

    // Presents the add-on spends in the resync without three agreeing tags
    // (tags not readable: a 16 bit back buffer) before it acknowledges anyway,
    // its count unchanged. Below the probe's resyncMaxTicks.
    constexpr std::uint32_t kAddonResyncMaxPresents = 90;
}
