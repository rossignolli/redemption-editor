// replay_objects_stands.cpp: the props played: the hat dresser, the props update, stands and originals, gore pieces.
// Split from replay_objects.cpp (moved code); shared internals in replay_objects_internal.h.

#include "replay_objects_internal.h"

namespace replay::objects
{
    // The stand-ins and the originals the
    // stands hide, sorted (defined with the stands).
    void CollectPoseV2ReplayOwnObjects(std::vector<Object>& out);
    namespace
    {

        // The layers a prop wears: its own, or the ones its owner lost at the
        // pose segment nearest to the prop's create.
        const char* PropLayersOf(const replay::entity::CreatePacket& prop, replay::entity::Appearance& out)
        {
            return replay::entity::PropLayers(
                Ctx().clip.segments, Ctx().clip.FindCreate(prop.owner.Packed()), prop, out);
        }

    // hat_look.h: the hat dresser. A hat with a recorded look and no owner
    // puppet to drop it (a scenery hat, a moved world object, a dropped hat
    // whose owner is not built or did not drop it) is made the way the engine
    // made it: a helper ped wears the layer and the engine drops it
    // (KNOCK_OFF_PED_PROP); the dropped object carries the look and becomes the
    // stand-in or the prop's puppet. The helper (the player's model: the story
    // script puts any picked up hat's layer on the player with
    // _SET_META_PED_TAG, short_update.c EVENT_PLAYER_HAT_EQUIPPED) stands
    // kDresserDepthM under the hat, frozen, without collision. One hat at a
    // time; each client asks every tick until it gets its object or is told it
    // failed (then nothing is drawn).
    struct HatDresser
    {
        enum class Step : std::uint8_t
        {
            Helper,
            Assets,
            Wear,
            Drop,
        };
        Ped helper = 0;
        std::uint64_t client = 0; // 0: idle
        Step step = Step::Helper;
        std::uint64_t since = 0;  // the job's start (GetTickCount64)
        std::uint64_t stepAt = 0; // the step's start
        std::uint64_t woreAt = 0;
        Object hatBefore = 0;
        std::vector<int> requests; // metaped asset requests held for the job
        bool assetsLoaded = false;
        bool knockLate = false;
        std::unordered_set<std::uint64_t> failedClients;
        std::uint32_t jobs = 0;
        std::uint32_t made = 0;
        std::uint32_t failed = 0;
        std::uint32_t logs = 0;
    } g_hatDresser;
    }
    // The hat models of the clip (hatlook::IsHatModel's learned list) and
    // whether a hat is rebuilt by its look (ini HatLook).
    std::vector<std::uint32_t> g_hatModels;
    namespace
    {
    std::uint32_t g_hatHiddenLogs = 0;
    constexpr std::uint32_t kHatDressLogCap = 48;

    std::uint64_t HatClientOfStand(std::size_t stand) { return (1ull << 32) | static_cast<std::uint32_t>(stand); }
    std::uint64_t HatClientOfProp(replay::entity::ReplayId id) { return (2ull << 32) | id.Packed(); }

    void ReleasePoseV2HatRequests()
    {
        for (const int request : g_hatDresser.requests)
            PED::_RELEASE_META_PED_ASSET_REQUEST(request);
        g_hatDresser.requests.clear();
    }

    // The helper is drawn like the owner puppets that drop
    // their hats (the knock of an undrawn ped is unproven) only while it
    // wears a hat for a job, out of sight under the ground; hidden otherwise.
    void PoseV2HatHelperDrawn(bool drawn)
    {
        const HatDresser& d = g_hatDresser;
        if (d.helper && replay::entities::Exists(d.helper))
            ENTITY::SET_ENTITY_VISIBLE(d.helper, drawn ? TRUE : FALSE);
    }

    void LogPoseV2HatDress(const char* status, std::uint64_t client, std::uint32_t model,
        const std::vector<replay::entity::MetaPedAsset>& look, const char* verify, Object hat, std::uint32_t timeMs)
    {
        if (g_hatDresser.logs >= kHatDressLogCap)
            return;
        ++g_hatDresser.logs;
        const HatDresser& d = g_hatDresser;
        char line[320];
        std::snprintf(line, sizeof(line),
            "POSE_V2_HAT_DRESS,status=%s,client=%s:%u,model=0x%x,drawable=0x%x,tags=%zu,assets_loaded=%d,knock_late=%d,"
            "verify=%s,hat=%d,hat_model=0x%x,job_ms=%llu,playhead_ms=%u\n",
            status, (client >> 32) == 1 ? "stand" : "prop", static_cast<std::uint32_t>(client & 0xFFFFFFFFu), model,
            look.empty() ? 0u : look.front().drawable, look.size(), d.assetsLoaded ? 1 : 0, d.knockLate ? 1 : 0, verify,
            hat, hat && replay::entities::Exists(hat) ? static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(hat)) : 0u,
            static_cast<unsigned long long>(GetTickCount64() - d.since), timeMs);
        Ctx().log << line;
    }

    // The job of `client` ends without a hat; the client is told so on its
    // next ask (and never asks again this playback).
    void FailPoseV2HatJob(const char* why, std::uint32_t model, const std::vector<replay::entity::MetaPedAsset>& look,
        std::uint32_t timeMs)
    {
        HatDresser& d = g_hatDresser;
        LogPoseV2HatDress(why, d.client, model, look, "none", 0, timeMs);
        ReleasePoseV2HatRequests();
        PoseV2HatHelperDrawn(false);
        d.failedClients.insert(d.client);
        d.client = 0;
        ++d.failed;
    }

    // The job's client stopped asking (its puppet deleted, the playhead
    // moved away): the dresser is freed, the client may ask again later (a
    // hat the helper dropped meanwhile is deleted).
    void AbandonPoseV2HatJob(std::uint32_t timeMs)
    {
        HatDresser& d = g_hatDresser;
        if (d.step == HatDresser::Step::Drop && d.helper && replay::entities::Exists(d.helper))
        {
            Object stray = PED::_GET_PED_LAST_DROPPED_HAT(d.helper);
            if (stray && stray != d.hatBefore && replay::entities::Exists(stray))
            {
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(stray, TRUE, TRUE);
                ENTITY::DELETE_ENTITY(&stray);
            }
        }
        LogPoseV2HatDress("abandoned", d.client, 0, {}, "none", 0, timeMs);
        ReleasePoseV2HatRequests();
        PoseV2HatHelperDrawn(false);
        d.client = 0;
    }

    bool EnsurePoseV2HatHelper(const float at[3])
    {
        HatDresser& d = g_hatDresser;
        if (d.helper && replay::entities::Exists(d.helper))
            return true;
        d.helper = 0;
        const Hash model = ENTITY::GET_ENTITY_MODEL(PLAYER::PLAYER_PED_ID());
        if (!replay::pose::playback::RequestModel(model))
            return false;
        replay::pose::WorldTransform where{};
        where.position.x = at[0];
        where.position.y = at[1];
        where.position.z = at[2] - replay::hatlook::kDresserDepthM;
        d.helper = replay::pose::playback::CreatePuppet(model, false, where);
        if (!d.helper)
            return false;
        // Hidden (CreatePuppet) until it wears a hat: drawn only from the
        // wear to the drop (PoseV2HatHelperDrawn).
        return true;
    }


