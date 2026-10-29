#pragma once

// The closed theater's safe exit. While the replay owns the world
// (world_theatre.h) the script threads are paused, the persistent characters
// held, the ambient spawn denied and the open world music off; the normal way
// out is the save reload (world_model.h). A replay stopped half way could
// leave the game without ground, scripts paused, persistent characters held
// and no reload. Here every way out that is not the normal one ends in the
// same place: the world handed back (release) and the reload pushed, or, when
// the reload cannot run, at least the release and a notice.
//
// The watchdog runs every tick on the script thread with what the tick
// sees; it is pure (no engine access) and unit tested in ReplayCoreTests
// (theatre_exit_tests.cpp). script.cpp acts on its verdicts. The marker
// file carries the ownership across a hot reload or an unload (DllMain
// cannot run natives): the next copy in the same process reads it and
// repairs the world.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace replay::theatre::safeexit
{
    // Out of the replay (nothing plays, no editor, no clip opening) with the
    // world cleared and no reload for this long: the normal exit (1 s,
    // world_model.h kExitSettleMs) did not run.
    constexpr std::uint64_t kIdleStuckMs = 8000;
    // A clip or project opening, or the exit's load wait, counts as the
    // replay only this long (a load that never ends).
    constexpr std::uint64_t kOpenTimeoutMs = 60000;
    // After the push: no loading screen within this, the reload did not start.
    constexpr std::uint64_t kReloadStartMs = 15000;
    // A refused or failed reload is tried again once, this long after.
    constexpr std::uint64_t kRetryAfterMs = 5000;
    constexpr std::uint32_t kMaxRetries = 1;
    // ScriptHook's reload key: the world is handed back at once (the unload
    // can come before another tick); a key that unloaded nothing takes the
    // world back after this.
    constexpr std::uint64_t kReloadKeyMs = 5000;

    enum class Cause : std::uint8_t
    {
        None,
        Idle,             // out of the replay, no reload
        OpenTimeout,      // a clip, project or exit load wait that never ends
        PlayerDead,
        Exception,        // a C++ exception left a tick
        ReloadKey,        // Ctrl+R (hot reload) with the world owned
        ReloadFailed,     // refused before the push, or no restart after it
        ReloadNotStarted, // pushed, no loading screen
        PreviousCopy,     // the marker of a copy unloaded with the world owned
    };

    inline const char* CauseName(Cause cause)
    {
        switch (cause)
        {
        case Cause::None: return "none";
        case Cause::Idle: return "idle";
        case Cause::OpenTimeout: return "open_timeout";
        case Cause::PlayerDead: return "player_dead";
        case Cause::Exception: return "exception";
        case Cause::ReloadKey: return "reload_key";
        case Cause::ReloadFailed: return "reload_failed";
        case Cause::ReloadNotStarted: return "reload_not_started";
        case Cause::PreviousCopy: return "previous_copy";
        }
        return "?";
    }

    enum class Action : std::uint8_t
    {
        None,
        // Hand the world back (scripts run, persistent characters resumed,
        // spawn funnel open, music flag off), the world stays cleared.
        Release,
        // Stop the replay (playback, editor screens, clip loads), release,
        // and push the reload now (not after the exit's settle).
        ReleaseAndReload,
        // The reload key unloaded nothing and the replay goes on: own the
        // world again.
        Retake,
        // Push the reload again (once).
        RetryReload,
        // No reload possible: released, the screen back, a notice to load
        // the last save from the pause menu.
        GiveUp,
    };

    inline const char* ActionName(Action action)
    {
        switch (action)
        {
        case Action::None: return "none";
        case Action::Release: return "release";
        case Action::ReleaseAndReload: return "release_and_reload";
        case Action::Retake: return "retake";
        case Action::RetryReload: return "retry_reload";
        case Action::GiveUp: return "give_up";
        }
        return "?";
    }

    // What the tick sees.
    struct Tick
    {
        std::uint64_t nowMs = 0;
        bool cleared = false;       // the world was cleared for the replay (g_world.cleared)
        bool owning = false;        // the theater owns it now (scripts paused, ...)
        bool reloadStarted = false; // pushed, or given up (world_model.h)
        bool watching = false;      // a push in flight
        std::uint64_t pushedAtMs = 0;
        bool reloadFailed = false;  // the last push attempt failed (WorldReloadFailed)
        std::uint64_t reloadFailedAtMs = 0;
        bool loadingScreen = false;
        bool playing = false;
        bool editorOpen = false;
        bool recording = false;
        bool clipOpening = false;   // a clip or project read, a clip load wait
        bool exitFading = false;    // the exit's load wait
        bool playerDead = false;
        bool exception = false;     // an exception left the last tick
        bool reloadKey = false;     // Ctrl+R within kReloadKeyMs
    };

    struct Verdict
    {
        Action action = Action::None;
        Cause cause = Cause::None;
    };

    class Watchdog
    {
    public:
        Verdict Update(const Tick& t)
        {
            if (!t.cleared)
            {
                *this = Watchdog{};
                return {};
            }
            // The last reload attempt.
            if (t.watching)
            {
                if (!m_watchingSeen || t.pushedAtMs != m_pushedAtMs)
                {
                    m_watchingSeen = true;
                    m_pushedAtMs = t.pushedAtMs;
                    m_loadingSeen = false;
                    m_notStartedSaid = false;
                }
                m_loadingSeen = m_loadingSeen || t.loadingScreen;
            }
            else
                m_watchingSeen = false;

            // 1. An exception or the player's death with the world cleared:
            // out now (once per reload attempt).
            if ((t.exception || t.playerDead) && !t.reloadStarted && !m_stopping)
            {
                m_stopping = true;
                return {Action::ReleaseAndReload, t.exception ? Cause::Exception : Cause::PlayerDead};
            }
            // 2. The reload key: hand the world back before the unload; a key
            // that unloaded nothing takes it back while the replay goes on.
            if (t.reloadKey && t.owning && !m_keyReleased && !t.reloadStarted)
            {
                m_keyReleased = true;
                m_keyAtMs = t.nowMs;
                return {Action::Release, Cause::ReloadKey};
            }
            if (m_keyReleased)
            {
                if (t.nowMs - m_keyAtMs < kReloadKeyMs)
                    return {};
                m_keyReleased = false;
                if (!t.reloadStarted && InReplay(t) && !t.owning)
                    return {Action::Retake, Cause::ReloadKey};
            }
            // 3. A push in flight: the loading screen must show.
            if (t.watching)
            {
                if (!m_loadingSeen && t.nowMs - t.pushedAtMs >= kReloadStartMs && !m_notStartedSaid)
                {
                    m_notStartedSaid = true;
                    return Retry(t, Cause::ReloadNotStarted);
                }
                return {};
            }
            // 4. A failed attempt (refused before the push, or no restart).
            if (t.reloadStarted)
            {
                if (!t.reloadFailed || m_gaveUp)
                    return t.owning ? Verdict{Action::Release, Cause::ReloadFailed} : Verdict{};
                if (m_retries < kMaxRetries && t.nowMs - t.reloadFailedAtMs < kRetryAfterMs)
                    return t.owning ? Verdict{Action::Release, Cause::ReloadFailed} : Verdict{};
                return Retry(t, Cause::ReloadFailed);
            }
            // 5. Out of the replay with no reload, or a load that never ends.
            const bool opening = t.clipOpening || t.exitFading;
            if (opening && !m_opening)
                m_openingSinceMs = t.nowMs;
            m_opening = opening;
            const bool openTimedOut = opening && t.nowMs - m_openingSinceMs >= kOpenTimeoutMs;
            if (openTimedOut && !t.playing && !t.recording && !m_stopping)
            {
                m_stopping = true;
                return {Action::ReleaseAndReload, Cause::OpenTimeout};
            }
            // Playing or recording with a clip load wait (a seek settle) is
            // the replay however long it takes; a load alone is, until it
            // times out.
            const bool active = t.playing || t.recording || t.editorOpen || (opening && !openTimedOut);
            if (active)
            {
                m_idle = false;
                return {};
            }
            if (!m_idle)
            {
                m_idle = true;
                m_idleSinceMs = t.nowMs;
            }
            if (t.nowMs - m_idleSinceMs >= kIdleStuckMs && !m_stopping)
            {
                m_stopping = true;
                return {Action::ReleaseAndReload, Cause::Idle};
            }
            return {};
        }

        std::uint32_t Retries() const { return m_retries; }
        bool GaveUp() const { return m_gaveUp; }

    private:
        static bool InReplay(const Tick& t)
        {
            return t.playing || t.editorOpen || t.recording || t.clipOpening || t.exitFading;
        }

        Verdict Retry(const Tick& t, Cause cause)
        {
            (void)t;
            if (m_retries >= kMaxRetries)
            {
                m_gaveUp = true;
                return {Action::GiveUp, cause};
            }
            ++m_retries;
            m_stopping = false;
            return {Action::RetryReload, cause};
        }

        bool m_idle = false;
        std::uint64_t m_idleSinceMs = 0;
        bool m_opening = false;
        std::uint64_t m_openingSinceMs = 0;
        bool m_stopping = false; // a ReleaseAndReload was given: the reload decides now
        bool m_keyReleased = false;
        std::uint64_t m_keyAtMs = 0;
        bool m_watchingSeen = false;
        std::uint64_t m_pushedAtMs = 0;
        bool m_loadingSeen = false;
        bool m_notStartedSaid = false;
        std::uint32_t m_retries = 0;
        bool m_gaveUp = false;
    };

    // ---- the marker file (RedemptionEditor.theatre) ------------------------

    // One line: pid=<process>,owning=<0|1>,pushed=<0|1>,slot=<n>. Written
    // when the world is cleared, rewritten on every change of ownership and
    // on the push, removed when the save's world is back (the session reset
    // of the reload) or the world was never cleared.
    struct Marker
    {
        std::uint32_t pid = 0;
        bool owning = false;
        bool pushed = false;
        int slot = -1;
    };

    inline std::string FormatMarker(const Marker& marker)
    {
        char line[96]{};
        std::snprintf(line, sizeof(line), "pid=%u,owning=%d,pushed=%d,slot=%d\n", marker.pid, marker.owning ? 1 : 0,
            marker.pushed ? 1 : 0, marker.slot);
        return line;
    }

    inline bool ParseMarker(const char* text, Marker& out)
    {
        if (!text)
            return false;
        // key=value pairs in this order (no sscanf: the build treats its
        // deprecation warning as an error).
        const char* keys[4] = {"pid=", ",owning=", ",pushed=", ",slot="};
        long values[4] = {};
        const char* at = text;
        for (int n = 0; n < 4; ++n)
        {
            const std::size_t length = std::strlen(keys[n]);
            if (std::strncmp(at, keys[n], length) != 0)
                return false;
            at += length;
            char* end = nullptr;
            values[n] = std::strtol(at, &end, 10);
            if (end == at)
                return false;
            at = end;
        }
        const unsigned pid = values[0] > 0 ? static_cast<unsigned>(values[0]) : 0u;
        const int owning = static_cast<int>(values[1]), pushed = static_cast<int>(values[2]), slot = static_cast<int>(values[3]);
        if (pid == 0)
            return false;
        out.pid = pid;
        out.owning = owning != 0;
        out.pushed = pushed != 0;
        out.slot = slot;
        return true;
    }

    enum class Startup : std::uint8_t
    {
        None,    // no marker
        Stale,   // another process wrote it (the game restarted: a fresh world); delete
        Broken,  // unreadable; delete
        Recover, // this process: a copy unloaded with the world cleared
    };

    inline const char* StartupName(Startup startup)
    {
        switch (startup)
        {
        case Startup::None: return "none";
        case Startup::Stale: return "stale";
        case Startup::Broken: return "broken";
        case Startup::Recover: return "recover";
        }
        return "?";
    }

    inline Startup DecideStartup(bool present, bool parsed, const Marker& marker, std::uint32_t pid)
    {
        if (!present)
            return Startup::None;
        if (!parsed)
            return Startup::Broken;
        return marker.pid == pid ? Startup::Recover : Startup::Stale;
    }

    // ---- the entry (script.cpp WorldModelEnter) ----------------------------

    // The entry clears the world in steps with frames in between (the
    // scripts' cleanup handshake waits up to 5 s, then the persistent
    // characters, CLEAR_AREA, the engine's clear and the trains). The world
    // counts as cleared from before the first of them: this marker is written
    // and the session marked cleared then, so a hot reload during those
    // frames leaves the next copy a world to restore (it recovers and pushes
    // the reload: owning, not pushed), and an exception half way reaches the
    // watchdog with the world cleared (release and reload) instead of a world
    // half cleared that nothing restores.
    inline Marker EntryMarker(std::uint32_t pid, int slot)
    {
        Marker marker;
        marker.pid = pid;
        marker.owning = true;
        marker.pushed = false;
        marker.slot = slot;
        return marker;
    }

    // What a release must undo itself when the entry stopped before the
    // theater owned the world (EndTheatre only ends an owning theater).
    enum class PartialEntry : std::uint8_t
    {
        None,            // no entry in progress, or it got as far as owning: EndTheatre does it
        Forget,          // nothing held yet: only the entry's state goes
        ResumePerschar,  // the persistent characters are held: resume them (the load never ends otherwise)
    };

    inline PartialEntry UndoPartialEntry(bool entering, bool owning, bool perscharHeld)
    {
        if (!entering || owning)
            return PartialEntry::None;
        return perscharHeld ? PartialEntry::ResumePerschar : PartialEntry::Forget;
    }
}
