#pragma once

// ReShade API 20 (ReShade 6.8.0) inserted a query_type parameter after `heap`
// in reshade::api::device::get_query_heap_results, in the same vtable slot
// (32, +0x100), with no compatibility path for add-ons built against an older
// API. The editor is built with the API 18 header (ReShade 6.6.2). A call
// through the API 18 signature on ReShade 6.8 shifts every argument by one:
// the driver gets `count` as firstQuery, the low bits of `results` as
// queryCount and the stride slot as pData, and writes there (a crash with
// pData = 0x100000008).
//
// So the export picks the signature from the version of the ReShade module
// loaded in the process (its ReShadeVersion export):
//   6.0 to 6.7.x: API 18, the header's call;
//   6.8.x: API 20, through slot 32 with the new argument list;
//   anything else (or no version): Unknown, the export refuses with a message.
// A future ReShade is refused rather than guessed: it may move the slot again
// or add a compatibility path. Pure (no ReShade or Windows header).

#include <cstdint>

namespace render_export
{
    enum class QueryAbi : std::uint8_t
    {
        Unknown = 0,
        Api18 = 18,
        Api20 = 20,
    };

    inline const char* QueryAbiName(QueryAbi abi)
    {
        switch (abi)
        {
        case QueryAbi::Api18: return "18";
        case QueryAbi::Api20: return "20";
        default: return "unknown";
        }
    }

    // `text`: ReShadeVersion, "major.minor.patch" plus an optional suffix
    // (" UNOFFICIAL" on self built copies). Only major and minor count.
    inline QueryAbi QueryAbiForVersion(const char* text)
    {
        if (!text)
            return QueryAbi::Unknown;
        auto number = [&text](std::uint32_t& out) {
            if (*text < '0' || *text > '9')
                return false;
            out = 0;
            while (*text >= '0' && *text <= '9')
            {
                if (out > 100000u)
                    return false;
                out = out * 10u + static_cast<std::uint32_t>(*text - '0');
                ++text;
            }
            return true;
        };
        std::uint32_t major = 0, minor = 0;
        if (!number(major) || *text != '.')
            return QueryAbi::Unknown;
        ++text;
        if (!number(minor))
            return QueryAbi::Unknown;
        if (*text != '\0' && *text != '.' && *text != ' ')
            return QueryAbi::Unknown;
        if (major == 6 && minor <= 7)
            return QueryAbi::Api18;
        if (major == 6 && minor == 8)
            return QueryAbi::Api20;
        return QueryAbi::Unknown;
    }

    // Slot of get_query_heap_results in reshade::api::device, the same in API
    // 18 and 20: the core's own call site is `call qword ptr [rax+0x100]` and
    // the API 20 header keeps the method in place.
    constexpr std::uint32_t kQueryResultsSlot = 32;

    // The API 20 call through the vtable. x64 has one calling convention:
    // `this` in rcx like a first argument; query_heap (one 64 bit handle) is
    // passed like a 64 bit integer and query_type (an int enum) like a 32 bit
    // one, so this prototype is the same ABI as the API 20 method.
    using QueryResultsApi20 = bool (*)(void* device, std::uint64_t heap, std::uint32_t type, std::uint32_t first, std::uint32_t count,
        void* results, std::uint32_t stride);

    inline bool CallQueryResultsApi20(void* device, std::uint64_t heap, std::uint32_t type, std::uint32_t first, std::uint32_t count,
        void* results, std::uint32_t stride)
    {
        void* const* table = *static_cast<void* const* const*>(device);
        return reinterpret_cast<QueryResultsApi20>(table[kQueryResultsSlot])(device, heap, type, first, count, results, stride);
    }
}
