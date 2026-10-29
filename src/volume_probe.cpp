#include "volume_probe.h"
#include "volume_probe_core.h"

#include "audio_observer.h"
#include "entity_cache.h"
#include "game_listener_core.h"
#include "guarded_memory.h"
#include "mix_probe_core.h"
#include "mod_clock.h"
#include "ped_audio_state.h" // wvol names a PED_VARIABLES float
#include "replay_playback.h"
#include "sound_census.h"
#include "sound_generic.h" // IsPlayerSound

#include <windows.h>

#include "main.h"
#include "natives.h"

#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <ostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The volume probe (volume_probe.h). Read only: guarded reads of the fields of
// volume_probe_core.h after its code sites matched, natives that read, and the
// census owner table. Script thread only.
//
// The walk follows each bucket's voice list (every voice the audio frame
// processes), splits the chain's residual into mix groups and mixer scenes,
// reads the quad speaker levels of the unpositioned beds, classes the voices
// of the global audio entities (wind, weather, ambience, emitters, particles,
// script, music) and logs the controls of the wind and the speech duck per
// window (VOLUME_PROBE_ENV).

namespace replay::volumeprobe
{
    namespace mp = replay::mixprobe;

    namespace
    {
        // Every read of the probe refuses an address below 4 GB without
        // touching it, the rule the audio observer, the census and the
        // capture and sound fx probes already follow (guarded_memory.h
        // kGameHeapLowest). The probe reads, from the script fiber, pointers
        // the audio thread is writing (the voice state E+0x108, the mixer
        // scene lists, the settings blocks, the speech lines): a torn or stale
        // one below 4 GB would fault, and a fault on the script fiber runs
        // every vectored handler of the process first (another mod's CLR
        // handler can overflow the fiber's stack on one). The image and the
        // game's heaps live above 4 GB, so nothing valid is refused. This
        // shadows replay::mem for the whole file.
        namespace mem
        {
            inline bool Copy(void* out, std::uintptr_t source, std::size_t size)
            {
                return replay::mem::Copy(out, source, size, replay::mem::kGameHeapLowest);
            }
            template <typename T>
            inline bool Read(std::uintptr_t address, T& out)
            {
                return Copy(&out, address, sizeof(T));
            }
        }

        Config g_cfg;

        enum class Proof : std::uint8_t { Unknown, Proven, Failed };
        Proof g_proof = Proof::Unknown;
        std::uintptr_t g_base = 0;
        std::uintptr_t g_imageEnd = 0;
        bool g_part[kPartCount] = {};
        std::uint32_t g_entityMask = 0;         // proven kGlobalEntities
        std::uint32_t g_flagHashes[kAudioFlagCount] = {};

        const char* g_phase = "idle";
        std::uint32_t g_pass = 0;
        Segments g_segments;
        std::uint32_t g_windowSeg = 0;
        std::uint64_t g_lastTickWall = 0;
        std::uint64_t g_nextSnapWall = 0;
        Window g_window;
        EnvWindow g_env;
        std::uint32_t g_windowScenes = 0;     // mixer scenes in the last snapshot of the window
        std::string g_windowLowScenes;
        PassTotals g_totals;

        bool PartOn(Part part) { return g_part[static_cast<std::uint8_t>(part)]; }

        // Classes learnt at a sound's birth (census rows): top sound -> class.
        struct Born
        {
            Cls cls = Cls::World;
            std::uint32_t owner = census::kNoOwner;
            bool live = false;
            std::uint16_t settings = kNoIndex;
            std::uint64_t seenWall = 0;
            std::uint64_t ref = 0;  // the create's ref (VOLUME_PROBE_SRC)
            bool mine = false;      // ... made under census::ModScope
        };
        std::unordered_map<std::uint64_t, Born> g_born;
        constexpr std::uint64_t kBornKeepMs = 10000; // a row neither born nor matched for this long goes
        std::uint64_t g_nextBornPruneWall = 0;

        // Speech line sounds (line data +0xC0, layers +0x198): sound -> line.
        struct SpeechSound
        {
            Cls cls = Cls::SpeechAmbient;
            std::uint32_t source = 0;
            std::uint32_t type = 0;
            std::string who;
            std::uint64_t seenWall = 0;
        };
        std::unordered_map<std::uint64_t, SpeechSound> g_speech;

        // The create-and-play requests' caller families (the ped steps reach
        // the creator through the queue; mix_probe_core.h RequestFamilies).
        mp::RequestFamilies g_requests;
        std::uint64_t g_requestCursor = 0;
        std::vector<replay::audio::SoundRequest> g_requestBatch;

        // The loudest voice (effective power) of each class in the open window.
        struct TopVoice
        {
            bool set = false;
            Voice v;
            Stages s;
            std::uint64_t sound = 0;
            std::uint64_t top = 0;
            std::uint32_t owner = census::kNoOwner;
            bool live = false;
            std::uint8_t mg[kEnvMixGroupCount] = {};
            float quad[4] = {};
            std::string scenes;
            std::uint16_t listIndex = 0;
            std::uint16_t voiceIndex = 0;
            std::string who;
            std::string wvol = "-";
            std::uint64_t ref = 0;
            const char* src = "-";
        };
        TopVoice g_top[kClsCount];

        // Who made each owned voice of the window, by maker and ref. "player":
        // the generic player's (soundgeneric::IsPlayerSound); "mod": another
        // create of the mod (census ModScope); "own": the engine made it for a
        // puppet (its own audio entity); "-": no census row.
        enum class Src : std::uint8_t { Player, Mod, Own, Unknown, Count };
        const char* SrcName(Src s)
        {
            switch (s)
            {
            case Src::Player: return "player";
            case Src::Mod: return "mod";
            case Src::Own: return "own";
            default: return "-";
            }
        }
        struct SrcAgg
        {
            std::uint32_t n = 0;
            double power = 0.0;
            std::int32_t category = -1;
            float maxDb = -200.0f;
            float volDb = 0.0f;
            float distM = 0.0f;
            std::uint32_t owner = census::kNoOwner;
        };
        std::unordered_map<std::uint64_t, SrcAgg> g_srcRefs[static_cast<std::size_t>(Src::Count)]; // owned voices, window
        Src SrcOf(std::uint64_t top, std::uint64_t& ref)
        {
            ref = 0;
            const auto born = g_born.find(top);
            if (born != g_born.end())
                ref = born->second.ref;
            if (replay::soundgeneric::IsPlayerSound(top))
                return Src::Player;
            if (born == g_born.end())
                return Src::Unknown;
            return born->second.mine ? Src::Mod : Src::Own;
        }
        WrapperVar ReadWrapperVar(std::uint64_t top, std::uint32_t topVtRva);

        // Evidence counters (reset per pass).
        struct Counters
        {
            std::uint64_t snapshots = 0;
            std::uint64_t voices = 0;
            std::uint64_t poolInsane = 0;
            std::uint64_t voiceUnreadable = 0;
            std::uint64_t climbBroken = 0;
            std::uint64_t settingsMissing = 0;
            std::uint64_t bornClassified = 0;
            std::uint64_t speechClassified = 0;
            std::uint64_t ownerClassified = 0;
            std::uint64_t entityClassified = 0;
            std::uint64_t world = 0;
            std::uint64_t listVoices = 0;
            std::uint64_t listPartial = 0;
            std::uint64_t backlinkBad = 0;
            std::uint64_t linkMixed = 0;   // kept, E+0x242 = 0xFFFF (mixed this frame)
            std::uint64_t linkPending = 0; // kept, E+0x242 = i (registered, not mixed yet)
            std::uint64_t listDup = 0;     // a sound slot the bucket walk took already
            std::uint64_t listNotEnv = 0;
            std::uint64_t listOutOfRange = 0;
            std::uint64_t listInsane = 0;
            std::uint64_t noVoice = 0;
            std::uint64_t groupUnknown = 0;
            std::uint64_t listRetried = 0; // bucket walks repeated (the list moved)
            std::uint64_t listGrowing = 0; // buckets committed unsettled
            double walkUsTotal = 0.0;
            double walkUsMax = 0.0;
        };
        Counters g_counters;
        double g_windowWalkUs = 0.0;
        double g_windowWalkMax = 0.0;

        std::uint64_t Wall() { return GetTickCount64(); }

        bool Play(const char* phase) { return std::strcmp(phase, "play") == 0; }

