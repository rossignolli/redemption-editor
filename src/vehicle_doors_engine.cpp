#include "vehicle_doors_engine.h"

#include "entity_cache.h"
#include "entity_lifecycle.h"
#include "guarded_memory.h"
#include "log_format.h"
#include "profiler.h"

#include "main.h"
#include "natives.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <unordered_map>
#include <unordered_set>

#include <windows.h>

// Engine-side vehicle door reads and writes (vehicle_doors_engine.h).

namespace replay::vehdoors
{
    namespace
    {
        // IS_VEHICLE_DOOR_FULLY_OPEN's core, whole up to the compare of +0x28.
        constexpr std::uint32_t kFullyOpenRva = 0x10EC3FC;
        constexpr std::uint8_t kFullyOpenSignature[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48,
            0x63, 0xFA, 0x33, 0xDB, 0xE8, 0x08, 0x77, 0xDC, 0xFF, 0x48, 0x8B, 0xC8, 0x48, 0x85, 0xC0, 0x74, 0x23, 0x48,
            0x8D, 0x15, 0x09, 0xB7, 0x9F, 0x02, 0x8B, 0x14, 0xBA, 0xE8, 0x45, 0xA6, 0x4A, 0x00, 0x48, 0x85, 0xC0, 0x74,
            0x0F, 0xF3, 0x0F, 0x10, 0x05, 0xB0, 0x27, 0x28, 0x02, 0x0F, 0x2F, 0x40, 0x28, 0x0F, 0x92, 0xC3};
        constexpr std::uint32_t kTableLeaRva = 0x10EC418; // lea rdx, [rip+disp32]: the script door -> hierarchy id table
        // GetDoorFromId: the array, the count, the stride, the id.
        constexpr std::uint32_t kDoorFromIdRva = 0x1596A6C;
        constexpr std::uint8_t kDoorFromIdSignature[] = {0x44, 0x8B, 0x91, 0xD8, 0x0B, 0x00, 0x00, 0x45, 0x33, 0xC0, 0x45,
            0x8B, 0xC8, 0x45, 0x85, 0xD2, 0x7E, 0x28, 0x4D, 0x63, 0xD9, 0x45, 0x3B, 0xCA, 0x7D, 0x0D, 0x49, 0x6B, 0xC3,
            0x70, 0x48, 0x03, 0x81, 0xD0, 0x0B, 0x00, 0x00, 0xEB, 0x03, 0x49, 0x8B, 0xC0, 0x0F, 0xBF, 0x40, 0x0C, 0x3B,
            0xC2, 0x74, 0x0B, 0x41, 0xFF, 0xC1, 0x45, 0x3B, 0xCA, 0x7C, 0xD8, 0x33, 0xC0, 0xC3};
        // SetTargetDoorOpenRatio: old target +0x24, target +0x20, flags +0x14, bit 18.
        constexpr std::uint32_t kSetTargetRva = 0x14D7CC7;
        constexpr std::uint8_t kSetTargetSignature[] = {0xF3, 0x0F, 0x11, 0x41, 0x24, 0xF3, 0x0F, 0x11, 0x79, 0x20, 0x8B,
            0x49, 0x14, 0x8B, 0xC1, 0xC1, 0xE8, 0x12, 0xA8, 0x01};
        // The latch: the handler's type test (17, 18) and the store.
        constexpr std::uint32_t kLatchTypeRva = 0x10F287D;
        constexpr std::uint8_t kLatchTypeSignature[] = {0x8B, 0x8F, 0xB0, 0x0B, 0x00, 0x00, 0x83, 0xE9, 0x11, 0x83, 0xF9,
            0x01, 0x77, 0x0B};
        constexpr std::uint32_t kLatchStoreRva = 0x1545F6F;
        constexpr std::uint8_t kLatchStoreSignature[] = {0x40, 0x88, 0xAF, 0x80, 0x16, 0x00, 0x00};

