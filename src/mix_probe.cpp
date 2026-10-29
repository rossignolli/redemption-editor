#include "mix_probe.h"
#include "mix_probe_core.h"

#include "audio_observer.h"
#include "entity_cache.h"
#include "mod_clock.h"
#include "log_format.h"
#include "qa_kit.h"
#include "replay_playback.h"
#include "sound_census.h"
#include "version.generated.h"
#include "storage_paths.h" // ReplayDebug in the base folder

#include <windows.h>

#include "main.h"
#include "natives.h"

#include <cstring>
#include <ctime>
#include <fstream>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

// The mix probe, engine side (mix_probe.h). Every engine access here reads:
// natives that read, and the guarded reads of audio_observer.cpp (PedAudioLod,
// PedAudioPlayerFlag, MountLinkIsPlayer, MountLinkComponent) on fields proven
// offline before this probe. Script thread only.

namespace replay::mixprobe
{
    namespace
    {
        bool g_enabled = false;
        const char* g_phase = "idle"; // census phase: "rec", "play", "idle"
        std::uint64_t g_lastTickWall = 0;
        Totals g_totals;
        LineBudget g_lines;
        SampleBudget g_sampleBudget;
        std::uint64_t g_stored = 0;
        std::uint64_t g_dropped = 0;
        // The queued requests' families (the ped steps reach the creator
        // through the S03 queue).
        RequestFamilies g_requests;
        std::uint64_t g_requestCursor = 0;
        std::vector<replay::audio::SoundRequest> g_requestBatch;
        std::uint64_t g_queuedKinds = 0; // samples whose kind came from the queued request
        // Weapons whose holder came from the clip's WeaponPoint attachment in
        // this pass (the replay never sets Puppet::attached for a weapon:
        // replay_playback.cpp skips them).
        std::uint64_t g_weaponHolders = 0;
        std::uint64_t g_weaponNoHolder = 0;
        std::vector<replay::entity::Attachment> g_attachments;
        replay::audio::PlayerAudioStats g_weaponBase;
        replay::audio::MountAudioStats g_mountBase;

        // The playback's side.
        std::vector<Sample> g_play;
        std::vector<Listener> g_playListeners;
        Coverage g_cover;
        bool g_playing = false;
        double g_lastClipMs = -1.0;
        bool g_lastNormal = false;
        std::uint64_t g_markSeen = 0;
        std::uint64_t g_nextListenerWall = 0;
        std::uint32_t g_secondSounds = 0;
        constexpr std::uint32_t kMarkWindowMs = 3000;
        constexpr std::size_t kPlaySamplesMax = 400000; // ~10 MB; a playback longer than that stops keeping samples

        // The entity table (rebuilt 4 times a second).
        struct Entry
        {
            Entity handle = 0;
            std::uint64_t address = 0;
            census::OwnerClass cls = census::OwnerClass::World;
            std::uint32_t holder = census::kNoOwner; // a weapon's ped
        };
        std::unordered_map<std::uint32_t, Entry> g_table;
        std::unordered_map<std::uint32_t, census::OwnerClass> g_classes; // per pass
        std::uint64_t g_tableBuiltWall = 0;
        std::uint32_t g_mainId = census::kNoOwner;
        std::uint64_t g_mainAddress = 0;
        Entity g_mainHandle = 0;
        std::uint32_t g_mainHorseId = census::kNoOwner;

        // Per tick: what was read of each entity.
        struct Probe
        {
            bool hasPos = false;
            float pos[3] = {};
            std::uint8_t lod = kNoLod;
            float metric = -1.0f;
            int pbit = -1;           // the engine's player bit (a horse: its rider's)
            bool playerAudio = false; // the weapon (mount) predicate answers player
            bool mainHorse = false;
        };
        std::unordered_map<std::uint32_t, Probe> g_tick;
        Probe g_liveProbe;
        bool g_liveProbed = false;
        float g_cam[3] = {};
        float g_player[3] = {};
        bool g_hasPlayer = false;

        std::uint64_t Wall() { return GetTickCount64(); }

