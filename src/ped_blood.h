#pragma once

// Ped blood and damage on bodies. Blood on a ped is not a rage decal: the ped damage
// manager (*0x4C38A18) keeps per ped a set of up to 64 damage records of
// 0x160 bytes (bullet wounds, bleeding, damage packs) blitted into the ped's
// damage texture. Every record, whatever made it (bullet 0x125DE00, bleeding
// queue 0x12C6038, damage packs 0x125D470), is finalized by 0x1285DA4(record,
// owner entity, set slot).
// A read-only detour records each finalized record after the original ran.
// Build 1491.50, signature-checked, SEH-guarded; the detour never calls natives.

#include <cstdint>
#include <string>
#include <vector>

namespace replay::blood
{
    constexpr std::size_t kRecordBytes = 0x160;
    // Return addresses into the three finalize callers (offline).
    constexpr std::uint32_t kCallerPack = 0x125D604;
    constexpr std::uint32_t kCallerBlood = 0x125EAFF;
    constexpr std::uint32_t kCallerEvolve = 0x125DDC3;
    // Return address of the finalize's own projected-decal call: those
    // projections are rebuilt by the engine when a record is applied again.
    constexpr std::uint32_t kFinalizeProjReturn = 0x1285F50;

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
    // engine's own path 0x125D470(manager, ped set slot, record copy,
    // refuse) after the ped's damage component (0xCC5A30) and set slot
    // (0x126AB48) exist. The record's time stamp (+0x130) is set so its age
    // is `age` seconds. When the ped's set already holds a record the
    // engine would merge this one into (0x125D86C's own test), nothing is
    // added and it returns true with error "already_present". `busy`: the render job holds the sets (the gate the
    // engine checks, *0x5A9D690 +0xB0/+0xC0), retry next tick.
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
    // A record that absorbs enough hits evolves to the next info inside
    // 0x125D86C: +0x108 takes the old +0x10C, the count +0x110 grows, and the engine
    // calls 0x1285DA4(record, owner, 0xFF) (the finalize the observer sees
    // with return 0x125DDC3). Playing it: find in the ped's set the record at
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
    // Removes every record of a ped (0xE1F5A0 on its damage component).
    bool Clear(void* ped, std::string& error);

    // Wetness and environment dirt of a ped: the ped's component in slot
    // +0x98 of the entity component table (index [ped+0x9C] & 0x1FFFF minus
    // [0x39ECE40], table [0x39ECE48], stride 0x148, tagged pointer); +0x30
    // vec4 wetness (low/high height, low/high amount), +0x40 wet flags,
    // +0x3A8/+0x3AC/+0x3B0 environment dirt 0..10000.
    struct Surface
    {
        float wet[4] = {};
        std::uint8_t wetFlags = 0;
        std::int32_t dirt[3] = {};
    };
    bool ReadSurface(void* ped, Surface& out);
    // Wetness: guarded writes of the plain fields on a replay puppet. Dirt:
    // the engine's three setters (they mirror the value to the MetaPed),
    // signatures checked, then +0xC0 = 1 asks the bank recompute.
    bool ApplySurface(void* ped, const Surface& surface, bool dirt, std::string& error);
}
