#pragma once

// Clip on disk: a header, then one block per track. A recording, its pose
// tracks and its camera track are written as
//
//   char magic[8] "RPOCCLIP", u32 format version, u64 duration ms
//   then sections: u32 tag (fourcc), u16 section version, u64 payload
//   length, payload
//
// one section per clip member of replay::Recording, one for the pose
// records and one for the camera track. Every section has its own version
// next to its writer in clip_file.cpp. There is no migration: Load refuses a
// file with an unknown, duplicated or missing section, or with a section
// whose version differs from this build's, naming the section.
//
// Session values never reach the file: script handles and heap pointers are
// written as 0 (CreatePacket::sourceHandle, SpeechEvent::voiceObject,
// MeleeEvent::info, BodyImpactEvent::definition, the entity references in a
// shot's fire info, the tracker and engine-owned fields of sound params),
// the objects alive before the recording and the adopted objects are not
// written, and pointers into the game image (MeleeEvent::infoVtable,
// BodyImpactEvent::definitionVtable, FireStartEvent desc +0xF0,
// SoundEvent::globalAudio) are written as an offset from the image base (0
// when outside the image) and rebased on load. The image range is a
// parameter so the tests can give their own.
//
// Pure data: no engine or native access, unit tested in ReplayCoreTests.

