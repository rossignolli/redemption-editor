#include "clip_compress.h"

#include "clip_file.h"
#include "clip_pack.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>

#include "../external/zstd/zstd.h"

namespace replay::clipcompress
{
    namespace
    {
        constexpr std::size_t kHeaderBytes = 20; // magic, format version, duration
        constexpr std::size_t kSectionHeaderBytes = 14; // tag, version, length
        constexpr std::size_t kSectionHashBytes = 8;
        constexpr std::size_t kMaxListPayload = 4096;
        constexpr std::size_t kChunkBytes = 256 * 1024; // between two cancel checks
        // The summary frame: a zstd skippable frame (magic 0x184D2A50 to
        // 0x184D2A5F, then its u32 size), payload "RPOCZSUM", the summary,
        // then the frame's whole size again as the file's last 4 bytes, so
        // the list finds it from the end.
        constexpr std::uint32_t kSkippableMagic = 0x184D2A5Eu;
        constexpr char kSummaryTag[8] = {'R', 'P', 'O', 'C', 'Z', 'S', 'U', 'M'};
        constexpr std::uint32_t kMaxSummaryFrame = 64 * 1024;

        bool Cancelled(const std::atomic<bool>* cancel)
        {
            return cancel && cancel->load(std::memory_order_acquire);
        }

        std::string ZstdError(const char* what, std::size_t code)
        {
            return std::string(what) + ": " + ZSTD_getErrorName(code);
        }

        struct CCtxFree
        {
            void operator()(ZSTD_CCtx* context) const { ZSTD_freeCCtx(context); }
        };
        struct DCtxFree
        {
            void operator()(ZSTD_DCtx* context) const { ZSTD_freeDCtx(context); }
        };
        using CCtx = std::unique_ptr<ZSTD_CCtx, CCtxFree>;
        using DCtx = std::unique_ptr<ZSTD_DCtx, DCtxFree>;

        DCtx NewDCtx()
        {
            DCtx context(ZSTD_createDCtx());
            if (context)
                ZSTD_DCtx_setParameter(context.get(), ZSTD_d_windowLogMax, kWindowLog);
            return context;
        }

        std::uint64_t Ticks(const FILETIME& time)
        {
            return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        }

        FILETIME ToFileTime(std::uint64_t ticks)
        {
            FILETIME time{};
            time.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFu);
            time.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
            return time;
        }

        // A file opened for reading that keeps its last access time: the
        // clip list and the compressor read every clip, which must not make
        // them look recently used (SetFileTime with 0xFFFFFFFF, below).
        class ReadHandle
        {
        public:
            explicit ReadHandle(const std::wstring& path)
            {
                m_handle = CreateFileW(path.c_str(), GENERIC_READ | FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                if (m_handle != INVALID_HANDLE_VALUE)
                {
                    FILETIME keep{0xFFFFFFFFu, 0xFFFFFFFFu};
                    SetFileTime(m_handle, nullptr, &keep, nullptr);
                }
                else
                    m_handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            }
            ~ReadHandle()
            {
                if (m_handle != INVALID_HANDLE_VALUE)
                    CloseHandle(m_handle);
            }
            ReadHandle(const ReadHandle&) = delete;
            ReadHandle& operator=(const ReadHandle&) = delete;
            bool Valid() const { return m_handle != INVALID_HANDLE_VALUE; }
            HANDLE Get() const { return m_handle; }
            std::uint64_t Size() const
            {
                LARGE_INTEGER size{};
                return GetFileSizeEx(m_handle, &size) ? static_cast<std::uint64_t>(size.QuadPart) : 0;
            }
            bool ReadAt(std::uint64_t offset, void* out, std::size_t bytes) { return Seek(offset) && Read(out, bytes); }
            bool Read(void* out, std::size_t bytes)
            {
                auto* cursor = static_cast<std::uint8_t*>(out);
                while (bytes > 0)
                {
                    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(bytes, 1u << 24));
                    DWORD read = 0;
                    if (!ReadFile(m_handle, cursor, want, &read, nullptr) || read == 0)
                        return false;
                    cursor += read;
                    bytes -= read;
                }
                return true;
            }
            bool Seek(std::uint64_t offset)
            {
                LARGE_INTEGER at{};
                at.QuadPart = static_cast<LONGLONG>(offset);
                return SetFilePointerEx(m_handle, at, nullptr, FILE_BEGIN) != FALSE;
            }
            bool Skip(std::uint64_t bytes)
            {
                LARGE_INTEGER by{};
                by.QuadPart = static_cast<LONGLONG>(bytes);
                return SetFilePointerEx(m_handle, by, nullptr, FILE_CURRENT) != FALSE;
            }

        private:
            HANDLE m_handle = INVALID_HANDLE_VALUE;
        };

        // A stream of clip bytes for the summary walk.
        class Source
        {
        public:
            virtual ~Source() = default;
            virtual bool Read(void* out, std::size_t bytes) = 0;
            virtual bool Skip(std::uint64_t bytes) = 0;
        };

        class MemorySource final : public Source
        {
        public:
            MemorySource(const std::uint8_t* data, std::size_t size) : m_data(data), m_size(size) {}
            bool Read(void* out, std::size_t bytes) override
            {
                if (bytes > m_size - m_at)
                    return false;
                std::memcpy(out, m_data + m_at, bytes);
                m_at += bytes;
                return true;
            }
            bool Skip(std::uint64_t bytes) override
            {
                if (bytes > m_size - m_at)
                    return false;
                m_at += static_cast<std::size_t>(bytes);
                return true;
            }

