#pragma once

// The look of a loose hat object, whatever path recorded it.
//
// A hat knocked off a ped is an object of a generic model (0x30762E1A,
// 0xE6856EC4 and 0x0B8A6250 are the only models seen with origin=hat) that
// draws the metaped layer it carries: the drawable, albedo, normal,
// material, palette and three tints, the eight values _SET_META_PED_TAG
// takes. The bare model built by CREATE_OBJECT has no layer and draws as a
// grey octagon. Engine layout, build 1491.50:
//
// - _GET_SHOP_ITEM_HAT_COMPONENT (0x7E02E4218D916B94, handler 0xFB6900)
//   takes the hat object (short_update.c calls it on the object of
//   EVENT_PLAYER_HAT_KNOCKED_OFF and EVENT_PLAYER_HAT_EQUIPPED). Its core
//   0xFD4E58 accepts only an object (entity type byte [+0x30] == 5) with a
//   tag count word [+0x418] above 0, takes the tag array [+0x410] and passes
//   tag 0's +0xC, +0x10, +0x14, +0x18 and the qword +0x0 to the shop item
//   lookup 0x2B34DD4.
// - 0xB440C8 calls the same lookup for a ped's layer with the drawable,
//   albedo, normal and material (its getters 0xB439C4, 0xB42D4C, 0xB44A5C,
//   0xB44704 read slots 0, 8, 0x10, 0x18 of the ped's layer, the order
//   GET_META_PED_ASSET_GUIDS's core 0xB443D8 returns them in) and the packed
//   qword of 0xB4619C, the source GET_META_PED_ASSET_TINT unpacks: so +0xC
//   is the drawable, +0x10 the albedo, +0x14 the normal, +0x18 the material
//   and +0x0 the packed palette and tints.
// - The packing is _SET_META_PED_TAG's 0x2B3FE34: palette in the high dword,
//   tint0 in byte 2, tint1 in byte 1, tint2 in byte 0 (0xB5E4E4 unpacks it
//   the same way).
// - The object streaming walk 0xF1813C steps through the tags by 0x38 (its
//   loop end at 0xF1847B) and requests each tag's assets from +0x20, +0x24,
//   +0x28 (-1: none).
//
// The script thread reads the tags under the guard (replay::mem) after both
// code sites matched their bytes; no engine write. The look recorded is what
// the hat draws; playback rebuilds the hat the way the engine made it: a ped
// puppet wears the layer and drops it (KNOCK_OFF_PED_PROP), and the dropped
// object carries the look (the generic model takes no layer from script).
// A replay editor usually records a loose ped prop the same way: the prop's
// hash, texture and anchor in the object's create, whatever path made it.
//
// Pure: unit tested in tests/scenery_speech_tests.cpp.

