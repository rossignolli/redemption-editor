#pragma once

// The bone a camera marker is attached to or looks at. Shared by the core
// and the editor add-on (pure, no engine access).
//
// A bone reference is a u32: 0 is none (the entity's root); mode 1 is a
// named bone of the list below (its position in the list is stored, so the
// list only grows at the end); mode 2 is a skeleton index (the engine's bone
// index, the index of the recorded pose's object space bones). A named bone
// is portable between entities of the same family (Head on any character);
// an index is picked from the full list and belongs to that entity's skeleton.
//
// Each named bone lists up to three engine names; the first one the skeleton
// has wins (GET_ENTITY_BONE_INDEX_BY_NAME on the puppet). Names with
// evidence: the human ones from a reference dump of player_zero;
// PH_SaddleHorn, SKEL_Tail5, bodyshell, chassis_dummy and seat_front from the
// game's own scripts; the weapon ones from the engine's weapon bone name
// table in .rdata of RDR2 1491.50 (RVA 0x33B74B8 on), the names the weapon
// code looks up on every weapon model. Whether a model has one is decided by
// the engine (WEAPON_BONE_NAMES logs what each model answered). The other
// families' names are candidates; a name the skeleton lacks is not offered.

#include <cstdint>

#include "i18n.h" // TR_NOOP: the bone names' lang keys

namespace replay::camera::bones
{
    enum class Family : std::uint8_t
    {
        None = 0,    // index list only
        Human = 1,
        Horse = 2,
        Animal = 3,  // a ped that is not human
        Weapon = 4,
        Vehicle = 5, // wagons, trains, boats
    };

    constexpr std::uint32_t kModeNamed = 1;
    constexpr std::uint32_t kModeIndex = 2;
    constexpr std::uint32_t kMaxBoneIndex = 1024; // the largest recorded skeletons have about 860 bones

    constexpr std::uint32_t Named(std::uint32_t id) { return (kModeNamed << 16) | (id & 0xFFFFu); }
    constexpr std::uint32_t Index(std::uint32_t index) { return (kModeIndex << 16) | (index & 0xFFFFu); }
    constexpr std::uint32_t ModeOf(std::uint32_t ref) { return ref >> 16; }
    constexpr std::uint32_t ValueOf(std::uint32_t ref) { return ref & 0xFFFFu; }

    struct NamedBone
    {
        Family family;
        const char* key;      // the label's lang key (TR in the editor)
        const char* label;    // shown in the editor (English)
        const char* names[3]; // engine bone names, first found wins (nullptr ends)
        std::uint8_t order = 255; // place in the editor's list within the family (lower first; 255: list order)
    };

