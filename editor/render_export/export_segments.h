#pragma once

// Render export: the segmented export ([Export] SegmentSeconds). A long
// export is written as fixed lengths of video (60 s by default), each its own
// file in a folder next to the requested one (<output>.parts\seg-0003.mp4),
// each from a fresh encoder (it starts on a keyframe and the closed one gives
// its memory back). A manifest in the folder, rewritten after each segment
// closes, says which segments are done; when the last one closes they are
// joined without a re-encode (libav_encoder.h LibavFinalizeParts) and the
// folder goes. An export cut short (a crash, a cancel, a failure) keeps them:
// the same export asked again continues from the first missing segment.
//
// The sound is not cut into the segments' files: the audio pass (a second,
// real-time playback of the whole range, audio_loopback.cpp) gives one
// continuous WAV, muxed once into the joined file, so a seam has no gap, no
// overlap and no click (an AAC stream per segment would add its encoder
// delay at every seam). Each segment's share of it is exact arithmetic,
// recorded in the manifest: from the sample where its first frame starts to
// the next segment's (projectexport::AudioSampleAt), the shares adding up
// to the whole video's samples at any frame rate.
//
// A resume renders a pre-roll before the first missing segment (the world,
// the particles and the game's motion blur settle; nothing is written),
// then writes from the segment's exact first frame. Where it starts is not
// computed from the markers: the export that wrote the manifest saw the
// playhead of that very frame and recorded it with the segment before.
//
// Pure (unit tested in ReplayCoreTests, tests/export_segments_tests.cpp).

