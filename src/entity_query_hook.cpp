#include "entity_query.h"
#include "entity_query_track.h"

#include "guarded_memory.h"
#include "hook_batch.h"
#include "mod_clock.h"
#include "module_lifetime.h"
#include "event_ring.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>

// The detour on the ped audio entity's sound query 0x31947D0 (see
// entity_query.h).

namespace replay::entityquery
{
    namespace
    {
        // Prologue (30 bytes, unique in .text): mov [rsp+8],rbx; push rbp,
        // rsi, rdi, r12, r13, r14, r15; mov rbp,rsp; sub rsp,40h; cmp dword
        // [r8],0EB69052Eh (the first head of its switch).
        constexpr std::uint8_t kResolverSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
            0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x40, 0x41, 0x81, 0x38, 0x2E, 0x05, 0x69, 0xEB};

        using ResolverFn = std::uint64_t*(__fastcall*)(void*, std::uint64_t*, const std::uint32_t*, std::uint32_t, void*);

        constexpr std::uint32_t kRing = 2048;

        using Slot = replay::ring::Slot<Query>; // event_ring.h

        std::atomic<ResolverFn> g_original{nullptr};
        std::atomic<std::int32_t> g_inFlight{0};
        void* g_target = nullptr;
        bool g_installed = false;
        std::uintptr_t g_base = 0;
        std::atomic<bool> g_logging{false};
        std::atomic<bool> g_relation{true};
        std::atomic<bool> g_logWanted{true};
        std::atomic<std::uint32_t> g_scriptThread{0};
        Slot g_ring[kRing];
        std::atomic<std::uint64_t> g_next{0};
        std::uint64_t g_drained = 0; // script thread only

        // The pairs: written by the script thread, read by the detour. A
        // query racing an update may miss or keep a pair for one call.
        std::atomic<std::uintptr_t> g_riders[kMaxPairs] = {};
        std::atomic<std::uintptr_t> g_mounts[kMaxPairs] = {};
        std::atomic<std::uint32_t> g_pairEnd{0};

        std::atomic<std::uint64_t> g_calls{0};
        std::atomic<std::uint64_t> g_outermost{0};
        std::atomic<std::uint64_t> g_mountHead{0};
        std::atomic<std::uint64_t> g_forwarded{0};
        std::atomic<std::uint64_t> g_forwardedNull{0};
        std::atomic<std::uint64_t> g_engineNull{0};
        std::atomic<std::uint64_t> g_recorded{0};
        std::atomic<std::uint64_t> g_recordedNull{0};
        std::atomic<bool> g_replayAnswers{true};
        std::atomic<bool> g_recordAnswers{true};

        // The recorded answers: two buffers, the script thread fills the one
        // not in use and flips; each has a sequence (odd while written) so a
        // query that raced two flips sees the change and asks the engine.
        struct AnswerBuffer
        {
            std::atomic<std::uint32_t> sequence{0};
            std::atomic<std::uint32_t> count{0};
            std::atomic<std::uintptr_t> holder[kMaxRecordedAnswers] = {};
            std::atomic<std::uint32_t> hash[kMaxRecordedAnswers] = {};
            std::atomic<std::uint32_t> n[kMaxRecordedAnswers] = {};
            std::atomic<std::uint64_t> answer[kMaxRecordedAnswers] = {};
        };
        AnswerBuffer g_answers[2];
        std::atomic<std::uint32_t> g_activeAnswers{0};

