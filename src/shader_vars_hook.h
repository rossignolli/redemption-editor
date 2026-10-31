#pragma once

// The engine side of the animated shader variables (shader_vars_core.h):
// the creature's shader vars component, read on the script thread
// (recording, evidence), and the puppets' recorded values written into it at
// the end of the engine's own write (playback).
//
// Reads are guarded; every function the layout comes from is found by its
// signature once a session (shader_vars_hook.cpp).
//
// Write (rung 5, MuscleReplay): a detour of the job's frame write node, the
// only writer of the buffer. After the original copies the motion tree's
// values, a node whose destination is a published puppet's buffer gets the
// recorded values on top. Writing from the shader reader instead was tried
// and failed in game (clip 1272): the job refills the buffer every update.
// The reader detour stays as an observer only (shader_reads, shader_lanes,
// shader_matched).
// Research: docs/research/shaders-muscle-vars.md

#include "shader_vars_core.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace replay::shadervars::engine
{
    constexpr std::uint32_t kMaxTargets = 128;
    constexpr std::uint32_t kMaxValues = 128 * kMaxLanes; // published values, all targets

    // The signatures and the component type (once; no hook). The reads
    // below need it.
    bool Init(std::string& error);
    bool Ready();

    enum class Read : std::uint8_t
    {
        Ok,
        Off,         // Init failed (signatures)
        NoEntity,
        NoComponent, // the creature has no shader vars component
        Layout,      // the component is not the layout
        Fault,       // a guarded read failed
    };
    const char* ReadText(Read r);

    // Script thread, guarded reads only. buffer: the component's value
    // buffer (the write node's destination).
    Read ComponentOf(std::uintptr_t ped, std::uintptr_t& component);
    Read BufferOf(std::uintptr_t component, std::uintptr_t& buffer, std::uint16_t& bytes);
    // The ped's sweat as the shader effect copies it every update (the
    // graphics component's +0x488 into the effect instance +0xB4) and the
    // script override flag +0x484 (SET_PED_SWEAT).
    Read SweatOf(std::uintptr_t ped, float& value, std::uint8_t& on);
    bool SweatReady();
    Read LayoutOf(std::uintptr_t component, Layout& out, ParseResult& parse);
    Read ValuesOf(std::uintptr_t component, const Layout& layout, std::vector<std::uint32_t>& bits);

    // The detours (playback). Install calls Init. The observer of the shader
    // reader is optional (observerError says why it is off).
    bool Install(std::string& error, std::string& observerError);
    void Shutdown();
    bool Installed();
    bool ObserverInstalled();

    // The puppets to write from now on (replaces the table; empty: none):
    // each value is (byte offset in the component buffer, raw bits).
    struct PlayTarget
    {
        std::uintptr_t ped = 0;
        std::uintptr_t buffer = 0;
        std::uint16_t bytes = 0;
        std::vector<std::pair<std::uint16_t, std::uint32_t>> values;
    };
    void SetPlayTargets(const std::vector<PlayTarget>& targets);

    // Recording: the recorded horses whose buffer the write node copies into
    // snapshot slot `slot` (0..kMaxTargets-1, the caller keeps it per horse)
    // right after the engine's copy: the values the shader reads, never a
    // read in the middle of the job's 0xFF fill.
    struct RecordTarget
    {
        std::uintptr_t ped = 0;
        std::uintptr_t buffer = 0;
        std::uint16_t bytes = 0;
        std::uint32_t slot = 0;
    };
    void SetRecordTargets(const std::vector<RecordTarget>& targets);
    // The slot's latest copy when newer than `seq` (updated).
    bool TakeSnapshot(std::uint32_t slot, std::uint32_t& seq, std::vector<std::uint8_t>& out);

    // Replace (default) the engine's copy into a published buffer with the
    // recorded values, or (false, MuscleDebugOverwrite=1) let it copy and
    // write on top, an earlier build's behaviour.
    void SetReplace(bool replace);

    // Peds the observer counts without writing (the live recorded horses):
    // how often the shader effect reads them and the effect's slot count.
    void SetWatch(const std::vector<std::uintptr_t>& peds);

    // The component's shader var bindings (+0x18, u16 count +0x20; 16-byte
    // rows: u32 hash, u8 component index, s8 slot, u8 -, u8 channel bits,
    // u16 value offset, u16 id, u8 track, u8 type), as text
    // "hash:index:slot:channel:track:id:type" ('|' separated, first 32).
    Read BindingsText(std::uintptr_t component, std::string& text, std::uint32_t& count);

    struct Stats
    {
        std::uint64_t nodeCalls = 0;    // every call of the write node
        std::uint64_t hits = 0;         // calls whose destination is a published buffer
        std::uint64_t writes = 0;       // of those, the ones that wrote
        std::uint64_t values = 0;       // 4-byte values written
        std::uint64_t failed = 0;       // a value outside the node's bytes, or a write that faulted
        std::uint64_t engineSet = 0;    // lanes the motion tree had set where a value went (before it)
        std::uint64_t engineLanes = 0;  // lanes looked at for engineSet
        std::uint64_t retries = 0;      // a publish waited for a reader of the table it fills
        std::uint64_t mismatch = 0;     // a published buffer with other bytes (left alone)
        std::uint64_t shaderCalls = 0;  // every call of the shader reader (observer)
        std::uint64_t shaderReads = 0;  // calls for a published puppet
        std::uint64_t shaderLanes = 0;  // values compared there
        std::uint64_t shaderMatched = 0; // equal to the published value
        std::uint64_t watchReads = 0;   // shader reads of a watched ped
        std::uint64_t replaced = 0;     // hits written instead of the engine's copy
        std::uint64_t snapshots = 0;    // recorded horses' buffers copied at the node
        std::uint64_t snapshotsEmpty = 0; // node passes that left the buffer empty (not kept)
        std::uint32_t effectSlotsMin = 0xFFFF; // u16 [effect + 0x88] at those reads and the published ones
        std::uint32_t effectSlotsMax = 0;
    };
    Stats Snapshot();
}
