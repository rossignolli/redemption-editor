#include "replay_editor_addon_internal.h"

#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H

namespace
{

    // ------------------------------------------------------------------
    // Transport to ReplayProbe
    // ------------------------------------------------------------------
    HANDLE g_transportMapping = nullptr;
    SharedTransport* g_transport = nullptr;
int g_sentLightEditorState = -1;
}
    Snapshot g_snapshot{};
    bool g_hasSnapshot = false;

namespace
{

    bool EnsureTransport()
    {
        if (g_transport)
            return true;
        wchar_t mappingName[kSharedTransportNameCapacity]{};
        if (!FormatSharedTransportName(mappingName, kSharedTransportNameCapacity, GetCurrentProcessId()))
            return false;
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(sizeof(SharedTransport)), mappingName);
        if (!mapping)
            return false;
        void* view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedTransport));
        if (!view)
        {
            CloseHandle(mapping);
            return false;
        }
        g_transportMapping = mapping;
        g_transport = static_cast<SharedTransport*>(view);
        return true;
    }

    void DisconnectTransport()
    {
        if (g_transport)
        {
            UnmapViewOfFile(g_transport);
            g_transport = nullptr;
        }
        if (g_transportMapping)
        {
            CloseHandle(g_transportMapping);
            g_transportMapping = nullptr;
        }
        g_hasSnapshot = false;
        g_sentLightEditorState = -1;
    }

    bool TryConnect()
    {
        return EnsureTransport() && SharedTransportOnline(*g_transport, GetCurrentProcessId());
    }
}
    bool Send(CommandType type, const Command* payload)
    {
        if (!TryConnect())
            return false;
        Command command{};
        if (payload)
            command = *payload;
        command.structSize = sizeof(Command);
        command.version = kApiVersion;
        command.type = type;
        return SubmitSharedCommand(*g_transport, command, GetCurrentProcessId());
    }

    bool SendNamed(CommandType type, const char* name)
    {
        Command command{};
        std::snprintf(command.name, sizeof(command.name), "%s", name);
        return Send(type, &command);
    }

namespace
{

    // Menu sounds. A screen asks for one with UiSound; one plays per frame (an
    // error before a back, a back before a select, a select before a move) and
    // the core plays it (PlayUiSound): the editor never calls the engine.
    int g_pendingUiSound = -1;

    int UiSoundRank(UiSound sound)
    {
        switch (sound)
        {
        case UiSound::Error: return 4;
        case UiSound::Back: return 3;
        case UiSound::Select: return 2;
        case UiSound::NavLeft:
        case UiSound::NavRight: return 1;
        default: return 0;
        }
    }
}

    void PlayUi(UiSound sound)
    {
        if (g_pendingUiSound < 0 || UiSoundRank(sound) > UiSoundRank(static_cast<UiSound>(g_pendingUiSound)))
            g_pendingUiSound = static_cast<int>(sound);
    }

    void PlayUiStep(int step) { PlayUi(step < 0 ? UiSound::NavLeft : UiSound::NavRight); }

namespace
{

    void FlushUiSound()
    {
        if (g_pendingUiSound < 0)
            return;
        Command command{};
        command.enabled = static_cast<std::uint32_t>(g_pendingUiSound);
        command.timeMs = GetTickCount64();
        g_pendingUiSound = -1;
        Send(CommandType::PlayUiSound, &command);
    }

    void RefreshSnapshot()
    {
        if (!TryConnect())
        {
            g_hasSnapshot = false;
            return;
        }
        static Snapshot latest{};
        latest.structSize = sizeof(Snapshot);
        latest.version = kApiVersion;
        if (ReadSharedSnapshot(*g_transport, latest, GetCurrentProcessId()))
        {
            g_snapshot = latest;
            g_hasSnapshot = true;
        }
    }
}

    bool Flag(std::uint32_t flag) { return (g_snapshot.flags & flag) != 0; }

    // The clip count is known once the core has read the clips folder: its list
    // sequence moves past 0 when the first read ends, and SnapshotClipListLoading
    // is set while a read runs on its thread (813 clips take seconds). Until
    // then a count of 0 is not "no clips"; a re-read with a list already shown
    // keeps the last count.
    bool ClipListKnown()
    {
        return g_snapshot.clipListSequence != 0 && !(Flag(SnapshotClipListLoading) && g_snapshot.clipTotal == 0);
    }

    // ------------------------------------------------------------------
    // Keys
    // ------------------------------------------------------------------
    // ReShade does not forward every key to ImGui (F11 never arrives), so the
    // editor's shortcuts outside the navigation keys read ReShade's input.
    reshade::api::effect_runtime* g_runtime = nullptr;

namespace
{

    // A key pressed and released inside one frame is not lost (editor_input.h:
    // ReShade gives ImGui the key levels once a frame, and a stall such as the
    // gallery's disk read or a clip load would swallow quick presses). The levels
    // of every key, each frame.
    replay::editor::input::KeyEdges g_keyEdges;

    void BeginKeyFrame()
    {
        if (!g_runtime)
            return;
        for (unsigned vk = 8; vk < 256; ++vk)
            g_keyEdges.BeginFrame(vk, g_runtime->is_key_down(vk));
    }
}

    bool MissedTap(int vk)
    {
        return g_runtime && vk > 0 && vk < 256 && g_keyEdges.MissedTap(static_cast<unsigned>(vk), g_runtime->is_key_released(vk));
    }

    bool KeyVk(int vk) { return g_runtime && (g_runtime->is_key_pressed(vk) || MissedTap(vk)); }

namespace
{

    // The same for the left mouse button. ReShade keeps the button like a key
    // (VK_LBUTTON, set by WM_LBUTTONDOWN/UP and raw input) and feeds ImGui its
    // level once a frame, so a click inside a long frame never reaches ImGui;
    // is_mouse_button_released still says it came up.
    void BeginMouseFrame()
    {
        if (g_runtime)
            g_keyEdges.BeginFrame(VK_LBUTTON, g_runtime->is_mouse_button_down(0));
    }
}

    bool MissedClick() { return g_runtime && g_keyEdges.MissedTap(VK_LBUTTON, g_runtime->is_mouse_button_released(0)); }

    // A left click this frame: ImGui's, or one that fell inside the last frame.
    bool MouseClicked() { return ImGui::IsMouseClicked(ImGuiMouseButton_Left) || MissedClick(); }

    // The last item was clicked (InvisibleButton's own press, or a lost one over it).
    bool ItemClicked(bool pressed) { return pressed || (ImGui::IsItemHovered() && MissedClick()); }

namespace
{

    // The virtual key behind the ImGui keys the editor reads (ReShade's own
    // mapping, runtime_gui.cpp).
    int VkOf(ImGuiKey key)
    {
        if (key >= ImGuiKey_A && key <= ImGuiKey_Z)
            return 'A' + (key - ImGuiKey_A);
        switch (key)
        {
        case ImGuiKey_Enter:
        case ImGuiKey_KeypadEnter: return VK_RETURN;
        case ImGuiKey_Escape: return VK_ESCAPE;
        case ImGuiKey_Backspace: return VK_BACK;
        case ImGuiKey_Tab: return VK_TAB;
        case ImGuiKey_Space: return VK_SPACE;
        case ImGuiKey_Delete: return VK_DELETE;
        case ImGuiKey_LeftArrow: return VK_LEFT;
        case ImGuiKey_RightArrow: return VK_RIGHT;
        case ImGuiKey_UpArrow: return VK_UP;
        case ImGuiKey_DownArrow: return VK_DOWN;
        case ImGuiKey_PageUp: return VK_PRIOR;
        case ImGuiKey_PageDown: return VK_NEXT;
        case ImGuiKey_Comma: return VK_OEM_COMMA;
        case ImGuiKey_Period: return VK_OEM_PERIOD;
        default: return 0;
        }
    }
}
    bool KeyPressed(ImGuiKey key, bool repeat) { return ImGui::IsKeyPressed(key, repeat) || MissedTap(VkOf(key)); }

namespace
{

    // The ImGui key of a virtual key, for the key repeat of a held remapped key
    // (ReShade's is_key_pressed has no repeat); ImGuiKey_None for a key ReShade
    // does not give ImGui (the F keys: F11 never arrives).
    ImGuiKey ImGuiKeyOfVk(int vk)
    {
        if (vk >= 'A' && vk <= 'Z')
            return static_cast<ImGuiKey>(ImGuiKey_A + (vk - 'A'));
        if (vk >= '0' && vk <= '9')
            return static_cast<ImGuiKey>(ImGuiKey_0 + (vk - '0'));
        if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
            return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (vk - VK_NUMPAD0));
        switch (vk)
        {
        case VK_OEM_COMMA: return ImGuiKey_Comma;
        case VK_OEM_PERIOD: return ImGuiKey_Period;
        case VK_OEM_4: return ImGuiKey_LeftBracket;
        case VK_OEM_6: return ImGuiKey_RightBracket;
        case VK_OEM_1: return ImGuiKey_Semicolon;
        case VK_OEM_2: return ImGuiKey_Slash;
        case VK_OEM_5: return ImGuiKey_Backslash;
        case VK_OEM_7: return ImGuiKey_Apostrophe;
        case VK_OEM_MINUS: return ImGuiKey_Minus;
        case VK_OEM_PLUS: return ImGuiKey_Equal;
        case VK_OEM_3: return ImGuiKey_GraveAccent;
        case VK_ADD: return ImGuiKey_KeypadAdd;
        case VK_SUBTRACT: return ImGuiKey_KeypadSubtract;
        case VK_MULTIPLY: return ImGuiKey_KeypadMultiply;
        case VK_DIVIDE: return ImGuiKey_KeypadDivide;
        case VK_DECIMAL: return ImGuiKey_KeypadDecimal;
        default: return ImGuiKey_None;
        }
    }
}
    const replay::keys::Table& Keys()
    {
        static bool loaded = false;
        if (!loaded)
        {
            loaded = true;
            wchar_t folder[MAX_PATH]{};
            GetCurrentDirectoryW(MAX_PATH, folder);
            replay::keys::LoadFromIni(replay::identity::IniPathInW(folder).c_str());
        }
        return replay::keys::Current();
    }
    bool ActionPressed(Action action, bool repeat)
    {
        if (!g_runtime)
            return false;
        const replay::keys::Table& table = Keys();
        const bool ctrl = g_runtime->is_key_down(VK_CONTROL);
        const bool shift = g_runtime->is_key_down(VK_SHIFT);
        // A text box (rename, the QA note) takes the typing keys.
        const bool typing = ImGui::GetIO().WantTextInput;
        const replay::keys::Binding* keys = table.Of(action);
        for (std::size_t slot = 0; slot < replay::keys::kSlots; ++slot)
        {
            const int vk = keys[slot].vk;
            if (!vk || !replay::keys::Matches(table, action, static_cast<unsigned>(vk), ctrl, shift))
                continue;
            if (typing && !keys[slot].ctrl && !(vk >= VK_F1 && vk <= VK_F24))
                continue;
            if (KeyVk(vk))
                return true;
            const ImGuiKey key = repeat ? ImGuiKeyOfVk(vk) : ImGuiKey_None;
            if (key != ImGuiKey_None && ImGui::IsKeyPressed(key, true))
                return true;
        }
        return false;
    }

namespace
{

    // Texts built for this frame's key bar and help: a ring of buffers, as
    // KeyTip keeps a const char* (a frame builds well under 256 of them).
    const char* HoldText(std::string text)
    {
        static std::string ring[256];
        static std::size_t next = 0;
        std::string& slot = ring[next++ % 256];
        slot = std::move(text);
        return slot.c_str();
    }
}
    const char* KeyText(Action action, const char* extra, const char* separator)
    {
        const std::string text = replay::keys::Keys(Keys(), action, separator, extra);
        return HoldText(text.empty() ? std::string("-") : text);
    }

    // Two actions on one key cap: ", ." and "[ ]" by default.
    const char* PairText(Action first, Action second)
    {
        const std::string text = replay::keys::Pair(Keys(), first, second);
        return HoldText(text.empty() ? std::string("-") : text);
    }

    // "<first> / <second>" for the help ("Space / K").
    const char* JoinText(const char* first, const char* second) { return HoldText(std::string(first) + " / " + second); }

    // A text's {key_record}, {key_save}... as the [Keys] table and the fixed
    // keys have them (replay::keys::Fill), held for this frame like KeyText.
    const char* KeyFill(const char* text) { return HoldText(replay::keys::Fill(text, Keys())); }

namespace
{

    // ------------------------------------------------------------------
    // Look: the palette of the game's menus (placeholders for the art)
    // ------------------------------------------------------------------
    constexpr ImU32 kShade = IM_COL32(8, 7, 6, 190);        // veil over the game frame
    constexpr ImU32 kDark = IM_COL32(13, 12, 11, 255);      // text on the red brush
}

    // Layout units: the display height is 1080 units at every resolution, so the
    // editor keeps its proportion on any screen. The menus are drawn on a
    // 1280x720 stage centred on the screen (g_ox, g_oy); the edit screen and the
    // playback bar are anchored to the screen edges (offset 0, g_vw x g_vh units).
    float g_scale = 1.0f;
    float g_vw = 1920.0f;
    float g_vh = 1080.0f;
    float g_ox = 0.0f;
    float g_oy = 0.0f;
    // The height of the key bar at the foot of the screen (0 without one); the
    // screens anchored to the bottom edge (timelines, playback bar, notices)
    // stand above it.
    float g_footH = 0.0f;

namespace
{

    // Replay Settings' "Interface scale" (src/ui_scale.h): RedemptionEditor.ini
    // InterfaceScale in percent, read once, written when the row changes it.
    // g_scale is the display's 1080 units times it (UpdateLayoutScale), so the
    // fonts (baked once, drawn at any size), the art, the cursor, the notices
    // and every hit test follow without a rebake.
    int g_uiScalePercent = replay::editor::uiscale::kDefault;
    bool g_uiScaleRead = false;
}
    float g_uiScaleFactor = 1.0f; // the factor in use on this display (uiscale::Effective)

namespace
{

    void ReadInterfaceScale()
    {
        if (g_uiScaleRead)
            return;
        g_uiScaleRead = true;
        wchar_t folder[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, folder);
        const std::wstring ini = replay::identity::IniPathInW(folder);
        const int read = static_cast<int>(replay::identity::IniIntW(L"InterfaceScale", 100, ini.c_str()));
        g_uiScalePercent = replay::editor::uiscale::Clamp(read);
        char line[96];
        std::snprintf(line, sizeof(line), "UI_SCALE,percent=%d,read=%d,from=ini", g_uiScalePercent, read);
        reshade::log::message(reshade::log::level::info, line);
    }

    // The Replay Settings row: in use at once, kept in RedemptionEditor.ini.
    void SetInterfaceScale(int percent)
    {
        g_uiScalePercent = replay::editor::uiscale::Clamp(percent);
        g_uiScaleRead = true;
#ifndef REDEMPTION_UI_HARNESS
        wchar_t folder[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, folder);
        const std::wstring ini = replay::identity::IniPathInW(folder);
        const std::wstring value = std::to_wstring(g_uiScalePercent);
        replay::identity::IniWriteW(L"InterfaceScale", value.c_str(), ini.c_str()); // the player's file when the game folder takes no writes
#endif
        char line[96];
        std::snprintf(line, sizeof(line), "UI_SCALE,percent=%d,from=editor", g_uiScalePercent);
        reshade::log::message(reshade::log::level::info, line);
    }

    // Replay Settings' "Font" (display_font.h g_titleFace): RedemptionEditor.ini
    // Font, redemption (the default) or segoe, read once, written when the row
    // changes it. Segoe UI Bold comes from Windows; without it the editor
    // keeps Redemption (display_font::Title).
    bool g_titleFontRead = false;

    void ReadTitleFont()
    {
        if (g_titleFontRead)
            return;
        g_titleFontRead = true;
#ifndef REDEMPTION_UI_HARNESS
        wchar_t folder[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, folder);
        const std::wstring ini = replay::identity::IniPathInW(folder);
        wchar_t value[32]{};
        replay::identity::IniStringW(L"Font", L"redemption", value, 32, ini.c_str());
        display_font::g_titleFace = _wcsicmp(value, L"segoe") == 0 ? 1 : 0;
        char line[96];
        std::snprintf(line, sizeof(line), "UI_FONT,face=%s,from=ini", display_font::g_titleFace == 1 ? "segoe" : "redemption");
        reshade::log::message(reshade::log::level::info, line);
#endif
    }

    void SetTitleFont(int face)
    {
        display_font::g_titleFace = face == 1 ? 1 : 0;
        g_titleFontRead = true;
#ifndef REDEMPTION_UI_HARNESS
        wchar_t folder[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, folder);
        const std::wstring ini = replay::identity::IniPathInW(folder);
        replay::identity::IniWriteW(L"Font", display_font::g_titleFace == 1 ? L"segoe" : L"redemption", ini.c_str());
#endif
        char line[96];
        std::snprintf(line, sizeof(line), "UI_FONT,face=%s,from=editor", display_font::g_titleFace == 1 ? "segoe" : "redemption");
        reshade::log::message(reshade::log::level::info, line);
    }

    // The units of this frame: 1080 of them over the display's height at 100%.
    void UpdateLayoutScale(const ImVec2& display)
    {
        ReadInterfaceScale();
        ReadTitleFont();
        g_uiScaleFactor = replay::editor::uiscale::Effective(g_uiScalePercent, display.x, display.y);
        g_scale = display.y / 1080.0f * g_uiScaleFactor;
        g_vw = display.x / g_scale;
        g_vh = display.y / g_scale;
    }
}

    float S(float value) { return value * g_scale; }

    ImVec2 P(float x, float y) { return ImVec2(S(x + g_ox), S(y + g_oy)); }

    void SetTextInputCentered(float x, float rowY, float rowHeight)
    {
        const float itemHeight = (ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f) /
            std::max(0.001f, g_scale);
        const float y = rowY + std::max(0.0f, (rowHeight - itemHeight) * 0.5f);
        ImGui::SetCursorScreenPos(P(x, y));
    }

namespace
{

    void UseStage(bool centred)
    {
        g_ox = centred ? (g_vw - 1280.0f) * 0.5f : 0.0f;
        g_oy = centred ? (g_vh - 720.0f) * 0.5f : 0.0f;
    }

    // The 1280x720 stage against the left edge, centred vertically (the
    // gallery: a menu column like the home screen's).
    void UseLeftStage()
    {
        g_ox = 0.0f;
        g_oy = (g_vh - 720.0f) * 0.5f;
    }
}

    // The pause menu's column: dark and solid under the items, fading into the
    // game past `solidWidth`, over the whole screen height.
    void MenuColumn(ImDrawList* draw, float solidWidth)
    {
        draw->AddRectFilled(P(-g_ox, -g_oy), P(solidWidth, g_vh - g_oy), IM_COL32(10, 9, 8, 225));
        draw->AddRectFilledMultiColor(P(solidWidth, -g_oy), P(solidWidth + 280.0f, g_vh - g_oy), IM_COL32(10, 9, 8, 225),
            IM_COL32(10, 9, 8, 0), IM_COL32(10, 9, 8, 0), IM_COL32(10, 9, 8, 225));
    }

namespace
{

    // Every single line label uses the title font; the condensed capitals read
    // small, so labels under title size are set a little larger. Paragraphs use
    // the body font (Paragraph).
    constexpr float kTitleSize = 24.0f;

    float TitleFontSize(float size) { return size >= kTitleSize ? size : size * 1.18f; }

    // The pixel size of a label and of a paragraph, never under the fonts'
    // readable minimum (display_font.h kMinCapPx), so small labels stay legible
    // at 720 to 800 lines. Every text and every measure of one goes through
    // these, so the layout measures what is drawn.
    float TitlePx(float size) { return std::max(S(TitleFontSize(size)), display_font::Title().MinPx()); }
    float BodyPx(float size) { return std::max(S(size), display_font::g_body.MinPx()); }
    // The corner pieces: their size, and the least side of a panel that gets them.
    constexpr float kCornerSide = 26.0f;
    constexpr float kCornerMinSide = 160.0f;

    // One side: `origin` its outer corner, `along` and `inward` unit vectors,
    // `length` and `edge` in pixels.
    void InkSide(ImDrawList* draw, ImTextureID texture, ImVec2 origin, ImVec2 along, ImVec2 inward, float length, float edge, ImU32 color)
    {
        const float tile = edge * ui_art::Aspect(ui_art::Art::EdgeInk);
        const auto at = [&](float s, float d) { return ImVec2(origin.x + along.x * s + inward.x * d, origin.y + along.y * s + inward.y * d); };
        // u across a tile (mirrored on odd tiles), v from the ragged outside (1) to the inside (0).
        const auto uv = [&](float s, float d, int k) {
            float f = std::clamp((s - static_cast<float>(k) * tile) / tile, 0.0f, 1.0f);
            if (k & 1)
                f = 1.0f - f;
            return ImVec2(f, 1.0f - d / edge);
        };
        const auto tileOf = [&](float s) { return static_cast<int>(std::floor(s / tile + 1e-4f)); };
        float s = edge;
        while (s < length - edge - 0.01f)
        {
            const int k = tileOf(s);
            const float next = std::min(length - edge, static_cast<float>(k + 1) * tile);
            draw->AddImageQuad(ImTextureRef(texture), at(s, 0.0f), at(next, 0.0f), at(next, edge), at(s, edge), uv(s, 0.0f, k), uv(next, 0.0f, k),
                uv(next, edge, k), uv(s, edge, k), color);
            s = next;
        }
        // The two corner halves (a triangle each: a quad with its last corner twice).
        const int k0 = 0, k1 = tileOf(length - edge);
        draw->AddImageQuad(ImTextureRef(texture), at(0.0f, 0.0f), at(edge, 0.0f), at(edge, edge), at(edge, edge), uv(0.0f, 0.0f, k0),
            uv(edge, 0.0f, k0), uv(edge, edge, k0), uv(edge, edge, k0), color);
        draw->AddImageQuad(ImTextureRef(texture), at(length - edge, 0.0f), at(length, 0.0f), at(length - edge, edge), at(length - edge, edge),
            uv(length - edge, 0.0f, k1), uv(length, 0.0f, k1), uv(length - edge, edge, k1), uv(length - edge, edge, k1), color);
    }
}
    void InkPanel(ImDrawList* draw, float x0, float y0, float x1, float y1, ImU32 color, float edge, bool corners)
    {
        const ImVec2 a = P(x0, y0), b = P(x1, y1);
        UI_PROBE_BOX("panel", a, b);
        const ImTextureID texture = ui_art::Get(ui_art::Art::EdgeInk);
        const float e = S(std::min(edge, std::min(x1 - x0, y1 - y0) * 0.25f));
        if (!texture || e < 1.0f)
        {
            draw->AddRectFilled(a, b, color); // the flat look (no art)
            return;
        }
        draw->AddRectFilled(ImVec2(a.x + e, a.y + e), ImVec2(b.x - e, b.y - e), color);
        UI_PROBE_BOX("panelinner", ImVec2(a.x + e, a.y + e), ImVec2(b.x - e, b.y - e));
        const float w = b.x - a.x, h = b.y - a.y;
        InkSide(draw, texture, a, ImVec2(1.0f, 0.0f), ImVec2(0.0f, 1.0f), w, e, color);
        InkSide(draw, texture, ImVec2(a.x, b.y), ImVec2(1.0f, 0.0f), ImVec2(0.0f, -1.0f), w, e, color);
        InkSide(draw, texture, a, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f), h, e, color);
        InkSide(draw, texture, ImVec2(b.x, a.y), ImVec2(0.0f, 1.0f), ImVec2(-1.0f, 0.0f), h, e, color);
        // The gold corner pieces inside the ink band of a large panel (mirrored
        // to each corner), faint.
        const ImTextureID corner = ui_art::Get(ui_art::Art::Corner);
        if (corner && corners && edge >= kInkEdge && std::min(x1 - x0, y1 - y0) >= kCornerMinSide)
        {
            const float side = S(kCornerSide), in = e * 0.75f;
            const ImU32 tint = IM_COL32(255, 255, 255, 100);
            draw->AddImage(ImTextureRef(corner), ImVec2(a.x + in, a.y + in), ImVec2(a.x + in + side, a.y + in + side), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
            draw->AddImage(ImTextureRef(corner), ImVec2(b.x - in - side, a.y + in), ImVec2(b.x - in, a.y + in + side), ImVec2(1.0f, 0.0f), ImVec2(0.0f, 1.0f), tint);
            draw->AddImage(ImTextureRef(corner), ImVec2(a.x + in, b.y - in - side), ImVec2(a.x + in + side, b.y - in), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f), tint);
            draw->AddImage(ImTextureRef(corner), ImVec2(b.x - in - side, b.y - in - side), ImVec2(b.x - in, b.y - in), ImVec2(1.0f, 1.0f), ImVec2(0.0f, 0.0f), tint);
        }
    }

    void Text(ImDrawList* draw, float x, float y, float size, ImU32 color, const char* text)
    {
        if (display_font::Ready())
            display_font::Draw(draw, P(x, y), TitlePx(size), color, text);
        else
            draw->AddText(nullptr, S(size), P(x, y), color, text);
    }

    // A wrapped paragraph in the body font; returns its height in layout units.
    float Paragraph(ImDrawList* draw, float x, float y, float size, ImU32 color, const char* text, float wrap)
    {
        if (display_font::g_body.Ready())
            return display_font::g_body.DrawWrapped(draw, P(x, y), BodyPx(size), color, text, S(wrap)) / g_scale;
        ImGui::PushFont(nullptr, S(size));
        draw->AddText(nullptr, S(size), P(x, y), color, text, nullptr, S(wrap));
        const float height = ImGui::CalcTextSize(text, nullptr, false, S(wrap)).y;
        ImGui::PopFont();
        return height / g_scale;
    }

    // The height Paragraph would take, nothing drawn.
    float ParagraphHeight(float size, const char* text, float wrap)
    {
        if (display_font::g_body.Ready())
            return display_font::g_body.DrawWrapped(nullptr, ImVec2(0.0f, 0.0f), BodyPx(size), 0, text, S(wrap)) / g_scale;
        ImGui::PushFont(nullptr, S(size));
        const float height = ImGui::CalcTextSize(text, nullptr, false, S(wrap)).y;
        ImGui::PopFont();
        return height / g_scale;
    }

    float TextWidth(float size, const char* text)
    {
        if (display_font::Ready())
            return display_font::Width(TitlePx(size), text);
        ImGui::PushFont(nullptr, S(size));
        const float width = ImGui::CalcTextSize(text).x;
        ImGui::PopFont();
        return width;
    }

    // `text` cut to `maxWidth` layout units with "..." (ui_layout_check.h
    // Ellipsize), for a name in a fixed place.
    std::string Ellipsized(float size, const std::string& text, float maxWidth)
    {
        return replay::editor::uilayout::Ellipsize(text, maxWidth,
            [size](const std::string& candidate) { return TextWidth(size, candidate.c_str()) / g_scale; });
    }

    // A label that never runs past `maxWidth` (cut with "...").
    void TextFit(ImDrawList* draw, float x, float y, float size, ImU32 color, const char* text, float maxWidth)
    {
        Text(draw, x, y, size, color, Ellipsized(size, text, maxWidth).c_str());
    }

    // A label wrapped at `wrap` units in the title font (the F1 help's rows);
    // draw null: only measured. Its height in units.
    float TitleWrapped(ImDrawList* draw, float x, float y, float size, ImU32 color, const char* text, float wrap)
    {
        if (!display_font::Ready())
        {
            if (draw)
                Text(draw, x, y, size, color, text);
            return size;
        }
        return display_font::Title().DrawWrapped(draw, P(x, y), TitlePx(size), color, text, S(wrap)) / g_scale;
    }

    // The red brush stroke behind a selected item, like the pause menu
    // selection: the painted stroke (brush_red.png), its body's rows 8 to 120
    // over the band, the start and the frayed tail at their own aspect and the
    // body stretched between them; without the art, a band with a ragged
    // right edge.
    void Brush(ImDrawList* draw, float x, float y, float width, float height, ImU32 color, bool useArtwork)
    {
        if (useArtwork)
            if (const ImTextureID texture = ui_art::Get(ui_art::Art::BrushRed))
        {
            constexpr float kStartU = 64.0f / 1024.0f, kTailU = 920.0f / 1024.0f;
            const float k = height / 112.0f; // layout units per texel row
            const float top = y - 8.0f * k, bottom = y + 120.0f * k;
            const float bodyStart = std::min(x + 64.0f * k, x + width * 0.5f);
            const float tailStart = std::max(bodyStart, x + width - 40.0f * k);
            draw->AddImage(ImTextureRef(texture), P(x, top), P(bodyStart, bottom), ImVec2(0.0f, 0.0f), ImVec2(kStartU, 1.0f), color);
            draw->AddImage(ImTextureRef(texture), P(bodyStart, top), P(tailStart, bottom), ImVec2(kStartU, 0.0f), ImVec2(kTailU, 1.0f), color);
            draw->AddImage(ImTextureRef(texture), P(tailStart, top), P(tailStart + 104.0f * k, bottom), ImVec2(kTailU, 0.0f), ImVec2(1.0f, 1.0f), color);
            UI_PROBE_BOX("brush", P(x, y), P(x + width + height * 0.3f, y + height)); // the painted tail, as far as the flat brush's points
            return;
        }
        const float jag[] = {0.0f, 0.35f, -0.25f, 0.5f, -0.1f, 0.3f, 0.0f};
        const int steps = static_cast<int>(sizeof(jag) / sizeof(jag[0])) - 1;
        std::vector<ImVec2> points;
        points.push_back(P(x, y));
        for (int n = 0; n <= steps; ++n)
        {
            const float t = static_cast<float>(n) / steps;
            points.push_back(P(x + width + jag[n] * height * 0.6f, y + t * height));
        }
        points.push_back(P(x, y + height));
        draw->AddConvexPolyFilled(points.data(), static_cast<int>(points.size()), color);
    }

namespace
{

    // ---------------- The menu rows' boxes ----------------
    // Every list row in its own box, as the pause menu's settings list: a dark
    // grainy body with rough edges, a thin gap to the next row; the selected
    // row outlined in red ink, the row under the mouse in grey ink. Painted:
    // row_box.png and row_ink.png in three slices (ui_art.h); flat: a filled
    // rectangle and a plain outline.
    enum class RowLook
    {
        Rest,
        Hovered,       // under the mouse, not the selected row
        Selected,      // the selected (focused) row
        SelectedGreyed // the focus on a greyed row (it must show)
    };
    constexpr ImU32 kRowBody = IM_COL32(40, 38, 35, 232);
    constexpr ImU32 kRowBodySelected = IM_COL32(48, 45, 41, 240);
    constexpr ImU32 kRowInkGreyed = IM_COL32(179, 22, 15, 150);
    constexpr float kRowInkOut = 3.0f; // the ink outline's art past the box on every side (its strokes on the box's edge)

    // An art in three slices over (x0, y0)-(x1, y1) layout units: its first and
    // last `capTexels` columns at their aspect, the rest stretched between.
    void ThreeSlice(ImDrawList* draw, ui_art::Art art, float capTexels, float x0, float y0, float x1, float y1, ImU32 tint)
    {
        const ui_art::Texture& t = ui_art::Info(art);
        if (!t.id || t.width <= 0 || t.height <= 0)
            return;
        const ImTextureRef texture(static_cast<ImTextureID>(t.id));
        const float cap = std::min((y1 - y0) * capTexels / static_cast<float>(t.height), (x1 - x0) * 0.5f);
        const float u = capTexels / static_cast<float>(t.width);
        draw->AddImage(texture, P(x0, y0), P(x0 + cap, y1), ImVec2(0.0f, 0.0f), ImVec2(u, 1.0f), tint);
        draw->AddImage(texture, P(x0 + cap, y0), P(x1 - cap, y1), ImVec2(u, 0.0f), ImVec2(1.0f - u, 1.0f), tint);
        draw->AddImage(texture, P(x1 - cap, y0), P(x1, y1), ImVec2(1.0f - u, 0.0f), ImVec2(1.0f, 1.0f), tint);
    }

#ifdef REDEMPTION_UI_HARNESS
    // The looks the rows took this frame and their boxes in screen units, in
    // drawing order (the interaction scripts count them and aim the mouse).
    int g_rowLooks[4] = {};
    std::vector<ImVec4> g_rowBoxes;
#endif

    void RowBox(ImDrawList* draw, float x0, float y0, float x1, float y1, RowLook look,
        int theme = 1, ImU32 activeInk = kRed, ImU32 hoverInk = kRowInkGrey)
    {
        UI_PROBE_BOX("row", P(x0, y0), P(x1, y1));
#ifdef REDEMPTION_UI_HARNESS
        ++g_rowLooks[static_cast<int>(look)];
        g_rowBoxes.push_back(ImVec4(x0 + g_ox, y0 + g_oy, x1 + g_ox, y1 + g_oy));
#endif
        const bool selected = look == RowLook::Selected || look == RowLook::SelectedGreyed;
        if (theme == 0 || theme == 2 || theme == 3 || theme == 4)
        {
            const bool blue = theme == 0 || theme == 4;
            const ImU32 body = theme == 0
                ? (selected ? IM_COL32(18, 50, 68, 244) : IM_COL32(12, 25, 36, 228))
                : theme == 2
                    ? (selected ? IM_COL32(61, 27, 25, 244) : IM_COL32(34, 20, 20, 228))
                    : theme == 3
                        ? (selected ? IM_COL32(52, 30, 69, 244) : IM_COL32(27, 17, 37, 228))
                        : (selected ? IM_COL32(18, 39, 68, 244) : IM_COL32(10, 22, 39, 228));
            draw->AddRectFilled(P(x0, y0), P(x1, y1), body, S(2.0f));
            const ImU32 outline = selected ? activeInk : look == RowLook::Hovered ? hoverInk :
                (blue ? (theme == 0 ? IM_COL32(66, 142, 174, 70) : IM_COL32(72, 119, 177, 90)) :
                    theme == 3 ? IM_COL32(126, 75, 158, 90) : IM_COL32(159, 77, 70, 70));
            draw->AddRect(P(x0, y0), P(x1, y1), outline, S(2.0f), 0,
                S(selected ? 1.8f : look == RowLook::Hovered ? 1.2f : 0.7f));
            return;
        }
        const ImU32 body = selected ? kRowBodySelected : kRowBody;
        if (ui_art::Get(ui_art::Art::RowBox))
            ThreeSlice(draw, ui_art::Art::RowBox, 32.0f, x0, y0, x1, y1, body);
        else
            draw->AddRectFilled(P(x0, y0), P(x1, y1), body);
        if (look == RowLook::Rest)
            return;
        const ImU32 ink = look == RowLook::Selected ? activeInk : look == RowLook::SelectedGreyed ? kRowInkGreyed : hoverInk;
        if (ui_art::Get(ui_art::Art::RowInk))
            ThreeSlice(draw, ui_art::Art::RowInk, 48.0f, x0 - kRowInkOut, y0 - kRowInkOut, x1 + kRowInkOut, y1 + kRowInkOut, ink);
        else
            draw->AddRect(P(x0 - 1.0f, y0 - 1.0f), P(x1 + 1.0f, y1 + 1.0f), ink, 0.0f, 0, S(2.0f));
    }

    // The room a banner's mark takes before its title of `height` units.
    float TitleMarkRoom(float height, ui_art::Art mark)
    {
        return mark == ui_art::Art::ToolLantern || ui_art::Get(mark) ? height * 0.74f + 12.0f : 0.0f;
    }

    // A small line-art lantern, drawn in layout units so it stays crisp at the
    // menu icon and title sizes. The selected menu row uses the same mark in
    // white; at rest it is warm brass like the supplied lantern reference.
    void DrawLanternMark(ImDrawList* draw, float cx, float cy, float size, ImU32 color)
    {
        const auto p = [&](float x, float y) { return P(cx + x * size, cy + y * size); };
        const float stroke = S(std::max(0.9f, size * 0.052f));
        const auto line = [&](float x0, float y0, float x1, float y1, float weight = 1.0f) {
            draw->AddLine(p(x0, y0), p(x1, y1), color, stroke * weight);
        };
        const auto path = [&](const ImVec2* points, int count, bool closed = false) {
            ImVec2 screen[10]{};
            const int used = std::min(count, static_cast<int>(std::size(screen)));
            for (int i = 0; i < used; ++i)
                screen[i] = p(points[i].x, points[i].y);
            draw->AddPolyline(screen, used, color, closed ? ImDrawFlags_Closed : ImDrawFlags_None, stroke);
        };

        const ImVec2 handle[] = {{-0.21f, -0.29f}, {-0.20f, -0.43f}, {-0.12f, -0.50f}, {0.0f, -0.53f},
            {0.12f, -0.50f}, {0.20f, -0.43f}, {0.21f, -0.29f}};
        path(handle, static_cast<int>(std::size(handle)));
        line(-0.10f, -0.44f, 0.10f, -0.44f, 1.2f);
        line(0.0f, -0.53f, 0.0f, -0.59f, 1.2f);
        draw->AddCircle(p(0.0f, -0.61f), S(size * 0.025f), color, 12, stroke * 0.8f);

        const ImVec2 cap[] = {{-0.25f, -0.30f}, {0.25f, -0.30f}, {0.22f, -0.23f}, {0.15f, -0.20f},
            {-0.15f, -0.20f}, {-0.22f, -0.23f}};
        path(cap, static_cast<int>(std::size(cap)), true);
        line(-0.28f, -0.18f, 0.28f, -0.18f, 1.2f);

        const ImVec2 cage[] = {{-0.25f, -0.17f}, {-0.31f, -0.06f}, {-0.31f, 0.20f}, {-0.24f, 0.30f},
            {0.24f, 0.30f}, {0.31f, 0.20f}, {0.31f, -0.06f}, {0.25f, -0.17f}};
        path(cage, static_cast<int>(std::size(cage)), true);
        line(-0.25f, -0.14f, -0.25f, 0.23f, 0.8f);
        line(0.25f, -0.14f, 0.25f, 0.23f, 0.8f);
        line(-0.25f, -0.12f, 0.25f, -0.12f, 0.9f);
        line(-0.25f, 0.21f, 0.25f, 0.21f, 0.9f);
        const ImVec2 flame[] = {{0.0f, -0.06f}, {-0.07f, 0.04f}, {-0.04f, 0.12f}, {0.0f, 0.17f},
            {0.05f, 0.10f}, {0.07f, 0.03f}, {0.0f, -0.06f}};
        path(flame, static_cast<int>(std::size(flame)));

        const ImVec2 base[] = {{-0.25f, 0.30f}, {-0.20f, 0.37f}, {0.20f, 0.37f}, {0.25f, 0.30f}};
        path(base, static_cast<int>(std::size(base)));
        line(-0.22f, 0.39f, 0.22f, 0.39f, 1.2f);
    }
}

ImU32 WithAlpha(ImU32 color, float alpha)
{
    const std::uint32_t a = static_cast<std::uint32_t>(std::clamp(((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha, 0.0f, 255.0f));
    return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

    // A screen's title banner: the red brush from (x, y), `height` tall, at
    // least `minWidth` wide and `extra` wider than the title; the title at
    // (textX, textY), `size` units. Painted, the Cylinder Reel stands at the
    // title's start and the title and the brush move right by its room.
    void TitleBanner(ImDrawList* draw, float x, float y, float minWidth, float extra, float height, float textX, float textY, float size,
        const char* title, ImU32 accent, ui_art::Art mark)
    {
        const float room = TitleMarkRoom(height, mark);
        Brush(draw, x, y, std::max(minWidth, TextWidth(size, title) / g_scale + extra + room), height, accent, accent == kRed);
        if (mark == ui_art::Art::ToolLantern)
        {
            const float side = height * 0.74f, cy = y + height * 0.5f;
            DrawLanternMark(draw, textX - 2.0f + side * 0.5f, cy, side, kGold);
        }
        else if (const ImTextureID reel = ui_art::Get(mark))
        {
            const float side = height * 0.74f, cy = y + height * 0.5f;
            draw->AddImage(ImTextureRef(reel), P(textX - 2.0f, cy - side * 0.5f), P(textX - 2.0f + side, cy + side * 0.5f), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), kText);
        }
        Text(draw, textX + room, textY, size, kText, title);
    }

    // The game menus' divider: a hairline that fades out at both ends, with a
    // small diamond between two dots at its centre. Painted, the worn gold
    // hairline (divider.png: its line from column 85 to 938 of 1024, on row
    // 24 of 48) over the same width.
    void OrnamentDivider(ImDrawList* draw, float x, float y, float width, ImU32 accent)
    {
        if (accent == kGold)
            if (const ImTextureID texture = ui_art::Get(ui_art::Art::Divider))
        {
            const float k = width / 853.0f; // layout units per texel
            const float left = x - 85.0f * k, top = y + 0.5f - 24.0f * k;
            draw->AddImage(ImTextureRef(texture), P(left, top), P(left + 1024.0f * k, top + 48.0f * k), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                IM_COL32(255, 255, 255, 200));
            return;
        }
        const float mid = x + width * 0.5f;
        const ImU32 solid = WithAlpha(accent, 0.59f);
        const ImU32 clear = WithAlpha(accent, 0);
        draw->AddRectFilledMultiColor(P(x, y), P(mid - 14.0f, y + 1.0f), clear, solid, solid, clear);
        draw->AddRectFilledMultiColor(P(mid + 14.0f, y), P(x + width, y + 1.0f), solid, clear, clear, solid);
        draw->AddQuadFilled(P(mid, y - 4.0f), P(mid + 4.0f, y + 0.5f), P(mid, y + 5.0f), P(mid - 4.0f, y + 0.5f), solid);
        draw->AddCircleFilled(P(mid - 9.0f, y + 0.5f), S(1.4f), solid);
        draw->AddCircleFilled(P(mid + 9.0f, y + 0.5f), S(1.4f), solid);
    }

    // A section title as the game menus set it: small capitals, centred,
    // between two short hairlines.
    void SectionTitle(ImDrawList* draw, float x, float y, float width, const char* title, ImU32 accent)
    {
        const float titleWidth = TextWidth(14.0f, title) / g_scale;
        const float mid = x + width * 0.5f;
        Text(draw, mid - titleWidth * 0.5f, y, 14.0f, accent == kGold ? kMuted : accent, title);
        const ImU32 line = accent == kGold ? IM_COL32(236, 230, 218, 60) : WithAlpha(accent, 0.39f);
        draw->AddLine(P(mid - titleWidth * 0.5f - 46.0f, y + 8.0f), P(mid - titleWidth * 0.5f - 10.0f, y + 8.0f), line, S(1.0f));
        draw->AddLine(P(mid + titleWidth * 0.5f + 10.0f, y + 8.0f), P(mid + titleWidth * 0.5f + 46.0f, y + 8.0f), line, S(1.0f));
    }

    // One key hint at the bottom: a key cap and its label; returns the width.
    // `on` false draws it muted (the key does nothing right now).
    float KeyHint(ImDrawList* draw, float x, float y, const char* key, const char* label, bool on = true)
    {
        const ImU32 color = on ? kText : kMuted;
        const float keyWidth = TextWidth(13.0f, key) / g_scale + 12.0f;
        draw->AddRect(P(x, y), P(x + keyWidth, y + 20.0f), color, S(2.0f), 0, S(1.2f));
        Text(draw, x + 6.0f, y + 2.0f, 13.0f, color, key);
        Text(draw, x + keyWidth + 7.0f, y + 1.0f, 15.0f, color, label);
        return keyWidth + 7.0f + TextWidth(15.0f, label) / g_scale + 22.0f;
    }

    // A label's line height in layout units (its pixel size, the readable
    // minimum included).
    float LabelHeight(float size) { return TitlePx(size) / g_scale; }
    void RowTextFit(ImDrawList* draw, float x, float y, float size, ImU32 color, const char* text, float maxWidth, bool shrink)
    {
        float shown = size;
        if (shrink || replay::i18n::Active())
            shown = replay::editor::uilayout::ShrinkToFit(size, size * 0.8f, maxWidth, [&](float at) { return TextWidth(at, text) / g_scale; });
        TextFit(draw, x, y + (LabelHeight(size) - LabelHeight(shown)) * 0.5f, shown, color, text, maxWidth);
    }

    // A label that fits `maxWidth` is drawn as TextFit draws it; in another
    // language than English a longer one wraps onto more lines instead of
    // being cut (English keeps the release layout, a long name cut with
    // "..."). Draw null: only measured. Its height in layout units.
    float LabelFit(ImDrawList* draw, float x, float y, float size, ImU32 color, const char* text, float maxWidth)
    {
        if (!display_font::Ready() || !replay::i18n::Active() || TextWidth(size, text) / g_scale <= maxWidth)
        {
            if (draw)
                TextFit(draw, x, y, size, color, text, maxWidth);
            return LabelHeight(size);
        }
        return TitleWrapped(draw, x, y, size, color, text, maxWidth);
    }
    bool LoadingCue(ImDrawList* draw, float x, float y, float size, ImU32 tint)
    {
        const ImTextureID texture = ui_art::Get(ui_art::Art::Loading);
        if (!texture)
            return false;
        const int frame = static_cast<int>((GetTickCount64() / 50) % ui_art::kLoadingFrames);
        const float u0 = static_cast<float>(frame) / ui_art::kLoadingFrames, u1 = static_cast<float>(frame + 1) / ui_art::kLoadingFrames;
        UI_PROBE_BOX("cue", P(x - size * 0.5f, y - size * 0.5f), P(x + size * 0.5f, y + size * 0.5f)); // the harness's cue check
        draw->AddImage(ImTextureRef(texture), P(x - size * 0.5f, y - size * 0.5f), P(x + size * 0.5f, y + size * 0.5f), ImVec2(u0, 0.0f),
            ImVec2(u1, 1.0f), tint);
        return true;
    }

    // The busy cue after a label drawn at (x, y) in `textSize` units, when it
    // fits before `maxRight`.
    void LoadingAfter(ImDrawList* draw, float x, float y, float textSize, const char* text, float maxRight)
    {
        const float size = std::max(16.0f, LabelHeight(textSize) * 0.95f);
        const float cx = x + TextWidth(textSize, text) / g_scale + 8.0f + size * 0.5f;
        if (cx + size * 0.5f <= maxRight)
            LoadingCue(draw, cx, y + LabelHeight(textSize) * 0.5f, size);
    }

namespace
{

    // The description panel beside a menu (Home, Replay Settings): a heading, a
    // paragraph, then a detail line or a note. The content is kDescWidth wide,
    // kPanelPad from the panel's edges; the panel grows to its measured content.
    constexpr float kDescWidth = 392.0f;
}

    // `busy`: the detail (or the note) says something is being read; the turning
    // cylinder follows it when it fits on its line.
    void DescriptionPanel(ImDrawList* draw, float x, float top, float minHeight, const char* heading, const char* body,
        const char* detail, const char* note, bool busy = false)
    {
        const float cx = x + kPanelPad;
        float y = top + 72.0f + ParagraphHeight(17.0f, body, kDescWidth);
        const bool hasDetail = detail && detail[0];
        const bool hasNote = note && note[0];
        const float detailY = std::max(top + 168.0f, y + 8.0f);
        if (hasDetail)
            y = detailY + LabelFit(nullptr, cx, detailY, 14.0f, 0, detail, kDescWidth);
        const float noteY = std::max(top + 196.0f, y + 8.0f);
        if (hasNote)
            y = noteY + ParagraphHeight(15.0f, note, kDescWidth);
        const float bottom = std::max(top + minHeight, y + kPanelPadBottom);
        InkPanel(draw, x, top, cx + kDescWidth + kPanelPad, bottom, kPanel);
        RowTextFit(draw, cx, top + 18.0f, 26.0f, kText, heading, kDescWidth);
        OrnamentDivider(draw, cx, top + 58.0f, kDescWidth);
        Paragraph(draw, cx, top + 72.0f, 17.0f, kText, body, kDescWidth);
        if (hasDetail)
            LabelFit(draw, cx, detailY, 14.0f, kMuted, detail, kDescWidth);
        if (hasNote)
            Paragraph(draw, cx, noteY, 15.0f, kMuted, note, kDescWidth);
        if (busy && hasDetail)
            LoadingAfter(draw, cx, detailY, 14.0f, detail, cx + kDescWidth);
        else if (busy && hasNote && display_font::g_body.Ready())
        {
            // The note in the body font, on one line.
            const float line = display_font::g_body.LineHeight(BodyPx(15.0f)) / g_scale;
            const float size = std::max(16.0f, line * 0.95f);
            const float cueX = cx + display_font::g_body.Width(BodyPx(15.0f), note) / g_scale + 8.0f + size * 0.5f;
            if (cueX + size * 0.5f <= cx + kDescWidth)
                LoadingCue(draw, cueX, noteY + line * 0.5f, size);
        }
    }

    bool TimelineArtReady()
    {
        using ui_art::Art;
        for (Art art : {Art::BarTorn, Art::BarRed, Art::IconKey, Art::IconKeySelected, Art::IconKeyTick, Art::IconCut, Art::IconShake, Art::IconPlayhead})
            if (!ui_art::Get(art))
                return false;
        return true;
    }

    // A torn strip from x0 to x1 (layout units), y0 to y1, tiled at its
    // height with every other tile mirrored; the tiles counted from `origin`,
    // so the played part's edges line up with the track's.
    void TornStrip(ImDrawList* draw, ui_art::Art art, float origin, float x0, float x1, float y0, float y1, ImU32 tint)
    {
        const ImTextureID texture = ui_art::Get(art);
        if (!texture || x1 <= x0)
            return;
        const float top = P(0.0f, y0).y, bottom = P(0.0f, y1).y;
        const float base = P(origin, 0.0f).x;
        const float tile = (bottom - top) * ui_art::Aspect(art);
        float s = P(x0, 0.0f).x - base;
        const float end = P(x1, 0.0f).x - base;
        while (s < end - 0.01f)
        {
            const int k = static_cast<int>(std::floor(s / tile + 1e-4f));
            const float next = std::min(end, static_cast<float>(k + 1) * tile);
            float u0 = (s - static_cast<float>(k) * tile) / tile, u1 = (next - static_cast<float>(k) * tile) / tile;
            if (k & 1)
            {
                u0 = 1.0f - u0;
                u1 = 1.0f - u1;
            }
            draw->AddImage(ImTextureRef(texture), ImVec2(base + s, top), ImVec2(base + next, bottom), ImVec2(u0, 0.0f), ImVec2(u1, 1.0f), tint);
            s = next;
        }
    }

    // An icon centred on (x, y), `size` layout units square.
    void Icon(ImDrawList* draw, ui_art::Art art, float x, float y, float size, ImU32 tint)
    {
        if (const ImTextureID texture = ui_art::Get(art))
            draw->AddImage(ImTextureRef(texture), P(x - size * 0.5f, y - size * 0.5f), P(x + size * 0.5f, y + size * 0.5f), ImVec2(0.0f, 0.0f),
                ImVec2(1.0f, 1.0f), tint);
    }

    // A timeline mark's icon with a thin dark outline under it: its silhouette in
    // black, nudged a unit around it, so the gold reads on the red played part.
    void OutlinedIcon(ImDrawList* draw, ui_art::Art art, float x, float y, float size, ImU32 tint)
    {
        const ImTextureID texture = ui_art::Get(art);
        if (!texture)
            return;
        constexpr float kOutline = 1.0f;
        static const float kAround[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0.7f, 0.7f}, {-0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}};
        const float o = std::max(kOutline, 1.0f / g_scale); // a pixel at least
        for (const auto& d : kAround)
            draw->AddImage(ImTextureRef(texture), P(x + d[0] * o - size * 0.5f, y + d[1] * o - size * 0.5f),
                P(x + d[0] * o + size * 0.5f, y + d[1] * o + size * 0.5f), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(0, 0, 0, 150));
        Icon(draw, art, x, y, size, tint);
    }
    void PlayheadIcon(ImDrawList* draw, float x, float tipY, float height)
    {
        const ImTextureID texture = ui_art::Get(ui_art::Art::IconPlayhead);
        if (!texture)
            return;
        const float box = height * 136.0f / kPlayheadRows;
        const float left = x - box * 67.5f / 136.0f, top = tipY - box * 135.0f / 136.0f;
        draw->AddImage(ImTextureRef(texture), P(left, top), P(left + box, top + box), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255));
    }

namespace
{

    // The seam between two clips of a project on the painted bar: the gold
    // stitched post (rows 8 to 95 of 104, its stroke on column 52) from top to
    // bottom at x. False without the painted timeline, or when the clips are
    // `spacing` units apart on average, closer than kSeamMinSpacing (the
    // caller draws its line).
    constexpr float kSeamMinSpacing = 24.0f;
}
    bool SeamMark(ImDrawList* draw, float x, float top, float bottom, float spacing)
    {
        if (!TimelineArtReady() || !ui_art::Get(ui_art::Art::IconSeam) || spacing < kSeamMinSpacing)
            return false;
        const float k = (bottom - top) / 88.0f;
        OutlinedIcon(draw, ui_art::Art::IconSeam, x, top + 44.0f * k, 104.0f * k, kIconTint);
        return true;
    }
    void MenuIcon(ImDrawList* draw, ui_art::Art art, float x, float y, float size, bool onBrush, bool enabled)
    {
        if (art == ui_art::Art::Count)
            return;
        if (art == ui_art::Art::ToolLantern || art == ui_art::Art::ToolLanternWhite)
        {
            const ImU32 color = !enabled ? IM_COL32(233, 178, 64, 80) : onBrush ? kText : kGold;
            DrawLanternMark(draw, x, y, size, color);
            return;
        }
        const ui_art::Art shown = onBrush && enabled && ui_art::Get(ui_art::IconWhite(art)) ? ui_art::IconWhite(art) : art;
        Icon(draw, shown, x, y, size, enabled ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 80));
    }

    // True when one of `arts` is loaded: a list with icons sets every label
    // past the icons' column, so the labels keep one edge.
    bool AnyArt(std::initializer_list<ui_art::Art> arts)
    {
        for (ui_art::Art art : arts)
            if (art != ui_art::Art::Count && ui_art::Get(art))
                return true;
        return false;
    }

    // A camera marker on the painted timeline: the clip's start and end
    // stay gold brackets; a key inside a directed shot (an Anchor) the
    // pin, a key a cut goes into the sheriff star, any other key the key
    // icon; the selected one the key in its ring, larger, with its number
    // over the playhead's crown, or beside it at (numberX, numberY) when the
    // crown stands over the marker (numberX >= 0).
    void DrawPaintedMark(ImDrawList* draw, std::uint32_t n, float x, float my, float y, bool selected, bool hot, float numberX = -1.0f,
        float numberY = 0.0f)
    {
        using ui_art::Art;
        const CameraKeyframe& key = g_snapshot.cameraKeyframes[n];
        const std::uint32_t keyCount = g_snapshot.cameraKeyframeCount;
        const bool bound = keyCount >= 2 && (n == 0 || n + 1 == keyCount);
        const ImU32 tint = selected || hot ? IM_COL32(255, 255, 255, 255) : kIconTint;
        const float barTop = y + kTrackTop;
        float numberAt = my - 15.0f;
        if (bound && ui_art::Get(n == 0 ? Art::IconTrimIn : Art::IconTrimOut))
        {
            // The bent nail brackets: the stroke (column 44.5 of [, 58.5 of ]) on
            // the marker, rows 6 to 91 over the bracket's height.
            const float top = barTop - 4.0f, bottom = y + kTrackBottom + 3.0f;
            const float k = (bottom - top) / 86.0f;
            const float cx = x + (52.0f - (n == 0 ? 44.5f : 58.5f)) * k;
            OutlinedIcon(draw, n == 0 ? Art::IconTrimIn : Art::IconTrimOut, cx, top + 46.0f * k, 104.0f * k, tint);
            if (selected)
                draw->AddCircle(P(x, my), S(14.0f), kText, 0, S(1.5f));
        }
        else if (bound)
        {
            const float dir = n == 0 ? 1.0f : -1.0f;
            const ImU32 gold = selected ? IM_COL32(255, 229, 142, 255) : hot ? IM_COL32(245, 205, 110, 255) : kGold;
            const float top = barTop - 4.0f, bottom = y + kTrackBottom + 3.0f;
            // The same thin dark outline as the icons (OutlinedIcon), under the bracket.
            const float o = std::max(1.0f, 1.0f / g_scale);
            draw->AddLine(P(x, top - o), P(x, bottom + o), IM_COL32(0, 0, 0, 150), S((selected ? 4.0f : 3.0f) + 2.0f * o));
            draw->AddLine(P(x - dir * o, top), P(x + dir * (6.0f + o), top), IM_COL32(0, 0, 0, 150), S(3.0f + 2.0f * o));
            draw->AddLine(P(x - dir * o, bottom), P(x + dir * (6.0f + o), bottom), IM_COL32(0, 0, 0, 150), S(3.0f + 2.0f * o));
            draw->AddLine(P(x, top), P(x, bottom), gold, S(selected ? 4.0f : 3.0f));
            draw->AddLine(P(x, top), P(x + dir * 6.0f, top), gold, S(3.0f));
            draw->AddLine(P(x, bottom), P(x + dir * 6.0f, bottom), gold, S(3.0f));
            if (selected)
                draw->AddCircle(P(x, my), S(14.0f), kText, 0, S(1.5f));
        }
        else
        {
            const bool cut = n > 0 && g_snapshot.cameraKeyframes[n - 1].blend == Blend::None &&
                g_snapshot.cameraKeyframes[n - 1].markerType != MarkerType::Anchor;
            const Art art = key.markerType == MarkerType::Anchor ? Art::IconKeyTick : cut ? Art::IconCut : Art::IconKey;
            if (selected && my < barTop)
                draw->AddLine(P(x, my + 8.0f), P(x, barTop), IM_COL32(255, 229, 142, 200), S(1.5f));
            const float size = (art == Art::IconKeyTick ? 22.0f : 20.0f) + (hot ? 3.0f : 0.0f);
            if (selected && art == Art::IconKey)
                OutlinedIcon(draw, Art::IconKeySelected, x, my, 30.0f, tint);
            else if (selected && art == Art::IconCut && ui_art::Get(Art::IconCutSelected))
                OutlinedIcon(draw, Art::IconCutSelected, x, my, 30.0f, tint); // white on its red halo
            else
            {
                OutlinedIcon(draw, art, x, my, selected ? size + 4.0f : size, tint);
                if (selected)
                    draw->AddCircle(P(x, my), S(15.0f), kText, 0, S(1.5f));
            }
        }
        if (selected)
        {
            // Over the playhead's crown (the playhead stands on the selected marker).
            numberAt = std::min(numberAt - 24.0f, y - 44.0f);
            char number[8]{};
            std::snprintf(number, sizeof(number), "%u", n + 1);
            if (numberX >= 0.0f)
                Text(draw, numberX, numberY, 14.0f, kText, number);
            else
                Text(draw, x - TextWidth(14.0f, number) / g_scale * 0.5f, numberAt, 14.0f, kText, number);
        }
    }

    // The width KeyHint takes, nothing drawn.
    float KeyHintWidth(const char* key, const char* label)
    {
        return TextWidth(13.0f, key) / g_scale + 12.0f + 7.0f + TextWidth(15.0f, label) / g_scale + 22.0f;
    }
    Screen g_screen = Screen::Closed;
    Screen g_lightReturnScreen = Screen::Closed;
    std::uint32_t g_lightFocusMarkerId = 0;
    std::uint32_t g_lightLastFocusMarkerId = 0;
    std::uint32_t g_lightCopySourceMarkerId = 0;
    int g_lightSelected = 0;
    std::uint32_t g_lightSelectedId = 0;
    int g_lightDraftIndex = -1;
    bool g_lightDraftDirty = false;
    bool g_lightDraftAttachSyncPending = false;
    std::uint32_t g_lightDraftAttachSyncId = 0;
    std::uint32_t g_lightDraftAttachSyncTarget = 0;
    std::uint32_t g_lightDraftAttachSyncFrames = 0;
    bool g_lightRenameFocus = false;
    AuthoredLight g_lightDraft{};
    int g_lightNumericEditAxis = -1;
    float g_lightNumericEditValue = 0.0f;
    bool g_lightNumericEditFocus = false;
    bool g_lightAdvancedOpen = false;
    bool g_lightSplineOpen = false;
    bool g_lightSplineDefaultsOpen = false;
    bool g_lightSettingsMenuOpen = false;
    bool g_lightRigMenuOpen = false;
    bool g_lightPreviewSolo = false;
    bool g_lightTargetPickerOpen = false;
    int g_lightTargetPickerMode = 0; // 0: look at, 1: attachment
    char g_lightTargetSearch[64]{};
    bool g_lightTargetFavoritesRead = false;
    std::vector<std::string> g_lightTargetFavorites;
    char g_lightRigName[48] = "My Light Rig";
    char g_lightRigStatus[128]{};
    ULONGLONG g_lightRigSelectionPendingAt = 0;
    bool g_reopenLightsWithF2 = false;
    std::uint32_t g_lightReopenMarkerId = 0;
    int g_lightMenuPreset = 5; // Original is the legacy appearance and remains the default.
    int g_lightSplineSelectedRow = 0;
    replay::editor::SplineTuning g_lightSplineDefaultsDraft = replay::editor::DefaultSplineTuning();
    bool g_lightSplineDefaultsDirty = false;
    int g_lightListFirst = 0;
    bool g_lightMenuOpacityRead = false;
    int g_lightMenuOpacity = 50;
    bool g_lightGizmosVisibleUi = false;
    bool g_lightPreviewVisible = true;
    float g_lightPreviewWidth = 460.0f;
    float g_lightPreviewHeight = 360.0f;
    float g_lightPreviewX = -1.0f;
    float g_lightPreviewY = -1.0f;
    bool g_lightPreviewMoving = false;

    struct LightRigEntry
    {
        AuthoredLight light{};
        char attachmentLabel[kTargetLabelCapacity]{};
        char lookAtLabel[kTargetLabelCapacity]{};
    };

    struct LightRigGroup
    {
        std::uint32_t id = 0;
        char savedName[48]{};
        char displayName[48]{};
        std::vector<std::uint32_t> memberIds;
    };

    struct LightListRow
    {
        std::uint32_t representativeIndex = 0;
        std::uint32_t rigGroupId = 0;
        std::vector<std::uint32_t> sourceIndices;
    };

    struct LightRigEditDraft
    {
        struct Member
        {
            std::uint32_t sourceIndex = 0;
            AuthoredLight light{};
        };
        bool valid = false;
        std::uint32_t groupId = 0;
        std::uint32_t markerId = 0;
        std::uint32_t targetMarkerId = 0;
        std::vector<Member> members;
        replay::editor::Vec3 center{};
        replay::editor::Vec3 baselineCenter{};
        float range = 0.0f;
        float baselineRange = 0.0f;
        float intensity = 0.0f;
        float baselineIntensity = 0.0f;
        std::uint32_t colorR = 255;
        std::uint32_t colorG = 255;
        std::uint32_t colorB = 255;
        std::uint32_t blend = 3;
        bool colorChanged = false;
        bool blendChanged = false;
        float pitch = 0.0f;
        float roll = 0.0f;
        float yaw = 0.0f;
    };

    struct LightRigUngroupUndoState
    {
        bool valid = false;
        bool ungrouped = false;
        std::string clip;
        std::uint32_t markerId = 0;
        LightRigGroup group;
    };

    std::vector<LightRigGroup> g_lightRigGroups;
    char g_lightRigGroupsClip[kClipNameCapacity]{};
    std::uint32_t g_lightSelectedRigGroupId = 0;
    LightRigEditDraft g_lightRigEditDraft;
    LightRigUngroupUndoState g_lightRigUngroupUndo;

    std::wstring LightRigFolder();

    std::wstring LightRigGroupsPath()
    {
        if (!g_snapshot.currentClip[0])
            return {};
        std::string key;
        for (const unsigned char c : std::string(g_snapshot.currentClip))
            key.push_back(std::isalnum(c) || c == '-' || c == '_' ? static_cast<char>(c) : '_');
        if (key.empty())
            return {};
        return LightRigFolder() + L"\\groups_" + std::wstring(key.begin(), key.end()) + L".bin";
    }

    void SaveLightRigGroups()
    {
        const std::wstring path = LightRigGroupsPath();
        if (path.empty())
            return;
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file)
            return;
        const std::uint32_t header[3] = {0x47524C52u, 1u, static_cast<std::uint32_t>(std::min<std::size_t>(g_lightRigGroups.size(), 128))};
        bool ok = std::fwrite(header, sizeof(header), 1, file) == 1;
        for (std::size_t i = 0; ok && i < header[2]; ++i)
        {
            const LightRigGroup& group = g_lightRigGroups[i];
            struct DiskGroup
            {
                std::uint32_t id;
                std::uint32_t memberCount;
                char savedName[48];
                char displayName[48];
                std::uint32_t memberIds[64];
            } disk{};
            disk.id = group.id;
            disk.memberCount = static_cast<std::uint32_t>(std::min<std::size_t>(group.memberIds.size(), 64));
            std::snprintf(disk.savedName, sizeof(disk.savedName), "%s", group.savedName);
            std::snprintf(disk.displayName, sizeof(disk.displayName), "%s", group.displayName);
            std::copy_n(group.memberIds.begin(), disk.memberCount, disk.memberIds);
            ok = std::fwrite(&disk, sizeof(disk), 1, file) == 1;
        }
        if (ok)
            std::fflush(file);
        std::fclose(file);
    }

    void LoadLightRigGroups()
    {
        g_lightRigGroups.clear();
        const std::wstring path = LightRigGroupsPath();
        if (path.empty())
            return;
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
            return;
        std::uint32_t header[3]{};
        bool ok = std::fread(header, sizeof(header), 1, file) == 1 && header[0] == 0x47524C52u &&
            header[1] == 1u && header[2] <= 128u;
        for (std::uint32_t i = 0; ok && i < header[2]; ++i)
        {
            struct DiskGroup
            {
                std::uint32_t id;
                std::uint32_t memberCount;
                char savedName[48];
                char displayName[48];
                std::uint32_t memberIds[64];
            } disk{};
            ok = std::fread(&disk, sizeof(disk), 1, file) == 1 && disk.id != 0 && disk.memberCount > 0 && disk.memberCount <= 64;
            if (!ok)
                break;
            disk.savedName[sizeof(disk.savedName) - 1] = '\0';
            disk.displayName[sizeof(disk.displayName) - 1] = '\0';
            LightRigGroup group;
            group.id = disk.id;
            std::snprintf(group.savedName, sizeof(group.savedName), "%s", disk.savedName);
            std::snprintf(group.displayName, sizeof(group.displayName), "%s", disk.displayName[0] ? disk.displayName : disk.savedName);
            group.memberIds.assign(disk.memberIds, disk.memberIds + disk.memberCount);
            g_lightRigGroups.push_back(std::move(group));
        }
        std::fclose(file);
        if (!ok)
            g_lightRigGroups.clear();
    }

    void RegisterLightRigGroup(const std::wstring& filename, const std::vector<std::uint32_t>& ids)
    {
        if (ids.empty())
            return;
        std::string sourceName;
        for (wchar_t c : filename)
        {
            if (c == L'.')
                break;
            sourceName.push_back(static_cast<char>(c));
        }
        LightRigGroup group;
        group.id = ids.front();
        std::snprintf(group.savedName, sizeof(group.savedName), "%s", sourceName.c_str());
        std::snprintf(group.displayName, sizeof(group.displayName), "%s", sourceName.c_str());
        group.memberIds = ids;
        g_lightRigGroups.erase(std::remove_if(g_lightRigGroups.begin(), g_lightRigGroups.end(), [&](const LightRigGroup& item) {
            return item.id == group.id;
        }), g_lightRigGroups.end());
        g_lightRigGroups.push_back(std::move(group));
        SaveLightRigGroups();
    }

    LightRigGroup* FindLightRigGroup(std::uint32_t id)
    {
        const auto found = std::find_if(g_lightRigGroups.begin(), g_lightRigGroups.end(), [id](const LightRigGroup& group) {
            return group.id == id;
        });
        return found == g_lightRigGroups.end() ? nullptr : &*found;
    }

    replay::editor::Vec3 RotateLightRigVector(replay::editor::Vec3 value,
        float pitchDegrees, float rollDegrees, float yawDegrees)
    {
        constexpr float kRadians = 0.01745329251994329577f;
        const float pitch = pitchDegrees * kRadians;
        const float roll = rollDegrees * kRadians;
        const float yaw = yawDegrees * kRadians;
        const float x1 = std::cos(roll) * value.x + std::sin(roll) * value.z;
        const float y1 = value.y;
        const float z1 = -std::sin(roll) * value.x + std::cos(roll) * value.z;
        const float x2 = x1;
        const float y2 = std::cos(pitch) * y1 - std::sin(pitch) * z1;
        const float z2 = std::sin(pitch) * y1 + std::cos(pitch) * z1;
        return {std::cos(yaw) * x2 - std::sin(yaw) * y2,
            std::sin(yaw) * x2 + std::cos(yaw) * y2, z2};
    }

    void LightRigDirectionUp(float pitchDegrees, float yawDegrees, float rollDegrees,
        replay::editor::Vec3& forward, replay::editor::Vec3& up)
    {
        constexpr float kRadians = 0.01745329251994329577f;
        const float pitch = pitchDegrees * kRadians, yaw = yawDegrees * kRadians, roll = rollDegrees * kRadians;
        const float rightX = std::cos(yaw), rightY = std::sin(yaw);
        const float baseUpX = std::sin(yaw) * std::sin(pitch);
        const float baseUpY = -std::cos(yaw) * std::sin(pitch);
        const float baseUpZ = std::cos(pitch);
        forward = {-std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch)};
        up = {baseUpX * std::cos(roll) - rightX * std::sin(roll),
            baseUpY * std::cos(roll) - rightY * std::sin(roll), baseUpZ * std::cos(roll)};
    }

    void LightRigAnglesFromDirectionUp(const replay::editor::Vec3& forward, const replay::editor::Vec3& up,
        float& pitchDegrees, float& yawDegrees, float& rollDegrees)
    {
        constexpr float kDegrees = 57.295779513082320876f;
        const float pitch = std::atan2(forward.z, std::sqrt(forward.x * forward.x + forward.y * forward.y));
        const float yaw = std::atan2(-forward.x, forward.y);
        const float rightX = std::cos(yaw), rightY = std::sin(yaw);
        const float baseUpX = std::sin(yaw) * std::sin(pitch);
        const float baseUpY = -std::cos(yaw) * std::sin(pitch);
        const float baseUpZ = std::cos(pitch);
        const float alongRight = up.x * rightX + up.y * rightY;
        const float alongUp = up.x * baseUpX + up.y * baseUpY + up.z * baseUpZ;
        pitchDegrees = pitch * kDegrees;
        yawDegrees = std::atan2(-forward.x, forward.y) * kDegrees;
        rollDegrees = std::atan2(-alongRight, alongUp) * kDegrees;
    }

    void BeginLightRigEditDraft(const LightRigGroup& group, const LightListRow& row, std::uint32_t markerId)
    {
        g_lightRigEditDraft = {};
        LightRigEditDraft& draft = g_lightRigEditDraft;
        draft.groupId = group.id;
        draft.markerId = markerId;
        draft.members.reserve(row.sourceIndices.size());
        for (const std::uint32_t sourceIndex : row.sourceIndices)
        {
            if (sourceIndex >= g_snapshot.authoredLightCount)
                continue;
            LightRigEditDraft::Member member;
            member.sourceIndex = sourceIndex;
            member.light = g_snapshot.authoredLights[sourceIndex];
            draft.members.push_back(member);
            draft.center.x += member.light.position.x;
            draft.center.y += member.light.position.y;
            draft.center.z += member.light.position.z;
            draft.range += member.light.range;
            draft.intensity += member.light.intensity;
        }
        if (draft.members.empty())
            return;
        const float inverseCount = 1.0f / static_cast<float>(draft.members.size());
        draft.center.x *= inverseCount;
        draft.center.y *= inverseCount;
        draft.center.z *= inverseCount;
        draft.range *= inverseCount;
        draft.intensity *= inverseCount;
        std::uint32_t colorR = 0, colorG = 0, colorB = 0;
        for (const LightRigEditDraft::Member& member : draft.members)
        {
            colorR += member.light.colorR;
            colorG += member.light.colorG;
            colorB += member.light.colorB;
        }
        draft.colorR = static_cast<std::uint32_t>(std::lround(colorR * inverseCount));
        draft.colorG = static_cast<std::uint32_t>(std::lround(colorG * inverseCount));
        draft.colorB = static_cast<std::uint32_t>(std::lround(colorB * inverseCount));
        draft.blend = draft.members.front().light.blend;
        draft.baselineCenter = draft.center;
        draft.baselineRange = draft.range;
        draft.baselineIntensity = draft.intensity;
        draft.targetMarkerId = draft.members.front().light.markerId;
        draft.valid = true;
    }

    AuthoredLight LightRigEditedMember(const LightRigEditDraft& draft, const LightRigEditDraft::Member& member)
    {
        AuthoredLight result = member.light;
        const replay::editor::Vec3 relative{member.light.position.x - draft.baselineCenter.x,
            member.light.position.y - draft.baselineCenter.y, member.light.position.z - draft.baselineCenter.z};
        const replay::editor::Vec3 moved = RotateLightRigVector(relative, draft.pitch, draft.roll, draft.yaw);
        result.position = {draft.center.x + moved.x, draft.center.y + moved.y, draft.center.z + moved.z};
        result.range = std::clamp(member.light.range + draft.range - draft.baselineRange, 0.5f, 250.0f);
        result.intensity = std::clamp(member.light.intensity + draft.intensity - draft.baselineIntensity, 0.0f, 300.0f);
        if (draft.colorChanged)
        {
            result.colorR = draft.colorR;
            result.colorG = draft.colorG;
            result.colorB = draft.colorB;
        }
        if (draft.blendChanged)
            result.blend = draft.blend;
        result.markerId = draft.targetMarkerId;
        replay::editor::Vec3 direction{}, up{};
        LightRigDirectionUp(member.light.aimPitch, member.light.aimYaw, member.light.aimRoll, direction, up);
        direction = RotateLightRigVector(direction, draft.pitch, draft.roll, draft.yaw);
        up = RotateLightRigVector(up, draft.pitch, draft.roll, draft.yaw);
        LightRigAnglesFromDirectionUp(direction, up, result.aimPitch, result.aimYaw, result.aimRoll);
        return result;
    }

    std::array<int, 3> LightTemperatureRgb(float kelvin)
    {
        const float t = kelvin / 100.0f;
        float r = 255.0f, g = 255.0f, b = 255.0f;
        if (t <= 66.0f)
        {
            g = 99.4708025861f * std::log(t) - 161.1195681661f;
            b = t <= 19.0f ? 0.0f : 138.5177312231f * std::log(t - 10.0f) - 305.0447927307f;
        }
        else
        {
            r = 329.698727446f * std::pow(t - 60.0f, -0.1332047592f);
            g = 288.1221695283f * std::pow(t - 60.0f, -0.0755148492f);
        }
        return {static_cast<int>(std::lround(std::clamp(r, 0.0f, 255.0f))),
            static_cast<int>(std::lround(std::clamp(g, 0.0f, 255.0f))),
            static_cast<int>(std::lround(std::clamp(b, 0.0f, 255.0f)))};
    }

    const char* LightBlendLabel(std::uint32_t blend)
    {
        switch (blend)
        {
        case static_cast<std::uint32_t>(replay::editor::Blend::Linear): return "LINEAR";
        case static_cast<std::uint32_t>(replay::editor::Blend::Smooth): return "SMOOTH";
        case static_cast<std::uint32_t>(replay::editor::Blend::Spline): return "SPLINE";
        default: return "NO BLEND";
        }
    }

    std::uint32_t CycleLightBlend(std::uint32_t blend, int direction)
    {
        static constexpr std::array<std::uint32_t, 4> order = {
            static_cast<std::uint32_t>(replay::editor::Blend::Spline),
            static_cast<std::uint32_t>(replay::editor::Blend::Linear),
            static_cast<std::uint32_t>(replay::editor::Blend::Smooth),
            static_cast<std::uint32_t>(replay::editor::Blend::None)};
        const auto current = std::find(order.begin(), order.end(), blend);
        const int index = current == order.end() ? 0 : static_cast<int>(current - order.begin());
        const int next = (index + direction + static_cast<int>(order.size())) % static_cast<int>(order.size());
        return order[static_cast<std::size_t>(next)];
    }

    std::wstring LightRigFolder()
    {
        wchar_t local[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        std::wstring folder = length > 0 && length < MAX_PATH ? std::wstring(local, length) : std::wstring(L".");
        folder += L"\\RedemptionEditor\\LightRigs";
        const std::size_t separator = folder.find_last_of(L"\\/");
        if (separator != std::wstring::npos)
            CreateDirectoryW(folder.substr(0, separator).c_str(), nullptr);
        CreateDirectoryW(folder.c_str(), nullptr);
        return folder;
    }

    bool SafeLightRigName(const char* name)
    {
        if (!name || !*name || std::strlen(name) >= 40)
            return false;
        for (const unsigned char c : std::string(name))
            if (!(std::isalnum(c) || c == ' ' || c == '-' || c == '_'))
                return false;
        return true;
    }

    std::wstring LightRigPath(const char* name)
    {
        std::wstring path = LightRigFolder();
        path += L"\\";
        for (const unsigned char c : std::string(name ? name : ""))
            path.push_back(static_cast<wchar_t>(c));
        path += L".rdrig";
        return path;
    }

    std::vector<std::wstring> LightRigFiles()
    {
        std::vector<std::wstring> files;
        const std::wstring pattern = LightRigFolder() + L"\\*.rdrig";
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW(pattern.c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
            return files;
        do
        {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                files.emplace_back(data.cFileName);
        } while (FindNextFileW(find, &data));
        FindClose(find);
        std::sort(files.begin(), files.end());
        return files;
    }

    const char* LightTargetLabel(std::uint32_t id)
    {
        for (std::uint32_t n = 0; n < g_snapshot.targetCount; ++n)
            if (g_snapshot.targets[n].id == id)
                return g_snapshot.targets[n].label;
        return "";
    }

    bool SaveLightRig(const char* name, const std::vector<AuthoredLight>& lights)
    {
        if (!SafeLightRigName(name) || lights.size() > 64)
            return false;
        const std::wstring path = LightRigPath(name);
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file)
            return false;
        const std::uint32_t header[3] = {0x47495252u, 1u, static_cast<std::uint32_t>(lights.size())}; // R RIG
        bool ok = std::fwrite(header, sizeof(header), 1, file) == 1;
        for (const AuthoredLight& source : lights)
        {
            AuthoredLight light = source;
            light.markerId = 0; // rigs apply to the marker selected when loaded
            light.id = 0;       // remapped to a fresh clip-wide id on load
            char attachment[kTargetLabelCapacity]{};
            char lookAt[kTargetLabelCapacity]{};
            std::snprintf(attachment, sizeof(attachment), "%s", LightTargetLabel(source.attachedTarget));
            std::snprintf(lookAt, sizeof(lookAt), "%s", LightTargetLabel(source.lookAtTarget));
            ok = ok && std::fwrite(&light, sizeof(light), 1, file) == 1 &&
                std::fwrite(attachment, sizeof(attachment), 1, file) == 1 &&
                std::fwrite(lookAt, sizeof(lookAt), 1, file) == 1;
        }
        ok = ok && std::fflush(file) == 0;
        std::fclose(file);
        return ok;
    }

    bool LoadLightRig(const std::wstring& filename, std::vector<LightRigEntry>& entries)
    {
        entries.clear();
        const std::wstring path = LightRigFolder() + L"\\" + filename;
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
            return false;
        std::uint32_t header[3]{};
        bool ok = std::fread(header, sizeof(header), 1, file) == 1 && header[0] == 0x47495252u &&
            header[1] == 1u && header[2] <= 64u;
        if (ok)
        {
            entries.resize(header[2]);
            for (LightRigEntry& entry : entries)
            {
                ok = std::fread(&entry.light, sizeof(entry.light), 1, file) == 1 &&
                    std::fread(entry.attachmentLabel, sizeof(entry.attachmentLabel), 1, file) == 1 &&
                    std::fread(entry.lookAtLabel, sizeof(entry.lookAtLabel), 1, file) == 1;
                if (!ok)
                    break;
                entry.light.name[sizeof(entry.light.name) - 1] = '\0';
                entry.attachmentLabel[sizeof(entry.attachmentLabel) - 1] = '\0';
                entry.lookAtLabel[sizeof(entry.lookAtLabel) - 1] = '\0';
                if (!std::isfinite(entry.light.position.x) || !std::isfinite(entry.light.position.y) ||
                    !std::isfinite(entry.light.position.z) || !std::isfinite(entry.light.range) ||
                    !std::isfinite(entry.light.intensity) || entry.light.range < 0.5f || entry.light.range > 250.0f ||
                    entry.light.intensity < 0.0f || entry.light.intensity > 300.0f)
                {
                    ok = false;
                    break;
                }
            }
        }
        std::fclose(file);
        if (!ok)
            entries.clear();
        return ok;
    }

    void LoadLightTargetFavorites()
    {
        if (g_lightTargetFavoritesRead)
            return;
        g_lightTargetFavoritesRead = true;
#ifndef REDEMPTION_UI_HARNESS
        wchar_t folder[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, folder);
        const std::wstring ini = replay::identity::IniPathInW(folder);
        wchar_t value[2048]{};
        replay::identity::IniStringW(L"LightTargetFavorites", L"", value, static_cast<DWORD>(std::size(value)), ini.c_str());
        std::string encoded;
        for (const wchar_t c : std::wstring(value))
            encoded.push_back(c <= 0x7f ? static_cast<char>(c) : '?');
        std::size_t start = 0;
        while (start < encoded.size())
        {
            const std::size_t end = encoded.find('|', start);
            const std::string label = encoded.substr(start, end == std::string::npos ? end : end - start);
            if (!label.empty() && std::find(g_lightTargetFavorites.begin(), g_lightTargetFavorites.end(), label) == g_lightTargetFavorites.end())
                g_lightTargetFavorites.push_back(label);
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
#endif
    }

    void SaveLightTargetFavorites()
    {
#ifndef REDEMPTION_UI_HARNESS
        wchar_t folder[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, folder);
        const std::wstring ini = replay::identity::IniPathInW(folder);
        std::wstring value;
        for (const std::string& label : g_lightTargetFavorites)
        {
            if (!value.empty()) value += L"|";
            for (const unsigned char c : label) value.push_back(static_cast<wchar_t>(c));
        }
        replay::identity::IniWriteW(L"LightTargetFavorites", value.c_str(), ini.c_str());
#endif
    }

    void ClearLightDraftAttachSync()
    {
        g_lightDraftAttachSyncPending = false;
        g_lightDraftAttachSyncId = 0;
        g_lightDraftAttachSyncTarget = 0;
        g_lightDraftAttachSyncFrames = 0;
    }

    void BeginLightDraftAttachSync()
    {
        g_lightDraftAttachSyncPending = true;
        g_lightDraftAttachSyncId = g_lightDraft.id;
        g_lightDraftAttachSyncTarget = g_lightDraft.attachedTarget;
        g_lightDraftAttachSyncFrames = 0;
    }

    // Attachment changes are canonicalized on the game thread, which converts
    // the UI's world-space position into a local offset. Wait for that snapshot
    // before sending another full light draft, or the stale world position can
    // overwrite the converted offset on the next Look At or property edit.
    bool SyncLightDraftAttachment(const AuthoredLight& current)
    {
        if (!g_lightDraftAttachSyncPending)
            return false;
        if (current.id != g_lightDraftAttachSyncId)
        {
            ClearLightDraftAttachSync();
            return false;
        }
        if (current.attachedTarget != g_lightDraftAttachSyncTarget && ++g_lightDraftAttachSyncFrames < 30)
            return true;

        g_lightDraft = current;
        if (g_lightDraft.reflectorEnabled)
        {
            g_lightDraft.coneEnabled = 1;
            g_lightDraft.reflectorEnabled = 0;
            g_lightDraft.reflectorHidden = 0;
        }
        g_lightDraftDirty = false;
        ClearLightDraftAttachSync();
        return true;
    }

    std::uint32_t PreviousCameraMarkerId(std::uint32_t markerId)
    {
        if (!markerId)
            return 0;
        for (std::uint32_t n = 1; n < g_snapshot.cameraKeyframeCount; ++n)
            if (g_snapshot.cameraKeyframes[n].id == markerId)
                return g_snapshot.cameraKeyframes[n - 1].id;
        return 0;
    }

    void OpenLightsForMarker(std::uint32_t markerId)
    {
        g_lightReturnScreen = g_screen;
        // F6 can open lighting directly over a running replay. Keep the replay
        // timeline behind the light controls in that path as well.
        if (g_lightReturnScreen == Screen::Closed && g_hasSnapshot &&
            (Flag(SnapshotPlaying) || g_snapshot.cameraKeyframeCount > 0))
            g_lightReturnScreen = PlaybackScreen();
        g_lightFocusMarkerId = markerId;
        g_lightLastFocusMarkerId = markerId;
        g_lightDraftIndex = -1;
        g_lightDraftDirty = false;
        ClearLightDraftAttachSync();
        g_lightNumericEditAxis = -1;
        g_lightSplineOpen = false;
        g_lightSplineDefaultsOpen = false;
        g_lightSplineSelectedRow = 0;
        g_lightCopySourceMarkerId = PreviousCameraMarkerId(markerId);
        g_screen = Screen::Lights;
        g_helpOpen = false;
    }

    std::uint32_t CurrentLightMarkerId()
    {
        if (!g_hasSnapshot)
            return 0;
        // The light panel leaves the edit timeline visible. Tab there changes
        // the selected camera marker while the panel stays open, so follow
        // that selection instead of pinning the filter to the marker that
        // happened to be active when Lights was opened.
        if (g_screen == Screen::Lights && g_lightReturnScreen == Screen::Edit && g_selectedKeyframeId)
            for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
                if (g_snapshot.cameraKeyframes[n].id == g_selectedKeyframeId)
                    return g_selectedKeyframeId;
        // A timeline seek can clear or briefly invalidate the selection while
        // its snapshot catches up. In Lights, follow the playhead before using
        // the cached panel marker so a Tab seek cannot leave the old light list
        // visible for the next marker.
        if (g_screen == Screen::Lights && g_lightReturnScreen == Screen::Edit)
        {
            std::uint32_t atPlayhead = 0;
            for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
            {
                const CameraKeyframe& marker = g_snapshot.cameraKeyframes[n];
                if (marker.timeMs > g_snapshot.playheadMs)
                    break;
                atPlayhead = marker.id;
            }
            if (atPlayhead)
                return atPlayhead;
        }
        if (g_screen == Screen::Lights && g_lightFocusMarkerId)
            for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
                if (g_snapshot.cameraKeyframes[n].id == g_lightFocusMarkerId)
                    return g_lightFocusMarkerId;
        if (g_snapshot.editingMarker)
            for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
                if (g_snapshot.cameraKeyframes[n].id == g_snapshot.editingMarker)
                    return g_snapshot.editingMarker;
        if (g_screen == Screen::Edit && g_selectedKeyframeId)
            for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
                if (g_snapshot.cameraKeyframes[n].id == g_selectedKeyframeId)
                    return g_selectedKeyframeId;
        std::uint32_t active = 0;
        for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
        {
            const CameraKeyframe& marker = g_snapshot.cameraKeyframes[n];
            if (marker.timeMs > g_snapshot.playheadMs)
                break;
            active = marker.id;
        }
        return active;
    }

    void SyncLightEditorMarkerSelection(std::uint32_t markerId)
    {
        if (g_screen != Screen::Lights || g_lightReturnScreen != Screen::Edit || !markerId)
            return;
        for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
            if (g_snapshot.cameraKeyframes[n].id == markerId)
            {
                g_lightFocusMarkerId = markerId;
                return;
            }
    }

    bool LightMenuOwnsPointer()
    {
        if (g_screen != Screen::Lights || g_lightReturnScreen != Screen::Edit)
            return false;
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float panelWidth = std::min(500.0f, g_vw - 36.0f);
        const ImVec2 panelMin = P(0.0f, 0.0f);
        const ImVec2 panelMax = P(panelWidth, g_vh);
        const float menuBottom = std::min(g_vh - g_footH - 18.0f, 32.0f + 900.0f);
        // The menu owns clicks in its control area; the timeline below the panel
        // remains interactive even where the translucent background continues.
        return mouse.x >= panelMin.x && mouse.x <= panelMax.x &&
            mouse.y >= panelMin.y && mouse.y < P(0.0f, menuBottom).y;
    }

    void RestoreLightMarkerSelection()
    {
        if (g_lightReturnScreen != Screen::Edit)
            return;
        const std::uint32_t markerId = CurrentLightMarkerId();
        if (!markerId)
            return;
        for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
            if (g_snapshot.cameraKeyframes[n].id == markerId)
            {
                g_selectedKeyframeId = markerId;
                return;
            }
    }

    std::uint32_t NextSnapshotLightId()
    {
        std::uint32_t next = 1;
        for (std::uint32_t n = 0; n < std::min(g_snapshot.authoredLightCount, kMaxAuthoredLights); ++n)
            if (g_snapshot.authoredLights[n].id >= next && g_snapshot.authoredLights[n].id != 0xFFFFFFFFu)
                next = g_snapshot.authoredLights[n].id + 1;
        // Rig members can be queued while the snapshot still reflects the
        // previous marker. Keep new standalone lights clear of those reserved IDs.
        for (const LightRigGroup& group : g_lightRigGroups)
            for (const std::uint32_t id : group.memberIds)
                if (id >= next && id != 0xFFFFFFFFu)
                    next = id + 1;
        return next ? next : 1;
    }

    // The open project (Snapshot::projectFlags).
    bool ProjectFlag(std::uint32_t flag) { return g_hasSnapshot && (g_snapshot.projectFlags & flag) != 0; }
    bool ProjectIsOpen() { return ProjectFlag(ProjectOpenFlag); }
    // The screen of a playback: the project's timeline while the project
    // plays as a whole, the clip editor otherwise (a project clip in it too).
    Screen PlaybackScreen() { return ProjectFlag(ProjectPreviewing) ? Screen::Project : Screen::Edit; }

    void SyncLightEditorState()
    {
        if (!g_hasSnapshot)
            return;
        const int state = (g_screen == Screen::Lights ? 1 : 0) | (g_lightGizmosVisibleUi ? 2 : 0);
        if (state == g_sentLightEditorState)
            return;
        Command command{};
        command.enabled = (state & 1) ? 1u : 0u;
        command.target = ((state & 2) ? 1u : 0u) | ((state & 4) ? 2u : 0u);
        if (Send(CommandType::SetLightEditorState, &command))
            g_sentLightEditorState = state;
    }

namespace
{
    // The clip editor shows a project's clip (Esc goes back to the project).
    bool g_editingProjectClip = false;
    bool g_previousMouseDrawCursor = false;
    // H hides the replay UI: in the clip editor (fly the free camera with the UI
    // hidden) and with the editor closed while a clip plays: the playback bar,
    // the key bar and the notices go, the replay keys (the core's) keep working.
    replay::editor::uiqa::HideState g_hide;
}
    bool UiHidden()
    {
        const bool playing = g_hasSnapshot && Flag(SnapshotPlaying);
        return g_hide.HiddenOn(g_screen == Screen::Edit, g_screen == Screen::Closed, playing);
    }
    Alert g_alert;

    void ShowAlert(const char* title, const std::string& body, const char* confirm, CommandType action,
        const std::string& name, Screen back)
    {
        g_alert = {title, body, confirm ? confirm : "", action, name, back, Screen::Count};
        g_screen = Screen::Alert;
    }

    void FormatClock(std::uint64_t timeMs, char* out, std::size_t size, bool tenths)
    {
        const unsigned minutes = static_cast<unsigned>(timeMs / 60000);
        const unsigned seconds = static_cast<unsigned>((timeMs / 1000) % 60);
        if (tenths)
            std::snprintf(out, size, "%02u:%02u.%u", minutes, seconds, static_cast<unsigned>((timeMs / 100) % 10));
        else
            std::snprintf(out, size, "%02u:%02u", minutes, seconds);
    }

    // "clip-20260927-170346.rpclip" -> "27/09 17:03"; "042-SaintDenis-p2.rpclip"
    // -> "042 Saint Denis p2" (dashes to spaces, the CamelCase place split);
    // other names as they are.
    std::string ClipLabel(const char* name)
    {
        unsigned y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
        if (std::sscanf(name, "clip-%4u%2u%2u-%2u%2u%2u", &y, &mo, &d, &h, &mi, &se) == 6)
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "%02u/%02u %02u:%02u", d, mo, h, mi);
            return text;
        }
        std::string text(name);
        const std::size_t dot = text.rfind(".rpclip");
        if (dot != std::string::npos)
            text = text.substr(0, dot);
        std::size_t digits = 0;
        while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9')
            ++digits;
        if (digits < 3 || digits >= text.size() || text[digits] != '-')
            return text;
        std::string label;
        for (std::size_t n = 0; n < text.size(); ++n)
        {
            const char c = text[n];
            if (c == '-')
            {
                label += ' ';
                continue;
            }
            if (n > digits && c >= 'A' && c <= 'Z' && text[n - 1] >= 'a' && text[n - 1] <= 'z')
                label += ' ';
            label += c;
        }
        return label;
    }

    // The alerts and refusals a screen words (the UI harness's fixtures show the same).
    void ShowDeleteClipAlert(const char* name, bool openNow)
    {
        ShowAlert(TR("gallery.delete.title", "Delete clip"), TRF("gallery.delete.body", "Delete {label} ({file})? The file is removed from the ReplayClips folder.{open}",
            {{"label", ClipLabel(name)}, {"file", name},
                {"open", openNow ? (Flag(SnapshotClipEdited) ? TR("gallery.delete.open_edited", " It is open: it closes and its unsaved changes are lost.")
                                                             : TR("gallery.delete.open", " It is open: it closes.")) : ""}}),
            TR("gallery.delete.confirm", "Delete"), CommandType::DeleteClip, name, Screen::Gallery);
    }

    void ShowDeleteProjectAlert(const char* file, const char* shownName, bool openNow)
    {
        ShowAlert(TR("projects.delete.title", "Delete project"), TRF("projects.delete.body",
            "Delete the project {name}? Its file is removed from the ReplayProjects folder; the clips stay in ReplayClips.{open}",
            {{"name", shownName}, {"open", openNow ? TR("gallery.delete.open_edited", " It is open: it closes and its unsaved changes are lost.") : ""}}),
            TR("gallery.delete.confirm", "Delete"), CommandType::ProjectDelete, file, Screen::Projects);
    }

namespace
{

    void ShowSaveFailedAlert(Screen back)
    {
        ShowAlert(TR("alert.title", "Alert"),
            KeyFill(TR("alert.save_failed", "A replay could not be saved (the disk may be full or the file in use). It is kept in memory and saving "
            "is retried; saving again ({key_save}) retries at once. Keep the game open until it is saved.")),
            nullptr, CommandType::None, "", back);
    }
}

    const char* NameEmptyError() { return TR("rename.error.empty", "Type a name."); }
    const char* NameCharsError() { return TR("rename.error.chars", "Only letters, numbers, spaces, - and _."); }

    void FormatSize(std::uint64_t bytes, char* out, std::size_t size)
    {
        if (bytes >= 1024ull * 1024ull * 1024ull)
            std::snprintf(out, size, "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
        else if (bytes >= 1024ull * 1024ull)
            std::snprintf(out, size, "%.1f MB", bytes / (1024.0 * 1024.0));
        else
            std::snprintf(out, size, "%llu KB", static_cast<unsigned long long>((bytes + 1023) / 1024));
    }

    // The menus' mouse and keys (src/editor_ui_qa.h MenuPointer): the row under
    // the mouse takes the selection only on a frame the mouse moved, a
    // navigation key always wins, and the grey outline under the mouse shows
    // only while the mouse is what moved last. A resting mouse over a list
    // used to keep its grey outline where it stood while Up and Down moved the
    // red one: the arrows looked dead.
    replay::editor::uiqa::MenuPointer g_menuPointer;

namespace
{

    // A navigation key this frame (the keys that move or use a selection).
    bool NavigationKeyPressed()
    {
        for (ImGuiKey key : {ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_PageUp, ImGuiKey_PageDown,
                 ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Tab, ImGuiKey_Home, ImGuiKey_End})
            if (KeyPressed(key))
                return true;
        for (ImGuiKey key : {ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
                 ImGuiKey_GamepadFaceDown})
            if (ImGui::IsKeyPressed(key, true))
                return true;
        return false;
    }

    // Once a frame, before the screens draw.
    void BeginMenuPointerFrame()
    {
        const ImGuiIO& io = ImGui::GetIO();
        g_menuPointer.BeginFrame(ImGui::IsMousePosValid(&io.MousePos), io.MousePos.x, io.MousePos.y, NavigationKeyPressed());
    }
}

    // The grey outline of the item under the mouse (not after a key).
    bool HoverShown(bool hovered) { return hovered && g_menuPointer.HoverShown(); }

    // A list item in its row box (RowBox: red ink when selected, grey ink under
    // the mouse once it moved), click selects; true on activation (double
    // click). `pointed` (a list that never scrolls under the mouse): set when
    // the mouse moved onto this row, which then takes the selection.
    bool ListItem(ImDrawList* draw, const char* id, float x, float y, float width, float height, bool selected, bool& clicked,
        bool* pointed, float visualWidth, int theme, ImU32 activeInk, ImU32 hoverInk)
    {
        ImGui::SetCursorScreenPos(P(x, y));
        ImGui::InvisibleButton(id, ImVec2(S(width), S(height)));
        clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left) || (ImGui::IsItemHovered() && MissedClick());
        const bool hovered = ImGui::IsItemHovered();
        const bool point = pointed && !clicked && !selected && hovered && g_menuPointer.Moved();
        if (pointed)
            *pointed = point;
        if ((clicked || point) && !selected)
            PlayUi(UiSound::NavDown);
        RowBox(draw, x - 14.0f, y, x + (visualWidth > 0.0f ? visualWidth : width), y + height,
            selected || point ? RowLook::Selected : HoverShown(hovered) ? RowLook::Hovered : RowLook::Rest, theme, activeInk, hoverInk);
        return hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    }

    // Keyboard list navigation shared by the menus.
    void NavigateList(int& index, int count)
    {
        if (count <= 0)
            return;
        if (KeyPressed(ImGuiKey_DownArrow))
        {
            index = (index + 1) % count;
            PlayUi(count > 1 ? UiSound::NavDown : UiSound::Error);
        }
        if (KeyPressed(ImGuiKey_UpArrow))
        {
            index = (index + count - 1) % count;
            PlayUi(count > 1 ? UiSound::NavUp : UiSound::Error);
        }
        index = std::clamp(index, 0, count - 1);
    }

    // The key help (F1) takes Esc and Enter while it is open, and the Esc that
    // closed it goes no further.
    bool g_helpOpen = false;
    bool g_helpSwallow = false;

    // Without the key repeat: a held Enter or Esc would go on through the
    // screens (open the gallery and play the clip under the selection, add a
    // marker and open its options, close the menu after cancelling a dialog).
    bool Confirmed() { return !g_helpOpen && (KeyPressed(ImGuiKey_Enter, false) || KeyPressed(ImGuiKey_KeypadEnter, false)); }

    bool Back() { return !g_helpOpen && !g_helpSwallow && (KeyPressed(ImGuiKey_Escape, false) || KeyPressed(ImGuiKey_Backspace, false)); }

    // ---------------- Home ----------------
    // Recorded Clips first (the most used), then New Project / Edit and Projects.
    // The selection is kept between visits; a clip closed from its editor comes
    // back on the Edit row.
    int g_homeIndex = 0; // Recorded Clips
    // The gallery adds a clip to the open project (A on the project's timeline)
    // instead of playing it.
    bool g_galleryPicking = false;
    std::uint32_t g_pickInsertAt = ~0u;
    ULONGLONG g_projectOpenAsked = 0; // a project asked for (new or open), the timeline waits for it
    recind::BriefTimer g_recBrief;
    recind::ReplayOnTimer g_replayOn;

namespace
{
    recind::Environment g_recEnv;
}
    recind::Choice g_recChoice;
    rec_window::Frame g_recRows; // this frame's rows for the window (DrawRecordingNotices)

namespace
{
#ifndef REDEMPTION_UI_HARNESS
    rec_window::Window g_recWindow;
    ULONGLONG g_recCheckedAt = 0;
    std::string g_recLogged;
#endif

    recind::Mode WantedIndicator() { return g_hasSnapshot ? recind::FromValue(g_snapshot.recordingIndicator) : recind::Mode::Normal; }
}

    recind::Kind IndicatorKind()
    {
        if (!g_hasSnapshot)
            return recind::Kind::None;
        return recind::KindOf(Flag(SnapshotBufferRunning), Flag(SnapshotBufferManual), g_snapshot.recordingMs);
    }

namespace
{

#ifndef REDEMPTION_UI_HARNESS
    bool WindowsExcludesFromCapture()
    {
        static int known = -1;
        if (known < 0)
        {
            OSVERSIONINFOEXW version{};
            version.dwOSVersionInfoSize = sizeof(version);
            using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
            const auto get = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
            known = get && get(&version) == 0 && recind::WindowsSupports(version.dwMajorVersion, version.dwBuildNumber) ? 1 : 0;
        }
        return known == 1;
    }

    // The game owns its monitor (exclusive fullscreen): no window shows over it.
    bool GameExclusiveFullscreen(reshade::api::effect_runtime* runtime)
    {
        const HWND hwnd = static_cast<HWND>(runtime->get_hwnd());
        RECT window{};
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (!hwnd || !GetWindowRect(hwnd, &window) || !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
            return false;
        const bool covers = recind::CoversMonitor({window.left, window.top, window.right, window.bottom},
            {monitor.rcMonitor.left, monitor.rcMonitor.top, monitor.rcMonitor.right, monitor.rcMonitor.bottom});
        if (!covers)
            return false;
        bool swapChainFullscreen = false;
        const reshade::api::device_api api = runtime->get_device()->get_api();
        if (api == reshade::api::device_api::d3d10 || api == reshade::api::device_api::d3d11 || api == reshade::api::device_api::d3d12)
        {
            IUnknown* native = reinterpret_cast<IUnknown*>(runtime->get_native());
            IDXGISwapChain* swapChain = nullptr;
            if (native && SUCCEEDED(native->QueryInterface(__uuidof(IDXGISwapChain), reinterpret_cast<void**>(&swapChain))))
            {
                BOOL fullscreen = FALSE;
                if (SUCCEEDED(swapChain->GetFullscreenState(&fullscreen, nullptr)))
                    swapChainFullscreen = fullscreen != FALSE;
                swapChain->Release();
            }
        }
        QUERY_USER_NOTIFICATION_STATE state{};
        const bool shellD3d = SUCCEEDED(SHQueryUserNotificationState(&state)) && state == QUNS_RUNNING_D3D_FULL_SCREEN;
        return recind::ExclusiveFullscreen(covers, swapChainFullscreen, shellD3d);
    }
#endif

    // Once a frame, before the screens: the brief clock, the mode drawn and,
    // in hidden, the window (its checks once a second).
    void UpdateRecordingIndicator(reshade::api::effect_runtime* runtime)
    {
        const ULONGLONG now = GetTickCount64();
        if (g_hasSnapshot)
        {
            g_recBrief.Update(now, IndicatorKind(), g_snapshot.recordingMs, g_snapshot.savedClipSequence);
            g_replayOn.Update(now, IndicatorKind(), g_snapshot.recordPauseReasons != 0);
        }
        const recind::Mode wanted = WantedIndicator();
        g_recRows.rows.clear();
#ifndef REDEMPTION_UI_HARNESS
        g_recRows.game = static_cast<HWND>(runtime->get_hwnd());
        if (wanted != recind::Mode::Hidden)
        {
            g_recWindow.Stop();
            g_recEnv = recind::Environment{}; // tried again the next time hidden is chosen
            g_recCheckedAt = 0;
        }
        else if (g_recCheckedAt == 0 || now - g_recCheckedAt >= 1000)
        {
            g_recCheckedAt = now;
            g_recEnv.windowsSupports = WindowsExcludesFromCapture();
            g_recEnv.exclusiveFullscreen = g_recEnv.windowsSupports && GameExclusiveFullscreen(runtime);
            if (g_recEnv.windowsSupports && !g_recEnv.affinityFailed && !g_recEnv.windowFailed && !g_recWindow.Started())
            {
                rec_window::Options options;
                options.fontFile = bundled_files::RedemptionEditorFolder() + L"fonts\\Redemption.otf";
                if (!g_recWindow.Start(options))
                    (g_recWindow.Failure().rfind("affinity", 0) == 0 ? g_recEnv.affinityFailed : g_recEnv.windowFailed) = true;
            }
        }
        g_recChoice = recind::Choose(wanted, g_recEnv);
        std::string line = std::string("REC_INDICATOR,mode=") + recind::IniValue(wanted) + ",shown=" + recind::IniValue(g_recChoice.mode) +
            ",why=" + recind::FallbackName(g_recChoice.why);
        if (!g_recWindow.Failure().empty())
            line += ",window=" + g_recWindow.Failure();
        if (line != g_recLogged)
        {
            g_recLogged = line;
            reshade::log::message(reshade::log::level::info, line.c_str());
        }
#else
        (void)runtime;
        g_recChoice = recind::Choose(wanted, g_recEnv);
#endif
    }

    // The window's rows of this frame, sent on every way out of OnOverlay.
    void SubmitRecordingWindow()
    {
#ifndef REDEMPTION_UI_HARNESS
        if (g_recWindow.Started())
            g_recWindow.Submit(g_recRows);
#endif
    }

    // ---------------- Replay Settings ----------------
    int g_settingsIndex = 0;
    // Replay Settings' list scroll, in units (only when it does not fit), and
    // the row it last brought into view.
    float g_settingsScroll = 0.0f;
    int g_settingsScrolledTo = -1;

    // A Replay Settings row as RDR2's own settings list: the label left, the
    // value right in gold between arrows (bright on the selected row). An
    // action row (no value) is its label alone.
    void SettingsRow(ImDrawList* draw, float x, float y, float width, const char* label, const char* value, bool selected)
    {
        constexpr float kSize = 24.0f;
        const float top = y + (44.0f - LabelHeight(kSize)) * 0.5f;
        const ImU32 labelColor = selected ? kText : IM_COL32(205, 198, 186, 255);
        if (!value || !*value)
        {
            RowTextFit(draw, x, top, kSize, labelColor, label, width, false);
            return;
        }
        const float right = x + width - 10.0f;
        const float valueWidth = std::min(TextWidth(kSize, value) / g_scale, width * 0.55f);
        const float valueX = right - 18.0f - valueWidth;
        TextFit(draw, x, top, kSize, labelColor, label, valueX - x - 34.0f);
        TextFit(draw, valueX, top, kSize, kGold, value, valueWidth);
        const ImU32 arrow = selected ? kGold : IM_COL32(150, 143, 131, 120);
        const float cy = y + 22.0f, h = kSize * 0.28f;
        draw->AddTriangleFilled(P(valueX - 18.0f, cy), P(valueX - 10.0f, cy - h), P(valueX - 10.0f, cy + h), arrow);
        draw->AddTriangleFilled(P(right, cy), P(right - 8.0f, cy - h), P(right - 8.0f, cy + h), arrow);
    }

    void DrawLightsLegacyWidgets(ImDrawList*)
    {
        if (!g_lightMenuOpacityRead)
        {
            g_lightMenuOpacityRead = true;
#ifndef REDEMPTION_UI_HARNESS
            wchar_t folder[MAX_PATH]{};
            GetCurrentDirectoryW(MAX_PATH, folder);
            const std::wstring ini = replay::identity::IniPathInW(folder);
            g_lightMenuOpacity = std::clamp(static_cast<int>(replay::identity::IniIntW(L"LightMenuOpacity", 50, ini.c_str())), 0, 100);
#endif
        }
        const float width = std::min(540.0f, g_vw - 36.0f);
        const float height = std::min(780.0f, std::max(430.0f, g_vh - 220.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.035f, 0.031f, 0.027f, g_lightMenuOpacity / 100.0f));
        ImGui::SetCursorScreenPos(P(18.0f, 42.0f));
        if (!ImGui::BeginChild("##LightStudio", ImVec2(S(width), S(height)), true, ImGuiWindowFlags_AlwaysVerticalScrollbar))
        {
            ImGui::EndChild();
            ImGui::PopStyleColor();
            return;
        }

        auto markerLabel = [](std::uint32_t markerId) {
            if (!markerId)
                return std::string("World (always)");
            for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
            {
                const CameraKeyframe& marker = g_snapshot.cameraKeyframes[n];
                if (marker.id == markerId)
                {
                    char label[64]{};
                    std::snprintf(label, sizeof(label), "Marker %u (%.3f s)", n + 1, marker.timeMs / 1000.0);
                    return std::string(label);
                }
            }
            return std::string("Missing marker");
        };
        const std::uint32_t totalCount = std::min(g_snapshot.authoredLightCount, kMaxAuthoredLights);
        const std::uint32_t focusMarker = CurrentLightMarkerId();
        std::vector<std::uint32_t> lightIndices;
        lightIndices.reserve(totalCount);
        for (std::uint32_t n = 0; n < totalCount; ++n)
            if (g_snapshot.authoredLights[n].markerId == 0 || g_snapshot.authoredLights[n].markerId == focusMarker)
                lightIndices.push_back(n);
        const std::uint32_t count = static_cast<std::uint32_t>(lightIndices.size());
        if (g_lightLastFocusMarkerId != focusMarker)
        {
            g_lightLastFocusMarkerId = focusMarker;
            g_lightFocusMarkerId = focusMarker;
            g_lightSelectedId = 0;
            g_lightSelected = 0;
            g_lightAdvancedOpen = false;
            g_lightSplineOpen = false;
            g_lightSplineDefaultsOpen = false;
            g_lightSettingsMenuOpen = false;
            g_lightRigMenuOpen = false;
            g_lightTargetPickerOpen = false;
            g_lightSplineSelectedRow = 0;
            g_lightListFirst = 0;
            g_lightDraftIndex = -1;
            g_lightDraftDirty = false;
            g_lightCopySourceMarkerId = PreviousCameraMarkerId(focusMarker);
        }
        ImGui::Text("LIGHTING  |  %s", markerLabel(focusMarker).c_str());
        ImGui::Text("F6 closes  |  %u lights max", kMaxAuthoredLights);
        ImGui::TextDisabled("Light settings save with this clip. Menu opacity: %d%% (INI: LightMenuOpacity).", g_lightMenuOpacity);
        ImGui::Separator();

        if (g_lightSelected >= static_cast<int>(std::max(1u, count)))
            g_lightSelected = static_cast<int>(count ? count - 1 : 0);
        const std::uint32_t selectedSourceIndex = count ? lightIndices[static_cast<std::size_t>(g_lightSelected)] : 0u;
        if (count == 0)
        {
            g_lightDraftIndex = -1;
            g_lightDraftDirty = false;
            ClearLightDraftAttachSync();
        }
        else if (static_cast<int>(selectedSourceIndex) != g_lightDraftIndex)
        {
            ClearLightDraftAttachSync();
            g_lightDraft = g_snapshot.authoredLights[selectedSourceIndex];
            if (g_lightDraft.reflectorEnabled)
            {
                g_lightDraft.coneEnabled = 1;
                g_lightDraft.reflectorEnabled = 0;
                g_lightDraft.reflectorHidden = 0;
            }
            g_lightDraftIndex = static_cast<int>(selectedSourceIndex);
            g_lightSelectedId = g_lightDraft.id;
            g_lightDraftDirty = false;
        }
        else
        {
            const AuthoredLight& current = g_snapshot.authoredLights[selectedSourceIndex];
            if (!SyncLightDraftAttachment(current) && g_lightDraftDirty && current.position.x == g_lightDraft.position.x && current.position.y == g_lightDraft.position.y &&
                current.position.z == g_lightDraft.position.z && current.range == g_lightDraft.range &&
                current.intensity == g_lightDraft.intensity && current.colorR == g_lightDraft.colorR &&
                current.colorG == g_lightDraft.colorG && current.colorB == g_lightDraft.colorB && current.enabled == g_lightDraft.enabled &&
                current.markerId == g_lightDraft.markerId && current.aimPitch == g_lightDraft.aimPitch &&
                current.aimYaw == g_lightDraft.aimYaw && current.beamAngle == g_lightDraft.beamAngle &&
                current.coneEnabled == g_lightDraft.coneEnabled && current.aimRoll == g_lightDraft.aimRoll &&
                current.reflectorEnabled == g_lightDraft.reflectorEnabled && current.reflectorHidden == g_lightDraft.reflectorHidden &&
                current.attachedTarget == g_lightDraft.attachedTarget && current.lookAtTarget == g_lightDraft.lookAtTarget)
                g_lightDraftDirty = false;
            if (!g_lightDraftDirty)
            {
                g_lightDraft = current;
                if (g_lightDraft.reflectorEnabled)
                {
                    g_lightDraft.coneEnabled = 1;
                    g_lightDraft.reflectorEnabled = 0;
                    g_lightDraft.reflectorHidden = 0;
                }
            }
        }

        if (ImGui::Button("Add light at camera") && count < kMaxAuthoredLights)
        {
            AuthoredLight light{};
            light.markerId = focusMarker;
            const float pitch = g_snapshot.camera.rotation.x * 0.01745329251994329577f;
            const float yaw = g_snapshot.camera.rotation.z * 0.01745329251994329577f;
            const float cp = std::cos(pitch);
            light.position.x = g_snapshot.camera.position.x - std::sin(yaw) * cp * 4.0f;
            light.position.y = g_snapshot.camera.position.y + std::cos(yaw) * cp * 4.0f;
            light.position.z = g_snapshot.camera.position.z + std::sin(pitch) * 4.0f;
            light.range = 30.0f;
            light.intensity = 75.0f;
            light.colorR = light.colorG = light.colorB = 255;
            light.enabled = 1;
            light.aimPitch = g_snapshot.camera.rotation.x;
            light.aimYaw = g_snapshot.camera.rotation.z;
            light.beamAngle = 35.0f;
            light.coneEnabled = 0;
            light.blend = static_cast<std::uint32_t>(replay::editor::Blend::Spline);
            Command command{};
            command.authoredLight = light;
            if (Send(CommandType::LightAdd, &command))
            {
                g_lightSelected = static_cast<int>(count);
                g_lightDraftIndex = -1;
                g_lightDraftDirty = false;
                PlayUi(UiSound::Select);
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Adds a point light 4 m in front of the current editor camera and binds it to the selected marker.");
        ImGui::SameLine();
        if (ImGui::Button("Duplicate selected") && count > 0 && count < kMaxAuthoredLights)
        {
            AuthoredLight light = g_snapshot.authoredLights[g_lightSelected];
            light.markerId = focusMarker;
            Command command{};
            command.authoredLight = light;
            if (Send(CommandType::LightAdd, &command))
            {
                g_lightSelected = static_cast<int>(count);
                g_lightDraftIndex = -1;
                g_lightDraftDirty = false;
                PlayUi(UiSound::Select);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete selected") && count > 0)
        {
            Command command{};
            command.keyframeId = static_cast<std::uint32_t>(g_lightSelected);
            if (Send(CommandType::LightDelete, &command))
            {
                g_lightSelected = std::max(0, g_lightSelected - 1);
                g_lightDraftIndex = -1;
                g_lightDraftDirty = false;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear all") && count > 0)
        {
            Command command{};
            if (Send(CommandType::LightClear, &command))
            {
                g_lightSelected = 0;
                g_lightDraftIndex = -1;
                g_lightDraftDirty = false;
            }
        }

        ImGui::Spacing();
        ImGui::Text("Lights: %u / %u", count, kMaxAuthoredLights);
        ImGui::BeginChild("##LightList", ImVec2(0.0f, S(180.0f)), true);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const AuthoredLight& light = g_snapshot.authoredLights[i];
            const std::string binding = light.markerId ? markerLabel(light.markerId) : std::string("World");
            char label[112]{};
            std::snprintf(label, sizeof(label), "%sLight %02u  |  %s", light.enabled ? "" : "(off) ", i + 1, binding.c_str());
            if (ImGui::Selectable(label, g_lightSelected == static_cast<int>(i)))
            {
                g_lightSelected = static_cast<int>(i);
                g_lightDraftIndex = -1;
                g_lightDraftDirty = false;
            }
        }
        ImGui::EndChild();
        ImGui::BeginGroup();
        if (count > 0)
        {
            const AuthoredLight& selected = g_snapshot.authoredLights[g_lightSelected];
            ImGui::Text("Editing Light %02d  |  %s", g_lightSelected + 1, markerLabel(selected.markerId).c_str());
            bool changed = false;
            ImGui::TextDisabled("Drag values to adjust; hold Ctrl for finer movement. Double-click any value to type it.");
            const auto dragValue = [&](const char* label, float& value, int field, float speed, float minimum, float maximum, const char* format) {
                if (ImGui::GetIO().KeyCtrl)
                    speed *= 0.1f;
                ImGui::SetNextItemWidth(S(220.0f));
                changed |= ImGui::DragFloat(label, &value, speed, minimum, maximum, format, ImGuiSliderFlags_NoInput);
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    g_lightNumericEditAxis = field;
                    g_lightNumericEditValue = value;
                    g_lightNumericEditFocus = true;
                    ImGui::OpenPopup("##LightExactCoordinate");
                }
            };
            dragValue("X", g_lightDraft.position.x, 0, 0.01f, -10000.0f, 10000.0f, "%.3f");
            dragValue("Y", g_lightDraft.position.y, 1, 0.01f, -10000.0f, 10000.0f, "%.3f");
            dragValue("Z", g_lightDraft.position.z, 2, 0.01f, -10000.0f, 10000.0f, "%.3f");
            dragValue("Range (m)", g_lightDraft.range, 3, 0.05f, 0.5f, 250.0f, "%.2f");
            dragValue("Intensity", g_lightDraft.intensity, 4, 0.02f, 0.0f, 300.0f, "%.2f");
            ImGui::SetNextItemWidth(S(220.0f));
            if (ImGui::Button("Advanced light settings..."))
            {
                g_lightAdvancedOpen = true;
                ImGui::OpenPopup("##LightAdvancedSettings");
            }
            bool beamApprox = g_lightDraft.coneEnabled != 0 || g_lightDraft.reflectorEnabled != 0;
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.035f, 0.031f, 0.027f, g_lightMenuOpacity / 100.0f));
            if (ImGui::BeginPopupModal("##LightAdvancedSettings", &g_lightAdvancedOpen, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text("ADVANCED LIGHT SETTINGS  |  %02d", g_lightSelected + 1);
                ImGui::Separator();
                std::string selectedMarker = markerLabel(g_lightDraft.markerId);
                if (ImGui::BeginCombo("Active on", selectedMarker.c_str()))
                {
                    if (ImGui::Selectable("World (always)", g_lightDraft.markerId == 0))
                    {
                        g_lightDraft.markerId = 0;
                        changed = true;
                    }
                    for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
                    {
                        const CameraKeyframe& marker = g_snapshot.cameraKeyframes[n];
                        const std::string label = markerLabel(marker.id);
                        if (ImGui::Selectable(label.c_str(), g_lightDraft.markerId == marker.id))
                        {
                            g_lightDraft.markerId = marker.id;
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::Checkbox("Beam-like point-light approximation", &beamApprox))
                {
                    g_lightDraft.coneEnabled = beamApprox ? 1u : 0u;
                    g_lightDraft.reflectorEnabled = 0;
                    g_lightDraft.reflectorHidden = 0;
                    changed = true;
                }
                if (beamApprox)
                {
                    ImGui::TextWrapped("Uses stacked point lights. It suggests a directional beam, but some light spills outside it.");
                    dragValue("Aim pitch", g_lightDraft.aimPitch, 5, 0.50f, -180.0f, 180.0f, "%.2f deg");
                    dragValue("Aim yaw", g_lightDraft.aimYaw, 6, 0.50f, -360.0f, 360.0f, "%.2f deg");
                    dragValue("Aim roll", g_lightDraft.aimRoll, 8, 0.50f, -180.0f, 180.0f, "%.2f deg");
                    dragValue("Beam angle", g_lightDraft.beamAngle, 7, 0.50f, 0.0f, 360.0f, "%.2f deg");
                }
                if (ImGui::Button("Done"))
                {
                    g_lightAdvancedOpen = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::PopStyleColor();
            if (g_lightNumericEditAxis >= 0 && ImGui::BeginPopup("##LightExactCoordinate"))
            {
                static constexpr const char* kValueLabels[] = {"X", "Y", "Z", "Range", "Intensity", "Aim pitch", "Aim yaw", "Beam angle", "Aim roll"};
                static constexpr const char* kValueUnits[] = {"metres", "metres", "metres", "metres", "intensity", "degrees", "degrees", "degrees", "degrees"};
                ImGui::Text("Set exact %s (%s)", kValueLabels[g_lightNumericEditAxis], kValueUnits[g_lightNumericEditAxis]);
                ImGui::SetNextItemWidth(S(180.0f));
                if (g_lightNumericEditFocus)
                {
                    ImGui::SetKeyboardFocusHere();
                    g_lightNumericEditFocus = false;
                }
                ImGui::InputFloat("##LightExactValue", &g_lightNumericEditValue, 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_AutoSelectAll);
                const bool submit = KeyPressed(ImGuiKey_Enter, false);
                ImGui::SameLine();
                const bool apply = ImGui::Button("Apply");
                ImGui::SameLine();
                const bool cancel = ImGui::Button("Cancel");
                if ((submit || apply) && std::isfinite(g_lightNumericEditValue))
                {
                    float* values[] = {&g_lightDraft.position.x, &g_lightDraft.position.y, &g_lightDraft.position.z,
                        &g_lightDraft.range, &g_lightDraft.intensity, &g_lightDraft.aimPitch, &g_lightDraft.aimYaw,
                        &g_lightDraft.beamAngle, &g_lightDraft.aimRoll};
                    *values[g_lightNumericEditAxis] = g_lightNumericEditValue;
                    changed = true;
                    g_lightNumericEditAxis = -1;
                    ImGui::CloseCurrentPopup();
                }
                else if (cancel)
                {
                    g_lightNumericEditAxis = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            int red = static_cast<int>(g_lightDraft.colorR), green = static_cast<int>(g_lightDraft.colorG), blue = static_cast<int>(g_lightDraft.colorB);
            const bool colorChanged = ImGui::SliderInt("Red", &red, 0, 255) | ImGui::SliderInt("Green", &green, 0, 255) |
                ImGui::SliderInt("Blue", &blue, 0, 255);
            if (colorChanged)
            {
                g_lightDraft.colorR = static_cast<std::uint32_t>(red);
                g_lightDraft.colorG = static_cast<std::uint32_t>(green);
                g_lightDraft.colorB = static_cast<std::uint32_t>(blue);
                changed = true;
            }
            if (changed)
            {
                Command command{};
                command.keyframeId = static_cast<std::uint32_t>(g_lightSelected);
                command.authoredLight = g_lightDraft;
                g_lightDraftDirty = Send(CommandType::LightUpdate, &command);
            }
        }
        else
            ImGui::TextWrapped("Adds a point light 4 m in front of the camera (30 m range, 75 intensity), active on the selected marker.");
        ImGui::EndGroup();

        if (focusMarker)
        {
            ImGui::Separator();
            ImGui::Text("Copy marker lighting");
            const std::string sourceLabel = g_lightCopySourceMarkerId ? markerLabel(g_lightCopySourceMarkerId) : std::string("No other marker");
            if (ImGui::BeginCombo("Copy from", sourceLabel.c_str()))
            {
                for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
                {
                    const CameraKeyframe& marker = g_snapshot.cameraKeyframes[n];
                    if (marker.id == focusMarker)
                        continue;
                    std::uint32_t lightCount = 0;
                    for (std::uint32_t l = 0; l < count; ++l)
                        lightCount += g_snapshot.authoredLights[l].markerId == marker.id ? 1u : 0u;
                    char label[96]{};
                    std::snprintf(label, sizeof(label), "Marker %u (%u lights)", n + 1, lightCount);
                    if (ImGui::Selectable(label, g_lightCopySourceMarkerId == marker.id))
                        g_lightCopySourceMarkerId = marker.id;
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            std::uint32_t sourceLightCount = 0;
            std::uint32_t destinationLightCount = 0;
            for (std::uint32_t l = 0; l < count; ++l)
            {
                sourceLightCount += g_snapshot.authoredLights[l].markerId == g_lightCopySourceMarkerId ? 1u : 0u;
                destinationLightCount += g_snapshot.authoredLights[l].markerId == focusMarker ? 1u : 0u;
            }
            const bool copyFits = sourceLightCount <= kMaxAuthoredLights - (count - destinationLightCount);
            if (g_lightCopySourceMarkerId && copyFits && ImGui::Button("Replace this marker's lights") && g_lightCopySourceMarkerId != focusMarker)
            {
                Command command{};
                command.keyframeId = g_lightCopySourceMarkerId;
                command.target = focusMarker;
                if (Send(CommandType::LightCopyMarker, &command))
                {
                    g_lightDraftIndex = -1;
                    g_lightDraftDirty = false;
                    PlayUi(UiSound::Select);
                }
            }
            if (g_lightCopySourceMarkerId && !copyFits)
                ImGui::TextDisabled("Not enough free light slots to copy this set.");
            ImGui::TextDisabled("Replaces only this marker's lights; World lights stay as they are.");
        }
        ImGui::EndChild();
    }

    void DrawLights(ImDrawList* draw)
    {
        if (!g_lightMenuOpacityRead)
        {
            g_lightMenuOpacityRead = true;
#ifndef REDEMPTION_UI_HARNESS
            wchar_t folder[MAX_PATH]{};
            GetCurrentDirectoryW(MAX_PATH, folder);
            const std::wstring ini = replay::identity::IniPathInW(folder);
            g_lightMenuOpacity = std::clamp(static_cast<int>(replay::identity::IniIntW(L"LightMenuOpacity", 50, ini.c_str())), 0, 100);
            const int presetVersion = static_cast<int>(replay::identity::IniIntW(L"LightMenuPresetVersion", 0, ini.c_str()));
            int savedPreset = static_cast<int>(replay::identity::IniIntW(L"LightMenuPreset", 3, ini.c_str()));
            if (presetVersion < 2)
            {
                // Before adding Violet Outlaw and Midnight Frontier, index 3 was Original.
                savedPreset = std::clamp(savedPreset, 0, 3);
                if (savedPreset == 3)
                    savedPreset = 5;
                const std::wstring migratedPreset = std::to_wstring(savedPreset);
                replay::identity::IniWriteW(L"LightMenuPreset", migratedPreset.c_str(), ini.c_str());
                replay::identity::IniWriteW(L"LightMenuPresetVersion", L"2", ini.c_str());
            }
            g_lightMenuPreset = std::clamp(savedPreset, 0, 5);
            g_lightPreviewVisible = replay::identity::IniIntW(L"LightPreviewEnabled", 1, ini.c_str()) != 0;
            g_lightPreviewSolo = replay::identity::IniIntW(L"LightPreviewSolo", 0, ini.c_str()) != 0;
            g_lightPreviewWidth = static_cast<float>(std::clamp(static_cast<int>(replay::identity::IniIntW(L"LightPreviewWidth", 460, ini.c_str())), 280, 1200));
            g_lightPreviewHeight = static_cast<float>(std::clamp(static_cast<int>(replay::identity::IniIntW(L"LightPreviewHeight", 360, ini.c_str())), 220, 900));
            g_lightPreviewX = static_cast<float>(replay::identity::IniIntW(L"LightPreviewX", -1, ini.c_str()));
            g_lightPreviewY = static_cast<float>(replay::identity::IniIntW(L"LightPreviewY", -1, ini.c_str()));
#endif
        }

        if (std::strncmp(g_lightRigGroupsClip, g_snapshot.currentClip, sizeof(g_lightRigGroupsClip)) != 0)
        {
            std::snprintf(g_lightRigGroupsClip, sizeof(g_lightRigGroupsClip), "%s", g_snapshot.currentClip);
            LoadLightRigGroups();
            g_lightRigEditDraft = {};
            g_lightRigUngroupUndo = {};
            g_lightRigSelectionPendingAt = 0;
            g_lightSelectedRigGroupId = 0;
            g_lightSelectedId = 0;
            g_lightDraftIndex = -1;
        }

        static constexpr ImU32 kLightPresetAccents[] = {
            IM_COL32(43, 177, 232, 255), // Blueprint: cyan blue from the reference.
            IM_COL32(226, 181, 79, 255), // Frontier Classic.
            IM_COL32(207, 57, 48, 255),  // Crimson Red.
            IM_COL32(145, 77, 191, 255), // Violet Outlaw.
            IM_COL32(47, 102, 177, 255), // Midnight Frontier.
            kGold                        // Original: red brush with classic gold controls.
        };
        static constexpr ImU32 kLightPresetHover[] = {
            IM_COL32(104, 211, 250, 210),
            IM_COL32(255, 220, 142, 210),
            IM_COL32(246, 111, 91, 210),
            IM_COL32(195, 139, 231, 220),
            IM_COL32(115, 163, 225, 220),
            kRowInkGrey
        };
        static constexpr ImU32 kLightPresetOverlay[] = {
            IM_COL32(5, 13, 22, 255),
            IM_COL32(10, 9, 8, 255),
            IM_COL32(16, 8, 9, 255),
            IM_COL32(13, 7, 19, 255),
            IM_COL32(4, 10, 19, 255),
            IM_COL32(10, 9, 8, 255)
        };
        static constexpr ImU32 kLightPresetPanel[] = {
            IM_COL32(9, 18, 28, 255),
            IM_COL32(12, 11, 10, 255),
            IM_COL32(20, 11, 12, 255),
            IM_COL32(21, 12, 29, 255),
            IM_COL32(8, 18, 31, 255),
            IM_COL32(12, 11, 10, 255)
        };
        static constexpr ImU32 kLightPresetPreview[] = {
            IM_COL32(6, 17, 27, 238),
            IM_COL32(20, 16, 12, 238),
            IM_COL32(24, 11, 13, 238),
            IM_COL32(18, 10, 25, 238),
            IM_COL32(7, 15, 29, 238),
            IM_COL32(20, 16, 12, 238)
        };
        static const ImVec4 kLightPresetPopupBackground[] = {
            ImVec4(0.025f, 0.055f, 0.08f, 0.98f),
            ImVec4(0.035f, 0.031f, 0.027f, 0.98f),
            ImVec4(0.075f, 0.028f, 0.03f, 0.98f),
            ImVec4(0.055f, 0.027f, 0.078f, 0.98f),
            ImVec4(0.018f, 0.04f, 0.075f, 0.98f),
            ImVec4(0.035f, 0.031f, 0.027f, 0.98f)
        };
        const ImU32 lightAccent = kLightPresetAccents[g_lightMenuPreset];
        const ImU32 lightHoverInk = kLightPresetHover[g_lightMenuPreset];
        const ImU32 lightBannerAccent = g_lightMenuPreset == 5 ? kRed : lightAccent;
        const ImU32 lightSelectedInk = g_lightMenuPreset == 5 ? kRed : lightAccent;
        const ImVec4 lightPopupBackground = kLightPresetPopupBackground[g_lightMenuPreset];
        const auto lightRowBox = [&](float x0, float y0, float x1, float y1, RowLook look) {
            RowBox(draw, x0, y0, x1, y1, look, g_lightMenuPreset, lightSelectedInk, lightHoverInk);
        };

        const float x = 20.0f;
        const float top = 32.0f;
        const float width = std::min(500.0f, g_vw - 36.0f);
        const float bottom = std::min(g_vh - g_footH - 18.0f, top + 900.0f);
        const int bgAlpha = static_cast<int>(225.0f * g_lightMenuOpacity / 100.0f);
        const ImU32 overlay = WithAlpha(kLightPresetOverlay[g_lightMenuPreset], bgAlpha / 255.0f);
        draw->AddRectFilled(P(0.0f, 0.0f), P(width, g_vh), overlay);
        draw->AddRectFilledMultiColor(P(width, 0.0f), P(width + 160.0f, g_vh), overlay,
            WithAlpha(overlay, 0), WithAlpha(overlay, 0), overlay);
        InkPanel(draw, x + 6.0f, top, x + width - 10.0f, bottom,
            WithAlpha(kLightPresetPanel[g_lightMenuPreset], bgAlpha / 255.0f), 16.0f, false);

        auto markerLabel = [](std::uint32_t markerId) {
            if (!markerId)
                return std::string("World (always)");
            for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
                if (g_snapshot.cameraKeyframes[n].id == markerId)
                {
                    char label[64]{};
                    std::snprintf(label, sizeof(label), "Marker %u", n + 1);
                    return std::string(label);
                }
            return std::string("Missing marker");
        };

        const std::uint32_t focusMarker = CurrentLightMarkerId();
        const std::uint32_t totalCount = std::min(g_snapshot.authoredLightCount, kMaxAuthoredLights);
        std::vector<std::uint32_t> lightIndices;
        lightIndices.reserve(totalCount);
        for (std::uint32_t n = 0; n < totalCount; ++n)
            if (g_snapshot.authoredLights[n].markerId == 0 || g_snapshot.authoredLights[n].markerId == focusMarker)
                lightIndices.push_back(n);
        const std::uint32_t count = static_cast<std::uint32_t>(lightIndices.size());
        if (g_lightLastFocusMarkerId != focusMarker)
        {
            ClearLightDraftAttachSync();
            g_lightLastFocusMarkerId = focusMarker;
            g_lightFocusMarkerId = focusMarker;
            if (g_lightSelectedRigGroupId)
            {
                // The delayed-import grace applies only on the marker where a
                // rig was placed. A marker change must resolve the group against
                // that marker immediately, or stale rig selection can leak into
                // the next Add Light action.
                g_lightRigSelectionPendingAt = 0;
                g_lightSelectedId = 0;
                g_lightSelected = 0;
            }
            else if (g_lightRigSelectionPendingAt)
            {
                // A queued light from the prior marker must not hold selection
                // open on this marker while its snapshot catches up.
                g_lightRigSelectionPendingAt = 0;
                g_lightSelectedId = 0;
                g_lightSelected = 0;
            }
            g_lightDraftIndex = -1;
            g_lightDraftDirty = false;
            g_lightRigEditDraft = {};
            g_lightCopySourceMarkerId = PreviousCameraMarkerId(focusMarker);
        }
        if (!g_lightSplineDefaultsDirty)
            g_lightSplineDefaultsDraft = g_snapshot.splineDefaults;
        else if (replay::camera::spline::Same(g_lightSplineDefaultsDraft, g_snapshot.splineDefaults))
            g_lightSplineDefaultsDirty = false;
        std::vector<LightListRow> lightRows;
        lightRows.reserve(count);
        for (const std::uint32_t sourceIndex : lightIndices)
        {
            const AuthoredLight& light = g_snapshot.authoredLights[sourceIndex];
            const LightRigGroup* owningGroup = nullptr;
            for (const LightRigGroup& group : g_lightRigGroups)
                if (std::find(group.memberIds.begin(), group.memberIds.end(), light.id) != group.memberIds.end())
                {
                    owningGroup = &group;
                    break;
                }
            if (!owningGroup)
            {
                LightListRow row;
                row.representativeIndex = sourceIndex;
                row.sourceIndices.push_back(sourceIndex);
                lightRows.push_back(std::move(row));
                continue;
            }
            auto row = std::find_if(lightRows.begin(), lightRows.end(), [&](const LightListRow& item) {
                return item.rigGroupId == owningGroup->id;
            });
            if (row == lightRows.end())
            {
                LightListRow newRow;
                newRow.rigGroupId = owningGroup->id;
                newRow.representativeIndex = sourceIndex;
                newRow.sourceIndices.push_back(sourceIndex);
                lightRows.push_back(std::move(newRow));
            }
            else
                row->sourceIndices.push_back(sourceIndex);
        }
        const int rowCount = static_cast<int>(lightRows.size());
        bool selectionPending = false;
        if (g_lightSelectedRigGroupId)
        {
            const auto selectedRigRow = std::find_if(lightRows.begin(), lightRows.end(), [](const LightListRow& row) {
                return row.rigGroupId == g_lightSelectedRigGroupId;
            });
            if (selectedRigRow == lightRows.end())
            {
                const bool pendingImport = g_lightRigSelectionPendingAt && GetTickCount64() - g_lightRigSelectionPendingAt < 5000;
                if (!g_lightRigMenuOpen && !pendingImport)
                {
                    g_lightSelectedRigGroupId = 0;
                    g_lightRigEditDraft = {};
                    g_lightSelectedId = 0;
                }
                else if (pendingImport)
                    selectionPending = true;
            }
            else
            {
                g_lightSelected = static_cast<int>(selectedRigRow - lightRows.begin());
                g_lightRigSelectionPendingAt = 0;
            }
        }
        if (g_lightSelectedId && !g_lightSelectedRigGroupId)
        {
            const auto selectedLightRow = std::find_if(lightRows.begin(), lightRows.end(), [&](const LightListRow& row) {
                return std::any_of(row.sourceIndices.begin(), row.sourceIndices.end(), [&](std::uint32_t sourceIndex) {
                    return g_snapshot.authoredLights[sourceIndex].id == g_lightSelectedId;
                });
            });
            if (selectedLightRow != lightRows.end())
            {
                g_lightSelected = static_cast<int>(selectedLightRow - lightRows.begin());
                g_lightRigSelectionPendingAt = 0;
            }
            else if (g_lightRigSelectionPendingAt && GetTickCount64() - g_lightRigSelectionPendingAt < 5000)
                selectionPending = true;
            else
            {
                g_lightSelectedId = 0;
                g_lightRigSelectionPendingAt = 0;
            }
        }
        if (!selectionPending && g_lightSelected >= std::max(1, rowCount))
            g_lightSelected = std::max(0, rowCount - 1);
        const LightListRow* selectedRow = rowCount && !selectionPending ? &lightRows[static_cast<std::size_t>(g_lightSelected)] : nullptr;
        const bool selectedIsRig = selectedRow && selectedRow->rigGroupId != 0;
        if (selectedRow)
            g_lightSelectedRigGroupId = selectedRow->rigGroupId;
        const std::uint32_t selectedSourceIndex = selectedRow ? selectedRow->representativeIndex : 0u;
        if (count == 0)
        {
            g_lightDraftIndex = -1;
            g_lightDraftDirty = false;
            ClearLightDraftAttachSync();
        }
        else if (static_cast<int>(selectedSourceIndex) != g_lightDraftIndex)
        {
            ClearLightDraftAttachSync();
            g_lightDraft = g_snapshot.authoredLights[selectedSourceIndex];
            if (g_lightDraft.reflectorEnabled)
            {
                g_lightDraft.coneEnabled = 1;
                g_lightDraft.reflectorEnabled = 0;
                g_lightDraft.reflectorHidden = 0;
            }
            g_lightDraftIndex = static_cast<int>(selectedSourceIndex);
            g_lightDraftDirty = false;
        }
        else
        {
            const AuthoredLight& current = g_snapshot.authoredLights[selectedSourceIndex];
            if (!SyncLightDraftAttachment(current) && g_lightDraftDirty && current.position.x == g_lightDraft.position.x && current.position.y == g_lightDraft.position.y &&
                current.position.z == g_lightDraft.position.z && current.range == g_lightDraft.range && current.intensity == g_lightDraft.intensity &&
                current.colorR == g_lightDraft.colorR && current.colorG == g_lightDraft.colorG && current.colorB == g_lightDraft.colorB &&
                current.enabled == g_lightDraft.enabled && current.markerId == g_lightDraft.markerId && current.aimPitch == g_lightDraft.aimPitch &&
                current.aimYaw == g_lightDraft.aimYaw && current.beamAngle == g_lightDraft.beamAngle && current.coneEnabled == g_lightDraft.coneEnabled &&
                current.aimRoll == g_lightDraft.aimRoll && current.reflectorEnabled == g_lightDraft.reflectorEnabled &&
                current.reflectorHidden == g_lightDraft.reflectorHidden && current.attachedTarget == g_lightDraft.attachedTarget &&
                current.lookAtTarget == g_lightDraft.lookAtTarget &&
                current.id == g_lightDraft.id && current.attachedBone == g_lightDraft.attachedBone &&
                current.attachType == g_lightDraft.attachType && current.blend == g_lightDraft.blend &&
                replay::camera::spline::Same(current.spline, g_lightDraft.spline) &&
                std::strncmp(current.name, g_lightDraft.name, sizeof(current.name)) == 0)
                g_lightDraftDirty = false;
            if (!g_lightDraftDirty)
            {
                g_lightDraft = current;
                if (g_lightDraft.reflectorEnabled)
                {
                    g_lightDraft.coneEnabled = 1;
                    g_lightDraft.reflectorEnabled = 0;
                    g_lightDraft.reflectorHidden = 0;
                }
            }
        }

        const auto ensureLightRigDraft = [&]() {
            if (!selectedIsRig || !selectedRow)
                return;
            const LightRigGroup* group = FindLightRigGroup(selectedRow->rigGroupId);
            if (!group)
                return;
            if (!g_lightRigEditDraft.valid || g_lightRigEditDraft.groupId != group->id ||
                g_lightRigEditDraft.markerId != focusMarker)
                BeginLightRigEditDraft(*group, *selectedRow, focusMarker);
        };
        const auto applyLightRigDraft = [&]() {
            ensureLightRigDraft();
            if (!g_lightRigEditDraft.valid)
                return false;
            bool queued = true;
            for (const LightRigEditDraft::Member& member : g_lightRigEditDraft.members)
            {
                if (member.sourceIndex >= g_snapshot.authoredLightCount)
                    continue;
                Command command{};
                command.keyframeId = member.sourceIndex;
                command.authoredLight = LightRigEditedMember(g_lightRigEditDraft, member);
                queued = Send(CommandType::LightUpdate, &command) && queued;
            }
            return queued;
        };
        ensureLightRigDraft();
        const auto rowForSourceIndex = [&](std::uint32_t sourceIndex) {
            for (std::size_t rowIndex = 0; rowIndex < lightRows.size(); ++rowIndex)
                if (std::find(lightRows[rowIndex].sourceIndices.begin(), lightRows[rowIndex].sourceIndices.end(), sourceIndex) !=
                    lightRows[rowIndex].sourceIndices.end())
                    return static_cast<int>(rowIndex);
            return -1;
        };

        const auto deleteSelectedLight = [&]() {
            if (!count || !selectedRow || selectionPending)
                return false;
            if (selectedIsRig && selectedRow)
            {
                std::vector<std::uint32_t> deleteIndices = selectedRow->sourceIndices;
                std::sort(deleteIndices.begin(), deleteIndices.end(), std::greater<std::uint32_t>());
                bool queued = true;
                for (const std::uint32_t sourceIndex : deleteIndices)
                {
                    Command command{};
                    command.keyframeId = sourceIndex;
                    queued = Send(CommandType::LightDelete, &command) && queued;
                }
                if (!queued)
                    return false;
                g_lightSelectedRigGroupId = 0;
                g_lightRigEditDraft = {};
                g_lightSelectedId = 0;
                g_lightRigSelectionPendingAt = 0;
                g_lightDraftIndex = -1;
                g_lightDraftDirty = false;
                PlayUi(UiSound::Back);
                return true;
            }
            Command command{};
            command.keyframeId = selectedSourceIndex;
            if (!Send(CommandType::LightDelete, &command))
                return false;
            g_lightSelectedId = 0;
            g_lightRigSelectionPendingAt = 0;
            g_lightSelected = std::max(0, g_lightSelected - 1);
            g_lightDraftIndex = -1;
            g_lightDraftDirty = false;
            PlayUi(UiSound::Back);
            return true;
        };
        bool deletedSelectedLight = false;
        if (count > 0 && selectedRow && !selectionPending && !ImGui::GetIO().WantTextInput && g_lightNumericEditAxis < 0 &&
            KeyPressed(ImGuiKey_Delete, false))
            deletedSelectedLight = deleteSelectedLight();

        const bool lightRigPage = g_lightRigMenuOpen && !g_lightAdvancedOpen && !g_lightSplineOpen;
        const bool lightPresetPage = g_lightSettingsMenuOpen && !g_lightRigMenuOpen && !g_lightAdvancedOpen && !g_lightSplineOpen;
        TitleBanner(draw, x + 10.0f, top, width - 20.0f, 44.0f, 54.0f, x + 20.0f, top + 11.0f, 34.0f,
            g_lightSplineDefaultsOpen ? "LIGHT SPLINE DEFAULTS" : g_lightSplineOpen ? "LIGHT SPLINE OPTIONS" :
            g_lightAdvancedOpen ? "ADVANCED LIGHT SETTINGS" : lightRigPage ? "LIGHT RIG LIBRARY" :
            lightPresetPage ? "LIGHT MENU LOOKS" : "LIGHTING", lightBannerAccent, ui_art::Art::ToolLantern);
        char countLabel[32]{};
        if (lightPresetPage)
            std::snprintf(countLabel, sizeof(countLabel), "LOOK %d / 6", g_lightMenuPreset + 1);
        else
            std::snprintf(countLabel, sizeof(countLabel), "%u / %u HERE", count, totalCount);
        TextFit(draw, x + width - 174.0f, top + 19.0f, 14.0f, kText, countLabel, 120.0f);
        if (!g_lightAdvancedOpen && !g_lightSplineOpen && !g_lightSettingsMenuOpen)
        {
            const float gearX = x + width - 50.0f, gearY = top + 10.0f;
            ImGui::SetCursorScreenPos(P(gearX, gearY));
            ImGui::InvisibleButton("##LightingSettingsGear", ImVec2(S(34.0f), S(34.0f)));
            const bool gearHovered = ImGui::IsItemHovered();
            if (gearHovered)
                ImGui::SetTooltip("Settings");
            const ImU32 gearColor = gearHovered ? lightAccent : kText;
            const ImVec2 gearCenter = P(gearX + 17.0f, gearY + 17.0f);
            const float gearRadius = S(7.0f), toothRadius = S(11.0f);
            for (int tooth = 0; tooth < 8; ++tooth)
            {
                const float angle = tooth * 0.7853981633974483f;
                const ImVec2 inner(gearCenter.x + std::cos(angle) * gearRadius, gearCenter.y + std::sin(angle) * gearRadius);
                const ImVec2 outer(gearCenter.x + std::cos(angle) * toothRadius, gearCenter.y + std::sin(angle) * toothRadius);
                draw->AddLine(inner, outer, gearColor, S(2.4f));
            }
            draw->AddCircle(gearCenter, gearRadius, gearColor, 16, S(2.0f));
            draw->AddCircleFilled(gearCenter, S(2.2f), gearColor);
            if (gearHovered && MouseClicked())
            {
                g_lightSettingsMenuOpen = true;
                g_lightRigMenuOpen = false;
                PlayUi(UiSound::Select);
            }
        }
        OrnamentDivider(draw, x + 20.0f, top + 68.0f, width - 40.0f, lightAccent);

        const auto hit = [](const char* id, float left, float y, float w, float h) {
            ImGui::SetCursorScreenPos(P(left, y));
            ImGui::InvisibleButton(id, ImVec2(S(w), S(h)));
            return ImGui::IsItemHovered() && MouseClicked();
        };
        const auto button = [&](const char* label, const char* id, float left, float y, float w, float h, bool active = true) {
            ImGui::SetCursorScreenPos(P(left, y));
            ImGui::InvisibleButton(id, ImVec2(S(w), S(h)));
            const bool hovered = ImGui::IsItemHovered();
            const bool clicked = active && hovered && MouseClicked();
            lightRowBox(left, y, left + w, y + h, clicked ? RowLook::Selected : HoverShown(hovered) ? RowLook::Hovered : RowLook::Rest);
            const ImU32 color = active ? kText : kMuted;
            const float textW = TextWidth(15.0f, label) / g_scale;
            Text(draw, left + std::max(8.0f, (w - textW) * 0.5f), y + (h - 17.0f) * 0.5f, 15.0f, color, label);
            return clicked;
        };
        const auto saveLightPreviewSettings = [&]() {
#ifndef REDEMPTION_UI_HARNESS
            wchar_t folder[MAX_PATH]{};
            GetCurrentDirectoryW(MAX_PATH, folder);
            const std::wstring ini = replay::identity::IniPathInW(folder);
            const std::wstring enabled = std::to_wstring(g_lightPreviewVisible ? 1 : 0);
            const std::wstring solo = std::to_wstring(g_lightPreviewSolo ? 1 : 0);
            const std::wstring previewWidth = std::to_wstring(static_cast<int>(std::lround(g_lightPreviewWidth)));
            const std::wstring previewHeight = std::to_wstring(static_cast<int>(std::lround(g_lightPreviewHeight)));
            const std::wstring previewX = std::to_wstring(static_cast<int>(std::lround(g_lightPreviewX)));
            const std::wstring previewY = std::to_wstring(static_cast<int>(std::lround(g_lightPreviewY)));
            replay::identity::IniWriteW(L"LightPreviewEnabled", enabled.c_str(), ini.c_str());
            replay::identity::IniWriteW(L"LightPreviewSolo", solo.c_str(), ini.c_str());
            replay::identity::IniWriteW(L"LightPreviewWidth", previewWidth.c_str(), ini.c_str());
            replay::identity::IniWriteW(L"LightPreviewHeight", previewHeight.c_str(), ini.c_str());
            replay::identity::IniWriteW(L"LightPreviewX", previewX.c_str(), ini.c_str());
            replay::identity::IniWriteW(L"LightPreviewY", previewY.c_str(), ini.c_str());
#endif
        };
        const auto toggle = [&](const char* label, bool& value, const char* id, float left, float y, bool active = true, float hitWidth = -1.0f) {
            const bool clicked = hit(id, left, y, hitWidth > 0.0f ? hitWidth : width - 44.0f, 28.0f);
            if (clicked && active)
                value = !value;
            const ImU32 edge = value ? lightAccent : kMuted;
            draw->AddRect(P(left, y + 5.0f), P(left + 18.0f, y + 23.0f), edge, 1.0f, 0, S(1.3f));
            if (value)
            {
                // Draw the mark as strokes; the bundled game font renders the
                // Unicode check glyph as a question mark on some installations.
                draw->AddLine(P(left + 4.0f, y + 14.0f), P(left + 8.0f, y + 18.0f), lightAccent, S(2.0f));
                draw->AddLine(P(left + 8.0f, y + 18.0f), P(left + 15.0f, y + 9.0f), lightAccent, S(2.0f));
            }
            Text(draw, left + 28.0f, y + 3.0f, 16.0f, active ? kText : kMuted, label);
            return clicked && active;
        };

        bool changed = false;
        const auto splineValueRow = [&](const char* label, const std::string& value, const char* id,
            int rowIndex, float y, bool active, const auto& step) {
            const float left = x + 28.0f, rowWidth = width - 56.0f, rowHeight = 34.0f;
            ImGui::SetCursorScreenPos(P(left, y));
            ImGui::InvisibleButton(id, ImVec2(S(rowWidth), S(rowHeight)));
            const bool hovered = ImGui::IsItemHovered();
            const bool clicked = active && hovered && MouseClicked();
            const RowLook look = clicked || g_lightSplineSelectedRow == rowIndex ? RowLook::Selected
                : HoverShown(hovered) ? RowLook::Hovered : RowLook::Rest;
            lightRowBox(left, y, left + rowWidth, y + rowHeight, look);
            TextFit(draw, left + 12.0f, y + 9.0f, 14.0f, active ? kText : kMuted, label, rowWidth * 0.53f);
            const float valueLeft = left + rowWidth - 188.0f;
            Text(draw, valueLeft, y + 8.0f, 15.0f, active ? kText : kMuted, "<");
            TextFit(draw, valueLeft + 22.0f, y + 8.0f, 14.0f, active ? lightAccent : kMuted, value.c_str(), 132.0f);
            Text(draw, left + rowWidth - 22.0f, y + 8.0f, 15.0f, active ? kText : kMuted, ">");
            if (clicked)
            {
                g_lightSplineSelectedRow = rowIndex;
                const float mouseX = (ImGui::GetIO().MousePos.x - P(left, y).x) / g_scale;
                const float leftArrow = valueLeft - left;
                const float rightArrow = rowWidth - 22.0f;
                if (mouseX >= rightArrow - 18.0f)
                    step(1);
                else if (mouseX >= leftArrow - 12.0f && mouseX <= leftArrow + 24.0f)
                    step(-1);
            }
        };
        const auto splineActionRow = [&](const char* label, const char* id, int rowIndex, float y,
            bool active, const auto& action) {
            const float left = x + 28.0f, rowWidth = width - 56.0f, rowHeight = 34.0f;
            ImGui::SetCursorScreenPos(P(left, y));
            ImGui::InvisibleButton(id, ImVec2(S(rowWidth), S(rowHeight)));
            const bool hovered = ImGui::IsItemHovered();
            const bool clicked = active && hovered && MouseClicked();
            lightRowBox(left, y, left + rowWidth, y + rowHeight,
                clicked || g_lightSplineSelectedRow == rowIndex ? RowLook::Selected : HoverShown(hovered) ? RowLook::Hovered : RowLook::Rest);
            Text(draw, left + 12.0f, y + 9.0f, 14.0f, active ? kText : kMuted, label);
            if (clicked)
            {
                g_lightSplineSelectedRow = rowIndex;
                action();
            }
        };
        const auto markerSplineFallback = [&]() {
            const SplineTuning defaults = g_lightSplineDefaultsDirty ? g_lightSplineDefaultsDraft : g_snapshot.splineDefaults;
            for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
                if (g_snapshot.cameraKeyframes[n].id == g_lightDraft.markerId)
                    return replay::camera::spline::Effective(g_snapshot.cameraKeyframeSpline[n], defaults);
            return replay::camera::spline::Clamped(defaults);
        };
        const auto splinePacingName = [](std::uint8_t pacing) {
            static constexpr const char* names[] = {"Natural", "Continuous", "Per Segment"};
            return std::string(names[std::min<std::uint8_t>(pacing, kSplinePacingCount - 1)]);
        };
        const auto stepPacing = [](SplineTuning& tuning, bool perLight, int direction) {
            const int states = perLight ? static_cast<int>(kSplinePacingCount) + 1 : static_cast<int>(kSplinePacingCount);
            const int at = perLight ? ((tuning.overrides & SplineOverridePacing) ? tuning.pacing + 1 : 0) : tuning.pacing;
            const int next = (at + (direction > 0 ? 1 : states - 1)) % states;
            if (perLight && next == 0)
                tuning.overrides = static_cast<std::uint8_t>(tuning.overrides & ~SplineOverridePacing);
            else
            {
                tuning.pacing = static_cast<std::uint8_t>(perLight ? next - 1 : next);
                if (perLight)
                    tuning.overrides = static_cast<std::uint8_t>(tuning.overrides | SplineOverridePacing);
            }
        };
        const auto stepSplineNumber = [](SplineTuning& tuning, const SplineTuning& fallback,
            float SplineTuning::*field, std::uint8_t bit, bool perLight, int direction) {
            const float current = perLight && !(tuning.overrides & bit) ? fallback.*field : tuning.*field;
            tuning.*field = std::clamp(std::round((current + direction * 0.05f) * 100.0f) / 100.0f, 0.0f, 1.0f);
            if (perLight)
                tuning.overrides = static_cast<std::uint8_t>(tuning.overrides | bit);
        };
        const auto stepSplineChannel = [](SplineTuning& tuning, std::uint8_t channel, std::uint8_t bit,
            bool perLight, int direction) {
            const bool overridden = (tuning.overrides & bit) != 0;
            const bool isOn = (tuning.channels & channel) != 0;
            const int states = perLight ? 3 : 2;
            int at = perLight ? (overridden ? (isOn ? 1 : 2) : 0) : (isOn ? 0 : 1);
            at = (at + (direction > 0 ? 1 : states - 1)) % states;
            if (perLight && at == 0)
            {
                tuning.overrides = static_cast<std::uint8_t>(tuning.overrides & ~bit);
                return;
            }
            const bool wantOn = perLight ? at == 1 : at == 0;
            tuning.channels = static_cast<std::uint8_t>(wantOn ? tuning.channels | channel : tuning.channels & ~channel);
            if (perLight)
                tuning.overrides = static_cast<std::uint8_t>(tuning.overrides | bit);
        };
        const auto shownSplineNumber = [](float value, bool inherited) {
            char text[24]{};
            std::snprintf(text, sizeof(text), inherited ? "(%.2f)" : "%.2f", value);
            return std::string(text);
        };
        const auto shownSplineChannel = [](bool on, bool inherited) {
            const char* state = on ? "On" : "Off";
            return inherited ? std::string("(") + state + ")" : state;
        };
        const auto sendSplineDefaults = [&](const auto& edit) {
            SplineTuning defaults = g_lightSplineDefaultsDirty ? g_lightSplineDefaultsDraft : g_snapshot.splineDefaults;
            edit(defaults);
            defaults.overrides = 0;
            Command command{};
            command.spline = defaults;
            if (Send(CommandType::SetSplineDefaults, &command))
            {
                g_lightSplineDefaultsDraft = defaults;
                g_lightSplineDefaultsDirty = true;
            }
        };
        if (g_lightAdvancedOpen && g_lightSplineOpen)
        {
            const bool editingDefaults = g_lightSplineDefaultsOpen;
            const SplineTuning fallback = markerSplineFallback();
            const SplineTuning own = g_lightDraft.spline;
            const SplineTuning shown = editingDefaults
                ? (g_lightSplineDefaultsDirty ? g_lightSplineDefaultsDraft : g_snapshot.splineDefaults)
                : replay::camera::spline::Effective(own, fallback);
            SectionTitle(draw, x + 22.0f, top + 84.0f, width - 44.0f,
                editingDefaults ? "SPLINE DEFAULTS" : "LIGHT SPLINE OPTIONS", lightAccent);
            TextFit(draw, x + 28.0f, top + 105.0f, 12.0f, kMuted,
                editingDefaults ? "Defaults are shared with camera spline settings." :
                    "Parenthesized values follow this marker's camera spline settings.", width - 56.0f);
            const float rowY = top + 124.0f;
            const float rowStep = 39.0f;
            const auto addRows = [&](bool perLight) {
                int row = 0;
                int rowIndex = row++;
                splineValueRow("SPEED PROFILE", (perLight && !(own.overrides & SplineOverridePacing)
                        ? "(" + splinePacingName(shown.pacing) + ")" : splinePacingName(shown.pacing)),
                    "##LightSplinePacing", rowIndex, rowY + rowIndex * rowStep, count > 0 || editingDefaults,
                    [&](int direction) {
                        if (perLight) { stepPacing(g_lightDraft.spline, true, direction); changed = true; }
                        else sendSplineDefaults([&](SplineTuning& t) { stepPacing(t, false, direction); });
                    });
                const auto addNumber = [&](const char* label, const char* id, float SplineTuning::*field,
                    std::uint8_t bit) {
                    const bool inherited = perLight && !(own.overrides & bit);
                    const std::string value = shownSplineNumber(shown.*field, inherited);
                    const int index = row++;
                    splineValueRow(label, value, id, index, rowY + index * rowStep, count > 0 || editingDefaults,
                        [&](int direction) {
                            if (perLight) { stepSplineNumber(g_lightDraft.spline, fallback, field, bit, true, direction); changed = true; }
                            else sendSplineDefaults([&](SplineTuning& t) { stepSplineNumber(t, t, field, bit, false, direction); });
                        });
                };
                addNumber("CURVE SHAPE", "##LightSplineAlpha", &SplineTuning::alpha, SplineOverrideAlpha);
                addNumber("EASE IN", "##LightSplineEaseIn", &SplineTuning::easeIn, SplineOverrideEaseIn);
                addNumber("EASE OUT", "##LightSplineEaseOut", &SplineTuning::easeOut, SplineOverrideEaseOut);
                addNumber("LIGHT WEIGHT", "##LightSplineWeight", &SplineTuning::weight, SplineOverrideWeight);
                const auto addChannel = [&](const char* label, const char* id, std::uint8_t channel, std::uint8_t bit) {
                    const bool inherited = perLight && !(own.overrides & bit);
                    const std::string value = shownSplineChannel((shown.channels & channel) != 0, inherited);
                    const int index = row++;
                    splineValueRow(label, value, id, index, rowY + index * rowStep, count > 0 || editingDefaults,
                        [&](int direction) {
                            if (perLight) { stepSplineChannel(g_lightDraft.spline, channel, bit, true, direction); changed = true; }
                            else sendSplineDefaults([&](SplineTuning& t) { stepSplineChannel(t, channel, bit, false, direction); });
                        });
                };
                addChannel("SPLINE POSITION", "##LightSplinePosition", SplineChannelPosition, SplineOverridePosition);
                addChannel("SPLINE ROTATION", "##LightSplineRotation", SplineChannelRotation, SplineOverrideRotation);
                addChannel("SPLINE LIGHT OUTPUT", "##LightSplineOutput", SplineChannelFov, SplineOverrideFov);
                return row;
            };
            const int rows = addRows(!editingDefaults);
            const float actionY = rowY + rows * rowStep + 4.0f;
            if (editingDefaults)
            {
                splineActionRow("RESET DEFAULTS", "##LightSplineResetDefaults", 8, actionY, true, [&]() {
                    sendSplineDefaults([](SplineTuning& t) { t = DefaultSplineTuning(); });
                });
                if (button("BACK TO LIGHT SPLINE OPTIONS", "##LightSplineDefaultsBack", x + 28.0f,
                        actionY + 41.0f, width - 56.0f, 34.0f))
                {
                    g_lightSplineDefaultsOpen = false;
                    PlayUi(UiSound::Back);
                }
            }
            else
            {
                splineActionRow("USE DEFAULTS", "##LightSplineUseDefaults", 8, actionY, count > 0, [&]() {
                    g_lightDraft.spline.overrides = 0;
                    changed = true;
                });
                splineActionRow("SPLINE DEFAULTS  >", "##LightSplineDefaultsOpen", 9, actionY + 39.0f,
                    true, [&]() { g_lightSplineDefaultsOpen = true; });
                if (button("BACK TO ADVANCED SETTINGS", "##LightSplineBack", x + 28.0f,
                        actionY + 78.0f, width - 56.0f, 34.0f))
                {
                    g_lightSplineOpen = false;
                    g_lightSplineDefaultsOpen = false;
                    PlayUi(UiSound::Back);
                }
            }
        }
        else if (g_lightAdvancedOpen && selectedIsRig && selectedRow)
        {
            ensureLightRigDraft();
            const LightRigGroup* group = FindLightRigGroup(g_lightSelectedRigGroupId);
            SectionTitle(draw, x + 22.0f, top + 84.0f, width - 44.0f, "EDIT LIGHT RIG", lightAccent);
            char groupHeading[128]{};
            std::snprintf(groupHeading, sizeof(groupHeading), "%s  |  %u LIGHTS",
                group && group->displayName[0] ? group->displayName : "Light rig",
                static_cast<unsigned>(g_lightRigEditDraft.members.size()));
            TextFit(draw, x + 28.0f, top + 108.0f, 15.0f, kText, groupHeading, width - 56.0f);
            TextFit(draw, x + 28.0f, top + 130.0f, 11.0f, kMuted,
                (std::string("SAVED RIG: ") + (group ? group->savedName : "")).c_str(), width - 56.0f);
            bool groupChanged = false;
            const auto groupSlider = [&](const char* label, const char* id, float& value,
                float lo, float hi, float perPixel, const char* format, float y) {
                const float left = x + 28.0f, w = width - 56.0f;
                ImGui::SetCursorScreenPos(P(left, y));
                ImGui::InvisibleButton(id, ImVec2(S(w), S(40.0f)));
                const bool hovered = ImGui::IsItemHovered();
                const bool active = ImGui::IsItemActive();
                lightRowBox(left, y, left + w, y + 40.0f,
                    active ? RowLook::Selected : HoverShown(hovered) ? RowLook::Hovered : RowLook::Rest);
                if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                {
                    value = std::clamp(value + ImGui::GetIO().MouseDelta.x * perPixel *
                        (ImGui::GetIO().KeyCtrl ? 0.1f : 1.0f), lo, hi);
                    groupChanged = true;
                }
                Text(draw, left + 12.0f, y + 3.0f, 13.0f, kText, label);
                const float barL = left + 12.0f, barR = left + w - 88.0f, barY = y + 30.0f;
                draw->AddRectFilled(P(barL, barY - 2.5f), P(barR, barY + 2.5f), IM_COL32(236, 230, 218, 60));
                const float t = std::clamp((value - lo) / (hi - lo), 0.0f, 1.0f);
                draw->AddRectFilled(P(barL, barY - 2.5f), P(barL + (barR - barL) * t, barY + 2.5f), lightAccent);
                draw->AddCircleFilled(P(barL + (barR - barL) * t, barY), S(5.0f), kText);
                char shown[48]{};
                std::snprintf(shown, sizeof(shown), format, value);
                TextFit(draw, barR + 8.0f, y + 3.0f, 13.0f, hovered ? lightAccent : kText, shown, 72.0f);
            };
            SectionTitle(draw, x + 22.0f, top + 150.0f, width - 44.0f, "ROTATE THE WHOLE RIG", lightAccent);
            groupSlider("Pitch", "##RigAdvancedPitch", g_lightRigEditDraft.pitch, -180.0f, 180.0f, 0.2f, "%.1f deg", top + 180.0f);
            groupSlider("Roll", "##RigAdvancedRoll", g_lightRigEditDraft.roll, -180.0f, 180.0f, 0.2f, "%.1f deg", top + 222.0f);
            groupSlider("Yaw", "##RigAdvancedYaw", g_lightRigEditDraft.yaw, -180.0f, 180.0f, 0.2f, "%.1f deg", top + 264.0f);

            int bindingIndex = 0;
            for (std::uint32_t i = 0; i < g_snapshot.cameraKeyframeCount; ++i)
                if (g_snapshot.cameraKeyframes[i].id == g_lightRigEditDraft.targetMarkerId)
                    bindingIndex = static_cast<int>(i) + 1;
            Text(draw, x + 28.0f, top + 335.0f, 14.0f, kText, "ACTIVE ON");
            if (button("<", "##RigBindingLeft", x + 145.0f, top + 327.0f, 42.0f, 36.0f,
                    bindingIndex > 0))
            {
                --bindingIndex;
                g_lightRigEditDraft.targetMarkerId = bindingIndex == 0 ? 0u : g_snapshot.cameraKeyframes[bindingIndex - 1].id;
                groupChanged = true;
            }
            if (button(">", "##RigBindingRight", x + width - 70.0f, top + 327.0f, 42.0f, 36.0f,
                    bindingIndex < static_cast<int>(g_snapshot.cameraKeyframeCount)))
            {
                ++bindingIndex;
                g_lightRigEditDraft.targetMarkerId = bindingIndex == 0 ? 0u : g_snapshot.cameraKeyframes[bindingIndex - 1].id;
                groupChanged = true;
            }
            const std::string activeOn = g_lightRigEditDraft.targetMarkerId == 0 ? std::string("World (always)") :
                markerLabel(g_lightRigEditDraft.targetMarkerId);
            TextFit(draw, x + 198.0f, top + 335.0f, 14.0f, lightAccent, activeOn.c_str(), 190.0f);
            TextFit(draw, x + 28.0f, top + 374.0f, 11.0f, kMuted,
                "Group edits keep each light's relative position and range/intensity offset.", width - 56.0f);
            Text(draw, x + 28.0f, top + 429.0f, 14.0f, kText, "LIGHT BLEND");
            if (button("<", "##RigBlendLeft", x + 145.0f, top + 421.0f, 42.0f, 36.0f))
            {
                g_lightRigEditDraft.blend = CycleLightBlend(g_lightRigEditDraft.blend, -1);
                g_lightRigEditDraft.blendChanged = true;
                groupChanged = true;
            }
            if (button(">", "##RigBlendRight", x + width - 70.0f, top + 421.0f, 42.0f, 36.0f))
            {
                g_lightRigEditDraft.blend = CycleLightBlend(g_lightRigEditDraft.blend, 1);
                g_lightRigEditDraft.blendChanged = true;
                groupChanged = true;
            }
            TextFit(draw, x + 198.0f, top + 429.0f, 14.0f, lightAccent,
                LightBlendLabel(g_lightRigEditDraft.blend), width - 286.0f);
            if (groupChanged && !applyLightRigDraft())
                std::snprintf(g_lightRigStatus, sizeof(g_lightRigStatus), "Could not update every light in this rig.");
            const float backY = std::min(top + 706.0f, bottom - 96.0f);
            if (button("BACK TO LIGHTS", "##LightAdvancedBack", x + 28.0f, backY, width - 56.0f, 36.0f))
            {
                g_lightAdvancedOpen = false;
                g_lightSplineOpen = false;
                g_lightSplineDefaultsOpen = false;
                PlayUi(UiSound::Back);
            }
        }
        else if (g_lightAdvancedOpen)
        {
            SectionTitle(draw, x + 22.0f, top + 84.0f, width - 44.0f, "ROTATION AND BEAM", lightAccent);
            Text(draw, x + 28.0f, top + 117.0f, 16.0f, kText, "Aim direction and beam spread");
            bool beam = g_lightDraft.coneEnabled != 0 || g_lightDraft.reflectorEnabled != 0;
            if (toggle("Beam-like point-light approximation", beam, "##LightBeamEnabled", x + 28.0f, top + 147.0f, count > 0))
            {
                g_lightDraft.coneEnabled = beam ? 1u : 0u;
                g_lightDraft.reflectorEnabled = 0;
                g_lightDraft.reflectorHidden = 0;
                changed = true;
            }
            if (beam)
            {
                Text(draw, x + 28.0f, top + 186.0f, 14.0f, kText, "LOOK AT CHARACTER");
                std::vector<std::uint32_t> lookAtTargets{0u};
                for (std::uint32_t n = 0; n < g_snapshot.targetCount; ++n)
                    if (g_snapshot.targets[n].kind == 0u)
                        lookAtTargets.push_back(g_snapshot.targets[n].id);
                int lookAtIndex = 0;
                for (std::size_t n = 1; n < lookAtTargets.size(); ++n)
                    if (lookAtTargets[n] == g_lightDraft.lookAtTarget)
                        lookAtIndex = static_cast<int>(n);
                const auto stepLookAt = [&](int direction) {
                    if (lookAtTargets.size() <= 1)
                        return;
                    const int next = (lookAtIndex + direction + static_cast<int>(lookAtTargets.size())) %
                        static_cast<int>(lookAtTargets.size());
                    g_lightDraft.lookAtTarget = lookAtTargets[static_cast<std::size_t>(next)];
                    changed = true;
                };
                if (button("<", "##LightLookAtLeft", x + 145.0f, top + 178.0f, 38.0f, 30.0f,
                        count > 0 && lookAtTargets.size() > 1))
                    stepLookAt(-1);
                if (button(">", "##LightLookAtRight", x + width - 66.0f, top + 178.0f, 38.0f, 30.0f,
                        count > 0 && lookAtTargets.size() > 1))
                    stepLookAt(1);
                std::string lookAtLabel = "Manual aim";
                if (g_lightDraft.lookAtTarget)
                {
                    lookAtLabel = "Character unavailable";
                    for (std::uint32_t n = 0; n < g_snapshot.targetCount; ++n)
                        if (g_snapshot.targets[n].id == g_lightDraft.lookAtTarget && g_snapshot.targets[n].kind == 0u)
                        {
                            lookAtLabel = g_snapshot.targets[n].label;
                            break;
                        }
                }
                if (button(lookAtLabel.c_str(), "##LightLookAtPicker", x + 192.0f, top + 178.0f,
                        width - 286.0f, 30.0f, count > 0))
                {
                    g_lightTargetPickerMode = 0;
                    g_lightTargetPickerOpen = true;
                    g_lightTargetSearch[0] = '\0';
                }
            }
            if (button(g_lightGizmosVisibleUi ? "LIGHT GIZMOS: ON" : "LIGHT GIZMOS: OFF", "##LightGizmoAdvanced",
                    x + 28.0f, top + 218.0f, width - 56.0f, 34.0f))
            {
                g_lightGizmosVisibleUi = !g_lightGizmosVisibleUi;
                PlayUi(UiSound::Select);
            }

            const auto targetLabel = [&]() {
                if (!g_lightDraft.attachedTarget)
                    return std::string("World space");
                for (std::uint32_t n = 0; n < g_snapshot.targetCount; ++n)
                    if (g_snapshot.targets[n].id == g_lightDraft.attachedTarget)
                        return std::string(g_snapshot.targets[n].label);
                return std::string("Character unavailable");
            };
            Text(draw, x + 28.0f, top + 270.0f, 15.0f, kText, "ATTACH TO");
            const auto stepTarget = [&](int direction) {
                std::vector<std::uint32_t> targets{0u};
                for (std::uint32_t n = 0; n < g_snapshot.targetCount; ++n)
                    if (g_snapshot.targets[n].kind == 0u)
                        targets.push_back(g_snapshot.targets[n].id);
                if (targets.size() <= 1)
                    return;
                auto current = std::find(targets.begin(), targets.end(), g_lightDraft.attachedTarget);
                int index = current == targets.end() ? 0 : static_cast<int>(current - targets.begin());
                index = (index + direction + static_cast<int>(targets.size())) % static_cast<int>(targets.size());
                g_lightDraft.attachedTarget = targets[index];
                BeginLightDraftAttachSync();
                changed = true;
            };
            if (button("<", "##LightAttachLeft", x + 145.0f, top + 263.0f, 40.0f, 34.0f,
                    count > 0 && g_snapshot.targetCount > 0))
                stepTarget(-1);
            if (button(">", "##LightAttachRight", x + width - 72.0f, top + 263.0f, 40.0f, 34.0f,
                    count > 0 && g_snapshot.targetCount > 0))
                stepTarget(1);
            const std::string attachedLabel = targetLabel();
            if (button(attachedLabel.c_str(), "##LightAttachPicker", x + 194.0f, top + 263.0f,
                    width - 295.0f, 34.0f, count > 0))
            {
                g_lightTargetPickerMode = 1;
                g_lightTargetPickerOpen = true;
                g_lightTargetSearch[0] = '\0';
            }
            Text(draw, x + 28.0f, top + 294.0f, 13.0f, kMuted,
                g_lightDraft.attachedTarget ? "Offsets are local to this character; mount and bone can be changed below." :
                    "World space: mount type and bone do not apply.");

            const char* mountNames[] = {"Rigid", "Default", "Flexible"};
            const std::uint32_t mountValue = std::min(g_lightDraft.attachType, 2u);
            Text(draw, x + 28.0f, top + 321.0f, 14.0f, kText, "MOUNT TYPE");
            if (button("<", "##LightMountLeft", x + 145.0f, top + 314.0f, 38.0f, 30.0f, count > 0 && g_lightDraft.attachedTarget))
            {
                g_lightDraft.attachType = (mountValue + 2u) % 3u;
                changed = true;
            }
            if (button(">", "##LightMountRight", x + width - 66.0f, top + 314.0f, 38.0f, 30.0f,
                    count > 0 && g_lightDraft.attachedTarget))
            {
                g_lightDraft.attachType = (mountValue + 1u) % 3u;
                changed = true;
            }
            TextFit(draw, x + 192.0f, top + 321.0f, 14.0f, lightAccent, mountNames[mountValue], width - 286.0f);

            int boneTargetSlot = -1;
            for (std::uint32_t n = 0; n < g_snapshot.targetCount; ++n)
                if (g_snapshot.targets[n].id == g_lightDraft.attachedTarget)
                {
                    boneTargetSlot = static_cast<int>(n);
                    break;
                }
            std::uint64_t availableBoneMask = 0;
            std::uint32_t targetBoneCount = 0;
            if (boneTargetSlot >= 0)
            {
                const auto slot = static_cast<std::uint32_t>(boneTargetSlot);
                availableBoneMask = g_snapshot.targetBoneMask[slot] |
                    (static_cast<std::uint64_t>(g_snapshot.targetBoneMaskHigh[slot]) << 32);
                targetBoneCount = g_snapshot.targetBoneCount[slot];
            }
            const auto lightBoneLabel = [&]() {
                const std::uint32_t ref = g_lightDraft.attachedBone;
                if (!ref)
                    return std::string("Root");
                if (replay::camera::bones::ModeOf(ref) == replay::camera::bones::kModeNamed)
                {
                    const std::uint32_t index = replay::camera::bones::ValueOf(ref);
                    if (index < replay::camera::bones::kNamedCount)
                        return std::string(TR(replay::camera::bones::kNamed[index].key,
                            replay::camera::bones::kNamed[index].label));
                }
                if (replay::camera::bones::ModeOf(ref) == replay::camera::bones::kModeIndex)
                    return std::string("Bone #") + std::to_string(replay::camera::bones::ValueOf(ref));
                return std::string("Root");
            };
            Text(draw, x + 28.0f, top + 356.0f, 14.0f, kText, "BONE");
            if (button("<", "##LightBoneLeft", x + 145.0f, top + 349.0f, 38.0f, 30.0f,
                    count > 0 && g_lightDraft.attachedTarget))
            {
                g_lightDraft.attachedBone = replay::camera::bones::Step(g_lightDraft.attachedBone, -1,
                    availableBoneMask, targetBoneCount);
                changed = true;
            }
            if (button(">", "##LightBoneRight", x + width - 66.0f, top + 349.0f, 38.0f, 30.0f,
                    count > 0 && g_lightDraft.attachedTarget))
            {
                g_lightDraft.attachedBone = replay::camera::bones::Step(g_lightDraft.attachedBone, 1,
                    availableBoneMask, targetBoneCount);
                changed = true;
            }
            const std::string boneLabel = lightBoneLabel();
            TextFit(draw, x + 192.0f, top + 356.0f, 14.0f, lightAccent, boneLabel.c_str(), width - 286.0f);

            Text(draw, x + 28.0f, top + 391.0f, 14.0f, kText, "LIGHT BLEND");
            if (button("<", "##LightBlendLeft", x + 145.0f, top + 383.0f, 42.0f, 32.0f, count > 0))
            {
                g_lightDraft.blend = CycleLightBlend(g_lightDraft.blend, -1);
                changed = true;
            }
            if (button(">", "##LightBlendRight", x + width - 70.0f, top + 383.0f, 42.0f, 32.0f, count > 0))
            {
                g_lightDraft.blend = CycleLightBlend(g_lightDraft.blend, 1);
                changed = true;
            }
            TextFit(draw, x + 192.0f, top + 391.0f, 14.0f, lightAccent,
                LightBlendLabel(g_lightDraft.blend), width - 286.0f);
            if (button("SPLINE OPTIONS  >", "##LightSplineOpen", x + 28.0f, top + 424.0f, width - 56.0f, 34.0f, count > 0))
            {
                g_lightSplineOpen = true;
                g_lightSplineDefaultsOpen = false;
                g_lightSplineSelectedRow = 0;
                PlayUi(UiSound::Select);
            }

            if (count > 0 && beam)
            {
                // These are horizontal drag fields rendered in the editor's gold and ink style.
                const auto slider = [&](const char* label, const char* id, float& value, int field, float lo, float hi,
                                        float perPixel, const char* format, float y) {
                    const float left = x + 28.0f, w = width - 56.0f;
                    ImGui::SetCursorScreenPos(P(left, y));
                    ImGui::InvisibleButton(id, ImVec2(S(w), S(44.0f)));
                    const bool hovered = ImGui::IsItemHovered();
                    const bool active = ImGui::IsItemActive();
                    lightRowBox(left, y, left + w, y + 44.0f,
                        active ? RowLook::Selected : HoverShown(hovered) ? RowLook::Hovered : RowLook::Rest);
                    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                    {
                        const float fine = ImGui::GetIO().KeyCtrl ? 0.1f : 1.0f;
                        value = std::clamp(value + ImGui::GetIO().MouseDelta.x * perPixel * fine, lo, hi);
                        changed = true;
                    }
                    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        g_lightNumericEditAxis = field;
                        g_lightNumericEditValue = value;
                        g_lightNumericEditFocus = true;
                    }
                    Text(draw, left + 12.0f, y + 4.0f, 14.0f, kText, label);
                    const float barL = left + 12.0f, barR = left + w - 88.0f, barY = y + 31.0f;
                    draw->AddRectFilled(P(barL, barY - 3.0f), P(barR, barY + 3.0f), IM_COL32(236, 230, 218, 60));
                    const float t = std::clamp((value - lo) / (hi - lo), 0.0f, 1.0f);
                    draw->AddRectFilled(P(barL, barY - 3.0f), P(barL + (barR - barL) * t, barY + 3.0f), lightAccent);
                    draw->AddCircleFilled(P(barL + (barR - barL) * t, barY), S(7.0f), kText);
                    char shown[48]{};
                    std::snprintf(shown, sizeof(shown), format, value);
                    TextFit(draw, barR + 10.0f, y + 4.0f, 14.0f, hovered ? lightAccent : kText, shown, 74.0f);
                };
                slider("Pitch", "##AimPitch", g_lightDraft.aimPitch, 5, -180.0f, 180.0f, 1.00f, "%.2f deg", top + 464.0f);
                slider("Yaw", "##AimYaw", g_lightDraft.aimYaw, 6, 0.0f, 360.0f, 1.00f, "%.2f deg", top + 510.0f);
                slider("Roll", "##AimRoll", g_lightDraft.aimRoll, 8, 0.0f, 360.0f, 1.00f, "%.2f deg", top + 556.0f);
                if (beam)
                    slider("Beam angle", "##BeamAngle", g_lightDraft.beamAngle, 7, 0.0f, 360.0f, 1.00f, "%.2f deg", top + 602.0f);
            }

            std::string activeOn = markerLabel(g_lightDraft.markerId);
            int bindingIndex = 0; // World is the first choice, followed by markers in time order.
            for (std::uint32_t i = 0; i < g_snapshot.cameraKeyframeCount; ++i)
                if (g_snapshot.cameraKeyframes[i].id == g_lightDraft.markerId)
                    bindingIndex = static_cast<int>(i) + 1;
            Text(draw, x + 28.0f, top + 661.0f, 15.0f, kText, "ACTIVE ON");
            if (button("<", "##LightBindingLeft", x + 145.0f, top + 654.0f, 44.0f, 38.0f, count > 0 && bindingIndex > 0))
            {
                --bindingIndex;
                g_lightDraft.markerId = bindingIndex == 0 ? 0u : g_snapshot.cameraKeyframes[bindingIndex - 1].id;
                changed = true;
            }
            if (button(">", "##LightBindingRight", x + width - 72.0f, top + 654.0f, 44.0f, 38.0f,
                    count > 0 && bindingIndex < static_cast<int>(g_snapshot.cameraKeyframeCount)))
            {
                ++bindingIndex;
                g_lightDraft.markerId = bindingIndex == 0 ? 0u : g_snapshot.cameraKeyframes[bindingIndex - 1].id;
                changed = true;
            }
            activeOn = markerLabel(g_lightDraft.markerId);
            TextFit(draw, x + 198.0f, top + 661.0f, 15.0f, lightAccent, activeOn.c_str(), 190.0f);
            if (button("BACK TO LIGHTS", "##LightAdvancedBack", x + 28.0f,
                    std::min(top + 706.0f, bottom - 96.0f), width - 56.0f, 36.0f))
            {
                g_lightAdvancedOpen = false;
                g_lightSplineOpen = false;
                g_lightSplineDefaultsOpen = false;
                PlayUi(UiSound::Back);
            }
            if (changed && count > 0)
            {
                Command command{};
                command.keyframeId = selectedSourceIndex;
                command.authoredLight = g_lightDraft;
                g_lightDraftDirty = Send(CommandType::LightUpdate, &command);
            }
        }
        else if (lightRigPage)
        {
            SectionTitle(draw, x + 22.0f, top + 84.0f, width - 44.0f, "SAVE A REUSABLE LIGHT RIG", lightAccent);
            TextFit(draw, x + 28.0f, top + 106.0f, 12.0f, kMuted,
                "LOAD adds a rig group to this marker. AT CAMERA adds it in front of the camera.", width - 56.0f);
            const float fieldX = x + 28.0f, fieldY = top + 132.0f, fieldW = width - 182.0f;
            RowBox(draw, fieldX, fieldY, fieldX + fieldW, fieldY + 36.0f,
                RowLook::Rest, g_lightMenuPreset, lightAccent, lightHoverInk);
            Text(draw, fieldX + 10.0f, fieldY + 11.0f, 12.0f, lightAccent, "NAME");
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            SetTextInputCentered(fieldX + 62.0f, fieldY, 36.0f);
            ImGui::SetNextItemWidth(S(fieldW - 72.0f));
            ImGui::InputText("##LightRigName", g_lightRigName, sizeof(g_lightRigName));
            ImGui::PopStyleVar(); ImGui::PopStyleColor();
            if (button("SAVE RIG", "##LightRigSave", x + width - 144.0f, fieldY, 116.0f, 36.0f))
            {
                std::vector<AuthoredLight> markerLights;
                for (std::uint32_t n = 0; n < g_snapshot.authoredLightCount; ++n)
                    if (g_snapshot.authoredLights[n].markerId == focusMarker)
                        markerLights.push_back(g_snapshot.authoredLights[n]);
                if (markerLights.empty())
                    std::snprintf(g_lightRigStatus, sizeof(g_lightRigStatus), "No lights are assigned to this marker.");
                else if (!SaveLightRig(g_lightRigName, markerLights))
                    std::snprintf(g_lightRigStatus, sizeof(g_lightRigStatus), "Could not save. Use letters, numbers, spaces, _ or - (up to 39 characters).");
                else
                    std::snprintf(g_lightRigStatus, sizeof(g_lightRigStatus), "Saved %u light%s for this marker.",
                        static_cast<unsigned>(markerLights.size()), markerLights.size() == 1 ? "" : "s");
                PlayUi(UiSound::Select);
            }
            if (g_lightRigStatus[0])
                TextFit(draw, x + 28.0f, top + 178.0f, 11.0f, lightAccent, g_lightRigStatus, width - 56.0f);
            SectionTitle(draw, x + 22.0f, top + 206.0f, width - 44.0f, "SAVED RIGS", lightAccent);
            const std::vector<std::wstring> files = LightRigFiles();
            const float rowTop = top + 236.0f, rowHeight = 39.0f;
            const int visible = std::max(1, std::min(10, static_cast<int>((bottom - rowTop - 70.0f) / rowHeight)));
            for (int i = 0; i < std::min<int>(visible, static_cast<int>(files.size())); ++i)
            {
                const std::wstring& file = files[static_cast<std::size_t>(i)];
                std::string name;
                for (wchar_t c : file)
                    if (c != L'.') name.push_back(static_cast<char>(c)); else break;
                const float rowY = rowTop + i * rowHeight;
                lightRowBox(x + 28.0f, rowY, x + width - 28.0f, rowY + 34.0f,
                    RowLook::Rest);
                TextFit(draw, x + 42.0f, rowY + 10.0f, 14.0f, kText, name.c_str(), width - 302.0f);
                if (button("AT CAMERA", ("##AtCameraLightRig" + std::to_string(i)).c_str(),
                        x + width - 250.0f, rowY + 2.0f, 78.0f, 30.0f))
                {
                    std::vector<LightRigEntry> entries;
                    if (!LoadLightRig(file, entries))
                        std::snprintf(g_lightRigStatus, sizeof(g_lightRigStatus), "That rig file could not be read.");
                    else if (entries.empty())
                        std::snprintf(g_lightRigStatus, sizeof(g_lightRigStatus), "That rig is empty.");
                    else if (g_snapshot.authoredLightCount + entries.size() > kMaxAuthoredLights)
                        std::snprintf(g_lightRigStatus, sizeof(g_lightRigStatus), "There is not enough room for this rig.");
                    else
                    {
                        replay::editor::Vec3 center{};
                        for (const LightRigEntry& entry : entries)
                        {
                            center.x += entry.light.position.x;
                            center.y += entry.light.position.y;
                            center.z += entry.light.position.z;
                        }
                        const float inverseCount = 1.0f / static_cast<float>(entries.size());
                        center.x *= inverseCount; center.y *= inverseCount; center.z *= inverseCount;
                        const float cameraPitch = g_snapshot.camera.rotation.x;
                        const float cameraRoll = g_snapshot.camera.rotation.y;
                        const float cameraYaw = g_snapshot.camera.rotation.z;
                        constexpr float kRadians = 0.01745329251994329577f;
                        const float pitch = cameraPitch * kRadians, yaw = cameraYaw * kRadians;
                        const replay::editor::Vec3 forward{-std::sin(yaw) * std::cos(pitch),
                            std::cos(yaw) * std::cos(pitch), std::sin(pitch)};
                        const replay::editor::Vec3 cameraCenter{
                            g_snapshot.camera.position.x + forward.x * 4.0f,
                            g_snapshot.camera.position.y + forward.y * 4.0f,
                            g_snapshot.camera.position.z + forward.z * 4.0f};
                        std::uint32_t nextId = NextSnapshotLightId();
                        std::vector<std::uint32_t> groupIds;
                        groupIds.reserve(entries.size());
                        bool queued = true;
                        for (LightRigEntry& entry : entries)
                        {
                            AuthoredLight& light = entry.light;
                            const replay::editor::Vec3 relative{light.position.x - center.x,
                                light.position.y - center.y, light.position.z - center.z};
                            const replay::editor::Vec3 moved = RotateLightRigVector(relative,
                                cameraPitch, cameraRoll, cameraYaw);
                            light.position = {cameraCenter.x + moved.x, cameraCenter.y + moved.y, cameraCenter.z + moved.z};
                            replay::editor::Vec3 direction{}, up{};
                            LightRigDirectionUp(light.aimPitch, light.aimYaw, light.aimRoll, direction, up);
                            direction = RotateLightRigVector(direction, cameraPitch, cameraRoll, cameraYaw);
                            up = RotateLightRigVector(up, cameraPitch, cameraRoll, cameraYaw);
                            LightRigAnglesFromDirectionUp(direction, up, light.aimPitch, light.aimYaw, light.aimRoll);
                            light.markerId = focusMarker;
                            light.id = nextId++;
                            groupIds.push_back(light.id);
                            light.attachedTarget = 0;
                            light.attachedBone = 0;
                            light.lookAtTarget = 0;
                            light.attachType = static_cast<std::uint32_t>(AttachType::Default);
                            Command command{};
                            command.authoredLight = light;
                            queued = Send(CommandType::LightAdd, &command) && queued;
                        }
                        if (queued)
                        {
                            RegisterLightRigGroup(file, groupIds);
                            g_lightSelectedRigGroupId = groupIds.empty() ? 0u : groupIds.front();
                            g_lightSelectedId = groupIds.empty() ? 0u : groupIds.front();
                            g_lightRigSelectionPendingAt = GetTickCount64();
                            g_lightSelected = rowCount;
                            g_lightRigEditDraft = {};
                            std::snprintf(g_lightRigStatus, sizeof(g_lightRigStatus),
                                "Placed %u-light rig at the camera. The saved rig file is unchanged.",
                                static_cast<unsigned>(entries.size()));
                            g_lightDraftIndex = -1;
                            g_lightDraftDirty = false;
                        }
                        else
                            std::snprintf(g_lightRigStatus, sizeof(g_lightRigStatus), "The command queue disconnected while placing the rig.");
                    }
                    const bool placed = std::strncmp(g_lightRigStatus, "Placed ", 7) == 0;
                    PlayUi(placed ? UiSound::Select : UiSound::Error);
                }
                if (button("LOAD", ("##LoadLightRig" + std::to_string(i)).c_str(), x + width - 166.0f, rowY + 2.0f, 62.0f, 30.0f))
                {
                    bool queued = true;
                    std::vector<LightRigEntry> entries;
                    if (!LoadLightRig(file, entries))
                    {
                        queued = false;
                        std::snprintf(g_lightRigStatus, sizeof(g_lightRigStatus), "That rig file could not be read.");
                    }
                    else if (entries.size() > 128 || entries.size() > kMaxAuthoredLights - count)
                    {
                        queued = false;
                        std::snprintf(g_lightRigStatus, sizeof(g_lightRigStatus), "This rig exceeds the safe light or command limit.");
                    }
                    else
                    {
                        const auto targetByName = [](const char* label, bool activeOnly) -> std::uint32_t {
                            if (!label || !*label) return 0;
                            for (std::uint32_t n = 0; n < g_snapshot.targetCount; ++n)
                                if (g_snapshot.targets[n].kind == 0u && (!activeOnly || g_snapshot.targets[n].active) &&
                                    _stricmp(g_snapshot.targets[n].label, label) == 0)
                                    return g_snapshot.targets[n].id;
                            return 0;
                        };
                        std::uint32_t nextId = NextSnapshotLightId();
                        std::vector<std::uint32_t> groupIds;
                        groupIds.reserve(entries.size());
                        for (LightRigEntry& entry : entries)
                        {
                            AuthoredLight& light = entry.light;
                            light.markerId = focusMarker;
                            light.id = nextId++;
                            groupIds.push_back(light.id);
                            if (entry.attachmentLabel[0])
                            {
                                light.attachedTarget = targetByName(entry.attachmentLabel, true);
                                if (!light.attachedTarget)
                                {
                                    light.attachedBone = 0;
                                    light.enabled = 0; // local offset cannot be converted without the recorded character being present
                                }
                            }
                            if (entry.lookAtLabel[0])
                                light.lookAtTarget = targetByName(entry.lookAtLabel, false);
                            Command command{}; command.authoredLight = light;
                            queued = Send(CommandType::LightAdd, &command) && queued;
                        }
                        if (queued)
                        {
                            RegisterLightRigGroup(file, groupIds);
                            g_lightSelectedRigGroupId = groupIds.empty() ? 0u : groupIds.front();
                            g_lightSelectedId = groupIds.empty() ? 0u : groupIds.front();
                            g_lightRigSelectionPendingAt = GetTickCount64();
                            g_lightSelected = 0;
                            g_lightRigEditDraft = {};
                            std::snprintf(g_lightRigStatus, sizeof(g_lightRigStatus), "Added %u light%s as a rig on this marker.",
                                static_cast<unsigned>(entries.size()), entries.size() == 1 ? "" : "s");
                        }
                        else
                            std::snprintf(g_lightRigStatus, sizeof(g_lightRigStatus),
                                "The command queue disconnected while loading; reopen the rig library and check the result.");
                        g_lightDraftIndex = -1;
                        g_lightDraftDirty = false;
                    }
                    PlayUi(queued ? UiSound::Select : UiSound::Error);
                }
                if (button("DELETE", ("##DeleteLightRig" + std::to_string(i)).c_str(), x + width - 96.0f, rowY + 2.0f, 68.0f, 30.0f))
                {
                    const std::wstring path = LightRigFolder() + L"\\" + file;
                    if (DeleteFileW(path.c_str()))
                        std::snprintf(g_lightRigStatus, sizeof(g_lightRigStatus), "Deleted %s.", name.c_str());
                    else
                        std::snprintf(g_lightRigStatus, sizeof(g_lightRigStatus), "Could not delete that rig file.");
                    PlayUi(UiSound::Select);
                }
            }
            if (files.empty())
                Text(draw, x + 30.0f, rowTop + 12.0f, 14.0f, kMuted, "No saved rigs yet.");
            const float backY = std::min(top + 846.0f, bottom - 96.0f);
            if (button("BACK TO LIGHTS", "##LightRigBack", x + 28.0f, backY, width - 56.0f, 36.0f))
            {
                g_lightRigMenuOpen = false;
                g_lightSettingsMenuOpen = false;
                g_lightRigStatus[0] = '\0';
                PlayUi(UiSound::Back);
            }
        }
        else if (g_lightSettingsMenuOpen)
        {
            SectionTitle(draw, x + 22.0f, top + 84.0f, width - 44.0f, "MENU PRESETS", lightAccent);
            TextFit(draw, x + 28.0f, top + 105.0f, 12.0f, kMuted,
                "All looks use the live lighting controls and share the movable light preview.", width - 56.0f);

            static constexpr const char* kPresetNames[] = {
                "BLUEPRINT", "FRONTIER CLASSIC", "CRIMSON RED", "VIOLET OUTLAW", "MIDNIGHT FRONTIER", "ORIGINAL"
            };
            static constexpr const char* kPresetDescriptions[] = {
                "Current cyan-blue theme.",
                "Traditional gold Western theme.",
                "Deep red theme.",
                "New purple theme.",
                "New dark-blue theme.",
                "Legacy Redemption Editor appearance."
            };
            const float cardX = x + 28.0f, cardWidth = width - 56.0f;
            const float backY = std::min(top + 706.0f, bottom - 96.0f);
            const float cardStartY = top + 142.0f;
            const float cardAvailable = std::max(0.0f, backY - 54.0f - cardStartY);
            const float cardGap = std::clamp(cardAvailable * 0.012f, 4.0f, 9.0f);
            const float cardHeight = (cardAvailable - cardGap * 5.0f) / 6.0f;
            for (int i = 0; i < 6; ++i)
            {
                const float cardY = cardStartY + i * (cardHeight + cardGap);
                ImGui::SetCursorScreenPos(P(cardX, cardY));
                ImGui::InvisibleButton(("##LightPresetCard" + std::to_string(i)).c_str(), ImVec2(S(cardWidth), S(cardHeight)));
                const bool hovered = ImGui::IsItemHovered();
                const bool selected = g_lightMenuPreset == i;
                const bool clicked = hovered && MouseClicked();
                RowBox(draw, cardX, cardY, cardX + cardWidth, cardY + cardHeight,
                    selected ? RowLook::Selected : HoverShown(hovered) ? RowLook::Hovered : RowLook::Rest,
                    i, kLightPresetAccents[i], kLightPresetHover[i]);
                draw->AddRect(P(cardX + 2.0f, cardY + 2.0f), P(cardX + cardWidth - 2.0f, cardY + cardHeight - 2.0f),
                    selected ? kLightPresetAccents[i] : WithAlpha(kLightPresetAccents[i], 0.39f), S(3.0f), 0,
                    S(selected ? 2.0f : 1.0f));
                const float titleFont = std::clamp(cardHeight * 0.23f, 11.0f, 17.0f);
                const float descriptionFont = std::clamp(cardHeight * 0.17f, 9.0f, 13.0f);
                TextFit(draw, cardX + 16.0f, cardY + 6.0f, titleFont, kLightPresetAccents[i], kPresetNames[i], cardWidth - 132.0f);
                if (selected)
                    Text(draw, cardX + cardWidth - 90.0f, cardY + 7.0f, 11.0f, kLightPresetAccents[i], "CURRENT");
                TextFit(draw, cardX + 16.0f, cardY + cardHeight * 0.53f, descriptionFont, kText,
                    kPresetDescriptions[i], cardWidth - 132.0f);

                const float swatchY = cardY + (cardHeight - 14.0f) * 0.5f;
                const float swatchX = cardX + cardWidth - 94.0f;
                draw->AddRectFilled(P(swatchX, swatchY), P(swatchX + 72.0f, swatchY + 14.0f),
                    kLightPresetPanel[i], S(2.0f));
                draw->AddRect(P(swatchX, swatchY), P(swatchX + 72.0f, swatchY + 14.0f),
                    WithAlpha(kLightPresetAccents[i], 0.70f), S(2.0f), 0, S(1.0f));
                draw->AddRectFilled(P(swatchX + 8.0f, swatchY + 5.0f), P(swatchX + 47.0f, swatchY + 9.0f),
                    kLightPresetAccents[i], S(2.0f));
                if (i == 5)
                    draw->AddRectFilled(P(swatchX + 49.0f, swatchY + 5.0f), P(swatchX + 54.0f, swatchY + 9.0f), kRed);
                draw->AddCircleFilled(P(swatchX + 61.0f, swatchY + 7.0f), S(4.0f), kText);
                if (clicked)
                {
                    g_lightMenuPreset = i;
#ifndef REDEMPTION_UI_HARNESS
                    wchar_t folder[MAX_PATH]{};
                    GetCurrentDirectoryW(MAX_PATH, folder);
                    const std::wstring ini = replay::identity::IniPathInW(folder);
                    const std::wstring value = std::to_wstring(i);
                    replay::identity::IniWriteW(L"LightMenuPreset", value.c_str(), ini.c_str());
                    replay::identity::IniWriteW(L"LightMenuPresetVersion", L"2", ini.c_str());
#endif
                    g_lightSettingsMenuOpen = false;
                    g_lightRigMenuOpen = false;
                    g_lightAdvancedOpen = false;
                    g_lightSplineOpen = false;
                    PlayUi(UiSound::Select);
                }
            }

            TextFit(draw, x + 30.0f, backY - 25.0f, 11.0f, kMuted,
                "Original keeps the legacy Redemption Editor appearance and remains the default.", width - 60.0f);
            if (button("BACK TO LIGHTS", "##LightPresetBack", x + 28.0f, backY, width - 56.0f, 36.0f))
            {
                g_lightSettingsMenuOpen = false;
                g_lightRigMenuOpen = false;
                PlayUi(UiSound::Back);
            }
        }        else
        {
            SectionTitle(draw, x + 22.0f, top + 84.0f, width - 44.0f, "LIGHTS", lightAccent);
            Text(draw, x + 30.0f, top + 105.0f, 13.0f, kMuted,
                "Double-click a light or rig name to rename it. New markers inherit these lights.");
            const float listY = top + 132.0f;
            const float rowH = 31.0f;
            const int visibleRows = std::max(1, std::min(6, static_cast<int>((bottom - listY - 554.0f) / rowH)));
            g_lightListFirst = std::clamp(g_lightListFirst, 0, std::max(0, rowCount - visibleRows));
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const ImVec2 listTL = P(x + 22.0f, listY), listBR = P(x + width - 22.0f, listY + visibleRows * rowH);
            if (mouse.x >= listTL.x && mouse.x <= listBR.x && mouse.y >= listTL.y && mouse.y <= listBR.y && ImGui::GetIO().MouseWheel != 0.0f)
                g_lightListFirst = std::clamp(g_lightListFirst - static_cast<int>(ImGui::GetIO().MouseWheel), 0,
                    std::max(0, rowCount - visibleRows));
            for (int r = 0; r < visibleRows; ++r)
            {
                const int i = g_lightListFirst + r;
                if (i >= rowCount)
                    break;
                const LightListRow& row = lightRows[static_cast<std::size_t>(i)];
                const std::uint32_t sourceIndex = row.representativeIndex;
                const AuthoredLight& light = g_snapshot.authoredLights[sourceIndex];
                const float rowY = listY + r * rowH;
                bool clicked = false;
                const bool doubleClicked = ListItem(draw, ("##LightRow" + std::to_string(row.rigGroupId ? row.rigGroupId : light.id)).c_str(), x + 30.0f, rowY, width - 138.0f, rowH - 2.0f,
                    g_lightSelected == i, clicked, nullptr, width - 78.0f, g_lightMenuPreset, lightAccent, lightHoverInk);
                if (clicked)
                {
                    g_lightSelected = i;
                    g_lightSelectedRigGroupId = row.rigGroupId;
                    g_lightSelectedId = light.id;
                    g_lightDraft = light;
                    if (g_lightDraft.reflectorEnabled)
                    {
                        g_lightDraft.coneEnabled = 1;
                        g_lightDraft.reflectorEnabled = 0;
                        g_lightDraft.reflectorHidden = 0;
                    }
                    g_lightDraftIndex = static_cast<int>(sourceIndex);
                    g_lightDraftDirty = false;
                    g_lightRigEditDraft = {};
                    g_lightRigSelectionPendingAt = 0;
                }
                if (doubleClicked)
                    g_lightRenameFocus = true;
                const bool allEnabled = std::all_of(row.sourceIndices.begin(), row.sourceIndices.end(), [&](std::uint32_t index) {
                    return g_snapshot.authoredLights[index].enabled != 0;
                });
                draw->AddCircle(P(x + 55.0f, rowY + 14.0f), S(6.0f), allEnabled ? lightAccent : kMuted, 16, S(1.5f));
                draw->AddRectFilled(P(x + 52.0f, rowY + 21.0f), P(x + 58.0f, rowY + 24.0f), allEnabled ? lightAccent : kMuted);
                char label[72]{};
                char type[32]{};
                if (row.rigGroupId)
                {
                    const LightRigGroup* group = FindLightRigGroup(row.rigGroupId);
                    std::snprintf(label, sizeof(label), "%s", group && group->displayName[0] ? group->displayName : "Light rig");
                    std::snprintf(type, sizeof(type), "RIG  (%u)", static_cast<unsigned>(row.sourceIndices.size()));
                }
                else
                {
                    std::snprintf(label, sizeof(label), "%s", light.name[0] ? light.name : "Light");
                    std::snprintf(type, sizeof(type), "%s", (light.coneEnabled || light.reflectorEnabled) ? "BEAM" : "POINT");
                }
                TextFit(draw, x + 70.0f, rowY + 5.0f, 14.0f, g_lightSelected == i ? kText : kMuted, label, 180.0f);
                Text(draw, x + 265.0f, rowY + 6.0f, 12.0f, kMuted, type);
                if (row.rigGroupId)
                {
                    const float ungroupX = x + width - 106.0f;
                    const float ungroupY = rowY + 4.0f;
                    const std::string ungroupId = "##LightUngroup" + std::to_string(row.rigGroupId);
                    ImGui::SetCursorScreenPos(P(ungroupX, ungroupY));
                    ImGui::InvisibleButton(ungroupId.c_str(), ImVec2(S(30.0f), S(22.0f)));
                    const bool ungroupHovered = ImGui::IsItemHovered();
                    const bool ungroupClicked = ungroupHovered && MouseClicked();
                    RowBox(draw, ungroupX, ungroupY, ungroupX + 30.0f, ungroupY + 22.0f,
                        ungroupClicked ? RowLook::Selected : HoverShown(ungroupHovered) ? RowLook::Hovered : RowLook::Rest,
                        g_lightMenuPreset, lightAccent, lightHoverInk);
                    TextFit(draw, ungroupX + 2.0f, ungroupY + 5.0f, 10.0f,
                        ungroupHovered ? lightAccent : kMuted, "UNG", 26.0f);
                    if (ungroupHovered)
                        ImGui::SetTooltip("Ungroup this rig and keep each light on this marker. Ctrl+Z restores the rig.");
                    if (ungroupClicked)
                    {
                        const std::uint32_t firstLightId = light.id;
                        const std::uint32_t firstLightIndex = sourceIndex;
                        const std::uint32_t groupId = row.rigGroupId;
                        if (const LightRigGroup* group = FindLightRigGroup(groupId))
                        {
                            g_lightRigUngroupUndo.valid = true;
                            g_lightRigUngroupUndo.ungrouped = true;
                            g_lightRigUngroupUndo.clip = g_snapshot.currentClip;
                            g_lightRigUngroupUndo.markerId = focusMarker;
                            g_lightRigUngroupUndo.group = *group;
                        }
                        g_lightRigGroups.erase(std::remove_if(g_lightRigGroups.begin(), g_lightRigGroups.end(),
                            [&](const LightRigGroup& group) { return group.id == groupId; }), g_lightRigGroups.end());
                        SaveLightRigGroups();
                        g_lightSelected = i;
                        g_lightSelectedId = firstLightId;
                        g_lightSelectedRigGroupId = 0;
                        g_lightRigSelectionPendingAt = 0;
                        g_lightDraft = light;
                        g_lightDraftIndex = static_cast<int>(firstLightIndex);
                        g_lightDraftDirty = false;
                        g_lightRigEditDraft = {};
                        ClearLightDraftAttachSync();
                        PlayUi(UiSound::Select);
                    }
                }
                bool enabled = allEnabled;
                if (toggle("", enabled, ("##LightEnable" + std::to_string(row.rigGroupId ? row.rigGroupId : sourceIndex)).c_str(), x + width - 68.0f, rowY, true, 22.0f))
                {
                    g_lightSelected = i;
                    g_lightSelectedRigGroupId = row.rigGroupId;
                    g_lightSelectedId = light.id;
                    g_lightDraft = light;
                    g_lightDraftIndex = static_cast<int>(sourceIndex);
                    g_lightDraftDirty = false;
                    g_lightRigEditDraft = {};
                    g_lightRigSelectionPendingAt = 0;
                    bool queued = true;
                    for (const std::uint32_t memberIndex : row.sourceIndices)
                    {
                        if (memberIndex >= g_snapshot.authoredLightCount)
                            continue;
                        Command command{};
                        command.keyframeId = memberIndex;
                        command.authoredLight = g_snapshot.authoredLights[memberIndex];
                        command.authoredLight.enabled = enabled ? 1u : 0u;
                        queued = Send(CommandType::LightUpdate, &command) && queued;
                    }
                    if (queued)
                    {
                        g_lightDraft.enabled = enabled ? 1u : 0u;
                        PlayUi(UiSound::Select);
                    }
                }
            }
            if (button("ADD LIGHT", "##LightAdd", x + 22.0f, listY + visibleRows * rowH + 4.0f, 130.0f, 34.0f, totalCount < kMaxAuthoredLights))
            {
                AuthoredLight light{};
                light.id = NextSnapshotLightId();
                std::snprintf(light.name, sizeof(light.name), "Light %02u", totalCount + 1);
                light.markerId = focusMarker;
                const float pitch = g_snapshot.camera.rotation.x * 0.01745329251994329577f;
                const float yaw = g_snapshot.camera.rotation.z * 0.01745329251994329577f;
                const float cp = std::cos(pitch);
                light.position.x = g_snapshot.camera.position.x - std::sin(yaw) * cp * 4.0f;
                light.position.y = g_snapshot.camera.position.y + std::cos(yaw) * cp * 4.0f;
                light.position.z = g_snapshot.camera.position.z + std::sin(pitch) * 4.0f;
                light.range = 30.0f; light.intensity = 75.0f;
                light.colorR = light.colorG = light.colorB = 255; light.enabled = 1;
                light.attachType = static_cast<std::uint32_t>(AttachType::Default);
                light.aimPitch = g_snapshot.camera.rotation.x; light.aimYaw = g_snapshot.camera.rotation.z; light.beamAngle = 35.0f;
                light.blend = static_cast<std::uint32_t>(replay::editor::Blend::Spline);
                Command command{}; command.authoredLight = light;
                if (Send(CommandType::LightAdd, &command))
                {
                    g_lightSelectedRigGroupId = 0;
                    g_lightSelected = rowCount; g_lightSelectedId = light.id; g_lightDraftIndex = -1; g_lightDraftDirty = false;
                    g_lightRigSelectionPendingAt = GetTickCount64();
                    g_lightRigEditDraft = {};
                    g_lightDraft = light;
                    PlayUi(UiSound::Select);
                }
            }
            if (button("DUPLICATE", "##LightDuplicate", x + 160.0f, listY + visibleRows * rowH + 4.0f, 126.0f, 34.0f,
                    count > 0 && selectedRow && totalCount < kMaxAuthoredLights))
            {
                if (selectedIsRig && selectedRow && selectedRow->sourceIndices.size() <= kMaxAuthoredLights - totalCount)
                {
                    std::vector<std::uint32_t> ids;
                    std::uint32_t nextId = NextSnapshotLightId();
                    bool queued = true;
                    for (const std::uint32_t sourceIndex : selectedRow->sourceIndices)
                    {
                        AuthoredLight light = g_snapshot.authoredLights[sourceIndex];
                        light.markerId = focusMarker;
                        light.id = nextId++;
                        char duplicateName[32]{};
                        std::snprintf(duplicateName, sizeof(duplicateName), "%.23s Copy", light.name[0] ? light.name : "Light");
                        std::memcpy(light.name, duplicateName, sizeof(light.name));
                        Command command{}; command.authoredLight = light;
                        queued = Send(CommandType::LightAdd, &command) && queued;
                        ids.push_back(light.id);
                    }
                    if (queued && !ids.empty())
                    {
                        LightRigGroup group;
                        group.id = ids.front();
                        if (const LightRigGroup* sourceGroup = FindLightRigGroup(selectedRow->rigGroupId))
                        {
                            std::snprintf(group.savedName, sizeof(group.savedName), "%s", sourceGroup->savedName);
                            std::snprintf(group.displayName, sizeof(group.displayName), "Copy of %.38s", sourceGroup->displayName);
                        }
                        else
                        {
                            std::snprintf(group.savedName, sizeof(group.savedName), "Light Rig");
                            std::snprintf(group.displayName, sizeof(group.displayName), "Copy of Light Rig");
                        }
                        group.memberIds = std::move(ids);
                        g_lightRigGroups.push_back(std::move(group));
                        SaveLightRigGroups();
                        g_lightSelectedRigGroupId = g_lightRigGroups.back().id;
                        g_lightRigSelectionPendingAt = GetTickCount64();
                        g_lightSelected = rowCount;
                        g_lightSelectedId = ids.front();
                        g_lightDraftIndex = -1;
                        g_lightDraftDirty = false;
                        g_lightRigEditDraft = {};
                        PlayUi(UiSound::Select);
                    }
                    else
                        PlayUi(UiSound::Error);
                }
                else if (!selectedIsRig)
                {
                    AuthoredLight light = g_snapshot.authoredLights[selectedSourceIndex]; light.markerId = focusMarker;
                    light.id = NextSnapshotLightId();
                    char duplicateName[32]{};
                    std::snprintf(duplicateName, sizeof(duplicateName), "%.23s Copy", light.name[0] ? light.name : "Light");
                    std::memcpy(light.name, duplicateName, sizeof(light.name));
                    Command command{}; command.authoredLight = light;
                    if (Send(CommandType::LightAdd, &command))
                    {
                        g_lightSelectedRigGroupId = 0;
                        g_lightSelected = rowCount; g_lightSelectedId = light.id; g_lightDraftIndex = -1; g_lightDraftDirty = false;
                        g_lightRigSelectionPendingAt = GetTickCount64();
                        g_lightRigEditDraft = {};
                        g_lightDraft = light;
                        PlayUi(UiSound::Select);
                    }
                }
            }
            if (!deletedSelectedLight && button("DELETE", "##LightDelete", x + 294.0f,
                    listY + visibleRows * rowH + 4.0f, 82.0f, 34.0f, count > 0 && selectedRow))
            {
                deleteSelectedLight();
            }
            if (button("RIGS", "##LightRigLibrary", x + 384.0f,
                    listY + visibleRows * rowH + 4.0f, width - 406.0f, 34.0f))
            {
                g_lightSettingsMenuOpen = false;
                g_lightRigMenuOpen = true;
                g_lightRigStatus[0] = '\0';
                PlayUi(UiSound::Select);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Save, load, or place an editable light rig at the camera.");
            const float propsY = listY + visibleRows * rowH + 48.0f;
            SectionTitle(draw, x + 22.0f, propsY, width - 44.0f, "LIGHT PROPERTIES", lightAccent);
            if (button(g_lightGizmosVisibleUi ? "GIZMOS: ON" : "GIZMOS: OFF", "##LightGizmosVisible",
                    x + width - 194.0f, propsY + 18.0f, 164.0f, 28.0f))
            {
                g_lightGizmosVisibleUi = !g_lightGizmosVisibleUi;
                PlayUi(UiSound::Select);
            }
            if (count > 0 && selectedRow)
            {
                if (selectedIsRig && g_lightRigEditDraft.valid)
                {
                    LightRigGroup* group = FindLightRigGroup(g_lightSelectedRigGroupId);
                    const float nameLeft = x + 28.0f, nameY = propsY + 17.0f, nameWidth = width - 226.0f;
                    RowBox(draw, nameLeft, nameY, nameLeft + nameWidth, nameY + 36.0f,
                        RowLook::Rest, g_lightMenuPreset, lightAccent, lightHoverInk);
                    Text(draw, nameLeft + 12.0f, nameY + 11.0f, 12.0f, lightAccent, "RIG NAME");
                    if (group)
                    {
                        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.10f, 0.09f, 0.08f, 0.28f));
                        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8.0f), S(5.0f)));
                        SetTextInputCentered(nameLeft + 86.0f, nameY, 36.0f);
                        ImGui::SetNextItemWidth(S(nameWidth - 100.0f));
                        if (g_lightRenameFocus)
                        {
                            ImGui::SetKeyboardFocusHere();
                            g_lightRenameFocus = false;
                        }
                        if (ImGui::InputText("##LightRigDisplayName", group->displayName, sizeof(group->displayName)))
                            SaveLightRigGroups();
                        ImGui::PopStyleVar(2);
                        ImGui::PopStyleColor(3);
                        TextFit(draw, x + 30.0f, propsY + 57.0f, 11.0f, kMuted,
                            (std::string("SAVED RIG: ") + group->savedName).c_str(), width - 60.0f);
                    }
                    bool groupChanged = false;
                    const auto groupSlider = [&](const char* label, const char* id, float& value,
                        float lo, float hi, float perPixel, const char* format, float y) {
                        const float left = x + 30.0f, w = width - 60.0f;
                        ImGui::SetCursorScreenPos(P(left, y));
                        ImGui::InvisibleButton(id, ImVec2(S(w), S(39.0f)));
                        const bool hovered = ImGui::IsItemHovered();
                        const bool active = ImGui::IsItemActive();
                        RowBox(draw, left, y, left + w, y + 39.0f,
                            active ? RowLook::Selected : HoverShown(hovered) ? RowLook::Hovered : RowLook::Rest,
                            g_lightMenuPreset, lightAccent, lightHoverInk);
                        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                        {
                            value = std::clamp(value + ImGui::GetIO().MouseDelta.x * perPixel *
                                (ImGui::GetIO().KeyCtrl ? 0.1f : 1.0f), lo, hi);
                            groupChanged = true;
                        }
                        Text(draw, left + 12.0f, y + 3.0f, 13.0f, kText, label);
                        const float barL = left + 12.0f, barR = left + w - 92.0f, barY = y + 29.0f;
                        draw->AddRectFilled(P(barL, barY - 2.5f), P(barR, barY + 2.5f), IM_COL32(236, 230, 218, 60));
                        const float t = std::clamp((value - lo) / (hi - lo), 0.0f, 1.0f);
                        draw->AddRectFilled(P(barL, barY - 2.5f), P(barL + (barR - barL) * t, barY + 2.5f), lightAccent);
                        draw->AddCircleFilled(P(barL + (barR - barL) * t, barY), S(5.0f), kText);
                        char shown[48]{};
                        std::snprintf(shown, sizeof(shown), format, value);
                        TextFit(draw, barR + 8.0f, y + 3.0f, 13.0f, hovered ? lightAccent : kText, shown, 76.0f);
                    };
                    groupSlider("Rig X", "##RigX", g_lightRigEditDraft.center.x, -10000.0f, 10000.0f, 0.01f, "%.2f", propsY + 73.0f);
                    groupSlider("Rig Y", "##RigY", g_lightRigEditDraft.center.y, -10000.0f, 10000.0f, 0.01f, "%.2f", propsY + 112.0f);
                    groupSlider("Rig Z", "##RigZ", g_lightRigEditDraft.center.z, -10000.0f, 10000.0f, 0.01f, "%.2f", propsY + 151.0f);
                    groupSlider("Intensity", "##RigIntensity", g_lightRigEditDraft.intensity, 0.0f, 300.0f, 0.1f, "%.2f", propsY + 190.0f);
                    groupSlider("Range", "##RigRange", g_lightRigEditDraft.range, 0.5f, 250.0f, 0.05f, "%.2f m", propsY + 229.0f);
                    Text(draw, x + 30.0f, propsY + 270.0f, 10.0f, kMuted, "QUICK COLOR TEMPERATURE");
                    static constexpr const char* rigTemperatureNames[] = {"CANDLE", "TUNGSTEN", "NEUTRAL", "DAYLIGHT", "MOON"};
                    static constexpr float rigTemperatures[] = {1900.0f, 2700.0f, 4500.0f, 5600.0f, 8000.0f};
                    const float rigTempWidth = (width - 80.0f) / 5.0f;
                    for (int i = 0; i < 5; ++i)
                    {
                        if (button(rigTemperatureNames[i], ("##RigTemp" + std::to_string(i)).c_str(),
                                x + 30.0f + i * (rigTempWidth + 5.0f), propsY + 281.0f, rigTempWidth, 20.0f))
                        {
                            const auto rgb = LightTemperatureRgb(rigTemperatures[i]);
                            g_lightRigEditDraft.colorR = static_cast<std::uint32_t>(rgb[0]);
                            g_lightRigEditDraft.colorG = static_cast<std::uint32_t>(rgb[1]);
                            g_lightRigEditDraft.colorB = static_cast<std::uint32_t>(rgb[2]);
                            g_lightRigEditDraft.colorChanged = true;
                            groupChanged = true;
                        }
                    }
                    std::uint32_t rigRgb[3] = {g_lightRigEditDraft.colorR, g_lightRigEditDraft.colorG, g_lightRigEditDraft.colorB};
                    const char* rigRgbNames[3] = {"Red", "Green", "Blue"};
                    for (int c = 0; c < 3; ++c)
                    {
                        const float rowY = propsY + 304.0f + c * 19.0f;
                        const float rowLeft = x + 30.0f, rowWidth = width - 60.0f;
                        const float barL = rowLeft + 78.0f, barR = rowLeft + rowWidth - 48.0f;
                        ImGui::SetCursorScreenPos(P(barL, rowY + 3.0f));
                        ImGui::InvisibleButton(("##RigRGB" + std::to_string(c)).c_str(), ImVec2(S(barR - barL), S(12.0f)));
                        const bool hovered = ImGui::IsItemHovered();
                        const bool clicked = hovered && MouseClicked();
                        const bool dragging = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
                        RowBox(draw, rowLeft, rowY, rowLeft + rowWidth, rowY + 18.0f,
                            HoverShown(hovered) ? RowLook::Hovered : RowLook::Rest,
                            g_lightMenuPreset, lightAccent, lightHoverInk);
                        if (clicked || dragging)
                        {
                            const float t = std::clamp((ImGui::GetIO().MousePos.x - P(barL, rowY + 3.0f).x) /
                                std::max(1.0f, S(barR - barL)), 0.0f, 1.0f);
                            rigRgb[c] = static_cast<std::uint32_t>(std::lround(t * 255.0f));
                            g_lightRigEditDraft.colorR = rigRgb[0];
                            g_lightRigEditDraft.colorG = rigRgb[1];
                            g_lightRigEditDraft.colorB = rigRgb[2];
                            g_lightRigEditDraft.colorChanged = true;
                            groupChanged = true;
                        }
                        Text(draw, rowLeft + 12.0f, rowY + 2.0f, 11.0f, kText, rigRgbNames[c]);
                        draw->AddRectFilled(P(barL, rowY + 7.0f), P(barR, rowY + 11.0f), IM_COL32(236, 230, 218, 60));
                        draw->AddRectFilled(P(barL, rowY + 7.0f), P(barL + (barR - barL) * rigRgb[c] / 255.0f, rowY + 11.0f), lightAccent);
                        draw->AddCircleFilled(P(barL + (barR - barL) * rigRgb[c] / 255.0f, rowY + 9.0f), S(4.0f), kText);
                        char value[8]{};
                        std::snprintf(value, sizeof(value), "%u", rigRgb[c]);
                        Text(draw, barR + 8.0f, rowY + 2.0f, 11.0f, kText, value);
                    }
                    if (button("ROTATION / ADVANCED  >", "##LightAdvancedOpen", x + 28.0f, propsY + 368.0f, width - 56.0f, 34.0f))
                    {
                        g_lightAdvancedOpen = true;
                        PlayUi(UiSound::Select);
                    }
                    if (groupChanged && !applyLightRigDraft())
                        std::snprintf(g_lightRigStatus, sizeof(g_lightRigStatus), "Could not update every light in this rig.");
                    TextFit(draw, x + 30.0f, propsY + 408.0f, 11.0f, kMuted,
                        "Drag a slider; hold CTRL for fine movement. Changes apply to the full rig.", width - 60.0f);
                    if (button(g_lightPreviewVisible ? "PREVIEW: ON" : "PREVIEW: OFF", "##LightPreviewToggleRig",
                            x + width - 136.0f, propsY + 440.0f, 108.0f, 36.0f))
                    {
                        g_lightPreviewVisible = !g_lightPreviewVisible;
                        saveLightPreviewSettings();
                        PlayUi(UiSound::Select);
                    }
                }
                else
                {
                const float nameLeft = x + 28.0f, nameY = propsY + 17.0f, nameWidth = width - 226.0f;
                const ImVec2 nameMouse = ImGui::GetIO().MousePos;
                const bool nameHovered = nameMouse.x >= P(nameLeft, nameY).x && nameMouse.x <= P(nameLeft + nameWidth, nameY).x &&
                    nameMouse.y >= P(nameLeft, nameY).y && nameMouse.y <= P(nameLeft, nameY + 36.0f).y;
                RowBox(draw, nameLeft, nameY, nameLeft + nameWidth, nameY + 36.0f,
                    HoverShown(nameHovered) ? RowLook::Hovered : RowLook::Rest, g_lightMenuPreset, lightAccent, lightHoverInk);
                Text(draw, nameLeft + 12.0f, nameY + 11.0f, 14.0f, lightAccent, "NAME");
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.10f, 0.09f, 0.08f, 0.28f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8.0f), S(5.0f)));
                SetTextInputCentered(nameLeft + 86.0f, nameY, 36.0f);
                ImGui::SetNextItemWidth(S(nameWidth - 100.0f));
                if (g_lightRenameFocus)
                {
                    ImGui::SetKeyboardFocusHere();
                    g_lightRenameFocus = false;
                }
                if (ImGui::InputText("##LightName", g_lightDraft.name, sizeof(g_lightDraft.name), ImGuiInputTextFlags_AutoSelectAll))
                    changed = true;
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(3);
                const auto slider = [&](const char* label, const char* id, float& value, int field, float lo, float hi,
                                        float perPixel, const char* format, float y, float left, float w) {
                    ImGui::SetCursorScreenPos(P(left, y));
                    ImGui::InvisibleButton(id, ImVec2(S(w), S(40.0f)));
                    const bool hovered = ImGui::IsItemHovered();
                    const bool active = ImGui::IsItemActive();
                    RowBox(draw, left, y, left + w, y + 40.0f,
                        active ? RowLook::Selected : HoverShown(hovered) ? RowLook::Hovered : RowLook::Rest,
                        g_lightMenuPreset, lightAccent, lightHoverInk);
                    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                    {
                        value = std::clamp(value + ImGui::GetIO().MouseDelta.x * perPixel * (ImGui::GetIO().KeyCtrl ? 0.1f : 1.0f), lo, hi);
                        changed = true;
                    }
                    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        g_lightNumericEditAxis = field; g_lightNumericEditValue = value; g_lightNumericEditFocus = true;
                    }
                    Text(draw, left + 12.0f, y + 3.0f, 13.0f, kText, label);
                    const float barL = left + 12.0f, barR = left + w - 92.0f, barY = y + 29.0f;
                    draw->AddRectFilled(P(barL, barY - 2.5f), P(barR, barY + 2.5f), IM_COL32(236, 230, 218, 60));
                    const float t = std::clamp((value - lo) / (hi - lo), 0.0f, 1.0f);
                    draw->AddRectFilled(P(barL, barY - 2.5f), P(barL + (barR - barL) * t, barY + 2.5f), lightAccent);
                    draw->AddCircleFilled(P(barL + (barR - barL) * t, barY), S(5.0f), kText);
                    char shown[48]{}; std::snprintf(shown, sizeof(shown), format, value);
                    TextFit(draw, barR + 8.0f, y + 3.0f, 13.0f, hovered ? lightAccent : kText, shown, 76.0f);
                };
                slider(g_lightDraft.attachedTarget ? "Offset X" : "X", "##LightX", g_lightDraft.position.x, 0, -10000.0f, 10000.0f, 0.01f, "%.3f", propsY + 61.0f, x + 30.0f, width - 60.0f);
                slider(g_lightDraft.attachedTarget ? "Offset Y" : "Y", "##LightY", g_lightDraft.position.y, 1, -10000.0f, 10000.0f, 0.01f, "%.3f", propsY + 103.0f, x + 30.0f, width - 60.0f);
                slider(g_lightDraft.attachedTarget ? "Offset Z" : "Z", "##LightZ", g_lightDraft.position.z, 2, -10000.0f, 10000.0f, 0.01f, "%.3f", propsY + 145.0f, x + 30.0f, width - 60.0f);
                slider("Intensity", "##LightIntensity", g_lightDraft.intensity, 4, 0.0f, 300.0f, 0.20f, "%.2f", propsY + 187.0f, x + 30.0f, width - 60.0f);
                slider("Range", "##LightRange", g_lightDraft.range, 3, 0.5f, 250.0f, 0.50f, "%.2f m", propsY + 229.0f, x + 30.0f, width - 60.0f);
                Text(draw, x + 30.0f, propsY + 270.0f, 10.0f, kMuted, "QUICK COLOR TEMPERATURE");
                static constexpr const char* temperatureNames[] = {"CANDLE", "TUNGSTEN", "NEUTRAL", "DAYLIGHT", "MOON"};
                static constexpr float temperatures[] = {1900.0f, 2700.0f, 4500.0f, 5600.0f, 8000.0f};
                const float tempWidth = (width - 60.0f - 20.0f) / 5.0f;
                for (int i = 0; i < 5; ++i)
                {
                    if (button(temperatureNames[i], ("##LightTemp" + std::to_string(i)).c_str(),
                            x + 30.0f + i * (tempWidth + 5.0f), propsY + 281.0f, tempWidth, 20.0f))
                    {
                        const auto rgbTemp = LightTemperatureRgb(temperatures[i]);
                        g_lightDraft.colorR = static_cast<std::uint32_t>(rgbTemp[0]);
                        g_lightDraft.colorG = static_cast<std::uint32_t>(rgbTemp[1]);
                        g_lightDraft.colorB = static_cast<std::uint32_t>(rgbTemp[2]);
                        changed = true;
                    }
                }
                int rgb[3] = {static_cast<int>(g_lightDraft.colorR), static_cast<int>(g_lightDraft.colorG), static_cast<int>(g_lightDraft.colorB)};
                const char* rgbNames[3] = {"Red", "Green", "Blue"};
                for (int c = 0; c < 3; ++c)
                {
                    const float rowY = propsY + 304.0f + c * 21.0f;
                    const float rowLeft = x + 30.0f, rowWidth = width - 60.0f;
                    const float barL = rowLeft + 78.0f, barR = rowLeft + rowWidth - 48.0f;
                    ImGui::SetCursorScreenPos(P(barL, rowY + 4.0f));
                    ImGui::InvisibleButton(("##LightRGB" + std::to_string(c)).c_str(), ImVec2(S(barR - barL), S(12.0f)));
                    const bool rowHovered = ImGui::IsItemHovered();
                    const bool clicked = rowHovered && MouseClicked();
                    const bool dragging = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
                    RowBox(draw, rowLeft, rowY, rowLeft + rowWidth, rowY + 20.0f,
                        HoverShown(rowHovered) ? RowLook::Hovered : RowLook::Rest, g_lightMenuPreset, lightAccent, lightHoverInk);
                    if (clicked || dragging)
                    {
                        const float t = std::clamp((ImGui::GetIO().MousePos.x - P(barL, rowY + 4.0f).x) /
                            std::max(1.0f, S(barR - barL)), 0.0f, 1.0f);
                        rgb[c] = static_cast<int>(std::lround(t * 255.0f)); changed = true;
                    }
                    Text(draw, rowLeft + 12.0f, rowY + 3.0f, 12.0f, kText, rgbNames[c]);
                    draw->AddRectFilled(P(barL, rowY + 8.0f), P(barR, rowY + 12.0f), IM_COL32(236, 230, 218, 60));
                    draw->AddRectFilled(P(barL, rowY + 8.0f), P(barL + (barR - barL) * rgb[c] / 255.0f, rowY + 12.0f), lightAccent);
                    draw->AddCircleFilled(P(barL + (barR - barL) * rgb[c] / 255.0f, rowY + 10.0f), S(4.5f), kText);
                    char value[8]{}; std::snprintf(value, sizeof(value), "%d", rgb[c]);
                    Text(draw, barR + 8.0f, rowY + 3.0f, 12.0f, kText, value);
                }
                g_lightDraft.colorR = static_cast<std::uint32_t>(rgb[0]);
                g_lightDraft.colorG = static_cast<std::uint32_t>(rgb[1]);
                g_lightDraft.colorB = static_cast<std::uint32_t>(rgb[2]);

                if (button("ROTATION / ADVANCED  >", "##LightAdvancedOpen", x + 28.0f, propsY + 374.0f, width - 56.0f, 34.0f))
                {
                    g_lightAdvancedOpen = true; PlayUi(UiSound::Select);
                }

                if (focusMarker)
                {
                    Text(draw, x + 30.0f, propsY + 419.0f, 14.0f, kMuted, "OPTIONAL: REPLACE LIGHTS FROM ANOTHER MARKER");
                    const auto stepCopySource = [&](int direction) {
                        const int markerCount = static_cast<int>(g_snapshot.cameraKeyframeCount);
                        if (markerCount <= 0)
                            return;
                        int index = -1;
                        for (int n = 0; n < markerCount; ++n)
                            if (g_snapshot.cameraKeyframes[n].id == g_lightCopySourceMarkerId)
                                index = n;
                        if (index < 0)
                            for (int n = 0; n < markerCount; ++n)
                                if (g_snapshot.cameraKeyframes[n].id == focusMarker)
                                {
                                    index = n;
                                    break;
                                }
                        for (int step = 0; step < markerCount; ++step)
                        {
                            index = (index + direction + markerCount) % markerCount;
                            const std::uint32_t candidate = g_snapshot.cameraKeyframes[index].id;
                            if (candidate != focusMarker)
                            {
                                g_lightCopySourceMarkerId = candidate;
                                return;
                            }
                        }
                    };
                    if (button("<", "##LightCopyLeft", x + 30.0f, propsY + 440.0f, 42.0f, 36.0f))
                    {
                        stepCopySource(-1);
                    }
                    if (button(">", "##LightCopyRight", x + 78.0f, propsY + 440.0f, 42.0f, 36.0f))
                    {
                        stepCopySource(1);
                    }
                    const std::string sourceLabel = g_lightCopySourceMarkerId ? markerLabel(g_lightCopySourceMarkerId) : std::string("No previous marker");
                    TextFit(draw, x + 130.0f, propsY + 450.0f, 14.0f, kText, sourceLabel.c_str(), 100.0f);
                    if (button("REPLACE LIGHTS", "##LightCopy", x + width - 252.0f, propsY + 440.0f, 110.0f, 36.0f,
                            g_lightCopySourceMarkerId && g_lightCopySourceMarkerId != focusMarker))
                    {
                        Command command{}; command.keyframeId = g_lightCopySourceMarkerId; command.target = focusMarker;
                        if (g_lightCopySourceMarkerId && g_lightCopySourceMarkerId != focusMarker && Send(CommandType::LightCopyMarker, &command))
                        { g_lightDraftIndex = -1; g_lightDraftDirty = false; PlayUi(UiSound::Select); }
                    }
                    if (button(g_lightPreviewVisible ? "PREVIEW: ON" : "PREVIEW: OFF", "##LightPreviewToggle",
                            x + width - 136.0f, propsY + 440.0f, 108.0f, 36.0f))
                    {
                        g_lightPreviewVisible = !g_lightPreviewVisible;
                        saveLightPreviewSettings();
                        PlayUi(UiSound::Select);
                    }
                }
                else if (button(g_lightPreviewVisible ? "PREVIEW: ON" : "PREVIEW: OFF", "##LightPreviewToggleNoMarker",
                        x + width - 136.0f, propsY + 440.0f, 108.0f, 36.0f))
                {
                    g_lightPreviewVisible = !g_lightPreviewVisible;
                    saveLightPreviewSettings();
                    PlayUi(UiSound::Select);
                }
                if (changed)
                {
                    Command command{}; command.keyframeId = selectedSourceIndex; command.authoredLight = g_lightDraft;
                    g_lightDraftDirty = Send(CommandType::LightUpdate, &command);
                }
                }
            }
            else
            {
                if (selectionPending)
                    TextFit(draw, x + 30.0f, propsY + 72.0f, 14.0f, kMuted, "ADDING LIGHT TO THIS MARKER...", width - 60.0f);
                else if (button("ADD LIGHT AT CAMERA", "##LightEmptyAdd", x + 24.0f, propsY + 62.0f, width - 48.0f, 36.0f))
                {
                    AuthoredLight light{};
                    light.id = NextSnapshotLightId();
                    std::snprintf(light.name, sizeof(light.name), "Light %02u", totalCount + 1);
                    light.markerId = focusMarker;
                    const float pitch = g_snapshot.camera.rotation.x * 0.01745329251994329577f;
                    const float yaw = g_snapshot.camera.rotation.z * 0.01745329251994329577f;
                    const float cp = std::cos(pitch);
                    light.position.x = g_snapshot.camera.position.x - std::sin(yaw) * cp * 4.0f;
                    light.position.y = g_snapshot.camera.position.y + std::cos(yaw) * cp * 4.0f;
                    light.position.z = g_snapshot.camera.position.z + std::sin(pitch) * 4.0f;
                    light.range = 30.0f;
                    light.intensity = 75.0f;
                    light.colorR = light.colorG = light.colorB = 255; light.enabled = 1; light.beamAngle = 35.0f;
                    light.attachType = static_cast<std::uint32_t>(AttachType::Default);
                    light.aimPitch = g_snapshot.camera.rotation.x;
                    light.aimYaw = g_snapshot.camera.rotation.z;
                    light.blend = static_cast<std::uint32_t>(replay::editor::Blend::Spline);
                    Command command{}; command.authoredLight = light;
                    if (Send(CommandType::LightAdd, &command))
                    {
                        g_lightSelectedRigGroupId = 0;
                        g_lightRigSelectionPendingAt = GetTickCount64();
                        g_lightRigEditDraft = {};
                        g_lightSelected = rowCount;
                        g_lightSelectedId = light.id;
                        g_lightDraft = light;
                        g_lightDraftIndex = -1;
                        g_lightDraftDirty = false;
                        PlayUi(UiSound::Select);
                    }
                }

                Text(draw, x + 30.0f, propsY + 419.0f, 14.0f, kMuted, "OPTIONAL: REPLACE LIGHTS FROM ANOTHER MARKER");
                const auto stepCopySource = [&](int direction) {
                    const int markerCount = static_cast<int>(g_snapshot.cameraKeyframeCount);
                    if (!focusMarker || markerCount <= 1)
                        return;
                    int index = -1;
                    for (int n = 0; n < markerCount; ++n)
                        if (g_snapshot.cameraKeyframes[n].id == g_lightCopySourceMarkerId)
                            index = n;
                    if (index < 0)
                        for (int n = 0; n < markerCount; ++n)
                            if (g_snapshot.cameraKeyframes[n].id == focusMarker)
                            {
                                index = n;
                                break;
                            }
                    for (int step = 0; step < markerCount; ++step)
                    {
                        index = (index + direction + markerCount) % markerCount;
                        const std::uint32_t candidate = g_snapshot.cameraKeyframes[index].id;
                        if (candidate != focusMarker)
                        {
                            g_lightCopySourceMarkerId = candidate;
                            return;
                        }
                    }
                };
                if (button("<", "##LightCopyLeftEmpty", x + 30.0f, propsY + 440.0f, 42.0f, 36.0f, focusMarker != 0))
                    stepCopySource(-1);
                if (button(">", "##LightCopyRightEmpty", x + 78.0f, propsY + 440.0f, 42.0f, 36.0f, focusMarker != 0))
                    stepCopySource(1);
                const std::string sourceLabel = g_lightCopySourceMarkerId ? markerLabel(g_lightCopySourceMarkerId) : std::string("No previous marker");
                TextFit(draw, x + 130.0f, propsY + 450.0f, 14.0f, kText, sourceLabel.c_str(), 100.0f);
                if (button("REPLACE LIGHTS", "##LightCopyEmpty", x + width - 252.0f, propsY + 440.0f, 110.0f, 36.0f,
                        focusMarker && g_lightCopySourceMarkerId && g_lightCopySourceMarkerId != focusMarker))
                {
                    Command command{}; command.keyframeId = g_lightCopySourceMarkerId; command.target = focusMarker;
                    if (g_lightCopySourceMarkerId && g_lightCopySourceMarkerId != focusMarker && Send(CommandType::LightCopyMarker, &command))
                    { g_lightDraftIndex = -1; g_lightDraftDirty = false; PlayUi(UiSound::Select); }
                }
                if (button(g_lightPreviewVisible ? "PREVIEW: ON" : "PREVIEW: OFF", "##LightPreviewToggleEmpty",
                        x + width - 136.0f, propsY + 440.0f, 108.0f, 36.0f))
                {
                    g_lightPreviewVisible = !g_lightPreviewVisible;
                    saveLightPreviewSettings();
                    PlayUi(UiSound::Select);
                }
            }
        }

        if (changed && g_lightAdvancedOpen && count > 0 && !deletedSelectedLight)
        {
            Command command{}; command.keyframeId = selectedSourceIndex; command.authoredLight = g_lightDraft;
            g_lightDraftDirty = Send(CommandType::LightUpdate, &command);
        }

        // The optional light preview is shared by every menu look and remains
        // visible while editing advanced or spline properties.
        if (g_lightPreviewVisible)
        {
            const float defaultX = x + width + 22.0f;
            const float defaultY = top + 126.0f;
            const float requestedWidth = std::clamp(g_lightPreviewWidth, 220.0f, 1200.0f);
            const float requestedHeight = std::clamp(g_lightPreviewHeight, 220.0f, 900.0f);
            const float maxX = std::max(12.0f, g_vw - std::min(requestedWidth, std::max(220.0f, g_vw - 24.0f)) - 12.0f);
            const float previewX = std::clamp(g_lightPreviewX >= 0.0f ? g_lightPreviewX : defaultX, 12.0f, maxX);
            const float available = std::max(220.0f, g_vw - previewX - 12.0f);
            const float previewWidth = std::clamp(requestedWidth, 220.0f, available);
            const float maxY = std::max(12.0f, g_vh - std::min(requestedHeight, std::max(220.0f, g_vh - 24.0f)) - 12.0f);
            const float previewY = std::clamp(g_lightPreviewY >= 0.0f ? g_lightPreviewY : defaultY, 12.0f, maxY);
            const float maxHeight = std::max(220.0f, g_vh - previewY - 12.0f);
            const float previewHeight = std::clamp(requestedHeight, 220.0f, maxHeight);
                const ImU32 previewBg = kLightPresetPreview[g_lightMenuPreset];
                const ImU32 previewEdge = WithAlpha(lightAccent, 0.78f);
                draw->AddRectFilled(P(previewX, previewY), P(previewX + previewWidth, previewY + previewHeight), previewBg, S(4.0f));
                draw->AddRect(P(previewX, previewY), P(previewX + previewWidth, previewY + previewHeight),
                    previewEdge, S(4.0f), 0, S(1.4f));
                ImGui::SetCursorScreenPos(P(previewX + 5.0f, previewY + 4.0f));
                ImGui::InvisibleButton("##LightPreviewMove", ImVec2(S(std::max(40.0f, previewWidth - 130.0f)), S(32.0f)));
                const bool moveHovered = ImGui::IsItemHovered();
                const bool moveActive = ImGui::IsItemActive();
                if (moveHovered)
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (moveActive && MouseClicked())
                    g_lightPreviewMoving = true;
                if (g_lightPreviewMoving && ImGui::IsMouseDown(ImGuiMouseButton_Left))
                {
                    const ImVec2 delta = ImGui::GetIO().MouseDelta;
                    g_lightPreviewX = std::clamp(previewX + delta.x / std::max(0.001f, g_scale), 12.0f,
                        std::max(12.0f, g_vw - previewWidth - 12.0f));
                    g_lightPreviewY = std::clamp(previewY + delta.y / std::max(0.001f, g_scale), 12.0f,
                        std::max(12.0f, g_vh - previewHeight - 12.0f));
                }
                if (g_lightPreviewMoving && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                {
                    g_lightPreviewMoving = false;
                    saveLightPreviewSettings();
                }

                constexpr float kPi = 3.14159265358979323846f;
                const float cameraYaw = g_snapshot.camera.rotation.z * kPi / 180.0f;
                const float cameraPitch = g_snapshot.camera.rotation.x * kPi / 180.0f;
                const float cameraRightX = std::cos(cameraYaw);
                const float cameraRightY = std::sin(cameraYaw);
                const float cameraForwardX = -std::sin(cameraYaw) * std::cos(cameraPitch);
                const float cameraForwardY = std::cos(cameraYaw) * std::cos(cameraPitch);
                const float cameraForwardZ = std::sin(cameraPitch);
                const auto projectPosition = [&](const AuthoredLight& light) {
                    const float dx = light.position.x - (light.attachedTarget ? 0.0f : g_snapshot.camera.position.x);
                    const float dy = light.position.y - (light.attachedTarget ? 0.0f : g_snapshot.camera.position.y);
                    const float dz = light.position.z - (light.attachedTarget ? 0.0f : g_snapshot.camera.position.z);
                    return ImVec2(dx * cameraRightX + dy * cameraRightY,
                        dx * cameraForwardX + dy * cameraForwardY + dz * cameraForwardZ);
                };
                std::vector<const AuthoredLight*> previewLights;
                std::vector<std::uint32_t> previewLightIndices;
                previewLights.reserve(count);
                previewLightIndices.reserve(count);
                std::uint32_t activeCount = 0;
                float extent = 30.0f;
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    const int sourceIndex = static_cast<int>(lightIndices[static_cast<std::size_t>(i)]);
                    const int sourceRow = rowForSourceIndex(static_cast<std::uint32_t>(sourceIndex));
                    if (g_lightPreviewSolo && sourceRow != g_lightSelected)
                        continue;
                    const AuthoredLight* light = sourceIndex == static_cast<int>(selectedSourceIndex)
                        ? &g_lightDraft : &g_snapshot.authoredLights[lightIndices[static_cast<std::size_t>(i)]];
                    if (!light->enabled && !g_lightPreviewSolo)
                        continue;
                    previewLights.push_back(light);
                    previewLightIndices.push_back(i);
                    if (light->enabled)
                        ++activeCount;
                    const ImVec2 local = projectPosition(*light);
                    const float range = std::clamp(light->range, 0.5f, 250.0f);
                    extent = std::max(extent, std::abs(local.x) + range);
                    extent = std::max(extent, std::abs(local.y) + range);
                }

                char countText[64]{};
                if (g_lightPreviewSolo)
                    std::snprintf(countText, sizeof(countText), "LIGHT PREVIEW  |  SOLO LIGHT");
                else
                    std::snprintf(countText, sizeof(countText), "LIGHT PREVIEW  |  %u ACTIVE / %u TOTAL", activeCount, count);
                TextFit(draw, previewX + 14.0f, previewY + 12.0f, 14.0f, lightAccent, countText, previewWidth - 146.0f);
                if (button(g_lightPreviewSolo ? "SOLO: ON" : "SOLO: OFF", "##LightPreviewSolo",
                        previewX + previewWidth - 116.0f, previewY + 7.0f, 102.0f, 26.0f))
                {
                    g_lightPreviewSolo = !g_lightPreviewSolo;
                    saveLightPreviewSettings();
                    PlayUi(UiSound::Select);
                }
                const float canvasL = previewX + 12.0f, canvasT = previewY + 40.0f;
                const float canvasR = previewX + previewWidth - 12.0f, canvasB = previewY + previewHeight - 40.0f;
                const float canvasW = canvasR - canvasL, canvasH = canvasB - canvasT;
                draw->AddRectFilled(P(canvasL, canvasT), P(canvasR, canvasB), IM_COL32(3, 10, 16, 240), S(2.0f));
                draw->AddRect(P(canvasL, canvasT), P(canvasR, canvasB), WithAlpha(lightAccent, 0.34f), S(2.0f), 0, S(0.8f));
                draw->PushClipRect(P(canvasL + 1.0f, canvasT + 1.0f), P(canvasR - 1.0f, canvasB - 1.0f), true);
                const ImU32 grid = WithAlpha(lightAccent, 0.15f);
                for (float gx = canvasL + 16.0f; gx < canvasR; gx += 24.0f)
                    draw->AddLine(P(gx, canvasT), P(gx, canvasB), grid, S(0.7f));
                for (float gy = canvasT + 12.0f; gy < canvasB; gy += 24.0f)
                    draw->AddLine(P(canvasL, gy), P(canvasR, gy), grid, S(0.7f));

                const float scale = std::min(canvasW * 0.44f, canvasH * 0.43f) / extent;
                const float centerX = (canvasL + canvasR) * 0.5f;
                const float centerY = (canvasT + canvasB) * 0.5f;
                draw->AddLine(P(centerX, canvasT), P(centerX, canvasB), WithAlpha(kText, 0.30f), S(1.0f));
                draw->AddLine(P(canvasL, centerY), P(canvasR, centerY), WithAlpha(kText, 0.30f), S(1.0f));
                for (std::size_t i = 0; i < previewLights.size(); ++i)
                {
                    const AuthoredLight& light = *previewLights[i];
                    const ImVec2 local = projectPosition(light);
                    const float sourceX = centerX + local.x * scale;
                    const float sourceY = centerY - local.y * scale;
                    const float range = std::clamp(light.range, 0.5f, 250.0f);
                    const float radius = std::clamp(range * scale, 5.0f, std::min(canvasW, canvasH) * 0.46f);
                    const ImU32 rgb = IM_COL32(
                        static_cast<int>(std::clamp(light.colorR, 0u, 255u)),
                        static_cast<int>(std::clamp(light.colorG, 0u, 255u)),
                        static_cast<int>(std::clamp(light.colorB, 0u, 255u)), 255);
                    if (!light.enabled)
                    {
                        draw->AddCircle(P(sourceX, sourceY), S(6.0f), IM_COL32(142, 144, 143, 140), 20, S(1.0f));
                    }
                    else
                    {
                        const float power = std::clamp(light.intensity / 100.0f, 0.0f, 3.0f);
                        const float glowAlpha = 0.05f + power * 0.10f;
                        draw->AddCircleFilled(P(sourceX, sourceY), S(radius), WithAlpha(rgb, glowAlpha), 48);
                        draw->AddCircle(P(sourceX, sourceY), S(radius), WithAlpha(rgb, 0.28f + power * 0.42f), 48, S(1.0f));

                        const bool beam = light.coneEnabled || light.reflectorEnabled;
                        if (beam)
                        {
                            const float yaw = light.aimYaw * kPi / 180.0f;
                            const float pitch = light.aimPitch * kPi / 180.0f;
                            const float worldDirX = -std::sin(yaw) * std::cos(pitch);
                            const float worldDirY = std::cos(yaw) * std::cos(pitch);
                            const float worldDirZ = std::sin(pitch);
                            const float dirRight = worldDirX * cameraRightX + worldDirY * cameraRightY;
                            const float dirForward = worldDirX * cameraForwardX + worldDirY * cameraForwardY + worldDirZ * cameraForwardZ;
                            const float dirLength = std::max(0.001f, std::sqrt(dirRight * dirRight + dirForward * dirForward));
                            const float dirX = dirRight / dirLength;
                            const float dirY = -dirForward / dirLength;
                            const float planarLength = std::max(5.0f, radius * dirLength);
                            const float endX = sourceX + dirX * planarLength;
                            const float endY = sourceY + dirY * planarLength;
                            const float halfWidth = std::min(std::min(canvasW, canvasH) * 0.42f,
                                std::tan(std::clamp(light.beamAngle, 1.0f, 170.0f) * 0.00872664626f) * planarLength);
                            const float sideX = -dirY * halfWidth, sideY = dirX * halfWidth;
                            const ImVec2 cone[] = {P(sourceX, sourceY), P(endX + sideX, endY + sideY), P(endX - sideX, endY - sideY)};
                            draw->AddConvexPolyFilled(cone, 3, WithAlpha(rgb, 0.07f + power * 0.15f));
                            draw->AddLine(cone[0], cone[1], WithAlpha(rgb, 0.30f + power * 0.48f), S(1.0f));
                            draw->AddLine(cone[0], cone[2], WithAlpha(rgb, 0.30f + power * 0.48f), S(1.0f));
                            draw->AddCircleFilled(P(endX, endY), S(5.0f + power * 5.0f), WithAlpha(rgb, 0.10f + power * 0.22f), 24);
                        }
                        else
                        {
                            draw->AddCircleFilled(P(sourceX, sourceY), S(std::max(5.0f, radius * 0.38f)), WithAlpha(rgb, 0.08f + power * 0.17f), 32);
                        }
                    }

                    const std::uint32_t uiIndex = previewLightIndices[i];
                    const std::uint32_t sourceIndex = lightIndices[uiIndex];
                    const int sourceRow = rowForSourceIndex(sourceIndex);
                    const bool selected = sourceRow == g_lightSelected;
                    draw->AddCircle(P(sourceX, sourceY), S(selected ? 7.0f : 5.0f), selected ? kText : rgb, 20, S(selected ? 1.5f : 1.0f));
                    draw->AddCircleFilled(P(sourceX, sourceY), S(2.5f), rgb);
                    if (selected)
                        TextFit(draw, sourceX + 8.0f, sourceY - 12.0f, 10.0f, kText, light.name, 92.0f);

                    const std::string handleId = "##LightPreviewHandle" + std::to_string(light.id ? light.id : uiIndex + 1);
                    ImGui::SetCursorScreenPos(P(sourceX - 10.0f, sourceY - 10.0f));
                    ImGui::InvisibleButton(handleId.c_str(), ImVec2(S(20.0f), S(20.0f)));
                    const bool hovered = ImGui::IsItemHovered();
                    const bool active = ImGui::IsItemActive();
                    if (hovered)
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (hovered && MouseClicked() && !selected)
                    {
                        if (sourceRow >= 0)
                        {
                            g_lightSelected = sourceRow;
                            g_lightSelectedRigGroupId = lightRows[static_cast<std::size_t>(sourceRow)].rigGroupId;
                        }
                        g_lightSelectedId = light.id;
                        g_lightDraft = light;
                        if (g_lightDraft.reflectorEnabled)
                        {
                            g_lightDraft.coneEnabled = 1;
                            g_lightDraft.reflectorEnabled = 0;
                            g_lightDraft.reflectorHidden = 0;
                        }
                        g_lightDraftIndex = static_cast<int>(sourceIndex);
                        g_lightDraftDirty = false;
                        g_lightRigEditDraft = {};
                    }
                    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && count > 0)
                    {
                        if (sourceRow >= 0 && g_lightSelected != sourceRow)
                        {
                            g_lightSelected = sourceRow;
                            g_lightSelectedRigGroupId = lightRows[static_cast<std::size_t>(sourceRow)].rigGroupId;
                            g_lightSelectedId = light.id;
                            g_lightDraft = light;
                            g_lightDraftIndex = static_cast<int>(sourceIndex);
                            g_lightDraftDirty = false;
                            g_lightRigEditDraft = {};
                        }
                        const ImVec2 delta = ImGui::GetIO().MouseDelta;
                        const float movementScale = ImGui::GetIO().KeyCtrl ? 0.1f : 1.0f;
                        const float moveRight = delta.x * movementScale / std::max(0.001f, g_scale * scale);
                        const float moveForward = -delta.y * movementScale / std::max(0.001f, g_scale * scale);
                        if (sourceRow >= 0 && lightRows[static_cast<std::size_t>(sourceRow)].rigGroupId)
                        {
                            const LightRigGroup* group = FindLightRigGroup(lightRows[static_cast<std::size_t>(sourceRow)].rigGroupId);
                            if (group && (!g_lightRigEditDraft.valid || g_lightRigEditDraft.groupId != group->id ||
                                g_lightRigEditDraft.markerId != focusMarker))
                                BeginLightRigEditDraft(*group, lightRows[static_cast<std::size_t>(sourceRow)], focusMarker);
                            if (g_lightRigEditDraft.valid)
                            {
                                g_lightRigEditDraft.center.x += cameraRightX * moveRight + cameraForwardX * moveForward;
                                g_lightRigEditDraft.center.y += cameraRightY * moveRight + cameraForwardY * moveForward;
                                g_lightRigEditDraft.center.z += cameraForwardZ * moveForward;
                                applyLightRigDraft();
                            }
                        }
                        else
                        {
                            g_lightDraft.position.x += cameraRightX * moveRight + cameraForwardX * moveForward;
                            g_lightDraft.position.y += cameraRightY * moveRight + cameraForwardY * moveForward;
                            g_lightDraft.position.z += cameraForwardZ * moveForward;
                            Command command{};
                            command.keyframeId = sourceIndex;
                            command.authoredLight = g_lightDraft;
                            g_lightDraftDirty = Send(CommandType::LightUpdate, &command);
                        }
                    }
                }
                draw->PopClipRect();
                draw->AddLine(P(previewX + 14.0f, previewY + previewHeight - 24.0f), P(previewX + previewWidth - 14.0f, previewY + previewHeight - 24.0f),
                    WithAlpha(lightAccent, 0.48f), S(1.0f));
                TextFit(draw, previewX + 14.0f, previewY + previewHeight - 19.0f, 10.0f, kMuted,
                    g_lightPreviewSolo ? "SOLO PREVIEW  |  DRAG MOVE  |  CTRL: FINE" :
                    activeCount ? "CLICK SELECT  |  DRAG MOVE  |  CTRL: FINE" :
                        count ? "ENABLE A LIGHT TO SHOW IT HERE" : "ADD A LIGHT TO BUILD THE LIVE PREVIEW",
                    previewWidth - 48.0f);

                const float grip = 20.0f;
                const float gripX = previewX + previewWidth - grip;
                const float gripY = previewY + previewHeight - grip;
                ImGui::SetCursorScreenPos(P(gripX, gripY));
                ImGui::InvisibleButton("##LightPreviewResize", ImVec2(S(grip), S(grip)));
                const bool resizeHovered = ImGui::IsItemHovered();
                const bool resizeActive = ImGui::IsItemActive();
                if (resizeHovered || resizeActive)
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
                draw->AddLine(P(gripX + 8.0f, gripY + 17.0f), P(gripX + 17.0f, gripY + 8.0f), lightAccent, S(1.4f));
                draw->AddLine(P(gripX + 13.0f, gripY + 17.0f), P(gripX + 17.0f, gripY + 13.0f), lightAccent, S(1.4f));
                if (resizeActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                {
                    const ImVec2 delta = ImGui::GetIO().MouseDelta;
                    g_lightPreviewWidth = std::clamp(previewWidth + delta.x / g_scale, 220.0f, available);
                    g_lightPreviewHeight = std::clamp(previewHeight + delta.y / g_scale, 220.0f, maxHeight);
                }
                if (resizeActive && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                    saveLightPreviewSettings();
        }

        TextFit(draw, x + 24.0f, bottom - 48.0f, 11.0f, kMuted,
            "F11 FREE CAM ON/OFF  |  F2 REOPEN LIGHTING  |  GIZMOS PREVIEW ONLY", width - 48.0f);
        TextFit(draw, x + 24.0f, bottom - 28.0f, 11.0f, kMuted,
            "F6 CLOSE  |  K PLAY/PAUSE  |  , / . FRAME  |  CTRL+Z UNDO  |  CTRL+Y REDO", width - 48.0f);

        if (g_lightTargetPickerOpen)
        {
            LoadLightTargetFavorites();
            const float modalW = 470.0f, modalH = 520.0f;
            const ImGuiWindowFlags pickerFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
            ImGui::SetNextWindowPos(P(std::max(12.0f, (g_vw - modalW) * 0.5f), std::max(12.0f, (g_vh - modalH) * 0.5f)));
            ImGui::SetNextWindowSize(ImVec2(S(modalW), S(modalH)));
            const ImVec4 accent = ImGui::ColorConvertU32ToFloat4(lightAccent);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, lightPopupBackground);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(accent.x, accent.y, accent.z, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.06f, 0.07f, 0.07f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(accent.x * 0.35f, accent.y * 0.35f, accent.z * 0.35f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(accent.x * 0.65f, accent.y * 0.65f, accent.z * 0.65f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, S(1.5f));
            if (ImGui::Begin("##LightCharacterPicker", nullptr, pickerFlags))
            {
                ImGui::TextColored(accent, g_lightTargetPickerMode == 0 ? "AIM AT CHARACTER" : "ATTACH LIGHT TO CHARACTER");
                ImGui::Separator();
                ImGui::SetNextItemWidth(S(292.0f));
                ImGui::InputText("##LightTargetSearch", g_lightTargetSearch, sizeof(g_lightTargetSearch));
                ImGui::SameLine();
                if (ImGui::Button("REFRESH"))
                    Send(CommandType::RefreshTargets);
                ImGui::SameLine();
                if (ImGui::Button("CLOSE"))
                    g_lightTargetPickerOpen = false;
                ImGui::TextDisabled(g_lightTargetPickerMode == 1 ?
                    "Only characters present at this marker can be attached now." :
                    "Characters from the whole replay are listed; inactive targets update when they appear.");
                if (g_lightTargetPickerMode == 0 && ImGui::Button("MANUAL AIM"))
                {
                    g_lightDraft.lookAtTarget = 0;
                    if (count > 0)
                    {
                        Command command{}; command.keyframeId = selectedSourceIndex; command.authoredLight = g_lightDraft;
                        g_lightDraftDirty = Send(CommandType::LightUpdate, &command);
                    }
                    g_lightTargetPickerOpen = false;
                }
                std::vector<std::uint32_t> candidates;
                std::string query(g_lightTargetSearch);
                std::transform(query.begin(), query.end(), query.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                for (std::uint32_t n = 0; n < g_snapshot.targetCount; ++n)
                {
                    const TargetInfo& info = g_snapshot.targets[n];
                    if (info.kind != 0u)
                        continue;
                    std::string label(info.label);
                    std::string lower = label;
                    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (query.empty() || lower.find(query) != std::string::npos)
                        candidates.push_back(n);
                }
                std::sort(candidates.begin(), candidates.end(), [](std::uint32_t a, std::uint32_t b) {
                    const TargetInfo& left = g_snapshot.targets[a];
                    const TargetInfo& right = g_snapshot.targets[b];
                    const bool favLeft = std::find(g_lightTargetFavorites.begin(), g_lightTargetFavorites.end(), left.label) != g_lightTargetFavorites.end();
                    const bool favRight = std::find(g_lightTargetFavorites.begin(), g_lightTargetFavorites.end(), right.label) != g_lightTargetFavorites.end();
                    if (favLeft != favRight) return favLeft;
                    if ((left.active != 0) != (right.active != 0)) return left.active != 0;
                    return _stricmp(left.label, right.label) < 0;
                });
                if (ImGui::BeginChild("##LightTargetRows", ImVec2(0.0f, S(g_lightTargetPickerMode == 0 ? 350.0f : 378.0f)), true))
                {
                    for (const std::uint32_t slot : candidates)
                    {
                        const TargetInfo& info = g_snapshot.targets[slot];
                        const bool favorite = std::find(g_lightTargetFavorites.begin(), g_lightTargetFavorites.end(), info.label) != g_lightTargetFavorites.end();
                        const bool selectable = g_lightTargetPickerMode == 0 || info.active != 0;
                        char row[112]{};
                        std::snprintf(row, sizeof(row), "%s  |  %s", info.label, info.active ? "HERE NOW" : "IN CLIP");
                        ImGui::PushStyleColor(ImGuiCol_Text, selectable ? ImVec4(0.93f, 0.90f, 0.85f, 1.0f) : ImVec4(0.52f, 0.50f, 0.47f, 1.0f));
                        if (ImGui::Selectable(row, false, 0, ImVec2(S(325.0f), S(29.0f))) && selectable)
                        {
                            if (g_lightTargetPickerMode == 0)
                                g_lightDraft.lookAtTarget = info.id;
                            else
                            {
                                g_lightDraft.attachedTarget = info.id;
                                BeginLightDraftAttachSync();
                            }
                            if (count > 0)
                            {
                                Command command{}; command.keyframeId = selectedSourceIndex; command.authoredLight = g_lightDraft;
                                g_lightDraftDirty = Send(CommandType::LightUpdate, &command);
                            }
                            g_lightTargetPickerOpen = false;
                        }
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                        const std::string favButton = std::string(favorite ? "FAV*##" : "FAV##") + std::to_string(slot);
                        if (ImGui::SmallButton(favButton.c_str()))
                        {
                            const auto found = std::find(g_lightTargetFavorites.begin(), g_lightTargetFavorites.end(), info.label);
                            if (found == g_lightTargetFavorites.end()) g_lightTargetFavorites.emplace_back(info.label);
                            else g_lightTargetFavorites.erase(found);
                            SaveLightTargetFavorites();
                        }
                    }
                    if (candidates.empty())
                        ImGui::TextDisabled("No matching characters in this replay.");
                }
                ImGui::EndChild();
            }
            ImGui::End();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(5);
        }

        if (g_lightNumericEditAxis >= 0)
        {
            const float px = x + 48.0f, py = top + 260.0f;
            const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
            ImGui::SetNextWindowPos(P(px, py));
            const ImVec4 accent = ImGui::ColorConvertU32ToFloat4(lightAccent);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(lightPopupBackground.x, lightPopupBackground.y,
                lightPopupBackground.z, 0.97f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.93f, 0.90f, 0.85f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.11f, 0.10f, 0.09f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(accent.x * 0.22f, accent.y * 0.22f, accent.z * 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(accent.x, accent.y, accent.z, 0.55f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(accent.x * 0.48f, accent.y * 0.48f, accent.z * 0.48f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(accent.x * 0.72f, accent.y * 0.72f, accent.z * 0.72f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent);
            if (ImGui::Begin("##LightExactValueWindow", nullptr, flags))
            {
                static constexpr const char* labels[] = {"X", "Y", "Z", "Range", "Intensity", "Aim pitch", "Aim yaw", "Beam angle", "Aim roll"};
                static constexpr const char* units[] = {"m", "m", "m", "m", "", "deg", "deg", "deg", "deg"};
                Text(ImGui::GetWindowDrawList(), 12.0f, 8.0f, 19.0f, lightAccent, "SET LIGHT VALUE");
                ImGui::Dummy(ImVec2(1.0f, S(26.0f)));
                ImGui::Text("%s (%s)", labels[g_lightNumericEditAxis], units[g_lightNumericEditAxis]);
                ImGui::SetNextItemWidth(S(250.0f));
                if (g_lightNumericEditFocus) { ImGui::SetKeyboardFocusHere(); g_lightNumericEditFocus = false; }
                ImGui::InputFloat("##LightExactValue", &g_lightNumericEditValue, 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_AutoSelectAll);
                const bool apply = ImGui::Button("APPLY"); ImGui::SameLine();
                const bool cancel = ImGui::Button("CANCEL");
                if ((apply || KeyPressed(ImGuiKey_Enter, false)) && std::isfinite(g_lightNumericEditValue) && count > 0)
                {
                    float* values[] = {&g_lightDraft.position.x, &g_lightDraft.position.y, &g_lightDraft.position.z,
                        &g_lightDraft.range, &g_lightDraft.intensity, &g_lightDraft.aimPitch, &g_lightDraft.aimYaw,
                        &g_lightDraft.beamAngle, &g_lightDraft.aimRoll};
                    *values[g_lightNumericEditAxis] = g_lightNumericEditValue;
                    g_lightNumericEditAxis = -1;
                    Command command{}; command.keyframeId = selectedSourceIndex; command.authoredLight = g_lightDraft;
                    g_lightDraftDirty = Send(CommandType::LightUpdate, &command);
                }
                else if (cancel || KeyPressed(ImGuiKey_Escape, false))
                    g_lightNumericEditAxis = -1;
            }
            ImGui::End();
            ImGui::PopStyleColor(8);
        }
    }

    void DrawSettings(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("Settings");
        const float columnWidth = 540.0f;
        draw->AddRectFilled(P(0.0f, 0.0f), P(columnWidth - 60.0f, g_vh), IM_COL32(10, 9, 8, 225));
        draw->AddRectFilledMultiColor(P(columnWidth - 60.0f, 0.0f), P(columnWidth + 220.0f, g_vh), IM_COL32(10, 9, 8, 225),
            IM_COL32(10, 9, 8, 0), IM_COL32(10, 9, 8, 0), IM_COL32(10, 9, 8, 225));
        // The title a sixth of the way down, raised (to 36 units) when the
        // list would reach the key bar: a larger interface scale has fewer
        // units of height (src/ui_scale.h). Past that the list scrolls.
        constexpr int kRows = 14;
        constexpr int kSections = 4;
        constexpr float kRowPitch = 50.0f, kSectionHead = 32.0f, kSectionGap = 10.0f;
        constexpr float kListHeight = kSections * (kSectionHead + kSectionGap) - kSectionGap + kRows * kRowPitch - 6.0f;
        const float listBottom = g_vh - g_footH - 16.0f;
        const float top = std::max(36.0f, std::min(std::max(60.0f, g_vh * 0.16f), listBottom - kListHeight - 120.0f));
        const char* const title = TR("settings.title", "REPLAY SETTINGS");
        TitleBanner(draw, 50.0f, top, 420.0f, 70.0f, 62.0f, 72.0f, top + 10.0f, 44.0f, title); // as wide as a long title
        OrnamentDivider(draw, 60.0f, top + 104.0f, 400.0f);

        static const std::uint32_t kSeconds[] = {30, 60, 90, 120, 180, 300};
        // The last step is the core's ceiling (buffercov::kBufferMaxMB): a saved
        // buffer must stay under the 1 GiB a clip file may hold.
        static const std::uint32_t kMegabytes[] = {64, 128, 256, 512, 960};
        bool enabled = g_snapshot.bufferEnabled != 0;
        std::uint32_t seconds = g_snapshot.bufferSeconds ? g_snapshot.bufferSeconds : 60;
        std::uint32_t megabytes = g_snapshot.bufferMB ? g_snapshot.bufferMB : 128;
        // A click changes the option (not only selects the row); clicks go forward
        // and wrap at the end, the arrows stop at both ends.
        bool wrap = false;
        auto step = [&wrap](const std::uint32_t* values, int count, std::uint32_t current, int direction) {
            int at = 0;
            for (int n = 0; n < count; ++n)
                if (values[n] <= current)
                    at = n;
            at = wrap && at + direction >= count ? 0 : std::clamp(at + direction, 0, count - 1);
            return values[at];
        };
        const auto onOff = [](bool on) { return on ? TR("common.on_cap", "On") : TR("common.off_cap", "Off"); };
        std::string row0, row1, row2, row3, row4, row5, row6, row7, row8, row9, row10;
        row0 = TRF("settings.buffer", "Replay Buffer: {state}", {{"state", onOff(enabled)}});
        row1 = TRF("settings.length", "Length: {seconds} seconds", {{"seconds", std::to_string(seconds)}});
        row2 = TRF("settings.memory", "Memory: {mb} MB", {{"mb", std::to_string(megabytes)}});
        const bool deadEye = Flag(SnapshotReplayDeadEye);
        row3 = TRF("settings.dead_eye", "Dead Eye in replay: {state}", {{"state", onOff(deadEye)}});
        // Clip compression.
        static const std::uint32_t kDays[] = {1, 2, 3, 5, 7, 10, 14, 21, 30, 45, 60};
        const bool compress = g_snapshot.compressEnabled != 0;
        const std::uint32_t days = g_snapshot.compressDays ? g_snapshot.compressDays : 1;
        row4 = TRF("settings.compress", "Auto-compress clips: {state}", {{"state", onOff(compress)}});
        row5 = TRF("settings.compress_after", "Compress after: {count} {unit}",
            {{"count", std::to_string(days)}, {"unit", days == 1 ? TR("settings.day", "day") : TR("settings.days", "days")}});
        if (g_snapshot.compressState == 2)
            row6 = TRF("settings.compressing", "Compressing {done} of {total}...",
                {{"done", std::to_string(std::min(g_snapshot.compressDone + 1, g_snapshot.compressTotal))}, {"total", std::to_string(g_snapshot.compressTotal)}});
        else if (g_snapshot.compressState == 3)
            row6 = TR("settings.compress_waiting", "Compression waiting...");
        else
            row6 = TR("settings.compress_now", "Compress old clips now");
        // The recording gate.
        const bool recordCutscenes = Flag(SnapshotRecordCutscenes);
        const bool lockCutsceneCamera = Flag(SnapshotLockCutsceneCamera);
        row7 = TRF("settings.cutscenes", "Cutscenes: {state}",
            {{"state", recordCutscenes ? TR("settings.cutscenes.record", "Record") : TR("settings.cutscenes.skip", "Skip")}});
        row8 = TRF("settings.lock_camera", "Lock camera in cutscenes: {state}", {{"state", onOff(lockCutsceneCamera)}});
        // The world freeze of the paused replay.
        const bool freezeOnPause = Flag(SnapshotFreezeOnPause);
        row9 = TRF("settings.freeze", "Freeze world when paused: {state}", {{"state", onOff(freezeOnPause)}});
        // The recording indicator (src/rec_indicator.h).
        const recind::Mode indicator = WantedIndicator();
        const auto indicatorName = [](recind::Mode mode) {
            switch (mode)
            {
            case recind::Mode::Brief: return TR("settings.rec_indicator.brief", "Brief");
            case recind::Mode::Hidden: return TR("settings.rec_indicator.hidden", "Hidden from capture");
            case recind::Mode::Off: return TR("common.off_cap", "Off");
            default: return TR("settings.rec_indicator.normal", "Normal");
            }
        };
        row10 = TRF("settings.rec_indicator", "Recording indicator: {state}", {{"state", indicatorName(indicator)}});
        // The interface scale (src/ui_scale.h): the add-on's own, no command.
        // Last in the list (an INTERFACE group of its own once the rows are
        // grouped).
        std::string row11 = TRF("settings.interface_scale", "Interface scale: {pct}%", {{"pct", std::to_string(g_uiScalePercent)}});
        // The pose detail of the recording (pose_deadband_smooth.h Detail):
        // 1 standard, 2 maximum; 0 is a developer's old codec, shown as Standard.
        const bool maximumPose = g_hasSnapshot && g_snapshot.poseDetail == 2u;
        std::string row12 = TRF("settings.pose_detail", "Pose detail: {state}",
            {{"state", maximumPose ? TR("settings.pose_detail.maximum", "Maximum") : TR("settings.pose_detail.standard", "Standard")}});
        // The title face (display_font.h): Redemption, or Segoe UI Bold.
        const int titleFace = display_font::g_titleFace;
        const char* const faceName = titleFace == 1 ? "Segoe UI" : "Redemption";
        std::string row13 = TRF("settings.font", "Font: {name}", {{"name", faceName}});
        // The full texts head the help panel; the list shows each row's label
        // and value apart (SettingsRow).
        const char* rows[] = {row0.c_str(), row1.c_str(), row2.c_str(), row3.c_str(), row4.c_str(), row5.c_str(), row6.c_str(), row7.c_str(),
            row8.c_str(), row9.c_str(), row10.c_str(), row11.c_str(), row12.c_str(), row13.c_str()};
        const std::string lengthValue = TRF("settings.length.value", "{seconds} seconds", {{"seconds", std::to_string(seconds)}});
        const std::string memoryValue = TRF("settings.memory.value", "{mb} MB", {{"mb", std::to_string(megabytes)}});
        const std::string daysValue = TRF("settings.compress_after.value", "{count} {unit}",
            {{"count", std::to_string(days)}, {"unit", days == 1 ? TR("settings.day", "day") : TR("settings.days", "days")}});
        const std::string scaleValue = std::to_string(g_uiScalePercent) + "%";
        const char* const labels[] = {TR("settings.buffer.label", "Replay Buffer"), TR("settings.length.label", "Length"),
            TR("settings.memory.label", "Memory"), TR("settings.dead_eye.label", "Dead Eye in replay"),
            TR("settings.compress.label", "Auto-compress clips"), TR("settings.compress_after.label", "Compress after"), row6.c_str(),
            TR("settings.cutscenes.label", "Cutscenes"), TR("settings.lock_camera.label", "Lock camera in cutscenes"),
            TR("settings.freeze.label", "Freeze world when paused"), TR("settings.rec_indicator.label", "Recording indicator"),
            TR("settings.interface_scale.label", "Interface scale"), TR("settings.pose_detail.label", "Pose detail"),
            TR("settings.font.label", "Font")};
        const char* const values[] = {onOff(enabled), lengthValue.c_str(), memoryValue.c_str(), onOff(deadEye), onOff(compress), daysValue.c_str(), "",
            recordCutscenes ? TR("settings.cutscenes.record", "Record") : TR("settings.cutscenes.skip", "Skip"), onOff(lockCutsceneCamera),
            onOff(freezeOnPause), indicatorName(indicator), scaleValue.c_str(),
            maximumPose ? TR("settings.pose_detail.maximum", "Maximum") : TR("settings.pose_detail.standard", "Standard"), faceName};
        const int count = kRows;
        // The rows in four groups under a section title each. g_settingsIndex
        // stays the row's own number (its key handling and help below); the
        // order is only the list's: Up and Down walk it, the titles are not
        // in it and cannot be selected.
        struct SettingsSection
        {
            const char* title;
            int first, end; // into kOrder
            ui_art::Art icon;
        };
        static const int kOrder[count] = {0, 1, 2, 12, 7, 10, 3, 9, 8, 4, 5, 6, 11, 13};
        const SettingsSection sections[kSections] = {{TR("settings.section.recording", "RECORDING"), 0, 6, ui_art::Art::MenuRecord},
            {TR("settings.section.playback", "PLAYBACK"), 6, 9, ui_art::Art::MenuClips},
            {TR("settings.section.storage", "STORAGE"), 9, 12, ui_art::Art::MenuSaveBuffer},
            {TR("settings.section.interface", "INTERFACE"), 12, 14, ui_art::Art::MenuSettings}};
        int at = 0;
        for (int n = 0; n < count; ++n)
            if (kOrder[n] == g_settingsIndex)
                at = n;
        NavigateList(at, count);
        g_settingsIndex = kOrder[at];
        const float itemsTop = top + 126.0f;
        int direction = 0;
        if (KeyPressed(ImGuiKey_RightArrow))
            direction = 1;
        if (KeyPressed(ImGuiKey_LeftArrow))
            direction = -1;
        // The list between the divider and the key bar. When it does not fit
        // (a large interface scale on a short screen) it scrolls: a new
        // selection is brought into view (with its section's title when it
        // is the first row), the wheel over the column moves the list, and a
        // chevron says there is more past an end. Only whole entries are drawn.
        const float listTop = itemsTop - 6.0f;
        const float room = listBottom - listTop;
        const float maxScroll = std::max(0.0f, kListHeight - room);
        if (maxScroll <= 0.0f)
            g_settingsScroll = 0.0f;
        else
        {
            const ImGuiIO& io = ImGui::GetIO();
            if (io.MouseWheel != 0.0f && io.MousePos.x / g_scale - g_ox < columnWidth)
                g_settingsScroll -= io.MouseWheel * kRowPitch;
            if (g_settingsIndex != g_settingsScrolledTo)
            {
                float rowY = 0.0f, from = 0.0f, to = 0.0f;
                for (const SettingsSection& section : sections)
                {
                    const float head = rowY;
                    rowY += kSectionHead;
                    for (int k = section.first; k < section.end; ++k, rowY += kRowPitch)
                        if (kOrder[k] == g_settingsIndex)
                        {
                            from = k == section.first ? head : rowY;
                            to = rowY + 44.0f;
                        }
                    rowY += kSectionGap;
                }
                if (from < g_settingsScroll)
                    g_settingsScroll = from;
                else if (to > g_settingsScroll + room)
                    g_settingsScroll = to - room;
            }
        }
        g_settingsScrolledTo = g_settingsIndex;
        g_settingsScroll = std::clamp(g_settingsScroll, 0.0f, maxScroll);
        const auto inView = [&](float y0, float y1) { return y0 >= listTop - 0.5f && y1 <= listBottom + 0.5f; };
        float y = listTop - g_settingsScroll;
        for (const SettingsSection& section : sections)
        {
            if (inView(y, y + 22.0f))
            {
                // The section title as the key help sets its sections (gold, the
                // title font), with the divider's fading hairline after it.
                // Its icon before it (the home menu's art), when the art is there.
                const bool icon = ui_art::Get(section.icon) != 0;
                const float titleX = icon ? 114.0f : 88.0f;
                if (icon)
                    MenuIcon(draw, section.icon, 97.0f, y + 9.0f, 22.0f, false, true);
                const std::string sectionTitle = Ellipsized(18.0f, section.title, 460.0f - titleX);
                Text(draw, titleX, y, 18.0f, kGold, sectionTitle.c_str());
                const float lineX = titleX + TextWidth(18.0f, sectionTitle.c_str()) / g_scale + 12.0f;
                if (lineX + 40.0f < 460.0f)
                {
                    // At least a pixel tall: below 1080 lines a unit rounds away.
                    const ImVec2 a = P(lineX, y + 10.0f);
                    const ImVec2 b(P(460.0f, 0.0f).x, std::max(P(0.0f, y + 11.0f).y, a.y + 1.0f));
                    draw->AddRectFilledMultiColor(a, b, IM_COL32(226, 181, 79, 140), IM_COL32(226, 181, 79, 0), IM_COL32(226, 181, 79, 0),
                        IM_COL32(226, 181, 79, 140));
                }
            }
            y += kSectionHead;
            for (int k = section.first; k < section.end; ++k, y += kRowPitch)
            {
                if (!inView(y, y + 44.0f))
                    continue;
                const int n = kOrder[k];
                bool clicked = false, pointed = false;
                ImGui::PushID(n);
                ListItem(draw, "##settings", 80.0f, y, 380.0f, 44.0f, n == g_settingsIndex, clicked, &pointed);
                ImGui::PopID();
                if (clicked || pointed)
                {
                    g_settingsIndex = n;
                    g_settingsScrolledTo = n; // a click or the mouse does not scroll
                }
                if (clicked)
                {
                    direction = 1;
                    wrap = true;
                }
                SettingsRow(draw, 88.0f, y, 372.0f, labels[n], values[n], n == g_settingsIndex); // inside its row
            }
            y += kSectionGap;
        }
        if (maxScroll > 0.0f)
        {
            // More above, more below: a small gold chevron beside the rows.
            if (g_settingsScroll > 0.5f)
                draw->AddTriangleFilled(P(466.0f, listTop + 14.0f), P(478.0f, listTop + 14.0f), P(472.0f, listTop + 6.0f), kGold);
            if (g_settingsScroll < maxScroll - 0.5f)
                draw->AddTriangleFilled(P(466.0f, listBottom - 14.0f), P(478.0f, listBottom - 14.0f), P(472.0f, listBottom - 6.0f), kGold);
        }
        if (Confirmed())
            direction = 1;
        if (direction != 0)
            PlayUi(g_settingsIndex == 6 ? UiSound::Select : direction < 0 ? UiSound::NavLeft : UiSound::NavRight);
        if (direction != 0 && g_settingsIndex == 3)
        {
            Command command{};
            command.enabled = deadEye ? 0u : 1u;
            Send(CommandType::SetReplayDeadEye, &command);
            direction = 0;
        }
        if (direction != 0 && (g_settingsIndex == 4 || g_settingsIndex == 5))
        {
            Command command{};
            command.enabled = (g_settingsIndex == 4 ? !compress : compress) ? 1u : 0u;
            command.keyframeId = g_settingsIndex == 5 ? step(kDays, 11, days, direction) : days;
            Send(CommandType::SetClipCompression, &command);
            direction = 0;
        }
        if (direction != 0 && (g_settingsIndex == 7 || g_settingsIndex == 8))
        {
            Command command{};
            command.enabled = (g_settingsIndex == 7 ? !recordCutscenes : !lockCutsceneCamera) ? 1u : 0u;
            Send(g_settingsIndex == 7 ? CommandType::SetRecordCutscenes : CommandType::SetLockCutsceneCamera, &command);
            direction = 0;
        }
        if (direction != 0 && g_settingsIndex == 9)
        {
            Command command{};
            command.enabled = freezeOnPause ? 0u : 1u;
            Send(CommandType::SetFreezeOnPause, &command);
            direction = 0;
        }
        if (direction != 0 && g_settingsIndex == 10)
        {
            Command command{};
            command.enabled = static_cast<std::uint32_t>(recind::Step(indicator, direction, wrap));
            Send(CommandType::SetRecordingIndicator, &command);
            direction = 0;
        }
        if (direction != 0 && g_settingsIndex == 13)
        {
            // Two faces: the arrows stop at both ends, a click goes to the other.
            const int face = wrap ? 1 - titleFace : std::clamp(titleFace + direction, 0, 1);
            if (face != titleFace)
                SetTitleFont(face);
            direction = 0;
        }
        if (direction != 0 && g_settingsIndex == 11)
        {
            SetInterfaceScale(replay::editor::uiscale::Step(g_uiScalePercent, direction, wrap));
            direction = 0;
        }
        if (direction != 0 && g_settingsIndex == 12)
        {
            // Two values: any step toggles (the arrows stop at the ends).
            const bool wantMaximum = wrap ? !maximumPose : direction > 0;
            if (wantMaximum != maximumPose)
            {
                Command command{};
                command.enabled = wantMaximum ? 2u : 1u;
                Send(CommandType::SetPoseDetail, &command);
            }
            direction = 0;
        }
        if (direction != 0 && g_settingsIndex == 6)
        {
            if (g_snapshot.compressState != 2 && g_snapshot.compressState != 3)
            {
                Command command{};
                Send(CommandType::CompressClipsNow, &command);
            }
            else
                PlayUi(UiSound::Error);
            direction = 0;
        }
        if (direction != 0)
        {
            if (g_settingsIndex == 0)
                enabled = !enabled;
            else if (g_settingsIndex == 1)
                seconds = step(kSeconds, 6, seconds, direction);
            else
                megabytes = step(kMegabytes, 5, megabytes, direction);
            Command command{};
            command.enabled = enabled ? 1u : 0u;
            command.timeMs = seconds;
            command.keyframeId = megabytes;
            Send(CommandType::SetReplayBuffer, &command);
        }

        const char* heading = rows[g_settingsIndex];
        const char* body = g_settingsIndex == 0
            ? KeyFill(TR("settings.buffer.body", "The game records all the time, like the Rockstar Editor's replay mode. {key_save_buffer} saves what it holds; {key_record} marks a recording inside it."))
            // Length and Memory are the buffer's only: a manual recording splits at
            // ManualMaxMB (testers raised these for a manual recording).
            : g_settingsIndex == 1 ? KeyFill(TR("settings.length.about", "How many seconds the buffer keeps. The buffer stops at whichever comes first: these seconds or "
                                                                        "the Memory limit, so a busy scene holds fewer seconds. A manual recording ({key_record}) is not limited by it."))
            : g_settingsIndex == 2 ? KeyFill(TR("settings.memory.about", "The most the buffer may use; a busy town takes about 6 MB a second. The buffer stops at whichever "
                                                                        "comes first: this limit or the Length in seconds. A manual recording ({key_record}) is not limited by it: a long one "
                                                                        "is saved in parts (ManualMaxMB in RedemptionEditor.ini)."))
            : g_settingsIndex == 3 ? TR("settings.dead_eye.body", "Play the recorded Dead Eye and Eagle Eye: the game's screen effect and the slow motion. Off: the replay plays those moments at normal speed, without the effect.")
            : g_settingsIndex == 4 ? TR("settings.compress.body", "Clips not played nor saved for a while are compressed in the background, about 3 times smaller. They open as before; the first load takes a fraction of a second more.")
            : g_settingsIndex == 5 ? TR("settings.compress_after.body", "How long a clip stays untouched before it is compressed. Playing or saving a clip counts as a use.")
            : g_settingsIndex == 6 ? TR("settings.compress_now.body", "Compress now every clip older than the days above, without waiting. It runs in the background; the game keeps going.")
            : g_settingsIndex == 9 ? TR("settings.freeze.body", "On: a paused replay stands still like the photo mode (fire, smoke, water, cloth, wind, ambient sound). Off: the world goes on behind the paused replay.")
            : g_settingsIndex == 7 ? TR("settings.cutscenes.body", "Skip: the recording pauses while a cutscene plays and the clip cuts straight past it. Record: cutscenes are kept in the clip.")
            : g_settingsIndex == 13 ? TR("settings.font.body", "The font of the editor's titles, menus, keys and notices. Redemption: the game's own. "
                                                              "Segoe UI: the Windows font in bold, easier to read. The help texts keep their font.")
            : g_settingsIndex == 11 ? TR("settings.interface_scale.body", "The size of the editor's menus, panels, texts and notices. 100% looks the same at every resolution; larger reads "
                                                                         "better from a distance, smaller leaves more of the picture. A 4:3 screen stays at 100% at most, and a small screen does not go below its 100% size.")
            : g_settingsIndex == 12 ? TR("settings.pose_detail.body", "How finely the recording keeps the characters' poses. Standard: the detail you see in the game, "
                                                                       "still when they stand still. Maximum: every movement of every bone, at full precision; the poses take "
                                                                       "about twice the memory (the buffer holds about 40% fewer seconds) and clips grow about 1.6 times. "
                                                                       "It applies to what is recorded from now on.")
            : g_settingsIndex == 10 ? TR("settings.rec_indicator.about", "The REC notice while the replay records. Normal: REC always on screen, REPLAY ON for 5 seconds when the replay buffer starts. Brief: for 3 seconds when a recording starts, stops or saves. Hidden from capture: you see it, screen recorders (OBS, NVIDIA, Game Bar) do not. Off: no notice.")
                                   : TR("settings.lock_camera.body", "On: markers inside a recorded cutscene show the cutscene's own camera. Off: the markers' cameras work there as anywhere else.");
        // The clips folder's disk use (all the clips, not only the listed ones).
        std::string usage;
        if (g_settingsIndex >= 4 && g_settingsIndex <= 6 && !ClipListKnown())
            usage = TR("settings.disk.reading", "Clips on disk: reading clips...");
        else if (g_settingsIndex >= 4 && g_settingsIndex <= 6)
        {
            char disk[32]{}, saved[32]{};
            FormatSize(g_snapshot.clipsDiskBytes, disk, sizeof(disk));
            FormatSize(g_snapshot.clipsBytes > g_snapshot.clipsDiskBytes ? g_snapshot.clipsBytes - g_snapshot.clipsDiskBytes : 0,
                saved, sizeof(saved));
            usage = TRF("settings.disk", "Clips on disk: {size} in {count} clips, {compressed} compressed ({saved} saved).",
                {{"size", disk}, {"count", std::to_string(g_snapshot.clipTotal)}, {"compressed", std::to_string(g_snapshot.compressedClips)},
                    {"saved", saved}});
        }
        // What the buffer holds now: Length, Memory and Pose detail each change
        // it (the seconds or the memory, whichever comes first).
        if ((g_settingsIndex == 1 || g_settingsIndex == 2 || g_settingsIndex == 12) && g_snapshot.bufferEnabled)
            usage = TRF("settings.pose_detail.held", "The replay buffer holds {held} seconds now (memory {mb} MB).",
                {{"held", std::to_string(g_snapshot.bufferHeldMs / 1000u)}, {"mb", std::to_string(g_snapshot.bufferMB)}});
        // Hidden from capture shown as Brief: why.
        if (g_settingsIndex == 10 && indicator == recind::Mode::Hidden && g_recChoice.why != recind::Fallback::None)
            usage = g_recChoice.why == recind::Fallback::OldWindows
                ? TR("settings.rec_indicator.fallback.windows", "Hidden from capture needs Windows 10 version 2004 or newer: it works as Brief here.")
                : g_recChoice.why == recind::Fallback::ExclusiveFullscreen
                ? TR("settings.rec_indicator.fallback.fullscreen", "The game is in exclusive fullscreen, where no window shows over it: Hidden from capture works as Brief. Borderless or windowed shows it.")
                : TR("settings.rec_indicator.fallback.window", "Windows refused the indicator's window: Hidden from capture works as Brief. ReShade.log says why.");
        const float panelX = columnWidth + 30.0f;
        const float panelTop = itemsTop - 6.0f;
        DescriptionPanel(draw, panelX, panelTop, usage.empty() ? 200.0f : 260.0f, heading, body, nullptr, usage.c_str(),
            g_settingsIndex >= 4 && g_settingsIndex <= 6 && !ClipListKnown());
        if (Back())
        {
            PlayUi(UiSound::Back);
            g_screen = Screen::Home;
        }
    }

    // ---------------- Alert ----------------
    void DrawAlert(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("Alert");
        draw->AddRectFilled(ImVec2(0.0f, 0.0f), ImGui::GetIO().DisplaySize, IM_COL32(0, 0, 0, 215));
        // Deleting a clip shows its picture above the question.
        if (g_alert.action == CommandType::DeleteClip)
        {
            // 12 units higher, clear of the dialog's panel.
            const ImTextureID thumbnail = clip_thumbnails::Thumbnail(g_runtime, g_alert.actionName.c_str());
            draw->AddRectFilled(P(476.0f, 4.0f), P(804.0f, 188.0f), IM_COL32(40, 38, 34, 255));
            if (thumbnail)
                draw->AddImage(ImTextureRef(thumbnail), P(480.0f, 8.0f), P(800.0f, 184.0f), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
            else
            {
                const bool loading = clip_thumbnails::Loading(g_alert.actionName);
                Text(draw, 580.0f, 86.0f, 16.0f, kMuted, loading ? TR("gallery.thumb.loading", "[ loading ]") : TR("gallery.thumb.none", "[ no thumbnail ]"));
                if (loading)
                    LoadingCue(draw, 640.0f, 56.0f, 36.0f);
            }
        }
        DialogPanel(draw, 292.0f + ParagraphHeight(19.0f, g_alert.body.c_str(), 400.0f) + kPanelPadBottom);
        const float width = TextWidth(40.0f, g_alert.title.c_str()) / g_scale;
        Text(draw, 640.0f - width / 2.0f, 210.0f, 40.0f, kText, g_alert.title.c_str());
        draw->AddLine(P(440.0f, 268.0f), P(840.0f, 268.0f), kRed, S(3.0f));
        Paragraph(draw, 440.0f, 292.0f, 19.0f, kText, g_alert.body.c_str(), 400.0f);
        if (Confirmed())
        {
            PlayUi(UiSound::Select);
            if (g_alert.action != CommandType::None)
            {
                if (g_alert.action == CommandType::DeleteClip)
                    clip_thumbnails::Forget(g_alert.actionName);
                if (g_alert.action == CommandType::LoadClip)
                    RequestLoad(g_alert.actionName);
                else if (g_alert.action == CommandType::ProjectDeleteClip)
                {
                    // A project clip goes by its place.
                    Command command{};
                    command.keyframeId = static_cast<std::uint32_t>(std::strtoul(g_alert.actionName.c_str(), nullptr, 10));
                    Send(CommandType::ProjectDeleteClip, &command);
                }
                else
                    SendNamed(g_alert.action, g_alert.actionName.c_str());
            }
            g_screen = g_alert.after != Screen::Count ? g_alert.after : g_alert.back;
        }
        else if (Back())
        {
            PlayUi(UiSound::Back);
            g_screen = g_alert.back;
        }
    }
}

    // A panel row under the mouse takes Left and Right this frame (only such a
    // row: not any hovered item, such as the timeline where the mouse rests after
    // a click).
    bool g_valueRowHovered = false;

namespace
{

    // A panel row's label at the left and its value at the right, "<  value  >"
    // with `arrows`. When they would meet (a target's long name) both step down
    // from 18 to 15 units, then the value is cut with "..."; a value that already
    // ends with ">" (a submenu: "Classic  >") gets no value arrows. `lead`: the
    // icons' column before the label (EditIconLead), `icon` drawn in it.
    void RowTexts(ImDrawList* draw, float x, float y, float width, const char* label, const char* value, bool arrows, ImU32 color,
        float lead = 0.0f, ui_art::Art icon = ui_art::Art::Count, bool onBrush = false)
    {
        const std::string inner = value ? value : "";
        if (!inner.empty() && inner.back() == '>')
            arrows = false;
        const auto shownOf = [arrows](const std::string& v) { return arrows ? "<  " + v + "  >" : v; };
        constexpr float kGap = 16.0f;
        const float room = width - 6.0f;
        const std::string full = inner.empty() ? std::string() : shownOf(inner);
        const float size = replay::editor::uilayout::ShrinkToFit(18.0f, 15.0f, room, [&](float at) {
            return lead + (TextWidth(at, label) + (full.empty() ? 0.0f : TextWidth(at, full.c_str()))) / g_scale + kGap;
        });
        std::string shown = full;
        if (!shown.empty())
        {
            const float valueRoom = room - lead - TextWidth(size, label) / g_scale - kGap;
            if (TextWidth(size, shown.c_str()) / g_scale > valueRoom)
                shown = shownOf(Ellipsized(size, inner, std::max(0.0f, valueRoom - (arrows ? TextWidth(size, "<    >") / g_scale : 0.0f))));
        }
        MenuIcon(draw, icon, x + 10.0f, y + 17.0f, 22.0f, onBrush, color == kText);
        TextFit(draw, x + lead, y + 7.0f, size, color, label, room - lead);
        if (!shown.empty())
            Text(draw, x + width - TextWidth(size, shown.c_str()) / g_scale - 6.0f, y + 7.0f, size, color, shown.c_str());
    }
}
    int ValueRow(ImDrawList* draw, const char* id, float y, const char* label, const char* value, bool enabled, bool repeat,
        float lead, ui_art::Art icon)
    {
        const float x = kEditX;
        const float width = kEditW;
        ImGui::SetCursorScreenPos(P(x, y));
        ImGui::InvisibleButton(id, ImVec2(S(width), S(34.0f)));
        const bool hovered = ImGui::IsItemHovered();
        g_valueRowHovered = g_valueRowHovered || (hovered && enabled);
        RowBox(draw, x - 8.0f, y, x + width, y + 34.0f, HoverShown(hovered) && enabled ? RowLook::Hovered : RowLook::Rest);
        const ImU32 color = enabled ? kText : kMuted;
        RowTexts(draw, x, y, width, label, value, true, color, lead, icon);
        if (!enabled || !hovered)
            return 0;
        int step = 0;
        if (MouseClicked())
            step = ImGui::GetMousePos().x < S(x + width * 0.6f) ? -1 : 1;
        else if (KeyPressed(ImGuiKey_LeftArrow, repeat))
            step = -1;
        else if (KeyPressed(ImGuiKey_RightArrow, repeat))
            step = 1;
        if (step != 0)
            PlayUiStep(step);
        return step;
    }
    int MenuRowDraw(ImDrawList* draw, int n, float y, const MenuRow& row, bool focused, bool& clickedEnter, float lead,
        bool* pointed)
    {
        const float x = kEditX;
        const float width = kEditW;
        ImGui::SetCursorScreenPos(P(x, y));
        ImGui::PushID(n);
        ImGui::InvisibleButton("##menurow", ImVec2(S(width), S(34.0f)));
        ImGui::PopID();
        const bool hovered = ImGui::IsItemHovered();
        const bool point = pointed && !focused && hovered && g_menuPointer.Moved();
        if (pointed)
            *pointed = point;
        if (point)
        {
            PlayUi(UiSound::NavDown);
            focused = true;
        }
        RowBox(draw, x - 8.0f, y, x + width, y + 34.0f,
            focused ? (row.enabled ? RowLook::Selected : RowLook::SelectedGreyed) // the focus on a greyed row must show
                    : HoverShown(hovered) && row.enabled ? RowLook::Hovered : RowLook::Rest);
        const ImU32 color = row.enabled ? kText : kMuted;
        if (row.kind == 3)
        {
            // "Event 0   t=0.00 Shake Amp=1.50 snap", smaller when it is long.
            const float size = replay::editor::uilayout::ShrinkToFit(16.0f, 12.0f, width, [&](float at) {
                return (TextWidth(at, row.label.c_str()) + TextWidth(at, row.value.c_str())) / g_scale + 18.0f;
            });
            Text(draw, x, y + 8.0f, size, color, row.label.c_str());
            Text(draw, x + width - TextWidth(size, row.value.c_str()) / g_scale - 6.0f, y + 8.0f, size, color, row.value.c_str());
        }
        else // RowTexts fits a long value
            RowTexts(draw, x, y, width, row.label.c_str(), row.kind == 1 ? ">" : row.kind >= 2 ? "" : row.value.c_str(), row.kind == 0, color, lead,
                row.icon, focused);
        clickedEnter = false;
        if (!row.enabled || !hovered || !MouseClicked())
            return 0;
        if (row.kind != 0)
        {
            clickedEnter = true;
            return 0;
        }
        return ImGui::GetMousePos().x < S(x + width * 0.6f) ? -1 : 1;
    }

namespace
{

    // ------------------------------------------------------------------
    // Frame
    // ------------------------------------------------------------------
    ImGuiConfigFlags g_previousConfigFlags = 0;
    bool g_configFlagsHeld = false; // EditorCoreStop gives ReShade its flags back
    ecur::IoWrites g_cursorWrites = ecur::IoWrites::Unknown;
    ecur::StateLog g_cursorLog;

#ifdef REDEMPTION_UI_HARNESS
    // A fixture's mouse (1080p units): ImGui's mouse there too (the rows'
    // hover), the cursor as ReShade 6.8 serves it (the write lost) or, with
    // `reach`, as 6.6 and 6.7 do (the write reaches ImGui).
    struct HarnessMouse
    {
        bool on = false;
        float x = 0.0f;
        float y = 0.0f;
        bool reach = false;
        bool moved = false; // it just moved onto (x, y): the menus take it as a move (MenuPointer)
    };
    HarnessMouse g_harnessMouse;
#endif

    void DrawEditorCursor(float x, float y)
    {
        ImDrawList* draw = ImGui::GetForegroundDrawList(nullptr);
        const ui_art::Texture& ink = ui_art::Info(ui_art::Art::Cursor);
        if (ink.id)
        {
            const ecur::Rect r = ecur::InkRect(x, y, g_scale, ink.width, ink.height);
            draw->AddImage(ImTextureRef(static_cast<ImTextureID>(ink.id)), ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1));
            return;
        }
        const float s = std::max(1.0f, g_scale);
        ImVec2 shadow[ecur::kArrowPoints];
        ImVec2 arrow[ecur::kArrowPoints];
        for (int i = 0; i < ecur::kArrowPoints; ++i)
        {
            arrow[i] = ImVec2(x + ecur::kArrow[i][0] * s, y + ecur::kArrow[i][1] * s);
            shadow[i] = ImVec2(arrow[i].x + 1.5f * s, arrow[i].y + 1.5f * s);
        }
        draw->AddConcavePolyFilled(shadow, ecur::kArrowPoints, IM_COL32(0, 0, 0, 90));
        draw->AddConcavePolyFilled(arrow, ecur::kArrowPoints, IM_COL32(255, 255, 255, 255));
        draw->AddPolyline(arrow, ecur::kArrowPoints, IM_COL32(0, 0, 0, 255), ImDrawFlags_Closed, std::max(1.0f, s));
    }

    void SetInteractive(reshade::api::effect_runtime* runtime, bool interactive, ecur::Reason reason)
    {
        ImGuiIO& io = ImGui::GetIO();
        static bool was = false;
        // ReShade sets the flag at the start of its frame (true while its
        // own window is open), before this callback.
        const bool reshadeCursor = io.MouseDrawCursor;
        if (interactive && !was)
            g_previousMouseDrawCursor = io.MouseDrawCursor;
        if (!interactive && was)
            io.MouseDrawCursor = g_previousMouseDrawCursor;
        // ReShade's ImGui keyboard navigation off while the editor has the keyboard
        // (Space or Enter would re-fire the last clicked button).
        if (interactive && !was)
        {
            g_menuPointer.Reset(); // where the mouse came to while the game had it is no move
            g_previousConfigFlags = io.ConfigFlags;
            g_configFlagsHeld = true;
        }
        if (!interactive && was)
        {
            io.ConfigFlags = g_previousConfigFlags;
            g_configFlagsHeld = false;
        }
        was = interactive;
        if (interactive)
        {
            io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
            io.MouseDrawCursor = true;
            runtime->block_input_next_frame();
        }
        // A fresh GetIO(): the real flag (a copy is converted again).
        const ImGuiIO& now = ImGui::GetIO();
        ecur::Frame frame;
        frame.interactive = interactive;
        frame.reshadeCursor = reshadeCursor;
        frame.cursorAfterWrite = interactive && now.MouseDrawCursor;
        frame.mouseValid = ImGui::IsMousePosValid(&now.MousePos);
        frame.x = now.MousePos.x;
        frame.y = now.MousePos.y;
        frame.width = now.DisplaySize.x;
        frame.height = now.DisplaySize.y;
        frame.reason = reason;
        frame.inkReady = ui_art::Get(ui_art::Art::Cursor) != 0;
#ifdef REDEMPTION_UI_HARNESS
        if (g_harnessMouse.on)
        {
            frame.reshadeCursor = false; // ReShade's window closed (the harness never clears the flag ReShade would)
            frame.cursorAfterWrite = interactive && g_harnessMouse.reach;
            frame.mouseValid = true;
            frame.x = g_harnessMouse.x * g_scale;
            frame.y = g_harnessMouse.y * g_scale;
        }
#endif
        const ecur::Decision decision = ecur::Decide(frame, g_cursorWrites);
        g_cursorWrites = decision.writes;
        if (decision.hideImGui)
            io.MouseDrawCursor = false; // ImGui's arrow off where the write reaches (the ink is drawn instead)
#ifdef REDEMPTION_UI_HARNESS
        if (g_harnessMouse.on && !g_harnessMouse.reach)
            io.MouseDrawCursor = false; // ReShade 6.8: the write never reached ImGui
#endif
        if (decision.drawn == ecur::Drawn::Editor || decision.drawn == ecur::Drawn::Ink)
            DrawEditorCursor(decision.drawX, decision.drawY);
        char line[320];
        if (g_cursorLog.Update(frame, decision, ImGui::GetVersion(), IMGUI_VERSION_NUM, line, sizeof(line)))
            reshade::log::message(reshade::log::level::info, line);
    }

    HANDLE g_closeRequest = nullptr;
    oc::Channel* g_ovChannel = nullptr;
    HANDLE g_ovMapping = nullptr;
    ULONGLONG g_ovRetryAt = 0;
    struct OverlayCopy
    {
        std::uint32_t flags = 0;
        std::uint32_t lineCount = 0;
        std::uint32_t dotCount = 0;
        std::uint32_t labelCount = 0;
        oc::Line lines[oc::kMaxLines];
        oc::Dot dots[oc::kMaxDots];
        oc::Label labels[oc::kMaxLabels];
    };
    OverlayCopy g_ovCopy;
    bool g_ovHave = false;

    oc::Channel* OverlayChannel()
    {
        if (g_ovChannel)
        {
            if (g_ovChannel->magic == oc::kMagic && g_ovChannel->version == oc::kVersion && g_ovChannel->bytes == sizeof(oc::Channel))
                return g_ovChannel;
            return nullptr; // another layout (a mixed pair after a reload): nothing drawn
        }
        const ULONGLONG now = GetTickCount64();
        if (now < g_ovRetryAt)
            return nullptr;
        g_ovRetryAt = now + 2000;
        wchar_t name[64];
        oc::Name(name, GetCurrentProcessId());
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
        if (!mapping)
            return nullptr;
        void* view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(oc::Channel));
        if (!view)
        {
            CloseHandle(mapping);
            return nullptr;
        }
        g_ovMapping = mapping;
        g_ovChannel = static_cast<oc::Channel*>(view);
        return OverlayChannel();
    }

    // The published frame, copied whole (dropped when the core wrote under it).
    void ReadOverlay()
    {
        oc::Channel* channel = OverlayChannel();
        if (!channel)
        {
            g_ovHave = false;
            return;
        }
        const std::uint32_t published = channel->published.load(std::memory_order_acquire);
        if (published > 1)
        {
            g_ovHave = false;
            return;
        }
        const oc::Frame& frame = channel->frames[published];
        const std::uint64_t before = frame.sequence.load(std::memory_order_acquire);
        if (before & 1u)
            return; // being written: the last copy stays
        g_ovCopy.flags = frame.flags;
        g_ovCopy.lineCount = std::min(frame.lineCount, oc::kMaxLines);
        g_ovCopy.dotCount = std::min(frame.dotCount, oc::kMaxDots);
        g_ovCopy.labelCount = std::min(frame.labelCount, oc::kMaxLabels);
        std::memcpy(g_ovCopy.lines, frame.lines, sizeof(oc::Line) * g_ovCopy.lineCount);
        std::memcpy(g_ovCopy.dots, frame.dots, sizeof(oc::Dot) * g_ovCopy.dotCount);
        std::memcpy(g_ovCopy.labels, frame.labels, sizeof(oc::Label) * g_ovCopy.labelCount);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (frame.sequence.load(std::memory_order_relaxed) != before)
        {
            channel->addonTorn.fetch_add(1, std::memory_order_relaxed);
            g_ovHave = false;
            return;
        }
        g_ovHave = true;
    }

    void DrawCameraOverlay(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("CameraOverlay");
        ReadOverlay();
        if (!g_ovHave || (g_ovCopy.flags & oc::FrameHidden))
            return;
        if (g_ovChannel)
            g_ovChannel->addonDrawn.fetch_add(1, std::memory_order_relaxed);
        const ImGuiIO& io = ImGui::GetIO();
        const float half = io.DisplaySize.y * 0.5f;
        const ImVec2 centre(io.DisplaySize.x * 0.5f, half);
        const auto at = [&](float x, float y) { return ImVec2(centre.x + x * half, centre.y - y * half); };
        const float dim = (g_ovCopy.flags & oc::FrameDim) ? 0.35f : 1.0f;
        constexpr ImU32 kPathSpline = IM_COL32(226, 181, 79, 230); // gold, as the timeline's marks
        constexpr ImU32 kPathOther = IM_COL32(236, 230, 218, 200);
        constexpr ImU32 kTrail = IM_COL32(110, 190, 235, 210);
        constexpr ImU32 kGrey = IM_COL32(150, 143, 131, 220);
        constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 150);
        for (std::uint32_t n = 0; n < g_ovCopy.lineCount; ++n)
        {
            const oc::Line& l = g_ovCopy.lines[n];
            const bool past = (l.flags & oc::FlagPast) != 0;
            ImU32 color = kPathOther;
            float width = 2.0f;
            if (l.kind == static_cast<std::uint16_t>(oc::LineKind::Path))
                color = (l.flags & oc::FlagSpline) ? kPathSpline : kPathOther;
            else if (l.kind == static_cast<std::uint16_t>(oc::LineKind::Arrow))
            {
                color = (l.id == g_selectedKeyframeId || (l.flags & (oc::FlagCurrent | oc::FlagEditing))) ? kRed : kText;
                width = 1.6f;
            }
            else
            {
                color = kTrail;
                width = 1.6f;
            }
            const float alpha = dim * (past ? 0.45f : 1.0f);
            const ImVec2 a = at(l.x0, l.y0), b = at(l.x1, l.y1);
            draw->AddLine(a, b, WithAlpha(kShadow, alpha), S(width + 1.6f));
            draw->AddLine(a, b, WithAlpha(color, alpha), S(width));
        }
        for (std::uint32_t n = 0; n < g_ovCopy.dotCount; ++n)
        {
            const oc::Dot& d = g_ovCopy.dots[n];
            const ImVec2 p = at(d.x, d.y);
            const float radius = S(std::clamp(140.0f / std::max(1.0f, d.meters), 3.5f, 8.0f));
            const float alpha = dim * ((d.flags & oc::FlagPast) ? 0.6f : 1.0f);
            switch (static_cast<oc::DotKind>(d.kind))
            {
            case oc::DotKind::Marker:
            {
                const bool selected = d.id == g_selectedKeyframeId;
                const bool current = (d.flags & oc::FlagCurrent) != 0;
                const bool editing = (d.flags & oc::FlagEditing) != 0;
                const ImU32 fill = (d.flags & oc::FlagNotFree) ? kGrey : current || editing ? kRed : kGold;
                draw->AddCircleFilled(p, radius + S(1.5f), WithAlpha(kShadow, alpha));
                if (d.flags & oc::FlagClipBound)
                    draw->AddRectFilled(ImVec2(p.x - radius, p.y - radius), ImVec2(p.x + radius, p.y + radius), WithAlpha(fill, alpha));
                else
                    draw->AddCircleFilled(p, radius, WithAlpha(fill, alpha));
                if (d.flags & oc::FlagAttached)
                    draw->AddCircleFilled(p, radius * 0.4f, WithAlpha(kTrail, alpha));
                if (selected || editing)
                    draw->AddCircle(p, radius + S(4.0f), WithAlpha(kText, alpha), 0, S(2.0f));
                break;
            }
            case oc::DotKind::Camera:
            {
                // The marker camera where the playhead puts it (seen from the free camera).
                draw->AddRectFilled(ImVec2(p.x - S(7.0f), p.y - S(5.0f)), ImVec2(p.x + S(7.0f), p.y + S(5.0f)), WithAlpha(kRed, dim));
                draw->AddRect(ImVec2(p.x - S(7.0f), p.y - S(5.0f)), ImVec2(p.x + S(7.0f), p.y + S(5.0f)), WithAlpha(kText, dim), 0.0f, 0, S(1.2f));
                draw->AddText(nullptr, S(13.0f), ImVec2(p.x + S(10.0f), p.y - S(8.0f)), WithAlpha(kText, dim), TR("overlay.cam", "CAM"));
                break;
            }
            case oc::DotKind::Target:
            {
                const float r = S(7.0f);
                draw->AddQuadFilled(ImVec2(p.x, p.y - r), ImVec2(p.x + r, p.y), ImVec2(p.x, p.y + r), ImVec2(p.x - r, p.y), WithAlpha(kTrail, dim));
                draw->AddQuad(ImVec2(p.x, p.y - r), ImVec2(p.x + r, p.y), ImVec2(p.x, p.y + r), ImVec2(p.x - r, p.y), WithAlpha(kShadow, dim), S(1.2f));
                draw->AddText(nullptr, S(14.0f), ImVec2(p.x + S(11.0f), p.y - S(9.0f)), WithAlpha(kText, dim), TargetLabel(d.id));
                break;
            }
            case oc::DotKind::TrailTick:
                draw->AddCircleFilled(p, S(2.5f), WithAlpha(kTrail, alpha));
                break;
            default:
                break;
            }
        }
        for (std::uint32_t n = 0; n < g_ovCopy.labelCount; ++n)
        {
            const oc::Label& l = g_ovCopy.labels[n];
            char text[16]{};
            std::snprintf(text, sizeof(text), "%.0fs", l.value / 1000.0);
            const ImVec2 p = at(l.x, l.y);
            const float alpha = dim * ((l.flags & oc::FlagPast) ? 0.5f : 0.9f);
            const ImU32 color = l.kind == static_cast<std::uint16_t>(oc::LabelKind::TrailTime) ? kTrail : kText;
            draw->AddText(nullptr, S(13.0f), ImVec2(p.x + S(5.0f) + 1.0f, p.y - S(17.0f) + 1.0f), WithAlpha(kShadow, alpha), text);
            draw->AddText(nullptr, S(13.0f), ImVec2(p.x + S(5.0f), p.y - S(17.0f)), WithAlpha(color, alpha), text);
        }
    }

    bool g_toldOpen = false; // the core was told the editor covers the screen

    // ---------------- Language (src/i18n.h) ----------------
    // RedemptionEditor.ini Language, read once (auto: the game's language, which
    // the core publishes in the snapshot); a change loads that language's table
    // from RedemptionEditor\lang and gives the fonts its letters.
    std::string g_languageSetting;
    bool g_languageRead = false;
    std::string g_languageCode = "en";
    bool g_languageChecked = true;

    void UpdateLanguage()
    {
        if (!g_languageRead)
        {
            g_languageRead = true;
            wchar_t folder[MAX_PATH]{};
            GetCurrentDirectoryW(MAX_PATH, folder);
            const std::wstring ini = replay::identity::IniPathInW(folder);
            wchar_t value[32]{};
            replay::identity::IniStringW(L"Language", L"auto", value, 32, ini.c_str());
            for (const wchar_t* c = value; *c; ++c)
                g_languageSetting += *c < 0x80 ? static_cast<char>(*c) : '?';
        }
        const std::string code = replay::i18n::Choose(g_languageSetting, g_hasSnapshot ? g_snapshot.gameLanguage : 0u);
        if (code == g_languageCode)
            return;
        g_languageCode = code;
        std::string loaded;
        std::unique_ptr<replay::i18n::Table> table = replay::i18n::Load(bundled_files::RedemptionEditorFolder() + L"lang\\", code, &loaded);
        char line[256];
        std::snprintf(line, sizeof(line), "LANGUAGE,code=%s,file=%s,keys=%zu,problems=%zu", code.c_str(), loaded.empty() ? "none" : loaded.c_str(),
            table ? table->Size() : 0u, table ? table->problems.size() : 0u);
        reshade::log::message(reshade::log::level::info, line);
        if (table)
            for (const std::string& problem : table->problems)
                reshade::log::message(reshade::log::level::warning, ("LANGUAGE," + loaded + "," + problem).c_str());
        display_font::SetLanguageGlyphs(replay::i18n::LanguageGlyphs(table.get()), table ? table->code : std::string());
        replay::i18n::SetActive(std::move(table));
        g_languageChecked = false;
    }

    // After the fonts are baked: a language most of whose letters no face had
    // (a CJK language on a PC without its fonts) shows English, said once in
    // the log.
    void CheckLanguageGlyphs()
    {
        if (g_languageChecked || !display_font::Ready())
            return;
        g_languageChecked = true;
        const float missing = display_font::MissingShare();
        if (!replay::i18n::Active() || missing <= 0.01f)
            return;
        char line[160];
        std::snprintf(line, sizeof(line), "LANGUAGE,code=%s,refused=no_font,missing=%.0f%%,shown=en", g_languageCode.c_str(), missing * 100.0f);
        reshade::log::message(reshade::log::level::warning, line);
        replay::i18n::SetActive(nullptr);
        display_font::SetLanguageGlyphs({});
    }

    void OnOverlay(reshade::api::effect_runtime* runtime)
    {
        g_runtime = runtime;
        BeginKeyFrame(); // before any key is read this frame
        BeginMouseFrame();
        UpdateLanguage(); // before the fonts: a new language's letters are baked this frame
        display_font::Ensure(runtime);
        CheckLanguageGlyphs();
        ui_art::Ensure(runtime); // the painted art, once per device
        RefreshSnapshot();
        FlushUiSound(); // the sound the last frame asked for
        SendRequestedLoad();
        // A clip was saved: its thumbnail from the frames taken while recording.
        static std::uint64_t seenSaved = ~0ull;
        if (g_hasSnapshot)
        {
            if (seenSaved != ~0ull && g_snapshot.savedClipSequence != seenSaved && g_snapshot.savedClip[0])
            {
                clip_thumbnails::OnClipSaved(g_snapshot.savedClip);
                g_toastUntil = GetTickCount64() + 4000;
                g_toastClipMs = g_snapshot.savedClipMs;
            }
            seenSaved = g_snapshot.savedClipSequence;
        }
        // The recording indicator's mode and its window's rows (sent on every way out).
        UpdateRecordingIndicator(runtime);
        struct SubmitRecordingRows
        {
            ~SubmitRecordingRows() { SubmitRecordingWindow(); }
        } submitRecordingRows;
        const bool playing = g_hasSnapshot && Flag(SnapshotPlaying);
        // Enter on a project clip opens the clip editor once the clip is open (its
        // file may have to be read first).
        if (g_editAfterOpen >= 0)
        {
            if (ProjectFlag(ProjectEditingClip) && playing && g_snapshot.projectActiveClip == static_cast<std::uint32_t>(g_editAfterOpen) &&
                !ProjectFlag(ProjectLoadingClip))
            {
                if (g_screen == Screen::Project)
                {
                    g_screen = Screen::Edit;
                    g_hide.Show();
                }
                g_editAfterOpen = -1;
            }
            else if (GetTickCount64() - g_editAfterOpenAt > 60000)
                g_editAfterOpen = -1;
        }
        if (g_screen == Screen::Edit && playing)
            g_editingProjectClip = ProjectFlag(ProjectEditingClip);
        UpdateWorldNotice();
        // A far jump keeps the last picture with a spinner; drawn under the
        // editor's UI. Render export: the picture of this present, before the
        // editor's UI and before the frozen seek picture is drawn.
        render_export::ExportController::Get().OnOverlay(runtime);
        // QA kit: the screenshot ring and a mark's pictures, from the game's picture
        // before the editor's UI.
        qa_shots::OnFrame(runtime);
        const bool noteOpen = qa_shots::NoteOpen();
        if (g_hasSnapshot)
            exportui::Service();
        seek_freeze::Update(runtime, playing, g_hasSnapshot ? g_snapshot.seekLoadState : 0u,
            g_hasSnapshot ? g_snapshot.seekLoadSequence : 0u, g_hasSnapshot ? g_snapshot.seekLoadElapsedMs : 0u,
            false, g_uiScaleFactor); // the picture only: its cue is the editor's (DrawLoadingBadge, the bars' "Loading")

        // The bench closes the editor before a script reload (bench_shot.h).
        if (!g_closeRequest)
        {
            char name[96];
            replay::bench::FormatCloseEditorEventName(name, sizeof(name), GetCurrentProcessId());
            g_closeRequest = CreateEventA(nullptr, FALSE, FALSE, name);
        }
        if (g_closeRequest && WaitForSingleObject(g_closeRequest, 0) == WAIT_OBJECT_0)
        {
            g_screen = Screen::Closed;
            g_hide.Show();
        }
        // The game restarted its scripts (a save loaded) and the core zeroed its
        // session (no clip, no playback, no project): the screens that showed them
        // close, as the bench's close request does.
        {
            static bool seenResetInit = false;
            static std::uint32_t seenReset = 0;
            if (g_hasSnapshot)
            {
                if (!seenResetInit)
                {
                    seenReset = g_snapshot.sessionResetSequence;
                    seenResetInit = true;
                }
                else if (g_snapshot.sessionResetSequence != seenReset)
                {
                    seenReset = g_snapshot.sessionResetSequence;
                    g_sentLightEditorState = -1;
                    g_screen = Screen::Closed;
                    g_reopenLightsWithF2 = false;
                    g_lightReopenMarkerId = 0;
                    g_hide.Show();
                    g_helpOpen = false;
                }
            }
        }

        // F2 opens and closes the editor: the edit screen while a clip
        // plays, the home screen otherwise.
        // F1 opens and closes the key help (Esc closes it too).
        g_helpSwallow = false;
        // Only with the editor's UI on screen (in Edit Camera or hidden it would
        // open unseen and take Enter later).
        const bool uiUp = g_screen != Screen::Closed && !(g_screen == Screen::Edit && UiHidden()) &&
            !(g_hasSnapshot && Flag(SnapshotEditingCamera));
        if (!uiUp)
            g_helpOpen = false;
        // QA kit: Shift+F1 is the report (the core's key; sent through the QA
        // block while the editor has the keyboard), F1 alone the help.
        // The report's key from the [Keys] table (Shift+F1 by default).
        const bool f1Pressed = KeyVk(VK_F1);
        const bool f1Shift = g_runtime->is_key_down(VK_SHIFT);
        if (g_toldOpen && ActionPressed(Action::Report))
            qa_shots::RequestReport();
        if (uiUp && !noteOpen && f1Pressed && !f1Shift)
            g_helpOpen = !g_helpOpen;
        else if (g_helpOpen && KeyVk(VK_ESCAPE))
        {
            g_helpOpen = false;
            g_helpSwallow = true;
        }
        // The Edit Camera overlay arms again for each Edit Camera (otherwise the
        // Enter that opened the next one would close it at once).
        if (!(g_hasSnapshot && Flag(SnapshotEditingCamera)))
            g_editOverlayArmed = false;
        // A clip that did not open says so.
        {
            static bool failedSeen = false;
            const bool failed = g_hasSnapshot && Flag(SnapshotLoadFailed);
            if (failed && !failedSeen)
            {
                PlayUi(UiSound::Error);
                ShowAlert(TR("alert.title", "Alert"), TR("alert.open_failed", "This clip could not be opened. The file may be damaged or from another version."), nullptr,
                    CommandType::None, "", Screen::Gallery);
            }
            failedSeen = failed;
            // A rename the core could not do (the name is taken, the file is in use).
            static bool renameFailedSeen = false;
            const bool renameFailed = g_hasSnapshot && Flag(SnapshotRenameFailed);
            if (renameFailed && !renameFailedSeen && g_screen != Screen::Closed)
            {
                PlayUi(UiSound::Error);
                ShowAlert(TR("alert.title", "Alert"), TR("alert.rename_failed", "The clip could not be renamed. Another clip may have that name, or the file is in use."), nullptr,
                    CommandType::None, "", Screen::Gallery);
            }
            renameFailedSeen = renameFailed;
            // A clip the core could not write is kept in memory and retried; said here
            // when the editor is open (the game's notice says it otherwise).
            static bool saveFailedSeen = false;
            const bool saveFailed = g_hasSnapshot && Flag(SnapshotSaveFailed);
            if (saveFailed && !saveFailedSeen && g_screen != Screen::Closed && g_screen != Screen::Alert)
            {
                PlayUi(UiSound::Error);
                ShowSaveFailedAlert(g_screen);
            }
            saveFailedSeen = saveFailed;
        }
        // QA kit: Shift+F2 is the core's "mark problem" (not the editor's F2),
        // and the note box keeps every key while it is open.
        // Editor (F2) and MarkProblem (Shift+F2) from the [Keys] table.
        const bool f2 = !noteOpen && ActionPressed(Action::Editor);
        const bool lightsPressed = !noteOpen && !(g_hasSnapshot && Flag(SnapshotEditingCamera)) && ActionPressed(Action::LightEditor);
        const bool lightPlayKey = g_screen == Screen::Lights && !noteOpen && !g_helpOpen && !ImGui::GetIO().WantTextInput &&
            (KeyPressed(ImGuiKey_Space, false) || ActionPressed(Action::PlayPause));
        if (lightPlayKey)
            Send(CommandType::PlayPause);
        const bool lightUndoKey = g_screen == Screen::Lights && !noteOpen && !g_helpOpen && !ImGui::GetIO().WantTextInput &&
            ImGui::GetIO().KeyCtrl && KeyPressed(ImGuiKey_Z, true);
        const bool lightRedoKey = g_screen == Screen::Lights && !noteOpen && !g_helpOpen && !ImGui::GetIO().WantTextInput &&
            ImGui::GetIO().KeyCtrl && KeyPressed(ImGuiKey_Y, true);
        if (lightUndoKey || lightRedoKey)
        {
            const bool redo = lightRedoKey || ImGui::GetIO().KeyShift;
            bool ungroupHistoryHandled = false;
            const bool sameUngroupContext = g_lightRigUngroupUndo.valid &&
                g_lightRigUngroupUndo.clip == g_snapshot.currentClip &&
                g_lightRigUngroupUndo.markerId == CurrentLightMarkerId();
            const auto ungroupMembersAvailable = [&](const LightRigGroup& group) {
                for (const std::uint32_t id : group.memberIds)
                {
                    bool foundLight = false;
                    for (std::uint32_t i = 0; i < std::min(g_snapshot.authoredLightCount, kMaxAuthoredLights); ++i)
                        foundLight = foundLight || g_snapshot.authoredLights[i].id == id;
                    if (!foundLight)
                        return false;
                    for (const LightRigGroup& other : g_lightRigGroups)
                        if (other.id != group.id &&
                            std::find(other.memberIds.begin(), other.memberIds.end(), id) != other.memberIds.end())
                            return false;
                }
                return !group.memberIds.empty();
            };
            if (sameUngroupContext && !redo && g_lightRigUngroupUndo.ungrouped &&
                !FindLightRigGroup(g_lightRigUngroupUndo.group.id) &&
                ungroupMembersAvailable(g_lightRigUngroupUndo.group))
            {
                g_lightRigGroups.push_back(g_lightRigUngroupUndo.group);
                SaveLightRigGroups();
                g_lightRigUngroupUndo.ungrouped = false;
                g_lightSelectedRigGroupId = g_lightRigUngroupUndo.group.id;
                g_lightSelectedId = g_lightRigUngroupUndo.group.memberIds.front();
                g_lightRigSelectionPendingAt = GetTickCount64();
                g_lightDraftIndex = -1;
                g_lightDraftDirty = false;
                g_lightRigEditDraft = {};
                for (std::uint32_t i = 0; i < std::min(g_snapshot.authoredLightCount, kMaxAuthoredLights); ++i)
                    if (g_snapshot.authoredLights[i].id == g_lightSelectedId)
                    {
                        g_lightDraft = g_snapshot.authoredLights[i];
                        g_lightDraftIndex = static_cast<int>(i);
                        break;
                    }
                PlayUi(UiSound::NavLeft);
                ungroupHistoryHandled = true;
            }
            else if (sameUngroupContext && redo && !g_lightRigUngroupUndo.ungrouped)
            {
                LightRigGroup* group = FindLightRigGroup(g_lightRigUngroupUndo.group.id);
                if (group && ungroupMembersAvailable(*group))
                {
                    g_lightRigUngroupUndo.group = *group;
                    const std::uint32_t groupId = group->id;
                    g_lightRigGroups.erase(std::remove_if(g_lightRigGroups.begin(), g_lightRigGroups.end(),
                        [groupId](const LightRigGroup& item) { return item.id == groupId; }), g_lightRigGroups.end());
                    SaveLightRigGroups();
                    g_lightRigUngroupUndo.ungrouped = true;
                    g_lightSelectedRigGroupId = 0;
                    g_lightSelectedId = g_lightRigUngroupUndo.group.memberIds.front();
                    g_lightRigSelectionPendingAt = GetTickCount64();
                    g_lightDraftIndex = -1;
                    g_lightDraftDirty = false;
                    g_lightRigEditDraft = {};
                    for (std::uint32_t i = 0; i < std::min(g_snapshot.authoredLightCount, kMaxAuthoredLights); ++i)
                        if (g_snapshot.authoredLights[i].id == g_lightSelectedId)
                        {
                            g_lightDraft = g_snapshot.authoredLights[i];
                            g_lightDraftIndex = static_cast<int>(i);
                            break;
                        }
                    PlayUi(UiSound::NavRight);
                    ungroupHistoryHandled = true;
                }
            }
            if (ungroupHistoryHandled)
            {
                // The ungroup metadata change is handled locally; do not also
                // consume the camera-marker undo stack for the same keypress.
            }
            else if (!Flag(redo ? SnapshotCanRedo : SnapshotCanUndo))
                PlayUi(UiSound::Error);
            else if (Send(redo ? CommandType::RedoMarkers : CommandType::UndoMarkers))
            {
                g_lightDraftIndex = -1;
                g_lightDraftDirty = false;
                PlayUi(redo ? UiSound::NavRight : UiSound::NavLeft);
            }
        }
        const bool lightEscapeBack = g_screen == Screen::Lights && !noteOpen && !g_helpOpen && !g_helpSwallow &&
            !ImGui::GetIO().WantTextInput && g_lightNumericEditAxis < 0 && KeyPressed(ImGuiKey_Escape, false);
        const bool lightBackspaceBack = g_screen == Screen::Lights && !noteOpen && !g_helpOpen && !g_helpSwallow &&
            !ImGui::GetIO().WantTextInput && g_lightNumericEditAxis < 0 && KeyPressed(ImGuiKey_Backspace, false);
        if (lightsPressed)
        {
            if (g_screen == Screen::Lights)
            {
                RestoreLightMarkerSelection();
                g_screen = g_lightReturnScreen;
                g_lightDraftDirty = false;
                g_lightSettingsMenuOpen = false;
                g_lightRigMenuOpen = false;
                g_lightTargetPickerOpen = false;
                g_reopenLightsWithF2 = false;
                g_lightReopenMarkerId = 0;
            }
            else
            {
                OpenLightsForMarker(CurrentLightMarkerId());
                g_lightSettingsMenuOpen = false;
                g_lightRigMenuOpen = false;
                g_lightTargetPickerOpen = false;
                g_reopenLightsWithF2 = false;
                g_lightReopenMarkerId = 0;
            }
            PlayUi(g_screen == Screen::Lights ? UiSound::Select : UiSound::Back);
        }
        else if (lightEscapeBack || lightBackspaceBack)
        {
            if (g_lightTargetPickerOpen)
                g_lightTargetPickerOpen = false;
            else if (g_lightSplineDefaultsOpen)
                g_lightSplineDefaultsOpen = false;
            else if (g_lightSplineOpen)
            {
                g_lightSplineOpen = false;
                g_lightSplineDefaultsOpen = false;
            }
            else if (g_lightSettingsMenuOpen)
                g_lightSettingsMenuOpen = false;
            else if (g_lightRigMenuOpen)
            {
                g_lightRigMenuOpen = false;
                g_lightRigStatus[0] = '\0';
            }
            else if (g_lightAdvancedOpen)
            {
                g_lightAdvancedOpen = false;
            }
            else
            {
                RestoreLightMarkerSelection();
                g_screen = g_lightReturnScreen;
                g_lightDraftDirty = false;
                g_reopenLightsWithF2 = false;
                g_lightReopenMarkerId = 0;
            }
            PlayUi(UiSound::Back);
        }
        if (g_screen == Screen::Lights && !noteOpen && !g_helpOpen && !ImGui::GetIO().WantTextInput &&
            g_lightNumericEditAxis < 0 && !(g_hasSnapshot && Flag(SnapshotEditingCamera)) && ActionPressed(Action::FreeCam))
        {
            const bool enteringFreeCam = !(g_hasSnapshot && Flag(SnapshotFreeCamEnabled));
            if (enteringFreeCam)
            {
                RestoreLightMarkerSelection();
                g_lightReopenMarkerId = CurrentLightMarkerId();
                g_reopenLightsWithF2 = true;
                g_lightSettingsMenuOpen = false;
                g_lightRigMenuOpen = false;
                g_lightTargetPickerOpen = false;
                g_screen = Screen::Closed;
            }
            Send(CommandType::ToggleFreeCam);
        }
        // With the editor's keyboard (interactive) the core does not see
        // Shift+F2: sent through the QA block.
        if (!noteOpen && g_toldOpen && ActionPressed(Action::MarkProblem))
            qa_shots::RequestMark();
        if (f2 && g_hasSnapshot && Flag(SnapshotEditingCamera))
        {
            // F2 in Edit Camera cancels it (the overlay and the core's Edit Camera end
            // together).
            Command command{};
            command.keyframeId = g_snapshot.editingMarker;
            command.target = 1;
            Send(CommandType::EditCameraKeyframe, &command);
            PlayUi(UiSound::Back);
        }
        else if (f2)
        {
            const bool wasLighting = g_screen == Screen::Lights;
            if (wasLighting)
            {
                RestoreLightMarkerSelection();
                if (g_hasSnapshot && Flag(SnapshotFreeCamEnabled))
                {
                    g_lightReopenMarkerId = CurrentLightMarkerId();
                    g_reopenLightsWithF2 = true;
                }
                else
                {
                    g_lightReopenMarkerId = 0;
                    g_reopenLightsWithF2 = false;
                }
            }
            if (g_screen == Screen::Closed)
            {
                if (g_reopenLightsWithF2 && g_hasSnapshot)
                {
                    const Screen returnScreen = g_lightReturnScreen;
                    std::uint32_t markerId = g_lightReopenMarkerId;
                    bool markerStillExists = false;
                    for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
                        markerStillExists = markerStillExists || g_snapshot.cameraKeyframes[n].id == markerId;
                    if (!markerStillExists)
                        markerId = CurrentLightMarkerId();
                    OpenLightsForMarker(markerId);
                    if (returnScreen != Screen::Closed)
                        g_lightReturnScreen = returnScreen;
                    if (returnScreen == Screen::Edit && markerId)
                        g_selectedKeyframeId = markerId;
                    g_reopenLightsWithF2 = false;
                    g_lightReopenMarkerId = 0;
                }
                else
                {
                    // The project's timeline while it plays as a whole or is open with nothing
                    // playing.
                    g_screen = playing ? PlaybackScreen() : ProjectIsOpen() ? Screen::Project : Screen::Home;
                    if (g_screen == Screen::Home)
                        RefreshClipList(false);
                }
                PlayUi(UiSound::Select);
            }
            else
            {
                g_screen = Screen::Closed;
                PlayUi(UiSound::Back);
            }
            g_hide.Show();
        }
        // Debug mode (RedemptionEditor.ini): no playback bar, the core's text HUD
        // is the interface and F2 still opens the editor.
        const bool debug = g_hasSnapshot && Flag(SnapshotDebugMode);
        // E from the playback HUD enters editing; H hides and shows the edit UI.
        if (!debug && !noteOpen && playing && g_screen == Screen::Closed && !Flag(SnapshotFreeCamEnabled) && KeyVk('E'))
        {
            g_screen = PlaybackScreen();
            g_hide.Show(); // opening the editor asks for its UI
            PlayUi(UiSound::Select);
        }
        // Not in Edit Camera (H would hide the editor unseen there). Also with the
        // editor closed while a clip plays (the playback bar and the key bar go
        // too). A sound, no text: a notice would be in an external capture.
        {
            replay::editor::uiqa::HideKeyContext hideKey;
            hideKey.editScreen = g_screen == Screen::Edit;
            hideKey.closedScreen = g_screen == Screen::Closed;
            hideKey.playing = playing;
            hideKey.debug = debug;
            hideKey.noteOpen = noteOpen;
            hideKey.editingCamera = g_hasSnapshot && Flag(SnapshotEditingCamera);
            if (replay::editor::uiqa::HideKeyApplies(hideKey) && ActionPressed(Action::HideUi))
            {
                g_hide.Toggle();
                PlayUi(g_hide.hidden ? UiSound::Back : UiSound::Select);
            }
        }
        g_hide.Tick(GetTickCount64(), playing);
        if (!playing && g_screen == Screen::Edit)
            g_screen = g_editingProjectClip && ProjectIsOpen() ? Screen::Project : Screen::Home;
        UpdateQaNotices(playing); // the reminder when a playback stops with marks waiting
        SyncLightEditorState();
#ifndef REDEMPTION_UI_HARNESS
        CheckCrashNotice(); // once, a crash of an earlier session (the harness sets it)
        CheckStorageNotice(); // once, nowhere to save (the harness sets it)
#endif

        const ImGuiIO& io = ImGui::GetIO();
        UpdateLayoutScale(io.DisplaySize);
        UseStage(g_screen == Screen::Alert || g_screen == Screen::Rename || g_screen == Screen::ProjectName || noteOpen);
        if (g_screen == Screen::Gallery || g_screen == Screen::Projects)
            UseLeftStage();
        // Edit Camera hands input to the game. Free Cam can be paused under the
        // interactive Lighting page so its controls can be edited at the held view.
        const bool editingCamera = g_hasSnapshot && Flag(SnapshotEditingCamera);
        const bool interactive = (g_screen != Screen::Closed && !(g_screen == Screen::Edit && UiHidden()) && !editingCamera) || noteOpen;
        // Why, for the CURSOR_STATE line.
        const replay::editor::cursor::Reason cursorReason = noteOpen ? replay::editor::cursor::Reason::Note
            : g_screen == Screen::Closed                             ? replay::editor::cursor::Reason::Closed
            : editingCamera                                          ? replay::editor::cursor::Reason::EditCamera
            : !interactive                                           ? replay::editor::cursor::Reason::UiHidden
                                                                     : replay::editor::cursor::Reason::Screen;
        SetInteractive(runtime, interactive, cursorReason);
#ifdef REDEMPTION_UI_HARNESS
        if (g_harnessMouse.on && g_harnessMouse.moved)
        {
            // A fixture's mouse that just moved there: from a little to its left.
            g_menuPointer.Reset();
            g_menuPointer.BeginFrame(true, io.MousePos.x - 8.0f, io.MousePos.y, false);
        }
#endif
        BeginMenuPointerFrame(); // after the open's Reset, before the screens
        if (interactive != g_toldOpen && g_hasSnapshot)
        {
            Command command{};
            command.enabled = interactive ? 1u : 0u;
            if (Send(CommandType::SetEditorOpen, &command))
                g_toldOpen = interactive;
        }
        // The loading cue is the editor's (DrawLoadingBadge): the core leaves out its
        // own text while this keep-alive comes (every 2 s, kept 5 s there).
        {
            static ULONGLONG cueToldAt = 0;
            const ULONGLONG tick = GetTickCount64();
            if (g_hasSnapshot && (cueToldAt == 0 || tick - cueToldAt >= 2000))
            {
                Command command{};
                command.enabled = 1u;
                if (Send(CommandType::SetLoadingCue, &command))
                    cueToldAt = tick;
            }
        }
        // The camera markers in the world, under every window; not with the UI
        // hidden (H), the help, the note box, an export or a frozen seek picture.
        if (g_hasSnapshot && playing && !UiHidden() && !g_helpOpen && !noteOpen && !render_export::ExportController::Get().Busy() &&
            g_snapshot.seekLoadState != SeekLoadFrozen)
            DrawCameraOverlay(ImGui::GetBackgroundDrawList());
        // The recording notices (replay on, REC, saving, clip saved) are drawn with
        // the editor closed too.
        if (g_screen == Screen::Closed && (!playing || debug) && !RecordingNoticeVisible() && !WorldNoticeVisible() && !noteOpen &&
            !QaNoticeVisible() && !CrashNoticeVisible() && !StorageNoticeVisible() && !LoadingBadgeCaption())
            return;

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
            // No ImGui keyboard navigation in the editor's window. The ConfigFlags write
            // (SetInteractive) is lost under ReShade 6.8 (src/editor_cursor.h) and Space
            // or Enter could re-fire the last clicked button; the window flag is ours
            // and reaches ImGui whatever ReShade serves (SetKeyboardFocusHere still
            // focuses the text boxes).
            (interactive ? ImGuiWindowFlags_NoNavInputs : ImGuiWindowFlags_NoInputs);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        if (ImGui::Begin("##RDR2ReplayEditor", nullptr, flags))
        {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            // The key bar's height first (the screens anchored to the bottom edge stand
            // above it); the bar itself is drawn after the screen, with the keys of the
            // state the screen left.
            {
                KeyTips tips;
                ScreenTips(tips);
                g_footH = KeyBarHeight(KeyBarRows(tips).size());
            }
            if (!g_hasSnapshot && interactive)
            {
                draw->AddRectFilled(ImVec2(0.0f, 0.0f), ImGui::GetIO().DisplaySize, kShade);
                Text(draw, 80.0f, 320.0f, 24.0f, kText, TR("waiting.asi", "Waiting for RedemptionEditor.asi..."));
                LoadingAfter(draw, 80.0f, 320.0f, 24.0f, TR("waiting.asi", "Waiting for RedemptionEditor.asi..."), g_vw - 40.0f);
            }
            else if (noteOpen)
            {
                // QA kit: the note box over the playback (which goes on).
                DrawQaNote(draw);
            }
            else if (g_helpOpen && interactive)
            {
                // The help alone (clicks on it must not go through to the screen under it).
                draw->AddRectFilled(ImVec2(0.0f, 0.0f), ImGui::GetIO().DisplaySize, IM_COL32(0, 0, 0, 150));
                DrawHelp(draw);
            }
            else
            {
                switch (g_screen)
                {
                case Screen::Home: DrawHome(draw); break;
                case Screen::Gallery: DrawGallery(draw); break;
                case Screen::Settings: DrawSettings(draw); break;
                case Screen::Alert: DrawAlert(draw); break;
                case Screen::Rename: DrawRename(draw); break;
                case Screen::Projects: DrawProjects(draw); break;
                case Screen::Project: DrawProject(draw); break;
                case Screen::ProjectName: DrawProjectName(draw); break;
                case Screen::Export: DrawExport(draw); break;
                case Screen::Lights:
                    if (g_lightReturnScreen == Screen::Edit && !UiHidden())
                        DrawEditTimelineOverlay(draw);
                    DrawLights(draw);
                    break;
                case Screen::Edit:
                    // Hidden, nothing (the playback bar goes too).
                    if (editingCamera)
                        DrawEditCameraOverlay(draw);
                    else if (!UiHidden())
                        DrawEdit(draw);
                    if (!editingCamera && !UiHidden())
                        DrawWorldNotice(draw);
                    break;
                case Screen::Closed:
                    g_helpOpen = false;
                    // Hidden: no bar, no notice and no fade of the add-on's (the core's fade, in
                    // the picture, stays).
                    if (!UiHidden())
                    {
                        DrawClipFade(draw);
                        if (playing && !debug)
                            DrawPlaybackHud(draw);
                        DrawRecordingNotices(draw);
                        DrawWorldNotice(draw);
                    }
                    break;
                }
            }
            if (!noteOpen && !g_helpOpen && g_hasSnapshot)
                DrawLoadingBadge(draw);
            if (!noteOpen && !g_helpOpen)
                DrawQaNotice(draw);
            if (g_screen != Screen::Closed)
                g_crashNotice.until = 0; // opening the editor dismisses the crash notice
            if (!noteOpen && !g_helpOpen && g_screen == Screen::Closed)
            {
                // Nowhere to save first; the crash notice once it is gone.
                DrawStorageNotice(draw, 110.0f);
                if (!StorageNoticeVisible())
                    DrawCrashNotice(draw); // the editor closed, over the game
            }
            KeyTips tips;
            ScreenTips(tips);
            DrawKeyBar(draw, tips);
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    // Bench screenshots: a request from ReplayProbe is saved on the next
    // present, with the request's tag as the file name postfix.
    replay::bench::ShotShared* g_benchShot = nullptr;
    unsigned g_benchShotRetry = 0;

    void OnDestroyRuntime(reshade::api::effect_runtime* runtime)
    {
        render_export::ExportController::Get().Shutdown(runtime);
        qa_shots::Release();
        seek_freeze::Release();
        clip_thumbnails::Release();
        display_font::Release();
        ui_art::Release();
        g_runtime = nullptr;
    }

    void OnPresent(reshade::api::effect_runtime* runtime)
    {
        {
            // The present thread's id in this core's thread table (not renamed: it is
            // the game's).
            static bool noted = false;
            if (!noted)
            {
                noted = true;
                replay::threads::NoteForeign("present");
                const std::string line = "THREADS,module=RedemptionEditorCore.dll," + replay::threads::MapText();
                reshade::log::message(reshade::log::level::info, line.c_str());
            }
        }
        render_export::ExportController::Get().OnPresent(runtime);
        clip_thumbnails::OnPresent(runtime, g_hasSnapshot && Flag(SnapshotRecording), g_hasSnapshot && Flag(SnapshotGamePaused));
        if (!g_benchShot)
        {
            // Opened lazily, retried about once a second until it exists.
            if ((g_benchShotRetry++ % 60) != 0)
                return;
            g_benchShot = replay::bench::OpenShotShared();
            if (!g_benchShot)
                return;
        }
        const LONG64 requested = InterlockedCompareExchange64(&g_benchShot->requested, 0, 0);
        if (requested == InterlockedCompareExchange64(&g_benchShot->saved, 0, 0))
            return;
        char tag[replay::bench::kShotTagSize];
        std::memcpy(tag, g_benchShot->tag, sizeof(tag));
        tag[sizeof(tag) - 1] = '\0';
        runtime->save_screenshot(tag);
        InterlockedExchange64(&g_benchShot->saved, requested);
    }
}

// The editor is loaded by the ReShade add-on in editor_loader.cpp from a
// copy of this DLL, and replaced when the file changes (the loader owns the
// ReShade registration). EditorCoreStop leaves nothing behind: the core
// text HUD comes back, the font texture and every handle are released.
extern "C" __declspec(dllexport) bool EditorCoreStart(const imgui_function_table* table)
{
    imgui_function_table_instance() = table;
    render_export::ExportController::Get().LoadConfig(); // off without [Export] Enabled=1
    return table != nullptr;
}

// The Redemption Editor host (host/, no ReShade) calls this before
// EditorCoreStart: the module that answers reshade::log::message and
// ReShadeVersion (render_export's query ABI) is the host DLL, not whatever
// module the header's lookup would find first (a ReShade also loaded in the
// process). Never called under ReShade, where the lookup finds ReShade.
extern "C" __declspec(dllexport) void EditorCoreSetApiModule(HMODULE module)
{
    reshade::internal::get_reshade_module_handle(module);
}

extern "C" __declspec(dllexport) void EditorCoreOverlay(reshade::api::effect_runtime* runtime) { OnOverlay(runtime); }
extern "C" __declspec(dllexport) void EditorCorePresent(reshade::api::effect_runtime* runtime) { OnPresent(runtime); }
extern "C" __declspec(dllexport) void EditorCoreDestroyRuntime(reshade::api::effect_runtime* runtime) { OnDestroyRuntime(runtime); }

extern "C" __declspec(dllexport) void EditorCoreStop()
{
    render_export::ExportController::Get().Shutdown(g_runtime);
#ifndef REDEMPTION_UI_HARNESS
    g_recWindow.Stop(); // the recording indicator's window and its thread
#endif
    if (g_toldOpen && g_hasSnapshot)
    {
        Command command{};
        command.enabled = 0;
        Send(CommandType::SetEditorOpen, &command);
    }
    if (imgui_function_table_instance())
    {
        ImGui::GetIO().MouseDrawCursor = false;
        if (g_configFlagsHeld) // ReShade's keyboard navigation back
            ImGui::GetIO().ConfigFlags = g_previousConfigFlags;
    }
    display_font::Release();
    ui_art::Release();
    clip_thumbnails::Release();
    qa_shots::Release();
    seek_freeze::Release();
    if (g_closeRequest)
        CloseHandle(g_closeRequest);
    g_closeRequest = nullptr;
    if (g_benchShot)
        UnmapViewOfFile(g_benchShot);
    g_benchShot = nullptr;
    DisconnectTransport();
}

#ifdef REDEMPTION_UI_HARNESS
// The UI harness's fixtures, in this translation unit to reach the editor's
// state (tools/ui_harness, ReplayEditorUiTests.vcxproj).
#include "../tools/ui_harness/ui_harness_fixtures.inl"
#endif