        bool Play(const char* phase) { return std::strcmp(phase, "play") == 0; }
        const char* PhaseText(const char* phase) { return Play(phase) ? "play" : "record"; }

        void ToFloats(const Vector3& v, float out[3])
        {
            out[0] = v.x;
            out[1] = v.y;
            out[2] = v.z;
        }

        std::uint64_t AddressOf(Entity handle)
        {
            if (!handle || !replay::entities::Exists(handle))
                return 0;
            return reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(handle));
        }

        census::OwnerClass PedClassOf(std::uint32_t id, Entity handle, bool horse, bool player)
        {
            if (player)
                return census::OwnerClass::Player;
            if (horse)
                return census::OwnerClass::Horse;
            const auto found = g_classes.find(id);
            if (found != g_classes.end())
                return found->second;
            const census::OwnerClass cls = PED::IS_PED_HUMAN(handle) ? census::OwnerClass::Ped : census::OwnerClass::Animal;
            g_classes[id] = cls;
            return cls;
        }

        void BuildTable(Context& context, bool play, double clipMs)
        {
            g_table.clear();
            g_mainAddress = 0;
            g_mainHandle = 0;
            g_mainHorseId = census::kNoOwner;
            const Ped player = PLAYER::PLAYER_PED_ID();
            if (!play)
            {
                std::unordered_map<Entity, std::uint32_t> byHandle;
                for (const replay::recorder::RecordedEntity& entity : context.recorder.entities)
                {
                    const std::uint32_t id = entity.id.Packed();
                    Entry entry;
                    entry.handle = entity.handle;
                    entry.address = AddressOf(entity.handle);
                    if (!entry.address)
                        continue;
                    byHandle[entity.handle] = id;
                    if (entity.vehicle)
                        entry.cls = census::OwnerClass::Vehicle;
                    else
                    {
                        bool horse = false;
                        const auto cached = g_classes.find(id);
                        if (cached != g_classes.end())
                            horse = cached->second == census::OwnerClass::Horse;
                        else if (const replay::entity::CreatePacket* packet = context.clip.FindCreate(id))
                        {
                            horse = packet->kind == replay::entity::EntityKind::Horse;
                            if (horse)
                                g_classes[id] = census::OwnerClass::Horse;
                        }
                        entry.cls = PedClassOf(id, entity.handle, horse, entity.handle == player);
                    }
                    if (entity.handle == player)
                    {
                        g_mainId = id;
                        g_mainAddress = entry.address;
                        g_mainHandle = entity.handle;
                    }
                    g_table[id] = entry;
                }
                for (const replay::recorder::RecordedWeapon& weapon : context.recorder.weapons)
                {
                    Entry entry;
                    entry.handle = weapon.object;
                    entry.address = AddressOf(weapon.object);
                    entry.cls = census::OwnerClass::Weapon;
                    const auto holder = byHandle.find(weapon.parent);
                    if (holder != byHandle.end())
                        entry.holder = holder->second;
                    g_table[weapon.id.Packed()] = entry;
                }
                for (const replay::recorder::RecordedProp& prop : context.recorder.props)
                {
                    Entry entry;
                    entry.handle = prop.object;
                    entry.address = AddressOf(prop.object);
                    entry.cls = census::OwnerClass::Object;
                    g_table[prop.id.Packed()] = entry;
                }
                for (const replay::recorder::RecordedTrainCar& car : context.recorder.trainCars)
                {
                    Entry entry;
                    entry.handle = car.handle;
                    entry.address = AddressOf(car.handle);
                    entry.cls = census::OwnerClass::Train;
                    g_table[car.id.Packed()] = entry;
                }
                if (g_mainHandle)
                {
                    const Ped mount = PED::GET_MOUNT(player);
                    const auto found = mount ? byHandle.find(mount) : byHandle.end();
                    if (found != byHandle.end())
                        g_mainHorseId = found->second;
                }
                context.clip.mixProbe.mainId = g_mainId;
                return;
            }
            g_mainId = context.clip.mixProbe.mainId != census::kNoOwner ? context.clip.mixProbe.mainId
                                                                         : census::RecordedPlayerId();
            // The weapons' holders at this clip time, from the clip (the attach
            // record: WeaponPoint child -> ped parent).
            std::unordered_map<std::uint32_t, std::uint32_t> holderOf;
            context.clip.lifecycle.AttachmentsAt(clipMs > 0.0 ? static_cast<std::uint32_t>(clipMs) : 0u, g_attachments);
            for (const replay::entity::Attachment& attachment : g_attachments)
                if (attachment.kind == replay::entity::AttachmentKind::WeaponPoint)
                    holderOf[attachment.child.Packed()] = attachment.parent.Packed();
            for (const replay::playback::Puppet& puppet : context.puppets.list)
            {
                if (!puppet.clone)
                    continue;
                const std::uint32_t id = puppet.packet.id.Packed();
                Entry entry;
                entry.handle = puppet.clone;
                entry.address = AddressOf(puppet.clone);
                if (!entry.address)
                    continue;
                switch (puppet.packet.kind)
                {
                case replay::entity::EntityKind::Ped:
                case replay::entity::EntityKind::Horse:
                    entry.cls = PedClassOf(id, puppet.clone, puppet.packet.kind == replay::entity::EntityKind::Horse,
                        id == g_mainId);
                    break;
                case replay::entity::EntityKind::Vehicle: entry.cls = census::OwnerClass::Vehicle; break;
                case replay::entity::EntityKind::TrainCar: entry.cls = census::OwnerClass::Train; break;
                case replay::entity::EntityKind::Weapon:
                {
                    entry.cls = census::OwnerClass::Weapon;
                    const auto holder = holderOf.find(id);
                    if (holder != holderOf.end())
                        entry.holder = holder->second;
                    else if (puppet.attached && puppet.attachment.kind == replay::entity::AttachmentKind::WeaponPoint)
                        entry.holder = puppet.attachment.parent.Packed();
                    ++(entry.holder != census::kNoOwner ? g_weaponHolders : g_weaponNoHolder);
                    break;
                }
                default: entry.cls = census::OwnerClass::Object; break;
                }
                if (id == g_mainId)
                {
                    g_mainAddress = entry.address;
                    g_mainHandle = puppet.clone;
                }
                g_table[id] = entry;
            }
            if (g_mainId != census::kNoOwner)
            {
                const std::uint32_t mount = replay::playback::MountOf(g_mainId);
                if (mount)
                    g_mainHorseId = mount;
            }
        }

