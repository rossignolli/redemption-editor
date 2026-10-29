#include "replay_project.h"

#include "camera_marker_shake.h"

#include "clip_file.h"
#include "i18n.h" // the refusals in the player's language

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

// See replay_project.h.
namespace replay::project
{
    namespace
    {
        using camera::Keyframe;

        std::uint64_t Fnv64(const std::uint8_t* data, std::size_t size)
        {
            std::uint64_t hash = 0xCBF29CE484222325ull;
            for (std::size_t n = 0; n < size; ++n)
            {
                hash ^= data[n];
                hash *= 0x100000001B3ull;
            }
            return hash;
        }

        void Say(std::string* why, const char* text)
        {
            if (why)
                *why = text;
        }

        class Out
        {
        public:
            explicit Out(std::vector<std::uint8_t>& bytes) : m_bytes(bytes) {}
            template <typename T>
            void Put(T value)
            {
                static_assert(std::is_arithmetic_v<T>, "plain values only");
                const auto* raw = reinterpret_cast<const std::uint8_t*>(&value);
                m_bytes.insert(m_bytes.end(), raw, raw + sizeof(T));
            }
            void Text(const std::string& text)
            {
                Put(static_cast<std::uint16_t>(text.size()));
                m_bytes.insert(m_bytes.end(), text.begin(), text.end());
            }
            void Raw(const std::vector<std::uint8_t>& bytes) { m_bytes.insert(m_bytes.end(), bytes.begin(), bytes.end()); }

        private:
            std::vector<std::uint8_t>& m_bytes;
        };

        class In
        {
        public:
            In(const std::uint8_t* data, std::size_t size) : m_data(data), m_size(size) {}
            template <typename T>
            T Get()
            {
                T value{};
                if (!m_ok || m_size - m_pos < sizeof(T))
                {
                    m_ok = false;
                    return value;
                }
                std::memcpy(&value, m_data + m_pos, sizeof(T));
                m_pos += sizeof(T);
                return value;
            }
            std::string Text(std::size_t maxBytes)
            {
                const std::uint16_t size = Get<std::uint16_t>();
                if (!m_ok || size > maxBytes || m_size - m_pos < size)
                {
                    m_ok = false;
                    return {};
                }
                std::string text(reinterpret_cast<const char*>(m_data + m_pos), size);
                m_pos += size;
                return text;
            }
            const std::uint8_t* Take(std::size_t size)
            {
                if (!m_ok || m_size - m_pos < size)
                {
                    m_ok = false;
                    return nullptr;
                }
                const std::uint8_t* at = m_data + m_pos;
                m_pos += size;
                return at;
            }
            bool Ok() const { return m_ok; }
            std::size_t Left() const { return m_size - m_pos; }

        private:
            const std::uint8_t* m_data = nullptr;
            std::size_t m_size = 0;
            std::size_t m_pos = 0;
            bool m_ok = true;
        };

        // A clip file name as the gallery has it: no folder, no "..".
        bool PlainFileName(const std::string& file)
        {
            return !file.empty() && file.size() < 64 && file.find_first_of("\\/:*?\"<>|") == std::string::npos &&
                file.find("..") == std::string::npos;
        }

        // The marker in effect at timeMs: the last one before it, an Edit
        // marker over an Anchor (an Anchor carries no camera).
        const Keyframe* ActiveBefore(const std::vector<Keyframe>& keys, std::uint64_t timeMs)
        {
            const Keyframe* active = nullptr;
            const Keyframe* anchor = nullptr;
            for (const Keyframe& key : keys)
            {
                if (key.timeMs >= timeMs)
                    break;
                if (key.markerType == camera::MarkerType::Anchor)
                    anchor = &key;
                else
                    active = &key;
            }
            return active ? active : anchor;
        }

        const Keyframe* At(const std::vector<Keyframe>& keys, std::uint64_t timeMs)
        {
            for (const Keyframe& key : keys)
                if (key.timeMs == timeMs)
                    return &key;
            return nullptr;
        }

        // a + b, false when it does not fit in 64 bits.
        bool CheckedAdd(std::uint64_t a, std::uint64_t b, std::uint64_t& sum)
        {
            if (b > UINT64_MAX - a)
                return false;
            sum = a + b;
            return true;
        }

