// replay_capture_sampling.cpp: the pose capture's start, its per-tick sampling (appearance, scorch,
// vehicle windows, look, parts, doors, fire char, fidelity), its update and its stop with the record stats.
// Split from replay_capture.cpp (moved code); shared internals in replay_capture_internal.h.

#include "replay_capture_internal.h"
#include "pose_capture_rules.h" // kDefaultStallPollMs, kWorkerPollMs

namespace replay::recorder
{

    namespace
    {

    std::uint32_t g_poseV2ScorchLogs = 0; // POSE_V2_SCORCHED record lines
    std::uint32_t g_poseV2WindowLogs = 0; // POSE_V2_VEHICLE_WINDOWS record lines
    std::uint32_t g_poseV2LookLogs = 0;   // POSE_V2_VEHICLE_LOOK change lines
    std::uint32_t g_poseV2PartLogs = 0;   // POSE_V2_VEHICLE_PARTS record lines
    std::uint32_t g_poseV2BurntLogs = 0;  // POSE_V2_VEHICLE_BURNT record lines
    std::uint32_t g_poseV2FireCharLogs = 0;       // POSE_V2_FIRE_CHAR record lines
    std::uint32_t g_poseV2FireCharNextMs = 0;     // next fire record sample
    std::uint32_t g_poseV2LateLogs = 0;   // POSE_V2_LATE_POSE record lines

    // An entity's track through an index kept per track vector (track_index.h),
    // in place of a find_if over every entity the recording has seen; end()
    // when it has none yet.
    template <class Track>
    typename std::vector<Track>::iterator FindPoseV2Track(
        replay::TrackIndex<Track>& index, std::vector<Track>& tracks, std::uint32_t id)
    {
        Track* found = index.Find(tracks, id);
        return found ? tracks.begin() + (found - tracks.data()) : tracks.end();
    }

    // The appearance change probes (UpdatePoseV2Appearance below).
    struct PoseV2AppearanceProbe
    {
        std::uint32_t id = 0;
        std::uint32_t lastCheckMs = 0;
        std::uint32_t lastFullMs = 0;
        int lastCount = -1; // -1: never checked
        std::uint64_t pendingPrint = 0;
        std::uint32_t pendingSinceMs = 0;
        bool pending = false;
    };
    struct PoseV2AppearanceStats
    {
        std::uint64_t ticks = 0;
        std::uint64_t visits = 0;
        std::uint64_t fullReads = 0;
        std::uint64_t empty = 0;       // no layers (the metaped rebuilding)
        std::uint64_t unconfirmed = 0; // a pending print that did not hold
        std::uint64_t changes = 0;
        std::uint64_t entitiesChanged = 0;
        double totalUs = 0.0;
        double maxUs = 0.0;
    };
    std::vector<PoseV2AppearanceProbe> g_poseV2AppearanceProbes;
    PoseV2AppearanceStats g_poseV2AppearanceStats;
    std::size_t g_poseV2AppearanceCursor = 0;
    std::uint32_t g_poseV2AppearanceLogs = 0; // POSE_V2_APPEARANCE_CHANGE record lines
    }

    void StartPoseV2Capture()
    {
        replay::objects::EnsureStrapVisibility();
        replay::events::BeginRecording();
        replay::pose::capture::BeginRecording();
        replay::pose::fidelity::Reset();
        Ctx().clip.Begin();
        {
            static int handles[replay::recorder::kPoolCapacity]{};
            // Not a handle for every object of a town when the
            // handle pool cannot spare them (replay::objects::ListAllObjectsGuarded).
            const char* objectsMode = "listing";
            const int objects = (std::min)((std::max)(0, replay::objects::ListAllObjectsGuarded(handles,
                replay::recorder::kPoolCapacity, objectsMode)), replay::recorder::kPoolCapacity);
            Ctx().clip.objectsBefore.assign(handles, handles + objects);
            if (std::strcmp(objectsMode, "all") != 0)
                Ctx().log << "RECORDING_OBJECTS_BEFORE,mode=" << objectsMode << ",objects=" << objects
                          << ",guid_room=" << replay::pools::GuidRoom() << '\n';
            const int pickups = (std::min)((std::max)(0, worldGetAllPickups(handles, replay::recorder::kPoolCapacity)),
                replay::recorder::kPoolCapacity);
            Ctx().clip.pickupsBefore.assign(handles, handles + pickups);
            for (int n = 0; n < pickups; ++n)
                if (const Object object = OBJECT::GET_PICKUP_OBJECT(handles[n]))
                    Ctx().clip.objectsBefore.push_back(object);
            std::sort(Ctx().clip.objectsBefore.begin(), Ctx().clip.objectsBefore.end());
            std::sort(Ctx().clip.pickupsBefore.begin(), Ctx().clip.pickupsBefore.end());
        }
        Ctx().recorder.entities.clear();
        Ctx().recorder.weapons.clear();
        Ctx().recorder.props.clear();
        Ctx().recorder.drops.clear();
        replay::objects::BeginRecording();
        replay::trains::BeginRecording();
        replay::vehdoors::BeginRecording();
        Ctx().recorder.rejected.clear();
        g_poseV2NextScanAt = 0;
        g_poseV2ScorchLogs = 0;
        g_poseV2AppearanceProbes.clear();
        g_poseV2MetapedProbes.clear();
        g_poseV2MetapedStats = {};
        g_poseV2MetapedCursor = 0;
        g_poseV2MetapedLogs = 0;
        Ctx().log << replay::metaped::engine::ReaderStatus() << '\n';
        g_poseV2AppearanceStats = {};
        g_poseV2AppearanceCursor = 0;
        g_poseV2AppearanceLogs = 0;
        g_poseV2WindowLogs = 0;
        g_poseV2LookLogs = 0;
        g_poseV2BurntLogs = 0;
        g_poseV2FireCharLogs = 0;
        g_poseV2FireCharNextMs = 0;
        g_poseV2PartLogs = 0;
        g_poseV2LateLogs = 0;
        g_poseV2ScriptTiming = {};
        g_poseV2Selection = {};
        g_poseV2Retired.Clear();
        g_poseV2Discovery = {};
        g_unresolvedTries.clear();
        g_unresolvedRetryLogs = 0;
        Ctx().recorder.recording = true;
    }

    namespace
    {

    // Existence is checked every frame (one native per entity), and so is
    // the skeleton (CheckPoseV2Skeletons from UpdatePoseV2Capture): the
    // resolver walks a few pointers under SEH, and the capture detour already
    // rejects a bone-count change.
    // The entity keeps its ReplayId; its pose continues in a new segment
    // with the new skeleton and the appearance it has now.
    bool StartPoseV2Segment(replay::recorder::RecordedEntity& entity, const replay::skeleton::ResolvedPoseSkeleton& resolved)
    {
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        const std::uint32_t previousRecord = entity.recordId;
        const std::uint32_t previousBones = entity.boneCount;
        replay::pose::capture::Stop(previousRecord);
        replay::entity::PoseSegment segment;
        segment.entity = entity.id;
        segment.startMs = timeMs;
        segment.recordId = Ctx().clip.segments.NextRecordId();
        segment.boneCount = resolved.boneCount;
        std::string error;
        const bool started = replay::pose::capture::Start(
            segment.recordId, resolved.skeleton, resolved.boneCount, replay::recorder::kSampleHz, error);
        if (started)
        {
            if (!entity.vehicle)
            {
                replay::entity::appearance::Capture(entity.handle, segment.appearance);
                RecordPoseV2Metaped(entity.handle, entity.id, replay::metaped::Source::Segment, timeMs,
                    segment.appearance);
            }
            Ctx().clip.segments.Add(segment);
            entity.recordId = segment.recordId;
            entity.boneCount = resolved.boneCount;
            entity.skeleton = resolved.skeleton;
        }
        std::ostream& log = Ctx().log;
        log << "POSE_V2_POSE_SEGMENT"
            << ",id=" << replay::entity::IdText(entity.id)
            << ",status=" << (started ? "started" : "failed")
            << ",error=" << (started ? "none" : error.c_str())
            << ",record=0x" << std::hex << segment.recordId
            << ",previous_record=0x" << previousRecord << std::dec
            << ",bones=" << resolved.boneCount
            << ",previous_bones=" << previousBones
            << ",layers=" << segment.appearance.assets.size()
            << ",time_ms=" << timeMs
            << '\n';
        log.flush();
        return started;
    }

    // Every frame: a skeleton that changed (new object or new bone count)
    // starts a new pose segment, so no frame is lost to a mismatch.
    void CheckPoseV2Skeletons()
    {
        for (std::size_t index = Ctx().recorder.entities.size(); index-- > 0;)
        {
            replay::recorder::RecordedEntity& entity = Ctx().recorder.entities[index];
            replay::skeleton::ResolvedPoseSkeleton resolved{};
            ++g_poseV2ScriptTiming.resolves;
            if (!replay::skeleton::FastResolvePedPoseSkeleton(entity.handle, resolved))
            {
                RetirePoseV2Entity(index, "skeleton_unresolved");
                continue;
            }
            if (resolved.skeleton == entity.skeleton && resolved.boneCount == entity.boneCount)
                continue;
            if (!StartPoseV2Segment(entity, resolved))
                RetirePoseV2Entity(index, "segment_failed");
        }
    }