        bool PedLike(census::OwnerClass cls)
        {
            return cls == census::OwnerClass::Player || cls == census::OwnerClass::Ped || cls == census::OwnerClass::Animal ||
                cls == census::OwnerClass::Horse || cls == census::OwnerClass::LivePlayer;
        }

        void ReadPed(std::uint32_t id, std::uint64_t address, census::OwnerClass cls, bool play, Probe& probe)
        {
            if (!address || !PedLike(cls))
                return;
            void* ped = reinterpret_cast<void*>(address);
            std::uint8_t lod = kNoLod;
            float metric = -1.0f;
            if (replay::audio::PedAudioLod(ped, lod, metric))
            {
                probe.lod = lod <= 4 ? lod : kNoLod;
                probe.metric = metric;
            }
            if (cls == census::OwnerClass::Horse)
            {
                std::uint64_t linked = 0;
                probe.pbit = replay::audio::MountLinkIsPlayer(ped, linked);
                const std::uint64_t component = replay::audio::MountLinkComponent(ped);
                const bool published = replay::audio::MountAudioPublished(component);
                probe.playerAudio = probe.pbit == 1 || published;
                probe.mainHorse = id == g_mainHorseId || (linked && linked == g_mainAddress) || (play && published);
                return;
            }
            probe.pbit = replay::audio::PedAudioPlayerFlag(ped);
            probe.playerAudio = probe.pbit == 1 || replay::audio::PlayerAudioPublished(address);
        }

