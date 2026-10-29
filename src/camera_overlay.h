#pragma once

// What the world overlay shows (pure, no natives): the replay's markers as
// keyframe spheres with a look arrow, the camera path as a line with time
// labels, the locked entity with a marker and the recorded vehicle's path:
//
//   BuildPath   the marker curve sampled in the world (the camera's position
//               at each instant of every segment that blends, from
//               EvaluateMarkersRaw), each marker's own view and its state.
//               Built when the markers change (it costs up to kMaxPathPoints
//               evaluations), not every tick.
//   BuildTrail  a target's recorded path around the playhead.
//   Project     both, seen from the camera placed this tick, into the
//               overlay channel's frame (clipped at the near plane).

#include "camera_overlay_channel.h"
#include "camera_track.h"

#include <cstdint>
#include <vector>

namespace replay::camera::overlay
{
    constexpr std::size_t kMaxPathPoints = 2400;
    constexpr std::uint32_t kMinSampleMs = 33;
    constexpr std::uint32_t kMaxSampleMs = 250;
    constexpr float kNearMeters = 0.15f;
    constexpr float kArrowMeters = 0.9f;
    constexpr float kMaxDrawMeters = 1500.0f;
    constexpr float kMaxAspect = 2.6f; // the widest screen kept (21:9 is 2.37)

    struct PathPoint
    {
        editor::Vec3 at{};
        std::uint32_t timeMs = 0;
        std::uint32_t sourceId = 0; // the segment's source marker
        bool spline = false;
        bool breakBefore = false;   // not joined to the point before
    };

    struct MarkerPoint
    {
        std::uint32_t id = 0;
        std::uint32_t timeMs = 0;
        editor::Vec3 at{};
        editor::Vec3 forward{};
        std::uint16_t flags = 0; // overlaychannel flags (Attached, Anchor, NotFree, ClipBound)
    };

    struct WorldPath
    {
        std::vector<PathPoint> points;
        std::vector<MarkerPoint> markers;
        std::uint32_t evaluations = 0; // what the build cost
    };

    WorldPath BuildPath(const Track& track, const Scene& scene);

    struct TrailPoint
    {
        editor::Vec3 at{};
        std::uint32_t timeMs = 0;
        bool breakBefore = false; // the target was not there just before
    };

    // The target's recorded position (its head, else origin + 0.6 m) from
    // fromMs to toMs every stepMs.
    std::vector<TrailPoint> BuildTrail(const EntityLookup& lookup, std::uint32_t target, std::uint64_t fromMs,
        std::uint64_t toMs, std::uint32_t stepMs = 100);

    struct ProjectSettings
    {
        std::uint64_t playheadMs = 0;
        std::uint32_t editingId = 0;    // the marker in Edit Camera
        std::uint32_t currentId = 0;    // the marker the playhead is on
        std::uint32_t labelEveryMs = 1000; // 0: no time labels
        bool dim = false;
        bool showCamera = false;        // the marker camera at the playhead (when another camera renders)
        editor::Vec3 cameraAt{};        // where it is
        std::uint32_t targetId = 0;     // the free camera's target (a dot over it)
        bool targetValid = false;
        editor::Vec3 targetAt{};
    };

    struct ProjectReport
    {
        std::uint32_t lines = 0;
        std::uint32_t dots = 0;
        std::uint32_t labels = 0;
        std::uint32_t dropped = 0; // over the channel's capacity
    };

    // The point in camera space (x right, y forward, z up).
    editor::Vec3 ToCamera(const editor::CameraState& camera, const editor::Vec3& point);
    // A segment seen from the camera, clipped at kNearMeters; false when it
    // is all behind or farther than kMaxDrawMeters. Screen units of the
    // picture's height (y -1..1 up, x -aspect..aspect right): the vertical
    // FOV is all it needs (ProjectToScreen's x times the aspect).
    bool ProjectSegment(const editor::CameraState& camera, const editor::Vec3& a, const editor::Vec3& b,
        editor::Vec2& sa, editor::Vec2& sb);

    ProjectReport Project(const WorldPath& path, const std::vector<TrailPoint>& trail, const editor::CameraState& camera,
        const ProjectSettings& settings, overlaychannel::Frame& frame);
}
