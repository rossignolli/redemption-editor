#pragma once

// Keyboard and mouse for the editor's own host (no ReShade): the editor's key
// state per frame, ImGui's IO, and hiding the game's input while the editor
// UI wants it.
//
// Threads: Install/Uninstall/BeginFrame/FeedImGui/BlockNextFrame and the key
// queries run on the render thread (the one that presents). The window
// procedure runs on the game's window thread and only queues events; the
// user32/dinput8 detours run on whatever game thread calls them and only read
// the block flags (plus the raw-input gate under a mutex).

#include <imgui.h>

#include <Windows.h>

namespace host::input
{
    bool Install(HWND gameWindow);    // WndProc chain + user32/dinput8 hooks (MinHook)
    void Uninstall();
    void BeginFrame();                // latch edges for this frame (taps lost in long frames kept)
    bool KeyDown(unsigned vk);
    bool KeyPressed(unsigned vk);     // went down this frame
    bool KeyReleased(unsigned vk);    // went up this frame
    bool MouseDown(int button);
    bool MouseReleased(int button);
    void BlockNextFrame(bool mouse, bool keyboard); // the game sees nothing while set
    void FeedImGui(ImGuiIO& io);      // mouse pos, buttons, wheel, chars, keys, modifiers, focus (after io.DisplaySize is set)

    // Additions to the contract (not required by the callers above):

    // Where the input code's log lines go (one line, no newline). Default:
    // host.log in the host DLL (REDEMPTION_HOST_LOG), OutputDebugStringA
    // elsewhere (ReplayCoreTests).
    void SetLogSink(void (*sink)(const char* line));
    // The mouse position latched at BeginFrame, client pixels of the game
    // window; false when the window has no client area (minimised).
    bool MousePosition(int& x, int& y);
    // UTF-16 text typed this frame (WM_CHAR, or ToUnicode on raw keys).
    const wchar_t* TextInput(unsigned& count);
    // Mouse wheel this frame, notches (positive up).
    float MouseWheel();
}