        private:
            const std::uint8_t* m_data;
            std::size_t m_size;
            std::size_t m_at = 0;
        };

        class FileSource final : public Source
        {
        public:
            explicit FileSource(ReadHandle& file) : m_file(file) {}
            bool Read(void* out, std::size_t bytes) override { return m_file.Read(out, bytes); }
            bool Skip(std::uint64_t bytes) override { return m_file.Skip(bytes); }

        private:
            ReadHandle& m_file;
        };

        // A compressed file decompressed as it is read; Skip decompresses
        // and drops. Used for the list only when a file has no summary frame.
        class ZstdFileSource final : public Source
        {
        public:
            explicit ZstdFileSource(ReadHandle& file) : m_file(file), m_context(NewDCtx())
            {
                m_input.resize(ZSTD_DStreamInSize());
                m_output.resize(ZSTD_DStreamOutSize());
            }
            bool Read(void* out, std::size_t bytes) override
            {
                auto* cursor = static_cast<std::uint8_t*>(out);
                while (bytes > 0)
                {
                    if (m_outAt == m_outSize && !Fill())
                        return false;
                    const std::size_t take = std::min(bytes, m_outSize - m_outAt);
                    std::memcpy(cursor, m_output.data() + m_outAt, take);
                    m_outAt += take;
                    cursor += take;
                    bytes -= take;
                }
                return true;
            }
            bool Skip(std::uint64_t bytes) override
            {
                while (bytes > 0)
                {
                    if (m_outAt == m_outSize && !Fill())
                        return false;
                    const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(bytes, m_outSize - m_outAt));
                    m_outAt += take;
                    bytes -= take;
                }
                return true;
            }

        private:
            bool Fill()
            {
                if (!m_context || m_ended)
                    return false;
                m_outAt = m_outSize = 0;
                while (m_outSize == 0)
                {
                    if (m_in.pos == m_in.size)
                    {
                        DWORD read = 0;
                        if (!ReadFile(m_file.Get(), m_input.data(), static_cast<DWORD>(m_input.size()), &read, nullptr) ||
                            read == 0)
                            return false;
                        m_in = {m_input.data(), read, 0};
                    }
                    ZSTD_outBuffer out{m_output.data(), m_output.size(), 0};
                    const std::size_t result = ZSTD_decompressStream(m_context.get(), &out, &m_in);
                    if (ZSTD_isError(result))
                        return false;
                    m_outSize = out.pos;
                    m_total += out.pos;
                    if (m_total > kMaxClipBytes)
                        return false;
                    if (result == 0)
                    {
                        m_ended = true; // the clip's frame ended
                        return m_outSize > 0;
                    }
                }
                return true;
            }

