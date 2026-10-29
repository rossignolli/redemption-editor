#include "clip_compress_worker.h"

#include "clip_compress.h"
#include "module_lifetime.h"
#include "thread_names.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <mutex>

namespace replay::clipcompress::worker
{
    namespace
    {
        constexpr DWORD kRescanMs = 30 * 60 * 1000;
        constexpr int kHeldRetries = 3;

        HANDLE g_thread = nullptr;
        HANDLE g_wake = nullptr;
        std::atomic<bool> g_running{false};
        std::atomic<bool> g_exited{false};
        // Set to stop the file in progress: unload, a save or a load
        // starting, the file opened in the editor.
        std::atomic<bool> g_cancel{false};
        std::atomic<bool> g_hold{false};
        std::atomic<bool> g_manualRequested{false};
        std::atomic<bool> g_fileRequested{false}; // one clip, any age (g_onlyFile)
        std::wstring g_onlyFile;                  // guarded by g_lock
        std::atomic<bool> g_automatic{true};
        std::atomic<std::uint32_t> g_days{kDefaultDays};
        std::atomic<int> g_level{19};
        std::atomic<int> g_format{static_cast<int>(Format::Packed)};
        std::atomic<bool> g_upgrade{true};
        std::atomic<std::uint32_t> g_state{static_cast<std::uint32_t>(State::Off)};
        std::atomic<std::uint32_t> g_done{0};
        std::atomic<std::uint32_t> g_total{0};
        std::atomic<std::uint32_t> g_compressed{0};
        std::atomic<std::uint64_t> g_changes{0};
        DWORD g_firstDelayMs = 30000;

        std::mutex g_lock; // the strings below
        std::wstring g_folder;
        std::wstring g_openClip;  // file name
        std::wstring g_fileInWork; // file name
        std::vector<std::string> g_log;
        // Files that failed this session (not skipped): not tried again in
        // every pass (a large clip costs minutes of a thread each time).
        std::vector<std::wstring> g_failedNames;

        bool FailedBefore(const std::wstring& name)
        {
            std::lock_guard<std::mutex> guard(g_lock);
            for (const std::wstring& failed : g_failedNames)
                if (_wcsicmp(failed.c_str(), name.c_str()) == 0)
                    return true;
            return false;
        }

        void Log(const std::string& line)
        {
            std::lock_guard<std::mutex> guard(g_lock);
            if (g_log.size() < 512)
                g_log.push_back(line);
        }

        std::wstring FileName(const std::wstring& path)
        {
            const std::size_t slash = path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? path : path.substr(slash + 1);
        }

        std::string Narrow(const std::wstring& text)
        {
            std::string out;
            for (const wchar_t c : text)
                out += c < 128 ? static_cast<char>(c) : '?';
            return out;
        }

        bool IsOpenClip(const std::wstring& name)
        {
            std::lock_guard<std::mutex> guard(g_lock);
            return !g_openClip.empty() && _wcsicmp(g_openClip.c_str(), name.c_str()) == 0;
        }

        // Waits while a save or a load is in progress; false on unload.
        bool WaitWhileHeld()
        {
            bool held = false;
            while (g_hold.load(std::memory_order_acquire) && g_running.load(std::memory_order_acquire))
            {
                held = true;
                g_state.store(static_cast<std::uint32_t>(State::Held));
                WaitForSingleObject(g_wake, 100);
            }
            if (held)
                g_state.store(static_cast<std::uint32_t>(State::Running));
            return g_running.load(std::memory_order_acquire);
        }

        struct Candidate
        {
            std::wstring path;
            std::wstring name;
        };

