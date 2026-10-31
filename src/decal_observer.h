#pragma once

// Read-only observer of the engine decal add and its player.
//
// Every decal (bullet hole, blood splat, scorch, footprint, script decal)
// ends in one rage function that takes a slot from the decal manager's
// queue and copies a settings struct into it: the core (manager, entity,
// settings, id, flag) -> id or 0. Two callers: the generic wrapper (allocates
// the id first; reached from the game's decal add by weapon impacts,
// explosions, blood and the ADD_DECAL native) and an internal re-add. A
// detour on the core records each call after the original ran: entity, id,
// flag, result, the caller return addresses and a copy of the settings (0x14C
// bytes, the size the core's settings copy reads).
//
// Playback adds a decal through the generic wrapper with a recorded settings
// copy (its +0xB8 as recorded) and the puppet entity. Offline evidence:
// natives ADD_DECAL/REMOVE_DECAL/IS_DECAL_ALIVE handlers in the dump, the
// manager pointer the game's decal add loads. The detour never calls natives.

#include <cstdint>
#include <string>
#include <vector>

#include "decal_add_guard.h"    // guard::Refusals
#include "decal_producer_gate.h" // GateCounters

namespace replay::decal
{
    constexpr std::size_t kSettingsBytes = 0x150;
    constexpr std::uint32_t kCallerDepth = 3;
    // The return address into the generic wrapper after its call of the core,
    // as AddCall::callers records it: an RVA of the running exe. A return the
    // game lacks gets a value above every RVA (never a recorded caller), its
    // own; so do the two returns below.
    std::uint32_t GenericReturn();
    constexpr std::size_t kSettingsAttachFlags = 0x13A; // bit 0: attached to the entity
    // Word the game's impact path computes from its impact record (the 4th
    // argument, never the entity), caching the id in that record (+0x70 = 1,
    // +0x72 = id). Given an entity it writes those two fields into the entity:
    // a stand-in deck tile jumped from x -775.473 to -768.000 (the wagon
    // dynamite bench). Playback keeps the recorded word.
    constexpr std::size_t kSettingsEntityId = 0xB8;
    // Settings +0x88 is a pointer to a decal info record (0x160 B) in the
    // game's info table (every one of the 11 callers of the info lookup passes
    // that table; the game's impact path writes the lookup result at +0x88). A
    // raw pointer does not survive a save load: reading the freed record
    // through it crashes the game's decal code. The recording keeps (info hash,
    // index); playback asks the table again.
    constexpr std::size_t kSettingsInfo = 0x88;
    struct InfoKey
    {
        bool known = false;
        std::uint32_t hash = 0;
        std::int32_t index = 0;
    };
    // Reverse lookup of an info pointer (guarded reads of the table, cached
    // per pointer until ClearInfoKeys). known=false when it is not in it.
    InfoKey InfoKeyOf(std::uint64_t info);
    void ClearInfoKeys();
    // The info pointer now for a recorded key, through the game's lookup
    // (signature checked, SEH); 0 when refused.
    std::uint64_t InfoPointerOf(const InfoKey& key, std::string& error);

    struct AddCall
    {
        std::uint64_t timeUs = 0;
        std::uint64_t entity = 0;
        std::uint32_t id = 0;
        std::uint32_t result = 0; // id when added, 0 when refused
        std::uint8_t flag = 0;
        std::uint32_t callers[kCallerDepth] = {};
        std::uint32_t threadId = 0;
        bool byReplay = false; // added by the replay's own AddDecal
        std::uint8_t settings[kSettingsBytes] = {};
    };

    // A removal by id (the REMOVE_DECAL path and the engine's own expiry of
    // pools that are re-added while they grow).
    struct RemoveCall
    {
        std::uint64_t timeUs = 0;
        std::uint32_t id = 0;
        std::uint32_t caller = 0;
    };
    std::uint64_t TotalRemoves();
    void CollectRemovesSince(std::uint64_t& cursor, std::vector<RemoveCall>& out);
    // Removes a decal of ours by id through the same engine function.
    bool RemoveDecal(std::uint32_t id, std::string& error);

