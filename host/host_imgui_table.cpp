// The imgui_function_table the core draws with under the host: ReShade's own
// table code (external/reshade/source/imgui_function_table_19222.cpp,
// BSD-3-Clause OR MIT, Copyright (C) 2021 Patrick Mours) over the ImGui
// linked into the host, as the UI harness does (tools/ui_harness).

#include <imgui.h>

#ifndef RESHADE_API_LIBRARY_EXPORT // set by RedemptionEditorHost.vcxproj
#define RESHADE_API_LIBRARY_EXPORT 1
#endif
#define RESHADE_ADDON 1
#define RESHADE_GUI 1
#include "imgui_function_table_19222.cpp"

const imgui_function_table_19222* HostImGuiFunctionTable()
{
    static const imgui_function_table_19222 table = init_imgui_function_table_19222();
    return &table;
}
