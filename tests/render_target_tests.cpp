// Named render targets and UI apps as state events (render_target_core.h). The
// state at a playhead, the playback reconcile (seek back and forward, nothing
// done twice, the app only while a target is alive), the live state that
// becomes the initial events, the poll of the UI apps with the launch entry,
// the window cut and the ini list.
#include "render_target_core.h"

#include <cstdio>
#include <cstring>

namespace
{
    using namespace replay::rendertarget;

    int g_rtFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_rtFailures;
    }

    constexpr std::uint32_t kLedger = 0x490CC7C7u; // S_CAMPLEDGER01X
    const std::uint32_t kShop = Joaat("SHOP_BROWSING");
    constexpr std::uint32_t kEntry = static_cast<std::uint32_t>(-702860656); // camp_clemenspoint.c

    Event Make(std::uint32_t t, Kind kind, const char* name, std::uint32_t hash = 0, std::uint32_t entry = 0)
    {
        Event e;
        e.timeMs = t;
        e.kind = kind;
        e.SetName(name);
        e.hash = hash;
        e.entry = entry;
        return e;
    }

    // The ledger as the camp script does it: register, link, the app open,
    // two page flips (nothing recorded), the app closed, the target released.
    Track Ledger()
    {
        Track track;
        track.events.push_back(Make(8400, kRegister, "ledger_ct"));
        track.events.push_back(Make(8400, kLink, "ledger_ct", kLedger));
        track.events.push_back(Make(8500, kAppOpen, "", kShop, kEntry));
        track.events.push_back(Make(11900, kAppClose, "", kShop));
        track.events.push_back(Make(12000, kRelease, "ledger_ct"));
        return track;
    }

    void TestJoaat()
    {
        // The engine's hash of names (lower-cased): known values.
        Check(Joaat("S_CAMPLEDGER01X") == kLedger, "joaat S_CAMPLEDGER01X = 0x490CC7C7 (visual.md)");
        Check(Joaat("mount") == 0xA07A1994u, "joaat mount (entity_query.h)");
        Check(Joaat("SHOP_BROWSING") == Joaat("shop_browsing"), "joaat lower-cases");
    }

    void TestStateAt()
    {
        const Track track = Ledger();
        Check(StateAt(track, 0).Empty(), "nothing before the register");
        const State open = StateAt(track, 9000);
        Check(open.HasTarget("ledger_ct") && open.HasLink(kLedger) && open.HasApp(kShop) && open.apps[0].entry == kEntry,
            "inside the span: target, link and app with its entry");
        const State after = StateAt(track, 11950);
        Check(after.HasTarget("ledger_ct") && !after.HasApp(kShop), "the app closed, the target still there");
        const State gone = StateAt(track, 12000);
        Check(gone.Empty(), "the release takes the target and its link");
        // A refused register changes nothing.
        Track refused;
        Event e = Make(10, kRegister, "x");
        e.accepted = 0;
        refused.events.push_back(e);
        Check(StateAt(refused, 100).targets.empty(), "a refused register is no target");
        // A second register of the same name is one target.
        Track twice;
        twice.events.push_back(Make(10, kRegister, "a"));
        twice.events.push_back(Make(20, kRegister, "a"));
        Check(StateAt(twice, 30).targets.size() == 1, "one target per name");
    }

    void TestReconcile()
    {
        const Track track = Ledger();
        State have;
        // Relink only (c): the app is never planned.
        Plan plan = Reconcile(have, StateAt(track, 9000), false);
        Check(plan.registers.size() == 1 && plan.links.size() == 1 && plan.open.empty() && plan.close.empty() && plan.release.empty(),
            "c: register and link, no app");
        // Applied, nothing more to do on the next tick.
        for (const Target& t : plan.registers)
        {
            Event e = Make(0, kRegister, t.name.c_str());
            have.Apply(e);
        }
        for (const Link& l : plan.links)
            have.Apply(Make(0, kLink, l.target.c_str(), l.model));
        State want = StateAt(track, 9100);
        want.apps.clear();
        Check(Reconcile(have, want, false).Empty(), "c: nothing twice");
        // Past the release: release only.
        plan = Reconcile(have, StateAt(track, 13000), false);
        Check(plan.release.size() == 1 && plan.release[0] == "ledger_ct" && plan.registers.empty(), "past the end: release");
        // Seek back before the register: release; seek forward again: register and link again.
        Check(Reconcile(have, StateAt(track, 100), false).release.size() == 1, "seek back: release");
        State released;
        plan = Reconcile(released, StateAt(track, 10000), false);
        Check(plan.registers.size() == 1 && plan.links.size() == 1, "seek forward from empty: register and link");
        // A link already made is made again when its target is registered again.
        State linkOnly;
        linkOnly.Apply(Make(0, kLink, "ledger_ct", kLedger));
        plan = Reconcile(linkOnly, StateAt(track, 10000), false);
        Check(plan.registers.size() == 1 && plan.links.size() == 1, "a target registered again is linked again");
        // (a): the app opens with its entry, closes after its span.
        plan = Reconcile(State{}, StateAt(track, 9000), true);
        Check(plan.open.size() == 1 && plan.open[0].app == kShop && plan.open[0].entry == kEntry, "a: the app with its entry");
        State withApp = StateAt(track, 9000);
        plan = Reconcile(withApp, StateAt(track, 11950), true);
        Check(plan.close.size() == 1 && plan.open.empty() && plan.release.empty(), "a: the app closes at its span's end");
        // Apps off: an app the replay holds is closed.
        Check(Reconcile(withApp, StateAt(track, 9000), false).close.size() == 1, "apps off: what is open closes");
        // A different entry: closed and opened again.
        State other = StateAt(track, 9000);
        other.apps[0].entry = 1;
        plan = Reconcile(other, StateAt(track, 9000), true);
        Check(plan.close.size() == 1 && plan.open.size() == 1, "another entry: close then open");
    }

    void TestAppDrawsTarget()
    {
        const Track track = Ledger();
        Check(AppDrawsTarget(track, kShop, 9000), "the ledger's app draws into ledger_ct");
        Check(!AppDrawsTarget(track, kShop, 12500), "no span after the close");
        Track screenOnly;
        screenOnly.events.push_back(Make(100, kAppOpen, "", kShop, kEntry));
        screenOnly.events.push_back(Make(900, kAppClose, "", kShop));
        Check(!AppDrawsTarget(screenOnly, kShop, 500), "an app with no target alive is not relaunched");
        // A target registered after the open, inside the span.
        Track late;
        late.events.push_back(Make(100, kAppOpen, "", kShop, kEntry));
        late.events.push_back(Make(300, kRegister, "ledger_ct"));
        late.events.push_back(Make(900, kAppClose, "", kShop));
        Check(AppDrawsTarget(late, kShop, 150), "a target registered inside the span counts");
        // A target released before the open does not.
        Track before;
        before.events.push_back(Make(50, kRegister, "ledger_ct"));
        before.events.push_back(Make(60, kRelease, "ledger_ct"));
        before.events.push_back(Make(100, kAppOpen, "", kShop, kEntry));
        Check(!AppDrawsTarget(before, kShop, 150), "a target gone before the open does not count");
    }

    void TestLive()
    {
        Live live;
        live.Register("ledger_ct", 0, true);
        Check(live.Link(kLedger) == "ledger_ct", "a link is filed with the last accepted register");
        live.Register("other", 0, false);
        Check(live.Link(0x1234u) == "ledger_ct", "a refused register does not take the link");
        std::vector<Event> initial = StateEvents(live.Targets(), 0);
        Check(initial.size() == 3 && initial[0].kind == kRegister && initial[0].initial == 1 && initial[1].kind == kLink &&
                  initial[2].kind == kLink && initial[1].SameName("ledger_ct"),
            "the live state as initial events: target first, then links");
        live.Release("ledger_ct");
        Check(live.Targets().Empty(), "the release takes the links filed with it");
        Check(live.Link(0x99u).empty(), "after the release a link has no target");
    }

    void TestPoll()
    {
        AppPoll poll;
        poll.NoteLaunch(kShop, kEntry); // before the recording
        poll.Reset({kShop});
        Event e;
        Check(!poll.Observe(kShop, false, e), "closed and closed: nothing");
        Check(poll.Observe(kShop, true, e) && e.kind == kAppOpen && e.entry == kEntry, "open: event with the launch's entry");
        Check(!poll.Observe(kShop, true, e), "open and open: nothing");
        Check(poll.Observe(kShop, false, e) && e.kind == kAppClose, "closed: event");
        Check(poll.Add(0x55u) && !poll.Add(0x55u) && !poll.Add(0), "apps deduplicated, 0 refused");
        poll.NoteLaunch(0x66u, 7);
        Check(poll.Apps().size() == 3 && poll.EntryOf(0x66u) == 7, "a launched app is polled with its entry");
        for (std::uint32_t n = 0; n < 40; ++n)
            poll.Add(0x1000u + n);
        Check(poll.Apps().size() == kMaxApps, "at most kMaxApps polled");
    }

    void TestCut()
    {
        const Track track = Ledger();
        // A window starting inside the span: the state moves to 0.
        const Track cut = Cut(track, 9000, 20000);
        Check(cut.events.size() == 5 && cut.events[0].timeMs == 0 && cut.events[0].initial == 1 &&
                  cut.events[0].kind == kRegister && cut.events[1].kind == kLink && cut.events[2].kind == kAppOpen &&
                  cut.events[2].entry == kEntry && cut.events[3].timeMs == 2900 && cut.events[4].timeMs == 3000,
            "cut inside the span: the state at base at 0, the rest moved by -base");
        Check(StateAt(cut, 2950).HasTarget("ledger_ct") && !StateAt(cut, 2950).HasApp(kShop), "the cut plays the same");
        const Track early = Cut(track, 0, 9000);
        Check(early.events.size() == 3, "events after the end go");
        const Track late = Cut(track, 12500, 20000);
        Check(late.events.empty(), "after the release nothing is left");
    }

    void TestIni()
    {
        const std::vector<std::uint32_t> apps = ParseAppList(" SHOP_BROWSING , 0x0000ABCD;;shop_browsing,");
        Check(apps.size() == 2 && apps[0] == kShop && apps[1] == 0xABCDu, "app list: names, 0x hashes, deduplicated");
        Check(ParseAppList("").empty(), "empty list");
        Check(ModeFromInt(0) == Mode::Off && ModeFromInt(1) == Mode::Relink && ModeFromInt(2) == Mode::UiApp &&
                  ModeFromInt(9) == Mode::UiApp,
            "modes");
    }

    void TestNames()
    {
        Event e;
        e.SetName("0123456789012345678901234567890123456789");
        Check(e.NameValid() && std::strlen(e.name) == kNameBytes - 1, "a long name is cut to 31 characters");
        Event bad;
        std::memset(bad.name, 'a', sizeof(bad.name));
        Check(!bad.NameValid(), "an unterminated name is refused");
        bad.SetName("ab");
        bad.name[5] = 'x';
        Check(!bad.NameValid(), "bytes after the terminator are refused");
    }
}

int RunRenderTargetTests()
{
    TestJoaat();
    TestStateAt();
    TestReconcile();
    TestAppDrawsTarget();
    TestLive();
    TestPoll();
    TestCut();
    TestIni();
    TestNames();
    if (g_rtFailures == 0)
        std::printf("Render target tests passed.\n");
    return g_rtFailures;
}
