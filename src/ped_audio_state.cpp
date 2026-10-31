#include "ped_audio_state.h"
#include "engine_sigs.h"
#include "ped_speed_core.h"

#include "game_sig.h"
#include "guarded_memory.h"
#include "hook_batch.h"
#include "module_lifetime.h"
#include "event_ring.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstring>

// The engine side (ped_audio_state.h) of the puppets' foley variants and of the ped variable
// evidence. The variants and the blocks are read and written on the script
// thread only (guarded); the read probe is a detour on the audio threads that
// never changes the answer, publishes nothing but a ring, and calls no native.
namespace replay::pedaudiostate
{
    using enginesig::kAudioHolderCtor;
    using enginesig::kAudioHolderVtable;
    using enginesig::kAudioHolderVtableA;
    using enginesig::kAudioHolderVtableB;

    namespace
    {
        namespace sig = replay::sig;

        // ---- (a) the foley variants ----
        // The activation (component slot +0x68): cmp dword [rcx+0ECh],-1;
        // jne; call the draw.
        const sig::Signature kActivate{"pedaudio_activate",
            "48 89 5C 24 18 57 48 83 EC 50 83 B9 EC 00 00 00 FF 48 8B F9 0F 29 74 24 40 75 05 E8 ?? ?? ?? ??", 0x318A2D8};
        // The draw (a leaf): owner +0x18, row = table + ((owner +0x9C &
        // 1FFFFh) - base index) * 148h, flags of row +0xB8 (bit 0 clear, bit 1 set),
        // then the player's 0 into +0xF0 and the jump to the +0xEC store
        // (mov [r8+0ECh],r9d; ret).
        const sig::Signature kDraw{"pedaudio_draw", kActivate, sig::Rel32{0x1C, 0x20},
            "48 8B 41 18 45 33 C9 4C 8B C1 41 BA 01 00 00 00 48 85 C0 74 4F 8B 80 9C 00 00 00 25 FF FF 01 00 2B 05 ?? ?? "
            "?? ?? 48 69 D0 48 01 00 00 48 8B 05 ?? ?? ?? ?? 48 8B 8C 02 B8 00 00 00 48 8B D1 48 83 E2 FE 48 F7 D9 48 1B "
            "C0 48 23 C2 44 84 50 40 75 12 F6 40 40 02 74 0C 45 89 88 F0 00 00 00 E9 ?? ?? ?? ??",
            0x3194FF4};
        const sig::Signature kDrawStore{"pedaudio_draw_store", kDraw, sig::Rel32{0x60, 0x64}, "45 89 88 EC 00 00 00 C3",
            0x3195117};
        // In the activation: +0x168 empty, then +0xEC or +0xF0 minus 2 at most 1.
        const sig::Signature kActivateResource{"pedaudio_activate_resource", kActivate, sig::Offset{0x114},
            "48 83 BF 68 01 00 00 00 0F 85 ?? ?? ?? ?? 8B 87 EC 00 00 00 83 E8 02 83 F8 01 76 12 8B 87 F0 00 00 00 83 E8 "
            "02 83 F8 01 0F 87 ?? ?? ?? ??",
            0x318A3EC};
        // The reader (whole): +0xEC as 0x99789761 and +0xF0 as 0xDF33D3DE.
        const sig::Signature kReader{"pedaudio_reader",
            "48 89 5C 24 10 57 48 83 EC 20 66 0F 6E 91 EC 00 00 00 48 8B FA 48 8B D9 C7 44 24 30 61 97 78 99 0F 5B D2 48 "
            "8D 54 24 30 48 8B CF E8 ?? ?? ?? ?? 66 0F 6E 93 F0 00 00 00 48 8D 54 24 30 0F 5B D2 48 8B CF C7 44 24 30 DE "
            "D3 33 DF E8 ?? ?? ?? ?? 48 8B 5C 24 38 48 83 C4 20 5F C3",
            0x3196B04};
        // The holder's constructor (engine_sigs.h): at +0x38 lea rcx,[rsi+210h];
        // mov rdx,rsi; call the component's constructor, whose lea rax at +0x10
        // is the component's vtable.
        const sig::Signature kHolderCtorComponent{"pedaudio_holder_ctor_component", kAudioHolderCtor, sig::Offset{0x38},
            "48 8D 8E 10 02 00 00 48 8B D6 E8 ?? ?? ?? ??", 0x3178898};
        const sig::Signature kComponentCtor{"pedaudio_component_ctor", kHolderCtorComponent, sig::Rel32{0x0B, 0x0F},
            "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 33 C9", 0x3178D6C};
        const sig::Signature kComponentVtableLea{"pedaudio_component_vtable_lea", kComponentCtor, sig::Offset{0x10},
            "48 8D 05 ?? ?? ?? ??", 0x3178D7C};
        const sig::Signature kComponentVtable{"pedaudio_component_vtable", kComponentVtableLea, sig::Rel32{3, 7}, "",
            0x3839038};
        // ---- (b) the ped variables ----
        // The holder's PED_VARIABLES block (whole): made when +0x12 is -1,
        // then holder_465c through the block setter.
        const sig::Signature kBlockAlloc{"pedaudio_block_alloc",
            "40 53 48 83 EC 20 66 83 79 12 FF 48 8B D9 75 26 BA FB D7 9D 54 E8 ?? ?? ?? ?? 66 83 7B 12 FF 74 15 F3 0F 10 "
            "93 5C 46 00 00 BA 2B 33 AB 69 48 8B CB E8 ?? ?? ?? ?? 48 83 C4 20 5B C3",
            0x317AD78};
        // The block setter (whole): the block index word +0x12, an entry of 16
        // bytes in the block table, then the block write.
        const sig::Signature kBlockSetter{"pedaudio_block_setter", kBlockAlloc, sig::Rel32{0x32, 0x36},
            "48 83 EC 28 45 33 C0 66 44 39 41 12 7C 14 0F BF 49 12 48 8D 05 ?? ?? ?? ?? 48 C1 E1 04 48 03 C8 EB 03 49 8B "
            "C8 48 85 C9 74 05 E8 ?? ?? ?? ?? 48 83 C4 28 C3",
            0x258A9B4};
        const sig::Signature kBlockTable{"pedaudio_block_table", kBlockSetter, sig::Rel32{0x15, 0x19}, "", 0x53598F0};
        // The block write (whole): descriptor count +8, names +0xC step 8,
        // values [entry+8].
        const sig::Signature kBlockWrite{"pedaudio_block_write", kBlockSetter, sig::Rel32{0x2B, 0x2F},
            "4C 8B 09 33 C0 41 39 41 08 76 1D 44 8B C0 41 39 54 C1 0C 74 09 FF C0 41 3B 41 08 72 EE C3 48 8B 41 08 F3 42 "
            "0F 11 14 80 C3",
            0x253708C};