        bool Prove(std::ostream& log)
        {
            if (g_proof != Proof::Unknown)
                return g_proof == Proof::Proven;
            g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            g_proof = Proof::Proven;
            for (bool& p : g_part)
                p = true;
            const char* failed = nullptr;
            std::uint32_t failedRva = 0;
            for (const Site& site : kSites)
            {
                std::uint8_t code[16]{};
                if (!PartOn(site.part))
                    continue;
                if (!g_base || !mem::Copy(code, g_base + site.rva, site.size) || !SiteMatches(site, code))
                {
                    if (site.part == Part::Walk)
                    {
                        failed = site.name;
                        failedRva = site.rva;
                        break;
                    }
                    g_part[static_cast<std::uint8_t>(site.part)] = false;
                    log << "VOLUME_PROBE_CHECK,ok=0,part=" << PartName(site.part) << ",site=" << site.name << ",rva=0x"
                        << std::hex << site.rva << std::dec << ",note=part_disabled\n";
                }
            }
            if (!failed)
            {
                for (const replay::gamelistener::Site& site : replay::gamelistener::kSites)
                {
                    std::uint8_t code[16]{};
                    if (!mem::Copy(code, g_base + site.rva, site.size) || !replay::gamelistener::SiteMatches(site, code))
                    {
                        failed = site.name;
                        failedRva = site.rva;
                        break;
                    }
                }
            }
            // The environment sound's vtable: its update slot and the sound
            // vtables' slot 1 (relocated pointers, read live).
            std::uint64_t play = 0, slot1 = 0;
            if (!failed && (!mem::Read(g_base + kEnvVtableRva + 0x50, play) || play != g_base + kEnvPlayRva ||
                               !mem::Read(g_base + kEnvVtableRva + 0x08, slot1) || slot1 != g_base + kSoundSlot1Rva))
            {
                failed = "env_vtable";
                failedRva = kEnvVtableRva;
            }
            if (failed)
            {
                g_proof = Proof::Failed;
                log << "VOLUME_PROBE_CHECK,ok=0,site=" << failed << ",rva=0x" << std::hex << failedRva << std::dec
                    << ",note=probe_disabled\n";
                return false;
            }
            // The image's end (the RVA of an entity or vtable).
            std::int32_t lfanew = 0;
            std::uint32_t sizeOfImage = 0;
            if (mem::Read(g_base + 0x3C, lfanew) && lfanew > 0 && lfanew < 0x1000 &&
                mem::Read(g_base + static_cast<std::uint32_t>(lfanew) + 0x50, sizeOfImage) && sizeOfImage > 0x6000000 &&
                sizeOfImage < 0x20000000)
                g_imageEnd = g_base + sizeOfImage;
            else
                g_imageEnd = g_base + 0x8000000; // the image is about 0x6000000; a bound for RVA text only
            // The global audio entities: the vtable their constructor writes.
            g_entityMask = 0;
            if (PartOn(Part::Entities))
            {
                for (std::size_t n = 0; n < kGlobalEntityCount; ++n)
                {
                    std::uint64_t vt = 0;
                    if (mem::Read(g_base + kGlobalEntities[n].rva, vt) && vt == g_base + kGlobalEntities[n].vtableRva)
                        g_entityMask |= 1u << n;
                }
            }
            // The audio flags' name hashes (.rdata).
            if (PartOn(Part::Env) && !mem::Copy(g_flagHashes, g_base + kAudioFlagTableRva, sizeof(g_flagHashes)))
                std::memset(g_flagHashes, 0, sizeof(g_flagHashes));
            float fade = 0.0f;
            const bool fadeOk = mem::Read(g_base + kFadeScaleRva, fade) && std::fabs(fade - kFadeScale) < 1e-6f;
            if (PartOn(Part::Split) && !fadeOk)
            {
                g_part[static_cast<std::uint8_t>(Part::Split)] = false;
                log << "VOLUME_PROBE_CHECK,ok=0,part=split,site=fade_scale_value,rva=0x" << std::hex << kFadeScaleRva
                    << std::dec << ",note=part_disabled\n";
            }
            log << "VOLUME_PROBE_CHECK,ok=1,sites=" << kSiteCount << ",listener_sites="
                << (sizeof(replay::gamelistener::kSites) / sizeof(replay::gamelistener::kSites[0]))
                << ",env_vtable=0x" << std::hex << kEnvVtableRva << std::dec << ",parts=";
            for (std::uint8_t n = 0; n < kPartCount; ++n)
                log << (n ? ";" : "") << PartName(static_cast<Part>(n)) << ':' << (g_part[n] ? 1 : 0);
            log << ",entities=";
            for (std::size_t n = 0; n < kGlobalEntityCount; ++n)
                log << (n ? ";" : "") << kGlobalEntities[n].name << ':' << ((g_entityMask >> n) & 1u);
            log << ",walk=voice_list,sample_ms=" << g_cfg.sampleMs << ",window_ms=" << g_cfg.windowMs
                << ",categories=" << g_cfg.categories << ",top=" << (g_cfg.top ? 1 : 0) << ",world=" << g_cfg.world
                << ",env=" << (g_cfg.env ? 1 : 0) << ",census=" << (census::Enabled() ? 1 : 0) << '\n';
            return true;
        }

        // ------------------------------------------------ classification

        BornKind BornKindOf(mp::Kind kind)
        {
            switch (kind)
            {
            case mp::Kind::Shot: return BornKind::Shot;
            case mp::Kind::Footstep: return BornKind::Footstep;
            case mp::Kind::HorseStep: return BornKind::HorseStep;
            case mp::Kind::Movement: return BornKind::Movement;
            case mp::Kind::Speech: return BornKind::Speech;
            case mp::Kind::Melee: return BornKind::Melee;
            default: return BornKind::Generic;
            }
        }

        void NoteBirths(std::uint64_t wall)
        {
            const std::uint64_t nowUs = replay::clock::NowUs();
            replay::audio::CollectPlayRequestsSince(g_requestCursor, g_requestBatch);
            for (const replay::audio::SoundRequest& request : g_requestBatch)
                if (!request.flags[0]) // a queued create-and-play, not a persistent create
                    g_requests.Note(request.audioEntity, request.soundRef, nowUs, request.callers, 4);
            g_requests.Prune(nowUs);

            for (const census::TickRow& tick : census::Rows())
            {
                const census::Row& row = tick.row;
                if (!row.sound)
                    continue;
                const census::Owner& owner = tick.owner;
                const bool owned = owner.kind != census::OwnerKind::None;
                const bool weapon = owned &&
                    (owner.cls == census::OwnerClass::Weapon || owner.base == census::AudioBase::WeaponAudio);
                const bool horse = owned && owner.cls == census::OwnerClass::Horse;
                mp::Kind kind = mp::KindOf(weapon, horse, row.ref, row.callers, census::kRowCallers);
                if (kind == mp::Kind::Generic && row.audio)
                {
                    soundowner::Family family = soundowner::Family::Owned;
                    mp::Kind queued = mp::Kind::Generic;
                    if (g_requests.Take(row.audio, row.ref, family) && mp::KindOfFamily(family, horse, queued))
                        kind = queued;
                }
                Born b;
                b.cls = ClsOfBorn(BornKindOf(kind), owned, horse);
                b.owner = owned ? owner.id : census::kNoOwner;
                b.live = owner.kind == census::OwnerKind::LivePlayer;
                mem::Read(row.sound + kSoundSettingsIndex, b.settings);
                b.seenWall = wall;
                b.ref = row.ref;
                b.mine = row.mine;
                g_born[row.sound] = b;
            }
            // A born row names a pool slot, and the pool hands a freed slot
            // (and, first fit, often its freed settings index too) to the next
            // sound of the bucket: a later sound the census did not see (a
            // dropped row, a creator it does not hook) at the same slot and
            // settings would take the old family. Rows neither born nor matched
            // by a voice for kBornKeepMs go (Classify refreshes the ones that
            // still play).
            if (wall >= g_nextBornPruneWall)
            {
                g_nextBornPruneWall = wall + 1000;
                for (auto at = g_born.begin(); at != g_born.end();)
                    at = wall - at->second.seenWall > kBornKeepMs ? g_born.erase(at) : std::next(at);
            }
            if (g_born.size() > 20000)
                g_born.clear();
        }

        // The speech lines of one ped: their sounds into g_speech.
        void NoteSpeechPed(Entity handle, const std::string& who, std::uint64_t wall)
        {
            if (!handle || !replay::entities::Exists(handle) || !replay::entities::IsPed(handle))
                return;
            const std::uint64_t speech = replay::audio::SpeechEntity(replay::entities::AddressOf(handle));
            if (!speech)
                return;
            std::uint64_t list = 0;
            std::uint16_t count = 0;
            if (!mem::Read(speech + 0x78, list) || !list || !mem::Read(speech + 0x80, count) || count == 0 || count > 64)
                return;
            for (std::uint16_t n = 0; n < count; ++n)
            {
                std::uint64_t line = 0, data = 0;
                if (!mem::Read(list + n * 8u, line) || !line || !mem::Read(line + 0x1D8, data) || !data)
                    continue;
                std::uint32_t source = 0, type = 0;
                mem::Read(line + 0x218, source);
                mem::Read(data + 0xE0, type);
                std::uint64_t sounds[1 + kLineDataLayerCount]{};
                mem::Read(data + kLineDataSound, sounds[0]);
                mem::Copy(&sounds[1], data + kLineDataLayers, sizeof(std::uint64_t) * kLineDataLayerCount);
                for (const std::uint64_t sound : sounds)
                {
                    if (!sound)
                        continue;
                    SpeechSound& s = g_speech[sound];
                    s.cls = SpeechCls(source, type);
                    s.source = source;
                    s.type = type;
                    s.who = who;
                    s.seenWall = wall;
                }
            }
        }

        // Rebuilt right before every snapshot, with only the sounds the
        // speakers' lines hold NOW. The line's reference is cleared when the
        // sound is freed (0x2FA10C0 passes &data+0xC0 to the creator
        // 0x2C2AA5C), and the pool gives that slot to the next sound of the
        // bucket, so a stale table would count any sound born there (a step, a
        // hoof, an ambience loop) as speech_script or speech_ambient.
        void NoteSpeech(Context& context, bool play, std::uint64_t wall)
        {
            g_speech.clear();
            std::unordered_set<Entity> done;
            const auto one = [&](Entity handle, const std::string& who) {
                if (handle && done.insert(handle).second)
                    NoteSpeechPed(handle, who, wall);
            };
            if (play)
            {
                for (const replay::playback::Puppet& puppet : context.puppets.list)
                    if (puppet.clone && (puppet.packet.kind == replay::entity::EntityKind::Ped ||
                                            puppet.packet.kind == replay::entity::EntityKind::Horse))
                        one(puppet.clone, replay::entity::IdText(puppet.packet.id));
            }
            else
            {
                for (const replay::recorder::RecordedEntity& entity : context.recorder.entities)
                    if (!entity.vehicle)
                        one(entity.handle, replay::entity::IdText(entity.id));
            }
            one(PLAYER::PLAYER_PED_ID(), "live_player");
        }

        // What a snapshot knows about the global entities' sounds.
        struct Known
        {
            std::uint64_t wind[kWindSlotCount] = {};
            std::uint64_t musicMgr = 0;
            std::uint64_t music[kMusicTrackCount] = {};
        };

        bool OneOf(const std::uint64_t* list, std::size_t count, std::uint64_t value)
        {
            if (!value)
                return false;
            for (std::size_t n = 0; n < count; ++n)
                if (list[n] == value)
                    return true;
            return false;
        }