        // The clip file's length within what a clip can be (MakeClip's
        // shortest, the clip timestamps' u32 range), the trim inside it.
        bool ValidClip(const Clip& clip)
        {
            if (!PlainFileName(clip.file) || clip.rawDurationMs < kMinClipMs || clip.rawDurationMs > kMaxClipRawMs ||
                clip.inMs >= clip.outMs || clip.outMs > clip.rawDurationMs)
                return false;
            if (!clip.hasMarkers)
                return true;
            const std::vector<Keyframe>& keys = clip.markers.Keyframes();
            return !clip.markers.LegacyBlendAnyMode() && keys.size() >= 2 && keys.front().timeMs == clip.inMs &&
                keys.back().timeMs == clip.outMs;
        }
    }

    bool ValidName(const std::string& name)
    {
        if (name.empty() || name.size() > kMaxNameBytes || name.front() == ' ' || name.back() == ' ' || DeviceName(name))
            return false;
        for (const char c : name)
        {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
                c == '_';
            if (!ok)
                return false;
        }
        return true;
    }

    bool MakeClip(const std::string& file, std::uint64_t rawDurationMs, Clip& out)
    {
        if (!PlainFileName(file) || rawDurationMs < kMinClipMs || rawDurationMs > kMaxClipRawMs)
            return false;
        out = Clip{};
        out.file = file;
        out.rawDurationMs = rawDurationMs;
        out.inMs = 0;
        out.outMs = rawDurationMs;
        return true;
    }

    // ------------------------------------------------------------------
    // Edits.

    bool AddClip(Project& project, std::size_t index, const Clip& clip, std::string* why)
    {
        if (project.clips.size() >= kMaxClips)
        {
            Say(why, TR("project.msg.full", "The project is full (512 clips)."));
            return false;
        }
        if (!ValidClip(clip))
        {
            Say(why, TR("project.msg.cannot_add", "That clip cannot be added."));
            return false;
        }
        const std::uint64_t total = TotalMs(project);
        if (total > kMaxDurationMs || clip.DurationMs() > kMaxDurationMs - total)
        {
            Say(why, TR("project.msg.too_long", "A project lasts 30 minutes at most."));
            return false;
        }
        index = std::min(index, project.clips.size());
        project.clips.insert(project.clips.begin() + static_cast<std::ptrdiff_t>(index), clip);
        return true;
    }

    bool MoveClip(Project& project, std::size_t from, std::size_t to, std::string* why)
    {
        if (from >= project.clips.size() || to >= project.clips.size())
        {
            Say(why, TR("project.msg.no_clip", "No clip there."));
            return false;
        }
        if (from == to)
            return false;
        Clip picked = std::move(project.clips[from]);
        project.clips.erase(project.clips.begin() + static_cast<std::ptrdiff_t>(from));
        project.clips.insert(project.clips.begin() + static_cast<std::ptrdiff_t>(to), std::move(picked));
        return true;
    }

    bool DuplicateClip(Project& project, std::size_t index, std::string* why)
    {
        if (index >= project.clips.size())
        {
            Say(why, TR("project.msg.no_clip", "No clip there."));
            return false;
        }
        const Clip copy = project.clips[index];
        return AddClip(project, index + 1, copy, why);
    }

