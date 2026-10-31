#pragma once

// The host's input rules with no Win32 call, kept pure so
// tests/host_input_tests.cpp can pin them. host/host_input.cpp feeds them the
// window messages and raw input packets it sees and asks them what the game
// may see.
//
// Origin: the key state bits, the "a key up the game saw go down always
// passes" rule, the stale key release and the VK -> ImGuiKey table follow
// ReShade's source/input.cpp and source/runtime_gui.cpp
// (Copyright (C) 2014 Patrick Mours, SPDX-License-Identifier: BSD-3-Clause),
// rewritten as a frame state machine fed by an event queue.
//
// Three things live here:
//  - Keys: the editor's view of the keyboard and mouse buttons for one frame.
//    Events queued by the window thread are applied at BeginFrame; a key that
//    went down and up inside one frame still reports Pressed and Released
//    (a tap in a long frame is kept), the level (Down) is the one at the end.
//  - Block + Gate: what the game is allowed to see. Block holds the
//    BlockNextFrame request (ReShade's block_input_next_frame semantics: on
//    from the call until the start of the first frame that did not ask
//    again). Gate remembers which key downs the game saw, so the matching up
//    always reaches it (no key stuck in the game when the editor opens with a
//    key held) and an up whose down was hidden stays hidden.
//  - The VK -> ImGuiKey table and the small conversions FeedImGui needs.

#include <imgui.h>

#include <cstddef>
#include <cstdint>

namespace host::input::rules
{
    // ---- Win32 values (no windows.h here) ----------------------------------
    constexpr unsigned kVkLButton = 0x01;
    constexpr unsigned kVkRButton = 0x02;
    constexpr unsigned kVkMButton = 0x04;
    constexpr unsigned kVkXButton1 = 0x05;
    constexpr unsigned kVkXButton2 = 0x06;
    constexpr unsigned kVkShift = 0x10;
    constexpr unsigned kVkControl = 0x11;
    constexpr unsigned kVkMenu = 0x12;
    constexpr unsigned kVkCapital = 0x14;
    constexpr unsigned kVkF4 = 0x73;
    constexpr unsigned kVkLWin = 0x5B;
    constexpr unsigned kVkRWin = 0x5C;
    constexpr unsigned kVkLShift = 0xA0;
    constexpr unsigned kVkRShift = 0xA1;
    constexpr unsigned kVkLControl = 0xA2;
    constexpr unsigned kVkRControl = 0xA3;
    constexpr unsigned kVkLMenu = 0xA4;
    constexpr unsigned kVkRMenu = 0xA5;

    constexpr unsigned kWmActivate = 0x0006;
    constexpr unsigned kWmSetFocus = 0x0007;
    constexpr unsigned kWmKillFocus = 0x0008;
    constexpr unsigned kWmActivateApp = 0x001C;
    constexpr unsigned kWmInput = 0x00FF;
    constexpr unsigned kWmKeyDown = 0x0100;
    constexpr unsigned kWmKeyUp = 0x0101;
    constexpr unsigned kWmChar = 0x0102;
    constexpr unsigned kWmDeadChar = 0x0103;
    constexpr unsigned kWmSysKeyDown = 0x0104;
    constexpr unsigned kWmSysKeyUp = 0x0105;
    constexpr unsigned kWmSysChar = 0x0106;
    constexpr unsigned kWmSysDeadChar = 0x0107;
    constexpr unsigned kWmUniChar = 0x0109;
    constexpr unsigned kWmMouseMove = 0x0200;
    constexpr unsigned kWmLButtonDown = 0x0201;
    constexpr unsigned kWmLButtonUp = 0x0202;
    constexpr unsigned kWmLButtonDblClk = 0x0203;
    constexpr unsigned kWmRButtonDown = 0x0204;
    constexpr unsigned kWmRButtonUp = 0x0205;
    constexpr unsigned kWmRButtonDblClk = 0x0206;
    constexpr unsigned kWmMButtonDown = 0x0207;
    constexpr unsigned kWmMButtonUp = 0x0208;
    constexpr unsigned kWmMButtonDblClk = 0x0209;
    constexpr unsigned kWmMouseWheel = 0x020A;
    constexpr unsigned kWmXButtonDown = 0x020B;
    constexpr unsigned kWmXButtonUp = 0x020C;
    constexpr unsigned kWmXButtonDblClk = 0x020D;
    constexpr unsigned kWmMouseHWheel = 0x020E;
    constexpr unsigned kWmMouseFirst = 0x0200;
    constexpr unsigned kWmMouseLast = 0x020E;
    constexpr unsigned kWmKeyFirst = 0x0100;
    constexpr unsigned kWmKeyLast = 0x0109;
    constexpr unsigned kUnicodeNoChar = 0xFFFF; // WM_UNICHAR probe
    constexpr int kWheelDelta = 120;

