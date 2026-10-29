#include "replay_trains.h"

#include "entity_cache.h"
#include "entity_selection.h"
#include "guarded_memory.h"
#include "log_format.h"
#include "pose_capture.h"
#include "tpose_check.h" // the pose records' signature
#include "pose_playback.h"
#include "profiler.h"
#include "skeleton_resolve.h"
#include "train_consist.h"
#include "replay_objects.h" // PlayRecordedPose
#include "vehicle_doors_engine.h" // the cars' doors

#include "main.h"
#include "natives.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <map>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

// Trains (replay_trains.h, train_consist.h).

namespace replay::trains
{
    namespace
    {
        Context* g_context = nullptr;
        Context& Ctx() { return *g_context; }

        constexpr std::uint32_t kIntervalMs = 1000 / replay::recorder::kSampleHz;

        // ----- Engine structure (build 1491.50), by signature -----

        // The car count of _GET_TRAIN_CARRIAGE_TRAILER_NUMBER (0x10E3264
        // jumps here for vehicle types 17..19): 1 + the cars behind
        // (+0x11F8) + the cars in front (+0x11F0).
        constexpr std::uint32_t kCarCountRva = 0x1504F90;
        constexpr std::uint8_t kCarCountSignature[] = {
            0xB8, 0x01, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74, 0x27, 0x48, 0x8B, 0x91, 0xF8, 0x11, 0x00, 0x00,
            0xEB, 0x09, 0x48, 0x8B, 0x92, 0xF8, 0x11, 0x00, 0x00, 0xFF, 0xC0, 0x48, 0x85, 0xD2, 0x75, 0xF2, 0xEB,
            0x02, 0xFF, 0xC0, 0x48, 0x8B, 0x89, 0xF0, 0x11, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x75, 0xF2, 0xC3};
        // GET_TRAIN_CARRIAGE's walk (0x10E80C4 calls it): n steps along
        // +0x11F8 from the train, so +0x11F8 is the car behind.
        constexpr std::uint32_t kCarriageWalkRva = 0x1510670;
        constexpr std::uint8_t kCarriageWalkSignature[] = {
            0x32, 0xC0, 0x84, 0xD2, 0x74, 0x12, 0x48, 0x8B, 0x89, 0xF8, 0x11, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74,
            0x0A, 0xFE, 0xC0, 0x3A, 0xC2, 0x72, 0xEE, 0x48, 0x8B, 0xC1, 0xC3};
        // The trailer number's type test: [car+0xBB0] - 17 <= 2.
        constexpr std::uint32_t kTypeTestRva = 0x10E3272;
        constexpr std::uint8_t kTypeTestSignature[] = {0x8B, 0x88, 0xB0, 0x0B, 0x00, 0x00, 0x83, 0xE9, 0x11, 0x83, 0xF9, 0x02};
        // _TRIGGER_TRAIN_WHISTLE's core (0x10F7A24): the same type test,
        // then the train's audio entity at [car+0x11E0], on which the
        // whistle is played (0x154D500).
        constexpr std::uint32_t kAudioRva = 0x10F7A69;
        constexpr std::uint8_t kAudioSignature[] = {0x8B, 0x8F, 0xB0, 0x0B, 0x00, 0x00, 0x83, 0xE9, 0x11, 0x83, 0xF9,
            0x02, 0x77, 0x2F, 0x48, 0x8B, 0x9F, 0xE0, 0x11, 0x00, 0x00};
        // SET_RANDOM_TRAINS (handler 0x10E0CA4): the byte `arg == 0` goes to
        // a global (0x15088AC: mov [rip+x], cl; ret): nonzero = random trains
        // off. Read to give the user's state back at the end.
        constexpr std::uint32_t kRandomTrainsHandlerRva = 0x10E0CA4;
        constexpr std::uint8_t kRandomTrainsHandlerSignature[] = {0x48, 0x8B, 0x41, 0x10, 0x83, 0x38, 0x00, 0x0F, 0x94, 0xC1, 0xE9};
        constexpr std::uint32_t kRandomTrainsSetRva = 0x15088AC;
        // The train config lookup (0x151D5F0, _GET_NUM_CARS_FROM_TRAIN_CONFIG
        // through 0x10EBFF4): the count at a global, the configs in an array
        // of 0x1260 bytes whose first dword is the config's hash. Bench only
        // (train_spawn auto).
        constexpr std::uint32_t kConfigLookupRva = 0x151D5F0;
        constexpr std::uint8_t kConfigLookupHead[] = {0x44, 0x8B, 0x0D};
        constexpr std::uint8_t kConfigLookupMiddle[] = {0x45, 0x32, 0xC0, 0x45, 0x85, 0xC9, 0x7E, 0x27, 0x33, 0xC0, 0x48, 0x69,
            0xD0, 0x60, 0x12, 0x00, 0x00, 0x48, 0x8D, 0x05};
        constexpr std::uint32_t kConfigStride = 0x1260;

        constexpr std::uint32_t kForwardLink = 0x11F0;
        constexpr std::uint32_t kBackwardLink = 0x11F8;
        constexpr std::uint32_t kVehicleType = 0xBB0;
        constexpr std::uint32_t kAudioEntity = 0x11E0;

        struct Engine
        {
            bool checked = false;
            bool links = false;  // car links and the walk
            bool types = false;  // [car+0xBB0]
            bool audio = false;  // [car+0x11E0]
            bool random = false; // the random trains flag
            bool configs = false;
            std::uintptr_t randomFlag = 0; // absolute
            std::uintptr_t configCount = 0;
            std::uintptr_t configBase = 0;
            std::string error;
        };
        Engine g_engine;

