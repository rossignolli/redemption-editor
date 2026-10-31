// replay_events_weapons.cpp: shots: the gunshot audio delay, the player's weapon bank and mount audio, the shots played back, the weapon fx intents.
// Split from replay_events.cpp (moved code); shared internals in replay_events_internal.h.

#include "replay_events_internal.h"

namespace replay::events
{
    namespace
    {

    // The gunshot's start after its fire event, per shooter, recorded and
    // replayed (shot_audio_delay.h). A fire (the game's in the recording,
    // the replay's EnqueueFire in the playback) is matched with the census
    // row of the sound created on its weapon's audio (CWeapon +0x100, the
    // event's +0x00), the bank of that sound's ref is probed, and the
    // sound's play state is read every tick until it plays. Read only
    // (guarded reads, the game's own bank lookups); nothing in the world
    // changes, so nothing to restore.
    replay::shotdelay::Tracker g_shotDelay;
    const char* g_shotDelayPhase = "rec";
    bool g_shotDelayOn = false; // between Begin and End
    int g_shotDelayShotLines = 0;
    constexpr int kShotDelayShotLines = 40; // POSE_V2_SHOT_AUDIO_DELAY_SHOT lines per phase
    constexpr std::size_t kWeaponAudioInCWeapon = 0x100;
    std::vector<replay::shotdelay::Create> g_shotDelayRows;
    std::vector<std::size_t> g_shotDelayIndexes;
    }

    void BeginShotAudioDelay(const char* phase)
    {
        g_shotDelayOn = true;
        g_shotDelay.Reset();
        g_shotDelayPhase = phase;
        g_shotDelayShotLines = 0;
    }
    namespace
    {

    // The listener's distance: the rendered camera to the muzzle.
    float ShotAudioDistance(const replay::pose::Vec3& muzzle)
    {
        const Vector3 camera = CAM::GET_FINAL_RENDERED_CAM_COORD();
        const float dx = camera.x - muzzle.x;
        const float dy = camera.y - muzzle.y;
        const float dz = camera.z - muzzle.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    }

    void NoteShotAudioFire(const replay::entity::ReplayId& shooter, const std::uint8_t* info, std::uint64_t fireUs,
        std::uint32_t eventMs, const replay::pose::Vec3& muzzle, bool player)
    {
        if (!g_shotDelayOn || !info)
            return;
        std::uint64_t cweapon = 0;
        std::memcpy(&cweapon, info, 8);
        if (!cweapon)
            return;
        replay::shotdelay::Fire fire;
        fire.shooter = shooter.Packed();
        fire.audio = cweapon + kWeaponAudioInCWeapon;
        fire.fireUs = fireUs;
        fire.eventMs = eventMs;
        fire.distanceM = ShotAudioDistance(muzzle);
        fire.player = player; // POSE_V2_SHOT_PLAYER_BANK
        g_shotDelay.AddFire(fire);
    }
    namespace
    {

    void LogShotAudioDelayFinished()
    {
        for (const replay::shotdelay::Shot& shot : g_shotDelay.TakeFinished())
        {
            if (g_shotDelayShotLines >= kShotDelayShotLines)
                break;
            ++g_shotDelayShotLines;
            Ctx().log << replay::shotdelay::ShotLine(g_shotDelayPhase, shot,
                replay::entity::IdText(replay::entity::ReplayId::FromPacked(shot.fire.shooter)));
        }
    }
    }

    // Every recording or playback tick, after the shots of the tick (the
    // census rows of this tick are still in census::Rows()).
    void TickShotAudioDelay()
    {
        if (!g_shotDelayOn || !g_shotDelay.Size())
            return;
        REPLAY_PROFILE_ZONE("shots.audio_delay");
        g_shotDelayRows.clear();
        for (const replay::census::TickRow& tick : replay::census::Rows())
        {
            replay::shotdelay::Create create;
            create.timeUs = tick.row.timeUs;
            create.audio = tick.row.audio;
            create.sound = tick.row.sound;
            create.ref = tick.row.ref;
            create.firePath = replay::shotdelay::InFirePlay(tick.row.callers, replay::census::kRowCallers,
                replay::audio::FirePlayExtent());
            g_shotDelayRows.push_back(create);
        }
        g_shotDelay.OnCreates(g_shotDelayRows, g_shotDelayIndexes);
        for (const std::size_t index : g_shotDelayIndexes)
        {
            replay::shotdelay::Shot& shot = g_shotDelay.At(index);
            shot.identity = replay::audio::SoundIdentity(shot.sound);
            replay::audio::BankProbe bank;
            std::string error;
            if (shot.ref && replay::audio::ProbeSoundBank(shot.ref, bank, error))
                shot.bank = replay::shotdelay::BankOfName(replay::audio::BankStateName(bank));
        }
        g_shotDelay.Waiting(g_shotDelayIndexes);
        const std::uint64_t nowUs = replay::clock::NowUs();
        for (const std::size_t index : g_shotDelayIndexes)
        {
            replay::shotdelay::Shot& shot = g_shotDelay.At(index);
            int state = -1;
            const bool alive = replay::audio::SoundPlayState(shot.sound, shot.identity, state);
            g_shotDelay.Sample(index, alive, state, nowUs);
        }
        g_shotDelay.Expire(nowUs);
        LogShotAudioDelayFinished();
    }

    // The phase's end: everything still open closes, then the summary
    // (`refs`: the clip's reference, playback only).
    void EndShotAudioDelay(const std::map<std::uint32_t, replay::shotdelay::Reference>* refs)
    {
        if (!g_shotDelayOn)
            return;
        g_shotDelay.Expire(replay::clock::NowUs(), true);
        LogShotAudioDelayFinished();
        Ctx().log << replay::shotdelay::SummaryLines(g_shotDelayPhase, g_shotDelay.Tallies(), refs,
            [](std::uint32_t id) { return replay::entity::IdText(replay::entity::ReplayId::FromPacked(id)); });
        g_shotDelay.Reset();
        g_shotDelayOn = false;
    }

    // The recording's own gunshot creates of the clip (SGEN), per shooter,
    // against the shots' recorded ticks: the baseline a clip carries.
    std::map<std::uint32_t, replay::shotdelay::Reference> ShotAudioClipReference()
    {
        std::vector<replay::shotdelay::RefShot> shots;
        for (const replay::entity::ShotEvent& shot : Ctx().clip.shots.Shots())
            if (shot.weapon.Valid())
                shots.push_back({shot.shooter.Packed(), shot.weapon.Packed(), shot.timeMs});
        std::vector<replay::shotdelay::RefPacket> packets;
        const replay::shotdelay::FirePlay& fire = replay::audio::FirePlayExtent();
        for (const replay::soundgeneric::CreatePacket& c : Ctx().clip.soundGeneric.creates)
        {
            const bool weapon = c.audioKnown && c.audioBase == replay::census::AudioBase::WeaponAudio;
            if (weapon)
                packets.push_back({c.owner, c.timeMs, true,
                    replay::shotdelay::InFirePlay(&c.caller, 1, fire) || replay::shotdelay::InFirePlay(c.path, 3, fire)});
        }
        std::stable_sort(packets.begin(), packets.end(),
            [](const replay::shotdelay::RefPacket& a, const replay::shotdelay::RefPacket& b) { return a.timeMs < b.timeMs; });
        return replay::shotdelay::ClipReference(shots, packets);
    }

    // `hand`: the weapon attach point that fired (0 primary, 1 secondary
    // when dual wielding); `fire`: the engine fire event of this shot when
    // the shot comes from the fire event queue.
    // Evidence, read only. Per shot, record and play: the shooter's gunfight
    // offsets the shot sound adds to the shot (volume, high-pass) and the
    // audio flags dword (audio_observer.cpp kAudioFlags).
    void LogShotGunfight(const char* phase, const replay::entity::ReplayId& shooter, void* ped, std::uint32_t timeMs)
    {
        float volume = 0.0f;
        std::int32_t highPass = 0;
        std::uint32_t flags = 0;
        const bool ok = ped && replay::audio::PedGunfightOffsets(ped, volume, highPass);
        const bool flagsOk = replay::audio::AudioGlobalFlags(flags);
        Ctx().log << "SHOT_GUNFIGHT,phase=" << phase << ",shooter=" << replay::entity::IdText(shooter) << ",time_ms=" << timeMs
                  << ",ok=" << (ok ? 1 : 0) << ",vol=" << volume << ",hpf=" << highPass << ",flags_ok=" << (flagsOk ? 1 : 0)
                  << ",flags=0x" << std::hex << flags << std::dec << ",bit2=" << ((flags >> 2) & 1) << ",bit3=" << ((flags >> 3) & 1)
                  << ",bit4=" << ((flags >> 4) & 1) << ",bit5=" << ((flags >> 5) & 1) << '\n';
    }

    // Surface normal at the impact: one read-only ray from the muzzle
    // through the impact point.
    void MeasurePoseV2ImpactNormal(replay::entity::ShotEvent& shot, Ped shooter)
    {
        float dx = shot.impact.x - shot.muzzle.x;
        float dy = shot.impact.y - shot.muzzle.y;
        float dz = shot.impact.z - shot.muzzle.z;
        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (length < 0.01f)
            return;
        dx /= length;
        dy /= length;
        dz /= length;
        const float startBack = (std::min)(0.5f, length);
        const ScrHandle probe = SHAPETEST::START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE(
            shot.impact.x - dx * startBack, shot.impact.y - dy * startBack, shot.impact.z - dz * startBack,
            shot.impact.x + dx * 0.25f, shot.impact.y + dy * 0.25f, shot.impact.z + dz * 0.25f,
            -1, shooter, 7);
        BOOL hit = FALSE;
        Vector3 end{};
        Vector3 normal{};
        Entity entity = 0;
        if (SHAPETEST::GET_SHAPE_TEST_RESULT(probe, &hit, &end, &normal, &entity) == 2 && hit)
        {
            shot.impactNormal = ToPoseVec(normal);
            shot.hasNormal = true;
        }
    }

