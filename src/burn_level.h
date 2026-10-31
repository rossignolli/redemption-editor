#pragma once

// Burn level of an entity.
// The burn state lives in the instance shader variables of the entity's shader
// effect (draw handler +0x18, returned by the effect's virtual +0x88): +0x1C
// burn level, +0x20 the same with affectAsh=0, +0x24 intensity, +0x28 opacity,
// +0x30 entity mask colour. Peds keep a second effect ([[draw handler +0x748]
// +0xC0]); both blocks are read and written.
// No native reads the level and none writes it on a ped or vehicle, so the
// replay reads and writes the block itself. Found by signature once (Check),
// SEH-guarded; never calls engine code. Offline dump, build 1491.50.
// Research: docs/research/fx-fires-decals-effects.md

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace replay::burn
{
    // Checks the build (the effect virtual, the level accessors, the fire
    // and resolver offsets). Runs once; later calls return the first result.
    bool Check(std::string& error);
    // Check passed.
    bool Enabled();

    // What an entity's blocks hold. The fire path is draw handler +0x18 (the
    // one the fire and the vehicle write); the ped path is +0x748 -> +0xC0
    // (peds only). A block is used only when its effect's virtual +0x88 is
    // the engine's block getter.
    struct State
    {
        bool fireBlock = false;
        bool pedBlock = false;
        bool sameBlock = false;           // both paths reach one block
        std::uint32_t fireVtableRva = 0;  // evidence: effect vtable RVAs
        std::uint32_t pedVtableRva = 0;
        float level = 0.0f;               // +0x1C, fire path first
        float pedLevel = 0.0f;            // +0x1C of the ped path block
        float levelNoAsh = 0.0f;          // +0x20 of the block `level` came from
        float intensity = 0.0f;           // +0x24
        float opacity = 0.0f;             // +0x28
    };

    // Reads an entity (engine address, getScriptHandleBaseAddress). false:
    // disabled, unreadable, or no block on either path.
    bool Read(const void* entity, State& state);

    // Writes `level` at +0x1C of every block the entity has. Returns the
    // blocks written (0: disabled, unreadable or none).
    int Write(const void* entity, float level);

    // The burnt look.
    // The look of a burnt ped, horse or vehicle (charred hide, mane gone, black
    // wagon body) is drawn by the fire renderer from the entity's fire record
    // (g_FireVolumeBuffer, g_FireCharShaderConstants), not from the shader
    // effect above. The engine keeps one record per burning or burnt entity in
    // a tree (a static root pointer, the record manager's +0x60; node +0x08
    // right (tagged), +0x10 left, +0x20 entity, +0x28 key -1; the record is the
    // node). The record outlives the fire (the corpse stays charred) and
    // RESURRECT_PED removes it (the removal (manager, entity, -1, 1)), which
    // takes the look off at once. Record +0xD0: the burn volumes (linked
    // by +0x00, 0xF0 bytes each, count at +0xD8); per volume the extents and
    // age (+0x10..+0x3C), the char amount (+0x40, vec4, 1 = charred) and the
    // growth values (+0xC0..+0xDC); +0x50 is the world centre (the engine's).
    // Record +0x141: flags (0x40 consumed by fire).
    constexpr std::size_t kCharFloats = 24;     // +0x10..+0x4C and +0xC0..+0xDC
    // A wagon burning for a minute has 10 to 20.
    constexpr std::size_t kCharMaxVolumes = 64;
    struct CharVolume
    {
        float v[kCharFloats] = {};
        // Where it sits (FireCharAnchor in replay_recording.h):
        // +0xB0 component, +0xE4 sub, +0xE6 flags, +0xB8 mask, +0x60 dir.
        std::int32_t component = -1;
        std::uint16_t sub = 0xFFFF;
        std::uint8_t flags = 0;
        std::uint32_t mask = 0;
        float dir[3] = {0.0f, 0.0f, 1.0f};
    };
    struct CharState
    {
        bool present = false;
        std::uint8_t flags = 0;
        std::uint32_t count = 0; // the engine's volume count (may exceed the kept volumes)
        std::vector<CharVolume> volumes;
    };
    // A volume that draws nothing: extents (+0x10..+0x18,
    // +0x20..+0x28, +0x3C, +0xC0) to a speck, char amount (+0x40 vec4) to 0;
    // age and timers kept. Pure (tested).
    inline void NeutralizeVolume(CharVolume& volume)
    {
        constexpr float kSpeck = 0.001f;
        for (std::size_t k : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{5}, std::size_t{6},
                 std::size_t{11}, std::size_t{16}})
            volume.v[k] = kSpeck;
        for (std::size_t k = 12; k < 16; ++k)
            volume.v[k] = 0.0f;
    }
    // Checks the fire record sites (the tree root use in
    // _IS_ENTITY_CONSUMED_BY_FIRE's lookup, the removal and its manager in
    // RESURRECT_PED) and the volume code the writes rely on. Runs once.
    bool CheckChar(std::string& error);
    bool CharEnabled();
    // Faults caught in the guarded reads so far (evidence).
    std::uint64_t CharFaults();
    // The entity's record now (guarded reads). false: disabled or unreadable.
    bool ReadChar(const void* entity, CharState& state);
    // A record of the engine's fire record tree (0: none; present false).
    bool ReadRecord(std::uintptr_t record, CharState& state);
    // The fire record tree (its root node pointer at +0), found by
    // signature; 0 when not found.
    std::uintptr_t RecordTree();
    // Writes `state` into the entity's existing record part by
    // part. match[r] is the entity volume (index in ReadChar order) that
    // takes recorded volume r (MatchFireCharVolumes), -1 none: with
    // `anchored` a volume is made for it on its part (CreateVolume), else it
    // is left out. Anchored writes also set the part's flags (low nibble)
    // and mask. Every entity volume no recorded one took is neutralized
    // (NeutralizeVolume). Nothing is written unless the engine's volume code
    // shows the fields (CheckChar). Script thread only.
    struct CharWriteReport
    {
        int written = 0;
        int created = 0;
        int createFailed = 0;
        int neutralized = 0;
    };
    CharWriteReport WriteChar(const void* entity, const CharState& state, const std::vector<int>& match, bool anchored);
    // Volume creation through the engine's own record copy (its clone of a
    // volume, then the link into a record, which counts it and sets its mask
    // from the part), signature checked, SEH guarded. Off by the ini
    // (FireCharCreate=0) or a mismatch.
    // CreateChar gives an entity without one a record (fields copied from
    // any other live record through the engine's record copy, no volumes:
    // WriteChar adds the recorded ones right after, in the same pass, before
    // the engine's update removes an empty record). code 2: no record to copy
    // from.
    bool CreateChar(void* entity, std::uint32_t& code);
    void SetCharCreate(bool on);
    bool CharCreateEnabled();
    // Keeps records through the engine's area clear. The clear (the region
    // clear and its core, called by the replay's fire rebuild through
    // ClearBurntGround) removes every entity record whose entity is in the
    // box; it skips a record with +0x142 bit 1 set and +0xF0 not 0 (its skip
    // test, signature checked). ShieldChars sets both on the
    // records of `entities` (saving the old values), UnshieldChars puts them
    // back; the clear runs between the two on the same thread.
    struct CharShield
    {
        std::uintptr_t record = 0;
        std::uintptr_t entity = 0;
        std::uint8_t flags142 = 0;
        std::uint32_t count0F0 = 0;
    };
    int ShieldChars(const std::vector<const void*>& entities, std::vector<CharShield>& saved);
    void UnshieldChars(const std::vector<CharShield>& saved);
    // Removes the entity's record through the engine (as RESURRECT_PED
    // does, without the resurrect). Script thread only. true when removed.
    bool RemoveChar(void* entity, std::uint32_t& code);
}