            ReadHandle& m_file;
            DCtx m_context;
            std::vector<std::uint8_t> m_input;
            std::vector<std::uint8_t> m_output;
            ZSTD_inBuffer m_in{nullptr, 0, 0};
            std::size_t m_outAt = 0;
            std::size_t m_outSize = 0;
            std::uint64_t m_total = 0;
            bool m_ended = false;
        };

        // The sections the list reads, and where they stand in file order:
        // the walk stops at the first section after the last of them (the
        // pose section, most of a clip, is never read).
        struct ListSections
        {
            std::uint32_t environment = 0;
            std::uint32_t place = 0;
            std::uint32_t worldInfo = 0; // the chapter for the gallery
            std::size_t lastIndex = 0;
        };

        const ListSections& Wanted()
        {
            static const ListSections wanted = [] {
                ListSections out;
                const std::vector<clipfile::SectionInfo>& sections = clipfile::Sections();
                for (std::size_t n = 0; n < sections.size(); ++n)
                {
                    if (std::strcmp(sections[n].name, "environment") == 0)
                    {
                        out.environment = sections[n].tag;
                        out.lastIndex = std::max(out.lastIndex, n);
                    }
                    if (std::strcmp(sections[n].name, "place") == 0)
                    {
                        out.place = sections[n].tag;
                        out.lastIndex = std::max(out.lastIndex, n);
                    }
                    if (std::strcmp(sections[n].name, "world info") == 0)
                    {
                        out.worldInfo = sections[n].tag;
                        out.lastIndex = std::max(out.lastIndex, n);
                    }
                }
                return out;
            }();
            return wanted;
        }

        std::size_t SectionIndex(std::uint32_t tag)
        {
            const std::vector<clipfile::SectionInfo>& sections = clipfile::Sections();
            for (std::size_t n = 0; n < sections.size(); ++n)
            {
                if (sections[n].tag == tag)
                    return n;
            }
            return sections.size(); // unknown: treated as past the list's sections
        }

        bool WalkSummary(Source& source, std::vector<std::uint8_t>& summary)
        {
            summary.clear();
            std::uint8_t header[kHeaderBytes]{};
            if (!source.Read(header, sizeof(header)) || std::memcmp(header, clipfile::kMagic, sizeof(clipfile::kMagic)) != 0)
                return false;
            summary.assign(header, header + sizeof(header));
            std::uint32_t version = 0;
            std::memcpy(&version, header + 8, 4);
            if (version != clipfile::kFormatVersion)
                return true;
            const ListSections& wanted = Wanted();
            bool haveEnvironment = false, havePlace = false, haveWorldInfo = false;
            for (int guard = 0; guard < 64 && !(haveEnvironment && havePlace && haveWorldInfo); ++guard)
            {
                std::uint8_t section[kSectionHeaderBytes]{};
                if (!source.Read(section, sizeof(section)))
                    break;
                std::uint32_t tag = 0;
                std::uint64_t length = 0;
                std::memcpy(&tag, section, 4);
                std::memcpy(&length, section + 6, 8);
                if ((tag == wanted.environment || tag == wanted.place || tag == wanted.worldInfo) && length <= kMaxListPayload)
                {
                    std::uint8_t payload[kMaxListPayload + kSectionHashBytes];
                    const std::size_t bytes = static_cast<std::size_t>(length) + kSectionHashBytes;
                    if (!source.Read(payload, bytes))
                        break;
                    summary.insert(summary.end(), section, section + sizeof(section));
                    summary.insert(summary.end(), payload, payload + bytes);
                    (tag == wanted.environment ? haveEnvironment : tag == wanted.place ? havePlace : haveWorldInfo) = true;
                    continue;
                }
                if (SectionIndex(tag) > wanted.lastIndex || length > kMaxClipBytes)
                    break;
                if (!source.Skip(length + kSectionHashBytes))
                    break;
            }
            return true;
        }

        // The summary frame at the end of a compressed file, if there is one.
        bool ReadSummaryFrame(ReadHandle& file, std::uint64_t fileSize, std::vector<std::uint8_t>& summary)
        {
            if (fileSize < 4 + 8 + sizeof(kSummaryTag) + 4)
                return false;
            std::uint32_t frameBytes = 0;
            if (!file.ReadAt(fileSize - 4, &frameBytes, 4) || frameBytes < 8 + sizeof(kSummaryTag) + 4 ||
                frameBytes > kMaxSummaryFrame || frameBytes > fileSize - 4)
                return false;
            std::vector<std::uint8_t> frame(frameBytes);
            if (!file.ReadAt(fileSize - frameBytes, frame.data(), frame.size()))
                return false;
            std::uint32_t magic = 0, size = 0;
            std::memcpy(&magic, frame.data(), 4);
            std::memcpy(&size, frame.data() + 4, 4);
            if (magic != kSkippableMagic || size != frameBytes - 8 ||
                std::memcmp(frame.data() + 8, kSummaryTag, sizeof(kSummaryTag)) != 0)
                return false;
            summary.assign(frame.begin() + 8 + sizeof(kSummaryTag), frame.end() - 4);
            return summary.size() >= kHeaderBytes && std::memcmp(summary.data(), clipfile::kMagic, sizeof(clipfile::kMagic)) == 0;
        }

        bool WriteAll(HANDLE file, const std::uint8_t* data, std::size_t size, const std::atomic<bool>* cancel)
        {
            while (size > 0)
            {
                if (Cancelled(cancel))
                    return false;
                const DWORD want = static_cast<DWORD>(std::min<std::size_t>(size, 4u << 20));
                DWORD written = 0;
                if (!WriteFile(file, data, want, &written, nullptr) || written != want)
                    return false;
                data += want;
                size -= want;
            }
            return true;
        }
    }

    bool IsZstd(const std::uint8_t* data, std::size_t size)
    {
        return data && size >= sizeof(kZstdMagic) && std::memcmp(data, kZstdMagic, sizeof(kZstdMagic)) == 0;
    }

    bool IsPacked(const std::uint8_t* data, std::size_t size) { return clippack::IsPacked(data, size); }

    bool IsCompressed(const std::uint8_t* data, std::size_t size) { return IsZstd(data, size) || IsPacked(data, size); }

    int LzmaLevel(int level) { return std::clamp((std::clamp(level, 1, 22) + 1) / 2, 1, 9); }

    bool IsPlainClip(const std::uint8_t* data, std::size_t size)
    {
        return data && size >= sizeof(clipfile::kMagic) && std::memcmp(data, clipfile::kMagic, sizeof(clipfile::kMagic)) == 0;
    }

    std::uint64_t ContentSize(const std::uint8_t* data, std::size_t size)
    {
        if (IsPacked(data, size))
            return clippack::ContentSize(data, size);
        if (!IsZstd(data, size))
            return 0;
        const unsigned long long content = ZSTD_getFrameContentSize(data, size);
        return content == ZSTD_CONTENTSIZE_UNKNOWN || content == ZSTD_CONTENTSIZE_ERROR ? 0 : content;
    }

    bool Compress(const std::uint8_t* data, std::size_t size, int level, const std::atomic<bool>* cancel,
        std::vector<std::uint8_t>& out, std::string& error)
    {
        out.clear();
        CCtx context(ZSTD_createCCtx());
        if (!context)
        {
            error = "zstd context";
            return false;
        }
        level = std::clamp(level, 1, ZSTD_maxCLevel());
        std::size_t result = ZSTD_CCtx_setParameter(context.get(), ZSTD_c_compressionLevel, level);
        if (!ZSTD_isError(result))
            result = ZSTD_CCtx_setParameter(context.get(), ZSTD_c_windowLog, kWindowLog);
        if (!ZSTD_isError(result))
            result = ZSTD_CCtx_setParameter(context.get(), ZSTD_c_checksumFlag, 1);
        if (!ZSTD_isError(result))
            result = ZSTD_CCtx_setPledgedSrcSize(context.get(), size);
        if (ZSTD_isError(result))
        {
            error = ZstdError("zstd parameters", result);
            return false;
        }
        out.reserve(size / 2 + 4096);
        std::vector<std::uint8_t> chunk(ZSTD_CStreamOutSize());
        std::size_t at = 0;
        for (;;)
        {
            if (Cancelled(cancel))
            {
                out.clear();
                error = "cancelled";
                return false;
            }
            const std::size_t take = std::min(kChunkBytes, size - at);
            const bool last = at + take == size;
            ZSTD_inBuffer in{data + at, take, 0};
            const ZSTD_EndDirective mode = last ? ZSTD_e_end : ZSTD_e_continue;
            bool finished = false;
            do
            {
                ZSTD_outBuffer output{chunk.data(), chunk.size(), 0};
                result = ZSTD_compressStream2(context.get(), &output, &in, mode);
                if (ZSTD_isError(result))
                {
                    out.clear();
                    error = ZstdError("zstd compress", result);
                    return false;
                }
                out.insert(out.end(), chunk.data(), chunk.data() + output.pos);
                finished = last ? result == 0 : in.pos == in.size;
            } while (!finished);
            at += take;
            if (last)
                break;
        }
        error.clear();
        return true;
    }

    bool Decompress(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out, std::string& error)
    {
        out.clear();
        if (IsPacked(data, size))
        {
            clippack::UnpackOptions options;
            options.maxClipBytes = kMaxClipBytes;
            return clippack::Unpack(data, size, options, out, error);
        }
        if (!IsZstd(data, size))
        {
            error = "compressed clip: not a zstd frame";
            return false;
        }
        const unsigned long long content = ZSTD_getFrameContentSize(data, size);
        if (content == ZSTD_CONTENTSIZE_ERROR)
        {
            error = "compressed clip: bad frame header";
            return false;
        }
        const std::size_t frameBytes = ZSTD_findFrameCompressedSize(data, size);
        if (ZSTD_isError(frameBytes))
        {
            error = ZstdError("compressed clip: truncated or corrupt", frameBytes);
            return false;
        }
        // After the clip's frame only skippable frames (the summary).
        for (std::size_t at = frameBytes; at < size;)
        {
            std::uint32_t magic = 0, length = 0;
            if (size - at < 8)
            {
                error = "compressed clip: trailing data";
                return false;
            }
            std::memcpy(&magic, data + at, 4);
            std::memcpy(&length, data + at + 4, 4);
            if ((magic & 0xFFFFFFF0u) != 0x184D2A50u || length > size - at - 8)
            {
                error = "compressed clip: trailing data";
                return false;
            }
            at += 8 + static_cast<std::size_t>(length);
        }
        DCtx context = NewDCtx();
        if (!context)
        {
            error = "zstd context";
            return false;
        }
        if (content != ZSTD_CONTENTSIZE_UNKNOWN)
        {
            if (content > kMaxClipBytes)
            {
                error = "compressed clip: too large";
                return false;
            }
            out.resize(static_cast<std::size_t>(content));
            const std::size_t result = ZSTD_decompressDCtx(context.get(), out.data(), out.size(), data, frameBytes);
            if (ZSTD_isError(result) || result != out.size())
            {
                out.clear();
                error = ZSTD_isError(result) ? ZstdError("compressed clip", result) : "compressed clip: size mismatch";
                return false;
            }
            error.clear();
            return true;
        }
        // No size in the frame (written by another tool): streamed.
        std::vector<std::uint8_t> chunk(ZSTD_DStreamOutSize());
        ZSTD_inBuffer in{data, frameBytes, 0};
        std::size_t result = 1;
        while (result != 0)
        {
            ZSTD_outBuffer output{chunk.data(), chunk.size(), 0};
            result = ZSTD_decompressStream(context.get(), &output, &in);
            if (ZSTD_isError(result) || (output.pos == 0 && in.pos == in.size && result != 0) ||
                out.size() + output.pos > kMaxClipBytes)
            {
                out.clear();
                error = ZSTD_isError(result) ? ZstdError("compressed clip", result) : "compressed clip: truncated or too large";
                return false;
            }
            out.insert(out.end(), chunk.data(), chunk.data() + output.pos);
        }
        error.clear();
        return true;
    }

    bool DecompressConsume(std::vector<std::uint8_t>&& file, std::vector<std::uint8_t>& out, std::string& error,
        unsigned threads)
    {
        out.clear();
        if (IsPacked(file.data(), file.size()))
        {
            clippack::UnpackOptions options;
            options.threads = threads;
            options.maxClipBytes = kMaxClipBytes;
            return clippack::UnpackConsume(std::move(file), options, out, error);
        }
        const bool ok = Decompress(file.data(), file.size(), out, error);
        file.clear();
        file.shrink_to_fit();
        return ok;
    }

    bool Verify(const std::uint8_t* compressed, std::size_t compressedSize, const std::uint8_t* original,
        std::size_t originalSize, const std::atomic<bool>* cancel, std::string& error)
    {
        if (IsPacked(compressed, compressedSize))
        {
            clippack::UnpackOptions options;
            options.maxClipBytes = kMaxClipBytes;
            options.cancel = cancel;
            std::vector<std::uint8_t> back;
            if (!clippack::Unpack(compressed, compressedSize, options, back, error))
                return false;
            if (back.size() != originalSize || std::memcmp(back.data(), original, originalSize) != 0)
            {
                error = "verify: bytes differ";
                return false;
            }
            return true;
        }
        DCtx context = NewDCtx();
        if (!context)
        {
            error = "zstd context";
            return false;
        }
        std::vector<std::uint8_t> chunk(ZSTD_DStreamOutSize());
        ZSTD_inBuffer in{compressed, compressedSize, 0};
        std::size_t at = 0;
        std::size_t result = 1;
        while (result != 0)
        {
            if (Cancelled(cancel))
            {
                error = "cancelled";
                return false;
            }
            ZSTD_outBuffer output{chunk.data(), chunk.size(), 0};
            result = ZSTD_decompressStream(context.get(), &output, &in);
            if (ZSTD_isError(result))
            {
                error = ZstdError("verify", result);
                return false;
            }
            if (output.pos > originalSize - at || std::memcmp(chunk.data(), original + at, output.pos) != 0)
            {
                error = "verify: bytes differ";
                return false;
            }
            at += output.pos;
            if (output.pos == 0 && in.pos == in.size && result != 0)
            {
                error = "verify: truncated";
                return false;
            }
        }
        if (at != originalSize)
        {
            error = "verify: size differs";
            return false;
        }
        return true;
    }

    bool Summary(const std::uint8_t* clip, std::size_t size, std::vector<std::uint8_t>& summary)
    {
        MemorySource source(clip, size);
        return WalkSummary(source, summary);
    }

    void AppendSummaryFrame(const std::vector<std::uint8_t>& summary, std::vector<std::uint8_t>& out)
    {
        const std::uint32_t payload = static_cast<std::uint32_t>(sizeof(kSummaryTag) + summary.size() + 4);
        const std::uint32_t frameBytes = 8 + payload;
        const std::size_t start = out.size();
        out.resize(start + frameBytes);
        std::uint8_t* at = out.data() + start;
        std::memcpy(at, &kSkippableMagic, 4);
        std::memcpy(at + 4, &payload, 4);
        std::memcpy(at + 8, kSummaryTag, sizeof(kSummaryTag));
        if (!summary.empty())
            std::memcpy(at + 8 + sizeof(kSummaryTag), summary.data(), summary.size());
        std::memcpy(at + frameBytes - 4, &frameBytes, 4);
    }

    bool ReadHead(const std::wstring& path, FileHead& head)
    {
        head = FileHead{};
        ReadHandle file(path);
        if (!file.Valid())
            return false;
        head.diskBytes = file.Size();
        std::uint8_t first[32]{}; // ZSTD_FRAMEHEADERSIZE_MAX (18), the packed header (32)
        const std::size_t firstBytes = static_cast<std::size_t>(std::min<std::uint64_t>(sizeof(first), head.diskBytes));
        if (!file.ReadAt(0, first, firstBytes))
            return false;
        head.compressed = IsCompressed(first, firstBytes);
        head.packed = IsPacked(first, firstBytes);
        if (!head.compressed)
        {
            head.clipBytes = head.diskBytes;
            if (!file.Seek(0))
                return false;
            FileSource source(file);
            return WalkSummary(source, head.summary);
        }
        head.clipBytes = ContentSize(first, firstBytes);
        if (ReadSummaryFrame(file, head.diskBytes, head.summary))
            return true;
        if (head.packed)
        {
            // The compressor always writes the summary frame; without it the
            // whole container is unpacked (its rest stream is not in file
            // order).
            if (head.diskBytes > kMaxClipBytes)
                return false;
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(head.diskBytes));
            if (!file.ReadAt(0, bytes.data(), bytes.size()))
                return false;
            std::vector<std::uint8_t> clip;
            std::string error;
            if (!DecompressConsume(std::move(bytes), clip, error))
                return false;
            return Summary(clip.data(), clip.size(), head.summary);
        }
        if (!file.Seek(0))
            return false;
        ZstdFileSource source(file);
        return WalkSummary(source, head.summary);
    }

    bool GetTimes(const std::wstring& path, FileTimes& times)
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            return false;
        times.creation = Ticks(data.ftCreationTime);
        times.lastAccess = Ticks(data.ftLastAccessTime);
        times.lastWrite = Ticks(data.ftLastWriteTime);
        times.bytes = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        return true;
    }

    std::uint64_t NowTicks()
    {
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        return Ticks(now);
    }

    bool StampAccess(const std::wstring& path)
    {
        HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        const FILETIME now = ToFileTime(NowTicks());
        const bool ok = SetFileTime(file, nullptr, &now, nullptr) != FALSE;
        CloseHandle(file);
        return ok;
    }

    bool OldEnough(std::uint64_t lastWrite, std::uint64_t lastAccess, std::uint64_t now, std::uint32_t days)
    {
        if (days == 0)
            return false;
        const std::uint64_t latest = std::max(lastWrite, lastAccess);
        if (latest > now)
            return false;
        return now - latest >= static_cast<std::uint64_t>(days) * kTicksPerDay;
    }

    Kind Peek(const std::wstring& path)
    {
        ReadHandle file(path);
        if (!file.Valid())
            return Kind::Unreadable;
        std::uint8_t start[8]{};
        if (!file.Read(start, sizeof(start)))
            return Kind::Other;
        if (IsPacked(start, sizeof(start)))
            return Kind::Packed;
        return IsZstd(start, sizeof(start)) ? Kind::Compressed : IsPlainClip(start, sizeof(start)) ? Kind::Plain : Kind::Other;
    }

    std::uint64_t AvailableMemoryBytes()
    {
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (!GlobalMemoryStatusEx(&status))
            return 0;
        return std::min<std::uint64_t>(status.ullAvailPageFile, status.ullAvailVirtual);
    }

    bool ReadWholeFile(const std::wstring& path, std::vector<std::uint8_t>& bytes, const std::atomic<bool>* cancel,
        std::string& error)
    {
        bytes.clear();
        ReadHandle file(path);
        if (!file.Valid())
        {
            error = "cannot open the file";
            return false;
        }
        const std::uint64_t size = file.Size();
        if (size > kMaxClipBytes)
        {
            error = "too_large";
            return false;
        }
        bytes.resize(static_cast<std::size_t>(size));
        for (std::size_t at = 0; at < bytes.size();)
        {
            if (Cancelled(cancel))
            {
                bytes.clear();
                error = "cancelled";
                return false;
            }
            const std::size_t take = std::min<std::size_t>(bytes.size() - at, 4u << 20);
            if (!file.Read(bytes.data() + at, take))
            {
                bytes.clear();
                error = "read failed";
                return false;
            }
            at += take;
        }
        return true;
    }

    namespace
    {
        // `data` written to <path>.ztmp with the original's times, then
        // renamed over the original if it did not change meanwhile. Empty
        // on success, else the reason (`skipped` for a changed original).
        std::string WriteOver(const std::wstring& path, const std::vector<std::uint8_t>& packed, const FileTimes& before,
            const std::atomic<bool>* cancel, bool& skipped)
        {
            skipped = false;
            const auto finish = [&](bool, bool skip, std::string reason) {
                skipped = skip;
                return reason;
            };
            const std::wstring temporary = path + L".ztmp";
            HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return finish(false, false, "cannot create the temporary file");
            bool written = WriteAll(file, packed.data(), packed.size(), cancel);
            if (written)
            {
                // The original's times, so the age rule and the gallery's date
                // order see the clip as it was.
                const FILETIME creation = ToFileTime(before.creation);
                const FILETIME access = ToFileTime(before.lastAccess);
                const FILETIME write = ToFileTime(before.lastWrite);
                written = SetFileTime(file, &creation, &access, &write) && FlushFileBuffers(file);
            }
            CloseHandle(file);
            if (!written)
            {
                DeleteFileW(temporary.c_str());
                return finish(false, false, Cancelled(cancel) ? "cancelled" : "write failed");
            }
            // The original changed meanwhile (saved again): keep it.
            FileTimes now;
            if (!GetTimes(path, now) || now.lastWrite != before.lastWrite || now.bytes != before.bytes || Cancelled(cancel))
            {
                DeleteFileW(temporary.c_str());
                return finish(false, !Cancelled(cancel), Cancelled(cancel) ? "cancelled" : "changed_meanwhile");
            }
            // A reader holding the file without delete sharing (a load) makes
            // the rename fail for a moment: a few tries.
            bool moved = false;
            for (int attempt = 0; attempt < 5 && !moved; ++attempt)
            {
                moved = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
                if (!moved)
                    Sleep(200);
            }
            if (!moved)
            {
                const DWORD code = GetLastError();
                DeleteFileW(temporary.c_str());
                return finish(false, false, "rename failed " + std::to_string(code));
            }
            return std::string();
        }

        // The clip `clip` compared byte for byte with the clip the file at
        // `path` holds now: a plain file read in chunks, a zstd file
        // decompressed in chunks, a packed one unpacked (Verify).
        bool SameAsFile(const std::wstring& path, bool zstd, const std::vector<std::uint8_t>& clip, const std::atomic<bool>* cancel,
            std::string& error)
        {
            if (zstd)
            {
                std::vector<std::uint8_t> file;
                if (!ReadWholeFile(path, file, cancel, error))
                    return false;
                return Verify(file.data(), file.size(), clip.data(), clip.size(), cancel, error);
            }
            ReadHandle file(path);
            if (!file.Valid() || file.Size() != clip.size())
            {
                error = "verify: the file changed";
                return false;
            }
            std::vector<std::uint8_t> chunk(4u << 20);
            for (std::size_t at = 0; at < clip.size();)
            {
                if (Cancelled(cancel))
                {
                    error = "cancelled";
                    return false;
                }
                const std::size_t take = std::min(chunk.size(), clip.size() - at);
                if (!file.Read(chunk.data(), take))
                {
                    error = "verify: read failed";
                    return false;
                }
                if (std::memcmp(chunk.data(), clip.data() + at, take) != 0)
                {
                    error = "verify: bytes differ";
                    return false;
                }
                at += take;
            }
            return true;
        }

        // The clip of a plain, zstd or packed file.
        bool ReadClip(const std::wstring& path, std::vector<std::uint8_t>& clip, const std::atomic<bool>* cancel,
            std::string& error)
        {
            std::vector<std::uint8_t> file;
            if (!ReadWholeFile(path, file, cancel, error))
                return false;
            if (IsCompressed(file.data(), file.size()))
                return DecompressConsume(std::move(file), clip, error, 1);
            clip.swap(file);
            return true;
        }
    }

    FileResult CompressFile(const std::wstring& path, int level, const std::atomic<bool>* cancel)
    {
        FileResult result;
        const ULONGLONG startedAt = GetTickCount64();
        const auto finish = [&](bool ok, bool skipped, std::string reason) {
            result.ok = ok;
            result.skipped = skipped;
            result.reason = std::move(reason);
            result.ms = static_cast<std::uint32_t>(GetTickCount64() - startedAt);
            return result;
        };
        FileTimes before;
        if (!GetTimes(path, before))
            return finish(false, false, "cannot read the file times");
        std::vector<std::uint8_t> bytes;
        std::string error;
        if (!ReadWholeFile(path, bytes, cancel, error))
            return finish(false, error == "too_large", error);
        result.bytesIn = bytes.size();
        if (IsCompressed(bytes.data(), bytes.size()))
            return finish(false, true, "already_compressed");
        if (!IsPlainClip(bytes.data(), bytes.size()))
            return finish(false, true, "not_a_clip");

        std::vector<std::uint8_t> packed;
        if (!Compress(bytes.data(), bytes.size(), level, cancel, packed, error))
            return finish(false, false, error);
        if (!Verify(packed.data(), packed.size(), bytes.data(), bytes.size(), cancel, error))
            return finish(false, false, error);
        std::vector<std::uint8_t> summary;
        if (Summary(bytes.data(), bytes.size(), summary))
            AppendSummaryFrame(summary, packed);
        result.bytesOut = packed.size();
        if (packed.size() >= bytes.size())
            return finish(false, true, "no_gain");
        bytes.clear();
        bytes.shrink_to_fit();

        bool skipped = false;
        const std::string written = WriteOver(path, packed, before, cancel, skipped);
        if (!written.empty())
            return finish(false, skipped, written);
        return finish(true, false, "");
    }
}