        constexpr std::uint32_t kDoorArray = 0xBD0;
        constexpr std::uint32_t kDoorCount = 0xBD8;
        constexpr std::uint32_t kDoorStride = 0x70;
        constexpr std::uint32_t kDoorId = 0xC;
        constexpr std::uint32_t kDoorFlags = 0x14;
        constexpr std::uint32_t kDoorRatio = 0x28;
        constexpr std::uint32_t kVehicleType = 0xBB0;
        constexpr std::uint32_t kLatch = 0x1680;
        constexpr std::uint32_t kArticulatedBit = 1u << 18;
        constexpr std::int32_t kMaxDoorsInArray = 32;

        struct Engine
        {
            bool checked = false;
            bool doors = false;
            bool table = false;
            bool flags = false;
            bool latch = false;
            std::int32_t hierarchy[kScriptDoors]{};
            std::string error;
        };
        Engine g_engine;
        bool g_write = true;

        std::uintptr_t Base() { return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)); }

        bool Same(std::uintptr_t at, const std::uint8_t* bytes, std::size_t size)
        {
            return replay::mem::Matches(reinterpret_cast<const void*>(at), bytes, size);
        }

        void CheckEngine()
        {
            if (g_engine.checked)
                return;
            g_engine.checked = true;
            const std::uintptr_t base = Base();
            g_engine.doors = Same(base + kFullyOpenRva, kFullyOpenSignature, sizeof(kFullyOpenSignature)) &&
                Same(base + kDoorFromIdRva, kDoorFromIdSignature, sizeof(kDoorFromIdSignature));
            if (g_engine.doors)
            {
                // The table the core reads the hierarchy id from (lea rdx, [rip+disp32]).
                std::int32_t disp = 0;
                const std::uintptr_t lea = base + kTableLeaRva;
                if (replay::mem::Read(lea + 3, disp))
                {
                    const std::uintptr_t table = lea + 7 + static_cast<std::intptr_t>(disp);
                    bool plausible = replay::mem::Copy(g_engine.hierarchy, table, sizeof(g_engine.hierarchy));
                    for (int n = 0; plausible && n < kScriptDoors; ++n)
                    {
                        plausible = g_engine.hierarchy[n] > 0 && g_engine.hierarchy[n] < 1024;
                        for (int m = 0; plausible && m < n; ++m)
                            plausible = g_engine.hierarchy[m] != g_engine.hierarchy[n];
                    }
                    g_engine.table = plausible;
                }
            }
            g_engine.flags = Same(base + kSetTargetRva, kSetTargetSignature, sizeof(kSetTargetSignature));
            g_engine.latch = Same(base + kLatchTypeRva, kLatchTypeSignature, sizeof(kLatchTypeSignature)) &&
                Same(base + kLatchStoreRva, kLatchStoreSignature, sizeof(kLatchStoreSignature));
            std::string error;
            if (!g_engine.doors)
                error += "doors;";
            if (g_engine.doors && !g_engine.table)
                error += "table;";
            if (!g_engine.flags)
                error += "flags;";
            if (!g_engine.latch)
                error += "latch;";
            g_engine.error = error;
        }

        std::string IdOf(std::uint32_t packed) { return replay::entity::IdText(replay::entity::ReplayId::FromPacked(packed)); }

        bool DoorsReadable() { return g_engine.doors && g_engine.table; }

        // The door of a script index (0 when the vehicle has none), as GetDoorFromId.
        std::uintptr_t DoorOf(std::uintptr_t vehicle, int scriptDoor)
        {
            if (!DoorsReadable() || !vehicle || scriptDoor < 0 || scriptDoor >= kScriptDoors)
                return 0;
            std::uint64_t array = 0;
            std::int32_t count = 0;
            if (!replay::mem::Read(vehicle + kDoorArray, array) || !replay::mem::Read(vehicle + kDoorCount, count) ||
                count <= 0 || count > kMaxDoorsInArray || !replay::mem::InUserRange(static_cast<std::uintptr_t>(array), 8))
                return 0;
            for (std::int32_t n = 0; n < count; ++n)
            {
                const std::uintptr_t door = static_cast<std::uintptr_t>(array) + static_cast<std::uintptr_t>(n) * kDoorStride;
                std::int16_t id = 0;
                if (!replay::mem::Read(door + kDoorId, id))
                    return 0;
                if (id == g_engine.hierarchy[scriptDoor])
                    return door;
            }
            return 0;
        }

        bool ReadRatio(std::uintptr_t door, float& ratio)
        {
            return door && replay::mem::Read(door + kDoorRatio, ratio) && std::isfinite(ratio);
        }

        bool IsLatchType(std::uintptr_t vehicle)
        {
            std::int32_t type = 0;
            return g_engine.latch && vehicle && replay::mem::Read(vehicle + kVehicleType, type) && (type == 17 || type == 18);
        }

        bool ReadLatch(std::uintptr_t vehicle, std::uint8_t& latch)
        {
            return IsLatchType(vehicle) && replay::mem::Read(vehicle + kLatch, latch);
        }

        // ----- Recording -----

        struct RecordStats
        {
            std::uint32_t vehicles = 0;      // with a track (a door or the latch)
            std::uint32_t noDoors = 0;       // read, nothing to record
            std::uint32_t unreadable = 0;
            std::uint32_t doorsPresent = 0;  // door channels seen
            std::uint32_t doorsOpened = 0;   // tracks with a door that opened
            std::uint32_t articulated = 0;
            std::uint32_t latchTracks = 0;
            std::uint64_t reads = 0;
            std::uint32_t logs = 0;
            std::uint32_t openLogs = 0;
        };
        RecordStats g_record;
        std::unordered_map<std::uint32_t, std::size_t> g_recordIndex;
        std::unordered_set<std::uint32_t> g_recordNoDoors;

        // ----- Playback -----

        struct ChannelPlay
        {
            float wanted = -1.0f;
            ULONGLONG since = 0; // the wanted value or the puppet changed
            bool judged = false;
        };
        struct PuppetPlay
        {
            std::int32_t clone = 0;
            ChannelPlay channels[kChannels];
        };
        struct PlayStats
        {
            std::uint32_t tracks = 0;
            std::uint32_t tracksOpening = 0;
            std::uint32_t puppets = 0;
            std::uint64_t driven = 0;    // SET_VEHICLE_DOOR_CONTROL calls
            std::uint64_t written = 0;   // rung 3 writes
            std::uint32_t writeFailed = 0;
            std::uint32_t noDoor = 0;    // a recorded door the puppet does not have
            std::uint32_t checks = 0;
            std::uint32_t match = 0;
            std::uint32_t mismatch = 0;
            std::uint32_t unread = 0;
            std::uint64_t latchApplied = 0;
            std::uint32_t latchMismatch = 0;
            float worst = 0.0f;
            std::uint32_t logs = 0;
        };
        PlayStats g_play;
        std::unordered_map<std::uint32_t, PuppetPlay> g_puppets;
        std::unordered_map<std::uint32_t, std::size_t> g_playIndex;
    }

    bool Check(std::string& error)
    {
        CheckEngine();
        error = g_engine.error;
        return DoorsReadable();
    }

    std::string Describe()
    {
        CheckEngine();
        char text[320]{};
        std::snprintf(text, sizeof(text),
            "VEHICLE_DOORS,doors=%d,table=%d,flags=%d,latch=%d,write=%d,hierarchy=%d;%d;%d;%d;%d;%d;%d;%d;%d;%d,error=%s",
            g_engine.doors ? 1 : 0, g_engine.table ? 1 : 0, g_engine.flags ? 1 : 0, g_engine.latch ? 1 : 0, g_write ? 1 : 0,
            g_engine.hierarchy[0], g_engine.hierarchy[1], g_engine.hierarchy[2], g_engine.hierarchy[3], g_engine.hierarchy[4],
            g_engine.hierarchy[5], g_engine.hierarchy[6], g_engine.hierarchy[7], g_engine.hierarchy[8], g_engine.hierarchy[9],
            g_engine.error.empty() ? "none" : g_engine.error.c_str());
        return text;
    }

    void SetWrite(bool on) { g_write = on; }

    void BeginRecording()
    {
        CheckEngine();
        g_record = RecordStats{};
        g_recordIndex.clear();
        g_recordNoDoors.clear();
    }

    void RecordVehicle(std::vector<DoorTrack>& tracks, std::uint32_t id, std::int32_t handle, bool train,
        std::uint32_t timeMs, std::ostream& log)
    {
        if (!DoorsReadable() || !id || g_recordNoDoors.count(id))
            return;
        DoorTrack* track = nullptr;
        const auto found = g_recordIndex.find(id);
        if (found != g_recordIndex.end() && found->second < tracks.size() && tracks[found->second].id == id)
            track = &tracks[found->second];
        if (track && track->checked && timeMs - track->lastCheckMs < kSampleMs)
            return;
        const auto vehicle = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(handle));
        if (!vehicle)
            return;
        ++g_record.reads;
        float ratios[kScriptDoors]{};
        std::uint32_t flags[kScriptDoors]{};
        std::uint16_t present = 0;
        for (int door = 0; door < kScriptDoors; ++door)
        {
            const std::uintptr_t at = DoorOf(vehicle, door);
            if (!ReadRatio(at, ratios[door]))
                continue;
            present = static_cast<std::uint16_t>(present | (1u << door));
            if (g_engine.flags)
                replay::mem::Read(at + kDoorFlags, flags[door]);
        }
        std::uint8_t latch = 0;
        const bool latchRead = train && ReadLatch(vehicle, latch);
        if (!track)
        {
            if (!present && !latchRead)
            {
                g_recordNoDoors.insert(id);
                ++g_record.noDoors;
                return;
            }
            g_recordIndex[id] = tracks.size();
            tracks.push_back(DoorTrack{});
            track = &tracks.back();
            track->id = id;
            ++g_record.vehicles;
            g_record.latchTracks += latchRead ? 1u : 0u;
            for (int door = 0; door < kScriptDoors; ++door)
            {
                if (!(present & (1u << door)))
                    continue;
                ++g_record.doorsPresent;
                g_record.articulated += (flags[door] & kArticulatedBit) ? 1u : 0u;
            }
            if (g_record.logs < 48)
            {
                ++g_record.logs;
                const replay::logfmt::FormatScope logFormat(log);
                log << std::fixed << std::setprecision(3) << "VEHICLE_DOOR_RECORD,id=" << IdOf(id)
                    << ",kind=" << (train ? "train_car" : "vehicle") << ",doors=0x" << std::hex << present << std::dec
                    << ",ratios=";
                for (int door = 0, shown = 0; door < kScriptDoors; ++door)
                    if (present & (1u << door))
                        log << (shown++ ? ";" : "") << door << '=' << ratios[door];
                log << ",flags=";
                for (int door = 0, shown = 0; door < kScriptDoors; ++door)
                    if (present & (1u << door))
                        log << (shown++ ? ";" : "") << door << "=0x" << std::hex << flags[door] << std::dec;
                log << ",latch=" << (latchRead ? static_cast<int>(latch) : -1) << ",time_ms=" << timeMs << '\n';
            }
        }
        track->checked = true;
        track->lastCheckMs = timeMs;
        for (int door = 0; door < kScriptDoors; ++door)
        {
            if (!(present & (1u << door)))
                continue;
            const DoorSample* before = LastStored(*track, static_cast<std::uint8_t>(door));
            const bool wasShut = !before || before->ratio < kStoreStep;
            if (Observe(*track, timeMs, static_cast<std::uint8_t>(door), ratios[door]) && wasShut &&
                Quantize(ratios[door]) >= kStoreStep && g_record.openLogs < 64)
            {
                ++g_record.openLogs;
                const replay::logfmt::FormatScope logFormat(log);
                log << std::fixed << std::setprecision(3) << "VEHICLE_DOOR_OPEN,id=" << IdOf(id)
                    << ",door=" << door << ",ratio=" << ratios[door] << ",flags=0x" << std::hex << flags[door] << std::dec
                    << ",time_ms=" << timeMs << '\n';
            }
        }
        if (latchRead)
            Observe(*track, timeMs, kLatchChannel, latch ? 1.0f : 0.0f);
    }

    void EndRecording(const std::vector<DoorTrack>& tracks, std::ostream& log)
    {
        if (!g_record.reads)
            return;
        std::size_t samples = 0;
        std::uint32_t opened = 0;
        for (const DoorTrack& track : tracks)
        {
            samples += track.samples.size();
            opened += EverOpen(track) ? 1u : 0u;
        }
        g_record.doorsOpened = opened;
        log << "VEHICLE_DOOR_RECORD_SUMMARY,tracks=" << tracks.size() << ",vehicles=" << g_record.vehicles
            << ",no_doors=" << g_record.noDoors << ",doors_present=" << g_record.doorsPresent
            << ",tracks_opened=" << opened << ",articulated=" << g_record.articulated
            << ",latch_tracks=" << g_record.latchTracks << ",samples=" << samples << ",reads=" << g_record.reads
            << ",engine=" << (g_engine.error.empty() ? "ok" : g_engine.error.c_str()) << '\n';
    }

    void BeginPlayback(const std::vector<DoorTrack>& tracks)
    {
        CheckEngine();
        g_play = PlayStats{};
        g_puppets.clear();
        g_playIndex.clear();
        for (std::size_t n = 0; n < tracks.size(); ++n)
        {
            g_playIndex[tracks[n].id] = n;
            ++g_play.tracks;
            g_play.tracksOpening += EverOpen(tracks[n]) ? 1u : 0u;
        }
    }

    const DoorTrack* TrackOf(const std::vector<DoorTrack>& tracks, std::uint32_t id)
    {
        const auto found = g_playIndex.find(id);
        if (found != g_playIndex.end() && found->second < tracks.size() && tracks[found->second].id == id)
            return &tracks[found->second];
        for (const DoorTrack& track : tracks)
            if (track.id == id)
                return &track;
        return nullptr;
    }

    void Apply(std::int32_t clone, const DoorTrack& track, std::uint32_t timeMs, std::ostream& log)
    {
        REPLAY_PROFILE_ZONE("play.vehicle_doors");
        if (!clone || !replay::entities::Exists(clone))
            return;
        PuppetPlay& play = g_puppets[track.id];
        const ULONGLONG now = GetTickCount64();
        if (play.clone != clone)
        {
            if (!play.clone)
                ++g_play.puppets;
            play = PuppetPlay{};
            play.clone = clone;
        }
        const auto vehicle = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(clone));
        const std::uint16_t channels = Channels(track);
        for (int door = 0; door < kScriptDoors; ++door)
        {
            if (!(channels & (1u << door)))
                continue;
            float wanted = 0.0f;
            if (!RatioAt(track, static_cast<std::uint8_t>(door), timeMs, wanted))
                continue;
            ChannelPlay& channel = play.channels[door];
            if (channel.wanted < 0.0f || std::fabs(wanted - channel.wanted) >= kStoreStep)
            {
                channel.wanted = wanted;
                channel.since = now;
                channel.judged = false;
            }
            const std::uintptr_t at = DoorOf(vehicle, door);
            if (DoorsReadable() && !at)
            {
                if (!channel.judged)
                {
                    channel.judged = true;
                    ++g_play.noDoor;
                }
                continue;
            }
            float got = 0.0f;
            bool readable = ReadRatio(at, got);
            if (NeedsDrive(wanted, got, readable))
            {
                // Rung 1: the target, the engine's flags; the setter steps
                // the current ratio toward it.
                VEHICLE::SET_VEHICLE_DOOR_CONTROL(clone, door, 1, wanted);
                ++g_play.driven;
                readable = ReadRatio(at, got);
                // Rung 3 (VehicleDoorWrite): the puppet's own door to the
                // recorded ratio when the step left it off.
                if (g_write && readable && std::fabs(wanted - got) > kDriveTolerance)
                {
                    if (replay::mem::Put(at + kDoorRatio, &wanted, sizeof(wanted)))
                        ++g_play.written;
                    else
                        ++g_play.writeFailed;
                    readable = ReadRatio(at, got);
                }
            }
            if (channel.judged)
                continue;
            const std::uint32_t since = static_cast<std::uint32_t>((std::min)(now - channel.since, ULONGLONG{0xFFFFFFFF}));
            const Verdict verdict = Judge(wanted, got, readable, since);
            if (verdict == Verdict::Settling)
                continue;
            channel.judged = true;
            ++g_play.checks;
            if (verdict == Verdict::Match)
                ++g_play.match;
            else if (verdict == Verdict::Unread)
                ++g_play.unread;
            else
            {
                ++g_play.mismatch;
                g_play.worst = (std::max)(g_play.worst, std::fabs(wanted - got));
                if (g_play.logs < 32)
                {
                    ++g_play.logs;
                    std::uint32_t flags = 0;
                    if (g_engine.flags)
                        replay::mem::Read(at + kDoorFlags, flags);
                    const replay::logfmt::FormatScope logFormat(log);
                    log << std::fixed << std::setprecision(3) << "VEHICLE_DOOR_CHECK,id=" << IdOf(track.id)
                        << ",door=" << door << ",wanted=" << wanted << ",got=" << got
                        << ",flags=0x" << std::hex << flags << std::dec << ",articulated=" << ((flags & kArticulatedBit) ? 1 : 0)
                        << ",write=" << (g_write ? 1 : 0) << ",verdict=mismatch,playhead_ms=" << timeMs << '\n';
                }
            }
        }
        if (channels & (1u << kLatchChannel))
        {
            float wanted = 0.0f;
            std::uint8_t got = 0;
            if (RatioAt(track, kLatchChannel, timeMs, wanted) && ReadLatch(vehicle, got) &&
                (got != 0) != (wanted >= 0.5f))
            {
                VEHICLE::_0x762FDC4C19E5A981(clone, wanted >= 0.5f ? TRUE : FALSE);
                ++g_play.latchApplied;
                if (ReadLatch(vehicle, got) && (got != 0) != (wanted >= 0.5f))
                    ++g_play.latchMismatch;
            }
        }
    }

    bool ReadDoorRatio(std::int32_t vehicle, int door, float& ratio)
    {
        CheckEngine();
        ratio = 0.0f;
        return ReadRatio(DoorOf(reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(vehicle)), door), ratio);
    }

    void EndPlayback(std::ostream& log)
    {
        if (!g_play.tracks && !g_play.puppets)
            return;
        const replay::logfmt::FormatScope logFormat(log);
        log << std::fixed << std::setprecision(3) << "VEHICLE_DOOR_SUMMARY,phase=play,tracks=" << g_play.tracks
            << ",tracks_opening=" << g_play.tracksOpening << ",puppets=" << g_play.puppets << ",driven=" << g_play.driven
            << ",written=" << g_play.written << ",write_failed=" << g_play.writeFailed << ",write=" << (g_write ? 1 : 0)
            << ",no_door=" << g_play.noDoor << ",checks=" << g_play.checks << ",match=" << g_play.match
            << ",mismatch=" << g_play.mismatch << ",unread=" << g_play.unread << ",latch_applied=" << g_play.latchApplied
            << ",latch_mismatch=" << g_play.latchMismatch << ",worst_diff=" << g_play.worst
            << ",engine=" << (g_engine.error.empty() ? "ok" : g_engine.error.c_str()) << '\n';
        g_puppets.clear();
        g_playIndex.clear();
    }
}
