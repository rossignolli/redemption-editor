#pragma once

// Shake events model, as Simple Camera (crxhvrd, MIT) has it: each event sets
// ONE parameter of the shake to a float value at its time, at once (Snap) or
// as a ramp (Ramp: the value goes from the event before it of the same kind,
// at that event's time, to its own over the time between them). The list
// reads like the mod's: "Event 0  t=0.00 Shake Amp=1.50 snap".
//
// Kinds (the mod's stable ids, then ours): 0 On/Off, 1 Preset (the mod's five:
// Off, Subtle, Handheld, Vehicle, Earthquake; a preset other than Off turns
// the shake on), 2 Amp, 3 Freq (Hz), 4 Speed->Amp, 5 Speed->Freq, 6 Rot
// Weight, 7 Pos Weight, 8 Stop When Still, 9 Randomize (a new pattern seed and
// a phase reset; always Snap), 10 Profile (our realistic profiles: 0 none,
// else the generator is the profile, Amp its gain (1 = intensity 5) and Freq
// its clock (4 Hz = 100 %)).
//
// The shake at t is the fold of every event up to t plus the ramps in flight,
// from the mod's defaults (off, amp 0, 4 Hz, speed->amp 1, speed->freq 0.5,
// weights 1, not still, pattern 0). Pure and deterministic: the phase is the
// frequency integrated from the moment the shake turned on (or the last
// Randomize) to t, so a scrub shows what the play shows.
//
// Rule with the markers' shake: while the track's shake is on (On/Off 1, or
// a Preset or Profile other than Off in force) it decides; off, the markers'
// own shake plays.
//
// CFXE v1 and v2 (one event held a whole shake: type, intensity, speed, with
// movement, Custom numbers) are converted on load to events of this model
// (ConvertLegacy).
//
// The editor has no events track: the shake lives in the markers
// (camera_marker_shake.h). This model is the migration's input and its
// reference: an old clip's CFXE section, a project entry's tail, the
// director's "fx" lines and the bench's fx_add are read into
// Track::LegacyEffects() and folded into the markers (mshake::Migrate),
// which must give the same shake at every frame as StateAt/Evaluate here.