        Cls Classify(std::uint64_t top, std::uint16_t settings, std::uint64_t entity, std::uint64_t tracker,
            const Known& known, std::uint64_t wall, Counters& c, std::uint32_t& owner, bool& live, std::string& who)
        {
            owner = census::kNoOwner;
            live = false;
            who.clear();
            const auto speech = g_speech.find(top);
            if (speech != g_speech.end())
            {
                ++c.speechClassified;
                who = speech->second.who;
                return speech->second.cls;
            }
            if (OneOf(known.wind, kWindSlotCount, top))
            {
                ++c.entityClassified;
                return Cls::Wind;
            }
            if (OneOf(known.music, kMusicTrackCount, top) || (known.musicMgr && entity == known.musicMgr))
            {
                ++c.entityClassified;
                return Cls::Music;
            }
            if (entity && entity >= g_base && entity < g_imageEnd)
            {
                const Cls byEntity = ClsOfEntityRva(static_cast<std::uint32_t>(entity - g_base), g_entityMask);
                if (byEntity != Cls::World)
                {
                    ++c.entityClassified;
                    return byEntity;
                }
            }
            const auto born = g_born.find(top);
            if (born != g_born.end() && born->second.settings == settings)
            {
                born->second.seenWall = wall;
                ++c.bornClassified;
                owner = born->second.owner;
                live = born->second.live;
                return born->second.cls;
            }
            census::Owner o;
            if ((entity || tracker) && census::OwnerOf(entity, tracker, o) && o.kind != census::OwnerKind::None)
            {
                ++c.ownerClassified;
                owner = o.id;
                live = o.kind == census::OwnerKind::LivePlayer;
                if (o.cls == census::OwnerClass::Weapon || o.base == census::AudioBase::WeaponAudio)
                    return Cls::Shot;
                if (o.cls == census::OwnerClass::Horse)
                    return Cls::Horse;
                return Cls::Owned;
            }
            ++c.world;
            return Cls::World;
        }

        // ------------------------------------------------ the walk

        struct Listener
        {
            bool ok = false;
            std::uint32_t points = 0;
            float pos[replay::gamelistener::kMaxVolumePoints][3] = {};
        };

        Listener ReadListener()
        {
            Listener l;
            const std::uintptr_t env = g_base + replay::gamelistener::kEnvironmentRva;
            std::uint8_t current = 0xFF;
            if (!mem::Read(env + replay::gamelistener::kCurrentIndex, current) ||
                current >= replay::gamelistener::kListenerCount)
                return l;
            const std::uintptr_t desc = env + replay::gamelistener::ListenerOffset(current);
            std::uint32_t points = 0;
            if (!mem::Read(desc + replay::gamelistener::kVolumePointCount, points) || points == 0 ||
                points > replay::gamelistener::kMaxVolumePoints)
                return l;
            for (std::uint32_t j = 0; j < points; ++j)
            {
                float p[4]{};
                if (!mem::Copy(p, desc + replay::gamelistener::kVolumePointPos + j * replay::gamelistener::kVolumePointStride,
                        sizeof(p)) ||
                    !Finite3(p))
                    return l;
                std::memcpy(l.pos[j], p, sizeof(float) * 3);
            }
            l.points = points;
            l.ok = !Zero3(l.pos[0]);
            return l;
        }

        float NearestPoint(const Listener& l, const float pos[3])
        {
            float best = -1.0f;
            for (std::uint32_t j = 0; j < l.points; ++j)
            {
                const float d = Dist3(l.pos[j], pos);
                if (best < 0.0f || d < best)
                    best = d;
            }
            return best;
        }

        void ToFloats(const Vector3& v, float out[3])
        {
            out[0] = v.x;
            out[1] = v.y;
            out[2] = v.z;
        }

        // One requested settings block, parsed once per snapshot.
        struct SettingsInfo
        {
            bool ok = false;
            float reqVolumeDb = 0.0f, postDb = 0.0f, wet = 0.0f, dry = 1.0f;
            float pos[4] = {};
            std::uint8_t posMode = 0;
            bool quad = false;
            float open = -1.0f;
            std::int16_t groupIds[kGroupSlots][2] = {};
            std::uint8_t fade[kGroupSlots] = {};
        };

        // A top sound, read once per snapshot.
        struct TopInfo
        {
            std::uint16_t settings = kNoIndex; // +0xD6
            std::uint64_t tracker = 0;
            std::uint64_t entity = 0;
            std::uint64_t vtable = 0;
        };

        // The snapshot's caches.
        struct Caches
        {
            std::uint64_t catTable = 0;
            std::unordered_map<std::int32_t, float> catDb;       // category -> dB (NaN unreadable)
            std::uint64_t groupBase = 0, groupStride = 0;
            std::uint32_t groupCount = 0;
            std::vector<std::uint32_t> groupBits;
            std::unordered_map<std::uint32_t, std::uint64_t> groupRemap; // group -> remap table (0 none)
            std::unordered_map<std::uint32_t, std::int16_t> remapEntry;  // group << 16 | category -> remapped (-2 unreadable)
            bool groupsOk = false;
            std::vector<SceneInfo> scenes;
            std::unordered_map<std::uint16_t, std::size_t> sceneById;
            std::unordered_map<std::uint64_t, SettingsInfo> settings; // bucket << 32 | index
            std::unordered_map<std::uint64_t, TopInfo> tops;
            std::unordered_map<std::uint64_t, std::uint64_t> climb;   // bucket << 32 | parent slot -> top (0 broken)
        };

        bool CatDb(Caches& c, std::int32_t category, float& db)
        {
            if (!c.catTable || category < 0 || category >= kMaxCategory)
                return false;
            const auto at = c.catDb.find(category);
            if (at != c.catDb.end())
            {
                db = at->second;
                return std::isfinite(db);
            }
            float v = 0.0f;
            const bool ok = mem::Read(c.catTable + static_cast<std::uint64_t>(category) * kCategoryStride + kCategoryVolume, v) &&
                std::isfinite(v);
            c.catDb[category] = ok ? v : NAN;
            db = v;
            return ok;
        }

        void ReadGroups(Caches& c)
        {
            std::uint64_t pool = 0;
            if (!mem::Read(g_base + kGroupPoolRva, pool) || !pool || !mem::Read(pool + kGroupPoolBase, c.groupBase) ||
                !mem::Read(pool + kGroupPoolCount, c.groupCount) || !mem::Read(pool + kGroupPoolStride, c.groupStride) ||
                !c.groupBase || c.groupCount == 0 || c.groupCount > kMaxGroups || c.groupStride < 0x10 || c.groupStride > 0x10000)
                return;
            std::uint64_t bits = 0;
            if (!mem::Read(pool + kGroupPoolBits, bits) || !bits)
                return;
            c.groupBits.assign((c.groupCount + 31) / 32, 0u);
            if (!mem::Copy(c.groupBits.data(), bits, c.groupBits.size() * sizeof(std::uint32_t)))
                return;
            c.groupsOk = true;
        }

        // The patched category of a category under a mix group (-1 none,
        // -2 unreadable).
        std::int16_t RemapOf(Caches& c, std::uint32_t group, std::int32_t category)
        {
            if (!c.groupsOk || group >= c.groupCount || category < 0 || category >= kMaxCategory)
                return -2;
            if (!((c.groupBits[group >> 5] >> (group & 31)) & 1u))
                return -2;
            const std::uint32_t key = (group << 16) | (static_cast<std::uint32_t>(category) & 0xFFFFu);
            const auto at = c.remapEntry.find(key);
            if (at != c.remapEntry.end())
                return at->second;
            std::uint64_t table = 0;
            const auto t = c.groupRemap.find(group);
            if (t != c.groupRemap.end())
                table = t->second;
            else
            {
                if (!mem::Read(c.groupBase + static_cast<std::uint64_t>(group) * c.groupStride + kGroupRemap, table))
                    table = 0;
                c.groupRemap[group] = table;
            }
            std::int16_t out = -2;
            if (table)
            {
                std::int16_t v = -2;
                if (mem::Read(table + static_cast<std::uint64_t>(category) * 2u, v))
                    out = v;
            }
            c.remapEntry[key] = out;
            return out;
        }

        void ReadScenes(Caches& c)
        {
            const std::uint32_t lists[2][2] = {{kSceneListARva, kSceneCountARva}, {kSceneListBRva, kSceneCountBRva}};
            std::vector<std::uint64_t> ptrs;
            for (const auto& l : lists)
            {
                std::uint64_t base = 0;
                std::uint16_t count = 0;
                if (!mem::Read(g_base + l[0], base) || !base || !mem::Read(g_base + l[1], count) || count == 0 ||
                    count > kMaxScenesPerList)
                    continue;
                ptrs.assign(count, 0);
                if (!mem::Copy(ptrs.data(), base, count * sizeof(std::uint64_t)))
                    continue;
                for (const std::uint64_t p : ptrs)
                {
                    if (!p)
                        continue;
                    SceneInfo s;
                    s.address = p;
                    std::uint8_t tail[0x10]{}; // +0x1FFC mode, +0x2000 tag, +0x2004, +0x2008 id
                    if (!mem::Read(p + kSceneVolume, s.volume) || !std::isfinite(s.volume) ||
                        !mem::Copy(tail, p + kSceneMode, sizeof(tail)))
                        continue;
                    std::memcpy(&s.mode, tail, 4);
                    std::memcpy(&s.tag, tail + (kSceneTag - kSceneMode), 4);
                    std::memcpy(&s.key, tail + (kSceneKey - kSceneMode), 4);
                    std::memcpy(&s.id, tail + (kSceneId - kSceneMode), 2);
                    if (c.sceneById.find(s.id) != c.sceneById.end())
                        continue; // the engine's lookup returns the first one
                    c.sceneById[s.id] = c.scenes.size();
                    c.scenes.push_back(s);
                }
            }
        }

