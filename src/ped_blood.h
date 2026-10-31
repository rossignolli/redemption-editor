#pragma once

// Ped blood and damage on bodies. Blood on a ped is not a rage decal: the ped damage
// manager keeps per ped a set of up to 64 damage records of 0x160 bytes (bullet
// wounds, bleeding, damage packs) blitted into the ped's damage texture. Every
// record, whatever made it (the bullet blood, the bleeding queue, the damage
// packs' add), is finalized by the record finalize (record, owner entity, set
// slot).
// A read-only detour records each finalized record after the original ran.
// Signature-checked, SEH-guarded; the detour never calls natives.

#include <cstdint>
#include <string>
#include <vector>

namespace replay::blood
{
    constexpr std::size_t kRecordBytes = 0x160;
    // The return addresses of the finalize's three callers (the damage packs'
    // add, the bullet blood, the merge's evolve) as Call::caller records them,
    // RVAs of the running exe. A caller the game lacks gets a value above every
    // RVA, its own: never a recorded caller, never another caller's.
    std::uint32_t CallerPack();
    std::uint32_t CallerBlood();
    std::uint32_t CallerEvolve();
    // The return address of the finalize's own projected-decal call (the same
    // way): those projections are rebuilt by the engine when a record is
    // applied again.
    std::uint32_t FinalizeProjReturn();

    struct Call
    {
        std::uint64_t timeUs = 0;
        std::uint64_t entity = 0;
        std::uint8_t slot = 0;
        std::uint32_t caller = 0;
        std::int32_t index = -1; // record pool index, -1 unknown
        float age = 0.0f;        // engine time minus record +0x130
        bool copied = false;
        std::uint8_t record[kRecordBytes] = {};
    };

    bool InstallObserver(std::string& error);
    void SetObserving(bool on);
    void Shutdown();
    std::uint64_t NowUs();
    std::uint64_t TotalCalls();
    void CollectSince(std::uint64_t& cursor, std::vector<Call>& out);

    // Playback (script thread): adds a recorded record to a ped through the
    // engine's own add path (manager, ped set slot, record copy, refuse) after
    // the ped's damage component and its set slot exist. The record's time
    // stamp (+0x130) is set so its age is `age` seconds. When the ped's set
    // already holds a record the engine would merge this one into (the merge's
    // own test), nothing is added and it returns true with error
    // "already_present". `busy`: the render job holds the sets (the gate the
    // engine checks: the render gate object's +0xB0/+0xC0), retry next tick.
    // The last Apply's engine slot allocation and add, in ms.
    void LastApplyTiming(double& allocMs, double& addMs, bool& allocated);
    // The readability checks (VirtualQuery) of Apply, ApplyEvolve,
    // ReadRecords and Clear, summed since load: queries made, cache hits and
    // the time the queries took. ForgetReadableRegions drops the cache (at
    // playback start and end).
    void QueryStats(std::uint64_t& queries, std::uint64_t& hits, double& queryMs);
    // The readability checks answered by the page's working set entry instead of
    // VirtualQuery (readable_page.h) since load, and their time.
    void PageAnswerStats(std::uint64_t& answers, double& ms);
    void ForgetReadableRegions();
    bool Apply(void* ped, const std::uint8_t* record, float age, bool pack, std::uint64_t ref78,
        bool& busy, std::string& error);
    // A record that absorbs enough hits evolves to the next info inside the
    // merge: +0x108 takes the old +0x10C, the count +0x110 grows, and the engine
    // calls the finalize (record, owner, 0xFF) (the finalize the observer sees
    // from CallerEvolve). Playing it: find in the ped's set the record at
    // the same position (+0x00 xyz) whose +0x10C is the event's +0x108, write
    // the event's +0x108, +0x10C, +0x110 and +0x104 into it and call the
    // finalize the same way (signature checked, SEH, under the render gate).
    // A record already at the event's info returns true with error
    // "already_evolved"; none found: false with "no_base_record" (the base
    // record may still be pending).
    bool ApplyEvolve(void* ped, const std::uint8_t* record, bool& busy, std::string& error);
    // The records a ped carries right now (its damage set: [manager+0xD8 +
    // slot*8], u16 indices at +0x610, count at +0x690), for the state a
    // recording starts with. Guarded reads only.
    struct HeldRecord
    {
        float age = 0.0f;
        bool pack = false; // record +0x83
        std::uint8_t record[kRecordBytes] = {};
    };
    bool ReadRecords(const void* ped, std::vector<HeldRecord>& out, std::string& error);
    // Removes every record of a ped (the engine's clear on its damage component).
    bool Clear(void* ped, std::string& error);

    // Wetness and environment dirt of a ped: the ped's graphics component, in
    // slot +0x98 of the entity component table (index [ped+0x9C] & 0x1FFFF
    // minus the table's base index, rows of 0x148, tagged pointer: engine_sigs.h
    // kComponentRead); +0x30 vec4 wetness (low/high height, low/high amount),
    // +0x40 wet flags, +0x3A8/+0x3AC/+0x3B0 environment dirt 0..10000.
    struct Surface
    {
        float wet[4] = {};
        std::uint8_t wetFlags = 0;
        std::int32_t dirt[3] = {};
    };
    bool ReadSurface(void* ped, Surface& out);
    // Wetness: guarded writes of the plain fields on a replay puppet (the
    // fields the engine's wetness clear writes). Dirt: the engine's three
    // setters (they mirror the value to the MetaPed), signatures checked, then
    // +0xC0 = 1 asks the bank recompute (as the engine's recompute request
    // does).
    bool ApplySurface(void* ped, const Surface& surface, bool dirt, std::string& error);
}