        // `only`: one file name, compressed whatever its age (the gallery's
        // Compress); empty: every clip older than the days.
        void Pass(bool manual, const std::wstring& only = {})
        {
            const ULONGLONG startedAt = GetTickCount64();
            std::wstring folder;
            {
                std::lock_guard<std::mutex> guard(g_lock);
                folder = g_folder;
            }
            const std::uint32_t days = g_days.load();
            const int level = g_level.load();
            CompressOptions options;
            options.format = static_cast<Format>(g_format.load());
            options.level = level;
            options.threads = 1; // in the game: one low priority thread
            options.upgrade = g_upgrade.load() && options.format == Format::Packed;
            const std::uint64_t now = NowTicks();
            std::vector<Candidate> candidates;
            std::uint32_t alreadyCompressed = 0, notClips = 0, recent = 0, open = 0, upgrades = 0, failedBefore = 0;
            WIN32_FIND_DATAW entry{};
            HANDLE find = FindFirstFileW((folder + L"\\*.rpclip").c_str(), &entry);
            if (find != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        continue;
                    const std::wstring name = entry.cFileName;
                    const std::wstring path = folder + L"\\" + name;
                    const auto ticks = [](const FILETIME& time) {
                        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
                    };
                    if (!only.empty() && _wcsicmp(name.c_str(), only.c_str()) != 0)
                        continue;
                    if (only.empty() && !OldEnough(ticks(entry.ftLastWriteTime), ticks(entry.ftLastAccessTime), now, days))
                    {
                        ++recent;
                        continue;
                    }
                    if (IsOpenClip(name))
                    {
                        ++open;
                        continue;
                    }
                    if (only.empty() && FailedBefore(name))
                    {
                        ++failedBefore;
                        continue;
                    }
                    const Kind kind = Peek(path);
                    if ((kind == Kind::Compressed || (kind == Kind::Packed && IsFastPackedFile(path))) && options.upgrade)
                    {
                        ++upgrades; // a zstd clip or a save's fast pack repacked with LZMA
                        candidates.push_back({path, name});
                    }
                    else if (kind == Kind::Compressed || kind == Kind::Packed)
                        ++alreadyCompressed;
                    else if (kind != Kind::Plain)
                        ++notClips;
                    else
                        candidates.push_back({path, name});
                } while (FindNextFileW(find, &entry) && g_running.load(std::memory_order_acquire));
                FindClose(find);
            }
            g_done.store(0);
            g_total.store(static_cast<std::uint32_t>(candidates.size()));
            g_compressed.store(0);
            std::uint32_t compressed = 0, skipped = 0, failed = 0;
            std::uint64_t bytesIn = 0, bytesOut = 0;
            for (const Candidate& candidate : candidates)
            {
                bool finished = false;
                for (int attempt = 0; attempt <= kHeldRetries && !finished; ++attempt)
                {
                    if (!WaitWhileHeld())
                        break;
                    if (IsOpenClip(candidate.name))
                    {
                        ++skipped;
                        finished = true;
                        break;
                    }
                    {
                        std::lock_guard<std::mutex> guard(g_lock);
                        g_fileInWork = candidate.name;
                    }
                    g_cancel.store(false, std::memory_order_release);
                    if (g_hold.load(std::memory_order_acquire))
                    {
                        // A save or a load started between the wait and the
                        // reset of the cancel flag: wait again.
                        {
                            std::lock_guard<std::mutex> guard(g_lock);
                            g_fileInWork.clear();
                        }
                        --attempt;
                        continue;
                    }
                    const FileResult result = CompressFile(candidate.path, options, &g_cancel);
                    {
                        std::lock_guard<std::mutex> guard(g_lock);
                        g_fileInWork.clear();
                    }
                    // Cancelled by a save, a load or the editor opening it:
                    // tried again once that is over (not when unloading).
                    if (!result.ok && result.reason == "cancelled" && g_running.load(std::memory_order_acquire))
                    {
                        Log("CLIP_COMPRESS,file=" + Narrow(candidate.name) + ",cancelled=1,retry=" +
                            std::to_string(attempt < kHeldRetries ? 1 : 0));
                        continue;
                    }
                    finished = true;
                    const double ratio = result.bytesOut ? static_cast<double>(result.bytesIn) / result.bytesOut : 0.0;
                    char ratioText[16];
                    std::snprintf(ratioText, sizeof(ratioText), "%.2f", ratio);
                    Log("CLIP_COMPRESS,file=" + Narrow(candidate.name) + ",bytes_in=" + std::to_string(result.bytesIn) +
                        ",bytes_out=" + std::to_string(result.bytesOut) + ",ratio=" + ratioText + ",ms=" +
                        std::to_string(result.ms) + ",ok=" + (result.ok ? "1" : "0") +
                        ",format=" + (options.format == Format::Packed ? "packed" : "zstd") +
                        (result.upgraded ? ",upgraded=1" : "") + (result.fastPacked ? ",fast_packed=1" : "") + (result.plainPack ? ",plain_pack=1" : "") +
                        (result.reason.empty() ? "" : ",reason=" + result.reason));
                    if (result.ok)
                    {
                        ++compressed;
                        bytesIn += result.bytesIn;
                        bytesOut += result.bytesOut;
                        g_compressed.fetch_add(1);
                        g_changes.fetch_add(1);
                    }
                    else if (result.skipped)
                    {
                        ++skipped;
                        // A repack that did not get smaller stays as it is: not
                        // tried again in every pass of this session.
                        if (result.reason == "no_gain" && result.upgraded)
                        {
                            std::lock_guard<std::mutex> guard(g_lock);
                            if (g_failedNames.size() < 4096)
                                g_failedNames.push_back(candidate.name);
                        }
                    }
                    else
                    {
                        ++failed;
                        std::lock_guard<std::mutex> guard(g_lock);
                        if (g_failedNames.size() < 4096)
                            g_failedNames.push_back(candidate.name);
                    }
                }
                if (!g_running.load(std::memory_order_acquire))
                    break;
                g_done.fetch_add(1);
            }
            char ratioText[16];
            std::snprintf(ratioText, sizeof(ratioText), "%.2f", bytesOut ? static_cast<double>(bytesIn) / bytesOut : 0.0);
            Log(std::string("CLIP_COMPRESS_PASS,why=") + (!only.empty() ? "file" : manual ? "manual" : "auto") + ",days=" + std::to_string(days) +
                ",level=" + std::to_string(level) + ",format=" + (options.format == Format::Packed ? "packed" : "zstd") +
                ",candidates=" + std::to_string(candidates.size()) + ",upgrades=" + std::to_string(upgrades) + ",failed_before=" + std::to_string(failedBefore) +
                ",compressed=" + std::to_string(compressed) + ",skipped=" + std::to_string(skipped) + ",failed=" +
                std::to_string(failed) + ",already_compressed=" + std::to_string(alreadyCompressed) + ",recent=" +
                std::to_string(recent) + ",open=" + std::to_string(open) + ",not_clips=" + std::to_string(notClips) +
                ",bytes_in=" + std::to_string(bytesIn) + ",bytes_out=" + std::to_string(bytesOut) + ",ratio=" + ratioText +
                ",ms=" + std::to_string(GetTickCount64() - startedAt) +
                ",stopped=" + (g_running.load(std::memory_order_acquire) ? "0" : "1"));
        }