    // RAWKEYBOARD.Flags / RAWMOUSE.usButtonFlags / RAWMOUSE.usFlags
    constexpr std::uint16_t kRiKeyBreak = 0x01;
    constexpr std::uint16_t kRiKeyE0 = 0x02;
    constexpr std::uint16_t kRiMouseLeftDown = 0x0001;
    constexpr std::uint16_t kRiMouseLeftUp = 0x0002;
    constexpr std::uint16_t kRiMouseRightDown = 0x0004;
    constexpr std::uint16_t kRiMouseRightUp = 0x0008;
    constexpr std::uint16_t kRiMouseMiddleDown = 0x0010;
    constexpr std::uint16_t kRiMouseMiddleUp = 0x0020;
    constexpr std::uint16_t kRiMouseButton4Down = 0x0040;
    constexpr std::uint16_t kRiMouseButton4Up = 0x0080;
    constexpr std::uint16_t kRiMouseButton5Down = 0x0100;
    constexpr std::uint16_t kRiMouseButton5Up = 0x0200;
    constexpr std::uint16_t kRiMouseWheel = 0x0400;
    constexpr std::uint16_t kRiMouseHWheel = 0x0800;
    constexpr std::uint16_t kMouseMoveAbsolute = 0x0001;

    // ---- small conversions --------------------------------------------------

    // ImGui's / ReShade's button index (0 left, 1 right, 2 middle, 3 X1, 4 X2)
    // to its virtual key; 0 for anything else.
    inline unsigned MouseButtonVk(int button)
    {
        switch (button)
        {
        case 0: return kVkLButton;
        case 1: return kVkRButton;
        case 2: return kVkMButton;
        case 3: return kVkXButton1;
        case 4: return kVkXButton2;
        default: return 0;
        }
    }

    inline bool IsMouseVk(unsigned vk)
    {
        return vk == kVkLButton || vk == kVkRButton || vk == kVkMButton || vk == kVkXButton1 || vk == kVkXButton2;
    }

    inline bool IsSpecificModifier(unsigned vk)
    {
        return vk >= kVkLShift && vk <= kVkRMenu;
    }

    // A window message or a raw keyboard packet names Shift/Ctrl/Alt
    // generically; the side comes from the scan code (right Shift is 0x36) or
    // the extended (E0) bit (right Ctrl, right Alt). Other keys unchanged.
    inline unsigned SpecificVk(unsigned vk, unsigned scanCode, bool extended)
    {
        switch (vk)
        {
        case kVkShift: return scanCode == 0x36 ? kVkRShift : kVkLShift;
        case kVkControl: return extended ? kVkRControl : kVkLControl;
        case kVkMenu: return extended ? kVkRMenu : kVkLMenu;
        default: return vk;
        }
    }

    // The generic modifier of a sided one (VK_LSHIFT -> VK_SHIFT), 0 if none.
    inline unsigned GenericOf(unsigned vk)
    {
        switch (vk)
        {
        case kVkLShift: case kVkRShift: return kVkShift;
        case kVkLControl: case kVkRControl: return kVkControl;
        case kVkLMenu: case kVkRMenu: return kVkMenu;
        default: return 0;
        }
    }

    // ---- events -------------------------------------------------------------

    enum class Kind : std::uint8_t
    {
        KeyDown = 0, // code = specific VK (mouse buttons too)
        KeyUp = 1,
        Char = 2,    // code = one UTF-16 unit
        Wheel = 3,   // value = raw delta (120 per notch), up positive
        WheelH = 4,  // value = raw delta, right positive (Win32 sign)
        Focus = 5    // value = 1 gained, 0 lost
    };

    struct Event
    {
        Kind kind = Kind::KeyDown;
        std::uint16_t code = 0;
        std::int32_t value = 0;
        std::uint32_t timeMs = 0; // GetTickCount() at the time the window thread saw it
    };

