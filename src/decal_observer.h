#pragma once

// Read-only observer of the engine decal add and its player.
//
// Every decal (bullet hole, blood splat, scorch, footprint, script decal)
// ends in one rage function that takes a slot from the decal manager's
// queue and copies a settings struct into it: build 1491.50 core 0x2C2EF08
// (manager, entity, settings, id, flag) -> id or 0. Two callers: the
// generic path 0x2C2F054 (allocates the id first; reached from the game's
// decal manager 0x125F9EC by weapon impacts, explosions, blood and the
// ADD_DECAL native) and an internal re-add 0x2C49168. A detour on the core
// records each call after the original ran: entity, id, flag, result, the
// caller return addresses and a copy of the settings (0x14C bytes, the
// size the copy 0x2C2D144 reads).
//
// Playback adds a decal through the generic path 0x2C2F054 with a recorded
// settings copy (its +0xB8 as recorded) and the puppet entity. Offline evidence:
// natives ADD_DECAL/REMOVE_DECAL/IS_DECAL_ALIVE handlers in the dump, the
// manager pointer at 0x5B1F940. The detour never calls natives.

#include <cstdint>
#include <string>
#include <vector>

#include "decal_add_guard.h"    // guard::Refusals
#include "decal_producer_gate.h" // GateCounters

namespace replay::decal
{
    constexpr std::size_t kSettingsBytes = 0x150;
    constexpr std::uint32_t kCallerDepth = 3;
    // Return address into the generic wrapper after its call of the core.
    constexpr std::uint32_t kGenericReturn = 0x2C2F09A;
    constexpr std::size_t kSettingsAttachFlags = 0x13A; // bit 0: attached to the entity
    // Word the game's path 0x125A000 computes with 0x14126F0 from its impact
    // record (the 4th argument, never the entity): 0x14126F0 caches the id in
    // that record (+0x70 = 1, +0x72 = id). Given an entity it writes those two
    // fields into the entity: a stand-in deck tile jumped from x -775.473 to
    // -768.000 (the wagon dynamite bench). Playback keeps the
    // recorded word.
    constexpr std::size_t kSettingsEntityId = 0xB8;
    // Settings +0x88 is a pointer to a decal info record (0x160 B) in the
    // game's info table 0x3CE4F00 (every one of the 11 callers of the lookup
    // 0x2C3A3D8 passes that table; the game's path 0x125A000 writes the
    // lookup result at +0x88). A raw pointer does not survive a save load:
    // reading the freed record through it crashes at RDR2.exe+0x2C2FFDE.
    // The recording keeps (info hash, index); playback asks the table again.
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

    // A removal by id (0x2C49FCC, the REMOVE_DECAL path and the engine's own
    // expiry of pools that are re-added while they grow).
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
    // (scorch of explosions and fires on the deck slab). Add 0x140CF14:
    // (manager 0x4F15040, &type hash, entity, int, &position, &direction,
    // &side, size, u16, u8, int, int, owner*).
    // REMOVE_DECALS_IN_RANGE / FROM_OBJECT do not remove them.
    // The 13th argument is not a decal info. The add has two callers: the
    // blood record finalize 0x1285DA4 passes the blood record (return 0x1285F50),
    // and the rage decal callbacks (vtable 0x3472C00 slot +0x1E8, 0x124AC84,
    // return 0x124AE3C) pass 0: the automatic mirror of a rage decal that
    // landed on an entity (a bullet mark on a wooden deck). The engine makes
    // that mirror again when the rage decal comes back with its entity.
    constexpr std::uint32_t kRageMirrorProjReturn = 0x124AE3C;
    // The pool add 0x125CCF4 returns here from the game's decal add 0x125F9EC.
    constexpr std::uint32_t kPoolAddReturn = 0x125D332;
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
    // drain 0x1431DD4 is hooked and DecalGate is not 0 in the ini; the
    // counters say whether the two producers met (waits) and for how long.
    bool DrainGateInstalled();
    GateCounters GateStats();
    void Shutdown();
}
