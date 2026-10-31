// clip_file_world.cpp: the world sections: meta, area, world objects, fragments, scenery, visibility,
// game camera, environment, sky, timecycle, moon, place, world state, abilities, weapon conditions,
// cuts, spans, the vehicles' state and looks, burn levels, object looks, fire chars, appearance
// changes, metapeds and doors.
// Split from clip_file.cpp (moved code); shared internals in clip_file_internal.h.

#include "clip_file_internal.h"

namespace replay::clipfile
{
        std::uint32_t g_buildTimestamp = 0;
        std::uint64_t g_buildImageSize = 0;
        void WriteMeta(Writer& w, const SaveContext&)
        {
            w.Put(g_buildTimestamp);
            w.Put(g_buildImageSize);
        }
        // Read before the other sections (MetaSameBuild): a clip of another
        // build opens, its raw addresses dropped (RvaOfSite, FromRva).
        void ReadMeta(Reader& r, LoadContext&)
        {
            r.Get<std::uint32_t>();
            r.Get<std::uint64_t>();
        }
        void WriteArea(Writer& w, const SaveContext& c)
        {
            w.Bool(c.recording.hasArea);
            w.Array(c.recording.areaMin);
            w.Array(c.recording.areaMax);
        }
        void ReadArea(Reader& r, LoadContext& c)
        {
            c.recording.hasArea = r.Bool();
            r.Array(c.recording.areaMin);
            r.Array(c.recording.areaMax);
        }
        void WriteWorldObjects(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.worldObjects.size());
            for (const WorldObjectOrigin& origin : c.recording.worldObjects)
            {
                w.Id(origin.id);
                w.Put(origin.model);
                w.Array(origin.position);
                w.Array(origin.rotation);
            }
        }
        void ReadWorldObjects(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(4 + 4 + 24);
            for (std::size_t n = 0; n < count; ++n)
            {
                WorldObjectOrigin origin;
                origin.id = r.Id();
                origin.model = r.Get<std::uint32_t>();
                r.Array(origin.position);
                r.Array(origin.rotation);
                c.recording.worldObjects.push_back(origin);
            }
        }
        void WriteFragBreaks(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.fragBreaks.size());
            for (const FragBreak& e : c.recording.fragBreaks)
            {
                w.Put(e.timeMs);
                w.Id(e.id);
                w.Put(e.group);
            }
        }
        void ReadFragBreaks(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(4 + 4 + 2);
            for (std::size_t n = 0; n < count; ++n)
            {
                FragBreak e;
                e.timeMs = r.Get<std::uint32_t>();
                e.id = r.Id();
                e.group = r.Get<std::uint16_t>();
                c.recording.fragBreaks.push_back(e);
            }
        }
        void WriteFragPieces(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.fragPieces.size());
            for (const FragPiece& row : c.recording.fragPieces)
            {
                w.Id(row.id);
                w.Put(row.source);
                w.Values(row.groups);
            }
        }
        void ReadFragPieces(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(4 + 1 + 8 + 2);
            if (count > fragpiece::kMaxRows)
                r.Fail();
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                FragPiece row;
                row.id = r.Id();
                row.source = r.Get<std::uint8_t>();
                r.Values(row.groups);
                if (row.source > fragpiece::kSourceMax || !fragpiece::ValidGroups(row.groups))
                    r.Fail();
                c.recording.fragPieces.push_back(std::move(row));
            }
        }
        void WriteScenery(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.scenery.size());
            for (const SceneryObject& object : c.recording.scenery)
            {
                w.Put(object.model);
                w.Array(object.position);
                w.Array(object.rotation);
                w.Put(object.appearMs);
                w.Put(object.goneMs);
                w.Id(object.prop);
                w.Put(object.owner);
                w.Put(object.mission);
                w.Put(object.visible);
                w.Values(object.visibleFlipsMs);
                w.Put(object.rule); // v3
                w.Put(object.ipl);
            }
        }
        void ReadScenery(Reader& r, LoadContext& c)
        {
            const bool v2 = c.sectionVersion >= 2;
            const bool v3 = c.sectionVersion >= 3;
            const std::size_t count = r.Count(4 + 24 + 4 + 4 + 4 + 1 + 1 + (v2 ? 1 + 8 : 0) + (v3 ? 1 + 4 : 0));
            for (std::size_t n = 0; n < count; ++n)
            {
                SceneryObject object;
                object.model = r.Get<std::uint32_t>();
                r.Array(object.position);
                r.Array(object.rotation);
                object.appearMs = r.Get<std::uint32_t>();
                object.goneMs = r.Get<std::uint32_t>();
                object.prop = r.Id();
                object.owner = r.Get<std::uint8_t>();
                object.mission = r.Get<std::uint8_t>();
                if (v2)
                {
                    object.visible = r.Get<std::uint8_t>() != 0 ? 1 : 0;
                    r.Values(object.visibleFlipsMs);
                }
                if (v3)
                {
                    object.rule = r.Get<std::uint8_t>();
                    object.ipl = r.Get<std::uint32_t>();
                    if (object.rule >= objcheck::kSceneryRuleCount)
                        r.Fail();
                }
                c.recording.scenery.push_back(object);
            }
        }
        void WriteSceneryChildren(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.sceneryChildren.size());
            for (const SceneryChild& child : c.recording.sceneryChildren)
            {
                w.Put(child.model);
                w.Put(child.parent);
                w.Put(child.hasFrame);
                w.Put(child.frameBone);
                w.Array(child.framePosition);
                w.Array(child.frameRotation);
                w.Array(child.relativePosition);
                w.Array(child.relativeRotation);
                w.Array(child.position);
                w.Array(child.rotation);
                w.Put(child.appearMs);
                w.Put(child.goneMs);
                w.Put(child.visible);
                w.Values(child.visibleFlipsMs);
            }
        }
        void ReadSceneryChildren(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(4 + 4 + 1 + 2 + 12 + 16 + 12 + 16 + 12 + 12 + 4 + 4 + 1 + 8);
            if (count > objcheck::kMaxSceneryChildren)
                r.Fail();
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                SceneryChild child;
                child.model = r.Get<std::uint32_t>();
                child.parent = r.Get<std::uint32_t>();
                child.hasFrame = r.Get<std::uint8_t>() != 0 ? 1 : 0;
                child.frameBone = r.Get<std::int16_t>();
                r.Array(child.framePosition);
                r.Array(child.frameRotation);
                r.Array(child.relativePosition);
                r.Array(child.relativeRotation);
                r.Array(child.position);
                r.Array(child.rotation);
                child.appearMs = r.Get<std::uint32_t>();
                child.goneMs = r.Get<std::uint32_t>();
                child.visible = r.Get<std::uint8_t>() != 0 ? 1 : 0;
                r.Values(child.visibleFlipsMs);
                // The frame plausible when kept (a parent that is no scenery
                // entry, SCNY left out, is dropped by ValidateLoaded).
                if (child.visibleFlipsMs.size() > objcheck::kMaxVisibilityFlips ||
                    !std::is_sorted(child.visibleFlipsMs.begin(), child.visibleFlipsMs.end()) ||
                    (child.hasFrame && !objcheck::FramePlausible(child.frameBone, child.framePosition, child.frameRotation)))
                    r.Fail();
                c.recording.sceneryChildren.push_back(std::move(child));
            }
        }
        bool OmitSceneryChildren(const SaveContext& c) { return c.recording.sceneryChildren.empty(); }
        void WriteSceneryHats(Writer& w, const SaveContext& c)
        {
            std::size_t count = 0;
            for (const SceneryObject& object : c.recording.scenery)
                count += object.hatLook.empty() ? 0u : 1u;
            w.Count(count);
            for (std::size_t index = 0; index < c.recording.scenery.size(); ++index)
            {
                const std::vector<entity::MetaPedAsset>& look = c.recording.scenery[index].hatLook;
                if (look.empty())
                    continue;
                w.Put(static_cast<std::uint32_t>(index));
                w.Count(look.size());
                for (const entity::MetaPedAsset& asset : look)
                {
                    w.Put(asset.drawable);
                    w.Put(asset.albedo);
                    w.Put(asset.normal);
                    w.Put(asset.material);
                    w.Put(asset.palette);
                    w.Put(asset.tint0);
                    w.Put(asset.tint1);
                    w.Put(asset.tint2);
                }
            }
        }
        void ReadSceneryHats(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(4 + 8 + 32);
            if (count > objcheck::kMaxScenery)
                r.Fail();
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                const std::uint32_t index = r.Get<std::uint32_t>();
                std::vector<entity::MetaPedAsset> look(r.Count(32));
                if (look.empty() || look.size() > hatlook::kMaxTags)
                    r.Fail();
                for (entity::MetaPedAsset& asset : look)
                {
                    asset.drawable = r.Get<std::uint32_t>();
                    asset.albedo = r.Get<std::uint32_t>();
                    asset.normal = r.Get<std::uint32_t>();
                    asset.material = r.Get<std::uint32_t>();
                    asset.palette = r.Get<std::uint32_t>();
                    asset.tint0 = r.Get<std::int32_t>();
                    asset.tint1 = r.Get<std::int32_t>();
                    asset.tint2 = r.Get<std::int32_t>();
                }
                if (r.Ok() && index < c.recording.scenery.size())
                    c.recording.scenery[index].hatLook = std::move(look);
            }
        }
        bool OmitSceneryHats(const SaveContext& c)
        {
            return std::none_of(c.recording.scenery.begin(), c.recording.scenery.end(),
                [](const SceneryObject& object) { return !object.hatLook.empty(); });
        }
        void ResetSceneryHats(LoadContext& c)
        {
            for (SceneryObject& object : c.recording.scenery)
                object.hatLook.clear();
        }
        void WritePedVisibility(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.pedVisibility.size());
            for (const PedVisibility& row : c.recording.pedVisibility)
            {
                w.Id(row.id);
                w.Put(row.visible);
                w.Values(row.flipsMs);
            }
        }
        void ReadPedVisibility(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(4 + 1 + 8);
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                PedVisibility row;
                row.id = r.Id();
                row.visible = r.Get<std::uint8_t>() != 0 ? 1 : 0;
                r.Values(row.flipsMs);
                if (row.flipsMs.size() > objcheck::kMaxVisibilityFlips ||
                    !std::is_sorted(row.flipsMs.begin(), row.flipsMs.end()))
                    r.Fail();
                c.recording.pedVisibility.push_back(std::move(row));
            }
        }
        void WriteGameCamera(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.gameCamera.size());
            for (const camera::GameSample& e : c.recording.gameCamera)
            {
                w.Put(e.timeMs);
                w.Put(e.position.x);
                w.Put(e.position.y);
                w.Put(e.position.z);
                w.Put(e.rotation.x);
                w.Put(e.rotation.y);
                w.Put(e.rotation.z);
                w.Put(e.fov);
            }
        }
        void ReadGameCamera(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(4 + 7 * 4);
            std::uint32_t lastMs = 0;
            for (std::size_t n = 0; n < count; ++n)
            {
                camera::GameSample e{};
                e.timeMs = r.Get<std::uint32_t>();
                e.position.x = r.Get<float>();
                e.position.y = r.Get<float>();
                e.position.z = r.Get<float>();
                e.rotation.x = r.Get<float>();
                e.rotation.y = r.Get<float>();
                e.rotation.z = r.Get<float>();
                e.fov = r.Get<float>();
                // EvaluateGame expects the samples sorted by time.
                if (e.timeMs < lastMs)
                    r.Fail();
                lastMs = e.timeMs;
                c.recording.gameCamera.push_back(e);
            }
        }
        void WriteEnvironment(Writer& w, const SaveContext& c)
        {
            const Environment& e = c.recording.environment;
            w.Bool(e.recorded);
            w.Put(e.clockStartS);
            w.Put(e.clockEndS);
            w.Put(e.weatherFrom);
            w.Put(e.weatherTo);
            w.Put(e.weatherMix);
        }
        void ReadEnvironment(Reader& r, LoadContext& c)
        {
            Environment& e = c.recording.environment;
            e.recorded = r.Bool();
            e.clockStartS = r.Get<std::uint32_t>();
            e.clockEndS = r.Get<std::uint32_t>();
            e.weatherFrom = r.Get<std::uint32_t>();
            e.weatherTo = r.Get<std::uint32_t>();
            e.weatherMix = r.Get<float>();
            if (e.clockStartS >= 86400u || e.clockEndS >= 86400u)
                r.Fail();
        }

    namespace
    {
        constexpr std::uint8_t kTransitionKnownBit = 1;
        constexpr std::uint8_t kTransitionFlagBit = 2;
    }
        void WriteEnvironmentTrack(Writer& w, const SaveContext& c)
        {
            const std::vector<EnvironmentSample>& track = c.recording.environment.track;
            w.Count(track.size());
            for (const EnvironmentSample& s : track)
            {
                w.Put(s.timeMs);
                w.Put(s.weatherFrom);
                w.Put(s.weatherTo);
                w.Put(s.weatherMix);
                w.Put(s.clockS);
                w.Put(s.windSpeed);
                w.Put(s.windDirection);
                w.Put(s.rain);
                w.Put(s.snow);
                w.Put(s.transitionTo); // v2
                w.Put(s.transitionTotalMs);
                w.Put(s.transitionLeftMs);
                const std::uint8_t flags = static_cast<std::uint8_t>((s.transitionKnown ? kTransitionKnownBit : 0) |
                    (s.transitionFlag ? kTransitionFlagBit : 0));
                w.Put(flags);
            }
        }
        void ReadEnvironmentTrack(Reader& r, LoadContext& c)
        {
            const bool v2 = c.sectionVersion >= 2;
            const std::size_t count = r.Count(9 * 4 + (v2 ? 3 * 4 + 1 : 0));
            std::vector<EnvironmentSample>& track = c.recording.environment.track;
            std::uint32_t lastMs = 0;
            for (std::size_t n = 0; n < count; ++n)
            {
                EnvironmentSample s;
                s.timeMs = r.Get<std::uint32_t>();
                s.weatherFrom = r.Get<std::uint32_t>();
                s.weatherTo = r.Get<std::uint32_t>();
                s.weatherMix = r.Get<float>();
                s.clockS = r.Get<std::uint32_t>();
                s.windSpeed = r.Get<float>();
                s.windDirection = r.Get<float>();
                s.rain = r.Get<float>();
                s.snow = r.Get<float>();
                if (v2)
                {
                    s.transitionTo = r.Get<std::uint32_t>();
                    s.transitionTotalMs = r.Get<float>();
                    s.transitionLeftMs = r.Get<float>();
                    const std::uint8_t flags = r.Get<std::uint8_t>();
                    if (flags & ~(kTransitionKnownBit | kTransitionFlagBit))
                        r.Fail();
                    s.transitionKnown = (flags & kTransitionKnownBit) != 0;
                    s.transitionFlag = (flags & kTransitionFlagBit) ? 1 : 0;
                }
                if (!envtrack::Valid(s) || s.timeMs < lastMs)
                    r.Fail();
                lastMs = s.timeMs;
                track.push_back(s);
            }
        }
        void WriteSkyTrack(Writer& w, const SaveContext& c)
        {
            const Environment& e = c.recording.environment;
            for (float period : e.skyPeriods.value)
                w.Put(period);
            w.Count(e.sky.size());
            for (const SkySample& s : e.sky)
            {
                w.Put(s.timeMs);
                for (float v : s.value)
                    w.Put(v);
                for (float v : s.unpinned)
                    w.Put(v);
            }
        }
        void ReadSkyTrack(Reader& r, LoadContext& c)
        {
            Environment& e = c.recording.environment;
            for (float& period : e.skyPeriods.value)
                period = r.Get<float>();
            if (!skytrack::Valid(e.skyPeriods))
                r.Fail();
            const std::size_t count = r.Count(4 + (sky::kChannels + 3) * 4);
            std::uint32_t lastMs = 0;
            for (std::size_t n = 0; n < count; ++n)
            {
                SkySample s;
                s.timeMs = r.Get<std::uint32_t>();
                for (float& v : s.value)
                    v = r.Get<float>();
                for (float& v : s.unpinned)
                    v = r.Get<float>();
                if (!skytrack::Finite(s) || s.timeMs < lastMs)
                    r.Fail();
                lastMs = s.timeMs;
                e.sky.push_back(s);
            }
        }

    namespace
    {
        constexpr std::size_t kTimecycleSampleMinBytes = 4 * 8 + 2;
    }
        void WriteTimecycle(Writer& w, const SaveContext& c)
        {
            const std::vector<TimecycleSample>& track = c.recording.environment.timecycle;
            w.Count(track.size());
            for (const TimecycleSample& s : track)
            {
                w.Put(s.timeMs);
                w.Put(s.modifierKey);
                w.Put(s.modifierIndex);
                w.Put(s.strength);
                w.Put(s.transitionKey);
                w.Put(s.transitionIndex);
                w.Put(s.progress);
                w.Put(s.speed);
                w.Put(s.fadeOut);
                w.Put(s.variationCount);
                for (std::size_t n = 0; n < s.variationCount; ++n)
                {
                    w.Put(s.variations[n].type);
                    w.Put(s.variations[n].variation);
                }
            }
        }
        void ReadTimecycle(Reader& r, LoadContext& c)
        {
            std::vector<TimecycleSample>& track = c.recording.environment.timecycle;
            const std::size_t count = r.Count(kTimecycleSampleMinBytes);
            std::uint32_t lastMs = 0;
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                TimecycleSample s;
                s.timeMs = r.Get<std::uint32_t>();
                s.modifierKey = r.Get<std::uint32_t>();
                s.modifierIndex = r.Get<std::int32_t>();
                s.strength = r.Get<float>();
                s.transitionKey = r.Get<std::uint32_t>();
                s.transitionIndex = r.Get<std::int32_t>();
                s.progress = r.Get<float>();
                s.speed = r.Get<float>();
                s.fadeOut = r.Get<std::uint8_t>() != 0 ? 1 : 0;
                s.variationCount = r.Get<std::uint8_t>();
                if (s.variationCount > tctrack::kMaxVariations)
                {
                    r.Fail();
                    return;
                }
                for (std::size_t v = 0; v < s.variationCount; ++v)
                {
                    s.variations[v].type = r.Get<std::uint32_t>();
                    s.variations[v].variation = r.Get<std::uint32_t>();
                }
                if (!tctrack::Valid(s) || s.timeMs < lastMs)
                    r.Fail();
                lastMs = s.timeMs;
                track.push_back(s);
            }
        }

    namespace
    {
        constexpr std::size_t kMoonSampleBytes = 4 + 2 * 8 + 6 * 4 + 2 + 1 + 1;
    }
        void WriteMoon(Writer& w, const SaveContext& c)
        {
            const std::vector<MoonSample>& track = c.recording.environment.moon;
            w.Count(track.size());
            for (const MoonSample& s : track)
            {
                w.Put(s.timeMs);
                w.Put(s.angle);
                w.Put(s.orbit);
                for (float v : s.sun)
                    w.Put(v);
                for (float v : s.moon)
                    w.Put(v);
                w.Put(s.year);
                w.Put(s.month);
                w.Put(s.day);
            }
        }
        void ReadMoon(Reader& r, LoadContext& c)
        {
            std::vector<MoonSample>& track = c.recording.environment.moon;
            const std::size_t count = r.Count(kMoonSampleBytes);
            std::uint32_t lastMs = 0;
            for (std::size_t n = 0; n < count && r.Ok(); ++n)
            {
                MoonSample s;
                s.timeMs = r.Get<std::uint32_t>();
                s.angle = r.Get<double>();
                s.orbit = r.Get<double>();
                for (float& v : s.sun)
                    v = r.Get<float>();
                for (float& v : s.moon)
                    v = r.Get<float>();
                s.year = r.Get<std::uint16_t>();
                s.month = r.Get<std::uint8_t>();
                s.day = r.Get<std::uint8_t>();
                if (!moontrack::Valid(s) || s.timeMs < lastMs)
                {
                    r.Fail();
                    return;
                }
                lastMs = s.timeMs;
                track.push_back(s);
            }
        }
        void WriteEnvironmentEdit(Writer& w, const SaveContext& c)
        {
            std::uint8_t bytes[envoverride::kBytes];
            envoverride::Pack(c.recording.environment.edit, bytes);
            for (std::uint8_t b : bytes)
                w.Put(b);
        }
        void ReadEnvironmentEdit(Reader& r, LoadContext& c)
        {
            const std::size_t size = c.sectionVersion >= 2 ? envoverride::kBytes : envoverride::kBytesV1;
            std::uint8_t bytes[envoverride::kBytes]{};
            for (std::size_t n = 0; n < size; ++n)
                bytes[n] = r.Get<std::uint8_t>();
            if (!envoverride::Unpack(bytes, size, c.recording.environment.edit))
                r.Fail();
        }
        void WritePlace(Writer& w, const SaveContext& c)
        {
            const Place& p = c.recording.place;
            w.Bool(p.recorded);
            w.Put(p.zoneTown);
            w.Put(p.zoneDistrict);
            w.Put(p.zoneState);
            w.Put(p.playerModel);
            w.Array(p.town);
            w.Array(p.district);
            w.Array(p.state);
            w.Array(p.character);
            w.Array(p.weather);
        }
        void ReadPlace(Reader& r, LoadContext& c)
        {
            Place& p = c.recording.place;
            p.recorded = r.Bool();
            p.zoneTown = r.Get<std::uint32_t>();
            p.zoneDistrict = r.Get<std::uint32_t>();
            p.zoneState = r.Get<std::uint32_t>();
            p.playerModel = r.Get<std::uint32_t>();
            r.Array(p.town);
            r.Array(p.district);
            r.Array(p.state);
            r.Array(p.character);
            r.Array(p.weather);
            // Text fields always end in a terminator.
            p.town[sizeof(p.town) - 1] = p.district[sizeof(p.district) - 1] = p.state[sizeof(p.state) - 1] = '\0';
            p.character[sizeof(p.character) - 1] = p.weather[sizeof(p.weather) - 1] = '\0';
        }
        void WriteWorldInfo(Writer& w, const SaveContext& c)
        {
            const WorldState& world = c.recording.world;
            w.Bool(world.recorded);
            w.Put(world.camp);
            w.Put(world.chapter);
            w.Put(world.flags);
            w.Put(world::Fingerprint(world));
            w.Put(static_cast<std::uint32_t>(world::IplsOnAt(world, 0).size()));
            w.Put(world.candidateListHash);
            w.Put(world.candidateCount);
        }
        void ReadWorldInfo(Reader& r, LoadContext& c)
        {
            WorldState& world = c.recording.world;
            world.recorded = r.Bool();
            world.camp = r.Get<std::uint8_t>();
            world.chapter = r.Get<std::uint8_t>();
            world.flags = r.Get<std::uint8_t>();
            r.Get<std::uint32_t>(); // fingerprint: derived from the WRLD events
            r.Get<std::uint32_t>(); // active count: likewise
            world.candidateListHash = r.Get<std::uint32_t>();
            world.candidateCount = r.Get<std::uint32_t>();
            if ((world.camp > 8 && world.camp != world::kCampUnknown) || world.chapter > world::kChapterEpilogue2)
                r.Fail();
        }
        void WriteWorld(Writer& w, const SaveContext& c)
        {
            const WorldState& world = c.recording.world;
            w.Count(world.ipls.size());
            for (const IplChange& e : world.ipls)
            {
                w.Put(e.timeMs);
                w.Put(e.hash);
                w.Bool(e.active);
            }
            w.Count(world.spheres.size());
            for (const IplSphere& e : world.spheres)
            {
                w.Put(e.hash);
                w.Array(e.center);
                w.Put(e.radius);
            }
            w.Count(world.interiors.size());
            for (const WorldInterior& e : world.interiors)
            {
                w.Put(e.nameHash);
                w.Array(e.position);
            }
            w.Count(world.entitySets.size());
            for (const EntitySetChange& e : world.entitySets)
            {
                w.Put(e.timeMs);
                w.Put(e.interior);
                w.Bool(e.active);
                const std::size_t length = strnlen(e.name, sizeof(e.name) - 1);
                w.Put(static_cast<std::uint8_t>(length));
                w.Raw(reinterpret_cast<const std::uint8_t*>(e.name), length);
            }
        }
        void ReadWorld(Reader& r, LoadContext& c)
        {
            WorldState& world = c.recording.world;
            world.ipls.resize(r.Count(9));
            std::uint32_t lastMs = 0;
            for (IplChange& e : world.ipls)
            {
                e.timeMs = r.Get<std::uint32_t>();
                e.hash = r.Get<std::uint32_t>();
                e.active = r.Bool();
                if (e.timeMs < lastMs)
                    r.Fail();
                lastMs = e.timeMs;
            }
            world.spheres.resize(r.Count(20));
            std::uint32_t lastHash = 0;
            for (std::size_t n = 0; n < world.spheres.size(); ++n)
            {
                IplSphere& e = world.spheres[n];
                e.hash = r.Get<std::uint32_t>();
                r.Array(e.center);
                e.radius = r.Get<float>();
                if (n > 0 && e.hash <= lastHash)
                    r.Fail(); // FindSphere searches by hash
                lastHash = e.hash;
            }
            world.interiors.resize(r.Count(16));
            for (WorldInterior& e : world.interiors)
            {
                e.nameHash = r.Get<std::uint32_t>();
                r.Array(e.position);
            }
            world.entitySets.resize(r.Count(8));
            lastMs = 0;
            for (EntitySetChange& e : world.entitySets)
            {
                e.timeMs = r.Get<std::uint32_t>();
                e.interior = r.Get<std::uint16_t>();
                e.active = r.Bool();
                const std::uint8_t length = r.Get<std::uint8_t>();
                if (length >= sizeof(e.name) || e.interior >= world.interiors.size() || e.timeMs < lastMs)
                {
                    r.Fail();
                    return;
                }
                r.Raw(reinterpret_cast<std::uint8_t*>(e.name), length);
                e.name[length] = '\0';
                lastMs = e.timeMs;
            }
        }
        void WriteAbilities(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.abilities.size());
            for (const AbilitySpan& span : c.recording.abilities)
            {
                w.Put(span.startMs);
                w.Put(span.endMs);
                w.Enum(span.kind);
            }
        }
        void ReadAbilities(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(9);
            for (std::size_t n = 0; n < count; ++n)
            {
                AbilitySpan span;
                span.startMs = r.Get<std::uint32_t>();
                span.endMs = r.Get<std::uint32_t>();
                span.kind = r.Enum<AbilityKind>(static_cast<std::uint32_t>(AbilityKind::EagleEye));
                if (span.endMs < span.startMs)
                    r.Fail();
                c.recording.abilities.push_back(span);
            }
        }
        void WriteWeaponConditions(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.weaponConditions.size());
            for (const WeaponConditionTrack& track : c.recording.weaponConditions)
            {
                w.Put(track.recordId);
                w.Count(track.states.size());
                for (const auto& [timeMs, state] : track.states)
                {
                    w.Put(timeMs);
                    w.Put(state.dirt);
                    w.Put(state.soot);
                    w.Put(state.damage);
                    w.Put(state.degradation);
                    w.Put(state.permanent);
                    w.Put(state.wearE4);
                    w.Put(state.flags);
                    w.Values(state.components);
                }
            }
        }
        void ReadWeaponConditions(Reader& r, LoadContext& c)
        {
            const bool look = c.sectionVersion >= 2;
            c.recording.weaponConditions.resize(r.Count(8));
            for (WeaponConditionTrack& track : c.recording.weaponConditions)
            {
                track.recordId = r.Get<std::uint32_t>();
                track.states.resize(r.Count(look ? 33 : 20));
                std::uint32_t lastMs = 0;
                for (auto& [timeMs, state] : track.states)
                {
                    timeMs = r.Get<std::uint32_t>();
                    state.dirt = r.Get<float>();
                    state.soot = r.Get<float>();
                    state.damage = r.Get<float>();
                    state.degradation = r.Get<float>();
                    if (look)
                    {
                        state.permanent = r.Get<float>();
                        state.wearE4 = r.Get<float>();
                        state.flags = r.Get<std::uint8_t>();
                        r.Values(state.components);
                        if (state.components.size() > weaponlook::kComponentsMax)
                            r.Fail();
                    }
                    if (timeMs < lastMs)
                        r.Fail();
                    lastMs = timeMs;
                }
            }
        }
        void WriteCuts(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.cuts.size());
            for (const std::uint32_t cut : c.recording.cuts)
                w.Put(cut);
        }
        void ReadCuts(Reader& r, LoadContext& c)
        {
            c.recording.cuts.resize(r.Count(4));
            std::uint32_t last = 0;
            for (std::size_t n = 0; n < c.recording.cuts.size(); ++n)
            {
                const std::uint32_t cut = r.Get<std::uint32_t>();
                if (n > 0 && cut <= last)
                    r.Fail();
                c.recording.cuts[n] = last = cut;
            }
        }
        void WriteSpans(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.spans.size());
            for (const gate::Span& span : c.recording.spans)
            {
                w.Put(span.startMs);
                w.Put(span.endMs);
                w.Enum(span.kind);
            }
        }
        void ReadSpans(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(9);
            for (std::size_t n = 0; n < count; ++n)
            {
                gate::Span span;
                span.startMs = r.Get<std::uint32_t>();
                span.endMs = r.Get<std::uint32_t>();
                span.kind = r.Enum<gate::SpanKind>(static_cast<std::uint32_t>(gate::SpanKind::ControlsOff));
                if (span.endMs < span.startMs)
                    r.Fail();
                c.recording.spans.push_back(span);
            }
        }
        void WriteScorched(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.scorched.size());
            for (const ScorchTrack& track : c.recording.scorched)
            {
                w.Put(track.id);
                w.Count(track.states.size());
                for (const auto& [timeMs, on] : track.states)
                {
                    w.Put(timeMs);
                    w.Put(static_cast<std::uint8_t>(on ? 1 : 0));
                }
            }
        }
        void ReadScorched(Reader& r, LoadContext& c)
        {
            c.recording.scorched.resize(r.Count(8));
            for (ScorchTrack& track : c.recording.scorched)
            {
                track.id = r.Get<std::uint32_t>();
                track.states.resize(r.Count(5));
                std::uint32_t lastMs = 0;
                for (auto& [timeMs, on] : track.states)
                {
                    timeMs = r.Get<std::uint32_t>();
                    const std::uint8_t value = r.Get<std::uint8_t>();
                    if (value > 1 || timeMs < lastMs)
                        r.Fail();
                    on = value != 0;
                    lastMs = timeMs;
                }
            }
        }
        void WriteVehicleWindows(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.vehicleWindows.size());
            for (const WindowTrack& track : c.recording.vehicleWindows)
            {
                w.Put(track.id);
                w.Count(track.states.size());
                for (const auto& [timeMs, mask] : track.states)
                {
                    w.Put(timeMs);
                    w.Put(mask);
                }
            }
        }
        void ReadVehicleWindows(Reader& r, LoadContext& c)
        {
            c.recording.vehicleWindows.resize(r.Count(8));
            for (WindowTrack& track : c.recording.vehicleWindows)
            {
                track.id = r.Get<std::uint32_t>();
                track.states.resize(r.Count(5));
                std::uint32_t lastMs = 0;
                for (auto& [timeMs, mask] : track.states)
                {
                    timeMs = r.Get<std::uint32_t>();
                    mask = r.Get<std::uint8_t>();
                    if (mask >= (1u << kVehicleWindows) || timeMs < lastMs)
                        r.Fail();
                    lastMs = timeMs;
                }
            }
        }
        void WriteVehicleBurnt(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.vehicleBurnt.size());
            for (const ScorchTrack& track : c.recording.vehicleBurnt)
            {
                w.Put(track.id);
                w.Count(track.states.size());
                for (const auto& [timeMs, on] : track.states)
                {
                    w.Put(timeMs);
                    w.Put(static_cast<std::uint8_t>(on ? 1 : 0));
                }
            }
        }
        void ReadVehicleBurnt(Reader& r, LoadContext& c)
        {
            c.recording.vehicleBurnt.resize(r.Count(8));
            for (ScorchTrack& track : c.recording.vehicleBurnt)
            {
                track.id = r.Get<std::uint32_t>();
                track.states.resize(r.Count(5));
                std::uint32_t lastMs = 0;
                for (auto& [timeMs, on] : track.states)
                {
                    timeMs = r.Get<std::uint32_t>();
                    const std::uint8_t value = r.Get<std::uint8_t>();
                    if (value > 1 || timeMs < lastMs)
                        r.Fail();
                    on = value != 0;
                    lastMs = timeMs;
                }
            }
        }
        void WriteVehicleParts(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.vehicleParts.size());
            for (const VehiclePartTrack& track : c.recording.vehicleParts)
            {
                w.Put(track.id);
                w.Count(track.states.size());
                for (const auto& [timeMs, mask] : track.states)
                {
                    w.Put(timeMs);
                    w.Put(mask);
                }
            }
        }
        void ReadVehicleParts(Reader& r, LoadContext& c)
        {
            c.recording.vehicleParts.resize(r.Count(8));
            for (VehiclePartTrack& track : c.recording.vehicleParts)
            {
                track.id = r.Get<std::uint32_t>();
                track.states.resize(r.Count(6));
                std::uint32_t lastMs = 0;
                for (auto& [timeMs, mask] : track.states)
                {
                    timeMs = r.Get<std::uint32_t>();
                    mask = r.Get<std::uint16_t>();
                    if (timeMs < lastMs)
                        r.Fail();
                    lastMs = timeMs;
                }
            }
        }
        void WriteVehicleDoors(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.vehicleDoors.size());
            for (const vehdoors::DoorTrack& track : c.recording.vehicleDoors)
            {
                w.Put(track.id);
                w.Count(track.samples.size());
                for (const vehdoors::DoorSample& sample : track.samples)
                {
                    w.Put(sample.timeMs);
                    w.Put(sample.door);
                    w.Put(sample.ratio);
                }
            }
        }
        void ReadVehicleDoors(Reader& r, LoadContext& c)
        {
            c.recording.vehicleDoors.resize(r.Count(8));
            for (vehdoors::DoorTrack& track : c.recording.vehicleDoors)
            {
                track.id = r.Get<std::uint32_t>();
                track.samples.resize(r.Count(9));
                std::uint32_t lastMs = 0;
                for (vehdoors::DoorSample& sample : track.samples)
                {
                    sample.timeMs = r.Get<std::uint32_t>();
                    sample.door = r.Get<std::uint8_t>();
                    sample.ratio = r.Get<float>();
                    if (sample.timeMs < lastMs || sample.door >= vehdoors::kChannels || !(sample.ratio >= 0.0f) ||
                        sample.ratio > 1.0f)
                        r.Fail();
                    lastMs = sample.timeMs;
                }
            }
        }
        bool OmitVehicleDoors(const SaveContext& c)
        {
            return c.recording.vehicleDoors.empty();
        }

    namespace
    {
        void WriteBurnTracks(Writer& w, const std::vector<BurnLevelTrack>& tracks)
        {
            w.Count(tracks.size());
            for (const BurnLevelTrack& track : tracks)
            {
                w.Put(track.id);
                w.Count(track.states.size());
                for (const auto& [timeMs, level] : track.states)
                {
                    w.Put(timeMs);
                    w.Put(level);
                }
            }
        }
        void ReadBurnTracks(Reader& r, std::vector<BurnLevelTrack>& tracks)
        {
            tracks.resize(r.Count(8));
            for (BurnLevelTrack& track : tracks)
            {
                track.id = r.Get<std::uint32_t>();
                track.states.resize(r.Count(8));
                std::uint32_t lastMs = 0;
                for (auto& [timeMs, level] : track.states)
                {
                    timeMs = r.Get<std::uint32_t>();
                    level = r.Get<float>();
                    if (timeMs < lastMs || !std::isfinite(level))
                        r.Fail();
                    lastMs = timeMs;
                }
            }
        }
    }
        void WriteBurnLevels(Writer& w, const SaveContext& c) { WriteBurnTracks(w, c.recording.burnLevels); }
        void ReadBurnLevels(Reader& r, LoadContext& c) { ReadBurnTracks(r, c.recording.burnLevels); }
        void WriteSceneryBurnLevels(Writer& w, const SaveContext& c) { WriteBurnTracks(w, c.recording.sceneryBurnLevels); }
        void ReadSceneryBurnLevels(Reader& r, LoadContext& c) { ReadBurnTracks(r, c.recording.sceneryBurnLevels); }
        bool OmitSceneryBurnLevels(const SaveContext& c) { return c.recording.sceneryBurnLevels.empty(); }

    namespace
    {
        void WriteObjectLookTracks(Writer& w, const std::vector<ObjectLookTrack>& tracks)
        {
            w.Count(tracks.size());
            for (const ObjectLookTrack& track : tracks)
            {
                w.Put(track.id);
                w.Count(track.states.size());
                for (const auto& [timeMs, look] : track.states)
                {
                    w.Put(timeMs);
                    w.Put(look.txd);
                    w.Put(look.p2);
                    w.Put(look.p3);
                    w.Put(look.tint);
                }
            }
        }
        void ReadObjectLookTracks(Reader& r, std::vector<ObjectLookTrack>& tracks)
        {
            tracks.resize(r.Count(8));
            for (ObjectLookTrack& track : tracks)
            {
                track.id = r.Get<std::uint32_t>();
                track.states.resize(r.Count(17));
                std::uint32_t lastMs = 0;
                for (auto& [timeMs, look] : track.states)
                {
                    timeMs = r.Get<std::uint32_t>();
                    look.txd = r.Get<std::uint32_t>();
                    look.p2 = r.Get<std::uint32_t>();
                    look.p3 = r.Get<std::uint32_t>();
                    look.tint = r.Get<std::uint8_t>();
                    if (timeMs < lastMs)
                        r.Fail();
                    lastMs = timeMs;
                }
            }
        }
    }
        void WriteObjectLooks(Writer& w, const SaveContext& c)
        {
            WriteObjectLookTracks(w, c.recording.objectLooks);
            WriteObjectLookTracks(w, c.recording.sceneryObjectLooks);
        }
        void ReadObjectLooks(Reader& r, LoadContext& c)
        {
            ReadObjectLookTracks(r, c.recording.objectLooks);
            ReadObjectLookTracks(r, c.recording.sceneryObjectLooks);
        }
        bool OmitObjectLooks(const SaveContext& c)
        {
            return c.recording.objectLooks.empty() && c.recording.sceneryObjectLooks.empty();
        }
        void ResetObjectLooks(LoadContext& c)
        {
            c.recording.objectLooks.clear();
            c.recording.sceneryObjectLooks.clear();
        }
        void WriteFireChars(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.fireChars.size());
            for (const FireCharTrack& track : c.recording.fireChars)
            {
                w.Put(track.id);
                w.Count(track.states.size());
                for (const auto& [timeMs, state] : track.states)
                {
                    w.Put(timeMs);
                    w.Put(static_cast<std::uint8_t>(state.present ? 1 : 0));
                    w.Put(state.flags);
                    w.Count(state.volumes.size());
                    for (std::size_t n = 0; n < state.volumes.size(); ++n)
                    {
                        for (float value : state.volumes[n])
                            w.Put(value);
                        const FireCharAnchor anchor = n < state.anchors.size() ? state.anchors[n] : FireCharAnchor{};
                        w.Put(anchor.component);
                        w.Put(anchor.sub);
                        w.Put(anchor.flags);
                        w.Put(anchor.mask);
                        for (float value : anchor.dir)
                            w.Put(value);
                    }
                }
            }
        }
        void ReadFireChars(Reader& r, LoadContext& c)
        {
            c.recording.fireChars.resize(r.Count(8));
            for (FireCharTrack& track : c.recording.fireChars)
            {
                track.id = r.Get<std::uint32_t>();
                track.states.resize(r.Count(6));
                std::uint32_t lastMs = 0;
                for (auto& [timeMs, state] : track.states)
                {
                    timeMs = r.Get<std::uint32_t>();
                    state.present = r.Get<std::uint8_t>() != 0;
                    state.flags = r.Get<std::uint8_t>();
                    const bool anchored = c.sectionVersion >= 2;
                    state.volumes.resize(r.Count(kFireCharFloats * 4 + (anchored ? 23 : 0)));
                    if (state.volumes.size() > 64)
                        r.Fail();
                    if (anchored)
                        state.anchors.resize(state.volumes.size());
                    for (std::size_t n = 0; n < state.volumes.size(); ++n)
                    {
                        for (float& value : state.volumes[n])
                        {
                            value = r.Get<float>();
                            if (!std::isfinite(value))
                                r.Fail();
                        }
                        if (!anchored)
                            continue;
                        FireCharAnchor& anchor = state.anchors[n];
                        anchor.component = r.Get<std::int32_t>();
                        anchor.sub = r.Get<std::uint16_t>();
                        anchor.flags = r.Get<std::uint8_t>();
                        anchor.mask = r.Get<std::uint32_t>();
                        for (float& value : anchor.dir)
                        {
                            value = r.Get<float>();
                            if (!std::isfinite(value))
                                r.Fail();
                        }
                    }
                    if (timeMs < lastMs)
                        r.Fail();
                    lastMs = timeMs;
                }
            }
        }
        void WriteAppearanceChanges(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.appearances.size());
            for (const AppearanceTrack& track : c.recording.appearances)
            {
                w.Put(track.id);
                w.Count(track.changes.size());
                for (const AppearanceChange& change : track.changes)
                {
                    w.Put(change.timeMs);
                    WriteAppearance(w, change.appearance);
                }
            }
        }
        void ReadAppearanceChanges(Reader& r, LoadContext& c)
        {
            c.recording.appearances.resize(r.Count(12));
            for (AppearanceTrack& track : c.recording.appearances)
            {
                track.id = r.Get<std::uint32_t>();
                track.changes.resize(r.Count(24));
                std::uint32_t lastMs = 0;
                for (AppearanceChange& change : track.changes)
                {
                    change.timeMs = r.Get<std::uint32_t>();
                    ReadAppearance(r, change.appearance);
                    if (change.timeMs < lastMs)
                        r.Fail();
                    lastMs = change.timeMs;
                }
            }
        }

    namespace
    {
        constexpr std::uint32_t kMetapedMaxExpressions = 4096;
        bool MetapedHasShopItems(const SaveContext& c)
        {
            for (const metaped::Track& track : c.recording.metapeds)
                for (const metaped::State& state : track.states)
                    if (!state.shopItems.empty())
                        return true;
            return false;
        }
    }
        std::uint16_t MetapedSaveVersion(const SaveContext& c)
        {
            return MetapedHasShopItems(c) ? kMetapedVersion : kMetapedV1Version;
        }
        void WriteMetapeds(Writer& w, const SaveContext& c)
        {
            const bool v2 = MetapedHasShopItems(c);
            w.Count(c.recording.metapeds.size());
            for (const metaped::Track& track : c.recording.metapeds)
            {
                w.Put(track.id);
                w.Count(track.states.size());
                for (const metaped::State& state : track.states)
                {
                    w.Put(state.timeMs);
                    w.Enum(state.source);
                    w.Put(state.flags);
                    w.Put(state.outfit);
                    w.Put(state.metapedType);
                    w.Put(state.layerCount);
                    w.Put(state.layersPrint);
                    w.Put(state.scale);
                    w.Put(state.cleanliness);
                    w.Count(state.expressions.size());
                    for (const metaped::Expression& e : state.expressions)
                    {
                        w.Put(e.id);
                        w.Put(e.value);
                    }
                    if (!v2)
                        continue;
                    w.Count(state.shopItems.size());
                    for (const metaped::ShopItem& item : state.shopItems)
                    {
                        w.Put(item.item);
                        w.Put(item.state);
                        w.Put(item.flags);
                    }
                }
            }
        }
        void ReadMetapeds(Reader& r, LoadContext& c)
        {
            c.recording.metapeds.resize(r.Count(8));
            for (metaped::Track& track : c.recording.metapeds)
            {
                track.id = r.Get<std::uint32_t>();
                track.states.resize(r.Count(41));
                std::uint32_t lastMs = 0;
                for (metaped::State& state : track.states)
                {
                    state.timeMs = r.Get<std::uint32_t>();
                    state.source = r.Enum<metaped::Source>(static_cast<std::uint8_t>(metaped::Source::Late));
                    state.flags = r.Get<std::uint32_t>();
                    state.outfit = r.Get<std::uint32_t>();
                    state.metapedType = r.Get<std::int32_t>();
                    state.layerCount = r.Get<std::uint32_t>();
                    state.layersPrint = r.Get<std::uint64_t>();
                    state.scale = r.Get<float>();
                    state.cleanliness = r.Get<std::int32_t>();
                    const std::size_t count = r.Count(6);
                    if (count > kMetapedMaxExpressions)
                    {
                        r.Fail();
                        return;
                    }
                    state.expressions.resize(count);
                    std::uint16_t lastId = 0;
                    for (std::size_t n = 0; n < count; ++n)
                    {
                        metaped::Expression& e = state.expressions[n];
                        e.id = r.Get<std::uint16_t>();
                        e.value = r.Get<float>();
                        if (!std::isfinite(e.value) || (n && e.id <= lastId))
                            r.Fail();
                        lastId = e.id;
                    }
                    if (c.sectionVersion >= 2)
                    {
                        const std::size_t items = r.Count(9);
                        if (items > metaped::kMaxShopItems)
                        {
                            r.Fail();
                            return;
                        }
                        state.shopItems.resize(items);
                        for (metaped::ShopItem& item : state.shopItems)
                        {
                            item.item = r.Get<std::uint32_t>();
                            item.state = r.Get<std::uint32_t>();
                            item.flags = r.Get<std::uint8_t>();
                            if (!item.item)
                                r.Fail(); // AddShopItem never keeps 0
                        }
                    }
                    if (state.timeMs < lastMs)
                        r.Fail();
                    lastMs = state.timeMs;
                }
            }
        }
        void WriteVehicleLooks(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.vehicleLooks.size());
            for (const VehicleLook& look : c.recording.vehicleLooks)
            {
                w.Put(look.id.Packed());
                w.Put(look.propSet);
                w.Put(look.lightPropSet);
                w.Put(look.extrasExist);
                w.Put(look.extrasOn);
                w.Put(look.tint);   // version 2
                w.Put(look.livery); // version 2
            }
        }
        void ReadVehicleLooks(Reader& r, LoadContext& c)
        {
            const std::uint16_t version = c.sectionVersion;
            const std::size_t count = r.Count(version >= 2 ? 28 : 20);
            for (std::size_t n = 0; n < count; ++n)
            {
                VehicleLook look;
                look.id = entity::ReplayId::FromPacked(r.Get<std::uint32_t>());
                look.propSet = r.Get<std::uint32_t>();
                look.lightPropSet = r.Get<std::uint32_t>();
                look.extrasExist = r.Get<std::uint32_t>();
                look.extrasOn = r.Get<std::uint32_t>();
                if (version >= 2)
                {
                    look.tint = r.Get<std::int32_t>();
                    look.livery = r.Get<std::int32_t>();
                }
                c.recording.vehicleLooks.push_back(look);
            }
        }
        void WriteDoors(Writer& w, const SaveContext& c)
        {
            w.Count(c.recording.doors.size());
            for (const DoorTrack& door : c.recording.doors)
            {
                w.Put(door.hash);
                w.Count(door.samples.size());
                for (const DoorSample& e : door.samples)
                {
                    w.Put(e.timeMs);
                    for (float v : e.rotation)
                        w.Put(v);
                    for (float v : e.position)
                        w.Put(v);
                }
            }
        }
        void ReadDoors(Reader& r, LoadContext& c)
        {
            const std::size_t count = r.Count(8);
            for (std::size_t n = 0; n < count; ++n)
            {
                DoorTrack door;
                door.hash = r.Get<std::uint32_t>();
                const std::size_t samples = r.Count(4 + 6 * 4);
                std::uint32_t lastMs = 0;
                for (std::size_t k = 0; k < samples; ++k)
                {
                    DoorSample e;
                    e.timeMs = r.Get<std::uint32_t>();
                    for (float& v : e.rotation)
                        v = r.Get<float>();
                    for (float& v : e.position)
                        v = r.Get<float>();
                    if (e.timeMs < lastMs)
                        r.Fail();
                    lastMs = e.timeMs;
                    door.samples.push_back(e);
                }
                c.recording.doors.push_back(std::move(door));
            }
        }
}
