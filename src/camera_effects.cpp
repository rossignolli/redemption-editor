#include "camera_effects.h"

#include "camera_shake.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace replay::camera::fx
{
    namespace
    {
        bool Discrete(Kind k)
        {
            return k == Kind::OnOff || k == Kind::Preset || k == Kind::StopWhenStill || k == Kind::Randomize || k == Kind::Profile;
        }

        int RoundIndex(float v, int top)
        {
            const int n = static_cast<int>(std::floor(v + 0.5f));
            return n < 0 ? 0 : n > top ? top : n;
        }

        // One parameter of the fold at `at` (Simple Camera's ApplyEffectValue).
        void Apply(State& s, Kind kind, float v, double at)
        {
            const auto turnOn = [&](double when) {
                if (!s.enabled)
                {
                    s.enabled = true;
                    s.onSinceMs = when;
                }
            };
            switch (kind)
            {
            case Kind::OnOff:
                if (v >= 0.5f)
                    turnOn(at);
                else
                    s.enabled = false;
                break;
            case Kind::Preset:
            {
                const PresetValues& p = kPresets[RoundIndex(v, kPresetCount - 1)];
                s.amp = p.amp;
                s.hz = p.hz;
                s.speedAmp = p.speedAmp;
                s.speedHz = p.speedHz;
                if (RoundIndex(v, kPresetCount - 1) > 0)
                    turnOn(at); // the mod: a preset other than Off enables the shake
                break;
            }
            case Kind::Amp: s.amp = v; break;
            case Kind::Freq: s.hz = v; break;
            case Kind::SpeedAmp: s.speedAmp = v; break;
            case Kind::SpeedFreq: s.speedHz = v; break;
            case Kind::RotWeight: s.rotWeight = v; break;
            case Kind::PosWeight: s.posWeight = v; break;
            case Kind::StopWhenStill: s.still = v >= 0.5f; break;
            case Kind::Randomize:
                s.seed = static_cast<std::uint16_t>(RoundIndex(v, 65535));
                s.randomizedMs = at;
                break;
            case Kind::Profile:
                s.profile = static_cast<std::uint8_t>(RoundIndex(v, 10));
                if (s.profile > 0)
                    turnOn(at);
                break;
            }
        }

        double SpeedOf(std::uint8_t index)
        {
            const std::uint32_t i = std::min<std::uint32_t>(index, editor::kMarkerSpeedCount - 1);
            return static_cast<double>(editor::kMarkerSpeedPercent[i]) / 100.0;
        }

        bool Number(const std::string& text, float& out)
        {
            char* end = nullptr;
            out = std::strtof(text.c_str(), &end);
            return end != text.c_str() && !*end && std::isfinite(out);
        }
    }

    bool ParseKind(const std::string& key, Kind& kind)
    {
        for (std::uint8_t i = 0; i < kKindCount; ++i)
            if (key == kKinds[i].key)
            {
                kind = static_cast<Kind>(i);
                return true;
            }
        return false;
    }

    bool Valid(const ShakeEvent& e)
    {
        const std::uint8_t k = static_cast<std::uint8_t>(e.kind);
        if (k >= kKindCount || static_cast<std::uint8_t>(e.mode) >= kModeCount || e.timeMs > 0xFFFFFFFFull)
            return false;
        const KindInfo& info = Info(e.kind);
        if (!std::isfinite(e.value) || e.value < info.minimum || e.value > info.maximum)
            return false;
        return !(e.kind == Kind::Randomize && e.mode == Mode::Ramp);
    }

    void EffectTrack::Sort()
    {
        std::stable_sort(events_.begin(), events_.end(), [](const ShakeEvent& a, const ShakeEvent& b) {
            return a.timeMs != b.timeMs ? a.timeMs < b.timeMs : a.id < b.id;
        });
    }

    std::uint32_t EffectTrack::Add(const ShakeEvent& event)
    {
        ShakeEvent added = event;
        if (static_cast<std::uint8_t>(added.kind) >= kKindCount || events_.size() >= kMaxEvents)
            return 0;
        added.value = ClampValue(added.kind, added.value);
        if (added.kind == Kind::Randomize)
            added.mode = Mode::Snap;
        if (!Valid(added))
            return 0;
        added.id = nextId_++;
        events_.push_back(added);
        Sort();
        return added.id;
    }

    bool EffectTrack::Remove(std::uint32_t id)
    {
        const auto it = std::find_if(events_.begin(), events_.end(), [id](const ShakeEvent& e) { return e.id == id; });
        if (it == events_.end())
            return false;
        events_.erase(it);
        return true;
    }

    bool EffectTrack::Move(std::uint32_t id, std::uint64_t timeMs)
    {
        const auto it = std::find_if(events_.begin(), events_.end(), [id](const ShakeEvent& e) { return e.id == id; });
        if (it == events_.end() || timeMs > 0xFFFFFFFFull)
            return false;
        it->timeMs = timeMs;
        Sort();
        return true;
    }

    bool EffectTrack::Update(std::uint32_t id, const ShakeEvent& values)
    {
        const auto it = std::find_if(events_.begin(), events_.end(), [id](const ShakeEvent& e) { return e.id == id; });
        if (it == events_.end() || static_cast<std::uint8_t>(values.kind) >= kKindCount)
            return false;
        ShakeEvent changed = values;
        changed.id = it->id;
        changed.timeMs = it->timeMs;
        changed.value = ClampValue(changed.kind, changed.value);
        if (changed.kind == Kind::Randomize)
            changed.mode = Mode::Snap;
        if (!Valid(changed))
            return false;
        *it = changed;
        return true;
    }

    const ShakeEvent* EffectTrack::Find(std::uint32_t id) const
    {
        for (const ShakeEvent& e : events_)
            if (e.id == id)
                return &e;
        return nullptr;
    }

    void EffectTrack::Clear()
    {
        events_.clear();
        nextId_ = 1;
    }

    bool EffectTrack::Assign(std::vector<ShakeEvent> events, std::uint32_t nextId)
    {
        if (events.size() > kMaxEvents)
            return false;
        for (std::size_t n = 0; n < events.size(); ++n)
        {
            if (!Valid(events[n]) || events[n].id == 0 || events[n].id >= nextId)
                return false;
            if (n > 0 && events[n].timeMs < events[n - 1].timeMs)
                return false;
            for (std::size_t m = 0; m < n; ++m)
                if (events[m].id == events[n].id)
                    return false;
        }
        events_ = std::move(events);
        nextId_ = nextId;
        Sort();
        return true;
    }

    State StateAt(const EffectTrack& track, double timeMs)
    {
        State s;
        const std::vector<ShakeEvent>& events = track.Events();
        for (const ShakeEvent& e : events)
        {
            if (static_cast<double>(e.timeMs) > timeMs)
                break;
            Apply(s, e.kind, e.value, static_cast<double>(e.timeMs));
            s.lastEventId = e.id;
        }
        // The ramps in flight (the mod's DriveEvents pass 2): from the last
        // event of the kind at or before t to the first after t, when that
        // one is a Ramp; a ramp with no event before it acts at its time.
        for (std::uint8_t k = 0; k < kKindCount; ++k)
        {
            const Kind kind = static_cast<Kind>(k);
            if (kind == Kind::Randomize)
                continue;
            const ShakeEvent* prev = nullptr;
            const ShakeEvent* next = nullptr;
            for (const ShakeEvent& e : events)
            {
                if (e.kind != kind)
                    continue;
                if (static_cast<double>(e.timeMs) <= timeMs)
                    prev = &e;
                else
                {
                    next = &e;
                    break;
                }
            }
            if (!prev || !next || next->mode != Mode::Ramp || next->timeMs <= prev->timeMs)
                continue;
            const double span = static_cast<double>(next->timeMs - prev->timeMs);
            const double u = (timeMs - static_cast<double>(prev->timeMs)) / span;
            const float v = prev->value + (next->value - prev->value) * static_cast<float>(u);
            double at = timeMs;
            if (Discrete(kind) && next->value != prev->value)
            {
                // A switch or an index changes where the ramp crosses the
                // half step; the shake turned on there (not at every t).
                const double cross = (0.5 - prev->value) / (next->value - prev->value);
                at = static_cast<double>(prev->timeMs) + std::clamp(cross, 0.0, 1.0) * span;
            }
            Apply(s, kind, v, at);
            s.ramping = true;
        }
        return s;
    }

    float ValueOf(const State& s, Kind kind)
    {
        switch (kind)
        {
        case Kind::OnOff: return s.enabled ? 1.0f : 0.0f;
        case Kind::Preset: return 0.0f;
        case Kind::Amp: return s.amp;
        case Kind::Freq: return s.hz;
        case Kind::SpeedAmp: return s.speedAmp;
        case Kind::SpeedFreq: return s.speedHz;
        case Kind::RotWeight: return s.rotWeight;
        case Kind::PosWeight: return s.posWeight;
        case Kind::StopWhenStill: return s.still ? 1.0f : 0.0f;
        case Kind::Randomize: return static_cast<float>(s.seed);
        case Kind::Profile: return static_cast<float>(s.profile);
        }
        return 0.0f;
    }

    namespace
    {
        // The frequency integrated from `from` to `to` (cycles): split at the
        // events that change it (Freq, Preset), each piece by the trapezoid,
        // exact on a ramp's straight line.
        double Integrate(const EffectTrack& track, double from, double to)
        {
            if (!(to > from))
                return 0.0;
            std::vector<double> cuts{from};
            for (const ShakeEvent& e : track.Events())
            {
                const double t = static_cast<double>(e.timeMs);
                if ((e.kind == Kind::Freq || e.kind == Kind::Preset) && t > from && t < to)
                    cuts.push_back(t);
            }
            cuts.push_back(to);
            double cycles = 0.0;
            for (std::size_t n = 0; n + 1 < cuts.size(); ++n)
            {
                const double a = cuts[n], b = cuts[n + 1];
                if (!(b > a))
                    continue;
                const double hzA = StateAt(track, a).hz;
                const double hzB = StateAt(track, std::max(a, b - 1e-6)).hz;
                cycles += (hzA + hzB) * 0.5 * (b - a) / 1000.0;
            }
            return cycles;
        }
    }

    FxShake Evaluate(const EffectTrack& track, std::uint64_t timeMs)
    {
        FxShake out;
        const double t = static_cast<double>(timeMs);
        const State s = StateAt(track, t);
        out.eventId = s.lastEventId;
        out.ramping = s.ramping;
        if (!s.enabled || s.onSinceMs < 0.0)
            return out;
        out.active = true;
        out.profile = static_cast<ShakeType>(s.profile);
        editor::ShakeCustom c = editor::DefaultShakeCustom();
        c.amp = s.amp;
        c.hz = std::max(0.0f, s.hz);
        c.speedAmp = s.speedAmp;
        c.speedHz = s.speedHz;
        c.rotWeight = s.rotWeight;
        c.posWeight = s.posWeight;
        c.speedRef = 30.0f; // the mod's g_ShakeSpeedRefMax
        c.seed = s.seed;
        c.still = s.still ? 1 : 0;
        out.custom = c;
        const double origin = std::max(s.onSinceMs, s.randomizedMs);
        out.runStartMs = static_cast<std::uint64_t>(std::floor(std::max(0.0, origin)));
        out.sinceOnMs = std::max(0.0, t - s.onSinceMs);
        out.cycles = Integrate(track, origin, t);
        return out;
    }

    const char* ShakeName(ShakeType type)
    {
        static const char* const kNames[] = {"none", "handheld", "drunk", "ground_vibration", "air_turbulence", "explosion",
                                             "walking", "running", "horseback", "steadicam", "wagon", "custom", "gunshot"};
        const std::uint8_t i = static_cast<std::uint8_t>(type);
        return i < editor::kShakeTypeCount ? kNames[i] : "none";
    }

    bool ParseShakeName(const std::string& name, ShakeType& type)
    {
        for (std::uint8_t i = 0; i < editor::kShakeTypeCount; ++i)
        {
            if (name == ShakeName(static_cast<ShakeType>(i)))
            {
                type = static_cast<ShakeType>(i);
                return true;
            }
        }
        if (name == "ground" || name == "air")
        {
            type = name == "ground" ? ShakeType::GroundVibration : ShakeType::AirTurbulence;
            return true;
        }
        return false;
    }

    bool ParseEventFields(const std::vector<std::string>& tokens, ShakeEvent& event, std::string& error)
    {
        std::string valueText;
        for (const std::string& token : tokens)
        {
            const std::size_t eq = token.find('=');
            if (eq == std::string::npos)
            {
                error = "expected key=value: " + token;
                return false;
            }
            const std::string key = token.substr(0, eq), value = token.substr(eq + 1);
            if (key == "t")
            {
                char* end = nullptr;
                const unsigned long long t = std::strtoull(value.c_str(), &end, 10);
                if (end == value.c_str() || *end || t > 0xFFFFFFFFull)
                {
                    error = "bad t: " + value;
                    return false;
                }
                event.timeMs = t;
            }
            else if (key == "kind")
            {
                if (!ParseKind(value, event.kind))
                {
                    error = "kind onoff|preset|amp|freq|spdamp|spdfreq|rot|pos|still|randomize|profile: " + value;
                    return false;
                }
            }
            else if (key == "value")
                valueText = value; // read once the kind is known
            else if (key == "mode")
            {
                if (value == "snap")
                    event.mode = Mode::Snap;
                else if (value == "ramp")
                    event.mode = Mode::Ramp;
                else
                {
                    error = "mode snap|ramp: " + value;
                    return false;
                }
            }
            else
            {
                error = "unknown key: " + key;
                return false;
            }
        }
        if (!valueText.empty())
        {
            float v = 0.0f;
            bool named = false;
            if (event.kind == Kind::OnOff && (valueText == "on" || valueText == "off"))
            {
                v = valueText == "on" ? 1.0f : 0.0f;
                named = true;
            }
            else if (event.kind == Kind::Preset)
            {
                static const char* const kKeys[kPresetCount] = {"off", "subtle", "handheld", "vehicle", "earthquake"};
                for (std::uint8_t p = 0; p < kPresetCount; ++p)
                    if (valueText == kKeys[p])
                    {
                        v = static_cast<float>(p);
                        named = true;
                    }
            }
            else if (event.kind == Kind::Profile)
            {
                ShakeType type = ShakeType::None;
                if (ParseShakeName(valueText, type) && type != ShakeType::Custom)
                {
                    v = static_cast<float>(type);
                    named = true;
                }
            }
            if (!named && !Number(valueText, v))
            {
                error = "bad value: " + valueText;
                return false;
            }
            const KindInfo& info = Info(event.kind);
            if (v < info.minimum || v > info.maximum)
            {
                char text[96]{};
                std::snprintf(text, sizeof(text), "value %s out of %g..%g for %s", valueText.c_str(), info.minimum,
                    info.maximum, info.key);
                error = text;
                return false;
            }
            event.value = info.integer ? std::floor(v + 0.5f) : v;
        }
        if (event.kind == Kind::Randomize && event.mode == Mode::Ramp)
        {
            error = "randomize is snap only";
            return false;
        }
        return true;
    }

    std::string FormatEventFields(const ShakeEvent& e)
    {
        char text[128]{};
        std::snprintf(text, sizeof(text), "t=%llu kind=%s value=%.3f mode=%s", static_cast<unsigned long long>(e.timeMs),
            Info(e.kind).key, e.value, e.mode == Mode::Ramp ? "ramp" : "snap");
        return text;
    }

    // ------------------------------------------------------------------
    // CFXE v1/v2 and the old director lines.

    std::vector<ShakeEvent> ConvertLegacy(const std::vector<LegacyEvent>& old)
    {
        std::vector<ShakeEvent> out;
        std::uint32_t id = 1;
        bool has[kKindCount]{};
        float last[kKindCount]{};
        const auto emit = [&](std::uint64_t t, Kind kind, float value, Mode mode) {
            value = ClampValue(kind, value);
            const std::uint8_t k = static_cast<std::uint8_t>(kind);
            if (has[k] && last[k] == value)
                return; // only what changes
            if (!has[k] || kind == Kind::Randomize || Discrete(kind))
                mode = Mode::Snap;
            ShakeEvent e;
            e.id = id++;
            e.timeMs = t;
            e.kind = kind;
            e.value = value;
            e.mode = mode;
            out.push_back(e);
            has[k] = true;
            last[k] = value;
        };
        std::vector<LegacyEvent> sorted = old;
        std::stable_sort(sorted.begin(), sorted.end(), [](const LegacyEvent& a, const LegacyEvent& b) { return a.timeMs < b.timeMs; });
        const LegacyEvent* before = nullptr;
        std::uint16_t seed = 0;
        for (const LegacyEvent& e : sorted)
        {
            // An old Ramp ramped only from an event of the same type.
            const Mode mode = e.mode == Mode::Ramp && before && before->shake == e.shake ? Mode::Ramp : Mode::Snap;
            before = &e;
            if (e.shake == ShakeType::None)
            {
                emit(e.timeMs, Kind::OnOff, 0.0f, Mode::Snap);
                continue;
            }
            const float speed = static_cast<float>(SpeedOf(e.speedIndex));
            const float gain = shake::Gain(e.intensity);
            emit(e.timeMs, Kind::OnOff, 1.0f, Mode::Snap);
            if (e.shake == ShakeType::Custom)
            {
                const editor::ShakeCustom& c = e.custom;
                emit(e.timeMs, Kind::Profile, 0.0f, Mode::Snap);
                emit(e.timeMs, Kind::Amp, c.amp * gain, mode);
                emit(e.timeMs, Kind::Freq, c.hz * speed, mode);
                emit(e.timeMs, Kind::SpeedAmp, c.speedAmp, mode);
                emit(e.timeMs, Kind::SpeedFreq, c.speedHz, mode);
                emit(e.timeMs, Kind::RotWeight, c.rotWeight, mode);
                emit(e.timeMs, Kind::PosWeight, c.posWeight, mode);
                emit(e.timeMs, Kind::StopWhenStill, c.still ? 1.0f : 0.0f, Mode::Snap);
                if (c.seed != seed)
                {
                    seed = c.seed;
                    emit(e.timeMs, Kind::Randomize, static_cast<float>(c.seed), Mode::Snap);
                }
            }
            else
            {
                emit(e.timeMs, Kind::Profile, static_cast<float>(e.shake), Mode::Snap);
                emit(e.timeMs, Kind::Amp, gain, mode);
                emit(e.timeMs, Kind::Freq, static_cast<float>(kProfileClockHz) * speed, mode);
                emit(e.timeMs, Kind::SpeedAmp, e.motion != ShakeMotion::Off ? 1.0f : 0.0f, Mode::Snap);
                emit(e.timeMs, Kind::SpeedFreq, 0.0f, Mode::Snap);
                emit(e.timeMs, Kind::RotWeight, 1.0f, Mode::Snap);
                emit(e.timeMs, Kind::PosWeight, 1.0f, Mode::Snap);
                emit(e.timeMs, Kind::StopWhenStill, e.motion == ShakeMotion::SpeedStill ? 1.0f : 0.0f, Mode::Snap);
            }
        }
        return out;
    }

    bool IsLegacyLine(const std::vector<std::string>& tokens)
    {
        for (const std::string& token : tokens)
        {
            const std::string key = token.substr(0, token.find('='));
            if (key == "shake" || key == "motion" || key.rfind("shake_", 0) == 0)
                return true;
        }
        return false;
    }

    bool ParseLegacyFields(const std::vector<std::string>& tokens, LegacyEvent& event, std::string& error)
    {
        for (const std::string& token : tokens)
        {
            const std::size_t eq = token.find('=');
            if (eq == std::string::npos)
            {
                error = "expected key=value: " + token;
                return false;
            }
            const std::string key = token.substr(0, eq), value = token.substr(eq + 1);
            char* end = nullptr;
            if (key == "t")
            {
                const unsigned long long t = std::strtoull(value.c_str(), &end, 10);
                if (end == value.c_str() || *end || t > 0xFFFFFFFFull)
                {
                    error = "bad t: " + value;
                    return false;
                }
                event.timeMs = t;
            }
            else if (key == "shake")
            {
                if (!ParseShakeName(value, event.shake))
                {
                    error = "unknown shake: " + value;
                    return false;
                }
            }
            else if (key == "shake_i")
            {
                const long i = std::strtol(value.c_str(), &end, 10);
                if (end == value.c_str() || *end || i < 1 || i > 10)
                {
                    error = "shake_i 1..10: " + value;
                    return false;
                }
                event.intensity = static_cast<std::uint8_t>(i);
            }
            else if (key == "shake_speed")
            {
                const long percent = std::strtol(value.c_str(), &end, 10);
                bool found = false;
                for (std::uint32_t n = 0; n < editor::kMarkerSpeedCount && end != value.c_str() && !*end; ++n)
                    if (static_cast<long>(editor::kMarkerSpeedPercent[n]) == percent)
                    {
                        event.speedIndex = static_cast<std::uint8_t>(n);
                        found = true;
                    }
                if (!found)
                {
                    error = "shake_speed one of 5 20 35 50 100 125 150 175 200: " + value;
                    return false;
                }
            }
            else if (key == "motion")
            {
                if (value == "off")
                    event.motion = ShakeMotion::Off;
                else if (value == "speed")
                    event.motion = ShakeMotion::Speed;
                else if (value == "still")
                    event.motion = ShakeMotion::SpeedStill;
                else
                {
                    error = "motion off|speed|still: " + value;
                    return false;
                }
            }
            else if (key == "mode")
            {
                if (value == "snap")
                    event.mode = Mode::Snap;
                else if (value == "ramp")
                    event.mode = Mode::Ramp;
                else
                {
                    error = "mode snap|ramp: " + value;
                    return false;
                }
            }
            else
            {
                bool known = false;
                if (!shake::ParseCustomField(key, value, event.custom, known, error))
                    return false;
                if (!known)
                {
                    error = "unknown key: " + key;
                    return false;
                }
            }
        }
        return true;
    }
}