    // Speech lines of the recording.
    // Footsteps of the recording.
    std::uint64_t g_poseV2FootstepCursor = 0;
    std::uint64_t g_poseV2ExplosionCursor = 0;
    std::uint64_t g_poseV2ExplosionsPlayed = 0;
    std::uint64_t g_poseV2ExplosionsRefused = 0;
    std::string g_poseV2ExplosionLastError;
    std::uint64_t g_poseV2FootstepsUnrecorded = 0;
    // Each recorded step gets the step container its movement update played
    // it with (samples paired with foot-downs, foot_material.h).
    std::uint64_t g_poseV2FootMaterialCursor = 0;
    replay::footmaterial::Pairer g_poseV2FootMaterialPairer;
    std::uint64_t g_poseV2FootMaterialPaired = 0;
    std::uint64_t g_poseV2FootMaterialInvalid = 0;
    std::uint64_t g_poseV2FootMaterialLost = 0;
    // Generic fx track: recording cursor and counters.
    std::uint64_t g_poseV2FxStartCursor = 0;
    std::uint64_t g_poseV2FxSkippedShot = 0;
    std::uint64_t g_poseV2FxSkippedEntity = 0;
    std::uint64_t g_poseV2FxNoHash = 0;
    std::uint64_t g_poseV2FxPlayed = 0;
    std::uint64_t g_poseV2FxNoPuppet = 0;
    std::uint64_t g_poseV2FxNoWorldPlay = 0; // world fx of an old clip near the map origin, not played
    std::uint64_t g_poseV2FxFailed = 0;
    std::string g_poseV2FxLastError;
    replay::entity::ForwardEventCursor g_poseV2FxCursorPlay;
    // Evidence: recorded events by hash (attached/world), attached starts
    // skipped for their entity by hash, starts without a trigger by caller.
    std::map<std::uint32_t, std::uint32_t> g_poseV2FxRecordedByHash;
    std::map<std::uint32_t, std::uint32_t> g_poseV2FxSkippedByHash;
    std::map<std::uint32_t, std::uint32_t> g_poseV2FxNoHashByCaller;
    std::map<std::uint32_t, std::uint32_t> g_poseV2FxStartsByCaller;
    std::uint32_t g_poseV2FxSamples = 0;
    // Registered (continuous) effects evidence: per hash, calls, creates,
    // distinct keys; owner samples.
    std::uint64_t g_poseV2FxRegisteredCursor = 0;
    std::vector<PoseV2WeaponMemo> g_poseV2WeaponMemo;
    std::uint64_t g_poseV2FireRefFromMemo = 0;
    void RememberPoseV2Weapons()
    {
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        const auto remember = [&](Object object, replay::entity::ReplayId id) {
            if (!object || !replay::entities::Exists(object))
                return;
            const std::uint64_t weapon = replay::audio::WeaponAudio(replay::entities::AddressOf(object));
            if (!weapon)
                return;
            for (PoseV2WeaponMemo& memo : g_poseV2WeaponMemo)
            {
                if (memo.weapon == weapon)
                {
                    memo.id = id;
                    memo.lastMs = nowMs;
                    return;
                }
            }
            g_poseV2WeaponMemo.push_back({weapon, id, nowMs});
        };
        for (const replay::recorder::RecordedWeapon& weapon : Ctx().recorder.weapons)
            remember(weapon.object, weapon.id);
        g_poseV2WeaponMemo.erase(std::remove_if(g_poseV2WeaponMemo.begin(), g_poseV2WeaponMemo.end(),
            [&](const PoseV2WeaponMemo& memo) { return nowMs > memo.lastMs + 2000; }), g_poseV2WeaponMemo.end());
    }
    std::vector<PoseV2ShotFirePending> g_poseV2ShotFirePending;
    std::uint64_t g_poseV2ShotFireViaFallback = 0;
    namespace
    {

    std::uint64_t PoseV2WeaponHeldBySameWeapon(const replay::entity::ShotEvent& shot, replay::entity::ReplayId holderId,
        std::uint16_t viaOffset)
    {
        // Only the weapon field (+0x448) is taken this way: a validated weapon
        // object (vtable in the image), never an arbitrary word.
        if (viaOffset != 0x448)
            return 0;
        const replay::playback::Puppet* holder = Ctx().puppets.Find(holderId);
        const std::uint32_t weaponHash = holder ? holder->packet.weaponHash : 0;
        const replay::playback::Puppet* best = nullptr;
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.state != replay::playback::PuppetState::Ready || !puppet.clone || !replay::entities::Exists(puppet.clone))
                continue;
            if (!puppet.packet.weaponHash || (weaponHash && puppet.packet.weaponHash != weaponHash))
                continue;
            if (!weaponHash && puppet.packet.owner != shot.shooter)
                continue;
            if (!best || (puppet.packet.owner == shot.shooter && best->packet.owner != shot.shooter))
                best = &puppet;
        }
        if (!best)
            return 0;
        const std::uint64_t value = replay::audio::WeaponAudio(replay::entities::AddressOf(best->clone));
        if (value)
            ++g_poseV2ShotFireViaFallback;
        return value;
    }

