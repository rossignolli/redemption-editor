// The closed theater's safe exit as a state machine. Every way out of a replay
// that is not the normal one (Stop, F7 and the exit's 1 s settle, world_model.h
// DecideExit) ends with the world handed back and the reload pushed, or a
// notice.
#include "theatre_exit.h"

#include <cstdio>
#include <cstring>

namespace
{
    using namespace replay::theatre::safeexit;

    int g_exitFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_exitFailures;
    }

    // The world cleared, the replay playing, the theater owning.
    Tick Playing(std::uint64_t now)
    {
        Tick t;
        t.nowMs = now;
        t.cleared = true;
        t.owning = true;
        t.playing = true;
        return t;
    }

    bool Is(const Verdict& v, Action action, Cause cause)
    {
        return v.action == action && v.cause == cause;
    }

    void TestNormalReplay()
    {
        Watchdog dog;
        Tick t;
        t.nowMs = 1000;
        Check(Is(dog.Update(t), Action::None, Cause::None), "world not cleared: nothing");
        for (std::uint64_t now = 0; now < 120000; now += 1000)
            Check(Is(dog.Update(Playing(now)), Action::None, Cause::None), "playing a long time: nothing");
        // The editor's gallery between two clips, an hour.
        Tick gallery = Playing(0);
        gallery.playing = false;
        gallery.editorOpen = true;
        for (std::uint64_t now = 0; now < 3600000; now += 5000)
        {
            gallery.nowMs = now;
            Check(Is(dog.Update(gallery), Action::None, Cause::None), "editor open: nothing");
        }
        // The normal exit: out of the replay, the push 1 s later.
        Tick out = gallery;
        out.editorOpen = false;
        out.nowMs = 3600000;
        Check(Is(dog.Update(out), Action::None, Cause::None), "just out: the normal exit's settle");
        out.nowMs += 1000;
        out.owning = false; // EndTheatre("reload") before the push
        out.reloadStarted = true;
        out.watching = true;
        out.pushedAtMs = out.nowMs;
        Check(Is(dog.Update(out), Action::None, Cause::None), "pushed: nothing");
        out.nowMs += 3000;
        out.loadingScreen = true;
        Check(Is(dog.Update(out), Action::None, Cause::None), "loading: nothing");
        out.nowMs += 40000;
        Check(Is(dog.Update(out), Action::None, Cause::None), "a long load with its screen: nothing");
    }

    void TestIdleStuck()
    {
        // The replay stopped, the world cleared, no reload.
        Watchdog dog;
        dog.Update(Playing(0));
        Tick stuck = Playing(1000);
        stuck.playing = false;
        Check(Is(dog.Update(stuck), Action::None, Cause::None), "stopped: the normal exit gets its chance");
        stuck.nowMs = 1000 + kIdleStuckMs - 1;
        Check(Is(dog.Update(stuck), Action::None, Cause::None), "stopped: not yet");
        stuck.nowMs = 1000 + kIdleStuckMs;
        Check(Is(dog.Update(stuck), Action::ReleaseAndReload, Cause::Idle), "stopped 8 s with no reload: out and reload");
        stuck.nowMs += 1000;
        Check(Is(dog.Update(stuck), Action::None, Cause::None), "given once: the reload decides now");
        // Back into the replay and out again: a new idle span (not a repeat).
        Watchdog again;
        Tick a = Playing(0);
        again.Update(a);
        a.playing = false;
        a.nowMs = 5000;
        again.Update(a);
        a.playing = true;
        a.nowMs = 6000;
        again.Update(a);
        a.playing = false;
        a.nowMs = 7000;
        Check(Is(again.Update(a), Action::None, Cause::None), "the idle span restarts");
        a.nowMs = 7000 + kIdleStuckMs;
        Check(Is(again.Update(a), Action::ReleaseAndReload, Cause::Idle), "and fires 8 s after the last exit");
    }

    void TestErrors()
    {
        Watchdog dog;
        Tick t = Playing(100);
        t.exception = true;
        Check(Is(dog.Update(t), Action::ReleaseAndReload, Cause::Exception), "exception in a tick: out and reload");
        t.nowMs = 200;
        Check(Is(dog.Update(t), Action::None, Cause::None), "repeated exceptions: once");

        Watchdog dead;
        Tick d = Playing(100);
        d.playerDead = true;
        Check(Is(dead.Update(d), Action::ReleaseAndReload, Cause::PlayerDead), "player died: out and reload");
        Watchdog loading;
        Tick l = Playing(0);
        l.playerDead = true;
        l.reloadStarted = true;
        l.watching = true;
        l.owning = false;
        l.playing = false;
        Check(Is(loading.Update(l), Action::None, Cause::None), "dead during the reload: nothing");
    }

    void TestOpenTimeout()
    {
        Watchdog dog;
        Tick t = Playing(0);
        t.playing = false;
        t.clipOpening = true;
        for (std::uint64_t now = 0; now < kOpenTimeoutMs; now += 1000)
        {
            t.nowMs = now;
            Check(Is(dog.Update(t), Action::None, Cause::None), "a clip opening: the replay");
        }
        t.nowMs = kOpenTimeoutMs;
        Check(Is(dog.Update(t), Action::ReleaseAndReload, Cause::OpenTimeout), "a load that never ends: out and reload");
        // The exit's load wait the same.
        Watchdog fade;
        Tick f = Playing(0);
        f.playing = false;
        f.exitFading = true;
        fade.Update(f);
        f.nowMs = kOpenTimeoutMs;
        Check(Is(fade.Update(f), Action::ReleaseAndReload, Cause::OpenTimeout), "exit wait that never ends");
        // Playing with a clip load wait (a seek settle) is never cut.
        Watchdog seek;
        Tick s = Playing(0);
        s.clipOpening = true;
        seek.Update(s);
        s.nowMs = 10 * kOpenTimeoutMs;
        Check(Is(seek.Update(s), Action::None, Cause::None), "playing: a long load wait is the replay's");
    }

    void TestReloadKey()
    {
        // Ctrl+R with the world owned: handed back at once.
        Watchdog dog;
        Tick t = Playing(0);
        t.reloadKey = true;
        Check(Is(dog.Update(t), Action::Release, Cause::ReloadKey), "reload key: release now");
        t.owning = false;
        t.nowMs = 1000;
        Check(Is(dog.Update(t), Action::None, Cause::None), "reload key: waits for the unload");
        // Nothing unloaded and the replay goes on: retake.
        t.reloadKey = false;
        t.nowMs = kReloadKeyMs;
        Check(Is(dog.Update(t), Action::Retake, Cause::ReloadKey), "key reloaded nothing: retake");
        // Nothing unloaded and the replay is over: the normal exit and the idle rule.
        Watchdog over;
        Tick o = Playing(0);
        o.reloadKey = true;
        over.Update(o);
        o.owning = false;
        o.playing = false;
        o.reloadKey = false;
        o.nowMs = kReloadKeyMs;
        Check(Is(over.Update(o), Action::None, Cause::None), "key, replay over: no retake");
        o.nowMs = kReloadKeyMs + kIdleStuckMs;
        Check(Is(over.Update(o), Action::ReleaseAndReload, Cause::Idle), "then the idle rule");
    }

    void TestReloadFailures()
    {
        // Refused before the push (the menu busy): released at once, one
        // retry 5 s later, then give up.
        Watchdog dog;
        Tick t;
        t.cleared = true;
        t.owning = true; // a failed world reload can leave it owning
        t.reloadStarted = true;
        t.reloadFailed = true;
        t.reloadFailedAtMs = 1000;
        t.nowMs = 1000;
        Check(Is(dog.Update(t), Action::Release, Cause::ReloadFailed), "refused while owning: release");
        t.owning = false;
        t.nowMs = 2000;
        Check(Is(dog.Update(t), Action::None, Cause::None), "released: wait for the retry");
        t.nowMs = 1000 + kRetryAfterMs;
        Check(Is(dog.Update(t), Action::RetryReload, Cause::ReloadFailed), "retry once");
        Check(dog.Retries() == 1, "one retry counted");
        // The retry fails too.
        t.reloadFailedAtMs = t.nowMs + 100;
        t.nowMs += 100;
        Check(Is(dog.Update(t), Action::GiveUp, Cause::ReloadFailed), "second failure: give up");
        Check(dog.GaveUp(), "gave up");
        t.nowMs += 100000;
        Check(Is(dog.Update(t), Action::None, Cause::None), "given up: quiet (the notice was shown)");

        // Pushed, no loading screen: retry, then give up.
        Watchdog push;
        Tick p;
        p.cleared = true;
        p.reloadStarted = true;
        p.watching = true;
        p.pushedAtMs = 10000;
        p.nowMs = 10000;
        Check(Is(push.Update(p), Action::None, Cause::None), "pushed");
        p.nowMs = 10000 + kReloadStartMs;
        Check(Is(push.Update(p), Action::RetryReload, Cause::ReloadNotStarted), "no loading screen in 15 s: retry");
        p.pushedAtMs = p.nowMs + 50;
        p.nowMs += 50;
        Check(Is(push.Update(p), Action::None, Cause::None), "second push");
        p.nowMs += kReloadStartMs;
        Check(Is(push.Update(p), Action::GiveUp, Cause::ReloadNotStarted), "second push silent: give up");

        // A manual step 2 (reloadStarted, no failure) with the theater owning: released.
        Watchdog manual;
        Tick m;
        m.cleared = true;
        m.owning = true;
        m.reloadStarted = true;
        Check(Is(manual.Update(m), Action::Release, Cause::ReloadFailed), "WorldModelStep=2 with the world owned: release");
    }

    void TestWorldBack()
    {
        Watchdog dog;
        dog.Update(Playing(0));
        Tick t = Playing(1000);
        t.exception = true;
        dog.Update(t);
        Tick back;
        back.nowMs = 2000;
        Check(Is(dog.Update(back), Action::None, Cause::None), "the save's world back: reset");
        Check(dog.Retries() == 0 && !dog.GaveUp(), "reset clears the retries");
    }

    void TestMarker()
    {
        Marker marker;
        marker.pid = 4242;
        marker.owning = true;
        marker.pushed = false;
        marker.slot = 7;
        const std::string line = FormatMarker(marker);
        Check(line == "pid=4242,owning=1,pushed=0,slot=7\n", "marker: one line");
        Marker read;
        Check(ParseMarker(line.c_str(), read) && read.pid == 4242 && read.owning && !read.pushed && read.slot == 7, "marker: read back");
        Check(!ParseMarker("garbage", read), "marker: garbage");
        Check(!ParseMarker(nullptr, read), "marker: nothing");
        Check(!ParseMarker("pid=0,owning=1,pushed=0,slot=1", read), "marker: no pid");
        Check(DecideStartup(false, false, read, 4242) == Startup::None, "startup: no marker");
        Check(DecideStartup(true, false, read, 4242) == Startup::Broken, "startup: unreadable");
        Check(DecideStartup(true, true, marker, 4242) == Startup::Recover, "startup: this process, a copy unloaded in the replay");
        Check(DecideStartup(true, true, marker, 99) == Startup::Stale, "startup: another process (the game restarted)");
        Check(std::strcmp(StartupName(Startup::Recover), "recover") == 0 && std::strcmp(CauseName(Cause::PreviousCopy), "previous_copy") == 0 &&
                std::strcmp(ActionName(Action::ReleaseAndReload), "release_and_reload") == 0,
            "names");
    }
}