namespace replay::clipcompress
{
    FileResult CompressFile(const std::wstring& path, const CompressOptions& options, const std::atomic<bool>* cancel)
    {
        if (options.format == Format::Zstd)
            return CompressFile(path, options.level, cancel);
        FileResult result;
        const ULONGLONG startedAt = GetTickCount64();
        const auto finish = [&](bool ok, bool skipped, std::string reason) {
            result.ok = ok;
            result.skipped = skipped;
            result.reason = std::move(reason);
            result.ms = static_cast<std::uint32_t>(GetTickCount64() - startedAt);
            return result;
        };
        FileTimes before;
        if (!GetTimes(path, before))
            return finish(false, false, "cannot read the file times");
        std::vector<std::uint8_t> bytes;
        std::string error;
        if (!ReadWholeFile(path, bytes, cancel, error))
            return finish(false, error == "too_large", error);
        result.bytesIn = bytes.size();
        // A packed file is repacked only when it is a save's fast pack (zstd
        // blocks); the LZMA ones are done (no loop over them).
        clippack::HeaderInfo header;
        const bool fastPacked = IsPacked(bytes.data(), bytes.size()) &&
            clippack::ReadHeaderInfo(bytes.data(), bytes.size(), header) && clippack::FastPacked(header);
        if (IsPacked(bytes.data(), bytes.size()) && (!fastPacked || !options.upgrade))
            return finish(false, true, "already_compressed");
        // A compressed source (zstd or fast packed): decompressed first,
        // compared with the file decompressed again.
        const bool zstd = IsZstd(bytes.data(), bytes.size()) || fastPacked;
        if (zstd && !options.upgrade)
            return finish(false, true, "already_compressed");
        if (!zstd && !IsPlainClip(bytes.data(), bytes.size()))
            return finish(false, true, "not_a_clip");
        const std::uint64_t clipBytes = zstd ? ContentSize(bytes.data(), bytes.size()) : bytes.size();
        if (clipBytes == 0 || clipBytes > kMaxClipBytes)
            return finish(false, true, "too_large");
        // The work's peak: the clip and its streams, then the streams, the
        // packed file and the clip unpacked again; each LZMA block being
        // compressed takes about 11 times its 32 MB.
        constexpr std::uint64_t kEncoderBytes = 400ull << 20;
        constexpr std::uint64_t kReserveBytes = 512ull << 20;
        const std::uint64_t need = clipBytes * 5 / 2 + bytes.size() + std::max(1u, options.threads) * kEncoderBytes;
        const std::uint64_t available = AvailableMemoryBytes();
        if (available != 0 && need + kReserveBytes > available)
            return finish(false, true, "low_memory");
        if (zstd)
        {
            std::vector<std::uint8_t> clip;
            if (!DecompressConsume(std::move(bytes), clip, error, std::max(1u, options.threads)))
                return finish(false, false, error);
            bytes.swap(clip);
            result.upgraded = true;
            result.fastPacked = fastPacked;
            if (!IsPlainClip(bytes.data(), bytes.size()))
                return finish(false, true, "not_a_clip");
        }
        std::vector<std::uint8_t> summary;
        const bool haveSummary = Summary(bytes.data(), bytes.size(), summary);
        const std::size_t clipSize = bytes.size();

        clippack::PackOptions pack;
        pack.codec = clippack::Codec::Lzma;
        pack.lzmaLevel = LzmaLevel(options.level);
        pack.threads = std::max(1u, options.threads);
        pack.cancel = cancel;
        std::vector<std::uint8_t> packed;
        clippack::PackStats stats;
        // The clip's buffer goes as soon as it is split; the comparison
        // reads the clip from the file again.
        if (!clippack::PackConsume(std::move(bytes), pack, packed, error, &stats))
            return finish(false, false, error);
        const auto verify = [&](std::string& why) {
            clippack::UnpackOptions unpack;
            unpack.threads = std::max(1u, options.threads);
            unpack.maxClipBytes = kMaxClipBytes;
            unpack.cancel = cancel;
            std::vector<std::uint8_t> back;
            if (!clippack::Unpack(packed.data(), packed.size(), unpack, back, why))
                return false;
            if (back.size() != clipSize)
            {
                why = "verify: size differs";
                return false;
            }
            return SameAsFile(path, zstd, back, cancel, why);
        };
        if (!verify(error))
        {
            if (Cancelled(cancel) || error == "cancelled")
                return finish(false, false, "cancelled");
            if (!stats.pose && !stats.rope)
                return finish(false, false, error);
            // The transform did not give the clip back: packed again
            // without it (still verified).
            const std::string first = error;
            std::vector<std::uint8_t> clip;
            if (!ReadClip(path, clip, cancel, error))
                return finish(false, false, error);
            pack.transform = false;
            if (!clippack::PackConsume(std::move(clip), pack, packed, error, &stats))
                return finish(false, false, error);
            if (!verify(error))
                return finish(false, false, Cancelled(cancel) ? std::string("cancelled") : first + "; plain pack: " + error);
            result.plainPack = true;
        }
        if (haveSummary)
            AppendSummaryFrame(summary, packed);
        result.bytesOut = packed.size();
        if (packed.size() >= result.bytesIn)
            return finish(false, true, "no_gain");
        bool skipped = false;
        const std::string written = WriteOver(path, packed, before, cancel, skipped);
        if (!written.empty())
            return finish(false, skipped, written);
        return finish(true, false, "");
    }
}

