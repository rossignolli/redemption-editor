#pragma once

// QA kit: a .zip writer for the QA report.
//
// Plain PKZIP (no zip64): local header, data, central directory, end
// record. Each entry is stored (method 0) or raw deflate (method 8). The
// deflate comes from zlib loaded at run time (z.dll of the video export's
// libraries, RedemptionEditor\dlls), through the function pointers of
// ZlibApi; without them every entry is stored and the zip is still valid.
// Sizes and the CRC go into the local header by patching it after the
// data (the sink seeks back), so an entry is streamed in chunks.
//
// Limits (no zip64): every entry and the whole archive under 4 GiB, at
// most 65535 entries; past them the writer fails and the caller keeps the
// plain folder.
//
// Pure: unit tested in ReplayCoreTests (qa_kit_tests.cpp, with an in-memory
// sink and, when it is there, the staged z.dll).

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace replay::qa::zip
{
    inline std::uint32_t Crc32Update(std::uint32_t crc, const std::uint8_t* data, std::size_t size)
    {
        static const struct Table
        {
            std::uint32_t v[256];
            Table()
            {
                for (std::uint32_t n = 0; n < 256; ++n)
                {
                    std::uint32_t c = n;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    v[n] = c;
                }
            }
        } table;
        crc = ~crc;
        for (std::size_t n = 0; n < size; ++n)
            crc = table.v[(crc ^ data[n]) & 0xFF] ^ (crc >> 8);
        return ~crc;
    }

    // MS-DOS date and time of a local time (1980..2107; earlier is 1980).
    inline void DosDateTime(unsigned year, unsigned month, unsigned day, unsigned hour, unsigned minute, unsigned second,
        std::uint16_t& date, std::uint16_t& time)
    {
        if (year < 1980)
        {
            year = 1980;
            month = 1;
            day = 1;
        }
        date = static_cast<std::uint16_t>(((year - 1980) << 9) | ((month & 15) << 5) | (day & 31));
        time = static_cast<std::uint16_t>(((hour & 31) << 11) | ((minute & 63) << 5) | ((second / 2) & 31));
    }

    // Where the archive goes: a file (qa_bundle.cpp) or memory (tests).
    struct Sink
    {
        virtual ~Sink() = default;
        virtual bool Write(const void* data, std::size_t size) = 0;
        virtual std::uint64_t Tell() const = 0;
        // Overwrites `size` bytes at `offset` (already written) and goes
        // back to the end.
        virtual bool Patch(std::uint64_t offset, const void* data, std::size_t size) = 0;
    };

    struct MemorySink final : Sink
    {
        std::vector<std::uint8_t> bytes;
        bool Write(const void* data, std::size_t size) override
        {
            const auto* p = static_cast<const std::uint8_t*>(data);
            bytes.insert(bytes.end(), p, p + size);
            return true;
        }
        std::uint64_t Tell() const override { return bytes.size(); }
        bool Patch(std::uint64_t offset, const void* data, std::size_t size) override
        {
            if (offset + size > bytes.size())
                return false;
            std::memcpy(bytes.data() + offset, data, size);
            return true;
        }
    };

    // zlib's z_stream (1.x, 64-bit Windows: uLong is 32 bits). deflateInit2_
    // checks the version's first digit and this struct's size, so a zlib
    // with another layout refuses (Z_VERSION_ERROR) and the entry is stored.
    struct ZStream
    {
        const std::uint8_t* next_in;
        unsigned int avail_in;
        unsigned long total_in;
        std::uint8_t* next_out;
        unsigned int avail_out;
        unsigned long total_out;
        const char* msg;
        void* state;
        void* zalloc;
        void* zfree;
        void* opaque;
        int data_type;
        unsigned long adler;
        unsigned long reserved;
    };

    struct ZlibApi
    {
        int (*deflateInit2_)(ZStream*, int level, int method, int windowBits, int memLevel, int strategy, const char* version,
            int streamSize) = nullptr;
        int (*deflate)(ZStream*, int flush) = nullptr;
        int (*deflateEnd)(ZStream*) = nullptr;
        bool Ready() const { return deflateInit2_ && deflate && deflateEnd; }
    };

    inline constexpr int kZOk = 0;
    inline constexpr int kZStreamEnd = 1;
    inline constexpr int kZNoFlush = 0;
    inline constexpr int kZFinish = 4;
    inline constexpr int kZDeflated = 8;

    class Writer
    {
    public:
        Writer(Sink& sink, const ZlibApi* zlib = nullptr, int level = 6) : m_sink(sink), m_zlib(zlib), m_level(level) {}
        ~Writer() { EndDeflate(); }
        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;

        // `deflate` false: stored (already compressed data, a JPEG, a
        // zstd clip). Falls back to stored when zlib is not there.
        bool Begin(const std::string& name, bool deflate, std::uint16_t dosDate, std::uint16_t dosTime)
        {
            if (!m_error.empty())
                return false;
            if (m_open)
                return Fail("entry_open");
            if (m_entries.size() >= 0xFFFF)
                return Fail("too_many_entries");
            if (name.empty() || name.size() > 0xFFFF)
                return Fail("bad_name");
            Entry entry;
            entry.name = name;
            for (char& c : entry.name)
                if (c == '\\')
                    c = '/';
            entry.date = dosDate;
            entry.time = dosTime;
            entry.offset = m_sink.Tell();
            if (entry.offset > 0xFFFFFFFFull)
                return Fail("zip64_needed");
            entry.method = 0;
            if (deflate && m_zlib && m_zlib->Ready())
            {
                std::memset(&m_z, 0, sizeof(m_z));
                if (m_zlib->deflateInit2_(&m_z, m_level, kZDeflated, -15, 8, 0, "1.3.1", static_cast<int>(sizeof(ZStream))) == kZOk)
                {
                    m_deflating = true;
                    entry.method = 8;
                }
            }
            m_entries.push_back(entry);
            m_open = true;
            m_crc = 0;
            m_raw = 0;
            m_packed = 0;
            return WriteLocalHeader(m_entries.back());
        }

        bool Write(const std::uint8_t* data, std::size_t size)
        {
            if (!m_error.empty())
                return false;
            if (!m_open)
                return Fail("no_entry");
            m_crc = Crc32Update(m_crc, data, size);
            m_raw += size;
            if (m_raw > 0xFFFFFFFFull)
                return Fail("zip64_needed");
            if (!m_deflating)
            {
                m_packed += size;
                return Put(data, size);
            }
            return Pump(data, size, false);
        }

        bool End()
        {
            if (!m_error.empty())
                return false;
            if (!m_open)
                return Fail("no_entry");
            if (m_deflating && !Pump(nullptr, 0, true))
                return false;
            EndDeflate();
            Entry& entry = m_entries.back();
            entry.crc = m_crc;
            entry.raw = static_cast<std::uint32_t>(m_raw);
            if (m_packed > 0xFFFFFFFFull)
                return Fail("zip64_needed");
            entry.packed = static_cast<std::uint32_t>(m_packed);
            m_open = false;
            std::uint8_t fields[12];
            Put32(fields + 0, entry.crc);
            Put32(fields + 4, entry.packed);
            Put32(fields + 8, entry.raw);
            if (!m_sink.Patch(entry.offset + 14, fields, sizeof(fields)))
                return Fail("patch_failed");
            return true;
        }

        // One whole entry from memory.
        bool Add(const std::string& name, const void* data, std::size_t size, bool deflate, std::uint16_t dosDate, std::uint16_t dosTime)
        {
            return Begin(name, deflate, dosDate, dosTime) && Write(static_cast<const std::uint8_t*>(data), size) && End();
        }

        // The central directory and the end record.
        bool Finish()
        {
            if (!m_error.empty())
                return false;
            if (m_open)
                return Fail("entry_open");
            const std::uint64_t start = m_sink.Tell();
            if (start > 0xFFFFFFFFull)
                return Fail("zip64_needed");
            for (const Entry& entry : m_entries)
            {
                std::vector<std::uint8_t> header(46 + entry.name.size());
                std::uint8_t* h = header.data();
                Put32(h + 0, 0x02014B50u);
                Put16(h + 4, 20);    // made by: MS-DOS, 2.0
                Put16(h + 6, 20);    // needed: 2.0
                Put16(h + 8, 0x0800); // UTF-8 names
                Put16(h + 10, entry.method);
                Put16(h + 12, entry.time);
                Put16(h + 14, entry.date);
                Put32(h + 16, entry.crc);
                Put32(h + 20, entry.packed);
                Put32(h + 24, entry.raw);
                Put16(h + 28, static_cast<std::uint16_t>(entry.name.size()));
                Put16(h + 30, 0);
                Put16(h + 32, 0);
                Put16(h + 34, 0);
                Put16(h + 36, 0);
                Put32(h + 38, 0);
                Put32(h + 42, static_cast<std::uint32_t>(entry.offset));
                std::memcpy(h + 46, entry.name.data(), entry.name.size());
                if (!Put(header.data(), header.size()))
                    return false;
            }
            const std::uint64_t end = m_sink.Tell();
            if (end > 0xFFFFFFFFull)
                return Fail("zip64_needed");
            std::uint8_t record[22];
            Put32(record + 0, 0x06054B50u);
            Put16(record + 4, 0);
            Put16(record + 6, 0);
            Put16(record + 8, static_cast<std::uint16_t>(m_entries.size()));
            Put16(record + 10, static_cast<std::uint16_t>(m_entries.size()));
            Put32(record + 12, static_cast<std::uint32_t>(end - start));
            Put32(record + 16, static_cast<std::uint32_t>(start));
            Put16(record + 20, 0);
            return Put(record, sizeof(record));
        }

        const std::string& Error() const { return m_error; }
        std::size_t Entries() const { return m_entries.size(); }
        // Entries that went out deflated.
        std::size_t Deflated() const
        {
            std::size_t count = 0;
            for (const Entry& entry : m_entries)
                count += entry.method == 8 ? 1 : 0;
            return count;
        }

    private:
        struct Entry
        {
            std::string name;
            std::uint16_t method = 0;
            std::uint16_t date = 0;
            std::uint16_t time = 0;
            std::uint32_t crc = 0;
            std::uint32_t packed = 0;
            std::uint32_t raw = 0;
            std::uint64_t offset = 0;
        };

        static void Put16(std::uint8_t* p, std::uint16_t v)
        {
            p[0] = static_cast<std::uint8_t>(v);
            p[1] = static_cast<std::uint8_t>(v >> 8);
        }
        static void Put32(std::uint8_t* p, std::uint32_t v)
        {
            for (int n = 0; n < 4; ++n)
                p[n] = static_cast<std::uint8_t>(v >> (8 * n));
        }

        bool Fail(const char* error)
        {
            if (m_error.empty())
                m_error = error;
            EndDeflate();
            return false;
        }

        bool Put(const void* data, std::size_t size)
        {
            if (size && !m_sink.Write(data, size))
                return Fail("write_failed");
            return true;
        }

        bool WriteLocalHeader(const Entry& entry)
        {
            std::vector<std::uint8_t> header(30 + entry.name.size());
            std::uint8_t* h = header.data();
            Put32(h + 0, 0x04034B50u);
            Put16(h + 4, 20);
            Put16(h + 6, 0x0800);
            Put16(h + 8, entry.method);
            Put16(h + 10, entry.time);
            Put16(h + 12, entry.date);
            Put32(h + 14, 0); // crc, packed and raw sizes: patched by End
            Put32(h + 18, 0);
            Put32(h + 22, 0);
            Put16(h + 26, static_cast<std::uint16_t>(entry.name.size()));
            Put16(h + 28, 0);
            std::memcpy(h + 30, entry.name.data(), entry.name.size());
            return Put(header.data(), header.size());
        }

        bool Pump(const std::uint8_t* data, std::size_t size, bool finish)
        {
            std::uint8_t out[1 << 16];
            m_z.next_in = data;
            m_z.avail_in = static_cast<unsigned int>(size);
            while (true)
            {
                m_z.next_out = out;
                m_z.avail_out = sizeof(out);
                const int result = m_zlib->deflate(&m_z, finish ? kZFinish : kZNoFlush);
                if (result != kZOk && result != kZStreamEnd && !(result == -5 && m_z.avail_in == 0)) // -5: Z_BUF_ERROR, nothing to do
                    return Fail("deflate_failed");
                const std::size_t produced = sizeof(out) - m_z.avail_out;
                m_packed += produced;
                if (!Put(out, produced))
                    return false;
                if (finish ? result == kZStreamEnd : (m_z.avail_in == 0 && m_z.avail_out != 0))
                    return true;
            }
        }

        void EndDeflate()
        {
            if (m_deflating && m_zlib)
                m_zlib->deflateEnd(&m_z);
            m_deflating = false;
        }

        Sink& m_sink;
        const ZlibApi* m_zlib;
        int m_level;
        std::vector<Entry> m_entries;
        bool m_open = false;
        bool m_deflating = false;
        ZStream m_z{};
        std::uint32_t m_crc = 0;
        std::uint64_t m_raw = 0;
        std::uint64_t m_packed = 0;
        std::string m_error;
    };

    // Reads back an archive's central directory (tests and the report's
    // check that the file it wrote is whole): name, method, crc, sizes.
    struct ListedEntry
    {
        std::string name;
        std::uint16_t method = 0;
        std::uint32_t crc = 0;
        std::uint32_t packed = 0;
        std::uint32_t raw = 0;
        std::uint32_t offset = 0;
    };

    inline bool ListArchive(const std::uint8_t* data, std::size_t size, std::vector<ListedEntry>& out)
    {
        out.clear();
        if (size < 22)
            return false;
        const auto get16 = [&](std::size_t at) { return static_cast<std::uint16_t>(data[at] | (data[at + 1] << 8)); };
        const auto get32 = [&](std::size_t at) {
            return static_cast<std::uint32_t>(data[at]) | (static_cast<std::uint32_t>(data[at + 1]) << 8) |
                (static_cast<std::uint32_t>(data[at + 2]) << 16) | (static_cast<std::uint32_t>(data[at + 3]) << 24);
        };
        std::size_t end = size - 22;
        while (true)
        {
            if (get32(end) == 0x06054B50u)
                break;
            if (end == 0 || size - end > 22 + 0xFFFF)
                return false;
            --end;
        }
        const std::uint16_t count = get16(end + 10);
        std::size_t at = get32(end + 16);
        for (std::uint16_t n = 0; n < count; ++n)
        {
            if (at + 46 > size || get32(at) != 0x02014B50u)
                return false;
            ListedEntry entry;
            entry.method = get16(at + 10);
            entry.crc = get32(at + 16);
            entry.packed = get32(at + 20);
            entry.raw = get32(at + 24);
            const std::uint16_t nameLength = get16(at + 28);
            const std::size_t extra = static_cast<std::size_t>(get16(at + 30)) + get16(at + 32);
            entry.offset = get32(at + 42);
            if (at + 46 + nameLength > size)
                return false;
            entry.name.assign(reinterpret_cast<const char*>(data + at + 46), nameLength);
            // The local header agrees.
            const std::size_t local = entry.offset;
            if (local + 30 > size || get32(local) != 0x04034B50u || get32(local + 14) != entry.crc ||
                get32(local + 18) != entry.packed || get32(local + 22) != entry.raw)
                return false;
            out.push_back(entry);
            at += 46 + nameLength + extra;
        }
        return true;
    }
}