    // One window message to at most two events (a key and nothing else today).
    // `rawKeyboard` / `rawMouse`: the window registered that device with
    // RIDEV_NOLEGACY, so the legacy messages for it are not trusted (they do
    // not come) and the raw packets are used instead.
    inline std::size_t FromMessage(unsigned msg, std::uint64_t wParam, std::int64_t lParam, std::uint32_t timeMs, Event* out)
    {
        const auto key = [&](Kind kind, unsigned vk) {
            out[0].kind = kind;
            out[0].code = static_cast<std::uint16_t>(vk);
            out[0].value = 0;
            out[0].timeMs = timeMs;
            return std::size_t{1};
        };
        const unsigned scan = static_cast<unsigned>((lParam >> 16) & 0xFF);
        const bool extended = ((lParam >> 24) & 1) != 0;
        const unsigned xbutton = static_cast<unsigned>((wParam >> 16) & 0xFFFF); // HIWORD
        switch (msg)
        {
        case kWmKeyDown:
        case kWmSysKeyDown:
            if ((wParam & 0xFF) == 0 || wParam >= 0xFF)
                return 0;
            return key(Kind::KeyDown, SpecificVk(static_cast<unsigned>(wParam), scan, extended));
        case kWmKeyUp:
        case kWmSysKeyUp:
            if ((wParam & 0xFF) == 0 || wParam >= 0xFF)
                return 0;
            return key(Kind::KeyUp, SpecificVk(static_cast<unsigned>(wParam), scan, extended));
        case kWmChar:
        case kWmSysChar:
            if (wParam == 0 || wParam > 0xFFFF)
                return 0;
            out[0].kind = Kind::Char;
            out[0].code = static_cast<std::uint16_t>(wParam);
            out[0].value = 0;
            out[0].timeMs = timeMs;
            return 1;
        case kWmLButtonDown: case kWmLButtonDblClk: return key(Kind::KeyDown, kVkLButton);
        case kWmLButtonUp: return key(Kind::KeyUp, kVkLButton);
        case kWmRButtonDown: case kWmRButtonDblClk: return key(Kind::KeyDown, kVkRButton);
        case kWmRButtonUp: return key(Kind::KeyUp, kVkRButton);
        case kWmMButtonDown: case kWmMButtonDblClk: return key(Kind::KeyDown, kVkMButton);
        case kWmMButtonUp: return key(Kind::KeyUp, kVkMButton);
        case kWmXButtonDown: case kWmXButtonDblClk:
            return xbutton == 1 || xbutton == 2 ? key(Kind::KeyDown, xbutton == 1 ? kVkXButton1 : kVkXButton2) : 0;
        case kWmXButtonUp:
            return xbutton == 1 || xbutton == 2 ? key(Kind::KeyUp, xbutton == 1 ? kVkXButton1 : kVkXButton2) : 0;
        case kWmMouseWheel:
        case kWmMouseHWheel:
            out[0].kind = msg == kWmMouseWheel ? Kind::Wheel : Kind::WheelH;
            out[0].code = 0;
            out[0].value = static_cast<std::int16_t>((wParam >> 16) & 0xFFFF); // GET_WHEEL_DELTA_WPARAM
            out[0].timeMs = timeMs;
            return 1;
        case kWmSetFocus:
            out[0] = Event{Kind::Focus, 0, 1, timeMs};
            return 1;
        case kWmKillFocus:
            out[0] = Event{Kind::Focus, 0, 0, timeMs};
            return 1;
        case kWmActivateApp:
            out[0] = Event{Kind::Focus, 0, wParam != 0 ? 1 : 0, timeMs};
            return 1;
        default:
            return 0;
        }
    }

    // One raw keyboard packet. VKey 0 and 0xFF (the E1/E0 prefixes, fake
    // shifts) carry no key.
    inline std::size_t FromRawKeyboard(std::uint16_t vkey, std::uint16_t makeCode, std::uint16_t flags, std::uint32_t timeMs, Event* out)
    {
        if (vkey == 0 || vkey >= 0xFF)
            return 0;
        out[0].kind = (flags & kRiKeyBreak) ? Kind::KeyUp : Kind::KeyDown;
        out[0].code = static_cast<std::uint16_t>(SpecificVk(vkey, makeCode, (flags & kRiKeyE0) != 0));
        out[0].value = 0;
        out[0].timeMs = timeMs;
        return 1;
    }

