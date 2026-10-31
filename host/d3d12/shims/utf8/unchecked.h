#pragma once

// Stands in for utfcpp's utf8/unchecked.h (the nested submodule is not in the
// worktrees): the two conversions ReShade's d3d12_impl_device.cpp uses, for
// valid input (adapter names and shader entry points).

#include <cstdint>

namespace utf8::unchecked
{
    template <typename U16It, typename OctetIt>
    OctetIt utf16to8(U16It start, U16It end, OctetIt out)
    {
        while (start != end)
        {
            std::uint32_t cp = static_cast<std::uint16_t>(*start++);
            if (cp >= 0xD800 && cp <= 0xDBFF && start != end)
                cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<std::uint16_t>(*start++) - 0xDC00);
            if (cp < 0x80)
                *out++ = static_cast<char>(cp);
            else if (cp < 0x800)
            {
                *out++ = static_cast<char>(0xC0 | (cp >> 6));
                *out++ = static_cast<char>(0x80 | (cp & 0x3F));
            }
            else if (cp < 0x10000)
            {
                *out++ = static_cast<char>(0xE0 | (cp >> 12));
                *out++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                *out++ = static_cast<char>(0x80 | (cp & 0x3F));
            }
            else
            {
                *out++ = static_cast<char>(0xF0 | (cp >> 18));
                *out++ = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                *out++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                *out++ = static_cast<char>(0x80 | (cp & 0x3F));
            }
        }
        return out;
    }

    template <typename OctetIt, typename U16It>
    U16It utf8to16(OctetIt start, OctetIt end, U16It out)
    {
        while (start != end)
        {
            std::uint32_t cp = static_cast<std::uint8_t>(*start++);
            int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
            cp &= extra == 3 ? 0x07u : extra == 2 ? 0x0Fu : extra == 1 ? 0x1Fu : 0x7Fu;
            while (extra-- > 0 && start != end)
                cp = (cp << 6) | (static_cast<std::uint8_t>(*start++) & 0x3Fu);
            if (cp >= 0x10000)
            {
                cp -= 0x10000;
                *out++ = static_cast<wchar_t>(0xD800 + (cp >> 10));
                *out++ = static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
            }
            else
                *out++ = static_cast<wchar_t>(cp);
        }
        return out;
    }
}
