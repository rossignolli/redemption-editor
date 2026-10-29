// The editor's layout maths and the UI harness's checks
// (src/ui_layout_check.h): the rect maths, the key bar's row packing and
// the label fit, and each check of Analyze on frames built by hand.
#include "ui_layout_check.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    using namespace replay::editor::uilayout;

    int g_failures = 0;

    void Expect(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    bool Near(float a, float b) { return a > b - 0.01f && a < b + 0.01f; }

    TextItem Text(const char* text, Rect ink, int order, int group = -1, const char* scope = "Screen")
    {
        TextItem t;
        t.text = text;
        t.ink = ink;
        t.clip = Rect{0.0f, 0.0f, 1920.0f, 1080.0f};
        t.px = 24.0f;
        t.capPx = 12.0f;
        t.group = group;
        t.order = order;
        t.scope = scope;
        t.font = "title";
        return t;
    }

    Box Panel(Rect rect, int order, bool container = true)
    {
        Box b;
        b.kind = "fill";
        b.rect = rect;
        b.order = order;
        b.container = container;
        return b;
    }

    Frame Screen()
    {
        Frame f;
        f.width = 1920.0f;
        f.height = 1080.0f;
        return f;
    }

    std::size_t CountOf(const std::vector<Finding>& findings, Check check, Severity severity)
    {
        std::size_t n = 0;
        for (const Finding& f : findings)
            n += f.check == check && f.severity == severity ? 1u : 0u;
        return n;
    }

    std::size_t CountOf(const std::vector<Finding>& findings, Check check)
    {
        std::size_t n = 0;
        for (const Finding& f : findings)
            n += f.check == check ? 1u : 0u;
        return n;
    }

    void TestRects()
    {
        const Rect a{0.0f, 0.0f, 100.0f, 50.0f};
        const Rect b{80.0f, 40.0f, 120.0f, 70.0f};
        const Rect i = Intersect(a, b);
        Expect(Near(i.x0, 80.0f) && Near(i.y0, 40.0f) && Near(i.x1, 100.0f) && Near(i.y1, 50.0f), "intersection");
        Expect(Empty(Intersect(a, Rect{200.0f, 0.0f, 300.0f, 10.0f})), "apart: empty intersection");
        const Rect u = Union(Rect{}, b);
        Expect(Near(u.x0, 80.0f) && Near(u.x1, 120.0f), "an empty rect leaves the union alone");
        Expect(Near(Area(a), 5000.0f) && Near(Area(Rect{5.0f, 5.0f, 4.0f, 9.0f}), 0.0f), "area, an inverted rect has none");
        Expect(Near(Overflow(a, Rect{10.0f, 10.0f, 90.0f, 40.0f}), 0.0f), "inside: no overflow");
        Expect(Near(Overflow(a, Rect{10.0f, 10.0f, 107.0f, 40.0f}), 7.0f), "7 past the right edge");
        Expect(Near(Overflow(a, Rect{-3.0f, 10.0f, 90.0f, 52.0f}), 3.0f), "the largest side counts");
        Expect(Contains(a, Rect{0.0f, 0.0f, 100.5f, 50.0f}, 1.0f) && !Contains(a, Rect{0.0f, 0.0f, 101.5f, 50.0f}, 1.0f), "the tolerance");
        Expect(ContainsPoint(a, 100.0f, 50.0f) && !ContainsPoint(a, 100.1f, 50.0f), "a point on the edge is in");
    }

    // The key bar's packing as an inline reference.
    std::vector<std::size_t> OldKeyBarRows(const std::vector<float>& widths, float room)
    {
        std::vector<std::size_t> starts;
        float used = 0.0f;
        for (std::size_t n = 0; n < widths.size(); ++n)
        {
            const float width = widths[n];
            if (starts.empty() || used + width > room)
            {
                starts.push_back(n);
                used = 0.0f;
            }
            used += width;
        }
        return starts;
    }

    void TestPackRows()
    {
        Expect(PackRows({}, 100.0f).empty(), "no tips, no row");
        Expect(PackRows({30.0f, 30.0f, 40.0f}, 100.0f) == std::vector<std::size_t>{0}, "an exact fit is one row");
        Expect(PackRows({30.0f, 30.0f, 41.0f}, 100.0f) == (std::vector<std::size_t>{0, 2}), "one past the room: a second row");
        Expect(PackRows({250.0f, 10.0f}, 100.0f) == (std::vector<std::size_t>{0, 1}), "a tip wider than the room has its row");
        Expect(PackRows({10.0f, 250.0f, 10.0f}, 100.0f) == (std::vector<std::size_t>{0, 1, 2}), "and the next starts another");
        std::uint32_t seed = 12345u;
        bool same = true;
        for (int round = 0; round < 200; ++round)
        {
            std::vector<float> widths;
            const int count = static_cast<int>(seed % 40u);
            for (int n = 0; n < count; ++n)
            {
                seed = seed * 1664525u + 1013904223u;
                widths.push_back(40.0f + static_cast<float>(seed % 260u));
            }
            seed = seed * 1664525u + 1013904223u;
            const float room = 600.0f + static_cast<float>(seed % 2400u);
            same = same && PackRows(widths, room) == OldKeyBarRows(widths, room);
        }
        Expect(same, "PackRows is the key bar's old packing (200 random bars)");
    }

    void TestShrinkToFit()
    {
        // 10 px a size unit: 16 is 160 wide.
        const auto width = [](float size) { return size * 10.0f; };
        Expect(Near(ShrinkToFit(16.0f, 12.0f, 200.0f, width), 16.0f), "fits: the size stays");
        Expect(Near(ShrinkToFit(16.0f, 12.0f, 140.0f, width), 14.0f), "stepped down until it fits");
        Expect(Near(ShrinkToFit(16.0f, 12.0f, 50.0f, width), 12.0f), "never under the minimum (still too wide there)");
        // A plain loop over the same widths, as the reference.
        for (float room = 50.0f; room < 220.0f; room += 7.0f)
        {
            float size = 16.0f;
            while (size > 12.0f && width(size) > room)
                size -= 1.0f;
            Expect(Near(size, ShrinkToFit(16.0f, 12.0f, room, width)), "ShrinkToFit is MenuRowDraw's old loop");
        }
    }

    void TestEllipsize()
    {
        // 10 units a character, "..." is 30.
        const auto width = [](const std::string& s) { return static_cast<float>(s.size()) * 10.0f; };
        Expect(Ellipsize("Arthur", 60.0f, width) == "Arthur", "fits: the whole text");
        Expect(Ellipsize("Arthur Morgan (player)", 100.0f, width) == "Arthur...", "cut, the trailing space dropped, then ...");
        Expect(Ellipsize("Arthur Morgan (player)", 101.0f, width).size() * 10 <= 101, "never wider than the room");
        Expect(Ellipsize("Arthur", 20.0f, width) == "...", "no room for a letter: ... alone");
        Expect(Ellipsize("project-20261024-170346.rpproj", 150.0f, width) == "project-2026...", "no space needed to cut");
    }

    void TestViewportAndClip()
    {
        Frame f = Screen();
        f.texts.push_back(Text("PAST THE EDGE", Rect{1850.0f, 10.0f, 1990.0f, 30.0f}, 0));
        TextItem clipped = Text("A LONG CLIP NAME IN A BLOCK", Rect{100.0f, 900.0f, 400.0f, 920.0f}, 1);
        clipped.clip = Rect{90.0f, 880.0f, 200.0f, 960.0f};
        f.texts.push_back(clipped);
        const std::vector<Finding> found = Analyze(f, Options{});
        Expect(CountOf(found, Check::Viewport, Severity::Error) == 1, "text past the screen's edge");
        for (const Finding& finding : found)
            if (finding.check == Check::Viewport)
                Expect(Near(finding.amount, 70.0f), "by 70 px");
        Expect(CountOf(found, Check::Clipped, Severity::Info) == 1, "a clip rect cuts the text: info");
        for (const Finding& finding : found)
            if (finding.check == Check::Clipped)
                Expect(!finding.allowed.empty(), "clipped on purpose: allowed");
        Expect(CountOf(found, Check::Clipped) == 1, "the screen's own clip is not a clipped finding");
        Expect(FailingCount(found) == 1, "only the viewport fails");

        // Past the screen's edge but cut inside it by a clip rect (a block of the
        // project's timeline): what shows is in the screen.
        Frame g = Screen();
        TextItem inBlock = Text("A NAME PAST THE EDGE", Rect{1800.0f, 900.0f, 2100.0f, 920.0f}, 0);
        inBlock.clip = Rect{1700.0f, 880.0f, 1900.0f, 960.0f};
        g.texts.push_back(inBlock);
        const std::vector<Finding> cut = Analyze(g, Options{});
        Expect(CountOf(cut, Check::Viewport) == 0 && CountOf(cut, Check::Clipped) == 1, "a clip inside the screen: clipped, not past the screen");
    }

    void TestContainer()
    {
        Frame f = Screen();
        f.boxes.push_back(Panel(Rect{0.0f, 0.0f, 1920.0f, 1080.0f}, 0)); // a veil
        f.boxes.push_back(Panel(Rect{100.0f, 100.0f, 500.0f, 300.0f}, 1)); // the panel
        f.texts.push_back(Text("FITS", Rect{120.0f, 120.0f, 480.0f, 140.0f}, 2));
        f.texts.push_back(Text("RUNS OUT OF THE PANEL", Rect{120.0f, 160.0f, 530.0f, 180.0f}, 3));
        f.texts.push_back(Text("A HAIR OVER", Rect{120.0f, 200.0f, 501.5f, 220.0f}, 4));
        f.boxes.push_back(Panel(Rect{110.0f, 230.0f, 140.0f, 250.0f}, 9)); // drawn after the next text
        f.texts.push_back(Text("AFTER", Rect{120.0f, 235.0f, 300.0f, 245.0f}, 5));
        f.boxes.push_back(Panel(Rect{600.0f, 100.0f, 700.0f, 130.0f}, 6, false)); // a mouse area only
        f.texts.push_back(Text("OVER A MOUSE AREA", Rect{610.0f, 105.0f, 900.0f, 125.0f}, 7));
        const std::vector<Finding> found = Analyze(f, Options{});
        Expect(CountOf(found, Check::Container, Severity::Error) == 1, "the text past the panel is an error");
        Expect(CountOf(found, Check::Container, Severity::Warning) == 1, "1.5 px over: a warning");
        for (const Finding& finding : found)
            if (finding.check == Check::Container && finding.severity == Severity::Error)
            {
                Expect(f.texts[finding.text].text == "RUNS OUT OF THE PANEL" && finding.box == 1, "the smallest box holding its start");
                Expect(Near(finding.amount, 30.0f), "30 px past it");
            }
        Expect(ContainerOf(f, f.texts[3]) == 1, "a box drawn after the text is not its container");
        Expect(ContainerOf(f, f.texts[4]) == 0, "a mouse area is not a container: the veil is");
    }

    void TestParagraph()
    {
        Frame f = Screen();
        f.boxes.push_back(Panel(Rect{100.0f, 100.0f, 500.0f, 200.0f}, 0));
        f.texts.push_back(Text("The first line of the help", Rect{120.0f, 150.0f, 480.0f, 170.0f}, 1, 0));
        f.texts.push_back(Text("and its second line", Rect{120.0f, 172.0f, 400.0f, 192.0f}, 2, 0));
        f.texts.push_back(Text("and a third below the panel", Rect{120.0f, 194.0f, 420.0f, 214.0f}, 3, 0));
        const std::vector<Finding> found = Analyze(f, Options{});
        Expect(CountOf(found, Check::Container) == 1, "a paragraph is checked once");
        for (const Finding& finding : found)
            if (finding.check == Check::Container)
                Expect(finding.text == 0 && Near(finding.a.y1, 214.0f) && Near(finding.amount, 14.0f), "all its lines against the first line's box");
        Expect(CountOf(found, Check::Overlap) == 0, "its own lines never overlap");
    }

    void TestOverlap()
    {
        Frame f = Screen();
        f.texts.push_back(Text("LABEL", Rect{100.0f, 100.0f, 300.0f, 120.0f}, 0));
        f.texts.push_back(Text("< VALUE >", Rect{280.0f, 102.0f, 400.0f, 118.0f}, 1));
        f.texts.push_back(Text("CAM", Rect{500.0f, 100.0f, 540.0f, 112.0f}, 2));
        f.texts.push_back(Text("CAM", Rect{501.0f, 101.0f, 541.0f, 113.0f}, 3)); // its drop shadow
        f.texts.push_back(Text("TOUCHING", Rect{400.5f, 102.0f, 480.0f, 118.0f}, 4)); // 0.5 px into the value
        const std::vector<Finding> found = Analyze(f, Options{});
        Expect(CountOf(found, Check::Overlap, Severity::Error) == 1, "two texts over each other, once");
        for (const Finding& finding : found)
            if (finding.check == Check::Overlap)
                Expect(finding.text == 0 && finding.other == 1 && Near(finding.amount, 16.0f), "the label and the value, 20 x 16 px");
    }

    void TestKeyBarAndHeight()
    {
        Frame f = Screen();
        f.keyBar = Rect{0.0f, 1030.0f, 1920.0f, 1080.0f};
        f.texts.push_back(Text("UNDER THE BAR", Rect{100.0f, 1020.0f, 300.0f, 1040.0f}, 0));
        TextItem tip = Text("SELECT", Rect{1500.0f, 1050.0f, 1560.0f, 1062.0f}, 1, -1, "KeyBar");
        tip.keyBar = true;
        f.texts.push_back(tip);
        TextItem tiny = Text("TINY", Rect{100.0f, 500.0f, 140.0f, 505.0f}, 2);
        tiny.capPx = 5.0f;
        TextItem small = Text("SMALL", Rect{100.0f, 600.0f, 140.0f, 607.0f}, 3);
        small.capPx = 6.5f;
        f.texts.push_back(tiny);
        f.texts.push_back(small);
        f.texts.push_back(Text("   ", Rect{100.0f, 700.0f, 140.0f, 720.0f}, 4)); // blank: never checked
        const std::vector<Finding> found = Analyze(f, Options{});
        Expect(CountOf(found, Check::KeyBar, Severity::Error) == 1, "a screen's text under the key bar");
        for (const Finding& finding : found)
            if (finding.check == Check::KeyBar)
                Expect(finding.text == 0 && Near(finding.amount, 10.0f), "10 px under it; the bar's own tips are not");
        Expect(CountOf(found, Check::MinHeight, Severity::Error) == 1, "capitals 5 px high: an error");
        Expect(CountOf(found, Check::MinHeight, Severity::Warning) == 1, "6.5 px: a warning");
        Options strict;
        strict.minCapError = 7.0f;
        strict.minCapWarning = 8.0f;
        Expect(CountOf(Analyze(f, strict), Check::MinHeight, Severity::Error) == 2, "the thresholds are options");
    }

    void TestPanel()
    {
        // A help column running past its panel's foot: the rows below start
        // outside every box, the panel still holds them.
        Frame f = Screen();
        f.boxes.push_back(Panel(Rect{0.0f, 0.0f, 1920.0f, 1080.0f}, 0)); // the dimmed screen
        Box panel = Panel(Rect{300.0f, 60.0f, 1040.0f, 780.0f}, 1);
        panel.kind = "panel";
        f.boxes.push_back(panel);
        TextItem in = Text("F2", Rect{660.0f, 700.0f, 700.0f, 715.0f}, 2);
        TextItem below = Text("SHIFT+F4", Rect{660.0f, 850.0f, 760.0f, 865.0f}, 3);
        TextItem free = Text("NOT IN THE PANEL", Rect{20.0f, 900.0f, 200.0f, 915.0f}, 4);
        in.panel = 1;
        below.panel = 1;
        f.texts = {in, below, free};
        const std::vector<Finding> found = Analyze(f, Options{});
        Expect(CountOf(found, Check::Container, Severity::Error) == 1, "a row below its panel");
        for (const Finding& finding : found)
            if (finding.check == Check::Container)
                Expect(finding.text == 1 && finding.box == 1 && Near(finding.amount, 85.0f), "85 px under the panel's foot");
    }

    void TestCrowded()
    {
        Frame f = Screen();
        f.texts.push_back(Text("LOOK AT TARGET", Rect{100.0f, 100.0f, 300.0f, 112.0f}, 0));
        f.texts.push_back(Text("< ARTHUR >", Rect{302.0f, 100.0f, 400.0f, 112.0f}, 1)); // 2 px apart, capitals 12 high
        f.texts.push_back(Text("FAR", Rect{460.0f, 100.0f, 500.0f, 112.0f}, 2));
        f.texts.push_back(Text("NEXT LINE", Rect{100.0f, 113.0f, 300.0f, 125.0f}, 3)); // under, not beside
        const std::vector<Finding> found = Analyze(f, Options{});
        Expect(CountOf(found, Check::Crowded, Severity::Warning) == 1, "two texts of a line 2 px apart: crowded");
        for (const Finding& finding : found)
            if (finding.check == Check::Crowded)
                Expect(finding.text == 0 && finding.other == 1 && Near(finding.amount, 2.0f), "the label and its value");
        Expect(FailingCount(found) == 0, "crowded is a warning");
    }

    void TestEdgeBand()
    {
        // A painted panel: its body 16 px inside the ragged edge.
        Frame f = Screen();
        Box panel = Panel(Rect{100.0f, 100.0f, 500.0f, 300.0f}, 0);
        panel.kind = "panel";
        panel.inner = Rect{116.0f, 116.0f, 484.0f, 284.0f};
        f.boxes.push_back(panel);
        Box brush = Panel(Rect{100.0f, 110.0f, 300.0f, 150.0f}, 1);
        brush.kind = "brush";
        f.boxes.push_back(brush);
        TextItem body = Text("IN THE BODY", Rect{130.0f, 200.0f, 400.0f, 215.0f}, 2);
        TextItem band = Text("ON THE EDGE", Rect{108.0f, 230.0f, 300.0f, 245.0f}, 3);
        TextItem title = Text("ON ITS BRUSH", Rect{108.0f, 120.0f, 280.0f, 140.0f}, 4);
        body.panel = band.panel = title.panel = 0;
        f.texts = {body, band, title};
        const std::vector<Finding> found = Analyze(f, Options{});
        Expect(CountOf(found, Check::EdgeBand, Severity::Error) == 1, "a text on the ragged band");
        for (const Finding& finding : found)
            if (finding.check == Check::EdgeBand)
                Expect(finding.text == 1 && Near(finding.amount, 8.0f), "8 px into the band; the title on its brush is not");
        f.boxes[0].inner = Rect{};
        Expect(CountOf(Analyze(f, Options{}), Check::EdgeBand) == 0, "a flat panel has no band");
    }

    void TestAllowances()
    {
        Frame f = Screen();
        f.texts.push_back(Text("Event 4  t=1.00 Shake Amp=1.50 snap", Rect{100.0f, 100.0f, 300.0f, 120.0f}, 0, -1, "Edit/EditTimeline"));
        f.texts.push_back(Text("Paused", Rect{280.0f, 102.0f, 400.0f, 118.0f}, 1, -1, "Edit/EditTimeline"));
        std::vector<Allowance> allow = {{Check::Overlap, "EditTimeline", "Event ", "the hovered event's label sits over the lane by design"}};
        std::vector<Finding> found = Analyze(f, Options{}, allow);
        Expect(CountOf(found, Check::Overlap) == 1 && !found[0].allowed.empty(), "an allowance marks the finding with its reason");
        Expect(FailingCount(found) == 0, "an allowed error does not fail the run");
        allow[0].scope = "Gallery";
        found = Analyze(f, Options{}, allow);
        Expect(found.size() == 1 && found[0].allowed.empty() && FailingCount(found) == 1, "the allowance's scope must match");
        Expect(std::string(CheckName(Check::KeyBar)) == "keybar" && std::string(SeverityName(Severity::Warning)) == "warning", "the report's names");
    }
}

int RunUiLayoutTests()
{
    TestRects();
    TestPackRows();
    TestShrinkToFit();
    TestEllipsize();
    TestViewportAndClip();
    TestContainer();
    TestParagraph();
    TestOverlap();
    TestKeyBarAndHeight();
    TestPanel();
    TestCrowded();
    TestEdgeBand();
    TestAllowances();
    if (g_failures == 0)
        std::printf("ui layout tests passed\n");
    return g_failures;
}