        const SettingsInfo& Settings(Caches& c, std::uint32_t bucket, std::uint16_t index, std::uint64_t settingsBase,
            std::uint32_t settingsStride, std::uint32_t audioIndex)
        {
            const std::uint64_t key = (static_cast<std::uint64_t>(bucket) << 32) | index;
            const auto at = c.settings.find(key);
            if (at != c.settings.end())
                return at->second;
            SettingsInfo& out = c.settings[key];
            std::uint8_t s[kSettingsBytes];
            const std::uint32_t bytes = SettingsCopyBytes(settingsStride);
            if (index == kNoIndex || !settingsBase ||
                !mem::Copy(s, settingsBase + static_cast<std::uint64_t>(index) * settingsStride, bytes))
                return out;
            const std::uint32_t at2 = audioIndex * kSettingsBufferStride;
            std::memcpy(&out.reqVolumeDb, s + kSettingsVolume + at2, 4);
            std::memcpy(&out.postDb, s + kSettingsPost + at2, 4);
            std::memcpy(&out.wet, s + kSettingsWet + at2, 4);
            std::memcpy(&out.dry, s + kSettingsDry + at2, 4);
            std::memcpy(out.pos, s + kSettingsPosition + audioIndex * 0x10u, sizeof(out.pos));
            out.posMode = static_cast<std::uint8_t>((s[kSettingsPosMode] >> 2) & 3u);
            std::uint16_t speaker = 0;
            std::memcpy(&speaker, s + kSettingsSpeakerFlags + at2, 2);
            out.quad = (speaker & kQuadFlag) != 0;
            std::uint64_t group = 0;
            std::memcpy(&group, s + kSettingsEnvGroup, 8);
            if (group)
            {
                float open = -1.0f;
                std::memcpy(&open, s + kSettingsOpen, 4);
                out.open = std::isfinite(open) ? open : -1.0f;
            }
            for (std::uint32_t j = 0; j < kGroupSlots; ++j)
            {
                std::memcpy(&out.groupIds[j][0], s + kSettingsGroupIds + j * 6u, 2);
                std::memcpy(&out.groupIds[j][1], s + kSettingsGroupIds + j * 6u + 2u, 2);
                out.fade[j] = s[kSettingsGroupIds + j * 6u + 4u];
            }
            if (!std::isfinite(out.reqVolumeDb))
                out.reqVolumeDb = 0.0f;
            if (!std::isfinite(out.postDb))
                out.postDb = 0.0f;
            if (!std::isfinite(out.wet))
                out.wet = 0.0f;
            if (!std::isfinite(out.dry))
                out.dry = 1.0f;
            out.ok = true;
            return out;
        }

        const TopInfo& Top(Caches& c, std::uint64_t top)
        {
            const auto at = c.tops.find(top);
            if (at != c.tops.end())
                return at->second;
            TopInfo& out = c.tops[top];
            std::uint64_t refs[2]{}; // +0x48 tracker, +0x50 entity
            mem::Read(top, out.vtable);
            if (mem::Copy(refs, top + kSoundTracker, sizeof(refs)))
            {
                out.tracker = refs[0];
                out.entity = refs[1];
            }
            mem::Read(top + kSoundSettingsIndex, out.settings);
            return out;
        }

        // The top-level sound of a voice (parents in the same bucket); 0 when
        // the chain breaks.
        std::uint64_t ClimbToTop(Caches& c, std::uint32_t bucket, std::uint64_t sound, std::uint16_t parent,
            std::uint64_t soundsBase, std::uint32_t soundStride, std::uint32_t slots)
        {
            if (parent == kNoIndex)
                return sound;
            const std::uint64_t key = (static_cast<std::uint64_t>(bucket) << 32) | parent;
            const auto at = c.climb.find(key);
            if (at != c.climb.end())
                return at->second;
            std::uint64_t top = 0;
            std::uint16_t next = parent;
            for (int depth = 0; depth < 16; ++depth)
            {
                if (next >= slots)
                    break;
                const std::uint64_t p = soundsBase + static_cast<std::uint64_t>(next) * soundStride;
                std::uint16_t up = kNoIndex;
                if (!mem::Read(p + kSoundParent, up))
                    break;
                if (up == kNoIndex)
                {
                    top = p;
                    break;
                }
                next = up;
            }
            c.climb[key] = top;
            return top;
        }

        void ReadKnown(Known& known)
        {
            if (!PartOn(Part::Entities))
                return;
            if (g_entityMask & kEntityWeatherBit)
            {
                const std::uint64_t slots = g_base + kWeatherRva + kWindBlock + kWindSlots;
                for (std::uint32_t i = 0; i < kWindSlotCount; ++i)
                    mem::Read(slots + static_cast<std::uint64_t>(i) * kWindSlotStride, known.wind[i]);
            }
            std::uint64_t mgr = 0, vt = 0;
            if (mem::Read(g_base + kMusicPtrRva, mgr) && mgr && mem::Read(mgr, vt) && vt == g_base + kMusicVtableRva)
            {
                known.musicMgr = mgr;
                for (std::uint32_t k = 0; k < kMusicTrackCount; ++k)
                    mem::Read(mgr + kMusicTracks + static_cast<std::uint64_t>(k) * kMusicTrackStride + kMusicTrackSound,
                        known.music[k]);
            }
        }

        bool SoundPlaying(std::uint64_t sound)
        {
            std::uint8_t state = 0;
            return sound && mem::Read(sound + kSoundState, state) && (state >> 6) == kStatePlaying;
        }

        // A game scene object (the one a scene reference holds): its hash and
        // its "apply" variable (-1 when it has none).
        bool GameSceneAt(std::uint64_t pointer, std::uint32_t& hash, float& apply)
        {
            apply = -1.0f;
            if (!pointer || !mem::Read(pointer + kGameSceneHash, hash))
                return false;
            std::int32_t count = 0;
            std::uint32_t vars[2 * kGameSceneMaxVars]{};
            if (mem::Read(pointer + kGameSceneVarCount, count) && count > 0 &&
                count <= static_cast<std::int32_t>(kGameSceneMaxVars) &&
                mem::Copy(vars, pointer + kGameSceneVars, static_cast<std::size_t>(count) * 8u))
            {
                for (std::int32_t n = 0; n < count; ++n)
                    if (vars[2 * n] == kApplyHash)
                    {
                        float v = 0.0f;
                        std::memcpy(&v, &vars[2 * n + 1], 4);
                        if (std::isfinite(v))
                            apply = v;
                    }
            }
            return true;
        }

        // One variable of a game scene object (-1 when it has none).
        float GameSceneVar(std::uint64_t pointer, std::uint32_t name)
        {
            std::int32_t count = 0;
            std::uint32_t vars[2 * kGameSceneMaxVars]{};
            if (!pointer || !mem::Read(pointer + kGameSceneVarCount, count) || count <= 0 ||
                count > static_cast<std::int32_t>(kGameSceneMaxVars) ||
                !mem::Copy(vars, pointer + kGameSceneVars, static_cast<std::size_t>(count) * 8u))
                return -1.0f;
            for (std::int32_t n = 0; n < count; ++n)
                if (vars[2 * n] == name)
                {
                    float v = 0.0f;
                    std::memcpy(&v, &vars[2 * n + 1], 4);
                    return std::isfinite(v) && v >= 0.0f ? v : -1.0f;
                }
            return -1.0f;
        }

        void ReadEnv(const Known& known, EnvSample& e)
        {
            if (!g_cfg.env)
                return;
            if (PartOn(Part::Env) && PartOn(Part::Entities) && (g_entityMask & kEntityWeatherBit))
            {
                const std::uint64_t w = g_base + kWeatherRva;
                e.weather = mem::Read(w + kWindBlock + kWindStrength, e.windStrength) && std::isfinite(e.windStrength);
                if (!e.weather)
                    e.windStrength = 0.0f;
                mem::Read(w + kWindTypeOverride, e.windTypeOverride);
                mem::Read(w + kWindTypeOld, e.windTypeOld);
                mem::Read(w + kWindTypeNew, e.windTypeNew);
                mem::Read(w + kWindVarOverride, e.windVarOverride);
                if (!mem::Read(w + kWindVarValue, e.windVarValue) || !std::isfinite(e.windVarValue))
                    e.windVarValue = 0.0f;
                std::uint64_t scene = 0;
                if (mem::Read(w + kWeatherScene, scene))
                    e.weatherScene = GameSceneAt(scene, e.weatherSceneHash, e.weatherSceneApply);
                std::uint32_t prev = 0, next = 0;
                if (mem::Read(g_base + kWeatherPrevRva, prev) && prev < kMaxWeatherTypes)
                    mem::Read(g_base + kWeatherTableRva + prev * kWeatherTypeStride + kWeatherTypeHash, e.weatherPrev);
                if (mem::Read(g_base + kWeatherNextRva, next) && next < kMaxWeatherTypes)
                    mem::Read(g_base + kWeatherTableRva + next * kWeatherTypeStride + kWeatherTypeHash, e.weatherNext);
                for (const std::uint64_t s : known.wind)
                {
                    e.windSlots += s ? 1u : 0u;
                    e.windSlotsPlaying += SoundPlaying(s) ? 1u : 0u;
                }
            }
            if (PartOn(Part::Env))
            {
                e.interiorRead = mem::Read(g_base + kInteriorRva + kInteriorIn, e.interior) &&
                    mem::Read(g_base + kInteriorRva + kInteriorRatio, e.interiorRatio) && std::isfinite(e.interiorRatio);
                e.exposureRead = mem::Copy(e.exposure, g_base + kExposureRva + kExposure, sizeof(e.exposure)) &&
                    std::isfinite(e.exposure[0]) && std::isfinite(e.exposure[1]) && std::isfinite(e.exposure[2]) &&
                    std::isfinite(e.exposure[3]);
                e.duckRead = mem::Read(g_base + kDuckLevelRva, e.duckLevel) && std::isfinite(e.duckLevel) &&
                    mem::Read(g_base + kDuckFlagRva, e.duckFlag);
                std::uint64_t duck = 0;
                if (e.duckRead && mem::Read(g_base + kDuckSceneRva, duck))
                    e.duckScene = GameSceneAt(duck, e.duckSceneHash, e.duckSceneApply);
                if (e.duckScene)
                {
                    e.duckSceneWeather = GameSceneVar(duck, kWeatherApplyHash);
                    e.duckSceneSfx = GameSceneVar(duck, kSfxApplyHash);
                }
                e.flagsRead = mem::Copy(e.flags, g_base + kAudioFlagCountersRva, sizeof(e.flags));
            }
            e.music = known.musicMgr != 0;
            for (const std::uint64_t s : known.music)
                e.musicTracks += SoundPlaying(s) ? 1u : 0u;
        }