    struct RawButtonBits
    {
        std::uint16_t down;
        std::uint16_t up;
        unsigned vk;
    };

    constexpr RawButtonBits kRawButtons[5] = {
        {kRiMouseLeftDown, kRiMouseLeftUp, kVkLButton},
        {kRiMouseRightDown, kRiMouseRightUp, kVkRButton},
        {kRiMouseMiddleDown, kRiMouseMiddleUp, kVkMButton},
        {kRiMouseButton4Down, kRiMouseButton4Up, kVkXButton1},
        {kRiMouseButton5Down, kRiMouseButton5Up, kVkXButton2},
    };

    // One raw mouse packet: its button transitions and wheel. `out` needs 12.
    inline std::size_t FromRawMouse(std::uint16_t buttonFlags, std::uint16_t buttonData, std::uint32_t timeMs, Event* out)
    {
        std::size_t n = 0;
        for (const RawButtonBits& b : kRawButtons)
        {
            if (buttonFlags & b.down)
                out[n++] = Event{Kind::KeyDown, static_cast<std::uint16_t>(b.vk), 0, timeMs};
            if (buttonFlags & b.up)
                out[n++] = Event{Kind::KeyUp, static_cast<std::uint16_t>(b.vk), 0, timeMs};
        }
        if (buttonFlags & kRiMouseWheel)
            out[n++] = Event{Kind::Wheel, 0, static_cast<std::int16_t>(buttonData), timeMs};
        if (buttonFlags & kRiMouseHWheel)
            out[n++] = Event{Kind::WheelH, 0, static_cast<std::int16_t>(buttonData), timeMs};
        return n;
    }

    // ---- the editor's key state -------------------------------------------

    struct Keys
    {
        static constexpr std::size_t kMaxChars = 64;
        static constexpr std::uint32_t kStaleMs = 5000;

        std::uint8_t down[256]{};     // level at the end of the frame's events
        std::uint8_t pressed[256]{};  // went down this frame (from up)
        std::uint8_t released[256]{}; // went up this frame
        std::uint8_t repeated[256]{}; // an auto-repeat down arrived this frame while down
        std::uint32_t lastMs[256]{};
        std::int32_t wheel = 0;  // raw units this frame
        std::int32_t wheelH = 0;
        std::uint16_t chars[kMaxChars]{};
        std::size_t charCount = 0;
        bool focused = true;
        bool focusChanged = false;

        // Start a frame: the edges of the frame before are gone.
        void BeginFrame()
        {
            for (std::size_t i = 0; i < 256; ++i)
            {
                pressed[i] = 0;
                released[i] = 0;
                repeated[i] = 0;
            }
            wheel = 0;
            wheelH = 0;
            charCount = 0;
            focusChanged = false;
        }

        void Apply(const Event& e)
        {
            switch (e.kind)
            {
            case Kind::KeyDown:
                if (e.code == 0 || e.code >= 256)
                    return;
                if (down[e.code])
                    repeated[e.code] = 1;
                else
                {
                    down[e.code] = 1;
                    pressed[e.code] = 1;
                }
                lastMs[e.code] = e.timeMs;
                SyncGeneric(e.code);
                return;
            case Kind::KeyUp:
                if (e.code == 0 || e.code >= 256)
                    return;
                Up(e.code);
                lastMs[e.code] = e.timeMs;
                SyncGeneric(e.code);
                return;
            case Kind::Char:
                if (e.code != 0 && charCount < kMaxChars)
                    chars[charCount++] = e.code;
                return;
            case Kind::Wheel:
                wheel += e.value;
                return;
            case Kind::WheelH:
                wheelH += e.value;
                return;
            case Kind::Focus:
                if ((e.value != 0) != focused)
                {
                    focused = e.value != 0;
                    focusChanged = true;
                }
                if (!focused)
                    ReleaseAll(); // the ups go to whoever has focus now
                return;
            }
        }

        void ReleaseAll()
        {
            for (unsigned i = 1; i < 256; ++i)
                Up(i);
        }

