// camera_track_markers.cpp: a marker's depth of field (focus plan, blend, smoothing), its attach
// and look-at extras, the prefetch plan and the marker tools (retime, move, duplicate, distribute, lock).
// Split from camera_track.cpp (moved code); shared internals in camera_track_internal.h.

#include "camera_track_internal.h"

namespace replay::camera
{

// Depth of field of a marker (focus source, f-stop, focus depth).
    const char* FocusSourceName(FocusSource source)
    {
        switch (source)
        {
        case FocusSource::Default: return "default";
        case FocusSource::Sharp: return "none";
        case FocusSource::Manual: return "manual";
        case FocusSource::LookAt: return "look_at";
        case FocusSource::Attach: return "attach";
        case FocusSource::Target: return "target";
        case FocusSource::MainCharacter: return "main";
        case FocusSource::Engine: return "engine";
        case FocusSource::Held: return "held";
        }
        return "?";
    }

    float DofFStop(float intensity)
    {
        const float amount = std::max(0.0f, std::min(100.0f, intensity)) / 100.0f;
        return kDofSharpFStop * std::pow(0.5f / kDofSharpFStop, amount);
    }

    float FocusDepth(const editor::CameraState& camera, const editor::Vec3& point)
    {
        const Vec3 delta = Sub(point, camera.position);
        const Vec3 local = Mul(Transpose(FromEuler(camera.rotation)), delta);
        float depth = local.y;
        if (!(depth > kFocusMinMeters))
            depth = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        if (!std::isfinite(depth))
            depth = kFocusMinMeters;
        return std::max(kFocusMinMeters, std::min(kFocusMaxMeters, depth));
    }

    FocusPlan PlanFocus(const Keyframe* marker, const editor::CameraState& camera, const Scene& scene, std::uint64_t timeMs)
    {
        return PlanFocus(marker, camera, scene, timeMs, nullptr, FocusConfig{});
    }

    void FocusMemory::ResetShot()
    {
        hold[0] = FocusHold{};
        hold[1] = FocusHold{};
        hasDistance = false;
        lastDistance = 10.0f;
        springPrimed = false;
        springValue = 0.0f;
        springVelocity = 0.0f;
        lastSource = FocusSource::Default;
    }

    FocusHold& FocusMemory::HoldFor(std::uint32_t markerId)
    {
        if (hold[0].marker == markerId)
            return hold[0];
        if (hold[1].marker == markerId)
            return hold[1];
        // The blend's two markers: a new one pushes out the oldest.
        if (hold[0].marker == 0)
        {
            hold[0] = FocusHold{};
            hold[0].marker = markerId;
            return hold[0];
        }
        if (hold[1].marker != 0)
            hold[0] = hold[1];
        hold[1] = FocusHold{};
        hold[1].marker = markerId;
        return hold[1];
    }

    namespace
    {

        bool HasDistance(FocusSource source)
        {
            return source == FocusSource::Manual || source == FocusSource::LookAt || source == FocusSource::Attach ||
                source == FocusSource::Target || source == FocusSource::MainCharacter || source == FocusSource::Held;
        }

        void Remember(FocusMemory* memory, const FocusPlan& plan)
        {
            if (!memory)
                return;
            if (HasDistance(plan.source))
            {
                memory->hasDistance = true;
                memory->lastDistance = plan.distance;
            }
        }
    }

