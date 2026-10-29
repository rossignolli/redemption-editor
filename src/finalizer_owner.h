#pragma once

// Who holds the engine pose finalizer hook (RVA 0x266C294, build 1491.50).
//
// The Pose Editor and the ReplayPoC both detour the same function. They are
// installed together but used one at a time: a mod claims this slot before
// installing its detour and releases it after removing the detour. The slot
// is a named shared page per game process, so it works across the two DLLs
// and does not depend on which thread claims or releases it (ScriptHook
// scripts run on fibers; a mutex would be owned by a thread).
//
// This header is the whole protocol; the ReplayPoC carries an identical copy.
// Keep the name, the layout and the owner ids stable.
//
// Protocol v2: the ReplayPoC keeps its detour installed from mod load
// (each install or removal freezes the game for 60 to 160 ms), so it holds
// the slot while idle. The Pose Editor asks for it by writing its id to
// `wanted`; the ReplayPoC, when it is not recording or playing, removes its
// detour and releases the slot, and claims it back once the slot is free and
// nobody wants it. The page name changes with the layout (v1 was 12 bytes).

#include <Windows.h>
#include <cstdio>

namespace rdr2::finalizer_owner
{
    constexpr LONG kFree = 0;
    constexpr LONG kReplayPoC = 1;
    constexpr LONG kPoseEditor = 2;

    constexpr LONG kMagic = 0x4F464452; // "RDFO"

    struct Shared
    {
        volatile LONG magic;
        volatile LONG owner;
        // Claims granted since the page was created, for the logs.
        volatile LONG claims;
        // v2: the id of a mod waiting for the slot, or kFree.
        volatile LONG wanted;
    };

    inline const char* OwnerName(LONG owner)
    {
        switch (owner)
        {
        case kFree: return "free";
        case kReplayPoC: return "RedemptionEditor"; // the display name (the value 1 is the protocol)
        case kPoseEditor: return "PoseEditor";
        default: return "unknown";
        }
    }

    class Slot
    {
    public:
        Slot() = default;
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
        ~Slot() { Close(); }

        bool Open()
        {
            if (shared_)
                return true;
            wchar_t name[96]{};
            swprintf_s(name, L"Local\\RDR2.PoseFinalizerOwner.v2.%lu", GetCurrentProcessId());
            mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                static_cast<DWORD>(sizeof(Shared)), name);
            if (!mapping_)
                return false;
            shared_ = static_cast<Shared*>(
                MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Shared)));
            if (!shared_)
            {
                CloseHandle(mapping_);
                mapping_ = nullptr;
                return false;
            }
            // A fresh page is zeroed; either side may be first.
            InterlockedCompareExchange(&shared_->magic, kMagic, 0);
            return shared_->magic == kMagic;
        }

        void Close()
        {
            if (shared_)
                UnmapViewOfFile(shared_);
            if (mapping_)
                CloseHandle(mapping_);
            shared_ = nullptr;
            mapping_ = nullptr;
        }

        // True when `me` holds the slot after the call (already held counts).
        // `holder` receives the owner seen, for the refusal message.
        bool TryClaim(LONG me, LONG& holder)
        {
            holder = kFree;
            if (!Open())
                return false;
            const LONG previous = InterlockedCompareExchange(&shared_->owner, me, kFree);
            holder = previous == kFree ? me : previous;
            if (previous == kFree)
                InterlockedIncrement(&shared_->claims);
            return previous == kFree || previous == me;
        }

        void Release(LONG me)
        {
            if (shared_)
                InterlockedCompareExchange(&shared_->owner, kFree, me);
        }

        LONG Holder() const { return shared_ ? shared_->owner : kFree; }

        // v2: ask the holder to give the slot back when it can.
        void Request(LONG me)
        {
            if (Open())
                InterlockedExchange(&shared_->wanted, me);
        }

        // v2: withdraw a request (after the claim, or when giving up).
        void Withdraw(LONG me)
        {
            if (shared_)
                InterlockedCompareExchange(&shared_->wanted, kFree, me);
        }

        LONG Wanted() const { return shared_ ? shared_->wanted : kFree; }

    private:
        HANDLE mapping_ = nullptr;
        Shared* shared_ = nullptr;
    };
}