    bool DeleteClip(Project& project, std::size_t index, std::string* why)
    {
        if (index >= project.clips.size())
        {
            Say(why, TR("project.msg.no_clip", "No clip there."));
            return false;
        }
        project.clips.erase(project.clips.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

    bool SplitClip(Project& project, std::size_t index, std::uint64_t clipMs, std::string* why)
    {
        if (index >= project.clips.size())
        {
            Say(why, TR("project.msg.no_clip", "No clip there."));
            return false;
        }
        if (project.clips.size() >= kMaxClips)
        {
            Say(why, TR("project.msg.full", "The project is full (512 clips)."));
            return false;
        }
        const Clip& whole = project.clips[index];
        if (clipMs < whole.inMs + kMinClipMs || clipMs + kMinClipMs > whole.outMs)
        {
            Say(why, TR("project.msg.cut_edge", "Too close to the clip's start or end to cut there."));
            return false;
        }
        Clip first = whole;
        Clip second = whole;
        first.outMs = clipMs;
        second.inMs = clipMs;
        if (whole.hasMarkers)
        {
            first.markers = FitMarkers(whole.markers, first.inMs, first.outMs);
            second.markers = FitMarkers(whole.markers, second.inMs, second.outMs);
        }
        project.clips[index] = std::move(first);
        project.clips.insert(project.clips.begin() + static_cast<std::ptrdiff_t>(index) + 1, std::move(second));
        return true;
    }

    bool SetTrim(Project& project, std::size_t index, std::uint64_t inMs, std::uint64_t outMs, std::string* why)
    {
        if (index >= project.clips.size())
        {
            Say(why, TR("project.msg.no_clip", "No clip there."));
            return false;
        }
        Clip& clip = project.clips[index];
        outMs = std::min(outMs, clip.rawDurationMs);
        if (inMs >= outMs || outMs - inMs < kMinClipMs)
        {
            Say(why, TR("project.msg.too_short", "The clip would be too short."));
            return false;
        }
        if (inMs == clip.inMs && outMs == clip.outMs)
            return false;
        const std::uint64_t others = TotalMs(project) - clip.DurationMs();
        if (others > kMaxDurationMs || outMs - inMs > kMaxDurationMs - others)
        {
            Say(why, TR("project.msg.too_long", "A project lasts 30 minutes at most."));
            return false;
        }
        if (clip.hasMarkers)
            clip.markers = FitMarkers(clip.markers, inMs, outMs);
        clip.inMs = inMs;
        clip.outMs = outMs;
        return true;
    }

    // ------------------------------------------------------------------
    // Markers.

    camera::Track FitMarkers(const camera::Track& source, std::uint64_t inMs, std::uint64_t outMs,
        const camera::Keyframe& startTemplate, const camera::Keyframe& endTemplate)
    {
        camera::Track result;
        if (outMs <= inMs)
            return result;
        static const std::vector<Keyframe> kNone;
        const std::vector<Keyframe>& keys = source.LegacyBlendAnyMode() ? kNone : source.Keyframes();
        Keyframe start = startTemplate;
        Keyframe end = endTemplate;
        // The source markers that became the bounds by moving (a trim moves
        // the first and last marker to the new start and end): not kept
        // inside as well.
        const Keyframe* movedStart = nullptr;
        const Keyframe* movedEnd = nullptr;
        if (!keys.empty())
        {
            // The start: the marker there, else the one in effect at inMs,
            // else the clip's first marker (the trim moved before it).
            const Keyframe* from = At(keys, inMs);
            if (!from)
                from = ActiveBefore(keys, inMs);
            if (!from)
                from = movedStart = &keys.front();
            start = *from;
            if (from != &keys.front())
                start.transition = 0; // a fade in belongs to the clip's first marker
            // The end: the marker there, else the clip's last marker when
            // the trim moved past it, else the one in effect at outMs.
            const Keyframe* to = At(keys, outMs);
            if (!to && keys.back().timeMs < outMs)
                to = movedEnd = &keys.back();
            if (!to)
                to = ActiveBefore(keys, outMs);
            if (!to)
                to = &keys.front();
            end = *to;
            if (to != &keys.back())
                end.transition = 0; // a fade out belongs to the clip's last marker
        }
        start.timeMs = inMs;
        end.timeMs = outMs;
        result.AddMarker(start);
        result.AddMarker(end);
        for (const Keyframe& key : keys)
            if (key.timeMs > inMs && key.timeMs < outMs && &key != movedStart && &key != movedEnd)
                result.AddMarker(key); // refused when too close to a bound or a neighbour
        camera::ApplyLastMarkerRule(result);
        // The legacy view as a loaded clip has it (ReadCamera derives it):
        // the markers compare equal after a save and load of the project.
        result.DeriveLegacy();
        return result;
    }

    bool TakeMarkers(Clip& clip, const camera::Track& markers)
    {
        const std::vector<Keyframe>& keys = markers.Keyframes();
        if (markers.LegacyBlendAnyMode() || keys.size() < 2 || keys.back().timeMs > clip.rawDurationMs ||
            keys.back().timeMs <= keys.front().timeMs)
            return false;
        clip.markers = markers;
        clip.hasMarkers = true;
        clip.inMs = keys.front().timeMs;
        clip.outMs = keys.back().timeMs;
        return true;
    }

    // ------------------------------------------------------------------
    // Timeline.

    std::uint64_t TotalMs(const Project& project)
    {
        std::uint64_t total = 0;
        for (const Clip& clip : project.clips)
            if (!CheckedAdd(total, clip.DurationMs(), total))
                return UINT64_MAX; // held, never wrapped (Decode refuses such a project)
        return total;
    }

    std::uint64_t ClipStartMs(const Project& project, std::size_t index)
    {
        std::uint64_t start = 0;
        for (std::size_t n = 0; n < index && n < project.clips.size(); ++n)
            if (!CheckedAdd(start, project.clips[n].DurationMs(), start))
                return UINT64_MAX;
        return start;
    }

    bool Locate(const Project& project, std::uint64_t projectMs, std::size_t& index, std::uint64_t& clipMs)
    {
        if (project.clips.empty())
            return false;
        std::uint64_t start = 0;
        for (std::size_t n = 0; n < project.clips.size(); ++n)
        {
            const Clip& clip = project.clips[n];
            if (projectMs < start + clip.DurationMs())
            {
                index = n;
                clipMs = clip.inMs + (projectMs - start);
                return true;
            }
            start += clip.DurationMs();
        }
        index = project.clips.size() - 1;
        clipMs = project.clips.back().outMs;
        return true;
    }

    std::uint64_t ToProjectMs(const Project& project, std::size_t index, std::uint64_t clipMs)
    {
        if (index >= project.clips.size())
            return TotalMs(project);
        const Clip& clip = project.clips[index];
        return ClipStartMs(project, index) + (std::clamp(clipMs, clip.inMs, clip.outMs) - clip.inMs);
    }

    // ------------------------------------------------------------------
    // File.

    std::vector<std::uint8_t> Encode(const Project& project)
    {
        std::vector<std::uint8_t> bytes;
        Out out(bytes);
        out.Put(kMagic);
        out.Put(kFormatVersion);
        out.Put(static_cast<std::uint16_t>(clipfile::CameraSectionVersion()));
        out.Put(static_cast<std::uint16_t>(0));
        out.Put(TotalMs(project));
        out.Text(project.name);
        out.Put(static_cast<std::uint32_t>(project.clips.size()));
        for (const Clip& clip : project.clips)
        {
            std::vector<std::uint8_t> entry;
            Out e(entry);
            e.Text(clip.file);
            e.Put(clip.rawDurationMs);
            e.Put(clip.inMs);
            e.Put(clip.outMs);
            e.Put(static_cast<std::uint8_t>(clip.hasMarkers ? 1 : 0));
            // A legacy shake still waiting is folded first (the file keeps
            // only the markers' shake).
            camera::Track folded;
            if (clip.hasMarkers && clip.markers.HasLegacyShake())
            {
                folded = clip.markers;
                camera::mshake::Migrate(folded);
                folded.Migration() = {};
            }
            const std::vector<std::uint8_t> markers = !clip.hasMarkers ? std::vector<std::uint8_t>()
                : clipfile::EncodeCameraTrack(clip.markers.HasLegacyShake() ? folded : clip.markers);
            e.Put(static_cast<std::uint64_t>(markers.size()));
            e.Raw(markers);
            // Appended fields: an older build skips the entry's tail.
            e.Put(static_cast<std::uint8_t>(clip.hasEnvironment ? 1 : 0));
            std::uint8_t environment[envoverride::kBytes];
            envoverride::Pack(clip.hasEnvironment ? clip.environment : EnvironmentOverride{}, environment);
            for (std::uint8_t b : environment)
                e.Put(b);
            // The shake lives in the markers (CAMR v9), so the entry ends
            // here; an older entry's trailing shake events are still read and
            // folded in.
            out.Put(static_cast<std::uint32_t>(entry.size()));
            out.Raw(entry);
        }
        out.Put(Fnv64(bytes.data(), bytes.size()));
        return bytes;
    }

    bool Decode(const std::uint8_t* data, std::size_t size, Project& project, std::string& error)
    {
        if (!data || size < sizeof(std::uint64_t) + 8)
        {
            error = "project file too short";
            return false;
        }
        const std::size_t body = size - sizeof(std::uint64_t);
        std::uint64_t stored = 0;
        std::memcpy(&stored, data + body, sizeof(stored));
        In in(data, body);
        if (in.Get<std::uint32_t>() != kMagic)
        {
            error = "not a project file";
            return false;
        }
        const std::uint32_t version = in.Get<std::uint32_t>();
        if (!in.Ok() || version < kMinFormatVersion || version > kFormatVersion)
        {
            error = "project format " + std::to_string(version) + " not readable by this build";
            return false;
        }
        if (Fnv64(data, body) != stored)
        {
            error = "project file damaged (checksum)";
            return false;
        }
        const std::uint16_t markerVersion = in.Get<std::uint16_t>();
        in.Get<std::uint16_t>();
        in.Get<std::uint64_t>(); // the cached total: computed again
        Project loaded;
        loaded.name = in.Text(kMaxNameBytes);
        const std::uint32_t count = in.Get<std::uint32_t>();
        if (!in.Ok() || count > kMaxClips)
        {
            error = "project head damaged";
            return false;
        }
        loaded.clips.reserve(count);
        std::uint64_t totalMs = 0;
        for (std::uint32_t n = 0; n < count; ++n)
        {
            const std::uint32_t entryBytes = in.Get<std::uint32_t>();
            const std::uint8_t* entryData = in.Take(entryBytes);
            if (!in.Ok())
            {
                error = "clip " + std::to_string(n + 1) + " cut short";
                return false;
            }
            In e(entryData, entryBytes);
            Clip clip;
            clip.file = e.Text(63);
            clip.rawDurationMs = e.Get<std::uint64_t>();
            clip.inMs = e.Get<std::uint64_t>();
            clip.outMs = e.Get<std::uint64_t>();
            const std::uint8_t hasMarkers = e.Get<std::uint8_t>();
            const std::uint64_t markerBytes = e.Get<std::uint64_t>();
            const std::uint8_t* markerData = e.Ok() && markerBytes <= e.Left() ? e.Take(static_cast<std::size_t>(markerBytes)) : nullptr;
            if (!e.Ok() || hasMarkers > 1 || (markerBytes && !markerData) || (!hasMarkers && markerBytes))
            {
                error = "clip " + std::to_string(n + 1) + " damaged";
                return false;
            }
            // The environment settings, when the entry has them (otherwise
            // the clip file's own apply): 24 bytes (sun and moon), or 12 in
            // an older entry with nothing after them.
            if (e.Left() >= 1 + envoverride::kBytesV1)
            {
                const std::uint8_t hasEnvironment = e.Get<std::uint8_t>();
                const std::size_t envBytes = e.Left() >= envoverride::kBytes ? envoverride::kBytes : envoverride::kBytesV1;
                const std::uint8_t* environment = e.Take(envBytes);
                if (!e.Ok() || hasEnvironment > 1 || !environment ||
                    (hasEnvironment && !envoverride::Unpack(environment, envBytes, clip.environment)))
                {
                    error = "clip " + std::to_string(n + 1) + " has bad environment settings";
                    return false;
                }
                clip.hasEnvironment = hasEnvironment != 0;
            }
            // Legacy shake events, when the entry has them.
            std::uint16_t shakeVersion = 0;
            std::uint64_t shakeBytes = 0;
            const std::uint8_t* shakeData = nullptr;
            if (e.Left() >= sizeof(std::uint16_t) + sizeof(std::uint64_t))
            {
                shakeVersion = e.Get<std::uint16_t>();
                shakeBytes = e.Get<std::uint64_t>();
                shakeData = e.Ok() && shakeBytes <= e.Left() ? e.Take(static_cast<std::size_t>(shakeBytes)) : nullptr;
                if (!e.Ok() || (shakeBytes && !shakeData))
                {
                    error = "clip " + std::to_string(n + 1) + " has damaged shake events";
                    return false;
                }
            }
            // Bytes after these (a later version's fields) are skipped.
            clip.hasMarkers = hasMarkers != 0;
            if (clip.hasMarkers)
            {
                std::string markerError;
                if (!clipfile::DecodeCameraTrack(markerData, static_cast<std::size_t>(markerBytes), markerVersion, clip.markers,
                        markerError))
                {
                    error = "clip " + std::to_string(n + 1) + ": " + markerError;
                    return false;
                }
                if (shakeBytes && !clipfile::DecodeShakeEvents(shakeData, static_cast<std::size_t>(shakeBytes), shakeVersion,
                                      clip.markers, markerError))
                {
                    error = "clip " + std::to_string(n + 1) + ": " + markerError;
                    return false;
                }
                // Fold the legacy marker shake and events into the markers
                // (clip.markers.Migration()).
                camera::mshake::Migrate(clip.markers);
            }
            // The raw length and the trim within a clip's range (ValidClip),
            // the project's length summed without wrapping.
            if (!ValidClip(clip))
            {
                error = "clip " + std::to_string(n + 1) + " has a bad length, trim or file name";
                return false;
            }
            if (!CheckedAdd(totalMs, clip.DurationMs(), totalMs))
            {
                error = "clip " + std::to_string(n + 1) + " makes the project too long";
                return false;
            }
            loaded.clips.push_back(std::move(clip));
        }
        if (!in.Ok() || in.Left() != 0)
        {
            error = "project file has extra bytes";
            return false;
        }
        project = std::move(loaded);
        error.clear();
        return true;
    }

    bool SaveFile(const std::wstring& path, const Project& project, std::string& error)
    {
        const std::vector<std::uint8_t> bytes = Encode(project);
        const std::wstring temporary = path + L".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                error = "cannot create the project file";
                return false;
            }
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            file.flush();
            if (!file)
            {
                error = "cannot write the project file (disk full?)";
                file.close();
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return false;
            }
        }
        std::error_code code;
        std::filesystem::rename(temporary, path, code);
        if (code)
        {
            error = "cannot replace the project file: " + code.message();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
        error.clear();
        return true;
    }

    bool LoadFile(const std::wstring& path, Project& project, std::string& error, std::uint64_t maxBytes)
    {
        const std::string tooLarge = "project file too large (more than " + std::to_string(maxBytes) + " bytes)";
        // The size before anything is read (a large file of another kind is
        // refused here), and the same bound while reading (a file that grows).
        std::error_code sizeError;
        const std::uint64_t fileBytes = std::filesystem::file_size(std::filesystem::path(path), sizeError);
        if (!sizeError && fileBytes > maxBytes)
        {
            error = tooLarge;
            return false;
        }
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            error = "cannot open the project file";
            return false;
        }
        try
        {
            std::vector<std::uint8_t> bytes;
            if (!sizeError)
                bytes.reserve(static_cast<std::size_t>(fileBytes));
            char chunk[64 * 1024];
            while (file)
            {
                file.read(chunk, sizeof(chunk));
                const std::size_t got = static_cast<std::size_t>(file.gcount());
                if (got > maxBytes - bytes.size())
                {
                    error = tooLarge;
                    return false;
                }
                bytes.insert(bytes.end(), chunk, chunk + got);
            }
            if (file.bad())
            {
                error = "cannot read the project file";
                return false;
            }
            return Decode(bytes.data(), bytes.size(), project, error);
        }
        catch (const std::bad_alloc&)
        {
            // No memory for the file or its markers: a failed open, not an
            // exception for the script thread's recovery.
            error = "not enough memory to open the project file";
            return false;
        }
        catch (const std::length_error&)
        {
            error = "not enough memory to open the project file";
            return false;
        }
    }