    FocusPlan PlanFocus(const Keyframe* marker, const editor::CameraState& camera, const Scene& scene, std::uint64_t timeMs,
        FocusMemory* memory, const FocusConfig& config)
    {
        FocusPlan plan;
        if (!marker || marker->dof == DofMode::Default)
            return plan;
        if (marker->dof == DofMode::None)
        {
            plan.source = FocusSource::Sharp;
            return plan;
        }
        // The marker's own f-stop and lens when set (the intensity stays the
        // shortcut that maps to both).
        plan.fstop = marker->dofFStop > 0.0f ? marker->dofFStop : DofFStop(marker->dofIntensity);
        plan.lensMm = marker->dofLensMm;
        plan.intensity = marker->dofIntensity;
        if (marker->focus == FocusMode::Manual)
        {
            plan.source = FocusSource::Manual;
            plan.distance = std::max(kFocusMinMeters, std::min(kFocusMaxMeters, marker->focusDistance));
            plan.straightMeters = plan.distance;
            Remember(memory, plan);
            return plan;
        }
        const bool hysteresis = memory && config.hysteresis;
        const auto take = [&](std::uint32_t id, FocusSource source, const EntityState& state) {
            const Vec3 point = AimPoint(state);
            const Vec3 delta = Sub(point, camera.position);
            plan.source = source;
            plan.entity = id;
            plan.point = point;
            plan.distance = FocusDepth(camera, point);
            plan.straightMeters = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
            Remember(memory, plan);
            return true;
        };
        const auto focusOn = [&](std::uint32_t id, FocusSource source, bool onScreenOnly) {
            EntityState state;
            if (!id || !scene.lookup || !scene.lookup(id, timeMs, state))
                return false;
            if (onScreenOnly)
            {
                editor::Vec2 screen{};
                if (!ProjectToScreen(camera, AimPoint(state), scene.aspect, screen) || std::fabs(screen.x) > 1.0f ||
                    std::fabs(screen.y) > 1.0f)
                    return false;
            }
            return take(id, source, state);
        };
        // The main character in Auto, with the hysteresis (FocusConfig).
        const auto mainInFrame = [&]() {
            const std::uint32_t id = scene.mainCharacter;
            if (!hysteresis)
                return focusOn(id, FocusSource::MainCharacter, true);
            FocusHold& hold = memory->HoldFor(marker->id);
            EntityState state;
            const bool found = id && scene.lookup && scene.lookup(id, timeMs, state);
            editor::Vec2 screen{};
            const bool projected = found && ProjectToScreen(camera, AimPoint(state), scene.aspect, screen);
            const bool inside = projected && std::fabs(screen.x) < config.enter && std::fabs(screen.y) < config.enter;
            const bool within = projected && std::fabs(screen.x) <= config.leave && std::fabs(screen.y) <= config.leave;
            if (!hold.locked)
            {
                if (inside)
                {
                    hold.locked = true;
                    hold.outside = false;
                }
            }
            else if (within)
            {
                hold.outside = false;
            }
            else
            {
                if (!hold.outside || timeMs < hold.outsideSinceMs)
                {
                    hold.outside = true;
                    hold.outsideSinceMs = timeMs;
                }
                if (timeMs - hold.outsideSinceMs >= config.graceMs)
                {
                    hold.locked = false;
                    hold.outside = false;
                }
            }
            return hold.locked && found && take(id, FocusSource::MainCharacter, state);
        };
        if (marker->focus == FocusMode::Target)
        {
            // The focus target list starts at the player (index 0); an unset
            // target (0) is the main character.
            const std::uint32_t id = marker->focusTarget ? marker->focusTarget : scene.mainCharacter;
            if (focusOn(id, FocusSource::Target, false))
                return plan;
            plan.targetMissing = id != 0;
        }
        else
        {
            // A Look At on a bone focuses that bone.
            EntityState lookState;
            if (marker->lookAtTarget && marker->lookAtBone && LookupLookAt(*marker, timeMs, scene, lookState) &&
                take(marker->lookAtTarget, FocusSource::LookAt, lookState))
                return plan;
            if (marker->lookAtTarget && focusOn(marker->lookAtTarget, FocusSource::LookAt, false))
                return plan;
            plan.targetMissing = marker->lookAtTarget != 0;
            if (marker->attachTarget && focusOn(marker->attachTarget, FocusSource::Attach, false))
                return plan;
            if (mainInFrame())
                return plan;
        }
        plan.entity = 0;
        if (hysteresis && config.holdLast && memory->hasDistance)
        {
            plan.source = FocusSource::Held;
            plan.distance = memory->lastDistance;
            plan.straightMeters = plan.distance;
            return plan;
        }
        plan.source = FocusSource::Engine;
        return plan;
    }

