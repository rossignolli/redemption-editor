#pragma once

// Compressed clips on disk. A compressed clip keeps its name (.rpclip); the
// file is one zstd frame (magic 28 B5 2F FD at offset 0) holding the clip's
// bytes exactly as clipfile::Save wrote them, followed by an optional zstd
// skippable frame with the clip list's summary (the clip header, its
// environment and place sections, and world info), so the gallery reads a
// compressed clip without decompressing it. A file of this format opens with
// the zstd command line tool too (it skips the skippable frame).
//
// A compressed clip is either that zstd frame or the packed container of
// clip_pack.h (magic "RPOCPACK", about 40% smaller, the same summary frame
// at the end). IsCompressed, ContentSize, Decompress and ReadHead take both;
// the background compressor writes the packed one and upgrades zstd files to
// it. A save writes the packed container already (zstd blocks, fast:
// PackForSave); the background compressor repacks those with LZMA.
//
// clipfile::Load decompresses such a file into memory and parses it as
// before; the background compressor (clip_compress_worker.h) writes them.
// No engine or native access: the file functions use Win32 only and are
// unit tested in ReplayCoreTests.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace replay::clipcompress
{
    // The zstd frame magic 0xFD2FB528, little endian, at offset 0.
    constexpr std::uint8_t kZstdMagic[4] = {0x28, 0xB5, 0x2F, 0xFD};
    // 2^27 = 128 MB window (measured on real clips: level 19 and window 27
    // gave 2.8x). The decompressor accepts this window and no larger one.
    constexpr int kWindowLog = 27;
    constexpr int kDefaultLevel = 19;
    constexpr std::uint32_t kDefaultDays = 1;
    // Clips larger than this are not compressed (the compressor holds the
    // clip and its compressed copy in memory) and a compressed frame that
    // claims more is refused.
    constexpr std::uint64_t kMaxClipBytes = 1ull << 30;

    // A zstd frame or a packed container.
    bool IsCompressed(const std::uint8_t* data, std::size_t size);
    bool IsZstd(const std::uint8_t* data, std::size_t size);
    bool IsPacked(const std::uint8_t* data, std::size_t size);
    // A plain clip: starts with clipfile::kMagic.
    bool IsPlainClip(const std::uint8_t* data, std::size_t size);
    // The clip's size written in the frame or container header (0 when
    // unknown).
    std::uint64_t ContentSize(const std::uint8_t* data, std::size_t size);

    // One zstd frame of `data` (level, window 27, content size and checksum
    // in the frame), compressed in chunks: `cancel` (may be null) is checked
    // between chunks and a cancelled call returns false with error
    // "cancelled".
    bool Compress(const std::uint8_t* data, std::size_t size, int level, const std::atomic<bool>* cancel,
        std::vector<std::uint8_t>& out, std::string& error);
    // The first frame of a compressed file (skippable frames after it are
    // ignored), or a packed container (decompressed on up to `threads`
    // threads, 0: the processors). On failure `out` is empty and `error`
    // says why.
    bool Decompress(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out, std::string& error);
    // The same; `file` is released as soon as it is not needed (a packed
    // file once its blocks are decompressed), which lowers the peak.
    bool DecompressConsume(std::vector<std::uint8_t>&& file, std::vector<std::uint8_t>& out, std::string& error,
        unsigned threads = 0);
    // Decompresses `compressed` in chunks and compares with `original`.
    bool Verify(const std::uint8_t* compressed, std::size_t compressedSize, const std::uint8_t* original,
        std::size_t originalSize, const std::atomic<bool>* cancel, std::string& error);

    // The clip list's summary: the 20 byte clip header, then the
    // environment and place sections as they are in the file (section
    // header, payload, hash). Only the header when the format is not this
    // build's. False when `clip` is not a plain clip.
    bool Summary(const std::uint8_t* clip, std::size_t size, std::vector<std::uint8_t>& summary);
    // The skippable frame that carries a summary, appended to `out`.
    void AppendSummaryFrame(const std::vector<std::uint8_t>& summary, std::vector<std::uint8_t>& out);

    // What the clip list needs from a clip file, read cheaply: the summary
    // (a plain file by seeking over the section payloads; a compressed one
    // from its summary frame, else by decompressing only up to the place
    // section), the size on disk and the clip's own size.
    struct FileHead
    {
        bool compressed = false; // zstd or packed
        bool packed = false;
        std::uint64_t diskBytes = 0;
        std::uint64_t clipBytes = 0; // uncompressed (0 when unknown)
        std::vector<std::uint8_t> summary;
    };
    bool ReadHead(const std::wstring& path, FileHead& head);

    // File times as 100 ns ticks since 1601 (FILETIME).
    struct FileTimes
    {
        std::uint64_t creation = 0;
        std::uint64_t lastAccess = 0;
        std::uint64_t lastWrite = 0;
        std::uint64_t bytes = 0;
    };
    bool GetTimes(const std::wstring& path, FileTimes& times);
    std::uint64_t NowTicks();
    // The file's last access set to now (NTFS may not keep it, so a clip
    // load writes it: the compressor's age rule reads it).
    bool StampAccess(const std::wstring& path);

    // The age rule: a clip is compressed when neither written nor opened
    // for `days` days (the later of the two times). Pure. False when days
    // is 0 or the time is in the future.
    constexpr std::uint64_t kTicksPerDay = 864000000000ull;
    bool OldEnough(std::uint64_t lastWrite, std::uint64_t lastAccess, std::uint64_t now, std::uint32_t days);

    // One clip file compressed in place: read, compress, verify, written to
    // <path>.ztmp next to it with the original times, then renamed over the
    // original (MoveFileExW, replace and write through) if the original did
    // not change meanwhile. Already compressed files and files that are not
    // plain clips are skipped. `cancel` stops it between chunks (the
    // temporary file is removed).
    struct FileResult
    {
        bool ok = false;
        bool skipped = false;
        std::string reason; // skip reason or error
        std::uint64_t bytesIn = 0;
        std::uint64_t bytesOut = 0;
        std::uint32_t ms = 0;
        bool upgraded = false;  // a zstd or fast packed file repacked (bytesIn is that file)
        bool fastPacked = false; // the file was a save's fast pack
        bool plainPack = false; // packed without the pose and rope transform
    };
    // This overload writes the zstd frame (and skips compressed files).
    FileResult CompressFile(const std::wstring& path, int level, const std::atomic<bool>* cancel);

    // The background compressor's format.
    enum class Format
    {
        Zstd = 1,  // one zstd frame (before 1.0's packed container)
        Packed = 2 // clip_pack.h, LZMA blocks
    };
    struct CompressOptions
    {
        Format format = Format::Packed;
        int level = kDefaultLevel; // zstd level; for the packed format 1 to 22 maps to LZMA 1 to 9
        unsigned threads = 1;      // packed: blocks compressed at once
        bool upgrade = true;       // packed: a zstd file, or a fast packed one (zstd blocks), is repacked too
    };
    // The LZMA level of a CompressLevel (1 fast to 22 smallest; 19 and up 9).
    int LzmaLevel(int level);
    // The packed format: a plain clip (or, with upgrade, a zstd one or a
    // save's fast pack) is packed with LZMA, the result unpacked again and compared byte for byte with the
    // clip read back from the file on disk (a zstd file is decompressed
    // again for that), and only then written over the original as above.
    // When the pose or rope transform does not give the clip back, the clip
    // is packed again without it. Skipped ("low_memory", tried again at the
    // next pass) when the memory left cannot hold the work.
    FileResult CompressFile(const std::wstring& path, const CompressOptions& options, const std::atomic<bool>* cancel);

    // The save's own pack (clipfile::Save with SaveOptions::pack): a plain
    // clip written already in the packed container, with the pose and rope
    // transform and zstd blocks at a low level on a few threads, on the
    // save's thread. The background compressor later repacks such a file
    // with LZMA (its zstd blocks mark it, clippack::FastPacked).
    // Measured on real clips (ReplayClipTool save-pack-verify, 28/10/2026):
    // the table is in clip_compress.cpp (zstd 3 on 4 threads: about 2 s for
    // a 450 MB clip, 31% of its size).
    constexpr int kSavePackLevel = 3;
    constexpr unsigned kSavePackThreads = 4;
    struct SavePackOptions
    {
        int zstdLevel = kSavePackLevel;
        unsigned threads = kSavePackThreads;
        // The memory the pack may take (0: AvailableMemoryBytes()); not
        // enough is "low_memory" and the clip is written plain.
        std::uint64_t availableBytes = 0;
        const std::atomic<bool>* cancel = nullptr;
    };
    struct SavePackResult
    {
        std::string fallback; // why the clip was not packed (empty when it was)
        std::uint64_t clipBytes = 0;
        std::uint64_t fileBytes = 0; // the packed file with its summary frame
        double packMs = 0.0;       // all of the pack: summary, hash, transform, blocks, file
        double transformMs = 0.0;  // its transform (one thread)
        double compressMs = 0.0;   // its blocks (on the threads)
        double verifyMs = 0.0;
        bool pose = false; // the pose section went through the transform
        bool rope = false;
    };
    // The memory a save's pack takes besides the clip's own bytes, about
    // (the streams while the clip is split, then the packed file, the
    // streams again and the clip unpacked for the check).
    std::uint64_t SavePackNeedBytes(std::uint64_t clipBytes, unsigned threads);
    // `clip` (a plain clip) packed into `out` (with the clip list's summary
    // frame), unpacked again and compared with the clip by its hash. The
    // clip's buffer is released as soon as it is split, so the peak stays
    // near the streams, not the clip and its streams. False with
    // result.fallback when it was not packed: `clip` is then still the clip
    // when it is not empty (not a clip, low memory) and gone otherwise (the
    // caller encodes it again to write it plain). Never throws.
    bool PackForSave(std::vector<std::uint8_t>& clip, const SavePackOptions& options, std::vector<std::uint8_t>& out,
        SavePackResult& result);

    // A packed file whose blocks are zstd (a save's fast pack; reads the
    // header only).
    bool IsFastPackedFile(const std::wstring& path);

    // What a file holds, from its first bytes (cheap; keeps the last
    // access time).
    enum class Kind
    {
        Unreadable,
        Plain,
        Compressed, // a zstd frame
        Other,
        Packed // the packed container
    };
    Kind Peek(const std::wstring& path);

    // The memory a load may still take: the smaller of the commit the system
    // has left and this process's free address space (GlobalMemoryStatusEx);
    // 0 when unknown.
    std::uint64_t AvailableMemoryBytes();

    // Reads a whole file without changing its last access time.
    bool ReadWholeFile(const std::wstring& path, std::vector<std::uint8_t>& bytes, const std::atomic<bool>* cancel,
        std::string& error);
}
