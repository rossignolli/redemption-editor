// Projects (src/replay_project.h), a montage of clips: the edits (add, move,
// duplicate, delete, split, trim) and their limits, the markers fitted to a
// trim (start and end markers bound every other one), the project timeline,
// the file's round trip (bytes and disk), its refusals (checksum, version,
// trim, truncation) and the export plan. Also camera::WithinClipBounds.
#include "replay_project.h"

#include "camera_track.h"
#include "clip_file.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    using replay::camera::Keyframe;
    using replay::camera::Track;
    namespace project = replay::project;

    int g_projectFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_projectFailures;
    }

    Keyframe Marker(std::uint64_t timeMs, replay::editor::CameraMode mode = replay::editor::CameraMode::Game)
    {
        Keyframe key;
        key.timeMs = timeMs;
        key.cameraMode = mode;
        key.pose.position = {1.0f + timeMs * 0.001f, 2.0f, 3.0f};
        return key;
    }

    // A clip's own markers: start 0 (fade in), Free at 2 s with a blend,
    // Game at 5 s, end 10 s (fade out).
    Track ClipMarkers()
    {
        Track track;
        Keyframe start = Marker(0);
        start.transition = 1;
        track.AddMarker(start);
        Keyframe free = Marker(2000, replay::editor::CameraMode::Free);
        free.blend = replay::editor::Blend::Smooth;
        free.filter = 3;
        track.AddMarker(free);
        track.AddMarker(Marker(5000));
        Keyframe end = Marker(10000);
        end.transition = 1;
        track.AddMarker(end);
        replay::camera::ApplyLastMarkerRule(track);
        return track;
    }

    project::Clip ClipOf(const char* file, std::uint64_t rawMs)
    {
        project::Clip clip;
        Check(project::MakeClip(file, rawMs, clip), "MakeClip takes a plain file and a length");
        return clip;
    }

    void TestEdits()
    {
        project::Project p;
        p.name = "Test";
        Check(project::AddClip(p, 99, ClipOf("001-Valentine.rpclip", 10000)), "add at the end (index clamped)");
        Check(project::AddClip(p, 0, ClipOf("002-Rhodes.rpclip", 4000)), "add at the front");
        Check(p.clips.size() == 2 && p.clips[0].file == "002-Rhodes.rpclip", "the front insert is first");
        Check(project::TotalMs(p) == 14000, "total is the sum of the trims");
        Check(project::ClipStartMs(p, 1) == 4000, "the second clip starts after the first");

        Check(project::MoveClip(p, 0, 1), "move forward");
        Check(p.clips[1].file == "002-Rhodes.rpclip", "moved to the back");
        Check(!project::MoveClip(p, 1, 1), "a move to the same place changes nothing");
        Check(!project::MoveClip(p, 0, 5), "a move past the end is refused");

        Check(project::DuplicateClip(p, 0), "duplicate");
        Check(p.clips.size() == 3 && p.clips[1].file == p.clips[0].file, "the copy follows its source");
        Check(project::DeleteClip(p, 1), "delete");
        Check(p.clips.size() == 2 && !project::DeleteClip(p, 7), "delete of a missing clip is refused");

        project::Clip bad;
        Check(!project::MakeClip("..\\evil.rpclip", 5000, bad), "a path is not a clip name");
        Check(!project::MakeClip("short.rpclip", project::kMinClipMs - 1, bad), "a clip under two frames is refused");

        // Limits: 30 minutes and 512 clips (CMontage).
        project::Project longOne;
        Check(project::AddClip(longOne, 0, ClipOf("a.rpclip", project::kMaxDurationMs - 1000)), "29:59 fits");
        std::string why;
        Check(!project::AddClip(longOne, 1, ClipOf("b.rpclip", 2000), &why) && !why.empty(), "past 30 minutes is refused, with a reason");
        Check(!project::DuplicateClip(longOne, 0), "a duplicate past 30 minutes is refused");
        project::Project many;
        for (std::size_t n = 0; n < project::kMaxClips; ++n)
            project::AddClip(many, n, ClipOf("c.rpclip", 100));
        Check(many.clips.size() == project::kMaxClips, "512 clips fit");
        Check(!project::AddClip(many, 0, ClipOf("c.rpclip", 100)), "the 513th is refused");
        Check(!project::SplitClip(many, 0, 50), "a split that would make the 513th is refused");
    }

    void TestFitMarkers()
    {
        const Track source = ClipMarkers();
        // Whole clip: the same markers.
        {
            const Track fit = project::FitMarkers(source, 0, 10000);
            Check(fit.Keyframes().size() == 4, "whole trim keeps every marker");
            Check(fit.Keyframes().front().transition == 1 && fit.Keyframes().back().transition == 1, "whole trim keeps both fades");
        }
        // Trim inside: start in the Free stretch, end in the Game stretch.
        {
            const Track fit = project::FitMarkers(source, 3000, 7000);
            const auto& keys = fit.Keyframes();
            Check(keys.size() == 3, "3..7 s: start, the 5 s marker, end");
            Check(keys.front().timeMs == 3000 && keys.back().timeMs == 7000, "bounds at the trim");
            Check(keys.front().cameraMode == replay::editor::CameraMode::Free && keys.front().filter == 3,
                "the start goes on with the camera in effect (Free at 2 s)");
            Check(keys.front().transition == 0, "no fade in at a start copied from a middle marker");
            Check(keys.back().transition == 0, "no fade out at an end copied from a middle marker");
            Check(replay::camera::IsClipBound(fit, keys.front().id) && replay::camera::IsClipBound(fit, keys.back().id),
                "the bounds are the clip's start and end markers");
        }
        // The trim grows past the markers: the clip's first and last move out.
        {
            const Track inner = project::FitMarkers(source, 3000, 7000);
            const Track wider = project::FitMarkers(inner, 1000, 9000);
            Check(wider.Keyframes().front().timeMs == 1000 && wider.Keyframes().back().timeMs == 9000, "wider trim moves the bounds out");
            Check(wider.Keyframes().size() == 3, "no marker is invented");
        }
        // Markers too close to a bound go (the spacing).
        {
            const Track fit = project::FitMarkers(source, 1990, 10000);
            for (const Keyframe& key : fit.Keyframes())
                Check(key.timeMs == 1990 || key.timeMs >= 1990 + replay::camera::kMarkerFrameMs, "no marker within a frame of the start");
        }
        // No markers: the caller's templates.
        {
            Keyframe startTemplate = Marker(0);
            startTemplate.pose.fov = 33.0f;
            const Track fit = project::FitMarkers(Track{}, 500, 2500, startTemplate, Marker(0));
            Check(fit.Keyframes().size() == 2 && fit.Keyframes().front().pose.fov == 33.0f, "an empty source gets the templates");
            Check(fit.Keyframes().front().timeMs == 500 && fit.Keyframes().back().timeMs == 2500, "templates at the trim");
        }
        // A legacy (v2) track counts as empty.
        {
            Track legacy = source;
            legacy.SetLegacyBlendAnyMode(true);
            const Track fit = project::FitMarkers(legacy, 0, 10000);
            Check(fit.Keyframes().size() == 2 && !fit.LegacyBlendAnyMode(), "a legacy track is not carried over");
        }
        // WithinClipBounds: nothing outside the start and end.
        {
            const Track fit = project::FitMarkers(source, 3000, 7000);
            Check(!replay::camera::WithinClipBounds(fit, 2000), "before the start is outside");
            Check(!replay::camera::WithinClipBounds(fit, 3000), "on the start is not inside");
            Check(replay::camera::WithinClipBounds(fit, 4000), "between is inside");
            Check(!replay::camera::WithinClipBounds(fit, 8000), "after the end is outside");
            Check(replay::camera::WithinClipBounds(Track{}, 8000), "no bounds, no limit");
        }
    }

    void TestSplitAndTrim()
    {
        project::Project p;
        project::Clip clip = ClipOf("001-Valentine.rpclip", 10000);
        Check(project::TakeMarkers(clip, project::FitMarkers(ClipMarkers(), 0, 10000)), "the clip takes its markers");
        project::AddClip(p, 0, clip);
        project::AddClip(p, 1, ClipOf("002-Rhodes.rpclip", 4000));

        Check(!project::SplitClip(p, 0, 30), "a cut on the start is refused");
        Check(!project::SplitClip(p, 0, 9990), "a cut on the end is refused");
        Check(project::SplitClip(p, 0, 4000), "a cut at 4 s");
        Check(p.clips.size() == 3, "two pieces and the other clip");
        Check(p.clips[0].file == p.clips[1].file, "both pieces of the same file");
        Check(p.clips[0].inMs == 0 && p.clips[0].outMs == 4000 && p.clips[1].inMs == 4000 && p.clips[1].outMs == 10000,
            "the pieces meet at the cut");
        Check(project::TotalMs(p) == 14000, "a cut keeps the total");
        Check(p.clips[0].markers.Keyframes().back().timeMs == 4000 && p.clips[1].markers.Keyframes().front().timeMs == 4000,
            "the markers end and start at the cut");
        Check(p.clips[0].markers.Keyframes().front().transition == 1 && p.clips[0].markers.Keyframes().back().transition == 0,
            "the first piece keeps the fade in, not a fade out at the cut");
        Check(p.clips[1].markers.Keyframes().back().transition == 1 && p.clips[1].markers.Keyframes().front().transition == 0,
            "the second piece keeps the fade out");
        Check(p.clips[1].markers.Keyframes().front().cameraMode == replay::editor::CameraMode::Free,
            "the second piece starts with the camera in effect at the cut");
        // A clip never opened (no markers) splits by its trim alone.
        Check(project::SplitClip(p, 2, 1000) && !p.clips[2].hasMarkers && p.clips[3].inMs == 1000, "split without markers");

        // Trim.
        Check(project::SetTrim(p, 1, 5000, 9000), "trim the second piece");
        Check(p.clips[1].inMs == 5000 && p.clips[1].outMs == 9000, "the trim is set");
        Check(p.clips[1].markers.Keyframes().front().timeMs == 5000 && p.clips[1].markers.Keyframes().back().timeMs == 9000,
            "the markers follow the trim");
        Check(!project::SetTrim(p, 1, 5000, 9000), "the same trim changes nothing");
        Check(!project::SetTrim(p, 1, 5000, 5010), "a trim under two frames is refused");
        Check(project::SetTrim(p, 1, 5000, 99999) && p.clips[1].outMs == 10000, "the out is clamped to the clip file");

        // TakeMarkers: the trim follows the markers' first and last.
        project::Clip taken = p.clips[0];
        Track moved = taken.markers;
        const std::uint32_t endId = moved.Keyframes().back().id;
        Check(moved.MoveMarker(endId, 3500), "the end marker moves in");
        Check(project::TakeMarkers(taken, moved) && taken.outMs == 3500, "the out follows the end marker");
        Track one;
        one.AddMarker(Marker(100));
        Check(!project::TakeMarkers(taken, one), "one marker cannot be a trim");
    }

    void TestTimeline()
    {
        project::Project p;
        project::Clip a = ClipOf("a.rpclip", 10000);
        a.inMs = 2000;
        a.outMs = 6000; // 4 s
        project::AddClip(p, 0, a);
        project::AddClip(p, 1, ClipOf("b.rpclip", 3000));
        std::size_t index = 99;
        std::uint64_t clipMs = 0;
        Check(project::Locate(p, 0, index, clipMs) && index == 0 && clipMs == 2000, "project 0 is the first clip's in");
        Check(project::Locate(p, 3999, index, clipMs) && index == 0 && clipMs == 5999, "just before the cut");
        Check(project::Locate(p, 4000, index, clipMs) && index == 1 && clipMs == 0, "the cut belongs to the next clip");
        Check(project::Locate(p, 99999, index, clipMs) && index == 1 && clipMs == 3000, "past the end: the last clip's out");
        Check(project::ToProjectMs(p, 0, 2500) == 500 && project::ToProjectMs(p, 1, 1000) == 5000, "clip time to project time");
        Check(project::ToProjectMs(p, 0, 9000) == 4000, "clamped to the trim");
        Check(!project::Locate(project::Project{}, 0, index, clipMs), "an empty project locates nothing");
    }

    project::Project Sample()
    {
        project::Project p;
        p.name = "Saint Denis chase";
        project::Clip clip = ClipOf("001-SaintDenis.rpclip", 10000);
        project::TakeMarkers(clip, project::FitMarkers(ClipMarkers(), 1000, 9000));
        project::AddClip(p, 0, clip);
        project::AddClip(p, 1, ClipOf("002-Rhodes-p2.rpclip", 4000));
        project::SplitClip(p, 0, 5000);
        return p;
    }

    bool SameProject(const project::Project& a, const project::Project& b)
    {
        if (a.name != b.name || a.clips.size() != b.clips.size())
            return false;
        for (std::size_t n = 0; n < a.clips.size(); ++n)
        {
            const project::Clip& x = a.clips[n];
            const project::Clip& y = b.clips[n];
            if (x.file != y.file || x.rawDurationMs != y.rawDurationMs || x.inMs != y.inMs || x.outMs != y.outMs ||
                x.hasMarkers != y.hasMarkers || !replay::camera::SameMarkers(x.markers, y.markers) ||
                x.hasEnvironment != y.hasEnvironment || x.environment != y.environment)
                return false;
        }
        return true;
    }

    void TestFile()
    {
        const project::Project p = Sample();
        const std::vector<std::uint8_t> bytes = project::Encode(p);
        project::Project back;
        std::string error;
        Check(project::Decode(bytes.data(), bytes.size(), back, error), "decode what was encoded");
        Check(SameProject(p, back), "round trip keeps every clip and marker");
        Check(project::Encode(back) == bytes, "encode of the decoded project is byte identical");

        // Refusals leave the project as it was.
        project::Project kept = back;
        std::vector<std::uint8_t> damaged = bytes;
        damaged[damaged.size() / 2] ^= 0x5A;
        Check(!project::Decode(damaged.data(), damaged.size(), kept, error) && error.find("checksum") != std::string::npos,
            "a flipped byte fails the checksum");
        Check(SameProject(kept, back), "a refused decode leaves the project alone");
        Check(!project::Decode(bytes.data(), bytes.size() - 3, kept, error), "a cut file is refused");
        std::vector<std::uint8_t> future = bytes;
        future[4] = static_cast<std::uint8_t>(project::kFormatVersion + 1);
        Check(!project::Decode(future.data(), future.size(), kept, error) && error.find("format") != std::string::npos,
            "a newer format is refused by name");
        std::vector<std::uint8_t> notOne = bytes;
        notOne[0] = 'X';
        Check(!project::Decode(notOne.data(), notOne.size(), kept, error), "another file is refused");
        // A bad trim (out past the clip) with a valid checksum.
        project::Project badTrim = p;
        badTrim.clips[2].outMs = 99999;
        const std::vector<std::uint8_t> badBytes = project::Encode(badTrim);
        Check(!project::Decode(badBytes.data(), badBytes.size(), kept, error), "a trim past the clip file is refused");
        // Finding 14 of the release review: the raw lengths and trims within a
        // clip's range, the total summed without wrapping. Two markerless
        // entries of 2^63 ms each summed to 0 and opened.
        {
            project::Project huge;
            huge.name = "Huge";
            for (const char* file : {"a.rpclip", "b.rpclip"})
            {
                project::Clip clip;
                clip.file = file;
                clip.rawDurationMs = 1ull << 63;
                clip.outMs = 1ull << 63;
                huge.clips.push_back(clip);
            }
            Check(project::TotalMs(huge) == UINT64_MAX && project::ClipStartMs(huge, 2) == UINT64_MAX,
                "a total past 64 bits is held, not wrapped to 0");
            const std::vector<std::uint8_t> hugeBytes = project::Encode(huge);
            project::Project untouched = back;
            Check(!project::Decode(hugeBytes.data(), hugeBytes.size(), untouched, error) &&
                    error.find("bad length") != std::string::npos && SameProject(untouched, back),
                "clips of 2^63 ms are refused, the project left alone");
            project::Project edge;
            edge.name = "Edge";
            project::Clip longest;
            Check(project::MakeClip("c.rpclip", project::kMaxClipRawMs, longest) &&
                    !project::MakeClip("d.rpclip", project::kMaxClipRawMs + 1, longest) &&
                    project::MakeClip("c.rpclip", project::kMaxClipRawMs, longest),
                "MakeClip takes a clip up to the timestamps' range");
            longest.inMs = project::kMaxClipRawMs - 60000;
            edge.clips.push_back(longest);
            const std::vector<std::uint8_t> edgeBytes = project::Encode(edge);
            Check(project::Decode(edgeBytes.data(), edgeBytes.size(), kept, error) && kept.clips.size() == 1 &&
                    kept.clips[0].rawDurationMs == project::kMaxClipRawMs,
                "a clip at the timestamps' range opens");
            edge.clips[0].rawDurationMs = project::kMaxClipRawMs + 1;
            const std::vector<std::uint8_t> pastBytes = project::Encode(edge);
            Check(!project::Decode(pastBytes.data(), pastBytes.size(), kept, error), "a clip past the timestamps' range is refused");
            edge.clips[0].rawDurationMs = project::kMinClipMs - 1;
            edge.clips[0].inMs = 0;
            edge.clips[0].outMs = 1;
            const std::vector<std::uint8_t> shortBytes = project::Encode(edge);
            Check(!project::Decode(shortBytes.data(), shortBytes.size(), kept, error), "a clip file under two frames is refused");
        }
        // An empty project round trips too.
        project::Project empty;
        empty.name = "Empty";
        const std::vector<std::uint8_t> emptyBytes = project::Encode(empty);
        Check(project::Decode(emptyBytes.data(), emptyBytes.size(), kept, error) && kept.clips.empty() && kept.name == "Empty",
            "an empty project round trips");

        // Disk: through the temporary file, then the summary from the head.
        const std::filesystem::path folder = std::filesystem::temp_directory_path() / "replaypoc-project-tests";
        std::error_code code;
        std::filesystem::create_directories(folder, code);
        const std::wstring path = (folder / L"Saint Denis chase.rpproj").wstring();
        Check(project::SaveFile(path, p, error), "save to disk");
        Check(project::SaveFile(path, p, error), "save over the file again");
        Check(!std::filesystem::exists(path + L".tmp"), "no temporary file left");
        project::Project loaded;
        Check(project::LoadFile(path, loaded, error) && SameProject(p, loaded), "load from disk");
        // Finding 4 of the release review: a byte limit before the read (and
        // during it); the refused open says why and leaves the project alone.
        {
            const std::uint64_t fileBytes = std::filesystem::file_size(path, code);
            project::Project limited = back;
            Check(!project::LoadFile(path, limited, error, fileBytes - 1) && error.find("too large") != std::string::npos &&
                    SameProject(limited, back),
                "a project file one byte over the limit is refused before it is read");
            Check(project::LoadFile(path, limited, error, fileBytes) && SameProject(p, limited),
                "a project file at the limit opens");
            // A large file of another kind renamed .rpproj: refused by its size,
            // not read to find its magic.
            const std::wstring other = (folder / L"not a project.rpproj").wstring();
            {
                std::ofstream junk(other, std::ios::binary | std::ios::trunc);
                const std::vector<char> zeros(4096, 0);
                junk.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
            }
            Check(!project::LoadFile(other, limited, error, 1024) && error.find("too large") != std::string::npos,
                "a large file of another kind is refused by its size");
            Check(!project::LoadFile(other, limited, error) && error.find("not a project") != std::string::npos,
                "under the default limit it is refused by its magic");
        }
        // The default limit holds the largest project the edits can make:
        // kMaxClips clips, each with camera::kMaxMarkers markers.
        {
            Track many;
            for (std::uint64_t n = 0; n < replay::camera::kMaxMarkers; ++n)
            {
                Keyframe key = Marker(n * 1000, n % 2 ? replay::editor::CameraMode::Free : replay::editor::CameraMode::Game);
                key.transition = n == 0 ? 1 : 0;
                many.AddMarker(key);
            }
            project::Clip full = ClipOf("123456789012345678901234567890123456789012345678901234.rpclip",
                replay::camera::kMaxMarkers * 1000);
            const bool taken = many.Keyframes().size() == replay::camera::kMaxMarkers && project::TakeMarkers(full, many);
            project::Project one;
            one.name = std::string(project::kMaxNameBytes, 'N');
            const std::size_t headBytes = project::Encode(one).size();
            one.clips.push_back(full);
            const std::size_t perClip = project::Encode(one).size() - headBytes;
            std::printf("Project file bound: %zu bytes per clip of %zu markers, %zu for %zu clips (limit %llu)\n", perClip,
                replay::camera::kMaxMarkers, headBytes + project::kMaxClips * perClip, project::kMaxClips,
                static_cast<unsigned long long>(project::kMaxFileBytes));
            Check(taken && headBytes + project::kMaxClips * perClip <= project::kMaxFileBytes,
                "kMaxFileBytes holds kMaxClips clips of kMaxMarkers markers");
        }
        project::Summary summary;
        Check(project::ReadSummary(path, summary) && summary.readable && summary.name == p.name &&
                summary.clipCount == 3 && summary.totalMs == project::TotalMs(p),
            "the summary reads the head");
        std::filesystem::remove_all(folder, code);

        Check(project::ValidName("Saint Denis chase") && project::ValidName("a_b-1"), "plain names");
        Check(!project::ValidName("") && !project::ValidName(" x") && !project::ValidName("a/b") && !project::ValidName("a.b"),
            "names that cannot be a file");
    }

    // A clip's environment settings in the project: round trip, copies
    // (duplicate, split) keep them, the export plan carries them, and an entry
    // written without them (no tail) loads with none (the clip file's own apply on
    // first open).
    std::uint64_t TestFnv(const std::uint8_t* data, std::size_t size)
    {
        std::uint64_t hash = 0xCBF29CE484222325ull;
        for (std::size_t n = 0; n < size; ++n)
        {
            hash ^= data[n];
            hash *= 0x100000001B3ull;
        }
        return hash;
    }

    void TestEnvironment()
    {
        project::Project p;
        p.name = "Env";
        project::Clip clip = ClipOf("010-Strawberry.rpclip", 12000);
        clip.hasEnvironment = true;
        clip.environment.clockMinutes = 5 * 60 + 30;
        clip.environment.weather = replay::envoverride::Joaat("FOG");
        clip.environment.blendS = 5;
        std::string error;
        Check(project::AddClip(p, 0, clip, &error), "env: add");
        Check(project::DuplicateClip(p, 0, &error) && p.clips[1].hasEnvironment && p.clips[1].environment == clip.environment,
            "env: a duplicate keeps the settings");
        Check(project::SplitClip(p, 1, 6000, &error) && p.clips.size() == 3 && p.clips[2].environment == clip.environment,
            "env: both pieces of a split keep them");
        const std::vector<std::uint8_t> bytes = project::Encode(p);
        project::Project back;
        Check(project::Decode(bytes.data(), bytes.size(), back, error) && SameProject(p, back), "env: round trip");
        const std::vector<project::ExportSegment> plan = project::ExportPlan(p);
        Check(plan.size() == 3 && plan[2].ownEnvironment && plan[2].environment == clip.environment, "env: the export plan carries them");

        // The same project as an older build wrote it: one clip, no tail.
        project::Project one;
        one.name = "Old";
        project::Clip plain = ClipOf("011-Annesburg.rpclip", 8000);
        Check(project::AddClip(one, 0, plain, &error), "env: add a plain clip");
        std::vector<std::uint8_t> v1 = project::Encode(one);
        // magic, version, marker version, reserved, total, name, count.
        const std::size_t entryAt = 4 + 4 + 2 + 2 + 8 + 2 + one.name.size() + 4;
        std::uint32_t entryBytes = 0;
        std::memcpy(&entryBytes, v1.data() + entryAt, 4);
        // The entry's tail is the environment override alone (no shake events block).
        const std::size_t tail = 1 + replay::envoverride::kBytes;
        v1.erase(v1.begin() + static_cast<std::ptrdiff_t>(entryAt + 4 + entryBytes - tail),
            v1.begin() + static_cast<std::ptrdiff_t>(entryAt + 4 + entryBytes));
        entryBytes -= static_cast<std::uint32_t>(tail);
        std::memcpy(v1.data() + entryAt, &entryBytes, 4);
        const std::size_t body = v1.size() - 8;
        const std::uint64_t hash = TestFnv(v1.data(), body);
        std::memcpy(v1.data() + body, &hash, 8);
        project::Project old;
        Check(project::Decode(v1.data(), v1.size(), old, error) && old.clips.size() == 1 && !old.clips[0].hasEnvironment &&
                !old.clips[0].environment.Any(),
            "env: an older entry without settings loads with none");

        // A bad value in the tail is refused by name.
        std::vector<std::uint8_t> bad = project::Encode(p);
        const std::size_t firstEntry = 4 + 4 + 2 + 2 + 8 + 2 + p.name.size() + 4;
        std::uint32_t firstBytes = 0;
        std::memcpy(&firstBytes, bad.data() + firstEntry, 4);
        bad[firstEntry + 4 + firstBytes - replay::envoverride::kBytes + 8] = 7; // blend not in the list
        const std::uint64_t badHash = TestFnv(bad.data(), bad.size() - 8);
        std::memcpy(bad.data() + bad.size() - 8, &badHash, 8);
        project::Project kept;
        Check(!project::Decode(bad.data(), bad.size(), kept, error) && error.find("environment") != std::string::npos,
            "env: a bad setting is refused");
    }

    void TestExportPlan()
    {
        const project::Project p = Sample();
        const std::vector<project::ExportSegment> plan = project::ExportPlan(p);
        Check(plan.size() == 3, "one segment per clip");
        Check(plan[0].inMs == 1000 && plan[0].outMs == 5000 && plan[0].projectStartMs == 0 && plan[0].ownMarkers,
            "the first piece");
        Check(plan[1].inMs == 5000 && plan[1].outMs == 9000 && plan[1].projectStartMs == 4000, "the second piece follows");
        Check(plan[2].file == "002-Rhodes-p2.rpclip" && !plan[2].ownMarkers && plan[2].projectStartMs == 8000,
            "a clip never opened says so");
        Check(plan[1].markers.Keyframes().front().timeMs == plan[1].inMs && plan[1].markers.Keyframes().back().timeMs == plan[1].outMs,
            "the markers bound each segment");
    }

    // Renaming (src/project_name.h): a typed name made a file name, and the
    // check against the folder (a taken name refused, a case-only change kept).
    void TestRename()
    {
        Check(project::CleanName("Saint Denis chase") == "Saint Denis chase", "a plain name stays");
        Check(project::CleanName("  Chase:  part 2?  ") == "Chase part 2", "file name characters become one space, the ends trimmed");
        Check(project::CleanName("a/b\\c*d\"e<f>g|h") == "a b c d e f g h", "every forbidden file name character");
        Check(project::CleanName("Caf\xC3\xA9 night") == "Caf night", "a non-ASCII letter goes");
        Check(project::CleanName("x.y") == "x y" && project::CleanName("..") == "", "dots never reach the file name");
        Check(project::CleanName("?*:") == "" && project::CleanName("") == "", "nothing left: empty");
        Check(project::CleanName("con") == "con_" && project::CleanName("LPT1") == "LPT1_" && project::CleanName("COM10") == "COM10",
            "a Windows device name gets a _");
        Check(project::CleanName("Nul.") == "Nul_", "a device name after cleaning too");
        const std::string longName(60, 'a');
        Check(project::CleanName(longName).size() == project::kMaxNameBytes, "cut to the longest name");
        Check(project::CleanName(std::string(47, 'a') + " b") == std::string(47, 'a'), "no space left at the cut");
        for (const char* typed : {"a/b", " x ", "con", "Chase: 2", "\t", "x..y"})
        {
            const std::string clean = project::CleanName(typed);
            Check(clean.empty() || project::ValidName(clean), "a cleaned name is always valid");
        }
        Check(!project::ValidName("CON") && !project::ValidName("aux") && project::ValidName("CONS"), "ValidName refuses a device name");
        Check(project::NameFile("Chase") == "Chase.rpproj", "the file of a name");
        Check(project::SameFile("Chase.rpproj", "chase.RPPROJ") && !project::SameFile("Chase.rpproj", "Chase2.rpproj"),
            "one file whatever the capitals");

        using project::RenameCheck;
        const std::vector<std::string> files{"Chase.rpproj", "Rhodes.rpproj", "notes.txt"};
        Check(project::CheckRename("Chase.rpproj", "Night ride", files) == RenameCheck::Ok, "a free name");
        Check(project::CheckRename("Chase.rpproj", "Rhodes", files) == RenameCheck::Exists, "another project's name is refused");
        Check(project::CheckRename("Chase.rpproj", "rhodes", files) == RenameCheck::Exists, "whatever the capitals");
        Check(project::CheckRename("Chase.rpproj", "Chase", files) == RenameCheck::Same, "the same name: nothing to do");
        Check(project::CheckRename("Chase.rpproj", "CHASE", files) == RenameCheck::CaseOnly, "only the capitals change");
        Check(project::CheckRename("Chase.rpproj", "", files) == RenameCheck::Empty, "no name");
        Check(project::CheckRename("", "Chase", files) == RenameCheck::Exists, "a new project cannot take a used name");
        Check(project::CheckRename("", "notes", files) == RenameCheck::Ok, "other files do not count");
    }
}

int RunProjectTests()
{
    TestEdits();
    TestFitMarkers();
    TestSplitAndTrim();
    TestTimeline();
    TestFile();
    TestExportPlan();
    TestEnvironment();
    TestRename();
    if (g_projectFailures == 0)
        std::puts("Project tests passed.");
    return g_projectFailures;
}
