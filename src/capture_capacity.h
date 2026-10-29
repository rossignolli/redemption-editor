#pragma once

// How many skeletons the recording captures at once, and the slot arrays
// behind it. The bound is capture and playback cost, so the ped class has a
// capacity (nearest first, entity_selection.h): the ini key PedCapacity, read
// once at load before any recording; the vehicle class has 8 places.
// With WorldModel=gta the whole ped pool is recorded: the capacity accepts up
// to the pool and defaults to the pool less 2 (the hidden real player and his
// horse still hold pool slots while every recorded ped has its puppet). The
// vehicle class then goes by the vehicle pool too, less 1 (the hidden
// player's vehicle), so every vehicle of the pool can be recorded.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace replay::capacity
{
    // The vehicle class with the GTA world model off.
    constexpr std::uint32_t kLegacyVehicleCapacity = 8;
    // The vehicle pool of build 1491.50 ("CVehicle", pool at 0x500D850,
    // entity_pools.h). Its creator 0x15A34CC sizes it with GetSizeOfPool
    // 0x2B09CAC("CVehicle" 0xF820AAA9, default = the "Vehicles" 0x74B31BE3
    // pool size, stored at 0x500CFE8 by 0x15D761C from 0xAA9C4B); its caller
    // 0x155E0E4 asserts that default is at most 0x80. The size itself is
    // gameconfig data: the live header (+0x18) reads 80 in every session
    // (ENTITY_POOLS vehicles=n/80), like the ped pool's 150.
    constexpr std::uint32_t kVehiclePoolSlots = 80;
    constexpr std::uint32_t kMaxVehicleCapacity = kVehiclePoolSlots - 1;
    // Capture records armed at once and playback override slots: the size
    // of every array indexed by a capture or override slot. The 256 of the
    // 8-vehicle class plus the vehicle pool's extra places, so the weapon
    // places (object_pose.h WeaponPlaces, the slots left) stay 100 at the
    // GTA model's 148 peds.
    constexpr std::uint32_t kLiveTargetSlots = 256 + kMaxVehicleCapacity - kLegacyVehicleCapacity;
    constexpr std::uint32_t kDefaultPedCapacity = 56;
    constexpr std::uint32_t kMinPedCapacity = 8;
    // The ped pool of build 1491.50 (pool at 0x4A11F70, 150 slots).
    constexpr std::uint32_t kPedPoolSlots = 150;
    // The limit with the GTA world model off (128 slots less the vehicles).
    constexpr std::uint32_t kLegacyMaxPedCapacity = 120;
    // The limit with WorldModel=gta: the whole pool.
    constexpr std::uint32_t kMaxPedCapacity = kPedPoolSlots;
    constexpr std::uint32_t kGtaDefaultPedCapacity = kPedPoolSlots - 2;
    static_assert(kMaxPedCapacity + kMaxVehicleCapacity <= kLiveTargetSlots, "both pools fit the slots");
    static_assert(kLegacyMaxPedCapacity <= kMaxPedCapacity, "the legacy limit is below the pool");
    // The weapon registry (weapons and attached objects of the recorded
    // peds) at the default ped capacity; it grows with the ped class.
    constexpr std::size_t kDefaultWeaponCapacity = 96;
    // New script handles per pool scan beyond the ped capacity, and beyond
    // the vehicle capacity.
    constexpr int kPoolHandleMargin = 24;
    constexpr int kPoolVehicleHandleMargin = 8;

    // The highest ped capacity the ini may ask for: the whole pool with
    // the GTA world model, 120 without it.
    constexpr std::uint32_t PedCapacityLimit(bool wholePool)
    {
        return wholePool ? kMaxPedCapacity : kLegacyMaxPedCapacity;
    }

    // PedCapacity's default: 56, or the pool less 2 with the GTA model.
    constexpr std::uint32_t DefaultPedCapacity(bool wholePool)
    {
        return wholePool ? kGtaDefaultPedCapacity : kDefaultPedCapacity;
    }

    inline std::uint32_t ClampPedCapacity(int requested, bool wholePool = false)
    {
        const std::uint32_t limit = PedCapacityLimit(wholePool);
        if (requested < static_cast<int>(kMinPedCapacity))
            return kMinPedCapacity;
        if (requested > static_cast<int>(limit))
            return limit;
        return static_cast<std::uint32_t>(requested);
    }

    // The vehicle class: 8, or the pool less 1 with the GTA model.
    constexpr std::uint32_t VehicleCapacityFor(bool wholePool)
    {
        return wholePool ? kMaxVehicleCapacity : kLegacyVehicleCapacity;
    }

    // Capture places in use: the ped class and the vehicle class.
    inline std::uint32_t LiveTargets(std::uint32_t pedCapacity, std::uint32_t vehicleCapacity)
    {
        return pedCapacity + vehicleCapacity;
    }

    // The weapon registry for a ped capacity: 96 at 56 peds or fewer, in
    // proportion above (206 at 120, 258 at 150).
    inline std::size_t WeaponCapacity(std::uint32_t pedCapacity)
    {
        if (pedCapacity <= kDefaultPedCapacity)
            return kDefaultWeaponCapacity;
        return (kDefaultWeaponCapacity * pedCapacity + kDefaultPedCapacity - 1) / kDefaultPedCapacity;
    }

    inline int PoolPedBudget(std::uint32_t pedCapacity)
    {
        return static_cast<int>(pedCapacity) + kPoolHandleMargin;
    }

    inline int PoolVehicleBudget(std::uint32_t vehicleCapacity)
    {
        return static_cast<int>(vehicleCapacity) + kPoolVehicleHandleMargin;
    }

    // The session's ped capacity (PedCapacity, set once at load by
    // script.cpp before the first recording).
    inline std::atomic<std::uint32_t> g_pedCapacity{kDefaultPedCapacity};

    // Clamped to the widest limit (the pool); script.cpp clamps first to
    // the limit of the session's world model.
    inline void SetPedCapacity(std::uint32_t pedCapacity)
    {
        g_pedCapacity.store(ClampPedCapacity(static_cast<int>(pedCapacity), true), std::memory_order_relaxed);
    }

    inline std::uint32_t PedCapacity()
    {
        return g_pedCapacity.load(std::memory_order_relaxed);
    }

    // The session's vehicle capacity (VehicleCapacityFor the world model,
    // set by script.cpp with the ped capacity).
    inline std::atomic<std::uint32_t> g_vehicleCapacity{kLegacyVehicleCapacity};

    inline void SetVehicleCapacity(std::uint32_t vehicleCapacity)
    {
        g_vehicleCapacity.store(vehicleCapacity < kMaxVehicleCapacity ? vehicleCapacity : kMaxVehicleCapacity,
            std::memory_order_relaxed);
    }

    inline std::uint32_t VehicleCapacity()
    {
        return g_vehicleCapacity.load(std::memory_order_relaxed);
    }

    // Read-only views of the session's value, usable where a plain constant
    // is expected (replay::recorder::kPedCapacity,
    // replay::pose::capture::kMaxLiveTargets).
    struct PedCapacityValue
    {
        operator std::uint32_t() const { return PedCapacity(); }
    };
    struct LiveTargetsValue
    {
        operator std::uint32_t() const { return LiveTargets(PedCapacity(), VehicleCapacity()); }
    };
    struct WeaponCapacityValue
    {
        operator std::size_t() const { return WeaponCapacity(PedCapacity()); }
    };

    // A bit per slot over as many 64-bit words as the slots need. Each word
    // is set and cleared atomically; Load takes a snapshot word by word (a
    // bit set or cleared during the read may or may not be seen).
    template <std::uint32_t Slots>
    struct SlotMask
    {
        static constexpr std::uint32_t kWords = (Slots + 63) / 64;
        std::atomic<std::uint64_t> words[kWords] = {};

        static constexpr std::uint32_t Word(std::uint32_t slot) { return slot >> 6; }
        static constexpr std::uint64_t Bit(std::uint32_t slot) { return 1ull << (slot & 63u); }

        void Set(std::uint32_t slot, bool on)
        {
            if (slot >= Slots)
                return;
            std::atomic<std::uint64_t>& word = words[Word(slot)];
            if (on)
                word.fetch_or(Bit(slot), std::memory_order_acq_rel);
            else
                word.fetch_and(~Bit(slot), std::memory_order_acq_rel);
        }

        // False when no bit is set (the detours' fast path).
        bool Load(std::uint64_t (&out)[kWords]) const
        {
            std::uint64_t any = 0;
            for (std::uint32_t word = 0; word < kWords; ++word)
            {
                out[word] = words[word].load(std::memory_order_acquire);
                any |= out[word];
            }
            return any != 0;
        }

        void Clear()
        {
            for (std::atomic<std::uint64_t>& word : words)
                word.store(0, std::memory_order_release);
        }
    };

    // The slots a lookup scans: one past the highest slot armed since the
    // last full disarm. Raised before a slot is published, lowered to 0
    // only once every slot is free (script thread, which arms and disarms),
    // so a scan up to it never misses an armed slot; with the lowest free
    // slot taken first it stays at the live count (unused slots cost
    // nothing).
    inline std::uint32_t RaisedScanEnd(std::uint32_t scanEnd, std::uint32_t slot)
    {
        return slot + 1 > scanEnd ? slot + 1 : scanEnd;
    }
}
