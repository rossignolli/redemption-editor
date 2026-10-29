#pragma once

// The editor's fonts. ReShade does not expose ImFontAtlas to add-ons, so each
// font is baked with stb_truetype into the add-on's own texture and drawn one
// image quad per glyph.
//   title: Redemption, an OpenType CFF file shipped in RedemptionEditor\fonts,
//          fitted (RedemptionFit) to the layout the labels were designed for
//          and drawn upper case.
//   body:  Georgia from the Windows fonts folder, for paragraphs (wrapped).
// Without a font file the editor keeps ReShade's font for that role.
//
// Texts are UTF-8. Printable ASCII is always baked, first and alone in its
// pass, so an English atlas is the same texture as before; the active
// language adds the code points it uses (SetLanguageGlyphs), packed after it.
// A code point the face lacks comes from the first fallback face of its range
// (Fallback) that has it, its capitals set to the face's height; else '?'.
// Chinese, Japanese and Korean come from the Windows face of that language
// (SetLanguageGlyphs: msyh.ttc, msjh.ttc, YuGothM.ttc, malgun.ttf, then the
// others in turn, a glyph at a time), baked smaller than Latin (kCjkScale,
// no oversampling) so a language's few hundred ideographs fit a 1024 x 2048
// or 1024 x 4096 atlas; MissingGlyphs says what no face had.

#define STB_RECT_PACK_IMPLEMENTATION
#define STBRP_STATIC
#include <imstb_rectpack.h>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <imstb_truetype.h>

#include "bundled_files.h"
#include "i18n.h"          // UTF-8 decoding, capitals
#include "ui_probe.h"      // the UI harness probe (empty in the add-on)

#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace display_font
{
    constexpr int kFirstChar = 32;
    constexpr int kCharCount = 95; // printable ASCII
    constexpr int kAtlasSize = 1024; // the width, and the height while ASCII and a language fit
    constexpr int kMaxAtlasHeight = 4096;
    // The smallest capital the editor draws, in pixels (the H as drawn,
    // antialiasing included); small labels at low resolutions are drawn at it.
    constexpr float kMinCapPx = 7.2f;

    inline std::vector<unsigned char> ReadWholeFile(const std::wstring& path)
    {
        std::vector<unsigned char> bytes;
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
            return bytes;
        std::fseek(file, 0, SEEK_END);
        const long size = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        if (size > 0 && size < 64 * 1024 * 1024) // msjh.ttc is 21 MB
        {
            bytes.resize(static_cast<std::size_t>(size));
            if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size())
                bytes.clear();
        }
        std::fclose(file);
        return bytes;
    }

#ifdef REDEMPTION_UI_HARNESS
    // The UI harness's --hide-face: installed faces taken as missing (a PC without them).
    inline std::vector<std::wstring>& HiddenFaces()
    {
        static std::vector<std::wstring> names;
        return names;
    }
