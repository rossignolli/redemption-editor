#pragma once

// Stands in for ReShade's source/dll_resources.hpp in its D3D12
// implementation compiled into the host. The only resource asked for there
// is the mipmap compute shader of command_list::generate_mipmaps, which the
// editor never calls: no data, so device_impl logs that the mipmap pipeline
// is missing and everything else works.

#include <cstddef>

#ifndef IDR_MIPMAP_CS
#define IDR_MIPMAP_CS 1
#endif

namespace reshade::resources
{
    struct data_resource
    {
        size_t data_size;
        const void* data;
    };

    inline data_resource load_data_resource(unsigned short)
    {
        return {0, nullptr};
    }
}
