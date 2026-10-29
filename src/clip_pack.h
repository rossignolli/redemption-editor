#pragma once

// The packed clip container ("RPOCPACK"): a lossless way to keep a clip on
// disk smaller than the zstd frame of clip_compress.h. The clip's bytes come
// back exactly as clipfile::Save wrote them (RPOCCLIP, same format version),
// so the clip parser does not change.
//
// How: the pose section (POSE, most of a clip) and the rope section (ROPE)
// are split into streams of one field each, every value stored as the
// difference from the previous value of the same channel, the field streams
// in channel order (track, channel, time) so the animation loops repeat
// close together. The record times and offsets of each pose chunk are
// rebuilt from the records (stored only when they differ). Everything else
// of the clip goes into the "rest" stream unchanged. Each stream is cut into
// blocks of up to 32 MB and each block is compressed alone (LZMA by default,
// zstd or stored per block), so the blocks decode on several threads.
// A pose or rope section of a version or with mask bits this code does not
// know stays in the rest stream as it is.
//
// File layout (little endian):
//   char magic[8] "RPOCPACK", u32 version (1), u32 block count,
//   u64 clip bytes, u32 cut count, u32 0,
//   cuts: { u32 section tag, u32 0, u64 payload offset in the clip },
//   blocks: { u32 stream, u8 codec, u8 shuffle width, u8 props[5], u8 0,
//             u64 raw bytes, u64 packed bytes, u64 hash of the packed
//             bytes, u32 0 },
//   u64 hash of everything above, then the blocks' bytes in table order.
// Anything after the blocks must be zstd skippable frames (the clip list's
// summary frame of clip_compress.h, kept at the file's end).
//
// Pure: no engine, native or Win32 access (std::thread for the parallel
// parts); unit tested in ReplayCoreTests and checked on real clips with
// ReplayClipTool pack-verify.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace replay::clippack
{
    constexpr std::uint8_t kMagic[8] = {'R', 'P', 'O', 'C', 'P', 'A', 'C', 'K'};
    constexpr std::uint32_t kVersion = 1;
    constexpr std::size_t kBlockBytes = 32u << 20;
    // A block larger than this is refused on read.
    constexpr std::size_t kMaxBlockBytes = 64u << 20;

    // What the transform knows of the clip format (clip_file.cpp). A clip of
    // another format version, or a POSE or ROPE section of another version,
    // is packed without the transform (still lossless, larger). The core
    // tests check these against clipfile.
    constexpr char kClipMagic[8] = {'R', 'P', 'O', 'C', 'C', 'L', 'I', 'P'};
    constexpr std::uint32_t kClipFormatVersion = 4;
    constexpr std::uint16_t kPoseSectionVersion = 1;
    constexpr std::uint16_t kRopeSectionVersion = 1;

    enum class Codec : std::uint8_t
    {
        Stored = 0,
        Lzma = 1,
        Zstd = 2
    };

    // The streams. The ids are part of the file format.
    enum Stream : std::uint32_t
    {
        kRest = 0,
        kPoseMeta,
        kPoseTime,
        kPoseFlags,
        kPoseCount,
        kPoseWorld,
        kPoseChannelMask,
        kPoseRot32Big,
        kPoseRot32Fields,
        kPoseRot64Big,
        kPoseRot64Fields,
        kPoseTrans16,
        kPoseTransFloat,
        kPoseTransW,
        kPoseScale,
        kRopeMeta,
        kRopeTimes,
        kRopePositions,
        kStreamCount
    };
    const char* StreamName(std::uint32_t stream);
    using StreamSet = std::vector<std::vector<std::uint8_t>>; // kStreamCount entries, by Stream

    struct PackOptions
    {
        Codec codec = Codec::Lzma;
        int lzmaLevel = 9;  // 1 to 9
        int zstdLevel = 19; // for Codec::Zstd
        unsigned threads = 1; // blocks compressed at once (each LZMA block takes about 11x its size)
        std::size_t blockBytes = kBlockBytes;
        bool transform = true; // false: the whole clip in the rest stream
        const std::atomic<bool>* cancel = nullptr; // checked between and inside blocks
    };

    struct StreamStat
    {
        std::uint32_t stream = 0;
        std::uint32_t blocks = 0;
        std::uint64_t rawBytes = 0;
        std::uint64_t packedBytes = 0;
    };

    struct PackStats
    {
        bool pose = false; // the POSE section went through the transform
        bool rope = false;
        std::string poseSkip; // why not (empty when it did or there was none)
        std::string ropeSkip;
        std::uint64_t clipBytes = 0;
        std::uint64_t packedBytes = 0;
        double transformMs = 0.0;
        double compressMs = 0.0;
        std::vector<StreamStat> streams;
    };

    struct UnpackOptions
    {
        unsigned threads = 0; // 0: the processors, at most 12
        std::uint64_t maxClipBytes = 1ull << 30;
        const std::atomic<bool>* cancel = nullptr;
    };

    struct UnpackStats
    {
        double blocksMs = 0.0;  // read and decompress the blocks
        double rebuildMs = 0.0; // the clip rebuilt from the streams
        std::uint64_t streamBytes = 0;
        std::uint32_t blocks = 0;
        unsigned threads = 0;
    };

    // Starts with kMagic.
    bool IsPacked(const std::uint8_t* data, std::size_t size);

    // The block table read from the header alone (the file's first
    // HeaderBytes): how many blocks of each codec. The codec byte of each
    // block is what tells a save's fast pack (zstd blocks, clip_compress.h
    // PackForSave) from the background compressor's LZMA one: a file with a
    // zstd block is still worth repacking, one without is not.
    struct HeaderInfo
    {
        std::uint64_t clipBytes = 0;
        std::uint32_t blocks = 0;
        std::uint32_t stored = 0;
        std::uint32_t lzma = 0;
        std::uint32_t zstd = 0;
    };
    // The header's size from its first 32 bytes (0 when they are not a
    // packed header of this version).
    std::size_t HeaderBytes(const std::uint8_t* data, std::size_t size);
    // False when the header is not whole in `size`, damaged (its hash) or of
    // another version.
    bool ReadHeaderInfo(const std::uint8_t* data, std::size_t size, HeaderInfo& info);
    // True when some block is zstd (a fast pack).
    inline bool FastPacked(const HeaderInfo& info) { return info.zstd > 0; }

    // The 64-bit hash the container uses for its blocks (not a
    // cryptographic one), for a check of a clip against its unpacked copy
    // without keeping both.
    std::uint64_t HashBytes(const std::uint8_t* data, std::size_t size);
    // The clip's size from the header (0 when the header is not a valid one).
    std::uint64_t ContentSize(const std::uint8_t* data, std::size_t size);

    // `clip` must be a plain clip (kClipMagic). On failure `out` is empty and
    // `error` says why ("cancelled" when cancelled).
    bool Pack(const std::uint8_t* clip, std::size_t size, const PackOptions& options, std::vector<std::uint8_t>& out,
        std::string& error, PackStats* stats = nullptr);
    // The same, the clip's buffer released (cleared) as soon as it is split
    // into streams: the peak is the streams, not the clip and its streams.
    bool PackConsume(std::vector<std::uint8_t>&& clip, const PackOptions& options, std::vector<std::uint8_t>& out,
        std::string& error, PackStats* stats = nullptr);

    // Any damage (a truncated file, a wrong size, a hash that does not
    // match, a stream that does not rebuild) is an error, never a crash.
    bool Unpack(const std::uint8_t* data, std::size_t size, const UnpackOptions& options, std::vector<std::uint8_t>& out,
        std::string& error, UnpackStats* stats = nullptr);
    // The same, the file's buffer released once its blocks are decompressed.
    bool UnpackConsume(std::vector<std::uint8_t>&& file, const UnpackOptions& options, std::vector<std::uint8_t>& out,
        std::string& error, UnpackStats* stats = nullptr);

    // The memory an unpack takes besides the file, about: the streams and
    // the clip (each about the clip's size).
    constexpr std::uint64_t kUnpackFactor = 2;

    // The transforms alone (tests). Split fills its streams of `streams`
    // (resized to kStreamCount); false (with `why`) when the payload is not
    // one the transform knows. Join rebuilds the payload.
    bool SplitPose(const std::uint8_t* payload, std::size_t size, StreamSet& streams, std::string& why);
    // The same on up to `threads` threads (groups of tracks, the same
    // streams as one thread).
    bool SplitPose(const std::uint8_t* payload, std::size_t size, unsigned threads, StreamSet& streams, std::string& why);
    bool JoinPose(const StreamSet& streams, unsigned threads, std::vector<std::uint8_t>& payload, std::string& error);
    bool SplitRope(const std::uint8_t* payload, std::size_t size, StreamSet& streams, std::string& why);
    bool JoinRope(const StreamSet& streams, std::vector<std::uint8_t>& payload, std::string& error);
}