        // A signature Init needs and the name its failure gives the error (a
        // parent before what is reached from it: the first failure is the
        // cause).
        struct Required
        {
            const sig::Signature* signature;
            const char* name;
        };
        const Required kVariantSignatures[] = {
            {&kActivate, "activate"},
            {&kDraw, "draw"},
            {&kDrawStore, "draw_store"},
            {&kReader, "reader"},
            {&kActivateResource, "activate_resource"},
            {&kHolderCtorComponent, "holder_ctor"},
            {&kAudioHolderVtable, "holder_ctor"},
            {&kAudioHolderVtableA, "holder_ctor"},
            {&kAudioHolderVtableB, "holder_ctor"},
            {&kComponentVtable, "component_vtable"},
        };
        // The holders' vtables too: ReadHolderBlock knows a holder by them.
        const Required kBlockSignatures[] = {
            {&kBlockAlloc, "block_alloc"},
            {&kBlockSetter, "block_setter"},
            {&kBlockTable, "block_setter"},
            {&kBlockWrite, "block_write"},
            {&kAudioHolderVtable, "holder_ctor"},
            {&kAudioHolderVtableA, "holder_ctor"},
            {&kAudioHolderVtableB, "holder_ctor"},
        };

        // Where the engine's code has them (Init; 0: not in this game).
        std::uintptr_t g_holderVtables[3] = {};
        std::uintptr_t g_componentVtable = 0;
        std::uintptr_t g_blockTable = 0;
        bool g_variantsReady = false;
        bool g_blocksReady = false;

