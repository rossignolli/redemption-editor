#pragma once

// The standing material of a ped's feet: the replay sets the recorded
// material on the replay ped's footstep audio instead of the one its own
// ground probe finds. Without it a puppet plays the same step index from
// another container: same step, other surface.
//
// The foot audio refreshes each foot's main step container from the ground
// under it when the foot lands, so a puppet's own probe would decide the
// container of a replayed step. The mechanism: record, per foot-down, the slot
// the movement update played it with (sampled at the movement update entry,
// paired here with the foot-down); play it back by setting that slot on the
// puppet's foot before the movement update consumes the replayed step.
//
// Evidence: offline dump of build 1491.50.
// Research: docs/research/footsteps.md
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
    // surface, not on what its own probes find). The main step sound plays up
    // to five layer sounds weighted by [foot +0x3D0 + 4 * (5 * foot + i)];
    // the layer update writes them right before the movement update, so a
    // value written at the movement update entry is the one the steps read,
    // as the wetness. Research: docs/research/footsteps.md
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
    // (holder +0x19A4, read by the holder's LOD update: any value but 4 is the
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
    // audio. The step plays its wet layer only when the foot's wetness
    // (+0x380 + 4 * foot) is above 0, and reads the value the previous frame's
    // wetness update left, so the movement update entry is where the recorder
    // samples it and where the replay sets it on the puppet's foot.
    // Without it the puppets' steps never play the wet layer.
    // Research: docs/research/footsteps.md
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

    // A step whose wet layer plays (the main step's test: wetness > 0).
    inline bool WetStep(float wetness) { return wetness > 0.0f; }

    // The per-foot mix of a step (volume, high-pass, low-pass), recorded with
    // the step. The quadruped foot-down handler writes each hoof hit's mix
    // into the foot's slot (+0x390 volume, +0x3A0 delay, +0x3B0 high-pass,
    // +0x3C0 low-pass) before it queues the step, and the step sound reads
    // it. A puppet's step is queued by the replay straight into the footstep
    // entry, so its slots keep the reset values: every hoof hit at 0 dB,
    // unfiltered (a live gallop has about -2 to -6 dB and 50 to 300 Hz
    // high-pass per hit). Research: docs/research/footsteps.md
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
    //   - the movement sounds' base volume, foot audio +0x510 (the step player
    //     puts it in the step's params volume). A volume update, run by the
    //     step player right before the steps play, ramps it down while the
    //     ped's locomotion gait (row +0x88 component +0x630) is canter or
    //     gallop: about -2 dB on a live horse, 0 on the puppet, which
    //     has no locomotion gait. Recorded per step and folded into the slot
    //     volume on the puppet (slot + recorded base - puppet base), since
    //     the puppet's own update rewrites +0x510 before the step reads it.
    //   - the step's variable 0x01401A30 (set by the step sound from [row +0x88
    //     component + 0xF0 + 0x90 * foot + 0x70]): the highest the foot rose
    //     over its ground plane since its last contact (the per-foot update
    //     keeps max(h, [+0x70]) and zeroes it after the foot-down). About
    //     0.16 to 0.85 live; on a
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

    // The -2 dB term of the movement volume (build 1491.50): 0 dB
    // when the step's owner has the player bit (row +0xB8 component byte +0x40:
    // bit 1 set, bit 0 clear) or its row +0xA8 link answers the mount predicate
    // (the rider is the player); otherwise -2.0 dB (an f32 of the exe) when
    // the real player has no mount (the mount link of the decrypted player is
    // null), else 0.
    // In a replay the live player is hidden on foot: the player's puppet (no
    // player bit) and every horse of a clip where the player rode lose 2 dB the
    // recording did not have. Correction to add to the original's result: the
    // term back when the engine applied it now and the recorded state would not
    // have.
    struct MovementTermState
    {
        bool enabled = false;            // a clip playing
        bool puppetFoot = false;         // the foot audio is a replay puppet's
        bool ownerPlayerFlag = false;    // the engine's own player bit test
        bool riderIsPlayer = false;      // the mount predicate on the row +0xA8 link (with the mount detour's answer)
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