        // Keys left down without a recent event whose physical state is up
        // (an up eaten by another window, Alt-Tab, both Shifts held): the
        // sided modifiers every frame, the rest after kStaleMs. Mouse buttons
        // never (remapped buttons make the physical state lie).
        template <typename PhysicallyDown>
        void ReleaseStale(std::uint32_t nowMs, PhysicallyDown physicallyDown)
        {
            for (unsigned i = 8; i < 256; ++i)
            {
                if (!down[i] || i == kVkShift || i == kVkControl || i == kVkMenu)
                    continue; // the generic ones follow their sides
                const bool check = IsSpecificModifier(i) || nowMs - lastMs[i] > kStaleMs;
                if (check && !physicallyDown(i))
                {
                    Up(i);
                    SyncGeneric(i);
                }
            }
        }

        bool KeyDown(unsigned vk) const { return vk < 256 && down[vk] != 0; }
        bool KeyPressed(unsigned vk) const { return vk > 0 && vk < 256 && pressed[vk] != 0; }
        bool KeyReleased(unsigned vk) const { return vk > 0 && vk < 256 && released[vk] != 0; }
        bool KeyRepeated(unsigned vk) const { return vk < 256 && repeated[vk] != 0; }
        bool MouseDown(int button) const { return KeyDown(MouseButtonVk(button)); }
        bool MousePressed(int button) const { return KeyPressed(MouseButtonVk(button)); }
        bool MouseReleased(int button) const { return KeyReleased(MouseButtonVk(button)); }

    private:
        void Up(unsigned vk)
        {
            if (down[vk])
            {
                down[vk] = 0;
                released[vk] = 1;
            }
        }

        // VK_SHIFT/VK_CONTROL/VK_MENU follow their two sides.
        void SyncGeneric(unsigned specific)
        {
            const unsigned generic = GenericOf(specific);
            if (generic == 0)
                return;
            const unsigned left = generic == kVkShift ? kVkLShift : generic == kVkControl ? kVkLControl : kVkLMenu;
            const bool level = down[left] || down[left + 1];
            if (level && !down[generic])
            {
                down[generic] = 1;
                pressed[generic] = 1;
            }
            else if (!level && down[generic])
            {
                down[generic] = 0;
                released[generic] = 1;
            }
            lastMs[generic] = lastMs[specific];
        }
    };

    // ---- what the game may see -----------------------------------------------

    // BlockNextFrame(mouse, keyboard): on at once (the editor asks while it
    // builds its frame and the game's input of that same frame must already
    // be hidden, as under ReShade, which applies it at the end of the same
    // present), kept while the editor asks every frame, off at the start of
    // the first frame after one that did not ask.
    struct Block
    {
        bool mouse = false;
        bool keyboard = false;
        bool askedMouse = false;
        bool askedKeyboard = false;

        void Request(bool m, bool k)
        {
            askedMouse = askedMouse || m;
            askedKeyboard = askedKeyboard || k;
            mouse = mouse || m;
            keyboard = keyboard || k;
        }

        void BeginFrame()
        {
            mouse = askedMouse;
            keyboard = askedKeyboard;
            askedMouse = false;
            askedKeyboard = false;
        }
    };

    // Per input path (window messages, raw input): which key downs reached the
    // game. One Gate per path: the same physical key arrives both as WM_KEYDOWN
    // and as a raw packet and each path must close what it opened.
    struct Gate
    {
        std::uint8_t gameDown[256]{};

        // A down: the game sees it unless blocked. An auto-repeat of a key the
        // game already holds is hidden while blocked too.
        bool Down(unsigned vk, bool blocked)
        {
            if (vk == 0 || vk >= 256)
                return !blocked;
            if (blocked)
                return false;
            gameDown[vk] = 1;
            return true;
        }

        // An up: always passes when the game saw the down (so nothing sticks),
        // hidden when blocked otherwise.
        bool Up(unsigned vk, bool blocked)
        {
            if (vk == 0 || vk >= 256)
                return !blocked;
            if (gameDown[vk])
            {
                gameDown[vk] = 0;
                return true;
            }
            return !blocked;
        }
    };

    enum class Route : std::uint8_t
    {
        Pass = 0,   // forward to the next window procedure (ScriptHook, the game)
        Swallow = 1 // the game must not see it (DefWindowProc gets it instead)
    };

    // Alt+F4 always reaches the window: a blocked game must still close.
    inline bool AlwaysPass(unsigned msg, unsigned vk)
    {
        return msg == kWmSysKeyDown && vk == kVkF4;
    }

