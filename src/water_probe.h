#pragma once

// Water height evidence (diagnostic only). Recording the dynamic water
// height grid around the camera and writing it back needs a CPU copy of
// the grid. This probe answers whether RDR2 has one:
//  - WATER_PROBE: the CPU water height natives (TEST_VERTICAL_PROBE_AGAINST_ALL_WATER
//    and GET_WATER_HEIGHT) on a 5x5 grid, 10 Hz; if the CPU sees the shallow
//    water simulation, the heights move with the ripples;
//  - WATER_RECT: the simulation area the vehicle producers test (0x6E5280
//    reads [0x453A520..0x453A52C], written by 0x6FDD84 from 0x600314), 2 Hz;
//  - WATER_MEM: the water manager block ([0x3F0D968], 0x7C940 bytes) and the
//    heap blocks it points to, copied every 1.5 s while recording: dwords
//    changed per page, and the first copies saved to disk for an offline
//    diff.
// Only with RedemptionEditor.ini [RedemptionEditor] WaterGridProbe=1; WaterProbeAt=x,y,z
// moves the grid. Reads are guarded; no writes to the game. Also
// WATER_GRID: the River height grid copied every 250 ms.

#include <ostream>
#include <string>

namespace replay::water::probe
{
#if defined(REDEMPTION_DEV)
    void Configure(const std::string& iniPath);
    // The layout check stays inside.
    bool Enabled();
    // Script thread, every tick. `active` is true while recording or playing.
    void Tick(std::ostream& log, const char* phase, bool active, unsigned long long nowMs);
#else
    // A research probe, built only in the developer build (REDEMPTION_DEV);
    // the public build has these no-ops, which is what the probe does with
    // its key at 0 (the default).
    inline void Configure(const std::string&) {}
    inline bool Enabled() { return false; }
    inline void Tick(std::ostream&, const char*, bool, unsigned long long) {}
#endif
}