    BlendedFocus BlendFocus(const Track& track, const SegmentPlan& plan, const editor::CameraState& camera, const Scene& scene,
        std::uint64_t timeMs, FocusMemory* memory, const FocusConfig& config)
    {
        BlendedFocus out;
        const std::vector<Keyframe>& keys = track.Keyframes();
        const int count = static_cast<int>(keys.size());
        const Keyframe* from = plan.valid && plan.dofIndex >= 0 && plan.dofIndex < count ? &keys[plan.dofIndex] : nullptr;
        const FocusPlan a = PlanFocus(from, camera, scene, timeMs, memory, config);
        out.plan = a;
        out.from = out.to = a.source;
        if (!config.blend || !from || !plan.blending || plan.dofNextIndex < 0 || plan.dofNextIndex >= count ||
            plan.dofNextIndex == plan.dofIndex)
            return out;
        const FocusPlan b = PlanFocus(&keys[plan.dofNextIndex], camera, scene, timeMs, memory, config);
        out.to = b.source;
        if (a.source == FocusSource::Default && b.source == FocusSource::Default)
            return out;
        const float t = Clamp01(plan.phase);
        out.blended = true;
        out.phase = t;
        const bool sharpA = a.source == FocusSource::Default || a.source == FocusSource::Sharp;
        const bool sharpB = b.source == FocusSource::Default || b.source == FocusSource::Sharp;
        const float fstopA = sharpA ? kDofSharpFStop : a.fstop;
        const float fstopB = sharpB ? kDofSharpFStop : b.fstop;
        const bool distA = HasDistance(a.source), distB = HasDistance(b.source);
        FocusPlan r = t < 0.5f ? a : b;
        r.fstop = std::exp(Lerp(std::log(std::max(0.01f, fstopA)), std::log(std::max(0.01f, fstopB)), t));
        // The lens numbers through the blend (an automatic lens
        // on one side takes the other's when it has one).
        r.intensity = Lerp(sharpA ? b.intensity : a.intensity, sharpB ? a.intensity : b.intensity, t);
        if (a.lensMm > 0.0f && b.lensMm > 0.0f)
            r.lensMm = Lerp(a.lensMm, b.lensMm, t);
        if (!distA && !distB)
        {
            // Nothing to focus on either side: the autofocus if a side asks
            // for it, else sharp.
            r.source = a.source == FocusSource::Engine || b.source == FocusSource::Engine ? FocusSource::Engine
                                                                                            : FocusSource::Sharp;
            out.plan = r;
            return out;
        }
        const float dA = distA ? a.distance : b.distance;
        const float dB = distB ? b.distance : a.distance;
        r.distance = std::max(kFocusMinMeters, std::min(kFocusMaxMeters, Lerp(dA, dB, t)));
        r.straightMeters = Lerp(distA ? a.straightMeters : b.straightMeters, distB ? b.straightMeters : a.straightMeters, t);
        if (!HasDistance(r.source))
        {
            const FocusPlan& other = HasDistance(a.source) ? a : b;
            r.source = other.source;
            r.entity = other.entity;
            r.point = other.point;
        }
        out.plan = r;
        return out;
    }

    float SmoothFocusDistance(FocusMemory& memory, float distance, float dtSeconds, float omega)
    {
        const float goal = std::log(std::max(kFocusMinMeters, std::min(kFocusMaxMeters, distance)));
        if (!memory.springPrimed || !(dtSeconds > 0.0f) || !(omega > 0.0f))
        {
            memory.springValue = goal;
            memory.springVelocity = 0.0f;
            memory.springPrimed = true;
            return std::exp(goal);
        }
        // The same exact critically damped step as SpringState::Step.
        const float decay = std::exp(-omega * dtSeconds);
        const float error = memory.springValue - goal;
        const float v0 = memory.springVelocity;
        const float c = v0 + omega * error;
        const float e = (error + c * dtSeconds) * decay;
        memory.springVelocity = (v0 - omega * c * dtSeconds) * decay;
        memory.springValue = goal + e;
        return std::exp(memory.springValue);
    }

    // ------------------------------------------------------------------
    // Attach and Look At targets, bones and the near clip.