    // A window message's fate. WM_INPUT always passes here: the raw packet is
    // filtered where the game reads it (GetRawInputData / GetRawInputBuffer).
    inline Route RouteMessage(unsigned msg, std::uint64_t wParam, const Block& block, Gate& gate)
    {
        const unsigned vk = static_cast<unsigned>(wParam & 0xFF);
        const unsigned xbutton = static_cast<unsigned>((wParam >> 16) & 0xFFFF);
        const unsigned xvk = xbutton == 2 ? kVkXButton2 : kVkXButton1;
        auto r = [](bool pass) { return pass ? Route::Pass : Route::Swallow; };
        switch (msg)
        {
        case kWmKeyDown:
        case kWmSysKeyDown:
            if (AlwaysPass(msg, vk))
                return Route::Pass;
            return r(gate.Down(vk, block.keyboard));
        case kWmKeyUp:
        case kWmSysKeyUp:
            return r(gate.Up(vk, block.keyboard));
        case kWmChar:
        case kWmDeadChar:
        case kWmSysChar:
        case kWmSysDeadChar:
        case kWmUniChar:
            return r(!block.keyboard);
        case kWmLButtonDown: case kWmLButtonDblClk: return r(gate.Down(kVkLButton, block.mouse));
        case kWmLButtonUp: return r(gate.Up(kVkLButton, block.mouse));
        case kWmRButtonDown: case kWmRButtonDblClk: return r(gate.Down(kVkRButton, block.mouse));
        case kWmRButtonUp: return r(gate.Up(kVkRButton, block.mouse));
        case kWmMButtonDown: case kWmMButtonDblClk: return r(gate.Down(kVkMButton, block.mouse));
        case kWmMButtonUp: return r(gate.Up(kVkMButton, block.mouse));
        case kWmXButtonDown: case kWmXButtonDblClk: return r(gate.Down(xvk, block.mouse));
        case kWmXButtonUp: return r(gate.Up(xvk, block.mouse));
        case kWmMouseMove:
        case kWmMouseWheel:
        case kWmMouseHWheel:
            return r(!block.mouse);
        default:
            return Route::Pass;
        }
    }

    // A raw keyboard packet as the game will read it. Returns true when it is
    // left untouched; false when it was neutralised (turned into an up of key
    // 0, as ReShade's GetRawInputBuffer hook does, which games ignore).
    inline bool FilterRawKeyboard(std::uint16_t& vkey, std::uint16_t& flags, const Block& block, Gate& gate)
    {
        bool pass;
        if (vkey == 0 || vkey >= 0xFF)
            pass = !block.keyboard;
        else if (flags & kRiKeyBreak)
            pass = gate.Up(vkey, block.keyboard);
        else
            pass = gate.Down(vkey, block.keyboard);
        if (!pass)
        {
            flags = static_cast<std::uint16_t>(flags | kRiKeyBreak);
            vkey = 0;
        }
        return pass;
    }

    struct RawMouse
    {
        std::uint16_t flags = 0;       // usFlags
        std::uint16_t buttonFlags = 0; // usButtonFlags
        std::uint16_t buttonData = 0;  // usButtonData
        std::uint32_t rawButtons = 0;  // ulRawButtons
        std::int32_t lastX = 0;
        std::int32_t lastY = 0;
    };

    // A raw mouse packet as the game will read it. Blocked: no movement, no
    // wheel, no button down, and only the ups of buttons the game saw go down.
    // Not blocked: untouched (the gate still records the downs). Returns true
    // when something is left for the game (false: the packet may be marked as
    // an input-sink one).
    inline bool FilterRawMouse(RawMouse& m, const Block& block, Gate& gate)
    {
        std::uint16_t keep = m.buttonFlags;
        for (const RawButtonBits& b : kRawButtons)
        {
            if ((m.buttonFlags & b.down) && !gate.Down(b.vk, block.mouse))
                keep = static_cast<std::uint16_t>(keep & ~b.down);
            if ((m.buttonFlags & b.up) && !gate.Up(b.vk, block.mouse))
                keep = static_cast<std::uint16_t>(keep & ~b.up);
        }
        if (!block.mouse)
            return true;
        keep = static_cast<std::uint16_t>(keep & ~(kRiMouseWheel | kRiMouseHWheel));
        m.buttonFlags = keep;
        m.buttonData = 0;
        m.rawButtons = 0;
        m.lastX = 0;
        m.lastY = 0;
        m.flags = static_cast<std::uint16_t>(m.flags & ~kMouseMoveAbsolute); // a zero absolute move would jump to the corner
        return keep != 0;
    }

