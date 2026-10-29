#pragma once

// The DRAWSTORE_SLOT evidence line, logged around a crash at RDR2.exe+0x363068
// (drawstore_probe_core.h has the layout and the rules). Script thread only;
// pure guarded reads, nothing written to the engine.

#include <cstddef>
#include <ostream>

namespace replay::drawstore
{
    // One DRAWSTORE_SLOT line for `why` (load, playback_start,
    // reload_push, session_reset). The signatures are checked on the first
    // call; a mismatch logs signature_mismatch once and disables the probe.
    void LogSlot(const char* why, std::ostream& log);

    // The last line written, for the crash handler (any thread, no
    // allocation): copies at most `capacity - 1` bytes, returns the length.
    int CopyLastLine(char* out, int capacity);
}