    bool ReadSummary(const std::wstring& path, Summary& summary)
    {
        summary = Summary{};
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;
        std::uint8_t head[4 + 4 + 2 + 2 + 8 + 2 + kMaxNameBytes + 4]{};
        file.read(reinterpret_cast<char*>(head), sizeof(head));
        In in(head, static_cast<std::size_t>(file.gcount()));
        if (in.Get<std::uint32_t>() != kMagic)
            return false;
        summary.formatVersion = in.Get<std::uint32_t>();
        in.Get<std::uint16_t>();
        in.Get<std::uint16_t>();
        summary.totalMs = in.Get<std::uint64_t>();
        summary.name = in.Text(kMaxNameBytes);
        summary.clipCount = in.Get<std::uint32_t>();
        summary.readable = in.Ok() && summary.formatVersion >= kMinFormatVersion && summary.formatVersion <= kFormatVersion;
        return in.Ok();
    }

    // ------------------------------------------------------------------
    // Export hook.

    std::vector<ExportSegment> ExportPlan(const Project& project)
    {
        std::vector<ExportSegment> plan;
        plan.reserve(project.clips.size());
        std::uint64_t start = 0;
        for (const Clip& clip : project.clips)
        {
            ExportSegment segment;
            segment.file = clip.file;
            segment.inMs = clip.inMs;
            segment.outMs = clip.outMs;
            segment.projectStartMs = start;
            segment.ownMarkers = clip.hasMarkers;
            if (clip.hasMarkers)
                segment.markers = clip.markers;
            segment.ownEnvironment = clip.hasEnvironment;
            if (clip.hasEnvironment)
                segment.environment = clip.environment;
            plan.push_back(std::move(segment));
            start += clip.DurationMs();
        }
        return plan;
    }
}