    // The object of the hat `client` asked for once it is made (0 while the
    // dresser works on it or on another hat); `failed` when it cannot be.
    Object DressPoseV2Hat(std::uint64_t client, std::uint32_t model, const std::vector<replay::entity::MetaPedAsset>& look,
        const float at[3], bool& failed, std::uint32_t timeMs)
    {
        failed = false;
        HatDresser& d = g_hatDresser;
        if (look.empty() || d.failedClients.count(client))
        {
            failed = true;
            return 0;
        }
        const std::uint64_t now = GetTickCount64();
        if (d.client && d.client != client)
        {
            if (now - d.since <= replay::hatlook::kJobTimeoutMs)
                return 0; // another hat first
            AbandonPoseV2HatJob(timeMs); // every step has its own limit: its client stopped asking
        }
        if (!d.client)
        {
            d.client = client;
            d.step = HatDresser::Step::Helper;
            d.since = d.stepAt = now;
            d.woreAt = 0;
            d.hatBefore = 0;
            d.assetsLoaded = false;
            d.knockLate = false;
            ReleasePoseV2HatRequests();
            ++d.jobs;
        }
        switch (d.step)
        {
        case HatDresser::Step::Helper:
            if (!EnsurePoseV2HatHelper(at))
            {
                if (now - d.stepAt > replay::hatlook::kHelperWaitMs)
                {
                    FailPoseV2HatJob("no_helper", model, look, timeMs);
                    failed = true;
                }
                return 0;
            }
            ENTITY::SET_ENTITY_COORDS_NO_OFFSET(d.helper, at[0], at[1], at[2] - replay::hatlook::kDresserDepthM, FALSE, FALSE, FALSE);
            // The layer's assets held for the job, as the story script holds
            // a picked up hat's (_0x3FCBB5FCFD968698, released by
            // _RELEASE_META_PED_ASSET_REQUEST).
            for (const replay::entity::MetaPedAsset& asset : look)
                d.requests.push_back(PED::_0x3FCBB5FCFD968698(asset.drawable, asset.albedo, asset.normal, asset.material, 0));
            d.step = HatDresser::Step::Assets;
            d.stepAt = now;
            return 0;
        case HatDresser::Step::Assets:
        {
            bool loaded = true;
            for (const int request : d.requests)
                loaded = loaded && PED::_HAS_META_PED_ASSET_LOADED(request);
            if (!loaded && now - d.stepAt < replay::hatlook::kAssetWaitMs)
                return 0;
            d.assetsLoaded = loaded;
            PoseV2HatHelperDrawn(true);
            for (const replay::entity::MetaPedAsset& asset : look)
                PED::_SET_META_PED_TAG(d.helper, asset.drawable, asset.albedo, asset.normal, asset.material, asset.palette,
                    asset.tint0, asset.tint1, asset.tint2);
            PED::_UPDATE_PED_VARIATION(d.helper, FALSE, TRUE, TRUE, TRUE, FALSE);
            d.woreAt = now;
            d.step = HatDresser::Step::Wear;
            d.stepAt = now;
            return 0;
        }
        case HatDresser::Step::Wear:
        {
            replay::entity::Appearance layers;
            layers.assets = look;
            const replay::entity::appearance::Match worn = replay::entity::appearance::Compare(d.helper, layers);
            const replay::hatlook::KnockStep knock =
                replay::hatlook::WhenToKnock(d.woreAt, now, worn.recorded && worn.present >= worn.recorded);
            if (knock == replay::hatlook::KnockStep::Wait)
                return 0;
            d.knockLate = knock == replay::hatlook::KnockStep::KnockLate;
            d.hatBefore = PED::_GET_PED_LAST_DROPPED_HAT(d.helper);
            PED::KNOCK_OFF_PED_PROP(d.helper, TRUE, TRUE, TRUE, TRUE);
            d.step = HatDresser::Step::Drop;
            d.stepAt = now;
            return 0;
        }
        case HatDresser::Step::Drop:
        {
            Object hat = PED::_GET_PED_LAST_DROPPED_HAT(d.helper);
            if (!hat || hat == d.hatBefore || !replay::entities::Exists(hat))
            {
                if (now - d.stepAt < replay::hatlook::kDropWaitMs)
                    return 0;
                FailPoseV2HatJob("no_drop", model, look, timeMs);
                failed = true;
                return 0;
            }
            // The dropped object read back: the helper's own hat (the layer
            // not taken) is refused, an object whose tags are not readable is
            // kept (logged).
            std::vector<replay::entity::MetaPedAsset> dropped;
            const char* status = ReadPoseV2HatLook(hat, dropped);
            const replay::hatlook::Verify verify =
                replay::hatlook::VerifyDropped(look, dropped, std::strcmp(status, "read") == 0);
            if (verify == replay::hatlook::Verify::Other)
            {
                LogPoseV2HatDress("other_look", d.client, model, look, replay::hatlook::VerifyText(verify), hat, timeMs);
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(hat, TRUE, TRUE);
                ENTITY::DELETE_ENTITY(&hat);
                FailPoseV2HatJob("refused", model, look, timeMs);
                failed = true;
                return 0;
            }
            LogPoseV2HatDress("made", d.client, model, look, replay::hatlook::VerifyText(verify), hat, timeMs);
            ReleasePoseV2HatRequests();
            PoseV2HatHelperDrawn(false);
            d.client = 0;
            ++d.made;
            return hat;
        }
        }
        return 0;
    }
    }

    // Playback end (and a script restart: forget, no native).
    void EndPoseV2HatDresser(bool forget)
    {
        HatDresser& d = g_hatDresser;
        if (!forget)
        {
            if (d.client)
                AbandonPoseV2HatJob(0);
            ReleasePoseV2HatRequests();
            if (d.helper && replay::entities::Exists(d.helper))
            {
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(d.helper, TRUE, TRUE);
                PED::DELETE_PED(&d.helper);
            }
            if (d.jobs)
                Ctx().log << "POSE_V2_HAT_DRESS_SUMMARY,jobs=" << d.jobs << ",made=" << d.made << ",failed=" << d.failed
                          << ",hidden_bare=" << g_hatHiddenLogs << '\n';
        }
        d = HatDresser{};
        g_hatHiddenLogs = 0;
    }
    namespace
    {

    // How a prop or stand of `model` with `look` is built (and the evidence
    // line of a hidden bare hat, once per object).
    replay::hatlook::Rebuild PoseV2HatRebuild(std::uint32_t model, const std::vector<replay::entity::MetaPedAsset>& look)
    {
        return replay::hatlook::ChooseRebuild(true, !look.empty(),
            replay::hatlook::IsHatModel(model, g_hatModels));
    }

    void LogPoseV2HatHidden(const char* path, const std::string& what, std::uint32_t model, std::uint32_t timeMs)
    {
        ++g_hatHiddenLogs;
        if (g_hatHiddenLogs > kHatDressLogCap)
            return;
        Ctx().log << "POSE_V2_HAT_DRESS,status=hidden_bare,path=" << path << ",what=" << what << ",model=0x" << std::hex << model
                  << std::dec << ",why=no_recorded_look,playhead_ms=" << timeMs << '\n';
    }
    }

    void UpdatePoseV2Props(std::uint32_t timeMs, std::uint32_t& creates)
    {
        REPLAY_PROFILE_ZONE("play.props");
        g_wheelPlayheadMs = timeMs;
        const ULONGLONG now = GetTickCount64();
        if (!g_propRetryOnly)
        {
            g_fragOrderWaited.clear();
            g_propMadeThisPass = false;
        }
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind != replay::entity::EntityKind::Prop ||
                puppet.state == replay::playback::PuppetState::Failed ||
                (g_propRetryOnly && !g_propRetryOnly->count(puppet.packet.id.Packed())))
            {
                continue;
            }
            const replay::entity::RelativeTrack* track =
                Ctx().clip.WeaponTrackOf(puppet.packet.id.Packed());
            replay::entity::TransformSample sample;
            const bool sampled = track && track->Sample(timeMs, sample, &Ctx().clip.cuts); // cut-aware
            if (puppet.state == replay::playback::PuppetState::Loading &&
                puppet.packet.propOrigin == replay::entity::PropOrigin::VehiclePiece)
            {
                if (!CreatePoseV2VehiclePiece(puppet, sampled, sample, timeMs, creates))
                    continue;
            }
            else if (puppet.state == replay::playback::PuppetState::Loading)
            {
                // A puppet built again (scrub back before a break, PROP_LOST)
                // is a whole object: it is made the piece again even when the
                // new handle repeats the old one (the shaped map compares
                // handles only).
                g_fragPieceShaped.erase(puppet.packet.id.Packed());
                // A prop of a vehicle's model that is no vehicle
                // piece (clips before it: a wagon board taken as a
                // projectile's successor) would be built as the whole vehicle.
                if (STREAMING::IS_MODEL_A_VEHICLE(puppet.packet.modelHash))
                {
                    puppet.state = replay::playback::PuppetState::Failed;
                    Ctx().hooks->LogPuppet(puppet, "vehicle_model_refused", timeMs);
                    continue;
                }
                // A rope's own entity recorded as a prop by a clip older than
                // the recording's filter never loads (rope_models.h): failed
                // without a request, not after the model timeout (bench:
                // six ROPE_REINS props ran it out on QA-MountHagen010).
                if (!puppet.packet.weaponHash && replay::ropemodels::IsRopeModel(puppet.packet.modelHash))
                {
                    puppet.state = replay::playback::PuppetState::Failed;
                    Ctx().hooks->LogPuppet(puppet, "rope_model", timeMs);
                    continue;
                }
                const bool hat = puppet.packet.propOrigin == replay::entity::PropOrigin::DroppedHat;
                const bool gorePiece = puppet.packet.propOrigin == replay::entity::PropOrigin::GorePiece;
                const bool loaded = (puppet.packet.weaponHash
                    ? replay::pose::playback::RequestWeaponAsset(puppet.packet.weaponHash)
                    : replay::pose::playback::RequestModel(puppet.packet.modelHash)) &&
                    replay::pose::playback::RequestWeaponComponents(puppet.packet.weaponComponents);
                if (!sampled)
                {
                    // A gore piece lost to a gore clear before its span waits
                    // for the span (the reconcile drops a real piece again).
                    if (puppet.packet.propOrigin == replay::entity::PropOrigin::GorePiece)
                    {
                        puppet.stateSince = now;
                        continue;
                    }
                    if (now - puppet.stateSince > replay::playback::kModelTimeoutMs)
                    {
                        puppet.state = replay::playback::PuppetState::Failed;
                        Ctx().hooks->LogPuppet(puppet, "no_track", timeMs);
                    }
                    continue;
                }
                replay::playback::Puppet* owner = Ctx().puppets.Find(puppet.packet.owner);
                const bool ownerReady = owner && owner->state == replay::playback::PuppetState::Ready &&
                    owner->clone && replay::entities::Exists(owner->clone);
                // The engine builds the prop the way it did in the game: the
                // owner puppet wears the prop's layers and drops it, and the
                // dropped object carries the look (the generic prop model
                // takes no metaped layers from script).
                if (hat && !puppet.knockRequested && ownerReady)
                {
                    replay::entity::Appearance layers;
                    const char* source = PropLayersOf(puppet.packet, layers);
                    const replay::entity::appearance::Match worn =
                        replay::entity::appearance::Compare(owner->clone, layers);
                    const bool wear = !layers.assets.empty() && worn.present < worn.recorded;
                    if (wear && !puppet.hatWoreAt)
                    {
                        for (const replay::entity::MetaPedAsset& asset : layers.assets)
                        {
                            PED::_SET_META_PED_TAG(owner->clone, asset.drawable, asset.albedo,
                                asset.normal, asset.material, asset.palette,
                                asset.tint0, asset.tint1, asset.tint2);
                        }
                        PED::_UPDATE_PED_VARIATION(owner->clone, FALSE, TRUE, TRUE, TRUE, FALSE);
                        // The owner's skeleton was rebuilt with the hat: its
                        // pose again before this frame renders.
                        Ctx().hooks->RefreshOverride(*owner, timeMs);
                        // hatlook::WhenToKnock the knock waits for the layer to
                        // land (a same-tick knock drops nothing: the plain
                        // model, an octagon).
                        puppet.hatWoreAt = now;
                        continue;
                    }
                    bool knockLate = false;
                    if (puppet.hatWoreAt)
                    {
                        const replay::hatlook::KnockStep step = replay::hatlook::WhenToKnock(puppet.hatWoreAt, now, !wear);
                        if (step == replay::hatlook::KnockStep::Wait)
                            continue;
                        knockLate = step == replay::hatlook::KnockStep::KnockLate;
                    }
                    puppet.hatBefore = PED::_GET_PED_LAST_DROPPED_HAT(owner->clone);
                    PED::KNOCK_OFF_PED_PROP(owner->clone, TRUE, TRUE, TRUE, TRUE);
                    Ctx().hooks->RefreshOverride(*owner, timeMs);
                    puppet.knockRequested = true;
                    puppet.knockDeadline = now + 1000;
                    std::ostream& log = Ctx().log;
                    log << "POSE_V2_PLAYBACK_PROP_KNOCK"
                        << ",id=" << replay::entity::IdText(puppet.packet.id)
                        << ",owner=" << replay::entity::IdText(puppet.packet.owner)
                        << ",source=" << source
                        << ",layers=" << layers.assets.size()
                        << ",drawable=0x" << std::hex
                        << (layers.assets.empty() ? 0u : layers.assets[0].drawable) << std::dec
                        << ",was_worn=" << (puppet.hatWoreAt || wear ? 0 : 1)
                        << ",wear_wait_ms=" << (puppet.hatWoreAt ? now - puppet.hatWoreAt : 0)
                        << ",knock_late=" << (knockLate ? 1 : 0)
                        << ",playhead_ms=" << timeMs
                        << '\n';
                    log.flush();
                    continue;
                }
                Object dropped = 0;
                // A clip recorded before took a map object's fragment (the
                // railing) for a gore piece: it is not built whole.
                if (gorePiece && !replay::objcheck::GorePiecePlayable(
                                     IsPedGoreModel(puppet.packet.modelHash), PoseV2ClipWorldModel(puppet.packet.modelHash)))
                {
                    ++g_goreWorldModelRefused;
                    puppet.state = replay::playback::PuppetState::Failed;
                    Ctx().hooks->LogPuppet(puppet, "gore_piece_world_model", timeMs);
                    continue;
                }
                if (gorePiece && sampled)
                {
                    // The gore event replayed on the owner puppet makes the
                    // engine drop its own piece (with the owner's look): the
                    // nearest free object of the recorded model near the
                    // recorded position. Waits briefly, then the bare model.
                    static int objects[1024];
                    const int count = (std::min)(ListObjects(objects, 1024), 1024);
                    // Never a stand-in or the original a stand hides (the stand
                    // hid it, the prop showed it: duplicate why=reshown).
                    static std::vector<Object> standObjects;
                    CollectPoseV2ReplayOwnObjects(standObjects);
                    // First only a piece at the recorded spot (the engine
                    // drops it there within a frame or two); later anything
                    // of the model close enough.
                    const bool strict = now - puppet.stateSince < 300;
                    float best = strict ? 0.3f : 2.5f;
                    for (int slot = 0; slot < count; ++slot)
                    {
                        const Object object = objects[slot];
                        if (!object || !replay::entities::Exists(object) ||
                            static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object)) != puppet.packet.modelHash ||
                            ENTITY::IS_ENTITY_ATTACHED(object) ||
                            !replay::objcheck::AdoptForGore(ENTITY::IS_ENTITY_A_MISSION_ENTITY(object) != FALSE,
                                std::binary_search(standObjects.begin(), standObjects.end(), object), false))
                        {
                            continue;
                        }
                        bool taken = false;
                        for (const replay::playback::Puppet& other : Ctx().puppets.list)
                            if (other.clone == object)
                                taken = true;
                        if (taken)
                            continue;
                        const Vector3 at = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
                        const float dx = at.x - sample.relative.position.x;
                        const float dy = at.y - sample.relative.position.y;
                        const float dz = at.z - sample.relative.position.z;
                        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                        if (distance < best)
                        {
                            best = distance;
                            dropped = object;
                        }
                    }
                    if (!dropped && count == 0)
                    {
                        Vector3 at{};
                        at.x = sample.relative.position.x;
                        at.y = sample.relative.position.y;
                        at.z = sample.relative.position.z;
                        dropped = FindLooseObject(puppet.packet.modelHash, at, best, true);
                        if (dropped && (ENTITY::IS_ENTITY_A_MISSION_ENTITY(dropped) ||
                                           std::binary_search(standObjects.begin(), standObjects.end(), dropped)))
                            dropped = 0;
                    }
                    if (!dropped && now - puppet.stateSince < 600)
                        continue;
                    Ctx().log << "POSE_V2_PLAYBACK_GORE_PIECE,id=" << replay::entity::IdText(puppet.packet.id)
                        << ",owner=" << replay::entity::IdText(puppet.packet.owner) << ",model=0x" << std::hex
                        << puppet.packet.modelHash << std::dec << ",adopted=" << dropped << ",distance="
                        << (dropped ? best : -1.0f) << ",objects=" << count << ",strict=" << strict << ",wait_ms=" << (now - puppet.stateSince)
                        << ",playhead_ms=" << timeMs << '\n';
                }
                else if (puppet.knockRequested)
                {
                    const Object droppedHat = ownerReady ? PED::_GET_PED_LAST_DROPPED_HAT(owner->clone) : 0;
                    if (droppedHat && droppedHat != puppet.hatBefore && replay::entities::Exists(droppedHat))
                        dropped = droppedHat;
                    else if (now < puppet.knockDeadline)
                        continue;
                    // The drop changed (or tried to change) the owner's
                    // layers and skeleton: it takes its segment again next
                    // tick, and its pose now.
                    if (owner)
                    {
                        owner->recordId = 0;
                        if (ownerReady)
                            Ctx().hooks->RefreshOverride(*owner, timeMs);
                    }
                }
                else if (hat && now - puppet.stateSince < replay::playback::kModelTimeoutMs)
                {
                    continue; // waiting for the owner puppet
                }
                // hat_look.h: a hat object not dropped by its owner puppet (no
                // owner, the knock dropped nothing, or a hat recorded by
                // another path: a moved world object, a picked up hat) is made
                // by the hat dresser with its recorded look; a hat model
                // without a look is not drawn (the bare model is the grey
                // octagon).
                bool dressed = false;
                if (!dropped && !gorePiece && !puppet.packet.weaponHash)
                {
                    replay::entity::Appearance look;
                    if (hat)
                        PropLayersOf(puppet.packet, look);
                    else
                        look.assets = puppet.packet.appearance.assets;
                    const replay::hatlook::Rebuild rebuild = PoseV2HatRebuild(puppet.packet.modelHash, look.assets);
                    if (rebuild == replay::hatlook::Rebuild::Dress)
                    {
                        bool dressFailed = false;
                        const float at[3] = {sample.relative.position.x, sample.relative.position.y, sample.relative.position.z};
                        dropped = DressPoseV2Hat(HatClientOfProp(puppet.packet.id), puppet.packet.modelHash, look.assets, at,
                            dressFailed, timeMs);
                        if (!dropped && !dressFailed)
                            continue;
                        if (!dropped)
                        {
                            puppet.state = replay::playback::PuppetState::Failed;
                            Ctx().hooks->LogPuppet(puppet, "hat_dress_failed", timeMs);
                            continue;
                        }
                        dressed = true;
                    }
                    else if (rebuild == replay::hatlook::Rebuild::Hide)
                    {
                        puppet.state = replay::playback::PuppetState::Failed;
                        LogPoseV2HatHidden("prop", replay::entity::IdText(puppet.packet.id), puppet.packet.modelHash, timeMs);
                        Ctx().hooks->LogPuppet(puppet, "hat_hidden_bare", timeMs);
                        continue;
                    }
                }
                if (dropped)
                {
                    ENTITY::SET_ENTITY_AS_MISSION_ENTITY(dropped, TRUE, TRUE);
                    ENTITY::SET_ENTITY_INVINCIBLE(dropped, TRUE);
                    ENTITY::SET_ENTITY_COLLISION(dropped, FALSE, FALSE);
                    ENTITY::FREEZE_ENTITY_POSITION(dropped, TRUE);
                    replay::pose::playback::PlaceClone(dropped, sample.relative);
                    SuppressPickupLook(dropped);
                    puppet.clone = dropped;
                }
                else
                {
                    // A dropped weapon is its weapon object again; a hat
                    // without its owner falls back to the bare prop model.
                    if (!loaded)
                    {
                        if (now - puppet.stateSince > replay::playback::kModelTimeoutMs)
                        {
                            puppet.state = replay::playback::PuppetState::Failed;
                            Ctx().hooks->LogPuppet(puppet, "model_timeout", timeMs);
                        }
                        continue;
                    }
                    // A fragment piece comes off in the frame the engine broke it
                    // (an explosion breaks a wheel into a dozen at once): the
                    // per-frame stagger of the creates is wider for them.
                    const std::uint32_t createCap = FragPieceOf(puppet.packet.id) ? kMaxPieceCreatesPerFrame
                                                                                  : replay::playback::kMaxCreatesPerFrame;
                    if (creates >= createCap || now < puppet.nextCreateAt)
                        continue;
                    // A dropped wheel is the wheel object a vehicle puppet
                    // gave; a wheel's fragment piece breaks off its wheel.
                    Object wheelObject = 0;
                    const int fromParent = PoseV2FragPieceFromParent(puppet, sample.relative, now, timeMs, wheelObject);
                    if (fromParent < 0)
                        continue;
                    if (!fromParent && PoseV2WheelCopy(puppet, sample.relative, wheelObject, now, timeMs,
                                           FragPieceOf(puppet.packet.id) != nullptr) == replay::vehicle_pieces::WheelCopy::Wait)
                        continue;
                    ++creates;
                    ++puppet.createAttempts;
                    puppet.clone = wheelObject ? wheelObject : replay::pose::playback::CreateWeaponPuppet(
                        puppet.packet.weaponHash, puppet.packet.modelHash, sample.relative);
                    puppet.conditionApplied = -1;
                    puppet.strapApplied = -1;
                    puppet.weaponLookCheckAt = puppet.weaponLookWaitUntil = 0;
                    puppet.weaponLookReapplied = 0;
                    ApplyPoseV2WeaponComponents(puppet);
                    if (!puppet.clone)
                    {
                        puppet.nextCreateAt = now + replay::playback::kCreateRetryMs;
                        if (now - puppet.stateSince > replay::playback::kModelTimeoutMs)
                        {
                            puppet.state = replay::playback::PuppetState::Failed;
                            Ctx().hooks->LogPuppet(puppet, "create_failed", timeMs);
                        }
                        continue;
                    }
                    // A loose weapon object would show the gameplay pickup
                    // light: the replay object is a prop.
                    SuppressPickupLook(puppet.clone);
                }
                puppet.state = replay::playback::PuppetState::Ready;
                g_propMadeThisPass = true;
                Ctx().hooks->LogPuppet(puppet,
                    gorePiece ? (dropped ? "adopted_gore_piece" : "created_gore_piece")
                    : dressed ? "created_dressed"
                    : dropped ? "created_knock_off" : hat ? "created_plain" : "created_weapon", timeMs);
            }
            if (!puppet.clone || !replay::entities::Exists(puppet.clone))
            {
                // An adopted engine object can be removed by the engine (a
                // gore piece pool): build it again, as the bare model when
                // nothing new of the model is around.
                if (puppet.state == replay::playback::PuppetState::Ready && puppet.clone)
                {
                    Ctx().log << "POSE_V2_PLAYBACK_PROP_LOST,id=" << replay::entity::IdText(puppet.packet.id)
                        << ",handle=" << puppet.clone << ",playhead_ms=" << timeMs << '\n';
                    // A posed prop's override is disarmed before its handle is
                    // dropped, as DeletePoseV2Puppet does, so the detour never
                    // writes into the skeleton of an object the engine removed
                    // and the override slot is free for the new puppet.
                    Ctx().hooks->ReleaseOverride(puppet);
                    puppet.clone = 0;
                    puppet.state = replay::playback::PuppetState::Loading;
                    puppet.stateSince = now;
                }
                continue;
            }
            // A loose fragment piece is made from the whole object
            // first (hidden while it waits; never drawn whole).
            if (!ShapePoseV2FragPiece(puppet, sampled, sample, timeMs))
                continue;
            std::uint32_t firstMs = 0;
            std::uint32_t lastMs = 0;
            const bool inTrack = track && track->Span(firstMs, lastMs) &&
                timeMs >= firstMs && timeMs <= lastMs + kIntervalMs;
            // A world object whose root group came off was destroyed (all of
            // it broke away; in a recorded test the small crate lost
            // groups 0 to 5): from then on it is not drawn whole.
            bool destroyed = false;
            if (puppet.packet.propOrigin != replay::entity::PropOrigin::VehiclePiece) // any prop
                for (const replay::FragBreak& e : Ctx().clip.fragBreaks)
                    if (e.id == puppet.packet.id && e.group == 0 && e.timeMs <= timeMs)
                        destroyed = true;
            replay::pose::playback::SetCloneVisible(
                puppet.clone, sampled && inTrack && sample.visible && !destroyed);
            // An adopted engine object stays frozen (a fragment woken by a hit
            // runs its physics again).
            if (puppet.packet.propOrigin == replay::entity::PropOrigin::GorePiece && ENTITY::_IS_ENTITY_FROZEN(puppet.clone) == FALSE)
            {
                ENTITY::FREEZE_ENTITY_POSITION(puppet.clone, TRUE);
                ENTITY::SET_ENTITY_COLLISION(puppet.clone, FALSE, FALSE);
                ++g_goreRefrozen;
            }
            if (sampled)
                replay::pose::playback::PlaceClone(puppet.clone, sample.relative);
            if (puppet.packet.weaponHash) // a loose weapon keeps its look
                ApplyPoseV2WeaponLook(puppet, timeMs);
            // object_pose.h PoseableProp: a dropped object with no
            // weapon (the rolled pelt that left the hand) plays the pose its
            // prop id recorded; the others have no pose record.
            // And a free object spawned next to a recorded ped (Projectile
            // origin) plays its pose when it has one. And a world object
            // animated in place (the donation box's lid, the ledger) plays the
            // pose it recorded.
            if (replay::object_pose::PoseableProp(
                    puppet.packet.propOrigin == replay::entity::PropOrigin::DroppedWeapon,
                    puppet.packet.propOrigin == replay::entity::PropOrigin::Projectile ||
                        puppet.packet.propOrigin == replay::entity::PropOrigin::WorldObject,
                    puppet.packet.weaponHash))
                PlayPoseV2ObjectPose(puppet, timeMs);
        }
        // The pieces whose turn came in this tick, again (a few rounds: one
        // per step of the chain).
        for (int round = 0; !g_propRetryOnly && round < 6 && g_propMadeThisPass && !g_fragOrderWaited.empty(); ++round)
        {
            const std::unordered_set<std::uint32_t> retry = std::move(g_fragOrderWaited);
            g_fragOrderWaited.clear();
            g_propMadeThisPass = false;
            g_propRetryOnly = &retry;
            UpdatePoseV2Props(timeMs, creates);
            g_propRetryOnly = nullptr;
        }
    }

        // Log evidence (484 recordings): every registered piece had model
        // 0x5595880b or 0x4f6fa8e4, whatever the ped model.
        std::set<std::uint32_t> g_goreModels{0x5595880bu, 0x4f6fa8e4u};
    namespace
    {

        bool OwnedByPuppet(Entity entity)
        {
            for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
                if (puppet.clone == entity)
                    return true;
            return false;
        }
    }

    bool IsGorePieceModel(std::uint32_t model) { return g_goreModels.count(model) != 0; }
    namespace
    {

        bool g_poolListingDisabled = false;
        // The object pool walk of this frame (ListObjects).
        std::vector<int> g_poolObjects;
        int g_poolObjectsFrame = -1;
        replay::pools::ListStats g_poolObjectStats;
        replay::log::LogLimit g_poolObjectLogs; // per session (was never reset)
        constexpr float kPoolObjectRadius = 80.0f;          // new handles: the nearest objects
        constexpr float kPoolObjectExistingRadius = 200.0f; // objects that have a handle
        constexpr int kPoolObjectBudget = 96;               // new handles a frame
        constexpr int kPoolObjectCapacity = 4096;
    }
    // The object listing reads the object pool (entity_pools.h)
    // instead of worldGetAllObjects, which gives every object in the world a
    // script handle and returns 0 once that pool is near full (the town).
    // Handles for the objects near the player or the camera (new ones within
    // 80 m, nearest first, 96 a frame; those with a handle within 200 m);
    // one walk per frame, the callers ask several times. Every caller looks
    // near a recorded entity, a puppet or the camera. The ScriptHook listing
    // only when the reader is unavailable.
    int ListObjects(int* handles, int capacity)
    {
        if (replay::pools::Available())
        {
            const int frame = MISC::GET_FRAME_COUNT();
            if (frame != g_poolObjectsFrame)
            {
                REPLAY_PROFILE_ZONE("objects.pool_list");
                const Vector3 player = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE, FALSE);
                const Vector3 camera = CAM::GET_FINAL_RENDERED_CAM_COORD();
                const replay::pools::Position centers[2] = {{player.x, player.y, player.z}, {camera.x, camera.y, camera.z}};
                g_poolObjects.resize(kPoolObjectCapacity);
                const int count = replay::pools::ListHandles(replay::pools::Kind::Object, centers, 2, kPoolObjectRadius,
                    kPoolObjectExistingRadius, kPoolObjectBudget, g_poolObjects.data(), kPoolObjectCapacity, &g_poolObjectStats);
                g_poolObjects.resize(static_cast<std::size_t>((std::max)(0, count)));
                g_poolObjectsFrame = count >= 0 ? frame : -1;
                if (count >= 0 && (g_poolObjectLogs.Seen() < 4 || frame % 1800 == 0) && g_poolObjectLogs.Take(16))
                {
                    Ctx().log << "OBJECT_POOL_LIST,live=" << g_poolObjectStats.live << ",handled=" << g_poolObjectStats.listed
                              << ",existing=" << g_poolObjectStats.existing << ",created=" << g_poolObjectStats.created
                              << ",far=" << g_poolObjectStats.skippedFar << ",budget=" << g_poolObjectStats.skippedBudget
                              << ",guid_free=" << g_poolObjectStats.guidFree << "/" << g_poolObjectStats.guidSize
                              << ",us=" << static_cast<int>(g_poolObjectStats.us) << '\n';
                }
            }
            if (g_poolObjectsFrame == frame)
            {
                const int count = (std::min)(capacity, static_cast<int>(g_poolObjects.size()));
                std::copy(g_poolObjects.begin(), g_poolObjects.begin() + count, handles);
                return count;
            }
        }
        return g_poolListingDisabled ? 0 : worldGetAllObjects(handles, capacity);
    }
    int ListAllObjectsGuarded(int* handles, int capacity, const char*& mode)
    {
        mode = "listing";
        if (replay::pools::Available())
        {
            const replay::pools::PoolState objects = replay::pools::State(replay::pools::Kind::Object);
            if (objects.ok && replay::pools::GuidRoom() < objects.used)
            {
                mode = "existing_only";
                const Vector3 player = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE, FALSE);
                const replay::pools::Position center{player.x, player.y, player.z};
                const int count = replay::pools::ListHandles(replay::pools::Kind::Object, &center, 1, 0.0f, 1.0e9f, 0,
                    handles, capacity, nullptr);
                if (count >= 0)
                    return count;
                mode = "listing";
            }
            else if (objects.ok)
                mode = "all";
        }
        return (std::max)(0, worldGetAllObjects(handles, capacity));
    }
    int ListPickups(int* handles, int capacity) { return g_poolListingDisabled ? 0 : worldGetAllPickups(handles, capacity); }
    // The recorder's ped and vehicle candidates go through the
    // same switch, so the lake stage reproduces the town (listing 0).
    int ListPeds(int* handles, int capacity) { return g_poolListingDisabled ? 0 : worldGetAllPeds(handles, capacity); }
    int ListVehicles(int* handles, int capacity) { return g_poolListingDisabled ? 0 : worldGetAllVehicles(handles, capacity); }
    void SetPoolListingDisabled(bool disabled) { g_poolListingDisabled = disabled; }

    Object FindLooseObject(std::uint32_t model, const Vector3& at, float radius, bool excludePuppets)
    {
        static int objects[1024];
        const int count = (std::min)(ListObjects(objects, 1024), 1024);
        Object best = 0;
        float bestDistance = radius;
        const auto consider = [&](Object object) {
            if (!object || !replay::entities::Exists(object) ||
                static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object)) != model ||
                ENTITY::IS_ENTITY_ATTACHED(object) || (excludePuppets && OwnedByPuppet(object)))
                return;
            const Vector3 p = ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE);
            const float dx = p.x - at.x, dy = p.y - at.y, dz = p.z - at.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (distance <= bestDistance)
            {
                bestDistance = distance;
                best = object;
            }
        };
        if (count > 0)
        {
            for (int slot = 0; slot < count; ++slot)
                consider(objects[slot]);
            return best;
        }
        consider(OBJECT::GET_CLOSEST_OBJECT_OF_TYPE(at.x, at.y, at.z, radius, model, FALSE, FALSE, FALSE));
        return best;
    }

    int DeleteStrayGorePieces(Entity around, float radius)
    {
        if (!around || !replay::entities::Exists(around))
            return 0;
        const Vector3 at = ENTITY::GET_ENTITY_COORDS(around, TRUE, FALSE);
        int deleted = 0;
        for (std::uint32_t model : g_goreModels)
        {
            // One at a time: closest-of-type returns the same object until it goes.
            for (int attempt = 0; attempt < 8; ++attempt)
            {
                Object piece = FindLooseObject(model, at, radius, true);
                if (!piece || ENTITY::IS_ENTITY_A_MISSION_ENTITY(piece))
                    break;
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(piece, TRUE, TRUE);
                ENTITY::DELETE_ENTITY(&piece);
                ++deleted;
            }
        }
        return deleted;
    }

    void SuppressPickupLook(Object object)
    {
        if (!object || !replay::entities::Exists(object))
            return;
        GRAPHICS::SET_PICKUP_LIGHT(object, FALSE);
        GRAPHICS::_BLOCK_PICKUP_OBJECT_LIGHT(object, TRUE);
    }
    void SearchDrops()
    {
        SearchPoseV2WeaponDrops();
        SearchPoseV2Projectiles();
    }

    void RetireWeaponsOf(Entity parent, const char* reason)
    {
        std::vector<replay::recorder::RecordedWeapon>& weapons = Ctx().recorder.weapons;
        for (std::size_t weapon = weapons.size(); weapon-- > 0;)
        {
            if (weapons[weapon].parent == parent)
                RetirePoseV2Weapon(weapon, reason);
        }
    }

    std::map<std::uint32_t, std::size_t> g_fragApplied; // prop packed id -> events applied
    std::map<std::uint32_t, Object> g_fragPuppet;       // prop packed id -> puppet it was applied to
    std::uint32_t g_fragBreaksPlayed = 0;
    std::uint32_t g_fragFixes = 0;
    std::uint32_t g_fragUnmapped = 0;
    int g_fragLastError = 0;
    std::uint32_t g_fragMaxPlayheadMs = 0; // how far the playback went
    std::vector<ObjectStand> g_objectStands;
    std::unordered_set<Object> g_objectStandObjects; // the stand-ins, for the searches
    std::uint32_t g_lootStandLogs = 0;               // POSE_V2_PLAYBACK_LOOT lines this playback
    std::uint32_t g_lateStandLogs = 0;               // POSE_V2_PLAYBACK_SCENERY_LATE lines this playback
    std::uint64_t g_objectStandsBegan = 0;
    std::uint64_t g_objectStandsNextSearch = 0;
    struct ObjectStandStats
    {
        std::uint32_t hidden = 0;
        std::uint32_t hiddenLate = 0; // found after the start wait: drawn twice until then
        std::uint32_t reshown = 0;    // made visible again by someone else, hidden again
        std::uint32_t created = 0;
        std::uint32_t failed = 0;
        std::uint32_t searches = 0;
        std::uint32_t childAttaches = 0;  // attaches of a child to a host
        std::uint32_t childOnProp = 0;    // of them on the prop that took the parent over
        std::uint32_t childByFrame = 0;   // of them by the engine frame
        std::uint32_t childNoEuler = 0;   // the rotation not converted yet (helper model)
        std::uint32_t burnWrites = 0;     // burn levels written on stand-ins
        std::uint32_t burnLogs = 0;
        std::uint32_t handoffs = 0;            // stand-ins hidden for a ready prop
        std::uint32_t handoffOverlapTicks = 0; // the ticks both were shown (the overlap kept)
        std::uint32_t handoffLogs = 0;
        // The map dressing verdicts of this playback.
        std::uint32_t mapDressing = 0;
        std::uint32_t mapLive = 0;
        std::uint32_t mapBuildIplOff = 0;
        std::uint32_t mapBuildMissing = 0; // IPL on, no original within kMapDressingWaitMs
        std::uint32_t mapBuildChanged = 0; // its visibility flipped while recorded
        std::uint32_t mapTaken = 0; // built: a prop took it over
        std::uint32_t mapDecidedWithoutWorld = 0; // the map state never looked at (the fallback wait)
        std::uint32_t mapLogs = 0;
        std::set<std::uint32_t> mapIplsOff;
    } g_objectStandStats;
    // The map dressing decides once the clip's map state was looked at
    // (replay::world::PlaybackApplied), or after this long without it.
    constexpr std::uint64_t kMapDressingWorldFallbackMs = 15000;
    constexpr std::uint64_t kStandFastSearchMs = 10000;
    constexpr std::uint64_t kStandSlowSearchMs = 3000;
    constexpr std::uint64_t kStandLateMs = 3000;

    // The stand-in's model this tick (objcheck::StandModelStep): true once it
    // is loaded; a model that fails marks the stand failed.
    bool PoseV2StandModelReady(ObjectStand& stand, std::uint64_t now)
    {
        if (!stand.loadingSince)
            stand.loadingSince = now;
        const bool valid = STREAMING::IS_MODEL_VALID(stand.model) != FALSE;
        const bool loaded = valid && replay::pose::playback::RequestModel(stand.model);
        const replay::objcheck::StandModel step =
            replay::objcheck::StandModelStep(valid, loaded, now - stand.loadingSince, replay::playback::kModelTimeoutMs);
        if (step == replay::objcheck::StandModel::Fail)
        {
            stand.failed = true;
            stand.failWhy = valid ? "model_timeout" : "model_invalid";
            ++g_objectStandStats.failed;
        }
        return step == replay::objcheck::StandModel::Loaded;
    }

    void DeletePoseV2Stand(ObjectStand& stand)
    {
        if (stand.standIn && replay::entities::Exists(stand.standIn))
        {
            Object object = stand.standIn;
            ENTITY::SET_ENTITY_AS_MISSION_ENTITY(object, TRUE, TRUE);
            ENTITY::DELETE_ENTITY(&object);
        }
        g_objectStandObjects.erase(stand.standIn);
        stand.standIn = 0;
    }

    void HidePoseV2Original(ObjectStand& stand, Object original, const char* how)
    {
        stand.original = original;
        stand.originalWasVisible = ENTITY::IS_ENTITY_VISIBLE(original) != FALSE;
        ENTITY::SET_ENTITY_VISIBLE(original, FALSE);
        ENTITY::SET_ENTITY_COLLISION(original, FALSE, FALSE);
        ++g_objectStandStats.hidden;
        const std::uint64_t since = GetTickCount64() - g_objectStandsBegan;
        if (since > kStandLateMs && stand.originalWasVisible)
        {
            // The original was drawn until now next to the replay's copy.
            ++g_objectStandStats.hiddenLate;
            char extra[96];
            std::snprintf(extra, sizeof(extra), ",phase=play,why=hidden_late,how=%s,after_ms=%llu", how,
                static_cast<unsigned long long>(since));
            LogObjectCheck(replay::objcheck::Verdict::Duplicate, stand.prop.Valid() ? replay::entity::IdText(stand.prop) : "scenery",
                stand.model, 0, extra);
        }
    }

    void EndPoseV2WorldStandIns()
    {
        for (ObjectStand& stand : g_objectStands)
        {
            DeletePoseV2Stand(stand);
            if (stand.original && replay::entities::Exists(stand.original))
            {
                ENTITY::SET_ENTITY_VISIBLE(stand.original, stand.originalWasVisible ? TRUE : FALSE);
                ENTITY::SET_ENTITY_COLLISION(stand.original, TRUE, TRUE);
            }
        }
        if (!g_objectStands.empty())
        {
            std::uint32_t world = 0, never = 0, children = 0;
            for (const ObjectStand& stand : g_objectStands)
            {
                world += stand.scenery ? 0u : 1u;
                never += stand.original ? 0u : 1u;
                children += stand.parentStand >= 0 ? 1u : 0u;
            }
            Ctx().log << "POSE_V2_PLAYBACK_WORLD_OBJECTS,end=" << world << ",scenery=" << (g_objectStands.size() - world - children)
                      << ",children=" << children << ",child_attaches=" << g_objectStandStats.childAttaches
                      << ",child_on_prop=" << g_objectStandStats.childOnProp << ",child_by_frame=" << g_objectStandStats.childByFrame
                      << ",child_no_euler=" << g_objectStandStats.childNoEuler
                      << ",originals_hidden=" << g_objectStandStats.hidden << ",hidden_late=" << g_objectStandStats.hiddenLate
                      << ",reshown=" << g_objectStandStats.reshown << ",no_original=" << never
                      << ",stand_ins=" << g_objectStandStats.created << ",failed=" << g_objectStandStats.failed
                      << ",searches=" << g_objectStandStats.searches
                      << ",burn_tracks=" << Ctx().clip.sceneryBurnLevels.size() << ",burn_writes=" << g_objectStandStats.burnWrites
                      << ",handoff_ticks=" << kSceneryHandoffTicks << ",handoffs=" << g_objectStandStats.handoffs
                      << ",handoff_overlap_ticks=" << g_objectStandStats.handoffOverlapTicks
                      << '\n';
            // The map dressing's verdicts.
            std::uint32_t dressing = 0, waiting = 0;
            for (const ObjectStand& stand : g_objectStands)
            {
                dressing += stand.mapDressing ? 1u : 0u;
                waiting += stand.mapDressing && stand.mapVerdict == replay::objcheck::MapDressing::Wait ? 1u : 0u;
            }
            if (dressing)
            {
                Ctx().log << "POSE_V2_MAP_DRESSING,phase=play_end,entries=" << dressing << ",live=" << g_objectStandStats.mapLive
                          << ",built_ipl_off=" << g_objectStandStats.mapBuildIplOff
                          << ",built_missing=" << g_objectStandStats.mapBuildMissing
                          << ",built_changed=" << g_objectStandStats.mapBuildChanged << ",built_taken=" << g_objectStandStats.mapTaken
                          << ",undecided=" << waiting << ",decided_without_world=" << g_objectStandStats.mapDecidedWithoutWorld
                          << ",ipls_off=";
                for (const std::uint32_t ipl : g_objectStandStats.mapIplsOff)
                    Ctx().log << "0x" << std::hex << ipl << std::dec << '|';
                Ctx().log << '\n';
            }
        }
        g_objectStands.clear();
        g_objectStandObjects.clear();
    }

    // See replay_objects.h.
    void CollectPoseV2ReplayOwnObjects(std::vector<Object>& out)
    {
        out.clear();
        if (g_poseV2AttachHelper)
            out.push_back(g_poseV2AttachHelper);
        for (const ObjectStand& stand : g_objectStands)
        {
            if (stand.standIn)
                out.push_back(stand.standIn);
            if (stand.original)
                out.push_back(stand.original);
        }
        for (const PoseV2WheelObject& wheel : g_wheelObjects)
            out.push_back(wheel.object);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    void BeginPoseV2WorldStandIns()
    {
        EndPoseV2WorldStandIns();
        g_objectStandStats = {};
        g_lootStandLogs = 0;
        g_lateStandLogs = 0;
        g_objectStandsBegan = GetTickCount64();
        g_objectStandsNextSearch = 0;
        int hidden = 0;
        int invisible = 0;
        int loot = 0;
        int mapDressing = 0;
        for (const replay::WorldObjectOrigin& origin : Ctx().clip.worldObjects)
        {
            ObjectStand stand;
            stand.model = origin.model;
            for (int axis = 0; axis < 3; ++axis)
            {
                stand.position[axis] = origin.position[axis];
                stand.rotation[axis] = origin.rotation[axis];
            }
            stand.prop = origin.id;
            // The same session: the object recorded, wherever it is now.
            const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(origin.id.Packed());
            if (create && !create->weaponHash)
                stand.hatLook = create->appearance.assets; // the prop's recorded hat look
            const Object source = create && !Ctx().clip.fromDisk ? static_cast<Object>(create->sourceHandle) : 0;
            g_objectStands.push_back(stand);
            if (source && replay::entities::Exists(source) && !OwnedByPuppet(source) &&
                static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(source)) == origin.model)
            {
                HidePoseV2Original(g_objectStands.back(), source, "session_handle");
                ++hidden;
            }
        }
        for (const replay::SceneryObject& object : Ctx().clip.scenery)
        {
            ObjectStand stand;
            stand.model = object.model;
            for (int axis = 0; axis < 3; ++axis)
            {
                stand.position[axis] = object.position[axis];
                stand.rotation[axis] = object.rotation[axis];
            }
            stand.appearMs = object.appearMs;
            stand.goneMs = object.goneMs;
            stand.prop = object.prop;
            stand.scenery = true;
            stand.visibleWhenRecorded = object.visible != 0;
            stand.visibleFlipsMs = object.visibleFlipsMs;
            if (!stand.visibleWhenRecorded || !stand.visibleFlipsMs.empty())
                ++invisible;
            // A map dressing entry waits for its verdict
            // (UpdatePoseV2WorldStandIns) and is no loot.
            stand.mapDressing = object.rule == static_cast<std::uint8_t>(replay::objcheck::SceneryRule::MapDressing);
            stand.mapIpl = object.ipl;
            mapDressing += stand.mapDressing ? 1 : 0;
            // The loot recorded at its take (objcheck::IsLootEntry);
            // its stand-in and the original's hide are the scenery's.
            stand.loot = !stand.mapDressing && replay::objcheck::IsLootEntry(object.mission != 0, object.owner);
            loot += stand.loot ? 1 : 0;
            // Its burn level track (the index is the entry's).
            const std::size_t entry = static_cast<std::size_t>(&object - Ctx().clip.scenery.data());
            for (std::size_t n = 0; n < Ctx().clip.sceneryBurnLevels.size(); ++n)
                if (Ctx().clip.sceneryBurnLevels[n].id == entry && !Ctx().clip.sceneryBurnLevels[n].states.empty())
                    stand.burnTrack = static_cast<int>(n);
            stand.lookTrack = replay::objlook::SceneryTrackOf(static_cast<std::uint32_t>(entry));
            stand.hatLook = object.hatLook;
            g_objectStands.push_back(stand);
        }
        // The scenery's children, after the scenery stands (a scenery entry n
        // is the stand worldObjects + n).
        const std::size_t sceneryBase = Ctx().clip.worldObjects.size();
        std::uint32_t children = 0;
        for (const replay::SceneryChild& child : Ctx().clip.sceneryChildren)
        {
            if (child.parent >= Ctx().clip.scenery.size())
                continue;
            ObjectStand stand;
            stand.model = child.model;
            for (int axis = 0; axis < 3; ++axis)
            {
                stand.position[axis] = child.position[axis];
                stand.rotation[axis] = child.rotation[axis];
                stand.framePosition[axis] = child.framePosition[axis];
                stand.relativePosition[axis] = child.relativePosition[axis];
            }
            for (int axis = 0; axis < 4; ++axis)
            {
                stand.frameRotation[axis] = child.frameRotation[axis];
                stand.relativeRotation[axis] = child.relativeRotation[axis];
            }
            stand.appearMs = child.appearMs;
            stand.goneMs = child.goneMs;
            stand.scenery = true;
            stand.visibleWhenRecorded = child.visible != 0;
            stand.visibleFlipsMs = child.visibleFlipsMs;
            stand.parentStand = static_cast<int>(sceneryBase + child.parent);
            stand.hasFrame = child.hasFrame != 0;
            stand.frameBone = child.frameBone;
            g_objectStands.push_back(stand);
            ++children;
        }
        // A rope's own entity (rope_models.h) recorded as scenery or as a
        // world object by a clip older than the recording's filter:
        // its model never loads (ROPE_REINS, ROPE_THIN and ROPE_REINS_ROPE ran
        // out the model timeout on every open in the QA logs); the rope track
        // replays it. Never built, never awaited.
        std::uint32_t ropeStands = 0;
        for (ObjectStand& stand : g_objectStands)
            if (replay::ropemodels::IsRopeModel(stand.model))
            {
                stand.failed = true;
                stand.failWhy = "rope_model";
                ++ropeStands;
            }
        // hat_look.h: how each hat is built.
        std::uint32_t hatsDressed = 0;
        std::uint32_t hatsHidden = 0;
        for (std::size_t index = 0; index < g_objectStands.size(); ++index)
        {
            ObjectStand& stand = g_objectStands[index];
            if (stand.parentStand >= 0)
                continue;
            stand.hatRebuild = PoseV2HatRebuild(stand.model, stand.hatLook);
            hatsDressed += stand.hatRebuild == replay::hatlook::Rebuild::Dress ? 1u : 0u;
            if (stand.hatRebuild == replay::hatlook::Rebuild::Hide)
            {
                stand.hatHidden = true;
                ++hatsHidden;
                LogPoseV2HatHidden(stand.scenery ? "scenery" : "world_object",
                    stand.prop.Valid() ? replay::entity::IdText(stand.prop) : "stand=" + std::to_string(index), stand.model, 0);
            }
        }
        if (hatsDressed || hatsHidden)
            Ctx().log << "POSE_V2_HAT_LOOK,phase=play_begin,stands_dressed=" << hatsDressed << ",stands_hidden_bare=" << hatsHidden
                      << ",hat_models=" << g_hatModels.size() << '\n';
        if (!g_objectStands.empty())
            Ctx().log << "POSE_V2_PLAYBACK_WORLD_OBJECTS,begin=" << Ctx().clip.worldObjects.size()
                      << ",scenery=" << Ctx().clip.scenery.size() << ",children=" << children << ",originals_hidden=" << hidden
                      << ",scenery_hidden_or_flipping=" << invisible << ",loot=" << loot
                      << ",map_dressing=" << mapDressing << ",rope_models=" << ropeStands << '\n';
    }

    // One walk of the object list for every stand without its original.
    void SearchPoseV2Originals()
    {
        const std::uint64_t now = GetTickCount64();
        if (now < g_objectStandsNextSearch)
            return;
        g_objectStandsNextSearch = now + (now - g_objectStandsBegan < kStandFastSearchMs ? 500 : kStandSlowSearchMs);
        std::unordered_set<std::uint32_t> models;
        for (const ObjectStand& stand : g_objectStands)
        {
            // Map dressing left to the map or to a prop is never looked for;
            // one waiting looks for its original only until it is found (kept
            // in mapOriginal, not hidden).
            if (stand.mapDressing && stand.mapVerdict != replay::objcheck::MapDressing::Build)
            {
                if (stand.mapVerdict == replay::objcheck::MapDressing::Wait &&
                    (!stand.mapOriginal || !replay::entities::Exists(stand.mapOriginal)))
                    models.insert(stand.model);
                continue;
            }
            if (!stand.original || !replay::entities::Exists(stand.original))
                models.insert(stand.model);
            else if (ENTITY::IS_ENTITY_VISIBLE(stand.original))
            {
                // Someone drew it again (a script, the streaming): hidden again.
                ++g_objectStandStats.reshown;
                ENTITY::SET_ENTITY_VISIBLE(stand.original, FALSE);
                LogObjectCheck(replay::objcheck::Verdict::Duplicate,
                    stand.prop.Valid() ? replay::entity::IdText(stand.prop) : "scenery", stand.model, 0, ",phase=play,why=reshown");
            }
        }
        if (models.empty())
            return;
        ++g_objectStandStats.searches;
        std::unordered_set<Object> puppets;
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
            if (puppet.clone)
                puppets.insert(puppet.clone);
        static int objects[1024];
        const int count = (std::min)(ListObjects(objects, 1024), 1024);
        std::unordered_set<Object> taken;
        for (const ObjectStand& stand : g_objectStands)
        {
            if (stand.original)
                taken.insert(stand.original);
            if (stand.mapOriginal)
                taken.insert(stand.mapOriginal);
        }
        // objcheck::NearestOriginals: the closest object of the model wins, not
        // the first listed (a shelf's row of one model lost a neighbor next to
        // the taken item's stand-in).
        static std::vector<Object> candidates;
        static std::vector<replay::objcheck::OriginalPair> pairs;
        candidates.clear();
        pairs.clear();
        for (int slot = 0; slot < count; ++slot)
        {
            const Object object = objects[slot];
            if (!object || g_objectStandObjects.count(object) || puppets.count(object) || taken.count(object))
                continue;
            if (!replay::entities::Exists(object))
                continue;
            const std::uint32_t model = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object));
            if (!models.count(model))
                continue;
            float at[3]{};
            if (!replay::entities::Position(object, at))
                continue;
            for (std::size_t index = 0; index < g_objectStands.size(); ++index)
            {
                const ObjectStand& stand = g_objectStands[index];
                if (stand.model != model || (stand.original && replay::entities::Exists(stand.original)) ||
                    !replay::objcheck::Near(at, stand.position, replay::objcheck::kOriginalRadiusM))
                    continue;
                if (stand.mapDressing && (stand.mapVerdict == replay::objcheck::MapDressing::Live ||
                                             (stand.mapOriginal && replay::entities::Exists(stand.mapOriginal))))
                    continue;
                const float dx = at[0] - stand.position[0];
                const float dy = at[1] - stand.position[1];
                const float dz = at[2] - stand.position[2];
                pairs.push_back({index, candidates.size(), dx * dx + dy * dy + dz * dz});
            }
            candidates.push_back(object);
        }
        for (const auto& [index, candidate] : replay::objcheck::NearestOriginals(pairs))
        {
            ObjectStand& stand = g_objectStands[index];
            // A waiting map dressing stand only notes the map's original (the
            // verdict leaves it drawn).
            if (stand.mapDressing && stand.mapVerdict == replay::objcheck::MapDressing::Wait)
                stand.mapOriginal = candidates[candidate];
            else
                HidePoseV2Original(stand, candidates[candidate], "search");
        }
    }

    // objcheck::MapDressingVerdict a waiting map dressing stand's verdict, once
    // the clip's map state was looked at. False while it is not to be built
    // (wait, live).
    bool DecidePoseV2MapDressing(ObjectStand& stand, std::uint32_t timeMs, std::uint64_t now)
    {
        using replay::objcheck::MapDressing;
        if (!stand.mapDressing || stand.mapVerdict == MapDressing::Build)
            return true;
        if (stand.mapVerdict != MapDressing::Wait)
            return false;
        const std::uint64_t waited = now - g_objectStandsBegan;
        const bool worldLooked = replay::world::PlaybackApplied();
        if (!worldLooked && waited < kMapDressingWorldFallbackMs)
            return false;
        const bool taken = stand.prop.Valid();
        const bool changed = !stand.visibleWhenRecorded || !stand.visibleFlipsMs.empty() ||
            stand.goneMs != replay::objcheck::kNeverGone;
        // An entry of the always-on map (no IPL) counts as on: built only when missing.
        const bool iplActive = !stand.mapIpl || STREAMING::IS_IPL_ACTIVE_HASH(static_cast<Hash>(stand.mapIpl)) != FALSE;
        const bool found = stand.mapOriginal && replay::entities::Exists(stand.mapOriginal);
        stand.mapVerdict = replay::objcheck::MapDressingVerdict(taken, changed, iplActive, found, waited);
        if (stand.mapVerdict == MapDressing::Wait)
            return false;
        g_objectStandStats.mapDecidedWithoutWorld += worldLooked ? 0u : 1u;
        const char* why = "";
        switch (stand.mapVerdict)
        {
        case MapDressing::Live: ++g_objectStandStats.mapLive; why = "original_live"; break;
        default:
            if (taken)
            {
                ++g_objectStandStats.mapTaken;
                why = "taken";
            }
            else if (changed)
            {
                ++g_objectStandStats.mapBuildChanged;
                why = "visibility_changed";
            }
            else if (!iplActive)
            {
                ++g_objectStandStats.mapBuildIplOff;
                g_objectStandStats.mapIplsOff.insert(stand.mapIpl);
                why = "ipl_off";
            }
            else
            {
                ++g_objectStandStats.mapBuildMissing;
                why = "original_missing";
            }
            break;
        }
        if (stand.mapVerdict == MapDressing::Live)
            stand.mapOriginal = 0; // the map's: neither hidden nor given back
        else if (stand.mapOriginal && replay::entities::Exists(stand.mapOriginal) && !stand.original)
        {
            // Built: the original noted while waiting is hidden now (the
            // search skips noted originals, so it stayed drawn next to the
            // stand-in and the prop).
            HidePoseV2Original(stand, stand.mapOriginal, "map_dressing");
            stand.mapOriginal = 0;
        }
        if (g_objectStandStats.mapLogs < 32 && stand.mapVerdict == MapDressing::Build)
        {
            ++g_objectStandStats.mapLogs;
            char line[256];
            std::snprintf(line, sizeof(line),
                "POSE_V2_PLAYBACK_MAP_DRESSING,verdict=%s,why=%s,model=0x%x,pos=%.2f;%.2f;%.2f,ipl=0x%x,world_looked=%d,waited_ms=%llu,playhead_ms=%u\n",
                replay::objcheck::MapDressingText(stand.mapVerdict), why, stand.model, stand.position[0], stand.position[1],
                stand.position[2], stand.mapIpl, worldLooked ? 1 : 0, static_cast<unsigned long long>(waited), timeMs);
            Ctx().log << line;
        }
        return stand.mapVerdict == MapDressing::Build;
    }

    // A loot stand-in built, shown and hidden (the take), for the
    // bench and the QA logs; 64 lines per playback.
    void LogPoseV2LootStand(const ObjectStand& stand, const char* what, std::uint32_t timeMs)
    {
        if (!stand.loot || g_lootStandLogs >= 64)
            return;
        ++g_lootStandLogs;
        char line[256];
        std::snprintf(line, sizeof(line),
            "POSE_V2_PLAYBACK_LOOT,what=%s,model=0x%x,pos=%.2f;%.2f;%.2f,original=%d,flips=%zu,gone_ms=%d,playhead_ms=%u\n", what,
            stand.model, stand.position[0], stand.position[1], stand.position[2], stand.original ? 1 : 0,
            stand.visibleFlipsMs.size(), stand.goneMs == replay::objcheck::kNeverGone ? -1 : static_cast<int>(stand.goneMs), timeMs);
        Ctx().log << line;
    }

    // object_check.h ChooseChildHost: where a child hangs now, the
    // parent's prop once it took over (its puppet ready), else the parent's
    // stand-in while shown; 0 when the parent is not drawn.
    Object PoseV2ChildHost(const ObjectStand& parent, replay::objcheck::ChildHost& kind)
    {
        const bool standInShown = parent.standIn && parent.shown && replay::entities::Exists(parent.standIn);
        kind = replay::objcheck::ChooseChildHost(parent.propTookOver, parent.propClone != 0, standInShown);
        if (kind == replay::objcheck::ChildHost::Prop)
            return parent.propClone;
        return kind == replay::objcheck::ChildHost::StandIn ? parent.standIn : 0;
    }

    // A scenery child's object (kept in `standIn`), created within
    // its life while its parent is drawn, attached to the host and attached
    // again when the host changes (the prop taking over, a puppet rebuilt
    // after a scrub). Shown with its recorded visibility and only while the
    // host is visible.
    void UpdatePoseV2ChildStand(ObjectStand& stand, const ObjectStand& parent, std::uint32_t timeMs, std::uint32_t& creates,
        std::uint64_t now)
    {
        replay::objcheck::ChildHost kind = replay::objcheck::ChildHost::None;
        const Object host = PoseV2ChildHost(parent, kind);
        const bool hostVisible = host && (kind == replay::objcheck::ChildHost::StandIn || ENTITY::IS_ENTITY_VISIBLE(host));
        const bool wantShown = hostVisible && replay::objcheck::SceneryShown(stand.appearMs, stand.goneMs, false, 0, false, timeMs) &&
            replay::objcheck::VisibleAt(stand.visibleWhenRecorded, stand.visibleFlipsMs, timeMs);
        if (stand.standIn && !replay::entities::Exists(stand.standIn))
        {
            g_objectStandObjects.erase(stand.standIn);
            stand.standIn = 0;
            stand.host = 0;
        }
        if (!stand.standIn && wantShown && !stand.failed)
        {
            if (!PoseV2StandModelReady(stand, now))
                return;
            if (creates >= replay::objcheck::kSceneryCreatesPerTick)
                return;
            ++creates;
            const float* p = stand.position;
            stand.standIn = OBJECT::CREATE_OBJECT(stand.model, p[0], p[1], p[2], FALSE, FALSE, FALSE, FALSE, FALSE);
            if (!stand.standIn)
                return;
            ++g_objectStandStats.created;
            g_objectStandObjects.insert(stand.standIn);
            ENTITY::SET_ENTITY_AS_MISSION_ENTITY(stand.standIn, TRUE, TRUE);
            ENTITY::SET_ENTITY_COLLISION(stand.standIn, FALSE, FALSE);
            // Frozen where it stood until the attach (a failed Euler left it
            // falling, shown, collision off).
            ENTITY::FREEZE_ENTITY_POSITION(stand.standIn, TRUE);
            ENTITY::SET_ENTITY_ROTATION(stand.standIn, stand.rotation[0], stand.rotation[1], stand.rotation[2], 2, TRUE);
            stand.host = 0;
            stand.shown = true;
        }
        if (stand.failed && wantShown && !stand.missingLogged)
        {
            stand.missingLogged = true;
            LogObjectCheck(replay::objcheck::Verdict::SceneryMissing, "scenery", stand.model, timeMs,
                (std::string(",why=") + stand.failWhy + ",child=1").c_str());
        }
        if (stand.standIn && host)
        {
            const bool byFrame = replay::objcheck::AttachByFrame(kind, stand.hasFrame, parent.propPosed);
            if (host != stand.host || byFrame != stand.hostByFrame || !ENTITY::IS_ENTITY_ATTACHED_TO_ENTITY(stand.standIn, host))
            {
                const float* q = byFrame ? stand.frameRotation : stand.relativeRotation;
                const float* o = byFrame ? stand.framePosition : stand.relativePosition;
                Vector3 euler{};
                // The engine's own Euler for the quaternion (the weapon
                // attach's helper object, rotation order 2).
                if (PoseV2EngineEuler(replay::pose::Quat{q[0], q[1], q[2], q[3]}, stand.model, euler))
                {
                    ENTITY::FREEZE_ENTITY_POSITION(stand.standIn, FALSE); // as the puppet attach does
                    ENTITY::ATTACH_ENTITY_TO_ENTITY(stand.standIn, host, byFrame ? stand.frameBone : -1, o[0], o[1], o[2],
                        euler.x, euler.y, euler.z, FALSE, FALSE, FALSE, FALSE, kPoseV2AttachRotationOrder, TRUE, FALSE, FALSE);
                    stand.host = host;
                    stand.hostByFrame = byFrame;
                    ++g_objectStandStats.childAttaches;
                    g_objectStandStats.childOnProp += kind == replay::objcheck::ChildHost::Prop ? 1u : 0u;
                    g_objectStandStats.childByFrame += byFrame ? 1u : 0u;
                    if (g_objectStandStats.childAttaches <= 32)
                        Ctx().log << "POSE_V2_PLAYBACK_SCENERY_CHILD,model=0x" << std::hex << stand.model << ",parent_model=0x"
                                  << parent.model << std::dec << ",host=" << (kind == replay::objcheck::ChildHost::Prop ? "prop" : "stand_in")
                                  << ",by_frame=" << (byFrame ? 1 : 0) << ",bone=" << (byFrame ? stand.frameBone : -1)
                                  << ",playhead_ms=" << timeMs << '\n';
                }
                else
                    ++g_objectStandStats.childNoEuler;
            }
        }
        if (stand.standIn && stand.shown != wantShown)
        {
            stand.shown = wantShown;
            ENTITY::SET_ENTITY_VISIBLE(stand.standIn, stand.shown ? TRUE : FALSE);
        }
    }

    // The recorded burn level at the playhead on a scenery stand-in (the
    // replay's own object, deleted at the end: nothing to give back), written
    // into its effect block (burn::Write, the store _SET_OBJECT_BURN_LEVEL
    // makes) whenever its own differs, in both directions (scrub back lowers
    // it).
    void PlayPoseV2StandBurnLevel(ObjectStand& stand, std::uint32_t timeMs)
    {
        if (stand.burnTrack < 0 || !g_objectBurnLevel || !replay::burn::Enabled() ||
            static_cast<std::size_t>(stand.burnTrack) >= Ctx().clip.sceneryBurnLevels.size())
            return;
        const replay::BurnLevelTrack& track = Ctx().clip.sceneryBurnLevels[static_cast<std::size_t>(stand.burnTrack)];
        const float level = replay::BurnLevelAt(track, timeMs);
        const float wanted = std::isfinite(level) ? std::clamp(level, 0.0f, 1.0f) : 0.0f;
        const void* address = replay::entities::AddressOf(stand.standIn);
        replay::burn::State state;
        if (!replay::burn::Read(address, state) || !state.fireBlock || std::fabs(state.level - wanted) < 0.001f)
            return;
        const int blocks = replay::burn::Write(address, wanted);
        ++g_objectStandStats.burnWrites;
        if (g_objectStandStats.burnLogs < 32)
        {
            ++g_objectStandStats.burnLogs;
            Ctx().log << "POSE_V2_BURN_LEVEL,phase=play_scenery,id=scenery:" << track.id << ",model=0x" << std::hex << stand.model
                      << std::dec << ",from=" << state.level << ",to=" << wanted << ",blocks=" << blocks
                      << ",writes=" << g_objectStandStats.burnWrites << ",playhead_ms=" << timeMs << '\n';
        }
    }

    void UpdatePoseV2WorldStandIns(std::uint32_t timeMs)
    {
        if (g_objectStands.empty())
            return;
        REPLAY_PROFILE_ZONE("play.object_stands");
        SearchPoseV2Originals();
        std::uint32_t creates = 0;
        const std::uint64_t now = GetTickCount64();
        for (ObjectStand& stand : g_objectStands)
        {
            if (stand.parentStand >= 0)
            {
                // A child, after its parent (lower index) this tick.
                if (static_cast<std::size_t>(stand.parentStand) < g_objectStands.size())
                    UpdatePoseV2ChildStand(stand, g_objectStands[static_cast<std::size_t>(stand.parentStand)], timeMs,
                        creates, now);
                continue;
            }
            // Map dressing is built only when the map does not have it
            // (objcheck::MapDressingVerdict).
            if (!DecidePoseV2MapDressing(stand, timeMs, now))
                continue;
            // Before the prop took over: the stand-in; from then on the prop
            // (and nothing once the prop is gone: broken, removed), with the
            // stand-in kept until the prop puppet is built.
            const replay::entity::CreatePacket* create = stand.prop.Valid() ? Ctx().clip.FindCreate(stand.prop.Packed()) : nullptr;
            const std::uint32_t takeoverMs = create ? create->timeMs : 0;
            bool propReady = false;
            stand.propClone = 0; // for the children
            stand.propPosed = false;
            stand.propTookOver = create && timeMs >= takeoverMs;
            if (stand.prop.Valid())
                for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
                    if (puppet.packet.id == stand.prop && puppet.state == replay::playback::PuppetState::Ready &&
                        puppet.clone && replay::entities::Exists(puppet.clone))
                    {
                        propReady = true;
                        stand.propClone = puppet.clone;
                        stand.propPosed = puppet.overrideSlot >= 0;
                    }
            // The prop puppet is created, given its skeleton and armed in one
            // tick; hiding the stand-in in that tick would leave a frame with
            // neither drawn. The stand-in stays until the tick after the
            // prop's first published pose (objcheck::HandoffPropReady): both
            // stand in the same place and shape for that one frame.
            if (propReady)
            {
                ++stand.propReadyTicks;
                stand.propPosedTicks = stand.propPosed ? stand.propPosedTicks + 1 : 0;
                if (stand.propHasPose < 0)
                {
                    std::uint32_t firstMs = 0, lastMs = 0;
                    stand.propHasPose = replay::pose::capture::TrackSpan(stand.prop.Packed(), firstMs, lastMs) ? 1 : 0;
                }
            }
            else
            {
                stand.propReadyTicks = 0;
                stand.propPosedTicks = 0;
                stand.handoffLogged = false;
            }
            const bool handoffReady = replay::objcheck::HandoffPropReady(propReady, stand.propHasPose > 0, stand.propReadyTicks,
                stand.propPosedTicks, kSceneryHandoffTicks);
            if (propReady && !handoffReady && stand.shown && stand.standIn && create && timeMs >= takeoverMs)
                ++g_objectStandStats.handoffOverlapTicks;
            const bool wantShown = replay::objcheck::SceneryShown(stand.appearMs, stand.goneMs,
                stand.prop.Valid() && create, takeoverMs, handoffReady, timeMs) &&
                replay::objcheck::VisibleAt(stand.visibleWhenRecorded, stand.visibleFlipsMs, timeMs);
            if (handoffReady && !wantShown && stand.shown && stand.standIn && !stand.handoffLogged && create &&
                timeMs >= takeoverMs)
            {
                stand.handoffLogged = true;
                ++g_objectStandStats.handoffs;
                if (g_objectStandStats.handoffLogs < 32)
                {
                    ++g_objectStandStats.handoffLogs;
                    Ctx().log << "POSE_V2_SCENERY_HANDOFF,prop=" << replay::entity::IdText(stand.prop) << ",model=0x" << std::hex
                              << stand.model << std::dec << ",scenery=" << (stand.scenery ? 1 : 0)
                              << ",ready_ticks=" << stand.propReadyTicks << ",posed_ticks=" << stand.propPosedTicks
                              << ",has_pose=" << static_cast<int>(stand.propHasPose) << ",overlap=" << kSceneryHandoffTicks
                              << ",takeover_ms=" << takeoverMs << ",playhead_ms=" << timeMs << '\n';
                }
            }
            if (stand.standIn && !replay::entities::Exists(stand.standIn))
            {
                g_objectStandObjects.erase(stand.standIn);
                stand.standIn = 0;
            }
            if (!stand.standIn && wantShown && !stand.failed && !stand.hatHidden &&
                stand.hatRebuild == replay::hatlook::Rebuild::Dress)
            {
                // hat_look.h: the hat made with its recorded look by the hat
                // dresser (the bare model is the grey octagon); nothing drawn
                // when it cannot be made.
                bool dressFailed = false;
                const Object hat = DressPoseV2Hat(HatClientOfStand(static_cast<std::size_t>(&stand - g_objectStands.data())),
                    stand.model, stand.hatLook, stand.position, dressFailed, timeMs);
                if (dressFailed)
                    stand.hatHidden = true;
                if (hat)
                {
                    stand.standIn = hat;
                    ++g_objectStandStats.created;
                    g_objectStandObjects.insert(stand.standIn);
                    ENTITY::SET_ENTITY_AS_MISSION_ENTITY(stand.standIn, TRUE, TRUE);
                    ENTITY::SET_ENTITY_INVINCIBLE(stand.standIn, TRUE);
                    ENTITY::FREEZE_ENTITY_POSITION(stand.standIn, TRUE);
                    ENTITY::SET_ENTITY_COLLISION(stand.standIn, FALSE, FALSE);
                    const float* p = stand.position;
                    ENTITY::SET_ENTITY_COORDS_NO_OFFSET(stand.standIn, p[0], p[1], p[2], FALSE, FALSE, FALSE);
                    const float* r = stand.rotation;
                    ENTITY::SET_ENTITY_ROTATION(stand.standIn, r[0], r[1], r[2], 2, TRUE);
                    SuppressPickupLook(stand.standIn);
                    ENTITY::SET_ENTITY_VISIBLE(stand.standIn, TRUE);
                    stand.shown = true;
                    LogPoseV2LootStand(stand, "created", timeMs);
                }
            }
            if (!stand.standIn && wantShown && !stand.failed && !stand.hatHidden &&
                stand.hatRebuild != replay::hatlook::Rebuild::Dress)
            {
                if (!PoseV2StandModelReady(stand, now))
                    continue;
                if (creates >= replay::objcheck::kSceneryCreatesPerTick)
                    continue;
                ++creates;
                const float* p = stand.position;
                stand.standIn = OBJECT::CREATE_OBJECT(stand.model, p[0], p[1], p[2], FALSE, FALSE, FALSE, FALSE, FALSE);
                if (!stand.standIn)
                    continue;
                ++g_objectStandStats.created;
                g_objectStandObjects.insert(stand.standIn);
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(stand.standIn, TRUE, TRUE);
                ENTITY::FREEZE_ENTITY_POSITION(stand.standIn, TRUE);
                ENTITY::SET_ENTITY_COLLISION(stand.standIn, FALSE, FALSE);
                ENTITY::SET_ENTITY_COORDS_NO_OFFSET(stand.standIn, p[0], p[1], p[2], FALSE, FALSE, FALSE);
                const float* r = stand.rotation;
                ENTITY::SET_ENTITY_ROTATION(stand.standIn, r[0], r[1], r[2], 2, TRUE);
                stand.shown = true;
                LogPoseV2LootStand(stand, "created", timeMs);
                // A late scenery entry's stand-in (the
                // camp's prop set objects, a mission camp reached later).
                if (stand.scenery && stand.appearMs > 0 && g_lateStandLogs < 48)
                {
                    ++g_lateStandLogs;
                    char line[224];
                    std::snprintf(line, sizeof(line),
                        "POSE_V2_PLAYBACK_SCENERY_LATE,what=created,model=0x%x,pos=%.2f;%.2f;%.2f,appear_ms=%u,taken=%d,playhead_ms=%u\n",
                        stand.model, p[0], p[1], p[2], stand.appearMs, stand.prop.Valid() ? 1 : 0, timeMs);
                    Ctx().log << line;
                }
            }
            if (stand.failed && wantShown && !stand.missingLogged)
            {
                stand.missingLogged = true;
                LogObjectCheck(replay::objcheck::Verdict::SceneryMissing,
                    stand.prop.Valid() ? replay::entity::IdText(stand.prop) : "scenery", stand.model, timeMs,
                    (std::string(",why=") + stand.failWhy).c_str());
            }
            if (stand.standIn && stand.shown != wantShown)
            {
                stand.shown = wantShown;
                ENTITY::SET_ENTITY_VISIBLE(stand.standIn, stand.shown ? TRUE : FALSE);
                LogPoseV2LootStand(stand, stand.shown ? "shown" : "hidden", timeMs);
            }
            if (stand.standIn)
            {
                PlayPoseV2StandBurnLevel(stand, timeMs);
                // The stand-in's custom texture and tint (a scenery entry's own
                // track, else the prop's that takes a world object over).
                if (stand.scenery && stand.lookTrack >= 0)
                    replay::objlook::PlayStand(stand.standIn, true, static_cast<std::uint32_t>(stand.lookTrack), stand.model, timeMs);
                else if (!stand.scenery && stand.prop.Valid())
                    replay::objlook::PlayStand(stand.standIn, false, stand.prop.Packed(), stand.model, timeMs);
            }
        }
    }

    // object_check.h WeaponPlain: a weapon or prop of the clip
    // built as its bare model although its model is a weapon's in the same
    // clip (clips before the fix: the five finger fillet knife without its
    // grip), one line per object at the playback start.
    void WarnPoseV2WeaponPlain()
    {
        const std::map<std::uint32_t, std::uint32_t> weaponModels = replay::objcheck::WeaponModels(Ctx().clip.created);
        for (const replay::entity::CreatePacket& packet : Ctx().clip.created)
            if (replay::objcheck::WeaponPlain(packet, weaponModels))
                LogObjectCheck(replay::objcheck::Verdict::WeaponPlain, replay::entity::IdText(packet.id), packet.modelHash,
                    packet.timeMs, ",phase=play,why=bare_model");
    }

    void ReplayOwnObjects(std::vector<Object>& out) { CollectPoseV2ReplayOwnObjects(out); }
    std::uint32_t EntityOwnerOf(Entity entity) { return ReadEntityOwner(entity); }

    // A script restart with a playback open: the stand-ins and the attach
    // helper are the old world's handles (the next BeginPlayback would delete
    // them by number): forgotten, not deleted.
    std::uint32_t ForgetPlaybackAfterRestart()
    {
        std::uint32_t forgotten = static_cast<std::uint32_t>(g_objectStands.size()) + (g_poseV2AttachHelper ? 1u : 0u);
        g_objectStands.clear();
        g_objectStandObjects.clear();
        g_poseV2AttachHelper = 0;
        forgotten += g_hatDresser.helper ? 1u : 0u;
        EndPoseV2HatDresser(true);
        g_fragApplied.clear();
        g_fragPuppet.clear();
        g_fragPieceShaped.clear();
        g_fragPieceWaitUntil.clear();
        replay::objlook::ForgetAfterRestart();
        return forgotten;
    }

    void UpdateWeaponPuppets(std::uint32_t timeMs, std::uint32_t& creates) { UpdatePoseV2Weapons(timeMs, creates); }
    void PublishStrapOwned() { PublishPoseV2StrapOwned(); }
}