    // Projected decals: a second decal system that paints on entities only
    // (scorch of explosions and fires on the deck slab). The projected add:
    // (the projected decal manager, &type hash, entity, int, &position,
    // &direction, &side, size, u16, u8, int, int, owner*).
    // REMOVE_DECALS_IN_RANGE / FROM_OBJECT do not remove them.
    // The 13th argument is not a decal info. The add has two callers: the
    // blood record finalize passes the blood record (blood::FinalizeProjReturn),
    // and the rage decal callback (a vtable slot +0x1E8) passes 0: the automatic
    // mirror of a rage decal that landed on an entity (a bullet mark on a
    // wooden deck). The engine makes that mirror again when the rage decal
    // comes back with its entity. Its return, as ProjCall::caller records it:
    std::uint32_t RageMirrorProjReturn();
    // The blood pool add's return from its call of the game's decal add.
    std::uint32_t PoolAddReturn();
    struct ProjCall
    {
        std::uint64_t timeUs = 0;
        std::uint32_t typeHash = 0;
        std::uint64_t entity = 0;
        std::int32_t arg4 = 0;
        float position[4] = {};
        float direction[4] = {};
        float side[4] = {};
        float size = 0.0f;
        std::uint16_t arg9 = 0;
        std::uint8_t arg10 = 0;
        std::int32_t arg11 = 0;
        std::int32_t arg12 = 0;
        std::uint64_t owner = 0; // the 13th argument (blood record or null)
        std::uint32_t caller = 0;
        bool byReplay = false; // added by the replay's own AddProjDecal (same thread)
    };
    std::uint64_t TotalProj();
    void CollectProjSince(std::uint64_t& cursor, std::vector<ProjCall>& out);
    // Adds a projected decal with `call.owner` as the 13th argument (0 for a
    // null owner, the rage mirror's own value).
    bool AddProjDecal(const ProjCall& call, void* entity, std::string& error);

    bool Install(std::string& error);
    void SetEnabled(bool enabled);
    std::uint64_t NowUs();
    std::uint64_t TotalCalls();
    void CollectSince(std::uint64_t& cursor, std::vector<AddCall>& out);
    // The entity type byte (+0x30) is 4 (a ped), guarded read.
    bool IsPedEntity(std::uint64_t entity);

    bool EnsurePlayer(std::string& error);
    // Adds a decal the way the game does; returns the id, 0 when refused.
    std::uint32_t AddDecal(void* entity, const std::uint8_t settings[kSettingsBytes], std::uint8_t flag,
        std::string& error);
    // Adds (or replaces) a decal with a given id through the core, as the
    // engine re-adds a growing pool with its own id every frame.
    bool AddDecalWithId(void* entity, const std::uint8_t settings[kSettingsBytes], std::uint32_t id, std::uint8_t flag,
        std::string& error);
    // Both adds (decal_add_guard.h) refuse, before the core, a settings info that is null, unreadable or
    // without data (+0x130), an id that is out of the table or not alive
    // (AddDecalWithId), and a ring entry whose previous entity is not a
    // readable pointer. The error names the reason; counted here.
    struct RingStale
    {
        std::uint32_t index = 0;
        std::uint64_t previous = 0;
        bool readFailed = false;
    };
    guard::Refusals GuardRefusals();
    RingStale LastRingStale();
    bool IsDecalAlive(std::uint32_t id, std::string& error);
    // The producer gate between the engine's deferred decal job and the replay's adds. Installed means the
    // job's drain is hooked and DecalGate is not 0 in the ini; the
    // counters say whether the two producers met (waits) and for how long.
    bool DrainGateInstalled();
    GateCounters GateStats();
    void Shutdown();
}