namespace
{
    // An entry stopped half way (a hot reload in the scripts' cleanup wait, an
    // exception between CLEAR_AREA and the ownership) restores the world like a
    // full one.
    void TestPartialEntry()
    {
        const Marker entry = EntryMarker(4242, 9);
        Check(entry.owning && !entry.pushed && entry.slot == 9, "entry marker: owning, not pushed, the newest slot");
        Marker read;
        Check(ParseMarker(FormatMarker(entry).c_str(), read) && DecideStartup(true, true, read, 4242) == Startup::Recover && !read.pushed,
            "entry marker: the next copy in the process recovers and pushes the reload");

        // The exception reached the watchdog with the world marked cleared and
        // the theater not owning yet.
        Watchdog dog;
        Tick t;
        t.nowMs = 100;
        t.cleared = true;
        t.owning = false;
        t.playing = true;
        t.exception = true;
        Check(Is(dog.Update(t), Action::ReleaseAndReload, Cause::Exception), "partial entry, exception: out and reload");
        // Without the early mark the session was not cleared and the
        // watchdog had nothing to do (the old behaviour).
        Watchdog before;
        t.cleared = false;
        Check(Is(before.Update(t), Action::None, Cause::None), "a world not marked cleared is never restored");

        Check(UndoPartialEntry(false, false, false) == PartialEntry::None, "no entry: nothing to undo");
        Check(UndoPartialEntry(true, true, true) == PartialEntry::None, "the entry reached ownership: EndTheatre undoes it");
        Check(UndoPartialEntry(true, false, false) == PartialEntry::Forget, "stopped before the hold: forget the entry");
        Check(UndoPartialEntry(true, false, true) == PartialEntry::ResumePerschar, "stopped after the hold: resume the persistent characters");
    }
}

int RunTheatreExitTests()
{
    TestPartialEntry();
    TestNormalReplay();
    TestIdleStuck();
    TestErrors();
    TestOpenTimeout();
    TestReloadKey();
    TestReloadFailures();
    TestWorldBack();
    TestMarker();
    if (!g_exitFailures)
        std::printf("theatre exit tests passed\n");
    return g_exitFailures;
}
