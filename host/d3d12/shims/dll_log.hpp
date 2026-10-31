#pragma once

// host/d3d12/shims comes first in the include path of ReShade's D3D12
// implementation files (external/reshade/source/d3d12/d3d12_impl_*.cpp)
// compiled into the host: host-vk's log shim plus the hr_to_string helper
// ReShade's dll_log.hpp has (d3d12_impl_command_list_immediate.cpp uses it).

#include "../../shims/dll_log.hpp"

#include <Windows.h>

#include <cstdio>
#include <string>

namespace reshade::log
{
    inline std::string hr_to_string(HRESULT hr)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(hr));
        return text;
    }
}
