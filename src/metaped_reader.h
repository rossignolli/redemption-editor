#pragma once

// The engine side of the metaped look. Script thread only.
//
// Reads (guarded, layout in metaped_state.h): the ped's expression frame
// (every expression the creature has, in one read) and the ped scale. The code of
// every engine function the layout comes from is compared byte for byte
// once a session (the getter chain 0xB44250 -> 0x2BDDE1C, 0xB32AD8,
// 0x2BC96C4 -> 0x265FDBC -> 0x26580BC; the scale setter 0xFEF2FC and its
// state test 0x8B70E8); the global addresses are taken from those bytes. A
// mismatch turns the reader off (METAPED_READER,phase=config,status=off)
// and the capture keeps the natives' part (layers, outfit, type,
// cleanliness). The first reads of the session are cross-checked with
// _GET_CHAR_EXPRESSION; a disagreement turns it off as well.
//
// Writes use natives only: _SET_CHAR_EXPRESSION,
// _SET_PED_SCALE, _SET_PED_DAMAGE_CLEANLINESS, _UPDATE_PED_VARIATION on
// puppets only.

#include "metaped_state.h"

#include <cstdint>
#include <string>
#include <vector>

namespace replay::metaped::engine
{
    // Ini MetapedRecord (the reader's part of the capture: expressions and
    // scale) and MetapedApply (the playback side), both default 1.
    void SetEnabled(bool record, bool apply);

    // Signatures checked once (first call); the log line of the result.
    bool ReaderOn();
    std::string ReaderStatus();

    enum class ReadResult : std::uint8_t
    {
        Ok,
        Off,        // signatures did not match, or a cross-check failed
        NoEntity,   // no engine address for the handle
        NoCreature, // the ped has no creature or expression frame yet
        Layout,     // the DOF table did not have the layout
        Fault,      // a guarded read failed
    };
    const char* ReadResultText(ReadResult result);

    // Every set expression of the ped (ascending id).
    ReadResult ReadExpressions(int ped, std::vector<Expression>& out, std::uint32_t& notSet);
    // The ped scale the setter stores (_SET_PED_SCALE has no getter).
    ReadResult ReadScale(int ped, float& out);

    // Everything the natives and the reader give, without the layers (they
    // stay in Appearance): outfit, metaped type, cleanliness, expressions,
    // scale, the shop items and their wearable states (natives, read also
    // with MetapedRecord=0). `state.timeMs` and `source` are the caller's.
    void Capture(int ped, State& state);

    // The puppet side: the recorded state applied (only what differs from
    // the puppet's own), then _UPDATE_PED_VARIATION when something changed.
    struct ApplyReport
    {
        std::uint32_t expressionsSet = 0;
        bool scaleSet = false;
        bool cleanlinessSet = false;
        bool puppetRead = false; // the puppet's expressions were read first
    };
    ApplyReport Apply(int puppet, const State& state);

    // What the puppet has against the recorded state.
    struct Check
    {
        bool read = false;
        Diff expressions;
        float scale = 0.0f;
        bool scaleRead = false;
        std::int32_t cleanliness = -1;
    };
    Check Verify(int puppet, const State& state);

    // The first reads of a session compared with _GET_CHAR_EXPRESSION (a
    // few ids per ped); called by Capture. Counters for the summary line.
    struct Stats
    {
        std::uint64_t reads = 0;
        std::uint64_t readFailures = 0;
        std::uint64_t crossChecks = 0;
        std::uint64_t crossMismatches = 0;
        std::uint64_t applies = 0;
        std::uint64_t expressionsSet = 0;
    };
    const Stats& Counters();
    void ResetCounters();
}