#include "camera_shake_custom.h"
#include "editor_protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace replay::camera::fx
{
    using editor::ShakeMotion;
    using editor::ShakeType;

    enum class Kind : std::uint8_t
    {
        OnOff = 0,
        Preset = 1,
        Amp = 2,
        Freq = 3,
        SpeedAmp = 4,
        SpeedFreq = 5,
        RotWeight = 6,
        PosWeight = 7,
        StopWhenStill = 8,
        Randomize = 9,
        Profile = 10
    };
    constexpr std::uint8_t kKindCount = 11;

    enum class Mode : std::uint8_t
    {
        Snap = 0, // the value from the event's time on
        Ramp = 1  // the value ramps from the event before of the same kind
    };
    constexpr std::uint8_t kModeCount = 2;

    constexpr std::size_t kMaxEvents = 1024;

    struct KindInfo
    {
        const char* label; // what the list and the menu show ("Shake Amp")
        const char* key;   // what the director and the bench write ("amp")
        float minimum;
        float maximum;
        float step;        // one arrow press (PageUp/PageDown: ten)
        bool integer;      // the value is a whole number (a switch, an index, a seed)
    };
    // Label, key, range, arrow step, whole number. The steps: 0.01 on the
    // small ranges (amp, couplings, weights), 0.1 on the frequency. Inline:
    // the editor (ReplayEditor.addon) reads them without the core.
    inline constexpr KindInfo kKinds[kKindCount] = {
        {"Shake On/Off", "onoff", 0.0f, 1.0f, 1.0f, true},
        {"Shake Preset", "preset", 0.0f, 4.0f, 1.0f, true},
        {"Shake Amp", "amp", 0.0f, 5.0f, 0.01f, false},
        {"Shake Freq", "freq", 0.0f, 30.0f, 0.1f, false},
        {"Speed->Amp", "spdamp", 0.0f, 2.0f, 0.01f, false},
        {"Speed->Freq", "spdfreq", 0.0f, 2.0f, 0.01f, false},
        {"Rot Weight", "rot", 0.0f, 2.0f, 0.01f, false},
        {"Pos Weight", "pos", 0.0f, 2.0f, 0.01f, false},
        {"Stop When Still", "still", 0.0f, 1.0f, 1.0f, true},
        {"Randomize", "randomize", 0.0f, 65535.0f, 1.0f, true},
        {"Profile", "profile", 0.0f, 10.0f, 1.0f, true},
    };
    inline const KindInfo& Info(Kind kind)
    {
        const std::uint8_t i = static_cast<std::uint8_t>(kind);
        return kKinds[i < kKindCount ? i : 0];
    }
    bool ParseKind(const std::string& key, Kind& kind);
    // A value held in the kind's range (rounded for the whole ones).
    inline float ClampValue(Kind kind, float value)
    {
        const KindInfo& info = Info(kind);
        if (!std::isfinite(value))
            value = info.minimum;
        value = std::clamp(value, info.minimum, info.maximum);
        return info.integer ? std::floor(value + 0.5f) : value;
    }

    // Simple Camera's presets: amp, Hz, speed->amp, speed->freq.
    constexpr std::uint8_t kPresetCount = 5;
    struct PresetValues
    {
        float amp;
        float hz;
        float speedAmp;
        float speedHz;
    };
    inline constexpr PresetValues kPresets[kPresetCount] = {
        {0.00f, 4.0f, 0.0f, 0.0f},  // Off
        {0.15f, 2.5f, 0.5f, 0.3f},  // Subtle
        {0.40f, 4.0f, 1.0f, 0.5f},  // Handheld
        {0.70f, 6.0f, 1.5f, 1.0f},  // Vehicle
        {1.50f, 10.0f, 0.0f, 0.0f}, // Earthquake
    };
    inline const char* PresetName(std::uint8_t preset)
    {
        static const char* const kNames[kPresetCount] = {"Off", "Subtle", "Handheld", "Vehicle", "Earthquake"};
        return preset < kPresetCount ? kNames[preset] : "Off";
    }

    struct ShakeEvent
    {
        std::uint32_t id = 0;
        std::uint64_t timeMs = 0;
        Kind kind = Kind::Amp;
        float value = 0.0f;
        Mode mode = Mode::Snap;

        bool operator==(const ShakeEvent& o) const
        {
            return id == o.id && timeMs == o.timeMs && kind == o.kind && value == o.value && mode == o.mode;
        }
    };

    // Kind, mode and value in range (Randomize only as Snap).
    bool Valid(const ShakeEvent& event);

    class EffectTrack final
    {
    public:
        // Adds an event (its id is assigned; the value held in range; a
        // Randomize is Snap). 0 when the track is full or a field is not
        // usable. Several events may share a time (the mod's list has three
        // at t=0.00): they apply in the order they were added.
        std::uint32_t Add(const ShakeEvent& event);
        bool Remove(std::uint32_t id);
        bool Move(std::uint32_t id, std::uint64_t timeMs);
        // New kind, value and mode, keeping the id and the time.
        bool Update(std::uint32_t id, const ShakeEvent& values);
        const ShakeEvent* Find(std::uint32_t id) const;
        const std::vector<ShakeEvent>& Events() const noexcept { return events_; }
        bool Empty() const noexcept { return events_.empty(); }
        void Clear();
        std::uint32_t NextId() const noexcept { return nextId_; }
        // Loading: the events as they were (by time, ids unique and under
        // nextId); false and nothing changed when they are not.
        bool Assign(std::vector<ShakeEvent> events, std::uint32_t nextId);

        bool operator==(const EffectTrack& o) const { return nextId_ == o.nextId_ && events_ == o.events_; }
        bool operator!=(const EffectTrack& o) const { return !(*this == o); }

    private:
        void Sort();
        std::vector<ShakeEvent> events_;
        std::uint32_t nextId_ = 1;
    };

    // The shake's parameters at an instant (the fold).
    struct State
    {
        bool enabled = false;
        std::uint8_t profile = 0; // ShakeType 0..10 (0: Simple Camera's own noise)
        float amp = 0.0f;
        float hz = 4.0f;
        float speedAmp = 1.0f;
        float speedHz = 0.5f;
        float rotWeight = 1.0f;
        float posWeight = 1.0f;
        bool still = false;
        std::uint16_t seed = 0;
        double onSinceMs = -1.0;     // when the shake last turned on (-1: never)
        double randomizedMs = -1.0;  // the last Randomize (the phase starts again there)
        bool ramping = false;        // a ramp is in flight
        std::uint32_t lastEventId = 0; // the last event at or before t
    };
    State StateAt(const EffectTrack& track, double timeMs);
    // The value the kind has in a state (On/Off 0/1, Preset 0, Randomize the
    // seed, Profile its index): what a new event of that kind starts from.
    float ValueOf(const State& state, Kind kind);

    // The shake the track asks for at t.
    struct FxShake
    {
        bool active = false;           // false: the markers' shake plays
        ShakeType profile = ShakeType::None; // None: Simple Camera's noise with `custom`
        editor::ShakeCustom custom = editor::DefaultShakeCustom(); // amp, hz, couplings, weights, still, seed (speedRef 30)
        double cycles = 0.0;           // the frequency integrated since runStartMs (the speed coupling is the caller's)
        std::uint64_t runStartMs = 0;  // where the phase starts (turned on, or the last Randomize)
        double sinceOnMs = 0.0;        // since the shake turned on (the fade in)
        std::uint32_t eventId = 0;     // the last event at or before t
        bool ramping = false;
    };
    FxShake Evaluate(const EffectTrack& track, std::uint64_t timeMs);
    // The profile's clock in ms from the integrated cycles: Freq 4 Hz plays
    // the profile at 100 %.
    constexpr double kProfileClockHz = 4.0;
    inline double ProfileClockMs(double cycles) { return cycles * 1000.0 / kProfileClockHz; }

    // Names of our profiles as the director and the bench write them: none
    // handheld drunk ground_vibration (ground) air_turbulence (air) explosion
    // walking running horseback steadicam wagon custom.
    const char* ShakeName(ShakeType type);
    bool ParseShakeName(const std::string& name, ShakeType& type);

    // "key=value" tokens of an event: t (ms), kind (onoff preset amp freq
    // spdamp spdfreq rot pos still randomize profile), value (a number; for
    // preset and profile also a name), mode (snap ramp). Keys left out keep
    // the event's values; an unknown key or a value out of range is an error.
    bool ParseEventFields(const std::vector<std::string>& tokens, ShakeEvent& event, std::string& error);
    // The event as a director line's fields: "t=43900 kind=amp value=1.500 mode=snap".
    std::string FormatEventFields(const ShakeEvent& event);
    // What the list shows: "t=1.50 Shake Amp=1.50 snap".
    inline std::string FormatListLine(std::uint64_t timeMs, Kind kind, float value, Mode mode)
    {
        char text[96]{};
        std::snprintf(text, sizeof(text), "t=%.2f %s=%.2f %s", static_cast<double>(timeMs) / 1000.0, Info(kind).label, value,
            mode == Mode::Ramp ? "ramp" : "snap");
        return text;
    }

    // ---- CFXE v1/v2 and the old director lines.
    struct LegacyEvent
    {
        std::uint64_t timeMs = 0;
        ShakeType shake = ShakeType::Handheld;
        std::uint8_t intensity = 5;
        std::uint8_t speedIndex = editor::kMarkerSpeedDefaultIndex;
        ShakeMotion motion = ShakeMotion::Off;
        Mode mode = Mode::Snap;
        editor::ShakeCustom custom = editor::DefaultShakeCustom();
    };
    // The old events as events of this model, in time order: None turns the
    // shake off; a profile sets Profile, Amp (its gain), Freq (its speed: 4 Hz
    // x the percent), Speed->Amp (1 with "with movement") and Stop When Still;
    // Custom sets Profile 0 and its numbers (Amp = amp x the intensity's
    // gain, Freq = Hz x the speed, the couplings, the weights, Stop When Still,
    // Randomize = its seed). Only what changes from the event before is
    // written; an old Ramp ramps the numbers that change. Ids from 1.
    std::vector<ShakeEvent> ConvertLegacy(const std::vector<LegacyEvent>& old);
    // An old director line's fields (shake= shake_i= shake_speed= motion=
    // mode= shake_amp= ...): true when the tokens are the old form.
    bool IsLegacyLine(const std::vector<std::string>& tokens);
    bool ParseLegacyFields(const std::vector<std::string>& tokens, LegacyEvent& event, std::string& error);
}
