#pragma once

// Always-on replay buffer: a rolling ring on top of the recording. The
// recording keeps the capture clock; every block the buffer drops, the heavy
// per-frame tracks lose what lies before the ring's head: continuous particle
// frames (up to 475 KB/s), the rendered camera, the weapon transform tracks.
// The pose chunks go through pose::capture::DropBefore. One-shot events and
// state tracks are small and are cut when a clip is saved (replay::window).
// Pure data, no natives.

#include "replay_recording.h"
#include "object_check.h"
#include "world_state.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace replay::buffer
{
    // The visibility rows (Recording::pedVisibility, one per recorded ped and
    // horse) are capped; the ring gives the rows of entities it no longer
    // keeps back (TrimBefore), so a long session keeps recording new ones.
    constexpr std::size_t kMaxVisibilityRows = 4096;

    // The row of a replay id for the per-frame scan, without walking every
    // row for every ped. Rebuilt when the rows changed under it (a trim, a
    // new recording); a hit is checked against the row's id.
    class VisibilityIndex
    {
    public:
        PedVisibility* Find(std::vector<PedVisibility>& rows, entity::ReplayId id)
        {
            if (Stale(rows))
                Rebuild(rows);
            auto found = m_at.find(id.Packed());
            if (found != m_at.end() && (found->second >= rows.size() || rows[found->second].id != id))
            {
                Rebuild(rows);
                found = m_at.find(id.Packed());
            }
            return found == m_at.end() ? nullptr : &rows[found->second];
        }
        // False at the cap (the row is not added).
        bool Add(std::vector<PedVisibility>& rows, const PedVisibility& row)
        {
            if (rows.size() >= kMaxVisibilityRows)
                return false;
            if (Stale(rows))
                Rebuild(rows);
            rows.push_back(row);
            m_at.emplace(row.id.Packed(), rows.size() - 1);
            Note(rows);
            return true;
        }

    private:
        // Other rows than the ones indexed: another buffer, size, first or
        // last row (a trim, a new recording).
        bool Stale(const std::vector<PedVisibility>& rows) const
        {
            return rows.data() != m_data || rows.size() != m_size ||
                (!rows.empty() && (rows.front().id.Packed() != m_first || rows.back().id.Packed() != m_last));
        }
        void Note(const std::vector<PedVisibility>& rows)
        {
            m_data = rows.data();
            m_size = rows.size();
            m_first = rows.empty() ? 0 : rows.front().id.Packed();
            m_last = rows.empty() ? 0 : rows.back().id.Packed();
        }
        void Rebuild(const std::vector<PedVisibility>& rows)
        {
            m_at.clear();
            for (std::size_t n = 0; n < rows.size(); ++n)
                m_at.emplace(rows[n].id.Packed(), n); // the first row of an id, as the scan found it
            Note(rows);
        }
        std::unordered_map<std::uint32_t, std::size_t> m_at;
        const PedVisibility* m_data = nullptr;
        std::size_t m_size = 0;
        std::uint32_t m_first = 0;
        std::uint32_t m_last = 0;
    };

    // The rows whose entity the lifecycle no longer names (gone before the
    // ring's head). Nothing without lifecycle blocks. Returns the rows dropped.
    inline std::size_t PruneVisibilityRows(Recording& clip)
    {
        if (clip.lifecycle.BlockCount() == 0 || clip.pedVisibility.empty())
            return 0;
        std::vector<std::uint32_t> kept;
        clip.lifecycle.KeptIds(kept);
        const std::size_t before = clip.pedVisibility.size();
        clip.pedVisibility.erase(std::remove_if(clip.pedVisibility.begin(), clip.pedVisibility.end(),
            [&](const PedVisibility& row) { return !std::binary_search(kept.begin(), kept.end(), row.id.Packed()); }),
            clip.pedVisibility.end());
        return before - clip.pedVisibility.size();
    }
    struct TrimStats
    {
        std::size_t fxFrames = 0;
        std::size_t fxTracksEmptied = 0;
        std::size_t cameraSamples = 0;
        std::size_t weaponSamples = 0;
        std::size_t lifecycleBlocks = 0;
        std::size_t animAudioEvents = 0;
        std::size_t worldEvents = 0;
        std::size_t appearanceChanges = 0;
        std::size_t ropeRecords = 0;       // rope records freed
        std::size_t ropeSamples = 0;       // and vertex samples dropped
        std::size_t visibilityRows = 0;    // rows of entities the lifecycle no longer names
    };

    // The one boundary every track keeps from. A
    // saved window starts at the lifecycle block that holds its from
    // (window::Cut), and the earliest from is the ring's head: after
    // LifecycleTrack::DropBefore(headMs) that block is the oldest one kept.
    // Everything a cut at that base needs (the sample or state in force
    // there, the spans and effects that reach it) stays. Before the drop
    // the oldest block is older still (more is kept, never less). Without
    // blocks, one block before the head.
    inline std::uint32_t ExportableBaseMs(const Recording& clip, std::uint32_t headMs)
    {
        if (clip.lifecycle.BlockCount() == 0)
            return headMs > entity::LifecycleTrack::kBlockMs ? headMs - entity::LifecycleTrack::kBlockMs : 0;
        return (std::min)(clip.lifecycle.FirstBlockMs(), headMs);
    }

    // Drops the per-frame samples and states before the exportable base
    // (ExportableBaseMs), keeping the one in force there. Track and event
    // indices the recorder holds stay valid: no track is removed, an ended
    // one only loses its frames. The registered fx tracks it emptied are
    // removed after it by the recorder, which remaps its live keys
    // (events::DropTrimmedRegisteredFx, entity::DropEmptiedRegisteredFx).
    inline TrimStats TrimBefore(Recording& clip, std::uint32_t headMs)
    {
        TrimStats stats;
        stats.lifecycleBlocks = clip.lifecycle.DropBefore(headMs);
        // Every track below keeps from the same base: kept from headMs
        // instead, a cut at the block start would show their future state at
        // its frame 0.
        const std::uint32_t keepFrom = ExportableBaseMs(clip, headMs);
        // window::Cut takes the last attach change at or before its base
        // onto an entity it keeps (the ones alive there): the trim keeps
        // from that one.
        std::vector<entity::CreatePacket> aliveAtBase;
        bool aliveRead = false;
        const auto aliveAt = [&](entity::ReplayId id) {
            if (!aliveRead)
            {
                clip.lifecycle.AliveAt(keepFrom, aliveAtBase);
                aliveRead = true;
            }
            return std::any_of(aliveAtBase.begin(), aliveAtBase.end(),
                [&](const entity::CreatePacket& packet) { return packet.id == id; });
        };
        for (entity::RegisteredFxTrack& track : clip.registeredFx)
        {
            if (track.frames.empty())
                continue;
            if (track.endMs < keepFrom)
            {
                stats.fxFrames += track.frames.size();
                ++stats.fxTracksEmptied;
                track.frames.clear();
                track.frames.shrink_to_fit();
                track.attachTimeline.clear(); // goes with its frames
                continue;
            }
            std::size_t keep = 0;
            while (keep + 1 < track.frames.size() && track.frames[keep + 1].timeMs <= keepFrom)
                ++keep;
            if (keep)
            {
                track.frames.erase(track.frames.begin(), track.frames.begin() + static_cast<std::ptrdiff_t>(keep));
                stats.fxFrames += keep;
            }
            // The attach changes likewise, keeping the one in force at the base.
            std::size_t attachKeep = 0;
            for (std::size_t n = 0; n < track.attachTimeline.size() && track.attachTimeline[n].timeMs <= keepFrom; ++n)
                if (n > attachKeep && aliveAt(track.attachTimeline[n].id))
                    attachKeep = n;
            if (attachKeep)
                track.attachTimeline.erase(track.attachTimeline.begin(),
                    track.attachTimeline.begin() + static_cast<std::ptrdiff_t>(attachKeep));
        }
        // The rope records like the fx tracks: a rope gone before the base
        // frees its record, a live one keeps from the base.
        {
            const ropes::RopeTrimStats ropeStats = clip.ropes.DropBefore(keepFrom);
            stats.ropeRecords = ropeStats.records;
            stats.ropeSamples = ropeStats.samples;
        }
        envtrack::DropBefore(clip.environment.track, keepFrom);
        skytrack::DropBefore(clip.environment.sky, keepFrom);
        tctrack::DropBefore(clip.environment.timecycle, keepFrom);
        moontrack::DropBefore(clip.environment.moon, keepFrom);
        {
            std::size_t keep = 0;
            while (keep + 1 < clip.gameCamera.size() && clip.gameCamera[keep + 1].timeMs <= keepFrom)
                ++keep;
            if (keep)
            {
                clip.gameCamera.erase(clip.gameCamera.begin(), clip.gameCamera.begin() + static_cast<std::ptrdiff_t>(keep));
                stats.cameraSamples = keep;
            }
        }
        for (WeaponTrack& weapon : clip.weaponTracks)
            stats.weaponSamples += weapon.track.DropBefore(keepFrom);
        clip.trains.DropBefore(keepFrom); // the head cars' speed (the cars' tracks are weapon tracks)
        // The sound census windows and the captured sounds that
        // ended before the base (a live loop stays: sound_capture.cpp finds
        // it by owner, ref and start, not by index).
        clip.soundCensus.DropBefore(keepFrom);
        clip.generalSounds.erase(std::remove_if(clip.generalSounds.begin(), clip.generalSounds.end(),
            [keepFrom](const census::GeneralSound& sound) { return sound.ended && sound.endMs < keepFrom; }),
            clip.generalSounds.end());
        // The generic sound packets of sounds released
        // before the base (a live one keeps its create and its state).
        soundgeneric::TrimBefore(clip.soundGeneric, keepFrom);
        // The answer in force of each owner and chain stays.
        entityquery::TrimBefore(clip.entityQueries, keepFrom);
        clip.mixProbe.DropBefore(keepFrom);
        clip.gameListener.DropBefore(keepFrom);
        clip.windAudio.DropBefore(keepFrom);
        clip.pedSpeed.DropBefore(keepFrom); // the value in force stays
        clip.shaderVars.DropBefore(keepFrom); // the values in force stay
        clip.footEvents.DropBefore(keepFrom); // one-shots, as the anim audio
        clip.footMovement.DropBefore(keepFrom); // one value per step
        clip.mixScenes.DropBefore(keepFrom); // active scenes and the groups in force stay
        clip.categoryState.DropBefore(keepFrom); // each category's value in force stays
        for (PedVisibility& row : clip.pedVisibility)
            objcheck::TrimVisibility(row.visible, row.flipsMs, keepFrom);
        stats.visibilityRows = PruneVisibilityRows(clip);
        // Animation audio events are one-shots, but many (every
        // recorded ped and horse, ambient birds included), so the ring drops
        // them too, before the base.
        stats.animAudioEvents = clip.animAudio.DropBefore(keepFrom);
        clip.abilities.erase(std::remove_if(clip.abilities.begin(), clip.abilities.end(),
            [keepFrom](const AbilitySpan& span) { return !span.open && span.endMs < keepFrom; }), clip.abilities.end());
        // Spans that ended before the head go, like the ability
        // spans; the cuts before it too (nothing is interpolated there any
        // more). Charred tracks keep the state in force at headMs.
        clip.spans.erase(std::remove_if(clip.spans.begin(), clip.spans.end(),
            [keepFrom](const gate::Span& span) { return !span.open && span.endMs < keepFrom; }), clip.spans.end());
        clip.cuts.erase(std::remove_if(clip.cuts.begin(), clip.cuts.end(),
            [keepFrom](std::uint32_t cut) { return cut < keepFrom; }), clip.cuts.end());
        for (ScorchTrack& track : clip.scorched)
        {
            std::size_t keep = 0;
            while (keep + 1 < track.states.size() && track.states[keep + 1].first <= keepFrom)
                ++keep;
            if (keep)
                track.states.erase(track.states.begin(), track.states.begin() + static_cast<std::ptrdiff_t>(keep));
        }
        // The state tracks below keep the state in force at the base (a saved
        // window starts up to one block before the head and its frame 0 must
        // show the right glass, wheels or burn).
        const std::uint32_t stateKeepFrom = keepFrom;
        // Broken window masks, like the charred tracks.
        for (WindowTrack& track : clip.vehicleWindows)
        {
            std::size_t keep = 0;
            while (keep + 1 < track.states.size() && track.states[keep + 1].first <= stateKeepFrom)
                ++keep;
            if (keep)
                track.states.erase(track.states.begin(), track.states.begin() + static_cast<std::ptrdiff_t>(keep));
        }
        // Lost wheel and door masks, like the windows.
        for (VehiclePartTrack& track : clip.vehicleParts)
        {
            std::size_t keep = 0;
            while (keep + 1 < track.states.size() && track.states[keep + 1].first <= stateKeepFrom)
                ++keep;
            if (keep)
                track.states.erase(track.states.begin(), track.states.begin() + static_cast<std::ptrdiff_t>(keep));
        }
        // Door ratios, the last of each door kept.
        for (vehdoors::DoorTrack& track : clip.vehicleDoors)
            vehdoors::DropBefore(track, stateKeepFrom);
        // Vehicle burnt states, like the charred tracks.
        for (ScorchTrack& track : clip.vehicleBurnt)
        {
            std::size_t keep = 0;
            while (keep + 1 < track.states.size() && track.states[keep + 1].first <= stateKeepFrom)
                ++keep;
            if (keep)
                track.states.erase(track.states.begin(), track.states.begin() + static_cast<std::ptrdiff_t>(keep));
        }
        // Fire records, like the charred tracks.
        for (FireCharTrack& track : clip.fireChars)
        {
            std::size_t keep = 0;
            while (keep + 1 < track.states.size() && track.states[keep + 1].first <= stateKeepFrom)
                ++keep;
            if (keep)
                track.states.erase(track.states.begin(), track.states.begin() + static_cast<std::ptrdiff_t>(keep));
        }
        // Burn levels, like the charred tracks, the scenery's too (its
        // entries are not trimmed here, so the indices hold).
        for (std::vector<BurnLevelTrack>* tracks : {&clip.burnLevels, &clip.sceneryBurnLevels})
            for (BurnLevelTrack& track : *tracks)
            {
                std::size_t keep = 0;
                while (keep + 1 < track.states.size() && track.states[keep + 1].first <= stateKeepFrom)
                    ++keep;
                if (keep)
                    track.states.erase(track.states.begin(), track.states.begin() + static_cast<std::ptrdiff_t>(keep));
            }
        // Object looks likewise (the scenery entries are not trimmed here,
        // so the indices hold).
        for (std::vector<ObjectLookTrack>* tracks : {&clip.objectLooks, &clip.sceneryObjectLooks})
            for (ObjectLookTrack& track : *tracks)
            {
                std::size_t keep = 0;
                while (keep + 1 < track.states.size() && track.states[keep + 1].first <= stateKeepFrom)
                    ++keep;
                if (keep)
                    track.states.erase(track.states.begin(), track.states.begin() + static_cast<std::ptrdiff_t>(keep));
            }
        // Metaped states likewise.
        for (metaped::Track& track : clip.metapeds)
        {
            std::size_t keep = 0;
            while (keep + 1 < track.states.size() && track.states[keep + 1].timeMs <= keepFrom)
                ++keep;
            if (keep)
                track.states.erase(track.states.begin(), track.states.begin() + static_cast<std::ptrdiff_t>(keep));
        }
        // Appearance changes keep the one in force at the base:
        // window::Cut folds that one to the window's 0.
        {
            for (AppearanceTrack& track : clip.appearances)
            {
                std::size_t keep = 0;
                while (keep + 1 < track.changes.size() && track.changes[keep + 1].timeMs <= keepFrom)
                    ++keep;
                if (keep)
                {
                    track.changes.erase(track.changes.begin(), track.changes.begin() + static_cast<std::ptrdiff_t>(keep));
                    stats.appearanceChanges += keep;
                }
            }
        }
        for (DoorTrack& door : clip.doors)
        {
            std::size_t keep = 0;
            while (keep + 1 < door.samples.size() && door.samples[keep + 1].timeMs <= keepFrom)
                ++keep;
            if (keep)
                door.samples.erase(door.samples.begin(), door.samples.begin() + static_cast<std::ptrdiff_t>(keep));
        }
        // The map state keeps the state in force at the base
        // (state events), like the charred tracks.
        stats.worldEvents = world::TrimWorldBefore(clip.world, keepFrom);
        return stats;
    }

    // What the recording holds in memory, roughly: the big per-frame tracks
    // exactly, the event tracks by count (the pose bytes come from
    // pose::capture::TrackBytes). The payloads nested
    // in the tracks count too (the shallow water frames, up to 64 KB each,
    // the fire records' volumes, the sound params, the state tracks). By
    // size: the ring's budget loop trims until the count falls, and an
    // erase keeps the capacity. Runs once per buffer block (2 s), not per
    // frame.
    template <typename T>
    inline std::size_t VectorBytes(const std::vector<T>& values)
    {
        return values.size() * sizeof(T);
    }
    template <typename Track>
    inline std::size_t StateTracksBytes(const std::vector<Track>& tracks)
    {
        std::size_t bytes = VectorBytes(tracks);
        for (const Track& track : tracks)
            bytes += VectorBytes(track.states);
        return bytes;
    }

    // The door tracks keep samples, not states.
    inline std::size_t VehicleDoorsBytes(const std::vector<vehdoors::DoorTrack>& tracks)
    {
        std::size_t bytes = VectorBytes(tracks);
        for (const vehdoors::DoorTrack& track : tracks)
            bytes += VectorBytes(track.samples);
        return bytes;
    }

    inline std::size_t ApproxBytes(const Recording& clip)
    {
        std::size_t bytes = clip.lifecycle.ApproxBytes();
        for (const entity::RegisteredFxTrack& track : clip.registeredFx)
            bytes += sizeof(track) + track.frames.capacity() * sizeof(entity::RegisteredFxFrame) + VectorBytes(track.attachTimeline);
        bytes += clip.gameCamera.capacity() * sizeof(camera::GameSample);
        bytes += ropes::RopeRecordsBytes(clip.ropes.Records()); // no record cap: the budget bounds them
        for (const WeaponTrack& weapon : clip.weaponTracks)
            bytes += sizeof(weapon) + weapon.track.Size() * sizeof(entity::TransformSample);
        bytes += clip.sounds.Events().size() * (sizeof(entity::SoundEvent) + 32);
        bytes += clip.decals.Events().size() * sizeof(entity::DecalEvent);
        bytes += clip.fx.Events().size() * sizeof(entity::FxEvent);
        bytes += clip.fireItems.Events().size() * sizeof(entity::FireItemSample);
        bytes += clip.footsteps.Events().size() * sizeof(entity::FootstepEvent);
        bytes += clip.animAudio.Events().size() * sizeof(entity::AnimAudioEvent);
        bytes += clip.created.size() * sizeof(entity::CreatePacket);
        // The nested payloads.
        bytes += VectorBytes(clip.collisionSounds);
        for (const CollisionSound& sound : clip.collisionSounds)
            bytes += sound.params.size() + VectorBytes(sound.intensity);
        bytes += VectorBytes(clip.entitySounds);
        for (const EntitySound& sound : clip.entitySounds)
            bytes += sound.params.size();
        bytes += VectorBytes(clip.fireChars);
        for (const FireCharTrack& track : clip.fireChars)
        {
            bytes += VectorBytes(track.states);
            for (const auto& state : track.states)
                bytes += VectorBytes(state.second.volumes);
        }
        bytes += StateTracksBytes(clip.scorched) + StateTracksBytes(clip.vehicleWindows) +
            StateTracksBytes(clip.vehicleParts) + VehicleDoorsBytes(clip.vehicleDoors) + StateTracksBytes(clip.vehicleBurnt) + StateTracksBytes(clip.burnLevels) +
            StateTracksBytes(clip.sceneryBurnLevels) +
            StateTracksBytes(clip.objectLooks) + StateTracksBytes(clip.sceneryObjectLooks) +
            StateTracksBytes(clip.strapTracks) + StateTracksBytes(clip.weaponConditions);
        bytes += VectorBytes(clip.trains.cars) + VectorBytes(clip.trains.speeds);
        bytes += clip.soundCensus.Bytes() + VectorBytes(clip.generalSounds);
        bytes += clip.soundGeneric.ApproxBytes();
        bytes += clip.entityQueries.ApproxBytes();
        bytes += clip.mixProbe.ApproxBytes();
        bytes += clip.gameListener.ApproxBytes();
        bytes += clip.windAudio.ApproxBytes();
        bytes += clip.pedSpeed.ApproxBytes();
        bytes += clip.shaderVars.ApproxBytes();
        bytes += clip.footEvents.ApproxBytes();
        bytes += clip.footMovement.ApproxBytes();
        bytes += clip.mixScenes.ApproxBytes();
        bytes += clip.categoryState.ApproxBytes();
        bytes += VectorBytes(clip.environment.timecycle);
        bytes += VectorBytes(clip.environment.moon);
        for (const census::GeneralSound& sound : clip.generalSounds)
            bytes += sound.params.size();
        for (const trains::SpeedTrack& track : clip.trains.speeds)
            bytes += VectorBytes(track.samples);
        bytes += VectorBytes(clip.appearances);
        bytes += VectorBytes(clip.metapeds);
        for (const metaped::Track& track : clip.metapeds)
        {
            bytes += VectorBytes(track.states);
            for (const metaped::State& state : track.states)
                bytes += VectorBytes(state.expressions) + VectorBytes(state.shopItems);
        }
        for (const AppearanceTrack& track : clip.appearances)
            bytes += VectorBytes(track.changes);
        bytes += VectorBytes(clip.doors);
        for (const DoorTrack& door : clip.doors)
            bytes += VectorBytes(door.samples);
        bytes += VectorBytes(clip.attachFrames);
        for (const AttachFrameTrack& track : clip.attachFrames)
            bytes += VectorBytes(track.frames);
        bytes += VectorBytes(clip.world.ipls) + VectorBytes(clip.world.entitySets) + VectorBytes(clip.world.spheres);
        bytes += VectorBytes(clip.spans) + VectorBytes(clip.cuts) + VectorBytes(clip.abilities) + VectorBytes(clip.fragBreaks);
        bytes += VectorBytes(clip.fragPieces);
        for (const FragPiece& row : clip.fragPieces)
            bytes += VectorBytes(row.groups);
        return bytes;
    }
}
