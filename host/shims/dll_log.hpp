#pragma once

// Stands in for ReShade's source/dll_log.hpp when the host compiles ReShade's
// Vulkan implementation (external/reshade/source/vulkan/vulkan_impl_*.cpp):
// the same declarations, written to RedemptionEditor\host.log by
// host/host_log.cpp. This directory comes before external\reshade\source in
// the include path of RedemptionEditorHost.vcxproj.

#include <cassert>
#include <cinttypes>
#include <string>

namespace reshade::log
{
    enum class level
    {
        error = 1,
        warning = 2,
        info = 3,
        debug = 4,
    };

    void message(level level, const char* format, ...);
}