    // Player weapon sound. Live, the weapon audio plays the local player's
    // shots with the player variant (settings, fire sound, volume, tail) and
    // every other ped's with the NPC one; it asks one predicate
    // (replay::audio::InstallPlayerWeaponAudio). The puppets of the shooters
    // recorded as the local player answer yes to that predicate while the clip
    // plays. A clip without the flag: the puppet of a story player model stands
    // in (source=model).
    std::vector<replay::entity::ReplayId> g_poseV2PlayerAudioIds;
    }
    const char* g_poseV2PlayerAudioSource = "none";
    std::vector<std::uint64_t> g_poseV2PlayerAudioPeds;
    replay::audio::PlayerAudioStats g_poseV2PlayerAudioBase;
    namespace
    {
    std::uint32_t g_poseV2PlayerAudioLogs = 0;
    // Weapon objects whose audio settings were set up again for the player
    // (replay::audio::RefreshWeaponAudioSettings), once each.
    std::vector<std::uint64_t> g_poseV2PlayerAudioWeapons;
    constexpr std::uint32_t kPoseV2PlayerAudioLogCap = 20;
    }

    void BeginPoseV2PlayerAudio()
    {
        g_poseV2PlayerAudioIds.clear();
        bool flagged = false;
        for (const replay::entity::ShotEvent& shot : Ctx().clip.shots.Shots())
        {
            if (shot.shooterPlayer != 0)
                flagged = true;
            if (shot.shooterPlayer == 2 &&
                std::find(g_poseV2PlayerAudioIds.begin(), g_poseV2PlayerAudioIds.end(), shot.shooter) ==
                    g_poseV2PlayerAudioIds.end())
            {
                g_poseV2PlayerAudioIds.push_back(shot.shooter);
            }
        }
        g_poseV2PlayerAudioSource = flagged ? "recorded" : Ctx().clip.shots.Shots().empty() ? "none" : "model";
        g_poseV2PlayerAudioPeds.clear();
        replay::audio::SetPlayerAudioPeds({});
        g_poseV2PlayerAudioBase = replay::audio::PlayerAudioSnapshot();
        g_poseV2PlayerAudioLogs = 0;
        g_poseV2PlayerAudioWeapons.clear();
    }
    namespace
    {

    // Every frame of playback: the puppets' current addresses (a scrub
    // recreates them).
    void UpdatePoseV2PlayerAudio(std::uint32_t timeMs)
    {
        static std::vector<std::uint64_t> peds;
        static std::string ids;
        peds.clear();
        ids.clear();
        const auto add = [&](const replay::playback::Puppet& puppet) {
            if (puppet.state != replay::playback::PuppetState::Ready || !puppet.clone ||
                !replay::entities::Exists(puppet.clone))
            {
                return;
            }
            const std::uint64_t address = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone));
            if (!address || std::find(peds.begin(), peds.end(), address) != peds.end())
                return;
            peds.push_back(address);
            ids += (ids.empty() ? "" : "|") + replay::entity::IdText(puppet.packet.id);
        };
        if (std::strcmp(g_poseV2PlayerAudioSource, "model") == 0)
        {
            static const Hash arthur = MISC::GET_HASH_KEY("player_zero");
            static const Hash john = MISC::GET_HASH_KEY("player_three");
            for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
            {
                if (puppet.packet.kind == replay::entity::EntityKind::Ped &&
                    (puppet.packet.modelHash == arthur || puppet.packet.modelHash == john))
                {
                    add(puppet);
                }
            }
        }
        else
        {
            for (const replay::entity::ReplayId& id : g_poseV2PlayerAudioIds)
            {
                if (const replay::playback::Puppet* puppet = Ctx().puppets.Find(id))
                    add(*puppet);
            }
        }
        if (peds == g_poseV2PlayerAudioPeds)
            return;
        g_poseV2PlayerAudioPeds = peds;
        replay::audio::SetPlayerAudioPeds(peds);
        if (g_poseV2PlayerAudioLogs >= kPoseV2PlayerAudioLogCap)
            return;
        ++g_poseV2PlayerAudioLogs;
        const replay::audio::PlayerAudioStats stats = replay::audio::PlayerAudioSnapshot();
        // audio_flag: the engine's own answer for the puppet (0 expected;
        // the detour answers yes in its place).
        Ctx().log << "POSE_V2_PLAYER_AUDIO,phase=play,event=set,source=" << g_poseV2PlayerAudioSource
            << ",ids=" << (ids.empty() ? "none" : ids.c_str()) << ",peds=" << peds.size()
            << ",audio_flag=" << (peds.empty() ? -1 : replay::audio::PedAudioPlayerFlag(reinterpret_cast<void*>(peds[0])))
            << ",hook=" << (stats.installed ? 1 : 0) << ",playhead_ms=" << timeMs << '\n';
    }

    bool PoseV2PlayerAudioPuppet(const replay::playback::Puppet& puppet)
    {
        if (!puppet.clone)
            return false;
        const std::uint64_t address = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone));
        return address && std::find(g_poseV2PlayerAudioPeds.begin(), g_poseV2PlayerAudioPeds.end(), address) !=
            g_poseV2PlayerAudioPeds.end();
    }
    }

    void LogPoseV2PlayerAudioTotal(const char* phase, std::size_t playerShots)
    {
        const replay::audio::PlayerAudioStats now = replay::audio::PlayerAudioSnapshot();
        Ctx().log << "POSE_V2_PLAYER_AUDIO,phase=" << phase << ",hook=" << (now.installed ? 1 : 0)
            << ",source=" << g_poseV2PlayerAudioSource << ",player_shots=" << playerShots
            << ",calls=" << (now.calls - g_poseV2PlayerAudioBase.calls)
            << ",engine_true=" << (now.engineTrue - g_poseV2PlayerAudioBase.engineTrue)
            << ",overrides=" << (now.overrides - g_poseV2PlayerAudioBase.overrides)
            << ",sites=" << replay::audio::PlayerAudioSites(now, g_poseV2PlayerAudioBase) << '\n';
    }

    // The player's shot plays from the player's equipped-weapon wave slots,
    // filled with the banks of the weapons the hidden live player equips
    // (player_weapon_bank_core.h). PlayerWeaponBank=1: while a clip plays, the
    // live player equips the weapon of the recorded player's next shot (natives,
    // script thread), so the engine loads its bank ahead of it. Weapons given
    // only for this are taken back and the held weapon is restored at the end
    // (the closed theater's save reload restores the inventory anyway).
    // From a tester's report: the request is checked. A weapon
    // he owns but does not carry (a long arm stored on the horse) is never
    // equipped from there (that takes it off the horse): a second copy goes
    // into his hand (GIVE_WEAPON_TO_PED, bAllowMultipleCopies) and exactly that
    // copy leaves at the end, by its inventory GUID. A carried weapon that does
    // not come to the hand after kEquipTries requests (a playback started
    // while mounted) gets the same copy; event=equip_failed says it. His
    // loadout (hand, attach points, horse holster, ownership and ammo of every
    // weapon the playback touches) is read before the first change and again
    // after the restore: event=inventory,match=1.
    namespace
    {
    std::vector<replay::weaponbank::Shot> g_pwbShots;
    bool g_pwbBuilt = false;
    Hash g_pwbWanted = 0;
    Hash g_pwbOriginal = 0;
    bool g_pwbTouched = false;
    std::vector<Hash> g_pwbGiven;
    // A temporary copy given beside a weapon he owns, and its inventory GUID
    // (read from the hand once the copy is there; the buffer oversized and
    // zeroed, as replay_bench.cpp's inventory step). moved: the give took the
    // stored weapon off the horse holster instead of making a copy (the
    // holster changed); it goes back to the holster at the end, not removed.
    struct PwbCopy
    {
        Hash weapon = 0;
        alignas(8) std::uint64_t guid[32]{};
        bool guidRead = false;
        bool moved = false;
    };
    std::vector<PwbCopy> g_pwbCopies;
    replay::weaponbank::EquipState g_pwbEquip;
    std::uint32_t g_pwbSwitches = 0;
    std::uint32_t g_pwbRetries = 0;
    std::uint32_t g_pwbFailed = 0;
    std::uint32_t g_pwbLogs = 0;
    // The loadout before the first change, the ped and horse it was read on,
    // and the check after the restore (Monitor, kPwbVerifyMs later: the hand
    // switch takes a few frames).
    replay::weaponbank::Loadout g_pwbBefore;
    Ped g_pwbPlayer = 0;
    Ped g_pwbHorse = 0;
    }
    std::uint64_t g_pwbVerifyAtMs = 0;
    namespace
    {
    constexpr std::uint64_t kPwbVerifyMs = 1500;
    // The weapon object in the hidden player's hand: hidden with him (a
    // visible gun would float where the replay parks him, at the recorded
    // player's place), shown again at the end.
    std::vector<Entity> g_pwbHiddenObjects;
    std::uint32_t g_pwbObjectsSeenVisible = 0;
    constexpr std::uint32_t kPwbLogCap = 20;
    constexpr Hash kPwbUnarmed = 0xA2719263; // joaat("WEAPON_UNARMED")
    constexpr Hash kPwbAddReasonDefault = 0x2CD419DC;
    }

    void BeginPoseV2PlayerWeaponBank()
    {
        g_pwbShots.clear();
        g_pwbBuilt = false;
        g_pwbWanted = 0;
        g_pwbOriginal = 0;
        g_pwbTouched = false;
        g_pwbGiven.clear();
        g_pwbCopies.clear();
        g_pwbEquip = {};
        g_pwbSwitches = 0;
        g_pwbRetries = 0;
        g_pwbFailed = 0;
        g_pwbLogs = 0;
        g_pwbHiddenObjects.clear();
        g_pwbObjectsSeenVisible = 0;
    }
    namespace
    {

    // The horse whose holster counts as his: the one he rides, else his
    // saddle horse, else the last he rode.
    Ped PoseV2PlayerWeaponBankHorse(Ped player)
    {
        if (PED::IS_PED_ON_MOUNT(player))
            if (const Ped mount = PED::GET_MOUNT(player))
                return mount;
        if (const Ped saddle = PLAYER::_GET_SADDLE_HORSE_FOR_PLAYER(PLAYER::PLAYER_ID()))
            if (ENTITY::DOES_ENTITY_EXIST(saddle))
                return saddle;
        const Ped last = PED::_GET_LAST_MOUNT(player);
        return last && ENTITY::DOES_ENTITY_EXIST(last) ? last : 0;
    }

    bool PoseV2PlayerWeaponOnPerson(Ped player, Hash weapon)
    {
        return WEAPON::HAS_PED_GOT_WEAPON(player, weapon, 0, TRUE) || WEAPON::IS_PED_CARRYING_WEAPON(player, weapon);
    }

    replay::weaponbank::Loadout ReadPoseV2PlayerLoadout(Ped player, Ped horse, const std::vector<std::uint32_t>& weapons)
    {
        replay::weaponbank::Loadout out;
        Hash held = 0;
        WEAPON::GET_CURRENT_PED_WEAPON(player, &held, FALSE, 0, FALSE);
        out.held = held ? held : kPwbUnarmed;
        for (std::size_t ap = 0; ap < out.attach.size(); ++ap)
        {
            Hash at = 0;
            WEAPON::GET_CURRENT_PED_WEAPON(player, &at, FALSE, static_cast<int>(ap), FALSE);
            out.attach[ap] = at;
        }
        out.horseHolster = horse && ENTITY::DOES_ENTITY_EXIST(horse) ? WEAPON::_GET_WEAPON_FROM_HORSE_HOLSTER(horse) : 0;
        for (const std::uint32_t weapon : weapons)
        {
            replay::weaponbank::WeaponFacts facts;
            facts.weapon = weapon;
            facts.owned = WEAPON::HAS_PED_GOT_WEAPON(player, weapon, 0, FALSE) != FALSE;
            facts.onPerson = PoseV2PlayerWeaponOnPerson(player, weapon);
            facts.ammo = WEAPON::GET_AMMO_IN_PED_WEAPON(player, weapon);
            out.weapons.push_back(facts);
        }
        return out;
    }

    void HidePoseV2PlayerWeaponObject(Ped player, std::uint32_t timeMs)
    {
        const Entity object = WEAPON::GET_CURRENT_PED_WEAPON_ENTITY_INDEX(player, 0);
        if (!object || !ENTITY::DOES_ENTITY_EXIST(object) || !ENTITY::IS_ENTITY_VISIBLE(object))
            return;
        ENTITY::SET_ENTITY_VISIBLE(object, FALSE);
        ++g_pwbObjectsSeenVisible;
        if (std::find(g_pwbHiddenObjects.begin(), g_pwbHiddenObjects.end(), object) == g_pwbHiddenObjects.end())
            g_pwbHiddenObjects.push_back(object);
        if (g_pwbLogs < kPwbLogCap)
        {
            ++g_pwbLogs;
            Ctx().log << "PLAYER_WEAPON_BANK,event=hide_object,playhead_ms=" << timeMs << ",object=" << object << '\n';
        }
    }

    // The copy of `weapon` given in this playback, if any.
    PwbCopy* FindPoseV2PlayerWeaponCopy(Hash weapon)
    {
        for (PwbCopy& copy : g_pwbCopies)
            if (copy.weapon == weapon)
                return &copy;
        return nullptr;
    }

    // The GUID of a copy, read from the primary hand once the copy is there.
    void ReadPoseV2PlayerWeaponCopyGuid(Ped player, PwbCopy& copy)
    {
        if (copy.guidRead || copy.moved)
            return;
        Hash inHand = 0;
        WEAPON::GET_CURRENT_PED_WEAPON(player, &inHand, FALSE, 0, FALSE);
        if (inHand != copy.weapon)
            return;
        std::memset(copy.guid, 0, sizeof(copy.guid));
        copy.guidRead = WEAPON::GET_PED_WEAPON_GUID_AT_ATTACH_POINT(player, 0, reinterpret_cast<Any*>(copy.guid)) &&
            INVENTORY::_INVENTORY_IS_GUID_VALID(reinterpret_cast<Any*>(copy.guid));
    }

    // The player's shots: flagged ones (shooter_player=2), or for a clip
    // without the flag, the shots of a story player model's puppet (the same
    // stand-in as the weapon predicate, source=model).
    void BuildPoseV2PlayerWeaponBankShots()
    {
        g_pwbShots.clear();
        const bool model = std::strcmp(g_poseV2PlayerAudioSource, "model") == 0;
        static const Hash arthur = MISC::GET_HASH_KEY("player_zero");
        static const Hash john = MISC::GET_HASH_KEY("player_three");
        for (const replay::entity::ShotEvent& shot : Ctx().clip.shots.Shots())
        {
            bool player = shot.shooterPlayer == 2;
            if (!player && model)
            {
                const replay::playback::Puppet* puppet = Ctx().puppets.Find(shot.shooter);
                player = puppet && (puppet->packet.modelHash == arthur || puppet->packet.modelHash == john);
            }
            if (player)
                g_pwbShots.push_back({shot.timeMs, shot.weaponHash});
        }
        std::sort(g_pwbShots.begin(), g_pwbShots.end(),
            [](const replay::weaponbank::Shot& a, const replay::weaponbank::Shot& b) { return a.timeMs < b.timeMs; });
        g_pwbBuilt = true;
        Ctx().log << "PLAYER_WEAPON_BANK,event=begin,source=" << g_poseV2PlayerAudioSource
                  << ",player_shots=" << g_pwbShots.size() << '\n';
    }

    void UpdatePoseV2PlayerWeaponBank(std::uint32_t timeMs)
    {
        namespace wb = replay::weaponbank;
        if (!g_pwbBuilt)
            BuildPoseV2PlayerWeaponBankShots();
        const Hash wanted = wb::Wanted(g_pwbShots, timeMs);
        if (!wanted)
            return;
        const Ped player = PLAYER::PLAYER_PED_ID();
        if (!player || !ENTITY::DOES_ENTITY_EXIST(player))
            return;
        Hash current = 0;
        WEAPON::GET_CURRENT_PED_WEAPON(player, &current, FALSE, 0, FALSE);
        PwbCopy* copy = FindPoseV2PlayerWeaponCopy(wanted);
        if (copy && current == wanted)
            ReadPoseV2PlayerWeaponCopyGuid(player, *copy);
        wb::Probe probe;
        probe.held = current;
        if (current != wanted)
        {
            probe.owned = WEAPON::HAS_PED_GOT_WEAPON(player, wanted, 0, FALSE) != FALSE;
            probe.onPerson = PoseV2PlayerWeaponOnPerson(player, wanted);
            probe.copyGiven = copy || std::find(g_pwbGiven.begin(), g_pwbGiven.end(), wanted) != g_pwbGiven.end();
        }
        const wb::Decision decision = wb::Decide(g_pwbEquip, wanted, probe, GetTickCount64());
        g_pwbWanted = wanted;
        if (current == wanted)
        {
            HidePoseV2PlayerWeaponObject(player, timeMs);
            return;
        }
        if (decision.failed)
        {
            ++g_pwbFailed;
            // Always written (one per weapon and playback): the defect this guards.
            Ctx().log << "PLAYER_WEAPON_BANK,event=equip_failed,playhead_ms=" << timeMs << ",weapon=0x" << std::hex << wanted
                      << ",held=0x" << current << std::dec << ",requests=" << (g_pwbEquip.attempts - 1)
                      << ",owned=" << (probe.owned ? 1 : 0) << ",on_person=" << (probe.onPerson ? 1 : 0)
                      << ",copy=" << (probe.copyGiven ? 1 : 0) << ",mounted=" << (PED::IS_PED_ON_MOUNT(player) ? 1 : 0)
                      << ",next=" << wb::ActionName(decision.action) << '\n';
        }
        if (decision.action == wb::Action::None)
            return;
        if (!g_pwbTouched)
        {
            // Before the first change: what he held and his whole loadout.
            g_pwbOriginal = current ? current : kPwbUnarmed;
            g_pwbTouched = true;
            g_pwbPlayer = player;
            g_pwbHorse = PoseV2PlayerWeaponBankHorse(player);
            g_pwbBefore = ReadPoseV2PlayerLoadout(player, g_pwbHorse, wb::LoadoutWeapons(g_pwbShots, g_pwbOriginal));
        }
        const Hash replacedBefore = WEAPON::_GET_WEAPON_REPLACED_HASH(FALSE);
        const Hash holsterBefore = g_pwbHorse && ENTITY::DOES_ENTITY_EXIST(g_pwbHorse) ? WEAPON::_GET_WEAPON_FROM_HORSE_HOLSTER(g_pwbHorse) : 0;
        if (decision.action == wb::Action::GiveNew)
        {
            WEAPON::GIVE_WEAPON_TO_PED(player, wanted, 0, TRUE, FALSE, 0, FALSE, 0.5f, 1.0f, kPwbAddReasonDefault, TRUE,
                0.0f, FALSE);
            if (std::find(g_pwbGiven.begin(), g_pwbGiven.end(), wanted) == g_pwbGiven.end())
                g_pwbGiven.push_back(wanted);
        }
        else if (decision.action == wb::Action::GiveCopy)
        {
            // bAllowMultipleCopies: a second weapon beside the stored one.
            WEAPON::GIVE_WEAPON_TO_PED(player, wanted, 0, TRUE, FALSE, 0, TRUE, 0.5f, 1.0f, kPwbAddReasonDefault, TRUE,
                0.0f, FALSE);
            g_pwbCopies.push_back({});
            copy = &g_pwbCopies.back();
            copy->weapon = wanted;
            const Hash holsterAfter = holsterBefore ? WEAPON::_GET_WEAPON_FROM_HORSE_HOLSTER(g_pwbHorse) : 0;
            copy->moved = holsterBefore == wanted && holsterAfter != wanted;
            ReadPoseV2PlayerWeaponCopyGuid(player, *copy);
        }
        WEAPON::SET_CURRENT_PED_WEAPON(player, wanted, TRUE, 0, FALSE, FALSE);
        if (decision.switched)
            ++g_pwbSwitches;
        else
            ++g_pwbRetries;
        const Hash replacedAfter = WEAPON::_GET_WEAPON_REPLACED_HASH(FALSE);
        if (g_pwbLogs < kPwbLogCap || decision.action == wb::Action::GiveCopy)
        {
            ++g_pwbLogs;
            Ctx().log << "PLAYER_WEAPON_BANK,event=equip,playhead_ms=" << timeMs << ",weapon=0x" << std::hex << wanted
                      << ",held=0x" << current << std::dec << ",given=" << (decision.action == wb::Action::GiveNew ? 1 : 0)
                      << ",action=" << wb::ActionName(decision.action) << ",try=" << g_pwbEquip.attempts
                      << ",owned=" << (probe.owned ? 1 : 0) << ",on_person=" << (probe.onPerson ? 1 : 0)
                      << ",mounted=" << (PED::IS_PED_ON_MOUNT(player) ? 1 : 0);
            if (decision.action == wb::Action::GiveCopy)
                Ctx().log << ",guid=" << (copy->guidRead ? 1 : 0) << ",moved=" << (copy->moved ? 1 : 0);
            if (replacedAfter != replacedBefore)
                Ctx().log << ",displaced=0x" << std::hex << replacedAfter << std::dec;
            Ctx().log << '\n';
        }
    }
    }

    void EndPoseV2PlayerWeaponBank(bool restore)
    {
        if (!g_pwbBuilt)
            return;
        std::size_t removed = 0;
        std::size_t copiesRemoved = 0;
        std::size_t copiesReturned = 0;
        std::size_t copiesLeft = 0;
        const Ped player = PLAYER::PLAYER_PED_ID();
        if (restore && g_pwbTouched && player && ENTITY::DOES_ENTITY_EXIST(player))
        {
            static const Hash reason = MISC::GET_HASH_KEY("REMOVE_REASON_DEFAULT");
            // The copies first, each by its GUID: the stored weapon of the same
            // hash stays. One whose GUID was never read is left (removing by
            // hash could take his own) and counted.
            for (PwbCopy& copy : g_pwbCopies)
            {
                ReadPoseV2PlayerWeaponCopyGuid(player, copy);
                if (copy.moved)
                {
                    if (g_pwbHorse && ENTITY::DOES_ENTITY_EXIST(g_pwbHorse))
                    {
                        WEAPON::_ATTACH_WEAPON_TO_HORSE_HOLSTER(g_pwbHorse, copy.weapon, player);
                        ++copiesReturned;
                    }
                    else
                        ++copiesLeft;
                    continue;
                }
                if (copy.guidRead && INVENTORY::_INVENTORY_IS_GUID_VALID(reinterpret_cast<Any*>(copy.guid)))
                {
                    WEAPON::_REMOVE_WEAPON_FROM_PED_BY_GUID(player, reinterpret_cast<Any*>(copy.guid), reason);
                    ++copiesRemoved;
                }
                else
                    ++copiesLeft;
            }
            for (const Hash weapon : g_pwbGiven)
            {
                if (weapon == g_pwbOriginal)
                    continue;
                WEAPON::REMOVE_WEAPON_FROM_PED(player, weapon, TRUE, reason);
                ++removed;
            }
            WEAPON::SET_CURRENT_PED_WEAPON(player, g_pwbOriginal, TRUE, 0, FALSE, FALSE);
            g_pwbVerifyAtMs = GetTickCount64() + kPwbVerifyMs;
        }
        for (const Entity object : g_pwbHiddenObjects)
            if (ENTITY::DOES_ENTITY_EXIST(object))
                ENTITY::SET_ENTITY_VISIBLE(object, TRUE);
        Ctx().log << "PLAYER_WEAPON_BANK,event=end,player_shots=" << g_pwbShots.size() << ",switches=" << g_pwbSwitches
                  << ",retries=" << g_pwbRetries << ",given=" << g_pwbGiven.size() << ",removed=" << removed
                  << ",copies=" << g_pwbCopies.size() << ",copies_removed=" << copiesRemoved
                  << ",copies_returned=" << copiesReturned << ",copies_left=" << copiesLeft
                  << ",equip_failed=" << g_pwbFailed
                  << ",restored=0x" << std::hex << (g_pwbTouched ? g_pwbOriginal : 0) << std::dec
                  << ",restore=" << (restore ? 1 : 0) << ",objects_hidden=" << g_pwbHiddenObjects.size()
                  << ",seen_visible=" << g_pwbObjectsSeenVisible << '\n';
        BeginPoseV2PlayerWeaponBank();
    }

    // The loadout check, kPwbVerifyMs after the restore (Monitor, every tick):
    // his loadout against the one read before the first change. The held
    // weapon is asked once more when it is not back yet. Skipped when the
    // player ped changed (the closed theater's save reload restores it).
    void VerifyPoseV2PlayerWeaponBank()
    {
        if (!g_pwbVerifyAtMs || GetTickCount64() < g_pwbVerifyAtMs || Ctx().transport.Playing())
            return;
        g_pwbVerifyAtMs = 0;
        const Ped player = PLAYER::PLAYER_PED_ID();
        if (!player || player != g_pwbPlayer || !ENTITY::DOES_ENTITY_EXIST(player))
        {
            Ctx().log << "PLAYER_WEAPON_BANK,event=inventory,match=-1,skipped=player_changed\n";
            return;
        }
        std::vector<std::uint32_t> weapons;
        for (const replay::weaponbank::WeaponFacts& facts : g_pwbBefore.weapons)
            weapons.push_back(facts.weapon);
        const replay::weaponbank::Loadout after = ReadPoseV2PlayerLoadout(player, g_pwbHorse, weapons);
        const std::string diff = replay::weaponbank::LoadoutDiff(g_pwbBefore, after);
        const bool heldAgain = after.held != g_pwbBefore.held;
        if (heldAgain)
            WEAPON::SET_CURRENT_PED_WEAPON(player, g_pwbBefore.held, TRUE, 0, FALSE, FALSE);
        // Ammo of a weapon he still owns put back as it was (a weapon taken
        // back can share its ammo type with one of his).
        std::uint32_t ammoRestored = 0;
        for (const replay::weaponbank::WeaponFacts& was : g_pwbBefore.weapons)
        {
            for (const replay::weaponbank::WeaponFacts& now : after.weapons)
            {
                if (now.weapon != was.weapon || !was.owned || !now.owned || now.ammo == was.ammo)
                    continue;
                WEAPON::SET_PED_AMMO(player, was.weapon, was.ammo);
                ++ammoRestored;
            }
        }
        // match: the loadout as read now (before the two fixes above, which
        // the next playback's check or the bench's weapon_state confirms).
        Ctx().log << "PLAYER_WEAPON_BANK,event=inventory,match=" << (diff.empty() ? 1 : 0)
                  << ",held=0x" << std::hex << after.held << ",horse_holster=0x" << after.horseHolster << std::dec
                  << ",weapons=" << weapons.size() << ",held_asked_again=" << (heldAgain ? 1 : 0)
                  << ",ammo_restored=" << ammoRestored << ",diff=" << (diff.empty() ? "none" : diff.c_str()) << '\n';
    }

    // The player's horse (mount_audio.h). Live, the ped audio of the horse the
    // local player rides takes the player variant (movement sounds, hooves, the
    // player horse branches) through the mount audio predicate; a puppet horse
    // carries a puppet rider and would take the NPC one. While a clip plays, the
    // horse puppet the player's puppet sits on (the rider and mount pairs of the
    // jerk detector, replay::playback::MountOf) answers yes at the ped audio
    // sites. The player's puppet: the shooters recorded as the local player, else
    // the story player models.
    replay::audio::MountAudioStats g_poseV2MountAudioBase;
    namespace
    {
    std::vector<std::uint64_t> g_poseV2MountAudioComponents;
    std::uint32_t g_poseV2MountAudioLogs = 0;
    }
    bool g_poseV2MountProbeLogged = false;
    namespace
    {
    constexpr std::uint32_t kPoseV2MountAudioLogCap = 20;
    }

    void PoseV2PlayerRiderIds(std::vector<std::uint32_t>& riders)
    {
        riders.clear();
        for (const replay::entity::ReplayId& id : g_poseV2PlayerAudioIds)
            riders.push_back(id.Packed());
        if (!riders.empty())
            return;
        static const Hash arthur = MISC::GET_HASH_KEY("player_zero");
        static const Hash john = MISC::GET_HASH_KEY("player_three");
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
            if (puppet.packet.kind == replay::entity::EntityKind::Ped &&
                (puppet.packet.modelHash == arthur || puppet.packet.modelHash == john))
                riders.push_back(puppet.packet.id.Packed());
    }

    void BeginPoseV2PlayerMountAudio()
    {
        g_poseV2MountAudioComponents.clear();
        replay::audio::SetPlayerMountAudioComponents({});
        g_poseV2MountAudioBase = replay::audio::MountAudioSnapshot();
        g_poseV2MountAudioLogs = 0;
    }
    namespace
    {

    // Every frame of playback (the horse puppet is recreated by a scrub).
    void UpdatePoseV2PlayerMountAudio(std::uint32_t timeMs)
    {
        static std::vector<std::uint32_t> riders;
        static std::vector<std::uint64_t> components;
        static std::string ids;
        PoseV2PlayerRiderIds(riders);
        const std::vector<std::uint32_t> mounts =
            replay::mountaudio::PlayerMounts(riders, [](std::uint32_t rider) { return replay::playback::MountOf(rider); });
        components.clear();
        ids.clear();
        for (const std::uint32_t mount : mounts)
        {
            const replay::playback::Puppet* puppet = Ctx().puppets.Find(replay::entity::ReplayId::FromPacked(mount));
            if (!puppet || puppet->state != replay::playback::PuppetState::Ready || !puppet->clone ||
                !replay::entities::Exists(puppet->clone) || !replay::entities::IsPed(puppet->clone))
                continue;
            const std::uint64_t component = replay::audio::MountLinkComponent(replay::entities::AddressOf(puppet->clone));
            if (!component)
                continue;
            components.push_back(component);
            ids += (ids.empty() ? "" : "|") + replay::entity::IdText(puppet->packet.id);
        }
        if (components == g_poseV2MountAudioComponents)
            return;
        g_poseV2MountAudioComponents = components;
        replay::audio::SetPlayerMountAudioComponents(components);
        if (g_poseV2MountAudioLogs >= kPoseV2MountAudioLogCap)
            return;
        ++g_poseV2MountAudioLogs;
        std::string riderText;
        for (const std::uint32_t rider : riders)
            riderText += (riderText.empty() ? "" : "|") + replay::entity::IdText(replay::entity::ReplayId::FromPacked(rider));
        Ctx().log << "POSE_V2_PLAYER_MOUNT_AUDIO,phase=play,event=set,riders=" << (riderText.empty() ? "none" : riderText.c_str())
            << ",mounts=" << (ids.empty() ? "none" : ids.c_str()) << ",components=" << components.size()
            << ",hook=" << (replay::audio::MountAudioSnapshot().installed ? 1 : 0) << ",playhead_ms=" << timeMs << '\n';
    }
    }

    void EndPoseV2PlayerMountAudio()
    {
        replay::audio::SetPlayerMountAudioComponents({});
        g_poseV2MountAudioComponents.clear();
    }

    // Recording, once: the predicate's own answer for the player's mount
    // (link=1 proves the rider link of the slot +0xA8 component live).
    void ProbePoseV2PlayerMountAudio(std::uint32_t nowMs)
    {
        if (g_poseV2MountProbeLogged)
            return;
        const Ped player = PLAYER::PLAYER_PED_ID();
        if (!player || !PED::IS_PED_ON_MOUNT(player))
            return;
        const Ped horse = PED::GET_MOUNT(player);
        if (!horse || !replay::entities::Exists(horse))
            return;
        g_poseV2MountProbeLogged = true;
        std::uint64_t linked = 0;
        const int answer = replay::audio::MountLinkIsPlayer(replay::entities::AddressOf(horse), linked);
        const std::uint64_t playerAddress = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(player));
        Ctx().log << "POSE_V2_PLAYER_MOUNT_AUDIO,phase=record,event=probe,link_is_player=" << answer
            << ",linked_is_player_ped=" << (linked && linked == playerAddress ? 1 : 0)
            << ",component=0x" << std::hex << replay::audio::MountLinkComponent(replay::entities::AddressOf(horse)) << std::dec
            << ",time_ms=" << nowMs << '\n';
    }

    void LogPoseV2PlayerMountAudioTotal(const char* phase)
    {
        const replay::audio::MountAudioStats now = replay::audio::MountAudioSnapshot();
        Ctx().log << "POSE_V2_PLAYER_MOUNT_AUDIO,phase=" << phase << ",hook=" << (now.installed ? 1 : 0)
            << ",calls=" << (now.calls - g_poseV2MountAudioBase.calls)
            << ",engine_true=" << (now.engineTrue - g_poseV2MountAudioBase.engineTrue)
            << ",overrides=" << (now.overrides - g_poseV2MountAudioBase.overrides)
            << ",sites=" << replay::audio::MountAudioSites(now, g_poseV2MountAudioBase) << '\n';
    }
    namespace
    {

    std::string PlayPoseV2ShotFireAudio(const replay::entity::ShotEvent& shot, const replay::playback::Puppet& shooterPuppet,
        const replay::playback::Puppet* weapon)
    {
        const replay::playback::Puppet* shooter = &shooterPuppet;
        std::string fireStatus = "none";
        std::array<std::uint8_t, 0xA0> info = shot.fireInfo;
        BYTE* puppetAddress = replay::entities::AddressOf(shooter->clone);
        const std::uint64_t puppet = reinterpret_cast<std::uint64_t>(puppetAddress);
        const std::uint64_t puppetAudio = replay::audio::PedAudio(puppetAddress);
        const std::uint64_t puppetWeapon = weapon && weapon->clone
            ? reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(weapon->clone))
            : 0;
        std::string unmapped;
        for (const replay::entity::ShotEvent::FireRefField& ref : shot.fireRefs)
        {
            std::uint64_t value = 0;
            if (ref.target.Valid())
            {
                const replay::playback::Puppet* target = Ctx().puppets.Find(ref.target);
                if (target && target->state == replay::playback::PuppetState::Ready && target->clone)
                    value = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(target->clone));
            }
            else if (ref.via.Valid())
            {
                // The same field of the holder's puppet.
                const replay::playback::Puppet* holder = Ctx().puppets.Find(ref.via);
                if (holder && holder->state == replay::playback::PuppetState::Ready && holder->clone)
                {
                    replay::audio::ReadU64(
                        reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(holder->clone)) + ref.viaOffset,
                        value);
                }
                if (!value)
                    value = PoseV2WeaponHeldBySameWeapon(shot, ref.via, ref.viaOffset);
            }
            else switch (ref.role)
            {
            case replay::entity::ShotEvent::FireRef::Shooter:
                value = puppet;
                break;
            case replay::entity::ShotEvent::FireRef::ShooterAudio:
                value = puppetAudio;
                break;
            case replay::entity::ShotEvent::FireRef::Weapon:
                value = puppetWeapon;
                break;
            default:
                break;
            }
            if (!value)
            {
                char text[16]{};
                std::snprintf(text, sizeof(text), "%s%X", unmapped.empty() ? "" : "|",
                    static_cast<unsigned>(ref.offset));
                unmapped += text;
                continue;
            }
            std::memcpy(info.data() + ref.offset, &value, 8);
        }
        if (!unmapped.empty())
        {
            fireStatus = "unmapped:" + unmapped;
        }
        else
        {
            std::string error;
            // The player's shot plays the settings its weapon's audio cached at
            // set-up, and the puppet's weapon was set up as an NPC's: its cache gets the
            // player variant of the same settings, once per weapon. +0x00 of the event is
            // the weapon object (mapped above).
            std::uint64_t cweapon = 0;
            std::memcpy(&cweapon, info.data(), 8);
            if (cweapon && PoseV2PlayerAudioPuppet(*shooter) &&
                std::find(g_poseV2PlayerAudioWeapons.begin(), g_poseV2PlayerAudioWeapons.end(), cweapon) ==
                    g_poseV2PlayerAudioWeapons.end())
            {
                g_poseV2PlayerAudioWeapons.push_back(cweapon);
                std::uint64_t before = 0;
                std::uint64_t after = 0;
                std::string refreshError;
                const bool refreshed = replay::audio::RefreshWeaponAudioSettings(cweapon, before, after, refreshError);
                Ctx().log << "POSE_V2_PLAYER_AUDIO,phase=weapon_settings,shooter=" << replay::entity::IdText(shot.shooter)
                    << ",ok=" << (refreshed ? 1 : 0) << ",changed=" << (before != after ? 1 : 0)
                    << ",before=0x" << std::hex << before << ",after=0x" << after << std::dec
                    << ",error=" << refreshError << ",event_ms=" << shot.timeMs << '\n';
            }
            const std::uint64_t fireUs = replay::clock::NowUs();
            fireStatus = replay::audio::EnqueueFire(info.data(), error) ? "ok" : error;
            if (fireStatus == "ok")
                NoteShotAudioFire(shot.shooter, info.data(), fireUs, shot.timeMs, shot.muzzle,
                    shot.shooterPlayer == 2 || PoseV2PlayerAudioPuppet(*shooter));
        }
        return fireStatus;
    }
    }

    void PlayPoseV2Shots(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.shots");
        UpdatePoseV2PlayerAudio(timeMs);
        UpdatePoseV2PlayerWeaponBank(timeMs);
        UpdatePoseV2PlayerMountAudio(timeMs);
        TickEntityQuery(false, timeMs);
        TickPedAudioState(false, timeMs);
        TickMotionState(false, timeMs);
        TickPoseV2HorseAudioEvidence(false, timeMs);
        // Fire audio waiting for its puppet (normal play only).
        if (!Ctx().hooks->NormalPlay())
            g_poseV2ShotFirePending.clear();
        for (std::size_t pending = g_poseV2ShotFirePending.size(); pending-- > 0;)
        {
            const PoseV2ShotFirePending entry = g_poseV2ShotFirePending[pending];
            const replay::entity::ShotEvent& shot = Ctx().clip.shots.Shots()[entry.shot];
            const replay::playback::Puppet* shooter = Ctx().puppets.Find(shot.shooter);
            std::string status = "shooter_gone";
            if (shooter && shooter->state == replay::playback::PuppetState::Ready && shooter->clone &&
                replay::entities::Exists(shooter->clone))
            {
                const replay::playback::Puppet* weapon = shot.weapon.Valid() ? Ctx().puppets.Find(shot.weapon) : nullptr;
                status = PlayPoseV2ShotFireAudio(shot, *shooter, weapon);
            }
            if (status.rfind("unmapped", 0) == 0 && timeMs < entry.untilMs)
                continue;
            Ctx().log << "POSE_V2_PLAYBACK_SHOT_FIRE_RETRY,shooter=" << replay::entity::IdText(shot.shooter)
                << ",status=" << status << ",event_ms=" << shot.timeMs << ",playhead_ms=" << timeMs << '\n';
            g_poseV2ShotFirePending.erase(g_poseV2ShotFirePending.begin() + static_cast<std::ptrdiff_t>(pending));
        }
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
        const bool normalPlay = Ctx().hooks->NormalPlay();
        if (!g_poseV2ShotCursor.Advance(timeMs, normalPlay, fromMs, toMs))
            return;
        static std::vector<std::size_t> due;
        Ctx().clip.shots.Range(fromMs, toMs, due);
        for (const std::size_t index : due)
        {
            const replay::entity::ShotEvent& shot = Ctx().clip.shots.Shots()[index];
            const replay::playback::Puppet* shooter = Ctx().puppets.Find(shot.shooter);
            const bool present = shooter && shooter->state == replay::playback::PuppetState::Ready &&
                shooter->clone && replay::entities::Exists(shooter->clone) &&
                ENTITY::IS_ENTITY_VISIBLE(shooter->clone);
            const replay::playback::Puppet* weapon = shot.weapon.Valid() ? Ctx().puppets.Find(shot.weapon) : nullptr;
            Vector3 from{};
            from.x = shot.muzzle.x;
            from.y = shot.muzzle.y;
            from.z = shot.muzzle.z;
            bool puppetMuzzle = false;
            if (present && weapon && weapon->state == replay::playback::PuppetState::Ready && weapon->clone)
                puppetMuzzle = replay::pose::playback::WeaponMuzzle(weapon->clone, from);
            Vector3 to{};
            if (shot.hasImpact)
            {
                to.x = shot.impact.x;
                to.y = shot.impact.y;
                to.z = shot.impact.z;
            }
            else
            {
                to.x = from.x + shot.direction.x * kPoseV2ShotRangeM;
                to.y = from.y + shot.direction.y * kPoseV2ShotRangeM;
                to.z = from.z + shot.direction.z * kPoseV2ShotRangeM;
            }
            // The weapon's own effects: muzzle flash attached to the weapon puppet's
            // muzzle bone the way the game's weapon vfx attaches it, impact in the world
            // with the surface normal on Z. No bullet.
            std::string muzzleError = "none";
            std::string impactError = "none";
            bool muzzlePlayed = false;
            bool impactPlayed = false;
            // A clip with the weapon effects as events plays them from the fx track;
            // an older clip plays the shot's, a hash several shots took once
            // (shot_fx_audit_core.h, the legacy rule).
            const replay::shotfx::Decision decision =
                index < g_fxShotDecisions.size() ? g_fxShotDecisions[index] : replay::shotfx::Decision{};
            const bool playMuzzle = !g_fxClipWeaponEvents && decision.playMuzzle;
            const bool playImpact = !g_fxClipWeaponEvents && decision.playImpact;
            if (g_fxClipWeaponEvents)
                muzzleError = impactError = "events";
            else
            {
                if (!decision.playMuzzle)
                    muzzleError = "legacy_shared";
                if (!decision.playImpact)
                    impactError = decision.onBody ? "legacy_on_body" : "legacy_shared";
            }
            if (present && g_poseV2FxPlayerReady)
            {
                if (playMuzzle && shot.muzzleFx && weapon && weapon->state == replay::playback::PuppetState::Ready && weapon->clone)
                {
                    const int bone = ENTITY::GET_ENTITY_BONE_INDEX_BY_NAME(weapon->clone, "Gun_Muzzle");
                    BYTE* entity = replay::entities::AddressOf(weapon->clone);
                    muzzleError.clear();
                    if (entity && bone >= 0)
                        muzzlePlayed = replay::fx::PlayAttachedWeaponEffect(
                            shot.muzzleFx, entity, bone, 1.0f,
                            shot.hasMuzzleFlags ? shot.muzzleFlags : replay::fx::kMuzzleDefaultFlags,
                            muzzleError);
                    else
                        muzzleError = "no_muzzle_bone";
                }
                if (playImpact && shot.impactFx && shot.hasImpact)
                {
                    replay::pose::Vec3 z = shot.hasNormal
                        ? shot.impactNormal
                        : replay::pose::Vec3{-shot.direction.x, -shot.direction.y, -shot.direction.z};
                    const float zLength = std::sqrt(z.x * z.x + z.y * z.y + z.z * z.z);
                    if (zLength > 0.0001f)
                        z = {z.x / zLength, z.y / zLength, z.z / zLength};
                    // Any X perpendicular to the normal, Y = Z x X.
                    replay::pose::Vec3 helper = std::fabs(z.z) < 0.9f
                        ? replay::pose::Vec3{0.0f, 0.0f, 1.0f}
                        : replay::pose::Vec3{1.0f, 0.0f, 0.0f};
                    replay::pose::Vec3 x{
                        helper.y * z.z - helper.z * z.y,
                        helper.z * z.x - helper.x * z.z,
                        helper.x * z.y - helper.y * z.x};
                    const float xLength = std::sqrt(x.x * x.x + x.y * x.y + x.z * x.z);
                    x = {x.x / xLength, x.y / xLength, x.z / xLength};
                    const replay::pose::Vec3 y{
                        z.y * x.z - z.z * x.y,
                        z.z * x.x - z.x * x.z,
                        z.x * x.y - z.y * x.x};
                    const float matrix[16] = {
                        x.x, x.y, x.z, 0.0f,
                        y.x, y.y, y.z, 0.0f,
                        z.x, z.y, z.z, 0.0f,
                        shot.impact.x, shot.impact.y, shot.impact.z, 0.0f};
                    impactError.clear();
                    impactPlayed = replay::fx::PlayEffect(shot.impactFx, nullptr, -1, matrix, 1.0f, impactError);
                    // A surface impact made for a shot that hit a body (FX_IMPACT_ON_BODY).
                    if (impactPlayed && decision.onBody)
                    {
                        g_fxCheck.NoteImpactOnBody();
                        if (g_fxImpactOnBodyLogs < 16)
                        {
                            ++g_fxImpactOnBodyLogs;
                            Ctx().log << "FX_IMPACT_ON_BODY,shooter=" << replay::entity::IdText(shot.shooter) << ",impact_fx=0x"
                                << std::hex << shot.impactFx << std::dec << ",event_ms=" << shot.timeMs << '\n';
                        }
                    }
                }
                if (muzzlePlayed)
                    g_fxCheck.NoteWeapon(replay::fx::MuzzleFlashSite(), 2);
                if (impactPlayed)
                    g_fxCheck.NoteWeapon(replay::fx::BulletImpactSite(), 2);
            }
            // Fire sound: the recorded engine fire event, its entity
            // references pointed at the puppets, handed to the same enqueue
            // the weapon code uses. Refused when a reference has no puppet
            // counterpart.
            std::string fireStatus = "none";
            if (present && shot.hasFireInfo)
            {
                fireStatus = PlayPoseV2ShotFireAudio(shot, *shooter, weapon);
                // A thrown weapon's reference lives in the projectile, whose puppet may
                // come a few frames after the throw: tried again for up to
                // kPoseV2ShotFireWaitMs.
                if (fireStatus.rfind("unmapped", 0) == 0)
                    g_poseV2ShotFirePending.push_back({index, timeMs + kPoseV2ShotFireWaitMs});
            }
            // The cancelled count belongs to the played test, not to the snapshot's
            // if (that would count every shot after the first muzzle as cancelled).
            if (present && shot.weaponHash)
                ++g_poseV2ShotsPlayed;
            else
                ++g_poseV2ShotsCancelled;
            static bool replaySnapshotLogged = false;
            if (muzzlePlayed && !replaySnapshotLogged)
            {
                std::vector<std::uint8_t> snapshot;
                if (replay::fx::TakeReplaySnapshot(snapshot))
                {
                    LogPoseV2FxSnapshot("replay", snapshot);
                    replaySnapshotLogged = true;
                }
            }
            std::ostream& log = Ctx().log;
            log << "POSE_V2_PLAYBACK_SHOT"
                << ",shooter=" << replay::entity::IdText(shot.shooter)
                << ",weapon=0x" << std::hex << shot.weaponHash << std::dec
                << ",status=" << (present && shot.weaponHash ? "played" : "cancelled")
                << ",muzzle_fx=0x" << std::hex << shot.muzzleFx << std::dec
                << ",muzzle=" << (muzzlePlayed ? "ok" : muzzleError.c_str())
                << ",impact_fx=0x" << std::hex << shot.impactFx << std::dec
                << ",impact_play=" << (impactPlayed ? "ok" : impactError.c_str())
                << ",fire_audio=" << fireStatus
                << ",shooter_player=" << static_cast<int>(shot.shooterPlayer)
                << ",player_audio=" << (present && PoseV2PlayerAudioPuppet(*shooter) ? 1 : 0)
                << ",puppet_muzzle=" << (puppetMuzzle ? 1 : 0)
                << ",impact=" << (shot.hasImpact ? 1 : 0)
                << ",event_ms=" << shot.timeMs
                << ",playhead_ms=" << timeMs
                << '\n';
            if (present)
                LogShotGunfight("play", shot.shooter, replay::entities::AddressOf(shooter->clone), shot.timeMs);
            log.flush();
        }
    }

    std::uint64_t g_poseV2SpeechMatches = 0;
    std::uint64_t g_poseV2SpeechMismatches = 0;
    std::uint64_t g_poseV2SpeechRefused = 0;
    std::uint64_t g_poseV2VoicePinned = 0;
    std::uint64_t g_poseV2VoiceFailed = 0;
    std::uint64_t g_poseV2VoiceAnimal = 0;
    std::uint64_t g_poseV2SpeechStopsPlayed = 0;
    std::uint64_t g_poseV2SpeechPlayStartCursor = 0;
    // Load time the replay requests a line ahead of its recorded start
    // (median request-to-start of the recorded AI lines).
    std::uint32_t g_poseV2SpeechLoadMs = 0;
    namespace
    {

    // ---- The recorded weapon fx requests played back. Per WFXI request in
    // normal forward play, the same wrapper the weapon system called
    // (fx::CallWeaponFxRequest) with the puppets' arguments; the engine makes
    // or culls the effect against the camera that renders now. The recorded
    // muzzle effects and shell tracks the requests replace are dropped at the
    // playback's start (g_wfxDropEvent, g_wfxDropTrack). Every call is measured
    // for sound: census rows of the shooter and its weapon made during the
    // call, the puppet refusals during it, and per shot the weapon's rows in
    // the shot's two census windows against the recording's.
        // The census clock (sound_census.cpp Push: counter * 10^6 / frequency).
        std::uint64_t WfxCensusUs()
        {
            static LARGE_INTEGER frequency{};
            if (!frequency.QuadPart)
                QueryPerformanceFrequency(&frequency);
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            return frequency.QuadPart
                ? static_cast<std::uint64_t>(now.QuadPart) * 1000000ull / static_cast<std::uint64_t>(frequency.QuadPart)
                : 0;
        }
    }

        // The muzzle info map's entries, copied once per playback (static
        // weapon metadata; weapon_fx_intent_core.h kWeaponMuzzleInfoTable).
        std::vector<std::uint8_t> g_wfxInfoEntries;
        bool g_wfxInfoTried = false;
        const char* g_wfxInfoError = "none";
    namespace
    {

        bool WfxLoadInfoEntries()
        {
            if (g_wfxInfoTried)
                return !g_wfxInfoEntries.empty();
            g_wfxInfoTried = true;
            namespace wf = replay::weaponfx;
            const std::uintptr_t table = replay::fx::MuzzleInfoTable();
            if (!table)
                return g_wfxInfoError = "no_info_table", false;
            const std::uintptr_t map = table + wf::kInfoMapOffset;
            std::uint64_t entries = 0;
            std::uint16_t count = 0;
            if (!replay::mem::Read(map + wf::kInfoMapEntries, entries) || !replay::mem::Read(map + wf::kInfoMapCount, count))
                return g_wfxInfoError = "map_unread", false;
            if (!count || count > wf::kMaxInfoEntries || !wf::Plausible(entries))
                return g_wfxInfoError = "map_implausible", false;
            g_wfxInfoEntries.resize(static_cast<std::size_t>(count) * wf::kInfoEntryBytes);
            if (!replay::mem::Copy(g_wfxInfoEntries.data(), static_cast<std::uintptr_t>(entries), g_wfxInfoEntries.size()))
            {
                g_wfxInfoEntries.clear();
                return g_wfxInfoError = "entries_unread", false;
            }
            return true;
        }

        std::uint64_t WfxEntryValue(std::size_t n)
        {
            std::uint64_t value = 0;
            std::memcpy(&value, g_wfxInfoEntries.data() + n * replay::weaponfx::kInfoEntryBytes + replay::weaponfx::kInfoEntryValue,
                sizeof(value));
            return value;
        }

        bool WfxInfoMatches(std::uint64_t info, const replay::weaponfx::Intent& intent)
        {
            std::uint8_t head[12] = {};
            return replay::weaponfx::Plausible(info) && replay::mem::Copy(head, static_cast<std::uintptr_t>(info), sizeof(head)) &&
                replay::weaponfx::InfoMatches(head, intent);
        }

        // The muzzle info for a call: first as the engine's caller asks it
        // (the puppet weapon's key, the component's when it has one), then
        // any entry of the map with the recorded hashes and range. 0: none.
        std::uint64_t WfxMuzzleInfo(std::uint64_t weapon, std::uint64_t weaponInfo, const replay::weaponfx::Intent& intent,
            const char*& source)
        {
            namespace wf = replay::weaponfx;
            source = "none";
            if (!WfxLoadInfoEntries())
                return source = g_wfxInfoError, 0;
            std::uint32_t key = 0;
            replay::mem::Read(static_cast<std::uintptr_t>(weaponInfo + wf::kWeaponInfoMuzzleKey), key);
            std::uint64_t component = 0;
            if (replay::mem::Read(static_cast<std::uintptr_t>(weapon + wf::kWeaponComponentOffset), component) &&
                wf::Plausible(component))
            {
                std::uint64_t componentInfo = 0;
                std::uint32_t componentKey = 0;
                if (replay::mem::Read(static_cast<std::uintptr_t>(component + wf::kComponentInfoOffset), componentInfo) &&
                    wf::Plausible(componentInfo) &&
                    replay::mem::Read(static_cast<std::uintptr_t>(componentInfo + wf::kComponentMuzzleKey), componentKey) &&
                    componentKey)
                    key = componentKey;
            }
            const std::size_t count = g_wfxInfoEntries.size() / wf::kInfoEntryBytes;
            if (key)
                for (std::size_t n = 0; n < count; ++n)
                {
                    std::uint32_t entryKey = 0;
                    std::memcpy(&entryKey, g_wfxInfoEntries.data() + n * wf::kInfoEntryBytes, sizeof(entryKey));
                    if (entryKey != key)
                        continue;
                    const std::uint64_t info = WfxEntryValue(n);
                    if (WfxInfoMatches(info, intent))
                        return source = "weapon_key", info;
                    break;
                }
            for (std::size_t n = 0; n < count; ++n)
            {
                const std::uint64_t info = WfxEntryValue(n);
                if (WfxInfoMatches(info, intent))
                    return source = "recorded_hashes", info;
            }
            return source = "no_match", 0;
        }

        // The engine's planes test for a point (the viewport the cores read);
        // -1 when the viewport is not readable.
        int WfxEngineInView(const float point[3])
        {
            namespace wf = replay::weaponfx;
            const std::uintptr_t global = replay::fx::Viewport();
            std::uint64_t viewport = 0;
            if (!global || !replay::mem::Read(global, viewport) || !wf::Plausible(viewport))
                return -1;
            float planes[8][4] = {};
            if (!replay::mem::Copy(planes, static_cast<std::uintptr_t>(viewport + wf::kViewportPlanes), sizeof(planes)))
                return -1;
            for (const auto& row : planes)
                for (const float v : row)
                    if (!std::isfinite(v))
                        return -1;
            return wf::EngineInView(planes, point) ? 1 : 0;
        }
    }
        std::vector<WfxShotWindow> g_wfxWindows;
        replay::entity::ForwardEventCursor g_wfxCursor;
        replay::weaponfx::PlayTotals g_wfxTotals;
        std::vector<std::uint8_t> g_wfxShotExtra;   // per g_fxShotRows: 1 extra sound, 2 refused, 3 both
        std::vector<std::uint8_t> g_wfxShotCalled;
        std::uint32_t g_wfxPlayLogs = 0;
    namespace
    {
        constexpr std::uint32_t kWfxPlayLogCap = 96;

        std::int32_t WfxRecordedRows(std::uint32_t weapon, std::uint32_t window)
        {
            const replay::census::Summary& rec = Ctx().clip.soundCensus;
            if (rec.keys.empty())
                return -1;
            std::int32_t total = 0;
            for (const replay::census::SummaryCell& cell : rec.cells)
            {
                if (cell.window < window || cell.window > window + 1 || cell.key >= rec.keys.size())
                    continue;
                if (rec.keys[cell.key].owner == weapon)
                    total += cell.count;
            }
            return total;
        }

        void WfxCloseWindow(WfxShotWindow& window, bool measured)
        {
            namespace wf = replay::weaponfx;
            const std::int32_t expected = measured && !window.partial ? WfxRecordedRows(window.weapon, window.window) : -1;
            const std::int32_t after = measured && !window.partial ? window.after : -1;
            bool extra = false;
            bool refused = false;
            for (WfxCall& call : window.calls)
            {
                wf::SoundWindow sound;
                sound.measured = measured;
                sound.inCall = call.inCall < 0 ? 0 : call.inCall;
                sound.after = after;
                sound.expected = expected;
                sound.refusedInCall = call.refused + call.blocked;
                const bool callExtra = wf::ExtraSound(sound);
                extra = extra || callExtra;
                refused = refused || sound.refusedInCall > 0;
                g_wfxTotals.soundsInCall += call.inCall > 0 ? static_cast<std::uint32_t>(call.inCall) : 0;
                g_wfxTotals.refusedInCall += static_cast<std::uint32_t>(call.refused + call.blocked);
                if (g_wfxPlayLogs < kWfxPlayLogCap)
                {
                    ++g_wfxPlayLogs;
                    Ctx().log << "WEAPON_FX_INTENT_PLAY,kind=" << (call.kind == 1 ? "muzzle" : "shell") << ",shot=" << call.shot
                        << ",t=" << call.intentMs << ",playhead_ms=" << call.playheadMs
                        << ",ped=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(call.shooter))
                        << ",weapon=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(call.weapon))
                        << ",result=" << call.result << ",created_fx=" << call.createdFx << ",starts=" << call.starts
                        << ",shell_gets=" << call.shellGets << ",sounds_after=" << after << ",sounds_expected=" << expected
                        << ",sounds_in_call=" << call.inCall << ",refused_in_call=" << call.refused
                        << ",blocked_in_call=" << call.blocked << ",extra_sound=" << (callExtra ? 1 : 0)
                        << ",in_view=" << call.inView << ",cam_view=" << call.camView << ",in_range=" << call.inRange
                        << ",dist_m=" << call.distM << ",info=" << call.infoSource << ",window=" << window.window
                        << ",partial=" << (window.partial ? 1 : 0) << std::hex << ",code=0x" << call.code << ",rip=0x" << call.rip
                        << std::dec << '\n';
                }
            }
            if (measured && !window.partial)
            {
                g_wfxTotals.soundsAfter += static_cast<std::uint32_t>(after > 0 ? after : 0);
                g_wfxTotals.soundsExpected += static_cast<std::uint32_t>(expected > 0 ? expected : 0);
            }
            else
                ++g_wfxTotals.windowsUnmeasured;
            // Per shot once (a request without a shot counts as one).
            bool counted = false;
            for (const WfxCall& call : window.calls)
            {
                if (call.shot < 0 || static_cast<std::size_t>(call.shot) >= g_wfxShotExtra.size())
                {
                    if (!counted)
                    {
                        g_wfxTotals.shotsWithExtraSound += extra ? 1 : 0;
                        g_wfxTotals.shotsWithRefusedSound += refused ? 1 : 0;
                        counted = true;
                    }
                    continue;
                }
                std::uint8_t& mark = g_wfxShotExtra[static_cast<std::size_t>(call.shot)];
                if (extra && !(mark & 1))
                {
                    mark |= 1;
                    ++g_wfxTotals.shotsWithExtraSound;
                }
                if (refused && !(mark & 2))
                {
                    mark |= 2;
                    ++g_wfxTotals.shotsWithRefusedSound;
                }
            }
        }
    }

        void WfxFlushWindows(bool partial)
        {
            const bool measured = replay::census::Enabled();
            for (WfxShotWindow& window : g_wfxWindows)
            {
                window.partial = window.partial || partial;
                WfxCloseWindow(window, measured);
            }
            g_wfxWindows.clear();
        }
    namespace
    {

        // This tick's census rows (drained at the start of Play) into the
        // open windows; then the windows whose two census windows the
        // playhead has passed are closed.
        void WfxTallyRows(std::uint32_t timeMs, bool normal)
        {
            if (g_wfxWindows.empty())
                return;
            const bool measured = replay::census::Enabled();
            for (const replay::census::TickRow& tick : replay::census::Rows())
            {
                if (!tick.row.sound || tick.owner.kind != replay::census::OwnerKind::Puppet)
                    continue;
                for (WfxShotWindow& window : g_wfxWindows)
                {
                    const bool ofWeapon = tick.owner.id == window.weapon;
                    const bool ofShooter = window.shooter != replay::weaponfx::kNoId && tick.owner.id == window.shooter;
                    if (!ofWeapon && !ofShooter)
                        continue;
                    for (WfxCall& call : window.calls)
                        if (!call.drained && tick.row.timeUs >= call.beganUs && tick.row.timeUs <= call.endedUs)
                            ++call.inCall;
                    const double from = static_cast<double>(window.window) * replay::census::kWindowMs;
                    if (ofWeapon && tick.clipMs >= from && tick.clipMs < from + 2.0 * replay::census::kWindowMs)
                        ++window.after;
                }
            }
            for (WfxShotWindow& window : g_wfxWindows)
                for (WfxCall& call : window.calls)
                    if (!call.drained && call.beganUs)
                    {
                        call.drained = true;
                        if (call.inCall < 0)
                            call.inCall = 0;
                    }
            for (std::size_t n = 0; n < g_wfxWindows.size();)
            {
                WfxShotWindow& window = g_wfxWindows[n];
                const std::uint32_t endMs = (window.window + 2) * replay::census::kWindowMs + 150;
                if (!normal)
                    window.partial = true;
                if (!normal || timeMs >= endMs)
                {
                    WfxCloseWindow(window, measured);
                    g_wfxWindows.erase(g_wfxWindows.begin() + static_cast<std::ptrdiff_t>(n));
                    continue;
                }
                ++n;
            }
        }

        void WfxPlayOne(const replay::weaponfx::Intent& intent, std::uint32_t timeMs)
        {
            namespace wf = replay::weaponfx;
            WfxCall call;
            call.kind = intent.kind;
            call.intentMs = intent.timeMs;
            call.playheadMs = timeMs;
            call.shooter = intent.shooter;
            call.weapon = intent.weapon;
            call.shot = wf::ShotIndexFor(g_fxShotRows, intent);
            ++(intent.kind == static_cast<std::uint8_t>(wf::Kind::Muzzle) ? g_wfxTotals.muzzle : g_wfxTotals.shell);
            wf::CallInput input;
            Entity weaponHandle = 0;
            if (const replay::playback::Puppet* weapon = Ctx().puppets.Find(replay::entity::ReplayId::FromPacked(intent.weapon));
                weapon && weapon->state == replay::playback::PuppetState::Ready && weapon->clone &&
                replay::entities::Exists(weapon->clone))
            {
                weaponHandle = weapon->clone;
                input.object = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(weapon->clone));
            }
            if (input.object)
                replay::mem::Read(static_cast<std::uintptr_t>(input.object + wf::kObjectWeaponOffset), input.weapon);
            if (wf::Plausible(input.weapon))
                replay::mem::Read(static_cast<std::uintptr_t>(input.weapon + wf::kWeaponInfoOffset), input.weaponInfo);
            if (intent.shooter != wf::kNoId)
                if (const replay::playback::Puppet* shooter = Ctx().puppets.Find(replay::entity::ReplayId::FromPacked(intent.shooter));
                    shooter && shooter->state == replay::playback::PuppetState::Ready && shooter->clone &&
                    replay::entities::Exists(shooter->clone) && replay::entities::IsPed(shooter->clone))
                    input.shooter = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(shooter->clone));
            if (intent.kind == static_cast<std::uint8_t>(wf::Kind::Muzzle) && wf::Plausible(input.weapon) &&
                wf::Plausible(input.weaponInfo))
                input.info = WfxMuzzleInfo(input.weapon, input.weaponInfo, intent, call.infoSource);
            const wf::CallPlan plan = wf::PlanCall(intent, input);
            // Where the engine will judge it: the weapon puppet against the
            // viewport (weapon_fx_intent_core.h) and against the camera that renders.
            if (weaponHandle)
            {
                float point[3] = {};
                replay::entities::Position(weaponHandle, point);
                call.inView = WfxEngineInView(point);
                const Vector3 cam = CAM::GET_FINAL_RENDERED_CAM_COORD();
                const Vector3 rot = CAM::GET_FINAL_RENDERED_CAM_ROT(2);
                const float camera[3] = {cam.x, cam.y, cam.z};
                call.camView = wf::InView(camera, rot.x, rot.z, CAM::GET_FINAL_RENDERED_CAM_FOV(), point) ? 1 : 0;
                const float dx = point[0] - cam.x, dy = point[1] - cam.y, dz = point[2] - cam.z;
                call.distM = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (intent.kind == static_cast<std::uint8_t>(wf::Kind::Muzzle) && intent.range > 0.0f)
                    call.inRange = call.distM < intent.range ? 1 : 0;
            }
            if (!plan.ok)
            {
                call.result = plan.why;
                ++g_wfxTotals.skipped;
            }
            else
            {
                const std::uint64_t refusedBefore = replay::audio::RefusedPuppetOneShots();
                const std::uint64_t blockedBefore = replay::audio::BlockedPuppetCreates();
                replay::fx::WeaponFxPlayResult result;
                call.beganUs = WfxCensusUs();
                const bool ok = replay::fx::CallWeaponFxRequest(plan.kind, reinterpret_cast<void*>(plan.shooter),
                    reinterpret_cast<void*>(plan.weapon), reinterpret_cast<void*>(plan.object), plan.bone,
                    reinterpret_cast<const void*>(plan.info), plan.vec, plan.argA, result);
                call.endedUs = WfxCensusUs();
                call.refused = static_cast<std::int32_t>(replay::audio::RefusedPuppetOneShots() - refusedBefore);
                call.blocked = static_cast<std::int32_t>(replay::audio::BlockedPuppetCreates() - blockedBefore);
                call.result = result.why;
                call.code = result.code;
                call.rip = result.rip;
                call.starts = result.starts;
                call.shellGets = result.shellGets;
                call.createdFx = plan.kind == 1 ? result.startsMuzzle : result.shellCreated;
                if (result.called)
                {
                    ++g_wfxTotals.calls;
                    if (!ok)
                    {
                        ++g_wfxTotals.failed;
                        // A fault inside the engine: no more calls this playback (the line says where).
                        Ctx().log << "WEAPON_FX_INTENT_PLAY_FAULT,kind=" << static_cast<int>(plan.kind) << std::hex << ",code=0x"
                                  << result.code << ",rip=0x" << result.rip << std::dec << ",t=" << intent.timeMs
                                  << ",playhead_ms=" << timeMs << '\n';
                        g_wfxPlayOn = false;
                    }
                    else if (call.createdFx)
                        ++g_wfxTotals.created;
                }
                else
                    ++g_wfxTotals.skipped;
                if (call.shot >= 0 && static_cast<std::size_t>(call.shot) < g_wfxShotCalled.size() &&
                    !g_wfxShotCalled[static_cast<std::size_t>(call.shot)])
                {
                    g_wfxShotCalled[static_cast<std::size_t>(call.shot)] = 1;
                    ++g_wfxTotals.shotsCalled;
                }
            }
            if (call.inView == 1)
                ++g_wfxTotals.viewIn;
            else if (call.inView == 0)
                ++g_wfxTotals.viewOut;
            else
                ++g_wfxTotals.viewUnknown;
            if (call.inView >= 0 && call.camView >= 0)
                ++(call.inView == call.camView ? g_wfxTotals.viewAgree : g_wfxTotals.viewDisagree);
            // The shot's sound window: joins an open one of the same weapon.
            const std::uint32_t window = intent.timeMs / replay::census::kWindowMs;
            for (WfxShotWindow& open : g_wfxWindows)
                if (open.weapon == intent.weapon && window >= open.window && window <= open.window + 1 && open.calls.size() < 8)
                {
                    open.calls.push_back(call);
                    return;
                }
            if (g_wfxWindows.size() >= replay::weaponfx::kMaxSoundWindows)
            {
                // Full: closed at once (unmeasured), so no call goes unlogged.
                WfxShotWindow lone;
                lone.weapon = intent.weapon;
                lone.shooter = intent.shooter;
                lone.window = window;
                lone.partial = true;
                lone.calls.push_back(call);
                WfxCloseWindow(lone, replay::census::Enabled());
                return;
            }
            WfxShotWindow fresh;
            fresh.weapon = intent.weapon;
            fresh.shooter = intent.shooter;
            fresh.window = window;
            // No weapon recorded: no owner to count (kNoId is the census's
            // "no owner", the world's rows), unmeasured.
            fresh.partial = intent.weapon == replay::weaponfx::kNoId;
            fresh.calls.push_back(call);
            g_wfxWindows.push_back(std::move(fresh));
        }
    }

    void PlayWeaponFxIntents(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.weapon_fx_intent");
        const bool normal = Ctx().hooks && Ctx().hooks->NormalPlay();
        WfxTallyRows(timeMs, normal);
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
        if (!g_wfxCursor.Advance(timeMs, normal, fromMs, toMs) || !g_wfxPlayOn)
            return;
        const std::vector<replay::weaponfx::Intent>& intents = Ctx().clip.weaponFxIntents.intents;
        auto at = std::upper_bound(intents.begin(), intents.end(), fromMs,
            [](std::uint32_t t, const replay::weaponfx::Intent& i) { return t < i.timeMs; });
        std::uint32_t made = 0;
        for (; at != intents.end() && at->timeMs <= toMs && g_wfxPlayOn; ++at)
        {
            if (made >= replay::weaponfx::kMaxCallsPerTick)
            {
                ++g_wfxTotals.capped;
                continue;
            }
            ++made;
            WfxPlayOne(*at, timeMs);
        }
    }
}
