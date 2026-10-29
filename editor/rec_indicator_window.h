#pragma once

// The recording indicator's own window (src/rec_indicator.h, mode hidden).
//
// The recording notices drawn with Direct2D in a small borderless, transparent,
// topmost, click-through window over the bottom left of the game window. The
// window has SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE): the player sees
// it, screen and window capture (Desktop Duplication, Windows.Graphics.Capture,
// BitBlt) leave it out. It is not in the game's swap chain either, so a hook on
// the game's present (OBS Game Capture) never sees it.
//
// One window, on its own thread (it pumps the window's messages). The editor
// submits the rows each frame (Submit); the thread follows the game window
// (its client area, whose height is 1080 layout units as in the editor) and
// hides the window when the game is minimized, not in the foreground, or the
// editor stops submitting. Stop ends the thread. The thread holds a reference
// on its module and leaves through FreeLibraryAndExitThread, so a core
// unloaded under the loader lock waits for it only a bounded time and is never
// unmapped under it.
//
// tools/rec_indicator/capture_test.cpp drives the same window to prove that
// capture leaves it out.

#include "rec_indicator.h"

#include <Windows.h>
#include <d2d1.h>
#include <dwrite_3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace rec_window
{
    struct Row
    {
        std::wstring label;      // drawn as given (the editor's capitals)
        std::wstring value;      // the clock, or empty
        std::uint32_t dot = 0;   // 0xRRGGBB
        bool pulse = false;      // the dot breathes (the editor's 1.2 s pulse)
        bool spinner = false;    // the value is a turning | / - \ the window draws
        float y = 0.0f;          // the row's top, layout units
    };

    struct Frame
    {
        HWND game = nullptr;
        float x = 40.0f; // the rows' left, layout units
        float uiScale = 1.0f; // the editor's interface scale (src/ui_scale.h Effective)
        std::vector<Row> rows;
    };

    struct Options
    {
        bool excludeFromCapture = true; // false: WDA_NONE (the capture test's control run)
        bool requireForeground = true;  // the game's process must own the foreground window
        std::wstring fontFile;          // the title font (Redemption.otf); empty or missing: Segoe UI
    };

    // A frame older than this hides the window (the editor stopped drawing).
    constexpr ULONGLONG kStaleMs = 500;
    constexpr DWORD kTickMs = 33;

    namespace detail
    {
        template <class T>
        void Release(T*& p)
        {
            if (p)
                p->Release();
            p = nullptr;
        }

        inline LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            switch (message)
            {
            case WM_NCHITTEST: return HTTRANSPARENT;
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
            default: return DefWindowProcW(hwnd, message, wParam, lParam);
            }
        }

        inline D2D1_COLOR_F Color(std::uint32_t rgb, float alpha)
        {
            return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                static_cast<float>(rgb & 0xFF) / 255.0f, alpha);
        }

        // Everything the thread owns; shared with Window until both let go.
        struct Shared
        {
            HANDLE stop = nullptr;
            HANDLE ready = nullptr;
            HANDLE done = nullptr;
            Options options;
            std::mutex lock;
            Frame frame;
            ULONGLONG frameAt = 0;
            // Set by the thread.
            volatile LONG running = 0;
            volatile LONG shown = 0;
            std::string failure;
            RECT shownRect{};

            ~Shared()
            {
                for (HANDLE h : {stop, ready, done})
                    if (h)
                        CloseHandle(h);
            }
        };

        class Painter
        {
        public:
            bool Init(const Options& options, std::string& why)
            {
                if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_d2d)))
                    return Fail(why, "d2d_factory");
                if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&m_dwrite))))
                    return Fail(why, "dwrite_factory");
                const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
                if (FAILED(m_d2d->CreateDCRenderTarget(&props, &m_target)))
                    return Fail(why, "dc_render_target");
                m_target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // no ClearType on a transparent surface
                if (FAILED(m_target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &m_brush)))
                    return Fail(why, "brush");
                m_memory = CreateCompatibleDC(nullptr);
                if (!m_memory)
                    return Fail(why, "memory_dc");
                LoadFont(options.fontFile);
                return true;
            }

            ~Painter()
            {
                Release(m_format);
                Release(m_brush);
                Release(m_target);
                Release(m_collection);
                Release(m_dwrite);
                Release(m_d2d);
                if (m_memory && m_old)
                    SelectObject(m_memory, m_old);
                if (m_bitmap)
                    DeleteObject(m_bitmap);
                if (m_memory)
                    DeleteDC(m_memory);
            }

            // The rows at `scale` into the window's bitmap; the box in layout
            // units comes back in x0..y1. False when nothing could be drawn.
            bool Paint(const Frame& frame, float scale, ULONGLONG now, float& x0, float& y0, int& width, int& height)
            {
                if (frame.rows.empty() || scale <= 0.0f || !Format(scale))
                    return false;
                struct Laid
                {
                    IDWriteTextLayout* label = nullptr;
                    IDWriteTextLayout* value = nullptr;
                    float labelW = 0.0f, valueW = 0.0f, width = 0.0f;
                };
                std::vector<Laid> laid(frame.rows.size());
                static const wchar_t* const kSpin[] = {L"|", L"/", L"-", L"\\"};
                float right = 0.0f;
                y0 = frame.rows[0].y;
                float y1 = y0;
                for (std::size_t n = 0; n < frame.rows.size(); ++n)
                {
                    const Row& row = frame.rows[n];
                    const std::wstring value = row.spinner ? kSpin[(now / 150) % 4] : row.value;
                    laid[n].label = Layout(row.label, laid[n].labelW);
                    if (!value.empty())
                        laid[n].value = Layout(value, laid[n].valueW);
                    laid[n].width = replay::recindicator::RowWidth(laid[n].labelW / scale, laid[n].valueW / scale);
                    right = std::max(right, laid[n].width);
                    y0 = std::min(y0, row.y);
                    y1 = std::max(y1, row.y + 40.0f);
                }
                x0 = frame.x;
                width = static_cast<int>(std::ceil(right * scale)) + 2;
                height = static_cast<int>(std::ceil((y1 - y0) * scale)) + 2;
                bool ok = Bitmap(width, height);
                if (ok)
                {
                    const RECT bounds{0, 0, width, height};
                    ok = SUCCEEDED(m_target->BindDC(m_memory, &bounds));
                }
                if (ok)
                {
                    m_target->BeginDraw();
                    m_target->Clear(D2D1::ColorF(0, 0, 0, 0));
                    for (std::size_t n = 0; n < frame.rows.size(); ++n)
                        DrawRow(frame.rows[n], laid[n].label, laid[n].value, laid[n].valueW, laid[n].width, scale, frame.rows[n].y - y0, now);
                    ok = SUCCEEDED(m_target->EndDraw());
                }
                for (Laid& l : laid)
                {
                    Release(l.label);
                    Release(l.value);
                }
                return ok;
            }

            HDC Memory() const { return m_memory; }

        private:
            static bool Fail(std::string& why, const char* what)
            {
                why = what;
                return false;
            }

            void LoadFont(const std::wstring& file)
            {
                // The editor's title font, through a font set (Windows 10 1703 and
                // newer; the hidden mode needs 2004 anyway).
                IDWriteFactory5* factory5 = nullptr;
                if (!file.empty() && GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES &&
                    SUCCEEDED(m_dwrite->QueryInterface(__uuidof(IDWriteFactory5), reinterpret_cast<void**>(&factory5))))
                {
                    IDWriteFontSetBuilder1* builder = nullptr;
                    IDWriteFontFile* fontFile = nullptr;
                    IDWriteFontSet* set = nullptr;
                    IDWriteFontCollection1* collection = nullptr;
                    if (SUCCEEDED(factory5->CreateFontSetBuilder(&builder)) && SUCCEEDED(factory5->CreateFontFileReference(file.c_str(), nullptr, &fontFile)) &&
                        SUCCEEDED(builder->AddFontFile(fontFile)) && SUCCEEDED(builder->CreateFontSet(&set)) &&
                        SUCCEEDED(factory5->CreateFontCollectionFromFontSet(set, &collection)) && collection->GetFontFamilyCount() > 0)
                    {
                        IDWriteFontFamily* family = nullptr;
                        IDWriteLocalizedStrings* names = nullptr;
                        wchar_t name[128]{};
                        if (SUCCEEDED(collection->GetFontFamily(0, &family)) && SUCCEEDED(family->GetFamilyNames(&names)) &&
                            SUCCEEDED(names->GetString(0, name, 128)))
                        {
                            m_family = name;
                            m_collection = collection;
                            collection = nullptr;
                            m_capPerEm = CapPerEm(family);
                        }
                        Release(names);
                        Release(family);
                    }
                    Release(collection);
                    Release(set);
                    Release(fontFile);
                    Release(builder);
                    Release(factory5);
                }
                if (!m_collection)
                {
                    m_family = L"Segoe UI";
                    m_weight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
                    IDWriteFontCollection* system = nullptr;
                    IDWriteFontFamily* family = nullptr;
                    UINT32 index = 0;
                    BOOL exists = FALSE;
                    if (SUCCEEDED(m_dwrite->GetSystemFontCollection(&system)) && SUCCEEDED(system->FindFamilyName(m_family.c_str(), &index, &exists)) &&
                        exists && SUCCEEDED(system->GetFontFamily(index, &family)))
                        m_capPerEm = CapPerEm(family);
                    Release(family);
                    Release(system);
                }
            }

            float CapPerEm(IDWriteFontFamily* family) const
            {
                IDWriteFont* font = nullptr;
                float cap = 0.7f;
                if (SUCCEEDED(family->GetFirstMatchingFont(m_weight, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)))
                {
                    DWRITE_FONT_METRICS metrics{};
                    font->GetMetrics(&metrics);
                    if (metrics.designUnitsPerEm && metrics.capHeight)
                        cap = static_cast<float>(metrics.capHeight) / metrics.designUnitsPerEm;
                    font->Release();
                }
                return cap;
            }

            // The editor's 22-unit notice label: the title font at 1.18 times the
            // size, its capitals 0.62 / 1.147 of that (display_font.h RedemptionFit).
            static float CapUnits() { return 22.0f * 1.18f * (744.0f / 1200.0f / 1.147f); }

            bool Format(float scale)
            {
                if (m_format && m_formatScale == scale)
                    return true;
                Release(m_format);
                const float cap = std::max(7.2f, CapUnits() * scale); // display_font.h kMinCapPx
                m_capPx = cap;
                const float em = cap / std::max(0.3f, m_capPerEm);
                if (FAILED(m_dwrite->CreateTextFormat(m_family.c_str(), m_collection, m_weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                        em, L"", &m_format)))
                    return false;
                m_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                m_formatScale = scale;
                return true;
            }

            IDWriteTextLayout* Layout(const std::wstring& text, float& width)
            {
                IDWriteTextLayout* layout = nullptr;
                width = 0.0f;
                if (FAILED(m_dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), m_format, 8192.0f, 1024.0f, &layout)))
                    return nullptr;
                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(layout->GetMetrics(&metrics)))
                    width = metrics.widthIncludingTrailingWhitespace;
                return layout;
            }

            // The text with its capitals centred on `middle` (pixels).
            void DrawLabel(IDWriteTextLayout* layout, float x, float middle, std::uint32_t rgb)
            {
                DWRITE_LINE_METRICS line{};
                UINT32 lines = 0;
                if (!layout || FAILED(layout->GetLineMetrics(&line, 1, &lines)) || lines == 0)
                    return;
                m_brush->SetColor(Color(rgb, 1.0f));
                const float baseline = middle + m_capPx * 0.5f;
                m_target->DrawTextLayout(D2D1::Point2F(std::round(x), std::round(baseline - line.baseline)), layout, m_brush);
            }

            void DrawRow(const Row& row, IDWriteTextLayout* label, IDWriteTextLayout* value, float valueW, float width, float scale, float top, ULONGLONG now)
            {
                const auto px = [scale](float units) { return units * scale; };
                // The panel (the editor's flat ink panel: rgba 10 9 8 200).
                m_brush->SetColor(Color(0x0A0908, 200.0f / 255.0f));
                m_target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.0f, px(top), px(width), px(top + 40.0f)), px(4.0f), px(4.0f)), m_brush);
                float alpha = 1.0f;
                if (row.pulse)
                    alpha = 0.55f + 0.45f * std::sin(static_cast<float>(now % 1200) / 1200.0f * 6.2832f);
                m_brush->SetColor(Color(row.dot, alpha));
                m_target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px(22.0f), px(top + 20.0f)), px(7.0f), px(7.0f)), m_brush);
                DrawLabel(label, px(40.0f), px(top + 20.0f), 0xECE6DA);                         // the editor's kText
                if (value)
                    DrawLabel(value, px(width - 14.0f) - valueW, px(top + 20.0f), 0x968F83);   // kMuted
            }

            bool Bitmap(int width, int height)
            {
                if (m_bitmap && width == m_width && height == m_height)
                    return true;
                if (m_old)
                    SelectObject(m_memory, m_old);
                m_old = nullptr;
                if (m_bitmap)
                    DeleteObject(m_bitmap);
                m_bitmap = nullptr;
                BITMAPINFO info{};
                info.bmiHeader.biSize = sizeof(info.bmiHeader);
                info.bmiHeader.biWidth = width;
                info.bmiHeader.biHeight = -height; // top-down
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                info.bmiHeader.biCompression = BI_RGB;
                void* bits = nullptr;
                m_bitmap = CreateDIBSection(m_memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
                if (!m_bitmap)
                    return false;
                m_old = SelectObject(m_memory, m_bitmap);
                m_width = width;
                m_height = height;
                return true;
            }

            ID2D1Factory* m_d2d = nullptr;
            IDWriteFactory* m_dwrite = nullptr;
            ID2D1DCRenderTarget* m_target = nullptr;
            ID2D1SolidColorBrush* m_brush = nullptr;
            IDWriteFontCollection1* m_collection = nullptr;
            IDWriteTextFormat* m_format = nullptr;
            std::wstring m_family;
            DWRITE_FONT_WEIGHT m_weight = DWRITE_FONT_WEIGHT_NORMAL;
            float m_capPerEm = 0.7f;
            float m_capPx = 14.0f;
            float m_formatScale = 0.0f;
            HDC m_memory = nullptr;
            HBITMAP m_bitmap = nullptr;
            HGDIOBJ m_old = nullptr;
            int m_width = 0, m_height = 0;
        };

        inline void Run(Shared& shared, HMODULE module)
        {
            static const wchar_t kClass[] = L"RedemptionEditorRecIndicator";
            WNDCLASSEXW cls{};
            cls.cbSize = sizeof(cls);
            cls.lpfnWndProc = WindowProc;
            cls.hInstance = module;
            cls.lpszClassName = kClass;
            const bool registered = RegisterClassExW(&cls) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
            HWND hwnd = registered ? CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass,
                                         L"Redemption Editor recording indicator", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, module, nullptr)
                                   : nullptr;
            Painter painter;
            std::string why;
            if (!hwnd)
                why = "create_window,error=" + std::to_string(GetLastError());
            else
            {
                const DWORD affinity = shared.options.excludeFromCapture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
                DWORD got = 0xFFFFFFFF;
                if (!SetWindowDisplayAffinity(hwnd, affinity))
                    why = "affinity,error=" + std::to_string(GetLastError());
                else if (!GetWindowDisplayAffinity(hwnd, &got) || got != affinity)
                    why = "affinity,read=" + std::to_string(got);
                else if (!painter.Init(shared.options, why))
                    why = "draw," + why;
            }
            {
                std::lock_guard<std::mutex> guard(shared.lock);
                shared.failure = why;
            }
            InterlockedExchange(&shared.running, why.empty() ? 1 : 0);
            SetEvent(shared.ready);

            bool visible = false;
            ULONGLONG raisedAt = 0;
            POINT shownAt{};
            SIZE shownSize{};
            while (why.empty())
            {
                const DWORD wait = MsgWaitForMultipleObjects(1, &shared.stop, FALSE, kTickMs, QS_ALLINPUT);
                if (wait == WAIT_OBJECT_0)
                    break;
                MSG message;
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                Frame frame;
                ULONGLONG at = 0;
                {
                    std::lock_guard<std::mutex> guard(shared.lock);
                    frame = shared.frame;
                    at = shared.frameAt;
                }
                const ULONGLONG now = GetTickCount64();
                HWND game = frame.game;
                bool show = !frame.rows.empty() && now - at < kStaleMs && game && IsWindow(game) && IsWindowVisible(game) && !IsIconic(game);
                if (show && shared.options.requireForeground)
                {
                    DWORD pid = 0;
                    const HWND foreground = GetForegroundWindow();
                    if (foreground)
                        GetWindowThreadProcessId(foreground, &pid);
                    show = pid == GetCurrentProcessId();
                }
                RECT client{};
                POINT origin{0, 0};
                if (show)
                    show = GetClientRect(game, &client) && ClientToScreen(game, &origin) && client.bottom >= 100;
                float x0 = 0.0f, y0 = 0.0f;
                int width = 0, height = 0;
                const float scale = replay::recindicator::Scale(client.bottom) * frame.uiScale;
                if (show)
                    show = painter.Paint(frame, scale, now, x0, y0, width, height);
                if (!show)
                {
                    if (visible)
                        ShowWindow(hwnd, SW_HIDE);
                    visible = false;
                    InterlockedExchange(&shared.shown, 0);
                    continue;
                }
                POINT position{origin.x + static_cast<LONG>(std::floor(x0 * scale)), origin.y + static_cast<LONG>(std::floor(y0 * scale))};
                SIZE size{width, height};
                POINT source{0, 0};
                BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                UpdateLayeredWindow(hwnd, nullptr, &position, &size, painter.Memory(), &source, 0, &blend, ULW_ALPHA);
                // Above the game: shown without activation, raised again now and
                // then (a game window that is topmost too comes back on top when
                // it is activated).
                if (!visible || now - raisedAt > 500 || position.x != shownAt.x || position.y != shownAt.y || size.cx != shownSize.cx ||
                    size.cy != shownSize.cy)
                {
                    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                    raisedAt = now;
                }
                visible = true;
                shownAt = position;
                shownSize = size;
                {
                    std::lock_guard<std::mutex> guard(shared.lock);
                    shared.shownRect = RECT{position.x, position.y, position.x + size.cx, position.y + size.cy};
                }
                InterlockedExchange(&shared.shown, 1);
            }
            InterlockedExchange(&shared.shown, 0);
            InterlockedExchange(&shared.running, 0);
            if (hwnd)
                DestroyWindow(hwnd);
            UnregisterClassW(kClass, module);
        }

        struct ThreadStart
        {
            std::shared_ptr<Shared> shared;
            HMODULE module = nullptr;
        };

        inline DWORD WINAPI ThreadMain(void* param)
        {
            ThreadStart* start = static_cast<ThreadStart*>(param);
            const HMODULE module = start->module;
            {
                std::shared_ptr<Shared> shared = std::move(start->shared);
                delete start;
                Run(*shared, module);
                SetEvent(shared->done);
            }
            FreeLibraryAndExitThread(module, 0);
        }
    }

    class Window
    {
    public:
        // The thread and its window. False, with `why`, when the window, its
        // display affinity or its drawing could not be made; Failure() then
        // keeps the reason until the next Start.
        bool Start(const Options& options, std::string* why = nullptr)
        {
            if (m_shared)
                return Running();
            auto shared = std::make_shared<detail::Shared>();
            shared->options = options;
            shared->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            shared->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            shared->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            HMODULE module = nullptr;
            if (!shared->stop || !shared->ready || !shared->done ||
                !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&detail::ThreadMain), &module))
                return Failed("thread_setup", why);
            auto* start = new detail::ThreadStart{shared, module};
            HANDLE thread = CreateThread(nullptr, 0, detail::ThreadMain, start, 0, nullptr);
            if (!thread)
            {
                delete start;
                FreeLibrary(module);
                return Failed("create_thread", why);
            }
            CloseHandle(thread);
            m_shared = shared;
            if (WaitForSingleObject(shared->ready, 3000) != WAIT_OBJECT_0)
            {
                Stop();
                return Failed("thread_start_timeout", why);
            }
            if (!Running())
            {
                std::string failure;
                {
                    std::lock_guard<std::mutex> guard(shared->lock);
                    failure = shared->failure;
                }
                Stop();
                return Failed(failure.c_str(), why);
            }
            m_failure.clear();
            return true;
        }

        // The latest rows (the window draws the last one it got).
        void Submit(const Frame& frame)
        {
            if (!m_shared)
                return;
            std::lock_guard<std::mutex> guard(m_shared->lock);
            m_shared->frame = frame;
            m_shared->frameAt = GetTickCount64();
        }

        // Ends the thread (bounded wait: under the loader lock the thread
        // cannot finish until the lock is free; it holds its module meanwhile).
        void Stop()
        {
            if (!m_shared)
                return;
            SetEvent(m_shared->stop);
            WaitForSingleObject(m_shared->done, 2000);
            m_shared.reset();
        }

        bool Started() const { return m_shared != nullptr; }
        bool Running() const { return m_shared && m_shared->running != 0; }
        // The window is on screen now, and where (screen pixels).
        bool Shown(RECT* rect = nullptr) const
        {
            if (!m_shared || m_shared->shown == 0)
                return false;
            if (rect)
            {
                std::lock_guard<std::mutex> guard(m_shared->lock);
                *rect = m_shared->shownRect;
            }
            return true;
        }
        const std::string& Failure() const { return m_failure; }

    private:
        bool Failed(const char* what, std::string* why)
        {
            m_failure = what;
            if (why)
                *why = what;
            return false;
        }

        std::shared_ptr<detail::Shared> m_shared;
        std::string m_failure;
    };
}
