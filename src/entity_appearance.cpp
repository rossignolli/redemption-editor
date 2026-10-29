#include "entity_appearance.h"
#include "appearance_track.h"

#include "natives.h"

#include <algorithm>

namespace replay::entity::appearance
{
    namespace
    {
        constexpr int kMaxLayers = 128;

        // `categories`, when given, gets each layer's category
        // (_GET_CATEGORY_OF_COMPONENT_AT_INDEX(ped, index, 0), as feud1.c reads
        // the hat among the components).
        void ReadLayers(Ped ped, std::vector<MetaPedAsset>& out, std::vector<Hash>* categories = nullptr)
        {
            out.clear();
            if (categories)
                categories->clear();
            const int count = (std::min)(PED::_GET_NUM_COMPONENTS_IN_PED(ped), kMaxLayers);
            for (int index = 0; index < count; ++index)
            {
                MetaPedAsset asset;
                Hash drawable = 0;
                Hash albedo = 0;
                Hash normal = 0;
                Hash material = 0;
                if (!PED::GET_META_PED_ASSET_GUIDS(ped, index, &drawable, &albedo, &normal, &material))
                    continue;
                Hash palette = 0;
                int tint0 = 0;
                int tint1 = 0;
                int tint2 = 0;
                PED::GET_META_PED_ASSET_TINT(ped, index, &palette, &tint0, &tint1, &tint2);
                asset.drawable = drawable;
                asset.albedo = albedo;
                asset.normal = normal;
                asset.material = material;
                asset.palette = palette;
                asset.tint0 = tint0;
                asset.tint1 = tint1;
                asset.tint2 = tint2;
                out.push_back(asset);
                if (categories)
                    categories->push_back(PED::_GET_CATEGORY_OF_COMPONENT_AT_INDEX(ped, index, 0));
            }
        }

        bool SameLayer(const MetaPedAsset& a, const MetaPedAsset& b)
        {
            return a.drawable == b.drawable && a.albedo == b.albedo &&
                a.normal == b.normal && a.material == b.material &&
                a.palette == b.palette && a.tint0 == b.tint0 &&
                a.tint1 == b.tint1 && a.tint2 == b.tint2;
        }
    }

    void Capture(Ped ped, Appearance& out)
    {
        out = {};
        if (!ped || !ENTITY::DOES_ENTITY_EXIST(ped))
            return;
        out.outfit = PED::_GET_PED_META_OUTFIT_HASH(ped);
        ReadLayers(ped, out.assets);
    }

    bool ApplyOutfit(Ped ped, const Appearance& appearance, Hash model)
    {
        if (!appearance.outfit ||
            !PED::_DOES_META_PED_OUTFIT_EXIST_FOR_PED_MODEL(appearance.outfit, model))
        {
            return false;
        }
        PED::_EQUIP_META_PED_OUTFIT(ped, appearance.outfit);
        PED::_UPDATE_PED_VARIATION(ped, FALSE, TRUE, TRUE, TRUE, FALSE);
        return true;
    }

    void ApplyLayers(Ped ped, const Appearance& appearance)
    {
        const int categories = (std::min)(PED::_GET_NUM_COMPONENT_CATEGORIES_IN_PED(ped), kMaxLayers);
        std::vector<Hash> present;
        for (int index = 0; index < categories; ++index)
            present.push_back(PED::_GET_PED_COMPONENT_CATEGORY_BY_INDEX(ped, index));
        for (const Hash category : present)
            PED::REMOVE_TAG_FROM_META_PED(ped, category, 0);
        for (const MetaPedAsset& asset : appearance.assets)
        {
            PED::_SET_META_PED_TAG(ped, asset.drawable, asset.albedo, asset.normal,
                asset.material, asset.palette, asset.tint0, asset.tint1, asset.tint2);
        }
        // As the game's clothing routine ends, the active components marked
        // updated before the variation update (handler 0xFBB904 -> 0xB569C0 ->
        // 0xB56C04: applies the dressing object's pending shop item entries,
        // then 0xB51E80; isMp unused).
        PED::_SET_ACTIVE_META_PED_COMPONENTS_UPDATED(ped, FALSE);
        PED::_UPDATE_PED_VARIATION(ped, FALSE, TRUE, TRUE, TRUE, FALSE);
    }

