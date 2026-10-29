#pragma once

// Names for the threads this module creates, so a crash line's `thread=` and a
// minidump say whose thread it was (the game's, the driver's, the encoder's or
// one of ours):
// - every thread the mod or the editor core creates calls Scope (or Name)
//   first: SetThreadDescription (shown by debuggers, WinDbg ~, the dump's
//   thread names stream and GetThreadDescription in the crash handler) and
//   an entry in this module's table;
// - threads the module does not own (the game's main thread that runs the
//   script fiber, the present thread of the add-on) are only noted in the
//   table (NoteForeign): their own description is the game's, never changed;
// - the crash handler reads the table without a lock, an allocation or a
//   system call (Lookup), and the load logs the map (MapText).
//
// Header only: the ASI and the editor core each get their own table.
// SetThreadDescription is looked up at run time (Windows 10 1607+); older
// systems just get the table.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace replay::threads
{
    constexpr int kMaxEntries = 64;
    constexpr int kNameCapacity = 32;

    struct Entry
    {
        volatile LONG threadId = 0; // 0: free
        char name[kNameCapacity] = {};
        volatile LONG own = 0;      // 1: created by this module (its description was set)
    };

    inline Entry g_entries[kMaxEntries];

    // "pose_worker" -> L"RedemptionEditor pose_worker" (the description other
    // tools show, prefixed so the mod's threads stand out among the game's).
    inline void SetDescription(const char* name) noexcept
    {
        using SetFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        static SetFn set = reinterpret_cast<SetFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
        if (!set || !name)
            return;
        constexpr wchar_t kPrefix[] = L"RedemptionEditor ";
        wchar_t wide[kNameCapacity + 24] = L"RedemptionEditor ";
        std::size_t at = sizeof(kPrefix) / sizeof(kPrefix[0]) - 1;
        for (const char* c = name; *c && at + 1 < sizeof(wide) / sizeof(wide[0]); ++c)
            wide[at++] = static_cast<wchar_t>(static_cast<unsigned char>(*c));
        wide[at] = 0;
        set(GetCurrentThread(), wide);
    }

    inline void Store(DWORD threadId, const char* name, bool own) noexcept
    {
        if (!threadId || !name)
            return;
        // The same thread again (a reused id or a second note): its slot.
        for (Entry& entry : g_entries)
        {
            if (static_cast<DWORD>(entry.threadId) == threadId)
            {
                strncpy_s(entry.name, name, _TRUNCATE);
                entry.own = own ? 1 : 0;
                return;
            }
        }
        for (Entry& entry : g_entries)
        {
            if (InterlockedCompareExchange(&entry.threadId, static_cast<LONG>(threadId), 0) == 0)
            {
                strncpy_s(entry.name, name, _TRUNCATE);
                entry.own = own ? 1 : 0;
                return;
            }
        }
        // Full: the description alone (64 named threads alive is not expected).
    }

    // First statement of a thread this module created.
    inline void Name(const char* name) noexcept
    {
        SetDescription(name);
        Store(GetCurrentThreadId(), name, true);
    }

    // A thread the module did not create (the game's): table only.
    inline void NoteForeign(const char* name) noexcept { Store(GetCurrentThreadId(), name, false); }

    // Last statement of a named thread (its id may be reused by another).
    inline void Forget() noexcept
    {
        const DWORD self = GetCurrentThreadId();
        for (Entry& entry : g_entries)
        {
            if (static_cast<DWORD>(entry.threadId) == self)
            {
                entry.name[0] = 0;
                InterlockedExchange(&entry.threadId, 0);
                return;
            }
        }
    }

    // Name on entry, Forget on exit. A worker whose exit flag must be its
    // last statement (module_lifetime.h) closes the scope before the flag.
    struct Scope
    {
        explicit Scope(const char* name) noexcept { Name(name); }
        ~Scope() { Forget(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

    // Crash handler side: no lock, no allocation. "" when unknown.
    inline const char* Lookup(DWORD threadId) noexcept
    {
        if (!threadId)
            return "";
        for (const Entry& entry : g_entries)
            if (static_cast<DWORD>(entry.threadId) == threadId && entry.name[0])
                return entry.name;
        return "";
    }

    // "script=1234,pose_worker=5678" (the log line at load and on demand).
    inline std::string MapText()
    {
        std::string text;
        for (const Entry& entry : g_entries)
        {
            const DWORD id = static_cast<DWORD>(entry.threadId);
            if (!id || !entry.name[0])
                continue;
            char item[kNameCapacity + 24];
            std::snprintf(item, sizeof(item), "%s%s=%lu", text.empty() ? "" : ",", entry.name, static_cast<unsigned long>(id));
            text += item;
        }
        return text.empty() ? std::string("none") : text;
    }
}
