// clip_file_internal.h: the writer, reader, contexts and section codecs the clip_file*.cpp files share.
// Internal to those files, not a public header.
#pragma once

#include "clip_file.h"
#include "clip_sites.h" // the game addresses written as site ids

#include "camera_marker_shake.h"

#include "camera_spline.h"
#include "clip_compress.h"
#include "world_state.h"
#include "object_check.h" // kMaxVisibilityFlips
#include "frag_piece.h"   // the loose pieces' groups
#include "hat_look.h"     // the scenery hats' layer cap
#include "weapon_look_core.h" // WCND version 2

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace replay::clipfile
{

        using entity::ReplayId;

        struct Image
        {
            std::uintptr_t base = 0;
            std::size_t size = 0;
        };
        std::uint64_t ToRva(std::uint64_t address, const Image& image);
        std::uint64_t FromRva(std::uint64_t rva, const Image& image);
        std::uint64_t FromRawRva(std::uint64_t rva, const Image& image);
        std::uint32_t RawCaller(std::uint32_t rva);
        std::uint64_t SiteIdOf(std::uint64_t rva);
        std::uint32_t RvaOfSite(std::uint64_t id);
        std::uint32_t RvaOf1491(std::uint64_t rva);
        void ZeroBytes(std::uint8_t* data, std::size_t size, std::size_t offset, std::size_t count);
        std::uint64_t ReadU64At(const std::uint8_t* data, std::size_t offset);
        void WriteU64At(std::uint8_t* data, std::size_t offset, std::uint64_t value);

        // Session pointers inside the raw engine blobs.
        constexpr std::size_t kDecalSettingsInfo = 0x88;   // info record, kept as (hash, index)
        constexpr std::size_t kBloodRecordEntity = 0x78;   // entity, kept as ref78Entity
        constexpr std::size_t kGoreContextPed = 0x00;      // ped, kept as entity
        constexpr std::size_t kGoreContextInflictor = 0x08; // inflictor, kept as inflictor
        constexpr std::size_t kFireDescEntityA = 0x40;     // kept as entityA
        constexpr std::size_t kFireDescEntityB = 0x48;     // kept as entityB
        constexpr std::size_t kFireDescCulprit = 0xC0;
        constexpr std::size_t kFireDescImage = 0xF0;       // pointer into the image: RVA
        // Sound init params: playback copies [0, 0xC8), [0xE8, 0x12C) and
        // [0x12E, 0x140) (the tracker at +0xC8, the wave slot, category and
        // environment group up to +0xE8 and the u16 handle at +0x12C are the
        // engine's), so those are not written. The tail after the handle is
        // kept: it holds value fields with non-zero defaults (ids -1, 0xFF, a
        // flags byte 0x20, from the ctor) that playback needs (audio_observer.cpp
        // CopyRecordedSoundParams). Only the handle is left out.
        constexpr std::size_t kSoundParamsBytes = 0x140;
        constexpr std::size_t kSoundParamsEngineStart = 0xC8;
        constexpr std::size_t kSoundParamsEngineEnd = 0xE8;
        constexpr std::size_t kSoundParamsHandle = 0x12C;
        constexpr std::size_t kSoundParamsHandleBytes = 2;
        constexpr std::size_t kFireInfoBytes = 0xA0;
        constexpr std::size_t kRegisteredFxMaxPlanes = 16;

        class Writer
        {
        public:
            explicit Writer(std::vector<std::uint8_t>& out) : m_out(out) {}

            template <typename T>
            void Put(T value)
            {
                static_assert(std::is_arithmetic_v<T>, "plain values only");
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
                m_out.insert(m_out.end(), bytes, bytes + sizeof(T));
            }
            void Bool(bool value) { Put<std::uint8_t>(value ? 1u : 0u); }
            void Id(ReplayId id) { Put(id.Packed()); }
            template <typename E>
            void Enum(E value)
            {
                Put(static_cast<std::underlying_type_t<E>>(value));
            }
            template <typename T, std::size_t N>
            void Array(const T (&values)[N])
            {
                for (const T& value : values)
                    Put(value);
            }
            void Count(std::size_t count) { Put(static_cast<std::uint64_t>(count)); }
            template <typename T>
            void Values(const std::vector<T>& values)
            {
                Count(values.size());
                for (const T& value : values)
                    Put(value);
            }
            void Raw(const std::uint8_t* data, std::size_t size) { m_out.insert(m_out.end(), data, data + size); }
            void Blob(const std::vector<std::uint8_t>& bytes)
            {
                Count(bytes.size());
                Raw(bytes.data(), bytes.size());
            }

        private:
            std::vector<std::uint8_t>& m_out;
        };

        class Reader
        {
        public:
            Reader(const std::uint8_t* data, std::size_t size) : m_data(data), m_size(size) {}

            template <typename T>
            T Get()
            {
                static_assert(std::is_arithmetic_v<T>, "plain values only");
                T value{};
                if (!m_ok || m_size - m_pos < sizeof(T))
                {
                    m_ok = false;
                    return value;
                }
                std::memcpy(&value, m_data + m_pos, sizeof(T));
                m_pos += sizeof(T);
                return value;
            }
            bool Bool()
            {
                const std::uint8_t value = Get<std::uint8_t>();
                if (value > 1)
                    m_ok = false;
                return value != 0;
            }
            ReplayId Id() { return ReplayId::FromPacked(Get<std::uint32_t>()); }
            // An enum value within [0, maxValue].
            template <typename E>
            E Enum(std::underlying_type_t<E> maxValue)
            {
                const auto value = Get<std::underlying_type_t<E>>();
                if (value > maxValue)
                    m_ok = false;
                return static_cast<E>(value);
            }
            template <typename T, std::size_t N>
            void Array(T (&values)[N])
            {
                for (T& value : values)
                    value = Get<T>();
            }
            // An element count, refused when the rest of the payload cannot
            // hold that many elements of at least minBytes each.
            std::size_t Count(std::size_t minBytes)
            {
                const std::uint64_t count = Get<std::uint64_t>();
                if (!m_ok)
                    return 0;
                if (minBytes == 0)
                    minBytes = 1;
                if (count > (m_size - m_pos) / minBytes)
                {
                    m_ok = false;
                    return 0;
                }
                return static_cast<std::size_t>(count);
            }
            template <typename T>
            void Values(std::vector<T>& values)
            {
                const std::size_t count = Count(sizeof(T));
                values.resize(count);
                for (T& value : values)
                    value = Get<T>();
            }
            void Raw(std::uint8_t* data, std::size_t size)
            {
                if (!m_ok || m_size - m_pos < size)
                {
                    m_ok = false;
                    return;
                }
                std::memcpy(data, m_data + m_pos, size);
                m_pos += size;
            }
            void Blob(std::vector<std::uint8_t>& bytes)
            {
                const std::size_t count = Count(1);
                bytes.resize(count);
                if (count)
                    Raw(bytes.data(), count);
            }
            void Fail() { m_ok = false; }
            bool Ok() const { return m_ok; }
            bool AtEnd() const { return m_pos == m_size; }
            std::size_t Position() const { return m_pos; } // where a refused payload stopped
            std::size_t Size() const { return m_size; }

        private:
            const std::uint8_t* m_data = nullptr;
            std::size_t m_size = 0;
            std::size_t m_pos = 0;
            bool m_ok = true;
        };

        struct SaveContext
        {
            const Recording& recording;
            const std::vector<pose::PoseRecordExport>& poses;
            const camera::Track& camera;
            Image image;
        };

        struct LoadContext
        {
            Recording& recording;
            std::vector<pose::PoseRecordExport>& poses;
            camera::Track& camera;
            Image image;
            // The version of the section being read (a section with a
            // minVersion reads older payloads, see Section).
            std::uint16_t sectionVersion = 0;
        };
        void WriteVec3(Writer& w, const pose::Vec3& v);
        pose::Vec3 ReadVec3(Reader& r);
        void WriteAppearance(Writer& w, const entity::Appearance& appearance);
        void ReadAppearance(Reader& r, entity::Appearance& appearance);

        // Timed event tracks: count, then each event.
        template <typename Event, typename WriteOne>
        void WriteEvents(Writer& w, const std::vector<Event>& events, WriteOne writeOne)
        {
            w.Count(events.size());
            for (const Event& event : events)
                writeOne(w, event);
        }

        template <typename Track, typename ReadOne>
        void ReadEvents(Reader& r, Track& track, std::size_t minBytes, ReadOne readOne)
        {
            track.Begin();
            const std::size_t count = r.Count(minBytes);
            std::uint32_t lastMs = 0;
            for (std::size_t index = 0; index < count && r.Ok(); ++index)
            {
                typename std::decay_t<decltype(track.Events())>::value_type event;
                readOne(r, event);
                // Add clamps a late time; a file must already be in order.
                if (event.timeMs < lastMs)
                    r.Fail();
                lastMs = event.timeMs;
                track.Add(event);
            }
        }

        // Section "shots" (SHOT). The entity references in the fire info
        // are session pointers (playback writes the puppets there): zeroed.
        // Version 2 adds the shooter's local player flag at the
        // end of each shot (version 1 reads it as unknown).
        constexpr std::uint16_t kShotsVersion = 2;
        void WriteShots(Writer& w, const SaveContext& c);
        void ReadShots(Reader& r, LoadContext& c);

        // Section "speechLines" (SPCH). voiceObject is a session pointer: 0.
        // Version 2 adds endMs (the natural end, 0 when unknown); a version 1
        // reads it as 0. Version 3 adds startedBeforeMs (a
        // line adopted already playing); older versions read it as 0.
        constexpr std::uint16_t kSpeechVersion = 3;
        constexpr std::uint16_t kSpeechMinVersion = 1;
        void WriteSpeechLines(Writer& w, const SaveContext& c);
        void ReadSpeechLines(Reader& r, LoadContext& c);

        // Section "speech requests" (SPRQ, speech_request.h), optional: per speech line, in the order of the
        // speechLines section (written before it in the file), whether the
        // request fields were read and their values. Left out when no line
        // has them; a clip without it plays its lines with the ctor's fields.
        // v2 (speech_conversation.h): each row
        // also has the line's sound set (read flag, name hash, ref); a v1
        // row has none (the playback then takes the set from the line's own
        // recorded sounds, ESND).
        constexpr std::uint16_t kSpeechRequestsVersion = 2;
        bool OmitSpeechRequests(const SaveContext& c);
        void WriteSpeechRequests(Writer& w, const SaveContext& c);
        void ReadSpeechRequests(Reader& r, LoadContext& c);
        void ResetSpeechRequests(LoadContext& c);

        // Section "footsteps" (FOOT). Version 2 adds the step container of
        // the foot (materialHash, materialRef); a version 1 payload loads them
        // as 0 (the puppet's own surface). Version 3 adds the foot's wetness
        // (one byte, 0 unknown). Version 4 adds the foot's mix (known byte,
        // volume f32, high-pass and low-pass u16). Version 5 adds the drive
        // (known bits, base f32, impact f32). Version 6 adds the ground layers
        // (known byte, five u16 thousandths) and the audio LOD byte.
        constexpr std::uint16_t kFootstepsVersion = 6;
        constexpr std::uint16_t kFootstepsMinVersion = 1;
        void WriteFootsteps(Writer& w, const SaveContext& c);
        void ReadFootsteps(Reader& r, LoadContext& c);

        // Section "explosions" (EXPL).
        constexpr std::uint16_t kExplosionsVersion = 1;
        void WriteExplosions(Writer& w, const SaveContext& c);
        void ReadExplosions(Reader& r, LoadContext& c);

        // Section "surfaces" (SURF).
        constexpr std::uint16_t kSurfacesVersion = 1;
        void WriteSurfaces(Writer& w, const SaveContext& c);
        void ReadSurfaces(Reader& r, LoadContext& c);

        // Section "fx" (FXEV): one-shot particle effects.
        // Version 2: callerSite as a site id; version 1 holds a 1491.50 RVA.
        constexpr std::uint16_t kFxVersion = 2;
        constexpr std::uint16_t kFxMinVersion = 1;
        void WriteFxTrack(Writer& w, const SaveContext& c);
        void ReadFxTrack(Reader& r, LoadContext& c);

        // Section "registeredFx" (RGFX): continuous particle effects.
        // Version 2: callerSite as a site id; version 1 holds a 1491.50 RVA.
        constexpr std::uint16_t kRegisteredFxVersion = 2;
        constexpr std::uint16_t kRegisteredFxMinVersion = 1;
        void WriteRegisteredFx(Writer& w, const SaveContext& c);
        void ReadRegisteredFx(Reader& r, LoadContext& c);

        // Section "decals" (DECL). settings +0x88 (the info pointer) is
        // already zero in the recording and forced to zero here.
        // Version 2 adds hasEntity after the settings; a version 1 payload
        // still loads (hasEntity false). Version 3: callerReturn as a site id
        // (before, a 1491.50 RVA).
        constexpr std::uint16_t kDecalsVersion = 3;
        constexpr std::uint16_t kDecalsMinVersion = 1;
        void WriteDecals(Writer& w, const SaveContext& c);
        void ReadDecals(Reader& r, LoadContext& c);

        // Section "decalRemoves" (DCRM).
        // Version 2 adds the expired byte; a version 1 payload loads
        // with expired 0.
        constexpr std::uint16_t kDecalRemovesVersion = 2;
        constexpr std::uint16_t kDecalRemovesMinVersion = 1;
        void WriteDecalRemoves(Writer& w, const SaveContext& c);
        void ReadDecalRemoves(Reader& r, LoadContext& c);

        // Section "projDecals" (PDCL).
        constexpr std::uint16_t kProjDecalsVersion = 1;
        void WriteProjDecals(Writer& w, const SaveContext& c);
        void ReadProjDecals(Reader& r, LoadContext& c);

        // Section "gore" (GORE).
        constexpr std::uint16_t kGoreVersion = 1;
        void WriteGoreTrack(Writer& w, const SaveContext& c);
        void ReadGoreTrack(Reader& r, LoadContext& c);

        // Section "goreHits" (GHIT). context +0x00 and +0x08 (ped and
        // inflictor) are already zero and forced to zero here.
        constexpr std::uint16_t kGoreHitsVersion = 1;
        void WriteGoreHits(Writer& w, const SaveContext& c);
        void ReadGoreHits(Reader& r, LoadContext& c);

        // Section "blood" (BLOD). record +0x78 (an entity pointer, kept as
        // ref78Entity) is already zero and forced to zero here.
        // Version 2: caller as a site id; version 1 holds a 1491.50 RVA.
        constexpr std::uint16_t kBloodVersion = 2;
        constexpr std::uint16_t kBloodMinVersion = 1;
        void WriteBlood(Writer& w, const SaveContext& c);
        void ReadBlood(Reader& r, LoadContext& c);

        // Section "fireStarts" (FIRS). desc +0x40, +0x48 (ids) and +0xC0
        // (culprit) are already zero and forced to zero here; +0xF0 points
        // into the game image and is written as an RVA.
        // Version 2: the world object model and position of an unrecorded
        // entity A/B after the descriptor; version 1 reads as none.
        // Version 3: the rebuilt flag (an
        // initial fire read from the engine's fire list); older reads false.
        constexpr std::uint16_t kFireStartsVersion = 3;
        void WriteFireStarts(Writer& w, const SaveContext& c);
        void ReadFireStarts(Reader& r, LoadContext& c);

        // Section "fireStops" (FSTP).
        constexpr std::uint16_t kFireStopsVersion = 1;
        void WriteFireStops(Writer& w, const SaveContext& c);
        void ReadFireStops(Reader& r, LoadContext& c);

        // Section "fireItems" (FITM).
        constexpr std::uint16_t kFireItemsVersion = 1;
        void WriteFireItems(Writer& w, const SaveContext& c);
        void ReadFireItems(Reader& r, LoadContext& c);

        // Section "meta" (META): the game build (see SetBuildIdentity).
        constexpr std::uint16_t kMetaVersion = 1;
        extern std::uint32_t g_buildTimestamp;
        extern std::uint64_t g_buildImageSize;
        void WriteMeta(Writer& w, const SaveContext&);
        void ReadMeta(Reader& r, LoadContext&);

        // Section "area" (AREA).
        constexpr std::uint16_t kAreaVersion = 1;
        void WriteArea(Writer& w, const SaveContext& c);
        void ReadArea(Reader& r, LoadContext& c);

        // Section "worldObjects" (WOBJ): how the moved or broken world
        // objects stood at the start.
        constexpr std::uint16_t kWorldObjectsVersion = 1;
        void WriteWorldObjects(Writer& w, const SaveContext& c);
        void ReadWorldObjects(Reader& r, LoadContext& c);

        // Section "fragBreaks" (FRAG): fragment groups that came off.
        constexpr std::uint16_t kFragBreaksVersion = 1;
        void WriteFragBreaks(Writer& w, const SaveContext& c);
        void ReadFragBreaks(Reader& r, LoadContext& c);

        // Section "frag pieces" (FPCE): per
        // loose piece of a fragment object, where its groups came from and
        // the groups it carries (frag_piece.h). Optional: without it a piece
        // plays as the whole object.
        constexpr std::uint16_t kFragPiecesVersion = 1;
        void WriteFragPieces(Writer& w, const SaveContext& c);
        void ReadFragPieces(Reader& r, LoadContext& c);

        // Section "scenery" (SCNY, optional): the objects of the scene that
        // are not of the map, as state (object_check.h).
        // v2: the visibility when recorded and its flips (u8, then a count and
        // u32 times); v1 reads visible.
        // v3: the entry's rule (u8,
        // objcheck::SceneryRule) and a map dressing entry's IPL (u32); v2
        // and v1 read rule 0 (legacy) and no IPL.
        constexpr std::uint16_t kSceneryVersion = 3;
        void WriteScenery(Writer& w, const SaveContext& c);
        void ReadScenery(Reader& r, LoadContext& c);

        // Section "scenery children" (SCCH, optional):
        // the objects attached to a scenery object (object_check.h), each
        // naming its parent by its index in SCNY. Written
        // only when the recording has one, so a clip without children stays
        // readable by a build that does not know the section (an unknown
        // section refuses the clip).
        constexpr std::uint16_t kSceneryChildrenVersion = 1;
        void WriteSceneryChildren(Writer& w, const SaveContext& c);
        void ReadSceneryChildren(Reader& r, LoadContext& c);
        bool OmitSceneryChildren(const SaveContext& c);

        // Section "scenery hats" (SCHT, optional, hat_look.h): the metaped layer of each scenery hat (a hat that fell
        // before the recording started), naming its entry by its index in
        // SCNY: the index, then the layers (the eight values of
        // _SET_META_PED_TAG each). Written only when a scenery entry has one;
        // an index past the scenery (SCNY left out or refused) is dropped.
        constexpr std::uint16_t kSceneryHatsVersion = 1;
        void WriteSceneryHats(Writer& w, const SaveContext& c);
        void ReadSceneryHats(Reader& r, LoadContext& c);
        bool OmitSceneryHats(const SaveContext& c);
        void ResetSceneryHats(LoadContext& c);

        // Section "ped visibility" (PVIS):
        // per recorded ped and horse, visible at its first sample and the
        // times it flipped since. Optional: without it every puppet shows.
        constexpr std::uint16_t kPedVisibilityVersion = 1;
        void WritePedVisibility(Writer& w, const SaveContext& c);
        void ReadPedVisibility(Reader& r, LoadContext& c);

        // Section "gameCamera" (GCAM): the rendered camera per captured
        // frame.
        constexpr std::uint16_t kGameCameraVersion = 1;
        void WriteGameCamera(Writer& w, const SaveContext& c);
        void ReadGameCamera(Reader& r, LoadContext& c);

        // Section "environment" (ENVR): clock and weather.
        constexpr std::uint16_t kEnvironmentVersion = 1;
        void WriteEnvironment(Writer& w, const SaveContext& c);
        void ReadEnvironment(Reader& r, LoadContext& c);

        // Section "environment track" (ENVT, optional): the
        // weather, clock, wind, rain and snow over time
        // (environment_track.h). A clip without it plays the start state.
        // v2 adds the weather manager's transition over time to each sample
        // (target hash, total, time left, flags: bit 0 known, bit 1 the
        // manager's flag byte). A v1 section still loads, its transition
        // unknown (the replay leaves the engine's alone).
        constexpr std::uint16_t kEnvironmentTrackVersion = 2;
        constexpr std::uint16_t kEnvironmentTrackMinVersion = 1;
        void WriteEnvironmentTrack(Writer& w, const SaveContext& c);
        void ReadEnvironmentTrack(Reader& r, LoadContext& c);

        // Section "sky track" (SKYT, optional): the cloud accumulators over
        // time and their periods (sky_track.h). A clip without it leaves the
        // clouds to the session.
        constexpr std::uint16_t kSkyTrackVersion = 1;
        void WriteSkyTrack(Writer& w, const SaveContext& c);
        void ReadSkyTrack(Reader& r, LoadContext& c);

        // Section "timecycle" (TCMD, timecycle_track.h):
        // the script timecycle modifier (key, index, strength, transition key
        // and index, progress, speed, fade) and the weather types with a
        // variation, over time. Optional: a clip without it leaves the
        // session's modifier and variations alone.
        constexpr std::uint16_t kTimecycleVersion = 1;
        void WriteTimecycle(Writer& w, const SaveContext& c);
        void ReadTimecycle(Reader& r, LoadContext& c);

        // Section "moon" (MOON, moon_track.h):
        // the sky object's two moon angles (+0xA80 turn, +0x8E8 orbit) over
        // time, with the drawn sun and moon and the clock date as evidence.
        // Optional: a clip without it plays with the session's moon (which
        // the replay gives back when the clip ends).
        constexpr std::uint16_t kMoonVersion = 1;
        void WriteMoon(Writer& w, const SaveContext& c);
        void ReadMoon(Reader& r, LoadContext& c);

        // Section "environment edit" (ENVO, optional): the editor's
        // environment settings of the clip (env_override.h). A clip without it
        // plays everything as recorded.
        // v2: 12 bytes appended for the experimental sun and moon rows (24
        // bytes); 1.0 cut the rows, so they are written as Recorded and
        // skipped when read (env_override.h). v1 (12 bytes) still read.
        constexpr std::uint16_t kEnvironmentEditVersion = 2;
        constexpr std::uint16_t kEnvironmentEditMinVersion = 1;
        void WriteEnvironmentEdit(Writer& w, const SaveContext& c);
        void ReadEnvironmentEdit(Reader& r, LoadContext& c);

        // Section "place" (PLAC, optional): zones, character,
        // weather name.
        constexpr std::uint16_t kPlaceVersion = 1;
        void WritePlace(Writer& w, const SaveContext& c);
        void ReadPlace(Reader& r, LoadContext& c);

        // Section "world info" (WINF, optional): the chapter, the
        // camp and a fingerprint of the map state at the clip start, small
        // and fixed (20 bytes) so the clip list reads it from the summary.
        constexpr std::uint16_t kWorldInfoVersion = 1;
        void WriteWorldInfo(Writer& w, const SaveContext& c);
        void ReadWorldInfo(Reader& r, LoadContext& c);

        // Section "world" (WRLD, optional): the IPL changes (the
        // active candidates at 0, then each change), their bounding spheres,
        // the interiors found near the scene and their entity set changes.
        constexpr std::uint16_t kWorldVersion = 1;
        void WriteWorld(Writer& w, const SaveContext& c);
        void ReadWorld(Reader& r, LoadContext& c);

        // Section "abilities" (ABIL, optional): Dead Eye and
        // Eagle Eye spans.
        constexpr std::uint16_t kAbilitiesVersion = 1;
        void WriteAbilities(Writer& w, const SaveContext& c);
        void ReadAbilities(Reader& r, LoadContext& c);

        // Section "weapon conditions" (WCND, optional).
        // Version 2 (weapon_look_core.h) is the whole look per state: also the
        // rust (permanent degradation), the sixth wear channel, the flags and
        // the engine's component list. Version 1 (dirt, soot, damage,
        // degradation) still reads, with flags 0 (the create's components
        // apply).
        constexpr std::uint16_t kWeaponConditionsVersion = 2;
        constexpr std::uint16_t kWeaponConditionsMinVersion = 1;
        void WriteWeaponConditions(Writer& w, const SaveContext& c);
        void ReadWeaponConditions(Reader& r, LoadContext& c);

        // Section "cuts" (CUTS, optional): the instants the recording clock
        // went on after a cutscene, fade or loading screen. A clip without it
        // loads with none.
        constexpr std::uint16_t kCutsVersion = 1;
        void WriteCuts(Writer& w, const SaveContext& c);
        void ReadCuts(Reader& r, LoadContext& c);

        // Section "spans" (SPAN, optional): the restricted-camera
        // spans (cutscene, script camera, no control).
        constexpr std::uint16_t kSpansVersion = 1;
        void WriteSpans(Writer& w, const SaveContext& c);
        void ReadSpans(Reader& r, LoadContext& c);

        // Section "scorched" (SCOR, optional): the charred state
        // changes of each recorded ped and horse.
        constexpr std::uint16_t kScorchedVersion = 1;
        void WriteScorched(Writer& w, const SaveContext& c);
        void ReadScorched(Reader& r, LoadContext& c);

        // Section "vehicle windows" (VWIN, optional): the
        // broken window mask changes of each recorded vehicle.
        constexpr std::uint16_t kVehicleWindowsVersion = 1;
        void WriteVehicleWindows(Writer& w, const SaveContext& c);
        void ReadVehicleWindows(Reader& r, LoadContext& c);

        // Section "water gpu" (WGPU) is retired (kRetiredSections below).

        // Section "vehicle burnt" (VBRN, optional): the
        // burnt state changes of each recorded vehicle (ScorchTrack layout).
        constexpr std::uint16_t kVehicleBurntVersion = 1;
        void WriteVehicleBurnt(Writer& w, const SaveContext& c);
        void ReadVehicleBurnt(Reader& r, LoadContext& c);

        // Section "vehicle parts" (VPRT, optional): the
        // lost wheel and door mask changes of each recorded vehicle (bits
        // 0..5 wheels, 6..15 doors, VehiclePartTrack).
        constexpr std::uint16_t kVehiclePartsVersion = 1;
        void WriteVehicleParts(Writer& w, const SaveContext& c);
        void ReadVehicleParts(Reader& r, LoadContext& c);

        // Section "vehicle doors" (VDOR, optional, left out when empty,
        // vehicle_doors.h): the door ratio changes of
        // each recorded vehicle and train car (script door 0..9, latch 15).
        // Clips without it play the doors as the puppet spawns them (shut).
        constexpr std::uint16_t kVehicleDoorsVersion = 1;
        void WriteVehicleDoors(Writer& w, const SaveContext& c);
        void ReadVehicleDoors(Reader& r, LoadContext& c);
        bool OmitVehicleDoors(const SaveContext& c);

        // Section "burn levels" (BLVL, optional): the burn level changes of
        // each recorded vehicle, ped and horse (BurnLevelTrack), and of each
        // recorded weapon object and prop (same payload, version 1). Clips
        // without it play without the level (the fire replayed on a vehicle
        // puppet darkens it; peds and horses stay clean).
        constexpr std::uint16_t kBurnLevelsVersion = 1;
        void WriteBurnLevels(Writer& w, const SaveContext& c);
        void ReadBurnLevels(Reader& r, LoadContext& c);

        // Section "scenery burn levels" (SBLV,
        // optional): the burn level changes of each scenery object, the BLVL
        // payload with the entry's index in SCNY as the track id. Written
        // only when the recording has one (a build that does not know the
        // section still opens the clip); a track whose index is no scenery
        // entry is dropped by ValidateLoaded.
        constexpr std::uint16_t kSceneryBurnLevelsVersion = 1;
        void WriteSceneryBurnLevels(Writer& w, const SaveContext& c);
        void ReadSceneryBurnLevels(Reader& r, LoadContext& c);
        bool OmitSceneryBurnLevels(const SaveContext& c);

        // Section "object looks" (OLOK, optional, object_look_core.h): the
        // custom texture (document text: txd, p2, p3 hashes) and tint changes
        // of each recorded weapon object and prop (track id: packed id), then
        // of each scenery object (track id: its index in SCNY). Written only
        // when the recording has one (a reader that does not know the section
        // still opens the clip); ValidateLoaded
        // drops a scenery track whose index is no scenery entry and bounds
        // the tint.
        constexpr std::uint16_t kObjectLooksVersion = 1;
        void WriteObjectLooks(Writer& w, const SaveContext& c);
        void ReadObjectLooks(Reader& r, LoadContext& c);
        bool OmitObjectLooks(const SaveContext& c);
        void ResetObjectLooks(LoadContext& c);

        // Section "fire chars" (FREC, optional): the fire record (burnt look)
        // changes of each recorded ped, horse and vehicle (FireCharTrack):
        // time, present, flags, volume count and the kept floats of each
        // volume. Clips without it play without the look (the fire replayed
        // on a puppet still burns it in normal play).
        // Version 2: after the floats of each volume its anchor
        // (component i32, sub u16, flags u8, mask u32, direction 3 x f32);
        // version 1 reads without anchors.
        constexpr std::uint16_t kFireCharsVersion = 2;
        constexpr std::uint16_t kFireCharsMinVersion = 1;
        void WriteFireChars(Writer& w, const SaveContext& c);
        void ReadFireChars(Reader& r, LoadContext& c);

        // Section "appearance changes" (APPC, optional): the metaped layers a
        // recorded ped or horse took mid-clip without a skeleton change
        // (appearance_track.h). Clips without it wear the create and segment
        // layers only.
        constexpr std::uint16_t kAppearanceChangesVersion = 1;
        void WriteAppearanceChanges(Writer& w, const SaveContext& c);
        void ReadAppearanceChanges(Reader& r, LoadContext& c);

        // Section "metaped" (MPED, optional, metaped_state.h): the whole
        // metaped look of each ped and horse (stable expressions, scale,
        // cleanliness, outfit, type) at its entry, at each skeleton change and
        // at each change the poll saw. Clips without it wear the layers only
        // (the puppet's own random face).
        // v2 adds after each state's expressions its shop items (item,
        // wearable state, flags). Written only when a state has one
        // (MetapedSaveVersion), so a clip without them stays v1 for v1
        // readers; v1 is still read (no items).
        constexpr std::uint16_t kMetapedVersion = 2;
        constexpr std::uint16_t kMetapedV1Version = 1;
        std::uint16_t MetapedSaveVersion(const SaveContext& c);
        void WriteMetapeds(Writer& w, const SaveContext& c);
        void ReadMetapeds(Reader& r, LoadContext& c);

        // Section "anim audio" (AANI, optional): the animation
        // audio events of recorded peds and horses (entity_events.h).
        constexpr std::uint16_t kAnimAudioVersion = 1;
        void WriteAnimAudio(Writer& w, const SaveContext& c);
        void ReadAnimAudio(Reader& r, LoadContext& c);

        // Section "registered fx attaches" (RFXA, optional): the attach
        // changes of the registered fx tracks, by track index (the
        // registeredFx section comes first in the file). A clip without it
        // loads with no attach changes.
        constexpr std::uint16_t kRegisteredFxAttachesVersion = 1;
        void WriteRegisteredFxAttaches(Writer& w, const SaveContext& c);
        void ReadRegisteredFxAttaches(Reader& r, LoadContext& c);

        // Section "vehicle looks" (VEHL, optional): cargo prop
        // set, lantern and extras per recorded vehicle. Version 2
        // adds the latest tint and livery (a version 1 payload loads them
        // as kLookUnset: the create packet's values apply).
        constexpr std::uint16_t kVehicleLooksVersion = 2;
        constexpr std::uint16_t kVehicleLooksMinVersion = 1;
        void WriteVehicleLooks(Writer& w, const SaveContext& c);
        void ReadVehicleLooks(Reader& r, LoadContext& c);

        // Section "doors" (DOOR, optional): map doors by hash.
        constexpr std::uint16_t kDoorsVersion = 1;
        void WriteDoors(Writer& w, const SaveContext& c);
        void ReadDoors(Reader& r, LoadContext& c);

        // Section "meleeHits" (MELE). info is a heap pointer (0); its
        // vtable points into the image (RVA). infoHash is the action
        // result's name hash, which playback resolves back to the engine's
        // object (audio_observer.cpp FindActionResult), so the
        // format needs no new field.
        constexpr std::uint16_t kMeleeHitsVersion = 1;
        void WriteMeleeHits(Writer& w, const SaveContext& c);
        void ReadMeleeHits(Reader& r, LoadContext& c);

        // Section "bodyImpacts" (BIMP). definition is a heap pointer (0);
        // its vtable points into the image (RVA).
        constexpr std::uint16_t kBodyImpactsVersion = 1;
        void WriteBodyImpacts(Writer& w, const SaveContext& c);
        void ReadBodyImpacts(Reader& r, LoadContext& c);

        // Section "sounds" (SNDS). globalAudio is a static audio entity in
        // the image: version 2 writes its site id, version 1 its 1491.50 RVA.
        constexpr std::uint16_t kSoundsVersion = 2;
        constexpr std::uint16_t kSoundsMinVersion = 1;
        void WriteSounds(Writer& w, const SaveContext& c);
        void ReadSounds(Reader& r, LoadContext& c);

        // Section "collisionSounds" (CSND).
        constexpr std::uint16_t kCollisionSoundsVersion = 1;
        void WriteCollisionSounds(Writer& w, const SaveContext& c);
        void ReadCollisionSounds(Reader& r, LoadContext& c);

        // Section "entity sounds" (ESND, optional): sounds tied to a
        // recorded object or vehicle (replay_recording.h EntitySound).
        // Version 2: oneShot after the params; version 1 reads as a
        // persistent sound (all it could hold).
        // Version 3: pedAudio after oneShot (ped audio sounds).
        // Version 4: vehicleAudio after pedAudio (train car sounds).
        // Version 5: audioRva and caller as site ids (u64 each); before, 1491.50
        // RVAs.
        constexpr std::uint16_t kEntitySoundsVersion = 5;
        void WriteEntitySounds(Writer& w, const SaveContext& c);
        void ReadEntitySounds(Reader& r, LoadContext& c);

        // Section "trains" (TRNC, optional): the recorded train cars and the
        // head cars' speed (train_consist.h). The cars' world tracks are
        // weapon tracks (WTRK) keyed by their ids; clips without it have no
        // train car.
        constexpr std::uint16_t kTrainsVersion = 1;
        void WriteTrains(Writer& w, const SaveContext& c);
        void ReadTrains(Reader& r, LoadContext& c);

        // Section "sound census" (SCEN, optional): the recording's engine
        // sound counts per owner, ref and caller path per 250 ms window
        // (sound_check_core.h), compared with each playback's. Clips without
        // it have none (the report says so).
        constexpr std::uint16_t kSoundCensusVersion = 1;
        void WriteSoundCensus(Writer& w, const SaveContext& c);
        void ReadSoundCensus(Reader& r, LoadContext& c);

        // Section "general sounds" (GSND, optional): the sounds of
        // recorded vehicles, train cars and objects the census caught and no
        // other track covers (sound_capture.h). The audio base and offset
        // become an engine pointer in playback: only what the recorder makes
        // is accepted (GeneralSoundValid).
        // Version 2: the vtable as a site id (u64), and for an Image base the
        // offset written 0 and the static object as a site id (u64) after the
        // vtable (0 for any other base); version 1 holds 1491.50 RVAs.
        constexpr std::uint16_t kGeneralSoundsVersion = 2;
        constexpr std::uint16_t kGeneralSoundsMinVersion = 1;
        void WriteGeneralSounds(Writer& w, const SaveContext& c);
        void ReadGeneralSounds(Reader& r, LoadContext& c);

        // Section "sound generic" (SGEN, optional): the generic sound effect
        // packets (create, adjust, end; sound_generic_core.h). A clip without
        // it loads with an empty track. Every packet is written with the byte
        // count kCreatePacketBytes, AdjustPacketBytes and kEndPacketBytes say
        // (the budget counts the same bytes).
        // Version 2 adds, after the caller path of each create, the audio
        // entity's base, field, offset and vtable RVA (11 bytes); a version 1
        // payload loads with audioKnown false (the player falls back to the
        // static audio).
        // Version 3 adds the init variables of each create packet (params
        // +0x10, count +0x90): a count byte and that many (hash, value).
        // Version 4 adds the value dwords of the params away from the
        // constructor's (a count byte, then index and raw dword) and the bit
        // 0x10 of the bits byte (the game gave an environment group).
        // Version 5: the family caller and the vtable as site ids (u64 each),
        // and for an Image base the offset written 0 and the static object as
        // a site id (u64) after the vtable (0 for any other base); before,
        // 1491.50 RVAs. The caller path stays RVAs (evidence).
        constexpr std::uint16_t kSoundGenericVersion = 5;
        constexpr std::uint16_t kSoundGenericMinVersion = 1;
        void WriteSoundGeneric(Writer& w, const SaveContext& c);
        void ReadSoundGeneric(Reader& r, LoadContext& c);

        // Section "mix probe" (MIXP, mix_probe_core.h): per owned sound of the recording,
        // its owner, role, kind, ref, distances to the camera and the live
        // player, the engine distance, the ped audio LOD and the player
        // flags; once a second the camera, player and main character
        // positions and whether the game's music played. Evidence only
        // (nothing plays from it). Written only when the probe recorded
        // something (MixProbe=1), so readers that do not know the section
        // still open a clip without it.
        constexpr std::uint16_t kMixProbeVersion = 1;
        void WriteMixProbe(Writer& w, const SaveContext& c);
        void ReadMixProbe(Reader& r, LoadContext& c);
        bool OmitMixProbe(const SaveContext& c);

        // Section "render targets" (RTGT, render_target_core.h): the named render targets
        // (register with its flag and answer, link of a model filed with the
        // target's name, release) and the UI apps open (with entry) and
        // closed, in time order; initial = the state at the recording's start.
        // Optional and written only when there is something.
        constexpr std::uint16_t kRenderTargetsVersion = 1;
        void WriteRenderTargets(Writer& w, const SaveContext& c);
        void ReadRenderTargets(Reader& r, LoadContext& c);
        bool OmitRenderTargets(const SaveContext& c);
        // Section "game listener" (LSTN, game_listener_core.h): the live
        // engine volume listener position
        // (point 0 of the current listener description) about 30 times a
        // second, the number of volume points and whether the engine's own
        // override was on. Optional: omitted when empty.
        // v2: after the v1 samples, one record per sample with the microphone
        // hash, where it came from and up to 4 volume points (position, slot
        // +0x50 weight, slot +0x70). Saved as v2 only when a sample has v2
        // data (as MIXS v3); v1 still reads (no microphone).
        constexpr std::uint16_t kGameListenerVersion = 2;
        void WriteGameListener(Writer& w, const SaveContext& c);
        std::uint16_t GameListenerSaveVersion(const SaveContext& c);
        void ReadGameListener(Reader& r, LoadContext& c);
        bool OmitGameListener(const SaveContext& c);
        // Section "wind audio" (WNDA, wind_replay_core.h): the
        // weather audio's smoothed wind strength (W+0x4F44), the weather of
        // each wind row (W+0x5094/W+0x5098 through the weather table) and
        // whether the simple path ran, 4 times a second. Evidence only: the
        // playback logs the recorded value against the engine's (nothing is
        // applied from it). Optional: omitted when empty; a clip without it
        // plays with rec=none.
        constexpr std::uint16_t kWindAudioVersion = 1;
        void WriteWindAudio(Writer& w, const SaveContext& c);
        void ReadWindAudio(Reader& r, LoadContext& c);
        bool OmitWindAudio(const SaveContext& c);
        // Section "mix scenes" (MIXS, mix_scene_core.h):
        // the mixer scenes of the recording (id, hash, the start's return
        // address, kind, start, stop or open, a conversation scene's
        // participant tags) and each recorded ped's audio mix group changes.
        // Optional and omitted when empty (nothing seen, or an older build that did not record it).
        // v2: the scene variables (time, instance id, name hash, value) after
        // the groups; v1 still reads.
        // v3: a trailing u32 of flags (kTrackPoolScanned: the recording read
        // the scene pool, so its direct scenes, the speech duck among them,
        // are complete); v1 and v2 still read (flags 0).
        // A track without flags (an older build, or a pool that could
        // not be read) is saved as v2, without the trailing field (a newer
        // section version refuses the whole clip in an older reader); only a
        // track with flags needs v3.
        constexpr std::uint16_t kMixScenesVersion = 3;
        void WriteMixScenes(Writer& w, const SaveContext& c);
        std::uint16_t MixScenesSaveVersion(const SaveContext& c);
        void ReadMixScenes(Reader& r, LoadContext& c);
        bool OmitMixScenes(const SaveContext& c);

        // Section "category state" (CATS, category_state_core.h): every static sound category's controller values (hash, volume dB,
        // the two multipliers, pitch, low and high pass) on first sight and
        // on change, after the session's static and map counts. The playback
        // puts them back into the categories (CategoryStateReplay). Optional
        // and omitted when empty: a clip without it plays with the session's
        // categories.
        constexpr std::uint16_t kCategoryStateVersion = 1;
        void WriteCategoryState(Writer& w, const SaveContext& c);
        void ReadCategoryState(Reader& r, LoadContext& c);
        bool OmitCategoryState(const SaveContext& c);

        // Section "entity queries" (EQRY, entity_query_track.h): the engine's answers to the
        // owner queries of the dynamic entity sounds of the recorded owners.
        // The chains once (hash, length, names), then the changes of the
        // answer per owner and chain in time order. Optional.
        constexpr std::uint16_t kEntityQueriesVersion = 1;
        void WriteEntityQueries(Writer& w, const SaveContext& c);
        void ReadEntityQueries(Reader& r, LoadContext& c);

        // Section "foley variants" (FVAR, ped_audio_state.h): the two variant indices the
        // engine drew once for each recorded owner's ped audio (the local
        // player's 0 and 0 included), sorted by owner. Optional: without it
        // only the recorded player's puppet is given the player's indices.
        constexpr std::uint16_t kFoleyVariantsVersion = 1;
        void WriteFoleyVariants(Writer& w, const SaveContext& c);
        void ReadFoleyVariants(Reader& r, LoadContext& c);

        // Section "ped speed" (PSPD, ped_speed_core.h): each
        // recorded ped's and horse's PED_VARIABLES speed (the variable the
        // sounds of that ped read by name), 4 times a second, by owner. The
        // playback writes it into the puppet's block (PedSpeedReplay).
        // Optional and omitted when empty: a clip without it plays with the
        // speed of the puppets' recorded motion.
        constexpr std::uint16_t kPedSpeedVersion = 1;
        void WritePedSpeed(Writer& w, const SaveContext& c);
        void ReadPedSpeed(Reader& r, LoadContext& c);
        bool OmitPedSpeed(const SaveContext& c);

        // Section "shader vars" (SHVR, shader_vars_core.h): each recorded
        // horse's animated shader variables (the muscles under its skin: the
        // values of its creature's shader vars component, track 0x17 and the
        // others the component holds), about 30 times a second, by owner and
        // layout; the values as half floats. The playback writes them into the
        // puppet's component before the shader reads them (MuscleReplay).
        // Optional and omitted when empty: a clip without it plays the
        // puppets' own values (still muscles).
        constexpr std::uint16_t kShaderVarsVersion = 1;
        void WriteShaderVars(Writer& w, const SaveContext& c);
        void ReadShaderVars(Reader& r, LoadContext& c);
        bool OmitShaderVars(const SaveContext& c);

        // Section "foot events" (FSEV, foot_event_core.h): the secondary steps of each recorded ped and horse (the entries
        // the engine put in the foot audio's second queue: type, foot, origin,
        // the foot slot's container and the foot's rise), queued again on the
        // puppet at the same clip time (FootEventReplay). Optional and omitted
        // when empty: a clip without it keeps
        // the puppets' own secondary steps.
        constexpr std::uint16_t kFootEventsVersion = 1;
        void WriteFootEvents(Writer& w, const SaveContext& c);
        void ReadFootEvents(Reader& r, LoadContext& c);
        bool OmitFootEvents(const SaveContext& c);
        // Section "foot movement" (FMOV, foot_movement_core.h): the movement volume result of each
        // recorded ped's and horse's step (owner, time, foot, result), given
        // back to the puppet's step (FootMovementReplay). Optional and omitted
        // when empty.
        constexpr std::uint16_t kFootMovementVersion = 1;
        void WriteFootMovement(Writer& w, const SaveContext& c);
        void ReadFootMovement(Reader& r, LoadContext& c);
        bool OmitFootMovement(const SaveContext& c);
        // Section "weapon fx intents" (WFXI, weapon_fx_intent_core.h): the weapon
        // system's requests for each shot's muzzle flash and shell (shooter
        // and weapon ids, bone, info hashes and range, the vector argument).
        // Optional and omitted when empty (WeaponFxIntent=0 records none).
        constexpr std::uint16_t kWeaponFxIntentsVersion = 1;
        void WriteWeaponFxIntents(Writer& w, const SaveContext& c);
        void ReadWeaponFxIntents(Reader& r, LoadContext& c);
        bool OmitWeaponFxIntents(const SaveContext& c);

        // The fx settings. FXSS: per one-shot event of the fx section
        // (by its index there; FXEV comes first in the file) the instance
        // fields the engine set before the start (entity::FxSettings). RGSS:
        // per registered track (index into RGFX) the settings changes and the
        // stretches without a call (callGaps). Both optional and omitted when
        // empty: a clip without them plays with the engine's settings and
        // gaps derived from the frame times.
        constexpr std::uint16_t kFxSettingsVersion = 1;
        constexpr std::uint16_t kRegisteredFxSettingsVersion = 1;
        void WriteFxSettings(Writer& w, const SaveContext& c);
        void ReadFxSettings(Reader& r, LoadContext& c);
        void ResetFxSettings(LoadContext& c);
        bool OmitFxSettings(const SaveContext& c);
        void WriteRegisteredFxSettings(Writer& w, const SaveContext& c);
        void ReadRegisteredFxSettings(Reader& r, LoadContext& c);
        void ResetRegisteredFxSettings(LoadContext& c);
        bool OmitRegisteredFxSettings(const SaveContext& c);
}
