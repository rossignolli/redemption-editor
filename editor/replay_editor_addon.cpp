#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H

#include <imgui.h>
#include <reshade.hpp>

#include "bench_shot.h"
#include "display_font.h"
#include "clip_thumbnails.h"
#include "seek_freeze.h"
#include "editor_protocol.h"
#include "project_name.h" // CleanName, CheckRename: the name screen shows what the core does
#include "env_override.h" // the weather names
#include "editor_input.h"
#include "editor_ui_qa.h"
#include "editor_cursor.h" // the cursor under ReShade 6.8
#include "world_notice.h"
#include "gpu_events.h" // the loader's GPU event table
#include "render_export/export_controller.h" // render export
#include "qa_marks.h" // the note box's time text
#include "qa_shots.h" // QA kit: screenshot ring, mark pictures, note box
#include "mod_identity.h" // RedemptionEditor.ini
#include "editor_keys_ini.h" // [Keys], the table the core loads too
#include "camera_overlay_channel.h" // the camera markers in the world
#include "camera_shake_custom.h" // the Custom shake's ranges and presets
#include "camera_effects.h" // Simple Camera's presets; the marker's shake rows
#include "camera_bones.h" // the bone list of the attach and Look At rows
#include "ui_layout_check.h" // the key bar's rows, the label fit
#include "ui_probe.h" // UI_PROBE_* tell the UI harness what is drawn (empty in the add-on)
#include "ui_art.h" // the painted panel edges and timeline (RedemptionEditor\ui)
#include "crash_notice.h" // the notice after a crash
#include "i18n.h" // TR, TRF: the texts in the player's language
#include "qa_report.h" // the report folder's name, for the crash notice
#include "rec_indicator.h" // Replay Settings' "Recording indicator"
#include "rec_indicator_window.h" // its window that screen capture leaves out
#include "ui_scale.h" // Replay Settings' "Interface scale"

#include <Windows.h>
#include <dxgi.h>
#include <shellapi.h> // SHQueryUserNotificationState
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

// Redemption Editor front end: home, clip gallery, alert, playback HUD and
// edit panel with timeline, drawn in ImGui with the look of the game's menus
// (dark translucent panels, warm white text, the red brush behind the
// selection). All UI text is English. The editor only talks to ReplayProbe
// through the shared transport (editor_protocol.h); it never touches the
// engine. While the editor is open ReShade keeps the keyboard from the game,
// so the core's own keys (K, , ., [ ], F11) do not reach it: the edit screen
// handles them itself. With the UI hidden (H) the keys go to the game and the
// core again.

namespace
{
    using namespace replay::editor;

    // ------------------------------------------------------------------
    // Transport to ReplayProbe
    // ------------------------------------------------------------------
    HANDLE g_transportMapping = nullptr;
    SharedTransport* g_transport = nullptr;
    Snapshot g_snapshot{};
    bool g_hasSnapshot = false;

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
    }

    bool TryConnect()
    {
        return EnsureTransport() && SharedTransportOnline(*g_transport, GetCurrentProcessId());
    }

    bool Send(CommandType type, const Command* payload = nullptr)
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

    void PlayUi(UiSound sound)
    {
        if (g_pendingUiSound < 0 || UiSoundRank(sound) > UiSoundRank(static_cast<UiSound>(g_pendingUiSound)))
            g_pendingUiSound = static_cast<int>(sound);
    }

    void PlayUiStep(int step) { PlayUi(step < 0 ? UiSound::NavLeft : UiSound::NavRight); }

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

    bool MissedTap(int vk)
    {
        return g_runtime && vk > 0 && vk < 256 && g_keyEdges.MissedTap(static_cast<unsigned>(vk), g_runtime->is_key_released(vk));
    }

    bool KeyVk(int vk) { return g_runtime && (g_runtime->is_key_pressed(vk) || MissedTap(vk)); }

    // The same for the left mouse button. ReShade keeps the button like a key
    // (VK_LBUTTON, set by WM_LBUTTONDOWN/UP and raw input) and feeds ImGui its
    // level once a frame, so a click inside a long frame never reaches ImGui;
    // is_mouse_button_released still says it came up.
    void BeginMouseFrame()
    {
        if (g_runtime)
            g_keyEdges.BeginFrame(VK_LBUTTON, g_runtime->is_mouse_button_down(0));
    }

    bool MissedClick() { return g_runtime && g_keyEdges.MissedTap(VK_LBUTTON, g_runtime->is_mouse_button_released(0)); }

    // A left click this frame: ImGui's, or one that fell inside the last frame.
    bool MouseClicked() { return ImGui::IsMouseClicked(ImGuiMouseButton_Left) || MissedClick(); }

    // The last item was clicked (InvisibleButton's own press, or a lost one over it).
    bool ItemClicked(bool pressed) { return pressed || (ImGui::IsItemHovered() && MissedClick()); }

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

    // ImGui's press, or a whole press that fell inside the last frame.
    bool KeyPressed(ImGuiKey key, bool repeat = true) { return ImGui::IsKeyPressed(key, repeat) || MissedTap(VkOf(key)); }

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

    // The [Keys] table (editor_keys.h), loaded once from the same
    // RedemptionEditor.ini the core reads (its refusals are the core's log lines
    // and notice: same file, same pure Load, same result).
    using replay::keys::Action;
    namespace fixedkey = replay::keys::fixed; // the keys no [Keys] entry moves
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

    // A press of one of the action's keys with its modifiers (the same rule
    // as the core's, replay::keys::Matches); `repeat`: a held key repeats.
    bool ActionPressed(Action action, bool repeat = false)
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

    // The key bar's and the help's text of an action: "K", "F2 / E" with a
    // fixed key after, "-" when the action has no key.
    const char* KeyText(Action action, const char* extra = nullptr, const char* separator = " / ")
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

    // ------------------------------------------------------------------
    // Look: the palette of the game's menus (placeholders for the art)
    // ------------------------------------------------------------------
    constexpr ImU32 kShade = IM_COL32(8, 7, 6, 190);        // veil over the game frame
    constexpr ImU32 kPanel = IM_COL32(12, 11, 10, 225);     // panel background
    constexpr ImU32 kPanelLine = IM_COL32(236, 230, 218, 60);
    constexpr ImU32 kText = IM_COL32(236, 230, 218, 255);   // warm white
    constexpr ImU32 kMuted = IM_COL32(150, 143, 131, 255);
    constexpr ImU32 kDark = IM_COL32(13, 12, 11, 255);      // text on the red brush
    constexpr ImU32 kRed = IM_COL32(179, 22, 15, 255);      // the RDR2 red
    constexpr ImU32 kRedDim = IM_COL32(179, 22, 15, 120);
    constexpr ImU32 kGold = IM_COL32(226, 181, 79, 255);    // camera keyframes
    constexpr ImU32 kTrack = IM_COL32(236, 230, 218, 22);

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

    // Replay Settings' "Interface scale" (src/ui_scale.h): RedemptionEditor.ini
    // InterfaceScale in percent, read once, written when the row changes it.
    // g_scale is the display's 1080 units times it (UpdateLayoutScale), so the
    // fonts (baked once, drawn at any size), the art, the cursor, the notices
    // and every hit test follow without a rebake.
    int g_uiScalePercent = replay::editor::uiscale::kDefault;
    bool g_uiScaleRead = false;
    float g_uiScaleFactor = 1.0f; // the factor in use on this display (uiscale::Effective)

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

    // The units of this frame: 1080 of them over the display's height at 100%.
    void UpdateLayoutScale(const ImVec2& display)
    {
        ReadInterfaceScale();
        g_uiScaleFactor = replay::editor::uiscale::Effective(g_uiScalePercent, display.x, display.y);
        g_scale = display.y / 1080.0f * g_uiScaleFactor;
        g_vw = display.x / g_scale;
        g_vh = display.y / g_scale;
    }

    float S(float value) { return value * g_scale; }

    ImVec2 P(float x, float y) { return ImVec2(S(x + g_ox), S(y + g_oy)); }

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

    // The pause menu's column: dark and solid under the items, fading into the
    // game past `solidWidth`, over the whole screen height.
    void MenuColumn(ImDrawList* draw, float solidWidth)
    {
        draw->AddRectFilled(P(-g_ox, -g_oy), P(solidWidth, g_vh - g_oy), IM_COL32(10, 9, 8, 225));
        draw->AddRectFilledMultiColor(P(solidWidth, -g_oy), P(solidWidth + 280.0f, g_vh - g_oy), IM_COL32(10, 9, 8, 225),
            IM_COL32(10, 9, 8, 0), IM_COL32(10, 9, 8, 0), IM_COL32(10, 9, 8, 225));
    }

    // Every single line label uses the title font; the condensed capitals read
    // small, so labels under title size are set a little larger. Paragraphs use
    // the body font (Paragraph).
    constexpr float kTitleSize = 24.0f;

    float TitleFontSize(float size) { return size >= kTitleSize ? size : size * 1.18f; }

    // The pixel size of a label and of a paragraph, never under the fonts'
    // readable minimum (display_font.h kMinCapPx), so small labels stay legible
    // at 720 to 800 lines. Every text and every measure of one goes through
    // these, so the layout measures what is drawn.
    float TitlePx(float size) { return std::max(S(TitleFontSize(size)), display_font::g_title.MinPx()); }
    float BodyPx(float size) { return std::max(S(size), display_font::g_body.MinPx()); }

    // The inner padding of every panel that holds text (its content stands this
    // far from each edge): the ink edge's band (kInkEdge) and a clear margin
    // inside it.
    constexpr float kPanelPad = 28.0f;
    // Under a panel's last line: clear of the ink band (kInkEdge) too.
    constexpr float kPanelPadBottom = kPanelPad - 6.0f;

    // The panels' worn letterpress edge. The body is drawn inset by the edge; the
    // edge_ink strip (opaque inside, ragged outside) goes along the four sides in
    // the body's colour, tiled with every other tile mirrored, and the corners
    // are split on their diagonal between the two sides (a mitre: the two strips
    // never draw over each other). Without the art, a flat panel. Every text
    // panel draws through it.
    constexpr float kInkEdge = 16.0f;
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

    // A text panel from (x0, y0) to (x1, y1) in layout units; `edge` the ink
    // band (smaller on a small box, a quarter of its least side at most).
    // `corners` false: no corner pieces (a panel whose rows and buttons reach
    // into its corners).
    void InkPanel(ImDrawList* draw, float x0, float y0, float x1, float y1, ImU32 color, float edge = kInkEdge, bool corners = true)
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
        return display_font::g_title.DrawWrapped(draw, P(x, y), TitlePx(size), color, text, S(wrap)) / g_scale;
    }

    // The red brush stroke behind a selected item, like the pause menu
    // selection: the painted stroke (brush_red.png), its body's rows 8 to 120
    // over the band, the start and the frayed tail at their own aspect and the
    // body stretched between them; without the art, a band with a ragged
    // right edge.
    void Brush(ImDrawList* draw, float x, float y, float width, float height, ImU32 color)
    {
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
    constexpr ImU32 kRowInkGrey = IM_COL32(206, 200, 190, 235);
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

    void RowBox(ImDrawList* draw, float x0, float y0, float x1, float y1, RowLook look)
    {
        UI_PROBE_BOX("row", P(x0, y0), P(x1, y1));
#ifdef REDEMPTION_UI_HARNESS
        ++g_rowLooks[static_cast<int>(look)];
        g_rowBoxes.push_back(ImVec4(x0 + g_ox, y0 + g_oy, x1 + g_ox, y1 + g_oy));
#endif
        const bool selected = look == RowLook::Selected || look == RowLook::SelectedGreyed;
        const ImU32 body = selected ? kRowBodySelected : kRowBody;
        if (ui_art::Get(ui_art::Art::RowBox))
            ThreeSlice(draw, ui_art::Art::RowBox, 32.0f, x0, y0, x1, y1, body);
        else
            draw->AddRectFilled(P(x0, y0), P(x1, y1), body);
        if (look == RowLook::Rest)
            return;
        const ImU32 ink = look == RowLook::Selected ? kRed : look == RowLook::SelectedGreyed ? kRowInkGreyed : kRowInkGrey;
        if (ui_art::Get(ui_art::Art::RowInk))
            ThreeSlice(draw, ui_art::Art::RowInk, 48.0f, x0 - kRowInkOut, y0 - kRowInkOut, x1 + kRowInkOut, y1 + kRowInkOut, ink);
        else
            draw->AddRect(P(x0 - 1.0f, y0 - 1.0f), P(x1 + 1.0f, y1 + 1.0f), ink, 0.0f, 0, S(2.0f));
    }

    // The room the reel mark takes before a banner's title of `height` units
    // (0 without its art).
    float ReelMarkRoom(float height) { return ui_art::Get(ui_art::Art::ReelMark) ? height * 0.74f + 12.0f : 0.0f; }

    // A screen's title banner: the red brush from (x, y), `height` tall, at
    // least `minWidth` wide and `extra` wider than the title; the title at
    // (textX, textY), `size` units. Painted, the Cylinder Reel stands at the
    // title's start and the title and the brush move right by its room.
    void TitleBanner(ImDrawList* draw, float x, float y, float minWidth, float extra, float height, float textX, float textY, float size, const char* title)
    {
        const float room = ReelMarkRoom(height);
        Brush(draw, x, y, std::max(minWidth, TextWidth(size, title) / g_scale + extra + room), height, kRed);
        if (const ImTextureID reel = ui_art::Get(ui_art::Art::ReelMark))
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
    void OrnamentDivider(ImDrawList* draw, float x, float y, float width)
    {
        if (const ImTextureID texture = ui_art::Get(ui_art::Art::Divider))
        {
            const float k = width / 853.0f; // layout units per texel
            const float left = x - 85.0f * k, top = y + 0.5f - 24.0f * k;
            draw->AddImage(ImTextureRef(texture), P(left, top), P(left + 1024.0f * k, top + 48.0f * k), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                IM_COL32(255, 255, 255, 200));
            return;
        }
        const float mid = x + width * 0.5f;
        const ImU32 solid = IM_COL32(236, 230, 218, 110);
        const ImU32 clear = IM_COL32(236, 230, 218, 0);
        draw->AddRectFilledMultiColor(P(x, y), P(mid - 14.0f, y + 1.0f), clear, solid, solid, clear);
        draw->AddRectFilledMultiColor(P(mid + 14.0f, y), P(x + width, y + 1.0f), solid, clear, clear, solid);
        draw->AddQuadFilled(P(mid, y - 4.0f), P(mid + 4.0f, y + 0.5f), P(mid, y + 5.0f), P(mid - 4.0f, y + 0.5f), solid);
        draw->AddCircleFilled(P(mid - 9.0f, y + 0.5f), S(1.4f), solid);
        draw->AddCircleFilled(P(mid + 9.0f, y + 0.5f), S(1.4f), solid);
    }

    // A section title as the game menus set it: small capitals, centred,
    // between two short hairlines.
    void SectionTitle(ImDrawList* draw, float x, float y, float width, const char* title)
    {
        const float titleWidth = TextWidth(14.0f, title) / g_scale;
        const float mid = x + width * 0.5f;
        Text(draw, mid - titleWidth * 0.5f, y, 14.0f, kMuted, title);
        const ImU32 line = IM_COL32(236, 230, 218, 60);
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

    // A list row's label: in another language than English a long one is set
    // smaller (to 80 % of `size`) before it is cut with "..."; English draws
    // it as TextFit.
    // `shrink`: in English too (a row newer than the release layout).
    void RowTextFit(ImDrawList* draw, float x, float y, float size, ImU32 color, const char* text, float maxWidth, bool shrink = false)
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

    // The busy cue: the cylinder turning one chamber at a time (a frame every
    // 50 ms, the loop 60 degrees), centred on (x, y). False without its art (the
    // caller keeps its text cue).
    bool LoadingCue(ImDrawList* draw, float x, float y, float size, ImU32 tint = IM_COL32(255, 255, 255, 255))
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

    // The description panel beside a menu (Home, Replay Settings): a heading, a
    // paragraph, then a detail line or a note. The content is kDescWidth wide,
    // kPanelPad from the panel's edges; the panel grows to its measured content.
    constexpr float kDescWidth = 392.0f;

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

    // ---------------- The painted timeline ----------------
    // The track's torn strip spans kTrackTop to kTrackBottom from the bar's top
    // (y); the marks' first lane is its middle.
    constexpr float kTrackTop = -5.0f;
    constexpr float kTrackBottom = 7.0f;
    constexpr ImU32 kTrackTint = IM_COL32(150, 143, 131, 190);
    constexpr ImU32 kIconTint = IM_COL32(222, 222, 222, 255); // a mark at rest; hover and selection draw it at full brightness

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

    // The playhead: the watch crown and its needle, the needle's tip at
    // (x, tipY), the drawing `height` units tall (the art's 134 of 136 rows,
    // the tip on row 135 at column 67.5).
    constexpr float kPlayheadRows = 134.0f;
    void PlayheadIcon(ImDrawList* draw, float x, float tipY, float height)
    {
        const ImTextureID texture = ui_art::Get(ui_art::Art::IconPlayhead);
        if (!texture)
            return;
        const float box = height * 136.0f / kPlayheadRows;
        const float left = x - box * 67.5f / 136.0f, top = tipY - box * 135.0f / 136.0f;
        draw->AddImage(ImTextureRef(texture), P(left, top), P(left + box, top + box), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255));
    }

    // The seam between two clips of a project on the painted bar: the gold
    // stitched post (rows 8 to 95 of 104, its stroke on column 52) from top to
    // bottom at x. False without the painted timeline, or when the clips are
    // `spacing` units apart on average, closer than kSeamMinSpacing (the
    // caller draws its line).
    constexpr float kSeamMinSpacing = 24.0f;
    bool SeamMark(ImDrawList* draw, float x, float top, float bottom, float spacing)
    {
        if (!TimelineArtReady() || !ui_art::Get(ui_art::Art::IconSeam) || spacing < kSeamMinSpacing)
            return false;
        const float k = (bottom - top) / 88.0f;
        OutlinedIcon(draw, ui_art::Art::IconSeam, x, top + 44.0f * k, 104.0f * k, kIconTint);
        return true;
    }

    // A menu or tool icon centred on (x, y): gold, its white twin on the
    // selected row (`onBrush`), dimmed on a greyed item. Nothing without its art.
    void MenuIcon(ImDrawList* draw, ui_art::Art art, float x, float y, float size, bool onBrush, bool enabled = true)
    {
        if (art == ui_art::Art::Count)
            return;
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

    // ------------------------------------------------------------------
    // Screens
    // ------------------------------------------------------------------
    enum class Screen
    {
        Closed,
        Home,
        Gallery,
        Alert,
        Edit,
        Settings,
        Rename, // a clip's new name (the gallery's R)
        // Projects: clips put together into one montage.
        Projects,    // the project files: open, new, rename, delete
        Project,     // the open project's timeline: its clips in order
        ProjectName, // a new project's name, or a project's new name
        Export, // render export
        Count
    };
    Screen g_screen = Screen::Closed;

    // The open project (Snapshot::projectFlags).
    bool ProjectFlag(std::uint32_t flag) { return g_hasSnapshot && (g_snapshot.projectFlags & flag) != 0; }
    bool ProjectIsOpen() { return ProjectFlag(ProjectOpenFlag); }
    // The screen of a playback: the project's timeline while the project
    // plays as a whole, the clip editor otherwise (a project clip in it too).
    Screen PlaybackScreen() { return ProjectFlag(ProjectPreviewing) ? Screen::Project : Screen::Edit; }
    // The clip editor shows a project's clip (Esc goes back to the project).
    bool g_editingProjectClip = false;
    bool g_previousMouseDrawCursor = false;
    // H hides the replay UI: in the clip editor (fly the free camera with the UI
    // hidden) and with the editor closed while a clip plays: the playback bar,
    // the key bar and the notices go, the replay keys (the core's) keep working.
    replay::editor::uiqa::HideState g_hide;
    bool UiHidden()
    {
        const bool playing = g_hasSnapshot && Flag(SnapshotPlaying);
        return g_hide.HiddenOn(g_screen == Screen::Edit, g_screen == Screen::Closed, playing);
    }

    // Alert: a message with Continue (optional action) and Cancel.
    struct Alert
    {
        std::string title;
        std::string body;
        std::string confirmLabel;
        CommandType action = CommandType::None;
        std::string actionName;
        Screen back = Screen::Home;
        Screen after = Screen::Count; // where Continue goes (Count: back)
    };
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

    void ShowSaveFailedAlert(Screen back)
    {
        ShowAlert(TR("alert.title", "Alert"),
            KeyFill(TR("alert.save_failed", "A replay could not be saved (the disk may be full or the file in use). It is kept in memory and saving "
            "is retried; saving again ({key_save}) retries at once. Keep the game open until it is saved.")),
            nullptr, CommandType::None, "", back);
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

    // The grey outline of the item under the mouse (not after a key).
    bool HoverShown(bool hovered) { return hovered && g_menuPointer.HoverShown(); }

    // A list item in its row box (RowBox: red ink when selected, grey ink under
    // the mouse once it moved), click selects; true on activation (double
    // click). `pointed` (a list that never scrolls under the mouse): set when
    // the mouse moved onto this row, which then takes the selection.
    bool ListItem(ImDrawList* draw, const char* id, float x, float y, float width, float height, bool selected, bool& clicked,
        bool* pointed = nullptr)
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
        RowBox(draw, x - 14.0f, y, x + width, y + height, selected || point ? RowLook::Selected : HoverShown(hovered) ? RowLook::Hovered : RowLook::Rest);
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

    void RefreshClipList(bool reread = true); // the gallery's window and sort kept (below)

    // ---------------- Home ----------------
    // Recorded Clips first (the most used), then New Project / Edit and Projects.
    // The selection is kept between visits; a clip closed from its editor comes
    // back on the Edit row.
    int g_homeIndex = 0; // Recorded Clips
    constexpr int kHomeEditRow = 1; // New Project, Edit Clip or Edit Project
    // The gallery adds a clip to the open project (A on the project's timeline)
    // instead of playing it.
    bool g_galleryPicking = false;
    std::uint32_t g_pickInsertAt = ~0u;
    ULONGLONG g_projectOpenAsked = 0; // a project asked for (new or open), the timeline waits for it

    void DrawHome(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("Home");
        // The pause menu's layout: a dark column on the left edge, the title on a
        // red brush, the items in light text with the selected one on the brush, and
        // a description panel beside them.
        const float columnWidth = 540.0f;
        // Solid under the items, then fading out into the game (no hard edge).
        draw->AddRectFilled(P(0.0f, 0.0f), P(columnWidth - 60.0f, g_vh), IM_COL32(10, 9, 8, 225));
        draw->AddRectFilledMultiColor(P(columnWidth - 60.0f, 0.0f), P(columnWidth + 220.0f, g_vh), IM_COL32(10, 9, 8, 225),
            IM_COL32(10, 9, 8, 0), IM_COL32(10, 9, 8, 0), IM_COL32(10, 9, 8, 225));
        const float top = std::max(60.0f, g_vh * 0.16f);
        TitleBanner(draw, 50.0f, top, 420.0f, 70.0f, 62.0f, 72.0f, top + 10.0f, 44.0f, "REDEMPTION EDITOR");
        OrnamentDivider(draw, 60.0f, top + 104.0f, 400.0f);

        // The items by what they do (a project adds Close Project).
        enum class Do
        {
            Edit,
            Projects,
            Clips,
            Record,
            SaveBuffer,
            Settings,
            CloseProject,
            Back
        };
        struct Item
        {
            Do what;
            const char* label;
            bool enabled;
            ui_art::Art icon = ui_art::Art::Count; // none
        };
        // With a clip loaded the first item edits it (the edit screen's Esc
        // comes back here), and Back to Game closes the clip.
        // With a project open it edits the project; without one and no clip, it
        // starts a new project.
        const bool clipOpen = Flag(SnapshotPlaying);
        const bool projectOpen = ProjectIsOpen();
        // The always-on replay buffer (RedemptionEditor.ini ReplayBuffer=1) has two
        // modes: the ring of the last seconds, saved on demand, and a manual
        // recording marked inside it.
        const bool buffer = Flag(SnapshotBufferRunning);
        const bool manual = Flag(SnapshotBufferManual);
        // A recording started with F4 (the buffer off) shows the item as a stop.
        const bool recordingAlone = !buffer && Flag(SnapshotRecording);
        std::vector<Item> items;
        items.push_back({Do::Clips, TR("home.recorded_clips", "Recorded Clips"), true, ui_art::Art::MenuClips});
        items.push_back({Do::Edit, projectOpen ? TR("home.edit_project", "Edit Project") : clipOpen ? TR("home.edit_clip", "Edit Clip") : TR("home.new_project", "New Project"), true,
            projectOpen || clipOpen ? ui_art::Art::Count : ui_art::Art::MenuNewProject});
        items.push_back({Do::Projects, TR("home.projects", "Projects"), true, ui_art::Art::MenuProjects});
        items.push_back({Do::Record, buffer ? (manual ? TR("home.stop_recording", "Stop Recording") : TR("home.start_recording", "Start Recording")) : recordingAlone ? TR("home.stop_recording", "Stop Recording") : TR("home.record_now", "Record Now"), true, ui_art::Art::MenuRecord});
        // Save Replay Buffer only while the buffer runs and holds something to
        // save (src/editor_ui_qa.h): hidden, Up and Down skip it and the items
        // below it move up.
        if (replay::editor::uiqa::SaveBufferShown(buffer, g_snapshot.bufferHeldMs))
            items.push_back({Do::SaveBuffer, TR("home.save_buffer", "Save Replay Buffer"), true, ui_art::Art::MenuSaveBuffer});
        items.push_back({Do::Settings, TR("home.replay_settings", "Replay Settings"), true, ui_art::Art::MenuSettings});
        if (projectOpen)
            items.push_back({Do::CloseProject, TR("home.close_project", "Close Project"), true});
        // Last, under the boxed rows: Back to Game (a plain line, not a box).
        items.push_back({Do::Back, TR("home.back_to_game", "Back to Game"), true});
        const int count = static_cast<int>(items.size());
        // The selection keeps its item when Close Project or Save Replay Buffer
        // comes or goes (the item after a vanished one takes its place).
        static std::vector<int> seenItems;
        std::vector<int> itemIds;
        for (const Item& item : items)
            itemIds.push_back(static_cast<int>(item.what));
        if (!seenItems.empty() && seenItems != itemIds)
            g_homeIndex = replay::editor::uiqa::KeepSelection(seenItems, g_homeIndex, itemIds);
        seenItems = itemIds;
        g_homeIndex = std::clamp(g_homeIndex, 0, count - 1);
        NavigateList(g_homeIndex, count);
        int activated = -1;
        const float itemsTop = top + 126.0f;
        // Painted, the items' icons in a column before the labels.
        const float iconLead = AnyArt({ui_art::Art::MenuNewProject, ui_art::Art::MenuProjects, ui_art::Art::MenuClips, ui_art::Art::MenuRecord,
                                   ui_art::Art::MenuSaveBuffer, ui_art::Art::MenuSettings}) ? 40.0f : 0.0f;
        // Every item but the last in its row box, as the pause menu's settings list.
        const int boxed = count - 1;
        for (int n = 0; n < boxed; ++n)
        {
            const float y = itemsTop + n * 50.0f;
            bool clicked = false, pointed = false;
            ImGui::PushID(n);
            const bool doubled = ListItem(draw, "##home", 80.0f, y, 380.0f, 44.0f, n == g_homeIndex, clicked, &pointed);
            ImGui::PopID();
            if (clicked || pointed)
                g_homeIndex = n;
            if (doubled || (clicked && items[n].enabled))
                activated = n;
            const ImU32 color = !items[n].enabled ? IM_COL32(236, 230, 218, 70) : kText;
            MenuIcon(draw, items[n].icon, 88.0f + 14.0f, y + 22.0f, 28.0f, n == g_homeIndex, items[n].enabled);
            Text(draw, 88.0f + iconLead, y + 8.0f, 30.0f, color, items[n].label);
        }
        {
            // Back to Game: a plain line below the boxes, set apart; selected,
            // a red ink stroke under it (grey under the mouse). Selected and
            // clicked as a row.
            const int n = boxed;
            const char* const label = items[n].label;
            const float size = 24.0f;
            const float x = 66.0f, y = itemsTop + boxed * 50.0f + 16.0f;
            const float width = std::min(394.0f, TextWidth(size, label) / g_scale);
            const float height = LabelHeight(size);
            ImGui::SetCursorScreenPos(P(x - 6.0f, y - 4.0f));
            ImGui::InvisibleButton("##homeback", ImVec2(S(width + 12.0f), S(height + 12.0f)));
            const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left) || (ImGui::IsItemHovered() && MissedClick());
            const bool pointed = !clicked && n != g_homeIndex && ImGui::IsItemHovered() && g_menuPointer.Moved();
            const bool hovered = HoverShown(ImGui::IsItemHovered());
            if ((clicked || pointed) && n != g_homeIndex)
                PlayUi(UiSound::NavDown);
            if (clicked || pointed)
                g_homeIndex = n;
            if (clicked)
                activated = n;
            const bool selected = n == g_homeIndex;
            if (selected || hovered)
                draw->AddRectFilled(P(x, y + height + 2.0f), P(x + width, y + height + 4.5f), selected ? kRed : kRowInkGrey);
            TextFit(draw, x, y, size, selected || hovered ? kText : IM_COL32(236, 230, 218, 190), label, 394.0f);
        }
        if (Confirmed())
            activated = g_homeIndex;

        // What the selected item does, beside the items.
        const char* heading = items[g_homeIndex].label;
        const char* body = "";
        char detail[160]{};
        switch (items[g_homeIndex].what)
        {
        case Do::Edit:
            if (projectOpen)
            {
                body = TR("home.edit_project.body", "Back to the open project: its clips in order, cut, moved, trimmed and played as one video.");
                char total[16]{};
                FormatClock(g_snapshot.projectTotalMs, total, sizeof(total), false);
                std::snprintf(detail, sizeof(detail), "%s", TRF("home.project.detail", "{name}  -  {count} clips  -  {length}{unsaved}",
                    {{"name", g_snapshot.projectName}, {"count", std::to_string(g_snapshot.projectClipCount)}, {"length", total},
                        {"unsaved", ProjectFlag(ProjectDirty) ? TR("home.project.unsaved", "  -  unsaved") : ""}}).c_str());
            }
            else
                body = clipOpen ? TR("home.edit_clip.body", "Back to the clip that is open: cameras, speed and the timeline.")
                                : TR("home.new_project.body", "Start a new project: put clips in order, cut and trim them, and play them as one video.");
            break;
        case Do::CloseProject:
            body = TR("home.close_project.body", "Close the project. Its changes are saved first; it stays in Projects.");
            break;
        case Do::Projects:
            body = TR("home.projects.body", "Open, rename or delete your projects. Each one is saved to the ReplayProjects folder.");
            std::snprintf(detail, sizeof(detail), "%s", TRF("home.projects.count", "{count} projects", {{"count", std::to_string(g_snapshot.projectListCount)}}).c_str());
            break;
        case Do::Clips:
        {
            body = TR("home.clips.body", "Browse, play and delete your recorded clips. Every recording is saved to the ReplayClips folder.");
            if (!ClipListKnown())
            {
                std::snprintf(detail, sizeof(detail), "%s", TR("home.clips.reading", "Reading clips..."));
                break;
            }
            std::uint64_t bytes = 0;
            bytes = g_snapshot.clipsDiskBytes; // the whole folder (the list is a window)
            char size[32]{};
            FormatSize(bytes, size, sizeof(size));
            std::snprintf(detail, sizeof(detail), "%s", TRF("home.clips.count", "{count} clips  -  {size} on disk",
                {{"count", std::to_string(g_snapshot.clipTotal)}, {"size", size}}).c_str());
            break;
        }
        case Do::Record:
            if (buffer)
                body = KeyFill(manual ? TR("home.record.mark_saves.body", "Save everything from the mark to now as a clip ({key_record}). The replay buffer keeps running.")
                                      : TR("home.record.mark.body", "Mark the start of a recording ({key_record}); the second {key_record} saves from the mark. Long recordings are saved in parts."));
            else if (recordingAlone)
                body = KeyFill(TR("home.record.stop.body", "Stop the recording and save it as a clip ({key_record})."));
            else
                body = KeyFill(clipOpen ? TR("home.record.close_clip.body", "Close the open clip and start recording now. Stop with {key_record}; long recordings are saved in parts.")
                                        : TR("home.record.now.body", "Start recording now. Stop with {key_record}; the clip is saved when it stops. Long recordings are saved in parts."));
            break;
        case Do::SaveBuffer:
            if (buffer)
            {
                body = KeyFill(TR("home.save_buffer.body", "Save what the replay buffer holds as a clip ({key_save_buffer}). Recording goes on in the background."));
                // The real window against the length set (busy places fill the memory
                // before it).
                const std::uint32_t held = g_snapshot.bufferHeldMs / 1000u;
                if (g_snapshot.bufferSeconds && held + 2u < g_snapshot.bufferSeconds)
                    std::snprintf(detail, sizeof(detail), "%s", TRF("home.buffer.holds_of", "The buffer holds {held} of {seconds} seconds (memory {mb} MB)",
                        {{"held", std::to_string(held)}, {"seconds", std::to_string(g_snapshot.bufferSeconds)},
                            {"mb", std::to_string(g_snapshot.bufferMB)}}).c_str());
                else
                    std::snprintf(detail, sizeof(detail), "%s", TRF("home.buffer.holds", "The buffer holds {held} seconds", {{"held", std::to_string(held)}}).c_str());
            }
            else
                body = TR("home.buffer_off.body", "The replay buffer is off. Turn it on in Replay Settings.");
            break;
        case Do::Settings:
            body = KeyFill(TR("home.settings.body", "The replay buffer: the game records all the time and you save the last seconds ({key_save_buffer}). Turn it on and set its length and memory."));
            std::snprintf(detail, sizeof(detail), "%s", TRF("home.settings.detail", "Buffer {state}  -  {seconds} s  -  {mb} MB",
                {{"state", g_snapshot.bufferEnabled ? TR("common.on", "on") : TR("common.off", "off")},
                    {"seconds", std::to_string(g_snapshot.bufferSeconds)}, {"mb", std::to_string(g_snapshot.bufferMB)}}).c_str());
            break;
        default: body = clipOpen ? TR("home.back.close_clip.body", "Close the clip and the editor and return to the game.") : TR("home.back.body", "Close the editor and return to the game."); break;
        }
        const float panelX = columnWidth + 30.0f;
        const float panelTop = itemsTop - 6.0f;
        DescriptionPanel(draw, panelX, panelTop, 200.0f, heading, body, detail, nullptr, // measured
            items[g_homeIndex].what == Do::Clips && !ClipListKnown());

        if (Back())
        {
            PlayUi(UiSound::Back);
            g_screen = clipOpen ? PlaybackScreen() : Screen::Closed;
            return;
        }
        if (activated >= 0 && !items[activated].enabled)
            PlayUi(UiSound::Error);
        if (activated < 0 || !items[activated].enabled)
            return;
        PlayUi(UiSound::Select);
        switch (items[activated].what)
        {
        case Do::Edit:
            if (projectOpen)
                g_screen = Screen::Project;
            else if (clipOpen)
                g_screen = Screen::Edit;
            else
            {
                // A new project, named "Project N" (R renames it in Projects).
                SendNamed(CommandType::ProjectNew, "");
                g_projectOpenAsked = GetTickCount64();
                g_screen = Screen::Project;
            }
            break;
        case Do::CloseProject:
            Send(CommandType::ProjectClose);
            break;
        case Do::Projects:
            Send(CommandType::RefreshProjects);
            g_screen = Screen::Projects;
            break;
        case Do::Clips:
            g_galleryPicking = false;
            RefreshClipList();
            g_screen = Screen::Gallery;
            break;
        case Do::Record:
            Send(CommandType::ToggleRecording);
            g_screen = Screen::Closed;
            break;
        case Do::SaveBuffer:
            Send(CommandType::SaveBuffer);
            g_screen = Screen::Closed;
            break;
        case Do::Settings:
            g_screen = Screen::Settings;
            break;
        case Do::Back:
            if (clipOpen)
                Send(CommandType::Stop);
            g_screen = Screen::Closed;
            break;
        default:
            break;
        }
    }

    // The clip's fades: the first marker's Fade In and the last marker's Fade
    // Out, a black layer over the picture during playback.
    void DrawClipFade(ImDrawList* draw)
    {
        if (!g_hasSnapshot || !Flag(SnapshotPlaying) || g_snapshot.cameraKeyframeCount < 2)
            return;
        const CameraKeyframe& first = g_snapshot.cameraKeyframes[0];
        const CameraKeyframe& last = g_snapshot.cameraKeyframes[g_snapshot.cameraKeyframeCount - 1];
        const double t = static_cast<double>(g_snapshot.playheadMs);
        float alpha = 0.0f;
        if (first.transition && first.transitionMs && t < first.timeMs + first.transitionMs)
            alpha = std::max(alpha, 1.0f - static_cast<float>((t - first.timeMs) / first.transitionMs));
        if (last.transition && last.transitionMs && t > static_cast<double>(last.timeMs) - last.transitionMs)
            alpha = std::max(alpha, static_cast<float>((t - (static_cast<double>(last.timeMs) - last.transitionMs)) / last.transitionMs));
        alpha = std::clamp(alpha, 0.0f, 1.0f);
        if (alpha > 0.0f)
            draw->AddRectFilled(ImVec2(0.0f, 0.0f), ImGui::GetIO().DisplaySize, IM_COL32(0, 0, 0, static_cast<int>(alpha * 255.0f)));
    }

    // ---------------- Recording notices ----------------
    // Shown when the replay is recording, when a clip is being saved and when it
    // was saved. Top right, small, in the pause menu's style; hidden while a clip
    // plays and in the pause menu.
    ULONGLONG g_toastUntil = 0;
    std::uint32_t g_toastClipMs = 0;

    // Opening a clip. The load is sent two frames after the gallery's Enter, so
    // the "OPENING CLIP" notice is on screen before the core's synchronous read;
    // with unsaved changes the core first waits for their save on its thread
    // (SnapshotOpeningClip) instead of freezing the game, and the notice says
    // "SAVING CHANGES".
    std::string g_loadName;
    int g_loadWaitFrames = 0;
    bool g_openingAsked = false;
    std::uint32_t g_openingSeq = 0;
    ULONGLONG g_openingAt = 0;
    // A load that saves the recording first says "SAVING RECORDING" instead of
    // "SAVING CHANGES".
    bool g_openingSavesRecording = false;

    void RequestLoad(const std::string& name)
    {
        g_loadName = name;
        g_loadWaitFrames = 1;
        g_openingAsked = true;
        g_openingSeq = g_snapshot.clipLoadSequence;
        g_openingAt = GetTickCount64();
        g_openingSavesRecording = false;
    }

    void SendRequestedLoad()
    {
        if (g_loadName.empty() || g_loadWaitFrames-- > 0)
            return;
        SendNamed(CommandType::LoadClip, g_loadName.c_str());
        g_loadName.clear();
    }

    bool OpeningVisible()
    {
        if (!g_loadName.empty() || (g_hasSnapshot && Flag(SnapshotOpeningClip)))
            return true;
        if (!g_openingAsked)
            return false;
        if (!g_hasSnapshot || g_snapshot.clipLoadSequence != g_openingSeq || Flag(SnapshotLoadFailed) ||
            GetTickCount64() - g_openingAt > 20000)
        {
            g_openingAsked = false;
            return false;
        }
        return true;
    }

    // ---------------- The recording indicator (src/rec_indicator.h) ----------------
    // Replay Settings' "Recording indicator": the mode the core keeps
    // (Snapshot::recordingIndicator) and the mode drawn this frame (hidden
    // shows as brief when its window cannot show over the game). In hidden
    // the recording notices go to the window of rec_indicator_window.h, which
    // screen capture leaves out, instead of the game frame.
    namespace recind = replay::recindicator;
    recind::BriefTimer g_recBrief;
    recind::Environment g_recEnv;
    recind::Choice g_recChoice;
    rec_window::Frame g_recRows; // this frame's rows for the window (DrawRecordingNotices)
#ifndef REDEMPTION_UI_HARNESS
    rec_window::Window g_recWindow;
    ULONGLONG g_recCheckedAt = 0;
    std::string g_recLogged;
#endif

    recind::Mode WantedIndicator() { return g_hasSnapshot ? recind::FromValue(g_snapshot.recordingIndicator) : recind::Mode::Normal; }

    recind::Kind IndicatorKind()
    {
        if (!g_hasSnapshot)
            return recind::Kind::None;
        return recind::KindOf(Flag(SnapshotBufferRunning), Flag(SnapshotBufferManual), g_snapshot.recordingMs);
    }

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
            g_recBrief.Update(now, IndicatorKind(), g_snapshot.recordingMs, g_snapshot.savedClipSequence);
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

    // Where a recording notice goes this frame.
    enum class NoticeSink
    {
        None,
        Frame,
        Window
    };

    struct RecordingRows
    {
        NoticeSink paused = NoticeSink::None;    // "Recording paused: ..."
        NoticeSink indicator = NoticeSink::None; // REC and its clock, or REPLAY
        NoticeSink saving = NoticeSink::None;    // SAVING CLIP or CLIP SAVED
        recind::Kind kind = recind::Kind::None;  // the indicator's row
        std::uint32_t ms = 0;                    // its clock
        bool live = true;                        // false: brief, a stopped recording's row (its clock held)
    };

    RecordingRows PlanRecordingRows()
    {
        RecordingRows rows;
        const recind::Mode mode = g_recChoice.mode;
        const NoticeSink sink = mode == recind::Mode::Hidden ? NoticeSink::Window : mode == recind::Mode::Off ? NoticeSink::None : NoticeSink::Frame;
        if (Flag(SnapshotRecording) && g_snapshot.recordPauseReasons)
            rows.paused = sink;
        if (mode == recind::Mode::Brief)
        {
            if (g_recBrief.Visible(GetTickCount64()))
            {
                rows.kind = g_recBrief.Shown();
                rows.live = !g_recBrief.Stopped();
                rows.ms = rows.live ? g_snapshot.recordingMs : g_recBrief.endedMs;
                rows.indicator = NoticeSink::Frame;
            }
        }
        else if (sink != NoticeSink::None)
        {
            rows.kind = IndicatorKind();
            rows.ms = g_snapshot.recordingMs;
            rows.indicator = sink;
        }
        if (Flag(SnapshotSavingClip) || GetTickCount64() < g_toastUntil)
            rows.saving = mode == recind::Mode::Hidden ? NoticeSink::Window : NoticeSink::Frame;
        return rows;
    }

    bool RecordingNoticeVisible()
    {
        if (OpeningVisible())
            return true;
        if (!g_hasSnapshot || Flag(SnapshotPlaying) || Flag(SnapshotGamePaused))
            return false;
        if (g_recChoice.mode == recind::Mode::Normal)
            return Flag(SnapshotRecording) || Flag(SnapshotSavingClip) || GetTickCount64() < g_toastUntil;
        const RecordingRows rows = PlanRecordingRows();
        return rows.paused != NoticeSink::None || (rows.indicator != NoticeSink::None && rows.kind != recind::Kind::None) ||
            rows.saving != NoticeSink::None;
    }

    // A text as the title font draws it (capitals), for the indicator's window.
    std::wstring WideCapitals(const char* text)
    {
        std::wstring out;
        for (const char* p = text; p && *p;)
        {
            std::uint32_t c = replay::i18n::ToUpper(replay::i18n::Decode(p));
            if (c >= 0x10000)
            {
                c -= 0x10000;
                out.push_back(static_cast<wchar_t>(0xD800 + (c >> 10)));
                out.push_back(static_cast<wchar_t>(0xDC00 + (c & 0x3FF)));
            }
            else
                out.push_back(static_cast<wchar_t>(c));
        }
        return out;
    }

    // `busy`: the value is a text spinner, drawn as the turning cylinder when
    // its art is there.
    void Notice(ImDrawList* draw, float y, ImU32 dot, bool pulse, const char* label, const char* value, float x = 40.0f, bool busy = false)
    {
        UI_PROBE_SCOPE("Notice");
        constexpr float kCue = 26.0f;
        const bool cue = busy && ui_art::Get(ui_art::Art::Loading);
        if (cue)
            value = "";
        // Wider when the label needs it ("Recording paused: ...").
        const float needed = 54.0f + TextWidth(22.0f, label) / g_scale +
            (value && value[0] ? TextWidth(22.0f, value) / g_scale + 28.0f : cue ? kCue + 28.0f : 0.0f);
        const float width = std::max(250.0f, needed);
        // x: bottom left by default; beside the edit panel there.
        InkPanel(draw, x, y, x + width, y + 40.0f, IM_COL32(10, 9, 8, 200), 6.0f); // a thin ink edge
        float alpha = 1.0f;
        if (pulse)
            alpha = 0.55f + 0.45f * std::sin(static_cast<float>(GetTickCount64() % 1200) / 1200.0f * 6.2832f);
        const ImU32 color = (dot & 0x00FFFFFFu) | (static_cast<ImU32>(alpha * 255.0f) << 24);
        draw->AddCircleFilled(P(x + 22.0f, y + 20.0f), S(7.0f), color, 20);
        Text(draw, x + 40.0f, y + 8.0f, 22.0f, kText, label);
        if (value && value[0])
            Text(draw, x + width - TextWidth(22.0f, value) / g_scale - 14.0f, y + 8.0f, 22.0f, kMuted, value);
        if (cue)
            LoadingCue(draw, x + width - 14.0f - kCue * 0.5f, y + 20.0f, kCue);
    }

    // One recording notice in the game frame or in the indicator's window
    // (each moves the stack up), or none.
    void RecordingRow(ImDrawList* draw, NoticeSink sink, float& y, ImU32 dot, bool pulse, const char* label, const char* value, bool busy = false)
    {
        if (sink == NoticeSink::Frame)
            Notice(draw, y, dot, pulse, label, value, 40.0f, busy);
        else if (sink == NoticeSink::Window)
        {
            rec_window::Row row;
            row.label = WideCapitals(label);
            row.value = busy ? std::wstring() : WideCapitals(value);
            row.spinner = busy;
            row.dot = ((dot & 0xFFu) << 16) | (dot & 0xFF00u) | ((dot >> 16) & 0xFFu); // IM_COL32 is 0xAABBGGRR
            row.pulse = pulse;
            row.y = y;
            g_recRows.x = 40.0f;
            g_recRows.uiScale = g_uiScaleFactor;
            g_recRows.rows.push_back(row);
        }
        else
            return;
        y -= 48.0f;
    }

    // ---------------- The loading cue ----------------
    // The visual identity's loading cue (the art brief:
    // "a small spinning emblem bottom right with a caps label"): the turning
    // cylinder after its caption on an ink plate, bottom right above the key
    // bar (and above the playback bar when that shows). It replaces the core's
    // plain game text "Loading replay..." (the core leaves it out while the
    // editor says it draws the cue, CommandType::SetLoadingCue) and the far
    // seek's dots, which stood alone at the right edge.

    // A seek waits on the world with a cue: the far jump's frozen picture, or a
    // near settle past seek_freeze's threshold (as its spinner was).
    bool SeekCueWanted()
    {
        if (!g_hasSnapshot || !Flag(SnapshotPlaying))
            return false;
        return g_snapshot.seekLoadState == SeekLoadFrozen ||
            (g_snapshot.seekLoadState == SeekLoadSettling && g_snapshot.seekLoadElapsedMs >= seek_freeze::kSettlingSpinnerMs);
    }

    // The playback bars' state "Loading" with its cylinder: the puppets built,
    // a far jump, a seek waiting (not the world's load behind the black: the
    // badge says that one).
    bool PlaybackBusy() { return g_hasSnapshot && (Flag(SnapshotPreparing) || SeekCueWanted()) && !Flag(SnapshotLoadingWorld); }

    // The playback bar the editor draws with the editor closed (DrawPlaybackHud).
    bool PlaybackBarShown() { return g_screen == Screen::Closed && Flag(SnapshotPlaying) && !Flag(SnapshotDebugMode) && !UiHidden(); }

    // The caption of the badge this frame, or nullptr. The world's load: on
    // every screen but the clip editor (its status line has the cue), and over
    // a hidden interface too (as the core's text was). A seek's wait: only
    // where no playback bar carries it (Debug=1's text HUD, the editor closed).
    const char* LoadingBadgeCaption()
    {
        if (!g_hasSnapshot)
            return nullptr;
        if (Flag(SnapshotLoadingWorld) && (g_screen != Screen::Edit || UiHidden()))
            return Flag(SnapshotPlaying) ? TR("hud.loading_replay", "Loading replay...") : TR("hud.loading_world", "Loading...");
        if (SeekCueWanted() && g_screen == Screen::Closed && Flag(SnapshotDebugMode) && !UiHidden())
            return TR("hud.loading", "Loading");
        return nullptr;
    }

    void DrawLoadingBadge(ImDrawList* draw)
    {
        const char* caption = LoadingBadgeCaption();
        if (!caption)
            return;
        UI_PROBE_SCOPE("LoadingBadge");
        // Screen units whatever stage the screen under it uses.
        const float ox = g_ox, oy = g_oy;
        g_ox = g_oy = 0.0f;
        constexpr float kCue = 30.0f, kH = 44.0f, kPad = 18.0f, kGap = 12.0f;
        const bool cue = ui_art::Get(ui_art::Art::Loading) != 0;
        const float labelW = TextWidth(22.0f, caption) / g_scale;
        const float width = kPad + labelW + (cue ? kGap + kCue : 0.0f) + kPad;
        const float x1 = g_vw - 40.0f;
        // Above the key bar; above the playback bar and its project line too.
        const float bottom = g_vh - g_footH - (PlaybackBarShown() ? 100.0f : 24.0f);
        const float x0 = x1 - width, y0 = bottom - kH;
        InkPanel(draw, x0, y0, x1, bottom, IM_COL32(10, 9, 8, 210), 6.0f);
        Text(draw, x0 + kPad, y0 + (kH - LabelHeight(22.0f)) * 0.5f, 22.0f, kText, caption);
        if (cue)
            LoadingCue(draw, x1 - kPad - kCue * 0.5f, y0 + kH * 0.5f, kCue);
        g_ox = ox;
        g_oy = oy;
    }

    void DrawRecordingNotices(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("RecordingNotices");
        if (!RecordingNoticeVisible())
            return;
        // Bottom left, stacked upwards, above the game's cores.
        float y = g_vh - 160.0f - g_footH; // above the key bar
        static const char* kSpinner[] = {"|", "/", "-", "\\"};
        const bool opening = OpeningVisible();
        if (opening)
        {
            Notice(draw, y, IM_COL32(236, 230, 218, 255), true, Flag(SnapshotSavingClip) ? (g_openingSavesRecording ? TR("notice.saving_recording", "SAVING RECORDING") : TR("notice.saving_changes", "SAVING CHANGES")) : TR("notice.opening_clip", "OPENING CLIP"),
                kSpinner[(GetTickCount64() / 150) % 4], 40.0f, true);
            y -= 48.0f;
            if (Flag(SnapshotPlaying) || Flag(SnapshotGamePaused))
                return; // the clip left still shows: only this notice
        }
        // The recording indicator's mode: each row in the frame, in the window
        // or not shown (normal: all in the frame).
        const RecordingRows rows = PlanRecordingRows();
        char clock[32]{};
        // The recording gate holds (cutscene, fade, loading).
        const std::uint32_t gate = g_snapshot.recordPauseReasons;
        if (rows.paused != NoticeSink::None)
        {
            const char* why = (gate & 1u) ? TR("notice.paused.menu", "menu") : (gate & 8u) ? TR("notice.paused.loading", "loading")
                : (gate & 4u) ? TR("notice.paused.fade", "fade") : TR("notice.paused.cutscene", "cutscene");
            const std::string label = TRF("notice.recording_paused", "Recording paused: {why}", {{"why", why}});
            RecordingRow(draw, rows.paused, y, IM_COL32(236, 190, 70, 255), false, label.c_str(), "");
        }
        if (rows.kind == recind::Kind::Rec)
        {
            FormatClock(rows.ms, clock, sizeof(clock), false);
            RecordingRow(draw, rows.indicator, y, IM_COL32(200, 30, 30, 255), rows.live, TR("notice.rec", "REC"), clock);
        }
        else if (rows.kind == recind::Kind::Replay)
        {
            // No seconds (the held window only wobbles between 60 and 61 once full):
            // the tag alone says the buffer runs.
            RecordingRow(draw, rows.indicator, y, IM_COL32(170, 60, 50, 255), false, TR("notice.replay", "REPLAY"), "");
        }
        if (rows.saving != NoticeSink::None && Flag(SnapshotSavingClip) && !opening)
        {
            static const char* kSpin[] = {"|", "/", "-", "\\"};
            RecordingRow(draw, rows.saving, y, IM_COL32(236, 230, 218, 255), true, TR("notice.saving_clip", "SAVING CLIP"),
                kSpin[(GetTickCount64() / 150) % 4], true);
        }
        else if (rows.saving != NoticeSink::None && GetTickCount64() < g_toastUntil)
        {
            FormatClock(g_toastClipMs, clock, sizeof(clock), false);
            RecordingRow(draw, rows.saving, y, IM_COL32(120, 170, 90, 255), false, TR("notice.clip_saved", "CLIP SAVED"), clock);
        }
    }

    // ---------------- World notice ----------------
    // The clip was recorded in another chapter or its area's map state
    // differs from the live world: a notice for 8 s once the clip shows
    // (after the load wait), top left, in the recording notices' style.
    std::uint32_t g_worldNoticeSeen = 0;
    bool g_worldNoticeInit = false;
    bool g_worldNoticeArmed = false;
    ULONGLONG g_worldNoticeUntil = 0;
    char g_worldNoticeText[512] = {};

    void UpdateWorldNotice()
    {
        if (!g_hasSnapshot)
            return;
        if (!g_worldNoticeInit)
        {
            g_worldNoticeInit = true;
            g_worldNoticeSeen = g_snapshot.worldNoticeSequence;
            return;
        }
        if (g_snapshot.worldNoticeSequence != g_worldNoticeSeen)
        {
            g_worldNoticeSeen = g_snapshot.worldNoticeSequence;
            g_worldNoticeArmed = g_snapshot.worldNoticeKind != replay::world::kNoticeNone &&
                replay::world::NoticeText(static_cast<std::uint8_t>(g_snapshot.worldChapterRecorded),
                    static_cast<std::uint8_t>(g_snapshot.worldChapterNow), g_snapshot.worldDifferences,
                    g_snapshot.worldNoticeKind == replay::world::kNoticeRestored, g_worldNoticeText,
                    sizeof(g_worldNoticeText));
        }
        if (!Flag(SnapshotPlaying))
        {
            g_worldNoticeArmed = false;
            g_worldNoticeUntil = 0;
            return;
        }
        if (g_worldNoticeArmed && !Flag(SnapshotPreparing))
        {
            g_worldNoticeArmed = false;
            g_worldNoticeUntil = GetTickCount64() + 8000;
        }
    }

    bool WorldNoticeVisible()
    {
        return g_hasSnapshot && Flag(SnapshotPlaying) && GetTickCount64() < g_worldNoticeUntil;
    }

    void DrawWorldNotice(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("WorldNotice");
        if (!WorldNoticeVisible())
            return;
        // Right of the edit panel while it shows (so it does not cover the panel's
        // title).
        const float x = g_screen == Screen::Edit && !UiHidden() ? 390.0f : 40.0f;
        Notice(draw, 40.0f, IM_COL32(236, 190, 70, 255), false, g_worldNoticeText, "", x);
    }

    // ---------------- QA notices ----------------
    // The core's notices are the game's objective text, hidden while a replay
    // plays, so the add-on confirms each mark (when its note box closes) and
    // says once when a playback stops with marks waiting. Top right, in the
    // recording notices' style; drawn with the UI hidden too (marking is a QA
    // action) and never during an export (the add-on draws after the export's
    // capture point anyway).
    char g_qaNoticeText[512] = {};
    ULONGLONG g_qaNoticeUntil = 0;
    replay::editor::uiqa::StopReminder g_qaStopReminder;

    void ShowQaNotice(const char* text, ULONGLONG ms)
    {
        std::snprintf(g_qaNoticeText, sizeof(g_qaNoticeText), "%s", text);
        g_qaNoticeUntil = GetTickCount64() + ms;
    }

    bool QaNoticeVisible() { return GetTickCount64() < g_qaNoticeUntil && !render_export::ExportController::Get().Busy(); }

    void ShowMarkNotice()
    {
        char text[512];
        replay::editor::uiqa::FormatMarkNotice(text, sizeof(text), qa_shots::PendingMarks(), Keys());
        ShowQaNotice(text, 6000);
    }

    void UpdateQaNotices(bool playing)
    {
        const std::uint32_t pending = qa_shots::PendingMarks();
        if (g_qaStopReminder.Update(GetTickCount64(), playing, pending))
        {
            char text[512];
            replay::editor::uiqa::FormatStopReminder(text, sizeof(text), pending, Keys());
            ShowQaNotice(text, 8000);
        }
    }

    void DrawQaNotice(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("QaNotice");
        if (!QaNoticeVisible())
            return;
        const float width = std::max(250.0f, 54.0f + TextWidth(22.0f, g_qaNoticeText) / g_scale);
        Notice(draw, 40.0f, IM_COL32(236, 190, 70, 255), false, g_qaNoticeText, "", std::max(40.0f, g_vw - 40.0f - width));
    }

    // ---------------- Crash notice (src/crash_notice.h) ----------------
    // On the first frame the core answers, the crash dumps of
    // %LOCALAPPDATA%\RedemptionEditor\crashes are listed; a crash of an earlier
    // session not told yet shows this notice (the editor closed: top centre, over
    // the game) for kShowMs, once: the newest dump's time goes to
    // crash-notice.txt as it shows. The report key keeps working under it.
    struct CrashNoticeState
    {
        bool checked = false;
        ULONGLONG until = 0;
        // NoticeText's inputs: the text is built when drawn, in the language
        // of that frame.
        replay::crashnotice::Decision decision;
        std::string reportsFolder;
        bool includeOpenClip = false;
    };
    CrashNoticeState g_crashNotice;

    bool CrashNoticeVisible() { return GetTickCount64() < g_crashNotice.until && !render_export::ExportController::Get().Busy(); }

    std::string Utf8(const wchar_t* text)
    {
        char out[MAX_PATH * 3]{};
        WideCharToMultiByte(CP_UTF8, 0, text, -1, out, static_cast<int>(sizeof(out)) - 1, nullptr, nullptr);
        return out;
    }

    void CheckCrashNotice()
    {
        if (g_crashNotice.checked || !g_hasSnapshot)
            return;
        g_crashNotice.checked = true;
        wchar_t local[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        if (length == 0 || length >= MAX_PATH - 64)
            return;
        const std::wstring root = std::wstring(local) + L"\\RedemptionEditor";
        std::vector<replay::crashnotice::Dump> dumps;
        WIN32_FIND_DATAW found{};
        HANDLE find = FindFirstFileW((root + L"\\crashes\\*.dmp").c_str(), &found);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    continue;
                replay::crashnotice::Dump dump;
                dump.name = Utf8(found.cFileName);
                dump.written = (static_cast<std::uint64_t>(found.ftLastWriteTime.dwHighDateTime) << 32) | found.ftLastWriteTime.dwLowDateTime;
                dumps.push_back(dump);
            } while (dumps.size() < 256 && FindNextFileW(find, &found));
            FindClose(find);
        }
        if (dumps.empty())
            return;
        const std::wstring ackPath = root + L"\\" + std::wstring(replay::crashnotice::kAckFileName,
            replay::crashnotice::kAckFileName + std::strlen(replay::crashnotice::kAckFileName));
        std::uint64_t acknowledged = 0;
        {
            HANDLE file = CreateFileW(ackPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                char text[64]{};
                DWORD read = 0;
                if (ReadFile(file, text, sizeof(text) - 1, &read, nullptr))
                    acknowledged = replay::crashnotice::ParseAck(std::string(text, read));
                CloseHandle(file);
            }
        }
        FILETIME nowTime{};
        GetSystemTimeAsFileTime(&nowTime);
        const std::uint64_t now = (static_cast<std::uint64_t>(nowTime.dwHighDateTime) << 32) | nowTime.dwLowDateTime;
        const replay::crashnotice::Decision decision = replay::crashnotice::Decide(dumps, GetCurrentProcessId(), acknowledged, now);
        if (!decision.show)
            return;
        wchar_t folder[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, folder);
        const std::wstring ini = replay::identity::IniPathInW(folder);
        const bool includeOpenClip = replay::identity::IniIntW(L"ReportIncludeClips", 0, ini.c_str()) != 0;
        g_crashNotice.decision = decision;
        g_crashNotice.reportsFolder = replay::storage::Utf8(replay::storage::Path(replay::storage::kReports)); // the base folder (storage_paths.h)
        g_crashNotice.includeOpenClip = includeOpenClip;
        g_crashNotice.until = GetTickCount64() + replay::crashnotice::kShowMs;
        // Told: the next start shows it only for a newer crash.
        CreateDirectoryW(root.c_str(), nullptr);
        HANDLE file = CreateFileW(ackPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            const std::string ack = replay::crashnotice::AckText(decision.newest);
            DWORD wrote = 0;
            WriteFile(file, ack.data(), static_cast<DWORD>(ack.size()), &wrote, nullptr);
            CloseHandle(file);
        }
    }

    // When no folder takes writes (storage_paths.h Layout::failed: the game
    // folder, Documents and %LOCALAPPDATA% all refused), the player is told
    // where saving failed and why, at the game start (the editor closed, top
    // centre) for kStorageNoticeMs, instead of finding no clips later.
    struct StorageNoticeState
    {
        bool checked = false;
        ULONGLONG until = 0;
        std::string gameDetail;     // "C:\\...\\Red Dead Redemption 2\\: denied (create error 5)"
        std::string fallbackDetail; // the Documents and %LOCALAPPDATA% refusals
    };
    StorageNoticeState g_storageNotice;
    constexpr ULONGLONG kStorageNoticeMs = 30000;

    bool StorageNoticeVisible() { return GetTickCount64() < g_storageNotice.until && !render_export::ExportController::Get().Busy(); }

    void CheckStorageNotice()
    {
        if (g_storageNotice.checked || !g_hasSnapshot)
            return;
        g_storageNotice.checked = true;
        const replay::storage::Layout& layout = replay::storage::Current();
        reshade::log::message(reshade::log::level::info, replay::storage::LogLine().c_str());
        if (!layout.failed)
            return;
        g_storageNotice.gameDetail = layout.gameDetail;
        g_storageNotice.fallbackDetail = layout.fallbackDetail;
        g_storageNotice.until = GetTickCount64() + kStorageNoticeMs;
    }

    void DrawStorageNotice(ImDrawList* draw, float y0)
    {
        UI_PROBE_SCOPE("StorageNotice");
        if (!StorageNoticeVisible())
            return;
        const float width = std::min(900.0f, g_vw - 80.0f);
        const float x0 = (g_vw - width) * 0.5f;
        const float pad = 30.0f;
        const float wrap = width - 2.0f * pad;
        const float bodyY = y0 + pad + 46.0f;
        const std::string title = TR("storage.title", "Redemption Editor cannot save anything");
        const std::string body = TRF("storage.body",
            "Windows refused every folder the editor saves in, so clips, projects, reports and changed settings are not kept. "
            "Game folder: {game}. Fallback folders: {fallback}. Give your Windows account write access to one of them, or move the game "
            "out of Program Files.",
            {{"game", g_storageNotice.gameDetail}, {"fallback", g_storageNotice.fallbackDetail}});
        const float bodyH = ParagraphHeight(20.0f, body.c_str(), wrap);
        InkPanel(draw, x0, y0, x0 + width, bodyY + bodyH + pad, IM_COL32(12, 11, 10, 235));
        Text(draw, x0 + pad, y0 + pad - 4.0f, 28.0f, kText, title.c_str());
        draw->AddLine(P(x0 + pad, y0 + pad + 34.0f), P(x0 + width - pad, y0 + pad + 34.0f), kRed, S(3.0f));
        Paragraph(draw, x0 + pad, bodyY, 20.0f, kText, body.c_str(), wrap);
    }

    void DrawCrashNotice(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("CrashNotice");
        if (!CrashNoticeVisible())
            return;
        const float width = std::min(900.0f, g_vw - 80.0f);
        const float x0 = (g_vw - width) * 0.5f;
        const float y0 = 110.0f;
        const float pad = 30.0f;
        const float wrap = width - 2.0f * pad;
        const float bodyY = y0 + pad + 46.0f;
        const replay::crashnotice::Text text =
            replay::crashnotice::NoticeText(g_crashNotice.decision, Keys(), g_crashNotice.reportsFolder, g_crashNotice.includeOpenClip);
        const float bodyH = ParagraphHeight(20.0f, text.body.c_str(), wrap);
        InkPanel(draw, x0, y0, x0 + width, bodyY + bodyH + pad, IM_COL32(12, 11, 10, 235));
        Text(draw, x0 + pad, y0 + pad - 4.0f, 28.0f, kText, text.title.c_str());
        draw->AddLine(P(x0 + pad, y0 + pad + 34.0f), P(x0 + width - pad, y0 + pad + 34.0f), kRed, S(3.0f));
        Paragraph(draw, x0 + pad, bodyY, 20.0f, kText, text.body.c_str(), wrap);
    }

    // ---------------- Replay Settings ----------------
    int g_settingsIndex = 0;
    // Replay Settings' list scroll, in units (only when it does not fit), and
    // the row it last brought into view.
    float g_settingsScroll = 0.0f;
    int g_settingsScrolledTo = -1;

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
        constexpr int kRows = 12;
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
        const char* rows[] = {row0.c_str(), row1.c_str(), row2.c_str(), row3.c_str(), row4.c_str(), row5.c_str(), row6.c_str(), row7.c_str(),
            row8.c_str(), row9.c_str(), row10.c_str(), row11.c_str()};
        const int count = kRows;
        // The rows in four groups under a section title each. g_settingsIndex
        // stays the row's own number (its key handling and help below); the
        // order is only the list's: Up and Down walk it, the titles are not
        // in it and cannot be selected.
        struct SettingsSection
        {
            const char* title;
            int first, end; // into kOrder
        };
        static const int kOrder[count] = {0, 1, 2, 7, 10, 3, 9, 8, 4, 5, 6, 11};
        const SettingsSection sections[kSections] = {{TR("settings.section.recording", "RECORDING"), 0, 5},
            {TR("settings.section.playback", "PLAYBACK"), 5, 8}, {TR("settings.section.storage", "STORAGE"), 8, 11},
            {TR("settings.section.interface", "INTERFACE"), 11, 12}};
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
                float at = 0.0f, from = 0.0f, to = 0.0f;
                for (const SettingsSection& section : sections)
                {
                    const float head = at;
                    at += kSectionHead;
                    for (int k = section.first; k < section.end; ++k, at += kRowPitch)
                        if (kOrder[k] == g_settingsIndex)
                        {
                            from = k == section.first ? head : at;
                            to = at + 44.0f;
                        }
                    at += kSectionGap;
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
                const std::string title = Ellipsized(18.0f, section.title, 372.0f);
                Text(draw, 88.0f, y, 18.0f, kGold, title.c_str());
                const float lineX = 88.0f + TextWidth(18.0f, title.c_str()) / g_scale + 12.0f;
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
                RowTextFit(draw, 88.0f, y + 8.0f, 30.0f, kText, rows[n], 372.0f, n == 10); // inside its row
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
        if (direction != 0 && g_settingsIndex == 11)
        {
            SetInterfaceScale(replay::editor::uiscale::Step(g_uiScalePercent, direction, wrap));
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
            : g_settingsIndex == 1 ? KeyFill(TR("settings.length.body", "How many seconds the buffer keeps. It holds less when the scene is heavy and the memory limit comes first. "
                                                                       "A manual recording ({key_record}) is not limited by it."))
            : g_settingsIndex == 2 ? KeyFill(TR("settings.memory.body", "The most the buffer may use; a busy town takes about 6 MB a second. A manual recording ({key_record}) "
                                                                       "is not limited by it: a long one is saved in parts (ManualMaxMB in RedemptionEditor.ini)."))
            : g_settingsIndex == 3 ? TR("settings.dead_eye.body", "Play the recorded Dead Eye and Eagle Eye: the game's screen effect and the slow motion. Off: the replay plays those moments at normal speed, without the effect.")
            : g_settingsIndex == 4 ? TR("settings.compress.body", "Clips not played nor saved for a while are compressed in the background, about 3 times smaller. They open as before; the first load takes a fraction of a second more.")
            : g_settingsIndex == 5 ? TR("settings.compress_after.body", "How long a clip stays untouched before it is compressed. Playing or saving a clip counts as a use.")
            : g_settingsIndex == 6 ? TR("settings.compress_now.body", "Compress now every clip older than the days above, without waiting. It runs in the background; the game keeps going.")
            : g_settingsIndex == 9 ? TR("settings.freeze.body", "On: a paused replay stands still like the photo mode (fire, smoke, water, cloth, wind, ambient sound). Off: the world goes on behind the paused replay.")
            : g_settingsIndex == 7 ? TR("settings.cutscenes.body", "Skip: the recording pauses while a cutscene plays and the clip cuts straight past it. Record: cutscenes are kept in the clip.")
            : g_settingsIndex == 11 ? TR("settings.interface_scale.body", "The size of the editor's menus, panels, texts and notices. 100% looks the same at every resolution; larger reads "
                                                                         "better from a distance, smaller leaves more of the picture. A 4:3 screen stays at 100% at most, and a small screen does not go below its 100% size.")
            : g_settingsIndex == 10 ? TR("settings.rec_indicator.body", "The REC notice while the replay records. Normal: always on screen. Brief: for 3 seconds when a recording starts, stops or saves. Hidden from capture: you see it, screen recorders (OBS, NVIDIA, Game Bar) do not. Off: no notice.")
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

    void ShowRename(const ClipInfo& clip); // below

    // ---------------- Gallery ----------------
    int g_galleryIndex = 0;
    int g_gallerySort = 0; // 0 date, 1 duration
    std::string g_compressingClip; // the clip C asked for
    std::uint64_t g_gallerySequence = 0;

    // The snapshot holds a window of 64 clips of the whole sorted list (the
    // folder can hold hundreds): the core sorts, the window follows the
    // selection.
    std::uint32_t g_requestedWindowFirst = ~0u;
    int g_requestedSort = -1;

    void RequestClipWindow(std::uint32_t first)
    {
        if (first == g_requestedWindowFirst && g_gallerySort == g_requestedSort)
            return;
        Command command{};
        command.timeMs = first;
        command.enabled = static_cast<std::uint32_t>(g_gallerySort);
        command.direction = g_requestedWindowFirst == ~0u ? 0 : 1; // 1: a page move, no re-read
        if (Send(CommandType::RefreshClips, &command))
        {
            g_requestedWindowFirst = first;
            g_requestedSort = g_gallerySort;
        }
    }

    // `reread` false (the home screen, which only shows the counts): the list the
    // core keeps (rebuilt after every save, delete, rename and compression), no
    // disk read. Reading the folder's hundreds of headers stalls the game's frame
    // right as the editor opens, and keys pressed during it would be lost. The
    // gallery still reads the folder when it opens (clips copied in by hand).
    void RefreshClipList(bool reread)
    {
        const std::uint32_t first = g_requestedWindowFirst == ~0u ? 0u : g_requestedWindowFirst;
        if (reread)
            g_requestedWindowFirst = ~0u;
        RequestClipWindow(first);
    }

    // The clip at a place of the whole list, when the window holds it.
    const ClipInfo* GalleryClip(int n)
    {
        if (g_snapshot.clipCount == 0 || n < 0)
            return nullptr;
        const std::uint32_t first = g_snapshot.clips[0].listIndex;
        const std::uint32_t at = static_cast<std::uint32_t>(n);
        return at >= first && at < first + g_snapshot.clipCount ? &g_snapshot.clips[at - first] : nullptr;
    }

    int g_projectIndex = 0; // the clip selected on the project's timeline

    void PlayClip(const ClipInfo& clip)
    {
        // Picking a clip for the project (A on its timeline).
        if (g_galleryPicking)
        {
            if (!clip.loadable)
            {
                PlayUi(UiSound::Error);
                ShowAlert(TR("alert.title", "Alert"), TR("alert.clip_version.add", "This clip was saved by another version of the replay and cannot be added."), nullptr,
                    CommandType::None, "", Screen::Gallery);
                return;
            }
            Command command{};
            std::snprintf(command.name, sizeof(command.name), "%s", clip.name);
            command.keyframeId = g_pickInsertAt;
            if (Send(CommandType::ProjectAddClip, &command))
            {
                PlayUi(UiSound::Select);
                if (g_pickInsertAt != ~0u)
                    g_projectIndex = static_cast<int>(g_pickInsertAt);
                else
                    g_projectIndex = static_cast<int>(g_snapshot.projectClipCount);
            }
            g_galleryPicking = false;
            g_screen = Screen::Project;
            return;
        }
        if (!clip.loadable)
        {
            PlayUi(UiSound::Error);
            ShowAlert(TR("alert.title", "Alert"), TR("alert.clip_version.open", "This clip was saved by another version of the replay and cannot be opened."), nullptr,
                CommandType::None, "", Screen::Gallery);
            return;
        }
        // Enter on the clip already open goes back to the Edit screen; no reload
        // from disk, so the undo history and the playhead stay.
        if (Flag(SnapshotPlaying) && g_snapshot.currentClip[0] && std::strcmp(clip.name, g_snapshot.currentClip) == 0)
        {
            PlayUi(UiSound::Select);
            g_screen = Screen::Edit;
            return;
        }
        // A recording in progress (F4 alone, or the F4 mark inside the replay
        // buffer) is saved as a clip by the core first, through the save queue, and
        // the chosen clip opens once that save is written.
        const bool savesRecording = Flag(SnapshotRecording) && (!Flag(SnapshotBufferRunning) || Flag(SnapshotBufferManual));
        PlayUi(UiSound::Select);
        RequestLoad(clip.name); // after the notice is on screen
        g_openingSavesRecording = savesRecording;
        g_screen = Screen::Closed;
    }

    // ---------------- Export (render export) ----------------
    // The export dialog: format, frame rate, range, motion blur, look, audio;
    // Enter starts, A adds to the queue; while it runs: progress, time left, the
    // stage timings, Cancel. The UI is drawn after the capture point, so it never
    // reaches the video.
    namespace exportui
    {
        namespace ec = replay::exportchannel;
        using render_export::ExportController;

        // A choice's text: TR(key, label) where it is drawn.
        struct Choice
        {
            const char* key;
            const char* label;
            float value;
        };
        const ec::VideoPreset kPresets[] = {ec::VideoPreset::H264Nvenc, ec::VideoPreset::HevcNvenc, ec::VideoPreset::Av1Nvenc,
            ec::VideoPreset::PreviewH264, ec::VideoPreset::ProRes422Hq, ec::VideoPreset::DnxhrHqx, ec::VideoPreset::LosslessFfv1,
            ec::VideoPreset::LosslessNvencHevc, ec::VideoPreset::PngSequence};
        const replay::exporting::FrameRate kRates[] = {{24, 1}, {25, 1}, {30, 1}, {50, 1}, {60, 1}, {120, 1}, {30000, 1001}, {60000, 1001}};
        const char* const kRateLabels[] = {"24", "25", "30", "50", "60", "120", "29.97", "59.94"};
        const std::uint32_t kSamples[] = {1, 8, 16, 32};
        const float kShutters[] = {90.0f, 180.0f, 270.0f, 360.0f};
        const Choice kLetterbox[] = {{TR_NOOP("common.off_cap", "Off"), 0.0f}, {"", "2.39:1", 2.39f}, {"", "2.00:1", 2.0f}, {"", "1.85:1", 1.85f}};
        const Choice kGrain[] = {{TR_NOOP("common.off_cap", "Off"), 0.0f}, {TR_NOOP("export.grain.light", "Light"), 1.5f},
            {TR_NOOP("export.grain.medium", "Medium"), 3.0f}, {TR_NOOP("export.grain.heavy", "Heavy"), 5.0f}};

        int g_index = 0;
        int g_preset = 0;
        int g_rate = 4;
        int g_range = 1; // 0 whole clip, 1 start to end markers, 2 whole project (a project with clips open)
        Screen g_back = Screen::Edit; // where Esc goes (the project screen opens it on the whole project)
        int g_samples = 0;
        int g_shutter = 1;
        int g_letterbox = 0;
        int g_grain = 0;
        bool g_lut = false;
        bool g_audio = true;
        bool g_chapters = true;
        std::wstring g_lutPath; // [Export] Lut
        bool g_configLoaded = false;

        struct Pending
        {
            ec::Settings settings;
            std::vector<render_export::Chapter> chapters;
            std::string label;
            bool project = false;            // the whole project (StartExport with enabled 1)
            std::vector<std::string> titles; // its clips, the chapters' names
        };
        std::vector<Pending> g_queue;
        std::string g_lastLabel;
        std::uint64_t g_requestedAt = 0;
        std::string g_note;
        std::uint64_t g_noteUntil = 0;

        bool TrimRange(std::uint64_t& start, std::uint64_t& end)
        {
            const std::uint32_t count = g_snapshot.cameraKeyframeCount;
            if (count < 2)
                return false;
            start = g_snapshot.cameraKeyframes[0].timeMs;
            end = g_snapshot.cameraKeyframes[count - 1].timeMs;
            return end > start;
        }

        // The open project has clips: the "Whole project" range is offered.
        bool ProjectRange() { return ProjectIsOpen() && g_snapshot.projectClipCount > 0; }

        // A project clip whose file is gone (the export is refused before it starts).
        int MissingProjectClip()
        {
            const std::uint32_t count = std::min(g_snapshot.projectClipCount, kMaxProjectClipsInSnapshot);
            for (std::uint32_t n = 0; n < count; ++n)
                if (g_snapshot.projectClips[n].flags & ProjectClipMissing)
                    return static_cast<int>(n);
            return -1;
        }

        void Range(std::uint64_t& start, std::uint64_t& end)
        {
            if (g_range == 2 && !ProjectRange())
                g_range = 0;
            else if (g_range != 2 && !Flag(SnapshotPlaying) && ProjectRange())
                g_range = 2; // no clip open: the project is what there is to export
            start = 0;
            end = g_range == 2 ? g_snapshot.projectTotalMs : g_snapshot.durationMs;
            if (g_range == 1)
                TrimRange(start, end);
        }

        // The range row's next value: whole clip, start to end markers (when the
        // clip has them), whole project (when a project with clips is open);
        // without a clip open only the project.
        int NextRange(int range, int direction)
        {
            std::uint64_t a = 0, b = 0;
            const bool clip = Flag(SnapshotPlaying);
            const bool available[3] = {clip, clip && TrimRange(a, b), ProjectRange()};
            for (int k = 1; k <= 3; ++k)
            {
                const int candidate = ((range + direction * k) % 3 + 3) % 3;
                if (available[candidate])
                    return candidate;
            }
            return 0;
        }

        std::wstring Widen(const char* text)
        {
            wchar_t out[256]{};
            MultiByteToWideChar(CP_UTF8, 0, text, -1, out, 255);
            return out;
        }

        Pending Build()
        {
            Pending pending;
            ec::Settings& s = pending.settings;
            Range(s.startMs, s.endMs);
            s.rate = kRates[g_rate];
            s.samples = kSamples[g_samples];
            s.shutterDegrees = kShutters[g_shutter];
            s.prerollFrames = 30;
            s.preset = kPresets[g_preset];
            s.audio = g_audio && s.preset != ec::VideoPreset::PngSequence ? ec::AudioMode::RealtimePass : ec::AudioMode::None;
            s.letterboxAspect = kLetterbox[g_letterbox].value;
            s.grain = kGrain[g_grain].value;
            s.thumbnail = 1;
            s.chapters = g_chapters ? 1u : 0u;
            if (g_lut && !g_lutPath.empty())
                wcsncpy_s(s.lutPath, g_lutPath.c_str(), _TRUNCATE);
            std::wstring clip = g_snapshot.currentClip[0] ? Widen(g_snapshot.currentClip) : L"replay-buffer";
            const std::size_t dot = clip.rfind(L".rpclip");
            if (dot != std::wstring::npos)
                clip = clip.substr(0, dot);
            pending.project = g_range == 2;
            if (pending.project)
            {
                // Named after the project; one chapter per clip (the add-on's).
                clip = g_snapshot.projectName[0] ? Widen(g_snapshot.projectName) : L"project";
                const std::uint32_t count = std::min(g_snapshot.projectClipCount, kMaxProjectClipsInSnapshot);
                for (std::uint32_t n = 0; n < count; ++n)
                    pending.titles.emplace_back(g_snapshot.projectClips[n].file, strnlen(g_snapshot.projectClips[n].file, sizeof(g_snapshot.projectClips[n].file)));
            }
            SYSTEMTIME now{};
            GetLocalTime(&now);
            const std::wstring path = render_export::DefaultOutputPath(ExportController::Get().Folder(), clip, s.preset, now.wYear, now.wMonth,
                now.wDay, now.wHour, now.wMinute, now.wSecond);
            wcsncpy_s(s.outputPath, path.c_str(), _TRUNCATE);
            // Markers inside the range become chapters ("Start" at 0).
            pending.chapters.push_back({0, "Start"});
            for (std::uint32_t k = 0; k < g_snapshot.cameraKeyframeCount && !pending.project; ++k)
            {
                const std::uint64_t t = g_snapshot.cameraKeyframes[k].timeMs;
                if (t > s.startMs + 500 && t + 500 < s.endMs)
                    pending.chapters.push_back({t - s.startMs, "Marker " + std::to_string(k + 1)});
            }
            const render_export::PresetInfo info = render_export::InfoOf(s.preset);
            pending.label = TRF("export.job", "{format}, {rate} fps{blur}", {{"format", TR(info.labelKey, info.label)}, {"rate", kRateLabels[g_rate]},
                {"blur", s.samples > 1 ? TR("export.job.blur", ", motion blur") : ""}});
            const std::string backend = ExportController::Get().BackendLabel(s.preset);
            if (!backend.empty())
                pending.label += " [" + backend + "]";
            return pending;
        }

        // Starts the next queued export when the controller is free.
        void Service()
        {
            ExportController& controller = ExportController::Get();
            // A project export opens its clips itself; a clip range needs its clip.
            if (g_queue.empty() || controller.Busy() || (!g_queue.front().project && !Flag(SnapshotPlaying)))
                return;
            const std::uint64_t now = GetTickCount64();
            if (now < g_requestedAt + 1500)
                return; // the probe's state follows a command a tick later
            Pending next = g_queue.front();
            std::uint32_t requestSeq = 0;
            if (next.project ? !controller.RequestProjectExport(next.settings, next.titles, requestSeq)
                             : !controller.RequestExport(next.settings, next.chapters, requestSeq))
                return;
            g_queue.erase(g_queue.begin());
            Command command{};
            command.keyframeId = requestSeq;
            command.enabled = next.project ? 1u : 0u; // 1: the whole project, one clip after the other
            Send(CommandType::StartExport, &command);
            g_lastLabel = next.label;
            g_requestedAt = now;
        }

        // Where the motion blur will take fewer samples (the markers' slow motion, or
        // the engine's minimum step at full speed), told before the export starts.
        // The probe decides per frame with the game's own values; this is the same
        // rule on the markers and the playback speed (a recorded Dead Eye or kill cam
        // slow motion is not in the snapshot: the export log names those spans).
        std::vector<replay::exporting::BlurSpan> BlurPlan(std::uint64_t startMs, std::uint64_t endMs)
        {
            std::vector<replay::exporting::MarkerSpeed> markers;
            if (Flag(SnapshotCameraTrackEnabled))
            {
                for (std::uint32_t k = 0; k < g_snapshot.cameraKeyframeCount; ++k)
                {
                    const CameraKeyframe& key = g_snapshot.cameraKeyframes[k];
                    if (key.markerType == MarkerType::Anchor)
                        continue;
                    const std::uint32_t index = std::min<std::uint32_t>(key.speedIndex, kMarkerSpeedCount - 1);
                    markers.push_back({key.timeMs, static_cast<double>(kMarkerSpeedPercent[index]) / 100.0});
                }
            }
            const double global = g_snapshot.speedPercent ? static_cast<double>(g_snapshot.speedPercent) / 100.0 : 1.0;
            const std::vector<replay::exporting::SpeedSpan> spans = replay::exporting::SpeedSpansFromMarkers(markers, startMs, endMs, global);
            return replay::exporting::PlanBlurSpans(kRates[g_rate], {kSamples[g_samples], kShutters[g_shutter]}, spans,
                replay::exporting::kEngineMinStepS);
        }

        // A whole project's progress: the pieces done and the current one's
        // frames against the whole (between two pieces the next one's count is 0).
        double ProjectFraction(const render_export::ExportStatus& status)
        {
            return replay::projectexport::OverallFraction(status.projectFramesBefore,
                status.state == ec::State::SegmentDone ? 0 : status.framesEncoded, status.projectFramesTotal);
        }

        // "Segment 2 of 3: frame 120 of 480" ("Segment 2 of 3: opening the clip..." between pieces).
        std::string ProjectProgressText(const render_export::ExportStatus& status)
        {
            const bool between = status.state == ec::State::SegmentDone;
            // Between two pieces the probe names the one opening.
            const std::uint32_t shown = std::min(status.segmentIndex, status.segmentCount - 1) + 1;
            if (between)
                return TRF("export.segment_opening", "Segment {n} of {count}: opening the clip...",
                    {{"n", std::to_string(shown)}, {"count", std::to_string(status.segmentCount)}});
            return TRF("export.segment_of", "Segment {n} of {count}: frame {done} of {total}",
                {{"n", std::to_string(shown)}, {"count", std::to_string(status.segmentCount)}, {"done", std::to_string(status.framesEncoded)},
                    {"total", std::to_string(status.framesTotalHint)}});
        }

        const char* StateText(ec::State state)
        {
            switch (state)
            {
            case ec::State::Requested: return TR("export.state.requested", "Preparing the scene...");
            case ec::State::Arming: return TR("export.state.arming", "Starting the encoder...");
            case ec::State::Preroll: return TR("export.state.preroll", "Warming up the world...");
            case ec::State::Capture: return TR("export.state.capture", "Exporting video");
            case ec::State::Flush: return TR("export.state.flush", "Last frames...");
            case ec::State::Finishing: return TR("export.state.finishing", "Finishing the file...");
            case ec::State::AudioSeek: return TR("export.state.audio_seek", "Audio pass: seeking...");
            case ec::State::AudioPass: return TR("export.state.audio_pass", "Audio pass (real time)");
            case ec::State::AudioDone: return TR("export.state.audio_done", "Muxing audio...");
            case ec::State::SegmentDone: return TR("export.state.segment_done", "Next clip of the project...");
            case ec::State::Done: return TR("export.state.done", "Done");
            case ec::State::Failed: return TR("export.state.failed", "Failed");
            case ec::State::Cancelled: return TR("export.state.cancelled", "Cancelled");
            default: return TR("export.state.ready", "Ready");
            }
        }
    }

    void DrawExport(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("Export");
        using namespace exportui;
        ExportController& controller = ExportController::Get();
        if (!g_configLoaded)
        {
            g_configLoaded = true;
            wchar_t folder[MAX_PATH]{};
            GetCurrentDirectoryW(MAX_PATH, folder);
            const std::wstring ini = replay::identity::IniPathInW(folder);
            wchar_t lut[MAX_PATH]{};
            replay::identity::ProfileStringW(L"Export", L"Lut", L"", lut, MAX_PATH, ini.c_str());
            g_lutPath = lut;
        }
        const float columnWidth = 600.0f;
        draw->AddRectFilled(P(0.0f, 0.0f), P(columnWidth - 60.0f, g_vh), IM_COL32(10, 9, 8, 225));
        draw->AddRectFilledMultiColor(P(columnWidth - 60.0f, 0.0f), P(columnWidth + 220.0f, g_vh), IM_COL32(10, 9, 8, 225),
            IM_COL32(10, 9, 8, 0), IM_COL32(10, 9, 8, 0), IM_COL32(10, 9, 8, 225));
        const float top = std::max(50.0f, g_vh * 0.10f);
        const char* const title = TR("export.title", "EXPORT VIDEO");
        TitleBanner(draw, 50.0f, top, 330.0f, 70.0f, 62.0f, 72.0f, top + 10.0f, 44.0f, title); // as wide as a long title
        OrnamentDivider(draw, 60.0f, top + 100.0f, 460.0f);

        const render_export::ExportStatus status = controller.Status();
        const bool running = controller.Busy();
        if (!controller.Enabled() || !status.channel)
        {
            Paragraph(draw, 80.0f, top + 130.0f, 22.0f, kText,
                TR("export.off", "Video export is off. Set [Export] Enabled=1 in RedemptionEditor.ini, then restart the game."),
                440.0f);
            if (Back())
            {
                PlayUi(UiSound::Back);
                g_screen = g_back;
            }
            return;
        }

        if (running)
        {
            const float y = top + 130.0f;
            Text(draw, 80.0f, y, 30.0f, kText, StateText(status.state));
            LoadingAfter(draw, 80.0f, y, 30.0f, StateText(status.state), 520.0f);
            TextFit(draw, 80.0f, y + 44.0f, 20.0f, kMuted, g_lastLabel.c_str(), 440.0f);
            const double total = static_cast<double>(std::max<std::uint64_t>(status.framesTotalHint, 1));
            float fraction = static_cast<float>(std::min(1.0, static_cast<double>(status.framesEncoded) / total));
            const bool project = status.segmentCount > 0;
            float barWidth = 420.0f;
            if (project)
            {
                // A whole project: the bar and its percent (after it) are the whole's.
                fraction = static_cast<float>(ProjectFraction(status));
                barWidth = 350.0f;
                Text(draw, 444.0f, y + 80.0f, 20.0f, kGold, (std::to_string(replay::projectexport::PercentOf(fraction)) + "%").c_str());
            }
            draw->AddRectFilled(P(80.0f, y + 84.0f), P(80.0f + barWidth, y + 100.0f), kTrack);
            draw->AddRectFilled(P(80.0f, y + 84.0f), P(80.0f + barWidth * fraction, y + 100.0f), kRed);
            char line[160];
            if (project)
                TextFit(draw, 80.0f, y + 112.0f, 22.0f, kText, ProjectProgressText(status).c_str(), 440.0f);
            else
                Text(draw, 80.0f, y + 112.0f, 22.0f, kText, TRF("export.frame_of", "Frame {done} of {total}",
                    {{"done", std::to_string(status.framesEncoded)}, {"total", std::to_string(status.framesTotalHint)}}).c_str());
            char elapsed[24]{}, left[24]{};
            FormatClock(static_cast<std::uint64_t>(status.secondsElapsed * 1000.0), elapsed, sizeof(elapsed), false);
            if (status.secondsLeft >= 0.0)
                FormatClock(static_cast<std::uint64_t>(status.secondsLeft * 1000.0), left, sizeof(left), false);
            Text(draw, 80.0f, y + 144.0f, 22.0f, kText, TRF("export.elapsed", "Elapsed {elapsed}   Time left {left}",
                {{"elapsed", elapsed}, {"left", status.secondsLeft >= 0.0 ? left : "--:--"}}).c_str());
            // The stage timings are for developers (Debug=1 in RedemptionEditor.ini);
            // the product shows progress only.
            if (g_hasSnapshot && Flag(SnapshotDebugMode))
            {
                const render_export::GpuTimings& t = status.timings;
                std::snprintf(line, sizeof(line), "GPU copy %.2f  blur %.2f  convert %.2f  readback %.2f ms", t.copyUs / 1000.0,
                    t.accumulateUs / 1000.0, t.resolveUs / 1000.0, t.readbackUs / 1000.0);
                Text(draw, 80.0f, y + 184.0f, 18.0f, kMuted, line);
                std::snprintf(line, sizeof(line), "CPU map %.2f ms   encoder queue %u   latency %d", t.mapUs / 1000.0, status.queueDepth,
                    status.latencyPresents == ~0u ? -1 : static_cast<int>(status.latencyPresents));
                Text(draw, 80.0f, y + 210.0f, 18.0f, kMuted, line);
            }
            if (!g_queue.empty())
            {
                Text(draw, 80.0f, y + 240.0f, 18.0f, kGold, TRF("export.queue_more", "{count} more in the queue", {{"count", std::to_string(g_queue.size())}}).c_str());
            }
            ImGui::SetCursorScreenPos(P(80.0f, y + 280.0f));
            const bool cancelClicked = ItemClicked(ImGui::InvisibleButton("##exportcancel", ImVec2(S(220.0f), S(36.0f))));
            Brush(draw, 72.0f, y + 280.0f, 220.0f, 36.0f, ImGui::IsItemHovered() ? kRed : kRedDim);
            Text(draw, 84.0f, y + 286.0f, 20.0f, kText, TR("export.cancel", "Cancel export"));
            if (cancelClicked || KeyPressed(ImGuiKey_Delete, false))
            {
                PlayUi(UiSound::Back);
                g_queue.clear();
                controller.Cancel();
                Send(CommandType::CancelExport);
            }
            // Its keys are on the key bar (ExportTips).
            return;
        }

        // Settings. The encoders of this PC are probed once (FFmpeg DLLs
        // loaded, every variant opened); the formats without one are skipped.
        controller.StartProbe();
        const bool probed = controller.ProbeDone();
        const int presetCount = static_cast<int>(std::size(kPresets));
        if (probed && !controller.PresetAvailable(kPresets[g_preset]))
        {
            for (int k = 1; k < presetCount; ++k)
            {
                const int candidate = (g_preset + k) % presetCount;
                if (controller.PresetAvailable(kPresets[candidate]))
                {
                    g_preset = candidate;
                    break;
                }
            }
        }
        const render_export::PresetInfo info = render_export::InfoOf(kPresets[g_preset]);
        const std::string backend = controller.BackendLabel(kPresets[g_preset]);
        const bool presetUsable = !probed || controller.PresetAvailable(kPresets[g_preset]);
        std::uint64_t startMs = 0, endMs = 0;
        Range(startMs, endMs);
        std::string rows[12];
        const char* formatLabel = TR(info.labelKey, info.label);
        if (!probed)
            rows[0] = TRF("export.format.checking", "Format: {format} (checking encoders...)", {{"format", formatLabel}});
        else if (!backend.empty())
            rows[0] = TRF("export.format.backend", "Format: {format} ({backend})", {{"format", formatLabel}, {"backend", backend}});
        else
            rows[0] = TRF("export.format", "Format: {format}", {{"format", formatLabel}});
        rows[1] = TRF("export.rate", "Frame rate: {rate} fps", {{"rate", kRateLabels[g_rate]}});
        {
            char a[24]{}, b[24]{};
            FormatClock(startMs, a, sizeof(a), true);
            FormatClock(endMs, b, sizeof(b), true);
            rows[2] = TRF("export.range", "Range: {range} ({start} - {end})",
                {{"range", g_range == 2 ? TR("export.range.project", "Whole project")
                        : g_range == 1  ? TR("export.range.markers", "Start to End markers")
                                        : TR("export.range.whole", "Whole clip")},
                    {"start", a}, {"end", b}});
        }
        if (kSamples[g_samples] > 1)
            rows[3] = TRF("export.blur", "Motion blur: {samples} samples", {{"samples", std::to_string(kSamples[g_samples])}});
        else
            rows[3] = TR("export.blur.off", "Motion blur: Off");
        rows[4] = TRF("export.shutter", "Shutter angle: {angle} deg", {{"angle", replay::i18n::Printf("%.0f", kShutters[g_shutter])}});
        rows[5] = TRF("export.letterbox", "Letterbox: {value}", {{"value", TR(kLetterbox[g_letterbox].key, kLetterbox[g_letterbox].label)}});
        rows[6] = TRF("export.grain", "Film grain: {value}", {{"value", TR(kGrain[g_grain].key, kGrain[g_grain].label)}});
        rows[7] = TRF("export.lut", "LUT: {value}", {{"value", g_lut && !g_lutPath.empty() ? TR("common.on_cap", "On")
                    : g_lutPath.empty() ? TR("export.lut.none", "None ([Export] Lut)") : TR("common.off_cap", "Off")}});
        rows[8] = TRF("export.audio", "Audio: {value}", {{"value", g_audio ? TR("export.audio.pass", "Real-time pass") : TR("export.audio.none", "None")}});
        rows[9] = TRF("export.chapters", "Markers as chapters: {value}", {{"value", g_chapters ? TR("common.on_cap", "On") : TR("common.off_cap", "Off")}});
        rows[10] = TR("export.now", "Export now");
        rows[11] = TRF("export.add_queue", "Add to queue ({count})", {{"count", std::to_string(g_queue.size())}});
        const int count = 12;
        NavigateList(g_index, count);
        int direction = 0;
        if (KeyPressed(ImGuiKey_RightArrow))
            direction = 1;
        if (KeyPressed(ImGuiKey_LeftArrow))
            direction = -1;
        bool activate = Confirmed();
        const float itemsTop = top + 116.0f;
        for (int n = 0; n < count; ++n)
        {
            const float y = itemsTop + n * 42.0f;
            bool clicked = false, pointed = false;
            ImGui::PushID(n);
            ListItem(draw, "##export", 80.0f, y, 440.0f, 38.0f, n == g_index, clicked, &pointed);
            ImGui::PopID();
            if (pointed)
                g_index = n;
            if (clicked)
            {
                g_index = n;
                if (n < 10)
                    direction = 1;
                else
                    activate = true;
            }
            RowTextFit(draw, 88.0f, y + 6.0f, 26.0f, n >= 10 ? kGold : kText, rows[n].c_str(), 424.0f); // inside its row
        }
        if (activate && g_index < 10)
        {
            direction = 1;
            activate = false;
        }
        auto cycle = [direction](int value, int size) { return (value + direction + size) % size; };
        if (direction != 0)
        {
            PlayUi(direction < 0 ? UiSound::NavLeft : UiSound::NavRight);
            switch (g_index)
            {
            case 0:
                // Only the formats with an encoder on this PC.
                for (int k = 0; k < presetCount; ++k)
                {
                    g_preset = cycle(g_preset, presetCount);
                    if (controller.PresetAvailable(kPresets[g_preset]))
                        break;
                }
                break;
            case 1: g_rate = cycle(g_rate, static_cast<int>(std::size(kRates))); break;
            case 2: g_range = NextRange(g_range, direction); break;
            case 3: g_samples = cycle(g_samples, static_cast<int>(std::size(kSamples))); break;
            case 4: g_shutter = cycle(g_shutter, static_cast<int>(std::size(kShutters))); break;
            case 5: g_letterbox = cycle(g_letterbox, static_cast<int>(std::size(kLetterbox))); break;
            case 6: g_grain = cycle(g_grain, static_cast<int>(std::size(kGrain))); break;
            case 7: g_lut = !g_lut && !g_lutPath.empty(); break;
            case 8: g_audio = !g_audio; break;
            case 9: g_chapters = !g_chapters; break;
            default: break;
            }
        }
        const bool addKey = KeyPressed(ImGuiKey_A, false);
        if ((activate && (g_index == 10 || g_index == 11)) || addKey)
        {
            const bool project = g_range == 2;
            const int missing = project ? MissingProjectClip() : -1;
            if (!project && !Flag(SnapshotPlaying))
            {
                PlayUi(UiSound::Error);
                g_note = TR("export.note.open_clip", "Open a clip first"); g_noteUntil = GetTickCount64() + 2500;
            }
            else if (missing >= 0)
            {
                // Said before anything runs (the script refuses it too).
                PlayUi(UiSound::Error);
                g_note = TRF("export.note.clip_missing", "{clip} is not in ReplayClips: the project cannot be exported",
                    {{"clip", ClipLabel(g_snapshot.projectClips[missing].file)}});
                g_noteUntil = GetTickCount64() + 4000;
            }
            else if (!controller.ReShadeProblem().empty())
            {
                PlayUi(UiSound::Error);
                g_note = TR("export.note.reshade", "This ReShade version is not supported"); g_noteUntil = GetTickCount64() + 2500;
            }
            else if (!presetUsable || !controller.EncoderProblem().empty())
            {
                PlayUi(UiSound::Error);
                g_note = TR("export.note.no_encoder", "No encoder for this format"); g_noteUntil = GetTickCount64() + 2500;
            }
            else if (endMs <= startMs)
            {
                PlayUi(UiSound::Error);
                g_note = TR("export.note.empty_range", "The range is empty"); g_noteUntil = GetTickCount64() + 2500;
            }
            else
            {
                PlayUi(UiSound::Select);
                g_queue.push_back(Build());
                g_requestedAt = 0;
                if (g_index == 10 && !addKey)
                    Service(); // starts right away
            }
        }

        // Right side: what will come out. Laid out in two passes, measured then
        // drawn: each block stands under the one before and the panel grows to them,
        // so a wrapped line never lands over the next one.
        const float rx = 640.0f;
        const float ry = top + 130.0f;
        constexpr float kInfoW = 480.0f;
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const std::uint32_t width = static_cast<std::uint32_t>(display.x);
        const std::uint32_t height = static_cast<std::uint32_t>(display.y);
        const double seconds = static_cast<double>(endMs - startMs) / 1000.0;
        const bool halved = kPresets[g_preset] == ec::VideoPreset::PreviewH264;
        const std::string resolution = TRF("export.resolution", "Resolution: {width}x{height} (the game's){halved}",
            {{"width", std::to_string(halved ? width / 2 : width)}, {"height", std::to_string(halved ? height / 2 : height)},
                {"halved", halved ? TR("export.resolution.halved", ", halved") : ""}});
        char size[32]{};
        FormatSize(render_export::EstimateBytes(kPresets[g_preset], width, height, kRates[g_rate], seconds), size, sizeof(size));
        const std::string length = TRF("export.length", "Length {seconds} s, about {size}", {{"seconds", replay::i18n::Printf("%.1f", seconds)}, {"size", size}});
        const std::uint32_t renders = kSamples[g_samples];
        const std::string frames = TRF("export.renders", "Game frames per video frame: {count}{slower}",
            {{"count", std::to_string(renders)}, {"slower", renders > 1 ? TR("export.renders.slower", " (slower, smoother motion)") : ""}});
        std::string problem = controller.ReShadeProblem();
        if (problem.empty())
            problem = controller.EncoderProblem();
        std::string encoder;
        if (!backend.empty())
            encoder = TRF("export.encoder", "Encoder: {encoder}", {{"encoder", backend == "CPU" ? std::string(TR("export.encoder.cpu", "CPU (slower)"))
                    : backend == "ffmpeg.exe" ? std::string("ffmpeg.exe") : backend + " (GPU)"}});
        // The blur reduced in slow motion, said before it starts.
        std::vector<std::string> blurLines;
        std::size_t moreSpans = 0;
        if (g_range != 2) // the open clip's markers (a project's other clips are not in the snapshot)
        {
            const std::vector<replay::exporting::BlurSpan> blur = BlurPlan(startMs, endMs);
            const std::uint32_t asked = kSamples[g_samples];
            const std::uint32_t atNormal = replay::exporting::SamplesForWorldScale(kRates[g_rate], {asked, kShutters[g_shutter]}, 0, 1.0,
                static_cast<std::int64_t>(replay::exporting::kEngineMinStepS * 1e9 + 0.5));
            if (atNormal < asked)
                blurLines.push_back(TRF("export.blur.capped", "Motion blur: {samples} samples per frame ({asked} would step below the game's minimum).",
                    {{"samples", std::to_string(atNormal)}, {"asked", std::to_string(asked)}}));
            std::size_t shown = 0;
            for (const replay::exporting::BlurSpan& span : blur)
            {
                if (span.worldScale >= 1.0)
                    continue; // the line above
                if (shown == 3)
                {
                    ++moreSpans;
                    continue;
                }
                ++shown;
                const std::string from = replay::i18n::Printf("%.1f", span.outStartS), to = replay::i18n::Printf("%.1f", span.outEndS);
                if (span.frameBelowMinimum)
                    blurLines.push_back(TRF("export.blur.below", "Slow motion at {from} to {to} s: no blur, and the world runs a little fast there (below the game's minimum step).",
                        {{"from", from}, {"to", to}}));
                else if (span.samples == 1)
                    blurLines.push_back(TRF("export.blur.none", "Slow motion at {from} to {to} s: blur reduced to 1 sample (none).", {{"from", from}, {"to", to}}));
                else
                    blurLines.push_back(TRF("export.blur.reduced", "Slow motion at {from} to {to} s: blur reduced to {samples} samples.",
                        {{"from", from}, {"to", to}, {"samples", std::to_string(span.samples)}}));
            }
        }
        std::string detail;
        if (!status.detail.empty())
            detail = std::string(StateText(status.state)) + ": " + status.detail;
        else if (status.state == ec::State::Failed && status.error == render_export::ExportError::ProjectClipMissing)
            detail = std::string(StateText(status.state)) + ": " +
                TR("export.project.missing", "a clip of the project is not in ReplayClips (see the project screen). Nothing was exported.");
        else if (status.state == ec::State::Failed && status.error == render_export::ExportError::ProjectClipLoad)
            detail = std::string(StateText(status.state)) + ": " +
                TR("export.project.load", "a clip of the project could not be opened (see the project screen). The pieces exported were deleted.");
        const auto layout = [&](ImDrawList* d) {
            float y = ry;
            const auto label = [&](float atLeast, float sizeUnits, ImU32 color, const char* text) {
                y = std::max(y, atLeast);
                y += LabelFit(d, rx, y, sizeUnits, color, text, kInfoW) + 6.0f;
            };
            const auto paragraph = [&](float atLeast, float sizeUnits, ImU32 color, const char* text) {
                y = std::max(y, atLeast);
                y += (d ? Paragraph(d, rx, y, sizeUnits, color, text, kInfoW) : ParagraphHeight(sizeUnits, text, kInfoW)) + 6.0f;
            };
            label(ry, 24.0f, kGold, formatLabel);
            paragraph(ry + 34.0f, 18.0f, kMuted, TR(info.noteKey, info.note));
            label(ry + 80.0f, 20.0f, kText, resolution.c_str());
            label(ry + 110.0f, 20.0f, kText, length.c_str());
            label(ry + 140.0f, 20.0f, kText, frames.c_str());
            if (!problem.empty())
                paragraph(ry + 170.0f, 16.0f, kRed, problem.c_str());
            else if (!presetUsable)
                label(ry + 170.0f, 20.0f, kRed, TR("export.no_encoder_pc", "No encoder for this format on this PC."));
            else if (!encoder.empty())
                label(ry + 170.0f, 20.0f, kText, encoder.c_str());
            paragraph(ry + 200.0f, 16.0f, kMuted,
                TR("export.how", "The game renders frame by frame on a locked clock: nothing stutters, whatever the speed of the PC. The game's sound is "
                "muted during the video pass and recorded in a second, real-time pass."));
            if (!blurLines.empty() || moreSpans)
                y = std::max(y, ry + 270.0f); // the blur notes where they always start
            for (const std::string& blurLine : blurLines)
                paragraph(y, 16.0f, kGold, blurLine.c_str());
            if (moreSpans)
            {
                label(y, 16.0f, kGold, TRF("export.blur.more", "... and {count} more slow motion spans.", {{"count", std::to_string(moreSpans)}}).c_str());
            }
            if (!detail.empty())
                paragraph(std::max(ry + 290.0f, y + 2.0f), 16.0f, status.state == ec::State::Done ? kGold : kRed, detail.c_str());
            return y;
        };
        // Fitted to the content as drawn (the last line's 6-unit gap replaced by the
        // panel's foot), like the other text panels.
        const float panelBottom = layout(nullptr) - 6.0f + kPanelPadBottom;
        InkPanel(draw, rx - kPanelPad, ry - kPanelPad, rx + kInfoW + kPanelPad, panelBottom, kPanel); // taller: the blur notes
        layout(draw);
        UI_PROBE_BOX("endpanel", P(0.0f, 0.0f), P(0.0f, 0.0f));
        if (GetTickCount64() < g_noteUntil)
            Text(draw, 80.0f, g_vh - g_footH - 50.0f, 22.0f, kRed, g_note.c_str());
        if (Back())
        {
            PlayUi(UiSound::Back);
            g_screen = g_back;
        }
    }

    // The length column of the gallery's and the projects' rows (x 590, the
    // row ending at 640): a longer word of another language ("old") ends at
    // the row's end instead.
    float LengthColumnX(const char* text)
    {
        const float width = TextWidth(20.0f, text) / g_scale;
        return !replay::i18n::Active() || 590.0f + width <= 636.0f ? 590.0f : 636.0f - width;
    }

    // The details panel of the gallery and of the projects (its content
    // kPanelPad inside; the values' column).
    constexpr float kDetailLeft = 700.0f;
    constexpr float kDetailRight = 1200.0f;
    constexpr float kDetailValueX = 850.0f;

    void DrawGallery(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("Gallery");
        MenuColumn(draw, 660.0f);
        TitleBanner(draw, 50.0f, 20.0f, 420.0f, 70.0f, 62.0f, 72.0f, 30.0f, 44.0f, "REDEMPTION EDITOR");
        SectionTitle(draw, 80.0f, 100.0f, 560.0f, g_galleryPicking ? TR("gallery.title.pick", "ADD A CLIP TO THE PROJECT") : TR("gallery.title", "RECORDED CLIPS"));

        // Sort row.
        Text(draw, 80.0f, 140.0f, 14.0f, kMuted, TR("gallery.sort_by", "SORT BY"));
        const char* sorts[] = {TR("gallery.sort.date", "Date"), TR("gallery.sort.duration", "Duration")};
        float sx = 160.0f;
        if (replay::i18n::Active()) // after a long "SORT BY" (English keeps its place)
            sx = std::max(sx, 80.0f + TextWidth(14.0f, TR("gallery.sort_by", "SORT BY")) / g_scale + 20.0f);
        for (int n = 0; n < 2; ++n)
        {
            ImGui::SetCursorScreenPos(P(sx - 6.0f, 136.0f));
            ImGui::PushID(100 + n);
            if (ItemClicked(ImGui::InvisibleButton("##sort", ImVec2(S(TextWidth(16.0f, sorts[n]) / g_scale + 12.0f), S(24.0f)))) &&
                g_gallerySort != n)
            {
                g_gallerySort = n;
                g_galleryIndex = 0;
                PlayUiStep(n == 0 ? -1 : 1);
            }
            ImGui::PopID();
            if (n == g_gallerySort)
                Brush(draw, sx - 6.0f, 136.0f, TextWidth(16.0f, sorts[n]) / g_scale + 6.0f, 24.0f, kRed);
            Text(draw, sx, 138.0f, 16.0f, kText, sorts[n]);
            sx += TextWidth(16.0f, sorts[n]) / g_scale + 34.0f;
        }
        if (KeyPressed(ImGuiKey_Tab))
        {
            g_gallerySort = 1 - g_gallerySort;
            g_galleryIndex = 0;
            PlayUiStep(g_gallerySort == 0 ? -1 : 1);
        }

        const int count = static_cast<int>(g_snapshot.clipTotal);
        if (g_snapshot.clipListSequence != g_gallerySequence)
        {
            g_gallerySequence = g_snapshot.clipListSequence;
            g_galleryIndex = std::clamp(g_galleryIndex, 0, std::max(0, count - 1));
        }
        NavigateList(g_galleryIndex, count);
        // The window: centred on the selection, moved when it nears an edge.
        {
            const int window = static_cast<int>(kMaxClipsInSnapshot);
            const int want = std::clamp(g_galleryIndex - window / 2, 0, std::max(0, count - window));
            const int held = g_snapshot.clipCount ? static_cast<int>(g_snapshot.clips[0].listIndex) : 0;
            if (g_gallerySort != g_requestedSort || !GalleryClip(g_galleryIndex) ||
                (std::abs(want - held) >= window / 4 && (held > 0 || want > held)))
                RequestClipWindow(static_cast<std::uint32_t>(want));
        }

        // The list: at most 11 rows, scrolled to keep the selection.
        constexpr int kRows = 11;
        const int first = std::clamp(g_galleryIndex - kRows / 2, 0, std::max(0, count - kRows));
        // The core reads the folder on a thread; the list shown meanwhile is the last
        // one read.
        const bool listLoading = Flag(SnapshotClipListLoading);
        if (count == 0)
        {
            const char* const text = !ClipListKnown() ? TR("gallery.loading", "Loading clips...") : KeyFill(TR("gallery.empty", "No clips yet. Record one from the home screen ({key_record})."));
            Text(draw, 80.0f, 190.0f, 20.0f, kMuted, text);
            if (!ClipListKnown())
                LoadingAfter(draw, 80.0f, 190.0f, 20.0f, text, 640.0f);
        }
        else if (listLoading)
        {
            // Painted, the turning cylinder after the words in place of the text spinner.
            static const char* kSpin[] = {"|", "/", "-", "\\"};
            const bool cue = ui_art::Get(ui_art::Art::Loading) != 0;
            const std::string text = TRF("gallery.loading_spin", "Loading clips {spinner}", {{"spinner", cue ? "" : kSpin[(GetTickCount64() / 150) % 4]}});
            Text(draw, 420.0f, 140.0f, 14.0f, kMuted, text.c_str());
            if (cue)
                LoadingAfter(draw, 420.0f, 140.0f, 14.0f, text.c_str(), 660.0f);
        }
        for (int row = 0; row < kRows && first + row < count; ++row)
        {
            const int n = first + row;
            const float y = 180.0f + row * 40.0f;
            const ClipInfo* listed = GalleryClip(n);
            if (!listed)
            {
                Text(draw, 88.0f, y + 7.0f, 20.0f, kMuted, "...");
                continue;
            }
            const ClipInfo& clip = *listed;
            // The mouse takes the selection only in a list that fits (a longer one
            // scrolls to keep the selection in its middle, under the mouse).
            bool clicked = false, pointed = false;
            ImGui::PushID(n);
            const bool doubled = ListItem(draw, "##clip", 80.0f, y, 560.0f, 36.0f, n == g_galleryIndex, clicked, count <= kRows ? &pointed : nullptr);
            ImGui::PopID();
            if (clicked || pointed)
                g_galleryIndex = n;
            if (doubled)
                PlayClip(clip);
            const bool selected = n == g_galleryIndex;
            const ImU32 color = !clip.loadable ? kMuted : (selected ? kText : kText);
            // Open while it plays.
            const bool openNow = Flag(SnapshotPlaying) && std::strcmp(clip.name, g_snapshot.currentClip) == 0;
            char duration[16]{};
            FormatClock(clip.durationMs, duration, sizeof(duration), false);
            const char* lengthText = clip.loadable ? duration : TR("gallery.old", "old");
            const float lengthX = LengthColumnX(lengthText);
            Text(draw, lengthX, y + 7.0f, 20.0f, color, lengthText);
            // A compressed clip: a gold badge left of the duration, as tall as its text
            // (at least 18 units), and the name cut with "..." before it.
            float labelRight = std::min(580.0f, lengthX - 10.0f);
            if (clip.compressed)
            {
                const float badgeWidth = TextWidth(12.0f, "ZIP") / g_scale + 10.0f;
                const float bx = labelRight + 2.0f - badgeWidth;
                const float badgeH = std::max(18.0f, LabelHeight(12.0f) + 4.0f);
                const float by = y + 18.0f - badgeH * 0.5f;
                draw->AddRect(P(bx, by), P(bx + badgeWidth, by + badgeH), kGold, S(3.0f), 0, S(1.5f));
                Text(draw, bx + 5.0f, by + 2.0f + (badgeH - 18.0f) * 0.5f, 12.0f, kGold, "ZIP");
                labelRight = bx - 8.0f;
            }
            const std::string label = ClipLabel(clip.name) + (openNow ? TR("gallery.open_now", "   (open)") : "");
            TextFit(draw, 88.0f, y + 7.0f, 20.0f, color, label.c_str(), labelRight - 88.0f);
        }
        if (count > kRows)
        {
            Text(draw, 80.0f, 628.0f, 14.0f, kMuted, TRF("gallery.position", "{at} of {count}", {{"at", std::to_string(g_galleryIndex + 1)}, {"count", std::to_string(count)}}).c_str());
        }

        // Details of the selected clip: the rows first, measured, so the panel grows
        // to them.
        std::vector<std::pair<const char*, std::string>> details;
        const ClipInfo* selectedClip = GalleryClip(g_galleryIndex);
        if (selectedClip)
        {
            const ClipInfo& clip = *selectedClip;
            char duration[16]{};
            char size[32]{};
            FormatClock(clip.durationMs, duration, sizeof(duration), true);
            FormatSize(clip.bytes, size, sizeof(size));
            std::string format;
            const char* cannotOpen = clip.loadable ? "" : TR("gallery.size.cannot_open", " (cannot open)");
            if (clip.compressed && clip.clipBytes)
            {
                char full[32]{};
                FormatSize(clip.clipBytes, full, sizeof(full));
                format = TRF("gallery.size.compressed_of", "{size} compressed ({full}), format {version}{cannot}",
                    {{"size", size}, {"full", full}, {"version", std::to_string(clip.formatVersion)}, {"cannot", cannotOpen}});
            }
            else
                format = TRF("gallery.size", "{size}{compressed}, format {version}{cannot}",
                    {{"size", size}, {"compressed", clip.compressed ? TR("gallery.size.compressed", " compressed") : ""},
                        {"version", std::to_string(clip.formatVersion)}, {"cannot", cannotOpen}});
            char clock[16]{};
            if (clip.hasClock)
                std::snprintf(clock, sizeof(clock), "%02u:%02u", clip.clockStartS / 3600, clip.clockStartS / 60 % 60);
            const auto orDash = [](const char* text) { return std::string(text && text[0] ? text : "-"); };
            // How the clip is stored, in words.
            std::string storage;
            if (clip.compressed && clip.clipBytes && clip.bytes)
                storage = TRF("gallery.storage.ratio", "Compressed, {ratio}x smaller",
                    {{"ratio", replay::i18n::Printf("%.1f", static_cast<double>(clip.clipBytes) / static_cast<double>(clip.bytes))}});
            // The file the compressor works on now (not necessarily the clip C asked
            // for).
            else if (!clip.compressed && std::strcmp(g_snapshot.compressingClip, clip.name) == 0)
                storage = TR("gallery.storage.compressing", "Compressing...");
            else if (!clip.compressed && g_compressingClip == clip.name && g_snapshot.compressState >= 2)
                storage = TR("gallery.storage.waiting", "Waiting to be compressed");
            else
                storage = clip.compressed ? TR("gallery.storage.compressed", "Compressed") : TR("gallery.storage.not_compressed", "Not compressed");
            // The chapter the clip was recorded in (and its camp).
            char chapter[160]{};
            replay::world::ChapterText(static_cast<std::uint8_t>(clip.chapter), chapter, sizeof(chapter));
            if (chapter[0] && clip.camp < 9)
            {
                const std::size_t used = std::strlen(chapter);
                std::snprintf(chapter + used, sizeof(chapter) - used, " (%s)",
                    replay::world::CampName(static_cast<std::uint8_t>(clip.camp)));
            }
            details = {
                {TR("gallery.detail.duration", "Duration"), duration},
                {TR("gallery.detail.time", "In-game time"), orDash(clock)},
                {TR("gallery.detail.weather", "Weather"), orDash(replay::envoverride::WeatherDisplay(clip.weather))},
                {TR("gallery.detail.character", "Character"), orDash(clip.character)},
                {TR("gallery.detail.chapter", "Chapter"), orDash(chapter)},
                {TR("gallery.detail.region", "Region"), orDash(clip.region)},
                {TR("gallery.detail.file", "File"), clip.name},
                {TR("gallery.detail.size", "Size"), format},
                {TR("gallery.detail.storage", "Storage"), storage},
            };
        }
        // The values' column from the panel's padding.
        const float detailX = kDetailLeft + kPanelPad;
        const float valueW = kDetailRight - kPanelPad - kDetailValueX;
        float detailsBottom = 600.0f;
        {
            float y = 432.0f;
            for (const auto& [label, value] : details)
                y += std::max(22.0f, ParagraphHeight(15.0f, value.c_str(), valueW) + 4.0f);
            detailsBottom = std::max(detailsBottom, y + kPanelPadBottom);
        }
        InkPanel(draw, kDetailLeft, 140.0f, kDetailRight, detailsBottom, kPanel);
        draw->AddRectFilled(P(725.0f, 165.0f), P(1175.0f, 418.0f), IM_COL32(40, 38, 34, 255));
        const ImTextureID thumbnail = selectedClip ? clip_thumbnails::Thumbnail(g_runtime, selectedClip->name) : 0;
        if (thumbnail)
            // The texture shows upside down on this renderer (the file on disk is
            // upright): sampled with V flipped.
            draw->AddImage(ImTextureRef(thumbnail), P(725.0f, 165.0f), P(1175.0f, 418.0f), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
        else if (selectedClip && clip_thumbnails::Loading(selectedClip->name))
        {
            Text(draw, 900.0f, 282.0f, 16.0f, kMuted, TR("gallery.thumb.loading", "[ loading ]")); // read on a thread
            LoadingCue(draw, 950.0f, 252.0f, 36.0f);
        }
        else
            Text(draw, 880.0f, 282.0f, 16.0f, kMuted, TR("gallery.thumb.none", "[ no thumbnail ]"));
        if (selectedClip)
        {
            const ClipInfo& clip = *selectedClip;
            float y = 432.0f;
            for (const auto& [label, value] : details)
            {
                TextFit(draw, detailX, y, 15.0f, kMuted, label, kDetailValueX - detailX - 8.0f);
                // Long values (a region) wrap inside the panel.
                const float height = Paragraph(draw, kDetailValueX, y, 15.0f, kText, value.c_str(), valueW);
                y += std::max(22.0f, height + 4.0f);
            }
            if (Confirmed())
                PlayClip(clip);
            // R renames the selected clip.
            if (!g_galleryPicking && KeyPressed(ImGuiKey_R, false))
                ShowRename(clip);
            // C compresses the selected clip now (any age; not the open one, which the
            // compressor never touches).
            if (!g_galleryPicking && KeyPressed(ImGuiKey_C, false))
            {
                const bool open = Flag(SnapshotPlaying) && std::strcmp(clip.name, g_snapshot.currentClip) == 0;
                if (clip.compressed || open || !clip.loadable)
                    PlayUi(UiSound::Error);
                else if (SendNamed(CommandType::CompressClip, clip.name))
                {
                    PlayUi(UiSound::Select);
                    g_compressingClip = clip.name;
                }
            }
            if (!g_galleryPicking && KeyPressed(ImGuiKey_Delete, false))
            {
                PlayUi(UiSound::Select);
                // The open clip closes and its unsaved marker changes go with it (the core
                // drops them).
                const bool openNow = Flag(SnapshotPlaying) && std::strcmp(clip.name, g_snapshot.currentClip) == 0;
                ShowDeleteClipAlert(clip.name, openNow);
            }
        }
        else if (Confirmed())
            PlayUi(UiSound::Error);

        if (Back())
        {
            PlayUi(UiSound::Back);
            g_screen = g_galleryPicking ? Screen::Project : Screen::Home;
            g_galleryPicking = false;
        }
    }

    // ---------------- Rename ----------------
    std::string g_renameFrom;
    char g_renameText[48]{};
    bool g_renameFocus = false;
    const char* g_renameError = ""; // why Enter was refused, under the field

    void ShowRename(const ClipInfo& clip)
    {
        g_renameFrom = clip.name;
        std::string stem = clip.name;
        if (stem.size() > 7 && stem.substr(stem.size() - 7) == ".rpclip")
            stem.resize(stem.size() - 7);
        std::snprintf(g_renameText, sizeof(g_renameText), "%s", stem.c_str());
        g_renameFocus = true;
        g_renameError = "";
        g_screen = Screen::Rename;
    }

    // The dialogs (Alert, Rename, New/Rename project): an ink panel round their
    // title, line and text (the content 440 to 840, the title at 210), from 192
    // to `bottom`.
    constexpr float kDialogTop = 192.0f;

    void DialogPanel(ImDrawList* draw, float bottom)
    {
        InkPanel(draw, 440.0f - kPanelPad, kDialogTop, 840.0f + kPanelPad, std::max(bottom, kDialogTop + 140.0f), kPanel);
    }

    // The name boxes (Rename, New/Rename project): the file line, the box, its
    // help and the refusal laid out under each other. A long file name wraps (at
    // any character) and pushes the rest down.
    struct NameBoxPlaces
    {
        float inputY;
        float helpY;
        float errorY;
    };

    NameBoxPlaces PlaceNameBox(const char* fileLine, const char* help)
    {
        NameBoxPlaces at{};
        at.inputY = std::max(330.0f, 292.0f + (fileLine ? ParagraphHeight(17.0f, fileLine, 400.0f) : 0.0f) + 8.0f);
        at.helpY = std::max(372.0f, at.inputY + ImGui::GetFrameHeight() / g_scale + 14.0f);
        at.errorY = std::max(400.0f, at.helpY + ParagraphHeight(15.0f, help, 400.0f) + 6.0f);
        return at;
    }

    void DrawRename(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("Rename");
        draw->AddRectFilled(ImVec2(0.0f, 0.0f), ImGui::GetIO().DisplaySize, IM_COL32(0, 0, 0, 215));
        const std::string fileLine = TRF("rename.file", "File: {file}", {{"file", g_renameFrom}});
        const char* const kHelp = TR("rename.clip.help", "Letters, numbers, spaces, - and _. The thumbnail keeps up with it.");
        const NameBoxPlaces at = PlaceNameBox(fileLine.c_str(), kHelp);
        DialogPanel(draw, (g_renameError[0] ? at.errorY + ParagraphHeight(15.0f, g_renameError, 400.0f) : at.helpY + ParagraphHeight(15.0f, kHelp, 400.0f)) +
            kPanelPadBottom);
        const char* const title = TR("rename.clip.title", "Rename clip");
        const float width = TextWidth(40.0f, title) / g_scale;
        Text(draw, 640.0f - width / 2.0f, 210.0f, 40.0f, kText, title);
        draw->AddLine(P(440.0f, 268.0f), P(840.0f, 268.0f), kRed, S(3.0f));
        Paragraph(draw, 440.0f, 292.0f, 17.0f, kMuted, fileLine.c_str(), 400.0f);
        ImGui::SetCursorScreenPos(P(440.0f, at.inputY));
        ImGui::PushItemWidth(S(400.0f));
        if (g_renameFocus)
        {
            ImGui::SetKeyboardFocusHere();
            g_renameFocus = false;
        }
        const bool entered = ImGui::InputText("##rename", g_renameText, sizeof(g_renameText),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll, nullptr, nullptr) ||
            MissedTap(VK_RETURN);
        ImGui::PopItemWidth();
        Paragraph(draw, 440.0f, at.helpY, 15.0f, kMuted, kHelp, 400.0f);
        if (g_renameError[0])
            Paragraph(draw, 440.0f, at.errorY, 15.0f, kGold, g_renameError, 400.0f);
        if (entered)
        {
            std::string name(g_renameText);
            while (!name.empty() && name.back() == ' ')
                name.pop_back(); // a trailing space is not part of the name
            bool ok = !name.empty();
            for (const char c : name)
                ok = ok && (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_');
            const std::string both = g_renameFrom + "|" + name; // the core adds .rpclip
            // The refusals say why, and the same name is no rename.
            g_renameError = name.empty() ? NameEmptyError() : !ok ? NameCharsError() : "";
            if (ok && both.size() >= kClipNameCapacity - 1)
            {
                ok = false;
                g_renameError = TR("rename.error.long", "That name is too long.");
            }
            if (ok && name + ".rpclip" == g_renameFrom)
            {
                PlayUi(UiSound::Back);
                g_screen = Screen::Gallery;
            }
            else if (ok && SendNamed(CommandType::RenameClip, both.c_str()))
            {
                clip_thumbnails::Forget(g_renameFrom);
                PlayUi(UiSound::Select);
                g_screen = Screen::Gallery;
            }
            else
                PlayUi(UiSound::Error);
        }
        else if (!g_helpOpen && !g_helpSwallow && KeyPressed(ImGuiKey_Escape))
        {
            PlayUi(UiSound::Back);
            g_screen = Screen::Gallery;
        }
    }

    // ---------------- QA note ----------------
    // Opened by a "mark problem" (Shift+F2, the core's key) over the playback,
    // which goes on (the 2 s after the press are part of the mark). Enter
    // sends the note, empty or not; Esc sends none. Drawn after the render
    // export's capture point and never during an export (the core refuses
    // a mark then).
    void DrawQaNote(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("QaNote");
        qa_shots::State& qa = qa_shots::g_state;
        // The content measured first and laid out under each other, the box
        // kPanelPad around it and as tall as it needs.
        constexpr float kNoteX = 350.0f, kNoteW = 580.0f;
        const char* const kExample =
            KeyFill(TR("qa.note.example", "For example: \"the horse froze\", \"no voice\", \"he vanished\". {key_select} saves (empty is fine), {key_back} skips."));
        const char* const kReport = KeyFill(TR("qa.note.report", "When you are done testing, press {key_report}: one zip with every mark and its clips."));
        const std::string at = TRF("qa.note.at", "At {time} of the clip. The 8 seconds before and 2 after are saved with it. What went wrong? (optional)",
            {{"time", replay::qa::TimeText(qa.notePlayheadMs)}});
        const float inputY = std::max(214.0f, 156.0f + ParagraphHeight(16.0f, at.c_str(), kNoteW) + 8.0f);
        const float exampleY = std::max(258.0f, inputY + ImGui::GetFrameHeight() / g_scale + 14.0f);
        const float reportY = std::max(300.0f, exampleY + ParagraphHeight(15.0f, kExample, kNoteW) + 8.0f);
        const float bottom = std::max(352.0f, reportY + ParagraphHeight(16.0f, kReport, kNoteW) + kPanelPadBottom);
        const float boxLeft = kNoteX - kPanelPad, boxRight = kNoteX + kNoteW + kPanelPad;
        InkPanel(draw, boxLeft, 96.0f, boxRight, bottom, kPanel); // taller, for the Shift+F1 line
        if (!ui_art::Get(ui_art::Art::EdgeInk))
            draw->AddRect(P(boxLeft, 96.0f), P(boxRight, bottom), kPanelLine, 0.0f, 0, S(1.0f)); // the flat look's outline
        const char* const title = TR("qa.note.title", "PROBLEM MARKED");
        Brush(draw, boxLeft, 106.0f, std::max(290.0f, TextWidth(24.0f, title) / g_scale + 40.0f), 36.0f, kRed); // as wide as a long title
        Text(draw, boxLeft + 16.0f, 110.0f, 24.0f, kText, title);
        Paragraph(draw, kNoteX, 156.0f, 16.0f, kText, at.c_str(), kNoteW);
        ImGui::SetCursorScreenPos(P(kNoteX, inputY));
        ImGui::PushItemWidth(S(kNoteW));
        if (qa.noteFocus)
        {
            ImGui::SetKeyboardFocusHere();
            qa.noteFocus = false;
        }
        const bool entered = ImGui::InputText("##qanote", qa.note, sizeof(qa.note), ImGuiInputTextFlags_EnterReturnsTrue, nullptr, nullptr) ||
            MissedTap(VK_RETURN);
        ImGui::PopItemWidth();
        Paragraph(draw, kNoteX, exampleY, 15.0f, kMuted, kExample, kNoteW);
        // The report key on its own line, in plain text.
        Paragraph(draw, kNoteX, reportY, 16.0f, kText, kReport, kNoteW);
        if (entered)
        {
            qa_shots::SendNote(true);
            PlayUi(UiSound::Select);
            ShowMarkNotice();
        }
        else if (KeyPressed(ImGuiKey_Escape, false))
        {
            qa_shots::SendNote(false);
            PlayUi(UiSound::Back);
            ShowMarkNotice();
        }
    }

    // ---------------- Projects ----------------
    // The project screens: the project files, and the open project's timeline,
    // where the clips stand in order as blocks as wide as their trimmed length.
    // The core owns the project (script.cpp, Projects); the editor shows the
    // snapshot and sends commands, as everywhere else.
    int g_projectsIndex = 0;
    std::uint64_t g_projectsSeen = 0;
    std::uint32_t g_projectMessageSeen = 0;
    ULONGLONG g_projectMessageUntil = 0;
    std::string g_projectMessageShown;
    int g_editAfterOpen = -1;          // Enter on a clip: the clip editor once it has opened
    ULONGLONG g_editAfterOpenAt = 0;
    int g_dragBlock = -1;              // a block held with the mouse (moved where it is let go)
    float g_dragBlockFromX = 0.0f;
    bool g_dragBlockMoved = false;
    bool g_projectScrubbing = false;   // the project's time bar dragged
    std::uint64_t g_projectLastSeek = UINT64_MAX;
    // The name screen: a new project or a project renamed.
    bool g_projectNameNew = true;
    std::string g_projectNameFile;
    char g_projectNameText[64]{}; // cut to kMaxNameBytes by CleanName
    std::string g_projectsSelectFile; // the file the Projects list selects when it comes back (a project renamed)
    bool g_projectNameFocus = false;
    const char* g_projectNameError = "";
    Screen g_projectNameBack = Screen::Projects;

    void ShowProjectName(bool isNew, const char* file, const char* name, Screen back)
    {
        g_projectNameNew = isNew;
        g_projectNameFile = file ? file : "";
        std::snprintf(g_projectNameText, sizeof(g_projectNameText), "%s", name ? name : "");
        g_projectNameFocus = true;
        g_projectNameError = "";
        g_projectNameBack = back;
        g_screen = Screen::ProjectName;
    }

    // A key hint that is a button too (the Projects panel, the project's header):
    // its key cap and label, a click does what the key does. True when clicked.
    float KeyActionWidth(const char* key, const char* label) { return KeyHintWidth(key, label) - 22.0f; }

    bool KeyAction(ImDrawList* draw, const char* id, float x, float y, const char* key, const char* label, bool on = true)
    {
        const float width = KeyActionWidth(key, label);
        ImGui::SetCursorScreenPos(P(x - 6.0f, y - 4.0f));
        const bool clicked = ItemClicked(ImGui::InvisibleButton(id, ImVec2(S(width + 12.0f), S(28.0f))));
        if (on && HoverShown(ImGui::IsItemHovered()))
            draw->AddRect(P(x - 6.0f, y - 4.0f), P(x + width + 6.0f, y + 24.0f), IM_COL32(236, 230, 218, 90), S(3.0f), 0, S(1.0f));
        KeyHint(draw, x, y, key, label, on);
        return clicked && on;
    }

    // The project files the snapshot lists (the name screen refuses a taken
    // name before it sends it; the core checks again).
    std::vector<std::string> ListedProjectFiles()
    {
        std::vector<std::string> files;
        const std::uint32_t count = std::min(g_snapshot.projectListCount, kMaxProjectsInSnapshot);
        for (std::uint32_t n = 0; n < count; ++n)
            files.emplace_back(g_snapshot.projects[n].file);
        return files;
    }

    void DrawProjectName(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("ProjectName");
        draw->AddRectFilled(ImVec2(0.0f, 0.0f), ImGui::GetIO().DisplaySize, IM_COL32(0, 0, 0, 215));
        const std::string fileLine = TRF("rename.file", "File: {file}", {{"file", g_projectNameFile}});
        const char* const kHelp = TR("rename.project.help", "Letters, numbers, spaces, - and _. The file in ReplayProjects takes the name.");
        const NameBoxPlaces at = PlaceNameBox(g_projectNameNew ? nullptr : fileLine.c_str(), kHelp);
        // What the typed name gives, as the core makes it (CleanName): the file it
        // is saved as when that differs from what was typed, a taken name at once.
        std::string typed(g_projectNameText);
        while (!typed.empty() && typed.back() == ' ')
            typed.pop_back();
        while (!typed.empty() && typed.front() == ' ')
            typed.erase(typed.begin());
        const std::string clean = replay::project::CleanName(typed);
        const replay::project::RenameCheck check =
            replay::project::CheckRename(g_projectNameNew ? std::string() : g_projectNameFile, clean, ListedProjectFiles());
        std::string note;
        if (check == replay::project::RenameCheck::Exists)
            note = TR("project.msg.name_exists", "A project with that name exists.");
        else if (!clean.empty() && clean != typed)
            note = TRF("rename.project.saved_as", "Saved as: {file}", {{"file", replay::project::NameFile(clean)}});
        const char* const shownError = g_projectNameError[0] ? g_projectNameError : note.c_str();
        DialogPanel(draw, (shownError[0] ? at.errorY + ParagraphHeight(15.0f, shownError, 400.0f)
                                         : at.helpY + ParagraphHeight(15.0f, kHelp, 400.0f)) + kPanelPadBottom);
        const char* title = g_projectNameNew ? TR("rename.project.new", "New project") : TR("rename.project.title", "Rename project");
        const float width = TextWidth(40.0f, title) / g_scale;
        Text(draw, 640.0f - width / 2.0f, 210.0f, 40.0f, kText, title);
        draw->AddLine(P(440.0f, 268.0f), P(840.0f, 268.0f), kRed, S(3.0f));
        if (!g_projectNameNew)
            Paragraph(draw, 440.0f, 292.0f, 17.0f, kMuted, fileLine.c_str(), 400.0f);
        ImGui::SetCursorScreenPos(P(440.0f, at.inputY));
        ImGui::PushItemWidth(S(400.0f));
        if (g_projectNameFocus)
        {
            ImGui::SetKeyboardFocusHere();
            g_projectNameFocus = false;
        }
        const bool entered = ImGui::InputText("##projectname", g_projectNameText, sizeof(g_projectNameText),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll, nullptr, nullptr) || MissedTap(VK_RETURN);
        ImGui::PopItemWidth();
        Paragraph(draw, 440.0f, at.helpY, 15.0f, kMuted, kHelp, 400.0f);
        if (shownError[0])
            Paragraph(draw, 440.0f, at.errorY, 15.0f, kGold, shownError, 400.0f);
        if (ImGui::IsItemEdited())
            g_projectNameError = ""; // typing again clears the last refusal
        if (entered)
        {
            // Characters a name cannot hold become spaces (CleanName); a taken
            // name is refused here and never overwrites the other project.
            const bool ok = check != replay::project::RenameCheck::Empty && check != replay::project::RenameCheck::Exists;
            g_projectNameError = clean.empty() ? NameEmptyError()
                : check == replay::project::RenameCheck::Exists ? TR("project.msg.name_exists", "A project with that name exists.") : "";
            if (ok)
            {
                bool sent = true;
                if (g_projectNameNew)
                {
                    sent = SendNamed(CommandType::ProjectNew, clean.c_str());
                    g_projectOpenAsked = GetTickCount64();
                }
                else if (check != replay::project::RenameCheck::Same)
                {
                    sent = SendNamed(CommandType::ProjectRename, (g_projectNameFile + "|" + clean).c_str());
                    g_projectsSelectFile = replay::project::NameFile(clean); // the list keeps it selected
                }
                PlayUi(sent ? UiSound::Select : UiSound::Error);
                g_screen = g_projectNameNew ? Screen::Project : g_projectNameBack;
            }
            else
                PlayUi(UiSound::Error);
        }
        else if (!g_helpOpen && !g_helpSwallow && KeyPressed(ImGuiKey_Escape))
        {
            PlayUi(UiSound::Back);
            g_screen = g_projectNameBack;
        }
    }

    void DrawProjects(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("Projects");
        MenuColumn(draw, 660.0f);
        TitleBanner(draw, 50.0f, 20.0f, 420.0f, 70.0f, 62.0f, 72.0f, 30.0f, 44.0f, "REDEMPTION EDITOR");
        SectionTitle(draw, 80.0f, 100.0f, 560.0f, TR("projects.title", "PROJECTS"));
        // Row 0 makes a new project; the files follow, newest first.
        const int files = static_cast<int>(std::min(g_snapshot.projectListCount, kMaxProjectsInSnapshot));
        const int count = files + 1;
        if (g_snapshot.projectListSequence != g_projectsSeen)
        {
            g_projectsSeen = g_snapshot.projectListSequence;
            g_projectsIndex = std::clamp(g_projectsIndex, 0, count - 1);
            // A renamed project moves (the list is newest first): it stays selected.
            for (int n = 0; n < files && !g_projectsSelectFile.empty(); ++n)
                if (replay::project::SameFile(g_snapshot.projects[n].file, g_projectsSelectFile))
                {
                    g_projectsIndex = n + 1;
                    g_projectsSelectFile.clear();
                }
        }
        NavigateList(g_projectsIndex, count);
        constexpr int kRows = 11;
        const int first = std::clamp(g_projectsIndex - kRows / 2, 0, std::max(0, count - kRows));
        for (int row = 0; row < kRows && first + row < count; ++row)
        {
            const int n = first + row;
            const float y = 140.0f + row * 40.0f;
            bool clicked = false, pointed = false; // the mouse selects in a list that fits (see the gallery)
            ImGui::PushID(n);
            const bool doubled = ListItem(draw, "##project", 80.0f, y, 560.0f, 36.0f, n == g_projectsIndex, clicked, count <= kRows ? &pointed : nullptr);
            ImGui::PopID();
            if (clicked || pointed)
                g_projectsIndex = n;
            if (doubled)
                g_projectsIndex = n;
            if (n == 0)
            {
                Text(draw, 88.0f, y + 7.0f, 20.0f, kText, TR("projects.new_row", "+  New Project"));
                if (doubled)
                {
                    PlayUi(UiSound::Select);
                    ShowProjectName(true, nullptr, "", Screen::Projects);
                    return;
                }
                continue;
            }
            const ProjectInfo& info = g_snapshot.projects[n - 1];
            const bool openNow = ProjectIsOpen() && std::strcmp(info.file, g_snapshot.projectFile) == 0;
            std::string label = info.name[0] ? info.name : info.file;
            if (openNow)
                label += TR("gallery.open_now", "   (open)");
            char duration[16]{};
            FormatClock(info.totalMs, duration, sizeof(duration), false);
            const char* lengthText = info.readable ? duration : TR("gallery.old", "old");
            const float lengthX = LengthColumnX(lengthText);
            TextFit(draw, 88.0f, y + 7.0f, 20.0f, info.readable ? kText : kMuted, label.c_str(), std::min(490.0f, lengthX - 98.0f)); // before the length
            Text(draw, lengthX, y + 7.0f, 20.0f, info.readable ? kText : kMuted, lengthText);
            if (doubled)
            {
                PlayUi(info.readable ? UiSound::Select : UiSound::Error);
                if (info.readable)
                {
                    SendNamed(CommandType::ProjectOpen, info.file);
                    g_projectOpenAsked = GetTickCount64();
                    g_projectIndex = 0;
                    g_screen = Screen::Project;
                    return;
                }
            }
        }
        // Details of the selected project, measured first so the panel grows to them
        // (the file name, which has no space, wraps at any character; a long name is
        // cut with "...").
        {
            const float cx = kDetailLeft + kPanelPad;
            const float cw = kDetailRight - kDetailLeft - 2.0f * kPanelPad;
            const float valueW = cx + cw - kDetailValueX;
            const char* const kNewProject =
                TR("projects.new.body", "A project puts clips in order, like the Rockstar Editor's: cut them, move them, trim each one with its "
                "start and end markers, give each its own cameras, and play them all as one video.");
            const char* const kOld = TR("projects.old", "Saved by another version of the editor: it cannot be opened.");
            const ProjectInfo* info = g_projectsIndex > 0 ? &g_snapshot.projects[g_projectsIndex - 1] : nullptr;
            const auto details = [&](ImDrawList* d) {
                float y = 216.0f;
                if (!info)
                    return y + (d ? Paragraph(d, cx, y, 17.0f, kText, kNewProject, cw) : ParagraphHeight(17.0f, kNewProject, cw));
                char duration[16]{};
                FormatClock(info->totalMs, duration, sizeof(duration), true);
                const std::string line = TRF("projects.length.value", "{count} clips, {length}", {{"count", std::to_string(info->clipCount)}, {"length", duration}});
                y = 220.0f;
                if (d)
                {
                    Text(d, cx, y, 15.0f, kMuted, TR("projects.length", "Length"));
                    TextFit(d, kDetailValueX, y, 15.0f, kText, line.c_str(), valueW);
                }
                y = std::max(244.0f, y + LabelHeight(15.0f) + 4.0f);
                if (d)
                    Text(d, cx, y, 15.0f, kMuted, TR("gallery.detail.file", "File"));
                y += std::max(22.0f, (d ? Paragraph(d, kDetailValueX, y, 15.0f, kText, info->file, valueW)
                                        : ParagraphHeight(15.0f, info->file, valueW)) + 4.0f);
                if (!info->readable)
                {
                    y = std::max(290.0f, y + 8.0f);
                    y += d ? Paragraph(d, cx, y, 15.0f, kGold, kOld, cw) : ParagraphHeight(15.0f, kOld, cw);
                }
                return y;
            };
            // A project's actions under its details, as buttons with their keys
            // (the key bar has them too): R renames it, Del deletes it.
            const char* const renameLabel = TR("keys.rename", "Rename");
            const char* const deleteLabel = TR("keys.delete", "Delete");
            const float actionsY = details(nullptr) + 18.0f;
            const float bottom = std::max(420.0f, (info ? actionsY + 20.0f : details(nullptr)) + kPanelPad);
            InkPanel(draw, kDetailLeft, 140.0f, kDetailRight, bottom, kPanel);
            TextFit(draw, cx, 160.0f, 26.0f, kText, info ? (info->name[0] ? info->name : info->file) : TR("home.new_project", "New Project"), cw);
            OrnamentDivider(draw, cx, 200.0f, cw);
            details(draw);
            if (info)
            {
                const ProjectInfo& chosen = *info;
                const float deleteX = cx + KeyActionWidth("R", renameLabel) + 22.0f;
                if (KeyAction(draw, "##projectrename", cx, actionsY, "R", renameLabel))
                {
                    PlayUi(UiSound::Select);
                    ShowProjectName(false, chosen.file, chosen.name, Screen::Projects);
                    return;
                }
                if (KeyAction(draw, "##projectdelete", deleteX, actionsY, "Del", deleteLabel))
                {
                    PlayUi(UiSound::Select);
                    const bool openNow = ProjectIsOpen() && std::strcmp(chosen.file, g_snapshot.projectFile) == 0;
                    ShowDeleteProjectAlert(chosen.file, chosen.name[0] ? chosen.name : chosen.file, openNow);
                    return;
                }
            }
        }
        if (Confirmed())
        {
            if (g_projectsIndex == 0)
            {
                PlayUi(UiSound::Select);
                ShowProjectName(true, nullptr, "", Screen::Projects);
                return;
            }
            const ProjectInfo& info = g_snapshot.projects[g_projectsIndex - 1];
            if (!info.readable)
                PlayUi(UiSound::Error);
            else
            {
                PlayUi(UiSound::Select);
                SendNamed(CommandType::ProjectOpen, info.file);
                g_projectOpenAsked = GetTickCount64();
                g_projectIndex = 0;
                g_screen = Screen::Project;
                return;
            }
        }
        if (g_projectsIndex > 0)
        {
            const ProjectInfo& info = g_snapshot.projects[g_projectsIndex - 1];
            if (KeyPressed(ImGuiKey_R, false))
            {
                PlayUi(UiSound::Select);
                ShowProjectName(false, info.file, info.name, Screen::Projects);
                return;
            }
            if (KeyPressed(ImGuiKey_Delete, false))
            {
                PlayUi(UiSound::Select);
                const bool openNow = ProjectIsOpen() && std::strcmp(info.file, g_snapshot.projectFile) == 0;
                ShowDeleteProjectAlert(info.file, info.name[0] ? info.name : info.file, openNow);
                return;
            }
        }
        if (Back())
        {
            PlayUi(UiSound::Back);
            g_screen = Screen::Home;
        }
    }

    // The blocks of the project's timeline: each clip as wide as its
    // trimmed length, at least a few pixels, fitted to the width.
    struct ProjectBlock
    {
        float x0 = 0.0f;
        float x1 = 0.0f;
        std::uint64_t startMs = 0;
        std::uint64_t durationMs = 0;
    };

    std::vector<ProjectBlock> LayoutProjectBlocks(float left, float width)
    {
        const std::uint32_t count = std::min(g_snapshot.projectClipCount, kMaxProjectClipsInSnapshot);
        std::vector<ProjectBlock> blocks(count);
        if (count == 0)
            return blocks;
        const double total = std::max<double>(1.0, static_cast<double>(g_snapshot.projectTotalMs));
        const float minWidth = std::min(26.0f, width / static_cast<float>(count));
        float sum = 0.0f;
        std::uint64_t start = 0;
        for (std::uint32_t n = 0; n < count; ++n)
        {
            const ProjectClipInfo& clip = g_snapshot.projectClips[n];
            blocks[n].startMs = start;
            blocks[n].durationMs = clip.outMs > clip.inMs ? clip.outMs - clip.inMs : 0;
            start += blocks[n].durationMs;
            blocks[n].x1 = std::max(minWidth, static_cast<float>(blocks[n].durationMs / total) * width);
            sum += blocks[n].x1;
        }
        const float scale = sum > 0.0f ? width / sum : 1.0f;
        float x = left;
        for (ProjectBlock& block : blocks)
        {
            const float w = block.x1 * scale;
            block.x0 = x;
            block.x1 = x + w;
            x += w;
        }
        return blocks;
    }

    float ProjectTimeX(const std::vector<ProjectBlock>& blocks, std::uint64_t timeMs)
    {
        if (blocks.empty())
            return 0.0f;
        for (const ProjectBlock& block : blocks)
            if (timeMs < block.startMs + block.durationMs)
                return block.x0 + (block.x1 - block.x0) * static_cast<float>(timeMs - std::min(timeMs, block.startMs)) /
                    static_cast<float>(std::max<std::uint64_t>(1, block.durationMs));
        return blocks.back().x1;
    }

    std::uint64_t ProjectTimeAtX(const std::vector<ProjectBlock>& blocks, float x)
    {
        for (const ProjectBlock& block : blocks)
            if (x < block.x1)
            {
                const float t = std::clamp((x - block.x0) / std::max(1.0f, block.x1 - block.x0), 0.0f, 1.0f);
                return block.startMs + static_cast<std::uint64_t>(t * static_cast<float>(block.durationMs));
            }
        return blocks.empty() ? 0 : blocks.back().startMs + blocks.back().durationMs;
    }

    int ProjectBlockAtX(const std::vector<ProjectBlock>& blocks, float x)
    {
        for (std::size_t n = 0; n < blocks.size(); ++n)
            if (x >= blocks[n].x0 && x < blocks[n].x1)
                return static_cast<int>(n);
        if (!blocks.empty() && x >= blocks.back().x1)
            return static_cast<int>(blocks.size()) - 1;
        return blocks.empty() ? -1 : 0;
    }

    void SendProjectIndex(CommandType type, int index, std::int32_t direction = 0, std::uint32_t target = 0)
    {
        Command command{};
        command.keyframeId = static_cast<std::uint32_t>(index);
        command.direction = direction;
        command.target = target;
        Send(type, &command);
    }

    void DrawProject(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("Project");
        const ULONGLONG nowTick = GetTickCount64();
        if (!ProjectIsOpen())
        {
            // A project asked for (new, open) shows up within a frame or two.
            InkPanel(draw, 30.0f, 30.0f, 620.0f, 110.0f, kPanel);
            const bool waiting = g_projectOpenAsked && nowTick - g_projectOpenAsked < 3000;
            Text(draw, 54.0f, 54.0f, 22.0f, kText, waiting ? TR("project.opening", "Opening the project...") : TR("project.none", "No project is open."));
            if (waiting)
                LoadingAfter(draw, 54.0f, 54.0f, 22.0f, TR("project.opening", "Opening the project..."), 600.0f);
            if (!waiting && Back())
            {
                PlayUi(UiSound::Back);
                g_screen = Screen::Home;
            }
            if (!waiting)
                g_screen = Screen::Projects;
            return;
        }
        g_projectOpenAsked = 0;
        DrawClipFade(draw);
        const int count = static_cast<int>(std::min(g_snapshot.projectClipCount, kMaxProjectClipsInSnapshot));
        g_projectIndex = count ? std::clamp(g_projectIndex, 0, count - 1) : 0;
        const bool playing = Flag(SnapshotPlaying);
        const bool previewing = ProjectFlag(ProjectPreviewing);
        const bool loading = ProjectFlag(ProjectLoadingClip);
        const int active = g_snapshot.projectActiveClip < static_cast<std::uint32_t>(count) ? static_cast<int>(g_snapshot.projectActiveClip) : -1;

        // The message of a refused edit (the core's words), for 4 s.
        if (g_snapshot.projectMessageSequence != g_projectMessageSeen)
        {
            if (g_projectMessageSeen != 0 || g_snapshot.projectMessageSequence > 0)
            {
                g_projectMessageShown = g_snapshot.projectMessage;
                g_projectMessageUntil = nowTick + 4000;
            }
            g_projectMessageSeen = g_snapshot.projectMessageSequence;
        }

        // Header: the project, its length, what plays.
        const float panelW = 640.0f;
        // The panel kPanelPad left of its lines (46), its brush and title over the
        // ink edge.
        InkPanel(draw, 46.0f - kPanelPad, 30.0f, 30.0f + panelW, 156.0f, kPanel);
        const std::string title = TRF("project.title", "PROJECT  {name}{dirty}", {{"name", g_snapshot.projectName}, {"dirty", ProjectFlag(ProjectDirty) ? " *" : ""}});
        // R renames the project: a button with its key at the header's right end.
        const char* const renameLabel = TR("keys.rename", "Rename");
        const float renameW = KeyActionWidth("R", renameLabel);
        const float renameX = 30.0f + panelW - 16.0f - renameW;
        bool renameAsked = KeyAction(draw, "##projectrename", renameX, 44.0f, "R", renameLabel);
        // A long name is cut with "..." inside the brush, clear of the button.
        const std::string shownTitle = Ellipsized(26.0f, title, std::min(panelW - 66.0f, renameX - 70.0f));
        Brush(draw, 30.0f, 36.0f, std::min(panelW - 20.0f, TextWidth(26.0f, shownTitle.c_str()) / g_scale + 40.0f), 40.0f, kRed);
        Text(draw, 44.0f, 42.0f, 26.0f, kText, shownTitle.c_str());
        char total[16]{}, now[16]{};
        FormatClock(g_snapshot.projectTotalMs, total, sizeof(total), true);
        FormatClock(g_snapshot.projectPlayheadMs, now, sizeof(now), true);
        std::string line = count == 1 ? TRF("project.length.one", "{now} / {total}   -   1 clip", {{"now", now}, {"total", total}})
                                       : TRF("project.length", "{now} / {total}   -   {count} clips", {{"now", now}, {"total", total}, {"count", std::to_string(count)}});
        TextFit(draw, 46.0f, 88.0f, 17.0f, kText, line.c_str(), panelW - 32.0f);
        if (active >= 0)
        {
            const ProjectClipInfo& clip = g_snapshot.projectClips[active];
            const char* doing = loading ? TR("project.active.opening", "Opening")
                : previewing ? (Flag(SnapshotPaused) ? TR("project.active.paused", "Paused at") : TR("project.active.playing", "Playing"))
                             : TR("project.active.editing", "Editing");
            line = TRF("project.active", "{doing} clip {at} of {count}: {clip}",
                {{"doing", doing}, {"at", std::to_string(active + 1)}, {"count", std::to_string(count)}, {"clip", ClipLabel(clip.file)}});
        }
        else
            line = KeyFill(count ? TR("project.hint.play", "{key_play} plays the project from the selected clip.")
                                 : TR("project.hint.empty", "Empty: {key_add_clip} adds a clip from your recorded clips."));
        TextFit(draw, 46.0f, 114.0f, 15.0f, kMuted, line.c_str(), panelW - 32.0f);
        UI_PROBE_BOX("endpanel", P(0.0f, 0.0f), P(0.0f, 0.0f)); // the message under the panel is not in it
        if (nowTick < g_projectMessageUntil)
            Paragraph(draw, 46.0f, 166.0f, 16.0f, kGold, g_projectMessageShown.c_str(), panelW - 30.0f);
        else if (ProjectFlag(ProjectSaveFailed))
            Paragraph(draw, 46.0f, 166.0f, 16.0f, kGold, TR("project.save_failed", "The project could not be saved."), panelW - 30.0f);

        // The timeline: the blocks over the project's time bar.
        const float left = 40.0f;
        const float right = g_vw - 40.0f;
        const float width = right - left;
        // Over the key bar.
        const float blocksTop = g_vh - g_footH - 136.0f;
        const float blocksHeight = 64.0f;
        const float barY = blocksTop + blocksHeight + 14.0f;
        draw->AddRectFilledMultiColor(P(0.0f, blocksTop - 50.0f), P(g_vw, g_vh), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
            IM_COL32(8, 7, 6, 215), IM_COL32(8, 7, 6, 215));
        const std::vector<ProjectBlock> blocks = LayoutProjectBlocks(left, width);
        const ImVec2 mouse = ImGui::GetMousePos();
        const float mouseX = mouse.x / g_scale - g_ox;

        // Blocks: interaction first (the drawing shows the drag).
        ImGui::SetCursorScreenPos(P(left, blocksTop));
        ImGui::InvisibleButton("##projectblocks", ImVec2(S(width), S(blocksHeight)));
        const bool blocksHovered = ImGui::IsItemHovered();
        const int hoveredBlock = blocksHovered ? ProjectBlockAtX(blocks, mouseX) : -1;
        if (blocksHovered && MouseClicked() && hoveredBlock >= 0)
        {
            if (hoveredBlock != g_projectIndex)
                PlayUi(UiSound::NavDown);
            g_projectIndex = hoveredBlock;
            g_dragBlock = hoveredBlock;
            g_dragBlockFromX = mouseX;
            g_dragBlockMoved = false;
        }
        if (blocksHovered && hoveredBlock >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            PlayUi(UiSound::Select);
            SendProjectIndex(CommandType::ProjectEditClip, hoveredBlock);
            g_editAfterOpen = hoveredBlock;
            g_editAfterOpenAt = nowTick;
            g_dragBlock = -1;
        }
        if (g_dragBlock >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left) && std::fabs(mouseX - g_dragBlockFromX) > 8.0f)
            g_dragBlockMoved = true;
        int dropAt = -1;
        if (g_dragBlock >= 0 && g_dragBlockMoved)
            dropAt = ProjectBlockAtX(blocks, mouseX);
        if (g_dragBlock >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (g_dragBlockMoved && dropAt >= 0 && dropAt != g_dragBlock)
            {
                PlayUi(UiSound::Select);
                SendProjectIndex(CommandType::ProjectMoveClip, g_dragBlock, 0, static_cast<std::uint32_t>(dropAt));
                g_projectIndex = dropAt;
            }
            g_dragBlock = -1;
            g_dragBlockMoved = false;
        }

        for (int n = 0; n < count; ++n)
        {
            const ProjectBlock& block = blocks[n];
            const ProjectClipInfo& clip = g_snapshot.projectClips[n];
            const bool selected = n == g_projectIndex;
            const bool missing = (clip.flags & ProjectClipMissing) != 0;
            const bool isActive = (clip.flags & ProjectClipActive) != 0;
            const float x0 = block.x0 + 1.0f;
            const float x1 = std::max(x0 + 2.0f, block.x1 - 1.0f);
            const ImU32 fill = missing ? IM_COL32(60, 20, 18, 220) : isActive ? IM_COL32(58, 50, 38, 240)
                : n == hoveredBlock ? IM_COL32(40, 36, 32, 235) : IM_COL32(24, 22, 20, 230);
            draw->AddRectFilled(P(x0, blocksTop), P(x1, blocksTop + blocksHeight), fill, S(3.0f));
            if (isActive)
                draw->AddRectFilled(P(x0, blocksTop), P(x1, blocksTop + 4.0f), kGold);
            if (selected)
                draw->AddRect(P(x0, blocksTop), P(x1, blocksTop + blocksHeight), kRed, S(3.0f), 0, S(3.0f));
            else
                draw->AddRect(P(x0, blocksTop), P(x1, blocksTop + blocksHeight), kPanelLine, S(3.0f), 0, S(1.0f));
            if (x1 - x0 > 44.0f)
            {
                draw->PushClipRect(P(x0 + 4.0f, blocksTop), P(x1 - 4.0f, blocksTop + blocksHeight), true);
                char number[8]{};
                std::snprintf(number, sizeof(number), "%d", n + 1);
                Text(draw, x0 + 7.0f, blocksTop + 8.0f, 14.0f, kMuted, number);
                Text(draw, x0 + 7.0f + TextWidth(14.0f, number) / g_scale + 8.0f, blocksTop + 8.0f, 15.0f, missing ? kMuted : kText,
                    ClipLabel(clip.file).c_str());
                char length[24]{};
                FormatClock(block.durationMs, length, sizeof(length), true);
                Text(draw, x0 + 7.0f, blocksTop + 36.0f, 14.0f, missing ? kGold : kMuted, missing ? TR("project.clip.missing", "missing") : length);
                draw->PopClipRect();
            }
            // The trim inside the clip file: a thin bar under the block.
            if (clip.rawMs > 0 && x1 - x0 > 12.0f)
            {
                const float a = x0 + (x1 - x0) * static_cast<float>(clip.inMs) / static_cast<float>(clip.rawMs);
                const float b = x0 + (x1 - x0) * static_cast<float>(clip.outMs) / static_cast<float>(clip.rawMs);
                draw->AddRectFilled(P(x0, blocksTop + blocksHeight - 5.0f), P(x1, blocksTop + blocksHeight - 3.0f), IM_COL32(236, 230, 218, 40));
                draw->AddRectFilled(P(a, blocksTop + blocksHeight - 5.0f), P(b, blocksTop + blocksHeight - 3.0f), IM_COL32(236, 230, 218, 150));
            }
        }
        if (g_dragBlock >= 0 && g_dragBlockMoved && dropAt >= 0)
        {
            // Where the held block goes: a gold line at that place.
            const float x = dropAt > g_dragBlock ? blocks[dropAt].x1 : blocks[dropAt].x0;
            draw->AddLine(P(x, blocksTop - 6.0f), P(x, blocksTop + blocksHeight + 6.0f), kGold, S(3.0f));
        }

        // The project's time bar (click or drag to move the playhead; a drag
        // over another clip's file loads it once let go).
        const std::uint64_t playhead = g_projectScrubbing && g_projectLastSeek != UINT64_MAX ? g_projectLastSeek : g_snapshot.projectPlayheadMs;
        const float playX = count ? std::clamp(ProjectTimeX(blocks, playhead), left, right) : left;
        if (TimelineArtReady()) // the torn bar (the blocks keep their look)
        {
            TornStrip(draw, ui_art::Art::BarTorn, left, left, right, barY - 3.0f, barY + 8.0f, kTrackTint);
            TornStrip(draw, ui_art::Art::BarRed, left, left, playX, barY - 3.0f, barY + 8.0f, IM_COL32(255, 255, 255, 255));
        }
        else
        {
            draw->AddRectFilled(P(left, barY), P(right, barY + 5.0f), IM_COL32(236, 230, 218, 70));
            draw->AddRectFilled(P(left, barY), P(playX, barY + 5.0f), kRed);
        }
        for (int n = 1; n < count; ++n)
            if (!SeamMark(draw, blocks[n].x0, barY - 6.0f, barY + 11.0f, width / static_cast<float>(count)))
                draw->AddLine(P(blocks[n].x0, barY - 3.0f), P(blocks[n].x0, barY + 8.0f), IM_COL32(236, 230, 218, 120), S(1.0f));
        if (count)
        {
            draw->AddLine(P(playX, blocksTop - 4.0f), P(playX, barY + 9.0f), kText, S(2.0f));
            draw->AddTriangleFilled(P(playX - 5.0f, barY + 15.0f), P(playX + 5.0f, barY + 15.0f), P(playX, barY + 8.0f), kText);
        }
        ImGui::SetCursorScreenPos(P(left, barY - 8.0f));
        ImGui::InvisibleButton("##projectbar", ImVec2(S(width), S(24.0f)));
        const bool barHovered = ImGui::IsItemHovered();
        if (count && barHovered && MouseClicked())
        {
            g_projectScrubbing = ImGui::IsMouseDown(ImGuiMouseButton_Left);
            g_projectLastSeek = UINT64_MAX;
            if (!g_projectScrubbing)
            {
                Command seek{};
                seek.timeMs = ProjectTimeAtX(blocks, mouseX);
                Send(CommandType::ProjectSeek, &seek);
            }
        }
        if (g_projectScrubbing)
        {
            const std::uint64_t at = ProjectTimeAtX(blocks, mouseX);
            const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
            if (down && at != g_projectLastSeek)
            {
                Command seek{};
                seek.timeMs = at;
                seek.enabled = kSeekDragging;
                Send(CommandType::ProjectSeek, &seek);
                g_projectLastSeek = at;
                const int under = ProjectBlockAtX(blocks, mouseX);
                if (under >= 0)
                    g_projectIndex = under;
            }
            if (!down)
            {
                Command seek{};
                seek.timeMs = g_projectLastSeek != UINT64_MAX ? g_projectLastSeek : at;
                seek.enabled = kSeekDragEnd;
                Send(CommandType::ProjectSeek, &seek);
                g_projectScrubbing = false;
            }
        }

        // Keys.
        const bool ctrl = ImGui::GetIO().KeyCtrl;
        if (count && !ctrl && KeyPressed(ImGuiKey_LeftArrow))
        {
            g_projectIndex = std::max(0, g_projectIndex - 1);
            PlayUi(UiSound::NavLeft);
        }
        if (count && !ctrl && KeyPressed(ImGuiKey_RightArrow))
        {
            g_projectIndex = std::min(count - 1, g_projectIndex + 1);
            PlayUi(UiSound::NavRight);
        }
        // Ctrl+Left / Ctrl+Right: the selected clip one place back or forward.
        if (count && ctrl && (KeyPressed(ImGuiKey_LeftArrow, false) || KeyPressed(ImGuiKey_RightArrow, false)))
        {
            const int step = KeyPressed(ImGuiKey_LeftArrow, false) ? -1 : 1;
            const int to = g_projectIndex + step;
            if (to < 0 || to >= count)
                PlayUi(UiSound::Error);
            else
            {
                SendProjectIndex(CommandType::ProjectMoveClip, g_projectIndex, 0, static_cast<std::uint32_t>(to));
                g_projectIndex = to;
                PlayUi(step < 0 ? UiSound::NavLeft : UiSound::NavRight);
            }
        }
        if (KeyPressed(ImGuiKey_Space, false) || ActionPressed(Action::PlayPause))
        {
            Command command{};
            if (previewing && playing && !Flag(SnapshotPaused))
                command.enabled = 0;
            else if (previewing && playing)
            {
                command.enabled = 1;
                command.timeMs = ~0ull;
            }
            else
            {
                command.enabled = 1;
                command.timeMs = count ? blocks[g_projectIndex].startMs : 0;
            }
            if (count)
                Send(CommandType::ProjectPlay, &command);
            else
                PlayUi(UiSound::Error);
        }
        if (count && !ctrl && Confirmed())
        {
            PlayUi(UiSound::Select);
            SendProjectIndex(CommandType::ProjectEditClip, g_projectIndex);
            g_editAfterOpen = g_projectIndex;
            g_editAfterOpenAt = nowTick;
        }
        if (!ctrl && KeyPressed(ImGuiKey_A, false))
        {
            PlayUi(UiSound::Select);
            g_galleryPicking = true;
            g_pickInsertAt = count ? static_cast<std::uint32_t>(g_projectIndex + 1) : ~0u;
            RefreshClipList();
            g_screen = Screen::Gallery;
            return;
        }
        // The edits on the clip open at the playhead: X cuts it there, I and O
        // trim its start or end to the playhead.
        const bool selectedIsActive = active >= 0 && active == g_projectIndex && playing && !loading;
        if (count && !ctrl && (KeyPressed(ImGuiKey_X, false) || KeyPressed(ImGuiKey_I, false) || KeyPressed(ImGuiKey_O, false)))
        {
            if (!selectedIsActive)
            {
                PlayUi(UiSound::Error);
                g_projectMessageShown = TR("project.message.play_first", "Play or open the selected clip first; the cut and the trim go where its playhead is.");
                g_projectMessageUntil = nowTick + 4000;
            }
            else if (KeyPressed(ImGuiKey_X, false))
            {
                PlayUi(UiSound::Select);
                SendProjectIndex(CommandType::ProjectSplitClip, g_projectIndex);
            }
            else
            {
                PlayUi(UiSound::Select);
                SendProjectIndex(CommandType::ProjectTrimClip, g_projectIndex, KeyPressed(ImGuiKey_I, false) ? -1 : 1);
            }
        }
        if (count && !ctrl && KeyPressed(ImGuiKey_D, false))
        {
            PlayUi(UiSound::Select);
            SendProjectIndex(CommandType::ProjectDuplicateClip, g_projectIndex);
        }
        if (count && KeyPressed(ImGuiKey_Delete, false))
        {
            PlayUi(UiSound::Select);
            ShowAlert(TR("project.remove.title", "Remove clip"), TRF("project.remove.body",
                "Remove clip {at} ({clip}) from the project? The clip file stays in ReplayClips; its trim and markers in this project go.",
                {{"at", std::to_string(g_projectIndex + 1)}, {"clip", ClipLabel(g_snapshot.projectClips[g_projectIndex].file)}}),
                TR("project.remove.confirm", "Remove"), CommandType::ProjectDeleteClip, std::to_string(g_projectIndex), Screen::Project);
            return;
        }
        if (renameAsked || (!ctrl && KeyPressed(ImGuiKey_R, false)))
        {
            PlayUi(UiSound::Select);
            ShowProjectName(false, g_snapshot.projectFile, g_snapshot.projectName, Screen::Project);
            return;
        }
        if (ctrl && KeyPressed(ImGuiKey_S, false))
        {
            PlayUi(UiSound::Select);
            Send(CommandType::ProjectSave);
            g_projectMessageShown = TR("project.message.saved", "Saved.");
            g_projectMessageUntil = nowTick + 2000;
        }
        // Ctrl+E: the export dialog on the whole project (each clip with its
        // trim, markers and settings, one video).
        if (ctrl && count > 0 && KeyPressed(ImGuiKey_E, false))
        {
            PlayUi(UiSound::Select);
            exportui::g_range = 2;
            exportui::g_back = Screen::Project;
            g_screen = Screen::Export;
            return;
        }
        const bool stepBack = ActionPressed(Action::FrameBack, true);
        if (playing && (stepBack || ActionPressed(Action::FrameForward, true)))
        {
            Command command{};
            command.direction = stepBack ? -1 : 1;
            Send(CommandType::StepFrame, &command);
        }
        if (ActionPressed(Action::Stop) && playing)
        {
            PlayUi(UiSound::Back);
            Send(CommandType::Stop);
        }
        if (Back())
        {
            PlayUi(UiSound::Back);
            g_screen = Screen::Home;
            g_homeIndex = kHomeEditRow;
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

    // ---------------- Playback HUD ----------------
    // Minimal HUD while a clip plays and the editor is closed: the red
    // progress bar with the cursor, the time and the keys.
    // The clip's restricted-camera spans as thin bands under a timeline bar
    // (cutscene, script camera, no control), the recording cuts as small ticks,
    // a legend of the kinds present and a tooltip.
    const char* SpanName(std::uint32_t kind)
    {
        return kind == 0 ? TR("span.cutscene", "Cutscene") : kind == 1 ? TR("span.script_camera", "Script camera") : TR("span.no_control", "No control");
    }
    const ImU32 kSpanColors[] = {IM_COL32(176, 120, 226, 230), IM_COL32(92, 152, 222, 230), IM_COL32(150, 143, 131, 230)};

    // Returns where its legend ends (legendX without one).
    float DrawSpanBands(ImDrawList* draw, float left, float width, float barBottom, float legendX, float legendY,
        double viewStart = 0.0, double viewSpan = 0.0)
    {
        UI_PROBE_SCOPE("SpanBands");
        const double duration = viewSpan > 0.0 ? viewSpan : g_snapshot.durationMs ? static_cast<double>(g_snapshot.durationMs) : 1.0;
        const ImVec2 mouse = ImGui::GetMousePos();
        bool present[3] = {};
        std::string tip;
        for (std::uint32_t n = 0; n < g_snapshot.spanCount && n < kMaxSpansInSnapshot; ++n)
        {
            const ClipSpan& span = g_snapshot.spans[n];
            if (span.kind > 2 || span.endMs <= span.startMs)
                continue;
            present[span.kind] = true;
            const float x0 = std::clamp(left + static_cast<float>((span.startMs - viewStart) / duration) * width, left, left + width);
            const float x1 = std::clamp(std::max(x0 + 2.0f, left + static_cast<float>((span.endMs - viewStart) / duration) * width), left, left + width);
            const float y0 = barBottom + 2.0f + span.kind * 3.0f;
            draw->AddRectFilled(P(x0, y0), P(x1, y0 + 2.0f), kSpanColors[span.kind]);
            if (mouse.x >= S(x0) && mouse.x <= S(x1) && mouse.y >= S(barBottom - 12.0f) && mouse.y <= S(barBottom + 14.0f))
            {
                char at[16]{}, to[16]{};
                FormatClock(span.startMs, at, sizeof(at), true);
                FormatClock(span.endMs, to, sizeof(to), true);
                tip += (tip.empty() ? "" : "\n") + TRF("span.tip", "{kind}: {from} to {to}", {{"kind", SpanName(span.kind)}, {"from", at}, {"to", to}});
            }
        }
        for (std::uint32_t n = 0; n < g_snapshot.cutCount && n < kMaxCutsInSnapshot; ++n)
        {
            const float x = left + static_cast<float>((g_snapshot.cuts[n] - viewStart) / duration) * width;
            if (x < left || x > left + width)
                continue;
            draw->AddLine(P(x, barBottom - 8.0f), P(x, barBottom + 2.0f), IM_COL32(236, 230, 218, 200), S(1.0f));
            if (std::fabs(mouse.x - S(x)) <= S(3.0f) && mouse.y >= S(barBottom - 12.0f) && mouse.y <= S(barBottom + 14.0f))
                tip += (tip.empty() ? "" : "\n") + std::string(TR("span.cut", "Cut: the recording paused here"));
        }
        if (!tip.empty())
            ImGui::SetTooltip("%s", tip.c_str());
        float x = legendX;
        for (int kind = 0; kind < 3; ++kind)
        {
            if (!present[kind])
                continue;
            draw->AddRectFilled(P(x, legendY + 7.0f), P(x + 14.0f, legendY + 10.0f), kSpanColors[kind]);
            Text(draw, x + 18.0f, legendY, 14.0f, kMuted, SpanName(static_cast<std::uint32_t>(kind)));
            x += 30.0f + TextWidth(14.0f, SpanName(static_cast<std::uint32_t>(kind))) / g_scale;
        }
        return x;
    }

    // The width DrawSpanBands' legend takes.
    float SpanLegendWidth()
    {
        bool present[3] = {};
        for (std::uint32_t n = 0; n < g_snapshot.spanCount && n < kMaxSpansInSnapshot; ++n)
        {
            const ClipSpan& span = g_snapshot.spans[n];
            if (span.kind <= 2 && span.endMs > span.startMs)
                present[span.kind] = true;
        }
        float width = 0.0f;
        for (int kind = 0; kind < 3; ++kind)
            if (present[kind])
                width += 30.0f + TextWidth(14.0f, SpanName(static_cast<std::uint32_t>(kind))) / g_scale;
        return width;
    }

    void DrawPlaybackHud(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("PlaybackHud");
        const float left = 120.0f;
        const float right = g_vw - 120.0f;
        const float y = g_vh - g_footH - 44.0f; // over the key bar
        // The project playing as a whole shows the project's time.
        const bool project = ProjectFlag(ProjectPreviewing);
        const std::uint64_t hudNow = project ? g_snapshot.projectPlayheadMs : g_snapshot.playheadMs;
        const std::uint64_t hudTotal = project ? g_snapshot.projectTotalMs : g_snapshot.durationMs;
        const float amount = hudTotal ? std::clamp(static_cast<float>(hudNow) / hudTotal, 0.0f, 1.0f) : 0.0f;
        const float x = left + (right - left) * amount;
        if (TimelineArtReady()) // the painted bar and crown
        {
            TornStrip(draw, ui_art::Art::BarTorn, left, left, right, y - 3.0f, y + 9.0f, kTrackTint);
            TornStrip(draw, ui_art::Art::BarRed, left, left, x, y - 3.0f, y + 9.0f, IM_COL32(255, 255, 255, 255));
            PlayheadIcon(draw, x, y + 11.0f, 26.0f);
        }
        else
        {
            draw->AddRectFilled(P(left, y), P(right, y + 6.0f), IM_COL32(236, 230, 218, 70));
            draw->AddRectFilled(P(left, y), P(x, y + 6.0f), kRed);
            draw->AddCircle(P(x, y - 6.0f), S(7.0f), kText, 0, S(2.0f));
            draw->AddTriangleFilled(P(x - 4.0f, y + 6.0f), P(x + 4.0f, y + 6.0f), P(x, y), kText);
        }
        char now[16]{};
        char total[16]{};
        FormatClock(hudNow, now, sizeof(now), false);
        FormatClock(hudTotal, total, sizeof(total), false);
        char time[48]{};
        std::snprintf(time, sizeof(time), "%s / %s", now, total);
        Text(draw, left, y + 16.0f, 16.0f, kText, time);
        if (project && g_snapshot.projectActiveClip < g_snapshot.projectClipCount)
        {
            // The project, the clip playing and its place.
            const std::string where = TRF("hud.project_clip", "{project}  -  clip {at} of {count}: {clip}",
                {{"project", g_snapshot.projectName}, {"at", std::to_string(g_snapshot.projectActiveClip + 1)},
                    {"count", std::to_string(g_snapshot.projectClipCount)}, {"clip", ClipLabel(g_snapshot.projectClips[g_snapshot.projectActiveClip].file)}});
            TextFit(draw, left, y - 34.0f, 16.0f, kText, where.c_str(), right - left);
            for (std::uint32_t n = 1; n < g_snapshot.projectClipCount; ++n)
            {
                // The cuts between the clips, on the bar.
                std::uint64_t at = 0;
                for (std::uint32_t k = 0; k < n; ++k)
                    at += g_snapshot.projectClips[k].outMs - g_snapshot.projectClips[k].inMs;
                const float cx = left + (right - left) * std::clamp(static_cast<float>(at) / std::max<std::uint64_t>(1, hudTotal), 0.0f, 1.0f);
                if (!SeamMark(draw, cx, y - 6.0f, y + 12.0f, (right - left) / static_cast<float>(g_snapshot.projectClipCount)))
                    draw->AddLine(P(cx, y - 3.0f), P(cx, y + 9.0f), IM_COL32(236, 230, 218, 160), S(1.0f));
                if (n > 64)
                    break;
            }
        }
        char speed[32]{};
        std::snprintf(speed, sizeof(speed), "%u%%", g_snapshot.speedPercent);
        if (g_snapshot.speedPercent != 100)
            Text(draw, left + 130.0f, y + 16.0f, 16.0f, kGold, speed);
        const bool busy = PlaybackBusy(); // the world's load behind the black: the badge (DrawLoadingBadge)
        const char* state = busy ? TR("hud.loading", "Loading") : Flag(SnapshotPaused) ? TR("hud.paused", "Paused") : nullptr;
        if (state)
            Text(draw, left + 200.0f, y + 16.0f, 16.0f, kMuted, state);
        if (busy) // the turning cylinder before "Loading"
            LoadingCue(draw, left + 188.0f, y + 16.0f + LabelHeight(16.0f) * 0.5f, 18.0f);
        DrawSpanBands(draw, left, right - left, y + 6.0f, left + 330.0f, y + 18.0f);
        // The keys are on the key bar under it (PlaybackTips).
    }

    // ---------------- Edit ----------------
    std::uint32_t g_selectedKeyframeId = 0;
    std::uint32_t g_draggingKeyframeId = 0;
    std::uint64_t g_lastDragTimeMs = UINT64_MAX;
    std::uint64_t g_dragKeyStartMs = 0; // the dragged marker's time at the click
    float g_dragStartX = 0.0f;          // the click, screen pixels
    bool g_dragMoved = false;
    bool g_draggingPlayhead = false;

    const CameraKeyframe* FindKeyframe(std::uint32_t id)
    {
        for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
            if (g_snapshot.cameraKeyframes[n].id == id)
                return &g_snapshot.cameraKeyframes[n];
        return nullptr;
    }

    // "Cut" is the Hold easing: the view jumps at the keyframe.
    const char* kEasings[] = {"Linear", "Smooth", "Ease in", "Ease out", "Cut"};
    const char* kCameraModes[] = {"Free", "Game", "Target", "Follow"};

    const char* TargetLabel(std::uint32_t id)
    {
        for (std::uint32_t n = 0; n < g_snapshot.targetCount && n < kMaxTargetsInSnapshot; ++n)
        {
            if (g_snapshot.targets[n].id == id)
                return g_snapshot.targets[n].label;
        }
        return id ? TR("target.not_in_clip", "(not in clip)") : TR("common.none", "None");
    }

    // A keyframe added here is selected once the core lists it: the one
    // nearest the playhead of the request, after the count grew.
    std::uint32_t g_keyTotalBeforeAdd = 0;
    std::uint64_t g_addAtMs = 0;
    bool g_selectAdded = false;
    ULONGLONG g_addAskedAt = 0; // an add the core did not make is said so, not left armed

    // A short message on the edit screen's status line.
    const char* g_editMessage = "";
    ULONGLONG g_editMessageUntil = 0;
    ULONGLONG g_saveAskedAt = 0; // Ctrl+S waiting for its end
    bool g_saveSeen = false;

    void EditMessage(const char* text, ULONGLONG forMs = 2500)
    {
        g_editMessage = text;
        g_editMessageUntil = GetTickCount64() + forMs;
    }

    void AddKeyframeHere()
    {
        // A marker already at the playhead (within two frames) is opened instead
        // (the core refuses a second one there).
        for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
        {
            const CameraKeyframe& key = g_snapshot.cameraKeyframes[n];
            const std::uint64_t d = key.timeMs > g_snapshot.playheadMs ? key.timeMs - g_snapshot.playheadMs : g_snapshot.playheadMs - key.timeMs;
            if (d <= 66)
            {
                g_selectedKeyframeId = key.id;
                PlayUi(UiSound::NavDown);
                Command seek{}; // onto it, so its menu stays open
                seek.timeMs = key.timeMs;
                Send(CommandType::SeekAbsolute, &seek);
                return;
            }
        }
        if (Send(CommandType::CaptureCameraKeyframe))
        {
            PlayUi(UiSound::Select);
            g_keyTotalBeforeAdd = g_snapshot.cameraKeyframeTotal;
            g_addAtMs = g_snapshot.playheadMs;
            g_selectAdded = true;
            g_addAskedAt = GetTickCount64();
        }
    }

    void SelectAddedKeyframe()
    {
        // The core refused the add (512 markers, no room between two close ones):
        // say so, and do not leave the selection armed for the next change of the
        // count (an undo, another clip's bounds).
        if (g_selectAdded && g_snapshot.cameraKeyframeTotal <= g_keyTotalBeforeAdd && GetTickCount64() - g_addAskedAt > 1500)
        {
            g_selectAdded = false;
            PlayUi(UiSound::Error);
            EditMessage(TR("edit.message.no_room", "No room for a marker here"));
            return;
        }
        if (!g_selectAdded || g_snapshot.cameraKeyframeTotal <= g_keyTotalBeforeAdd)
            return;
        g_selectAdded = false;
        std::uint64_t best = ~0ull;
        for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
        {
            const CameraKeyframe& key = g_snapshot.cameraKeyframes[n];
            const std::uint64_t distance = key.timeMs > g_addAtMs ? key.timeMs - g_addAtMs : g_addAtMs - key.timeMs;
            if (distance < best)
            {
                best = distance;
                g_selectedKeyframeId = key.id;
            }
        }
    }

    const char* kPaths[] = {"Straight", "Curved"};
    const int kSpeeds[] = {5, 20, 35, 50, 100, 125, 150, 175, 200};

    // A panel row under the mouse takes Left and Right this frame (only such a
    // row: not any hovered item, such as the timeline where the mouse rests after
    // a click).
    bool g_valueRowHovered = false;

    // The side panel's content (the panel stands kPanelPad around it) and its
    // rows.
    constexpr float kEditX = 40.0f;
    constexpr float kEditW = 300.0f;
    constexpr float kTypeErrorH = 20.0f; // the line a typed number's refusal takes under its row

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

    // "Label   < value >" row of the side panel; the arrows or a click
    // change the value. Returns -1, 0 or +1.
    int ValueRow(ImDrawList* draw, const char* id, float y, const char* label, const char* value, bool enabled, bool repeat = true,
        float lead = 0.0f, ui_art::Art icon = ui_art::Art::Count)
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

    // ---------------- Marker menus ----------------
    // A marker menu with Camera Options and Depth of Field submenus; up/down
    // picks a row, left/right changes it (PageUp/PageDown fast), Enter opens or
    // runs, Esc goes back; the focused row's help at the foot.
    enum class MarkerMenu { Root, Camera, Dof, Effects, Position, Tools, Spline, SplineDefaults, Shake, ShakeAdvanced };
    int g_menuReturnFocus = 0; // the root row a submenu was opened from
    MarkerMenu g_markerMenu = MarkerMenu::Root;
    int g_menuFocus = 0;
    std::uint32_t g_menuMarker = 0;

    struct MenuRow
    {
        std::string label;
        std::string value;
        bool enabled = true;
        int kind = 0; // 0 value, 1 submenu, 2 action, 3 a list line
        std::function<void(int)> step;
        std::function<void()> enter;
        std::string help;
        // Enter types the number (the Spline rows).
        std::function<void(double)> typed;
        std::string typeStart;
        float typeMin = 0.0f;
        float typeMax = 0.0f;
        ui_art::Art icon = ui_art::Art::Count; // the tool's icon before the label (none)
    };

    // The icons' column before a panel row's label (0 without the tools' icons).
    float EditIconLead()
    {
        using ui_art::Art;
        return AnyArt({Art::ToolCamera, Art::ToolDof, Art::ToolEffects, Art::ToolShake, Art::ToolWeather}) ? 28.0f : 0.0f;
    }

    // The Spline tuning's values last sent (a marker's, by id; the defaults, key
    // 1), the row the Spline Defaults were opened from, and the typed number over
    // a row.
    replay::editor::input::PendingValue<SplineTuning> g_pendingSpline;
    // A marker's bones, attach pose numbers, near clip and DOF numbers last
    // sent.
    replay::editor::input::PendingValue<CameraMarkerExtra> g_pendingExtra;
    // A marker's shake last sent.
    replay::editor::input::PendingValue<MarkerShake> g_pendingShake;
    replay::editor::input::PendingValue<SplineTuning> g_pendingSplineDefaults;
    int g_splineMenuReturn = 0;     // the Camera menu's Spline Options row
    int g_splineDefaultsReturn = 0; // the Spline menu's Spline Defaults row
    struct RowTyping
    {
        bool on = false;
        std::string label;
        char text[24]{};
        bool focus = false;
        const char* error = "";
    };
    RowTyping g_rowTyping;
    // A typed box had the keys this frame (DrawEdit's keys skip it).
    bool g_fxTypingFrame = false;

    CameraKeyframeOptions OptionsOf(const CameraKeyframe& k)
    {
        CameraKeyframeOptions o{};
        o.cameraMode = k.cameraMode;
        o.lookAtTarget = k.lookAtTarget;
        o.attachTarget = k.attachTarget;
        o.focusTarget = k.focusTarget;
        o.lookAtOffset = k.lookAtOffset;
        o.lookAtRoll = k.lookAtRoll;
        o.presetDistanceScale = k.presetDistanceScale;
        o.fov = k.camera.fov;
        o.dofIntensity = k.dofIntensity;
        o.focusDistance = k.focusDistance;
        o.markerType = k.markerType;
        o.blend = k.blend;
        o.ease = k.ease;
        o.attachType = k.attachType;
        o.shake = k.shake;
        o.shakeIntensity = k.shakeIntensity;
        o.shakeSpeedIndex = k.shakeSpeedIndex;
        o.shakeMotion = k.shakeMotion;
        o.speedIndex = k.speedIndex;
        o.dof = k.dof;
        o.focus = k.focus;
        o.transition = k.transition;
        o.transitionMs = k.transitionMs;
        o.filter = k.filter;
        o.filterIntensity = k.filterIntensity;
        o.saturation = k.saturation;
        o.contrast = k.contrast;
        o.vignette = k.vignette;
        o.brightness = k.brightness;
        return o;
    }

    // The values last sent, stepped from while the snapshot has not brought them
    // back (editor_input.h): otherwise a held arrow or a quick double press on a
    // marker option, the Time row or the playback speed sends the same value
    // twice and the second press is lost.
    replay::editor::input::PendingValue<CameraKeyframeOptions> g_pendingOptions;
    replay::editor::input::PendingValue<std::uint64_t> g_pendingMoveTime;
    replay::editor::input::PendingValue<std::uint32_t> g_pendingSpeed;

    // Any other change to a marker (paste, reset, undo, a drag) makes the
    // snapshot the only truth again.
    void ForgetPendingMarkerValues()
    {
        g_pendingOptions.Clear();
        g_pendingMoveTime.Clear();
        g_pendingSpline.Clear();
        g_pendingExtra.Clear();
        g_pendingShake.Clear();
    }

    void SendOptions(const CameraKeyframe& k, std::uint32_t mask, const std::function<void(CameraKeyframeOptions&)>& change)
    {
        Command command{};
        command.keyframeId = k.id;
        const ULONGLONG now = GetTickCount64();
        command.options = g_pendingOptions.Base(k.id, OptionsOf(k), now);
        change(command.options);
        command.options.mask = mask;
        if (Send(CommandType::SetCameraKeyframeOptions, &command))
            g_pendingOptions.Sent(k.id, command.options, now);
    }

    // The playback speed preset one step from the one last asked.
    void StepPlaybackSpeed(int step)
    {
        const int count = static_cast<int>(sizeof(kSpeeds) / sizeof(kSpeeds[0]));
        const ULONGLONG now = GetTickCount64();
        const int from = static_cast<int>(g_pendingSpeed.Base(0, g_snapshot.speedIndex, now));
        Command command{};
        command.enabled = static_cast<std::uint32_t>(std::clamp(from + step, 0, count - 1));
        if (static_cast<int>(command.enabled) == from)
        {
            PlayUi(UiSound::Error); // already the slowest or the fastest
            return;
        }
        PlayUiStep(step);
        if (Send(CommandType::SetPlaybackSpeed, &command))
            g_pendingSpeed.Sent(0, command.enabled, now);
    }

    // Rewind / FastForward in the editor, a seek from the time last asked (the
    // snapshot lags a frame or two behind each seek).
    constexpr std::int64_t kRewindStepMs = 500;
    replay::editor::input::PendingValue<std::uint64_t> g_pendingSeek;

    void SeekBy(std::int64_t deltaMs)
    {
        const ULONGLONG now = GetTickCount64();
        const std::int64_t from = static_cast<std::int64_t>(g_pendingSeek.Base(0, g_snapshot.playheadMs, now));
        const std::int64_t end = static_cast<std::int64_t>(g_snapshot.durationMs);
        Command seek{};
        seek.timeMs = static_cast<std::uint64_t>(std::clamp<std::int64_t>(from + deltaMs, 0, end));
        if (static_cast<std::int64_t>(seek.timeMs) == from)
            return; // at the start or the end
        if (Send(CommandType::SeekAbsolute, &seek))
            g_pendingSeek.Sent(0, seek.timeMs, now);
    }

    // Delete (the menu's row and the Delete key): the clip's start and end stay
    // (an error sound, the core would refuse them too), and the focus goes to the
    // nearest marker left instead of closing the menu.
    void DeleteMarker(std::uint32_t id)
    {
        const std::uint32_t count = g_snapshot.cameraKeyframeCount;
        std::uint32_t index = count;
        for (std::uint32_t n = 0; n < count; ++n)
            if (g_snapshot.cameraKeyframes[n].id == id)
                index = n;
        if (index >= count)
            return;
        if (count >= 2 && (index == 0 || index + 1 == count))
        {
            PlayUi(UiSound::Error);
            return;
        }
        Command c{};
        c.keyframeId = id;
        if (!Send(CommandType::DeleteCameraKeyframe, &c))
            return;
        ForgetPendingMarkerValues();
        PlayUi(UiSound::Back);
        const std::uint64_t at = g_snapshot.cameraKeyframes[index].timeMs;
        std::uint64_t nearest = ~0ull;
        g_selectedKeyframeId = 0;
        for (std::uint32_t n = 0; n < count; ++n)
        {
            if (n == index)
                continue;
            const std::uint64_t t = g_snapshot.cameraKeyframes[n].timeMs;
            const std::uint64_t distance = t > at ? t - at : at - t;
            if (distance < nearest)
            {
                nearest = distance;
                g_selectedKeyframeId = g_snapshot.cameraKeyframes[n].id;
            }
        }
        // The playhead goes to the marker selected.
        if (const CameraKeyframe* next = FindKeyframe(g_selectedKeyframeId))
        {
            Command seek{};
            seek.timeMs = next->timeMs;
            Send(CommandType::SeekAbsolute, &seek);
        }
    }

    // The camera list order: the presets, the game camera, free.
    const CameraMode kCameraCycle[] = {CameraMode::FrontLow, CameraMode::Rear, CameraMode::RightSide, CameraMode::LeftSide,
        CameraMode::Overhead, CameraMode::Game, CameraMode::Free};
    const char* CameraName(CameraMode mode)
    {
        switch (mode)
        {
        case CameraMode::Game: return TR("camera.game", "Game Camera");
        case CameraMode::FrontLow: return TR("camera.front_low", "Front Low");
        case CameraMode::Rear: return TR("camera.rear", "Rear");
        case CameraMode::RightSide: return TR("camera.right_side", "Right Side");
        case CameraMode::LeftSide: return TR("camera.left_side", "Left Side");
        case CameraMode::Overhead: return TR("camera.overhead", "Overhead");
        default: return TR("camera.free", "Free Camera");
        }
    }
    bool IsPreset(CameraMode mode) { return mode >= CameraMode::FrontLow && mode <= CameraMode::Overhead; }
    // A list of choice names (TR_NOOP entries) and one of them in the player's language.
    struct NameText
    {
        const char* key;
        const char* english;
    };
    template <std::size_t N>
    const char* NameOf(const NameText (&names)[N], int index)
    {
        const NameText& name = names[std::clamp(index, 0, static_cast<int>(N) - 1)];
        return TR(name.key, name.english);
    }
    const NameText kBlendNames[] = {{TR_NOOP("marker.blend.none", "No Blend")}, {TR_NOOP("marker.blend.linear", "Linear")},
        {TR_NOOP("marker.blend.smooth", "Smooth")}, {TR_NOOP("marker.blend.spline", "Spline")}};
    const NameText kAttachNames[] = {{TR_NOOP("marker.mount.rigid", "Rigid")}, {TR_NOOP("marker.mount.default", "Default")},
        {TR_NOOP("marker.mount.flexible", "Flexible")}};
    const NameText kShakeNames[] = {{TR_NOOP("common.none", "None")}, {TR_NOOP("shake.handheld", "Handheld")}, {TR_NOOP("shake.drunk", "Drunk")},
        {TR_NOOP("shake.ground", "Ground Vibration")}, {TR_NOOP("shake.air", "Air Turbulence")}, {TR_NOOP("shake.explosion", "Explosion")},
        {TR_NOOP("shake.walking", "Walking")}, {TR_NOOP("shake.running", "Running")}, {TR_NOOP("shake.horseback", "Horseback")},
        {TR_NOOP("shake.steadicam", "Steadicam")}, {TR_NOOP("shake.wagon", "Wagon")}, {TR_NOOP("shake.custom", "Custom")},
        {TR_NOOP("shake.gunshot", "Gunshot")}};
    const NameText kDofNames[] = {{TR_NOOP("marker.dof.default", "Default")}, {TR_NOOP("marker.dof.custom", "Custom")}, {TR_NOOP("common.none", "None")}};
    const NameText kFocusNames[] = {{TR_NOOP("marker.focus.auto", "Auto")}, {TR_NOOP("marker.focus.manual", "Manual")}, {TR_NOOP("marker.focus.target", "Target")}};
    const NameText kAnchorNames[] = {{TR_NOOP("common.off_cap", "Off")}, {TR_NOOP("common.on_cap", "On")}, {TR_NOOP("marker.anchor.exclusive", "Exclusive")}};

    template <typename E>
    E Cycle(E value, int step, int count)
    {
        return static_cast<E>((static_cast<int>(value) + (step > 0 ? 1 : -1) + count) % count);
    }

    // The next target (or None) after `id`, +1 or -1.
    std::uint32_t StepTargetOrNone(std::uint32_t id, int step)
    {
        const int count = static_cast<int>(std::min(g_snapshot.targetCount, kMaxTargetsInSnapshot));
        int index = -1; // -1 = None
        for (int n = 0; n < count; ++n)
            if (g_snapshot.targets[n].id == id)
                index = n;
        index = (index + 1 + (step > 0 ? 1 : -1) + count + 1) % (count + 1) - 1;
        return index < 0 ? 0 : g_snapshot.targets[index].id;
    }

    std::string FormatValue(const char* format, double value)
    {
        char text[48]{};
        std::snprintf(text, sizeof(text), format, value);
        return text;
    }

    // ---------------- Camera tools ----------------
    // The marker's numbers (Position), the move's tools (Move Tools) and the
    // free camera's options with the world markers (FREE CAMERA).

    struct PendingPose
    {
        std::uint32_t id = 0;
        CameraState camera{};
        ULONGLONG at = 0;
    };
    PendingPose g_pendingPose;

    // The pose through UpdateCameraKeyframe, stepped from the value last
    // sent while the snapshot has not brought it back (a held arrow).
    void SendPose(const CameraKeyframe& k, const std::function<void(CameraState&)>& change)
    {
        const ULONGLONG now = GetTickCount64();
        CameraState camera = g_pendingPose.id == k.id && now - g_pendingPose.at < 700 ? g_pendingPose.camera : k.camera;
        change(camera);
        Command command{};
        command.keyframeId = k.id;
        command.camera = camera;
        command.easing = k.easing;
        command.path = k.path;
        if (Send(CommandType::UpdateCameraKeyframe, &command))
            g_pendingPose = {k.id, camera, now};
    }

    float WrapAngle(float degrees)
    {
        while (degrees > 180.0f)
            degrees -= 360.0f;
        while (degrees <= -180.0f)
            degrees += 360.0f;
        return degrees;
    }

    std::vector<MenuRow> BuildPositionRows(const CameraKeyframe& k)
    {
        std::vector<MenuRow> rows;
        const auto metres = [&](const char* label, float Vec3::*axis) {
            MenuRow row{label, FormatValue("%.2f m", k.camera.position.*axis), true, 0};
            row.step = [k, axis](int step) {
                SendPose(k, [&](CameraState& c) { c.position.*axis += static_cast<float>(step) * 0.1f; });
            };
            row.help = KeyFill(TR("position.metres.help", "0.1 m a step ({key_page_up}/{key_page_down}: 0.5 m)."));
            rows.push_back(row);
        };
        metres("X", &Vec3::x);
        metres("Y", &Vec3::y);
        metres(TR("position.z", "Z (height)"), &Vec3::z);
        MenuRow pitch{TR("position.pitch", "Pitch"), FormatValue("%.0f", k.camera.rotation.x), true, 0};
        pitch.step = [k](int step) { SendPose(k, [&](CameraState& c) { c.rotation.x = std::clamp(c.rotation.x + step, -89.0f, 89.0f); }); };
        pitch.help = TR("position.pitch.help", "Up and down, one degree a step.");
        rows.push_back(pitch);
        MenuRow heading{TR("position.heading", "Heading"), FormatValue("%.0f", k.camera.rotation.z), true, 0};
        heading.step = [k](int step) { SendPose(k, [&](CameraState& c) { c.rotation.z = WrapAngle(c.rotation.z - step); }); };
        heading.help = TR("position.heading.help", "Left and right, one degree a step.");
        rows.push_back(heading);
        MenuRow roll{TR("position.roll", "Roll"), FormatValue("%.0f", k.camera.rotation.y), true, 0};
        roll.step = [k](int step) { SendPose(k, [&](CameraState& c) { c.rotation.y = WrapAngle(c.rotation.y + step); }); };
        roll.help = TR("position.roll.help", "Tilt the picture, one degree a step.");
        rows.push_back(roll);
        return rows;
    }

    int g_stretchPercent = 100;
    std::uint32_t g_lockTarget = 0;
    AttachType g_lockType = AttachType::Default;

    std::vector<MenuRow> BuildToolRows(const CameraKeyframe& k)
    {
        std::vector<MenuRow> rows;
        const bool playing = Flag(SnapshotPlaying);
        auto action = [&](const char* label, std::function<void()> run, const char* help, bool enabled = true) {
            MenuRow row{label, "", enabled && playing, 2};
            row.enter = std::move(run);
            row.help = help;
            rows.push_back(row);
        };
        const auto send = [](CommandType type, std::uint32_t id, std::uint32_t enabled, std::uint32_t target = 0) {
            Command c{};
            c.keyframeId = id;
            c.enabled = enabled;
            c.target = target;
            c.options.attachType = g_lockType;
            ForgetPendingMarkerValues();
            Send(type, &c);
        };
        action(TR("tools.duplicate", "Duplicate"), [k, send] { send(CommandType::DuplicateCameraKeyframe, k.id, 0); },
            TR("tools.duplicate.help", "A copy halfway to the next marker (2 s on after the last): a new point to shape the curve."));
        action(TR("tools.even_speed", "Even Out Speed"), [k, send] { send(CommandType::DistributeCameraKeyframes, k.id, 1); },
            TR("tools.even_speed.help", "The move's inner markers retimed by distance: the camera travels at one speed. The first and last keep their times."));
        action(TR("tools.even_times", "Even Out Times"), [k, send] { send(CommandType::DistributeCameraKeyframes, k.id, 0); },
            TR("tools.even_times.help", "The move's inner markers at equal times between its first and last."));
        MenuRow stretch{TR("tools.stretch", "Stretch Move"), FormatValue("%.0f%%", g_stretchPercent), playing, 0};
        stretch.step = [](int step) { g_stretchPercent = std::clamp(g_stretchPercent + (step > 0 ? 5 : -5) * (std::abs(step) > 1 ? 5 : 1), 25, 400); };
        stretch.enter = [k, send] {
            if (g_stretchPercent != 100)
                send(CommandType::StretchCameraKeyframes, k.id, static_cast<std::uint32_t>(g_stretchPercent));
            g_stretchPercent = 100;
        };
        stretch.help = KeyFill(TR("tools.stretch.help", "{key_select} applies: the move plays over this share of its time (over 100% slower). The action is "
                       "not touched; markers after the move move only when the spacing needs it."));
        rows.push_back(stretch);
        if (!g_lockTarget && g_snapshot.targetCount > 0)
            g_lockTarget = k.attachTarget ? k.attachTarget : g_snapshot.targets[0].id;
        MenuRow target{TR("tools.lock_target", "Lock Target"), TargetLabel(g_lockTarget), playing, 0};
        target.step = [](int step) { g_lockTarget = StepTargetOrNone(g_lockTarget, step); };
        target.help = TR("tools.lock_target.help", "The character, horse or vehicle the move locks to.");
        rows.push_back(target);
        MenuRow mount{TR("tools.lock_mount", "Lock Mount"), NameOf(kAttachNames, std::clamp(static_cast<int>(g_lockType), 0, 2)), playing, 0};
        mount.step = [](int step) { g_lockType = Cycle(g_lockType, step, 3); };
        mount.help = TR("tools.lock_mount.help", "Rigid turns with the target, Default follows its heading, Flexible only its position.");
        rows.push_back(mount);
        action(TR("tools.lock_move", "Lock This Move"), [k, send] { send(CommandType::LockCameraKeyframes, k.id, 0, g_lockTarget); },
            TR("tools.lock_move.help", "Every marker of the move rides the target, each keeping the view it shows now."), g_lockTarget != 0);
        action(TR("tools.lock_all", "Lock Every Marker"), [k, send] { send(CommandType::LockCameraKeyframes, k.id, 1, g_lockTarget); },
            TR("tools.lock_all.help", "Every free marker of the clip rides the target, each keeping its view."), g_lockTarget != 0);
        action(TR("tools.unlock", "Unlock This Move"), [k, send] { send(CommandType::LockCameraKeyframes, k.id, 0, 0); },
            TR("tools.unlock.help", "The move's markers drop their target and stay where they show now."));
        return rows;
    }

    bool g_freeCamMenuOpen = false;
    bool g_splineDefaultsOpen = false; // the Spline Defaults, opened from the free camera's menu
    struct PendingFreeCam
    {
        float value = 0.0f;
        ULONGLONG at = 0;
    };
    PendingFreeCam g_pendingFreeCam[static_cast<std::size_t>(FreeCamField::Count)];

    float FreeCamShown(FreeCamField field)
    {
        const std::size_t index = static_cast<std::size_t>(field);
        const PendingFreeCam& pending = g_pendingFreeCam[index];
        return pending.at && GetTickCount64() - pending.at < 700 ? pending.value : g_snapshot.freeCamValues[index];
    }

    void SendFreeCam(FreeCamField field, float value)
    {
        Command c{};
        c.keyframeId = static_cast<std::uint32_t>(field);
        c.camera.fov = value;
        if (Send(CommandType::SetFreeCamOption, &c))
            g_pendingFreeCam[static_cast<std::size_t>(field)] = {value, GetTickCount64()};
    }

    void SendFreeCamAction(FreeCamActionId action, std::uint32_t target = 0)
    {
        Command c{};
        c.keyframeId = static_cast<std::uint32_t>(action);
        c.target = target;
        Send(CommandType::FreeCamAction, &c);
    }

    void SendOverlay(OverlayField field, bool on)
    {
        Command c{};
        c.keyframeId = static_cast<std::uint32_t>(field);
        c.enabled = on ? 1u : 0u;
        Send(CommandType::SetCameraOverlay, &c);
    }

    std::vector<MenuRow> BuildFreeCamRows()
    {
        std::vector<MenuRow> rows;
        const bool flying = Flag(SnapshotFreeCamEnabled);
        const auto toggle = [&](const char* label, FreeCamField field, const char* on, const char* off, const char* help) {
            const bool value = FreeCamShown(field) >= 0.5f;
            MenuRow row{label, value ? on : off, true, 0};
            row.step = [field](int) { SendFreeCam(field, FreeCamShown(field) >= 0.5f ? 0.0f : 1.0f); };
            row.help = help;
            rows.push_back(row);
        };
        const auto number = [&](const char* label, FreeCamField field, const char* format, float delta, float lo, float hi,
                                const char* help) {
            MenuRow row{label, FormatValue(format, FreeCamShown(field)), true, 0};
            row.step = [field, delta, lo, hi](int step) {
                SendFreeCam(field, std::clamp(FreeCamShown(field) + static_cast<float>(step) * delta, lo, hi));
            };
            row.help = help;
            rows.push_back(row);
        };
        const bool drone = FreeCamShown(FreeCamField::Motion) >= 0.5f;
        toggle(TR("freecam.flight", "Flight"), FreeCamField::Motion, TR("freecam.flight.drone", "Drone"), TR("freecam.flight.classic", "Classic"), "");
        rows.back().help = KeyFill(TR("freecam.flight.help", "Drone: the camera has weight (it speeds up, coasts and banks into turns). Classic: it goes where you push "
                                      "it. ({key_drone})"));
        toggle(TR("freecam.acrobatic", "Acrobatic"), FreeCamField::Acrobatic, TR("common.on_cap", "On"), TR("common.off_cap", "Off"), "");
        rows.back().help = KeyFill(TR("freecam.acrobatic.help", "Turns about the camera's own axes: full rolls and loops. ({key_acrobatic})"));
        toggle(TR("freecam.walk", "Walk"), FreeCamField::Walk, TR("common.on_cap", "On"), TR("common.off_cap", "Off"), "");
        rows.back().help = KeyFill(TR("freecam.walk.help", "The camera keeps to the ground at eye height. ({key_walk})"));
        if (FreeCamShown(FreeCamField::Walk) >= 0.5f)
            number(TR("freecam.eye_height", "Eye Height"), FreeCamField::WalkHeight, "%.1f m", 0.1f, 0.3f, 5.0f, TR("freecam.eye_height.help", "Above the ground, 0.1 m a step."));
        toggle(TR("freecam.collision", "Collision"), FreeCamField::Collision, TR("common.on_cap", "On"), TR("common.off_cap", "Off"), "");
        rows.back().help = KeyFill(TR("freecam.collision.help", "The camera stops at walls and the ground. ({key_collision})"));
        toggle(TR("freecam.altitude", "Altitude"), FreeCamField::LockAltitude, TR("freecam.altitude.locked", "Locked"), TR("freecam.altitude.free", "Free"), "");
        rows.back().help = KeyFill(TR("freecam.altitude.help", "Locked: moving forward keeps the height ({key_fly_down} and {key_fly_up} still go down and "
                                      "up). ({key_altitude})"));
        MenuRow target{TR("freecam.target", "Target"), g_snapshot.freeCamTarget ? TargetLabel(g_snapshot.freeCamTarget) : TR("common.none", "None"), true, 0};
        target.step = [](int step) { SendFreeCamAction(FreeCamActionId::SetTarget, StepTargetOrNone(g_snapshot.freeCamTarget, step)); };
        target.help = KeyFill(TR("freecam.target.help", "Who the camera follows, and whose recorded path is drawn. ({key_pick}) picks the one under the crosshair."));
        rows.push_back(target);
        const char* const kFollowNames[] = {TR("common.off_cap", "Off"), TR("freecam.follow.rigid", "Rigid"), TR("freecam.follow.position", "Position Only")};
        MenuRow follow{TR("freecam.follow", "Follow"), kFollowNames[std::min<std::uint32_t>(g_snapshot.freeCamFollow, 2)], g_snapshot.freeCamTarget != 0, 0};
        follow.step = [](int step) {
            SendFreeCam(FreeCamField::Follow, static_cast<float>((g_snapshot.freeCamFollow + (step > 0 ? 1 : 2)) % 3));
        };
        follow.help = KeyFill(TR("freecam.follow.help", "Rigid: the camera rides the target and turns with it. Position Only: it moves with it and keeps its own view. ({key_follow})"));
        rows.push_back(follow);
        auto action = [&](const char* label, FreeCamActionId id, std::string help, bool enabled) {
            MenuRow row{label, "", enabled, 2};
            row.enter = [id] { SendFreeCamAction(id); };
            row.help = std::move(help);
            rows.push_back(row);
        };
        action(TR("freecam.pick_aimed", "Target Under Crosshair"), FreeCamActionId::PickAimed, KeyFill(TR("freecam.pick_aimed.help", "The character, horse or vehicle at the centre of the view (free camera on). ({key_pick})")), flying);
        action(TR("freecam.pick_nearest", "Nearest Target"), FreeCamActionId::PickNearest, KeyFill(TR("freecam.pick_nearest.help", "The nearest one to the camera. ({key_pick_nearest})")), flying);
        action(TR("freecam.level", "Level Horizon"), FreeCamActionId::LevelHorizon, KeyFill(TR("freecam.level.help", "Roll back to level. ({key_level})")), flying);
        action(TR("freecam.behind", "Behind The Character"), FreeCamActionId::ShotBehindMain, KeyFill(TR("freecam.behind.help", "The camera behind the target, or the main character. ({key_behind})")), flying);
        action((g_snapshot.freeCamFlags & FreeCamLocked) ? TR("freecam.release", "Release Camera") : TR("freecam.hold", "Hold Camera Still"), FreeCamActionId::ToggleLock,
            KeyFill(TR("freecam.hold.help", "The camera ignores the keys and the mouse (it still follows its target). ({key_hold})")), flying);
        if (drone)
        {
            number(TR("freecam.drag", "Drag"), FreeCamField::DroneDrag, "%.1f", 0.2f, 0.2f, 12.0f, TR("freecam.drag.help", "How fast it coasts to a stop (higher: sooner)."));
            number(TR("freecam.acceleration", "Acceleration"), FreeCamField::DroneAcceleration, "%.0f", 1.0f, 2.0f, 60.0f, TR("freecam.acceleration.help", "How hard it pushes (top speed is acceleration x 2 / drag)."));
            number(TR("freecam.gravity", "Gravity"), FreeCamField::DroneGravity, "%.1f", 0.5f, 0.0f, 20.0f, TR("freecam.gravity.help", "A pull down while it does not climb (0: none)."));
            number(TR("freecam.banking", "Banking"), FreeCamField::DroneBanking, "%.0f", 1.0f, 0.0f, 45.0f, TR("freecam.banking.help", "How much it rolls into a turn."));
            number(TR("freecam.turn_smoothing", "Turn Smoothing"), FreeCamField::DroneRotSmoothing, "%.0f", 1.0f, 1.0f, 30.0f, TR("freecam.turn_smoothing.help", "Lower: heavier, slower turns."));
            number(TR("freecam.zoom_smoothing", "Zoom Smoothing"), FreeCamField::DroneFovSmoothing, "%.0f", 1.0f, 0.0f, 20.0f, TR("freecam.zoom_smoothing.help", "Lower: softer zoom (0: none)."));
        }
        number(TR("freecam.look", "Look Sensitivity"), FreeCamField::LookSensitivity, "%.0f", 1.0f, 2.0f, 30.0f, TR("freecam.look.help", "The mouse's turn."));
        // The flying free camera's near clip (Edit Camera uses the marker's).
        number(TR("freecam.near_clip", "Near Clip"), FreeCamField::NearClip, "%.2f m", 0.01f, 0.01f, 1.0f,
            TR("freecam.near_clip.help", "How close the camera gets before things are cut. Lower gets closer; under about 0.05 m far edges can flicker. "
            "Edit Camera uses the marker's Near Clip."));
        MenuRow reset{TR("freecam.reset", "Reset Free Camera"), "", true, 2};
        reset.enter = [] { SendFreeCam(FreeCamField::ResetDefaults, 0.0f); };
        reset.help = TR("freecam.reset.help", "Every option above back to its default.");
        rows.push_back(reset);
        const auto overlay = [&](const char* label, OverlayField field, std::uint32_t flag, const char* help) {
            const bool on = (g_snapshot.overlayFlags & flag) != 0;
            MenuRow row{label, on ? TR("common.on_cap", "On") : TR("common.off_cap", "Off"), true, 0};
            row.step = [field, on](int) { SendOverlay(field, !on); };
            row.help = help;
            rows.push_back(row);
        };
        overlay(TR("overlay.world", "World Markers"), OverlayField::Enabled, OverlayEnabled, "");
        rows.back().help = KeyFill(TR("overlay.world.help", "The markers, their look arrows and the camera's path drawn in the world (never in an export). ({key_overlay})"));
        overlay(TR("overlay.while_playing", "Markers While Playing"), OverlayField::WhilePlaying, OverlayWhilePlaying,
            TR("overlay.while_playing.help", "Off: shown when paused, in Edit Camera and with the free camera. On: also, faint, while the clip plays."));
        overlay(TR("overlay.time_labels", "Time Labels"), OverlayField::TimeLabels, OverlayTimeLabels, TR("overlay.time_labels.help", "The seconds along the path and the target's trail."));
        overlay(TR("overlay.trail", "Target Trail"), OverlayField::TargetTrail, OverlayTargetTrail, TR("overlay.trail.help", "The free camera target's recorded path, 4 s back and 8 s ahead."));
        // The Spline blend's defaults (also from a marker's Spline Options).
        MenuRow spline{TR("spline.defaults", "Spline Defaults"), ">", true, 1};
        spline.enter = [] {
            g_splineDefaultsOpen = true;
            g_menuFocus = 0;
        };
        spline.help = TR("spline.defaults.help", "Speed profile, curve shape, ease, camera weight and channels every Spline marker uses unless it sets its own.");
        rows.push_back(spline);
        return rows;
    }

    // ---------------- The marker's shake ----------------
    // The shake lives in the marker (older shake events are folded into the
    // markers): Shake (off, a realistic profile or Simple Camera's noise),
    // Intensity, Frequency, Transition (Cut or Blend to the next marker) and the
    // Advanced numbers. The values last sent show while the snapshot lags
    // (PendingValue, as the other marker rows).
    int g_shakeAdvancedReturn = 0; // the Shake menu's Advanced row

    MarkerShake MarkerShakeOf(const CameraKeyframe& k)
    {
        for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
            if (g_snapshot.cameraKeyframes[n].id == k.id)
                return g_pendingShake.Base(k.id, g_snapshot.cameraKeyframeShake[n], GetTickCount64());
        return DefaultMarkerShake();
    }

    void SendMarkerShake(const CameraKeyframe& k, const std::function<void(MarkerShake&)>& edit)
    {
        MarkerShake s = MarkerShakeOf(k);
        edit(s);
        Command command{};
        command.keyframeId = k.id;
        const ULONGLONG now = GetTickCount64();
        command.options = g_pendingOptions.Base(k.id, OptionsOf(k), now);
        command.options.mask = CameraOptionShake;
        command.shake = s;
        if (Send(CommandType::SetCameraKeyframeOptions, &command))
            g_pendingShake.Sent(k.id, s, now);
    }

    // Intensity 1..10 and its gain ((i + 1) / 6)^2.
    float ShakeGainOf(int intensity)
    {
        const float g = (static_cast<float>(std::clamp(intensity, 1, 10)) + 1.0f) / 6.0f;
        return g * g;
    }
    int ShakeIntensityOf(float gain)
    {
        return std::clamp(static_cast<int>(std::floor(6.0f * std::sqrt(std::max(0.0f, gain)) - 1.0f + 0.5f)), 1, 10);
    }

    // The Shake row: Off, the eleven profiles (Gunshot after Explosion, the
    // two one shots together), Simple Camera's four presets (its noise with
    // their numbers) and Simple Custom (the noise with the marker's own
    // numbers).
    constexpr std::uint8_t kShakeProfileOrder[] = {1, 2, 3, 4, 5, 12, 6, 7, 8, 9, 10};
    constexpr int kShakeProfileChoices = static_cast<int>(sizeof(kShakeProfileOrder));
    constexpr int kShakePresetFirst = 1 + kShakeProfileChoices;
    constexpr int kShakeChoiceCount = kShakePresetFirst + 4 + 1;
    const NameText kShakePresetNames[] = {{TR_NOOP("shake.simple_subtle", "Simple: Subtle")}, {TR_NOOP("shake.simple_handheld", "Simple: Handheld")},
        {TR_NOOP("shake.simple_vehicle", "Simple: Vehicle")}, {TR_NOOP("shake.simple_earthquake", "Simple: Earthquake")}};
    int ShakeChoice(const MarkerShake& s)
    {
        if (!s.on)
            return 0;
        for (int n = 0; n < kShakeProfileChoices; ++n)
            if (s.profile != 0 && s.profile == kShakeProfileOrder[n])
                return 1 + n;
        for (int p = 0; p < 4; ++p)
        {
            const replay::camera::fx::PresetValues& v = replay::camera::fx::kPresets[p + 1];
            if (s.amp == v.amp && s.hz == v.hz && s.speedAmp == v.speedAmp && s.speedHz == v.speedHz)
                return kShakePresetFirst + p;
        }
        return kShakePresetFirst + 4;
    }
    const char* ShakeChoiceName(int choice)
    {
        if (choice <= 0)
            return TR("common.off_cap", "Off");
        if (choice < kShakePresetFirst)
            return NameOf(kShakeNames, kShakeProfileOrder[choice - 1]);
        if (choice < kShakePresetFirst + 4)
            return NameOf(kShakePresetNames, choice - kShakePresetFirst);
        return TR("shake.simple_custom", "Simple: Custom");
    }
    void ApplyShakeChoice(MarkerShake& s, int choice)
    {
        if (choice <= 0)
        {
            s.on = 0;
            return;
        }
        s.on = 1;
        if (choice < kShakePresetFirst)
        {
            // A profile at its own speed (4 Hz) and size (Amp 1).
            s.profile = kShakeProfileOrder[choice - 1];
            s.amp = 1.0f;
            s.hz = 4.0f;
            s.speedHz = 0.0f;
            return;
        }
        s.profile = 0;
        if (choice < kShakePresetFirst + 4)
        {
            const replay::camera::fx::PresetValues& v = replay::camera::fx::kPresets[choice - kShakePresetFirst + 1];
            s.amp = v.amp;
            s.hz = v.hz;
            s.speedAmp = v.speedAmp;
            s.speedHz = v.speedHz;
        }
    }
    std::string ShakeSummary(const MarkerShake& s)
    {
        if (!s.on)
            return TR("common.off_cap", "Off");
        return std::string(ShakeChoiceName(ShakeChoice(s))) + " " + std::to_string(ShakeIntensityOf(s.gain));
    }

    std::vector<MenuRow> BuildShakeRows(const CameraKeyframe& k)
    {
        std::vector<MenuRow> rows;
        const MarkerShake s = MarkerShakeOf(k);
        MenuRow kind{TR("shake.row", "Shake"), ShakeChoiceName(ShakeChoice(s)), true, 0};
        kind.step = [k](int step) {
            SendMarkerShake(k, [step](MarkerShake& v) {
                const int next = (ShakeChoice(v) + (step > 0 ? 1 : -1) + kShakeChoiceCount) % kShakeChoiceCount;
                ApplyShakeChoice(v, next);
            });
        };
        kind.help = TR("shake.row.help", "The camera shake from this marker to the next one: off, a realistic profile (Handheld to Wagon), or "
                    "Simple Camera's noise (its presets, or Custom with the numbers in Advanced). Explosion and Gunshot hit once, at this "
                    "marker (Restart Here fires them again).");
        rows.push_back(kind);
        MenuRow intensity{TR("shake.intensity", "Intensity"), FormatValue("%.0f", ShakeIntensityOf(s.gain)), s.on != 0, 0};
        intensity.step = [k](int step) {
            SendMarkerShake(k, [step](MarkerShake& v) { v.gain = ShakeGainOf(ShakeIntensityOf(v.gain) + (step > 0 ? 1 : -1)); });
        };
        intensity.help = TR("shake.intensity.help", "1 to 10 (5: the shake as tuned).");
        rows.push_back(intensity);
        char hz[48]{};
        if (s.profile != 0)
            std::snprintf(hz, sizeof(hz), "%.1f Hz  (%.0f%%)", s.hz, s.hz / 4.0f * 100.0f);
        else
            std::snprintf(hz, sizeof(hz), "%.1f Hz", s.hz);
        MenuRow frequency{TR("shake.frequency", "Frequency"), hz, s.on != 0, 0};
        frequency.step = [k](int step) {
            SendMarkerShake(k, [step](MarkerShake& v) { v.hz = std::clamp(v.hz + static_cast<float>(step) * 0.1f, 0.0f, 30.0f); });
        };
        frequency.help = KeyFill(TR("shake.frequency.help", "How fast it shakes ({key_page_up}/{key_page_down}: half a hertz). A profile plays at its own speed at 4 Hz."));
        rows.push_back(frequency);
        const bool blend = s.transition == static_cast<std::uint8_t>(ShakeTransition::Blend) && s.blendMask != 0;
        MenuRow transition{TR("shake.transition", "Transition"), !blend ? TR("shake.transition.cut", "Cut") : s.blendMask == ShakeBlendAll ? TR("shake.transition.blend", "Blend") : TR("shake.transition.part", "Blend (part)"), s.on != 0, 0};
        transition.step = [k](int) {
            SendMarkerShake(k, [](MarkerShake& v) {
                const bool on = v.transition == static_cast<std::uint8_t>(ShakeTransition::Blend) && v.blendMask != 0;
                v.transition = static_cast<std::uint8_t>(on ? ShakeTransition::Cut : ShakeTransition::Blend);
                v.blendMask = ShakeBlendAll;
            });
        };
        transition.help = TR("shake.transition.help", "Cut: this shake holds until the next marker. Blend: its numbers go smoothly to the next marker's "
                          "(Simple Camera's Ramp). Part: an old clip blended only some.");
        rows.push_back(transition);
        MenuRow advanced{TR("shake.advanced", "Advanced"), ">", s.on != 0, 1};
        advanced.enter = [n = rows.size()] {
            g_shakeAdvancedReturn = static_cast<int>(n);
            g_markerMenu = MarkerMenu::ShakeAdvanced;
            g_menuFocus = 0;
        };
        advanced.help = TR("shake.advanced.help", "Amplitude, the speed couplings, the weights, Stop When Still, the pattern, the clock.");
        rows.push_back(advanced);
        return rows;
    }

    std::vector<MenuRow> BuildShakeAdvancedRows(const CameraKeyframe& k)
    {
        std::vector<MenuRow> rows;
        const MarkerShake s = MarkerShakeOf(k);
        const auto number = [&](const char* label, const char* format, float value, float delta, float lo, float hi,
                                float MarkerShake::*field, const char* help) {
            MenuRow row{label, FormatValue(format, value), true, 0};
            row.step = [k, delta, lo, hi, field](int step) {
                SendMarkerShake(k, [&](MarkerShake& v) { v.*field = std::clamp(v.*field + static_cast<float>(step) * delta, lo, hi); });
            };
            row.help = help;
            rows.push_back(row);
        };
        number(TR("shake.amplitude", "Amplitude"), "%.2f", s.amp, 0.01f, 0.0f, 5.0f, &MarkerShake::amp,
            TR("shake.amplitude.help", "The noise: rotation up to amplitude x 1 degree, position amplitude x 5 cm. A profile: its size (1 as tuned)."));
        number(TR("shake.speed_amplitude", "Speed > Amplitude"), "%.2f", s.speedAmp, 0.05f, 0.0f, 2.0f, &MarkerShake::speedAmp,
            TR("shake.speed_amplitude.help", "Stronger as the camera moves faster: at the reference speed, x (1 + this)."));
        number(TR("shake.speed_frequency", "Speed > Frequency"), "%.2f", s.speedHz, 0.05f, 0.0f, 2.0f, &MarkerShake::speedHz,
            TR("shake.speed_frequency.help", "Faster as the camera moves faster: at the reference speed, frequency x (1 + this)."));
        number(TR("shake.reference_speed", "Reference Speed"), "%.0f m/s", s.speedRef, 1.0f, 1.0f, 100.0f, &MarkerShake::speedRef,
            TR("shake.reference_speed.help", "The camera speed counted as full for the two couplings."));
        number(TR("shake.rotation_weight", "Rotation Weight"), "%.2f", s.rotWeight, 0.05f, 0.0f, 2.0f, &MarkerShake::rotWeight,
            TR("shake.rotation_weight.help", "How much the shake turns the camera (0: none)."));
        number(TR("shake.position_weight", "Position Weight"), "%.2f", s.posWeight, 0.05f, 0.0f, 2.0f, &MarkerShake::posWeight,
            TR("shake.position_weight.help", "How much the shake moves the camera (0: none)."));
        const auto toggle = [&](const char* label, bool on, const std::function<void(MarkerShake&)>& flip, const char* help) {
            MenuRow row{label, on ? TR("common.on_cap", "On") : TR("common.off_cap", "Off"), true, 0};
            row.step = [k, flip](int) { SendMarkerShake(k, flip); };
            row.help = help;
            rows.push_back(row);
        };
        toggle(TR("shake.still", "Stop When Still"), s.still != 0, [](MarkerShake& v) { v.still = v.still ? 0 : 1; },
            TR("shake.still.help", "Fade the shake out while the camera stands still (back in when it moves)."));
        MenuRow pattern{TR("shake.pattern", "Pattern"), FormatValue("%.0f", s.seed), true, 0};
        pattern.step = [k](int step) {
            SendMarkerShake(k, [step](MarkerShake& v) { v.seed = static_cast<std::uint16_t>(static_cast<int>(v.seed) + step); });
        };
        pattern.help = TR("shake.pattern.help", "Another noise pattern with the same numbers (Simple Camera's Randomize, the same on every play).");
        rows.push_back(pattern);
        MenuRow speed{TR("shake.speed", "Shake Speed"), FormatValue("%.0f%%", s.rate * 100.0f), true, 0};
        speed.step = [k](int step) {
            SendMarkerShake(k, [step](MarkerShake& v) {
                // The marker speed presets (5 % to 200 %), from the one nearest.
                const int count = static_cast<int>(kMarkerSpeedCount);
                int at = 0;
                for (int n = 0; n < count; ++n)
                    if (std::fabs(kMarkerSpeedPercent[n] / 100.0f - v.rate) < std::fabs(kMarkerSpeedPercent[at] / 100.0f - v.rate))
                        at = n;
                at = std::clamp(at + (step > 0 ? 1 : -1), 0, count - 1);
                v.rate = static_cast<float>(kMarkerSpeedPercent[at]) / 100.0f;
            });
        };
        speed.help = TR("shake.speed.help", "The shake's clock, 5 % to 200 %.");
        rows.push_back(speed);
        toggle(TR("shake.real_time", "Real Time"), (s.flags & ShakeFlagRealTime) != 0,
            [](MarkerShake& v) { v.flags = static_cast<std::uint8_t>(v.flags ^ ShakeFlagRealTime); },
            TR("shake.real_time.help", "On: the shake keeps real time in this marker's slow motion. Off: it slows down with the clip."));
        toggle(TR("shake.fade_out", "Fade Out"), (s.flags & ShakeFlagFadeOut) != 0,
            [](MarkerShake& v) { v.flags = static_cast<std::uint8_t>(v.flags ^ ShakeFlagFadeOut); },
            TR("shake.fade_out.help", "With Cut: the shake fades out over the last quarter second before the next marker."));
        toggle(TR("shake.restart", "Restart Here"), (s.flags & ShakeFlagRestart) != 0,
            [](MarkerShake& v) {
                v.flags = static_cast<std::uint8_t>(v.flags ^ ShakeFlagRestart);
                v.startCycles = 0.0;
                v.startFadeMs = 0.0;
            },
            TR("shake.restart.help", "The pattern and its fade in start again at this marker (otherwise the shake goes on from the marker before)."));
        for (MenuRow& row : rows)
            row.enabled = s.on != 0;
        return rows;
    }

    // ---------------- The Spline tuning ----------------
    // A marker's own fields (the rest from the Spline Defaults) and the defaults
    // themselves. Numbers on the arrows (0.05 a step, PageUp and PageDown five),
    // Enter types them; a marker's row goes left past 0 to the default again.
    // Shown values in brackets come from the defaults.
    SplineTuning MarkerSpline(const CameraKeyframe& k)
    {
        for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
            if (g_snapshot.cameraKeyframes[n].id == k.id)
                return g_pendingSpline.Base(k.id, g_snapshot.cameraKeyframeSpline[n], GetTickCount64());
        return DefaultSplineTuning();
    }

    SplineTuning SplineDefaultsShown()
    {
        return g_pendingSplineDefaults.Base(1, g_snapshot.splineDefaults, GetTickCount64());
    }

    void SendMarkerSpline(const CameraKeyframe& k, const std::function<void(SplineTuning&)>& edit)
    {
        SplineTuning t = MarkerSpline(k);
        edit(t);
        Command command{};
        command.keyframeId = k.id;
        const ULONGLONG now = GetTickCount64();
        command.options = g_pendingOptions.Base(k.id, OptionsOf(k), now);
        command.options.mask = CameraOptionSpline;
        command.spline = t;
        if (Send(CommandType::SetCameraKeyframeOptions, &command))
            g_pendingSpline.Sent(k.id, t, now);
    }

    // ---------------- Camera attach, bones and near clip ----------------
    // The marker's bones, attach pose numbers, near clip and DOF numbers.
    CameraMarkerExtra MarkerExtra(const CameraKeyframe& k)
    {
        for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount && n < kMaxCameraKeyframesInSnapshot; ++n)
            if (g_snapshot.cameraKeyframes[n].id == k.id)
                return g_pendingExtra.Base(k.id, g_snapshot.cameraKeyframeExtra[n], GetTickCount64());
        return CameraMarkerExtra{};
    }

    void SendMarkerExtra(const CameraKeyframe& k, std::uint32_t fields, const std::function<void(CameraMarkerExtra&)>& edit)
    {
        CameraMarkerExtra e = MarkerExtra(k);
        edit(e);
        e.fields = fields;
        Command command{};
        command.keyframeId = k.id;
        const ULONGLONG now = GetTickCount64();
        command.options = g_pendingOptions.Base(k.id, OptionsOf(k), now);
        command.options.mask = CameraOptionExtra;
        command.extra = e;
        if (Send(CommandType::SetCameraKeyframeOptions, &command))
        {
            e.fields = 0;
            g_pendingExtra.Sent(k.id, e, now);
        }
    }

    int TargetSlot(std::uint32_t id)
    {
        for (std::uint32_t n = 0; n < g_snapshot.targetCount && n < kMaxTargetsInSnapshot; ++n)
            if (g_snapshot.targets[n].id == id)
                return static_cast<int>(n);
        return -1;
    }

    std::string BoneLabel(std::uint32_t ref)
    {
        namespace bones = replay::camera::bones;
        if (ref == 0)
            return TR("bone.root", "Root");
        if (bones::ModeOf(ref) == bones::kModeNamed && bones::ValueOf(ref) < bones::kNamedCount)
            return TR(bones::kNamed[bones::ValueOf(ref)].key, bones::kNamed[bones::ValueOf(ref)].label);
        return TRF("bone.index", "Bone {index}", {{"index", std::to_string(bones::ValueOf(ref))}});
    }

    // The Attach Bone or Look At Bone row of a target.
    void AddBoneRow(std::vector<MenuRow>& rows, const char* label, const CameraKeyframe& k, std::uint32_t target, bool attach)
    {
        namespace bones = replay::camera::bones;
        const int slot = TargetSlot(target);
        const std::uint64_t mask = slot >= 0 ? (static_cast<std::uint64_t>(g_snapshot.targetBoneMaskHigh[slot]) << 32) | g_snapshot.targetBoneMask[slot] : 0;
        const std::uint32_t count = slot >= 0 ? g_snapshot.targetBoneCount[slot] : 0;
        const CameraMarkerExtra e = MarkerExtra(k);
        const std::uint32_t ref = attach ? e.attachBone : e.lookAtBone;
        const std::uint32_t field = attach ? CameraExtraAttachBone : CameraExtraLookAtBone;
        MenuRow row{label, BoneLabel(ref), mask != 0 || count != 0 || ref != 0, 0};
        row.step = [k, attach, field, mask, count](int step) {
            SendMarkerExtra(k, field, [&](CameraMarkerExtra& x) {
                std::uint32_t& r = attach ? x.attachBone : x.lookAtBone;
                r = bones::Step(r, step, mask, count);
            });
        };
        if (count)
        {
            row.typed = [k, attach, field, count](double typedValue) {
                const long index = std::lround(typedValue);
                if (index < 0 || static_cast<std::uint32_t>(index) >= count)
                    return;
                SendMarkerExtra(k, field, [&](CameraMarkerExtra& x) {
                    (attach ? x.attachBone : x.lookAtBone) = bones::Index(static_cast<std::uint32_t>(index));
                });
            };
            row.typeStart = bones::ModeOf(ref) == bones::kModeIndex ? FormatValue("%.0f", bones::ValueOf(ref)) : "0";
            row.typeMin = 0.0f;
            row.typeMax = static_cast<float>(count - 1);
        }
        const bones::Family family = slot >= 0 ? static_cast<bones::Family>(g_snapshot.targetFamily[slot]) : bones::Family::None;
        const char* familyText = family == bones::Family::Human ? TR("bone.family.human", "human")
            : family == bones::Family::Horse ? TR("bone.family.horse", "horse")
            : family == bones::Family::Animal ? TR("bone.family.animal", "animal")
            : family == bones::Family::Weapon ? TR("bone.family.weapon", "weapon")
            : family == bones::Family::Vehicle ? TR("bone.family.vehicle", "vehicle") : TR("bone.family.none", "none");
        row.help = std::string(attach ? TR("bone.attach.help", "The camera rides this bone (Root: the whole entity). ") : TR("bone.look.help", "The camera looks at this bone. ")) +
            TRF("bone.list.help", "The named bones of the {family} come first, then every bone of its skeleton by index", {{"family", familyText}}) +
            (count ? replay::keys::Format(TR("bone.count.help", " ({count} bones; {key_page_up}/{key_page_down}: five at a time; {key_select} types an index)."),
                         Keys(), {{"count", std::to_string(count)}})
                   : std::string(TR("bone.no_skeleton.help", " (skeleton not recorded here: named bones only).")));
        rows.push_back(row);
    }

    // A number of the attach pose (metres or degrees in the attach frame).
    void AddPoseRow(std::vector<MenuRow>& rows, const char* label, const CameraKeyframe& k, bool rotation, int axis, const char* help)
    {
        const CameraMarkerExtra e = MarkerExtra(k);
        const Vec3& v = rotation ? e.relRotation : e.relPosition;
        const float value = axis == 0 ? v.x : axis == 1 ? v.y : v.z;
        const float delta = rotation ? 1.0f : 0.05f;
        MenuRow row{label, FormatValue(rotation ? "%.1f deg" : "%.2f m", value), true, 0};
        const std::uint32_t field = rotation ? CameraExtraRelRotation : CameraExtraRelPosition;
        row.step = [k, rotation, axis, delta, field](int step) {
            SendMarkerExtra(k, field, [&](CameraMarkerExtra& x) {
                Vec3& w = rotation ? x.relRotation : x.relPosition;
                float& c = axis == 0 ? w.x : axis == 1 ? w.y : w.z;
                c = std::round((c + static_cast<float>(step) * delta) * 100.0f) / 100.0f;
            });
        };
        row.typed = [k, rotation, axis, field](double typedValue) {
            SendMarkerExtra(k, field, [&](CameraMarkerExtra& x) {
                Vec3& w = rotation ? x.relRotation : x.relPosition;
                (axis == 0 ? w.x : axis == 1 ? w.y : w.z) = static_cast<float>(typedValue);
            });
        };
        row.typeStart = FormatValue("%.2f", value);
        row.typeMin = rotation ? -180.0f : -100.0f;
        row.typeMax = rotation ? 180.0f : 100.0f;
        row.help = std::string(help) +
            KeyFill(rotation ? TR("pose.rotation.step", " 1 degree a step ({key_page_up}/{key_page_down}: five).")
                             : TR("pose.position.step", " 5 cm a step ({key_page_up}/{key_page_down}: 25 cm).")) +
            KeyFill(TR("pose.types", " {key_select} types it."));
        rows.push_back(row);
    }

    // The engine's lens for a FOV (12 mm over tan(fov / 2)) times a focal length
    // multiplier (2.0 to 2.5 by intensity): camera::DofLensMm.
    float AutoDofLens(float fov, float intensity)
    {
        const float f = std::clamp(fov, 5.0f, 130.0f);
        const float base = 12.0f / std::tan(f * 0.5f * 3.14159265f / 180.0f);
        return std::clamp(base * (2.0f + 0.5f * std::clamp(intensity, 0.0f, 100.0f) / 100.0f), kDofLensMin, kDofLensMax);
    }

    float IntensityFStop(float intensity)
    {
        return 22.0f * std::pow(0.5f / 22.0f, std::clamp(intensity, 0.0f, 100.0f) / 100.0f);
    }

    void SendSplineDefaults(const std::function<void(SplineTuning&)>& edit)
    {
        SplineTuning t = SplineDefaultsShown();
        edit(t);
        t.overrides = 0;
        Command command{};
        command.spline = t;
        if (Send(CommandType::SetSplineDefaults, &command))
            g_pendingSplineDefaults.Sent(1, t, GetTickCount64());
    }

    const NameText kSplinePacingNames[] = {{TR_NOOP("spline.profile.natural", "Natural")}, {TR_NOOP("spline.profile.continuous", "Continuous")},
        {TR_NOOP("spline.profile.per_segment", "Per Segment")}};

    std::string SplineNumber(float value)
    {
        return FormatValue("%.2f", value);
    }

    // The value rows of a tuning. `own`: the tuning shown (a marker's or the
    // defaults); `defaults`: what a marker's field falls back to; `perMarker`:
    // the rows have the default state; `apply`: sends a change.
    void AddSplineRows(std::vector<MenuRow>& rows, const SplineTuning& own, const SplineTuning& defaults, bool perMarker,
        const std::function<void(const std::function<void(SplineTuning&)>&)>& apply)
    {
        const auto overridden = [&](std::uint8_t bit) { return !perMarker || (own.overrides & bit) != 0; };
        const auto bracket = [](const std::string& text, bool mine) { return mine ? text : "(" + text + ")"; };
        const SplinePacing pacing = static_cast<SplinePacing>(overridden(SplineOverridePacing) ? own.pacing : defaults.pacing);

        // Speed Profile: (default), Natural, Continuous, Per Segment.
        {
            const bool mine = overridden(SplineOverridePacing);
            const int shownPacing = std::min<int>(mine ? own.pacing : defaults.pacing, kSplinePacingCount - 1);
            MenuRow row{TR("spline.profile", "Speed Profile"), bracket(NameOf(kSplinePacingNames, shownPacing), mine), true, 0};
            row.step = [apply, perMarker](int step) {
                apply([step, perMarker](SplineTuning& t) {
                    const int states = perMarker ? kSplinePacingCount + 1 : kSplinePacingCount;
                    int at = perMarker ? ((t.overrides & SplineOverridePacing) ? t.pacing + 1 : 0) : t.pacing;
                    at = (at + (step > 0 ? 1 : states - 1)) % states;
                    if (perMarker && at == 0)
                        t.overrides = static_cast<std::uint8_t>(t.overrides & ~SplineOverridePacing);
                    else
                    {
                        t.pacing = static_cast<std::uint8_t>(perMarker ? at - 1 : at);
                        t.overrides = static_cast<std::uint8_t>(t.overrides | SplineOverridePacing);
                    }
                });
            };
            row.help = TR("spline.profile.help", "Natural: the curve over the marker times (it slows into tight bends). Continuous: one speed through the whole "
                       "move, every marker reached on time. Per Segment: an even speed inside each segment.");
            if (perMarker)
                row.help += TR("spline.brackets", " In brackets: the Spline Defaults.");
            rows.push_back(row);
        }

        const auto number = [&](const char* label, float SplineTuning::*field, std::uint8_t bit, std::string help) {
            const bool mine = overridden(bit);
            const float value = mine ? own.*field : defaults.*field;
            MenuRow row{label, bracket(SplineNumber(value), mine), true, 0};
            row.step = [apply, field, bit, perMarker](int step) {
                apply([&](SplineTuning& t) {
                    const bool set = !perMarker || (t.overrides & bit) != 0;
                    if (!set)
                    {
                        // From the default: the first step starts at its value.
                        t.overrides = static_cast<std::uint8_t>(t.overrides | bit);
                        t.*field = std::clamp(SplineDefaultsShown().*field + static_cast<float>(step) * 0.05f, 0.0f, 1.0f);
                        return;
                    }
                    if (perMarker && step < 0 && t.*field <= 0.0001f)
                    {
                        t.overrides = static_cast<std::uint8_t>(t.overrides & ~bit); // left past 0: the default again
                        return;
                    }
                    t.*field = std::clamp(std::round((t.*field + static_cast<float>(step) * 0.05f) * 100.0f) / 100.0f, 0.0f, 1.0f);
                });
            };
            row.typed = [apply, field, bit](double typedValue) {
                apply([&](SplineTuning& t) {
                    t.*field = std::clamp(static_cast<float>(typedValue), 0.0f, 1.0f);
                    t.overrides = static_cast<std::uint8_t>(t.overrides | bit);
                });
            };
            row.typeStart = SplineNumber(value);
            row.typeMin = 0.0f;
            row.typeMax = 1.0f;
            if (perMarker)
                help += TR("spline.left_past_zero", " Left past 0: back to the default.");
            row.help = help + KeyFill(TR("spline.types", " {key_select} types a number."));
            rows.push_back(row);
        };

        {
            const float alpha = overridden(SplineOverrideAlpha) ? own.alpha : defaults.alpha;
            std::string help = TR("spline.curve_shape.help", "The Catmull-Rom spacing: 0 Uniform (wide turns), 0.5 Centripetal, 1 Chordal (holds short legs tight).");
            if (pacing == SplinePacing::Natural)
                help += TR("spline.curve_shape.natural", " No effect with the Natural profile (a curve over the marker times has no spacing to choose).");
            else if (alpha < 0.001f)
                help += TR("spline.curve_shape.uniform", " Now: Uniform.");
            else if (std::fabs(alpha - 0.5f) < 0.001f)
                help += TR("spline.curve_shape.centripetal", " Now: Centripetal.");
            else if (alpha > 0.999f)
                help += TR("spline.curve_shape.chordal", " Now: Chordal.");
            number(TR("spline.curve_shape", "Curve Shape"), &SplineTuning::alpha, SplineOverrideAlpha, help);
        }
        number(TR("spline.ease_in", "Ease In"), &SplineTuning::easeIn, SplineOverrideEaseIn,
            TR("spline.ease_in.help", "The share of the segment spent speeding up from rest (it still arrives on time). Position, rotation, zoom and "
            "framing ease together."));
        number(TR("spline.ease_out", "Ease Out"), &SplineTuning::easeOut, SplineOverrideEaseOut,
            TR("spline.ease_out.help", "The share of the segment spent slowing down to rest at the next marker. Ease In plus Ease Out over 1 are scaled down."));
        number(TR("spline.weight", "Camera Weight"), &SplineTuning::weight, SplineOverrideWeight,
            TR("spline.weight.help", "Pulls the curve toward a B-spline: the camera cuts inside the corners like it has mass, never past a marker. "
            "Only while playing and exporting; paused, the camera sits on the markers."));

        const auto channel = [&](const char* label, std::uint8_t channelBit, std::uint8_t bit, const char* help) {
            const bool mine = overridden(bit);
            const bool on = ((mine ? own.channels : defaults.channels) & channelBit) != 0;
            MenuRow row{label, bracket(on ? TR("common.on_cap", "On") : TR("common.off_cap", "Off"), mine), true, 0};
            row.step = [apply, channelBit, bit, perMarker](int step) {
                apply([&](SplineTuning& t) {
                    // A marker: (default), On, Off; the defaults: On, Off.
                    const int states = perMarker ? 3 : 2;
                    const bool set = !perMarker || (t.overrides & bit) != 0;
                    const bool isOn = (t.channels & channelBit) != 0;
                    int at = perMarker ? (set ? (isOn ? 1 : 2) : 0) : (isOn ? 0 : 1);
                    at = (at + (step > 0 ? 1 : states - 1)) % states;
                    const bool wantOn = perMarker ? at == 1 : at == 0;
                    if (perMarker && at == 0)
                    {
                        t.overrides = static_cast<std::uint8_t>(t.overrides & ~bit);
                        return;
                    }
                    t.channels = static_cast<std::uint8_t>(wantOn ? (t.channels | channelBit) : (t.channels & ~channelBit));
                    t.overrides = static_cast<std::uint8_t>(t.overrides | bit);
                });
            };
            row.help = help;
            rows.push_back(row);
        };
        channel(TR("spline.position", "Spline Position"), SplineChannelPosition, SplineOverridePosition,
            TR("spline.position.help", "Off: the camera's position goes straight between the markers (with the ease)."));
        channel(TR("spline.rotation", "Spline Rotation"), SplineChannelRotation, SplineOverrideRotation,
            TR("spline.rotation.help", "Off: the rotation turns straight between the markers (with the ease)."));
        channel(TR("spline.zoom", "Spline Zoom"), SplineChannelFov, SplineOverrideFov,
            TR("spline.zoom.help", "Off: the field of view changes straight. On: a curve that never passes its target and comes back."));
    }

    std::vector<MenuRow> BuildSplineDefaultRows()
    {
        std::vector<MenuRow> rows;
        const SplineTuning d = SplineDefaultsShown();
        AddSplineRows(rows, d, d, false, [](const std::function<void(SplineTuning&)>& edit) { SendSplineDefaults(edit); });
        MenuRow reset{TR("spline.reset_defaults", "Reset Defaults"), "", true, 2};
        reset.enter = [] { SendSplineDefaults([](SplineTuning& t) { t = DefaultSplineTuning(); }); };
        reset.help = TR("spline.reset_defaults.help", "Natural, Curve Shape 1.00, no ease, no weight, every channel on. Saved in RedemptionEditor.ini.");
        rows.push_back(reset);
        return rows;
    }

    std::vector<MenuRow> BuildSplineRows(const CameraKeyframe& k)
    {
        std::vector<MenuRow> rows;
        const SplineTuning own = MarkerSpline(k);
        AddSplineRows(rows, own, SplineDefaultsShown(), true,
            [k](const std::function<void(SplineTuning&)>& edit) { SendMarkerSpline(k, edit); });
        MenuRow reset{TR("spline.use_defaults", "Use Defaults"), "", own.overrides != 0, 2};
        reset.enter = [k] { SendMarkerSpline(k, [](SplineTuning& t) { t.overrides = 0; }); };
        reset.help = TR("spline.use_defaults.help", "Every row of this marker back to the Spline Defaults.");
        rows.push_back(reset);
        MenuRow defaults{TR("spline.defaults", "Spline Defaults"), ">", true, 1};
        defaults.enter = [count = rows.size()] {
            g_splineDefaultsReturn = static_cast<int>(count);
            g_markerMenu = MarkerMenu::SplineDefaults;
            g_menuFocus = 0;
        };
        defaults.help = TR("spline.defaults.marker_help", "The values every marker uses unless it sets its own (saved in RedemptionEditor.ini).");
        rows.push_back(defaults);
        return rows;
    }

    // A typed number over the focused row (the Spline rows): Enter sends it, Esc
    // leaves the row as it was.
    void StartRowTyping(const MenuRow& row)
    {
        g_rowTyping.on = true;
        g_rowTyping.label = row.label;
        g_rowTyping.focus = true;
        g_rowTyping.error = "";
        std::snprintf(g_rowTyping.text, sizeof(g_rowTyping.text), "%s", row.typeStart.c_str());
    }

    void DrawRowTypeBox(ImDrawList* draw, float y, const MenuRow& row)
    {
        UI_PROBE_SCOPE("RowTypeBox");
        g_fxTypingFrame = true;
        const float x = 40.0f + 300.0f * 0.40f;
        const float width = 300.0f * 0.60f - 6.0f;
        draw->AddRectFilled(P(x - 4.0f, y + 2.0f), P(x + width + 2.0f, y + 32.0f), IM_COL32(10, 9, 8, 235));
        ImGui::SetCursorScreenPos(P(x, y + 4.0f));
        ImGui::PushItemWidth(S(width));
        if (g_rowTyping.focus)
        {
            ImGui::SetKeyboardFocusHere();
            g_rowTyping.focus = false;
        }
        const bool entered = ImGui::InputText("##rowtype", g_rowTyping.text, sizeof(g_rowTyping.text),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CharsDecimal, nullptr,
            nullptr) || MissedTap(VK_RETURN);
        ImGui::PopItemWidth();
        if (g_rowTyping.error[0])
            TextFit(draw, kEditX, y + 36.0f, 14.0f, kGold, g_rowTyping.error, kEditW);
        if (entered)
        {
            std::string text(g_rowTyping.text);
            std::replace(text.begin(), text.end(), ',', '.');
            char* end = nullptr;
            const double number = std::strtod(text.c_str(), &end);
            if (end == text.c_str() || *end || !std::isfinite(number) || number < row.typeMin || number > row.typeMax)
            {
                static std::string why;
                why = TRF("typing.range", "A number from {min} to {max}.", {{"min", replay::i18n::Printf("%g", row.typeMin)}, {"max", replay::i18n::Printf("%g", row.typeMax)}});
                g_rowTyping.error = why.c_str();
                PlayUi(UiSound::Error);
                return;
            }
            if (row.typed)
                row.typed(number);
            PlayUi(UiSound::Select);
            g_rowTyping.on = false;
        }
        else if (!g_helpOpen && !g_helpSwallow && KeyPressed(ImGuiKey_Escape, false))
        {
            PlayUi(UiSound::Back);
            g_rowTyping.on = false;
        }
    }

    // A photo mode filter's name ("Game 01", "Modern 01", "Vintage 01").
    std::string FilterName(std::uint32_t index)
    {
        const std::string english = kMarkerFilters[index];
        const std::size_t space = english.rfind(' ');
        if (index == 0 || space == std::string::npos)
            return TR("common.none", "None");
        const std::string number = english.substr(space + 1);
        const std::string family = english.substr(0, space);
        if (family == "Game")
            return TRF("filter.game", "Game {n}", {{"n", number}});
        if (family == "Modern")
            return TRF("filter.modern", "Modern {n}", {{"n", number}});
        return TRF("filter.vintage", "Vintage {n}", {{"n", number}});
    }

    std::vector<MenuRow> BuildMarkerRows(const CameraKeyframe& k, int index)
    {
        std::vector<MenuRow> rows;
        const int count = static_cast<int>(g_snapshot.cameraKeyframeCount);
        const bool isLast = index == count - 1 && count > 1;
        const CameraKeyframe* previous = index > 0 ? &g_snapshot.cameraKeyframes[index - 1] : nullptr;
        const bool free = k.cameraMode == CameraMode::Free || k.cameraMode == CameraMode::Target || k.cameraMode == CameraMode::Follow;
        const bool anchorOnly = k.markerType == MarkerType::Anchor;
        if (g_markerMenu == MarkerMenu::Root)
        {
            // LockCutsceneCamera: a marker inside a recorded cutscene is the recorded
            // camera; its options are greyed.
            bool inCutscene = false;
            for (std::uint32_t n = 0; n < g_snapshot.spanCount && n < kMaxSpansInSnapshot; ++n)
                inCutscene = inCutscene || (g_snapshot.spans[n].kind == 0 && k.timeMs >= g_snapshot.spans[n].startMs &&
                    k.timeMs < g_snapshot.spans[n].endMs);
            const bool locked = Flag(SnapshotLockCutsceneCamera) && inCutscene;
            MenuRow camera{TR("marker.camera_options", "Camera Options"), locked ? TR("marker.camera_options.game", "Game") : ">", !anchorOnly && !locked, 1};
            camera.icon = ui_art::Art::ToolCamera;
            camera.enter = [] { g_markerMenu = MarkerMenu::Camera; g_menuFocus = 0; };
            camera.help = locked ? TR("marker.camera_options.locked", "Unavailable during a cutscene (recorded camera)")
                                 : TR("marker.camera_options.help", "The camera for this marker: type, targets, blend, shake, field of view.");
            rows.push_back(camera);
            MenuRow dof{TR("marker.dof", "Depth of Field"), ">", !anchorOnly && !isLast, 1};
            dof.icon = ui_art::Art::ToolDof;
            dof.enter = [] { g_markerMenu = MarkerMenu::Dof; g_menuFocus = 0; };
            dof.help = isLast ? TR("marker.dof.last", "The last marker uses the depth of field of the one before it.") : TR("marker.dof.help", "Focus and background blur from this marker on.");
            rows.push_back(dof);
            MenuRow effects{TR("marker.effects", "Effects"), ">", !anchorOnly, 1};
            effects.icon = ui_art::Art::ToolEffects;
            effects.enter = [] { g_markerMenu = MarkerMenu::Effects; g_menuFocus = 0; };
            effects.help = TR("marker.effects.help", "The game's photo mode filters and fades from this marker on.");
            rows.push_back(effects);
            // The marker's shake (an Anchor the shake migration added holds one too).
            const bool holdsShake = !anchorOnly || k.markerOrigin == 1;
            MenuRow shake{TRF("marker.shake", "Shake ({summary})", {{"summary", ShakeSummary(MarkerShakeOf(k))}}), ">", holdsShake, 1};
            shake.icon = ui_art::Art::ToolShake;
            shake.enter = [] { g_menuReturnFocus = g_menuFocus; g_markerMenu = MarkerMenu::Shake; g_menuFocus = 0; };
            shake.help = k.markerOrigin == 1 ? TR("marker.shake.migrated", "Added when the clip's old shake events were moved into the markers: it holds a "
                                               "change of the shake, the camera passes it by.")
                                             : TR("marker.shake.help", "The camera shake from this marker on (profile, intensity, frequency, transition).");
            rows.push_back(shake);
            // The marker's numbers and the tools for the move it belongs to.
            const bool posable = free && !anchorOnly && !isLast;
            MenuRow position{TR("marker.position", "Position"), k.attachTarget ? TR("marker.position.attached", "Attached") : ">", posable && !k.attachTarget, 1};
            position.enter = [] { g_menuReturnFocus = g_menuFocus; g_markerMenu = MarkerMenu::Position; g_menuFocus = 0; };
            position.help = k.attachTarget ? TR("marker.position.attached_help", "An attached marker is framed in Edit Camera (its position rides the target).")
                                           : TR("marker.position.help", "The marker's position and rotation as numbers.");
            rows.push_back(position);
            MenuRow tools{TR("marker.tools", "Move Tools"), ">", !anchorOnly, 1};
            tools.enter = [] { g_menuReturnFocus = g_menuFocus; g_markerMenu = MarkerMenu::Tools; g_menuFocus = 0; };
            tools.help = TR("marker.tools.help", "Duplicate, even out the speed, stretch the move, lock it to a character.");
            rows.push_back(tools);
            MenuRow speed{TR("marker.speed", "Speed"), FormatValue("%.0f%%", kMarkerSpeedPercent[std::min<std::uint32_t>(k.speedIndex, kMarkerSpeedCount - 1)]), !isLast, 0};
            speed.step = [k](int step) {
                SendOptions(k, CameraOptionSpeed, [&](CameraKeyframeOptions& o) {
                    o.speedIndex = static_cast<std::uint8_t>(std::clamp(static_cast<int>(o.speedIndex) + (step > 0 ? 1 : -1), 0, static_cast<int>(kMarkerSpeedCount) - 1));
                });
            };
            speed.help = TR("marker.speed.help", "Slow motion or fast from this marker to the next one.");
            rows.push_back(speed);
            char at[24]{};
            FormatClock(k.timeMs, at, sizeof(at), true);
            MenuRow time{TR("marker.time", "Time"), at, true, 0};
            time.step = [k](int step) {
                Command command{};
                command.keyframeId = k.id;
                const ULONGLONG now = GetTickCount64();
                const std::uint64_t from = g_pendingMoveTime.Base(k.id, k.timeMs, now);
                const std::int64_t to = static_cast<std::int64_t>(from) + step * 33;
                command.timeMs = static_cast<std::uint64_t>(std::max<std::int64_t>(0, to));
                if (Send(CommandType::MoveCameraKeyframe, &command)) // the core seeks to the time taken
                    g_pendingMoveTime.Sent(k.id, command.timeMs, now);
            };
            time.help = KeyFill(TR("marker.time.help", "Move the marker one frame ({key_page_up}/{key_page_down}: ten)."));
            rows.push_back(time);
            const bool isFirst = index == 0 && count > 1;
            if (isFirst || isLast)
            {
                // The clip's ends: a fade instead of the anchor.
                MenuRow fade{TR("shake.transition", "Transition"), k.transition ? (isFirst ? TR("marker.fade_in", "Fade In") : TR("marker.fade_out", "Fade Out")) : TR("common.none", "None"), true, 0};
                fade.step = [k](int) {
                    SendOptions(k, CameraOptionTransition, [&](CameraKeyframeOptions& o) { o.transition = o.transition ? 0 : 1; });
                };
                fade.help = isFirst ? TR("marker.fade_in.help", "Fade from black at the start of the clip.") : TR("marker.fade_out.help", "Fade to black at the end of the clip.");
                rows.push_back(fade);
                if (k.transition)
                {
                    MenuRow length{TR("marker.fade_length", "Fade Length"), FormatValue("%.1f s", k.transitionMs / 1000.0), true, 0};
                    length.step = [k](int step) {
                        SendOptions(k, CameraOptionTransitionMs, [&](CameraKeyframeOptions& o) {
                            o.transitionMs = static_cast<std::uint16_t>(std::clamp(static_cast<int>(o.transitionMs) + step * 100, 100, 10000));
                        });
                    };
                    length.help = TR("marker.fade_length.help", "0.1 s a step.");
                    rows.push_back(length);
                }
            }
            else if (k.markerType != MarkerType::Edit)
            {
                // Anchor is hidden until it does something (anchors sync to a project's
                // music, which this editor has not). A marker already an anchor still shows
                // the row, so it can be turned Off; the row goes once it is.
                MenuRow anchor{TR("marker.anchor", "Anchor"), NameOf(kAnchorNames, std::clamp(static_cast<int>(k.markerType), 0, 2)), true, 0};
                anchor.step = [k](int step) {
                    SendOptions(k, CameraOptionMarkerType, [&](CameraKeyframeOptions& o) { o.markerType = Cycle(o.markerType, step, 3); });
                };
                anchor.help = TR("marker.anchor.help", "Exclusive: a mark on the timeline only, the camera ignores it.");
                rows.push_back(anchor);
            }
            auto action = [&](const char* label, std::function<void()> run, const char* help) {
                MenuRow row{label, "", true, 2};
                row.enter = std::move(run);
                row.help = help;
                rows.push_back(row);
            };
            action(TR("marker.copy", "Copy"), [k] { Command c{}; c.keyframeId = k.id; Send(CommandType::CopyCameraKeyframe, &c); }, KeyFill(TR("marker.copy.help", "Copy this marker's settings ({key_copy}).")));
            action(TR("marker.paste", "Paste"), [k] { Command c{}; c.keyframeId = k.id; c.enabled = 0; ForgetPendingMarkerValues(); Send(CommandType::PasteCameraKeyframe, &c); }, KeyFill(TR("marker.paste.help", "Paste the copied settings here ({key_paste}).")));
            rows.back().enabled = Flag(SnapshotMarkerCopied); // nothing copied, nothing to paste
            action(TR("marker.reset", "Reset"), [k] { Command c{}; c.keyframeId = k.id; ForgetPendingMarkerValues(); Send(CommandType::ResetCameraKeyframe, &c); }, TR("marker.reset.help", "Back to the default settings."));
            action(TR("marker.paste_effects", "Paste Effects"), [k] { Command c{}; c.keyframeId = k.id; c.enabled = 1; ForgetPendingMarkerValues(); Send(CommandType::PasteCameraKeyframe, &c); }, TR("marker.paste_effects.help", "Paste only the copied effects."));
            rows.back().enabled = Flag(SnapshotMarkerCopied);
            const bool bound = isFirst || isLast;
            action(TR("marker.delete", "Delete"), [k] { DeleteMarker(k.id); },
                bound ? TR("marker.delete.bound", "The clip's start and end stay; move them instead.") : KeyFill(TR("marker.delete.help", "Delete this marker ({key_delete}).")));
            rows.back().enabled = !bound;
            return rows;
        }
        if (g_markerMenu == MarkerMenu::Camera)
        {
            // The last marker's type cannot be changed. It takes the camera of the
            // marker before it: a Free Camera when that one blends into it
            // (ApplyLastMarkerRule), otherwise that marker's camera plays up to the end
            // (Plan skips the last marker), so its type is shown. The core refuses the
            // type on the last marker too.
            const CameraKeyframe* before = nullptr;
            for (int n = index - 1; isLast && n >= 0 && !before; --n)
                if (g_snapshot.cameraKeyframes[n].markerType != MarkerType::Anchor)
                    before = &g_snapshot.cameraKeyframes[n];
            const bool blendedInto = before && before->blend != Blend::None && before->cameraMode == CameraMode::Free;
            const CameraMode shownMode = isLast && before && !blendedInto ? before->cameraMode : k.cameraMode;
            MenuRow type{TR("marker.camera", "Camera"), CameraName(shownMode), !isLast, 0};
            type.step = [k](int step) {
                SendOptions(k, CameraOptionMode, [&](CameraKeyframeOptions& o) {
                    int at = 6;
                    for (int n = 0; n < 7; ++n)
                        if (kCameraCycle[n] == o.cameraMode) // from the value last sent
                            at = n;
                    at = (at + (step > 0 ? 1 : -1) + 7) % 7;
                    o.cameraMode = kCameraCycle[at];
                });
            };
            type.help = isLast ? TR("marker.camera.last", "The last marker keeps the camera of the marker before it.")
                               : TR("marker.camera.help", "Game Camera plays the recorded view; the presets orbit the main character; Free Camera is yours to frame.");
            rows.push_back(type);
            {
                // The near clip plane.
                const float own = MarkerExtra(k).nearClip;
                MenuRow nearRow{TR("freecam.near_clip", "Near Clip"), own > 0.0f ? FormatValue("%.2f m", own) : TRF("marker.near_clip.default", "Default ({value} m)", {{"value", replay::i18n::Printf("%.2f", kNearClipDefault)}}), true, 0};
                nearRow.step = [k](int step) {
                    SendMarkerExtra(k, CameraExtraNearClip, [&](CameraMarkerExtra& x) {
                        const float base = x.nearClip > 0.0f ? x.nearClip : kNearClipDefault;
                        x.nearClip = std::clamp(std::round((base + static_cast<float>(step) * 0.01f) * 100.0f) / 100.0f, kNearClipMin, kNearClipMax);
                    });
                };
                nearRow.typed = [k](double typedValue) {
                    const float v = static_cast<float>(typedValue);
                    SendMarkerExtra(k, CameraExtraNearClip, [&](CameraMarkerExtra& x) {
                        x.nearClip = v <= 0.0f ? 0.0f : std::clamp(v, kNearClipMin, kNearClipMax);
                    });
                };
                nearRow.typeStart = FormatValue("%.2f", own > 0.0f ? own : kNearClipDefault);
                nearRow.typeMin = 0.0f;
                nearRow.typeMax = kNearClipMax;
                const CameraLiveInfo& live = g_snapshot.cameraLive;
                nearRow.help = std::string(KeyFill(TR("marker.near_clip.help", "How close the camera gets before things are cut, in metres (0.01 to 1). Lower "
                                                      "gets closer; under about 0.05 m far edges can flicker. {key_select} types it, 0 is the default."))) +
                    ((live.flags & CameraLiveNearRead) ? TRF("marker.near_clip.reads", " The camera reads {value} m now.", {{"value", replay::i18n::Printf("%.3f", live.nearRead)}}) : std::string());
                rows.push_back(nearRow);
            }
            if (free)
            {
                MenuRow look{TR("marker.look_at", "Look At Target"), TargetLabel(k.lookAtTarget), true, 0};
                look.step = [k](int step) {
                    SendOptions(k, CameraOptionLookAtTarget, [&](CameraKeyframeOptions& o) { o.lookAtTarget = StepTargetOrNone(o.lookAtTarget, step); });
                };
                look.help = TR("marker.look_at.help", "The camera turns to keep this target in frame (framing set in Edit Camera).");
                rows.push_back(look);
                if (k.lookAtTarget)
                    AddBoneRow(rows, TR("marker.look_at_bone", "Look At Bone"), k, k.lookAtTarget, false);
                MenuRow attach{TR("marker.attach", "Attach Target"), TargetLabel(k.attachTarget), true, 0};
                attach.step = [k](int step) {
                    SendOptions(k, CameraOptionAttachTarget, [&](CameraKeyframeOptions& o) { o.attachTarget = StepTargetOrNone(o.attachTarget, step); });
                };
                attach.help = TR("marker.attach.help", "The camera moves with this target.");
                rows.push_back(attach);
                if (k.attachTarget)
                {
                    MenuRow mount{TR("marker.mount", "Mount Type"), NameOf(kAttachNames, std::clamp(static_cast<int>(k.attachType), 0, 2)), true, 0};
                    mount.step = [k](int step) {
                        SendOptions(k, CameraOptionAttachType, [&](CameraKeyframeOptions& o) { o.attachType = Cycle(o.attachType, step, 3); });
                    };
                    mount.help = TR("marker.mount.help", "Rigid turns with the target in every axis, Default follows its heading, Flexible only its position.");
                    rows.push_back(mount);
                    // The bone and the attach pose as numbers.
                    AddBoneRow(rows, TR("marker.attach_bone", "Attach Bone"), k, k.attachTarget, true);
                    AddPoseRow(rows, TR("pose.offset_right", "Offset Right"), k, false, 0, TR("pose.offset_right.help", "The camera's place in the attach frame: + right of the bone or entity."));
                    AddPoseRow(rows, TR("pose.offset_forward", "Offset Forward"), k, false, 1, TR("pose.offset_forward.help", "+ in front of it."));
                    AddPoseRow(rows, TR("pose.offset_up", "Offset Up"), k, false, 2, TR("pose.offset_up.help", "+ above it."));
                    AddPoseRow(rows, TR("position.pitch", "Pitch"), k, true, 0, TR("pose.pitch.help", "The camera's turn in the attach frame: up and down."));
                    AddPoseRow(rows, TR("position.roll", "Roll"), k, true, 1, TR("pose.roll.help", "Tilt around the view."));
                    AddPoseRow(rows, TR("position.heading", "Heading"), k, true, 2, TR("pose.heading.help", "Left and right (a Look At target overrides the turn)."));
                }
            }
            if (free && !isLast)
            {
                MenuRow blend{TR("marker.blend", "Blend"), NameOf(kBlendNames, std::clamp(static_cast<int>(k.blend), 0, static_cast<int>(kBlendCount) - 1)), true, 0};
                blend.step = [k](int step) {
                    SendOptions(k, CameraOptionBlend, [&](CameraKeyframeOptions& o) { o.blend = Cycle(o.blend, step, kBlendCount); });
                };
                blend.help = TR("marker.blend.help", "How the camera goes to the next marker: a cut, a straight line, smoothed, or Spline (the default): one "
                             "curve through the markers that blend into each other, keeping its speed through each marker. Blends only "
                             "between free cameras.");
                rows.push_back(blend);
                if (k.blend == Blend::Spline)
                {
                    // Speed profile, curve shape, ease, weight, channels.
                    const SplineTuning own = MarkerSpline(k);
                    MenuRow spline{own.overrides ? TR("marker.spline_options.own", "Spline Options (own)") : TR("marker.spline_options", "Spline Options"), ">", true, 1};
                    spline.enter = [n = rows.size()] {
                        g_splineMenuReturn = static_cast<int>(n);
                        g_markerMenu = MarkerMenu::Spline;
                        g_menuFocus = 0;
                    };
                    spline.help = TR("marker.spline_options.help", "Speed profile, curve shape, ease in and out, camera weight and channels of the segment from this "
                                  "marker (each one its own or the Spline Defaults).");
                    rows.push_back(spline);
                }
            }
            // The chain ease is for Linear and Smooth; a Spline segment has its own Ease
            // In and Ease Out (Spline Options).
            const auto chainEase = [](Blend blend) { return blend == Blend::Linear || blend == Blend::Smooth; };
            const bool easeRow = (free && chainEase(k.blend)) || (previous && chainEase(previous->blend));
            if (easeRow)
            {
                MenuRow ease{TR("marker.blend_ease", "Blend Ease"), k.ease ? TR("common.on_cap", "On") : TR("common.off_cap", "Off"), true, 0};
                ease.step = [k](int) {
                    SendOptions(k, CameraOptionEase, [&](CameraKeyframeOptions& o) { o.ease = o.ease ? 0 : 1; });
                };
                ease.help = TR("marker.blend_ease.help", "On: a chain of blends starts slow and ends slow. Off: constant speed.");
                rows.push_back(ease);
            }
            // The shake has its own menu (Shake, at the root).
            if (k.cameraMode != CameraMode::Game)
            {
                char fov[32]{};
                std::snprintf(fov, sizeof(fov), "%.0f  (%.2fX)", k.camera.fov, kMarkerFovDefault / std::max(1.0f, k.camera.fov));
                MenuRow zoom{TR("marker.fov", "Field of View"), fov, true, 0};
                zoom.step = [k](int step) {
                    SendOptions(k, CameraOptionFov, [&](CameraKeyframeOptions& o) {
                        o.fov = std::clamp(std::round(o.fov) - static_cast<float>(step) * kMarkerFovStep, kMarkerFovMin, kMarkerFovMax);
                    });
                };
                zoom.help = KeyFill(TR("marker.fov.help", "Right zooms in ({key_page_up}/{key_page_down}: five degrees)."));
                rows.push_back(zoom);
            }
            if (IsPreset(k.cameraMode))
            {
                MenuRow distance{TR("marker.distance", "Distance"), FormatValue("%.2fx", 0.5 + k.presetDistanceScale), true, 0};
                distance.step = [k](int step) {
                    SendOptions(k, CameraOptionPresetDistance, [&](CameraKeyframeOptions& o) {
                        o.presetDistanceScale = std::clamp(o.presetDistanceScale + step * 0.05f, 0.0f, 1.0f);
                    });
                };
                distance.help = TR("marker.distance.help", "Nearer or farther from the character.");
                rows.push_back(distance);
            }
            if (IsPreset(k.cameraMode) && k.cameraMode != CameraMode::Overhead || (free && k.lookAtTarget))
            {
                MenuRow roll{TR("position.roll", "Roll"), FormatValue("%.0f", k.lookAtRoll), true, 0};
                roll.step = [k](int step) {
                    SendOptions(k, CameraOptionLookAtRoll, [&](CameraKeyframeOptions& o) { o.lookAtRoll = std::clamp(o.lookAtRoll + step, -90.0f, 90.0f); });
                };
                roll.help = TR("marker.roll.help", "Tilt the picture.");
                rows.push_back(roll);
            }
            if (free || IsPreset(k.cameraMode))
            {
                MenuRow editCamera{TR("marker.edit_camera", "Edit Camera"), "", true, 2};
                editCamera.enter = [k] {
                    Command c{};
                    c.keyframeId = k.id;
                    c.enabled = 1;
                    Send(CommandType::EditCameraKeyframe, &c);
                };
                editCamera.help = KeyFill(TR("marker.edit_camera.help", "Frame this marker's shot: {key_fly}, mouse, {key_roll_left}/{key_roll_right} roll, "
                                                                        "{key_page_up}/{key_page_down} zoom, {key_select} to keep, {key_cancel} to cancel."));
                rows.push_back(editCamera);
            }
            return rows;
        }
        if (g_markerMenu == MarkerMenu::Shake)
            return BuildShakeRows(k);
        if (g_markerMenu == MarkerMenu::ShakeAdvanced)
            return BuildShakeAdvancedRows(k);
        if (g_markerMenu == MarkerMenu::Position)
            return BuildPositionRows(k);
        if (g_markerMenu == MarkerMenu::Spline)
            return BuildSplineRows(k);
        if (g_markerMenu == MarkerMenu::SplineDefaults)
            return BuildSplineDefaultRows();
        if (g_markerMenu == MarkerMenu::Tools)
            return BuildToolRows(k);
        if (g_markerMenu == MarkerMenu::Effects)
        {
            MenuRow filter{TR("marker.filter", "Filter"), FilterName(std::min<std::uint32_t>(k.filter, kMarkerFilterCount - 1)), true, 0};
            filter.step = [k](int step) {
                SendOptions(k, CameraOptionFilter, [&](CameraKeyframeOptions& o) {
                    const int n = static_cast<int>(kMarkerFilterCount);
                    o.filter = static_cast<std::uint8_t>((static_cast<int>(o.filter) + step % n + n) % n);
                });
            };
            filter.help = KeyFill(TR("marker.filter.help", "The game's photo mode filters ({key_page_up}/{key_page_down}: five at a time)."));
            rows.push_back(filter);
            if (k.filter)
            {
                MenuRow intensity{TR("marker.filter_intensity", "Filter Intensity"), FormatValue("%.0f", k.filterIntensity), true, 0};
                intensity.step = [k](int step) {
                    SendOptions(k, CameraOptionFilterIntensity, [&](CameraKeyframeOptions& o) {
                        o.filterIntensity = static_cast<std::uint8_t>(std::clamp(static_cast<int>(o.filterIntensity) + step, 0, 100));
                    });
                };
                intensity.help = TR("marker.filter_intensity.help", "0 to 100.");
                rows.push_back(intensity);
            }
            // Exposure and contrast are not exposed yet.
            return rows;
        }
        // Depth of Field
        MenuRow mode{TR("marker.dof.mode", "DOF"), NameOf(kDofNames, std::clamp(static_cast<int>(k.dof), 0, 2)), true, 0};
        mode.step = [k](int step) { SendOptions(k, CameraOptionDof, [&](CameraKeyframeOptions& o) { o.dof = Cycle(o.dof, step, 3); }); };
        mode.help = TR("marker.dof.mode.help", "Default: the game's own. Custom: your focus and blur. None: everything sharp.");
        rows.push_back(mode);
        if (k.dof == DofMode::Custom)
        {
            MenuRow intensity{TR("shake.intensity", "Intensity"), FormatValue("%.0f", k.dofIntensity), true, 0};
            intensity.step = [k](int step) {
                SendOptions(k, CameraOptionDofIntensity, [&](CameraKeyframeOptions& o) { o.dofIntensity = std::clamp(o.dofIntensity + step, 0.0f, 100.0f); });
            };
            intensity.help = TR("marker.dof_intensity.help", "How strong the blur is (0 to 100): a shortcut that sets the f-stop and the lens "
                             "while F-Stop and Lens are on Auto.");
            rows.push_back(intensity);
            // The numbers the engine takes.
            const CameraMarkerExtra ex = MarkerExtra(k);
            const float autoFStop = IntensityFStop(k.dofIntensity);
            MenuRow fstop{TR("marker.fstop", "F-Stop"), ex.dofFStop > 0.0f ? FormatValue("f/%.1f", ex.dofFStop) : TRF("marker.fstop.auto", "Auto (f/{value})", {{"value", replay::i18n::Printf("%.1f", autoFStop)}}), true, 0};
            fstop.step = [k, autoFStop](int step) {
                SendMarkerExtra(k, CameraExtraDofFStop, [&](CameraMarkerExtra& x) {
                    const float base = x.dofFStop > 0.0f ? x.dofFStop : autoFStop;
                    x.dofFStop = std::clamp(base * std::pow(2.0f, static_cast<float>(-step) / 6.0f), kDofFStopMin, kDofFStopMax);
                });
            };
            fstop.typed = [k](double typedValue) {
                const float v = static_cast<float>(typedValue);
                SendMarkerExtra(k, CameraExtraDofFStop, [&](CameraMarkerExtra& x) {
                    x.dofFStop = v <= 0.0f ? 0.0f : std::clamp(v, kDofFStopMin, kDofFStopMax);
                });
            };
            fstop.typeStart = FormatValue("%.1f", ex.dofFStop > 0.0f ? ex.dofFStop : autoFStop);
            fstop.typeMin = 0.0f;
            fstop.typeMax = kDofFStopMax;
            fstop.help = KeyFill(TR("marker.fstop.help", "The aperture: lower is more blur (right opens it, a third of a stop a step). {key_select} types it, 0 is Auto."));
            rows.push_back(fstop);
            const float autoLens = AutoDofLens(k.camera.fov, k.dofIntensity);
            MenuRow lens{TR("marker.lens", "Lens"), ex.dofLensMm > 0.0f ? FormatValue("%.0f mm", ex.dofLensMm) : TRF("marker.lens.auto", "Auto ({value} mm)", {{"value", replay::i18n::Printf("%.0f", autoLens)}}), true, 0};
            lens.step = [k, autoLens](int step) {
                SendMarkerExtra(k, CameraExtraDofLens, [&](CameraMarkerExtra& x) {
                    const float base = x.dofLensMm > 0.0f ? x.dofLensMm : autoLens;
                    x.dofLensMm = std::clamp(std::round(base + static_cast<float>(step) * 5.0f), kDofLensMin, kDofLensMax);
                });
            };
            lens.typed = [k](double typedValue) {
                const float v = static_cast<float>(typedValue);
                SendMarkerExtra(k, CameraExtraDofLens, [&](CameraMarkerExtra& x) {
                    x.dofLensMm = v <= 0.0f ? 0.0f : std::clamp(v, kDofLensMin, kDofLensMax);
                });
            };
            lens.typeStart = FormatValue("%.0f", ex.dofLensMm > 0.0f ? ex.dofLensMm : autoLens);
            lens.typeMin = 0.0f;
            lens.typeMax = kDofLensMax;
            lens.help = KeyFill(TR("marker.lens.help", "The focal length the blur is computed with (35 mm film; it does not zoom: the FOV stays the camera's). Longer "
                        "is more blur. Auto: the FOV's lens times 2 to 2.5. 5 mm a step, {key_select} types it, 0 is Auto."));
            rows.push_back(lens);
            {
                const CameraLiveInfo& live = g_snapshot.cameraLive;
                const bool on = (live.flags & CameraLiveDofActive) != 0;
                std::string range = "-";
                if (on && live.inFocusNear > 0.0f)
                    range = live.inFocusFar > 0.0f ? TRF("marker.in_focus.range", "{near} to {far} m", {{"near", replay::i18n::Printf("%.1f", live.inFocusNear)},
                                                         {"far", replay::i18n::Printf("%.1f", live.inFocusFar)}})
                                                   : TRF("marker.in_focus.infinity", "{near} m to infinity", {{"near", replay::i18n::Printf("%.1f", live.inFocusNear)}});
                MenuRow sharp{TR("marker.in_focus", "In Focus"), range, true, 0};
                sharp.help = TR("marker.in_focus.help", "What stays sharp now (near to far), from the f-stop, the lens and the focus distance the camera got.");
                rows.push_back(sharp);
                std::string read = "-";
                if (on && (live.flags & CameraLiveDofRead))
                    read = FormatValue("f/%.1f", live.dofFStopRead) + FormatValue("  %.0f mm", live.dofLensRead) +
                        (live.focusRead > 0.0f ? FormatValue("  %.1f m", live.focusRead) : std::string(TR("marker.camera_reads.auto", "  auto")));
                MenuRow back{TR("marker.camera_reads", "Camera Reads"), read, true, 0};
                back.help = std::string(TR("marker.camera_reads.help", "What the game's camera holds, read back from it this frame.")) +
                    ((live.flags & CameraLiveFocusPaused) ? TR("marker.camera_reads.paused", " The game's autofocus kept moving the focus, so it is paused for this camera.") : "");
                rows.push_back(back);
            }
            MenuRow focus{TR("marker.focus", "Focus"), NameOf(kFocusNames, std::clamp(static_cast<int>(k.focus), 0, 2)), true, 0};
            focus.step = [k](int step) { SendOptions(k, CameraOptionFocus, [&](CameraKeyframeOptions& o) { o.focus = Cycle(o.focus, step, 3); }); };
            // The core's PlanFocus order (Look At, attach, the player on screen, the game's autofocus).
            focus.help = TR("marker.focus.help", "Auto: the Look At target, else the attach target, else the player on screen, else the game's autofocus. Manual: a distance. Target: a character.");
            rows.push_back(focus);
            if (k.focus == FocusMode::Manual)
            {
                MenuRow distance{TR("marker.focus_distance", "Focus Distance"), FormatValue("%.1f m", k.focusDistance), true, 0};
                distance.step = [k](int step) {
                    SendOptions(k, CameraOptionFocusDistance, [&](CameraKeyframeOptions& o) { o.focusDistance = std::clamp(o.focusDistance + step * 0.1f, 0.5f, 1000.0f); });
                };
                // Enter types the metres.
                distance.typed = [k](double typedValue) {
                    const float v = static_cast<float>(typedValue);
                    SendOptions(k, CameraOptionFocusDistance, [&](CameraKeyframeOptions& o) { o.focusDistance = std::clamp(v, 0.5f, 1000.0f); });
                };
                distance.typeStart = FormatValue("%.1f", k.focusDistance);
                distance.typeMin = 0.5f;
                distance.typeMax = 1000.0f;
                distance.help = KeyFill(TR("marker.focus_distance.help", "0.1 m a step ({key_page_up}/{key_page_down}: 0.5 m). {key_select} types it."));
                rows.push_back(distance);
            }
            if (k.focus == FocusMode::Target)
            {
                // No character chosen focuses the player (index 0), as the core does.
                MenuRow target{TR("marker.focus_target", "Focus Target"), k.focusTarget ? TargetLabel(k.focusTarget) : TR("marker.focus_target.player", "Player"), true, 0};
                target.step = [k](int step) {
                    SendOptions(k, CameraOptionFocusTarget, [&](CameraKeyframeOptions& o) { o.focusTarget = StepTargetOrNone(o.focusTarget, step); });
                };
                target.help = TR("marker.focus_target.help", "The character kept sharp.");
                rows.push_back(target);
            }
        }
        return rows;
    }

    // One menu row in its box: the label, "<  value  >" (or ">" for a
    // submenu), red ink around the focused row, grey ink around the row under
    // the mouse; a click on the left or right half steps.
    // `pointed` (a menu that does not scroll): set when the mouse moved onto
    // this row, which then takes the focus.
    int MenuRowDraw(ImDrawList* draw, int n, float y, const MenuRow& row, bool focused, bool& clickedEnter, float lead = 0.0f,
        bool* pointed = nullptr)
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

    // ---------------- Environment ----------------
    // The clip's clock, weather and clouds: "Recorded" (the clip's own, every
    // setting's first value) or an override the core applies at the playhead
    // (env_override.h), during playback and the export only.
    float EnvironmentPanelH()
    {
        return 26.0f + 4 * 38.0f;
    }

    void SendEnvironmentStep(EnvironmentField field, int step)
    {
        Command command{};
        command.keyframeId = static_cast<std::uint32_t>(field);
        command.direction = step < 0 ? -1 : 1;
        Send(CommandType::SetEnvironment, &command);
    }

    // The section title and its four rows from `y`; returns the y under them.
    float DrawEnvironmentRows(ImDrawList* draw, float y, bool playing)
    {
        UI_PROBE_SCOPE("Environment");
        namespace env = replay::envoverride;
        SectionTitle(draw, 40.0f, y + 4.0f, 300.0f, TR("env.title", "ENVIRONMENT"));
        y += 26.0f;
        const bool editable = playing && (g_snapshot.envFlags & EnvironmentEditable) != 0;
        const float lead = EditIconLead();
        std::string clock = TR("env.recorded", "Recorded");
        if (g_snapshot.envClockMinutes >= 0)
            clock = replay::i18n::Printf("%02d:%02d", g_snapshot.envClockMinutes / 60, g_snapshot.envClockMinutes % 60);
        else if (g_snapshot.envClockMinutes == env::kClockLive) // the game's clock (another mod may set it)
            clock = TR("env.live", "Game (live)");
        if (const int step = ValueRow(draw, "##envclock", y, TR("env.time", "Time of day"), clock.c_str(), editable, true, lead, ui_art::Art::ToolWeather))
            SendEnvironmentStep(EnvironmentField::Clock, step);
        y += 38.0f;
        const bool weatherLive = g_snapshot.envWeather == env::kWeatherLive;
        const int weather = g_snapshot.envWeather && !weatherLive ? env::WeatherIndex(g_snapshot.envWeather) : -1;
        const char* weatherText = weatherLive ? TR("env.live", "Game (live)") : weather >= 0 ? TR(env::kWeathers[weather].key, env::kWeathers[weather].label)
                                                                                     : TR("env.recorded", "Recorded");
        if (const int step = ValueRow(draw, "##envweather", y, TR("gallery.detail.weather", "Weather"), weatherText, editable, true, lead, ui_art::Art::ToolWeather))
            SendEnvironmentStep(EnvironmentField::Weather, step);
        y += 38.0f;
        std::string blend = TR("shake.transition.cut", "Cut");
        if (g_snapshot.envBlendS)
            blend = std::to_string(g_snapshot.envBlendS) + " s";
        // The blend is the override weather's way in: nothing to blend into
        // while the weather is the recorded one or the game's.
        if (const int step = ValueRow(draw, "##envblend", y, TR("env.weather_blend", "Weather blend"), blend.c_str(),
                editable && g_snapshot.envWeather != 0 && !weatherLive, true, lead))
            SendEnvironmentStep(EnvironmentField::Blend, step);
        y += 38.0f;
        // A clip from before the cloud track never pinned them: Live, fixed.
        const bool sky = (g_snapshot.envFlags & EnvironmentSkyTrack) != 0;
        const char* clouds = !sky || g_snapshot.envCloudsLive ? TR("env.clouds.live", "Live") : TR("env.recorded", "Recorded");
        if (const int step = ValueRow(draw, "##envclouds", y, TR("env.clouds", "Clouds"), clouds, editable && sky, false, lead))
            SendEnvironmentStep(EnvironmentField::Clouds, step);
        y += 38.0f;
        return y;
    }

    // True while the marker menu has the keyboard (the edit screen's other
    // keys stand back).
    bool g_markerMenuActive = false;
    bool g_backConsumed = false;

    // The marker selection the panel last saw and when (a selection the playhead
    // left closes 700 ms after it was made).
    std::uint32_t g_seenSelection = 0;
    ULONGLONG g_selectedAt = 0;
    // Where the seek onto the selected first marker stopped, when the core could not
    // reach it (UINT64_MAX: not needed). The core's earliest playhead is the clip's
    // opening hold (replay_transport.h SetStartMs, ApplyClipTrim): a first marker
    // before it (a project clip's start at its in point 0, a clip's markers saved
    // with another hold) is sought to that hold, and the playhead never stands on
    // the marker itself.
    std::uint64_t g_selectionLandMs = UINT64_MAX;
    // The longest opening hold (replay_playback.cpp: the census over 100 ms, the
    // first poses within 1000 ms of the first create), with room.
    constexpr std::uint64_t kOpeningHoldMaxMs = 2000;

    void DrawEditPanel(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("EditPanel");
        g_valueRowHovered = false;
        g_fxTypingFrame = false;
        // Another clip (or the buffer played): the selection, its menu and an add
        // waiting to be selected belonged to the one before (marker ids start at 1
        // in every clip, so a stale id would open the new clip's marker of that
        // number).
        {
            // By the core's load sequence: a rename of the open clip or the first save
            // of a buffer clip changes its name, not the clip, and keeps the marker's
            // menu open.
            static std::uint32_t seenLoad = ~0u;
            if (seenLoad != g_snapshot.clipLoadSequence)
            {
                seenLoad = g_snapshot.clipLoadSequence;
                g_selectedKeyframeId = 0;
                g_markerMenu = MarkerMenu::Root;
                g_menuFocus = 0;
                g_selectAdded = false;
                ForgetPendingMarkerValues();
            }
        }
        SelectAddedKeyframe();
        // The marker's menu closes when the playhead leaves the marker (play, a
        // click on the bar, a scrub, a frame step): Enter adds again and the arrows
        // step frames. A selection just made waits for its seek (the snapshot lags
        // it).
        {
            const ULONGLONG now = GetTickCount64();
            if (g_selectedKeyframeId != g_seenSelection)
            {
                g_seenSelection = g_selectedKeyframeId;
                g_selectedAt = now;
                g_selectionLandMs = UINT64_MAX;
            }
            if (const CameraKeyframe* key = FindKeyframe(g_selectedKeyframeId))
            {
                const std::uint64_t playhead = g_snapshot.playheadMs;
                std::uint64_t d = key->timeMs > playhead ? key->timeMs - playhead : playhead - key->timeMs;
                const bool running = !Flag(SnapshotPaused);
                const bool settled = g_draggingKeyframeId == 0 && now - g_selectedAt > 700;
                // The first marker before the opening hold: the paused playhead its seek
                // left at the hold (after the marker, before the second one) stands for
                // the marker, until the playhead leaves that spot.
                const CameraKeyframe* keys = g_snapshot.cameraKeyframes;
                const std::uint32_t count = g_snapshot.cameraKeyframeCount;
                if (settled && !running && d > 66 && g_selectionLandMs == UINT64_MAX && count > 0 && key == &keys[0] &&
                    playhead > key->timeMs && playhead - key->timeMs <= kOpeningHoldMaxMs && (count < 2 || playhead < keys[1].timeMs))
                {
                    g_selectionLandMs = playhead;
                }
                if (g_selectionLandMs != UINT64_MAX)
                    d = std::min(d, g_selectionLandMs > playhead ? g_selectionLandMs - playhead : playhead - g_selectionLandMs);
                if (settled && (running || d > 66))
                {
                    g_selectedKeyframeId = 0;
                    g_seenSelection = 0;
                    g_selectionLandMs = UINT64_MAX;
                }
            }
        }
        const bool playing = Flag(SnapshotPlaying);
        const CameraKeyframe* selected = FindKeyframe(g_selectedKeyframeId);
        int index = -1;
        for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
            if (selected && g_snapshot.cameraKeyframes[n].id == selected->id)
                index = static_cast<int>(n);
        // Only when the marker selection changes. With no marker selected the panel
        // can hold the free camera's menu, and resetting every frame would take the
        // focus back to the first row.
        if ((selected ? selected->id : 0u) != g_menuMarker)
        {
            g_rowTyping.on = false;
            g_markerMenu = MarkerMenu::Root;
            g_menuFocus = 0;
            g_menuMarker = selected ? selected->id : 0;
        }
        if (selected)
        {
            g_freeCamMenuOpen = false; // a marker's menu takes the panel
            g_splineDefaultsOpen = false;
        }
        if (!g_freeCamMenuOpen)
            g_splineDefaultsOpen = false; // it lives inside the free camera's menu
        g_markerMenuActive = selected != nullptr || g_freeCamMenuOpen;

        std::vector<MenuRow> rows;
        std::string title;
        if (!selected && g_freeCamMenuOpen)
        {
            rows = BuildFreeCamRows();
            title = Flag(SnapshotFreeCamEnabled) ? TR("edit.title.freecam_on", "FREE CAMERA  (on)") : TR("edit.title.freecam_off", "FREE CAMERA  (off)");
        }
        if (!selected && g_splineDefaultsOpen)
        {
            rows = BuildSplineDefaultRows();
            title = TR("edit.title.spline_defaults", "SPLINE DEFAULTS");
        }
        if (selected)
        {
            rows = BuildMarkerRows(*selected, index);
            const int count = static_cast<int>(g_snapshot.cameraKeyframeCount);
            char at[24]{};
            FormatClock(selected->timeMs, at, sizeof(at), true);
            const char* name = index == 0 ? TR("edit.title.start", "START") : index == count - 1 && count > 1 ? TR("edit.title.end", "END")
                                                                                                            : TR("edit.title.marker", "MARKER");
            const char* sub = g_markerMenu == MarkerMenu::Camera ? TR("edit.title.camera", "  CAMERA") : g_markerMenu == MarkerMenu::Dof ? TR("edit.title.dof", "  DEPTH OF FIELD")
                : g_markerMenu == MarkerMenu::Effects ? TR("edit.title.effects", "  EFFECTS") : g_markerMenu == MarkerMenu::Position ? TR("edit.title.position", "  POSITION")
                : g_markerMenu == MarkerMenu::Tools ? TR("edit.title.move", "  MOVE") : g_markerMenu == MarkerMenu::Spline ? TR("edit.title.spline", "  SPLINE")
                : g_markerMenu == MarkerMenu::SplineDefaults ? TR("edit.title.spline_defaults_sub", "  SPLINE DEFAULTS") : g_markerMenu == MarkerMenu::Shake ? TR("edit.title.shake", "  SHAKE")
                : g_markerMenu == MarkerMenu::ShakeAdvanced ? TR("edit.title.shake_advanced", "  SHAKE ADVANCED") : "";
            title = TRF("edit.title.marker_line", "{name} {n}  {time}{menu}", {{"name", name}, {"n", std::to_string(index + 1)}, {"time", at}, {"menu", sub}});
        }
        // Render export: the Export button; the ENVIRONMENT rows. A long list
        // scrolls with the focus (the rows the screen holds).
        const int fits = std::max(4, static_cast<int>((g_vh - g_footH - 140.0f - 260.0f) / 38.0f));
        const bool homePanel = !selected && !g_freeCamMenuOpen;
        // The home panel's own height: four rows from 128, the environment, the note
        // and the help paragraph measured, the buttons and the panel's foot under
        // them (at small resolutions the readable minimum makes them taller); the
        // panel kPanelPad around the rows.
        const char* const kEditNote =
            KeyFill(TR("edit.note", "{key_select} adds a marker at the playhead, with the settings of the marker before it (the free camera's view "
            "when it is on). Click a gold mark or press {key_next_marker} to open its menu."));
        const float buttonsAt = std::max(100.0f, 4.0f + ParagraphHeight(16.0f, kEditNote, kEditW) + 8.0f);
        float height = homePanel ? 128.0f + 4 * 38.0f + EnvironmentPanelH() + 12.0f + buttonsAt + 74.0f + 14.0f - 20.0f
            : 150.0f + std::clamp<std::size_t>(rows.size(), 4, static_cast<std::size_t>(fits)) * 38.0f + 90.0f;
        // A typed number's refusal takes a line under its row.
        const bool typeError = g_rowTyping.on && g_rowTyping.error[0];
        if (!homePanel && typeError)
            height += kTypeErrorH;
        if (!homePanel && !rows.empty())
        {
            // The rows' menus fit their content as drawn below: the rows from 140, the
            // divider, the focused row's help paragraph from 18 under the last row, and
            // the panel's foot under it (the panel starts at 20).
            const std::size_t shownRows = std::min(rows.size(), static_cast<std::size_t>(fits));
            const std::string& help = rows[std::clamp<std::size_t>(static_cast<std::size_t>(std::max(0, g_menuFocus)), 0, rows.size() - 1)].help;
            height = 140.0f + static_cast<float>(shownRows) * 38.0f + (typeError ? kTypeErrorH : 0.0f) + 18.0f +
                ParagraphHeight(15.0f, help.c_str(), kEditW) + kPanelPadBottom - 20.0f;
        }
        const float panelLeft = kEditX - kPanelPad, panelRight = kEditX + kEditW + kPanelPad;
        InkPanel(draw, panelLeft, 20.0f, panelRight, 20.0f + height, kPanel, kInkEdge, false);
        TitleBanner(draw, panelLeft, 36.0f, 0.0f, 50.0f, 44.0f, panelLeft + 14.0f, 42.0f, 30.0f, "REDEMPTION EDITOR");
        OrnamentDivider(draw, 40.0f, 96.0f, 300.0f);

        if (homePanel)
        {
            SectionTitle(draw, 40.0f, 106.0f, 300.0f, TR("edit.camera", "CAMERA"));
            float y = 128.0f;
            const float lead = EditIconLead();
            if (ValueRow(draw, "##free", y, TR("edit.free_camera", "Free camera"), Flag(SnapshotFreeCamEnabled) ? TR("common.on_cap", "On") : TR("common.off_cap", "Off"), playing, false,
                    lead, ui_art::Art::ToolCamera) != 0)
                Send(CommandType::ToggleFreeCam);
            y += 38.0f;
            // Drone, follow, Walk, the world markers...
            if (ValueRow(draw, "##freeopts", y, TR("edit.free_camera_options", "Free camera options"),
                    (std::string(g_snapshot.freeCamMotion ? TR("freecam.flight.drone", "Drone") : TR("freecam.flight.classic", "Classic")) + "  >").c_str(), true, false, lead) != 0)
            {
                PlayUi(UiSound::Select);
                g_freeCamMenuOpen = true;
                g_menuFocus = 0;
            }
            y += 38.0f;
            if (ValueRow(draw, "##track", y, TR("edit.markers", "Markers"), Flag(SnapshotCameraTrackEnabled) ? TR("common.on_cap", "On") : TR("common.off_cap", "Off"), playing, false, lead) != 0)
            {
                Command command{};
                command.enabled = Flag(SnapshotCameraTrackEnabled) ? 0u : 1u;
                Send(CommandType::SetCameraTrackEnabled, &command);
            }
            y += 38.0f;
            char speed[16]{};
            std::snprintf(speed, sizeof(speed), "%u%%", g_snapshot.speedPercent);
            if (const int step = ValueRow(draw, "##speed", y, TR("marker.speed", "Speed"), speed, playing, true, lead))
                StepPlaybackSpeed(step);
            y += 38.0f;
            y = DrawEnvironmentRows(draw, y, playing);
            y += 12.0f;
            OrnamentDivider(draw, 40.0f, y - 10.0f, 300.0f);
            Paragraph(draw, 40.0f, y + 4.0f, 16.0f, kMuted, kEditNote, kEditW);
            ImGui::SetCursorScreenPos(P(40.0f, y + buttonsAt));
            if (ItemClicked(ImGui::InvisibleButton("##addkey", ImVec2(S(220.0f), S(32.0f)))))
                AddKeyframeHere();
            Brush(draw, 32.0f, y + buttonsAt, 220.0f, 32.0f, ImGui::IsItemHovered() ? kRed : kRedDim);
            Text(draw, 40.0f, y + buttonsAt + 6.0f, 18.0f, kText, TR("edit.add_marker", "Add marker here"));
            // Render export: the export dialog.
            ImGui::SetCursorScreenPos(P(40.0f, y + buttonsAt + 42.0f));
            if (ItemClicked(ImGui::InvisibleButton("##export", ImVec2(S(220.0f), S(32.0f)))))
            {
                PlayUi(UiSound::Select);
                g_screen = Screen::Export;
                exportui::g_back = Screen::Edit;
                if (exportui::g_range == 2)
                    exportui::g_range = 1;
            }
            Brush(draw, 32.0f, y + buttonsAt + 42.0f, 220.0f, 32.0f, ImGui::IsItemHovered() ? kRed : kRedDim);
            const float exportLead = ui_art::Get(ui_art::Art::MenuExport) ? 28.0f : 0.0f;
            MenuIcon(draw, ui_art::Art::MenuExport, 50.0f, y + buttonsAt + 58.0f, 22.0f, true);
            const char* const exportLabel = KeyFill(TR("edit.export", "Export video ({key_export})"));
            // Past the icon a long label is set smaller to stay on the button.
            if (exportLead > 0.0f)
                RowTextFit(draw, 40.0f + exportLead, y + buttonsAt + 48.0f, 18.0f, kText, exportLabel, 220.0f - exportLead - 4.0f);
            else
                Text(draw, 40.0f, y + buttonsAt + 48.0f, 18.0f, kText, exportLabel);
            return;
        }

        RowTextFit(draw, 40.0f, 108.0f, 18.0f, kGold, title.c_str(), kEditW);
        const int count = static_cast<int>(rows.size());
        // A Spline row's typed number (the row it was started on).
        const bool rowTyping = g_rowTyping.on && count > 0 && g_menuFocus < count && rows[g_menuFocus].typed &&
            rows[g_menuFocus].label == g_rowTyping.label;
        if (!rowTyping)
            g_rowTyping.on = false;
        const bool typing = rowTyping; // the typed number
        if (!typing)
            NavigateList(g_menuFocus, count);
        g_menuFocus = std::clamp(g_menuFocus, 0, std::max(0, count - 1));
        const int first = count > fits ? std::clamp(g_menuFocus - fits / 2, 0, count - fits) : 0;
        // A menu with tools' icons (the marker's root) sets every label past their column.
        const bool menuIcons = std::any_of(rows.begin(), rows.end(), [](const MenuRow& row) { return row.icon != ui_art::Art::Count; });
        const float lead = menuIcons ? EditIconLead() : 0.0f;
        float y = 140.0f;
        for (int n = first; n < std::min(count, first + fits); ++n)
        {
            bool clickedEnter = false;
            bool pointed = false; // the mouse moved onto the row (a menu that does not scroll)
            const int step = typing ? 0 : MenuRowDraw(draw, n, y, rows[n], n == g_menuFocus, clickedEnter, lead, count <= fits ? &pointed : nullptr);
            if (pointed)
                g_menuFocus = n;
            if (typing)
            {
                MenuRowDraw(draw, n, y, rows[n], n == g_menuFocus, clickedEnter, lead);
                clickedEnter = false;
                if (n == g_menuFocus && rowTyping)
                    DrawRowTypeBox(draw, y, rows[n]);
                // The refusal under the box takes a line of its own.
                if (n == g_menuFocus && rowTyping && g_rowTyping.error[0] != 0)
                    y += kTypeErrorH;
            }
            if (step != 0 || clickedEnter)
                g_menuFocus = n;
            if (step != 0 && rows[n].step)
            {
                PlayUiStep(step);
                rows[n].step(step);
            }
            if (clickedEnter && rows[n].enter)
            {
                PlayUi(UiSound::Select);
                rows[n].enter();
            }
            y += 38.0f;
        }
        if (rowTyping)
        {
            // The box has the keys (Enter, Esc, the digits).
        }
        else if (count > 0 && rows[g_menuFocus].enabled)
        {
            const MenuRow& row = rows[g_menuFocus];
            int step = 0;
            const bool ctrl = ImGui::GetIO().KeyCtrl; // Ctrl+arrows are the clip's start and end
            if (!ctrl && KeyPressed(ImGuiKey_LeftArrow))
                step = -1;
            if (!ctrl && KeyPressed(ImGuiKey_RightArrow))
                step = 1;
            if (KeyPressed(ImGuiKey_PageUp))
                step = 5;
            if (KeyPressed(ImGuiKey_PageDown))
                step = -5;
            if (row.label == TR("marker.time", "Time") && (step == 5 || step == -5))
                step *= 2; // ten frames
            if (step != 0 && row.kind == 0 && row.step)
            {
                PlayUiStep(step);
                row.step(step);
            }
            if (Confirmed())
            {
                const bool runs = static_cast<bool>(row.enter) || static_cast<bool>(row.step);
                if (runs && row.typed)
                {
                    PlayUi(UiSound::Select);
                    StartRowTyping(row);
                }
                else if (runs && row.enter)
                {
                    PlayUi(UiSound::Select);
                    row.enter();
                }
                else if (runs && row.step)
                {
                    PlayUiStep(1);
                    row.step(1);
                }
            }
        }
        else if (count > 0 && (Confirmed() ||
                                  (!ImGui::GetIO().KeyCtrl && (KeyPressed(ImGuiKey_LeftArrow, false) || KeyPressed(ImGuiKey_RightArrow, false)))))
            PlayUi(UiSound::Error); // the arrows on a greyed row too
        OrnamentDivider(draw, 40.0f, y + 6.0f, 300.0f);
        if (count > 0)
            Paragraph(draw, 40.0f, y + 18.0f, 15.0f, kMuted, rows[g_menuFocus].help.c_str(), 300.0f);
        if (!rowTyping && Back())
        {
            PlayUi(UiSound::Back);
            if (g_splineDefaultsOpen && !selected)
            {
                g_splineDefaultsOpen = false; // back to the free camera's menu, on its row
                g_menuFocus = static_cast<int>(BuildFreeCamRows().size()) - 1;
            }
            else if (g_freeCamMenuOpen && !selected)
                g_freeCamMenuOpen = false;
            else if (g_markerMenu == MarkerMenu::SplineDefaults)
            {
                g_markerMenu = MarkerMenu::Spline;
                g_menuFocus = g_splineDefaultsReturn;
            }
            else if (g_markerMenu == MarkerMenu::Spline)
            {
                g_markerMenu = MarkerMenu::Camera;
                g_menuFocus = g_splineMenuReturn;
            }
            else if (g_markerMenu == MarkerMenu::ShakeAdvanced)
            {
                g_markerMenu = MarkerMenu::Shake;
                g_menuFocus = g_shakeAdvancedReturn;
            }
            else if (g_markerMenu != MarkerMenu::Root)
            {
                g_menuFocus = g_markerMenu == MarkerMenu::Camera ? 0 : g_markerMenu == MarkerMenu::Dof ? 1
                    : g_markerMenu == MarkerMenu::Effects ? 2 : g_menuReturnFocus;
                g_markerMenu = MarkerMenu::Root;
            }
            else
                g_selectedKeyframeId = 0; // the menu closes; Esc again goes home
            g_backConsumed = true;
        }
    }

    // The timeline's zoom (Ctrl + wheel over it): the window of the clip shown,
    // centred on the playhead.
    double g_timelineZoom = 1.0;
    double g_viewStartMs = 0.0;
    double g_viewSpanMs = 1.0;

    std::uint64_t TimeAt(float mouseX, float left, float width)
    {
        const float t = width > 1.0f ? std::clamp((mouseX - S(left)) / S(width), 0.0f, 1.0f) : 0.0f;
        return static_cast<std::uint64_t>(g_viewStartMs + t * g_viewSpanMs + 0.5);
    }

    // The edit timeline: the playback bar, thin and red, with the camera
    // keyframes on it; a diamond where a shot starts (a cut) or a key stands
    // apart, a tick for the keys inside a directed shot. Click or drag the bar
    // to seek, a mark to select or move.
    void DrawEditTimeline(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("EditTimeline");
        const float left = 40.0f;
        const float right = g_vw - 40.0f;
        const float width = right - left;
        // Over the key bar.
        const float y = g_vh - g_footH - 54.0f; // the bar
        // A soft band behind the bar keeps it readable over the game.
        draw->AddRectFilledMultiColor(P(0.0f, y - 72.0f), P(g_vw, g_vh), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
            IM_COL32(8, 7, 6, 200), IM_COL32(8, 7, 6, 200));

        const double clipMs = g_snapshot.durationMs ? static_cast<double>(g_snapshot.durationMs) : 1.0;
        // A new clip starts unzoomed.
        static std::uint32_t zoomClip = ~0u; // per clip load, not per name
        if (zoomClip != g_snapshot.clipLoadSequence)
        {
            zoomClip = g_snapshot.clipLoadSequence;
            g_timelineZoom = 1.0;
        }
        g_viewSpanMs = clipMs / g_timelineZoom;
        // A drag the timeline never saw end (F2, Esc, H or Edit Camera while the
        // button was held) would hold the window and keep the selection from
        // closing.
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            // This runs before the release check below, which would clear the drag on
            // the release frame first: the drag's end (kSeekDragEnd) is sent here, once.
            if (g_draggingPlayhead && g_lastDragTimeMs != UINT64_MAX)
            {
                Command command{};
                command.timeMs = g_lastDragTimeMs;
                command.enabled = kSeekDragEnd;
                Send(CommandType::SeekAbsolute, &command);
            }
            g_draggingKeyframeId = 0;
            g_draggingPlayhead = false;
        }
        // The window holds while the mouse drags (re-centred on every seek, the same
        // mouse x would mean a later time each frame).
        if (!g_draggingPlayhead && !g_draggingKeyframeId)
            g_viewStartMs = std::clamp(static_cast<double>(g_snapshot.playheadMs) - g_viewSpanMs * 0.5, 0.0, clipMs - g_viewSpanMs);
        const double duration = g_viewSpanMs; // below: times map through the window (TimeX)
        const auto TimeX = [&](double timeMs) { return left + static_cast<float>((timeMs - g_viewStartMs) / duration) * width; };
        const float amount = std::clamp(static_cast<float>((g_snapshot.playheadMs - g_viewStartMs) / duration), 0.0f, 1.0f);
        const float playX = left + amount * width;
        // The painted timeline: a torn track, the played part a torn red strip, the
        // marks on the bar as icons, the playhead a watch crown. Without its art,
        // the flat timeline.
        const bool painted = TimelineArtReady();
        if (painted)
        {
            TornStrip(draw, ui_art::Art::BarTorn, left, left, right, y + kTrackTop, y + kTrackBottom, kTrackTint);
            TornStrip(draw, ui_art::Art::BarRed, left, left, playX, y + kTrackTop, y + kTrackBottom, IM_COL32(255, 255, 255, 255));
            // The crown is drawn once the marks are laid out (below): it stands over
            // the selected marker's ring when the playhead is on it.
        }
        else
        {
            draw->AddRectFilled(P(left, y), P(right, y + 5.0f), IM_COL32(236, 230, 218, 70));
            draw->AddRectFilled(P(left, y), P(playX, y + 5.0f), kRed);
            // The playhead: a line through the bar and a small head under it.
            // Drawn before the marks, so its ring never covers a marker added at the
            // playhead.
            draw->AddLine(P(playX, y - 6.0f), P(playX, y + 9.0f), kText, S(2.0f));
            draw->AddTriangleFilled(P(playX - 5.0f, y + 15.0f), P(playX + 5.0f, y + 15.0f), P(playX, y + 8.0f), kText);
        }

        // Interaction area: the bar and the marks above it (the stacked
        // lanes included).
        ImGui::SetCursorScreenPos(P(left, y - 48.0f));
        ImGui::InvisibleButton("##timeline", ImVec2(S(width), S(60.0f)));
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 mouse = ImGui::GetMousePos();

        // Markers too close to share a spot stack in lanes above the bar instead of
        // drawing over each other (a tick on the bar when all the lanes are taken);
        // the selected one is drawn last, larger, ringed, with a stem down to the
        // bar and its number over it. Painted, the first lane is the bar's middle
        // (the icons sit on the bar); the hit circles move with them, the same size.
        const float markY = painted ? y + (kTrackTop + kTrackBottom) * 0.5f : y - 11.0f;
        constexpr int kLanes = 3;
        constexpr float kLaneStep = 13.0f;
        constexpr float kMarkGap = 13.0f; // two diamonds side by side
        struct MarkLayout
        {
            float x = 0.0f;
            float y = 0.0f;
            bool tick = false; // no lane left: a tick on the bar
            bool hidden = false; // outside the zoomed window
        };
        const std::uint32_t keyCount = g_snapshot.cameraKeyframeCount;
        std::vector<MarkLayout> marks(keyCount);
        float laneEnd[kLanes];
        for (float& end : laneEnd)
            end = -1.0e9f;
        const float endX = keyCount >= 2
            ? TimeX(static_cast<double>(g_snapshot.cameraKeyframes[keyCount - 1].timeMs)) : 1.0e9f;
        std::uint32_t hoveredKey = 0;
        float best = S(painted ? 11.0f : 9.0f); // painted, about the icon's size
        for (std::uint32_t n = 0; n < keyCount; ++n)
        {
            const CameraKeyframe& key = g_snapshot.cameraKeyframes[n];
            MarkLayout& mark = marks[n];
            mark.x = TimeX(static_cast<double>(key.timeMs));
            mark.hidden = mark.x < left - 1.0f || mark.x > right + 1.0f;
            if (mark.hidden)
                continue;
            const bool bound = keyCount >= 2 && (n == 0 || n + 1 == keyCount);
            int lane = 0;
            if (!bound)
            {
                // Lane 0 is also taken next to the end bracket.
                while (lane < kLanes && (mark.x - laneEnd[lane] < kMarkGap || (lane == 0 && endX - mark.x < kMarkGap)))
                    ++lane;
            }
            if (lane < kLanes)
            {
                laneEnd[lane] = mark.x;
                mark.y = markY - lane * kLaneStep;
            }
            else
            {
                mark.tick = key.id != g_selectedKeyframeId;
                mark.y = markY; // the selected one is drawn over the others
            }
            if (!mark.tick)
            {
                const float d = std::hypot(mouse.x - S(mark.x), mouse.y - S(mark.y));
                if (d < best)
                {
                    best = d;
                    hoveredKey = key.id;
                }
            }
        }
        // The painted playhead. On the selected marker (the playhead goes there when
        // a marker is selected) the crown would sit on the marker's ring: it rises
        // instead, its needle's tip on the ring's top, a little smaller to stay clear
        // of the shake lane, and the marker's number goes beside the crown. It eases
        // up over the last few units as the playhead nears the marker. Drawing only:
        // the hit areas and the drag are the bar's and the marks', as before.
        float crownNumberX = -1.0f, crownNumberY = 0.0f;
        if (painted)
        {
            float lift = 0.0f;
            for (std::uint32_t n = 0; n < keyCount; ++n)
            {
                const MarkLayout& mark = marks[n];
                if (g_snapshot.cameraKeyframes[n].id != g_selectedKeyframeId || mark.hidden || mark.tick)
                    continue;
                const bool bound = keyCount >= 2 && (n == 0 || n + 1 == keyCount);
                if (bound || mark.y == markY) // on the bar (a marker up a lane has its stem)
                    lift = std::clamp((17.0f - std::fabs(mark.x - playX)) / 8.0f, 0.0f, 1.0f);
                break;
            }
            const float tipY = (y + kTrackBottom + 2.0f) + lift * ((markY - 13.0f) - (y + kTrackBottom + 2.0f));
            const float crownH = 38.0f - lift * 3.0f;
            PlayheadIcon(draw, playX, tipY, crownH);
            if (lift > 0.5f)
            {
                // The crown's ring: rows 1 to 58 of the art (centre 105 rows over the tip), 56 of its 136 columns.
                crownNumberX = playX + crownH * 28.0f / kPlayheadRows + 5.0f;
                crownNumberY = tipY - crownH * 105.0f / kPlayheadRows - LabelHeight(14.0f) * 0.5f;
            }
        }
        const auto drawMark = [&](std::uint32_t n) {
            const CameraKeyframe& key = g_snapshot.cameraKeyframes[n];
            const MarkLayout& mark = marks[n];
            const float x = mark.x;
            const float my = mark.y;
            if (mark.hidden)
                return;
            if (mark.tick)
            {
                if (painted) // the pin, small, on the bar
                    OutlinedIcon(draw, ui_art::Art::IconKeyTick, x, markY, 14.0f, IM_COL32(222, 222, 222, 200));
                else
                    draw->AddLine(P(x, y - 6.0f), P(x, y - 1.0f), IM_COL32(233, 178, 64, 170), S(1.0f));
                return;
            }
            const bool selected = key.id == g_selectedKeyframeId;
            const bool hot = hovered && key.id == hoveredKey;
            const float r = selected ? 7.0f : hot ? 6.0f : 5.0f;
            if (painted)
            {
                DrawPaintedMark(draw, n, x, my, y, selected, hot, crownNumberX, crownNumberY);
                return;
            }
            const ImU32 color = selected ? IM_COL32(255, 229, 142, 255) : hot ? IM_COL32(245, 205, 110, 255) : kGold;
            const bool bound = keyCount >= 2 && (n == 0 || n + 1 == keyCount);
            if (selected && !bound)
                draw->AddLine(P(x, my + r), P(x, y), IM_COL32(255, 229, 142, 200), S(1.5f));
            if (bound)
            {
                // The clip's start and end: a cut bracket, [ and ].
                const float dir = n == 0 ? 1.0f : -1.0f;
                draw->AddLine(P(x, markY - 9.0f), P(x, y + 5.0f), color, S(selected ? 4.0f : 3.0f));
                draw->AddLine(P(x, markY - 9.0f), P(x + dir * 6.0f, markY - 9.0f), color, S(3.0f));
                draw->AddLine(P(x, y + 5.0f), P(x + dir * 6.0f, y + 5.0f), color, S(3.0f));
            }
            else if (key.markerType == MarkerType::Anchor)
                draw->AddLine(P(x, my - r), P(x, my + r), color, S(2.0f));
            else
                draw->AddQuadFilled(P(x, my - r), P(x + r, my), P(x, my + r), P(x - r, my), color);
            if (selected)
            {
                const float ringY = bound ? markY - 9.0f : my;
                draw->AddCircle(P(x, ringY), S(r + 5.0f), kText, 0, S(1.5f));
                char number[8]{};
                std::snprintf(number, sizeof(number), "%u", n + 1);
                Text(draw, x - TextWidth(14.0f, number) / g_scale * 0.5f, ringY - r - 24.0f, 14.0f, kText, number);
            }
        };

        // Blend bands: a thin gold line from a marker that blends to the next one
        // (straight for Linear, a wave for Smooth); a cut has none.
        for (std::uint32_t n = 0; n + 1 < g_snapshot.cameraKeyframeCount; ++n)
        {
            const CameraKeyframe& key = g_snapshot.cameraKeyframes[n];
            if (key.blend == Blend::None || key.markerType == MarkerType::Anchor)
                continue;
            const float x0 = std::clamp(TimeX(static_cast<double>(key.timeMs)), left, right);
            const float x1 = std::clamp(TimeX(static_cast<double>(g_snapshot.cameraKeyframes[n + 1].timeMs)), left, right);
            const float by = painted ? y + kTrackTop - 5.0f : y - 20.0f; // just over the torn bar
            if (key.blend == Blend::Linear)
                draw->AddLine(P(x0, by), P(x1, by), IM_COL32(233, 178, 64, 200), S(2.0f));
            else
            {
                ImVec2 last = P(x0, by);
                for (float x = x0 + 3.0f; x <= x1; x += 3.0f)
                {
                    const ImVec2 point = P(x, by + std::sin((x - x0) * 0.35f) * 1.6f);
                    draw->AddLine(last, point, IM_COL32(233, 178, 64, 200), S(2.0f));
                    last = point;
                }
            }
        }

        // The marks over the bands, the selected one last (on top).
        std::uint32_t selectedIndex = keyCount;
        for (std::uint32_t n = 0; n < keyCount; ++n)
        {
            if (g_snapshot.cameraKeyframes[n].id == g_selectedKeyframeId)
                selectedIndex = n;
            else
                drawMark(n);
        }
        if (selectedIndex < keyCount)
            drawMark(selectedIndex);

        // The shake lives in the markers: a small cyan dot over each marker whose
        // shake is on (a ring on an Anchor the shake migration added), a thin line on
        // to the next shake marker while it plays. Drawing only.
        {
            constexpr ImU32 kShakeBadge = IM_COL32(110, 190, 235, 255);
            const float laneY = y - 58.0f;
            float lastX = left;
            bool lastOn = false;
            const auto run = [&](float x0, float x1) {
                x0 = std::clamp(x0, left, right);
                x1 = std::clamp(x1, left, right);
                if (x1 > x0)
                    draw->AddLine(P(x0, laneY), P(x1, laneY), IM_COL32(110, 190, 235, 90), S(1.5f));
            };
            for (std::uint32_t n = 0; n < keyCount; ++n)
            {
                const CameraKeyframe& key = g_snapshot.cameraKeyframes[n];
                if (key.markerType == MarkerType::Anchor && key.markerOrigin != 1)
                    continue; // a plain Anchor holds no shake
                const float x = TimeX(static_cast<double>(key.timeMs));
                if (lastOn)
                    run(lastX, x);
                const bool on = g_snapshot.cameraKeyframeShake[n].on != 0;
                if (on && x >= left - 1.0f && x <= right + 1.0f)
                {
                    // Painted, the spur rowel over the marker (dimmer on an Anchor the migration
                    // added; the selected marker's white on its red halo).
                    if (painted && key.id == g_selectedKeyframeId && ui_art::Get(ui_art::Art::IconShakeSelected))
                        Icon(draw, ui_art::Art::IconShakeSelected, x, laneY, 20.0f, IM_COL32(255, 255, 255, 255));
                    else if (painted)
                        Icon(draw, ui_art::Art::IconShake, x, laneY, 15.0f,
                            key.markerOrigin == 1 ? IM_COL32(222, 222, 222, 150) : kIconTint);
                    else if (key.markerOrigin == 1)
                        draw->AddCircle(P(x, laneY), S(3.5f), kShakeBadge, 0, S(1.5f));
                    else
                        draw->AddCircleFilled(P(x, laneY), S(3.0f), kShakeBadge);
                }
                lastX = x;
                lastOn = on;
            }
            if (lastOn)
                run(lastX, TimeX(static_cast<double>(g_snapshot.durationMs)));
        }



        char now[16]{};
        char total[16]{};
        FormatClock(g_snapshot.playheadMs, now, sizeof(now), true);
        FormatClock(g_snapshot.durationMs, total, sizeof(total), true);
        char time[48]{};
        std::snprintf(time, sizeof(time), "%s / %s", now, total);
        // The status line laid out one after the other, each at its own width; when
        // it and the spans' legend are wider than the bar, the clip's name, then the
        // message, are cut with "..." (at 4:3 the legend would leave the screen).
        struct StatusItem
        {
            ImU32 color;
            std::string text;
            float minWidth;
            bool flexible;
        };
        std::vector<StatusItem> items;
        const auto status = [&](ImU32 color, const char* text, float minWidth, bool flexible = false) {
            items.push_back({color, text, minWidth, flexible});
        };
        status(kText, time, 180.0f);
        // A seek's wait and the world's load too: the clip editor's cue is here (no badge over it).
        const bool busy = Flag(SnapshotPreparing) || SeekCueWanted() || Flag(SnapshotLoadingWorld);
        const char* state = busy ? TR("hud.loading", "Loading") : Flag(SnapshotPaused) ? TR("hud.paused", "Paused") : "";
        status(kMuted, state, 100.0f);
        std::string clip;
        if (ProjectFlag(ProjectEditingClip) && g_snapshot.projectActiveClip < g_snapshot.projectClipCount)
            // A project's clip: which one, of how many.
            clip = TRF("edit.status.project_clip", "{project}: clip {at} of {count}, {clip}", {{"project", g_snapshot.projectName},
                {"at", std::to_string(g_snapshot.projectActiveClip + 1)}, {"count", std::to_string(g_snapshot.projectClipCount)},
                {"clip", ClipLabel(g_snapshot.currentClip)}});
        else
            clip = g_snapshot.currentClip[0] ? ClipLabel(g_snapshot.currentClip) : std::string(TR("edit.status.last_recording", "last recording"));
        status(kMuted, clip.c_str(), 100.0f, true);
        if (g_timelineZoom > 1.01)
            status(kMuted, TRF("edit.status.zoom", "Zoom x{zoom}", {{"zoom", replay::i18n::Printf("%.1f", g_timelineZoom)}}).c_str(), 80.0f);
        // Marker changes not in the file yet (Ctrl+S; leaving the clip saves them
        // too).
        if (GetTickCount64() < g_editMessageUntil)
            status(kGold, g_editMessage, 0.0f, true);
        else if (Flag(SnapshotSavingClip))
            status(kGold, TR("edit.status.saving", "Saving..."), 0.0f);
        else if (Flag(SnapshotClipEdited))
            status(kGold, KeyFill(TR("edit.status.unsaved", "Unsaved changes ({key_save})")), 0.0f);
        {
            const auto itemWidth = [](const StatusItem& item) { return std::max(item.minWidth, TextWidth(16.0f, item.text.c_str()) / g_scale + 24.0f); };
            float used = SpanLegendWidth();
            for (const StatusItem& item : items)
                used += itemWidth(item);
            for (StatusItem& item : items)
            {
                const float excess = used - width;
                if (excess <= 0.0f)
                    break;
                if (!item.flexible)
                    continue;
                const float before = itemWidth(item);
                const float textW = TextWidth(16.0f, item.text.c_str()) / g_scale;
                item.text = Ellipsized(16.0f, item.text, std::max(60.0f, textW - excess));
                used += itemWidth(item) - before;
            }
        }
        float statusX = left;
        for (std::size_t n = 0; n < items.size(); ++n)
        {
            const StatusItem& item = items[n];
            Text(draw, statusX, y + 14.0f, 16.0f, item.color, item.text.c_str());
            // The turning cylinder in the gap before "Loading" (the state, item 1).
            if (n == 1 && busy)
                LoadingCue(draw, statusX - 12.0f, y + 14.0f + LabelHeight(16.0f) * 0.5f, 18.0f);
            statusX += std::max(item.minWidth, TextWidth(16.0f, item.text.c_str()) / g_scale + 24.0f);
        }
        // The spans' legend after the status.
        DrawSpanBands(draw, left, width, y + 5.0f, statusX, y + 14.0f, g_viewStartMs, g_viewSpanMs);

        // The wheel over the timeline steps frames (a notch, one frame; with Shift,
        // ten).
        if (hovered && ImGui::GetIO().MouseWheel != 0.0f && ImGui::GetIO().KeyCtrl)
        {
            // Ctrl + wheel zooms the timeline (x1 to x32).
            g_timelineZoom = std::clamp(g_timelineZoom * (ImGui::GetIO().MouseWheel > 0.0f ? 1.5 : 1.0 / 1.5), 1.0, 32.0);
        }
        else if (hovered && ImGui::GetIO().MouseWheel != 0.0f)
        {
            Command step{};
            step.direction = ImGui::GetIO().MouseWheel > 0.0f ? 1 : -1;
            const int count = ImGui::GetIO().KeyShift ? 10 : 1;
            for (int n = 0; n < count; ++n)
                Send(CommandType::StepFrame, &step);
        }
        if (hovered && MouseClicked())
        {
            // A click that came and went inside one frame: the button is up already, so
            // no drag follows; the bar is sought here, once (a marker is selected and
            // sought below as always).
            if (!hoveredKey && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                Command seek{};
                seek.timeMs = TimeAt(mouse.x, left, width);
                Send(CommandType::SeekAbsolute, &seek);
            }
            if (hoveredKey)
            {
                // Selecting a marker sounds like the menus do.
                PlayUi(UiSound::Select);
                g_selectedKeyframeId = hoveredKey;
                g_draggingKeyframeId = hoveredKey;
                g_lastDragTimeMs = UINT64_MAX;
                // The playhead jumps to the key clicked (paused there).
                if (const CameraKeyframe* key = FindKeyframe(hoveredKey))
                {
                    Command seek{};
                    seek.timeMs = key->timeMs;
                    Send(CommandType::SeekAbsolute, &seek);
                    g_lastDragTimeMs = key->timeMs; // a click without a move does not move the key
                    g_dragKeyStartMs = key->timeMs;
                }
                g_dragStartX = mouse.x;
                g_dragMoved = false;
                ForgetPendingMarkerValues(); // the drag moves it from the snapshot's time
            }
            else
            {
                g_draggingPlayhead = true;
                g_lastDragTimeMs = UINT64_MAX;
            }
        }
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && (g_draggingKeyframeId || g_draggingPlayhead))
        {
            std::uint64_t timeMs = TimeAt(mouse.x, left, width);
            // A marker moves only once the mouse has gone 4 px, and by how far it went,
            // so a click anywhere on the mark (9 px around it) never moves it.
            if (g_draggingKeyframeId)
            {
                if (!g_dragMoved && std::fabs(mouse.x - g_dragStartX) < S(4.0f))
                    timeMs = g_lastDragTimeMs;
                else
                {
                    g_dragMoved = true;
                    const std::int64_t delta = static_cast<std::int64_t>(timeMs) -
                        static_cast<std::int64_t>(TimeAt(g_dragStartX, left, width));
                    timeMs = static_cast<std::uint64_t>(std::clamp<std::int64_t>(
                        static_cast<std::int64_t>(g_dragKeyStartMs) + delta, 0, static_cast<std::int64_t>(g_snapshot.durationMs)));
                }
            }
            if (timeMs != g_lastDragTimeMs)
            {
                Command command{};
                command.timeMs = timeMs;
                if (g_draggingKeyframeId)
                {
                    command.keyframeId = g_draggingKeyframeId;
                    Send(CommandType::MoveCameraKeyframe, &command);
                }
                else
                {
                    // The drag is a scrub (cheap, no load) until the mouse is released.
                    command.enabled = kSeekDragging;
                    Send(CommandType::SeekAbsolute, &command);
                }
                g_lastDragTimeMs = timeMs;
            }
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            // The drag's end loads once, where it stopped.
            if (g_draggingPlayhead && g_lastDragTimeMs != UINT64_MAX)
            {
                Command command{};
                command.timeMs = g_lastDragTimeMs;
                command.enabled = kSeekDragEnd;
                Send(CommandType::SeekAbsolute, &command);
            }
            g_draggingKeyframeId = 0;
            g_draggingPlayhead = false;
        }

        // The keys of what has the keyboard (the marker's menu or the timeline) are
        // in the key bar under the timeline (EditTips).
    }

    // Edit Camera: the controls at the foot, a thin cross in the middle, the
    // zoom; Enter keeps the view in the marker, Backspace leaves it as it was.
    bool g_editOverlayArmed = false;

    void DrawEditCameraOverlay(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("EditCamera");
        const float cx = g_vw * 0.5f, cy = g_vh * 0.5f;
        draw->AddLine(P(cx - 14.0f, cy), P(cx + 14.0f, cy), IM_COL32(236, 230, 218, 120), S(1.0f));
        draw->AddLine(P(cx, cy - 14.0f), P(cx, cy + 14.0f), IM_COL32(236, 230, 218, 120), S(1.0f));
        const CameraKeyframe* key = FindKeyframe(g_snapshot.editingMarker);
        const std::string title = TRF("editcam.title", "EDIT CAMERA  {zoom}X ZOOM",
            {{"zoom", replay::i18n::Printf("%.2f", kMarkerFovDefault / std::max(1.0f, g_snapshot.camera.fov))}});
        Brush(draw, 40.0f, 40.0f, TextWidth(26.0f, title.c_str()) / g_scale + 40.0f, 40.0f, kRed);
        Text(draw, 54.0f, 46.0f, 26.0f, kText, title.c_str());
        const bool orbit = key && key->lookAtTarget && key->cameraMode == CameraMode::Free;
        if (orbit)
            Text(draw, 40.0f, 90.0f, 16.0f, kMuted, TR("editcam.orbit", "Orbit around the Look At target; the mouse places it on screen."));
        // Its controls are in the key bar (EditCameraTips). Enter or Backspace only
        // after both were seen up since the overlay came (the Enter that opened Edit
        // Camera, held or repeated, would close it again); Esc cancels too.
        if (!g_runtime->is_key_down(VK_RETURN) && !g_runtime->is_key_down(VK_BACK) && !g_runtime->is_key_down(VK_ESCAPE))
            g_editOverlayArmed = true;
        const bool keep = g_editOverlayArmed && KeyVk(VK_RETURN);
        const bool cancel = g_editOverlayArmed && (KeyVk(VK_BACK) || KeyVk(VK_ESCAPE));
        if (keep || cancel)
        {
            g_editOverlayArmed = false;
            Command command{};
            command.keyframeId = g_snapshot.editingMarker;
            command.enabled = 0;
            command.target = keep ? 0u : 1u;
            Send(CommandType::EditCameraKeyframe, &command);
        }
    }

    void DrawEdit(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("Edit");
        DrawClipFade(draw);
        DrawEditPanel(draw);
        DrawEditTimeline(draw);
        if (g_fxTypingFrame)
            return; // the shake event's typed Value or Time has the keys
        // Ctrl+S saves the clip with its markers.
        if (ImGui::GetIO().KeyCtrl && KeyPressed(ImGuiKey_S, false))
        {
            // A clip with its own file and no changes is not written again (a
            // compressed clip would go back to full size); the core skips it too (and
            // retries a held save). A buffer clip with no file yet is still saved.
            const bool unchanged = Flag(SnapshotPlaying) && g_snapshot.currentClip[0] && !Flag(SnapshotClipEdited);
            if (Send(CommandType::SaveOpenClip))
            {
                PlayUi(UiSound::Select);
                if (unchanged)
                {
                    g_saveAskedAt = 0;
                    EditMessage(TR("edit.message.no_changes", "No changes"));
                }
                else
                {
                    g_saveAskedAt = GetTickCount64();
                    g_saveSeen = false;
                }
            }
        }
        // The save's end is said. A failure keeps "Unsaved changes" and the core's
        // notice; only the success is confirmed here.
        if (g_saveAskedAt)
        {
            const bool saving = Flag(SnapshotSavingClip);
            g_saveSeen = g_saveSeen || saving;
            const ULONGLONG waited = GetTickCount64() - g_saveAskedAt;
            if ((g_saveSeen && !saving) || (!g_saveSeen && waited > 3000))
            {
                g_saveAskedAt = 0;
                if (!Flag(SnapshotClipEdited))
                    EditMessage(TR("edit.message.saved", "Saved"));
            }
            else if (waited > 15000)
                g_saveAskedAt = 0;
        }
        // Ctrl+Z / Ctrl+Y (Ctrl+Shift+Z) undo and redo marker changes.
        if (ImGui::GetIO().KeyCtrl && (KeyPressed(ImGuiKey_Z, true) || KeyPressed(ImGuiKey_Y, true)))
        {
            const bool redo = KeyPressed(ImGuiKey_Y, true) || ImGui::GetIO().KeyShift;
            if (!Flag(redo ? SnapshotCanRedo : SnapshotCanUndo))
                PlayUi(UiSound::Error);
            else if (Send(redo ? CommandType::RedoMarkers : CommandType::UndoMarkers))
            {
                PlayUi(redo ? UiSound::NavRight : UiSound::NavLeft);
                ForgetPendingMarkerValues();
            }
        }
        // Render export: Ctrl+E opens the export dialog.
        if (ImGui::GetIO().KeyCtrl && KeyPressed(ImGuiKey_E, false))
        {
            PlayUi(UiSound::Select);
            g_screen = Screen::Export;
            exportui::g_back = Screen::Edit;
            if (exportui::g_range == 2)
                exportui::g_range = 1; // the clip's range from the clip editor
            return;
        }
        // P: a photo of the frame shown, saved by ReShade to its screenshot folder.
        if (!ImGui::GetIO().KeyCtrl && KeyPressed(ImGuiKey_P, false))
        {
            g_runtime->save_screenshot("RedemptionEditor");
            PlayUi(UiSound::Select);
            EditMessage(TR("edit.message.photo", "Photo saved (ReShade screenshot folder)"));
        }
        // T: the frame shown becomes the clip's thumbnail.
        if (!ImGui::GetIO().KeyCtrl && KeyPressed(ImGuiKey_T, false))
        {
            if (!g_snapshot.currentClip[0])
            {
                // The replay buffer played before it was saved has no file to hold a
                // thumbnail.
                PlayUi(UiSound::Error);
                static std::string saveFirst; // EditMessage keeps the pointer
                saveFirst = replay::keys::Fill(TR("edit.message.save_first", "Save the clip first ({key_save})"), Keys());
                EditMessage(saveFirst.c_str());
            }
            else if (clip_thumbnails::SetFromScreen(g_runtime, g_snapshot.currentClip))
            {
                PlayUi(UiSound::Select);
                EditMessage(TR("edit.message.thumbnail", "Thumbnail set"));
            }
            else
            {
                PlayUi(UiSound::Error);
                EditMessage(TR("edit.message.thumbnail_failed", "Could not take the thumbnail"));
            }
        }
        // Ctrl+Left / Ctrl+Right: the clip's start and end (Home opens the Social
        // Club overlay).
        const bool toStart = ImGui::GetIO().KeyCtrl && KeyPressed(ImGuiKey_LeftArrow, false);
        const bool toEnd = ImGui::GetIO().KeyCtrl && KeyPressed(ImGuiKey_RightArrow, false);
        if (toStart || toEnd)
        {
            Command seek{};
            seek.timeMs = toEnd ? g_snapshot.durationMs : 0;
            Send(CommandType::SeekAbsolute, &seek);
            PlayUi(toEnd ? UiSound::NavRight : UiSound::NavLeft);
        }
        // F7 stops the clip here too, as the playback bar and the help say (with the
        // editor open the key never reaches the core). The [Keys] table's keys
        // (editor_keys.h); Space stays fixed.
        if (ActionPressed(Action::Stop))
        {
            PlayUi(UiSound::Back);
            Send(CommandType::Stop);
        }
        if (KeyPressed(ImGuiKey_Space, false) || ActionPressed(Action::PlayPause)) // no repeat (a held Space would flicker play and pause)
            Send(CommandType::PlayPause);
        if (ActionPressed(Action::FreeCam))
            Send(CommandType::ToggleFreeCam);
        // Held , . keep stepping (the key repeat), as the arrows do.
        const bool stepBack = ActionPressed(Action::FrameBack, true);
        if (stepBack || ActionPressed(Action::FrameForward, true))
        {
            Command command{};
            command.direction = stepBack ? -1 : 1;
            Send(CommandType::StepFrame, &command);
        }
        // Rewind / FastForward (no key by default) seek half a second per repeat
        // while held (about ten times real time).
        const bool rewind = ActionPressed(Action::Rewind, true);
        if (rewind || ActionPressed(Action::FastForward, true))
            SeekBy(rewind ? -kRewindStepMs : kRewindStepMs);
        const bool slower = ActionPressed(Action::SpeedDown);
        if (slower || ActionPressed(Action::SpeedUp))
            StepPlaybackSpeed(slower ? -1 : 1); // from the speed last asked
        // Enter adds a marker at the playhead (with the settings of the marker
        // before it); with a marker's menu open the keys are its.
        if (!g_markerMenuActive && Confirmed())
            AddKeyframeHere();
        // Tab / Ctrl+Tab (or Shift+Tab): the next or previous marker (the playhead
        // goes along). Ctrl too, because Shift+Tab opens the Steam overlay on Steam
        // copies of the game.
        if (KeyPressed(ImGuiKey_Tab) && g_snapshot.cameraKeyframeCount > 0)
        {
            const bool backwards = ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl;
            // From the selected marker when there is one: the snapshot's playhead lags
            // the seek a frame or two, and a quick second Tab would pick the same marker
            // again.
            const CameraKeyframe* from = FindKeyframe(g_selectedKeyframeId);
            const std::uint64_t reference = from ? from->timeMs : g_snapshot.playheadMs;
            const CameraKeyframe* pick = nullptr;
            for (std::uint32_t n = 0; n < g_snapshot.cameraKeyframeCount; ++n)
            {
                const CameraKeyframe& key = g_snapshot.cameraKeyframes[backwards ? g_snapshot.cameraKeyframeCount - 1 - n : n];
                if (backwards ? key.timeMs + 1 < reference : key.timeMs > reference + 1)
                {
                    pick = &key;
                    break;
                }
            }
            if (!pick)
                pick = &g_snapshot.cameraKeyframes[backwards ? g_snapshot.cameraKeyframeCount - 1 : 0];
            g_selectedKeyframeId = pick->id;
            PlayUi(backwards ? UiSound::NavUp : UiSound::NavDown);
            Command seek{};
            seek.timeMs = pick->timeMs;
            Send(CommandType::SeekAbsolute, &seek);
        }
        if (g_markerMenuActive && ImGui::GetIO().KeyCtrl && KeyPressed(ImGuiKey_C))
        {
            Command c{};
            c.keyframeId = g_selectedKeyframeId;
            if (Send(CommandType::CopyCameraKeyframe, &c))
                PlayUi(UiSound::Select);
        }
        if (g_markerMenuActive && ImGui::GetIO().KeyCtrl && KeyPressed(ImGuiKey_V))
        {
            Command c{};
            c.keyframeId = g_selectedKeyframeId;
            ForgetPendingMarkerValues();
            if (!Flag(SnapshotMarkerCopied))
                PlayUi(UiSound::Error);
            else if (Send(CommandType::PasteCameraKeyframe, &c))
                PlayUi(UiSound::Select);
        }
        if (g_markerMenuActive && KeyPressed(ImGuiKey_Delete, false)) // a held Delete would go on deleting the next ones
            DeleteMarker(g_selectedKeyframeId);
        if (!g_markerMenuActive && !g_valueRowHovered && !ImGui::GetIO().KeyCtrl)
        {
            Command command{};
            if (KeyPressed(ImGuiKey_LeftArrow))
            {
                command.direction = -1;
                Send(CommandType::StepFrame, &command);
            }
            if (KeyPressed(ImGuiKey_RightArrow))
            {
                command.direction = 1;
                Send(CommandType::StepFrame, &command);
            }
        }
        // Esc goes to the home screen with the clip still open; F2 closes the
        // editor. An Esc the marker menu used (back a level, close the menu) stays
        // there.
        if (g_backConsumed)
            g_backConsumed = false;
        else if (Back())
        {
            PlayUi(UiSound::Back);
            RefreshClipList(false);
            // A project's clip goes back to the project.
            g_screen = ProjectFlag(ProjectEditingClip) ? Screen::Project : Screen::Home;
            g_homeIndex = kHomeEditRow;
        }
    }

    // ---------------- Key bar ----------------
    // The keys of the state on screen, always at its foot, right-aligned at the
    // bottom, wrapped onto more rows when they do not fit. A key that does
    // nothing right now (X with no clip open, F7 with nothing playing) shows
    // muted. The same lists feed the F1 help (its helpKey and help text), so the
    // bar and the help never disagree. The bar is drawn in the editor's window
    // like the rest of its UI, after the render export's capture point: it never
    // reaches an exported video.
    struct KeyTip
    {
        const char* key;     // the bar's key cap
        const char* label;   // the bar's word
        const char* helpKey; // the F1 help's row (nullptr: on the bar only)
        const char* help;
        bool on;
    };
    using KeyTips = std::vector<KeyTip>;

    // A list for the bar (the tips of the state, `when` true) or for the F1
    // help (every tip with a help row, whatever the state).
    struct TipList
    {
        KeyTips& out;
        bool help;
        void Add(bool when, const char* key, const char* label, const char* helpKey = nullptr, const char* helpText = nullptr, bool on = true)
        {
            if (help ? helpKey != nullptr : (when && key != nullptr))
                out.push_back({key, label, helpKey, helpText ? helpText : label, on});
        }
    };

    // Shift+F1 on the bar while marks wait ("Send report (3)"); on the F1 help
    // always, with marks on.
    void ReportTip(TipList& t)
    {
        static char label[160];
        const std::uint32_t pending = qa_shots::PendingMarks();
        replay::editor::uiqa::FormatReportKey(label, sizeof(label), pending);
        const char* key = KeyText(Action::Report);
        t.Add(pending > 0, key, label, qa_shots::MarksEnabled() ? key : nullptr,
            TR("help.save_the_qa_report", "Save the QA report (one zip, every mark and its clips)"));
    }

    // The clip editor (DrawEdit, DrawEditPanel, DrawEditTimeline). With a
    // marker's menu open the arrows and Enter are the menu's.
    void EditTips(TipList& t, bool menu)
    {
        const bool root = g_markerMenu == MarkerMenu::Root;
        // The remappable keys from the [Keys] table (KeyText, PairText).
        const char* frame = PairText(Action::FrameBack, Action::FrameForward);
        t.Add(true, fixedkey::Play, TR("keys.play_pause", "Play/Pause"), JoinText(fixedkey::Play, KeyText(Action::PlayPause)), TR("help.play_pause", "Play, pause"));
        // Left and Right change the side panel row under the mouse (ValueRow)
        // instead of stepping a frame.
        t.Add(!menu && !g_valueRowHovered, "Left Right", TR("keys.frame", "Frame"), JoinText("Left Right", frame), TR("help.one_frame", "One frame"));
        t.Add(!menu && g_valueRowHovered, "Left Right", TR("keys.change", "Change"), TR("keys.cap.left_right_on_a_row", "Left Right on a row"), TR("help.change_the_setting_under_the_mouse", "Change the setting under the mouse"));
        t.Add(menu, frame, TR("keys.frame", "Frame"));
        if (Keys().Bound(Action::Rewind) || Keys().Bound(Action::FastForward))
        {
            const char* rewind = PairText(Action::Rewind, Action::FastForward);
            t.Add(!menu, rewind, TR("keys.rewind_ff", "Rewind/FF"), rewind, TR("help.rewind_fast_forward_hold", "Rewind, fast forward (hold)"));
        }
        t.Add(menu, "Up Down", TR("keys.row", "Row"), "Up Down, Left Right", TR("help.marker_menu_row_value", "Marker menu: row, value"));
        t.Add(menu, "Left Right", TR("keys.value", "Value"));
        t.Add(menu, "PgUp PgDn", TR("keys.big_step", "Big step"), "PageUp PageDown", TR("help.value_big_steps", "Value: big steps"));
        t.Add(menu, fixedkey::Select, TR("keys.select", "Select"));
        t.Add(!menu, fixedkey::Select, TR("keys.add_marker", "Add marker"), fixedkey::Select, TR("help.add_a_marker_open_it", "Add a marker / open it"));
        t.Add(true, "Tab Ctrl+Tab", TR("keys.markers", "Markers"), "Tab / Ctrl+Tab", TR("help.next_previous_marker", "Next / previous marker"));
        t.Add(!menu, TR("keys.cap.click_mark", "Click mark"), TR("keys.select_drag", "Select, drag"), TR("keys.cap.click_a_marker", "Click a marker"), TR("help.select_it_drag_move", "Select it (drag: move)"));
        t.Add(menu, "Del", TR("keys.delete_marker", "Delete marker"), "Del", TR("help.delete_the_marker", "Delete the marker"));
        t.Add(menu, "Ctrl+C Ctrl+V", TR("keys.copy_paste", "Copy/Paste"), "Ctrl+C / Ctrl+V", TR("help.copy_paste_marker", "Copy / paste marker"));
        t.Add(!menu, "Ctrl+Left Right", TR("keys.start_end", "Start/End"), "Ctrl+Left / Ctrl+Right", TR("help.clip_start_end", "Clip start / end"));
        t.Add(!menu, TR("keys.cap.wheel_on_bar", "Wheel on bar"), TR("keys.frames", "Frames"), TR("keys.cap.wheel_on_the_timeline", "Wheel on the timeline"), TR("help.frames_shift_ten", "Frames (Shift: ten)"));
        t.Add(!menu, TR("keys.cap.ctrl_wheel", "Ctrl+Wheel"), TR("keys.zoom", "Zoom"), TR("keys.cap.ctrl_wheel_on_it", "Ctrl + wheel on it"), TR("help.zoom_the_timeline", "Zoom the timeline"));
        const char* speed = PairText(Action::SpeedDown, Action::SpeedUp);
        t.Add(!menu, speed, TR("keys.speed", "Speed"), speed, TR("help.playback_speed", "Playback speed"));
        t.Add(true, "Ctrl+Z Ctrl+Y", TR("keys.undo_redo", "Undo/Redo"), "Ctrl+Z / Ctrl+Y", TR("help.undo_redo", "Undo / redo"), Flag(SnapshotCanUndo) || Flag(SnapshotCanRedo));
        t.Add(true, fixedkey::Save, TR("keys.save", "Save"), fixedkey::Save, TR("help.save_the_clip", "Save the clip"));
        t.Add(!menu, fixedkey::Export, TR("keys.export", "Export"), fixedkey::Export, TR("help.export_video_render_export", "Export video (render export)"));
        t.Add(!menu, "T", TR("keys.thumbnail", "Thumbnail"), "T", TR("help.thumbnail_from_this_frame", "Thumbnail from this frame"), g_snapshot.currentClip[0] != 0);
        t.Add(!menu, "P", TR("keys.photo", "Photo"), "P", TR("help.photo_of_this_frame", "Photo of this frame"));
        const char* freeCam = KeyText(Action::FreeCam);
        const char* hide = KeyText(Action::HideUi);
        const char* stop = KeyText(Action::Stop);
        const char* mark = KeyText(Action::MarkProblem);
        const char* editor = KeyText(Action::Editor);
        const char* markHelp = KeyFill(TR("help.mark_problem", "Mark a problem (QA report: {key_report})"));
        t.Add(!menu, freeCam, Flag(SnapshotFreeCamEnabled) ? TR("keys.free_cam_off", "Free cam off") : TR("keys.free_cam", "Free cam"), freeCam, TR("help.free_camera", "Free camera"));
        t.Add(true, hide, Flag(SnapshotFreeCamEnabled) ? TR("keys.hide_ui_fly", "Hide UI, fly") : TR("keys.hide_ui", "Hide UI"), hide, TR("help.hide_the_editor_and_the_playback", "Hide the editor and the playback bar"));
        t.Add(!menu, stop, TR("keys.stop", "Stop"), stop, TR("help.stop_the_clip", "Stop the clip"));
        t.Add(!menu && qa_shots::MarksEnabled(), mark, TR("keys.mark_problem", "Mark problem"), mark, markHelp);
        if (!menu)
            ReportTip(t);
        t.Add(!menu, editor, TR("keys.close", "Close"), editor, TR("help.close_the_editor", "Close the editor"));
        t.Add(true, fixedkey::Help, TR("keys.all_keys", "All keys"));
        t.Add(menu, fixedkey::Back, root ? TR("keys.close_menu", "Close menu") : TR("keys.back", "Back"));
        t.Add(!menu, fixedkey::Back, ProjectFlag(ProjectEditingClip) ? TR("keys.project", "Project") : TR("keys.menu", "Menu"), fixedkey::Back, TR("help.back", "Back"));
    }

    // Edit Camera (DrawEditCameraOverlay; the core flies the camera): the
    // free camera's keys, or the orbit's round a Look At target.
    void EditCameraTips(TipList& t, bool orbit)
    {
        t.Add(!orbit, fixedkey::Fly, TR("keys.move", "Move"), "W A S D", TR("help.move", "Move"));
        t.Add(!orbit, "Q E", TR("keys.down_up", "Down Up"), "Q E", TR("help.down_up", "Down, up"));
        t.Add(!orbit, TR("keys.cap.mouse", "Mouse"), TR("keys.look", "Look"), TR("keys.cap.mouse", "Mouse"), TR("help.look", "Look"));
        t.Add(orbit, "A D", TR("keys.orbit", "Orbit"));
        t.Add(orbit, "W S", TR("keys.over_under", "Over Under"));
        t.Add(orbit, "Up Down", TR("keys.distance", "Distance"));
        t.Add(orbit, "Q E", TR("keys.height", "Height"));
        t.Add(orbit, TR("keys.cap.mouse", "Mouse"), TR("keys.framing", "Framing"));
        t.Add(true, "Z C", TR("keys.roll", "Roll"), "Z C", TR("help.roll", "Roll"));
        t.Add(true, "PgUp PgDn", TR("keys.zoom", "Zoom"), "PageUp PageDown", TR("help.zoom", "Zoom"));
        t.Add(!orbit, "Shift Ctrl", TR("keys.fast_slow", "Fast Slow"), "Shift / Ctrl", TR("help.fast_slow", "Fast / slow"));
        t.Add(!orbit, TR("keys.cap.wheel", "Wheel"), TR("keys.speed", "Speed"), TR("keys.cap.wheel", "Wheel"), TR("help.speed", "Speed"));
        t.Add(true, fixedkey::Select, TR("keys.keep", "Keep"), fixedkey::Select, TR("help.keep_the_view", "Keep the view"));
        t.Add(true, fixedkey::Cancel, TR("keys.cancel", "Cancel"), JoinText(fixedkey::Cancel, fixedkey::Back), TR("help.cancel", "Cancel"));
    }

    // A clip or the project playing with the editor closed (the playback bar):
    // the keys go to the game and the core (script.cpp, OnKeyboard: K , . [ ] F7
    // F11), F2, E and H to the editor. H hides the bar and this key bar (hidden,
    // no bar is drawn: the F1 help explains).
    void PlaybackTips(TipList& t)
    {
        const bool freeCam = Flag(SnapshotFreeCamEnabled);
        // The keys of the [Keys] table.
        const char* play = KeyText(Action::PlayPause);
        const char* frame = PairText(Action::FrameBack, Action::FrameForward);
        const char* speed = PairText(Action::SpeedDown, Action::SpeedUp);
        const char* freeCamKey = KeyText(Action::FreeCam);
        t.Add(true, play, TR("keys.play_pause", "Play/Pause"), play, TR("help.pause_play", "Pause, play"));
        t.Add(true, frame, TR("keys.frame", "Frame"), frame, TR("help.one_frame_hold_rewind_fast_forward", "One frame (hold: rewind, fast forward)"));
        if (Keys().Bound(Action::Rewind) || Keys().Bound(Action::FastForward))
        {
            const char* rewind = PairText(Action::Rewind, Action::FastForward);
            t.Add(true, rewind, TR("keys.rewind_ff", "Rewind/FF"), rewind, TR("help.rewind_fast_forward_hold", "Rewind, fast forward (hold)"));
        }
        t.Add(true, speed, TR("keys.speed", "Speed"), speed, TR("help.playback_speed", "Playback speed"));
        t.Add(true, freeCamKey, freeCam ? TR("keys.free_cam_off", "Free cam off") : TR("keys.free_cam", "Free cam"), freeCamKey, TR("help.free_camera", "Free camera"));
        t.Add(freeCam, fixedkey::Fly, TR("keys.move", "Move"));
        t.Add(freeCam, "Q E", TR("keys.down_up", "Down Up"));
        t.Add(freeCam, TR("keys.cap.mouse", "Mouse"), TR("keys.look", "Look"));
        t.Add(freeCam, TR("keys.cap.wheel", "Wheel"), TR("keys.cam_speed", "Cam speed"));
        // Drone, the target, follow, the world markers.
        {
            const char* drone = KeyText(Action::FreeCamDrone);
            const char* pick = KeyText(Action::FreeCamPick);
            const char* follow = KeyText(Action::FreeCamFollow);
            const char* overlay = KeyText(Action::CameraOverlay);
            t.Add(freeCam, drone, g_snapshot.freeCamMotion ? TR("keys.classic", "Classic") : TR("keys.drone", "Drone"), drone, TR("help.free_camera_drone_flight_on_off", "Free camera: Drone flight on/off"));
            t.Add(freeCam, pick, TR("keys.target", "Target"), pick, TR("help.free_camera_the_target_under_the", "Free camera: the target under the crosshair"));
            t.Add(false, nullptr, nullptr, KeyText(Action::FreeCamPickNearest), TR("help.free_camera_the_nearest_target", "Free camera: the nearest target"));
            t.Add(freeCam && g_snapshot.freeCamTarget != 0, follow, TR("keys.follow", "Follow"), follow, TR("help.free_camera_follow_the_target_rigid", "Free camera: follow the target (rigid, position only, off)"));
            t.Add(false, nullptr, nullptr, KeyText(Action::FreeCamAcrobatic), TR("help.free_camera_acrobatic_rotation_on_off", "Free camera: acrobatic rotation on/off"));
            t.Add(false, nullptr, nullptr, KeyText(Action::FreeCamWalk), TR("help.free_camera_walk_on_off", "Free camera: Walk on/off"));
            t.Add(false, nullptr, nullptr, KeyText(Action::FreeCamCollision), TR("help.free_camera_collision_on_off", "Free camera: collision on/off"));
            t.Add(false, nullptr, nullptr, KeyText(Action::FreeCamLockAltitude), TR("help.free_camera_keep_the_altitude", "Free camera: keep the altitude"));
            t.Add(false, nullptr, nullptr, KeyText(Action::FreeCamLevel), TR("help.free_camera_level_the_horizon", "Free camera: level the horizon"));
            t.Add(false, nullptr, nullptr, KeyText(Action::FreeCamBehind), TR("help.free_camera_behind_the_character", "Free camera: behind the character"));
            t.Add(false, nullptr, nullptr, KeyText(Action::FreeCamLock), TR("help.free_camera_hold_still", "Free camera: hold still"));
            t.Add(true, overlay, TR("keys.markers", "Markers"), overlay, TR("help.camera_markers_and_path_in_the", "Camera markers and path in the world on/off"));
        }
        const char* hide = KeyText(Action::HideUi);
        const char* stop = KeyText(Action::Stop);
        const char* mark = KeyText(Action::MarkProblem);
        t.Add(true, hide, TR("keys.hide_ui", "Hide UI"), hide, TR("help.hide_the_replay_ui_the_keys", "Hide the replay UI (the keys keep working)"));
        // E only without the free camera (it flies up there).
        t.Add(true, freeCam ? KeyText(Action::Editor) : KeyText(Action::Editor, fixedkey::Edit), TR("keys.edit", "Edit"), KeyText(Action::Editor, fixedkey::Edit),
            TR("help.open_the_editor", "Open the editor"));
        t.Add(true, stop, TR("keys.stop", "Stop"), stop, TR("help.play_stop_the_last_clip", "Play / stop the last clip"));
        t.Add(qa_shots::MarksEnabled(), mark, TR("keys.mark_problem", "Mark problem"), mark,
            KeyFill(TR("help.mark_problem", "Mark a problem (QA report: {key_report})")));
        ReportTip(t);
        // F4 and Shift+F4 work with the editor closed only.
        t.Add(false, nullptr, nullptr, KeyText(Action::Record), TR("help.record_mark_a_recording", "Record / mark a recording"));
        t.Add(false, nullptr, nullptr, KeyText(Action::SaveBuffer), TR("help.save_the_replay_buffer", "Save the replay buffer"));
    }

    // The project's timeline (DrawProject). The cut and the trims work on
    // the selected clip while it is open and playing: muted otherwise.
    // The Projects list (DrawProjects): on the bar, and on the F1 help over it.
    void ProjectsTips(TipList& t)
    {
        const bool file = g_projectsIndex > 0;
        const bool readable = file && g_projectsIndex - 1 < static_cast<int>(kMaxProjectsInSnapshot) &&
            g_snapshot.projects[g_projectsIndex - 1].readable;
        t.Add(true, "Up Down", TR("keys.select", "Select"), "Up Down", TR("help.select_a_project", "Select a project"));
        t.Add(true, fixedkey::Select, file ? TR("keys.open", "Open") : TR("keys.new_project", "New project"), fixedkey::Select,
            TR("help.open_it_or_make_a_new_one", "Open it (on New Project: make one)"), !file || readable);
        t.Add(file, "R", TR("keys.rename", "Rename"), "R", TR("help.rename_the_project", "Rename the project (its file too)"));
        t.Add(file, "Del", TR("keys.delete", "Delete"), "Del", TR("help.delete_the_project", "Delete the project"));
        t.Add(true, fixedkey::Help, TR("keys.all_keys", "All keys"));
        t.Add(true, fixedkey::Back, TR("keys.back", "Back"));
    }

    void ProjectTips(TipList& t)
    {
        const int count = static_cast<int>(std::min(g_snapshot.projectClipCount, kMaxProjectClipsInSnapshot));
        const bool playing = Flag(SnapshotPlaying);
        const bool any = ProjectIsOpen() && count > 0;
        const int active = g_snapshot.projectActiveClip < static_cast<std::uint32_t>(count) ? static_cast<int>(g_snapshot.projectActiveClip) : -1;
        const bool atPlayhead = active >= 0 && active == g_projectIndex && playing && !ProjectFlag(ProjectLoadingClip);
        const bool previewing = ProjectFlag(ProjectPreviewing) && playing;
        t.Add(any, fixedkey::Play, previewing && !Flag(SnapshotPaused) ? TR("keys.pause", "Pause") : TR("keys.play", "Play"), JoinText(fixedkey::Play, KeyText(Action::PlayPause)),
            TR("help.play_the_project_pause", "Play the project, pause"));
        t.Add(any, "Left Right", TR("keys.select", "Select"), "Left Right", TR("help.select_a_clip", "Select a clip"));
        t.Add(any, fixedkey::Select, TR("keys.edit_clip", "Edit clip"), KeyFill(TR("keys.cap.enter_double_click", "{key_select} / double click")), TR("help.edit_the_clip_trim_cameras", "Edit the clip (trim, cameras)"));
        t.Add(ProjectIsOpen(), fixedkey::AddClip, TR("keys.add_clip", "Add clip"), fixedkey::AddClip, TR("help.add_a_clip_after_it", "Add a clip after it"));
        t.Add(any, "Ctrl+Left Right", TR("keys.move", "Move"), "Ctrl+Left / Ctrl+Right", TR("help.move_it_back_forward", "Move it back / forward"), count > 1);
        t.Add(any && count > 1, TR("keys.cap.drag_clip", "Drag clip"), TR("keys.move", "Move"), TR("keys.cap.drag_a_clip", "Drag a clip"), TR("help.move_it_there", "Move it there"));
        t.Add(any, "X", TR("keys.cut", "Cut"), "X", TR("help.cut_it_at_the_playhead", "Cut it at the playhead"), atPlayhead);
        t.Add(any, "I O", TR("keys.trim", "Trim"), "I / O", TR("help.its_start_end_to_the_playhead", "Its start / end to the playhead"), atPlayhead);
        t.Add(any, "D", TR("keys.duplicate", "Duplicate"), "D", TR("help.duplicate_it", "Duplicate it"));
        t.Add(any, "Del", TR("keys.remove", "Remove"), "Del", TR("help.remove_it", "Remove it"));
        t.Add(any, TR("keys.cap.click_bar", "Click bar"), TR("keys.seek", "Seek"), TR("keys.cap.click_drag_the_bar", "Click / drag the bar"), TR("help.project_time", "Project time"));
        const char* frame = PairText(Action::FrameBack, Action::FrameForward);
        const char* stop = KeyText(Action::Stop);
        const char* editor = KeyText(Action::Editor);
        t.Add(any, frame, TR("keys.frame", "Frame"), frame, TR("help.one_frame", "One frame"), playing);
        t.Add(ProjectIsOpen(), fixedkey::Save, TR("keys.save", "Save"), fixedkey::Save, TR("help.save_the_project", "Save the project"));
        t.Add(ProjectIsOpen(), "R", TR("keys.rename", "Rename"), "R", TR("help.rename_the_project", "Rename the project (its file too)"));
        t.Add(any, fixedkey::Export, TR("keys.export", "Export"), fixedkey::Export, TR("help.export_the_whole_project", "Export the whole project as one video"));
        t.Add(any, stop, TR("keys.stop", "Stop"), stop, TR("help.stop", "Stop"), playing);
        t.Add(true, editor, TR("keys.close", "Close"), editor, TR("help.close_the_editor", "Close the editor"));
        t.Add(true, fixedkey::Help, TR("keys.all_keys", "All keys"));
        t.Add(true, fixedkey::Back, TR("keys.back", "Back"), fixedkey::Back, TR("help.back", "Back"));
    }

    // The export dialog (DrawExport): its keys, from its code. While an export
    // runs only Del (cancel) works; with the export off only Esc. The bar, like
    // the dialog, is drawn after the export's capture point
    // (ExportController::OnOverlay), so it never reaches the video.
    void ExportTips(TipList& t)
    {
        render_export::ExportController& controller = render_export::ExportController::Get();
        if (!controller.Enabled() || !controller.Status().channel)
        {
            t.Add(true, fixedkey::Back, TR("keys.back", "Back"), fixedkey::Back, TR("help.back", "Back"));
            return;
        }
        if (controller.Busy())
        {
            t.Add(true, "Del", TR("keys.cancel_export", "Cancel export"), "Del", TR("help.cancel_the_export_and_the_queue", "Cancel the export (and the queue)"));
            return;
        }
        const int index = exportui::g_index;
        const bool clip = Flag(SnapshotPlaying) || exportui::g_range == 2; // an export needs the clip open (a project opens its own)
        t.Add(true, "Up Down", TR("keys.select", "Select"), "Up Down", TR("help.select_a_setting", "Select a setting"));
        t.Add(index < 10, "Left Right", TR("keys.change", "Change"), "Left Right", TR("help.change_the_setting", "Change the setting"));
        t.Add(true, KeyFill(TR("keys.cap.enter_click", "{key_select} / Click")), index < 10 ? TR("keys.next_value", "Next value") : index == 10 ? TR("keys.export", "Export") : TR("keys.add_to_queue", "Add to queue"), KeyFill(TR("keys.cap.enter_click_help", "{key_select} / click")),
            TR("help.next_value_export", "Next value; on Export now / Add to queue: do it"), index < 10 || clip);
        t.Add(true, "A", TR("keys.add_to_queue", "Add to queue"), "A", TR("help.add_to_the_export_queue", "Add to the export queue"), clip);
        t.Add(true, fixedkey::Help, TR("keys.all_keys", "All keys"));
        if (exportui::g_back == Screen::Project)
            t.Add(true, fixedkey::Back, TR("keys.back", "Back"), fixedkey::Back, TR("help.back_to_the_project", "Back to the project"));
        else
            t.Add(true, fixedkey::Back, TR("keys.back", "Back"), fixedkey::Back, TR("help.back_to_the_clip_editor", "Back to the clip editor"));
    }

    // The bar's tips for what is on screen now.
    void ScreenTips(KeyTips& out)
    {
        TipList t{out, false};
        const bool playing = g_hasSnapshot && Flag(SnapshotPlaying);
        const bool debug = g_hasSnapshot && Flag(SnapshotDebugMode);
        if (qa_shots::NoteOpen()) // the note box
        {
            t.Add(true, fixedkey::Select, TR("keys.save_note", "Save note"));
            t.Add(true, fixedkey::Back, TR("keys.no_note", "No note"));
            return;
        }
        if (g_screen == Screen::Closed)
        {
            // Nothing over the game unless a clip plays (its playback bar).
            if (playing && !debug && !UiHidden())
                PlaybackTips(t);
            return;
        }
        if (!g_hasSnapshot)
        {
            t.Add(true, KeyText(Action::Editor), TR("keys.close", "Close"));
            return;
        }
        if (g_screen == Screen::Edit && Flag(SnapshotEditingCamera))
        {
            const CameraKeyframe* key = FindKeyframe(g_snapshot.editingMarker);
            EditCameraTips(t, key && key->lookAtTarget && key->cameraMode == CameraMode::Free);
            return;
        }
        // Hidden, no key bar (H brings it back).
        if (g_screen == Screen::Edit && UiHidden())
            return;
        if (g_helpOpen)
        {
            t.Add(true, JoinText(fixedkey::Help, fixedkey::Back), TR("keys.close_help", "Close help"));
            return;
        }
        switch (g_screen)
        {
        case Screen::Home:
            t.Add(true, "Up Down", TR("keys.select", "Select"));
            t.Add(true, fixedkey::Select, TR("keys.open", "Open"));
            ReportTip(t);
            t.Add(true, KeyText(Action::Editor), TR("keys.close", "Close"));
            t.Add(true, fixedkey::Help, TR("keys.all_keys", "All keys"));
            t.Add(true, fixedkey::Back, playing ? TR("keys.back_to_clip", "Back to clip") : TR("keys.back_to_game", "Back to game"));
            break;
        case Screen::Settings:
            t.Add(true, "Up Down", TR("keys.select", "Select"));
            t.Add(true, "Left Right", TR("keys.change", "Change"));
            t.Add(true, KeyFill(TR("keys.cap.enter_click", "{key_select} / Click")), TR("keys.next_value", "Next value"));
            t.Add(true, fixedkey::Help, TR("keys.all_keys", "All keys"));
            t.Add(true, fixedkey::Back, TR("keys.back", "Back"));
            break;
        case Screen::Gallery:
        {
            const ClipInfo* clip = GalleryClip(g_galleryIndex);
            const bool open = clip && playing && std::strcmp(clip->name, g_snapshot.currentClip) == 0;
            t.Add(g_snapshot.clipTotal > 1, "Up Down", TR("keys.select", "Select"));
            if (g_galleryPicking)
                t.Add(clip != nullptr, fixedkey::Select, TR("keys.add_to_project", "Add to project"), nullptr, nullptr, clip && clip->loadable);
            else
            {
                t.Add(clip != nullptr, fixedkey::Select, open ? TR("keys.edit", "Edit") : TR("keys.play", "Play"), nullptr, nullptr, clip && clip->loadable);
                t.Add(clip != nullptr, "R", TR("keys.rename", "Rename"));
                t.Add(clip != nullptr, "C", TR("keys.compress", "Compress"), nullptr, nullptr, clip && !clip->compressed && !open && clip->loadable);
                t.Add(clip != nullptr, "Del", TR("keys.delete", "Delete"));
            }
            t.Add(true, "Tab", TR("keys.sort", "Sort"));
            t.Add(true, fixedkey::Help, TR("keys.all_keys", "All keys"));
            t.Add(true, fixedkey::Back, g_galleryPicking ? TR("keys.project", "Project") : TR("keys.back", "Back"));
            break;
        }
        case Screen::Projects:
            ProjectsTips(t);
            break;
        case Screen::Project:
            ProjectTips(t);
            break;
        case Screen::Edit:
            EditTips(t, g_markerMenuActive);
            break;
        case Screen::Alert:
            if (g_alert.action != CommandType::None)
            {
                t.Add(true, fixedkey::Select, g_alert.confirmLabel.c_str());
                t.Add(true, fixedkey::Back, TR("keys.cancel", "Cancel"));
            }
            else
                t.Add(true, fixedkey::Select, TR("keys.ok", "OK"));
            break;
        case Screen::Rename:
        case Screen::ProjectName:
            t.Add(true, fixedkey::Select, TR("keys.save", "Save"));
            t.Add(true, fixedkey::Back, TR("keys.cancel", "Cancel"));
            break;
        case Screen::Export:
            ExportTips(t);
            break;
        default:
            break;
        }
    }

    constexpr float kKeyBarRowHeight = 28.0f;

    // Where each row of the bar starts in `tips`, filled in order, as many tips
    // a row as fit between the screen's margins (ui_layout_check.h PackRows).
    std::vector<std::size_t> KeyBarRows(const KeyTips& tips)
    {
        std::vector<float> widths(tips.size());
        for (std::size_t n = 0; n < tips.size(); ++n)
            widths[n] = KeyHintWidth(tips[n].key, tips[n].label);
        return replay::editor::uilayout::PackRows(widths, g_vw - 80.0f + 22.0f); // the last tip's gap hangs past the margin
    }

    float KeyBarHeight(std::size_t rows) { return rows ? rows * kKeyBarRowHeight + 14.0f : 0.0f; }

    void DrawKeyBar(ImDrawList* draw, const KeyTips& tips)
    {
        UI_PROBE_SCOPE("KeyBar");
        const std::vector<std::size_t> starts = KeyBarRows(tips);
        g_footH = KeyBarHeight(starts.size());
        if (starts.empty())
            return;
        // Screen units, whatever stage the screen above used.
        const float ox = g_ox, oy = g_oy;
        g_ox = g_oy = 0.0f;
        const float top = g_vh - g_footH;
        draw->AddRectFilled(P(0.0f, top), P(g_vw, g_vh), IM_COL32(8, 7, 6, 170));
        UI_PROBE_BOX("keybar", P(0.0f, top), P(g_vw, g_vh));
        for (std::size_t row = 0; row < starts.size(); ++row)
        {
            const std::size_t end = row + 1 < starts.size() ? starts[row + 1] : tips.size();
            float width = 0.0f;
            for (std::size_t n = starts[row]; n < end; ++n)
                width += KeyHintWidth(tips[n].key, tips[n].label);
            // Right-aligned (the gap after the last tip past the margin).
            float x = std::max(40.0f, g_vw - 40.0f - width + 22.0f);
            const float y = top + 8.0f + row * kKeyBarRowHeight;
            for (std::size_t n = starts[row]; n < end; ++n)
                x += KeyHint(draw, x, y, tips[n].key, tips[n].label, tips[n].on);
        }
        g_ox = ox;
        g_oy = oy;
    }

    // F1: every key of the editor on one panel, the screen shown first. Its rows
    // come from the key bar's lists (EditTips, ProjectTips, EditCameraTips,
    // PlaybackTips); the menus' keys are on their own bar.
    void DrawHelp(ImDrawList* draw)
    {
        UI_PROBE_SCOPE("Help");
        // Laid out from measured text: the rows flow down columns as wide as their
        // longest key and help, as many columns as the screen's height needs; a key
        // or a help too wide for its column wraps; the panel is centred and grows to
        // them. Above 100% Interface scale the text steps down (never below its
        // pixels at 100%) until the columns fit the screen's width.
        struct Section
        {
            const char* title;
            KeyTips keys;
        };
        std::vector<Section> sections;
        const auto add = [&sections](const char* title, const std::function<void(TipList&)>& fill) {
            Section section{title, {}};
            TipList list{section.keys, true};
            fill(list);
            sections.push_back(std::move(section));
        };
        if (g_screen == Screen::Projects)
            add(TR("help.section.projects", "PROJECTS"), [](TipList& t) { ProjectsTips(t); });
        if (g_screen == Screen::Project || g_screen == Screen::Projects)
            add(TR("help.section.project", "PROJECT"), [](TipList& t) { ProjectTips(t); });
        else
            add(TR("help.section.editing", "EDITING A CLIP"), [](TipList& t) { EditTips(t, false); });
        add(TR("help.section.edit_camera", "EDIT CAMERA"), [](TipList& t) { EditCameraTips(t, false); });
        add(TR("help.section.closed", "EDITOR CLOSED"), [](TipList& t) { PlaybackTips(t); });
        // The hide key and where the keys are changed.
        const std::string note = replay::keys::Fill(TR("help.note", "Every screen shows its keys at the foot of the screen. {key_hide} hides every bar while a "
            "clip plays; {key_hide} brings them back. The keys can be changed in RedemptionEditor.ini, section [Keys]."), Keys());

        // Screen units, whatever stage the screen under it used.
        const float ox = g_ox, oy = g_oy;
        g_ox = g_oy = 0.0f;
        const float top = 60.0f;
        const float headerH = 68.0f; // the brush and its title over the rows
        const float footerH = LabelHeight(14.0f) + 8.0f + kPanelPadBottom; // "F1 or Esc closes this help.", clear of the ink band
        const float availH = std::max(200.0f, g_vh - g_footH - 20.0f - top - headerH - footerH - kPanelPad);
        const float availW = g_vw - 80.0f;

        // The rows' text size: 15 units, and above 100% Interface scale (where the
        // screen holds fewer units) a step smaller while the rows do not fit the
        // screen's width, down to the pixels they have at 100% (15 units at 100%,
        // 12 at 125%). The gaps and the key column's cap go with it. At 100% and
        // below the help is laid out as it always was.
        const float sizeFloor = std::min(15.0f, 15.0f / std::max(1.0f, g_uiScaleFactor));
        float size = 15.0f;
        float keyW = 0.0f, columnHelpW = 0.0f, keyGap = 16.0f, columnGap = 40.0f, titleSize = 16.0f, titleH = 26.0f;

        // One entry per title, row and the note, with its height at a help width.
        struct Entry
        {
            int section;
            int row; // -1 the section's title, -2 the note, -3 the title of a section going on from the column before
            float height;
        };
        std::vector<Entry> entries;
        std::vector<std::vector<Entry>> columns;
        float columnW = 0.0f, panelW = 0.0f;
        for (;;)
        {
            const float k = size / 15.0f;
            keyGap = 16.0f * k;
            columnGap = 40.0f * k;
            titleSize = 16.0f * k;
            titleH = 26.0f * k;
            const float maxKeyW = 230.0f * k, minHelpW = 160.0f * k;
            keyW = 0.0f;
            float helpW = 0.0f;
            for (const Section& section : sections)
                for (const KeyTip& tip : section.keys)
                {
                    keyW = std::max(keyW, TextWidth(size, tip.helpKey) / g_scale);
                    helpW = std::max(helpW, TextWidth(size, tip.help) / g_scale);
                }
            keyW = std::min(keyW, maxKeyW);
            columnHelpW = helpW;
            for (int count = 1; count <= 4; ++count)
            {
                columnHelpW = std::max(minHelpW, std::min(helpW, (availW - 2.0f * kPanelPad - static_cast<float>(count - 1) * columnGap) / static_cast<float>(count) - keyW - keyGap));
                entries.clear();
                for (int s = 0; s < static_cast<int>(sections.size()); ++s)
                {
                    entries.push_back({s, -1, titleH});
                    for (int r = 0; r < static_cast<int>(sections[s].keys.size()); ++r)
                    {
                        const KeyTip& tip = sections[s].keys[r];
                        const float h = std::max(TitleWrapped(nullptr, 0.0f, 0.0f, size, 0, tip.helpKey, keyW),
                            TitleWrapped(nullptr, 0.0f, 0.0f, size, 0, tip.help, columnHelpW));
                        entries.push_back({s, r, std::max(22.0f * k, h + 4.0f * k)});
                    }
                    entries.back().height += 12.0f * k; // the gap after a section
                }
                entries.push_back({-1, -2, ParagraphHeight(size, note.c_str(), keyW + keyGap + columnHelpW)});
                // Two packings at this column count: first every section whole (one that
                // does not fit under the last starts the next column, when it fits a
                // column); if that needs more columns, sections split where the column
                // ends, the part in the next column under its title marked (CONT.).
                for (int whole = 1; whole >= 0; --whole)
                {
                    columns.assign(1, {});
                    float used = 0.0f;
                    for (std::size_t n = 0; n < entries.size(); ++n)
                    {
                        // A title stays with its first row (whole: with all its rows).
                        float need = entries[n].height + (entries[n].row == -1 && n + 1 < entries.size() ? entries[n + 1].height : 0.0f);
                        if (whole && entries[n].row == -1)
                        {
                            float sectionH = 0.0f;
                            for (std::size_t m = n; m < entries.size() && entries[m].section == entries[n].section; ++m)
                                sectionH += entries[m].height;
                            if (sectionH <= availH)
                                need = sectionH;
                        }
                        if (used > 0.0f && used + need > availH)
                        {
                            columns.emplace_back();
                            used = 0.0f;
                            // A section going on in the next column: its title again on top, marked.
                            if (entries[n].row >= 0)
                            {
                                columns.back().push_back({entries[n].section, -3, titleH});
                                used += titleH;
                            }
                        }
                        columns.back().push_back(entries[n]);
                        used += entries[n].height;
                    }
                    if (static_cast<int>(columns.size()) <= count)
                        break;
                }
                if (static_cast<int>(columns.size()) <= count)
                    break;
            }
            columnW = keyW + keyGap + columnHelpW;
            const float columnCount = static_cast<float>(columns.size());
            panelW = 2.0f * kPanelPad + columnCount * columnW + (columnCount - 1.0f) * columnGap;
            if (panelW <= availW + 0.5f || size <= sizeFloor)
                break;
            size = std::max(sizeFloor, size - 1.0f);
        }
        float contentH = 0.0f;
        for (const std::vector<Entry>& column : columns)
        {
            float h = 0.0f;
            for (const Entry& entry : column)
                h += entry.height;
            contentH = std::max(contentH, h);
        }
        const float panelH = headerH + contentH + footerH;
        const float x0 = std::max(40.0f, (g_vw - panelW) * 0.5f);
        InkPanel(draw, x0, top, x0 + panelW, top + panelH, IM_COL32(10, 9, 8, 235));
        const char* const title = TR("help.title", "EDITOR KEYS");
        Brush(draw, x0, top + 10.0f, std::max(260.0f, TextWidth(26.0f, title) / g_scale + 40.0f), 40.0f, kRed); // as wide as a long title
        Text(draw, x0 + 16.0f, top + 16.0f, 26.0f, kText, title);
        for (std::size_t c = 0; c < columns.size(); ++c)
        {
            const float x = x0 + kPanelPad + static_cast<float>(c) * (columnW + columnGap);
            float y = top + headerH;
            for (const Entry& entry : columns[c])
            {
                if (entry.row == -2)
                    Paragraph(draw, x, y, size, kMuted, note.c_str(), columnW);
                else if (entry.row == -1)
                    Text(draw, x, y, titleSize, kGold, sections[entry.section].title);
                else if (entry.row == -3)
                    Text(draw, x, y, titleSize, kGold, TRF("help.section.cont", "{title} (CONT.)", {{"title", sections[entry.section].title}}).c_str());
                else
                {
                    const KeyTip& tip = sections[entry.section].keys[entry.row];
                    TitleWrapped(draw, x, y, size, kText, tip.helpKey, keyW);
                    TitleWrapped(draw, x + keyW + keyGap, y, size, kMuted, tip.help, columnHelpW);
                }
                y += entry.height;
            }
        }
        Text(draw, x0 + kPanelPad, top + panelH - footerH + 8.0f, 14.0f, kMuted, KeyFill(TR("help.close", "{key_help} or {key_back} closes this help.")));
        g_ox = ox;
        g_oy = oy;
    }

    // ------------------------------------------------------------------
    // Frame
    // ------------------------------------------------------------------
    ImGuiConfigFlags g_previousConfigFlags = 0;
    bool g_configFlagsHeld = false; // EditorCoreStop gives ReShade its flags back

    // The cursor whenever the editor wants the mouse. Under ReShade 6.8 the
    // add-on's GetIO() is a converted copy (src/editor_cursor.h) and writes to
    // it never reach ReShade: no cursor unless ReShade's own window is open. So
    // the flag is read back each frame from a fresh GetIO() (the measure,
    // logged). With the painted ink arrow loaded (editor/ui/cursor_ink.png)
    // the editor draws it on the foreground list (over every window, after the
    // export's capture) in every ReShade, and writes io.MouseDrawCursor back
    // to false so ImGui's small arrow (ReShade 6.6 and 6.7, where the write
    // reaches) never doubles it; ReShade's own window keeps its cursor.
    // Without the art: ImGui's where the write reaches, the drawn arrow where
    // it is lost.
    namespace ecur = replay::editor::cursor;
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
    // ---------------- The camera markers in the world ----------------
    // The core projects them each tick (camera_overlay.h) into the overlay
    // channel; drawn here behind the editor's windows, after the export's
    // capture and the QA pictures (never in a video), gone with H.
    namespace oc = replay::overlaychannel;
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

    ImU32 WithAlpha(ImU32 color, float alpha)
    {
        const std::uint32_t a = static_cast<std::uint32_t>(std::clamp(((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha, 0.0f, 255.0f));
        return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
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
                    g_screen = Screen::Closed;
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
            if (g_screen == Screen::Closed)
            {
                // The project's timeline while it plays as a whole or is open with nothing
                // playing.
                g_screen = playing ? PlaybackScreen() : ProjectIsOpen() ? Screen::Project : Screen::Home;
                if (g_screen == Screen::Home)
                    RefreshClipList(false);
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
#ifndef REDEMPTION_UI_HARNESS
        CheckCrashNotice(); // once, a crash of an earlier session (the harness sets it)
        CheckStorageNotice(); // once, nowhere to save (the harness sets it)
#endif

        const ImGuiIO& io = ImGui::GetIO();
        UpdateLayoutScale(io.DisplaySize);
        UseStage(g_screen == Screen::Alert || g_screen == Screen::Rename || g_screen == Screen::ProjectName || noteOpen);
        if (g_screen == Screen::Gallery || g_screen == Screen::Projects)
            UseLeftStage();
        // Edit Camera: the game has the keyboard and mouse (the free camera), the
        // editor only shows the controls.
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
}

namespace
{
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

// The GPU events the loader forwards (gpu_events.h). Nothing consumes them:
// the export stays so any loader keeps the same protocol, and returns no
// table (GpuCall already treats a null table as no consumer).
extern "C" __declspec(dllexport) const replay::editor::gpu::EventTable* EditorCoreGpuEvents()
{
    return nullptr;
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
