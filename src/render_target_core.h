#pragma once

// Named render targets and the UI apps that draw into them, as timestamped
// state events. The camp ledger S_CAMPLEDGER01X shows its pages through the
// render target "ledger_ct", registered and linked to the model by the camp
// script, and the UI app SHOP_BROWSING draws into it; a replay without these
// events shows the render target's placeholder (grey grid) on the clone.
//
// Engine model (build 1491.50, offline dump): detours on the engine's
// register, link and release of named render targets and on the UI app
// launch, each the callee of its native's handler. The link's target is not
// read; a link is filed with the name of the latest accepted register before
// it (the camp scripts register then link at once; hypothesis for other
// scripts).
// Research: docs/research/recording-render-targets.md
//
// Pure data and rules here (unit tested in tests/render_target_tests.cpp); the
// engine side is render_target.cpp.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace replay::rendertarget
{
    constexpr std::size_t kNameBytes = 32; // the name, NUL padded (31 characters at most)

    enum Kind : std::uint8_t
    {
        kRegister = 0, // name, flag, accepted
        kLink = 1,     // model (hash), name of the target it is filed with
        kRelease = 2,  // name, accepted
        kAppOpen = 3,  // app (hash), entry (hash, 0 unknown)
        kAppClose = 4, // app (hash)
        kKindCount
    };

    inline const char* KindName(std::uint8_t kind)
    {
        switch (kind)
        {
        case kRegister: return "register";
        case kLink: return "link";
        case kRelease: return "release";
        case kAppOpen: return "app_open";
        case kAppClose: return "app_close";
        default: return "unknown";
        }
    }

    struct Event
    {
        std::uint32_t timeMs = 0;
        std::uint8_t kind = kRegister;
        std::uint8_t flag = 0;     // register: the bool argument
        std::uint8_t accepted = 1; // register/release: the engine's answer
        std::uint8_t initial = 0;  // 1: state found at the recording's start (or the cut's base)
        std::uint32_t hash = 0;    // link: model; app_*: app
        std::uint32_t entry = 0;   // app_open: entry
        char name[kNameBytes] = {};

        void SetName(const char* text)
        {
            std::memset(name, 0, sizeof(name));
            if (!text)
                return;
            for (std::size_t n = 0; n + 1 < kNameBytes && text[n]; ++n)
                name[n] = text[n];
        }
        bool SameName(const char* text) const { return std::strncmp(name, text ? text : "", kNameBytes) == 0; }
        // A name that fits and is NUL terminated (the file reader's check).
        bool NameValid() const
        {
            for (std::size_t n = 0; n < kNameBytes; ++n)
                if (!name[n])
                {
                    for (std::size_t k = n; k < kNameBytes; ++k)
                        if (name[k])
                            return false;
                    return true;
                }
            return false;
        }
    };

    struct Track
    {
        std::vector<Event> events; // time ascending
        std::uint32_t dropped = 0; // detour ring overflow while recording

        bool Empty() const { return events.empty() && !dropped; }
        void Clear()
        {
            events.clear();
            dropped = 0;
        }
    };

    // ------------------------------------------------------------ state

    struct Target
    {
        std::string name;
        std::uint8_t flag = 0;
    };
    struct Link
    {
        std::uint32_t model = 0;
        std::string target; // "" when no target was registered before it
    };
    struct App
    {
        std::uint32_t app = 0;
        std::uint32_t entry = 0;
    };

    struct State
    {
        std::vector<Target> targets;
        std::vector<Link> links;
        std::vector<App> apps;

        bool HasTarget(const std::string& name) const
        {
            return std::any_of(targets.begin(), targets.end(), [&](const Target& t) { return t.name == name; });
        }
        bool HasLink(std::uint32_t model) const
        {
            return std::any_of(links.begin(), links.end(), [&](const Link& l) { return l.model == model; });
        }
        bool HasApp(std::uint32_t app) const
        {
            return std::any_of(apps.begin(), apps.end(), [&](const App& a) { return a.app == app; });
        }
        bool Empty() const { return targets.empty() && links.empty() && apps.empty(); }

        // One event applied (refused registers and releases change nothing).
        void Apply(const Event& e)
        {
            switch (e.kind)
            {
            case kRegister:
                if (e.accepted && !HasTarget(e.name))
                    targets.push_back({e.name, e.flag});
                break;
            case kRelease:
            {
                const std::string name = e.name;
                targets.erase(std::remove_if(targets.begin(), targets.end(), [&](const Target& t) { return t.name == name; }),
                    targets.end());
                links.erase(std::remove_if(links.begin(), links.end(), [&](const Link& l) { return l.target == name; }),
                    links.end());
                break;
            }
            case kLink:
                if (!HasLink(e.hash))
                    links.push_back({e.hash, e.name});
                break;
            case kAppOpen:
                if (!HasApp(e.hash))
                    apps.push_back({e.hash, e.entry});
                break;
            case kAppClose:
                apps.erase(std::remove_if(apps.begin(), apps.end(), [&](const App& a) { return a.app == e.hash; }), apps.end());
                break;
            default:
                break;
            }
        }
    };

    // The state after every event with timeMs <= t (in file order).
    inline State StateAt(const Track& track, std::uint32_t timeMs)
    {
        State state;
        for (const Event& e : track.events)
        {
            if (e.timeMs > timeMs)
                break;
            state.Apply(e);
        }
        return state;
    }

    // The events that rebuild a state at one instant (initial = 1): the
    // targets, then the links, then the apps.
    inline std::vector<Event> StateEvents(const State& state, std::uint32_t timeMs)
    {
        std::vector<Event> out;
        for (const Target& t : state.targets)
        {
            Event e;
            e.timeMs = timeMs;
            e.kind = kRegister;
            e.flag = t.flag;
            e.initial = 1;
            e.SetName(t.name.c_str());
            out.push_back(e);
        }
        for (const Link& l : state.links)
        {
            Event e;
            e.timeMs = timeMs;
            e.kind = kLink;
            e.hash = l.model;
            e.initial = 1;
            e.SetName(l.target.c_str());
            out.push_back(e);
        }
        for (const App& a : state.apps)
        {
            Event e;
            e.timeMs = timeMs;
            e.kind = kAppOpen;
            e.hash = a.app;
            e.entry = a.entry;
            e.initial = 1;
            out.push_back(e);
        }
        return out;
    }

    // ------------------------------------------------------------ recording

    // What the detours saw, kept outside the recording so that a recording that
    // starts with a target already registered gets it at 0 (initial state events).
    // The link is filed with the latest accepted register's name.
    class Live
    {
    public:
        void Register(const char* name, std::uint8_t flag, bool accepted)
        {
            Event e;
            e.kind = kRegister;
            e.flag = flag;
            e.accepted = accepted ? 1 : 0;
            e.SetName(name);
            m_state.Apply(e);
            if (accepted)
                m_lastTarget = e.name;
        }
        // The name the link is filed with (the event's name).
        std::string Link(std::uint32_t model)
        {
            Event e;
            e.kind = kLink;
            e.hash = model;
            e.SetName(m_lastTarget.c_str());
            m_state.Apply(e);
            return m_lastTarget;
        }
        void Release(const char* name)
        {
            Event e;
            e.kind = kRelease;
            e.SetName(name);
            m_state.Apply(e);
            if (m_lastTarget == e.name)
                m_lastTarget.clear();
        }
        void Clear()
        {
            m_state = State{};
            m_lastTarget.clear();
        }
        const State& Targets() const { return m_state; } // targets and links only (apps come from the poll)

    private:
        State m_state;
        std::string m_lastTarget;
    };

    // The UI apps polled while recording: the configured list, then every
    // app the launch detour saw (deduplicated, at most kMaxApps).
    constexpr std::size_t kMaxApps = 16;
    class AppPoll
    {
    public:
        // A recording starts: the apps to poll are the configured ones again
        // and none is open; the entries the launch detour saw are kept (an
        // app launched before the recording keeps its entry).
        void Reset(const std::vector<std::uint32_t>& configured)
        {
            m_apps.clear();
            m_open.clear();
            for (const std::uint32_t app : configured)
                Add(app);
        }
        bool Add(std::uint32_t app)
        {
            if (!app || std::find(m_apps.begin(), m_apps.end(), app) != m_apps.end() || m_apps.size() >= kMaxApps)
                return false;
            m_apps.push_back(app);
            return true;
        }
        // The launch detour's entry for that app (the latest wins).
        void NoteLaunch(std::uint32_t app, std::uint32_t entry)
        {
            Add(app);
            for (auto& e : m_entries)
                if (e.app == app)
                {
                    e.entry = entry;
                    return;
                }
            m_entries.push_back({app, entry});
        }
        std::uint32_t EntryOf(std::uint32_t app) const
        {
            for (const auto& e : m_entries)
                if (e.app == app)
                    return e.entry;
            return 0;
        }
        const std::vector<std::uint32_t>& Apps() const { return m_apps; }
        // One poll answer: true and `out` filled when the app opened or
        // closed since the last answer.
        bool Observe(std::uint32_t app, bool active, Event& out)
        {
            const bool was = std::find(m_open.begin(), m_open.end(), app) != m_open.end();
            if (active == was)
                return false;
            out = Event{};
            out.hash = app;
            if (active)
            {
                m_open.push_back(app);
                out.kind = kAppOpen;
                out.entry = EntryOf(app);
            }
            else
            {
                m_open.erase(std::remove(m_open.begin(), m_open.end(), app), m_open.end());
                out.kind = kAppClose;
            }
            return true;
        }
        bool Open(std::uint32_t app) const { return std::find(m_open.begin(), m_open.end(), app) != m_open.end(); }

    private:
        struct Entry
        {
            std::uint32_t app;
            std::uint32_t entry;
        };
        std::vector<std::uint32_t> m_apps;
        std::vector<std::uint32_t> m_open;
        std::vector<Entry> m_entries;
    };

    // ------------------------------------------------------------ playback

    enum class Mode : std::uint8_t
    {
        Off = 0,      // nothing played (the events stay in the clip)
        Relink = 1,   // (c) re-register and relink (the UI app relaunch, mode 2, gone)
    };
    inline Mode ModeFromInt(int value) { return value <= 0 ? Mode::Off : Mode::Relink; }
    inline const char* ModeName(Mode mode) { return mode == Mode::Off ? "off" : "relink"; }

    // What to do to go from what the replay applied (`have`) to the clip's
    // state at the playhead (`want`). Order: close apps, release targets,
    // register targets, link models, open apps.
    struct Plan
    {
        std::vector<App> close;
        std::vector<std::string> release;
        std::vector<Target> registers;
        std::vector<Link> links;
        std::vector<App> open;
        bool Empty() const { return close.empty() && release.empty() && registers.empty() && links.empty() && open.empty(); }
    };

    inline Plan Reconcile(const State& have, const State& want, bool apps)
    {
        Plan plan;
        for (const App& a : have.apps)
        {
            const bool keep = apps && std::any_of(want.apps.begin(), want.apps.end(),
                [&](const App& w) { return w.app == a.app && w.entry == a.entry; });
            if (!keep)
                plan.close.push_back(a);
        }
        for (const Target& t : have.targets)
            if (!want.HasTarget(t.name))
                plan.release.push_back(t.name);
        for (const Target& t : want.targets)
            if (!have.HasTarget(t.name))
                plan.registers.push_back(t);
        for (const Link& l : want.links)
        {
            // A link whose target is (re)registered by this plan is made
            // again (the engine links the model to a live target).
            const bool targetNew = std::any_of(plan.registers.begin(), plan.registers.end(),
                [&](const Target& t) { return t.name == l.target; });
            if (!have.HasLink(l.model) || targetNew)
                plan.links.push_back(l);
        }
        if (apps)
            for (const App& a : want.apps)
            {
                const bool had = std::any_of(have.apps.begin(), have.apps.end(),
                    [&](const App& h) { return h.app == a.app && h.entry == a.entry; });
                if (!had)
                    plan.open.push_back(a);
            }
        return plan;
    }

    // ------------------------------------------------------------ window cut

    // The track cut to (base, end]: the state at base becomes initial events
    // at 0, the later events move by -base, everything after end goes.
    inline Track Cut(const Track& in, std::uint32_t base, std::uint32_t end)
    {
        Track out;
        out.dropped = in.dropped;
        out.events = StateEvents(StateAt(in, base), 0);
        for (const Event& e : in.events)
        {
            if (e.timeMs <= base || e.timeMs > end)
                continue;
            Event copy = e;
            copy.timeMs = e.timeMs - base;
            out.events.push_back(copy);
        }
        return out;
    }

    // ------------------------------------------------------------ ini

    // joaat of the text, lower-cased (the engine's hash of a name).
    inline std::uint32_t Joaat(const char* text)
    {
        std::uint32_t hash = 0;
        for (; text && *text; ++text)
        {
            char c = *text;
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
            hash += static_cast<std::uint8_t>(c);
            hash += hash << 10;
            hash ^= hash >> 6;
        }
        hash += hash << 3;
        hash ^= hash >> 11;
        hash += hash << 15;
        return hash;
    }

}