    void ReadShopItems(Ped ped, std::vector<metaped::ShopItem>& out)
    {
        out.clear();
        if (!ped || !ENTITY::DOES_ENTITY_EXIST(ped))
            return;
        const int count = LayerCount(ped);
        for (int index = 0; index < count; ++index)
        {
            BOOL status = FALSE;
            Hash state = 0;
            const Hash item = PED::_GET_SHOP_ITEM_COMPONENT_AT_INDEX(ped, index, FALSE, &status, &state);
            metaped::AddShopItem(out, item, state, status != FALSE);
        }
    }

    void DressByShopItems(Ped ped, const Appearance& appearance, const std::vector<metaped::ShopItem>& items)
    {
        // The game's routine (story mode, isMp false): every category
        // emptied of its shop item (REMOVE_SHOP_ITEM_FROM_PED_BY_CATEGORY,
        // p2 0) and of its layer; the recorded layers (the body, the hair
        // and anything no item gives); then each item and its state.
        const int categories = (std::min)(PED::_GET_NUM_COMPONENT_CATEGORIES_IN_PED(ped), kMaxLayers);
        std::vector<Hash> present;
        for (int index = 0; index < categories; ++index)
            present.push_back(PED::_GET_PED_COMPONENT_CATEGORY_BY_INDEX(ped, index));
        for (const Hash category : present)
        {
            PED::REMOVE_SHOP_ITEM_FROM_PED_BY_CATEGORY(ped, category, 0, FALSE);
            PED::REMOVE_TAG_FROM_META_PED(ped, category, 0);
        }
        for (const MetaPedAsset& asset : appearance.assets)
        {
            PED::_SET_META_PED_TAG(ped, asset.drawable, asset.albedo, asset.normal,
                asset.material, asset.palette, asset.tint0, asset.tint1, asset.tint2);
        }
        for (const metaped::ShopItem& item : items)
        {
            PED::_APPLY_SHOP_ITEM_TO_PED(ped, item.item, FALSE, FALSE, FALSE);
            if (item.state)
                PED::_UPDATE_SHOP_ITEM_WEARABLE_STATE(ped, item.item, item.state, 0, FALSE, 1);
        }
        PED::_SET_ACTIVE_META_PED_COMPONENTS_UPDATED(ped, FALSE);
        PED::_UPDATE_PED_VARIATION(ped, FALSE, TRUE, TRUE, TRUE, FALSE);
    }

    int LayerCount(Ped ped)
    {
        return (std::min)(PED::_GET_NUM_COMPONENTS_IN_PED(ped), kMaxLayers);
    }

    bool Fingerprint(Ped ped, std::uint64_t& print, int& count)
    {
        print = 0;
        count = LayerCount(ped);
        if (count <= 0)
            return false;
        std::uint64_t sum = 0;
        std::size_t layers = 0;
        for (int index = 0; index < count; ++index)
        {
            Hash drawable = 0;
            Hash albedo = 0;
            Hash normal = 0;
            Hash material = 0;
            // Same skip as ReadLayers, so the print matches a Capture.
            if (!PED::GET_META_PED_ASSET_GUIDS(ped, index, &drawable, &albedo, &normal, &material))
                continue;
            sum += replay::LayerPrint(drawable, albedo, normal, material);
            ++layers;
        }
        print = replay::CombineLayerPrints(sum, layers);
        return true;
    }

    Match Compare(Ped ped, const Appearance& appearance)
    {
        Match match;
        match.recorded = static_cast<std::uint32_t>(appearance.assets.size());
        std::vector<MetaPedAsset> layers;
        ReadLayers(ped, layers);
        match.pedLayers = static_cast<std::uint32_t>(layers.size());
        for (const MetaPedAsset& recorded : appearance.assets)
        {
            if (std::any_of(layers.begin(), layers.end(),
                    [&](const MetaPedAsset& layer) { return SameLayer(layer, recorded); }))
            {
                ++match.present;
            }
        }
        return match;
    }

    void ReadWithCategories(Ped ped, std::vector<MetaPedAsset>& layers, std::vector<Hash>& categories)
    {
        layers.clear();
        categories.clear();
        if (!ped || !ENTITY::DOES_ENTITY_EXIST(ped))
            return;
        ReadLayers(ped, layers, &categories);
    }
}