#endif

    // The per-user font folder first (where Windows installs a font by
    // double click), then the system one.
    inline std::vector<unsigned char> FindInstalledFont(const std::vector<const wchar_t*>& names)
    {
        std::vector<std::wstring> folders;
        wchar_t buffer[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH))
            folders.push_back(std::wstring(buffer) + L"\\Microsoft\\Windows\\Fonts\\");
        if (GetEnvironmentVariableW(L"WINDIR", buffer, MAX_PATH))
            folders.push_back(std::wstring(buffer) + L"\\Fonts\\");
        for (const std::wstring& folder : folders)
            for (const wchar_t* name : names)
            {
#ifdef REDEMPTION_UI_HARNESS
                if (std::find(HiddenFaces().begin(), HiddenFaces().end(), std::wstring(name)) != HiddenFaces().end())
                    continue;
#endif
                std::vector<unsigned char> bytes = ReadWholeFile(folder + name);
                if (!bytes.empty())
                    return bytes;
            }
        return {};
    }

    // A font shipped with the mod, in RedemptionEditor\fonts next to RDR2.exe.
    inline std::vector<unsigned char> FindBundledFont(const std::vector<const wchar_t*>& names)
    {
        const std::wstring folder = bundled_files::RedemptionEditorFolder() + L"fonts\\";
        for (const wchar_t* name : names)
        {
            std::vector<unsigned char> bytes = ReadWholeFile(folder + name);
            if (!bytes.empty())
                return bytes;
        }
        return {};
    }

    enum class Source
    {
        Installed, // the per-user and the Windows fonts folders
        Bundled,   // RedemptionEditor\fonts
    };

    // How a face replaces another without moving the layout made for the
    // first one. capHeight: the height of a capital as a fraction of the pixel
    // size (0 keeps the face's own); capMiddle: where the middle of a capital
    // sits under the top of the line, same unit; upper: every letter drawn and
    // measured as a capital.
    struct Fit
    {
        float capHeight = 0.0f;
        float capMiddle = 0.0f;
        bool upper = false;
    };

    // The metrics the labels were laid out for: Chinese Rocks RG (hhea ascent
    // 950, descent -250, H 744 units high, lower case drawn as capitals).
    constexpr Fit TitleFit{744.0f / 1200.0f, (950.0f - 744.0f * 0.5f) / 1200.0f, true};

    // Redemption (H 719 of 1000 units) at TitleFit's capital height is 15 %
    // wider in the median label, so its capitals are set at 0.62 / 1.147 of
    // the pixel size: labels keep their width and stay centred where TitleFit
    // centres them.
    constexpr Fit RedemptionFit{TitleFit.capHeight / 1.147f, TitleFit.capMiddle, true};

    // A face for the code points of [first, last] the main face lacks;
    // `scale` < 1 bakes it smaller, without oversampling (the CJK faces).
    struct Fallback
    {
        std::uint32_t first;
        std::uint32_t last;
        std::vector<const wchar_t*> files;
        Source source;
        float scale = 1.0f;
    };

    // The CJK bake: this share of the Latin size, 1 x 1 oversampling (an
    // ideograph is drawn from about 43 px in the title font, 36 px in the body).
    constexpr float kCjkTitleScale = 0.6f;
    constexpr float kCjkBodyScale = 0.75f;

    class BakedFont
    {
    public:
        BakedFont(std::vector<const wchar_t*> files, float bakePx, Source source = Source::Installed, Fit fit = {},
            std::vector<Fallback> fallbacks = {})
            : m_files(std::move(files)), m_bakePx(bakePx), m_source(source), m_fit(fit), m_fallbacks(std::move(fallbacks)),
              m_fixedFallbacks(m_fallbacks.size()) {}

        // The code points past ASCII to bake (the active language's); a change
        // bakes the atlas again at the next Ensure.
        void SetExtraGlyphs(std::vector<std::uint32_t> codes)
        {
            for (std::uint32_t& code : codes)
                code = Fold(code);
            codes.erase(std::remove_if(codes.begin(), codes.end(), [](std::uint32_t c) { return c < 0x80; }), codes.end());
            std::sort(codes.begin(), codes.end());
            codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
            if (codes == m_wanted)
                return;
            m_wanted = std::move(codes);
            if (m_tried)
                m_rebake = true;
        }

        // The faces after the fixed fallbacks: the language's CJK faces, in
        // the order they are tried (each one a glyph at a time).
        void SetLanguageFaces(std::vector<Fallback> faces)
        {
            m_fallbacks.resize(m_fixedFallbacks);
            for (Fallback& face : faces)
                m_fallbacks.push_back(std::move(face));
            m_fallbackFiles.clear();
            if (m_tried)
                m_rebake = true;
        }

        // The wanted code points no face had (drawn as '?').
        std::size_t MissingGlyphs() const { return m_missing; }
        std::size_t WantedGlyphs() const { return m_wanted.size(); }

        // Once per effect runtime, and again when the glyphs change, on the
        // render thread.
        void Ensure(reshade::api::effect_runtime* runtime)
        {
            RetireOld();
            if (m_tried && !m_rebake)
                return;
            const bool rebake = m_rebake;
            m_tried = true;
            m_rebake = false;
            if (m_file.empty())
                m_file = m_source == Source::Bundled ? FindBundledFont(m_files) : FindInstalledFont(m_files);
            if (m_file.empty())
                return;
            std::vector<unsigned char>& file = m_file;
            stbtt_fontinfo info{};
            if (!stbtt_InitFont(&info, file.data(), stbtt_GetFontOffsetForIndex(file.data(), 0)))
                return;
            int ascent = 0, descent = 0, gap = 0;
            stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
            const float scale = stbtt_ScaleForPixelHeight(&info, m_bakePx);
            m_ascent = static_cast<float>(ascent) * scale;
            m_lineHeight = static_cast<float>(ascent - descent + gap) * scale;
            // The Fit, from the face's own capital H.
            m_size = 1.0f;
            m_shift = 0.0f;
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            if (m_fit.capHeight > 0.0f && stbtt_GetCodepointBox(&info, 'H', &x0, &y0, &x1, &y1) && y1 > 0)
            {
                const float cap = static_cast<float>(y1) * scale / m_bakePx; // fraction of the pixel size
                m_size = m_fit.capHeight / cap;
                m_shift = m_fit.capMiddle - (m_ascent / m_bakePx - cap * 0.5f) * m_size;
            }

            // ASCII first and alone (the English atlas), then the language's
            // code points; a taller atlas when they do not fit.
            std::vector<unsigned char> alpha;
            bool packed = false;
            for (int height = kAtlasSize; height <= kMaxAtlasHeight && !packed; height *= 2)
            {
                alpha.assign(static_cast<std::size_t>(kAtlasSize) * height, 0);
                stbtt_pack_context pack{};
                if (!stbtt_PackBegin(&pack, alpha.data(), kAtlasSize, height, 0, 2, nullptr))
                    return;
                stbtt_PackSetOversampling(&pack, 2, 2);
                packed = stbtt_PackFontRange(&pack, file.data(), 0, m_bakePx, kFirstChar, kCharCount, m_chars) != 0 &&
                    PackExtra(pack, info, y1Of(info, 'H', scale));
                stbtt_PackEnd(&pack);
                m_atlasHeight = height;
            }
            if (!packed)
                return;
            {
                // A capital's height per pixel of size (the H as drawn), for MinPx.
                float hx = 0.0f, hy = m_ascent;
                stbtt_aligned_quad h{};
                stbtt_GetPackedQuad(Glyph('H'), kAtlasSize, m_atlasHeight, 0, &hx, &hy, &h, 0);
                m_capPerPx = (h.y1 - h.y0) * m_size / m_bakePx;
            }
            if (rebake)
                RetireCurrent();

            std::vector<std::uint32_t> rgba(alpha.size());
            for (std::size_t i = 0; i < alpha.size(); ++i)
                rgba[i] = 0x00FFFFFFu | (static_cast<std::uint32_t>(alpha[i]) << 24);
            {
                // The atlas's size and how much of it the glyphs take (the memory a language costs).
                int used = 0;
                for (const stbtt_packedchar& c : m_chars)
                    used = std::max(used, static_cast<int>(c.y1));
                for (const stbtt_packedchar& c : m_extraChars)
                    used = std::max(used, static_cast<int>(c.y1));
                char line[200];
                std::snprintf(line, sizeof(line), "FONT_BAKE,font=%s,atlas=%dx%d,rows_used=%d,extra=%zu,asked=%zu,missing=%zu,faces=%s",
                    m_source == Source::Bundled ? "title" : "body", kAtlasSize, m_atlasHeight, used, m_extraCodes.size(), m_wanted.size(), m_missing,
                    m_facesUsed.c_str());
                reshade::log::message(reshade::log::level::info, line);
            }

#ifdef REDEMPTION_UI_HARNESS
            // The UI harness's texture (no ReShade device there).
            if (ui_probe::Current())
            {
                m_view.handle = ui_probe::Current()->CreateTexture(rgba.data(), kAtlasSize, m_atlasHeight);
                m_ready = m_view.handle != 0;
                return;
            }
#endif
            using namespace reshade::api;
            device* dev = runtime->get_device();
            subresource_data data{};
            data.data = rgba.data();
            data.row_pitch = kAtlasSize * 4;
            data.slice_pitch = kAtlasSize * m_atlasHeight * 4;
            const resource_desc desc(kAtlasSize, m_atlasHeight, 1, 1, format::r8g8b8a8_unorm, 1, memory_heap::gpu_only, resource_usage::shader_resource);
            if (!dev->create_resource(desc, &data, resource_usage::shader_resource, &m_texture))
                return;
            if (!dev->create_resource_view(m_texture, resource_usage::shader_resource, resource_view_desc(format::r8g8b8a8_unorm), &m_view))
            {
                dev->destroy_resource(m_texture);
                m_texture = {0};
                return;
            }
            m_device = dev;
            m_ready = true;
        }

        void Release()
        {
            for (Retired& old : m_retired)
                Destroy(old.device, old.texture, old.view);
            m_retired.clear();
            if (m_device)
                Destroy(m_device, m_texture, m_view);
            m_device = nullptr;
            m_texture = {0};
            m_view = {0};
            m_ready = false;
            m_tried = false;
        }

        bool Ready() const { return m_ready; }
        // The atlas's size in bytes (RGBA) and the code points baked past ASCII.
        std::size_t AtlasBytes() const { return m_ready ? static_cast<std::size_t>(kAtlasSize) * m_atlasHeight * 4 : 0; }
        std::size_t ExtraGlyphs() const { return m_extraCodes.size(); }
        // The smallest pixel size whose capitals are kMinCapPx high: small
        // labels at small resolutions are drawn at it.
        float MinPx() const { return m_capPerPx > 0.0f ? kMinCapPx / m_capPerPx : 0.0f; }
        float LineHeight(float px) const { return m_lineHeight * px * m_size / m_bakePx; }

        float Width(float px, const char* text, const char* end = nullptr) const
        {
            float advance = 0.0f;
            for (const char* p = text; *p && p != end;)
                advance += Glyph(replay::i18n::Decode(p, end))->xadvance;
            return advance * px * m_size / m_bakePx;
        }

        // pos is the top left of the line, like ImDrawList::AddText.
        void Draw(ImDrawList* draw, ImVec2 pos, float px, ImU32 color, const char* text, const char* end = nullptr) const
        {
            const float scale = px * m_size / m_bakePx;
            pos.y += m_shift * px;
            const ImTextureRef texture(static_cast<ImTextureID>(m_view.handle));
            float x = 0.0f;
#ifdef REDEMPTION_UI_HARNESS
            ImVec2 inkMin(1.0e9f, 1.0e9f), inkMax(-1.0e9f, -1.0e9f); // the glyphs' pixels
#endif
            for (const char* p = text; *p && p != end;)
            {
                float y = m_ascent;
                stbtt_aligned_quad quad{};
                stbtt_GetPackedQuad(Glyph(replay::i18n::Decode(p, end)), kAtlasSize, m_atlasHeight, 0, &x, &y, &quad, 0);
                draw->AddImage(texture,
                    ImVec2(pos.x + quad.x0 * scale, pos.y + quad.y0 * scale),
                    ImVec2(pos.x + quad.x1 * scale, pos.y + quad.y1 * scale),
                    ImVec2(quad.s0, quad.t0), ImVec2(quad.s1, quad.t1), color);
#ifdef REDEMPTION_UI_HARNESS
                if (quad.x1 > quad.x0 && quad.y1 > quad.y0)
                {
                    inkMin = ImVec2(std::min(inkMin.x, pos.x + quad.x0 * scale), std::min(inkMin.y, pos.y + quad.y0 * scale));
                    inkMax = ImVec2(std::max(inkMax.x, pos.x + quad.x1 * scale), std::max(inkMax.y, pos.y + quad.y1 * scale));
                }
#endif
            }
#ifdef REDEMPTION_UI_HARNESS
            if (ui_probe::Current())
            {
                // A capital's height: the H's glyph box at this size.
                float hx = 0.0f, hy = m_ascent;
                stbtt_aligned_quad h{};
                stbtt_GetPackedQuad(Glyph('H'), kAtlasSize, m_atlasHeight, 0, &hx, &hy, &h, 0);
                ui_probe::Current()->Text(draw, text, end, inkMin, inkMax, px * m_size, (h.y1 - h.y0) * scale,
                    m_source == Source::Bundled ? "title" : "body");
            }
#endif
        }

        // Word wrap at wrapWidth pixels; returns the height drawn. A line breaks
        // at a space, and between two CJK characters (or one and a Latin one)
        // where the kinsoku rules allow it (i18n.h NoLineStart, NoLineEnd).
        float DrawWrapped(ImDrawList* draw, ImVec2 pos, float px, ImU32 color, const char* text, float wrapWidth) const
        {
            const float line = LineHeight(px) * 1.05f;
            UI_PROBE_GROUP(draw != nullptr); // the lines of one paragraph
            float y = pos.y;
            const char* lineStart = text;
            while (*lineStart)
            {
                const char* lastBreak = nullptr;
                bool breakAtSpace = true; // the break drops a space (else it is between two characters)
                const char* cut = nullptr; // a word wider than the line breaks at a character
                const char* p = lineStart;
                std::uint32_t previous = 0;
                while (*p && *p != '\n')
                {
                    const char* next = p;
                    const std::uint32_t code = replay::i18n::Decode(next); // the character's end (UTF-8)
                    const bool cjkBreak = p > lineStart && previous != ' ' && code != ' ' &&
                        (replay::i18n::IsCjk(code) || replay::i18n::IsCjk(previous)) && !replay::i18n::NoLineStart(code) &&
                        !replay::i18n::NoLineEnd(previous);
                    if (*p == ' ' || cjkBreak)
                    {
                        if (Width(px, lineStart, p) > wrapWidth && lastBreak)
                            break;
                        lastBreak = p;
                        breakAtSpace = *p == ' ';
                    }
                    else if (!lastBreak && p > lineStart && Width(px, lineStart, next) > wrapWidth)
                    {
                        cut = p; // a file name, a path: no space to break at
                        break;
                    }
                    previous = code;
                    p = next;
                }
                // The text before the last break is too wide: a break between
                // characters past the width is taken where it was.
                const char* end = p;
                if (cut)
                    end = cut;
                else if (*p && *p != '\n' && lastBreak)
                    end = lastBreak;
                else if (!*p && Width(px, lineStart, p) > wrapWidth && lastBreak)
                    end = lastBreak;
                if (draw) // null: only measured
                    Draw(draw, ImVec2(pos.x, y), px, color, lineStart, end);
                y += line;
                const bool between = end == lastBreak && !breakAtSpace && !cut;
                lineStart = cut ? cut : between ? end : *end ? end + 1 : end;
            }
            return y - pos.y;
        }

    private:
        // The code point as baked and looked up (a capital in the title font).
        std::uint32_t Fold(std::uint32_t code) const { return m_fit.upper ? replay::i18n::ToUpper(code) : code; }

        // A code point's glyph: ASCII, a baked extra, else '?'.
        const stbtt_packedchar* Glyph(std::uint32_t code) const
        {
            code = Fold(code);
            if (code >= kFirstChar && code < static_cast<std::uint32_t>(kFirstChar + kCharCount))
                return &m_chars[code - kFirstChar];
            const auto it = std::lower_bound(m_extraCodes.begin(), m_extraCodes.end(), code);
            if (it != m_extraCodes.end() && *it == code)
                return &m_extraChars[static_cast<std::size_t>(it - m_extraCodes.begin())];
            return &m_chars['?' - kFirstChar];
        }

        // The top of a capital H in font units scaled to pixels (0: none).
        static float y1Of(const stbtt_fontinfo& info, int code, float scale)
        {
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            return stbtt_GetCodepointBox(&info, code, &x0, &y0, &x1, &y1) ? static_cast<float>(y1) * scale : 0.0f;
        }

        // The wanted code points after ASCII: the main face's first, then each
        // fallback face's (its size set so its H is the main face's), the rest
        // left to '?'. False when the atlas is full.
        bool PackExtra(stbtt_pack_context& pack, const stbtt_fontinfo& info, float capPx)
        {
            m_extraCodes.clear();
            m_extraChars.clear();
            m_missing = 0;
            m_facesUsed.clear();
            if (m_wanted.empty())
                return true;
            std::vector<std::pair<std::uint32_t, stbtt_packedchar>> baked;
            std::vector<std::uint32_t> missing;
            const auto packFace = [&](const unsigned char* data, const stbtt_fontinfo& face, float size, const std::vector<std::uint32_t>& codes,
                                      std::vector<std::uint32_t>* left, float scale) {
                std::vector<int> have;
                for (const std::uint32_t code : codes)
                {
                    if (stbtt_FindGlyphIndex(&face, static_cast<int>(code)) != 0)
                        have.push_back(static_cast<int>(code));
                    else if (left)
                        left->push_back(code);
                }
                if (have.empty())
                    return true;
                std::vector<stbtt_packedchar> chars(have.size());
                stbtt_pack_range range{};
                range.font_size = size;
                range.array_of_unicode_codepoints = have.data();
                range.num_chars = static_cast<int>(have.size());
                range.chardata_for_range = chars.data();
                if (scale < 1.0f)
                {
                    // Baked smaller and drawn as if at the Latin size: its metrics scaled up.
                    range.font_size = size * scale;
                    stbtt_PackSetOversampling(&pack, 1, 1);
                }
                const int ok = stbtt_PackFontRanges(&pack, data, 0, &range, 1);
                stbtt_PackSetOversampling(&pack, 2, 2);
                if (!ok)
                    return false;
                for (std::size_t n = 0; n < have.size(); ++n)
                {
                    stbtt_packedchar c = chars[n];
                    if (scale < 1.0f)
                    {
                        c.xoff /= scale;
                        c.yoff /= scale;
                        c.xoff2 /= scale;
                        c.yoff2 /= scale;
                        c.xadvance /= scale;
                    }
                    baked.emplace_back(static_cast<std::uint32_t>(have[n]), c);
                }
                return true;
            };
            m_missing = 0;
            m_facesUsed.clear();
            if (!packFace(m_file.data(), info, m_bakePx, m_wanted, &missing, 1.0f))
                return false;
            if (m_fallbackFiles.size() != m_fallbacks.size())
            {
                m_fallbackFiles.clear();
                for (const Fallback& fallback : m_fallbacks)
                    m_fallbackFiles.push_back(fallback.source == Source::Bundled ? FindBundledFont(fallback.files) : FindInstalledFont(fallback.files));
            }
            for (std::size_t f = 0; f < m_fallbacks.size() && !missing.empty(); ++f)
            {
                const std::vector<unsigned char>& data = m_fallbackFiles[f];
                stbtt_fontinfo face{};
                if (data.empty() || !stbtt_InitFont(&face, data.data(), stbtt_GetFontOffsetForIndex(data.data(), 0)))
                    continue;
                std::vector<std::uint32_t> inRange, rest;
                for (const std::uint32_t code : missing)
                    (code >= m_fallbacks[f].first && code <= m_fallbacks[f].last ? inRange : rest).push_back(code);
                const float faceCap = y1Of(face, 'H', stbtt_ScaleForPixelHeight(&face, m_bakePx));
                const float size = capPx > 0.0f && faceCap > 0.0f ? m_bakePx * capPx / faceCap : m_bakePx;
                const std::size_t before = rest.size();
                if (!packFace(data.data(), face, size, inRange, &rest, m_fallbacks[f].scale))
                    return false;
                if (rest.size() - before < inRange.size())
                {
                    // The face that gave glyphs (its first file found), for the log.
                    for (const wchar_t* name : m_fallbacks[f].files)
                    {
                        std::string narrow;
                        for (const wchar_t* c = name; *c; ++c)
                            narrow += static_cast<char>(*c);
                        m_facesUsed += (m_facesUsed.empty() ? "" : ";") + narrow;
                        break;
                    }
                }
                missing = std::move(rest);
            }
            m_missing = missing.size();
            m_fallbackFiles.clear(); // a CJK face is 14 to 21 MB: read again at the next bake
            std::sort(baked.begin(), baked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& entry : baked)
            {
                m_extraCodes.push_back(entry.first);
                m_extraChars.push_back(entry.second);
            }
            return true;
        }

        static void Destroy(reshade::api::device* device, reshade::api::resource texture, reshade::api::resource_view view)
        {
            if (!device)
                return;
            if (view.handle)
                device->destroy_resource_view(view);
            if (texture.handle)
                device->destroy_resource(texture);
        }

        // A texture replaced by a new bake is destroyed some frames later, when
        // no frame in flight draws with it any more.
        struct Retired
        {
            reshade::api::device* device;
            reshade::api::resource texture;
            reshade::api::resource_view view;
            int framesLeft;
        };

        void RetireCurrent()
        {
            if (m_device)
                m_retired.push_back({m_device, m_texture, m_view, 30});
            m_device = nullptr;
            m_texture = {0};
            m_view = {0};
            m_ready = false;
        }

        void RetireOld()
        {
            for (std::size_t n = 0; n < m_retired.size();)
            {
                if (--m_retired[n].framesLeft > 0)
                {
                    ++n;
                    continue;
                }
                Destroy(m_retired[n].device, m_retired[n].texture, m_retired[n].view);
                m_retired.erase(m_retired.begin() + static_cast<std::ptrdiff_t>(n));
            }
        }

        std::vector<const wchar_t*> m_files;
        float m_bakePx = 72.0f;
        Source m_source = Source::Installed;
        Fit m_fit{};
        std::vector<Fallback> m_fallbacks;
        std::size_t m_fixedFallbacks = 0;  // the constructor's; the language's come after
        std::size_t m_missing = 0;
        std::string m_facesUsed;
        std::vector<unsigned char> m_file;                     // the main face, read once
        std::vector<std::vector<unsigned char>> m_fallbackFiles; // empty: not found
        std::vector<std::uint32_t> m_wanted;     // the code points past ASCII asked for
        std::vector<std::uint32_t> m_extraCodes; // ... baked (sorted), m_extraChars their glyphs
        std::vector<stbtt_packedchar> m_extraChars;
        std::vector<Retired> m_retired;
        int m_atlasHeight = kAtlasSize;
        bool m_rebake = false;
        float m_size = 1.0f;  // glyph scale over the pixel size (Fit)
        float m_shift = 0.0f; // line top offset, fraction of the pixel size (Fit)
        float m_capPerPx = 0.0f; // a capital's height per pixel of size
        bool m_tried = false;
        bool m_ready = false;
        float m_ascent = 0.0f;
        float m_lineHeight = 0.0f;
        stbtt_packedchar m_chars[kCharCount]{};
        reshade::api::device* m_device = nullptr;
        reshade::api::resource m_texture{0};
        reshade::api::resource_view m_view{0};
    };

    // Bundled only: every user sees the same title face. The few Latin and
    // Cyrillic letters it lacks come from Georgia.
    inline BakedFont g_title({L"Redemption.otf"}, 72.0f, Source::Bundled, RedemptionFit, {{0x00A0, 0x04FF, {L"georgia.ttf"}, Source::Installed}});
    inline BakedFont g_body({L"georgia.ttf"}, 48.0f);

    // The Windows faces of a CJK language, its own first, then the others
    // (a PC without the first still draws from the next); none for another
    // language.
    inline std::vector<const wchar_t*> CjkFaces(const std::string& code)
    {
        if (code == "ja")
            return {L"YuGothM.ttc", L"meiryo.ttc", L"msgothic.ttc", L"msyh.ttc", L"msjh.ttc", L"malgun.ttf"};
        if (code == "ko")
            return {L"malgun.ttf", L"gulim.ttc", L"msyh.ttc", L"YuGothM.ttc"};
        if (code == "zh-CN")
            return {L"msyh.ttc", L"simsun.ttc", L"msjh.ttc", L"YuGothM.ttc"};
        if (code == "zh-TW")
            return {L"msjh.ttc", L"mingliu.ttc", L"mingliub.ttc", L"msyh.ttc", L"YuGothM.ttc"};
        return {};
    }

    // The code points the active language draws (i18n LanguageGlyphs) and its
    // CJK faces.
    inline void SetLanguageGlyphs(const std::vector<std::uint32_t>& codes, const std::string& code = std::string())
    {
        std::vector<Fallback> title, body;
        for (const wchar_t* face : CjkFaces(code))
        {
            title.push_back({0x1100, 0xFFEF, {face}, Source::Installed, kCjkTitleScale});
            body.push_back({0x1100, 0xFFEF, {face}, Source::Installed, kCjkBodyScale});
        }
        g_title.SetLanguageFaces(std::move(title));
        g_body.SetLanguageFaces(std::move(body));
        g_title.SetExtraGlyphs(codes);
        g_body.SetExtraGlyphs(codes);
    }

    // The share of the language's glyphs neither font could bake (0 to 1).
    inline float MissingShare()
    {
        const std::size_t wanted = g_title.WantedGlyphs() + g_body.WantedGlyphs();
        return wanted ? static_cast<float>(g_title.MissingGlyphs() + g_body.MissingGlyphs()) / static_cast<float>(wanted) : 0.0f;
    }

    inline void Ensure(reshade::api::effect_runtime* runtime)
    {
        g_title.Ensure(runtime);
        g_body.Ensure(runtime);
    }

    inline void Release()
    {
        g_title.Release();
        g_body.Release();
    }

    inline bool Ready() { return g_title.Ready(); }
    inline float Width(float px, const char* text) { return g_title.Width(px, text); }
    inline void Draw(ImDrawList* draw, ImVec2 pos, float px, ImU32 color, const char* text) { g_title.Draw(draw, pos, px, color, text); }
}
