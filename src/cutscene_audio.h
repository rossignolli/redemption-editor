#pragma once

#include "record_gate.h"

#include <cstdint>
#include <iosfwd>
#include <vector>

namespace replay::cutsceneaudio
{
    // Loose files are read under CutsceneAudio/<mission>/<cutscene>/ first,
    // then CutsceneAudio/<cutscene>/ for audio shared by mission variants.
    // Arthur.wav or John.wav, when present, takes priority based on the clip's
    // recorded player model; main.wav is the fallback full-voice master.
    // Echo streams are intentionally omitted from replay playback.
    void Update(const std::vector<gate::Span>& spans, std::uint32_t playerModel,
        std::uint64_t playheadMs, bool playing, bool paused, bool scrubbing,
        double rate, std::ostream& log);
    void Stop(std::ostream* log = nullptr);
}