        bool RecordedAnswerOf(std::uintptr_t holder, std::uint32_t hash, std::uint32_t n, std::uint64_t& answer)
        {
            const std::uint32_t active = g_activeAnswers.load(std::memory_order_acquire) & 1u;
            const AnswerBuffer& buffer = g_answers[active];
            const std::uint32_t before = buffer.sequence.load(std::memory_order_acquire);
            if (before & 1u)
                return false;
            const std::uint32_t count = buffer.count.load(std::memory_order_relaxed);
            bool found = false;
            for (std::uint32_t index = 0; index < count && index < kMaxRecordedAnswers; ++index)
            {
                if (buffer.holder[index].load(std::memory_order_relaxed) != holder ||
                    buffer.hash[index].load(std::memory_order_relaxed) != hash ||
                    buffer.n[index].load(std::memory_order_relaxed) != n)
                    continue;
                answer = buffer.answer[index].load(std::memory_order_relaxed);
                found = true;
                break;
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            return found && buffer.sequence.load(std::memory_order_relaxed) == before;
        }

        // Guarded, with the default floor: `out` is on the caller's stack,
        // which can sit below 4 GB.
        bool Read(std::uintptr_t address, void* out, std::size_t size)
        {
            return replay::mem::Copy(out, reinterpret_cast<const void*>(address), size);
        }

        std::uintptr_t PublishedMount(std::uintptr_t holder)
        {
            const std::uint32_t end = g_pairEnd.load(std::memory_order_acquire);
            for (std::uint32_t index = 0; index < end && index < kMaxPairs; ++index)
                if (g_riders[index].load(std::memory_order_relaxed) == holder)
                    return g_mounts[index].load(std::memory_order_relaxed);
            return 0;
        }

        bool IsPedAudioHolder(std::uintptr_t holder)
        {
            std::uintptr_t vtable = 0;
            if (!holder || !Read(holder, &vtable, sizeof(vtable)))
                return false;
            for (const std::uintptr_t rva : kHolderVtableRvas)
                if (vtable == g_base + rva)
                    return true;
            return false;
        }

        std::uint32_t NullNameNow()
        {
            std::uint32_t name = 0;
            Read(g_base + kNullNameRva, &name, sizeof(name));
            return name;
        }

        void Push(const Query& query)
        {
            replay::ring::Writer<Query> slot(g_ring, kRing, g_next);
            slot.value = query;
            slot.Publish();
        }

        std::uint64_t* __fastcall ResolverDetour(void* holder, std::uint64_t* out, const std::uint32_t* refs,
            std::uint32_t n, void* top)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            struct InFlight
            {
                InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            const ResolverFn original = g_original.load(std::memory_order_acquire);
            g_calls.fetch_add(1, std::memory_order_relaxed);
            const std::uintptr_t holderAddress = reinterpret_cast<std::uintptr_t>(holder);
            std::uintptr_t topEntity = 0;
            const bool outermost = top && Read(reinterpret_cast<std::uintptr_t>(top) + kTopEntityOffset, &topEntity,
                sizeof(topEntity)) && topEntity == holderAddress;
            const bool logging = g_logging.load(std::memory_order_relaxed);
            std::uint32_t head = 0;
            const bool headRead = refs && n >= 1 && Read(reinterpret_cast<std::uintptr_t>(refs), &head, sizeof(head));
            Query query;
            std::uintptr_t mountHolder = 0;
            std::uintptr_t engineMount = 0;
            const bool mountChain = headRead && head == kMountHead;
            if (mountChain)
            {
                std::uintptr_t ped = 0;
                if (Read(holderAddress + kHolderPedOffset, &ped, sizeof(ped)))
                    engineMount = EngineMountOf(g_base, ped, Read);
                mountHolder = PublishedMount(holderAddress);
            }
            std::uint64_t* result = nullptr;
            bool forwarded = false;
            bool recorded = false;
            // The recording's answer for this owner and chain first (an
            // outermost query only: the owner is the top-level sound's).
            std::uint32_t chainNow[kMaxChain] = {};
            const std::uint32_t chainCopy = n < kMaxChain ? n : kMaxChain;
            const bool chainRead = refs && n >= 1 && n <= kMaxChain &&
                Read(reinterpret_cast<std::uintptr_t>(refs), chainNow, chainCopy * sizeof(std::uint32_t));
            if (outermost && out && chainRead && g_replayAnswers.load(std::memory_order_relaxed) &&
                g_answers[g_activeAnswers.load(std::memory_order_acquire) & 1u].count.load(std::memory_order_relaxed))
            {
                std::uint64_t answer = 0;
                if (RecordedAnswerOf(holderAddress, ChainHash(n, chainNow), n, answer))
                {
                    *out = answer;
                    result = out;
                    recorded = true;
                }
            }
            if (!recorded && mountChain && original && g_relation.load(std::memory_order_relaxed) &&
                ForwardsToMount(head, n, engineMount, mountHolder, IsPedAudioHolder(mountHolder)))
            {
                // The engine's own mount branch (0x3194DD2..0x3194E42): the
                // rest of the chain to the mount's audio entity, same out and
                // top-level sound. Its slot +0x68 is this function in all three
                // vtables, so the trampoline is the same call.
                result = original(reinterpret_cast<void*>(mountHolder), out, refs + 1, n - 1, top);
                forwarded = true;
            }
            else if (!recorded && original)
            {
                result = original(holder, out, refs, n, top);
            }
            if (!outermost && !forwarded && !recorded)
                return result;
            g_outermost.fetch_add(outermost ? 1 : 0, std::memory_order_relaxed);
            std::uint64_t answer = 0;
            if (out)
                Read(reinterpret_cast<std::uintptr_t>(out), &answer, sizeof(answer));
            const bool nullAnswer = NullAnswer(answer, NullNameNow());
            if (mountChain)
                g_mountHead.fetch_add(1, std::memory_order_relaxed);
            if (recorded)
            {
                g_recorded.fetch_add(1, std::memory_order_relaxed);
                if (nullAnswer)
                    g_recordedNull.fetch_add(1, std::memory_order_relaxed);
            }
            else if (forwarded)
            {
                g_forwarded.fetch_add(1, std::memory_order_relaxed);
                if (nullAnswer)
                    g_forwardedNull.fetch_add(1, std::memory_order_relaxed);
            }
            else if (nullAnswer)
            {
                g_engineNull.fetch_add(1, std::memory_order_relaxed);
            }
            if (!logging)
                return result;
            query.timeUs = replay::clock::NowUs();
            query.thread = GetCurrentThreadId();
            query.holder = holderAddress;
            query.top = reinterpret_cast<std::uintptr_t>(top);
            query.n = n;
            if (chainRead)
                std::copy(chainNow, chainNow + chainCopy, query.chain);
            else if (refs && chainCopy && !Read(reinterpret_cast<std::uintptr_t>(refs), query.chain, chainCopy * sizeof(std::uint32_t)))
                query.chain[0] = head;
            query.answer = answer;
            query.engineMount = engineMount;
            query.mountHolder = mountHolder;
            query.source = recorded ? Source::Recorded : forwarded ? Source::Relation : Source::Engine;
            query.outermost = outermost;
            query.nullAnswer = nullAnswer;
            Push(query);
            return result;
        }
    }

