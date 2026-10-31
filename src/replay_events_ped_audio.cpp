// replay_events_ped_audio.cpp: ped audio: surfaces, anim events and footsteps recorded, the entity query, ped audio state, foot events, horse audio evidence, own feet.
// Split from replay_events.cpp (moved code); shared internals in replay_events_internal.h.

#include "replay_events_internal.h"

namespace replay::events
{
    void RecordPoseV2Surfaces()
    {
        REPLAY_PROFILE_ZONE("rec.surfaces");
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        if (nowMs < g_poseV2SurfaceNextMs)
            return;
        g_poseV2SurfaceNextMs = nowMs + 200;
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (entity.vehicle || !replay::entities::Exists(entity.handle))
                continue;
            replay::blood::Surface surface;
            if (!replay::blood::ReadSurface(replay::entities::AddressOf(entity.handle), surface))
            {
                ++g_poseV2SurfacesUnreadable;
                continue;
            }
            const auto last = g_poseV2SurfaceLast.find(entity.id.Packed());
            if (last != g_poseV2SurfaceLast.end())
            {
                // Bit 2 ("got wetter this frame") flickers every read: not state.
                bool same = (last->second.wetFlags & ~4) == (surface.wetFlags & ~4) &&
                    std::memcmp(last->second.dirt, surface.dirt, sizeof(surface.dirt)) == 0;
                for (int n = 0; n < 4 && same; ++n)
                    same = std::fabs(last->second.wet[n] - surface.wet[n]) < 0.01f;
                if (same)
                    continue;
            }
            g_poseV2SurfaceLast[entity.id.Packed()] = surface;
            replay::entity::SurfaceEvent event;
            event.timeMs = nowMs;
            event.entity = entity.id;
            std::memcpy(event.wet, surface.wet, sizeof(event.wet));
            event.wetFlags = surface.wetFlags;
            std::memcpy(event.dirt, surface.dirt, sizeof(event.dirt));
            Ctx().clip.surfaces.Add(event);
            ++g_poseV2SurfacesRecorded;
            if (g_poseV2SurfacesRecorded <= 40)
                Ctx().log << "POSE_V2_SURFACE,phase=record,id=" << replay::entity::IdText(entity.id) << ",wet=" << surface.wet[0] << ";"
                    << surface.wet[1] << ";" << surface.wet[2] << ";" << surface.wet[3] << ",flags=" << int(surface.wetFlags)
                    << ",dirt=" << surface.dirt[0] << ";" << surface.dirt[1] << ";" << surface.dirt[2] << ",time_ms=" << nowMs << '\n';
        }
    }
    namespace
    {

    // Playback: the state at the playhead on every ped puppet, every frame
    // for the wetness (the puppet dries by itself), dirt when it changes.
    std::map<std::uint32_t, std::vector<std::size_t>> g_poseV2SurfaceIndex;
    }
    std::map<std::uint32_t, std::size_t> g_poseV2SurfaceApplied;
    bool g_poseV2SurfaceIndexBuilt = false;
    std::uint64_t g_poseV2SurfacesApplied = 0;
    std::uint64_t g_poseV2SurfacesFailed = 0;
    std::string g_poseV2SurfaceLastError;
    void PlayPoseV2Surfaces(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.surfaces");
        const auto& events = Ctx().clip.surfaces.Events();
        if (events.empty())
            return;
        if (!g_poseV2SurfaceIndexBuilt)
        {
            g_poseV2SurfaceIndex.clear();
            for (std::size_t n = 0; n < events.size(); ++n)
                g_poseV2SurfaceIndex[events[n].entity.Packed()].push_back(n);
            g_poseV2SurfaceIndexBuilt = true;
        }
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.state != replay::playback::PuppetState::Ready || !puppet.clone || !replay::entities::Exists(puppet.clone) ||
                !replay::entities::IsPed(puppet.clone))
                continue;
            const auto list = g_poseV2SurfaceIndex.find(puppet.packet.id.Packed());
            if (list == g_poseV2SurfaceIndex.end())
                continue;
            std::size_t chosen = list->second.front();
            for (const std::size_t index : list->second)
            {
                if (events[index].timeMs > timeMs)
                    break;
                chosen = index;
            }
            const replay::entity::SurfaceEvent& event = events[chosen];
            replay::blood::Surface surface;
            std::memcpy(surface.wet, event.wet, sizeof(surface.wet));
            surface.wetFlags = event.wetFlags;
            std::memcpy(surface.dirt, event.dirt, sizeof(surface.dirt));
            const auto applied = g_poseV2SurfaceApplied.find(puppet.packet.id.Packed());
            const bool dirt = applied == g_poseV2SurfaceApplied.end() || applied->second != chosen;
            std::string error;
            if (replay::blood::ApplySurface(replay::entities::AddressOf(puppet.clone), surface, dirt, error))
            {
                if (dirt)
                {
                    g_poseV2SurfaceApplied[puppet.packet.id.Packed()] = chosen;
                    ++g_poseV2SurfacesApplied;
                }
            }
            else
            {
                ++g_poseV2SurfacesFailed;
                g_poseV2SurfaceLastError = error;
            }
        }
    }

    // Every animation audio batch of a recorded ped, with what the engine
    // created inside the handler, and the animation audio track. An event goes
    // on the track only when its batch made no sound, footstep or speech line
    // (those have their own tracks; replaying the event too would play them
    // twice) and its hash is not one that starts speech or footsteps (a speech
    // event replayed would draw a new line with the engine's RNG).
    std::uint64_t g_poseV2AnimCursor = 0;
    std::uint64_t g_poseV2AnimLogged = 0;
    std::uint64_t g_poseV2AnimBatches = 0;
    std::uint64_t g_poseV2AnimRecorded = 0;
    std::uint64_t g_poseV2AnimSkippedDuplicate = 0;
    std::uint64_t g_poseV2AnimSkippedSpeechHash = 0;
    std::uint64_t g_poseV2AnimUnrecordedOwner = 0;
    std::uint64_t g_poseV2AnimTailNonZero = 0;
    std::uint64_t g_poseV2AnimRecordLines = 0;
    namespace
    {
    // Hashes that start speech or footsteps. Seeded from the anim event
    // registry (the 12 hashes of the footstep handler and 0xC0698860 of the
    // speech handler) and from observed single event batches
    // that made a speech line or a footstep (humans, horses and animals); a hash
    // seen doing so in this session is added (never cleared).
    constexpr std::uint32_t kPoseV2AnimSpeechOrStepHashes[] = {
        0x033E7BF8, 0x04F09FF5, 0x0656E748, 0x1358E9FF, 0x13A64E0C, 0x13A94A5A, 0x1438FBA1, 0x15F5E124, 0x1A9CC4FC,
        0x1AA223B2, 0x2AF8126D, 0x31A570AA, 0x33673DC0, 0x35CC810F, 0x3A43E26D, 0x3AEB215F, 0x3DC0D219, 0x434CBD8D,
        0x49AE0BA6, 0x4DB642E4, 0x4EA71DF9, 0x4F26F7F8, 0x56412BCF, 0x59290187, 0x5A412BB4, 0x5F1A0D8B, 0x644473A9,
        0x6701E18F, 0x6DE979FC, 0x6E2E2D9B, 0x6E910A08, 0x6F2142A0, 0x70568AE6, 0x71C21326, 0x7AC92F22, 0x80D4CAEB,
        0x83A7ADD0, 0x85864423, 0x85D37294, 0x8D9BDA25, 0x92F703D9, 0x9A3E6BF2, 0x9D021DB6, 0x9D669182, 0x9FC23E6E,
        0x9FE4C053, 0xA4257049, 0xA7478BC1, 0xA776A6A8, 0xAEEE893D, 0xB5241C2C, 0xB7E94D4F, 0xBCF84976, 0xBE629D62,
        0xBFCEB899, 0xC0698860, 0xC27F1EF0, 0xC32581E2, 0xC392AB61, 0xC6C2CA68, 0xCD447E7B, 0xCE5DECFB, 0xD6435FC2,
        0xD6F59DE8, 0xDFF2D689, 0xE6E71225, 0xEBEBFBAC, 0xEE14FDBE, 0xF1792BA9, 0xF37DAB85, 0xF537F454, 0xF67082ED,
        0xF8F9787C, 0xF9C094AB, 0xFB49D7B0, 0xFC8D24CA, 0xFD11320D};
    std::set<std::uint32_t> g_poseV2AnimSpeechOrStepLearned;

    bool PoseV2AnimSpeechOrStepHash(std::uint32_t hash)
    {
        return std::binary_search(std::begin(kPoseV2AnimSpeechOrStepHashes), std::end(kPoseV2AnimSpeechOrStepHashes), hash) ||
            g_poseV2AnimSpeechOrStepLearned.count(hash) != 0;
    }
    }

    void RecordPoseV2AnimEvents()
    {
        REPLAY_PROFILE_ZONE("rec.anim_audio");
        static std::vector<replay::audio::AnimAudioBatch> batches;
        replay::audio::CollectAnimBatchesSince(g_poseV2AnimCursor, batches);
        if (batches.empty())
            return;
        // The owners once per call.
        static std::vector<std::pair<std::uint64_t, replay::entity::ReplayId>> owners;
        owners.clear();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (entity.vehicle || !replay::entities::Exists(entity.handle))
                continue;
            owners.push_back({reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(entity.handle)), entity.id});
        }
        const std::uint64_t now = replay::audio::NowUs();
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        for (const replay::audio::AnimAudioBatch& batch : batches)
        {
            ++g_poseV2AnimBatches;
            g_poseV2AnimTailNonZero += batch.tailNonZero;
            // A single event batch that made speech or a footstep teaches its hash.
            if (batch.count == 1 && (batch.speech || batch.footsteps))
                g_poseV2AnimSpeechOrStepLearned.insert(batch.hash[0]);
            replay::entity::ReplayId id;
            for (const auto& [address, owner] : owners)
            {
                if (address == batch.owner)
                {
                    id = owner;
                    break;
                }
            }
            if (!id.Valid())
            {
                g_poseV2AnimUnrecordedOwner += batch.count;
                continue;
            }
            const std::uint64_t ageMs = now > batch.timeUs ? (now - batch.timeUs) / 1000 : 0;
            const std::uint32_t timeMs = replay::pose::capture::RecordMsAgo(ageMs, nowMs);
            const bool covered = batch.sounds || batch.footsteps || batch.speech;
            std::uint32_t kept = 0;
            for (std::uint32_t n = 0; n < batch.count && n < 4; ++n)
            {
                if (covered)
                {
                    ++g_poseV2AnimSkippedDuplicate;
                    continue;
                }
                if (PoseV2AnimSpeechOrStepHash(batch.hash[n]))
                {
                    ++g_poseV2AnimSkippedSpeechHash;
                    continue;
                }
                replay::entity::AnimAudioEvent event;
                event.timeMs = timeMs;
                event.entity = id;
                event.hash = batch.hash[n];
                event.extra = batch.extra[n];
                Ctx().clip.animAudio.Add(event);
                ++g_poseV2AnimRecorded;
                ++kept;
                if (replay::log::Verbose() && ++g_poseV2AnimRecordLines <= 60)
                    Ctx().log << "POSE_V2_ANIM_AUDIO,phase=record,ped=" << replay::entity::IdText(id) << ",hash=0x" << std::hex
                              << batch.hash[n] << std::dec << ",extra=" << batch.extra[n] << ",time_ms=" << timeMs << '\n';
            }
            // One line per animation event batch, also under the always-on buffer:
            // Debug=1 only.
            if (g_poseV2AnimLogged >= 4000 || !replay::log::Verbose())
                continue;
            ++g_poseV2AnimLogged;
            std::ostream& log = Ctx().log;
            log << "POSE_V2_ANIM_EVENT,phase=record,ped=" << replay::entity::IdText(id) << ",count=" << batch.count << ",events=";
            for (std::uint32_t n = 0; n < batch.count; ++n)
                log << (n ? "|" : "") << "0x" << std::hex << batch.hash[n] << std::dec << ":" << batch.extra[n];
            log << ",sounds=" << batch.sounds << ",footsteps=" << batch.footsteps << ",speech=" << batch.speech
                << ",time_ms=" << timeMs << ",track=" << kept << '\n';
        }
    }

    void LogPoseV2AnimAudioRecordTotal()
    {
        Ctx().log << "POSE_V2_ANIM_AUDIO,phase=record,kind=total,batches=" << g_poseV2AnimBatches
                  << ",recorded=" << g_poseV2AnimRecorded << ",skipped_duplicate=" << g_poseV2AnimSkippedDuplicate
                  << ",skipped_speech_hash=" << g_poseV2AnimSkippedSpeechHash
                  << ",unrecorded_owner=" << g_poseV2AnimUnrecordedOwner << ",tail_nonzero=" << g_poseV2AnimTailNonZero
                  << ",learned_hashes=" << g_poseV2AnimSpeechOrStepLearned.size()
                  << ",track=" << Ctx().clip.animAudio.Events().size() << '\n';
    }
    namespace
    {

    // The recorded step a pairing names (its index, checked by time, entity
    // and foot: a buffer trim shifts the indices).
    std::uint64_t PoseV2FootMaterialCheck(const replay::entity::FootstepEvent& step)
    {
        return (static_cast<std::uint64_t>(step.timeMs) << 32) ^ (static_cast<std::uint64_t>(step.entity.Packed()) << 4) ^
            step.foot;
    }

    void ApplyPoseV2FootMaterial(const replay::footmaterial::Pairer::Match& match)
    {
        // The foot's wetness rides with the same sample, valid material or not
        // (foot_material.h kWetnessBase).
        const std::uint8_t wetness = replay::footmaterial::EncodeWetness(match.material.wetness);
        if (!match.material.Valid())
        {
            ++g_poseV2FootMaterialInvalid;
            if (!wetness && !match.material.layersKnown) // the layers alone too
                return;
        }
        auto& track = Ctx().clip.footsteps;
        const std::size_t size = track.Events().size();
        if (!size)
            return;
        std::size_t index = static_cast<std::size_t>(match.token);
        std::size_t start = index < size ? index : size - 1;
        for (std::size_t n = 0; n <= start && n < 256; ++n)
        {
            replay::entity::FootstepEvent& step = track.At(start - n);
            if (PoseV2FootMaterialCheck(step) != match.check || step.materialRef || step.wetness || step.layersKnown)
                continue;
            if (match.material.Valid())
            {
                step.materialHash = match.material.hash;
                step.materialRef = match.material.ref;
                ++g_poseV2FootMaterialPaired;
            }
            step.wetness = wetness;
            // The foot's ground layer weights from the same sample (foot_material.h
            // kLayerBase).
            if (match.material.layersKnown)
            {
                step.layersKnown = 1;
                bool nonZero = false;
                for (std::uint8_t l = 0; l < replay::footmaterial::kLayers; ++l)
                {
                    step.layer[l] = replay::footmaterial::EncodeLayer(match.material.layers[l]);
                    nonZero = nonZero || step.layer[l] != 0;
                }
                ++g_poseV2FootLayersRecorded;
                if (nonZero)
                    ++g_poseV2FootLayersNonZero;
            }
            if (wetness)
            {
                ++g_poseV2FootWetnessRecorded;
                if (replay::footmaterial::WetStep(match.material.wetness))
                    ++g_poseV2FootWetRecorded;
            }
            return;
        }
    }
    }

    void RecordPoseV2Footsteps()
    {
        REPLAY_PROFILE_ZONE("rec.footsteps");
        static std::vector<replay::audio::FootstepCall> calls;
        replay::audio::CollectFootstepsSince(g_poseV2FootstepCursor, calls);
        static std::vector<replay::footmaterial::Sample> samples;
        replay::audio::CollectFootMaterialSamplesSince(g_poseV2FootMaterialCursor, samples);
        if (calls.empty() && samples.empty() && !g_poseV2FootMaterialPairer.PendingCount())
            return;
        static std::vector<std::pair<std::uint64_t, replay::entity::ReplayId>> owners;
        owners.clear();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (entity.vehicle || !replay::entities::Exists(entity.handle))
                continue;
            const std::uint64_t footAudio = replay::audio::FootstepAudio(replay::entities::AddressOf(entity.handle));
            if (footAudio)
                owners.push_back({footAudio, entity.id});
        }
        const std::uint64_t now = replay::audio::NowUs();
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        for (const replay::audio::FootstepCall& call : calls)
        {
            const auto owner = std::find_if(owners.begin(), owners.end(),
                [&](const auto& entry) { return entry.first == call.footAudio; });
            if (owner == owners.end())
            {
                ++g_poseV2FootstepsUnrecorded;
                continue;
            }
            const std::uint64_t ageMs = now > call.timeUs ? (now - call.timeUs) / 1000 : 0;
            replay::entity::FootstepEvent step;
            step.timeMs = replay::pose::capture::RecordMsAgo(ageMs, nowMs);
            step.entity = owner->second;
            step.eventId = call.eventId;
            step.arg3 = call.arg3;
            step.foot = call.foot;
            step.arg5 = call.arg5;
            step.flag = call.flag;
            step.arg7 = call.arg7;
            step.arg8 = call.arg8;
            // The foot's mix slot (foot_material.h FootMix).
            if (call.mix.known)
            {
                step.mixKnown = 1;
                step.mixVolume = call.mix.volume;
                step.mixHighPass = replay::footmaterial::EncodeMixFilter(call.mix.highPass);
                step.mixLowPass = replay::footmaterial::EncodeMixFilter(call.mix.lowPass);
            }
            // Base volume and impact (foot_material.h FootDrive).
            step.driveKnown = static_cast<std::uint8_t>((call.drive.baseKnown ? 1 : 0) | (call.drive.impactKnown ? 2 : 0));
            step.driveBase = call.drive.base;
            step.driveImpact = call.drive.impact;
            // The holder's audio LOD at the foot-down.
            step.audioLod = call.audioLod;
            if (call.audioLod != replay::footmaterial::kLodUnknown)
                ++g_poseV2FootLodRecorded;
            const std::size_t index = Ctx().clip.footsteps.Add(step);
            if (call.foot < replay::footmaterial::kFeet)
            {
                replay::footmaterial::Pairer::Pending pending;
                pending.timeUs = call.timeUs;
                pending.footAudio = call.footAudio;
                pending.foot = call.foot;
                pending.token = index;
                pending.check = PoseV2FootMaterialCheck(Ctx().clip.footsteps.Events()[index]);
                g_poseV2FootMaterialPairer.AddPending(pending);
            }
        }
        // The samples of the recorded feet only.
        for (const replay::footmaterial::Sample& sample : samples)
            if (std::any_of(owners.begin(), owners.end(), [&](const auto& entry) { return entry.first == sample.footAudio; }))
                g_poseV2FootMaterialPairer.AddSample(sample);
        static std::vector<replay::footmaterial::Pairer::Match> matches;
        g_poseV2FootMaterialPairer.Pair(matches);
        for (const replay::footmaterial::Pairer::Match& match : matches)
            ApplyPoseV2FootMaterial(match);
        g_poseV2FootMaterialLost += g_poseV2FootMaterialPairer.Expire(now, 500000);
    }

    // Fx evidence counters (POSE_V2_FX_EVENTS).
    std::uint64_t g_poseV2FxImpactNoShot = 0;
    std::uint64_t g_poseV2FxWorldOwner = 0;
    namespace
    {

    // The positional emitter sounds (e.g. the particle audio of a lit rag and
    // of a flying bottle), one line when a slot starts and one when it ends, per
    // phase (record, play), capped.
    struct PoseV2EmitterSeen
    {
        std::uint8_t index = 0;
        std::uint64_t key = 0;
        std::uint64_t desc = 0;
        std::uint32_t startMs = 0;
        bool sound = false;
    };
    std::vector<PoseV2EmitterSeen> g_poseV2EmittersSeen;
    std::uint32_t g_poseV2EmitterLogs = 0;
    const char* g_poseV2EmitterPhase = "";
    }
    void TracePoseV2Emitters(const char* phase, std::uint32_t timeMs)
    {
        if (std::strcmp(phase, g_poseV2EmitterPhase) != 0)
        {
            g_poseV2EmitterPhase = phase;
            g_poseV2EmittersSeen.clear();
            g_poseV2EmitterLogs = 0;
        }
        static std::vector<replay::audio::EmitterSlot> slots;
        std::string error;
        if (!replay::audio::SampleEmitterSlots(slots, error))
        {
            if (g_poseV2EmitterLogs == 0)
            {
                ++g_poseV2EmitterLogs;
                Ctx().log << "POSE_V2_EMITTER,phase=" << phase << ",error=" << error << '\n';
            }
            return;
        }
        for (const replay::audio::EmitterSlot& slot : slots)
        {
            auto seen = std::find_if(g_poseV2EmittersSeen.begin(), g_poseV2EmittersSeen.end(), [&](const PoseV2EmitterSeen& entry) {
                return entry.index == slot.index && entry.key == slot.key && entry.desc == slot.desc; });
            if (seen != g_poseV2EmittersSeen.end())
            {
                if (!seen->sound && slot.sound && g_poseV2EmitterLogs < 120)
                {
                    ++g_poseV2EmitterLogs;
                    Ctx().log << "POSE_V2_EMITTER,phase=" << phase << ",event=sound,slot=" << int(slot.index)
                        << ",time_ms=" << timeMs << '\n';
                }
                seen->sound = seen->sound || slot.sound;
                continue;
            }
            g_poseV2EmittersSeen.push_back({slot.index, slot.key, slot.desc, timeMs, slot.sound});
            if (g_poseV2EmitterLogs < 120)
            {
                ++g_poseV2EmitterLogs;
                Ctx().log << "POSE_V2_EMITTER,phase=" << phase << ",event=start,slot=" << int(slot.index) << std::hex
                    << ",hash=0x" << slot.hash << ",key=0x" << slot.key << ",desc=0x" << slot.desc << ",entity=0x" << slot.entity
                    << std::dec << ",sound=" << slot.sound << ",stopping=" << slot.stopping << ",pos=" << slot.position[0] << ";"
                    << slot.position[1] << ";" << slot.position[2] << ",time_ms=" << timeMs << '\n';
            }
        }
        for (std::size_t index = g_poseV2EmittersSeen.size(); index-- > 0;)
        {
            const PoseV2EmitterSeen& entry = g_poseV2EmittersSeen[index];
            const bool alive = std::any_of(slots.begin(), slots.end(), [&](const replay::audio::EmitterSlot& slot) {
                return entry.index == slot.index && entry.key == slot.key && entry.desc == slot.desc; });
            if (alive)
                continue;
            if (g_poseV2EmitterLogs < 120)
            {
                ++g_poseV2EmitterLogs;
                Ctx().log << "POSE_V2_EMITTER,phase=" << phase << ",event=end,slot=" << int(entry.index) << ",sound=" << entry.sound
                    << ",start_ms=" << entry.startMs << ",time_ms=" << timeMs << '\n';
            }
            g_poseV2EmittersSeen.erase(g_poseV2EmittersSeen.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }
    namespace
    {

    // The owner relations the dynamic entity sounds ask about
    // (entity_query.h). Playback: every rider the pose playback seats on a horse
    // puppet (the pairs of the mount audio above, replay::playback::MountOf)
    // answers the "mount" chains through that horse puppet's audio entity, as the
    // engine's own mount branch does for a mounted ped. Both phases: the
    // outermost queries of the owners into the log (ENTITY_QUERY, capped) and a
    // summary per head.
    struct EntityQueryHeadTally
    {
        std::uint32_t queries = 0;
        std::uint32_t nulls = 0;
        std::uint32_t forwarded = 0;
        std::uint32_t forwardedNull = 0;
    };
    std::map<std::uint32_t, EntityQueryHeadTally> g_entityQueryHeads;
    std::uint32_t g_entityQueryLines = 0;
    std::uint32_t g_entityQueryOtherThread = 0;
    std::uint64_t g_entityQueryDropped = 0;
    std::uint32_t g_entityQueryPairLogs = 0;
    std::vector<replay::entityquery::Pair> g_entityQueryPairs;
    // Playback: the clip's recorded answers per owner and chain, and the
    // table published to the detour (entity_query_track.h).
    std::vector<replay::entityquery::Timeline> g_entityQueryTimelines;
    std::vector<replay::entityquery::RecordedAnswer> g_entityQueryPublished;
    std::uint32_t g_entityQueryRecordedNotes = 0;
    constexpr std::uint32_t kEntityQueryLineCap = 400;
    constexpr std::uint32_t kEntityQueryPairLogCap = 20;
    }

    void BeginEntityQuery(bool recording)
    {
        g_entityQueryTimelines.clear();
        g_entityQueryPublished.clear();
        g_entityQueryRecordedNotes = 0;
        replay::entityquery::SetRecordedAnswers({});
        if (!recording)
        {
            const replay::entityquery::Track& track = Ctx().clip.entityQueries;
            g_entityQueryTimelines = replay::entityquery::Timelines(track);
            Ctx().log << "ENTITY_QUERY_BEGIN,phase=play,chains=" << track.chains.size() << ",answers=" << track.events.size()
                << ",timelines=" << g_entityQueryTimelines.size() << ",replay=" << (replay::entityquery::ReplayAnswersOn() ? 1 : 0)
                << ",relation=" << (replay::entityquery::RelationOn() ? 1 : 0) << '\n';
        }
        g_entityQueryHeads.clear();
        g_entityQueryLines = 0;
        g_entityQueryOtherThread = 0;
        g_entityQueryDropped = 0;
        g_entityQueryPairLogs = 0;
        g_entityQueryPairs.clear();
        replay::entityquery::SetPairs({});
        replay::entityquery::SetLogging(false);
        static std::vector<replay::entityquery::Query> stale;
        std::uint64_t dropped = 0;
        replay::entityquery::Drain(stale, dropped);
        replay::entityquery::ResetStats();
        // The ring feeds the log lines and, while recording, the clip's answers.
        replay::entityquery::SetLogging(replay::entityquery::Installed());
    }
    namespace
    {

    // The puppet whose ped audio entity is `holder` (playback), or the
    // recorded owner of that audio entity (recording).
    std::string EntityQueryOwnerText(bool recording, std::uint64_t holder,
        const std::unordered_map<std::uint64_t, std::uint32_t>& puppetHolders)
    {
        if (!holder)
            return "none";
        if (recording)
        {
            replay::census::Owner owner;
            if (replay::census::OwnerOf(holder, 0, owner) && owner.kind != replay::census::OwnerKind::None)
                return replay::entity::IdText(replay::entity::ReplayId::FromPacked(owner.id)) + (owner.player ? "p" : "");
            return "unrecorded";
        }
        const auto found = puppetHolders.find(holder);
        return found == puppetHolders.end() ? std::string("live")
            : replay::entity::IdText(replay::entity::ReplayId::FromPacked(found->second));
    }
    }

    void TickEntityQuery(bool recording, std::uint32_t timeMs)
    {
        if (!replay::entityquery::Installed())
            return;
        static std::unordered_map<std::uint64_t, std::uint32_t> puppetHolders;
        static std::unordered_map<std::uint32_t, std::uint64_t> ownerHolders;
        puppetHolders.clear();
        ownerHolders.clear();
        if (!recording)
        {
            // The pairs, every frame (a scrub recreates the puppets).
            static std::vector<replay::entityquery::Pair> pairs;
            pairs.clear();
            for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
            {
                if ((puppet.packet.kind != replay::entity::EntityKind::Ped &&
                        puppet.packet.kind != replay::entity::EntityKind::Horse) ||
                    puppet.state != replay::playback::PuppetState::Ready || !puppet.clone ||
                    !replay::entities::Exists(puppet.clone) || !replay::entities::IsPed(puppet.clone))
                    continue;
                const std::uint64_t holder = replay::audio::PedAudioHolder(replay::entities::AddressOf(puppet.clone));
                if (holder)
                {
                    puppetHolders[holder] = puppet.packet.id.Packed();
                    ownerHolders[puppet.packet.id.Packed()] = holder;
                }
                if (puppet.packet.kind != replay::entity::EntityKind::Ped)
                    continue;
                const std::uint32_t mountId = replay::playback::MountOf(puppet.packet.id.Packed());
                if (!mountId || !holder)
                    continue;
                const replay::playback::Puppet* mount = Ctx().puppets.Find(replay::entity::ReplayId::FromPacked(mountId));
                if (!mount || mount->state != replay::playback::PuppetState::Ready || !mount->clone ||
                    !replay::entities::Exists(mount->clone) || !replay::entities::IsPed(mount->clone))
                    continue;
                const std::uint64_t mountHolder = replay::audio::PedAudioHolder(replay::entities::AddressOf(mount->clone));
                if (!mountHolder)
                    continue;
                replay::entityquery::Pair pair;
                pair.riderHolder = static_cast<std::uintptr_t>(holder);
                pair.mountHolder = static_cast<std::uintptr_t>(mountHolder);
                pairs.push_back(pair);
            }
            const bool changed = pairs.size() != g_entityQueryPairs.size() ||
                !std::equal(pairs.begin(), pairs.end(), g_entityQueryPairs.begin(),
                    [](const replay::entityquery::Pair& a, const replay::entityquery::Pair& b) {
                        return a.riderHolder == b.riderHolder && a.mountHolder == b.mountHolder;
                    });
            if (changed)
            {
                g_entityQueryPairs = pairs;
                replay::entityquery::SetPairs(pairs);
                if (g_entityQueryPairLogs < kEntityQueryPairLogCap)
                {
                    ++g_entityQueryPairLogs;
                    std::string text;
                    for (const replay::entityquery::Pair& pair : pairs)
                        text += (text.empty() ? "" : "|") + EntityQueryOwnerText(false, pair.riderHolder, puppetHolders) + ">" +
                            EntityQueryOwnerText(false, pair.mountHolder, puppetHolders);
                    Ctx().log << "ENTITY_QUERY_PAIRS,phase=play,playhead_ms=" << timeMs << ",pairs=" << pairs.size()
                        << ",riders=" << (text.empty() ? "none" : text.c_str())
                        << ",relation=" << (replay::entityquery::RelationOn() ? 1 : 0) << '\n';
                }
            }
            // The recorded answers in force kLeadMs ahead, for the puppets
            // the clip has answers of (entity_query_track.h).
            static std::vector<replay::entityquery::RecordedAnswer> answers;
            answers.clear();
            if (replay::entityquery::ReplayAnswersOn())
            {
                const replay::entityquery::Track& track = Ctx().clip.entityQueries;
                for (const replay::entityquery::Timeline& line : g_entityQueryTimelines)
                {
                    const auto holder = ownerHolders.find(line.owner);
                    std::uint64_t answer = 0;
                    if (holder == ownerHolders.end() || line.chain >= track.chains.size() ||
                        !line.At(timeMs + replay::entityquery::kLeadMs, answer))
                        continue;
                    replay::entityquery::RecordedAnswer a;
                    a.holder = static_cast<std::uintptr_t>(holder->second);
                    a.hash = track.chains[line.chain].hash;
                    a.n = track.chains[line.chain].n;
                    a.answer = answer;
                    answers.push_back(a);
                }
            }
            const bool same = answers.size() == g_entityQueryPublished.size() &&
                std::equal(answers.begin(), answers.end(), g_entityQueryPublished.begin(),
                    [](const replay::entityquery::RecordedAnswer& a, const replay::entityquery::RecordedAnswer& b) {
                        return a.holder == b.holder && a.hash == b.hash && a.n == b.n && a.answer == b.answer;
                    });
            if (!same)
            {
                g_entityQueryPublished = answers;
                replay::entityquery::SetRecordedAnswers(answers);
            }
        }
        static std::vector<replay::entityquery::Query> queries;
        std::uint64_t dropped = 0;
        replay::entityquery::Drain(queries, dropped);
        g_entityQueryDropped += dropped;
        if (queries.empty())
            return;
        const std::uint32_t scriptThread = replay::entityquery::ScriptThread();
        const std::uint64_t nowUs = replay::clock::NowUs();
        const bool keep = recording && replay::entityquery::RecordAnswersOn() && !replay::pose::capture::IsClockPaused();
        for (const replay::entityquery::Query& q : queries)
        {
            if (keep && q.outermost && q.source == replay::entityquery::Source::Engine)
            {
                // Recording: the engine's answer for a recorded owner, at the
                // recording time of the query (it was drained a little later).
                replay::census::Owner recorded;
                if (replay::census::OwnerOf(q.holder, 0, recorded) && recorded.kind == replay::census::OwnerKind::Recorded)
                {
                    const std::uint64_t ageMs = nowUs > q.timeUs ? (nowUs - q.timeUs) / 1000 : 0;
                    const std::uint32_t at = timeMs > ageMs ? static_cast<std::uint32_t>(timeMs - ageMs) : 0u;
                    if (Ctx().clip.entityQueries.Note(at, recorded.id, q.n, q.chain, q.answer))
                        ++g_entityQueryRecordedNotes;
                }
            }
            std::string owner = EntityQueryOwnerText(recording, q.holder, puppetHolders);
            // Only the owners of the clip: recorded owners while recording,
            // puppets in playback (the live world's own queries are noise).
            if (owner == "live" || owner == "unrecorded" || owner == "none")
                continue;
            EntityQueryHeadTally& tally = g_entityQueryHeads[q.n ? q.chain[0] : 0];
            ++tally.queries;
            tally.nulls += q.nullAnswer ? 1 : 0;
            if (q.source == replay::entityquery::Source::Relation)
            {
                ++tally.forwarded;
                tally.forwardedNull += q.nullAnswer ? 1 : 0;
            }
            const bool other = scriptThread && q.thread != scriptThread;
            g_entityQueryOtherThread += other ? 1 : 0;
            if (g_entityQueryLines >= kEntityQueryLineCap)
                continue;
            ++g_entityQueryLines;
            char line[512]{};
            std::snprintf(line, sizeof(line),
                "ENTITY_QUERY,phase=%s,ms=%u,owner=%s,thread=%s,outermost=%d,src=%s,n=%u,chain=%s,answer=0x%llx,null=%d,"
                "engine_mount=%d,mount=%s",
                recording ? "record" : "play", timeMs, owner.c_str(), other ? "other" : "script", q.outermost ? 1 : 0,
                q.source == replay::entityquery::Source::Relation ? "relation"
                    : q.source == replay::entityquery::Source::Recorded ? "recorded" : "engine", q.n,
                replay::entityquery::ChainText(q).c_str(), static_cast<unsigned long long>(q.answer), q.nullAnswer ? 1 : 0,
                q.engineMount ? 1 : 0,
                q.mountHolder ? EntityQueryOwnerText(recording, q.mountHolder, puppetHolders).c_str() : "none");
            Ctx().log << line << '\n';
        }
    }

    void EndEntityQuery(const char* phase)
    {
        replay::entityquery::SetPairs({});
        g_entityQueryPairs.clear();
        replay::entityquery::SetRecordedAnswers({});
        g_entityQueryPublished.clear();
        const replay::entityquery::Stats stats = replay::entityquery::Snapshot();
        std::string heads;
        char part[96]{};
        for (const auto& [head, tally] : g_entityQueryHeads)
        {
            std::snprintf(part, sizeof(part), "%s0x%08x:%u/%u/%u/%u", heads.empty() ? "" : ";", head, tally.queries, tally.nulls,
                tally.forwarded, tally.forwardedNull);
            heads += part;
        }
        Ctx().log << "ENTITY_QUERY_SUMMARY,phase=" << phase << ",hook=" << (replay::entityquery::Installed() ? 1 : 0)
            << ",relation=" << (replay::entityquery::RelationOn() ? 1 : 0) << ",calls=" << stats.calls
            << ",outermost=" << stats.outermost << ",mount_head=" << stats.mountHead << ",forwarded=" << stats.forwarded
            << ",forwarded_null=" << stats.forwardedNull << ",engine_null=" << stats.engineNull
            << ",recorded=" << stats.recorded << ",recorded_null=" << stats.recordedNull
            << ",clip_chains=" << Ctx().clip.entityQueries.chains.size() << ",clip_answers=" << Ctx().clip.entityQueries.events.size()
            << ",noted=" << g_entityQueryRecordedNotes << ",dropped_long=" << Ctx().clip.entityQueries.droppedLong
            << ",dropped_full=" << Ctx().clip.entityQueries.droppedFull
            << ",other_thread=" << g_entityQueryOtherThread << ",dropped=" << g_entityQueryDropped
            << ",lines=" << g_entityQueryLines << ",heads(queries/null/forwarded/forwarded_null)="
            << (heads.empty() ? "none" : heads.c_str()) << '\n';
        replay::entityquery::SetLogging(false);
    }
    namespace
    {
    bool g_pedAudioStateTried = false;
    std::string g_pedAudioStateError;
    struct PedAudioVariantSeen
    {
        replay::pedaudiostate::Decision decision = replay::pedaudiostate::Decision::NoTarget;
        replay::pedaudiostate::Variants current;
        bool logged = false;
    };
    std::unordered_map<std::uint64_t, PedAudioVariantSeen> g_pedAudioVariantSeen; // by holder
    struct PedAudioVariantTally
    {
        std::uint32_t seen = 0;
        std::uint32_t readFailed = 0;
        std::uint32_t same = 0;
        std::uint32_t noTarget = 0;
        std::uint32_t noResource = 0;
        std::uint32_t implausible = 0;
        std::uint32_t noted = 0;
        // Record side: why a recorded ped's draw was not noted.
        std::uint32_t recReadFailed = 0;
        std::uint32_t recUndrawn = 0;
        std::uint32_t recKnown = 0; // already noted for that owner
        std::uint32_t recLogged = 0;
    } g_pedAudioVariantTally;
    // The in-session reference of the play phase (the last recording's
    // blocks per owner, never saved) and the last block logged per owner.
    std::vector<replay::pedaudiostate::BlockHistory> g_pedVarsRecorded;
    std::unordered_map<std::uint32_t, replay::pedaudiostate::Block> g_pedVarsLast;
    std::unordered_map<std::uint32_t, std::string> g_pedVarsLastDiff;
    std::vector<replay::pedaudiostate::Range> g_pedVarsRanges;
    std::uint32_t g_pedVarsLines = 0;
    std::uint32_t g_pedVarsDiffLines = 0;
    std::uint32_t g_pedVarsCompared = 0;
    std::uint32_t g_pedVarsDiffering = 0;
    std::uint32_t g_pedAudioVariantLines = 0;
    std::uint64_t g_pedVarsNextScanUs = 0;
    constexpr std::uint32_t kPedVarsLineCap = 300;
    }

    // The speed in each puppet's PED_VARIABLES block (ped_speed_core.h). The
    // engine computes it from the ped's movement component, which a frozen
    // puppet keeps at 0: every sound of a puppet would read speed 0 where the
    // recording read the ped's. The recording keeps each owner's value (PSPD);
    // the playback writes it into the puppet's block after the
    // engine's update (PedSpeedReplay, detour on the block update), from the section
    // or, for a clip without it, from the puppet's recorded motion. A replay
    // editor usually gives each ped's recorded velocity to its audio entity for
    // the same reason.
    bool g_pedSpeedReplay = true; // PedSpeedReplay
    namespace
    {
    bool g_pedSpeedInstallFailed = false;
    std::string g_pedSpeedError;
    std::unordered_map<std::uint32_t, replay::pedspeed::MotionEstimator> g_pedSpeedMotion; // by owner
    std::unordered_map<std::uint32_t, std::uint32_t> g_pedSpeedLoggedBlock;              // owner -> clip block
    }
    std::vector<replay::pedaudiostate::SpeedTarget> g_pedSpeedTargets;
    namespace
    {
    replay::pedspeed::Totals g_pedSpeedTotals;
    replay::pedaudiostate::SpeedCounts g_pedSpeedBase;
    std::uint32_t g_pedSpeedNoted = 0;
    std::uint32_t g_pedSpeedRefused = 0;
    }
    bool g_pedSpeedPublished = false;
    namespace
    {
    constexpr std::uint32_t kPedAudioVariantLineCap = 120;
    constexpr std::uint64_t kPedVarsScanUs = 250000;
    }

    // The movement component's own speed (C +0x5DC) on the puppets, from the
    // same target as the block speed (MotionSpeedReplay, detour on the speed source),
    // and the component's state as evidence (MOTION_STATE).
    bool g_motionSpeedReplay = true; // MotionSpeedReplay
    namespace
    {
    bool g_motionSpeedInstallFailed = false;
    std::string g_motionSpeedError;
    }
    std::vector<replay::pedaudiostate::MotionTarget> g_motionTargets;
    bool g_motionPublished = false;
    namespace
    {
    struct MotionTally
    {
        std::uint64_t puppetTicks = 0; // puppet x tick in normal play with a target
        std::uint32_t lines = 0;
        std::uint32_t checked = 0;     // lines with the component read back
        std::uint32_t agree = 0;       // ... within 0.05 m/s of the target
        std::uint32_t gaitMoving = 0;  // ... with a gait other than 0
    } g_motionTally;
    replay::pedaudiostate::MotionCounts g_motionBase;
    std::unordered_map<std::uint32_t, std::uint32_t> g_motionLoggedBlock; // owner -> 5 s block
    std::unordered_map<std::uint32_t, std::uint32_t> g_motionStateSlot;   // owner -> 1 s slot
    std::unordered_map<std::uint32_t, bool> g_motionStateHorse;           // owner -> a horse (record, cached)
    std::uint32_t g_motionStateLines = 0;
    constexpr std::uint32_t kMotionSpeedLineCap = 240;
    }

    // The secondary steps (the foot audio's second queue, foot_event_core.h),
    // recorded as events (FSEV) and queued again on the puppets
    // (FootEventReplay), with their evidence per owner and 5 s.
    bool g_footEventReplay = true; // FootEventReplay
    // foot_movement_core.h
    bool g_footMovementReplay = true;  // FootMovementReplay
    namespace
    {
    std::uint64_t g_footMovementRecorded = 0;
    std::uint64_t g_footMovementRejected = 0;
    std::uint64_t g_footMovementPublished = 0;
    replay::audio::MovementReplayStats g_footMovementReplayBase;
    }
    namespace
    {
    std::uint64_t g_footEventCursor = 0;
    replay::footevent::OwnerWindows g_footEventWindows;
    replay::footevent::Totals g_footEventTotals;
    }
    replay::entity::ForwardEventCursor g_footEventCursorPlay;
    bool g_footEventRefusing = false;
    namespace
    {
    const char* g_footEventPhase = "rec";
    // The movement volume's inputs per owner and 5 s (MOVEMENT_TERM_DETAIL).
    std::uint64_t g_movementDetailCursor = 0;
    std::map<std::uint32_t, std::pair<std::uint32_t, replay::footevent::MovementWindow>> g_movementWindows;
    std::uint32_t g_movementLines = 0;

    // Once, on the script thread: the signatures, and the read probe when asked.
    void InitPedAudioState()
    {
        if (g_pedAudioStateTried)
            return;
        g_pedAudioStateTried = true;
        replay::pedaudiostate::Init(g_pedAudioStateError);
        Ctx().log << "PED_AUDIO_STATE_INIT,variants=" << (replay::pedaudiostate::VariantsReady() ? 1 : 0)
            << ",blocks=" << (replay::pedaudiostate::BlocksReady() ? 1 : 0)
            << ",error=" << (g_pedAudioStateError.empty() ? "none" : g_pedAudioStateError.c_str()) << '\n';
    }
    }

    void BeginPedAudioState(bool recording)
    {
        InitPedAudioState();
        g_pedAudioVariantSeen.clear();
        g_pedAudioVariantTally = PedAudioVariantTally{};
        g_pedVarsLast.clear();
        g_pedVarsLastDiff.clear();
        g_pedVarsRanges.clear();
        g_pedVarsLines = 0;
        g_pedVarsDiffLines = 0;
        g_pedVarsCompared = 0;
        g_pedVarsDiffering = 0;
        g_pedAudioVariantLines = 0;
        g_pedVarsNextScanUs = 0;
        if (recording)
            g_pedVarsRecorded.clear(); // a new reference for the next playback
        // The puppets' speed.
        g_pedSpeedMotion.clear();
        g_pedSpeedLoggedBlock.clear();
        g_pedSpeedTargets.clear();
        g_pedSpeedTotals = replay::pedspeed::Totals{};
        g_pedSpeedNoted = 0;
        g_pedSpeedRefused = 0;
        g_pedSpeedPublished = false;
        replay::pedaudiostate::SetSpeedTargets({});
        if (!recording && g_pedSpeedReplay && !g_pedSpeedInstallFailed && !replay::pedaudiostate::SpeedOverrideInstalled())
        {
            g_pedSpeedError.clear();
            if (!replay::pedaudiostate::BlocksReady())
                g_pedSpeedError = "blocks_not_ready";
            else if (!replay::pedaudiostate::InstallSpeedOverride(g_pedSpeedError))
                g_pedSpeedInstallFailed = true; // a signature mismatch does not get better
        }
        g_pedSpeedBase = replay::pedaudiostate::SpeedSnapshot();
        // The movement component's speed.
        g_motionTargets.clear();
        g_motionPublished = false;
        g_motionTally = MotionTally{};
        g_motionLoggedBlock.clear();
        g_motionStateSlot.clear();
        g_motionStateHorse.clear();
        g_motionStateLines = 0;
        replay::pedaudiostate::SetMotionTargets({});
        if (!recording && g_motionSpeedReplay && !g_motionSpeedInstallFailed && !replay::pedaudiostate::MotionSpeedInstalled())
        {
            g_motionSpeedError.clear();
            if (!replay::pedaudiostate::InstallMotionSpeed(g_motionSpeedError))
                g_motionSpeedInstallFailed = true; // a signature mismatch does not get better
        }
        g_motionBase = replay::pedaudiostate::MotionSnapshot();
        if (!recording)
            Ctx().log << "MOTION_SPEED_BEGIN,phase=play,enabled=" << (g_motionSpeedReplay ? 1 : 0)
                << ",installed=" << (replay::pedaudiostate::MotionSpeedInstalled() ? 1 : 0)
                << ",error=" << (g_motionSpeedError.empty() ? "none" : g_motionSpeedError.c_str())
                << ",section=" << (Ctx().clip.pedSpeed.Empty() ? "none" : "PSPD") << '\n';
        if (!recording)
            Ctx().log << "PED_SPEED_BEGIN,phase=play,enabled=" << (g_pedSpeedReplay ? 1 : 0)
                << ",installed=" << (replay::pedaudiostate::SpeedOverrideInstalled() ? 1 : 0)
                << ",error=" << (g_pedSpeedError.empty() ? "none" : g_pedSpeedError.c_str())
                << ",section=" << (Ctx().clip.pedSpeed.Empty() ? "none" : "PSPD")
                << ",rec_owners=" << Ctx().clip.pedSpeed.series.size() << ",rec_points=" << Ctx().clip.pedSpeed.Points() << '\n';
        replay::pedaudiostate::SetNamedRanges({});
        Ctx().log << "PED_AUDIO_STATE_BEGIN,phase=" << (recording ? "record" : "play")
            << ",clip_variants=" << Ctx().clip.foleyVariants.rows.size()
            << ",reference_owners=" << g_pedVarsRecorded.size() << '\n';
    }
    namespace
    {

    replay::pedaudiostate::BlockHistory* PedVarsHistory(std::uint32_t owner)
    {
        for (replay::pedaudiostate::BlockHistory& history : g_pedVarsRecorded)
            if (history.owner == owner)
                return &history;
        return nullptr;
    }

    // One owner's block: logged when it changed, kept as the reference while
    // recording, compared with the reference in playback, published to the
    // read probe.
    void ScanPedVarsOwner(bool recording, std::uint32_t timeMs, std::uint32_t owner, bool player, std::uint32_t model,
        std::uint64_t holder)
    {
        replay::pedaudiostate::Block block;
        if (!replay::pedaudiostate::ReadHolderBlock(static_cast<std::uintptr_t>(holder), block))
            return;
        replay::pedaudiostate::Range range;
        range.values = block.values;
        range.count = block.count;
        range.owner = owner;
        for (std::uint32_t n = 0; n < block.count && n < replay::pedaudiostate::kMaxBlockVariables; ++n)
            range.names[n] = block.names[n];
        g_pedVarsRanges.push_back(range);
        // The owner's speed, 4 times a second (PSPD).
        if (recording)
        {
            float speed = 0.0f;
            if (replay::pedaudiostate::ValueOf(block, replay::pedspeed::kSpeedVariable, speed) &&
                Ctx().clip.pedSpeed.Note(owner, timeMs, speed))
                ++g_pedSpeedNoted;
            else
                ++g_pedSpeedRefused;
        }
        const std::string id = replay::entity::IdText(replay::entity::ReplayId::FromPacked(owner));
        if (recording)
        {
            replay::pedaudiostate::BlockHistory* history = PedVarsHistory(owner);
            if (!history && g_pedVarsRecorded.size() < 256)
            {
                g_pedVarsRecorded.push_back({});
                history = &g_pedVarsRecorded.back();
                history->owner = owner;
                history->model = model;
            }
            if (history)
                history->Note(timeMs, block);
        }
        const auto last = g_pedVarsLast.find(owner);
        const bool changed = last == g_pedVarsLast.end() || replay::pedaudiostate::BlockChanged(last->second, block);
        if (changed)
        {
            g_pedVarsLast[owner] = block;
            if (g_pedVarsLines < kPedVarsLineCap)
            {
                ++g_pedVarsLines;
                Ctx().log << "PED_VARS,phase=" << (recording ? "record" : "play") << ",ms=" << timeMs << ",owner=" << id
                    << ",player=" << (player ? 1 : 0) << ",block=" << block.index << ",count=" << block.count
                    << ",vars=" << replay::pedaudiostate::BlockText(block) << '\n';
            }
        }
        if (recording)
            return;
        const replay::pedaudiostate::BlockHistory* history = PedVarsHistory(owner);
        const replay::pedaudiostate::Block* reference = history && history->model == model ? history->At(timeMs) : nullptr;
        if (!reference)
            return;
        ++g_pedVarsCompared;
        std::uint32_t differing = 0;
        const std::string diff = replay::pedaudiostate::DiffText(*reference, block, differing);
        g_pedVarsDiffering += differing ? 1 : 0;
        std::string& lastDiff = g_pedVarsLastDiff[owner];
        if (diff == lastDiff)
            return;
        lastDiff = diff;
        if (g_pedVarsDiffLines < kPedVarsLineCap)
        {
            ++g_pedVarsDiffLines;
            Ctx().log << "PED_VARS_DIFF,phase=play,ms=" << timeMs << ",owner=" << id << ",player=" << (player ? 1 : 0)
                << ",reference=session,differing=" << differing << ",diff(rec>play)=" << diff << '\n';
        }
    }

    // A puppet's foley variants: the target and the decision (evidence; never
    // written).
    void TickPuppetFoleyVariants(const replay::playback::Puppet& puppet, std::uint64_t holder, bool recordedPlayer,
        const replay::pedaudiostate::Variants& livePlayer, std::uint32_t timeMs)
    {
        using namespace replay::pedaudiostate;
        const std::uint32_t owner = puppet.packet.id.Packed();
        Variants target;
        const TargetSource source = TargetFor(Ctx().clip.foleyVariants, owner, recordedPlayer, livePlayer, target);
        Variants current;
        bool resourceHeld = false;
        PedAudioVariantSeen& seen = g_pedAudioVariantSeen[holder];
        const bool first = !seen.logged;
        if (!ReadVariants(static_cast<std::uintptr_t>(holder), current, resourceHeld))
        {
            if (first)
            {
                ++g_pedAudioVariantTally.seen;
                ++g_pedAudioVariantTally.readFailed;
                seen.logged = true;
            }
            return;
        }
        const Decision decision = source == TargetSource::None ? Decision::NoTarget : Decide(current, target, resourceHeld);
        if (!first && decision == seen.decision && current == seen.current)
            return;
        if (first)
            ++g_pedAudioVariantTally.seen;
        switch (decision)
        {
        case Decision::Same: g_pedAudioVariantTally.same += first ? 1 : 0; break;
        case Decision::Write: break;
        case Decision::NoTarget: g_pedAudioVariantTally.noTarget += first ? 1 : 0; break;
        case Decision::Implausible: ++g_pedAudioVariantTally.implausible; break;
        case Decision::NoResource: ++g_pedAudioVariantTally.noResource; break;
        }
        seen.logged = true;
        seen.decision = decision;
        seen.current = current;
        if (g_pedAudioVariantLines >= kPedAudioVariantLineCap)
            return;
        ++g_pedAudioVariantLines;
        Ctx().log << "FOLEY_VARIANT,phase=play,ms=" << timeMs << ",owner=" << replay::entity::IdText(puppet.packet.id)
            << ",player=" << (recordedPlayer ? 1 : 0) << ",source=" << TargetSourceName(source) << ",puppet=" << current.a
            << '/' << current.b << ",target=" << target.a << '/' << target.b << ",resource=" << (resourceHeld ? 1 : 0)
            << ",decision=" << DecisionName(decision) << '\n';
    }

    // One puppet's speed target for this tick (normal play only) and, once
    // per kBlockMs of clip time, its evidence. The same target also goes to the
    // movement component (MotionSpeedReplay), with its own line per 5 s.
    void TickPuppetMotion(const replay::playback::Puppet& puppet, const replay::pedspeed::Target& target, std::uint32_t timeMs)
    {
        using namespace replay::pedspeed;
        const std::uintptr_t ped = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(puppet.clone));
        if (target.source != Source::None && ped && g_motionTargets.size() < replay::pedaudiostate::kMaxMotionTargets)
        {
            g_motionTargets.push_back({ped, target.speed});
            ++g_motionTally.puppetTicks;
        }
        const std::uint32_t owner = puppet.packet.id.Packed();
        const std::uint32_t block = timeMs / kBlockMs;
        auto logged = g_motionLoggedBlock.find(owner);
        if (logged != g_motionLoggedBlock.end() && logged->second == block)
            return;
        g_motionLoggedBlock[owner] = block;
        float raw = 0.0f;
        const bool rawKnown = replay::pedaudiostate::EngineMotionOf(ped, raw);
        replay::footevent::MotionState state;
        const bool read = replay::audio::ReadMotionState(reinterpret_cast<void*>(ped), state);
        if (read && target.source != Source::None)
        {
            ++g_motionTally.checked;
            g_motionTally.agree += std::fabs(state.speed - target.speed) <= 0.05f ? 1u : 0u;
            g_motionTally.gaitMoving += state.gait != 0 ? 1u : 0u;
        }
        if (g_motionTally.lines >= kMotionSpeedLineCap)
            return;
        ++g_motionTally.lines;
        char text[400];
        std::snprintf(text, sizeof(text),
            "MOTION_SPEED_REPLAY,clip_ms=%u,owner=%s,kind=%s,source=%s,published=%d,target=%s,raw_engine=%s,speed5dc=%s,gait=%d,"
            "gait_loaded=%d",
            timeMs, replay::entity::IdText(puppet.packet.id).c_str(),
            puppet.packet.kind == replay::entity::EntityKind::Horse ? "horse" : "ped", SourceName(target.source),
            g_motionPublished ? 1 : 0, Fmt2(target.source != Source::None, target.speed).c_str(), Fmt2(rawKnown, raw).c_str(),
            Fmt2(read, state.speed).c_str(), read ? state.gait : -1, read ? state.gaitLoaded : -1);
        Ctx().log << text << '\n';
    }

    void TickPuppetSpeed(const replay::playback::Puppet& puppet, std::uint64_t holder, std::uint32_t timeMs, bool normalPlay,
        bool speedOn, bool motionOn)
    {
        using namespace replay::pedspeed;
        const std::uint32_t owner = puppet.packet.id.Packed();
        MotionEstimator& motion = g_pedSpeedMotion[owner];
        if (!normalPlay)
        {
            motion.Reset(); // a pause, scrub or seek: the motion starts over
            return;
        }
        const Vector3 at = ENTITY::GET_ENTITY_COORDS(puppet.clone, TRUE, FALSE);
        motion.Note(timeMs, at.x, at.y, at.z);
        const Target target = Choose(Ctx().clip.pedSpeed, owner, timeMs, motion);
        if (motionOn)
            TickPuppetMotion(puppet, target, timeMs);
        if (!speedOn)
            return;
        g_pedSpeedTotals.AddTarget(target);
        if (target.source != Source::None && g_pedSpeedTargets.size() < replay::pedaudiostate::kMaxSpeedTargets)
            g_pedSpeedTargets.push_back({static_cast<std::uintptr_t>(holder), target.speed});
        const std::uint32_t block = timeMs / kBlockMs;
        auto logged = g_pedSpeedLoggedBlock.find(owner);
        if (logged != g_pedSpeedLoggedBlock.end() && logged->second == block)
            return;
        g_pedSpeedLoggedBlock[owner] = block;
        float engine = 0.0f;
        float inBlock = 0.0f;
        const bool engineKnown = replay::pedaudiostate::EngineSpeedOf(static_cast<std::uintptr_t>(holder), engine);
        const bool blockKnown = replay::pedaudiostate::ReadHolderSpeed(static_cast<std::uintptr_t>(holder), inBlock);
        g_pedSpeedTotals.AddCheck(target, engineKnown, engine, blockKnown, inBlock);
        if (g_pedSpeedTotals.lines >= kLineCap)
            return;
        ++g_pedSpeedTotals.lines;
        Ctx().log << OwnerLine(timeMs, replay::entity::IdText(puppet.packet.id),
                         puppet.packet.kind == replay::entity::EntityKind::Horse ? "horse" : "ped", target, engineKnown, engine,
                         blockKnown, inBlock, g_pedSpeedPublished)
                  << '\n';
    }
    }

    void TickPedAudioState(bool recording, std::uint32_t timeMs)
    {
        using namespace replay::pedaudiostate;
        if (!VariantsReady() && !BlocksReady())
            return;
        const std::uint64_t nowUs = replay::clock::NowUs();
        const bool scan = BlocksReady() && nowUs >= g_pedVarsNextScanUs;
        if (scan)
        {
            g_pedVarsNextScanUs = nowUs + kPedVarsScanUs;
            g_pedVarsRanges.clear();
        }
        if (recording)
        {
            const Ped player = PLAYER::PLAYER_PED_ID();
            for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
            {
                if (entity.vehicle || !entity.handle || !replay::entities::Exists(entity.handle) ||
                    !replay::entities::IsPed(entity.handle))
                    continue;
                const std::uint64_t holder = replay::audio::PedAudioHolder(replay::entities::AddressOf(entity.handle));
                if (!holder)
                    continue;
                const bool isPlayer = entity.handle == player;
                const std::uint32_t owner = entity.id.Packed();
                Variants v;
                bool resourceHeld = false;
                const bool read = VariantsReady() && ReadVariants(static_cast<std::uintptr_t>(holder), v, resourceHeld);
                if (VariantsReady() && !read)
                    ++g_pedAudioVariantTally.recReadFailed;
                else if (read && !v.Drawn())
                    ++g_pedAudioVariantTally.recUndrawn;
                if (read && g_pedAudioVariantTally.recLogged < 8)
                {
                    ++g_pedAudioVariantTally.recLogged;
                    Ctx().log << "FOLEY_VARIANT_READ,phase=record,ms=" << timeMs << ",owner=" << replay::entity::IdText(entity.id)
                        << ",player=" << (isPlayer ? 1 : 0) << ",a=" << v.a << ",b=" << v.b << ",resource=" << (resourceHeld ? 1 : 0) << '\n';
                }
                if (read && v.Drawn() && !Ctx().clip.foleyVariants.Note(owner, v, isPlayer))
                    ++g_pedAudioVariantTally.recKnown;
                else if (read && v.Drawn())
                {
                    ++g_pedAudioVariantTally.noted;
                    if (g_pedAudioVariantLines < kPedAudioVariantLineCap)
                    {
                        ++g_pedAudioVariantLines;
                        Ctx().log << "FOLEY_VARIANT,phase=record,ms=" << timeMs << ",owner=" << replay::entity::IdText(entity.id)
                            << ",player=" << (isPlayer ? 1 : 0) << ",a=" << v.a << ",b=" << v.b
                            << ",resource=" << (resourceHeld ? 1 : 0) << '\n';
                    }
                }
                if (scan)
                    ScanPedVarsOwner(true, timeMs, owner, isPlayer,
                        static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(entity.handle)), holder);
            }
        }
        else
        {
            // The puppets' speeds of this tick.
            const bool normalPlay = Ctx().hooks && Ctx().hooks->NormalPlay();
            const bool speedOn = g_pedSpeedReplay && SpeedOverrideInstalled();
            const bool motionOn = g_motionSpeedReplay && MotionSpeedInstalled();
            g_pedSpeedTargets.clear();
            g_motionTargets.clear();
            Variants livePlayer;
            {
                bool unused = false;
                const Ped player = PLAYER::PLAYER_PED_ID();
                const std::uint64_t holder = player ? replay::audio::PedAudioHolder(replay::entities::AddressOf(player)) : 0;
                if (!holder || !ReadVariants(static_cast<std::uintptr_t>(holder), livePlayer, unused))
                    livePlayer = Variants{};
            }
            for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
            {
                if ((puppet.packet.kind != replay::entity::EntityKind::Ped &&
                        puppet.packet.kind != replay::entity::EntityKind::Horse) ||
                    puppet.state != replay::playback::PuppetState::Ready || !puppet.clone ||
                    !replay::entities::Exists(puppet.clone) || !replay::entities::IsPed(puppet.clone))
                    continue;
                const std::uint64_t holder = replay::audio::PedAudioHolder(replay::entities::AddressOf(puppet.clone));
                if (!holder)
                    continue;
                const bool recordedPlayer = puppet.packet.kind == replay::entity::EntityKind::Ped && PoseV2IsRecordedPlayer(puppet);
                if (VariantsReady())
                    TickPuppetFoleyVariants(puppet, holder, recordedPlayer, livePlayer, timeMs);
                if (scan)
                    ScanPedVarsOwner(false, timeMs, puppet.packet.id.Packed(), recordedPlayer, puppet.packet.modelHash, holder);
                if (speedOn || motionOn)
                    TickPuppetSpeed(puppet, holder, timeMs, normalPlay, speedOn, motionOn);
            }
            // Published in normal play only; otherwise the engine's own value
            // (a paused puppet stands still).
            if (speedOn && normalPlay)
            {
                SetSpeedTargets(g_pedSpeedTargets);
                g_pedSpeedPublished = !g_pedSpeedTargets.empty();
            }
            else if (g_pedSpeedPublished)
            {
                SetSpeedTargets({});
                g_pedSpeedPublished = false;
            }
            // The same rule for the movement component.
            if (motionOn && normalPlay)
            {
                SetMotionTargets(g_motionTargets);
                g_motionPublished = !g_motionTargets.empty();
            }
            else if (g_motionPublished)
            {
                SetMotionTargets({});
                g_motionPublished = false;
            }
        }
        if (scan)
            SetNamedRanges(g_pedVarsRanges); // the volume probe's wvol names
    }

    void EndPedAudioState(const char* phase)
    {
        using namespace replay::pedaudiostate;
        if (!g_pedAudioStateTried)
            return;
        SetNamedRanges({});
        // The puppets' speed, totals.
        SetSpeedTargets({});
        g_pedSpeedPublished = false;
        SetMotionTargets({});
        g_motionPublished = false;
        if (std::strcmp(phase, "record_total") != 0)
        {
            const MotionCounts now = MotionSnapshot();
            Ctx().log << "MOTION_SPEED_REPLAY_TOTAL,enabled=" << (g_motionSpeedReplay ? 1 : 0)
                << ",installed=" << (MotionSpeedInstalled() ? 1 : 0)
                << ",error=" << (g_motionSpeedError.empty() ? "none" : g_motionSpeedError.c_str())
                << ",puppet_ticks=" << g_motionTally.puppetTicks << ",calls=" << (now.calls - g_motionBase.calls)
                << ",hits=" << (now.hits - g_motionBase.hits) << ",failed=" << (now.failed - g_motionBase.failed)
                << ",checked=" << g_motionTally.checked << ",speed_agree=" << g_motionTally.agree << '/' << g_motionTally.checked
                << ",gait_moving=" << g_motionTally.gaitMoving << '/' << g_motionTally.checked
                << ",state_lines=" << g_motionStateLines << ",lines=" << g_motionTally.lines << '\n';
        }
        if (std::strcmp(phase, "record_total") == 0)
            Ctx().log << replay::pedspeed::RecordLine(true, Ctx().clip.pedSpeed.series.size(),
                             Ctx().clip.pedSpeed.Points(), g_pedSpeedNoted, g_pedSpeedRefused)
                      << '\n';
        else
        {
            const SpeedCounts now = SpeedSnapshot();
            replay::pedspeed::DetourCounts d;
            d.calls = now.calls - g_pedSpeedBase.calls;
            d.hits = now.hits - g_pedSpeedBase.hits;
            d.writes = now.writes - g_pedSpeedBase.writes;
            d.failed = now.failed - g_pedSpeedBase.failed;
            Ctx().log << replay::pedspeed::TotalLine(g_pedSpeedReplay, SpeedOverrideInstalled(), g_pedSpeedError,
                             !Ctx().clip.pedSpeed.Empty(), Ctx().clip.pedSpeed.series.size(), Ctx().clip.pedSpeed.Points(),
                             g_pedSpeedTotals, d)
                      << '\n';
        }
        const PedAudioVariantTally& t = g_pedAudioVariantTally;
        Ctx().log << "FOLEY_VARIANT_SUMMARY,phase=" << phase << ",ready=" << (VariantsReady() ? 1 : 0)
            << ",clip_rows=" << Ctx().clip.foleyVariants.rows.size()
            << ",noted=" << t.noted << ",rec_read_failed=" << t.recReadFailed << ",rec_undrawn=" << t.recUndrawn
            << ",rec_known=" << t.recKnown << ",changed=" << Ctx().clip.foleyVariants.changed << ",puppets=" << t.seen
            << ",same=" << t.same
            << ",no_target=" << t.noTarget << ",no_resource=" << t.noResource << ",implausible=" << t.implausible
            << ",read_failed=" << t.readFailed << '\n';
        Ctx().log << "PED_VARS_SUMMARY,phase=" << phase << ",ready=" << (BlocksReady() ? 1 : 0)
            << ",owners=" << g_pedVarsLast.size() << ",lines=" << g_pedVarsLines << ",compared=" << g_pedVarsCompared
            << ",differing=" << g_pedVarsDiffering << ",diff_lines=" << g_pedVarsDiffLines << '\n';
    }

    // The movement component's state of each horse (foot_event_core.h), once
    // a second of clip time (recording clock while recording, playhead in
    // playback): speed, gait, thresholds, the feet's height, rise and last
    // event.
    void TickMotionState(bool recording, std::uint32_t timeMs)
    {
        if (g_motionStateLines >= replay::footevent::kMotionLineCap)
            return;
        const std::uint32_t slot = timeMs / replay::footevent::kMotionIntervalMs;
        // Once per owner and slot (the slot is marked first, so a ped that is
        // not a horse costs one lookup of its kind per second, cached).
        const auto due = [&](std::uint32_t owner) {
            auto it = g_motionStateSlot.find(owner);
            if (it != g_motionStateSlot.end() && it->second == slot)
                return false;
            g_motionStateSlot[owner] = slot;
            return true;
        };
        const auto log = [&](std::uint32_t owner, void* ped, const char* src) {
            if (g_motionStateLines >= replay::footevent::kMotionLineCap)
                return;
            ++g_motionStateLines;
            replay::footevent::MotionState state;
            replay::audio::ReadMotionState(ped, state);
            Ctx().log << replay::footevent::MotionLine(recording ? "rec" : "play", timeMs,
                             replay::entity::IdText(replay::entity::ReplayId::FromPacked(owner)), "horse", state, src)
                      << '\n';
        };
        if (recording)
        {
            for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
            {
                if (entity.vehicle || !entity.handle)
                    continue;
                const std::uint32_t owner = entity.id.Packed();
                if (!due(owner))
                    continue;
                auto kind = g_motionStateHorse.find(owner);
                if (kind == g_motionStateHorse.end())
                {
                    const replay::entity::CreatePacket* packet = Ctx().clip.FindCreate(owner);
                    kind = g_motionStateHorse.emplace(owner, packet && packet->kind == replay::entity::EntityKind::Horse).first;
                }
                if (!kind->second || !replay::entities::Exists(entity.handle) || !replay::entities::IsPed(entity.handle))
                    continue;
                log(owner, replay::entities::AddressOf(entity.handle), "engine");
            }
            return;
        }
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind != replay::entity::EntityKind::Horse ||
                puppet.state != replay::playback::PuppetState::Ready || !puppet.clone ||
                !replay::entities::Exists(puppet.clone) || !replay::entities::IsPed(puppet.clone) ||
                !due(puppet.packet.id.Packed()))
                continue;
            void* ped = replay::entities::AddressOf(puppet.clone);
            const bool replayed = g_motionPublished &&
                std::any_of(g_motionTargets.begin(), g_motionTargets.end(),
                    [ped](const replay::pedaudiostate::MotionTarget& t) { return t.ped == reinterpret_cast<std::uintptr_t>(ped); });
            log(puppet.packet.id.Packed(), ped, replayed ? "replay" : "engine");
        }
    }
    namespace
    {

    // ---- the secondary steps (FSEV) ----
    void EmitFootEventWindow(std::uint32_t windowMs, std::uint32_t owner, const replay::footevent::WindowCounts& counts)
    {
        if (g_footEventTotals.lines >= replay::footevent::kWindowLineCap)
            return;
        ++g_footEventTotals.lines;
        Ctx().log << replay::footevent::WindowLine(g_footEventPhase, windowMs,
                         replay::entity::IdText(replay::entity::ReplayId::FromPacked(owner)), counts)
                  << '\n';
    }

    replay::footevent::WindowCounts& FootEventWindow(std::uint32_t owner, std::uint32_t timeMs)
    {
        return g_footEventWindows.At(owner, timeMs,
            [](std::uint32_t windowMs, std::uint32_t o, const replay::footevent::WindowCounts& c) { EmitFootEventWindow(windowMs, o, c); });
    }

    void EmitMovementWindow(std::uint32_t window, std::uint32_t owner, const replay::footevent::MovementWindow& w)
    {
        if (!w.Any() || g_movementLines >= replay::footevent::kMovementLineCap)
            return;
        ++g_movementLines;
        Ctx().log << replay::footevent::MovementLine(g_footEventPhase, window * replay::footevent::kWindowMs,
                         replay::entity::IdText(replay::entity::ReplayId::FromPacked(owner)), w)
                  << '\n';
    }
    }

    // Both phases, every tick (after the foot events): the movement volume
    // calls of the recorded peds (record) or the puppets (play), per owner
    // and 5 s of clip time.
    void TickMovementDetail(bool recording, std::uint32_t timeMs)
    {
        // The ring also feeds FMOV while recording.
        static std::vector<replay::audio::MovementDetailCall> calls;
        replay::audio::CollectMovementDetailSince(g_movementDetailCursor, calls);
        if (calls.empty())
            return;
        static std::vector<std::pair<std::uint64_t, std::uint32_t>> feet;
        feet.clear();
        if (recording)
        {
            for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
            {
                if (entity.vehicle || !entity.handle || !replay::entities::Exists(entity.handle) ||
                    !replay::entities::IsPed(entity.handle))
                    continue;
                const std::uint64_t foot = replay::audio::FootstepAudio(replay::entities::AddressOf(entity.handle));
                if (foot)
                    feet.push_back({foot, entity.id.Packed()});
            }
        }
        else
        {
            for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
            {
                if ((puppet.packet.kind != replay::entity::EntityKind::Ped &&
                        puppet.packet.kind != replay::entity::EntityKind::Horse) ||
                    puppet.state != replay::playback::PuppetState::Ready || !puppet.clone ||
                    !replay::entities::Exists(puppet.clone) || !replay::entities::IsPed(puppet.clone))
                    continue;
                const std::uint64_t foot = replay::audio::FootstepAudio(replay::entities::AddressOf(puppet.clone));
                if (foot)
                    feet.push_back({foot, puppet.packet.id.Packed()});
            }
        }
        const std::uint64_t now = replay::audio::NowUs();
        const std::uint32_t nowMs = recording ? replay::pose::capture::NowMs() : timeMs;
        for (const replay::audio::MovementDetailCall& call : calls)
        {
            const auto owner = std::find_if(feet.begin(), feet.end(), [&](const auto& entry) { return entry.first == call.footAudio; });
            if (owner == feet.end())
                continue;
            const std::uint64_t ageMs = now > call.timeUs ? (now - call.timeUs) / 1000 : 0;
            const std::uint32_t at = recording ? replay::pose::capture::RecordMsAgo(ageMs, nowMs) : timeMs;
            if (recording)
            {
                // The live step's result, to give back to the puppet.
                replay::footmovement::Sample s;
                s.owner = owner->second;
                s.timeMs = at;
                s.foot = call.foot;
                s.result = call.result;
                ++(Ctx().clip.footMovement.Add(s) ? g_footMovementRecorded : g_footMovementRejected);
            }
            const std::uint32_t window = at / replay::footevent::kWindowMs;
            auto& entry = g_movementWindows[owner->second];
            if (entry.second.Any() && entry.first != window)
            {
                EmitMovementWindow(entry.first, owner->second, entry.second);
                entry.second = replay::footevent::MovementWindow{};
            }
            entry.first = window;
            entry.second.Add(call.value, call.result, call.gate, call.f504Known, call.f504);
        }
    }

    // Playback, every tick: the recorded result of each puppet's feet nearest
    // to the playhead, for the movement volume detour.
    void PublishFootMovement(std::uint32_t timeMs)
    {
        if (!g_footMovementReplay || Ctx().clip.footMovement.Empty())
            return;
        const std::size_t slots = replay::audio::RecordedMovementSlots();
        std::size_t used = 0;
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (used >= slots)
                break;
            if ((puppet.packet.kind != replay::entity::EntityKind::Ped &&
                    puppet.packet.kind != replay::entity::EntityKind::Horse) ||
                puppet.state != replay::playback::PuppetState::Ready || !puppet.clone ||
                !replay::entities::Exists(puppet.clone))
                continue;
            float values[replay::footmovement::kMaxFoot] = {};
            std::uint32_t mask = 0;
            for (std::uint8_t foot = 0; foot < replay::footmovement::kMaxFoot; ++foot)
                if (replay::footmovement::Nearest(Ctx().clip.footMovement, puppet.packet.id.Packed(), foot, timeMs, values[foot]))
                    mask |= 1u << foot;
            if (!mask)
                continue;
            replay::audio::PublishRecordedMovement(used++,
                reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone)), mask, values);
        }
        for (std::size_t n = used; n < slots; ++n)
            replay::audio::PublishRecordedMovement(n, 0, 0, nullptr);
        ++g_footMovementPublished;
    }

    // Both phases: the ring's stale calls go, the windows and totals start over.
    void BeginFootEvents(bool recording)
    {
        {
            static std::vector<replay::audio::MovementDetailCall> staleDetail;
            replay::audio::CollectMovementDetailSince(g_movementDetailCursor, staleDetail);
            g_movementWindows.clear();
            g_movementLines = 0;
            replay::audio::SetMovementDetail(true);
            g_footMovementRecorded = g_footMovementRejected = 0;
            g_footMovementPublished = 0;
            g_footMovementReplayBase = replay::audio::MovementReplaySnapshot();
            // The recorded results on the puppets, for a clip with FMOV.
            replay::audio::SetMovementReplay(!recording && g_footMovementReplay && !Ctx().clip.footMovement.Empty());
            for (std::size_t n = 0; n < replay::audio::RecordedMovementSlots(); ++n)
                replay::audio::PublishRecordedMovement(n, 0, 0, nullptr);
        }
        static std::vector<replay::audio::FootEventCall> stale;
        replay::audio::CollectFootEventsSince(g_footEventCursor, stale);
        g_footEventWindows = replay::footevent::OwnerWindows{};
        g_footEventTotals = replay::footevent::Totals{};
        g_footEventPhase = recording ? "rec" : "play";
        g_footEventRefusing = !recording && replay::footevent::RefuseOwn(!Ctx().clip.footEvents.Empty(), g_footEventReplay);
        replay::audio::SetFootEventRefuseOwn(g_footEventRefusing);
        const replay::audio::FootEventStats stats = replay::audio::FootEventSnapshot();
        Ctx().log << "FOOT_EVENT_BEGIN,phase=" << g_footEventPhase << ",installed=" << (stats.installed ? 1 : 0)
                  << ",error=" << (stats.error.empty() ? "none" : stats.error.c_str())
                  << ",replay=" << (g_footEventReplay ? 1 : 0)
                  << ",section=" << (Ctx().clip.footEvents.Empty() ? "none" : "FSEV")
                  << ",clip_events=" << Ctx().clip.footEvents.events.size() << ",refuse_own=" << (g_footEventRefusing ? 1 : 0)
                  << '\n';
    }

    void EndFootEvents(const char* phase)
    {
        for (const auto& [owner, entry] : g_movementWindows)
            EmitMovementWindow(entry.first, owner, entry.second);
        g_movementWindows.clear();
        replay::audio::SetMovementDetail(false);
        {
            // FOOT_MOVEMENT_TOTAL.
            const replay::audio::MovementReplayStats now = replay::audio::MovementReplaySnapshot();
            Ctx().log << "FOOT_MOVEMENT_TOTAL,phase=" << phase
                      << ",replay=" << (g_footMovementReplay ? 1 : 0) << ",section=" << (Ctx().clip.footMovement.Empty() ? "none" : "FMOV")
                      << ",clip_samples=" << Ctx().clip.footMovement.samples.size() << ",recorded=" << g_footMovementRecorded
                      << ",rejected=" << g_footMovementRejected << ",published_ticks=" << g_footMovementPublished
                      << ",given=" << (now.given - g_footMovementReplayBase.given)
                      << ",unmatched=" << (now.unmatched - g_footMovementReplayBase.unmatched) << ",on=" << (now.on ? 1 : 0)
                      << '\n';
            replay::audio::SetMovementReplay(false);
            for (std::size_t n = 0; n < replay::audio::RecordedMovementSlots(); ++n)
                replay::audio::PublishRecordedMovement(n, 0, 0, nullptr);
        }
        g_footEventWindows.Flush(
            [](std::uint32_t windowMs, std::uint32_t o, const replay::footevent::WindowCounts& c) { EmitFootEventWindow(windowMs, o, c); });
        replay::audio::SetFootEventRefuseOwn(false);
        const bool refused = g_footEventRefusing;
        g_footEventRefusing = false;
        const replay::audio::FootEventStats stats = replay::audio::FootEventSnapshot();
        Ctx().log << replay::footevent::TotalLine(phase, stats.installed, stats.error, true, g_footEventReplay,
                         !Ctx().clip.footEvents.Empty(), refused, Ctx().clip.footEvents.events.size(),
                         Ctx().clip.footEvents.Owners(), g_footEventTotals)
                  << '\n';
    }

    // Recording, every tick: the entries the engine queued on a recorded
    // ped's or horse's foot audio go to the clip at their recording time.
    void RecordFootEvents()
    {
        REPLAY_PROFILE_ZONE("rec.foot_events");
        static std::vector<replay::audio::FootEventCall> calls;
        replay::audio::CollectFootEventsSince(g_footEventCursor, calls);
        if (calls.empty())
            return;
        static std::vector<std::pair<std::uint64_t, std::uint32_t>> owners;
        owners.clear();
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (entity.vehicle || !entity.handle || !replay::entities::Exists(entity.handle) ||
                !replay::entities::IsPed(entity.handle))
                continue;
            const std::uint64_t footAudio = replay::audio::FootstepAudio(replay::entities::AddressOf(entity.handle));
            if (footAudio)
                owners.push_back({footAudio, entity.id.Packed()});
        }
        const std::uint64_t now = replay::audio::NowUs();
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        for (const replay::audio::FootEventCall& call : calls)
        {
            const auto owner = std::find_if(owners.begin(), owners.end(),
                [&](const auto& entry) { return entry.first == call.footAudio; });
            if (owner == owners.end())
            {
                g_footEventTotals.unmapped += call.queued ? 1 : 0;
                continue;
            }
            const std::uint64_t ageMs = now > call.timeUs ? (now - call.timeUs) / 1000 : 0;
            const std::uint32_t timeMs = replay::pose::capture::RecordMsAgo(ageMs, nowMs);
            const replay::footevent::Origin origin = static_cast<replay::footevent::Origin>(call.origin);
            replay::footevent::WindowCounts& window = FootEventWindow(owner->second, timeMs);
            window.AddCall(origin);
            ++g_footEventTotals.calls;
            g_footEventTotals.gen += origin == replay::footevent::Origin::Generator ? 1 : 0;
            g_footEventTotals.drain += origin == replay::footevent::Origin::Drain ? 1 : 0;
            g_footEventTotals.other += origin == replay::footevent::Origin::Other ? 1 : 0;
            if (!call.queued)
                continue;
            ++window.enq;
            window.AddType(call.type);
            ++g_footEventTotals.enq;
            replay::footevent::Event e;
            e.owner = owner->second;
            e.timeMs = timeMs;
            e.type = static_cast<std::int8_t>(call.type);
            e.foot = call.foot;
            e.origin = call.origin;
            e.flags = call.impactKnown ? replay::footevent::kFlagImpact : 0;
            e.materialHash = call.materialHash;
            e.materialRef = call.materialRef;
            e.impact = call.impactKnown ? call.impact : 0.0f;
            if (replay::footevent::ValidType(call.type) && Ctx().clip.footEvents.Add(e))
                ++g_footEventTotals.recorded;
            else
                ++g_footEventTotals.rejected;
        }
    }

    // The recording gate: the ring's calls of the pause are discarded.
    void DiscardFootEventsWhilePaused()
    {
        static std::vector<replay::audio::FootEventCall> stale;
        replay::audio::CollectFootEventsSince(g_footEventCursor, stale);
        static std::vector<replay::audio::MovementDetailCall> staleDetail;
        replay::audio::CollectMovementDetailSince(g_movementDetailCursor, staleDetail);
    }

    // Playback, every tick: what the engine did on the puppets' feet (their
    // own generator, refused or not), then the clip's events the playhead
    // passed in normal play, queued on their puppets.
    void PlayFootEvents(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.foot_events");
        static std::vector<replay::audio::FootEventCall> calls;
        replay::audio::CollectFootEventsSince(g_footEventCursor, calls);
        if (!calls.empty())
        {
            static std::vector<std::pair<std::uint64_t, std::uint32_t>> feet;
            feet.clear();
            for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
            {
                if ((puppet.packet.kind != replay::entity::EntityKind::Ped &&
                        puppet.packet.kind != replay::entity::EntityKind::Horse) ||
                    puppet.state != replay::playback::PuppetState::Ready || !puppet.clone ||
                    !replay::entities::Exists(puppet.clone) || !replay::entities::IsPed(puppet.clone))
                    continue;
                const std::uint64_t foot = replay::audio::FootstepAudio(replay::entities::AddressOf(puppet.clone));
                if (foot)
                    feet.push_back({foot, puppet.packet.id.Packed()});
            }
            for (const replay::audio::FootEventCall& call : calls)
            {
                const auto owner = std::find_if(feet.begin(), feet.end(),
                    [&](const auto& entry) { return entry.first == call.footAudio; });
                if (owner == feet.end())
                    continue;
                const replay::footevent::Origin origin = static_cast<replay::footevent::Origin>(call.origin);
                replay::footevent::WindowCounts& window = FootEventWindow(owner->second, timeMs);
                window.AddCall(origin);
                ++g_footEventTotals.calls;
                g_footEventTotals.gen += origin == replay::footevent::Origin::Generator ? 1 : 0;
                g_footEventTotals.drain += origin == replay::footevent::Origin::Drain ? 1 : 0;
                g_footEventTotals.other += origin == replay::footevent::Origin::Other ? 1 : 0;
                if (call.refused)
                {
                    ++window.refused;
                    ++g_footEventTotals.refusedOwn;
                }
                else if (call.queued)
                {
                    ++window.enq;
                    window.AddType(call.type);
                    ++g_footEventTotals.enq;
                }
            }
        }
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
        const bool normalPlay = Ctx().hooks && Ctx().hooks->NormalPlay();
        if (!g_footEventCursorPlay.Advance(timeMs, normalPlay, fromMs, toMs) || !g_footEventReplay ||
            Ctx().clip.footEvents.Empty())
            return;
        static std::vector<std::size_t> due;
        Ctx().clip.footEvents.Range(fromMs, toMs, due);
        for (const std::size_t index : due)
        {
            const replay::footevent::Event& e = Ctx().clip.footEvents.events[index];
            if (!replay::footevent::Dispatch(g_footEventReplay, normalPlay, e))
                continue;
            ++g_footEventTotals.due;
            replay::footevent::WindowCounts& window = FootEventWindow(e.owner, e.timeMs);
            const replay::playback::Puppet* puppet = Ctx().puppets.Find(replay::entity::ReplayId::FromPacked(e.owner));
            if (!puppet || puppet->state != replay::playback::PuppetState::Ready || !puppet->clone ||
                !replay::entities::Exists(puppet->clone) || !replay::entities::IsPed(puppet->clone))
            {
                ++g_footEventTotals.noPuppet;
                ++window.failed;
                continue;
            }
            std::string status;
            if (replay::audio::PlayFootEvent(replay::entities::AddressOf(puppet->clone), e.type, e.foot, e.materialHash,
                    e.materialRef, (e.flags & replay::footevent::kFlagImpact) != 0, e.impact, status))
            {
                ++g_footEventTotals.played;
                ++window.replayed;
                window.AddType(e.type);
            }
            else if (status == replay::footevent::PlayStatusName(replay::footevent::PlayStatus::QueueFull))
            {
                ++g_footEventTotals.queueFull;
                ++window.queueFull;
            }
            else
            {
                ++g_footEventTotals.failed;
                ++window.failed;
                static std::uint32_t failLogs = 0;
                if (failLogs < 20)
                {
                    ++failLogs;
                    Ctx().log << "FOOT_EVENT_FAIL,clip_ms=" << e.timeMs << ",owner=" << replay::entity::IdText(puppet->packet.id)
                              << ",type=" << int(e.type) << ",foot=" << int(e.foot) << ",status=" << status << '\n';
                }
            }
        }
    }

    // Evidence only (horse_audio_session.h). The horse whose audio is
    // compared: recording, the horse the local player rides (natives); playback,
    // the horse puppet the player's puppet sits on (the same pairs as the mount
    // audio).
    void TickPoseV2HorseAudioEvidence(bool recording, std::uint32_t timeMs)
    {
        if (!replay::horseaudio::Enabled())
            return;
        void* horse = nullptr;
        void* rider = nullptr; // the rider's own sounds logged too
        std::string id = "none";
        if (recording)
        {
            const Ped player = PLAYER::PLAYER_PED_ID();
            if (player && replay::entities::Exists(player))
                rider = replay::entities::AddressOf(player);
            if (player && PED::IS_PED_ON_MOUNT(player))
            {
                const Ped mount = PED::GET_MOUNT(player);
                if (mount && replay::entities::Exists(mount))
                {
                    horse = replay::entities::AddressOf(mount);
                    const replay::entity::ReplayId recorded = RecordedIdAtAddress(reinterpret_cast<std::uint64_t>(horse));
                    id = recorded.Valid() ? replay::entity::IdText(recorded) : std::string("unrecorded");
                }
            }
        }
        else
        {
            static std::vector<std::uint32_t> riders;
            PoseV2PlayerRiderIds(riders);
            const std::vector<std::uint32_t> mounts =
                replay::mountaudio::PlayerMounts(riders, [](std::uint32_t rider) { return replay::playback::MountOf(rider); });
            for (const std::uint32_t riderId : riders)
            {
                const replay::playback::Puppet* puppet = Ctx().puppets.Find(replay::entity::ReplayId::FromPacked(riderId));
                if (puppet && puppet->state == replay::playback::PuppetState::Ready && puppet->clone &&
                    replay::entities::Exists(puppet->clone) && replay::entities::IsPed(puppet->clone))
                {
                    rider = replay::entities::AddressOf(puppet->clone);
                    break;
                }
            }
            for (const std::uint32_t mount : mounts)
            {
                const replay::playback::Puppet* puppet = Ctx().puppets.Find(replay::entity::ReplayId::FromPacked(mount));
                if (!puppet || puppet->state != replay::playback::PuppetState::Ready || !puppet->clone ||
                    !replay::entities::Exists(puppet->clone) || !replay::entities::IsPed(puppet->clone))
                    continue;
                horse = replay::entities::AddressOf(puppet->clone);
                id = replay::entity::IdText(puppet->packet.id);
                break;
            }
        }
        replay::horseaudio::Tick(horse, rider, id, timeMs, Ctx().log);
    }

    // The replay's feet. The recording keeps every footstep (FOOT) and every
    // foot puff (FXEV, FootVfxSite); the puppets' own animation and the hidden
    // player (moved to the focus puppet, inside the horse when the main character
    // rides) would raise theirs on top: hooves out of step with the recorded ones
    // and puffs at feet the replay does not show. While a clip plays their own
    // foot-down audio and the engine's foot puffs are kept out; footsteps and
    // foot fx come only from the clip.
    namespace
    {
    // The same rule for the puppets' own create-and-play one-shots
    // (puppet_audio_rule.h; small animals would otherwise make many times the
    // recorded count).
    std::uint64_t g_poseV2OwnOneShotsBase = 0;
    std::size_t g_poseV2OwnOneShotHolders = 0;
    ULONGLONG g_poseV2OwnFeetAt = 0;
    std::size_t g_poseV2OwnFeetCount = 0;
    std::uint64_t g_poseV2OwnFeetSilencedBase = 0;
    std::uint64_t g_poseV2OwnFootVfxBase = 0;
    // The weapon puppets' own one-shots (SND kind 4 carries the recorded ones)
    // and the puppets' own persistent ped audio sounds from a call site the
    // clip's ESND track carries (puppet_audio_rule.h). Same switch, plus
    // RefusePuppetOwnPersistent=0 for the persistent part alone.
    bool g_poseV2OwnClipScanned = false;
    bool g_poseV2ClipHasWeaponSounds = false;
    std::size_t g_poseV2OwnPersistentCallers = 0;
    // The clip's (entity, call site, entry) keys (puppet_audio_rule.h
    // OwnPersistentCarried).
    std::size_t g_poseV2OwnPersistentKeys = 0;
    bool g_poseV2OwnPersistentKeysTruncated = false;
    std::size_t g_poseV2OwnWeaponAudios = 0;
    std::uint64_t g_poseV2OwnWeaponOneShotsBase = 0;
    }

    void BeginPoseV2OwnFeet()
    {
        g_poseV2OwnFeetAt = 0;
        g_poseV2OwnFeetCount = 0;
        g_poseV2OwnClipScanned = false;
        g_poseV2ClipHasWeaponSounds = false;
        g_poseV2OwnPersistentCallers = 0;
        g_poseV2OwnPersistentKeys = 0;
        g_poseV2OwnPersistentKeysTruncated = false;
        g_poseV2OwnWeaponAudios = 0;
        g_poseV2OwnWeaponOneShotsBase = replay::audio::RefusedPuppetWeaponOneShots();
        replay::audio::SetSilencedPuppetWeaponAudios({});
        replay::audio::SetOwnPersistentCallers({});
        replay::audio::SetOwnPersistentKeys({});
        replay::audio::BeginPuppetOwnPersistent();
        replay::audio::BeginUnrecordedPersistent();
        g_poseV2OwnFeetSilencedBase = replay::audio::SilencedFootsteps();
        g_poseV2OwnFootVfxBase = replay::fx::SuppressedFootVfx();
        replay::audio::SetSilencedFootAudios({});
        replay::audio::SetSilencedPuppetAudios({});
        replay::audio::ClearFootMaterials();
        g_poseV2OwnOneShotsBase = replay::audio::BeginRefusedPuppetOneShots();
        g_poseV2OwnOneShotHolders = 0;
        replay::fx::SetEngineFootVfxSuppressed(true);
    }

    // The hidden player and the horse under him (the world clear hides both).
    void PoseV2HiddenPlayerEntities(std::vector<std::uint64_t>& out)
    {
        const Ped player = PLAYER::PLAYER_PED_ID();
        if (!player || !replay::entities::Exists(player))
            return;
        if (const auto* address = replay::entities::AddressOf(player))
            out.push_back(reinterpret_cast<std::uint64_t>(address));
        if (PED::IS_PED_ON_MOUNT(player))
        {
            const Ped mount = PED::GET_MOUNT(player);
            if (mount && replay::entities::Exists(mount))
                if (const auto* address = replay::entities::AddressOf(mount))
                    out.push_back(reinterpret_cast<std::uint64_t>(address));
        }
    }

    void UpdatePoseV2OwnFeet()
    {
        const ULONGLONG now = GetTickCount64();
        if (now - g_poseV2OwnFeetAt < 250)
            return;
        g_poseV2OwnFeetAt = now;
        static std::vector<std::uint64_t> feet;
        static std::vector<std::uint64_t> peds;
        static std::vector<std::uint32_t> pedOwners; // the puppets' clip entities
        feet.clear();
        peds.clear();
        pedOwners.clear();
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if ((puppet.packet.kind != replay::entity::EntityKind::Ped &&
                    puppet.packet.kind != replay::entity::EntityKind::Horse) ||
                puppet.state != replay::playback::PuppetState::Ready || !puppet.clone ||
                !replay::entities::Exists(puppet.clone) || !replay::entities::IsPed(puppet.clone))
                continue;
            peds.push_back(reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone)));
            pedOwners.push_back(puppet.packet.id.Packed());
        }
        PoseV2HiddenPlayerEntities(peds);
        static std::vector<std::uint64_t> holders;
        holders.clear();
        // Each holder's clip entity (puppet_audio_rule.h OwnPersistentCarried);
        // the hidden player and his mount (after the puppets in `peds`) have none.
        static std::vector<std::uint32_t> holderOwners;
        holderOwners.clear();
        for (std::size_t index = 0; index < peds.size(); ++index)
        {
            const std::uint64_t ped = peds[index];
            if (const std::uint64_t foot = replay::audio::FootstepAudio(reinterpret_cast<void*>(ped)))
                feet.push_back(foot);
            if (const std::uint64_t holder = replay::audio::PedAudioHolder(reinterpret_cast<void*>(ped)))
            {
                holders.push_back(holder);
                holderOwners.push_back(index < pedOwners.size() ? pedOwners[index] : replay::puppetaudio::kNoOwner);
            }
        }
        // What the clip's tracks carry, once per playback.
        if (!g_poseV2OwnClipScanned)
        {
            g_poseV2OwnClipScanned = true;
            std::vector<std::uint32_t> callers;
            std::vector<std::uint64_t> keys; // (entity, call site, entry)
            for (const replay::EntitySound& sound : Ctx().clip.entitySounds)
            {
                if (!sound.pedAudio || !sound.caller)
                    continue;
                if (std::find(callers.begin(), callers.end(), sound.caller) == callers.end())
                    callers.push_back(sound.caller);
                keys.push_back(replay::puppetaudio::OwnPersistentKey(sound.entity.Packed(), sound.caller,
                    replay::puppetaudio::SoundEntryKey(sound.soundRef, sound.byHash)));
            }
            std::sort(keys.begin(), keys.end()); // the published list is sorted (binary search in the detour)
            keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
            replay::audio::SetOwnPersistentCallers(callers);
            // The count the detour really searches (the table holds 4096 keys), not
            // the clip's.
            g_poseV2OwnPersistentKeys = replay::audio::SetOwnPersistentKeys(keys);
            g_poseV2OwnPersistentKeysTruncated = keys.size() > g_poseV2OwnPersistentKeys;
            g_poseV2OwnPersistentCallers = callers.size();
            for (const replay::entity::SoundEvent& sound : Ctx().clip.sounds.Events())
                if (sound.kind == 4)
                {
                    g_poseV2ClipHasWeaponSounds = true;
                    break;
                }
        }
        static std::vector<std::uint64_t> weapons;
        weapons.clear();
        if (g_poseV2ClipHasWeaponSounds)
            for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
            {
                if (puppet.packet.kind != replay::entity::EntityKind::Weapon ||
                    puppet.state != replay::playback::PuppetState::Ready || !puppet.clone ||
                    !replay::entities::Exists(puppet.clone))
                    continue;
                if (const std::uint64_t weapon = replay::audio::WeaponAudio(replay::entities::AddressOf(puppet.clone)))
                    weapons.push_back(weapon);
            }
        replay::audio::SetSilencedFootAudios(feet);
        replay::audio::SetSilencedPuppetAudios(holders, holderOwners); // with the clip entities
        replay::audio::SetSilencedPuppetWeaponAudios(weapons);
        g_poseV2OwnFeetCount = feet.size();
        g_poseV2OwnOneShotHolders = holders.size();
        g_poseV2OwnWeaponAudios = weapons.size();
    }

    bool PoseV2OwnPersistentRefused()
    {
        // Packets mode refuses every own create of the holders.
        if (replay::audio::BlockPuppetSounds() && g_poseV2OwnOneShotHolders > 0)
            return true;
        return replay::audio::RefusePuppetOwnPersistent() &&
            g_poseV2OwnOneShotHolders > 0 && g_poseV2OwnPersistentCallers > 0;
    }

    replay::audio::MovementTermStats g_poseV2MovementTermBase;

    void EndPoseV2OwnFeet(std::ostream& log)
    {
        replay::audio::SetPuppetSpeechEntities({}, {});
        replay::audio::SetFootEventRefuseOwn(false);
        replay::audio::SetSilencedFootAudios({});
        replay::audio::SetSilencedPuppetAudios({});
        replay::audio::SetSilencedPuppetWeaponAudios({});
        replay::audio::SetOwnPersistentCallers({});
        replay::audio::SetOwnPersistentKeys({});
        log << "POSE_V2_PLAYBACK_OWN_EXTRA,persistent_enabled=" << (replay::audio::RefusePuppetOwnPersistent() ? 1 : 0)
            << ",persistent_callers=" << g_poseV2OwnPersistentCallers
            << ",persistent_keys=" << g_poseV2OwnPersistentKeys
            << (g_poseV2OwnPersistentKeysTruncated ? ",truncated=1" : "")
            << ",own_persistent_refused=" << replay::audio::RefusedPuppetOwnPersistent()
            << ",own_persistent_kept=" << replay::audio::KeptPuppetOwnPersistent()
            << ",clip_weapon_sounds=" << (g_poseV2ClipHasWeaponSounds ? 1 : 0)
            << ",weapon_audios=" << g_poseV2OwnWeaponAudios
            << ",weapon_one_shots_refused="
            << (replay::audio::RefusedPuppetWeaponOneShots() - g_poseV2OwnWeaponOneShotsBase)
            << ",samples=" << replay::audio::PuppetOwnPersistentSamples() << '\n';
        log << replay::audio::UnrecordedPersistentLine() << '\n';
        {
            // The recorded surface on the puppets' feet (foot_material.h).
            const replay::audio::FootMaterialStats stats = replay::audio::GetFootMaterialStats();
            log << "POSE_V2_PLAYBACK_FOOT_MATERIAL,requested=" << stats.requested << ",written=" << stats.written << ",same=" << stats.same
                << ",no_container=" << stats.noContainer << ",bad_foot=" << stats.badFoot
                << ",detour_writes=" << stats.detourWrites << '\n';
            // The ground layers.
            log << "POSE_V2_PLAYBACK_FOOT_LAYERS,enabled=" << (replay::audio::FootLayerReplay() ? 1 : 0)
                << ",requested=" << stats.layerRequested << ",written=" << stats.layerWritten << ",same=" << stats.layerSame
                << ",bad_foot=" << stats.layerBadFoot << ",detour_writes=" << stats.layerDetourWrites << '\n';
            // The foot mix (foot_material.h FootMix) and the movement volume term.
            replay::audio::SetMovementTermState(false, false);
            {
                const replay::audio::MovementTermStats term = replay::audio::MovementTermSnapshot();
                log << "POSE_V2_PLAYBACK_MOVEMENT_TERM,installed=" << (term.installed ? 1 : 0)
                    << ",calls=" << (term.calls - g_poseV2MovementTermBase.calls)
                    << ",given=" << (term.given - g_poseV2MovementTermBase.given)
                    << ",given_player=" << (term.givenPlayer - g_poseV2MovementTermBase.givenPlayer) << '\n';
            }
            log << "POSE_V2_PLAYBACK_FOOT_MIX,requested=" << stats.mixRequested << ",written=" << stats.mixWritten << ",same=" << stats.mixSame
                << ",bad_foot=" << stats.mixBadFoot << ",detour_writes=" << stats.mixDetourWrites
                << ",base_applied=" << stats.baseApplied << ",impact_written=" << stats.impactWritten
                << ",impact_detour_writes=" << stats.impactDetourWrites << '\n';
            replay::audio::ClearFootMaterials();
        }
        replay::fx::SetEngineFootVfxSuppressed(false);
        log << "POSE_V2_PLAYBACK_OWN_FEET,own_steps_silenced=" << (replay::audio::SilencedFootsteps() - g_poseV2OwnFeetSilencedBase)
            << ",engine_foot_fx_refused=" << (replay::fx::SuppressedFootVfx() - g_poseV2OwnFootVfxBase)
            << ",feet=" << g_poseV2OwnFeetCount
            << ",own_one_shots_refused=" << (replay::audio::RefusedPuppetOneShots() - g_poseV2OwnOneShotsBase)
            << ",holders=" << g_poseV2OwnOneShotHolders
            << ",samples=" << replay::audio::RefusedPuppetOneShotSamples() << '\n';
    }
}
