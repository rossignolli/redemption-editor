#include "game_sig.h"

#include "guarded_memory.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace replay::sig
{
    namespace
    {
        // The exe the 1491.50 addresses come from (Steam).
        constexpr std::uint32_t kKnownTimestamp = 0x65EEC92E;
        constexpr std::uint32_t kKnownImageSize = 0x7397600;
        constexpr std::uint32_t kKnownChecksum = 0x5576F9C;

        struct Section
        {
            std::uint32_t rva;
            std::uint32_t size;
            char name[9];
            bool code;
            bool writable;
            std::uint32_t fileSize; // the bytes the exe file holds (SizeOfRawData); zero-filled past them
        };

        struct Image
        {
            bool read = false;
            std::uintptr_t base = 0;
            std::uint32_t timestamp = 0;
            std::uint32_t imageSize = 0;
            std::uint32_t checksum = 0;
            bool known = false;
            std::vector<Section> code; // the executable sections
            std::vector<Section> all;
        };

        std::mutex g_lock;
        Image g_image;
        bool g_forceScan = false;
        bool g_resolved = false;
        // The hook-tolerant signatures whose code started with another
        // module's jump when they were resolved (before any detour of ours).
        std::vector<const Signature*> g_hookedByOthers;
        double g_scanMs = 0;
        int g_scanFaults = 0;

        void ReadImage()
        {
            if (g_image.read)
                return;
            g_image.read = true;
            const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            IMAGE_DOS_HEADER dos{};
            if (!base || !mem::Copy(&dos, base, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE)
                return;
            IMAGE_NT_HEADERS64 nt{};
            const std::uintptr_t ntAt = base + static_cast<std::uintptr_t>(dos.e_lfanew);
            if (!mem::Copy(&nt, ntAt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE)
                return;
            g_image.base = base;
            g_image.timestamp = nt.FileHeader.TimeDateStamp;
            g_image.imageSize = nt.OptionalHeader.SizeOfImage;
            g_image.checksum = nt.OptionalHeader.CheckSum;
            g_image.known = g_image.timestamp == kKnownTimestamp && g_image.imageSize == kKnownImageSize &&
                            g_image.checksum == kKnownChecksum;
            const std::uintptr_t sectionsAt =
                ntAt + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
            for (WORD n = 0; n < nt.FileHeader.NumberOfSections; ++n)
            {
                IMAGE_SECTION_HEADER section{};
                if (!mem::Copy(&section, sectionsAt + n * sizeof(section), sizeof(section)))
                    break;
                if (section.Misc.VirtualSize == 0 ||
                    section.VirtualAddress + static_cast<std::uint64_t>(section.Misc.VirtualSize) > g_image.imageSize)
                    continue;
                Section entry{section.VirtualAddress, section.Misc.VirtualSize, {}, (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0,
                    (section.Characteristics & IMAGE_SCN_MEM_WRITE) != 0, section.SizeOfRawData};
                std::memcpy(entry.name, section.Name, 8);
                g_image.all.push_back(entry);
                if (entry.code)
                    g_image.code.push_back(entry);
            }
        }

        bool PatternAt(std::uintptr_t address, const Pattern& pattern)
        {
            std::uint8_t code[kMaxPatternBytes];
            return mem::Copy(code, address, pattern.size) && replay::sig::MatchesAt(pattern, code, pattern.size);
        }

        // MSVC: no object with a destructor in a function with __try.
        bool GuardedScan(const ScanIndex& index, const std::uint8_t* data, std::size_t size, std::uint64_t origin, Hits* hits)
        {
            __try
            {
                index.Scan(data, size, origin, hits);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        void Settle(const Signature& s, Result result, std::uintptr_t address)
        {
            s.address.store(address, std::memory_order_relaxed);
            s.result.store(result, std::memory_order_release);
        }

        // Every pending signature of `list` searched in one pass over the
        // code sections.
        void Search(const std::vector<const Signature*>& list)
        {
            if (list.empty())
                return;
            std::vector<Pattern> patterns(list.size());
            std::vector<const Pattern*> parsed;
            std::vector<const Signature*> owners;
            parsed.reserve(list.size());
            owners.reserve(list.size());
            for (std::size_t n = 0; n < list.size(); ++n)
            {
                // A pattern without an indexable pair cannot be searched: it is
                // only a check at an address (MatchesAt).
                if (!Parse(list[n]->pattern, patterns[n]) || !patterns[n].hasKey)
                {
                    Settle(*list[n], Result::Bad, 0);
                    continue;
                }
                parsed.push_back(&patterns[n]);
                owners.push_back(list[n]);
            }
            const ScanIndex index(parsed.data(), parsed.size());
            std::vector<Hits> hits(parsed.size());
            LARGE_INTEGER frequency{}, a{}, b{};
            QueryPerformanceFrequency(&frequency);
            QueryPerformanceCounter(&a);
            bool faulted = false;
            for (const Section& section : g_image.code)
            {
                const auto* data = reinterpret_cast<const std::uint8_t*>(g_image.base + section.rva);
                if (!GuardedScan(index, data, section.size, section.rva, hits.data()))
                {
                    faulted = true;
                    ++g_scanFaults;
                }
            }
            QueryPerformanceCounter(&b);
            g_scanMs += static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
            for (std::size_t n = 0; n < owners.size(); ++n)
            {
                // A search cut by a fault proves nothing: missing, not ambiguous.
                if (hits[n].count == 1 && !faulted)
                    Settle(*owners[n], Result::Scan, g_image.base + static_cast<std::uintptr_t>(hits[n].first));
                else
                    Settle(*owners[n], hits[n].count > 1 ? Result::Ambiguous : Result::Missing, 0);
            }
        }

        bool InImage(std::uintptr_t address)
        {
            return address >= g_image.base && address < g_image.base + g_image.imageSize;
        }

        // Anchors: checked at their 1491.50 address on the known exe, else
        // searched together. Under g_lock.
        void ResolveAnchors(const std::vector<const Signature*>& list)
        {
            std::vector<const Signature*> search;
            for (const Signature* s : list)
            {
                if (s->result.load(std::memory_order_acquire) != Result::Pending)
                    continue;
                if (!g_image.base)
                {
                    Settle(*s, Result::Missing, 0);
                    continue;
                }
                Pattern pattern;
                if (!Parse(s->pattern, pattern))
                {
                    Settle(*s, Result::Bad, 0);
                    continue;
                }
                if (g_image.known && !g_forceScan && s->rva < g_image.imageSize &&
                    PatternAt(g_image.base + s->rva, pattern))
                {
                    Settle(*s, Result::Hint, g_image.base + s->rva);
                    continue;
                }
                // Another exe, the forced search, or the known exe with other
                // bytes at the address (another mod's detour there): search.
                search.push_back(s);
            }
            Search(search);
        }

        std::uintptr_t Usable(const Signature& s)
        {
            const Result result = s.result.load(std::memory_order_acquire);
            return result == Result::Hint || result == Result::Scan || result == Result::Derived
                ? s.address.load(std::memory_order_relaxed)
                : 0;
        }

        // The static objects of the writable sections whose first 8 bytes are
        // `head` (8-aligned), the pattern checked on each: how many (counting
        // stops at 2) and the first. On the known exe the 1491.50 address is
        // checked first and is the whole answer when it holds.
        struct Instances
        {
            int count = 0;
            std::uintptr_t first = 0;
            bool incomplete = false; // a page could not be read: a second one may hide there
        };

        Instances FindInstances(const Signature& s, std::uint64_t head, const Pattern* pattern)
        {
            auto holds = [&](std::uintptr_t at) {
                std::uint64_t value = 0;
                return mem::Read(at, value) && value == head && (!pattern || PatternAt(at, *pattern));
            };
            Instances found;
            if (g_image.known && !g_forceScan && s.rva < g_image.imageSize && holds(g_image.base + s.rva))
            {
                found.count = 1;
                found.first = g_image.base + s.rva;
                return found;
            }
            std::vector<std::uint64_t> chunk(8192);
            for (const Section& section : g_image.all)
            {
                if (!section.writable || section.code)
                    continue;
                // A constant initialized object lies in the part the file holds.
                const std::uint32_t size = s.at == 1 ? std::min(section.size, section.fileSize) : section.size;
                const std::uintptr_t begin = g_image.base + ((section.rva + 7u) & ~7u);
                const std::uintptr_t end = g_image.base + section.rva + size;
                for (std::uintptr_t at = begin; at + 8 <= end && found.count < 2; at += chunk.size() * 8)
                {
                    const std::size_t words = static_cast<std::size_t>(std::min<std::uintptr_t>(chunk.size(), (end - at) / 8));
                    if (!mem::Copy(chunk.data(), at, words * 8))
                    {
                        found.incomplete = true;
                        continue;
                    }
                    for (std::size_t n = 0; n < words && found.count < 2; ++n)
                    {
                        if (chunk[n] != head || !holds(at + n * 8))
                            continue;
                        if (++found.count == 1)
                            found.first = at + n * 8;
                    }
                }
            }
            return found;
        }

        // A signature reached from its parent (resolved first). Under g_lock.
        void ResolveDerived(const Signature& s, int depth)
        {
            if (s.result.load(std::memory_order_acquire) != Result::Pending)
                return;
            const Signature* parent = s.parent;
            if (!parent || depth > 16)
            {
                Settle(s, Result::Bad, 0);
                return;
            }
            if (parent->result.load(std::memory_order_acquire) == Result::Pending)
            {
                if (parent->via == Via::Search)
                    ResolveAnchors({parent});
                else
                    ResolveDerived(*parent, depth + 1);
            }
            const std::uintptr_t from = Usable(*parent);
            std::int32_t disp = 0;
            std::uint64_t pointee = 0;
            if (!from || (s.via == Via::Rel32 && !mem::Read(from + s.at, disp)) ||
                (s.via == Via::Pointer && !mem::Read(from + s.at, pointee)))
            {
                Settle(s, Result::Missing, 0);
                return;
            }
            if (s.via == Via::Instance)
            {
                Pattern pattern;
                const bool patterned = *s.pattern != 0;
                if (patterned && !Parse(s.pattern, pattern))
                {
                    Settle(s, Result::Bad, 0);
                    return;
                }
                const Instances found = FindInstances(s, from, patterned ? &pattern : nullptr);
                // One found with a part of the data unread is not proven unique.
                if (found.count == 1 && !found.incomplete)
                    Settle(s, Result::Derived, found.first);
                else if (found.count == 1)
                    Settle(s, Result::Ambiguous, 0);
                else
                    Settle(s, found.count > 1 ? Result::Ambiguous : Result::Missing, 0);
                return;
            }
            const auto address = s.via == Via::Pointer
                ? static_cast<std::uintptr_t>(pointee)
                : static_cast<std::uintptr_t>(DerivedAddress(s, static_cast<std::int64_t>(from), disp));
            if (!*s.pattern)
            {
                // A global: inside the image is all that can be checked.
                Settle(s, InImage(address) ? Result::Derived : Result::Missing, InImage(address) ? address : 0);
                return;
            }
            Pattern pattern;
            if (!Parse(s.pattern, pattern))
            {
                Settle(s, Result::Bad, 0);
                return;
            }
            const bool same = InImage(address) && PatternAt(address, pattern);
            Settle(s, same ? Result::Derived : Result::Missing, same ? address : 0);
        }

        // Under g_lock.
        void ResolveLocked(const Signature* only)
        {
            ReadImage();
            // The anchors first (one search for all of them), then what is
            // reached from them.
            std::vector<const Signature*> anchors, derived;
            for (const Signature* s = only ? only : FirstSignature(); s; s = only ? nullptr : s->link)
            {
                if (s->result.load(std::memory_order_acquire) != Result::Pending)
                    continue;
                (s->via == Via::Search ? anchors : derived).push_back(s);
            }
            ResolveAnchors(anchors);
            for (const Signature* s : derived)
                ResolveDerived(*s, 0);
            for (const Signature* s = only ? only : FirstSignature(); s; s = only ? nullptr : s->link)
            {
                std::uint8_t head[2] = {};
                const std::uintptr_t address = Usable(*s);
                if (address && HookTolerant(s->pattern) && mem::Copy(head, address, sizeof(head)) &&
                    sig::StartsWithJump(head, sizeof(head)) &&
                    std::find(g_hookedByOthers.begin(), g_hookedByOthers.end(), s) == g_hookedByOthers.end())
                    g_hookedByOthers.push_back(s);
            }
        }
    }

    void Configure(bool forceScan)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        // A script restart runs ScriptMain again in this module: the
        // signatures stay as they were resolved, and so does the mode the log
        // reports.
        if (!g_resolved)
            g_forceScan = forceScan;
    }

    void ResolveAll()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_resolved)
            return;
        g_resolved = true;
        ResolveLocked(nullptr);
    }

    std::uintptr_t Find(const Signature& s)
    {
        if (s.result.load(std::memory_order_acquire) == Result::Pending)
        {
            std::lock_guard<std::mutex> lock(g_lock);
            if (!g_resolved)
            {
                g_resolved = true;
                ResolveLocked(nullptr);
            }
            ResolveLocked(&s); // one built after ResolveAll
        }
        return Usable(s);
    }

    bool MatchesAt(std::uintptr_t address, const char* pattern)
    {
        Pattern parsed;
        return address && Parse(pattern, parsed) && PatternAt(address, parsed);
    }

    bool StartsWithJump(std::uintptr_t address)
    {
        std::uint8_t head[2] = {};
        return address && mem::Copy(head, address, sizeof(head)) && sig::StartsWithJump(head, sizeof(head));
    }

    std::uintptr_t RipTarget(std::uintptr_t instruction, std::uint32_t dispAt, std::uint32_t length)
    {
        std::int32_t displacement = 0;
        if (!instruction || !mem::Read(instruction + dispAt, displacement))
            return 0;
        return static_cast<std::uintptr_t>(Rel32Target(static_cast<std::int64_t>(instruction), length, displacement));
    }

    std::uintptr_t ImageBase()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        ReadImage();
        return g_image.base;
    }

    bool KnownExe()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        ReadImage();
        return g_image.known;
    }

    void Report(std::ostream& log)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        ReadImage();
        int total = 0, hint = 0, scan = 0, derived = 0, missing = 0, ambiguous = 0, bad = 0, pending = 0, moved = 0;
        for (const Signature* s = FirstSignature(); s; s = s->link)
        {
            ++total;
            const Result result = s->result.load(std::memory_order_acquire);
            const std::uintptr_t address = s->address.load(std::memory_order_relaxed);
            const bool away = (result == Result::Scan || result == Result::Derived) && address != g_image.base + s->rva;
            switch (result)
            {
            case Result::Hint: ++hint; break;
            case Result::Scan: ++scan; break;
            case Result::Derived: ++derived; break;
            case Result::Missing: ++missing; break;
            case Result::Ambiguous: ++ambiguous; break;
            case Result::Bad: ++bad; break;
            case Result::Pending: ++pending; break;
            }
            moved += away ? 1 : 0;
            const bool failed = result != Result::Hint && result != Result::Scan && result != Result::Derived;
            if (!failed && !g_forceScan)
                continue;
            char line[256];
            static const char* const kVia[] = {"search", "rel32", "offset", "pointer", "instance"};
            std::snprintf(line, sizeof(line), "SIG,name=%s,via=%s,result=%s,rva=0x%llX,rva_1491=0x%X,moved=%d\n", s->name,
                kVia[static_cast<int>(s->via)], ResultName(result),
                static_cast<unsigned long long>(address && address >= g_image.base ? address - g_image.base : 0), s->rva,
                away ? 1 : 0);
            log << line;
        }
        for (const Signature* s : g_hookedByOthers)
        {
            char line[256];
            std::snprintf(line, sizeof(line), "SIG_HOOKED,name=%s,rva=0x%llX,note=hooked by another module\n", s->name,
                static_cast<unsigned long long>(s->address.load(std::memory_order_relaxed) - g_image.base));
            log << line;
        }
        char summary[512];
        std::snprintf(summary, sizeof(summary),
            "SIG_SUMMARY,exe_timestamp=0x%X,exe_image=0x%X,exe_checksum=0x%X,known=%d,force_scan=%d,signatures=%d,"
            "hint=%d,scan=%d,derived=%d,missing=%d,ambiguous=%d,bad=%d,pending=%d,moved=%d,code_sections=%zu,"
            "scan_ms=%.1f,scan_faults=%d\n",
            g_image.timestamp, g_image.imageSize, g_image.checksum, g_image.known ? 1 : 0, g_forceScan ? 1 : 0, total, hint,
            scan, derived, missing, ambiguous, bad, pending, moved, g_image.code.size(), g_scanMs, g_scanFaults);
        log << summary;
    }

    bool DumpCode(const std::wstring& parent, std::string& report)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        ReadImage();
        if (!g_image.base)
        {
            report = "error=image";
            return false;
        }
        wchar_t folder[MAX_PATH]{};
        swprintf_s(folder, L"%ls\\%08X-%08X", parent.c_str(), g_image.timestamp, g_image.checksum);
        CreateDirectoryW(folder, nullptr);
        LARGE_INTEGER frequency{}, a{}, b{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&a);
        std::vector<std::uint8_t> chunk(1u << 20);
        int files = 0;
        int codeIndex = 0;
        std::uint64_t bytes = 0;
        for (const Section& section : g_image.all)
        {
            wchar_t name[MAX_PATH]{};
            // The code, and the sections sigtool reads besides it: .pdata
            // (func), .rdata (vtables) and .data (static objects).
            if (section.code)
                swprintf_s(name, L"%ls\\.text_%d_.rva_%08X.size_%08X.bin", folder, codeIndex++, section.rva, section.size);
            else if (std::strcmp(section.name, ".pdata") == 0 || std::strcmp(section.name, ".rdata") == 0 ||
                     std::strcmp(section.name, ".data") == 0)
                swprintf_s(name, L"%ls\\%hs.rva_%08X.size_%08X.bin", folder, section.name, section.rva, section.size);
            else
                continue;
            FILE* file = nullptr;
            if (_wfopen_s(&file, name, L"wb") != 0 || !file)
            {
                report = "error=open";
                return false;
            }
            bool ok = true;
            for (std::uint32_t done = 0; done < section.size && ok;)
            {
                const std::uint32_t n = std::min<std::uint32_t>(static_cast<std::uint32_t>(chunk.size()), section.size - done);
                ok = mem::Copy(chunk.data(), g_image.base + section.rva + done, n) && std::fwrite(chunk.data(), 1, n, file) == n;
                done += n;
            }
            std::fclose(file);
            if (!ok)
            {
                report = "error=read_or_write";
                return false;
            }
            ++files;
            bytes += section.size;
        }
        wchar_t manifest[MAX_PATH]{};
        swprintf_s(manifest, L"%ls\\manifest.txt", folder);
        FILE* file = nullptr;
        if (_wfopen_s(&file, manifest, L"w") == 0 && file)
        {
            std::fprintf(file, "module_base=0x%llX\nexe_timestamp=0x%X\nexe_image=0x%X\nexe_checksum=0x%X\nknown_1491_50=%d\n",
                static_cast<unsigned long long>(g_image.base), g_image.timestamp, g_image.imageSize, g_image.checksum,
                g_image.known ? 1 : 0);
            // Instance{true} searches only the part of .data the file holds.
            for (const Section& section : g_image.all)
                if (std::strcmp(section.name, ".data") == 0)
                    std::fprintf(file, "data_file_size=0x%X\n", section.fileSize);
            std::fclose(file);
        }
        QueryPerformanceCounter(&b);
        char text[128];
        std::snprintf(text, sizeof(text), "files=%d,bytes=%llu,ms=%.0f", files, static_cast<unsigned long long>(bytes),
            static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart));
        report = text;
        char utf8[MAX_PATH * 3]{};
        WideCharToMultiByte(CP_UTF8, 0, folder, -1, utf8, sizeof(utf8), nullptr, nullptr);
        report += ",folder=";
        report += utf8;
        return true;
    }
}