        bool Read(std::uintptr_t address, void* out, std::size_t size)
        {
            return replay::mem::Copy(out, reinterpret_cast<const void*>(address), size);
        }

        bool CheckAll(const Required* list, std::size_t n, std::string& error)
        {
            for (std::size_t i = 0; i < n; ++i)
            {
                if (!sig::Find(*list[i].signature))
                {
                    if (error.empty())
                        error = std::string("ped_audio_state_signature_") + list[i].name;
                    return false;
                }
            }
            return true;
        }

        // One of the ped audio holders' vtables.
        bool HolderVtable(std::uintptr_t vtable)
        {
            for (const std::uintptr_t known : g_holderVtables)
                if (known && vtable == known)
                    return true;
            return false;
        }

        // The holder's vtable is one of the ped audio holders'.
        bool IsHolder(std::uintptr_t holder)
        {
            std::uintptr_t vtable = 0;
            if (!holder || !Read(holder, &vtable, sizeof(vtable)))
                return false;
            return HolderVtable(vtable);
        }

        // The holder is a ped audio holder and its +0x210 the variant component.
        bool ComponentOf(std::uintptr_t holder, std::uintptr_t& component)
        {
            component = 0;
            std::uintptr_t vtable = 0;
            if (!g_variantsReady || !holder || !Read(holder, &vtable, sizeof(vtable)) || !HolderVtable(vtable))
                return false;
            std::uintptr_t componentVtable = 0;
            if (!g_componentVtable || !Read(holder + kComponentOffset, &componentVtable, sizeof(componentVtable)) ||
                componentVtable != g_componentVtable)
                return false;
            component = holder + kComponentOffset;
            return true;
        }

        // Script thread only: the last scan's blocks with names (the volume
        // probe's DescribeAddress).
        std::vector<Range> g_namedRanges;

        // ---- (c) the speed override (its signatures: ped_audio_state.h) ----
        using UpdateFn = void(__fastcall*)(void*);
        std::atomic<UpdateFn> g_updateOriginal{nullptr};
        void* g_updateTarget = nullptr;
        std::atomic<std::int32_t> g_updateInFlight{0};

        // Published by the script thread, read by the detour. A slot's
        // holder is cleared before it changes and set after its speed, so
        // the detour reads either nothing or a holder with its own speed
        // (a slot reused in the same publish can give the leaving holder,
        // a removed puppet's, the new value once).
        std::atomic<std::uintptr_t> g_speedHolder[kMaxSpeedTargets] = {};
        std::atomic<std::uint32_t> g_speedBits[kMaxSpeedTargets] = {};
        std::atomic<std::uint32_t> g_speedEngineBits[kMaxSpeedTargets] = {};
        std::atomic<std::uint8_t> g_speedEngineSeen[kMaxSpeedTargets] = {};
        std::atomic<std::uint32_t> g_speedEnd{0};
        pedspeed::SlotTable<kMaxSpeedTargets> g_speedSlots; // script thread's side

        std::atomic<std::uint64_t> g_speedCalls{0};
        std::atomic<std::uint64_t> g_speedHits{0};
        std::atomic<std::uint64_t> g_speedWrites{0};
        std::atomic<std::uint64_t> g_speedFailed{0};

        std::uint32_t Bits(float v)
        {
            std::uint32_t b = 0;
            static_assert(sizeof(b) == sizeof(v), "float bits");
            std::memcpy(&b, &v, sizeof(b));
            return b;
        }
        float FromBits(std::uint32_t b)
        {
            float v = 0.0f;
            std::memcpy(&v, &b, sizeof(v));
            return v;
        }

