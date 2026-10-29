#pragma once

// What the mod log keeps, and the shared evidence cap.
//
// Debug=0 in RedemptionEditor.ini keeps only the essential lines (EssentialLine:
// the load header, recording, clip and buffer lines, summaries, timings and
// errors, the ASI identity, the ini as read, the report hotkey and any detour
// or guarded call failure); Debug=1 keeps every evidence line. The filter is
// the log stream's own (script.cpp, QuietFilter); a heavy evidence site asks
// Verbose() before formatting so a release build does not even build the line.
//
// LogLimit counts per recording or playback session, and NewSession()
// (BeginRecording, BeginPlayback) re-arms every limit at once, so a second
// playback in the same game session logs again.
//
// Pure apart from the two atomics: unit tested (ReplayCoreTests).

#include <atomic>
#include <cstdint>
#include <string_view>

namespace replay::log
{
    namespace detail
    {
        inline std::atomic<bool> g_verbose{true};
        inline std::atomic<std::uint32_t> g_session{1};
        inline std::atomic<bool> g_quiet{false};
    }

    inline void SetVerbose(bool verbose) { detail::g_verbose.store(verbose, std::memory_order_relaxed); }
    inline bool Verbose() { return detail::g_verbose.load(std::memory_order_relaxed); }

    // The log filter's quiet state of this tick (the always-on buffer drops its
    // noisy lines; script.cpp sets both at the top of every tick), kept by text
    // written on a later tick (deferred_log.h).
    inline void SetQuiet(bool quiet) { detail::g_quiet.store(quiet, std::memory_order_relaxed); }
    inline bool Quiet() { return detail::g_quiet.load(std::memory_order_relaxed); }

    // A recording or a playback starts: every LogLimit counts from zero.
    inline void NewSession() { detail::g_session.fetch_add(1, std::memory_order_relaxed); }
    inline std::uint32_t Session() { return detail::g_session.load(std::memory_order_relaxed); }

    // The tag of a log line: up to the first ',', '=', ':' or ' '.
    inline std::string_view TagOf(std::string_view line)
    {
        const std::size_t end = line.find_first_of(",=: \n");
        return end == std::string_view::npos ? line : line.substr(0, end);
    }