        std::string ScenesText(const Caches& c, const std::uint16_t* ids, std::uint32_t count)
        {
            std::string out;
            for (std::uint32_t n = 0; n < count && n < 8; ++n)
            {
                const auto at = c.sceneById.find(ids[n]);
                char b[40];
                if (at == c.sceneById.end())
                    std::snprintf(b, sizeof(b), "%s%u:gone", out.empty() ? "" : ";", ids[n]);
                else
                    std::snprintf(b, sizeof(b), "%s%u:%.3f", out.empty() ? "" : ";", ids[n], c.scenes[at->second].volume);
                out += b;
            }
            return out.empty() ? std::string("none") : out;
        }

        // One bucket's walk, kept aside until the bucket's list proves settled
        // (Snapshot commits it, or walks the bucket again: ListChangeOf in
        // volume_probe_core.h).
        struct Pending
        {
            Voice v;
            std::uint64_t sound = 0;
            std::uint64_t top = 0;
            std::uint32_t owner = census::kNoOwner;
            bool live = false;
            std::uint8_t mg[kEnvMixGroupCount] = {};
            float quad[4] = {};
            std::uint16_t sceneIds[kMaxSceneIds] = {};
            std::uint32_t sceneCount = 0;
            std::uint16_t listIndex = 0;
            std::uint16_t voiceIndex = 0;
            std::string who;
        };
        struct BucketWalk
        {
            Counters c; // the walk's share of the pass counters
            std::uint64_t listVoices = 0, holes = 0, listNotEnv = 0, backlinkBad = 0, idle = 0, settingsMismatch = 0,
                unmatched = 0, linkMixed = 0, linkPending = 0, listDup = 0;
            std::vector<Pending> voices;
            SlotSet seen; // the sound slots this walk took (Link does not prove one entry per voice)
            void Clear()
            {
                c = Counters{};
                listVoices = holes = listNotEnv = backlinkBad = idle = settingsMismatch = unmatched = 0;
                linkMixed = linkPending = listDup = 0;
                voices.clear();
            }
        };
        BucketWalk g_walk; // reused: no allocation per bucket once grown

        void AddCounts(Counters& to, const Counters& from)
        {
            to.voices += from.voices;
            to.voiceUnreadable += from.voiceUnreadable;
            to.climbBroken += from.climbBroken;
            to.settingsMissing += from.settingsMissing;
            to.bornClassified += from.bornClassified;
            to.speechClassified += from.speechClassified;
            to.ownerClassified += from.ownerClassified;
            to.entityClassified += from.entityClassified;
            to.world += from.world;
            to.listNotEnv += from.listNotEnv;
            to.backlinkBad += from.backlinkBad;
            to.linkMixed += from.linkMixed;
            to.linkPending += from.linkPending;
            to.listDup += from.listDup;
            to.listOutOfRange += from.listOutOfRange;
            to.noVoice += from.noVoice;
            to.groupUnknown += from.groupUnknown;
        }

        // The entries [0, usable) of one bucket's voice list into `w`.
        void WalkBucket(BucketWalk& w, Caches& caches, const Known& known, const Listener& listener, std::uint32_t b,
            std::uint64_t bucket, std::uint64_t soundsBase, std::uint64_t settingsBase, std::uint32_t soundStride,
            std::uint32_t settingsStride, std::uint32_t slots, std::uint32_t audioIndex, bool split, bool speaker,
            const std::uint16_t* list, std::uint16_t usable, std::uint64_t wall)
        {
            const std::uint64_t envVtable = g_base + kEnvVtableRva;
            std::uint8_t buf[kEnvBytes];
            w.seen.Reset(slots);
            for (std::uint16_t i = 0; i < usable; ++i)
            {
                const std::uint16_t slot = list[i];
                const Entry entry = EntryOf(slot, slots);
                if (entry == Entry::Hole)
                {
                    ++w.holes;
                    continue;
                }
                if (entry == Entry::OutOfRange)
                {
                    ++w.c.listOutOfRange;
                    continue;
                }
                const std::uint64_t sound = soundsBase + static_cast<std::uint64_t>(slot) * soundStride;
                std::uint64_t vt = 0;
                if (!mem::Read(sound, vt) || vt != envVtable)
                {
                    ++w.listNotEnv;
                    ++w.c.listNotEnv;
                    continue;
                }
                if (!mem::Copy(buf, sound, kEnvBytes))
                    continue;
                std::uint16_t listIndex = kNoIndex, voiceIndex = kNoIndex;
                std::memcpy(&listIndex, buf + kEnvListIndex, 2);
                std::memcpy(&voiceIndex, buf + kEnvVoiceIndex, 2);
                // i (registered, not mixed yet) or 0xFFFF (mixed: the metric
                // builder resets it) is this entry's voice; another index is a
                // newer registration.
                const Link link = LinkOf(listIndex, i);
                if (link == Link::Other)
                {
                    ++w.backlinkBad;
                    ++w.c.backlinkBad;
                    continue;
                }
                // A freed voice (FreeVoice 0x2573810 clears E+0x240) whose
                // entry the destructor left (it clears the list only while
                // E+0x242 holds the entry).
                if (voiceIndex == kNoIndex)
                {
                    ++w.c.noVoice;
                    continue;
                }
                if (!w.seen.Take(slot))
                {
                    ++w.listDup;
                    ++w.c.listDup;
                    continue;
                }
                if (link == Link::Mixed)
                {
                    ++w.linkMixed;
                    ++w.c.linkMixed;
                }
                else
                {
                    ++w.linkPending;
                    ++w.c.linkPending;
                }
                Pending p;
                Voice& v = p.v;
                v.playing = (buf[kSoundState] >> 6) == kStatePlaying;
                if (!v.playing)
                    ++w.idle;
                std::memcpy(&v.final, buf + kEnvFinal, 4);
                std::int16_t cat = -1;
                std::memcpy(&cat, buf + kEnvCategory, 2);
                v.category = cat;
                std::memcpy(p.mg, buf + kEnvMixGroups, sizeof(p.mg));
                const std::uint8_t* mg = p.mg;
                for (std::uint32_t j = 0; j < kGroupSlots; ++j)
                    v.mixGroups += mg[j] != 0xFF ? 1 : 0;
                std::uint64_t voice = 0;
                std::memcpy(&voice, buf + kEnvVoice, 8);
                float vc[2]{};
                if (voice && mem::Copy(vc, voice + kVoiceVolume, sizeof(vc)) && std::isfinite(vc[0]) && std::isfinite(vc[1]))
                {
                    v.volume = vc[0];
                    v.chain = vc[1];
                    v.voiceRead = true;
                }
                else
                    ++w.c.voiceUnreadable;
                float comb[2]{};
                if (mem::Copy(comb, bucket + kBucketCombine + static_cast<std::uint64_t>(i) * kCombineStride, sizeof(comb)) &&
                    std::isfinite(comb[0]) && std::isfinite(comb[1]))
                {
                    v.combDb = comb[0];
                    v.combPostDb = comb[1];
                    v.combRead = true;
                }
                float catDb = 0.0f;
                if (CatDb(caches, cat, catDb))
                {
                    v.catDb = catDb;
                    v.catKnown = true;
                }
                if (split)
                    v.bus = BusSkipsCategory(buf[kEnvBus]);

                // The top-level sound (parents in the same bucket).
                std::uint16_t parent = kNoIndex;
                std::memcpy(&parent, buf + kSoundParent, 2);
                std::uint64_t top = 0;
                if (buf[kSoundBucket] == b)
                    top = ClimbToTop(caches, b, sound, parent, soundsBase, soundStride, slots);
                if (!top)
                {
                    ++w.c.climbBroken;
                    top = sound;
                }
                const TopInfo& topInfo = Top(caches, top);
                std::uint16_t settingsIndex = kNoIndex;
                std::memcpy(&settingsIndex, buf + kEnvTopSettings, 2);
                if (topInfo.settings != settingsIndex)
                    ++w.settingsMismatch;
                v.entityRva = RvaOf(topInfo.entity, g_base, g_imageEnd);
                v.topVtRva = RvaOf(topInfo.vtable, g_base, g_imageEnd);

                const SettingsInfo& si = Settings(caches, b, settingsIndex, settingsBase, settingsStride, audioIndex);
                if (si.ok)
                {
                    v.reqVolumeDb = si.reqVolumeDb;
                    v.postDb = si.postDb;
                    v.wet = si.wet;
                    v.dry = si.dry;
                    v.open = si.open;
                    v.posMode = si.posMode;
                    bool havePos = Finite3(si.pos) && !Zero3(si.pos);
                    if (speaker)
                    {
                        v.quad = si.quad;
                        if (si.quad)
                        {
                            std::memcpy(p.quad, si.pos, sizeof(p.quad));
                            v.quadDb = QuadPowerDb(p.quad);
                        }
                        havePos = havePos && !si.quad && si.posMode != 0;
                    }
                    if (havePos && listener.ok)
                        v.distM = NearestPoint(listener, si.pos);
                }
                else
                    ++w.c.settingsMissing;

                // The chain's split: mix groups and mixer scenes.
                if (split && si.ok && caches.groupsOk)
                {
                    float factor = 1.0f;
                    bool known2 = true;
                    for (std::uint32_t j = 0; j < kGroupSlots; ++j)
                    {
                        const std::uint8_t cur = mg[j];
                        if (cur == 0xFF)
                            continue;
                        bool hasPrev = false, hasCur = false;
                        float prevDb = 0.0f, curDb = 0.0f;
                        if (!v.bus && si.groupIds[j][1] != -1)
                        {
                            const std::uint8_t prev = buf[kEnvPrevGroups + j];
                            if (prev != 0xFF)
                            {
                                const std::int16_t r = RemapOf(caches, prev, cat);
                                if (r == -2)
                                    known2 = false;
                                else if (r != -1)
                                {
                                    hasPrev = CatDb(caches, r, prevDb);
                                    known2 = known2 && hasPrev;
                                }
                            }
                        }
                        const std::int16_t r = RemapOf(caches, cur, cat);
                        if (r == -2)
                            known2 = false;
                        else if (r != -1)
                        {
                            hasCur = CatDb(caches, r, curDb);
                            known2 = known2 && hasCur;
                        }
                        factor *= GroupSlotFactor(v.bus, hasPrev, prevDb, hasCur, curDb, si.fade[j]);
                    }
                    v.group = factor;
                    v.groupKnown = known2;
                    if (!known2)
                        ++w.c.groupUnknown;
                }
                if (split)
                {
                    std::int32_t count = 0;
                    std::memcpy(&count, buf + kEnvSceneCount, 4);
                    p.sceneCount = count > 0 ? static_cast<std::uint32_t>(count) : 0u;
                    if (p.sceneCount > kMaxSceneIds)
                        p.sceneCount = kMaxSceneIds;
                    std::memcpy(p.sceneIds, buf + kEnvSceneIds, p.sceneCount * sizeof(std::uint16_t));
                    std::int16_t tags[kGroupSlots];
                    std::memcpy(tags, buf + kEnvTags, sizeof(tags));
                    std::vector<const SceneInfo*> scenes;
                    for (std::uint32_t n = 0; n < p.sceneCount; ++n)
                    {
                        const auto at = caches.sceneById.find(p.sceneIds[n]);
                        if (at != caches.sceneById.end())
                            scenes.push_back(&caches.scenes[at->second]);
                    }
                    v.scene = SceneTerm(scenes, tags, kGroupSlots);
                    v.sceneKnown = true;
                    v.scenes = static_cast<std::uint8_t>(p.sceneCount);
                }

                v.cls = Classify(top, topInfo.settings, topInfo.entity, topInfo.tracker, known, wall, w.c, p.owner, p.live, p.who);
                if (v.cls == Cls::World)
                    ++w.unmatched;
                ++w.c.voices;
                p.sound = sound;
                p.top = top;
                p.listIndex = i;
                p.voiceIndex = voiceIndex;
                w.voices.push_back(std::move(p));
            }
        }

