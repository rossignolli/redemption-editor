// The generic sound player's engine side (sound_generic.h BeginPlayback, Play,
// EndPlayback). The rules are in sound_generic_player_core.h (Player, pure);
// here is what touches the game: the puppets and their audio entities, the
// engine ports (audio_observer.h: create, S10 prepare, S11 play, S13 stop, S14
// stop and forget, S16 position, S18 volume, S19 pitch, S21 and S22 filters,
// S23 post submix, S24 curve scale, S38 variable), the slots, the census rows
// for the anti-double rule and the double detector, and the log.
//
// Script thread only (the playback tick), as the other sound tracks.
//
// A handle is the slot and its generation (a slot stolen for another packet no
// longer answers for the first one, so the first plan never drives the new
// sound with its adjusts, its stop and its release); the census rows and the
// anti-double rule name a sound by its set and entry; the step packets away
// from a recorded foot event are this player's; the ports judge a puppet's own
// create against the packets' lives (SoundGenericBlock); the per-sound tally
// and SoundGenericTrace.
#include "sound_generic.h"
#include "export_probe.h" // SoundGenericExport

#include "audio_observer.h"
#include "entity_cache.h"
#include "log_policy.h"
#include "mod_clock.h"
#include "profiler.h"
#include "sound_census.h"
#include "sound_generic_player_core.h"
#include "clip_area.h" // PositionValid
#include "sound_slot_policy.h"
#include "sound_world_core.h" // steal and yield evidence

#include <windows.h>

