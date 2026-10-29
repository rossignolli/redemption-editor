#include "free_cam.h"

#include <algorithm>
#include <cmath>

namespace replay::freecam
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kDeg = kPi / 180.0f;

        Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
        Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
        Vec3 Scale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
        float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
        float Lerp(float a, float b, float t) { return a + (b - a) * t; }
        Vec3 LerpVec(const Vec3& a, const Vec3& b, float t) { return {Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t)}; }
        float Blend(float rate, float dt) { return 1.0f - std::exp(-rate * dt); }

        Vec3 Flatten(const Vec3& v)
        {
            const float length = std::sqrt(v.x * v.x + v.y * v.y);
            return length > 0.001f ? Vec3{v.x / length, v.y / length, 0.0f} : Vec3{};
        }

        // Turns about the camera's own axes (acrobatic): heading about its
        // up, pitch about its right, roll about its forward.
        void TurnLocal(State& state, float yawDegrees, float pitchDegrees, float rollDegrees)
        {
            Quat q = state.orientation;
            q = Multiply(q, AxisAngle({0.0f, 0.0f, 1.0f}, yawDegrees));
            q = Multiply(q, AxisAngle({1.0f, 0.0f, 0.0f}, pitchDegrees));
            q = Multiply(q, AxisAngle({0.0f, 1.0f, 0.0f}, rollDegrees));
            state.orientation = Normalize(q);
            state.rotation = ToRotation(state.orientation, state.rotation.y);
        }

        // A move of `delta` against the world (Simple Camera's three passes:
        // push out of the surface, slide along it).
        bool Collide(State& state, Vec3 delta, float radius, const World& world)
        {
            bool collided = false;
            for (int pass = 0; pass < 3; ++pass)
            {
                if (Length(delta) < 0.0005f)
                    return collided;
                Vec3 hit{}, normal{};
                if (!world.sweep(state.position, Add(state.position, delta), radius, hit, normal) || Length(normal) < 0.5f)
                {
                    state.position = Add(state.position, delta);
                    return collided;
                }
                collided = true;
                const float startDistance = Dot(Sub(state.position, hit), normal);
                if (startDistance < radius)
                    state.position = Add(state.position, Scale(normal, radius - startDistance + 0.0001f));
                Vec3 target = Add(state.position, delta);
                const float targetDistance = Dot(Sub(target, hit), normal);
                if (targetDistance < radius)
                    target = Add(target, Scale(normal, radius - targetDistance + 0.0001f));
                delta = Sub(target, state.position);
                const float into = Dot(state.velocity, normal);
                if (into < 0.0f)
                    state.velocity = Sub(state.velocity, Scale(normal, into));
            }
            return collided;
        }
    }

    Quat Multiply(const Quat& a, const Quat& b)
    {
        return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
    }

    Quat Normalize(const Quat& q)
    {
        const float length = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
        if (!(length > 1e-6f))
            return {};
        return {q.w / length, q.x / length, q.y / length, q.z / length};
    }

    Quat Conjugate(const Quat& q) { return {q.w, -q.x, -q.y, -q.z}; }

    Quat AxisAngle(const Vec3& axis, float degrees)
    {
        const float half = degrees * kDeg * 0.5f;
        const float s = std::sin(half);
        return {std::cos(half), axis.x * s, axis.y * s, axis.z * s};
    }

    Vec3 Rotate(const Quat& q, const Vec3& v)
    {
        const Quat p{0.0f, v.x, v.y, v.z};
        const Quat r = Multiply(Multiply(q, p), Conjugate(q));
        return {r.x, r.y, r.z};
    }

    Quat FromRotation(const Vec3& rotation)
    {
        // Rz(heading) * Rx(pitch) * Ry(roll), heading counter-clockwise.
        return Normalize(Multiply(Multiply(AxisAngle({0.0f, 0.0f, 1.0f}, rotation.z), AxisAngle({1.0f, 0.0f, 0.0f}, rotation.x)),
            AxisAngle({0.0f, 1.0f, 0.0f}, rotation.y)));
    }

    Vec3 ToRotation(const Quat& source, float previousRoll)
    {
        const Quat q = Normalize(source);
        // The matrix entries the decomposition needs (row, column).
        const float r01 = 2.0f * (q.x * q.y - q.w * q.z);
        const float r11 = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
        const float r21 = 2.0f * (q.y * q.z + q.w * q.x);
        const float r20 = 2.0f * (q.x * q.z - q.w * q.y);
        const float r22 = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
        const float r00 = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
        const float r10 = 2.0f * (q.x * q.y + q.w * q.z);
        Vec3 out{};
        out.x = std::asin(std::clamp(r21, -1.0f, 1.0f)) / kDeg;
        if (std::fabs(r21) < 0.9995f)
        {
            out.z = std::atan2(-r01, r11) / kDeg;
            out.y = std::atan2(-r20, r22) / kDeg;
        }
        else
        {
            // Straight up or down: heading and roll turn about the same
            // axis. Keep the roll, the heading takes the rest.
            out.y = previousRoll;
            const float combined = std::atan2(r10, r00) / kDeg; // heading + (or -) roll
            out.z = r21 > 0.0f ? combined - previousRoll : combined + previousRoll;
        }
        return out;
    }

    Vec3 Forward(const Quat& q) { return Rotate(q, {0.0f, 1.0f, 0.0f}); }
    Vec3 Right(const Quat& q) { return Rotate(q, {1.0f, 0.0f, 0.0f}); }
    Vec3 Up(const Quat& q) { return Rotate(q, {0.0f, 0.0f, 1.0f}); }

    void State::Reset()
    {
        velocity = {};
        smoothedMouseX = smoothedMouseY = 0.0f;
        yawRate = pitchRate = rollRate = fovRate = 0.0f;
        bankRoll = 0.0f;
        orientationValid = false;
    }

    void State::SetView(const Vec3& at, const Vec3& rotationDegrees, float fovDegrees)
    {
        position = at;
        rotation = rotationDegrees;
        fov = fovDegrees;
        Reset();
    }

    StepReport Step(State& state, const Input& input, const Settings& settings, float dt, const World* world)
    {
        StepReport report;
        if (!(dt > 0.0f))
            return report;
        if (!state.orientationValid)
        {
            state.orientation = FromRotation(state.rotation);
            state.orientationValid = true;
        }
        const bool drone = settings.motion == Motion::Drone;

        // ---- Rotation.
        // The mouse is a delta of this frame; the arrows and the stick a rate.
        const float mouseBlend = Blend(settings.lookResponse, dt);
        state.smoothedMouseX = Lerp(state.smoothedMouseX, input.mouseX, mouseBlend);
        state.smoothedMouseY = Lerp(state.smoothedMouseY, input.mouseY, mouseBlend);
        float yawDelta = 0.0f, pitchDelta = 0.0f, rollDelta = 0.0f;
        if (drone)
        {
            // Turn rates eased to the wanted ones: the turns have weight.
            const float wantYaw = -input.mouseX * settings.lookSensitivity / dt - input.turnX * settings.turnRate;
            const float wantPitch = -input.mouseY * settings.lookSensitivity / dt + input.turnY * settings.turnRate;
            const float wantRoll = input.roll * settings.rollRate;
            const float k = Blend(settings.droneRotSmoothing, dt);
            state.yawRate = Lerp(state.yawRate, wantYaw, k);
            state.pitchRate = Lerp(state.pitchRate, wantPitch, k);
            state.rollRate = Lerp(state.rollRate, wantRoll, k);
            yawDelta = state.yawRate * dt;
            pitchDelta = state.pitchRate * dt;
            rollDelta = state.rollRate * dt;
        }
        else
        {
            yawDelta = -state.smoothedMouseX * settings.lookSensitivity - input.turnX * settings.turnRate * dt;
            pitchDelta = -state.smoothedMouseY * settings.lookSensitivity + input.turnY * settings.turnRate * dt;
            rollDelta = input.roll * settings.rollRate * dt;
        }

        // The Drone banks into its turns (a roll the turn rate asks for,
        // eased; taken back when it flies straight). Kept apart from the
        // user's own roll.
        float bankDelta = 0.0f;
        if (drone)
        {
            const float wantBank = -state.yawRate * (settings.droneBanking / 60.0f);
            const float next = Lerp(state.bankRoll, std::clamp(wantBank, -60.0f, 60.0f), Blend(4.0f, dt));
            bankDelta = next - state.bankRoll;
            state.bankRoll = next;
        }

        if (settings.acrobatic)
        {
            TurnLocal(state, yawDelta, pitchDelta, rollDelta + bankDelta);
        }
        else
        {
            state.rotation.z += yawDelta;
            state.rotation.x += pitchDelta;
            state.rotation.y += rollDelta + bankDelta;
            state.rotation.x = std::clamp(state.rotation.x, -89.0f, 89.0f);
            state.orientation = FromRotation(state.rotation);
        }

        // ---- Movement axes.
        Vec3 forward{}, right{}, up{0.0f, 0.0f, 1.0f};
        if (settings.acrobatic)
        {
            forward = Forward(state.orientation);
            right = Right(state.orientation);
            up = Up(state.orientation);
        }
        else
        {
            // script.cpp RotationToDirection, with a level right vector.
            const float pitch = state.rotation.x * kDeg, heading = state.rotation.z * kDeg;
            const float horizontal = std::fabs(std::cos(pitch));
            forward = {-std::sin(heading) * horizontal, std::cos(heading) * horizontal, std::sin(pitch)};
            right = {std::cos(heading), std::sin(heading), 0.0f};
        }
        if (settings.lockAltitude)
        {
            forward = Flatten(forward);
            right = Flatten(right);
            up = {0.0f, 0.0f, 1.0f};
        }
        Vec3 direction = Add(Add(Scale(forward, input.moveForward), Scale(right, input.moveRight)), Scale(up, input.moveUp));
        const float directionLength = Length(direction);
        // Keys give unit steps (normalised); a stick pushed half way moves
        // at half speed.
        if (directionLength > 0.0001f && (!input.analog || directionLength > 1.0f))
            direction = Scale(direction, 1.0f / directionLength);
        const float multiplier = (input.fast ? 3.0f : 1.0f) * (input.slow ? settings.slowFactor : 1.0f) * settings.speedScale;

        Vec3 delta{};
        if (drone)
        {
            const float acceleration = settings.droneAcceleration * multiplier * 2.0f;
            state.velocity = Add(state.velocity, Scale(direction, acceleration * dt));
            if (input.moveUp <= 0.0f)
                state.velocity.z -= settings.droneGravity * dt;
            state.velocity = Scale(state.velocity, std::exp(-settings.droneDrag * dt));
            delta = Scale(state.velocity, dt);
        }
        else
        {
            const float targetSpeed = (input.fast ? settings.fastSpeed : settings.baseSpeed) *
                (input.slow ? settings.slowFactor : 1.0f) * settings.speedScale;
            state.velocity = LerpVec(state.velocity, Scale(direction, targetSpeed), Blend(settings.moveResponse, dt));
            delta = Scale(state.velocity, dt);
        }
        if (settings.lockAltitude && input.moveUp == 0.0f)
        {
            delta.z = 0.0f;
            state.velocity.z = 0.0f;
        }
        if (settings.collision && world && world->sweep)
            report.collided = Collide(state, delta, settings.collisionRadius, *world);
        else
            state.position = Add(state.position, delta);

        // Walk: the ground under the camera at eye height (after the move,
        // so it wins over the Drone's gravity and the locked altitude).
        if (settings.walk && world && world->ground)
        {
            float groundZ = 0.0f;
            if (world->ground(state.position.x, state.position.y, state.position.z + 50.0f, groundZ))
            {
                state.position.z = groundZ + settings.walkHeight;
                state.velocity.z = 0.0f;
                report.walked = true;
            }
        }

        // ---- Zoom.
        const float wantFovRate = -input.zoom * settings.fovRate;
        if (drone && settings.droneFovSmoothing > 0.0f)
            state.fovRate = Lerp(state.fovRate, wantFovRate, Blend(settings.droneFovSmoothing, dt));
        else
            state.fovRate = wantFovRate;
        state.fov = std::clamp(state.fov + state.fovRate * dt, settings.fovMin, settings.fovMax);
        if (state.fov == settings.fovMin || state.fov == settings.fovMax)
            state.fovRate = 0.0f;

        report.speed = Length(state.velocity);
        return report;
    }

    Settings LevelFlight(Settings settings)
    {
        settings.acrobatic = false;
        settings.droneBanking = 0.0f;
        return settings;
    }

    void KeepRoll(State& state, float roll)
    {
        const Vec3 rotation = state.orientationValid ? ToRotation(state.orientation, state.rotation.y) : state.rotation;
        state.rotation = {std::clamp(rotation.x, -89.0f, 89.0f), roll, rotation.z};
        state.orientation = FromRotation(state.rotation);
        state.orientationValid = true;
        state.bankRoll = 0.0f;
    }

    void LevelHorizon(State& state)
    {
        const Vec3 rotation = state.orientationValid ? ToRotation(state.orientation, state.rotation.y) : state.rotation;
        state.rotation = {std::clamp(rotation.x, -89.0f, 89.0f), 0.0f, rotation.z};
        state.orientation = FromRotation(state.rotation);
        state.orientationValid = true;
        state.bankRoll = 0.0f;
        state.rollRate = 0.0f;
    }

    void ApplyFollow(State& state, Follow mode, const TargetFrame& before, const TargetFrame& now)
    {
        if (mode == Follow::Off)
            return;
        if (mode == Follow::Translate)
        {
            state.position = Add(state.position, Sub(now.position, before.position));
            return;
        }
        if (!state.orientationValid)
        {
            state.orientation = FromRotation(state.rotation);
            state.orientationValid = true;
        }
        const Quat turn = Normalize(Multiply(FromRotation(now.rotation), Conjugate(FromRotation(before.rotation))));
        state.position = Add(now.position, Rotate(turn, Sub(state.position, before.position)));
        state.velocity = Rotate(turn, state.velocity);
        state.orientation = Normalize(Multiply(turn, state.orientation));
        state.rotation = ToRotation(state.orientation, state.rotation.y);
    }

    std::uint32_t PickAimed(const Vec3& position, const Vec3& rotation, const std::vector<Candidate>& candidates,
        float maxAngleDegrees, float maxMeters)
    {
        const Vec3 forward = Forward(FromRotation(rotation));
        std::uint32_t best = 0;
        float bestAngle = maxAngleDegrees;
        float bestDistance = 0.0f;
        for (const Candidate& candidate : candidates)
        {
            const Vec3 to = Sub(candidate.point, position);
            const float distance = Length(to);
            if (distance < 0.3f || distance > maxMeters || candidate.id == 0)
                continue;
            const float angle = std::acos(std::clamp(Dot(to, forward) / distance, -1.0f, 1.0f)) / kDeg;
            if (angle < bestAngle - 0.01f || (std::fabs(angle - bestAngle) <= 0.01f && best && distance < bestDistance))
            {
                best = candidate.id;
                bestAngle = angle;
                bestDistance = distance;
            }
        }
        return best;
    }

    std::uint32_t PickNearest(const Vec3& position, const std::vector<Candidate>& candidates, float maxMeters)
    {
        std::uint32_t best = 0;
        float bestDistance = maxMeters;
        for (const Candidate& candidate : candidates)
        {
            const float distance = Length(Sub(candidate.point, position));
            if (candidate.id != 0 && distance <= bestDistance)
            {
                best = candidate.id;
                bestDistance = distance;
            }
        }
        return best;
    }

    void ShotBehind(State& state, const TargetFrame& target, const Vec3* head, float back, float up)
    {
        const float heading = target.rotation.z * kDeg;
        const Vec3 facing{-std::sin(heading), std::cos(heading), 0.0f};
        const Vec3 look = head ? *head : Vec3{target.position.x, target.position.y, target.position.z + 0.6f};
        state.position = {target.position.x - facing.x * back, target.position.y - facing.y * back, target.position.z + up};
        const Vec3 to = Sub(look, state.position);
        const float horizontal = std::sqrt(to.x * to.x + to.y * to.y);
        state.rotation = {std::atan2(to.z, horizontal) / kDeg, 0.0f, std::atan2(-to.x, to.y) / kDeg};
        state.Reset();
        state.orientation = FromRotation(state.rotation);
        state.orientationValid = true;
    }
}
