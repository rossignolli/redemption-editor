#pragma once

// The world's ropes while the replay owns the world. As a replay editor
// usually clears the world's ropes before it plays, only the replay's ropes
// should be drawn. Here the ropes that are not the replay's (the reins of the player's real horse, a
// hitch rope of a hidden camp horse) are hidden with the public
// _ROPE_CHANGE_VISIBILITY while the clip plays and shown again when it
// stops, the way the hidden world entities are (replay_playback.cpp).
//
// A rope is the replay's when the replay plays it (its own rope, or the
// puppet rope a record follows) or when one of its attachment entries holds
// a puppet. Every other rope with an entry on an entity (the player, a
// hidden entity, anything of the world) is hidden. A rope tied to nothing
// (no entity in any entry) is left: it is map dressing, like the map's
// objects the replay keeps.
//
// The rope header's flag 0x2 (+0x758) is read
// as "drawn". It is not taken on trust: the first hide is checked on the
// next scan (Bit()), and the restore and the rope_stray detector only
// believe the flag once a hide cleared it.
//
// Pure (no engine call): unit tested in ReplayCoreTests.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace replay::ropes
{
    constexpr std::uint32_t kRopeVisibleFlag = 0x2u;
    // rope_stray: a world rope drawn this close to the camera while the
    // replay plays (40 m).
    constexpr float kRopeStrayRadiusM = 40.0f;
    constexpr unsigned long long kRopeStrayEveryMs = 2000; // per rope
    // The flag is called wrong after this many hides it did not follow
    // with none it did.
    constexpr std::uint32_t kRopeBitDisproveAfter = 3;

    struct WorldRopeView
    {
        std::uint32_t id = 0;
        bool replay = false;        // played by the replay, or an entry on a puppet
        bool onWorldEntity = false; // an entry on an entity that is not a puppet
        bool visible = false;       // kRopeVisibleFlag set
        float cameraM = 1.0e9f;     // nearest end to the camera
    };

    enum class WorldRopeAction : std::uint8_t
    {
        Leave,
        Hide,
        Show,
    };

    struct WorldRopeStep
    {
        WorldRopeAction action = WorldRopeAction::Leave;
        // rope_stray: a world rope drawn near the camera after it was
        // hidden (a script showed it, or the hide did not take).
        bool stray = false;
    };

    enum class VisibleBit : std::uint8_t
    {
        Unknown,
        Proven,    // a hide cleared the flag
        Disproven, // hides did not clear it: the flag says nothing
    };

    inline const char* VisibleBitText(VisibleBit bit)
    {
        return bit == VisibleBit::Proven ? "proven" : bit == VisibleBit::Disproven ? "disproven" : "unknown";
    }

    class WorldRopeHider
    {
    public:
        struct Hidden
        {
            std::uint32_t id = 0;
            bool wasVisible = false;  // drawn when first hidden
            bool hideChecked = true;  // false: a hide of a drawn rope waits for the next scan's check
            bool seen = false;        // listed in this scan
            unsigned long long strayAt = 0;
            bool strayLogged = false;
        };

        struct Counters
        {
            std::uint32_t hidden = 0;    // world ropes hidden, first time
            std::uint32_t rehidden = 0;  // drawn again after a hide, hidden again
            std::uint32_t released = 0;  // hidden, then the replay's (a puppet took it)
            std::uint32_t gone = 0;      // hidden, then deleted by the game
            std::uint32_t strays = 0;    // rope_stray findings
            std::uint32_t hideHeld = 0;  // a hide the flag followed
            std::uint32_t hideMissed = 0; // a hide the flag did not follow
        };

        void Reset()
        {
            m_hidden.clear();
            m_counters = {};
        }

        bool Empty() const { return m_hidden.empty(); }
        std::size_t Size() const { return m_hidden.size(); }
        const Counters& Stats() const { return m_counters; }

        VisibleBit Bit() const
        {
            if (m_counters.hideHeld > 0)
                return VisibleBit::Proven;
            return m_counters.hideMissed >= kRopeBitDisproveAfter ? VisibleBit::Disproven : VisibleBit::Unknown;
        }

        void BeginScan()
        {
            for (Hidden& hidden : m_hidden)
                hidden.seen = false;
        }

        WorldRopeStep Observe(const WorldRopeView& view, unsigned long long nowMs)
        {
            WorldRopeStep step;
            const auto it = std::find_if(m_hidden.begin(), m_hidden.end(), [&](const Hidden& h) { return h.id == view.id; });
            if (view.replay)
            {
                // A puppet's rope (or one the replay took over) is never
                // hidden; one hidden before the puppet held it comes back.
                if (it == m_hidden.end())
                    return step;
                if (ShowOnRestore(*it))
                    step.action = WorldRopeAction::Show;
                m_hidden.erase(it);
                ++m_counters.released;
                return step;
            }
            if (it == m_hidden.end())
            {
                if (!view.onWorldEntity)
                    return step;
                Hidden hidden;
                hidden.id = view.id;
                hidden.wasVisible = view.visible;
                hidden.hideChecked = !view.visible;
                hidden.seen = true;
                m_hidden.push_back(hidden);
                ++m_counters.hidden;
                // Always hidden on first sight: the flag is not trusted yet.
                step.action = WorldRopeAction::Hide;
                return step;
            }
            Hidden& hidden = *it;
            hidden.seen = true;
            bool drawnAfterHide = false;
            if (!hidden.hideChecked)
            {
                hidden.hideChecked = true;
                if (view.visible)
                {
                    ++m_counters.hideMissed;
                    drawnAfterHide = true;
                }
                else
                    ++m_counters.hideHeld;
            }
            else if (view.visible)
                drawnAfterHide = true;
            const VisibleBit bit = Bit();
            if (!drawnAfterHide || bit == VisibleBit::Disproven)
                return step;
            // Drawn again: hidden again, and checked on the next scan.
            step.action = WorldRopeAction::Hide;
            hidden.hideChecked = false;
            ++m_counters.rehidden;
            if (bit == VisibleBit::Proven && view.cameraM <= kRopeStrayRadiusM &&
                (!hidden.strayLogged || nowMs >= hidden.strayAt + kRopeStrayEveryMs))
            {
                hidden.strayLogged = true;
                hidden.strayAt = nowMs;
                step.stray = true;
                ++m_counters.strays;
            }
            return step;
        }

        // The hidden ropes this scan did not list were deleted by the game.
        std::uint32_t EndScan()
        {
            const std::size_t before = m_hidden.size();
            m_hidden.erase(std::remove_if(m_hidden.begin(), m_hidden.end(), [](const Hidden& h) { return !h.seen; }),
                m_hidden.end());
            const std::uint32_t gone = static_cast<std::uint32_t>(before - m_hidden.size());
            m_counters.gone += gone;
            return gone;
        }

        // Playback ends: the ropes to show again (those drawn when hidden;
        // every one while the flag is not proven). The list empties.
        std::vector<std::uint32_t> TakeRestore()
        {
            std::vector<std::uint32_t> show;
            for (const Hidden& hidden : m_hidden)
            {
                if (ShowOnRestore(hidden))
                    show.push_back(hidden.id);
            }
            m_hidden.clear();
            return show;
        }

    private:
        bool ShowOnRestore(const Hidden& hidden) const
        {
            // Only a proven flag may keep a rope hidden after the replay.
            return hidden.wasVisible || Bit() != VisibleBit::Proven;
        }

        std::vector<Hidden> m_hidden;
        Counters m_counters;
    };
}
