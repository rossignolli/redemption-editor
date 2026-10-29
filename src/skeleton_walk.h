#pragma once

// Pure parts of the skeleton walk (unit tested in ReplayCoreTests), shared by
// the guarded resolver (skeleton_resolve.cpp) and the evidence line.
//
// Every native that reads an entity's bones (GET_ENTITY_BONE_INDEX_BY_NAME
// 0xF96A64, GET_WORLD_POSITION_OF_ENTITY_BONE 0xF99468, PLAY_ENTITY_ANIM
// 0xF9D01C, build 1491.50) gets the skeleton the same way:
//   fragment = entity->vtable[0xB8](entity);
//   skeleton = fragment ? [[fragment+0x128]+0x160]   (0x296ECEC)
//                       : [[entity+0xA8]+0x40];
// The slot has three bodies in the dump: the ped one, the vehicle one
// (`mov rax,[rcx+938h]; ret`) and the generic one at 0x65B6FC, used by the
// object vtables, which returns [entity+0x38] when bit 6 of the dword at
// entity+0xB0 says the entity is a fragment, and null otherwise. Objects (the
// carried pelt p_cs_pelt_large among them) need the generic one.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace replay::skeleton::walk
{
    enum class GetterKind : std::uint8_t
    {
        Unknown = 0,
        Ped,     // mov eax,[rcx+9Ch]; and eax,1FFFFh; ...
        Vehicle, // mov rax,[rcx+938h]; ret
        Generic, // 0x65B6FC: the base entity's default, objects
    };

    // Ped override (prefix, the body continues with the pool lookup).
    constexpr std::uint8_t kPedGetter[] = {
        0x8B, 0x81, 0x9C, 0x00, 0x00, 0x00, // mov eax,[rcx+9Ch]
        0x25, 0xFF, 0xFF, 0x01, 0x00,       // and eax,1FFFFh
    };
    // Vehicle override, whole body.
    constexpr std::uint8_t kVehicleGetter[] = {
        0x48, 0x8B, 0x81, 0x38, 0x09, 0x00, 0x00, // mov rax,[rcx+938h]
        0xC3,                                     // ret
    };
    // Generic body at 0x65B6FC, whole (a leaf without .pdata entry); the
    // first 12 bytes are the `getter=` field of the live evidence line.
    constexpr std::uint8_t kGenericGetter[] = {
        0x8B, 0x81, 0xB0, 0x00, 0x00, 0x00, // mov eax,[rcx+0B0h]
        0xC1, 0xE8, 0x06,                   // shr eax,6
        0xA8, 0x01,                         // test al,1
        0x74, 0x05,                         // je +5
        0x48, 0x8B, 0x41, 0x38,             // mov rax,[rcx+38h]
        0xC3,                               // ret
        0x33, 0xC0,                         // xor eax,eax
        0xC3,                               // ret
    };
    constexpr std::size_t kGetterProbeBytes = 24;

    // Layout read by the generic body and by CreateSkeleton (0xE34A18).
    constexpr std::size_t kEntityFlagsOffset = 0xB0;
    constexpr std::uint32_t kEntityFragmentBit = 6;
    constexpr std::size_t kEntityPhysicsInstOffset = 0x38;
    // CreateSkeleton and the object skeleton native (0xE52368): entity type
    // byte (3 and 4 build their skeleton elsewhere), the draw handler and
    // its drawable, whose skeleton data a skeleton is built from.
    constexpr std::size_t kEntityTypeOffset = 0x30;
    constexpr std::size_t kEntityDrawHandlerOffset = 0x28;
    constexpr std::size_t kDrawHandlerDrawableOffset = 0x08;
    constexpr std::size_t kDrawableSkeletonDataOffset = 0x18;

    inline bool Matches(const std::uint8_t* bytes, std::size_t available, const std::uint8_t* signature,
        std::size_t size)
    {
        return bytes && available >= size && std::memcmp(bytes, signature, size) == 0;
    }

    inline GetterKind ClassifyGetter(const std::uint8_t* bytes, std::size_t available)
    {
        if (Matches(bytes, available, kGenericGetter, sizeof(kGenericGetter)))
            return GetterKind::Generic;
        if (Matches(bytes, available, kVehicleGetter, sizeof(kVehicleGetter)))
            return GetterKind::Vehicle;
        if (Matches(bytes, available, kPedGetter, sizeof(kPedGetter)))
            return GetterKind::Ped;
        return GetterKind::Unknown;
    }

    inline const char* GetterKindName(GetterKind kind)
    {
        switch (kind)
        {
        case GetterKind::Ped: return "ped";
        case GetterKind::Vehicle: return "vehicle";
        case GetterKind::Generic: return "generic";
        default: return "unknown";
        }
    }

    // What the generic body returns for these flags (true: [entity+0x38]).
    inline bool GenericIsFragment(std::uint32_t flags)
    {
        return ((flags >> kEntityFragmentBit) & 1u) != 0;
    }

    // Evidence of one walk; -1 is "not read" (printed as '-').
    struct Evidence
    {
        std::uint8_t getter[12] = {};
        GetterKind kind = GetterKind::Unknown;
        int entityType = -1;
        int isFragment = -1;     // generic: bit 6 of +0xB0
        int fragment = -1;       // generic: [+0x38]; vehicle: [+0x938]
        int fragmentCache = -1;  // [fragment+0x128]
        int fragmentSkel = -1;   // [cache+0x160]
        int owner = -1;          // [entity+0xA8]
        int ownerSkel = -1;      // [owner+0x40]
        int skeletonData = -1;   // [[[entity+0x28]+8]+0x18]
        std::int64_t bones = -1; // [skeleton+0x108] of the skeleton found
    };

    inline std::string Format(const Evidence& evidence)
    {
        char text[320]{};
        int length = std::snprintf(text, sizeof(text), "getter=");
        for (int index = 0; index < 12; ++index)
            length += std::snprintf(text + length, sizeof(text) - length, "%02X", evidence.getter[index]);
        const auto field = [&](const char* name, std::int64_t value) {
            if (length < 0 || static_cast<std::size_t>(length) >= sizeof(text))
                return;
            if (value < 0)
                length += std::snprintf(text + length, sizeof(text) - length, "|%s=-", name);
            else
                length += std::snprintf(text + length, sizeof(text) - length, "|%s=%lld", name,
                    static_cast<long long>(value));
        };
        length += std::snprintf(text + length, sizeof(text) - length, "|gk=%s", GetterKindName(evidence.kind));
        field("type", evidence.entityType);
        field("isfrag", evidence.isFragment);
        field("frag", evidence.fragment);
        field("frag_owner", evidence.fragmentCache);
        field("frag_skel", evidence.fragmentSkel);
        field("owner", evidence.owner);
        field("skel", evidence.ownerSkel);
        field("skel_data", evidence.skeletonData);
        field("bones", evidence.bones);
        return text;
    }
}
