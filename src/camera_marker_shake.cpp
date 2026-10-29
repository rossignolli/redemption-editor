#include "camera_marker_shake.h"

#include "camera_effects.h"
#include "camera_shake.h"
#include "camera_shake_custom.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace replay::camera::mshake
{
    namespace
    {
        float Clamp01(float value) { return std::max(0.0f, std::min(1.0f, value)); }

        float Hold(float value, Range range)
        {
            if (!std::isfinite(value))
                return range.minimum;
            return std::max(range.minimum, std::min(range.maximum, value));
        }

        bool Inside(float value, Range range) { return std::isfinite(value) && value >= range.minimum && value <= range.maximum; }

        // The eight numbers a Blend can carry, in ShakeBlendBits order.
        constexpr int kFieldCount = 8;
        float MarkerShake::*const kFields[kFieldCount] = {&MarkerShake::gain, &MarkerShake::amp, &MarkerShake::hz,
            &MarkerShake::speedAmp, &MarkerShake::speedHz, &MarkerShake::rotWeight, &MarkerShake::posWeight,
            &MarkerShake::speedRef};

        std::vector<int> ShakeMarkers(const Track& track)
        {
            std::vector<int> list;
            const std::vector<Keyframe>& keys = track.Keyframes();
            list.reserve(keys.size());
            for (std::size_t n = 0; n < keys.size(); ++n)
                if (HoldsShake(keys[n]))
                    list.push_back(static_cast<int>(n));
            return list;
        }

        // The clock's speed over the segment that starts at this marker.
        double RateOf(const Keyframe& k)
        {
            return k.shake.rate;
        }

        // A segment's clip span on the shake's clock (ms), as a chained shake clock
        // counts it: duration x shake speed / marker speed.
        double ClockSpan(const Keyframe& k, std::uint64_t durationMs)
        {
            if ((k.shake.flags & editor::ShakeFlagRealTime) != 0)
                return static_cast<double>(durationMs) * k.shake.rate / SpeedFactor(k.speedIndex);
            return static_cast<double>(durationMs) * RateOf(k);
        }

        bool Blends(const MarkerShake& s, std::uint16_t bit)
        {
            return s.transition == static_cast<std::uint8_t>(editor::ShakeTransition::Blend) && (s.blendMask & bit) != 0;
        }

        // The gain's weight with the camera blend's ease (the
        // intensity, Plan's eased phase) on the segment [from, to].
        float CameraWeight(const Track& track, std::uint64_t fromMs, std::uint64_t toMs, std::uint64_t timeMs, float linear)
        {
            const std::vector<Keyframe>& keys = track.Keyframes();
            const SegmentPlan plan = Plan(track, timeMs);
            if (!plan.valid || !plan.blending || plan.next < 0 || keys[plan.next].timeMs != toMs)
                return linear;
            if (keys[plan.current].timeMs == fromMs)
                return plan.phase;
            const SegmentPlan start = Plan(track, fromMs);
            if (!start.valid || !start.blending || start.next != plan.next)
                return linear;
            const float left = 1.0f - start.phase;
            return left > 1e-6f ? (plan.phase - start.phase) / left : 1.0f;
        }

        void ApplyDelta(ShakeDelta& out, const shake::Sample& sample)
        {
            out.position = {sample.value[shake::X], sample.value[shake::Y], sample.value[shake::Z]};
            out.rotation = {sample.value[shake::Pitch], sample.value[shake::Roll], sample.value[shake::Yaw]};
        }
    }

    // ------------------------------------------------------------------
    // The state's rules.

    bool Usable(const MarkerShake& s)
    {
        for (float MarkerShake::*field : kFields)
            if (!std::isfinite(s.*field))
                return false;
        return std::isfinite(s.rate) && std::isfinite(s.startCycles) && std::isfinite(s.startFadeMs) && s.on <= 1 &&
            ValidProfile(s.profile) && s.transition < editor::kShakeTransitionCount && s.still <= 1 && s.flags < 32;
    }

    MarkerShake Clamped(MarkerShake s)
    {
        s.on = s.on ? 1 : 0;
        s.profile = ValidProfile(s.profile) ? s.profile : 0;
        s.transition = std::min<std::uint8_t>(s.transition, editor::kShakeTransitionCount - 1);
        s.flags &= 31;
        s.blendMask &= editor::ShakeBlendAll;
        s.still = s.still ? 1 : 0;
        s.reserved[0] = s.reserved[1] = s.reserved[2] = 0;
        s.gain = Hold(s.gain, kGainRange);
        s.amp = Hold(s.amp, kAmpRange);
        s.hz = Hold(s.hz, kHzRange);
        s.speedAmp = Hold(s.speedAmp, kCouplingRange);
        s.speedHz = Hold(s.speedHz, kCouplingRange);
        s.rotWeight = Hold(s.rotWeight, kWeightRange);
        s.posWeight = Hold(s.posWeight, kWeightRange);
        s.speedRef = Hold(s.speedRef, kSpeedRefRange);
        s.rate = Hold(s.rate, kRateRange);
        s.startCycles = std::isfinite(s.startCycles) ? std::max(0.0, s.startCycles) : 0.0;
        s.startFadeMs = std::isfinite(s.startFadeMs) ? std::max(0.0, s.startFadeMs) : 0.0;
        return s;
    }

    bool Same(const MarkerShake& a, const MarkerShake& b)
    {
        for (float MarkerShake::*field : kFields)
            if (a.*field != b.*field)
                return false;
        return a.on == b.on && a.profile == b.profile && a.transition == b.transition && a.flags == b.flags &&
            a.blendMask == b.blendMask && a.seed == b.seed && a.still == b.still && a.rate == b.rate &&
            a.startCycles == b.startCycles && a.startFadeMs == b.startFadeMs;
    }

    MarkerShake Continued(MarkerShake s)
    {
        s.flags &= static_cast<std::uint8_t>(~(editor::ShakeFlagRestart | editor::ShakeFlagNewPattern));
        s.startCycles = 0.0;
        s.startFadeMs = 0.0;
        return s;
    }

    float GainOf(std::uint8_t intensity) { return shake::Gain(intensity); }

    std::uint8_t IntensityOf(float gain)
    {
        const float i = 6.0f * std::sqrt(std::max(0.0f, gain)) - 1.0f;
        return static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::floor(i + 0.5f)), 1, 10));
    }

    editor::ShakeType SummaryType(const MarkerShake& s)
    {
        if (!s.on)
            return editor::ShakeType::None;
        return s.profile == 0 ? editor::ShakeType::Custom : static_cast<editor::ShakeType>(s.profile);
    }

    const char* PresetName(std::uint8_t preset)
    {
        static const char* const kNames[kPresetCount] = {"Subtle", "Handheld", "Vehicle", "Earthquake"};
        return preset < kPresetCount ? kNames[preset] : "Custom";
    }

    void ApplyPreset(MarkerShake& s, std::uint8_t preset)
    {
        const fx::PresetValues& p = fx::kPresets[std::min<std::uint8_t>(preset, kPresetCount - 1) + 1];
        s.on = 1;
        s.profile = 0;
        s.amp = p.amp;
        s.hz = p.hz;
        s.speedAmp = p.speedAmp;
        s.speedHz = p.speedHz;
    }

    std::uint8_t MatchingPreset(const MarkerShake& s)
    {
        if (s.profile != 0)
            return 0xFF;
        for (std::uint8_t n = 0; n < kPresetCount; ++n)
        {
            const fx::PresetValues& p = fx::kPresets[n + 1];
            if (s.amp == p.amp && s.hz == p.hz && s.speedAmp == p.speedAmp && s.speedHz == p.speedHz)
                return n;
        }
        return 0xFF;
    }

    void ApplyProfile(MarkerShake& s, std::uint8_t profile)
    {
        s.on = 1;
        s.profile = ValidProfile(profile) && profile != 0 ? profile : static_cast<std::uint8_t>(editor::ShakeType::Handheld);
        s.amp = 1.0f;
        s.hz = static_cast<float>(kProfileClockHz);
        s.speedHz = 0.0f;
    }

    std::string Describe(const MarkerShake& s)
    {
        if (!s.on)
            return "Off";
        if (s.profile != 0)
            return fx::ShakeName(static_cast<editor::ShakeType>(s.profile));
        const std::uint8_t preset = MatchingPreset(s);
        return std::string("simple ") + (preset == 0xFF ? "custom" : PresetName(preset));
    }

    // ------------------------------------------------------------------
    // Evaluation.

    bool FiresImpulse(const MarkerShake* previous, const MarkerShake& s)
    {
        if (!s.on || s.profile == 0 || !shake::OneShot(static_cast<editor::ShakeType>(s.profile)))
            return false;
        if (!previous || !previous->on || previous->profile != s.profile || previous->rate != s.rate ||
            (previous->flags & editor::ShakeFlagRealTime) != (s.flags & editor::ShakeFlagRealTime))
            return true;
        for (int f = 0; f < kFieldCount; ++f)
            if (previous->*kFields[f] != s.*kFields[f] && !Blends(*previous, static_cast<std::uint16_t>(1u << f)))
                return true;
        return false;
    }

    namespace
    {
        // `impulses` false: the rules before the one shot fired at its
        // marker (the migration keeps the old clips' phase with them).
        Resolved ResolveRules(const Track& track, std::uint64_t timeMs, bool impulses);
    }

    Resolved Resolve(const Track& track, std::uint64_t timeMs)
    {
        return ResolveRules(track, timeMs, true);
    }

    namespace
    {
    Resolved ResolveRules(const Track& track, std::uint64_t timeMs, bool impulses)
    {
        Resolved r;
        const std::vector<Keyframe>& keys = track.Keyframes();
        const std::vector<int> list = ShakeMarkers(track);
        int at = -1;
        for (std::size_t n = 0; n < list.size(); ++n)
        {
            if (keys[list[n]].timeMs <= timeMs)
                at = static_cast<int>(n);
            else
                break;
        }
        if (at < 0)
            return r;
        const Keyframe& k = keys[list[at]];
        const MarkerShake& s = k.shake;
        r.marker = list[at];
        r.next = at + 1 < static_cast<int>(list.size()) ? list[at + 1] : -1;
        if (!s.on)
            return r;
        r.active = true;
        r.profile = s.profile;
        const Keyframe* next = r.next >= 0 ? &keys[r.next] : nullptr;

        // The numbers: Cut holds them, Blend goes to the next marker's.
        float u = 0.0f;
        if (next && next->timeMs > k.timeMs && timeMs >= k.timeMs)
            u = Clamp01(static_cast<float>(static_cast<double>(timeMs - k.timeMs) / static_cast<double>(next->timeMs - k.timeMs)));
        const bool blend = next && s.transition == static_cast<std::uint8_t>(editor::ShakeTransition::Blend);
        r.blending = blend && s.blendMask != 0;
        float values[kFieldCount];
        for (int f = 0; f < kFieldCount; ++f)
        {
            const float a = s.*kFields[f];
            values[f] = a;
            if (!blend || (s.blendMask & (1u << f)) == 0)
                continue;
            float w = u;
            if (f == 0 && (s.flags & editor::ShakeFlagEaseWithCamera) != 0)
                w = CameraWeight(track, k.timeMs, next->timeMs, timeMs, u);
            const float b = next->shake.*kFields[f];
            values[f] = a + (b - a) * w;
        }
        float gain = values[0];
        if ((s.flags & editor::ShakeFlagFadeOut) != 0 && next && next->timeMs > timeMs)
        {
            const float left = static_cast<float>(next->timeMs - timeMs) / kFadeOutMs;
            if (left < 1.0f)
                gain *= left * left * (3.0f - 2.0f * left);
        }
        r.gain = gain;
        editor::ShakeCustom& c = r.custom;
        c.amp = values[1];
        c.hz = values[2];
        c.speedAmp = values[3];
        c.speedHz = values[4];
        c.rotWeight = values[5];
        c.posWeight = values[6];
        c.speedRef = values[7];
        c.seed = s.seed;
        c.still = s.still;
        c.reserved = 0;

        // Where the phase and the fade in start: the turn on, else the last
        // Restart (the phase also at a NewPattern, and where a one shot fires).
        int phaseFrom = -1;
        int fadeFrom = -1;
        for (int m = at; m >= 0 && (phaseFrom < 0 || fadeFrom < 0); --m)
        {
            const MarkerShake& ms = keys[list[m]].shake;
            const bool turnOn = m == 0 || !keys[list[m - 1]].shake.on;
            const bool fires = impulses && FiresImpulse(m > 0 ? &keys[list[m - 1]].shake : nullptr, ms);
            if (phaseFrom < 0 && (turnOn || fires || (ms.flags & (editor::ShakeFlagRestart | editor::ShakeFlagNewPattern)) != 0))
                phaseFrom = m;
            if (fadeFrom < 0 && (turnOn || (ms.flags & editor::ShakeFlagRestart) != 0))
                fadeFrom = m;
        }
        const auto startOf = [&](int m, bool phase) {
            const MarkerShake& ms = keys[list[m]].shake;
            if ((ms.flags & editor::ShakeFlagRestart) == 0)
                return 0.0;
            return phase ? ms.startCycles : ms.startFadeMs;
        };
        double cycles = startOf(phaseFrom, true);
        double fadeMs = startOf(fadeFrom, false);
        for (int m = std::min(phaseFrom, fadeFrom); m < at; ++m)
        {
            const Keyframe& a = keys[list[m]];
            const Keyframe& b = keys[list[m + 1]];
            const double span = ClockSpan(a, b.timeMs - a.timeMs);
            if (m >= fadeFrom)
                fadeMs += span;
            if (m >= phaseFrom)
            {
                const bool lerped = Blends(a.shake, editor::ShakeBlendHz);
                cycles += span / 1000.0 * (lerped ? 0.5 * (static_cast<double>(a.shake.hz) + b.shake.hz) : a.shake.hz);
            }
        }
        if (timeMs > k.timeMs)
        {
            const double span = ClockSpan(k, timeMs - k.timeMs);
            fadeMs += span;
            const double hz = blend && (s.blendMask & editor::ShakeBlendHz) != 0
                ? s.hz + (static_cast<double>(next->shake.hz) - s.hz) * u * 0.5
                : s.hz;
            cycles += span / 1000.0 * hz;
        }
        r.cycles = cycles;
        r.fadeMs = fadeMs;
        r.originMs = keys[list[phaseFrom]].timeMs;
        return r;
    }
    }

    ShakeDelta Delta(const Track& track, std::uint64_t timeMs, const Scene& scene)
    {
        ShakeDelta out;
        const Resolved r = Resolve(track, timeMs);
        if (!r.active)
            return out;
        const editor::ShakeCustom& c = r.custom;
        double cycles = r.cycles;
        if (c.speedHz > 0.0f)
            cycles += CustomCouplingCycles(track, scene, 2, r.originMs, timeMs);
        const std::int64_t now = static_cast<std::int64_t>(timeMs);
        const float speed = c.speedAmp > 0.0f || c.still ? ShakeCurveSpeed(track, scene, now) : 0.0f;
        const float still = c.still ? ShakeStillGate(track, scene, now, speed) : 1.0f;
        const float speedNorm = std::min(1.0f, speed / std::max(1.0f, c.speedRef));
        if (r.profile != 0)
        {
            // A realistic profile: the gain x Amp x the speed coupling, at
            // the clock cycles x 250 ms; the weights scale its channels.
            const float gain = r.gain * c.amp * (1.0f + c.speedAmp * speedNorm) * still;
            const ShakeDelta d = ShakeOffset(static_cast<editor::ShakeType>(r.profile), gain, cycles * 1000.0 / kProfileClockHz);
            out.position = {d.position.x * c.posWeight, d.position.y * c.posWeight, d.position.z * c.posWeight};
            out.rotation = {d.rotation.x * c.rotWeight, d.rotation.y * c.rotWeight, d.rotation.z * c.rotWeight};
            return out;
        }
        ApplyDelta(out, shake::EvaluateCustom(c, r.gain, cycles, speedNorm, still, r.fadeMs / 1000.0));
        return out;
    }

    MarkerShake HeldAt(const Track& track, std::uint64_t timeMs)
    {
        const Resolved r = Resolve(track, timeMs);
        if (r.marker < 0)
            return editor::DefaultMarkerShake();
        const Keyframe& k = track.Keyframes()[r.marker];
        MarkerShake s = Continued(k.shake);
        if (!r.active)
            return s;
        // The blended numbers (the gain before the fade out).
        s.amp = r.custom.amp;
        s.hz = r.custom.hz;
        s.speedAmp = r.custom.speedAmp;
        s.speedHz = r.custom.speedHz;
        s.rotWeight = r.custom.rotWeight;
        s.posWeight = r.custom.posWeight;
        s.speedRef = r.custom.speedRef;
        if (r.next >= 0 && Blends(k.shake, editor::ShakeBlendGain))
        {
            const Keyframe& n = track.Keyframes()[r.next];
            const float u = n.timeMs > k.timeMs
                ? Clamp01(static_cast<float>(static_cast<double>(timeMs - k.timeMs) / static_cast<double>(n.timeMs - k.timeMs)))
                : 0.0f;
            s.gain = k.shake.gain + (n.shake.gain - k.shake.gain) * u;
        }
        return Clamped(s);
    }

    // ------------------------------------------------------------------
    // The legacy rules (the events track, then the legacy marker shake), for the
    // migration's proof.

    ShakeDelta LegacyDelta(const Track& track, std::uint64_t timeMs, const Scene& scene)
    {
        ShakeDelta out;
        const fx::FxShake events = fx::Evaluate(track.LegacyEffects(), timeMs);
        if (events.active)
        {
            const editor::ShakeCustom& c = events.custom;
            double cycles = events.cycles;
            if (c.speedHz > 0.0f)
                cycles += CustomCouplingCycles(track, scene, 1, events.runStartMs, timeMs);
            const std::int64_t now = static_cast<std::int64_t>(timeMs);
            const float speed = c.speedAmp > 0.0f || c.still ? ShakeCurveSpeed(track, scene, now) : 0.0f;
            const float still = c.still ? ShakeStillGate(track, scene, now, speed) : 1.0f;
            const float speedNorm = std::min(1.0f, speed / std::max(1.0f, c.speedRef));
            if (events.profile != editor::ShakeType::None && events.profile != editor::ShakeType::Custom)
            {
                const float gain = c.amp * (1.0f + c.speedAmp * speedNorm) * still;
                const ShakeDelta delta = ShakeOffset(events.profile, gain, fx::ProfileClockMs(cycles));
                out.position = {delta.position.x * c.posWeight, delta.position.y * c.posWeight, delta.position.z * c.posWeight};
                out.rotation = {delta.rotation.x * c.rotWeight, delta.rotation.y * c.rotWeight, delta.rotation.z * c.rotWeight};
            }
            else
                ApplyDelta(out, shake::EvaluateCustom(c, 1.0f, cycles, speedNorm, still, events.sinceOnMs / 1000.0));
            return out;
        }
        const SegmentPlan plan = Plan(track, timeMs);
        if (!plan.valid)
            return out;
        if (plan.shake.type == editor::ShakeType::Custom)
        {
            const editor::ShakeCustom c = plan.shake.custom;
            double cycles = plan.shake.cycles;
            if (c.speedHz > 0.0f)
                cycles += CustomCouplingCycles(track, scene, 0, plan.shake.runStartMs, timeMs);
            const std::int64_t now = static_cast<std::int64_t>(timeMs);
            const float speed = c.speedAmp > 0.0f || c.still ? ShakeCurveSpeed(track, scene, now) : 0.0f;
            const float still = c.still ? ShakeStillGate(track, scene, now, speed) : 1.0f;
            ApplyDelta(out, shake::EvaluateCustom(c, plan.shake.amplitude, cycles, speed / std::max(1.0f, c.speedRef), still,
                                plan.shake.timeMs / 1000.0));
        }
        else if (plan.shake.type != editor::ShakeType::None)
        {
            const Keyframe& current = track.Keyframes()[plan.current];
            const float motionGain = ShakeMotionGain(track, timeMs, scene, current.legacyShake.motion);
            out = ShakeOffset(plan.shake.type, plan.shake.amplitude * motionGain, plan.shake.timeMs);
        }
        return out;
    }

    // ------------------------------------------------------------------
    // Migration.

    namespace
    {
        MarkerShake FromLegacy(const Keyframe::LegacyShake& l)
        {
            MarkerShake s = editor::DefaultMarkerShake();
            s.on = 1;
            s.gain = shake::Gain(l.intensity);
            if (l.type == editor::ShakeType::Custom)
            {
                const editor::ShakeCustom& c = l.custom;
                s.profile = 0;
                s.amp = c.amp;
                s.hz = c.hz;
                s.speedAmp = c.speedAmp;
                s.speedHz = c.speedHz;
                s.rotWeight = c.rotWeight;
                s.posWeight = c.posWeight;
                s.speedRef = c.speedRef;
                s.seed = c.seed;
                s.still = c.still ? 1 : 0;
            }
            else
            {
                // A profile with speed coupling ("with movement"):
                // 1 + speed / 15 m/s held to 2, and gone standing still.
                s.profile = static_cast<std::uint8_t>(l.type);
                s.amp = 1.0f;
                s.hz = static_cast<float>(kProfileClockHz);
                s.speedAmp = l.motion != editor::ShakeMotion::Off ? 1.0f : 0.0f;
                s.speedHz = 0.0f;
                s.rotWeight = 1.0f;
                s.posWeight = 1.0f;
                s.speedRef = kShakeMotionFullSpeed;
                s.still = l.motion == editor::ShakeMotion::SpeedStill ? 1 : 0;
            }
            s.transition = static_cast<std::uint8_t>(editor::ShakeTransition::Cut);
            s.blendMask = 0;
            return s;
        }

        MarkerShake FromEvents(const fx::State& st)
        {
            MarkerShake s = editor::DefaultMarkerShake();
            s.on = 1;
            s.profile = st.profile;
            s.gain = 1.0f;
            s.amp = st.amp;
            s.hz = st.hz;
            s.speedAmp = st.speedAmp;
            s.speedHz = st.speedHz;
            s.rotWeight = st.rotWeight;
            s.posWeight = st.posWeight;
            s.speedRef = 30.0f; // the mod's g_ShakeSpeedRefMax
            s.still = st.still ? 1 : 0;
            s.seed = st.seed;
            s.rate = 1.0f;
            s.transition = static_cast<std::uint8_t>(editor::ShakeTransition::Cut);
            s.blendMask = 0;
            return s;
        }

        bool Close(float a, float b)
        {
            return std::fabs(a - b) <= 1e-6f * std::max(1.0f, std::max(std::fabs(a), std::fabs(b)));
        }

        // The times the events' shake changes: every event, and where a
        // ramp of a switch or an index crosses a step (the first whole ms
        // at or after it: the clock is whole ms).
        std::set<std::uint64_t> EventTimes(const fx::EffectTrack& effects)
        {
            std::set<std::uint64_t> times;
            const std::vector<fx::ShakeEvent>& events = effects.Events();
            for (const fx::ShakeEvent& e : events)
                times.insert(e.timeMs);
            for (std::uint8_t k : {static_cast<std::uint8_t>(fx::Kind::OnOff), static_cast<std::uint8_t>(fx::Kind::Preset),
                     static_cast<std::uint8_t>(fx::Kind::StopWhenStill), static_cast<std::uint8_t>(fx::Kind::Profile)})
            {
                const fx::ShakeEvent* previous = nullptr;
                for (const fx::ShakeEvent& e : events)
                {
                    if (static_cast<std::uint8_t>(e.kind) != k)
                        continue;
                    if (previous && e.mode == fx::Mode::Ramp && e.timeMs > previous->timeMs && e.value != previous->value)
                    {
                        const double lo = std::min(previous->value, e.value);
                        const double hi = std::max(previous->value, e.value);
                        for (double h = std::floor(lo) + 0.5; h < hi; h += 1.0)
                        {
                            if (h <= lo)
                                continue;
                            const double u = (h - previous->value) / (static_cast<double>(e.value) - previous->value);
                            const double at = static_cast<double>(previous->timeMs) + u * static_cast<double>(e.timeMs - previous->timeMs);
                            times.insert(static_cast<std::uint64_t>(std::ceil(at)));
                            times.insert(static_cast<std::uint64_t>(std::ceil(at)) + 1);
                        }
                    }
                    previous = &e;
                }
            }
            return times;
        }

        float FieldOf(const fx::State& st, int f)
        {
            switch (f)
            {
            case 0: return 1.0f;
            case 1: return st.amp;
            case 2: return st.hz;
            case 3: return st.speedAmp;
            case 4: return st.speedHz;
            case 5: return st.rotWeight;
            case 6: return st.posWeight;
            default: return 30.0f;
            }
        }

        // A sample of the resolved state, to tell whether an added marker
        // changes anything.
        bool SameResolved(const Resolved& a, const Resolved& b)
        {
            if (a.active != b.active)
                return false;
            if (!a.active)
                return true;
            const auto nearly = [](double x, double y) { return std::fabs(x - y) <= 1e-6 * std::max(1.0, std::max(std::fabs(x), std::fabs(y))); };
            const editor::ShakeCustom& p = a.custom;
            const editor::ShakeCustom& q = b.custom;
            return a.profile == b.profile && nearly(a.gain, b.gain) && nearly(p.amp, q.amp) && nearly(p.hz, q.hz) &&
                nearly(p.speedAmp, q.speedAmp) && nearly(p.speedHz, q.speedHz) && nearly(p.rotWeight, q.rotWeight) &&
                nearly(p.posWeight, q.posWeight) && nearly(p.speedRef, q.speedRef) && p.seed == q.seed && p.still == q.still &&
                nearly(a.cycles, b.cycles) && nearly(a.fadeMs, b.fadeMs) && a.originMs == b.originMs;
        }
    }

    void Migrate(Track& track)
    {
        if (!track.HasLegacyShake())
            return;
        ShakeMigrationReport report;
        report.ran = true;
        auto before = std::make_shared<Track>(track);
        before->Migration() = {};
        const Track& old = *before;
        const fx::EffectTrack& effects = old.LegacyEffects();
        report.events = static_cast<std::uint32_t>(effects.Events().size());
        for (const Keyframe& k : old.Keyframes())
            if (k.legacyShake.type != editor::ShakeType::None)
                ++report.legacyMarkers;
        const auto eventsOn = [&](std::uint64_t t) { return fx::StateAt(effects, static_cast<double>(t)).enabled; };

        // 1. Where the events' shake changes while it plays (or stops).
        std::vector<std::uint64_t> breakpoints;
        for (std::uint64_t t : EventTimes(effects))
            if (eventsOn(t) || (t > 0 && eventsOn(t - 1)))
                breakpoints.push_back(t);
        report.breakpoints = static_cast<std::uint32_t>(breakpoints.size());
        // A Preset that ramps: the legacy phase took the frequency as a straight line
        // over the ramp while the preset stepped (camera_effects.cpp Integrate); the
        // markers step it. Counted as not held exactly.
        {
            const fx::ShakeEvent* previous = nullptr;
            for (const fx::ShakeEvent& e : effects.Events())
            {
                if (e.kind != fx::Kind::Preset)
                    continue;
                if (previous && e.mode == fx::Mode::Ramp && e.value != previous->value && eventsOn(e.timeMs))
                    ++report.inexact;
                previous = &e;
            }
        }
        for (const fx::ShakeEvent& e : effects.Events())
            for (const Keyframe& k : old.Keyframes())
                if (k.timeMs == e.timeMs && k.markerType != MarkerType::Anchor)
                {
                    ++report.eventsOnMarkers;
                    break;
                }

        // 2. Each one on the marker at its time, else on an Anchor added
        // there (the camera skips it: the path is the same).
        std::vector<std::uint32_t> added;
        const Scene noScene;
        for (std::uint64_t t : breakpoints)
        {
            Keyframe* there = nullptr;
            for (const Keyframe& k : track.Keyframes())
                if (k.timeMs == t)
                    there = track.FindMutable(k.id);
            if (there && HoldsShake(*there))
                continue;
            if (there)
            {
                there->origin = kMarkerOriginShakeMigration; // a plain Anchor there holds the shake from then on
                continue;
            }
            Keyframe anchor;
            const Keyframe* previous = nullptr;
            for (const Keyframe& k : track.Keyframes())
                if (k.timeMs <= t)
                    previous = &k;
            if (previous)
                InheritFrom(anchor, *previous);
            anchor.timeMs = t;
            anchor.markerType = MarkerType::Anchor;
            anchor.origin = kMarkerOriginShakeMigration;
            anchor.legacyShake = {};
            anchor.transition = 0;
            anchor.editedMask = 0;
            editor::CameraState view{};
            if (EvaluateMarkersRaw(old, t, noScene, view))
            {
                // The camera as the curve had it there (it changes nothing:
                // an Anchor; made an Edit marker later it starts there).
                anchor.cameraMode = CameraMode::Free;
                anchor.pose.position = view.position;
                anchor.pose.orientation = Track::FromEulerDegrees(view.rotation);
                anchor.pose.fov = view.fov;
            }
            const std::uint32_t id = track.InsertMigrated(anchor);
            if (id)
                added.push_back(id);
            else
                ++report.inexact; // the track is full (512 markers)
        }

        // 3. Each shake marker's state at its time: the events' while their shake is
        // on, the legacy marker shake otherwise.
        std::vector<Keyframe>& keys = const_cast<std::vector<Keyframe>&>(track.Keyframes());
        const std::vector<int> list = ShakeMarkers(track);
        std::vector<std::uint8_t> fromEvents(list.size(), 0);
        std::vector<std::uint8_t> kept(list.size(), 0);
        for (std::size_t n = 0; n < list.size(); ++n)
        {
            Keyframe& k = keys[list[n]];
            const std::uint64_t t = k.timeMs;
            if (!eventsOn(t) && k.shake.on && k.legacyShake.type == editor::ShakeType::None)
            {
                // A marker that already has the new shake (a director's or a bench line with
                // the new keys, a migrated one) keeps it.
                kept[n] = 1;
                continue;
            }
            if (eventsOn(t))
            {
                fromEvents[n] = 1;
                const fx::State st = fx::StateAt(effects, static_cast<double>(t));
                MarkerShake s = FromEvents(st);
                const bool going = n > 0 && fromEvents[n - 1] && st.onSinceMs <= static_cast<double>(keys[list[n - 1]].timeMs) &&
                    st.randomizedMs <= static_cast<double>(keys[list[n - 1]].timeMs);
                if (!going)
                {
                    // Turned on, a new pattern, or the events take over from the markers' shake:
                    // the phase and the fade in as they were there.
                    const fx::FxShake ev = fx::Evaluate(effects, t);
                    s.flags |= editor::ShakeFlagRestart;
                    s.startCycles = ev.cycles;
                    s.startFadeMs = ev.sinceOnMs;
                }
                k.shake = s;
                continue;
            }
            const SegmentPlan plan = Plan(old, t);
            const Keyframe* current = plan.valid ? &old.Keyframes()[plan.current] : nullptr;
            if (!current || current->legacyShake.type == editor::ShakeType::None)
            {
                MarkerShake s = editor::DefaultMarkerShake();
                s.blendMask = 0;
                k.shake = s;
                continue;
            }
            const Keyframe::LegacyShake& l = current->legacyShake;
            MarkerShake s = FromLegacy(l);
            const Keyframe* next = plan.next >= 0 ? &old.Keyframes()[plan.next] : nullptr;
            if (plan.blending && next && next->legacyShake.type == l.type)
            {
                // Inside a blend (an Anchor added there): its state there.
                s.gain = shake::Gain(l.intensity) + (shake::Gain(next->legacyShake.intensity) - shake::Gain(l.intensity)) * plan.phase;
                if (l.type == editor::ShakeType::Custom)
                {
                    const editor::ShakeCustom c = shake::LerpCustom(l.custom, next->legacyShake.custom, plan.rawPhase);
                    s.amp = c.amp;
                    s.hz = c.hz;
                    s.speedAmp = c.speedAmp;
                    s.speedHz = c.speedHz;
                    s.rotWeight = c.rotWeight;
                    s.posWeight = c.posWeight;
                    s.speedRef = c.speedRef;
                }
            }
            // The clock: the shake speed over the marker's speed.
            if (k.speedIndex == current->speedIndex)
            {
                s.flags |= editor::ShakeFlagRealTime;
                s.rate = SpeedFactor(l.speedIndex);
            }
            else
                s.rate = static_cast<float>(static_cast<double>(SpeedFactor(l.speedIndex)) / SpeedFactor(current->speedIndex));
            bool going = false;
            if (n > 0 && !fromEvents[n - 1] && keys[list[n - 1]].shake.on)
            {
                const SegmentPlan earlier = Plan(old, keys[list[n - 1]].timeMs);
                going = earlier.valid && earlier.shake.type == l.type && earlier.shake.runStartMs == plan.shake.runStartMs;
            }
            if (!going)
            {
                s.flags |= editor::ShakeFlagRestart;
                s.startCycles = l.type == editor::ShakeType::Custom ? plan.shake.cycles : plan.shake.timeMs / (1000.0 / kProfileClockHz);
                s.startFadeMs = plan.shake.timeMs;
            }
            k.shake = s;
        }

        // 4. The transitions to the next shake marker.
        for (std::size_t n = 0; n < list.size(); ++n)
        {
            Keyframe& k = keys[list[n]];
            MarkerShake& s = k.shake;
            if (kept[n] || !s.on || n + 1 >= list.size())
                continue;
            Keyframe& j = keys[list[n + 1]];
            const std::uint64_t t0 = k.timeMs, t1 = j.timeMs;
            if (fromEvents[n])
            {
                // Each number: constant, or a straight line (a ramp) that
                // reaches the next marker's value.
                const double mid = 0.5 * (static_cast<double>(t0) + static_cast<double>(t1));
                const double quarter = 0.75 * static_cast<double>(t0) + 0.25 * static_cast<double>(t1);
                const fx::State m = fx::StateAt(effects, mid);
                const fx::State q = fx::StateAt(effects, quarter);
                if (!m.enabled || m.profile != s.profile || m.seed != s.seed || (m.still ? 1 : 0) != s.still)
                    ++report.inexact;
                std::uint16_t mask = 0;
                for (int f = 0; f < kFieldCount; ++f)
                {
                    const float v0 = s.*kFields[f];
                    const float vm = FieldOf(m, f);
                    const float limit = 2.0f * vm - v0;
                    if (Close(vm, v0) && Close(FieldOf(q, f), v0))
                        continue;
                    if (!Close(FieldOf(q, f), 0.75f * v0 + 0.25f * limit))
                        ++report.inexact;
                    mask |= static_cast<std::uint16_t>(1u << f);
                    if (fromEvents[n + 1])
                    {
                        if (!Close(j.shake.*kFields[f], limit))
                            ++report.inexact;
                    }
                    else if (!j.shake.on)
                        j.shake.*kFields[f] = limit; // the next one is off: its numbers are only the ramp's end
                    else if (!Close(j.shake.*kFields[f], limit))
                        ++report.inexact;
                }
                if (mask)
                {
                    s.transition = static_cast<std::uint8_t>(editor::ShakeTransition::Blend);
                    s.blendMask = mask;
                }
                continue;
            }
            const SegmentPlan plan = Plan(old, t0);
            const Keyframe& current = old.Keyframes()[plan.current];
            const Keyframe* next = plan.next >= 0 ? &old.Keyframes()[plan.next] : nullptr;
            const editor::ShakeType type = current.legacyShake.type;
            if (plan.blending && next && next->legacyShake.type == type)
            {
                // The intensity blends (and a Custom's numbers):
                // only what changes from this marker to the next.
                std::uint16_t mask = 0;
                if (shake::Gain(current.legacyShake.intensity) != shake::Gain(next->legacyShake.intensity))
                    mask |= editor::ShakeBlendGain;
                if (type == editor::ShakeType::Custom)
                {
                    const editor::ShakeCustom& p = current.legacyShake.custom;
                    const editor::ShakeCustom& q = next->legacyShake.custom;
                    const float pv[7] = {p.amp, p.hz, p.speedAmp, p.speedHz, p.rotWeight, p.posWeight, p.speedRef};
                    const float qv[7] = {q.amp, q.hz, q.speedAmp, q.speedHz, q.rotWeight, q.posWeight, q.speedRef};
                    for (int f = 0; f < 7; ++f)
                        if (pv[f] != qv[f])
                            mask |= static_cast<std::uint16_t>(1u << (f + 1));
                }
                if (mask)
                {
                    s.transition = static_cast<std::uint8_t>(editor::ShakeTransition::Blend);
                    s.blendMask = mask;
                    if (mask & editor::ShakeBlendGain)
                        s.flags |= editor::ShakeFlagEaseWithCamera;
                    if (next->timeMs != t1 || fromEvents[n + 1])
                        ++report.inexact;
                }
            }
            // The fade out before a marker with another shake.
            const Keyframe* following = next;
            for (std::size_t m = 0; !following && m < old.Keyframes().size(); ++m)
            {
                const Keyframe& f = old.Keyframes()[m];
                if (f.timeMs > current.timeMs && f.markerType != MarkerType::Anchor)
                    following = &f;
            }
            if (following && following->timeMs > t0 && following->legacyShake.type != type)
            {
                s.flags |= editor::ShakeFlagFadeOut;
                if (following->timeMs != t1 && following->timeMs < t1 + static_cast<std::uint64_t>(kFadeOutMs))
                    ++report.inexact;
                else if (following->timeMs != t1)
                    s.flags &= static_cast<std::uint8_t>(~editor::ShakeFlagFadeOut);
            }
        }

        // 4b. A one shot fires at a marker that sets it (FiresImpulse); the old
        // rules went on with the clock from before there (a blast long run
        // out). Such a marker keeps the old phase as a Restart from it, so the
        // old clip plays as it did. In order: an earlier one is set first.
        for (std::size_t n = 0; n < list.size(); ++n)
        {
            Keyframe& k = keys[list[n]];
            if (kept[n] || (k.shake.flags & editor::ShakeFlagRestart) != 0 ||
                !FiresImpulse(n > 0 ? &keys[list[n - 1]].shake : nullptr, k.shake))
                continue;
            const Resolved was = ResolveRules(track, k.timeMs, false);
            if (was.marker != list[n] || was.originMs == k.timeMs)
                continue; // the old phase starts here too
            k.shake.flags |= editor::ShakeFlagRestart;
            k.shake.startCycles = was.cycles;
            k.shake.startFadeMs = was.fadeMs;
        }

        // 5. An added Anchor that changes nothing goes again.
        for (std::uint32_t id : added)
        {
            const Keyframe* anchor = track.Find(id);
            if (!anchor)
                continue;
            const std::vector<int> now = ShakeMarkers(track);
            int at = -1;
            for (std::size_t n = 0; n < now.size(); ++n)
                if (keys[now[n]].id == id)
                    at = static_cast<int>(n);
            if (at <= 0)
                continue;
            const std::uint64_t ta = anchor->timeMs;
            const std::uint64_t tp = keys[now[at - 1]].timeMs;
            const std::uint64_t tn = at + 1 < static_cast<int>(now.size()) ? keys[now[at + 1]].timeMs : ta + 2000;
            Track without = track;
            std::vector<Keyframe>& w = const_cast<std::vector<Keyframe>&>(without.Keyframes());
            w.erase(std::remove_if(w.begin(), w.end(), [id](const Keyframe& k) { return k.id == id; }), w.end());
            bool same = true;
            for (std::uint64_t t : {tp, (tp + ta) / 2, ta > tp + 125 ? ta - 125 : tp, ta - 1, ta, (ta + tn) / 2,
                     tn > ta + 125 ? tn - 125 : ta, tn > ta ? tn - 1 : ta})
                same = same && SameResolved(Resolve(track, t), Resolve(without, t));
            if (same)
            {
                track = std::move(without);
                ++report.pruned;
            }
        }

        // 6. Nothing legacy is left.
        for (Keyframe& k : const_cast<std::vector<Keyframe>&>(track.Keyframes()))
            k.legacyShake = {};
        track.LegacyEffects().Clear();
        for (const Keyframe& k : track.Keyframes())
            if (k.origin == kMarkerOriginShakeMigration && k.markerType == MarkerType::Anchor)
                ++report.markersAdded;
        report.before = before;
        track.Migration() = report;
    }

    Proof Verify(const Track& before, const Track& after, const Scene& scene, std::uint64_t durationMs, std::uint32_t stepMs)
    {
        Proof proof;
        ShakePhaseCache cacheBefore, cacheAfter;
        Scene sb = scene;
        sb.shakePhase = &cacheBefore;
        Scene sa = scene;
        sa.shakePhase = &cacheAfter;
        // The sample times, ascending and each once: the frame grid (each 30 Hz
        // frame, or every stepMs) merged with the markers' times (and 1 ms
        // either side), walked without a table of the grid (its size is the
        // declared duration's; only the markers' few times are held).
        std::vector<std::uint64_t> marks;
        for (const Track* track : {&before, &after})
            for (const Keyframe& k : track->Keyframes())
                for (std::uint64_t t : {k.timeMs > 0 ? k.timeMs - 1 : 0ull, k.timeMs, k.timeMs + 1})
                    if (t <= durationMs)
                        marks.push_back(t);
        std::sort(marks.begin(), marks.end());
        marks.erase(std::unique(marks.begin(), marks.end()), marks.end());
        std::uint64_t frame = 0;
        const auto gridAt = [&](std::uint64_t n) { return stepMs == 0 ? (n * 1000ull + 15ull) / 30ull : n * stepMs; };
        std::size_t mark = 0;
        const auto nextTime = [&](std::uint64_t& t) {
            const bool grid = gridAt(frame) <= durationMs;
            const bool marker = mark < marks.size();
            if (!grid && !marker)
                return false;
            t = !grid ? marks[mark] : !marker ? gridAt(frame) : std::min(gridAt(frame), marks[mark]);
            if (grid && gridAt(frame) == t)
                ++frame;
            if (marker && marks[mark] == t)
                ++mark;
            return true;
        };
        const auto angle = [](float a, float b) {
            float d = std::fmod(a - b, 360.0f);
            if (d > 180.0f)
                d -= 360.0f;
            if (d < -180.0f)
                d += 360.0f;
            return static_cast<double>(std::fabs(d));
        };
        double worst = -1.0;
        for (std::uint64_t t = 0; nextTime(t);)
        {
            editor::CameraState fb{}, rb{}, fa{}, ra{};
            const bool okB = EvaluateMarkers(before, t, sb, fb) && EvaluateMarkersRaw(before, t, sb, rb);
            const bool okA = EvaluateMarkers(after, t, sa, fa) && EvaluateMarkersRaw(after, t, sa, ra);
            ++proof.samples;
            if (okB != okA)
            {
                proof.pathPosition = std::max(proof.pathPosition, 1e9);
                continue;
            }
            if (!okB)
                continue;
            const auto dist = [](const editor::Vec3& a, const editor::Vec3& b) {
                const double x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
                return std::sqrt(x * x + y * y + z * z);
            };
            proof.pathPosition = std::max(proof.pathPosition, dist(ra.position, rb.position));
            proof.pathRotation = std::max({proof.pathRotation, angle(ra.rotation.x, rb.rotation.x), angle(ra.rotation.y, rb.rotation.y),
                angle(ra.rotation.z, rb.rotation.z)});
            proof.pathFov = std::max(proof.pathFov, static_cast<double>(std::fabs(ra.fov - rb.fov)));
            // The camera as rendered (the path with the shake): its float
            // pose rounds the sum (one step of a float near 180 degrees is
            // 1.5e-5), so this is the picture's own precision.
            proof.cameraPosition = std::max(proof.cameraPosition, dist(fa.position, fb.position));
            proof.cameraRotation = std::max({proof.cameraRotation, angle(fa.rotation.x, fb.rotation.x), angle(fa.rotation.y, fb.rotation.y),
                angle(fa.rotation.z, fb.rotation.z)});
            // The shake itself: what each model adds at t (the legacy rules on the old
            // track, the markers' on the new one), compared before it is added to the camera.
            ShakeDelta oldDelta = Delta(before, t, sb);
            if (before.HasLegacyShake())
            {
                const ShakeDelta legacy = LegacyDelta(before, t, sb);
                oldDelta.position = {oldDelta.position.x + legacy.position.x, oldDelta.position.y + legacy.position.y,
                    oldDelta.position.z + legacy.position.z};
                oldDelta.rotation = {oldDelta.rotation.x + legacy.rotation.x, oldDelta.rotation.y + legacy.rotation.y,
                    oldDelta.rotation.z + legacy.rotation.z};
            }
            const ShakeDelta newDelta = Delta(after, t, sa);
            proof.peakRotation = std::max({proof.peakRotation, std::fabs(static_cast<double>(oldDelta.rotation.x)),
                std::fabs(static_cast<double>(oldDelta.rotation.y)), std::fabs(static_cast<double>(oldDelta.rotation.z))});
            proof.peakPosition = std::max(proof.peakPosition, dist(oldDelta.position, editor::Vec3{}));
            const double position = dist(oldDelta.position, newDelta.position);
            const double rotation = std::max({std::fabs(static_cast<double>(oldDelta.rotation.x) - newDelta.rotation.x),
                std::fabs(static_cast<double>(oldDelta.rotation.y) - newDelta.rotation.y),
                std::fabs(static_cast<double>(oldDelta.rotation.z) - newDelta.rotation.z)});
            proof.shakePosition = std::max(proof.shakePosition, position);
            proof.shakeRotation = std::max(proof.shakeRotation, rotation);
            if (std::max(position, rotation) > worst)
            {
                worst = std::max(position, rotation);
                proof.worstMs = t;
            }
        }
        return proof;
    }

    std::string FormatLine(const ShakeMigrationReport& report, const Proof& proof)
    {
        char text[640]{};
        if (proof.skipped)
        {
            std::snprintf(text, sizeof(text),
                "SHAKE_MIGRATION,legacy_markers=%u,events=%u,on_markers=%u,breakpoints=%u,markers_added=%u,pruned=%u,inexact=%u,"
                "proof=skipped",
                report.legacyMarkers, report.events, report.eventsOnMarkers, report.breakpoints, report.markersAdded, report.pruned,
                report.inexact);
            return text;
        }
        std::snprintf(text, sizeof(text),
            "SHAKE_MIGRATION,legacy_markers=%u,events=%u,on_markers=%u,breakpoints=%u,markers_added=%u,pruned=%u,inexact=%u,"
            "samples=%u,max_rot_deg=%.3g,max_pos_m=%.3g,max_path_m=%.3g,max_path_deg=%.3g,max_cam_deg=%.3g,max_cam_m=%.3g,"
            "peak_rot_deg=%.3g,peak_pos_m=%.3g,worst_ms=%llu,exact=%d",
            report.legacyMarkers, report.events, report.eventsOnMarkers, report.breakpoints, report.markersAdded, report.pruned,
            report.inexact, proof.samples, proof.shakeRotation, proof.shakePosition, proof.pathPosition, proof.pathRotation,
            proof.cameraRotation, proof.cameraPosition, proof.peakRotation, proof.peakPosition,
            static_cast<unsigned long long>(proof.worstMs), proof.Exact() ? 1 : 0);
        return text;
    }

    // ------------------------------------------------------------------
    // The director's and the bench's fields.

    bool ParseField(const std::string& key, const std::string& value, MarkerShake& s, bool& known, std::string& error)
    {
        known = true;
        char* end = nullptr;
        const auto number = [&](float& field, Range range) {
            const float v = std::strtof(value.c_str(), &end);
            if (end == value.c_str() || *end || !Inside(v, range))
            {
                error = key + " out of range: " + value;
                return false;
            }
            field = v;
            return true;
        };
        const auto whole = [&](long top, long& out) {
            out = std::strtol(value.c_str(), &end, 10);
            if (end == value.c_str() || *end || out < 0 || out > top)
            {
                error = key + " out of range: " + value;
                return false;
            }
            return true;
        };
        long v = 0;
        if (key == "shake")
        {
            if (value == "off" || value == "none")
            {
                s.on = 0;
                return true;
            }
            static const char* const kPresetKeys[kPresetCount] = {"subtle", "simple_handheld", "vehicle", "earthquake"};
            for (std::uint8_t p = 0; p < kPresetCount; ++p)
                if (value == kPresetKeys[p])
                {
                    ApplyPreset(s, p);
                    return true;
                }
            if (value == "noise" || value == "custom")
            {
                s.on = 1;
                s.profile = 0;
                return true;
            }
            editor::ShakeType type = editor::ShakeType::None;
            if (!fx::ParseShakeName(value, type) || type == editor::ShakeType::None)
            {
                error = "unknown shake: " + value;
                return false;
            }
            ApplyProfile(s, static_cast<std::uint8_t>(type));
            return true;
        }
        if (key == "shake_i")
        {
            if (!whole(10, v) || v < 1)
            {
                error = "shake_i 1..10: " + value;
                return false;
            }
            s.gain = GainOf(static_cast<std::uint8_t>(v));
            return true;
        }
        if (key == "shake_gain")
            return number(s.gain, kGainRange);
        if (key == "shake_amp")
            return number(s.amp, kAmpRange);
        if (key == "shake_hz")
            return number(s.hz, kHzRange);
        if (key == "shake_spdamp")
            return number(s.speedAmp, kCouplingRange);
        if (key == "shake_spdhz")
            return number(s.speedHz, kCouplingRange);
        if (key == "shake_rot")
            return number(s.rotWeight, kWeightRange);
        if (key == "shake_pos")
            return number(s.posWeight, kWeightRange);
        if (key == "shake_ref")
            return number(s.speedRef, kSpeedRefRange);
        if (key == "shake_rate")
            return number(s.rate, kRateRange);
        if (key == "shake_still")
        {
            if (!whole(1, v))
                return false;
            s.still = static_cast<std::uint8_t>(v);
            return true;
        }
        if (key == "shake_seed")
        {
            if (!whole(65535, v))
                return false;
            s.seed = static_cast<std::uint16_t>(v);
            return true;
        }
        if (key == "shake_speed")
        {
            if (!whole(200, v))
                return false;
            for (std::uint32_t n = 0; n < editor::kMarkerSpeedCount; ++n)
                if (static_cast<long>(editor::kMarkerSpeedPercent[n]) == v)
                {
                    s.rate = static_cast<float>(v) / 100.0f;
                    return true;
                }
            error = "shake_speed one of 5 20 35 50 100 125 150 175 200: " + value;
            return false;
        }
        if (key == "shake_s") // the bench's legacy speed index
        {
            if (!whole(8, v))
                return false;
            s.rate = SpeedFactor(static_cast<std::uint8_t>(v));
            return true;
        }
        if (key == "shake_realtime" || key == "shake_restart")
        {
            if (!whole(1, v))
                return false;
            const std::uint8_t bit = key == "shake_realtime" ? editor::ShakeFlagRealTime : editor::ShakeFlagRestart;
            s.flags = static_cast<std::uint8_t>(v ? (s.flags | bit) : (s.flags & ~bit));
            return true;
        }
        if (key == "shake_transition")
        {
            if (value != "cut" && value != "blend")
            {
                error = "shake_transition cut|blend: " + value;
                return false;
            }
            s.transition = static_cast<std::uint8_t>(value == "blend" ? editor::ShakeTransition::Blend : editor::ShakeTransition::Cut);
            if (value == "blend" && s.blendMask == 0)
                s.blendMask = editor::ShakeBlendAll;
            return true;
        }
        if (key == "shake_motion")
        {
            // "With movement" on a profile: speed 15 m/s doubles it.
            if (value != "off" && value != "speed" && value != "still")
            {
                error = "shake_motion off|speed|still: " + value;
                return false;
            }
            s.speedAmp = value == "off" ? 0.0f : 1.0f;
            s.speedRef = kShakeMotionFullSpeed;
            s.still = value == "still" ? 1 : 0;
            return true;
        }
        known = false;
        return true;
    }

    std::string FormatFields(const MarkerShake& s)
    {
        if (!s.on)
            return "shake=off";
        char text[448]{};
        const std::string name = s.profile ? fx::ShakeName(static_cast<editor::ShakeType>(s.profile)) : "noise";
        std::snprintf(text, sizeof(text),
            "shake=%s shake_gain=%.4g shake_amp=%.4g shake_hz=%.4g shake_spdamp=%.4g shake_spdhz=%.4g shake_rot=%.4g shake_pos=%.4g "
            "shake_ref=%.4g shake_still=%u shake_seed=%u shake_rate=%.4g shake_realtime=%d shake_transition=%s shake_restart=%d",
            name.c_str(), s.gain, s.amp, s.hz, s.speedAmp, s.speedHz, s.rotWeight, s.posWeight, s.speedRef,
            static_cast<unsigned>(s.still), static_cast<unsigned>(s.seed), s.rate, (s.flags & editor::ShakeFlagRealTime) ? 1 : 0,
            s.transition ? "blend" : "cut", (s.flags & editor::ShakeFlagRestart) ? 1 : 0);
        return text;
    }
}