        const Probe& ProbeOf(std::uint32_t id, bool play)
        {
            const auto found = g_tick.find(id);
            if (found != g_tick.end())
                return found->second;
            Probe& probe = g_tick[id];
            const auto entry = g_table.find(id);
            if (entry == g_table.end() || !entry->second.handle || !replay::entities::Exists(entry->second.handle))
                return probe;
            ToFloats(ENTITY::GET_ENTITY_COORDS(entry->second.handle, TRUE, FALSE), probe.pos);
            probe.hasPos = true;
            ReadPed(id, entry->second.address, entry->second.cls, play, probe);
            return probe;
        }

        const Probe& LiveProbe()
        {
            if (g_liveProbed)
                return g_liveProbe;
            g_liveProbed = true;
            g_liveProbe = Probe{};
            const Ped player = PLAYER::PLAYER_PED_ID();
            if (g_hasPlayer)
            {
                std::memcpy(g_liveProbe.pos, g_player, sizeof(g_player));
                g_liveProbe.hasPos = true;
            }
            ReadPed(census::kNoOwner, AddressOf(player), census::OwnerClass::LivePlayer, true, g_liveProbe);
            return g_liveProbe;
        }

        Role RoleOf(const census::Owner& owner, std::uint32_t source, census::OwnerClass sourceClass, const Probe& probe)
        {
            if (owner.kind == census::OwnerKind::LivePlayer)
                return Role::LivePlayer;
            if (source != census::kNoOwner && source == g_mainId)
                return Role::Main;
            switch (sourceClass)
            {
            case census::OwnerClass::Player: return Role::Main;
            case census::OwnerClass::Horse: return probe.mainHorse ? Role::MainHorse : Role::Horse;
            case census::OwnerClass::Ped: return Role::Npc;
            case census::OwnerClass::Animal: return Role::Animal;
            case census::OwnerClass::Vehicle:
            case census::OwnerClass::Train: return Role::Vehicle;
            case census::OwnerClass::Object: return Role::Object;
            default: return Role::Other;
            }
        }

        std::string Fixed(double value)
        {
            if (value < 0.0)
                return "-";
            char text[32];
            std::snprintf(text, sizeof(text), "%.1f", value);
            return text;
        }

        std::string Xyz(const float v[3])
        {
            char text[96];
            std::snprintf(text, sizeof(text), "%.1f;%.1f;%.1f", v[0], v[1], v[2]);
            return text;
        }

        void ClosePass(std::ostream& log)
        {
            if (std::strcmp(g_phase, "idle") == 0)
                return;
            const char* phase = PhaseText(g_phase);
            for (const std::string& line : g_totals.Lines(phase))
                log << line << '\n';
            const replay::audio::PlayerAudioStats weapon = replay::audio::PlayerAudioSnapshot();
            const replay::audio::MountAudioStats mount = replay::audio::MountAudioSnapshot();
            log << "MIX_PASS_TOTAL,phase=" << phase << ",samples=" << g_totals.samples << ",world=" << g_totals.world
                << ",stored=" << g_stored << ",dropped=" << g_dropped << ",lines=" << g_lines.Lines()
                << ",suppressed=" << g_lines.Suppressed() << ",main=" << IdText(g_mainId)
                << ",queued_noted=" << g_requests.noted << ",queued_matched=" << g_requests.matched
                << ",queued_expired=" << g_requests.expired << ",queued_kinds=" << g_queuedKinds
                << ",weapon_holder_reads=" << g_weaponHolders << ",weapon_no_holder_reads=" << g_weaponNoHolder << '\n';
            // The player-bit sites hit in this pass (both detours count the
            // engine's own yes and the replay's).
            log << "MIX_PLAYER_AUDIO_TOTAL,phase=" << phase << ",weapon_hook=" << (weapon.installed ? 1 : 0)
                << ",calls=" << (weapon.calls - g_weaponBase.calls)
                << ",engine_true=" << (weapon.engineTrue - g_weaponBase.engineTrue)
                << ",overrides=" << (weapon.overrides - g_weaponBase.overrides)
                << ",sites=" << replay::audio::PlayerAudioSites(weapon, g_weaponBase)
                << ",mount_hook=" << (mount.installed ? 1 : 0) << ",mount_calls=" << (mount.calls - g_mountBase.calls)
                << ",mount_engine_true=" << (mount.engineTrue - g_mountBase.engineTrue)
                << ",mount_overrides=" << (mount.overrides - g_mountBase.overrides)
                << ",mount_sites=" << replay::audio::MountAudioSites(mount, g_mountBase) << '\n';
            g_phase = "idle";
        }

