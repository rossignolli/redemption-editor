// clip_file_events.cpp: the event sections: shots, speech, footsteps, explosions, surfaces, fx and
// registered fx (their attaches and settings), decals, gore, blood, fire, melee hits, body impacts and
// the weapon fx intents.
// Split from clip_file.cpp (moved code); shared internals in clip_file_internal.h.

#include "clip_file_internal.h"

namespace replay::clipfile
{

    namespace
    {
        void WriteShot(Writer& w, const entity::ShotEvent& shot)
        {
            w.Put(shot.timeMs);
            w.Id(shot.shooter);
            w.Id(shot.weapon);
            w.Put(shot.weaponHash);
            WriteVec3(w, shot.muzzle);
            WriteVec3(w, shot.direction);
            WriteVec3(w, shot.impact);
            w.Bool(shot.hasImpact);
            WriteVec3(w, shot.impactNormal);
            w.Bool(shot.hasNormal);
            w.Put(shot.muzzleFx);
            w.Put(shot.impactFx);
            w.Put(shot.muzzleFlags);
            w.Bool(shot.hasMuzzleFlags);
            std::array<std::uint8_t, kFireInfoBytes> info = shot.fireInfo;
            for (const entity::ShotEvent::FireRefField& ref : shot.fireRefs)
                ZeroBytes(info.data(), info.size(), ref.offset, sizeof(std::uint64_t));
            w.Raw(info.data(), info.size());
            w.Bool(shot.hasFireInfo);
            w.Count(shot.fireRefs.size());
            for (const entity::ShotEvent::FireRefField& ref : shot.fireRefs)
            {
                w.Put(ref.offset);
                w.Enum(ref.role);
                w.Id(ref.target);
                w.Id(ref.via);
                w.Put(ref.viaOffset);
            }
            w.Put(shot.shooterPlayer);
        }
        void ReadShot(Reader& r, entity::ShotEvent& shot, std::uint16_t version)
        {
            shot.timeMs = r.Get<std::uint32_t>();
            shot.shooter = r.Id();
            shot.weapon = r.Id();
            shot.weaponHash = r.Get<std::uint32_t>();
            shot.muzzle = ReadVec3(r);
            shot.direction = ReadVec3(r);
            shot.impact = ReadVec3(r);
            shot.hasImpact = r.Bool();
            shot.impactNormal = ReadVec3(r);
            shot.hasNormal = r.Bool();
            shot.muzzleFx = r.Get<std::uint32_t>();
            shot.impactFx = r.Get<std::uint32_t>();
            shot.muzzleFlags = r.Get<std::uint32_t>();
            shot.hasMuzzleFlags = r.Bool();
            r.Raw(shot.fireInfo.data(), shot.fireInfo.size());
            shot.hasFireInfo = r.Bool();
            shot.fireRefs.resize(r.Count(13));
            for (entity::ShotEvent::FireRefField& ref : shot.fireRefs)
            {
                ref.offset = r.Get<std::uint16_t>();
                ref.role = static_cast<entity::ShotEvent::FireRef>(r.Get<std::uint8_t>());
                if (ref.role < entity::ShotEvent::FireRef::Shooter || ref.role > entity::ShotEvent::FireRef::Unknown ||
                    ref.offset + sizeof(std::uint64_t) > kFireInfoBytes)
                {
                    r.Fail();
                }
                ref.target = r.Id();
                ref.via = r.Id();
                ref.viaOffset = r.Get<std::uint16_t>();
            }
            shot.shooterPlayer = version >= 2 ? r.Get<std::uint8_t>() : 0;
            if (shot.shooterPlayer > 2)
                r.Fail();
        }
    }
        void WriteShots(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.shots.Events(), WriteShot); }
        void ReadShots(Reader& r, LoadContext& c)
        {
            ReadEvents(r, c.recording.shots, 240,
                [&](Reader& reader, entity::ShotEvent& shot) { ReadShot(reader, shot, c.sectionVersion); });
        }

    namespace
    {
        void WriteSpeech(Writer& w, const entity::SpeechEvent& e)
        {
            w.Put(e.timeMs);
            w.Id(e.speaker);
            w.Put(e.voiceHash);
            w.Put(e.contextPartialHash);
            w.Put(e.variation);
            w.Put(e.requestedContextPartialHash);
            w.Put(e.voiceObjectType);
            w.Put(e.voiceObjectPartial);
            w.Put(e.dataType);
            w.Put(e.path);
            w.Put(e.coreMs);
            w.Bool(e.timedByStart);
            w.Put(e.stopMs);
            w.Put(e.stopReason);
            w.Put(e.endMs);
            w.Put(e.startedBeforeMs);
        }
        void ReadSpeech(Reader& r, entity::SpeechEvent& e, std::uint16_t version)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.speaker = r.Id();
            e.voiceHash = r.Get<std::uint32_t>();
            e.contextPartialHash = r.Get<std::uint32_t>();
            e.variation = r.Get<std::int32_t>();
            e.requestedContextPartialHash = r.Get<std::uint32_t>();
            e.voiceObject = 0;
            e.voiceObjectType = r.Get<std::uint8_t>();
            e.voiceObjectPartial = r.Get<std::uint32_t>();
            e.dataType = r.Get<std::uint32_t>();
            e.path = r.Get<std::uint32_t>();
            e.coreMs = r.Get<std::uint32_t>();
            e.timedByStart = r.Bool();
            e.stopMs = r.Get<std::uint32_t>();
            e.stopReason = r.Get<std::uint32_t>();
            e.endMs = version >= 2 ? r.Get<std::uint32_t>() : 0;
            e.startedBeforeMs = version >= 3 ? r.Get<std::uint32_t>() : 0;
        }
    }
        void WriteSpeechLines(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.speechLines.Events(), WriteSpeech); }
        void ReadSpeechLines(Reader& r, LoadContext& c)
        {
            ReadEvents(r, c.recording.speechLines, 50,
                [&](Reader& reader, entity::SpeechEvent& e) { ReadSpeech(reader, e, c.sectionVersion); });
        }
        bool OmitSpeechRequests(const SaveContext& c)
        {
            for (const entity::SpeechEvent& e : c.recording.speechLines.Events())
                if (e.request.read || e.request.setRead)
                    return false;
            return true;
        }
        void WriteSpeechRequests(Writer& w, const SaveContext& c)
        {
            const auto& events = c.recording.speechLines.Events();
            w.Put(static_cast<std::uint16_t>(speech::request::kFieldCount));
            w.Count(events.size());
            for (const entity::SpeechEvent& e : events)
            {
                w.Bool(e.request.read);
                for (std::size_t f = 0; f < speech::request::kFieldCount; ++f)
                    w.Put(e.request.value[f]);
                w.Bool(e.request.setRead); // v2
                w.Put(e.request.setHash);
                w.Put(e.request.setRef);
            }
        }
        void ReadSpeechRequests(Reader& r, LoadContext& c)
        {
            const bool withSet = c.sectionVersion >= 2;
            const std::uint16_t fields = r.Get<std::uint16_t>();
            const std::size_t count = r.Count(1 + 4u * fields + (withSet ? 9u : 0u));
            // One row per speech line, the same field count as this build.
            if (!r.Ok() || fields != speech::request::kFieldCount || count != c.recording.speechLines.Events().size())
            {
                r.Fail();
                return;
            }
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                speech::request::Fields& request = c.recording.speechLines.At(n).request;
                request.read = r.Bool();
                for (std::size_t f = 0; f < speech::request::kFieldCount; ++f)
                    request.value[f] = r.Get<std::uint32_t>();
                request.setRead = false;
                request.setHash = request.setRef = 0;
                if (withSet)
                {
                    request.setRead = r.Bool();
                    request.setHash = r.Get<std::uint32_t>();
                    request.setRef = r.Get<std::uint32_t>();
                }
            }
        }
        void ResetSpeechRequests(LoadContext& c)
        {
            const std::size_t count = c.recording.speechLines.Events().size();
            for (std::size_t n = 0; n < count; ++n)
                c.recording.speechLines.At(n).request = speech::request::Fields{};
        }

    namespace
    {
        void WriteFootstep(Writer& w, const entity::FootstepEvent& e)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Put(e.eventId);
            w.Put(e.arg3);
            w.Put(e.foot);
            w.Put(e.arg5);
            w.Put(e.flag);
            w.Put(e.arg8);
            w.Put(e.arg7);
            w.Put(e.materialHash);
            w.Put(e.materialRef);
            w.Put(e.wetness);
            w.Put(e.mixKnown);
            w.Put(e.mixVolume);
            w.Put(e.mixHighPass);
            w.Put(e.mixLowPass);
            w.Put(e.driveKnown);
            w.Put(e.driveBase);
            w.Put(e.driveImpact);
            w.Put(e.layersKnown);
            for (const std::uint16_t layer : e.layer)
                w.Put(layer);
            w.Put(e.audioLod);
        }
        void ReadFootstep(Reader& r, entity::FootstepEvent& e, std::uint16_t version)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            e.eventId = r.Get<std::int32_t>();
            e.arg3 = r.Get<std::uint32_t>();
            e.foot = r.Get<std::uint8_t>();
            e.arg5 = r.Get<std::uint8_t>();
            e.flag = r.Get<std::uint8_t>();
            e.arg8 = r.Get<std::uint8_t>();
            e.arg7 = r.Get<std::uint32_t>();
            e.materialHash = version >= 2 ? r.Get<std::uint32_t>() : 0u;
            e.materialRef = version >= 2 ? r.Get<std::uint32_t>() : 0u;
            e.wetness = version >= 3 ? r.Get<std::uint8_t>() : std::uint8_t{0};
            if (version >= 4)
            {
                e.mixKnown = r.Get<std::uint8_t>();
                e.mixVolume = r.Get<float>();
                e.mixHighPass = r.Get<std::uint16_t>();
                e.mixLowPass = r.Get<std::uint16_t>();
            }
            if (version >= 5)
            {
                e.driveKnown = r.Get<std::uint8_t>();
                e.driveBase = r.Get<float>();
                e.driveImpact = r.Get<float>();
            }
            if (version >= 6)
            {
                e.layersKnown = r.Get<std::uint8_t>();
                for (std::uint16_t& layer : e.layer)
                    layer = r.Get<std::uint16_t>();
                e.audioLod = r.Get<std::uint8_t>();
            }
        }
    }
        void WriteFootsteps(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.footsteps.Events(), WriteFootstep); }
        void ReadFootsteps(Reader& r, LoadContext& c)
        {
            ReadEvents(r, c.recording.footsteps, 24,
                [&](Reader& reader, entity::FootstepEvent& step) { ReadFootstep(reader, step, c.sectionVersion); });
        }

    namespace
    {
        void WriteExplosion(Writer& w, const entity::ExplosionAudioEvent& e)
        {
            w.Put(e.timeMs);
            w.Bool(e.hasInstance);
            w.Put(e.tag);
            w.Put(e.overrideRef);
            w.Array(e.position);
            w.Put(e.underwater);
            w.Put(e.b31);
            w.Put(e.b32);
            w.Put(e.interior);
            w.Put(e.caller);
        }
        void ReadExplosion(Reader& r, entity::ExplosionAudioEvent& e)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.hasInstance = r.Bool();
            e.tag = r.Get<std::int32_t>();
            e.overrideRef = r.Get<std::uint32_t>();
            r.Array(e.position);
            e.underwater = r.Get<std::uint8_t>();
            e.b31 = r.Get<std::uint8_t>();
            e.b32 = r.Get<std::uint8_t>();
            e.interior = r.Get<std::uint32_t>();
            e.caller = RawCaller(r.Get<std::uint32_t>());
        }
    }
        void WriteExplosions(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.explosions.Events(), WriteExplosion); }
        void ReadExplosions(Reader& r, LoadContext& c) { ReadEvents(r, c.recording.explosions, 36, ReadExplosion); }

    namespace
    {
        void WriteSurface(Writer& w, const entity::SurfaceEvent& e)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Array(e.wet);
            w.Put(e.wetFlags);
            w.Array(e.dirt);
        }
        void ReadSurface(Reader& r, entity::SurfaceEvent& e)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            r.Array(e.wet);
            e.wetFlags = r.Get<std::uint8_t>();
            r.Array(e.dirt);
        }
    }
        void WriteSurfaces(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.surfaces.Events(), WriteSurface); }
        void ReadSurfaces(Reader& r, LoadContext& c) { ReadEvents(r, c.recording.surfaces, 37, ReadSurface); }

    namespace
    {
        void WriteFx(Writer& w, const entity::FxEvent& e)
        {
            w.Put(e.timeMs);
            w.Put(e.effectHash);
            w.Put(e.assetHash);
            w.Put(SiteIdOf(e.callerSite)); // version 2
            w.Id(e.entity);
            w.Put(e.bone);
            w.Put(e.zoom);
            w.Put(e.flags);
            w.Array(e.matrix);
            w.Array(e.evolutions);
        }
        void ReadFx(Reader& r, entity::FxEvent& e, std::uint16_t version)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.effectHash = r.Get<std::uint32_t>();
            e.assetHash = r.Get<std::uint32_t>();
            e.callerSite = version >= 2 ? RvaOfSite(r.Get<std::uint64_t>()) : RvaOf1491(r.Get<std::uint32_t>());
            e.entity = r.Id();
            e.bone = r.Get<std::int32_t>();
            e.zoom = r.Get<float>();
            e.flags = r.Get<std::uint32_t>();
            r.Array(e.matrix);
            r.Array(e.evolutions);
        }
    }
        void WriteFxTrack(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.fx.Events(), WriteFx); }
        void ReadFxTrack(Reader& r, LoadContext& c)
        {
            const std::uint16_t version = c.sectionVersion;
            ReadEvents(r, c.recording.fx, version >= 2 ? 116 : 112,
                [version](Reader& reader, entity::FxEvent& e) { ReadFx(reader, e, version); });
        }
        void WriteRegisteredFx(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.registeredFx.size());
            for (const entity::RegisteredFxTrack& track : c.recording.registeredFx)
            {
                w.Put(track.effectHash);
                w.Put(track.flag);
                w.Put(track.arg8);
                w.Put(track.arg9);
                w.Put(SiteIdOf(track.callerSite)); // version 2
                w.Bool(track.attached);
                w.Id(track.attachId);
                w.Put(track.attachBone);
                w.Put(track.attachModel);
                w.Array(track.attachPos);
                w.Put(track.startMs);
                w.Put(track.endMs);
                w.Count(track.frames.size());
                for (const entity::RegisteredFxFrame& frame : track.frames)
                {
                    w.Put(frame.timeMs);
                    w.Array(frame.matrix);
                    w.Array(frame.parent);
                    w.Array(frame.evolutions);
                    w.Put(frame.zoom);
                    w.Array(frame.aux350);
                    w.Put(frame.flags);
                    w.Put(frame.planeCount);
                    w.Array(frame.planes);
                    w.Array(frame.start310);
                    w.Put(frame.farClip);
                }
            }
        }
        void ReadRegisteredFx(Reader& r, LoadContext& c)
        {
            const std::uint16_t version = c.sectionVersion;
            c.recording.registeredFx.resize(r.Count(version >= 2 ? 52 : 48));
            for (entity::RegisteredFxTrack& track : c.recording.registeredFx)
            {
                track.effectHash = r.Get<std::uint32_t>();
                track.flag = r.Get<std::uint8_t>();
                track.arg8 = r.Get<std::uint8_t>();
                track.arg9 = r.Get<std::uint8_t>();
                track.callerSite =
                    version >= 2 ? RvaOfSite(r.Get<std::uint64_t>()) : RvaOf1491(r.Get<std::uint32_t>());
                track.attached = r.Bool();
                track.attachId = r.Id();
                track.attachBone = r.Get<std::int32_t>();
                track.attachModel = r.Get<std::uint32_t>();
                r.Array(track.attachPos);
                track.startMs = r.Get<std::uint32_t>();
                track.endMs = r.Get<std::uint32_t>();
                track.frames.resize(r.Count(964));
                std::uint32_t lastMs = 0;
                for (entity::RegisteredFxFrame& frame : track.frames)
                {
                    frame.timeMs = r.Get<std::uint32_t>();
                    r.Array(frame.matrix);
                    r.Array(frame.parent);
                    r.Array(frame.evolutions);
                    frame.zoom = r.Get<float>();
                    r.Array(frame.aux350);
                    frame.flags = r.Get<std::uint32_t>();
                    frame.planeCount = r.Get<std::uint32_t>();
                    r.Array(frame.planes);
                    r.Array(frame.start310);
                    frame.farClip = r.Get<float>();
                    if (frame.planeCount > kRegisteredFxMaxPlanes || frame.timeMs < lastMs)
                        r.Fail();
                    lastMs = frame.timeMs;
                }
            }
        }

    namespace
    {
        void WriteDecal(Writer& w, const entity::DecalEvent& e)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Bool(e.attached);
            w.Put(e.attachModel);
            w.Array(e.attachPos);
            w.Put(SiteIdOf(e.callerReturn)); // version 3
            w.Put(e.engineId);
            w.Put(e.requestId);
            w.Put(e.flag);
            w.Bool(e.infoKnown);
            w.Put(e.infoHash);
            w.Put(e.infoIndex);
            std::uint8_t settings[sizeof(e.settings)];
            std::memcpy(settings, e.settings, sizeof(settings));
            ZeroBytes(settings, sizeof(settings), kDecalSettingsInfo, sizeof(std::uint64_t));
            w.Raw(settings, sizeof(settings));
            w.Bool(e.hasEntity); // version 2
        }
        void ReadDecal(Reader& r, entity::DecalEvent& e, std::uint16_t version)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            e.attached = r.Bool();
            e.attachModel = r.Get<std::uint32_t>();
            r.Array(e.attachPos);
            e.callerReturn = version >= 3 ? RvaOfSite(r.Get<std::uint64_t>()) : RvaOf1491(r.Get<std::uint32_t>());
            e.engineId = r.Get<std::uint32_t>();
            e.requestId = r.Get<std::uint32_t>();
            e.flag = r.Get<std::uint8_t>();
            e.infoKnown = r.Bool();
            e.infoHash = r.Get<std::uint32_t>();
            e.infoIndex = r.Get<std::int32_t>();
            r.Raw(e.settings, sizeof(e.settings));
            e.hasEntity = version >= 2 ? r.Bool() : false;
        }
    }
        void WriteDecals(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.decals.Events(), WriteDecal); }
        void ReadDecals(Reader& r, LoadContext& c)
        {
            const std::uint16_t version = c.sectionVersion;
            ReadEvents(r, c.recording.decals, version >= 3 ? 384 : 380, [version](Reader& reader, entity::DecalEvent& e) { ReadDecal(reader, e, version); });
        }

    namespace
    {
        void WriteDecalRemove(Writer& w, const entity::DecalRemoveEvent& e)
        {
            w.Put(e.timeMs);
            w.Put(e.engineId);
            w.Put(e.expired); // version 2
        }
        void ReadDecalRemove(Reader& r, entity::DecalRemoveEvent& e, std::uint16_t version)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.engineId = r.Get<std::uint32_t>();
            e.expired = version >= 2 ? r.Get<std::uint8_t>() : std::uint8_t{0};
            if (e.expired > 1)
                r.Fail();
        }
    }
        void WriteDecalRemoves(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.decalRemoves.Events(), WriteDecalRemove); }
        void ReadDecalRemoves(Reader& r, LoadContext& c)
        {
            const std::uint16_t version = c.sectionVersion;
            ReadEvents(r, c.recording.decalRemoves, 8,
                [version](Reader& reader, entity::DecalRemoveEvent& e) { ReadDecalRemove(reader, e, version); });
        }

    namespace
    {
        void WriteProjDecal(Writer& w, const entity::ProjDecalEvent& e)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Put(e.attachModel);
            w.Array(e.attachPos);
            w.Put(e.typeHash);
            w.Put(e.arg4);
            w.Array(e.position);
            w.Array(e.direction);
            w.Array(e.side);
            w.Put(e.size);
            w.Put(e.arg9);
            w.Put(e.arg10);
            w.Put(e.arg11);
            w.Put(e.arg12);
            w.Bool(e.infoKnown);
            w.Put(e.infoHash);
            w.Put(e.infoIndex);
        }
        void ReadProjDecal(Reader& r, entity::ProjDecalEvent& e)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            e.attachModel = r.Get<std::uint32_t>();
            r.Array(e.attachPos);
            e.typeHash = r.Get<std::uint32_t>();
            e.arg4 = r.Get<std::int32_t>();
            r.Array(e.position);
            r.Array(e.direction);
            r.Array(e.side);
            e.size = r.Get<float>();
            e.arg9 = r.Get<std::uint16_t>();
            e.arg10 = r.Get<std::uint8_t>();
            e.arg11 = r.Get<std::int32_t>();
            e.arg12 = r.Get<std::int32_t>();
            e.infoKnown = r.Bool();
            e.infoHash = r.Get<std::uint32_t>();
            e.infoIndex = r.Get<std::int32_t>();
        }
    }
        void WriteProjDecals(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.projDecals.Events(), WriteProjDecal); }
        void ReadProjDecals(Reader& r, LoadContext& c) { ReadEvents(r, c.recording.projDecals, 100, ReadProjDecal); }

    namespace
    {
        void WriteGore(Writer& w, const entity::GoreEvent& e)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Put(e.limb);
        }
        void ReadGore(Reader& r, entity::GoreEvent& e)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            e.limb = r.Get<std::int32_t>();
        }
    }
        void WriteGoreTrack(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.gore.Events(), WriteGore); }
        void ReadGoreTrack(Reader& r, LoadContext& c) { ReadEvents(r, c.recording.gore, 12, ReadGore); }

    namespace
    {
        void WriteGoreHit(Writer& w, const entity::GoreHitEvent& e)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Id(e.inflictor);
            std::uint8_t context[sizeof(e.context)];
            std::memcpy(context, e.context, sizeof(context));
            ZeroBytes(context, sizeof(context), kGoreContextPed, sizeof(std::uint64_t));
            ZeroBytes(context, sizeof(context), kGoreContextInflictor, sizeof(std::uint64_t));
            w.Raw(context, sizeof(context));
            w.Put(e.result);
            w.Put(e.caller);
        }
        void ReadGoreHit(Reader& r, entity::GoreHitEvent& e)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            e.inflictor = r.Id();
            r.Raw(e.context, sizeof(e.context));
            e.result = r.Get<std::uint8_t>();
            e.caller = RawCaller(r.Get<std::uint32_t>());
        }
    }
        void WriteGoreHits(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.goreHits.Events(), WriteGoreHit); }
        void ReadGoreHits(Reader& r, LoadContext& c) { ReadEvents(r, c.recording.goreHits, 97, ReadGoreHit); }

    namespace
    {
        void WriteBloodEvent(Writer& w, const entity::BloodEvent& e)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Put(e.slot);
            w.Put(SiteIdOf(e.caller)); // version 2
            w.Put(e.index);
            w.Put(e.age);
            w.Id(e.ref78Entity);
            std::uint8_t record[sizeof(e.record)];
            std::memcpy(record, e.record, sizeof(record));
            ZeroBytes(record, sizeof(record), kBloodRecordEntity, sizeof(std::uint64_t));
            w.Raw(record, sizeof(record));
        }
        void ReadBloodEvent(Reader& r, entity::BloodEvent& e, std::uint16_t version)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            e.slot = r.Get<std::uint8_t>();
            e.caller = version >= 2 ? RvaOfSite(r.Get<std::uint64_t>()) : RvaOf1491(r.Get<std::uint32_t>());
            e.index = r.Get<std::int32_t>();
            e.age = r.Get<float>();
            e.ref78Entity = r.Id();
            r.Raw(e.record, sizeof(e.record));
        }
    }
        void WriteBlood(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.blood.Events(), WriteBloodEvent); }
        void ReadBlood(Reader& r, LoadContext& c)
        {
            const std::uint16_t version = c.sectionVersion;
            ReadEvents(r, c.recording.blood, version >= 2 ? 381 : 377,
                [version](Reader& reader, entity::BloodEvent& e) { ReadBloodEvent(reader, e, version); });
        }

    namespace
    {
        void WriteFireStart(Writer& w, const entity::FireStartEvent& e, const Image& image)
        {
            w.Put(e.timeMs);
            w.Put(e.slot);
            w.Put(e.type);
            w.Put(e.group);
            w.Array(e.position);
            w.Put(e.rng);
            w.Id(e.entityA);
            w.Id(e.entityB);
            w.Put(e.caller);
            w.Bool(e.initial);
            w.Bool(e.initialRemnant);
            w.Put(e.initialAge);
            std::uint8_t desc[sizeof(e.desc)];
            std::memcpy(desc, e.desc, sizeof(desc));
            ZeroBytes(desc, sizeof(desc), kFireDescEntityA, sizeof(std::uint64_t));
            ZeroBytes(desc, sizeof(desc), kFireDescEntityB, sizeof(std::uint64_t));
            ZeroBytes(desc, sizeof(desc), kFireDescCulprit, sizeof(std::uint64_t));
            WriteU64At(desc, kFireDescImage, ToRva(ReadU64At(desc, kFireDescImage), image));
            w.Raw(desc, sizeof(desc));
            w.Put(e.entityAModel);
            w.Array(e.entityAPos);
            w.Put(e.entityBModel);
            w.Array(e.entityBPos);
            w.Bool(e.rebuilt);
        }
        void ReadFireStart(Reader& r, entity::FireStartEvent& e, const Image& image, std::uint16_t version)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.slot = r.Get<std::int32_t>();
            e.type = r.Get<std::uint32_t>();
            e.group = r.Get<std::uint32_t>();
            r.Array(e.position);
            e.rng = r.Get<std::uint64_t>();
            e.entityA = r.Id();
            e.entityB = r.Id();
            e.caller = RawCaller(r.Get<std::uint32_t>());
            e.initial = r.Bool();
            e.initialRemnant = r.Bool();
            e.initialAge = r.Get<float>();
            r.Raw(e.desc, sizeof(e.desc));
            WriteU64At(e.desc, kFireDescImage, FromRawRva(ReadU64At(e.desc, kFireDescImage), image));
            if (version >= 2)
            {
                e.entityAModel = r.Get<std::uint32_t>();
                r.Array(e.entityAPos);
                e.entityBModel = r.Get<std::uint32_t>();
                r.Array(e.entityBPos);
            }
            if (version >= 3)
                e.rebuilt = r.Bool();
        }
    }
        void WriteFireStarts(Writer& w, const SaveContext& c)
        {
            WriteEvents(w, c.recording.fireStarts.Events(),
                [&](Writer& writer, const entity::FireStartEvent& e) { WriteFireStart(writer, e, c.image); });
        }
        void ReadFireStarts(Reader& r, LoadContext& c)
        {
            ReadEvents(r, c.recording.fireStarts, 340,
                [&](Reader& reader, entity::FireStartEvent& e) { ReadFireStart(reader, e, c.image, c.sectionVersion); });
        }

    namespace
    {
        void WriteFireStop(Writer& w, const entity::FireStopEvent& e)
        {
            w.Put(e.timeMs);
            w.Put(e.start);
            w.Put(e.instant);
            w.Put(e.arg3);
        }
        void ReadFireStop(Reader& r, entity::FireStopEvent& e)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.start = r.Get<std::uint32_t>();
            e.instant = r.Get<std::uint8_t>();
            e.arg3 = r.Get<std::uint8_t>();
        }
    }
        void WriteFireStops(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.fireStops.Events(), WriteFireStop); }
        void ReadFireStops(Reader& r, LoadContext& c) { ReadEvents(r, c.recording.fireStops, 10, ReadFireStop); }

    namespace
    {
        void WriteFireItem(Writer& w, const entity::FireItemSample& e)
        {
            w.Put(e.timeMs);
            w.Put(e.start);
            w.Put(e.outer);
            w.Put(e.burnt);
            w.Put(e.ember);
            w.Put(e.glow);
            w.Put(e.width);
        }
        void ReadFireItem(Reader& r, entity::FireItemSample& e)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.start = r.Get<std::uint32_t>();
            e.outer = r.Get<float>();
            e.burnt = r.Get<float>();
            e.ember = r.Get<float>();
            e.glow = r.Get<float>();
            e.width = r.Get<float>();
        }
    }
        void WriteFireItems(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.fireItems.Events(), WriteFireItem); }
        void ReadFireItems(Reader& r, LoadContext& c) { ReadEvents(r, c.recording.fireItems, 28, ReadFireItem); }
        void WriteRegisteredFxAttaches(Writer& w, const SaveContext& c)
        {
            const std::vector<entity::RegisteredFxTrack>& tracks = c.recording.registeredFx;
            std::size_t withTimeline = 0;
            for (const entity::RegisteredFxTrack& track : tracks)
                withTimeline += track.attachTimeline.empty() ? 0 : 1;
            w.Count(withTimeline);
            for (std::size_t index = 0; index < tracks.size(); ++index)
            {
                if (tracks[index].attachTimeline.empty())
                    continue;
                w.Put(static_cast<std::uint32_t>(index));
                w.Count(tracks[index].attachTimeline.size());
                for (const entity::RegisteredFxAttach& entry : tracks[index].attachTimeline)
                {
                    w.Put(entry.timeMs);
                    w.Id(entry.id);
                    w.Put(entry.bone);
                }
            }
        }
        void ReadRegisteredFxAttaches(Reader& r, LoadContext& c)
        {
            std::vector<entity::RegisteredFxTrack>& tracks = c.recording.registeredFx;
            const std::size_t count = r.Count(4 + 8);
            std::uint32_t lastIndex = 0;
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                const std::uint32_t index = r.Get<std::uint32_t>();
                // Each track once, in order, and one the registeredFx section has.
                if (index >= tracks.size() || (n > 0 && index <= lastIndex) || !tracks[index].attachTimeline.empty())
                {
                    r.Fail();
                    return;
                }
                lastIndex = index;
                std::vector<entity::RegisteredFxAttach>& timeline = tracks[index].attachTimeline;
                timeline.resize(r.Count(12));
                std::uint32_t lastMs = 0;
                for (entity::RegisteredFxAttach& entry : timeline)
                {
                    entry.timeMs = r.Get<std::uint32_t>();
                    entry.id = r.Id();
                    entry.bone = r.Get<std::int32_t>();
                    if (entry.timeMs < lastMs)
                        r.Fail();
                    lastMs = entry.timeMs;
                }
            }
        }

    namespace
    {
        void WriteMelee(Writer& w, const entity::MeleeEvent& e, const Image& image)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Put(ToRva(e.infoVtable, image));
            w.Put(e.infoHash);
        }
        void ReadMelee(Reader& r, entity::MeleeEvent& e, const Image& image)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            e.info = 0;
            e.infoVtable = FromRawRva(r.Get<std::uint64_t>(), image);
            e.infoHash = r.Get<std::uint32_t>();
        }
    }
        void WriteMeleeHits(Writer& w, const SaveContext& c)
        {
            WriteEvents(w, c.recording.meleeHits.Events(),
                [&](Writer& writer, const entity::MeleeEvent& e) { WriteMelee(writer, e, c.image); });
        }
        void ReadMeleeHits(Reader& r, LoadContext& c)
        {
            ReadEvents(r, c.recording.meleeHits, 20,
                [&](Reader& reader, entity::MeleeEvent& e) { ReadMelee(reader, e, c.image); });
        }

    namespace
    {
        void WriteBodyImpact(Writer& w, const entity::BodyImpactEvent& e, const Image& image)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Id(e.other);
            w.Array(e.position);
            w.Put(e.arg4);
            w.Put(e.arg5);
            w.Put(ToRva(e.definitionVtable, image));
            w.Put(e.definitionHash);
            w.Put(e.arg7);
            w.Put(e.arg10);
        }
        void ReadBodyImpact(Reader& r, entity::BodyImpactEvent& e, const Image& image)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            e.other = r.Id();
            r.Array(e.position);
            e.arg4 = r.Get<std::uint64_t>();
            e.arg5 = r.Get<std::uint64_t>();
            e.definition = 0;
            e.definitionVtable = FromRawRva(r.Get<std::uint64_t>(), image);
            e.definitionHash = r.Get<std::uint32_t>();
            e.arg7 = r.Get<std::uint64_t>();
            e.arg10 = r.Get<std::uint64_t>();
        }
    }
        void WriteBodyImpacts(Writer& w, const SaveContext& c)
        {
            WriteEvents(w, c.recording.bodyImpacts.Events(),
                [&](Writer& writer, const entity::BodyImpactEvent& e) { WriteBodyImpact(writer, e, c.image); });
        }
        void ReadBodyImpacts(Reader& r, LoadContext& c)
        {
            ReadEvents(r, c.recording.bodyImpacts, 72,
                [&](Reader& reader, entity::BodyImpactEvent& e) { ReadBodyImpact(reader, e, c.image); });
        }
        void WriteWeaponFxIntents(Writer& w, const SaveContext& c)
        {
            const weaponfx::Track& track = c.recording.weaponFxIntents;
            w.Count(track.intents.size());
            for (const weaponfx::Intent& i : track.intents)
            {
                w.Put(i.timeMs);
                w.Put(i.shooter);
                w.Put(i.weapon);
                w.Put(i.kind);
                w.Put(i.bone);
                w.Put(i.argA);
                w.Put(i.fxHash);
                w.Put(i.fxHashAlt);
                w.Put(i.range);
                w.Array(i.vec);
            }
        }
        void ReadWeaponFxIntents(Reader& r, LoadContext& c)
        {
            weaponfx::Track& track = c.recording.weaponFxIntents;
            track.Clear();
            const std::size_t count = r.Count(weaponfx::kIntentBytes);
            if (count > weaponfx::kMaxIntents)
            {
                r.Fail();
                return;
            }
            track.intents.resize(count);
            for (weaponfx::Intent& i : track.intents)
            {
                i.timeMs = r.Get<std::uint32_t>();
                i.shooter = r.Get<std::uint32_t>();
                i.weapon = r.Get<std::uint32_t>();
                i.kind = r.Get<std::uint8_t>();
                i.bone = r.Get<std::int32_t>();
                i.argA = r.Get<std::int32_t>();
                i.fxHash = r.Get<std::uint32_t>();
                i.fxHashAlt = r.Get<std::uint32_t>();
                i.range = r.Get<float>();
                r.Array(i.vec);
            }
            if (r.Ok() && !track.Valid())
                r.Fail();
        }
        bool OmitWeaponFxIntents(const SaveContext& c) { return c.recording.weaponFxIntents.Empty(); }

    namespace
    {
        constexpr std::size_t kFxSettingsBytes = 4 + 11 * 4 + 2 + 4;
        constexpr std::size_t kMaxSettingsChanges = 4096;
        constexpr std::size_t kMaxCallGaps = 65536;
        void PutFxSettings(Writer& w, const entity::FxSettings& s)
        {
            w.Put(s.mask);
            w.Array(s.vel350);
            w.Array(s.tint380);
            w.Put(s.alpha38C);
            w.Put(s.aux39C);
            w.Put(s.lod3A0);
            w.Put(s.tele3A8);
            w.Put(s.cull3BC);
            w.Put(s.ev5CC);
        }
        void GetFxSettings(Reader& r, entity::FxSettings& s)
        {
            s.mask = r.Get<std::uint32_t>();
            if (s.mask & ~entity::FxSettings::kAll)
                r.Fail();
            r.Array(s.vel350);
            r.Array(s.tint380);
            s.alpha38C = r.Get<float>();
            s.aux39C = r.Get<float>();
            s.lod3A0 = r.Get<float>();
            s.tele3A8 = r.Get<float>();
            s.cull3BC = r.Get<std::uint16_t>();
            s.ev5CC = r.Get<std::uint32_t>();
            entity::FxSettingsKeepFinite(s); // no NaN or infinity into an instance
        }
    }
        void WriteFxSettings(Writer& w, const SaveContext& c)
        {
            const std::vector<entity::FxEvent>& events = c.recording.fx.Events();
            std::size_t with = 0;
            for (const entity::FxEvent& event : events)
                with += event.hasSettings ? 1 : 0;
            w.Count(with);
            for (std::size_t index = 0; index < events.size(); ++index)
            {
                if (!events[index].hasSettings)
                    continue;
                w.Put(static_cast<std::uint32_t>(index));
                PutFxSettings(w, events[index].settings);
            }
        }
        void ReadFxSettings(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(4 + kFxSettingsBytes);
            const std::size_t events = c.recording.fx.Events().size();
            std::uint32_t lastIndex = 0;
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                const std::uint32_t index = r.Get<std::uint32_t>();
                // Each event once, in order, one the fx section has.
                if (index >= events || (n > 0 && index <= lastIndex))
                {
                    r.Fail();
                    return;
                }
                lastIndex = index;
                entity::FxEvent& event = c.recording.fx.At(index);
                GetFxSettings(r, event.settings);
                event.hasSettings = r.Ok();
            }
        }
        void ResetFxSettings(LoadContext& c)
        {
            for (std::size_t index = 0; index < c.recording.fx.Events().size(); ++index)
            {
                c.recording.fx.At(index).hasSettings = false;
                c.recording.fx.At(index).settings = {};
            }
        }
        bool OmitFxSettings(const SaveContext& c)
        {
            for (const entity::FxEvent& event : c.recording.fx.Events())
                if (event.hasSettings)
                    return false;
            return true;
        }
        void WriteRegisteredFxSettings(Writer& w, const SaveContext& c)
        {
            const std::vector<entity::RegisteredFxTrack>& tracks = c.recording.registeredFx;
            std::size_t with = 0;
            for (const entity::RegisteredFxTrack& track : tracks)
                with += (track.settings.empty() && track.callGaps.empty()) ? 0 : 1;
            w.Count(with);
            for (std::size_t index = 0; index < tracks.size(); ++index)
            {
                const entity::RegisteredFxTrack& track = tracks[index];
                if (track.settings.empty() && track.callGaps.empty())
                    continue;
                w.Put(static_cast<std::uint32_t>(index));
                w.Count(track.settings.size());
                for (const entity::RegisteredFxSettingsChange& change : track.settings)
                {
                    w.Put(change.timeMs);
                    PutFxSettings(w, change.settings);
                }
                w.Count(track.callGaps.size());
                for (const entity::RegisteredFxGap& gap : track.callGaps)
                {
                    w.Put(gap.fromMs);
                    w.Put(gap.toMs);
                }
            }
        }
        void ReadRegisteredFxSettings(Reader& r, LoadContext& c)
        {
            std::vector<entity::RegisteredFxTrack>& tracks = c.recording.registeredFx;
            const std::size_t count = r.Count(4 + 8 + 8);
            std::uint32_t lastIndex = 0;
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                const std::uint32_t index = r.Get<std::uint32_t>();
                if (index >= tracks.size() || (n > 0 && index <= lastIndex) || !tracks[index].settings.empty() ||
                    !tracks[index].callGaps.empty())
                {
                    r.Fail();
                    return;
                }
                lastIndex = index;
                entity::RegisteredFxTrack& track = tracks[index];
                const std::size_t changes = r.Count(4 + kFxSettingsBytes);
                if (changes > kMaxSettingsChanges)
                {
                    r.Fail();
                    return;
                }
                track.settings.resize(changes);
                std::uint32_t lastMs = 0;
                for (entity::RegisteredFxSettingsChange& change : track.settings)
                {
                    change.timeMs = r.Get<std::uint32_t>();
                    GetFxSettings(r, change.settings);
                    if (change.timeMs < lastMs)
                        r.Fail();
                    lastMs = change.timeMs;
                }
                const std::size_t gaps = r.Count(8);
                if (gaps > kMaxCallGaps)
                {
                    r.Fail();
                    return;
                }
                track.callGaps.resize(gaps);
                std::uint32_t lastTo = 0;
                for (entity::RegisteredFxGap& gap : track.callGaps)
                {
                    gap.fromMs = r.Get<std::uint32_t>();
                    gap.toMs = r.Get<std::uint32_t>();
                    if (gap.toMs <= gap.fromMs || gap.fromMs < lastTo)
                        r.Fail();
                    lastTo = gap.toMs;
                }
            }
        }
        void ResetRegisteredFxSettings(LoadContext& c)
        {
            for (entity::RegisteredFxTrack& track : c.recording.registeredFx)
            {
                track.settings.clear();
                track.callGaps.clear();
            }
        }
        bool OmitRegisteredFxSettings(const SaveContext& c)
        {
            for (const entity::RegisteredFxTrack& track : c.recording.registeredFx)
                if (!track.settings.empty() || !track.callGaps.empty())
                    return false;
            return true;
        }
}
