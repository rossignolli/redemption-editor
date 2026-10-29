#pragma once

// The UI harness (ReplayEditorUiTests.vcxproj). replay_editor_addon.cpp is
// compiled with REDEMPTION_UI_HARNESS and includes ui_harness_fixtures.inl
// at its end, so the fixtures reach the editor's own state (its anonymous
// namespace) and play the core: they publish a snapshot through the same
// shared transport the core writes. ui_harness_main.cpp renders each
// fixture at each resolution through ui_harness_runtime (ImGui on WARP)
// and checks the texts (src/ui_layout_check.h).

#include "render_export/export_controller.h"

#include <reshade.hpp>

#include <functional>
#include <string>
#include <vector>

namespace ui_harness
{
    struct FixtureInfo
    {
        std::string name;  // "edit/marker-camera"
        std::string about; // what it stresses
    };

    // ui_harness_fixtures.inl (the editor's translation unit).
    std::vector<FixtureInfo> Fixtures();
    // Once: the transport (the harness is the core), the QA block.
    void Begin(const void* imguiTable);
    // Before every frame of fixture `index`: the editor's state and the
    // snapshot of the fixture, from a clean base.
    void Prepare(std::size_t index);
    // The editor's frame (OnOverlay), as ReShade calls it.
    void Overlay(reshade::api::effect_runtime* runtime);
    // The title font was found and baked (display_font.h).
    bool FontsReady();
    // The active language's letters no font face had (drawn as '?').
    std::size_t MissingGlyphs();
    // Between fixtures: the editor's leftovers (text boxes, keys table).
    void End();
    // A Windows font file taken as missing (display_font.h HiddenFaces).
    void HideFace(const char* file);
    // --scale: the interface scale (percent, src/ui_scale.h) the fixtures
    // start from, as RedemptionEditor.ini InterfaceScale.
    void SetScale(int percent);
    // The fixture's mouse in pixels at width x height (ImGui's, for the
    // rows' hover); false: no mouse.
    bool MousePixels(int width, int height, float& x, float& y);

    // ---------------- Interaction scripts ----------------
    // A fixture's start, then frames with keys held and the mouse; each script
    // checks the editor's state after them (the menus' keys under a resting
    // mouse, the mouse taking the selection when it moves, a click).
    struct InteractionInput
    {
        std::vector<int> keysDown; // ImGuiKey values held this frame
        bool mouseDown = false;    // the left button
        float mouseX = 0.0f;       // pixels; -FLT_MAX: no mouse
        float mouseY = 0.0f;
    };
    struct InteractionResult
    {
        std::string name;
        std::string failure; // empty: passed
    };
    // `frame` runs one frame with that input at width x height (the caller's
    // ImGui frame around Overlay).
    std::vector<InteractionResult> RunInteractions(int width, int height, const std::function<void(const InteractionInput&)>& frame);

    // The export controller of the harness (ui_harness_export_stub.cpp): the
    // real header, a stub body whose state the fixtures set.
    namespace exportstub
    {
        struct State
        {
            bool enabled = false;
            bool probed = true;
            bool busy = false;
            bool allPresets = true;
            std::string backend = "NVENC";
            std::string encoderProblem;
            std::string reshadeProblem;
            render_export::ExportStatus status;
        };
        State& Get();
    }
}
