#pragma once

// The rules of the registered (continuous) effects in playback.
//
// Build 1491.50: the registered get-or-create core 0x2C49B08 sets the entry's
// "got this frame" byte (+0x28 = 1, 0x2C49D02) on every call; the registry
// update 0x2C52554 walks the entries once per frame and, for one not got
// (+0x28 == 0) whose instance is playing (+0x344 == 2), calls the instance
// stop 0x29E7C3C(inst, 0) (0x2C52635), then frees the entry when the instance
// reached state 5 (0x2C52651 -> 0x2C50F68, 0x2C4DA88); an entry that was got
// has the byte cleared for the next frame (0x2C5268F). So the engine does stop
// an instance the replay stops calling, one frame later, letting its particles
// die off. The replay stops it itself at the track's end (looped effects are
// stopped explicitly), and kills it (state 4, a removal on a cursor jump) when
// a scrub starts, so the scrub leaves no trails.
//
// Pure (no game); unit tested in tests/sound_particles_tests.cpp.

#include <cstdint>
#include <cstring>

namespace replay::regfx
{
    // What the registered track playback does at this tick.
    enum class Mode : std::uint8_t
    {
        Normal, // tracks in force get or create their instances
        Hold,   // paused: live instances keep their state, none is created
        Clear,  // scrubbing: live instances are killed, none is created
    };

    inline Mode PlayMode(bool normalPlay, bool scrubbing)
    {
        if (normalPlay)
            return Mode::Normal;
        return scrubbing ? Mode::Clear : Mode::Hold;
    }

    // A track may create its instance only in normal play, and an attached track
    // only when the entity it rides is there (its puppet, or the same world
    // object): an effect whose owner is not in the replay is not shown on its own
    // at the recorded path (e.g. the locomotive smoke of a train the clip does not
    // have).
    inline bool MayCreate(Mode mode, bool attached, bool ownerPresent)
    {
        return mode == Mode::Normal && (!attached || ownerPresent);
    }

    // The engine's instance states (+0x344).
    constexpr std::uint32_t kStatePlaying = 2;
    constexpr std::uint32_t kStateStopping = 3;
    constexpr std::uint32_t kStateKilled = 4;
    constexpr std::uint32_t kStateFinished = 5;

    // What an instance looks like one second after its track ended.
    enum class AfterEnd : std::uint8_t
    {
        Unreadable, // the memory could not be read
        Reused,     // another effect (rule), one of our live tracks, or moved away: not ours
        AliveOurs,  // still playing, same effect, where we left it: not stopped
        Finishing,  // same effect, stopping or done (states 3 to 5)
    };

    struct Identity
    {
        std::uint64_t rule = 0;
        std::uint32_t hash = 0;
        std::uint32_t state = 0;
        float position[3] = {};
        float offset[3] = {}; // +0x280, the offset row of +0x250 (on the bone when attached)
    };

    constexpr float kSamePlaceM = 3.0f;

    // An attached instance rides an entity that moves (an instance on a galloping
    // horse is metres away one second later and must not read as reused); it is
    // compared by its offset on the bone (+0x250), which only a new tenant
    // changes. A world one by its place.
    inline AfterEnd ClassifyAfterEnd(bool readable, const Identity& atEnd, const Identity& now, bool heldByLiveTrack,
        bool attached = false)
    {
        if (!readable)
            return AfterEnd::Unreadable;
        if (heldByLiveTrack || now.rule != atEnd.rule || now.hash != atEnd.hash)
            return AfterEnd::Reused;
        const float* a = attached ? atEnd.offset : atEnd.position;
        const float* b = attached ? now.offset : now.position;
        const float limit = attached ? 0.01f : kSamePlaceM;
        const float dx = b[0] - a[0];
        const float dy = b[1] - a[1];
        const float dz = b[2] - a[2];
        if (dx * dx + dy * dy + dz * dz > limit * limit)
            return AfterEnd::Reused;
        if (now.state == kStatePlaying)
            return AfterEnd::AliveOurs;
        if (now.state >= kStateStopping && now.state <= kStateFinished)
            return AfterEnd::Finishing;
        return AfterEnd::Reused; // 0, 1, 6: a pooled instance set up again
    }

    // The stop at a track's end is ours to make only on an instance that is
    // still the one we made (same effect rule) and still playing.
    inline bool StopAllowed(bool readable, const Identity& created, const Identity& now)
    {
        return readable && now.rule == created.rule && now.hash == created.hash && now.state == kStatePlaying;
    }

    // Recording: a track that moved onto an owner the recording does not
    // have ends there when that owner is an actor (a ped or a vehicle moves
    // away; a map object stays where the effect was recorded).
    inline bool EndOnLostOwner(bool ownerIsActor) { return ownerIsActor; }

    // When a map effect ends, its instance address can go to a new tenant within
    // the 250 ms the key stays live in the recording; that tenant's attach must
    // not be taken as the track's owner (the whole track would ride, say, a horse's
    // hoof bone with the map offset). The engine hands an instance to another
    // effect only after its key stopped calling (registry update 0x2C52554 frees
    // the entry at state 5), so an attach record belongs to a key only while the
    // key is called: in a tick where it was called, or right after its last call
    // (the site attaches right after the get, dt_us 0 to 14).
    constexpr std::uint64_t kKeyAttachWindowUs = 2000;

    inline bool AttachBelongsToKey(bool calledThisTick, std::uint64_t attachUs, std::uint64_t lastCallUs)
    {
        return calledThisTick || attachUs <= lastCallUs + kKeyAttachWindowUs;
    }

    // Playback: an attached instance sits on its entity's bone, so the recorded
    // parent matrix (+0x210, that bone's world matrix) is at the entity. A track
    // whose parent is far from the puppet it would ride was recorded on another
    // entity: it is not created on that puppet. A parent of zeros or not finite is
    // unknown and does not decide.
    constexpr float kOwnerParentMaxM = 20.0f;

    // Finite by the bits (the ASI builds with /fp:fast, where x == x may fold).
    inline bool FiniteBits(float value)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return (bits & 0x7F800000u) != 0x7F800000u;
    }

    inline bool ParentAtOwner(const float parent[16], const float owner[3], float maxM = kOwnerParentMaxM)
    {
        const float px = parent[12];
        const float py = parent[13];
        const float pz = parent[14];
        if (!FiniteBits(px) || !FiniteBits(py) || !FiniteBits(pz) || (px == 0.0f && py == 0.0f && pz == 0.0f))
            return true;
        const float dx = px - owner[0];
        const float dy = py - owner[1];
        const float dz = pz - owner[2];
        return dx * dx + dy * dy + dz * dz <= maxM * maxM;
    }
}
