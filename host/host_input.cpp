// Keyboard and mouse for the editor's own host: the window procedure that
// collects input, the user32/dinput8 detours that hide it from the game while
// the editor UI wants it, and the per-frame latch. FeedImGui lives in
// host_input_imgui.cpp (the only part that links ImGui), so this file builds
// into ReplayCoreTests too (tests/host_input_tests.cpp drives it with a
// message-only window).
//
// Origin: the set of detours and what each one hides (GetRawInputBuffer's
// key-up of key 0, GetCursorPos answering the game's last position,
// ClipCursor lifted while blocked, GetKeyState ranges, the DirectInput
// device filters) follows ReShade's source/input.cpp and
// source/windows/dinput8.cpp (Copyright (C) 2014-2021 Patrick Mours,
// SPDX-License-Identifier: BSD-3-Clause). ReShade blocks inside its
// GetMessage/PeekMessage hooks; this host subclasses the game window instead
// (ScriptHookRDR2 already does, the chain is respected).
//
// Paths the game can read input through, and what happens while blocked:
//  - WM_KEY*/WM_CHAR/WM_*BUTTON*/WM_MOUSEMOVE/WM_MOUSEWHEEL: swallowed in the
//    window procedure (an up whose down the game saw always passes; Alt+F4
//    always passes).
//  - WM_INPUT: passes, but GetRawInputData / GetRawInputBuffer hand the game
//    a neutralised packet (no motion, no wheel, no button downs, keys turned
//    into an up of key 0, header marked RIM_INPUTSINK when nothing is left).
//  - GetKeyState / GetAsyncKeyState: 0 for the blocked kind.
//  - GetCursorPos: the game's last known position; SetCursorPos: recorded,
//    not applied; ClipCursor: lifted (restored when the block ends).
//  - DirectInput keyboard/mouse devices: GetDeviceState zeroed,
//    GetDeviceData returns no items.
//  - Not blocked: XInput (the controller stays with the game), ShowCursor /
//    SetCursor (the editor draws its own cursor), GetKeyboardState,
//    SetWindowsHookEx hooks (the game's low-level hooks see everything, as
//    they call CallNextHookEx), GameInput (not imported).

#include "host_input.h"
#include "input_rules.h"

#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <dinput.h>

#include <MinHook.h>

