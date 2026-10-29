// The UI harness. Renders every fixture of the editor (ui_harness_fixtures.inl) at
// every resolution, offline (ImGui on a D3D11 WARP device, the bundled
// fonts), records each text's pixels and the boxes under it, checks them
// (src/ui_layout_check.h) and writes, under --out:
//   png\<fixture>@<WxH>.png       every frame
//   marked\<fixture>@<WxH>.png    the frames with findings, the rects outlined
//   report.json                   every finding (and with --dump, every text)
//   index.html                    the contact sheet (thumbnails, findings, viewer)
//   findings.txt                  the findings grouped, worst first
//   findings-table.md             the same table in Markdown
// Exit code: 0 clean, 1 an error-level finding not allowed by design, 2 the
// harness could not run.
//
// ReplayEditorUiTests.exe [--out <dir>] [--filter <text>] [--res 1920x1080,...]
//     [--frames N] [--min-cap-error px] [--min-cap-warning px] [--no-png] [--dump]
//     [--flat] (without the painted art, as when RedemptionEditor\ui is missing)
//     [--lang <code>] (the editor's language: Language=<code> in the output
//     folder's RedemptionEditor.ini; pseudo is the pseudo language)
//     [--hide-face <file>] (a Windows font taken as missing, as on a PC without
//     it: the CJK fallbacks; repeatable)
//     [--scale 80,100,120] (the interface scale in percent, InterfaceScale in
//     the ini, src/ui_scale.h: every resolution at each; a scale other than
//     100 is in the frame's name, <fixture>@<WxH>@<pct>.png)

#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#include <imgui.h>

#include "ui_harness.h"
#include "ui_harness_runtime.h"
#include "fake_runtime.h"
#include "ui_probe.h"
#include "ui_layout_check.h"

#include <Windows.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace lay = replay::editor::uilayout;

// ReShade's log (reshade::log::message finds the module exporting these):
// the harness is that module, its log a file under --out.
namespace
{
    FILE* g_log = nullptr;
}

extern "C" __declspec(dllexport) bool ReShadeRegisterAddon(HMODULE, std::uint32_t) { return true; }
extern "C" __declspec(dllexport) void ReShadeUnregisterAddon(HMODULE) {}
extern "C" __declspec(dllexport) void ReShadeLogMessage(HMODULE, int level, const char* message)
{
    if (g_log)
        std::fprintf(g_log, "%d %s\n", level, message ? message : "");
}

namespace
{
    // ------------------------------------------------------------------
    // The recorder: what the editor draws in the recorded frame.
    // ------------------------------------------------------------------
    struct Recorder final : ui_probe::Sink
    {
        lay::Frame frame;
        bool on = false;
        int order = 0;
        std::vector<std::string> scopes;
        int group = -1;
        int groupDepth = 0;
        int groups = 0;
        // UI_PROBE_BOX("panel"): the panel of the texts after it, until its
        // scope ends or an "endpanel".
        std::size_t panel = lay::kNone;
        std::size_t panelDepth = 0;

        void Reset(float width, float height)
        {
            frame = lay::Frame{};
            frame.width = width;
            frame.height = height;
            order = 0;
            scopes.clear();
            group = -1;
            groupDepth = 0;
            panel = lay::kNone;
            panelDepth = 0;
        }

        std::string Scope() const
        {
            std::string text;
            for (const std::string& s : scopes)
                text += (text.empty() ? "" : "/") + s;
            return text.empty() ? "Screen" : text;
        }

        lay::Rect ClipOf(ImDrawList* draw) const
        {
            if (!draw)
                return lay::Rect{0.0f, 0.0f, frame.width, frame.height};
            const ImVec4 c = draw->_CmdHeader.ClipRect;
            return lay::Rect{c.x, c.y, c.z, c.w};
        }

        void AddText(ImDrawList* draw, std::string text, lay::Rect ink, float px, float capPx, const char* font)
        {
            if (!on)
                return;
            lay::TextItem t;
            t.text = std::move(text);
            t.ink = ink;
            t.clip = ClipOf(draw);
            t.px = px;
            t.capPx = capPx;
            t.group = group;
            t.order = order++;
            t.scope = Scope();
            t.font = font;
            t.keyBar = t.scope.find("KeyBar") != std::string::npos;
            t.panel = panel;
            frame.texts.push_back(std::move(t));
        }

        void AddBox(const char* kind, const std::string& id, ImVec2 min, ImVec2 max, bool container = true)
        {
            if (!on)
                return;
            lay::Box b;
            b.kind = kind;
            b.id = id;
            b.rect = lay::Rect{std::min(min.x, max.x), std::min(min.y, max.y), std::max(min.x, max.x), std::max(min.y, max.y)};
            b.order = order++;
            b.scope = Scope();
            b.container = container;
            frame.boxes.push_back(b);
        }

        void Text(ImDrawList* draw, const char* begin, const char* end, ImVec2 inkMin, ImVec2 inkMax, float px, float capPx,
            const char* font) override
        {
            const lay::Rect ink = inkMin.x <= inkMax.x ? lay::Rect{inkMin.x, inkMin.y, inkMax.x, inkMax.y} : lay::Rect{};
            AddText(draw, std::string(begin, end ? end : begin + std::strlen(begin)), ink, px, capPx, font);
        }

        void Box(const char* kind, ImVec2 min, ImVec2 max) override
        {
            if (on && std::strcmp(kind, "keybar") == 0)
                frame.keyBar = lay::Rect{min.x, min.y, max.x, max.y};
            if (std::strcmp(kind, "endpanel") == 0)
            {
                panel = lay::kNone;
                return;
            }
            if (std::strcmp(kind, "panelinner") == 0) // the painted panel's body (ui_art.h)
            {
                if (on && panel != lay::kNone)
                    frame.boxes[panel].inner = lay::Rect{min.x, min.y, max.x, max.y};
                return;
            }
            // A loading cue holds no text (the cue check, ui_layout_check.h, places it).
            AddBox(kind, kind, min, max, std::strcmp(kind, "cue") != 0);
            if (on && std::strcmp(kind, "panel") == 0)
            {
                panel = frame.boxes.size() - 1;
                panelDepth = scopes.size();
            }
        }

