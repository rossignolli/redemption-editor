#pragma once

// The pure rules of the capture's script-thread side (pose_capture.cpp),
// in a header the core tests compile too (tests/pose_capture_rules_tests.cpp):
// when an end-of-frame check is settled without a copy, when the script
// copies a ped the finalizer did not visit, and when a record's next
// end-of-frame check runs.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace replay::pose::capture
{
    // A capture track's own byte budget (pose_stream.h StreamConfig::byteBudget,
    // past which a track drops its oldest chunks). The recorder's budgets
    // already count every track's bytes (script.cpp RecordingBytes: the
    // buffer's ring, ReplayBufferMaxMB up to 4096 MB, moves its head with
    // DropBefore; a manual part closes at ManualMaxMB, at most 960 MB), so a
    // track never trims itself inside them: the default 32 MiB cut the start
    // of the heaviest record of a scene, the player's (QA 224: 68 s of Arthur
    // lost, player_hold.h). Kept as a bound only, at the ring's largest budget.
    constexpr std::size_t kCaptureTrackByteBudget = std::size_t{4096} * 1024 * 1024;

    // End-of-frame check pacing (PollAndCheckLate): once a second per record,
    // four times a second while it differs or is sampled at the end of the
    // frame.
    constexpr std::uint64_t kLateCheckMs = 1000;
    constexpr std::uint64_t kLateCheckChangedMs = 250;
    // One end-of-frame check in this many is also run through the full
    // CompareLatePose (its result is the one used), so the log shows in the
    // game that the memcmp path agrees.
    constexpr std::uint64_t kLateVerifyEvery = 16;

    // Whether the check that brings a record's check count to `checks` is
    // one of those.
    constexpr bool IsLateVerifyCheck(std::uint64_t checks)
    {
        return checks % kLateVerifyEvery == 1;
    }

    // With the shortcuts on (PoseCaptureShortcuts), a check that is not going
    // to be a verification check (`checksBefore`: the record's count before
    // it) first compares the end-of-frame buffer with the detour's copy in
    // place. Equal bytes settle it without a copy: the zero change with equal
    // blocks, what CompareLatePoseChanged returns for a copy of those bytes.
    // Different bytes, and every verification check, go through the copy and
    // the comparison.
    constexpr bool LateDirectCompare(bool shortcuts, std::uint64_t checksBefore)
    {
        return shortcuts && !IsLateVerifyCheck(checksBefore + 1);
    }

    // The direct comparison: locals then objects (2 x bones BoneQts) as one
    // block, equal exactly when both of CompareLatePoseChanged's block
    // memcmp are. pose_capture.cpp runs it under SEH on the engine buffer.
    inline bool LateBlocksEqual(const void* engine, const void* copy, std::size_t bytes)
    {
        return std::memcmp(engine, copy, bytes) == 0;
    }

    // PoseStallPollMs: a ped the finalizer did not visit since the previous
    // script tick is copied by the script only when the finalizer never
    // visited it (`calls` 0, or no visit time yet) or its last visit is at
    // least `stallTicks` old: a corpse, a ragdoll, a very slow NPC. A visit
    // time after `nowQpc` (taken on another core just after ours) is recent.
    // The key's default, 250 ms: at 0 the script
    // copied an NPC the engine animates on fewer frames than the game runs (off
    // the game camera, 25 to 47 finalizer calls per second at 60 fps) on every
    // frame the finalizer skipped, when the buffer still holds the previous
    // pose, through the detour's shared rate gate: the track repeated the pose
    // while the body moved (A/B at Valentine, frozen walking pairs off camera:
    // 5.7% pooled at 0, 0.3% at 250).
    constexpr std::uint32_t kDefaultStallPollMs = 250;
    constexpr bool StallPollDue(std::uint64_t calls, std::uint64_t lastCallQpc, std::uint64_t stallTicks,
        std::uint64_t nowQpc)
    {
        return calls == 0 || lastCallQpc == 0 || (lastCallQpc <= nowQpc && nowQpc - lastCallQpc >= stallTicks);
    }

    // PoseLateBackoff=1: the longest wait between two clean checks.
    constexpr std::uint64_t kLateBackoffMaxMs = 8000;

    // The wait before a record's next end-of-frame check, after one that
    // ran: `changed`, it found a change; `lateCapture`, the record is
    // sampled at the end of the frame after it. Without the backoff:
    // kLateCheckChangedMs while either holds, else kLateCheckMs (`cleanWaitMs`
    // untouched). With it, the same 250 ms while either holds (a change also
    // starts the clean waits over), and a clean check waits `cleanWaitMs`,
    // which then doubles up to 8 s: 1, 2, 4, 8, 8 s. A record leaves
    // end-of-frame capture on a clean check after the change that put it
    // there, so its waits start again at 1 s.
    inline std::uint64_t NextLateCheckMs(bool backoff, bool changed, bool lateCapture, std::uint64_t& cleanWaitMs)
    {
        const bool fast = changed || lateCapture;
        if (!backoff)
            return fast ? kLateCheckChangedMs : kLateCheckMs;
        if (changed)
            cleanWaitMs = kLateCheckMs;
        if (fast)
            return kLateCheckChangedMs;
        const std::uint64_t wait = cleanWaitMs;
        cleanWaitMs = wait * 2u < kLateBackoffMaxMs ? wait * 2u : kLateBackoffMaxMs;
        return wait;
    }

    // How long after its engine parent table is ready a record's first
    // end-of-frame check waits, 0 to 999 ms from a hash of its id (murmur3's
    // 32-bit finalizer), so the records one scan registered do not all check
    // in the same tick.
    constexpr std::uint32_t LateFirstCheckDelayMs(std::uint32_t recordId)
    {
        std::uint32_t hash = recordId;
        hash ^= hash >> 16;
        hash *= 0x85EBCA6Bu;
        hash ^= hash >> 13;
        hash *= 0xC2B2AE35u;
        hash ^= hash >> 16;
        return hash % 1000u;
    }

    // The worker's held frames. Frames drained before a record's layout is
    // known, or while a re-parent job is queued, wait in the record's
    // `pending` list ("held"). Appending all of them (up to 300, 30 to
    // 100 us each) in the one g_mutex hold that applies a layout stalls the
    // script thread (up to ~2 ms in a busy town), so a hold appends at most
    // this many, and the worker appends the rest a few per visit to the
    // record, before the record's new frames.
    constexpr std::size_t kHeldAppendPerHold = 4;

    // Appends the first frames of `held` in order through `append` (false:
    // the frame contradicts the layout; it and the rest stay held, the job
    // for it is queued by the caller's append), at most `most` of them (0:
    // no limit), and erases the appended ones. Returns how many.
    template <typename Frames, typename Append>
    std::size_t AppendHeldPrefix(Frames& held, std::size_t most, Append&& append)
    {
        std::size_t appended = 0;
        for (; appended < held.size() && (most == 0 || appended < most); ++appended)
        {
            if (!append(held[appended]))
                break;
        }
        held.erase(held.begin(), held.begin() + static_cast<std::ptrdiff_t>(appended));
        return appended;
    }

    // Whether a record's held frames can be appended now: its layout is
    // known, and no re-parent job is queued or running (a job changes the
    // layout, and the held frames go in after it, on its layout, as the job
    // appended them).
    constexpr bool HeldFramesAppendable(bool layoutReady, bool reparentQueued, bool reparentRunning, bool held)
    {
        return layoutReady && held && !reparentQueued && !reparentRunning;
    }

    // Whether a frame just drained for a record whose layout is known goes
    // straight to the track: no re-parent job queued and no frame held,
    // which it would overtake. Otherwise it is held behind them.
    constexpr bool DrainedFrameDirect(bool reparentQueued, bool held)
    {
        return !reparentQueued && !held;
    }

    // A record the worker has nothing left to do for. It left g_armed (retired)
    // and its ring went back to the pool, so no frame can come any more; no
    // frame is held; no discovery or re-parent job is queued or running, so
    // nothing can change its layout or append to its track. Each of these stays
    // true once they all hold (frames, held frames and jobs only come from the
    // ring), so the worker takes such a record off its drain list for good and
    // seals its track.
    constexpr bool RecordFinished(bool retired, bool ringReleased, bool held, bool discoveryQueued,
        bool reparentQueued, bool reparentRunning)
    {
        return retired && ringReleased && !held && !discoveryQueued && !reparentQueued && !reparentRunning;
    }

    // The frames a retired record holds before its layout is known, once its
    // ring is released and no discovery job is queued (one failed, or the
    // record left before the first was due). ProcessFrame is the only path that
    // queues a discovery and no frame can arrive any more, so they can never
    // reach a track: the worker drops them and the record can finish.
    constexpr bool HeldFramesStranded(bool retired, bool ringReleased, bool layoutReady, bool discoveryQueued)
    {
        return retired && ringReleased && !layoutReady && !discoveryQueued;
    }

    // A finished record the buffer's DropBefore(timeMs) removes. Its last frame
    // is before timeMs (replay::buffer:: ExportableBaseMs, the earliest base a
    // later window cut can have), so every later cut drops it as "ended before
    // the window" (recording_window.cpp CutPoseRecord), or it has no track
    // (never exported). Not while the re-parent queue still holds an entry for
    // it (the safety valve clears reparentQueued and leaves the entry, which
    // the discovery thread pops later through the pointer).
    constexpr bool RecordPrunable(bool finished, bool queueEntry, bool hasTrack, std::uint32_t lastTimeMs,
        std::uint32_t timeMs)
    {
        return finished && !queueEntry && (!hasTrack || lastTimeMs < timeMs);
    }

    // Removes the elements `prune` selects, keeping the order of the rest (the
    // records' creation order); `removed` sees each one before it goes. Returns
    // how many went.
    template <typename Vector, typename Prune, typename Removed>
    std::size_t StableRemove(Vector& items, Prune&& prune, Removed&& removed)
    {
        std::size_t write = 0;
        const std::size_t count = items.size();
        for (std::size_t read = 0; read < count; ++read)
        {
            if (prune(items[read]))
            {
                removed(items[read]);
                continue;
            }
            if (write != read)
                items[write] = std::move(items[read]);
            ++write;
        }
        const std::size_t gone = count - write;
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(write), items.end());
        return gone;
    }

    // RedemptionEditor.ini PoseWorkerPollMs, the worker's wait between two
    // passes (5 ms unless set; 1 to 100).
    constexpr std::uint32_t kWorkerPollMsDefault = 5;
    constexpr std::uint32_t ClampWorkerPollMs(int ms)
    {
        return ms < 1 ? 1u : ms > 100 ? 100u : static_cast<std::uint32_t>(ms);
    }
}