#include "entity_lifecycle.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace replay::hatlook
{
    // ---- engine layout (build 1491.50) ---------------------------------------

    constexpr std::uint32_t kTagCoreRva = 0xFD4E58;
    // 0xFD4E58..0xFD4E96: prologue, the type test, the count and the array
    // offsets and tag 0's four dword reads (+0x18, +0xC, +0x10, +0x14) and
    // the qword +0x0.
    constexpr std::uint8_t kTagCoreSignature[] = {
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x33, 0xFF, 0x45, 0x8A, 0xD8, 0x8B,
        0xDA, 0x48, 0x85, 0xC9, 0x74, 0x7F, 0x80, 0x79, 0x30, 0x05, 0x75, 0x79, 0x66, 0x39, 0xB9, 0x18,
        0x04, 0x00, 0x00, 0x76, 0x70, 0x4C, 0x8B, 0x89, 0x10, 0x04, 0x00, 0x00, 0x41, 0x8B, 0x41, 0x18,
        0x45, 0x8B, 0x41, 0x0C, 0x41, 0x8B, 0x51, 0x10, 0x41, 0x8B, 0x49, 0x14, 0x4D, 0x8B, 0x11};
    // 0xF1847B: inc r12; add rdi, 0x38; cmp r12, [rbp-0x10] (the tag walk).
    constexpr std::uint32_t kTagWalkRva = 0xF1847B;
    constexpr std::uint8_t kTagWalkSignature[] = {0x49, 0xFF, 0xC4, 0x48, 0x83, 0xC7, 0x38, 0x4C, 0x3B, 0x65, 0xF0};

    constexpr std::uint32_t kEntityTypeOffset = 0x30;
    constexpr std::uint8_t kEntityTypeObject = 5;
    constexpr std::uint32_t kTagArrayOffset = 0x410;
    constexpr std::uint32_t kTagCountOffset = 0x418;
    constexpr std::uint32_t kTagStride = 0x38;
    constexpr std::size_t kTagBytes = 0x1C; // the packed qword up to the material
    // A dropped hat carries one layer; a few more are read, never past the
    // array's count.
    constexpr std::size_t kMaxTags = 4;

    // The generic models of the hat object (origin=hat in every log seen).
    constexpr std::uint32_t kKnownHatModels[] = {0x30762E1Au, 0xE6856EC4u, 0x0B8A6250u};

    // ---- reading -------------------------------------------------------------

    // One tag: the eight values _SET_META_PED_TAG takes.
    inline entity::MetaPedAsset DecodeTag(const std::uint8_t (&bytes)[kTagBytes])
    {
        std::uint64_t packed = 0;
        std::uint32_t words[4]{};
        std::memcpy(&packed, bytes, sizeof(packed));
        std::memcpy(words, bytes + 0xC, sizeof(words));
        entity::MetaPedAsset asset;
        asset.drawable = words[0];
        asset.albedo = words[1];
        asset.normal = words[2];
        asset.material = words[3];
        asset.palette = static_cast<std::uint32_t>(packed >> 32);
        asset.tint0 = static_cast<std::int32_t>((packed >> 16) & 0xFFu);
        asset.tint1 = static_cast<std::int32_t>((packed >> 8) & 0xFFu);
        asset.tint2 = static_cast<std::int32_t>(packed & 0xFFu);
        return asset;
    }

    // How many tags to read: the object's count, capped; only the first one
    // when the walk's stride was not proven in this build.
    inline std::size_t TagsToRead(std::uint16_t count, bool strideProven)
    {
        const std::size_t cap = strideProven ? kMaxTags : 1;
        return (std::min)(static_cast<std::size_t>(count), cap);
    }

    // A tag kept: a drawable (the walk skips nothing, but an emptied slot
    // has none).
    inline bool UsableTag(const entity::MetaPedAsset& asset) { return asset.drawable != 0; }

    // ---- playback -------------------------------------------------------------

    // A model of the hat object: a known one, or one the clip itself showed
    // as a hat (the model of a dropped hat's prop).
    inline bool IsHatModel(std::uint32_t model, const std::vector<std::uint32_t>& learned)
    {
        for (const std::uint32_t known : kKnownHatModels)
            if (known == model)
                return true;
        return std::find(learned.begin(), learned.end(), model) != learned.end();
    }

    // How a hat object is built in playback.
    enum class Rebuild : std::uint8_t
    {
        Model, // CREATE_OBJECT of the recorded model (every other object; the ini key off)
        Dress, // a puppet wears the recorded layer and drops it
        Hide,  // a hat model without a recorded look: the bare model is the
               // grey octagon, nothing is drawn
    };

    // Only a hat model is dressed or hidden: another object that carries
    // tags (none seen yet; the object streaming walk serves every object
    // with tags) keeps its model, its look recorded for a later build (a
    // knock drops only a hat).
    inline Rebuild ChooseRebuild(bool enabled, bool hasLook, bool hatModel)
    {
        if (!enabled || !hatModel)
            return Rebuild::Model;
        return hasLook ? Rebuild::Dress : Rebuild::Hide;
    }

    inline const char* RebuildText(Rebuild rebuild)
    {
        switch (rebuild)
        {
        case Rebuild::Dress: return "dress";
        case Rebuild::Hide: return "hidden_bare";
        default: return "model";
        }
    }

    // The knock after a wear. A ped that already wore the hat drops it
    // (created_knock_off); one dressed with _SET_META_PED_TAG in the same tick
    // drops nothing (created_plain, the octagon): the layer change lands
    // frames later. A puppet dressed now waits until the layer is on (or
    // kWearWaitMs), at least kWearSettleMs.
    constexpr std::uint64_t kWearSettleMs = 100;
    constexpr std::uint64_t kWearWaitMs = 1200;
    enum class KnockStep : std::uint8_t
    {
        Wait,
        Knock,      // the layer is on (or was already)
        KnockLate,  // the wait ran out with the layer not seen on
    };

    // `woreAtMs`: when the puppet was dressed for the knock (0: it already
    // wore the layer). `layerOn`: every recorded layer found on it now.
    inline KnockStep WhenToKnock(std::uint64_t woreAtMs, std::uint64_t nowMs, bool layerOn)
    {
        if (!woreAtMs)
            return KnockStep::Knock;
        const std::uint64_t since = nowMs >= woreAtMs ? nowMs - woreAtMs : 0;
        if (since < kWearSettleMs)
            return KnockStep::Wait;
        if (layerOn)
            return KnockStep::Knock;
        return since >= kWearWaitMs ? KnockStep::KnockLate : KnockStep::Wait;
    }

    // Whether the dropped object shows the recorded look: its first layer's
    // drawable among the dropped object's (unread: not refused).
    enum class Verify : std::uint8_t
    {
        Same,
        Other,
        Unread,
    };
    inline Verify VerifyDropped(const std::vector<entity::MetaPedAsset>& recorded,
        const std::vector<entity::MetaPedAsset>& dropped, bool droppedRead)
    {
        if (!droppedRead || recorded.empty())
            return Verify::Unread;
        for (const entity::MetaPedAsset& asset : dropped)
            if (asset.drawable == recorded.front().drawable)
                return Verify::Same;
        return Verify::Other;
    }

    inline const char* VerifyText(Verify verify)
    {
        switch (verify)
        {
        case Verify::Same: return "same";
        case Verify::Other: return "other";
        default: return "unread";
        }
    }

    // The dresser's time limits: the helper's model (the puppets' model
    // timeout), the asset request, the drop after the knock and the whole
    // job (a stalled job frees the dresser for the next hat). The job's limit
    // covers every step's own, the helper's included, so a slow first job is
    // not taken over while its client still asks.
    constexpr std::uint64_t kHelperWaitMs = 5000;
    constexpr std::uint64_t kAssetWaitMs = 2000;
    constexpr std::uint64_t kDropWaitMs = 1000;
    constexpr std::uint64_t kJobTimeoutMs = kHelperWaitMs + kAssetWaitMs + kWearWaitMs + kDropWaitMs + 1000;
    // The dresser stands this far under the hat it makes (out of sight; the
    // dropped object is frozen and placed within a frame or two). It is drawn
    // only from its wear to the drop, as an owner puppet is when it knocks;
    // hidden between jobs.
    constexpr float kDresserDepthM = 40.0f;
}