        // The address of the speed in the holder's block (guarded reads).
        std::uintptr_t SpeedAddress(std::uintptr_t holder)
        {
            std::int16_t index = -1;
            if (!holder || !g_blockTable || !Read(holder + kHolderBlockIndexOffset, &index, sizeof(index)) || index < 0 ||
                index > kMaxBlockIndex)
                return 0;
            const std::uintptr_t entry = g_blockTable + static_cast<std::uintptr_t>(index) * kBlockEntryBytes;
            std::uintptr_t descriptor = 0;
            std::uintptr_t values = 0;
            if (!Read(entry, &descriptor, sizeof(descriptor)) || !descriptor || !Read(entry + kBlockValuesOffset, &values, sizeof(values)) ||
                !values)
                return 0;
            // The names in one guarded copy (stride 8, the name in the low
            // dword), searched every time: no state shared between threads.
            std::uint32_t count = 0;
            std::uint8_t names[kMaxBlockVariables * kDescriptorNameStride] = {};
            if (!Read(descriptor + kDescriptorCountOffset, &count, sizeof(count)) || count == 0 || count > kMaxBlockVariables ||
                !Read(descriptor + kDescriptorNamesOffset, names, (count - 1) * kDescriptorNameStride + sizeof(std::uint32_t)))
                return 0;
            for (std::uint32_t n = 0; n < count; ++n)
            {
                std::uint32_t name = 0;
                std::memcpy(&name, names + n * kDescriptorNameStride, sizeof(name));
                if (name == pedspeed::kSpeedVariable)
                    return values + static_cast<std::uintptr_t>(n) * sizeof(float);
            }
            return 0;
        }

