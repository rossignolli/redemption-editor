#pragma once

// Read-only view of a weapon object's components:
// the weapon ([object+0x448]) keeps them in an array (+0x390, count
// +0x3E0) and a hash table (+0x10, bucket count +0x18); each component's
// info (+0x10) carries the component hash (+0x20). Guarded reads only.

#include <cstdint>
#include <string>
#include <vector>

namespace replay::weapon
{
    struct ComponentView
    {
        std::uint64_t component = 0;
        std::uint64_t info = 0;
        std::uint32_t hash = 0;
        bool fromTable = false; // hash table, else the array
    };

    // Strap state of a weapon, as the owner ped's weapon manager keeps it
    // every frame (offline: the ped weapon manager update): a per-component
    // bit in the weapon's component mask ([weapon+0x80]+0x10, bit = array
    // index + 1), the weapon flags (+0x62C: 0x20000 strap state, 0x8000
    // "update the straps from the attach parent ped"), and the two strap
    // blend weights (+0x5D4, +0x5D8).
    struct StrapState
    {
        std::vector<std::pair<std::uint32_t, bool>> maskBits; // component hash, bit set
        std::uint32_t flags = 0;
        float weightA = 0.0f;
        float weightB = 0.0f;
        bool hasStraps = false; // +0x440 or +0x450 set
    };
    bool ReadStrapState(std::uint64_t object, StrapState& out);

    // Playback: puts the recorded mask bits, weights and strap bits on the
    // puppet weapon and requests the strap update (0x8000) where the ped
    // weapon manager did (strap_state_core.h PlaybackFlags: not while the
    // recording shows the weapon in a hand without the request, the cloth
    // sling while aiming). Guarded writes of plain fields only. The result,
    // when given, has the flags before, the flags wanted and the read back.
    struct StrapApplyResult
    {
        std::uint32_t before = 0;
        std::uint32_t wanted = 0;
        std::uint32_t after = 0;
        bool readBack = false;
    };
    bool ApplyStrapState(std::uint64_t object, const StrapState& state, StrapApplyResult* result = nullptr);

    // Developer bench (strap_test, QA 232-Valentine): which drawable is which
    // on a puppet weapon, and which played strap field changes it. A forced
    // mask bit per component hash (1 hides, 0 shows, -1 removes the override)
    // over the recorded mask of every puppet weapon; flag bits cleared from
    // the recorded word and the two weights forced (below 0: as recorded).
    // The public build ignores all of it.
    void SetStrapTestBit(std::uint32_t hash, int bit);
    void ClearStrapTestBits();
    void SetStrapTestFlags(std::uint32_t clearBits, float weightA, float weightB);
    // strap_test regive <hash>: every puppet weapon that has the component in
    // its recording removes and gives it again once (a new generation each
    // call); the playback reads the request with StrapTestRegive.
    void RequestStrapTestRegive(std::uint32_t hash);
    std::uint32_t StrapTestRegive(std::uint32_t& generation);

    // Attach frame of a weapon object, as the engine attachment extension
    // keeps it ([object+0xA8]+0x50: +0x58 parent bone index, +0x30 offset
    // position, +0x40 offset quaternion xyzw). The shoulder strap update
    // (offline 0x118F1C0) rebuilds the weapon's frame from the parent bone
    // matrix and these offsets, so a puppet weapon attached to the entity
    // root (bone -1) gets its strap computed from the wrong bone.
    struct AttachFrame
    {
        std::int16_t bone = -1;
        float position[3] = {0.0f, 0.0f, 0.0f};
        float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    };
    // False when the object has no attachment extension.
    bool ReadAttachFrame(std::uint64_t object, AttachFrame& out);

    // The weapon object's look read from its CWeapon with guarded reads
    // (weapon_look_core.h), layout from the core of
    // HAS_WEAPON_GOT_WEAPON_COMPONENT 0x10EABB4 and the wear getters and
    // setters. ReadComponentList: every component the engine lists (model
    // components in the array, decorations in the hash table), normalized;
    // false when the weapon or a bound is not readable.
    bool ReadComponentList(std::uint64_t object, std::vector<std::uint32_t>& out);
    struct WearFields
    {
        float degradation = 0.0f; // +0xD0
        float permanent = 0.0f;   // +0xD4 (rust)
        float soot = 0.0f;        // +0xD8
        float damage = 0.0f;      // +0xDC
        float dirt = 0.0f;        // +0xE0
        float e4 = 0.0f;          // +0xE4 (sixth channel, role not proven)
        bool hasInfo = false;     // weapon info at +0x60 (the setters need it)
    };
    bool ReadWear(std::uint64_t object, WearFields& out);
    // Playback only, on a puppet weapon the replay owns (ini
    // WeaponWearWrite): the two wear fields no native sets, rust and the
    // sixth channel, written as plain floats clamped to 0..1.
    bool WriteWearRaw(std::uint64_t object, float permanent, float e4);
    // The dirt the drawing uses (weapon shader effect, weapon_look_core.h
    // kEffectDirt); false while the drawable has no weapon effect.
    bool ReadWearShaderDirt(std::uint64_t object, float& value);