namespace replay::clipcompress
{
    // kSavePackLevel and kSavePackThreads, measured with ReplayClipTool
    // save-pack-verify on 1272-Blackwater (453 MB raw; 28/10/2026, release
    // build, the pose split on the same threads). Time = the pack and its
    // check, the save's added work before the (smaller) write; size of the
    // file against the raw clip and against the compressor's LZMA 9 pack
    // (98.8 MB, 21.8% of raw):
    //   zstd 1, 4 threads: 1.75 s, 32.1% raw, 1.47 x LZMA
    //   zstd 3, 4 threads: 2.00 s, 30.8% raw, 1.41 x LZMA   <- chosen
    //   zstd 6, 4 threads: 2.77 s, 29.2% raw, 1.34 x LZMA
    //   zstd 3, 1 thread:  5.45 s; zstd 3, 8 threads: 1.48 s
    // Four threads leave the game most of the processor; zstd 3 is the
    // smallest within about 2 s on a 450 MB clip.

    std::uint64_t SavePackNeedBytes(std::uint64_t clipBytes, unsigned threads)
    {
        // The streams (about the clip) while it is split, the packed blocks,
        // then the check's streams and clip; each zstd block in work holds
        // its shuffled copy and its output (about 2 x 32 MB).
        return clipBytes * 3 / 2 + static_cast<std::uint64_t>(std::max(1u, threads)) * (64ull << 20);
    }

