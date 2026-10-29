// Whole project export: the plan's frames per piece (slow motion markers in
// some pieces), each piece's first frame and its time in the joined video,
// the sound cut per piece to its frames (no drift over many seams), the
// whole's progress, the pieces' file names, the project's WAV spliced from
// the pieces' audio passes (offsets, silence where a pass is missing or
// short), one chapter per clip and the ffmpeg.exe concat list.
#include "project_export_plan.h"

#include "../editor/render_export/ffmpeg_args.h"
#include "../editor/render_export/project_parts.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    namespace pe = replay::projectexport;
    namespace ex = replay::exporting;

    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    void TestPlanFrames()
    {
        const ex::FrameRate fps30{30, 1};
        pe::SegmentInput plain{0, 1010, {}};
        pe::SegmentInput slow{0, 1010, {{0, 0.5}}};
        // Three clips, the middle one in 50 % slow motion all through.
        const std::vector<pe::SegmentFrames> plan = pe::PlanFrames({plain, slow, plain}, fps30);
        Check(plan.size() == 3, "one piece per clip");
        Check(plan[0].frames == 30, "1.01 s at 30 fps: 30 frames");
        Check(plan[1].frames == 60, "the same span at 50 %: twice the frames");
        Check(plan[2].frames == 30, "the third clip at 100 % again");
        Check(plan[0].firstFrame == 0 && plan[1].firstFrame == 30 && plan[2].firstFrame == 90, "each piece starts after the frames before it");
        Check(pe::TotalFrames(plan) == 120, "the whole: the pieces' frames");
        Check(pe::FrameTimeNs(fps30, plan[1].firstFrame) == 1000000000ll, "piece 2 starts at 1 s of the video (no gap)");
        Check(pe::FrameTimeNs(fps30, plan[2].firstFrame) == 3000000000ll, "piece 3 at 3 s: the slow motion piece lasts 2 s");
        // As the probe counts a range (exportprobe::Request: SpanOutputFrames).
        Check(plan[1].frames == ex::SpanOutputFrames(0, 1010, fps30, [](std::uint64_t) { return 0.5; }, 50000000ull),
            "the plan counts as the export clock");

        // A slow motion marker inside a piece, and one before its start.
        pe::SegmentInput half{0, 2010, {{1000, 0.5}}};
        const std::vector<pe::SegmentFrames> mixed = pe::PlanFrames({half}, fps30);
        Check(mixed[0].frames > 60 + 25 && mixed[0].frames < 60 + 35, "1 s at 100 % then 1 s at 50 %: about 90 frames");
        pe::SegmentInput before{3000, 4010, {{1000, 2.0}}};
        Check(pe::PlanFrames({before}, fps30)[0].frames == 15, "a 200 % marker before the range is in effect from its start");
        Check(pe::SpeedAt({{1000, 0.5}, {2000, 1.25}}, 1500) == 0.5 && pe::SpeedAt({{1000, 0.5}}, 999) == 1.0, "the last marker at or before");
        Check(pe::PlanFrames({}, fps30).empty() && pe::TotalFrames({}) == 0, "an empty project");
        pe::SegmentInput empty{500, 500, {}};
        Check(pe::PlanFrames({empty}, fps30)[0].frames == 0, "an empty range: no frame");
    }

    void TestAudio()
    {
        const ex::FrameRate fps60{60, 1};
        Check(pe::AudioSampleAt(120, fps60, 48000) == 96000, "2 s at 60 fps: 96000 samples");
        Check(pe::AudioSamples(0, 60, fps60, 48000) == 48000 && pe::AudioSamples(60, 60, fps60, 48000) == 48000, "1 s pieces: 48000 each");
        // 29.97: the pieces' samples add up to the whole's exactly.
        const ex::FrameRate ntsc{30000, 1001};
        const std::uint64_t frames[] = {1001, 77, 500, 3, 12345};
        std::uint64_t first = 0, sum = 0;
        for (std::uint64_t n : frames)
        {
            sum += pe::AudioSamples(first, n, ntsc, 48000);
            first += n;
        }
        Check(sum == pe::AudioSampleAt(first, ntsc, 48000), "no drift over the seams at 29.97 fps");
        Check(pe::AudioSampleAt(30000, ntsc, 48000) == 48048000, "30000 frames at 29.97 fps: 1001 s of sound");

        std::vector<render_export::ProjectPart> parts(3);
        parts[0].video.frames = 30;
        parts[1].video.frames = 60;
        parts[2].video.frames = 30;
        parts[0].wav = L"a.wav";
        parts[0].audioStartS = 0.25;
        const std::vector<render_export::AudioPiece> pieces = render_export::PlanAudio(parts, {30, 1}, 48000);
        Check(pieces.size() == 3 && pieces[0].samples == 48000 && pieces[1].samples == 96000 && pieces[2].samples == 48000,
            "each piece's sound: exactly its frames' length (the slow motion piece's 2 s)");
        Check(pieces[0].wav == L"a.wav" && pieces[0].startS == 0.25 && pieces[1].wav.empty(), "the piece's own pass and offset");
    }

    void TestProgress()
    {
        Check(pe::OverallFraction(0, 0, 0) == 0.0, "nothing planned: 0");
        Check(std::fabs(pe::OverallFraction(100, 50, 300) - 0.5) < 1e-9, "the pieces done and the current one's frames");
        Check(pe::OverallFraction(300, 50, 300) == 1.0, "never past 100 %");
        Check(pe::PercentOf(0.999) == 99 && pe::PercentOf(1.0) == 100 && pe::PercentOf(-1.0) == 0, "the percent rounds down");
        const std::vector<pe::SegmentFrames> plan = pe::PlanFrames({{0, 1010, {}}, {0, 1010, {{0, 0.5}}}, {0, 1010, {}}}, {30, 1});
        Check(pe::TotalEstimate(plan, 0, 0, 30) == 120, "piece 1 started: the plan's rest");
        Check(pe::TotalEstimate(plan, 1, 31, 58) == 31 + 58 + 30, "the pieces done as they came out, the current one's count");
        Check(pe::TotalEstimate(plan, 2, 89, 30) == 119, "the last piece: nothing planned after it");
        Check(pe::ThumbnailSegment(plan) == 1, "the middle frame (60 of 120) is in the slow motion piece");
        Check(pe::ThumbnailSegment(pe::PlanFrames({{0, 1010, {}}}, {30, 1})) == 0, "one piece: its own");
    }

    void TestPaths()
    {
        using render_export::ProjectPartPath;
        Check(ProjectPartPath(L"C:\\v\\Chase_h264.mp4", 0, false) == L"C:\\v\\Chase_h264.part01.mp4", "a piece next to the video");
        Check(ProjectPartPath(L"C:\\v\\Chase_ffv1.mkv", 11, false) == L"C:\\v\\Chase_ffv1.part12.mkv", "two digits");
        Check(ProjectPartPath(L"C:\\v.d\\Chase", 1, false) == L"C:\\v.d\\Chase.part02", "a dot in a folder is not an extension");
        Check(ProjectPartPath(L"C:\\v\\Chase_png", 2, true) == L"C:\\v\\Chase_png.part03", "an image sequence: a folder per piece");
        const std::string list = render_export::BuildConcatList({"C:\\v\\a.part01.mp4", "C:\\v\\it's.part02.mp4"});
        Check(list == "ffconcat version 1.0\nfile 'C:\\v\\a.part01.mp4'\nfile 'C:\\v\\it'\\''s.part02.mp4'\n", "the concat list, a quote escaped");
        const auto chapters = render_export::PartChapterTimes({"042-Valentine.rpclip", "chase.rpclip", ""}, {30, 60, 30}, {30, 1});
        Check(chapters.size() == 3 && chapters[0].first == 0 && chapters[1].first == 1000 && chapters[2].first == 3000,
            "a chapter at each piece's first frame");
        Check(chapters[0].second == "1. 042-Valentine" && chapters[2].second == "3.", "named after the clip");
    }

    // A small WAV: 1 kHz mono 16 bit, the values given; `list`: a LIST chunk
    // before "data" (as some writers put).
    void WriteWav(const std::wstring& path, const std::vector<std::int16_t>& values, bool list)
    {
        std::FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), L"wb");
        if (!f)
            return;
        const std::uint32_t data = static_cast<std::uint32_t>(values.size() * 2);
        const std::uint32_t listBytes = list ? 12 : 0;
        const auto put32 = [f](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
        const auto put16 = [f](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
        std::fwrite("RIFF", 1, 4, f);
        put32(36 + listBytes + data);
        std::fwrite("WAVEfmt ", 1, 8, f);
        put32(16);
        put16(1);
        put16(1);
        put32(1000);
        put32(2000);
        put16(2);
        put16(16);
        if (list)
        {
            std::fwrite("LIST", 1, 4, f);
            put32(4);
            std::fwrite("INFO", 1, 4, f);
        }
        std::fwrite("data", 1, 4, f);
        put32(data);
        std::fwrite(values.data(), 2, values.size(), f);
        std::fclose(f);
    }

    std::vector<std::int16_t> ReadSamples(const std::wstring& path, render_export::WavInfo& info)
    {
        std::vector<std::int16_t> out;
        if (!render_export::ReadWavInfo(path, info))
            return out;
        std::FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), L"rb");
        if (!f)
            return out;
        std::fseek(f, static_cast<long>(info.dataOffset), SEEK_SET);
        out.resize(static_cast<std::size_t>(info.dataBytes / 2));
        out.resize(std::fread(out.data(), 2, out.size(), f));
        std::fclose(f);
        return out;
    }

    void TestSplice()
    {
        const std::filesystem::path folder = std::filesystem::temp_directory_path() / "replay_project_export_tests";
        std::filesystem::create_directories(folder);
        const std::wstring a = (folder / L"a.audio.wav").wstring();
        const std::wstring b = (folder / L"b.audio.wav").wstring();
        const std::wstring stereo = (folder / L"c.audio.wav").wstring();
        const std::wstring out = (folder / L"project.audio.wav").wstring();
        std::vector<std::int16_t> va, vb;
        for (int i = 1; i <= 100; ++i)
        {
            va.push_back(static_cast<std::int16_t>(i));
            vb.push_back(static_cast<std::int16_t>(1000 + i));
        }
        WriteWav(a, va, false);
        WriteWav(b, vb, true);
        {
            // Another format (stereo): silence in a mono project.
            render_export::WavInfo info;
            WriteWav(stereo, va, false);
            std::FILE* f = nullptr;
            _wfopen_s(&f, stereo.c_str(), L"r+b");
            if (f)
            {
                const std::uint16_t two = 2;
                std::fseek(f, 22, SEEK_SET);
                std::fwrite(&two, 2, 1, f);
                std::fclose(f);
            }
            Check(render_export::ReadWavInfo(stereo, info) && info.channels == 2, "the stereo file reads as stereo");
        }
        const std::vector<render_export::AudioPiece> pieces = {
            {a, 0.010, 50},     // from sample 10: 11..60
            {L"", 0.0, 20},     // no audio pass: 20 silent
            {b, -0.005, 30},    // the range starts 5 samples before the file: 5 silent, then 1001..1025
            {b, 0.095, 20},     // from 95: 1096..1100, then the pass ended: 15 silent
            {stereo, 0.0, 10},  // another format: 10 silent
        };
        std::string error;
        render_export::SpliceReport report;
        Check(render_export::SpliceWav(pieces, out, error, &report), "the project's sound written");
        Check(report.samples == 130 && report.silentSamples == 20 + 5 + 15 + 10 && report.sampleRate == 1000, "every piece its length, silence counted");
        render_export::WavInfo info;
        const std::vector<std::int16_t> got = ReadSamples(out, info);
        Check(info.sampleRate == 1000 && info.channels == 1 && info.bitsPerSample == 16 && !info.isFloat, "the first piece's format");
        Check(got.size() == 130, "130 samples: the pieces' lengths added up");
        std::vector<std::int16_t> want;
        for (int i = 11; i <= 60; ++i)
            want.push_back(static_cast<std::int16_t>(i));
        want.insert(want.end(), 20 + 5, 0);
        for (int i = 1001; i <= 1025; ++i)
            want.push_back(static_cast<std::int16_t>(i));
        for (int i = 1096; i <= 1100; ++i)
            want.push_back(static_cast<std::int16_t>(i));
        want.insert(want.end(), 15 + 10, 0);
        Check(got == want, "each piece cut at its own start, in order, silence where it has none");

        // No readable piece: 48 kHz float stereo silence of the right length.
        Check(render_export::SpliceWav({{L"", 0.0, 480}}, out, error, &report) && report.samples == 480 && report.sampleRate == 48000,
            "no audio pass at all: silence");
        Check(render_export::ReadWavInfo(out, info) && info.isFloat && info.channels == 2 && info.dataBytes == 480 * 8, "float stereo");
        Check(!render_export::SpliceWav({{a, 0.0, 1}}, (folder / L"no-such-folder" / L"x.wav").wstring(), error), "an output that cannot be written");

        // The head parser: no RIFF, a truncated fmt.
        const std::uint8_t junk[16] = {'R', 'I', 'F', 'X'};
        render_export::WavInfo none;
        Check(!render_export::ParseWavHeader(junk, sizeof(junk), none), "not a WAV");
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
    }
}

int RunProjectExportTests()
{
    g_failures = 0;
    TestPlanFrames();
    TestAudio();
    TestProgress();
    TestPaths();
    TestSplice();
    std::printf("project export tests: %s\n", g_failures ? "FAILED" : "passed");
    return g_failures;
}