        // A settled (or last) walk of one bucket into the window and the pass.
        void CommitBucket(const BucketWalk& w, const Caches& caches, bool split)
        {
            WindowInfo& info = g_window.info;
            info.listVoices += w.listVoices;
            info.holes += w.holes;
            info.listNotEnv += w.listNotEnv;
            info.backlinkBad += w.backlinkBad;
            info.linkMixed += w.linkMixed;
            info.linkPending += w.linkPending;
            info.listDup += w.listDup;
            info.idle += w.idle;
            info.settingsMismatch += w.settingsMismatch;
            info.unmatched += w.unmatched;
            g_counters.listVoices += w.listVoices;
            AddCounts(g_counters, w.c);
            for (const Pending& p : w.voices)
            {
                g_window.Add(p.v);
                std::uint64_t ref = 0;
                const Src src = SrcOf(p.top, ref);
                if (p.v.cls == Cls::Owned) // VOLUME_PROBE_SRC
                {
                    const Stages st = StagesOf(p.v);
                    SrcAgg& a = g_srcRefs[static_cast<std::size_t>(src)][ref];
                    ++a.n;
                    a.power += std::pow(10.0, st.effDb / 10.0);
                    if (st.effDb > a.maxDb)
                    {
                        a.maxDb = st.effDb;
                        a.category = p.v.category;
                        a.volDb = st.volumeDb;
                        a.distM = p.v.distM;
                        a.owner = p.owner;
                    }
                }
                if (!g_cfg.top)
                    continue;
                TopVoice& t = g_top[static_cast<std::uint8_t>(p.v.cls) < kClsCount ? static_cast<std::uint8_t>(p.v.cls) : kClsCount - 1];
                const Stages s = StagesOf(p.v);
                if (!t.set || s.effDb > t.s.effDb)
                {
                    t.set = true;
                    t.v = p.v;
                    t.s = s;
                    t.sound = p.sound;
                    t.top = p.top;
                    t.owner = p.owner;
                    t.live = p.live;
                    std::memcpy(t.mg, p.mg, sizeof(t.mg));
                    std::memcpy(t.quad, p.quad, sizeof(t.quad));
                    t.scenes = split ? ScenesText(caches, p.sceneIds, p.sceneCount) : std::string("na");
                    t.listIndex = p.listIndex;
                    t.voiceIndex = p.voiceIndex;
                    t.who = p.who;
                    t.wvol = WrapperVarText(ReadWrapperVar(p.top, p.v.topVtRva));
                    t.ref = ref;
                    t.src = SrcName(src);
                }
            }
        }

        void Snapshot(std::uint64_t wall)
        {
            const std::uint64_t t0 = replay::clock::NowUs();
            std::uint64_t pool = 0;
            std::uint32_t soundStride = 0, settingsStride = 0, buckets = 0, slots = 0, audioIndex = 0;
            std::uint8_t envSync = 0;
            if (!mem::Read(g_base + kPoolTableRva, pool) || !mem::Read(g_base + kSoundStrideRva, soundStride) ||
                !mem::Read(g_base + kSettingsStrideRva, settingsStride) || !mem::Read(g_base + kBucketCountRva, buckets) ||
                !mem::Read(g_base + kSlotCountRva, slots) || !mem::Read(g_base + kAudioIndexRva, audioIndex) || !pool ||
                !PoolSane(buckets, slots, soundStride, settingsStride))
            {
                ++g_counters.poolInsane;
                return;
            }
            Caches caches;
            mem::Read(g_base + kCategoryTableRva, caches.catTable);
            mem::Read(g_base + kEnvSyncRva, envSync);
            audioIndex &= 1u;
            const bool split = PartOn(Part::Split);
            const bool speaker = PartOn(Part::Speaker);
            if (split)
            {
                ReadGroups(caches);
                ReadScenes(caches);
            }
            Known known;
            ReadKnown(known);

            const Listener listener = ReadListener();
            float player[3]{}, cam[3]{};
            const Ped playerPed = PLAYER::PLAYER_PED_ID();
            const bool hasPlayer = playerPed && replay::entities::Exists(playerPed);
            if (hasPlayer)
                ToFloats(ENTITY::GET_ENTITY_COORDS(playerPed, TRUE, FALSE), player);
            ToFloats(CAM::GET_FINAL_RENDERED_CAM_COORD(), cam);

            WindowInfo& info = g_window.info;
            ++info.snaps;
            if (listener.ok)
            {
                std::memcpy(info.listener, listener.pos[0], sizeof(info.listener));
                info.hasListener = true;
                if (hasPlayer)
                {
                    info.lstPlayer += Dist3(listener.pos[0], player);
                    ++info.lstPlayerN;
                }
                info.lstCam += Dist3(listener.pos[0], cam);
                ++info.lstN;
                info.lstPoints += listener.points;
                if (listener.points >= 2)
                {
                    std::memcpy(info.listener1, listener.pos[1], sizeof(info.listener1));
                    info.hasListener1 = true;
                }
            }
            info.music += AUDIO::AUDIO_IS_MUSIC_PLAYING() != FALSE ? 1u : 0u;
            info.envSync += envSync ? 1u : 0u;
            info.scenesSeen += caches.scenes.size();
            g_windowScenes = static_cast<std::uint32_t>(caches.scenes.size());
            g_windowLowScenes = LowScenesText(caches.scenes, 8);

            EnvSample env;
            ReadEnv(known, env);
            g_env.Add(env);

            const std::uint64_t envVtable = g_base + kEnvVtableRva;
            const std::uint32_t words = slots >> 5;
            std::vector<std::uint32_t> bits(words);
            std::uint16_t list[kMaxVoicesPerBucket];
            BucketWalk& walk = g_walk;
            for (std::uint32_t b = 0; b < buckets; ++b)
            {
                const std::uint64_t bucket = pool + static_cast<std::uint64_t>(b) * kBucketStride;
                std::uint64_t soundsBase = 0, settingsBase = 0;
                if (!mem::Read(bucket + kBucketSounds, soundsBase) || !mem::Read(bucket + kBucketSettings, settingsBase) ||
                    !soundsBase)
                    continue;

                // The parent bits (diagnostic only).
                if (mem::Copy(bits.data(), bucket + kBucketParentBits, words * sizeof(std::uint32_t)))
                {
                    for (std::uint32_t w = 0; w < words; ++w)
                    {
                        std::uint32_t set = bits[w];
                        while (set)
                        {
                            unsigned long bit = 0;
                            _BitScanForward(&bit, set);
                            set &= set - 1;
                            ++info.tops;
                            std::uint64_t vt = 0;
                            const std::uint64_t sound = soundsBase + (static_cast<std::uint64_t>(w) * 32u + bit) * soundStride;
                            if (mem::Read(sound, vt) && vt == envVtable)
                                ++info.topsEnv;
                        }
                    }
                }

                // The voice list (count, entries, count again), walked into a
                // buffer that is committed only once the list proves settled
                // (ListChangeOf): a copy taken while the frame rebuilds the list
                // (outside the bucket lock) holds only the voices registered so
                // far. A count that moved during the copy or during the walk
                // repeats the bucket (list_retried), and after kListAttempts
                // what is still moving is committed and counted (list_growing:
                // that snapshot misses part of the bucket).
                for (int attempt = 0; attempt < kListAttempts; ++attempt)
                {
                    const bool last = attempt + 1 == kListAttempts;
                    std::uint16_t c1 = 0, c2 = 0;
                    if (!mem::Read(bucket + kBucketVoiceCount, c1))
                        break;
                    if (c1 > kMaxVoicesPerBucket)
                    {
                        ++g_counters.listInsane;
                        break;
                    }
                    if (c1 && !mem::Copy(list, bucket + kBucketVoiceSlots, c1 * sizeof(std::uint16_t)))
                        break;
                    if (!mem::Read(bucket + kBucketVoiceCount, c2))
                        break;
                    bool partial = false;
                    const std::uint16_t usable = ListUsable(c1, c2, partial);
                    if (ListChangeOf(c1, c2) != ListChange::Settled && !last)
                    {
                        ++info.listRetried;
                        ++g_counters.listRetried;
                        continue;
                    }
                    walk.Clear();
                    walk.listVoices = usable;
                    WalkBucket(walk, caches, known, listener, b, bucket, soundsBase, settingsBase, soundStride,
                        settingsStride, slots, audioIndex, split, speaker, list, usable, wall);
                    std::uint16_t c3 = c2;
                    const ListChange after = mem::Read(bucket + kBucketVoiceCount, c3) ? ListChangeOf(c1, c3) : ListChange::Settled;
                    if (after != ListChange::Settled && !last)
                    {
                        ++info.listRetried;
                        ++g_counters.listRetried;
                        continue;
                    }
                    if (partial)
                    {
                        ++info.listPartial;
                        ++g_counters.listPartial;
                    }
                    if (partial || after != ListChange::Settled)
                    {
                        ++info.listGrowing;
                        ++g_counters.listGrowing;
                    }
                    CommitBucket(walk, caches, split);
                    break;
                }
            }
            ++g_counters.snapshots;
            const double us = static_cast<double>(replay::clock::NowUs() - t0);
            g_counters.walkUsTotal += us;
            if (us > g_counters.walkUsMax)
                g_counters.walkUsMax = us;
            g_windowWalkUs += us;
            if (us > g_windowWalkMax)
                g_windowWalkMax = us;
        }

