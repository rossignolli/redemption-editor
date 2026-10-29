#pragma once

// Shallow water disturbances (evidence only). Everything that disturbs the
// river and lake water (a wagon or a horse wading, a boat's wake, a swimming
// ped, the Vfx Water system) pushes "shapes" into the water manager (the
// singleton [0x3F0D968], vtable 0x33BEAD8) through four virtuals, each adding
// the shape to both simulations (+0x400F0, +0x5E520) under their lock. The
// replay puppets are frozen, so the engine's producers never run for them;
// the way to replay it is to record what the engine sends and send it again.
//
// This module reads only: detours on the four methods, signature-checked
// (prologue plus the call into the simulation's adder), which forward first
// and copy the arguments and the callers into a ring the script thread
// drains into the log. Installed only with RedemptionEditor.ini
// [RedemptionEditor] WaterShapeLog=1 (off by default). No natives, no calls
// into the game. Build 1491.50.

#include <cstdint>
#include <ostream>
#include <string>

namespace replay::water
{
    // The four virtuals of the water manager, by vtable slot.
    //   +0x118 0x58ADD8 (this, const Desc*)             desc 0xA0 bytes, type u32 [+0] 0..5, adder 0x6CA38C
    //   +0x120 0x58AF3C (this, f1..f6, const v4*, const v4*) adder 0x6CA7F0
    //   +0x128 0x58AE24 (this, const A* 0x20, const B* 0x40, float, const v4*, const v4*) adder 0x6CA634
    //   +0x130 0x58B110 (this, f1..f6, const v4*, const v4*) adder 0x6CA98C
    enum class Slot : std::uint8_t { Desc118 = 0, Floats120 = 1, Box128 = 2, Floats130 = 3 };

    struct ShapeCall
    {
        std::uint64_t timeUs = 0;
        std::uint32_t callers[3] = {}; // RVAs of the return addresses (0: outside the game module)
        std::uint32_t threadId = 0;
        Slot slot = Slot::Desc118;
        bool managerMatches = false;   // `this` is [0x3F0D968]
        bool copied = false;
        // +0x118: the descriptor. +0x128: A (0x20) then B (0x40).
        std::uint8_t data[0xA0] = {};
        float scalars[6] = {};         // +0x120/+0x130: f1..f6; +0x128: the float in [0]
        float vecA[4] = {};
        float vecB[4] = {};
    };

    bool Install(std::string& error);
    void Shutdown();
    bool Installed();
    std::uint64_t Total();
    // Script thread, every tick while installed: the first calls one per
    // line and, once a second, the counts per slot and caller. `phase` names
    // what the mod is doing (record, play, idle).
    void Drain(std::ostream& log, const char* phase);
}