    bool LookupTarget(const Scene& scene, std::uint32_t id, std::uint32_t bone, std::uint64_t timeMs, EntityState& state,
        bool* boneMissing)
    {
        if (boneMissing)
            *boneMissing = false;
        if (!id)
            return false;
        if (bone != 0 && bones::Valid(bone) && scene.boneLookup && scene.boneLookup(id, bone, timeMs, state))
            return true;
        if (bone != 0 && boneMissing)
            *boneMissing = true;
        return scene.lookup && scene.lookup(id, timeMs, state);
    }

    namespace
    {

        Mat3 FromQuaternion(const float q[4])
        {
            const float x = q[0], y = q[1], z = q[2], w = q[3];
            Mat3 r{};
            r.m[0][0] = 1.0f - 2.0f * (y * y + z * z);
            r.m[0][1] = 2.0f * (x * y - w * z);
            r.m[0][2] = 2.0f * (x * z + w * y);
            r.m[1][0] = 2.0f * (x * y + w * z);
            r.m[1][1] = 1.0f - 2.0f * (x * x + z * z);
            r.m[1][2] = 2.0f * (y * z - w * x);
            r.m[2][0] = 2.0f * (x * z - w * y);
            r.m[2][1] = 2.0f * (y * z + w * x);
            r.m[2][2] = 1.0f - 2.0f * (x * x + y * y);
            return r;
        }

        bool UnitQuaternion(const float q[4], float out[4])
        {
            const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (!(n > 1e-6f) || !std::isfinite(n))
                return false;
            for (int i = 0; i < 4; ++i)
                out[i] = q[i] / n;
            return true;
        }
    }