        void PushScope(const char* name) override { scopes.push_back(name); }
        void PopScope() override
        {
            if (!scopes.empty())
                scopes.pop_back();
            if (scopes.size() < panelDepth)
            {
                panel = lay::kNone;
                panelDepth = 0;
            }
        }
        void BeginGroup() override
        {
            if (groupDepth++ == 0)
                group = groups++;
        }
        void EndGroup() override
        {
            if (--groupDepth <= 0)
            {
                groupDepth = 0;
                group = -1;
            }
        }
        ImU64 CreateTexture(const void* rgba, int width, int height) override { return UiHarnessCreateTexture(rgba, width, height); }
        ImU64 CreateTextureMips(const void* const* levels, int width, int height, int count) override
        {
            // --flat: as if RedemptionEditor\ui were missing (the flat look).
            return flatArt ? 0 : UiHarnessCreateTextureMips(levels, width, height, count);
        }
        bool flatArt = false;
    };

    Recorder g_rec;

    // ------------------------------------------------------------------
    // The function table the editor draws through, with the recorder in
    // the calls that draw text or the boxes under it.
    // ------------------------------------------------------------------
    imgui_function_table g_real{};
    imgui_function_table g_wrapped{};

    ImU32 Alpha(ImU32 color) { return (color >> IM_COL32_A_SHIFT) & 0xFF; }

    // An ImGui font text: its line box (glyph ink about 10 % in from the top and bottom).
    void RecordImGuiText(ImDrawList* draw, ImFont* font, float size, const ImVec2& pos, const char* begin, const char* end, float wrap)
    {
        if (!g_rec.on || !begin)
            return;
        if (!end)
            end = begin + std::strlen(begin);
        if (size > 0.0f)
            g_real.PushFont(font, size);
        const float px = g_real.GetFontSize();
        const ImVec2 box = g_real.CalcTextSize(begin, end, false, wrap > 0.0f ? wrap : -1.0f);
        if (size > 0.0f)
            g_real.PopFont();
        const lay::Rect ink{pos.x, pos.y + px * 0.1f, pos.x + box.x, pos.y + std::max(px * 0.2f, box.y - px * 0.1f)};
        g_rec.AddText(draw, std::string(begin, end), ink, px, px * 0.7f, "imgui");
    }

    void WAddText(ImDrawList* draw, const ImVec2& pos, ImU32 col, const char* begin, const char* end)
    {
        g_real.ImDrawList_AddText(draw, pos, col, begin, end);
        if (Alpha(col) >= 40)
            RecordImGuiText(draw, nullptr, 0.0f, pos, begin, end, 0.0f);
    }

    void WAddText2(ImDrawList* draw, ImFont* font, float size, const ImVec2& pos, ImU32 col, const char* begin, const char* end, float wrap,
        const ImVec4* clip)
    {
        g_real.ImDrawList_AddText2(draw, font, size, pos, col, begin, end, wrap, clip);
        if (Alpha(col) >= 40)
            RecordImGuiText(draw, font, size, pos, begin, end, wrap);
    }

    void WAddRectFilled(ImDrawList* draw, const ImVec2& a, const ImVec2& b, ImU32 col, float rounding, ImDrawFlags flags)
    {
        g_real.ImDrawList_AddRectFilled(draw, a, b, col, rounding, flags);
        // A panel: opaque enough to hold text, not a hairline or a bar.
        if (Alpha(col) >= 150 && std::fabs(b.x - a.x) >= 8.0f && std::fabs(b.y - a.y) >= 8.0f)
            g_rec.AddBox("fill", "", a, b);
    }

    void WAddRect(ImDrawList* draw, const ImVec2& a, const ImVec2& b, ImU32 col, float rounding, ImDrawFlags flags, float thickness)
    {
        g_real.ImDrawList_AddRect(draw, a, b, col, rounding, flags, thickness);
        if (Alpha(col) >= 40 && std::fabs(b.x - a.x) >= 6.0f && std::fabs(b.y - a.y) >= 6.0f)
            g_rec.AddBox("frame", "", a, b);
    }

    void WAddConvexPolyFilled(ImDrawList* draw, const ImVec2* points, int count, ImU32 col)
    {
        g_real.ImDrawList_AddConvexPolyFilled(draw, points, count, col);
        // The red brush behind a title or a selection (Brush: a band with a ragged right edge).
        if (Alpha(col) >= 100 && count >= 6)
        {
            ImVec2 min(1.0e9f, 1.0e9f), max(-1.0e9f, -1.0e9f);
            for (int n = 0; n < count; ++n)
            {
                min = ImVec2(std::min(min.x, points[n].x), std::min(min.y, points[n].y));
                max = ImVec2(std::max(max.x, points[n].x), std::max(max.y, points[n].y));
            }
            g_rec.AddBox("brush", "", min, max);
        }
    }

    // Interaction areas that are not drawn boxes (text over or beside them is by design).
    bool InteractionOnly(const char* id)
    {
        static const char* const ids[] = {"##timeline", "##fxlane", "##projectbar", "##projectblocks", "##sort"};
        for (const char* known : ids)
            if (std::strcmp(id, known) == 0)
                return true;
        return false;
    }

    bool WInvisibleButton(const char* id, const ImVec2& size, ImGuiButtonFlags flags)
    {
        const bool pressed = g_real.InvisibleButton(id, size, flags);
        g_rec.AddBox("item", id, g_real.GetItemRectMin(), g_real.GetItemRectMax(), !InteractionOnly(id));
        return pressed;
    }

    bool WInputText(const char* label, char* buf, size_t size, ImGuiInputTextFlags flags, ImGuiInputTextCallback callback, void* user)
    {
        const bool entered = g_real.InputText(label, buf, size, flags, callback, user);
        const ImVec2 min = g_real.GetItemRectMin(), max = g_real.GetItemRectMax();
        g_rec.AddBox("input", label, min, max);
        if (g_rec.on && buf && buf[0])
        {
            const ImVec2 pad = g_real.GetStyle().FramePadding;
            RecordImGuiText(nullptr, nullptr, 0.0f, ImVec2(min.x + pad.x, min.y + pad.y), buf, nullptr, 0.0f);
            // The box scrolls its text: what shows is inside it.
            if (!g_rec.frame.texts.empty())
                g_rec.frame.texts.back().clip = lay::Rect{min.x, min.y, max.x, max.y};
        }
        return entered;
    }

    // ------------------------------------------------------------------
    // Findings allowed by design (the reason is the report's).
    // ------------------------------------------------------------------
    std::vector<lay::Allowance> Allowances()
    {
        return {
        };
    }