    bool PackForSave(std::vector<std::uint8_t>& clip, const SavePackOptions& options, std::vector<std::uint8_t>& out,
        SavePackResult& result)
    {
        result = SavePackResult{};
        out.clear();
        result.clipBytes = clip.size();
        if (!IsPlainClip(clip.data(), clip.size()))
        {
            result.fallback = "not_a_clip";
            return false;
        }
        if (clip.size() > kMaxClipBytes)
        {
            result.fallback = "too_large";
            return false;
        }
        constexpr std::uint64_t kReserveBytes = 512ull << 20;
        const unsigned threads = std::clamp(options.threads, 1u, 16u);
        const std::uint64_t available = options.availableBytes ? options.availableBytes : AvailableMemoryBytes();
        if (available != 0 && SavePackNeedBytes(clip.size(), threads) + kReserveBytes > available)
        {
            result.fallback = "low_memory";
            return false;
        }
        try
        {
            const auto started = std::chrono::steady_clock::now();
            std::vector<std::uint8_t> summary;
            const bool haveSummary = Summary(clip.data(), clip.size(), summary);
            const std::uint64_t hash = clippack::HashBytes(clip.data(), clip.size());
            const std::size_t clipSize = clip.size();
            clippack::PackOptions pack;
            pack.codec = clippack::Codec::Zstd;
            pack.zstdLevel = std::clamp(options.zstdLevel, 1, 19);
            pack.threads = threads;
            pack.cancel = options.cancel;
            clippack::PackStats stats;
            std::string error;
            // The clip's buffer goes once it is split.
            if (!clippack::PackConsume(std::move(clip), pack, out, error, &stats))
            {
                out.clear();
                result.fallback = "pack: " + error;
                return false;
            }
            clip.clear();
            clip.shrink_to_fit();
            result.pose = stats.pose;
            result.rope = stats.rope;
            result.transformMs = stats.transformMs;
            result.compressMs = stats.compressMs;
            const auto packed = std::chrono::steady_clock::now();
            result.packMs = std::chrono::duration<double, std::milli>(packed - started).count();
            // The check: unpacked again, the same size and hash as the clip.
            {
                clippack::UnpackOptions unpack;
                unpack.threads = threads;
                unpack.maxClipBytes = kMaxClipBytes;
                unpack.cancel = options.cancel;
                std::vector<std::uint8_t> back;
                if (!clippack::Unpack(out.data(), out.size(), unpack, back, error))
                {
                    out.clear();
                    result.fallback = "verify: " + error;
                    return false;
                }
                if (back.size() != clipSize || clippack::HashBytes(back.data(), back.size()) != hash)
                {
                    out.clear();
                    result.fallback = "verify: bytes differ";
                    return false;
                }
            }
            result.verifyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - packed).count();
            if (haveSummary)
                AppendSummaryFrame(summary, out);
            result.fileBytes = out.size();
            return true;
        }
        catch (const std::bad_alloc&)
        {
            result.fallback = "pack: out of memory";
        }
        catch (...)
        {
            result.fallback = "pack: exception";
        }
        out.clear();
        out.shrink_to_fit();
        return false;
    }

    bool IsFastPackedFile(const std::wstring& path)
    {
        ReadHandle file(path);
        if (!file.Valid())
            return false;
        std::uint8_t start[32]{};
        if (file.Size() < sizeof(start) || !file.Read(start, sizeof(start)))
            return false;
        const std::size_t headerBytes = clippack::HeaderBytes(start, sizeof(start));
        if (headerBytes == 0 || headerBytes > file.Size() || headerBytes > (4u << 20))
            return false;
        std::vector<std::uint8_t> header(headerBytes);
        std::memcpy(header.data(), start, sizeof(start));
        if (!file.Read(header.data() + sizeof(start), headerBytes - sizeof(start)))
            return false;
        clippack::HeaderInfo info;
        return clippack::ReadHeaderInfo(header.data(), header.size(), info) && clippack::FastPacked(info);
    }
}
