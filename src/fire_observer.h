#pragma once

// Engine fires. RDR2's fire manager (module+0x4C3F1E0: 128 fires of 0x1F0
// bytes at +0x10, remnants 2048 x 0xF0 at +0xF820) starts every fire, seeds
// and spread children alike, through StartFireCore 0x12B5EA4(mgr, desc); the
// fire shutdown 0x12B2368 (fire, instant, u8) ends it and, for map fires
// (type 12), leaves a remnant (RemnantAdd 0x126E6D8) that keeps smouldering;
// RemnantRemove 0x12B31C4 ends a remnant. Spreading happens in TrySpread
// 0x12B3B54, called only by the fire update. Flames, the burnt ground, embers
// and the lingering smoke are drawn by the area fire system from live fires
// and remnants, so the replay recreates real fires.
// Read-only detours record every call; during playback TrySpread returns
// without spreading so only recorded fires exist. Build 1491.50,
// signature-checked, SEH-guarded; detours never call natives.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace replay::fire
{
    constexpr std::size_t kDescBytes = 0x120;
    constexpr std::size_t kFireBytes = 0x1F0;
    constexpr std::uint32_t kFireCount = 128;

    // Descriptor pointers a replayed start must not carry from the
    // recording. +0xF0 is the parent fire (a slot of the static manager):
    // 0x127B1BC (called by StartFireCore at 0x12B5EF2) passes the desc to
    // 0x1269D8C, which for types 13..15 reads parent+0x70 (the parent's
    // entity) and, when it differs from desc+0x40, reads [desc+0x40]+0x30
    // (0x1269E2A) with no null check. At playback the recorded slot holds
    // another fire (often one on a burning puppet) while this fire's entity
    // is null: the fault at RDR2.exe+0x1269E2A, fault_address=0x30. With
    // +0xF0 null the check returns at once (0x1269DAC) and the recorded world
    // position (+0x20) is kept. +0xF8 is the second entity excluded from the
    // parent line-of-sight

    // probe (0x12B6266), only read when +0x115 bit 0 is set, which the
    // replay clears; a stale heap pointer there is cleared too.
    constexpr std::size_t kDescEntityA = 0x40;
    constexpr std::size_t kDescEntityB = 0x48;
    constexpr std::size_t kDescCulprit = 0xC0;
    constexpr std::size_t kDescParentFire = 0xF0;
    constexpr std::size_t kDescTraceExclude = 0xF8;
    // Returns whether a parent pointer was there.
    inline bool ClearDescParent(std::uint8_t* desc)
    {
        std::uint64_t parent = 0;
        std::memcpy(&parent, desc + kDescParentFire, sizeof(parent));
        std::memset(desc + kDescParentFire, 0, 0x10); // +0xF0 and +0xF8
        return parent != 0;
    }

    // A recorded fire that could not start (the engine refused it, or its
    // recorded entity has no puppet yet) is tried again after a delay, at most
    // kStartMaxAttempts times per playback, then left out. Retrying every frame
    // made every scrub tick rebuild all fires, since the refused ones never
    // matched.
    constexpr std::uint32_t kStartMaxAttempts = 4;
    constexpr std::uint64_t kStartRetryMs = 500;
    struct StartRetry
    {
        std::uint32_t failures = 0;
        std::uint64_t retryAtMs = 0;
        bool gaveUp = false;
    };
    inline bool StartAllowed(const StartRetry& retry, std::uint64_t nowMs)
    {
        return !retry.gaveUp && nowMs >= retry.retryAtMs;
    }
    // Returns true when this failure makes the start give up.
    inline bool NoteStartFailed(StartRetry& retry, std::uint64_t nowMs)
    {
        if (retry.gaveUp)
            return false;
        ++retry.failures;
        retry.retryAtMs = nowMs + kStartRetryMs;
        retry.gaveUp = retry.failures >= kStartMaxAttempts;
        return retry.gaveUp;
    }
    inline void NoteStartOk(StartRetry& retry)
    {
        retry.failures = 0;
        retry.retryAtMs = 0;
    }

    // A reconcile's wanted state against what the replay has, as
    // (start index, burning) pairs. True when everything it has is wanted as
    // it is and only some wanted starts are missing (refused, waiting for a
    // retry, or newly due after a forward jump): those are started alone,
    // without ending and restarting all the others. `missing` gets their
    // indices.
    using FireWant = std::pair<std::uint32_t, bool>;
    inline bool OnlyMissing(std::vector<FireWant> have, std::vector<FireWant> wanted, std::vector<std::uint32_t>& missing)
    {
        missing.clear();
        std::sort(have.begin(), have.end());
        std::sort(wanted.begin(), wanted.end());
        if (std::adjacent_find(have.begin(), have.end()) != have.end())
            return false;
        if (!std::includes(wanted.begin(), wanted.end(), have.begin(), have.end()))
            return false;
        std::vector<FireWant> rest;
        std::set_difference(wanted.begin(), wanted.end(), have.begin(), have.end(), std::back_inserter(rest));
        for (const FireWant& want : rest)
        {
            // A start wanted in the other state is already in `have`: not a
            // plain missing one (the caller rebuilds).
            for (const FireWant& had : have)
                if (had.first == want.first)
                    return false;
            missing.push_back(want.first);
        }
        return true;
    }

    // One path per start index. A start waits either
    // in the retry list or in the ended queue, never in both (each would
    // start it), and a start with a live fire of ours is never started
    // again whatever list asks. QueueStartOnce adds `index` to `into` only
    // when neither list has it; LiveStart tells whether `fires` (anything
    // with .start and .alive) already owns a live fire for it.
    inline bool QueueStartOnce(std::vector<std::uint32_t>& into, const std::vector<std::uint32_t>& other, std::uint32_t index)
    {
        if (std::find(into.begin(), into.end(), index) != into.end() ||
            std::find(other.begin(), other.end(), index) != other.end())
            return false;
        into.push_back(index);
        return true;
    }
    template <class Fires>
    bool LiveStart(const Fires& fires, std::uint32_t index)
    {
        return std::any_of(std::begin(fires), std::end(fires),
            [&](const auto& fire) { return fire.start == index && fire.alive; });
    }

    enum class CallKind : std::uint8_t
    {
        Start = 0,
        Refused = 1,
        Stop = 2,
        RemnantAdd = 3,
        RemnantRemove = 4,
    };

    struct Call
    {
        CallKind kind = CallKind::Start;
        std::uint64_t timeUs = 0;
        std::uint64_t fire = 0;   // the fire (start/stop/remnant add source)

        std::int32_t slot = -1;   // fire slot 0..127
        std::int32_t remnant = -1; // remnant index
        std::uint8_t instant = 0;
        std::uint8_t arg3 = 0;
        std::uint32_t caller = 0;
        std::uint8_t desc[kDescBytes] = {};
        std::uint8_t state[kFireBytes] = {};
    };

    // Fires and remnants alive right now with the descriptor that started
    // them (tracked by the detours all the time, recording or not), for the
    // state a recording starts with: a fire lit before record start.
    struct LiveFire
    {
        int slot = -1;
        std::uint64_t fire = 0;
        std::uint8_t desc[kDescBytes] = {};
        std::uint8_t state[kFireBytes] = {};
    };
    struct LiveRemnant
    {
        int index = -1;
        std::uint64_t entry = 0;
        float position[3] = {};
        std::uint8_t desc[kDescBytes] = {}; // of the fire that left it
    };
    void SnapshotLive(std::vector<LiveFire>& fires, std::vector<LiveRemnant>& remnants);

    bool Install(std::string& error);
    // Recording and playback both need the calls (playback: to know when the
    // engine ends one of our fires).
    void SetObserving(bool on);
    void Shutdown();
    std::uint64_t NowUs();
    std::uint64_t TotalCalls();
    void CollectSince(std::uint64_t& cursor, std::vector<Call>& out);

    // Playback (script thread).
    void SetBlockSpread(bool on);
    // While the playhead stands still (pause, scrub) the world keeps
    // running: the fires and area items of these owners (our fires and their
    // remnants) are updated with a zero time step (fire update 0x12BC2D4,
    // area item update 0x12C0B88).
    void SetFrozenOwners(const std::vector<std::uint64_t>& owners);
    void SetFreeze(bool on);
    // A fire from a recorded descriptor (already patched by the caller); null
    // when the engine refuses.
    void* StartFire(const std::uint8_t* desc, std::string& error);
    bool StopFire(void* fire, bool instant, std::uint8_t arg3, std::string& error);
    // Every remnant within radius of a point is ended (RemnantRemove, no
    // group kill); returns how many.
    int ClearRemnants(const float* center, float radius, std::string& error);
    // The burnt ground: dead area-fire items that never expire (they paint
    // the char and block spread) are cleared in a sphere through the
    // engine's own region clear 0x12A35E4(area 0x4E2F5B0, &centre, &radius
    // splat, full). Fires must be ended first (items of live fires are kept).
    bool ClearBurntGround(const float* center, float radius, std::string& error);
    // The engine's own counts of active fires (manager +0xB18B8, int, read
    // by 0x1272740 and 0x12C8204) and active remnants (+0x88820), guarded
    // reads. Evidence for the recording start: a fire the start detour never
    // saw (lit before the observer was installed: game load, a session reset
    // after the save reload, a hot reload) is in these counts and not in
    // SnapshotLive. False when not installed or unreadable.
    bool EngineLiveCounts(std::uint32_t& fires, std::uint32_t& remnants);
    // The engine's active fires themselves (the list STOP_FIRE_IN_RANGE and
    // the fire manager update walk: count +0xB18B8, int[128] slot indices
    // +0xB18BC, guarded reads), each with its state

    // and whether the start detour saw it start (`tracked`). A fire the
    // detour never saw was lit before the observer was installed or by the
    // ptfx registration, which does not go through StartFireCore
    // (fire_initial.h). False when not installed or unreadable.
    struct EngineFire
    {
        int slot = -1;
        std::uint64_t fire = 0;
        bool tracked = false;
        std::uint8_t state[kFireBytes] = {}; // fields read with memcpy (fire_initial.h FireField)
    };
    bool ReadActiveFires(std::vector<EngineFire>& out);
    // Bench hook (fire_forget): the observer forgets the live fires it
    // tracks, as if they were lit before it was installed. Returns how many.
    int ForgetLiveFires();
    // Area-fire items (the burning / burnt disc of a map fire; area
    // 0x4E2F5B0, pool of 2048 x 0xE0 at [area+0x58]). Guarded reads, and
    // writes of plain floats only on items owned by the replay's fires.
    struct AreaItem
    {
        std::uint64_t owner = 0;     // +0xB8: the fire, then its remnant
        bool primary = true;         // +0xB0 == 0
        float center[3] = {};        // +0x30
        float outer = 0.0f;          // +0x3C, R
        float burnt = 0.0f;          // +0x64, r (grows to R)
        float width = 0.0f;          // +0x6C, flame ring width
        float ember = 0.0f;          // +0x80
        float glow = 0.0f;           // +0x88
        float glowStart = 0.0f;      // +0x8C
        std::uint32_t state = 0;     // +0x60
        std::uint8_t flags = 0;      // +0xD0
    };
    bool ReadAreaItems(std::vector<AreaItem>& out, std::string& error);
    struct AreaItemWrite
    {
        std::uint64_t owner = 0;
        float outer = 0.0f;
        float burnt = 0.0f;
        float ember = -1.0f;
        float glow = -1.0f;
        float width = -1.0f;
        bool done = false; // out
    };
    // The same for many owners in one pass over the pool.
    bool WriteAreaItems(std::vector<AreaItemWrite>& writes, std::string& error);
    std::uint64_t FireAddress(int slot);
    std::uint64_t RemnantAddress(int index);
    int SlotOf(const void* fire);

    // The replay's writes into a fire it started (its
    // age and its random seed) go through one guarded helper. Every start
    // and stop the mod sees on a slot (the detours and the mod's own
    // StartFire/StopFire) bumps the slot's generation; the replay keeps the
    // generation of its start and a write is refused once the slot was
    // stopped or reused since, outside the pool, outside the two known
    // fields, or before the start/stop signatures were verified (Install).
    constexpr std::size_t kFireAgeOffset = 0x188; // float, seconds
    constexpr std::size_t kFireRngOffset = 0x1C8; // 8 bytes, the effect seed

    // Pure checks (tests).
    inline bool FireStateFieldAllowed(std::size_t offset, std::size_t size)
    {
        return (offset == kFireAgeOffset && size == sizeof(float)) ||
            (offset == kFireRngOffset && size == sizeof(std::uint64_t));
    }
    inline bool FireOwnedBy(std::uint32_t slotGeneration, std::uint32_t startGeneration)
    {
        return startGeneration != 0 && slotGeneration == startGeneration;
    }

    // The slot's generation at `fire` (0 outside the pool).
    std::uint32_t FireGeneration(const void* fire);
    bool WriteFireState(void* fire, std::uint32_t startGeneration, std::size_t offset, const void* data,
        std::size_t size, std::string& error);
}