    // Lines kept with Debug=0. Anything that does not start with an upper
    // case tag (the load header: "=== Redemption Editor loaded ===",
    // "buildVersion=...") is kept.
    inline bool EssentialLine(std::string_view line)
    {
        if (line.empty() || line[0] < 'A' || line[0] > 'Z')
            return true;
        const std::string_view tag = TagOf(line);
        static constexpr std::string_view kPrefixes[] = {
            "RECORD",         // RECORDING_START, RECORD_GATE_*, RECORD_PAUSE
            "REC_",           // REC_START_TIMING, REC_STOP_TIMING, REC_FRAME_GAPS
            "CLIP_",          // CLIP_LOAD, CLIP_SAVE, CLIP_LIST, CLIP_READ_START...
            "BUFFER_",        // BUFFER_SAVE, BUFFER_SAVE_COPY, BUFFER_STATE, BUFFER_STOP
            "CAPTURE_STOP",
            "PLAYBACK_",      // PLAYBACK_STOP, PLAYBACK_TRANSPORT, PLAYBACK_SCRUB (not POSE_V2_PLAYBACK_*)
            "EDITOR_",        // EDITOR_COMMAND, EDITOR_BRIDGE: what the editor asked
            "CAMERA_MARKER",
            "CAMERA_SHAKE",   // the shake in force (capped per session) and a marker's shake set by the editor
            "SHAKE_MIGRATION", // a legacy shake folded on load: counts (public), the proof (developer build)
            "MARKER_",
            "JUMP_LOAD",
            "SCRUB_COST",
            "SEEK_",
            "HOOK_WARMUP",
            "ENTITY_POOLS",
            "DETACH_STEP",    // unload, crash-relevant
            "RING_PROTOCOL",  // unload: the event ring's dropped/torn/lost counts
            "UNLOAD",
            "BENCH_",         // only written while the bench drives the game
            "PROFILE_TOTAL",  // session totals, one block per recording or playback
            "ASI_",           // ASI_IDENTITY, the version and SHA-256 at load
            "CONFIG_",        // CONFIG_INI, the ini as read
            "STORAGE_",       // STORAGE_BASE, STORAGE_SETTINGS: where the mod writes (storage_paths.h)
            "REPORT_",        // REPORT_SAVE, the report hotkey
            "POSE_FINALIZER", // refused, yielded to the Pose Editor
            "MUSIC_",         // MUSIC_STOP (enter, exit, restop), MUSIC_WATCH (the changes; every second with Debug=1)
            "WORLD_MODEL",    // WORLD_MODEL (config, checks, clear, reload, reload_watch, session_reset)
            "CAMERA_CUT_RATE", // camera_cut.h: cut frames per second
            "CAMERA_CUT_MODE", // camera_cut.h: config, verified, fallback
            "METAPED_READER",  // the metaped reader's checks
            "POSE_V2_METAPED", // the look recorded per ped (capped)
            "POSE_V2_PLAYBACK_METAPED", // ... and applied, with the difference (capped)
            "POSE_V2_APPEARANCE_NOT_RECORDED", // ... the QA rule appearance_not_recorded
            "POSE_V2_PLAYBACK_SHOP_ITEMS", // a body dressed by shop items, read back (capped)
            "POSE_V2_SEGMENT_SWITCH", // a skeleton change re-armed in the tick
            "CAMERA_RELEASE",  // the view lent to another mod's camera (config, lend, take back)
            "POSE_V2_PUPPET_ANIM_SKIP", // a puppet the engine stopped updating (capped)
            "POSE_V2_PUPPET_LOW_LOD",   // ... and the QA rule puppet_low_lod: shown with an old pose (capped)
            "SPEECH_SLOT_",    // the slot gate's waits, takes and releases (capped per playback)
            "POSE_V2_SPEECH_ADOPT", // a line adopted already playing
            "KEYS",           // KEYS (a [Keys] value refused), KEYS_TABLE
            "POSE_V2_REENTRY", // one engine entity recorded twice (capped)
            "POSE_V2_TPOSE",   // the T-pose watch (episodes, capped), the bind layout check
            "POSE_V2_NEVER_ANIMATED", // ... the never animated hide verdict per record (capped)
            "MIX_",            // only with MixProbe=1 (budgeted per second)
            "ENV_MODS",        // the loaded mod environment (on change, at most every 30 s)
            "POSE_V2_DECAL_GUARD",     // a decal refused before the core (capped)
            "POSE_V2_DECAL_NULL_INFO", // a decal without info refused (capped)
            "POSE_V2_DECAL_MONITOR",   // the decal monitor emptied on a script restart
            "POSE_V2_SHOT_PLAYER_BANK", // the player's shots without their bank
            "PLAYER_WEAPON_BANK", // the hidden player's weapon for the bank: equip (capped), equip_failed, end, inventory
            "DRAWSTORE_",      // DRAWSTORE_SLOT (load, playback start, reload push, session reset)
            "POSE_V2_SCRIPT_FX_REC", // the script looped effects kept by their per-frame get
            "FX_SCRIPT_LOOP_CHECK",  // ... and their playback check (one line per playback)
            "POSE_V2_ROPE_PIN_CHECK", // the recorded hand and rest pins held (one line per playback)
            "POSE_V2_PLAYBACK_REINS_GUARD", // ... the reins guards' counts (one line per playback)
        };
        for (const std::string_view prefix : kPrefixes)
        {
            if (tag.substr(0, prefix.size()) == prefix)
                return true;
        }
        static constexpr std::string_view kSuffixes[] = {"_TOTAL", "_SUMMARY", "_TIMING", "_OBSERVER"};
        for (const std::string_view suffix : kSuffixes)
        {
            if (tag.size() >= suffix.size() && tag.substr(tag.size() - suffix.size()) == suffix)
                return true;
        }
        static constexpr std::string_view kWords[] = {"ERROR", "FAIL", "CRASH", "EXCEPTION"};
        for (const std::string_view word : kWords)
        {
            if (tag.find(word) != std::string_view::npos)
                return true;
        }
        // A detour or engine call that failed its check, whatever the line's tag: the
        // error texts of the install paths (prologue signature, MinHook) and of the SEH
        // guards. Only these markers: a "status=failed" alone stays evidence.
        static constexpr std::string_view kFailureMarkers[] = {
            "signature_mismatch", "mh_create_", "mh_enable_", "mh_initialize_", "=exception_"};
        for (const std::string_view marker : kFailureMarkers)
        {
            if (line.find(marker) != std::string_view::npos)
                return true;
        }
        return false;
    }

    // The shared evidence cap: the first `cap` calls per session say yes,
    // then one call in `every` (0: none). Script thread only.
    struct LogLimit
    {
        std::uint32_t count = 0;
        std::uint32_t session = 0;

        bool Take(std::uint32_t cap, std::uint32_t every = 0)
        {
            Sync();
            const std::uint32_t n = count;
            if (count != UINT32_MAX)
                ++count;
            if (n < cap)
                return true;
            return every != 0 && (n - cap) % every == every - 1;
        }
        // Calls seen this session (the ones refused included).
        std::uint32_t Seen()
        {
            Sync();
            return count;
        }

    private:
        void Sync()
        {
            const std::uint32_t now = Session();
            if (session != now)
            {
                session = now;
                count = 0;
            }
        }
    };
}