    // ------------------------------------------------------------------
    // Output helpers.
    // ------------------------------------------------------------------
    std::string Json(const std::string& text)
    {
        std::string out = "\"";
        for (const unsigned char c : text)
        {
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    char hex[8];
                    std::snprintf(hex, sizeof(hex), "\\u%04x", c);
                    out += hex;
                }
                else
                    out += static_cast<char>(c);
            }
        }
        return out + "\"";
    }

    std::string RectJson(const lay::Rect& r)
    {
        char text[96];
        std::snprintf(text, sizeof(text), "[%.1f,%.1f,%.1f,%.1f]", r.x0, r.y0, r.x1, r.y1);
        return text;
    }

    std::string RectText(const lay::Rect& r)
    {
        char text[96];
        std::snprintf(text, sizeof(text), "(%.0f,%.0f)-(%.0f,%.0f)", r.x0, r.y0, r.x1, r.y1);
        return text;
    }

    std::wstring Wide(const std::string& text)
    {
        std::wstring out;
        for (const char c : text)
            out += static_cast<wchar_t>(static_cast<unsigned char>(c));
        return out;
    }

    std::string FileStem(const std::string& fixture, int width, int height, int scale)
    {
        std::string stem;
        for (const char c : fixture)
            stem += (c == '/' || c == '\\' || c == ' ') ? '_' : c;
        char res[48];
        if (scale == 100)
            std::snprintf(res, sizeof(res), "@%dx%d", width, height);
        else
            std::snprintf(res, sizeof(res), "@%dx%d@%d", width, height, scale);
        return stem + res;
    }

    bool WriteFile(const std::string& path, const std::string& text)
    {
        FILE* file = nullptr;
        if (fopen_s(&file, path.c_str(), "wb") != 0 || !file)
            return false;
        std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
        return true;
    }

    // The rects of the findings over the picture (the marked PNG).
    void Outline(std::vector<std::uint8_t>& rgba, int width, int height, const lay::Rect& r, std::uint32_t color, int thickness)
    {
        const int x0 = std::clamp(static_cast<int>(std::floor(r.x0)) - 1, 0, width - 1);
        const int y0 = std::clamp(static_cast<int>(std::floor(r.y0)) - 1, 0, height - 1);
        const int x1 = std::clamp(static_cast<int>(std::ceil(r.x1)) + 1, 0, width - 1);
        const int y1 = std::clamp(static_cast<int>(std::ceil(r.y1)) + 1, 0, height - 1);
        const auto put = [&](int x, int y) {
            std::uint8_t* p = &rgba[(static_cast<std::size_t>(y) * width + x) * 4];
            p[0] = static_cast<std::uint8_t>(color >> 16);
            p[1] = static_cast<std::uint8_t>(color >> 8);
            p[2] = static_cast<std::uint8_t>(color);
            p[3] = 255;
        };
        for (int t = 0; t < thickness; ++t)
        {
            for (int x = x0; x <= x1; ++x)
            {
                put(x, std::min(y0 + t, height - 1));
                put(x, std::max(y1 - t, 0));
            }
            for (int y = y0; y <= y1; ++y)
            {
                put(std::min(x0 + t, width - 1), y);
                put(std::max(x1 - t, 0), y);
            }
        }
    }

    std::uint32_t CheckColor(lay::Check check)
    {
        switch (check)
        {
        case lay::Check::Viewport: return 0xFF2020;
        case lay::Check::Container: return 0xFF9000;
        case lay::Check::Overlap: return 0xFF20FF;
        case lay::Check::KeyBar: return 0xFFFF00;
        case lay::Check::MinHeight: return 0x20E0FF;
        case lay::Check::Crowded: return 0x60FF60;
        case lay::Check::EdgeBand: return 0xFF4080;
        case lay::Check::Cue: return 0x00FFA0;
        default: return 0x909090;
        }
    }

    struct Resolution
    {
        int width;
        int height;
        int scale = 100; // the interface scale, percent (--scale)
    };

    struct FrameResult
    {
        std::string fixture;
        Resolution res{};
        std::string png;
        std::string marked;
        lay::Frame frame;
        std::vector<lay::Finding> findings;
    };

    // A finding as the reports list it (one per fixture, check and text, all resolutions).
    struct Group
    {
        lay::Check check = lay::Check::Count;
        lay::Severity severity = lay::Severity::Info;
        std::string fixture;
        std::string scope;
        std::string text;
        std::string other;
        std::string box;
        std::string allowed;
        float worst = 0.0f;
        std::string worstRes;
        std::string worstPng;
        std::string worstRects;
        std::vector<std::string> resolutions;
        std::set<std::string> fixtures;
        std::vector<std::string> examples; // min_height: some of the texts
    };

    int CheckRank(lay::Check check)
    {
        switch (check)
        {
        case lay::Check::Viewport: return 0;
        case lay::Check::KeyBar: return 1;
        case lay::Check::Overlap: return 2;
        case lay::Check::Container: return 3;
        case lay::Check::MinHeight: return 4;
        case lay::Check::Crowded: return 6;
        case lay::Check::EdgeBand: return 3;
        default: return 7;
        }
    }

    std::string ResText(const Resolution& r)
    {
        char text[32];
        if (r.scale == 100)
            std::snprintf(text, sizeof(text), "%dx%d", r.width, r.height);
        else
            std::snprintf(text, sizeof(text), "%dx%d@%d%%", r.width, r.height, r.scale);
        return text;
    }

    std::string Shorten(const std::string& text, std::size_t most)
    {
        return text.size() <= most ? text : text.substr(0, most - 3) + "...";
    }

    // Markdown cell: no pipes, no line breaks.
    std::string Cell(std::string text)
    {
        for (char& c : text)
            if (c == '|' || c == '\n')
                c = '/';
        return text;
    }
}