    // Strap visibility. Build 1491.50: the weapon system's per-frame pass over
    // every weapon instance (0xC23738 -> 0x117D49C) ends by calling
    // 0x1172E44(weapon), which looks the weapon up in the attach-parent ped's
    // inventory items and, when no item owns it, sets the hidden bit of the
    // three strap components. That pass runs after the script update and before
    // the draw list, so the recorded mask a puppet weapon gets from
    // ApplyStrapState never reaches the render. The detour skips the hide for
    // weapons whose object is a playback puppet (set from the script thread)
    // and forwards every other weapon to the original untouched.
    // Signature-checked, same MinHook pattern as the fx and audio observers.
    bool InstallStrapVisibility(std::string& error);
    void ShutdownStrapVisibility();
    // Objects (CObject addresses) whose straps keep the script-side mask.
    void SetStrapOwnedObjects(const std::vector<std::uint64_t>& objects);
    struct StrapVisibilityStats
    {
        std::uint64_t calls = 0;   // detour calls
        std::uint64_t skipped = 0; // hides skipped for owned objects
        std::uint64_t faults = 0;  // unreadable weapon (forwarded)
    };
    StrapVisibilityStats StrapVisibilitySnapshot();

    // Ini WeaponPassPose, default 1. The weapon system's per-frame pass over
    // every weapon instance, 0x117D49C(CWeapon*) called from 0xC23738,
    // writes the weapon object's own bones after the script tick
    // (0x1182660 -> 0x2648C40, the turning part's local; 0x264967C, the
    // object matrices of the component bone pairs). A replay weapon's pose
    // written by the script thread is then overwritten. The detour forwards
    // every call and, only for a playback puppet weapon with an armed pose
    // slot (published from the script thread), writes that slot's pose right
    // after the pass, so the replay owns its objects' bones.
    // Signature-checked; no natives in the detour.
    bool InstallWeaponPassPose(std::string& error);
    void ShutdownWeaponPassPose();
    struct WeaponPoseSlot
    {
        std::uint64_t object = 0; // CObject address of the puppet weapon
        std::uint32_t slot = 0;   // pose override slot
    };
    void SetWeaponPoseSlots(const std::vector<WeaponPoseSlot>& slots);
    struct WeaponPassStats
    {
        std::uint64_t calls = 0;  // detour calls (every weapon, every frame)
        std::uint64_t hits = 0;   // calls on a published puppet weapon
        std::uint64_t writes = 0; // pose writes made after the pass
        std::uint64_t faults = 0; // unreadable CWeapon (forwarded)
    };
    WeaponPassStats WeaponPassSnapshot();

    // The recording side.
    // A recorded weapon in a hand (published from the recorder tick: object,
    // pose record id) is sampled right after the pass, and the pass's own
    // change to its bones is measured (before against after, per bone).
    struct WeaponRecordEntry
    {
        std::uint64_t object = 0;
        std::uint32_t recordId = 0;
    };
    void SetWeaponRecordIds(const std::vector<WeaponRecordEntry>& entries);
    constexpr std::uint32_t kPassBones = 64; // per-bone counts kept for the first 64 bones
    struct WeaponPassRecordStats
    {
        std::uint32_t calls = 0;         // passes over this weapon with the record armed
        std::uint32_t compared = 0;      // of those, before and after read
        std::uint32_t changed = 0;       // the pass changed a bone (local or object, 1 degree / 1 mm)
        std::uint32_t maxMilliDeg = 0;   // the largest change of one bone in one pass
        int worstBone = -1;
        std::uint32_t boneChanged[kPassBones] = {}; // passes that changed each bone
    };
    bool WeaponPassRecord(std::uint32_t recordId, WeaponPassRecordStats& out);
    void ResetWeaponPassRecords();
}