    bool Install(std::string& error)
    {
        if (g_installed)
            return true;
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        g_base = reinterpret_cast<std::uintptr_t>(base);
        void* target = base + kResolverRva;
        if (!replay::mem::Matches(target, kResolverSignature, sizeof(kResolverSignature)))
        {
            error = "entity_query_signature_mismatch";
            return false;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&ResolverDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            return false;
        }
        g_original.store(reinterpret_cast<ResolverFn>(trampoline), std::memory_order_release);
        status = replay::hooks::Enable(target);
        if (status != MH_OK)
        {
            MH_RemoveHook(target);
            g_original.store(nullptr, std::memory_order_release);
            error = std::string("mh_enable_") + MH_StatusToString(status);
            return false;
        }
        g_target = target;
        g_installed = true;
        return true;
    }

    void Shutdown()
    {
        g_pairEnd.store(0, std::memory_order_release);
        SetRecordedAnswers({});
        g_logging.store(false, std::memory_order_release);
        if (g_installed && g_target)
        {
            MH_DisableHook(g_target);
            MH_RemoveHook(g_target);
            g_installed = false;
            g_target = nullptr;
        }
        for (int wait = 0; wait < 200 && g_inFlight.load(std::memory_order_seq_cst) > 0; ++wait)
            Sleep(1);
        g_original.store(nullptr, std::memory_order_release);
    }

    bool Installed() { return g_installed; }
    void SetLogging(bool on) { g_logging.store(on, std::memory_order_release); }
    void SetRelation(bool on) { g_relation.store(on, std::memory_order_release); }
    bool RelationOn() { return g_relation.load(std::memory_order_relaxed); }
    void SetLogWanted(bool on) { g_logWanted.store(on, std::memory_order_relaxed); }
    bool LogWanted() { return g_logWanted.load(std::memory_order_relaxed); }
    void SetReplayAnswers(bool on) { g_replayAnswers.store(on, std::memory_order_relaxed); }
    bool ReplayAnswersOn() { return g_replayAnswers.load(std::memory_order_relaxed); }
    void SetRecordAnswers(bool on) { g_recordAnswers.store(on, std::memory_order_relaxed); }
    bool RecordAnswersOn() { return g_recordAnswers.load(std::memory_order_relaxed); }