    // Append only: a project stores the position.
    constexpr NamedBone kNamed[] = {
        {Family::Human, TR_NOOP("bone.head", "Head"), {"SKEL_Head", nullptr, nullptr}},
        {Family::Human, TR_NOOP("bone.neck", "Neck"), {"SKEL_Neck0", "SKEL_Neck1", nullptr}},
        {Family::Human, TR_NOOP("bone.chest", "Chest"), {"SKEL_Spine4", "SKEL_Spine3", "SKEL_Spine2"}},
        {Family::Human, TR_NOOP("bone.pelvis", "Pelvis"), {"SKEL_Pelvis", nullptr, nullptr}},
        {Family::Human, TR_NOOP("bone.right_hand", "Right Hand"), {"SKEL_R_Hand", nullptr, nullptr}},
        {Family::Human, TR_NOOP("bone.left_hand", "Left Hand"), {"SKEL_L_Hand", nullptr, nullptr}},
        {Family::Human, TR_NOOP("bone.right_foot", "Right Foot"), {"SKEL_R_Foot", nullptr, nullptr}},
        {Family::Human, TR_NOOP("bone.left_foot", "Left Foot"), {"SKEL_L_Foot", nullptr, nullptr}},
        {Family::Horse, TR_NOOP("bone.head", "Head"), {"SKEL_Head", nullptr, nullptr}},
        {Family::Horse, TR_NOOP("bone.neck", "Neck"), {"SKEL_Neck2", "SKEL_Neck1", "SKEL_Neck0"}},
        {Family::Horse, TR_NOOP("bone.saddle", "Saddle"), {"PH_SaddleHorn", "SKEL_Saddle", nullptr}},
        {Family::Horse, TR_NOOP("bone.spine", "Spine"), {"SKEL_Spine3", "SKEL_Spine2", "SKEL_Spine1"}},
        {Family::Horse, TR_NOOP("bone.pelvis", "Pelvis"), {"SKEL_Pelvis", nullptr, nullptr}},
        {Family::Horse, TR_NOOP("bone.tail", "Tail"), {"SKEL_Tail01", "SKEL_Tail1", "SKEL_Tail0"}},
        {Family::Animal, TR_NOOP("bone.head", "Head"), {"SKEL_Head", nullptr, nullptr}},
        {Family::Animal, TR_NOOP("bone.neck", "Neck"), {"SKEL_Neck1", "SKEL_Neck0", nullptr}},
        {Family::Animal, TR_NOOP("bone.spine", "Spine"), {"SKEL_Spine2", "SKEL_Spine1", "SKEL_Spine0"}},
        {Family::Animal, TR_NOOP("bone.pelvis", "Pelvis"), {"SKEL_Pelvis", nullptr, nullptr}},
        {Family::Animal, TR_NOOP("bone.tail", "Tail"), {"SKEL_Tail01", "SKEL_Tail1", "SKEL_Tail5"}},
        // 19..21: replaced by the engine's names (same places).
        {Family::Weapon, TR_NOOP("bone.muzzle", "Muzzle"), {"gun_muzzle", nullptr, nullptr}, 0},
        {Family::Weapon, TR_NOOP("bone.grip", "Grip"), {"gun_gripr", nullptr, nullptr}, 8},
        {Family::Weapon, TR_NOOP("bone.barrel", "Barrel"), {"gun_barrels", nullptr, nullptr}, 1},
        {Family::Vehicle, TR_NOOP("bone.body", "Body"), {"bodyshell", "chassis_dummy", nullptr}},
        {Family::Vehicle, TR_NOOP("bone.front_seat", "Front Seat"), {"seat_front", "seat_dside_f", "SEAT_DSIDE_F"}},
        // The rest of the engine's weapon bone names.
        {Family::Weapon, TR_NOOP("bone.cylinder", "Cylinder"), {"gun_drum", nullptr, nullptr}, 2},
        {Family::Weapon, TR_NOOP("bone.hammer", "Hammer"), {"gun_cock", nullptr, nullptr}, 3},
        {Family::Weapon, TR_NOOP("bone.hammer_2", "Hammer 2"), {"gun_cock1", nullptr, nullptr}, 18},
        {Family::Weapon, TR_NOOP("bone.hammer_fx", "Hammer FX"), {"gun_vfx_hammer", nullptr, nullptr}, 19},
        {Family::Weapon, TR_NOOP("bone.trigger", "Trigger"), {"WAPTrigger1", nullptr, nullptr}, 4},
        {Family::Weapon, TR_NOOP("bone.trigger_2", "Trigger 2"), {"WAPTrigger2", nullptr, nullptr}, 20},
        {Family::Weapon, TR_NOOP("bone.lever", "Lever"), {"WAPLever", nullptr, nullptr}, 5},
        {Family::Weapon, TR_NOOP("bone.pump", "Pump"), {"gun_forend", nullptr, nullptr}, 6},
        {Family::Weapon, TR_NOOP("bone.bolt_knob", "Bolt Knob"), {"gun_knob", nullptr, nullptr}, 7},
        {Family::Weapon, TR_NOOP("bone.support_grip", "Support Grip"), {"gun_gripl", nullptr, nullptr}, 9},
        {Family::Weapon, TR_NOOP("bone.stock", "Stock"), {"gun_butt", nullptr, nullptr}, 10},
        {Family::Weapon, TR_NOOP("bone.butt_marker", "Butt Marker"), {"NM_Butt_Marker", nullptr, nullptr}, 21},
        {Family::Weapon, TR_NOOP("bone.scope", "Scope"), {"WAPScope", "WAPScop", nullptr}, 11},
        {Family::Weapon, TR_NOOP("bone.scope_2", "Scope 2"), {"WAPScop_2", nullptr, nullptr}, 22},
        {Family::Weapon, TR_NOOP("bone.sight", "Sight"), {"WAPSight1", nullptr, nullptr}, 12},
        {Family::Weapon, TR_NOOP("bone.sight_2", "Sight 2"), {"WAPSight2", nullptr, nullptr}, 23},
        {Family::Weapon, TR_NOOP("bone.shell_eject", "Shell Eject"), {"gun_vfx_eject", nullptr, nullptr}, 13},
        {Family::Weapon, TR_NOOP("bone.shell_eject_2", "Shell Eject 2"), {"gun_vfx_eject2", nullptr, nullptr}, 24},
        {Family::Weapon, TR_NOOP("bone.shell_eject_3", "Shell Eject 3"), {"gun_vfx_eject3", nullptr, nullptr}, 25},
        {Family::Weapon, TR_NOOP("bone.shell_eject_4", "Shell Eject 4"), {"gun_vfx_eject4", nullptr, nullptr}, 26},
        {Family::Weapon, TR_NOOP("bone.shell_eject_5", "Shell Eject 5"), {"gun_vfx_eject5", nullptr, nullptr}, 27},
        {Family::Weapon, TR_NOOP("bone.shell_eject_6", "Shell Eject 6"), {"gun_vfx_eject6", nullptr, nullptr}, 28},
        {Family::Weapon, TR_NOOP("bone.magazine", "Magazine"), {"gun_magazine", nullptr, nullptr}, 14},
        {Family::Weapon, TR_NOOP("bone.clip", "Clip"), {"WAPClip", nullptr, nullptr}, 15},
        {Family::Weapon, TR_NOOP("bone.ammo", "Ammo"), {"gun_ammo", nullptr, nullptr}, 16},
        {Family::Weapon, TR_NOOP("bone.projectile_trail", "Projectile Trail"), {"gun_vfx_projtrail", nullptr, nullptr}, 17},
        {Family::Weapon, TR_NOOP("bone.muzzle_2", "Muzzle 2"), {"gun_muzzle1", nullptr, nullptr}, 29},
        {Family::Weapon, TR_NOOP("bone.gun_root", "Gun Root"), {"gun_root", nullptr, nullptr}, 30},
        {Family::Weapon, TR_NOOP("bone.barrel_mount", "Barrel Mount"), {"WAPBarrel", nullptr, nullptr}, 31},
        {Family::Weapon, TR_NOOP("bone.grip_mount", "Grip Mount"), {"WAPGrip", nullptr, nullptr}, 32},
        {Family::Weapon, TR_NOOP("bone.stock_mount", "Stock Mount"), {"WAPStck", nullptr, nullptr}, 33},
        {Family::Weapon, TR_NOOP("bone.suppressor_mount", "Suppressor Mount"), {"WAPSupp", nullptr, nullptr}, 34},
        {Family::Weapon, TR_NOOP("bone.strap", "Strap"), {"WAPStrap", nullptr, nullptr}, 35},
        {Family::Weapon, TR_NOOP("bone.strap_2", "Strap 2"), {"WAPStrap2", nullptr, nullptr}, 36},
        {Family::Weapon, TR_NOOP("bone.wrap", "Wrap"), {"WAPWrap1", nullptr, nullptr}, 37},
        {Family::Weapon, TR_NOOP("bone.misc_mount", "Misc Mount"), {"WAPMisc", nullptr, nullptr}, 38},
        {Family::Weapon, TR_NOOP("bone.weapon_position", "Weapon Position"), {"weapPos", nullptr, nullptr}, 39},
    };
    constexpr std::uint32_t kNamedCount = static_cast<std::uint32_t>(sizeof(kNamed) / sizeof(kNamed[0]));
    static_assert(kNamedCount <= 64, "a target's available named bones fit a u64 mask (two u32 in the snapshot)");

