#pragma once

// The UI harness's ImGui runtime (ReplayEditorUiRuntime.vcxproj, a DLL). It
// plays ReShade's part: the real ImGui with the D3D11 backend on a WARP device
// (no GPU, no window), the function table the editor draws through
// (reshade_overlay.hpp), an offscreen target, its pixels and PNG files. The
// ImGui of this DLL and the editor's inline wrappers of reshade_overlay.hpp
// cannot live in one module (the same symbols), as ReShade and an add-on never
// do.

#include <cstdint>

#ifdef UI_HARNESS_RUNTIME_EXPORTS
#define UI_HARNESS_RUNTIME_API extern "C" __declspec(dllexport)
#else
#define UI_HARNESS_RUNTIME_API extern "C" __declspec(dllimport)
#endif

// The D3D11 WARP device and the ImGui context; false with the reason.
UI_HARNESS_RUNTIME_API bool UiHarnessInit(char* error, int errorSize);
UI_HARNESS_RUNTIME_API void UiHarnessShutdown();
// The imgui_function_table_19222 of this ImGui (ReShade's own table code).
UI_HARNESS_RUNTIME_API const void* UiHarnessTable();
// An RGBA8 texture the editor draws with: its ImTextureID (the SRV).
UI_HARNESS_RUNTIME_API std::uint64_t UiHarnessCreateTexture(const void* rgba, int width, int height);
// The same with `count` mip levels (each half the one before).
UI_HARNESS_RUNTIME_API std::uint64_t UiHarnessCreateTextureMips(const void* const* levels, int width, int height, int count);
// One frame at width x height: the backdrop (a stand-in for the game's
// picture) on the background list, no keys; the mouse at (mouseX, mouseY)
// pixels, or none (-FLT_MAX).
UI_HARNESS_RUNTIME_API void UiHarnessNewFrame(int width, int height, double timeSeconds, float mouseX, float mouseY);
// The keys held (ImGuiKey values) and the left mouse button for the next
// UiHarnessNewFrame (the interaction scripts; a fixture frame has none). A
// key held before and missing now is released.
UI_HARNESS_RUNTIME_API void UiHarnessInput(const int* keysDown, int count, bool mouseDown);
// Ends the frame; `render`: drawn into the offscreen target and read back
// into `rgba` (width x height x 4 bytes).
UI_HARNESS_RUNTIME_API bool UiHarnessEndFrame(bool render, std::uint8_t* rgba);
// RGBA8 pixels to a PNG file (WIC).
UI_HARNESS_RUNTIME_API bool UiHarnessSavePng(const wchar_t* path, const std::uint8_t* rgba, int width, int height);