        // An exception out of a pass (a bad_alloc on a large clip) would
        // leave the thread entry unhandled and close the game; the pass is
        // reported and the worker goes on.
        void GuardedPass(bool manual, const std::wstring& only = {})
        {
            try
            {
                Pass(manual, only);
                return;
            }
            catch (const std::exception& exception)
            {
                try
                {
                    Log(std::string("CLIP_COMPRESS_ERROR,exception=") + exception.what());
                }
                catch (...)
                {
                }
            }
            catch (...)
            {
                try
                {
                    Log("CLIP_COMPRESS_ERROR,exception=unknown");
                }
                catch (...)
                {
                }
            }
            std::lock_guard<std::mutex> guard(g_lock);
            g_fileInWork.clear(); // the file the pass was on is free again
        }

        void Run()
        {
            ULONGLONG nextAuto = GetTickCount64() + g_firstDelayMs;
            while (g_running.load(std::memory_order_acquire))
            {
                const ULONGLONG now = GetTickCount64();
                const DWORD wait = g_automatic.load() && nextAuto > now
                    ? static_cast<DWORD>(std::min<ULONGLONG>(nextAuto - now, kRescanMs))
                    : (g_automatic.load() ? 0 : kRescanMs);
                if (!g_manualRequested.load() && !g_fileRequested.load())
                    WaitForSingleObject(g_wake, wait);
                if (!g_running.load(std::memory_order_acquire))
                    break;
                if (g_fileRequested.exchange(false))
                {
                    std::wstring only;
                    {
                        std::lock_guard<std::mutex> guard(g_lock);
                        only.swap(g_onlyFile);
                    }
                    if (!only.empty())
                    {
                        g_state.store(static_cast<std::uint32_t>(State::Running));
                        GuardedPass(true, only);
                        g_state.store(static_cast<std::uint32_t>(State::Idle));
                    }
                }
                const bool manual = g_manualRequested.exchange(false);
                const bool automatic = g_automatic.load() && GetTickCount64() >= nextAuto;
                if (!manual && !automatic)
                    continue;
                g_state.store(static_cast<std::uint32_t>(State::Running));
                GuardedPass(manual);
                g_state.store(static_cast<std::uint32_t>(State::Idle));
                nextAuto = GetTickCount64() + kRescanMs;
            }
        }

