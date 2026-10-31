#pragma once

// Stands in for ReShade's source/hook_manager.hpp in its D3D12
// implementation files compiled into the host. ReShade asks whether an
// export is hooked by ReShade itself to call the original; the host has no
// such hooks on dxgi/d3d12 exports it calls from there (CreateDXGIFactory1/2
// in adapter_from_device), so "not hooked" and plain calls.

#include <cstddef>

namespace reshade::hooks
{
    template <typename T>
    inline bool is_hooked(T)
    {
        return false;
    }

    template <typename T>
    inline T call(void*, T original)
    {
        return original;
    }

    // A call through the object's own vtable slot.
    template <std::size_t index, typename R, typename T, typename... Args>
    inline R call_vtable(T* object, Args... args)
    {
        using Fn = R(__stdcall*)(T*, Args...);
        return (*reinterpret_cast<Fn**>(object))[index](object, args...);
    }
}
