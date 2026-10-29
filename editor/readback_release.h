#pragma once

// The CPU readback buffers of the clip thumbnails (clip_thumbnails.h) and the
// QA shots (qa_shots.h). A worker thread converts the pixels straight from the
// mapped buffer, so the buffer may be unmapped and destroyed only after that
// worker is done with it. Release waits for the workers a bounded time on the
// render thread; a buffer whose worker is still busy after that is left
// mapped and alive (leaked, and logged by the caller) instead of being freed
// under the worker. Pure (unit tested in ReplayCoreTests).

#include <cstdint>

namespace editor_readback
{
    // What Release does with one readback buffer.
    struct Disposal
    {
        bool unmap = false;   // unmap_buffer_region first
        bool destroy = false; // then destroy_resource
        bool leak = false;    // a worker still reads it: neither, the handle is only forgotten
    };

    inline Disposal Dispose(bool hasBuffer, bool mapped, bool workerBusy)
    {
        Disposal out;
        if (!hasBuffer)
            return out;
        if (workerBusy)
        {
            out.leak = true;
            return out;
        }
        out.unmap = mapped;
        out.destroy = true;
        return out;
    }

    // Polls `busy` until it returns false or `limitMs` passed on the `now`
    // clock (milliseconds), calling `pause` between polls. True when idle. The
    // limit is time, not a count of polls, so a coarse Sleep cannot stretch it.
    template <class Busy, class Now, class Pause>
    bool WaitIdle(Busy&& busy, std::uint64_t limitMs, Now&& now, Pause&& pause)
    {
        const std::uint64_t start = now();
        while (busy())
        {
            if (now() - start >= limitMs)
                return false;
            pause();
        }
        return true;
    }
}
