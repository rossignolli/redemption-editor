// Segmented export (editor/render_export/export_segments.h): segment
// boundaries in frames and in audio samples at any frame rate (29.97 and
// 59.94 included, slow motion), the manifest's round trip and what a resume
// accepts, the resume's start and pre-roll, a resume giving the same frames
// and samples per segment as the export it continues, and the join plan.
#include "../editor/render_export/export_segments.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace
{
    namespace sg = render_export::segments;
    namespace ex = replay::exporting;
    namespace pe = replay::projectexport;
    namespace ec = replay::exportchannel;

    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    const ex::FrameRate kRates[] = {{24, 1}, {25, 1}, {30, 1}, {30000, 1001}, {24000, 1001}, {60, 1}, {60000, 1001}, {120, 1}};

    void TestBoundaries()
    {
        Check(sg::StartFrame(1, 60, {60, 1}) == 3600 && sg::StartFrame(2, 60, {60, 1}) == 7200, "60 fps: a segment every 3600 frames");
        Check(sg::StartFrame(1, 60, {30000, 1001}) == 1799 && sg::StartFrame(2, 60, {30000, 1001}) == 3597,
            "29.97 fps: 1798.2 and 3596.4 frames rounded up (the first frame at or after 60 s and 120 s)");
        Check(sg::Count(36000, 60, {60, 1}) == 10 && sg::Count(36001, 60, {60, 1}) == 11 && sg::Count(1, 60, {60, 1}) == 1 &&
                sg::Count(0, 60, {60, 1}) == 0,
            "10 minutes at 60 fps: 10 segments; one frame more opens an 11th");
        bool ok = true, timeOk = true, samplesOk = true, ofOk = true;
        for (const ex::FrameRate rate : kRates)
            for (const std::uint32_t seconds : {5u, 7u, 60u})
                for (const std::uint64_t total : {1ull, 299ull, 3600ull, 36000ull, 36001ull, 107892ull})
                {
                    const std::uint32_t count = sg::Count(total, seconds, rate);
                    ok = ok && count >= 1 && sg::StartFrame(count - 1, seconds, rate) < total && sg::StartFrame(count, seconds, rate) >= total;
                    std::uint64_t next = 0, nextSample = 0, samples = 0;
                    for (std::uint32_t k = 0; k < count; ++k)
                    {
                        const sg::Bounds b = sg::BoundsOf(k, total, seconds, rate);
                        ok = ok && b.first == next && b.frames > 0;
                        samplesOk = samplesOk && b.firstSample == nextSample;
                        // Segment k starts on the first frame at or after k * seconds.
                        const std::int64_t at = static_cast<std::int64_t>(k) * seconds * ex::kNsPerSecond;
                        timeOk = timeOk && ex::FrameStartNs(rate, static_cast<std::int64_t>(b.first)) >= at &&
                            (b.first == 0 || ex::FrameStartNs(rate, static_cast<std::int64_t>(b.first) - 1) < at);
                        ofOk = ofOk && sg::SegmentOf(b.first, seconds, rate) == k && sg::SegmentOf(b.first + b.frames - 1, seconds, rate) == k;
                        next = b.first + b.frames;
                        nextSample = b.firstSample + b.samples;
                        samples += b.samples;
                    }
                    ok = ok && next == total;
                    samplesOk = samplesOk && samples == pe::AudioSampleAt(total, rate, sg::kSampleRate);
                }
        Check(ok, "the segments cover the video once, in order, at every rate and length");
        Check(timeOk, "each segment starts on the first frame at or after its time");
        Check(samplesOk, "the segments' samples follow each other and add up to the video's (no gap, no overlap, no drift)");
        Check(ofOk, "SegmentOf names the segment of a frame");
        // 60 s at 30000/1001 and 48 kHz: 1799 frames of 1601.6 samples are 2881278.4, cut at a whole sample.
        const sg::Bounds first = sg::BoundsOf(0, 107892, 60, {30000, 1001});
        const sg::Bounds second = sg::BoundsOf(1, 107892, 60, {30000, 1001});
        Check(first.frames == 1799 && first.samples == 2881278 && second.firstSample == 2881278 && second.frames == 1798 &&
                second.samples == 2879677,
            "29.97 fps: the first segment's 1799 frames are 2881278 samples, the second starts there");

        // Slow motion: the segments are video time, so a 20 % marker only
        // makes more of them (the export's frames, counted as the probe does).
        const auto slow = [](std::uint64_t ms) { return ms >= 30000 && ms < 40000 ? 0.2 : 1.0; };
        const std::uint64_t frames = ex::SpanOutputFrames(0, 60000, {60, 1}, slow, 50000000ull);
        Check(frames >= 50 * 60 + 10 * 5 * 60 - 2 && frames <= 50 * 60 + 10 * 5 * 60 + 2, "60 s with 10 s at 20 %: about 100 s of video");
        Check(sg::Count(frames, 60, {60, 1}) == 2 && sg::BoundsOf(0, frames, 60, {60, 1}).frames == 3600,
            "slow motion: segments of 60 s of video, whatever the playhead does");

        Check(sg::ClampSeconds(0) == 0 && sg::ClampSeconds(-5) == 0 && sg::ClampSeconds(1) == 5 && sg::ClampSeconds(60) == 60 &&
                sg::ClampSeconds(100000) == 3600,
            "SegmentSeconds: 0 off, else 5 to 3600");
    }

    sg::Manifest Sample(std::uint64_t total, std::uint32_t done, std::uint64_t preroll)
    {
        sg::Manifest m;
        m.settingsHash = "0123456789abcdef";
        m.identity = "042-Valentine.rpclip|5f00aa";
        m.finalOutput = "C:\\Videos\\042-Valentine_20261030-101500_h264.mp4";
        m.extension = ".mp4";
        m.rate = {30000, 1001};
        m.seconds = 60;
        m.totalFrames = total;
        m.width = 2560;
        m.height = 1440;
        for (std::uint32_t k = 0; k < done; ++k)
        {
            const sg::Bounds b = sg::BoundsOf(k, total, m.seconds, m.rate);
            sg::Record r;
            r.index = k;
            r.frames = b.frames;
            r.firstSample = b.firstSample;
            r.samples = b.samples;
            r.file = sg::SegmentFileName(k, m.extension);
            if (k + 1 < m.SegmentCount())
            {
                r.hasResume = true;
                r.resumeFrame = sg::ResumeFrame(k + 1, preroll, m.seconds, m.rate, 0);
                r.resumeMs = 1000 + r.resumeFrame * 1001 / 30; // a playhead near the frame's time
            }
            m.segments.push_back(r);
        }
        return m;
    }

    void TestManifest()
    {
        sg::Manifest m = Sample(107892, 3, 60);
        m.audioDone = true;
        m.audioFile = "audio.wav";
        m.audioStartS = 0.512345;
        const std::string text = sg::Format(m);
        sg::Manifest back;
        std::string why;
        Check(sg::Parse(text, back, why), "the manifest reads back");
        Check(back.settingsHash == m.settingsHash && back.identity == m.identity && back.finalOutput == m.finalOutput &&
                back.extension == ".mp4" && back.rate.num == 30000 && back.rate.den == 1001 && back.seconds == 60 && back.totalFrames == 107892 &&
                back.width == 2560 && back.height == 1440 && back.sampleRate == 48000,
            "the header round trips");
        bool same = back.segments.size() == 3;
        for (std::size_t k = 0; same && k < 3; ++k)
            same = back.segments[k].index == m.segments[k].index && back.segments[k].frames == m.segments[k].frames &&
                back.segments[k].firstSample == m.segments[k].firstSample && back.segments[k].samples == m.segments[k].samples &&
                back.segments[k].file == m.segments[k].file && back.segments[k].hasResume && back.segments[k].resumeFrame == m.segments[k].resumeFrame &&
                back.segments[k].resumeMs == m.segments[k].resumeMs;
        Check(same, "the segments round trip, with their resume points");
        Check(back.audioDone && back.audioFile == "audio.wav" && back.audioStartS > 0.512344 && back.audioStartS < 0.512346, "the audio pass round trips");
        Check(sg::Format(back) == text, "written again, the same text");

        // A path with commas and spaces stays whole (the file field is the line's rest).
        sg::Manifest odd = Sample(7200, 1, 60);
        odd.finalOutput = "D:\\My Videos, 2026\\a,b.mov";
        odd.segments[0].file = "seg-0001.mov";
        Check(sg::Parse(sg::Format(odd), back, why) && back.finalOutput == odd.finalOutput, "a final path with commas");

        const std::string cut = text.substr(0, text.size() - 4); // no "end"
        Check(!sg::Parse(cut, back, why), "a manifest cut short is refused");
        std::string other = text;
        other.replace(other.find("version=1"), 9, "version=9");
        Check(!sg::Parse(other, back, why), "another manifest version is refused");
        Check(!sg::Parse("version=1\nend\n", back, why), "a header without the video is refused");
        std::string bad = text;
        bad.replace(bad.find("segment=2,frames="), 17, "segment=2,frame=");
        Check(!sg::Parse(bad, back, why), "a damaged segment line is refused");
    }

    void TestValidation()
    {
        const sg::Manifest m = Sample(107892, 3, 60);
        Check(sg::Check(m, m.settingsHash, m.identity) == sg::ResumeCheck::Resumable, "the same export: resumable");
        Check(sg::Check(m, "fedcba9876543210", m.identity) == sg::ResumeCheck::SettingsChanged, "other settings: no resume");
        Check(sg::Check(m, m.settingsHash, "043-Saint Denis.rpclip|5f00aa") == sg::ResumeCheck::OtherExport, "another clip: no resume");
        Check(sg::Check(m, m.settingsHash, "042-Valentine.rpclip|77") == sg::ResumeCheck::OtherExport, "the clip edited since: no resume");
        Check(sg::Check(Sample(107892, 0, 60), m.settingsHash, m.identity) == sg::ResumeCheck::Nothing, "no segment done: nothing to continue");
        Check(sg::DoneSegments(m) == 3, "three segments done");

        sg::Manifest shortSegment = m;
        shortSegment.segments[1].frames -= 1; // a segment that lost a frame
        Check(sg::DoneSegments(shortSegment) == 1, "a segment short of a frame and the ones after it are redone");
        sg::Manifest gap = m;
        gap.segments.erase(gap.segments.begin() + 1);
        Check(sg::DoneSegments(gap) == 1, "a missing segment: only the ones before it count");
        sg::Manifest noPoint = m;
        noPoint.segments[2].hasResume = false;
        Check(sg::DoneSegments(noPoint) == 2, "the last one without a resume point: resumed from the one before");
        sg::Manifest samples = m;
        samples.segments[0].samples += 1;
        Check(sg::DoneSegments(samples) == 0, "a sample count that is not the segment's: not kept");
    }

    // The frames and samples per segment a run writes: frames [firstFrame,
    // firstFrame + frames) of the whole rendered, the ones from writeFrom on
    // written, each to the segment it belongs to.
    std::vector<sg::Bounds> Written(std::uint64_t firstFrame, std::uint64_t frames, std::uint64_t writeFrom, std::uint64_t total,
        std::uint32_t seconds, ex::FrameRate rate)
    {
        std::vector<sg::Bounds> out;
        for (std::uint64_t f = firstFrame; f < firstFrame + frames && f < total; ++f)
        {
            if (f < writeFrom)
                continue;
            const std::uint32_t k = sg::SegmentOf(f, seconds, rate);
            if (out.size() <= k)
                out.resize(k + 1);
            if (out[k].frames == 0)
                out[k].first = f;
            ++out[k].frames;
        }
        for (std::size_t k = 0; k < out.size(); ++k)
            if (out[k].frames)
            {
                out[k].firstSample = pe::AudioSampleAt(out[k].first, rate, sg::kSampleRate);
                out[k].samples = pe::AudioSamples(out[k].first, out[k].frames, rate, sg::kSampleRate);
            }
        return out;
    }

    void TestResume()
    {
        Check(sg::PrerollFrames(2000, {60, 1}) == 120 && sg::PrerollFrames(2000, {30000, 1001}) == 60 && sg::PrerollFrames(0, {60, 1}) == 0 &&
                sg::PrerollFrames(2000, {24000, 1001}) == 48,
            "2 s of pre-roll in frames, rounded up");
        Check(sg::ResumeFrame(3, 120, 60, {60, 1}, 0) == 3 * 3600 - 120, "the pre-roll ends on the segment's first frame");
        Check(sg::ResumeFrame(1, 9000, 60, {60, 1}, 0) == 0, "a pre-roll longer than the video before: from frame 0");
        Check(sg::ResumeFrame(4, 120, 60, {60, 1}, 14000) == 14280 && sg::ResumeFrame(4, 9000, 60, {60, 1}, 14000) == 14000,
            "never before the first frame the recording run rendered");

        bool same = true;
        for (const ex::FrameRate rate : kRates)
            for (const std::uint64_t total : {36000ull, 107892ull, 4321ull})
            {
                const std::uint64_t preroll = sg::PrerollFrames(2000, rate);
                // The uninterrupted export.
                const std::vector<sg::Bounds> straight = Written(0, total, 0, total, 60, rate);
                const std::uint32_t count = sg::Count(total, 60, rate);
                for (std::uint32_t done = 1; done < count; ++done)
                {
                    sg::Manifest cut = Sample(total, done, preroll);
                    cut.rate = rate;
                    cut.segments.clear();
                    for (std::uint32_t k = 0; k < done; ++k)
                    {
                        const sg::Bounds b = sg::BoundsOf(k, total, 60, rate);
                        sg::Record r;
                        r.index = k;
                        r.frames = b.frames;
                        r.firstSample = b.firstSample;
                        r.samples = b.samples;
                        r.file = sg::SegmentFileName(k, ".mp4");
                        r.hasResume = true;
                        r.resumeFrame = sg::ResumeFrame(k + 1, preroll, 60, rate, 0);
                        r.resumeMs = r.resumeFrame;
                        cut.segments.push_back(r);
                    }
                    sg::ResumePlan plan;
                    if (!sg::PlanResume(cut, plan) || plan.videoDone || plan.segment != done)
                    {
                        same = false;
                        continue;
                    }
                    same = same && plan.writeFrom == sg::StartFrame(done, 60, rate) && plan.writeFrom - plan.firstFrame == std::min(preroll, plan.writeFrom) &&
                        plan.firstFrame + plan.frames == total && plan.startMs == plan.firstFrame;
                    const std::vector<sg::Bounds> resumed = Written(plan.firstFrame, plan.frames, plan.writeFrom, total, 60, rate);
                    for (std::uint32_t k = done; k < count; ++k)
                        same = same && k < resumed.size() && resumed[k].first == straight[k].first && resumed[k].frames == straight[k].frames &&
                            resumed[k].firstSample == straight[k].firstSample && resumed[k].samples == straight[k].samples;
                    for (std::uint32_t k = 0; k < done && k < resumed.size(); ++k)
                        same = same && resumed[k].frames == 0; // the pre-roll writes nothing
                }
            }
        Check(same, "a resume at any segment writes the same frames and samples per segment as the uninterrupted export");

        sg::ResumePlan plan;
        Check(!sg::PlanResume(Sample(36000, 0, 120), plan), "nothing done: no resume");
        sg::Manifest all = Sample(36000, 10, 120);
        all.rate = {60, 1};
        all.segments.clear();
        for (std::uint32_t k = 0; k < 10; ++k)
        {
            const sg::Bounds b = sg::BoundsOf(k, 36000, 60, {60, 1});
            all.segments.push_back({k, b.frames, b.firstSample, b.samples, sg::SegmentFileName(k, ".mp4"), k < 9, 0, 0});
        }
        Check(sg::PlanResume(all, plan) && plan.videoDone && plan.frames == 0 && plan.segment == 10, "every segment done: no video pass");
    }

    void TestJoin()
    {
        sg::Manifest m;
        m.settingsHash = "x";
        m.rate = {60, 1};
        m.seconds = 60;
        m.totalFrames = 7300;
        for (std::uint32_t k = 0; k < 3; ++k)
        {
            const sg::Bounds b = sg::BoundsOf(k, m.totalFrames, 60, m.rate);
            m.segments.push_back({k, b.frames, b.firstSample, b.samples, sg::SegmentFileName(k, ".mkv"), k < 2, k * 3600u + 3480u, 0});
        }
        std::vector<sg::JoinPart> parts;
        std::string why;
        Check(sg::PlanJoin(m, parts, why) && parts.size() == 3 && parts[0].file == "seg-0001.mkv" && parts[2].file == "seg-0003.mkv" &&
                parts[0].frames == 3600 && parts[1].frames == 3600 && parts[2].frames == 100,
            "the join: the three segments in order with their frames");
        sg::Manifest missing = m;
        missing.segments.pop_back();
        Check(!sg::PlanJoin(missing, parts, why), "a segment missing: no join");
        sg::Manifest more = m;
        more.totalFrames = 7400; // the plan says 7400 frames, the segments hold 7300
        Check(!sg::PlanJoin(more, parts, why), "segments that do not add up to the video: no join");
    }

    void TestSettingsHash()
    {
        ec::Settings a;
        a.startMs = 1000;
        a.endMs = 601000;
        a.rate = {60, 1};
        a.samples = 8;
        a.preset = ec::VideoPreset::H264Nvenc;
        wcscpy_s(a.outputPath, L"C:\\v\\clip_20261030-101500_h264.mp4");
        ec::Settings b = a;
        wcscpy_s(b.outputPath, L"C:\\v\\clip_20261030-111500_h264.mp4");
        Check(sg::SettingsHash(a, 60, 4, false) == sg::SettingsHash(b, 60, 4, false), "the output name (its time of day) is not a setting");
        Check(sg::SettingsHash(a, 60, 4, false).size() == 16, "16 hex digits");
        ec::Settings c = a;
        c.rate = {30, 1};
        Check(sg::SettingsHash(a, 60, 4, false) != sg::SettingsHash(c, 60, 4, false), "another frame rate: another hash");
        Check(sg::SettingsHash(a, 60, 4, false) != sg::SettingsHash(a, 30, 4, false), "another segment length: another hash");
        Check(sg::SettingsHash(a, 60, 4, false) != sg::SettingsHash(a, 60, 5, false), "another encoder: another hash");
        Check(sg::SettingsHash(a, 60, 4, false) != sg::SettingsHash(a, 60, 4, true), "[Export] Hdr10: another hash");
        ec::Settings d = a;
        wcscpy_s(d.lutPath, L"C:\\luts\\warm.cube");
        Check(sg::SettingsHash(a, 60, 4, false) != sg::SettingsHash(d, 60, 4, false), "a LUT: another hash");
        ec::Settings e = a;
        e.endMs += 1;
        Check(sg::SettingsHash(a, 60, 4, false) != sg::SettingsHash(e, 60, 4, false), "another range: another hash");
        Check(sg::SegmentFileName(2, ".mp4") == "seg-0003.mp4" && sg::SegmentFileName(9999, ".mov") == "seg-10000.mov", "the segments' names");
    }
}

int RunExportSegmentsTests()
{
    g_failures = 0;
    TestBoundaries();
    TestManifest();
    TestValidation();
    TestResume();
    TestJoin();
    TestSettingsHash();
    std::printf("export segments tests: %s\n", g_failures ? "FAILED" : "passed");
    return g_failures;
}