        void __fastcall UpdatePedVariablesDetour(void* holder)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            struct InFlight
            {
                InFlight() { g_updateInFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_updateInFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            const UpdateFn original = g_updateOriginal.load(std::memory_order_acquire);
            if (original)
                original(holder);
            g_speedCalls.fetch_add(1, std::memory_order_relaxed);
            const std::uint32_t end = g_speedEnd.load(std::memory_order_acquire);
            const std::uintptr_t h = reinterpret_cast<std::uintptr_t>(holder);
            if (!end || !h)
                return;
            for (std::uint32_t i = 0; i < end && i < kMaxSpeedTargets; ++i)
            {
                if (g_speedHolder[i].load(std::memory_order_acquire) != h)
                    continue;
                g_speedHits.fetch_add(1, std::memory_order_relaxed);
                const float value = FromBits(g_speedBits[i].load(std::memory_order_relaxed));
                const std::uintptr_t at = SpeedAddress(h);
                float engine = 0.0f;
                if (at && Read(at, &engine, sizeof(engine)) && replay::mem::Put(at, &value, sizeof(value)))
                {
                    g_speedEngineBits[i].store(Bits(engine), std::memory_order_relaxed);
                    g_speedEngineSeen[i].store(1, std::memory_order_release);
                    g_speedWrites.fetch_add(1, std::memory_order_relaxed);
                }
                else
                    g_speedFailed.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }

        // ---- (d) the movement component's speed (MotionSpeedReplay) ----
        using MotionSourceFn = float(__fastcall*)(void*);
        std::atomic<MotionSourceFn> g_motionOriginal{nullptr};
        void* g_motionTarget = nullptr;
        std::atomic<std::int32_t> g_motionInFlight{0};
        // Published by the script thread (the SlotTable order of (c)): a
        // slot's ped is cleared before it changes and set after its speed.
        std::atomic<std::uintptr_t> g_motionPed[kMaxMotionTargets] = {};
        std::atomic<std::uint32_t> g_motionBits[kMaxMotionTargets] = {};
        std::atomic<std::uint32_t> g_motionEngineBits[kMaxMotionTargets] = {};
        std::atomic<std::uint8_t> g_motionEngineSeen[kMaxMotionTargets] = {};
        std::atomic<std::uint32_t> g_motionEnd{0};
        pedspeed::SlotTable<kMaxMotionTargets> g_motionSlots; // script thread's side
        std::atomic<std::uint64_t> g_motionCalls{0};
        std::atomic<std::uint64_t> g_motionHits{0};
        std::atomic<std::uint64_t> g_motionFailed{0};

        float __fastcall MotionSourceDetour(void* component)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            struct InFlight
            {
                InFlight() { g_motionInFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_motionInFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            const MotionSourceFn original = g_motionOriginal.load(std::memory_order_acquire);
            const float raw = original ? original(component) : 0.0f;
            g_motionCalls.fetch_add(1, std::memory_order_relaxed);
            const std::uint32_t end = g_motionEnd.load(std::memory_order_acquire);
            if (!end || !component)
                return raw;
            // The component's ped, as the original reads it (mov rbx,[rcx+8]; and rbx,-10h).
            std::uintptr_t ped = 0;
            if (!Read(reinterpret_cast<std::uintptr_t>(component) + 8, &ped, sizeof(ped)))
            {
                g_motionFailed.fetch_add(1, std::memory_order_relaxed);
                return raw;
            }
            ped &= ~static_cast<std::uintptr_t>(0xF);
            if (!ped)
                return raw;
            for (std::uint32_t i = 0; i < end && i < kMaxMotionTargets; ++i)
            {
                if (g_motionPed[i].load(std::memory_order_acquire) != ped)
                    continue;
                g_motionHits.fetch_add(1, std::memory_order_relaxed);
                g_motionEngineBits[i].store(Bits(raw), std::memory_order_relaxed);
                g_motionEngineSeen[i].store(1, std::memory_order_release);
                return FromBits(g_motionBits[i].load(std::memory_order_relaxed));
            }
            return raw;
        }
    }

    bool Init(std::string& error)
    {
        error.clear();
        // The vtables and the block table where the engine's code has them.
        g_holderVtables[0] = sig::Find(kAudioHolderVtable);
        g_holderVtables[1] = sig::Find(kAudioHolderVtableA);
        g_holderVtables[2] = sig::Find(kAudioHolderVtableB);
        g_componentVtable = sig::Find(kComponentVtable);
        g_blockTable = sig::Find(kBlockTable);
        g_variantsReady = CheckAll(kVariantSignatures, sizeof(kVariantSignatures) / sizeof(kVariantSignatures[0]), error);
        g_blocksReady = CheckAll(kBlockSignatures, sizeof(kBlockSignatures) / sizeof(kBlockSignatures[0]), error);
        return g_variantsReady && g_blocksReady;
    }

    bool VariantsReady() { return g_variantsReady; }
    bool BlocksReady() { return g_blocksReady; }

    bool ReadVariants(std::uintptr_t holder, Variants& out, bool& resourceHeld)
    {
        out = Variants{};
        resourceHeld = false;
        std::uintptr_t component = 0;
        if (!ComponentOf(holder, component))
            return false;
        std::uintptr_t resource = 0;
        if (!Read(component + kVariantAOffset, &out.a, sizeof(out.a)) ||
            !Read(component + kVariantBOffset, &out.b, sizeof(out.b)) ||
            !Read(component + kResourceOffset, &resource, sizeof(resource)))
            return false;
        resourceHeld = resource != 0;
        return true;
    }

    bool ReadHolderBlock(std::uintptr_t holder, Block& out)
    {
        out = Block{};
        if (!g_blocksReady)
            return false;
        std::uintptr_t vtable = 0;
        if (!holder || !Read(holder, &vtable, sizeof(vtable)))
            return false;
        return HolderVtable(vtable) && ReadBlock(g_blockTable, holder, Read, out);
    }

    void SetNamedRanges(const std::vector<Range>& ranges) { g_namedRanges = ranges; }

    bool DescribeAddress(std::uintptr_t address, std::uint32_t& owner, std::uint32_t& name)
    {
        for (const Range& r : g_namedRanges)
        {
            if (!r.values || address < r.values || address >= r.values + r.count * sizeof(float) ||
                ((address - r.values) % sizeof(float)) != 0)
                continue;
            const std::size_t slot = (address - r.values) / sizeof(float);
            if (slot >= kMaxBlockVariables)
                return false;
            owner = r.owner;
            name = r.names[slot];
            return true;
        }
        return false;
    }

    bool InstallSpeedOverride(std::string& error)
    {
        if (g_updateTarget)
            return true;
        if (!g_blocksReady)
        {
            error = "blocks_not_ready";
            return false;
        }
        void* target = reinterpret_cast<void*>(sig::Find(kUpdatePedVariables));
        if (!target)
        {
            error = "update_ped_variables_signature_mismatch";
            return false;
        }
        if (!sig::Find(kSpeedStore) || !sig::Find(kSpeedSource))
        {
            error = "speed_store_signature_mismatch";
            return false;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&UpdatePedVariablesDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            return false;
        }
        g_updateOriginal.store(reinterpret_cast<UpdateFn>(trampoline), std::memory_order_release);
        status = replay::hooks::Enable(target);
        if (status != MH_OK)
        {
            MH_RemoveHook(target);
            g_updateOriginal.store(nullptr, std::memory_order_release);
            error = std::string("mh_enable_") + MH_StatusToString(status);
            return false;
        }
        g_updateTarget = target;
        return true;
    }

    void ShutdownSpeedOverride()
    {
        SetSpeedTargets({});
        // Disable, wait for the calls in flight, then remove (the trampoline
        // goes with MH_RemoveHook).
        if (g_updateTarget)
            MH_DisableHook(g_updateTarget);
        for (int wait = 0; wait < 200 && g_updateInFlight.load(std::memory_order_seq_cst) > 0; ++wait)
            Sleep(1);
        if (g_updateTarget)
        {
            MH_RemoveHook(g_updateTarget);
            g_updateTarget = nullptr;
        }
        g_updateOriginal.store(nullptr, std::memory_order_release);
    }

    bool SpeedOverrideInstalled() { return g_updateTarget != nullptr; }

    void SetSpeedTargets(const std::vector<SpeedTarget>& targets)
    {
        std::vector<std::uintptr_t> holders;
        std::vector<float> speeds;
        holders.reserve(targets.size());
        speeds.reserve(targets.size());
        for (const SpeedTarget& t : targets)
        {
            holders.push_back(t.holder);
            speeds.push_back(t.speed);
        }
        const std::uint32_t end = g_speedSlots.Apply(holders.data(), speeds.data(), holders.size(),
            [](std::uintptr_t holder) { return IsHolder(holder); },
            [](std::uint32_t slot) {
                g_speedHolder[slot].store(0, std::memory_order_release);
                g_speedEngineSeen[slot].store(0, std::memory_order_relaxed);
            },
            [](std::uint32_t slot, float value) { g_speedBits[slot].store(Bits(value), std::memory_order_relaxed); },
            [](std::uint32_t slot, std::uintptr_t holder, float value) {
                g_speedEngineSeen[slot].store(0, std::memory_order_relaxed);
                g_speedBits[slot].store(Bits(value), std::memory_order_relaxed);
                g_speedHolder[slot].store(holder, std::memory_order_release);
            });
        g_speedEnd.store(end, std::memory_order_release);
    }

    bool EngineSpeedOf(std::uintptr_t holder, float& out)
    {
        const std::uint32_t slot = g_speedSlots.SlotOf(holder);
        if (slot >= kMaxSpeedTargets || !g_speedEngineSeen[slot].load(std::memory_order_acquire))
            return false;
        out = FromBits(g_speedEngineBits[slot].load(std::memory_order_relaxed));
        return true;
    }

    bool ReadHolderSpeed(std::uintptr_t holder, float& out)
    {
        Block block;
        return ReadHolderBlock(holder, block) && ValueOf(block, pedspeed::kSpeedVariable, out);
    }

    SpeedCounts SpeedSnapshot()
    {
        SpeedCounts c;
        c.calls = g_speedCalls.load(std::memory_order_relaxed);
        c.hits = g_speedHits.load(std::memory_order_relaxed);
        c.writes = g_speedWrites.load(std::memory_order_relaxed);
        c.failed = g_speedFailed.load(std::memory_order_relaxed);
        return c;
    }

    // ---- (d) MotionSpeedReplay ----
    bool InstallMotionSpeed(std::string& error)
    {
        if (g_motionTarget)
            return true;
        void* target = reinterpret_cast<void*>(sig::Find(kMotionSpeedSource));
        if (!target)
        {
            error = "motion_speed_signature_mismatch";
            return false;
        }
        // The callers this changes: the only writer of +0x5DC (its calls: the
        // raw speed, then the smoother), the sound's call of it; and the gait
        // classifier's read of +0x5DC.
        if (!sig::Find(kMotionSpeedWriter) || !sig::Find(kMotionWriterSource) || !sig::Find(kMotionSmoother) ||
            !sig::Find(kMotionSpeedSound) || !sig::Find(kMotionSoundSource) || !sig::Find(kMotionGaitRead))
        {
            error = "motion_speed_site_mismatch";
            return false;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&MotionSourceDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            return false;
        }
        g_motionOriginal.store(reinterpret_cast<MotionSourceFn>(trampoline), std::memory_order_release);
        status = replay::hooks::Enable(target);
        if (status != MH_OK)
        {
            MH_RemoveHook(target);
            g_motionOriginal.store(nullptr, std::memory_order_release);
            error = std::string("mh_enable_") + MH_StatusToString(status);
            return false;
        }
        g_motionTarget = target;
        return true;
    }

    void ShutdownMotionSpeed()
    {
        SetMotionTargets({});
        // Disable, wait for the calls in flight, then remove (the order of (c)).
        if (g_motionTarget)
            MH_DisableHook(g_motionTarget);
        for (int wait = 0; wait < 200 && g_motionInFlight.load(std::memory_order_seq_cst) > 0; ++wait)
            Sleep(1);
        if (g_motionTarget)
        {
            MH_RemoveHook(g_motionTarget);
            g_motionTarget = nullptr;
        }
        g_motionOriginal.store(nullptr, std::memory_order_release);
    }

    bool MotionSpeedInstalled() { return g_motionTarget != nullptr; }

    void SetMotionTargets(const std::vector<MotionTarget>& targets)
    {
        std::vector<std::uintptr_t> peds;
        std::vector<float> speeds;
        peds.reserve(targets.size());
        speeds.reserve(targets.size());
        for (const MotionTarget& t : targets)
        {
            peds.push_back(t.ped);
            speeds.push_back(t.speed);
        }
        const std::uint32_t end = g_motionSlots.Apply(peds.data(), speeds.data(), peds.size(),
            [](std::uintptr_t ped) { return ped != 0 && (ped & 0xF) == 0; },
            [](std::uint32_t slot) {
                g_motionPed[slot].store(0, std::memory_order_release);
                g_motionEngineSeen[slot].store(0, std::memory_order_relaxed);
            },
            [](std::uint32_t slot, float value) { g_motionBits[slot].store(Bits(value), std::memory_order_relaxed); },
            [](std::uint32_t slot, std::uintptr_t ped, float value) {
                g_motionEngineSeen[slot].store(0, std::memory_order_relaxed);
                g_motionBits[slot].store(Bits(value), std::memory_order_relaxed);
                g_motionPed[slot].store(ped, std::memory_order_release);
            });
        g_motionEnd.store(end, std::memory_order_release);
    }

    bool EngineMotionOf(std::uintptr_t ped, float& out)
    {
        const std::uint32_t slot = g_motionSlots.SlotOf(ped);
        if (slot >= kMaxMotionTargets || !g_motionEngineSeen[slot].load(std::memory_order_acquire))
            return false;
        out = FromBits(g_motionEngineBits[slot].load(std::memory_order_relaxed));
        return true;
    }

    MotionCounts MotionSnapshot()
    {
        MotionCounts c;
        c.calls = g_motionCalls.load(std::memory_order_relaxed);
        c.hits = g_motionHits.load(std::memory_order_relaxed);
        c.failed = g_motionFailed.load(std::memory_order_relaxed);
        return c;
    }
}