    // The time a camera lookup samples an entity's pose record at. Before
    // the record's first sample (a marker at the clip start; the record
    // begins a frame later) the camera holds the first sample, as the puppet
    // holds its last one after the end (endSlackMs). Reading the live clone
    // there would use the entity's rotation instead of the bone's.
    // false: past the end slack (no record there).
    constexpr bool CameraRecordTime(std::uint32_t timeMs, std::uint32_t firstMs, std::uint32_t lastMs, std::uint32_t endSlackMs,
        std::uint32_t& sampleAt)
    {
        if (timeMs > lastMs + endSlackMs)
            return false;
        sampleAt = timeMs < firstMs ? firstMs : timeMs;
        return true;
    }

    // A reference the core accepts (named in the list, index in range).
    constexpr bool Valid(std::uint32_t ref)
    {
        return ref == 0 || (ModeOf(ref) == kModeNamed && ValueOf(ref) < kNamedCount) ||
            (ModeOf(ref) == kModeIndex && ValueOf(ref) < kMaxBoneIndex);
    }

    inline const char* FamilyName(Family family)
    {
        switch (family)
        {
        case Family::Human: return "human";
        case Family::Horse: return "horse";
        case Family::Animal: return "animal";
        case Family::Weapon: return "weapon";
        case Family::Vehicle: return "vehicle";
        default: return "none";
        }
    }