#ifdef REDEMPTION_HOST_LOG
#include "host_log.h"
#endif

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace host::input
{
    namespace
    {
        namespace r = host::input::rules;

        // ---- log ----------------------------------------------------------
        std::atomic<void (*)(const char*)> g_logSink{nullptr};

        void Log(const char* format, ...)
        {
            char line[512];
            va_list args;
            va_start(args, format);
            std::vsnprintf(line, sizeof(line), format, args);
            va_end(args);
            if (auto* sink = g_logSink.load())
                sink(line);
            else
            {
#ifdef REDEMPTION_HOST_LOG
                host::log::Write(std::strstr(line, " failed ") != nullptr ? host::log::Level::Warning : host::log::Level::Info, line);
#else
                OutputDebugStringA(line);
                OutputDebugStringA("\n");
#endif
            }
        }

        // ---- originals (the real function until a hook replaces it) -------
        using GetRawInputData_t = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
        using GetRawInputBuffer_t = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
        using GetCursorPos_t = BOOL(WINAPI*)(LPPOINT);
        using SetCursorPos_t = BOOL(WINAPI*)(int, int);
        using ClipCursor_t = BOOL(WINAPI*)(const RECT*);
        using GetKeyState_t = SHORT(WINAPI*)(int);
        using DiGetDeviceState_t = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID);
        using DiGetDeviceData_t = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
        using DiGetCapabilities_t = HRESULT(STDMETHODCALLTYPE*)(void*, LPDIDEVCAPS);

        GetRawInputData_t g_getRawInputData = ::GetRawInputData;
        GetRawInputBuffer_t g_getRawInputBuffer = ::GetRawInputBuffer;
        GetCursorPos_t g_getCursorPos = ::GetCursorPos;
        SetCursorPos_t g_setCursorPos = ::SetCursorPos;
        ClipCursor_t g_clipCursor = ::ClipCursor;
        GetKeyState_t g_getKeyState = ::GetKeyState;
        GetKeyState_t g_getAsyncKeyState = ::GetAsyncKeyState;

        constexpr int kDiSlots = 4;
        DiGetDeviceState_t g_diState[kDiSlots]{};
        DiGetDeviceData_t g_diData[kDiSlots]{};

        // ---- shared state -----------------------------------------------
        std::atomic<bool> g_blockMouse{false};
        std::atomic<bool> g_blockKeyboard{false};

        std::mutex g_queueMutex;
        std::vector<r::Event> g_queue;
        constexpr std::size_t kQueueCap = 4096;

        // The windows our procedure sits in. Normally one; a window that was
        // left as a pass-through (someone chained after us) keeps its slot so
        // its procedure still reaches the next one.
        struct Chain
        {
            std::atomic<HWND> hwnd{nullptr};
            std::atomic<WNDPROC> next{nullptr};
            std::atomic<bool> active{false}; // false: pass-through
        };
        constexpr int kChains = 4;
        Chain g_chains[kChains];
        std::atomic<HWND> g_hwnd{nullptr}; // the window being served
        std::atomic<bool> g_installed{false};

        std::atomic<bool> g_rawKeyboard{false}; // RIDEV_NOLEGACY: the editor reads raw packets
        std::atomic<bool> g_rawMouse{false};
        std::atomic<ULONGLONG> g_sourcesCheckedAt{0};

        // The position the game last set or read (answered while blocked).
        std::atomic<long long> g_gameCursor{0};
        std::mutex g_clipMutex;
        RECT g_gameClip{};
        bool g_gameClipSet = false;

        // Raw input as the game reads it: one gate, one small handle cache so
        // a second GetRawInputData on the same packet gets the same answer.
        std::mutex g_rawMutex;
        r::Gate g_rawGate;
        struct CachedPacket
        {
            HRAWINPUT handle = nullptr;
            RAWINPUT before{}; // as read (a recycled handle with other data is not a hit)
            RAWINPUT after{};  // as handed to the game
        };
        CachedPacket g_rawCache[8];
        unsigned g_rawCacheNext = 0;

        // Window thread only.
        r::Gate g_msgGate;
        BYTE g_textKeyState[256]{};

        // Render thread only.
        r::Keys g_keys;
        r::Block g_block;
        int g_mouseX = 0;
        int g_mouseY = 0;
        int g_clientW = 0;
        int g_clientH = 0;
        bool g_mouseValid = false;
        wchar_t g_text[r::Keys::kMaxChars + 1]{};
        unsigned g_textCount = 0;

        std::vector<void*> g_hookTargets;

        long long PackPoint(int x, int y)
        {
            return (static_cast<long long>(static_cast<unsigned>(x)) << 32) | static_cast<unsigned>(y);
        }
        POINT UnpackPoint(long long v)
        {
            return POINT{static_cast<LONG>(static_cast<int>(static_cast<unsigned long long>(v) >> 32)),
                         static_cast<LONG>(static_cast<int>(static_cast<unsigned>(v & 0xFFFFFFFF)))};
        }

        r::Block CurrentBlock()
        {
            r::Block b;
            b.mouse = g_blockMouse.load(std::memory_order_relaxed);
            b.keyboard = g_blockKeyboard.load(std::memory_order_relaxed);
            return b;
        }

        void Push(const r::Event* events, std::size_t count)
        {
            if (count == 0)
                return;
            const std::lock_guard<std::mutex> lock(g_queueMutex);
            for (std::size_t i = 0; i < count && g_queue.size() < kQueueCap; ++i)
                g_queue.push_back(events[i]);
        }

        // Which devices the game registered with RIDEV_NOLEGACY (process-wide),
        // re-read at most every 2 s from the window thread.
        void RefreshSources(bool force)
        {
            const ULONGLONG now = GetTickCount64();
            if (!force && now - g_sourcesCheckedAt.load(std::memory_order_relaxed) < 2000)
                return;
            g_sourcesCheckedAt.store(now, std::memory_order_relaxed);
            RAWINPUTDEVICE devices[16];
            UINT count = 16;
            const UINT got = GetRegisteredRawInputDevices(devices, &count, sizeof(RAWINPUTDEVICE));
            bool rawKeyboard = false;
            bool rawMouse = false;
            if (got != static_cast<UINT>(-1))
            {
                for (UINT i = 0; i < got; ++i)
                {
                    if (devices[i].usUsagePage != 1 || (devices[i].dwFlags & RIDEV_NOLEGACY) == 0)
                        continue;
                    rawKeyboard = rawKeyboard || devices[i].usUsage == 6;
                    rawMouse = rawMouse || devices[i].usUsage == 2;
                }
            }
            g_rawKeyboard.store(rawKeyboard, std::memory_order_relaxed);
            g_rawMouse.store(rawMouse, std::memory_order_relaxed);
        }

        // A raw packet's keys/buttons/wheel for the editor (unfiltered).
        void CollectRaw(const RAWINPUT& raw, DWORD timeMs)
        {
            r::Event events[12];
            std::size_t n = 0;
            if (raw.header.dwType == RIM_TYPEKEYBOARD && g_rawKeyboard.load(std::memory_order_relaxed))
            {
                const RAWKEYBOARD& k = raw.data.keyboard;
                n = r::FromRawKeyboard(k.VKey, k.MakeCode, k.Flags, timeMs, events);
                if (k.VKey > 0 && k.VKey < 0xFF)
                {
                    const bool up = (k.Flags & RI_KEY_BREAK) != 0;
                    g_textKeyState[k.VKey] = up ? 0 : 0x80;
                    g_textKeyState[VK_CAPITAL] = static_cast<BYTE>(g_getKeyState(VK_CAPITAL) & 1);
                    // No WM_CHAR without legacy messages: the text from the
                    // layout (flag 4: the keyboard state is left untouched).
                    WCHAR text[4]{};
                    const int chars = up ? 0 : ToUnicode(k.VKey, k.MakeCode, g_textKeyState, text, 3, 0x4);
                    for (int i = 0; i < chars && n < 12; ++i)
                        events[n++] = r::Event{r::Kind::Char, static_cast<std::uint16_t>(text[i]), 0, timeMs};
                }
            }
            else if (raw.header.dwType == RIM_TYPEMOUSE && g_rawMouse.load(std::memory_order_relaxed))
                n = r::FromRawMouse(raw.data.mouse.usButtonFlags, raw.data.mouse.usButtonData, timeMs, events);
            Push(events, n);
        }

        // ---- the game's view of raw input --------------------------------
        void FilterPacket(RAWINPUT& raw, UINT size, const r::Block& block)
        {
            if (raw.header.dwType == RIM_TYPEMOUSE && size >= sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE))
            {
                RAWMOUSE& m = raw.data.mouse;
                r::RawMouse f;
                f.flags = m.usFlags;
                f.buttonFlags = m.usButtonFlags;
                f.buttonData = m.usButtonData;
                f.rawButtons = m.ulRawButtons;
                f.lastX = m.lLastX;
                f.lastY = m.lLastY;
                const bool left = r::FilterRawMouse(f, block, g_rawGate);
                if (!block.mouse)
                    return;
                m.usFlags = f.flags;
                m.usButtonFlags = f.buttonFlags;
                m.usButtonData = f.buttonData;
                m.ulRawButtons = f.rawButtons;
                m.lLastX = f.lastX;
                m.lLastY = f.lastY;
                m.ulExtraInformation = 0;
                if (!left)
                    raw.header.wParam = RIM_INPUTSINK;
            }
            else if (raw.header.dwType == RIM_TYPEKEYBOARD && size >= sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD))
            {
                RAWKEYBOARD& k = raw.data.keyboard;
                std::uint16_t vkey = k.VKey;
                std::uint16_t flags = k.Flags;
                if (!r::FilterRawKeyboard(vkey, flags, block, g_rawGate))
                {
                    k.VKey = vkey;
                    k.Flags = flags;
                    k.MakeCode = 0;
                    k.Message = WM_KEYUP;
                    raw.header.wParam = RIM_INPUTSINK;
                }
            }
        }

        UINT WINAPI HookGetRawInputData(HRAWINPUT handle, UINT command, LPVOID data, PUINT size, UINT headerSize)
        {
            const UINT result = g_getRawInputData(handle, command, data, size, headerSize);
            if (command != RID_INPUT || data == nullptr || result == static_cast<UINT>(-1) || result < sizeof(RAWINPUTHEADER))
                return result;
            RAWINPUT& raw = *static_cast<RAWINPUT*>(data);
            const r::Block block = CurrentBlock();
            // Fast path: mouse motion with nothing blocked changes no state.
            if (!block.mouse && !block.keyboard && raw.header.dwType == RIM_TYPEMOUSE &&
                result >= sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE) && raw.data.mouse.usButtonFlags == 0)
                return result;
            if (raw.header.dwType != RIM_TYPEMOUSE && raw.header.dwType != RIM_TYPEKEYBOARD)
                return result;
            const UINT bytes = result < sizeof(RAWINPUT) ? result : static_cast<UINT>(sizeof(RAWINPUT));
            const std::lock_guard<std::mutex> lock(g_rawMutex);
            for (const CachedPacket& c : g_rawCache)
            {
                if (c.handle == handle && std::memcmp(&c.before, &raw, bytes) == 0)
                {
                    std::memcpy(&raw, &c.after, bytes);
                    return result;
                }
            }
            CachedPacket& slot = g_rawCache[g_rawCacheNext++ % 8];
            slot.handle = handle;
            slot.before = RAWINPUT{};
            std::memcpy(&slot.before, &raw, bytes);
            FilterPacket(raw, result, block);
            slot.after = RAWINPUT{};
            std::memcpy(&slot.after, &raw, bytes);
            return result;
        }

        // Buffered raw input never arrives as WM_INPUT in the window procedure:
        // the editor's events are taken here too (before filtering).
        UINT WINAPI HookGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT headerSize)
        {
            const UINT result = g_getRawInputBuffer(data, size, headerSize);
            if (data == nullptr || result == 0 || result == static_cast<UINT>(-1))
                return result;
            const r::Block block = CurrentBlock();
            const DWORD now = GetTickCount();
            using QWORD = UINT64; // NEXTRAWINPUTBLOCK needs it (as in ReShade)
            const std::lock_guard<std::mutex> lock(g_rawMutex);
            PRAWINPUT packet = data;
            for (UINT i = 0; i < result; ++i, packet = NEXTRAWINPUTBLOCK(packet))
            {
                if (packet->header.dwType != RIM_TYPEMOUSE && packet->header.dwType != RIM_TYPEKEYBOARD)
                    continue;
                if (g_installed.load(std::memory_order_relaxed) && packet->header.wParam == RIM_INPUT)
                    CollectRaw(*packet, now);
                if (!block.mouse && !block.keyboard && packet->header.dwType == RIM_TYPEMOUSE && packet->data.mouse.usButtonFlags == 0)
                    continue;
                FilterPacket(*packet, packet->header.dwSize, block);
            }
            return result;
        }

        BOOL WINAPI HookGetCursorPos(LPPOINT point)
        {
            if (point != nullptr && g_blockMouse.load(std::memory_order_relaxed))
            {
                *point = UnpackPoint(g_gameCursor.load(std::memory_order_relaxed));
                return TRUE;
            }
            const BOOL ok = g_getCursorPos(point);
            if (ok && point != nullptr)
                g_gameCursor.store(PackPoint(point->x, point->y), std::memory_order_relaxed);
            return ok;
        }

        BOOL WINAPI HookSetCursorPos(int x, int y)
        {
            g_gameCursor.store(PackPoint(x, y), std::memory_order_relaxed);
            if (g_blockMouse.load(std::memory_order_relaxed))
                return TRUE; // the cursor stays where the user left it
            return g_setCursorPos(x, y);
        }

        BOOL WINAPI HookClipCursor(const RECT* rect)
        {
            {
                const std::lock_guard<std::mutex> lock(g_clipMutex);
                g_gameClipSet = rect != nullptr;
                g_gameClip = rect != nullptr ? *rect : RECT{};
            }
            if (g_blockMouse.load(std::memory_order_relaxed))
                rect = nullptr; // free over the whole screen while the editor has the mouse
            return g_clipCursor(rect);
        }

        SHORT WINAPI HookGetKeyState(int vk)
        {
            if (r::HideKeyState(vk, CurrentBlock()))
                return 0;
            return g_getKeyState(vk);
        }

        SHORT WINAPI HookGetAsyncKeyState(int vk)
        {
            if (r::HideKeyState(vk, CurrentBlock()))
                return 0;
            return g_getAsyncKeyState(vk);
        }

        // ---- DirectInput -------------------------------------------------
        BYTE DeviceType(void* device)
        {
            void** vtable = *static_cast<void***>(device);
            DIDEVCAPS caps{};
            caps.dwSize = sizeof(caps);
            if (FAILED(reinterpret_cast<DiGetCapabilities_t>(vtable[3])(device, &caps)))
                return 0;
            return LOBYTE(caps.dwDevType);
        }

        template <int Slot>
        HRESULT STDMETHODCALLTYPE HookDiGetDeviceState(void* device, DWORD size, LPVOID data)
        {
            const HRESULT hr = g_diState[Slot](device, size, data);
            const r::Block block = CurrentBlock();
            if (FAILED(hr) || data == nullptr || (!block.mouse && !block.keyboard))
                return hr;
            switch (DeviceType(device))
            {
            case DI8DEVTYPE_MOUSE:
                if (block.mouse && (size == sizeof(DIMOUSESTATE) || size == sizeof(DIMOUSESTATE2)))
                    std::memset(data, 0, size); // no motion, no buttons
                break;
            case DI8DEVTYPE_KEYBOARD:
                if (block.keyboard)
                    std::memset(data, 0, size);
                break;
            default:
                break;
            }
            return hr;
        }

        template <int Slot>
        HRESULT STDMETHODCALLTYPE HookDiGetDeviceData(void* device, DWORD objectSize, LPDIDEVICEOBJECTDATA items, LPDWORD inOut, DWORD flags)
        {
            HRESULT hr = g_diData[Slot](device, objectSize, items, inOut, flags);
            const r::Block block = CurrentBlock();
            if (FAILED(hr) || (flags & DIGDD_PEEK) != 0 || items == nullptr || inOut == nullptr || *inOut == 0 || (!block.mouse && !block.keyboard))
                return hr;
            const BYTE type = DeviceType(device);
            if ((type == DI8DEVTYPE_MOUSE && block.mouse) || (type == DI8DEVTYPE_KEYBOARD && block.keyboard))
            {
                *inOut = 0;
                hr = DI_OK; // a DI_BUFFEROVERFLOW is not the game's business either
            }
            return hr;
        }

        void* const kDiStateDetours[kDiSlots] = {
            reinterpret_cast<void*>(&HookDiGetDeviceState<0>), reinterpret_cast<void*>(&HookDiGetDeviceState<1>),
            reinterpret_cast<void*>(&HookDiGetDeviceState<2>), reinterpret_cast<void*>(&HookDiGetDeviceState<3>)};
        void* const kDiDataDetours[kDiSlots] = {
            reinterpret_cast<void*>(&HookDiGetDeviceData<0>), reinterpret_cast<void*>(&HookDiGetDeviceData<1>),
            reinterpret_cast<void*>(&HookDiGetDeviceData<2>), reinterpret_cast<void*>(&HookDiGetDeviceData<3>)};

        // dxguid.lib values, kept here so nothing extra links.
        constexpr GUID kIidDirectInput8W = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
        constexpr GUID kIidDirectInput8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
        constexpr GUID kGuidSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
        constexpr GUID kGuidSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

        bool HookAt(void* target, void* detour, void** original, const char* name)
        {
            MH_STATUS status = MH_CreateHook(target, detour, original);
            if (status == MH_ERROR_ALREADY_CREATED)
                status = MH_OK; // a second Install: the hook is ours already
            if (status == MH_OK)
                status = MH_EnableHook(target);
            if (status != MH_OK && status != MH_ERROR_ENABLED)
            {
                Log("HOST_INPUT_HOOK failed reason=%s fn=%s", MH_StatusToString(status), name);
                return false;
            }
            bool known = false;
            for (void* t : g_hookTargets)
                known = known || t == target;
            if (!known)
                g_hookTargets.push_back(target);
            return true;
        }

        template <typename Fn>
        bool HookExport(HMODULE module, const char* name, void* detour, Fn& original)
        {
            void* target = module != nullptr ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
            if (target == nullptr)
            {
                Log("HOST_INPUT_HOOK failed reason=no_export fn=%s", name);
                return false;
            }
            void* trampoline = nullptr;
            if (!HookAt(target, detour, &trampoline, name))
                return false;
            if (trampoline != nullptr)
                original = reinterpret_cast<Fn>(trampoline);
            return true;
        }

        // The game's DirectInput devices share their class's vtable: a dummy
        // keyboard and mouse (W and A factories) give the functions to hook.
        int HookDirectInput()
        {
            HMODULE dinput = LoadLibraryW(L"dinput8.dll"); // the game has it loaded; the reference is kept
            using Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
            const auto create = dinput != nullptr ? reinterpret_cast<Create_t>(GetProcAddress(dinput, "DirectInput8Create")) : nullptr;
            if (create == nullptr)
            {
                Log("HOST_INPUT_HOOK failed reason=no_dinput8 fn=DirectInput8Create");
                return 0;
            }
            void* stateTargets[kDiSlots]{};
            void* dataTargets[kDiSlots]{};
            int states = 0;
            int datas = 0;
            const auto add = [](void** list, int& count, void* fn) {
                for (int i = 0; i < count; ++i)
                    if (list[i] == fn)
                        return;
                if (count < kDiSlots)
                    list[count++] = fn;
            };
            const GUID* factories[] = {&kIidDirectInput8W, &kIidDirectInput8A};
            const GUID* devices[] = {&kGuidSysKeyboard, &kGuidSysMouse};
            for (const GUID* iid : factories)
            {
                IUnknown* factory = nullptr;
                if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, *iid, reinterpret_cast<LPVOID*>(&factory), nullptr)) || factory == nullptr)
                    continue;
                for (const GUID* guid : devices)
                {
                    IUnknown* device = nullptr;
                    // IDirectInput8{A,W}::CreateDevice is vtable slot 3 in both.
                    using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, void**, LPUNKNOWN);
                    void** vtable = *reinterpret_cast<void***>(factory);
                    if (FAILED(reinterpret_cast<CreateDevice_t>(vtable[3])(factory, *guid, reinterpret_cast<void**>(&device), nullptr)) || device == nullptr)
                        continue;
                    void** deviceTable = *reinterpret_cast<void***>(device);
                    add(stateTargets, states, deviceTable[9]); // GetDeviceState
                    add(dataTargets, datas, deviceTable[10]);  // GetDeviceData
                    device->Release();
                }
                factory->Release();
            }
            int hooked = 0;
            for (int i = 0; i < states; ++i)
            {
                void* trampoline = nullptr;
                if (HookAt(stateTargets[i], kDiStateDetours[i], &trampoline, "IDirectInputDevice8::GetDeviceState"))
                {
                    if (trampoline != nullptr)
                        g_diState[i] = reinterpret_cast<DiGetDeviceState_t>(trampoline);
                    ++hooked;
                }
            }
            for (int i = 0; i < datas; ++i)
            {
                void* trampoline = nullptr;
                if (HookAt(dataTargets[i], kDiDataDetours[i], &trampoline, "IDirectInputDevice8::GetDeviceData"))
                {
                    if (trampoline != nullptr)
                        g_diData[i] = reinterpret_cast<DiGetDeviceData_t>(trampoline);
                    ++hooked;
                }
            }
            return hooked;
        }

        bool g_hooksCreated = false;

        void InstallHooks()
        {
            if (g_hooksCreated)
            {
                for (void* target : g_hookTargets)
                    MH_EnableHook(target);
                return;
            }
            const MH_STATUS init = MH_Initialize();
            if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
            {
                Log("HOST_INPUT_HOOK failed reason=%s fn=MH_Initialize", MH_StatusToString(init));
                return;
            }
            g_hooksCreated = true;
            HMODULE user32 = GetModuleHandleW(L"user32.dll");
            int hooked = 0;
            hooked += HookExport(user32, "GetRawInputData", reinterpret_cast<void*>(&HookGetRawInputData), g_getRawInputData) ? 1 : 0;
            hooked += HookExport(user32, "GetRawInputBuffer", reinterpret_cast<void*>(&HookGetRawInputBuffer), g_getRawInputBuffer) ? 1 : 0;
            hooked += HookExport(user32, "GetCursorPos", reinterpret_cast<void*>(&HookGetCursorPos), g_getCursorPos) ? 1 : 0;
            hooked += HookExport(user32, "SetCursorPos", reinterpret_cast<void*>(&HookSetCursorPos), g_setCursorPos) ? 1 : 0;
            hooked += HookExport(user32, "ClipCursor", reinterpret_cast<void*>(&HookClipCursor), g_clipCursor) ? 1 : 0;
            hooked += HookExport(user32, "GetKeyState", reinterpret_cast<void*>(&HookGetKeyState), g_getKeyState) ? 1 : 0;
            hooked += HookExport(user32, "GetAsyncKeyState", reinterpret_cast<void*>(&HookGetAsyncKeyState), g_getAsyncKeyState) ? 1 : 0;
            const int dinput = HookDirectInput();
            Log("HOST_INPUT hooks user32=%d/7 dinput8=%d", hooked, dinput);
        }

        // ---- block ---------------------------------------------------------
        // Render thread: publish the block to the detours; lift the game's
        // cursor clip when the mouse block starts, put it back when it ends.
        void PublishBlock(bool mouseBefore)
        {
            g_blockMouse.store(g_block.mouse, std::memory_order_relaxed);
            g_blockKeyboard.store(g_block.keyboard, std::memory_order_relaxed);
            if (!g_installed.load(std::memory_order_relaxed) || mouseBefore == g_block.mouse)
                return;
            if (g_block.mouse)
            {
                g_clipCursor(nullptr);
                return;
            }
            RECT clip{};
            bool set = false;
            {
                const std::lock_guard<std::mutex> lock(g_clipMutex);
                clip = g_gameClip;
                set = g_gameClipSet;
            }
            if (set && clip.right > clip.left && clip.bottom > clip.top)
                g_clipCursor(&clip);
        }

        // ---- window procedure ----------------------------------------------
        Chain* FindChain(HWND hwnd)
        {
            for (Chain& c : g_chains)
                if (c.hwnd.load(std::memory_order_acquire) == hwnd)
                    return &c;
            return nullptr;
        }

        LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            Chain* chain = FindChain(hwnd);
            const WNDPROC next = chain != nullptr ? chain->next.load(std::memory_order_acquire) : nullptr;
            const auto forward = [&]() -> LRESULT {
                return next != nullptr ? CallWindowProcW(next, hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
            };
            if (chain == nullptr || !chain->active.load(std::memory_order_acquire) || !g_installed.load(std::memory_order_acquire))
                return forward();

            const bool keyMsg = msg >= r::kWmKeyFirst && msg <= r::kWmKeyLast;
            const bool mouseMsg = msg >= r::kWmMouseFirst && msg <= r::kWmMouseLast;
            const DWORD time = static_cast<DWORD>(GetMessageTime());

            if (msg == WM_INPUT)
            {
                RefreshSources(false);
                if (GET_RAWINPUT_CODE_WPARAM(wParam) == RIM_INPUT && (g_rawKeyboard.load(std::memory_order_relaxed) || g_rawMouse.load(std::memory_order_relaxed)))
                {
                    RAWINPUT raw{};
                    UINT size = sizeof(raw);
                    const UINT got = g_getRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER));
                    if (got != static_cast<UINT>(-1) && got >= sizeof(RAWINPUTHEADER))
                        CollectRaw(raw, time);
                }
                return forward(); // the game reads it through GetRawInputData (filtered there)
            }

            const bool fromLegacy = (keyMsg && !g_rawKeyboard.load(std::memory_order_relaxed)) ||
                                    (mouseMsg && !g_rawMouse.load(std::memory_order_relaxed)) ||
                                    (!keyMsg && !mouseMsg);
            if (fromLegacy)
            {
                r::Event events[2];
                Push(events, r::FromMessage(msg, wParam, lParam, time, events));
            }

            if ((keyMsg || mouseMsg) && r::RouteMessage(msg, wParam, CurrentBlock(), g_msgGate) == r::Route::Swallow)
                return 0; // handled: neither ScriptHook nor the game sees it
            return forward();
        }

        void PinModule()
        {
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&HostWndProc), &self);
        }
    }

    void SetLogSink(void (*sink)(const char* line))
    {
        g_logSink.store(sink);
    }

    bool Install(HWND gameWindow)
    {
        if (gameWindow == nullptr || !IsWindow(gameWindow))
        {
            Log("HOST_INPUT failed reason=no_window");
            return false;
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(gameWindow, &pid);
        if (pid != GetCurrentProcessId())
        {
            Log("HOST_INPUT failed reason=window_of_other_process");
            return false;
        }

        const HWND current = g_hwnd.load();
        if (current != nullptr && current != gameWindow)
            Uninstall(); // a new window: leave the old one (pass-through if not the head)

        if (Chain* chain = FindChain(gameWindow))
        {
            // Our procedure still sits in this window's chain (an Uninstall
            // that was not the head, or a second Install): switch it on.
            chain->active.store(true, std::memory_order_release);
        }
        else
        {
            Chain* slot = nullptr;
            for (Chain& c : g_chains)
                if (slot == nullptr && c.hwnd.load() == nullptr)
                    slot = &c;
            if (slot == nullptr)
            {
                Log("HOST_INPUT failed reason=too_many_windows");
                return false;
            }
            PinModule(); // a window procedure or a detour of ours may outlive Uninstall
            // The slot is filled before the swap: a message that arrives in
            // between already finds the next procedure.
            slot->next.store(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(gameWindow, GWLP_WNDPROC)), std::memory_order_release);
            slot->active.store(true, std::memory_order_release);
            slot->hwnd.store(gameWindow, std::memory_order_release);
            SetLastError(0);
            const LONG_PTR previous = SetWindowLongPtrW(gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HostWndProc));
            if (previous == 0 && GetLastError() != 0)
            {
                const DWORD error = GetLastError();
                slot->hwnd.store(nullptr);
                slot->next.store(nullptr);
                slot->active.store(false);
                Log("HOST_INPUT failed reason=set_wndproc error=%lu", error);
                return false;
            }
            slot->next.store(reinterpret_cast<WNDPROC>(previous), std::memory_order_release);
        }
        g_hwnd.store(gameWindow);
        g_installed.store(true, std::memory_order_release);

        InstallHooks();
        RefreshSources(true);
        POINT cursor{};
        if (g_getCursorPos(&cursor)) // what GetCursorPos answers the game if it is blocked before it ever asked
            g_gameCursor.store(PackPoint(cursor.x, cursor.y), std::memory_order_relaxed);
        g_keys = r::Keys{};
        g_keys.focused = GetForegroundWindow() == gameWindow;
        Log("HOST_INPUT installed hwnd=%p raw_keyboard=%d raw_mouse=%d", static_cast<void*>(gameWindow),
            g_rawKeyboard.load() ? 1 : 0, g_rawMouse.load() ? 1 : 0);
        return true;
    }

    void Uninstall()
    {
        const HWND hwnd = g_hwnd.load();
        const bool mouseBefore = g_block.mouse;
        g_block = r::Block{};
        PublishBlock(mouseBefore);
        g_installed.store(false, std::memory_order_release);
        for (void* target : g_hookTargets)
            MH_DisableHook(target); // trampolines stay (a game thread may be inside one)
        g_hwnd.store(nullptr);
        {
            const std::lock_guard<std::mutex> lock(g_queueMutex);
            g_queue.clear();
        }
        Chain* chain = hwnd != nullptr ? FindChain(hwnd) : nullptr;
        if (chain == nullptr)
            return;
        chain->active.store(false, std::memory_order_release);
        if (!IsWindow(hwnd))
        {
            // The window is gone (the game recreated it): nothing to restore, the
            // slot is free for the next one.
            chain->hwnd.store(nullptr, std::memory_order_release);
            chain->next.store(nullptr);
            Log("HOST_INPUT uninstalled (window destroyed)");
            return;
        }
        const WNDPROC head = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
        if (head == &HostWndProc)
        {
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(chain->next.load()));
            chain->hwnd.store(nullptr, std::memory_order_release);
            chain->next.store(nullptr);
            Log("HOST_INPUT uninstalled (window procedure restored)");
        }
        else
        {
            // Someone chained after us: restoring would cut them off. Stay in
            // the chain as a pass-through (the module is pinned).
            Log("HOST_INPUT uninstalled (left as pass-through, not the head)");
        }
    }

    void BeginFrame()
    {
        static std::vector<r::Event> events; // render thread only; keeps its capacity
        events.clear();
        {
            const std::lock_guard<std::mutex> lock(g_queueMutex);
            events.swap(g_queue);
        }
        g_keys.BeginFrame();
        for (const r::Event& e : events)
            g_keys.Apply(e);
        if (g_keys.focused)
            g_keys.ReleaseStale(GetTickCount(), [](unsigned vk) { return (g_getAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0; });

        g_textCount = 0;
        for (std::size_t i = 0; i < g_keys.charCount; ++i)
            g_text[g_textCount++] = static_cast<wchar_t>(g_keys.chars[i]);
        g_text[g_textCount] = L'\0';

        const HWND hwnd = g_hwnd.load();
        g_mouseValid = false;
        RECT client{};
        POINT pt{};
        if (hwnd != nullptr && GetClientRect(hwnd, &client) && client.right > 0 && client.bottom > 0 && g_getCursorPos(&pt) &&
            ScreenToClient(hwnd, &pt))
        {
            g_mouseX = pt.x;
            g_mouseY = pt.y;
            g_clientW = client.right;
            g_clientH = client.bottom;
            g_mouseValid = true;
        }

        const bool mouseBefore = g_block.mouse;
        g_block.BeginFrame();
        PublishBlock(mouseBefore);
    }

    bool KeyDown(unsigned vk) { return g_keys.KeyDown(vk); }
    bool KeyPressed(unsigned vk) { return g_keys.KeyPressed(vk); }
    bool KeyReleased(unsigned vk) { return g_keys.KeyReleased(vk); }
    bool MouseDown(int button) { return g_keys.MouseDown(button); }
    bool MouseReleased(int button) { return g_keys.MouseReleased(button); }

    void BlockNextFrame(bool mouse, bool keyboard)
    {
        const bool mouseBefore = g_block.mouse;
        g_block.Request(mouse, keyboard);
        PublishBlock(mouseBefore);
    }

    bool MousePosition(int& x, int& y)
    {
        x = g_mouseX;
        y = g_mouseY;
        return g_mouseValid;
    }

    const wchar_t* TextInput(unsigned& count)
    {
        count = g_textCount;
        return g_text;
    }

    float MouseWheel()
    {
        return r::WheelNotches(g_keys.wheel);
    }

    // For host_input_imgui.cpp.
    namespace detail
    {
        const r::Keys& Keys() { return g_keys; }
        void ClientSize(int& w, int& h)
        {
            w = g_clientW;
            h = g_clientH;
        }
    }
}