        // ------------------------------------------------ lines

        // The top sound's {name, value} pairs hold ref: their slot is the
        // pool slot [top+0x9C] of the top's bucket (the layout 0x25736B4
        // reads), the count at slot+0x80. Guarded reads only.
        bool PairOf(std::uint64_t top, std::uint64_t ref, std::uint32_t& name)
        {
            std::uint16_t index = kNoIndex;
            std::uint16_t parent = 0;
            std::uint8_t bucket = 0;
            std::uint64_t pool = 0;
            std::uint32_t soundStride = 0, settingsStride = 0, buckets = 0, slots = 0;
            if (!mem::Read(top + kSoundVarIndex, index) || index == kNoIndex || !mem::Read(top + kSoundParent, parent) ||
                parent != kNoIndex || !mem::Read(top + kSoundBucket, bucket) || !mem::Read(g_base + kPoolTableRva, pool) ||
                !pool || !mem::Read(g_base + kSoundStrideRva, soundStride) ||
                !mem::Read(g_base + kSettingsStrideRva, settingsStride) || !mem::Read(g_base + kBucketCountRva, buckets) ||
                !mem::Read(g_base + kSlotCountRva, slots) || !PoolSane(buckets, slots, soundStride, settingsStride) ||
                bucket >= buckets || index >= slots)
                return false;
            std::uint64_t soundsBase = 0;
            if (!mem::Read(pool + static_cast<std::uint64_t>(bucket) * kBucketStride + kBucketSounds, soundsBase) || !soundsBase)
                return false;
            const std::uint64_t slot = soundsBase + static_cast<std::uint64_t>(index) * soundStride;
            std::int32_t count = 0;
            return mem::Read(slot + kVarPairCount, count) && PairSlotHit(slot, count, ref) && mem::Read(ref - 4, name);
        }

        // The variable a top wrapper sound adds to its volume, read when the
        // voice becomes its class's top (guarded reads; the PED_VARIABLES
        // names are the last PED_VARS scan's).
        WrapperVar ReadWrapperVar(std::uint64_t top, std::uint32_t topVtRva)
        {
            WrapperVar w;
            if (!PartOn(Part::Wrapper) || topVtRva != kWrapperVtableRva || !top)
                return w;
            std::uint64_t ref = 0;
            if (!mem::Read(top + kWrapperVolumeRef, ref))
            {
                w.home = VarHome::Unreadable;
                return w;
            }
            w.ref = ref;
            if (!ref)
            {
                w.home = VarHome::None;
                return w;
            }
            if (ref < kInternalVariableLimit)
            {
                w.home = VarHome::Internal;
                return w;
            }
            if (!replay::mem::Read(ref, w.value))
            {
                w.home = VarHome::Unreadable;
                return w;
            }
            std::uint32_t owner = 0;
            std::uint32_t name = 0;
            if (replay::pedaudiostate::DescribeAddress(static_cast<std::uintptr_t>(ref), owner, name))
            {
                w.home = VarHome::Ped;
                w.owner = census::PackedIdText(owner);
                w.name = name;
                w.label = replay::pedaudiostate::KnownLabel(name);
                return w;
            }
            // A pair only inside the top sound's own pairs
            // (0x25736B4, sites var_req_*); anything else is "other", never a
            // name read from the dword before an unknown float.
            std::uint32_t pairName = 0;
            if (PairOf(top, ref, pairName))
            {
                w.home = VarHome::Pair;
                w.name = pairName;
            }
            else
                w.home = VarHome::Other;
            return w;
        }

        std::string OwnerText(std::uint32_t owner, bool live)
        {
            if (live)
                return "live_player";
            return owner == census::kNoOwner ? std::string("-") : census::PackedIdText(owner);
        }

        // The mix group ids: "current/previous" (hex, ';' between, '-' none).
        std::string GroupsText(const std::uint8_t* mg)
        {
            std::string half[2];
            for (std::uint32_t j = 0; j < kEnvMixGroupCount; ++j)
            {
                if (mg[j] == 0xFF)
                    continue;
                std::string& out = half[j < kGroupSlots ? 0 : 1];
                char b[8];
                std::snprintf(b, sizeof(b), "%s%02x", out.empty() ? "" : ";", mg[j]);
                out += b;
            }
            return (half[0].empty() ? std::string("-") : half[0]) + "/" + (half[1].empty() ? std::string("-") : half[1]);
        }

        void ResetWindowExtras()
        {
            for (TopVoice& t : g_top)
                t = TopVoice{};
            g_windowWalkUs = 0.0;
            g_windowWalkMax = 0.0;
            g_env.Reset();
            g_windowScenes = 0;
            g_windowLowScenes.clear();
            for (auto& m : g_srcRefs)
                m.clear();
        }

