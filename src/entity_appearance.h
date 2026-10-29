#pragma once

// Metaped appearance, recorded in the create packet and rebuilt on the
// playback puppet. Script thread only.

#include "entity_lifecycle.h"
#include "metaped_state.h"

#include "types.h"

namespace replay::entity::appearance
{
    void Capture(Ped ped, Appearance& out);

    // Equips the recorded outfit when the model has it. Returns false when
    // there is no usable outfit.
    bool ApplyOutfit(Ped ped, const Appearance& appearance, Hash model);
    // Replaces every layer of `ped` with the recorded layers.
    void ApplyLayers(Ped ped, const Appearance& appearance);

    // The shop items the ped wears with their wearable states (one native per
    // component), and the puppet dressed as the game dresses the story player:
    // the categories emptied, the recorded layers, then each shop item and its
    // state, _SET_ACTIVE_META_PED_COMPONENTS_UPDATED, _UPDATE_PED_VARIATION.
    void ReadShopItems(Ped ped, std::vector<metaped::ShopItem>& out);
    void DressByShopItems(Ped ped, const Appearance& appearance, const std::vector<metaped::ShopItem>& items);

    // The layer count (one native) and the fingerprint of the layers as
    // appearance_track.h's AppearancePrint computes it from a recorded
    // Appearance (1 + count natives, no tints). Fingerprint is false when the
    // ped has no layers (the engine empties them for a frame or two while it
    // rebuilds the metaped).
    int LayerCount(Ped ped);
    bool Fingerprint(Ped ped, std::uint64_t& print, int& count);

    struct Match
    {
        std::uint32_t recorded = 0;
        std::uint32_t present = 0; // recorded layers found on the ped
        std::uint32_t pedLayers = 0;
        bool Exact() const { return present == recorded && pedLayers == recorded; }
    };
    Match Compare(Ped ped, const Appearance& appearance);

    // The ped's layers with the category of each (one more native per layer),
    // for the layer diff of a puppet that could not build the recorded skeleton.
    void ReadWithCategories(Ped ped, std::vector<MetaPedAsset>& layers, std::vector<Hash>& categories);
}