    // The editor's step through a target's bones: Root (0), the named bones
    // of its family it has (mask bit n = kNamed[n] found on it), then the
    // full list by index 0..boneCount-1. Wraps around. `step` is +1 or -1
    // (PageUp/PageDown pass +-10 through the index part).
    inline std::uint32_t Step(std::uint32_t ref, int step, std::uint64_t namedMask, std::uint32_t boneCount)
    {
        // Positions: 0 root, 1..N named (by `order`, then list order), N+1.. index.
        std::uint32_t named[kNamedCount]{};
        std::uint32_t namedTotal = 0;
        for (std::uint32_t n = 0; n < kNamedCount; ++n)
            if ((namedMask >> n) & 1ull)
                named[namedTotal++] = n;
        for (std::uint32_t a = 1; a < namedTotal; ++a) // insertion sort by order (stable)
            for (std::uint32_t b = a; b > 0 && kNamed[named[b]].order < kNamed[named[b - 1]].order; --b)
            {
                const std::uint32_t t = named[b];
                named[b] = named[b - 1];
                named[b - 1] = t;
            }
        const std::uint32_t indexTotal = boneCount < kMaxBoneIndex ? boneCount : kMaxBoneIndex;
        const std::int64_t total = 1 + static_cast<std::int64_t>(namedTotal) + indexTotal;
        std::int64_t at = 0;
        if (ModeOf(ref) == kModeNamed)
        {
            for (std::uint32_t n = 0; n < namedTotal; ++n)
                if (named[n] == ValueOf(ref))
                    at = 1 + n;
        }
        else if (ModeOf(ref) == kModeIndex && ValueOf(ref) < indexTotal)
            at = 1 + namedTotal + ValueOf(ref);
        at = ((at + step) % total + total) % total;
        if (at == 0)
            return 0;
        if (at <= static_cast<std::int64_t>(namedTotal))
            return Named(named[at - 1]);
        return Index(static_cast<std::uint32_t>(at - 1 - namedTotal));
    }
}
