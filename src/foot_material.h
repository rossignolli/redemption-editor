#pragma once

// The standing material of a ped's feet: the replay sets the recorded
// material on the replay ped's footstep audio instead of the one its own
// ground probe finds. Without it a puppet plays the same step index from
// another container: same step, other surface.
//
// Engine facts (build 1491.50):
//   - the foot audio (ped audio holder +0x1560, vtable 0x38389B0) keeps one
//     64-byte record per foot at +0x240 (constructor 0x3178EB4 loops 4 times
//     from +0x240 with stride 0x40). Its first 16 bytes are the main step
//     container: +0x0 the container object (0x252B194 of the ref), +0x8 the
//     container name hash, +0xC the ref low dword (written by 0x3188614 at
//     0x3188741/0x3188762/0x318877B, from the material settings of the foot,
//     [foot +0x30 + 8 * foot], field +0x25C for a quadruped, +0x140..+0x148
//     by shoe type for a biped).
//   - the main step sound (0x318D1B8, called from 0x318D96E inside
//     0x318D52C) takes that slot ((foot index + 9) << 6 at 0x318D1D4) to
//     0x3181374, which builds the indirect ref container[index of the step
//     name] (0x252A888) and plays it (0x318E6E8, create-and-play return
//     0x318E828).
//   - the foot audio update 0x317F3E8 resolves the ground material of each
//     foot every frame (0x31A0A44 -> 0x3116524 by the physics material id of
//     the foot contact) and refreshes the slot (0x3188614) when that foot
//     landed: a puppet's own probe decides the container of a replayed step.
//   - the queued steps (entries of 16 bytes at +0x70, count at +0xB0, max 4,
//     foot index at entry +0xC) are played by 0x3193F20 inside the movement
//     update 0x3193D14 (detoured, MovementUpdateDetour); nothing in
//     0x3193D14 refreshes the slots before 0x3193F20 consumes the queue.
//
// The mechanism: record, per foot-down, the slot the movement update played
// it with (sampled at the movement update entry, paired here with the
// foot-down); play it back by setting that slot on the puppet's foot before
// the movement update consumes the replayed step.
//
// Pure (no game); unit tested in tests/foot_material_tests.cpp.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay::footmaterial
{
    constexpr std::uint8_t kFeet = 4;
    constexpr std::uintptr_t kSlotBase = 0x240;
    constexpr std::uintptr_t kSlotStride = 0x40;
    constexpr std::uintptr_t kSlotContainer = 0x0;
    constexpr std::uintptr_t kSlotHash = 0x8;
    constexpr std::uintptr_t kSlotRef = 0xC;
    constexpr std::uintptr_t kQueue = 0x70;
    constexpr std::uintptr_t kQueueStride = 0x10;
    constexpr std::uintptr_t kQueueFoot = 0xC;
    constexpr std::uintptr_t kQueueCount = 0xB0;
    constexpr std::int32_t kQueueMax = 4;

    inline std::uintptr_t SlotOffset(std::uint8_t foot)
    {
        return kSlotBase + static_cast<std::uintptr_t>(foot) * kSlotStride;
    }

    // A ref low dword the engine resolved (0 and ~0 are its "none").
    inline bool ValidRef(std::uint32_t ref) { return ref != 0 && ref != 0xFFFFFFFFu; }

    // The ground layers of each foot (the replay ped steps on the recorded
    // surface, not on what its own probes find).
    //
    // Engine facts (build 1491.50):
    //   - the step sound 0x318D1B8 (called for every step by 0x318D52C) plays,
    //     after the main step, up to five layer sounds through 0x318CEB4
    //     (create-and-play return 0x318D0A0): for layer i in 0..4, when
    //     [foot +0x3D0 + 4 * (5 * foot + i)] > 0 (0x318D297), the container
    //     [0x5C7E370 + 16 * (size class + 4 * i)] at that weight as volume
    //     (0x318D2FF); the size class is [ped audio +0x4608] +0x20, 3 when none;
    //   - the only writer is 0x319F070 (store at 0x319F539: a per-layer
    //     ground query 0x31FF1A0 with attack/decay toward the previous
    //     value, layer 4 by 0x31157B4 with the player bit of the owner),
    //     called by 0x319CBF0, which is the foot audio's vtable slot +0x80
    //     (vtable 0x38389B0: +0x10 reset 0x319580C, +0x18 foot update
    //     0x31925AC, +0x80 0x319CBF0);
    //   - the foot update 0x31925AC calls [vtable +0x80] at 0x319292B right
    //     before the movement update 0x3193D14 (0x3192934), which plays the
    //     queued steps (0x3193F20): a value written at the movement update
    //     entry is the one the steps read, as the wetness.
    constexpr std::uint8_t kLayers = 5;
    constexpr std::uintptr_t kLayerBase = 0x3D0;
    inline std::uintptr_t LayerOffset(std::uint8_t foot, std::uint8_t layer)
    {
        return kLayerBase + (static_cast<std::uintptr_t>(foot) * kLayers + layer) * 4;
    }
    // A weight as read from the game: finite, not negative, not absurd.
    inline bool PlausibleLayer(float w) { return w >= 0.0f && w <= 64.0f; }
    // Clip field (FOOT v6): thousandths in a u16 (65.535 at most).
    inline std::uint16_t EncodeLayer(float w)
    {
        if (!(w > 0.0f))
            return 0;
        const float scaled = w * 1000.0f + 0.5f;
        return scaled >= 65535.0f ? static_cast<std::uint16_t>(65535) : static_cast<std::uint16_t>(scaled);
    }
    inline float DecodeLayer(std::uint16_t stored) { return static_cast<float>(stored) / 1000.0f; }
    // Write when the slot holds another weight (more than the clip's step).
    inline bool NeedsLayerWrite(float current, float desired)
    {
        const float delta = current - desired;
        return !(delta < 0.0006f && delta > -0.0006f);
    }

    struct Material
    {
        std::uint32_t hash = 0;
        std::uint32_t ref = 0;
        // The foot's wetness the step played with (foot +0x380 + 4 * foot);
        // below 0 = unknown.
        float wetness = -1.0f;
        // The foot's ground layer weights the step played with (LayerOffset);
        // layersKnown false = unknown.
        bool layersKnown = false;
        float layers[kLayers] = {};
        bool Valid() const { return ValidRef(ref); }
    };

    // The audio LOD of a puppet with recorded steps. The override foot +0x444
    // (holder +0x19A4, read by 0x317BABC at 0x317BB0F: any value but 4 is the
    // holder's LOD) set to 0 keeps the foot (max LOD foot +0x28, 2) from ever
    // being turned off, but a LOD 0 holder runs components the live one had off.
    // The step records the holder's LOD (+0x45E8) at the foot-down; the playback
    // asks the LOD of the latest recorded step at the playhead (the first one
    // before it), never above the foot's max LOD: the recorded step proves the
    // foot was on then.
    constexpr std::uint8_t kLodUnknown = 0xFF;
    constexpr std::uint8_t kLodNoOverride = 4;
    struct LodPoint
    {
        std::uint32_t timeMs = 0;
        std::uint8_t lod = kLodUnknown;
    };
    // points: one owner's, sorted by time, known LODs only. False when empty
    // (the caller keeps the override, LOD 0).
    inline bool PuppetAudioLodAt(const std::vector<LodPoint>& points, std::uint32_t playheadMs, std::uint8_t footMaxLod,
        std::uint8_t& out)
    {
        if (points.empty())
            return false;
        std::size_t pick = 0;
        std::size_t low = 0, high = points.size();
        while (low < high) // the last point at or before the playhead
        {
            const std::size_t middle = low + (high - low) / 2;
            if (points[middle].timeMs <= playheadMs)
                low = middle + 1;
            else
                high = middle;
        }
        pick = low ? low - 1 : 0;
        std::uint8_t lod = points[pick].lod;
        if (lod >= kLodNoOverride)
            lod = kLodNoOverride - 1;
        const std::uint8_t ceiling = footMaxLod < kLodNoOverride ? footMaxLod : static_cast<std::uint8_t>(kLodNoOverride - 1);
        out = lod > ceiling ? ceiling : lod;
        return true;
    }

    // The wetness of each foot, recorded and set on the replay ped's footstep
    // audio.
    //
    // Engine facts (build 1491.50):
    //   - the foot audio keeps one float per foot at +0x380 (the foot audio
    //     reset 0x319580C clears it); the foot update 0x31925AC calls
    //     0x31A67E8 after the movement update 0x3193D14, and 0x31A67E8 sets
    //     each foot's value to the world ground wetness (the audio
    //     environment's +0x15F0, read by 0x31BE794) when that is higher,
    //     else lets it decay (1.0 while the ped audio's +0x774 says the foot
    //     is in water);
    //   - the step sound 0x318D1B8 (called for every step by 0x318D52C)
    //     plays the wet layer of the step through 0x318CEB4 (create and
    //     play return 0x318D0A0, the set 0x29934a for horses) only when
    //     foot +0x380 + 4 * foot > 0 (0x318D338) and the ped's +0xB0 bit 7
    //     is clear (0x318D346), with the wetness as its variable 0x02D01730;
    //   - so the step reads the value the previous frame's update left: the
    //     movement update entry is where the recorder samples it and where
    //     the replay sets it on the puppet's foot.
    // Without it the puppets' steps never play the wet layer.
    constexpr std::uintptr_t kWetnessBase = 0x380;
    inline std::uintptr_t WetnessOffset(std::uint8_t foot)
    {
        return kWetnessBase + static_cast<std::uintptr_t>(foot) * 4;
    }

    // Clip byte: 0 unknown (clips before FOOT v3), 1 dry .. 255 fully wet.
    inline std::uint8_t EncodeWetness(float wetness)
    {
        if (!(wetness >= 0.0f))
            return 0;
        if (wetness > 1.0f)
            wetness = 1.0f;
        return static_cast<std::uint8_t>(1 + static_cast<int>(wetness * 254.0f + 0.5f));
    }

    inline float DecodeWetness(std::uint8_t stored)
    {
        return stored ? static_cast<float>(stored - 1) / 254.0f : -1.0f;
    }

    // Write the recorded wetness when it is known and the foot holds another.
    inline bool NeedsWetnessWrite(float current, float desired)
    {
        if (!(desired >= 0.0f))
            return false;
        const float delta = current - desired;
        return !(delta < 0.002f && delta > -0.002f);
    }

    // A step whose wet layer plays (0x318D338: wetness > 0).
    inline bool WetStep(float wetness) { return wetness > 0.0f; }

    // The per-foot mix of a step (volume, high-pass, low-pass), recorded with
    // the step.
    //
    // Engine facts (build 1491.50):
    //   - the quadruped foot-down handler 0x317A57C (called by the foot-down
    //     0x8C2B64 when [+0x4B8] type is 4) walks the gait's hit pattern
    //     ([foot +0x420] metadata, up to 8 entries of 0x14 bytes: type,
    //     volume, delay, high-pass, low-pass) and, for each hoof hit, calls
    //     0x317AAD0 with the next foot ((foot + i) & 3);
    //   - 0x317AAD0 writes that foot's slot (+0x390 + 4 * foot volume f32,
    //     with the random +0x53C added; +0x3A0 delay; +0x3B0 high-pass u32;
    //     +0x3C0 low-pass u32) and then queues the step through the footstep
    //     entry 0x317A340, which the mod records (the delay goes as arg7);
    //   - the step sound 0x318D52C reads the slot of the queued foot: volume
    //     += [+0x390 + 4 * foot] (0x318D6EC), high-pass = max(params,
    //     [+0x3B0 + 4 * foot]) (0x318D776), low-pass = min(params,
    //     [+0x3C0 + 4 * foot]) (0x318D785);
    //   - the foot audio reset 0x319580C sets them to 0, 0, 0 and 24000.
    // A puppet's step is queued by the replay straight into 0x317A340, so
    // its slots keep the reset values: every hoof hit at 0 dB, unfiltered
    // (a live gallop has about -2 to -6 dB and 50 to 300 Hz high-pass per hit).
    constexpr std::uintptr_t kMixVolumeBase = 0x390;
    constexpr std::uintptr_t kMixHighPassBase = 0x3B0;
    constexpr std::uintptr_t kMixLowPassBase = 0x3C0;
    constexpr std::uint32_t kMixFilterMax = 48000; // Hz, above any value the engine writes (24000)

    inline std::uintptr_t MixVolumeOffset(std::uint8_t foot) { return kMixVolumeBase + static_cast<std::uintptr_t>(foot) * 4; }
    inline std::uintptr_t MixHighPassOffset(std::uint8_t foot) { return kMixHighPassBase + static_cast<std::uintptr_t>(foot) * 4; }
    inline std::uintptr_t MixLowPassOffset(std::uint8_t foot) { return kMixLowPassBase + static_cast<std::uintptr_t>(foot) * 4; }

    struct FootMix
    {
        bool known = false; // false: clip before FOOT v4 or unreadable slot
        float volume = 0.0f; // dB
        std::uint32_t highPass = 0; // Hz
        std::uint32_t lowPass = 24000; // Hz
    };

    // Two more inputs of the step's sound that the puppet does not have:
    //   - the movement sounds' base volume, foot audio +0x510 (0x3193FE7 puts
    //     it in the step's params volume). 0x31A048C (via 0x31A09C4, run by
    //     0x3193F20 right before the steps play) ramps it down while the
    //     ped's locomotion gait (row +0x88 component +0x630) is canter or
    //     gallop: about -2 dB on a live horse, 0 on the puppet, which
    //     has no locomotion gait. Recorded per step and folded into the slot
    //     volume on the puppet (slot + recorded base - puppet base), since
    //     the puppet's own update rewrites +0x510 before the step reads it.
    //   - the step's variable 0x01401A30 (0x318D93D: [row +0x88 component +
    //     0xF0 + 0x90 * foot + 0x70]): the highest the foot rose over its
    //     ground plane since its last contact (the per-foot update
    //     0x8EED4A..0x8EF2A1 keeps max(h, [+0x70]) at 0x8EEEBA and zeroes it
    //     after the foot-down at 0x8EF292). About 0.16 to 0.85 live; on a
    //     puppet the ground plane goes stale (the contact never fires) and it
    //     grows to 1 to 4.7. Written on the puppet's component before the step
    //     plays (FootImpactReplay, default 1).
    constexpr std::uintptr_t kBaseVolumeOffset = 0x510;   // foot audio
    constexpr std::uintptr_t kImpactBase = 0x160;         // row +0x88 component, + 0x90 * foot
    constexpr std::uintptr_t kImpactStride = 0x90;
    inline std::uintptr_t ImpactOffset(std::uint8_t foot) { return kImpactBase + static_cast<std::uintptr_t>(foot) * kImpactStride; }

    struct FootDrive
    {
        bool baseKnown = false;
        float base = 0.0f;   // dB, foot audio +0x510 at the foot-down
        bool impactKnown = false;
        float impact = 0.0f; // the variable 0x01401A30's source
    };
    inline bool PlausibleBase(float v) { return v >= -60.0f && v <= 24.0f; }
    inline bool PlausibleImpact(float v) { return v >= -100.0f && v <= 100.0f; }
    // The slot volume that makes the step's params volume the recorded one.
    inline float SlotVolumeWithBase(float recordedSlot, const FootDrive& drive, float puppetBase, bool useBase)
    {
        if (!useBase || !drive.baseKnown || !PlausibleBase(puppetBase))
            return recordedSlot;
        return recordedSlot + (drive.base - puppetBase);
    }

    // A slot as read from the game: plausible values only (a fault or a
    // foreign layout gives garbage, which must not be replayed).
    inline bool PlausibleMix(float volume, std::uint32_t highPass, std::uint32_t lowPass)
    {
        return volume >= -100.0f && volume <= 24.0f && highPass <= kMixFilterMax && lowPass <= kMixFilterMax;
    }

    // Write the recorded mix when it is known and the slot holds another.
    inline bool NeedsMixWrite(const FootMix& current, const FootMix& desired)
    {
        if (!desired.known)
            return false;
        const float delta = current.volume - desired.volume;
        return !(delta < 0.001f && delta > -0.001f) || current.highPass != desired.highPass ||
            current.lowPass != desired.lowPass;
    }

    // Clip fields (FOOT v4): the filters as u16 (the engine's are <= 24000).
    inline std::uint16_t EncodeMixFilter(std::uint32_t hz) { return static_cast<std::uint16_t>(hz > 0xFFFF ? 0xFFFF : hz); }

    // The -2 dB term of the movement volume 0x31815C4 (build 1491.50): 0 dB
    // when the step's owner has the player bit (row +0xB8 component byte +0x40:
    // bit 1 set, bit 0 clear) or its row +0xA8 link answers 0x8B69B0 (the rider
    // is the player); otherwise -2.0 dB (float at 0x3E67E5C) when the real
    // player has no mount (0xCC857C of the decrypted player is null), else 0.
    // In a replay the live player is hidden on foot: the player's puppet (no
    // player bit) and every horse of a clip where the player rode lose 2 dB the
    // recording did not have. Correction to add to the original's result: the
    // term back when the engine applied it now and the recorded state would not
    // have.
    struct MovementTermState
    {
        bool enabled = false;            // ini MovementVolumeReplay and a clip playing
        bool puppetFoot = false;         // the foot audio is a replay puppet's
        bool ownerPlayerFlag = false;    // the engine's own player bit test
        bool riderIsPlayer = false;      // 0x8B69B0 on the row +0xA8 link (with the mount detour's answer)
        bool livePlayerMounted = false;  // the hidden live player rides (the engine's test)
        bool ownerIsPlayerPuppet = false; // the owner is the recorded player's puppet
        bool recordedPlayerMounted = false; // the recorded player rides at this playhead
    };
    inline float MovementTermCorrection(const MovementTermState& s, float term)
    {
        if (!s.enabled || !s.puppetFoot)
            return 0.0f;
        const bool appliedNow = !s.ownerPlayerFlag && !s.riderIsPlayer && !s.livePlayerMounted;
        if (!appliedNow)
            return 0.0f;
        return (s.ownerIsPlayerPuppet || s.recordedPlayerMounted) ? -term : 0.0f;
    }

    // One queued step seen at the movement update entry.
    struct Sample
    {
        std::uint64_t timeUs = 0;
        std::uint64_t footAudio = 0;
        std::uint8_t foot = 0;
        Material material;
    };

    // The slot a replayed step needs: write when the desired material is
    // valid and the slot holds another container (or another ref/hash).
    inline bool NeedsWrite(std::uint64_t currentContainer, const Material& current, std::uint64_t desiredContainer,
        const Material& desired)
    {
        if (!desired.Valid() || !desiredContainer)
            return false;
        return currentContainer != desiredContainer || current.ref != desired.ref || current.hash != desired.hash;
    }

    // Pairs each recorded foot-down (the footstep entry, which only queues)
    // with the movement update sample that played it: same foot audio, same
    // foot, the first sample at or after the foot-down, in order. A sample
    // can arrive one script tick after its foot-down (the order of the
    // script thread and the audio update in a frame is not known), so a
    // foot-down waits until it is matched or too old.
    class Pairer
    {
    public:
        struct Pending
        {
            std::uint64_t timeUs = 0;
            std::uint64_t footAudio = 0;
            std::uint8_t foot = 0;
            std::uint64_t token = 0; // the caller's (event index)
            std::uint64_t check = 0; // the caller's (verifies the event)
        };
        struct Match
        {
            std::uint64_t token = 0;
            std::uint64_t check = 0;
            Material material;
        };

        static constexpr std::size_t kMaxPending = 512;
        static constexpr std::size_t kMaxSamples = 1024;

        void Clear()
        {
            m_pending.clear();
            m_samples.clear();
        }

        // Returns false when the pending list is full (the oldest goes).
        bool AddPending(const Pending& pending)
        {
            bool room = true;
            if (m_pending.size() >= kMaxPending)
            {
                m_pending.erase(m_pending.begin());
                ++m_expired;
                room = false;
            }
            m_pending.push_back(pending);
            return room;
        }

        void AddSample(const Sample& sample)
        {
            if (m_samples.size() >= kMaxSamples)
                m_samples.erase(m_samples.begin());
            m_samples.push_back(sample);
        }

        // Matches what it can, oldest foot-down first. A sample never
        // matches a foot-down after it; unmatched samples (steps of peds not
        // recorded, or before the recording) go with Expire.
        void Pair(std::vector<Match>& out)
        {
            out.clear();
            std::vector<bool> used(m_samples.size(), false);
            std::vector<Pending> left;
            left.reserve(m_pending.size());
            for (const Pending& pending : m_pending)
            {
                std::size_t best = m_samples.size();
                for (std::size_t n = 0; n < m_samples.size(); ++n)
                {
                    const Sample& sample = m_samples[n];
                    if (used[n] || sample.footAudio != pending.footAudio || sample.foot != pending.foot ||
                        sample.timeUs < pending.timeUs)
                        continue;
                    if (best == m_samples.size() || sample.timeUs < m_samples[best].timeUs)
                        best = n;
                }
                if (best == m_samples.size())
                {
                    left.push_back(pending);
                    continue;
                }
                used[best] = true;
                out.push_back({pending.token, pending.check, m_samples[best].material});
            }
            m_pending.swap(left);
            std::vector<Sample> kept;
            kept.reserve(m_samples.size());
            for (std::size_t n = 0; n < m_samples.size(); ++n)
                if (!used[n])
                    kept.push_back(m_samples[n]);
            m_samples.swap(kept);
        }

        // Drops the foot-downs older than maxAgeUs (no sample came: the step
        // was dropped by the engine or the movement update did not run) and
        // the samples as old. Returns the foot-downs dropped.
        std::size_t Expire(std::uint64_t nowUs, std::uint64_t maxAgeUs)
        {
            std::size_t dropped = 0;
            std::vector<Pending> kept;
            kept.reserve(m_pending.size());
            for (const Pending& pending : m_pending)
            {
                if (nowUs > pending.timeUs && nowUs - pending.timeUs > maxAgeUs)
                    ++dropped;
                else
                    kept.push_back(pending);
            }
            m_pending.swap(kept);
            m_expired += dropped;
            std::vector<Sample> samples;
            samples.reserve(m_samples.size());
            for (const Sample& sample : m_samples)
                if (!(nowUs > sample.timeUs && nowUs - sample.timeUs > maxAgeUs))
                    samples.push_back(sample);
            m_samples.swap(samples);
            return dropped;
        }

        std::size_t PendingCount() const { return m_pending.size(); }
        std::size_t SampleCount() const { return m_samples.size(); }
        std::uint64_t Expired() const { return m_expired; }

    private:
        std::vector<Pending> m_pending;
        std::vector<Sample> m_samples;
        std::uint64_t m_expired = 0;
    };
}
