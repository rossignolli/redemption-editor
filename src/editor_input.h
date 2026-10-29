#pragma once

// The editor's input rules that need no ReShade nor ImGui, kept pure so
// tests/editor_input_tests.cpp can pin them.
//
// Key taps lost in a long frame: ReShade feeds ImGui the key *levels* it sees
// once per frame (AddKeyEvent(key, is_key_down)) and its own is_key_pressed
// needs the key down at the frame's end. A press and release that both land
// inside one frame (a stall: the gallery's disk read, a clip load, a save that
// waits) never show as down. The release still leaves ReShade's
// is_key_released set for that frame; KeyEdges tells such a release (the key
// was up at the end of the frame before) from the end of a press already seen.
//
// Steps built on a lagging snapshot: the editor steps a value (a marker
// option, the time, the speed) from the core's snapshot, which comes back a
// frame or more after the command. A second step before it returns would
// build on the old value and send the same one again (a held arrow, a quick
// double press, PageUp at a low frame rate). PendingValue keeps the value last
// sent for a short window so the next step builds on it.

#include <cstdint>

namespace replay::editor::input
{
    struct KeyEdges
    {
        std::uint8_t down[256]{};     // the key's level at this frame
        std::uint8_t downBefore[256]{}; // at the frame before

        // Once per frame, before any key is read: the level ReShade reports now.
        void BeginFrame(unsigned vk, bool downNow)
        {
            if (vk >= 256)
                return;
            downBefore[vk] = down[vk];
            down[vk] = downNow ? 1 : 0;
        }

        // `releasedNow`: ReShade's is_key_released this frame. True when the
        // whole press came and went inside this frame (never seen down).
        bool MissedTap(unsigned vk, bool releasedNow) const
        {
            return vk > 0 && vk < 256 && releasedNow && !downBefore[vk] && !down[vk];
        }
    };

    template <typename T>
    struct PendingValue
    {
        std::uint32_t key = 0;
        T value{};
        std::uint64_t at = 0;
        bool set = false;

        static constexpr std::uint64_t kWindowMs = 300;

        // The value to step from: the one sent last for this key while the
        // snapshot may still lag it, the snapshot's otherwise.
        T Base(std::uint32_t forKey, const T& snapshot, std::uint64_t now) const
        {
            return set && key == forKey && now >= at && now - at < kWindowMs ? value : snapshot;
        }

        void Sent(std::uint32_t forKey, const T& sent, std::uint64_t now)
        {
            key = forKey;
            value = sent;
            at = now;
            set = true;
        }

        void Clear() { set = false; }
    };
}