    // The charred render bit of every recorded ped and horse
    // (_0x37B01666BAE8F7EF reads entity +0x1B0 bit 4, the bit
    // SET_ENTITY_RENDER_SCORCHED writes), twice a second, stored when it
    // changes. The first read of an entity already charred stores it at
    // that time (the state at the recording start or at its entry).
    // Vehicles too; the engine sets the same bit on a vehicle (the fire
    // shutdown of an entity fire, vehicle explosions).
    void SamplePoseV2Scorched()
    {
        REPLAY_PROFILE_ZONE("rec.scorched");
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (!replay::entities::Exists(entity.handle))
                continue;
            static replay::TrackIndex<replay::ScorchTrack> index;
            auto track = FindPoseV2Track(index, Ctx().clip.scorched, entity.id.Packed());
            if (track != Ctx().clip.scorched.end() && track->checked && timeMs - track->lastCheckMs < 500)
                continue;
            const bool on = (static_cast<std::uint32_t>(ENTITY::_0x37B01666BAE8F7EF(entity.handle)) & 1u) != 0;
            if (track == Ctx().clip.scorched.end())
            {
                Ctx().clip.scorched.push_back({entity.id.Packed(), {}, 0, false});
                track = Ctx().clip.scorched.end() - 1;
            }
            track->lastCheckMs = timeMs;
            track->checked = true;
            const bool previous = track->states.empty() ? false : track->states.back().second;
            if (on == previous)
                continue;
            track->states.push_back({timeMs, on});
            if (g_poseV2ScorchLogs < 64)
            {
                ++g_poseV2ScorchLogs;
                Ctx().log << "POSE_V2_SCORCHED,phase=record,id=" << replay::entity::IdText(entity.id)
                    << ",scorched=" << (on ? 1 : 0) << ",time_ms=" << timeMs << '\n';
            }
        }
    }

