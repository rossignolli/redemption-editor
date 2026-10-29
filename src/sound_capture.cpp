// Census-level sound capture (sound_capture.h).
#include "sound_capture.h"
#include "sound_generic.h" // SoundGenericMode=packets

#include "audio_observer.h"
#include "entity_cache.h"
#include "log_policy.h"
#include "pose_capture.h"
#include "profiler.h"
#include "sound_census.h"

#include <windows.h>

#include "main.h"
#include "natives.h"

#include <algorithm>
#include <cstring>
#include <ostream>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace replay::capture
{
    namespace
    {
        using census::AudioBase;
        using census::GeneralSound;
        using census::Owner;
        using census::OwnerClass;
        using census::OwnerKind;

        // A captured sound waits this long (recording clock) before it is
        // kept: the entity sound, sound request and collision tracks file the
        // same create within a tick or two, and a sound they took is theirs.
        constexpr std::uint32_t kDecideMs = 300;
        constexpr std::uint32_t kCoverToleranceMs = 400;
        // Playback: a captured sound starts this late, to see whether its
        // puppet makes it itself (the entity sound track waits 120 ms; this
        // one waits a little longer so a ped audio sound the entity track
        // plays counts as the puppet's).
        constexpr std::uint32_t kHoldMs = 150;
        constexpr std::uint32_t kBeforeMs = 300;
        constexpr std::uint32_t kKeepMs = 3000;
        constexpr std::uint32_t kOneShotWindowMs = 500;
        constexpr std::uint32_t kLoopMs = 1000;
        constexpr std::uint32_t kMaxStepMs = 250; // ForwardEventCursor::kMaxStepMs
        constexpr std::size_t kSlots = 32;

        struct Candidate
        {
            census::Row row;
            Owner owner;
            std::uint32_t startMs = 0;
            std::uint32_t dueMs = 0;
            bool alive = false;
        };
        struct LiveCapture
        {
            std::uint64_t sound = 0;
            std::uint64_t slot = 0;
            std::uint32_t entity = 0;
            std::uint64_t ref = 0;
            std::uint32_t startMs = 0;
        };
        struct RecordStats
        {
            std::uint64_t candidates = 0;
            std::uint64_t captured = 0;
            std::uint64_t oneShots = 0;
            std::uint64_t loops = 0;
            std::uint64_t adopted = 0;
            std::uint64_t coveredEntity = 0;
            std::uint64_t coveredRequest = 0;
            std::uint64_t coveredCollision = 0;
            std::uint64_t classSkipped = 0;
            std::uint64_t unresolved = 0;
            std::uint64_t notObject = 0;
            std::uint64_t invalid = 0;
            std::uint64_t ended = 0;
            std::uint64_t perClass[census::kOwnerClassCount] = {};
        };
        struct PlayStats
        {
            std::uint64_t played = 0;
            std::uint64_t byPuppet = 0;
            std::uint64_t refused = 0;
            std::uint64_t noSlot = 0;
            std::uint64_t fallback = 0;
            std::uint64_t overrun = 0; // one-shots stopped after kOneShotOverrunMs
            std::uint64_t vtableDiffers = 0;
            std::string last;
        };

        std::vector<Candidate> g_pending;
        std::unordered_set<std::uint64_t> g_known; // sounds this recording has looked at
        std::vector<LiveCapture> g_live;
        std::uint32_t g_liveCheckedMs = 0;
        std::uint32_t g_adoptCheckedMs = 0;
        bool g_adoptFirst = true;
        RecordStats g_record;
        replay::log::LogLimit g_recordLogs;
        replay::log::LogLimit g_playLogs;

        census::OwnCreates g_own;
        PlayStats g_play;
        // Refs of the clip's captured sounds, and the (puppet, ref) pairs the
        // engine is playing on a puppet right now (rebuilt once per tick when
        // a start is tried): a puppet already playing its own loop of the
        // same ref keeps it (made before the playback's first tick).
        std::unordered_set<std::uint64_t> g_clipRefs;
        std::vector<std::pair<std::uint32_t, std::uint64_t>> g_livePuppet;
        bool g_livePuppetBuilt = false;
        std::uint32_t g_lastMs = 0;
        std::vector<std::uint8_t> g_waiting;
        std::size_t g_slotEvent[kSlots] = {};
        bool g_slotFollows[kSlots] = {};
        std::uint32_t g_slotStartMs[kSlots] = {}; // the playhead when the slot's sound started
        // What playback plays: the clip's sounds with the runs of a sound requested
        // every frame merged into one (census::CoalesceRepeatedOneShots); built at
        // playback start, so any clip gets it.
        std::vector<GeneralSound> g_playSounds;
        census::CoalesceStats g_coalesce;
        // A captured one-shot still holding its slot this long after it started is
        // stopped: a create-and-play one-shot ends by itself in the game, one that does
        // not on the persistent slot would hold the slot to the end of the playback.
        constexpr std::uint32_t kOneShotOverrunMs = 8000;

        // The engine nulls a slot when its sound ends, possibly after the
        // module moved on: the slots live on the process heap (the entity
        // sound track's EngineSoundSlots).
        std::uint64_t* Slots()
        {
            static std::uint64_t* slots = [] {
                static std::uint64_t fallback[kSlots] = {};
                void* memory = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(fallback));
                return memory ? static_cast<std::uint64_t*>(memory) : fallback;
            }();
            return slots;
        }

        bool InImage(std::uint64_t address)
        {
            return address >= census::ImageBase() && address < census::ImageEnd();
        }

        // Another track filed this create (same owner and ref, near the time).
        const char* CoveredBy(Context& context, const Candidate& c)
        {
            const auto close = [&](std::uint32_t timeMs) {
                return (timeMs > c.startMs ? timeMs - c.startMs : c.startMs - timeMs) <= kCoverToleranceMs; };
            const auto& entity = context.clip.entitySounds;
            for (std::size_t n = entity.size(), seen = 0; n-- > 0 && seen < 512; ++seen)
            {
                const replay::EntitySound& sound = entity[n];
                // A train car sound of the entity track (vehicleAudio) plays on
                // [puppet+0x11E0], the consist's engine pointer, which a puppet
                // car does not have: not counted as covered (the anti-double
                // rule keeps the two from doubling when it does play).
                if (sound.vehicleAudio)
                    continue;
                if (sound.entity.Packed() == c.owner.id && close(sound.startMs) &&
                    (sound.soundRef == c.row.ref || sound.byHash))
                    return "entity";
            }
            const auto& requests = context.clip.sounds.Events();
            for (std::size_t n = requests.size(), seen = 0; n-- > 0 && seen < 512; ++seen)
            {
                const replay::entity::SoundEvent& sound = requests[n];
                if (sound.entity.Packed() == c.owner.id && sound.soundRef == c.row.ref && close(sound.timeMs))
                    return "request";
            }
            const auto& collisions = context.clip.collisionSounds;
            for (std::size_t n = collisions.size(), seen = 0; n-- > 0 && seen < 128; ++seen)
            {
                const replay::CollisionSound& sound = collisions[n];
                if (sound.entity.Packed() == c.owner.id && sound.soundRef == c.row.ref && close(sound.startMs))
                    return "collision";
            }
            return nullptr;
        }

        // Where the audio entity is from the owner (the base the puppet has too): the
        // rule is ResolveAudioBase below (shared with the generic sound recorder).
        bool ResolveBase(const census::Row& row, const Owner& owner, GeneralSound& out)
        {
            return ResolveAudioBase(row.audio, owner, out.base, out.field, out.offset);
        }

        void Capture(Context& context, const census::Row& row, const Owner& owner, std::uint32_t startMs,
            std::uint64_t slot, bool preexisting)
        {
            GeneralSound sound;
            sound.entity = owner.id;
            sound.cls = owner.cls;
            sound.ref = row.ref;
            for (std::uint32_t n = 0; n < census::kPathDepth; ++n)
                sound.path[n] = row.callers[n + 1];
            const char* status = "captured";
            std::uint64_t vtable = 0;
            if (!ResolveBase(row, owner, sound))
            {
                ++g_record.unresolved;
                status = "audio_unresolved";
            }
            else if (!census::ReadQword(row.audio, vtable) || !InImage(vtable) || (vtable & 7u) != 0)
            {
                ++g_record.notObject;
                status = "audio_not_an_object";
            }
            else
            {
                sound.vtableRva = static_cast<std::uint32_t>(vtable - census::ImageBase());
                sound.follows = census::Classify(row) != census::Placement::Position;
                sound.preexisting = preexisting;
                if (row.hasParams)
                    sound.params.assign(row.params, row.params + census::kParamsBytes);
                sound.startMs = startMs;
                sound.endMs = startMs;
                std::uint64_t current = 0;
                const bool alive = slot && census::ReadQword(slot, current) && current == row.sound;
                if (alive)
                {
                    g_live.push_back({row.sound, slot, sound.entity, sound.ref, sound.startMs});
                    ++g_record.loops;
                }
                else if (slot)
                {
                    // A persistent sound that already ended: short, never cut.
                    const std::uint32_t nowMs = replay::pose::capture::NowMs();
                    sound.endMs = nowMs > startMs ? nowMs : startMs + 1;
                    sound.ended = true;
                    ++g_record.oneShots;
                }
                else
                {
                    sound.oneShot = true;
                    sound.ended = true;
                    sound.endMs = startMs + kOneShotWindowMs;
                    ++g_record.oneShots;
                }
                if (!census::GeneralSoundValid(sound, census::ImageEnd() - census::ImageBase(), census::kParamsBytes))
                {
                    ++g_record.invalid;
                    status = "invalid";
                    if (alive)
                        g_live.pop_back();
                }
                else
                {
                    context.clip.generalSounds.push_back(sound);
                    ++g_record.captured;
                    ++g_record.perClass[static_cast<int>(sound.cls)];
                    g_record.adopted += preexisting ? 1 : 0;
                }
            }
            if (replay::log::Verbose() && g_recordLogs.Take(40))
            {
                context.log << "SOUND_CAPTURE,phase=record,status=" << status << ",id=" << census::IdText(owner.id)
                            << ",cls=" << census::ClassToken(owner.cls) << std::hex << ",ref=0x" << row.ref
                            << ",audio=" << census::BaseToken(sound.base) << "+0x" << sound.offset << ",field=0x"
                            << sound.field << std::dec << ",path=" << census::PathText(sound.path)
                            << ",one_shot=" << sound.oneShot << ",preexisting=" << preexisting << ",time_ms=" << startMs
                            << '\n';
            }
        }

        void EndLive(Context& context, const LiveCapture& live, std::uint32_t endMs, bool ended)
        {
            auto& sounds = context.clip.generalSounds;
            for (std::size_t n = sounds.size(); n-- > 0;)
            {
                GeneralSound& sound = sounds[n];
                if (sound.ended || sound.entity != live.entity || sound.ref != live.ref || sound.startMs != live.startMs)
                    continue;
                sound.endMs = endMs > sound.startMs ? endMs : sound.startMs + 1;
                sound.ended = ended;
                ++g_record.ended;
                return;
            }
        }

        // ------------------------------------------------------------ playback

        const replay::playback::Puppet* ReadyPuppet(Context& context, std::uint32_t id)
        {
            const replay::playback::Puppet* puppet = context.puppets.Find(replay::entity::ReplayId::FromPacked(id));
            return puppet && puppet->state == replay::playback::PuppetState::Ready && puppet->clone &&
                    replay::entities::Exists(puppet->clone)
                ? puppet
                : nullptr;
        }

        // The puppet's audio entity for a captured sound (0 with a status).
        std::uint64_t PuppetAudio(const GeneralSound& sound, const replay::playback::Puppet& puppet, std::string& status)
        {
            void* entity = replay::entities::AddressOf(puppet.clone);
            const auto address = reinterpret_cast<std::uint64_t>(entity);
            std::uint64_t base = 0;
            switch (sound.base)
            {
            case AudioBase::Entity: base = address; break;
            case AudioBase::PedHolder: base = replay::audio::PedAudioHolder(entity); break;
            case AudioBase::WeaponAudio: base = replay::audio::WeaponAudio(entity); break;
            case AudioBase::VehicleAudio:
                if (!census::ReadQword(address + census::kVehicleAudioField, base))
                    base = 0;
                break;
            case AudioBase::Image: base = census::ImageBase(); break;
            case AudioBase::Field:
                if (!census::ReadQword(address + sound.field, base))
                    base = 0;
                break;
            }
            if (!base || !address)
            {
                status = "no_base";
                return 0;
            }
            const std::uint64_t audio = base + static_cast<std::int64_t>(sound.offset);
            std::uint64_t vtable = 0;
            if (!census::ReadQword(audio, vtable) || vtable != census::ImageBase() + sound.vtableRva)
            {
                status = "vtable_differs";
                return 0;
            }
            return audio;
        }

        void StopAll()
        {
            std::uint64_t* slots = Slots();
            for (std::size_t n = 0; n < kSlots; ++n)
                replay::audio::StopPersistentSound(&slots[n]);
            std::fill(g_waiting.begin(), g_waiting.end(), 0);
        }

        void PlaceOnPuppet(std::uint64_t sound, Entity clone)
        {
            const Vector3 at = ENTITY::GET_ENTITY_COORDS(clone, TRUE, TRUE);
            const float position[4] = {at.x, at.y, at.z, 0.0f};
            replay::audio::SetSoundPosition(sound, position);
        }
    }

    bool ResolveAudioBase(std::uint64_t audio, const census::Owner& owner, census::AudioBase& base, std::uint16_t& field,
        std::int32_t& offset)
    {
        if (InImage(audio))
        {
            base = AudioBase::Image;
            field = 0;
            offset = static_cast<std::int32_t>(audio - census::ImageBase());
            return true;
        }
        Owner range = owner;
        const bool inside = audio >= owner.lo && audio < owner.hi;
        if (!inside)
        {
            // Found by its tracker: the audio entity's own range, if it is
            // the same owner's (a vehicle's [+0x900], a learned field).
            Owner byAudio;
            if (!census::OwnerOf(audio, 0, byAudio) || byAudio.id != owner.id || byAudio.kind != owner.kind)
            {
                std::uint8_t bytes[census::kFieldScanBytes];
                if (!owner.entity || !census::ReadBytes(owner.entity, bytes, sizeof(bytes)))
                    return false;
                std::uint16_t found = 0;
                std::int32_t at = 0;
                if (!census::FindFieldBase(bytes, sizeof(bytes), audio, found, at))
                    return false;
                base = AudioBase::Field;
                field = found;
                offset = at;
                return true;
            }
            range = byAudio;
        }
        // A ped's tracker component is not a base the puppet rebuilds.
        if (range.base == AudioBase::Entity && range.lo != range.entity)
            return false;
        base = range.base;
        field = range.base == AudioBase::Field ? range.field : 0;
        offset = static_cast<std::int32_t>(audio - range.lo);
        return true;
    }

    void BeginRecording(Context& context)
    {
        (void)context;
        g_pending.clear();
        g_known.clear();
        g_live.clear();
        g_liveCheckedMs = 0;
        g_adoptCheckedMs = 0;
        g_adoptFirst = true;
        g_record = RecordStats{};
    }

    void Record(Context& context)
    {
        const int mode = census::CaptureMode();
        if (mode <= 0 || std::strcmp(census::Phase(), "rec") != 0)
            return;
        REPLAY_PROFILE_ZONE("rec.sound_capture");
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        if (nowMs < g_liveCheckedMs || nowMs < g_adoptCheckedMs)
        {
            // The buffer started over (a new recording without BeginRecording).
            g_liveCheckedMs = g_adoptCheckedMs = 0;
        }
        // New creates of recorded owners.
        for (const census::TickRow& tick : census::Rows())
        {
            if (!tick.normal || tick.owner.kind != OwnerKind::Recorded || !tick.row.sound || tick.row.mine)
                continue;
            if (!census::CaptureClass(tick.owner.cls, mode))
            {
                ++g_record.classSkipped;
                continue;
            }
            g_known.insert(tick.row.sound);
            Candidate candidate;
            candidate.row = tick.row;
            candidate.owner = tick.owner;
            candidate.startMs = tick.clipMs > 0.0 ? static_cast<std::uint32_t>(tick.clipMs) : 0;
            candidate.dueMs = nowMs + kDecideMs;
            candidate.alive = tick.alive;
            g_pending.push_back(candidate);
            ++g_record.candidates;
        }
        // Decide the ones whose tracks had their time.
        for (std::size_t n = 0; n < g_pending.size();)
        {
            Candidate& c = g_pending[n];
            if (nowMs < c.dueMs && c.dueMs - nowMs <= kDecideMs)
            {
                ++n;
                continue;
            }
            if (const char* by = CoveredBy(context, c))
            {
                if (std::strcmp(by, "entity") == 0)
                    ++g_record.coveredEntity;
                else if (std::strcmp(by, "request") == 0)
                    ++g_record.coveredRequest;
                else
                    ++g_record.coveredCollision;
            }
            else
                Capture(context, c.row, c.owner, c.startMs, c.alive ? c.row.out : 0, false);
            g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(n));
        }
        // Sounds alive before their owner entered the recording (a train's
        // loops made when it streamed in), four times a second.
        if (g_adoptFirst || nowMs - g_adoptCheckedMs >= 250)
        {
            g_adoptFirst = false;
            g_adoptCheckedMs = nowMs;
            for (const census::LiveSound& live : census::Live())
            {
                if (g_known.count(live.sound))
                    continue;
                Owner owner;
                if (!census::OwnerOf(live.row.audio, live.row.tracker, owner) || owner.kind != OwnerKind::Recorded)
                    continue;
                g_known.insert(live.sound);
                if (!census::CaptureClass(owner.cls, mode))
                    continue;
                Capture(context, live.row, owner, nowMs < 400 ? 0 : nowMs, live.slot, true);
            }
        }
        // The loops' ends.
        if (nowMs - g_liveCheckedMs >= 250)
        {
            g_liveCheckedMs = nowMs;
            for (std::size_t n = g_live.size(); n-- > 0;)
            {
                std::uint64_t current = 0;
                if (census::ReadQword(g_live[n].slot, current) && current == g_live[n].sound)
                    continue;
                EndLive(context, g_live[n], nowMs, true);
                g_live.erase(g_live.begin() + static_cast<std::ptrdiff_t>(n));
            }
        }
    }

    void EndRecording(Context& context)
    {
        if (census::CaptureMode() <= 0)
            return;
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        // Still pending: decided now (the tracks are closed too).
        for (Candidate& c : g_pending)
        {
            if (!CoveredBy(context, c))
                Capture(context, c.row, c.owner, c.startMs, c.alive ? c.row.out : 0, false);
        }
        g_pending.clear();
        for (const LiveCapture& live : g_live)
            EndLive(context, live, nowMs, false); // alive at the end: plays to the clip's end
        g_live.clear();
        std::ostringstream classes;
        for (std::uint8_t n = 0; n < census::kOwnerClassCount; ++n)
            if (g_record.perClass[n])
                classes << (classes.tellp() > 0 ? ";" : "") << census::ClassToken(static_cast<OwnerClass>(n)) << ":"
                        << g_record.perClass[n];
        context.log << "SOUND_CAPTURE_SUMMARY,phase=record,mode=" << census::CaptureMode()
                    << ",candidates=" << g_record.candidates << ",captured=" << g_record.captured
                    << ",one_shots=" << g_record.oneShots << ",loops=" << g_record.loops << ",adopted=" << g_record.adopted
                    << ",covered_entity=" << g_record.coveredEntity << ",covered_request=" << g_record.coveredRequest
                    << ",covered_collision=" << g_record.coveredCollision << ",class_skipped=" << g_record.classSkipped
                    << ",unresolved=" << g_record.unresolved << ",not_object=" << g_record.notObject
                    << ",invalid=" << g_record.invalid << ",ended=" << g_record.ended
                    << ",clip_sounds=" << context.clip.generalSounds.size()
                    << ",classes=" << (classes.tellp() > 0 ? classes.str() : std::string("none")) << '\n';
    }

    void BeginPlayback(Context& context)
    {
        StopAll();
        g_own.Clear();
        g_play = PlayStats{};
        g_lastMs = 0;
        g_playSounds = context.clip.generalSounds;
        g_coalesce = census::CoalesceRepeatedOneShots(g_playSounds);
        g_waiting.assign(g_playSounds.size(), 0);
        g_clipRefs.clear();
        for (const GeneralSound& sound : g_playSounds)
            g_clipRefs.insert(sound.ref);
    }

    void Play(Context& context, std::uint32_t timeMs)
    {
        const auto& sounds = g_playSounds;
        if (sounds.empty() || census::CaptureMode() <= 0)
            return;
        REPLAY_PROFILE_ZONE("play.sound_capture");
        const bool normal = context.hooks && context.hooks->NormalPlay();
        const std::uint32_t lastMs = g_lastMs;
        const bool step = normal && timeMs >= lastMs && timeMs - lastMs <= kMaxStepMs;
        g_lastMs = timeMs;
        // What the puppets made (not the capture's own starts).
        for (const census::TickRow& tick : census::Rows())
        {
            if (tick.owner.kind == OwnerKind::Puppet && !tick.row.mine && tick.row.sound)
                g_own.Add(tick.owner.id, tick.row.ref, tick.clipMs > 0.0 ? static_cast<std::uint32_t>(tick.clipMs) : 0);
        }
        g_own.Forget(timeMs, kKeepMs);
        if (g_waiting.size() != sounds.size())
            g_waiting.assign(sounds.size(), 0);
        if (!step)
        {
            StopAll();
            return;
        }
        std::uint64_t* slots = Slots();
        g_livePuppetBuilt = false;
        const auto playingOnPuppet = [&](std::uint32_t id, std::uint64_t ref) {
            if (!g_livePuppetBuilt)
            {
                g_livePuppetBuilt = true;
                g_livePuppet.clear();
                for (const census::LiveSound& live : census::Live())
                {
                    if (live.row.mine || !g_clipRefs.count(live.row.ref))
                        continue;
                    Owner owner;
                    if (census::OwnerOf(live.row.audio, live.row.tracker, owner) && owner.kind == OwnerKind::Puppet)
                        g_livePuppet.push_back({owner.id, live.row.ref});
                }
            }
            return std::find(g_livePuppet.begin(), g_livePuppet.end(), std::make_pair(id, ref)) != g_livePuppet.end();
        };
        const auto shortOf = [](const GeneralSound& sound) {
            return sound.oneShot || (sound.ended && sound.endMs - sound.startMs < kLoopMs); };
        // Drive and stop the playing ones.
        for (std::size_t slot = 0; slot < kSlots; ++slot)
        {
            if (!slots[slot])
                continue;
            if (g_slotEvent[slot] >= sounds.size())
            {
                replay::audio::StopPersistentSound(&slots[slot]);
                continue;
            }
            const GeneralSound& sound = sounds[g_slotEvent[slot]];
            const replay::playback::Puppet* puppet = ReadyPuppet(context, sound.entity);
            if (!puppet || timeMs < sound.startMs || (sound.ended && !shortOf(sound) && timeMs >= sound.endMs + kHoldMs))
            {
                replay::audio::StopPersistentSound(&slots[slot]);
                continue;
            }
            if (sound.oneShot && timeMs >= g_slotStartMs[slot] + kOneShotOverrunMs)
            {
                replay::audio::StopPersistentSound(&slots[slot]);
                ++g_play.overrun;
                continue;
            }
            if (g_slotFollows[slot])
                PlaceOnPuppet(slots[slot], puppet->clone);
        }
        // Start the ones due.
        for (std::size_t index = 0; index < sounds.size(); ++index)
        {
            const GeneralSound& sound = sounds[index];
            const std::uint32_t beginMs = sound.startMs + kHoldMs;
            const std::uint32_t windowEndMs = shortOf(sound)
                ? (std::max)(sound.endMs + kHoldMs, beginMs + kOneShotWindowMs)
                : sound.endMs + kHoldMs;
            if (timeMs < beginMs || (sound.ended && timeMs >= windowEndMs))
            {
                g_waiting[index] = 0;
                continue;
            }
            // SoundGenericMode=packets: a recorded packet of the generic player carries
            // this sound.
            if (replay::soundgeneric::PacketCovers(sound.entity, sound.ref, sound.startMs))
            {
                g_waiting[index] = 0;
                continue;
            }
            const bool crossed = lastMs < beginMs || (sound.startMs == 0 && lastMs == 0 && timeMs > 0);
            const bool loop = !shortOf(sound);
            const bool waiting = g_waiting[index] != 0;
            if (!crossed && !loop && !waiting)
                continue;
            if (!loop)
                g_waiting[index] = 1;
            bool playing = false;
            std::size_t free = kSlots;
            for (std::size_t slot = 0; slot < kSlots; ++slot)
            {
                if (slots[slot] && g_slotEvent[slot] == index)
                    playing = true;
                else if (!slots[slot] && free == kSlots)
                    free = slot;
            }
            if (playing)
            {
                g_waiting[index] = 0;
                continue;
            }
            // The puppet's own sound wins (the anti-double rule).
            const bool midSpan = !crossed && !waiting && loop;
            if ((midSpan ? g_own.Ever(sound.entity, sound.ref)
                         : g_own.Take(sound.entity, sound.ref, sound.startMs, timeMs, kBeforeMs)) ||
                (loop && playingOnPuppet(sound.entity, sound.ref)))
            {
                if (!midSpan)
                    ++g_play.byPuppet;
                g_waiting[index] = 0;
                continue;
            }
            const replay::playback::Puppet* puppet = ReadyPuppet(context, sound.entity);
            if (!puppet)
                continue; // tried again next frame while its window lasts
            std::string status = "no_slot";
            bool ok = false;
            bool fallback = false;
            if (free != kSlots)
            {
                std::uint64_t audio = 0;
                if (!census::GeneralSoundValid(sound, census::ImageEnd() - census::ImageBase(), census::kParamsBytes))
                    status = "invalid";
                else
                {
                    audio = PuppetAudio(sound, *puppet, status);
                    if (!audio)
                    {
                        // The puppet has no such audio entity (an object
                        // puppet of a train car, another vehicle class): the
                        // static collision audio placed on the puppet, so the
                        // sound is there at all.
                        if (status == "vtable_differs")
                            ++g_play.vtableDiffers;
                        audio = replay::audio::StaticCollisionAudio();
                        fallback = audio != 0;
                    }
                    if (audio)
                    {
                        std::string error;
                        {
                            census::ModScope mine; // not the puppet's own (OwnCreates)
                            ok = replay::audio::StartEntitySound(audio, sound.ref, false,
                                sound.params.empty() ? nullptr : sound.params.data(), &slots[free], error);
                        }
                        status = ok ? (fallback ? "playing_fallback" : "playing") : error;
                    }
                }
                if (ok)
                {
                    g_slotEvent[free] = index;
                    g_slotStartMs[free] = timeMs;
                    g_slotFollows[free] = sound.follows || fallback;
                    g_waiting[index] = 0;
                    g_play.fallback += fallback ? 1 : 0;
                    if (g_slotFollows[free])
                        PlaceOnPuppet(slots[free], puppet->clone);
                }
                else
                    g_waiting[index] = 0; // one try, as the entity sound track
            }
            else if (waiting && !crossed)
                continue;
            if (ok)
                ++g_play.played;
            else if (free == kSlots)
                ++g_play.noSlot;
            else
                ++g_play.refused;
            g_play.last = status;
            if (replay::log::Verbose() && g_playLogs.Take(40))
            {
                context.log << "SOUND_CAPTURE,phase=play,id=" << census::IdText(sound.entity)
                            << ",cls=" << census::ClassToken(sound.cls) << std::hex << ",ref=0x" << sound.ref << std::dec
                            << ",audio=" << census::BaseToken(sound.base) << ",status=" << status
                            << ",crossed=" << crossed << ",event_ms=" << sound.startMs << ",playhead_ms=" << timeMs << '\n';
            }
        }
    }

    void ForgetPlaybackAfterRestart() { StopAll(); }

    std::string EndPlayback(Context& context)
    {
        StopAll();
        const auto& sounds = context.clip.generalSounds;
        context.log << "SOUND_CAPTURE_SUMMARY,phase=play,mode=" << census::CaptureMode() << ",clip_sounds=" << sounds.size()
                    << ",play_sounds=" << g_playSounds.size() << ",repeat_runs=" << g_coalesce.runs
                    << ",repeat_merged=" << g_coalesce.merged << ",one_shot_overrun=" << g_play.overrun
                    << ",played=" << g_play.played << ",by_puppet=" << g_play.byPuppet << ",refused=" << g_play.refused
                    << ",no_slot=" << g_play.noSlot << ",fallback_static=" << g_play.fallback
                    << ",vtable_differs=" << g_play.vtableDiffers
                    << ",last=" << (g_play.last.empty() ? "none" : g_play.last.c_str()) << '\n';
        if (sounds.empty())
            return std::string();
        std::ostringstream line;
        line << "Sounds captured by the census in this clip (vehicles, train, objects with no other track): " << sounds.size()
             << ". In the replay: played " << g_play.played << ", left to the puppet itself " << g_play.byPuppet
             << ", refused " << g_play.refused << ", no free slot " << g_play.noSlot << ", played on the static audio "
             << g_play.fallback << ".";
        return line.str();
    }
}
