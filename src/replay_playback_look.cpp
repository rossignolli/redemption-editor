// replay_playback_look.cpp: the recorded look played: appearance, scorch, the vehicle look (windows, doors, parts, frags, burnt), burn level, fire char.
// Split from replay_playback.cpp (moved code); shared internals in replay_playback_internal.h.

#include "replay_playback_internal.h"

namespace replay::playback
{
    namespace
    {
    // The clip's vehicle pieces and their puppets' state.
    std::vector<replay::vehicle_pieces::PieceState> g_poseV2PieceStates;
    }

    // A shop dress of a shown puppet, checked once the engine rebuilt it (ready
    // to render, at most 2 s after the dress).
    void StepPoseV2ShopCheck(replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        const ULONGLONG now = GetTickCount64();
        if (!puppet.shopCheckAt || now < puppet.shopCheckAt || !puppet.clone)
            return;
        if (!replay::pose::playback::IsReadyToRender(puppet.clone) &&
            now - puppet.shopCheckSince < replay::playback::kDressTimeoutMs)
            return;
        puppet.shopCheckAt = 0;
        const replay::entity::appearance::Match match =
            replay::entity::appearance::Compare(puppet.clone, PoseV2Wear(puppet));
        if (!CheckPoseV2ShopDress(puppet, timeMs, "shown", match))
            return;
        // Dressed again by layers: the skeleton may change, as after an
        // appearance change (ApplyPoseV2Appearance).
        puppet.layerRetries = 0;
        puppet.nextLayerRetryAt = now + 600;
        puppet.noRecord.Reset();
        puppet.noRecordMapRefused = false;
        if (puppet.overrideSlot >= 0 && !puppet.mapped)
            ScheduleSignatureRecheck(puppet);
    }

    // The recorded appearance changes at the playhead on a ped or horse puppet
    // (appearance_track.h), in both directions: a scrub back before a change
    // puts the earlier layers back, a seek past one wears it. A pose segment
    // switch applies its own (SwitchPoseV2Segment, which also picks the change
    // in force); this acts only when the change at the playhead is not the one
    // worn. Layers are applied only when they differ from the puppet's:
    // applying them rebuilds the skeleton, which RefreshPoseV2Override re-arms,
    // and a bone count no record has retries the layers through
    // ArmPoseV2Puppet. A dropped hat still to be knocked off waits, like the
    // segment switch.
    void ApplyPoseV2Appearance(replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        // The metaped look follows the playhead (a change, a seek back before
        // one).
        if (!PoseV2KnockPending(puppet.packet.id))
            ApplyPoseV2Metaped(puppet, timeMs, false);
        if ((puppet.packet.kind != replay::entity::EntityKind::Ped &&
                puppet.packet.kind != replay::entity::EntityKind::Horse) ||
            Ctx().clip.appearances.empty())
            return;
        const replay::WantedAppearance wanted = PoseV2AppearanceAt(puppet, timeMs);
        if (wanted.key == puppet.appearanceKey || PoseV2KnockPending(puppet.packet.id))
            return;
        SetPoseV2Wear(puppet, *wanted.appearance);
        const bool layersApplied =
            !replay::entity::appearance::Compare(puppet.clone, *wanted.appearance).Exact();
        if (layersApplied)
        {
            LearnPoseV2Categories(puppet, *wanted.appearance);
            DressPoseV2Body(puppet, *wanted.appearance, timeMs, true); // shop items or layers
            ApplyPoseV2Metaped(puppet, timeMs, true);
            puppet.layerRetries = 0;
            puppet.nextLayerRetryAt = GetTickCount64() + 600;
            puppet.noRecord.Reset();
            puppet.noRecordMapRefused = false;
            if (puppet.overrideSlot >= 0 && !puppet.mapped)
                ScheduleSignatureRecheck(puppet);
        }
        const std::uint32_t previousKey = puppet.appearanceKey;
        puppet.appearanceKey = wanted.key;
        if (g_poseV2AppearancePlayLogs >= 64)
            return;
        ++g_poseV2AppearancePlayLogs;
        Ctx().log << "POSE_V2_APPEARANCE_CHANGE,phase=play,id=" << replay::entity::IdText(puppet.packet.id)
            << ",key=" << wanted.key << ",previous_key=" << previousKey
            << ",layers=" << wanted.appearance->assets.size() << ",layers_applied=" << (layersApplied ? 1 : 0)
            << ",playhead_ms=" << timeMs << '\n';
    }

    // The recorded charred state at the playhead on a ped or horse puppet,
    // written when it changes (both directions: a scrub back before the change
    // takes the black off). The fire replayed on the puppet can make the engine
    // set the bit itself: once a second the bit is read back and written again
    // when it differs (engine_set=1).
    void ApplyPoseV2Scorched(replay::playback::Puppet& puppet, std::uint32_t timeMs)
    {
        // Vehicle puppets too (a burnt wagon).
        if (puppet.packet.kind != replay::entity::EntityKind::Ped && puppet.packet.kind != replay::entity::EntityKind::Horse &&
            puppet.packet.kind != replay::entity::EntityKind::Vehicle)
            return;
        const auto track = std::find_if(Ctx().clip.scorched.begin(), Ctx().clip.scorched.end(),
            [&](const replay::ScorchTrack& entry) { return entry.id == puppet.packet.id.Packed(); });
        const bool wanted = track != Ctx().clip.scorched.end() && replay::ScorchedAt(*track, timeMs);
        bool engineSet = false;
        const ULONGLONG now = GetTickCount64();
        if (puppet.scorchedApplied == (wanted ? 1 : 0))
        {
            if (now < puppet.scorchCheckAt)
                return;
            puppet.scorchCheckAt = now + 1000;
            const bool actual = (static_cast<std::uint32_t>(ENTITY::_0x37B01666BAE8F7EF(puppet.clone)) & 1u) != 0;
            if (actual == wanted)
                return;
            engineSet = true;
        }
        ENTITY::SET_ENTITY_RENDER_SCORCHED(puppet.clone, wanted ? TRUE : FALSE);
        const bool first = puppet.scorchedApplied < 0;
        puppet.scorchedApplied = wanted ? 1 : 0;
        puppet.scorchCheckAt = now + 1000;
        // A new puppet without the bit needs no line.
        if ((first && !wanted) || g_poseV2ScorchPlayLogs >= 64)
            return;
        ++g_poseV2ScorchPlayLogs;
        Ctx().log << "POSE_V2_SCORCHED,phase=play,id=" << replay::entity::IdText(puppet.packet.id)
            << ",scorched=" << (wanted ? 1 : 0) << ",engine_set=" << (engineSet ? 1 : 0) << ",playhead_ms=" << timeMs << '\n';
    }