#include "export_channel.h"
#include "export_schedule.h"
#include "project_export_plan.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace render_export::segments
{
    namespace ex = replay::exporting;

    // The audio pass's rate (audio_loopback.cpp: 48 kHz float stereo).
    constexpr std::uint32_t kSampleRate = 48000;
    constexpr std::uint32_t kMinSeconds = 5;
    constexpr std::uint32_t kMaxSeconds = 3600;
    constexpr std::uint32_t kManifestVersion = 1;

    // [Export] SegmentSeconds: 0 (one file, as before) or 5 to 3600.
    inline std::uint32_t ClampSeconds(long long value) noexcept
    {
        if (value <= 0)
            return 0;
        return value < kMinSeconds ? kMinSeconds : value > kMaxSeconds ? kMaxSeconds : static_cast<std::uint32_t>(value);
    }

    // The first frame of segment `k`: the first whose start is at or after
    // k * seconds of video (exact, no drift at 30000/1001).
    inline std::uint64_t StartFrame(std::uint64_t k, std::uint32_t seconds, ex::FrameRate rate) noexcept
    {
        if (!ex::ValidRate(rate) || seconds == 0)
            return 0;
        const std::uint64_t scaled = k * seconds * rate.num;
        return (scaled + rate.den - 1) / rate.den;
    }

    // The segments of `total` frames (0 for none).
    inline std::uint32_t Count(std::uint64_t total, std::uint32_t seconds, ex::FrameRate rate) noexcept
    {
        if (total == 0 || !ex::ValidRate(rate) || seconds == 0)
            return total ? 1u : 0u;
        return static_cast<std::uint32_t>((total - 1) * rate.den / (static_cast<std::uint64_t>(seconds) * rate.num) + 1);
    }

    // The segment that holds frame `frame`.
    inline std::uint32_t SegmentOf(std::uint64_t frame, std::uint32_t seconds, ex::FrameRate rate) noexcept
    {
        if (!ex::ValidRate(rate) || seconds == 0)
            return 0;
        return static_cast<std::uint32_t>(frame * rate.den / (static_cast<std::uint64_t>(seconds) * rate.num));
    }

    // A segment in frames and in audio samples (at kSampleRate).
    struct Bounds
    {
        std::uint64_t first = 0;  // its first frame in the whole video
        std::uint64_t frames = 0; // the last segment may be shorter
        std::uint64_t firstSample = 0;
        std::uint64_t samples = 0;
    };

    inline Bounds BoundsOf(std::uint32_t k, std::uint64_t total, std::uint32_t seconds, ex::FrameRate rate) noexcept
    {
        Bounds b;
        b.first = StartFrame(k, seconds, rate);
        if (b.first > total)
            b.first = total;
        std::uint64_t end = StartFrame(k + 1ull, seconds, rate);
        if (end > total)
            end = total;
        b.frames = end - b.first;
        b.firstSample = replay::projectexport::AudioSampleAt(b.first, rate, kSampleRate);
        b.samples = replay::projectexport::AudioSamples(b.first, b.frames, rate, kSampleRate);
        return b;
    }

    // A resume's pre-roll: `ms` of video, rounded up to whole frames.
    inline std::uint64_t PrerollFrames(std::uint32_t ms, ex::FrameRate rate) noexcept
    {
        if (!ex::ValidRate(rate))
            return 0;
        const std::uint64_t scaled = static_cast<std::uint64_t>(ms) * rate.num;
        const std::uint64_t per = 1000ull * rate.den;
        return (scaled + per - 1) / per;
    }

    // Where a resume of segment `k` starts its video pass: `preroll` frames
    // before the segment, never before frame 0 nor before `runStart` (the
    // first frame the recording run rendered: it saw no playhead earlier).
    inline std::uint64_t ResumeFrame(std::uint32_t k, std::uint64_t preroll, std::uint32_t seconds, ex::FrameRate rate,
        std::uint64_t runStart) noexcept
    {
        const std::uint64_t first = StartFrame(k, seconds, rate);
        const std::uint64_t at = first > preroll ? first - preroll : 0;
        return at > runStart ? at : (runStart < first ? runStart : first);
    }

    // ------------------------------------------------------------------
    // The manifest (manifest.txt in the parts folder, written to a
    // temporary name and moved over the old one after each segment).
    // ------------------------------------------------------------------

    struct Record
    {
        std::uint32_t index = 0;
        std::uint64_t frames = 0;
        std::uint64_t firstSample = 0;
        std::uint64_t samples = 0;
        std::string file;               // seg-0001.mp4, in the parts folder
        bool hasResume = false;         // where the next segment's resume starts:
        std::uint64_t resumeFrame = 0;  //   its pre-roll's first frame
        std::uint64_t resumeMs = 0;     //   and the playhead the export had there
    };

    struct Manifest
    {
        std::uint32_t version = kManifestVersion;
        std::string settingsHash;  // SettingsHash
        std::string identity;      // the clip and its edits (the dialog's)
        std::string finalOutput;   // the requested file, UTF-8
        std::string extension;     // the segments' (the requested file's)
        ex::FrameRate rate;
        std::uint32_t seconds = 0;
        std::uint64_t totalFrames = 0;
        std::uint32_t sampleRate = kSampleRate;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<Record> segments; // done, in order
        bool audioDone = false;       // the audio pass of the whole range is in audioFile
        std::string audioFile;
        double audioStartS = 0.0;     // where frame 0 is in it

        std::uint32_t SegmentCount() const noexcept { return Count(totalFrames, seconds, rate); }
    };

    inline std::string OneLine(std::string text)
    {
        for (char& c : text)
            if (c == '\r' || c == '\n')
                c = ' ';
        return text;
    }

    // "seg-0003.mp4" for k = 2.
    inline std::string SegmentFileName(std::uint32_t k, const std::string& extension)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "seg-%04u", k + 1);
        return name + extension;
    }

    inline std::string Format(const Manifest& m)
    {
        std::string out = "# Redemption Editor: a segmented video export not joined yet (editor/render_export/export_segments.h)\n";
        char line[512];
        std::snprintf(line, sizeof(line), "version=%u\n", m.version);
        out += line;
        out += "settings=" + OneLine(m.settingsHash) + "\n";
        out += "identity=" + OneLine(m.identity) + "\n";
        out += "final=" + OneLine(m.finalOutput) + "\n";
        out += "extension=" + OneLine(m.extension) + "\n";
        std::snprintf(line, sizeof(line), "rate=%u/%u\nsegment_seconds=%u\ntotal_frames=%llu\nsample_rate=%u\nsize=%ux%u\n", m.rate.num, m.rate.den,
            m.seconds, static_cast<unsigned long long>(m.totalFrames), m.sampleRate, m.width, m.height);
        out += line;
        for (const Record& r : m.segments)
        {
            std::snprintf(line, sizeof(line), "segment=%u,frames=%llu,first_sample=%llu,samples=%llu", r.index + 1,
                static_cast<unsigned long long>(r.frames), static_cast<unsigned long long>(r.firstSample),
                static_cast<unsigned long long>(r.samples));
            out += line;
            if (r.hasResume)
            {
                std::snprintf(line, sizeof(line), ",resume_frame=%llu,resume_ms=%llu", static_cast<unsigned long long>(r.resumeFrame),
                    static_cast<unsigned long long>(r.resumeMs));
                out += line;
            }
            out += ",file=" + OneLine(r.file) + "\n"; // last: the rest of the line
        }
        if (m.audioDone)
        {
            std::snprintf(line, sizeof(line), "audio_start_s=%.6f\n", m.audioStartS);
            out += line;
            out += "audio=" + OneLine(m.audioFile) + "\n";
        }
        out += "end\n"; // a file cut short has no end line
        return out;
    }

    namespace detail
    {
        inline bool Number(const std::string& text, std::uint64_t& out)
        {
            if (text.empty() || text.size() > 20)
                return false;
            out = 0;
            for (char c : text)
            {
                if (c < '0' || c > '9')
                    return false;
                out = out * 10 + static_cast<std::uint64_t>(c - '0');
            }
            return true;
        }

        // "frames=3600" fields of a segment line, up to ",file=" (the rest).
        inline bool Field(const std::string& line, const char* key, std::uint64_t& out)
        {
            const std::string tag = std::string(key) + "=";
            std::size_t at = line.find(tag);
            while (at != std::string::npos && at != 0 && line[at - 1] != ',')
                at = line.find(tag, at + 1);
            if (at == std::string::npos)
                return false;
            const std::size_t from = at + tag.size();
            const std::size_t to = line.find(',', from);
            return Number(line.substr(from, to == std::string::npos ? std::string::npos : to - from), out);
        }
    }

    // False (with `why`) for a file that is not a whole manifest of this version.
    inline bool Parse(const std::string& text, Manifest& m, std::string& why)
    {
        m = Manifest{};
        bool ended = false, haveVersion = false, haveRate = false;
        std::size_t at = 0;
        while (at < text.size())
        {
            std::size_t end = text.find('\n', at);
            if (end == std::string::npos)
                end = text.size();
            std::string line = text.substr(at, end - at);
            at = end + 1;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty() || line[0] == '#')
                continue;
            if (line == "end")
            {
                ended = true;
                break;
            }
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos)
            {
                why = "bad line: " + line;
                return false;
            }
            const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
            std::uint64_t n = 0;
            if (key == "version")
            {
                if (!detail::Number(value, n) || n != kManifestVersion)
                {
                    why = "version " + value;
                    return false;
                }
                haveVersion = true;
            }
            else if (key == "settings")
                m.settingsHash = value;
            else if (key == "identity")
                m.identity = value;
            else if (key == "final")
                m.finalOutput = value;
            else if (key == "extension")
                m.extension = value;
            else if (key == "rate")
            {
                const std::size_t slash = value.find('/');
                std::uint64_t num = 0, den = 0;
                if (slash == std::string::npos || !detail::Number(value.substr(0, slash), num) || !detail::Number(value.substr(slash + 1), den))
                {
                    why = "bad rate";
                    return false;
                }
                m.rate = {static_cast<std::uint32_t>(num), static_cast<std::uint32_t>(den)};
                haveRate = ex::ValidRate(m.rate);
            }
            else if (key == "segment_seconds" && detail::Number(value, n))
                m.seconds = static_cast<std::uint32_t>(n);
            else if (key == "total_frames" && detail::Number(value, n))
                m.totalFrames = n;
            else if (key == "sample_rate" && detail::Number(value, n))
                m.sampleRate = static_cast<std::uint32_t>(n);
            else if (key == "size")
            {
                const std::size_t x = value.find('x');
                std::uint64_t w = 0, h = 0;
                if (x != std::string::npos && detail::Number(value.substr(0, x), w) && detail::Number(value.substr(x + 1), h))
                {
                    m.width = static_cast<std::uint32_t>(w);
                    m.height = static_cast<std::uint32_t>(h);
                }
            }
            else if (key == "segment")
            {
                Record r;
                std::uint64_t index = 0;
                const std::size_t file = line.find(",file=");
                if (!detail::Field(line, "segment", index) || index == 0 || !detail::Field(line, "frames", r.frames) ||
                    !detail::Field(line, "first_sample", r.firstSample) || !detail::Field(line, "samples", r.samples) || file == std::string::npos)
                {
                    why = "bad segment line: " + line;
                    return false;
                }
                r.index = static_cast<std::uint32_t>(index - 1);
                r.file = line.substr(file + 6);
                r.hasResume = detail::Field(line, "resume_frame", r.resumeFrame) && detail::Field(line, "resume_ms", r.resumeMs);
                m.segments.push_back(r);
            }
            else if (key == "audio_start_s")
                m.audioStartS = std::strtod(value.c_str(), nullptr);
            else if (key == "audio")
            {
                m.audioFile = value;
                m.audioDone = !value.empty();
            }
            // Unknown keys of a later minor change are skipped.
        }
        if (!ended)
        {
            why = "cut short (no end line)";
            return false;
        }
        if (!haveVersion || !haveRate || m.seconds == 0 || m.totalFrames == 0 || m.settingsHash.empty())
        {
            why = "header incomplete";
            return false;
        }
        return true;
    }

    // The segments of `m` that can be kept: the leading ones in order, each
    // exactly its planned frames and samples, and (unless every segment is
    // there) the last one kept naming where the next one's resume starts.
    inline std::uint32_t DoneSegments(const Manifest& m)
    {
        const std::uint32_t count = m.SegmentCount();
        std::uint32_t valid = 0;
        while (valid < m.segments.size() && valid < count)
        {
            const Record& r = m.segments[valid];
            const Bounds b = BoundsOf(valid, m.totalFrames, m.seconds, m.rate);
            if (r.index != valid || r.frames != b.frames || r.firstSample != b.firstSample || r.samples != b.samples || r.file.empty())
                break;
            ++valid;
        }
        while (valid > 0 && valid < count && !m.segments[valid - 1].hasResume)
            --valid;
        return valid;
    }

    enum class ResumeCheck
    {
        Resumable = 0,   // some segments done: offer to continue
        Nothing,         // nothing usable in it (no segment done): the folder may go
        SettingsChanged, // the same clip with other settings: left alone
        OtherExport,     // another clip, range or edit
    };

    inline ResumeCheck Check(const Manifest& m, const std::string& settingsHash, const std::string& identity)
    {
        if (m.identity != OneLine(identity))
            return ResumeCheck::OtherExport;
        if (m.settingsHash != settingsHash)
            return ResumeCheck::SettingsChanged;
        return DoneSegments(m) == 0 && !m.audioDone ? ResumeCheck::Nothing : ResumeCheck::Resumable;
    }

    // What a resume does: the video pass from `firstFrame` (the recorded
    // pre-roll start, at playhead `startMs`), `frames` frames of it (the
    // pre-roll and the rest of the video), written from `writeFrom` (the
    // first missing segment's first frame) on. `videoDone`: every segment is
    // there (no video pass; the audio pass, unless done, and the join).
    struct ResumePlan
    {
        std::uint32_t segment = 0;
        std::uint64_t firstFrame = 0;
        std::uint64_t writeFrom = 0;
        std::uint64_t frames = 0;
        std::uint64_t startMs = 0;
        bool videoDone = false;
    };

    inline bool PlanResume(const Manifest& m, ResumePlan& plan)
    {
        plan = ResumePlan{};
        const std::uint32_t done = DoneSegments(m);
        const std::uint32_t count = m.SegmentCount();
        if (done == 0)
            return false;
        plan.segment = done;
        if (done >= count)
        {
            plan.videoDone = true;
            plan.firstFrame = plan.writeFrom = m.totalFrames;
            return true;
        }
        const Record& before = m.segments[done - 1];
        plan.writeFrom = StartFrame(done, m.seconds, m.rate);
        plan.firstFrame = before.resumeFrame <= plan.writeFrom ? before.resumeFrame : plan.writeFrom;
        plan.startMs = before.resumeMs;
        plan.frames = m.totalFrames - plan.firstFrame;
        return true;
    }

    // The join: the segments' files in order (paths relative to the parts
    // folder) and their frames. False (with `why`) unless every segment is
    // there and they add up to the whole.
    struct JoinPart
    {
        std::string file;
        std::uint64_t frames = 0;
    };

    inline bool PlanJoin(const Manifest& m, std::vector<JoinPart>& parts, std::string& why)
    {
        parts.clear();
        const std::uint32_t count = m.SegmentCount();
        if (DoneSegments(m) < count || m.segments.size() < count)
        {
            why = "segments missing: " + std::to_string(DoneSegments(m)) + " of " + std::to_string(count);
            return false;
        }
        std::uint64_t frames = 0, samples = 0;
        for (std::uint32_t k = 0; k < count; ++k)
        {
            parts.push_back({m.segments[k].file, m.segments[k].frames});
            frames += m.segments[k].frames;
            samples += m.segments[k].samples;
        }
        if (frames != m.totalFrames || samples != replay::projectexport::AudioSampleAt(m.totalFrames, m.rate, kSampleRate))
        {
            why = "the segments do not add up to the video";
            return false;
        }
        return true;
    }

    // ------------------------------------------------------------------
    // What a resume must match.
    // ------------------------------------------------------------------

    // FNV-1a, 64 bit, as 16 hex digits.
    inline std::string HashText(const std::string& text)
    {
        std::uint64_t h = 14695981039346656037ull;
        for (unsigned char c : text)
        {
            h ^= c;
            h *= 1099511628211ull;
        }
        char out[17];
        std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(h));
        return out;
    }

    // The settings that change the picture, the segments or the sound, as
    // the dialog asked them (the output name is not one: it has the time
    // of day). `variant`: the encoder the export will open; `hdr10`: [Export]
    // Hdr10 (the back buffer read as PQ).
    inline std::string SettingsKey(const replay::exportchannel::Settings& s, std::uint32_t seconds, int variant, bool hdr10)
    {
        char text[512];
        std::snprintf(text, sizeof(text),
            "start=%llu;end=%llu;rate=%u/%u;samples=%u;shutter=%.3f;preroll=%u;stage=%u;preset=%u;container=%u;audio=%u;quality=%u;hdr=%u;"
            "letterbox=%.4f;grain=%.4f;chapters=%u;offset=%d;segment=%u;variant=%d;hdr10=%d;lut=",
            static_cast<unsigned long long>(s.startMs), static_cast<unsigned long long>(s.endMs), s.rate.num, s.rate.den, s.samples,
            static_cast<double>(s.shutterDegrees), s.prerollFrames, static_cast<unsigned>(s.stage), static_cast<unsigned>(s.preset),
            static_cast<unsigned>(s.container), static_cast<unsigned>(s.audio), s.quality, s.hdrOutput, static_cast<double>(s.letterboxAspect),
            static_cast<double>(s.grain), s.chapters, s.audioOffsetMs, seconds, variant, hdr10 ? 1 : 0);
        std::string key = text;
        for (std::size_t i = 0; i < replay::exportchannel::kPathCapacity && s.lutPath[i]; ++i)
            key += std::to_string(static_cast<unsigned>(s.lutPath[i])) + ".";
        return key;
    }

    inline std::string SettingsHash(const replay::exportchannel::Settings& s, std::uint32_t seconds, int variant, bool hdr10)
    {
        return HashText(SettingsKey(s, seconds, variant, hdr10));
    }
}
