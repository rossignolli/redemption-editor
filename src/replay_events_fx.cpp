// replay_events_fx.cpp: particle effects: the shot fx windows, the one-shot effects and explosions recorded and played, the fx check.
// Split from replay_events.cpp (moved code); shared internals in replay_events_internal.h.

#include "replay_events_internal.h"

namespace replay::events
{
    namespace
    {
    std::uint64_t g_fxCheckStartCursor = 0;
    std::uint64_t g_fxCheckRegisteredCursor = 0;
    bool g_fxCheckRawBasis = false;
    std::uint64_t g_fxOwnerKilledBase = 0;
    std::string g_fxOwnerError;
    std::set<std::uint64_t> g_fxCheckRefusedKeys; // registered keys refused on puppets, once each
    std::map<std::uint32_t, std::uint32_t> g_fxOwnerKilledByHash;

    std::uint32_t g_fxOwnerKillLogs = 0;
    std::set<std::uint64_t> g_fxCheckPuppetOwners; // puppet clones and the hidden player, refreshed each tick
    // The replay's own one-shot instances still possibly alive: a scrub, a
    // jump back or the end of playback kills them (a replay editor removes
    // its fx on a cursor jump), so a long burst (an explosion's smoke) does
    // not stay over a playhead that is no longer there.
    struct OwnOneShot
    {
        std::uint64_t inst = 0;
        std::uint32_t hash = 0;
        std::uint32_t timeMs = 0;
        std::uint64_t entity = 0; // the entity it rides (FxStart::entity), 0 in the world
    };
    std::vector<OwnOneShot> g_fxOwnOneShots;
    std::uint64_t g_fxGapMissed[5] = {};
    std::uint32_t g_fxGapMissedLogs = 0;
    std::vector<std::uint8_t> g_wfxDropEvent;
    std::uint64_t g_wfxDroppedEventsPlayed = 0; // recorded muzzle effects not played (the request plays)
    std::uint64_t g_fxPauseRestarts = 0;
    std::uint64_t g_fxPauseExpiredBase = 0; // PauseFreezeExpired at the playback's start
    std::string g_fxHookErrors;
    replay::shotfx::Audit g_fxShotAudit;
    bool IsWeaponFxSite(std::uint32_t site)
    {
        static const std::uint32_t weaponA = CallReturnRva(kWeaponFxACall);
        static const std::uint32_t weaponB = CallReturnRva(kWeaponFxBCall);
        return site == replay::fx::MuzzleFlashSite() || site == replay::fx::BulletImpactSite() ||
            site == replay::fx::BodyHitSite() || (site && (site == weaponA || site == weaponB));
    }
    std::uint32_t g_fxCheckReachedMs = 0; // the furthest playhead of this playback (clip basis counts up to it)
    std::uint64_t g_fxPassedNotNormal = 0; // one-shots the playhead passed outside normal play
    // The clip time this playback traversed in normal play (merged spans):
    // the clip basis counts the events inside them (a clip started at its
    // in-point jumps over the start).
    std::vector<std::pair<std::uint32_t, std::uint32_t>> g_fxCheckSpans;
    std::uint32_t g_fxCheckLastMs = 0;
    std::uint32_t g_fxPassedLogs = 0;
    std::uint32_t g_fxOwnLastPlayhead = 0;
    std::uint64_t g_fxOwnOneShotsCleared = 0;
    std::uint64_t g_fxOwnOneShotsClears = 0;

    // The families with their own tracks that the owner rule never touches
    // (blood, explosion, fire, gore; the weapon sites are checked by site in
    // the observer): names from the public femga list.
    constexpr std::uint32_t FxJoaat(const char* text)
    {
        std::uint32_t hash = 0;
        for (; *text; ++text)
        {
            const char c = (*text >= 'A' && *text <= 'Z') ? static_cast<char>(*text - 'A' + 'a') : *text;
            hash += static_cast<std::uint8_t>(c);
            hash += hash << 10;
            hash ^= hash >> 6;
        }
        hash += hash << 3;
        hash ^= hash >> 11;
        hash += hash << 15;
        return hash;
    }
    std::vector<std::uint32_t> FxOwnerExemptHashes()
    {
        static const char* const kNames[] = {
            "blood_entry", "blood_entry_critical", "blood_exit", "blood_exit_critical", "blood_mist", "blood_melee_punch",
            "blood_melee_blunt", "blood_melee_slash", "blood_stab", "blood_headshot", "blood_wheel", "blood_animal",
            "exp_grd_dynamite", "exp_air_dynamite", "exp_air_firebottle", "exp_grd_firebottle", "exp_grd_molotov",
            "fire_spread_generic", "fire_spread_generic_obj", "fire_spread_generic_smoulder_loop", "fire_ped_body",
            "fire_ped_limb", "bullet_tracer", "bullet_tracer_incendiary", "bullet_shotgun_tracer",
        };
        std::vector<std::uint32_t> out;
        for (const char* name : kNames)
            out.push_back(FxJoaat(name));
        out.push_back(0x45FAF2C0u); // the bleeding (registered)
        return out;
    }
    constexpr std::uint64_t kPoseV2FxBeforeUs = 100000;
    }

    void LogPoseV2FxSnapshot(const char* source, const std::vector<std::uint8_t>& bytes)
    {
        static const char digits[] = "0123456789abcdef";
        std::string hex;
        hex.reserve(bytes.size() * 2);
        for (const std::uint8_t byte : bytes)
        {
            hex.push_back(digits[byte >> 4]);
            hex.push_back(digits[byte & 0x0F]);
        }
        std::ostream& log = Ctx().log;
        log << "POSE_V2_FX_SNAPSHOT,source=" << source << ",bytes=" << bytes.size() << ",hex=" << hex << '\n';
        log.flush();
    }

