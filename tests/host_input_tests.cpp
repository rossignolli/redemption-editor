// The host's input (host/input_rules.h, host/host_input.cpp): the editor's
// key edges per frame, what the game may see while the editor blocks it, the
// raw input filters, the VK -> ImGuiKey table; then host_input.cpp driven for
// real through a message-only window (window procedure chain, swallowing,
// pass-through when someone chained after us, GetKeyState/GetCursorPos
// detours). Nothing here moves the cursor or clips it.
#include "input_rules.h"
#include "host_input.h"

#include <cstdio>
#include <cstring>

namespace
{
    namespace r = host::input::rules;

    int g_failures = 0;
    int g_checks = 0;

    void Check(bool condition, const char* message)
    {
        ++g_checks;
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    r::Event Down(unsigned vk, std::uint32_t t = 0) { return r::Event{r::Kind::KeyDown, static_cast<std::uint16_t>(vk), 0, t}; }
    r::Event Up(unsigned vk, std::uint32_t t = 0) { return r::Event{r::Kind::KeyUp, static_cast<std::uint16_t>(vk), 0, t}; }

    void TestMessages()
    {
        r::Event e[2];
        // Right Shift: scan code 0x36 in lParam bits 16..23.
        Check(r::FromMessage(r::kWmKeyDown, r::kVkShift, 0x36LL << 16, 5, e) == 1 && e[0].kind == r::Kind::KeyDown &&
                  e[0].code == r::kVkRShift && e[0].timeMs == 5,
              "messages: right Shift by its scan code");
        Check(r::FromMessage(r::kWmKeyDown, r::kVkShift, 0x2ALL << 16, 0, e) == 1 && e[0].code == r::kVkLShift,
              "messages: left Shift");
        Check(r::FromMessage(r::kWmKeyUp, r::kVkControl, (1LL << 24) | (0x1DLL << 16), 0, e) == 1 && e[0].kind == r::Kind::KeyUp &&
                  e[0].code == r::kVkRControl,
              "messages: right Ctrl by the extended bit");
        Check(r::FromMessage(r::kWmSysKeyDown, r::kVkMenu, 0x38LL << 16, 0, e) == 1 && e[0].code == r::kVkLMenu,
              "messages: left Alt comes as WM_SYSKEYDOWN");
        Check(r::FromMessage(r::kWmXButtonDown, 2ULL << 16, 0, 0, e) == 1 && e[0].code == r::kVkXButton2,
              "messages: X2 from HIWORD(wParam)");
        Check(r::FromMessage(r::kWmLButtonDblClk, 0, 0, 0, e) == 1 && e[0].kind == r::Kind::KeyDown && e[0].code == r::kVkLButton,
              "messages: a double click is a down");
        Check(r::FromMessage(r::kWmMouseWheel, static_cast<std::uint64_t>(static_cast<std::uint16_t>(-240)) << 16, 0, 0, e) == 1 &&
                  e[0].kind == r::Kind::Wheel && e[0].value == -240,
              "messages: wheel delta is signed");
        Check(r::FromMessage(r::kWmChar, 0x00E9, 0, 0, e) == 1 && e[0].kind == r::Kind::Char && e[0].code == 0x00E9,
              "messages: WM_CHAR carries the UTF-16 unit");
        Check(r::FromMessage(r::kWmChar, 0, 0, 0, e) == 0, "messages: a NUL char is dropped");
        Check(r::FromMessage(r::kWmKillFocus, 0, 0, 0, e) == 1 && e[0].kind == r::Kind::Focus && e[0].value == 0,
              "messages: focus lost");
        Check(r::FromMessage(r::kWmMouseMove, 0, 0, 0, e) == 0, "messages: a move is no event (the position is polled)");

        Check(r::FromRawKeyboard(r::kVkMenu, 0x38, r::kRiKeyE0, 0, e) == 1 && e[0].code == r::kVkRMenu && e[0].kind == r::Kind::KeyDown,
              "raw: right Alt (E0)");
        Check(r::FromRawKeyboard('A', 0x1E, r::kRiKeyBreak, 0, e) == 1 && e[0].kind == r::Kind::KeyUp, "raw: break is an up");
        Check(r::FromRawKeyboard(0xFF, 0, 0, 0, e) == 0, "raw: VKey 0xFF (a prefix) is no key");

        r::Event m[12];
        const std::size_t n = r::FromRawMouse(r::kRiMouseLeftDown | r::kRiMouseRightUp | r::kRiMouseWheel, static_cast<std::uint16_t>(120), 0, m);
        Check(n == 3 && m[0].code == r::kVkLButton && m[0].kind == r::Kind::KeyDown && m[1].code == r::kVkRButton &&
                  m[1].kind == r::Kind::KeyUp && m[2].kind == r::Kind::Wheel && m[2].value == 120,
              "raw: mouse buttons and wheel in one packet");
    }

    void TestKeys()
    {
        // A normal press over several frames.
        {
            r::Keys k;
            k.BeginFrame();
            k.Apply(Down('A'));
            Check(k.KeyPressed('A') && k.KeyDown('A') && !k.KeyReleased('A'), "keys: down this frame");
            k.BeginFrame();
            k.Apply(Down('A')); // auto-repeat
            Check(!k.KeyPressed('A') && k.KeyDown('A') && k.KeyRepeated('A'), "keys: a repeat is not a press");
            k.BeginFrame();
            Check(!k.KeyPressed('A') && k.KeyDown('A'), "keys: held");
            k.Apply(Up('A'));
            Check(k.KeyReleased('A') && !k.KeyDown('A'), "keys: up this frame");
            k.BeginFrame();
            Check(!k.KeyReleased('A') && !k.KeyPressed('A'), "keys: edges last one frame");
        }
        // A tap inside one long frame keeps both edges.
        {
            r::Keys k;
            k.BeginFrame();
            k.Apply(Down(0x0D));
            k.Apply(Up(0x0D));
            Check(k.KeyPressed(0x0D) && k.KeyReleased(0x0D) && !k.KeyDown(0x0D), "keys: a tap in one frame is pressed and released");
        }
        // Up then down again in one frame: a new press.
        {
            r::Keys k;
            k.BeginFrame();
            k.Apply(Down('S'));
            k.BeginFrame();
            k.Apply(Up('S'));
            k.Apply(Down('S'));
            Check(k.KeyPressed('S') && k.KeyReleased('S') && k.KeyDown('S'), "keys: released and pressed again in one frame");
        }
        // An up with no down is nothing.
        {
            r::Keys k;
            k.BeginFrame();
            k.Apply(Up('Q'));
            Check(!k.KeyReleased('Q'), "keys: a lone up is ignored");
        }
        // Generic modifiers follow both sides.
        {
            r::Keys k;
            k.BeginFrame();
            k.Apply(Down(r::kVkLShift));
            Check(k.KeyPressed(r::kVkShift) && k.KeyDown(r::kVkShift), "mods: left Shift presses Shift");
            k.BeginFrame();
            k.Apply(Down(r::kVkRShift));
            Check(!k.KeyPressed(r::kVkShift), "mods: the second side is no second Shift press");
            k.Apply(Up(r::kVkLShift));
            Check(k.KeyDown(r::kVkShift) && !k.KeyReleased(r::kVkShift), "mods: Shift held while one side is");
            k.BeginFrame();
            k.Apply(Up(r::kVkRShift));
            Check(!k.KeyDown(r::kVkShift) && k.KeyReleased(r::kVkShift), "mods: Shift up with both sides");
        }
        // Focus lost: everything up.
        {
            r::Keys k;
            k.BeginFrame();
            k.Apply(Down('W'));
            k.Apply(Down(r::kVkLButton));
            k.BeginFrame();
            k.Apply(r::Event{r::Kind::Focus, 0, 0, 0});
            Check(!k.focused && k.focusChanged, "focus: lost is reported once");
            Check(!k.KeyDown('W') && k.KeyReleased('W') && !k.MouseDown(0) && k.MouseReleased(0), "focus: keys and buttons released");
            k.BeginFrame();
            k.Apply(r::Event{r::Kind::Focus, 0, 0, 0});
            Check(!k.focusChanged, "focus: the same state twice is no change");
        }
        // Text and wheel are per frame.
        {
            r::Keys k;
            k.BeginFrame();
            k.Apply(r::Event{r::Kind::Char, 'h', 0, 0});
            k.Apply(r::Event{r::Kind::Char, 'i', 0, 0});
            k.Apply(r::Event{r::Kind::Wheel, 0, 120, 0});
            k.Apply(r::Event{r::Kind::Wheel, 0, 240, 0});
            k.Apply(r::Event{r::Kind::WheelH, 0, -120, 0});
            Check(k.charCount == 2 && k.chars[0] == 'h' && k.chars[1] == 'i', "text: chars in order");
            Check(k.wheel == 360 && k.wheelH == -120, "wheel: accumulated in the frame");
            k.BeginFrame();
            Check(k.charCount == 0 && k.wheel == 0 && k.wheelH == 0, "text/wheel: cleared next frame");
            for (int i = 0; i < 100; ++i)
                k.Apply(r::Event{r::Kind::Char, 'x', 0, 0});
            Check(k.charCount == r::Keys::kMaxChars, "text: capped");
        }
        // Stale keys: a sided modifier whose up was lost goes up at once; any
        // other key after 5 s; mouse buttons never.
        {
            r::Keys k;
            k.BeginFrame();
            k.Apply(Down(r::kVkLControl, 1000));
            k.Apply(Down('D', 1000));
            k.Apply(Down(r::kVkLButton, 1000));
            k.BeginFrame();
            k.ReleaseStale(1100, [](unsigned) { return false; });
            Check(!k.KeyDown(r::kVkLControl) && !k.KeyDown(r::kVkControl) && k.KeyReleased(r::kVkControl),
                  "stale: Ctrl physically up is released at once");
            Check(k.KeyDown('D'), "stale: a letter is kept under 5 s");
            k.BeginFrame();
            k.ReleaseStale(1000 + r::Keys::kStaleMs + 1, [](unsigned) { return false; });
            Check(!k.KeyDown('D') && k.KeyReleased('D'), "stale: a letter physically up after 5 s is released");
            Check(k.MouseDown(0), "stale: mouse buttons are never released by the physical state");
            r::Keys held;
            held.BeginFrame();
            held.Apply(Down('D', 0));
            held.BeginFrame();
            held.ReleaseStale(100000, [](unsigned) { return true; });
            Check(held.KeyDown('D'), "stale: a key still physically down stays");
        }
        Check(r::MouseButtonVk(0) == r::kVkLButton && r::MouseButtonVk(2) == r::kVkMButton && r::MouseButtonVk(4) == r::kVkXButton2 &&
                  r::MouseButtonVk(5) == 0,
              "buttons: ReShade's index to VK");
    }

    void TestBlock()
    {
        r::Block b;
        b.Request(true, false);
        Check(b.mouse && !b.keyboard, "block: on at once, mouse only");
        b.BeginFrame();
        Check(b.mouse, "block: kept for the frame after the ask");
        b.Request(true, true);
        b.BeginFrame();
        Check(b.mouse && b.keyboard, "block: asked every frame, stays");
        b.BeginFrame();
        Check(!b.mouse && !b.keyboard, "block: off after a frame that did not ask");
    }

    void TestRoute()
    {
        using r::Route;
        r::Block blocked;
        blocked.mouse = blocked.keyboard = true;
        const r::Block open;

        // Held before the block: its up reaches the game during the block.
        {
            r::Gate g;
            Check(r::RouteMessage(r::kWmKeyDown, 'W', open, g) == Route::Pass, "route: a down passes when open");
            Check(r::RouteMessage(r::kWmKeyDown, 'W', blocked, g) == Route::Swallow, "route: its repeat is hidden while blocked");
            Check(r::RouteMessage(r::kWmKeyUp, 'W', blocked, g) == Route::Pass, "route: the up of a key the game saw passes while blocked");
            Check(r::RouteMessage(r::kWmKeyUp, 'W', blocked, g) == Route::Swallow, "route: a second up does not");
        }
        // Pressed during the block: down and up hidden, even if the block
        // ends in between.
        {
            r::Gate g;
            Check(r::RouteMessage(r::kWmKeyDown, 'E', blocked, g) == Route::Swallow, "route: a down is hidden while blocked");
            Check(r::RouteMessage(r::kWmKeyUp, 'E', blocked, g) == Route::Swallow, "route: its up too");
            Check(r::RouteMessage(r::kWmKeyUp, 'E', open, g) == Route::Pass, "route: an up with nothing blocked passes (harmless)");
        }
        {
            r::Gate g;
            Check(r::RouteMessage(r::kWmSysKeyDown, r::kVkF4, blocked, g) == Route::Pass, "route: Alt+F4 always passes");
            Check(r::RouteMessage(r::kWmSysKeyDown, r::kVkMenu, blocked, g) == Route::Swallow, "route: Alt alone is hidden (no menu)");
            Check(r::RouteMessage(r::kWmChar, 'a', blocked, g) == Route::Swallow, "route: text is hidden while the keyboard is");
            Check(r::RouteMessage(r::kWmMouseMove, 0, blocked, g) == Route::Swallow, "route: mouse moves are hidden");
            Check(r::RouteMessage(r::kWmMouseWheel, 0, blocked, g) == Route::Swallow, "route: the wheel is hidden");
            Check(r::RouteMessage(r::kWmInput, 0, blocked, g) == Route::Pass, "route: WM_INPUT passes (filtered at GetRawInputData)");
            Check(r::RouteMessage(0x000F /* WM_PAINT */, 0, blocked, g) == Route::Pass, "route: other messages pass");
        }
        // Mouse and keyboard are blocked independently.
        {
            r::Gate g;
            r::Block mouseOnly;
            mouseOnly.mouse = true;
            Check(r::RouteMessage(r::kWmKeyDown, 'R', mouseOnly, g) == Route::Pass, "route: keys pass while only the mouse is blocked");
            Check(r::RouteMessage(r::kWmLButtonDown, 0, mouseOnly, g) == Route::Swallow, "route: clicks do not");
            Check(r::RouteMessage(r::kWmXButtonDown, 1ULL << 16, open, g) == Route::Pass, "route: X1 down seen");
            Check(r::RouteMessage(r::kWmXButtonUp, 1ULL << 16, mouseOnly, g) == Route::Pass, "route: X1 up of a seen down passes");
        }
    }

    void TestRawFilters()
    {
        r::Block blocked;
        blocked.mouse = blocked.keyboard = true;
        const r::Block open;
        {
            r::Gate g;
            std::uint16_t vk = 'F';
            std::uint16_t flags = 0;
            Check(!r::FilterRawKeyboard(vk, flags, blocked, g) && vk == 0 && (flags & r::kRiKeyBreak),
                  "raw keys: a blocked make becomes an up of key 0");
            vk = 'G';
            flags = 0;
            Check(r::FilterRawKeyboard(vk, flags, open, g) && vk == 'G', "raw keys: open, untouched");
            flags = r::kRiKeyBreak;
            Check(r::FilterRawKeyboard(vk, flags, blocked, g) && vk == 'G', "raw keys: the break of a key the game saw passes");
        }
        {
            r::Gate g;
            r::RawMouse down;
            down.buttonFlags = r::kRiMouseRightDown;
            r::FilterRawMouse(down, open, g); // aim held before the editor opens

            r::RawMouse m;
            m.flags = r::kMouseMoveAbsolute;
            m.lastX = 40;
            m.lastY = -7;
            m.rawButtons = 3;
            m.buttonData = 120;
            m.buttonFlags = r::kRiMouseLeftDown | r::kRiMouseRightUp | r::kRiMouseWheel;
            const bool left = r::FilterRawMouse(m, blocked, g);
            Check(left && m.buttonFlags == r::kRiMouseRightUp, "raw mouse: only the up of a button the game saw is left");
            Check(m.lastX == 0 && m.lastY == 0 && m.buttonData == 0 && m.rawButtons == 0, "raw mouse: no motion, no wheel");
            Check((m.flags & r::kMouseMoveAbsolute) == 0, "raw mouse: not an absolute jump to 0,0");

            r::RawMouse motion;
            motion.lastX = 5;
            Check(!r::FilterRawMouse(motion, blocked, g), "raw mouse: pure motion leaves nothing (input sink)");

            r::RawMouse free;
            free.lastX = 5;
            free.buttonFlags = r::kRiMouseWheel;
            Check(r::FilterRawMouse(free, open, g) && free.lastX == 5 && free.buttonFlags == r::kRiMouseWheel, "raw mouse: open, untouched");
        }
        r::Block kb;
        kb.keyboard = true;
        Check(r::HideKeyState('A', kb) && r::HideKeyState(0xA0, kb) && !r::HideKeyState(1, kb), "key state: keyboard keys hidden, buttons not");
        r::Block mouse;
        mouse.mouse = true;
        Check(r::HideKeyState(1, mouse) && r::HideKeyState(6, mouse) && !r::HideKeyState('A', mouse) && !r::HideKeyState(0, mouse),
              "key state: buttons 1..6 hidden with the mouse");
    }

    void TestImGui()
    {
        Check(r::ImGuiKeyOf(0x7A) == ImGuiKey_F11 && r::ImGuiKeyOf(0x7B) == ImGuiKey_F12, "imgui: F11/F12 (ReShade had them wrong)");
        Check(r::ImGuiKeyOf('A') == ImGuiKey_A && r::ImGuiKeyOf(0x0D) == ImGuiKey_Enter && r::ImGuiKeyOf(r::kVkRShift) == ImGuiKey_RightShift,
              "imgui: letters, Enter, sided Shift");
        Check(r::ImGuiKeyOf(0x07) == ImGuiKey_None, "imgui: an unmapped VK");
        bool unique = true;
        for (std::size_t i = 0; i < sizeof(r::kKeyMap) / sizeof(r::kKeyMap[0]); ++i)
            for (std::size_t j = i + 1; j < sizeof(r::kKeyMap) / sizeof(r::kKeyMap[0]); ++j)
                unique = unique && r::kKeyMap[i].vk != r::kKeyMap[j].vk && r::kKeyMap[i].key != r::kKeyMap[j].key;
        Check(unique, "imgui: each VK and each ImGuiKey once in the table");

        r::Keys k;
        k.BeginFrame();
        k.Apply(Down(r::kVkRControl));
        k.Apply(Down(r::kVkLWin));
        const r::Mods mods = r::ModsOf(k);
        Check(mods.ctrl && !mods.shift && !mods.alt && mods.super, "imgui: modifiers from the generic levels");

        float x = 0.0f;
        float y = 0.0f;
        r::ScaleMouse(960.0f, 540.0f, 1920.0f, 1080.0f, 3840.0f, 2160.0f, x, y);
        Check(x == 1920.0f && y == 1080.0f, "imgui: client to back buffer scale");
        r::ScaleMouse(10.0f, 20.0f, 0.0f, 0.0f, 3840.0f, 2160.0f, x, y);
        Check(x == 10.0f && y == 20.0f, "imgui: no client size, unscaled");
        Check(r::WheelNotches(240) == 2.0f && r::WheelHNotches(120) == -1.0f, "imgui: wheel notches, horizontal sign flipped");
    }

    // ---- host_input.cpp through a real window ---------------------------------

    int g_gameSaw[0x210]{};
    WNDPROC g_hostProc = nullptr;
    int g_laterSaw = 0;

    LRESULT CALLBACK GameProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg < 0x210)
            ++g_gameSaw[msg];
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    // Someone who subclasses after the host (an overlay, a second mod).
    LRESULT CALLBACK LaterProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        ++g_laterSaw;
        return CallWindowProcW(g_hostProc, hwnd, msg, wParam, lParam);
    }

    void ResetSaw() { std::memset(g_gameSaw, 0, sizeof(g_gameSaw)); }

    void TestLive()
    {
        namespace hi = host::input;
        static int logLines = 0;
        hi::SetLogSink([](const char*) { ++logLines; });

        WNDCLASSW wc{};
        wc.lpfnWndProc = &GameProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"HostInputTestWindow";
        RegisterClassW(&wc);
        const HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        Check(hwnd != nullptr, "live: test window");
        if (hwnd == nullptr)
            return;

        Check(!hi::Install(nullptr), "live: no window, no install");
        Check(hi::Install(hwnd), "live: install");
        Check(logLines > 0, "live: install logs");
        Check(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC)) != &GameProc, "live: our procedure heads the chain");

        // Open: the game and the editor both see a key.
        ResetSaw();
        SendMessageW(hwnd, WM_KEYDOWN, 'A', 0x1E << 16);
        hi::BeginFrame();
        Check(g_gameSaw[WM_KEYDOWN] == 1, "live: open, the game sees the down");
        Check(hi::KeyPressed('A') && hi::KeyDown('A'), "live: the editor sees the press");

        // Blocked: the editor still sees everything, the game nothing new.
        hi::BlockNextFrame(true, true);
        ResetSaw();
        SendMessageW(hwnd, WM_KEYDOWN, 'B', 0x30 << 16);
        SendMessageW(hwnd, WM_CHAR, 'b', 0);
        SendMessageW(hwnd, WM_LBUTTONDOWN, 0, 0);
        SendMessageW(hwnd, WM_MOUSEWHEEL, static_cast<WPARAM>(static_cast<std::uint16_t>(240)) << 16, 0);
        SendMessageW(hwnd, WM_KEYUP, 'A', (0x1E << 16) | (3u << 30));
        Check(g_gameSaw[WM_KEYDOWN] == 0 && g_gameSaw[WM_CHAR] == 0 && g_gameSaw[WM_LBUTTONDOWN] == 0 && g_gameSaw[WM_MOUSEWHEEL] == 0,
              "live: blocked, the game sees no key, text, click or wheel");
        Check(g_gameSaw[WM_KEYUP] == 1, "live: blocked, the up of a key the game held still reaches it");
        Check(GetKeyState(VK_NUMLOCK) == 0 && GetAsyncKeyState(VK_SHIFT) == 0, "live: blocked, GetKeyState/GetAsyncKeyState say nothing");
        hi::BeginFrame();
        Check(hi::KeyPressed('B') && hi::KeyReleased('A') && hi::MouseDown(0), "live: the editor sees keys and buttons while blocked");
        unsigned count = 0;
        const wchar_t* text = hi::TextInput(count);
        Check(count == 1 && text[0] == L'b', "live: typed text");
        Check(hi::MouseWheel() == 2.0f, "live: wheel notches");

        // GetCursorPos while blocked answers the game's last position.
        hi::BeginFrame();
        hi::BeginFrame(); // a frame without the ask: open again
        POINT gameView{};
        GetCursorPos(&gameView); // recorded as the game's position
        hi::BlockNextFrame(true, false);
        POINT blockedView{-1, -1};
        GetCursorPos(&blockedView);
        Check(blockedView.x == gameView.x && blockedView.y == gameView.y, "live: blocked, GetCursorPos answers the game's last position");

        // A tap inside one frame.
        hi::BeginFrame();
        hi::BeginFrame();
        ResetSaw();
        SendMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0x1C << 16);
        SendMessageW(hwnd, WM_KEYUP, VK_RETURN, (0x1C << 16) | (3u << 30));
        SendMessageW(hwnd, WM_LBUTTONUP, 0, 0);
        hi::BeginFrame();
        Check(hi::KeyPressed(VK_RETURN) && hi::KeyReleased(VK_RETURN) && !hi::KeyDown(VK_RETURN), "live: a tap in one frame keeps both edges");
        Check(hi::MouseReleased(0) && !hi::MouseDown(0), "live: button released");
        Check(g_gameSaw[WM_KEYDOWN] == 1 && g_gameSaw[WM_LBUTTONUP] == 1, "live: open again, the game sees keys (and a stray up, harmless)");

        // Someone chains after us: Uninstall leaves a pass-through, the chain
        // keeps working; Install switches it back on.
        g_hostProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&LaterProc));
        hi::Uninstall();
        Check(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC)) == &LaterProc, "live: not the head, nothing restored");
        ResetSaw();
        g_laterSaw = 0;
        SendMessageW(hwnd, WM_KEYDOWN, 'C', 0x2E << 16);
        hi::BeginFrame();
        Check(g_laterSaw == 1 && g_gameSaw[WM_KEYDOWN] == 1, "live: pass-through, the message goes on down the chain");
        Check(!hi::KeyPressed('C'), "live: pass-through collects nothing");
        Check(hi::Install(hwnd), "live: install again");
        Check(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC)) == &LaterProc, "live: re-install does not chain twice");
        hi::BlockNextFrame(false, true);
        ResetSaw();
        SendMessageW(hwnd, WM_KEYDOWN, 'D', 0x20 << 16);
        Check(g_gameSaw[WM_KEYDOWN] == 0, "live: re-installed, blocking works through the later procedure");
        hi::BeginFrame();
        Check(hi::KeyPressed('D'), "live: re-installed, the editor sees keys");

        // The later procedure leaves; we are the head again and restore.
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_hostProc));
        hi::Uninstall();
        Check(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC)) == &GameProc, "live: the head restores the game's procedure");

        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        hi::SetLogSink(nullptr);
    }
}

int RunHostInputTests()
{
    TestMessages();
    TestKeys();
    TestBlock();
    TestRoute();
    TestRawFilters();
    TestImGui();
    TestLive();
    std::printf("  host input: %d checks, %d failed\n", g_checks, g_failures);
    if (g_failures == 0)
        std::puts("Host input tests passed.");
    return g_failures;
}
