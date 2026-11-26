#pragma once

#include "record_gate.h"

#include <cstdint>
#include <iosfwd>
#include <vector>

namespace replay::cutsceneaudio
{
    // Cutscene master tracks are streamed from the installed RDR2 audio
    // archives. The editor includes only the scene, soundset, track hash, and
    // duration metadata; it does not include extracted audio files.
    void Update(const std::vector<gate::Span>& spans, std::uint32_t playerModel,
        std::uint64_t playheadMs, bool playing, bool paused, bool scrubbing,
        double rate, std::ostream& log);
    void Stop(std::ostream* log = nullptr);
}
