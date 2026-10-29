#pragma once

// The read probes of the sound and particle ports, pure part (unit tested in
// tests/soundfx_probe_tests.cpp).
//
// Questions only a live session answers: the name and unit of 18 sound
// settings, how often each setting and play/stop/prepare is called, which
// thread runs each port, whether a sound ref is the same in two sessions, what
// five addresses are for, whether a live sound ever changes owner and whether
// speech can be made again from its ref. The probe counts and samples at the
// ports, changes nothing (soundfx_probe.cpp: every detour forwards its
// arguments and returns the original's result) and writes
// ReplayDebug\soundfx-probe-<date-time>.txt.
//
// This header holds what needs no engine: the port table, the lock-free
// counters the detours write (calls per mode, thread set, caller set, value
// histogram, the first distinct samples, key tables), the two small tables kept
// under the probe's lock (live sounds, queued requests) and the report text
// tools/soundfx_probe_report.py parses.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace replay::soundfx
{
    // What the script was doing when a port was called (set once per tick).
    enum class Mode : std::uint8_t
    {
        Idle,
        Record, // a recording or the buffer
        Play,
    };
    constexpr std::size_t kModes = 3;
    inline const char* ModeName(std::size_t mode)
    {
        return mode == 0 ? "idle" : mode == 1 ? "rec" : mode == 2 ? "play" : "?";
    }

    enum class Access : std::uint8_t
    {
        Detour,  // a detour of the probe (only with SoundFxProbe=1)
        Tap,     // read inside a detour the mod already has
        Skipped, // never installed; the report says why
    };

    // How the value argument of a port is read into the histograms.
    enum class Value : std::uint8_t
    {
        None,
        F32,
        F32x2,
        S16,
        U32,
        U8,
        Pointer,
        Vec3,
        HashF32,
    };

    enum Port : std::uint8_t
    {
        kS01, kS02, kS03, kS04, kS05, kS06, kParams,
        kS10, kS11, kS12, kS13, kS14, kS15,
        kS16, kS17, kS18, kS19, kS20, kS21, kS22, kS23, kS24, kS25, kS26, kS27, kS28, kS29, kS30,
        kS31, kS32, kS33, kS34, kS35, kS36, kS37, kS38, kS39, kS40,
        kConstruct,
        kP02, kP12, kP13, kP14, kP17, kP18,
        kPortCount,
    };

    struct PortInfo
    {
        Port port;
        const char* id;   // the port's id (S01.., P01..)
        const char* name;
        std::uint32_t rva; // build 1491.50
        Access access;
        Value value;
        const char* label0; // histogram 0 ("" = unused)
        const char* label1; // histogram 1
        const char* note;   // Skipped: the reason; Tap: the detour it rides
    };

    // Their arguments and callers were read in the dump: S31 and S32 are called by
    // the function that copies the init params into the requested settings
    // (0x258A9E8: S31 gets the param +0xAE, the pan, S32 the float +0xBC), S40
    // writes a float by hash into the table of the static sound manager object
    // 0x5338800, P17 and P18 are instance setters called from the game's effect
    // code. Each is a read-only detour like the others.

    inline constexpr PortInfo kPorts[] = {
        {kS01, "S01", "creator_a", 0x2516CAC, Access::Tap, Value::None, "param_volume", "param_pitch", "sound census detour"},
        {kS02, "S02", "creator_b", 0x2516900, Access::Tap, Value::None, "param_volume", "param_pitch", "sound census detour"},
        {kS03, "S03", "create_and_play_enqueue", 0x2589A40, Access::Tap, Value::None, "enqueue_to_create_ms", "", "audio observer detour"},
        {kS04, "S04", "deferred_request", 0x310E680, Access::Tap, Value::None, "", "", "audio observer detour"},
        {kS05, "S05", "persistent_by_ref", 0x2589124, Access::Tap, Value::None, "", "", "audio observer detour"},
        {kS06, "S06", "persistent_by_hash_a", 0x2589058, Access::Tap, Value::None, "", "", "audio observer detour"},
        {kParams, "PAR", "sound_params_ctor", 0x487E34, Access::Tap, Value::None, "", "", "audio observer detour"},
        {kS10, "S10", "prepare", 0x257CB78, Access::Detour, Value::None, "return_state", "priority", ""},
        {kS11, "S11", "prepare_and_play", 0x257CCBC, Access::Detour, Value::None, "arg3_int", "wave_slot_present", ""},
        {kS12, "S12", "play", 0x257C7AC, Access::Detour, Value::None, "", "", ""},
        {kS13, "S13", "stop", 0x2585BE4, Access::Detour, Value::None, "", "", ""},
        {kS14, "S14", "stop_and_forget", 0x2585C2C, Access::Tap, Value::None, "", "", "audio observer detour"},
        {kS15, "S15", "slot_release", 0x2529A00, Access::Detour, Value::None, "lifetime_s_after_forget", "lifetime_s_without_forget", ""},
        {kS16, "S16", "position", 0x2536E48, Access::Detour, Value::Vec3, "x", "z", ""},
        {kS17, "S17", "position_no_doppler", 0x2536ED4, Access::Detour, Value::Vec3, "x", "z", ""},
        {kS18, "S18", "volume", 0x2537168, Access::Detour, Value::F32, "value", "create_param_volume", ""},
        {kS19, "S19", "pitch", 0x2536E34, Access::Detour, Value::S16, "value", "create_param_pitch", ""},
        {kS20, "S20", "pan", 0x2536C9C, Access::Detour, Value::S16, "value", "", ""},
        {kS21, "S21", "low_pass_cutoff", 0x2536C70, Access::Detour, Value::U32, "value", "", ""},
        {kS22, "S22", "high_pass_cutoff", 0x2536A0C, Access::Detour, Value::U32, "value", "", ""},
        {kS23, "S23", "post_submix_attenuation", 0x2536E5C, Access::Detour, Value::F32, "value", "", ""},
        {kS24, "S24", "volume_curve_scale", 0x253717C, Access::Detour, Value::F32, "value", "", ""},
        {kS25, "S25", "doppler_factor", 0x253686C, Access::Detour, Value::F32, "value", "", ""},
        {kS26, "S26", "environmental_loudness_u8", 0x2536978, Access::Detour, Value::U8, "value", "", ""},
        {kS27, "S27", "environmental_loudness_float", 0x253698C, Access::Detour, Value::F32, "value", "", ""},
        {kS28, "S28", "source_effect_mix", 0x2536FB8, Access::Detour, Value::F32x2, "wet", "dry", ""},
        {kS29, "S29", "frequency_smoothing_rate", 0x25369AC, Access::Detour, Value::F32, "value", "", ""},
        {kS30, "S30", "environment_group", 0x2536948, Access::Detour, Value::Pointer, "is_null", "", ""},
        {kS31, "S31", "pan_from_init_param_AE", 0x2536CB0, Access::Detour, Value::S16, "value", "field_14C_low9_at_volume_call", ""},
        {kS32, "S32", "float_from_init_param_BC", 0x2536F68, Access::Detour, Value::F32, "value", "field_138_at_volume_call", ""},
        {kS33, "S33", "speaker_mask", 0x2537018, Access::Detour, Value::U8, "value", "", ""},
        {kS34, "S34", "listener_mask", 0x2536C5C, Access::Detour, Value::U8, "value", "", ""},
        {kS35, "S35", "distance_attenuation", 0x2536F7C, Access::Detour, Value::U8, "value", "", ""},
        {kS36, "S36", "rear_attenuation_off", 0x2536F90, Access::Detour, Value::U8, "value", "", ""},
        {kS37, "S37", "rear_filter_off", 0x2536FA4, Access::Detour, Value::U8, "value", "", ""},
        {kS38, "S38", "sound_variable", 0x25735EC, Access::Detour, Value::HashF32, "value", "", ""},
        {kS39, "S39", "entity_variable_block", 0x2573638, Access::Detour, Value::HashF32, "value", "", ""},
        {kS40, "S40", "global_variable", 0x25370B8, Access::Detour, Value::HashF32, "value", "create_if_missing", ""},
        {kConstruct, "CON", "sound_construct", 0x252AAE0, Access::Detour, Value::None, "top_level_E2", "child_E2", ""},
        {kP02, "P02", "fx_registered", 0x2C49B08, Access::Tap, Value::None, "detour_us", "engine_us", "fx observer detour"},
        {kP12, "P12", "fx_evolution", 0x29E33CC, Access::Detour, Value::HashF32, "value", "", ""},
        {kP13, "P13", "fx_colour", 0x2C4E008, Access::Detour, Value::Vec3, "r", "b", ""},
        {kP14, "P14", "fx_alpha", 0x2C4DE90, Access::Detour, Value::F32, "value", "", ""},
        {kP17, "P17", "fx_field_5CC", 0x29E372C, Access::Detour, Value::U32, "value", "changed", ""},
        {kP18, "P18", "fx_field_5C0_D94", 0x29E4160, Access::Detour, Value::U32, "value", "", ""},
    };
    static_assert(sizeof(kPorts) / sizeof(kPorts[0]) == kPortCount, "one row per port, in the enum's order");

    inline const PortInfo& Info(Port port) { return kPorts[port < kPortCount ? port : 0]; }

    // The 13 probes.
    struct ProbeInfo
    {
        int number;
        const char* name;
        bool implemented;
        const char* note;
    };
    inline constexpr ProbeInfo kProbes[] = {
        {1, "calls per port and thread", true, "every port, S40 included"},
        {2, "settings block to sound link", true, "every setting call: [block] is a sound and its settings are the block"},
        {3, "top-level and child sounds at construction", true, "parent, +0xD6 and +0xE2 at 0x252AAE0"},
        {4, "volume and pitch units", true, "creation params against the values of S18 and S19 of the same sound; no script sound is made"},
        {5, "sound refs for two sessions", true, "REF table; tools/soundfx_probe_report.py compares two reports"},
        {6, "speech callers of persistent by ref", true, "callers 0x2FA0DCC and 0x2FA10CF, the audio entity's vtable, wave slot in the params"},
        {7, "enqueue to creation time", true, "S03 against creator A, requests never created, params built per sound"},
        {8, "wet hoof variables", true, "S03 requests of the set 0x29934a: the variable list of the params"},
        {9, "replay A/B of one family", false, "not a read probe: it needs the phase 1 recorder and player (tools/bench audio diff)"},
        {10, "registered effect cost and evolution calls", true, "time inside the registered detour and the engine, P12 counted"},
        {11, "registered effect state", true, "range of alpha, zoom, colour and evolution per effect and mode at each registered call, not a per-frame trace"},
        {12, "global variable changes", true, "S40 (sound manager variable by hash): calls and value range per hash, changes counted"},
        {13, "sounds that end without stop-and-forget", true, "S15 mapped to its sound through the manager's slot table"},
    };

    // ------------------------------------------------------------ signatures

    // "8B 05 ?? ?? ?? ?? 48 C1": the bytes of a port in build 1491.50, `??`
    // for a byte that is not compared (a rip-relative displacement).
    struct Signature
    {
        std::vector<std::uint8_t> bytes;
        std::vector<std::uint8_t> mask; // 1: compared
        bool valid = false;
    };

    inline Signature ParseSignature(const char* text)
    {
        Signature out;
        const auto digit = [](char c) -> int {
            return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        };
        for (const char* at = text; at && *at;)
        {
            if (*at == ' ')
            {
                ++at;
                continue;
            }
            if (at[0] == '?' && at[1] == '?')
            {
                out.bytes.push_back(0);
                out.mask.push_back(0);
            }
            else
            {
                const int hi = digit(at[0]);
                const int lo = at[0] ? digit(at[1]) : -1;
                if (hi < 0 || lo < 0)
                    return Signature{};
                out.bytes.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
                out.mask.push_back(1);
            }
            at += 2;
            if (*at && *at != ' ')
                return Signature{};
        }
        out.valid = !out.bytes.empty() && out.mask.front() != 0;
        return out;
    }

    // `code` holds at least signature.bytes.size() bytes.
    inline bool MatchSignature(const std::uint8_t* code, const Signature& signature)
    {
        if (!signature.valid || !code)
            return false;
        for (std::size_t i = 0; i < signature.bytes.size(); ++i)
            if (signature.mask[i] && code[i] != signature.bytes[i])
                return false;
        return true;
    }

    // ------------------------------------------------------------ histogram

    // Decade bins around zero: the unit of a setting shows in which decades
    // its values fall (0..1 linear, -100..0 dB, hundreds of cents, Hz).
    constexpr int kBins = 17;
    inline int BinOf(double value)
    {
        if (value == 0.0)
            return 8;
        const double magnitude = value < 0.0 ? -value : value;
        int decade = magnitude < 0.01 ? 0 : magnitude < 0.1 ? 1 : magnitude < 1.0 ? 2 : magnitude < 10.0 ? 3
            : magnitude < 100.0 ? 4 : magnitude < 1000.0 ? 5 : magnitude < 10000.0 ? 6 : 7;
        return value < 0.0 ? 7 - decade : 9 + decade;
    }

    inline const char* BinLabel(int bin)
    {
        static const char* const kLabels[kBins] = {
            "<=-1e4", "-1e4..-1e3", "-1e3..-100", "-100..-10", "-10..-1", "-1..-0.1", "-0.1..-0.01", "-0.01..0",
            "0",
            "0..0.01", "0.01..0.1", "0.1..1", "1..10", "10..100", "100..1e3", "1e3..1e4", ">=1e4"};
        return bin >= 0 && bin < kBins ? kLabels[bin] : "?";
    }

    struct Histogram
    {
        std::uint64_t count = 0;
        std::uint64_t integral = 0;  // values with no fraction
        std::uint64_t nonFinite = 0; // NaN and infinities, not in count
        double min = 0.0;
        double max = 0.0;
        double sum = 0.0;
        std::uint64_t bins[kBins] = {};

        void Add(double value)
        {
            if (!std::isfinite(value))
            {
                ++nonFinite;
                return;
            }
            if (!count || value < min)
                min = value;
            if (!count || value > max)
                max = value;
            ++count;
            sum += value;
            if (value == std::floor(value))
                ++integral;
            ++bins[BinOf(value)];
        }

        double Mean() const { return count ? sum / static_cast<double>(count) : 0.0; }
    };

    // The same, written by any thread without a lock.
    struct AtomicHistogram
    {
        std::atomic<std::uint64_t> count{0};
        std::atomic<std::uint64_t> integral{0};
        std::atomic<std::uint64_t> nonFinite{0};
        std::atomic<double> min{std::numeric_limits<double>::infinity()};
        std::atomic<double> max{-std::numeric_limits<double>::infinity()};
        std::atomic<double> sum{0.0};
        std::atomic<std::uint64_t> bins[kBins] = {};

        void Add(double value) noexcept
        {
            if (!std::isfinite(value))
            {
                nonFinite.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            double seen = min.load(std::memory_order_relaxed);
            while (value < seen && !min.compare_exchange_weak(seen, value, std::memory_order_relaxed))
            {
            }
            seen = max.load(std::memory_order_relaxed);
            while (value > seen && !max.compare_exchange_weak(seen, value, std::memory_order_relaxed))
            {
            }
            seen = sum.load(std::memory_order_relaxed);
            while (!sum.compare_exchange_weak(seen, seen + value, std::memory_order_relaxed))
            {
            }
            if (value == std::floor(value))
                integral.fetch_add(1, std::memory_order_relaxed);
            bins[BinOf(value)].fetch_add(1, std::memory_order_relaxed);
            count.fetch_add(1, std::memory_order_relaxed);
        }

        Histogram Snapshot() const
        {
            Histogram out;
            out.count = count.load(std::memory_order_relaxed);
            out.integral = integral.load(std::memory_order_relaxed);
            out.nonFinite = nonFinite.load(std::memory_order_relaxed);
            out.sum = sum.load(std::memory_order_relaxed);
            if (out.count)
            {
                out.min = min.load(std::memory_order_relaxed);
                out.max = max.load(std::memory_order_relaxed);
                if (!std::isfinite(out.min) || !std::isfinite(out.max)) // a writer between its min and its count
                    out.min = out.max = 0.0;
            }
            for (int bin = 0; bin < kBins; ++bin)
                out.bins[bin] = bins[bin].load(std::memory_order_relaxed);
            return out;
        }
    };

    // ------------------------------------------------------------ small sets

    // The threads a port was called on (a port runs on one or two).
    struct ThreadSet
    {
        static constexpr int kSlots = 8;
        std::atomic<std::uint32_t> ids[kSlots] = {};
        std::atomic<std::uint64_t> overflow{0};

        void Note(std::uint32_t id) noexcept
        {
            for (std::atomic<std::uint32_t>& slot : ids)
            {
                std::uint32_t seen = slot.load(std::memory_order_relaxed);
                if (seen == id)
                    return;
                if (seen == 0 && (slot.compare_exchange_strong(seen, id, std::memory_order_relaxed) || seen == id))
                    return;
            }
            overflow.fetch_add(1, std::memory_order_relaxed);
        }

        std::vector<std::uint32_t> Snapshot() const
        {
            std::vector<std::uint32_t> out;
            for (const std::atomic<std::uint32_t>& slot : ids)
                if (const std::uint32_t id = slot.load(std::memory_order_relaxed))
                    out.push_back(id);
            std::sort(out.begin(), out.end());
            return out;
        }
    };

    struct CallerRow
    {
        std::uint32_t rva = 0;
        std::uint64_t calls = 0;
    };

    // Calls per return address (RVA; 0 = outside the game image).
    struct CallerSet
    {
        static constexpr int kSlots = 32;
        // The rva is kept + 1, so 0 is a free slot.
        std::atomic<std::uint32_t> keys[kSlots] = {};
        std::atomic<std::uint64_t> calls[kSlots] = {};
        std::atomic<std::uint64_t> overflow{0};

        void Note(std::uint32_t rva) noexcept
        {
            const std::uint32_t key = rva + 1;
            for (int slot = 0; slot < kSlots; ++slot)
            {
                std::uint32_t seen = keys[slot].load(std::memory_order_relaxed);
                if (seen == key ||
                    (seen == 0 && (keys[slot].compare_exchange_strong(seen, key, std::memory_order_relaxed) || seen == key)))
                {
                    calls[slot].fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
            overflow.fetch_add(1, std::memory_order_relaxed);
        }

        std::vector<CallerRow> Snapshot() const
        {
            std::vector<CallerRow> out;
            for (int slot = 0; slot < kSlots; ++slot)
            {
                const std::uint32_t key = keys[slot].load(std::memory_order_relaxed);
                if (key)
                    out.push_back({key - 1, calls[slot].load(std::memory_order_relaxed)});
            }
            std::sort(out.begin(), out.end(), [](const CallerRow& a, const CallerRow& b) {
                return a.calls != b.calls ? a.calls > b.calls : a.rva < b.rva;
            });
            return out;
        }
    };

    // One call as it came: the raw arguments, the sound the call was about
    // and what the probe knows of it.
    struct Sample
    {
        std::uint64_t args[4] = {};
        std::uint64_t sound = 0;
        std::uint64_t ref = 0;
        std::uint64_t owner = 0; // the sound's audio entity
        std::uint32_t caller = 0;
        std::uint32_t thread = 0;
        float value = 0.0f; // the value argument as the port reads it
    };

    // The first kMax distinct calls of a port (distinct: another caller or
    // another value argument). Full, a call costs one load.
    struct SampleStore
    {
        static constexpr std::uint32_t kMax = 32;
        Sample rows[kMax];
        std::atomic<std::uint32_t> used{0};
        std::atomic_flag busy = ATOMIC_FLAG_INIT;

        bool Full() const noexcept { return used.load(std::memory_order_relaxed) >= kMax; }

        bool Add(const Sample& sample) noexcept
        {
            if (Full())
                return false;
            if (busy.test_and_set(std::memory_order_acquire))
                return false; // another thread is adding: this call is not sampled
            const std::uint32_t count = used.load(std::memory_order_relaxed);
            bool added = false;
            if (count < kMax)
            {
                bool known = false;
                for (std::uint32_t i = 0; i < count && !known; ++i)
                    known = rows[i].caller == sample.caller && rows[i].args[1] == sample.args[1];
                if (!known)
                {
                    rows[count] = sample;
                    used.store(count + 1, std::memory_order_release);
                    added = true;
                }
            }
            busy.clear(std::memory_order_release);
            return added;
        }

        std::vector<Sample> Snapshot() const
        {
            const std::uint32_t count = std::min(used.load(std::memory_order_acquire), kMax);
            return std::vector<Sample>(rows, rows + count);
        }
    };

    // ------------------------------------------------------------ key table

    struct KeyRow
    {
        std::uint64_t key = 0;
        std::uint64_t count = 0;
        std::uint64_t flagged = 0; // calls that came with the flag (refused, speech)
        std::uint32_t mask = 0;    // or of the masks given
        bool hasValue = false;
        double min = 0.0;
        double max = 0.0;
    };

    // Count (and value range) per 64-bit key, written by any thread without
    // a lock: sound refs, variable hashes, vtables. Fixed size; a key that
    // finds no slot within kProbe steps is counted in overflow.
    class KeyTable
    {
    public:
        explicit KeyTable(std::size_t slotsPowerOfTwo)
            : m_mask(slotsPowerOfTwo - 1), m_slots(new Slot[slotsPowerOfTwo])
        {
        }
        ~KeyTable() { delete[] m_slots; }
        KeyTable(const KeyTable&) = delete;
        KeyTable& operator=(const KeyTable&) = delete;

        bool Add(std::uint64_t key, bool flag = false, std::uint32_t mask = 0) noexcept
        {
            Slot* slot = Find(key);
            if (!slot)
                return false;
            slot->count.fetch_add(1, std::memory_order_relaxed);
            if (flag)
                slot->flagged.fetch_add(1, std::memory_order_relaxed);
            if (mask)
                slot->mask.fetch_or(mask, std::memory_order_relaxed);
            return true;
        }

        // With `trackChanges` the row's flagged counts the calls whose value (as a
        // float) differs from the key's previous one.
        bool AddValue(std::uint64_t key, double value, bool trackChanges = false) noexcept
        {
            Slot* slot = Find(key);
            if (!slot)
                return false;
            slot->count.fetch_add(1, std::memory_order_relaxed);
            if (trackChanges)
            {
                const float narrow = static_cast<float>(value);
                std::uint32_t bits = 0;
                std::memcpy(&bits, &narrow, 4);
                if (bits == kNoLast)
                    bits = kNoLast - 1; // the one NaN pattern kept for "no value yet"
                const std::uint32_t before = slot->last.exchange(bits, std::memory_order_relaxed);
                if (before != kNoLast && before != bits)
                    slot->flagged.fetch_add(1, std::memory_order_relaxed);
            }
            if (std::isfinite(value))
            {
                double seen = slot->min.load(std::memory_order_relaxed);
                while (value < seen && !slot->min.compare_exchange_weak(seen, value, std::memory_order_relaxed))
                {
                }
                seen = slot->max.load(std::memory_order_relaxed);
                while (value > seen && !slot->max.compare_exchange_weak(seen, value, std::memory_order_relaxed))
                {
                }
            }
            return true;
        }

        std::uint64_t Overflow() const { return m_overflow.load(std::memory_order_relaxed); }

        // Largest count first, then the key.
        std::vector<KeyRow> Snapshot() const
        {
            std::vector<KeyRow> out;
            for (std::size_t i = 0; i <= m_mask; ++i)
            {
                const Slot& slot = m_slots[i];
                if (slot.state.load(std::memory_order_acquire) != kReady)
                    continue;
                KeyRow row;
                row.key = slot.key;
                row.count = slot.count.load(std::memory_order_relaxed);
                row.flagged = slot.flagged.load(std::memory_order_relaxed);
                row.mask = slot.mask.load(std::memory_order_relaxed);
                const double lo = slot.min.load(std::memory_order_relaxed);
                const double hi = slot.max.load(std::memory_order_relaxed);
                row.hasValue = std::isfinite(lo) && std::isfinite(hi);
                if (row.hasValue)
                {
                    row.min = lo;
                    row.max = hi;
                }
                out.push_back(row);
            }
            std::sort(out.begin(), out.end(), [](const KeyRow& a, const KeyRow& b) {
                return a.count != b.count ? a.count > b.count : a.key < b.key;
            });
            return out;
        }

    private:
        static constexpr std::uint32_t kFree = 0;
        static constexpr std::uint32_t kClaimed = 1;
        static constexpr std::uint32_t kReady = 2;
        static constexpr std::size_t kProbe = 48;
        static constexpr std::uint32_t kNoLast = 0x7FC0DEAD;

        struct Slot
        {
            std::atomic<std::uint32_t> state{kFree};
            std::uint64_t key = 0; // written once, between kClaimed and kReady
            std::atomic<std::uint64_t> count{0};
            std::atomic<std::uint64_t> flagged{0};
            std::atomic<std::uint32_t> mask{0};
            std::atomic<std::uint32_t> last{kNoLast}; // float bits of the last tracked value
            std::atomic<double> min{std::numeric_limits<double>::infinity()};
            std::atomic<double> max{-std::numeric_limits<double>::infinity()};
        };

        static std::uint64_t Mix(std::uint64_t key) noexcept
        {
            key ^= key >> 33;
            key *= 0xFF51AFD7ED558CCDull;
            key ^= key >> 33;
            key *= 0xC4CEB9FE1A85EC53ull;
            key ^= key >> 33;
            return key;
        }

        Slot* Find(std::uint64_t key) noexcept
        {
            std::size_t at = static_cast<std::size_t>(Mix(key)) & m_mask;
            const std::size_t steps = std::min(kProbe, m_mask + 1);
            for (std::size_t step = 0; step < steps; ++step, at = (at + 1) & m_mask)
            {
                Slot& slot = m_slots[at];
                std::uint32_t state = slot.state.load(std::memory_order_acquire);
                if (state == kFree)
                {
                    if (slot.state.compare_exchange_strong(state, kClaimed, std::memory_order_acq_rel))
                    {
                        slot.key = key;
                        slot.state.store(kReady, std::memory_order_release);
                        return &slot;
                    }
                }
                // Another thread is writing this slot's key: a few loads.
                while (state == kClaimed)
                    state = slot.state.load(std::memory_order_acquire);
                if (state == kReady && slot.key == key)
                    return &slot;
            }
            m_overflow.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }

        std::size_t m_mask;
        Slot* m_slots;
        std::atomic<std::uint64_t> m_overflow{0};
    };

    // ------------------------------------------------------------ port stats

    // Everything a detour writes about its port.
    struct PortStats
    {
        std::atomic<std::uint64_t> calls[kModes] = {};
        ThreadSet threads;
        CallerSet callers;
        AtomicHistogram values[2];
        SampleStore samples;
        // Probe 2 (setting ports): the block's sound link.
        std::atomic<std::uint64_t> linkOk{0};
        std::atomic<std::uint64_t> linkBad{0};
        std::atomic<std::uint64_t> linkUnread{0};
        // Script thread only (Tick): the largest count in one frame.
        std::uint64_t frameBase = 0;
        std::uint64_t maxPerFrame = 0;

        void Note(std::size_t mode, std::uint32_t thread, std::uint32_t callerRva) noexcept
        {
            calls[mode < kModes ? mode : 0].fetch_add(1, std::memory_order_relaxed);
            threads.Note(thread);
            callers.Note(callerRva);
        }

        std::uint64_t Total() const noexcept
        {
            std::uint64_t total = 0;
            for (const std::atomic<std::uint64_t>& mode : calls)
                total += mode.load(std::memory_order_relaxed);
            return total;
        }

        // Once per script tick.
        void EndFrame() noexcept
        {
            const std::uint64_t total = Total();
            maxPerFrame = std::max(maxPerFrame, total - frameBase);
            frameBase = total;
        }
    };

    // ------------------------------------------------------------ live sounds

    struct LiveSound
    {
        std::uint64_t sound = 0; // 0: free slot
        std::uint64_t ref = 0;
        std::uint64_t audio = 0;   // [sound + 0x50] when it was made
        std::uint64_t tracker = 0; // [sound + 0x48] when it was made
        std::uint64_t createdUs = 0;
        std::uint8_t core = 0;
        bool speech = false;  // made inside the speech's persistent-by-ref call
        bool forgot = false;  // S14 seen
        bool hasParams = false;
        float paramVolume = 0.0f;
        std::int16_t paramPitch = 0;
        std::uint32_t settings = 0; // setting calls on it
        std::uint32_t ownerChanges = 0;
    };

    // What a later look at the sound's owner fields says (probe "owner
    // change"): the entry keeps the newest values so one change counts once.
    struct OwnerCheck
    {
        bool audioChanged = false;
        bool trackerChanged = false;
        std::uint64_t oldAudio = 0;
        std::uint64_t oldTracker = 0;
    };

    inline OwnerCheck CheckOwner(LiveSound& live, std::uint64_t audioNow, std::uint64_t trackerNow)
    {
        OwnerCheck check;
        check.oldAudio = live.audio;
        check.oldTracker = live.tracker;
        check.audioChanged = audioNow != live.audio;
        check.trackerChanged = trackerNow != live.tracker;
        if (check.audioChanged || check.trackerChanged)
        {
            ++live.ownerChanges;
            live.audio = audioNow;
            live.tracker = trackerNow;
        }
        return check;
    }

    // Top-level sounds from their creation to their slot's release, by
    // address. Open addressing, no allocation after the constructor; the
    // caller holds the probe's lock.
    class LiveTable
    {
    public:
        explicit LiveTable(std::size_t slotsPowerOfTwo) : m_slots(slotsPowerOfTwo), m_mask(slotsPowerOfTwo - 1) {}

        std::size_t Size() const { return m_used; }
        std::size_t Capacity() const { return m_slots.size(); }

        LiveSound* Find(std::uint64_t sound)
        {
            if (!sound)
                return nullptr;
            for (std::size_t at = Home(sound), step = 0; step <= m_mask; ++step, at = (at + 1) & m_mask)
            {
                if (m_slots[at].sound == sound)
                    return &m_slots[at];
                if (!m_slots[at].sound)
                    return nullptr;
            }
            return nullptr;
        }

        // The entry for a new sound. `replaced`: the address was still live
        // (its release was not seen); null when the table is 3/4 full.
        LiveSound* Insert(std::uint64_t sound, bool& replaced)
        {
            replaced = false;
            if (!sound)
                return nullptr;
            if (LiveSound* known = Find(sound))
            {
                replaced = true;
                *known = LiveSound{};
                known->sound = sound;
                return known;
            }
            if ((m_used + 1) * 4 > m_slots.size() * 3)
                return nullptr;
            for (std::size_t at = Home(sound);; at = (at + 1) & m_mask)
            {
                if (!m_slots[at].sound)
                {
                    m_slots[at] = LiveSound{};
                    m_slots[at].sound = sound;
                    ++m_used;
                    return &m_slots[at];
                }
            }
        }

        bool Erase(std::uint64_t sound, LiveSound* removed = nullptr)
        {
            LiveSound* found = Find(sound);
            if (!found)
                return false;
            if (removed)
                *removed = *found;
            // Backward shift: no tombstones, a lookup stops at the first free slot.
            std::size_t hole = static_cast<std::size_t>(found - m_slots.data());
            m_slots[hole] = LiveSound{};
            for (std::size_t at = (hole + 1) & m_mask; m_slots[at].sound; at = (at + 1) & m_mask)
            {
                const std::size_t home = Home(m_slots[at].sound);
                const bool stays = hole <= at ? (home > hole && home <= at) : (home > hole || home <= at);
                if (stays)
                    continue;
                m_slots[hole] = m_slots[at];
                m_slots[at] = LiveSound{};
                hole = at;
            }
            --m_used;
            return true;
        }

    private:
        std::size_t Home(std::uint64_t sound) const
        {
            return static_cast<std::size_t>((sound >> 4) * 0x9E3779B97F4A7C15ull >> 32) & m_mask;
        }

        std::vector<LiveSound> m_slots;
        std::size_t m_mask;
        std::size_t m_used = 0;
    };

    // ------------------------------------------------------------ queue (probe 7)

    // Create-and-play requests between their enqueue (S03) and their
    // creation (creator A), matched by audio entity and ref, oldest first.
    // The caller holds the probe's lock.
    class PendingQueue
    {
    public:
        explicit PendingQueue(std::size_t capacity) : m_rows(capacity) {}

        // False: the queue was full and the oldest request was dropped.
        bool Push(std::uint64_t audio, std::uint64_t ref, std::uint64_t timeUs)
        {
            bool room = true;
            if (m_count == m_rows.size())
            {
                PopFront();
                ++m_overflow;
                room = false;
            }
            Row& row = m_rows[(m_head + m_count) % m_rows.size()];
            row.audio = audio;
            row.ref = ref;
            row.timeUs = timeUs;
            row.live = true;
            ++m_count;
            ++m_enqueued;
            return room;
        }

        // The oldest request of this entity and ref: its wait in microseconds.
        bool Match(std::uint64_t audio, std::uint64_t ref, std::uint64_t nowUs, std::uint64_t& waitedUs)
        {
            for (std::size_t i = 0; i < m_count; ++i)
            {
                Row& row = m_rows[(m_head + i) % m_rows.size()];
                if (!row.live || row.audio != audio || row.ref != ref)
                    continue;
                waitedUs = nowUs >= row.timeUs ? nowUs - row.timeUs : 0;
                row.live = false;
                ++m_matched;
                Trim();
                return true;
            }
            return false;
        }

        // Requests older than `olderThanUs` never reached the creator.
        std::uint64_t Expire(std::uint64_t nowUs, std::uint64_t olderThanUs)
        {
            std::uint64_t expired = 0;
            while (m_count)
            {
                Row& row = m_rows[m_head];
                if (row.live && nowUs - row.timeUs < olderThanUs)
                    break;
                if (row.live)
                    ++expired;
                PopFront();
            }
            m_expired += expired;
            return expired;
        }

        std::uint64_t Enqueued() const { return m_enqueued; }
        std::uint64_t Matched() const { return m_matched; }
        std::uint64_t Expired() const { return m_expired; }
        std::uint64_t Overflow() const { return m_overflow; }
        std::size_t Waiting() const
        {
            std::size_t waiting = 0;
            for (std::size_t i = 0; i < m_count; ++i)
                waiting += m_rows[(m_head + i) % m_rows.size()].live ? 1 : 0;
            return waiting;
        }

    private:
        struct Row
        {
            std::uint64_t audio = 0;
            std::uint64_t ref = 0;
            std::uint64_t timeUs = 0;
            bool live = false;
        };

        void PopFront()
        {
            m_rows[m_head].live = false;
            m_head = (m_head + 1) % m_rows.size();
            --m_count;
        }

        void Trim()
        {
            while (m_count && !m_rows[m_head].live)
                PopFront();
        }

        std::vector<Row> m_rows;
        std::size_t m_head = 0;
        std::size_t m_count = 0;
        std::uint64_t m_enqueued = 0;
        std::uint64_t m_matched = 0;
        std::uint64_t m_expired = 0;
        std::uint64_t m_overflow = 0;
    };

    // ------------------------------------------------------------ report

    struct PortReport
    {
        Port port = kS01;
        std::string status; // installed, tap, tap_not_seen, skipped, signature_mismatch, ...
        std::uint64_t calls[kModes] = {};
        std::uint64_t maxPerFrame = 0;
        std::vector<std::uint32_t> threads;
        std::uint64_t threadOverflow = 0;
        std::vector<CallerRow> callers;
        std::uint64_t callerOverflow = 0;
        Histogram values[2];
        std::vector<Sample> samples;
        std::uint64_t linkOk = 0;
        std::uint64_t linkBad = 0;
        std::uint64_t linkUnread = 0;

        std::uint64_t Total() const { return calls[0] + calls[1] + calls[2]; }
    };

    // The lock-free part of a port as plain numbers.
    inline PortReport Snapshot(Port port, const PortStats& stats, const std::string& status)
    {
        PortReport out;
        out.port = port;
        out.status = status;
        for (std::size_t mode = 0; mode < kModes; ++mode)
            out.calls[mode] = stats.calls[mode].load(std::memory_order_relaxed);
        out.maxPerFrame = stats.maxPerFrame;
        out.threads = stats.threads.Snapshot();
        out.threadOverflow = stats.threads.overflow.load(std::memory_order_relaxed);
        out.callers = stats.callers.Snapshot();
        out.callerOverflow = stats.callers.overflow.load(std::memory_order_relaxed);
        out.values[0] = stats.values[0].Snapshot();
        out.values[1] = stats.values[1].Snapshot();
        out.samples = stats.samples.Snapshot();
        out.linkOk = stats.linkOk.load(std::memory_order_relaxed);
        out.linkBad = stats.linkBad.load(std::memory_order_relaxed);
        out.linkUnread = stats.linkUnread.load(std::memory_order_relaxed);
        return out;
    }

    struct OwnerChange
    {
        std::uint64_t sound = 0;
        std::uint64_t ref = 0;
        std::uint64_t oldAudio = 0;
        std::uint64_t newAudio = 0;
        std::uint64_t oldTracker = 0;
        std::uint64_t newTracker = 0;
        std::uint32_t caller = 0;
        Port port = kS01;
        double ageS = 0.0;
    };

    // Counters kept under the probe's lock.
    struct LifeStats
    {
        std::uint64_t created = 0;
        std::uint64_t refused = 0;      // the creator returned no sound
        std::uint64_t reusedLive = 0;   // an address made again before its release was seen
        std::uint64_t tableFull = 0;
        std::uint64_t forgetCalls = 0;
        std::uint64_t forgetUnknown = 0; // S14 on a sound made before the probe (or a child)
        std::uint64_t releaseAfterForget = 0;
        std::uint64_t releaseWithoutForget = 0;
        std::uint64_t releaseUnknown = 0;      // S15 of a sound the probe did not see made
        std::uint64_t releaseCreateFailed = 0; // S15 from a creator's failure path
        std::uint64_t releaseUnmapped = 0;     // slot not readable
        std::uint64_t ownerChecks = 0;
        std::uint64_t audioChanges = 0;
        std::uint64_t trackerChanges = 0;
        std::uint64_t soundsWithChange = 0;
        std::uint64_t liveNow = 0;
    };

    struct QueueReport
    {
        std::uint64_t requests = 0; // S03 calls
        std::uint64_t refused = 0;  // S03 returned false
        std::uint64_t enqueued = 0;
        std::uint64_t matched = 0;
        std::uint64_t neverCreated = 0;
        std::uint64_t overflow = 0;
        std::uint64_t waiting = 0;
        std::uint64_t createdThroughDrain = 0; // creator A with the drain 0x258A348 in its caller path
        std::uint64_t createdOther = 0;
        std::uint64_t paramsBuilt = 0; // 0x487E34 calls
    };

    struct SpeechReport
    {
        std::uint64_t byRefFromSpeech = 0; // S05 called from 0x2FA0DCC or 0x2FA10CF
        std::uint64_t byRefFromOther = 0;
        std::uint64_t soundsCreated = 0;   // creator calls inside those
        std::uint64_t soundsRefused = 0;
        std::uint64_t withParams = 0;
        std::uint64_t withWaveSlot = 0;    // params +0xD0 not null
        std::vector<KeyRow> speechVtables; // audio entity vtable (RVA) of the speech calls
        std::vector<KeyRow> otherVtables;
        std::vector<KeyRow> refs;          // refs of the speech-made sounds
    };

    struct WetHoofSample
    {
        std::uint64_t ref = 0;
        std::uint32_t caller = 0;
        std::int32_t variableCount = 0;
        std::uint32_t words[6] = {}; // params +0x10C, as stored
    };

    struct WetHoofReport
    {
        std::uint64_t requests = 0;
        std::uint64_t withParams = 0;
        std::uint64_t withVariables = 0;
        std::uint64_t accepted = 0;
        std::vector<WetHoofSample> samples;
    };

    struct ConstructReport
    {
        std::uint64_t topLevel = 0;
        std::uint64_t child = 0;
        std::uint64_t failed = 0; // returned no sound
        std::uint64_t topWithSettings = 0;   // +0xD6 != 0xFFFF
        std::uint64_t childWithSettings = 0;
        std::uint64_t topParentIndexNone = 0;   // +0xE2 == 0xFFFF
        std::uint64_t childParentIndexNone = 0;
    };

    struct FxRow
    {
        std::uint32_t hash = 0;
        std::uint8_t mode = 0;
        std::uint8_t field = 0;
        std::uint64_t count = 0;
        double min = 0.0;
        double max = 0.0;
    };
    inline const char* FxFieldName(std::uint8_t field)
    {
        static const char* const kNames[] = {"alpha", "zoom", "evo0", "evo1", "evo2", "evo3", "red", "green", "blue"};
        return field < sizeof(kNames) / sizeof(kNames[0]) ? kNames[field] : "?";
    }
    constexpr std::uint8_t kFxFields = 9;
    inline std::uint64_t FxKey(std::uint32_t hash, std::uint8_t mode, std::uint8_t field)
    {
        return static_cast<std::uint64_t>(hash) | (static_cast<std::uint64_t>(mode) << 32) |
            (static_cast<std::uint64_t>(field) << 40);
    }
    inline FxRow FxRowOf(const KeyRow& row)
    {
        FxRow out;
        out.hash = static_cast<std::uint32_t>(row.key);
        out.mode = static_cast<std::uint8_t>(row.key >> 32);
        out.field = static_cast<std::uint8_t>(row.key >> 40);
        out.count = row.count;
        out.min = row.min;
        out.max = row.max;
        return out;
    }

    struct ThreadName
    {
        std::uint32_t id = 0;
        std::string name;
    };

    struct ReportData
    {
        std::string version;
        std::string started; // local time, "YYYY-MM-DD hh:mm:ss"
        std::string written;
        bool final = false; // written at the unload
        double elapsedS = 0.0;
        double modeS[kModes] = {};
        std::uint64_t frames = 0;
        std::string build; // "1491.50" when every signature matched, else what differs
        std::vector<PortReport> ports;
        std::vector<ThreadName> threads;
        std::vector<KeyRow> refs; // mask bit 0 creator A, bit 1 creator B; flagged = refused
        std::uint64_t refOverflow = 0;
        LifeStats life;
        std::vector<OwnerChange> ownerChanges;
        QueueReport queue;
        SpeechReport speech;
        WetHoofReport wetHoof;
        ConstructReport construct;
        std::vector<KeyRow> soundVariables;  // S38 hash -> calls, value range
        std::vector<KeyRow> entityVariables; // S39
        std::vector<KeyRow> evolutions;      // P12
        // S40 hash -> calls and value range (flagged: calls whose value differs from
        // the hash's previous one), and the distinct values of P17 and P18.
        std::vector<KeyRow> globalVariables;
        std::uint64_t globalVariableOverflow = 0;
        std::vector<KeyRow> fxField5CC;
        std::vector<KeyRow> fxFieldD94;
        std::vector<FxRow> fx;               // probe 11
        std::uint64_t fxOverflow = 0;
    };

    inline std::string Hex(std::uint64_t value)
    {
        char text[24];
        std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
        return text;
    }

    inline std::string Number(double value)
    {
        char text[40];
        std::snprintf(text, sizeof(text), "%.6g", value);
        return text;
    }

    inline double PerSecond(std::uint64_t calls, double seconds)
    {
        return seconds > 0.0 ? static_cast<double>(calls) / seconds : 0.0;
    }

    // "soundfx-probe-20261005-213012.txt"
    inline std::string ReportFileName(int year, int month, int day, int hour, int minute, int second)
    {
        char text[64];
        std::snprintf(text, sizeof(text), "soundfx-probe-%04d%02d%02d-%02d%02d%02d.txt", year, month, day, hour, minute,
            second);
        return text;
    }

    namespace detail
    {
        inline void HistogramLine(std::string& out, const char* id, int slot, const char* label, const Histogram& h)
        {
            if (!label || !*label || (!h.count && !h.nonFinite))
                return;
            out += "VALUE,id=";
            out += id;
            out += ",slot=" + std::to_string(slot) + ",label=" + label + ",count=" + std::to_string(h.count) +
                ",min=" + Number(h.min) + ",max=" + Number(h.max) + ",mean=" + Number(h.Mean()) +
                ",integral=" + std::to_string(h.integral) + ",non_finite=" + std::to_string(h.nonFinite) + ",bins=";
            bool first = true;
            for (int bin = 0; bin < kBins; ++bin)
            {
                if (!h.bins[bin])
                    continue;
                if (!first)
                    out += '|';
                first = false;
                out += BinLabel(bin);
                out += ':' + std::to_string(h.bins[bin]);
            }
            out += '\n';
        }

        inline void KeyLines(std::string& out, const char* tag, const std::vector<KeyRow>& rows)
        {
            for (const KeyRow& row : rows)
            {
                out += tag;
                out += ",key=" + Hex(row.key) + ",count=" + std::to_string(row.count);
                if (row.hasValue)
                    out += ",min=" + Number(row.min) + ",max=" + Number(row.max);
                out += '\n';
            }
        }
    }

    // The whole report: one record per line, `TAG,key=value,...`, the first
    // line the header. Text in English, like every text of the mod.
    inline std::string FormatReport(const ReportData& data)
    {
        std::string out;
        out.reserve(1 << 16);
        out += "SOUNDFX_PROBE_REPORT,format=1,version=" + data.version + ",started=" + data.started + ",written=" +
            data.written + ",final=" + (data.final ? "1" : "0") + ",elapsed_s=" + Number(data.elapsedS) +
            ",frames=" + std::to_string(data.frames);
        for (std::size_t mode = 0; mode < kModes; ++mode)
            out += std::string(",") + ModeName(mode) + "_s=" + Number(data.modeS[mode]);
        out += ",build=" + data.build + '\n';
        out += "# Read probes of the sound and particle ports. Nothing here changed the game:\n";
        out += "# every probe forwards its call. tools/soundfx_probe_report.py reads one report or compares two.\n";
        for (const ProbeInfo& probe : kProbes)
            out += "PROBE,n=" + std::to_string(probe.number) + ",status=" + (probe.implemented ? "implemented" : "skipped") +
                ",name=" + probe.name + ",note=" + probe.note + '\n';

        for (const PortReport& port : data.ports)
        {
            const PortInfo& info = Info(port.port);
            const std::uint64_t total = port.Total();
            out += std::string("PORT,id=") + info.id + ",rva=" + Hex(info.rva) + ",name=" + info.name +
                ",access=" + (info.access == Access::Detour ? "detour" : info.access == Access::Tap ? "tap" : "skipped") +
                ",status=" + port.status + ",calls=" + std::to_string(total);
            for (std::size_t mode = 0; mode < kModes; ++mode)
                out += std::string(",calls_") + ModeName(mode) + "=" + std::to_string(port.calls[mode]);
            out += ",per_s=" + Number(PerSecond(total, data.elapsedS)) +
                ",per_frame=" + Number(data.frames ? static_cast<double>(total) / static_cast<double>(data.frames) : 0.0) +
                ",max_frame=" + std::to_string(port.maxPerFrame) + ",threads=";
            for (std::size_t i = 0; i < port.threads.size(); ++i)
                out += (i ? ";" : "") + std::to_string(port.threads[i]);
            out += ",thread_overflow=" + std::to_string(port.threadOverflow) +
                ",callers=" + std::to_string(port.callers.size()) + ",caller_overflow=" + std::to_string(port.callerOverflow);
            if (port.linkOk || port.linkBad || port.linkUnread)
                out += ",link_ok=" + std::to_string(port.linkOk) + ",link_bad=" + std::to_string(port.linkBad) +
                    ",link_unread=" + std::to_string(port.linkUnread);
            if (info.access == Access::Skipped || (info.note && *info.note && info.access == Access::Tap))
                out += std::string(",note=") + info.note;
            out += '\n';
            detail::HistogramLine(out, info.id, 0, info.label0, port.values[0]);
            detail::HistogramLine(out, info.id, 1, info.label1, port.values[1]);
            for (const CallerRow& caller : port.callers)
                out += std::string("CALLER,id=") + info.id + ",rva=" + Hex(caller.rva) + ",calls=" +
                    std::to_string(caller.calls) + '\n';
            for (std::size_t i = 0; i < port.samples.size(); ++i)
            {
                const Sample& sample = port.samples[i];
                out += std::string("SAMPLE,id=") + info.id + ",n=" + std::to_string(i) + ",caller=" + Hex(sample.caller) +
                    ",thread=" + std::to_string(sample.thread) + ",a0=" + Hex(sample.args[0]) + ",a1=" + Hex(sample.args[1]) +
                    ",a2=" + Hex(sample.args[2]) + ",a3=" + Hex(sample.args[3]) + ",value=" + Number(sample.value) +
                    ",sound=" + Hex(sample.sound) + ",ref=" + Hex(sample.ref) + ",owner=" + Hex(sample.owner) + '\n';
            }
        }

        for (const ThreadName& thread : data.threads)
            out += "THREAD,id=" + std::to_string(thread.id) + ",name=" + thread.name + '\n';

        const LifeStats& life = data.life;
        out += "LIFE,created=" + std::to_string(life.created) + ",refused=" + std::to_string(life.refused) +
            ",live_now=" + std::to_string(life.liveNow) + ",reused_live=" + std::to_string(life.reusedLive) +
            ",table_full=" + std::to_string(life.tableFull) + ",forget_calls=" + std::to_string(life.forgetCalls) +
            ",forget_unknown=" + std::to_string(life.forgetUnknown) +
            ",release_after_forget=" + std::to_string(life.releaseAfterForget) +
            ",release_without_forget=" + std::to_string(life.releaseWithoutForget) +
            ",release_unknown=" + std::to_string(life.releaseUnknown) +
            ",release_create_failed=" + std::to_string(life.releaseCreateFailed) +
            ",release_unmapped=" + std::to_string(life.releaseUnmapped) + '\n';
        out += "OWNER,checks=" + std::to_string(life.ownerChecks) + ",audio_changes=" + std::to_string(life.audioChanges) +
            ",tracker_changes=" + std::to_string(life.trackerChanges) +
            ",sounds_with_change=" + std::to_string(life.soundsWithChange) + '\n';
        for (const OwnerChange& change : data.ownerChanges)
            out += "OWNER_CHANGE,sound=" + Hex(change.sound) + ",ref=" + Hex(change.ref) + ",port=" + Info(change.port).id +
                ",caller=" + Hex(change.caller) + ",age_s=" + Number(change.ageS) + ",audio_old=" + Hex(change.oldAudio) +
                ",audio_new=" + Hex(change.newAudio) + ",tracker_old=" + Hex(change.oldTracker) +
                ",tracker_new=" + Hex(change.newTracker) + '\n';

        const QueueReport& queue = data.queue;
        out += "QUEUE,requests=" + std::to_string(queue.requests) + ",refused=" + std::to_string(queue.refused) +
            ",enqueued=" + std::to_string(queue.enqueued) + ",matched=" + std::to_string(queue.matched) +
            ",never_created=" + std::to_string(queue.neverCreated) + ",waiting=" + std::to_string(queue.waiting) +
            ",overflow=" + std::to_string(queue.overflow) +
            ",created_through_drain=" + std::to_string(queue.createdThroughDrain) +
            ",created_other=" + std::to_string(queue.createdOther) + ",params_built=" + std::to_string(queue.paramsBuilt) +
            '\n';

        const SpeechReport& speech = data.speech;
        out += "SPEECH,by_ref_from_speech=" + std::to_string(speech.byRefFromSpeech) +
            ",by_ref_from_other=" + std::to_string(speech.byRefFromOther) +
            ",sounds_created=" + std::to_string(speech.soundsCreated) + ",sounds_refused=" +
            std::to_string(speech.soundsRefused) + ",with_params=" + std::to_string(speech.withParams) +
            ",with_wave_slot=" + std::to_string(speech.withWaveSlot) + ",distinct_refs=" +
            std::to_string(speech.refs.size()) + '\n';
        detail::KeyLines(out, "SPEECH_VTABLE,from=speech", speech.speechVtables);
        detail::KeyLines(out, "SPEECH_VTABLE,from=other", speech.otherVtables);
        detail::KeyLines(out, "SPEECH_REF", speech.refs);

        const WetHoofReport& hoof = data.wetHoof;
        out += "WET_HOOF,requests=" + std::to_string(hoof.requests) + ",accepted=" + std::to_string(hoof.accepted) +
            ",with_params=" + std::to_string(hoof.withParams) + ",with_variables=" + std::to_string(hoof.withVariables) +
            '\n';
        for (const WetHoofSample& sample : hoof.samples)
        {
            out += "WET_HOOF_SAMPLE,ref=" + Hex(sample.ref) + ",caller=" + Hex(sample.caller) + ",variables=" +
                std::to_string(sample.variableCount) + ",words=";
            for (int i = 0; i < 6; ++i)
                out += (i ? ";" : "") + Hex(sample.words[i]);
            out += '\n';
        }

        const ConstructReport& construct = data.construct;
        out += "CONSTRUCT,top_level=" + std::to_string(construct.topLevel) + ",child=" + std::to_string(construct.child) +
            ",failed=" + std::to_string(construct.failed) + ",top_with_settings=" +
            std::to_string(construct.topWithSettings) + ",child_with_settings=" +
            std::to_string(construct.childWithSettings) + ",top_parent_index_none=" +
            std::to_string(construct.topParentIndexNone) + ",child_parent_index_none=" +
            std::to_string(construct.childParentIndexNone) + '\n';

        detail::KeyLines(out, "SOUND_VARIABLE", data.soundVariables);
        detail::KeyLines(out, "ENTITY_VARIABLE", data.entityVariables);
        detail::KeyLines(out, "FX_EVOLUTION", data.evolutions);
        // Probe 12 and the two particle setters.
        out += "GLOBAL_VARIABLES,distinct=" + std::to_string(data.globalVariables.size()) + ",overflow=" +
            std::to_string(data.globalVariableOverflow) + '\n';
        for (const KeyRow& row : data.globalVariables)
        {
            out += "GLOBAL_VARIABLE,key=" + Hex(row.key) + ",count=" + std::to_string(row.count) + ",changes=" +
                std::to_string(row.flagged);
            if (row.hasValue)
                out += ",min=" + Number(row.min) + ",max=" + Number(row.max);
            out += '\n';
        }
        detail::KeyLines(out, "FX_FIELD_5CC", data.fxField5CC);
        detail::KeyLines(out, "FX_FIELD_D94", data.fxFieldD94);
        out += "FX_STATE,rows=" + std::to_string(data.fx.size()) + ",overflow=" + std::to_string(data.fxOverflow) + '\n';
        for (const FxRow& row : data.fx)
            out += "FX,hash=" + Hex(row.hash) + ",mode=" + ModeName(row.mode) + ",field=" + FxFieldName(row.field) +
                ",count=" + std::to_string(row.count) + ",min=" + Number(row.min) + ",max=" + Number(row.max) + '\n';

        out += "REFS,distinct=" + std::to_string(data.refs.size()) + ",overflow=" + std::to_string(data.refOverflow) + '\n';
        for (const KeyRow& row : data.refs)
            out += "REF,ref=" + Hex(row.key) + ",core=" + ((row.mask & 3u) == 3u ? "AB" : (row.mask & 2u) ? "B" : "A") +
                ",count=" + std::to_string(row.count) + ",refused=" + std::to_string(row.flagged) + '\n';
        out += "END\n";
        return out;
    }
}