    // GetKeyState / GetAsyncKeyState answered for the game: 0 while the key's
    // kind is blocked (ReShade: keyboard keys are 8..255, buttons 1..6).
    inline bool HideKeyState(int vk, const Block& block)
    {
        if ((vk & 0xF8) != 0)
            return block.keyboard;
        if (vk >= static_cast<int>(kVkLButton) && vk <= static_cast<int>(kVkXButton2))
            return block.mouse;
        return false;
    }

    // ---- ImGui -------------------------------------------------------------

    struct KeyMap
    {
        ImGuiKey key;
        unsigned vk;
    };

    // ReShade's table (runtime_gui.cpp) with VK_F11/F12 fixed (ReShade has
    // 0x80/0x81), F13..F24, the sided modifiers and the keypad/browser keys
    // ImGui 1.92 knows.
    constexpr KeyMap kKeyMap[] = {
        {ImGuiKey_Tab, 0x09}, {ImGuiKey_LeftArrow, 0x25}, {ImGuiKey_RightArrow, 0x27}, {ImGuiKey_UpArrow, 0x26},
        {ImGuiKey_DownArrow, 0x28}, {ImGuiKey_PageUp, 0x21}, {ImGuiKey_PageDown, 0x22}, {ImGuiKey_End, 0x23},
        {ImGuiKey_Home, 0x24}, {ImGuiKey_Insert, 0x2D}, {ImGuiKey_Delete, 0x2E}, {ImGuiKey_Backspace, 0x08},
        {ImGuiKey_Space, 0x20}, {ImGuiKey_Enter, 0x0D}, {ImGuiKey_Escape, 0x1B},
        {ImGuiKey_LeftCtrl, kVkLControl}, {ImGuiKey_LeftShift, kVkLShift}, {ImGuiKey_LeftAlt, kVkLMenu}, {ImGuiKey_LeftSuper, kVkLWin},
        {ImGuiKey_RightCtrl, kVkRControl}, {ImGuiKey_RightShift, kVkRShift}, {ImGuiKey_RightAlt, kVkRMenu}, {ImGuiKey_RightSuper, kVkRWin},
        {ImGuiKey_Menu, 0x5D},
        {ImGuiKey_0, '0'}, {ImGuiKey_1, '1'}, {ImGuiKey_2, '2'}, {ImGuiKey_3, '3'}, {ImGuiKey_4, '4'},
        {ImGuiKey_5, '5'}, {ImGuiKey_6, '6'}, {ImGuiKey_7, '7'}, {ImGuiKey_8, '8'}, {ImGuiKey_9, '9'},
        {ImGuiKey_A, 'A'}, {ImGuiKey_B, 'B'}, {ImGuiKey_C, 'C'}, {ImGuiKey_D, 'D'}, {ImGuiKey_E, 'E'},
        {ImGuiKey_F, 'F'}, {ImGuiKey_G, 'G'}, {ImGuiKey_H, 'H'}, {ImGuiKey_I, 'I'}, {ImGuiKey_J, 'J'},
        {ImGuiKey_K, 'K'}, {ImGuiKey_L, 'L'}, {ImGuiKey_M, 'M'}, {ImGuiKey_N, 'N'}, {ImGuiKey_O, 'O'},
        {ImGuiKey_P, 'P'}, {ImGuiKey_Q, 'Q'}, {ImGuiKey_R, 'R'}, {ImGuiKey_S, 'S'}, {ImGuiKey_T, 'T'},
        {ImGuiKey_U, 'U'}, {ImGuiKey_V, 'V'}, {ImGuiKey_W, 'W'}, {ImGuiKey_X, 'X'}, {ImGuiKey_Y, 'Y'},
        {ImGuiKey_Z, 'Z'},
        {ImGuiKey_F1, 0x70}, {ImGuiKey_F2, 0x71}, {ImGuiKey_F3, 0x72}, {ImGuiKey_F4, 0x73}, {ImGuiKey_F5, 0x74},
        {ImGuiKey_F6, 0x75}, {ImGuiKey_F7, 0x76}, {ImGuiKey_F8, 0x77}, {ImGuiKey_F9, 0x78}, {ImGuiKey_F10, 0x79},
        {ImGuiKey_F11, 0x7A}, {ImGuiKey_F12, 0x7B}, {ImGuiKey_F13, 0x7C}, {ImGuiKey_F14, 0x7D}, {ImGuiKey_F15, 0x7E},
        {ImGuiKey_F16, 0x7F}, {ImGuiKey_F17, 0x80}, {ImGuiKey_F18, 0x81}, {ImGuiKey_F19, 0x82}, {ImGuiKey_F20, 0x83},
        {ImGuiKey_F21, 0x84}, {ImGuiKey_F22, 0x85}, {ImGuiKey_F23, 0x86}, {ImGuiKey_F24, 0x87},
        {ImGuiKey_Apostrophe, 0xDE}, {ImGuiKey_Comma, 0xBC}, {ImGuiKey_Minus, 0xBD}, {ImGuiKey_Period, 0xBE},
        {ImGuiKey_Slash, 0xBF}, {ImGuiKey_Semicolon, 0xBA}, {ImGuiKey_Equal, 0xBB}, {ImGuiKey_LeftBracket, 0xDB},
        {ImGuiKey_Backslash, 0xDC}, {ImGuiKey_RightBracket, 0xDD}, {ImGuiKey_GraveAccent, 0xC0}, {ImGuiKey_Oem102, 0xE2},
        {ImGuiKey_CapsLock, kVkCapital}, {ImGuiKey_ScrollLock, 0x91}, {ImGuiKey_NumLock, 0x90},
        {ImGuiKey_PrintScreen, 0x2C}, {ImGuiKey_Pause, 0x13},
        {ImGuiKey_Keypad0, 0x60}, {ImGuiKey_Keypad1, 0x61}, {ImGuiKey_Keypad2, 0x62}, {ImGuiKey_Keypad3, 0x63},
        {ImGuiKey_Keypad4, 0x64}, {ImGuiKey_Keypad5, 0x65}, {ImGuiKey_Keypad6, 0x66}, {ImGuiKey_Keypad7, 0x67},
        {ImGuiKey_Keypad8, 0x68}, {ImGuiKey_Keypad9, 0x69}, {ImGuiKey_KeypadDecimal, 0x6E}, {ImGuiKey_KeypadDivide, 0x6F},
        {ImGuiKey_KeypadMultiply, 0x6A}, {ImGuiKey_KeypadSubtract, 0x6D}, {ImGuiKey_KeypadAdd, 0x6B},
        {ImGuiKey_AppBack, 0xA6}, {ImGuiKey_AppForward, 0xA7},
    };