int main(int argc, char** argv)
{
    std::string out = "out";
    std::string filter;
    std::vector<Resolution> resolutions = {{1280, 720}, {1366, 768}, {1920, 1080}, {1920, 1200}, {2560, 1080}, {2560, 1440}, {3840, 2160},
        {1280, 800}, {1024, 768}, {3440, 1440}};
    int frames = 4;
    bool png = true;
    bool dump = false;
    std::string language;
    std::vector<int> scales = {100};
    lay::Options options;
    for (int n = 1; n < argc; ++n)
    {
        const std::string arg = argv[n];
        const bool more = n + 1 < argc;
        if (arg == "--out" && more)
            out = argv[++n];
        else if (arg == "--filter" && more)
            filter = argv[++n];
        else if (arg == "--frames" && more)
            frames = std::max(2, std::atoi(argv[++n]));
        else if (arg == "--min-cap-error" && more)
            options.minCapError = static_cast<float>(std::atof(argv[++n]));
        else if (arg == "--min-cap-warning" && more)
            options.minCapWarning = static_cast<float>(std::atof(argv[++n]));
        else if (arg == "--no-png")
            png = false;
        else if (arg == "--dump")
            dump = true;
        else if (arg == "--flat")
            g_rec.flatArt = true;
        else if (arg == "--lang" && more)
            language = argv[++n];
        else if (arg == "--hide-face" && more)
            ui_harness::HideFace(argv[++n]);
        else if (arg == "--scale" && more)
        {
            scales.clear();
            std::string list = argv[++n];
            std::size_t at = 0;
            while (at < list.size())
            {
                const std::size_t comma = list.find(',', at);
                const int pct = std::atoi(list.substr(at, comma == std::string::npos ? std::string::npos : comma - at).c_str());
                if (pct > 0)
                    scales.push_back(pct);
                at = comma == std::string::npos ? list.size() : comma + 1;
            }
            if (scales.empty())
                scales.push_back(100);
        }
        else if (arg == "--res" && more)
        {
            resolutions.clear();
            std::string list = argv[++n];
            std::size_t at = 0;
            while (at < list.size())
            {
                const std::size_t comma = list.find(',', at);
                const std::string one = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                int w = 0, h = 0;
                if (std::sscanf(one.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0)
                    resolutions.push_back({w, h});
                at = comma == std::string::npos ? list.size() : comma + 1;
            }
        }
        else
        {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return 2;
        }
    }
    // Every resolution at every scale.
    {
        std::vector<Resolution> all;
        for (const int scale : scales)
            for (Resolution res : resolutions)
            {
                res.scale = scale;
                all.push_back(res);
            }
        resolutions = all;
    }
    CreateDirectoryA(out.c_str(), nullptr);
    CreateDirectoryA((out + "\\png").c_str(), nullptr);
    CreateDirectoryA((out + "\\marked").c_str(), nullptr);
    fopen_s(&g_log, (out + "\\harness.log").c_str(), "wb");
    // The editor's files (RedemptionEditor.ini, ReplayClips) are looked up
    // in the current folder: the output folder has none (the defaults).
    SetCurrentDirectoryA(out.c_str());
    // The language as a player sets it; without --lang no ini (the defaults: English).
    if (language.empty())
        DeleteFileA("RedemptionEditor.ini");
    else
        WriteFile("RedemptionEditor.ini", "[RedemptionEditor]\r\nLanguage=" + language + "\r\n");

    char error[256]{};
    if (!UiHarnessInit(error, sizeof(error)))
    {
        std::fprintf(stderr, "ui harness: %s\n", error);
        return 2;
    }
    g_real = *static_cast<const imgui_function_table*>(UiHarnessTable());
    g_wrapped = g_real;
    g_wrapped.ImDrawList_AddText = WAddText;
    g_wrapped.ImDrawList_AddText2 = WAddText2;
    g_wrapped.ImDrawList_AddRectFilled = WAddRectFilled;
    g_wrapped.ImDrawList_AddRect = WAddRect;
    g_wrapped.ImDrawList_AddConvexPolyFilled = WAddConvexPolyFilled;
    g_wrapped.InvisibleButton = WInvisibleButton;
    g_wrapped.InputText = WInputText;
    ui_probe::Current() = &g_rec;
    ui_harness::Begin(&g_wrapped);

    const std::vector<ui_harness::FixtureInfo> fixtures = ui_harness::Fixtures();
    const std::vector<lay::Allowance> allowances = Allowances();
    ui_harness::FakeRuntime runtime;
    std::vector<FrameResult> results;
    std::vector<std::uint8_t> pixels;
    double clock = 0.0;
    std::printf("ui harness: %zu fixtures x %zu resolutions\n", fixtures.size(), resolutions.size());
    for (std::size_t f = 0; f < fixtures.size(); ++f)
    {
        if (!filter.empty() && fixtures[f].name.find(filter) == std::string::npos)
            continue;
        for (const Resolution& res : resolutions)
        {
            pixels.assign(static_cast<std::size_t>(res.width) * res.height * 4, 0);
            for (int frame = 0; frame < frames; ++frame)
            {
                const bool last = frame == frames - 1;
                ui_harness::SetScale(res.scale);
                ui_harness::Prepare(f);
                float mouseX = -FLT_MAX, mouseY = -FLT_MAX;
                if (!ui_harness::MousePixels(res.width, res.height, mouseX, mouseY))
                    mouseX = mouseY = -FLT_MAX;
                UiHarnessNewFrame(res.width, res.height, clock, mouseX, mouseY);
                clock += 1.0 / 60.0;
                g_rec.Reset(static_cast<float>(res.width), static_cast<float>(res.height));
                g_rec.on = last;
                ui_harness::Overlay(&runtime);
                g_rec.on = false;
                UiHarnessEndFrame(last && png, last && png ? pixels.data() : nullptr);
                if (!last)
                    Sleep(1); // a thumbnail read on its thread finds the file missing
            }
            if (!ui_harness::FontsReady())
            {
                std::fprintf(stderr, "ui harness: the editor's fonts are not ready (RedemptionEditor\\fonts\\Redemption.otf next to the exe, georgia.ttf)\n");
                return 2;
            }
            FrameResult result;
            result.fixture = fixtures[f].name;
            result.res = res;
            result.frame = g_rec.frame;
            result.findings = lay::Analyze(result.frame, options, allowances);
            const std::string stem = FileStem(result.fixture, res.width, res.height, res.scale);
            if (png)
            {
                result.png = "png/" + stem + ".png";
                if (!UiHarnessSavePng(Wide("png\\" + stem + ".png").c_str(), pixels.data(), res.width, res.height))
                    std::fprintf(stderr, "ui harness: could not write png\\%s.png\n", stem.c_str());
                bool any = false;
                for (const lay::Finding& finding : result.findings)
                {
                    if (!finding.allowed.empty())
                        continue;
                    any = true;
                    const int thickness = std::max(2, res.height / 540);
                    Outline(pixels, res.width, res.height, finding.a, CheckColor(finding.check), thickness);
                    if (finding.check == lay::Check::Container || finding.check == lay::Check::Overlap || finding.check == lay::Check::Crowded ||
                        finding.check == lay::Check::EdgeBand)
                        Outline(pixels, res.width, res.height, finding.b, 0x40A0FF, 1);
                }
                if (any)
                {
                    result.marked = "marked/" + stem + ".png";
                    UiHarnessSavePng(Wide("marked\\" + stem + ".png").c_str(), pixels.data(), res.width, res.height);
                }
            }
            std::printf("  %-34s %-10s texts %3zu  findings %3zu (failing %zu)\n", result.fixture.c_str(), ResText(res).c_str(),
                result.frame.texts.size(), result.findings.size(), lay::FailingCount(result.findings));
            results.push_back(std::move(result));
        }
        ui_harness::End();
    }

    // ---------------- Interaction scripts (keys and the mouse over frames) ----------------
    // At 1080p and 4K at the run's interface scale; with --filter only when it names "interact".
    std::size_t interactionFailures = 0;
    if (filter.empty() || filter.find("interact") != std::string::npos)
    {
        const int scale = resolutions.empty() ? 100 : resolutions.front().scale;
        for (const Resolution res : {Resolution{1920, 1080, scale}, Resolution{3840, 2160, scale}})
        {
            ui_harness::SetScale(res.scale);
            const auto frame = [&](const ui_harness::InteractionInput& input) {
                UiHarnessInput(input.keysDown.data(), static_cast<int>(input.keysDown.size()), input.mouseDown);
                UiHarnessNewFrame(res.width, res.height, clock, input.mouseX, input.mouseY);
                clock += 1.0 / 60.0;
                g_rec.Reset(static_cast<float>(res.width), static_cast<float>(res.height));
                g_rec.on = false;
                ui_harness::Overlay(&runtime);
                UiHarnessEndFrame(false, nullptr);
            };
            for (const ui_harness::InteractionResult& r : ui_harness::RunInteractions(res.width, res.height, frame))
            {
                std::printf("  interact %-44s %-10s %s%s\n", r.name.c_str(), ResText(res).c_str(), r.failure.empty() ? "ok" : "FAILED: ",
                    r.failure.c_str());
                if (g_log)
                    std::fprintf(g_log, "INTERACTION,name=%s,res=%s,ok=%d,failure=%s\n", r.name.c_str(), ResText(res).c_str(), r.failure.empty() ? 1 : 0,
                        r.failure.c_str());
                interactionFailures += r.failure.empty() ? 0u : 1u;
            }
            const int none[1] = {0};
            UiHarnessInput(none, 0, false); // the keys and the button up for whatever runs next
        }
    }
    UiHarnessShutdown();

    // ---------------- Groups (a finding over every resolution) ----------------
    std::map<std::string, Group> groups;
    std::size_t counts[static_cast<int>(lay::Check::Count)][3]{};
    std::size_t allowedCount = 0;
    std::size_t failing = 0;
    for (const FrameResult& r : results)
    {
        for (const lay::Finding& finding : r.findings)
        {
            const lay::TextItem& t = r.frame.texts[finding.text];
            const std::string other = finding.other != lay::kNone ? r.frame.texts[finding.other].text : "";
            const std::string box = finding.box != lay::kNone ? r.frame.boxes[finding.box].kind + (r.frame.boxes[finding.box].id.empty() ? "" : " " + r.frame.boxes[finding.box].id) : "";
            // A too small text is the size of a label at a resolution, on every
            // screen that uses it: one group per font and size (in the 1080
            // units of the layout), not per fixture.
            // Texts out of one box on one side (the rows of a help column past
            // its panel's foot) are one finding of that box.
            const bool small = finding.check == lay::Check::MinHeight;
            const bool spill = finding.check == lay::Check::Container;
            char size[32];
            std::snprintf(size, sizeof(size), "%.1f", t.px * 1080.0f * 100.0f / (static_cast<float>(r.res.height) * static_cast<float>(r.res.scale)));
            const float sides[4] = {finding.b.x0 - finding.a.x0, finding.b.y0 - finding.a.y0, finding.a.x1 - finding.b.x1, finding.a.y1 - finding.b.y1};
            static const char* const sideNames[4] = {"left", "top", "right", "bottom"};
            const int side = static_cast<int>(std::max_element(sides, sides + 4) - sides);
            const std::string key = small ? std::string("min_height|") + t.font + "|" + size
                : spill ? std::string("container|") + r.fixture + "|" + t.scope + "|" + box + "|" + sideNames[side]
                        : std::string(lay::CheckName(finding.check)) + "|" + r.fixture + "|" + t.scope + "|" + t.text + "|" + other;
            Group& g = groups[key];
            if (g.resolutions.empty())
            {
                g.check = finding.check;
                g.fixture = r.fixture;
                g.scope = small ? t.font + " font, " + size + " units" : t.scope;
                g.text = t.text;
                g.other = other;
                g.box = spill ? box + ", past its " + sideNames[side] + " edge" : box;
                g.allowed = finding.allowed;
            }
            g.severity = std::max(g.severity, finding.severity);
            const std::string resText = ResText(r.res);
            if (std::find(g.resolutions.begin(), g.resolutions.end(), resText) == g.resolutions.end())
                g.resolutions.push_back(resText);
            g.fixtures.insert(r.fixture);
            if ((small || spill) && std::find(g.examples.begin(), g.examples.end(), t.text) == g.examples.end())
                g.examples.push_back(t.text);
            const bool worse = small ? finding.amount < g.worst : finding.amount > g.worst;
            if (g.worstRes.empty() || worse)
            {
                g.worst = finding.amount;
                g.worstRes = resText;
                g.worstPng = r.marked.empty() ? r.png : r.marked;
                g.worstRects = RectText(finding.a) + (finding.b.x1 > finding.b.x0 ? " vs " + RectText(finding.b) : "");
            }
            if (finding.allowed.empty())
                ++counts[static_cast<int>(finding.check)][static_cast<int>(finding.severity)];
            else
                ++allowedCount;
            if (finding.severity == lay::Severity::Error && finding.allowed.empty())
                ++failing;
        }
    }
    for (auto& [key, g] : groups)
    {
        if (g.check == lay::Check::MinHeight)
            g.fixture = std::to_string(g.fixtures.size()) + " fixtures";
        if (g.check != lay::Check::MinHeight && g.check != lay::Check::Container)
            continue;
        if (g.examples.size() < 2)
            continue;
        // The texts of the group: how many, the first ones.
        g.text = std::to_string(g.examples.size()) + " texts: ";
        for (std::size_t e = 0; e < g.examples.size() && e < 5; ++e)
            g.text += (e ? " | " : "") + Shorten(g.examples[e], 32);
    }
    std::vector<const Group*> sorted;
    for (const auto& [key, g] : groups)
        sorted.push_back(&g);
    std::sort(sorted.begin(), sorted.end(), [](const Group* a, const Group* b) {
        if (a->allowed.empty() != b->allowed.empty())
            return a->allowed.empty();
        if (a->severity != b->severity)
            return a->severity > b->severity;
        if (CheckRank(a->check) != CheckRank(b->check))
            return CheckRank(a->check) < CheckRank(b->check);
        if (a->resolutions.size() != b->resolutions.size())
            return a->resolutions.size() > b->resolutions.size();
        if (a->check == lay::Check::MinHeight)
            return a->worst < b->worst;
        return a->worst > b->worst;
    });

    // ---------------- findings.txt and findings-table.md ----------------
    std::string text = "UI harness findings (worst first). Counts by check, not allowed, error/warning/info:\n";
    for (int c = 0; c < static_cast<int>(lay::Check::Count); ++c)
    {
        char line[128];
        std::snprintf(line, sizeof(line), "  %-11s %5zu / %5zu / %5zu\n", lay::CheckName(static_cast<lay::Check>(c)), counts[c][2], counts[c][1],
            counts[c][0]);
        text += line;
    }
    {
        char line[160];
        std::snprintf(line, sizeof(line), "  allowed by design: %zu; failing (errors not allowed): %zu; frames: %zu; groups: %zu\n\n", allowedCount,
            failing, results.size(), sorted.size());
        text += line;
    }
    std::string table = "| # | Severity | Check | Screen (fixture) | Part | Text | Other text / box | Worst case | Resolutions | PNG |\n"
                        "|---|---|---|---|---|---|---|---|---|---|\n";
    int index = 0;
    for (const Group* g : sorted)
    {
        ++index;
        char amount[48];
        std::snprintf(amount, sizeof(amount), g->check == lay::Check::MinHeight ? "%.1f px cap" : "%.1f px", g->worst);
        std::string resList;
        for (const std::string& r : g->resolutions)
            resList += (resList.empty() ? "" : " ") + r;
        char head[96];
        std::snprintf(head, sizeof(head), "%4d %-7s %-10s ", index, lay::SeverityName(g->severity), lay::CheckName(g->check));
        text += std::string(head) + g->fixture + "  [" + g->scope + "]  \"" + g->text + "\"" + (g->other.empty() ? "" : "  vs \"" + g->other + "\"") +
            (g->box.empty() ? "" : "  in " + g->box) + "\n       worst " + amount + " at " + g->worstRes + " " + g->worstRects + "  (" +
            std::to_string(g->resolutions.size()) + " res: " + resList + ")" + (g->allowed.empty() ? "" : "\n       allowed: " + g->allowed) +
            "\n       " + g->worstPng + "\n";
        if (g->check != lay::Check::Clipped)
            table += "| " + std::to_string(index) + " | " + lay::SeverityName(g->severity) + (g->allowed.empty() ? "" : " (allowed)") + " | " +
                lay::CheckName(g->check) + " | " + Cell(g->fixture) + " | " + Cell(g->scope) + " | " + Cell(Shorten(g->text, 70)) + " | " +
                Cell(Shorten(g->other.empty() ? g->box : g->other, 50)) + " | " + amount + " at " + g->worstRes + " " + g->worstRects + " | " +
                std::to_string(g->resolutions.size()) + " | " + g->worstPng + " |\n";
    }
    WriteFile("findings.txt", text);
    WriteFile("findings-table.md", table);

    // ---------------- report.json ----------------
    std::string json = "{\n\"options\":{";
    {
        char line[256];
        std::snprintf(line, sizeof(line),
            "\"tolerance\":%.2f,\"spillError\":%.2f,\"overlapMin\":%.2f,\"minCapError\":%.2f,\"minCapWarning\":%.2f,\"frames\":%d},\n",
            options.tolerance, options.spillError, options.overlapMin, options.minCapError, options.minCapWarning, frames);
        json += line;
    }
    json += "\"fixtures\":[";
    for (std::size_t f = 0; f < fixtures.size(); ++f)
        json += (f ? ",\n" : "\n") + std::string("{\"name\":") + Json(fixtures[f].name) + ",\"about\":" + Json(fixtures[f].about) + "}";
    json += "],\n\"resolutions\":[";
    for (std::size_t r = 0; r < resolutions.size(); ++r)
        json += (r ? "," : "") + Json(ResText(resolutions[r]));
    json += "],\n\"frames\":[";
    for (std::size_t n = 0; n < results.size(); ++n)
    {
        const FrameResult& r = results[n];
        json += (n ? ",\n" : "\n") + std::string("{\"fixture\":") + Json(r.fixture) + ",\"res\":" + Json(ResText(r.res)) + ",\"png\":" + Json(r.png) +
            ",\"marked\":" + Json(r.marked) + ",\"texts\":" + std::to_string(r.frame.texts.size()) + ",\"keybar\":" + RectJson(r.frame.keyBar) +
            ",\"findings\":[";
        for (std::size_t k = 0; k < r.findings.size(); ++k)
        {
            const lay::Finding& finding = r.findings[k];
            const lay::TextItem& t = r.frame.texts[finding.text];
            json += (k ? "," : "") + std::string("{\"check\":") + Json(lay::CheckName(finding.check)) + ",\"severity\":" +
                Json(lay::SeverityName(finding.severity)) + ",\"text\":" + Json(t.text) + ",\"scope\":" + Json(t.scope) + ",\"font\":" + Json(t.font) +
                ",\"px\":" + std::to_string(t.px) + ",\"cap\":" + std::to_string(t.capPx) + ",\"a\":" + RectJson(finding.a) + ",\"b\":" + RectJson(finding.b) +
                ",\"amount\":" + std::to_string(finding.amount);
            if (finding.other != lay::kNone)
                json += ",\"other\":" + Json(r.frame.texts[finding.other].text) + ",\"otherScope\":" + Json(r.frame.texts[finding.other].scope);
            if (finding.box != lay::kNone)
                json += ",\"box\":" + Json(r.frame.boxes[finding.box].kind) + ",\"boxId\":" + Json(r.frame.boxes[finding.box].id);
            if (!finding.allowed.empty())
                json += ",\"allowed\":" + Json(finding.allowed);
            json += "}";
        }
        json += "]";
        if (dump)
        {
            json += ",\"all_texts\":[";
            for (std::size_t k = 0; k < r.frame.texts.size(); ++k)
            {
                const lay::TextItem& t = r.frame.texts[k];
                json += (k ? "," : "") + std::string("{\"t\":") + Json(t.text) + ",\"ink\":" + RectJson(t.ink) + ",\"clip\":" + RectJson(t.clip) +
                    ",\"scope\":" + Json(t.scope) + ",\"group\":" + std::to_string(t.group) + "}";
            }
            json += "],\"boxes\":[";
            for (std::size_t k = 0; k < r.frame.boxes.size(); ++k)
            {
                const lay::Box& b = r.frame.boxes[k];
                json += (k ? "," : "") + std::string("{\"kind\":") + Json(b.kind) + ",\"id\":" + Json(b.id) + ",\"r\":" + RectJson(b.rect) +
                    ",\"container\":" + (b.container ? "true" : "false") + "}";
            }
            json += "]";
        }
        json += "}";
    }
    json += "\n],\n\"groups\":[";
    for (std::size_t n = 0; n < sorted.size(); ++n)
    {
        const Group* g = sorted[n];
        std::string resList;
        for (const std::string& r : g->resolutions)
            resList += (resList.empty() ? "" : ",") + Json(r);
        json += (n ? ",\n" : "\n") + std::string("{\"check\":") + Json(lay::CheckName(g->check)) + ",\"severity\":" + Json(lay::SeverityName(g->severity)) +
            ",\"fixture\":" + Json(g->fixture) + ",\"scope\":" + Json(g->scope) + ",\"text\":" + Json(g->text) + ",\"other\":" + Json(g->other) +
            ",\"box\":" + Json(g->box) + ",\"worst\":" + std::to_string(g->worst) + ",\"worstRes\":" + Json(g->worstRes) + ",\"png\":" + Json(g->worstPng) +
            ",\"rects\":" + Json(g->worstRects) + ",\"allowed\":" + Json(g->allowed) + ",\"resolutions\":[" + resList + "]}";
    }
    json += "\n]\n}\n";
    WriteFile("report.json", json);

    // ---------------- index.html (the contact sheet) ----------------
    {
        std::string data = "{\"frames\":[";
        for (std::size_t n = 0; n < results.size(); ++n)
        {
            const FrameResult& r = results[n];
            data += (n ? "," : "") + std::string("{\"f\":") + Json(r.fixture) + ",\"r\":" + Json(ResText(r.res)) + ",\"w\":" + std::to_string(r.res.width) +
                ",\"h\":" + std::to_string(r.res.height) + ",\"png\":" + Json(r.png) + ",\"k\":[";
            for (std::size_t k = 0; k < r.findings.size(); ++k)
            {
                const lay::Finding& finding = r.findings[k];
                const lay::TextItem& t = r.frame.texts[finding.text];
                data += (k ? "," : "") + std::string("{\"c\":") + Json(lay::CheckName(finding.check)) + ",\"s\":" + Json(lay::SeverityName(finding.severity)) +
                    ",\"t\":" + Json(t.text) + ",\"o\":" + Json(finding.other != lay::kNone ? r.frame.texts[finding.other].text : "") + ",\"a\":" +
                    RectJson(finding.a) + ",\"b\":" + RectJson(finding.b) + ",\"al\":" + (finding.allowed.empty() ? "0" : "1") + "}";
            }
            data += "]}";
        }
        data += "],\"fixtures\":[";
        for (std::size_t f = 0; f < fixtures.size(); ++f)
            data += (f ? "," : "") + std::string("{\"n\":") + Json(fixtures[f].name) + ",\"a\":" + Json(fixtures[f].about) + "}";
        data += "],\"groups\":[";
        for (std::size_t n = 0; n < sorted.size(); ++n)
        {
            const Group* g = sorted[n];
            data += (n ? "," : "") + std::string("{\"c\":") + Json(lay::CheckName(g->check)) + ",\"s\":" + Json(lay::SeverityName(g->severity)) + ",\"f\":" +
                Json(g->fixture) + ",\"sc\":" + Json(g->scope) + ",\"t\":" + Json(g->text) + ",\"o\":" + Json(g->other.empty() ? g->box : g->other) +
                ",\"w\":" + std::to_string(g->worst) + ",\"wr\":" + Json(g->worstRes) + ",\"n\":" + std::to_string(g->resolutions.size()) + ",\"al\":" +
                Json(g->allowed) + "}";
        }
        data += "]}";
        // "</" never reaches the script block.
        std::string safe;
        for (std::size_t n = 0; n < data.size(); ++n)
        {
            if (data[n] == '<')
                safe += "\\u003c";
            else
                safe += data[n];
        }
        std::string html = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Redemption Editor UI tests</title>
<style>
:root{--bg:#151311;--panel:#1f1c19;--text:#ece6da;--muted:#968f83;--line:#3a352f;--red:#b3160f;--viewport:#ff2020;--container:#ff9000;--overlap:#ff20ff;--keybar:#ffff00;--min_height:#20e0ff;--clipped:#909090;--crowded:#60ff60;--edge_band:#ff4080;--cue:#00ffa0}
body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 system-ui,Segoe UI,sans-serif}
header{padding:16px;border-bottom:1px solid var(--line)}h1{margin:0 0 6px;font-size:20px}h2{font-size:16px;margin:18px 16px 8px}
.counts span{display:inline-block;margin:0 14px 4px 0}.bar{padding:8px 16px;display:flex;flex-wrap:wrap;gap:12px;align-items:center;position:sticky;top:0;background:var(--bg);z-index:2;border-bottom:1px solid var(--line)}
table{border-collapse:collapse;margin:0 16px;width:calc(100% - 32px)}td,th{border-bottom:1px solid var(--line);padding:4px 6px;text-align:left;vertical-align:top}th{color:var(--muted);font-weight:600}
td.t{max-width:420px;word-break:break-word}.sev-error{color:#ff6b5e}.sev-warning{color:#f0c040}.sev-info{color:var(--muted)}
.fx{margin:10px 16px;padding:8px;background:var(--panel);border-radius:6px}.fx h3{margin:0 0 6px;font-size:14px}.fx p{margin:0 0 6px;color:var(--muted)}
.row{display:flex;gap:8px;overflow-x:auto}.cell{flex:0 0 auto;cursor:pointer}.cell div{font-size:12px;color:var(--muted)}.cell b{color:#ff6b5e}
.thumb{position:relative;width:200px}.thumb img{width:100%;display:block;border:1px solid var(--line)}.thumb svg{position:absolute;inset:0;width:100%;height:100%}
#view{position:fixed;inset:0;background:rgba(0,0,0,.92);display:none;z-index:5;overflow:auto}#view .in{position:relative;margin:12px auto;max-width:96vw}
#view img{width:100%;display:block}#view svg{position:absolute;inset:0;width:100%;height:100%}#view .cap{color:var(--text);padding:6px 12px}
#view ul{margin:4px 12px;padding-left:18px;color:var(--muted)}
</style></head><body>
<header><h1>Redemption Editor UI tests</h1><div class="counts" id="counts"></div></header>
<div class="bar">Checks: <span id="checks"></span><label><input type="checkbox" id="allowed"> show allowed</label><label>min severity <select id="sev"><option value="0">info</option><option value="1" selected>warning</option><option value="2">error</option></select></label></div>
<h2>Findings (grouped over the resolutions, worst first)</h2><table id="groups"><thead><tr><th>#</th><th>sev</th><th>check</th><th>fixture</th><th>scope</th><th>text</th><th>other / box</th><th>worst</th><th>res</th></tr></thead><tbody></tbody></table>
<h2>Contact sheet</h2><div id="sheet"></div>
<div id="view"><div class="in"><div class="cap" id="vcap"></div><div style="position:relative"><img id="vimg" alt=""><svg id="vsvg" preserveAspectRatio="none"></svg></div><ul id="vlist"></ul></div></div>
<script>
const DATA=)HTML";
        html += safe;
        html += R"HTML(;
const CHECKS=["viewport","keybar","overlap","container","edge_band","cue","min_height","crowded","clipped"];const SEV={info:0,warning:1,error:2};
const on=new Set(["viewport","keybar","overlap","container","edge_band","cue","min_height","crowded"]);
function vis(k){return on.has(k.c)&&SEV[k.s]>=+document.getElementById('sev').value&&(document.getElementById('allowed').checked||!(k.al==1||k.al&&k.al.length))}
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
function rects(svg,fr){svg.setAttribute('viewBox','0 0 '+fr.w+' '+fr.h);let s='';for(const k of fr.k){if(!vis(k))continue;const a=k.a,b=k.b,col='var(--'+k.c+')';
s+='<rect x="'+a[0]+'" y="'+a[1]+'" width="'+Math.max(1,a[2]-a[0])+'" height="'+Math.max(1,a[3]-a[1])+'" fill="none" stroke="'+col+'" stroke-width="'+Math.max(2,fr.h/400)+'"><title>'+esc(k.c+': '+k.t)+'</title></rect>';
if((k.c=='container'||k.c=='overlap'||k.c=='crowded'||k.c=='edge_band')&&b[2]>b[0])s+='<rect x="'+b[0]+'" y="'+b[1]+'" width="'+(b[2]-b[0])+'" height="'+(b[3]-b[1])+'" fill="none" stroke="#40a0ff" stroke-dasharray="6 4" stroke-width="'+Math.max(1,fr.h/800)+'"/>';}svg.innerHTML=s}
function render(){const counts={};for(const fr of DATA.frames)for(const k of fr.k){if(!vis(k))continue;counts[k.c]=(counts[k.c]||0)+1}
document.getElementById('counts').innerHTML=DATA.frames.length+' frames, '+DATA.fixtures.length+' fixtures. '+CHECKS.map(c=>'<span style="color:var(--'+c+')">'+c+': '+(counts[c]||0)+'</span>').join('');
const tb=document.querySelector('#groups tbody');let rows='',i=0;for(const g of DATA.groups){const k={c:g.c,s:g.s,al:g.al};if(!vis(k))continue;i++;
rows+='<tr><td>'+i+'</td><td class="sev-'+g.s+'">'+g.s+(g.al?' (allowed)':'')+'</td><td style="color:var(--'+g.c+')">'+g.c+'</td><td><a href="#fx-'+esc(g.f)+'">'+esc(g.f)+'</a></td><td>'+esc(g.sc)+'</td><td class="t">'+esc(g.t)+'</td><td class="t">'+esc(g.o)+'</td><td>'+g.w.toFixed(1)+' @ '+g.wr+'</td><td>'+g.n+'</td></tr>'}
tb.innerHTML=rows;const sheet=document.getElementById('sheet');let h='';for(const fx of DATA.fixtures){const frs=DATA.frames.filter(f=>f.f==fx.n);if(!frs.length)continue;
h+='<div class="fx" id="fx-'+esc(fx.n)+'"><h3>'+esc(fx.n)+'</h3><p>'+esc(fx.a)+'</p><div class="row">';for(const fr of frs){const n=fr.k.filter(vis).length;const idx=DATA.frames.indexOf(fr);
h+='<div class="cell" data-i="'+idx+'"><div class="thumb"><img loading="lazy" src="'+fr.png+'" alt=""><svg preserveAspectRatio="none"></svg></div><div>'+fr.r+(n?' <b>'+n+'</b>':'')+'</div></div>'}h+='</div></div>'}
sheet.innerHTML=h;for(const c of sheet.querySelectorAll('.cell')){const fr=DATA.frames[+c.dataset.i];rects(c.querySelector('svg'),fr);c.onclick=()=>open(fr)}}
function open(fr){document.getElementById('vimg').src=fr.png;rects(document.getElementById('vsvg'),fr);document.getElementById('vcap').textContent=fr.f+' @ '+fr.r+' (click to close)';
document.getElementById('vlist').innerHTML=fr.k.filter(vis).map(k=>'<li><span style="color:var(--'+k.c+')">'+k.c+'</span> '+k.s+': "'+esc(k.t)+'"'+(k.o?' vs "'+esc(k.o)+'"':'')+'</li>').join('');document.getElementById('view').style.display='block'}
document.getElementById('view').onclick=()=>{document.getElementById('view').style.display='none'};
document.getElementById('checks').innerHTML=CHECKS.map(c=>'<label style="color:var(--'+c+')"><input type="checkbox" data-c="'+c+'"'+(on.has(c)?' checked':'')+'> '+c+'</label>').join(' ');
for(const b of document.querySelectorAll('#checks input'))b.onchange=()=>{b.checked?on.add(b.dataset.c):on.delete(b.dataset.c);render()};
document.getElementById('allowed').onchange=render;document.getElementById('sev').onchange=render;render();
</script></body></html>
)HTML";
        WriteFile("index.html", html);
    }

    std::printf("\n%s", text.substr(0, text.find("\n\n") + 2).c_str());
    // The readable list: the failing groups, worst first.
    int shown = 0;
    for (const Group* g : sorted)
    {
        if (!g->allowed.empty() || g->severity != lay::Severity::Error)
            continue;
        if (++shown > 40)
            break;
        std::printf("  %-10s %-30s %-28s \"%s\"%s%s  worst %.1f at %s (%zu res)\n", lay::CheckName(g->check), g->fixture.c_str(), g->scope.c_str(),
            Shorten(g->text, 60).c_str(), g->other.empty() ? "" : " vs ", g->other.empty() ? "" : ("\"" + Shorten(g->other, 40) + "\"").c_str(),
            g->worst, g->worstRes.c_str(), g->resolutions.size());
    }
    // A letter of the language no face had is drawn as '?': an error.
    const std::size_t missingGlyphs = ui_harness::MissingGlyphs();
    std::printf("\nmissing glyphs (drawn as '?'): %zu\n", missingGlyphs);
    if (missingGlyphs)
        ++failing;
    std::printf("interaction scripts failing: %zu\n", interactionFailures);
    failing += interactionFailures;
    std::printf("\nreport: %s\\index.html, report.json, findings.txt\n", out.c_str());
    if (g_log)
        std::fclose(g_log);
    return failing ? 1 : 0;
}
