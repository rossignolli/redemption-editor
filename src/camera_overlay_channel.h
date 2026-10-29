#pragma once

// The camera markers drawn in the world (keyframe spheres, look arrows, path
// line and time labels), between ReplayProbe and the editor add-on. The probe
// builds the picture on the script thread (camera_overlay.h: the marker curve
// sampled in the world, projected with the camera it placed this tick) and the
// add-on draws it with ImGui after the export capture and the QA pictures, so
// it never lands in an exported video or a QA shot.
//
// Same pattern as export_channel.h: the probe creates the mapping (named per
// process), the add-on opens it and refuses another layout (`bytes`). Two
// frames: the probe writes the one not published, then publishes it; each
// frame carries a sequence that is odd while it is written, so the add-on
// drops a copy that changed under it and keeps drawing the last good one.
//
// Positions are on screen in units of half the picture's height: y -1..1
// bottom to top, x -aspect..aspect left to right (ProjectToScreen's x times
// the aspect), so the add-on scales them by its display's height alone.

#include <atomic>
#include <cstdint>

namespace replay::overlaychannel
{
    constexpr std::uint32_t kMagic = 0x31564F43; // "COV1"
    constexpr std::uint32_t kVersion = 1;
    constexpr std::uint32_t kMaxLines = 3072;
    constexpr std::uint32_t kMaxDots = 768;
    constexpr std::uint32_t kMaxLabels = 256;
    constexpr std::uint32_t kNone = 0xFFFFFFFFu;

    enum class LineKind : std::uint16_t
    {
        Path = 1,  // the camera's path between markers that blend
        Arrow = 2, // where a marker looks
        Trail = 3  // the recorded path of the free camera's target
    };

    enum class DotKind : std::uint16_t
    {
        Marker = 1,
        Camera = 2,    // where the marker camera is at the playhead (seen from the free camera)
        Target = 3,    // the free camera's target (follow, lock)
        TrailTick = 4  // a second on the target's trail
    };

    enum class LabelKind : std::uint16_t
    {
        PathTime = 1,  // value: clip time (ms) on the path
        TrailTime = 2  // value: clip time (ms) on the target's trail
    };

    // Line and dot flags.
    enum : std::uint16_t
    {
        FlagSpline = 1u << 0,    // a Spline segment (else Linear or Smooth)
        FlagPast = 1u << 1,      // before the playhead
        FlagCurrent = 1u << 2,   // the marker the playhead is on (or its segment)
        FlagEditing = 1u << 3,   // the marker in Edit Camera
        FlagAttached = 1u << 4,  // the marker rides an attach target
        FlagAnchor = 1u << 5,
        FlagNotFree = 1u << 6,   // Game Camera or a preset (no own pose)
        FlagClipBound = 1u << 7  // the clip's start or end marker
    };

    struct Line
    {
        float x0, y0, x1, y1;
        std::uint16_t kind;
        std::uint16_t flags;
        std::uint32_t id; // the marker (Arrow), the segment's source marker (Path), the target (Trail)
    };

    struct Dot
    {
        float x, y;
        float meters;     // distance from the camera (the add-on sizes the dot by it)
        std::uint16_t kind;
        std::uint16_t flags;
        std::uint32_t id;     // marker id or target id
        std::uint32_t timeMs; // the marker's time
    };

    struct Label
    {
        float x, y;
        std::uint32_t value;
        std::uint16_t kind;
        std::uint16_t flags;
    };

    enum : std::uint32_t
    {
        FrameDim = 1u << 0,   // the clip plays: drawn faint
        FrameHidden = 1u << 1 // nothing to draw (off, exporting, no clip)
    };

    struct Frame
    {
        std::atomic<std::uint64_t> sequence{0}; // odd while written
        std::uint32_t flags = 0;
        std::uint32_t lineCount = 0;
        std::uint32_t dotCount = 0;
        std::uint32_t labelCount = 0;
        std::uint32_t playheadMs = 0;
        std::uint32_t reserved = 0;
        Line lines[kMaxLines];
        Dot dots[kMaxDots];
        Label labels[kMaxLabels];
    };

    struct Channel
    {
        std::uint32_t magic = kMagic;
        std::uint32_t version = kVersion;
        std::uint32_t bytes = 0; // sizeof(Channel) of the probe that made it
        std::atomic<std::uint32_t> published{kNone}; // the frame to draw
        std::atomic<std::uint64_t> probeTicks{0};
        std::atomic<std::uint64_t> addonDrawn{0};  // add-on -> probe: frames drawn (alive when it moves)
        std::atomic<std::uint64_t> addonTorn{0};   // copies dropped because the probe wrote under them
        Frame frames[2];
    };

    // Local\ReplayCameraOverlay-<pid>.
    inline void Name(wchar_t (&out)[64], unsigned long processId)
    {
        const wchar_t prefix[] = L"Local\\ReplayCameraOverlay-";
        int n = 0;
        for (; prefix[n]; ++n)
            out[n] = prefix[n];
        wchar_t digits[16];
        int d = 0;
        do
        {
            digits[d++] = static_cast<wchar_t>(L'0' + processId % 10);
            processId /= 10;
        } while (processId && d < 15);
        while (d)
            out[n++] = digits[--d];
        out[n] = 0;
    }

    // Writer side (probe): the frame not published, marked as being written.
    inline Frame& BeginWrite(Channel& channel)
    {
        const std::uint32_t published = channel.published.load(std::memory_order_acquire);
        Frame& frame = channel.frames[published == 1 ? 0 : 1];
        frame.sequence.fetch_add(1, std::memory_order_acq_rel); // odd: being written
        std::atomic_thread_fence(std::memory_order_release);
        frame.flags = 0;
        frame.lineCount = frame.dotCount = frame.labelCount = 0;
        return frame;
    }

    inline void EndWrite(Channel& channel, Frame& frame)
    {
        std::atomic_thread_fence(std::memory_order_release);
        frame.sequence.fetch_add(1, std::memory_order_acq_rel); // even: done
        channel.published.store(&frame == &channel.frames[0] ? 0u : 1u, std::memory_order_release);
        channel.probeTicks.fetch_add(1, std::memory_order_relaxed);
    }

    inline bool AddLine(Frame& frame, const Line& line)
    {
        if (frame.lineCount >= kMaxLines)
            return false;
        frame.lines[frame.lineCount++] = line;
        return true;
    }

    inline bool AddDot(Frame& frame, const Dot& dot)
    {
        if (frame.dotCount >= kMaxDots)
            return false;
        frame.dots[frame.dotCount++] = dot;
        return true;
    }

    inline bool AddLabel(Frame& frame, const Label& label)
    {
        if (frame.labelCount >= kMaxLabels)
            return false;
        frame.labels[frame.labelCount++] = label;
        return true;
    }
}