        void OpenPass(std::ostream& log, const char* phase)
        {
            g_phase = phase;
            g_totals.Clear();
            g_lines.SetPlay(Play(phase));
            g_sampleBudget.Reset();
            g_stored = 0;
            g_dropped = 0;
            g_weaponBase = replay::audio::PlayerAudioSnapshot();
            g_mountBase = replay::audio::MountAudioSnapshot();
            g_classes.clear();
            g_tableBuiltWall = 0;
            g_nextListenerWall = 0;
            g_secondSounds = 0;
            g_lastClipMs = -1.0;
            g_lastNormal = false;
            g_requests.Clear();
            replay::audio::CollectPlayRequestsSince(g_requestCursor, g_requestBatch); // older requests: not this pass
            g_queuedKinds = 0;
            g_weaponHolders = 0;
            g_weaponNoHolder = 0;
            log << "MIX_PASS,phase=" << PhaseText(phase) << ",event=open,budget_per_s=" << kMaxSamplesPerSecond << '\n';
        }

        void Listen(Context& context, const char* phase, double clipMs, bool normal, std::uint64_t wall)
        {
            if (wall < g_nextListenerWall)
                return;
            g_nextListenerWall = wall + 1000;
            Listener l;
            l.timeMs = clipMs > 0.0 ? static_cast<std::uint32_t>(clipMs) : 0u;
            std::memcpy(l.cam, g_cam, sizeof(g_cam));
            std::memcpy(l.player, g_player, sizeof(g_player));
            if (g_mainHandle && replay::entities::Exists(g_mainHandle))
            {
                ToFloats(ENTITY::GET_ENTITY_COORDS(g_mainHandle, TRUE, FALSE), l.main);
                l.hasMain = 1;
            }
            l.music = AUDIO::AUDIO_IS_MUSIC_PLAYING() != FALSE ? 1 : 0;
            if (normal)
            {
                if (Play(phase))
                    g_playListeners.push_back(l);
                else
                    context.clip.mixProbe.listeners.push_back(l);
            }
            const replay::logfmt::FormatScope format(context.log);
            replay::logfmt::DefaultFloat(context.log)
                << "MIX_LISTENER,phase=" << PhaseText(phase) << ",clip_ms=" << l.timeMs << ",normal=" << (normal ? 1 : 0)
                << ",cam=" << Xyz(l.cam) << ",player=" << Xyz(l.player) << ",main=" << (l.hasMain ? Xyz(l.main) : std::string("none"))
                << ",d_cam_main=" << (l.hasMain ? Fixed(Distance(l.cam, l.main)) : std::string("-"))
                << ",d_cam_player=" << Fixed(Distance(l.cam, l.player)) << ",main_id=" << IdText(g_mainId)
                << ",main_horse=" << IdText(g_mainHorseId) << ",music=" << static_cast<int>(l.music)
                << ",sounds=" << g_secondSounds << ",lines=" << g_lines.Lines() << ",suppressed=" << g_lines.Suppressed() << '\n';
            g_secondSounds = 0;
        }

