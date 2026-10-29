#pragma once

// The editor's layout maths and the checks of the UI harness, pure (no ImGui,
// no Windows), so tests/ui_layout_tests.cpp covers them.
//   - Rect: a screen rectangle in pixels (x0, y0 top left; x1, y1 bottom
//     right), with the containment and overlap maths of the checks.
//   - PackRows: the key bar's rows (replay_editor_addon.cpp KeyBarRows): as
//     many tips a row as fit, in order.
//   - ShrinkToFit: a label's size stepped down until it fits (MenuRowDraw's
//     list lines, a row's value).
//   - Ellipsize: a name cut with "..." to a width (the home detail line, the
//     timeline's status line, a project's name).
//   - Analyze: the harness's checks on one rendered frame (every text drawn
//     with its pixel rect, the boxes drawn under them, the key bar): text
//     out of the screen, out of its box or its panel, over another text,
//     under the key bar, too small to read, cut by a clip rect, crowded
//     against its neighbour on the line.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace replay::editor::uilayout
{
    struct Rect
    {
        float x0 = 0.0f;
        float y0 = 0.0f;
        float x1 = 0.0f;
        float y1 = 0.0f;
    };

    inline Rect MakeRect(float x0, float y0, float x1, float y1) { return Rect{x0, y0, x1, y1}; }
    inline float Width(const Rect& r) { return r.x1 - r.x0; }
    inline float Height(const Rect& r) { return r.y1 - r.y0; }
    inline bool Empty(const Rect& r) { return !(r.x1 > r.x0) || !(r.y1 > r.y0); }
    inline float Area(const Rect& r) { return Empty(r) ? 0.0f : Width(r) * Height(r); }

    inline Rect Intersect(const Rect& a, const Rect& b)
    {
        return Rect{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    }

    // The smallest rect holding both (an empty one is ignored).
    inline Rect Union(const Rect& a, const Rect& b)
    {
        if (Empty(a))
            return b;
        if (Empty(b))
            return a;
        return Rect{std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
    }

    inline bool ContainsPoint(const Rect& r, float x, float y) { return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1; }

    // How far `inner` goes past the edges of `outer` (the largest of the
    // four sides; 0 when it is inside).
    inline float Overflow(const Rect& outer, const Rect& inner)
    {
        const float left = outer.x0 - inner.x0;
        const float top = outer.y0 - inner.y0;
        const float right = inner.x1 - outer.x1;
        const float bottom = inner.y1 - outer.y1;
        return std::max(0.0f, std::max(std::max(left, right), std::max(top, bottom)));
    }

    inline bool Contains(const Rect& outer, const Rect& inner, float tolerance = 0.0f) { return Overflow(outer, inner) <= tolerance; }

    // ------------------------------------------------------------------
    // Layout maths shared with the editor (the behaviour is the editor's).
    // ------------------------------------------------------------------

    // Where each row starts in `widths`, filled in order: a row takes items
    // while their widths fit in `room`; an item wider than the room still
    // gets a row of its own.
    inline std::vector<std::size_t> PackRows(const std::vector<float>& widths, float room)
    {
        std::vector<std::size_t> starts;
        float used = 0.0f;
        for (std::size_t n = 0; n < widths.size(); ++n)
        {
            if (starts.empty() || used + widths[n] > room)
            {
                starts.push_back(n);
                used = 0.0f;
            }
            used += widths[n];
        }
        return starts;
    }

    // `size` stepped down by 1 while widthAt(size) is wider than `room` and
    // the size is above `minimum` (the result may still be too wide at the
    // minimum: the caller's text then runs over).
    template <typename WidthAt>
    float ShrinkToFit(float size, float minimum, float room, WidthAt widthAt)
    {
        while (size > minimum && widthAt(size) > room)
            size -= 1.0f;
        return size;
    }

    // `text` as wide as `room` at most: whole when it fits, else its longest
    // start (trailing spaces dropped) followed by "...", or "..." alone.
    // `widthOf(candidate)` measures a string in the caller's units.
    template <typename WidthOf>
    std::string Ellipsize(const std::string& text, float room, WidthOf widthOf)
    {
        if (widthOf(text) <= room)
            return text;
        // A cut never splits a UTF-8 character: it moves back to the character's start.
        const auto boundary = [&text](std::size_t at) {
            while (at > 0 && at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80)
                --at;
            return at;
        };
        // The longest prefix that fits, by bisection (the width grows with the length).
        std::size_t low = 0, high = text.size();
        while (low < high)
        {
            const std::size_t mid = (low + high + 1) / 2;
            std::string candidate = text.substr(0, boundary(mid));
            while (!candidate.empty() && candidate.back() == ' ')
                candidate.pop_back();
            if (widthOf(candidate + "...") <= room)
                low = mid;
            else
                high = mid - 1;
        }
        std::string cut = text.substr(0, boundary(low));
        while (!cut.empty() && cut.back() == ' ')
            cut.pop_back();
        return cut + "...";
    }

    // ------------------------------------------------------------------
    // The harness's checks.
    // ------------------------------------------------------------------
    enum class Check : std::uint8_t
    {
        Viewport,  // text past the edge of the screen
        Container, // text past the box it starts in (panel, row, key cap, brush)
        Overlap,   // two texts over each other
        KeyBar,    // a screen's text under the key bar at the foot
        MinHeight, // capitals too few pixels high to read
        Clipped,   // text cut by a clip rect the editor set (by design: info)
        Crowded,   // two texts of a line almost touching (polish: a warning)
        EdgeBand,  // text over a panel's painted ragged edge
        Cue,       // a loading cue (UI_PROBE_BOX "cue") over a text, off the screen, under the key bar, or alone (no caption beside it)
        Count
    };

    enum class Severity : std::uint8_t
    {
        Info,
        Warning,
        Error
    };

    inline const char* CheckName(Check check)
    {
        switch (check)
        {
        case Check::Viewport: return "viewport";
        case Check::Container: return "container";
        case Check::Overlap: return "overlap";
        case Check::KeyBar: return "keybar";
        case Check::MinHeight: return "min_height";
        case Check::Clipped: return "clipped";
        case Check::Crowded: return "crowded";
        case Check::EdgeBand: return "edge_band";
        case Check::Cue: return "cue";
        default: return "?";
        }
    }

    inline const char* SeverityName(Severity severity)
    {
        return severity == Severity::Error ? "error" : severity == Severity::Warning ? "warning" : "info";
    }

    // One text as drawn: `ink` the pixels its glyphs cover, `clip` the clip
    // rect it was drawn under (the screen's when none was set). Lines of one
    // wrapped paragraph share a `group` (>= 0); -1 is a text of its own.
    struct TextItem
    {
        std::string text;
        Rect ink;
        Rect clip;
        float px = 0.0f;    // the font's pixel size
        float capPx = 0.0f; // a capital's height in pixels
        int group = -1;
        int order = 0;      // draw order, shared with the boxes
        std::string scope;  // "Edit/EditPanel"
        std::string font;   // "title", "body", "imgui"
        bool keyBar = false;
        // The panel the screen part draws it in (UI_PROBE_BOX "panel", an
        // index in Frame::boxes; kNone: none): the text must stay in it even
        // where it starts outside every box (a column running past the
        // panel's foot).
        std::size_t panel = static_cast<std::size_t>(-1);
    };

    // A box drawn under texts: a filled panel, an outline (a key cap), a
    // brush, a list row. `container` false: an area that only takes the
    // mouse (it never holds text in the drawing).
    struct Box
    {
        std::string kind;
        std::string id;
        Rect rect;
        int order = 0;
        std::string scope;
        bool container = true;
        // A panel with a painted edge, its body inside the edge band (empty: a
        // flat panel). Its texts stay in the body.
        Rect inner;
    };

    struct Frame
    {
        float width = 0.0f;
        float height = 0.0f;
        std::vector<TextItem> texts;
        std::vector<Box> boxes;
        Rect keyBar; // empty: no key bar
    };

    struct Options
    {
        float tolerance = 1.0f;     // px: a glyph's antialiased edge
        float spillError = 2.0f;    // px past the box: an error (a warning up to it)
        float overlapMin = 1.0f;    // px: an overlap both ways wider than this
        float shadowOffset = 2.5f;  // px: the same text this close is its drop shadow
        float minCapError = 6.0f;   // px of capital height
        float minCapWarning = 7.0f;
        float crowdedGap = 0.35f;   // a gap under this many capital heights, on one line
        float cueReach = 3.0f;      // a loading cue's caption within this many cue sizes of it
    };

    // A finding the layout has by design: matched by check, a part of the
    // scope and a part of the text (empty: any), with the reason.
    struct Allowance
    {
        Check check = Check::Count;
        std::string scope;
        std::string text;
        std::string reason;
    };

    constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

    struct Finding
    {
        Check check = Check::Count;
        Severity severity = Severity::Info;
        std::size_t text = kNone;  // the text (the group's first line for a paragraph)
        std::size_t other = kNone; // the other text (Overlap)
        std::size_t box = kNone;   // the box (Container)
        Rect a;                    // the text's rect (a paragraph: all its lines)
        Rect b;                    // the box, the other text, the screen or the key bar
        float amount = 0.0f;       // px past the edge, overlap size, capital height
        std::string allowed;       // the allowance's reason; empty: a real finding
    };

    // The rect of a text as seen: its ink inside its clip rect.
    inline Rect Visible(const TextItem& t) { return Intersect(t.ink, t.clip); }

    inline bool Matches(const Allowance& allow, Check check, const TextItem& text)
    {
        return allow.check == check && (allow.scope.empty() || text.scope.find(allow.scope) != std::string::npos) &&
            (allow.text.empty() || text.text.find(allow.text) != std::string::npos);
    }

    // The container of a text: the smallest box drawn before it that holds
    // the point at the middle of its left edge (kNone: none).
    inline std::size_t ContainerOf(const Frame& frame, const TextItem& text)
    {
        const Rect ink = Visible(text);
        if (Empty(ink))
            return kNone;
        const float x = ink.x0 + std::min(1.0f, Width(ink) * 0.5f);
        const float y = (ink.y0 + ink.y1) * 0.5f;
        std::size_t best = kNone;
        float bestArea = std::numeric_limits<float>::max();
        for (std::size_t n = 0; n < frame.boxes.size(); ++n)
        {
            const Box& box = frame.boxes[n];
            if (!box.container || box.order > text.order || !ContainsPoint(box.rect, x, y))
                continue;
            const float area = Area(box.rect);
            if (area > 0.0f && area < bestArea)
            {
                bestArea = area;
                best = n;
            }
        }
        return best;
    }

    inline std::vector<Finding> Analyze(const Frame& frame, const Options& options, const std::vector<Allowance>& allowances = {})
    {
        std::vector<Finding> findings;
        const Rect screen{0.0f, 0.0f, frame.width, frame.height};
        const std::size_t count = frame.texts.size();
        std::vector<Rect> visible(count);
        for (std::size_t n = 0; n < count; ++n)
            visible[n] = Visible(frame.texts[n]);

        // Paragraphs: their lines' union, checked once from the first line.
        std::vector<std::size_t> head(count, kNone);
        std::vector<Rect> groupRect(count);
        for (std::size_t n = 0; n < count; ++n)
        {
            const int group = frame.texts[n].group;
            head[n] = n;
            if (group < 0)
                continue;
            for (std::size_t m = 0; m < n; ++m)
                if (frame.texts[m].group == group)
                {
                    head[n] = m;
                    break;
                }
        }
        for (std::size_t n = 0; n < count; ++n)
            groupRect[head[n]] = Union(groupRect[head[n]], visible[n]);

        for (std::size_t n = 0; n < count; ++n)
        {
            const TextItem& t = frame.texts[n];
            if (t.text.find_first_not_of(' ') == std::string::npos || Empty(t.ink))
                continue;
            // Past the screen: the ink before the window's clip (the screen's),
            // or what a clip rect inside the screen leaves of it.
            const bool screenClip = Contains(t.clip, screen, 0.5f);
            const Rect seen = screenClip ? t.ink : visible[n];
            const float out = Overflow(screen, seen);
            if (out > 0.5f)
                findings.push_back({Check::Viewport, Severity::Error, n, kNone, kNone, seen, screen, out, {}});
            // Cut by a clip rect inside the screen.
            if (!screenClip && Overflow(t.clip, t.ink) > options.tolerance)
                findings.push_back({Check::Clipped, Severity::Info, n, kNone, kNone, t.ink, t.clip, Overflow(t.clip, t.ink), {}});
            // Too small.
            if (t.capPx > 0.0f && t.capPx < options.minCapWarning)
                findings.push_back({Check::MinHeight, t.capPx < options.minCapError ? Severity::Error : Severity::Warning, n, kNone, kNone,
                    t.ink, Rect{}, t.capPx, {}});
            // Under the key bar.
            if (!t.keyBar && !Empty(frame.keyBar))
            {
                const Rect under = Intersect(visible[n], frame.keyBar);
                if (Width(under) > options.overlapMin && Height(under) > options.overlapMin)
                    findings.push_back({Check::KeyBar, Severity::Error, n, kNone, kNone, visible[n], frame.keyBar, Height(under), {}});
            }
            // Out of its box (a paragraph once, all its lines against the first
            // line's box), and out of the panel its screen part draws it in.
            if (head[n] == n)
            {
                const Rect all = groupRect[n];
                const std::size_t box = ContainerOf(frame, t);
                bool reported = false;
                if (box != kNone)
                {
                    const float spill = Overflow(frame.boxes[box].rect, all);
                    if (spill > options.tolerance)
                    {
                        findings.push_back({Check::Container, spill > options.spillError ? Severity::Error : Severity::Warning, n, kNone, box, all,
                            frame.boxes[box].rect, spill, {}});
                        reported = true;
                    }
                }
                if (!reported && t.panel < frame.boxes.size())
                {
                    const float spill = Overflow(frame.boxes[t.panel].rect, all);
                    if (spill > options.tolerance)
                    {
                        findings.push_back({Check::Container, spill > options.spillError ? Severity::Error : Severity::Warning, n, kNone, t.panel,
                            all, frame.boxes[t.panel].rect, spill, {}});
                        reported = true;
                    }
                }
                // On the panel's ragged edge band (a title on its red brush,
                // drawn over the edge on purpose, is not).
                const bool onBrush = box != kNone && frame.boxes[box].kind == "brush";
                if (!reported && !onBrush && t.panel < frame.boxes.size() && !Empty(frame.boxes[t.panel].inner))
                {
                    const float into = Overflow(frame.boxes[t.panel].inner, all);
                    if (into > options.tolerance)
                        findings.push_back({Check::EdgeBand, Severity::Error, n, kNone, t.panel, all, frame.boxes[t.panel].inner, into, {}});
                }
            }
        }

        // Over each other (not two lines of one paragraph, not a drop shadow).
        for (std::size_t a = 0; a < count; ++a)
        {
            const TextItem& ta = frame.texts[a];
            if (Empty(visible[a]) || ta.text.find_first_not_of(' ') == std::string::npos)
                continue;
            for (std::size_t b = a + 1; b < count; ++b)
            {
                const TextItem& tb = frame.texts[b];
                if (Empty(visible[b]) || tb.text.find_first_not_of(' ') == std::string::npos)
                    continue;
                if (ta.group >= 0 && ta.group == tb.group)
                    continue;
                if (ta.text == tb.text && std::abs(ta.ink.x0 - tb.ink.x0) <= options.shadowOffset &&
                    std::abs(ta.ink.y0 - tb.ink.y0) <= options.shadowOffset)
                    continue;
                const Rect both = Intersect(visible[a], visible[b]);
                if (Width(both) > options.overlapMin && Height(both) > options.overlapMin)
                {
                    findings.push_back({Check::Overlap, Severity::Error, a, b, kNone, visible[a], visible[b], std::min(Width(both), Height(both)), {}});
                    continue;
                }
                // Crowded: side by side on one line (most of the shorter one's
                // height shared), a gap of less than a fraction of a capital.
                const float shared = std::min(visible[a].y1, visible[b].y1) - std::max(visible[a].y0, visible[b].y0);
                const float shorter = std::min(Height(visible[a]), Height(visible[b]));
                if (shorter <= 0.0f || shared < shorter * 0.5f)
                    continue;
                const float gap = std::max(visible[a].x0, visible[b].x0) - std::min(visible[a].x1, visible[b].x1);
                const float cap = std::max(std::min(ta.capPx, tb.capPx), 1.0f);
                if (gap >= -options.overlapMin && gap < cap * options.crowdedGap)
                    findings.push_back({Check::Crowded, Severity::Warning, a, b, kNone, visible[a], visible[b], std::max(gap, 0.0f), {}});
            }
        }

        // The loading cues: the turning cylinder stands beside its caption,
        // never over a text, inside the screen and above the key bar. A cue
        // with no text near it is a spinner standing alone (the far seek's
        // dots at the right edge). The finding names the nearest text.
        for (std::size_t c = 0; c < frame.boxes.size(); ++c)
        {
            const Box& cueBox = frame.boxes[c];
            if (cueBox.kind != "cue" || Empty(cueBox.rect))
                continue;
            const Rect cue = cueBox.rect;
            std::size_t nearest = kNone;
            float nearestGap = std::numeric_limits<float>::max();
            for (std::size_t n = 0; n < count; ++n)
            {
                const TextItem& t = frame.texts[n];
                if (Empty(visible[n]) || t.keyBar || t.text.find_first_not_of(' ') == std::string::npos)
                    continue;
                const Rect over = Intersect(visible[n], cue);
                if (Width(over) > options.overlapMin && Height(over) > options.overlapMin)
                    findings.push_back({Check::Cue, Severity::Error, n, kNone, c, cue, visible[n], std::min(Width(over), Height(over)), {}});
                const float dx = std::max(0.0f, std::max(visible[n].x0 - cue.x1, cue.x0 - visible[n].x1));
                const float dy = std::max(0.0f, std::max(visible[n].y0 - cue.y1, cue.y0 - visible[n].y1));
                const float gap = std::sqrt(dx * dx + dy * dy);
                if (gap < nearestGap)
                {
                    nearestGap = gap;
                    nearest = n;
                }
            }
            if (nearest == kNone)
                continue; // nothing to name (no frame of the editor draws a cue without a text)
            const float out = Overflow(screen, cue);
            if (out > 0.5f)
                findings.push_back({Check::Cue, Severity::Error, nearest, kNone, c, cue, screen, out, {}});
            if (!Empty(frame.keyBar))
            {
                const Rect under = Intersect(cue, frame.keyBar);
                if (Width(under) > options.overlapMin && Height(under) > options.overlapMin)
                    findings.push_back({Check::Cue, Severity::Error, nearest, kNone, c, cue, frame.keyBar, Height(under), {}});
            }
            const float size = std::max(Width(cue), Height(cue));
            if (nearestGap > size * options.cueReach)
                findings.push_back({Check::Cue, Severity::Error, nearest, kNone, c, cue, visible[nearest], nearestGap, {}});
        }

        for (Finding& f : findings)
        {
            for (const Allowance& allow : allowances)
            {
                const bool first = Matches(allow, f.check, frame.texts[f.text]);
                const bool second = f.other != kNone && Matches(allow, f.check, frame.texts[f.other]);
                if (first || second)
                {
                    f.allowed = allow.reason;
                    break;
                }
            }
            if (f.check == Check::Clipped && f.allowed.empty())
                f.allowed = "clipped on purpose: a clip rect the editor set, or a text box that scrolls";
        }
        return findings;
    }

    // The findings that fail a run: errors not allowed by design.
    inline std::size_t FailingCount(const std::vector<Finding>& findings)
    {
        std::size_t failing = 0;
        for (const Finding& f : findings)
            failing += f.severity == Severity::Error && f.allowed.empty() ? 1u : 0u;
        return failing;
    }
}