        std::uintptr_t Base() { return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)); }

        bool Same(std::uintptr_t at, const std::uint8_t* bytes, std::size_t size)
        {
            std::uint8_t copy[64]{};
            if (size > sizeof(copy) || !replay::mem::Copy(copy, at, size))
                return false;
            return std::memcmp(copy, bytes, size) == 0;
        }

        std::int32_t Rel32(std::uintptr_t at)
        {
            std::int32_t value = 0;
            replay::mem::Read(at, value);
            return value;
        }

        void CheckEngine()
        {
            if (g_engine.checked)
                return;
            g_engine.checked = true;
            const std::uintptr_t base = Base();
            g_engine.links = Same(base + kCarCountRva, kCarCountSignature, sizeof(kCarCountSignature)) &&
                Same(base + kCarriageWalkRva, kCarriageWalkSignature, sizeof(kCarriageWalkSignature));
            g_engine.types = Same(base + kTypeTestRva, kTypeTestSignature, sizeof(kTypeTestSignature));
            g_engine.audio = g_engine.types && Same(base + kAudioRva, kAudioSignature, sizeof(kAudioSignature));
            if (Same(base + kRandomTrainsHandlerRva, kRandomTrainsHandlerSignature, sizeof(kRandomTrainsHandlerSignature)))
            {
                // The handler's jump lands on the setter; the setter stores cl.
                const std::uintptr_t jumpEnd = base + kRandomTrainsHandlerRva + sizeof(kRandomTrainsHandlerSignature) + 4;
                const std::uintptr_t target = jumpEnd + static_cast<std::intptr_t>(Rel32(jumpEnd - 4));
                std::uint8_t setter[7]{};
                if (target == base + kRandomTrainsSetRva && replay::mem::Copy(setter, target, sizeof(setter)) &&
                    setter[0] == 0x88 && setter[1] == 0x0D && setter[6] == 0xC3)
                {
                    std::int32_t disp = 0;
                    std::memcpy(&disp, setter + 2, sizeof(disp));
                    g_engine.randomFlag = target + 6 + static_cast<std::intptr_t>(disp);
                    g_engine.random = true;
                }
            }
            {
                const std::uintptr_t at = base + kConfigLookupRva;
                if (Same(at, kConfigLookupHead, sizeof(kConfigLookupHead)) && Same(at + 7, kConfigLookupMiddle, sizeof(kConfigLookupMiddle)))
                {
                    g_engine.configCount = at + 7 + static_cast<std::intptr_t>(Rel32(at + 3));
                    g_engine.configBase = at + 0x1F + static_cast<std::intptr_t>(Rel32(at + 0x1B));
                    g_engine.configs = true;
                }
            }
            std::string error;
            if (!g_engine.links)
                error += "links;";
            if (!g_engine.types)
                error += "types;";
            if (!g_engine.audio)
                error += "audio;";
            if (!g_engine.random)
                error += "random_trains;";
            if (!g_engine.configs)
                error += "configs;";
            g_engine.error = error;
        }

        int VehicleTypeAt(std::uint64_t car)
        {
            std::int32_t type = 0;
            if (!g_engine.types || !car || !replay::mem::Read(static_cast<std::uintptr_t>(car + kVehicleType), type))
                return -1;
            return type;
        }

        bool ReadLinks(std::uint64_t car, CarLinks& links)
        {
            links = {};
            if (!g_engine.links || !IsTrainVehicleType(VehicleTypeAt(car)))
                return false;
            std::uint64_t forward = 0, backward = 0;
            if (!replay::mem::Read(static_cast<std::uintptr_t>(car + kForwardLink), forward) ||
                !replay::mem::Read(static_cast<std::uintptr_t>(car + kBackwardLink), backward))
                return false;
            links.address = car;
            links.forward = forward;
            links.backward = backward;
            return true;
        }

        int g_puppetMode = 0;
        int g_worldClearMode = 0;

        // ----- Recording -----

        std::unordered_map<std::uint32_t, bool> g_trainModels; // model -> IS_THIS_MODEL_A_TRAIN
        struct RecordStats
        {
            std::uint32_t consists = 0;
            std::uint32_t cars = 0;
            std::uint32_t carsMax = 0;
            std::uint32_t walksMemory = 0;
            std::uint32_t walksNatives = 0;
            std::uint32_t walkErrors = 0;
            std::uint32_t full = 0;       // cars left out, kMaxRecordedCars
            std::uint32_t noHandle = 0;   // a linked car without a script handle
            std::uint32_t consistChanges = 0;
            std::uint64_t samples = 0;
            std::uint32_t speedSamples = 0;
            std::uint32_t retiredGone = 0;
            std::uint32_t retiredRange = 0;
            std::uint32_t logs = 0;
            WalkStatus lastError = WalkStatus::Ok;
        };
        RecordStats g_record;
        std::uint32_t g_nextSpeedMs = 0;
        std::uint32_t g_lookLogs = 0;
        // The cars' pose records (train_consist.h kPoseCars).
        struct PoseStats
        {
            std::uint32_t started = 0;
            std::uint32_t late = 0;
            std::uint32_t refusedFar = 0;
            std::uint32_t refusedFull = 0;
            std::uint32_t unresolved = 0;
            std::uint32_t startFailed = 0;
            std::uint32_t skeletonChanged = 0;
            std::uint32_t peak = 0;
            std::uint32_t logs = 0;
        };
        PoseStats g_pose;
        std::uint32_t g_nextPoseFillMs = 0;

        bool ModelIsTrain(Hash model)
        {
            const auto found = g_trainModels.find(static_cast<std::uint32_t>(model));
            if (found != g_trainModels.end())
                return found->second;
            const bool train = VEHICLE::IS_THIS_MODEL_A_TRAIN(model) != FALSE;
            g_trainModels.emplace(static_cast<std::uint32_t>(model), train);
            return train;
        }

        replay::recorder::RecordedTrainCar* FindRecorded(Entity handle)
        {
            for (replay::recorder::RecordedTrainCar& car : Ctx().recorder.trainCars)
                if (car.handle == handle)
                    return &car;
            return nullptr;
        }

        replay::pose::WorldTransform ReadTransform(Entity entity)
        {
            const Vector3 origin = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(entity, 0.0f, 0.0f, 0.0f);
            const Vector3 x = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(entity, 1.0f, 0.0f, 0.0f);
            const Vector3 y = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(entity, 0.0f, 1.0f, 0.0f);
            const Vector3 z = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(entity, 0.0f, 0.0f, 1.0f);
            replay::pose::WorldTransform transform;
            transform.position = {origin.x, origin.y, origin.z};
            transform.rotation = replay::pose::QuatFromBasis({x.x - origin.x, x.y - origin.y, x.z - origin.z},
                {y.x - origin.x, y.y - origin.y, y.z - origin.z}, {z.x - origin.x, z.y - origin.y, z.z - origin.z});
            return transform;
        }

        // The consist of `car` front first, as handles: through the engine
        // links (memory), else the natives (the car and those behind it).
        struct Walked
        {
            std::vector<Entity> handles;
            std::vector<std::uint64_t> addresses;
            bool fromLinks = false;
            WalkStatus status = WalkStatus::Ok;
        };

        void WalkFrom(Entity car, Walked& out)
        {
            out = Walked{};
            const std::uint64_t address = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(car));
            if (g_engine.links && address)
            {
                std::vector<CarLinks> ordered;
                out.status = WalkConsist(address, [](std::uint64_t at, CarLinks& links) { return ReadLinks(at, links); }, ordered);
                if (!ordered.empty())
                {
                    out.fromLinks = true;
                    for (const CarLinks& links : ordered)
                    {
                        // A recorded car keeps its handle; only a new one
                        // costs a pool lookup (and a script handle).
                        Entity handle = links.address == address ? car : 0;
                        for (const replay::recorder::RecordedTrainCar& known : Ctx().recorder.trainCars)
                            if (!handle && known.address == links.address && replay::entities::Exists(known.handle))
                                handle = known.handle;
                        bool available = false;
                        if (!handle)
                            handle = replay::entities::HandleAtAddress(links.address, available);
                        if (!handle)
                        {
                            ++g_record.noHandle;
                            continue;
                        }
                        out.handles.push_back(handle);
                        out.addresses.push_back(links.address);
                    }
                    if (!out.handles.empty())
                        return;
                }
            }
            // The natives: the car and every carriage behind it.
            out.fromLinks = false;
            out.handles.push_back(car);
            out.addresses.push_back(address);
            const int count = (std::min)(VEHICLE::_GET_TRAIN_CARRIAGE_TRAILER_NUMBER(car), static_cast<int>(kMaxConsistCars));
            for (int n = 1; n < count; ++n)
            {
                const Vehicle behind = VEHICLE::GET_TRAIN_CARRIAGE(car, n);
                if (!behind || !replay::entities::Exists(behind))
                    break;
                out.handles.push_back(behind);
                out.addresses.push_back(reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(behind)));
            }
        }

        float DistanceTo(Entity entity, const Vector3& a, const Vector3& b)
        {
            float at[3]{};
            if (!replay::entities::Position(entity, at))
                return 1.0e9f;
            const float ax = at[0] - a.x, ay = at[1] - a.y, az = at[2] - a.z;
            const float bx = at[0] - b.x, by = at[1] - b.y, bz = at[2] - b.z;
            return std::sqrt((std::min)(ax * ax + ay * ay + az * az, bx * bx + by * by + bz * bz));
        }

        // The car's whole look as a vehicle's (VehicleLook, the latest one):
        // paint, livery, extras and the two prop sets, the cargo read as
        // CaptureLook reads it, so the replayed train is the recorded one.
        void CaptureLook(Entity car, TrainCar& info);
        void ReadCarLook(Entity car, replay::VehicleLook& look)
        {
            TrainCar sets;
            CaptureLook(car, sets);
            look.propSet = sets.propSet;
            look.lightPropSet = sets.lightPropSet;
            look.extrasExist = 0;
            look.extrasOn = 0;
            for (int extra = 0; extra < 32; ++extra)
            {
                if (!VEHICLE::DOES_EXTRA_EXIST(car, extra))
                    continue;
                look.extrasExist |= 1u << extra;
                if (VEHICLE::IS_VEHICLE_EXTRA_TURNED_ON(car, extra))
                    look.extrasOn |= 1u << extra;
            }
            look.tint = VEHICLE::_GET_VEHICLE_TINT(car);
            look.livery = VEHICLE::_GET_VEHICLE_LIVERY(car);
        }

        bool SameLook(const replay::VehicleLook& a, const replay::VehicleLook& b)
        {
            return a.propSet == b.propSet && a.lightPropSet == b.lightPropSet && a.extrasExist == b.extrasExist &&
                a.extrasOn == b.extrasOn && a.tint == b.tint && a.livery == b.livery;
        }

        replay::VehicleLook* LookOf(replay::entity::ReplayId id)
        {
            for (replay::VehicleLook& look : Ctx().clip.vehicleLooks)
                if (look.id == id)
                    return &look;
            return nullptr;
        }

        void LogLook(const char* phase, const replay::VehicleLook& look, std::uint32_t timeMs)
        {
            if (g_lookLogs >= 64)
                return;
            ++g_lookLogs;
            Ctx().log << "TRAIN_CAR_LOOK,phase=" << phase << ",id=" << replay::entity::IdText(look.id) << std::hex
                      << ",propset=0x" << look.propSet << ",light=0x" << look.lightPropSet << ",extras=0x" << look.extrasExist
                      << ",extras_on=0x" << look.extrasOn << std::dec << ",tint=" << look.tint << ",livery=" << look.livery
                      << ",time_ms=" << timeMs << '\n';
        }

        void CaptureLook(Entity car, TrainCar& info)
        {
            // Cargo: the carriage prop set (a handle), else the vehicle one.
            if (const PropSet carriage = PROPSET::_GET_TRAIN_CARRIAGE_PROP_SET(car))
                info.propSet = PROPSET::_GET_PROP_SET_MODEL(carriage);
            if (!info.propSet && PROPSET::_DOES_VEHICLE_HAVE_ANY_PROP_SET(car))
                info.propSet = PROPSET::_GET_VEHICLE_PROP_SET_HASH(car);
            if (PROPSET::_DOES_VEHICLE_HAVE_ANY_LIGHT_PROP_SET(car))
                info.lightPropSet = PROPSET::_GET_PROP_SET_MODEL(PROPSET::_GET_VEHICLE_LIGHT_PROP_SET(car));
        }

        std::size_t CountPosedCars()
        {
            std::size_t posed = 0;
            for (const replay::recorder::RecordedTrainCar& car : Ctx().recorder.trainCars)
                posed += car.posed ? 1u : 0u;
            return posed;
        }

        float CarDistance(Entity car)
        {
            const Vector3 origin = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE, FALSE);
            const Vector3 camera = CAM::GET_FINAL_RENDERED_CAM_COORD();
            return DistanceTo(car, origin, camera);
        }

        // A pose record for the car (its wheels and rods, train_consist.h
        // kPoseCars) when it has bones, a place is free and it is near.
        // `bones` gets the count started (0: none). The status for the log.
        const char* TryPoseCar(replay::recorder::RecordedTrainCar& car, float distanceM, bool late, std::uint32_t& bones)
        {
            bones = 0;
            if (car.posed)
                return "posed";
            replay::skeleton::ResolvedPoseSkeleton skeleton{};
            if (!replay::skeleton::FastResolvePedPoseSkeleton(car.handle, skeleton))
            {
                ++g_pose.unresolved;
                return "skeleton_unresolved";
            }
            const std::size_t posed = CountPosedCars();
            if (!ShouldPoseCar(skeleton.boneCount, posed, distanceM))
            {
                if (skeleton.boneCount < kPoseMinBones)
                    return "rigid";
                if (distanceM > kPoseRadiusM)
                {
                    ++g_pose.refusedFar;
                    return "far";
                }
                ++g_pose.refusedFull;
                return "full";
            }
            std::string error;
            if (!replay::pose::capture::Start(car.id.Packed(), skeleton.skeleton, skeleton.boneCount,
                    replay::recorder::kSampleHz, error))
            {
                ++g_pose.startFailed;
                if (g_pose.logs < 48)
                {
                    ++g_pose.logs;
                    Ctx().log << "TRAIN_CAR_POSE,phase=record,id=" << replay::entity::IdText(car.id)
                              << ",status=start_failed,error=" << error << ",bones=" << skeleton.boneCount << '\n';
                }
                return "start_failed";
            }
            car.posed = true;
            car.poseSkeleton = skeleton.skeleton;
            car.poseBones = skeleton.boneCount;
            bones = skeleton.boneCount;
            ++g_pose.started;
            g_pose.late += late ? 1u : 0u;
            g_pose.peak = (std::max)(g_pose.peak, static_cast<std::uint32_t>(posed + 1));
            if (g_pose.logs < 48)
            {
                ++g_pose.logs;
                Ctx().log << "TRAIN_CAR_POSE,phase=record,id=" << replay::entity::IdText(car.id) << ",status=started"
                          << ",bones=" << skeleton.boneCount << ",distance_m=" << static_cast<int>(distanceM)
                          << ",late=" << (late ? 1 : 0) << ",places=" << (posed + 1) << "/" << kPoseCars
                          << ",time_ms=" << replay::pose::capture::NowMs() << '\n';
            }
            return "started";
        }

        bool RegisterCar(Entity car, std::uint64_t address, std::uint64_t head, bool fromLinks, float distanceM = 1.0e9f)
        {
            if (FindRecorded(car))
                return true;
            if (Ctx().recorder.trainCars.size() >= kMaxRecordedCars)
            {
                ++g_record.full;
                return false;
            }
            const replay::entity::ReplayId id = Ctx().clip.ids.Allocate();
            if (!id.Valid())
                return false;
            const Hash model = ENTITY::GET_ENTITY_MODEL(car);
            replay::entity::CreatePacket packet;
            packet.id = id;
            packet.timeMs = replay::pose::capture::NowMs();
            packet.kind = replay::entity::EntityKind::TrainCar;
            packet.modelHash = model;
            packet.sourceHandle = car;
            replay::recorder::RecordedTrainCar recorded;
            recorded.handle = car;
            recorded.id = id;
            recorded.address = address;
            recorded.head = head;
            // The pose record first, so the create carries its bone count.
            std::uint32_t posedBones = 0;
            TryPoseCar(recorded, distanceM, false, posedBones);
            packet.boneCount = posedBones;
            Ctx().clip.lifecycle.Create(packet);
            Ctx().clip.created.push_back(packet);
            Ctx().clip.weaponTracks.push_back({id.Packed(), replay::entity::RelativeTrack(replay::recorder::kSampleHz)});
            recorded.lookCheckMs = packet.timeMs;
            Ctx().recorder.trainCars.push_back(recorded);
            TrainCar info;
            info.id = id.Packed();
            const int type = VehicleTypeAt(address);
            info.vehicleType = type > 0 && type < 256 ? static_cast<std::uint8_t>(type) : 0;
            info.flags = static_cast<std::uint8_t>((ENTITY::IS_ENTITY_A_MISSION_ENTITY(car) ? kCarMission : 0) |
                (fromLinks ? kCarLinksRead : 0));
            CaptureLook(car, info);
            Ctx().clip.trains.cars.push_back(info);
            ++g_record.cars;
            // The whole look, kept as its latest.
            replay::VehicleLook look;
            look.id = id;
            ReadCarLook(car, look);
            look.lastCheckMs = packet.timeMs;
            Ctx().clip.vehicleLooks.push_back(look);
            LogLook("record", look, packet.timeMs);
            return true;
        }

        // The links between the consist's recorded cars, in the walk's
        // order (front first); the track and direction of its front car.
        void LinkRecorded(const std::vector<Entity>& frontFirst)
        {
            std::vector<std::uint32_t> ids;
            Entity front = 0;
            for (const Entity handle : frontFirst)
                if (const replay::recorder::RecordedTrainCar* car = FindRecorded(handle))
                {
                    ids.push_back(car->id.Packed());
                    if (!front)
                        front = handle;
                }
            if (ids.empty())
                return;
            LinkConsist(ids, Ctx().clip.trains.cars);
            const std::int32_t track = VEHICLE::_GET_TRAIN_TRACK_FROM_TRAIN_VEHICLE(front);
            const bool forward = VEHICLE::_GET_TRAIN_DIRECTION(front) != FALSE;
            for (TrainCar& car : Ctx().clip.trains.cars)
            {
                if (std::find(ids.begin(), ids.end(), car.id) == ids.end())
                    continue;
                car.track = track;
                car.flags = static_cast<std::uint8_t>((car.flags & ~kCarForward) | (forward ? kCarForward : 0));
            }
        }

        void RetireCar(std::size_t index, const char* reason)
        {
            const replay::recorder::RecordedTrainCar car = Ctx().recorder.trainCars[index];
            const std::uint32_t timeMs = replay::pose::capture::NowMs();
            if (car.posed)
                replay::pose::capture::Stop(car.id.Packed()); // its place is free
            Ctx().clip.lifecycle.Delete(car.id, timeMs);
            Ctx().clip.ids.Release(car.id);
            Ctx().recorder.trainCars.erase(Ctx().recorder.trainCars.begin() + static_cast<std::ptrdiff_t>(index));
            const replay::entity::RelativeTrack* track = Ctx().clip.WeaponTrackOf(car.id.Packed());
            Ctx().log << "POSE_V2_ENTITY_DELETE,id=" << replay::entity::IdText(car.id) << ",kind=train_car,handle=" << car.handle
                      << ",reason=" << reason << ",samples=" << (track ? track->Size() : 0) << ",time_ms=" << timeMs << '\n';
        }

        // Enters a consist: every car of the walk, front first.
        void EnterConsist(const Walked& walked, float nearestM)
        {
            if (walked.handles.empty())
                return;
            const std::uint64_t head = walked.addresses.front()
                ? walked.addresses.front() : static_cast<std::uint64_t>(walked.handles.front());
            std::size_t registered = 0;
            // The nearest cars first, so the pose places go to them
            // (train_consist.h PoseOrder).
            std::vector<float> distances(walked.handles.size());
            {
                const Vector3 origin = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE, FALSE);
                const Vector3 camera = CAM::GET_FINAL_RENDERED_CAM_COORD();
                for (std::size_t n = 0; n < walked.handles.size(); ++n)
                    distances[n] = DistanceTo(walked.handles[n], origin, camera);
            }
            for (const std::size_t n : PoseOrder(distances))
                registered += RegisterCar(walked.handles[n], walked.addresses[n], head, walked.fromLinks, distances[n]) ? 1 : 0;
            if (!registered)
                return;
            LinkRecorded(walked.handles);
            ++g_record.consists;
            (walked.fromLinks ? g_record.walksMemory : g_record.walksNatives) += 1;
            if (walked.status != WalkStatus::Ok)
            {
                ++g_record.walkErrors;
                g_record.lastError = walked.status;
            }
            g_record.carsMax = (std::max)(g_record.carsMax, static_cast<std::uint32_t>(Ctx().recorder.trainCars.size()));
            std::ostream& log = Ctx().log;
            for (std::size_t n = 0; n < walked.handles.size(); ++n)
            {
                const replay::recorder::RecordedTrainCar* car = FindRecorded(walked.handles[n]);
                if (!car)
                    continue;
                const TrainCar* info = Ctx().clip.trains.Find(car->id.Packed());
                log << "POSE_V2_ENTITY_CREATE,id=" << replay::entity::IdText(car->id) << ",kind=train_car,handle=" << car->handle
                    << ",model=0x" << std::hex << static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(car->handle)) << std::dec
                    << ",index=" << (info ? info->index : 0) << ",cars=" << (info ? info->cars : 0)
                    << ",type=" << (info ? info->vehicleType : 0) << ",mission=" << (info && (info->flags & kCarMission) ? 1 : 0)
                    << std::hex << ",propset=0x" << (info ? info->propSet : 0) << ",light=0x" << (info ? info->lightPropSet : 0)
                    << std::dec << ",bones=" << [&] {
                           replay::skeleton::ResolvedPoseSkeleton skeleton{};
                           return replay::skeleton::FastResolvePedPoseSkeleton(car->handle, skeleton) ? skeleton.boneCount : 0u;
                       }()
                    << ",links=" << (walked.fromLinks ? "memory" : "natives")
                    << ",walk=" << WalkStatusText(walked.status) << ",nearest_m=" << static_cast<int>(nearestM)
                    << ",time_ms=" << replay::pose::capture::NowMs() << '\n';
            }
        }

        // Per car and tick, after its world sample: the doors
        // (vehicle_doors_engine.h, rate gated there), the look once a second,
        // the pose record's poll (the finalizer does not visit vehicle
        // skeletons) and, once a second, a free pose place to the nearest car
        // without one.
        void SampleCarState(std::uint32_t timeMs)
        {
            REPLAY_PROFILE_ZONE("rec.trains.state");
            for (replay::recorder::RecordedTrainCar& car : Ctx().recorder.trainCars)
            {
                if (!replay::entities::Exists(car.handle))
                    continue;
                replay::vehdoors::RecordVehicle(Ctx().clip.vehicleDoors, car.id.Packed(), car.handle, true, timeMs, Ctx().log);
                if (timeMs - car.lookCheckMs >= 1000)
                {
                    car.lookCheckMs = timeMs;
                    if (replay::VehicleLook* look = LookOf(car.id))
                    {
                        replay::VehicleLook now = *look;
                        ReadCarLook(car.handle, now);
                        if (!SameLook(now, *look))
                        {
                            now.lastCheckMs = timeMs;
                            *look = now;
                            LogLook("change", now, timeMs);
                        }
                        look->lastCheckMs = timeMs;
                    }
                }
                if (!car.posed)
                    continue;
                replay::skeleton::ResolvedPoseSkeleton current{};
                if (replay::skeleton::FastResolvePedPoseSkeleton(car.handle, current) && current.skeleton == car.poseSkeleton &&
                    current.boneCount == car.poseBones)
                {
                    replay::pose::capture::PollFromScript(car.id.Packed());
                    continue;
                }
                // A rebuilt skeleton ends the record (the armed pointer is
                // no longer the car's); the place goes back.
                replay::pose::capture::Stop(car.id.Packed());
                car.posed = false;
                ++g_pose.skeletonChanged;
                if (g_pose.logs < 48)
                {
                    ++g_pose.logs;
                    Ctx().log << "TRAIN_CAR_POSE,phase=record,id=" << replay::entity::IdText(car.id)
                              << ",status=skeleton_changed,bones=" << current.boneCount << ",armed_bones=" << car.poseBones
                              << ",time_ms=" << timeMs << '\n';
                }
            }
            if (timeMs < g_nextPoseFillMs)
                return;
            g_nextPoseFillMs = timeMs + 1000;
            if (CountPosedCars() >= kPoseCars)
                return;
            // Late: a car never posed (one that lost its record keeps the
            // track it has) within the radius, nearest first.
            std::vector<replay::recorder::RecordedTrainCar*> waiting;
            std::vector<float> distances;
            for (replay::recorder::RecordedTrainCar& car : Ctx().recorder.trainCars)
            {
                if (car.posed || car.poseBones || !replay::entities::Exists(car.handle))
                    continue;
                waiting.push_back(&car);
                distances.push_back(CarDistance(car.handle));
            }
            for (const std::size_t n : PoseOrder(distances))
            {
                if (CountPosedCars() >= kPoseCars || distances[n] > kPoseRadiusM)
                    break;
                std::uint32_t bones = 0;
                TryPoseCar(*waiting[n], distances[n], true, bones);
                if (!bones)
                    continue;
                // The create the clip keeps takes the bone count (the
                // lifecycle's stays 0; object_pose.h PlaybackBones).
                for (auto packet = Ctx().clip.created.rbegin(); packet != Ctx().clip.created.rend(); ++packet)
                    if (packet->id == waiting[n]->id && packet->kind == replay::entity::EntityKind::TrainCar)
                    {
                        packet->boneCount = bones;
                        break;
                    }
            }
        }

        // ----- Playback -----

        struct PuppetState
        {
            PuppetKind kind = PuppetKind::Vehicle;
            PuppetJudge judge;
            bool placedLastTick = false;
            bool propSetsRequested = false;
            // The look read back once the puppet rendered (one reapply on a
            // mismatch), and the pose record met.
            ULONGLONG lookCheckAt = 0;
            std::uint32_t lookChecks = 0;
            bool lookJudged = false;
            bool poseMet = false;
            bool poseArmed = false;
            bool poseStill = false;
        };
        std::unordered_map<std::uint32_t, PuppetState> g_puppets;

        struct PlayStats
        {
            std::uint32_t created = 0;
            std::uint32_t asVehicle = 0;
            std::uint32_t asObject = 0;
            std::uint32_t refused = 0;
            std::uint32_t fallbackObject = 0;
            std::uint32_t failed = 0;
            std::uint32_t shownMax = 0;
            std::uint32_t aliveMax = 0;
            std::uint32_t lost = 0;
            float driftWorst = 0.0f;
            std::uint32_t logs = 0;
            std::uint32_t censusLogs = 0;
            ULONGLONG censusAt = 0;
            // The look and the pose records.
            std::uint32_t looks = 0;        // puppets with a recorded look
            std::uint32_t lookOk = 0;
            std::uint32_t lookMismatch = 0;
            std::uint32_t lookReapplied = 0;
            std::uint32_t lookLogs = 0;
            std::uint32_t poseRecords = 0;  // cars of the clip with a pose record
            std::uint32_t poseMet = 0;      // puppets that reached their record's span
            std::uint32_t poseArmed = 0;
            std::uint32_t poseStill = 0;    // records that never moved (left to the engine)
            std::uint32_t poseLogs = 0;
        };
        PlayStats g_play;

        struct HiddenCar
        {
            replay::pose::playback::HiddenPedState state;
            std::uint64_t address = 0;
            bool mission = false;
            std::uint32_t rehidden = 0;
        };
        struct HaltedTrain
        {
            Vehicle head = 0;
            float speed = 0.0f;
            ULONGLONG haltedAt = 0;
            bool checked = false;
        };
        std::vector<HiddenCar> g_hidden;
        std::vector<HaltedTrain> g_halted;
        bool g_randomChanged = false;
        int g_randomPrior = -1; // 1 random trains were off, 0 on, -1 unknown
        bool g_deletedAll = false;
        std::uint32_t g_worldLogs = 0;
        bool g_playing = false;

        bool RandomTrainsOff(int& value)
        {
            std::uint8_t flag = 0;
            if (!g_engine.random || !replay::mem::Read(g_engine.randomFlag, flag))
            {
                value = -1;
                return false;
            }
            value = flag ? 1 : 0;
            return true;
        }

        void DeleteAllTrains(const char* reason)
        {
            if (g_deletedAll)
                return;
            g_deletedAll = true;
            VEHICLE::DELETE_ALL_TRAINS();
            // What was hidden is gone; nothing to give back.
            g_hidden.clear();
            g_halted.clear();
            Ctx().log << "TRAIN_WORLD,action=delete_all,reason=" << reason << '\n';
        }

        const replay::VehicleLook* PlayLookOf(replay::entity::ReplayId id)
        {
            for (const replay::VehicleLook& look : Ctx().clip.vehicleLooks)
                if (look.id == id)
                    return &look;
            return nullptr;
        }

        // The recorded paint and livery at the create (clips without a look:
        // the model's own).
        Entity CreateCarPuppet(const replay::playback::Puppet& puppet, PuppetKind kind, const replay::pose::WorldTransform& at)
        {
            const replay::VehicleLook* look = PlayLookOf(puppet.packet.id);
            const int tint = look && look->tint != replay::kLookUnset ? look->tint : -1;
            const int livery = look && look->livery != replay::kLookUnset ? look->livery : -1;
            return kind == PuppetKind::Vehicle
                ? replay::pose::playback::CreateVehiclePuppet(puppet.packet.modelHash, tint, livery, at)
                : replay::pose::playback::CreateWeaponPuppet(0, puppet.packet.modelHash, at);
        }

        bool HasPropSet(Vehicle vehicle, Hash wanted)
        {
            if (!wanted)
                return PROPSET::_DOES_VEHICLE_HAVE_ANY_PROP_SET(vehicle) == FALSE;
            if (!PROPSET::_DOES_VEHICLE_HAVE_ANY_PROP_SET(vehicle))
                return false;
            return static_cast<Hash>(PROPSET::_GET_VEHICLE_PROP_SET_HASH(vehicle)) == wanted ||
                static_cast<Hash>(PROPSET::_GET_PROP_SET_MODEL(PROPSET::_GET_VEHICLE_PROP_SET(vehicle))) == wanted;
        }

        bool HasLightPropSet(Vehicle vehicle, Hash wanted)
        {
            if (!wanted)
                return PROPSET::_DOES_VEHICLE_HAVE_ANY_LIGHT_PROP_SET(vehicle) == FALSE;
            return PROPSET::_DOES_VEHICLE_HAVE_ANY_LIGHT_PROP_SET(vehicle) &&
                static_cast<Hash>(PROPSET::_GET_PROP_SET_MODEL(PROPSET::_GET_VEHICLE_LIGHT_PROP_SET(vehicle))) == wanted;
        }

        // The look on the vehicle puppet read back 500 ms after the create; a
        // mismatch is applied once more and judged again, then counted. One
        // line per mismatch.
        void CheckCarLook(replay::playback::Puppet& puppet, PuppetState& state, std::uint32_t timeMs)
        {
            if (state.lookJudged || !state.lookCheckAt || GetTickCount64() < state.lookCheckAt ||
                state.kind != PuppetKind::Vehicle)
                return;
            const replay::VehicleLook* look = PlayLookOf(puppet.packet.id);
            if (!look)
            {
                state.lookJudged = true;
                return;
            }
            ++state.lookChecks;
            const Vehicle clone = puppet.clone;
            const int tint = VEHICLE::_GET_VEHICLE_TINT(clone);
            const int livery = VEHICLE::_GET_VEHICLE_LIVERY(clone);
            const std::uint32_t extras = replay::pose::playback::VehicleExtrasOn(clone, look->extrasExist);
            const bool tintOk = look->tint == replay::kLookUnset || tint == look->tint;
            const bool liveryOk = look->livery == replay::kLookUnset || livery == look->livery;
            const bool extrasOk = (extras & look->extrasExist) == (look->extrasOn & look->extrasExist);
            const bool cargoOk = HasPropSet(clone, look->propSet);
            const bool lightOk = HasLightPropSet(clone, look->lightPropSet);
            if (tintOk && liveryOk && extrasOk && cargoOk && lightOk)
            {
                state.lookJudged = true;
                ++g_play.lookOk;
                return;
            }
            if (state.lookChecks == 1)
            {
                // Once more: the paint and livery natives, then the sets and extras.
                if (!tintOk && look->tint >= 0)
                    VEHICLE::_SET_VEHICLE_TINT(clone, look->tint);
                if (!liveryOk && look->livery >= 0)
                    VEHICLE::_SET_VEHICLE_LIVERY(clone, look->livery);
                replay::pose::playback::ApplyVehicleLook(clone, look->propSet, look->lightPropSet, look->extrasExist, look->extrasOn);
                ++g_play.lookReapplied;
                state.lookCheckAt = GetTickCount64() + 500;
                return;
            }
            state.lookJudged = true;
            ++g_play.lookMismatch;
            if (g_play.lookLogs < 32)
            {
                ++g_play.lookLogs;
                Ctx().log << "TRAIN_CAR_LOOK_CHECK,id=" << replay::entity::IdText(puppet.packet.id) << ",verdict=mismatch"
                          << ",tint=" << tint << "/" << look->tint << ",livery=" << livery << "/" << look->livery << std::hex
                          << ",extras_on=0x" << extras << "/0x" << look->extrasOn << ",propset=0x" << look->propSet
                          << ",light=0x" << look->lightPropSet << std::dec << ",cargo_ok=" << (cargoOk ? 1 : 0)
                          << ",light_ok=" << (lightOk ? 1 : 0)
                          << ",propset_loaded=" << (look->propSet ? PROPSET::_HAS_PROP_SET_LOADED(look->propSet) : 1)
                          << ",playhead_ms=" << timeMs << '\n';
            }
        }

        // The car's pose record (wheels and rods) on its vehicle puppet,
        // through the posed objects' arming (replay_objects PlayRecordedPose):
        // only on a skeleton with the recorded bone count.
        void PlayCarPose(replay::playback::Puppet& puppet, PuppetState& state, std::uint32_t timeMs)
        {
            const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(puppet.packet.id.Packed());
            const std::uint32_t bones = (std::max)(puppet.packet.boneCount, create ? create->boneCount : 0u);
            if (bones < kPoseMinBones || state.kind != PuppetKind::Vehicle)
                return;
            std::uint32_t firstMs = 0;
            std::uint32_t lastMs = 0;
            if (!replay::pose::capture::TrackSpan(puppet.packet.id.Packed(), firstMs, lastMs))
                return;
            // The object path asks an object for its skeleton when it has
            // none: a vehicle puppet is only passed with its skeleton built.
            if (puppet.overrideSlot < 0)
            {
                replay::skeleton::ResolvedPoseSkeleton resolved{};
                if (!replay::skeleton::FastResolvePedPoseSkeleton(puppet.clone, resolved) || resolved.boneCount != bones)
                {
                    if (!state.poseMet && g_play.poseLogs < 48 && timeMs >= firstMs)
                    {
                        ++g_play.poseLogs;
                        Ctx().log << "TRAIN_CAR_POSE,phase=play,id=" << replay::entity::IdText(puppet.packet.id)
                                  << ",status=bones_differ,clone_bones=" << resolved.boneCount << ",track_bones=" << bones
                                  << ",playhead_ms=" << timeMs << '\n';
                    }
                    if (!state.poseMet && timeMs >= firstMs)
                    {
                        state.poseMet = true;
                        ++g_play.poseMet;
                    }
                    return;
                }
            }
            puppet.recordId = puppet.packet.id.Packed();
            puppet.boneCount = bones;
            replay::objects::PlayRecordedPose(puppet, timeMs);
            if (!state.poseMet && timeMs >= firstMs)
            {
                state.poseMet = true;
                ++g_play.poseMet;
            }
            if (!state.poseArmed && puppet.overrideSlot >= 0)
            {
                state.poseArmed = true;
                ++g_play.poseArmed;
            }
            if (!state.poseStill && puppet.stillSkip)
            {
                state.poseStill = true;
                ++g_play.poseStill;
            }
        }

        void DeleteCarPuppet(Entity& clone, PuppetKind kind)
        {
            if (!clone)
                return;
            Ctx().recorder.ignored.push_back({clone, GetTickCount64() + 2000});
            if (kind == PuppetKind::Vehicle)
                replay::pose::playback::DeleteVehiclePuppet(clone);
            else
                replay::pose::playback::DeleteObjectPuppet(clone);
            clone = 0;
        }

        void LogPuppet(const replay::playback::Puppet& puppet, const char* status, const PuppetState& state,
            std::uint32_t timeMs, const char* reason = "none")
        {
            if (g_play.logs >= 120)
                return;
            ++g_play.logs;
            const TrainCar* info = Ctx().clip.trains.Find(puppet.packet.id.Packed());
            Ctx().log << "POSE_V2_PLAYBACK_PUPPET,id=" << replay::entity::IdText(puppet.packet.id) << ",kind=train_car"
                      << ",model=0x" << std::hex << puppet.packet.modelHash << std::dec << ",status=" << status
                      << ",as=" << (state.kind == PuppetKind::Vehicle ? "vehicle" : "object") << ",reason=" << reason
                      << ",index=" << (info ? info->index : 0) << ",attempts=" << puppet.createAttempts
                      << ",refusals=" << state.judge.refusals << ",drift_worst_m=" << state.judge.worstDriftM
                      << ",playhead_ms=" << timeMs << ",create_ms=" << puppet.packet.timeMs << '\n';
        }

        const replay::entity::RelativeTrack* TrackOf(const replay::playback::Puppet& puppet)
        {
            return Ctx().clip.WeaponTrackOf(puppet.packet.id.Packed());
        }
    }

    void Attach(Context& context) { g_context = &context; }

    bool Check(std::string& error)
    {
        CheckEngine();
        error = g_engine.error;
        return g_engine.links && g_engine.types;
    }

    std::string Describe()
    {
        CheckEngine();
        int prior = -1;
        RandomTrainsOff(prior);
        char text[320]{};
        std::snprintf(text, sizeof(text),
            "TRAINS,links=%d,types=%d,audio=%d,random_flag=%d,random_off=%d,configs=%d,config_count=%d,puppet_mode=%d,world_clear=%d,error=%s",
            g_engine.links ? 1 : 0, g_engine.types ? 1 : 0, g_engine.audio ? 1 : 0, g_engine.random ? 1 : 0, prior,
            g_engine.configs ? 1 : 0,
            [] {
                std::int32_t count = -1;
                if (g_engine.configs)
                    replay::mem::Read(g_engine.configCount, count);
                return static_cast<int>(count);
            }(),
            g_puppetMode, g_worldClearMode, g_engine.error.empty() ? "none" : g_engine.error.c_str());
        return text;
    }

    std::uint32_t PosePlaces() { return kPoseCars; }
    namespace
    {
        constexpr int kScriptDoorsBench = replay::vehdoors::kScriptDoors;
    }

    void SetPuppetMode(int mode) { g_puppetMode = (std::max)(0, (std::min)(2, mode)); }
    void SetWorldClearMode(int mode) { g_worldClearMode = (std::max)(0, (std::min)(1, mode)); }

    bool IsTrainCar(Entity vehicle)
    {
        if (!vehicle || !replay::entities::Exists(vehicle) || !replay::entities::IsVehicle(vehicle))
            return false;
        CheckEngine();
        if (g_engine.types)
            return IsTrainVehicleType(VehicleTypeAt(reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(vehicle))));
        return ModelIsTrain(ENTITY::GET_ENTITY_MODEL(vehicle));
    }

    std::uint64_t AudioEntityOf(const void* car)
    {
        CheckEngine();
        const auto address = reinterpret_cast<std::uint64_t>(car);
        if (!g_engine.audio || !address || !IsTrainVehicleType(VehicleTypeAt(address)))
            return 0;
        std::uint64_t audio = 0;
        if (!replay::mem::Read(static_cast<std::uintptr_t>(address + kAudioEntity), audio) ||
            !replay::mem::InUserRange(static_cast<std::uintptr_t>(audio), 8, replay::mem::kGameHeapLowest) || (audio & 7u) != 0)
            return 0;
        return audio;
    }

    // ----- Recording -----

    void BeginRecording()
    {
        CheckEngine();
        Ctx().recorder.trainCars.clear();
        g_record = RecordStats{};
        g_nextSpeedMs = 0;
        g_lookLogs = 0;
        g_pose = PoseStats{};
        g_nextPoseFillMs = 0;
    }

    void TakeCandidates(std::vector<replay::entity::Candidate>& vehicles)
    {
        REPLAY_PROFILE_ZONE("rec.trains");
        static std::vector<replay::entity::Candidate> cars;
        cars.clear();
        vehicles.erase(std::remove_if(vehicles.begin(), vehicles.end(),
                           [&](const replay::entity::Candidate& candidate) {
                               if (!IsTrainCar(candidate.handle))
                                   return false;
                               cars.push_back(candidate);
                               return true;
                           }),
            vehicles.end());
        const replay::entity::SelectionConfig config;
        const Ped player = PLAYER::PLAYER_PED_ID();
        const Vector3 origin = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
        const Vector3 camera = CAM::GET_FINAL_RENDERED_CAM_COORD();

        // The recorded consists: kept while any car is within the exit
        // radius, else every car leaves; a car gone is retired alone.
        std::vector<std::uint64_t> heads;
        for (const replay::recorder::RecordedTrainCar& car : Ctx().recorder.trainCars)
            if (std::find(heads.begin(), heads.end(), car.head) == heads.end())
                heads.push_back(car.head);
        for (const std::uint64_t head : heads)
        {
            float nearest = 1.0e9f;
            bool alive = false;
            Entity headHandle = 0;
            for (const replay::recorder::RecordedTrainCar& car : Ctx().recorder.trainCars)
            {
                if (car.head != head || !replay::entities::Exists(car.handle))
                    continue;
                alive = true;
                if (!headHandle)
                    headHandle = car.handle;
                nearest = (std::min)(nearest, DistanceTo(car.handle, origin, camera));
            }
            const ConsistAction action = DecideConsist(true, nearest, config.enterRadius, config.exitRadius, alive);
            if (action == ConsistAction::Leave)
            {
                for (std::size_t n = Ctx().recorder.trainCars.size(); n-- > 0;)
                    if (Ctx().recorder.trainCars[n].head == head)
                    {
                        ++g_record.retiredRange;
                        RetireCar(n, alive ? "out_of_range" : "entity_gone");
                    }
                continue;
            }
            // A car coupled on since (the engine relinks): it enters too.
            if (headHandle && g_engine.links)
            {
                Walked walked;
                WalkFrom(headHandle, walked);
                std::size_t added = 0;
                for (std::size_t n = 0; n < walked.handles.size(); ++n)
                    if (!FindRecorded(walked.handles[n]))
                        added += RegisterCar(walked.handles[n], walked.addresses[n], head, walked.fromLinks,
                                     CarDistance(walked.handles[n])) ? 1 : 0;
                if (added)
                {
                    ++g_record.consistChanges;
                    LinkRecorded(walked.handles);
                    Ctx().log << "TRAIN_CONSIST_CHANGE,head=0x" << std::hex << head << std::dec << ",added=" << added
                              << ",time_ms=" << replay::pose::capture::NowMs() << '\n';
                }
            }
        }

        // New consists: the nearest candidates first (the vehicle list is
        // sorted by the pool reader), one walk per consist.
        std::sort(cars.begin(), cars.end(),
            [](const replay::entity::Candidate& a, const replay::entity::Candidate& b) { return a.distance < b.distance; });
        for (const replay::entity::Candidate& candidate : cars)
        {
            if (FindRecorded(candidate.handle))
                continue;
            if (DecideConsist(false, candidate.distance, config.enterRadius, config.exitRadius, true) != ConsistAction::Enter)
                continue;
            Walked walked;
            WalkFrom(candidate.handle, walked);
            EnterConsist(walked, candidate.distance);
        }
    }

    void SampleRecording()
    {
        REPLAY_PROFILE_ZONE("rec.trains.sample");
        if (Ctx().recorder.trainCars.empty())
            return;
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        for (std::size_t index = Ctx().recorder.trainCars.size(); index-- > 0;)
        {
            replay::recorder::RecordedTrainCar& car = Ctx().recorder.trainCars[index];
            if (!replay::entities::Exists(car.handle))
            {
                ++g_record.retiredGone;
                RetireCar(index, "entity_gone");
                continue;
            }
            if (car.sampled && timeMs - car.lastSampleMs < kIntervalMs)
                continue;
            replay::entity::RelativeTrack* track = Ctx().clip.WeaponTrackOf(car.id.Packed());
            if (!track)
                continue;
            replay::entity::TransformSample sample;
            sample.timeMs = timeMs;
            sample.relative = ReadTransform(car.handle);
            sample.visible = ENTITY::IS_ENTITY_VISIBLE(car.handle) != FALSE;
            if (track->Append(sample))
            {
                car.lastSampleMs = timeMs;
                car.sampled = true;
                ++g_record.samples;
            }
        }
        SampleCarState(timeMs);
        // The head cars' speed and direction.
        if (timeMs < g_nextSpeedMs)
            return;
        g_nextSpeedMs = timeMs + kSpeedIntervalMs;
        for (const replay::recorder::RecordedTrainCar& car : Ctx().recorder.trainCars)
        {
            const TrainCar* info = Ctx().clip.trains.Find(car.id.Packed());
            if (!info || info->head != car.id.Packed())
                continue;
            SpeedTrack* speed = Ctx().clip.trains.SpeedOf(info->head);
            if (!speed)
            {
                Ctx().clip.trains.speeds.push_back(SpeedTrack{});
                speed = &Ctx().clip.trains.speeds.back();
                speed->head = info->head;
            }
            if (speed->Append(timeMs, ENTITY::GET_ENTITY_SPEED(car.handle), VEHICLE::_GET_TRAIN_DIRECTION(car.handle) != FALSE))
                ++g_record.speedSamples;
        }
    }

    void EndRecording()
    {
        if (g_pose.started || g_pose.refusedFar || g_pose.refusedFull || g_pose.unresolved || g_pose.startFailed)
            Ctx().log << "TRAIN_POSE_RECORD_SUMMARY,places=" << kPoseCars << ",radius_m=" << static_cast<int>(kPoseRadiusM)
                      << ",started=" << g_pose.started << ",late=" << g_pose.late << ",peak=" << g_pose.peak
                      << ",refused_far=" << g_pose.refusedFar << ",refused_full=" << g_pose.refusedFull
                      << ",unresolved=" << g_pose.unresolved << ",start_failed=" << g_pose.startFailed
                      << ",skeleton_changed=" << g_pose.skeletonChanged << '\n';
        if (g_record.consists || !Ctx().recorder.trainCars.empty())
        {
            Ctx().log << "TRAIN_RECORD_SUMMARY,consists=" << g_record.consists << ",cars=" << g_record.cars
                      << ",cars_max=" << g_record.carsMax << ",walks_memory=" << g_record.walksMemory
                      << ",walks_natives=" << g_record.walksNatives << ",walk_errors=" << g_record.walkErrors
                      << ",last_error=" << WalkStatusText(g_record.lastError) << ",full=" << g_record.full
                      << ",no_handle=" << g_record.noHandle << ",consist_changes=" << g_record.consistChanges
                      << ",samples=" << g_record.samples << ",speed_samples=" << g_record.speedSamples
                      << ",retired_gone=" << g_record.retiredGone << ",retired_range=" << g_record.retiredRange
                      << ",clip_cars=" << Ctx().clip.trains.cars.size() << '\n';
        }
        Ctx().recorder.trainCars.clear();
    }

    void RecordedCars(std::vector<RecordedCarRef>& out)
    {
        out.clear();
        for (const replay::recorder::RecordedTrainCar& car : Ctx().recorder.trainCars)
        {
            if (!replay::entities::Exists(car.handle))
                continue;
            RecordedCarRef ref;
            ref.handle = car.handle;
            ref.id = car.id;
            ref.address = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(car.handle));
            ref.audio = AudioEntityOf(reinterpret_cast<const void*>(ref.address));
            out.push_back(ref);
        }
    }

    replay::entity::ReplayId RecordedIdAt(std::uint64_t address)
    {
        if (!address)
            return {};
        for (const replay::recorder::RecordedTrainCar& car : Ctx().recorder.trainCars)
            if (reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(car.handle)) == address)
                return car.id;
        return {};
    }

    // ----- Playback -----

    void BeginPlayback()
    {
        CheckEngine();
        if (g_playing)
            EndPlayback(); // a new playback without a stop: the world goes back first
        g_puppets.clear();
        g_play = PlayStats{};
        g_hidden.clear();
        g_halted.clear();
        g_deletedAll = false;
        g_worldLogs = 0;
        g_playing = true;
        // No random train comes while the replay plays (the setter is a
        // flag; SET_DISABLE_RANDOM_TRAINS_THIS_FRAME is an empty stub in
        // this build, 0x11CC10 `ret 0`). The user's state goes back at the end.
        g_randomChanged = false;
        RandomTrainsOff(g_randomPrior);
        if (g_randomPrior != 1)
        {
            VEHICLE::SET_RANDOM_TRAINS(FALSE);
            g_randomChanged = true;
        }
        // Every train is cleared before playback only when the ini asks (a
        // mission train goes too).
        if (g_worldClearMode == 1)
            DeleteAllTrains("ini");
        // The cars' pose records, and whether anything moved in them (the
        // wheels): read from the track headers.
        for (const replay::entity::CreatePacket& packet : Ctx().clip.created)
        {
            if (packet.kind != replay::entity::EntityKind::TrainCar || packet.boneCount < kPoseMinBones)
                continue;
            ++g_play.poseRecords;
            replay::tpose::TrackSignature signature;
            const bool read = replay::pose::capture::ReadTrackSignature(packet.id.Packed(), signature);
            if (g_play.poseLogs < 24)
            {
                ++g_play.poseLogs;
                Ctx().log << "TRAIN_CAR_POSE,phase=play,id=" << replay::entity::IdText(packet.id) << ",model=0x" << std::hex
                          << packet.modelHash << std::dec << ",bones=" << packet.boneCount << ",track=" << (read ? 1 : 0)
                          << ",frames=" << signature.frames << ",pose_frames=" << signature.poseFrames
                          << ",never_animated=" << (read && replay::tpose::NeverAnimated(signature) ? 1 : 0) << '\n';
            }
        }
        Ctx().log << "TRAIN_WORLD,action=begin,random_prior=" << g_randomPrior << ",random_changed=" << (g_randomChanged ? 1 : 0)
                  << ",clip_cars=" << Ctx().clip.trains.cars.size() << ",world_clear=" << g_worldClearMode
                  << ",puppet_mode=" << g_puppetMode << '\n';
    }

    void SuppressThisFrame()
    {
        VEHICLE::SET_DISABLE_RANDOM_TRAINS_THIS_FRAME(TRUE);
    }

    bool ClearWorldCar(Entity vehicle)
    {
        if (!g_playing || !IsTrainCar(vehicle))
            return false;
        const auto known = std::find_if(g_hidden.begin(), g_hidden.end(),
            [&](const HiddenCar& car) { return car.state.ped == vehicle; });
        if (known != g_hidden.end())
        {
            if (ENTITY::IS_ENTITY_VISIBLE(vehicle))
            {
                ENTITY::SET_ENTITY_VISIBLE(vehicle, FALSE);
                ++known->rehidden;
                // Shown again and again by the train system: the fallback.
                if (known->rehidden >= 3 && !known->mission)
                    DeleteAllTrains("rehide");
            }
            return true;
        }
        HiddenCar car;
        car.address = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(vehicle));
        car.mission = ENTITY::IS_ENTITY_A_MISSION_ENTITY(vehicle) != FALSE;
        if (!replay::pose::playback::HideWorldPed(vehicle, car.state))
            return true;
        g_hidden.push_back(car);
        // An ambient train does not run on hidden: its head car is halted
        // (a script's train is its script's, only hidden).
        CarLinks links;
        const bool head = !g_engine.links || (ReadLinks(car.address, links) && links.forward == 0);
        if (!car.mission && head)
        {
            HaltedTrain halted;
            halted.head = vehicle;
            halted.speed = ENTITY::GET_ENTITY_SPEED(vehicle);
            halted.haltedAt = GetTickCount64();
            VEHICLE::SET_TRAIN_CRUISE_SPEED(vehicle, 0.0f);
            VEHICLE::SET_TRAIN_SPEED(vehicle, 0.0f);
            g_halted.push_back(halted);
        }
        if (g_worldLogs < 24)
        {
            ++g_worldLogs;
            Ctx().log << "TRAIN_WORLD,action=hide,handle=" << vehicle << ",model=0x" << std::hex
                      << static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(vehicle)) << std::dec << ",mission=" << (car.mission ? 1 : 0)
                      << ",head=" << (head ? 1 : 0) << ",halted=" << (!car.mission && head ? 1 : 0) << '\n';
        }
        return true;
    }

    void UpdatePuppets(std::uint32_t timeMs, std::uint32_t& creates)
    {
        REPLAY_PROFILE_ZONE("play.trains");
        const ULONGLONG now = GetTickCount64();
        // A halted ambient train that still runs 3 s later: the fallback.
        for (HaltedTrain& halted : g_halted)
        {
            if (halted.checked || now < halted.haltedAt + 3000)
                continue;
            halted.checked = true;
            if (replay::entities::Exists(halted.head) && ENTITY::GET_ENTITY_SPEED(halted.head) > 2.0f)
            {
                DeleteAllTrains("halt_failed");
                break;
            }
        }
        std::uint32_t alive = 0;
        std::uint32_t shown = 0;
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind != replay::entity::EntityKind::TrainCar)
                continue;
            ++alive;
            PuppetState& state = g_puppets[puppet.packet.id.Packed()];
            if (puppet.createAttempts == 0 && puppet.state == replay::playback::PuppetState::Loading && g_puppetMode == 2)
                state.kind = PuppetKind::Object;
            const replay::entity::RelativeTrack* track = TrackOf(puppet);
            replay::entity::TransformSample sample;
            const bool sampled = track && track->Sample(timeMs, sample, &Ctx().clip.cuts);
            if (puppet.state == replay::playback::PuppetState::Failed)
                continue;
            if (puppet.clone && !replay::entities::Exists(puppet.clone))
            {
                // Gone (DELETE_ALL_TRAINS, the engine): built again.
                ++g_play.lost;
                Ctx().hooks->ReleaseOverride(puppet); // its pose arm
                puppet.clone = 0;
                puppet.state = replay::playback::PuppetState::Loading;
                puppet.stateSince = now;
                puppet.visibleState = -1;
                LogPuppet(puppet, "lost", state, timeMs);
            }
            if (puppet.state == replay::playback::PuppetState::Loading)
            {
                const TrainCar* info = Ctx().clip.trains.Find(puppet.packet.id.Packed());
                const bool model = replay::pose::playback::RequestModel(puppet.packet.modelHash);
                // The look's sets (the latest), else the entry's.
                const replay::VehicleLook* look = PlayLookOf(puppet.packet.id);
                const Hash cargo = look ? look->propSet : info ? info->propSet : 0;
                const Hash lights = look ? look->lightPropSet : info ? info->lightPropSet : 0;
                bool sets = true;
                if ((info || look) && state.kind == PuppetKind::Vehicle)
                    sets = replay::pose::playback::RequestPropSet(cargo) && replay::pose::playback::RequestPropSet(lights);
                if (!model || (!sets && now - puppet.stateSince <= replay::playback::kModelTimeoutMs))
                {
                    if (!model && now - puppet.stateSince > replay::playback::kModelTimeoutMs)
                    {
                        puppet.state = replay::playback::PuppetState::Failed;
                        ++g_play.failed;
                        LogPuppet(puppet, "model_timeout", state, timeMs);
                    }
                    continue;
                }
                if (!sampled)
                {
                    if (now - puppet.stateSince > replay::playback::kModelTimeoutMs)
                    {
                        puppet.state = replay::playback::PuppetState::Failed;
                        ++g_play.failed;
                        LogPuppet(puppet, "no_track", state, timeMs);
                    }
                    continue;
                }
                if (creates >= replay::playback::kMaxCreatesPerFrame || now < puppet.nextCreateAt ||
                    !replay::playback::StartBatchLeft()) // ClipStartStagger
                    continue;
                ++creates;
                replay::playback::StartBatchUse();
                ++puppet.createAttempts;
                puppet.clone = CreateCarPuppet(puppet, state.kind, sample.relative);
                puppet.placed = false;
                puppet.visibleState = -1;
                if (!puppet.clone)
                {
                    ++g_play.refused;
                    puppet.nextCreateAt = now + replay::playback::kCreateRetryMs;
                    if (state.kind == PuppetKind::Vehicle && g_puppetMode == 0 && state.judge.Refused())
                    {
                        state.kind = PuppetKind::Object;
                        ++g_play.fallbackObject;
                        LogPuppet(puppet, "fallback_object", state, timeMs, "create_refused");
                    }
                    else if (now - puppet.stateSince > 2 * replay::playback::kModelTimeoutMs)
                    {
                        puppet.state = replay::playback::PuppetState::Failed;
                        ++g_play.failed;
                        LogPuppet(puppet, "create_failed", state, timeMs);
                    }
                    continue;
                }
                if (state.kind == PuppetKind::Vehicle && (info || look))
                {
                    replay::pose::playback::ApplyVehicleLook(puppet.clone, cargo, lights, look ? look->extrasExist : 0u,
                        look ? look->extrasOn : 0u);
                    if (look)
                    {
                        if (!state.lookCheckAt)
                            ++g_play.looks;
                        state.lookCheckAt = now + 500;
                        state.lookChecks = 0;
                        state.lookJudged = false;
                    }
                }
                ++g_play.created;
                ++(state.kind == PuppetKind::Vehicle ? g_play.asVehicle : g_play.asObject);
                state.placedLastTick = false;
                puppet.state = replay::playback::PuppetState::Ready;
                LogPuppet(puppet, "created", state, timeMs);
            }
            if (puppet.state != replay::playback::PuppetState::Ready || !puppet.clone)
                continue;
            // Where the engine had the puppet since our placement: a train
            // vehicle the train system moves (onto a track) becomes an object.
            if (state.placedLastTick && state.kind == PuppetKind::Vehicle)
            {
                float at[3]{};
                if (replay::entities::Position(puppet.clone, at))
                {
                    const float dx = at[0] - puppet.placedWorld.position.x;
                    const float dy = at[1] - puppet.placedWorld.position.y;
                    const float dz = at[2] - puppet.placedWorld.position.z;
                    const float drift = std::sqrt(dx * dx + dy * dy + dz * dz);
                    g_play.driftWorst = (std::max)(g_play.driftWorst, drift);
                    if (state.judge.Drift(drift) && g_puppetMode == 0)
                    {
                        ++g_play.fallbackObject;
                        LogPuppet(puppet, "fallback_object", state, timeMs, "drift");
                        Ctx().hooks->ReleaseOverride(puppet); // its pose arm
                        DeleteCarPuppet(puppet.clone, PuppetKind::Vehicle);
                        state.kind = PuppetKind::Object;
                        state.placedLastTick = false;
                        puppet.state = replay::playback::PuppetState::Loading;
                        puppet.stateSince = now;
                        puppet.nextCreateAt = now;
                        puppet.visibleState = -1;
                        continue;
                    }
                }
            }
            std::uint32_t firstMs = 0;
            std::uint32_t lastMs = 0;
            const bool inTrack = track && track->Span(firstMs, lastMs) && timeMs >= firstMs && timeMs <= lastMs + kIntervalMs;
            const int wantVisible = sampled && inTrack && sample.visible ? 1 : 0;
            if (puppet.visibleState != wantVisible)
            {
                replay::pose::playback::SetCloneVisible(puppet.clone, wantVisible != 0);
                puppet.visibleState = wantVisible;
            }
            shown += wantVisible ? 1u : 0u;
            if (!sampled)
            {
                state.placedLastTick = false;
                continue;
            }
            puppet.lastWorld = sample.relative;
            puppet.hasWorld = true;
            replay::pose::playback::PlaceClone(puppet.clone, sample.relative);
            puppet.placedWorld = sample.relative;
            puppet.placed = true;
            state.placedLastTick = true;
            // The doors, the pose (wheels and rods) and the look of a vehicle
            // puppet.
            if (state.kind == PuppetKind::Vehicle)
            {
                if (const replay::vehdoors::DoorTrack* doors =
                        replay::vehdoors::TrackOf(Ctx().clip.vehicleDoors, puppet.packet.id.Packed()))
                    replay::vehdoors::Apply(puppet.clone, *doors, timeMs, Ctx().log);
                PlayCarPose(puppet, state, timeMs);
                CheckCarLook(puppet, state, timeMs);
            }
        }
        g_play.aliveMax = (std::max)(g_play.aliveMax, alive);
        g_play.shownMax = (std::max)(g_play.shownMax, shown);
        if (alive && now >= g_play.censusAt && g_play.censusLogs < 20)
        {
            g_play.censusAt = now + 3000;
            ++g_play.censusLogs;
            Ctx().log << "TRAIN_PLAY_CENSUS,alive=" << alive << ",shown=" << shown << ",as_vehicle=" << g_play.asVehicle
                      << ",as_object=" << g_play.asObject << ",world_hidden=" << g_hidden.size() << ",halted=" << g_halted.size()
                      << ",playhead_ms=" << timeMs << '\n';
        }
    }

    void DeletePuppet(playback::Puppet& puppet, const char* reason)
    {
        const auto found = g_puppets.find(puppet.packet.id.Packed());
        const PuppetKind kind = found != g_puppets.end() ? found->second.kind : PuppetKind::Vehicle;
        if (found != g_puppets.end())
            g_play.driftWorst = (std::max)(g_play.driftWorst, found->second.judge.worstDriftM);
        (void)reason;
        Entity clone = puppet.clone;
        // An object puppet is deleted as an object, a vehicle as a vehicle;
        // what the engine left of either kind goes by the other path.
        DeleteCarPuppet(clone, kind);
        if (puppet.clone && replay::entities::Exists(puppet.clone))
        {
            Entity other = puppet.clone;
            DeleteCarPuppet(other, kind == PuppetKind::Vehicle ? PuppetKind::Object : PuppetKind::Vehicle);
        }
        puppet.clone = 0;
    }

    void EndPlayback()
    {
        if (!g_playing)
            return;
        g_playing = false;
        std::uint32_t restored = 0;
        for (const HiddenCar& car : g_hidden)
        {
            replay::pose::playback::RestoreWorldPed(car.state);
            ++restored;
        }
        std::uint32_t restarted = 0;
        for (const HaltedTrain& halted : g_halted)
        {
            if (!replay::entities::Exists(halted.head))
                continue;
            VEHICLE::SET_TRAIN_CRUISE_SPEED(halted.head, halted.speed);
            VEHICLE::SET_TRAIN_SPEED(halted.head, halted.speed);
            ++restarted;
        }
        if (g_randomChanged)
            VEHICLE::SET_RANDOM_TRAINS(TRUE);
        std::uint32_t clipCars = 0;
        for (const replay::entity::CreatePacket& packet : Ctx().clip.created)
            clipCars += packet.kind == replay::entity::EntityKind::TrainCar ? 1u : 0u;
        std::uint32_t consists = 0;
        for (const TrainCar& car : Ctx().clip.trains.cars)
            consists += car.flags & kCarHead ? 1u : 0u;
        if (clipCars || restored || g_deletedAll)
        {
            const replay::logfmt::FormatScope logFormat(Ctx().log); // precision not left behind
            Ctx().log << std::fixed << std::setprecision(2) << "TRAIN_SUMMARY,clip_cars=" << clipCars << ",consists=" << consists
                      << ",puppets_alive_max=" << g_play.aliveMax << ",puppets_shown_max=" << g_play.shownMax
                      << ",created=" << g_play.created << ",as_vehicle=" << g_play.asVehicle << ",as_object=" << g_play.asObject
                      << ",refused=" << g_play.refused << ",fallback_object=" << g_play.fallbackObject << ",failed=" << g_play.failed
                      << ",lost=" << g_play.lost << ",drift_worst_m=" << g_play.driftWorst << ",world_hidden=" << restored
                      << ",world_halted=" << restarted << ",delete_all=" << (g_deletedAll ? 1 : 0)
                      << ",random_prior=" << g_randomPrior << ",random_restored=" << (g_randomChanged ? 1 : 0)
                      << ",looks=" << g_play.looks << ",look_ok=" << g_play.lookOk << ",look_mismatch=" << g_play.lookMismatch
                      << ",look_reapplied=" << g_play.lookReapplied << ",pose_records=" << g_play.poseRecords
                      << ",pose_met=" << g_play.poseMet << ",pose_armed=" << g_play.poseArmed << ",pose_still=" << g_play.poseStill
                      << ",pose_unarmed=" << (g_play.poseMet - (std::min)(g_play.poseMet, g_play.poseArmed + g_play.poseStill)) << '\n';
            Ctx().log.unsetf(std::ios_base::floatfield);
        }
        g_hidden.clear();
        g_halted.clear();
        g_puppets.clear();
        g_randomChanged = false;
    }

    // See the header.
    std::uint32_t ForgetPlaybackAfterRestart()
    {
        const std::uint32_t forgotten = static_cast<std::uint32_t>(g_hidden.size() + g_halted.size() + g_puppets.size());
        if (g_randomChanged)
            VEHICLE::SET_RANDOM_TRAINS(TRUE); // global, no handle (what EndPlayback gives back)
        g_hidden.clear();
        g_halted.clear();
        g_puppets.clear();
        g_randomChanged = false;
        g_playing = false;
        return forgotten;
    }

    bool RecordedWorldAt(replay::entity::ReplayId id, std::uint32_t timeMs, float position[3])
    {
        const replay::entity::RelativeTrack* track = Ctx().clip.WeaponTrackOf(id.Packed());
        replay::entity::TransformSample sample;
        if (!track || !track->Sample(timeMs, sample, &Ctx().clip.cuts))
            return false;
        position[0] = sample.relative.position.x;
        position[1] = sample.relative.position.y;
        position[2] = sample.relative.position.z;
        return true;
    }

    // ----- Bench -----

    namespace
    {
        // The configs the engine loaded (train_spawn auto), from its table.
        void ListConfigs(std::vector<Hash>& out)
        {
            out.clear();
            CheckEngine();
            std::int32_t count = 0;
            if (!g_engine.configs || !replay::mem::Read(g_engine.configCount, count) || count <= 0 || count > 256)
                return;
            for (std::int32_t n = 0; n < count; ++n)
            {
                std::uint32_t hash = 0;
                if (replay::mem::Read(g_engine.configBase + static_cast<std::uintptr_t>(n) * kConfigStride, hash) && hash)
                    out.push_back(static_cast<Hash>(hash));
            }
        }

        Hash g_spawnConfig = 0;
        bool g_configsLogged = false;
    }

    int BenchSpawn(const std::string& config, float x, float y, float z, bool direction, std::uint64_t startedAt,
        std::uint64_t now, Vehicle& train, std::string& detail)
    {
        if (now == startedAt || !g_spawnConfig)
        {
            g_spawnConfig = 0;
            if (config == "auto")
            {
                std::vector<Hash> configs;
                ListConfigs(configs);
                for (const Hash hash : configs)
                {
                    const int cars = VEHICLE::_GET_NUM_CARS_FROM_TRAIN_CONFIG(hash);
                    const Hash first = cars > 0 ? VEHICLE::_GET_TRAIN_MODEL_FROM_TRAIN_CONFIG_BY_CAR_INDEX(hash, 0) : 0;
                    if (!g_configsLogged)
                        Ctx().log << "BENCH_TRAIN_CONFIG,hash=0x" << std::hex << hash << ",model0=0x" << first << std::dec
                                  << ",cars=" << cars << '\n';
                    if (!g_spawnConfig && cars >= 3 && cars <= 12 && first && VEHICLE::IS_THIS_MODEL_A_TRAIN(first))
                        g_spawnConfig = hash;
                }
                g_configsLogged = true;
                if (!g_spawnConfig)
                {
                    detail = configs.empty() ? "no_config_table" : "no_config_with_3_to_12_cars";
                    return -1;
                }
            }
            else
            {
                char* end = nullptr;
                g_spawnConfig = static_cast<Hash>(std::strtoul(config.c_str(), &end, 0));
                if (!g_spawnConfig || (end && *end))
                    g_spawnConfig = MISC::GET_HASH_KEY(config.c_str());
            }
        }
        const int cars = VEHICLE::_GET_NUM_CARS_FROM_TRAIN_CONFIG(g_spawnConfig);
        if (cars <= 0)
        {
            detail = "unknown_config";
            return -1;
        }
        bool loaded = true;
        for (int n = 0; n < cars; ++n)
        {
            const Hash model = VEHICLE::_GET_TRAIN_MODEL_FROM_TRAIN_CONFIG_BY_CAR_INDEX(g_spawnConfig, n);
            if (!model || !STREAMING::IS_MODEL_VALID(model))
                continue;
            if (!STREAMING::HAS_MODEL_LOADED(model))
            {
                STREAMING::REQUEST_MODEL(model, FALSE);
                loaded = false;
            }
        }
        if (!loaded)
        {
            if (now - startedAt > 10000)
            {
                detail = "model_timeout";
                return -1;
            }
            return 0;
        }
        const Vector3 track = VEHICLE::_GET_NEAREST_TRAIN_TRACK_POSITION(x, y, z);
        // As the RedM scripts do: passengers off, the two flags after on.
        train = VEHICLE::_CREATE_MISSION_TRAIN(g_spawnConfig, track.x, track.y, track.z, direction ? TRUE : FALSE, FALSE, TRUE, TRUE);
        if (!train || !ENTITY::DOES_ENTITY_EXIST(train))
        {
            detail = "create_refused";
            return -1;
        }
        // The player 20 m off the track point, towards the asked point,
        // facing the track.
        float dx = x - track.x;
        float dy = y - track.y;
        float length = std::sqrt(dx * dx + dy * dy);
        if (length < 1.0f)
        {
            dx = 1.0f;
            dy = 0.0f;
            length = 1.0f;
        }
        dx /= length;
        dy /= length;
        const float px = track.x + dx * 20.0f;
        const float py = track.y + dy * 20.0f;
        float ground = track.z;
        MISC::GET_GROUND_Z_FOR_3D_COORD(px, py, track.z + 30.0f, &ground, FALSE);
        const Ped player = PLAYER::PLAYER_PED_ID();
        if (PED::IS_PED_ON_MOUNT(player))
            PED::_REMOVE_PED_FROM_MOUNT(player, TRUE, TRUE);
        ENTITY::SET_ENTITY_COORDS(player, px, py, ground, FALSE, FALSE, FALSE, FALSE);
        ENTITY::SET_ENTITY_HEADING(player, MISC::GET_HEADING_FROM_VECTOR_2D(-dx, -dy));
        char text[200]{};
        std::snprintf(text, sizeof(text), "config=0x%X,cars=%d,count=%d,track=%.1f;%.1f;%.1f,player=%.1f;%.1f;%.1f",
            static_cast<unsigned>(g_spawnConfig), cars, VEHICLE::_GET_TRAIN_CARRIAGE_TRAILER_NUMBER(train), track.x, track.y,
            track.z, px, py, ground);
        detail = text;
        Ctx().log << "BENCH_TRAIN_SPAWN,handle=" << train << "," << text << '\n';
        g_spawnConfig = 0;
        return 1;
    }

    bool BenchSpeed(Vehicle train, float speed, std::string& detail)
    {
        if (!train || !ENTITY::DOES_ENTITY_EXIST(train))
        {
            detail = "no_train";
            return false;
        }
        VEHICLE::SET_TRAIN_CRUISE_SPEED(train, speed);
        VEHICLE::SET_TRAIN_SPEED(train, speed);
        char text[64]{};
        std::snprintf(text, sizeof(text), "speed=%.1f,was=%.1f", speed, ENTITY::GET_ENTITY_SPEED(train));
        detail = text;
        return true;
    }

    void BenchCensus(Vehicle train, const std::string& id, std::string& detail)
    {
        if (!train || !ENTITY::DOES_ENTITY_EXIST(train))
        {
            detail = "no_train";
            return;
        }
        CheckEngine();
        const int count = VEHICLE::_GET_TRAIN_CARRIAGE_TRAILER_NUMBER(train);
        Walked walked;
        WalkFrom(train, walked);
        std::uint32_t recorded = 0;
        for (int n = 0; n < count && n < static_cast<int>(kMaxConsistCars); ++n)
        {
            const Vehicle car = VEHICLE::GET_TRAIN_CARRIAGE(train, n);
            if (!car)
                continue;
            float at[3]{};
            replay::entities::Position(car, at);
            const std::uint64_t address = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(car));
            const replay::recorder::RecordedTrainCar* rec = FindRecorded(car);
            recorded += rec ? 1u : 0u;
            const auto inWalk = std::find(walked.handles.begin(), walked.handles.end(), car);
            const replay::logfmt::FormatScope logFormat(Ctx().log); // precision not left behind
            Ctx().log << std::fixed << std::setprecision(2) << "BENCH_TRAIN_CENSUS,id=" << id << ",car=" << n << ",handle=" << car
                      << ",model=0x" << std::hex << static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(car)) << std::dec
                      << ",type=" << VehicleTypeAt(address) << ",train_model=" << (ModelIsTrain(ENTITY::GET_ENTITY_MODEL(car)) ? 1 : 0)
                      << ",pos=" << at[0] << ";" << at[1] << ";" << at[2] << ",speed=" << ENTITY::GET_ENTITY_SPEED(car)
                      << ",mission=" << (ENTITY::IS_ENTITY_A_MISSION_ENTITY(car) ? 1 : 0)
                      << ",walk_index=" << (inWalk == walked.handles.end() ? -1 : static_cast<int>(inWalk - walked.handles.begin()))
                      << ",recorded=" << (rec ? replay::entity::IdText(rec->id) : std::string("none"))
                      << ",audio=" << (AudioEntityOf(reinterpret_cast<const void*>(address)) ? 1 : 0);
            // What a pose record would cost and give (the locomotive's wheels
            // and rods): the skeleton the resolver finds on this car.
            replay::skeleton::ResolvedPoseSkeleton skeleton{};
            const bool resolved = replay::skeleton::FastResolvePedPoseSkeleton(car, skeleton);
            Ctx().log << ",skeleton=" << (resolved ? 1 : 0) << ",bones=" << (resolved ? skeleton.boneCount : 0)
                      << ",fragment_path=" << (resolved && skeleton.usedFragmentPath ? 1 : 0) << '\n';
            Ctx().log.unsetf(std::ios_base::floatfield);
        }
        char text[160]{};
        std::snprintf(text, sizeof(text), "cars=%d,walk=%u,links=%s,walk_status=%s,recorded=%u", count,
            static_cast<unsigned>(walked.handles.size()), walked.fromLinks ? "memory" : "natives",
            WalkStatusText(walked.status), recorded);
        detail = text;
    }

    bool BenchDoor(Vehicle train, int car, const std::string& door, const std::string& action, std::string& detail)
    {
        if (!train || !ENTITY::DOES_ENTITY_EXIST(train))
        {
            detail = "no_train";
            return false;
        }
        const Vehicle target = car == 0 ? train : VEHICLE::GET_TRAIN_CARRIAGE(train, car);
        if (!target || !ENTITY::DOES_ENTITY_EXIST(target))
        {
            detail = "no_car";
            return false;
        }
        const int first = door == "all" ? 0 : std::atoi(door.c_str());
        const int last = door == "all" ? kScriptDoorsBench - 1 : first;
        std::string text = "car=" + std::to_string(car) + ",handle=" + std::to_string(target) + ",model=0x";
        char model[16]{};
        std::snprintf(model, sizeof(model), "%X", static_cast<unsigned>(ENTITY::GET_ENTITY_MODEL(target)));
        text += model;
        int present = 0;
        for (int n = first; n <= last; ++n)
        {
            float before = 0.0f;
            if (!replay::vehdoors::ReadDoorRatio(target, n, before))
                continue;
            ++present;
            if (action == "open")
                VEHICLE::SET_VEHICLE_DOOR_OPEN(target, n, FALSE, TRUE);
            else if (action == "shut")
                VEHICLE::SET_VEHICLE_DOOR_SHUT(target, n, TRUE);
            else
                VEHICLE::SET_VEHICLE_DOOR_CONTROL(target, n, 0, static_cast<float>(std::atof(action.c_str())));
            float after = 0.0f;
            replay::vehdoors::ReadDoorRatio(target, n, after);
            char row[64]{};
            std::snprintf(row, sizeof(row), ",d%d=%.3f>%.3f", n, before, after);
            text += row;
        }
        text += ",doors=" + std::to_string(present);
        detail = text;
        Ctx().log << "BENCH_TRAIN_DOOR," << text << ",action=" << action << '\n';
        return present > 0;
    }

    void BenchRandomTrains(bool off, std::string& detail)
    {
        CheckEngine();
        int before = -1;
        RandomTrainsOff(before);
        VEHICLE::SET_RANDOM_TRAINS(off ? FALSE : TRUE);
        int after = -1;
        RandomTrainsOff(after);
        char text[64]{};
        std::snprintf(text, sizeof(text), "random_off_before=%d,after=%d", before, after);
        detail = text;
    }

    void BenchDelete(Vehicle& train)
    {
        if (!train || !ENTITY::DOES_ENTITY_EXIST(train))
        {
            train = 0;
            return;
        }
        // DELETE_MISSION_TRAIN takes types 18 and 19 of its script
        // (0x10E406C); anything left goes with every train (bench only).
        Vehicle handle = train;
        VEHICLE::DELETE_MISSION_TRAIN(&handle);
        if (ENTITY::DOES_ENTITY_EXIST(train))
        {
            VEHICLE::DELETE_ALL_TRAINS();
            Ctx().log << "BENCH_TRAIN_DELETE,handle=" << train << ",how=delete_all_trains\n";
        }
        train = 0;
    }
}
