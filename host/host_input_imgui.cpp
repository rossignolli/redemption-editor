// host::input::FeedImGui: the frame latched by BeginFrame handed to ImGui.
// Kept apart from host_input.cpp so that file links without ImGui (it is
// built and driven by ReplayCoreTests).
//
// Keys and mouse buttons go to ImGui as levels once per frame, exactly as
// ReShade fed them (runtime_gui.cpp, BSD-3-Clause, Patrick Mours): the editor
// adds the taps that fall inside one frame itself (src/editor_input.h,
// MissedTap / MissedClick), and feeding ImGui the down+up pair as well would
// make such a tap count twice a frame later (ImGui's input queue trickles it).

#include "host_input.h"
#include "input_rules.h"

#include <imgui.h>

#include <cfloat>

namespace host::input
{
    namespace detail
    {
        const rules::Keys& Keys();
        void ClientSize(int& w, int& h);
    }

    // Call after io.DisplaySize is set for the frame (the mouse is scaled
    // from the window's client area to it) and after BeginFrame.
    void FeedImGui(ImGuiIO& io)
    {
        namespace r = host::input::rules;
        const r::Keys& keys = detail::Keys();

        if (keys.focusChanged)
            io.AddFocusEvent(keys.focused);

        int x = 0;
        int y = 0;
        if (MousePosition(x, y))
        {
            int w = 0;
            int h = 0;
            detail::ClientSize(w, h);
            float sx = 0.0f;
            float sy = 0.0f;
            r::ScaleMouse(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h),
                          io.DisplaySize.x, io.DisplaySize.y, sx, sy);
            io.AddMousePosEvent(sx, sy);
        }
        else
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);

        for (int button = 0; button < 5; ++button)
            io.AddMouseButtonEvent(button, keys.MouseDown(button));

        if (keys.wheel != 0 || keys.wheelH != 0)
            io.AddMouseWheelEvent(r::WheelHNotches(keys.wheelH), r::WheelNotches(keys.wheel));

        const r::Mods mods = r::ModsOf(keys);
        io.AddKeyEvent(ImGuiMod_Ctrl, mods.ctrl);
        io.AddKeyEvent(ImGuiMod_Shift, mods.shift);
        io.AddKeyEvent(ImGuiMod_Alt, mods.alt);
        io.AddKeyEvent(ImGuiMod_Super, mods.super);
        for (const r::KeyMap& m : r::kKeyMap)
            io.AddKeyEvent(m.key, keys.KeyDown(m.vk));

        for (std::size_t i = 0; i < keys.charCount; ++i)
            io.AddInputCharacterUTF16(static_cast<ImWchar16>(keys.chars[i]));
    }
}
