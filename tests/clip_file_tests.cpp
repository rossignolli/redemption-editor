#include "weapon_look_core.h"
#include "clip_file.h"
#include "camera_marker_shake.h"
#include "burn_level.h"

#include "clip_compress.h"
#include "clip_pack.h"
#include "clip_save_queue.h"
#include "world_state.h"
#include "object_check.h" // SceneryRule

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

// The rope records have no setter outside the recorder (the engine side);
// the test fills them the way clip_file.cpp reads them.
namespace clipfile_tests_access
{
    template <typename Tag, auto Member>
    struct Steal
    {
        template <typename Object, typename Visit>
        friend void With(Tag, Object& object, Visit&& visit)
        {
            visit(object.*Member);
        }
    };
    struct RopeRecords
    {
        template <typename Object, typename Visit>
        friend void With(RopeRecords, Object& object, Visit&& visit);
    };
    template struct Steal<RopeRecords, &replay::ropes::Ropes::m_records>;
}

namespace
{
    using namespace replay;
    using entity::ReplayId;

    int g_clipFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_clipFailures;
    }

    // A fake game image: RVAs are relative to this base.
    constexpr std::uintptr_t kBase = 0x140000000ull;
    constexpr std::size_t kImageSize = 0x6000000;
    constexpr std::uint64_t kInImage = kBase + 0x3C1D2E0; // e.g. a vtable
    constexpr std::uint64_t kHeap = 0x2A3B4C5D6E70ull;    // outside the image

    void Fill(std::uint8_t* data, std::size_t size, std::uint8_t seed)
    {
        for (std::size_t index = 0; index < size; ++index)
            data[index] = static_cast<std::uint8_t>(seed + index * 7u);
    }

    void PutU64(std::uint8_t* data, std::size_t offset, std::uint64_t value)
    {
        std::memcpy(data + offset, &value, sizeof(value));
    }

    std::uint64_t GetU64(const std::uint8_t* data, std::size_t offset)
    {
        std::uint64_t value = 0;
        std::memcpy(&value, data + offset, sizeof(value));
        return value;
    }

    entity::Appearance MakeAppearance(std::uint32_t seed)
    {
        entity::Appearance appearance;
        appearance.outfit = seed;
        entity::MetaPedAsset asset;
        asset.drawable = seed + 1;
        asset.albedo = seed + 2;
        asset.normal = seed + 3;
        asset.material = seed + 4;
        asset.palette = seed + 5;
        asset.tint0 = -1;
        asset.tint1 = 7;
        asset.tint2 = 9;
        appearance.assets.push_back(asset);
        appearance.vehicleTint = 3;
        appearance.vehicleLivery = -1;
        return appearance;
    }

    // A small three-bone pose track with two layouts (bone 2 demoted half
    // way) and a short keyframe interval, so it has full and relative
    // keyframe chunks.
    pose::PoseTrack MakePoseTrack()
    {
        pose::SkeletonLayout layout;
        layout.boneCount = 3;
        layout.parent = {pose::kRootParent, 0, 1};
        layout.ambiguous = {0, 0, 0};
        layout.highPrecision = {0, 0, 0};
        layout.evaluationOrder = {0, 1, 2};
        pose::StreamConfig config;
        config.keyframeIntervalMs = 200;
        pose::PoseTrack track;
        track.Begin(layout, config);
        for (std::uint32_t timeMs = 0; timeMs <= 900; timeMs += 33)
        {
            if (timeMs == 495)
            {
                pose::SkeletonLayout demoted = layout;
                pose::DemoteBones(demoted, {2});
                track.UpdateLayout(demoted);
            }
            const float t = static_cast<float>(timeMs) / 1000.0f;
            std::vector<pose::BoneQts> locals(3);
            for (std::uint32_t bone = 0; bone < 3; ++bone)
            {
                const float angle = t * (1.0f + static_cast<float>(bone));
                locals[bone].rotation = pose::Normalize({0.0f, 0.0f, std::sin(angle * 0.5f), std::cos(angle * 0.5f)});
                locals[bone].translation[0] = 0.1f * static_cast<float>(bone) + t;
                locals[bone].translation[2] = 0.3f;
            }
            std::vector<pose::BoneQts> objects(3);
            pose::ComposeObjects(layout, locals.data(), nullptr, objects.data());
            float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, t, 2.0f, 3.0f, 1};
            pose::FrameInput input;
            input.timeMs = timeMs;
            input.world = world;
            input.locals = locals.data();
            input.objects = objects.data();
            track.Append(input);
        }
        return track;
    }

    struct Clip
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        camera::Track camera;
    };

    // One event or more in every clip member, with the session pointers
    // already cleared (the pointer rules have their own test).
    void MakeClip(Clip& clip)
    {
        Recording& rec = clip.recording;
        const ReplayId ped = rec.ids.Allocate();
        const ReplayId weapon = rec.ids.Allocate();
        const ReplayId prop = rec.ids.Allocate();

        entity::CreatePacket pedCreate;
        pedCreate.id = ped;
        pedCreate.timeMs = 0;
        pedCreate.kind = entity::EntityKind::Ped;
        pedCreate.modelHash = 0xA1B2C3D4u;
        pedCreate.boneCount = 3;
        pedCreate.appearance = MakeAppearance(100);
        entity::CreatePacket weaponCreate;
        weaponCreate.id = weapon;
        weaponCreate.timeMs = 10;
        weaponCreate.kind = entity::EntityKind::Weapon;
        weaponCreate.modelHash = 0x11111111u;
        weaponCreate.weaponHash = 0x22222222u;
        weaponCreate.weaponComponents = {0x33u, 0x44u};
        entity::CreatePacket propCreate;
        propCreate.id = prop;
        propCreate.timeMs = 2600;
        propCreate.kind = entity::EntityKind::Prop;
        propCreate.modelHash = 0x55555555u;
        propCreate.owner = ped;
        propCreate.propOrigin = entity::PropOrigin::GorePiece;
        propCreate.appearance = MakeAppearance(200);

        rec.lifecycle.Begin();
        rec.lifecycle.Advance(0);
        rec.lifecycle.Create(pedCreate);
        rec.lifecycle.Create(weaponCreate);
        rec.lifecycle.Attach({weapon, ped, entity::AttachmentKind::WeaponPoint, 3}, 20);
        rec.lifecycle.Advance(2500);
        rec.lifecycle.Create(propCreate);
        rec.lifecycle.Detach(weapon, 3000);
        rec.lifecycle.Delete(prop, 4100);

        rec.segments.Begin();
        const std::uint32_t segmentRecord = rec.segments.NextRecordId();
        rec.segments.Add({ped, 1500, segmentRecord, 3, MakeAppearance(300)});
        rec.created = {pedCreate, weaponCreate, propCreate};

        WeaponTrack weaponTrack;
        weaponTrack.recordId = weapon.Packed();
        for (std::uint32_t timeMs = 0; timeMs <= 99; timeMs += 33)
        {
            entity::TransformSample sample;
            sample.timeMs = timeMs;
            sample.relative.position = {0.1f, 0.2f, static_cast<float>(timeMs)};
            sample.relative.rotation = {0.0f, 0.0f, 0.38268343f, 0.92387953f};
            sample.visible = timeMs != 66;
            weaponTrack.track.Append(sample);
        }
        rec.weaponTracks.push_back(weaponTrack);

        StrapTrack strap;
        strap.recordId = weapon.Packed();
        weapon::StrapState state;
        state.maskBits = {{0xABCu, true}, {0xDEFu, false}};
        state.flags = 0x20000u;
        state.weightA = 0.25f;
        state.weightB = 0.75f;
        state.hasStraps = true;
        strap.states.push_back({100, state});
        rec.strapTracks.push_back(strap);

        AttachFrameTrack attach;
        attach.recordId = weapon.Packed();
        weapon::AttachFrame frame;
        frame.bone = 12;
        frame.position[0] = 0.5f;
        frame.rotation[2] = 0.7071f;
        frame.rotation[3] = 0.7071f;
        attach.frames.push_back({120, frame});
        rec.attachFrames.push_back(attach);

        ropes::RopeRecord rope;
        rope.engineId = 9;
        rope.type = 4;
        rope.vertices = 2;
        rope.attachments = {{ped, 5}, {ReplayId{}, -1}};
        rope.nearPed = ped;
        rope.slotEvents.push_back({0, 0, ropes::RopeHolder::Recorded, ped, 5});
        rope.slotEvents.push_back({40, 1, ropes::RopeHolder::Other, ReplayId{}, -1});
        rope.slotState = rope.slotEvents;
        rope.heldEvents = {{0, false}, {20, true}};
        rope.times = {0, 33};
        rope.positions = {0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 2};
        rope.windowCounts = {2, 2};
        rope.nextStateLogMs = 500;
        With(clipfile_tests_access::RopeRecords{}, rec.ropes,
            [&](std::vector<ropes::RopeRecord>& records) { records.push_back(rope); });

        entity::ShotEvent shot;
        shot.timeMs = 50;
        shot.shooter = ped;
        shot.weapon = weapon;
        shot.weaponHash = 0x22222222u;
        shot.muzzle = {1, 2, 3};
        shot.direction = {0, 1, 0};
        shot.impact = {1, 30, 3};
        shot.hasImpact = true;
        shot.impactNormal = {0, -1, 0};
        shot.hasNormal = true;
        shot.muzzleFx = 0x1234u;
        shot.impactFx = 0x5678u;
        shot.muzzleFlags = 3;
        shot.hasMuzzleFlags = true;
        Fill(shot.fireInfo.data(), shot.fireInfo.size(), 1);
        shot.hasFireInfo = true;
        entity::ShotEvent::FireRefField ref;
        ref.offset = 0x10;
        ref.role = entity::ShotEvent::FireRef::Shooter;
        ref.target = ped;
        shot.fireRefs.push_back(ref);
        ref.offset = 0x40;
        ref.role = entity::ShotEvent::FireRef::Unknown;
        ref.target = ReplayId{};
        ref.via = weapon;
        ref.viaOffset = 0x448;
        shot.fireRefs.push_back(ref);
        PutU64(shot.fireInfo.data(), 0x10, 0);
        PutU64(shot.fireInfo.data(), 0x40, 0);
        shot.shooterPlayer = 2; // SHOT version 2
        rec.shots.Add(shot);

        entity::SpeechEvent speech;
        speech.timeMs = 60;
        speech.speaker = ped;
        speech.voiceHash = 0x99u;
        speech.contextPartialHash = 0x98u;
        speech.variation = 4;
        speech.requestedContextPartialHash = 0x97u;
        speech.voiceObjectType = 0x22;
        speech.voiceObjectPartial = 0x96u;
        speech.dataType = 2;
        speech.path = 0x3000u;
        speech.coreMs = 55;
        speech.timedByStart = true;
        speech.stopMs = 900;
        speech.stopReason = 3;
        speech.endMs = 1200; // SPCH v2
        speech.startedBeforeMs = 777; // SPCH v3
        speech.request.read = true; // SPRQ
        for (std::size_t f = 0; f < replay::speech::request::kFieldCount; ++f)
            speech.request.value[f] = 0x100u + static_cast<std::uint32_t>(f);
        speech.request.setRead = true; // SPRQ v2
        speech.request.setHash = 0xCAFEF00Du;
        speech.request.setRef = 0x438F79u;
        rec.speechLines.Add(speech);

        entity::FootstepEvent step;
        step.timeMs = 70;
        step.entity = ped;
        step.eventId = 12;
        step.arg3 = 13;
        step.foot = 1;
        step.arg5 = 2;
        step.flag = 3;
        step.arg8 = 4;
        step.arg7 = 5;
        step.materialHash = 0x9cfbf97au;
        step.materialRef = 0x2f36cbu;
        step.wetness = 214; // FOOT v3
        step.mixKnown = 1; // FOOT v4
        step.mixVolume = -4.07f;
        step.mixHighPass = 150;
        step.mixLowPass = 5000;
        step.driveKnown = 3; // FOOT v5
        step.driveBase = -2.15f;
        step.driveImpact = 0.39f;
        step.layersKnown = 1; // FOOT v6
        step.layer[0] = 0;
        step.layer[1] = 731;
        step.layer[4] = 65535;
        step.audioLod = 2;
        rec.footsteps.Add(step);

        entity::ExplosionAudioEvent explosion;
        explosion.timeMs = 80;
        explosion.hasInstance = true;
        explosion.tag = 7;
        explosion.overrideRef = 0x77u;
        explosion.position[0] = 5.0f;
        explosion.underwater = 1;
        explosion.b31 = 2;
        explosion.b32 = 3;
        explosion.interior = 4;
        explosion.caller = 0x5000u;
        rec.explosions.Add(explosion);

        // Animation audio events.
        entity::AnimAudioEvent anim;
        anim.timeMs = 85;
        anim.entity = ped;
        anim.hash = 0x0F198072u;
        anim.extra = -1;
        rec.animAudio.Add(anim);
        anim.timeMs = 86;
        anim.hash = 0x7F763E34u;
        anim.extra = 3;
        rec.animAudio.Add(anim);

        entity::SurfaceEvent surface;
        surface.timeMs = 90;
        surface.entity = ped;
        surface.wet[1] = 0.5f;
        surface.wetFlags = 6;
        surface.dirt[2] = -3;
        rec.surfaces.Add(surface);

        entity::FxEvent fx;
        fx.timeMs = 100;
        fx.effectHash = 0xF1u;
        fx.assetHash = 0xF2u;
        fx.callerSite = 0xF3u;
        fx.entity = ped;
        fx.bone = 11;
        fx.zoom = 1.5f;
        fx.flags = 0xF4u;
        fx.matrix[12] = 9.0f;
        fx.evolutions[3] = 200;
        fx.hasSettings = true; // FXSS
        fx.settings.mask = entity::FxSettings::kTele3A8 | entity::FxSettings::kLod3A0;
        fx.settings.tele3A8 = 1.0f;
        fx.settings.lod3A0 = 0.5f;
        rec.fx.Add(fx);

        entity::RegisteredFxTrack registered;
        registered.effectHash = 0xE1u;
        registered.flag = 1;
        registered.arg8 = 2;
        registered.arg9 = 3;
        registered.callerSite = 0xE2u;
        registered.attached = true;
        registered.attachId = prop;
        registered.attachBone = 4;
        registered.attachModel = 0xE3u;
        registered.attachPos[1] = 8.0f;
        registered.startMs = 110;
        registered.endMs = 180;
        entity::RegisteredFxFrame fxFrame;
        fxFrame.timeMs = 110;
        fxFrame.matrix[0] = 1.0f;
        fxFrame.parent[5] = 1.0f;
        fxFrame.evolutions[0] = 9;
        fxFrame.zoom = 0.8f;
        fxFrame.aux350[2] = 3.0f;
        fxFrame.flags = 0x580u;
        fxFrame.planeCount = 1;
        fxFrame.planes[11] = 4.0f;
        fxFrame.start310[1] = 6.0f;
        fxFrame.farClip = 50.0f;
        registered.frames.push_back(fxFrame);
        fxFrame.timeMs = 150;
        fxFrame.zoom = 0.9f;
        registered.frames.push_back(fxFrame);
        // Attach changes after the start (RFXA section).
        registered.attachTimeline.push_back({130, weapon, 23});
        registered.attachTimeline.push_back({160, ped, 5});
        {
            // Settings at the creation, one stretch without a call (RGSS).
            entity::RegisteredFxSettingsChange change;
            change.timeMs = 110;
            change.settings.mask = entity::FxSettings::kTele3A8;
            change.settings.tele3A8 = 1.0f;
            registered.settings.push_back(change);
            registered.callGaps.push_back({140, 150});
        }
        rec.registeredFx.push_back(registered);

        entity::DecalEvent decal;
        decal.timeMs = 120;
        decal.entity = ped;
        decal.attached = true;
        decal.attachModel = 0xD1u;
        decal.attachPos[2] = 1.0f;
        decal.callerReturn = 0xD2u;
        decal.engineId = 0xD3u;
        decal.requestId = 0xD4u;
        decal.flag = 5;
        decal.infoKnown = true;
        decal.infoHash = 0xD5u;
        decal.infoIndex = 6;
        Fill(decal.settings, sizeof(decal.settings), 2);
        PutU64(decal.settings, 0x88, 0);
        decal.hasEntity = true; // DECL 2
        rec.decals.Add(decal);

        rec.decalRemoves.Add({130, 0xD3u, 1}); // expired (DCRM 2)

        entity::ProjDecalEvent proj;
        proj.timeMs = 140;
        proj.entity = ped;
        proj.attachModel = 0xC1u;
        proj.attachPos[0] = 2.0f;
        proj.typeHash = 0xC2u;
        proj.arg4 = -4;
        proj.position[3] = 1.0f;
        proj.direction[1] = -1.0f;
        proj.side[0] = 1.0f;
        proj.size = 0.4f;
        proj.arg9 = 9;
        proj.arg10 = 10;
        proj.arg11 = 11;
        proj.arg12 = 12;
        proj.infoKnown = true;
        proj.infoHash = 0xC3u;
        proj.infoIndex = 7;
        rec.projDecals.Add(proj);

        rec.gore.Add({150, ped, 3});

        entity::GoreHitEvent goreHit;
        goreHit.timeMs = 160;
        goreHit.entity = ped;
        goreHit.inflictor = weapon;
        Fill(goreHit.context, sizeof(goreHit.context), 3);
        PutU64(goreHit.context, 0x00, 0);
        PutU64(goreHit.context, 0x08, 0);
        goreHit.result = 1;
        goreHit.caller = 0xB4EB3Cu;
        rec.goreHits.Add(goreHit);

        rec.timeScale.Note(0, 1.0f);
        rec.timeScale.Note(170, 0.3f);

        entity::BloodEvent blood;
        blood.timeMs = 170;
        blood.entity = ped;
        blood.slot = 2;
        blood.caller = 0x125E11Cu;
        blood.index = 5;
        blood.age = 0.25f;
        blood.ref78Entity = weapon;
        Fill(blood.record, sizeof(blood.record), 4);
        PutU64(blood.record, 0x78, 0);
        rec.blood.Add(blood);

        for (int n = 0; n < 2; ++n)
        {
            entity::FireStartEvent fire;
            fire.timeMs = 180u + static_cast<std::uint32_t>(n);
            fire.slot = n;
            fire.type = 1;
            fire.group = 2;
            fire.position[2] = 10.0f;
            fire.rng = 0x1122334455667788ull;
            fire.entityA = ped;
            // An unrecorded world object as entity B.
            fire.entityBModel = n == 1 ? 0x3718A8FAu : 0u;
            fire.entityBPos[0] = -752.0f;
            fire.entityBPos[2] = 44.5f;
            fire.caller = 0xAB00u;
            fire.initial = n == 0;
            fire.initialRemnant = n == 0;
            fire.initialAge = 1.5f;
            fire.rebuilt = n == 1; // FIRS version 3
            Fill(fire.desc, sizeof(fire.desc), static_cast<std::uint8_t>(5 + n));
            PutU64(fire.desc, 0x40, 0);
            PutU64(fire.desc, 0x48, 0);
            PutU64(fire.desc, 0xC0, 0);
            PutU64(fire.desc, 0xF0, n == 0 ? kInImage : 0);
            rec.fireStarts.Add(fire);
        }
        rec.fireStops.Add({190, 1, 1, 0});
        rec.fireItems.Add({195, 0, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f});

        // Scene positions (the map origin is refused by ExtendArea).
        rec.ExtendArea(-1300.0f, -400.0f, 90.0f);
        rec.ExtendArea(-1290.0f, -380.0f, 95.0f);

        entity::MeleeEvent melee;
        melee.timeMs = 200;
        melee.entity = ped;
        melee.infoVtable = kInImage;
        melee.infoHash = 0xAAu;
        rec.meleeHits.Add(melee);

        entity::BodyImpactEvent impact;
        impact.timeMs = 210;
        impact.entity = ped;
        impact.other = weapon;
        impact.position[1] = 3.0f;
        impact.arg4 = 4;
        impact.arg5 = 5;
        impact.definitionVtable = kInImage + 8;
        impact.definitionHash = 0xBBu;
        impact.arg7 = 7;
        impact.arg10 = 1;
        rec.bodyImpacts.Add(impact);

        entity::SoundEvent sound;
        sound.timeMs = 220;
        sound.entity = ped;
        sound.audioOffset = 0x40;
        sound.soundRef = 0x0080000012345678ull;
        sound.params.resize(0x140);
        Fill(sound.params.data(), sound.params.size(), 6);
        std::memset(sound.params.data() + 0xC8, 0, 0xE8 - 0xC8);
        std::memset(sound.params.data() + 0x12C, 0, 2); // the handle (the tail after it is kept)
        sound.flags[0] = 1;
        sound.flags[3] = 4;
        sound.kind = 2;
        sound.globalAudio = kInImage + 0x100;
        rec.sounds.Add(sound);
        sound.timeMs = 230;
        sound.params.clear();
        sound.kind = 1;
        sound.globalAudio = 0;
        rec.sounds.Add(sound);

        CollisionSound collision;
        collision.entity = ped;
        collision.startMs = 240;
        collision.endMs = 300;
        collision.ended = true;
        collision.soundRef = 0x77u;
        collision.params = rec.sounds.Events().front().params;
        collision.intensity = {{240, 0.5f}, {260, 1.0f}};
        rec.collisionSounds.push_back(collision);

        // An entity sound (ESND) on a static audio, by hash.
        EntitySound entitySound;
        entitySound.entity = ped;
        entitySound.startMs = 250;
        entitySound.endMs = 290;
        entitySound.ended = true;
        entitySound.soundRef = 0xABF5BCB8u;
        entitySound.byHash = true;
        entitySound.audioRva = 0x5B9F960u;
        entitySound.trackerOffset = 0x450u;
        entitySound.caller = 0x2F2A3FBu;
        entitySound.oneShot = true; // ESND version 2
        entitySound.params = rec.sounds.Events().front().params;
        rec.entitySounds.push_back(entitySound);
        // A ped audio sound (ESND version 3).
        EntitySound pedSound;
        pedSound.entity = ped;
        pedSound.startMs = 260;
        pedSound.endMs = 280;
        pedSound.ended = true;
        pedSound.soundRef = 0x80000400438196ull;
        pedSound.audioOnEntity = true;
        pedSound.audioOffset = 0x4400u;
        pedSound.pedAudio = true;
        pedSound.caller = 0x2F23236u;
        pedSound.params = rec.sounds.Events().front().params;
        rec.entitySounds.push_back(pedSound);

        WorldObjectOrigin world;
        world.id = ped;
        world.model = 0x1234ABCDu;
        world.position[0] = 1.5f;
        world.position[1] = -2.5f;
        world.position[2] = 30.25f;
        world.rotation[2] = 90.0f;
        rec.worldObjects.push_back(world);
        {
            // The scenery (SCNY).
            SceneryObject scenery;
            scenery.model = 0x490CC7C7u;
            scenery.position[0] = 653.43f;
            scenery.rotation[2] = -71.1f;
            scenery.appearMs = 1200;
            scenery.goneMs = 4500;
            scenery.prop = ped;
            scenery.owner = 5;
            scenery.mission = 1;
            scenery.visible = 0; // hidden when recorded, shown at 1500 ms, hidden again at 3000 ms
            scenery.visibleFlipsMs = {1500, 3000};
            rec.scenery.push_back(scenery);
            scenery.prop = {};
            scenery.goneMs = 0xFFFFFFFFu;
            scenery.visible = 1;
            scenery.visibleFlipsMs.clear();
            // A hat that fell before the recording, with the layer it draws
            // (SCHT).
            scenery.model = 0x30762E1Au;
            entity::MetaPedAsset hatLayer;
            hatLayer.drawable = 0xC1AE4E12u;
            hatLayer.albedo = 0x11111111u;
            hatLayer.normal = 0x22222222u;
            hatLayer.material = 0x33333333u;
            hatLayer.palette = 0x9A2F1E44u;
            hatLayer.tint0 = 18;
            hatLayer.tint1 = 52;
            hatLayer.tint2 = 86;
            scenery.hatLook = {hatLayer};
            rec.scenery.push_back(scenery);
            // The children of the scenery (SCCH): the chest's hay lining with
            // its engine frame, a loot item without.
            SceneryChild lining;
            lining.model = 0xF53A5301u; // P_DRESSINGLCMHAY01X
            lining.parent = 1;
            lining.hasFrame = 1;
            lining.frameBone = 0;
            lining.framePosition[2] = 0.12f;
            lining.frameRotation[2] = 0.70710678f;
            lining.frameRotation[3] = 0.70710678f;
            lining.relativePosition[2] = 0.12f;
            lining.position[0] = -1893.23f;
            lining.rotation[2] = 269.1f;
            lining.appearMs = 0;
            rec.sceneryChildren.push_back(lining);
            SceneryChild purse;
            purse.model = 0x8BA64C0Bu;
            purse.parent = 0;
            purse.relativePosition[1] = -0.2f;
            purse.appearMs = 1300;
            purse.goneMs = 4200;
            purse.visible = 0;
            purse.visibleFlipsMs = {1800};
            rec.sceneryChildren.push_back(purse);
            // A scenery entry's burn level (SBLV).
            rec.sceneryBurnLevels.push_back({1, {{0u, 0.0f}, {2500u, 0.6f}}, 0, false});
            // A held note's text and a poster's tint (OLOK), one object and one
            // scenery entry.
            ObjectLook note;
            note.txd = 0x5A0D6A3Bu;
            note.p3 = 0x11u;
            ObjectLook poster;
            poster.tint = 5;
            rec.objectLooks.push_back({0x00120248u, {{0u, ObjectLook{}}, {1200u, note}}});
            rec.sceneryObjectLooks.push_back({0, {{300u, poster}}});
            // A train car's sliding door and latch (VDOR).
            vehdoors::DoorTrack doors;
            doors.id = 0x00120249u;
            doors.samples = {{0u, 1, 0.0f}, {0u, vehdoors::kLatchChannel, 0.0f}, {800u, 1, 0.4f}, {1200u, 1, 1.0f}};
            rec.vehicleDoors.push_back(doors);
        }
        {
            // The owner query answers (EQRY).
            const std::uint32_t mount[4] = {0xA07A1994u, 0xA99DA182u, 0x23568AAEu, 0xD4A63068u};
            const std::uint32_t material[3] = {0x72277E17u, 0x9B922C90u, 0x1F2A20B7u};
            rec.entityQueries.Note(3566, 0x00000001u, 4, mount, 0x000fffff00440dcbull);
            rec.entityQueries.Note(4891, 0x00000001u, 4, mount, 0x0080000f000d9cecull);
            rec.entityQueries.Note(6325, 0x00020003u, 3, material, 0x0080000300128185ull);
        }
        {
            // The foley variants (FVAR): the player's 0/0, an NPC's draw.
            rec.foleyVariants.Note(ped.Packed(), {0, 0}, true);
            rec.foleyVariants.Note(0x00020003u, {1, 3}, false);
        }
        {
            // The recording's samples and listener (MIXP).
            rec.mixProbe.mainId = ped.Packed();
            rec.mixProbe.dropped = 2;
            mixprobe::Sample sample;
            sample.timeMs = 450;
            sample.owner = prop.Packed();
            sample.source = ped.Packed();
            sample.ref = 0x80000500123456ull;
            sample.role = static_cast<std::uint8_t>(mixprobe::Role::Main);
            sample.kind = static_cast<std::uint8_t>(mixprobe::Kind::Shot);
            sample.lod = 1;
            sample.flags = mixprobe::kFlagPlayerBit | mixprobe::kFlagPlayerAudio | mixprobe::kFlagBitKnown;
            sample.dCamDm = 34;
            sample.dPlayerDm = 0;
            sample.dListenerDm = 31;
            rec.mixProbe.samples.push_back(sample);
            mixprobe::Listener listener;
            listener.timeMs = 1000;
            listener.cam[0] = 1.5f;
            listener.player[1] = -2.0f;
            listener.main[2] = 3.25f;
            listener.hasMain = 1;
            listener.music = 1;
            rec.mixProbe.listeners.push_back(listener);
        }
        {
            // The render targets (RTGT): the ledger registered at 0 (initial),
            // linked, the UI app open with its entry, closed, released.
            rendertarget::Event e;
            e.kind = rendertarget::kRegister;
            e.initial = 1;
            e.SetName("ledger_ct");
            rec.renderTargets.events.push_back(e);
            e = rendertarget::Event{};
            e.timeMs = 120;
            e.kind = rendertarget::kLink;
            e.hash = 0x490CC7C7u;
            e.SetName("ledger_ct");
            rec.renderTargets.events.push_back(e);
            e = rendertarget::Event{};
            e.timeMs = 130;
            e.kind = rendertarget::kAppOpen;
            e.hash = 0x684E06DCu;
            e.entry = 0xD61B3290u;
            rec.renderTargets.events.push_back(e);
            e = rendertarget::Event{};
            e.timeMs = 900;
            e.kind = rendertarget::kRelease;
            e.accepted = 0;
            e.SetName("ledger_ct");
            rec.renderTargets.events.push_back(e);
            rec.renderTargets.dropped = 1;
            // The live volume listener (LSTN).
            gamelistener::Sample s;
            s.timeMs = 100;
            s.pos[0] = 10.5f;
            s.pos[1] = -20.25f;
            s.pos[2] = 30.0f;
            s.points = 1;
            s.flags = gamelistener::kSampleOverride;
            rec.gameListener.samples.push_back(s);
            s.timeMs = 133;
            s.pos[0] = 11.0f;
            s.flags = 0;
            // LSTN v2, the microphone and two points.
            s.points = 2;
            s.micHash = 0x2A7C18A2u;
            s.micSource = gamelistener::MicSource::Default;
            s.stored = 2;
            s.point[0].pos[0] = 11.0f;
            s.point[0].weight = 0.5f;
            s.point[1].pos[1] = -3.5f;
            s.point[1].weight = 0.5f;
            s.point[1].f70 = 1.25f;
            rec.gameListener.samples.push_back(s);
            // The movement volume result per step (FMOV).
            {
                footmovement::Sample m;
                m.owner = 0x00010001u;
                m.timeMs = 110;
                m.foot = 3;
                m.result = 2.06f;
                rec.footMovement.Add(m);
                m.timeMs = 140;
                m.foot = 0;
                m.result = -1.97f;
                rec.footMovement.Add(m);
            }
            // A muzzle request (WFXI).
            {
                weaponfx::Intent intent;
                intent.timeMs = 150;
                intent.shooter = 0x00010001u;
                intent.kind = static_cast<std::uint8_t>(weaponfx::Kind::Muzzle);
                intent.bone = 6;
                intent.range = 60.0f;
                rec.weaponFxIntents.Add(intent);
            }
            // The audio wind strength (WNDA).
            windreplay::Sample wind;
            wind.timeMs = 0;
            wind.strength = 0.614f;
            wind.lines[0] = 0x27EA2814u;
            wind.lines[1] = 0x7F622122u;
            wind.flags = windreplay::kSampleLines;
            rec.windAudio.samples.push_back(wind);
            wind.timeMs = 250;
            wind.strength = 0.075f;
            wind.lines[0] = wind.lines[1] = 0x23FB812Bu;
            wind.flags = windreplay::kSampleLines | windreplay::kSampleSimple;
            rec.windAudio.samples.push_back(wind);
            // Two owners' PED_VARIABLES speed (PSPD).
            rec.pedSpeed.Note(0x00010001u, 0, 6.6f);
            rec.pedSpeed.Note(0x00010001u, 250, 7.25f);
            rec.pedSpeed.Note(0x00020003u, 0, 0.0f);
            // A horse's shader variables, two layouts (SHVR).
            {
                const std::vector<shadervars::Dof> dofs = {shadervars::Dof{0x17, 2, 1}, shadervars::Dof{0x17, 4, 2}};
                auto lanes = [](float a, float b) {
                    return std::vector<std::uint32_t>{shadervars::BitsOf(a), shadervars::BitsOf(b), shadervars::BitsOf(b),
                        shadervars::BitsOf(b), shadervars::kNotSet};
                };
                rec.shaderVars.Note(0x00050001u, 0, dofs, lanes(0.25f, -0.5f));
                rec.shaderVars.Note(0x00050001u, 33, dofs, lanes(0.75f, 1.0f));
                rec.shaderVars.Note(0x00050001u, 66, {shadervars::Dof{0x11, 2, 5}}, {shadervars::BitsOf(0.125f)});
            }
            // Two secondary steps (FSEV).
            {
                footevent::Event secondary;
                secondary.owner = 0x00010001u;
                secondary.timeMs = 120;
                secondary.type = 8;
                secondary.foot = 2;
                secondary.origin = static_cast<std::uint8_t>(footevent::Origin::Drain);
                secondary.materialHash = 0x5A5A5A5Au;
                secondary.materialRef = 0x002F49ACu;
                rec.footEvents.Add(secondary);
                secondary.timeMs = 180;
                secondary.type = 2;
                secondary.foot = 3;
                secondary.origin = static_cast<std::uint8_t>(footevent::Origin::Generator);
                secondary.flags = footevent::kFlagImpact;
                secondary.impact = 0.42f;
                rec.footEvents.Add(secondary);
            }
            // A conversation scene with two tags, an ended script scene, and
            // one ped's mix group (MIXS).
            mixscene::Instance convo;
            convo.id = 7;
            convo.hash = mixscene::kConversationScene;
            convo.caller = mixscene::kCallerConversation;
            convo.kind = mixscene::Kind::Conversation;
            convo.startMs = 120;
            convo.tagCount = 2;
            convo.tags[0] = 0xAABBCCDDu;
            convo.tags[1] = 0x11223344u;
            mixscene::Instance script;
            script.id = 8;
            script.hash = 0x12345678u;
            script.caller = mixscene::kCallerScript;
            script.kind = mixscene::Kind::Script;
            script.startMs = 200;
            script.stopMs = 800;
            rec.mixScenes.instances = {convo, script};
            rec.mixScenes.groups.push_back(mixscene::GroupSample{50, ped.Packed(), 0xAABBCCDDu});
            rec.mixScenes.lostStops = 3;
            // MIXS v2: two variable values of the script scene.
            rec.mixScenes.vars.push_back(mixscene::VarSample{200, 8, 0x0D34EC8Au, 0.25f});
            rec.mixScenes.vars.push_back(mixscene::VarSample{500, 8, 0x0D34EC8Au, 1.0f});
            // MIXS v3: the recording read the scene pool.
            rec.mixScenes.flags = mixscene::kTrackPoolScanned;
            // CATS: two categories' controller values.
            rec.categoryState.staticCount = 310;
            rec.categoryState.mapCount = 322;
            catstate::Values speechCat;
            speechCat.volume = -3.9f;
            rec.categoryState.samples.push_back(catstate::Sample{0, 0x9BBF7C4u, speechCat});
            catstate::Values ambient;
            ambient.volume = -7.25f;
            ambient.rolloff = 0.5f;
            ambient.x54 = 0.75f;
            ambient.pitch = -120;
            ambient.lpf = 8000;
            ambient.hpf = 150;
            rec.categoryState.samples.push_back(catstate::Sample{0, 0x11223344u, ambient});
            speechCat.volume = 0.0f;
            rec.categoryState.samples.push_back(catstate::Sample{450, 0x9BBF7C4u, speechCat});
        }
        {
            // The ped visibility (PVIS): hidden at its first sample, shown at 900 ms.
            PedVisibility row;
            row.id = ped;
            row.visible = 0;
            row.flipsMs = {900};
            rec.pedVisibility.push_back(row);
        }
        {
            // A loose fragment piece's groups (FPCE).
            FragPiece row;
            row.id = prop;
            row.source = 0;
            row.groups = {14, 15, 21};
            rec.fragPieces.push_back(row);
        }

        rec.environment = {true, 23u * 3600u + 59u * 60u, 30u, 0x1234u, 0x5678u, 0.25f};
        {
            // The environment track (ENVT).
            EnvironmentSample e;
            e.timeMs = 0;
            e.weatherFrom = e.weatherTo = 0x1234u;
            e.clockS = 100;
            rec.environment.track.push_back(e);
            e.timeMs = 1000;
            e.weatherTo = 0x5678u;
            e.weatherMix = 0.5f;
            e.rain = 0.75f;
            e.windDirection = -1.5f;
            // The weather transition over time (ENVT v2).
            e.transitionKnown = true;
            e.transitionTo = 0x9ABCu;
            e.transitionTotalMs = 40000.0f;
            e.transitionLeftMs = 12500.0f;
            e.transitionFlag = 1;
            rec.environment.track.push_back(e);
        }
        {
            // The timecycle track (TCMD): a modifier with a transition, then a
            // blizzard variation.
            TimecycleSample t;
            t.timeMs = 0;
            t.modifierKey = 0x2A5D4C1Bu;
            t.modifierIndex = 812;
            t.strength = 0.8f;
            t.transitionKey = 0x0B1E07C3u;
            t.transitionIndex = 1401;
            t.progress = 0.25f;
            t.speed = 0.16f;
            rec.environment.timecycle.push_back(t);
            t.timeMs = 2500;
            t.transitionKey = 0;
            t.transitionIndex = -1;
            t.progress = 0.0f;
            t.speed = 0.0f;
            tctrack::AddVariation(t, 0x27EA2814u, 0x1C1B6E3Du);
            rec.environment.timecycle.push_back(t);
        }
        {
            // The moon track (MOON): two samples, the second across the fmod
            // wrap of the turn angle.
            MoonSample m;
            m.timeMs = 0;
            m.angle = 6.2;
            m.orbit = -1.25;
            m.sun[2] = -0.6f;
            m.moon[0] = 0.3f;
            m.moon[2] = 0.8f;
            m.year = 1899;
            m.month = 11;
            m.day = 24;
            rec.environment.moon.push_back(m);
            m.timeMs = 750;
            m.angle = 0.01;
            m.orbit = -1.2499;
            rec.environment.moon.push_back(m);
        }
        rec.place.recorded = true;
        rec.place.zoneTown = 0xABCDu;
        std::snprintf(rec.place.town, sizeof(rec.place.town), "%s", "Thieves Landing");
        std::snprintf(rec.place.character, sizeof(rec.place.character), "%s", "Arthur");
        rec.gameCamera.push_back({0, {1.0f, 2.0f, 3.0f}, {-10.0f, 0.0f, 170.0f}, 50.0f});
        rec.gameCamera.push_back({33, {1.5f, 2.0f, 3.0f}, {-10.0f, 0.0f, -170.0f}, 55.0f});

        pose::PoseRecordExport record;
        record.recordId = ped.Packed();
        record.boneCount = 3;
        MakePoseTrack().Export(record.track);
        Check(record.track.layouts.size() == 2 && !record.track.anchors.empty() && record.track.chunks.size() > 3,
            "pose track has two layouts, anchors and several chunks");
        bool relative = false;
        for (const pose::TrackChunkExport& chunk : record.track.chunks)
            relative = relative || chunk.anchor >= 0;
        Check(relative, "pose track has a relative keyframe chunk");
        clip.poses.push_back(record);
        record.recordId = segmentRecord;
        clip.poses.push_back(record);

        editor::CameraState first{};
        first.position = {1.0f, 2.0f, 3.0f};
        first.rotation = {10.0f, 0.0f, 45.0f};
        first.fov = 50.0f;
        editor::CameraState second = first;
        second.position.x = 20.0f;
        second.fov = 70.0f;
        clip.camera.AddOrReplace(0, first, editor::Easing::Linear, editor::Path::Linear);
        clip.camera.AddOrReplace(1500, second, editor::Easing::EaseIn, editor::Path::Smooth);
        clip.camera.AddOrReplace(3000, first);
        clip.camera.Remove(2); // ids keep counting past a removed one
        clip.camera.SetMode(3, editor::CameraMode::Follow, 0x00020003u, {1.5f, -4.0f, 2.0f});
    }

    bool SameId(ReplayId a, ReplayId b) { return a == b; }

    bool SameAppearance(const entity::Appearance& a, const entity::Appearance& b)
    {
        if (a.outfit != b.outfit || a.vehicleTint != b.vehicleTint || a.vehicleLivery != b.vehicleLivery ||
            a.assets.size() != b.assets.size())
        {
            return false;
        }
        for (std::size_t n = 0; n < a.assets.size(); ++n)
        {
            const entity::MetaPedAsset& x = a.assets[n];
            const entity::MetaPedAsset& y = b.assets[n];
            if (x.drawable != y.drawable || x.albedo != y.albedo || x.normal != y.normal ||
                x.material != y.material || x.palette != y.palette || x.tint0 != y.tint0 ||
                x.tint1 != y.tint1 || x.tint2 != y.tint2)
            {
                return false;
            }
        }
        return true;
    }

    // sourceHandle is not saved: the loaded one is 0.
    bool SameCreate(const entity::CreatePacket& saved, const entity::CreatePacket& loaded)
    {
        return saved.id == loaded.id && saved.timeMs == loaded.timeMs && saved.kind == loaded.kind &&
            saved.modelHash == loaded.modelHash && loaded.sourceHandle == 0 &&
            saved.boneCount == loaded.boneCount && saved.weaponHash == loaded.weaponHash &&
            saved.weaponComponents == loaded.weaponComponents && saved.owner == loaded.owner &&
            saved.propOrigin == loaded.propOrigin && SameAppearance(saved.appearance, loaded.appearance);
    }

    bool SameCreates(const std::vector<entity::CreatePacket>& a, const std::vector<entity::CreatePacket>& b)
    {
        if (a.size() != b.size())
            return false;
        for (std::size_t n = 0; n < a.size(); ++n)
        {
            if (!SameCreate(a[n], b[n]))
                return false;
        }
        return true;
    }

    template <typename T>
    bool SameBytes(const T& a, const T& b)
    {
        return std::memcmp(&a, &b, sizeof(T)) == 0;
    }

    bool SameVec3(const pose::Vec3& a, const pose::Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

    template <typename Event, typename Same>
    bool SameEvents(const std::vector<Event>& a, const std::vector<Event>& b, Same same)
    {
        if (a.size() != b.size())
            return false;
        for (std::size_t n = 0; n < a.size(); ++n)
        {
            if (!same(a[n], b[n]))
                return false;
        }
        return true;
    }

    bool SameLayoutFields(const pose::SkeletonLayout& a, const pose::SkeletonLayout& b)
    {
        return a.boneCount == b.boneCount && a.order == b.order && a.inheritScale == b.inheritScale &&
            a.parent == b.parent && a.ambiguous == b.ambiguous && a.highPrecision == b.highPrecision &&
            a.evaluationOrder == b.evaluationOrder && a.unresolvedBones == b.unresolvedBones;
    }

    bool SameTrackExport(const pose::TrackExport& a, const pose::TrackExport& b)
    {
        if (a.config.rotationEpsilon != b.config.rotationEpsilon ||
            a.config.translationEpsilon != b.config.translationEpsilon ||
            a.config.translationRange != b.config.translationRange ||
            a.config.keyframeIntervalMs != b.config.keyframeIntervalMs ||
            a.config.byteBudget != b.config.byteBudget || a.layouts.size() != b.layouts.size() ||
            a.anchors != b.anchors || a.chunks.size() != b.chunks.size() || a.currentLayout != b.currentLayout ||
            a.stats.frames != b.stats.frames || a.stats.keyframes != b.stats.keyframes ||
            a.stats.relativeKeyframes != b.stats.relativeKeyframes ||
            a.stats.fullKeyframeBytes != b.stats.fullKeyframeBytes ||
            a.stats.relativeKeyframeBytes != b.stats.relativeKeyframeBytes || a.stats.bytes != b.stats.bytes ||
            a.stats.droppedChunks != b.stats.droppedChunks || a.stats.firstTimeMs != b.stats.firstTimeMs ||
            a.stats.lastTimeMs != b.stats.lastTimeMs)
        {
            return false;
        }
        for (std::size_t n = 0; n < a.layouts.size(); ++n)
        {
            if (!SameLayoutFields(a.layouts[n], b.layouts[n]))
                return false;
        }
        for (std::size_t n = 0; n < a.chunks.size(); ++n)
        {
            const pose::TrackChunkExport& x = a.chunks[n];
            const pose::TrackChunkExport& y = b.chunks[n];
            if (x.layout != y.layout || x.anchor != y.anchor || x.bytes != y.bytes ||
                x.frameTimes != y.frameTimes || x.frameOffsets != y.frameOffsets)
            {
                return false;
            }
        }
        return true;
    }

    bool SamePoseSample(const pose::PoseSample& a, const pose::PoseSample& b)
    {
        if (a.locals.size() != b.locals.size() || a.objects.size() != b.objects.size() || a.t != b.t ||
            a.frameTimeA != b.frameTimeA || a.frameTimeB != b.frameTimeB ||
            std::memcmp(a.worldA, b.worldA, sizeof(a.worldA)) != 0 ||
            std::memcmp(a.worldB, b.worldB, sizeof(a.worldB)) != 0)
        {
            return false;
        }
        return std::memcmp(a.locals.data(), b.locals.data(), a.locals.size() * sizeof(pose::BoneQts)) == 0 &&
            std::memcmp(a.objects.data(), b.objects.data(), a.objects.size() * sizeof(pose::BoneQts)) == 0;
    }

    void CompareClips(Clip& saved, Clip& loaded)
    {
        const Recording& a = saved.recording;
        const Recording& b = loaded.recording;

        Check(a.ids.InUse() == b.ids.InUse(), "ids: in use");
        Check(saved.recording.ids.Allocate() == loaded.recording.ids.Allocate(), "ids: next allocation");

        Check(a.lifecycle.BlockCount() == b.lifecycle.BlockCount(), "lifecycle: block count");
        Check(SameBytes(a.lifecycle.Stats(), b.lifecycle.Stats()), "lifecycle: stats");
        Check(SameCreates(a.lifecycle.Live(), b.lifecycle.Live()), "lifecycle: live");
        Check(a.lifecycle.LiveAttachments() == b.lifecycle.LiveAttachments(), "lifecycle: live attachments");
        bool alive = true;
        for (std::uint32_t timeMs = 0; timeMs <= 5000; timeMs += 50)
        {
            std::vector<entity::CreatePacket> x;
            std::vector<entity::CreatePacket> y;
            a.lifecycle.AliveAt(timeMs, x);
            b.lifecycle.AliveAt(timeMs, y);
            std::vector<entity::Attachment> ax;
            std::vector<entity::Attachment> ay;
            a.lifecycle.AttachmentsAt(timeMs, ax);
            b.lifecycle.AttachmentsAt(timeMs, ay);
            alive = alive && SameCreates(x, y) && ax == ay;
        }
        Check(alive, "lifecycle: alive and attachments at every time");

        Check(SameEvents(a.segments.Segments(), b.segments.Segments(),
                  [](const entity::PoseSegment& x, const entity::PoseSegment& y) {
                      return x.entity == y.entity && x.startMs == y.startMs && x.recordId == y.recordId &&
                          x.boneCount == y.boneCount && SameAppearance(x.appearance, y.appearance);
                  }),
            "segments");
        Check(saved.recording.segments.NextRecordId() == loaded.recording.segments.NextRecordId(),
            "segments: next record id");
        Check(SameCreates(a.created, b.created), "created");

        bool weapons = a.weaponTracks.size() == b.weaponTracks.size();
        for (std::size_t n = 0; weapons && n < a.weaponTracks.size(); ++n)
        {
            const entity::RelativeTrack& x = a.weaponTracks[n].track;
            const entity::RelativeTrack& y = b.weaponTracks[n].track;
            weapons = a.weaponTracks[n].recordId == b.weaponTracks[n].recordId && x.Size() == y.Size() &&
                x.Dropped() == y.Dropped();
            for (std::uint32_t timeMs = 0; weapons && timeMs <= 120; timeMs += 7)
            {
                entity::TransformSample sx;
                entity::TransformSample sy;
                weapons = x.Sample(timeMs, sx) && y.Sample(timeMs, sy) && sx.timeMs == sy.timeMs &&
                    sx.visible == sy.visible && SameVec3(sx.relative.position, sy.relative.position) &&
                    SameBytes(sx.relative.rotation, sy.relative.rotation);
            }
        }
        Check(weapons, "weaponTracks");
        // A later append obeys the same rate gate.
        entity::TransformSample late;
        late.timeMs = 110;
        Check(saved.recording.weaponTracks[0].track.Append(late) == loaded.recording.weaponTracks[0].track.Append(late),
            "weaponTracks: interval kept");

        Check(SameEvents(a.strapTracks, b.strapTracks,
                  [](const StrapTrack& x, const StrapTrack& y) {
                      return x.recordId == y.recordId &&
                          SameEvents(x.states, y.states, [](const auto& p, const auto& q) {
                              return p.first == q.first && p.second.maskBits == q.second.maskBits &&
                                  p.second.flags == q.second.flags && p.second.weightA == q.second.weightA &&
                                  p.second.weightB == q.second.weightB && p.second.hasStraps == q.second.hasStraps;
                          });
                  }),
            "strapTracks");
        Check(SameEvents(a.attachFrames, b.attachFrames,
                  [](const AttachFrameTrack& x, const AttachFrameTrack& y) {
                      return x.recordId == y.recordId &&
                          SameEvents(x.frames, y.frames, [](const auto& p, const auto& q) {
                              return p.first == q.first && p.second.bone == q.second.bone &&
                                  SameBytes(p.second.position, q.second.position) &&
                                  SameBytes(p.second.rotation, q.second.rotation);
                          });
                  }),
            "attachFrames");

        const auto sameSlot = [](const ropes::RopeSlotEvent& x, const ropes::RopeSlotEvent& y) {
            return x.timeMs == y.timeMs && x.slot == y.slot && x.holder == y.holder && x.id == y.id && x.bone == y.bone;
        };
        Check(SameEvents(a.ropes.Records(), b.ropes.Records(),
                  [&](const ropes::RopeRecord& x, const ropes::RopeRecord& y) {
                      return x.engineId == y.engineId && x.type == y.type && x.vertices == y.vertices &&
                          SameEvents(x.attachments, y.attachments,
                              [](const ropes::RopeAttach& p, const ropes::RopeAttach& q) {
                                  return p.id == q.id && p.bone == q.bone;
                              }) &&
                          x.nearPed == y.nearPed && SameEvents(x.slotEvents, y.slotEvents, sameSlot) &&
                          SameEvents(x.slotState, y.slotState, sameSlot) && x.heldEvents == y.heldEvents &&
                          x.times == y.times && x.positions == y.positions && x.windowCounts == y.windowCounts &&
                          !y.playbackLogged && x.nextStateLogMs == y.nextStateLogMs;
                  }),
            "ropes");

        Check(SameEvents(a.shots.Events(), b.shots.Events(),
                  [](const entity::ShotEvent& x, const entity::ShotEvent& y) {
                      return x.timeMs == y.timeMs && x.shooter == y.shooter && x.weapon == y.weapon &&
                          x.weaponHash == y.weaponHash && SameVec3(x.muzzle, y.muzzle) &&
                          SameVec3(x.direction, y.direction) && SameVec3(x.impact, y.impact) &&
                          x.hasImpact == y.hasImpact && SameVec3(x.impactNormal, y.impactNormal) &&
                          x.hasNormal == y.hasNormal && x.muzzleFx == y.muzzleFx && x.impactFx == y.impactFx &&
                          x.muzzleFlags == y.muzzleFlags && x.hasMuzzleFlags == y.hasMuzzleFlags &&
                          x.fireInfo == y.fireInfo && x.hasFireInfo == y.hasFireInfo &&
                          x.shooterPlayer == y.shooterPlayer &&
                          SameEvents(x.fireRefs, y.fireRefs,
                              [](const entity::ShotEvent::FireRefField& p, const entity::ShotEvent::FireRefField& q) {
                                  return p.offset == q.offset && p.role == q.role && p.target == q.target &&
                                      p.via == q.via && p.viaOffset == q.viaOffset;
                              });
                  }),
            "shots");
        Check(SameEvents(a.speechLines.Events(), b.speechLines.Events(),
                  [](const entity::SpeechEvent& x, const entity::SpeechEvent& y) {
                      return x.timeMs == y.timeMs && x.speaker == y.speaker && x.voiceHash == y.voiceHash &&
                          x.contextPartialHash == y.contextPartialHash && x.variation == y.variation &&
                          x.requestedContextPartialHash == y.requestedContextPartialHash && y.voiceObject == 0 &&
                          x.voiceObjectType == y.voiceObjectType && x.voiceObjectPartial == y.voiceObjectPartial &&
                          x.dataType == y.dataType && x.path == y.path && x.coreMs == y.coreMs &&
                          x.timedByStart == y.timedByStart && x.stopMs == y.stopMs && x.stopReason == y.stopReason &&
                          x.endMs == y.endMs && x.startedBeforeMs == y.startedBeforeMs &&
                          replay::speech::request::SameFields(x.request, y.request) && y.request.read;
                  }),
            "speechLines");
        Check(SameEvents(a.footsteps.Events(), b.footsteps.Events(),
                  [](const entity::FootstepEvent& x, const entity::FootstepEvent& y) {
                      return x.timeMs == y.timeMs && x.entity == y.entity && x.eventId == y.eventId &&
                          x.arg3 == y.arg3 && x.foot == y.foot && x.arg5 == y.arg5 && x.flag == y.flag &&
                          x.arg8 == y.arg8 && x.arg7 == y.arg7 && x.materialHash == y.materialHash &&
                          x.materialRef == y.materialRef && x.wetness == y.wetness && x.mixKnown == y.mixKnown &&
                          x.mixVolume == y.mixVolume && x.mixHighPass == y.mixHighPass && x.mixLowPass == y.mixLowPass &&
                          x.driveKnown == y.driveKnown && x.driveBase == y.driveBase && x.driveImpact == y.driveImpact &&
                          x.layersKnown == y.layersKnown && x.layer[0] == y.layer[0] && x.layer[1] == y.layer[1] &&
                          x.layer[2] == y.layer[2] && x.layer[3] == y.layer[3] && x.layer[4] == y.layer[4] &&
                          x.audioLod == y.audioLod;
                  }),
            "footsteps");
        Check(SameEvents(a.explosions.Events(), b.explosions.Events(),
                  [](const entity::ExplosionAudioEvent& x, const entity::ExplosionAudioEvent& y) {
                      return x.timeMs == y.timeMs && x.hasInstance == y.hasInstance && x.tag == y.tag &&
                          x.overrideRef == y.overrideRef && SameBytes(x.position, y.position) &&
                          x.underwater == y.underwater && x.b31 == y.b31 && x.b32 == y.b32 &&
                          x.interior == y.interior && x.caller == y.caller;
                  }),
            "explosions");
        Check(SameEvents(a.animAudio.Events(), b.animAudio.Events(),
                  [](const entity::AnimAudioEvent& x, const entity::AnimAudioEvent& y) {
                      return x.timeMs == y.timeMs && x.entity == y.entity && x.hash == y.hash && x.extra == y.extra;
                  }),
            "animAudio");
        Check(SameEvents(a.surfaces.Events(), b.surfaces.Events(),
                  [](const entity::SurfaceEvent& x, const entity::SurfaceEvent& y) {
                      return x.timeMs == y.timeMs && x.entity == y.entity && SameBytes(x.wet, y.wet) &&
                          x.wetFlags == y.wetFlags && SameBytes(x.dirt, y.dirt);
                  }),
            "surfaces");
        Check(SameEvents(a.fx.Events(), b.fx.Events(),
                  [](const entity::FxEvent& x, const entity::FxEvent& y) {
                      return x.timeMs == y.timeMs && x.effectHash == y.effectHash && x.assetHash == y.assetHash &&
                          x.callerSite == y.callerSite && x.entity == y.entity && x.bone == y.bone &&
                          x.zoom == y.zoom && x.flags == y.flags && SameBytes(x.matrix, y.matrix) &&
                          SameBytes(x.evolutions, y.evolutions);
                  }),
            "fx");
        Check(SameEvents(a.registeredFx, b.registeredFx,
                  [](const entity::RegisteredFxTrack& x, const entity::RegisteredFxTrack& y) {
                      return x.effectHash == y.effectHash && x.flag == y.flag && x.arg8 == y.arg8 &&
                          x.arg9 == y.arg9 && x.callerSite == y.callerSite && x.attached == y.attached &&
                          x.attachId == y.attachId && x.attachBone == y.attachBone &&
                          x.attachModel == y.attachModel && SameBytes(x.attachPos, y.attachPos) &&
                          x.startMs == y.startMs && x.endMs == y.endMs &&
                          SameEvents(x.frames, y.frames,
                              [](const entity::RegisteredFxFrame& p, const entity::RegisteredFxFrame& q) {
                                  return p.timeMs == q.timeMs && SameBytes(p.matrix, q.matrix) &&
                                      SameBytes(p.parent, q.parent) && SameBytes(p.evolutions, q.evolutions) &&
                                      p.zoom == q.zoom && SameBytes(p.aux350, q.aux350) && p.flags == q.flags &&
                                      p.planeCount == q.planeCount && SameBytes(p.planes, q.planes) &&
                                      SameBytes(p.start310, q.start310) && p.farClip == q.farClip;
                              }) &&
                          SameEvents(x.attachTimeline, y.attachTimeline,
                              [](const entity::RegisteredFxAttach& p, const entity::RegisteredFxAttach& q) {
                                  return p.timeMs == q.timeMs && p.id == q.id && p.bone == q.bone;
                              });
                  }),
            "registeredFx");
        Check(SameEvents(a.decals.Events(), b.decals.Events(),
                  [](const entity::DecalEvent& x, const entity::DecalEvent& y) {
                      return x.timeMs == y.timeMs && x.entity == y.entity && x.attached == y.attached &&
                          x.attachModel == y.attachModel && SameBytes(x.attachPos, y.attachPos) &&
                          x.callerReturn == y.callerReturn && x.engineId == y.engineId &&
                          x.requestId == y.requestId && x.flag == y.flag && x.infoKnown == y.infoKnown &&
                          x.infoHash == y.infoHash && x.infoIndex == y.infoIndex && SameBytes(x.settings, y.settings) &&
                          x.hasEntity == y.hasEntity;
                  }),
            "decals");
        Check(SameEvents(a.decalRemoves.Events(), b.decalRemoves.Events(),
                  [](const entity::DecalRemoveEvent& x, const entity::DecalRemoveEvent& y) {
                      return x.timeMs == y.timeMs && x.engineId == y.engineId && x.expired == y.expired;
                  }),
            "decalRemoves");
        Check(SameEvents(a.projDecals.Events(), b.projDecals.Events(),
                  [](const entity::ProjDecalEvent& x, const entity::ProjDecalEvent& y) {
                      return x.timeMs == y.timeMs && x.entity == y.entity && x.attachModel == y.attachModel &&
                          SameBytes(x.attachPos, y.attachPos) && x.typeHash == y.typeHash && x.arg4 == y.arg4 &&
                          SameBytes(x.position, y.position) && SameBytes(x.direction, y.direction) &&
                          SameBytes(x.side, y.side) && x.size == y.size && x.arg9 == y.arg9 &&
                          x.arg10 == y.arg10 && x.arg11 == y.arg11 && x.arg12 == y.arg12 &&
                          x.infoKnown == y.infoKnown && x.infoHash == y.infoHash && x.infoIndex == y.infoIndex;
                  }),
            "projDecals");
        Check(SameEvents(a.gore.Events(), b.gore.Events(),
                  [](const entity::GoreEvent& x, const entity::GoreEvent& y) {
                      return x.timeMs == y.timeMs && x.entity == y.entity && x.limb == y.limb;
                  }),
            "gore");
        Check(SameEvents(a.goreHits.Events(), b.goreHits.Events(),
                  [](const entity::GoreHitEvent& x, const entity::GoreHitEvent& y) {
                      return x.timeMs == y.timeMs && x.entity == y.entity && x.inflictor == y.inflictor &&
                          SameBytes(x.context, y.context) && x.result == y.result && x.caller == y.caller;
                  }),
            "goreHits");
        Check(SameEvents(a.timeScale.Samples(), b.timeScale.Samples(),
                  [](const timescale::Sample& x, const timescale::Sample& y) {
                      return x.timeMs == y.timeMs && x.scale == y.scale;
                  }),
            "timeScale");
        Check(SameEvents(a.blood.Events(), b.blood.Events(),
                  [](const entity::BloodEvent& x, const entity::BloodEvent& y) {
                      return x.timeMs == y.timeMs && x.entity == y.entity && x.slot == y.slot &&
                          x.caller == y.caller && x.index == y.index && x.age == y.age &&
                          x.ref78Entity == y.ref78Entity && SameBytes(x.record, y.record);
                  }),
            "blood");
        Check(SameEvents(a.fireStarts.Events(), b.fireStarts.Events(),
                  [](const entity::FireStartEvent& x, const entity::FireStartEvent& y) {
                      return x.timeMs == y.timeMs && x.slot == y.slot && x.type == y.type && x.group == y.group &&
                          SameBytes(x.position, y.position) && x.rng == y.rng && x.entityA == y.entityA &&
                          x.entityB == y.entityB && x.caller == y.caller && x.initial == y.initial &&
                          x.initialRemnant == y.initialRemnant && x.initialAge == y.initialAge &&
                          SameBytes(x.desc, y.desc) && x.entityAModel == y.entityAModel &&
                          x.entityBModel == y.entityBModel && SameBytes(x.entityAPos, y.entityAPos) &&
                          SameBytes(x.entityBPos, y.entityBPos) && x.rebuilt == y.rebuilt;
                  }),
            "fireStarts");
        Check(SameEvents(a.fireStops.Events(), b.fireStops.Events(),
                  [](const entity::FireStopEvent& x, const entity::FireStopEvent& y) {
                      return x.timeMs == y.timeMs && x.start == y.start && x.instant == y.instant && x.arg3 == y.arg3;
                  }),
            "fireStops");
        Check(SameEvents(a.fireItems.Events(), b.fireItems.Events(),
                  [](const entity::FireItemSample& x, const entity::FireItemSample& y) {
                      return x.timeMs == y.timeMs && x.start == y.start && x.outer == y.outer &&
                          x.burnt == y.burnt && x.ember == y.ember && x.glow == y.glow && x.width == y.width;
                  }),
            "fireItems");
        Check(a.hasArea == b.hasArea && SameBytes(a.areaMin, b.areaMin) && SameBytes(a.areaMax, b.areaMax), "area");
        Check(SameEvents(a.meleeHits.Events(), b.meleeHits.Events(),
                  [](const entity::MeleeEvent& x, const entity::MeleeEvent& y) {
                      return x.timeMs == y.timeMs && x.entity == y.entity && y.info == 0 &&
                          x.infoVtable == y.infoVtable && x.infoHash == y.infoHash;
                  }),
            "meleeHits");
        Check(SameEvents(a.bodyImpacts.Events(), b.bodyImpacts.Events(),
                  [](const entity::BodyImpactEvent& x, const entity::BodyImpactEvent& y) {
                      return x.timeMs == y.timeMs && x.entity == y.entity && x.other == y.other &&
                          SameBytes(x.position, y.position) && x.arg4 == y.arg4 && x.arg5 == y.arg5 &&
                          y.definition == 0 && x.definitionVtable == y.definitionVtable &&
                          x.definitionHash == y.definitionHash && x.arg7 == y.arg7 && x.arg10 == y.arg10;
                  }),
            "bodyImpacts");
        Check(SameEvents(a.sounds.Events(), b.sounds.Events(),
                  [](const entity::SoundEvent& x, const entity::SoundEvent& y) {
                      return x.timeMs == y.timeMs && x.entity == y.entity && x.audioOffset == y.audioOffset &&
                          x.soundRef == y.soundRef && x.params == y.params && SameBytes(x.flags, y.flags) &&
                          x.kind == y.kind && x.globalAudio == y.globalAudio;
                  }),
            "sounds");
        Check(SameEvents(a.collisionSounds, b.collisionSounds,
                  [](const CollisionSound& x, const CollisionSound& y) {
                      return x.entity == y.entity && x.startMs == y.startMs && x.endMs == y.endMs &&
                          x.ended == y.ended && x.soundRef == y.soundRef && x.params == y.params &&
                          x.intensity == y.intensity;
                  }),
            "collisionSounds");
        Check(SameEvents(a.entitySounds, b.entitySounds,
                  [](const EntitySound& x, const EntitySound& y) {
                      return x.entity == y.entity && x.startMs == y.startMs && x.endMs == y.endMs &&
                          x.ended == y.ended && x.soundRef == y.soundRef && x.byHash == y.byHash &&
                          x.audioOnEntity == y.audioOnEntity && x.audioRva == y.audioRva &&
                          x.audioOffset == y.audioOffset && x.trackerOffset == y.trackerOffset &&
                          x.caller == y.caller && x.params == y.params && x.oneShot == y.oneShot &&
                          x.pedAudio == y.pedAudio;
                  }),
            "entitySounds");
        Check(a.worldObjects.size() == b.worldObjects.size() && !b.worldObjects.empty() &&
                  b.worldObjects[0].id == a.worldObjects[0].id && b.worldObjects[0].model == a.worldObjects[0].model &&
                  b.worldObjects[0].position[2] == a.worldObjects[0].position[2] && b.worldObjects[0].rotation[2] == a.worldObjects[0].rotation[2],
            "worldObjects");
        Check(b.scenery.size() == 2 && b.scenery[0].model == 0x490CC7C7u && b.scenery[0].position[0] == 653.43f &&
                  b.scenery[0].rotation[2] == -71.1f && b.scenery[0].appearMs == 1200 && b.scenery[0].goneMs == 4500 &&
                  b.scenery[0].prop == a.scenery[0].prop && b.scenery[0].owner == 5 && b.scenery[0].mission == 1 &&
                  !b.scenery[1].prop.Valid() && b.scenery[1].goneMs == 0xFFFFFFFFu,
            "scenery");
        Check(b.scenery.size() == 2 && b.scenery[0].visible == 0 && b.scenery[0].visibleFlipsMs.size() == 2 &&
                  b.scenery[0].visibleFlipsMs[0] == 1500 && b.scenery[0].visibleFlipsMs[1] == 3000 && b.scenery[1].visible == 1 &&
                  b.scenery[1].visibleFlipsMs.empty(),
            "scenery visibility (SCNY v2, task 46)");
        Check(b.scenery.size() == 2 && b.scenery[0].hatLook.empty() && b.scenery[1].hatLook.size() == 1 &&
                  b.scenery[1].hatLook[0].drawable == 0xC1AE4E12u && b.scenery[1].hatLook[0].material == 0x33333333u &&
                  b.scenery[1].hatLook[0].palette == 0x9A2F1E44u && b.scenery[1].hatLook[0].tint0 == 18 &&
                  b.scenery[1].hatLook[0].tint2 == 86,
            "scenery hats (SCHT, research 19 P2)");
        Check(b.sceneryChildren.size() == 2 && b.sceneryChildren[0].model == 0xF53A5301u && b.sceneryChildren[0].parent == 1 &&
                  b.sceneryChildren[0].hasFrame == 1 && b.sceneryChildren[0].frameBone == 0 &&
                  b.sceneryChildren[0].framePosition[2] == 0.12f && b.sceneryChildren[0].frameRotation[3] == 0.70710678f &&
                  b.sceneryChildren[0].relativePosition[2] == 0.12f && b.sceneryChildren[0].position[0] == -1893.23f &&
                  b.sceneryChildren[0].rotation[2] == 269.1f && b.sceneryChildren[0].goneMs == 0xFFFFFFFFu &&
                  b.sceneryChildren[1].parent == 0 && b.sceneryChildren[1].hasFrame == 0 && b.sceneryChildren[1].appearMs == 1300 &&
                  b.sceneryChildren[1].goneMs == 4200 && b.sceneryChildren[1].visible == 0 &&
                  b.sceneryChildren[1].visibleFlipsMs.size() == 1 && b.sceneryChildren[1].visibleFlipsMs[0] == 1800 &&
                  b.sceneryChildren[1].relativePosition[1] == -0.2f,
            "scenery children (SCCH, task 50)");
        Check(b.sceneryBurnLevels.size() == 1 && b.sceneryBurnLevels[0].id == 1 && b.sceneryBurnLevels[0].states.size() == 2 &&
                  b.sceneryBurnLevels[0].states[1].first == 2500 && b.sceneryBurnLevels[0].states[1].second == 0.6f,
            "scenery burn levels (SBLV, research 19 O3)");
        Check(b.objectLooks.size() == 1 && b.objectLooks[0].id == 0x00120248u && b.objectLooks[0].states.size() == 2 &&
                  b.objectLooks[0].states[1].first == 1200 && b.objectLooks[0].states[1].second.txd == 0x5A0D6A3Bu &&
                  b.objectLooks[0].states[1].second.p3 == 0x11u && b.objectLooks[0].states[0].second == ObjectLook{} &&
                  b.sceneryObjectLooks.size() == 1 && b.sceneryObjectLooks[0].id == 0 &&
                  b.sceneryObjectLooks[0].states[0].first == 300 && b.sceneryObjectLooks[0].states[0].second.tint == 5,
            "object looks (OLOK)");
        Check(b.entityQueries.chains.size() == 2 && b.entityQueries.chains[0].n == 4 &&
                  b.entityQueries.chains[0].refs[3] == 0xD4A63068u && b.entityQueries.chains[1].n == 3 &&
                  b.entityQueries.events.size() == 3 && b.entityQueries.events[1].timeMs == 4891 &&
                  b.entityQueries.events[1].answer == 0x0080000f000d9cecull && b.entityQueries.events[2].owner == 0x00020003u &&
                  b.entityQueries.events[2].chain == 1,
            "entity queries (EQRY)");
        Check(b.foleyVariants.rows.size() == 2 && b.foleyVariants.Find(a.foleyVariants.rows[0].owner) &&
                  b.foleyVariants.rows[0].owner == a.foleyVariants.rows[0].owner && b.foleyVariants.rows[1].a == a.foleyVariants.rows[1].a &&
                  b.foleyVariants.rows[1].b == a.foleyVariants.rows[1].b && b.foleyVariants.rows[0].player == a.foleyVariants.rows[0].player &&
                  b.foleyVariants.rows[1].player == a.foleyVariants.rows[1].player,
            "foley variants (FVAR)");
        Check(b.mixProbe.mainId == a.mixProbe.mainId && b.mixProbe.dropped == 2 && b.mixProbe.samples.size() == 1 &&
                  b.mixProbe.samples[0].timeMs == 450 && b.mixProbe.samples[0].owner == a.mixProbe.samples[0].owner &&
                  b.mixProbe.samples[0].source == a.mixProbe.samples[0].source && b.mixProbe.samples[0].ref == 0x80000500123456ull &&
                  b.mixProbe.samples[0].kind == a.mixProbe.samples[0].kind && b.mixProbe.samples[0].lod == 1 &&
                  b.mixProbe.samples[0].flags == a.mixProbe.samples[0].flags && b.mixProbe.samples[0].dCamDm == 34 &&
                  b.mixProbe.samples[0].dListenerDm == 31 && b.mixProbe.listeners.size() == 1 &&
                  b.mixProbe.listeners[0].timeMs == 1000 && b.mixProbe.listeners[0].cam[0] == 1.5f &&
                  b.mixProbe.listeners[0].player[1] == -2.0f && b.mixProbe.listeners[0].main[2] == 3.25f &&
                  b.mixProbe.listeners[0].hasMain == 1 && b.mixProbe.listeners[0].music == 1,
            "mix probe (MIXP)");
        Check(b.renderTargets.dropped == 1 && b.renderTargets.events.size() == 4 &&
                  b.renderTargets.events[0].initial == 1 && b.renderTargets.events[0].SameName("ledger_ct") &&
                  b.renderTargets.events[1].timeMs == 120 && b.renderTargets.events[1].hash == 0x490CC7C7u &&
                  b.renderTargets.events[1].kind == rendertarget::kLink && b.renderTargets.events[2].entry == 0xD61B3290u &&
                  b.renderTargets.events[2].kind == rendertarget::kAppOpen && b.renderTargets.events[3].accepted == 0 &&
                  b.renderTargets.events[3].kind == rendertarget::kRelease && b.renderTargets.events[3].SameName("ledger_ct"),
            "render targets (RTGT)");
        Check(b.gameListener.samples.size() == 2 && b.gameListener.samples[0].timeMs == 100 &&
                  b.gameListener.samples[0].pos[1] == -20.25f && b.gameListener.samples[0].points == 1 &&
                  b.gameListener.samples[0].flags == gamelistener::kSampleOverride && b.gameListener.samples[1].pos[0] == 11.0f &&
                  b.gameListener.samples[1].flags == 0,
            "game listener (LSTN)");
        Check(b.gameListener.samples.size() == 2 && b.gameListener.samples[0].micSource == gamelistener::MicSource::None &&
                  b.gameListener.samples[0].stored == 0 && b.gameListener.samples[1].micHash == 0x2A7C18A2u &&
                  b.gameListener.samples[1].micSource == gamelistener::MicSource::Default && b.gameListener.samples[1].stored == 2 &&
                  b.gameListener.samples[1].point[1].pos[1] == -3.5f && b.gameListener.samples[1].point[1].f70 == 1.25f &&
                  b.gameListener.samples[1].point[0].weight == 0.5f,
            "game listener v2 (LSTN)");
        Check(b.footMovement.samples.size() == 2 && b.footMovement.samples[0].owner == 0x00010001u &&
                  b.footMovement.samples[0].timeMs == 110 && b.footMovement.samples[0].foot == 3 &&
                  b.footMovement.samples[0].result == 2.06f && b.footMovement.samples[1].result == -1.97f,
            "foot movement (FMOV)");
        Check(b.windAudio.samples.size() == 2 && b.windAudio.samples[0].timeMs == 0 &&
                  b.windAudio.samples[0].strength == 0.614f && b.windAudio.samples[0].lines[1] == 0x7F622122u &&
                  b.windAudio.samples[0].flags == windreplay::kSampleLines && b.windAudio.samples[1].timeMs == 250 &&
                  b.windAudio.samples[1].strength == 0.075f && b.windAudio.samples[1].lines[0] == 0x23FB812Bu &&
                  b.windAudio.samples[1].flags == (windreplay::kSampleLines | windreplay::kSampleSimple),
            "wind audio (WNDA)");
        Check(b.pedSpeed.series.size() == 2 && b.pedSpeed.series[0].owner == 0x00010001u &&
                  b.pedSpeed.series[0].points.size() == 2 && b.pedSpeed.series[0].points[1].timeMs == 250 &&
                  b.pedSpeed.series[0].points[1].speed == 7.25f && b.pedSpeed.series[1].owner == 0x00020003u &&
                  b.pedSpeed.series[1].points.size() == 1 && b.pedSpeed.series[1].points[0].speed == 0.0f,
            "ped speed (PSPD)");
        Check(b.shaderVars.series.size() == 1 && b.shaderVars.series[0].owner == 0x00050001u &&
                  b.shaderVars.series[0].blocks.size() == 2 && b.shaderVars.series[0].blocks[0].times.size() == 2 &&
                  b.shaderVars.series[0].blocks[0].times[1] == 33 && b.shaderVars.series[0].blocks[0].dofs[1].type == 4 &&
                  shadervars::FloatOf(shadervars::FromHalf(b.shaderVars.series[0].blocks[0].values[1])) == -0.5f &&
                  b.shaderVars.series[0].blocks[0].values[4] == shadervars::kHalfNotSet &&
                  b.shaderVars.series[0].blocks[1].dofs[0].track == 0x11 && b.shaderVars.series[0].blocks[1].lanes == 1,
            "shader vars (SHVR)");
        Check(b.footEvents.events.size() == 2 && b.footEvents.events[0].owner == 0x00010001u &&
                  b.footEvents.events[0].timeMs == 120 && b.footEvents.events[0].type == 8 && b.footEvents.events[0].foot == 2 &&
                  b.footEvents.events[0].origin == static_cast<std::uint8_t>(footevent::Origin::Drain) &&
                  b.footEvents.events[0].materialHash == 0x5A5A5A5Au && b.footEvents.events[0].materialRef == 0x002F49ACu &&
                  b.footEvents.events[0].flags == 0 && b.footEvents.events[1].timeMs == 180 && b.footEvents.events[1].type == 2 &&
                  b.footEvents.events[1].flags == footevent::kFlagImpact && b.footEvents.events[1].impact == 0.42f,
            "foot events (FSEV)");
        Check(b.mixScenes.instances.size() == 2 && b.mixScenes.instances[0].id == 7 &&
                  b.mixScenes.instances[0].kind == mixscene::Kind::Conversation && b.mixScenes.instances[0].stopMs == mixscene::kOpen &&
                  b.mixScenes.instances[0].tagCount == 2 && b.mixScenes.instances[0].tags[1] == 0x11223344u &&
                  b.mixScenes.instances[1].hash == 0x12345678u && b.mixScenes.instances[1].stopMs == 800 &&
                  b.mixScenes.groups.size() == 1 && b.mixScenes.groups[0].group == 0xAABBCCDDu && b.mixScenes.lostStops == 3,
            "mix scenes (MIXS)");
        Check(b.mixScenes.vars.size() == 2 && b.mixScenes.vars[0].id == 8 && b.mixScenes.vars[0].name == 0x0D34EC8Au &&
                  b.mixScenes.vars[0].value == 0.25f && b.mixScenes.vars[1].timeMs == 500 && b.mixScenes.vars[1].value == 1.0f,
            "mix scene variables (MIXS v2)");
        Check(b.mixScenes.flags == mixscene::kTrackPoolScanned, "mix scene flags (MIXS v3)");
        Check(b.categoryState.staticCount == 310 && b.categoryState.mapCount == 322 && b.categoryState.samples.size() == 3 &&
                  b.categoryState.samples[0].hash == 0x9BBF7C4u && b.categoryState.samples[0].v.volume == -3.9f &&
                  b.categoryState.samples[1].v.rolloff == 0.5f && b.categoryState.samples[1].v.x54 == 0.75f &&
                  b.categoryState.samples[1].v.pitch == -120 && b.categoryState.samples[1].v.lpf == 8000 &&
                  b.categoryState.samples[1].v.hpf == 150 && b.categoryState.samples[2].timeMs == 450 &&
                  b.categoryState.samples[2].v.volume == 0.0f,
            "category state (CATS)");
        Check(b.pedVisibility.size() == 1 && b.pedVisibility[0].id == a.pedVisibility[0].id && b.pedVisibility[0].visible == 0 &&
                  b.pedVisibility[0].flipsMs.size() == 1 && b.pedVisibility[0].flipsMs[0] == 900,
            "ped visibility (PVIS, task 51)");
        Check(b.fragPieces.size() == 1 && b.fragPieces[0].id == a.fragPieces[0].id && b.fragPieces[0].source == 0 &&
                  b.fragPieces[0].groups == std::vector<std::uint16_t>{14, 15, 21},
            "frag pieces (FPCE, task 48)");
        Check(b.gameCamera.size() == 2 && b.gameCamera[1].timeMs == 33 && b.gameCamera[1].position.x == 1.5f &&
                  b.gameCamera[1].rotation.z == -170.0f && b.gameCamera[1].fov == 55.0f,
            "gameCamera");
        Check(b.environment.recorded && b.environment.clockStartS == a.environment.clockStartS &&
                  b.environment.clockEndS == 30u && b.environment.weatherTo == 0x5678u && b.environment.weatherMix == 0.25f,
            "environment");
        Check(b.environment.track.size() == 2 && b.environment.track[1].timeMs == 1000 &&
                  b.environment.track[1].weatherTo == 0x5678u && b.environment.track[1].rain == 0.75f &&
                  b.environment.track[1].windDirection == -1.5f && b.environment.track[0].clockS == 100,
            "environment track");
        Check(b.environment.track[1].transitionKnown && b.environment.track[1].transitionTo == 0x9ABCu &&
                  b.environment.track[1].transitionTotalMs == 40000.0f && b.environment.track[1].transitionLeftMs == 12500.0f &&
                  b.environment.track[1].transitionFlag == 1 && !b.environment.track[0].transitionKnown,
            "environment track v2: the weather transition (task 42)");
        Check(b.environment.timecycle.size() == 2 && b.environment.timecycle[0].modifierKey == 0x2A5D4C1Bu &&
                  b.environment.timecycle[0].modifierIndex == 812 && b.environment.timecycle[0].strength == 0.8f &&
                  b.environment.timecycle[0].transitionKey == 0x0B1E07C3u && b.environment.timecycle[0].transitionIndex == 1401 &&
                  b.environment.timecycle[0].progress == 0.25f && b.environment.timecycle[0].speed == 0.16f &&
                  b.environment.timecycle[0].variationCount == 0 && b.environment.timecycle[1].timeMs == 2500 &&
                  b.environment.timecycle[1].transitionIndex == -1 && b.environment.timecycle[1].variationCount == 1 &&
                  b.environment.timecycle[1].variations[0].type == 0x27EA2814u &&
                  b.environment.timecycle[1].variations[0].variation == 0x1C1B6E3Du,
            "timecycle track (TCMD, task 43)");
        Check(b.environment.moon.size() == 2 && b.environment.moon[0].angle == 6.2 && b.environment.moon[0].orbit == -1.25 &&
                  b.environment.moon[0].sun[2] == -0.6f && b.environment.moon[0].moon[0] == 0.3f && b.environment.moon[0].moon[2] == 0.8f &&
                  b.environment.moon[0].year == 1899 && b.environment.moon[0].month == 11 && b.environment.moon[0].day == 24 &&
                  b.environment.moon[1].timeMs == 750 && b.environment.moon[1].angle == 0.01 && b.environment.moon[1].orbit == -1.2499,
            "moon track (MOON, research 19 A2)");
        Check(b.place.recorded && b.place.zoneTown == 0xABCDu && std::string(b.place.town) == "Thieves Landing" &&
                  std::string(b.place.character) == "Arthur",
            "place");
        // 23:59:00 to 00:00:30 over 9 s of timeline: halfway is 23:59:45.
        Check(ClockAt(b.environment, 4500, 9000) == 23u * 3600u + 59u * 60u + 45u &&
                  ClockAt(b.environment, 20000, 9000) == 30u,
            "environment clock follows the timeline across midnight");
        Check(b.objectsBefore.empty() && b.pickupsBefore.empty() && b.adoptedObjects.empty(),
            "session handles are not loaded");

        bool poses = saved.poses.size() == loaded.poses.size();
        for (std::size_t n = 0; poses && n < saved.poses.size(); ++n)
        {
            poses = saved.poses[n].recordId == loaded.poses[n].recordId &&
                saved.poses[n].boneCount == loaded.poses[n].boneCount &&
                SameTrackExport(saved.poses[n].track, loaded.poses[n].track);
        }
        Check(poses, "pose records");
        if (poses && !saved.poses.empty())
        {
            pose::PoseTrack original = MakePoseTrack();
            pose::PoseTrack imported;
            std::string error;
            Check(imported.Import(loaded.poses[0].track, error), "pose track imports");
            bool samples = true;
            for (std::uint32_t timeMs = 0; timeMs <= 950; timeMs += 11)
            {
                pose::PoseSample x;
                pose::PoseSample y;
                samples = samples && original.Sample(timeMs, x) && imported.Sample(timeMs, y) && SamePoseSample(x, y);
            }
            Check(samples, "pose track samples identical after the round trip");
            Check(imported.Stats().firstTimeMs == original.Stats().firstTimeMs &&
                    imported.Stats().lastTimeMs == original.Stats().lastTimeMs &&
                    imported.Stats().frames == original.Stats().frames,
                "pose track stats");
        }

        Check(SameEvents(saved.camera.Keyframes(), loaded.camera.Keyframes(),
                  [](const camera::Keyframe& x, const camera::Keyframe& y) {
                      return x.id == y.id && x.timeMs == y.timeMs && SameBytes(x.pose.position, y.pose.position) &&
                          SameBytes(x.pose.orientation, y.pose.orientation) && x.pose.fov == y.pose.fov &&
                          x.easing == y.easing && x.path == y.path && x.mode == y.mode && x.target == y.target &&
                          SameBytes(x.offset, y.offset);
                  }),
            "camera keyframes");
        editor::CameraState state{};
        Check(saved.camera.AddOrReplace(4000, state) == loaded.camera.AddOrReplace(4000, state), "camera next id");
    }

    std::wstring TempPath(const wchar_t* name)
    {
        return (std::filesystem::temp_directory_path() / name).wstring();
    }

    void TestRoundTrip()
    {
        Clip saved;
        MakeClip(saved);
        saved.recording.created[0].sourceHandle = 77;
        saved.recording.objectsBefore = {1, 2};
        saved.recording.pickupsBefore = {3};
        saved.recording.adoptedObjects = {4};

        const std::wstring path = TempPath(L"replaypoc_clip_roundtrip.rpclip");
        std::string error;
        Check(clipfile::Save(saved.recording, saved.poses, saved.camera, 4321, kBase, kImageSize, path, error),
            "save succeeds");
        Check(!std::filesystem::exists(path + L".tmp"), "no temporary file left");

        Clip loaded;
        std::uint64_t duration = 0;
        Check(clipfile::Load(path, kBase, kImageSize, loaded.recording, loaded.poses, loaded.camera, duration, error),
            "load succeeds");
        if (!error.empty())
            std::fprintf(stderr, "load error: %s\n", error.c_str());
        Check(duration == 4321, "duration");

        // Saving the loaded clip gives the same bytes.
        std::vector<std::uint8_t> first;
        std::vector<std::uint8_t> second;
        Clip again;
        MakeClip(again);
        Check(clipfile::Encode(again.recording, again.poses, again.camera, 4321, kBase, kImageSize, first, error),
            "encode");
        Check(clipfile::Encode(loaded.recording, loaded.poses, loaded.camera, 4321, kBase, kImageSize, second, error),
            "encode loaded");
        Check(first == second, "re-encoding the loaded clip gives the same bytes");
        CompareClips(saved, loaded);

        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    bool Contains(const std::vector<std::uint8_t>& bytes, std::uint64_t value)
    {
        for (std::size_t n = 0; n + sizeof(value) <= bytes.size(); ++n)
        {
            if (std::memcmp(bytes.data() + n, &value, sizeof(value)) == 0)
                return true;
        }
        return false;
    }

    void TestPointerRules()
    {
        Clip clip;
        MakeClip(clip);
        Recording& rec = clip.recording;
        constexpr std::uint64_t kHandlePattern = 0x5EC0DE0000001234ull;
        rec.created[0].sourceHandle = 0x7654321;
        rec.speechLines.At(0).voiceObject = kHeap;
        rec.meleeHits.At(0).info = kHeap + 0x10;
        rec.meleeHits.At(0).infoVtable = kInImage;
        rec.bodyImpacts.At(0).definition = kHeap + 0x20;
        rec.bodyImpacts.At(0).definitionVtable = kHeap + 0x28; // not in the image: 0
        PutU64(rec.fireStarts.At(0).desc, 0x40, kHeap + 0x30);
        PutU64(rec.fireStarts.At(0).desc, 0x48, kHeap + 0x38);
        PutU64(rec.fireStarts.At(0).desc, 0xC0, kHeap + 0x40);
        PutU64(rec.fireStarts.At(0).desc, 0xF0, kInImage);
        PutU64(rec.fireStarts.At(1).desc, 0xF0, kHeap + 0x48); // not in the image: 0
        PutU64(rec.blood.At(0).record, 0x78, kHeap + 0x50);
        PutU64(rec.decals.At(0).settings, 0x88, kHeap + 0x58);
        PutU64(rec.goreHits.At(0).context, 0x00, kHeap + 0x60);
        PutU64(rec.goreHits.At(0).context, 0x08, kHeap + 0x68);
        PutU64(rec.shots.At(0).fireInfo.data(), 0x10, kHeap + 0x70);
        PutU64(rec.shots.At(0).fireInfo.data(), 0x40, kHeap + 0x78);
        PutU64(rec.sounds.At(0).params.data(), 0xC8, kHeap + 0x80);
        PutU64(rec.sounds.At(0).params.data(), 0x126, kHandlePattern); // its top 2 bytes are the u16 handle at +0x12C
        rec.sounds.At(0).globalAudio = kInImage + 0x100;
        rec.collisionSounds[0].params = rec.sounds.Events()[0].params;
        rec.objectsBefore = {0x51, 0x52};

        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(rec, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "encode");
        bool leaked = false;
        for (std::uint64_t offset = 0; offset <= 0x80; offset += 8)
            leaked = leaked || Contains(bytes, kHeap + offset);
        Check(!leaked, "no heap pointer in the file");
        Check(!Contains(bytes, kInImage) && !Contains(bytes, kInImage + 0x100), "no absolute image pointer in the file");
        Check(!Contains(bytes, kHandlePattern), "sound params engine fields not in the file");
        Check(Contains(bytes, kInImage - kBase), "image pointers written as RVAs");

        // Loaded with the game at another base (ASLR).
        constexpr std::uintptr_t kOtherBase = 0x7FF6A0000000ull;
        Clip loaded;
        std::uint64_t duration = 0;
        Check(clipfile::Decode(bytes.data(), bytes.size(), kOtherBase, kImageSize, loaded.recording, loaded.poses,
                  loaded.camera, duration, error),
            "decode");
        const Recording& out = loaded.recording;
        Check(out.created[0].sourceHandle == 0, "sourceHandle -> 0");
        Check(out.objectsBefore.empty(), "objectsBefore not saved");
        Check(out.speechLines.Events()[0].voiceObject == 0, "voiceObject -> 0");
        Check(out.meleeHits.Events()[0].info == 0, "melee info -> 0");
        Check(out.meleeHits.Events()[0].infoVtable == kOtherBase + (kInImage - kBase), "melee vtable rebased");
        Check(out.bodyImpacts.Events()[0].definition == 0, "impact definition -> 0");
        Check(out.bodyImpacts.Events()[0].definitionVtable == 0, "impact vtable outside the image -> 0");
        const std::uint8_t* desc = out.fireStarts.Events()[0].desc;
        Check(GetU64(desc, 0x40) == 0 && GetU64(desc, 0x48) == 0 && GetU64(desc, 0xC0) == 0, "fire desc entities -> 0");
        Check(GetU64(desc, 0xF0) == kOtherBase + (kInImage - kBase), "fire desc +0xF0 rebased");
        Check(GetU64(out.fireStarts.Events()[1].desc, 0xF0) == 0, "fire desc +0xF0 outside the image -> 0");
        Check(GetU64(out.blood.Events()[0].record, 0x78) == 0, "blood +0x78 -> 0");
        Check(GetU64(out.decals.Events()[0].settings, 0x88) == 0, "decal +0x88 -> 0");
        Check(GetU64(out.goreHits.Events()[0].context, 0x00) == 0 && GetU64(out.goreHits.Events()[0].context, 0x08) == 0,
            "gore context entities -> 0");
        Check(GetU64(out.shots.Events()[0].fireInfo.data(), 0x10) == 0 &&
                GetU64(out.shots.Events()[0].fireInfo.data(), 0x40) == 0,
            "fire info references -> 0");
        Check(GetU64(out.sounds.Events()[0].params.data(), 0xC8) == 0 &&
                out.sounds.Events()[0].params[0x12C] == 0 && out.sounds.Events()[0].params[0x12D] == 0,
            "sound params engine fields -> 0");
        Check(std::memcmp(out.sounds.Events()[0].params.data() + 0x12E, rec.sounds.Events()[0].params.data() + 0x12E,
                  0x140 - 0x12E) == 0,
            "sound params tail after the handle kept");
        Check(std::memcmp(out.sounds.Events()[0].params.data(), rec.sounds.Events()[0].params.data(), 0xC8) == 0,
            "sound params value fields kept");
        Check(out.sounds.Events()[0].globalAudio == kOtherBase + (kInImage + 0x100 - kBase), "globalAudio rebased");
    }

    // Offset of the section header with this name in an encoded clip.
    std::size_t FindSection(const std::vector<std::uint8_t>& bytes, const char* name)
    {
        std::uint32_t tag = 0;
        for (const clipfile::SectionInfo& info : clipfile::Sections())
        {
            if (std::strcmp(info.name, name) == 0)
                tag = info.tag;
        }
        std::size_t offset = 8 + 4 + 8;
        while (offset + 14 <= bytes.size())
        {
            std::uint32_t found = 0;
            std::uint64_t length = 0;
            std::memcpy(&found, bytes.data() + offset, 4);
            std::memcpy(&length, bytes.data() + offset + 6, 8);
            if (found == tag)
                return offset;
            offset += 14 + static_cast<std::size_t>(length) + 8; // payload hash
        }
        return bytes.size();
    }

    bool DecodeFails(const std::vector<std::uint8_t>& bytes, std::string& error)
    {
        Clip loaded;
        std::uint64_t duration = 0;
        return !clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
    }

    // The clip decodes, and the optional section `name` was left out (its
    // member empty); `note` gets the reason, or the decode error when the clip
    // was refused.
    bool DecodeSkips(const std::vector<std::uint8_t>& bytes, const char* name, std::string& note)
    {
        Clip loaded;
        std::uint64_t duration = 0;
        std::vector<clipfile::SkippedSection> skipped;
        if (!clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
                loaded.camera, duration, note, &skipped))
            return false;
        for (const clipfile::SkippedSection& section : skipped)
        {
            if (section.section == name)
            {
                note = section.reason;
                return true;
            }
        }
        note = "section '" + std::string(name) + "' was not skipped";
        return false;
    }

    std::uint64_t TestFnv64(const std::uint8_t* data, std::size_t size)
    {
        std::uint64_t hash = 0xCBF29CE484222325ull;
        for (std::size_t n = 0; n < size; ++n)
        {
            hash ^= data[n];
            hash *= 0x100000001B3ull;
        }
        return hash;
    }

    // A copy of `bytes` whose section `name` has `version` and its payload
    // shortened by `trim` bytes at the end (a trailing field of a one-event
    // track), with the payload hash recomputed.
    std::vector<std::uint8_t> WithOlderSection(const std::vector<std::uint8_t>& bytes, const char* name,
        std::uint16_t version, std::size_t trim)
    {
        const std::size_t at = FindSection(bytes, name);
        if (at >= bytes.size())
            return {};
        std::uint64_t length = 0;
        std::memcpy(&length, bytes.data() + at + 6, 8);
        if (length < trim)
            return {};
        std::vector<std::uint8_t> payload(bytes.begin() + static_cast<std::ptrdiff_t>(at + 14),
            bytes.begin() + static_cast<std::ptrdiff_t>(at + 14 + length - trim));
        std::vector<std::uint8_t> out(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(at + 4));
        const std::uint64_t newLength = payload.size();
        out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&version),
            reinterpret_cast<const std::uint8_t*>(&version) + sizeof(version));
        out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&newLength),
            reinterpret_cast<const std::uint8_t*>(&newLength) + sizeof(newLength));
        out.insert(out.end(), payload.begin(), payload.end());
        const std::uint64_t hash = TestFnv64(payload.data(), payload.size());
        out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&hash), reinterpret_cast<const std::uint8_t*>(&hash) + sizeof(hash));
        out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at + 14 + length + 8), bytes.end());
        return out;
    }

    // A copy of `bytes` whose section `name` has `version` and `payload`
    // (its hash recomputed).
    std::vector<std::uint8_t> WithSectionPayload(const std::vector<std::uint8_t>& bytes, const char* name, std::uint16_t version,
        const std::vector<std::uint8_t>& payload)
    {
        const std::size_t at = FindSection(bytes, name);
        if (at >= bytes.size())
            return {};
        std::uint64_t length = 0;
        std::memcpy(&length, bytes.data() + at + 6, 8);
        std::vector<std::uint8_t> out(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(at + 4));
        const std::uint64_t newLength = payload.size();
        out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&version),
            reinterpret_cast<const std::uint8_t*>(&version) + sizeof(version));
        out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&newLength),
            reinterpret_cast<const std::uint8_t*>(&newLength) + sizeof(newLength));
        out.insert(out.end(), payload.begin(), payload.end());
        const std::uint64_t hash = TestFnv64(payload.data(), payload.size());
        out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&hash), reinterpret_cast<const std::uint8_t*>(&hash) + sizeof(hash));
        out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at + 14 + length + 8), bytes.end());
        return out;
    }

    // A whole clip with its shake events section (CFXE) at v1, v2 and v3
    // opens through the section table with the events there (not dropped as
    // an optional section), the old ones converted.
    void TestShakeEventsSectionVersions()
    {
        Clip clip;
        MakeClip(clip);
        // A save writes CFXE only while events wait to be folded; one here so
        // the section is in the file.
        {
            camera::fx::ShakeEvent pending;
            pending.timeMs = 100;
            pending.kind = camera::fx::Kind::Amp;
            pending.value = 1.0f;
            clip.camera.LegacyEffects().Add(pending);
        }
        std::vector<std::uint8_t> bytes;
        std::string error;
        clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error);
        const auto put = [](std::vector<std::uint8_t>& out, const auto& value) {
            const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
            out.insert(out.end(), p, p + sizeof(value));
        };
        // One old event: id 1 at 200 ms, Handheld 5 at 100 %, motion off, snap
        // (+ the Custom numbers in v2).
        const auto oldPayload = [&](bool v2) {
            std::vector<std::uint8_t> out;
            put(out, std::uint32_t{2});
            put(out, std::uint64_t{1});
            put(out, std::uint32_t{1});
            put(out, std::uint32_t{200});
            put(out, static_cast<std::uint8_t>(editor::ShakeType::Handheld));
            put(out, std::uint8_t{5});
            put(out, std::uint8_t{4});
            put(out, std::uint8_t{0});
            put(out, std::uint8_t{0});
            if (v2)
            {
                const editor::ShakeCustom c = editor::DefaultShakeCustom();
                put(out, c.amp);
                put(out, c.hz);
                put(out, c.speedAmp);
                put(out, c.speedHz);
                put(out, c.rotWeight);
                put(out, c.posWeight);
                put(out, c.speedRef);
                put(out, c.seed);
                put(out, c.still);
                put(out, std::uint8_t{0});
            }
            return out;
        };
        camera::Track v3Track;
        camera::fx::ShakeEvent e;
        e.timeMs = 200;
        e.kind = camera::fx::Kind::Amp;
        e.value = 1.5f;
        v3Track.LegacyEffects().Add(e);
        e.kind = camera::fx::Kind::OnOff;
        e.value = 1.0f;
        v3Track.LegacyEffects().Add(e);
        const struct
        {
            std::uint16_t version;
            std::vector<std::uint8_t> payload;
            const char* what;
        } cases[] = {
            {1, oldPayload(false), "a clip with CFXE v1 opens with its events converted"},
            {2, oldPayload(true), "a clip with CFXE v2 opens with its events converted"},
            {3, clipfile::EncodeShakeEvents(v3Track), "a clip with CFXE v3 opens with its events"},
        };
        for (const auto& c : cases)
        {
            const std::vector<std::uint8_t> withFx = WithSectionPayload(bytes, "shake events", c.version, c.payload);
            Clip loaded;
            std::uint64_t duration = 0;
            std::vector<clipfile::SkippedSection> skipped;
            std::string loadError;
            const bool ok = !withFx.empty() && clipfile::Decode(withFx.data(), withFx.size(), kBase, kImageSize, loaded.recording,
                                                   loaded.poses, loaded.camera, duration, loadError, &skipped);
            // Folded into the markers on load (an Anchor at 200 ms holds it;
            // nothing old is left).
            const camera::mshake::Resolved s = camera::mshake::Resolve(loaded.camera, 300);
            bool held = ok && skipped.empty() && s.active && loaded.camera.LegacyEffects().Empty() && loaded.camera.Migration().ran;
            if (c.version < 3)
                held = held && s.profile == static_cast<std::uint8_t>(editor::ShakeType::Handheld);
            else
                held = held && s.profile == 0 && s.custom.amp == 1.5f;
            Check(held, c.what);
        }
        std::vector<std::uint8_t> future = WithSectionPayload(bytes, "shake events", 4, clipfile::EncodeShakeEvents(v3Track));
        std::string futureError;
        Check(!future.empty() && DecodeFails(future, futureError) && futureError.find("'shake events'") != std::string::npos,
            "CFXE version 4 refused");
    }

    // The scenery v2 carries each entry's visibility; a v1 section still
    // loads, visible.
    void TestScenerySectionVersions()
    {
        Clip clip;
        MakeClip(clip);
        clip.recording.scenery.clear(); // one entry: the v1 trim below drops its trailing fields
        clip.recording.sceneryChildren.clear(); // their parents were the entries cleared
        SceneryObject hidden;
        hidden.model = 0xD0F1B75Cu; // P_MAGNETO01X, an object in the sky
        hidden.appearMs = 0;
        hidden.mission = 1;
        hidden.visible = 0;
        hidden.rule = static_cast<std::uint8_t>(objcheck::SceneryRule::MapDressing); // SCNY v3
        hidden.ipl = 0xF67D5695u;                                                  // rdtc1's gang camp IPL
        clip.recording.scenery.push_back(hidden);
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "scenery v2: encode");
        Clip loaded;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok && loaded.recording.scenery.size() == 1 && loaded.recording.scenery[0].visible == 0,
            "scenery v2: the hidden entry round trips");
        Check(ok && loaded.recording.scenery.size() == 1 &&
                loaded.recording.scenery[0].rule == static_cast<std::uint8_t>(objcheck::SceneryRule::MapDressing) &&
                loaded.recording.scenery[0].ipl == 0xF67D5695u,
            "scenery v3: the rule and the map dressing IPL round trip");
        // v2: no rule byte, no IPL (1 + 4 bytes for the entry).
        const std::vector<std::uint8_t> v2 = WithOlderSection(bytes, "scenery", 2, 5);
        Check(!v2.empty(), "scenery: v2 payload built");
        if (!v2.empty())
        {
            Clip older;
            std::string olderError;
            const bool oldOk = clipfile::Decode(v2.data(), v2.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, olderError);
            Check(oldOk && older.recording.scenery.size() == 1 && older.recording.scenery[0].visible == 0 &&
                    older.recording.scenery[0].rule == 0 && older.recording.scenery[0].ipl == 0,
                "scenery: a v2 entry loads as legacy, without an IPL");
        }
        // v1: no visibility byte, no flip count (1 + 8 bytes for the entry),
        // nor the v3 rule and IPL (1 + 4).
        const std::vector<std::uint8_t> v1 = WithOlderSection(bytes, "scenery", 1, 9 + 5);
        Check(!v1.empty(), "scenery: v1 payload built");
        if (!v1.empty())
        {
            Clip older;
            std::string olderError;
            const bool oldOk = clipfile::Decode(v1.data(), v1.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, olderError);
            Check(oldOk, "scenery: a clip with the v1 section loads (every clip recorded before task 46)");
            Check(oldOk && older.recording.scenery.size() == 1 && older.recording.scenery[0].visible == 1 &&
                    older.recording.scenery[0].visibleFlipsMs.empty() && older.recording.scenery[0].model == 0xD0F1B75Cu,
                "scenery: a v1 entry loads visible, without flips");
        }
    }

    // The scenery children section is left out of a clip without children
    // (an older build refuses a clip with an unknown section) and written
    // with them; a child whose parent is not a scenery entry (SCNY left out)
    // is dropped on load.
    std::vector<std::uint8_t> WithoutSection(const std::vector<std::uint8_t>& bytes, const char* name); // below

    bool HasTag(const std::vector<std::uint8_t>& bytes, const char (&tag)[5])
    {
        return std::search(bytes.begin(), bytes.end(), tag, tag + 4) != bytes.end();
    }

    void TestSceneryChildrenSection()
    {
        Clip clip;
        MakeClip(clip);
        std::vector<std::uint8_t> with;
        std::vector<std::uint8_t> without;
        std::string error;
        clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, with, error);
        clip.recording.sceneryChildren.clear();
        clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, without, error);
        Check(HasTag(with, "SCCH") && !HasTag(without, "SCCH") && HasTag(without, "SCNY"),
            "scenery children: the section only in a clip that has children");
        Clip loaded;
        std::uint64_t duration = 0;
        Check(clipfile::Decode(without.data(), without.size(), kBase, kImageSize, loaded.recording, loaded.poses, loaded.camera,
                  duration, error) && loaded.recording.sceneryChildren.empty() && loaded.recording.scenery.size() == 2,
            "scenery children: a clip without the section loads (every clip before task 50)");
        const std::vector<std::uint8_t> noScenery = WithoutSection(with, "scenery");
        Clip orphan;
        Check(!noScenery.empty() &&
                  clipfile::Decode(noScenery.data(), noScenery.size(), kBase, kImageSize, orphan.recording, orphan.poses,
                      orphan.camera, duration, error) &&
                  orphan.recording.scenery.empty() && orphan.recording.sceneryChildren.empty(),
            "scenery children: without their scenery the children are dropped, the clip opens");
    }

    // The scenery hats section is left out of a clip without a hat look
    // (older builds open it), a clip without it loads with no look, and a
    // look whose scenery entry is missing (SCNY left out) is dropped.
    void TestSceneryHatsSection()
    {
        Clip clip;
        MakeClip(clip);
        std::vector<std::uint8_t> with;
        std::vector<std::uint8_t> without;
        std::string error;
        clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, with, error);
        for (SceneryObject& object : clip.recording.scenery)
            object.hatLook.clear();
        clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, without, error);
        Check(HasTag(with, "SCHT") && !HasTag(without, "SCHT") && HasTag(without, "SCNY"),
            "scenery hats: the section only in a clip that has a hat look");
        Clip loaded;
        std::uint64_t duration = 0;
        Check(clipfile::Decode(without.data(), without.size(), kBase, kImageSize, loaded.recording, loaded.poses, loaded.camera,
                  duration, error) && loaded.recording.scenery.size() == 2 && loaded.recording.scenery[1].hatLook.empty(),
            "scenery hats: a clip without the section loads, no look (every clip before research 19)");
        const std::vector<std::uint8_t> noScenery = WithoutSection(with, "scenery");
        Clip orphan;
        Check(!noScenery.empty() &&
                  clipfile::Decode(noScenery.data(), noScenery.size(), kBase, kImageSize, orphan.recording, orphan.poses,
                      orphan.camera, duration, error) &&
                  orphan.recording.scenery.empty(),
            "scenery hats: without their scenery the looks are dropped, the clip opens");
    }

    // The scenery burn levels (SBLV) are left out of a clip without them, a
    // clip without the section loads with none, a track whose index is no
    // scenery entry is dropped on load and the levels are bounded.
    void TestSceneryBurnLevelsSection()
    {
        Clip clip;
        MakeClip(clip);
        clip.recording.sceneryBurnLevels.push_back({7, {{100u, 0.5f}}, 0, false});   // no entry 7: dropped
        clip.recording.sceneryBurnLevels.push_back({0, {{100u, 3.0f}}, 0, false});   // bounded to 1
        std::vector<std::uint8_t> with;
        std::vector<std::uint8_t> without;
        std::string error;
        clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, with, error);
        Clip loaded;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(with.data(), with.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok && HasTag(with, "SBLV") && loaded.recording.sceneryBurnLevels.size() == 2 &&
                loaded.recording.sceneryBurnLevels[0].id == 1 && loaded.recording.sceneryBurnLevels[1].id == 0 &&
                loaded.recording.sceneryBurnLevels[1].states[0].second == 1.0f,
            "scenery burn levels: round trip, a track past the scenery dropped, the level bounded");
        clip.recording.sceneryBurnLevels.clear();
        clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, without, error);
        Check(!HasTag(without, "SBLV") && HasTag(without, "SCNY"), "scenery burn levels: the section only in a clip that has one");
        Clip older;
        Check(clipfile::Decode(without.data(), without.size(), kBase, kImageSize, older.recording, older.poses, older.camera,
                  duration, error) && older.recording.sceneryBurnLevels.empty() && older.recording.scenery.size() == 2,
            "scenery burn levels: a clip without the section loads (every clip before research 19)");
        const std::vector<std::uint8_t> noScenery = WithoutSection(with, "scenery");
        Clip orphan;
        Check(!noScenery.empty() &&
                  clipfile::Decode(noScenery.data(), noScenery.size(), kBase, kImageSize, orphan.recording, orphan.poses,
                      orphan.camera, duration, error) &&
                  orphan.recording.scenery.empty() && orphan.recording.sceneryBurnLevels.empty(),
            "scenery burn levels: without their scenery the tracks are dropped, the clip opens");
    }

    // The object looks (OLOK) are left out of a clip without them, a clip
    // without the section loads with none, a scenery track whose index is no
    // scenery entry is dropped on load and the tint is bounded to six bits.
    void TestObjectLooksSection()
    {
        Clip clip;
        MakeClip(clip);
        ObjectLook wild;
        wild.tint = 0xC7; // bounded to 7
        clip.recording.sceneryObjectLooks.push_back({9, {{100u, wild}}}); // no entry 9: dropped
        clip.recording.objectLooks.push_back({0x00130249u, {{50u, wild}}});
        std::vector<std::uint8_t> with;
        std::vector<std::uint8_t> without;
        std::string error;
        clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, with, error);
        Clip loaded;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(with.data(), with.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok && HasTag(with, "OLOK") && loaded.recording.sceneryObjectLooks.size() == 1 &&
                  loaded.recording.objectLooks.size() == 2 && loaded.recording.objectLooks[1].states[0].second.tint == 7,
            "object looks: round trip, a scenery track past the scenery dropped, the tint bounded");
        clip.recording.objectLooks.clear();
        clip.recording.sceneryObjectLooks.clear();
        clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, without, error);
        Check(!HasTag(without, "OLOK"), "object looks: the section only in a clip that has one");
        Clip older;
        Check(clipfile::Decode(without.data(), without.size(), kBase, kImageSize, older.recording, older.poses, older.camera,
                  duration, error) && older.recording.objectLooks.empty() && older.recording.sceneryObjectLooks.empty(),
            "object looks: a clip without the section loads (every older clip)");
        const std::vector<std::uint8_t> noScenery = WithoutSection(with, "scenery");
        Clip orphan;
        Check(!noScenery.empty() &&
                  clipfile::Decode(noScenery.data(), noScenery.size(), kBase, kImageSize, orphan.recording, orphan.poses,
                      orphan.camera, duration, error) &&
                  orphan.recording.sceneryObjectLooks.empty() && orphan.recording.objectLooks.size() == 2,
            "object looks: without their scenery the scenery tracks are dropped, the object tracks stay");
    }

    // The environment track v2 carries the weather transition over time; a
    // v1 section still loads, its transition unknown.
    void TestEnvironmentTrackVersions()
    {
        Clip clip;
        MakeClip(clip);
        std::vector<EnvironmentSample>& track = clip.recording.environment.track;
        track.resize(1); // one sample: the v1 trim below drops its trailing fields
        track[0].transitionKnown = true;
        track[0].transitionTo = 0x2C46C24Bu;
        track[0].transitionTotalMs = 40000.0f;
        track[0].transitionLeftMs = 40000.0f;
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "env track v2: encode");
        Clip loaded;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok && loaded.recording.environment.track.size() == 1 && loaded.recording.environment.track[0].transitionKnown &&
                loaded.recording.environment.track[0].transitionTo == 0x2C46C24Bu,
            "env track v2: the transition round trips");
        // v1: no target, total, time left, flags (3 * 4 + 1 bytes for the sample).
        const std::vector<std::uint8_t> v1 = WithOlderSection(bytes, "environment track", 1, 13);
        Check(!v1.empty(), "env track: v1 payload built");
        if (!v1.empty())
        {
            Clip older;
            std::string olderError;
            const bool oldOk = clipfile::Decode(v1.data(), v1.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, olderError);
            Check(oldOk, "env track: a clip with the v1 section loads (every clip recorded before task 42)");
            Check(oldOk && older.recording.environment.track.size() == 1 && !older.recording.environment.track[0].transitionKnown &&
                    older.recording.environment.track[0].transitionTo == 0 &&
                    older.recording.environment.track[0].weatherFrom == track[0].weatherFrom,
                "env track: a v1 sample loads with its transition unknown");
        }
        // A flags byte with an unknown bit is refused (the section is skipped, the clip opens).
        std::vector<std::uint8_t> bad = bytes;
        const std::size_t at = FindSection(bad, "environment track");
        if (at < bad.size())
        {
            std::uint64_t length = 0;
            std::memcpy(&length, bad.data() + at + 6, 8);
            bad[at + 14 + length - 1] = 0x80;
            const std::uint64_t hash = TestFnv64(bad.data() + at + 14, static_cast<std::size_t>(length));
            std::memcpy(bad.data() + at + 14 + length, &hash, sizeof(hash));
            Clip refused;
            std::string refusedError;
            const bool refusedOk = clipfile::Decode(bad.data(), bad.size(), kBase, kImageSize, refused.recording, refused.poses,
                refused.camera, duration, refusedError);
            Check(refusedOk && refused.recording.environment.track.empty(), "env track: an unknown flag bit drops the track, the clip opens");
        }
        else
            Check(false, "env track: section found");
    }

    // A buffer window saved while the recording ran (QA 232: environment=0 with 94
    // environment samples): the load sets the recorded flag and the clock from the
    // track. A clip without a track stays without its environment.
    void TestEnvironmentRepairOnLoad()
    {
        Clip clip;
        MakeClip(clip);
        Environment& env = clip.recording.environment;
        env.recorded = false;
        env.clockStartS = 53438; // the buffer's start, not the window's
        env.clockEndS = 0;
        env.track[0].clockS = 53500;
        env.track[1].clockS = 53502;
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 2000, kBase, kImageSize, bytes, error),
            "env repair: encode");
        Clip loaded;
        std::uint64_t duration = 0;
        bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        const Environment& got = loaded.recording.environment;
        Check(ok && got.recorded && got.repairedOnLoad && got.clockStartS == 53500 && got.clockEndS == 53502 &&
                got.weatherTo == 0x5678u && got.track.size() == 2,
            "env repair: a clip with a track and no flag loads recorded, the clock from the track");

        // Saved again: recorded on disk, no repair at the next load.
        std::vector<std::uint8_t> again;
        Check(clipfile::Encode(loaded.recording, loaded.poses, loaded.camera, duration, kBase, kImageSize, again, error),
            "env repair: encode the repaired clip");
        Clip reloaded;
        ok = clipfile::Decode(again.data(), again.size(), kBase, kImageSize, reloaded.recording, reloaded.poses,
            reloaded.camera, duration, error);
        Check(ok && reloaded.recording.environment.recorded && !reloaded.recording.environment.repairedOnLoad &&
                reloaded.recording.environment.clockStartS == 53500 && reloaded.recording.environment.clockEndS == 53502,
            "env repair: the repaired clip saved is recorded, no repair at the next load");

        // A clip from before the environment: no track, no clock.
        Clip bare;
        MakeClip(bare);
        bare.recording.environment = Environment{};
        std::vector<std::uint8_t> bareBytes;
        Check(clipfile::Encode(bare.recording, bare.poses, bare.camera, 2000, kBase, kImageSize, bareBytes, error),
            "env repair: encode a clip without environment");
        Clip bareLoaded;
        ok = clipfile::Decode(bareBytes.data(), bareBytes.size(), kBase, kImageSize, bareLoaded.recording, bareLoaded.poses,
            bareLoaded.camera, duration, error);
        Check(ok && !bareLoaded.recording.environment.recorded && !bareLoaded.recording.environment.repairedOnLoad,
            "env repair: a clip without a track stays without its environment");

        // A recorded clip is left as it was (its stop clock is not the track's).
        Clip recorded;
        MakeClip(recorded);
        std::vector<std::uint8_t> recordedBytes;
        Check(clipfile::Encode(recorded.recording, recorded.poses, recorded.camera, 2000, kBase, kImageSize, recordedBytes, error),
            "env repair: encode a recorded clip");
        Clip recordedLoaded;
        ok = clipfile::Decode(recordedBytes.data(), recordedBytes.size(), kBase, kImageSize, recordedLoaded.recording,
            recordedLoaded.poses, recordedLoaded.camera, duration, error);
        Check(ok && recordedLoaded.recording.environment.recorded && !recordedLoaded.recording.environment.repairedOnLoad &&
                recordedLoaded.recording.environment.clockEndS == 30u,
            "env repair: a recorded clip keeps its own clock");
    }

    // Footsteps v1 (no step container) still load with the material 0 (the
    // puppet's own surface).
    void TestFootstepSectionVersions()
    {
        Clip clip;
        MakeClip(clip);
        std::vector<std::uint8_t> bytes;
        std::string error;
        clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error);
        Check(clip.recording.footsteps.Events().size() == 1 && clip.recording.footsteps.Events()[0].materialRef == 0x2f36cbu,
            "footstep version test: one step with its material");
        // The mix after the wetness (v4: 1 + 4 + 2 + 2 bytes).
        // The drive after the mix (v5: 1 + 4 + 4 bytes).
        // The layers and the LOD after the drive (v6: 1 + 5 * 2 + 1 bytes).
        const std::vector<std::uint8_t> v5 = WithOlderSection(bytes, "footsteps", 5, 12);
        Check(!v5.empty(), "footsteps: v5 payload built");
        if (!v5.empty())
        {
            Clip loaded5;
            std::uint64_t duration5 = 0;
            std::string loadError5;
            const bool ok5 = clipfile::Decode(v5.data(), v5.size(), kBase, kImageSize, loaded5.recording, loaded5.poses,
                loaded5.camera, duration5, loadError5);
            Check(ok5 && loaded5.recording.footsteps.Events().size() == 1 &&
                    loaded5.recording.footsteps.Events()[0].driveKnown == 3 &&
                    loaded5.recording.footsteps.Events()[0].layersKnown == 0 &&
                    loaded5.recording.footsteps.Events()[0].audioLod == 0xFF,
                "footsteps v5: drive kept, layers and LOD unknown");
        }
        const std::vector<std::uint8_t> v4 = WithOlderSection(bytes, "footsteps", 4, 21);
        Check(!v4.empty(), "footsteps: v4 payload built");
        if (!v4.empty())
        {
            Clip loaded4;
            std::uint64_t duration4 = 0;
            std::string loadError4;
            const bool ok4 = clipfile::Decode(v4.data(), v4.size(), kBase, kImageSize, loaded4.recording, loaded4.poses,
                loaded4.camera, duration4, loadError4);
            Check(ok4 && loaded4.recording.footsteps.Events().size() == 1 &&
                    loaded4.recording.footsteps.Events()[0].mixKnown == 1 && loaded4.recording.footsteps.Events()[0].driveKnown == 0,
                "footsteps v4: mix kept, drive unknown");
        }
        const std::vector<std::uint8_t> v3 = WithOlderSection(bytes, "footsteps", 3, 30);
        Check(!v3.empty(), "footsteps: v3 payload built");
        if (!v3.empty())
        {
            Clip loaded3;
            std::uint64_t duration3 = 0;
            std::string loadError3;
            const bool ok3 = clipfile::Decode(v3.data(), v3.size(), kBase, kImageSize, loaded3.recording, loaded3.poses,
                loaded3.camera, duration3, loadError3);
            Check(ok3, "a clip with footsteps v3 loads");
            if (ok3)
            {
                const auto& steps3 = loaded3.recording.footsteps.Events();
                Check(steps3.size() == 1 && steps3[0].wetness == 214 && steps3[0].mixKnown == 0 && steps3[0].mixVolume == 0.0f,
                    "footsteps v3: wetness kept, mix unknown");
            }
        }
        // One wetness byte per step after the material (v3).
        const std::vector<std::uint8_t> v2 = WithOlderSection(bytes, "footsteps", 2, 31);
        Check(!v2.empty(), "footsteps: v2 payload built");
        if (!v2.empty())
        {
            Clip loaded2;
            std::uint64_t duration2 = 0;
            std::string loadError2;
            const bool ok2 = clipfile::Decode(v2.data(), v2.size(), kBase, kImageSize, loaded2.recording, loaded2.poses,
                loaded2.camera, duration2, loadError2);
            Check(ok2, "a clip with footsteps v2 loads");
            if (ok2)
            {
                const auto& steps2 = loaded2.recording.footsteps.Events();
                Check(steps2.size() == 1 && steps2[0].materialRef == 0x2f36cbu && steps2[0].wetness == 0,
                    "footsteps v2: material kept, wetness unknown");
            }
        }
        const std::vector<std::uint8_t> old = WithOlderSection(bytes, "footsteps", 1, 39);
        Check(!old.empty(), "footsteps: v1 payload built");
        if (old.empty())
            return;
        Clip loaded;
        std::uint64_t duration = 0;
        std::string loadError;
        const bool ok = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, loadError);
        Check(ok, "a clip with footsteps v1 loads");
        if (ok)
        {
            const auto& steps = loaded.recording.footsteps.Events();
            Check(steps.size() == 1 && steps[0].materialHash == 0 && steps[0].materialRef == 0 && steps[0].wetness == 0 && steps[0].arg7 == 5 &&
                    steps[0].foot == 1 && steps[0].eventId == 12,
                "footsteps v1: material 0, the rest kept");
        }
    }

    // Decals v1 (no hasEntity) and decalRemoves v1 (no expired) still load; a
    // version past this build's is refused.
    void TestDecalSectionVersions()
    {
        Clip clip;
        MakeClip(clip);
        std::vector<std::uint8_t> bytes;
        std::string error;
        clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error);
        Check(clip.recording.decals.Events().size() == 1 && clip.recording.decalRemoves.Events().size() == 1,
            "decal version test: one decal and one removal");
        std::vector<std::uint8_t> old = WithOlderSection(bytes, "decals", 1, 1);
        old = old.empty() ? old : WithOlderSection(old, "decalRemoves", 1, 1);
        Check(!old.empty(), "decal version test: v1 payloads built");
        if (old.empty())
            return;
        Clip loaded;
        std::uint64_t duration = 0;
        std::string loadError;
        const bool ok = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, loadError);
        Check(ok, "a clip with decals v1 and decalRemoves v1 loads");
        if (ok)
        {
            const auto& decals = loaded.recording.decals.Events();
            const auto& removes = loaded.recording.decalRemoves.Events();
            Check(decals.size() == 1 && !decals[0].hasEntity && decals[0].attached && decals[0].engineId == 0xD3u &&
                    std::memcmp(decals[0].settings, clip.recording.decals.Events()[0].settings, sizeof(decals[0].settings)) == 0,
                "decals v1: hasEntity false, the rest kept");
            Check(removes.size() == 1 && removes[0].expired == 0 && removes[0].engineId == 0xD3u && removes[0].timeMs == 130,
                "decalRemoves v1: expired 0");
        }
        std::vector<std::uint8_t> future = bytes;
        const std::size_t decals = FindSection(future, "decals");
        if (decals < future.size())
        {
            future[decals + 4] = 3;
            std::string futureError;
            Check(DecodeFails(future, futureError) && futureError.find("'decals'") != std::string::npos,
                "decals version 3 refused");
        }
        // An expired byte other than 0 or 1 is malformed (the last payload
        // byte of the one-removal section, hash recomputed).
        std::vector<std::uint8_t> bad = bytes;
        const std::size_t removes = FindSection(bad, "decalRemoves");
        if (removes < bad.size())
        {
            std::uint64_t length = 0;
            std::memcpy(&length, bad.data() + removes + 6, 8);
            std::uint8_t* payload = bad.data() + removes + 14;
            payload[length - 1] = 7;
            const std::uint64_t hash = TestFnv64(payload, static_cast<std::size_t>(length));
            std::memcpy(payload + length, &hash, sizeof(hash));
            std::string badError;
            Check(DecodeFails(bad, badError) && badError.find("'decalRemoves'") != std::string::npos,
                "decalRemoves expired 7 refused");
        }
    }

    // The recording gate's sections (CUTS, SPAN) and the charred tracks (SCOR):
    // round trip, a clip without them (older) loads with them empty, bad
    // payloads are refused.
    std::vector<std::uint8_t> WithoutSection(const std::vector<std::uint8_t>& bytes, const char* name)
    {
        const std::size_t at = FindSection(bytes, name);
        if (at >= bytes.size())
            return {};
        std::uint64_t length = 0;
        std::memcpy(&length, bytes.data() + at + 6, 8);
        std::vector<std::uint8_t> out(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
        out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at + 14 + length + 8), bytes.end());
        return out;
    }

    void TestGateSections()
    {
        Clip clip;
        MakeClip(clip);
        const ReplayId ped = clip.recording.created[0].id;
        clip.recording.cuts = {120, 480};
        clip.recording.spans.push_back({100, 300, gate::SpanKind::Cutscene, false});
        clip.recording.spans.push_back({150, 900, gate::SpanKind::ControlsOff, false});
        clip.recording.scorched.push_back({ped.Packed(), {{0, true}, {250, false}, {700, true}}, 0, false});
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "gate: encode");
        Clip loaded;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok, "gate: a clip with cuts, spans and charred tracks loads");
        if (ok)
        {
            const Recording& r = loaded.recording;
            Check(r.cuts == clip.recording.cuts, "gate: cuts round trip");
            Check(r.spans.size() == 2 && r.spans[0].startMs == 100 && r.spans[0].endMs == 300 &&
                    r.spans[0].kind == gate::SpanKind::Cutscene && r.spans[1].kind == gate::SpanKind::ControlsOff &&
                    r.spans[1].endMs == 900 && !r.spans[0].open,
                "gate: spans round trip");
            Check(r.scorched.size() == 1 && r.scorched[0].id == ped.Packed() && r.scorched[0].states.size() == 3 &&
                    r.scorched[0].states[1].first == 250 && !r.scorched[0].states[1].second && r.scorched[0].states[2].second,
                "gate: charred track round trip");
        }
        // Older clip: none of the three sections.
        std::vector<std::uint8_t> old = WithoutSection(bytes, "cuts");
        old = old.empty() ? old : WithoutSection(old, "spans");
        old = old.empty() ? old : WithoutSection(old, "scorched");
        Check(!old.empty(), "gate: clip without the new sections built");
        if (!old.empty())
        {
            Clip older;
            const bool oldOk = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, error);
            Check(oldOk && older.recording.cuts.empty() && older.recording.spans.empty() && older.recording.scorched.empty(),
                "gate: an older clip without the section loads with no cuts, spans or charred tracks");
        }
        // Cuts out of order, a span ending before it starts, a charred byte
        // other than 0 or 1: malformed.
        {
            Clip bad = clip;
            bad.recording.cuts = {480, 120};
            std::vector<std::uint8_t> badBytes;
            clipfile::Encode(bad.recording, bad.poses, bad.camera, 1000, kBase, kImageSize, badBytes, error);
            std::string badError;
            Check(DecodeSkips(badBytes, "cuts", badError), "gate: unsorted cuts refused");
        }
        {
            Clip bad = clip;
            bad.recording.spans[0].endMs = 50;
            std::vector<std::uint8_t> badBytes;
            clipfile::Encode(bad.recording, bad.poses, bad.camera, 1000, kBase, kImageSize, badBytes, error);
            std::string badError;
            Check(DecodeSkips(badBytes, "spans", badError), "gate: inverted span refused");
        }
        {
            std::vector<std::uint8_t> bad = bytes;
            const std::size_t at = FindSection(bad, "scorched");
            if (at < bad.size())
            {
                std::uint64_t length = 0;
                std::memcpy(&length, bad.data() + at + 6, 8);
                std::uint8_t* payload = bad.data() + at + 14;
                payload[length - 1] = 2; // the last state's byte
                const std::uint64_t hash = TestFnv64(payload, static_cast<std::size_t>(length));
                std::memcpy(payload + length, &hash, sizeof(hash));
                std::string badError;
                Check(DecodeSkips(bad, "scorched", badError), "gate: charred byte 2 refused");
            }
        }
    }

    // The broken window masks (VWIN): round trip, a clip without the section
    // (older) loads with none, a mask past window 3 is refused; the lookup
    // holds the first read before it.
    // The GPU water section (WGPU) is retired. A clip holding one (at its old
    // place, before "entity sounds") opens, the section unread (a payload
    // this build could never parse and a wrong hash change nothing), listed
    // as skipped; twice is fine too; a save of the loaded clip writes no WGPU.
    std::vector<std::uint8_t> RawSection(const char (&tag)[5], std::uint16_t version, const std::vector<std::uint8_t>& payload,
        bool goodHash)
    {
        std::vector<std::uint8_t> out(14);
        std::memcpy(out.data(), tag, 4);
        std::memcpy(out.data() + 4, &version, 2);
        const std::uint64_t length = payload.size();
        std::memcpy(out.data() + 6, &length, 8);
        out.insert(out.end(), payload.begin(), payload.end());
        std::uint64_t hash = goodHash ? TestFnv64(payload.data(), payload.size()) : 0x1234ull;
        const std::uint8_t* h = reinterpret_cast<const std::uint8_t*>(&hash);
        out.insert(out.end(), h, h + 8);
        return out;
    }

    void TestRetiredWaterGpuSection()
    {
        bool listed = false;
        for (const clipfile::RetiredSectionInfo& info : clipfile::RetiredSections())
            listed = listed || std::strcmp(info.name, "water gpu") == 0;
        bool known = false;
        for (const clipfile::SectionInfo& info : clipfile::Sections())
            known = known || std::strcmp(info.name, "water gpu") == 0;
        Check(listed && !known, "retired water gpu: listed as retired, not among the read sections");

        Clip clip;
        MakeClip(clip);
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error) && !HasTag(bytes, "WGPU"),
            "retired water gpu: a save writes no WGPU");
        std::size_t at = FindSection(bytes, "entity sounds");
        if (at >= bytes.size())
            at = FindSection(bytes, "pose");
        Check(at < bytes.size(), "retired water gpu: insertion point found");
        if (at >= bytes.size())
            return;
        // An old v1 payload: count 1, time, origin, four scales, a packed blob.
        std::vector<std::uint8_t> payload(4 + 4 + 8 + 16 + 8 + 300, 0xAB);
        const std::uint32_t count = 1;
        std::memcpy(payload.data(), &count, 4);
        const std::vector<std::uint8_t> good = RawSection("WGPU", 1, payload, true);
        const std::vector<std::uint8_t> garbage = RawSection("WGPU", 7, std::vector<std::uint8_t>(5000, 0xFF), false);
        struct Case
        {
            const char* what;
            std::vector<std::uint8_t> sections;
            std::size_t skips;
        };
        std::vector<Case> cases;
        cases.push_back({"an old clip with its WGPU section opens", good, 1});
        cases.push_back({"a WGPU of another version, bad hash and unreadable payload still opens", garbage, 1});
        std::vector<std::uint8_t> both = good;
        both.insert(both.end(), garbage.begin(), garbage.end());
        cases.push_back({"two WGPU sections open too", both, 2});
        for (const Case& c : cases)
        {
            std::vector<std::uint8_t> old(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
            old.insert(old.end(), c.sections.begin(), c.sections.end());
            old.insert(old.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.end());
            Clip loaded;
            std::uint64_t duration = 0;
            std::vector<clipfile::SkippedSection> skipped;
            std::string loadError;
            const bool ok = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, loaded.recording, loaded.poses,
                loaded.camera, duration, loadError, &skipped);
            bool reasons = skipped.size() == c.skips;
            for (const clipfile::SkippedSection& section : skipped)
                reasons = reasons && section.section == "water gpu" && section.reason.rfind("retired_not_read", 0) == 0;
            Check(ok && reasons && duration == 1000 && loaded.recording.created.size() == clip.recording.created.size(),
                (std::string("retired water gpu: ") + c.what + (ok ? "" : " (" + loadError + ")")).c_str());
            std::vector<std::uint8_t> again;
            Check(ok && clipfile::Encode(loaded.recording, loaded.poses, loaded.camera, duration, kBase, kImageSize, again, loadError) &&
                    again == bytes,
                (std::string("retired water gpu: saved again it is the clip without WGPU (") + c.what + ")").c_str());
        }
        // A WGPU cut short is a truncated file, still refused.
        std::vector<std::uint8_t> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
        cut.insert(cut.end(), good.begin(), good.begin() + 20);
        Clip refused;
        std::uint64_t duration = 0;
        std::string cutError;
        Check(!clipfile::Decode(cut.data(), cut.size(), kBase, kImageSize, refused.recording, refused.poses, refused.camera, duration,
                  cutError) && cutError.find("water gpu") != std::string::npos,
            "retired water gpu: a WGPU cut short is a truncated clip");
    }

    void TestVehicleWindowsSection()
    {
        Clip clip;
        MakeClip(clip);
        const ReplayId vehicle = clip.recording.created[0].id;
        clip.recording.vehicleWindows.push_back({vehicle.Packed(), {{100u, std::uint8_t{0x0}}, {400u, std::uint8_t{0x2}}, {900u, std::uint8_t{0xA}}}, 0, false});
        {
            const WindowTrack& track = clip.recording.vehicleWindows[0];
            Check(BrokenWindowsAt(track, 0) == 0 && BrokenWindowsAt(track, 399) == 0 && BrokenWindowsAt(track, 400) == 0x2 &&
                    BrokenWindowsAt(track, 5000) == 0xA,
                "windows: mask at the playhead");
            const WindowTrack glassless{vehicle.Packed(), {{100u, std::uint8_t{0x8}}}, 0, false};
            Check(BrokenWindowsAt(glassless, 0) == 0x8, "windows: the first read holds before it");
        }
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "windows: encode");
        Clip loaded;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok, "windows: a clip with window tracks loads");
        if (ok)
        {
            const Recording& r = loaded.recording;
            Check(r.vehicleWindows.size() == 1 && r.vehicleWindows[0].id == vehicle.Packed() &&
                    r.vehicleWindows[0].states.size() == 3 && r.vehicleWindows[0].states[1].first == 400 &&
                    r.vehicleWindows[0].states[2].second == 0xA,
                "windows: track round trip");
        }
        const std::vector<std::uint8_t> old = WithoutSection(bytes, "vehicle windows");
        Check(!old.empty(), "windows: clip without the section built");
        if (!old.empty())
        {
            Clip older;
            const bool oldOk = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, error);
            Check(oldOk && older.recording.vehicleWindows.empty(), "windows: an older clip without the section loads with no tracks");
        }
        {
            std::vector<std::uint8_t> bad = bytes;
            const std::size_t at = FindSection(bad, "vehicle windows");
            if (at < bad.size())
            {
                std::uint64_t length = 0;
                std::memcpy(&length, bad.data() + at + 6, 8);
                std::uint8_t* payload = bad.data() + at + 14;
                payload[length - 1] = 0x10; // the last state's mask: window 4
                const std::uint64_t hash = TestFnv64(payload, static_cast<std::size_t>(length));
                std::memcpy(payload + length, &hash, sizeof(hash));
                std::string badError;
                Check(DecodeSkips(bad, "vehicle windows", badError),
                    "windows: mask past window 3 refused");
            }
        }
    }

    // The lost wheel and door masks (VPRT): round trip, a clip without the
    // section (older) loads with none, states out of order are refused; the
    // lookup holds the first read before it.
    // A vehicle piece prop keeps its origin and group (boneCount) through the
    // file.
    void TestVehiclePieceCreate()
    {
        Clip clip;
        MakeClip(clip);
        clip.recording.created[2].propOrigin = entity::PropOrigin::VehiclePiece;
        clip.recording.created[2].boneCount = 97;
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "piece: encode");
        Clip loaded;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok, "piece: a clip with a vehicle piece loads");
        if (ok)
            Check(loaded.recording.created.size() == 3 &&
                    loaded.recording.created[2].propOrigin == entity::PropOrigin::VehiclePiece &&
                    loaded.recording.created[2].boneCount == 97 &&
                    loaded.recording.created[2].owner == clip.recording.created[2].owner,
                "piece: origin, group and vehicle round trip");
    }

    void TestVehiclePartsSection()
    {
        Clip clip;
        MakeClip(clip);
        const ReplayId vehicle = clip.recording.created[0].id;
        const std::uint16_t doorTwo = static_cast<std::uint16_t>(1u << (kVehiclePartDoorShift + 2));
        clip.recording.vehicleParts.push_back({vehicle.Packed(),
            {{100u, std::uint16_t{0x0}}, {400u, std::uint16_t{0x10}}, {900u, static_cast<std::uint16_t>(0x30 | doorTwo)}}, 0, false});
        {
            const VehiclePartTrack& track = clip.recording.vehicleParts[0];
            Check(BrokenPartsAt(track, 0) == 0 && BrokenPartsAt(track, 399) == 0 && BrokenPartsAt(track, 400) == 0x10 &&
                    BrokenPartsAt(track, 5000) == (0x30 | doorTwo),
                "parts: mask at the playhead");
            const VehiclePartTrack entered{vehicle.Packed(), {{100u, std::uint16_t{0x1}}}, 0, false};
            Check(BrokenPartsAt(entered, 0) == 0x1, "parts: the first read holds before it");
        }
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "parts: encode");
        Clip loaded;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok, "parts: a clip with part tracks loads");
        if (ok)
        {
            const Recording& r = loaded.recording;
            Check(r.vehicleParts.size() == 1 && r.vehicleParts[0].id == vehicle.Packed() &&
                    r.vehicleParts[0].states.size() == 3 && r.vehicleParts[0].states[1].first == 400 &&
                    r.vehicleParts[0].states[2].second == (0x30 | doorTwo),
                "parts: track round trip");
        }
        const std::vector<std::uint8_t> old = WithoutSection(bytes, "vehicle parts");
        Check(!old.empty(), "parts: clip without the section built");
        if (!old.empty())
        {
            Clip older;
            const bool oldOk = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, error);
            Check(oldOk && older.recording.vehicleParts.empty(), "parts: an older clip without the section loads with no tracks");
        }
        {
            std::vector<std::uint8_t> bad = bytes;
            const std::size_t at = FindSection(bad, "vehicle parts");
            if (at < bad.size())
            {
                std::uint64_t length = 0;
                std::memcpy(&length, bad.data() + at + 6, 8);
                std::uint8_t* payload = bad.data() + at + 14;
                // The last state's time (6 bytes before the end: u32 time,
                // u16 mask) goes back before the one ahead of it.
                const std::uint32_t early = 50u;
                std::memcpy(payload + length - 6, &early, sizeof(early));
                const std::uint64_t hash = TestFnv64(payload, static_cast<std::size_t>(length));
                std::memcpy(payload + length, &hash, sizeof(hash));
                std::string badError;
                Check(DecodeSkips(bad, "vehicle parts", badError),
                    "parts: states out of order refused");
            }
        }
    }

    // The door ratios (VDOR): round trip, left out when empty, a clip without
    // it loads with none, a bad ratio is refused (the section skipped, the clip
    // kept).
    void TestVehicleDoorsSection()
    {
        Clip clip;
        MakeClip(clip);
        const ReplayId vehicle = clip.recording.created[0].id;
        clip.recording.vehicleDoors.clear();
        {
            std::vector<std::uint8_t> bytes;
            std::string error;
            Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error) &&
                    FindSection(bytes, "vehicle doors") >= bytes.size(),
                "doors: no track, no section");
        }
        vehdoors::DoorTrack track;
        track.id = vehicle.Packed();
        track.samples = {{100u, 0, 0.0f}, {400u, 0, 0.25f}, {900u, 0, 1.0f}, {900u, vehdoors::kLatchChannel, 1.0f}};
        clip.recording.vehicleDoors.push_back(track);
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "doors: encode");
        Clip loaded;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok, "doors: a clip with door tracks loads");
        if (ok)
        {
            const Recording& r = loaded.recording;
            Check(r.vehicleDoors.size() == 1 && r.vehicleDoors[0].id == vehicle.Packed() &&
                    r.vehicleDoors[0].samples.size() == 4 && r.vehicleDoors[0].samples[1].timeMs == 400 &&
                    r.vehicleDoors[0].samples[1].ratio == 0.25f &&
                    r.vehicleDoors[0].samples[3].door == vehdoors::kLatchChannel,
                "doors: track round trip");
        }
        const std::vector<std::uint8_t> old = WithoutSection(bytes, "vehicle doors");
        Check(!old.empty(), "doors: clip without the section built");
        if (!old.empty())
        {
            Clip older;
            const bool oldOk = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, error);
            Check(oldOk && older.recording.vehicleDoors.empty(), "doors: an older clip without the section loads with no tracks");
        }
        {
            std::vector<std::uint8_t> bad = bytes;
            const std::size_t at = FindSection(bad, "vehicle doors");
            if (at < bad.size())
            {
                std::uint64_t length = 0;
                std::memcpy(&length, bad.data() + at + 6, 8);
                std::uint8_t* payload = bad.data() + at + 14;
                // The last sample's ratio (the payload's last 4 bytes) out of 0..1.
                const float wrong = 2.0f;
                std::memcpy(payload + length - 4, &wrong, sizeof(wrong));
                const std::uint64_t hash = TestFnv64(payload, static_cast<std::size_t>(length));
                std::memcpy(payload + length, &hash, sizeof(hash));
                std::string badError;
                Check(DecodeSkips(bad, "vehicle doors", badError), "doors: a ratio past 1 refused");
            }
            else
                Check(false, "doors: section found");
        }
    }

    // The vehicle burnt states (VBRN): round trip, a clip without the section
    // (older) loads with none, a bad state is refused.
    // Vehicle looks version 2 carry the latest tint and livery; a version 1
    // payload (no tint, no livery) loads them as kLookUnset.
    void TestVehicleLooksSection()
    {
        Clip clip;
        MakeClip(clip);
        const ReplayId vehicle = clip.recording.created[0].id;
        VehicleLook look;
        look.id = vehicle;
        look.propSet = 0x1234u;
        look.lightPropSet = 0x3c7bf340u;
        look.extrasExist = 0x6u;
        look.extrasOn = 0x4u;
        look.tint = 7;
        look.livery = -1;
        clip.recording.vehicleLooks.push_back(look);
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "looks: encode");
        Clip loaded;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok, "looks: a clip with vehicle looks loads");
        if (ok)
        {
            const auto& looks = loaded.recording.vehicleLooks;
            Check(looks.size() == 1 && looks[0].id == vehicle && looks[0].propSet == 0x1234u &&
                    looks[0].lightPropSet == 0x3c7bf340u && looks[0].extrasExist == 0x6u && looks[0].extrasOn == 0x4u &&
                    looks[0].tint == 7 && looks[0].livery == -1,
                "looks: round trip with tint and livery");
        }
        const std::vector<std::uint8_t> old = WithOlderSection(bytes, "vehicle looks", 1, 8);
        Check(!old.empty(), "looks: v1 payload built");
        if (!old.empty())
        {
            Clip older;
            const bool oldOk = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, error);
            Check(oldOk && older.recording.vehicleLooks.size() == 1 && older.recording.vehicleLooks[0].extrasOn == 0x4u &&
                    older.recording.vehicleLooks[0].tint == kLookUnset && older.recording.vehicleLooks[0].livery == kLookUnset,
                "looks: a v1 section loads with tint and livery unset");
        }
    }

    // SPRQ v2 carries each line's sound set; a v1 section still loads,
    // without a set; a future version is refused.
    void TestSpeechRequestsSoundSet()
    {
        Clip clip;
        MakeClip(clip);
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "sprq: encode");
        std::uint64_t duration = 0;
        Clip loaded;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok && loaded.recording.speechLines.Events().size() == 1 &&
                  loaded.recording.speechLines.Events()[0].request.setRead &&
                  loaded.recording.speechLines.Events()[0].request.setHash == 0xCAFEF00Du &&
                  loaded.recording.speechLines.Events()[0].request.setRef == 0x438F79u,
            "sprq: v2 round trip with the sound set");
        std::vector<std::uint8_t> v1;
        auto put = [&v1](const void* data, std::size_t size) {
            v1.insert(v1.end(), static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
        };
        const std::uint16_t fields = static_cast<std::uint16_t>(replay::speech::request::kFieldCount);
        const std::uint64_t rows = 1;
        const std::uint8_t read = 1;
        put(&fields, sizeof(fields));
        put(&rows, sizeof(rows));
        put(&read, sizeof(read));
        for (std::uint32_t f = 0; f < fields; ++f)
        {
            const std::uint32_t value = 0x200u + f;
            put(&value, sizeof(value));
        }
        const std::vector<std::uint8_t> old = WithSectionPayload(bytes, "speech requests", 1, v1);
        Check(!old.empty(), "sprq: v1 payload built");
        if (!old.empty())
        {
            Clip older;
            const bool oldOk = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, error);
            Check(oldOk, "sprq: a v1 section loads");
            if (oldOk)
            {
                const replay::speech::request::Fields& f = older.recording.speechLines.Events()[0].request;
                Check(f.read && f.value[0] == 0x200u && f.value[replay::speech::request::LineBits] == 0x200u + 15u &&
                          !f.setRead && f.setHash == 0 && f.setRef == 0,
                    "sprq: v1 fields kept, no sound set");
            }
        }
        const std::vector<std::uint8_t> future = WithOlderSection(bytes, "speech requests", 3, 0);
        Check(!future.empty() && DecodeFails(future, error), "sprq: a newer version is refused");
    }

    void TestVehicleBurntSection()
    {
        Clip clip;
        MakeClip(clip);
        const ReplayId vehicle = clip.recording.created[0].id;
        clip.recording.vehicleBurnt.push_back({vehicle.Packed(), {{2974u, true}}, 0, false});
        Check(!ScorchedAt(clip.recording.vehicleBurnt[0], 2973) && ScorchedAt(clip.recording.vehicleBurnt[0], 2974),
            "burnt: state at the playhead");
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "burnt: encode");
        Clip loaded;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok, "burnt: a clip with burnt tracks loads");
        if (ok)
        {
            const Recording& r = loaded.recording;
            Check(r.vehicleBurnt.size() == 1 && r.vehicleBurnt[0].id == vehicle.Packed() &&
                    r.vehicleBurnt[0].states.size() == 1 && r.vehicleBurnt[0].states[0].first == 2974 &&
                    r.vehicleBurnt[0].states[0].second,
                "burnt: track round trip");
        }
        const std::vector<std::uint8_t> old = WithoutSection(bytes, "vehicle burnt");
        Check(!old.empty(), "burnt: clip without the section built");
        if (!old.empty())
        {
            Clip older;
            const bool oldOk = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, error);
            Check(oldOk && older.recording.vehicleBurnt.empty(), "burnt: an older clip without the section loads with no tracks");
        }
        {
            std::vector<std::uint8_t> bad = bytes;
            const std::size_t at = FindSection(bad, "vehicle burnt");
            if (at < bad.size())
            {
                std::uint64_t length = 0;
                std::memcpy(&length, bad.data() + at + 6, 8);
                std::uint8_t* payload = bad.data() + at + 14;
                // The last byte is the state (0 or 1).
                payload[length - 1] = 2;
                const std::uint64_t hash = TestFnv64(payload, static_cast<std::size_t>(length));
                std::memcpy(payload + length, &hash, sizeof(hash));
                std::string badError;
                Check(DecodeSkips(bad, "vehicle burnt", badError),
                    "burnt: a state other than 0 or 1 refused");
            }
        }
    }

    // The appearance changes (APPC) are optional: round trip, a clip from
    // before them loads with none, changes out of order refused.
    void TestAppearanceSection()
    {
        Clip clip;
        MakeClip(clip);
        const ReplayId ped = clip.recording.created[0].id;
        AppearanceTrack track;
        track.id = ped.Packed();
        entity::Appearance first;
        first.outfit = 0x1234u;
        first.assets.push_back({0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 1, 2, 3});
        first.assets.push_back({0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 4, 5, 6});
        entity::Appearance second;
        second.assets.push_back({0x31u, 0x32u, 0x33u, 0x34u, 0x35u, 7, 8, 9});
        track.changes.push_back({250, first});
        track.changes.push_back({700, second});
        clip.recording.appearances.push_back(track);
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error),
            "appearance: encode");
        Clip loaded;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok, "appearance: a clip with appearance changes loads");
        if (ok)
        {
            const std::vector<AppearanceTrack>& r = loaded.recording.appearances;
            Check(r.size() == 1 && r[0].id == ped.Packed() && r[0].changes.size() == 2 && r[0].changes[0].timeMs == 250 &&
                    r[0].changes[0].appearance.outfit == 0x1234u && r[0].changes[0].appearance.assets.size() == 2 &&
                    r[0].changes[0].appearance.assets[1].material == 0x24u && r[0].changes[0].appearance.assets[1].tint2 == 6 &&
                    r[0].changes[1].timeMs == 700 && r[0].changes[1].appearance.assets[0].drawable == 0x31u,
                "appearance: track round trip");
        }
        const std::vector<std::uint8_t> old = WithoutSection(bytes, "appearance changes");
        Check(!old.empty(), "appearance: clip without the section built");
        if (!old.empty())
        {
            Clip older;
            const bool oldOk = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, error);
            Check(oldOk && older.recording.appearances.empty(), "appearance: an older clip without the section loads with no changes");
        }
        {
            Clip bad = clip;
            std::swap(bad.recording.appearances[0].changes[0].timeMs, bad.recording.appearances[0].changes[1].timeMs);
            std::vector<std::uint8_t> badBytes;
            clipfile::Encode(bad.recording, bad.poses, bad.camera, 1000, kBase, kImageSize, badBytes, error);
            std::string badError;
            Check(DecodeSkips(badBytes, "appearance changes", badError),
                "appearance: changes out of order refused");
        }
    }

    // The metaped section (MPED) is optional: round trip, a clip from before it
    // loads with none, states out of order and expressions out of order
    // refused.
    void TestMetapedSection()
    {
        Clip clip;
        MakeClip(clip);
        const ReplayId ped = clip.recording.created[0].id;
        metaped::Track track;
        track.id = ped.Packed();
        metaped::State first;
        first.timeMs = 100;
        first.source = metaped::Source::Create;
        first.flags = metaped::kHasExpressions | metaped::kHasScale | metaped::kHasCleanliness;
        first.outfit = 0;
        first.metapedType = 0;
        first.layerCount = 11;
        first.layersPrint = 0x1122334455667788ull;
        first.scale = 0.96f;
        first.cleanliness = 1;
        first.expressions = {{0x0012, 0.25f}, {0x84D6, -0.5f}, {0xF065, 1.0f}};
        metaped::State second = first;
        second.timeMs = 39618;
        second.source = metaped::Source::Segment;
        second.expressions = {{0x0012, 0.3f}};
        track.states = {first, second};
        clip.recording.metapeds.push_back(track);
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error),
            "metaped: encode");
        Clip loaded;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok, "metaped: a clip with the section loads");
        if (ok)
        {
            const std::vector<metaped::Track>& r = loaded.recording.metapeds;
            Check(r.size() == 1 && r[0].id == ped.Packed() && r[0].states.size() == 2 && r[0].states[0].timeMs == 100 &&
                    r[0].states[0].layerCount == 11 && r[0].states[0].layersPrint == 0x1122334455667788ull &&
                    r[0].states[0].scale == 0.96f && r[0].states[0].cleanliness == 1 &&
                    r[0].states[0].expressions.size() == 3 && r[0].states[0].expressions[1].id == 0x84D6 &&
                    r[0].states[0].expressions[1].value == -0.5f && r[0].states[1].source == metaped::Source::Segment &&
                    r[0].states[1].expressions.size() == 1,
                "metaped: track round trip");
        }
        const std::vector<std::uint8_t> old = WithoutSection(bytes, "metaped");
        Check(!old.empty(), "metaped: clip without the section built");
        if (!old.empty())
        {
            Clip older;
            const bool oldOk = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, error);
            Check(oldOk && older.recording.metapeds.empty(), "metaped: a clip from before it loads with no state (layers only)");
        }
        {
            Clip bad = clip;
            std::swap(bad.recording.metapeds[0].states[0].timeMs, bad.recording.metapeds[0].states[1].timeMs);
            std::vector<std::uint8_t> badBytes;
            clipfile::Encode(bad.recording, bad.poses, bad.camera, 1000, kBase, kImageSize, badBytes, error);
            std::string badError;
            Check(DecodeSkips(badBytes, "metaped", badError), "metaped: states out of order refused");
        }
        {
            Clip bad = clip;
            std::swap(bad.recording.metapeds[0].states[0].expressions[0], bad.recording.metapeds[0].states[0].expressions[2]);
            std::vector<std::uint8_t> badBytes;
            clipfile::Encode(bad.recording, bad.poses, bad.camera, 1000, kBase, kImageSize, badBytes, error);
            std::string badError;
            Check(DecodeSkips(badBytes, "metaped", badError), "metaped: expressions out of order refused");
        }
        // MPED v2 carries the shop items per state, saved as v2 only with an
        // item (a clip without them opens in an older build), v1 still read
        // (no items).
        const auto versionOf = [](const std::vector<std::uint8_t>& data) {
            const std::size_t at = FindSection(data, "metaped");
            std::uint16_t version = 0;
            if (at < data.size())
                std::memcpy(&version, data.data() + at + 4, sizeof(version));
            return version;
        };
        Check(versionOf(bytes) == 1, "metaped: no shop item, saved as v1");
        {
            Clip shop = clip;
            metaped::State& create = shop.recording.metapeds[0].states[0];
            create.flags |= metaped::kHasShopItems;
            create.shopItems = {{0x1A2B3C4Du, 0u, 0u}, {0x5E6F7081u, 0x91A2B3C4u, metaped::kShopItemStatus}};
            shop.recording.metapeds[0].states[1].flags |= metaped::kHasShopItems; // read, none
            std::vector<std::uint8_t> shopBytes;
            Check(clipfile::Encode(shop.recording, shop.poses, shop.camera, 1000, kBase, kImageSize, shopBytes, error),
                "metaped v2: encode");
            Check(versionOf(shopBytes) == 2, "metaped v2: a state with shop items saved as v2");
            Clip back;
            const bool shopOk = clipfile::Decode(shopBytes.data(), shopBytes.size(), kBase, kImageSize, back.recording,
                back.poses, back.camera, duration, error);
            const std::vector<metaped::Track>& r = back.recording.metapeds;
            Check(shopOk && r.size() == 1 && r[0].states.size() == 2 && r[0].states[0].shopItems.size() == 2 &&
                    r[0].states[0].shopItems[1].item == 0x5E6F7081u && r[0].states[0].shopItems[1].state == 0x91A2B3C4u &&
                    r[0].states[0].shopItems[1].flags == metaped::kShopItemStatus &&
                    r[0].states[0].shopItems[0].state == 0 && (r[0].states[1].flags & metaped::kHasShopItems) &&
                    r[0].states[1].shopItems.empty() && r[0].states[0].expressions.size() == 3,
                "metaped v2: shop items round trip");
            Clip bad = shop;
            bad.recording.metapeds[0].states[0].shopItems[0].item = 0;
            std::vector<std::uint8_t> badBytes;
            clipfile::Encode(bad.recording, bad.poses, bad.camera, 1000, kBase, kImageSize, badBytes, error);
            std::string badError;
            Check(DecodeSkips(badBytes, "metaped", badError), "metaped v2: a shop item 0 refused");
        }
        {
            Clip v1;
            const bool v1Ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, v1.recording, v1.poses,
                v1.camera, duration, error);
            Check(v1Ok && v1.recording.metapeds.size() == 1 && v1.recording.metapeds[0].states[0].shopItems.empty(),
                "metaped v1: still read, no shop items (the layer path)");
        }
    }

    // The world info (WINF) and world (WRLD) sections are optional: a clip from
    // before them loads with no map state; events out of order or an entity set
    // on a missing interior are refused; the clip list's summary carries the
    // world info.
    void TestWorldSections()
    {
        Clip clip;
        MakeClip(clip);
        WorldState& world = clip.recording.world;
        world.recorded = true;
        world.camp = 3;
        world.chapter = 4;
        world.flags = world::kFlagCampRead | world::kFlagCampConfirmed;
        world.candidateListHash = 0x1234ABCDu;
        world.candidateCount = 1996;
        world.ipls = {{0, 0x7F6C18B1u, true}, {0, 0x10u, true}, {400, 0x7F6C18B1u, false}, {900, 0x20u, true}};
        world.spheres = {{0x10u, {1.0f, 2.0f, 3.0f}, 40.0f}, {0x20u, {4.0f, 5.0f, 6.0f}, 10.0f}};
        world.interiors = {{0xBEEFu, {-10.0f, 20.0f, 30.0f}}};
        EntitySetChange set;
        set.timeMs = 0;
        set.interior = 0;
        set.active = true;
        std::snprintf(set.name, sizeof(set.name), "%s", "bee_01_house_stage06");
        world.entitySets.push_back(set);
        set.timeMs = 700;
        set.active = false;
        world.entitySets.push_back(set);
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "world: encode");
        Clip loaded;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok, "world: a clip with the map state loads");
        if (ok)
        {
            const WorldState& w = loaded.recording.world;
            Check(w.recorded && w.camp == 3 && w.chapter == 4 && w.flags == world.flags && w.candidateListHash == 0x1234ABCDu &&
                    w.candidateCount == 1996,
                "world: info round trip");
            Check(w.ipls.size() == 4 && w.ipls[2].timeMs == 400 && w.ipls[2].hash == 0x7F6C18B1u && !w.ipls[2].active &&
                    w.ipls[3].active,
                "world: IPL changes round trip");
            Check(w.spheres.size() == 2 && w.spheres[1].center[2] == 6.0f && w.spheres[0].radius == 40.0f,
                "world: spheres round trip");
            Check(w.interiors.size() == 1 && w.interiors[0].nameHash == 0xBEEFu && w.interiors[0].position[0] == -10.0f,
                "world: interiors round trip");
            Check(w.entitySets.size() == 2 && std::strcmp(w.entitySets[1].name, "bee_01_house_stage06") == 0 &&
                    w.entitySets[1].timeMs == 700 && !w.entitySets[1].active && w.entitySets[0].active,
                "world: entity sets round trip");
            Check(world::Fingerprint(w) == world::Fingerprint(world), "world: same fingerprint after the round trip");
        }
        // The clip list's summary holds the world info (the gallery's chapter).
        {
            std::vector<std::uint8_t> summary;
            Check(clipcompress::Summary(bytes.data(), bytes.size(), summary) && FindSection(summary, "world info") < summary.size(),
                "world: the list summary carries the world info");
            const std::size_t at = FindSection(summary, "world info");
            if (at < summary.size())
            {
                std::uint64_t length = 0;
                std::memcpy(&length, summary.data() + at + 6, 8);
                const std::uint8_t* payload = summary.data() + at + 14;
                Check(length == 20 && payload[0] == 1 && payload[1] == 3 && payload[2] == 4,
                    "world: world info layout (recorded, camp, chapter) as the clip list reads it");
            }
        }
        // Older clip: neither section.
        std::vector<std::uint8_t> old = WithoutSection(bytes, "world info");
        old = old.empty() ? old : WithoutSection(old, "world");
        Check(!old.empty(), "world: clip without the new sections built");
        if (!old.empty())
        {
            Clip older;
            const bool oldOk = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, error);
            Check(oldOk && !older.recording.world.recorded && older.recording.world.ipls.empty() &&
                    older.recording.world.chapter == world::kChapterUnknown,
                "world: an older clip without the section loads with no map state");
        }
        {
            Clip bad = clip;
            std::swap(bad.recording.world.ipls[2], bad.recording.world.ipls[3]);
            std::vector<std::uint8_t> badBytes;
            clipfile::Encode(bad.recording, bad.poses, bad.camera, 1000, kBase, kImageSize, badBytes, error);
            std::string badError;
            Check(DecodeSkips(badBytes, "world", badError),
                "world: IPL changes out of order refused");
        }
        {
            Clip bad = clip;
            bad.recording.world.entitySets[0].interior = 5;
            std::vector<std::uint8_t> badBytes;
            clipfile::Encode(bad.recording, bad.poses, bad.camera, 1000, kBase, kImageSize, badBytes, error);
            std::string badError;
            Check(DecodeSkips(badBytes, "world", badError),
                "world: an entity set on a missing interior refused");
        }
        {
            Clip bad = clip;
            bad.recording.world.chapter = 9;
            std::vector<std::uint8_t> badBytes;
            clipfile::Encode(bad.recording, bad.poses, bad.camera, 1000, kBase, kImageSize, badBytes, error);
            std::string badError;
            Check(DecodeSkips(badBytes, "world info", badError),
                "world: a chapter past the epilogues refused");
        }
    }

    // The anim audio section (AANI) is optional: a clip from before it loads
    // with no events; events out of order are refused.
    void TestAnimAudioSection()
    {
        Clip clip;
        MakeClip(clip);
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error),
            "anim audio: encode");
        Check(!clip.recording.animAudio.Events().empty(), "anim audio: the test clip has events");
        std::uint64_t duration = 0;
        const std::vector<std::uint8_t> old = WithoutSection(bytes, "anim audio");
        Check(!old.empty() && old.size() < bytes.size(), "anim audio: clip without the section built");
        if (!old.empty())
        {
            Clip older;
            const bool ok = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, error);
            Check(ok && older.recording.animAudio.Events().empty() && older.recording.footsteps.Events().size() ==
                    clip.recording.footsteps.Events().size(),
                "anim audio: an older clip without the section loads with no events");
        }
        {
            std::vector<std::uint8_t> bad = bytes;
            const std::size_t at = FindSection(bad, "anim audio");
            Check(at < bad.size(), "anim audio: section found");
            if (at < bad.size())
            {
                std::uint64_t length = 0;
                std::memcpy(&length, bad.data() + at + 6, 8);
                std::uint8_t* payload = bad.data() + at + 14;
                // Count (8 bytes, Writer::Count), then 16 bytes per event: the
                // second event's time goes before the first's.
                const std::uint32_t early = 10;
                if (length >= 8 + 32)
                    std::memcpy(payload + 8 + 16, &early, sizeof(early));
                const std::uint64_t hash = TestFnv64(payload, static_cast<std::size_t>(length));
                std::memcpy(payload + length, &hash, sizeof(hash));
                std::string badError;
                Check(DecodeSkips(bad, "anim audio", badError),
                    "anim audio: events out of order refused");
            }
        }
    }

    // The fire records (FREC): the state at the playhead, round trip, a clip
    // without the section loads with none, a value that is not a number is
    // refused.
    void TestFireCharsSection()
    {
        Clip clip;
        MakeClip(clip);
        const ReplayId vehicle = clip.recording.created[0].id;
        FireCharState none;
        FireCharState burnt;
        burnt.present = true;
        burnt.flags = 0x26;
        std::array<float, kFireCharFloats> volume{};
        volume[0] = 2.96f;
        volume[12] = 1.0f;
        burnt.volumes.push_back(volume);
        FireCharAnchor canvas;
        canvas.component = 0x77;
        canvas.sub = 0xFFFF;
        canvas.flags = 0x0a;
        canvas.mask = 0x112f;
        canvas.dir[0] = 0.25f;
        burnt.anchors.push_back(canvas);
        clip.recording.fireChars.push_back({vehicle.Packed(), {{100u, none}, {400u, burnt}}, 0, false});
        {
            const FireCharTrack& track = clip.recording.fireChars[0];
            Check(!FireCharAt(track, 0)->present && !FireCharAt(track, 399)->present && FireCharAt(track, 400)->present &&
                    FireCharAt(track, 5000)->volumes.size() == 1,
                "fire char: state at the playhead, both directions");
            const FireCharTrack empty{vehicle.Packed(), {}, 0, false};
            Check(FireCharAt(empty, 0) == nullptr, "fire char: an empty track has no state");
            FireCharState closeState = burnt;
            closeState.volumes[0][0] += kFireCharStep * 0.5f;
            FireCharState farState = burnt;
            farState.volumes[0][12] = 0.5f;
            Check(FireCharSame(burnt, closeState) && !FireCharSame(burnt, farState) && !FireCharSame(burnt, none),
                "fire char: change test");
            // Which puppets keep their record through the rebuild.
            std::vector<FireCharTrack> tracks = clip.recording.fireChars;
            tracks.push_back({vehicle.Packed() + 1u, {}, 0, false});
            Check(FireCharKept(tracks, vehicle.Packed()) && !FireCharKept(tracks, vehicle.Packed() + 1u) &&
                    !FireCharKept(tracks, vehicle.Packed() + 2u),
                "fire char: kept only with a track that has states");
            // The record kept through a rebuild depends on the playhead only
            // (idempotent scrub): kept once a fire on the entity started,
            // burning or ended.
            const std::vector<std::pair<std::uint32_t, std::uint32_t>> spans = {{1000u, 5000u}, {3000u, 0xFFFFFFFFu}};
            Check(!FireRecordKeptAt(spans, 500) && !FireRecordKeptAt(spans, 999) && FireRecordKeptAt(spans, 1000) &&
                    FireRecordKeptAt(spans, 4000) && FireRecordKeptAt(spans, 90000),
                "fire record: kept from the first fire on it, burning or not");
            bool same = true;
            for (std::uint32_t t : {6000u, 800u, 6000u, 1500u, 6000u, 800u})
                same = same && FireRecordKeptAt(spans, t) == (t >= 1000u);
            Check(same, "fire record: back and forth, the same playhead gives the same answer");
            std::vector<std::pair<std::uint32_t, std::uint32_t>> reversed(spans.rbegin(), spans.rend());
            Check(FireRecordKeptAt(reversed, 1000) && !FireRecordKeptAt(reversed, 999), "fire record: order of the fires does not matter");
            // Parts. Recorded canvas (0x77) and body (0) against a puppet with
            // body, a wheel (0xd) and two volumes on one part.
            const auto part = [](std::int32_t component, std::uint16_t sub) {
                FireCharAnchor anchor;
                anchor.component = component;
                anchor.sub = sub;
                return anchor;
            };
            const std::vector<FireCharAnchor> recordedParts = {part(0, 0xFFFF), part(0x77, 0xFFFF), part(-1, 1), part(-1, 0)};
            const std::vector<FireCharAnchor> puppetParts = {part(0xd, 0xFFFF), part(-1, 0), part(0, 0xFFFF), part(0, 0xFFFF)};
            const std::vector<int> match = MatchFireCharVolumes(recordedParts, puppetParts);
            Check(match.size() == 4 && match[0] == 2 && match[1] == -1 && match[2] == -1 && match[3] == 1,
                "fire char: each recorded part to the puppet's same part, missing parts -1");
            Check(MatchFireCharVolumes(recordedParts, puppetParts) == match, "fire char: the match is the same every pass");
            const std::vector<FireCharAnchor> sameParts = {part(0, 0xFFFF), part(0, 0xFFFF), part(0, 0xFFFF)};
            const std::vector<int> used = MatchFireCharVolumes(sameParts, puppetParts);
            Check(used[0] == 2 && used[1] == 3 && used[2] == -1, "fire char: a puppet volume takes one recorded volume only");
            Check(!FireRecordKeptAt({}, 5000), "fire record: an entity no fire touched keeps none");
            replay::burn::CharVolume extra;
            for (std::size_t k = 0; k < replay::burn::kCharFloats; ++k)
                extra.v[k] = 2.5f;
            replay::burn::NeutralizeVolume(extra);
            replay::burn::CharVolume twice = extra;
            replay::burn::NeutralizeVolume(twice);
            Check(extra.v[0] < 0.01f && extra.v[11] < 0.01f && extra.v[16] < 0.01f && extra.v[12] == 0.0f &&
                    extra.v[15] == 0.0f && extra.v[7] == 2.5f && extra.v[22] == 2.5f &&
                    std::memcmp(extra.v, twice.v, sizeof(extra.v)) == 0,
                "fire char: an extra puppet volume draws nothing, keeps its timers, same after a second pass");
        }
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error),
            "fire char: encode");
        Clip loaded;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok, "fire char: a clip with fire record tracks loads");
        if (ok)
        {
            const Recording& r = loaded.recording;
            Check(r.fireChars.size() == 1 && r.fireChars[0].states.size() == 2 && !r.fireChars[0].states[0].second.present &&
                    r.fireChars[0].states[1].second.flags == 0x26 && r.fireChars[0].states[1].second.volumes.size() == 1 &&
                    r.fireChars[0].states[1].second.volumes[0][12] == 1.0f &&
                    r.fireChars[0].states[1].second.anchors.size() == 1 && r.fireChars[0].states[1].second.anchors[0].component == 0x77 &&
                    r.fireChars[0].states[1].second.anchors[0].sub == 0xFFFF && r.fireChars[0].states[1].second.anchors[0].flags == 0x0a &&
                    r.fireChars[0].states[1].second.anchors[0].mask == 0x112f && r.fireChars[0].states[1].second.anchors[0].dir[0] == 0.25f,
                "fire char: track round trip (with the parts)");
        }
        const std::vector<std::uint8_t> old = WithoutSection(bytes, "fire chars");
        Check(!old.empty(), "fire char: clip without the section built");
        if (!old.empty())
        {
            Clip older;
            const bool oldOk = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, error);
            Check(oldOk && older.recording.fireChars.empty(), "fire char: an older clip without the section loads with no tracks");
        }
    }

    // The burn levels (BLVL): the level at the playhead, round trip, a clip
    // without the section (older) loads with none, states out of order and a
    // level that is not a number are refused.
    void TestBurnLevelsSection()
    {
        Clip clip;
        MakeClip(clip);
        const ReplayId vehicle = clip.recording.created[0].id;
        clip.recording.burnLevels.push_back({vehicle.Packed(), {{100u, 0.0f}, {400u, 0.35f}, {900u, 0.8f}}, 0, false});
        {
            const BurnLevelTrack& track = clip.recording.burnLevels[0];
            Check(BurnLevelAt(track, 0) == 0.0f && BurnLevelAt(track, 399) == 0.0f && BurnLevelAt(track, 400) == 0.35f &&
                    BurnLevelAt(track, 5000) == 0.8f && BurnLevelAt(track, 450) == 0.35f,
                "burn level: level at the playhead, both directions");
            const BurnLevelTrack entered{vehicle.Packed(), {{100u, 0.5f}}, 0, false};
            Check(BurnLevelAt(entered, 0) == 0.5f, "burn level: the first read holds before it");
            const BurnLevelTrack none{vehicle.Packed(), {}, 0, false};
            Check(BurnLevelAt(none, 0) == 0.0f, "burn level: an empty track reads 0");
        }
        {
            // What the object sampler stores (cooking meat: 0 at the create,
            // then rising to 1) and the puppets the level is written on.
            BurnLevelTrack meat{vehicle.Packed(), {}, 0, false};
            Check(NoteBurnLevel(meat, 100, 0.0f) && meat.states.size() == 1 && meat.checked && meat.lastCheckMs == 100,
                "burn level: the first read is stored");
            Check(!NoteBurnLevel(meat, 350, 0.01f) && meat.states.size() == 1 && meat.lastCheckMs == 350,
                "burn level: a move under the step is not stored");
            Check(NoteBurnLevel(meat, 600, 0.02f) && NoteBurnLevel(meat, 850, 0.5f) && meat.states.size() == 3,
                "burn level: a move of the step or more is stored");
            Check(NoteBurnLevel(meat, 1100, 0.99f) && meat.states.size() == 4, "burn level: a big move stored");
            Check(NoteBurnLevel(meat, 1350, 1.0f) && meat.states.back().second == 1.0f,
                "burn level: the end of the cook (1) is stored even under the step");
            Check(!NoteBurnLevel(meat, 1600, 1.0f), "burn level: the same end is not stored twice");
            Check(NoteBurnLevel(meat, 1850, 0.0f) && meat.states.back().second == 0.0f, "burn level: back to 0 is stored");
            Check(!NoteBurnLevel(meat, 2100, std::numeric_limits<float>::quiet_NaN()) &&
                    !NoteBurnLevel(meat, 2350, std::numeric_limits<float>::infinity()) && meat.states.back().second == 0.0f,
                "burn level: a value that is not a number is never stored");
            Check(!NoteBurnLevel(meat, 50, 0.7f) && meat.states.back().first == 1850, "burn level: never back in time");
            Check(BurnLevelAt(meat, 900) == 0.5f && BurnLevelAt(meat, 1400) == 1.0f && BurnLevelAt(meat, 0) == 0.0f,
                "burn level: the stored cook at the playhead");
            using replay::entity::EntityKind;
            Check(BurnLevelPlayable(EntityKind::Weapon) && BurnLevelPlayable(EntityKind::Prop) &&
                    BurnLevelPlayable(EntityKind::Vehicle) && BurnLevelPlayable(EntityKind::Ped) &&
                    BurnLevelPlayable(EntityKind::Horse) && !BurnLevelPlayable(EntityKind::TrainCar),
                "burn level: written on weapon objects and props too, not on train cars");
        }
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error),
            "burn level: encode");
        Clip loaded;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok, "burn level: a clip with burn level tracks loads");
        if (ok)
        {
            const Recording& r = loaded.recording;
            Check(r.burnLevels.size() == 1 && r.burnLevels[0].id == vehicle.Packed() && r.burnLevels[0].states.size() == 3 &&
                    r.burnLevels[0].states[1].first == 400 && r.burnLevels[0].states[1].second == 0.35f &&
                    r.burnLevels[0].states[2].second == 0.8f,
                "burn level: track round trip");
        }
        const std::vector<std::uint8_t> old = WithoutSection(bytes, "burn levels");
        Check(!old.empty(), "burn level: clip without the section built");
        if (!old.empty())
        {
            Clip older;
            const bool oldOk = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, error);
            Check(oldOk && older.recording.burnLevels.empty(), "burn level: an older clip without the section loads with no tracks");
        }
        // The last state (8 bytes before the end: u32 time, f32 level).
        const auto corrupt = [&](std::size_t fromEnd, const void* value, const char* what) {
            std::vector<std::uint8_t> bad = bytes;
            const std::size_t at = FindSection(bad, "burn levels");
            if (at >= bad.size())
            {
                Check(false, what);
                return;
            }
            std::uint64_t length = 0;
            std::memcpy(&length, bad.data() + at + 6, 8);
            std::uint8_t* payload = bad.data() + at + 14;
            std::memcpy(payload + length - fromEnd, value, 4);
            const std::uint64_t hash = TestFnv64(payload, static_cast<std::size_t>(length));
            std::memcpy(payload + length, &hash, sizeof(hash));
            std::string badError;
            Check(DecodeSkips(bad, "burn levels", badError), what);
        };
        const std::uint32_t early = 50u;
        corrupt(8, &early, "burn level: states out of order refused");
        const float notANumber = std::numeric_limits<float>::quiet_NaN();
        corrupt(4, &notANumber, "burn level: a level that is not a number refused");
    }

    // The fx settings (FXSS, per one-shot event) and the registered settings
    // and call gaps (RGSS, per track) go there and back; both are left out
    // when empty (an older build opens the clip), and a clip without them
    // loads with no settings and no gaps (the playback derives the gaps).
    void TestFxSettingsSections()
    {
        Clip clip;
        MakeClip(clip);
        Check(!clip.recording.fx.Events().empty() && !clip.recording.registeredFx.empty(), "fx settings: the fixture has fx");
        entity::FxSettings s;
        s.mask = entity::FxSettings::kAll;
        s.vel350[1] = 2.5f;
        s.tint380[0] = 0.25f;
        s.alpha38C = 0.75f;
        s.aux39C = 1.5f;
        s.lod3A0 = 0.125f;
        s.tele3A8 = 1.0f;
        s.cull3BC = 0x1234;
        s.ev5CC = 0xABCD;
        clip.recording.fx.At(0).hasSettings = true;
        clip.recording.fx.At(0).settings = s;
        entity::RegisteredFxTrack& track = clip.recording.registeredFx[0];
        track.settings.clear();
        track.callGaps.clear();
        track.settings.push_back({110, s});
        s.tele3A8 = 10.0f;
        track.settings.push_back({150, s});
        track.callGaps.push_back({120, 140});
        track.callGaps.push_back({160, 175});
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "fx settings: encode");
        const auto hasTag = [&](const std::vector<std::uint8_t>& data, const char* tag) {
            return std::search(data.begin(), data.end(), tag, tag + 4) != data.end();
        };
        Check(hasTag(bytes, "FXSS") && hasTag(bytes, "RGSS"), "fx settings: both sections written");
        Clip loaded;
        std::uint64_t duration = 0;
        Check(clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses, loaded.camera, duration,
                  error),
            "fx settings: decode");
        const entity::FxEvent& event = loaded.recording.fx.Events()[0];
        Check(event.hasSettings && event.settings.mask == entity::FxSettings::kAll && event.settings.vel350[1] == 2.5f &&
                event.settings.tint380[0] == 0.25f && event.settings.alpha38C == 0.75f && event.settings.aux39C == 1.5f &&
                event.settings.lod3A0 == 0.125f && event.settings.tele3A8 == 1.0f && event.settings.cull3BC == 0x1234 &&
                event.settings.ev5CC == 0xABCD,
            "fx settings: FXSS round trip");
        const entity::RegisteredFxTrack& back = loaded.recording.registeredFx[0];
        Check(back.settings.size() == 2 && back.settings[0].timeMs == 110 && back.settings[1].settings.tele3A8 == 10.0f &&
                back.callGaps.size() == 2 && back.callGaps[0].fromMs == 120 && back.callGaps[0].toMs == 140 &&
                back.callGaps[1].toMs == 175,
            "fx settings: RGSS round trip");
        Check(entity::RegisteredFxSettingsAt(back, 100) == &back.settings[0].settings &&
                entity::RegisteredFxSettingsAt(back, 160) == &back.settings[1].settings,
            "fx settings: the settings in force at a time");
        // The sound sections (FMOV, LSTN v2) and the particle sections (FXSS,
        // RGSS) in one clip, each read back.
        Check(hasTag(bytes, "FMOV") && hasTag(bytes, "LSTN"), "fx settings: FMOV and LSTN written beside FXSS and RGSS");
        Check(loaded.recording.footMovement.samples.size() == clip.recording.footMovement.samples.size() &&
                !loaded.recording.footMovement.samples.empty() &&
                loaded.recording.footMovement.samples[0].result == clip.recording.footMovement.samples[0].result &&
                loaded.recording.gameListener.samples.size() == clip.recording.gameListener.samples.size(),
            "fx settings: FMOV and LSTN round trip beside FXSS and RGSS");
        // Empty: left out, and the clip loads as a clip from before.
        clip.recording.fx.At(0).hasSettings = false;
        track.settings.clear();
        track.callGaps.clear();
        std::vector<std::uint8_t> without;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, without, error),
            "fx settings: encode without");
        Check(!hasTag(without, "FXSS") && !hasTag(without, "RGSS"), "fx settings: left out when empty");
        Clip old;
        Check(clipfile::Decode(without.data(), without.size(), kBase, kImageSize, old.recording, old.poses, old.camera, duration, error) &&
                !old.recording.fx.Events()[0].hasSettings && old.recording.registeredFx[0].settings.empty() &&
                old.recording.registeredFx[0].callGaps.empty(),
            "fx settings: a clip without them loads with none");
        // A clip with FMOV and no FXSS or RGSS keeps its sound sections.
        Check(hasTag(without, "FMOV") && old.recording.footMovement.samples.size() == clip.recording.footMovement.samples.size(),
            "fx settings: a sound-only clip keeps FMOV");
        // A clip with FXSS and RGSS and no FMOV keeps its particle sections.
        {
            Clip particles;
            MakeClip(particles);
            particles.recording.footMovement.Clear();
            particles.recording.fx.At(0).hasSettings = true;
            particles.recording.fx.At(0).settings = s;
            std::vector<std::uint8_t> only;
            Check(clipfile::Encode(particles.recording, particles.poses, particles.camera, 1000, kBase, kImageSize, only, error) &&
                    !hasTag(only, "FMOV") && hasTag(only, "FXSS"),
                "fx settings: a particle-only clip has FXSS and no FMOV");
            Clip back2;
            Check(clipfile::Decode(only.data(), only.size(), kBase, kImageSize, back2.recording, back2.poses, back2.camera, duration,
                      error) &&
                    back2.recording.fx.Events()[0].hasSettings && back2.recording.footMovement.samples.empty(),
                "fx settings: a particle-only clip loads with FXSS and no FMOV");
        }
        // A gap out of order is refused (the section is left out, the clip loads).
        track.callGaps.push_back({160, 175});
        track.callGaps.push_back({120, 140});
        std::vector<std::uint8_t> bad;
        clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bad, error);
        Clip refused;
        const bool loadedBad =
            clipfile::Decode(bad.data(), bad.size(), kBase, kImageSize, refused.recording, refused.poses, refused.camera, duration, error);
        Check(loadedBad && refused.recording.registeredFx[0].callGaps.empty(), "fx settings: gaps out of order leave the section out");
    }

    // The weapon fx requests (WFXI) round trip, left out when empty, a bad
    // kind refused.
    void TestWeaponFxIntentsSection()
    {
        Clip clip;
        MakeClip(clip);
        weaponfx::Intent muzzle;
        muzzle.timeMs = 120;
        muzzle.shooter = 0x00030001u;
        muzzle.weapon = 0x00070002u;
        muzzle.kind = static_cast<std::uint8_t>(weaponfx::Kind::Muzzle);
        muzzle.bone = 6;
        muzzle.argA = 1;
        muzzle.fxHash = 0x9086E654u;
        muzzle.fxHashAlt = 0x1234u;
        muzzle.range = 60.0f;
        muzzle.vec[0] = 0.25f;
        muzzle.vec[3] = 1.0f;
        weaponfx::Intent shell = muzzle;
        shell.timeMs = 130;
        shell.kind = static_cast<std::uint8_t>(weaponfx::Kind::Shell);
        shell.weapon = weaponfx::kNoId;
        clip.recording.weaponFxIntents.Clear();
        Check(clip.recording.weaponFxIntents.Add(muzzle) && clip.recording.weaponFxIntents.Add(shell), "weapon fx intents: add");
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "weapon fx intents: encode");
        const char tag[4] = {'W', 'F', 'X', 'I'};
        auto hasTag = [&](const std::vector<std::uint8_t>& data) {
            return std::search(data.begin(), data.end(), tag, tag + 4) != data.end();
        };
        Check(hasTag(bytes), "weapon fx intents: WFXI written");
        Clip loaded;
        std::uint64_t duration = 0;
        Check(clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses, loaded.camera, duration,
                  error),
            "weapon fx intents: decode");
        const std::vector<weaponfx::Intent>& back = loaded.recording.weaponFxIntents.intents;
        Check(back.size() == 2 && back[0].shooter == 0x00030001u && back[0].weapon == 0x00070002u && back[0].bone == 6 &&
                back[0].fxHash == 0x9086E654u && back[0].range == 60.0f && back[0].vec[3] == 1.0f &&
                back[1].kind == static_cast<std::uint8_t>(weaponfx::Kind::Shell) && back[1].weapon == weaponfx::kNoId,
            "weapon fx intents: WFXI round trip");
        clip.recording.weaponFxIntents.Clear();
        std::vector<std::uint8_t> without;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, without, error) && !hasTag(without),
            "weapon fx intents: left out when empty (a build from before opens the clip)");
        weaponfx::Intent bad = muzzle;
        bad.kind = 9;
        Check(!clip.recording.weaponFxIntents.Add(bad), "weapon fx intents: an unknown kind is refused");
    }

    // The speech requests section is left out when no line has its fields, and
    // a clip without it loads them unread.
    void TestSpeechRequestsSection()
    {
        Clip clip;
        MakeClip(clip);
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error),
            "speech requests: encode");
        const char tag[4] = {'S', 'P', 'R', 'Q'};
        auto hasTag = [&](const std::vector<std::uint8_t>& data) {
            return std::search(data.begin(), data.end(), tag, tag + 4) != data.end();
        };
        Check(hasTag(bytes), "speech requests: written when a line has them");
        for (std::size_t n = 0; n < clip.recording.speechLines.Events().size(); ++n)
            clip.recording.speechLines.At(n).request = replay::speech::request::Fields{};
        std::vector<std::uint8_t> without;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, without, error),
            "speech requests: encode without");
        Check(!hasTag(without), "speech requests: left out when no line has them (a build from before opens the clip)");
        Clip loaded;
        std::uint64_t duration = 0;
        Check(clipfile::Decode(without.data(), without.size(), kBase, kImageSize, loaded.recording, loaded.poses,
                  loaded.camera, duration, error) &&
                !loaded.recording.speechLines.Events().empty() && !loaded.recording.speechLines.Events()[0].request.read,
            "speech requests: a clip without the section loads its lines unread");
    }

    void TestVersionsAndSections()
    {
        Clip clip;
        MakeClip(clip);
        std::vector<std::uint8_t> bytes;
        std::string error;
        clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error);
        Check(clipfile::Sections().size() == 91,
            "91 sections (WGPU retired; VDOR, WFXI, FMOV, FXSS, RGSS, CATS, FSEV, PSPD, SHVR, WNDA, MIXS, OLOK, LSTN, RTGT, MIXP, CFXE, MOON, SPRQ, SCHT, SBLV, FPCE, SCCH, FVAR, TCMD, PVIS, EQRY, SCNY, MPED, SGEN, ENVO, SKYT, ENVT, VEHL, WCND, RFXA, CUTS, SPAN, SCOR, AANI, WINF, WRLD, VWIN, "
            "APPC, VPRT, VBRN, BLVL, FREC, ESND, TRNC, "
            "SCEN and GSND among them)");
        {
            // A clip from another game build is refused (META).
            clipfile::SetBuildIdentity(0x11111111u, kImageSize);
            std::vector<std::uint8_t> fromBuildA;
            std::string buildError;
            clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, fromBuildA, buildError);
            clipfile::SetBuildIdentity(0x22222222u, kImageSize);
            Recording other;
            std::vector<pose::PoseRecordExport> otherPoses;
            camera::Track otherCamera;
            std::uint64_t otherDuration = 0;
            const bool loaded = clipfile::Decode(fromBuildA.data(), fromBuildA.size(), kBase, kImageSize, other, otherPoses,
                otherCamera, otherDuration, buildError);
            Check(!loaded && buildError.find("meta") != std::string::npos, "a clip from another game build is refused");
            clipfile::SetBuildIdentity(0x11111111u, kImageSize);
            Check(clipfile::Decode(fromBuildA.data(), fromBuildA.size(), kBase, kImageSize, other, otherPoses, otherCamera,
                      otherDuration, buildError),
                "the same build loads");
            clipfile::SetBuildIdentity(0, 0);
        }

        {
            // An optional section (PLAC) may be missing: a clip written
            // before it loads with that member empty; a required one missing
            // is refused.
            std::vector<std::uint8_t> withoutPlace = bytes;
            const std::size_t place = FindSection(withoutPlace, "place");
            Check(place < withoutPlace.size(), "place section found");
            if (place < withoutPlace.size())
            {
                std::uint64_t length = 0;
                std::memcpy(&length, withoutPlace.data() + place + 6, 8);
                withoutPlace.erase(withoutPlace.begin() + static_cast<std::ptrdiff_t>(place),
                    withoutPlace.begin() + static_cast<std::ptrdiff_t>(place + 14 + length + 8));
                Clip loaded;
                std::uint64_t duration = 0;
                std::string placeError;
                Check(clipfile::Decode(withoutPlace.data(), withoutPlace.size(), kBase, kImageSize, loaded.recording,
                          loaded.poses, loaded.camera, duration, placeError) && !loaded.recording.place.recorded,
                    "a clip without the optional place section loads");
            }
            // A clip from before the registered fx attach changes (RFXA) loads
            // with its tracks and no attach changes.
            std::vector<std::uint8_t> withoutAttaches = bytes;
            const std::size_t attaches = FindSection(withoutAttaches, "registered fx attaches");
            Check(attaches < withoutAttaches.size(), "registered fx attaches section found");
            if (attaches < withoutAttaches.size())
            {
                std::uint64_t length = 0;
                std::memcpy(&length, withoutAttaches.data() + attaches + 6, 8);
                withoutAttaches.erase(withoutAttaches.begin() + static_cast<std::ptrdiff_t>(attaches),
                    withoutAttaches.begin() + static_cast<std::ptrdiff_t>(attaches + 14 + length + 8));
                Clip loaded;
                std::uint64_t duration = 0;
                std::string attachesError;
                Check(clipfile::Decode(withoutAttaches.data(), withoutAttaches.size(), kBase, kImageSize,
                          loaded.recording, loaded.poses, loaded.camera, duration, attachesError) &&
                        !loaded.recording.registeredFx.empty() &&
                        loaded.recording.registeredFx[0].attachTimeline.empty() &&
                        !loaded.recording.registeredFx[0].frames.empty(),
                    "a clip without the registered fx attaches section loads");
            }
            std::vector<std::uint8_t> withoutArea = bytes;
            const std::size_t area = FindSection(withoutArea, "area");
            if (area < withoutArea.size())
            {
                std::uint64_t length = 0;
                std::memcpy(&length, withoutArea.data() + area + 6, 8);
                withoutArea.erase(withoutArea.begin() + static_cast<std::ptrdiff_t>(area),
                    withoutArea.begin() + static_cast<std::ptrdiff_t>(area + 14 + length + 8));
                std::string areaError;
                Check(DecodeFails(withoutArea, areaError) && areaError.find("missing") != std::string::npos,
                    "a required section missing is refused");
            }
        }

        std::vector<std::uint8_t> changed = bytes;
        const std::size_t blood = FindSection(changed, "blood");
        Check(blood < changed.size(), "blood section found");
        if (blood < changed.size())
        {
            changed[blood + 4] = static_cast<std::uint8_t>(changed[blood + 4] + 1);
            Check(DecodeFails(changed, error), "changed section version refused");
            Check(error.find("'blood'") != std::string::npos && error.find("version") != std::string::npos,
                "refusal names the section and its version");
        }

        changed = bytes;
        const std::size_t pose = FindSection(changed, "pose");
        if (pose < changed.size())
        {
            changed[pose] = 'X';
            Check(DecodeFails(changed, error) && error.find("unknown section") != std::string::npos,
                "unknown section refused");
        }

        // Drop the camera section (the last one): missing.
        changed = bytes;
        const std::size_t cameraSection = FindSection(changed, "camera");
        if (cameraSection < changed.size())
        {
            changed.resize(cameraSection);
            Check(DecodeFails(changed, error) && error.find("missing section 'camera'") != std::string::npos,
                "missing section refused");
        }

        changed = bytes;
        changed[8] = 1; // format version (this build writes 2)
        Check(DecodeFails(changed, error) && error.find("format version") != std::string::npos,
            "other format version refused");
        changed = bytes;
        changed[0] = 'X';
        Check(DecodeFails(changed, error), "bad magic refused");
    }

    void TestTruncated()
    {
        Clip clip;
        MakeClip(clip);
        std::vector<std::uint8_t> bytes;
        std::string error;
        clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error);
        bool refused = true;
        const std::size_t step = bytes.size() / 97 + 1;
        for (std::size_t size = 0; size < bytes.size(); size += step)
        {
            std::vector<std::uint8_t> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size));
            refused = refused && DecodeFails(cut, error);
        }
        std::vector<std::uint8_t> cut(bytes.begin(), bytes.end() - 1);
        refused = refused && DecodeFails(cut, error);
        Check(refused, "every truncated file is refused");
        // With no shake events to fold there is no CFXE: the camera is the
        // last section.
        Check(error.find("'camera'") != std::string::npos, "truncation names the section");

        // A byte flipped inside a payload after save is refused by its hash.
        // The byte next to the middle may fall in the section table (more
        // sections move it), refused as an unknown section there: every flip is
        // refused and the payload ones name the hash.
        {
            Clip flipped;
            MakeClip(flipped);
            std::vector<std::uint8_t> flippedBytes;
            clipfile::Encode(flipped.recording, flipped.poses, flipped.camera, 1000, kBase, kImageSize, flippedBytes, error);
            bool allRefused = true;
            bool anyHash = false;
            for (std::size_t offset = flippedBytes.size() / 2; offset < flippedBytes.size() / 2 + 64; ++offset)
            {
                std::vector<std::uint8_t> copy = flippedBytes;
                copy[offset] ^= 0x01;
                allRefused = DecodeFails(copy, error) && allRefused;
                anyHash = anyHash || error.find("hash") != std::string::npos;
            }
            Check(allRefused && anyHash, "flipped byte refused by hash");
        }

        // A corrupt pose chunk is refused at load, not at playback.
        Clip corrupt;
        MakeClip(corrupt);
        corrupt.poses[0].track.chunks[1].bytes.Mutable()[0] ^= 0xFF; // record size of the chunk's first frame
        std::vector<std::uint8_t> corruptBytes;
        clipfile::Encode(corrupt.recording, corrupt.poses, corrupt.camera, 1000, kBase, kImageSize, corruptBytes, error);
        Check(DecodeFails(corruptBytes, error) && error.find("'pose'") != std::string::npos, "corrupt pose refused");

        // A failed load leaves the destination as it was.
        Clip target;
        MakeClip(target);
        const std::size_t before = target.recording.shots.Events().size();
        std::uint64_t duration = 77;
        Check(!clipfile::Decode(bytes.data(), bytes.size() - 1, kBase, kImageSize, target.recording, target.poses,
                  target.camera, duration, error) &&
                target.recording.shots.Events().size() == before && duration == 77 && target.poses.size() == 2,
            "failed load changes nothing");

        std::uint64_t loadedDuration = 0;
        Check(!clipfile::Load(TempPath(L"replaypoc_clip_missing.rpclip"), kBase, kImageSize, target.recording,
                  target.poses, target.camera, loadedDuration, error),
            "missing file refused");
    }

    // Compressed clips (clip_compress.h).
    bool WriteBytes(const std::wstring& path, const std::vector<std::uint8_t>& bytes)
    {
        std::ofstream file(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(file);
    }

    std::vector<std::uint8_t> EncodeLoaded(Clip& clip, std::uint64_t duration)
    {
        std::vector<std::uint8_t> bytes;
        std::string error;
        clipfile::Encode(clip.recording, clip.poses, clip.camera, duration, kBase, kImageSize, bytes, error);
        return bytes;
    }

    void SetOldTimes(const std::wstring& path, std::uint64_t ticks)
    {
        HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        FILETIME time{static_cast<DWORD>(ticks & 0xFFFFFFFFu), static_cast<DWORD>(ticks >> 32)};
        SetFileTime(file, &time, &time, &time);
        CloseHandle(file);
    }

    void TestCompressed()
    {
        Clip saved;
        MakeClip(saved);
        const std::wstring plainPath = TempPath(L"replaypoc_clip_plain.rpclip");
        const std::wstring packedPath = TempPath(L"replaypoc_clip_packed.rpclip");
        std::string error;
        Check(clipfile::Save(saved.recording, saved.poses, saved.camera, 4321, kBase, kImageSize, plainPath, error),
            "compress: save the plain clip");
        std::vector<std::uint8_t> plain;
        Check(clipcompress::ReadWholeFile(plainPath, plain, nullptr, error) && clipcompress::IsPlainClip(plain.data(), plain.size()),
            "compress: read the plain clip");

        // Compress then Load gives what the plain Load gives.
        std::vector<std::uint8_t> packed;
        Check(clipcompress::Compress(plain.data(), plain.size(), clipcompress::kDefaultLevel, nullptr, packed, error),
            "compress: level 19");
        Check(clipcompress::IsCompressed(packed.data(), packed.size()) && packed.size() < plain.size(),
            "compress: a smaller zstd frame");
        Check(clipcompress::ContentSize(packed.data(), packed.size()) == plain.size(), "compress: content size in the frame");
        Check(clipcompress::Verify(packed.data(), packed.size(), plain.data(), plain.size(), nullptr, error), "compress: verify");
        std::vector<std::uint8_t> summary;
        Check(clipcompress::Summary(plain.data(), plain.size(), summary) && summary.size() > 20, "compress: summary");
        std::vector<std::uint8_t> withSummary = packed;
        clipcompress::AppendSummaryFrame(summary, withSummary);
        Check(WriteBytes(packedPath, withSummary), "compress: write the compressed clip");

        Clip fromPlain, fromPacked;
        std::uint64_t plainDuration = 0, packedDuration = 0;
        clipfile::LoadInfo plainInfo, packedInfo;
        Check(clipfile::Load(plainPath, kBase, kImageSize, fromPlain.recording, fromPlain.poses, fromPlain.camera,
                  plainDuration, error, &plainInfo) && !plainInfo.compressed,
            "compress: plain load");
        Check(clipfile::Load(packedPath, kBase, kImageSize, fromPacked.recording, fromPacked.poses, fromPacked.camera,
                  packedDuration, error, &packedInfo),
            "compress: compressed load");
        if (!error.empty())
            std::fprintf(stderr, "compressed load error: %s\n", error.c_str());
        Check(packedInfo.compressed && packedInfo.clipBytes == plain.size() && packedInfo.fileBytes == withSummary.size(),
            "compress: load info");
        Check(plainDuration == 4321 && packedDuration == 4321, "compress: duration");
        Check(EncodeLoaded(fromPlain, plainDuration) == EncodeLoaded(fromPacked, packedDuration) &&
                EncodeLoaded(fromPacked, packedDuration) == plain,
            "compress: the compressed load equals the plain load");
        CompareClips(saved, fromPacked);

        // The clip list reads the same summary from both files, and from a
        // compressed file without its summary frame (streamed).
        clipcompress::FileHead head;
        Check(clipcompress::ReadHead(plainPath, head) && !head.compressed && head.summary == summary &&
                head.clipBytes == plain.size(),
            "compress: plain head");
        Check(clipcompress::ReadHead(packedPath, head) && head.compressed && head.summary == summary &&
                head.clipBytes == plain.size() && head.diskBytes == withSummary.size(),
            "compress: compressed head from the summary frame");
        Check(WriteBytes(packedPath, packed) && clipcompress::ReadHead(packedPath, head) && head.compressed &&
                head.summary == summary,
            "compress: compressed head streamed");

        // Corrupt and truncated compressed files are refused, the
        // destination unchanged.
        Clip target;
        MakeClip(target);
        const std::size_t shotsBefore = target.recording.shots.Events().size();
        std::uint64_t duration = 77;
        std::vector<std::uint8_t> corrupt = packed;
        corrupt[corrupt.size() / 2] ^= 0x5A;
        Check(WriteBytes(packedPath, corrupt), "compress: write corrupt");
        error.clear();
        Check(!clipfile::Load(packedPath, kBase, kImageSize, target.recording, target.poses, target.camera, duration, error) &&
                !error.empty() && duration == 77 && target.recording.shots.Events().size() == shotsBefore,
            "compress: corrupt compressed clip refused");
        std::vector<std::uint8_t> truncated(packed.begin(), packed.begin() + static_cast<std::ptrdiff_t>(packed.size() - 7));
        Check(WriteBytes(packedPath, truncated), "compress: write truncated");
        error.clear();
        Check(!clipfile::Load(packedPath, kBase, kImageSize, target.recording, target.poses, target.camera, duration, error) &&
                error.find("compressed clip") != std::string::npos && duration == 77,
            "compress: truncated compressed clip refused");
        std::vector<std::uint8_t> trailing = packed;
        trailing.push_back(0x42);
        std::vector<std::uint8_t> out;
        Check(!clipcompress::Decompress(trailing.data(), trailing.size(), out, error) && out.empty(),
            "compress: trailing garbage refused");
        std::vector<std::uint8_t> header(packed.begin(), packed.begin() + 4);
        Check(!clipcompress::Decompress(header.data(), header.size(), out, error), "compress: magic only refused");

        // Cancelled between chunks.
        std::atomic<bool> cancel{true};
        Check(!clipcompress::Compress(plain.data(), plain.size(), 3, &cancel, out, error) && error == "cancelled",
            "compress: cancel");

        // In place: times kept, a second pass skips it, not a clip skipped.
        const std::uint64_t old = clipcompress::NowTicks() - 30 * clipcompress::kTicksPerDay;
        Check(WriteBytes(packedPath, plain), "compress: plain copy");
        SetOldTimes(packedPath, old);
        const clipcompress::FileResult result = clipcompress::CompressFile(packedPath, 19, nullptr);
        Check(result.ok && result.bytesIn == plain.size() && result.bytesOut > 0 && result.bytesOut < plain.size(),
            "compress: file compressed in place");
        if (!result.ok)
            std::fprintf(stderr, "compress file: %s\n", result.reason.c_str());
        Check(!std::filesystem::exists(packedPath + L".ztmp"), "compress: no temporary file left");
        Check(clipcompress::Peek(packedPath) == clipcompress::Kind::Compressed, "compress: the file is compressed");
        clipcompress::FileTimes times;
        Check(clipcompress::GetTimes(packedPath, times) && times.lastWrite == old && times.lastAccess == old,
            "compress: the original times kept");
        Check(clipcompress::ReadHead(packedPath, head) && head.summary == summary, "compress: in place head");
        Clip inPlace;
        Check(clipfile::Load(packedPath, kBase, kImageSize, inPlace.recording, inPlace.poses, inPlace.camera, duration,
                  error) &&
                EncodeLoaded(inPlace, duration) == plain,
            "compress: in place load");
        const clipcompress::FileResult again = clipcompress::CompressFile(packedPath, 19, nullptr);
        Check(!again.ok && again.skipped && again.reason == "already_compressed", "compress: already compressed skipped");
        Check(WriteBytes(packedPath, std::vector<std::uint8_t>{'R', 'D', 'R', 'C', 'L', 'I', 'P', '1', 0, 0}),
            "compress: other file");
        const clipcompress::FileResult other = clipcompress::CompressFile(packedPath, 19, nullptr);
        Check(!other.ok && other.skipped && other.reason == "not_a_clip", "compress: not a clip skipped");

        // A load stamps the last access (the age rule reads it).
        SetOldTimes(plainPath, old);
        Check(clipcompress::StampAccess(plainPath) && clipcompress::GetTimes(plainPath, times) &&
                times.lastWrite == old && times.lastAccess > clipcompress::NowTicks() - clipcompress::kTicksPerDay,
            "compress: access stamped");

        std::error_code ignored;
        std::filesystem::remove(plainPath, ignored);
        std::filesystem::remove(packedPath, ignored);
    }

    void TestCompressAge()
    {
        constexpr std::uint64_t kDay = clipcompress::kTicksPerDay;
        const std::uint64_t now = 1000 * kDay;
        Check(clipcompress::OldEnough(now - 8 * kDay, now - 9 * kDay, now, 7), "age: written 8 days ago");
        Check(clipcompress::OldEnough(now - 7 * kDay, now - 7 * kDay, now, 7), "age: exactly 7 days");
        Check(!clipcompress::OldEnough(now - 7 * kDay + 1, now - 30 * kDay, now, 7), "age: written just under 7 days ago");
        Check(!clipcompress::OldEnough(now - 30 * kDay, now - 2 * kDay, now, 7), "age: opened 2 days ago keeps it");
        Check(!clipcompress::OldEnough(now - 30 * kDay, now + kDay, now, 7), "age: a time in the future keeps it");
        Check(!clipcompress::OldEnough(now - 30 * kDay, now - 30 * kDay, now, 0), "age: 0 days never");
        Check(clipcompress::OldEnough(now - kDay, 0, now, 1), "age: 1 day, no access time");
        Check(!clipcompress::OldEnough(0, 0, 0, 1), "age: zero clock");
    }
}

namespace
{
    std::vector<std::uint8_t> EncodeClip(const Clip& clip, std::uint64_t duration = 1000)
    {
        std::vector<std::uint8_t> bytes;
        std::string error;
        clipfile::Encode(clip.recording, clip.poses, clip.camera, duration, kBase, kImageSize, bytes, error);
        return bytes;
    }

    // A piece row the reader cannot trust (groups out of order, an unknown
    // source) leaves the optional section out: the clip opens and the pieces
    // play whole (the whole object).
    // ---- The packed container (clip_pack.h) ----
    struct PackRng
    {
        std::uint64_t state = 0x9E3779B97F4A7C15ull;
        std::uint32_t Next()
        {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return static_cast<std::uint32_t>(state >> 16);
        }
        std::uint32_t Below(std::uint32_t n) { return n ? Next() % n : 0; }
    };

    template <typename T> void PackPut(std::vector<std::uint8_t>& out, T value)
    {
        const std::size_t at = out.size();
        out.resize(at + sizeof(T));
        std::memcpy(out.data() + at, &value, sizeof(T));
    }
    void PackRandomBytes(std::vector<std::uint8_t>& out, PackRng& rng, std::size_t count)
    {
        for (std::size_t n = 0; n < count; ++n)
            out.push_back(static_cast<std::uint8_t>(rng.Next()));
    }

    // Pose records as pose_stream writes them: every mask bit, channels in
    // and out of order (0 and 65535 too), smooth and jumping values, the
    // rotation index changing, the time wrapping. `unknownMask` puts a mask
    // bit the transform does not know in one entry.
    std::vector<std::uint8_t> MakePackRecords(PackRng& rng, std::uint32_t records, std::uint32_t& time,
        std::vector<std::uint32_t>& times, std::vector<std::uint32_t>& offsets, bool unknownMask = false)
    {
        std::vector<std::uint8_t> out;
        static std::uint32_t smooth[8] = {};
        for (std::uint32_t r = 0; r < records; ++r)
        {
            const std::size_t start = out.size();
            offsets.push_back(static_cast<std::uint32_t>(start));
            time += rng.Below(5) == 0 ? 0xFFFFFF00u + rng.Below(512) : 33u + rng.Below(3);
            times.push_back(time);
            const bool odd = unknownMask && r == records / 2;
            const std::uint16_t count = static_cast<std::uint16_t>(odd ? 1 + rng.Below(13) : rng.Below(14));
            PackPut<std::uint32_t>(out, 0); // size, below
            PackPut<std::uint32_t>(out, time);
            PackPut<std::uint16_t>(out, static_cast<std::uint16_t>(rng.Below(4)));
            PackPut<std::uint16_t>(out, count);
            for (int k = 0; k < 16; ++k)
                PackPut<std::uint32_t>(out, rng.Below(3) == 0 ? rng.Next() : 0x3F800000u + static_cast<std::uint32_t>(k));
            std::uint16_t channel = static_cast<std::uint16_t>(rng.Below(3));
            for (std::uint16_t e = 0; e < count; ++e)
            {
                const std::uint32_t pick = rng.Below(10);
                channel = pick == 0 ? 65535 : pick == 1 ? static_cast<std::uint16_t>(rng.Below(40)) : static_cast<std::uint16_t>(channel + 1 + rng.Below(3));
                std::uint8_t mask = static_cast<std::uint8_t>(rng.Below(64));
                if (odd && e == 0)
                    mask |= 0x40;
                PackPut<std::uint16_t>(out, channel);
                out.push_back(mask);
                for (int b = 0; b < 6; ++b)
                {
                    if (!(mask & (1u << b)))
                        continue;
                    const std::size_t words[6] = {1, 2, 0, 3, 1, 4};
                    if (b == 2)
                    {
                        for (int k = 0; k < 3; ++k)
                            PackPut<std::uint16_t>(out, static_cast<std::uint16_t>(smooth[k] += rng.Below(7)));
                        continue;
                    }
                    for (std::size_t w = 0; w < words[b]; ++w)
                    {
                        smooth[b] += rng.Below(5);
                        PackPut<std::uint32_t>(out, rng.Below(4) == 0 ? rng.Next() : smooth[b] ^ (rng.Below(8) << 28));
                    }
                }
            }
            const std::uint32_t size = static_cast<std::uint32_t>(out.size() - start);
            std::memcpy(out.data() + start, &size, 4);
        }
        return out;
    }

    std::vector<std::uint8_t> MakePackPose(PackRng& rng, std::uint64_t tracks, bool unknownMask = false)
    {
        std::vector<std::uint8_t> out;
        PackPut<std::uint64_t>(out, tracks);
        for (std::uint64_t t = 0; t < tracks; ++t)
        {
            PackRandomBytes(out, rng, 24);
            PackPut<std::uint64_t>(out, 1u << 20);
            const std::uint64_t layouts = rng.Below(3);
            PackPut<std::uint64_t>(out, layouts);
            for (std::uint64_t l = 0; l < layouts; ++l)
            {
                PackPut<std::uint32_t>(out, rng.Below(200));
                out.push_back(static_cast<std::uint8_t>(rng.Below(2)));
                out.push_back(static_cast<std::uint8_t>(rng.Below(2)));
                const std::size_t sizes[5] = {4, 1, 1, 4, 4};
                for (const std::size_t size : sizes)
                {
                    const std::uint64_t n = rng.Below(6);
                    PackPut<std::uint64_t>(out, n);
                    PackRandomBytes(out, rng, static_cast<std::size_t>(n * size));
                }
            }
            std::uint32_t time = rng.Next();
            const std::uint64_t anchors = rng.Below(3);
            PackPut<std::uint64_t>(out, anchors);
            for (std::uint64_t a = 0; a < anchors; ++a)
            {
                std::vector<std::uint32_t> times, offsets;
                const std::vector<std::uint8_t> bytes = MakePackRecords(rng, rng.Below(3), time, times, offsets);
                PackPut<std::uint64_t>(out, bytes.size());
                out.insert(out.end(), bytes.begin(), bytes.end());
            }
            const bool odd = unknownMask && t == tracks - 1;
            const std::uint64_t chunks = odd ? 1 + rng.Below(3) : rng.Below(4);
            PackPut<std::uint64_t>(out, chunks);
            for (std::uint64_t c = 0; c < chunks; ++c)
            {
                PackPut<std::uint32_t>(out, rng.Below(3));
                PackPut<std::int32_t>(out, anchors ? static_cast<std::int32_t>(rng.Below(static_cast<std::uint32_t>(anchors))) : -1);
                std::vector<std::uint32_t> times, offsets;
                const std::vector<std::uint8_t> bytes =
                    MakePackRecords(rng, odd && c == 0 ? 2 + rng.Below(28) : rng.Below(30), time, times, offsets, odd && c == 0);
                PackPut<std::uint64_t>(out, bytes.size());
                out.insert(out.end(), bytes.begin(), bytes.end());
                // Usually the records' own times and offsets; sometimes not
                // (kept as they are), or one short.
                const std::uint32_t which = rng.Below(5);
                if (which == 0 && !times.empty())
                    times[0] ^= 1;
                if (which == 1 && !offsets.empty())
                    offsets.pop_back();
                PackPut<std::uint64_t>(out, times.size());
                for (const std::uint32_t value : times)
                    PackPut<std::uint32_t>(out, value);
                PackPut<std::uint64_t>(out, offsets.size());
                for (const std::uint32_t value : offsets)
                    PackPut<std::uint32_t>(out, value);
            }
            PackRandomBytes(out, rng, 68);
        }
        return out;
    }

    std::vector<std::uint8_t> MakePackRope(PackRng& rng, std::uint64_t ropes)
    {
        std::vector<std::uint8_t> out;
        PackPut<std::uint64_t>(out, ropes);
        for (std::uint64_t n = 0; n < ropes; ++n)
        {
            const std::uint32_t vertices = n == 1 ? 0 : 1 + rng.Below(6);
            PackPut<std::uint32_t>(out, rng.Next());
            PackPut<std::uint32_t>(out, rng.Below(5));
            PackPut<std::uint32_t>(out, vertices);
            const std::uint64_t attachments = rng.Below(3);
            PackPut<std::uint64_t>(out, attachments);
            PackRandomBytes(out, rng, static_cast<std::size_t>(attachments * 8));
            PackPut<std::uint32_t>(out, rng.Next());
            for (int list = 0; list < 2; ++list)
            {
                const std::uint64_t events = rng.Below(3);
                PackPut<std::uint64_t>(out, events);
                PackRandomBytes(out, rng, static_cast<std::size_t>(events * 17));
            }
            const std::uint64_t held = rng.Below(3);
            PackPut<std::uint64_t>(out, held);
            PackRandomBytes(out, rng, static_cast<std::size_t>(held * 5));
            const std::uint64_t samples = rng.Below(40);
            PackPut<std::uint64_t>(out, samples);
            std::uint32_t time = rng.Next();
            for (std::uint64_t s = 0; s < samples; ++s)
                PackPut<std::uint32_t>(out, time += 33);
            // Rope 2: positions that do not match times x vertices x 3.
            const std::uint64_t positions = n == 2 ? samples * 3 + 1 : samples * vertices * 3;
            PackPut<std::uint64_t>(out, positions);
            for (std::uint64_t p = 0; p < positions; ++p)
                PackPut<std::uint32_t>(out, 0x40000000u + static_cast<std::uint32_t>(p % 9) * 1000u + rng.Below(50));
            PackPut<std::uint64_t>(out, samples);
            for (std::uint64_t s = 0; s < samples; ++s)
                PackPut<std::uint32_t>(out, rng.Below(4));
            PackPut<std::uint32_t>(out, rng.Next());
        }
        return out;
    }

    void PackSection(std::vector<std::uint8_t>& clip, const char* tag, std::uint16_t version, const std::vector<std::uint8_t>& payload)
    {
        clip.insert(clip.end(), tag, tag + 4);
        PackPut<std::uint16_t>(clip, version);
        PackPut<std::uint64_t>(clip, payload.size());
        clip.insert(clip.end(), payload.begin(), payload.end());
        PackPut<std::uint64_t>(clip, 0x1234567890ABCDEFull); // the pack does not read the hash
    }

    std::vector<std::uint8_t> MakePackClip(PackRng& rng, std::uint16_t poseVersion, bool unknownMask, std::uint64_t tracks)
    {
        std::vector<std::uint8_t> clip(clipfile::kMagic, clipfile::kMagic + 8);
        PackPut<std::uint32_t>(clip, clipfile::kFormatVersion);
        PackPut<std::uint64_t>(clip, 12345);
        std::vector<std::uint8_t> other;
        PackRandomBytes(other, rng, 300);
        PackSection(clip, "IDAL", 1, other);
        PackSection(clip, "ROPE", clippack::kRopeSectionVersion, MakePackRope(rng, 4));
        PackSection(clip, "ENVR", 1, std::vector<std::uint8_t>(40, 7));
        PackSection(clip, "POSE", poseVersion, MakePackPose(rng, tracks, unknownMask));
        PackSection(clip, "CAMR", 9, other);
        return clip;
    }

    bool PackRoundTrip(const std::vector<std::uint8_t>& clip, const clippack::PackOptions& options, clippack::PackStats& stats,
        std::vector<std::uint8_t>* packedOut = nullptr)
    {
        std::vector<std::uint8_t> packed, back;
        std::string error;
        if (!clippack::Pack(clip.data(), clip.size(), options, packed, error, &stats))
        {
            std::fprintf(stderr, "pack: %s\n", error.c_str());
            return false;
        }
        for (const unsigned threads : {1u, 4u})
        {
            clippack::UnpackOptions unpack;
            unpack.threads = threads;
            if (!clippack::Unpack(packed.data(), packed.size(), unpack, back, error) || back != clip)
            {
                std::fprintf(stderr, "unpack (%u threads): %s\n", threads, error.c_str());
                return false;
            }
        }
        if (packedOut)
            packedOut->swap(packed);
        return true;
    }

    void TestPackedTransforms()
    {
        Check(std::memcmp(clippack::kClipMagic, clipfile::kMagic, 8) == 0 && clippack::kClipFormatVersion == clipfile::kFormatVersion,
            "pack: the transform knows this build's clip format");
        PackRng rng;
        // The pose and rope transforms alone, many random payloads.
        bool poseOk = true, ropeOk = true, splitThreadsOk = true;
        for (int round = 0; round < 40; ++round)
        {
            const std::vector<std::uint8_t> pose = MakePackPose(rng, round == 0 ? 0 : 1 + rng.Below(6));
            clippack::StreamSet streams;
            std::vector<std::uint8_t> back;
            std::string why;
            poseOk = poseOk && clippack::SplitPose(pose.data(), pose.size(), streams, why) &&
                clippack::JoinPose(streams, 1 + static_cast<unsigned>(round % 3), back, why) && back == pose;
            // On several threads (groups of tracks): the same streams.
            clippack::StreamSet threaded;
            splitThreadsOk = splitThreadsOk && clippack::SplitPose(pose.data(), pose.size(), 2 + static_cast<unsigned>(round % 7), threaded, why) &&
                threaded == streams;
            const std::vector<std::uint8_t> rope = MakePackRope(rng, round == 0 ? 0 : 1 + rng.Below(5));
            ropeOk = ropeOk && clippack::SplitRope(rope.data(), rope.size(), streams, why) && clippack::JoinRope(streams, back, why) &&
                back == rope;
        }
        Check(poseOk, "pack: pose transform round trip (every mask bit, layouts, anchors, chunks, odd frame times)");
        Check(ropeOk, "pack: rope transform round trip (with a rope whose positions are not time x vertex)");
        Check(splitThreadsOk, "pack: the pose split on several threads gives the one thread streams");
        {
            const std::vector<std::uint8_t> pose = MakePackPose(rng, 3, true);
            clippack::StreamSet streams;
            std::string why;
            Check(!clippack::SplitPose(pose.data(), pose.size(), streams, why) && why.find("mask") != std::string::npos,
                "pack: an unknown mask bit is refused by the transform");
            std::vector<std::uint8_t> cut(pose.begin(), pose.end() - 5);
            Check(!clippack::SplitPose(cut.data(), cut.size(), streams, why), "pack: a truncated pose payload is refused");
            Check(!clippack::SplitPose(pose.data(), pose.size(), 4, streams, why) && why.find("mask") != std::string::npos,
                "pack: an unknown mask bit is refused on several threads too");
            Check(!clippack::SplitPose(cut.data(), cut.size(), 4, streams, why), "pack: a truncated pose payload is refused on several threads");
        }

        // Whole clips: LZMA and zstd, one and four threads, small blocks
        // (several per stream, the shuffled streams across blocks).
        clippack::PackStats stats;
        const std::vector<std::uint8_t> clip = MakePackClip(rng, clippack::kPoseSectionVersion, false, 6);
        clippack::PackOptions options;
        Check(PackRoundTrip(clip, options, stats) && stats.pose && stats.rope, "pack: lzma round trip, pose and rope transformed");
        options.threads = 4;
        options.blockBytes = 4096;
        Check(PackRoundTrip(clip, options, stats) && stats.pose && stats.rope, "pack: small blocks, four threads");
        options.codec = clippack::Codec::Zstd;
        Check(PackRoundTrip(clip, options, stats), "pack: zstd blocks");
        options.codec = clippack::Codec::Stored;
        Check(PackRoundTrip(clip, options, stats), "pack: stored blocks");
        options = clippack::PackOptions{};
        options.transform = false;
        Check(PackRoundTrip(clip, options, stats) && !stats.pose && !stats.rope, "pack: without the transform");
        options = clippack::PackOptions{};
        // A pose section of another version, or with an unknown mask bit,
        // stays in the rest stream: still identical.
        const std::vector<std::uint8_t> newer = MakePackClip(rng, static_cast<std::uint16_t>(clippack::kPoseSectionVersion + 1), false, 2);
        Check(PackRoundTrip(newer, options, stats) && !stats.pose && stats.rope && !stats.poseSkip.empty(),
            "pack: an unknown pose version is stored as it is");
        const std::vector<std::uint8_t> masked = MakePackClip(rng, clippack::kPoseSectionVersion, true, 3);
        Check(PackRoundTrip(masked, options, stats) && !stats.pose && stats.poseSkip.find("mask") != std::string::npos,
            "pack: unknown mask bits are stored as they are");
        // A clip of another format version: no transform, identical.
        std::vector<std::uint8_t> otherFormat = clip;
        otherFormat[8] = static_cast<std::uint8_t>(clipfile::kFormatVersion + 1);
        Check(PackRoundTrip(otherFormat, options, stats) && !stats.pose && !stats.rope, "pack: another clip format version");
        std::vector<std::uint8_t> packed, out;
        std::string error;
        Check(!clippack::Pack(packed.data(), 0, options, packed, error), "pack: an empty input is refused");

        // Every one byte change and every truncation of a packed clip is an
        // error, never a crash or a wrong clip.
        Check(PackRoundTrip(MakePackClip(rng, clippack::kPoseSectionVersion, false, 2), options, stats, &packed), "pack: small clip");
        clippack::UnpackOptions unpack;
        unpack.threads = 2;
        bool flipsRefused = true, cutsRefused = true;
        for (std::size_t at = 0; at < packed.size(); ++at)
        {
            std::vector<std::uint8_t> damaged = packed;
            damaged[at] ^= static_cast<std::uint8_t>(1u << (at % 8));
            flipsRefused = flipsRefused && !clippack::Unpack(damaged.data(), damaged.size(), unpack, out, error) && out.empty() &&
                !error.empty();
        }
        for (std::size_t size = 0; size < packed.size(); size += 1 + size / 64)
            cutsRefused = cutsRefused && !clippack::Unpack(packed.data(), size, unpack, out, error) && out.empty();
        Check(flipsRefused, "pack: every flipped byte refused");
        Check(cutsRefused, "pack: every truncation refused");
        std::vector<std::uint8_t> trailing = packed;
        trailing.push_back(0x42);
        Check(!clippack::Unpack(trailing.data(), trailing.size(), unpack, out, error) && error.find("trailing") != std::string::npos,
            "pack: trailing bytes that are not a skippable frame refused");
        unpack.maxClipBytes = 100;
        Check(!clippack::Unpack(packed.data(), packed.size(), unpack, out, error), "pack: a clip over the limit refused");
        // Cancelled.
        std::atomic<bool> cancel{true};
        options.cancel = &cancel;
        Check(!clippack::Pack(clip.data(), clip.size(), options, packed, error) && error == "cancelled", "pack: cancel");
    }

    // The real format end to end: an encoded clip packed (its POSE and ROPE
    // through the transform), the gallery head, Load, the background
    // compressor in place and its upgrade of a zstd file.
    void TestPackedFiles()
    {
        Clip saved;
        MakeClip(saved);
        const std::wstring plainPath = TempPath(L"replaypoc_pack_plain.rpclip");
        const std::wstring workPath = TempPath(L"replaypoc_pack_work.rpclip");
        std::string error;
        Check(clipfile::Save(saved.recording, saved.poses, saved.camera, 4321, kBase, kImageSize, plainPath, error), "packed: save");
        std::vector<std::uint8_t> plain;
        Check(clipcompress::ReadWholeFile(plainPath, plain, nullptr, error), "packed: read the plain clip");
        clippack::PackStats stats;
        std::vector<std::uint8_t> packed;
        clippack::PackOptions options;
        Check(clippack::Pack(plain.data(), plain.size(), options, packed, error, &stats) && stats.pose && stats.rope,
            "packed: this build's POSE and ROPE go through the transform");
        Check(clipcompress::IsCompressed(packed.data(), packed.size()) && clipcompress::IsPacked(packed.data(), packed.size()) &&
                !clipcompress::IsZstd(packed.data(), packed.size()) && clipcompress::ContentSize(packed.data(), packed.size()) == plain.size(),
            "packed: detected, content size");
        std::vector<std::uint8_t> summary;
        Check(clipcompress::Summary(plain.data(), plain.size(), summary), "packed: summary");
        std::vector<std::uint8_t> withSummary = packed;
        clipcompress::AppendSummaryFrame(summary, withSummary);
        std::vector<std::uint8_t> back;
        Check(clipcompress::Decompress(withSummary.data(), withSummary.size(), back, error) && back == plain,
            "packed: Decompress with the summary frame");
        Check(clipcompress::Verify(withSummary.data(), withSummary.size(), plain.data(), plain.size(), nullptr, error), "packed: Verify");

        Check(WriteBytes(workPath, withSummary), "packed: write");
        clipcompress::FileHead head;
        Check(clipcompress::ReadHead(workPath, head) && head.compressed && head.packed && head.summary == summary &&
                head.clipBytes == plain.size() && head.diskBytes == withSummary.size(),
            "packed: head from the summary frame");
        Check(WriteBytes(workPath, packed) && clipcompress::ReadHead(workPath, head) && head.packed && head.summary == summary,
            "packed: head without the summary frame");
        Check(clipcompress::Peek(workPath) == clipcompress::Kind::Packed, "packed: Peek");
        Clip loaded;
        std::uint64_t duration = 0;
        clipfile::LoadInfo info;
        Check(WriteBytes(workPath, withSummary) &&
                clipfile::Load(workPath, kBase, kImageSize, loaded.recording, loaded.poses, loaded.camera, duration, error, &info) &&
                info.compressed && info.packed && info.clipBytes == plain.size() && EncodeLoaded(loaded, duration) == plain,
            "packed: Load equals the plain clip");
        CompareClips(saved, loaded);

        // Damaged files refused by Load, the destination unchanged.
        Clip target;
        MakeClip(target);
        const std::size_t shotsBefore = target.recording.shots.Events().size();
        std::uint64_t keep = 77;
        std::vector<std::uint8_t> corrupt = withSummary;
        corrupt[packed.size() / 2] ^= 0x5A;
        Check(WriteBytes(workPath, corrupt) &&
                !clipfile::Load(workPath, kBase, kImageSize, target.recording, target.poses, target.camera, keep, error) &&
                error.find("packed clip") != std::string::npos && keep == 77 && target.recording.shots.Events().size() == shotsBefore,
            "packed: a damaged file is refused");
        std::vector<std::uint8_t> truncated(packed.begin(), packed.begin() + static_cast<std::ptrdiff_t>(packed.size() / 3));
        Check(WriteBytes(workPath, truncated) &&
                !clipfile::Load(workPath, kBase, kImageSize, target.recording, target.poses, target.camera, keep, error) &&
                error.find("packed clip") != std::string::npos && keep == 77,
            "packed: a truncated file is refused");
        std::vector<std::uint8_t> headerOnly(packed.begin(), packed.begin() + 20);
        Check(WriteBytes(workPath, headerOnly) &&
                !clipfile::Load(workPath, kBase, kImageSize, target.recording, target.poses, target.camera, keep, error) && keep == 77,
            "packed: a header only file is refused");

        // The background compressor: a plain file packed in place (times
        // kept), then skipped; a zstd file upgraded.
        const std::uint64_t old = clipcompress::NowTicks() - 30 * clipcompress::kTicksPerDay;
        Check(WriteBytes(workPath, plain), "packed: plain copy");
        SetOldTimes(workPath, old);
        clipcompress::CompressOptions compress;
        clipcompress::FileResult result = clipcompress::CompressFile(workPath, compress, nullptr);
        if (!result.ok)
            std::fprintf(stderr, "packed compress file: %s\n", result.reason.c_str());
        Check(result.ok && !result.upgraded && !result.plainPack && result.bytesOut < plain.size(), "packed: file packed in place");
        Check(clipcompress::Peek(workPath) == clipcompress::Kind::Packed && !std::filesystem::exists(workPath + L".ztmp"),
            "packed: the file is packed, no temporary file left");
        clipcompress::FileTimes times;
        Check(clipcompress::GetTimes(workPath, times) && times.lastWrite == old, "packed: the original times kept");
        Check(clipcompress::ReadHead(workPath, head) && head.packed && head.summary == summary, "packed: in place head");
        Clip inPlace;
        Check(clipfile::Load(workPath, kBase, kImageSize, inPlace.recording, inPlace.poses, inPlace.camera, duration, error) &&
                EncodeLoaded(inPlace, duration) == plain,
            "packed: in place load");
        result = clipcompress::CompressFile(workPath, compress, nullptr);
        Check(!result.ok && result.skipped && result.reason == "already_compressed", "packed: a packed file is skipped");

        Check(WriteBytes(workPath, plain), "packed: plain copy again");
        SetOldTimes(workPath, old);
        result = clipcompress::CompressFile(workPath, 19, nullptr);
        Check(result.ok && clipcompress::Peek(workPath) == clipcompress::Kind::Compressed, "packed: zstd file for the upgrade");
        const std::uint64_t zstdBytes = result.bytesOut;
        compress.upgrade = false;
        result = clipcompress::CompressFile(workPath, compress, nullptr);
        Check(!result.ok && result.skipped && result.reason == "already_compressed", "packed: no upgrade when it is off");
        compress.upgrade = true;
        result = clipcompress::CompressFile(workPath, compress, nullptr);
        // This small clip may pack larger than its zstd frame (no_gain: the
        // zstd file stays); a real clip packs about 40% smaller.
        Check(result.upgraded && result.bytesIn == zstdBytes &&
                (result.ok ? clipcompress::Peek(workPath) == clipcompress::Kind::Packed
                           : result.skipped && result.reason == "no_gain" && clipcompress::Peek(workPath) == clipcompress::Kind::Compressed),
            "packed: a zstd file upgraded (or kept when not smaller)");
        Clip upgraded;
        Check(clipfile::Load(workPath, kBase, kImageSize, upgraded.recording, upgraded.poses, upgraded.camera, duration, error) &&
                EncodeLoaded(upgraded, duration) == plain && clipcompress::GetTimes(workPath, times) && times.lastWrite == old,
            "packed: the upgraded file loads the same clip, times kept");

        std::error_code ignored;
        std::filesystem::remove(plainPath, ignored);
        std::filesystem::remove(workPath, ignored);
    }

    // The save's pack (clipfile::Save with SaveOptions::pack): the file is
    // written packed with zstd blocks and loads the same clip; a pack that
    // fails (low memory, or after the clip's bytes were taken) writes the
    // clip plain; the background compressor repacks the fast pack with LZMA
    // and then leaves it.
    void TestSavePack()
    {
        Clip saved;
        MakeClip(saved);
        const std::wstring plainPath = TempPath(L"replaypoc_savepack_plain.rpclip");
        const std::wstring packPath = TempPath(L"replaypoc_savepack_packed.rpclip");
        std::string error;
        Check(clipfile::Save(saved.recording, saved.poses, saved.camera, 4321, kBase, kImageSize, plainPath, error), "save pack: plain save");
        std::vector<std::uint8_t> plain;
        Check(clipcompress::ReadWholeFile(plainPath, plain, nullptr, error), "save pack: read the plain clip");
        std::vector<std::uint8_t> summary;
        Check(clipcompress::Summary(plain.data(), plain.size(), summary), "save pack: summary");

        // Packed at save, round trip.
        clipfile::SaveOptions options;
        options.pack = true;
        clipfile::SaveInfo info;
        Check(clipfile::Save(saved.recording, saved.poses, saved.camera, 4321, kBase, kImageSize, packPath, error, options, &info) &&
                info.packed && info.fallback.empty() && info.clipBytes == plain.size(),
            "save pack: saved packed");
        std::vector<std::uint8_t> file;
        Check(clipcompress::ReadWholeFile(packPath, file, nullptr, error) && clipcompress::IsPacked(file.data(), file.size()) &&
                info.fileBytes == file.size() && !std::filesystem::exists(packPath + L".tmp"),
            "save pack: the file is the packed container, no temporary file left");
        clippack::HeaderInfo header;
        Check(clippack::ReadHeaderInfo(file.data(), file.size(), header) && clippack::FastPacked(header) && header.lzma == 0 &&
                header.clipBytes == plain.size(),
            "save pack: its blocks are zstd (a fast pack)");
        Check(clipcompress::IsFastPackedFile(packPath) && clipcompress::Peek(packPath) == clipcompress::Kind::Packed, "save pack: fast packed file");
        std::vector<std::uint8_t> back;
        Check(clipcompress::Decompress(file.data(), file.size(), back, error) && back == plain, "save pack: unpacks to the plain clip byte for byte");
        clipcompress::FileHead head;
        Check(clipcompress::ReadHead(packPath, head) && head.packed && head.summary == summary && head.clipBytes == plain.size(),
            "save pack: the clip list's summary frame");
        Clip loaded;
        std::uint64_t duration = 0;
        clipfile::LoadInfo loadInfo;
        Check(clipfile::Load(packPath, kBase, kImageSize, loaded.recording, loaded.poses, loaded.camera, duration, error, &loadInfo) &&
                loadInfo.packed && EncodeLoaded(loaded, duration) == plain,
            "save pack: Load equals the plain clip");

        // PackForSave alone: not a clip leaves the input, a cancel takes it.
        {
            std::vector<std::uint8_t> notClip(256, 0x11);
            std::vector<std::uint8_t> out;
            clipcompress::SavePackResult result;
            Check(!clipcompress::PackForSave(notClip, clipcompress::SavePackOptions{}, out, result) && result.fallback == "not_a_clip" &&
                    notClip.size() == 256 && out.empty(),
                "save pack: not a clip, the input kept");
            std::vector<std::uint8_t> copy = plain;
            clipcompress::SavePackOptions low;
            low.availableBytes = 1;
            Check(!clipcompress::PackForSave(copy, low, out, result) && result.fallback == "low_memory" && copy == plain && out.empty(),
                "save pack: low memory, the clip kept");
            std::atomic<bool> cancel{true};
            clipcompress::SavePackOptions cancelled;
            cancelled.cancel = &cancel;
            Check(!clipcompress::PackForSave(copy, cancelled, out, result) && result.fallback.find("cancelled") != std::string::npos &&
                    out.empty(),
                "save pack: a failed pack");
        }

        // A failed pack is a plain save, not a failed one.
        clipfile::SaveOptions lowMemory = options;
        lowMemory.availableBytes = 1;
        Check(clipfile::Save(saved.recording, saved.poses, saved.camera, 4321, kBase, kImageSize, packPath, error, lowMemory, &info) &&
                !info.packed && info.fallback == "low_memory" && clipcompress::ReadWholeFile(packPath, file, nullptr, error) && file == plain,
            "save pack: low memory writes the clip plain");
        std::atomic<bool> cancel{true};
        clipfile::SaveOptions failing = options;
        failing.cancel = &cancel;
        Check(clipfile::Save(saved.recording, saved.poses, saved.camera, 4321, kBase, kImageSize, packPath, error, failing, &info) &&
                !info.packed && info.fallback.find("cancelled") != std::string::npos &&
                clipcompress::ReadWholeFile(packPath, file, nullptr, error) && file == plain && !std::filesystem::exists(packPath + L".tmp"),
            "save pack: a pack that fails after taking the clip's bytes writes the clip plain (encoded again)");

        // The background compressor: a fast pack is repacked with LZMA, then
        // left; with the upgrade off it is left at once.
        Check(clipfile::Save(saved.recording, saved.poses, saved.camera, 4321, kBase, kImageSize, packPath, error, options, &info) &&
                info.packed,
            "save pack: packed again for the compressor");
        const std::uint64_t old = clipcompress::NowTicks() - 30 * clipcompress::kTicksPerDay;
        SetOldTimes(packPath, old);
        clipcompress::CompressOptions compress;
        compress.upgrade = false;
        clipcompress::FileResult result = clipcompress::CompressFile(packPath, compress, nullptr);
        Check(!result.ok && result.skipped && result.reason == "already_compressed" && clipcompress::IsFastPackedFile(packPath),
            "save pack: no repack when the upgrade is off");
        compress.upgrade = true;
        result = clipcompress::CompressFile(packPath, compress, nullptr);
        if (!result.ok)
            std::fprintf(stderr, "save pack compress file: %s\n", result.reason.c_str());
        // This small clip may not get smaller (no_gain keeps the fast pack);
        // a real clip packs about 15% smaller with LZMA.
        Check(result.upgraded && result.fastPacked && result.bytesIn == info.fileBytes &&
                (result.ok ? !clipcompress::IsFastPackedFile(packPath) && clipcompress::Peek(packPath) == clipcompress::Kind::Packed
                           : result.skipped && result.reason == "no_gain" && clipcompress::IsFastPackedFile(packPath)),
            "save pack: the compressor repacks the fast pack with LZMA");
        if (result.ok)
        {
            Check(clipcompress::ReadWholeFile(packPath, file, nullptr, error) && clippack::ReadHeaderInfo(file.data(), file.size(), header) &&
                    header.zstd == 0 && header.lzma > 0,
                "save pack: the repacked file has LZMA blocks only");
            result = clipcompress::CompressFile(packPath, compress, nullptr);
            Check(!result.ok && result.skipped && result.reason == "already_compressed", "save pack: an LZMA pack is not repacked again");
        }
        Clip upgraded;
        clipcompress::FileTimes times;
        Check(clipfile::Load(packPath, kBase, kImageSize, upgraded.recording, upgraded.poses, upgraded.camera, duration, error) &&
                EncodeLoaded(upgraded, duration) == plain && clipcompress::GetTimes(packPath, times) && times.lastWrite == old,
            "save pack: the repacked file loads the same clip, times kept");
        Check(clipcompress::ReadHead(packPath, head) && head.packed && head.summary == summary, "save pack: repacked head");
        // Last: the comparison may touch `saved` (the saves above encode it).
        CompareClips(saved, loaded);

        std::error_code ignored;
        std::filesystem::remove(plainPath, ignored);
        std::filesystem::remove(packPath, ignored);
    }

    void TestFragPiecesSection()
    {
        const auto loadSkipped = [](Clip& clip, Clip& out, std::vector<clipfile::SkippedSection>& skipped) {
            const std::vector<std::uint8_t> bytes = EncodeClip(clip);
            std::uint64_t duration = 0;
            std::string error;
            return clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, out.recording, out.poses, out.camera,
                duration, error, &skipped);
        };
        {
            Clip clip;
            MakeClip(clip);
            clip.recording.fragPieces[0].groups = {15, 14};
            Clip out;
            std::vector<clipfile::SkippedSection> skipped;
            Check(loadSkipped(clip, out, skipped) && skipped.size() == 1 && skipped[0].section == std::string("frag pieces") &&
                    out.recording.fragPieces.empty(),
                "frag pieces: groups out of order leave the section out");
        }
        {
            Clip clip;
            MakeClip(clip);
            clip.recording.fragPieces[0].source = 7;
            Clip out;
            std::vector<clipfile::SkippedSection> skipped;
            Check(loadSkipped(clip, out, skipped) && skipped.size() == 1 && out.recording.fragPieces.empty(),
                "frag pieces: an unknown source leaves the section out");
        }
        {
            Clip clip;
            MakeClip(clip);
            clip.recording.fragPieces[0].groups = {1024};
            Clip out;
            std::vector<clipfile::SkippedSection> skipped;
            Check(loadSkipped(clip, out, skipped) && skipped.size() == 1 && out.recording.fragPieces.empty(),
                "frag pieces: a group past the engine's bitset leaves the section out");
        }
    }

    // ESND offsets that the recorder cannot produce are refused at load; the
    // ones it produces still load.
    void TestEntitySoundOffsets()
    {
        std::string error;
        {
            Clip clip;
            MakeClip(clip);
            Check(!DecodeFails(EncodeClip(clip), error), "esnd: a static audio in the image loads");
            clip.recording.entitySounds[0].audioRva = 0;
            clip.recording.entitySounds[0].audioOnEntity = true;
            clip.recording.entitySounds[0].audioOffset = 0x100;
            Check(!DecodeFails(EncodeClip(clip), error), "esnd: an audio inside the entity loads");
        }
        const auto refused = [&](void (*change)(EntitySound&), const char* what) {
            Clip clip;
            MakeClip(clip);
            change(clip.recording.entitySounds[0]);
            Check(DecodeSkips(EncodeClip(clip), "entity sounds", error), what); // skipped, the clip opens
        };
        refused([](EntitySound& e) { e.audioRva = kImageSize; }, "esnd: an image offset past the image is refused");
        refused([](EntitySound& e) { e.audioRva = 0xFFFFFFFFFFFFFFF0ull; }, "esnd: a huge image offset is refused");
        refused([](EntitySound& e) { e.audioRva = 0x5B9F961u; }, "esnd: an unaligned image offset is refused");
        refused([](EntitySound& e) { e.audioRva = 0; }, "esnd: a static audio without offset is refused");
        refused([](EntitySound& e) { e.audioOffset = 0x10; }, "esnd: a static audio with an entity offset is refused");
        refused([](EntitySound& e) {
            e.audioOnEntity = true;
            e.audioRva = 0;
            e.audioOffset = kEntitySoundSpan;
        }, "esnd: an entity offset past the span is refused");
        refused([](EntitySound& e) {
            e.audioOnEntity = true;
            e.audioRva = 0;
            e.audioOffset = 0x103;
        }, "esnd: an unaligned entity offset is refused");
        refused([](EntitySound& e) {
            e.audioOnEntity = true;
            e.audioOffset = 0x100;
        }, "esnd: an entity audio with an image offset too is refused");
        refused([](EntitySound& e) { e.trackerOffset = 0xFFFFFFFFu; }, "esnd: a tracker offset past the span is refused");
        refused([](EntitySound& e) { e.soundRef = 0x1ABF5BCB8ull; }, "esnd: a hash wider than 32 bits is refused");
        // A ped audio sound names an offset in the ped audio holder.
        refused([](EntitySound& e) {
            e.pedAudio = true;
            e.audioOnEntity = true;
            e.audioRva = 0;
            e.trackerOffset = 0;
            e.audioOffset = kEntitySoundPedAudioSpan;
        }, "esnd: a ped audio offset past the holder is refused");
        refused([](EntitySound& e) {
            e.pedAudio = true;
            e.trackerOffset = 0;
        }, "esnd: a ped audio sound on a static audio is refused");
        {
            EntitySound ped;
            ped.pedAudio = true;
            ped.audioOnEntity = true;
            ped.audioOffset = 0x4400u;
            Check(EntitySoundAudioValid(ped, kImageSize), "esnd: a ped audio offset inside the holder is valid");
        }
        // The same rule playback applies before the call.
        EntitySound ok;
        ok.audioRva = 0x5B9F960u;
        Check(EntitySoundAudioValid(ok, kImageSize) && !EntitySoundAudioValid(ok, 0x5B9F960u) && EntitySoundAudioValid(ok, 0),
            "esnd: the image bound is the running image's size");
    }

    // A sparse file of `size` bytes starting with `head`: no disk space and
    // no write of the whole size.
    bool MakeSparse(const std::wstring& path, const std::vector<std::uint8_t>& head, std::uint64_t size)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        DWORD returned = 0;
        bool ok = DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) != FALSE;
        DWORD written = 0;
        ok = ok && ::WriteFile(file, head.data(), static_cast<DWORD>(head.size()), &written, nullptr) && written == head.size();
        LARGE_INTEGER end{};
        end.QuadPart = static_cast<LONGLONG>(size);
        ok = ok && SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && SetEndOfFile(file);
        CloseHandle(file);
        return ok;
    }

    // A file that is not a clip, a truncated or corrupt one and a huge one are
    // refused before the whole file is read, without a crash or a large
    // allocation.
    void TestLoadLimits()
    {
        std::string error;
        constexpr std::uint64_t kMb = 1024 * 1024;
        Check(!clipfile::CheckLoadBudget(2048 * kMb, 2048 * kMb, false, 64ull << 30, error) && error.find("too large") != std::string::npos,
            "load budget: a 2 GB file is refused");
        Check(!clipfile::CheckLoadBudget(10 * kMb, 4096 * kMb, true, 64ull << 30, error) && error.find("too large") != std::string::npos,
            "load budget: a compressed clip that claims 4 GB is refused");
        Check(clipfile::CheckLoadBudget(100 * kMb, 100 * kMb, false, 8ull << 30, error), "load budget: 100 MB with 8 GB free loads");
        Check(!clipfile::CheckLoadBudget(400 * kMb, 400 * kMb, false, 1ull << 30, error) &&
                error.find("not enough memory") != std::string::npos,
            "load budget: 400 MB with 1 GB free is refused (decode needs about 3x)");
        Check(!clipfile::CheckLoadBudget(100 * kMb, 700 * kMb, true, 2ull << 30, error), "load budget: the decompressed size counts");
        Check(clipfile::CheckLoadBudget(100 * kMb, 0, true, 0, error), "load budget: memory unknown, the size limits only");
        // A packed file also holds its streams (file + 2.25 x clip), still
        // under the decode's 3 x clip: the same limit as zstd.
        Check(clipfile::CheckLoadBudget(100 * kMb, 450 * kMb, true, 3 * 450 * kMb + clipfile::kLoadReserveBytes, error, true),
            "load budget: a packed 450 MB clip fits what the decode needs");
        Check(!clipfile::CheckLoadBudget(100 * kMb, 450 * kMb, true, 3 * 450 * kMb + clipfile::kLoadReserveBytes - 1, error, true) &&
                error.find("not enough memory") != std::string::npos,
            "load budget: a packed clip refused below that");

        Clip target;
        MakeClip(target);
        const std::size_t shotsBefore = target.recording.shots.Events().size();
        std::uint64_t duration = 77;
        const auto load = [&](const std::wstring& path) {
            return clipfile::Load(path, kBase, kImageSize, target.recording, target.poses, target.camera, duration, error);
        };

        // Not a clip.
        const std::wstring junkPath = TempPath(L"replaypoc_clip_junk.rpclip");
        std::vector<std::uint8_t> junk(1 << 20);
        Fill(junk.data(), junk.size(), 0x5A);
        WriteBytes(junkPath, junk);
        Check(!load(junkPath) && error.find("magic") != std::string::npos, "load: a file of another kind is refused");
        WriteBytes(junkPath, {});
        Check(!load(junkPath) && error.find("magic") != std::string::npos, "load: an empty file is refused");

        // Another format version: refused from the header.
        Clip clip;
        MakeClip(clip);
        std::vector<std::uint8_t> bytes = EncodeClip(clip);
        std::vector<std::uint8_t> future = bytes;
        future[8] = 99;
        WriteBytes(junkPath, future);
        Check(!load(junkPath) && error.find("format version 99") != std::string::npos, "load: another format is refused");

        // Truncated and corrupt files on disk.
        std::vector<std::uint8_t> half(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() / 2));
        WriteBytes(junkPath, half);
        Check(!load(junkPath) && !error.empty(), "load: a truncated file is refused");
        std::vector<std::uint8_t> flipped = bytes;
        flipped[bytes.size() / 3] ^= 0x40;
        WriteBytes(junkPath, flipped);
        Check(!load(junkPath) && !error.empty(), "load: a corrupt file is refused");

        // A compressed frame that claims 1 TB: refused from its header.
        std::vector<std::uint8_t> frame = {0x28, 0xB5, 0x2F, 0xFD, 0xE0};
        const std::uint64_t claimed = 1ull << 40;
        for (int n = 0; n < 8; ++n)
            frame.push_back(static_cast<std::uint8_t>(claimed >> (8 * n)));
        frame.insert(frame.end(), {0x01, 0x00, 0x00}); // a last raw block, empty
        WriteBytes(junkPath, frame);
        const auto started = GetTickCount64();
        Check(!load(junkPath) && error.find("too large") != std::string::npos, "load: a compressed clip claiming 1 TB is refused");

        // A 1.5 GB file with a clip header (sparse): refused by its size
        // before it is read.
        const std::wstring hugePath = TempPath(L"replaypoc_clip_huge.rpclip");
        std::vector<std::uint8_t> head(bytes.begin(), bytes.begin() + 20);
        if (MakeSparse(hugePath, head, 1536 * kMb))
        {
            Check(!load(hugePath) && error.find("too large") != std::string::npos, "load: a 1.5 GB file is refused by its size");
            Check(GetTickCount64() - started < 5000, "load: the huge files are refused without reading them");
        }
        else
            std::fprintf(stderr, "note: sparse file not made, huge file check skipped\n");
        Check(target.recording.shots.Events().size() == shotsBefore && duration == 77, "load: the refusals changed nothing");
        std::error_code ignored;
        std::filesystem::remove(hugePath, ignored);
        std::filesystem::remove(junkPath, ignored);

        // The good clip still loads through the checks.
        const std::wstring goodPath = TempPath(L"replaypoc_clip_good.rpclip");
        WriteBytes(goodPath, bytes);
        Check(load(goodPath) && duration == 1000, "load: a clip loads after the checks");
        std::filesystem::remove(goodPath, ignored);
    }

    // Values the sections parse but playback cannot use are refused; values of
    // a known range are brought into it.
    void TestValidateLoaded()
    {
        std::string error;
        const auto fails = [&](Clip& clip, std::uint64_t duration = 1000) { return DecodeFails(EncodeClip(clip, duration), error); };
        {
            Clip clip;
            MakeClip(clip);
            clip.camera.FindMutable(1)->pose.position.y = std::numeric_limits<float>::quiet_NaN();
            Check(fails(clip) && error.find("camera") != std::string::npos, "validate: a marker at NaN is refused");
        }
        {
            Clip clip;
            MakeClip(clip);
            clip.camera.FindMutable(3)->pose.orientation = {0.0f, 0.0f, 0.0f, 0.0f};
            Check(fails(clip) && error.find("camera") != std::string::npos, "validate: a null orientation is refused");
        }
        {
            Clip clip;
            MakeClip(clip);
            clip.camera.FindMutable(3)->pose.fov = std::numeric_limits<float>::infinity();
            Check(fails(clip), "validate: an infinite FOV is refused");
        }
        {
            Clip clip;
            MakeClip(clip);
            clip.camera.FindMutable(3)->id = 1;
            Check(fails(clip) && error.find("share an id") != std::string::npos, "validate: two markers with one id are refused");
        }
        {
            Clip clip;
            MakeClip(clip);
            Check(fails(clip, 0x100000000ull) && error.find("duration") != std::string::npos,
                "validate: a duration past the u32 timestamps is refused");
        }
        {
            Clip clip;
            MakeClip(clip);
            camera::GameSample bad{};
            bad.timeMs = 999;
            bad.fov = std::numeric_limits<float>::quiet_NaN();
            clip.recording.gameCamera.push_back(bad);
            Check(fails(clip) && error.find("gameCamera") != std::string::npos, "validate: a game camera sample at NaN is refused");
        }
        {
            Clip clip;
            MakeClip(clip);
            clip.camera.FindMutable(3)->pose.fov = 500.0f;
            const ReplayId vehicle = clip.recording.created[0].id;
            clip.recording.burnLevels.push_back({vehicle.Packed(), {{100u, -2.0f}, {400u, 3.5f}}, 0, false});
            Clip loaded;
            std::uint64_t duration = 0;
            const std::vector<std::uint8_t> bytes = EncodeClip(clip);
            const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
                loaded.camera, duration, error);
            Check(ok && loaded.camera.Find(3)->pose.fov == 170.0f && loaded.recording.burnLevels.size() == 1 &&
                    loaded.recording.burnLevels[0].states[0].second == 0.0f && loaded.recording.burnLevels[0].states[1].second == 1.0f,
                "validate: a FOV and burn levels out of range load, brought into range");
        }
    }

    // Two clips asked for in the same second get two files; the readable name
    // stays when it is free.
    void TestUniqueClipName()
    {
        std::vector<std::wstring> onDisk;
        std::vector<std::wstring> reserved;
        const auto taken = [&](const std::wstring& name) {
            return std::find(onDisk.begin(), onDisk.end(), name) != onDisk.end() ||
                std::find(reserved.begin(), reserved.end(), name) != reserved.end();
        };
        const std::wstring second = L"clip-20260929-213005";
        const std::wstring first = clipfile::UniqueClipName(second, taken);
        reserved.push_back(first); // its save is not written yet
        const std::wstring again = clipfile::UniqueClipName(second, taken);
        onDisk.push_back(again);
        const std::wstring third = clipfile::UniqueClipName(second, taken);
        Check(first == second, "name: a free name keeps the readable form");
        Check(again == second + L"-2" && third == second + L"-3", "name: the same second gets -2, -3 (a save not written counts)");
        Check(clipfile::UniqueClipName(L"clip-20260929-213005-p1", taken) == L"clip-20260929-213005-p1", "name: a part keeps its suffix");
        Check(clipfile::UniqueClipName(second, [](const std::wstring&) { return true; }).empty(), "name: none free, no name");
    }

    // Clip names "NNN-Place": the counter and the place of a new clip's name.
    void TestNumberedClipName()
    {
        Check(clipfile::ClipPlaceTag("Valentine", "Heartland Overflow", "New Hanover") == "Valentine", "place: the town first");
        Check(clipfile::ClipPlaceTag("", "Scarlett Meadows", "Lemoyne") == "ScarlettMeadows", "place: then the district, CamelCase");
        Check(clipfile::ClipPlaceTag("", "", "New Austin") == "NewAustin", "place: then the state");
        Check(clipfile::ClipPlaceTag("", "", "") == "Wild" && clipfile::ClipPlaceTag(nullptr, nullptr, nullptr) == "Wild",
            "place: none, Wild");
        Check(clipfile::ClipPlaceTag("Saint Denis", "", "") == "SaintDenis", "place: no spaces");
        Check(clipfile::ClipPlaceTag("O'Creagh's Run", "", "") == "OCreaghsRun", "place: an apostrophe starts no word");
        Check(clipfile::ClipPlaceTag("thieves_landing", "", "") == "ThievesLanding", "place: each word capitalized");
        Check(clipfile::ClipPlaceTag("S\xC3\xA3o Jos\xC3\xA9 \xC3\x86on", "", "") == "SaoJoseAEon", "place: accents to ASCII");
        Check(clipfile::ClipPlaceTag(" - ", "\xE2\x80\x94", "Lemoyne") == "Lemoyne", "place: a text with no letter is empty");
        Check(clipfile::ClipPlaceTag(std::string(80, 'a').c_str(), "", "").size() == 40, "place: at most 40 characters");

        std::uint32_t number = 0;
        Check(clipfile::ClipNumberOf(L"042-Valentine", number) && number == 42, "number: NNN-");
        Check(clipfile::ClipNumberOf(L"ReplayClips\\1000-Wild-p2.rpclip", number) && number == 1000, "number: four digits, a path");
        Check(!clipfile::ClipNumberOf(L"clip-20260929-213005", number), "number: the old names do not count");
        Check(!clipfile::ClipNumberOf(L"42-Valentine", number) && !clipfile::ClipNumberOf(L"042Valentine", number) &&
                !clipfile::ClipNumberOf(L"042", number) && !clipfile::ClipNumberOf(L"", number),
            "number: three digits and a dash");
        Check(!clipfile::ClipNumberOf(L"12345678901-x", number), "number: no overflow");

        Check(clipfile::NextClipNumber({}) == 1, "next: the first clip is 001");
        Check(clipfile::NextClipNumber({L"clip-20260929-213005", L"dress"}) == 1, "next: old names only, 001");
        Check(clipfile::NextClipNumber({L"ReplayClips\\007-Wild.rpclip", L"clip-20260929-213005.rpclip",
                  L"ReplayClips\\041-SaintDenis-p3.rpclip", L"003-Valentine-2.rpclip"}) == 42,
            "next: the highest plus one, parts and collisions too");
        Check(clipfile::NextClipNumber({L"999-Wild"}) == 1000, "next: four digits after 999");

        Check(clipfile::NumberedClipBase(42, "Valentine", L"") == L"042-Valentine", "base: three digits");
        Check(clipfile::NumberedClipBase(1000, "Wild", L"-p2") == L"1000-Wild-p2", "base: four digits, the part suffix");
        Check(clipfile::NumberedClipBase(7, "SaintDenis", L"") == L"007-SaintDenis", "base: zero padded");
    }

    // The save transaction. A job owns its data until written; a failure keeps
    // it and retries; a newer save of the same file replaces it; a load of that
    // file reads it.
    void TestSaveQueue()
    {
        using Queue = savequeue::Queue<int>;
        {
            Queue queue;
            const std::uint64_t id = queue.Add(L"A", 1);
            Queue::Job* job = queue.Begin(0);
            Check(job && job->id == id && job->payload == 1 && queue.Begin(0) == nullptr, "queue: one job written at a time");
            Queue::Job out;
            Check(queue.Finish(id, false, "disk full", 1000, &out) == savequeue::Outcome::Held && out.failures == 1 &&
                    queue.HeldCount() == 1 && queue.Find(L"A") && queue.Find(L"A")->payload == 1,
                "queue: a failed write keeps the job and its data");
            Check(queue.Begin(1000 + savequeue::RetryDelayMs(1) - 1) == nullptr, "queue: the retry waits its delay");
            job = queue.Begin(1000 + savequeue::RetryDelayMs(1));
            Check(job && job->payload == 1, "queue: then the same data is written again");
            Check(queue.Finish(id, false, "disk full", 7000) == savequeue::Outcome::Held &&
                    queue.Find(L"A")->retryAtMs == 7000 + savequeue::RetryDelayMs(2),
                "queue: the delay grows");
            queue.RetryNow();
            job = queue.Begin(7001);
            Check(job != nullptr, "queue: a save asked again retries at once");
            Check(queue.Finish(id, true, "", 8000, &out) == savequeue::Outcome::Saved && out.payload == 1 && queue.Empty(),
                "queue: written, the job goes");
        }
        {
            // Edits of A fail to save, B is opened and A is left: A's data
            // stays owned by its job until it is written.
            Queue queue;
            const std::uint64_t a = queue.Add(L"A", 10); // the edited clip A
            queue.Begin(0);
            const std::uint64_t b = queue.Add(L"B", 20); // a new recording meanwhile
            Check(queue.Finish(a, false, "rename failed", 100) == savequeue::Outcome::Held, "d01: A failed, held");
            Queue::Job* next = queue.Begin(100);
            Check(next && next->id == b, "d01: the other saves go on");
            queue.Finish(b, true, "", 200);
            Check(queue.Find(L"A") && queue.Find(L"A")->payload == 10, "d01: a load of A reads the held edits");
            // A is edited again: the new snapshot replaces the held one.
            const std::uint64_t a2 = queue.Add(L"A", 11);
            Check(queue.Size() == 1 && queue.HeldCount() == 0 && queue.Find(L"A")->payload == 11, "d01: a newer save of A replaces it");
            queue.Begin(200);
            Check(queue.Finish(a2, true, "", 300) == savequeue::Outcome::Saved && queue.Empty(), "d01: then A is written");
        }
        {
            // A save of a file being written waits behind it; if the first
            // fails, the newer one holds everything and the old one goes.
            Queue queue;
            const std::uint64_t first = queue.Add(L"A", 1);
            queue.Begin(0);
            queue.Add(L"A", 2);
            Check(queue.Size() == 2 && queue.Begin(0) == nullptr, "queue: the newer save waits for the write in progress");
            Check(queue.Finish(first, false, "x", 10) == savequeue::Outcome::Superseded && queue.Size() == 1 &&
                    queue.Find(L"A")->payload == 2 && queue.HeldCount() == 0,
                "queue: a failed write replaced by a newer one is not retried");
        }
        {
            // Parts in order, a failed part kept while the next goes.
            Queue queue;
            const std::uint64_t p1 = queue.Add(L"clip-p1", 1);
            const std::uint64_t p2 = queue.Add(L"clip-p2", 2);
            Check(queue.Begin(0)->id == p1, "d02: parts are written in the order asked");
            queue.Finish(p1, false, "disk full", 0);
            Check(queue.Begin(0)->id == p2, "d02: the next part is written while the failed one waits");
            queue.Finish(p2, false, "disk full", 0);
            Check(queue.HeldCount() == 2 && queue.Find(L"clip-p1") && queue.Find(L"clip-p2"), "d02: no part is lost");
        }
        {
            // A file with a job is deleted or renamed.
            Queue queue;
            const std::uint64_t id = queue.Add(L"A", 1);
            queue.Add(L"B", 2);
            queue.Begin(0);
            Check(queue.Drop(L"A") == 1 && queue.Finish(id, true, "", 0) == savequeue::Outcome::Dropped && !queue.Find(L"A"),
                "queue: a file deleted while written drops its job");
            queue.Rename(L"B", L"C");
            Check(!queue.Find(L"B") && queue.Find(L"C") && queue.Find(L"C")->payload == 2, "queue: a rename moves the job");
            Check(queue.Drop(L"C") == 1 && queue.Empty(), "queue: a deleted file's waiting job goes");
            Check(queue.Finish(999, true, "", 0) == savequeue::Outcome::Unknown, "queue: an unknown id changes nothing");
        }
    }
}

namespace
{
    // REPLAYPOC_CLIP_DIR=<folder> loads every clip there through the load
    // checks and fails on a clip that only those refuse (old clips must keep
    // loading). Skipped when unset.
    void TestRealClipsFromEnv()
    {
        wchar_t folder[MAX_PATH]{};
        if (!GetEnvironmentVariableW(L"REPLAYPOC_CLIP_DIR", folder, MAX_PATH))
            return;
        int loaded = 0, refused = 0, refusedByNewChecks = 0;
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ignored))
        {
            if (entry.path().extension() != L".rpclip")
                continue;
            Clip clip;
            std::uint64_t duration = 0;
            std::string error;
            if (clipfile::Load(entry.path().wstring(), kBase, 0x8000000, clip.recording, clip.poses, clip.camera, duration, error))
            {
                ++loaded;
                continue;
            }
            ++refused;
            const bool mine = error.find("'entity sounds'") != std::string::npos || error.find("unusable value") != std::string::npos ||
                error.find("share an id") != std::string::npos || error.find("past the next id") != std::string::npos ||
                error.find("timestamps") != std::string::npos || error.find("too large") != std::string::npos ||
                error.find("not enough memory") != std::string::npos;
            refusedByNewChecks += mine ? 1 : 0;
            std::fprintf(stderr, "  real clip %ls: %s%s\n", entry.path().filename().c_str(), error.c_str(), mine ? " [new check]" : "");
        }
        std::printf("  real clips: %d loaded, %d refused (%d by the new checks)\n", loaded, refused, refusedByNewChecks);
        Check(refusedByNewChecks == 0, "real clips: none refused by the new checks");
    }
}

namespace
{
    // The train cars (TRNC, CREA kind train_car, ESND version 4 vehicleAudio):
    // round trip, an older clip without the section, and the values the reader
    // refuses.
    void TestTrainsSection()
    {
        Clip clip;
        MakeClip(clip);
        Recording& rec = clip.recording;
        const ReplayId head = rec.ids.Allocate();
        const ReplayId back = rec.ids.Allocate();
        for (const ReplayId id : {head, back})
        {
            entity::CreatePacket car;
            car.id = id;
            car.timeMs = 40;
            car.kind = entity::EntityKind::TrainCar;
            car.modelHash = id == head ? 0x0A3A9C4Bu : 0x1B7A1A1Au;
            rec.created.push_back(car);
            WeaponTrack track;
            track.recordId = id.Packed();
            entity::TransformSample sample;
            sample.timeMs = 40;
            sample.relative.position = {100.0f, 200.0f, 50.0f};
            sample.relative.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
            track.track.Append(sample);
            rec.weaponTracks.push_back(track);
        }
        trains::TrainCar front;
        front.id = head.Packed();
        front.flags = trains::kCarHead | trains::kCarMission | trains::kCarLinksRead;
        front.vehicleType = 17;
        front.track = 3;
        front.propSet = 0x12345678u;
        rec.trains.cars.push_back(front);
        trains::TrainCar rear;
        rear.id = back.Packed();
        rear.flags = trains::kCarTail;
        rear.vehicleType = 18;
        rear.lightPropSet = 0x9ABCDEF0u;
        rec.trains.cars.push_back(rear);
        trains::LinkConsist({head.Packed(), back.Packed()}, rec.trains.cars);
        trains::SpeedTrack speed;
        speed.head = head.Packed();
        speed.Append(40, 0.0f, true);
        speed.Append(400, 3.5f, true);
        speed.Append(900, 7.0f, false);
        rec.trains.speeds.push_back(speed);
        EntitySound whistle;
        whistle.entity = head;
        whistle.startMs = 300;
        whistle.endMs = 1300;
        whistle.ended = true;
        whistle.soundRef = 0x80000400438196ull;
        whistle.audioOnEntity = true;
        whistle.audioOffset = 0x40u;
        whistle.vehicleAudio = true;
        rec.entitySounds.push_back(whistle);

        const std::vector<std::uint8_t> bytes = EncodeClip(clip);
        Clip loaded;
        std::uint64_t duration = 0;
        std::string error;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok, "trains: a clip with train cars loads");
        if (ok)
        {
            const Recording& r = loaded.recording;
            const trains::TrainCar* a = r.trains.Find(head.Packed());
            const trains::TrainCar* b = r.trains.Find(back.Packed());
            Check(a && b && a->head == head.Packed() && b->head == head.Packed() && a->backward == back.Packed() &&
                    b->forward == head.Packed() && a->forward == 0 && b->backward == 0 && a->index == 0 && b->index == 1 &&
                    a->cars == 2 && (a->flags & trains::kCarHead) && (b->flags & trains::kCarTail) &&
                    (a->flags & trains::kCarMission) && a->vehicleType == 17 && a->track == 3 && a->propSet == 0x12345678u &&
                    b->lightPropSet == 0x9ABCDEF0u,
                "trains: cars round trip");
            const trains::SpeedTrack* s = r.trains.SpeedOf(head.Packed());
            Check(s && s->samples.size() == 3 && s->samples[1].speed == 3.5f && s->samples[2].forward == 0,
                "trains: speed round trip");
            std::size_t cars = 0;
            for (const entity::CreatePacket& packet : r.created)
                cars += packet.kind == entity::EntityKind::TrainCar ? 1 : 0;
            Check(cars == 2, "trains: train car creates round trip");
            bool vehicleAudio = false;
            for (const EntitySound& sound : r.entitySounds)
                vehicleAudio = vehicleAudio || (sound.vehicleAudio && sound.audioOffset == 0x40u && sound.entity == head);
            Check(vehicleAudio, "trains: a train car sound (ESND 4) round trip");
        }
        const std::vector<std::uint8_t> old = WithoutSection(bytes, "trains");
        Check(!old.empty(), "trains: clip without the section built");
        if (!old.empty())
        {
            Clip older;
            const bool oldOk = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, error);
            Check(oldOk && older.recording.trains.Empty(), "trains: an older clip without the section loads with no train data");
        }
        // Refused values.
        const auto refused = [&](void (*change)(Recording&), const char* what) {
            Clip bad;
            MakeClip(bad);
            bad.recording.trains = rec.trains;
            change(bad.recording);
            Check(DecodeSkips(EncodeClip(bad), "trains", error), what); // skipped, the clip opens
        };
        refused([](Recording& r) { r.trains.cars[0].id = 0; }, "trains: a car without id is refused");
        refused([](Recording& r) { r.trains.cars[0].cars = 1000; }, "trains: a consist past the walk cap is refused");
        refused([](Recording& r) { r.trains.speeds[0].samples[2].timeMs = 10; }, "trains: speed samples out of order are refused");
        refused([](Recording& r) { r.trains.speeds[0].samples[0].speed = std::numeric_limits<float>::quiet_NaN(); },
            "trains: a speed that is not a number is refused");
        {
            EntitySound sound = whistle;
            Check(EntitySoundAudioValid(sound, kImageSize), "esnd: a train car audio offset is valid");
            sound.audioOffset = kEntitySoundVehicleAudioSpan;
            Check(!EntitySoundAudioValid(sound, kImageSize), "esnd: a train car audio offset past the span is refused");
            sound.audioOffset = 0x40u;
            sound.pedAudio = true;
            Check(!EntitySoundAudioValid(sound, kImageSize), "esnd: ped and train audio at once is refused");
            sound.pedAudio = false;
            sound.audioOnEntity = false;
            Check(!EntitySoundAudioValid(sound, kImageSize), "esnd: a train car audio off the entity is refused");
        }
    }
}

namespace
{
    // The sound census (SCEN) and the census-level captured sounds (GSND):
    // round trip, a clip without them, refused values.
    void TestSoundCheckSections()
    {
        Clip clip;
        MakeClip(clip);
        Recording& rec = clip.recording;
        census::SummaryKey whistle;
        whistle.owner = (3u << 16) | 912u;
        whistle.cls = census::OwnerClass::Train;
        whistle.ref = 0xfffff01170670ull;
        whistle.path[0] = 0x251688D;
        whistle.path[1] = 0x2589214;
        census::SummaryKey wind;
        wind.ref = 0x1e4a8;
        rec.soundCensus.keys = {whistle, wind};
        rec.soundCensus.cells = {{4, 0, 2, 0}, {4, 1, 7, 1}, {9, 0, 1, 0}};
        rec.soundCensus.covered = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
        census::GeneralSound loop;
        loop.entity = whistle.owner;
        loop.cls = census::OwnerClass::Train;
        loop.startMs = 0;
        loop.endMs = 2400;
        loop.ended = true;
        loop.preexisting = true;
        loop.ref = 0x8000030011bf0cdull;
        loop.base = census::AudioBase::VehicleAudio;
        loop.offset = 0x40;
        loop.vtableRva = 0x37F0000;
        loop.path[1] = 0x2589214;
        loop.params.assign(census::kParamsBytes, 0);
        rec.generalSounds.push_back(loop);
        census::GeneralSound field = loop;
        field.oneShot = true;
        field.preexisting = false;
        field.base = census::AudioBase::Field;
        field.field = 0x2D8;
        field.offset = 0x18;
        field.params.clear();
        rec.generalSounds.push_back(field);

        const std::vector<std::uint8_t> bytes = EncodeClip(clip);
        Clip loaded;
        std::uint64_t duration = 0;
        std::string error;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok, "sound check: a clip with a census and captured sounds loads");
        if (ok)
        {
            const Recording& r = loaded.recording;
            Check(r.soundCensus.keys.size() == 2 && r.soundCensus.keys[0] == whistle && r.soundCensus.keys[1] == wind &&
                    r.soundCensus.cells.size() == 3 && r.soundCensus.cells[1].count == 7 && r.soundCensus.cells[1].refused == 1 &&
                    r.soundCensus.covered.size() == 10,
                "sound check: census round trip");
            Check(r.generalSounds.size() == 2 && r.generalSounds[0].base == census::AudioBase::VehicleAudio &&
                    r.generalSounds[0].preexisting && r.generalSounds[0].params.size() == census::kParamsBytes &&
                    r.generalSounds[1].base == census::AudioBase::Field && r.generalSounds[1].field == 0x2D8 &&
                    r.generalSounds[1].oneShot && r.generalSounds[1].params.empty(),
                "sound check: captured sounds round trip");
        }
        std::vector<std::uint8_t> old = WithoutSection(bytes, "sound census");
        if (!old.empty())
            old = WithoutSection(old, "general sounds");
        Check(!old.empty(), "sound check: clip without the sections built");
        if (!old.empty())
        {
            Clip older;
            const bool oldOk = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, error);
            Check(oldOk && older.recording.soundCensus.Empty() && older.recording.generalSounds.empty(),
                "sound check: an older clip without the section loads with no census");
        }
        const auto refused = [&](void (*change)(Recording&), const char* section, const char* what) {
            Clip bad;
            MakeClip(bad);
            bad.recording.soundCensus = rec.soundCensus;
            bad.recording.generalSounds = rec.generalSounds;
            change(bad.recording);
            Check(DecodeSkips(EncodeClip(bad), section, error), what); // skipped, the clip opens
        };
        refused([](Recording& r) { r.soundCensus.cells[0].key = 5; }, "sound census", "sound check: a cell of no key is refused");
        refused([](Recording& r) { r.soundCensus.cells[2].window = 1; }, "sound census", "sound check: cells out of order are refused");
        refused([](Recording& r) { r.soundCensus.keys[0].cls = static_cast<census::OwnerClass>(40); }, "sound census",
            "sound check: an unknown owner class is refused");
        refused([](Recording& r) { r.generalSounds[0].offset = 0x7FFF0; }, "general sounds",
            "sound check: an audio offset past its base is refused");
        refused([](Recording& r) { r.generalSounds[1].field = 0x5000; }, "general sounds",
            "sound check: a field past the scanned bytes is refused");
        refused([](Recording& r) { r.generalSounds[0].vtableRva = 0; }, "general sounds",
            "sound check: a sound without its audio entity vtable is refused");
        refused([](Recording& r) { r.generalSounds[0].base = static_cast<census::AudioBase>(9); }, "general sounds",
            "sound check: an unknown audio base is refused");
    }

    // The sky track (SKYT) is optional: round trip of the periods and the
    // samples, a clip from before it loads with no track (its clouds are left
    // to the session), samples out of order and a value that is not a number
    // are refused.
    void TestSkyTrackSection()
    {
        Clip clip;
        MakeClip(clip);
        Environment& e = clip.recording.environment;
        e.skyPeriods.value[sky::kPeriodNoise] = 4096.0f;
        e.skyPeriods.value[sky::kPeriodPosition] = 3000.0f;
        e.skyPeriods.value[sky::kPeriodLayer] = 1.0f;
        e.skyPeriods.value[sky::kPeriodDrift] = 1.0f;
        e.skyPeriods.value[sky::kPeriodRegion] = 7680.0f;
        for (std::uint32_t n = 0; n < 3; ++n)
        {
            SkySample s;
            s.timeMs = n * 100u;
            for (std::uint32_t c = 0; c < sky::kChannels; ++c)
                s.value[c] = -0.01f * static_cast<float>(c + 1) * static_cast<float>(n + 1);
            s.value[sky::kNoiseX] = 2309.065f - static_cast<float>(n);
            s.unpinned[2] = 5.5f;
            e.sky.push_back(s);
        }
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "sky: encode");
        Clip loaded;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok, "sky: a clip with the sky track loads");
        if (ok)
        {
            const Environment& b = loaded.recording.environment;
            Check(b.sky.size() == 3 && b.sky[2].timeMs == 200 && b.sky[1].value[sky::kNoiseX] == 2308.065f &&
                    b.sky[2].value[sky::kRegionY] == e.sky[2].value[sky::kRegionY] && b.sky[0].unpinned[2] == 5.5f &&
                    b.skyPeriods.value[sky::kPeriodRegion] == 7680.0f && b.skyPeriods.value[sky::kPeriodNoise] == 4096.0f,
                "sky: track and periods round trip");
        }
        const std::vector<std::uint8_t> old = WithoutSection(bytes, "sky track");
        Check(!old.empty(), "sky: clip without the section built");
        if (!old.empty())
        {
            Clip older;
            const bool oldOk = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, error);
            Check(oldOk && older.recording.environment.sky.empty() && older.recording.environment.track.size() == 2,
                "sky: an older clip without the section loads with no sky track and its environment track");
        }
        const auto refused = [&](std::uint32_t sample, std::size_t fieldOffset, const void* value, const char* what) {
            std::vector<std::uint8_t> bad = bytes;
            const std::size_t at = FindSection(bad, "sky track");
            if (at >= bad.size())
            {
                Check(false, what);
                return;
            }
            std::uint64_t length = 0;
            std::memcpy(&length, bad.data() + at + 6, 8);
            std::uint8_t* payload = bad.data() + at + 14;
            // Periods (5 floats), count (u64), then samples of 4 + 17 * 4 bytes.
            const std::size_t sampleAt = 5 * 4 + 8 + sample * (4 + (sky::kChannels + 3) * 4);
            std::memcpy(payload + sampleAt + fieldOffset, value, 4);
            const std::uint64_t hash = TestFnv64(payload, static_cast<std::size_t>(length));
            std::memcpy(payload + length, &hash, sizeof(hash));
            std::string badError;
            Check(DecodeSkips(bad, "sky track", badError), what);
        };
        const std::uint32_t early = 50u;
        refused(2, 0, &early, "sky: samples out of order refused");
        const float notANumber = std::numeric_limits<float>::quiet_NaN();
        refused(1, 4, &notANumber, "sky: a value that is not a number refused");
    }

    // The timecycle track (TCMD) is optional: a clip from before it loads with
    // no track (the session's modifier and variations are left alone), samples
    // out of order, a strength that is not a number and a variation count past
    // the cap are refused.
    void TestTimecycleSection()
    {
        Clip clip;
        MakeClip(clip);
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "timecycle: encode");
        std::uint64_t duration = 0;
        const std::vector<std::uint8_t> old = WithoutSection(bytes, "timecycle");
        Check(!old.empty(), "timecycle: clip without the section built");
        if (!old.empty())
        {
            Clip older;
            const bool oldOk = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, error);
            Check(oldOk && older.recording.environment.timecycle.empty() && older.recording.environment.track.size() == 2,
                "timecycle: a clip from before task 43 loads with no timecycle track and its environment track");
        }
        const auto refused = [&](std::size_t fieldOffset, const void* value, std::size_t size, const char* what) {
            std::vector<std::uint8_t> bad = bytes;
            const std::size_t at = FindSection(bad, "timecycle");
            if (at >= bad.size())
            {
                Check(false, what);
                return;
            }
            std::uint64_t length = 0;
            std::memcpy(&length, bad.data() + at + 6, 8);
            std::uint8_t* payload = bad.data() + at + 14;
            // Count (u64), then the first sample (34 bytes, no variation), then the second.
            std::memcpy(payload + 8 + fieldOffset, value, size);
            const std::uint64_t hash = TestFnv64(payload, static_cast<std::size_t>(length));
            std::memcpy(payload + length, &hash, sizeof(hash));
            std::string badError;
            Check(DecodeSkips(bad, "timecycle", badError), what);
        };
        const std::uint32_t late = 9000u;
        refused(0, &late, 4, "timecycle: samples out of order refused");
        const float notANumber = std::numeric_limits<float>::quiet_NaN();
        refused(12, &notANumber, 4, "timecycle: a strength that is not a number refused");
        const std::uint8_t tooMany = static_cast<std::uint8_t>(tctrack::kMaxVariations + 1);
        refused(34 + 33, &tooMany, 1, "timecycle: a variation count past the cap refused");
    }

    // The moon track (MOON) is optional: a clip from before it loads with no
    // track (the session's moon is kept), samples out of order, an angle that
    // is not a number or far outside a turn, and a day past 31 are refused (the
    // section is left out).
    void TestMoonSection()
    {
        Clip clip;
        MakeClip(clip);
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "moon: encode");
        std::uint64_t duration = 0;
        const std::vector<std::uint8_t> old = WithoutSection(bytes, "moon");
        Check(!old.empty(), "moon: clip without the section built");
        if (!old.empty())
        {
            Clip older;
            const bool oldOk = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, error);
            Check(oldOk && older.recording.environment.moon.empty() && older.recording.environment.timecycle.size() == 2 &&
                      older.recording.environment.track.size() == 2,
                "moon: a clip from before the section loads with no moon track and its other environment tracks");
        }
        const auto refused = [&](std::size_t fieldOffset, const void* value, std::size_t size, const char* what) {
            std::vector<std::uint8_t> bad = bytes;
            const std::size_t at = FindSection(bad, "moon");
            if (at >= bad.size())
            {
                Check(false, what);
                return;
            }
            std::uint64_t length = 0;
            std::memcpy(&length, bad.data() + at + 6, 8);
            std::uint8_t* payload = bad.data() + at + 14;
            // Count (u64), then samples of 48 bytes: time, angle, orbit, sun, moon, year, month, day.
            std::memcpy(payload + 8 + fieldOffset, value, size);
            const std::uint64_t hash = TestFnv64(payload, static_cast<std::size_t>(length));
            std::memcpy(payload + length, &hash, sizeof(hash));
            std::string badError;
            Check(DecodeSkips(bad, "moon", badError), what);
        };
        const std::uint32_t late = 9000u;
        refused(0, &late, 4, "moon: samples out of order refused");
        const double notANumber = std::numeric_limits<double>::quiet_NaN();
        refused(4, &notANumber, 8, "moon: an angle that is not a number refused");
        const double farOrbit = 1000.0;
        refused(48 + 12, &farOrbit, 8, "moon: an orbit far outside a turn refused");
        const std::uint8_t day = 40;
        refused(47, &day, 1, "moon: a day past 31 refused");
    }

    // The generic sound packets (SGEN) are optional: round trip of creates,
    // adjusts and ends, the bytes per packet are the ones the budget counts, a
    // clip from before loads with an empty track, a packet naming a sound past
    // the next index is refused.
    void TestSoundGenericSection()
    {
        Clip clip;
        MakeClip(clip);
        soundgeneric::Track& track = clip.recording.soundGeneric;
        track.nextIndex = 2;
        for (std::uint32_t n = 0; n < 2; ++n)
        {
            soundgeneric::CreatePacket c;
            c.timeMs = 100 + n * 50;
            c.index = n;
            c.stableId = 0x40029934aull + n;
            c.idKind = n ? soundgeneric::IdKind::Hash : soundgeneric::IdKind::RefSet;
            c.rawRef = 0x008000040029934aull + n;
            c.owner = n ? census::kNoOwner : 0x00030002u;
            c.ownerClass = n ? census::OwnerClass::World : census::OwnerClass::Horse;
            c.source = n ? soundgeneric::Source::Direct : soundgeneric::Source::Queue;
            c.core = static_cast<std::uint8_t>(n);
            c.hasPosition = n == 1;
            c.hasParams = true;
            c.preexisting = n == 1;
            c.createDelayMs = static_cast<std::uint16_t>(35 + n);
            c.params.position[0] = 100.5f * static_cast<float>(n);
            c.params.position[2] = -3.0f;
            c.params.volume = -6.0f;
            c.params.volume2 = 1.5f;
            c.params.pitch = -200;
            c.params.pan = -1;
            c.params.flags = 0x20;
            c.params.variableCount = 1;
            c.params.hasTracker = n == 0;
            c.params.variables[0].hash = 0x644dcf02u;
            c.params.variables[0].value = 0.75f;
            c.caller = 0x2EFA7E0;
            c.path[0] = 0x2588996;
            c.path[1] = 0x2EFA7E0;
            // SGEN v2: the audio entity's base from the owner (the first one known).
            c.audioKnown = n == 0;
            c.audioBase = n ? census::AudioBase::Image : census::AudioBase::Field;
            c.audioField = n ? 0 : 0x900;
            c.audioOffset = n ? 0 : 0x48C0;
            c.vtableRva = n ? 0 : 0x3839538u;
            track.creates.push_back(c);
        }
        soundgeneric::AdjustPacket position;
        position.timeMs = 120;
        position.index = 0;
        position.kind = soundgeneric::Adjust::Position;
        position.value[0] = 1.0f;
        position.value[1] = 2.0f;
        position.value[2] = 3.0f;
        track.adjusts.push_back(position);
        soundgeneric::AdjustPacket volume;
        volume.timeMs = 130;
        volume.index = 1;
        volume.kind = soundgeneric::Adjust::Volume;
        volume.value[0] = -12.5f;
        track.adjusts.push_back(volume);
        soundgeneric::AdjustPacket variable;
        variable.timeMs = 140;
        variable.index = 0;
        variable.kind = soundgeneric::Adjust::Variable;
        variable.hash = 0x833ff9c9u;
        variable.value[0] = 0.25f;
        track.adjusts.push_back(variable);
        soundgeneric::EndPacket stop;
        stop.timeMs = 500;
        stop.index = 0;
        stop.reason = soundgeneric::EndReason::StopAndForget;
        track.ends.push_back(stop);
        soundgeneric::EndPacket release;
        release.timeMs = 900;
        release.index = 0;
        release.reason = soundgeneric::EndReason::Release;
        track.ends.push_back(release);
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "sound generic: encode");
        const std::size_t at = FindSection(bytes, "sound generic");
        Check(at < bytes.size(), "sound generic: section found");
        if (at < bytes.size())
        {
            std::uint64_t length = 0;
            std::memcpy(&length, bytes.data() + at + 6, 8);
            // nextIndex, three counts, then the packets' bytes (the budget counts the same).
            Check(length == 4 + 8 * 3 + track.FileBytes() && track.FileBytes() == 2 * 115 + 21 + 13 + 17 + 2 * 9, // SGEN v4: 115 bytes (no init variable, no value field)
                "sound generic: the section's bytes are the packets' bytes");
        }
        Clip loaded;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok, "sound generic: a clip with the track loads");
        if (ok)
        {
            const soundgeneric::Track& b = loaded.recording.soundGeneric;
            Check(b.nextIndex == 2 && b.creates.size() == 2 && b.adjusts.size() == 3 && b.ends.size() == 2, "sound generic: the counts");
            if (b.creates.size() == 2)
            {
                const soundgeneric::CreatePacket& c0 = b.creates[0];
                const soundgeneric::CreatePacket& c1 = b.creates[1];
                Check(c0.timeMs == 100 && c0.index == 0 && c0.stableId == 0x40029934aull && c0.idKind == soundgeneric::IdKind::RefSet &&
                        c0.rawRef == 0x008000040029934aull && c0.owner == 0x00030002u && c0.ownerClass == census::OwnerClass::Horse &&
                        c0.source == soundgeneric::Source::Queue && c0.core == 0 && !c0.hasPosition && c0.hasParams && !c0.preexisting &&
                        c0.createDelayMs == 35 && c0.params.hasTracker && c0.params.position[2] == -3.0f && c0.params.volume == -6.0f &&
                        c0.params.volume2 == 1.5f && c0.params.pitch == -200 && c0.params.pan == -1 && c0.params.flags == 0x20 &&
                        c0.params.variableCount == 1 && c0.params.variables[0].hash == 0x644dcf02u && c0.params.variables[0].value == 0.75f &&
                        c0.caller == 0x2EFA7E0 && c0.path[0] == 0x2588996 && c0.path[1] == 0x2EFA7E0 && c0.path[2] == 0,
                    "sound generic: the first create round trips");
                Check(c0.audioKnown && c0.audioBase == census::AudioBase::Field && c0.audioField == 0x900 && c0.audioOffset == 0x48C0 &&
                        c0.vtableRva == 0x3839538u && !c1.audioKnown && c1.vtableRva == 0 && c1.audioBase == census::AudioBase::Image,
                    "sound generic: the audio base of the create round trips (SGEN v2)");
                Check(c1.idKind == soundgeneric::IdKind::Hash && c1.owner == census::kNoOwner && c1.source == soundgeneric::Source::Direct &&
                        c1.core == 1 && c1.hasPosition && c1.preexisting && !c1.params.hasTracker && c1.params.position[0] == 100.5f,
                    "sound generic: the second create round trips");
            }
            if (b.adjusts.size() == 3)
            {
                Check(b.adjusts[0].kind == soundgeneric::Adjust::Position && b.adjusts[0].value[1] == 2.0f && b.adjusts[0].value[2] == 3.0f &&
                        b.adjusts[0].timeMs == 120 && b.adjusts[1].kind == soundgeneric::Adjust::Volume && b.adjusts[1].value[0] == -12.5f &&
                        b.adjusts[1].index == 1 && b.adjusts[2].kind == soundgeneric::Adjust::Variable && b.adjusts[2].hash == 0x833ff9c9u &&
                        b.adjusts[2].value[0] == 0.25f,
                    "sound generic: the adjusts round trip");
            }
            if (b.ends.size() == 2)
                Check(b.ends[0].reason == soundgeneric::EndReason::StopAndForget && b.ends[0].timeMs == 500 &&
                        b.ends[1].reason == soundgeneric::EndReason::Release && b.ends[1].timeMs == 900 && b.ends[1].index == 0,
                    "sound generic: the ends round trip");
        }
        const std::vector<std::uint8_t> old = WithoutSection(bytes, "sound generic");
        Check(!old.empty(), "sound generic: clip without the section built");
        if (!old.empty())
        {
            Clip older;
            const bool oldOk = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, error);
            Check(oldOk && older.recording.soundGeneric.Empty() && older.recording.soundGeneric.nextIndex == 0,
                "sound generic: an older clip without the section loads with an empty track");
        }
        // A refused payload leaves the section out and the clip opens
        // (DecodeSkips).
        const auto refused = [&](std::size_t offset, const void* value, std::size_t size, const char* what) {
            std::vector<std::uint8_t> bad = bytes;
            const std::size_t section = FindSection(bad, "sound generic");
            if (section >= bad.size())
            {
                Check(false, what);
                return;
            }
            std::uint64_t length = 0;
            std::memcpy(&length, bad.data() + section + 6, 8);
            std::uint8_t* payload = bad.data() + section + 14;
            std::memcpy(payload + offset, value, size);
            const std::uint64_t hash = TestFnv64(payload, static_cast<std::size_t>(length));
            std::memcpy(payload + length, &hash, sizeof(hash));
            std::string badError;
            Check(DecodeSkips(bad, "sound generic", badError), what);
        };
        // The first end's index: nextIndex (4), creates count (8) + 2 * 115 (SGEN v4), adjusts count (8) + 21 + 13 + 17, ends count (8), timeMs (4).
        const std::uint32_t pastIndex = 7;
        refused(4 + 8 + 2 * 115 + 8 + 51 + 8 + 4, &pastIndex, 4, "sound generic: an end naming a sound past the next index is refused");
        const std::uint32_t early = 50;
        refused(4 + 8 + 115, &early, 4, "sound generic: creates out of order are refused");
        const float notANumber = std::numeric_limits<float>::quiet_NaN();
        refused(4 + 8 + 2 * 115 + 8 + 9 + 4, &notANumber, 4, "sound generic: a position that is not a number is refused");
        const std::uint8_t badKind = 9;
        refused(4 + 8 + 2 * 115 + 8 + 8, &badKind, 1, "sound generic: an adjust of an unknown kind is refused");
        // Finding 2 of the release review: nextIndex sizes tables (the player's
        // asked 16 GiB for UINT32_MAX next to one create): past
        // kMaxNextIndex the section is left out.
        const std::uint32_t hugeNext = 0xFFFFFFFFu;
        refused(0, &hugeNext, 4, "sound generic: a nextIndex of UINT32_MAX is refused (no 16 GiB table)");
        const std::uint32_t pastBound = soundgeneric::kMaxNextIndex + 1;
        refused(0, &pastBound, 4, "sound generic: a nextIndex past kMaxNextIndex is refused");

        // A recorded clip had 8 of 10532 creates with variableCount 0 and
        // 0xFFFFFFFF in a slot the count does not name. The slot is zeroed,
        // the section stays whole.
        const auto withPayloadChange = [&](std::size_t offset, const void* value, std::size_t size) {
            std::vector<std::uint8_t> changed = bytes;
            const std::size_t section = FindSection(changed, "sound generic");
            if (section >= changed.size())
                return std::vector<std::uint8_t>();
            std::uint64_t length = 0;
            std::memcpy(&length, changed.data() + section + 6, 8);
            std::uint8_t* payload = changed.data() + section + 14;
            std::memcpy(payload + offset, value, size);
            const std::uint64_t hash = TestFnv64(payload, static_cast<std::size_t>(length));
            std::memcpy(payload + length, &hash, sizeof(hash));
            return changed;
        };
        const std::uint32_t garbage = 0xFFFFFFFFu;
        // The first create has variableCount 1: its second slot's value is at
        // nextIndex (4), count (8), 62 bytes to the variables, one slot (8), the hash (4).
        {
            const std::vector<std::uint8_t> clip036 = withPayloadChange(4 + 8 + 62 + 8 + 4, &garbage, 4);
            Clip whole;
            std::vector<clipfile::SkippedSection> skipped;
            std::string note;
            const bool wholeOk = !clip036.empty() && clipfile::Decode(clip036.data(), clip036.size(), kBase, kImageSize,
                whole.recording, whole.poses, whole.camera, duration, note, &skipped);
            const soundgeneric::Track& w = whole.recording.soundGeneric;
            Check(wholeOk && skipped.empty() && w.creates.size() == 2 && w.adjusts.size() == 3 && w.ends.size() == 2,
                "sound generic: garbage in a variable slot past the count opens with the section whole (recorded clip 036)");
            if (wholeOk && w.creates.size() == 2)
                Check(w.creates[0].params.variableCount == 1 && w.creates[0].params.variables[0].value == 0.75f &&
                        w.creates[0].params.variables[1].hash == 0 && w.creates[0].params.variables[1].value == 0.0f &&
                        w.creates[0].params.variables[2].value == 0.0f,
                    "sound generic: the slots past the count read as zeros");
        }
        // The slot the count names holds a raw dword of params +0x10C
        // (0xFFFD933F, a NaN as a float): the section stays whole and the
        // dword comes back bit for bit.
        {
            const std::uint32_t rawDword = 0xFFFD933Fu;
            const std::vector<std::uint8_t> clip1054 = withPayloadChange(4 + 8 + 62 + 4, &rawDword, 4);
            Clip whole;
            std::vector<clipfile::SkippedSection> skipped;
            std::string note;
            const bool wholeOk = !clip1054.empty() && clipfile::Decode(clip1054.data(), clip1054.size(), kBase,
                kImageSize, whole.recording, whole.poses, whole.camera, duration, note, &skipped);
            const soundgeneric::Track& w = whole.recording.soundGeneric;
            std::uint32_t back = 0;
            if (wholeOk && !w.creates.empty())
                std::memcpy(&back, &w.creates[0].params.variables[0].value, 4);
            Check(wholeOk && skipped.empty() && w.creates.size() == 2 && back == rawDword,
                "sound generic: a raw dword that is a NaN as a float in a named slot keeps the section (clip 1054)");
        }

        // A trimmed recording keeps the session's indices: a few packets
        // with a large nextIndex (76726 for 1067 creates in QA clip
        // 040-DownesRanch) still open whole, up to the bound itself.
        for (const std::uint32_t sparse : {76726u, soundgeneric::kMaxNextIndex})
        {
            const std::vector<std::uint8_t> sparseClip = withPayloadChange(0, &sparse, 4);
            Clip whole;
            std::vector<clipfile::SkippedSection> skipped;
            std::string note;
            const bool wholeOk = !sparseClip.empty() && clipfile::Decode(sparseClip.data(), sparseClip.size(), kBase,
                kImageSize, whole.recording, whole.poses, whole.camera, duration, note, &skipped);
            Check(wholeOk && skipped.empty() && whole.recording.soundGeneric.nextIndex == sparse &&
                    whole.recording.soundGeneric.creates.size() == 2,
                "sound generic: a sparse nextIndex of a trimmed recording opens whole");
        }

        // The optional-section rule itself: a truncated payload, a count
        // the bytes cannot hold and a count short of the bytes each leave
        // the section out; the clip opens with the track empty and the
        // other sections intact; the reason says where the reader stopped.
        const auto opensWithout = [&](const std::vector<std::uint8_t>& clipBytes, const char* expectInReason, const char* what) {
            Clip without;
            std::vector<clipfile::SkippedSection> skipped;
            std::string note;
            const bool ok2 = !clipBytes.empty() && clipfile::Decode(clipBytes.data(), clipBytes.size(), kBase, kImageSize,
                without.recording, without.poses, without.camera, duration, note, &skipped);
            const bool named = skipped.size() == 1 && skipped[0].section == "sound generic" &&
                skipped[0].reason.find(expectInReason) != std::string::npos;
            Check(ok2 && named && without.recording.soundGeneric.Empty() && without.recording.soundGeneric.nextIndex == 0 &&
                    !without.recording.entitySounds.empty() && without.recording.created.size() == clip.recording.created.size(),
                what);
            if (ok2 && !named)
                Check(false, (std::string(what) + ": skipped=" + std::to_string(skipped.size()) +
                    (skipped.empty() ? "" : " reason=" + skipped[0].reason)).c_str());
        };
        opensWithout(WithOlderSection(bytes, "sound generic", 2, 5), "malformed at payload byte",
            "sound generic: a truncated payload is skipped, the clip opens");
        const std::uint64_t tooMany = 9;
        opensWithout(withPayloadChange(4, &tooMany, 8), "malformed at payload byte 12 of",
            "sound generic: a create count the bytes cannot hold is skipped, the clip opens");
        const std::uint64_t tooFew = 1;
        opensWithout(withPayloadChange(4, &tooFew, 8), "malformed at payload byte",
            "sound generic: a create count short of the bytes is skipped, the clip opens");
        const std::uint64_t noEnds = 0;
        opensWithout(withPayloadChange(4 + 8 + 2 * 115 + 8 + 51, &noEnds, 8), "bytes left over after the payload",
            "sound generic: bytes past the last count are skipped, the clip opens");
        // Through Load: LoadInfo carries the sections left out.
        {
            const std::vector<std::uint8_t> truncated = WithOlderSection(bytes, "sound generic", 2, 5);
            const std::wstring path = TempPath(L"replaypoc_clip_sgen_truncated.rpclip");
            {
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                file.write(reinterpret_cast<const char*>(truncated.data()), static_cast<std::streamsize>(truncated.size()));
            }
            Clip fromFile;
            clipfile::LoadInfo info;
            std::string note;
            const bool loadOk = clipfile::Load(path, kBase, kImageSize, fromFile.recording, fromFile.poses, fromFile.camera,
                duration, note, &info);
            Check(loadOk && info.skipped.size() == 1 && info.skipped[0].section == "sound generic" &&
                    fromFile.recording.soundGeneric.Empty(),
                "sound generic: Load reports the section left out in LoadInfo");
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
        // A required section malformed still refuses the clip: TestDecalsV1 (decalRemoves) and TestTruncated (pose).
    }

    // SGEN v3 keeps the init variables of a create (params +0x10, count
    // +0x90): a skid grain (0x833ff9c9, the speed 0x48fe569d, 0x41f750d5)
    // round trips, a create without them costs one byte, a value that is not
    // a number leaves the section out and the clip opens.
    void TestSoundGenericInitVariables()
    {
        Clip clip;
        MakeClip(clip);
        soundgeneric::Track& track = clip.recording.soundGeneric;
        track.nextIndex = 2;
        for (std::uint32_t n = 0; n < 2; ++n)
        {
            soundgeneric::CreatePacket c;
            c.timeMs = 45541 + n * 17;
            c.index = n;
            c.stableId = 0xe002f36cbull;
            c.idKind = soundgeneric::IdKind::RefSet;
            c.rawRef = 0x0080000e002f36cbull;
            c.owner = 0x00000001u;
            c.ownerClass = census::OwnerClass::Horse;
            c.hasParams = true;
            if (n == 0)
            {
                c.params.initVariableCount = 3;
                c.params.initVariables[0] = {0x833ff9c9u, 1.0f};
                c.params.initVariables[1] = {0x48fe569du, 4.686f};
                c.params.initVariables[2] = {0x41f750d5u, 0.0f};
            }
            track.creates.push_back(c);
        }
        Check(track.FileBytes() == 115 + 24 + 115, "init variables: a create costs 115 bytes (SGEN v4) plus 8 per variable");
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "init variables: encode");
        Clip loaded;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        const soundgeneric::Track& b = loaded.recording.soundGeneric;
        Check(ok && b.creates.size() == 2, "init variables: the clip loads");
        if (ok && b.creates.size() == 2)
        {
            const soundgeneric::InitParams& p0 = b.creates[0].params;
            Check(p0.initVariableCount == 3 && p0.initVariables[0].hash == 0x833ff9c9u && p0.initVariables[0].value == 1.0f &&
                    p0.initVariables[1].hash == 0x48fe569du && p0.initVariables[1].value == 4.686f &&
                    p0.initVariables[2].hash == 0x41f750d5u && p0.initVariables[2].value == 0.0f &&
                    b.creates[1].params.initVariableCount == 0 && b.creates[1].timeMs == 45558,
                "init variables: the grain's three variables round trip, the next create has none");
        }
        // The speed's value of the first create: nextIndex (4), count (8), 113 bytes, the count byte, two pairs, the hash.
        const std::size_t section = FindSection(bytes, "sound generic");
        Check(section < bytes.size(), "init variables: section found");
        if (section < bytes.size())
        {
            std::vector<std::uint8_t> bad = bytes;
            std::uint64_t length = 0;
            std::memcpy(&length, bad.data() + section + 6, 8);
            std::uint8_t* payload = bad.data() + section + 14;
            const float notANumber = std::numeric_limits<float>::quiet_NaN();
            float speed = 0.0f;
            std::memcpy(&speed, payload + 4 + 8 + 113 + 1 + 8 + 4, 4);
            Check(speed == 4.686f, "init variables: the speed sits where the layout says");
            std::memcpy(payload + 4 + 8 + 113 + 1 + 8 + 4, &notANumber, 4);
            const std::uint64_t hash = TestFnv64(payload, static_cast<std::size_t>(length));
            std::memcpy(payload + length, &hash, sizeof(hash));
            std::string note;
            Check(DecodeSkips(bad, "sound generic", note), "init variables: a NaN value skips the section, the clip opens");
        }
    }

    // SGEN v4 keeps the value dwords and the environment group bit of each
    // create.
    void TestSoundGenericValueDwords()
    {
        Clip clip;
        MakeClip(clip);
        soundgeneric::Track& track = clip.recording.soundGeneric;
        track.nextIndex = 2;
        // The skid's body as the game passed it (a recorded SNDS row).
        std::uint8_t params[soundgeneric::kParamsBytes];
        soundgeneric::ConstructorParams(params);
        const std::uint32_t client = 0xee56493eu, tail = 0x3e8e1234u, word13c = 0x3d23d70au;
        std::memcpy(params + 0xF8, &client, 4);
        std::memcpy(params + 0x12C, &tail, 4); // 0x1234: the handle, never kept
        std::memcpy(params + 0x13C, &word13c, 4);
        params[0x13A] = 0x24;
        const std::uint64_t env = 0x1b1811ebe10ull;
        std::memcpy(params + 0xE0, &env, 8);
        for (std::uint32_t n = 0; n < 2; ++n)
        {
            soundgeneric::CreatePacket c;
            c.timeMs = 45525 + n * 16;
            c.index = n;
            c.stableId = 0x2b002f36cbull;
            c.idKind = soundgeneric::IdKind::RefSet;
            c.rawRef = 0x0080002b002f36cbull;
            c.owner = 0x00000001u;
            c.ownerClass = census::OwnerClass::Horse;
            c.hasParams = true;
            if (n == 0)
                c.params = soundgeneric::ReadInitParams(params);
            track.creates.push_back(c);
        }
        const soundgeneric::InitParams& p0 = track.creates[0].params;
        Check(p0.valuesKnown && p0.valueCount == 4 && p0.hasEnvGroup, "values: the body differs from the constructor in 4 dwords, and has a group");
        Check(track.FileBytes() == 115 + 4 * 5 + 115, "values: 5 bytes per value dword");
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "values: encode");
        Clip loaded;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        const soundgeneric::Track& b = loaded.recording.soundGeneric;
        Check(ok && b.creates.size() == 2, "values: the clip loads");
        if (ok && b.creates.size() == 2)
        {
            const soundgeneric::InitParams& l0 = b.creates[0].params;
            Check(l0.valuesKnown && l0.valueCount == 4 && l0.hasEnvGroup && soundgeneric::ValueAt(l0, 0xF8) == client &&
                    soundgeneric::ValueAt(l0, 0x12C) == 0x3e8e0000u && soundgeneric::ValueAt(l0, 0x13C) == word13c,
                "values: the client variable, the tail and the group bit round trip (the handle half is not kept)");
            Check(b.creates[1].params.valuesKnown && b.creates[1].params.valueCount == 0 && !b.creates[1].params.hasEnvGroup,
                "values: a create without values loads known and empty");
        }
    }

    // Every optional section has a reset; a payload the reader refuses (here
    // one byte short) is left out, the clip opens naming it, and what loaded
    // is, byte for byte once re-encoded, the clip without that section (so a
    // reader that wrote part of its member, or one that lives on another
    // section's records like the fx attaches, leaves nothing behind).
    void TestOptionalSectionSkip()
    {
        Clip clip;
        MakeClip(clip);
        // CFXE is written only while events wait to be folded; one waits here
        // so the section is covered.
        {
            camera::fx::ShakeEvent pending;
            pending.timeMs = 100;
            pending.kind = camera::fx::Kind::OnOff;
            pending.value = 1.0f;
            clip.camera.LegacyEffects().Add(pending);
        }
        std::vector<std::uint8_t> bytes;
        std::string error;
        clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error);
        std::size_t optionalCount = 0;
        for (const clipfile::SectionInfo& info : clipfile::Sections())
        {
            if (!info.optional)
            {
                Check(!info.skippable, (std::string("skip: required section '") + info.name + "' is not skippable").c_str());
                continue;
            }
            ++optionalCount;
            Check(info.skippable, (std::string("skip: optional section '") + info.name + "' has a reset").c_str());
            const std::vector<std::uint8_t> absent = WithoutSection(bytes, info.name);
            const std::vector<std::uint8_t> cut = WithOlderSection(bytes, info.name, info.version, 1);
            Clip a;
            Clip b;
            std::uint64_t durationA = 0;
            std::uint64_t durationB = 0;
            std::vector<clipfile::SkippedSection> skipped;
            std::string errorA;
            std::string errorB;
            const bool okA = !absent.empty() &&
                clipfile::Decode(absent.data(), absent.size(), kBase, kImageSize, a.recording, a.poses, a.camera, durationA, errorA);
            const bool okB = !cut.empty() &&
                clipfile::Decode(cut.data(), cut.size(), kBase, kImageSize, b.recording, b.poses, b.camera, durationB, errorB, &skipped);
            const bool named = skipped.size() == 1 && skipped[0].section == info.name &&
                skipped[0].reason.find("malformed at payload byte") != std::string::npos;
            Check(okA && okB && named,
                (std::string("skip: '") + info.name + "' one byte short opens the clip and is the one section left out (" +
                    (okA ? "" : "absent: " + errorA + " ") + (okB ? "" : "cut: " + errorB + " ") + "skipped " +
                    std::to_string(skipped.size()) + ")").c_str());
            if (okA && okB)
                Check(EncodeClip(a) == EncodeClip(b),
                    (std::string("skip: '") + info.name + "' left out loads as the clip without it").c_str());
        }
        Check(optionalCount >= 27, "skip: the optional sections were covered");
    }

    // The editor's environment settings (ENVO) are optional: round trip, a
    // clip from before loads all Recorded, a bad value is refused.
    void TestEnvironmentEditSection()
    {
        Clip clip;
        MakeClip(clip);
        EnvironmentOverride& edit = clip.recording.environment.edit;
        edit.clockMinutes = 18 * 60 + 45;
        edit.weather = envoverride::Joaat("THUNDERSTORM");
        edit.blendS = 10;
        edit.cloudsLive = 1;
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "envo: encode");
        Clip loaded;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok && loaded.recording.environment.edit == edit, "envo: the settings round trip");
        const std::vector<std::uint8_t> old = WithoutSection(bytes, "environment edit");
        Check(!old.empty(), "envo: clip without the section built");
        if (!old.empty())
        {
            Clip older;
            const bool oldOk = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, error);
            Check(oldOk && !older.recording.environment.edit.Any(), "envo: an older clip without the section loads all Recorded");
        }
        std::vector<std::uint8_t> bad = bytes;
        const std::size_t at = FindSection(bad, "environment edit");
        Check(at < bad.size(), "envo: section found");
        if (at < bad.size())
        {
            std::uint64_t length = 0;
            std::memcpy(&length, bad.data() + at + 6, 8);
            std::uint8_t* payload = bad.data() + at + 14;
            payload[8] = 7; // a blend not in the list
            const std::uint64_t hash = TestFnv64(payload, static_cast<std::size_t>(length));
            std::memcpy(payload + length, &hash, sizeof(hash));
            std::string badError;
            Check(DecodeSkips(bad, "environment edit", badError),
                "envo: a bad value is refused");
        }
    }
}

namespace
{
    // MIXS v3 (the trailing flags) with v2 still read (flags 0) and a newer
    // version refused; CATS left out of a clip without category samples,
    // which loads with none; a bad CATS (time going back) refused (the
    // optional section skipped). LSTN is saved as v2 only with v2 data, else
    // as v1 (older builds open the clip).
    void TestListenerV2Sections()
    {
        Clip clip;
        MakeClip(clip);
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "lstn: encode");
        const std::size_t at = FindSection(bytes, "game listener");
        std::uint16_t version = 0;
        if (at < bytes.size())
            std::memcpy(&version, bytes.data() + at + 4, sizeof(version));
        Check(version == 2, "lstn: a track with microphone data is saved as v2");
        Clip plain;
        MakeClip(plain);
        for (gamelistener::Sample& s : plain.recording.gameListener.samples)
        {
            s.micHash = 0;
            s.micSource = gamelistener::MicSource::None;
            s.stored = 0;
        }
        std::vector<std::uint8_t> plainBytes;
        Check(clipfile::Encode(plain.recording, plain.poses, plain.camera, 1000, kBase, kImageSize, plainBytes, error),
            "lstn: encode without v2");
        const std::size_t at2 = FindSection(plainBytes, "game listener");
        std::uint16_t version2 = 0;
        if (at2 < plainBytes.size())
            std::memcpy(&version2, plainBytes.data() + at2 + 4, sizeof(version2));
        Check(version2 == 1, "lstn: a track without v2 data is saved as v1");
        Clip back;
        std::uint64_t duration = 0;
        const bool ok = clipfile::Decode(plainBytes.data(), plainBytes.size(), kBase, kImageSize, back.recording, back.poses,
            back.camera, duration, error);
        Check(ok && back.recording.gameListener.samples.size() == 2 && !back.recording.gameListener.AnyV2(),
            "lstn: the v1 save reads back without microphone");
    }

    void TestMixSceneSections()
    {
        Clip clip;
        MakeClip(clip);
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "mix scenes: encode");
        const std::vector<std::uint8_t> v2 = WithOlderSection(bytes, "mix scenes", 2, 4);
        Check(!v2.empty(), "mix scenes: MIXS v2 built");
        if (!v2.empty())
        {
            Clip older;
            std::uint64_t duration = 0;
            const bool ok = clipfile::Decode(v2.data(), v2.size(), kBase, kImageSize, older.recording, older.poses, older.camera,
                duration, error);
            Check(ok && older.recording.mixScenes.instances.size() == 2 && older.recording.mixScenes.vars.size() == 2 &&
                      older.recording.mixScenes.flags == 0,
                "mix scenes: a MIXS v2 loads with flags 0 (every older clip, 1152-Colter among them)");
        }
        const std::vector<std::uint8_t> future = WithOlderSection(bytes, "mix scenes", 4, 0);
        Check(!future.empty() && DecodeFails(future, error), "mix scenes: a newer MIXS is refused");

        // MIXS is v3 only with flags; a track without them is saved as v2, so
        // a clip recorded with the new keys at 0 opens in older builds.
        {
            const std::size_t at = FindSection(bytes, "mix scenes");
            std::uint16_t version = 0;
            if (at < bytes.size())
                std::memcpy(&version, bytes.data() + at + 4, sizeof(version));
            Check(version == 3, "mix scenes: a track with flags is saved as MIXS v3");
            Clip noFlags;
            MakeClip(noFlags);
            noFlags.recording.mixScenes.flags = 0;
            std::vector<std::uint8_t> plainBytes;
            Check(clipfile::Encode(noFlags.recording, noFlags.poses, noFlags.camera, 1000, kBase, kImageSize, plainBytes, error),
                "mix scenes: encode without MIXS flags");
            const std::size_t at2 = FindSection(plainBytes, "mix scenes");
            std::uint16_t version2 = 0;
            if (at2 < plainBytes.size())
                std::memcpy(&version2, plainBytes.data() + at2 + 4, sizeof(version2));
            Check(version2 == 2, "mix scenes: a track without flags is saved as MIXS v2 (older builds open it)");
            Clip back;
            std::uint64_t backDuration = 0;
            const bool ok = clipfile::Decode(plainBytes.data(), plainBytes.size(), kBase, kImageSize, back.recording, back.poses,
                back.camera, backDuration, error);
            Check(ok && back.recording.mixScenes.instances.size() == 2 && back.recording.mixScenes.vars.size() == 2 &&
                    back.recording.mixScenes.flags == 0,
                "mix scenes: the v2 save reads back with its scenes, variables and flags 0");
        }

        Clip plain;
        MakeClip(plain);
        plain.recording.categoryState.Clear();
        std::vector<std::uint8_t> without;
        Check(clipfile::Encode(plain.recording, plain.poses, plain.camera, 1000, kBase, kImageSize, without, error),
            "mix scenes: encode without CATS");
        Check(FindSection(without, "category state") >= without.size() && FindSection(bytes, "category state") < bytes.size(),
            "mix scenes: CATS written only with samples (an older build opens the clip)");
        Clip loaded;
        std::uint64_t duration = 0;
        Check(clipfile::Decode(without.data(), without.size(), kBase, kImageSize, loaded.recording, loaded.poses, loaded.camera,
                  duration, error) &&
                loaded.recording.categoryState.Empty(),
            "mix scenes: a clip without CATS loads with none");

        // Time going back in CATS: the optional section is skipped (not loaded).
        Clip badClip;
        MakeClip(badClip);
        badClip.recording.categoryState.samples[2].timeMs = 0;
        badClip.recording.categoryState.samples[0].timeMs = 900;
        std::vector<std::uint8_t> bad;
        Check(clipfile::Encode(badClip.recording, badClip.poses, badClip.camera, 1000, kBase, kImageSize, bad, error),
            "mix scenes: encode a bad CATS");
        std::string note;
        Check(DecodeSkips(bad, "category state", note), "mix scenes: a CATS out of time order is skipped");
    }

    // WCND version 2 carries the whole weapon look per state (rust, sixth
    // wear channel, flags, the engine's component list); a version 1 section
    // still loads with flags 0 and no list; a newer version is refused; a
    // list past the cap skips the optional section and the clip opens.
    void TestWeaponLookSection()
    {
        Clip clip;
        MakeClip(clip);
        WeaponConditionTrack track;
        track.recordId = 0x00010002u;
        WeaponCondition first;
        first.dirt = 0.25f;
        first.soot = 0.5f;
        first.damage = 0.125f;
        first.degradation = 0.75f;
        first.permanent = 0.375f;
        first.wearE4 = 0.0625f;
        first.flags = weaponlook::kFlagComponents | weaponlook::kFlagWearRaw;
        first.components = {0x06ba0baau, 0x43910aecu, 0xa7f99cbfu};
        WeaponCondition second = first;
        second.soot = 0.625f;
        second.components = {0x06ba0baau, 0x43910aecu, 0xb3523ec0u};
        track.states.push_back({100, first});
        track.states.push_back({900, second});
        clip.recording.weaponConditions.push_back(track);
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(clip.recording, clip.poses, clip.camera, 1000, kBase, kImageSize, bytes, error), "look: encode");
        std::uint64_t duration = 0;
        Clip loaded;
        const bool ok = clipfile::Decode(bytes.data(), bytes.size(), kBase, kImageSize, loaded.recording, loaded.poses,
            loaded.camera, duration, error);
        Check(ok && loaded.recording.weaponConditions.size() == 1, "look: a v2 section loads");
        if (ok && loaded.recording.weaponConditions.size() == 1)
        {
            const auto& states = loaded.recording.weaponConditions[0].states;
            Check(states.size() == 2 && states[0].first == 100 && states[1].first == 900, "look: both states, their times");
            if (states.size() == 2)
            {
                const WeaponCondition& a = states[0].second;
                const WeaponCondition& b = states[1].second;
                Check(a.dirt == 0.25f && a.soot == 0.5f && a.damage == 0.125f && a.degradation == 0.75f &&
                        a.permanent == 0.375f && a.wearE4 == 0.0625f, "look: the six wear values round trip");
                Check(a.flags == (weaponlook::kFlagComponents | weaponlook::kFlagWearRaw), "look: flags round trip");
                Check(a.components == first.components && b.components == second.components, "look: component lists round trip");
            }
        }
        // Version 1 payload: one track, one state (time + four floats).
        std::vector<std::uint8_t> v1;
        auto put = [&v1](const void* data, std::size_t size) {
            v1.insert(v1.end(), static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
        };
        const std::uint64_t one = 1;
        const std::uint32_t id = 0x00030004u;
        const std::uint32_t at = 40;
        const float wear[4] = {0.5f, 0.25f, 0.125f, 0.0f};
        put(&one, sizeof(one));
        put(&id, sizeof(id));
        put(&one, sizeof(one));
        put(&at, sizeof(at));
        put(wear, sizeof(wear));
        const std::vector<std::uint8_t> old = WithSectionPayload(bytes, "weapon conditions", 1, v1);
        Check(!old.empty(), "look: v1 payload built");
        if (!old.empty())
        {
            Clip older;
            const bool oldOk = clipfile::Decode(old.data(), old.size(), kBase, kImageSize, older.recording, older.poses,
                older.camera, duration, error);
            Check(oldOk && older.recording.weaponConditions.size() == 1 &&
                    older.recording.weaponConditions[0].states.size() == 1,
                "look: a v1 section (older clips) loads");
            if (oldOk && older.recording.weaponConditions.size() == 1 && older.recording.weaponConditions[0].states.size() == 1)
            {
                const auto& state = older.recording.weaponConditions[0].states[0];
                Check(state.first == 40 && state.second.dirt == 0.5f && state.second.soot == 0.25f &&
                        state.second.damage == 0.125f && state.second.flags == 0 && state.second.components.empty() &&
                        state.second.permanent == 0.0f && state.second.wearE4 == 0.0f,
                    "look: v1 wear kept, no flags, no list (the create's components apply)");
            }
        }
        const std::vector<std::uint8_t> future = WithOlderSection(bytes, "weapon conditions", 3, 0);
        Check(!future.empty() && DecodeFails(future, error), "look: a newer version is refused");
        // A list past the cap: the optional section is skipped, the clip opens.
        Clip badClip;
        MakeClip(badClip);
        WeaponConditionTrack bad;
        bad.recordId = 0x00010002u;
        WeaponCondition big;
        big.flags = weaponlook::kFlagComponents;
        for (std::uint32_t n = 1; n <= weaponlook::kComponentsMax + 1; ++n)
            big.components.push_back(n);
        bad.states.push_back({0, big});
        badClip.recording.weaponConditions.push_back(bad);
        std::vector<std::uint8_t> badBytes;
        Check(clipfile::Encode(badClip.recording, badClip.poses, badClip.camera, 1000, kBase, kImageSize, badBytes, error),
            "look: encode an oversized list");
        std::string note;
        Check(DecodeSkips(badBytes, "weapon conditions", note), "look: a list past the cap skips WCND, the clip opens");
    }
}

int RunClipFileTests()
{
    TestRoundTrip();
    TestMetapedSection();
    TestPointerRules();
    TestVersionsAndSections();
    TestSpeechRequestsSection();
    TestWeaponFxIntentsSection();
    TestFxSettingsSections();
    TestDecalSectionVersions();
    TestListenerV2Sections();
    TestShakeEventsSectionVersions();
    TestFootstepSectionVersions();
    TestScenerySectionVersions();
    TestFragPiecesSection();
    TestSceneryChildrenSection();
    TestEnvironmentTrackVersions();
    TestEnvironmentRepairOnLoad();
    TestGateSections();
    TestVehicleWindowsSection();
    TestRetiredWaterGpuSection();
    TestVehiclePartsSection();
    TestVehicleDoorsSection();
    TestVehiclePieceCreate();
    TestVehicleBurntSection();
    TestVehicleLooksSection();
    TestSpeechRequestsSoundSet();
    TestMixSceneSections();
    TestBurnLevelsSection();
    TestSceneryBurnLevelsSection();
    TestObjectLooksSection();
    TestSceneryHatsSection();
    TestFireCharsSection();
    TestAppearanceSection();
    TestAnimAudioSection();
    TestWorldSections();
    TestSkyTrackSection();
    TestTimecycleSection();
    TestMoonSection();
    TestEnvironmentEditSection();
    TestSoundGenericSection();
    TestSoundGenericInitVariables();
    TestSoundGenericValueDwords();
    TestOptionalSectionSkip();
    TestWeaponLookSection();
    TestTruncated();
    TestCompressed();
    TestCompressAge();
    TestPackedTransforms();
    TestPackedFiles();
    TestSavePack();
    TestEntitySoundOffsets();
    TestTrainsSection();
    TestSoundCheckSections();
    TestLoadLimits();
    TestValidateLoaded();
    TestUniqueClipName();
    TestNumberedClipName();
    TestSaveQueue();
    TestRealClipsFromEnv();   // REPLAYPOC_CLIP_DIR only
    if (g_clipFailures == 0)
        std::printf("Clip file tests passed.\n");
    return g_clipFailures;
}
