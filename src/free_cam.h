#pragma once

// The free camera's motion, pure (no natives): the classic flight, the
// Drone (momentum: thrust, drag, gravity, banking into turns, smoothed turn
// rates and zoom), the acrobatic rotation (a quaternion, turns about the
// camera's own axes: full rolls and loops, no gimbal stop), Walk (the ground
// at eye height), a locked altitude, collision (a sphere swept by the
// caller's shape test), follow a target (rigid or position only), the
// target under the crosshair or the nearest one, level the horizon and the
// shot behind the main character.
//
// The drone model, banking and quaternion flight follow Simple Camera
// (crxhvrd, MIT licence). The engine side (shape tests, the ground probe,
// the pad) stays in script.cpp; the world reaches this file as callbacks.
//
// Rotation as the engine's rotation order 2: x pitch, y roll, z heading, in
// degrees; heading counter-clockwise seen from above (0 faces +Y, 90 faces
// -X), as script.cpp RotationToDirection. The orientation matrix is
// Rz(heading) * Rx(pitch) * Ry(roll): its columns are the camera's right,
// forward and up in the world.

#include "editor_protocol.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace replay::freecam
{
    using editor::Vec3;

    struct Quat
    {
        float w = 1.0f;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // Orientation of a rotation (degrees, order 2) and back. Near a pitch of
    // +-90 the heading and roll are one axis: ToRotation keeps `previousRoll`
    // and puts the rest in the heading, so a loop never snaps the view.
    Quat FromRotation(const Vec3& rotation);
    Vec3 ToRotation(const Quat& orientation, float previousRoll = 0.0f);
    // The camera's axes in the world.
    Vec3 Forward(const Quat& orientation);
    Vec3 Right(const Quat& orientation);
    Vec3 Up(const Quat& orientation);
    Quat Multiply(const Quat& a, const Quat& b);
    Quat Normalize(const Quat& q);
    Quat AxisAngle(const Vec3& axis, float degrees);
    Vec3 Rotate(const Quat& q, const Vec3& v);
    Quat Conjugate(const Quat& q);

    enum class Motion : std::uint8_t
    {
        Classic = 0, // velocity eased to the wanted one, look smoothed
        Drone = 1    // momentum: thrust, drag, gravity, banking
    };

    enum class Follow : std::uint8_t
    {
        Off = 0,
        Rigid = 1,    // moves and turns with the target (a camera bolted on it)
        Translate = 2 // moves with the target, keeps its own world rotation
    };
    constexpr std::uint8_t kFollowCount = 3;

    struct Settings
    {
        Motion motion = Motion::Classic;
        bool acrobatic = false;     // quaternion rotation about the camera's own axes
        bool walk = false;          // the ground at walkHeight under the camera
        float walkHeight = 1.7f;    // metres
        bool lockAltitude = false;  // forward and right stay level; up/down still move
        bool collision = false;     // a sphere of collisionRadius against the world
        float collisionRadius = 0.35f;
        // Classic (script.cpp kFreeCam* values).
        float lookSensitivity = 10.0f; // degrees per unit of mouse normal
        float lookResponse = 18.0f;    // 1/s, the look smoothing
        float moveResponse = 7.0f;     // 1/s, the velocity easing
        float baseSpeed = 5.0f;        // m/s
        float fastSpeed = 15.0f;       // m/s with Shift
        float slowFactor = 0.25f;      // with Ctrl
        float speedScale = 1.0f;       // the wheel: 0.25 .. 4
        float turnRate = 75.0f;        // degrees/s, arrows and the pad's stick
        float rollRate = 37.5f;        // degrees/s
        float fovRate = 30.0f;         // degrees/s
        float fovMin = editor::kMarkerFovMin;
        float fovMax = editor::kMarkerFovMax;
        // Drone (Simple Camera's defaults).
        float droneDrag = 3.0f;          // 1/s, velocity damping
        float droneAcceleration = 15.0f; // m/s^2 at speed scale 1 (x2, x the speed's multiplier)
        float droneGravity = 0.0f;       // m/s^2 down while it does not thrust up
        float droneBanking = 15.0f;      // degrees of roll at 60 degrees/s of turn
        float droneRotSmoothing = 8.0f;  // 1/s, how fast the turn rates follow the input
        float droneFovSmoothing = 5.0f;  // 1/s, how fast the zoom rate follows the input
    };

    struct Input
    {
        // The mouse: normals of this frame (a delta, GET_DISABLED_CONTROL_NORMAL).
        float mouseX = 0.0f;
        float mouseY = 0.0f;
        // Rates, -1..1: the arrows and the pad's right stick (x: right, y: up).
        float turnX = 0.0f;
        float turnY = 0.0f;
        float roll = 0.0f;  // -1 left, +1 right
        float moveForward = 0.0f; // -1..1 each
        float moveRight = 0.0f;
        float moveUp = 0.0f;
        float zoom = 0.0f;  // +1 zooms in (lower FOV)
        bool fast = false;
        bool slow = false;
        bool analog = false; // the pad's sticks: the move keeps its length (keys are normalised)
    };

    struct State
    {
        Vec3 position{};
        Vec3 rotation{}; // degrees, order 2
        float fov = editor::kMarkerFovDefault;
        Vec3 velocity{};
        Quat orientation{};
        bool orientationValid = false; // false: taken from `rotation` at the next step
        float smoothedMouseX = 0.0f;
        float smoothedMouseY = 0.0f;
        // Drone: turn rates (degrees/s) and the zoom rate.
        float yawRate = 0.0f;
        float pitchRate = 0.0f;
        float rollRate = 0.0f;
        float fovRate = 0.0f;
        float bankRoll = 0.0f; // the roll the banking added (taken back when level)

        // The view was set from outside (a marker, the evaluated track,
        // Edit Camera): no momentum carries over, the orientation follows.
        void Reset();
        void SetView(const Vec3& at, const Vec3& rotationDegrees, float fovDegrees);
    };

    // The world, from the caller (script.cpp: shape tests and the ground).
    struct World
    {
        // A sphere of `radius` moved from `from` to `to`: true when it hits,
        // with the hit point and the surface normal.
        std::function<bool(const Vec3& from, const Vec3& to, float radius, Vec3& hit, Vec3& normal)> sweep;
        // The ground under (x, y) below z: true with its height.
        std::function<bool(float x, float y, float z, float& groundZ)> ground;
    };

    struct StepReport
    {
        bool collided = false;
        bool walked = false; // the ground was found and the camera put on it
        float speed = 0.0f;  // m/s after the step
    };

    // One frame of flight. `world` may be null (no collision, no Walk).
    StepReport Step(State& state, const Input& input, const Settings& settings, float dtSeconds, const World* world);

    // Edit Camera (a camera marker being framed) always flies level, whatever
    // the free camera's options: no acrobatic rotation (its turns about the
    // camera's own axes roll the horizon) and no Drone banking. The roll
    // changes only by the roll keys (the marker's own roll); the rest of the
    // options (Classic or Drone, Walk, collision, the speeds) stay.
    Settings LevelFlight(Settings settings);
    // A view kept at `roll` degrees (a Rigid follow turned it with the target):
    // the pitch and heading stay, the orientation follows.
    void KeepRoll(State& state, float roll);

    // Levels the horizon (roll 0, the acrobatic orientation too).
    void LevelHorizon(State& state);

    // A target's frame at an instant (rotation as GET_ENTITY_ROTATION order 2).
    struct TargetFrame
    {
        Vec3 position{};
        Vec3 rotation{}; // x pitch, y roll, z heading
    };
    // Carries the camera with the target from `before` to `now`: Translate
    // adds the target's movement; Rigid also turns the camera's offset and
    // orientation by the target's turn (the camera stays where it was on
    // the target). Off does nothing.
    void ApplyFollow(State& state, Follow mode, const TargetFrame& before, const TargetFrame& now);

    // A target the camera can pick (a puppet at the playhead).
    struct Candidate
    {
        std::uint32_t id = 0;
        Vec3 point{}; // the head, else the origin raised
    };
    // The candidate nearest the centre of the view, inside maxAngleDegrees of
    // the view axis and maxMeters away (ties: the nearer one); 0 when none.
    std::uint32_t PickAimed(const Vec3& position, const Vec3& rotation, const std::vector<Candidate>& candidates,
        float maxAngleDegrees = 12.0f, float maxMeters = 250.0f);
    // The nearest candidate inside maxMeters; 0 when none.
    std::uint32_t PickNearest(const Vec3& position, const std::vector<Candidate>& candidates, float maxMeters = 250.0f);

    // The shot behind a character: `back` metres behind its heading and
    // `up` above, looking at its head (or origin + 0.6 m).
    void ShotBehind(State& state, const TargetFrame& target, const Vec3* head, float back = 3.0f, float up = 1.2f);
}