    // A vehicle puppet's paint and livery compared with the recorded ones after
    // it has rendered (500 ms and 2 s after the create) and set again when the
    // engine changed them, the variation data reapplied until it is verified.
    // Evidence in POSE_V2_VEHICLE_LOOK,phase=verify.
    void VerifyPoseV2VehicleLook(std::uint32_t timeMs)
    {
        const ULONGLONG now = GetTickCount64();
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind != replay::entity::EntityKind::Vehicle || !puppet.lookCheckAt ||
                now < puppet.lookCheckAt)
                continue;
            if (puppet.state != replay::playback::PuppetState::Ready || !puppet.clone ||
                puppet.clone != puppet.lookClone || !replay::entities::Exists(puppet.clone))
            {
                puppet.lookCheckAt = 0;
                continue;
            }
            const int tint = VEHICLE::_GET_VEHICLE_TINT(puppet.clone);
            const int livery = VEHICLE::_GET_VEHICLE_LIVERY(puppet.clone);
            const bool tintOff = puppet.lookTint >= 0 && tint != puppet.lookTint;
            const bool liveryOff = puppet.lookLivery >= 0 && livery != puppet.lookLivery;
            if (tintOff)
                VEHICLE::_SET_VEHICLE_TINT(puppet.clone, puppet.lookTint);
            if (liveryOff)
                VEHICLE::_SET_VEHICLE_LIVERY(puppet.clone, puppet.lookLivery);
            ++puppet.lookChecks;
            puppet.lookCheckAt = puppet.lookChecks < 2 ? now + 1500 : 0;
            if (g_poseV2LookVerifyLogs < 64 && (tintOff || liveryOff || puppet.lookChecks == 1))
            {
                ++g_poseV2LookVerifyLogs;
                Ctx().log << "POSE_V2_VEHICLE_LOOK,phase=verify,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",check=" << puppet.lookChecks
                    << ",tint=" << puppet.lookTint << ",tint_seen=" << tint
                    << ",livery=" << puppet.lookLivery << ",livery_seen=" << livery
                    << ",reapplied=" << ((tintOff || liveryOff) ? 1 : 0)
                    << ",tint_after=" << VEHICLE::_GET_VEHICLE_TINT(puppet.clone)
                    << ",playhead_ms=" << timeMs << '\n';
            }
        }
    }

    // The recorded broken windows at the playhead on a vehicle puppet. A window
    // broken since is removed (REMOVE_VEHICLE_WINDOW); back before a break the
    // puppet is rebuilt whole through the Loading path (no native puts the
    // glass back, as the frag rewind does) and the windows up to the playhead
    // come again.
    void PlayPoseV2VehicleWindows(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.veh_windows");
        if (Ctx().clip.vehicleWindows.empty())
            return;
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind != replay::entity::EntityKind::Vehicle ||
                puppet.state != replay::playback::PuppetState::Ready || !puppet.clone || !replay::entities::Exists(puppet.clone))
                continue;
            const auto track = std::find_if(Ctx().clip.vehicleWindows.begin(), Ctx().clip.vehicleWindows.end(),
                [&](const replay::WindowTrack& entry) { return entry.id == puppet.packet.id.Packed(); });
            if (track == Ctx().clip.vehicleWindows.end())
                continue;
            if (puppet.windowsClone != puppet.clone)
            {
                puppet.windowsClone = puppet.clone;
                puppet.windowsApplied = 0;
            }
            const std::uint8_t wanted = replay::BrokenWindowsAt(*track, timeMs);
            if (wanted == puppet.windowsApplied)
                continue;
            if (puppet.windowsApplied & ~wanted)
            {
                Ctx().log << "POSE_V2_VEHICLE_WINDOWS,phase=rebuild,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",applied=0x" << std::hex << static_cast<unsigned>(puppet.windowsApplied)
                    << ",wanted=0x" << static_cast<unsigned>(wanted) << std::dec << ",playhead_ms=" << timeMs << '\n';
                ReleaseVehicleForRebuild(puppet);
                puppet.clone = 0;
                puppet.windowsClone = 0;
                puppet.windowsApplied = 0;
                puppet.scorchedApplied = -1;
                puppet.state = replay::playback::PuppetState::Loading;
                puppet.stateSince = GetTickCount64();
                continue;
            }
            const std::uint8_t added = static_cast<std::uint8_t>(wanted & ~puppet.windowsApplied);
            for (int window = 0; window < replay::kVehicleWindows; ++window)
            {
                if (added & (1u << window))
                    VEHICLE::REMOVE_VEHICLE_WINDOW(puppet.clone, window);
            }
            puppet.windowsApplied = wanted;
            if (g_poseV2WindowPlayLogs < 64)
            {
                ++g_poseV2WindowPlayLogs;
                std::uint8_t intact = 0;
                for (int window = 0; window < replay::kVehicleWindows; ++window)
                {
                    if (VEHICLE::IS_VEHICLE_WINDOW_INTACT(puppet.clone, window))
                        intact |= static_cast<std::uint8_t>(1u << window);
                }
                Ctx().log << "POSE_V2_VEHICLE_WINDOWS,phase=play,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",broken=0x" << std::hex << static_cast<unsigned>(wanted)
                    << ",intact_after=0x" << static_cast<unsigned>(intact) << std::dec << ",playhead_ms=" << timeMs << '\n';
            }
        }
    }

    // The recorded door ratios at the playhead on every vehicle puppet (the
    // train cars in replay_trains), driven and read back
    // (vehicle_doors_engine.h).
    void PlayPoseV2VehicleDoors(std::uint32_t timeMs)
    {
        if (Ctx().clip.vehicleDoors.empty())
            return;
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind != replay::entity::EntityKind::Vehicle ||
                puppet.state != replay::playback::PuppetState::Ready || !puppet.clone)
                continue;
            if (const replay::vehdoors::DoorTrack* track =
                    replay::vehdoors::TrackOf(Ctx().clip.vehicleDoors, puppet.packet.id.Packed()))
                replay::vehdoors::Apply(puppet.clone, *track, timeMs, Ctx().log);
        }
    }

    // The recorded lost wheels and doors at the playhead on a vehicle puppet
    // (VehiclePartTrack), with the natives the game's own scripts use on
    // wagons: _BREAK_OFF_VEHICLE_WHEEL (bounds-checked by the engine: an index
    // the model lacks does nothing) and SET_VEHICLE_DOOR_BROKEN with deletion.
    // The wheel piece the native returns is deleted (the recorded piece is not
    // part of the clip). Back before a loss the puppet is rebuilt whole through
    // the Loading path, as the windows do. The object-only fragment native is
    // never used on a vehicle.
    void PlayPoseV2VehicleParts(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.veh_parts");
        if (Ctx().clip.vehicleParts.empty())
            return;
        bool pieceStatesRead = false;
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind != replay::entity::EntityKind::Vehicle ||
                puppet.state != replay::playback::PuppetState::Ready || !puppet.clone || !replay::entities::Exists(puppet.clone) ||
                !ENTITY::IS_ENTITY_A_VEHICLE(puppet.clone))
                continue;
            const auto track = std::find_if(Ctx().clip.vehicleParts.begin(), Ctx().clip.vehicleParts.end(),
                [&](const replay::VehiclePartTrack& entry) { return entry.id == puppet.packet.id.Packed(); });
            if (track == Ctx().clip.vehicleParts.end())
                continue;
            if (puppet.partsClone != puppet.clone)
            {
                puppet.partsClone = puppet.clone;
                puppet.partsApplied = 0;
            }
            // The wheels the clip's wheel pieces took are off too (a piece may
            // be born a sample before the 4 Hz part track; vehicle_pieces.h),
            // and the wheel models of the puppet are known to the object
            // puppets.
            replay::objects::NoteVehiclePuppetWheels(puppet.clone);
            std::uint16_t pieceWheels = 0;
            if (replay::objects::ClipHasWheelPieces())
            {
                if (!pieceStatesRead)
                {
                    replay::objects::VehiclePieceStates(g_poseV2PieceStates);
                    pieceStatesRead = true;
                }
                pieceWheels = replay::vehicle_pieces::WheelPieceBits(g_poseV2PieceStates, puppet.packet.id, timeMs);
            }
            // A recorded wheel child attached at the playhead keeps its wheel on
            // the puppet (vehicle_pieces.h wheel children).
            const std::uint16_t recorded = replay::BrokenPartsAt(*track, timeMs);
            const std::uint16_t keptWheels = replay::objects::KeptVehicleWheels(puppet, timeMs);
            // A wheel whose record left the vehicle came off as it was
            // (LeftVehicleWheels): never through the part native.
            const std::uint16_t leftWheels = replay::objects::LeftVehicleWheels(puppet, timeMs);
            const std::uint16_t wanted = replay::vehicle_pieces::PartsWanted(recorded, pieceWheels, keptWheels, leftWheels);
            std::uint16_t& keptLogged = g_poseV2PartKeptLogged[puppet.packet.id.Packed()];
            if ((recorded & keptWheels) && keptLogged != (recorded & keptWheels))
            {
                keptLogged = static_cast<std::uint16_t>(recorded & keptWheels);
                Ctx().log << "POSE_V2_VEHICLE_PARTS,phase=kept,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",recorded_wheels_off=0x" << std::hex << (recorded & ((1u << replay::kVehiclePartWheels) - 1))
                    << ",wheels_kept=0x" << (recorded & keptWheels) << std::dec << ",playhead_ms=" << timeMs << '\n';
            }
            // Back before a piece broke off one of its wheels: built whole again.
            const bool piecesAhead = replay::objects::VehicleWheelPiecesAhead(puppet.packet.id, timeMs);
            if (wanted == puppet.partsApplied && !piecesAhead)
                continue;
            if ((puppet.partsApplied & ~wanted) || piecesAhead)
            {
                Ctx().log << "POSE_V2_VEHICLE_PARTS,phase=rebuild,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",wheel_pieces_ahead=" << (piecesAhead ? 1 : 0)
                    << ",applied=0x" << std::hex << puppet.partsApplied << ",wanted=0x" << wanted << std::dec
                    << ",playhead_ms=" << timeMs << '\n';
                ReleaseVehicleForRebuild(puppet);
                puppet.clone = 0;
                puppet.partsClone = 0;
                puppet.partsApplied = 0;
                puppet.windowsClone = 0;
                puppet.windowsApplied = 0;
                puppet.scorchedApplied = -1;
                puppet.state = replay::playback::PuppetState::Loading;
                puppet.stateSince = GetTickCount64();
                continue;
            }
            // A recorded piece of this vehicle due now and not made yet breaks
            // its wheel or door off first.
            replay::objects::VehiclePieceStates(g_poseV2PieceStates);
            if (replay::vehicle_pieces::DeferPartNatives(g_poseV2PieceStates, puppet.packet.id, timeMs))
                continue;
            const std::uint16_t added = static_cast<std::uint16_t>(wanted & ~puppet.partsApplied);
            int pieces = 0;
            int kept = 0;
            for (int wheel = 0; wheel < replay::kVehiclePartWheels; ++wheel)
            {
                if (!(added & (1u << wheel)))
                    continue;
                Entity piece = VEHICLE::_BREAK_OFF_VEHICLE_WHEEL(puppet.clone, wheel);
                if (piece && piece != puppet.clone && replay::entities::Exists(piece) && ENTITY::IS_ENTITY_AN_OBJECT(piece))
                {
                    // Kept (hidden) when the clip has a use for it: its wheel
                    // piece, or an object puppet of its model (the carried
                    // wheel).
                    if (replay::objects::KeepVehicleWheel(puppet.packet.id, wheel, piece, timeMs))
                    {
                        ++kept;
                        continue;
                    }
                    ENTITY::SET_ENTITY_AS_MISSION_ENTITY(piece, TRUE, TRUE);
                    ENTITY::DELETE_ENTITY(&piece);
                    ++pieces;
                }
            }
            for (int door = 0; door < replay::kVehiclePartDoors; ++door)
            {
                if (added & (1u << (replay::kVehiclePartDoorShift + door)))
                    VEHICLE::SET_VEHICLE_DOOR_BROKEN(puppet.clone, door, TRUE);
            }
            puppet.partsApplied = wanted;
            if (g_poseV2PartPlayLogs < 64)
            {
                ++g_poseV2PartPlayLogs;
                unsigned doorsAfter = 0;
                for (int door = 0; door < replay::kVehiclePartDoors; ++door)
                {
                    if (VEHICLE::_IS_VEHICLE_DOOR_BROKEN(puppet.clone, door))
                        doorsAfter |= 1u << door;
                }
                Ctx().log << "POSE_V2_VEHICLE_PARTS,phase=play,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",wheels_off=0x" << std::hex << (wanted & ((1u << replay::kVehiclePartWheels) - 1))
                    << ",doors_off=0x" << (static_cast<unsigned>(wanted) >> replay::kVehiclePartDoorShift)
                    << ",doors_off_after=0x" << doorsAfter << std::dec
                    << ",any_wheel_off_after=" << (VEHICLE::_ARE_ANY_VEHICLE_WHEELS_DESTROYED(puppet.clone) ? 1 : 0)
                    << ",pieces_deleted=" << pieces << ",pieces_kept=" << kept << ",piece_wheels=0x" << std::hex << pieceWheels
                    << std::dec << ",playhead_ms=" << timeMs << '\n';
            }
        }
    }

    // The fragment groups a recorded vehicle lost (boards, canvas, cargo, the
    // wreck an explosion leaves: FragBreak events of the vehicle,
    // RecordVehicleFragBreak) taken off its puppet up to the playhead through
    // the engine's DeleteAboveGroup (replay::frag::DeleteVehicleGroup:
    // signature, fragInst owned by the vehicle, group in range, never the
    // root). Wheel and door groups are left to PlayPoseV2VehicleParts' natives.
    // Back before a break the puppet is rebuilt whole through the Loading path,
    // as the windows and parts do. BREAK_OBJECT_FRAGMENT_CHILD is never used on
    // a vehicle (its getter is object-only).
    void PlayPoseV2VehicleFrags(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.veh_frags");
        if (Ctx().clip.fragBreaks.empty() || !replay::frag::VehicleGroupsSupported())
            return;
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind != replay::entity::EntityKind::Vehicle ||
                puppet.state != replay::playback::PuppetState::Ready || !puppet.clone || !replay::entities::Exists(puppet.clone) ||
                !ENTITY::IS_ENTITY_A_VEHICLE(puppet.clone))
                continue;
            if (puppet.fragClone != puppet.clone)
            {
                puppet.fragClone = puppet.clone;
                puppet.fragApplied = 0;
            }
            std::uint32_t due = 0;
            for (const replay::FragBreak& e : Ctx().clip.fragBreaks)
                if (e.id == puppet.packet.id && e.timeMs <= timeMs)
                    ++due;
            if (due == puppet.fragApplied)
                continue;
            if (due < puppet.fragApplied)
            {
                ++g_poseV2VehicleFragRebuilds;
                Ctx().log << "POSE_V2_VEHICLE_FRAG,phase=rebuild,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",applied=" << puppet.fragApplied << ",due=" << due << ",playhead_ms=" << timeMs << '\n';
                ReleaseVehicleForRebuild(puppet);
                puppet.clone = 0;
                puppet.fragClone = 0;
                puppet.fragApplied = 0;
                puppet.partsClone = 0;
                puppet.partsApplied = 0;
                puppet.windowsClone = 0;
                puppet.windowsApplied = 0;
                puppet.scorchedApplied = -1;
                puppet.state = replay::playback::PuppetState::Loading;
                puppet.stateSince = GetTickCount64();
                continue;
            }
            const std::uint64_t address = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone));
            // The groups of the vehicle's recorded pieces are theirs to break
            // off (vehicle_pieces.h).
            replay::objects::VehiclePieceStates(g_poseV2PieceStates);
            std::uint16_t reserved[64];
            const int reservedCount = replay::vehicle_pieces::ReservedRoots(
                g_poseV2PieceStates, puppet.packet.id, timeMs, reserved, 64);
            // A recorded piece's group comes off as that piece at its own break,
            // before the groups above it go in the same tick (deleting a parent
            // takes the children with it; the recorder found the piece's object
            // a few ticks after the break).
            std::uint32_t pieceIndex = 0;
            for (const replay::FragBreak& e : Ctx().clip.fragBreaks)
            {
                if (!(e.id == puppet.packet.id) || e.timeMs > timeMs || pieceIndex++ < puppet.fragApplied ||
                    !replay::vehicle_pieces::IsUnmadePieceGroup(g_poseV2PieceStates, puppet.packet.id, e.group))
                    continue;
                std::uint64_t entity = 0;
                if (replay::frag::BreakOffVehicleGroup(address, e.group, entity) == replay::frag::VehicleGroupResult::Deleted)
                    replay::objects::KeepVehicleFragPiece(puppet.packet.id, e.group, entity, timeMs);
            }
            std::uint32_t index = 0;
            std::uint32_t counts[replay::frag::kVehicleGroupResults] = {};
            int firstGroup = -1;
            for (const replay::FragBreak& e : Ctx().clip.fragBreaks)
            {
                if (!(e.id == puppet.packet.id) || e.timeMs > timeMs)
                    continue;
                if (index++ < puppet.fragApplied)
                    continue;
                const replay::frag::VehicleGroupResult result = replay::frag::DeleteVehicleGroup(address, e.group, reserved, reservedCount);
                const int slot = static_cast<int>(result);
                if (slot >= 0 && slot < replay::frag::kVehicleGroupResults)
                {
                    ++counts[slot];
                    ++g_poseV2VehicleFragResults[slot];
                }
                if (firstGroup < 0)
                    firstGroup = e.group;
                // A vehicle without a usable fragInst now will not have one
                // later in this tick either.
                if (result == replay::frag::VehicleGroupResult::NoInst || result == replay::frag::VehicleGroupResult::Fault ||
                    result == replay::frag::VehicleGroupResult::Unsupported)
                    break;
            }
            puppet.fragApplied = due;
            if (g_poseV2VehicleFragLogs < 64)
            {
                ++g_poseV2VehicleFragLogs;
                Ctx().log << "POSE_V2_VEHICLE_FRAG,phase=play,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",events=" << due << ",first_group=" << firstGroup;
                for (int slot = 0; slot < replay::frag::kVehicleGroupResults; ++slot)
                    if (counts[slot])
                        Ctx().log << ',' << replay::frag::VehicleGroupResultText(static_cast<replay::frag::VehicleGroupResult>(slot))
                            << '=' << counts[slot];
                Ctx().log << ",playhead_ms=" << timeMs << '\n';
            }
        }
    }

    // The burnt state of a vehicle puppet. The fire replayed on the puppet
    // darkens it by itself in normal play (the engine's burn level,
    // SamplePoseV2VehicleBurnt), and that level never goes back: a scrub back
    // to before the burn would leave the wagon black. The playhead where the
    // puppet was first seen on fire or charred is kept; back before it, when
    // the recorded track does not have the vehicle burnt there, the puppet is
    // rebuilt clean through the Loading path (no native lowers the burn level),
    // as the windows and parts do. Old clips without the track rebuild the same
    // way.
    void PlayPoseV2VehicleBurnt(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.veh_burnt");
        constexpr std::uint32_t kNotSeen = 0xFFFFFFFFu;
        constexpr std::uint32_t kSlackMs = 250;
        const ULONGLONG now = GetTickCount64();
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind != replay::entity::EntityKind::Vehicle ||
                puppet.state != replay::playback::PuppetState::Ready || !puppet.clone || !replay::entities::Exists(puppet.clone))
                continue;
            if (puppet.burntClone != puppet.clone)
            {
                puppet.burntClone = puppet.clone;
                puppet.burntSeenMs = kNotSeen;
                puppet.burntCheckAt = 0;
            }
            if (puppet.burntSeenMs == kNotSeen)
            {
                if (now < puppet.burntCheckAt)
                    continue;
                puppet.burntCheckAt = now + 500;
                const bool onFire = FIRE::IS_ENTITY_ON_FIRE(puppet.clone) != FALSE;
                const bool charred = (static_cast<std::uint32_t>(ENTITY::_0x37B01666BAE8F7EF(puppet.clone)) & 1u) != 0;
                if (!onFire && !charred)
                    continue;
                puppet.burntSeenMs = timeMs;
                if (g_poseV2BurntPlayLogs < 64)
                {
                    ++g_poseV2BurntPlayLogs;
                    Ctx().log << "POSE_V2_VEHICLE_BURNT,phase=play,id=" << replay::entity::IdText(puppet.packet.id)
                        << ",on_fire=" << (onFire ? 1 : 0) << ",charred=" << (charred ? 1 : 0)
                        << ",playhead_ms=" << timeMs << '\n';
                }
                continue;
            }
            if (timeMs + kSlackMs >= puppet.burntSeenMs)
                continue;
            // A vehicle with a FREC track needs no rebuild: the black body is
            // the fire record (the vehicle branch of the fire renderer looks
            // the record up the same way as for peds) and PlayPoseV2FireChar
            // removes it back before the recorded fire.
            if (replay::burn::CharEnabled() && replay::FireCharKept(Ctx().clip.fireChars, puppet.packet.id.Packed()))
                continue;
            const auto track = std::find_if(Ctx().clip.vehicleBurnt.begin(), Ctx().clip.vehicleBurnt.end(),
                [&](const replay::ScorchTrack& entry) { return entry.id == puppet.packet.id.Packed(); });
            if (track != Ctx().clip.vehicleBurnt.end() && replay::ScorchedAt(*track, timeMs))
                continue;
            Ctx().log << "POSE_V2_VEHICLE_BURNT,phase=rebuild,id=" << replay::entity::IdText(puppet.packet.id)
                << ",seen_ms=" << puppet.burntSeenMs
                << ",recorded=" << (track != Ctx().clip.vehicleBurnt.end() ? 1 : 0)
                << ",playhead_ms=" << timeMs << '\n';
            ReleaseVehicleForRebuild(puppet);
            puppet.clone = 0;
            puppet.burntClone = 0;
            puppet.burntSeenMs = kNotSeen;
            puppet.partsClone = 0;
            puppet.partsApplied = 0;
            puppet.windowsClone = 0;
            puppet.windowsApplied = 0;
            puppet.scorchedApplied = -1;
            puppet.state = replay::playback::PuppetState::Loading;
            puppet.stateSince = now;
        }
    }

    // The recorded burn level at the playhead on every vehicle, ped and horse
    // puppet with a track (BurnLevelTrack), written straight into the puppet's
    // effect blocks (burn_level.h) whenever the puppet's own value differs.
    // Back before a burn it lowers the level, forward past one it darkens a
    // clean puppet (a rebuilt one included), and a ped or horse puppet,
    // invincible, gets the level the replayed fire cannot give it. The fire
    // replayed on a vehicle puppet writes the level too while it burns; the
    // track wins on the next frame (engine_changed). Also on the weapon object
    // and prop puppets (BurnLevelPlayable; ini ObjectBurnLevel): the cooked
    // meat, the campfire's logs. Their objects are the replay's own (created,
    // or taken from the engine during the playback and deleted at its end), so
    // nothing is given back.
    void PlayPoseV2BurnLevel(std::uint32_t timeMs)
    {
        if (Ctx().clip.burnLevels.empty() || !replay::burn::Enabled())
            return;
        constexpr float kSame = 0.001f;
        const bool objects = replay::objects::ObjectBurnLevel();
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            const replay::entity::EntityKind kind = puppet.packet.kind;
            const bool object = kind == replay::entity::EntityKind::Weapon || kind == replay::entity::EntityKind::Prop;
            if (!replay::BurnLevelPlayable(kind) || (object && !objects) ||
                puppet.state != replay::playback::PuppetState::Ready || !puppet.clone || !replay::entities::Exists(puppet.clone))
                continue;
            const auto track = std::find_if(Ctx().clip.burnLevels.begin(), Ctx().clip.burnLevels.end(),
                [&](const replay::BurnLevelTrack& entry) { return entry.id == puppet.packet.id.Packed(); });
            if (track == Ctx().clip.burnLevels.end() || track->states.empty())
                continue;
            if (puppet.burnLevelClone != puppet.clone)
            {
                puppet.burnLevelClone = puppet.clone;
                puppet.burnLevelHas = false;
            }
            // A file's value never reaches the engine unbounded.
            const float level = replay::BurnLevelAt(*track, timeMs);
            const float wanted = level == level ? std::clamp(level, 0.0f, 1.0f) : 0.0f;
            const void* address = replay::entities::AddressOf(puppet.clone);
            replay::burn::State state;
            if (!replay::burn::Read(address, state))
                continue;
            const bool fireSame = !state.fireBlock || std::fabs(state.level - wanted) < kSame;
            const bool pedSame = !state.pedBlock || std::fabs(state.pedLevel - wanted) < kSame;
            if (fireSame && pedSame)
                continue;
            const bool engineChanged = puppet.burnLevelHas && std::fabs(wanted - puppet.burnLevelWritten) < kSame;
            if (engineChanged)
                ++g_poseV2BurnLevelEngineChanged;
            const float before = state.fireBlock ? state.level : state.pedLevel;
            const int blocks = replay::burn::Write(address, wanted);
            puppet.burnLevelWritten = wanted;
            puppet.burnLevelHas = blocks > 0;
            ++g_poseV2BurnLevelWrites;
            if (g_poseV2BurnLevelPlayLogs < 64 && !engineChanged)
            {
                ++g_poseV2BurnLevelPlayLogs;
                Ctx().log << "POSE_V2_BURN_LEVEL,phase=play,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",kind=" << static_cast<int>(kind) // 3 weapon object, 4 prop
                    << ",from=" << before << ",to=" << wanted
                    << ",blocks=" << blocks
                    << ",fire_block=" << (state.fireBlock ? 1 : 0) << ",ped_block=" << (state.pedBlock ? 1 : 0)
                    << std::hex << ",fire_vt=0x" << state.fireVtableRva << ",ped_vt=0x" << state.pedVtableRva << std::dec
                    << ",on_fire=" << (FIRE::IS_ENTITY_ON_FIRE(puppet.clone) ? 1 : 0)
                    << ",writes=" << g_poseV2BurnLevelWrites
                    << ",engine_changed=" << g_poseV2BurnLevelEngineChanged
                    << ",playhead_ms=" << timeMs << '\n';
            }
        }
    }

    // The recorded fire record (FireCharTrack, the burnt look) on every puppet
    // with a track, checked 6 times a second (wall clock, so pause and scrub
    // too). No record at the playhead: the puppet's own is removed (not while
    // it burns). A record: the recorded volumes are written into the puppet's
    // own (made by the replayed fire, or by a short fire of ours put out on the
    // next check).
    void PlayPoseV2FireChar(std::uint32_t timeMs)
    {
        if (Ctx().clip.fireChars.empty() || !replay::burn::CharEnabled())
            return;
        REPLAY_PROFILE_ZONE("play.fire_char");
        const std::uint64_t now = GetTickCount64();
        // A scrub (the transport scrubbing, the playhead going back or jumping
        // more than wall time allows) makes the
        // fire track rebuild its fires and clear the area often: no fire of
        // ours then, and the per puppet budget starts again once it settles.
        static std::uint32_t lastPlayhead = 0;
        static std::uint64_t lastWall = 0;
        static std::uint64_t settledAt = 0;
        const std::uint64_t wallStep = now - lastWall;
        const std::uint32_t headStep = timeMs > lastPlayhead ? timeMs - lastPlayhead : lastPlayhead - timeMs;
        if (lastWall == 0 || headStep > wallStep * 2 + 100 || Ctx().transport.Scrubbing() || timeMs < lastPlayhead)
            settledAt = now + 400;
        lastPlayhead = timeMs;
        lastWall = now;
        const bool settled = now >= settledAt;
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.state != replay::playback::PuppetState::Ready || !puppet.clone || !replay::entities::Exists(puppet.clone))
                continue;
            if (puppet.fireCharClone != puppet.clone)
            {
                puppet.fireCharClone = puppet.clone;
                puppet.fireCharCheckAt = 0;
                puppet.fireCharIgnitedAt = 0;
                puppet.fireCharIgnites = 0;
                puppet.fireCharWrites = 0;
                puppet.fireCharHad = false;
            }
            if (!settled)
                puppet.fireCharIgnites = 0;
            // A short fire of ours is put out as soon as normal play stops
            // (pause, scrub), not at the next check.
            const bool normalPlay = PoseV2NormalPlay();
            if (now < puppet.fireCharCheckAt && !(puppet.fireCharIgnitedAt && !normalPlay))
                continue;
            const auto track = std::find_if(Ctx().clip.fireChars.begin(), Ctx().clip.fireChars.end(),
                [&](const replay::FireCharTrack& entry) { return entry.id == puppet.packet.id.Packed(); });
            if (track == Ctx().clip.fireChars.end())
                continue;
            const replay::FireCharState* wanted = replay::FireCharAt(*track, timeMs);
            if (!wanted)
                continue;
            puppet.fireCharCheckAt = now + 160;
            void* address = replay::entities::AddressOf(puppet.clone);
            replay::burn::CharState actual;
            if (!replay::burn::ReadChar(address, actual))
                continue;
            const bool onFire = FIRE::IS_ENTITY_ON_FIRE(puppet.clone) != FALSE;
            const char* action = nullptr;
            if (puppet.fireCharHad && !actual.present && g_poseV2FireCharPlayLogs < 96)
            {
                // Evidence: the engine or a fire clear took the record.
                ++g_poseV2FireCharPlayLogs;
                Ctx().log << "POSE_V2_FIRE_CHAR,phase=lost,id=" << replay::entity::IdText(puppet.packet.id)
                    << ",wanted=" << (wanted->present ? 1 : 0) << std::hex << ",wanted_flags=0x" << int(wanted->flags) << std::dec
                    << ",on_fire=" << (onFire ? 1 : 0) << ",dead=" << (ENTITY::IS_ENTITY_DEAD(puppet.clone) ? 1 : 0)
                    << ",faults=" << replay::burn::CharFaults() << ",playhead_ms=" << timeMs << '\n';
            }
            puppet.fireCharHad = actual.present;
            // Our short fire: out on the next check (at least 250 ms later),
            // whatever the record did (never left burning: a fire left on a
            // puppet leaks into later recordings).
            if (puppet.fireCharIgnitedAt && (now - puppet.fireCharIgnitedAt >= 250 || !normalPlay))
            {
                FIRE::STOP_ENTITY_FIRE(puppet.clone, 0);
                puppet.fireCharIgnitedAt = 0;
                if (g_poseV2FireCharPlayLogs < 96)
                {
                    ++g_poseV2FireCharPlayLogs;
                    Ctx().log << "POSE_V2_FIRE_CHAR,phase=ignite_done,id=" << replay::entity::IdText(puppet.packet.id)
                        << ",record=" << (actual.present ? 1 : 0) << ",on_fire=" << (onFire ? 1 : 0)
                        << ",dead=" << (ENTITY::IS_ENTITY_DEAD(puppet.clone) ? 1 : 0) << ",playhead_ms=" << timeMs << '\n';
                }
            }
            else if (puppet.fireCharIgnitedAt)
                continue;
            if (!wanted->present)
            {
                if (actual.present && !onFire)
                {
                    std::uint32_t code = 0;
                    const bool removed = replay::burn::RemoveChar(address, code);
                    ++g_poseV2FireCharRemoves;
                    action = removed ? "remove" : "remove_failed";
                }
            }
            else if (actual.present)
            {
                // Part by part (FREC v2 anchors): each recorded volume goes
                // into the puppet's volume on the same part of the entity, a
                // missing part gets a volume made by the engine's own copy, the
                // puppet's other parts are neutralized. Written every pass (the
                // engine moves a live record every frame), so the same playhead
                // always gives the same look. v1 clips (no anchors):
                // index-wise, no creation.
                const bool anchored = !wanted->anchors.empty() && wanted->anchors.size() == wanted->volumes.size();
                replay::burn::CharState recorded;
                recorded.present = true;
                recorded.flags = wanted->flags;
                for (std::size_t n = 0; n < wanted->volumes.size(); ++n)
                {
                    replay::burn::CharVolume volume;
                    std::copy(wanted->volumes[n].begin(), wanted->volumes[n].end(), volume.v);
                    if (anchored)
                    {
                        const replay::FireCharAnchor& anchor = wanted->anchors[n];
                        volume.component = anchor.component;
                        volume.sub = anchor.sub;
                        volume.flags = anchor.flags;
                        volume.mask = anchor.mask;
                        std::copy(std::begin(anchor.dir), std::end(anchor.dir), volume.dir);
                    }
                    recorded.volumes.push_back(volume);
                }
                std::vector<int> match(wanted->volumes.size(), -1);
                if (anchored)
                {
                    std::vector<replay::FireCharAnchor> parts;
                    for (const replay::burn::CharVolume& volume : actual.volumes)
                    {
                        replay::FireCharAnchor part;
                        part.component = volume.component;
                        part.sub = volume.sub;
                        parts.push_back(part);
                    }
                    match = replay::MatchFireCharVolumes(wanted->anchors, parts);
                }
                else
                    for (std::size_t n = 0; n < match.size() && n < actual.volumes.size(); ++n)
                        match[n] = static_cast<int>(n);
                const int missing = static_cast<int>(std::count(match.begin(), match.end(), -1));
                if (missing || actual.volumes.size() != wanted->volumes.size())
                {
                    ++g_poseV2FireCharShapeDiffers;
                    if (g_poseV2FireCharPlayLogs < 96 && puppet.fireCharWrites % 64 == 0)
                    {
                        ++g_poseV2FireCharPlayLogs;
                        Ctx().log << "POSE_V2_FIRE_CHAR,phase=shape_differs,id=" << replay::entity::IdText(puppet.packet.id)
                            << std::hex << ",flags=0x" << int(actual.flags) << ",wanted_flags=0x" << int(wanted->flags) << std::dec
                            << ",volumes=" << actual.volumes.size() << ",wanted_volumes=" << wanted->volumes.size()
                            << ",parts_missing=" << missing << ",anchored=" << (anchored ? 1 : 0)
                            << ",playhead_ms=" << timeMs << '\n';
                    }
                }
                const replay::burn::CharWriteReport report = replay::burn::WriteChar(address, recorded, match, anchored);
                g_poseV2FireCharCreated += static_cast<std::uint64_t>(report.created);
                g_poseV2FireCharCreateFailed += static_cast<std::uint64_t>(report.createFailed);
                if (report.written > 0)
                {
                    ++g_poseV2FireCharWrites;
                    // A line for the first write per puppet, every 64th and
                    // every one that made volumes.
                    if (puppet.fireCharWrites++ % 64 == 0 || report.created || report.createFailed)
                        action = report.created ? "write_created" : (report.createFailed ? "write_create_failed" : "write");
                }
            }
            else if (!onFire && !wanted->anchors.empty() && wanted->anchors.size() == wanted->volumes.size() &&
                replay::burn::CharCreateEnabled() && puppet.fireCharIgnites < 8)
            {
                // No record on the puppet (recreated by a frag or parts
                // rebuild, or scrubbed past a fire that ended): made without a
                // fire (CreateChar), then the recorded parts are written into
                // it in the same pass. No flame, so in pause and scrub too (the
                // normal-play rule is about fire particles).
                std::uint32_t code = 0;
                ++puppet.fireCharIgnites;
                if (replay::burn::CreateChar(address, code))
                {
                    replay::burn::CharState recorded;
                    recorded.present = true;
                    recorded.flags = wanted->flags;
                    for (std::size_t n = 0; n < wanted->volumes.size(); ++n)
                    {
                        replay::burn::CharVolume volume;
                        std::copy(wanted->volumes[n].begin(), wanted->volumes[n].end(), volume.v);
                        const replay::FireCharAnchor& anchor = wanted->anchors[n];
                        volume.component = anchor.component;
                        volume.sub = anchor.sub;
                        volume.flags = anchor.flags;
                        volume.mask = anchor.mask;
                        std::copy(std::begin(anchor.dir), std::end(anchor.dir), volume.dir);
                        recorded.volumes.push_back(volume);
                    }
                    const std::vector<int> none(recorded.volumes.size(), -1);
                    const replay::burn::CharWriteReport report = replay::burn::WriteChar(address, recorded, none, true);
                    g_poseV2FireCharCreated += static_cast<std::uint64_t>(report.created);
                    g_poseV2FireCharCreateFailed += static_cast<std::uint64_t>(report.createFailed);
                    action = report.created ? "record_created" : "record_created_empty";
                    if (!report.created)
                    {
                        // An empty record would linger: taken off again.
                        std::uint32_t removeCode = 0;
                        replay::burn::RemoveChar(address, removeCode);
                    }
                }
                else
                    action = code == 2 ? "record_no_source" : "record_create_failed";
            }
            else if (!onFire && !puppet.fireCharIgnitedAt && settled && normalPlay && puppet.fireCharIgnites < 8)
            {
                // Only in normal play (effects are built on play): paused after
                // a scrub the puppet waits without a record instead of burning
                // on a still playhead. The engine path that makes a record
                // without a fire (the fire record copy) is not validated. No
                // record on the puppet (a scrub past a fire that ended, or the fire rebuild
                // took it): a short fire of our own makes the engine create
                // one; it is put out on the next check and the recorded volumes
                // are written into it (a dead, frozen, invincible horse without
                // collision gets a record from a 300 ms fire, and a grown
                // volume chars it and stays). Invincible and collision-less
                // puppets refuse START_ENTITY_FIRE: both lifted for the call.
                ENTITY::SET_ENTITY_INVINCIBLE(puppet.clone, FALSE);
                ENTITY::SET_ENTITY_COLLISION(puppet.clone, TRUE, FALSE);
                FIRE::START_ENTITY_FIRE(puppet.clone, 1.0f, -1, 0);
                ENTITY::SET_ENTITY_COLLISION(puppet.clone, FALSE, FALSE);
                ENTITY::SET_ENTITY_INVINCIBLE(puppet.clone, TRUE);
                puppet.fireCharIgnitedAt = now;
                ++puppet.fireCharIgnites;
                action = FIRE::IS_ENTITY_ON_FIRE(puppet.clone) ? "ignite" : "ignite_refused";
            }
            if (action && g_poseV2FireCharPlayLogs < 96)
            {
                ++g_poseV2FireCharPlayLogs;
                Ctx().log << "POSE_V2_FIRE_CHAR,phase=" << action << ",id=" << replay::entity::IdText(puppet.packet.id)
                    << ",wanted=" << (wanted->present ? 1 : 0) << ",had=" << (actual.present ? 1 : 0)
                    << ",volumes=" << wanted->volumes.size() << ",puppet_volumes=" << actual.volumes.size()
                    << ",char=" << (wanted->volumes.empty() ? 0.0f : wanted->volumes[0][12])
                    << ",on_fire=" << (onFire ? 1 : 0) << ",writes=" << g_poseV2FireCharWrites
                    << ",removes=" << g_poseV2FireCharRemoves << ",shape_differs=" << g_poseV2FireCharShapeDiffers
                    << ",created=" << g_poseV2FireCharCreated << ",create_failed=" << g_poseV2FireCharCreateFailed
                    << ",playhead_ms=" << timeMs << '\n';
            }
        }
    }
}
