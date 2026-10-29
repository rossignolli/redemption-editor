// The pure logic behind playback fixes: the marker speed cascade against
// the clip's end, the fine scrub snap order, one start path per recorded
// fire and the per entity track index of the samplers.
#include "camera_track.h"
#include "fire_observer.h"
#include "track_index.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    using namespace replay;

    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    camera::Keyframe MarkerAt(std::uint64_t timeMs)
    {
        camera::Keyframe k;
        k.timeMs = timeMs;
        k.cameraMode = camera::CameraMode::Free;
        k.pose.fov = 45.0f;
        return k;
    }

    std::vector<std::uint64_t> Times(const camera::Track& track)
    {
        std::vector<std::uint64_t> out;
        for (const camera::Keyframe& key : track.Keyframes())
            out.push_back(key.timeMs);
        return out;
    }

    bool Spaced(const camera::Track& track)
    {
        const std::vector<camera::Keyframe>& keys = track.Keyframes();
        for (std::size_t n = 0; n + 1 < keys.size(); ++n)
            if (keys[n + 1].timeMs < keys[n].timeMs + camera::MarkerSpacingMs(keys[n].speedIndex))
                return false;
        return true;
    }

    // Marker speed cascade: a 1000 ms clip, markers at 934, 967 and 1000,
    // the spacing after 934 going from 33 to 66 ms.
    void TestSpeedCascadeAtEnd()
    {
        camera::Track track;
        track.AddMarker(MarkerAt(0));
        const std::uint32_t a = track.AddMarker(MarkerAt(934));
        track.AddMarker(MarkerAt(967));
        track.AddMarker(MarkerAt(1000));
        const std::vector<std::uint64_t> before = Times(track);
        editor::CameraKeyframeOptions speed{};
        speed.mask = editor::CameraOptionSpeed;
        speed.speedIndex = 8; // 200 %: 66 ms after it
        Check(!track.SetOptions(a, speed, 1000), "cascade: a speed that pushes the end past the clip is refused");
        Check(Times(track) == before && track.Find(a)->speedIndex == editor::kMarkerSpeedDefaultIndex,
            "cascade: a refused speed changes nothing (times and speed)");

        camera::Track free = track;
        Check(free.SetOptions(a, speed), "cascade: without a limit the speed is taken (the old behaviour)");
        Check(Spaced(free) && free.Keyframes().back().timeMs == 1033, "cascade: without a limit the markers are pushed");

        camera::Track room;
        room.AddMarker(MarkerAt(0));
        const std::uint32_t b = room.AddMarker(MarkerAt(900));
        room.AddMarker(MarkerAt(934));
        room.AddMarker(MarkerAt(1000));
        speed.speedIndex = 6; // 150 %: 50 ms
        Check(room.SetOptions(b, speed, 1000) && Spaced(room) && Times(room) == std::vector<std::uint64_t>{0, 900, 950, 1000},
            "cascade: a push that fits in the clip is taken, spacing kept, end in place");

        camera::Track tight;
        tight.AddMarker(MarkerAt(0));
        const std::uint32_t c = tight.AddMarker(MarkerAt(950));
        tight.AddMarker(MarkerAt(1000));
        speed.speedIndex = 8;
        Check(!tight.SetOptions(c, speed, 1000) && Times(tight) == std::vector<std::uint64_t>{0, 950, 1000},
            "cascade: an end at the limit is never pushed past it");
        camera::Track legacyEnd;
        legacyEnd.AddMarker(MarkerAt(0));
        const std::uint32_t d = legacyEnd.AddMarker(MarkerAt(500));
        legacyEnd.AddMarker(MarkerAt(1200));
        Check(legacyEnd.SetOptions(d, speed, 1000) && Times(legacyEnd) == std::vector<std::uint64_t>{0, 500, 1200},
            "cascade: an end already past the limit that the push does not reach does not block the change");

        camera::Track paste = track;
        camera::Keyframe fast = MarkerAt(0);
        fast.speedIndex = 8;
        Check(!paste.PasteMarker(a, fast, editor::CopyContext::All, 1000) && Times(paste) == before &&
                paste.Find(a)->speedIndex == editor::kMarkerSpeedDefaultIndex,
            "cascade: a paste that does not fit is refused whole");
    }

    // Fine scrub: a step backward from 1000 to 900 crosses 980 first.
    void TestScrubSnapOrder()
    {
        camera::Track track;
        track.AddMarker(MarkerAt(0));
        track.AddMarker(MarkerAt(920));
        track.AddMarker(MarkerAt(980));
        track.AddMarker(MarkerAt(2000));
        const camera::Keyframe* back = camera::FirstMarkerCrossed(track, 1000, 900);
        const camera::Keyframe* ahead = camera::FirstMarkerCrossed(track, 900, 1000);
        Check(back && back->timeMs == 980, "snap: going back, the first marker crossed is the latest below");
        Check(ahead && ahead->timeMs == 920, "snap: going forward, the earliest above");
        Check(!camera::FirstMarkerCrossed(track, 980, 920) && !camera::FirstMarkerCrossed(track, 920, 980),
            "snap: markers at the ends of the step are not crossed");
        Check(!camera::FirstMarkerCrossed(track, 1000, 1000), "snap: no step, no marker");
        const camera::Keyframe* from = camera::FirstMarkerCrossed(track, 981, 919);
        Check(from && from->timeMs == 980, "snap: one past a marker going back still stops on it");
    }

    // Fire start: a start refused and queued for a retry, then left ended in
    // the playhead by a rebuild: one list only, and no second fire once it
    // runs.
    struct Fire
    {
        std::uint32_t start = 0;
        bool alive = false;
    };

    void TestFireStartOnce()
    {
        std::vector<std::uint32_t> pending{7};
        std::vector<std::uint32_t> ended;
        Check(!fire::QueueStartOnce(ended, pending, 7) && ended.empty(),
            "fire once: a start waiting for its retry is not also queued as ended");
        Check(fire::QueueStartOnce(ended, pending, 9) && ended == std::vector<std::uint32_t>{9},
            "fire once: another ended start is queued");
        Check(!fire::QueueStartOnce(ended, pending, 9) && ended.size() == 1, "fire once: never twice in the ended queue");
        Check(!fire::QueueStartOnce(pending, ended, 9) && pending.size() == 1,
            "fire once: an ended start in the queue is not also put in the retry list");

        std::vector<Fire> ours{{3, false}, {7, true}};
        Check(fire::LiveStart(ours, 7), "fire once: a start with a live fire of ours is owned");
        Check(!fire::LiveStart(ours, 3), "fire once: an ended fire of ours does not own its start (a seek may start it again)");
        Check(!fire::LiveStart(ours, 9), "fire once: a start without a fire is free");
    }

    // Track index: finds what find_if finds, follows appends, and a
    // replaced vector costs a rebuild, never a wrong track.
    struct Track
    {
        std::uint32_t id = 0;
        int value = 0;
    };

    void TestTrackIndex()
    {
        std::vector<Track> tracks{{10, 1}, {11, 2}, {12, 3}};
        TrackIndex<Track> index;
        Check(index.Find(tracks, 11) && index.Find(tracks, 11)->value == 2, "track index: found");
        Check(!index.Find(tracks, 99), "track index: an id without a track");
        tracks.push_back({13, 4});
        Check(index.Find(tracks, 13) && index.Find(tracks, 13)->value == 4, "track index: an appended track");
        const std::size_t rebuilds = index.Rebuilds();
        for (std::uint32_t n = 0; n < 1000; ++n)
            tracks.push_back({100 + n, static_cast<int>(n)});
        Check(index.Find(tracks, 1099) && index.Find(tracks, 1099)->value == 999 && index.Rebuilds() == rebuilds,
            "track index: appends are indexed without a rebuild (reallocation included)");
        tracks.clear();
        tracks.push_back({12, 30});
        Check(index.Find(tracks, 12) && index.Find(tracks, 12)->value == 30 && !index.Find(tracks, 11),
            "track index: a new recording's tracks, not the old ones");
        tracks = {{5, 1}, {6, 2}, {12, 3}};
        Check(index.Find(tracks, 5) && index.Find(tracks, 5)->value == 1 && index.Find(tracks, 12)->value == 3,
            "track index: a replaced vector is indexed again");
        tracks.erase(tracks.begin() + 1);
        Check(index.Find(tracks, 12) && index.Find(tracks, 12)->value == 3 && !index.Find(tracks, 6),
            "track index: an erased track moves the others, found at their new place");
        std::vector<Track> twice{{7, 1}, {7, 2}};
        TrackIndex<Track> first;
        Check(first.Find(twice, 7)->value == 1, "track index: the first entry of an id, like find_if");
    }
}

int RunReplayFixTests()
{
    TestSpeedCascadeAtEnd();
    TestScrubSnapOrder();
    TestFireStartOnce();
    TestTrackIndex();
    if (g_failures)
        std::fprintf(stderr, "%d replay fix test(s) failed.\n", g_failures);
    return g_failures;
}