        DWORD WINAPI Entry(void*)
        {
            // Below the game's threads: background mode lowers the thread's
            // CPU, disk and memory priority, then the lowest CPU priority.
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
            replay::threads::Name("clip_compress");
            Run();
            replay::threads::Forget();
            g_state.store(static_cast<std::uint32_t>(State::Off));
            g_exited.store(true, std::memory_order_release);
            return 0;
        }
    }

    void Configure(const Settings& settings)
    {
        g_automatic.store(settings.automatic);
        g_days.store(std::clamp<std::uint32_t>(settings.days, 1u, 60u));
        g_level.store(std::clamp(settings.level, 1, 22));
        g_format.store(settings.format == Format::Zstd ? static_cast<int>(Format::Zstd) : static_cast<int>(Format::Packed));
        g_upgrade.store(settings.upgrade);
        if (g_wake)
            SetEvent(g_wake);
    }

    void Start(const std::wstring& folder, const Settings& settings, std::uint32_t delayMs)
    {
        Configure(settings);
        if (g_thread)
            return;
        wchar_t full[MAX_PATH]{};
        const DWORD length = GetFullPathNameW(folder.c_str(), MAX_PATH, full, nullptr);
        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_folder = length > 0 && length < MAX_PATH ? std::wstring(full) : folder;
        }
        g_firstDelayMs = delayMs;
        g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_running.store(true, std::memory_order_release);
        g_exited.store(false, std::memory_order_release);
        g_state.store(static_cast<std::uint32_t>(State::Idle));
        g_thread = CreateThread(nullptr, 0, &Entry, nullptr, 0, nullptr);
        if (!g_thread)
        {
            g_running.store(false);
            g_state.store(static_cast<std::uint32_t>(State::Off));
            Log("CLIP_COMPRESS_THREAD,ok=0,error=" + std::to_string(GetLastError()));
        }
    }

    void RequestFile(const std::wstring& name)
    {
        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_onlyFile = name;
        }
        g_fileRequested.store(true);
        if (g_wake)
            SetEvent(g_wake);
    }

    void RequestPass()
    {
        g_manualRequested.store(true);
        if (g_wake)
            SetEvent(g_wake);
    }

    void SetHold(bool hold)
    {
        const bool was = g_hold.exchange(hold, std::memory_order_acq_rel);
        if (hold && !was)
            g_cancel.store(true, std::memory_order_release);
    }

    void SetOpenClip(const std::wstring& path)
    {
        const std::wstring name = FileName(path);
        std::lock_guard<std::mutex> guard(g_lock);
        g_openClip = name;
        if (!name.empty() && _wcsicmp(g_fileInWork.c_str(), name.c_str()) == 0)
            g_cancel.store(true, std::memory_order_release);
    }

    std::wstring FileInWork()
    {
        std::lock_guard<std::mutex> guard(g_lock);
        return g_fileInWork;
    }

    Status CurrentStatus()
    {
        Status status;
        status.state = static_cast<State>(g_state.load());
        status.done = g_done.load();
        status.total = g_total.load();
        status.compressed = g_compressed.load();
        return status;
    }

    std::uint64_t ChangeSequence() { return g_changes.load(); }

    void DrainLog(std::vector<std::string>& lines)
    {
        std::lock_guard<std::mutex> guard(g_lock);
        lines.insert(lines.end(), g_log.begin(), g_log.end());
        g_log.clear();
    }

    void StopForUnload()
    {
        if (!g_thread)
            return;
        g_running.store(false, std::memory_order_release);
        g_cancel.store(true, std::memory_order_release);
        if (g_wake)
            SetEvent(g_wake);
        // Out of this image, not the flag and a Sleep (module_lifetime.h);
        // the flag is Entry's last statement.
        const bool left = replay::lifetime::StopWait(g_thread, g_exited, 5000, "clip_compress");
        CloseHandle(g_thread);
        g_thread = nullptr;
        if (left && g_wake)
        {
            CloseHandle(g_wake);
            g_wake = nullptr;
        }
    }
}