    EntityState EntityFromBone(const editor::Vec3& rootPosition, const float rootQuaternion[4], const float boneTranslation[3],
        const float boneQuaternion[4])
    {
        float rq[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float bq[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        UnitQuaternion(rootQuaternion, rq);
        UnitQuaternion(boneQuaternion, bq);
        const Mat3 root = FromQuaternion(rq);
        const Mat3 world = Mul(root, FromQuaternion(bq));
        const Vec3 at = Add(rootPosition, Mul(root, Vec3{boneTranslation[0], boneTranslation[1], boneTranslation[2]}));
        const Vec3 euler = ToEuler(world);
        EntityState state;
        state.position = at;
        state.pitch = euler.x;
        state.roll = euler.y;
        state.heading = euler.z;
        state.hasHead = true;
        state.head = at;
        return state;
    }

    bool SetMarkerExtra(Keyframe& marker, const editor::CameraMarkerExtra& e)
    {
        const std::uint32_t f = e.fields;
        const auto has = [f](std::uint32_t bit) { return (f & bit) != 0; };
        const auto finite3 = [](const editor::Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
        const auto zeroOr = [](float value, float low, float high) {
            return std::isfinite(value) && (value == 0.0f || (value >= low && value <= high));
        };
        if ((has(editor::CameraExtraAttachBone) && !bones::Valid(e.attachBone)) ||
            (has(editor::CameraExtraLookAtBone) && !bones::Valid(e.lookAtBone)) ||
            (has(editor::CameraExtraNearClip) && !zeroOr(e.nearClip, editor::kNearClipMin, editor::kNearClipMax)) ||
            (has(editor::CameraExtraDofFStop) && !zeroOr(e.dofFStop, editor::kDofFStopMin, editor::kDofFStopMax)) ||
            (has(editor::CameraExtraDofLens) && !zeroOr(e.dofLensMm, editor::kDofLensMin, editor::kDofLensMax)) ||
            (has(editor::CameraExtraRelPosition) &&
                (!finite3(e.relPosition) || std::fabs(e.relPosition.x) > 1000.0f || std::fabs(e.relPosition.y) > 1000.0f ||
                    std::fabs(e.relPosition.z) > 1000.0f)) ||
            (has(editor::CameraExtraRelRotation) && !finite3(e.relRotation)))
            return false;
        // A bone needs its target (a bone on no target is dropped).
        if (has(editor::CameraExtraAttachBone))
            marker.attachBone = marker.attachTarget ? e.attachBone : 0;
        if (has(editor::CameraExtraLookAtBone))
            marker.lookAtBone = marker.lookAtTarget ? e.lookAtBone : 0;
        if (has(editor::CameraExtraRelPosition))
            marker.relPosition = e.relPosition;
        if (has(editor::CameraExtraRelRotation))
        {
            marker.relRotation = {WrapDegrees(e.relRotation.x), WrapDegrees(e.relRotation.y), WrapDegrees(e.relRotation.z)};
            marker.relRotation.x = std::max(-89.5f, std::min(89.5f, marker.relRotation.x)); // pitch limit
        }
        if (has(editor::CameraExtraNearClip))
            marker.nearClip = e.nearClip;
        if (has(editor::CameraExtraDofFStop))
            marker.dofFStop = e.dofFStop;
        if (has(editor::CameraExtraDofLens))
            marker.dofLensMm = e.dofLensMm;
        marker.editedMask |= editor::CameraOptionExtra;
        if (has(editor::CameraExtraAttachBone) || has(editor::CameraExtraLookAtBone) || has(editor::CameraExtraRelPosition) ||
            has(editor::CameraExtraRelRotation))
            ClearBorrowed(marker);
        return true;
    }

    void FillSnapshotExtra(const Keyframe& marker, editor::CameraMarkerExtra& out)
    {
        out = {};
        out.attachBone = marker.attachBone;
        out.lookAtBone = marker.lookAtBone;
        out.nearClip = marker.nearClip;
        out.dofFStop = marker.dofFStop;
        out.dofLensMm = marker.dofLensMm;
        out.relPosition = marker.relPosition;
        out.relRotation = marker.relRotation;
    }

    float MarkerNearClip(const Keyframe& marker)
    {
        return marker.nearClip > 0.0f
            ? std::max(editor::kNearClipMin, std::min(editor::kNearClipMax, marker.nearClip))
            : editor::kNearClipDefault;
    }

    float NearClipAt(const Track& track, const SegmentPlan& plan)
    {
        const std::vector<Keyframe>& keys = track.Keyframes();
        const int count = static_cast<int>(keys.size());
        if (!plan.valid || plan.current < 0 || plan.current >= count)
            return editor::kNearClipDefault;
        const float a = MarkerNearClip(keys[plan.current]);
        if (!plan.blending || plan.next < 0 || plan.next >= count)
            return a;
        return Lerp(a, MarkerNearClip(keys[plan.next]), Clamp01(plan.phase));
    }

    float DofLensMm(float fovDegrees, float intensity, float lensOverride)
    {
        float lens = lensOverride;
        if (!(lens > 0.0f) || !std::isfinite(lens))
        {
            const float fov = std::max(5.0f, std::min(130.0f, std::isfinite(fovDegrees) ? fovDegrees : 45.0f));
            const float base = 12.0f / std::tan(fov * 0.5f * kDegreesToRadians);
            const float amount = std::max(0.0f, std::min(100.0f, std::isfinite(intensity) ? intensity : 50.0f)) / 100.0f;
            lens = base * Lerp(kDofLensMultiplierMin, kDofLensMultiplierMax, amount);
        }
        return std::max(editor::kDofLensMin, std::min(editor::kDofLensMax, lens));
    }

    void InFocusRange(float fstop, float lensMm, float focusMeters, float& nearMeters, float& farMeters)
    {
        nearMeters = farMeters = 0.0f;
        if (!(fstop > 0.0f) || !(lensMm > 0.0f) || !(focusMeters > 0.0f))
            return;
        constexpr float kCircleMm = 0.03f;
        const double f = lensMm;
        const double s = static_cast<double>(focusMeters) * 1000.0;
        const double hyperMm = f * f / (static_cast<double>(fstop) * kCircleMm) + f;
        nearMeters = static_cast<float>(s * (hyperMm - f) / (hyperMm + s - 2.0 * f) / 1000.0);
        farMeters = s < hyperMm ? static_cast<float>(s * (hyperMm - f) / (hyperMm - s) / 1000.0) : 0.0f;
    }

    EntityState EntityFromRecorded(const editor::Vec3& position, const float q[4], const editor::Vec3* head)
    {
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        Mat3 r{};
        r.m[0][0] = 1.0f - 2.0f * (y * y + z * z);
        r.m[0][1] = 2.0f * (x * y - w * z);
        r.m[0][2] = 2.0f * (x * z + w * y);
        r.m[1][0] = 2.0f * (x * y + w * z);
        r.m[1][1] = 1.0f - 2.0f * (x * x + z * z);
        r.m[1][2] = 2.0f * (y * z - w * x);
        r.m[2][0] = 2.0f * (x * z - w * y);
        r.m[2][1] = 2.0f * (y * z + w * x);
        r.m[2][2] = 1.0f - 2.0f * (x * x + y * y);
        const Vec3 euler = ToEuler(r);
        EntityState state;
        state.position = position;
        state.pitch = euler.x;
        state.roll = euler.y;
        state.heading = euler.z;
        state.hasHead = head != nullptr;
        if (head)
            state.head = *head;
        return state;
    }

    PrefetchTarget PlanPrefetch(const Track& track, const SegmentPlan& plan, std::uint64_t timeMs,
        const editor::CameraState& camera, const Scene& scene, std::uint32_t leadMs, float minMeters)
    {
        PrefetchTarget out;
        const std::vector<Keyframe>& keys = track.Keyframes();
        if (!plan.valid || plan.blending || plan.next < 0 || plan.next >= static_cast<int>(keys.size()))
            return out;
        const Keyframe& next = keys[plan.next];
        if (next.timeMs <= timeMs || next.timeMs - timeMs > leadMs)
            return out;
        const LivePose live = Resolve(next, next.timeMs, scene);
        const Vec3 at = live.pose.position;
        const Vec3 delta = Sub(at, camera.position);
        const float meters = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        if (!(meters >= minMeters))
            return out;
        out.want = true;
        out.marker = plan.next;
        out.cutMs = next.timeMs;
        out.meters = meters;
        out.view = {at, Track::ToEulerDegrees(live.pose.orientation), live.pose.fov};
        return out;
    }

// ----------------------------------------------------------------------
// Marker tools.
    bool Track::Retime(const std::vector<std::pair<std::uint32_t, std::uint64_t>>& times, std::uint64_t limitMs, bool pushAfter)
    {
        if (times.empty())
            return false;
        const std::vector<Keyframe> before = keyframes_;
        std::size_t lastIndex = 0;
        for (const auto& entry : times)
        {
            const std::size_t index = IndexOf(entry.first);
            if (index >= keyframes_.size())
            {
                keyframes_ = before;
                return false;
            }
            keyframes_[index].timeMs = entry.second;
            lastIndex = std::max(lastIndex, index);
        }
        bool ok = !pushAfter || KeepSpacingAfter(lastIndex, limitMs);
        for (std::size_t n = 1; ok && n < keyframes_.size(); ++n)
        {
            const Keyframe& previous = keyframes_[n - 1];
            if (keyframes_[n].timeMs < previous.timeMs + MarkerSpacingMs(previous.speedIndex))
                ok = false;
        }
        for (std::size_t n = 0; ok && n < keyframes_.size(); ++n)
        {
            if (keyframes_[n].timeMs > limitMs && keyframes_[n].timeMs != before[n].timeMs)
                ok = false;
        }
        if (!ok)
        {
            keyframes_ = before;
            return false;
        }
        ApplyLastMarkerRule(*this);
        return true;
    }

    std::uint32_t Track::AddCopy(const Keyframe& marker)
    {
        Keyframe copy = marker;
        ClearBorrowed(copy);
        const std::uint32_t id = AddMarker(copy);
        if (id)
            ApplyLastMarkerRule(*this);
        return id;
    }

    namespace
    {

        bool Usable(const Keyframe& k) { return k.markerType != MarkerType::Anchor; }

        // Usable markers a and b (a before b) are joined in one move.
        bool Joined(const Keyframe& a, const Keyframe& b, const std::vector<std::uint32_t>* cuts)
        {
            if (a.blend == Blend::None || a.cameraMode != CameraMode::Free || b.cameraMode != CameraMode::Free)
                return false;
            if (cuts)
                for (const std::uint32_t cut : *cuts)
                    if (cut > a.timeMs && cut <= b.timeMs)
                        return false;
            return true;
        }
    }

    MoveRange MoveOf(const Track& track, std::uint32_t id, const std::vector<std::uint32_t>* cuts)
    {
        MoveRange range;
        const std::vector<Keyframe>& keys = track.Keyframes();
        int at = -1;
        for (std::size_t n = 0; n < keys.size(); ++n)
            if (keys[n].id == id)
                at = static_cast<int>(n);
        if (at < 0)
            return range;
        const auto previousUsable = [&](int from) {
            for (int n = from - 1; n >= 0; --n)
                if (Usable(keys[n]))
                    return n;
            return -1;
        };
        const auto nextUsable = [&](int from) {
            for (int n = from + 1; n < static_cast<int>(keys.size()); ++n)
                if (Usable(keys[n]))
                    return n;
            return -1;
        };
        // An Anchor belongs to the move around it: start from the usable
        // marker at or before it.
        int start = Usable(keys[at]) ? at : previousUsable(at);
        if (start < 0)
            start = nextUsable(at);
        if (start < 0)
            return range;
        range.first = range.last = start;
        for (int n = previousUsable(range.first); n >= 0 && Joined(keys[n], keys[range.first], cuts); n = previousUsable(n))
            range.first = n;
        for (int n = nextUsable(range.last); n >= 0 && Joined(keys[range.last], keys[n], cuts); n = nextUsable(n))
            range.last = n;
        return range;
    }

    std::uint32_t DuplicateMarker(Track& track, std::uint32_t id, std::uint64_t limitMs)
    {
        const std::vector<Keyframe>& keys = track.Keyframes();
        std::size_t index = keys.size();
        for (std::size_t n = 0; n < keys.size(); ++n)
            if (keys[n].id == id)
                index = n;
        if (index >= keys.size())
            return 0;
        const Keyframe source = keys[index];
        std::uint64_t at = 0;
        if (index + 1 < keys.size())
            at = source.timeMs + (keys[index + 1].timeMs - source.timeMs) / 2;
        else
        {
            at = source.timeMs + kDuplicateAfterLastMs;
            if (at > limitMs)
                return 0;
        }
        if (!WithinClipBounds(track, at))
            return 0;
        Keyframe copy = source;
        copy.timeMs = at;
        copy.transition = 0; // a fade belongs to the clip's first or last marker
        return track.AddCopy(copy);
    }

    namespace
    {

        // The inner times of a run: first and last kept, the weights between
        // consecutive markers share the span.
        std::vector<std::uint64_t> Spread(std::uint64_t first, std::uint64_t last, const std::vector<double>& weights)
        {
            std::vector<std::uint64_t> out;
            double total = 0.0;
            for (const double w : weights)
                total += w;
            if (!(total > 0.0))
                return out;
            double sum = 0.0;
            for (std::size_t n = 0; n + 1 < weights.size(); ++n)
            {
                sum += weights[n];
                out.push_back(first + static_cast<std::uint64_t>(std::llround(static_cast<double>(last - first) * sum / total)));
            }
            return out;
        }
    }

    bool DistributeMove(Track& track, std::uint32_t id, bool byDistance, const Scene& scene)
    {
        const MoveRange range = MoveOf(track, id, scene.cuts);
        if (!range.Valid())
            return false;
        const std::vector<Keyframe>& keys = track.Keyframes();
        if (range.last - range.first < 2)
            return false;
        // One weight per gap between consecutive markers (Anchors included:
        // an Anchor sits on the path where its time puts it, so it splits
        // the distance of the gap it is in evenly).
        std::vector<double> weights;
        std::vector<Vec3> at;
        for (int n = range.first; n <= range.last; ++n)
            at.push_back(Usable(keys[n]) ? Resolve(keys[n], keys[n].timeMs, scene).pose.position : Vec3{});
        for (int n = range.first; n < range.last; ++n)
        {
            double w = 1.0;
            if (byDistance)
            {
                // From the usable marker at or before n to the next usable one.
                int a = n, b = n + 1;
                while (a > range.first && !Usable(keys[a]))
                    --a;
                while (b < range.last && !Usable(keys[b]))
                    ++b;
                int inGap = 0;
                for (int m = a; m < b; ++m)
                    ++inGap;
                const Vec3 d = Sub(at[b - range.first], at[a - range.first]);
                w = std::sqrt(static_cast<double>(d.x) * d.x + static_cast<double>(d.y) * d.y + static_cast<double>(d.z) * d.z) /
                    std::max(1, inGap);
            }
            weights.push_back(w);
        }
        double total = 0.0;
        for (const double w : weights)
            total += w;
        if (!(total > 0.001))
            std::fill(weights.begin(), weights.end(), 1.0); // a move that stands still: even times
        const std::vector<std::uint64_t> inner = Spread(keys[range.first].timeMs, keys[range.last].timeMs, weights);
        std::vector<std::pair<std::uint32_t, std::uint64_t>> times;
        for (int n = range.first + 1; n < range.last; ++n)
            times.push_back({keys[n].id, inner[static_cast<std::size_t>(n - range.first - 1)]});
        return track.Retime(times);
    }

    bool StretchMove(Track& track, std::uint32_t id, std::uint32_t percent, std::uint64_t limitMs, const Scene& scene)
    {
        if (percent < 25 || percent > 400)
            return false;
        const MoveRange range = MoveOf(track, id, scene.cuts);
        if (!range.Valid() || range.last == range.first)
            return false;
        const std::vector<Keyframe>& keys = track.Keyframes();
        if (IsClipBound(track, keys[range.last].id))
            return false;
        const int last = range.last;
        const std::uint64_t origin = keys[range.first].timeMs;
        std::vector<std::pair<std::uint32_t, std::uint64_t>> times;
        for (int n = range.first + 1; n <= last; ++n)
        {
            const std::uint64_t offset = keys[n].timeMs - origin;
            times.push_back({keys[n].id, origin + (offset * percent + 50) / 100});
        }
        return track.Retime(times, limitMs, true);
    }

    int LockMarkers(Track& track, std::uint32_t id, bool everyMarker, std::uint32_t target, AttachType type, const Scene& scene)
    {
        std::vector<std::uint32_t> ids;
        const std::vector<Keyframe>& keys = track.Keyframes();
        if (everyMarker)
        {
            for (const Keyframe& k : keys)
                ids.push_back(k.id);
        }
        else
        {
            const MoveRange range = MoveOf(track, id, scene.cuts);
            if (!range.Valid())
                return 0;
            for (int n = range.first; n <= range.last; ++n)
                ids.push_back(keys[n].id);
        }
        int changed = 0;
        for (const std::uint32_t markerId : ids)
        {
            Keyframe* marker = track.FindMutable(markerId);
            if (!marker || marker->cameraMode != CameraMode::Free || marker->markerType == MarkerType::Anchor)
                continue;
            if (marker->attachTarget == target && (target == 0 || marker->attachType == type))
                continue;
            EntityState state;
            if (target != 0 && !(scene.lookup && scene.lookup(target, marker->timeMs, state)))
                continue; // the target is not in the clip at that marker: left as it was
            const LivePose live = Resolve(*marker, marker->timeMs, scene);
            const editor::CameraState view{live.pose.position, Track::ToEulerDegrees(live.pose.orientation), live.pose.fov};
            SetAttach(*marker, target);
            marker->attachBone = 0; // a lock is to the character's root
            if (target != 0)
                SetAttachType(*marker, type);
            SetMarkerView(*marker, view, scene);
            marker->editedMask |= editor::CameraOptionAttachTarget | editor::CameraOptionAttachType;
            ++changed;
        }
        if (changed)
            ApplyLastMarkerRule(track);
        return changed;
    }
}
