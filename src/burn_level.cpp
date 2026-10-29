#include "burn_level.h"
#include "guarded_memory.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace replay::burn
{
    namespace
    {
        // Entity +0x28: draw handler; +0x30: entity type byte (4 = ped).
        constexpr std::uintptr_t kEntityDrawHandler = 0x28;
        constexpr std::uintptr_t kEntityType = 0x30;
        constexpr std::uint8_t kTypePed = 4;
        // Draw handler +0x18: shader effect (fire path); ped draw handler
        // +0x748 -> +0xC0: the ped's effect (resolver 0x558244).
        constexpr std::uintptr_t kHandlerEffect = 0x18;
        constexpr std::uintptr_t kHandlerPedData = 0x748;
        constexpr std::uintptr_t kPedDataEffect = 0xC0;
        // Effect +0x18: instance block; vtable slot 17 (+0x88) returns it.
        constexpr std::uintptr_t kEffectBlock = 0x18;
        constexpr std::size_t kBlockGetterSlot = 0x88 / 8;
        constexpr std::uintptr_t kBlockLevel = 0x1C;
        constexpr std::uintptr_t kBlockLevelNoAsh = 0x20;
        constexpr std::uintptr_t kBlockIntensity = 0x24;
        constexpr std::uintptr_t kBlockOpacity = 0x28;

        // The block getter every effect class with a block has at +0x88:
        // `jmp 0x6FCB3C1` (the protected text), where `mov rax, [rcx+0x18]`.
        constexpr std::uintptr_t kBlockGetterRva = 0x3090A4;
        constexpr std::uint8_t kBlockGetterSignature[] = {0xE9, 0x18, 0x23, 0xCC, 0x06};
        constexpr std::uintptr_t kBlockGetterBodyRva = 0x6FCB3C1;
        constexpr std::uint8_t kBlockGetterBodySignature[] = {0x48, 0x8B, 0x41, 0x18};
        // Level setter and getter (movss [rcx+0x1C], xmm1 / movss xmm0, [rcx+0x1C]).
        constexpr std::uintptr_t kLevelSetterRva = 0x6FD2E4;
        constexpr std::uint8_t kLevelSetterSignature[] = {0xF3, 0x0F, 0x11, 0x49, 0x1C, 0xC3};
        constexpr std::uintptr_t kLevelGetterRva = 0xB00950;
        constexpr std::uint8_t kLevelGetterSignature[] = {0xF3, 0x0F, 0x10, 0x41, 0x1C, 0xC3};
        // The fire's burnable entity update 0x12BD608 at 0x12BD7A9: entity
        // +0x28, +0x18, call [rax+0x88].
        constexpr std::uintptr_t kFirePathRva = 0x12BD7A9;
        constexpr std::uint8_t kFirePathSignature[] = {
            0x48, 0x8B, 0x47, 0x28, 0x0F, 0x5C, 0xFB, 0x48, 0x8B, 0x78, 0x18, 0x0F, 0x59, 0x7B, 0x40, 0x0F,
            0x58, 0xFB, 0x48, 0x85, 0xFF, 0x0F, 0x84, 0x8F, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x07, 0x48, 0x8B,
            0xCF, 0xFF, 0x90, 0x88, 0x00, 0x00, 0x00};
        // The resolver 0x558244 at 0x558284: type byte 4, [dh+0x748], +0xC0,
        // object +0x3C8, else +0x28 -> +0x18, then call [rax+0x88].
        constexpr std::uintptr_t kResolverRva = 0x558284;
        constexpr std::uint8_t kResolverSignature[] = {
            0x80, 0x7A, 0x30, 0x04, 0x75, 0x2E, 0x48, 0x39, 0x5A, 0x28, 0x74, 0x14, 0x48, 0x8B, 0xCA, 0xE8,
            0x08, 0x0B, 0xE0, 0xFF, 0x48, 0x8B, 0x0F, 0x48, 0x8B, 0x90, 0x48, 0x07, 0x00, 0x00, 0xEB, 0x06,
            0x48, 0x8B, 0xD3, 0x48, 0x8B, 0xCB, 0x48, 0x85, 0xD2, 0x74, 0x39, 0x48, 0x8B, 0x8A, 0xC0, 0x00,
            0x00, 0x00, 0xEB, 0x2D, 0x84, 0xC0, 0x74, 0x17, 0x48, 0x8B, 0x82, 0xC8, 0x03, 0x00, 0x00, 0x48,
            0x8B, 0xCB, 0x48, 0x85, 0xC0, 0x74, 0x1D, 0x48, 0x89, 0x07, 0x48, 0x8B, 0xC8, 0xEB, 0x15, 0x48,
            0x8B, 0x4A, 0x28, 0x48, 0x85, 0xC9, 0x74, 0x1D, 0x48, 0x8B, 0x49, 0x18, 0x48, 0x85, 0xC9, 0x74,
            0x14, 0x48, 0x89, 0x0F, 0x48, 0x85, 0xC9, 0x74, 0x0C, 0x48, 0x8B, 0x01, 0xFF, 0x90, 0x88, 0x00,
            0x00, 0x00};

        bool g_checked = false;
        bool g_enabled = false;
        std::string g_error;
        std::uintptr_t g_base = 0;
        std::uintptr_t g_imageEnd = 0;
        std::uintptr_t g_blockGetter = 0;

        bool GuardedCompare(const void* target, const std::uint8_t* signature, std::size_t size)
        {
            return replay::mem::Matches(target, signature, size); // guarded_memory.h
        }

        bool GuardedImageSize(std::uintptr_t base, std::uintptr_t& size)
        {
            __try
            {
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
                size = nt->OptionalHeader.SizeOfImage;
                return dos->e_magic == IMAGE_DOS_SIGNATURE && nt->Signature == IMAGE_NT_SIGNATURE;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // One path's result (plain data, filled under __try).
        struct Block
        {
            std::uintptr_t block = 0;
            std::uint32_t vtableRva = 0;
        };

        // The block of an effect when its virtual +0x88 is the engine's
        // block getter; 0 otherwise. Called only under __try.
        void EffectBlock(std::uintptr_t effect, Block& out)
        {
            out = {};
            if (!effect)
                return;
            const std::uintptr_t vtable = *reinterpret_cast<const std::uintptr_t*>(effect);
            if (vtable < g_base || vtable >= g_imageEnd)
                return;
            out.vtableRva = static_cast<std::uint32_t>(vtable - g_base);
            const std::uintptr_t getter = reinterpret_cast<const std::uintptr_t*>(vtable)[kBlockGetterSlot];
            if (getter != g_blockGetter)
                return;
            out.block = *reinterpret_cast<const std::uintptr_t*>(effect + kEffectBlock);
        }

        // Both paths of an entity. false when the entity could not be read.
        bool GuardedBlocks(std::uintptr_t entity, Block& fire, Block& ped)
        {
            __try
            {
                fire = {};
                ped = {};
                const std::uintptr_t handler = *reinterpret_cast<const std::uintptr_t*>(entity + kEntityDrawHandler);
                if (!handler)
                    return true;
                EffectBlock(*reinterpret_cast<const std::uintptr_t*>(handler + kHandlerEffect), fire);
                if (*reinterpret_cast<const std::uint8_t*>(entity + kEntityType) == kTypePed)
                {
                    const std::uintptr_t data = *reinterpret_cast<const std::uintptr_t*>(handler + kHandlerPedData);
                    if (data)
                        EffectBlock(*reinterpret_cast<const std::uintptr_t*>(data + kPedDataEffect), ped);
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool GuardedReadFloat(std::uintptr_t address, float& value)
        {
            __try
            {
                value = *reinterpret_cast<const float*>(address);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool GuardedWriteFloat(std::uintptr_t address, float value)
        {
            __try
            {
                *reinterpret_cast<float*>(address) = value;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    bool Check(std::string& error)
    {
        if (g_checked)
        {
            error = g_error;
            return g_enabled;
        }
        g_checked = true;
        g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        std::uintptr_t size = 0;
        const auto* base = reinterpret_cast<const std::uint8_t*>(g_base);
        struct Site
        {
            std::uintptr_t rva;
            const std::uint8_t* bytes;
            std::size_t size;
            const char* name;
        };
        const Site sites[] = {
            {kBlockGetterRva, kBlockGetterSignature, sizeof(kBlockGetterSignature), "burn_block_getter_signature_mismatch"},
            {kBlockGetterBodyRva, kBlockGetterBodySignature, sizeof(kBlockGetterBodySignature),
                "burn_block_getter_body_signature_mismatch"},
            {kLevelSetterRva, kLevelSetterSignature, sizeof(kLevelSetterSignature), "burn_level_setter_signature_mismatch"},
            {kLevelGetterRva, kLevelGetterSignature, sizeof(kLevelGetterSignature), "burn_level_getter_signature_mismatch"},
            {kFirePathRva, kFirePathSignature, sizeof(kFirePathSignature), "burn_fire_path_signature_mismatch"},
            {kResolverRva, kResolverSignature, sizeof(kResolverSignature), "burn_resolver_signature_mismatch"},
        };
        if (!g_base || !GuardedImageSize(g_base, size))
        {
            g_error = "burn_no_module";
        }
        else
        {
            g_imageEnd = g_base + size;
            for (const Site& site : sites)
            {
                if (site.rva + site.size > size || !GuardedCompare(base + site.rva, site.bytes, site.size))
                {
                    g_error = site.name;
                    break;
                }
            }
        }
        g_enabled = g_error.empty();
        g_blockGetter = g_enabled ? g_base + kBlockGetterRva : 0;
        error = g_error;
        return g_enabled;
    }

    bool Enabled()
    {
        return g_enabled;
    }

    bool Read(const void* entity, State& state)
    {
        state = {};
        if (!g_enabled || !entity)
            return false;
        Block fire;
        Block ped;
        if (!GuardedBlocks(reinterpret_cast<std::uintptr_t>(entity), fire, ped))
            return false;
        state.fireVtableRva = fire.vtableRva;
        state.pedVtableRva = ped.vtableRva;
        state.sameBlock = fire.block && fire.block == ped.block;
        float level = 0.0f;
        if (fire.block && GuardedReadFloat(fire.block + kBlockLevel, level) && std::isfinite(level))
        {
            state.fireBlock = true;
            state.level = level;
        }
        if (ped.block && GuardedReadFloat(ped.block + kBlockLevel, level) && std::isfinite(level))
        {
            state.pedBlock = true;
            state.pedLevel = level;
            if (!state.fireBlock)
                state.level = level;
        }
        if (!state.fireBlock && !state.pedBlock)
            return false;
        const std::uintptr_t block = state.fireBlock ? fire.block : ped.block;
        GuardedReadFloat(block + kBlockLevelNoAsh, state.levelNoAsh);
        GuardedReadFloat(block + kBlockIntensity, state.intensity);
        GuardedReadFloat(block + kBlockOpacity, state.opacity);
        return true;
    }

    int Write(const void* entity, float level)
    {
        if (!g_enabled || !entity || !std::isfinite(level))
            return 0;
        Block fire;
        Block ped;
        if (!GuardedBlocks(reinterpret_cast<std::uintptr_t>(entity), fire, ped))
            return 0;
        int written = 0;
        if (fire.block && GuardedWriteFloat(fire.block + kBlockLevel, level))
            ++written;
        if (ped.block && ped.block != fire.block && GuardedWriteFloat(ped.block + kBlockLevel, level))
            ++written;
        return written;
    }

    // The fire record (burn_level.h, CheckChar and below).
    namespace
    {
        // 0xF9A91D: lea rdx,[rsp+30]; lea rcx,[rip+rel] -> the tree at
        // 0x4E2F610 (_IS_ENTITY_CONSUMED_BY_FIRE's lookup, 0xF9A8DC).
        constexpr std::uintptr_t kTreeSiteRva = 0xF9A91D;
        constexpr std::uint8_t kTreeSiteSignature[] = {0x48, 0x8D, 0x54, 0x24, 0x30, 0x48, 0x8D, 0x0D, 0xE7, 0x4C, 0xE9, 0x03};
        constexpr std::uintptr_t kTreeRva = 0x4E2F610;
        // RESURRECT_PED's removal call 0x8D9B0A: lea rcx,[rip+rel] -> the
        // manager 0x4E2F5B0 (tree at +0x60).
        constexpr std::uintptr_t kManagerSiteRva = 0x8D9B0A;
        constexpr std::uint8_t kManagerSiteSignature[] = {0x48, 0x8D, 0x0D, 0x9F, 0x5A, 0x55, 0x04};
        constexpr std::uintptr_t kManagerRva = 0x4E2F5B0;
        constexpr std::uintptr_t kRemoveRva = 0x12A3D58;
        constexpr std::uint8_t kRemoveSignature[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x57, 0x48,
            0x83, 0xEC, 0x70, 0x48, 0x89, 0x50, 0xB8, 0x41, 0x8A, 0xE9, 0x48, 0x8B, 0xF1};
        // The area clear's skip test (0x12A302C: +0x110, +0x142 & 2, +0xF0).
        constexpr std::uintptr_t kClearSkipRva = 0x12A302C;
        constexpr std::uint8_t kClearSkipSignature[] = {0x48, 0x83, 0xBB, 0x10, 0x01, 0x00, 0x00, 0x00, 0x74, 0x3B, 0x8A, 0x83,
            0x42, 0x01, 0x00, 0x00, 0x24, 0x02, 0x74, 0x09, 0x83, 0xBB, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x74, 0x28, 0x84, 0xC0,
            0x0F, 0x85, 0xE4, 0x00, 0x00, 0x00};
        constexpr std::uintptr_t kRecordShieldFlags = 0x142;
        constexpr std::uint8_t kShieldBit = 0x02;
        constexpr std::uintptr_t kRecordShieldCount = 0xF0;
        bool g_shieldEnabled = false;
        constexpr std::uintptr_t kRecordVolumes = 0xD0;
        constexpr std::uintptr_t kRecordCount = 0xD8;
        constexpr std::uintptr_t kRecordFlags = 0x141;
        constexpr std::size_t kVolumeBytes = 0xF0;
        // A volume's kept floats: 16 at +0x10, 8 at +0xC0.
        constexpr std::uintptr_t kVolumeFirst = 0x10;
        constexpr std::size_t kVolumeFirstFloats = 16;
        constexpr std::uintptr_t kVolumeSecond = 0xC0;
        constexpr std::size_t kVolumeSecondFloats = 8;
        constexpr int kTreeMaxNodes = 4096;

        bool g_charChecked = false;
        bool g_charEnabled = false;
        std::string g_charError;

        std::uint64_t g_charFaults = 0;
        bool GuardedCopy(void* out, std::uintptr_t source, std::size_t size)
        {
            // A refused or faulting read counts.
            if (replay::mem::Copy(out, source, size))
                return true;
            ++g_charFaults;
            return false;
        }

        bool GuardedPut(std::uintptr_t target, const void* source, std::size_t size)
        {
            return replay::mem::Put(target, source, size); // guarded_memory.h
        }

        // Every (entity, record) of the tree, or only `entity`'s when not 0:
        // a walk of the whole tree (a few hundred nodes at most). Returns the
        // record of `entity`.
        std::uintptr_t WalkRecords(std::uintptr_t entity, std::vector<std::pair<std::uintptr_t, std::uintptr_t>>* all)
        {
            std::uintptr_t root = 0;
            if (!GuardedCopy(&root, g_base + kTreeRva, sizeof(root)) || !root)
                return 0;
            std::uintptr_t stack[64];
            int depth = 0;
            stack[depth++] = root;
            int visited = 0;
            while (depth > 0 && visited < kTreeMaxNodes)
            {
                const std::uintptr_t node = stack[--depth];
                ++visited;
                // Heap nodes: a guarded copy only (a VirtualQuery per node
                // costs about 2 ms a sample).
                std::uintptr_t head[6] = {};
                if (!GuardedCopy(head, node, sizeof(head)))
                    continue;
                if (entity && head[4] == entity)
                    return node;
                if (all)
                    all->push_back({head[4], node});
                const std::uintptr_t right = head[1] & ~static_cast<std::uintptr_t>(1);
                const std::uintptr_t left = head[2];
                if (right && depth < 64)
                    stack[depth++] = right;
                if (left && depth < 64)
                    stack[depth++] = left;
            }
            return 0;
        }

        // The record of `entity` by the tree's own search (0xEB2170: a node
        // whose key is not below the target goes left (+0x10) and is kept,
        // else right (+0x08, tagged)); keys compare entity then the int.
        std::uintptr_t FindRecord(std::uintptr_t entity)
        {
            std::uintptr_t node = 0;
            if (!GuardedCopy(&node, g_base + kTreeRva, sizeof(node)))
                return 0;
            std::uintptr_t best = 0;
            for (int depth = 0; node && depth < 128; ++depth)
            {
                std::uintptr_t head[6] = {};
                if (!GuardedCopy(head, node, sizeof(head)))
                    return 0;
                if (head[4] >= entity)
                {
                    best = node;
                    node = head[2];
                }
                else
                {
                    node = head[1] & ~static_cast<std::uintptr_t>(1);
                }
            }
            std::uintptr_t key = 0;
            if (!best || !GuardedCopy(&key, best + 0x20, sizeof(key)) || key != entity)
                return 0;
            return best;
        }

        // A volume's part (0x1253160 initialises them: +0xB0 -1,
        // +0xE4 u16 -1, +0xB8 0, +0x60 direction; 0x12565A8 copies them).
        constexpr std::uintptr_t kVolumeDir = 0x60;
        constexpr std::uintptr_t kVolumeOwnerFire = 0x70;
        constexpr std::uintptr_t kVolumeComponent = 0xB0;
        constexpr std::uintptr_t kVolumeMask = 0xB8;
        constexpr std::uintptr_t kVolumeSub = 0xE4;
        constexpr std::uintptr_t kVolumeFlags = 0xE6;
        constexpr std::uint8_t kVolumeNoFire = 0x04; // 0x126DEF3: a copy without its fire

        bool ReadVolume(std::uintptr_t volume, CharVolume& out)
        {
            return GuardedCopy(out.v, volume + kVolumeFirst, kVolumeFirstFloats * sizeof(float)) &&
                GuardedCopy(out.v + kVolumeFirstFloats, volume + kVolumeSecond, kVolumeSecondFloats * sizeof(float)) &&
                GuardedCopy(&out.component, volume + kVolumeComponent, sizeof(out.component)) &&
                GuardedCopy(&out.sub, volume + kVolumeSub, sizeof(out.sub)) &&
                GuardedCopy(&out.flags, volume + kVolumeFlags, sizeof(out.flags)) &&
                GuardedCopy(&out.mask, volume + kVolumeMask, sizeof(out.mask)) &&
                GuardedCopy(out.dir, volume + kVolumeDir, sizeof(out.dir));
        }

        bool WriteVolumeValues(std::uintptr_t volume, const CharVolume& value, bool part)
        {
            if (!GuardedPut(volume + kVolumeFirst, value.v, kVolumeFirstFloats * sizeof(float)) ||
                !GuardedPut(volume + kVolumeSecond, value.v + kVolumeFirstFloats, kVolumeSecondFloats * sizeof(float)))
                return false;
            if (!part)
                return true;
            std::uint8_t flags = 0;
            if (GuardedCopy(&flags, volume + kVolumeFlags, 1))
            {
                flags = static_cast<std::uint8_t>((flags & 0xF0) | (value.flags & 0x0F));
                GuardedPut(volume + kVolumeFlags, &flags, 1);
            }
            GuardedPut(volume + kVolumeMask, &value.mask, sizeof(value.mask));
            return true;
        }

        // The engine's volume copy and link (0x126E04F's loop).
        constexpr std::uintptr_t kCloneRva = 0x126DE7C;
        constexpr std::uint8_t kCloneSignature[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89,
            0x70, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA, 0x48};
        constexpr std::uintptr_t kLinkRva = 0x125B82C;
        constexpr std::uint8_t kLinkSignature[] = {0x48, 0x85, 0xD2, 0x0F, 0x84, 0x43, 0x01, 0x00, 0x00, 0x48, 0x89, 0x5C, 0x24,
            0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20};
        // The engine's record copy onto another entity (used by
        // the fire spreading to a new entity, 0x1299869: (manager, source
        // record, entity, copy volumes)); the copy takes the source's
        // record fields, not its volumes (last argument 0).
        constexpr std::uintptr_t kRecordCopyRva = 0x126E5DC;
        constexpr std::uint8_t kRecordCopySignature[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x18, 0x48,
            0x89, 0x70, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x33, 0xDB, 0x45, 0x8A};
        using RecordCopyFn = void*(__fastcall*)(void* manager, void* source, void* entity, std::uint8_t copyVolumes);
        using CloneFn = void*(__fastcall*)(void* source, void* record, void* fire);
        using LinkFn = void(__fastcall*)(void* record, void* volume, std::uint8_t);
        bool g_createOk = false;
        bool g_createWanted = true;

        std::uintptr_t GuardedRecordCopy(std::uintptr_t source, void* entity, std::uint32_t& code)
        {
            __try
            {
                const auto copy = reinterpret_cast<RecordCopyFn>(g_base + kRecordCopyRva);
                return reinterpret_cast<std::uintptr_t>(
                    copy(reinterpret_cast<void*>(g_base + kManagerRva), reinterpret_cast<void*>(source), entity, 0));
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        // Clones `source` (a volume of `record`) and sets the copy's part
        // before the engine links it (the link sets the mask from the part).
        // Returns the new volume or 0. Only POD locals (SEH).
        std::uintptr_t GuardedCreateVolume(std::uintptr_t record, std::uintptr_t source, std::int32_t component, std::uint16_t sub,
            const float* dir, std::uint32_t& code)
        {
            __try
            {
                const auto clone = reinterpret_cast<CloneFn>(g_base + kCloneRva);
                const auto link = reinterpret_cast<LinkFn>(g_base + kLinkRva);
                const std::uintptr_t volume = reinterpret_cast<std::uintptr_t>(
                    clone(reinterpret_cast<void*>(source), reinterpret_cast<void*>(record), nullptr));
                if (!volume)
                    return 0;
                // A copy of a volume of the same record keeps the source's
                // fire (0x126DEC9); the engine's cross-record copy has none
                // and flag 4: the same here.
                *reinterpret_cast<std::uint64_t*>(volume + kVolumeOwnerFire) = 0;
                *reinterpret_cast<std::uint8_t*>(volume + kVolumeFlags) |= kVolumeNoFire;
                *reinterpret_cast<std::int32_t*>(volume + kVolumeComponent) = component;
                *reinterpret_cast<std::uint16_t*>(volume + kVolumeSub) = sub;
                *reinterpret_cast<std::uint32_t*>(volume + kVolumeMask) = 0;
                float* direction = reinterpret_cast<float*>(volume + kVolumeDir);
                direction[0] = dir[0];
                direction[1] = dir[1];
                direction[2] = dir[2];
                link(reinterpret_cast<void*>(record), reinterpret_cast<void*>(volume), 0);
                return volume;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        bool Finite(const CharVolume& volume)
        {
            for (float value : volume.v)
                if (!std::isfinite(value))
                    return false;
            return true;
        }
    }

    bool CheckChar(std::string& error)
    {
        if (g_charChecked)
        {
            error = g_charError;
            return g_charEnabled;
        }
        g_charChecked = true;
        std::string ignored;
        Check(ignored); // module range (g_base, g_imageEnd)
        if (!g_base)
            g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto* base = reinterpret_cast<const std::uint8_t*>(g_base);
        if (!GuardedCompare(base + kTreeSiteRva, kTreeSiteSignature, sizeof(kTreeSiteSignature)))
            g_charError = "char_tree_site_signature_mismatch";
        else if (!GuardedCompare(base + kManagerSiteRva, kManagerSiteSignature, sizeof(kManagerSiteSignature)))
            g_charError = "char_manager_site_signature_mismatch";
        else if (!GuardedCompare(base + kRemoveRva, kRemoveSignature, sizeof(kRemoveSignature)))
            g_charError = "char_remove_signature_mismatch";
        g_charEnabled = g_charError.empty();
        // The shield only when the clear's skip test is as read.
        g_shieldEnabled = g_charEnabled && GuardedCompare(base + kClearSkipRva, kClearSkipSignature, sizeof(kClearSkipSignature));
        g_createOk = g_charEnabled && GuardedCompare(base + kCloneRva, kCloneSignature, sizeof(kCloneSignature)) &&
            GuardedCompare(base + kRecordCopyRva, kRecordCopySignature, sizeof(kRecordCopySignature)) &&
            GuardedCompare(base + kLinkRva, kLinkSignature, sizeof(kLinkSignature));
        if (g_charEnabled && !g_shieldEnabled)
            g_charError = "char_shield_signature_mismatch"; // reported, reads and writes stay on
        error = g_charError;
        return g_charEnabled;
    }

    bool CharEnabled()
    {
        return g_charEnabled;
    }

    std::uint64_t CharFaults()
    {
        return g_charFaults;
    }

    bool ReadChar(const void* entity, CharState& state)
    {
        state = {};
        if (!g_charEnabled || !entity)
            return false;
        return ReadRecord(FindRecord(reinterpret_cast<std::uintptr_t>(entity)), state);
    }

    bool ReadRecord(std::uintptr_t record, CharState& state)
    {
        state = {};
        if (!g_charEnabled)
            return false;
        if (!record)
            return true;
        std::uintptr_t volume = 0;
        std::uint32_t count = 0;
        if (!GuardedCopy(&state.flags, record + kRecordFlags, 1) ||
            !GuardedCopy(&volume, record + kRecordVolumes, sizeof(volume)) || !GuardedCopy(&count, record + kRecordCount, sizeof(count)))
            return false;
        state.present = true;
        state.count = count;
        for (std::uint32_t n = 0; n < count && n < kCharMaxVolumes && volume; ++n)
        {
            CharVolume read;
            if (!ReadVolume(volume, read) || !Finite(read))
                break;
            state.volumes.push_back(read);
            std::uintptr_t next = 0;
            if (!GuardedCopy(&next, volume, sizeof(next)))
                break;
            volume = next;
        }
        return true;
    }

    CharWriteReport WriteChar(const void* entity, const CharState& state, const std::vector<int>& match, bool anchored)
    {
        CharWriteReport report;
        if (!g_charEnabled || !entity || !state.present || state.volumes.empty())
            return report;
        const std::uintptr_t record = FindRecord(reinterpret_cast<std::uintptr_t>(entity));
        if (!record)
            return report;
        std::uintptr_t volume = 0;
        std::uint32_t count = 0;
        if (!GuardedCopy(&volume, record + kRecordVolumes, sizeof(volume)) || !GuardedCopy(&count, record + kRecordCount, sizeof(count)))
            return report;
        std::uintptr_t volumes[kCharMaxVolumes] = {};
        std::size_t have = 0;
        for (std::uint32_t n = 0; n < count && have < kCharMaxVolumes && volume; ++n)
        {
            volumes[have++] = volume;
            std::uintptr_t next = 0;
            if (!GuardedCopy(&next, volume, sizeof(next)))
                break;
            volume = next;
        }
        // A record with no volume (just made by CreateChar): the copies come
        // from a volume of another record (the engine's cross-record copy).
        std::uintptr_t source = have ? volumes[0] : 0;
        if (!source && anchored && g_createOk && g_createWanted)
        {
            std::vector<std::pair<std::uintptr_t, std::uintptr_t>> all;
            WalkRecords(0, &all);
            for (const auto& [owner, other] : all)
            {
                std::uintptr_t first = 0;
                if (other != record && GuardedCopy(&first, other + kRecordVolumes, sizeof(first)) && first)
                {
                    source = first;
                    break;
                }
            }
        }
        if (!source)
            return report;
        bool taken[kCharMaxVolumes] = {};
        for (std::size_t r = 0; r < state.volumes.size(); ++r)
        {
            const int target = r < match.size() ? match[r] : -1;
            std::uintptr_t into = 0;
            if (target >= 0 && static_cast<std::size_t>(target) < have && !taken[target])
            {
                into = volumes[target];
                taken[target] = true;
            }
            else if (anchored && g_createOk && g_createWanted)
            {
                std::uint32_t code = 0;
                const CharVolume& part = state.volumes[r];
                into = GuardedCreateVolume(record, source, part.component, part.sub, part.dir, code);
                if (into)
                    ++report.created;
                else
                    ++report.createFailed;
            }
            if (into && WriteVolumeValues(into, state.volumes[r], anchored))
                ++report.written;
        }
        // The entity's parts the recording does not have (made by the fire
        // the replay restarted on it) draw nothing. Not freed: a live fire
        // may point at them.
        for (std::size_t n = 0; n < have; ++n)
        {
            if (taken[n])
                continue;
            CharVolume value;
            if (!ReadVolume(volumes[n], value))
                continue;
            NeutralizeVolume(value);
            if (WriteVolumeValues(volumes[n], value, false))
                ++report.neutralized;
        }
        return report;
    }

    bool CreateChar(void* entity, std::uint32_t& code)
    {
        code = 0;
        if (!g_charEnabled || !g_createOk || !g_createWanted || !entity)
            return false;
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(entity);
        if (FindRecord(address))
            return true;
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> all;
        WalkRecords(0, &all);
        std::uintptr_t source = 0;
        for (const auto& [owner, record] : all)
            if (owner && owner != address)
            {
                source = record;
                break;
            }
        if (!source)
        {
            code = 2; // no record to copy from
            return false;
        }
        GuardedRecordCopy(source, entity, code);
        return FindRecord(address) != 0;
    }

    void SetCharCreate(bool on)
    {
        g_createWanted = on;
    }

    bool CharCreateEnabled()
    {
        return g_createOk && g_createWanted;
    }

    namespace
    {
        using RemoveFn = void(__fastcall*)(void*, void*, std::int32_t, std::uint8_t);
        bool GuardedRemove(std::uintptr_t base, void* entity, std::uint32_t& code)
        {
            __try
            {
                reinterpret_cast<RemoveFn>(base + kRemoveRva)(reinterpret_cast<void*>(base + kManagerRva), entity, -1, 1);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    bool RemoveChar(void* entity, std::uint32_t& code)
    {
        code = 0;
        if (!g_charEnabled || !entity || !FindRecord(reinterpret_cast<std::uintptr_t>(entity)))
            return false;
        return GuardedRemove(g_base, entity, code) && !FindRecord(reinterpret_cast<std::uintptr_t>(entity));
    }

    int ShieldChars(const std::vector<const void*>& entities, std::vector<CharShield>& saved)
    {
        saved.clear();
        if (!g_charEnabled || !g_shieldEnabled)
            return 0;
        for (const void* entity : entities)
        {
            const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(entity);
            const std::uintptr_t record = address ? FindRecord(address) : 0;
            if (!record)
                continue;
            CharShield shield;
            shield.record = record;
            shield.entity = address;
            if (!GuardedCopy(&shield.flags142, record + kRecordShieldFlags, 1) ||
                !GuardedCopy(&shield.count0F0, record + kRecordShieldCount, sizeof(shield.count0F0)))
                continue;
            if ((shield.flags142 & kShieldBit) && shield.count0F0)
                continue; // the clear skips it already
            const std::uint8_t flags = static_cast<std::uint8_t>(shield.flags142 | kShieldBit);
            const std::uint32_t count = shield.count0F0 ? shield.count0F0 : 1u;
            if (!GuardedPut(record + kRecordShieldFlags, &flags, 1))
                continue;
            if (!GuardedPut(record + kRecordShieldCount, &count, sizeof(count)))
            {
                GuardedPut(record + kRecordShieldFlags, &shield.flags142, 1);
                continue;
            }
            saved.push_back(shield);
        }
        return static_cast<int>(saved.size());
    }

    void UnshieldChars(const std::vector<CharShield>& saved)
    {
        for (const CharShield& shield : saved)
        {
            // Only a record still in the tree for the same entity is touched.
            if (FindRecord(shield.entity) != shield.record)
                continue;
            GuardedPut(shield.record + kRecordShieldFlags, &shield.flags142, 1);
            GuardedPut(shield.record + kRecordShieldCount, &shield.count0F0, sizeof(shield.count0F0));
        }
    }
}

