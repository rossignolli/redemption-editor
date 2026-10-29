#include "ped_audio_state.h"
#include "ped_speed_core.h"

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
    namespace
    {
        struct Signature
        {
            std::uintptr_t rva;
            const std::uint8_t* bytes;
            std::size_t size;
            const char* name;
        };

        // 0x3194FF4..0x3195058: owner +0x18, row = table + ((owner +0x9C &
        // 1FFFFh) - base) * 148h, flags of row +0xB8 (bit 0 clear, bit 1 set),
        // then the player's 0 into +0xF0 and the jump to the +0xEC store.
        constexpr std::uint8_t kDrawSignature[] = {
            0x48, 0x8B, 0x41, 0x18, 0x45, 0x33, 0xC9, 0x4C, 0x8B, 0xC1, 0x41, 0xBA, 0x01, 0x00, 0x00, 0x00, 0x48, 0x85,
            0xC0, 0x74, 0x4F, 0x8B, 0x80, 0x9C, 0x00, 0x00, 0x00, 0x25, 0xFF, 0xFF, 0x01, 0x00, 0x2B, 0x05, 0x26, 0x7E,
            0x85, 0x00, 0x48, 0x69, 0xD0, 0x48, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x05, 0x20, 0x7E, 0x85, 0x00, 0x48, 0x8B,
            0x8C, 0x02, 0xB8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD1, 0x48, 0x83, 0xE2, 0xFE, 0x48, 0xF7, 0xD9, 0x48, 0x1B,
            0xC0, 0x48, 0x23, 0xC2, 0x44, 0x84, 0x50, 0x40, 0x75, 0x12, 0xF6, 0x40, 0x40, 0x02, 0x74, 0x0C, 0x45, 0x89,
            0x88, 0xF0, 0x00, 0x00, 0x00, 0xE9, 0xBF, 0x00, 0x00, 0x00};
        // mov [r8+0ECh],r9d; ret
        constexpr std::uint8_t kDrawStoreSignature[] = {0x45, 0x89, 0x88, 0xEC, 0x00, 0x00, 0x00, 0xC3};
        // The whole of 0x3196B04: +0xEC as 0x99789761 and +0xF0 as 0xDF33D3DE.
        constexpr std::uint8_t kReaderSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x66, 0x0F, 0x6E, 0x91, 0xEC, 0x00, 0x00, 0x00,
            0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xD9, 0xC7, 0x44, 0x24, 0x30, 0x61, 0x97, 0x78, 0x99, 0x0F, 0x5B, 0xD2, 0x48,
            0x8D, 0x54, 0x24, 0x30, 0x48, 0x8B, 0xCF, 0xE8, 0x04, 0xCB, 0x3D, 0xFF, 0x66, 0x0F, 0x6E, 0x93, 0xF0, 0x00,
            0x00, 0x00, 0x48, 0x8D, 0x54, 0x24, 0x30, 0x0F, 0x5B, 0xD2, 0x48, 0x8B, 0xCF, 0xC7, 0x44, 0x24, 0x30, 0xDE,
            0xD3, 0x33, 0xDF, 0xE8, 0xE4, 0xCA, 0x3D, 0xFF, 0x48, 0x8B, 0x5C, 0x24, 0x38, 0x48, 0x83, 0xC4, 0x20, 0x5F,
            0xC3};
        // 0x318A2D8: cmp dword [rcx+0ECh],-1 / jne / call 0x3194FF4.
        constexpr std::uint8_t kActivateSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x83, 0xB9, 0xEC, 0x00, 0x00, 0x00,
            0xFF, 0x48, 0x8B, 0xF9, 0x0F, 0x29, 0x74, 0x24, 0x40, 0x75, 0x05, 0xE8, 0xFC, 0xAC, 0x00, 0x00};
        // 0x318A3EC: +0x168 empty, then +0xEC or +0xF0 minus 2 at most 1.
        constexpr std::uint8_t kActivateResourceSignature[] = {
            0x48, 0x83, 0xBF, 0x68, 0x01, 0x00, 0x00, 0x00, 0x0F, 0x85, 0xBD, 0x00, 0x00, 0x00, 0x8B, 0x87,
            0xEC, 0x00, 0x00, 0x00, 0x83, 0xE8, 0x02, 0x83, 0xF8, 0x01, 0x76, 0x12, 0x8B, 0x87, 0xF0, 0x00,
            0x00, 0x00, 0x83, 0xE8, 0x02, 0x83, 0xF8, 0x01, 0x0F, 0x87, 0x9D, 0x00, 0x00, 0x00};
        // 0x3178898 in the holder's constructor: lea rcx,[rsi+210h]; mov rdx,rsi; call 0x3178D6C.
        constexpr std::uint8_t kHolderCtorSignature[] = {
            0x48, 0x8D, 0x8E, 0x10, 0x02, 0x00, 0x00, 0x48, 0x8B, 0xD6, 0xE8, 0xC5, 0x04, 0x00, 0x00};
        // 0x3178D7C in the component's constructor: lea rax,[0x3839038].
        constexpr std::uint8_t kComponentVtableSignature[] = {0x48, 0x8D, 0x05, 0xB5, 0x02, 0x6C, 0x00};

        // 0x258A9B4 (whole): the block index word +0x12 and the table 0x53598F0.
        constexpr std::uint8_t kBlockSetterSignature[] = {
            0x48, 0x83, 0xEC, 0x28, 0x45, 0x33, 0xC0, 0x66, 0x44, 0x39, 0x41, 0x12, 0x7C, 0x14, 0x0F, 0xBF, 0x49, 0x12,
            0x48, 0x8D, 0x05, 0x23, 0xEF, 0xDC, 0x02, 0x48, 0xC1, 0xE1, 0x04, 0x48, 0x03, 0xC8, 0xEB, 0x03, 0x49, 0x8B,
            0xC8, 0x48, 0x85, 0xC9, 0x74, 0x05, 0xE8, 0xA9, 0xC6, 0xFA, 0xFF, 0x48, 0x83, 0xC4, 0x28, 0xC3};
        // 0x317AD78 (whole): the PED_VARIABLES block of the holder.
        constexpr std::uint8_t kBlockAllocSignature[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x66, 0x83, 0x79, 0x12, 0xFF, 0x48, 0x8B, 0xD9, 0x75, 0x26, 0xBA, 0xFB,
            0xD7, 0x9D, 0x54, 0xE8, 0xF2, 0xEE, 0x40, 0xFF, 0x66, 0x83, 0x7B, 0x12, 0xFF, 0x74, 0x15, 0xF3, 0x0F, 0x10,
            0x93, 0x5C, 0x46, 0x00, 0x00, 0xBA, 0x2B, 0x33, 0xAB, 0x69, 0x48, 0x8B, 0xCB, 0xE8, 0x06, 0xFC, 0x40, 0xFF,
            0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3};
        // 0x253708C (whole): descriptor count +8, names +0xC step 8, values [entry+8].
        constexpr std::uint8_t kBlockWriteSignature[] = {
            0x4C, 0x8B, 0x09, 0x33, 0xC0, 0x41, 0x39, 0x41, 0x08, 0x76, 0x1D, 0x44, 0x8B, 0xC0, 0x41, 0x39, 0x54,
            0xC1, 0x0C, 0x74, 0x09, 0xFF, 0xC0, 0x41, 0x3B, 0x41, 0x08, 0x72, 0xEE, 0xC3, 0x48, 0x8B, 0x41, 0x08,
            0xF3, 0x42, 0x0F, 0x11, 0x14, 0x80, 0xC3};

        // 0x25876A8 prologue to the first field it reads (the sound's
        // parent index +0xE2): mov rax,rsp; four saves; push r14; sub
        // rsp,20h; xor ebx,ebx; mov bpl,r8b; mov esi,edx; mov rdi,rcx; mov
        // eax,ebx; test edx,edx; jz; movzx eax,word [rcx+0E2h].
        constexpr std::uint8_t kLookupSignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78,
            0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xDB, 0x41, 0x8A, 0xE8, 0x8B, 0xF2, 0x48, 0x8B, 0xF9, 0x8B,
            0xC3, 0x85, 0xD2, 0x0F, 0x84, 0x16, 0x01, 0x00, 0x00, 0x0F, 0xB7, 0x81, 0xE2, 0x00, 0x00, 0x00};

        const Signature kVariantSignatures[] = {
            {kDrawRva, kDrawSignature, sizeof(kDrawSignature), "draw"},
            {kDrawStoreRva, kDrawStoreSignature, sizeof(kDrawStoreSignature), "draw_store"},
            {kReaderRva, kReaderSignature, sizeof(kReaderSignature), "reader"},
            {kActivateRva, kActivateSignature, sizeof(kActivateSignature), "activate"},
            {kActivateResourceRva, kActivateResourceSignature, sizeof(kActivateResourceSignature), "activate_resource"},
            {kHolderCtorComponentRva, kHolderCtorSignature, sizeof(kHolderCtorSignature), "holder_ctor"},
            {kComponentVtableLeaRva, kComponentVtableSignature, sizeof(kComponentVtableSignature), "component_vtable"},
        };
        const Signature kBlockSignatures[] = {
            {kBlockSetterRva, kBlockSetterSignature, sizeof(kBlockSetterSignature), "block_setter"},
            {kBlockAllocRva, kBlockAllocSignature, sizeof(kBlockAllocSignature), "block_alloc"},
            {kBlockWriteRva, kBlockWriteSignature, sizeof(kBlockWriteSignature), "block_write"},
        };

        std::uintptr_t g_base = 0;
        bool g_variantsReady = false;
        bool g_blocksReady = false;

        bool Read(std::uintptr_t address, void* out, std::size_t size)
        {
            return replay::mem::Copy(out, reinterpret_cast<const void*>(address), size);
        }

        bool CheckAll(const Signature* list, std::size_t n, std::string& error)
        {
            for (std::size_t i = 0; i < n; ++i)
            {
                if (!replay::mem::Matches(reinterpret_cast<const void*>(g_base + list[i].rva), list[i].bytes, list[i].size))
                {
                    if (error.empty())
                        error = std::string("ped_audio_state_signature_") + list[i].name;
                    return false;
                }
            }
            return true;
        }

        // The holder's vtable is one of the ped audio holders'.
        bool IsHolder(std::uintptr_t holder)
        {
            std::uintptr_t vtable = 0;
            if (!g_base || !holder || !Read(holder, &vtable, sizeof(vtable)))
                return false;
            for (const std::uintptr_t rva : kHolderVtableRvas)
                if (vtable == g_base + rva)
                    return true;
            return false;
        }

        // The holder is a ped audio holder and its +0x210 the variant component.
        bool ComponentOf(std::uintptr_t holder, std::uintptr_t& component)
        {
            component = 0;
            std::uintptr_t vtable = 0;
            if (!g_variantsReady || !holder || !Read(holder, &vtable, sizeof(vtable)))
                return false;
            bool known = false;
            for (const std::uintptr_t rva : kHolderVtableRvas)
                known = known || vtable == g_base + rva;
            if (!known)
                return false;
            std::uintptr_t componentVtable = 0;
            if (!Read(holder + kComponentOffset, &componentVtable, sizeof(componentVtable)) ||
                componentVtable != g_base + kComponentVtableRva)
                return false;
            component = holder + kComponentOffset;
            return true;
        }

        // ---- the read probe ----
        using LookupFn = float*(__fastcall*)(void*, std::uint32_t, std::uint8_t);
        constexpr std::uint32_t kRing = 4096;
        using Slot = replay::ring::Slot<VarRead>; // event_ring.h
        std::atomic<LookupFn> g_original{nullptr};
        std::atomic<std::int32_t> g_inFlight{0};
        void* g_target = nullptr;
        bool g_installed = false;
        Slot g_ring[kRing];
        std::atomic<std::uint64_t> g_next{0};
        std::uint64_t g_drained = 0; // script thread only
        thread_local std::int32_t t_depth = 0;

        // The ranges: written by the script thread, read by the detour. A
        // lookup racing an update may miss or misname one read (evidence).
        std::atomic<std::uintptr_t> g_rangeValues[kMaxRanges] = {};
        std::atomic<std::uint32_t> g_rangeCount[kMaxRanges] = {};
        std::atomic<std::uint32_t> g_rangeOwner[kMaxRanges] = {};
        std::atomic<std::uint32_t> g_rangeEnd{0};

        std::atomic<std::uint64_t> g_calls{0};
        std::atomic<std::uint64_t> g_outermost{0};
        std::atomic<std::uint64_t> g_inRange{0};

        // Script thread only: the last scan's blocks with names (the volume
        // probe's DescribeAddress).
        std::vector<Range> g_namedRanges;

        // ---- (c) the speed override ----
        // 0x31A3ABC up to the block alloc call: mov [rsp+18h],rbx; push rsi;
        // push rdi; push r15; sub rsp,50h; cmp word [rcx+12h],-1 (the holder's
        // block index); mov rbx,rcx; movaps [rsp+40h],xmm6; movaps
        // [rsp+30h],xmm7; jne +5; call 0x317AD78 (the block's creation).
        constexpr std::uint8_t kUpdateSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x66, 0x83, 0x79, 0x12, 0xFF,
            0x48, 0x8B, 0xD9, 0x0F, 0x29, 0x74, 0x24, 0x40, 0x0F, 0x29, 0x7C, 0x24, 0x30, 0x75, 0x05, 0xE8, 0x96, 0x72,
            0xFD, 0xFF};
        // 0x31A3F7A: mov edx,8BAE9C31h; mov rcx,rbx; call 0x258A9B4 (the
        // speed into the holder's block through the setter).
        constexpr std::uint8_t kSpeedStoreSignature[] = {
            0xBA, 0x31, 0x9C, 0xAE, 0x8B, 0x48, 0x8B, 0xCB, 0xE8, 0x2D, 0x6A, 0x3E, 0xFF};
        // 0x31A3EF4: movss xmm2,[rcx+5DCh] (the movement component's speed).
        constexpr std::uint8_t kSpeedSourceSignature[] = {0xF3, 0x0F, 0x10, 0x91, 0xDC, 0x05, 0x00, 0x00};
        static_assert(sizeof(kUpdateSignature) >= 5, "the signature covers the detour's jump");
        // The ped variables update runs from 0x31A3ABC to its ret at 0x31A4068.
        static_assert(kSpeedStoreRva > kUpdatePedVariablesRva && kSpeedStoreRva < kUpdatePedVariablesRva + 0x5AC &&
                kSpeedSourceRva > kUpdatePedVariablesRva && kSpeedSourceRva < kSpeedStoreRva,
            "the speed's source and store are inside UpdatePedVariables");

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
            if (!holder || !Read(holder + kHolderBlockIndexOffset, &index, sizeof(index)) || index < 0 || index > kMaxBlockIndex)
                return 0;
            const std::uintptr_t entry = g_base + kBlockTableRva + static_cast<std::uintptr_t>(index) * kBlockEntryBytes;
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

        void Push(const VarRead& read)
        {
            replay::ring::Writer<VarRead> slot(g_ring, kRing, g_next);
            slot.value = read;
            slot.Publish();
        }

        float* __fastcall LookupDetour(void* sound, std::uint32_t name, std::uint8_t flag)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            struct InFlight
            {
                InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            const LookupFn original = g_original.load(std::memory_order_acquire);
            g_calls.fetch_add(1, std::memory_order_relaxed);
            // A child sound asks its parent through the same slot: only the
            // outermost lookup is the sound's own question.
            const bool outermost = t_depth == 0;
            ++t_depth;
            float* result = original ? original(sound, name, flag) : nullptr;
            --t_depth;
            if (!outermost)
                return result;
            g_outermost.fetch_add(1, std::memory_order_relaxed);
            const std::uint32_t end = g_rangeEnd.load(std::memory_order_acquire);
            if (!result || !end)
                return result;
            const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(result);
            for (std::uint32_t index = 0; index < end && index < kMaxRanges; ++index)
            {
                const std::uintptr_t values = g_rangeValues[index].load(std::memory_order_relaxed);
                const std::uint32_t count = g_rangeCount[index].load(std::memory_order_relaxed);
                if (!values || address < values || address >= values + count * sizeof(float) ||
                    ((address - values) % sizeof(float)) != 0)
                    continue;
                g_inRange.fetch_add(1, std::memory_order_relaxed);
                VarRead read;
                read.name = name;
                read.owner = g_rangeOwner[index].load(std::memory_order_relaxed);
                read.slot = static_cast<std::uint32_t>((address - values) / sizeof(float));
                Read(address, &read.value, sizeof(read.value));
                read.thread = GetCurrentThreadId();
                Push(read);
                break;
            }
            return result;
        }
    }

    bool Init(std::string& error)
    {
        error.clear();
        g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
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

    bool WriteVariants(std::uintptr_t holder, const Variants& v)
    {
        std::uintptr_t component = 0;
        if (!ComponentOf(holder, component) || !v.Drawn() || !v.Plausible())
            return false;
        // Two aligned dwords: the audio thread reads either the old or the
        // new value of each (0x3196B04 reads them one by one anyway).
        return replay::mem::Put(component + kVariantBOffset, &v.b, sizeof(v.b)) &&
            replay::mem::Put(component + kVariantAOffset, &v.a, sizeof(v.a));
    }

    bool ReadHolderBlock(std::uintptr_t holder, Block& out)
    {
        out = Block{};
        if (!g_blocksReady)
            return false;
        std::uintptr_t vtable = 0;
        if (!holder || !Read(holder, &vtable, sizeof(vtable)))
            return false;
        bool known = false;
        for (const std::uintptr_t rva : kHolderVtableRvas)
            known = known || vtable == g_base + rva;
        return known && ReadBlock(g_base, holder, Read, out);
    }

    bool InstallReadProbe(std::string& error)
    {
        if (g_installed)
            return true;
        if (!g_base)
            g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        void* target = reinterpret_cast<void*>(g_base + kVariableLookupRva);
        if (!replay::mem::Matches(target, kLookupSignature, sizeof(kLookupSignature)))
        {
            error = "ped_variables_lookup_signature_mismatch";
            return false;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&LookupDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            return false;
        }
        g_original.store(reinterpret_cast<LookupFn>(trampoline), std::memory_order_release);
        status = replay::hooks::Enable(target);
        if (status != MH_OK)
        {
            MH_RemoveHook(target);
            g_original.store(nullptr, std::memory_order_release);
            error = std::string("mh_enable_") + MH_StatusToString(status);
            return false;
        }
        g_target = target;
        g_installed = true;
        return true;
    }

    void ShutdownReadProbe()
    {
        g_rangeEnd.store(0, std::memory_order_release);
        // Disable, wait for the calls in flight, then remove (MH_RemoveHook
        // frees the trampoline a call in flight may still run).
        if (g_installed && g_target)
            MH_DisableHook(g_target);
        for (int wait = 0; wait < 200 && g_inFlight.load(std::memory_order_seq_cst) > 0; ++wait)
            Sleep(1);
        if (g_installed && g_target)
        {
            MH_RemoveHook(g_target);
            g_installed = false;
            g_target = nullptr;
        }
        g_original.store(nullptr, std::memory_order_release);
    }

    bool ReadProbeInstalled() { return g_installed; }

    void SetRanges(const std::vector<Range>& ranges)
    {
        const std::uint32_t oldEnd = g_rangeEnd.load(std::memory_order_relaxed);
        std::uint32_t count = 0;
        for (const Range& range : ranges)
        {
            if (count >= kMaxRanges)
                break;
            if (!range.values || !range.count || range.count > kMaxBlockVariables)
                continue;
            g_rangeValues[count].store(range.values, std::memory_order_relaxed);
            g_rangeCount[count].store(range.count, std::memory_order_relaxed);
            g_rangeOwner[count].store(range.owner, std::memory_order_relaxed);
            ++count;
        }
        for (std::uint32_t index = count; index < oldEnd && index < kMaxRanges; ++index)
        {
            g_rangeValues[index].store(0, std::memory_order_relaxed);
            g_rangeCount[index].store(0, std::memory_order_relaxed);
        }
        g_rangeEnd.store(count, std::memory_order_release);
    }

    void DrainReads(std::vector<VarRead>& out, std::uint64_t& dropped)
    {
        out.clear();
        // A read being written stops the drain: the next drain takes it.
        const replay::ring::Drained drained =
            replay::ring::ForEachSince(g_ring, kRing, g_next, g_drained, [&](const VarRead& value) { out.push_back(value); });
        dropped = drained.overflowed + drained.lost;
    }

    ReadStats ReadSnapshot()
    {
        ReadStats stats;
        stats.calls = g_calls.load(std::memory_order_relaxed);
        stats.outermost = g_outermost.load(std::memory_order_relaxed);
        stats.inRange = g_inRange.load(std::memory_order_relaxed);
        return stats;
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
        if (!g_base)
            g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (!g_blocksReady)
        {
            error = "blocks_not_ready";
            return false;
        }
        void* target = reinterpret_cast<void*>(g_base + kUpdatePedVariablesRva);
        if (!replay::mem::Matches(target, kUpdateSignature, sizeof(kUpdateSignature)))
        {
            error = "update_ped_variables_signature_mismatch";
            return false;
        }
        if (!replay::mem::Matches(reinterpret_cast<const void*>(g_base + kSpeedStoreRva), kSpeedStoreSignature,
                sizeof(kSpeedStoreSignature)) ||
            !replay::mem::Matches(reinterpret_cast<const void*>(g_base + kSpeedSourceRva), kSpeedSourceSignature,
                sizeof(kSpeedSourceSignature)))
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
        if (!g_base)
            g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        void* target = reinterpret_cast<void*>(g_base + kMotionSpeedSourceRva);
        if (!replay::mem::Matches(target, kMotionSpeedSourceSignature, sizeof(kMotionSpeedSourceSignature)))
        {
            error = "motion_speed_signature_mismatch";
            return false;
        }
        // The callers this changes: the only writer of +0x5DC, the sound
        // 0x31A15DC; and the gait classifier's read of +0x5DC.
        if (!replay::mem::Matches(reinterpret_cast<const void*>(g_base + kMotionSpeedWriterRva), kMotionSpeedWriterSignature,
                sizeof(kMotionSpeedWriterSignature)) ||
            !replay::mem::Matches(reinterpret_cast<const void*>(g_base + kMotionSpeedSoundRva), kMotionSpeedSoundSignature,
                sizeof(kMotionSpeedSoundSignature)) ||
            !replay::mem::Matches(reinterpret_cast<const void*>(g_base + kMotionGaitReadRva), kMotionGaitReadSignature,
                sizeof(kMotionGaitReadSignature)))
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
