#pragma once

// Whole project export, the add-on side's file work (render_export.cpp):
// where each piece's video pass and sound go, and the project's one sound
// file built from the pieces' audio passes (each cut where its own pass
// started and to exactly its frames' length, silence where a pass is
// missing or short), so the joined video keeps the sound in step at every
// seam. The WAV work is plain C++ on files (no FFmpeg, no WASAPI) so the
// core tests run it.

#include "project_export_plan.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace render_export
{
    // "C:\v\Chase_..._h264.mp4", 2 -> "C:\v\Chase_..._h264.part03.mp4" (an
    // image sequence's folder: "C:\v\Chase_..._png.part03").
    inline std::wstring ProjectPartPath(const std::wstring& finalPath, std::size_t index, bool folder)
    {
        wchar_t tag[16];
        std::swprintf(tag, 16, L".part%02u", static_cast<unsigned>(index + 1));
        if (folder)
            return finalPath + tag;
        const std::size_t dot = finalPath.find_last_of(L'.');
        const std::size_t slash = finalPath.find_last_of(L"\\/");
        if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
            return finalPath + tag;
        return finalPath.substr(0, dot) + tag + finalPath.substr(dot);
    }

    // One piece's video pass, as the finalize joins it.
    struct VideoPart
    {
        std::wstring path;
        std::uint64_t frames = 0;
    };

    // A finished piece (the add-on keeps them until the last one).
    struct ProjectPart
    {
        VideoPart video;
        std::wstring wav;         // its audio pass ("": none, silence)
        double audioStartS = 0.0; // where its range starts in that WAV
        std::wstring wavFile;     // the audio pass file to delete (even when not used)
    };

    // The format of a RIFF WAVE file and where its samples are.
    struct WavInfo
    {
        std::uint32_t sampleRate = 0;
        std::uint16_t channels = 0;
        std::uint16_t bitsPerSample = 0;
        bool isFloat = false;
        std::uint64_t dataOffset = 0;
        std::uint64_t dataBytes = 0;
        std::uint32_t BlockBytes() const { return static_cast<std::uint32_t>(channels) * bitsPerSample / 8u; }
    };

    namespace wavdetail
    {
        inline std::uint32_t Le32(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
        inline std::uint16_t Le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
        inline void Put32(std::FILE* f, std::uint32_t v)
        {
            const std::uint8_t b[4] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v >> 16),
                static_cast<std::uint8_t>(v >> 24)};
            std::fwrite(b, 1, 4, f);
        }
        inline void Put16(std::FILE* f, std::uint16_t v)
        {
            const std::uint8_t b[2] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8)};
            std::fwrite(b, 1, 2, f);
        }
    }

    // The head of a WAV file (the first bytes, at least up to the "data"
    // chunk's header). The data size of a capture cut short (0 or past the
    // file) is the caller's to clamp to the file.
    inline bool ParseWavHeader(const std::uint8_t* data, std::size_t size, WavInfo& info)
    {
        using namespace wavdetail;
        if (size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0)
            return false;
        bool haveFormat = false;
        std::size_t at = 12;
        while (at + 8 <= size)
        {
            const std::uint32_t bytes = Le32(data + at + 4);
            if (std::memcmp(data + at, "fmt ", 4) == 0)
            {
                if (bytes < 16 || at + 8 + 16 > size)
                    return false;
                const std::uint8_t* f = data + at + 8;
                const std::uint16_t tag = Le16(f);
                info.channels = Le16(f + 2);
                info.sampleRate = Le32(f + 4);
                info.bitsPerSample = Le16(f + 14);
                // WAVE_FORMAT_EXTENSIBLE: the sub format's first two bytes.
                const std::uint16_t sub = tag == 0xFFFE && bytes >= 40 && at + 8 + 26 <= size ? Le16(f + 24) : tag;
                info.isFloat = sub == 3;
                haveFormat = (sub == 1 || sub == 3) && info.channels > 0 && info.sampleRate > 0 && info.bitsPerSample >= 8 &&
                    info.bitsPerSample % 8 == 0;
            }
            else if (std::memcmp(data + at, "data", 4) == 0)
            {
                info.dataOffset = at + 8;
                info.dataBytes = bytes;
                return haveFormat;
            }
            at += 8 + bytes + (bytes & 1u);
        }
        return false;
    }

    inline bool SameWavFormat(const WavInfo& a, const WavInfo& b)
    {
        return a.sampleRate == b.sampleRate && a.channels == b.channels && a.bitsPerSample == b.bitsPerSample && a.isFloat == b.isFloat;
    }

    // One piece of the project's sound: `samples` frames of `wav` from
    // `startS` (silence where the file has none: before its start, past its
    // end, or no file at all).
    struct AudioPiece
    {
        std::wstring wav;
        double startS = 0.0;
        std::uint64_t samples = 0;
    };

    // The pieces of the project's sound for the parts as they came out (their
    // real frame counts): each one exactly its frames' length at the WAVs'
    // rate (projectexport::AudioSamples).
    inline std::vector<AudioPiece> PlanAudio(const std::vector<ProjectPart>& parts, replay::exporting::FrameRate rate, std::uint32_t sampleRate)
    {
        std::vector<AudioPiece> pieces;
        std::uint64_t first = 0;
        for (const ProjectPart& part : parts)
        {
            AudioPiece piece;
            piece.wav = part.wav;
            piece.startS = part.audioStartS;
            piece.samples = replay::projectexport::AudioSamples(first, part.video.frames, rate, sampleRate);
            pieces.push_back(piece);
            first += part.video.frames;
        }
        return pieces;
    }

    struct SpliceReport
    {
        std::uint64_t samples = 0;        // written
        std::uint64_t silentSamples = 0;  // of them, silence (a piece without sound, or short)
        std::uint32_t sampleRate = 0;
    };

    // Reads the WAV head of `path` (false: no file, or not a WAV).
    inline bool ReadWavInfo(const std::wstring& path, WavInfo& info)
    {
        std::FILE* f = nullptr;
        if (path.empty() || _wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
            return false;
        std::uint8_t head[4096];
        const std::size_t got = std::fread(head, 1, sizeof(head), f);
        std::fseek(f, 0, SEEK_END);
        const long long fileBytes = _ftelli64(f);
        std::fclose(f);
        if (!ParseWavHeader(head, got, info))
            return false;
        // A capture cut short (the sizes never patched) holds what is in the file.
        const std::uint64_t inFile = fileBytes > static_cast<long long>(info.dataOffset) ? static_cast<std::uint64_t>(fileBytes) - info.dataOffset : 0;
        if (info.dataBytes == 0 || info.dataBytes > inFile)
            info.dataBytes = inFile;
        return true;
    }

    // The project's sound: the pieces one after the other into `out` (the
    // format of the first readable piece; 48 kHz float stereo when none is).
    // A piece in another format counts as silence. False only when `out`
    // cannot be written.
    inline bool SpliceWav(const std::vector<AudioPiece>& pieces, const std::wstring& out, std::string& error, SpliceReport* report = nullptr)
    {
        using namespace wavdetail;
        WavInfo format;
        format.sampleRate = 48000;
        format.channels = 2;
        format.bitsPerSample = 32;
        format.isFloat = true;
        for (const AudioPiece& piece : pieces)
        {
            WavInfo info;
            if (ReadWavInfo(piece.wav, info))
            {
                format = info;
                break;
            }
        }
        std::FILE* f = nullptr;
        if (_wfopen_s(&f, out.c_str(), L"wb") != 0 || !f)
        {
            error = "cannot write the project's sound";
            return false;
        }
        const std::uint32_t block = format.BlockBytes();
        std::fwrite("RIFF", 1, 4, f);
        Put32(f, 0);
        std::fwrite("WAVEfmt ", 1, 8, f);
        Put32(f, 16);
        Put16(f, format.isFloat ? 3 : 1);
        Put16(f, format.channels);
        Put32(f, format.sampleRate);
        Put32(f, format.sampleRate * block);
        Put16(f, static_cast<std::uint16_t>(block));
        Put16(f, format.bitsPerSample);
        std::fwrite("data", 1, 4, f);
        Put32(f, 0);
        SpliceReport done;
        done.sampleRate = format.sampleRate;
        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(block) * 4096);
        const auto silence = [&](std::uint64_t samples) {
            std::fill(buffer.begin(), buffer.end(), std::uint8_t(0));
            while (samples)
            {
                const std::uint64_t n = std::min<std::uint64_t>(samples, 4096);
                std::fwrite(buffer.data(), block, static_cast<std::size_t>(n), f);
                samples -= n;
                done.samples += n;
                done.silentSamples += n;
            }
        };
        for (const AudioPiece& piece : pieces)
        {
            WavInfo info;
            std::FILE* in = nullptr;
            if (!ReadWavInfo(piece.wav, info) || !SameWavFormat(info, format) || _wfopen_s(&in, piece.wav.c_str(), L"rb") != 0 || !in)
            {
                silence(piece.samples);
                continue;
            }
            const std::uint64_t available = info.dataBytes / block;
            // Where the range starts in this WAV (a start before the file's
            // first sample is silence first).
            const long long start = std::llround(piece.startS * static_cast<double>(format.sampleRate));
            std::uint64_t left = piece.samples;
            if (start < 0)
            {
                const std::uint64_t lead = std::min<std::uint64_t>(left, static_cast<std::uint64_t>(-start));
                silence(lead);
                left -= lead;
            }
            std::uint64_t from = start > 0 ? static_cast<std::uint64_t>(start) : 0;
            _fseeki64(in, static_cast<long long>(info.dataOffset + from * block), SEEK_SET);
            while (left && from < available)
            {
                const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>({left, available - from, 4096}));
                const std::size_t got = std::fread(buffer.data(), block, want, in);
                if (got == 0)
                    break;
                std::fwrite(buffer.data(), block, got, f);
                done.samples += got;
                left -= got;
                from += got;
            }
            std::fclose(in);
            silence(left); // the pass ended early: the piece keeps its length
        }
        const std::uint64_t dataBytes = done.samples * block;
        std::fseek(f, 4, SEEK_SET);
        Put32(f, static_cast<std::uint32_t>(std::min<std::uint64_t>(36 + dataBytes, 0xFFFFFFFFull)));
        std::fseek(f, 40, SEEK_SET);
        Put32(f, static_cast<std::uint32_t>(std::min<std::uint64_t>(dataBytes, 0xFFFFFFFFull)));
        const bool ok = std::ferror(f) == 0;
        std::fclose(f);
        if (!ok)
        {
            error = "the project's sound could not be written (disk full?)";
            return false;
        }
        if (report)
            *report = done;
        return true;
    }

    // One chapter per piece, at its first frame ("1. 042-Valentine").
    inline std::vector<std::pair<std::uint64_t, std::string>> PartChapterTimes(const std::vector<std::string>& titles,
        const std::vector<std::uint64_t>& frames, replay::exporting::FrameRate rate)
    {
        std::vector<std::pair<std::uint64_t, std::string>> out;
        std::uint64_t first = 0;
        for (std::size_t k = 0; k < frames.size(); ++k)
        {
            const std::uint64_t ms = static_cast<std::uint64_t>(replay::projectexport::FrameTimeNs(rate, first) / 1000000);
            std::string title = std::to_string(k + 1) + ".";
            if (k < titles.size() && !titles[k].empty())
            {
                std::string name = titles[k];
                const std::size_t dot = name.rfind(".rpclip");
                if (dot != std::string::npos)
                    name = name.substr(0, dot);
                title += " " + name;
            }
            out.emplace_back(ms, title);
            first += frames[k];
        }
        return out;
    }
}
