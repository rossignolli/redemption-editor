#pragma once

// The speech rules a line the replay asked does not have to pass again.
//
// A replayed line can die in state 3 with stop reason 6
// (POSE_V2_SPEECH_SILENT cause=revalidation) with the code of the speech
// entity's rules (vtable +0x68, 0x2F972C4 for a ped; the state 3 handler
// 0x2FC486C stops the line when the code is below 0x46, 0x2F49A64):
//   - 28 (0x1C, 0x2F975A4 at 0x2F97926): another speaker of the manager's
//     list (+0x78, count +0x80) is saying a line whose data priority (+0x25
//     of [line +0x68]) is at least this line's;
//   - 41 (0x29, 0x2F97CBB): the speaker is farther from the listener than
//     the line's range mode allows (0x2F9B3A4: 35, 60 or 100 m), unless line
//     +0x34 is 1 (e.g. away from the replay camera);
//   - 48 (0x30, 0x2F97E57): a scripted conversation is going on (the u16 at
//     0x5BD2F50, which a script native reads as "> 0") or its time stamp
//     0x5BD2F58 is not past, and neither the line's speaker nor its target
//     (+0x78) is the player ped (0x351808), unless line +0x38 is set.
// The three judge the world around the line (other speakers, the listener,
// the conversation state), which in a replay is the replay's own: live the
// recorded line passed them. A replay says a recorded line without the AI's
// can-say rules. So with SpeechReplayRules=1 (default) the rules detour
// (world_theatre.h kHookRules, forwarded first) gives "accepted" (0x4A) for
// these three codes on a line the replay asked; any other code (18: the
// speaker's own newest line of the context, 0x23: no ped, 0x2F: dead, ...)
// stays as the engine said, and so does every world line.
//
// Pure: unit tested in tests/missing_sound_tests.cpp.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace replay::speech::rules
{
    constexpr std::uint32_t kAccepted = 0x4A;
    constexpr std::uint32_t kOtherSpeaker = 0x1C;    // 28
    constexpr std::uint32_t kOutOfRange = 0x29;      // 41
    constexpr std::uint32_t kConversationOn = 0x30;  // 48

    // A code below 0x46 stops the line (0x2F49A64).
    inline bool Failed(std::uint32_t code) { return code != 0 && code < 0x46; }

    inline bool WorldContextCode(std::uint32_t code)
    {
        return code == kOtherSpeaker || code == kOutOfRange || code == kConversationOn;
    }

    // The code the detour returns. `replayLine`: the line is one the replay
    // asked (ReplayLines below, a puppet's speech entity, or asked on this
    // thread inside the replay's own call).
    inline std::uint32_t Decide(bool on, bool playing, bool replayLine, std::uint32_t code)
    {
        if (on && playing && replayLine && WorldContextCode(code))
            return kAccepted;
        return code;
    }

    // The lines the replay created in this playback (the line object the
    // state 3 handler gets), written by the script thread and read by the
    // detour on any thread. A ring: the newest kSize lines; an old entry
    // whose memory the engine gave to a world line can only let that line
    // past the three rules above.
    class ReplayLines
    {
    public:
        static constexpr std::size_t kSize = 256;
        void Clear()
        {
            for (auto& slot : m_lines)
                slot.store(0, std::memory_order_relaxed);
            m_next.store(0, std::memory_order_release);
        }
        void Note(std::uint64_t line)
        {
            if (!line)
                return;
            const std::uint64_t index = m_next.fetch_add(1, std::memory_order_acq_rel);
            m_lines[index % kSize].store(line, std::memory_order_release);
        }
        bool Has(std::uint64_t line) const
        {
            if (!line)
                return false;
            for (const auto& slot : m_lines)
                if (slot.load(std::memory_order_acquire) == line)
                    return true;
            return false;
        }

    private:
        std::atomic<std::uint64_t> m_lines[kSize] = {};
        std::atomic<std::uint64_t> m_next{0};
    };
}