    void SetRecordedAnswers(const std::vector<RecordedAnswer>& answers)
    {
        const std::uint32_t next = (g_activeAnswers.load(std::memory_order_relaxed) & 1u) ^ 1u;
        AnswerBuffer& buffer = g_answers[next];
        buffer.sequence.fetch_add(1, std::memory_order_acq_rel); // odd: being written
        std::atomic_thread_fence(std::memory_order_release);
        std::uint32_t count = 0;
        for (const RecordedAnswer& a : answers)
        {
            if (count >= kMaxRecordedAnswers)
                break;
            if (!a.holder)
                continue;
            buffer.holder[count].store(a.holder, std::memory_order_relaxed);
            buffer.hash[count].store(a.hash, std::memory_order_relaxed);
            buffer.n[count].store(a.n, std::memory_order_relaxed);
            buffer.answer[count].store(a.answer, std::memory_order_relaxed);
            ++count;
        }
        buffer.count.store(count, std::memory_order_relaxed);
        buffer.sequence.fetch_add(1, std::memory_order_release); // even: done
        g_activeAnswers.store(next, std::memory_order_release);
    }
    std::uint32_t NullName() { return g_base ? NullNameNow() : 0; }
    std::uint32_t ScriptThread() { return g_scriptThread.load(std::memory_order_relaxed); }
    void SetScriptThread(std::uint32_t thread) { g_scriptThread.store(thread, std::memory_order_relaxed); }

    void SetPairs(const std::vector<Pair>& pairs)
    {
        const std::uint32_t oldEnd = g_pairEnd.load(std::memory_order_relaxed);
        std::uint32_t count = 0;
        for (const Pair& pair : pairs)
        {
            if (count >= kMaxPairs)
                break;
            if (!pair.riderHolder || !pair.mountHolder)
                continue;
            g_riders[count].store(pair.riderHolder, std::memory_order_relaxed);
            g_mounts[count].store(pair.mountHolder, std::memory_order_relaxed);
            ++count;
        }
        for (std::uint32_t index = count; index < oldEnd && index < kMaxPairs; ++index)
        {
            g_riders[index].store(0, std::memory_order_relaxed);
            g_mounts[index].store(0, std::memory_order_relaxed);
        }
        g_pairEnd.store(count, std::memory_order_release);
    }

    void Drain(std::vector<Query>& out, std::uint64_t& dropped)
    {
        out.clear();
        // A query being written stops the drain: the next drain takes it.
        const replay::ring::Drained drained =
            replay::ring::ForEachSince(g_ring, kRing, g_next, g_drained, [&](const Query& value) { out.push_back(value); });
        dropped = drained.overflowed + drained.lost;
    }

    Stats Snapshot()
    {
        Stats stats;
        stats.calls = g_calls.load(std::memory_order_relaxed);
        stats.outermost = g_outermost.load(std::memory_order_relaxed);
        stats.mountHead = g_mountHead.load(std::memory_order_relaxed);
        stats.forwarded = g_forwarded.load(std::memory_order_relaxed);
        stats.forwardedNull = g_forwardedNull.load(std::memory_order_relaxed);
        stats.engineNull = g_engineNull.load(std::memory_order_relaxed);
        stats.recorded = g_recorded.load(std::memory_order_relaxed);
        stats.recordedNull = g_recordedNull.load(std::memory_order_relaxed);
        return stats;
    }

    void ResetStats()
    {
        g_calls.store(0, std::memory_order_relaxed);
        g_outermost.store(0, std::memory_order_relaxed);
        g_mountHead.store(0, std::memory_order_relaxed);
        g_forwarded.store(0, std::memory_order_relaxed);
        g_forwardedNull.store(0, std::memory_order_relaxed);
        g_engineNull.store(0, std::memory_order_relaxed);
        g_recorded.store(0, std::memory_order_relaxed);
        g_recordedNull.store(0, std::memory_order_relaxed);
    }
}