    // The broken windows of every recorded vehicle (IS_VEHICLE_WINDOW_INTACT,
    // windows 0..3), twice a second, stored when the mask changes. Vehicles
    // only: four natives per vehicle and sample.
    void SamplePoseV2VehicleWindows()
    {
        REPLAY_PROFILE_ZONE("rec.vehicle_windows");
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (!entity.vehicle || !replay::entities::Exists(entity.handle))
                continue;
            static replay::TrackIndex<replay::WindowTrack> index;
            auto track = FindPoseV2Track(index, Ctx().clip.vehicleWindows, entity.id.Packed());
            if (track != Ctx().clip.vehicleWindows.end() && track->checked && timeMs - track->lastCheckMs < 500)
                continue;
            std::uint8_t mask = 0;
            for (int window = 0; window < replay::kVehicleWindows; ++window)
            {
                if (!VEHICLE::IS_VEHICLE_WINDOW_INTACT(entity.handle, window))
                    mask |= static_cast<std::uint8_t>(1u << window);
            }
            if (track == Ctx().clip.vehicleWindows.end())
            {
                Ctx().clip.vehicleWindows.push_back({entity.id.Packed(), {}, 0, false});
                track = Ctx().clip.vehicleWindows.end() - 1;
            }
            track->lastCheckMs = timeMs;
            track->checked = true;
            // The first read is always stored: it is the vehicle's state from
            // its entry (a model without glass at an index reads broken).
            if (!track->states.empty() && mask == track->states.back().second)
                continue;
            track->states.push_back({timeMs, mask});
            if (g_poseV2WindowLogs < 64)
            {
                ++g_poseV2WindowLogs;
                Ctx().log << "POSE_V2_VEHICLE_WINDOWS,phase=record,id=" << replay::entity::IdText(entity.id)
                    << ",broken=0x" << std::hex << static_cast<unsigned>(mask) << std::dec << ",time_ms=" << timeMs << '\n';
            }
        }
    }

    // The look of every recorded vehicle read again once a second, kept as its
    // latest: a vehicle whose paint, livery, cargo, lantern or extras the game
    // set after it entered the recording (an ambient wagon dressed a frame
    // later, a stable repaint) is rebuilt as it was last seen. Logged on
    // change.
    void SamplePoseV2VehicleLook()
    {
        REPLAY_PROFILE_ZONE("rec.vehicle_look");
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (!entity.vehicle || !replay::entities::Exists(entity.handle))
                continue;
            auto look = std::find_if(Ctx().clip.vehicleLooks.begin(), Ctx().clip.vehicleLooks.end(),
                [&](const replay::VehicleLook& entry) { return entry.id == entity.id; });
            if (look == Ctx().clip.vehicleLooks.end() || timeMs - look->lastCheckMs < 1000)
                continue;
            replay::VehicleLook now = *look;
            Hash byHash = 0;
            Hash byHandle = 0;
            ReadVehicleLook(entity.handle, now, byHash, byHandle);
            look->lastCheckMs = timeMs;
            if (now.tint == look->tint && now.livery == look->livery && now.propSet == look->propSet &&
                now.lightPropSet == look->lightPropSet && now.extrasExist == look->extrasExist &&
                now.extrasOn == look->extrasOn)
            {
                continue;
            }
            now.lastCheckMs = timeMs;
            *look = now;
            if (g_poseV2LookLogs < 64)
            {
                ++g_poseV2LookLogs;
                LogVehicleLook("change", now, byHash, byHandle, timeMs);
            }
        }
    }

    // The burnt state of every recorded vehicle, twice a
    // second, stored when it changes (VehicleBurnt track). A wagon does not
    // go black through the charred bit (+0x1B0 bit 4 never set on the
    // burnt WAGON02X in a recorded test: no POSE_V2_SCORCHED line):
    // the fire darkens it through the burn level of its shader effect
    // (draw handler +0x28, effect +0x18, burn block from vfunc +0x88, level
    // at +0x1C; written by the fire's burn update and by the vehicle's,
    // as _SET_OBJECT_BURN_LEVEL does for objects), which only
    // grows. No native reads it, so the state recorded is the natives'
    // proxy: burnt from the first sample the vehicle is on fire or charred
    // on (latched, the level does not go back).
    void SamplePoseV2VehicleBurnt()
    {
        REPLAY_PROFILE_ZONE("rec.vehicle_burnt");
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (!entity.vehicle || !replay::entities::Exists(entity.handle))
                continue;
            static replay::TrackIndex<replay::ScorchTrack> index;
            auto track = FindPoseV2Track(index, Ctx().clip.vehicleBurnt, entity.id.Packed());
            if (track != Ctx().clip.vehicleBurnt.end() &&
                ((!track->states.empty() && track->states.back().second) ||
                    (track->checked && timeMs - track->lastCheckMs < 500)))
            {
                continue;
            }
            const bool onFire = FIRE::IS_ENTITY_ON_FIRE(entity.handle) != FALSE;
            const bool charred = (static_cast<std::uint32_t>(ENTITY::_0x37B01666BAE8F7EF(entity.handle)) & 1u) != 0;
            if (track == Ctx().clip.vehicleBurnt.end())
            {
                Ctx().clip.vehicleBurnt.push_back({entity.id.Packed(), {}, 0, false});
                track = Ctx().clip.vehicleBurnt.end() - 1;
            }
            track->lastCheckMs = timeMs;
            track->checked = true;
            if (!onFire && !charred)
                continue;
            track->states.push_back({timeMs, true});
            if (g_poseV2BurntLogs < 64)
            {
                ++g_poseV2BurntLogs;
                Ctx().log << "POSE_V2_VEHICLE_BURNT,phase=record,id=" << replay::entity::IdText(entity.id)
                    << ",on_fire=" << (onFire ? 1 : 0) << ",charred=" << (charred ? 1 : 0)
                    << ",fire_damage=" << (FIRE::_IS_ENTITY_BEING_DAMAGED_BY_FIRE(entity.handle) ? 1 : 0)
                    << ",time_ms=" << timeMs << '\n';
            }
        }
    }

    // The fire record (burnt look, burn_level.h CharState) of
    // every recorded ped, horse and vehicle, four times a second: a search
    // of the engine's record tree per entity (a few nodes deep).
    // Stored on change (FireCharSame); the first read always (absent for
    // most: the state before the fire).
    void SamplePoseV2FireChar()
    {
        if (!replay::burn::CharEnabled())
            return;
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        if (timeMs < g_poseV2FireCharNextMs && g_poseV2FireCharNextMs - timeMs <= 250)
            return;
        g_poseV2FireCharNextMs = timeMs + 250;
        REPLAY_PROFILE_ZONE("rec.fire_char");
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            static replay::TrackIndex<replay::FireCharTrack> index;
            auto track = FindPoseV2Track(index, Ctx().clip.fireChars, entity.id.Packed());
            const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(entity.handle));
            if (!address)
                continue;
            replay::burn::CharState read;
            {
                REPLAY_PROFILE_ZONE("rec.fire_char.read");
                if (!replay::burn::ReadChar(reinterpret_cast<const void*>(address), read))
                    continue;
            }
            replay::FireCharState state;
            state.present = read.present;
            state.flags = read.flags;
            for (const replay::burn::CharVolume& volume : read.volumes)
            {
                std::array<float, replay::kFireCharFloats> values{};
                std::copy(std::begin(volume.v), std::end(volume.v), values.begin());
                state.volumes.push_back(values);
                // Where it sits.
                replay::FireCharAnchor anchor;
                anchor.component = volume.component;
                anchor.sub = volume.sub;
                anchor.flags = volume.flags;
                anchor.mask = volume.mask;
                std::copy(std::begin(volume.dir), std::end(volume.dir), anchor.dir);
                state.anchors.push_back(anchor);
            }
            const bool first = track == Ctx().clip.fireChars.end();
            if (first)
            {
                Ctx().clip.fireChars.push_back({entity.id.Packed(), {}, 0, false});
                track = Ctx().clip.fireChars.end() - 1;
            }
            if (!track->states.empty() && replay::FireCharSame(track->states.back().second, state))
                continue;
            if (g_poseV2FireCharLogs < 64)
            {
                ++g_poseV2FireCharLogs;
                Ctx().log << "POSE_V2_FIRE_CHAR,phase=record,id=" << replay::entity::IdText(entity.id)
                    << ",present=" << (state.present ? 1 : 0) << std::hex << ",flags=0x" << int(state.flags) << std::dec
                    << ",count=" << read.count << ",volumes=" << state.volumes.size()
                    << ",char=" << (state.volumes.empty() ? 0.0f : state.volumes[0][12])
                    << ",extent=" << (state.volumes.empty() ? 0.0f : state.volumes[0][0])
                    << ",age=" << (state.volumes.empty() ? 0.0f : state.volumes[0][7])
                    << ",on_fire=" << (FIRE::IS_ENTITY_ON_FIRE(entity.handle) ? 1 : 0)
                    << ",faults=" << replay::burn::CharFaults()
                    << ",time_ms=" << timeMs << '\n';
            }
            track->states.push_back({timeMs, std::move(state)});
        }
    }

    // The wheel slot behind a wheel index (offline, build 1491.50).
    // _BREAK_OFF_VEHICLE_WHEEL's handler takes an index (0 LF, 1 RF,
    // 2 LM, 3 RM, 4 LR, 5 RR), turns it into a hierarchy id (a table:
    // 19, 20, 23, 24, 21, 22) and that into the vehicle's wheel slot, u8
    // [veh+0xBB1+hid] (0xFF: no such wheel).
    // _IS_VEHICLE_WHEEL_DESTROYED's handler tests bit n of u32
    // [veh+0xA50] with n as given: it takes the slot, not the index. Wheel
    // count s32 [veh+0xBC0]. Returns the slot, -1 when the vehicle has no such
    // wheel, -2 unreadable or implausible.
    constexpr int kPoseV2WheelHierarchy[replay::kVehiclePartWheels] = {19, 20, 23, 24, 21, 22};
    int GuardedWheelSlot(std::uint64_t vehicle, int wheel)
    {
        __try
        {
            const std::int32_t count = *reinterpret_cast<const std::int32_t*>(vehicle + 0xBC0);
            if (count <= 0 || count > 10)
                return -2;
            const std::uint8_t slot = *reinterpret_cast<const std::uint8_t*>(vehicle + 0xBB1 + kPoseV2WheelHierarchy[wheel]);
            if (slot == 0xFF)
                return -1;
            return slot < count ? slot : -2;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -2;
        }
    }

    // The wheels and doors every recorded vehicle lost
    // (VehiclePartTrack), four times a second, stored when the mask changes.
    // Natives only on the vehicle (checked to be one): _IS_VEHICLE_WHEEL_DESTROYED
    // on the slots GuardedWheelSlot maps (distinct slots, else the wheel
    // bits keep their last value) and _IS_VEHICLE_DOOR_BROKEN on doors 0..9
    // (false for a door the model lacks). The first read is always stored.
    void SamplePoseV2VehicleParts()
    {
        REPLAY_PROFILE_ZONE("rec.vehicle_parts");
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (!entity.vehicle || !replay::entities::Exists(entity.handle) || !ENTITY::IS_ENTITY_A_VEHICLE(entity.handle))
                continue;
            static replay::TrackIndex<replay::VehiclePartTrack> index;
            auto track = FindPoseV2Track(index, Ctx().clip.vehicleParts, entity.id.Packed());
            // A vehicle that just lost groups with no fragment piece (a wheel
            // comes off as an object of its own) is sampled at once, not at its
            // 4 Hz turn.
            if (track != Ctx().clip.vehicleParts.end() && track->checked && timeMs - track->lastCheckMs < 250 &&
                !replay::objects::TakeVehiclePartsUrgent(entity.id))
                continue;
            const std::uint64_t address = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(entity.handle));
            int slots[replay::kVehiclePartWheels]{};
            bool wheelsRead = address != 0;
            std::uint32_t seen = 0;
            for (int wheel = 0; wheel < replay::kVehiclePartWheels; ++wheel)
            {
                slots[wheel] = address ? GuardedWheelSlot(address, wheel) : -2;
                if (slots[wheel] == -2 || (slots[wheel] >= 0 && (seen & (1u << slots[wheel]))))
                    wheelsRead = false;
                else if (slots[wheel] >= 0)
                    seen |= 1u << slots[wheel];
            }
            const std::uint16_t previous = track == Ctx().clip.vehicleParts.end() || track->states.empty()
                ? 0 : track->states.back().second;
            constexpr std::uint16_t kWheelBits = (1u << replay::kVehiclePartWheels) - 1;
            std::uint16_t mask = 0;
            if (wheelsRead)
            {
                for (int wheel = 0; wheel < replay::kVehiclePartWheels; ++wheel)
                {
                    if (slots[wheel] >= 0 && VEHICLE::_IS_VEHICLE_WHEEL_DESTROYED(entity.handle, slots[wheel]))
                        mask |= static_cast<std::uint16_t>(1u << wheel);
                }
            }
            else
                mask = static_cast<std::uint16_t>(previous & kWheelBits);
            for (int door = 0; door < replay::kVehiclePartDoors; ++door)
            {
                if (VEHICLE::_IS_VEHICLE_DOOR_BROKEN(entity.handle, door))
                    mask |= static_cast<std::uint16_t>(1u << (replay::kVehiclePartDoorShift + door));
            }
            const bool first = track == Ctx().clip.vehicleParts.end();
            if (first)
            {
                Ctx().clip.vehicleParts.push_back({entity.id.Packed(), {}, 0, false});
                track = Ctx().clip.vehicleParts.end() - 1;
            }
            track->lastCheckMs = timeMs;
            track->checked = true;
            if (!track->states.empty() && mask == track->states.back().second)
                continue;
            track->states.push_back({timeMs, mask});
            // A wheel that came off now is an object of its own: recorded as a
            // piece of the vehicle with its track (replay_objects.cpp
            // NoteVehicleWheelOff).
            if (!first && wheelsRead)
                for (int wheel = 0; wheel < replay::kVehiclePartWheels; ++wheel)
                    if ((mask & ~previous & (1u << wheel)) && slots[wheel] >= 0)
                        replay::objects::NoteVehicleWheelOff(entity.id, entity.handle, wheel, slots[wheel]);
            if (g_poseV2PartLogs < 64)
            {
                ++g_poseV2PartLogs;
                std::ostream& log = Ctx().log;
                log << "POSE_V2_VEHICLE_PARTS,phase=record,id=" << replay::entity::IdText(entity.id)
                    << ",wheels_off=0x" << std::hex << static_cast<unsigned>(mask & kWheelBits)
                    << ",doors_off=0x" << static_cast<unsigned>(mask >> replay::kVehiclePartDoorShift) << std::dec
                    << ",wheels_read=" << (wheelsRead ? 1 : 0) << ",slots=";
                for (int wheel = 0; wheel < replay::kVehiclePartWheels; ++wheel)
                    log << (wheel ? ";" : "") << slots[wheel];
                log << ",first=" << (first ? 1 : 0) << ",time_ms=" << timeMs << '\n';
            }
        }
    }

    // The door ratios of every recorded vehicle, read from the engine (rung 2)
    // at most every 50 ms per vehicle, stored on change. The train cars are
    // read by replay_trains.
    void SamplePoseV2VehicleDoors()
    {
        REPLAY_PROFILE_ZONE("rec.vehicle_doors");
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (!entity.vehicle || !replay::entities::Exists(entity.handle))
                continue;
            replay::vehdoors::RecordVehicle(Ctx().clip.vehicleDoors, entity.id.Packed(), entity.handle, false, timeMs,
                Ctx().log);
        }
    }

    // Appearance changes over time (appearance_track.h). A few recorded peds
    // and horses per tick, round robin, each at most every 250 ms: the layer
    // count (one native); the full fingerprint (1 + layers natives) when the
    // count moved, once a second, or while a change waits for its confirmation,
    // at most one per tick. A print other than what the recording already has
    // for the entity at this time (create packet, pose segment or last change,
    // as AppearanceAt picks it) must be seen on two checks in a row (the engine
    // passes through partial layer sets while it rebuilds a metaped: 10 -> 0 ->
    // 11 in the hogtie bench) and is stored at its first sighting. A change
    // that also changes the skeleton is a pose segment already: the segment
    // captures the same layers, the print then matches and nothing is stored.

    constexpr std::size_t kPoseV2AppearanceVisitsPerTick = 4;
    constexpr std::uint32_t kPoseV2AppearanceCheckMs = 250;
    constexpr std::uint32_t kPoseV2AppearanceFullMs = 1000;

    void SamplePoseV2Appearance()
    {
        REPLAY_PROFILE_ZONE("rec.appearance");
        std::vector<replay::recorder::RecordedEntity>& entities = Ctx().recorder.entities;
        if (entities.empty())
            return;
        const std::uint64_t startedAt = replay::prof::Now();
        const std::uint32_t timeMs = replay::pose::capture::NowMs();
        PoseV2AppearanceStats& stats = g_poseV2AppearanceStats;
        bool fullBudget = true;
        const std::size_t visits = (std::min)(entities.size(), kPoseV2AppearanceVisitsPerTick);
        for (std::size_t visit = 0; visit < visits; ++visit)
        {
            // Entities that went away were retired this tick already.
            const replay::recorder::RecordedEntity& entity = entities[g_poseV2AppearanceCursor++ % entities.size()];
            if (entity.vehicle)
                continue;
            const std::uint32_t packed = entity.id.Packed();
            static replay::TrackIndex<PoseV2AppearanceProbe> index;
            auto probe = FindPoseV2Track(index, g_poseV2AppearanceProbes, packed);
            if (probe == g_poseV2AppearanceProbes.end())
            {
                g_poseV2AppearanceProbes.push_back({});
                probe = g_poseV2AppearanceProbes.end() - 1;
                probe->id = packed;
            }
            if (probe->lastCount >= 0 && timeMs - probe->lastCheckMs < kPoseV2AppearanceCheckMs)
                continue;
            ++stats.visits;
            const int count = replay::entity::appearance::LayerCount(entity.handle);
            const bool full = probe->pending || count != probe->lastCount ||
                timeMs - probe->lastFullMs >= kPoseV2AppearanceFullMs;
            if (!full)
            {
                probe->lastCheckMs = timeMs;
                continue;
            }
            // Over budget: checked again on its next turn.
            if (!fullBudget)
                continue;
            fullBudget = false;
            probe->lastCheckMs = timeMs;
            probe->lastFullMs = timeMs;
            probe->lastCount = count;
            ++stats.fullReads;
            std::uint64_t live = 0;
            int liveCount = 0;
            if (!replay::entity::appearance::Fingerprint(entity.handle, live, liveCount))
            {
                ++stats.empty;
                probe->pending = false;
                continue;
            }
            const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(packed);
            if (!create)
                continue;
            const replay::entity::PoseSegment* segment = Ctx().clip.segments.At(entity.id, timeMs);
            const replay::AppearanceTrack* current = replay::FindAppearanceTrack(Ctx().clip.appearances, packed);
            const replay::WantedAppearance recorded = replay::AppearanceAt(current, segment, create->appearance, timeMs);
            if (replay::AppearancePrint(*recorded.appearance) == live)
            {
                stats.unconfirmed += probe->pending ? 1 : 0;
                probe->pending = false;
                continue;
            }
            if (!probe->pending || probe->pendingPrint != live)
            {
                stats.unconfirmed += probe->pending ? 1 : 0;
                probe->pending = true;
                probe->pendingPrint = live;
                probe->pendingSinceMs = timeMs;
                continue;
            }
            probe->pending = false;
            replay::AppearanceChange change;
            change.timeMs = probe->pendingSinceMs;
            // Never before the last change or the segment in force (times
            // per track ascend; AppearanceAt skips a change older than the
            // segment).
            if (current && !current->changes.empty() && change.timeMs < current->changes.back().timeMs)
                change.timeMs = current->changes.back().timeMs;
            if (segment && change.timeMs < segment->startMs)
                change.timeMs = segment->startMs;
            const std::size_t previousLayers = recorded.appearance->assets.size();
            replay::entity::appearance::Capture(entity.handle, change.appearance);
            const std::size_t layers = change.appearance.assets.size();
            const std::uint32_t outfit = change.appearance.outfit;
            replay::AppearanceTrack* track = nullptr;
            for (replay::AppearanceTrack& entry : Ctx().clip.appearances)
            {
                if (entry.id == packed)
                    track = &entry;
            }
            if (!track)
            {
                Ctx().clip.appearances.push_back({});
                track = &Ctx().clip.appearances.back();
                track->id = packed;
                ++stats.entitiesChanged;
            }
            track->changes.push_back(std::move(change));
            ++stats.changes;
            // The whole look with the new layers.
            RecordPoseV2Metaped(entity.handle, entity.id, replay::metaped::Source::Change, track->changes.back().timeMs,
                track->changes.back().appearance);
            if (g_poseV2AppearanceLogs < 64)
            {
                ++g_poseV2AppearanceLogs;
                Ctx().log << "POSE_V2_APPEARANCE_CHANGE,phase=record,id=" << replay::entity::IdText(entity.id)
                    << ",layers=" << layers << ",previous_layers=" << previousLayers
                    << ",outfit=0x" << std::hex << outfit << std::dec
                    << ",bones=" << entity.boneCount
                    << ",time_ms=" << track->changes.back().timeMs << ",confirm_ms=" << timeMs << '\n';
            }
        }
        const double us = static_cast<double>(ElapsedUs(startedAt, replay::prof::Now()));
        ++stats.ticks;
        stats.totalUs += us;
        stats.maxUs = (std::max)(stats.maxUs, us);
    }

    // The pose fidelity judge's engine reads (PoseFidelity=1, pose_fidelity.h),
    // after every script capture of the tick (the polls, then the posed
    // objects' polls in replay::objects::Sample: a read is stamped with the
    // time of its content, capture::ContentTimeMs, so the capture of this tick
    // must come first): every recorded entity's current skeleton and record, a
    // few times a second each. Posed objects (a carried pelt) count as "other".
    // replay::objects::Sample has just retired the gone ones and ended the
    // pose of one whose skeleton changed, so a posed object's armed skeleton
    // is its current one: no second resolve (that would go through a handle
    // that may have been deleted since the last tick, with no Exists check
    // first). Reads only, no natives.
    void SamplePoseV2Fidelity()
    {
        REPLAY_PROFILE_ZONE("rec.pose_fidelity");
        namespace fidelity = replay::pose::fidelity;
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (!fidelity::Due(entity.recordId, nowMs))
                continue;
            const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(entity.recordId);
            const fidelity::Kind kind = entity.vehicle ? fidelity::Kind::Vehicle
                : !create ? fidelity::Kind::Other
                : create->kind == replay::entity::EntityKind::Ped ? fidelity::Kind::Ped
                : create->kind == replay::entity::EntityKind::Horse ? fidelity::Kind::Horse
                : fidelity::Kind::Other;
            fidelity::Sample(entity.recordId, entity.skeleton, entity.boneCount, kind, nowMs);
        }
        for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
        {
            if (weapon.posed && fidelity::Due(weapon.id.Packed(), nowMs))
                fidelity::Sample(weapon.id.Packed(), weapon.poseSkeleton, weapon.poseBones, fidelity::Kind::Other, nowMs);
        }
    }
    }

    void UpdatePoseV2Capture(Ped mount)
    {
        REPLAY_PROFILE_ZONE("rec.pose_v2");
        if (!Ctx().recorder.recording)
            return;
        const std::uint64_t startedAt = replay::prof::Now();

        for (std::size_t index = Ctx().recorder.entities.size(); index-- > 0;)
        {
            if (!replay::entities::Exists(Ctx().recorder.entities[index].handle))
                RetirePoseV2Entity(index, "entity_gone");
        }
        UpdateFullAnim(); // experiment, off by default
        // The recorded horses' muscles (shader_vars_replay.h, section SHVR).
        replay::muscles::TickRecord(replay::pose::capture::NowMs());
        const std::uint64_t checkStartedAt = replay::prof::Now();
        CheckPoseV2Skeletons();
        ++g_poseV2ScriptTiming.checks;
        g_poseV2ScriptTiming.checkTotalUs += static_cast<double>(
            ElapsedUs(checkStartedAt, replay::prof::Now()));
        // The pose finalizer may not visit vehicle skeletons; sample them
        // here with the same rate gate and ring.
        // The polls and end-of-frame checks.
        static const replay::prof::ZoneId pollsZone = replay::prof::Register("rec.pose_polls");
        const std::uint64_t pollsStartedAt = replay::prof::Now();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (entity.vehicle)
            {
                replay::pose::capture::PollFromScript(entity.recordId);
                continue;
            }
            // The end-of-frame check (and, while the engine
            // changes this skeleton after the finalizer, its capture).
            // With PollIfStalled, in one call (one lookup of the record).
            const replay::pose::capture::LateCheck late = replay::pose::capture::PollAndCheckLate(entity.recordId);
            if ((late.switchedOn || late.switchedOff || late.capped || (late.changed && g_poseV2LateLogs < 24)) &&
                g_poseV2LateLogs < 64)
            {
                ++g_poseV2LateLogs;
                std::ostream& log = Ctx().log;
                const replay::logfmt::FormatScope logFormat(log); // precision not left behind
                log << std::fixed << std::setprecision(3)
                    << "POSE_V2_LATE_POSE,id=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(entity.recordId))
                    << ",state=" << (late.switchedOn ? "late_on" : late.switchedOff ? "late_off"
                                     : late.capped ? "capped" : "changed")
                    << ",local_bones=" << late.localBones
                    << ",object_bones=" << late.objectBones
                    << ",worst_bone=" << late.worstBone
                    << ",max_m=" << late.maxTranslation
                    << ",max_rad=" << late.maxRotation
                    << ",time_ms=" << replay::pose::capture::NowMs() << '\n';
                log.unsetf(std::ios_base::floatfield);
                log.flush();
            }
        }
        replay::prof::Add(pollsZone, replay::prof::Now() - pollsStartedAt);
        // The recording gate. The clock stands still: entities
        // that went away are retired and skeleton changes open segments
        // (at the cut instant, which is where they belong), the pose polls
        // skip by themselves, the detour queues are drained and discarded
        // (events::Record takes its paused path), and nothing is sampled
        // or scanned.
        if (replay::pose::capture::IsClockPaused())
        {
            replay::objects::DiscardPendingEvents();
            replay::events::Record();
            return;
        }
        replay::objects::Sample();
        if (replay::pose::fidelity::Enabled()) // PoseFidelity=1 only, after the posed objects' polls
            SamplePoseV2Fidelity();
        replay::objects::UpdateRecordedProps();
        replay::trains::SampleRecording();
        SamplePoseV2Ropes();
        replay::objects::SearchDrops();
        SamplePoseV2Scorched();
        SamplePoseV2Appearance();
        SamplePoseV2Metaped();
        SamplePoseV2VehicleWindows();
        SamplePoseV2VehicleLook();
        SamplePoseV2VehicleParts();
        SamplePoseV2VehicleDoors();
        SamplePoseV2VehicleBurnt();
        SamplePoseV2FireChar();
        replay::events::Record();

        const ULONGLONG now = GetTickCount64();
        if (const std::uint64_t breaks = replay::frag::Total(); breaks != g_poseV2BreaksSeen)
        {
            g_poseV2BreaksSeen = breaks;
            g_poseV2BreakScanUntil = now + kPoseV2BreakScanMs;
        }
        if (now >= g_poseV2NextScanAt || now < g_poseV2BreakScanUntil)
        {
            const std::uint64_t scanStartedAt = replay::prof::Now();
            g_poseV2NextScanAt = now + kPoseV2ScanIntervalMs;
            ScanPoseV2Entities(mount);
            const double scanUs = static_cast<double>(
                ElapsedUs(scanStartedAt, replay::prof::Now()));
            ++g_poseV2ScriptTiming.scans;
            g_poseV2ScriptTiming.scanTotalUs += scanUs;
            g_poseV2ScriptTiming.scanMaxUs = (std::max)(g_poseV2ScriptTiming.scanMaxUs, scanUs);
        }
        Ctx().clip.lifecycle.Advance(replay::pose::capture::NowMs());

        const double updateUs = static_cast<double>(
            ElapsedUs(startedAt, replay::prof::Now()));
        ++g_poseV2ScriptTiming.updates;
        g_poseV2ScriptTiming.updateTotalUs += updateUs;
        g_poseV2ScriptTiming.updateMaxUs = (std::max)(g_poseV2ScriptTiming.updateMaxUs, updateUs);
    }

    namespace
    {

    // One POSE_V2_CAPTURE_STOP line's inputs, taken on the script thread at the
    // stop; the line is formatted on a worker (deferred_log.h).
    struct PoseV2StopLine
    {
        std::uint32_t recordId = 0;
        replay::entity::ReplayId id;
        const char* kind = "unknown"; // KindText's static strings
        replay::pose::capture::Stats stats;
    };

    // The stats come from the caller's one Snapshot and the line
    // is not flushed (the stop wrote 440 lines with a flush each, plus a
    // second Snapshot per record: ~25 ms of the city's 80 ms stop tick).
    // Runs on a worker thread (deferred_log.h) and writes to its stream: only
    // its arguments, never Ctx() or a global (the id and kind come resolved:
    // the clip is the script thread's). The text does not depend on the
    // stream's state: fixed and precision are set first and the first number
    // follows an explicit std::hex.
    void LogPoseV2RecordStats(std::ostream& log, std::uint32_t recordId, replay::entity::ReplayId id, const char* kind,
        ULONGLONG elapsedMs, const replay::pose::capture::Stats& stats)
    {
        const double seconds = stats.durationMs > 0
            ? static_cast<double>(stats.durationMs) / 1000.0
            : static_cast<double>(elapsedMs) / 1000.0;
        const double bytesPerSecond = seconds > 0.0
            ? static_cast<double>(stats.trackBytes) / seconds
            : 0.0;

        char engineErrorText[96]{};
        std::snprintf(engineErrorText, sizeof(engineErrorText), ",eng_max_rot=%.2e,eng_max_trans=%.2e",
            static_cast<double>(stats.engineComposeMaxRotation), static_cast<double>(stats.engineComposeMaxTranslation));
        log << std::fixed << std::setprecision(3)
            << "POSE_V2_CAPTURE_STOP"
            << ",id=" << replay::entity::IdText(id)
            << ",record=0x" << std::hex << recordId << std::dec
            << ",kind=" << kind
            << ",bones=" << stats.boneCount
            << ",recording_ms=" << elapsedMs
            << ",track_span_ms=" << stats.durationMs
            << ",captured=" << stats.captured
            << ",detour_calls=" << stats.detourCalls
            << ",dropped_ring=" << stats.droppedRingFull
            << ",bone_mismatch=" << stats.boneCountMismatch
            << ",copy_faults=" << stats.copyFaults
            << ",detour_avg_us=" << stats.detourCaptureAvgUs
            << ",detour_max_us=" << stats.detourCaptureMaxUs
            << ",layout_ready=" << (stats.layoutReady ? 1 : 0)
            << ",layout_reused=" << (stats.layoutReused ? 1 : 0)
            << ",layout_candidates=" << stats.layoutCandidates
            << ",eng_parents=" << (stats.engineParents ? 1 : 0)
            << ",eng_order_ok=" << (stats.engineParentOrderOk ? 1 : 0)
            << ",eng_root_scale=" << stats.engineRootScale
            << ",eng_groups=" << stats.engineGroups
            << ",eng_agree=" << stats.engineParentAgree
            << ",eng_disagree=" << stats.engineParentDisagree
            << ",eng_unresolved=" << stats.engineParentUnresolved
            << ",eng_checks=" << stats.engineComposeChecks
            << engineErrorText
            << ",eng_layout=" << (stats.engineLayout ? 1 : 0)
            << ",eng_layout_rejected=" << stats.engineLayoutRejected
            << ",composed_frames=" << stats.objectsComposed
            << ",frames_without_objects=" << stats.framesWithoutObjects
            << ",discovery_ms=" << stats.discoveryMs
            << ",order=" << static_cast<int>(stats.discovery.order)
            << ",inherit_scale=" << (stats.discovery.inheritScale ? 1 : 0)
            << ",demoted_bones=" << stats.demotedBones
            << ",demotion_events=" << stats.demotionEvents
            << ",reparented=" << stats.reparentedBones
            << ",reparent_jobs=" << stats.reparentJobs
            << ",reparent_timeouts=" << stats.reparentTimeouts
            << ",first_demotion_ms=" << stats.firstDemotionMs
            << ",last_demotion_ms=" << stats.lastDemotionMs
            << ",rebase_frames=" << stats.rebaseFrames << ",cut_frames=" << stats.cutFrames
            << ",roots=" << stats.discovery.roots
            << ",parented=" << stats.discovery.parented
            << ",unresolved=" << stats.discovery.unresolved
            << ",ambiguous=" << stats.discovery.ambiguous
            << ",high_precision=" << stats.discovery.highPrecision
            << ",encoded=" << stats.encodedFrames
            << ",keyframes=" << stats.keyframes
            << ",relative_keyframes=" << stats.relativeKeyframes
            << ",full_kf_bytes=" << stats.fullKeyframeBytes
            << ",relative_kf_bytes=" << stats.relativeKeyframeBytes
            << ",pending_dropped=" << stats.pendingDropped
            << ",track_bytes=" << stats.trackBytes
            << ",bytes_per_s=" << bytesPerSecond
            << ",entries_per_frame=" << stats.entriesPerFrameAvg
            << ",encode_avg_us=" << stats.encodeAvgUs
            << ",encode_max_us=" << stats.encodeMaxUs
            << ",composition_max_m=" << stats.compositionMaxTranslation
            << ",composition_max_rad=" << stats.compositionMaxRotation
            << ",world_read_faults=" << stats.worldReadFaults
            << ",script_polls=" << stats.scriptPolls
            << ",late_checks=" << stats.lateChecks
            << ",late_skipped=" << stats.lateSkipped
            << ",late_changed=" << stats.lateChanged
            << ",late_local_bones=" << stats.lateMaxLocalBones
            << ",late_object_bones=" << stats.lateMaxObjectBones
            << ",late_worst_bone=" << stats.lateWorstBone
            << ",late_max_m=" << stats.lateMaxTranslation
            << ",late_switches=" << stats.lateSwitches
            << ",late_capped=" << stats.lateCapped
            << ",late_samples=" << stats.lateSamples
            << ",engine_writes=0"
            << ",validated=" << stats.validatedFrames
            << ",validation_skipped=" << stats.validationSkipped
            << ",validation_incremental=" << stats.validationIncremental
            << ",validation_bones_checked=" << stats.validationBonesChecked
            << ",partial_composed=" << stats.partialComposed
            << ",raw_delta_channels=" << stats.rawDeltaChannels
            << ",raw_same_channels=" << stats.rawSameChannels
            << ",raw_same_pct=" << (stats.rawDeltaChannels
                   ? 100.0 * static_cast<double>(stats.rawSameChannels) / static_cast<double>(stats.rawDeltaChannels)
                   : 0.0)
            << ",raw_same_frames=" << stats.rawSameFrames
            << ",late_equal=" << stats.lateEqualChecks
            << ",late_verified=" << stats.lateVerified
            << ",late_verify_mismatch=" << stats.lateVerifyMismatch
            << ",poll_idle=" << stats.pollIdleFrames
            << ",poll_idle_local_frames=" << stats.pollIdleLocalFrames
            << ",poll_idle_object_frames=" << stats.pollIdleObjectFrames
            << ",poll_idle_root_frames=" << stats.pollIdleRootFrames
            << ",poll_idle_local_bones=" << stats.pollIdleLocalBones
            << ",poll_idle_object_bones=" << stats.pollIdleObjectBones
            << ",poll_idle_checked=" << stats.pollIdleChecked
            << ",poll_after_calls=" << stats.pollCallFrames
            << ",poll_after_calls_sum=" << stats.pollCallsSum
            << ",poll_after_same=" << stats.pollCallSameFrames
            << ",poll_after_local_bones=" << stats.pollCallLocalBones
            << ",poll_after_object_bones=" << stats.pollCallObjectBones
            << ",poll_after_checked=" << stats.pollCallChecked
            << ",poll_raced=" << stats.pollRaced
            << ",late_direct=" << stats.lateDirect
            << ",stall_polls=" << stats.stallPolls
            << ",polls_skipped_recent=" << stats.pollsSkippedRecent
            << ",ring_turn_busy=" << stats.ringTurnBusy
            << ",late_interval_max_ms=" << stats.lateIntervalMaxMs
            << '\n';
        // What the animation left unwritten in the finalizer's source
        // (pose_capture.h Stats::unwritten*).
        log << "POSE_V2_UNWRITTEN"
            << ",id=" << replay::entity::IdText(id)
            << ",record=0x" << std::hex << recordId << std::dec
            << ",kind=" << kind
            << ",bones=" << stats.boneCount
            << ",samples=" << stats.unwrittenSamples
            << ",always_t=" << stats.unwrittenAlways[0]
            << ",always_r=" << stats.unwrittenAlways[1]
            << ",always_s=" << stats.unwrittenAlways[2]
            << ",mixed_t=" << stats.unwrittenMixed[0]
            << ",mixed_r=" << stats.unwrittenMixed[1]
            << ",mixed_s=" << stats.unwrittenMixed[2]
            << ",min_rows=" << stats.unwrittenMinRows
            << ",max_rows=" << stats.unwrittenMaxRows
            << ",partial_rows=" << stats.unwrittenPartialRows
            << ",faults=" << stats.unwrittenFaults
            << ",scan_avg_us=" << stats.unwrittenScanAvgUs
            << ",scan_max_us=" << stats.unwrittenScanMaxUs
            << ",top=" << (stats.unwrittenTop.empty() ? "-" : stats.unwrittenTop)
            << ",timeline=" << (stats.unwrittenTimeline.empty() ? "-" : stats.unwrittenTimeline)
            // The census runs inside the detour; a record it never visited
            // (samples=0) whose every script copy had the same bytes never
            // moved: the skeleton as built, the bind pose (068 22:379), or a
            // corpse whose ragdoll slept before the take (008).
            << ",never_animated="
            << (replay::tpose::RecordedNeverAnimated(stats.captured, stats.detourCalls, stats.rawDeltaChannels,
                    stats.rawSameChannels) ? 1 : 0)
            << '\n';
        log.unsetf(std::ios_base::floatfield);
    }
    }

    void StopPoseV2Capture(ULONGLONG elapsedMs)
    {
        if (!Ctx().recorder.recording)
            return;
        Ctx().recorder.recording = false;
        // The stop's lines set fixed and precision 3 and 1
        // (POSE_V2_PERF) and left them on the shared log stream.
        const replay::logfmt::FormatScope logFormat(Ctx().log);
        // The stop's parts (the stop tick of a long clip, REC_STOP_TIMING).
        const std::uint64_t stopStartedAt = replay::prof::Now();
        replay::events::EndRecording();
        const std::uint64_t eventsEndedAt = replay::prof::Now();
        replay::pose::capture::StopAll();
        const std::uint64_t captureStoppedAt = replay::prof::Now();
        // Entities still live stay created until the end of the clip.
        Ctx().clip.lifecycle.Advance(replay::pose::capture::NowMs());
        for (const replay::recorder::RecordedProp& prop : Ctx().recorder.props)
            Ctx().clip.adoptedObjects.push_back(prop.object);
        EndFullAnim(); // before the entities are forgotten
        Ctx().recorder.entities.clear();
        Ctx().recorder.weapons.clear();
        Ctx().recorder.props.clear();
        Ctx().recorder.drops.clear();
        replay::objects::EndRecording();
        replay::trains::EndRecording();
        replay::vehdoors::EndRecording(Ctx().clip.vehicleDoors, Ctx().log);

        const std::uint64_t objectsEndedAt = replay::prof::Now();
        // POSE_V2_STOP_TIMING is written at the end of the stop now, with its
        // parts.
        Ctx().log << replay::entities::SummaryLine("record") << '\n';
        double totalBytes = 0.0;
        double worstDetourUs = 0.0;
        float worstErrorM = 0.0f;
        int records = 0;
        // The script copies of every record, by kind (ped, horse, other).
        struct PollEvidence
        {
            std::uint64_t records = 0, captured = 0, detourCalls = 0, validated = 0, incremental = 0;
            std::uint64_t idle = 0, idleLocalFrames = 0, idleObjectFrames = 0, idleRootFrames = 0;
            std::uint64_t idleLocalBones = 0, idleObjectBones = 0, idleChecked = 0;
            std::uint64_t after = 0, afterCalls = 0, afterSame = 0, afterLocalBones = 0, afterObjectBones = 0;
            std::uint64_t afterChecked = 0, raced = 0, bonesChecked = 0;
            std::uint64_t stallPolls = 0, skippedRecent = 0, turnBusy = 0;
        };
        PollEvidence pollEvidence[3];
        // The POSE_V2_CAPTURE_STOP lines (~2.4 KB of iostream formatting each;
        // the tail cost a median 49 us per record, 8 to 27 ms of the stop tick
        // in town) are formatted on a worker and written on a later tick
        // (deferred_log.h); the same text.
        const std::uint64_t recordsStartedAt = replay::prof::Now();
        std::vector<PoseV2StopLine> stopLines;
        // The detector of what the engine computed only part of the time. A
        // record with more than kDegradedMixedRows mixed rows (written in some
        // samples, left at the bind pose in others: off screen or far, before
        // the anim-visible byte) is counted as degraded; with the byte on the
        // bench measured 0 to 62 mixed rows per record (the expressions' own
        // switching), 530 to 1060 without it.
        constexpr std::uint32_t kDegradedMixedRows = 150;
        struct UnwrittenSummary
        {
            std::uint32_t records = 0;
            std::uint32_t scanned = 0;
            std::uint32_t degraded = 0;
            std::uint32_t worstMixed = 0;
            std::uint32_t worstRecord = 0;
            std::uint64_t mixedTotal = 0;
            std::uint64_t samples = 0;
            double scanUs = 0.0;
            // The records the census could not scan (no detour call) and, of
            // those, the never animated bodies (tpose::RecordedNeverAnimated:
            // the whole record still; the bind pose or a sleeping corpse, the
            // playback's POSE_V2_NEVER_ANIMATED tells them apart).
            std::uint32_t unscanned = 0;
            std::uint32_t neverAnimated = 0;
            std::string neverIds;
        } unwritten;
        for (const std::uint32_t recordId : replay::pose::capture::RecordIds())
        {
            const replay::pose::capture::Stats stats =
                replay::pose::capture::Snapshot(recordId);
            ++unwritten.records;
            if (stats.unwrittenSamples > 0)
            {
                const std::uint32_t mixed = stats.unwrittenMixed[0] + stats.unwrittenMixed[1] + stats.unwrittenMixed[2];
                ++unwritten.scanned;
                unwritten.mixedTotal += mixed;
                unwritten.samples += stats.unwrittenSamples;
                unwritten.scanUs += stats.unwrittenScanAvgUs * static_cast<double>(stats.unwrittenSamples);
                if (mixed > kDegradedMixedRows)
                    ++unwritten.degraded;
                if (mixed > unwritten.worstMixed)
                {
                    unwritten.worstMixed = mixed;
                    unwritten.worstRecord = recordId;
                }
            }
            else if (stats.captured > 0)
            {
                ++unwritten.unscanned;
                // Bodies only (peds and horses); a holstered
                // weapon or a prop is just as still and is not a T-pose.
                const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(recordId);
                const bool body = create && (create->kind == replay::entity::EntityKind::Ped ||
                    create->kind == replay::entity::EntityKind::Horse);
                if (body && replay::tpose::RecordedNeverAnimated(stats.captured, stats.detourCalls, stats.rawDeltaChannels,
                        stats.rawSameChannels))
                {
                    ++unwritten.neverAnimated;
                    if (unwritten.neverAnimated <= 16)
                        unwritten.neverIds += (unwritten.neverIds.empty() ? "" : ";") + replay::entity::IdText(create->id);
                }
            }
            {
                const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(recordId);
                stopLines.push_back({recordId, create ? create->id : replay::entity::ReplayId::FromPacked(recordId),
                    create ? replay::entity::KindText(create->kind) : "unknown", stats});
            }
            {
                const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(recordId);
                const char* kind = create ? replay::entity::KindText(create->kind) : "unknown";
                PollEvidence& e = pollEvidence[std::strcmp(kind, "ped") == 0 ? 0 : std::strcmp(kind, "horse") == 0 ? 1 : 2];
                ++e.records;
                e.captured += stats.captured;
                e.detourCalls += stats.detourCalls;
                e.validated += stats.validatedFrames;
                e.incremental += stats.validationIncremental;
                e.bonesChecked += stats.validationBonesChecked;
                e.idle += stats.pollIdleFrames;
                e.idleLocalFrames += stats.pollIdleLocalFrames;
                e.idleObjectFrames += stats.pollIdleObjectFrames;
                e.idleRootFrames += stats.pollIdleRootFrames;
                e.idleLocalBones += stats.pollIdleLocalBones;
                e.idleObjectBones += stats.pollIdleObjectBones;
                e.idleChecked += stats.pollIdleChecked;
                e.after += stats.pollCallFrames;
                e.afterCalls += stats.pollCallsSum;
                e.afterSame += stats.pollCallSameFrames;
                e.afterLocalBones += stats.pollCallLocalBones;
                e.afterObjectBones += stats.pollCallObjectBones;
                e.afterChecked += stats.pollCallChecked;
                e.raced += stats.pollRaced;
                e.stallPolls += stats.stallPolls;
                e.skippedRecent += stats.pollsSkippedRecent;
                e.turnBusy += stats.ringTurnBusy;
            }
            totalBytes += static_cast<double>(stats.trackBytes);
            worstDetourUs = std::max(worstDetourUs, stats.detourCaptureAvgUs);
            worstErrorM = std::max(worstErrorM, stats.compositionMaxTranslation);
            ++records;
        }
        Ctx().log << "POSE_V2_UNWRITTEN_SUMMARY,records=" << unwritten.records << ",scanned=" << unwritten.scanned
                  << ",degraded=" << unwritten.degraded << ",degraded_over_rows=" << kDegradedMixedRows
                  << ",worst_mixed_rows=" << unwritten.worstMixed << ",worst_record=0x" << std::hex
                  << unwritten.worstRecord << std::dec << ",mixed_rows_total=" << unwritten.mixedTotal
                  << ",samples=" << unwritten.samples << ",scan_avg_us="
                  << (unwritten.samples ? unwritten.scanUs / static_cast<double>(unwritten.samples) : 0.0)
                  << ",anim_visible=" << (g_fullAnim.animVisible ? 1 : 0) << ",unscanned=" << unwritten.unscanned
                  << ",never_animated=" << unwritten.neverAnimated
                  << ",never_animated_ids=" << (unwritten.neverIds.empty() ? "-" : unwritten.neverIds.c_str()) << '\n';
        if (!stopLines.empty())
        {
            // This stream as the lines left it
            // (the lines below print with its state).
            Ctx().log.unsetf(std::ios_base::floatfield);
            Ctx().log << std::dec << std::setprecision(3);
            replay::deferredlog::Log().Submit([lines = std::move(stopLines), elapsedMs]() {
                std::ostringstream text;
                for (const PoseV2StopLine& line : lines)
                    LogPoseV2RecordStats(text, line.recordId, line.id, line.kind, elapsedMs, line.stats);
                return text.str();
            });
        }
        const std::uint64_t recordsEndedAt = replay::prof::Now();
        {
            static const char* const kPollKinds[3] = {"ped", "horse", "other"};
            for (int kind = 0; kind < 3; ++kind)
            {
                const PollEvidence& e = pollEvidence[kind];
                if (e.records == 0)
                    continue;
                Ctx().log << "POSE_V2_POLL_EVIDENCE,kind=" << kPollKinds[kind] << ",records=" << e.records
                          << ",captured=" << e.captured << ",detour_calls=" << e.detourCalls
                          << ",validated=" << e.validated << ",incremental=" << e.incremental
                          << ",bones_checked=" << e.bonesChecked
                          << ",idle=" << e.idle << ",idle_local_frames=" << e.idleLocalFrames
                          << ",idle_object_frames=" << e.idleObjectFrames << ",idle_root_frames=" << e.idleRootFrames
                          << ",idle_local_bones=" << e.idleLocalBones << ",idle_object_bones=" << e.idleObjectBones
                          << ",idle_checked=" << e.idleChecked
                          << ",after_calls=" << e.after << ",after_calls_sum=" << e.afterCalls
                          << ",after_same=" << e.afterSame << ",after_local_bones=" << e.afterLocalBones
                          << ",after_object_bones=" << e.afterObjectBones << ",after_checked=" << e.afterChecked
                          << ",raced=" << e.raced
                          << ",stall_polls=" << e.stallPolls << ",polls_skipped_recent=" << e.skippedRecent
                          << ",ring_turn_busy=" << e.turnBusy << '\n';
            }
        }

        const replay::entity::LifecycleStats& lifecycle = Ctx().clip.lifecycle.Stats();
        std::ostream& log = Ctx().log;
        log << "POSE_V2_LIFECYCLE"
            << ",entities=" << records
            << ",blocks=" << lifecycle.blocks
            << ",creates=" << lifecycle.creates
            << ",deletes=" << lifecycle.deletes
            << ",full_creates=" << lifecycle.fullCreates
            << ",attaches=" << lifecycle.attaches
            << ",detaches=" << lifecycle.detaches
            << ",full_attachments=" << lifecycle.fullAttachments
            << ",live_at_end=" << Ctx().clip.lifecycle.Live().size()
            << ",rejected=" << Ctx().recorder.rejected.size()
            << ",evicted=" << g_poseV2Selection.evicted
            << ",full_scans=" << g_poseV2Selection.skippedScans
            << ",full_skipped_peak=" << g_poseV2Selection.skippedPeak
            << ",reentries=" << g_poseV2Selection.reentries
            << ",weapons=" << Ctx().clip.weaponTracks.size()
            << ",shots=" << Ctx().clip.shots.Shots().size()
            << ",shot_impacts=" << replay::events::ShotImpacts()
            << '\n';
        // How the entities were found (entity_discovery.h).
        log << "POSE_V2_DISCOVERY,phase=summary"
            << ",scans=" << g_poseV2Discovery.scans
            << ",ped_listing_empty=" << g_poseV2Discovery.pedListingEmpty
            << ",vehicle_listing_empty=" << g_poseV2Discovery.vehicleListingEmpty
            << ",nearby_calls=" << g_poseV2Discovery.nearbyCalls
            << ",nearby_peds=" << g_poseV2Discovery.nearbyPeds
            << ",nearby_vehicles=" << g_poseV2Discovery.nearbyVehicles
            << ",budget_spent=" << g_poseV2Discovery.budgetSpent
            << ",kept_recorded=" << g_poseV2Discovery.keptRecorded
            << ",dependents_added=" << g_poseV2Discovery.dependentsAdded
            << ",late_entries=" << g_poseV2Discovery.lateEntries;
        {
            const PoseV2DiscoveryStats& d = g_poseV2Discovery;
            const double scans = d.scans ? static_cast<double>(d.scans) : 1.0;
            log << ",vehicle_capacity=" << replay::capacity::VehicleCapacity() << std::fixed << std::setprecision(1)
                << ",recorded_peds_avg=" << static_cast<double>(d.recordedPedsSum) / scans
                << ",recorded_vehicles_avg=" << static_cast<double>(d.recordedVehiclesSum) / scans
                << ",recorded_vehicles_max=" << d.recordedVehiclesMax << std::defaultfloat << std::setprecision(6);
        }
        // The pool reader (entity_pools.h).
        {
            const PoseV2DiscoveryStats& d = g_poseV2Discovery;
            const double scans = d.poolScans ? static_cast<double>(d.poolScans) : 1.0;
            log << ",source=" << (d.poolScans ? (d.poolFallbacks ? "mixed" : "pool") : "listing")
                << ",pool_scans=" << d.poolScans << ",pool_fallbacks=" << d.poolFallbacks
                << ",pool_peds_avg=" << std::fixed << std::setprecision(1) << static_cast<double>(d.poolPedsLive) / scans
                << ",pool_peds_max=" << d.poolPedsLiveMax
                << ",ped_handled_avg=" << static_cast<double>(d.poolPedCandidates) / scans
                << ",pool_vehicles_avg=" << static_cast<double>(d.poolVehiclesLive) / scans
                << ",vehicle_handled_avg=" << static_cast<double>(d.poolVehicleCandidates) / scans
                << ",handles_created=" << d.poolCreated << ",handle_failed=" << d.poolFailed
                << ",budget_skipped=" << d.poolSkippedBudget
                << ",guid_free_min=" << (d.poolScans ? d.guidFreeMin : 0u) << ",guid_size=" << d.guidSize
                << ",pool_us_avg=" << d.poolUsTotal / scans << ",pool_us_max=" << d.poolUsMax
                << std::defaultfloat << std::setprecision(6);
        }
        log << '\n';
        for (const replay::WeaponTrack& entry : Ctx().clip.weaponTracks)
        {
            std::uint32_t firstMs = 0;
            std::uint32_t lastMs = 0;
            entry.track.Span(firstMs, lastMs);
            log << "POSE_V2_WEAPON_STOP"
                << ",id=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(entry.recordId))
                << ",samples=" << entry.track.Size()
                << ",dropped=" << entry.track.Dropped()
                << ",span_ms=" << (lastMs - firstMs)
                << '\n';
        }
        const PoseV2ScriptTiming& timing = g_poseV2ScriptTiming;
        const replay::pose::capture::EngineTiming engine =
            replay::pose::capture::EngineTimingSnapshot();
        log << std::fixed << std::setprecision(1)
            << "POSE_V2_PERF"
            << ",script_updates=" << timing.updates
            << ",script_avg_us=" << (timing.updates ? timing.updateTotalUs / timing.updates : 0.0)
            << ",script_max_us=" << timing.updateMaxUs
            << ",scans=" << timing.scans
            << ",scan_avg_us=" << (timing.scans ? timing.scanTotalUs / timing.scans : 0.0)
            << ",scan_max_us=" << timing.scanMaxUs
            << ",skeleton_resolves=" << timing.resolves
            << ",check_avg_us=" << (timing.checks ? timing.checkTotalUs / timing.checks : 0.0)
            << ",pool_avg_us=" << (timing.scans ? timing.poolTotalUs / timing.scans : 0.0)
            << ",pool_avg_peds=" << (timing.scans ? static_cast<double>(timing.poolPeds) / timing.scans : 0.0)
            << ",register_avg_us=" << (timing.scans ? timing.registerTotalUs / timing.scans : 0.0)
            << ",detour_calls_all=" << engine.detourCalls
            << ",detour_us_per_s=" << engine.detourUsPerSecond
            << ",pressure_wakes=" << engine.pressureWakes
            << ",worker_busy_ms=" << engine.workerBusyMs
            << ",worker_discovery_ms=" << engine.workerDiscoveryMs
            << ",pending_peak_frames=" << engine.pendingPeakFrames
            << '\n';
        {
            // The capture worker's time, which PROFILE_TOTAL_FRAME does not see
            // (another thread): per script tick of this recording and as a
            // share of one core, split by phase, and the g_mutex waits.
            const double ticks = static_cast<double>((std::max)(std::uint64_t{1}, replay::prof::SessionFrameCount()));
            const double spanMs = (std::max)(1.0, static_cast<double>(elapsedMs));
            const double busyMs = engine.workerBusyMs;
            const double otherMs = (std::max)(0.0, busyMs - engine.workerDrainMs - engine.workerComposeMs -
                engine.workerValidateMs - engine.workerEncodeMs);
            // The records left, those the worker still visited, those the
            // buffer removed.
            const replay::pose::capture::RecordCounts counts = replay::pose::capture::RecordCountsSnapshot();
            log << "POSE_V2_WORKER"
                << ",shortcuts=1,copy_plain=0" // release 1.0: always (the A/B keys are gone)
                << ",fidelity_hz=" << (replay::pose::fidelity::Enabled() ? replay::pose::fidelity::Hz() : 0u)
                << ",stall_poll_ms=" << replay::pose::capture::kDefaultStallPollMs
                << ",worker_poll_ms=" << replay::pose::capture::kWorkerPollMs
                << ",object_burn_level=" << (replay::objects::ObjectBurnLevel() ? 1 : 0)
                << ",object_drawn_check=" << (replay::objects::ObjectDrawnCheck() ? 1 : 0)
                << ",object_look=" << (replay::objlook::RecordAndPlay() ? 1 : 0) // object texture and tint
                << ",ped_capacity=" << replay::capacity::PedCapacity()
                << ",records=" << counts.records
                << ",drain=" << counts.drain
                << ",pruned=" << counts.pruned
                << ",ticks=" << static_cast<std::uint64_t>(ticks)
                << ",frames=" << engine.workerFrames
                << ",busy_ms=" << busyMs
                << ",cpu_ms=" << engine.workerCpuMs
                << ",core_pct=" << 100.0 * busyMs / spanMs
                << ",cpu_core_pct=" << 100.0 * engine.workerCpuMs / spanMs
                << ",busy_us_per_tick=" << busyMs * 1000.0 / ticks
                << ",cpu_us_per_tick=" << engine.workerCpuMs * 1000.0 / ticks
                << ",drain_ms=" << engine.workerDrainMs
                << ",compose_ms=" << engine.workerComposeMs
                << ",validate_ms=" << engine.workerValidateMs
                << ",encode_ms=" << engine.workerEncodeMs
                << ",other_ms=" << otherMs
                << ",lock_wait_ms=" << engine.workerLockWaitMs
                << ",lock_wait_max_us=" << engine.workerLockWaitMaxUs
                << ",script_lock_waits=" << engine.scriptLockWaits
                << ",script_lock_wait_ms=" << engine.scriptLockWaitMs
                << ",script_lock_wait_max_us=" << engine.scriptLockWaitMaxUs
                << ",ring_pool_hits=" << engine.ringPoolHits
                << ",ring_pool_misses=" << engine.ringPoolMisses
                << ",ring_pool_mb=" << engine.ringPoolMb
                << ",start_avg_us=" << engine.startAvgUs
                << ",start_max_us=" << engine.startMaxUs
                << ",reserve_hits=" << engine.reserveHits
                << ",reserve_misses=" << engine.reserveMisses
                << ",discovery_hold_max_us=" << engine.discoveryHoldMaxUs;
            for (std::size_t site = 0; site < replay::pose::capture::kScriptLockSites; ++site)
                log << ",script_lock_max_us_" << replay::pose::capture::kScriptLockSiteNames[site] << '='
                    << engine.scriptLockSiteMaxUs[site];
            log << '\n';
        }
        log.unsetf(std::ios_base::floatfield);
        {
            // The appearance change sampler's summary.
            const PoseV2AppearanceStats& appearance = g_poseV2AppearanceStats;
            char text[256]{};
            std::snprintf(text, sizeof(text),
                "POSE_V2_APPEARANCE_SUMMARY,entities_changed=%llu,changes=%llu,visits=%llu,full_reads=%llu,"
                "empty=%llu,unconfirmed=%llu,ticks=%llu,avg_us=%.1f,max_us=%.1f\n",
                static_cast<unsigned long long>(appearance.entitiesChanged),
                static_cast<unsigned long long>(appearance.changes),
                static_cast<unsigned long long>(appearance.visits),
                static_cast<unsigned long long>(appearance.fullReads),
                static_cast<unsigned long long>(appearance.empty),
                static_cast<unsigned long long>(appearance.unconfirmed),
                static_cast<unsigned long long>(appearance.ticks),
                appearance.ticks ? appearance.totalUs / static_cast<double>(appearance.ticks) : 0.0,
                appearance.maxUs);
            log << text;
        }
        {
            // The metaped sampler's summary.
            const PoseV2MetapedStats& m = g_poseV2MetapedStats;
            const replay::metaped::engine::Stats& reader = replay::metaped::engine::Counters();
            std::size_t tracks = 0, withExpressions = 0;
            std::size_t withShopItems = 0, shopStates = 0, shopItems = 0, shopWithState = 0;
            for (const replay::metaped::Track& track : Ctx().clip.metapeds)
            {
                ++tracks;
                for (const replay::metaped::State& state : track.states)
                    if (!state.expressions.empty())
                    {
                        ++withExpressions;
                        break;
                    }
                bool anyItems = false;
                for (const replay::metaped::State& state : track.states)
                {
                    if (state.shopItems.empty())
                        continue;
                    anyItems = true;
                    ++shopStates;
                    shopItems += state.shopItems.size();
                    for (const replay::metaped::ShopItem& item : state.shopItems)
                        shopWithState += item.state ? 1u : 0u;
                }
                withShopItems += anyItems ? 1u : 0u;
            }
            log << "POSE_V2_METAPED_SUMMARY,phase=record,entities=" << tracks << ",with_expressions=" << withExpressions
                << ",states=" << m.states << ",confirms=" << m.confirms << ",late=" << m.late << ",changes=" << m.changes
                << ",unstable_dropped=" << m.unstableDropped << ",read_failures=" << m.readFailures
                << ",no_reader=" << m.noExpressions << ",reader_reads=" << reader.reads
                << ",cross_checks=" << reader.crossChecks << ",cross_mismatches=" << reader.crossMismatches
                << ",with_shop_items=" << withShopItems << ",shop_item_states=" << shopStates
                << ",shop_items=" << shopItems << ",shop_items_with_state=" << shopWithState
                << ",reader=" << (replay::metaped::engine::ReaderOn() ? "on" : "off") << '\n';
        }
        log.flush();
        // The pose fidelity judge (PoseFidelity=1): this recording's engine
        // reads against the tracks, after StopAll and before any playback
        // samples them; last, so the lines above do not count its track reads.
        // Only the export and the hand-over to a
        // worker happen here (fidelity_us).
        const std::uint64_t fidelityStartedAt = replay::prof::Now();
        replay::pose::fidelity::Evaluate(log);
        {
            // The stop's parts. ev_*: replay::events::EndRecording by part
            // (events_end_us is their sum); records: the per-record loop
            // (snapshots, the lines handed to a worker); summary: the lines
            // from POSE_V2_POLL_EVIDENCE to POSE_V2_APPEARANCE_SUMMARY.
            const replay::events::EndTiming ev = replay::events::LastEndTiming();
            const std::uint64_t stopEndedAt = replay::prof::Now();
            log << "POSE_V2_STOP_TIMING,events_end_us=" << ElapsedUs(stopStartedAt, eventsEndedAt)
                << ",capture_stop_all_us=" << ElapsedUs(eventsEndedAt, captureStoppedAt)
                << ",objects_end_us=" << ElapsedUs(captureStoppedAt, objectsEndedAt)
                << ",ev_totals_us=" << ev.totalsUs << ",ev_flush_us=" << ev.flushUs << ",ev_decals_us=" << ev.decalsUs
                << ",ev_fires_us=" << ev.firesUs << ",ev_audit_us=" << ev.auditUs << ",ev_sounds_us=" << ev.soundsUs
                << ",ev_melee_us=" << ev.meleeUs << ",ev_tracks_us=" << ev.tracksUs << ",ev_speech_us=" << ev.speechUs
                << ",ev_tail_us=" << ev.tailUs
                << ",records=" << records << ",records_us=" << ElapsedUs(recordsStartedAt, recordsEndedAt)
                << ",summary_us=" << ElapsedUs(recordsEndedAt, fidelityStartedAt)
                << ",fidelity_us=" << ElapsedUs(fidelityStartedAt, stopEndedAt)
                << ",stop_us=" << ElapsedUs(stopStartedAt, stopEndedAt) << '\n';
        }

        const double seconds = static_cast<double>(elapsedMs) / 1000.0;
        char message[192]{};
        std::snprintf(
            message,
            sizeof(message),
            "PoseTrack v2 | %d entities | %.1f KB/s | hook %.0f us | error %.1f mm",
            records,
            seconds > 0.0 ? totalBytes / seconds / 1024.0 : 0.0,
            worstDetourUs,
            worstErrorM * 1000.0f);
        replay::hud::ShowObjective(message);
    }
}