    void FlushPoseV2FxWindows(bool all)
    {
        REPLAY_PROFILE_ZONE("rec.fx_windows");
        if (g_poseV2FxWindows.empty())
            return;
        const std::uint64_t now = replay::fx::NowUs();
        static std::vector<replay::fx::TriggerCall> calls;
        std::ostream& log = Ctx().log;
        for (std::size_t index = 0; index < g_poseV2FxWindows.size();)
        {
            const PoseV2FxWindow window = g_poseV2FxWindows[index];
            if (!all && now < window.shotUs + kPoseV2FxAfterUs)
            {
                ++index;
                continue;
            }
            replay::fx::Collect(window.shotUs - kPoseV2FxBeforeUs, window.shotUs + kPoseV2FxAfterUs, calls);
            // The weapon vfx call sites give this shot's muzzle and impact
            // effects (nearest to the shot); other effects stay evidence.
            replay::entity::ShotEvent& shot = Ctx().clip.shots.At(window.shot);
            std::uint64_t muzzleDistance = ~0ull;
            std::uint64_t impactDistance = ~0ull;
            std::uint32_t others = 0;
            std::uint64_t muzzleKey = 0, impactKey = 0; // the trigger each took
            for (const replay::fx::TriggerCall& call : calls)
            {
                const std::uint64_t distance = call.timeUs > window.shotUs
                    ? call.timeUs - window.shotUs
                    : window.shotUs - call.timeUs;
                if (call.callers[1] == replay::fx::MuzzleFlashSite() && distance < muzzleDistance)
                {
                    muzzleDistance = distance;
                    shot.muzzleFx = call.effectHash;
                    muzzleKey = call.timeUs ^ (std::uint64_t(call.effectHash) << 32);
                }
                else if (call.callers[1] == replay::fx::BulletImpactSite() && distance < impactDistance)
                {
                    impactDistance = distance;
                    shot.impactFx = call.effectHash;
                    impactKey = call.timeUs ^ (std::uint64_t(call.effectHash) << 32) ^ 1ull;
                }
                else if (call.callers[1] != replay::fx::MuzzleFlashSite() &&
                    call.callers[1] != replay::fx::BulletImpactSite())
                {
                    ++others;
                    if (!g_poseV2ResearchWindows)
                        continue;
                    log << "POSE_V2_FX_OBS"
                        << ",shot=" << window.shot
                        << ",dt_us=" << static_cast<std::int64_t>(call.timeUs - window.shotUs)
                        << ",effect=0x" << std::hex << call.effectHash
                        << ",callers=0x" << call.callers[0]
                        << "/0x" << call.callers[1]
                        << "/0x" << call.callers[2] << std::dec
                        << '\n';
                }
            }
            // The gameplay muzzle instance's flags at its start, nearest to
            // the shot.
            static std::vector<replay::fx::MuzzleStart> starts;
            replay::fx::CollectMuzzleStarts(
                window.shotUs - kPoseV2FxBeforeUs, window.shotUs + kPoseV2FxAfterUs, starts);
            std::uint64_t startDistance = ~0ull;
            for (const replay::fx::MuzzleStart& start : starts)
            {
                const std::uint64_t distance = start.timeUs > window.shotUs
                    ? start.timeUs - window.shotUs
                    : window.shotUs - start.timeUs;
                if (distance < startDistance)
                {
                    startDistance = distance;
                    shot.muzzleFlags = start.flags & replay::fx::kMuzzleFlagMask;
                    shot.hasMuzzleFlags = true;
                }
            }
            // The engine weapon fire event nearest to the shot, with its
            // entity references located by role.
            {
                static std::vector<replay::audio::FireCall> fires;
                replay::audio::CollectFires(
                    window.shotUs - kPoseV2FxBeforeUs, window.shotUs + kPoseV2FxAfterUs, fires);
                std::uint64_t fireDistance = ~0ull;
                const replay::audio::FireCall* nearest = window.hasFire ? &window.fire : nullptr;
                for (const replay::audio::FireCall& fire : fires)
                {
                    if (window.hasFire)
                        break;
                    const std::uint64_t distance = fire.timeUs > window.shotUs
                        ? fire.timeUs - window.shotUs
                        : window.shotUs - fire.timeUs;
                    if (distance < fireDistance)
                    {
                        fireDistance = distance;
                        nearest = &fire;
                    }
                }
                if (nearest)
                {
                    std::copy(std::begin(nearest->info), std::end(nearest->info), shot.fireInfo.begin());
                    shot.hasFireInfo = true;
                    shot.fireRefs.clear();
                    std::string roles;
                    // The copy constructor of the fire info registers entity
                    // references at +0x00, +0x10 and +0x18 only;
                    // +0x08 is the registration slot it writes itself.
                    for (const std::uint16_t offset : {std::uint16_t{0x00}, std::uint16_t{0x10}, std::uint16_t{0x18}})
                    {
                        std::uint64_t value = 0;
                        std::memcpy(&value, nearest->info + offset, 8);
                        if (!value)
                            continue;
                        replay::entity::ShotEvent::FireRefField ref;
                        ref.offset = offset;
                        if (value == window.shooterAudio)
                            ref.role = replay::entity::ShotEvent::FireRef::ShooterAudio;
                        std::string what = "unknown";
                        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
                        {
                            if (value == reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(entity.handle)))
                            {
                                ref.target = entity.id;
                                what = "entity:" + replay::entity::IdText(entity.id);
                            }
                        }
                        for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
                        {
                            if (value == reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(weapon.object)))
                            {
                                ref.target = weapon.id;
                                what = "weapon:" + replay::entity::IdText(weapon.id);
                            }
                        }
                        if (ref.target.Valid())
                            ref.role = value == window.shooter
                                ? replay::entity::ShotEvent::FireRef::Shooter
                                : replay::entity::ShotEvent::FireRef::Weapon;
                        if (!ref.target.Valid() && ref.role != replay::entity::ShotEvent::FireRef::ShooterAudio)
                        {
                            // Which recorded entity holds this reference in
                            // its own memory (first 0x1000 bytes)?
                            const auto holds = [&](Entity handle, replay::entity::ReplayId id) {
                                static std::uint64_t words[0x200];
                                const std::uint64_t address =
                                    reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(handle));
                                if (!address || !replay::audio::ReadBytes(address, words, sizeof(words)))
                                    return false;
                                for (std::uint16_t word = 0; word < 0x200; ++word)
                                {
                                    if (words[word] == value)
                                    {
                                        ref.via = id;
                                        ref.viaOffset = static_cast<std::uint16_t>(word * 8);
                                        return true;
                                    }
                                }
                                return false;
                            };
                            for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
                            {
                                if (!ref.via.Valid() && replay::entities::Exists(weapon.object))
                                    holds(weapon.object, weapon.id);
                            }
                            for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
                            {
                                if (!ref.via.Valid() && replay::entities::Exists(entity.handle))
                                    holds(entity.handle, entity.id);
                            }
                            // A projectile prop, then a weapon object gone
                            // since (a thrown weapon: the hand object is
                            // replaced by the projectile the frame it leaves).
                            for (const replay::recorder::RecordedProp& prop : Ctx().recorder.props)
                            {
                                if (!ref.via.Valid() && prop.object && replay::entities::Exists(prop.object))
                                    holds(prop.object, prop.id);
                            }
                            if (!ref.via.Valid())
                            {
                                for (const PoseV2WeaponMemo& memo : g_poseV2WeaponMemo)
                                {
                                    if (memo.weapon == value)
                                    {
                                        ref.via = memo.id;
                                        ref.viaOffset = 0x448;
                                        ++g_poseV2FireRefFromMemo;
                                        break;
                                    }
                                }
                            }
                            if (ref.via.Valid())
                            {
                                char text[64]{};
                                std::snprintf(text, sizeof(text), "held_by:%s+0x%X",
                                    replay::entity::IdText(ref.via).c_str(), static_cast<unsigned>(ref.viaOffset));
                                what = text;
                            }
                        }
                        if (!ref.target.Valid() && !ref.via.Valid() &&
                            ref.role != replay::entity::ShotEvent::FireRef::ShooterAudio)
                        {
                            // Evidence: which world entity it is.
                            static int pool[1024];
                            const auto search = [&](int count, const char* kind) {
                                for (int index = 0; index < count; ++index)
                                {
                                    if (value == reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(pool[index])))
                                    {
                                        char text[64]{};
                                        std::snprintf(text, sizeof(text), "%s:handle=%d:model=0x%X", kind, pool[index],
                                            static_cast<unsigned>(ENTITY::GET_ENTITY_MODEL(pool[index])));
                                        what = text;
                                        return true;
                                    }
                                }
                                return false;
                            };
                            if (!search(worldGetAllObjects(pool, 1024), "object") &&
                                !search(worldGetAllPeds(pool, 1024), "ped"))
                            {
                                search(worldGetAllVehicles(pool, 1024), "vehicle");
                            }
                        }
                        shot.fireRefs.push_back(ref);
                        char text[96]{};
                        std::snprintf(text, sizeof(text), "%s%X=%s", roles.empty() ? "" : "|",
                            static_cast<unsigned>(offset), what.c_str());
                        roles += text;
                    }
                    log << "POSE_V2_FIRE_INFO,shot=" << window.shot
                        << ",dt_us=" << static_cast<std::int64_t>(nearest->timeUs - window.shotUs)
                        << ",refs=" << (roles.empty() ? "none" : roles.c_str())
                        << ",fires_in_window=" << fires.size()
                        << '\n';
                    if (window.shot < 3)
                    {
                        LogPoseV2FxSnapshot("fire_info",
                            std::vector<std::uint8_t>(std::begin(nearest->info), std::end(nearest->info)));
                    }
                }
            }
            log << "POSE_V2_SHOT_FX"
                << ",shot=" << window.shot
                << ",muzzle_flags=0x" << std::hex << shot.muzzleFlags << std::dec
                << ",has_muzzle_flags=" << (shot.hasMuzzleFlags ? 1 : 0)
                << ",weapon=0x" << std::hex << shot.weaponHash
                << ",muzzle_fx=0x" << shot.muzzleFx
                << ",impact_fx=0x" << shot.impactFx << std::dec
                << ",impact=" << (shot.hasImpact ? 1 : 0)
                << ",normal=" << (shot.hasNormal ? 1 : 0)
                << ",other_fx=" << others
                << '\n';
            // The triggers claimed by more than one shot
            // (FX_SHOT_RESOLVE_TOTAL); the effects are
            // events of the fx track and the shot keeps no hash (the playback
            // then makes no flash or impact of its own).
            ++g_fxShotResolveShots;
            for (const std::uint64_t key : {muzzleKey, impactKey})
                if (key)
                {
                    ++g_fxShotClaimed;
                    if (++g_fxShotTriggerClaims[key] == 2)
                        ++g_fxShotClaimedMulti;
                }
            if (!shot.muzzleFx)
                ++g_fxShotNoMuzzle;
            shot.muzzleFx = 0;
            shot.impactFx = 0;
            g_poseV2FxWindows.erase(g_poseV2FxWindows.begin() + static_cast<std::ptrdiff_t>(index));
        }
        log.flush();
    }

    void RecordPoseV2Explosions()
    {
        REPLAY_PROFILE_ZONE("rec.explosions");
        static std::vector<replay::audio::ExplosionAudioCall> calls;
        replay::audio::CollectExplosionsSince(g_poseV2ExplosionCursor, calls);
        if (calls.empty())
            return;
        const std::uint64_t now = replay::audio::NowUs();
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        const Vector3 player = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE, FALSE);
        for (const replay::audio::ExplosionAudioCall& call : calls)
        {
            const float dx = call.position[0] - player.x, dy = call.position[1] - player.y, dz = call.position[2] - player.z;
            if (dx * dx + dy * dy + dz * dz > 400.0f * 400.0f)
                continue; // far away: not part of the scene
            const std::uint64_t ageMs = now > call.timeUs ? (now - call.timeUs) / 1000 : 0;
            replay::entity::ExplosionAudioEvent event;
            event.timeMs = replay::pose::capture::RecordMsAgo(ageMs, nowMs);
            event.hasInstance = call.hasInstance;
            event.tag = call.tag;
            event.overrideRef = call.overrideRef;
            std::memcpy(event.position, call.position, sizeof(event.position));
            event.underwater = call.underwater;
            event.b31 = call.b31;
            event.b32 = call.b32;
            event.interior = call.interior;
            event.caller = call.caller;
            Ctx().clip.explosions.Add(event);
            Ctx().log << "POSE_V2_EXPLOSION_AUDIO,phase=record,tag=" << call.tag << ",override=0x" << std::hex << call.overrideRef
                << ",caller=0x" << call.caller << std::dec << ",instance=" << call.hasInstance << ",pos=" << call.position[0] << ";"
                << call.position[1] << ";" << call.position[2] << ",underwater=" << int(call.underwater) << ",time_ms=" << event.timeMs << '\n';
        }
    }

    // Every particle instance the engine started (the generic one-shot fx
    // packets): muzzle and impact stay with the shot record; an attached
    // effect needs a recorded entity, a world one is kept as is.
    void RecordPoseV2Fx()
    {
        REPLAY_PROFILE_ZONE("rec.fx");
        static std::vector<replay::fx::FxStart> starts;
        replay::fx::CollectStartsSince(g_poseV2FxStartCursor, starts);
        if (starts.empty())
            return;
        static std::vector<std::pair<std::uint64_t, replay::entity::ReplayId>> owners;
        owners.clear();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (!replay::entities::Exists(entity.handle))
                continue;
            owners.push_back({reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(entity.handle)), entity.id});
        }
        for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
        {
            if (weapon.object && replay::entities::Exists(weapon.object))
                owners.push_back({reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(weapon.object)), weapon.id});
        }
        for (const replay::recorder::RecordedProp& prop : Ctx().recorder.props)
        {
            if (prop.object && replay::entities::Exists(prop.object))
                owners.push_back({reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(prop.object)), prop.id});
        }
        {
            // Train cars.
            static std::vector<replay::trains::RecordedCarRef> cars;
            replay::trains::RecordedCars(cars);
            for (const replay::trains::RecordedCarRef& car : cars)
                owners.push_back({car.address, car.id});
        }
        const std::uint64_t now = replay::fx::NowUs();
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        for (const replay::fx::FxStart& start : starts)
        {
            ++g_poseV2FxStartsByCaller[start.callers[1]];
            if (!start.effectHash)
            {
                ++g_poseV2FxNoHash;
                ++g_poseV2FxNoHashByCaller[start.callers[1]];
                // A registry restart of a live key (the key got again after a
                // stretch without a call).
                if (start.callers[1] == replay::fx::RegistryRestartSite())
                    for (const LiveRegistered& live : g_poseV2RegisteredLive)
                        if (live.inst == start.inst)
                        {
                            g_fxCheck.NoteRestart(Ctx().clip.registeredFx[live.track].effectHash, true);
                            break;
                        }
                continue;
            }
            // The weapon sites, live.
            if (IsWeaponFxSite(start.callerSite))
            {
                g_fxCheck.NoteWeapon(start.callerSite, 0);
                if (start.callerSite == replay::fx::MuzzleFlashSite())
                    ++g_fxShotMuzzleStarts;
                else if (start.callerSite == replay::fx::BulletImpactSite())
                    ++g_fxShotImpactStarts;
                else if (start.callerSite == replay::fx::BodyHitSite())
                    ++g_fxShotBodyStarts;
            }
            const std::uint64_t startAgeMs = now > start.timeUs ? (now - start.timeUs) / 1000 : 0;
            const std::uint32_t startMs = replay::pose::capture::RecordMsAgo(startAgeMs, nowMs);
            // The impact site is the weapon impact vfx of any hit, not only a
            // bullet's (a molotov's glass smash comes through it with no shot near).
            // One event per weapon effect start (muzzle flash and bullet impact), no
            // match with the shot by time: both sites are plain events and the shot
            // keeps no hash.
            if (start.callerSite == replay::fx::BulletImpactSite())
                ++g_poseV2FxImpactNoShot;
            if (g_poseV2FxSamples == 0)
            {
                std::ostream& log = Ctx().log;
                log << "POSE_V2_FX_OWNERS,attaches_since_start=" << (replay::fx::TotalAttaches() - g_poseV2FxAttachesAtStart) << ",list=";
                for (const auto& owner : owners)
                    log << replay::entity::IdText(owner.second) << "@0x" << std::hex << owner.first << std::dec << ";";
                log << '\n';
            }
            replay::entity::FxEvent event;
            // An instance with an attach node follows an entity: it needs a
            // recorded one. Without a node (the engine's hoof and foot puffs,
            // mode 1 with the world matrix in the parent) or in world mode
            // it is placed in the world.
            // An identity offset with a world parent is a world placement
            // (the engine's splashes hold the entity only to key the effect).
            const bool identityOffset = std::fabs(start.matrix[0] - 1.0f) < 1e-3f && std::fabs(start.matrix[5] - 1.0f) < 1e-3f &&
                std::fabs(start.matrix[10] - 1.0f) < 1e-3f && std::fabs(start.matrix[1]) < 1e-3f && std::fabs(start.matrix[2]) < 1e-3f &&
                std::fabs(start.matrix[4]) < 1e-3f && std::fabs(start.matrix[6]) < 1e-3f && std::fabs(start.matrix[8]) < 1e-3f &&
                std::fabs(start.matrix[9]) < 1e-3f && std::fabs(start.matrix[12]) < 1e-3f && std::fabs(start.matrix[13]) < 1e-3f &&
                std::fabs(start.matrix[14]) < 1e-3f;
            // The weapon sites attach on a bone of a recorded entity with an identity
            // offset (the flash on Gun_Muzzle): it stays attached, it does not become a
            // world puff.
            const bool weaponAttached = start.mode == 1 && start.entity && start.bone >= 0 &&
                (start.callerSite == replay::fx::MuzzleFlashSite() ||
                    start.callerSite == replay::fx::BulletImpactSite());
            if (start.mode == 1 && start.entity && (!identityOffset || weaponAttached))
            {
                const auto owner = std::find_if(owners.begin(), owners.end(),
                    [&](const auto& entry) { return entry.first == start.entity; });
                // An entity the recording does not have that is not an actor (a stage
                // deck, a map object, a wall) stays where it is: the effect plays where it
                // started in the world, like an explosion fx with no entity.
                bool worldOwner = false;
                if (owner == owners.end())
                {
                    const Entity handle = HandleAtAddress(start.entity);
                    worldOwner = !handle || (!replay::entities::IsPed(handle) && !ENTITY::IS_ENTITY_A_VEHICLE(handle));
                }
                if (worldOwner)
                {
                    // A parent of zeros leaves the offset local.
                    if (replay::cliparea::FxWorldUnresolved(start.mode, start.parent))
                    {
                        NotePoseV2FxUnresolved("unresolved_parent", start);
                        g_fxCheck.NoteRecord(start.effectHash, replay::fxcheck::Stored::Unresolved, false, start.callerSite);
                        continue;
                    }
                    ++g_poseV2FxWorldOwner;
                    replay::entity::ComposeFxWorld(start.matrix, start.parent, event.matrix);
                }
                else if (owner == owners.end())
                {
                    ++g_poseV2FxSkippedEntity;
                    ++g_poseV2FxSkippedByHash[start.effectHash];
                    g_fxCheck.NoteRecord(start.effectHash, replay::fxcheck::Stored::Skipped, false, start.callerSite);
                    LogPoseV2FxStartSample("skipped_entity", start);
                    // Evidence only (ResolvePoseV2RegisteredAttaches).
                    if (g_poseV2FxPendingOneShot.size() < 32)
                        g_poseV2FxPendingOneShot.push_back({start.entity, start.effectHash, nowMs});
                    continue;
                }
                else
                {
                    event.entity = owner->second;
                    event.bone = start.bone;
                    std::memcpy(event.matrix, start.matrix, sizeof(event.matrix));
                }
            }
            else
            {
                // A mode 1 instance whose parent is still zeros would come out at the map
                // origin and stretch the clip area. The world position is unknown: the puff
                // is not recorded.
                if (replay::cliparea::FxWorldUnresolved(start.mode, start.parent))
                {
                    NotePoseV2FxUnresolved("unresolved_parent", start);
                    g_fxCheck.NoteRecord(start.effectHash, replay::fxcheck::Stored::Unresolved, false, start.callerSite);
                    continue;
                }
                replay::entity::ComposeFxWorld(start.matrix, start.parent, event.matrix);
            }
            // A world placement must be a world position.
            if (!event.entity.Valid() &&
                !replay::cliparea::PositionValid(event.matrix[12], event.matrix[13], event.matrix[14]))
            {
                NotePoseV2FxUnresolved("invalid_world_position", start);
                g_fxCheck.NoteRecord(start.effectHash, replay::fxcheck::Stored::Unresolved, false, start.callerSite);
                continue;
            }
            event.timeMs = startMs;
            event.effectHash = start.effectHash;
            event.assetHash = start.assetHash;
            event.callerSite = start.callerSite;
            event.zoom = start.zoom;
            event.flags = start.flags;
            std::memcpy(event.evolutions, start.evolutions, sizeof(event.evolutions));
            // The fields the site wrote before the start (FXSS).
            event.hasSettings = start.settings.mask != 0;
            if (event.hasSettings)
                event.settings = start.settings;
            ++g_poseV2FxRecordedByHash[start.effectHash];
            g_fxCheck.NoteRecord(start.effectHash, replay::fxcheck::Stored::Stored, false, start.callerSite);
            if (!event.entity.Valid())
                Ctx().clip.ExtendArea(event.matrix[12], event.matrix[13], event.matrix[14]);
            LogPoseV2FxStartSample("recorded", start);
            Ctx().clip.fx.Add(event);
        }
    }
    std::unordered_map<std::uint32_t, replay::decal::AddCall> g_poseV2DecalMonitor;
    std::uint64_t g_poseV2MonitorAddCursor = 0;
    std::uint64_t g_poseV2MonitorRemoveCursor = 0;
    std::vector<std::uint32_t> g_poseV2MonitorCheck;
    std::size_t g_poseV2MonitorCheckAt = 0;
    bool g_poseV2DecalInitialPending = false;
    std::uint32_t g_poseV2DecalMonitorSession = 0; // session resets that cleared the monitor
    bool g_poseV2DecalInitialPlayPending = false;
    std::uint64_t g_poseV2DecalInitial = 0;

    // The generic fx track in normal play, forward only: attached effects on
    // the puppet of the recorded entity with the recorded offset, world effects
    // at the recorded matrix.
    void PlayPoseV2Fx(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.fx");
        if (!g_poseV2FxReplayEnabled)
            return;
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
        // The one-shots the playhead passed outside normal play (pause, preparing,
        // scrub, a step past the cursor's 250 ms) are not played; counted for the
        // fx check.
        static std::uint32_t lastSeenMs = 0;
        const std::uint32_t previousMs = lastSeenMs;
        lastSeenMs = timeMs;
        if (!g_poseV2FxCursorPlay.Advance(timeMs, Ctx().hooks->NormalPlay(), fromMs, toMs))
        {
            if (timeMs > previousMs)
            {
                static std::vector<std::size_t> passed;
                Ctx().clip.fx.Range(previousMs, timeMs, passed);
                if (!passed.empty())
                {
                    g_fxPassedNotNormal += passed.size();
                    const char* why = Ctx().transport.Paused() ? "paused" : Ctx().transport.Scrubbing() ? "scrub"
                        : !Ctx().hooks->NormalPlay() ? "preparing" : "step";
                    if (g_fxPassedLogs < 12)
                    {
                        ++g_fxPassedLogs;
                        Ctx().log << "FX_PASSED_NOT_PLAYED,from_ms=" << previousMs << ",to_ms=" << timeMs
                                  << ",events=" << passed.size() << ",why=" << why << '\n';
                    }
                }
            }
            return;
        }
        static std::vector<std::size_t> due;
        Ctx().clip.fx.Range(fromMs, toMs, due);
        for (const std::size_t index : due)
        {
            const replay::entity::FxEvent& event = Ctx().clip.fx.Events()[index];
            // A muzzle effect a WFXI request plays (PlayWeaponFxIntents), not twice.
            if (g_wfxPlayOn && g_wfxDropEvent.size() == Ctx().clip.fx.Events().size() && g_wfxDropEvent[index])
            {
                ++g_wfxDroppedEventsPlayed;
                continue;
            }
            void* entity = nullptr;
            // An older clip keeps world effects whose local offset was taken for the
            // world matrix (the map origin).
            if (!event.entity.Valid() &&
                !replay::cliparea::PositionValid(event.matrix[12], event.matrix[13], event.matrix[14]))
            {
                ++g_poseV2FxNoWorldPlay;
                continue;
            }
            if (event.entity.Valid())
            {
                const replay::playback::Puppet* puppet = Ctx().puppets.Find(event.entity);
                if (!puppet || puppet->state != replay::playback::PuppetState::Ready || !puppet->clone ||
                    !replay::entities::Exists(puppet->clone))
                {
                    ++g_poseV2FxNoPuppet;
                    continue;
                }
                entity = replay::entities::AddressOf(puppet->clone);
            }
            static const float identity[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
            std::string error;
            // The recorded settings and attach bits of the start, and the instance for
            // the one tick later checks.
            replay::fx::PlayOptions options;
            std::uint64_t made = 0;
            options.settings = g_fxCfg.settingsReplay && event.hasSettings ? &event.settings : nullptr;
            options.applyFlags = entity != nullptr;
            options.flags = event.flags;
            options.instOut = &made;
            if (replay::fx::PlayEffectAsEx(event.effectHash, entity, entity ? event.bone : -1, entity ? event.matrix : identity,
                    entity ? identity : event.matrix, event.evolutions, event.zoom, options, error))
            {
                ++g_poseV2FxPlayed;
                const bool weapon = IsWeaponFxSite(event.callerSite);
                if (weapon)
                    g_fxCheck.NoteWeapon(event.callerSite, 2);
                if (made && (weapon || options.settings) && g_fxPendingChecks.size() < 256)
                {
                    FxPendingCheck check;
                    check.inst = made;
                    check.hash = event.effectHash;
                    check.site = event.callerSite;
                    check.entity = reinterpret_cast<std::uint64_t>(entity);
                    check.expected[0] = event.matrix[12];
                    check.expected[1] = event.matrix[13];
                    check.expected[2] = event.matrix[14];
                    check.weapon = weapon;
                    check.hasSettings = options.settings != nullptr;
                    if (options.settings)
                        check.settings = *options.settings;
                    g_fxPendingChecks.push_back(check);
                }
            }
            else
            {
                ++g_poseV2FxFailed;
                g_poseV2FxLastError = error;
            }
        }
    }

    // Explosions play only in normal forward play: no scrub, jump or pause.
    void PlayPoseV2Explosions(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.explosions");
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
        if (!g_poseV2ExplosionCursorPlay.Advance(timeMs, Ctx().hooks->NormalPlay(), fromMs, toMs))
            return;
        static std::vector<std::size_t> due;
        // Past 16 in a frame the rest waits for the next frame (the manager queue
        // holds 16 requests).
        static std::vector<std::size_t> carried;
        static std::vector<std::size_t> range;
        Ctx().clip.explosions.Range(fromMs, toMs, range);
        due.assign(carried.begin(), carried.end());
        due.insert(due.end(), range.begin(), range.end());
        carried.clear();
        int thisFrame = 0;
        for (const std::size_t index : due)
        {
            if (++thisFrame > 16)
            {
                carried.push_back(index);
                continue;
            }
            const replay::entity::ExplosionAudioEvent& event = Ctx().clip.explosions.Events()[index];
            replay::audio::ExplosionAudioCall call;
            call.hasInstance = event.hasInstance;
            call.tag = event.tag;
            call.overrideRef = event.overrideRef;
            std::memcpy(call.position, event.position, sizeof(call.position));
            call.underwater = event.underwater;
            call.b31 = event.b31;
            call.b32 = event.b32;
            call.interior = event.interior;
            std::string error;
            const bool ok = replay::audio::PlayExplosionAudio(call, error);
            ++(ok ? g_poseV2ExplosionsPlayed : g_poseV2ExplosionsRefused);
            if (!ok)
                g_poseV2ExplosionLastError = error;
            Ctx().log << "POSE_V2_EXPLOSION_AUDIO,phase=play,tag=" << event.tag << ",status=" << error
                << ",event_ms=" << event.timeMs << ",playhead_ms=" << timeMs
                << ",rate=" << Ctx().transport.Rate() << '\n';
        }
    }

    // The fx check's playback side, every tick.
    void TallyPoseV2FxCheckPlay(std::uint32_t timeMs)
    {
        if (!g_poseV2FxObserverReady)
            return;
        g_fxCheckPuppetOwners.clear();
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
            if (puppet.clone && replay::entities::Exists(puppet.clone))
                if (const auto* address = replay::entities::AddressOf(puppet.clone))
                    g_fxCheckPuppetOwners.insert(reinterpret_cast<std::uint64_t>(address));
        {
            std::vector<std::uint64_t> hidden;
            PoseV2HiddenPlayerEntities(hidden);
            g_fxCheckPuppetOwners.insert(hidden.begin(), hidden.end());
        }
        static std::vector<replay::fx::FxStart> starts;
        replay::fx::CollectStartsSince(g_fxCheckStartCursor, starts);
        const bool holding = timeMs != 0xFFFFFFFFu && !Ctx().hooks->NormalPlay() && !Ctx().transport.Scrubbing();
        for (const replay::fx::FxStart& start : starts)
        {
            if (!start.effectHash)
            {
                // The registry's restart of one of our instances: a burst;
                // while paused, the evidence of an emit in the pause (FX_PAUSE_EMIT).
                if (start.callers[1] == replay::fx::RegistryRestartSite())
                    for (const std::unique_ptr<PlayRegistered>& entry : g_poseV2RegisteredPlay)
                        if (entry->inst == start.inst && entry->track < Ctx().clip.registeredFx.size())
                        {
                            if (holding)
                                ++g_fxPauseRestarts;
                            else
                            {
                                ++g_fxPlayRestarts;
                                g_fxCheck.NoteRestart(Ctx().clip.registeredFx[entry->track].effectHash, false);
                            }
                            break;
                        }
                continue; // a registered instance's start: counted by its creation
            }
            if (start.killed)
            {
                g_fxCheck.NoteRefused(start.effectHash);
                ++g_fxOwnerKilledByHash[start.effectHash];
                if (g_fxOwnerKillLogs < 16)
                {
                    ++g_fxOwnerKillLogs;
                    Ctx().log << "FX_OWNER_KILL,playhead_ms=" << timeMs << std::hex << ",hash=0x" << start.effectHash
                        << ",site=0x" << start.callerSite << ",entity=0x" << start.entity << std::dec << ",bone=" << start.bone
                        << ",thread=" << start.threadId << '\n';
                }
                continue;
            }
            const replay::fxcheck::Maker maker = start.byReplay ? replay::fxcheck::Maker::Ours
                : (start.entity && g_fxCheckPuppetOwners.count(start.entity)) ? replay::fxcheck::Maker::EnginePuppet
                : replay::fxcheck::Maker::EngineOther;
            g_fxCheck.NotePlay(start.effectHash, maker, false, start.callerSite);
            // An engine one-shot on a puppet the owner rule should have killed (its
            // owner not in the list yet).
            if (maker == replay::fxcheck::Maker::EnginePuppet &&
                (!IsWeaponFxSite(start.callerSite) || g_fxClipWeaponEvents) &&
                start.callerSite != replay::fx::FootVfxSite())
            {
                const std::vector<std::uint32_t> exempt = FxOwnerExemptHashes();
                if (std::find(exempt.begin(), exempt.end(), start.effectHash) == exempt.end())
                    g_fxCheck.NoteOwnerLate(start.effectHash);
            }
            if (start.byReplay && start.inst)
            {
                if (g_fxOwnOneShots.size() >= 512)
                    g_fxOwnOneShots.erase(g_fxOwnOneShots.begin());
                g_fxOwnOneShots.push_back({start.inst, start.effectHash, timeMs, start.entity});
            }
        }
        // Clear on a scrub, a jump back, or the end (timeMs == UINT32_MAX).
        const bool end = timeMs == 0xFFFFFFFFu;
        if (!end && timeMs > g_fxCheckReachedMs)
            g_fxCheckReachedMs = timeMs;
        if (!end)
        {
            if (Ctx().hooks->NormalPlay() && timeMs > g_fxCheckLastMs &&
                timeMs - g_fxCheckLastMs <= replay::entity::ForwardEventCursor::kMaxStepMs)
            {
                if (!g_fxCheckSpans.empty() && g_fxCheckSpans.back().second == g_fxCheckLastMs)
                    g_fxCheckSpans.back().second = timeMs;
                else
                    g_fxCheckSpans.push_back({g_fxCheckLastMs, timeMs});
            }
            g_fxCheckLastMs = timeMs;
        }
        const bool jumpBack = !end && timeMs + 250 < g_fxOwnLastPlayhead;
        if (!end)
            g_fxOwnLastPlayhead = timeMs;
        if (end || jumpBack || Ctx().transport.Scrubbing())
        {
            if (!g_fxOwnOneShots.empty())
                ++g_fxOwnOneShotsClears;
            for (const OwnOneShot& own : g_fxOwnOneShots)
            {
                replay::fx::InstanceIdentity identity;
                if (!replay::fx::ReadInstanceIdentity(own.inst, identity) || identity.hash != own.hash ||
                    identity.state != replay::regfx::kStatePlaying)
                    continue;
                std::string error;
                if (replay::fx::StopInstance(own.inst, true, error))
                    ++g_fxOwnOneShotsCleared;
            }
            g_fxOwnOneShots.clear();
        }
        else if (!g_fxOwnOneShots.empty() && g_fxOwnOneShots.front().timeMs + 60000 < timeMs)
        {
            // A one-shot is long gone after a minute: forget it.
            g_fxOwnOneShots.erase(g_fxOwnOneShots.begin());
        }
        static std::vector<replay::fx::RegisteredCall> calls;
        replay::fx::CollectRegisteredSince(g_fxCheckRegisteredCursor, calls);
        for (const replay::fx::RegisteredCall& call : calls)
        {
            if (!call.effectHash)
                continue;
            if (call.suppressed)
            {
                const std::uint64_t key = (call.owner + static_cast<std::uint64_t>(call.id)) ^ (std::uint64_t(call.effectHash) << 32);
                if (g_fxCheckRefusedKeys.insert(key).second)
                    g_fxCheck.NoteRefused(call.effectHash);
                continue;
            }
            if (call.created && !call.byReplay)
            {
                g_fxCheck.NotePlay(call.effectHash,
                    g_fxCheckPuppetOwners.count(call.owner) ? replay::fxcheck::Maker::EnginePuppet : replay::fxcheck::Maker::EngineOther,
                    true, call.caller);
                // The engine's own registered effect on a world object one of our tracks
                // rides (a second copy), or on a puppet that was not refused (the list
                // late).
                if (std::find(g_fxWorldOwnersLive.begin(), g_fxWorldOwnersLive.end(), call.owner) != g_fxWorldOwnersLive.end())
                    g_fxCheck.NoteDupWorld(call.effectHash);
                else if (g_fxCheckPuppetOwners.count(call.owner))
                    g_fxCheck.NoteOwnerLate(call.effectHash);
            }
        }
    }

    // Our instances on each entity, after this tick's events (the registered
    // ones we attached, the one-shots on an entity), keeping the last
    // end-of-tick bits of an instance.
    void RebuildFxOwned()
    {
        static std::vector<FxOwned> next;
        next.clear();
        const auto previous = [&](std::uint64_t inst) -> const FxOwned* {
            for (const FxOwned& owned : g_fxOwned)
                if (owned.inst == inst)
                    return &owned;
            return nullptr;
        };
        // The owner's handle, by its address.
        static std::unordered_map<std::uint64_t, Entity> handles;
        handles.clear();
        if (g_fxCfg.relocateClearStill)
            for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
                if (puppet.clone)
                    if (const auto* address = replay::entities::AddressOf(puppet.clone))
                        handles[reinterpret_cast<std::uint64_t>(address)] = puppet.clone;
        const auto add = [&](std::uint64_t owner, std::uint64_t inst, std::uint32_t hash, Entity handle = 0) {
            if (!owner || !inst || next.size() >= 512)
                return;
            FxOwned owned;
            owned.owner = owner;
            owned.inst = inst;
            owned.hash = hash;
            if (!handle)
            {
                const auto found = handles.find(owner);
                handle = found != handles.end() ? found->second : 0;
            }
            owned.handle = handle;
            if (const FxOwned* before = previous(inst))
            {
                owned.lastFlags2 = before->lastFlags2;
                owned.lastKnown = before->lastKnown;
            }
            next.push_back(owned);
        };
        for (const std::unique_ptr<PlayRegistered>& entry : g_poseV2RegisteredPlay)
            if (entry->inst && entry->ownerAddress && !entry->inGap && entry->track < Ctx().clip.registeredFx.size())
                add(entry->ownerAddress, entry->inst, Ctx().clip.registeredFx[entry->track].effectHash);
        // And the map objects our tracks ride (a prop of the scenery is never
        // placed by the playback: its mark was kept).
        if (g_fxCfg.relocateClearStill)
            for (const std::unique_ptr<PlayRegistered>& entry : g_poseV2RegisteredPlay)
                if (entry->inst && !entry->ownerAddress && entry->attachedOk && entry->worldTargetAddress && entry->worldTarget &&
                    !entry->inGap && entry->track < Ctx().clip.registeredFx.size())
                    add(entry->worldTargetAddress, entry->inst, Ctx().clip.registeredFx[entry->track].effectHash, entry->worldTarget);
        for (const OwnOneShot& own : g_fxOwnOneShots)
            if (own.entity && g_fxCheckPuppetOwners.count(own.entity) && own.timeMs + 5000 >= g_fxOwnLastPlayhead)
                add(own.entity, own.inst, own.hash);
        g_fxOwned.swap(next);
    }

    // Our one-shots one tick after their start: where they are (a weapon
    // effect away from its place) and their settings.
    void CheckFxPending()
    {
        for (std::size_t n = 0; n < g_fxPendingChecks.size();)
        {
            FxPendingCheck& check = g_fxPendingChecks[n];
            if (check.ticks++ < 1)
            {
                ++n;
                continue;
            }
            replay::fx::InstanceIdentity identity;
            if (replay::fx::ReadInstanceIdentity(check.inst, identity) && identity.hash == check.hash &&
                identity.state >= replay::regfx::kStatePlaying && identity.state <= replay::regfx::kStateStopping)
            {
                if (check.weapon)
                {
                    float expected[3] = {check.expected[0], check.expected[1], check.expected[2]};
                    bool known = true;
                    float farM = 0.5f;
                    if (check.entity)
                    {
                        // Attached: within reach of the entity it rides.
                        known = false;
                        farM = 3.0f;
                        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
                            if (puppet.clone && reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone)) == check.entity)
                            {
                                float at[3]{};
                                replay::entities::Position(puppet.clone, at);
                                expected[0] = at[0];
                                expected[1] = at[1];
                                expected[2] = at[2];
                                known = true;
                                break;
                            }
                    }
                    if (known)
                    {
                        const float dx = identity.position[0] - expected[0];
                        const float dy = identity.position[1] - expected[1];
                        const float dz = identity.position[2] - expected[2];
                        g_fxCheck.NotePlacement(std::sqrt(dx * dx + dy * dy + dz * dz), farM);
                    }
                }
                if (check.hasSettings)
                {
                    replay::entity::FxSettings now;
                    if (replay::fx::ReadInstanceSettings(check.inst, now))
                    {
                        const std::uint32_t mask = check.settings.mask & replay::entity::FxSettings::kApplied &
                            ~replay::entity::FxSettings::kVel350;
                        const std::uint32_t diff = replay::entity::FxSettingsDiff(check.settings, now, mask);
                        g_fxCheck.NoteSettings(check.hash, diff != 0);
                        if (diff && g_fxSettingsDiffLogs < 8)
                        {
                            ++g_fxSettingsDiffLogs;
                            Ctx().log << "FX_SETTINGS_DIFF,kind=one_shot,hash=0x" << std::hex << check.hash << ",site=0x" << check.site
                                << ",fields=0x" << diff << std::dec << ",tele=" << now.tele3A8 << "/" << check.settings.tele3A8 << '\n';
                        }
                    }
                }
            }
            g_fxPendingChecks.erase(g_fxPendingChecks.begin() + static_cast<std::ptrdiff_t>(n));
        }
    }

    // The playback's start of the particle fixes: the hooks (installed once),
    // the stretches without a call of clips without RGSS, the shot audit.
    void BeginFxParticles()
    {
        ++g_fxSpanId;
        g_fxTrackCreatedSpan.assign(Ctx().clip.registeredFx.size(), 0);
        g_fxTrackPlaySpan.assign(Ctx().clip.registeredFx.size(), 0);
        g_fxGapSeen.assign(Ctx().clip.registeredFx.size(), {});
        g_scriptLoopPlay.assign(Ctx().clip.registeredFx.size(), {});
        g_fxGapCreates = 0;
        g_fxGapMissedLogs = 0;
        std::fill(std::begin(g_fxGapMissed), std::end(g_fxGapMissed), 0ull);
        std::fill(std::begin(g_fxBlinkLogsByHash), std::end(g_fxBlinkLogsByHash), 0u);
        std::fill(std::begin(g_fxBlinkHashes), std::end(g_fxBlinkHashes), 0u);
        g_fxRecreateLogs = 0;
        std::fill(std::begin(g_fxRecreateLogsByKind), std::end(g_fxRecreateLogsByKind), 0u);
        g_fxRecreateLogsByHash.clear();
        g_fxEntryHeldNotReady = g_fxHoldNotReady = g_fxHoldSkips = 0;
        g_fxPendingEntries.clear();
        g_fxEarlyRefuse.clear();
        g_fxOwnerLastAt.clear();
        g_fxRelocateClearedStill = g_fxRelocateKeptUnplaced = 0;
        g_fxUnplacedLogs = 0;
        g_fxUnplacedLogsByHash.clear();
        g_fxWorldEarlyTicks = g_fxWorldRefuseExpired = 0;
        g_fxWorldRefuseLogs = 0;
        g_fxRecreateInSpan = g_fxRecreateReentered = g_fxRecreateNotReady = 0;
        g_fxOwnerNotReadyTicks = g_fxBackoffTicks = g_fxGapTicks = g_fxGapsEntered = 0;
        g_fxSettingsApplied = g_fxSettingsFailed = g_fxSettingsLegacyHoof = 0;
        g_fxSettingsError.clear();
        g_fxSettingsDiffLogs = 0;
        g_fxParentOnceSkipped = 0;
        g_fxBlinks = 0;
        g_fxBlinkLogs = 0;
        g_fxPauseRestarts = g_fxPlayRestarts = 0;
        g_fxRelocateProbeLogs = g_fxVeladdProbeLogs = 0;
        g_fxRelocateCleared = g_fxRelocateKept = g_fxRelocateCalls = g_fxRelocateFailed = 0;
        g_fxRelocateStale = g_fxGapEndSkipped = 0;
        g_fxPauseExpiredBase = replay::fx::PauseFreezeExpired();
        g_fxRelocateError.clear();
        g_fxWorldOwnerRefusals = g_fxOwnerListImmediate = 0;
        g_fxLastPuppetClones = 0;
        g_fxWorldOwnersLive.clear();
        g_fxOwned.clear();
        g_fxPlacedThisTick.clear();
        g_fxProbeBefore.clear();
        g_fxPendingChecks.clear();
        g_fxImpactOnBodyLogs = 0;
        // The hooks, once per session, each only when its key is on.
        std::string errors;
        if (g_fxCfg.puppetVelocity && !g_fxVelocityReady)
        {
            std::string error;
            g_fxVelocityReady = replay::vegetation::InstallFxVelocity(error);
            if (!g_fxVelocityReady)
                errors += "velocity:" + error + ";";
        }
        replay::vegetation::SetFxVelocity(g_fxCfg.puppetVelocity && g_fxVelocityReady);
        if (g_fxCfg.pauseFreeze && !g_fxPauseFreezeReady)
        {
            std::string error;
            g_fxPauseFreezeReady = replay::fx::InstallPauseFreeze(error);
            if (!g_fxPauseFreezeReady)
                errors += "pause_freeze:" + error + ";";
        }
        std::string lockError;
        const bool lockReady = replay::fx::RelocateMarkReady(lockError);
        if (!lockReady)
            errors += "relocate_lock:" + lockError + ";";
        g_fxHookErrors = errors;
        // Clips without RGSS: the stretches from the frame times.
        g_fxDerivedGaps.assign(Ctx().clip.registeredFx.size(), {});
        std::size_t derivedTracks = 0, rgssTracks = 0, settingsTracks = 0;
        for (std::size_t index = 0; index < Ctx().clip.registeredFx.size(); ++index)
        {
            const replay::entity::RegisteredFxTrack& track = Ctx().clip.registeredFx[index];
            settingsTracks += track.settings.empty() ? 0 : 1;
            if (!track.callGaps.empty())
            {
                ++rgssTracks;
                if (!g_fxCheckRawBasis)
                    g_fxCheck.NoteGap(track.effectHash, true, static_cast<std::uint32_t>(track.callGaps.size()));
                continue;
            }
            // Only a track on a recorded entity (a puppet that moves: its state
            // changes every called tick); a map object's smoke keeps the same
            // state for ticks and its frame times would make false stretches.
            if (!track.attached || !track.attachId.Valid())
                continue;
            std::vector<std::uint32_t> times;
            times.reserve(track.frames.size());
            for (const replay::entity::RegisteredFxFrame& frame : track.frames)
                times.push_back(frame.timeMs);
            g_fxDerivedGaps[index] = replay::fxmotion::DeriveCallGaps(times, 2); // two ticks or more
            if (!g_fxDerivedGaps[index].empty())
            {
                ++derivedTracks;
                g_fxCheck.NoteGapsDerived(track.effectHash);
                if (!g_fxCheckRawBasis)
                    g_fxCheck.NoteGap(track.effectHash, true, static_cast<std::uint32_t>(g_fxDerivedGaps[index].size()));
            }
        }
        // The clip's weapon effects as events or in the shot
        // track (older clips): the owner rule's weapon exemption and the audit.
        g_fxClipWeaponEvents = false;
        std::vector<std::uint32_t> bodyTimes;
        std::size_t settingsEvents = 0;
        for (const replay::entity::FxEvent& event : Ctx().clip.fx.Events())
        {
            settingsEvents += event.hasSettings ? 1 : 0;
            if (event.callerSite == replay::fx::MuzzleFlashSite())
                g_fxClipWeaponEvents = true;
            if (event.callerSite == replay::fx::BodyHitSite())
                bodyTimes.push_back(event.timeMs);
            if (IsWeaponFxSite(event.callerSite))
                g_fxCheck.NoteWeapon(event.callerSite, 1);
        }
        replay::fx::SetWeaponSitesExempt(!g_fxClipWeaponEvents);
        std::vector<replay::shotfx::Shot> shots;
        for (const replay::entity::ShotEvent& shot : Ctx().clip.shots.Shots())
        {
            replay::shotfx::Shot row;
            row.timeMs = shot.timeMs;
            row.shooter = shot.shooter.Packed();
            row.player = shot.shooterPlayer == 2;
            row.muzzleFx = shot.muzzleFx;
            row.impactFx = shot.impactFx;
            row.hasImpact = shot.hasImpact;
            shots.push_back(row);
            if (!g_fxClipWeaponEvents)
            {
                if (shot.muzzleFx)
                    g_fxCheck.NoteWeapon(replay::fx::MuzzleFlashSite(), 1);
                if (shot.impactFx && shot.hasImpact)
                    g_fxCheck.NoteWeapon(replay::fx::BulletImpactSite(), 1);
            }
        }
        g_fxShotAudit = replay::shotfx::Audit{};
        g_fxShotAudit.weaponEvents = g_fxClipWeaponEvents;
        g_fxShotDecisions = replay::shotfx::Decide(shots, bodyTimes, !g_fxClipWeaponEvents, g_fxShotAudit);
        // Per shot, a muzzle effect of its own (an event of the muzzle site on its
        // weapon or shooter within kShotWindowMs, or in an older clip its muzzle
        // hash unless two shots claimed it), a shell (the registered track of the
        // shell site on its weapon or shooter up to 1 s after), and whether the
        // gameplay camera saw the muzzle (the recorded camera at the shot).
        g_fxShotRows.clear();
        {
            const std::vector<replay::entity::ShotEvent>& clipShots = Ctx().clip.shots.Shots();
            const std::vector<replay::camera::GameSample>& cams = Ctx().clip.gameCamera;
            for (std::size_t n = 0; n < clipShots.size(); ++n)
            {
                const replay::entity::ShotEvent& shot = clipShots[n];
                replay::weaponfx::ShotRow row;
                row.timeMs = shot.timeMs;
                row.shooter = shot.shooter.Packed();
                row.player = shot.shooterPlayer == 2;
                row.hasLegacyMuzzle = !g_fxClipWeaponEvents && shot.muzzleFx != 0;
                row.muzzleShared = n < g_fxShotDecisions.size() && g_fxShotDecisions[n].sharedMuzzle;
                const auto ours = [&](const replay::entity::ReplayId& id) {
                    return id.Valid() && (id == shot.shooter || (shot.weapon.Valid() && id == shot.weapon));
                };
                for (const replay::entity::FxEvent& event : Ctx().clip.fx.Events())
                    if (event.callerSite == replay::fx::MuzzleFlashSite() && ours(event.entity) &&
                        replay::weaponfx::AbsDiff(event.timeMs, shot.timeMs) <= replay::weaponfx::kShotWindowMs)
                    {
                        row.muzzleEvent = true;
                        break;
                    }
                for (const replay::entity::RegisteredFxTrack& track : Ctx().clip.registeredFx)
                    if (track.callerSite == replay::fx::ShellRegisteredSite() && ours(track.attachId) &&
                        track.startMs + replay::weaponfx::kShotWindowMs >= shot.timeMs &&
                        track.startMs <= shot.timeMs + replay::weaponfx::kShellWindowMs)
                    {
                        row.shellEvent = true;
                        break;
                    }
                if (!cams.empty())
                {
                    const auto at = std::upper_bound(cams.begin(), cams.end(), shot.timeMs,
                        [](std::uint32_t t, const replay::camera::GameSample& s) { return t < s.timeMs; });
                    const replay::camera::GameSample& cam = at == cams.begin() ? *at : *std::prev(at);
                    const float camera[3] = {cam.position.x, cam.position.y, cam.position.z};
                    const float muzzle[3] = {shot.muzzle.x, shot.muzzle.y, shot.muzzle.z};
                    row.inView = replay::weaponfx::InView(camera, cam.rotation.x, cam.rotation.z, cam.fov, muzzle) ? 1 : 0;
                }
                g_fxShotRows.push_back(row);
            }
        }
        // With WFXI the replay calls the wrapper per request; the recorded muzzle
        // effects and shell tracks of the same shots are dropped (no effect
        // twice). A clip without the section, WeaponFxIntent=0 or the detours not
        // installed: nothing changes.
        {
            namespace wf = replay::weaponfx;
            WfxFlushWindows(true); // a playback that ended without its stop (restart): its windows go unlogged as partial
            g_wfxWindows.clear();
            g_wfxTotals = wf::PlayTotals{};
            g_wfxPlayLogs = 0;
            g_wfxShotExtra.assign(g_fxShotRows.size(), 0);
            g_wfxShotCalled.assign(g_fxShotRows.size(), 0);
            g_wfxInfoEntries.clear();
            g_wfxInfoTried = false;
            g_wfxInfoError = "none";
            g_wfxDroppedEventsPlayed = g_wfxDroppedTracksPlayed = 0;
            const wf::Track& intents = Ctx().clip.weaponFxIntents;
            g_wfxPlayOn = g_fxCfg.weaponFxIntent && g_fxWeaponIntentReady && g_poseV2FxReplayEnabled && !intents.Empty() &&
                intents.Valid();
            const std::vector<replay::entity::FxEvent>& events = Ctx().clip.fx.Events();
            g_wfxDropEvent.assign(events.size(), 0);
            g_wfxDropTrack.assign(Ctx().clip.registeredFx.size(), 0);
            if (g_wfxPlayOn)
            {
                for (std::size_t n = 0; n < events.size(); ++n)
                {
                    const wf::EventRef ref{events[n].callerSite, events[n].timeMs, events[n].entity.Packed()};
                    if (wf::CoversMuzzleEvent(intents, ref, replay::fx::MuzzleFlashSite()))
                    {
                        g_wfxDropEvent[n] = 1;
                        ++g_wfxTotals.droppedEvents;
                    }
                }
                for (std::size_t n = 0; n < Ctx().clip.registeredFx.size(); ++n)
                {
                    const replay::entity::RegisteredFxTrack& track = Ctx().clip.registeredFx[n];
                    const wf::TrackRef ref{track.callerSite, track.startMs, track.endMs, track.attachId.Packed()};
                    if (wf::CoversShellTrack(intents, ref, replay::fx::ShellRegisteredSite()))
                    {
                        g_wfxDropTrack[n] = 1;
                        ++g_wfxTotals.droppedTracks;
                    }
                }
            }
            g_wfxTotals.shotsRecorded = wf::DistinctShots(g_fxShotRows);
            Ctx().log << "WEAPON_FX_INTENT_PLAY_BEGIN,on=" << (g_wfxPlayOn ? 1 : 0) << ",intents=" << intents.intents.size()
                      << ",muzzle=" << intents.Count(wf::Kind::Muzzle) << ",shell=" << intents.Count(wf::Kind::Shell)
                      << ",dropped_events=" << g_wfxTotals.droppedEvents << ",dropped_tracks=" << g_wfxTotals.droppedTracks
                      << ",shots_recorded=" << g_wfxTotals.shotsRecorded << ",key=" << (g_fxCfg.weaponFxIntent ? 1 : 0)
                      << ",detours=" << (g_fxWeaponIntentReady ? 1 : 0) << ",census=" << (replay::census::Enabled() ? 1 : 0)
                      << ",audio_scope=none" << '\n';
        }
        g_fxCheck.NoteShotAudit(g_fxShotAudit.muzzleShared, g_fxShotAudit.impactShared, g_fxShotAudit.impactOnBody, g_fxClipWeaponEvents);
        std::string clipName = "no-file";
        {
            const std::wstring path = replay::census::CurrentClipPath();
            const std::size_t slash = path.find_last_of(L"\\/");
            const std::wstring name = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
            if (!name.empty())
            {
                clipName.clear();
                for (const wchar_t c : name)
                    clipName.push_back(c > 0x20 && c < 0x7F ? static_cast<char>(c) : '_');
            }
        }
        Ctx().log << g_fxShotAudit.Line(clipName) << '\n';
        const auto on = [](bool value) { return value ? 1 : 0; };
        Ctx().log << "FX_MOTION_RULE,no_warp=" << on(g_fxCfg.puppetNoWarp)
            << ",no_warp_lock=" << (lockReady ? "ok" : "sig_mismatch")
            << ",settings_replay=" << on(g_fxCfg.settingsReplay)
            << ",owner_ready_hold=" << on(g_fxCfg.ownerReadyHold)
            << ",backoff_ms=" << g_fxCfg.recreateBackoffMs
            << ",velocity=" << on(g_fxCfg.puppetVelocity && g_fxVelocityReady)
            << ",world_owner_early=" << on(g_fxCfg.worldOwnerEarly) << ",relocate_clear_still=" << on(g_fxCfg.relocateClearStill)
            << ",weapon_fx_intent=" << on(g_fxCfg.weaponFxIntent && g_fxWeaponIntentReady)
            << ",clip_weapon_fx_intents=" << Ctx().clip.weaponFxIntents.intents.size()
            << ",pause_freeze=" << on(g_fxCfg.pauseFreeze && g_fxPauseFreezeReady)
            << ",clip_rgss_tracks=" << rgssTracks << ",clip_settings_tracks=" << settingsTracks << ",derived_gap_tracks=" << derivedTracks
            << ",clip_fxss_events=" << settingsEvents << ",clip_weapon_events=" << on(g_fxClipWeaponEvents)
            << ",errors=" << (errors.empty() ? "none" : errors.c_str()) << '\n';
    }

    void BeginPoseV2FxCheck()
    {
        g_fxCheckRawBasis = g_fxCheck.HasRecord() && g_fxCheckRecordedEvents == Ctx().clip.fx.Events().size() &&
            g_fxCheckRecordedTracks == Ctx().clip.registeredFx.size();
        if (g_fxCheckRawBasis)
            g_fxCheck.ResetPlay();
        else
            g_fxCheck.Reset();
        for (const replay::entity::FxEvent& event : Ctx().clip.fx.Events())
            g_fxCheck.NoteClip(event.effectHash, false);
        for (const replay::entity::RegisteredFxTrack& track : Ctx().clip.registeredFx)
            g_fxCheck.NoteClip(track.effectHash, true);
        g_fxCheckStartCursor = replay::fx::TotalStarts();
        g_fxCheckRegisteredCursor = replay::fx::TotalRegistered();
        g_fxOwnOneShots.clear();
        g_fxRegReason.assign(Ctx().clip.registeredFx.size(), RegNotCreated::None);
        g_fxRegRecreated = 0;
        g_fxCheckReachedMs = 0;
        g_fxCheckSpans.clear();
        g_fxCheckLastMs = static_cast<std::uint32_t>(Ctx().transport.PlayheadMs());
        g_fxPassedNotNormal = 0;
        g_fxPassedLogs = 0;
        g_fxOwnLastPlayhead = 0;
        g_fxOwnOneShotsCleared = 0;
        g_fxOwnOneShotsClears = 0;
        g_fxCheckRefusedKeys.clear();
        g_fxOwnerKilledByHash.clear();
        g_fxOwnerKillLogs = 0;
        g_fxOwnerKilledBase = replay::fx::OneShotOwnerKilled();
        g_fxOwnerError.clear();
        const bool rule = g_poseV2FxReplayEnabled && g_poseV2FxObserverReady;
        const bool ok = replay::fx::SetOneShotOwnerRule(rule, FxOwnerExemptHashes(), g_fxOwnerError);
        // The world side: the sites of the clip's world one-shots (the foot
        // puffs have their own switch, the weapon sites never count).
        std::vector<std::uint32_t> worldSites;
        if (rule)
            for (const replay::entity::FxEvent& event : Ctx().clip.fx.Events())
                if (!event.entity.Valid() && event.callerSite && event.callerSite != replay::fx::MuzzleFlashSite() &&
                    event.callerSite != replay::fx::BulletImpactSite() &&
                    event.callerSite != replay::fx::FootVfxSite() &&
                    std::find(worldSites.begin(), worldSites.end(), event.callerSite) == worldSites.end())
                    worldSites.push_back(event.callerSite);
        replay::fx::SetWorldSiteRule(worldSites);
        Ctx().log << "FX_OWNER_RULE,status=" << (!rule ? "off" : ok ? "on" : "error")
            << ",exempt=" << FxOwnerExemptHashes().size() << ",world_sites=" << worldSites.size()
            << ",basis=" << (g_fxCheckRawBasis ? "recording" : "clip")
            << ",error=" << (g_fxOwnerError.empty() ? "none" : g_fxOwnerError.c_str()) << '\n';
        BeginFxParticles();
    }

    void EndPoseV2FxCheck(std::ostream& log)
    {
        replay::fx::SetPauseFreeze(false);
        replay::fx::SetWeaponSitesExempt(true);
        // The velocity detour answers only for published puppets (the slots are
        // cleared at the stop too), and nothing of this playback's instances is
        // kept for the next one.
        replay::vegetation::SetFxVelocity(false);
        g_fxOwned.clear();
        g_fxPlacedThisTick.clear();
        g_fxProbeBefore.clear();
        g_fxPendingChecks.clear();
        g_fxWorldOwnersLive.clear();
        {
            // The script looped tracks (a fuse, a lantern a script lit): recorded for
            // one tick (older clip), played shorter than recorded, or ok
            // (script_fx_loop.h).
            replay::scriptfx::LoopTally tally;
            for (std::size_t index = 0; index < Ctx().clip.registeredFx.size(); ++index)
            {
                const replay::entity::RegisteredFxTrack& track = Ctx().clip.registeredFx[index];
                if (!ScriptLoopSite(track.callerSite))
                    continue;
                replay::scriptfx::LoopTrackPlay row = index < g_scriptLoopPlay.size() ? g_scriptLoopPlay[index]
                                                                                      : replay::scriptfx::LoopTrackPlay{};
                row.recStartMs = track.startMs;
                row.recEndMs = track.endMs;
                tally.Add(row);
            }
            if (tally.tracks)
                log << "FX_SCRIPT_LOOP_CHECK,tracks=" << tally.tracks << ",one_tick=" << tally.oneTick
                    << ",entered=" << tally.entered << ",complete=" << tally.complete << ",short=" << tally.shortPlay
                    << ",rec_ms=" << tally.recMs << ",played_ms=" << tally.playedMs << ",verdict=" << tally.Verdict() << '\n';
            g_scriptLoopPlay.clear();
        }
        if (!g_poseV2FxObserverReady)
            return;
        TallyPoseV2FxCheckPlay(0xFFFFFFFFu); // the end: our one-shots still alive are cleared
        // The clip basis counts what was due: the events up to the furthest
        // playhead this playback reached (a stop before the end, a slow part).
        // Summed per traversed span, not over their union: a part played twice (a
        // jump back) is due twice.
        g_fxCheck.ClearClip();
        std::uint64_t spanMs = 0;
        bool overlap = false;
        for (std::size_t a = 0; a < g_fxCheckSpans.size(); ++a)
        {
            spanMs += g_fxCheckSpans[a].second - g_fxCheckSpans[a].first;
            for (std::size_t b = a + 1; b < g_fxCheckSpans.size(); ++b)
                overlap = overlap || (g_fxCheckSpans[a].first < g_fxCheckSpans[b].second && g_fxCheckSpans[b].first < g_fxCheckSpans[a].second);
        }
        for (const replay::entity::FxEvent& event : Ctx().clip.fx.Events())
            for (const auto& span : g_fxCheckSpans)
                if (span.first < event.timeMs && event.timeMs <= span.second)
                    g_fxCheck.NoteClip(event.effectHash, false);
        for (const replay::entity::RegisteredFxTrack& track : Ctx().clip.registeredFx)
        {
            // A track counts once per traversed span it was alive in.
            for (const auto& span : g_fxCheckSpans)
                if (track.startMs <= span.second && track.endMs > span.first)
                    g_fxCheck.NoteClip(track.effectHash, true);
        }
        // The restarts due are the stretches without a call that end inside a
        // traversed span (the registry restarts at the call that comes back), per
        // span, so a stop halfway or a jump back does not raise restart_mismatch
        // by itself.
        g_fxCheck.ClearRecGaps();
        for (std::size_t index = 0; index < Ctx().clip.registeredFx.size(); ++index)
        {
            const replay::entity::RegisteredFxTrack& track = Ctx().clip.registeredFx[index];
            const std::vector<replay::entity::RegisteredFxGap>* gaps = !track.callGaps.empty() ? &track.callGaps
                : index < g_fxDerivedGaps.size() ? &g_fxDerivedGaps[index] : nullptr;
            if (!gaps || gaps->empty())
                continue;
            std::uint32_t due = 0;
            for (const auto& span : g_fxCheckSpans)
                for (std::size_t g = 0; g < gaps->size(); ++g)
                {
                    const replay::entity::RegisteredFxGap& gap = (*gaps)[g];
                    if (!(span.first < gap.toMs && gap.toMs <= span.second))
                        continue;
                    ++due;
                    // A stretch due and not entered.
                    const std::vector<std::uint8_t>* seenOf = index < g_fxGapSeen.size() ? &g_fxGapSeen[index] : nullptr;
                    const auto seen = static_cast<replay::fxmotion::GapSeen>(seenOf && g < seenOf->size() ? (*seenOf)[g] : 0);
                    if (seen == replay::fxmotion::GapSeen::Entered)
                        continue;
                    ++g_fxGapMissed[static_cast<int>(seen)];
                    if (g_fxGapMissedLogs < 24 && ++g_fxGapMissedLogs)
                        log << "FX_GAP_MISSED,hash=0x" << std::hex << track.effectHash << std::dec << ",track=" << index
                            << ",gap_ms=" << gap.fromMs << "-" << gap.toMs << ",reason=" << replay::fxmotion::GapSeenName(seen)
                            << ",derived=" << (track.callGaps.empty() ? 1 : 0) << '\n';
                }
            if (due)
                g_fxCheck.NoteGap(track.effectHash, true, due);
        }
        log << "FX_CHECK_REACHED,playhead_ms=" << g_fxCheckReachedMs << ",traversed_ms=" << spanMs
            << ",spans=" << g_fxCheckSpans.size() << ",spans_overlap=" << (overlap ? 1 : 0) << '\n';
        {
            // The shots the playback traversed, without a muzzle effect of their own
            // (shot_no_fx), and the weapon poses reached and armed
            // (weapon_bones_static).
            std::vector<replay::weaponfx::ShotRow> due;
            for (const replay::weaponfx::ShotRow& row : g_fxShotRows)
                for (const auto& span : g_fxCheckSpans)
                    if (span.first < row.timeMs && row.timeMs <= span.second)
                    {
                        due.push_back(row);
                        break;
                    }
            const replay::weaponfx::ShotAudit shotAudit = replay::weaponfx::AuditShots(due, Ctx().clip.weaponFxIntents);
            g_fxCheck.NoteShots(shotAudit.noMuzzle, shotAudit.culled);
            std::uint32_t bonesSeen = 0;
            std::uint32_t bonesArmed = 0;
            replay::objects::WeaponBonesPlayed(bonesSeen, bonesArmed);
            g_fxCheck.NoteWeaponBones(bonesSeen, bonesArmed);
            // Armed is not drawn; the drawn verdicts.
            std::uint32_t drawnOverwritten = 0, drawnMixed = 0, drawnShown = 0, drawnNone = 0;
            replay::objects::WeaponBonesDrawn(drawnOverwritten, drawnMixed, drawnShown, drawnNone);
            g_fxCheck.NoteWeaponBonesDrawn(drawnOverwritten, drawnMixed, drawnShown);
            g_fxCheck.NoteWeaponBonesFrozen(replay::objects::WeaponBonesFrozen());
            {
                // The weapon looks read back (weapon_look_core.h).
                std::uint32_t lookChecked = 0, lookComponents = 0, lookWear = 0;
                replay::objects::WeaponLookPlayed(lookChecked, lookComponents, lookWear);
                g_fxCheck.NoteWeaponLook(lookChecked, lookComponents, lookWear);
            }
            log << shotAudit.Line("playback") << ",weapon_bones_seen=" << bonesSeen << ",weapon_bones_armed=" << bonesArmed << '\n';
            std::uint32_t lines = 0;
            for (const replay::weaponfx::ShotRow& row : due)
            {
                const bool muzzle = row.muzzleEvent || (row.hasLegacyMuzzle && !row.muzzleShared);
                if (muzzle || lines >= 32)
                    continue;
                ++lines;
                bool requested = false;
                for (const replay::weaponfx::Intent& intent : Ctx().clip.weaponFxIntents.intents)
                    if (intent.kind == static_cast<std::uint8_t>(replay::weaponfx::Kind::Muzzle) && intent.shooter == row.shooter &&
                        replay::weaponfx::AbsDiff(intent.timeMs, row.timeMs) <= replay::weaponfx::kShotWindowMs)
                        requested = true;
                log << "FX_SHOT_AUDIT_SHOT,t=" << row.timeMs
                    << ",shooter=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(row.shooter))
                    << ",player=" << (row.player ? 1 : 0) << ",in_view=" << row.inView << ",shell=" << (row.shellEvent ? 1 : 0)
                    << ",shared=" << (row.muzzleShared ? 1 : 0)
                    << ",request=" << (Ctx().clip.weaponFxIntents.Empty() ? "no_section" : requested ? "yes" : "no")
                    << ",verdict=" << (requested ? "culled_by_engine" : row.inView == 0 ? "out_of_view" : "no_muzzle") << '\n';
            }
            // The requests played back, their effects and the sound measured around
            // them.
            WfxFlushWindows(false);
            log << g_wfxTotals.Line() << ",shots_traversed=" << replay::weaponfx::DistinctShots(due)
                << ",dropped_events_due=" << g_wfxDroppedEventsPlayed << ",dropped_tracks_due=" << g_wfxDroppedTracksPlayed
                << ",on=" << (g_wfxPlayOn ? 1 : 0) << ",info=" << g_wfxInfoError << '\n';
            g_wfxPlayOn = false;
        }
        // What the particle fixes did in this playback.
        log << "FX_PARTICLES_TOTAL,recreate_in_span=" << g_fxRecreateInSpan << ",recreate_not_ready=" << g_fxRecreateNotReady
            << ",reentered=" << g_fxRecreateReentered << ",owner_not_ready_ticks=" << g_fxOwnerNotReadyTicks
            << ",backoff_ticks=" << g_fxBackoffTicks << ",gaps_entered=" << g_fxGapsEntered << ",gap_ticks=" << g_fxGapTicks
            << ",play_restarts=" << g_fxPlayRestarts << ",settings_applied=" << g_fxSettingsApplied
            << ",settings_failed=" << g_fxSettingsFailed << ",settings_legacy_hoof=" << g_fxSettingsLegacyHoof
            << ",settings_error=" << (g_fxSettingsError.empty() ? "none" : g_fxSettingsError.c_str())
            << ",parent_once_skipped=" << g_fxParentOnceSkipped << ",blinks=" << g_fxBlinks
            << ",relocate_cleared=" << g_fxRelocateCleared << ",relocate_kept=" << g_fxRelocateKept
            << ",relocate_calls=" << g_fxRelocateCalls << ",relocate_failed=" << g_fxRelocateFailed
            << ",relocate_stale=" << g_fxRelocateStale << ",gap_end_skipped=" << g_fxGapEndSkipped
            << ",pause_freeze_expired=" << (replay::fx::PauseFreezeExpired() - g_fxPauseExpiredBase)
            << ",relocate_detour_skipped=" << replay::fx::RelocateSkipped()
            << ",velocity_overrides=" << replay::vegetation::FxVelocityOverrides()
            << ",entry_held_not_ready=" << g_fxEntryHeldNotReady << ",hold_not_ready=" << g_fxHoldNotReady
            << ",hold_skips=" << g_fxHoldSkips
            << ",world_early_ticks=" << g_fxWorldEarlyTicks << ",world_refuse_expired=" << g_fxWorldRefuseExpired
            << ",relocate_cleared_still=" << g_fxRelocateClearedStill << ",relocate_kept_unplaced=" << g_fxRelocateKeptUnplaced
            << ",gap_creates=" << g_fxGapCreates << ",gap_missed=" << (g_fxGapMissed[0] + g_fxGapMissed[1] + g_fxGapMissed[2] + g_fxGapMissed[3])
            << ",gap_missed_not_seen=" << g_fxGapMissed[0] << ",gap_missed_no_entry=" << g_fxGapMissed[1]
            << ",gap_missed_hold=" << g_fxGapMissed[2] << ",gap_missed_owner_absent=" << g_fxGapMissed[3]
            << ",world_owner_refusals=" << g_fxWorldOwnerRefusals << ",owner_list_immediate=" << g_fxOwnerListImmediate
            << ",pause_frozen_updates=" << replay::fx::PauseFrozenUpdates() << '\n';
        log << "FX_PAUSE_EMIT,restarts=" << g_fxPauseRestarts << ",instances=" << g_poseV2RegisteredPlay.size()
            << ",freeze=" << (g_fxCfg.pauseFreeze && g_fxPauseFreezeReady ? 1 : 0) << '\n';
        log << "FX_PLACEMENT_TOTAL,checked=" << g_fxCheck.PlacementChecked() << ",far=" << g_fxCheck.PlacementFar()
            << ",max_m=" << g_fxCheck.PlacementMaxM() << '\n';
        std::string err;
        replay::fx::SetOneShotOwnerRule(false, {}, err);
        replay::fx::SetWorldSiteRule({});
        log << "FX_OWNER_TOTAL,killed=" << (replay::fx::OneShotOwnerKilled() - g_fxOwnerKilledBase)
            << ",own_one_shots_cleared=" << g_fxOwnOneShotsCleared << ",clears=" << g_fxOwnOneShotsClears
            << ",registered_recreated=" << g_fxRegRecreated << ",one_shots_passed_not_normal=" << g_fxPassedNotNormal << ",by_hash=";
        bool first = true;
        for (const auto& entry : g_fxOwnerKilledByHash)
        {
            log << (first ? "" : ";") << "0x" << std::hex << entry.first << std::dec << ":" << entry.second;
            first = false;
        }
        log << (first ? "none" : "") << '\n';
        {
            // The registered tracks the playback never created, by effect and reason.
            static const char* const kReason[] = {"never_due", "created", "fire_site", "camera_ambient", "paused", "owner_missing", "owner_mismatch", "owner_not_ready", "weapon_intent"};
            std::map<std::pair<std::uint32_t, int>, std::pair<std::uint32_t, std::size_t>> missing; // count, first track
            for (std::size_t index = 0; index < Ctx().clip.registeredFx.size(); ++index)
            {
                const RegNotCreated reason = index < g_fxRegReason.size() ? g_fxRegReason[index] : RegNotCreated::None;
                if (reason == RegNotCreated::Created)
                    continue;
                auto& entry = missing[{Ctx().clip.registeredFx[index].effectHash, static_cast<int>(reason)}];
                if (!entry.first++)
                    entry.second = index;
            }
            for (const auto& entry : missing)
            {
                const replay::entity::RegisteredFxTrack& track = Ctx().clip.registeredFx[entry.second.second];
                log << "FX_REGISTERED_NOT_CREATED,hash=0x" << std::hex << entry.first.first << std::dec
                    << ",reason=" << kReason[entry.first.second] << ",tracks=" << entry.second.first
                    << ",first_ms=" << track.startMs << "-" << track.endMs << ",frames=" << track.frames.size()
                    << ",attached=" << (track.attached ? 1 : 0) << ",attach_id=" << replay::entity::IdText(track.attachId)
                    << std::hex << ",model=0x" << track.attachModel << ",site=0x" << track.callerSite << std::dec
                    << ",bone=" << track.attachBone << ",changes=" << track.attachTimeline.size() << '\n';
            }
        }
        const std::wstring clipPath = replay::census::CurrentClipPath();
        std::wstring reportPath = replay::storage::Path(L"ReplayDebug\\no-file.fx-check.txt");
        std::string clipName = "no-file";
        if (!clipPath.empty())
        {
            const std::size_t slash = clipPath.find_last_of(L"\\/");
            const std::size_t dot = clipPath.find_last_of(L'.');
            const std::wstring stem = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash) ? clipPath.substr(0, dot) : clipPath;
            reportPath = replay::storage::Path(L"ReplayDebug\\") + stem.substr(slash == std::wstring::npos ? 0 : slash + 1) + L".fx-check.txt";
            const std::wstring name = clipPath.substr(slash == std::wstring::npos ? 0 : slash + 1);
            clipName.clear();
            for (const wchar_t w : name)
                clipName += static_cast<char>(w); // ASCII clip names
            for (char& c : clipName)
                if (static_cast<unsigned char>(c) > 0x7E)
                    c = '_';
        }
        std::time_t t = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &t);
        char when[32]{};
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &local);
        replay::storage::Folder(L"ReplayDebug"); // the base folder (storage_paths.h)
        bool written = false;
        {
            std::ofstream file(reportPath, std::ios::binary | std::ios::trunc);
            if (file)
            {
                file << g_fxCheck.Report(clipName, replay::build::kVersion, when, g_fxCheckRawBasis, {});
                written = static_cast<bool>(file);
            }
        }
        std::string reportText;
        if (written)
            for (const wchar_t c : reportPath)
                reportText.push_back(static_cast<char>(c < 0x7F ? c : '_'));
        log << g_fxCheck.SummaryLine(clipName, g_fxCheckRawBasis, written ? reportText : std::string("none")) << '\n';
        // The findings the bench fails on, and the motion and restart rows by
        // hash.
        for (const replay::fxcheck::Flag& flag : g_fxCheck.Flags())
            log << replay::fxcheck::Tally::FlagLine(clipName, flag) << '\n';
        for (const std::string& line : g_fxCheck.MotionLines(clipName))
            log << line << '\n';
        // The per-hash rows in the log too (the bench reads the slice).
        for (const auto& entry : g_fxCheck.Rows())
        {
            const replay::fxcheck::Row& row = entry.second;
            const std::uint32_t basis = g_fxCheckRawBasis ? row.rec : row.clip;
            if (!basis && !row.Play() && !row.refused)
                continue;
            log << "FX_CHECK_ROW,hash=0x" << std::hex << entry.first << std::dec << ",kind=" << (row.registered ? "reg" : "one")
                << ",rec=" << row.rec << ",stored=" << row.recStored << ",shot=" << row.recShot << ",skipped=" << row.recSkipped
                << ",unresolved=" << row.recUnresolved << ",clip=" << row.clip << ",play=" << row.Play() << ",ours=" << row.playOurs
                << ",engine_puppet=" << row.playEnginePuppet << ",engine_other=" << row.playEngineOther << ",refused=" << row.refused
                << std::hex << ",site=0x" << row.site << std::dec << '\n';
        }
    }
}
