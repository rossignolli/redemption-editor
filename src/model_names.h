#pragma once

// The game's model names of the clip's entities (joaat hash -> name), for
// the editor's labels: the community knows a_m_m_valtownfolk_01 or
// a_c_horse_arabian_white, not "Character 3". The table comes from the
// decompiled game scripts (tools/director_mcp/build_model_names.py).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>

namespace replay::models
{
    struct ModelName
    {
        std::uint32_t hash;
        const char* name;
    };

    inline constexpr ModelName kModelNames[] = {
#include "model_names_table.inl"
    };

    // The model's name, or nullptr when the table does not know the hash.
    inline const char* NameOf(std::uint32_t hash) noexcept
    {
        const auto at = std::lower_bound(std::begin(kModelNames), std::end(kModelNames), hash,
            [](const ModelName& entry, std::uint32_t value) { return entry.hash < value; });
        return at != std::end(kModelNames) && at->hash == hash ? at->name : nullptr;
    }

    // An entity's label: its model name, or the hash in hex (0x0D7114C9) when
    // the name is unknown. number > 0 tells apart entities of the same model
    // (" #2"); 0 leaves it out.
    inline void Label(std::uint32_t hash, int number, char* out, std::size_t size) noexcept
    {
        if (!out || size == 0)
            return;
        char hex[11];
        const char* name = NameOf(hash);
        if (!name)
        {
            std::snprintf(hex, sizeof(hex), "0x%08X", static_cast<unsigned int>(hash));
            name = hex;
        }
        if (number > 0)
            std::snprintf(out, size, "%s #%d", name, number);
        else
            std::snprintf(out, size, "%s", name);
    }
}
