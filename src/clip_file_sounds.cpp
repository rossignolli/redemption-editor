// clip_file_sounds.cpp: the sound sections: anim audio, sounds and their params, collision, entity,
// general and generic sounds, trains, the census, the mix probe, render targets, the game listener,
// wind, mix scenes, category state, entity queries, foley, ped speed, shader vars and the foot events.
// Split from clip_file.cpp (moved code); shared internals in clip_file_internal.h.

#include "clip_file_internal.h"

namespace replay::clipfile
{

    namespace
    {
        void WriteAnimAudioEvent(Writer& w, const entity::AnimAudioEvent& e)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Put(e.hash);
            w.Put(e.extra);
        }
        void ReadAnimAudioEvent(Reader& r, entity::AnimAudioEvent& e)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            e.hash = r.Get<std::uint32_t>();
            e.extra = r.Get<std::int32_t>();
        }
    }
        void WriteAnimAudio(Writer& w, const SaveContext& c) { WriteEvents(w, c.recording.animAudio.Events(), WriteAnimAudioEvent); }
        void ReadAnimAudio(Reader& r, LoadContext& c) { ReadEvents(r, c.recording.animAudio, 16, ReadAnimAudioEvent); }

    namespace
    {

        // Sound params without the engine's fields (see kSoundParams*).
        void WriteSoundParams(Writer& w, const std::vector<std::uint8_t>& params)
        {
            std::vector<std::uint8_t> copy = params;
            if (copy.size() > kSoundParamsEngineStart)
                ZeroBytes(copy.data(), copy.size(), kSoundParamsEngineStart, kSoundParamsEngineEnd - kSoundParamsEngineStart);
            if (copy.size() > kSoundParamsHandle)
                ZeroBytes(copy.data(), copy.size(), kSoundParamsHandle, kSoundParamsHandleBytes);
            w.Blob(copy);
        }
        void ReadSoundParams(Reader& r, std::vector<std::uint8_t>& params)
        {
            r.Blob(params);
            // Playback reads a full params block when there is one.
            if (!params.empty() && params.size() != kSoundParamsBytes)
                r.Fail();
        }
        void WriteSound(Writer& w, const entity::SoundEvent& e, const Image& image)
        {
            w.Put(e.timeMs);
            w.Id(e.entity);
            w.Put(e.audioOffset);
            w.Put(e.soundRef);
            WriteSoundParams(w, e.params);
            w.Array(e.flags);
            w.Put(e.kind);
            w.Put(SiteIdOf(ToRva(e.globalAudio, image))); // version 2
        }
        void ReadSound(Reader& r, entity::SoundEvent& e, const Image& image, std::uint16_t version)
        {
            e.timeMs = r.Get<std::uint32_t>();
            e.entity = r.Id();
            e.audioOffset = r.Get<std::uint64_t>();
            e.soundRef = r.Get<std::uint64_t>();
            ReadSoundParams(r, e.params);
            r.Array(e.flags);
            e.kind = r.Get<std::uint8_t>();
            const std::uint64_t audio = r.Get<std::uint64_t>();
            e.globalAudio = FromRva(version >= 2 ? RvaOfSite(audio) : RvaOf1491(audio), image);
        }
    }
        void WriteSounds(Writer& w, const SaveContext& c)
        {
            WriteEvents(w, c.recording.sounds.Events(),
                [&](Writer& writer, const entity::SoundEvent& e) { WriteSound(writer, e, c.image); });
        }
        void ReadSounds(Reader& r, LoadContext& c)
        {
            ReadEvents(r, c.recording.sounds, 69,
                [&](Reader& reader, entity::SoundEvent& e) { ReadSound(reader, e, c.image, c.sectionVersion); });
        }
        void WriteCollisionSounds(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.collisionSounds.size());
            for (const CollisionSound& sound : c.recording.collisionSounds)
            {
                w.Id(sound.entity);
                w.Put(sound.startMs);
                w.Put(sound.endMs);
                w.Bool(sound.ended);
                w.Put(sound.soundRef);
                WriteSoundParams(w, sound.params);
                w.Count(sound.intensity.size());
                for (const auto& [timeMs, value] : sound.intensity)
                {
                    w.Put(timeMs);
                    w.Put(value);
                }
            }
        }
        void ReadCollisionSounds(Reader& r, LoadContext& c)
        {
            c.recording.collisionSounds.resize(r.Count(37));
            for (CollisionSound& sound : c.recording.collisionSounds)
            {
                sound.entity = r.Id();
                sound.startMs = r.Get<std::uint32_t>();
                sound.endMs = r.Get<std::uint32_t>();
                sound.ended = r.Bool();
                sound.soundRef = r.Get<std::uint64_t>();
                ReadSoundParams(r, sound.params);
                sound.intensity.resize(r.Count(8));
                for (auto& [timeMs, value] : sound.intensity)
                {
                    timeMs = r.Get<std::uint32_t>();
                    value = r.Get<float>();
                }
            }
        }
        void WriteEntitySounds(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.entitySounds.size());
            for (const EntitySound& sound : c.recording.entitySounds)
            {
                w.Id(sound.entity);
                w.Put(sound.startMs);
                w.Put(sound.endMs);
                w.Bool(sound.ended);
                w.Put(sound.soundRef);
                w.Bool(sound.byHash);
                w.Bool(sound.audioOnEntity);
                w.Put(SiteIdOf(sound.audioRva)); // version 5
                w.Put(sound.audioOffset);
                w.Put(sound.trackerOffset);
                w.Put(SiteIdOf(sound.caller)); // version 5
                WriteSoundParams(w, sound.params);
                w.Bool(sound.oneShot);
                w.Bool(sound.pedAudio);
                w.Bool(sound.vehicleAudio);
            }
        }
        void ReadEntitySounds(Reader& r, LoadContext& c)
        {
            const bool ids = c.sectionVersion >= 5;
            c.recording.entitySounds.resize(r.Count(ids ? 51 : 47));
            for (EntitySound& sound : c.recording.entitySounds)
            {
                sound.entity = r.Id();
                sound.startMs = r.Get<std::uint32_t>();
                sound.endMs = r.Get<std::uint32_t>();
                sound.ended = r.Bool();
                sound.soundRef = r.Get<std::uint64_t>();
                sound.byHash = r.Bool();
                sound.audioOnEntity = r.Bool();
                const std::uint64_t audio = r.Get<std::uint64_t>();
                sound.audioRva = ids ? RvaOfSite(audio) : RvaOf1491(audio);
                sound.audioOffset = r.Get<std::uint32_t>();
                sound.trackerOffset = r.Get<std::uint32_t>();
                sound.caller = ids ? RvaOfSite(r.Get<std::uint64_t>()) : RvaOf1491(r.Get<std::uint32_t>());
                ReadSoundParams(r, sound.params);
                sound.oneShot = c.sectionVersion >= 2 ? r.Bool() : false;
                sound.pedAudio = c.sectionVersion >= 3 ? r.Bool() : false;
                sound.vehicleAudio = c.sectionVersion >= 4 ? r.Bool() : false;
                // The audio entity and tracker offsets
                // become engine pointers in playback: only the ones the
                // recorder can produce are accepted.
                if (!EntitySoundAudioValid(sound, c.image.size))
                    r.Fail();
            }
        }
        void WriteTrains(Writer& w, const SaveContext& c)
        {
            const trains::TrainData& data = c.recording.trains;
            w.Count(data.cars.size());
            for (const trains::TrainCar& car : data.cars)
            {
                w.Put(car.id);
                w.Put(car.head);
                w.Put(car.forward);
                w.Put(car.backward);
                w.Put(car.index);
                w.Put(car.cars);
                w.Put(car.flags);
                w.Put(car.vehicleType);
                w.Put(car.track);
                w.Put(car.propSet);
                w.Put(car.lightPropSet);
            }
            w.Count(data.speeds.size());
            for (const trains::SpeedTrack& track : data.speeds)
            {
                w.Put(track.head);
                w.Count(track.samples.size());
                for (const trains::SpeedSample& sample : track.samples)
                {
                    w.Put(sample.timeMs);
                    w.Put(sample.speed);
                    w.Put(sample.forward);
                }
            }
        }
        void ReadTrains(Reader& r, LoadContext& c)
        {
            trains::TrainData& data = c.recording.trains;
            data.Clear();
            data.cars.resize(r.Count(34));
            for (trains::TrainCar& car : data.cars)
            {
                car.id = r.Get<std::uint32_t>();
                car.head = r.Get<std::uint32_t>();
                car.forward = r.Get<std::uint32_t>();
                car.backward = r.Get<std::uint32_t>();
                car.index = r.Get<std::uint16_t>();
                car.cars = r.Get<std::uint16_t>();
                car.flags = r.Get<std::uint8_t>();
                car.vehicleType = r.Get<std::uint8_t>();
                car.track = r.Get<std::int32_t>();
                car.propSet = r.Get<std::uint32_t>();
                car.lightPropSet = r.Get<std::uint32_t>();
                if (!car.id || car.cars > trains::kMaxConsistCars || car.index >= trains::kMaxConsistCars)
                    r.Fail();
            }
            data.speeds.resize(r.Count(12));
            for (trains::SpeedTrack& track : data.speeds)
            {
                track.head = r.Get<std::uint32_t>();
                track.samples.resize(r.Count(9));
                std::uint32_t last = 0;
                for (trains::SpeedSample& sample : track.samples)
                {
                    sample.timeMs = r.Get<std::uint32_t>();
                    sample.speed = r.Get<float>();
                    sample.forward = r.Get<std::uint8_t>();
                    if (sample.timeMs < last || sample.forward > 1 || !std::isfinite(sample.speed))
                        r.Fail();
                    last = sample.timeMs;
                }
            }
        }
        void WriteSoundCensus(Writer& w, const SaveContext& c)
        {
            const census::Summary& summary = c.recording.soundCensus;
            w.Put(census::kWindowMs);
            w.Count(summary.keys.size());
            for (const census::SummaryKey& key : summary.keys)
            {
                w.Put(key.owner);
                w.Put(static_cast<std::uint8_t>(key.cls));
                w.Put(key.ref);
                for (const std::uint32_t caller : key.path)
                    w.Put(caller);
            }
            w.Count(summary.cells.size());
            for (const census::SummaryCell& cell : summary.cells)
            {
                w.Put(cell.window);
                w.Put(cell.key);
                w.Put(cell.count);
                w.Put(cell.refused);
            }
            w.Values(summary.covered);
            w.Put(summary.overflow);
        }
        void ReadSoundCensus(Reader& r, LoadContext& c)
        {
            census::Summary& summary = c.recording.soundCensus;
            summary.Clear();
            if (r.Get<std::uint32_t>() != census::kWindowMs)
                r.Fail();
            summary.keys.resize(r.Count(33));
            for (census::SummaryKey& key : summary.keys)
            {
                key.owner = r.Get<std::uint32_t>();
                const std::uint8_t cls = r.Get<std::uint8_t>();
                key.cls = static_cast<census::OwnerClass>(cls);
                key.ref = r.Get<std::uint64_t>();
                for (std::uint32_t& caller : key.path)
                    caller = RawCaller(r.Get<std::uint32_t>());
                if (cls >= census::kOwnerClassCount)
                    r.Fail();
            }
            if (summary.keys.size() > census::kMaxKeys)
                r.Fail();
            summary.cells.resize(r.Count(9));
            std::uint32_t last = 0;
            for (census::SummaryCell& cell : summary.cells)
            {
                cell.window = r.Get<std::uint32_t>();
                cell.key = r.Get<std::uint16_t>();
                cell.count = r.Get<std::uint16_t>();
                cell.refused = r.Get<std::uint8_t>();
                if (cell.key >= summary.keys.size() || cell.window < last)
                    r.Fail();
                last = cell.window;
            }
            r.Values(summary.covered);
            for (std::size_t n = 1; n < summary.covered.size(); ++n)
                if (summary.covered[n] <= summary.covered[n - 1])
                    r.Fail();
            summary.overflow = r.Get<std::uint64_t>();
        }
        void WriteGeneralSounds(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.generalSounds.size());
            for (const census::GeneralSound& sound : c.recording.generalSounds)
            {
                w.Put(sound.entity);
                w.Put(static_cast<std::uint8_t>(sound.cls));
                w.Put(sound.startMs);
                w.Put(sound.endMs);
                w.Bool(sound.ended);
                w.Bool(sound.oneShot);
                w.Bool(sound.preexisting);
                w.Bool(sound.follows);
                w.Put(sound.ref);
                w.Put(static_cast<std::uint8_t>(sound.base));
                w.Put(sound.field);
                const bool image = sound.base == census::AudioBase::Image;
                w.Put(image ? 0 : sound.offset);
                w.Put(SiteIdOf(sound.vtableRva)); // version 2
                w.Put(image ? SiteIdOf(static_cast<std::uint32_t>(sound.offset)) : 0ull);
                for (const std::uint32_t caller : sound.path)
                    w.Put(caller);
                WriteSoundParams(w, sound.params);
            }
        }
        void ReadGeneralSounds(Reader& r, LoadContext& c)
        {
            const bool ids = c.sectionVersion >= 2;
            c.recording.generalSounds.resize(r.Count(ids ? 68 : 56));
            for (census::GeneralSound& sound : c.recording.generalSounds)
            {
                sound.entity = r.Get<std::uint32_t>();
                const std::uint8_t cls = r.Get<std::uint8_t>();
                sound.startMs = r.Get<std::uint32_t>();
                sound.endMs = r.Get<std::uint32_t>();
                sound.ended = r.Bool();
                sound.oneShot = r.Bool();
                sound.preexisting = r.Bool();
                sound.follows = r.Bool();
                sound.ref = r.Get<std::uint64_t>();
                const std::uint8_t base = r.Get<std::uint8_t>();
                sound.field = r.Get<std::uint16_t>();
                sound.offset = r.Get<std::int32_t>();
                std::uint64_t object = 0;
                if (ids)
                {
                    sound.vtableRva = RvaOfSite(r.Get<std::uint64_t>());
                    object = r.Get<std::uint64_t>();
                }
                else
                    sound.vtableRva = RvaOf1491(r.Get<std::uint32_t>());
                for (std::uint32_t& caller : sound.path)
                    caller = RawCaller(r.Get<std::uint32_t>());
                ReadSoundParams(r, sound.params);
                if (cls >= census::kOwnerClassCount || base >= census::kAudioBaseCount)
                {
                    r.Fail();
                    continue;
                }
                sound.cls = static_cast<census::OwnerClass>(cls);
                sound.base = static_cast<census::AudioBase>(base);
                if (sound.base == census::AudioBase::Image)
                    sound.offset = static_cast<std::int32_t>(
                        ids ? RvaOfSite(object) : RvaOf1491(static_cast<std::uint32_t>(sound.offset)));
                if (!census::GeneralSoundValid(sound, c.image.size, kSoundParamsBytes))
                    r.Fail();
            }
        }
        void WriteSoundGeneric(Writer& w, const SaveContext& c)
        {
            const soundgeneric::Track& track = c.recording.soundGeneric;
            w.Put(track.nextIndex);
            w.Count(track.creates.size());
            for (const soundgeneric::CreatePacket& p : track.creates)
            {
                w.Put(p.timeMs);
                w.Put(p.index);
                w.Put(p.stableId);
                w.Enum(p.idKind);
                w.Put(p.rawRef);
                w.Put(p.owner);
                w.Enum(p.ownerClass);
                w.Enum(p.source);
                w.Put(p.core);
                w.Put<std::uint8_t>((p.hasPosition ? 1u : 0u) | (p.hasParams ? 2u : 0u) | (p.preexisting ? 4u : 0u) |
                    (p.params.hasTracker ? 8u : 0u) | (p.params.hasEnvGroup ? 0x10u : 0u)); // 0x10: version 4
                w.Put(p.createDelayMs);
                w.Array(p.params.position);
                w.Put(p.params.volume);
                w.Put(p.params.volume2);
                w.Put(p.params.pitch);
                w.Put(p.params.pan);
                w.Put(p.params.flags);
                w.Put(p.params.variableCount);
                w.Put<std::uint8_t>(0); // reserved
                // The slots past variableCount are written as zeros. The game
                // leaves them as they were in the init params (0xFFFFFFFF
                // seen, a NaN as a float), garbage the reader never uses.
                for (std::size_t n = 0; n < soundgeneric::kVariables; ++n)
                {
                    const bool used = n < p.params.variableCount;
                    w.Put(used ? p.params.variables[n].hash : 0u);
                    w.Put(used ? p.params.variables[n].value : 0.0f);
                }
                w.Put(SiteIdOf(p.caller)); // version 5
                w.Array(p.path);
                const census::AudioBase base = p.audioKnown ? p.audioBase : census::AudioBase::Image;
                const bool image = base == census::AudioBase::Image;
                w.Enum(base); // version 2
                w.Put(p.audioField);
                w.Put(image ? 0 : p.audioOffset);
                w.Put(SiteIdOf(p.audioKnown ? p.vtableRva : 0u)); // version 5
                w.Put(image ? SiteIdOf(static_cast<std::uint32_t>(p.audioOffset)) : 0ull);
                const std::uint8_t initCount = p.params.initVariableCount > soundgeneric::kInitVariables
                    ? static_cast<std::uint8_t>(soundgeneric::kInitVariables)
                    : p.params.initVariableCount; // version 3
                w.Put(initCount);
                for (std::uint8_t n = 0; n < initCount; ++n)
                {
                    w.Put(p.params.initVariables[n].hash);
                    w.Put(p.params.initVariables[n].value);
                }
                const std::uint8_t valueCount = p.params.valueCount > soundgeneric::kValueDwords
                    ? static_cast<std::uint8_t>(soundgeneric::kValueDwords)
                    : p.params.valueCount; // version 4
                w.Put(valueCount);
                for (std::uint8_t n = 0; n < valueCount; ++n)
                {
                    w.Put(p.params.values[n].index);
                    w.Put(p.params.values[n].raw);
                }
            }
            w.Count(track.adjusts.size());
            for (const soundgeneric::AdjustPacket& p : track.adjusts)
            {
                w.Put(p.timeMs);
                w.Put(p.index);
                w.Enum(p.kind);
                if (soundgeneric::IsVector(p.kind))
                    w.Array(p.value);
                else if (p.kind == soundgeneric::Adjust::Variable)
                {
                    w.Put(p.hash);
                    w.Put(p.value[0]);
                }
                else
                    w.Put(p.value[0]);
            }
            w.Count(track.ends.size());
            for (const soundgeneric::EndPacket& p : track.ends)
            {
                w.Put(p.timeMs);
                w.Put(p.index);
                w.Enum(p.reason);
            }
        }
        void ReadSoundGeneric(Reader& r, LoadContext& c)
        {
            soundgeneric::Track& track = c.recording.soundGeneric;
            track.Clear();
            track.nextIndex = r.Get<std::uint32_t>();
            // Every table sized by it must stay small (a 4 GiB nextIndex next
            // to one create asked the player for 16 GiB): refused past the
            // bound, the section left out.
            if (track.nextIndex > soundgeneric::kMaxNextIndex)
            {
                r.Fail();
                return;
            }
            const auto finite = [](float v) { return std::isfinite(v); };
            const std::uint16_t version = c.sectionVersion;
            track.creates.resize(r.Count(version >= 5   ? soundgeneric::kCreatePacketBytesV5Min
                    : version >= 4 ? soundgeneric::kCreatePacketBytesV4Min
                    : version >= 3 ? soundgeneric::kCreatePacketBytesV3Min
                    : version >= 2 ? soundgeneric::kCreatePacketBytes
                                   : soundgeneric::kCreatePacketBytesV1));
            std::uint32_t lastMs = 0;
            for (soundgeneric::CreatePacket& p : track.creates)
            {
                p.timeMs = r.Get<std::uint32_t>();
                p.index = r.Get<std::uint32_t>();
                p.stableId = r.Get<std::uint64_t>();
                p.idKind = r.Enum<soundgeneric::IdKind>(soundgeneric::kIdKindCount - 1);
                p.rawRef = r.Get<std::uint64_t>();
                p.owner = r.Get<std::uint32_t>();
                p.ownerClass = r.Enum<census::OwnerClass>(census::kOwnerClassCount - 1);
                p.source = r.Enum<soundgeneric::Source>(soundgeneric::kSourceCount - 1);
                p.core = r.Get<std::uint8_t>();
                const std::uint8_t bits = r.Get<std::uint8_t>();
                p.hasPosition = (bits & 1u) != 0;
                p.hasParams = (bits & 2u) != 0;
                p.preexisting = (bits & 4u) != 0;
                p.params.hasTracker = (bits & 8u) != 0;
                p.params.hasEnvGroup = version >= 4 && (bits & 0x10u) != 0;
                p.createDelayMs = r.Get<std::uint16_t>();
                r.Array(p.params.position);
                p.params.volume = r.Get<float>();
                p.params.volume2 = r.Get<float>();
                p.params.pitch = r.Get<std::int16_t>();
                p.params.pan = r.Get<std::int16_t>();
                p.params.flags = r.Get<std::uint8_t>();
                p.params.variableCount = r.Get<std::uint8_t>();
                r.Get<std::uint8_t>(); // reserved
                for (soundgeneric::Variable& v : p.params.variables)
                {
                    v.hash = r.Get<std::uint32_t>();
                    v.value = r.Get<float>();
                }
                p.caller = version >= 5 ? RvaOfSite(r.Get<std::uint64_t>()) : RvaOf1491(r.Get<std::uint32_t>());
                r.Array(p.path);
                if (version >= 2)
                {
                    p.audioBase = r.Enum<census::AudioBase>(census::kAudioBaseCount - 1);
                    p.audioField = r.Get<std::uint16_t>();
                    p.audioOffset = r.Get<std::int32_t>();
                    const bool image = p.audioBase == census::AudioBase::Image;
                    if (version >= 5)
                    {
                        p.vtableRva = RvaOfSite(r.Get<std::uint64_t>());
                        const std::uint64_t object = r.Get<std::uint64_t>();
                        if (image)
                            p.audioOffset = static_cast<std::int32_t>(RvaOfSite(object));
                    }
                    else
                    {
                        p.vtableRva = RvaOf1491(r.Get<std::uint32_t>());
                        if (image)
                            p.audioOffset = static_cast<std::int32_t>(RvaOf1491(static_cast<std::uint32_t>(p.audioOffset)));
                    }
                    p.audioKnown = p.vtableRva != 0;
                }
                else
                    p.audioKnown = false;
                if (version >= 3) // the init variables
                {
                    p.params.initVariableCount = r.Get<std::uint8_t>();
                    if (p.params.initVariableCount > soundgeneric::kInitVariables)
                        r.Fail();
                    for (std::uint8_t n = 0; n < p.params.initVariableCount && r.Ok(); ++n)
                    {
                        p.params.initVariables[n].hash = r.Get<std::uint32_t>();
                        p.params.initVariables[n].value = r.Get<float>();
                        if (!finite(p.params.initVariables[n].value))
                            r.Fail();
                    }
                }
                if (version >= 4) // the value dwords
                {
                    p.params.valueCount = r.Get<std::uint8_t>();
                    if (p.params.valueCount > soundgeneric::kValueDwords)
                        r.Fail();
                    for (std::uint8_t n = 0; n < p.params.valueCount && r.Ok(); ++n)
                    {
                        p.params.values[n].index = r.Get<std::uint8_t>();
                        p.params.values[n].raw = r.Get<std::uint32_t>();
                        if (!soundgeneric::IsValueDwordIndex(p.params.values[n].index))
                            r.Fail();
                    }
                    p.params.valuesKnown = true;
                }
                if (p.index >= track.nextIndex || p.timeMs < lastMs || p.core > 1 || bits > (version >= 4 ? 31 : 15) ||
                    p.params.variableCount > soundgeneric::kVariables || !finite(p.params.position[0]) ||
                    !finite(p.params.position[1]) || !finite(p.params.position[2]) || !finite(p.params.volume) ||
                    !finite(p.params.volume2))
                    r.Fail();
                // The slots past the count (possibly garbage) are zeroed.
                // The slots the count names are not checked as floats:
                // params +0x10C is a list of raw dwords the creators copy,
                // not (hash, value) pairs, and one can be a NaN read as a
                // float (0xFFFD933F seen). The dwords go back as they came.
                for (std::size_t n = 0; n < soundgeneric::kVariables; ++n)
                {
                    if (n >= p.params.variableCount)
                        p.params.variables[n] = soundgeneric::Variable{};
                }
                lastMs = p.timeMs;
                if (!r.Ok())
                    return;
            }
            track.adjusts.resize(r.Count(soundgeneric::kAdjustPacketHeadBytes + 4));
            lastMs = 0;
            for (soundgeneric::AdjustPacket& p : track.adjusts)
            {
                p.timeMs = r.Get<std::uint32_t>();
                p.index = r.Get<std::uint32_t>();
                p.kind = r.Enum<soundgeneric::Adjust>(soundgeneric::kAdjustKinds - 1);
                if (!r.Ok())
                    return;
                if (soundgeneric::IsVector(p.kind))
                    r.Array(p.value);
                else if (p.kind == soundgeneric::Adjust::Variable)
                {
                    p.hash = r.Get<std::uint32_t>();
                    p.value[0] = r.Get<float>();
                }
                else
                    p.value[0] = r.Get<float>();
                if (p.index >= track.nextIndex || p.timeMs < lastMs || !finite(p.value[0]) || !finite(p.value[1]) ||
                    !finite(p.value[2]))
                    r.Fail();
                lastMs = p.timeMs;
                if (!r.Ok())
                    return;
            }
            track.ends.resize(r.Count(soundgeneric::kEndPacketBytes));
            lastMs = 0;
            for (soundgeneric::EndPacket& p : track.ends)
            {
                p.timeMs = r.Get<std::uint32_t>();
                p.index = r.Get<std::uint32_t>();
                p.reason = r.Enum<soundgeneric::EndReason>(soundgeneric::kEndReasonCount - 1);
                if (p.index >= track.nextIndex || p.timeMs < lastMs)
                    r.Fail();
                lastMs = p.timeMs;
                if (!r.Ok())
                    return;
            }
        }

    namespace
    {
        constexpr std::size_t kMixSampleBytes = 4 + 4 + 4 + 8 + 4 + 2 + 2 + 2;
        constexpr std::size_t kMixListenerBytes = 4 + 9 * 4 + 2;
    }
        void WriteMixProbe(Writer& w, const SaveContext& c)
        {
            const mixprobe::Track& track = c.recording.mixProbe;
            w.Put(track.mainId);
            w.Put(track.dropped);
            w.Count(track.samples.size());
            for (const mixprobe::Sample& s : track.samples)
            {
                w.Put(s.timeMs);
                w.Put(s.owner);
                w.Put(s.source);
                w.Put(s.ref);
                w.Put(s.role);
                w.Put(s.kind);
                w.Put(s.lod);
                w.Put(s.flags);
                w.Put(s.dCamDm);
                w.Put(s.dPlayerDm);
                w.Put(s.dListenerDm);
            }
            w.Count(track.listeners.size());
            for (const mixprobe::Listener& l : track.listeners)
            {
                w.Put(l.timeMs);
                for (int n = 0; n < 3; ++n)
                    w.Put(l.cam[n]);
                for (int n = 0; n < 3; ++n)
                    w.Put(l.player[n]);
                for (int n = 0; n < 3; ++n)
                    w.Put(l.main[n]);
                w.Put(l.hasMain);
                w.Put(l.music);
            }
        }
        void ReadMixProbe(Reader& r, LoadContext& c)
        {
            mixprobe::Track& track = c.recording.mixProbe;
            track.Clear();
            track.mainId = r.Get<std::uint32_t>();
            track.dropped = r.Get<std::uint32_t>();
            const std::size_t samples = r.Count(kMixSampleBytes);
            track.samples.reserve(samples);
            for (std::size_t n = 0; n < samples && r.Ok(); ++n)
            {
                mixprobe::Sample s;
                s.timeMs = r.Get<std::uint32_t>();
                s.owner = r.Get<std::uint32_t>();
                s.source = r.Get<std::uint32_t>();
                s.ref = r.Get<std::uint64_t>();
                s.role = r.Get<std::uint8_t>();
                s.kind = r.Get<std::uint8_t>();
                s.lod = r.Get<std::uint8_t>();
                s.flags = r.Get<std::uint8_t>();
                s.dCamDm = r.Get<std::uint16_t>();
                s.dPlayerDm = r.Get<std::uint16_t>();
                s.dListenerDm = r.Get<std::uint16_t>();
                if (s.role >= mixprobe::kRoleCount || s.kind >= mixprobe::kKindCount)
                    r.Fail();
                track.samples.push_back(s);
            }
            const std::size_t listeners = r.Count(kMixListenerBytes);
            track.listeners.reserve(listeners);
            for (std::size_t n = 0; n < listeners && r.Ok(); ++n)
            {
                mixprobe::Listener l;
                l.timeMs = r.Get<std::uint32_t>();
                for (int k = 0; k < 3; ++k)
                    l.cam[k] = r.Get<float>();
                for (int k = 0; k < 3; ++k)
                    l.player[k] = r.Get<float>();
                for (int k = 0; k < 3; ++k)
                    l.main[k] = r.Get<float>();
                l.hasMain = r.Get<std::uint8_t>();
                l.music = r.Get<std::uint8_t>();
                track.listeners.push_back(l);
            }
        }
        bool OmitMixProbe(const SaveContext& c) { return c.recording.mixProbe.Empty(); }

    namespace
    {
        constexpr std::size_t kRenderTargetEventBytes = 4 + 4 + 4 + 4 + rendertarget::kNameBytes;
    }
        void WriteRenderTargets(Writer& w, const SaveContext& c)
        {
            const rendertarget::Track& track = c.recording.renderTargets;
            w.Put(track.dropped);
            w.Count(track.events.size());
            for (const rendertarget::Event& e : track.events)
            {
                w.Put(e.timeMs);
                w.Put(e.kind);
                w.Put(e.flag);
                w.Put(e.accepted);
                w.Put(e.initial);
                w.Put(e.hash);
                w.Put(e.entry);
                w.Raw(reinterpret_cast<const std::uint8_t*>(e.name), rendertarget::kNameBytes);
            }
        }
        void ReadRenderTargets(Reader& r, LoadContext& c)
        {
            rendertarget::Track& track = c.recording.renderTargets;
            track.Clear();
            track.dropped = r.Get<std::uint32_t>();
            const std::size_t count = r.Count(kRenderTargetEventBytes);
            track.events.reserve(count);
            std::uint32_t lastMs = 0;
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                rendertarget::Event e;
                e.timeMs = r.Get<std::uint32_t>();
                e.kind = r.Get<std::uint8_t>();
                e.flag = r.Get<std::uint8_t>();
                e.accepted = r.Get<std::uint8_t>();
                e.initial = r.Get<std::uint8_t>();
                e.hash = r.Get<std::uint32_t>();
                e.entry = r.Get<std::uint32_t>();
                r.Raw(reinterpret_cast<std::uint8_t*>(e.name), rendertarget::kNameBytes);
                if (e.kind >= rendertarget::kKindCount || e.flag > 1 || e.accepted > 1 || e.initial > 1 || e.timeMs < lastMs ||
                    !e.NameValid())
                    r.Fail();
                lastMs = e.timeMs;
                track.events.push_back(e);
            }
        }
        bool OmitRenderTargets(const SaveContext& c) { return c.recording.renderTargets.Empty(); }

    namespace
    {
        constexpr std::uint16_t kGameListenerV1Version = 1;
        constexpr std::size_t kGameListenerSampleBytes = 4 + 3 * 4 + 1 + 1;
        constexpr std::size_t kGameListenerV2MinBytes = 4 + 1 + 1;
    }
        void WriteGameListener(Writer& w, const SaveContext& c)
        {
            const gamelistener::Track& track = c.recording.gameListener;
            w.Count(track.samples.size());
            for (const gamelistener::Sample& s : track.samples)
            {
                w.Put(s.timeMs);
                for (int n = 0; n < 3; ++n)
                    w.Put(s.pos[n]);
                w.Put(s.points);
                w.Put(s.flags);
            }
            if (!track.AnyV2())
                return;
            w.Count(track.samples.size());
            for (const gamelistener::Sample& s : track.samples)
            {
                w.Put(s.micHash);
                w.Put(static_cast<std::uint8_t>(s.micSource));
                const std::uint8_t stored = std::min<std::uint8_t>(s.stored, gamelistener::kMaxRecordedPoints);
                w.Put(stored);
                for (std::uint8_t j = 0; j < stored; ++j)
                {
                    for (int n = 0; n < 3; ++n)
                        w.Put(s.point[j].pos[n]);
                    w.Put(s.point[j].weight);
                    w.Put(s.point[j].f70);
                }
            }
        }
        std::uint16_t GameListenerSaveVersion(const SaveContext& c)
        {
            return c.recording.gameListener.AnyV2() ? kGameListenerVersion : kGameListenerV1Version;
        }
        void ReadGameListener(Reader& r, LoadContext& c)
        {
            gamelistener::Track& track = c.recording.gameListener;
            track.Clear();
            const std::size_t samples = r.Count(kGameListenerSampleBytes);
            track.samples.reserve(samples);
            std::uint32_t last = 0;
            for (std::size_t n = 0; n < samples && r.Ok(); ++n)
            {
                gamelistener::Sample s;
                s.timeMs = r.Get<std::uint32_t>();
                for (int k = 0; k < 3; ++k)
                    s.pos[k] = r.Get<float>();
                s.points = r.Get<std::uint8_t>();
                s.flags = r.Get<std::uint8_t>();
                if ((n && s.timeMs < last) || !std::isfinite(s.pos[0]) || !std::isfinite(s.pos[1]) || !std::isfinite(s.pos[2]))
                    r.Fail();
                last = s.timeMs;
                track.samples.push_back(s);
            }
            if (c.sectionVersion < 2 || !r.Ok())
                return;
            const std::size_t v2 = r.Count(kGameListenerV2MinBytes);
            if (v2 != track.samples.size())
            {
                r.Fail();
                return;
            }
            for (gamelistener::Sample& s : track.samples)
            {
                if (!r.Ok())
                    break;
                s.micHash = r.Get<std::uint32_t>();
                s.micSource = r.Enum<gamelistener::MicSource>(static_cast<std::uint8_t>(gamelistener::MicSource::Default));
                s.stored = r.Get<std::uint8_t>();
                if (s.stored > gamelistener::kMaxRecordedPoints)
                {
                    r.Fail();
                    break;
                }
                for (std::uint8_t j = 0; j < s.stored; ++j)
                {
                    for (int k = 0; k < 3; ++k)
                        s.point[j].pos[k] = r.Get<float>();
                    s.point[j].weight = r.Get<float>();
                    s.point[j].f70 = r.Get<float>();
                    if (!std::isfinite(s.point[j].pos[0]) || !std::isfinite(s.point[j].pos[1]) ||
                        !std::isfinite(s.point[j].pos[2]))
                        r.Fail();
                }
            }
        }
        bool OmitGameListener(const SaveContext& c) { return c.recording.gameListener.Empty(); }

    namespace
    {
        constexpr std::size_t kWindAudioSampleBytes = 4 + 4 + 4 + 4 + 1;
    }
        void WriteWindAudio(Writer& w, const SaveContext& c)
        {
            const windreplay::Track& track = c.recording.windAudio;
            w.Count(track.samples.size());
            for (const windreplay::Sample& s : track.samples)
            {
                w.Put(s.timeMs);
                w.Put(s.strength);
                w.Put(s.lines[0]);
                w.Put(s.lines[1]);
                w.Put(s.flags);
            }
        }
        void ReadWindAudio(Reader& r, LoadContext& c)
        {
            windreplay::Track& track = c.recording.windAudio;
            track.Clear();
            const std::size_t samples = r.Count(kWindAudioSampleBytes);
            track.samples.reserve(samples);
            std::uint32_t last = 0;
            for (std::size_t n = 0; n < samples && r.Ok(); ++n)
            {
                windreplay::Sample s;
                s.timeMs = r.Get<std::uint32_t>();
                s.strength = r.Get<float>();
                s.lines[0] = r.Get<std::uint32_t>();
                s.lines[1] = r.Get<std::uint32_t>();
                s.flags = r.Get<std::uint8_t>();
                if ((n && s.timeMs < last) || !std::isfinite(s.strength) || (s.flags & ~windreplay::kSampleFlagsKnown) != 0)
                    r.Fail();
                last = s.timeMs;
                track.samples.push_back(s);
            }
        }
        bool OmitWindAudio(const SaveContext& c) { return c.recording.windAudio.Empty(); }

    namespace
    {
        constexpr std::uint16_t kMixScenesNoFlagsVersion = 2;
        constexpr std::size_t kMixSceneMinBytes = 4 + 4 + 4 + 1 + 4 + 4 + 1;
        constexpr std::size_t kMixGroupBytes = 4 + 4 + 4;
        constexpr std::size_t kMixVarBytes = 4 + 4 + 4 + 4;
    }
        void WriteMixScenes(Writer& w, const SaveContext& c)
        {
            const mixscene::Track& track = c.recording.mixScenes;
            w.Put(track.lostStops);
            w.Count(track.instances.size());
            for (const mixscene::Instance& i : track.instances)
            {
                w.Put(i.id);
                w.Put(i.hash);
                w.Put(i.caller);
                w.Put(static_cast<std::uint8_t>(i.kind));
                w.Put(i.startMs);
                w.Put(i.stopMs);
                w.Put(i.tagCount);
                for (std::uint8_t n = 0; n < i.tagCount; ++n)
                    w.Put(i.tags[n]);
            }
            w.Count(track.groups.size());
            for (const mixscene::GroupSample& g : track.groups)
            {
                w.Put(g.timeMs);
                w.Put(g.entity);
                w.Put(g.group);
            }
            w.Count(track.vars.size());
            for (const mixscene::VarSample& v : track.vars)
            {
                w.Put(v.timeMs);
                w.Put(v.id);
                w.Put(v.name);
                w.Put(v.value);
            }
            if (track.flags) // v3 (MixScenesSaveVersion)
                w.Put(track.flags);
        }
        std::uint16_t MixScenesSaveVersion(const SaveContext& c)
        {
            return c.recording.mixScenes.flags ? kMixScenesVersion : kMixScenesNoFlagsVersion;
        }
        void ReadMixScenes(Reader& r, LoadContext& c)
        {
            mixscene::Track& track = c.recording.mixScenes;
            track.Clear();
            track.lostStops = r.Get<std::uint32_t>();
            const std::size_t instances = r.Count(kMixSceneMinBytes);
            track.instances.reserve(instances);
            for (std::size_t n = 0; n < instances && r.Ok(); ++n)
            {
                mixscene::Instance i;
                i.id = r.Get<std::uint32_t>();
                i.hash = r.Get<std::uint32_t>();
                i.caller = RawCaller(r.Get<std::uint32_t>());
                i.kind = r.Enum<mixscene::Kind>(mixscene::kKindCount - 1);
                i.startMs = r.Get<std::uint32_t>();
                i.stopMs = r.Get<std::uint32_t>();
                i.tagCount = r.Get<std::uint8_t>();
                if (i.tagCount > mixscene::kMaxTags || (i.stopMs != mixscene::kOpen && i.stopMs < i.startMs))
                {
                    r.Fail();
                    return;
                }
                for (std::uint8_t t = 0; t < i.tagCount; ++t)
                    i.tags[t] = r.Get<std::uint32_t>();
                track.instances.push_back(i);
            }
            const std::size_t groups = r.Count(kMixGroupBytes);
            track.groups.reserve(groups);
            std::uint32_t lastMs = 0;
            for (std::size_t n = 0; n < groups && r.Ok(); ++n)
            {
                mixscene::GroupSample g;
                g.timeMs = r.Get<std::uint32_t>();
                g.entity = r.Get<std::uint32_t>();
                g.group = r.Get<std::uint32_t>();
                if (g.timeMs < lastMs)
                {
                    r.Fail();
                    return;
                }
                lastMs = g.timeMs;
                track.groups.push_back(g);
            }
            if (c.sectionVersion < 2)
                return; // v1: no variables
            const std::size_t vars = r.Count(kMixVarBytes);
            track.vars.reserve(vars);
            lastMs = 0;
            for (std::size_t n = 0; n < vars && r.Ok(); ++n)
            {
                mixscene::VarSample v;
                v.timeMs = r.Get<std::uint32_t>();
                v.id = r.Get<std::uint32_t>();
                v.name = r.Get<std::uint32_t>();
                v.value = r.Get<float>();
                if (v.timeMs < lastMs || !std::isfinite(v.value))
                {
                    r.Fail();
                    return;
                }
                lastMs = v.timeMs;
                track.vars.push_back(v);
            }
            if (c.sectionVersion < 3 || !r.Ok())
                return; // v2: no flags
            track.flags = r.Get<std::uint32_t>();
            if ((track.flags & ~mixscene::kTrackPoolScanned) != 0)
                r.Fail();
        }
        bool OmitMixScenes(const SaveContext& c) { return c.recording.mixScenes.Empty(); }
        void WriteCategoryState(Writer& w, const SaveContext& c)
        {
            const catstate::Track& track = c.recording.categoryState;
            w.Put(track.staticCount);
            w.Put(track.mapCount);
            w.Count(track.samples.size());
            for (const catstate::Sample& s : track.samples)
            {
                w.Put(s.timeMs);
                w.Put(s.hash);
                w.Put(s.v.volume);
                w.Put(s.v.rolloff);
                w.Put(s.v.x54);
                w.Put(s.v.pitch);
                w.Put(s.v.lpf);
                w.Put(s.v.hpf);
            }
        }
        void ReadCategoryState(Reader& r, LoadContext& c)
        {
            catstate::Track& track = c.recording.categoryState;
            track.Clear();
            track.staticCount = r.Get<std::uint32_t>();
            track.mapCount = r.Get<std::uint32_t>();
            const std::size_t count = r.Count(catstate::kSampleBytes);
            if (count > catstate::kMaxSamples)
            {
                r.Fail();
                return;
            }
            track.samples.resize(count);
            for (catstate::Sample& s : track.samples)
            {
                s.timeMs = r.Get<std::uint32_t>();
                s.hash = r.Get<std::uint32_t>();
                s.v.volume = r.Get<float>();
                s.v.rolloff = r.Get<float>();
                s.v.x54 = r.Get<float>();
                s.v.pitch = r.Get<std::int16_t>();
                s.v.lpf = r.Get<std::uint16_t>();
                s.v.hpf = r.Get<std::uint16_t>();
            }
            if (r.Ok() && !track.Valid())
                r.Fail();
        }
        bool OmitCategoryState(const SaveContext& c) { return c.recording.categoryState.Empty(); }
        void WriteEntityQueries(Writer& w, const SaveContext& c)
        {
            const entityquery::Track& track = c.recording.entityQueries;
            w.Count(track.chains.size());
            for (const entityquery::Chain& chain : track.chains)
            {
                w.Put(chain.hash);
                w.Put(chain.n);
                for (std::uint8_t n = 0; n < chain.n; ++n)
                    w.Put(chain.refs[n]);
            }
            w.Count(track.events.size());
            for (const entityquery::AnswerEvent& e : track.events)
            {
                w.Put(e.timeMs);
                w.Put(e.owner);
                w.Put(e.chain);
                w.Put(e.answer);
            }
        }
        void ReadEntityQueries(Reader& r, LoadContext& c)
        {
            entityquery::Track& track = c.recording.entityQueries;
            track.Clear();
            track.chains.resize(r.Count(entityquery::kChainBytesMin));
            if (track.chains.size() > entityquery::kMaxChains)
                r.Fail();
            for (entityquery::Chain& chain : track.chains)
            {
                if (!r.Ok())
                    return;
                chain.hash = r.Get<std::uint32_t>();
                chain.n = r.Get<std::uint8_t>();
                if (chain.n == 0 || chain.n > entityquery::kMaxChain)
                {
                    r.Fail();
                    return;
                }
                for (std::uint8_t n = 0; n < chain.n; ++n)
                    chain.refs[n] = r.Get<std::uint32_t>();
                if (chain.hash != entityquery::ChainHash(chain.n, chain.refs))
                    r.Fail();
            }
            if (!r.Ok())
                return;
            track.events.resize(r.Count(entityquery::kAnswerEventBytes));
            std::uint32_t lastMs = 0;
            for (entityquery::AnswerEvent& e : track.events)
            {
                e.timeMs = r.Get<std::uint32_t>();
                e.owner = r.Get<std::uint32_t>();
                e.chain = r.Get<std::uint16_t>();
                e.answer = r.Get<std::uint64_t>();
                if (e.chain >= track.chains.size() || e.timeMs < lastMs)
                    r.Fail();
                lastMs = e.timeMs;
                if (!r.Ok())
                    return;
            }
            track.Reindex();
        }
        void WriteFoleyVariants(Writer& w, const SaveContext& c)
        {
            const pedaudiostate::VariantTrack& track = c.recording.foleyVariants;
            w.Count(track.rows.size());
            for (const pedaudiostate::VariantRow& row : track.rows)
            {
                w.Put(row.owner);
                w.Put(row.a);
                w.Put(row.b);
                w.Put(row.player);
            }
        }
        void ReadFoleyVariants(Reader& r, LoadContext& c)
        {
            pedaudiostate::VariantTrack& track = c.recording.foleyVariants;
            track.Clear();
            track.rows.resize(r.Count(pedaudiostate::kVariantRowBytes));
            for (pedaudiostate::VariantRow& row : track.rows)
            {
                row.owner = r.Get<std::uint32_t>();
                row.a = r.Get<std::int32_t>();
                row.b = r.Get<std::int32_t>();
                row.player = r.Get<std::uint8_t>();
                if (!r.Ok())
                    return;
            }
            if (!track.Valid())
                r.Fail();
        }
        void WritePedSpeed(Writer& w, const SaveContext& c)
        {
            const pedspeed::Track& track = c.recording.pedSpeed;
            w.Count(track.series.size());
            for (const pedspeed::Series& s : track.series)
            {
                w.Put(s.owner);
                w.Count(s.points.size());
                for (const pedspeed::Point& p : s.points)
                {
                    w.Put(p.timeMs);
                    w.Put(p.speed);
                }
            }
        }
        void ReadPedSpeed(Reader& r, LoadContext& c)
        {
            pedspeed::Track& track = c.recording.pedSpeed;
            track.Clear();
            const std::size_t owners = r.Count(pedspeed::kSeriesHeaderBytes);
            if (owners > pedspeed::kMaxOwners)
            {
                r.Fail();
                return;
            }
            track.series.reserve(owners);
            for (std::size_t n = 0; n < owners && r.Ok(); ++n)
            {
                pedspeed::Series s;
                s.owner = r.Get<std::uint32_t>();
                const std::size_t points = r.Count(pedspeed::kPointBytes);
                if (points > pedspeed::kMaxPoints)
                {
                    r.Fail();
                    return;
                }
                s.points.resize(points);
                for (pedspeed::Point& p : s.points)
                {
                    p.timeMs = r.Get<std::uint32_t>();
                    p.speed = r.Get<float>();
                }
                track.series.push_back(std::move(s));
            }
            if (r.Ok() && !track.Valid())
                r.Fail();
        }
        bool OmitPedSpeed(const SaveContext& c) { return c.recording.pedSpeed.Empty(); }
        void WriteShaderVars(Writer& w, const SaveContext& c)
        {
            const shadervars::Track& track = c.recording.shaderVars;
            w.Count(track.series.size());
            for (const shadervars::Series& s : track.series)
            {
                w.Put(s.owner);
                w.Count(s.blocks.size());
                for (const shadervars::Block& b : s.blocks)
                {
                    w.Count(b.dofs.size());
                    for (const shadervars::Dof& dof : b.dofs)
                    {
                        w.Put(dof.track);
                        w.Put(dof.type);
                        w.Put(dof.id);
                    }
                    w.Count(b.times.size());
                    for (const std::uint32_t t : b.times)
                        w.Put(t);
                    for (const std::uint16_t v : b.values)
                        w.Put(v);
                }
            }
        }
        void ReadShaderVars(Reader& r, LoadContext& c)
        {
            shadervars::Track& track = c.recording.shaderVars;
            track.Clear();
            const std::size_t owners = r.Count(4 + 8);
            if (owners > shadervars::kMaxOwners)
            {
                r.Fail();
                return;
            }
            track.series.reserve(owners);
            for (std::size_t n = 0; n < owners && r.Ok(); ++n)
            {
                shadervars::Series s;
                s.owner = r.Get<std::uint32_t>();
                const std::size_t blocks = r.Count(8 + 4 + 8);
                if (blocks > shadervars::kMaxBlocks)
                {
                    r.Fail();
                    return;
                }
                for (std::size_t bi = 0; bi < blocks && r.Ok(); ++bi)
                {
                    shadervars::Block b;
                    const std::size_t dofs = r.Count(4);
                    if (dofs > shadervars::kMaxDofs)
                    {
                        r.Fail();
                        return;
                    }
                    b.dofs.resize(dofs);
                    for (shadervars::Dof& dof : b.dofs)
                    {
                        dof.track = r.Get<std::uint8_t>();
                        dof.type = r.Get<std::uint8_t>();
                        dof.id = r.Get<std::uint16_t>();
                    }
                    b.lanes = shadervars::LanesOf(b.dofs);
                    const std::size_t points = r.Count(4 + 2 * static_cast<std::size_t>(b.lanes ? b.lanes : 1));
                    if (points > shadervars::kMaxPoints || b.lanes > shadervars::kMaxLanes)
                    {
                        r.Fail();
                        return;
                    }
                    b.times.resize(points);
                    for (std::uint32_t& t : b.times)
                        t = r.Get<std::uint32_t>();
                    b.values.resize(points * b.lanes);
                    for (std::uint16_t& v : b.values)
                        v = r.Get<std::uint16_t>();
                    s.blocks.push_back(std::move(b));
                }
                track.series.push_back(std::move(s));
            }
            if (r.Ok() && !track.Valid())
                r.Fail();
        }
        bool OmitShaderVars(const SaveContext& c) { return c.recording.shaderVars.Empty(); }
        void WriteFootEvents(Writer& w, const SaveContext& c)
        {
            const footevent::Track& track = c.recording.footEvents;
            w.Count(track.events.size());
            for (const footevent::Event& e : track.events)
            {
                w.Put(e.owner);
                w.Put(e.timeMs);
                w.Put(e.type);
                w.Put(e.foot);
                w.Put(e.origin);
                w.Put(e.flags);
                w.Put(e.materialHash);
                w.Put(e.materialRef);
                w.Put(e.impact);
            }
        }
        void ReadFootEvents(Reader& r, LoadContext& c)
        {
            footevent::Track& track = c.recording.footEvents;
            track.Clear();
            const std::size_t count = r.Count(footevent::kEventBytes);
            if (count > footevent::kMaxEvents)
            {
                r.Fail();
                return;
            }
            track.events.resize(count);
            for (footevent::Event& e : track.events)
            {
                e.owner = r.Get<std::uint32_t>();
                e.timeMs = r.Get<std::uint32_t>();
                e.type = r.Get<std::int8_t>();
                e.foot = r.Get<std::uint8_t>();
                e.origin = r.Get<std::uint8_t>();
                e.flags = r.Get<std::uint8_t>();
                e.materialHash = r.Get<std::uint32_t>();
                e.materialRef = r.Get<std::uint32_t>();
                e.impact = r.Get<float>();
            }
            if (r.Ok() && !track.Valid())
                r.Fail();
        }
        bool OmitFootEvents(const SaveContext& c) { return c.recording.footEvents.Empty(); }
        void WriteFootMovement(Writer& w, const SaveContext& c)
        {
            const footmovement::Track& track = c.recording.footMovement;
            w.Count(track.samples.size());
            for (const footmovement::Sample& s : track.samples)
            {
                w.Put(s.owner);
                w.Put(s.timeMs);
                w.Put(s.foot);
                w.Put(s.result);
            }
        }
        void ReadFootMovement(Reader& r, LoadContext& c)
        {
            footmovement::Track& track = c.recording.footMovement;
            track.Clear();
            const std::size_t count = r.Count(footmovement::kSampleBytes);
            if (count > footmovement::kMaxSamples)
            {
                r.Fail();
                return;
            }
            track.samples.resize(count);
            for (footmovement::Sample& s : track.samples)
            {
                s.owner = r.Get<std::uint32_t>();
                s.timeMs = r.Get<std::uint32_t>();
                s.foot = r.Get<std::uint8_t>();
                s.result = r.Get<float>();
            }
            if (r.Ok() && !track.Valid())
                r.Fail();
        }
        bool OmitFootMovement(const SaveContext& c) { return c.recording.footMovement.Empty(); }
}