#include "main.h"
#include "natives.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace replay::soundgeneric
{
    namespace
    {
        bool g_originSkip = true; // SoundGenericOriginSkip
        using census::AudioBase;
        using census::OwnerKind;

        constexpr std::size_t kMaxSlots = 256;
        constexpr std::uint32_t kBeforeMs = 300; // the capture's own: a puppet create this early answers for a packet
        constexpr std::uint32_t kKeepMs = 3000;
        // The live world made it this recently, this near.
        constexpr std::uint32_t kWorldLiveMs = 2500;
        constexpr float kWorldNearM = 10.0f;
        constexpr double kLogEveryS = 60.0;
        constexpr std::size_t kOursMax = 8192;

        // Handle = slot | generation << 32.
        constexpr std::uint64_t kSlotMask = 0xFFFFFFFFull;
        inline std::size_t SlotOf(std::uint64_t handle) { return static_cast<std::size_t>(handle & kSlotMask); }

        struct SlotMeta
        {
            bool used = false;
            bool follow = false;     // placed on its puppet every tick
            std::uint32_t owner = census::kNoOwner;
            std::uint32_t startedMs = 0;
            float distance = 0.0f;
            bool longSound = false; // a loop or a recorded life of kLongSoundMs or more
            // SoundGenericEntityMode on this sound (applied: an owner's ESND or
            // SNDS packet), when the engine side made it (mod clock) and what
            // it did to it since.
            bool modeApplied = false;
            EntityMode mode = EntityMode::Detached;
            std::uint64_t createdUs = 0;
            std::uint64_t stoppedUs = 0; // our S13
            std::uint32_t adjusts = 0;
            std::uint32_t reapplies = 0; // mode b: the ticks written back
            bool updatesWithEntity = false;
            std::uint64_t puppetAddress = 0; // the owner's puppet at the create
            // The cap key (soundslots::CapKeyOf), the sound (stable id, else
            // raw ref), its family and recorded life, for the slot policy and
            // SOUND_GENERIC_STEAL.
            std::uint64_t capKey = 0;
            std::uint64_t ref = 0;
            soundowner::Family family = soundowner::Family::Owned;
            std::uint32_t lifeMs = soundworld::kNeverMs;
            census::OwnerClass cls = census::OwnerClass::World; // the slot limits and tallies by class
        };

        // Mode c: the sound the game's queue makes later for
        // the request, found among the census rows by (audio entity, ref);
        // alive while its slot `out` still holds it (no out: until our end).
        struct QueuedSound
        {
            bool active = false;
            std::uint64_t audio = 0;
            std::uint64_t ref = 0;
            std::uint64_t requestedUs = 0;
            std::uint64_t sound = 0;
            std::uint64_t out = 0;
            std::uint64_t bornUs = 0;
            std::uint64_t ticket = 0; // the request's, its sound comes back by it
            // The sound's identity at the birth (0: born
            // gone, or seen gone since); the sound is never used without it.
            std::uint64_t identity = 0;
        };

        // The engine side of the linked sound rule
        // (sound_generic_player_core.h): the identity is the back reference the
        // audio observer checks, the stop is S13 (which checks it again).
        struct EngineLinks : player::SoundLinks
        {
            std::uint64_t Identity(std::uint64_t sound) const override { return replay::audio::SoundIdentity(sound); }
            bool Stop(std::uint64_t sound) override
            {
                std::string error;
                return replay::audio::StopSound(sound, error);
            }
        };
        constexpr std::uint64_t kQueueBirthUs = 1500000; // never born after this: gone

        struct EngineCounters
        {
            std::uint64_t coreBByRef = 0;     // creator B packets created by ref
            std::uint64_t coreBByRefOk = 0;   // ... and created
            std::uint64_t keptAcrossJump = 0; // ... KeepAcrossJump said yes
            std::uint64_t keepRefused = 0;    // ... said no (queued, other puppet, gone)
            std::uint64_t creates = 0;
            std::uint64_t created = 0;
            std::uint64_t noPuppet = 0;
            std::uint64_t noPosition = 0;
            std::uint64_t noAudio = 0;
            std::uint64_t weaponEnvHolder = 0;   // a weapon packet asked its holder's group
            std::uint64_t weaponEnvNoHolder = 0; // no ready holder at the packet's time
            std::uint64_t vtableDiffers = 0;
            std::uint64_t fallbackStatic = 0;
            std::uint64_t noSlot = 0;
            std::uint64_t tooFar = 0;
            std::uint64_t stolen = 0;
            std::uint64_t refusedByEngine = 0;
            std::uint64_t prepares = 0;
            std::uint64_t prepareFailed = 0;
            std::uint64_t plays = 0;
            std::uint64_t playFailed = 0;
            std::uint64_t stopsS13 = 0;
            std::uint64_t stopsS14 = 0;
            std::uint64_t adjustApplied[kAdjustKinds] = {};
            std::uint64_t adjustFailed[kAdjustKinds] = {};
            std::uint64_t adjustIgnoredFollow = 0; // position adjusts of a sound that follows its puppet
            std::uint64_t adjustOriginSkipped = 0; // position adjusts at the map origin
            std::uint64_t censusRows = 0;
            std::uint64_t censusOurs = 0;
            std::string lastStatus;
            // Why S38 did not set a variable; where the init variables of a
            // create came from (the packet, another track's params, none);
            // the packets the distance refused at least once, and of those the
            // ones created later anyway.
            std::map<std::string, std::uint64_t> variableWhy;
            std::uint64_t initVarsPacket = 0;
            std::uint64_t initVarsTrack = 0;
            std::uint64_t initVarsNone = 0;
            std::uint64_t tooFarPackets = 0;
            std::uint64_t tooFarThenCreated = 0;
            float tooFarMaxM = 0.0f;
            // Where the value dwords came from; the environment group asked
            // for, by outcome; the requested settings read back after the
            // create (as_given, client_var_differs, env_group_differs,
            // update_flag_differs, unread); sounds that update with their
            // entity with and without the client variable the game passed.
            std::uint64_t valuesPacket = 0;
            std::uint64_t valuesTrack = 0;
            std::uint64_t valuesNone = 0;
            std::map<std::string, std::uint64_t> envStatus;
            std::map<std::string, std::uint64_t> settingsCheck;
            // The created sounds by the state of their dynamic wave bank at the
            // create (ProbeSoundBank).
            std::map<std::string, std::uint64_t> bankStates;
            std::uint64_t updateNoClientVar = 0;
            std::uint64_t updateWithClientVar = 0;
            // SoundGenericEntityMode.
            std::uint64_t modeCreates = 0;     // creates the mode applied to
            std::uint64_t detached = 0;        // a: the flag 0x04 taken off
            std::uint64_t overrideTicks = 0;   // b: sound ticks written back
            std::uint64_t overrideWrites = 0;  // b: S38/S18/S19 writes that took
            std::uint64_t overrideFailed = 0;
            std::uint64_t queueRequested = 0;  // c
            std::uint64_t queueRecorded = 0;   // c: with a track's params whole
            std::uint64_t queueRefused = 0;
            std::uint64_t queueBorn = 0;
            std::uint64_t queueBornNoOut = 0;
            std::uint64_t queueNeverBorn = 0;
            std::uint64_t queueBound = 0;   // the creator's sound tied to its packet by ticket
            std::uint64_t queueUnbound = 0; // born with no packet waiting (stopped at once)
            // Census rows taken as another source's sound
            // whose birth was ours, drained a tick later (a false double).
            std::uint64_t lateOurs = 0;
            std::uint64_t stolenLong = 0; // long sounds whose slot a new one took
            std::uint64_t worldLiveYields = 0; // world packets the live world already plays
            std::uint64_t censusOursLate = 0;
            std::uint64_t lateOursNear = 0; // of lateOurs, born within 10 ticks of the row // rows held back that turned out ours (not taken for another's)
            std::uint64_t directFull = 0;   // direct creates with the track's params whole
            std::uint64_t directFields = 0; // ... and without a row (the packet's fields)
            std::uint64_t mutedByKey = 0;   // SoundGenericMute
            std::uint64_t playerRouted = 0; // ... SoundGenericPlayerRoute: on the live player's audio
            std::uint64_t playerRouteFailed = 0;
            std::uint64_t directCategory = 0; // ... SoundGenericDirectParams=2: the row's category only
            std::uint64_t queueNetStops = 0; // S13 of the safety net (pause, scrub, jump, end)
            std::uint64_t queueByHash = 0;     // c by hash: no queue entry, made as a
            // The steals by rule (owner_cap of an owned packet, world_cap of a
            // world packet, farther) and the SOUND_GENERIC_STEAL lines written
            // and left out (budget).
            std::uint64_t ownerCapSteals = 0;
            std::uint64_t worldCapSteals = 0;
            std::uint64_t fartherSteals = 0;
            std::uint64_t worldVictims = 0;  // steals whose victim was a world packet
            std::uint64_t stealLines = 0;
            std::uint64_t stealLinesDropped = 0;
            std::uint64_t worldYieldsLong = 0; // world yields of a packet of kLongSoundMs or more
        };

        std::string Hex32(std::uint32_t value)
        {
            char text[16]{};
            std::snprintf(text, sizeof(text), "%08x", value);
            return text;
        }

        // The init variables of a clip older than SGEN v3.
        player::InitVariableIndex g_initIndex;
        constexpr std::uint32_t kInitMatchMs = 25;
        const char* InitSourceName(std::uint8_t source)
        {
            switch (source)
            {
            case 1: return "request";
            case 2: return "entity_sound";
            case 3: return "capture";
            }
            return "packet";
        }

        // Mode c's params whole, the other tracks' rows (SNDS, ESND, capture)
        // of the same owner and ref within 25 ms.
        struct FullParamsRow
        {
            std::uint32_t owner = 0;
            std::uint64_t ref = 0;
            std::uint32_t timeMs = 0;
            const std::uint8_t* params = nullptr;
            std::uint8_t source = 0;
        };
        std::vector<FullParamsRow> g_fullParams;

        const FullParamsRow* FindFullParams(std::uint32_t owner, std::uint64_t ref, std::uint32_t timeMs)
        {
            const FullParamsRow* best = nullptr;
            std::uint32_t bestGap = kInitMatchMs + 1;
            for (const FullParamsRow& row : g_fullParams)
            {
                if (row.owner != owner || row.ref != ref)
                    continue;
                const std::uint32_t gap = row.timeMs > timeMs ? row.timeMs - timeMs : timeMs - row.timeMs;
                if (gap < bestGap)
                {
                    best = &row;
                    bestGap = gap;
                }
            }
            return best;
        }

        // The engine nulls a slot when its sound ends, possibly after the
        // module moved on: the slots live on the process heap (the entity
        // sound track's EngineSoundSlots).
        // A reset abandons the array (never freed: the
        // engine may still null a slot of it) and takes a new one, so a sound
        // of the old session can never null a slot that holds a new sound.
        std::uint64_t* NewSlots()
        {
            static std::uint64_t fallback[kMaxSlots] = {};
            void* memory = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(fallback));
            return memory ? static_cast<std::uint64_t*>(memory) : fallback;
        }
        std::uint64_t* g_slots = nullptr;
        std::uint64_t* Slots()
        {
            if (!g_slots)
                g_slots = NewSlots();
            return g_slots;
        }

        float Distance(const Vector3& a, const Vector3& b)
        {
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        const replay::playback::Puppet* ReadyPuppet(Context& context, std::uint32_t id)
        {
            const replay::playback::Puppet* puppet = context.puppets.Find(replay::entity::ReplayId::FromPacked(id));
            return puppet && puppet->state == replay::playback::PuppetState::Ready && puppet->clone &&
                    replay::entities::Exists(puppet->clone)
                ? puppet
                : nullptr;
        }

        class EngineImpl : public player::Engine
        {
        public:
            void SetDirectParams(int mode) { m_directParams = mode; }
            void SetEnvRules(bool queue, bool weaponHolder)
            {
                m_queueEnv = queue;
                m_weaponHolderEnv = weaponHolder;
                replay::audio::SetQueueEnvGroup(queue);
            }
            void SetMute(const player::TraceWindow& mute) { m_mute = mute; }
            void SetPlayerRoute(bool on) { m_playerRoute = on; }
            void SetCoreBByRef(bool on) { m_coreBByRef = on; }
            // SoundGenericWorldCap (1: a world packet counts the cap with the
            // instances of its own sound; 0: with every world packet) and
            // SoundGenericWorldCapCount.
            void SetWorldCap(bool perSound, std::uint32_t count)
            {
                m_worldPerSound = perSound;
                m_worldCapCount = count;
            }
            bool WorldPerSound() const { return m_worldPerSound; }
            std::uint32_t WorldCapCount() const { return m_worldCapCount; }
            // The evidence lines of this tick (SOUND_GENERIC_STEAL, SOUND_GENERIC_YIELD_LOST).
            std::vector<std::string> TakeEvidence()
            {
                std::vector<std::string> out;
                out.swap(m_evidence);
                return out;
            }
            const soundworld::YieldCounters& Yields() const { return m_yields.Counters(); }

            // The long world packets that yielded to a live sound, watched
            // while their recorded life lasts: a live twin that ends early is
            // logged (SOUND_GENERIC_YIELD_LOST). Four times a second; a jump
            // forgets them.
            void CheckYields(std::uint32_t nowMs)
            {
                if (m_yieldCheckMs != 0xFFFFFFFFu && (nowMs < m_yieldCheckMs || nowMs > m_yieldCheckMs + 2000))
                    m_yields.Clear(); // a jump (or a long stall): the twins of another time
                if (m_yieldCheckMs != 0xFFFFFFFFu && nowMs >= m_yieldCheckMs && nowMs < m_yieldCheckMs + 250)
                    return;
                m_yieldCheckMs = nowMs;
                if (!m_yields.Size())
                    return;
                m_liveScratch.clear();
                for (const census::LiveSound& l : census::Live())
                    m_liveScratch.push_back(l.sound);
                std::sort(m_liveScratch.begin(), m_liveScratch.end());
                std::vector<soundworld::LostYield> lost;
                m_yields.Check(nowMs, [this](std::uint64_t sound) {
                    return std::binary_search(m_liveScratch.begin(), m_liveScratch.end(), sound);
                }, lost);
                for (const soundworld::LostYield& l : lost)
                    if (m_yieldBudget.Take())
                        m_evidence.push_back(soundworld::FormatLostYield(l));
            }

            // SoundGenericKeepLoops: a live sound goes on across a jump when
            // the engine still holds it, it is not a queued request (mode c's
            // net stops those) and the packet's owner has the same puppet it
            // was made on (a jump that rebuilt the puppet leaves the old sound
            // to the stop).
            bool KeepAcrossJump(std::uint64_t handle) override
            {
                const std::ptrdiff_t slot = Current(handle);
                bool keep = slot >= 0 && Held(static_cast<std::size_t>(slot)) && !m_queued[static_cast<std::size_t>(slot)].active;
                if (keep)
                {
                    const SlotMeta& meta = m_meta[static_cast<std::size_t>(slot)];
                    if (meta.owner != census::kNoOwner)
                    {
                        const replay::playback::Puppet* puppet = ReadyPuppet(*m_context, meta.owner);
                        keep = puppet && reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet->clone)) == meta.puppetAddress;
                    }
                }
                ++(keep ? m_counters.keptAcrossJump : m_counters.keepRefused);
                return keep;
            }

            void Begin(Context& context, std::uint32_t slots, bool envGroups, EntityMode mode)
            {
                m_context = &context;
                m_envGroups = envGroups;
                m_mode = mode;
                m_slots = slots > kMaxSlots ? kMaxSlots : slots < 1 ? 1 : slots;
                m_stale = player::StaleCounters{}; // per playback
                DrainBirths(); // births after the last playback's end, none waits: stopped
                StopAllSlots();
                for (SlotMeta& meta : m_meta)
                    meta = SlotMeta{};
                m_own.Clear();
                m_ours.clear();
                std::fill(std::begin(m_recentOthers), std::end(m_recentOthers), std::uint64_t{0});
                m_recentOthersNext = 0;
                m_waiting.clear();
                m_otherPaths.clear();
                m_counters = EngineCounters{};
                m_evidence.clear();
                m_stealBudget.Reset(kStealLines);
                m_yieldBudget.Reset(kYieldLostLines);
                m_yields.Reset();
                m_yieldCheckMs = 0xFFFFFFFFu;
                m_nextLifeMs = soundworld::kNeverMs;
                replay::audio::ResetUnticketedPuppetCreates();
                m_cameraMs = 0xFFFFFFFFu;
                m_tooFar.clear();
                for (std::string& detail : m_detail)
                    detail.clear();
                m_classSlots.Clear();
                m_vehicles.Clear();
            }

            void StopAllSlots()
            {
                std::uint64_t* slots = Slots();
                for (std::size_t n = 0; n < kMaxSlots; ++n)
                {
                    replay::audio::StopPersistentSound(&slots[n]);
                    StopQueued(n);
                    m_meta[n].used = false;
                }
                StopAllQueued();
            }

            // The safety net: every live mode c sound stops (S13), bound or
            // not; a request still waiting keeps its ticket, its sound is
            // stopped when born (DrainBirths).
            void StopAllQueued() override
            {
                for (std::size_t n = 0; n < kMaxSlots; ++n)
                    if (m_queued[n].active)
                        StopQueued(n);
                // Each only while it is still the same sound.
                const std::uint64_t before = m_stale.netStops;
                m_net.StopAll(m_links, m_stale);
                m_counters.queueNetStops += m_stale.netStops - before;
            }

            // The creator's sounds for mode c's tickets: tied to the slot whose
            // request waits for that ticket (its adjusts, stop, forget and
            // release apply), else stopped at once: its packet's recorded end
            // already came (released, stopped, stolen, scrubbed) or its
            // playback is gone.
            void DrainBirths()
            {
                replay::audio::CollectQueueBirthsSince(m_birthCursor, m_births);
                for (const replay::audio::QueueBirth& birth : m_births)
                {
                    // A sound FeedCensus already took for another source's (the
                    // anti-double and yield rules).
                    for (std::size_t r = 0; r < kRecentOthers; ++r)
                        if (m_recentOthers[r] && m_recentOthers[r] == birth.sound)
                        {
                            ++m_counters.lateOurs;
                            const std::uint64_t age = m_feedTick - m_recentOtherTick[r];
                            if (age <= 10)
                                ++m_counters.lateOursNear; // a late birth, not a reused address
                            break;
                        }
                    bool bound = false;
                    for (std::size_t n = 0; n < kMaxSlots && !bound; ++n)
                    {
                        QueuedSound& q = m_queued[n];
                        if (!q.active || q.sound || q.ticket != birth.ticket)
                            continue;
                        q.sound = birth.sound;
                        q.out = birth.out;
                        q.bornUs = replay::clock::NowUs();
                        {
                            // The identity now; a sound
                            // the engine already ended is born gone.
                            player::LinkedSound link;
                            player::Link(link, birth.sound, m_links, m_stale);
                            q.identity = link.identity;
                            m_net.Insert(link);
                        }
                        ++m_counters.queueBorn;
                        if (!q.out)
                            ++m_counters.queueBornNoOut;
                        ++m_counters.queueBound;
                        if (m_ours.size() >= kOursMax)
                            m_ours.clear();
                        m_ours.insert(q.sound);
                        bound = true;
                    }
                    if (bound)
                        continue;
                    ++m_counters.queueUnbound;
                    if (m_ours.size() >= kOursMax)
                        m_ours.clear();
                    m_ours.insert(birth.sound); // not a puppet's own for the anti-double rule
                    if (player::StopUnbound(birth.sound, m_links, m_stale)) // if still alive
                        ++m_counters.queueNetStops;
                }
            }

            // The live sound of a slot: the one the slot holds, or mode c's
            // sound the queue made (while its slot holds it).
            std::uint64_t SoundOf(std::size_t slot) const
            {
                const std::uint64_t held = Slots()[slot];
                if (held || !m_queued[slot].active)
                    return held;
                const QueuedSound& q = m_queued[slot];
                if (!q.sound)
                    return 0;
                if (q.out)
                {
                    std::uint64_t now = 0;
                    if (!census::ReadQword(q.out, now) || now != q.sound)
                        return 0;
                }
                // No slot of the engine names it (a birth has none): its
                // identity must still hold.
                return player::LiveSound(player::LinkedSound{q.sound, q.identity}, m_links);
            }

            // The slot is busy: a sound it holds, or a queued request still
            // waiting for its sound or whose sound lives.
            bool Held(std::size_t slot) const
            {
                if (Slots()[slot])
                    return true;
                const QueuedSound& q = m_queued[slot];
                if (!q.active)
                    return false;
                if (!q.sound)
                    return replay::clock::NowUs() < q.requestedUs + kQueueBirthUs;
                return SoundOf(slot) != 0;
            }

            void StopQueued(std::size_t slot)
            {
                QueuedSound& q = m_queued[slot];
                if (!q.active)
                    return;
                if (q.sound)
                {
                    // S13 only on the same live sound (the queue's slot keeps
                    // its reference); a gone one is counted (stop_skipped) and
                    // let go without a call.
                    player::LinkedSound link{q.sound, SoundOf(slot) ? q.identity : 0};
                    player::StopLinked(link, m_links, m_stale);
                    m_net.Erase(q.sound);
                }
                // Not born yet: its ticket stays open, its sound
                // is stopped when born (DrainBirths, unbound).
                q = QueuedSound{};
            }


            // Mode c's queued sounds are raw engine pointers: after a script
            // restart (a save loaded) they are forgotten, never stopped. The
            // persistent slots are the engine's weak references (it nulls a
            // slot when the sound ends): the slots still set are abandoned with
            // their array (a new one is taken) and nothing is called on the
            // engine; the queued sounds and the safety net are forgotten.
            std::uint32_t ForgetAfterRestart()
            {
                std::uint32_t forgotten = 0;
                std::uint32_t slotsHeld = 0;
                std::uint64_t* slots = Slots();
                for (std::size_t n = 0; n < kMaxSlots; ++n)
                {
                    forgotten += slots[n] || m_queued[n].active ? 1u : 0u;
                    slotsHeld += slots[n] ? 1u : 0u;
                    m_queued[n] = QueuedSound{};
                    m_meta[n] = SlotMeta{};
                }
                if (slotsHeld)
                    g_slots = NewSlots();
                m_abandonedSlots = slotsHeld;
                m_net.Forget(m_stale);
                m_own.Clear();
                m_ours.clear();
                m_yields.Clear(); // raw sounds of the old session
                return forgotten;
            }
            const EngineCounters& Counters() const { return m_counters; }
            // vehicle_sound_rule.h: the slot outcomes by owner class and the
            // vehicles' and train cars' rows.
            const vehiclesound::ClassSlotTally& ClassSlots() const { return m_classSlots; }
            vehiclesound::VehicleTally& Vehicles() { return m_vehicles; }
            const std::map<std::string, std::uint32_t>& OtherPaths() const { return m_otherPaths; }
            // The volume probe names who made a voice (VOLUME_PROBE_SRC).
            bool Ours(std::uint64_t sound) const { return sound && m_ours.count(sound) != 0; }
            const player::StaleCounters& Stale() const { return m_stale; }
            std::uint32_t AbandonedSlots() const { return m_abandonedSlots; }

            // The census rows of this tick: what other sources made (the
            // anti-double rule and the double detector); our own creates
            // are known by their sound.
            // A row is another source's only after it waited kOtherWaitTicks
            // ticks: the birth of our own mode c sound can be drained a tick
            // after the census saw its create (bench, 004-Colter: 271 rows
            // taken for another source's were our births, which made the
            // doubles of marks 02 and 17 and yields of our own).
            void FeedCensus(std::uint32_t nowMs, player::DoubleDetector& doubles)
            {
                DrainBirths(); // before the rows (their sounds are ours)
                ++m_feedTick;
                for (const census::TickRow& tick : census::Rows())
                {
                    if (!tick.row.sound)
                        continue;
                    ++m_counters.censusRows;
                    if (m_ours.count(tick.row.sound))
                    {
                        ++m_counters.censusOurs;
                        continue;
                    }
                    std::uint32_t owner = census::kNoOwner;
                    if (tick.owner.kind == OwnerKind::Puppet)
                        owner = tick.owner.id;
                    else if (tick.owner.kind == OwnerKind::LivePlayer)
                        continue; // the hidden live player is nobody's packet
                    // The sound is its set and entry (a hoof
                    // step does not answer for the skid of the same surface set).
                    WaitingRow w;
                    w.sound = tick.row.sound;
                    w.owner = owner;
                    w.key = soundowner::SoundKey(tick.row.ref);
                    w.at = tick.clipMs > 0.0 ? static_cast<std::uint32_t>(tick.clipMs) : 0;
                    w.tick = m_feedTick;
                    // The creator frames go with it, for
                    // the evidence line of a double.
                    for (std::size_t f = 0; f < player::DoubleDetector::kSourceFrames && f < census::kRowCallers; ++f)
                        w.source.frames[f] = tick.row.callers[f];
                    w.source.timeMs = w.at;
                    w.source.mine = tick.row.mine;
                    w.hasPosition = tick.row.hasParams;
                    std::memcpy(w.position, tick.row.position, sizeof(w.position));
                    w.source.audio = tick.row.audio;
                    for (std::size_t f = 0; f < 3 && f + 2 < census::kRowCallers; ++f)
                        w.frames3[f] = tick.row.callers[f + 2];
                    w.source.base = static_cast<std::uint8_t>(tick.owner.base);
                    w.source.field = tick.owner.field;
                    w.cls = tick.owner.cls;
                    if (m_waiting.size() < kMaxWaiting)
                        m_waiting.push_back(w);
                }
                std::size_t kept = 0;
                for (std::size_t n = 0; n < m_waiting.size(); ++n)
                {
                    WaitingRow& w = m_waiting[n];
                    if (m_ours.count(w.sound))
                    {
                        ++m_counters.censusOursLate;
                        continue;
                    }
                    if (m_feedTick - w.tick < kOtherWaitTicks)
                    {
                        m_waiting[kept++] = w;
                        continue;
                    }
                    m_own.Add(w.owner, w.key, w.at, w.hasPosition ? w.position : nullptr, w.sound); // the sound, for the yield watch
                    if (w.owner != census::kNoOwner) // who makes the puppets' own
                        ++m_otherPaths[OtherPathKey(w.frames3)];
                    m_recentOtherTick[m_recentOthersNext % kRecentOthers] = m_feedTick;
                    m_recentOthers[m_recentOthersNext++ % kRecentOthers] = w.sound;
                    // A copy after our start, per vehicle.
                    if (doubles.NoteOther(player::DoubleDetector::KeyOf(w.owner, w.key), w.at, w.source))
                        m_vehicles.NoteLateDouble(w.owner, w.cls);
                }
                m_waiting.resize(kept);
                m_own.Forget(nowMs, kKeepMs);
            }

            // Mode c's sound is tied by its ticket in DrainBirths (matching
            // census rows by audio entity and ref misses requests born after
            // their slot let go, and nobody would stop them).

            // The sounds that follow their puppet, placed every tick; a slot
            // the engine cleared is free again.
            void Follow(std::uint32_t nowMs)
            {
                std::uint64_t* slots = Slots();
                const Vector3 camera = Camera(nowMs);
                for (std::size_t n = 0; n < m_slots; ++n)
                {
                    SlotMeta& meta = m_meta[n];
                    if (!meta.used)
                        continue;
                    if (!Held(n))
                    {
                        if (m_queued[n].active && !m_queued[n].sound)
                            ++m_counters.queueNeverBorn;
                        if (m_queued[n].sound)
                        {
                            m_net.Erase(m_queued[n].sound); // its slot let go of it
                            if (m_queued[n].identity)
                                ++m_stale.seenGone; // the engine ended it
                        }
                        m_queued[n] = QueuedSound{};
                        meta.used = false;
                        continue;
                    }
                    // Mode b, the latest recorded values back
                    // over what the owner's audio entity computed this frame.
                    if (meta.modeApplied && meta.mode == EntityMode::Override)
                        Reapply(n);
                    if (!slots[n])
                        continue; // mode c: the game places its sound
                    if (!meta.follow || meta.owner == census::kNoOwner)
                        continue;
                    const replay::playback::Puppet* puppet = ReadyPuppet(*m_context, meta.owner);
                    if (!puppet)
                        continue;
                    const Vector3 at = ENTITY::GET_ENTITY_COORDS(puppet->clone, TRUE, TRUE);
                    const float position[4] = {at.x, at.y, at.z, 0.0f};
                    replay::audio::SetSoundPosition(slots[n], position);
                    meta.distance = Distance(at, camera);
                }
            }

            // ---- player::Engine ----

            // Mode b's write back of one sound.
            void Reapply(std::size_t slot)
            {
                const std::uint64_t sound = Slots()[slot];
                if (!sound)
                    return;
                const player::OverrideState& state = m_override[slot];
                std::uint64_t ok = 0, failed = 0;
                for (std::size_t v = 0; v < state.VariableCount(); ++v)
                {
                    const Variable& var = state.VariableAt(v);
                    ++(std::strcmp(replay::audio::WriteSoundVariableWhy(sound, var.hash, var.value), "ok") == 0 ? ok : failed);
                }
                ++(replay::audio::SetSoundVolume(sound, state.volume) ? ok : failed);
                ++(replay::audio::SetSoundPitch(sound, state.pitch) ? ok : failed);
                m_counters.overrideWrites += ok;
                m_counters.overrideFailed += failed;
                ++m_counters.overrideTicks;
                ++m_meta[slot].reapplies;
            }

            void SetCreateFamily(soundowner::Family family) override { m_family = family; }

            void NoteNextLife(std::uint32_t lifeMs) override
            {
                m_nextLong = lifeMs >= replay::soundslots::kLongSoundMs;
                m_nextLifeMs = lifeMs;
            }

            bool Create(const CreatePacket& packet, bool play, std::uint64_t& handle, std::string& status, bool& retry) override
            {
                ++m_counters.creates;
                retry = false;
                // SoundGenericMute (ablation): the packets of an owner in a
                // clip window are not created, so the bench measures what they
                // add to the replay's audio.
                if (m_mute.Holds(packet.owner, packet.timeMs))
                {
                    ++m_counters.mutedByKey;
                    status = "muted_by_key";
                    return false;
                }
                const bool owned = packet.owner != census::kNoOwner;
                const soundowner::Family family = m_family; // for the slot's meta
                // SoundGenericEntityMode on this packet.
                const bool modeApplied = player::EntityModeApplies(m_family, packet.owner);
                const EntityMode mode = modeApplied ? m_mode : EntityMode::Detached;
                m_family = soundowner::Family::Owned; // one Create per SetCreateFamily
                const bool queueMode = modeApplied && mode == EntityMode::GameQueue && packet.idKind != IdKind::Hash &&
                    !(packet.idKind == IdKind::None && packet.core != 0);
                if (queueMode && !play)
                {
                    // The queue cannot preload: the normal path asks it at the packet's time.
                    status = "queue_at_start";
                    retry = true;
                    return false;
                }
                const replay::playback::Puppet* puppet = nullptr;
                std::uint64_t address = 0;
                if (owned)
                {
                    puppet = ReadyPuppet(*m_context, packet.owner);
                    if (!puppet)
                    {
                        // The puppet may come a few frames late: tried again
                        // while the packet's window lasts.
                        ++m_counters.noPuppet;
                        status = "no_puppet";
                        retry = true;
                        return false;
                    }
                    address = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet->clone));
                }
                Vector3 at{};
                bool placed = false;
                if (puppet)
                {
                    at = ENTITY::GET_ENTITY_COORDS(puppet->clone, TRUE, TRUE);
                    placed = true;
                }
                else if (packet.hasPosition)
                {
                    at.x = packet.params.position[0];
                    at.y = packet.params.position[1];
                    at.z = packet.params.position[2];
                    placed = true;
                }
                if (!placed)
                {
                    // A sound of nobody and nowhere (the interface, a 2D
                    // sound): nothing to place it on.
                    ++m_counters.noPosition;
                    status = "no_position";
                    return false;
                }
                // The audio entity: the owner's own (the recorded base on
                // the puppet, vtable checked), a static one of the image,
                // else the static collision audio placed on the owner.
                bool follow = packet.params.hasTracker || (owned && !packet.hasPosition);
                // Modes a and c do not place the sound on the
                // puppet every tick: a by the recorded S16, c the game itself.
                const bool detached = modeApplied && mode != EntityMode::Override;
                bool fallback = false;
                std::uint64_t audio = 0;
                // SoundGenericPlayerRoute=1, the recorded player's sounds on
                // the hidden live player's own audio entity (the player's sound
                // sets came 7 to 8 dB under on the puppet, an NPC rider's did
                // not), placed on the puppet every tick.
                bool viaLivePlayer = false;
                if (m_playerRoute && packet.ownerClass == census::OwnerClass::Player && packet.audioKnown && puppet)
                {
                    const Ped live = PLAYER::PLAYER_PED_ID();
                    void* liveAddress = live ? replay::entities::AddressOf(live) : nullptr;
                    std::string why;
                    if (liveAddress)
                        audio = AudioOfPed(packet, liveAddress, reinterpret_cast<std::uint64_t>(liveAddress), why);
                    viaLivePlayer = audio != 0;
                    ++(viaLivePlayer ? m_counters.playerRouted : m_counters.playerRouteFailed);
                }
                if (packet.audioKnown && !audio)
                    audio = AudioOf(packet, puppet, address, status);
                if (!audio)
                {
                    audio = replay::audio::StaticCollisionAudio();
                    fallback = audio != 0;
                    if (!audio)
                    {
                        ++m_counters.noAudio;
                        status = "no_audio";
                        return false;
                    }
                    if (owned)
                        follow = true;
                }
                if (detached && !fallback)
                    follow = false;
                if (viaLivePlayer)
                    follow = true; // on the puppet, not where the live player stands
                // The slot (the entity sound track's policy).
                const Vector3 camera = Camera(m_context->transport.PlayheadMs() > 0xFFFFFFFFull
                        ? 0xFFFFFFFFu : static_cast<std::uint32_t>(m_context->transport.PlayheadMs()));
                const float distance = Distance(at, camera);
                std::uint64_t* slots = Slots();
                replay::soundslots::SlotView views[kMaxSlots];
                for (std::size_t n = 0; n < m_slots; ++n)
                {
                    if (m_meta[n].used && !Held(n)) // a queued request holds its slot
                        m_meta[n].used = false;
                    views[n].used = m_meta[n].used;
                    views[n].owner = m_meta[n].owner;
                    views[n].distance = m_meta[n].distance;
                    views[n].startedMs = m_meta[n].startedMs;
                    views[n].longSound = m_meta[n].longSound;
                    views[n].capKey = m_meta[n].capKey;
                }
                // SoundGenericWorldCap: a world packet counts the cap with the
                // other instances of its own sound, not with every ownerless
                // packet (a gallop loop could be stolen by wind gusts).
                const std::uint64_t soundKey = soundowner::PacketSoundKey(packet.stableId, packet.rawRef);
                const std::uint64_t capKey =
                    replay::soundslots::CapKeyOf(packet.owner, census::kNoOwner, soundKey, m_worldPerSound);
                const bool worldKey = replay::soundslots::IsWorldCapKey(capKey);
                // vehicle_sound_rule.h: the cap and the audible distance by
                // owner class (a train car's sounds were stolen by the ped cap
                // of 6, the locomotive refused at 120 m with the camera on the
                // train).
                const vehiclesound::SlotLimits limits = vehiclesound::LimitsFor(packet.ownerClass, worldKey, m_worldCapCount);
                const replay::soundslots::Choice choice =
                    replay::soundslots::ChooseByKey(views, m_slots, capKey, distance, limits.cap, limits.maxDistanceM);
                if (choice.slot < 0)
                {
                    if (choice.reason == replay::soundslots::Reason::TooFar)
                    {
                        ++m_counters.tooFar;
                        // Per packet, not per tick tried
                        const bool newPacket = m_tooFar.insert(packet.index).second;
                        if (newPacket)
                        {
                            ++m_counters.tooFarPackets;
                            m_vehicles.NoteTooFar(packet.owner, packet.ownerClass);
                        }
                        if (distance == distance && distance > m_counters.tooFarMaxM)
                            m_counters.tooFarMaxM = distance;
                        m_classSlots.NoteTooFar(packet.ownerClass, distance, newPacket);
                    }
                    else
                        ++m_counters.noSlot;
                    status = replay::soundslots::ReasonName(choice.reason);
                    retry = true; // the window decides
                    return false;
                }
                const std::size_t slot = static_cast<std::size_t>(choice.slot);
                if (choice.Steals())
                {
                    if (views[slot].longSound) // a loop taken (no short one of the owner was left)
                        ++m_counters.stolenLong;
                    // By rule, and one line per steal.
                    const SlotMeta& victim = m_meta[slot];
                    if (choice.reason == replay::soundslots::Reason::Farther)
                    {
                        ++m_counters.fartherSteals;
                        ++m_classSlots.Of(victim.cls).fartherVictims;
                        ++m_classSlots.Of(packet.ownerClass).fartherTakers;
                    }
                    else
                    {
                        ++(worldKey ? m_counters.worldCapSteals : m_counters.ownerCapSteals);
                        ++m_classSlots.Of(packet.ownerClass).capSteals;
                    }
                    m_vehicles.NoteStealLost(victim.owner, victim.cls);
                    if (victim.owner == census::kNoOwner)
                        ++m_counters.worldVictims;
                    if (m_stealBudget.Take())
                    {
                        soundworld::StealView s;
                        s.playheadMs = static_cast<std::uint32_t>(m_context->transport.PlayheadMs());
                        s.reason = choice.reason == replay::soundslots::Reason::Farther ? "farther"
                            : worldKey ? "world_cap" : "owner_cap";
                        s.capKey = capKey;
                        s.victimOwner = victim.owner;
                        s.victimRef = victim.ref;
                        s.victimFamily = soundowner::FamilyName(victim.family);
                        s.victimLong = victim.longSound;
                        s.victimStartedMs = victim.startedMs;
                        s.victimLifeMs = victim.lifeMs;
                        s.victimDistanceM = victim.distance;
                        s.newOwner = packet.owner;
                        s.newRef = packet.stableId ? packet.stableId : packet.rawRef;
                        s.newFamily = soundowner::FamilyName(family);
                        s.newDistanceM = distance;
                        m_evidence.push_back(soundworld::FormatSteal(s));
                        ++m_counters.stealLines;
                    }
                    else
                        ++m_counters.stealLinesDropped;
                    replay::audio::StopPersistentSound(&slots[slot]);
                    StopQueued(slot);
                    ++m_counters.stolen;
                }
                m_queued[slot] = QueuedSound{};
                replay::audio::GenericSoundInit init;
                init.hasPosition = packet.hasPosition;
                std::memcpy(init.position, packet.params.position, sizeof(init.position));
                init.volume = packet.params.volume;
                init.volume2 = packet.params.volume2;
                init.pitch = packet.params.pitch;
                init.pan = packet.params.pan;
                init.flags = packet.params.flags;
                if (viaLivePlayer)
                    init.flags = player::DetachedFlags(init.flags); // the live player's entity would move it to him
                // Mode a, no update by the owner's audio entity; the position
                // is the packet's or the puppet's at the create, the recorded
                // S16 move it.
                if (modeApplied && mode == EntityMode::Detached)
                {
                    if (init.flags & kParamsFlagUpdateEntity)
                        ++m_counters.detached;
                    init.flags = player::DetachedFlags(init.flags);
                    if (!init.hasPosition)
                    {
                        init.hasPosition = true;
                        init.position[0] = at.x;
                        init.position[1] = at.y;
                        init.position[2] = at.z;
                    }
                }
                init.variableCount = packet.params.variableCount;
                for (std::size_t n = 0; n < kVariables; ++n)
                {
                    init.variableHash[n] = packet.params.variables[n].hash;
                    init.variableValue[n] = packet.params.variables[n].value;
                }
                // The init variables, the packet's (SGEN v3) or,
                // for an older clip, those of the other track's row of this sound.
                InitParams vars;
                std::uint8_t varsSource = 0;
                // The row also gives the value dwords to a packet older than SGEN v4.
                InitParams row;
                std::uint8_t rowSource = 0;
                const bool haveRow = (packet.params.initVariableCount == 0 || !packet.params.valuesKnown) &&
                    g_initIndex.Find(packet.owner, packet.rawRef, packet.timeMs, kInitMatchMs, row, rowSource);
                bool haveVars = packet.params.initVariableCount != 0;
                if (haveVars)
                    vars = packet.params;
                else if (haveRow && row.initVariableCount)
                {
                    vars = row;
                    varsSource = rowSource;
                    haveVars = true;
                }
                init.initVariableCount = haveVars ? vars.initVariableCount : 0;
                for (std::size_t n = 0; n < init.initVariableCount && n < kInitVariables; ++n)
                {
                    init.initVariableHash[n] = vars.initVariables[n].hash;
                    init.initVariableValue[n] = vars.initVariables[n].value;
                }
                // The value dwords (client variable +0xF8, the
                // tail, the prepare limits...): the packet's (SGEN v4), else the row's.
                const InitParams* values = packet.params.valuesKnown ? &packet.params
                    : haveRow && row.valuesKnown ? &row : nullptr;
                init.valueCount = 0;
                if (values)
                {
                    for (std::size_t n = 0; n < values->valueCount && n < kValueDwords; ++n)
                    {
                        init.valueIndex[init.valueCount] = values->values[n].index;
                        init.valueRaw[init.valueCount] = values->values[n].raw;
                        ++init.valueCount;
                    }
                    ++(packet.params.valuesKnown ? m_counters.valuesPacket : m_counters.valuesTrack);
                }
                else
                    ++m_counters.valuesNone;
                // The environment group. The game gives one to
                // every ped request it runs (queue 0x312D9B6: the owner's, else
                // one for the position) and to the creates the recorder saw with
                // one; the player gives the owner puppet's.
                const bool wantEnv = m_envGroups && owned && address &&
                    (packet.params.hasEnvGroup || packet.source == Source::Deferred);
                init.envGroupEntity = wantEnv ? address : 0;
                // A weapon's sound asks the ped that holds the weapon puppet
                // (the live game gives the holder's group, a weapon object has
                // none).
                const bool weaponOwner = puppet && puppet->packet.kind == replay::entity::EntityKind::Weapon;
                std::uint32_t holderId = 0;
                if (wantEnv && weaponOwner && m_weaponHolderEnv)
                {
                    if (const replay::playback::Puppet* holder = WeaponHolder(*puppet, packet.timeMs))
                    {
                        init.envGroupEntity = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(holder->clone));
                        holderId = holder->packet.id.Packed();
                        ++m_counters.weaponEnvHolder;
                    }
                    else
                        ++m_counters.weaponEnvNoHolder;
                }
                // The sound: the stable id back to this session's ref.
                // sound_generic_core.h CreateRefOf a creator B packet carries
                // the resolved ref: by ref (SoundGenericCoreBByRef).
                bool byHash = false;
                const std::uint64_t ref =
                    CreateRefOf(packet.idKind, packet.stableId, packet.rawRef, packet.core, m_coreBByRef, byHash);
                const bool coreBByRef = m_coreBByRef && packet.core != 0;
                if (coreBByRef)
                    ++m_counters.coreBByRef;
                std::string error;
                bool ok = false;
                if (modeApplied)
                    ++m_counters.modeCreates;
                if (modeApplied && mode == EntityMode::GameQueue && !queueMode)
                    ++m_counters.queueByHash; // made as mode a would not: the direct create, as before
                std::string queueDetail;
                if (queueMode)
                {
                    // Mode c, the game's queue with the track's params whole
                    // (else the packet's fields).
                    const FullParamsRow* full = FindFullParams(packet.owner, packet.rawRef, packet.timeMs);
                    ++m_counters.queueRequested;
                    if (full)
                        ++m_counters.queueRecorded;
                    std::uint64_t ticket = 0;
                    ok = replay::audio::QueueGenericSound(audio, ref, full ? full->params : nullptr, init, error, &ticket);
                    queueDetail = std::string(",queue=") + error + "(" + (full ? InitSourceName(full->source) : "packet_fields") + ")";
                    if (ok)
                    {
                        QueuedSound& q = m_queued[slot];
                        q.active = true;
                        q.audio = audio;
                        q.ref = ref;
                        q.requestedUs = replay::clock::NowUs();
                        q.ticket = ticket;
                    }
                    else
                        ++m_counters.queueRefused;
                }
                else
                {
                    // SoundGenericDirectParams=1, the direct create with the
                    // track's params whole when a row kept them.
                    if (m_directParams && owned)
                    {
                        const FullParamsRow* full = FindFullParams(packet.owner, packet.rawRef, packet.timeMs);
                        if (m_directParams == 3)
                        {
                            init.listParams = full ? full->params : nullptr;
                            ++(full ? m_counters.directCategory : m_counters.directFields);
                        }
                        else if (m_directParams == 2)
                        {
                            if (full)
                                std::memcpy(&init.category, full->params + 0xD8, sizeof(init.category));
                            ++(init.category ? m_counters.directCategory : m_counters.directFields);
                        }
                        else
                        {
                            init.recordedParams = full ? full->params : nullptr;
                            ++(full ? m_counters.directFull : m_counters.directFields);
                        }
                    }
                    census::ModScope mine; // the census marks it the mod's (not a puppet's own)
                    ok = replay::audio::CreateGenericSound(audio, ref, byHash, init, play, &slots[slot], error);
                }
                if (ok && coreBByRef)
                    ++m_counters.coreBByRefOk;
                if (!ok)
                {
                    ++m_counters.refusedByEngine;
                    status = error;
                    m_counters.lastStatus = status;
                    return false;
                }
                ++m_counters.created;
                if (fallback)
                    ++m_counters.fallbackStatic;
                if (m_tooFar.count(packet.index))
                    ++m_counters.tooFarThenCreated;
                ++(!haveVars ? m_counters.initVarsNone : varsSource ? m_counters.initVarsTrack : m_counters.initVarsPacket);
                // Did the engine take what the game gave? The
                // requested settings read back (client variable, environment
                // group, update with the entity) against the params passed.
                {
                    if (wantEnv)
                        ++m_counters.envStatus[init.envGroupStatus];
                    if (wantEnv && weaponOwner && m_weaponEnvLines < 200) // SHOT_ENV
                    {
                        ++m_weaponEnvLines;
                        m_evidence.push_back("SHOT_ENV,phase=play,path=" + std::string(queueMode ? "queue" : "direct") +
                            ",owner=" + replay::entity::IdText(replay::entity::ReplayId::FromPacked(packet.owner)) +
                            ",holder=" + (holderId ? replay::entity::IdText(replay::entity::ReplayId::FromPacked(holderId)) : std::string("-")) +
                            ",recorded_env=" + (packet.params.hasEnvGroup ? "1" : "0") + ",caller=0x" + Hex32(packet.caller) +
                            ",given=" + (init.envGroupGiven ? "1" : "0") + ",status=" + init.envGroupStatus +
                            ",holder_rule=" + (m_weaponHolderEnv ? "1" : "0") + ",t=" + std::to_string(packet.timeMs));
                    }
                    const std::uint32_t wantClient = values ? ValueAt(*values, kParamsClientVariable) : 0;
                    const bool wantUpdate = (init.flags & kParamsFlagUpdateEntity) != 0; // what was passed
                    replay::audio::GenericSettingsView view;
                    const char* check = queueMode ? "queued" : "unread";
                    if (!queueMode && replay::audio::ReadGenericSettings(slots[slot], view))
                        check = view.clientVariable != wantClient ? "client_var_differs"
                            : (init.envGroupGiven != 0) != (view.envGroup != 0) ? "env_group_differs"
                            : view.updatesWithEntity != wantUpdate ? "update_flag_differs"
                            : "as_given";
                    ++m_counters.settingsCheck[check];
                    if (wantUpdate && !wantClient)
                        ++m_counters.updateNoClientVar;
                    if (wantUpdate && values && wantClient)
                        ++m_counters.updateWithClientVar;
                    m_lastSettings = std::string(check) + ",client_var=0x" + Hex32(view.clientVariable) +
                        ",env=" + (view.envGroup ? "1" : "0") + "(" + init.envGroupStatus + "),update_entity=" +
                        (view.updatesWithEntity ? "1" : "0") + ",values=" + std::to_string(init.valueCount) + "(" +
                        (!values ? "none" : packet.params.valuesKnown ? "packet" : InitSourceName(rowSource)) + ")";
                }
                SlotMeta& meta = m_meta[slot];
                meta.used = true;
                meta.follow = follow;
                meta.owner = packet.owner;
                meta.startedMs = static_cast<std::uint32_t>(m_context->transport.PlayheadMs());
                meta.longSound = m_nextLong;
                meta.distance = distance;
                meta.puppetAddress = address;
                meta.capKey = capKey;
                meta.ref = packet.stableId ? packet.stableId : packet.rawRef;
                meta.family = family;
                meta.lifeMs = m_nextLifeMs;
                meta.cls = packet.ownerClass;
                ++m_classSlots.Of(packet.ownerClass).placed;
                meta.modeApplied = modeApplied;
                meta.mode = mode;
                meta.createdUs = replay::clock::NowUs();
                meta.stoppedUs = 0;
                meta.adjusts = 0;
                meta.reapplies = 0;
                meta.updatesWithEntity = !queueMode && m_lastSettings.find("update_entity=1") != std::string::npos;
                if (modeApplied && mode == EntityMode::Override)
                    m_override[slot].Seed(packet.params, haveVars ? &vars : nullptr);
                if (m_ours.size() >= kOursMax)
                    m_ours.clear();
                if (slots[slot])
                    m_ours.insert(slots[slot]);
                if (slots[slot] && (follow || (fallback && !packet.hasPosition)))
                {
                    const float position[4] = {at.x, at.y, at.z, 0.0f};
                    replay::audio::SetSoundPosition(slots[slot], position);
                }
                m_gen[slot] = (m_gen[slot] + 1) & 0x7FFFFFFFu;
                {
                    // What this create carried, for the trace.
                    std::string detail = std::string("init_vars=") + std::to_string(init.initVariableCount) + "(" +
                        (!haveVars ? "none" : InitSourceName(varsSource)) + ")";
                    for (std::size_t n = 0; n < init.initVariableCount && n < kInitVariables; ++n)
                    {
                        char text[40]{};
                        std::snprintf(text, sizeof(text), "%s0x%08x=%.3f", n ? ";" : ":", init.initVariableHash[n],
                            init.initVariableValue[n]);
                        detail += text;
                    }
                    detail += ",distance_m=" + std::to_string(static_cast<int>(distance));
                    detail += ",settings=" + m_lastSettings;
                    detail += std::string(",entity_mode=") + EntityModeLetter(m_mode) + "(" +
                        (!modeApplied ? "not_applied" : queueMode || mode != EntityMode::GameQueue ? EntityModeName(mode)
                            : "game_queue_by_hash_direct") + ")" + queueDetail;
                    // The sound's dynamic bank and whether a wave slot holds it
                    // at this create.
                    {
                        replay::audio::BankProbe bank;
                        std::string bankError;
                        if (replay::audio::ProbeSoundBank(ref, bank, bankError))
                        {
                            const char* state = replay::audio::BankStateName(bank);
                            ++m_counters.bankStates[state];
                            char text[96]{};
                            std::snprintf(text, sizeof(text), ",bank=0x%08x,banks=%u,bank_state=%s", bank.bank, bank.count, state);
                            detail += text;
                        }
                        else
                        {
                            ++m_counters.bankStates[bankError];
                            detail += ",bank_state=" + bankError;
                        }
                    }
                    m_detail[slot] = detail;
                }
                handle = static_cast<std::uint64_t>(slot) | (static_cast<std::uint64_t>(m_gen[slot]) << 32);
                status = fallback ? (play ? "playing_fallback" : "created_fallback") : error;
                m_counters.lastStatus = status;
                return true;
            }

            // The slot of a handle whose generation is current, else -1.
            std::ptrdiff_t Current(std::uint64_t handle) const
            {
                const std::size_t slot = SlotOf(handle);
                if (handle == player::kNoHandle || slot >= m_slots || (handle >> 32) != m_gen[slot])
                    return -1;
                return static_cast<std::ptrdiff_t>(slot);
            }

            bool Prepare(std::uint64_t handle) override
            {
                std::uint64_t* slots = Slots();
                const std::ptrdiff_t slot = Current(handle);
                if (slot < 0 || !slots[slot])
                    return false;
                std::string error;
                const bool ok = replay::audio::PrepareSound(slots[slot], error);
                ++(ok ? m_counters.prepares : m_counters.prepareFailed);
                return ok;
            }

            bool Play(std::uint64_t handle) override
            {
                std::uint64_t* slots = Slots();
                const std::ptrdiff_t slot = Current(handle);
                if (slot < 0 || !slots[slot])
                    return false;
                std::string error;
                const bool ok = replay::audio::PlaySound(slots[slot], error);
                ++(ok ? m_counters.plays : m_counters.playFailed);
                return ok;
            }

            void Stop(std::uint64_t handle, EndReason reason) override
            {
                std::uint64_t* slots = Slots();
                const std::ptrdiff_t slot = Current(handle);
                if (slot < 0)
                    return; // the slot went to another packet
                if (m_queued[slot].active)
                {
                    // Mode c, S13 on the queue's sound; the
                    // queue's slot keeps it until the engine ends it.
                    if (reason == EndReason::Stop)
                    {
                        std::string error;
                        const std::uint64_t sound = SoundOf(static_cast<std::size_t>(slot));
                        if (sound && replay::audio::StopSound(sound, error))
                            ++m_counters.stopsS13;
                        else if (!sound && m_queued[slot].sound)
                            ++m_stale.stopSkipped; // gone, no call
                        m_meta[slot].stoppedUs = replay::clock::NowUs();
                        return;
                    }
                    StopQueued(static_cast<std::size_t>(slot));
                    ++m_counters.stopsS14;
                    m_meta[slot].used = false;
                    return;
                }
                if (reason == EndReason::Stop)
                {
                    // S13: the sound stops and keeps its reference; the
                    // engine clears the slot when it is gone (Alive false).
                    std::string error;
                    if (slots[slot] && replay::audio::StopSound(slots[slot], error))
                    {
                        ++m_counters.stopsS13;
                        m_meta[slot].stoppedUs = replay::clock::NowUs();
                        return;
                    }
                }
                replay::audio::StopPersistentSound(&slots[slot]); // S14, the slot is cleared
                ++m_counters.stopsS14;
                m_meta[slot].used = false;
            }

            bool Alive(std::uint64_t handle) override
            {
                const std::ptrdiff_t slot = Current(handle);
                return slot >= 0 && Held(static_cast<std::size_t>(slot)); // mode c's queued request too
            }

            void Adjust(std::uint64_t handle, const AdjustPacket& packet) override
            {
                m_lastAdjust = "slot_gone";
                const std::ptrdiff_t slotIndex = Current(handle);
                if (slotIndex < 0)
                    return;
                const std::size_t handleSlot = static_cast<std::size_t>(slotIndex);
                // Mode b keeps the latest values for the write
                // back; mode c's sound is the one the queue made (none yet:
                // the adjust is lost, "slot_gone").
                if (m_meta[handleSlot].modeApplied && m_meta[handleSlot].mode == EntityMode::Override)
                    m_override[handleSlot].Note(packet);
                const std::uint64_t sound = SoundOf(handleSlot);
                if (!sound)
                {
                    if (m_queued[handleSlot].active && !m_queued[handleSlot].sound)
                        m_lastAdjust = "queue_not_born";
                    else if (m_queued[handleSlot].active)
                    {
                        ++m_stale.adjustSkipped; // gone, no call
                        m_lastAdjust = "sound_gone";
                    }
                    return;
                }
                ++m_meta[handleSlot].adjusts;
                const std::uint8_t kind = static_cast<std::uint8_t>(packet.kind);
                bool ok = false;
                m_lastAdjust = "failed";
                switch (packet.kind)
                {
                case Adjust::Position:
                case Adjust::PositionNoDoppler:
                    if (m_meta[handleSlot].follow)
                    {
                        ++m_counters.adjustIgnoredFollow; // the puppet places it
                        m_lastAdjust = "follows_puppet";
                        return;
                    }
                    // The player's own sound requests (the mount and dismount
                    // sets 0x3c826e, 0x3c81a2, the horse's 0x2e96a6) get a
                    // position of (0, 0, 0) in the frame they are created and
                    // the real one from the owner's update a frame later. The
                    // recorder keeps the first value and samples the next 50 ms
                    // later: replayed as is, the sound would start at the map
                    // origin, kilometres from the listener, and its attack (the
                    // whole of a 0.5 s mount sound) would be lost. A position
                    // that is not a world position (clip_area.h) is not
                    // applied; the sound keeps the one it was created with
                    // until the next sample.
                    if (g_originSkip && !replay::cliparea::PositionValid(packet.value[0], packet.value[1], packet.value[2]))
                    {
                        ++m_counters.adjustOriginSkipped;
                        m_lastAdjust = "origin_skipped";
                        return;
                    }
                    {
                        const float position[4] = {packet.value[0], packet.value[1], packet.value[2], 0.0f};
                        ok = replay::audio::SetSoundPosition(sound, position); // S16 for both (S17 is not called)
                    }
                    break;
                case Adjust::Volume: ok = replay::audio::SetSoundVolume(sound, packet.value[0]); break;
                case Adjust::Pitch:
                    ok = replay::audio::SetSoundPitch(sound, static_cast<std::int16_t>(std::lround(packet.value[0])));
                    break;
                case Adjust::LowPass:
                    ok = replay::audio::SetSoundLowPass(sound, static_cast<std::uint32_t>(packet.value[0] < 0.0f ? 0.0f : packet.value[0]));
                    break;
                case Adjust::HighPass:
                    ok = replay::audio::SetSoundHighPass(sound, static_cast<std::uint32_t>(packet.value[0] < 0.0f ? 0.0f : packet.value[0]));
                    break;
                case Adjust::PostSubmix: ok = replay::audio::SetSoundPostSubmix(sound, packet.value[0]); break;
                case Adjust::VolumeCurveScale: ok = replay::audio::SetSoundVolumeCurveScale(sound, packet.value[0]); break;
                case Adjust::Variable:
                {
                    // S38 with the reason of a refusal.
                    const char* why = replay::audio::WriteSoundVariableWhy(sound, packet.hash, packet.value[0]);
                    ok = std::strcmp(why, "ok") == 0;
                    if (!ok)
                        ++m_counters.variableWhy[why];
                    m_lastAdjust = why;
                    break;
                }
                }
                if (ok)
                    m_lastAdjust = "ok";
                if (kind < kAdjustKinds)
                    ++(ok ? m_counters.adjustApplied[kind] : m_counters.adjustFailed[kind]);
            }

            const char* AdjustResult() const override { return m_lastAdjust; }

            std::string CreateDetail(std::uint64_t handle) const override
            {
                const std::ptrdiff_t slot = Current(handle);
                return slot < 0 ? std::string() : m_detail[slot];
            }

            // How a sound ended or is ending:
            // by=engine (its slot let go of it with no stop of ours), after_s13
            // (gone after our S13), ours (our end now), queue_never_born (mode
            // c's request made no sound in time); its life, adjusts, write backs.
            std::string EndDetail(std::uint64_t handle) const override
            {
                const std::ptrdiff_t slotIndex = Current(handle);
                if (slotIndex < 0)
                    return "by=slot_reused";
                const std::size_t slot = static_cast<std::size_t>(slotIndex);
                const SlotMeta& meta = m_meta[slot];
                const QueuedSound& q = m_queued[slot];
                const std::uint64_t now = replay::clock::NowUs();
                const bool held = Held(slot);
                const char* by = held ? "ours"
                    : q.active && !q.sound ? "queue_never_born"
                    : meta.stoppedUs ? "after_s13" : "engine";
                char text[256]{};
                std::snprintf(text, sizeof(text),
                    "by=%s,entity_mode=%s,lived_ms=%llu,s13_ms_before=%lld,adjusts=%u,reapplies=%u,updates_with_entity=%d,queue=%s",
                    by, meta.modeApplied ? EntityModeLetter(meta.mode) : "not_applied",
                    static_cast<unsigned long long>(meta.createdUs && now > meta.createdUs ? (now - meta.createdUs) / 1000 : 0),
                    meta.stoppedUs ? static_cast<long long>((now - meta.stoppedUs) / 1000) : -1ll, meta.adjusts, meta.reapplies,
                    meta.updatesWithEntity ? 1 : 0,
                    !q.active ? "none" : !q.sound ? "waiting" : "bound"); // tied by ticket
                return text;
            }

            bool MadeByOther(const CreatePacket& packet, std::uint32_t nowMs) override
            {
                const std::uint64_t key = soundowner::PacketSoundKey(packet.stableId, packet.rawRef);
                // A sound of no owner that the live world made near the same
                // place in the last kWorldLiveMs plays there already (an
                // ambient emitter the replay does not stop): the recorded copy
                // would be a double.
                std::uint64_t twin = 0;
                if (packet.owner == census::kNoOwner &&
                    m_own.Recent(census::kNoOwner, key, nowMs, kWorldLiveMs, packet.hasPosition ? packet.params.position : nullptr,
                        kWorldNearM, &twin))
                {
                    ++m_counters.worldLiveYields;
                    // A long copy that yields is watched with its live twin
                    // (CheckYields).
                    if (m_nextLifeMs >= replay::soundslots::kLongSoundMs) // kNeverMs (a loop) too
                    {
                        ++m_counters.worldYieldsLong;
                        bool alive = false;
                        if (twin)
                            for (const census::LiveSound& l : census::Live())
                                if (l.sound == twin)
                                {
                                    alive = true;
                                    break;
                                }
                        const std::uint32_t end = m_nextLifeMs == soundworld::kNeverMs
                            ? soundworld::kNeverMs
                            : packet.timeMs + m_nextLifeMs;
                        m_yields.Note(packet.index, packet.stableId ? packet.stableId : packet.rawRef, twin, alive, nowMs, end);
                    }
                    return true;
                }
                return m_own.Take(packet.owner, key, packet.timeMs, nowMs, kBeforeMs);
            }

            // The complement's second check, any entry of the packet's set.
            bool MadeSameSetByOther(const CreatePacket& packet, std::uint32_t nowMs) override
            {
                if (packet.owner == census::kNoOwner)
                    return false;
                const std::uint64_t key = soundowner::PacketSoundKey(packet.stableId, packet.rawRef);
                return m_own.TakeSet(packet.owner, key, packet.timeMs, nowMs, kBeforeMs);
            }

        private:
            Vector3 Camera(std::uint32_t nowMs)
            {
                if (m_cameraMs != nowMs)
                {
                    m_camera = CAM::GET_FINAL_RENDERED_CAM_COORD();
                    m_cameraMs = nowMs;
                }
                return m_camera;
            }

            // The ped puppet a weapon puppet hangs on at timeMs
            // (its WeaponPoint attachment, as the weapon sound track finds it).
            const replay::playback::Puppet* WeaponHolder(const replay::playback::Puppet& weapon, std::uint32_t timeMs)
            {
                static std::vector<replay::entity::Attachment> attachments;
                m_context->clip.lifecycle.AttachmentsAt(timeMs, attachments);
                for (const replay::entity::Attachment& attachment : attachments)
                {
                    if (attachment.child != weapon.packet.id ||
                        attachment.kind != replay::entity::AttachmentKind::WeaponPoint)
                        continue;
                    const replay::playback::Puppet* parent = ReadyPuppet(*m_context, attachment.parent.Packed());
                    return parent && replay::entities::IsPed(parent->clone) ? parent : nullptr;
                }
                return nullptr;
            }

            // The packet's audio entity on the puppet (the capture's PuppetAudio).
            std::uint64_t AudioOf(const CreatePacket& packet, const replay::playback::Puppet* puppet, std::uint64_t address,
                std::string& status)
            {
                return AudioOfPed(packet, puppet ? replay::entities::AddressOf(puppet->clone) : nullptr, address, status);
            }

            // The audio entity of the packet's base on a ped (the puppet, or
            // for SoundGenericPlayerRoute the hidden live player).
            std::uint64_t AudioOfPed(const CreatePacket& packet, void* ped, std::uint64_t address, std::string& status)
            {
                std::uint64_t base = 0;
                switch (packet.audioBase)
                {
                case AudioBase::Image: base = census::ImageBase(); break;
                case AudioBase::Entity: base = address; break;
                case AudioBase::PedHolder:
                    base = ped ? replay::audio::PedAudioHolder(ped) : 0;
                    break;
                case AudioBase::WeaponAudio:
                    base = ped ? replay::audio::WeaponAudio(ped) : 0;
                    break;
                case AudioBase::VehicleAudio:
                    if (!address || !census::ReadQword(address + census::kVehicleAudioField, base))
                        base = 0;
                    break;
                case AudioBase::Field:
                    if (!address || !census::ReadQword(address + packet.audioField, base))
                        base = 0;
                    break;
                }
                if (!base)
                {
                    status = "no_base";
                    return 0;
                }
                const std::uint64_t audio = base + static_cast<std::int64_t>(packet.audioOffset);
                std::uint64_t vtable = 0;
                if (!census::ReadQword(audio, vtable) || vtable != census::ImageBase() + packet.vtableRva)
                {
                    ++m_counters.vtableDiffers;
                    status = "vtable_differs";
                    return 0;
                }
                return audio;
            }

            Context* m_context = nullptr;
            std::size_t m_slots = 64;
            SlotMeta m_meta[kMaxSlots];
            std::uint32_t m_gen[kMaxSlots] = {}; // the slot's generation
            census::OwnCreates m_own;
            std::unordered_set<std::uint64_t> m_ours;
            EngineCounters m_counters;
            Vector3 m_camera{};
            std::uint32_t m_cameraMs = 0xFFFFFFFFu;
            const char* m_lastAdjust = "";
            std::string m_lastSettings;
            bool m_envGroups = true;    // SoundGenericEnvGroup
            bool m_queueEnv = true;       // SoundQueueEnvGroup
            bool m_weaponHolderEnv = true; // ShotEnvGroupReplay
            std::uint32_t m_weaponEnvLines = 0;
            player::TraceWindow m_mute; // SoundGenericMute
            bool m_playerRoute = false; // SoundGenericPlayerRoute
            bool m_coreBByRef = true;   // SoundGenericCoreBByRef
            int m_directParams = 0; // SoundGenericDirectParams (0, 1 whole, 2 category)
            // SoundGenericEntityMode, the family of the next create, mode c's
            // requests and mode b's values per slot.
            EntityMode m_mode = EntityMode::Detached;
            soundowner::Family m_family = soundowner::Family::Owned;
            QueuedSound m_queued[kMaxSlots];
            // Mode c's births drained by ticket; the live
            // sounds the safety net stops.
            std::uint64_t m_birthCursor = 0;
            bool m_nextLong = false; // NoteNextLife
            // SoundGenericWorldCap / SoundGenericWorldCapCount, the next
            // packet's recorded life (NoteNextLife), the evidence lines of the
            // tick and their budgets per playback, the yield watch.
            static constexpr std::uint32_t kStealLines = 200;
            static constexpr std::uint32_t kYieldLostLines = 100;
            bool m_worldPerSound = true;
            std::uint32_t m_worldCapCount = replay::soundslots::kPerOwnerCap;
            std::uint32_t m_nextLifeMs = soundworld::kNeverMs;
            std::vector<std::string> m_evidence;
            soundworld::Budget m_stealBudget{kStealLines};
            soundworld::Budget m_yieldBudget{kYieldLostLines};
            soundworld::YieldWatch m_yields;
            std::uint32_t m_yieldCheckMs = 0xFFFFFFFFu;
            std::vector<std::uint64_t> m_liveScratch; // CheckYields: the census's live sounds, sorted
            // The sounds of the last census rows taken as
            // another source's (DrainBirths checks a birth against them).
            static constexpr std::size_t kRecentOthers = 256;
            // Census rows waiting before they count as
            // another source's (FeedCensus).
            struct WaitingRow
            {
                std::uint64_t sound = 0;
                std::uint64_t key = 0;
                std::uint32_t owner = 0;
                std::uint32_t at = 0;
                std::uint64_t tick = 0;
                player::DoubleDetector::Source source;
                std::uint32_t frames3[3] = {}; // creator frames 2..4
                bool hasPosition = false;
                float position[3] = {};
                census::OwnerClass cls = census::OwnerClass::World; // the late doubles per vehicle
            };
            static std::string OtherPathKey(const std::uint32_t (&f)[3])
            {
                char text[40]{};
                std::snprintf(text, sizeof(text), "0x%x/0x%x/0x%x", f[0], f[1], f[2]);
                return text;
            }
            std::map<std::string, std::uint32_t> m_otherPaths;
            // 0: fed the tick they come (the late-birth theory did not hold: the
            // doubles were the puppets' persistent creates, fixed at the port).
            static constexpr std::uint64_t kOtherWaitTicks = 0;
            static constexpr std::size_t kMaxWaiting = 4096;
            std::vector<WaitingRow> m_waiting;
            std::uint64_t m_feedTick = 0;
            std::uint64_t m_recentOthers[kRecentOthers] = {};
            std::uint64_t m_recentOtherTick[kRecentOthers] = {};
            std::size_t m_recentOthersNext = 0;
            std::vector<replay::audio::QueueBirth> m_births;
            // The safety net holds sounds with their identity; the stale
            // counters.
            player::LinkedSet m_net;
            EngineLinks m_links;
            player::StaleCounters m_stale;
            std::uint32_t m_abandonedSlots = 0;
            player::OverrideState m_override[kMaxSlots];
            std::string m_detail[kMaxSlots];
            std::unordered_set<std::uint32_t> m_tooFar;
            vehiclesound::ClassSlotTally m_classSlots;
            vehiclesound::VehicleTally m_vehicles;
        };

        player::Player g_player;
        EngineImpl g_engine;
        soundowner::Overrides g_overrides;
        std::string g_ports;
        bool g_playing = false;
        // Packets mode.
        // g_packets: the packets this player takes (owned, taker generic),
        // for the tracks that replay a sound by its ref; g_owned: every
        // owned packet (any taker), to classify the blocked creates;
        // g_esndRows: per ESND row, 1 when a packet carries it.
        bool g_packetsMode = false;
        soundowner::PacketIndex g_packets;
        soundowner::PacketIndex g_owned;
        std::vector<std::uint8_t> g_esndRows;
        player::BlockedTally g_blocked;
        // The recorded foot events (the step rule), the
        // packets' lives the ports judge against (published to the observer;
        // a few older ones kept alive for a detour that read one just before
        // the swap), SoundGenericBlock=all, and the findings of the last stop.
        soundowner::FootWindows g_feet;
        soundowner::ShotWindows g_shots; // the recorded shots by weapon
        std::unique_ptr<soundowner::PacketLives> g_lives[4];
        std::size_t g_livesNext = 0;
        bool g_blockAll = false;
        bool g_stepSetGate = true;
        std::vector<census::Finding> g_lastFindings;
        std::uint64_t g_blockedCursor = 0;
        std::vector<replay::audio::BlockedPuppetCreate> g_blockedScratch;

        bool PacketsActive()
        {
            return g_playing && g_packetsMode && g_player.Active() && g_player.Configured().enabled;
        }

        // vehicle_sound_rule.h: the puppet vehicles' and train cars' audio
        // entities ([vehicle + 0x900]) the census creators judge an own create
        // against, four times a second while packets mode plays (as the ped
        // holders', replay_events.cpp), and each clip entity's class for the
        // per vehicle rows.
        ULONGLONG g_vehiclesAt = 0;
        bool g_vehicleBlock = false;
        std::size_t g_vehicleAudiosPeak = 0;
        std::map<std::uint32_t, census::OwnerClass> g_vehicleClasses;

        void ClearVehicleAudios()
        {
            replay::audio::SetSilencedVehicleAudios({}, {});
            g_vehiclesAt = 0;
        }

        void PublishVehicleAudios(Context& context)
        {
            const ULONGLONG now = GetTickCount64();
            if (g_vehiclesAt && now - g_vehiclesAt < 250)
                return;
            g_vehiclesAt = now;
            static std::vector<std::uint64_t> audios;
            static std::vector<std::uint32_t> owners;
            audios.clear();
            owners.clear();
            for (const replay::playback::Puppet& puppet : context.puppets.list)
            {
                const bool train = puppet.packet.kind == replay::entity::EntityKind::TrainCar;
                if ((puppet.packet.kind != replay::entity::EntityKind::Vehicle && !train) ||
                    puppet.state != replay::playback::PuppetState::Ready || !puppet.clone || !replay::entities::Exists(puppet.clone))
                    continue;
                const std::uint64_t address = reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone));
                std::uint64_t audio = 0;
                if (!address || !census::ReadQword(address + census::kVehicleAudioField, audio) || audio < 0x10000 || (audio & 7) != 0)
                    continue;
                const std::uint32_t id = puppet.packet.id.Packed();
                audios.push_back(audio);
                owners.push_back(id);
                g_vehicleClasses[id] = train ? census::OwnerClass::Train : census::OwnerClass::Vehicle;
            }
            replay::audio::SetSilencedVehicleAudios(audios, owners);
            g_vehicleAudiosPeak = (std::max)(g_vehicleAudiosPeak, audios.size());
        }

        census::OwnerClass VehicleClassOf(std::uint32_t owner)
        {
            const auto it = g_vehicleClasses.find(owner);
            return it != g_vehicleClasses.end() ? it->second : census::OwnerClass::Vehicle;
        }

        // A blocked ring entry of the creators (ports 1 and 2): a vehicle's own create.
        void NoteVehicleOwn(const replay::audio::BlockedPuppetCreate& blocked)
        {
            if (blocked.port == 1 || blocked.port == 2)
                g_engine.Vehicles().NoteOwn(blocked.owner, VehicleClassOf(blocked.owner), blocked.refused);
        }
        std::uint64_t g_lastPlayLogUs = 0;
        ClockSampler g_playClock; // SOUND_CLOCK while playing

        std::string MapText(const std::map<std::string, std::uint64_t>& map)
        {
            if (map.empty())
                return "none";
            std::string out;
            for (auto it = map.begin(); it != map.end(); ++it)
                out += std::string(it == map.begin() ? "" : ";") + it->first + ":" + std::to_string(it->second);
            return out;
        }

        // The sound check's section: what the engine kept of each replayed
        // create, read back from its requested settings. Not audibility: no
        // field of the live sound that says "heard" is proven (section 17.5).
        std::string SettingsSection(const EngineCounters& c)
        {
            std::uint64_t differs = 0, total = 0;
            for (const auto& entry : c.settingsCheck)
            {
                total += entry.second;
                if (entry.first != "as_given" && entry.first != "unread" && entry.first != "queued") // c is not read back
                    differs += entry.second;
            }
            std::string out = "\n== generic creates read back (requested settings) ==\n";
            out += "Creates read back: " + std::to_string(total) + ", not as the game gave them: " + std::to_string(differs) + "\n";
            out += "Read back: " + MapText(c.settingsCheck) + "\n";
            out += "Value fields from: the packet " + std::to_string(c.valuesPacket) + ", another track's row " +
                std::to_string(c.valuesTrack) + ", none (constructor's) " + std::to_string(c.valuesNone) + "\n";
            out += "Environment group asked: " + MapText(c.envStatus) + "\n";
            out += "Sounds the owner's audio entity updates every frame: with the game's client variable " +
                std::to_string(c.updateWithClientVar) + ", without one " + std::to_string(c.updateNoClientVar) + "\n";
            return out;
        }

        std::string EngineLine(const EngineCounters& c)
        {
            std::string out = "SOUND_GENERIC_PLAYER_ENGINE,creates=" + std::to_string(c.creates) + ",created=" + std::to_string(c.created) +
                ",no_puppet=" + std::to_string(c.noPuppet) + ",no_position=" + std::to_string(c.noPosition) + ",no_audio=" +
                std::to_string(c.noAudio) + ",vtable_differs=" + std::to_string(c.vtableDiffers) + ",fallback_static=" +
                std::to_string(c.fallbackStatic) + ",no_slot=" + std::to_string(c.noSlot) + ",too_far=" + std::to_string(c.tooFar) +
                ",stolen=" + std::to_string(c.stolen) + ",refused_by_engine=" + std::to_string(c.refusedByEngine) + ",prepares=" +
                std::to_string(c.prepares) + ",prepare_failed=" + std::to_string(c.prepareFailed) + ",plays=" + std::to_string(c.plays) +
                ",play_failed=" + std::to_string(c.playFailed) + ",stops_s13=" + std::to_string(c.stopsS13) + ",stops_s14=" +
                std::to_string(c.stopsS14) + ",adjust_ignored_follow=" + std::to_string(c.adjustIgnoredFollow) +
                ",adjust_origin_skipped=" + std::to_string(c.adjustOriginSkipped) + ",census_rows=" +
                std::to_string(c.censusRows) + ",census_ours=" + std::to_string(c.censusOurs) + ",adjusts=";
            for (std::uint8_t kind = 0; kind < kAdjustKinds; ++kind)
                out += std::string(kind ? ";" : "") + AdjustPort(static_cast<Adjust>(kind)) + ":" +
                    std::to_string(c.adjustApplied[kind]) + "/" + std::to_string(c.adjustFailed[kind]);
            out += ",s38_why=";
            if (c.variableWhy.empty())
                out += "none";
            for (auto it = c.variableWhy.begin(); it != c.variableWhy.end(); ++it)
                out += std::string(it == c.variableWhy.begin() ? "" : ";") + it->first + ":" + std::to_string(it->second);
            out += ",init_vars=packet:" + std::to_string(c.initVarsPacket) + ";track:" + std::to_string(c.initVarsTrack) +
                ";none:" + std::to_string(c.initVarsNone) + ",too_far_packets=" + std::to_string(c.tooFarPackets) +
                ",too_far_then_created=" + std::to_string(c.tooFarThenCreated) + ",too_far_max_m=" +
                std::to_string(static_cast<int>(c.tooFarMaxM)) + ",too_far_limit_m=" +
                std::to_string(static_cast<int>(replay::soundslots::kMaxDistanceM));
            out += ",values=packet:" + std::to_string(c.valuesPacket) + ";track:" + std::to_string(c.valuesTrack) + ";none:" +
                std::to_string(c.valuesNone) + ",env=" + MapText(c.envStatus) + ",weapon_env=holder:" + std::to_string(c.weaponEnvHolder) +
                ";no_holder:" + std::to_string(c.weaponEnvNoHolder) + ",settings_check=" + MapText(c.settingsCheck) + ",bank_states=" + MapText(c.bankStates) +
                ",update_entity=client_var:" + std::to_string(c.updateWithClientVar) + ";no_client_var:" +
                std::to_string(c.updateNoClientVar);
            out += ",entity_mode=" + std::string(EntityModeLetter(static_cast<EntityMode>(Configured().entityMode))) +
                ",mode_creates=" + std::to_string(c.modeCreates) + ",detached=" + std::to_string(c.detached) +
                ",override=ticks:" + std::to_string(c.overrideTicks) + ";writes:" + std::to_string(c.overrideWrites) +
                ";failed:" + std::to_string(c.overrideFailed) + ",queue=requested:" + std::to_string(c.queueRequested) +
                ";recorded_params:" + std::to_string(c.queueRecorded) + ";refused:" + std::to_string(c.queueRefused) +
                ";born:" + std::to_string(c.queueBorn) + ";born_no_out:" + std::to_string(c.queueBornNoOut) +
                ";never_born:" + std::to_string(c.queueNeverBorn) + ";bound:" + std::to_string(c.queueBound) +
                ";unbound:" + std::to_string(c.queueUnbound) + ";late_ours:" + std::to_string(c.lateOurs) + ";stolen_long:" + std::to_string(c.stolenLong) + ";world_live_yields:" + std::to_string(c.worldLiveYields) + ";ours_held:" + std::to_string(c.censusOursLate) + ";late_ours_near:" + std::to_string(c.lateOursNear) + ";net_stops:" + std::to_string(c.queueNetStops) + ";by_hash_direct:" + std::to_string(c.queueByHash) +
                ",direct_params=full:" + std::to_string(c.directFull) + ";fields:" + std::to_string(c.directFields) + ";category:" + std::to_string(c.directCategory) + ",muted_by_key=" + std::to_string(c.mutedByKey) +
                ",player_route=" + std::to_string(c.playerRouted) + ";failed:" + std::to_string(c.playerRouteFailed);
            // The steals by rule and the world yield watch.
            {
                const soundworld::YieldCounters& y = g_engine.Yields();
                out += ",steals=owner_cap:" + std::to_string(c.ownerCapSteals) + ";world_cap:" + std::to_string(c.worldCapSteals) +
                    ";farther:" + std::to_string(c.fartherSteals) + ";world_victims:" + std::to_string(c.worldVictims) +
                    ";lines:" + std::to_string(c.stealLines) + ";lines_dropped:" + std::to_string(c.stealLinesDropped) +
                    ",world_cap=" + (g_engine.WorldPerSound() ? "sound:" + std::to_string(g_engine.WorldCapCount()) : std::string("shared")) +
                    ",world_yields_long=" + std::to_string(c.worldYieldsLong) + ",yield_watch=tracked:" + std::to_string(y.tracked) +
                    ";untracked:" + std::to_string(y.untracked) + ";lost:" + std::to_string(y.lost) + ";outlived:" +
                    std::to_string(y.outlived) + ";cleared:" + std::to_string(y.cleared) + ";dropped:" + std::to_string(y.dropped);
            }
            // SoundGenericSkip, the calls not made per item.
            out += ",skipped_by_key=";
            const std::uint32_t mask = g_player.Configured().skipMask | g_player.Configured().entityDrivenMask;
            if (!mask)
                out += "none";
            bool first = true;
            for (std::uint8_t key = 0; key < player::kSkipKeys; ++key)
                if (mask & (1u << key))
                {
                    out += std::string(first ? "" : ";") + player::SkipKeyName(key) + ":" +
                        std::to_string(g_player.Stats().skippedByKey[key]);
                    first = false;
                }
            out += ",core_b_by_ref=" + std::to_string(c.coreBByRef) + ";created:" + std::to_string(c.coreBByRefOk) +
                ",keep_across_jump=yes:" + std::to_string(c.keptAcrossJump) + ";no:" + std::to_string(c.keepRefused);
            out += ",last=" + (c.lastStatus.empty() ? std::string("none") : c.lastStatus);
            return out;
        }
    }

    void BeginPlayback(Context& context, std::uint32_t firstMs)
    {
        g_playing = false;
        g_packetsMode = false; // legacy unless this playback plays packets
        replay::audio::SetBlockPuppetSounds(false);
        replay::audio::SetBlockPacketLives(nullptr, false);
        ClearVehicleAudios();
        g_vehicleBlock = false;
        g_vehicleAudiosPeak = 0;
        g_vehicleClasses.clear();
        g_lastFindings.clear();
        if (!Enabled())
            return;
        const Track& track = context.clip.soundGeneric;
        if (track.creates.empty())
        {
            context.log << "SOUND_GENERIC_PLAYER,status=empty_track\n";
            return;
        }
        const Config config = Configured();
        player::Config pc;
        // Off while an export owns the clock, unless SoundGenericExport=1.
        const bool exporting = replay::exportprobe::Active();
        pc.enabled = player::PlayAllowed(config.play, config.playInExport, exporting);
        pc.preloadMs = config.preloadMs;
        pc.preloadDelayMs = config.preloadDelayMs;
        pc.holdMs = config.holdMs;
        pc.skipMask = player::ParseSkip(config.skip);
        pc.entityDrivenMask = player::ParseSkip(config.entityDrivenSkip);
        pc.detachedEntityMode = static_cast<EntityMode>(config.entityMode) == EntityMode::Detached;
        g_overrides = soundowner::ParseOverrides(config.take);
        // What the clip's specific tracks carry (the owner table's cover).
        soundowner::Cover cover;
        for (const replay::EntitySound& sound : context.clip.entitySounds)
            cover.Add(sound.entity.Packed(), sound.soundRef, sound.startMs, soundowner::Family::EntitySound);
        for (const replay::entity::SoundEvent& sound : context.clip.sounds.Events())
            cover.Add(sound.entity.Packed(), sound.soundRef, sound.timeMs, soundowner::Family::SoundRequest);
        for (const replay::CollisionSound& sound : context.clip.collisionSounds)
            cover.Add(sound.entity.Packed(), sound.soundRef, sound.startMs, soundowner::Family::Collision);
        for (const census::GeneralSound& sound : context.clip.generalSounds)
            cover.Add(sound.entity, sound.ref, sound.startMs, soundowner::Family::Captured);
        // The recorded foot events: a step packet outside
        // their windows is not remade by the step track (packets mode).
        g_feet.Clear();
        for (const replay::entity::FootstepEvent& step : context.clip.footsteps.Events())
            g_feet.Add(step.entity.Packed(), step.timeMs, step.arg7);
        g_feet.Seal();
        // The recorded shots by weapon (a weapon audio packet of no named
        // caller away from them is not remade by the shot track;
        // SoundGenericWeaponOffShot=0 keeps the old rule).
        g_shots.Clear();
        for (const replay::entity::ShotEvent& shot : context.clip.shots.Shots())
            g_shots.Add(shot.weapon.Packed(), shot.timeMs);
        g_shots.Seal();
        // The init variables the other tracks' full params keep, for the
        // packets of a clip older than SGEN v3 (they carry none).
        g_initIndex.Clear();
        for (const replay::entity::SoundEvent& sound : context.clip.sounds.Events())
            if (!sound.params.empty())
                g_initIndex.Add(sound.entity.Packed(), sound.soundRef, sound.timeMs, sound.params.data(), sound.params.size(), 1);
        for (const replay::EntitySound& sound : context.clip.entitySounds)
            if (!sound.params.empty() && !sound.byHash)
                g_initIndex.Add(sound.entity.Packed(), sound.soundRef, sound.startMs, sound.params.data(), sound.params.size(), 2);
        for (const census::GeneralSound& sound : context.clip.generalSounds)
            if (!sound.params.empty())
                g_initIndex.Add(sound.entity, sound.ref, sound.startMs, sound.params.data(), sound.params.size(), 3);
        g_initIndex.Seal();
        // Mode c's params whole.
        g_fullParams.clear();
        for (const replay::entity::SoundEvent& sound : context.clip.sounds.Events())
            if (sound.params.size() >= replay::audio::kSoundParamsBytes)
                g_fullParams.push_back({sound.entity.Packed(), sound.soundRef, sound.timeMs, sound.params.data(), 1});
        for (const replay::EntitySound& sound : context.clip.entitySounds)
            if (sound.params.size() >= replay::audio::kSoundParamsBytes && !sound.byHash)
                g_fullParams.push_back({sound.entity.Packed(), sound.soundRef, sound.startMs, sound.params.data(), 2});
        for (const census::GeneralSound& sound : context.clip.generalSounds)
            if (sound.params.size() >= replay::audio::kSoundParamsBytes)
                g_fullParams.push_back({sound.entity, sound.ref, sound.startMs, sound.params.data(), 3});
        const player::TraceWindow trace = player::ParseTraceWindow(config.trace);
        // SoundGenericMode=packets (the default).
        g_packetsMode = config.packets;
        g_overrides.packets = config.packets;
        pc.packets = config.packets;
        g_blockAll = config.blockAll;
        g_stepSetGate = config.stepSetGate;
        const EntityMode entityMode = static_cast<EntityMode>(config.entityMode);
        g_engine.SetDirectParams(config.directParams);
        g_engine.SetMute(player::ParseTraceWindow(config.mute));
        g_engine.SetPlayerRoute(config.playerRoute);
        g_engine.SetCoreBByRef(config.coreBByRef);
        g_engine.SetWorldCap(config.worldCapPerSound, config.worldCapCount);
        pc.keepLoops = config.keepLoops;
        // SoundGenericVehicleBlock, packets mode only.
        g_vehicleBlock = config.packets && config.vehicleBlock;
        pc.vehicleBlock = g_vehicleBlock;
        replay::audio::SetVehicleOwnBlock(g_vehicleBlock);
        g_engine.Begin(context, config.slots, config.envGroups, entityMode);
        g_engine.SetEnvRules(config.queueEnvGroups, config.weaponHolderEnv);
        g_player.Begin(track, pc, cover, g_overrides, firstMs, &g_feet, trace, config.weaponOffShot ? &g_shots : nullptr);
        g_packets.Clear();
        g_owned.Clear();
        g_esndRows.clear();
        g_blocked.Clear();
        std::uint32_t esndHanded = 0;
        if (g_packetsMode)
        {
            for (const player::SoundPlan& plan : g_player.Plans())
            {
                if (plan.create >= track.creates.size())
                    continue;
                const CreatePacket& c = track.creates[plan.create];
                g_owned.Add(c.owner, c.stableId, c.rawRef, c.timeMs);
                if (plan.taker == soundowner::Taker::Generic)
                    g_packets.Add(c.owner, c.stableId, c.rawRef, c.timeMs);
            }
            g_packets.Seal();
            g_owned.Seal();
            // The lives the ports judge a puppet's own create against.
            std::unique_ptr<soundowner::PacketLives> lives(new soundowner::PacketLives());
            for (const player::SoundPlan& plan : g_player.Plans())
            {
                if (plan.create >= track.creates.size())
                    continue;
                const CreatePacket& c = track.creates[plan.create];
                lives->Add(c.owner, c.stableId, c.rawRef, plan.startMs, plan.released ? plan.endMs : 0xFFFFFFFFu);
            }
            lives->Seal();
            // The slot about to be reused held the lives of four playbacks ago:
            // no detour still reads that one.
            g_lives[g_livesNext % 4] = std::move(lives);
            replay::audio::SetBlockPacketLives(g_lives[g_livesNext % 4].get(), g_blockAll, g_stepSetGate);
            ++g_livesNext;
            g_esndRows.assign(context.clip.entitySounds.size(), std::uint8_t{0});
            for (std::size_t row = 0; row < context.clip.entitySounds.size(); ++row)
            {
                const replay::EntitySound& sound = context.clip.entitySounds[row];
                if (g_packets.Covers(sound.entity.Packed(), sound.soundRef, sound.startMs))
                {
                    g_esndRows[row] = 1;
                    ++esndHanded;
                }
            }
            replay::audio::CollectBlockedPuppetCreatesSince(g_blockedCursor, g_blockedScratch); // drop the older ones
        }
        replay::audio::SetBlockPuppetSounds(g_packetsMode && pc.enabled);
        context.log << "SOUND_GENERIC_MODE,mode=" << (g_packetsMode ? "packets" : "legacy") << ",play=" << (pc.enabled ? 1 : 0)
                    << ",owned_packets=" << g_owned.Size() << ",taken_packets=" << g_packets.Size()
                    << ",esnd_rows=" << context.clip.entitySounds.size() << ",esnd_to_generic=" << esndHanded
                    << ",port_block=" << (replay::audio::BlockPuppetSounds() ? 1 : 0)
                    << ",block=" << (g_blockAll ? "all" : "packet") << ",foot_windows=" << g_feet.Size()
                    << ",vehicle_block=" << (g_vehicleBlock ? 1 : 0)
                    << ",off_step=" << g_player.Stats().offStep << ",init_var_rows=" << g_initIndex.Size()
                    << ",shot_windows=" << g_shots.Size() << ",off_shot=" << g_player.Stats().offShot
                    << ",entity_mode=" << EntityModeLetter(entityMode) << ",full_params_rows=" << g_fullParams.size()
                    << ",trace=" << (trace.on ? config.trace : std::string("off")) << '\n';
        g_ports = replay::audio::GenericSoundPortStatus();
        const player::PlayStats& stats = g_player.Stats();
        context.log << "SOUND_GENERIC_PLAYER,status=" << (pc.enabled ? "ready" : config.play ? "export_off" : "counting_only")
                    << ",export=" << (exporting ? 1 : 0) << ",export_play=" << (config.playInExport ? 1 : 0) << ",packets=" << track.creates.size()
                    << ",adjusts=" << track.adjusts.size() << ",ends=" << track.ends.size() << ",generic=" << stats.generic
                    << ",cover_rows=" << cover.Size() << ",slots=" << config.slots << ",preload_ms=" << config.preloadMs
                    << ",preload_delay_ms=" << config.preloadDelayMs << ",hold_ms=" << config.holdMs << ",table="
                    << soundowner::FormatTable(g_overrides) << ",rejected=" << (g_overrides.rejected.empty() ? "none" : g_overrides.rejected)
                    << ",ports=" << g_ports
                    << ",world_cap=" << (config.worldCapPerSound ? "sound:" + std::to_string(config.worldCapCount) : std::string("shared"))
                    << ",engine_writes=0\n";
        g_playing = true;
        g_lastPlayLogUs = replay::clock::NowUs();
        g_playClock.Reset();
    }

    void Play(Context& context, std::uint32_t timeMs)
    {
        if (!g_playing)
            return;
        REPLAY_PROFILE_ZONE("play.sound_generic");
        // An export starts (and ends) with the playback running: the player
        // follows the export's state every tick (SoundGenericExport=0 turns it
        // off while exporting).
        {
            const Config config = Configured();
            const bool exporting = replay::exportprobe::Active();
            if (g_player.SetEnabled(player::PlayAllowed(config.play, config.playInExport, exporting), g_engine))
            {
                // The ports follow (the puppets' own come back while the player is off).
                replay::audio::SetBlockPuppetSounds(g_packetsMode && g_player.Configured().enabled);
                context.log << "SOUND_GENERIC_PLAYER,phase=" << (exporting ? "export_start" : "export_end") << ",play="
                            << (g_player.Configured().enabled ? 1 : 0) << ",export_play=" << (config.playInExport ? 1 : 0)
                            << ",playhead_ms=" << timeMs << ",engine_writes=0\n";
            }
        }
        // What the ports refused since the last tick.
        if (g_packetsMode)
        {
            replay::audio::SetBlockPlayheadMs(timeMs); // the time the ports judge at
            replay::audio::CollectBlockedPuppetCreatesSince(g_blockedCursor, g_blockedScratch);
            for (const replay::audio::BlockedPuppetCreate& blocked : g_blockedScratch)
            {
                g_blocked.Note(blocked.owner, blocked.ref, blocked.caller, blocked.port, timeMs,
                    static_cast<soundowner::LifeVerdict>(blocked.verdict), blocked.refused);
                NoteVehicleOwn(blocked);
            }
            if (g_vehicleBlock && g_player.Configured().enabled)
                PublishVehicleAudios(context);
            else if (g_vehiclesAt)
                ClearVehicleAudios();
        }
        const bool normal = context.hooks && context.hooks->NormalPlay();
        // The playhead against QPC, for the bench's audio capture.
        if (Configured().clockLog)
        {
            ClockSample samples[2];
            const std::size_t n = g_playClock.Tick(timeMs, replay::clock::NowUs(), normal, samples);
            for (std::size_t i = 0; i < n; ++i)
                context.log << FormatClockLine("play", samples[i]) << '\n';
        }
        g_engine.FeedCensus(timeMs, g_player.Doubles());
        g_player.Tick(timeMs, normal, g_engine);
        g_engine.Follow(timeMs);
        // The steals of this tick and the long world copies whose live twin
        // ended early (SOUND_GENERIC_STEAL, SOUND_GENERIC_YIELD_LOST).
        g_engine.CheckYields(timeMs);
        for (const std::string& line : g_engine.TakeEvidence())
            context.log << line << '\n';
        for (const std::string& line : g_player.TakeTrace()) // SoundGenericTrace
            context.log << line << '\n';
        // A start of ours that another source had just
        // made, with that source's creator (the first 80 of a playback).
        for (const player::Player::DoubleEvidence& e : g_player.TakeDoubleEvidence())
        {
            char line[320]{};
            std::snprintf(line, sizeof(line),
                "SOUND_GENERIC_DOUBLE,playhead_ms=%u,owner=%u:%u,ref=0x%llx,family=%s,other_ms=%u,other_mine=%d,other_callers=0x%x/0x%x/0x%x,"
                "other_audio=0x%llx,other_base=%u,other_field=0x%x",
                e.timeMs, e.owner >> 16, e.owner & 0xFFFFu, static_cast<unsigned long long>(e.ref), soundowner::FamilyName(e.family),
                e.other.timeMs, e.other.mine ? 1 : 0, e.other.frames[0], e.other.frames[1], e.other.frames[2],
                static_cast<unsigned long long>(e.other.audio), static_cast<unsigned>(e.other.base), static_cast<unsigned>(e.other.field));
            context.log << line << '\n';
        }
        const std::uint64_t now = replay::clock::NowUs();
        if (now >= g_lastPlayLogUs + static_cast<std::uint64_t>(kLogEveryS * 1000000.0))
        {
            g_lastPlayLogUs = now;
            context.log << player::FormatPlayLogLine(g_player.Stats(), "play", timeMs, g_player.Live(), g_player.Pending()) << '\n'
                        << EngineLine(g_engine.Counters()) << '\n';
        }
    }

    bool PacketsMode() { return PacketsActive(); }
    bool IsPlayerSound(std::uint64_t sound) { return g_engine.Ours(sound); }

    // The findings of the last EndPlayback (census::EndPlayback adds them to the notice).
    std::vector<census::Finding> LastFindings() { return g_lastFindings; }
    std::uint32_t MeleeComplementsPlayed() { return g_player.Stats().complementMeleePlayed; }

    bool EntitySoundHandedOff(std::size_t row)
    {
        return PacketsActive() && row < g_esndRows.size() && g_esndRows[row] != 0;
    }

    bool PacketCovers(std::uint32_t owner, std::uint64_t ref, std::uint32_t timeMs)
    {
        return PacketsActive() && g_packets.Covers(owner, ref, timeMs);
    }

    bool WindowReport(std::uint32_t fromMs, std::uint32_t toMs, player::WindowReport& out)
    {
        if (!g_playing || !g_player.Active())
            return false;
        out = g_player.Window(fromMs, toMs, g_overrides);
        return true;
    }

    std::uint32_t ForgetPlaybackAfterRestart(Context& context)
    {
        const bool was = g_playing;
        const std::uint32_t plans = static_cast<std::uint32_t>(g_player.Live() + g_player.Pending());
        g_playing = false;
        replay::audio::SetBlockPuppetSounds(false);
        replay::audio::SetBlockPacketLives(nullptr, false);
        ClearVehicleAudios();
        g_player.Abandon();
        const std::uint32_t slots = g_engine.ForgetAfterRestart();
        if (was || plans || slots)
        {
            context.log << "SOUND_GENERIC_FORGET,why=session_reset,playing=" << (was ? 1 : 0) << ",plans=" << plans
                        << ",slots=" << slots << ",engine_calls=0,abandoned_slots=" << g_engine.AbandonedSlots() << '\n';
            context.log << player::StaleLine("session_reset", g_engine.Stale()) << '\n';
        }
        return plans + slots;
    }

    std::string EndPlayback(Context& context)
    {
        if (!g_playing)
            return std::string();
        g_playing = false;
        replay::audio::SetBlockPuppetSounds(false);
        if (g_packetsMode)
        {
            replay::audio::CollectBlockedPuppetCreatesSince(g_blockedCursor, g_blockedScratch);
            const std::uint32_t atMs = static_cast<std::uint32_t>(context.transport.PlayheadMs());
            for (const replay::audio::BlockedPuppetCreate& blocked : g_blockedScratch)
            {
                g_blocked.Note(blocked.owner, blocked.ref, blocked.caller, blocked.port, atMs,
                    static_cast<soundowner::LifeVerdict>(blocked.verdict), blocked.refused);
                NoteVehicleOwn(blocked);
            }
        }
        ClearVehicleAudios();
        replay::audio::SetBlockPacketLives(nullptr, false);
        g_player.End(g_engine);
        g_engine.StopAllSlots();
        for (const std::string& line : g_player.TakeTrace())
            context.log << line << '\n';
        if (g_player.TraceDropped())
            context.log << "SOUND_GENERIC_TRACE_DROPPED,lines=" << g_player.TraceDropped() << '\n';
        // The sounds of the puppets that fell short, per owner
        // and sound (log, report, and the notice at Stop through the census).
        g_lastFindings = player::TallyFindings(g_player.Tally());
        for (const auto& entry : g_player.Tally())
        {
            const player::SoundTallyEntry& e = entry.second;
            if (!player::TallyShort(e))
                continue;
            char line[320]{};
            std::snprintf(line, sizeof(line),
                "SOUND_GENERIC_SHORT,owner=%u:%u,ref=0x%llx,family=%s,due=%u,played=%u,yielded=%u,refused=%u,missed=%u,ended_at_once=%u,ended_early=%u,off_step=%u,first_ms=%u",
                entry.first.owner >> 16, entry.first.owner & 0xFFFFu, static_cast<unsigned long long>(e.ref),
                soundowner::FamilyName(e.family), e.taken, e.played, e.yielded, e.refused, e.missed, e.endedAtOnce, e.endedEarly,
                e.offStep, e.firstShortMs);
            context.log << line << '\n';
        }
        const player::PlayStats& stats = g_player.Stats();
        // The step and melee packets the track's engine
        // call did not make again, played from the recording.
        for (const player::ComplementEvidence& e : g_player.Complements())
            context.log << player::FormatComplementLine(e) << '\n';
        context.log << player::FormatPlayLogLine(stats, "stop", static_cast<std::uint32_t>(context.transport.PlayheadMs()), 0, 0) << '\n'
                    << EngineLine(g_engine.Counters()) << '\n';
        {
            // The puppets' own sounds the port let through.
            std::string samples;
            const std::uint64_t unticketed = replay::audio::UnticketedPuppetCreates(samples);
            context.log << "SOUND_GENERIC_UNTICKETED,count=" << unticketed << ",samples=" << (samples.empty() ? "none" : samples.c_str()) << '\n';
            std::vector<std::pair<std::uint32_t, std::string>> paths;
            for (const auto& entry : g_engine.OtherPaths())
                paths.push_back({entry.second, entry.first});
            std::sort(paths.rbegin(), paths.rend());
            std::string text;
            for (std::size_t n = 0; n < paths.size() && n < 12; ++n)
                text += (text.empty() ? "" : ";") + paths[n].second + "=" + std::to_string(paths[n].first);
            context.log << "SOUND_GENERIC_OTHER_PATHS,paths=" << paths.size() << ",top=" << (text.empty() ? "none" : text.c_str()) << '\n';
        }
        {
            // The calls kept off freed sounds, and the
            // ports' own back reference refusals (cumulative in the process).
            const replay::audio::SoundLinkStats link = replay::audio::SoundLinkCounters();
            context.log << player::StaleLine("playback_end", g_engine.Stale()) << ",port_checks=" << link.checks
                        << ",port_not_sound=" << link.notSound << ",port_no_settings=" << link.noSettings
                        << ",port_link_broken=" << link.broken << ",refused=stop:" << link.refused[replay::audio::kLinkStop]
                        << ";prepare:" << link.refused[replay::audio::kLinkPrepare] << ";play:" << link.refused[replay::audio::kLinkPlay]
                        << ";adjust:" << link.refused[replay::audio::kLinkAdjust] << ";variable:" << link.refused[replay::audio::kLinkVariable]
                        << ";read:" << link.refused[replay::audio::kLinkRead] << ",persistent_link_broken=" << link.persistentBroken << '\n';
        }
        if (g_packetsMode)
            context.log << player::BlockedLogLine(g_blocked, g_blockAll) << '\n';
        {
            // vehicle_sound_rule.h: each vehicle and train car (its packets,
            // the doubles, its own creates at the creators, the slots and the
            // distance), the total with its verdict (doubled: the defect of
            // 135), and the slot outcomes by class.
            vehiclesound::VehicleTally& vehicles = g_engine.Vehicles();
            g_player.TallyVehicles(vehicles);
            for (const std::string& line : vehiclesound::FormatVehicleLines(vehicles, 24))
                context.log << line << '\n';
            context.log << vehiclesound::FormatVehicleTotal(vehicles, g_vehicleBlock) << ",audio_entities_peak="
                        << g_vehicleAudiosPeak << ",own_refused_process=" << replay::audio::VehicleOwnRefused()
                        << ",own_passed_process=" << replay::audio::VehicleOwnPassed() << '\n';
            for (const std::string& line : vehiclesound::FormatClassSlotLines(g_engine.ClassSlots(), g_engine.WorldCapCount()))
                context.log << line << '\n';
        }
        return player::FormatSection(stats, g_overrides, g_ports) + player::FormatTallySection(g_player.Tally()) +
            player::FormatBlockedSection(g_blocked, g_packetsMode, 40, g_blockAll) + SettingsSection(g_engine.Counters());
    }
    void SetOriginSkip(bool on) { g_originSkip = on; }
}