        void LogMark(Context& context, std::uint32_t clipMs)
        {
            const std::uint32_t from = clipMs > kMarkWindowMs ? clipMs - kMarkWindowMs : 0u;
            const Comparison c = Compare(context.clip.mixProbe, g_play, g_playListeners, g_cover, from, clipMs);
            char extra[48];
            std::snprintf(extra, sizeof(extra), ",clip_ms=%u", clipMs);
            context.log << "MIX_MARK,clip_ms=" << clipMs << ",from_ms=" << from << ",rec_n=" << c.recSamples
                        << ",play_n=" << c.playSamples << ",rec_cam_main=" << Fixed(c.recCamMain)
                        << ",play_cam_main=" << Fixed(c.playCamMain) << ",rec_music=" << Fixed(c.recMusic)
                        << ",play_music=" << Fixed(c.playMusic) << ",recorded=" << (c.recorded ? 1 : 0) << '\n';
            for (const std::string& line : RowLines(c, "MIX_MARK_ROW", extra))
                context.log << line << '\n';
            for (const std::string& line : VerdictLines(c, "MIX_MARK_VERDICT", extra))
                context.log << line << '\n';
        }

        std::string Narrow(const std::wstring& text)
        {
            std::string out;
            for (const wchar_t c : text)
                out += c < 128 ? static_cast<char>(c) : '?';
            return out;
        }
    }

    void SetEnabled(bool enabled)
    {
        g_enabled = enabled;
        // The create-and-play request ring (read only: what the queue was
        // asked, with its caller frames) also while the observer is off.
        replay::audio::SetPlayRequestMixProbe(enabled);
    }
    bool Enabled() { return g_enabled; }

    void BeginPlayback(Context& context)
    {
        if (!g_enabled)
            return;
        g_play.clear();
        g_playListeners.clear();
        g_cover.Clear();
        g_playing = true;
        g_markSeen = replay::qa::kit::MarkPresses();
        context.log << "MIX_PLAYBACK,event=begin,recorded_samples=" << context.clip.mixProbe.samples.size()
                    << ",recorded_listeners=" << context.clip.mixProbe.listeners.size()
                    << ",recorded_main=" << IdText(context.clip.mixProbe.mainId) << '\n';
    }

    void Tick(Context& context, const char* phase, double clipMs, bool normal)
    {
        if (!g_enabled)
            return;
        std::ostream& log = context.log;
        const std::uint64_t wall = Wall();
        const bool play = Play(phase);
        if (std::strcmp(phase, g_phase) != 0)
        {
            ClosePass(log);
            OpenPass(log, phase);
        }
        g_lastTickWall = wall;
        if (wall - g_tableBuiltWall >= 250 || !g_tableBuiltWall)
        {
            BuildTable(context, play, clipMs);
            g_tableBuiltWall = wall;
        }
        g_tick.clear();
        g_liveProbed = false;
        ToFloats(CAM::GET_FINAL_RENDERED_CAM_COORD(), g_cam);
        const Ped player = PLAYER::PLAYER_PED_ID();
        g_hasPlayer = player && replay::entities::Exists(player);
        if (g_hasPlayer)
            ToFloats(ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE), g_player);

        {
            const std::uint64_t nowUs = replay::clock::NowUs();
            replay::audio::CollectPlayRequestsSince(g_requestCursor, g_requestBatch);
            for (const replay::audio::SoundRequest& request : g_requestBatch)
                if (!request.flags[0]) // flags[0] = 1: a persistent create, not the queue
                    g_requests.Note(request.audioEntity, request.soundRef, nowUs, request.callers, 4);
            g_requests.Prune(nowUs);
        }

        if (play && normal)
            g_cover.Note(g_lastNormal && g_lastClipMs >= 0.0 && clipMs >= g_lastClipMs ? g_lastClipMs : clipMs, clipMs);
        g_lastClipMs = clipMs;
        g_lastNormal = normal;

        for (const census::TickRow& tick : census::Rows())
        {
            const census::Owner& owner = tick.owner;
            if (owner.kind == census::OwnerKind::None)
            {
                ++g_totals.world;
                continue;
            }
            ++g_secondSounds;
            const census::Row& row = tick.row;
            const bool weapon = owner.cls == census::OwnerClass::Weapon || owner.base == census::AudioBase::WeaponAudio;
            const bool live = owner.kind == census::OwnerKind::LivePlayer;
            std::uint32_t source = owner.id;
            census::OwnerClass sourceClass = owner.cls;
            if (weapon)
            {
                const auto entry = g_table.find(owner.id);
                source = entry != g_table.end() ? entry->second.holder : census::kNoOwner;
                const auto holder = source != census::kNoOwner ? g_table.find(source) : g_table.end();
                sourceClass = holder != g_table.end() ? holder->second.cls : census::OwnerClass::Weapon;
            }
            else if (!live)
            {
                const auto entry = g_table.find(owner.id);
                if (entry != g_table.end() && entry->second.cls != census::OwnerClass::World)
                    sourceClass = entry->second.cls;
            }
            Probe probe = live ? LiveProbe()
                : source != census::kNoOwner ? ProbeOf(source, play)
                                             : Probe{};
            // The distance is the owner's own (a weapon: the object in the hand).
            const Probe& at = live ? probe : ProbeOf(owner.id, play);
            const bool hasPos = at.hasPos || probe.hasPos;
            const float* pos = at.hasPos ? at.pos : probe.pos;

            Sample s;
            s.timeMs = tick.clipMs > 0.0 ? static_cast<std::uint32_t>(tick.clipMs) : 0u;
            s.owner = owner.id;
            s.source = source;
            s.ref = row.ref;
            const Role role = RoleOf(owner, source, sourceClass, probe);
            s.role = static_cast<std::uint8_t>(role);
            const bool horse = role == Role::Horse || role == Role::MainHorse;
            Kind kind = KindOf(weapon, horse, row.ref, row.callers, census::kRowCallers);
            if (kind == Kind::Generic && row.audio)
            {
                soundowner::Family family = soundowner::Family::Owned;
                Kind queued = Kind::Generic;
                if (g_requests.Take(row.audio, row.ref, family) && KindOfFamily(family, horse, queued))
                {
                    kind = queued;
                    ++g_queuedKinds;
                }
            }
            s.kind = static_cast<std::uint8_t>(kind);
            s.lod = probe.lod;
            if (probe.pbit >= 0)
                s.flags |= kFlagBitKnown | (probe.pbit == 1 ? kFlagPlayerBit : 0);
            s.flags |= probe.playerAudio ? kFlagPlayerAudio : 0;
            s.flags |= row.sound ? 0 : kFlagRefused;
            s.flags |= row.mine ? kFlagMine : 0;
            if (hasPos)
            {
                s.dCamDm = ToDm(Distance(pos, g_cam));
                if (g_hasPlayer)
                    s.dPlayerDm = ToDm(Distance(pos, g_player));
            }
            if (probe.lod != kNoLod)
                s.dListenerDm = ListenerDm(probe.metric);
            g_totals.Add(s);
            if (tick.normal)
            {
                std::vector<Sample>& store = play ? g_play : context.clip.mixProbe.samples;
                if (g_sampleBudget.Take(s.timeMs) && (!play || g_play.size() < kPlaySamplesMax))
                {
                    store.push_back(s);
                    ++g_stored;
                }
                else
                {
                    ++g_dropped;
                    if (!play)
                        ++context.clip.mixProbe.dropped;
                }
            }
            if (g_lines.Take(s.GetKind(), wall))
            {
                char line[512];
                std::snprintf(line, sizeof(line),
                    "MIX_SOUND,phase=%s,clip_ms=%u,owner=%s,src=%s,role=%s,kind=%s,ref=0x%llx,d_cam=%s,d_player=%s,d_lst=%s,"
                    "lmet=%.0f,lod=%d,pbit=%d,player_audio=%d,refused=%d,mine=%d,normal=%d",
                    PhaseText(phase), s.timeMs, live ? "live_player" : IdText(owner.id).c_str(), IdText(source).c_str(),
                    RoleName(role, play), KindName(s.GetKind()), static_cast<unsigned long long>(row.ref),
                    Fixed(FromDm(s.dCamDm)).c_str(), Fixed(FromDm(s.dPlayerDm)).c_str(), Fixed(FromDm(s.dListenerDm)).c_str(),
                    static_cast<double>(probe.metric), probe.lod == kNoLod ? -1 : static_cast<int>(probe.lod), probe.pbit,
                    probe.playerAudio ? 1 : 0, row.sound ? 0 : 1, row.mine ? 1 : 0, tick.normal ? 1 : 0);
                log << line << '\n';
            }
        }

        Listen(context, phase, clipMs, normal, wall);

        if (play && g_playing)
        {
            const std::uint64_t marks = replay::qa::kit::MarkPresses();
            if (marks != g_markSeen)
            {
                g_markSeen = marks;
                LogMark(context, clipMs > 0.0 ? static_cast<std::uint32_t>(clipMs) : 0u);
            }
        }
    }

    void Idle(Context& context)
    {
        if (!g_enabled || std::strcmp(g_phase, "idle") == 0)
            return;
        if (Wall() - g_lastTickWall < 300)
            return;
        // A playback closes in EndPlayback (its comparison needs the pass).
        if (Play(g_phase) && g_playing)
            return;
        ClosePass(context.log);
    }

    std::string EndPlayback(Context& context)
    {
        if (!g_enabled || !g_playing)
            return std::string();
        g_playing = false;
        std::ostream& log = context.log;
        if (Play(g_phase))
            ClosePass(log);
        const Comparison c = Compare(context.clip.mixProbe, g_play, g_playListeners, g_cover);
        log << "MIX_COMPARE_SUMMARY,recorded=" << (c.recorded ? 1 : 0) << ",covered_s=" << Fixed(c.coveredSeconds)
            << ",played_s=" << Fixed(c.playSeconds) << ",rec_n=" << c.recSamples << ",play_n=" << c.playSamples
            << ",rec_cam_main=" << Fixed(c.recCamMain) << ",play_cam_main=" << Fixed(c.playCamMain)
            << ",rec_music=" << Fixed(c.recMusic) << ",play_music=" << Fixed(c.playMusic) << ",swap_pairs=" << c.swaps.size()
            << '\n';
        for (const std::string& line : RowLines(c, "MIX_COMPARE"))
            log << line << '\n';
        std::size_t shown = 0;
        for (const SwapPair& pair : c.swaps)
        {
            if (++shown > 20)
                break;
            log << "MIX_VARIANT,role=" << RoleName(pair.role, true) << ",kind=" << KindName(pair.kind)
                << ",owner=" << IdText(pair.owner) << ",rec_ref=" << RefText(pair.recRef)
                << ",play_ref=" << RefText(pair.playRef) << ",count=" << pair.count << '\n';
        }
        for (const std::string& line : VerdictLines(c))
            log << line << '\n';

        const std::string section = FormatSection(c);
        // ReplayDebug\<clip>.sound-mix.txt (the Shift+F1 report takes it with the clip).
        const std::wstring clipPath = census::CurrentClipPath();
        std::wstring stem = L"no-file";
        if (!clipPath.empty())
        {
            const std::size_t slash = clipPath.find_last_of(L"\\/");
            const std::size_t dot = clipPath.find_last_of(L'.');
            const std::size_t begin = slash == std::wstring::npos ? 0 : slash + 1;
            stem = clipPath.substr(begin, dot != std::wstring::npos && dot > begin ? dot - begin : std::wstring::npos);
        }
        replay::storage::Folder(L"ReplayDebug"); // the base folder (storage_paths.h)
        const std::wstring path = replay::storage::Path(L"ReplayDebug\\") + stem + L".sound-mix.txt";
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (file)
        {
            std::time_t t = std::time(nullptr);
            std::tm local{};
            localtime_s(&local, &t);
            char when[32]{};
            std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &local);
            file << "Redemption Editor " << replay::build::kVersion << " mix probe report\n"
                 << "Clip: " << Narrow(stem) << "\nWritten: " << when << "\n\n"
                 << section;
        }
        log << "MIX_REPORT,path=" << Narrow(path) << ",written=" << (file ? 1 : 0) << '\n';
        g_play.clear();
        g_play.shrink_to_fit();
        g_playListeners.clear();
        return section;
    }
}
