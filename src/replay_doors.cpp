#include "replay_doors.h"
#include "door_table.h"
#include "door_rebind.h"

#include "main.h"
#include "natives.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace replay::doors
{
    namespace
    {
        constexpr float kScanRadiusM = 45.0f;
        constexpr std::uint32_t kScanIntervalMs = 500;
        constexpr float kMinTurnDeg = 0.2f;
        constexpr float kMinMoveM = 0.01f;

        struct Watched
        {
            std::size_t track = 0;
            Entity handle = 0;
        };
        std::unordered_map<std::uint32_t, Watched> g_watched; // by door hash
        std::uint32_t g_nextScanMs = 0;
        int g_missing = 0;

        struct Played
        {
            std::size_t track = 0;
            Entity handle = 0;
            Vector3 rotation{};   // the handle's own rotation when bound (given back at the end)
            bool wasFrozen = false;
            bool everBound = false;
            bool lostLogged = false;
            bool registered = false; // in the door system (DOOR_SYSTEM_* work on it)
            bool hasRatio = false;
            float lastRatio = 0.0f;  // the engine's ratio of the last played rotation
            bool held = false;       // one-sample track (door_rebind.h TrackPlay::Held)
            bool heldApplied = false; // placed since the last bind
            std::uint64_t heldAppliedAt = 0;
        };
        std::vector<Played> g_played;
        int g_unresolved = 0;
        // door_rebind.h.
        Config g_config;
        bool g_closedTheatre = false;
        std::uint64_t g_nextRebindAt = 0;
        RebindTotals g_totals;
        std::uint32_t g_doorLogs = 0;
        constexpr std::uint32_t kDoorLogCap = 64;

        float AngleDelta(float a, float b)
        {
            return std::fmod(a - b + 540.0f, 360.0f) - 180.0f;
        }

        void Sample(Recording& clip, const Watched& watched, std::uint32_t nowMs)
        {
            if (!watched.handle || !ENTITY::DOES_ENTITY_EXIST(watched.handle))
                return;
            const Vector3 r = ENTITY::GET_ENTITY_ROTATION(watched.handle, 2);
            const Vector3 p = ENTITY::GET_ENTITY_COORDS(watched.handle, TRUE, FALSE);
            DoorTrack& track = clip.doors[watched.track];
            if (!track.samples.empty())
            {
                const DoorSample& last = track.samples.back();
                const float turn = (std::max)({std::fabs(AngleDelta(r.x, last.rotation[0])),
                    std::fabs(AngleDelta(r.y, last.rotation[1])), std::fabs(AngleDelta(r.z, last.rotation[2]))});
                const float move = std::fabs(p.x - last.position[0]) + std::fabs(p.y - last.position[1]) +
                    std::fabs(p.z - last.position[2]);
                if (turn < kMinTurnDeg && move < kMinMoveM)
                    return;
            }
            track.samples.push_back({nowMs, {r.x, r.y, r.z}, {p.x, p.y, p.z}});
        }
    }

    void BeginRecording(Recording& clip)
    {
        clip.doors.clear();
        g_watched.clear();
        g_nextScanMs = 0;
        g_missing = 0;
    }

    void UpdateRecording(Recording& clip, std::uint32_t nowMs, float x, float y, float z)
    {
        if (nowMs >= g_nextScanMs)
        {
            g_nextScanMs = nowMs + kScanIntervalMs;
            for (const DoorEntry& entry : kDoorTable)
            {
                const float dx = entry.x - x, dy = entry.y - y, dz = entry.z - z;
                if (dx * dx + dy * dy + dz * dz > kScanRadiusM * kScanRadiusM)
                    continue;
                auto found = g_watched.find(entry.hash);
                if (found != g_watched.end() && found->second.handle && ENTITY::DOES_ENTITY_EXIST(found->second.handle))
                    continue;
                const Entity handle = ENTITY::_GET_ENTITY_BY_DOORHASH(static_cast<Hash>(entry.hash), 0);
                if (!handle || !ENTITY::DOES_ENTITY_EXIST(handle))
                {
                    ++g_missing;
                    continue;
                }
                if (found == g_watched.end())
                {
                    clip.doors.push_back({entry.hash, {}});
                    found = g_watched.emplace(entry.hash, Watched{clip.doors.size() - 1, handle}).first;
                }
                found->second.handle = handle; // streamed back in: a new handle
            }
        }
        for (const auto& [hash, watched] : g_watched)
            Sample(clip, watched, nowMs);
    }

    void EndRecording(const Recording& clip, std::ostream& log)
    {
        std::size_t moved = 0, samples = 0, single = 0;
        for (const DoorTrack& track : clip.doors)
        {
            samples += track.samples.size();
            moved += track.samples.size() > 1 ? 1 : 0;
            single += track.samples.size() == 1 ? 1 : 0;
        }
        // single = doors seen once (their state for the clip, played held).
        log << "POSE_V2_DOORS,watched=" << clip.doors.size() << ",moved=" << moved << ",single=" << single << ",samples=" << samples
            << ",not_streamed=" << g_missing << '\n';
        g_watched.clear();
    }

    std::uint32_t ForgetPlaybackAfterRestart()
    {
        const std::uint32_t forgotten = static_cast<std::uint32_t>(g_played.size());
        g_played.clear();
        return forgotten;
    }

    void Configure(const Config& config) { g_config = config; }
    const Config& GetConfig() { return g_config; }

    namespace
    {
        // A handle for the door now (bound: its own state kept for the end).
        bool Bind(Played& played, Entity handle)
        {
            if (!handle || !ENTITY::DOES_ENTITY_EXIST(handle))
                return false;
            played.handle = handle;
            played.rotation = ENTITY::GET_ENTITY_ROTATION(handle, 2);
            played.wasFrozen = ENTITY::_IS_ENTITY_FROZEN(handle) != FALSE;
            played.lostLogged = false;
            played.heldApplied = false; // a held door is placed again on a new handle
            return true;
        }

        void LogDoor(std::ostream& log, std::uint32_t hash, const char* event, std::uint32_t playheadMs, Entity handle)
        {
            if (g_doorLogs >= kDoorLogCap)
                return;
            ++g_doorLogs;
            log << "POSE_V2_PLAYBACK_DOOR,hash=0x" << std::hex << hash << std::dec << ",event=" << event
                << ",playhead_ms=" << playheadMs << ",handle=" << handle << '\n';
        }
    }

    void BeginPlayback(const Recording& clip, std::ostream& log, bool closedTheatre)
    {
        g_played.clear();
        g_unresolved = 0;
        g_totals = RebindTotals{};
        g_doorLogs = 0;
        g_closedTheatre = closedTheatre;
        g_nextRebindAt = GetTickCount64() + g_config.rebindMs;
        for (std::size_t n = 0; n < clip.doors.size(); ++n)
        {
            const DoorTrack& track = clip.doors[n];
            // A one-sample track is the door's state for the whole clip, held
            // (door_rebind.h); skipping it as "never moved" would leave the live
            // door in its own state.
            const TrackPlay kind = ClassifyTrack(track.samples.size());
            if (kind == TrackPlay::Skip)
                continue;
            Played played;
            played.track = n;
            played.held = kind == TrackPlay::Held;
            if (played.held)
                ++g_totals.heldSingle;
            else
                ++g_totals.moving;
            played.registered = OBJECT::IS_DOOR_REGISTERED_WITH_SYSTEM(static_cast<Hash>(track.hash)) != FALSE;
            // A door missing now is kept and looked for again
            // (UpdatePlayback): the clear made it a dummy, the engine makes
            // it again later.
            if (Bind(played, ENTITY::_GET_ENTITY_BY_DOORHASH(static_cast<Hash>(track.hash), 0)))
            {
                played.everBound = true;
                ++g_totals.atStart;
            }
            else
                ++g_unresolved;
            ++g_totals.tracks;
            g_played.push_back(played);
        }
        log << "POSE_V2_PLAYBACK_DOORS,begin=" << g_totals.atStart << ",unresolved=" << g_unresolved
            << ",tracks=" << clip.doors.size() << ",played=" << g_totals.tracks << ",moving=" << g_totals.moving
            << ",held_single=" << g_totals.heldSingle << ",rebind_ms=" << g_config.rebindMs
            << ",keep_ratio=" << (g_config.keepOpenRatio && closedTheatre ? 1 : 0) << '\n';
    }

    std::uint32_t MissingNear(const Recording& clip, float x, float y, float z, float radiusM)
    {
        std::uint32_t missing = 0;
        if (g_config.rebindMs == 0)
            return 0; // never looked for again: nothing to wait for
        for (const Played& played : g_played)
        {
            if (played.handle && ENTITY::DOES_ENTITY_EXIST(played.handle))
                continue;
            if (played.track >= clip.doors.size() || clip.doors[played.track].samples.empty())
                continue;
            const float* at = clip.doors[played.track].samples.front().position;
            const float dx = at[0] - x, dy = at[1] - y, dz = at[2] - z;
            if (dx * dx + dy * dy + dz * dz <= radiusM * radiusM)
                ++missing;
        }
        return missing;
    }

    void UpdatePlayback(const Recording& clip, std::uint32_t playheadMs, std::ostream& log)
    {
        // Every rebindMs, the doors whose handle
        // is gone (or never was) are looked for by their door hash, as the
        // recording does; a door found is played in this same tick, before
        // its first frame renders in the map's own state.
        if (RebindDue(GetTickCount64(), g_nextRebindAt, g_config.rebindMs))
        {
            ++g_totals.scans;
            for (Played& played : g_played)
            {
                if (played.track >= clip.doors.size())
                    continue;
                const std::uint32_t hash = clip.doors[played.track].hash;
                if (played.handle && ENTITY::DOES_ENTITY_EXIST(played.handle))
                    continue;
                if (played.handle && !played.lostLogged)
                {
                    played.lostLogged = true;
                    ++g_totals.lost;
                    // Closed theater: the engine keeps the door system's target ratio for
                    // a door without an object (entry +0x5C) and applies it when the
                    // object is made again, so the door comes back as the replay last
                    // played it.
                    bool ratio = false;
                    if (g_config.keepOpenRatio && g_closedTheatre && played.registered && played.hasRatio)
                    {
                        OBJECT::DOOR_SYSTEM_SET_OPEN_RATIO(static_cast<Hash>(hash), played.lastRatio, FALSE);
                        ++g_totals.ratioWrites;
                        ratio = true;
                    }
                    LogDoor(log, hash, ratio ? "lost_ratio_kept" : "lost", playheadMs, played.handle);
                }
                const Entity handle = ENTITY::_GET_ENTITY_BY_DOORHASH(static_cast<Hash>(hash), 0);
                if (!Bind(played, handle))
                    continue;
                if (played.everBound)
                    ++g_totals.rebound;
                else
                    ++g_totals.late;
                LogDoor(log, hash, played.everBound ? "rebound" : "bound_late", playheadMs, handle);
                played.everBound = true;
            }
        }
        for (Played& played : g_played)
        {
            if (played.track >= clip.doors.size() || !played.handle || !ENTITY::DOES_ENTITY_EXIST(played.handle))
                continue;
            const std::vector<DoorSample>& samples = clip.doors[played.track].samples;
            if (samples.empty())
                continue;
            if (played.held)
            {
                // One state for the whole clip, placed on bind and refreshed.
                const std::uint64_t now = GetTickCount64();
                if (!HeldApplyDue(played.heldApplied, now, played.heldAppliedAt, kHeldRefreshMs))
                    continue;
                played.heldApplied = true;
                played.heldAppliedAt = now;
                ++g_totals.heldApplies;
            }
            // The sample in force and the next one: the rotation between them.
            std::size_t at = 0;
            while (at + 1 < samples.size() && samples[at + 1].timeMs <= playheadMs)
                ++at;
            float rot[3] = {samples[at].rotation[0], samples[at].rotation[1], samples[at].rotation[2]};
            if (at + 1 < samples.size() && playheadMs > samples[at].timeMs)
            {
                const DoorSample& next = samples[at + 1];
                const float span = static_cast<float>(next.timeMs - samples[at].timeMs);
                const float t = span > 0.0f ? (std::min)(1.0f, (playheadMs - samples[at].timeMs) / span) : 0.0f;
                if (next.timeMs - samples[at].timeMs <= 200) // consecutive frames of a swing
                {
                    for (int k = 0; k < 3; ++k)
                        rot[k] += AngleDelta(next.rotation[k], rot[k]) * t;
                }
            }
            ENTITY::FREEZE_ENTITY_POSITION(played.handle, TRUE);
            ENTITY::SET_ENTITY_ROTATION(played.handle, rot[0], rot[1], rot[2], 2, TRUE);
            if (g_config.keepOpenRatio && g_closedTheatre && played.registered)
            {
                // The engine's ratio of the door object as placed (with an object,
                // computed from it, 0x810A88): kept for a loss.
                played.lastRatio = OBJECT::DOOR_SYSTEM_GET_OPEN_RATIO(static_cast<Hash>(clip.doors[played.track].hash));
                played.hasRatio = std::isfinite(played.lastRatio);
            }
        }
    }

    void EndPlayback(std::ostream& log)
    {
        int restored = 0;
        for (const Played& played : g_played)
        {
            if (!ENTITY::DOES_ENTITY_EXIST(played.handle))
                continue;
            ENTITY::SET_ENTITY_ROTATION(played.handle, played.rotation.x, played.rotation.y, played.rotation.z, 2, TRUE);
            if (!played.wasFrozen)
                ENTITY::FREEZE_ENTITY_POSITION(played.handle, FALSE);
            ++restored;
        }
        if (!g_played.empty())
        {
            log << "POSE_V2_PLAYBACK_DOORS,end=" << restored << '\n';
            std::uint32_t missing = 0;
            for (const Played& played : g_played)
                missing += played.everBound ? 0u : 1u;
            // Kept with Debug=0.
            log << "POSE_V2_DOOR_REBIND_SUMMARY,tracks=" << g_totals.tracks << ",moving=" << g_totals.moving
                << ",held_single=" << g_totals.heldSingle << ",held_applies=" << g_totals.heldApplies << ",at_start=" << g_totals.atStart
                << ",bound_late=" << g_totals.late << ",rebound=" << g_totals.rebound << ",lost=" << g_totals.lost
                << ",never_found=" << missing << ",scans=" << g_totals.scans << ",ratio_writes=" << g_totals.ratioWrites
                << ",rebind_ms=" << g_config.rebindMs << '\n';
        }
        g_played.clear();
    }
}
