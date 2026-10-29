#pragma once

// The game's score off while the replay owns the world, as a replay usually
// stops and resets the interactive music when it takes the world and never
// lets the score back in (the video's music is the project's track).
//
// Rung 1 only (public natives, called by script.cpp on the script thread):
// - enter: SET_AUDIO_FLAG("OpenWorldMusicDisabled", TRUE) holds the open
//   world music director (it neither starts nor brings back a track), and
//   TRIGGER_MUSIC_EVENT("STOP_MUSIC_8S") fades out what already plays (the
//   event startup_sp fires when a mission restarts; its content is audio
//   data, the native says whether it exists);
// - watch: AUDIO_IS_MUSIC_PLAYING once a second; music still playing after
//   the fade (a mission script fired another event) stops again and is
//   reported as the QA anomaly music_in_replay;
// - exit: the flag back to FALSE and STOP_AUDIO_SCENES() (the scenes of
//   this script's slot). The open world score comes back by the director's
//   own timer.
// The flag is a bit of this script's slot: setting it twice counts once and
// a slot freed by the engine (the script dies) drops it. The mod still clears
// it at startup, at unload and at every exit. The engine's own StopAndReset
// (rung 4) is not used.
//
// Pure: no engine access; unit tested in tests/music_stop_tests.cpp.

#include <cstdint>

namespace replay::music
{
    inline constexpr const char* kFlagName = "OpenWorldMusicDisabled";
    inline constexpr const char* kStopEvent = "STOP_MUSIC_8S";
    // The sensor's period while the flag is on.
    inline constexpr std::uint64_t kWatchPeriodMs = 1000;
    // STOP_MUSIC_8S fades in 8 s (hypothesis): music before this, counted
    // from the entry (or the last stop), is the fade, not an anomaly.
    inline constexpr std::uint64_t kFadeGraceMs = 9000;
    // Music that stays on this long after the grace is music in the replay.
    inline constexpr std::uint64_t kPlayingAnomalyMs = 2000;

    // MusicStop in RedemptionEditor.ini: 0 off, 1 public natives (default). 2 is
    // reserved for the engine's StopAndReset (not built): it runs as 1 and says so
    // in the log.
    enum class Mode : std::uint8_t
    {
        Off = 0,
        Natives = 1,
    };

    inline Mode ModeFromIni(std::uint32_t value)
    {
        return value == 0 ? Mode::Off : Mode::Natives;
    }

    // What the script thread has to call.
    struct Plan
    {
        bool setFlag = false;    // SET_AUDIO_FLAG(kFlagName, flagValue)
        bool flagValue = false;
        bool triggerStop = false; // TRIGGER_MUSIC_EVENT(kStopEvent)
        bool stopScenes = false;  // STOP_AUDIO_SCENES()
        bool already = false;     // nothing to do: already in that state
    };

    enum class WatchResult : std::uint8_t
    {
        Idle = 0,  // no sample this call
        Sampled,   // a sample, same as the last one
        Changed,   // a sample that changed (or the first one)
        Restop,    // music on past the fade: stop again, report the anomaly
    };

    class Guard
    {
    public:
        // The replay takes the world (BeginClipWorld). A second entry
        // without an exit does nothing (the flag is one bit per slot).
        Plan Enter(Mode mode, std::uint64_t now)
        {
            Plan plan;
            if (mode == Mode::Off)
            {
                plan.already = true;
                return plan;
            }
            if (m_on)
            {
                plan.already = true;
                return plan;
            }
            m_on = true;
            m_enteredAt = now;
            m_stopAt = now;
            m_nextSample = now + kWatchPeriodMs;
            m_sampled = false;
            m_playing = false;
            m_playingSince = 0;
            m_restops = 0;
            plan.setFlag = true;
            plan.flagValue = true;
            plan.triggerStop = true;
            return plan;
        }

        // The replay gives the world back (StopPlayback, unload). Only an
        // entry is undone; a second exit does nothing.
        Plan Exit()
        {
            Plan plan;
            if (!m_on)
            {
                plan.already = true;
                return plan;
            }
            m_on = false;
            plan.setFlag = true;
            plan.flagValue = false;
            plan.stopScenes = true;
            return plan;
        }

        // The mod starts (a reload in the middle of a replay): the flag of
        // this slot off whatever the state, no scene touched.
        static Plan Startup()
        {
            Plan plan;
            plan.setFlag = true;
            plan.flagValue = false;
            return plan;
        }

        // Called every tick while the replay owns the world; `sense` is
        // called only when a sample is due (AUDIO_IS_MUSIC_PLAYING).
        template <typename Sense>
        WatchResult Watch(std::uint64_t now, Sense&& sense)
        {
            if (!m_on || now < m_nextSample)
                return WatchResult::Idle;
            m_nextSample = now + kWatchPeriodMs;
            const bool playing = sense();
            const bool changed = !m_sampled || playing != m_playing;
            m_sampled = true;
            m_playing = playing;
            if (!playing)
            {
                m_playingSince = 0;
                return changed ? WatchResult::Changed : WatchResult::Sampled;
            }
            if (changed || m_playingSince == 0)
                m_playingSince = now;
            // Counted from the later of: the music seen on, the end of the
            // fade of the last stop.
            const std::uint64_t graceEnd = m_stopAt + kFadeGraceMs;
            const std::uint64_t since = m_playingSince > graceEnd ? m_playingSince : graceEnd;
            if (now >= since + kPlayingAnomalyMs)
            {
                m_stopAt = now; // the next one needs a whole new grace
                ++m_restops;
                return WatchResult::Restop;
            }
            return changed ? WatchResult::Changed : WatchResult::Sampled;
        }

        bool On() const { return m_on; }
        bool Playing() const { return m_playing; }
        std::uint64_t SinceEntryMs(std::uint64_t now) const { return now >= m_enteredAt ? now - m_enteredAt : 0; }
        std::uint32_t Restops() const { return m_restops; }

    private:
        bool m_on = false;
        std::uint64_t m_enteredAt = 0;
        std::uint64_t m_stopAt = 0;
        std::uint64_t m_nextSample = 0;
        bool m_sampled = false;
        bool m_playing = false;
        std::uint64_t m_playingSince = 0;
        std::uint32_t m_restops = 0;
    };
}