#include "camera_track.h"
#include "pose_stream.h"
#include "replay_recording.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace replay::clipfile
{
    constexpr char kMagic[8] = {'R', 'P', 'O', 'C', 'C', 'L', 'I', 'P'};
    // 2: each payload is followed by its FNV-1a 64 hash.
    // 3: the game camera section.
    // 4: the environment section (clock and weather).
    constexpr std::uint32_t kFormatVersion = 4;

    // The clip in memory as it is written (and read back).
    bool Encode(
        const Recording& recording,
        const std::vector<pose::PoseRecordExport>& poses,
        const camera::Track& camera,
        std::uint64_t durationMs,
        std::uintptr_t imageBase,
        std::size_t imageSize,
        std::vector<std::uint8_t>& out,
        std::string& error);
    // An optional section whose payload the reader refuses (a packet out of
    // its range, a count the bytes cannot hold, bytes left over) is left out,
    // the clip's member empty as if the clip were from before that section,
    // and the clip opens; the loader logs
    // CLIP_SECTION_SKIPPED,section=<name>,reason=<reason> for each one. A
    // required section, a version this build does not read, a hash that does
    // not match or a truncated file still refuse the clip. `reason` says where
    // the reader stopped, in payload bytes.
    struct SkippedSection
    {
        std::string section;
        std::string reason;
    };

    // On failure the outputs are left as they were. `skipped` (may be null)
    // receives the optional sections left out.
    bool Decode(
        const std::uint8_t* data,
        std::size_t size,
        std::uintptr_t imageBase,
        std::size_t imageSize,
        Recording& recording,
        std::vector<pose::PoseRecordExport>& poses,
        camera::Track& camera,
        std::uint64_t& durationMs,
        std::string& error);
    bool Decode(
        const std::uint8_t* data,
        std::size_t size,
        std::uintptr_t imageBase,
        std::size_t imageSize,
        Recording& recording,
        std::vector<pose::PoseRecordExport>& poses,
        camera::Track& camera,
        std::uint64_t& durationMs,
        std::string& error,
        std::vector<SkippedSection>* skipped);

    // Writes `path` through a temporary file next to it, renamed over the
    // target once complete, so a failed save never leaves half a clip.
    // `poses` comes from pose::capture::ExportRecords.
    bool Save(
        const Recording& recording,
        const std::vector<pose::PoseRecordExport>& poses,
        const camera::Track& camera,
        std::uint64_t durationMs,
        std::uintptr_t imageBase,
        std::size_t imageSize,
        const std::wstring& path,
        std::string& error);

    // The same, the file written already packed (clip_compress.h
    // PackForSave: the packed container with zstd blocks and the clip
    // list's summary frame) when `pack` is set. A pack that fails (low
    // memory, out of memory, a check that does not match, cancelled) is not
    // a failed save: the clip is written plain as before (encoded again
    // when the pack took its bytes) and `info` says why.
    struct SaveOptions
    {
        bool pack = false;
        int zstdLevel = 3;      // clipcompress::kSavePackLevel
        unsigned threads = 4;   // clipcompress::kSavePackThreads
        std::uint64_t availableBytes = 0; // 0: the system's (clipcompress::AvailableMemoryBytes)
        const std::atomic<bool>* cancel = nullptr; // stops the pack (the clip is then written plain)
    };
    struct SaveInfo
    {
        bool packed = false;
        std::string fallback; // why a pack asked for was not done
        std::uint64_t clipBytes = 0;
        std::uint64_t fileBytes = 0;
        std::uint32_t encodeMs = 0;
        std::uint32_t packMs = 0;   // the pack
        std::uint32_t verifyMs = 0; // its check
        std::uint32_t writeMs = 0;
    };
    bool Save(
        const Recording& recording,
        const std::vector<pose::PoseRecordExport>& poses,
        const camera::Track& camera,
        std::uint64_t durationMs,
        std::uintptr_t imageBase,
        std::size_t imageSize,
        const std::wstring& path,
        std::string& error,
        const SaveOptions& options,
        SaveInfo* info);
    // Reads a whole clip. On success `recording` is a fresh clip (the
    // objects before the recording and the adopted objects are empty),
    // `poses` goes to pose::capture::ImportRecords and `camera` holds the
    // saved keyframes. On failure nothing is changed and `error` says why.
    bool Load(
        const std::wstring& path,
        std::uintptr_t imageBase,
        std::size_t imageSize,
        Recording& recording,
        std::vector<pose::PoseRecordExport>& poses,
        camera::Track& camera,
        std::uint64_t& durationMs,
        std::string& error);

    // A compressed clip (clip_compress.h, a zstd frame of these
    // same bytes) is decompressed into memory, then parsed the same way.
    // `info` (may be null) says whether it was and what it cost.
    struct LoadInfo
    {
        bool compressed = false;
        bool packed = false; // the packed container (clip_pack.h)
        std::uint64_t fileBytes = 0;
        std::uint64_t clipBytes = 0;
        std::uint32_t decompressMs = 0;
        std::vector<SkippedSection> skipped; // the optional sections left out
    };
    bool Load(
        const std::wstring& path,
        std::uintptr_t imageBase,
        std::size_t imageSize,
        Recording& recording,
        std::vector<pose::PoseRecordExport>& poses,
        camera::Track& camera,
        std::uint64_t& durationMs,
        std::string& error,
        LoadInfo* info);

    // Load checks the file's size and first bytes (magic, format version, a
    // compressed frame's content size) before it reads the file, and refuses a
    // clip whose load would not fit in memory: the file buffer, the
    // decompressed copy and the decoded clip (about kDecodeFactor times the
    // clip's bytes) plus kLoadReserveBytes kept for the game, against
    // `availableBytes` (the commit the system has left). clipBytes 0: unknown
    // (a frame without its size; Decompress bounds it). An allocation failure
    // while reading, decompressing or decoding comes back as an error, never as
    // an exception. A packed file (`packed`) also holds its streams (about the
    // clip's size) while the clip is rebuilt. Pure.
    constexpr std::uint64_t kDecodeFactor = 2;
    constexpr std::uint64_t kLoadReserveBytes = 256ull << 20;
    bool CheckLoadBudget(std::uint64_t fileBytes, std::uint64_t clipBytes, bool compressed, std::uint64_t availableBytes,
        std::string& error, bool packed = false);

    // The clip's content as playback reads it, after the sections parsed:
    // the duration within the timestamps' range (u32 ms), the markers usable
    // (finite pose and offsets, an orientation that is not null, a finite FOV,
    // ids unique and below the next id), the game camera samples finite.
    // Values of a known range that an older build could have written outside
    // it are brought into it (burn levels to [0, 1], a marker's FOV to
    // [1, 170]); the rest refuses the clip. Decode runs it.
    bool ValidateLoaded(Recording& recording, camera::Track& camera, std::uint64_t durationMs, std::string& error);

    // The camera markers alone, as the clip's CAMR section writes them (a
    // project keeps each clip's markers this way). Decode takes the section
    // version the bytes were written with (any the clip reader still reads) and
    // runs the marker checks of ValidateLoaded; on failure `track` is left as
    // it was.
    std::uint16_t CameraSectionVersion();
    std::vector<std::uint8_t> EncodeCameraTrack(const camera::Track& track);
    bool DecodeCameraTrack(const std::uint8_t* data, std::size_t size, std::uint16_t version, camera::Track& track,
        std::string& error);
    // The shake events track (CFXE) alone, for a project's clip entry; Decode
    // replaces only track.LegacyEffects() (an input, folded into the markers
    // by the caller).
    std::uint16_t ShakeEventsSectionVersion();
    std::vector<std::uint8_t> EncodeShakeEvents(const camera::Track& track);
    bool DecodeShakeEvents(const std::uint8_t* data, std::size_t size, std::uint16_t version, camera::Track& track,
        std::string& error);

    // The name of a new clip file. `base` (the readable
    // clip-YYYYMMDD-HHMMSS[-pN]) when `taken` says it is free, else base-2,
    // base-3 and on: two clips saved in the same second never share a file.
    // `taken(name)` tells whether a name is on disk or held by a save not
    // written yet. Empty when none of 1000 is free.
    template <typename Taken>
    std::wstring UniqueClipName(const std::wstring& base, Taken&& taken)
    {
        if (!taken(base))
            return base;
        for (int suffix = 2; suffix <= 1000; ++suffix)
        {
            std::wstring name = base + L"-" + std::to_wstring(suffix);
            if (!taken(name))
                return name;
        }
        return std::wstring();
    }

    // Clip names "NNN-Place": a new clip is 042-Valentine.rpclip, a counter of
    // three digits (four after 999) and the place where the recording started.
    // Older clip-YYYYMMDD-HHMMSS files keep their names and do not count. Pure,
    // unit tested.
    //
    // The place of a name: the first of town, district and state (the PLAC
    // texts) that is not empty, in ASCII CamelCase without spaces or accents
    // ("Saint Denis" -> "SaintDenis", "O'Creagh's Run" -> "OCreaghsRun"),
    // "Wild" when none has a letter or digit. At most 40 characters.
    std::string ClipPlaceTag(const char* town, const char* district, const char* state);
    // The counter of a clip name or file name ("042-Valentine",
    // "ReplayClips\\1000-Wild-p2.rpclip"): its leading digits (three or
    // more, then '-'). False for any other name (clip-..., a renamed clip).
    bool ClipNumberOf(const std::wstring& name, std::uint32_t& number);
    // The next counter: the highest one among `names` plus one (1 when none).
    std::uint32_t NextClipNumber(const std::vector<std::wstring>& names);
    // "042-Valentine" plus the suffix as given (the part's -pN).
    std::wstring NumberedClipBase(std::uint32_t number, const std::string& placeTag, const std::wstring& suffix);

    // The game build the clips are written for: the executable's
    // PE timestamp and image size. Written in the META section; a clip from
    // another build is refused, since its image offsets would point wrong.
    // 0 (the default, tests) writes zeros and checks nothing.
    void SetBuildIdentity(std::uint32_t peTimestamp, std::uint64_t imageSize);

    // Name and version of each section in file order (docs, logs, tests).
    struct SectionInfo
    {
        std::uint32_t tag = 0;
        std::uint16_t version = 0;
        const char* name = "";
        bool optional = false;  // a clip without it loads
        bool skippable = false; // a payload the reader refuses is left out (see SkippedSection)
    };
    const std::vector<SectionInfo>& Sections();

    // Sections an older build wrote for a removed feature (WGPU, the GPU
    // water). Decode steps over them unread and lists each in `skipped` with
    // the reason "retired_not_read (<why>)"; a save never writes one.
    struct RetiredSectionInfo
    {
        std::uint32_t tag = 0;
        const char* name = "";
        const char* why = "";
    };
    const std::vector<RetiredSectionInfo>& RetiredSections();
}
