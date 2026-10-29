#pragma once

// Projects: an ordered list of clips, as a replay editor's montage. Each one
// names a clip file of ReplayClips (by file name) and keeps its own trim and
// its own edit markers, which live in the project and not in the clip file.
//
// The trim is the first and last marker: `inMs` and
// `outMs` mirror them. A clip added to a project and never opened has no
// markers yet (hasMarkers false): the first time it opens in playback it
// takes the clip file's own markers fitted to its trim (FitMarkers), so the
// camera work done in the clip editor carries over.
//
// Everything here is pure (no engine, no globals). The script thread owns the open project
// (script.cpp, "Projects"); the editor shows it from the snapshot.

#include "camera_track.h"
#include "env_override.h"
#include "project_name.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace replay::project
{
    // "RPRJ" in the file's first four bytes.
    constexpr std::uint32_t kMagic = 0x4A525052u;
    // 1: name, clips (file, raw duration, trim, markers).
    constexpr std::uint32_t kFormatVersion = 1;
    constexpr std::uint32_t kMinFormatVersion = 1;
    // At most 512 clips and 30 minutes per project.
    constexpr std::size_t kMaxClips = 512;
    constexpr std::uint64_t kMaxDurationMs = 30ull * 60ull * 1000ull;
    // The shortest trimmed clip: two marker frames, where a new clip's end
    // marker goes. Also the widest marker spacing (the
    // 200 % speed), so the start and end markers always fit.
    constexpr std::uint64_t kMinClipMs = 2ull * camera::kMarkerFrameMs;
    // The longest clip file a project names: a clip's timestamps are u32 ms
    // (clip_file.cpp ValidateLoaded). A project file naming a longer one is
    // refused, so its total (at most kMaxClips of these) never wraps.
    constexpr std::uint64_t kMaxClipRawMs = 0xFFFFFFFFull;
    // The largest project file LoadFile reads: above kMaxClips clips of
    // camera::kMaxMarkers markers each (project_tests.cpp proves it); a real
    // project is a few KB.
    constexpr std::uint64_t kMaxFileBytes = 64ull * 1024ull * 1024ull;

    struct Clip
    {
        std::string file;              // "042-Valentine.rpclip", a file of ReplayClips
        std::uint64_t rawDurationMs = 0; // the clip file's whole length
        std::uint64_t inMs = 0;        // the trim, in the clip's own time
        std::uint64_t outMs = 0;
        bool hasMarkers = false;       // false: the clip's own markers on first open
        camera::Track markers;         // hasMarkers: first at inMs, last at outMs
        // The clip's environment settings in
        // the project (the clip file's own on first open, as the markers).
        bool hasEnvironment = false;
        EnvironmentOverride environment;

        std::uint64_t DurationMs() const { return outMs > inMs ? outMs - inMs : 0; }
    };

    struct Project
    {
        std::string name;
        std::vector<Clip> clips;
    };

    // A plain name: letters, digits, spaces, - and _, not empty, at most
    // kMaxNameBytes, no leading or trailing space.
    bool ValidName(const std::string& name);

    // CleanName, CheckRename and the other rename rules: project_name.h.

    // A clip file whole (in 0, out its length), no markers yet. False when
    // the length is under kMinClipMs or the name is not a plain file name.
    bool MakeClip(const std::string& file, std::uint64_t rawDurationMs, Clip& out);

    // ---- Edits. Each returns false and
    // leaves the project as it was when the edit is refused; `why` (may be
    // null) then says why, in the editor's words.
    // Inserts at `index` (clamped to the end). Refused past kMaxClips or
    // when the project would pass kMaxDurationMs.
    bool AddClip(Project& project, std::size_t index, const Clip& clip, std::string* why = nullptr);
    // The clip at `from` to the place `to` (picked up and dropped there).
    bool MoveClip(Project& project, std::size_t from, std::size_t to, std::string* why = nullptr);
    // A copy right after it.
    bool DuplicateClip(Project& project, std::size_t index, std::string* why = nullptr);
    bool DeleteClip(Project& project, std::size_t index, std::string* why = nullptr);
    // Cuts the clip in two at `clipMs` (its own time): the first keeps
    // [in, clipMs], a new one after it [clipMs, out], both of the same file.
    // Each piece must last kMinClipMs. Their markers: FitMarkers of the
    // clip's markers to each piece.
    bool SplitClip(Project& project, std::size_t index, std::uint64_t clipMs, std::string* why = nullptr);
    // A new trim, clamped to the clip file (0 .. raw) and kept kMinClipMs
    // long; the markers follow (FitMarkers). False when nothing changed.
    bool SetTrim(Project& project, std::size_t index, std::uint64_t inMs, std::uint64_t outMs, std::string* why = nullptr);

    // ---- Markers.
    // The markers of `source` that fall in [inMs, outMs], with a start
    // marker at inMs and an end marker at outMs:
    //  - a marker already at the time is kept as the bound;
    //  - otherwise the start is a copy of the marker in effect at inMs (the
    //    camera goes on from the cut), or of the first marker when none is
    //    before it; the end a copy of the marker in effect at outMs, or of
    //    the last marker when it lies past outMs and is the source's end;
    //  - with no markers at all, `startTemplate` and `endTemplate` (the
    //    caller's Game Camera bounds) at those times.
    // A fade (transition) stays only on a copy of the source's own first
    // (start) or last (end) marker. Markers inside closer to a bound than
    // the spacing allows are dropped. A legacy (v2) source counts as empty.
    // Then the last marker rule. inMs < outMs.
    camera::Track FitMarkers(const camera::Track& source, std::uint64_t inMs, std::uint64_t outMs,
        const camera::Keyframe& startTemplate = {}, const camera::Keyframe& endTemplate = {});
    // The clip's trim from its markers (first and last) after an edit of
    // them; false (nothing changed) when they cannot be the trim (fewer than
    // two, legacy, past the clip file, shorter than kMinClipMs).
    bool TakeMarkers(Clip& clip, const camera::Track& markers);

    // ---- Timeline (the project's time is the sum of the trimmed clips;
    // marker speeds do not stretch it).
    std::uint64_t TotalMs(const Project& project);
    std::uint64_t ClipStartMs(const Project& project, std::size_t index);
    // The clip and its own time at a project time (past the end: the last
    // clip's out). False for an empty project.
    bool Locate(const Project& project, std::uint64_t projectMs, std::size_t& index, std::uint64_t& clipMs);
    // A clip's own time on the project timeline (clamped to its trim).
    std::uint64_t ToProjectMs(const Project& project, std::size_t index, std::uint64_t clipMs);

    // ---- File (ReplayProjects\<name>.rpproj), little endian:
    //   u32 magic, u32 format version, u16 marker version (the clip file's
    //   camera section version the markers are written in), u16 reserved,
    //   u64 total ms (cached for the list), u16 name bytes + name,
    //   u32 clip count, per clip: u32 entry bytes, then u16 file bytes +
    //   file, u64 raw ms, u64 in, u64 out, u8 has markers, u64 marker bytes
    //   + markers, then (appended in format 1: an older build skips it)
    //   u8 has environment + the bytes of env_override.h Pack (12 or 24 with
    //   the sun and the moon); and last the FNV-1a 64 of everything before it.
    // The entry size lets a later version append fields to a clip.
    std::vector<std::uint8_t> Encode(const Project& project);
    // On failure `project` is left as it was and `error` says why.
    bool Decode(const std::uint8_t* data, std::size_t size, Project& project, std::string& error);
    // Written through a temporary file renamed over the target (a failed
    // save never leaves half a project).
    bool SaveFile(const std::wstring& path, const Project& project, std::string& error);
    // A file over `maxBytes` is refused before it is read (and while it is,
    // should it grow); running out of memory is a refused open with an error,
    // never an exception.
    bool LoadFile(const std::wstring& path, Project& project, std::string& error, std::uint64_t maxBytes = kMaxFileBytes);

    // What the project list shows, from the file's head (no markers read).
    struct Summary
    {
        std::string name;
        std::uint64_t totalMs = 0;
        std::uint32_t clipCount = 0;
        std::uint32_t formatVersion = 0;
        bool readable = false;
    };
    bool ReadSummary(const std::wstring& path, Summary& summary);

    // ---- Export hook (the render pipeline exports a whole project from
    // this list): the trimmed pieces in play
    // order, each a clip file, its in and out in the clip's own time, where
    // it starts on the project timeline, and its markers. `ownMarkers`
    // false: the clip was never opened in the project; the exporter loads
    // the clip file and takes FitMarkers(clip file's markers, in, out) as
    // the playback does on first open.
    struct ExportSegment
    {
        std::string file;
        std::uint64_t inMs = 0;
        std::uint64_t outMs = 0;
        std::uint64_t projectStartMs = 0;
        bool ownMarkers = false;
        camera::Track markers;
        // false: the clip file's own settings (ENVO), as playback.
        bool ownEnvironment = false;
        EnvironmentOverride environment;
    };
    std::vector<ExportSegment> ExportPlan(const Project& project);
}