        // The owned voices of the window by maker (count and power per
        // snapshot) and the loudest refs.
        std::string SrcLine(const char* phase, std::uint32_t seg, std::uint32_t clipMs, std::uint32_t snaps)
        {
            if (!snaps)
                return std::string();
            struct Item { Src src; std::uint64_t ref; const SrcAgg* a; };
            std::vector<Item> items;
            std::ostringstream o;
            o << "VOLUME_PROBE_SRC,phase=" << phase << ",pass=" << g_pass << ",seg=" << seg << ",clip_ms=" << clipMs
              << ",cls=owned";
            bool any = false;
            for (std::size_t n = 0; n < static_cast<std::size_t>(Src::Count); ++n)
            {
                std::uint32_t count = 0;
                double power = 0.0;
                for (const auto& kv : g_srcRefs[n])
                {
                    count += kv.second.n;
                    power += kv.second.power;
                    items.push_back(Item{static_cast<Src>(n), kv.first, &kv.second});
                }
                any = any || count;
                const char* name = n == static_cast<std::size_t>(Src::Unknown) ? "unknown" : SrcName(static_cast<Src>(n));
                o << ',' << name << "_n=" << Fmt1(static_cast<double>(count) / snaps) << ',' << name
                  << "_db=" << (power > 0.0 ? Fmt1(10.0 * std::log10(power / snaps)) : std::string("na"));
            }
            if (!any)
                return std::string();
            std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.a->power > b.a->power; });
            o << ",top=";
            for (std::size_t n = 0; n < items.size() && n < 6; ++n)
            {
                const SrcAgg& a = *items[n].a;
                char text[160];
                std::snprintf(text, sizeof(text), "%s%s:0x%llx:%s:c%d:n%.1f:db%.1f:vol%.1f:d%.1f", n ? "|" : "",
                    SrcName(items[n].src), static_cast<unsigned long long>(items[n].ref), OwnerText(a.owner, false).c_str(),
                    a.category, static_cast<double>(a.n) / snaps, 10.0 * std::log10(a.power / snaps), a.volDb, a.distM);
                o << text;
            }
            return o.str();
        }

        void Flush(std::ostream& log)
        {
            if (g_window.Empty())
            {
                g_window.Reset(-1);
                ResetWindowExtras();
                return;
            }
            const char* phase = Play(g_phase) ? "play" : "rec";
            const std::uint32_t clipMs = static_cast<std::uint32_t>(g_window.index * g_cfg.windowMs);
            const std::uint32_t seg = g_windowSeg;
            for (std::uint8_t n = 0; n < kClsCount; ++n)
                if (g_window.cls[n].n)
                    log << ClassLine(phase, g_pass, seg, clipMs, g_cfg.windowMs, ClsName(static_cast<Cls>(n)),
                               g_window.cls[n], g_window.info)
                        << '\n';
            log << ClassLine(phase, g_pass, seg, clipMs, g_cfg.windowMs, "all", g_window.all, g_window.info) << '\n';
            {
                const std::string src = SrcLine(phase, seg, clipMs, static_cast<std::uint32_t>(g_window.info.snaps));
                if (!src.empty())
                    log << src << '\n';
            }
            for (const std::int32_t category : TopCategories(g_window, g_cfg.categories))
                log << CategoryLine(phase, g_pass, seg, clipMs, category, g_window.categories[category], g_window.info) << '\n';
            for (const WorldKey& key : TopWorld(g_window, g_cfg.world))
                log << WorldLine(phase, g_pass, seg, clipMs, key, g_window.world[key], g_window.info) << '\n';
            const WindowInfo& info = g_window.info;
            log << "VOLUME_PROBE_STATE,phase=" << phase << ",pass=" << g_pass << ",seg=" << seg << ",clip_ms=" << clipMs
                << ",snaps=" << info.snaps << ",voices=" << info.voices << ",world=" << info.unmatched
                << ",idle=" << info.idle
                << ",music=" << Fmt2(info.snaps ? static_cast<double>(info.music) / info.snaps : 0.0)
                << ",env_sync=" << Fmt2(info.snaps ? static_cast<double>(info.envSync) / info.snaps : 0.0)
                << ",walk_us_mean=" << Fmt1(info.snaps ? g_windowWalkUs / info.snaps : 0.0)
                << ",walk_us_max=" << Fmt1(g_windowWalkMax) << ",speech_sounds=" << g_speech.size()
                << ",born_sounds=" << g_born.size() << ",list_voices=" << info.listVoices << ",tops=" << info.tops
                << ",tops_env=" << info.topsEnv << ",list_partial=" << info.listPartial
                << ",list_retried=" << info.listRetried << ",list_growing=" << info.listGrowing
                << ",backlink_bad=" << info.backlinkBad << ",link_mixed=" << info.linkMixed
                << ",link_pending=" << info.linkPending << ",list_dup=" << info.listDup << ",list_not_env=" << info.listNotEnv
                << ",holes=" << info.holes << ",settings_mismatch=" << info.settingsMismatch
                << ",mix_scenes=" << Fmt1(info.snaps ? static_cast<double>(info.scenesSeen) / info.snaps : 0.0) << '\n';
            if (g_cfg.env)
                log << EnvLine(phase, g_pass, seg, clipMs, g_env, g_flagHashes, g_windowScenes, g_windowLowScenes) << '\n';
            if (g_cfg.top)
            {
                for (std::uint8_t n = 0; n < kClsCount; ++n)
                {
                    const TopVoice& t = g_top[n];
                    if (!t.set)
                        continue;
                    char quad[96];
                    if (t.v.quad)
                        std::snprintf(quad, sizeof(quad), "%.3f;%.3f;%.3f;%.3f", t.quad[0], t.quad[1], t.quad[2], t.quad[3]);
                    else
                        std::snprintf(quad, sizeof(quad), "none");
                    char line[1536];
                    std::snprintf(line, sizeof(line),
                        "VOLUME_PROBE_TOP,phase=%s,pass=%u,seg=%u,clip_ms=%u,cat=%s,owner=%s,who=%s,sound=0x%llx,top=0x%llx,"
                        "ent=%s,top_vt=%s,list=%u,voice=%u,category=%d,final_db=%.1f,eff_db=%.1f,vol_db=%.1f,comb_db=%s,"
                        "req_db=%.1f,post_db=%.1f,cat_db=%.1f,chain_db=%.1f,resid_db=%.1f,grp_db=%s,scn_db=%s,rest_db=%.1f,"
                        "wetdry_db=%.1f,dist_m=%.1f,open=%.2f,bus_skip=%d,posmode=%u,quad=%s,mix_groups=%s,scenes=%s,"
                        "playing=%d,consistent=%d,wvol=%s,ref=0x%llx,src=%s",
                        phase, g_pass, seg, clipMs, ClsName(static_cast<Cls>(n)), OwnerText(t.owner, t.live).c_str(),
                        t.who.empty() ? "-" : t.who.c_str(), static_cast<unsigned long long>(t.sound),
                        static_cast<unsigned long long>(t.top), RvaText(t.v.entityRva).c_str(), RvaText(t.v.topVtRva).c_str(),
                        t.listIndex, t.voiceIndex, t.v.category, t.s.finalDb, t.s.effDb, t.s.volumeDb,
                        t.v.combRead ? Fmt1(t.v.combDb).c_str() : "na", t.v.reqVolumeDb, t.s.postDb, // applied
                        t.s.catDb, t.s.chainDb, t.s.residDb,
                        t.v.groupKnown ? Fmt1(t.s.groupDb).c_str() : "na", t.v.sceneKnown ? Fmt1(t.s.sceneDb).c_str() : "na",
                        t.s.restDb, t.s.wetDryDb, t.v.distM, t.v.open, t.v.bus ? 1 : 0, t.v.posMode, quad,
                        GroupsText(t.mg).c_str(), t.scenes.c_str(), t.v.playing ? 1 : 0, t.s.consistent ? 1 : 0,
                        t.wvol.c_str(), static_cast<unsigned long long>(t.ref), t.src);
                    log << line << '\n';
                }
            }
            g_totals.Add(g_window);
            g_window.Reset(-1);
            ResetWindowExtras();
        }

        void ClosePass(std::ostream& log)
        {
            if (std::strcmp(g_phase, "idle") == 0)
                return;
            Flush(log);
            const Counters& c = g_counters;
            log << "VOLUME_PROBE_TOTAL,phase=" << (Play(g_phase) ? "play" : "rec") << ",pass=" << g_pass
                << ",segs=" << (g_segments.seg + 1) << ",windows=" << g_totals.windows << ",sum_db_by_cat=" << g_totals.Text()
                << ",eff_db_by_cat=" << g_totals.Text(true) << ",snapshots=" << c.snapshots << ",voices=" << c.voices
                << ",pool_insane=" << c.poolInsane << ",voice_unreadable=" << c.voiceUnreadable
                << ",climb_broken=" << c.climbBroken << ",settings_missing=" << c.settingsMissing
                << ",by_speech=" << c.speechClassified << ",by_entity=" << c.entityClassified << ",by_birth=" << c.bornClassified
                << ",by_owner=" << c.ownerClassified << ",world=" << c.world << ",list_voices=" << c.listVoices
                << ",list_partial=" << c.listPartial << ",list_retried=" << c.listRetried << ",list_growing=" << c.listGrowing
                << ",backlink_bad=" << c.backlinkBad << ",link_mixed=" << c.linkMixed << ",link_pending=" << c.linkPending
                << ",list_dup=" << c.listDup << ",list_not_env=" << c.listNotEnv
                << ",list_oob=" << c.listOutOfRange << ",list_insane=" << c.listInsane << ",no_voice=" << c.noVoice
                << ",group_unknown=" << c.groupUnknown
                << ",walk_us_mean=" << Fmt1(c.snapshots ? c.walkUsTotal / c.snapshots : 0.0)
                << ",walk_us_max=" << Fmt1(c.walkUsMax) << '\n';
            g_phase = "idle";
        }

        void OpenPass(std::ostream& log, const char* phase)
        {
            g_phase = Play(phase) ? "play" : "rec";
            ++g_pass;
            g_totals = PassTotals{};
            g_counters = Counters{};
            g_segments.Reset();
            g_windowSeg = 0;
            g_window.Reset(-1);
            ResetWindowExtras();
            g_nextSnapWall = 0;
            g_requests.Clear();
            replay::audio::CollectPlayRequestsSince(g_requestCursor, g_requestBatch); // older requests: not this pass
            log << "VOLUME_PROBE_PASS,phase=" << g_phase << ",pass=" << g_pass << ",event=open\n";
        }
    }

    void Configure(const Config& config)
    {
        g_cfg = config;
        if (g_cfg.sampleMs < 20)
            g_cfg.sampleMs = 20;
        if (g_cfg.windowMs < 100)
            g_cfg.windowMs = 100;
        if (g_cfg.categories > 32)
            g_cfg.categories = 32;
        if (g_cfg.world > 32)
            g_cfg.world = 32;
        // The create-and-play request ring (read only), as the mix probe
        // claims it: never turned off here (MixProbe may hold it).
        if (g_cfg.enabled)
            replay::audio::SetPlayRequestMixProbe(true);
    }

    bool Enabled() { return g_cfg.enabled; }

    void Tick(Context& context, const char* phase, double clipMs, bool normal)
    {
        if (!g_cfg.enabled)
            return;
        std::ostream& log = context.log;
        if (!Prove(log))
            return;
        const std::uint64_t wall = Wall();
        const bool play = Play(phase);
        if (std::strcmp(play ? "play" : "rec", g_phase) != 0)
        {
            ClosePass(log);
            OpenPass(log, phase);
        }
        g_lastTickWall = wall;
        NoteBirths(wall);
        if (!normal || wall < g_nextSnapWall)
            return;
        g_nextSnapWall = wall + g_cfg.sampleMs;
        // The speakers' lines of this very tick (NoteSpeech).
        NoteSpeech(context, play, wall);
        const std::int64_t index = WindowOf(clipMs, g_cfg.windowMs);
        if (index != g_window.index)
        {
            Flush(log);
            g_windowSeg = g_segments.Next(index);
            g_window.Reset(index);
        }
        Snapshot(wall);
    }

    void Idle(Context& context)
    {
        if (!g_cfg.enabled || std::strcmp(g_phase, "idle") == 0)
            return;
        if (Wall() - g_lastTickWall < 300)
            return;
        // A gap in the ticks is not the end of the take while the recording or
        // the playback still runs (a streaming hitch of 300 ms or more, the
        // editor's pause, the paused end of the clip): closing the pass there
        // would split one playback or one recording into several passes. The
        // pass ends with the recording, with BeginPlayback / EndPlayback, or on
        // a change of phase.
        if (Play(g_phase) ? context.transport.Playing() : context.recorder.recording)
            return;
        ClosePass(context.log);
    }

    void BeginPlayback(Context& context)
    {
        if (!g_cfg.enabled)
            return;
        // What the recording (or an earlier playback) left open.
        ClosePass(context.log);
    }

    void EndPlayback(Context& context)
    {
        if (!g_cfg.enabled)
            return;
        ClosePass(context.log);
    }
}