    inline ImGuiKey ImGuiKeyOf(unsigned vk)
    {
        for (const KeyMap& m : kKeyMap)
            if (m.vk == vk)
                return m.key;
        return ImGuiKey_None;
    }

    // The editor's modifiers as ImGui wants them (ImGuiMod_* levels).
    struct Mods
    {
        bool ctrl;
        bool shift;
        bool alt;
        bool super;
    };

    inline Mods ModsOf(const Keys& keys)
    {
        return Mods{keys.KeyDown(kVkControl), keys.KeyDown(kVkShift), keys.KeyDown(kVkMenu),
                    keys.KeyDown(kVkLWin) || keys.KeyDown(kVkRWin)};
    }

    // Raw wheel units to ImGui's: notches, ImGui's horizontal sign (positive
    // scrolls left) is the opposite of Win32's (positive is right).
    inline float WheelNotches(std::int32_t raw) { return static_cast<float>(raw) / static_cast<float>(kWheelDelta); }
    inline float WheelHNotches(std::int32_t raw) { return -static_cast<float>(raw) / static_cast<float>(kWheelDelta); }

    // A client-area position to ImGui's display space (the back buffer may be
    // smaller or larger than the client area: render scale, a borderless
    // window stretched by the compositor). Client size 0: unscaled.
    inline void ScaleMouse(float clientX, float clientY, float clientW, float clientH, float displayW, float displayH, float& outX, float& outY)
    {
        outX = clientW > 0.0f && displayW > 0.0f ? clientX * (displayW / clientW) : clientX;
        outY = clientH > 0.0f && displayH > 0.0f ? clientY * (displayH / clientH) : clientY;
    }

    // Raw input registrations decide which path carries the editor's keys:
    // with RIDEV_NOLEGACY on a device the window gets no WM_KEY*/WM_*BUTTON*
    // for it, so the raw packets are read instead. Never both (a key would
    // count twice).
    struct Sources
    {
        bool rawKeyboard = false;
        bool rawMouse = false;
    };
}
