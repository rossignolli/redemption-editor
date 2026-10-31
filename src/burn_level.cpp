#include "burn_level.h"
#include "engine_calls.h"
#include "game_sig.h"
#include "guarded_memory.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace replay::burn
{
    namespace
    {
        namespace sig = replay::sig;

        // Entity +0x28: draw handler; +0x30: entity type byte (4 = ped).
        constexpr std::uintptr_t kEntityDrawHandler = 0x28;
        constexpr std::uintptr_t kEntityType = 0x30;
        constexpr std::uint8_t kTypePed = 4;
        // Draw handler +0x18: shader effect (fire path); ped draw handler
        // +0x748 -> +0xC0: the ped's effect (the mask resolver below).
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

        // The block getter every effect class with a block has at +0x88: a
        // jump into the protected text, where `mov rax, [rcx+0x18]`. The
        // effect vtables hold the jump; the code calls the same (folded)
        // getter directly too: a caller of it is the anchor.
        const sig::Signature kBurnBlockCaller{"burn_block_caller",
            "48 89 5C 24 08 57 48 83 EC 20 E8 ?? ?? ?? ?? 48 8B C8 48 8B F8 33 DB E8 ?? ?? ?? ??", 0x1520F88};
        const sig::Signature kBurnBlockGetter{"burn_block_getter", kBurnBlockCaller, sig::Rel32{0x18, 0x1C},
            "E9 ?? ?? ?? ??", 0x3090A4};
        const sig::Signature kBurnBlockGetterBody{"burn_block_getter_body", kBurnBlockGetter, sig::Rel32{1, 5},
            "48 8B 41 18", 0x6FCB3C1};
        // The fire's burnable entity update: entity +0x28, +0x18, call
        // [rax+0x88]; then it calls the level setter (movss [rcx+0x1C], xmm1).
        const sig::Signature kBurnFireUpdate{"burn_fire_update",
            "48 8B C4 48 89 58 08 48 89 70 18 57 48 83 EC 60 48 8B 79 68 48 8B D9 0F 29 70 E8 0F 29 78 D8 44 0F 29 40 C8 "
            "44 0F 28 C1",
            0x12BD608};
        const sig::Signature kBurnFirePath{"burn_fire_path", kBurnFireUpdate, sig::Offset{0x1A1},
            "48 8B 47 28 0F 5C FB 48 8B 78 18 0F 59 7B 40 0F 58 FB 48 85 FF 0F 84 ?? ?? ?? ?? 48 8B 07 48 8B CF FF 90 88 "
            "00 00 00",
            0x12BD7A9};
        const sig::Signature kBurnLevelSetter{"burn_level_setter", kBurnFireUpdate, sig::Rel32{0x21B, 0x21F},
            "F3 0F 11 49 1C C3", 0x6FD2E4};
        // The vehicle's burn update reads the level through the getter (movss
        // xmm0, [rcx+0x1C]).
        const sig::Signature kBurnVehicleUpdate{"burn_vehicle_update",
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 48 83 EC 40 48 8B 59 20 41 8A E8 0F 29 70 E8 48 8B F9 0F 29 "
            "78 D8 0F 28 F9",
            0x15D5978};
        const sig::Signature kBurnLevelGetter{"burn_level_getter", kBurnVehicleUpdate, sig::Rel32{0xE2, 0xE6},
            "F3 0F 10 41 1C C3", 0xB00950};
        // The entity mask natives' resolver: type byte 4, [dh+0x748], +0xC0,
        // object +0x3C8, else +0x28 -> +0x18, then call [rax+0x88].
        const sig::Signature kBurnMaskResolver{"burn_mask_resolver",
            "48 89 5C 24 08 57 48 83 EC 20 33 DB 48 8B F9 48 89 19 48 89 59 08 48 85 D2 0F 84 ?? ?? ?? ?? 8B 42 34 C1 E8 "
            "08 A8 01 0F 84 ?? ?? ?? ??",
            0x558244};
        const sig::Signature kBurnResolver{"burn_resolver", kBurnMaskResolver, sig::Offset{0x40},
            "80 7A 30 04 75 2E 48 39 5A 28 74 14 48 8B CA E8 ?? ?? ?? ?? 48 8B 0F 48 8B 90 48 07 00 00 EB 06 48 8B D3 48 "
            "8B CB 48 85 D2 74 39 48 8B 8A C0 00 00 00 EB 2D 84 C0 74 17 48 8B 82 C8 03 00 00 48 8B CB 48 85 C0 74 1D 48 "
            "89 07 48 8B C8 EB 15 48 8B 4A 28 48 85 C9 74 1D 48 8B 49 18 48 85 C9 74 14 48 89 0F 48 85 C9 74 0C 48 8B 01 "
            "FF 90 88 00 00 00",
            0x558284};

        bool g_checked = false;
        bool g_enabled = false;
        std::string g_error;
        std::uintptr_t g_base = 0;
        std::uintptr_t g_imageEnd = 0;
        std::uintptr_t g_blockGetter = 0;

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
        g_base = sig::ImageBase();
        std::uintptr_t size = 0;
        struct Site
        {
            const sig::Signature* signature;
            const char* name;
        };
        const Site sites[] = {
            {&kBurnBlockGetter, "burn_block_getter_signature_mismatch"},
            {&kBurnBlockGetterBody, "burn_block_getter_body_signature_mismatch"},
            {&kBurnLevelSetter, "burn_level_setter_signature_mismatch"},
            {&kBurnLevelGetter, "burn_level_getter_signature_mismatch"},
            {&kBurnFirePath, "burn_fire_path_signature_mismatch"},
            {&kBurnResolver, "burn_resolver_signature_mismatch"},
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
                if (!sig::Find(*site.signature))
                {
                    g_error = site.name;
                    break;
                }
            }
        }
        g_enabled = g_error.empty();
        g_blockGetter = g_enabled ? sig::Find(kBurnBlockGetter) : 0;
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
        // _IS_ENTITY_CONSUMED_BY_FIRE's lookup: lea rdx,[rsp+30]; lea rcx,
        // [rip+rel] (the tree); the tree's find; the record's flags (+0x141).
        const sig::Signature kBurnConsumedLookup{"burn_consumed_lookup",
            "48 89 5C 24 08 57 48 83 EC 50 8B D9 E8 ?? ?? ?? ?? 41 B8 0D 00 00 00 8B D3 48 8B C8 48 8B F8 E8 ?? ?? ?? ?? "
            "33 DB",
            0xF9A8DC};
        const sig::Signature kBurnTreeSite{"burn_tree_site", kBurnConsumedLookup, sig::Offset{0x41},
            "48 8D 54 24 30 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 44 24 40 48 85 C0 74 0A F6 80 41 01 00 00 41 0F 95 "
            "C3",
            0xF9A91D};
        const sig::Signature kBurnTree{"burn_tree", kBurnTreeSite, sig::Rel32{8, 12}, "", 0x4E2F610};
        // RESURRECT_PED's removal call: lea rcx,[rip+rel] (the manager, tree
        // at +0x60), entity, -1, then the removal.
        const sig::Signature kBurnResurrect{"burn_resurrect",
            "48 8B C4 48 89 58 18 44 88 48 20 48 89 50 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 A8 48 81 EC 20 01 00 "
            "00 48 8B 79 08",
            0x8D972C};
        const sig::Signature kBurnManagerSite{"burn_manager_site", kBurnResurrect, sig::Offset{0x3DE},
            "48 8D 0D ?? ?? ?? ?? 41 83 C8 FF 48 8B D7 E8 ?? ?? ?? ??", 0x8D9B0A};
        const sig::Signature kBurnManager{"burn_manager", kBurnManagerSite, sig::Rel32{3, 7}, "", 0x4E2F5B0};
        const sig::Signature kBurnRemove{"burn_remove",
            "48 8B C4 48 89 68 10 48 89 70 18 57 48 83 EC 70 48 89 50 B8 41 8A E9 48 8B F1 48 85 D2 74 0C 80 7A 30 0D "
            "75 06 44 89 40 C0",
            0x12A3D58};
        // The area clear's skip test (+0x110, +0x142 & 2, +0xF0).
        const sig::Signature kBurnClearCore{"burn_clear_core",
            "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D A8 48 F8 FF FF 48 81 EC 90 08 "
            "00 00 33 F6",
            0x12A2FA4};
        const sig::Signature kBurnClearSkip{"burn_clear_skip", kBurnClearCore, sig::Offset{0x88},
            "48 83 BB 10 01 00 00 00 74 3B 8A 83 42 01 00 00 24 02 74 09 83 BB F0 00 00 00 00 74 28 84 C0 0F 85 ?? ?? ?? "
            "??",
            0x12A302C};
        constexpr std::uintptr_t kRecordShieldFlags = 0x142;
        constexpr std::uint8_t kShieldBit = 0x02;
        constexpr std::uintptr_t kRecordShieldCount = 0xF0;
        bool g_shieldEnabled = false;
        bool g_volumeWrites = false; // the volume fields shown by the engine's code
        std::uintptr_t g_tree = 0;    // the tree's root pointer
        std::uintptr_t g_manager = 0; // the fire record manager
        std::uintptr_t g_remove = 0;
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
            if (!g_tree || !GuardedCopy(&root, g_tree, sizeof(root)) || !root)
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

        // The record of `entity` by the tree's own search (its find: a node
        // whose key is not below the target goes left (+0x10) and is kept,
        // else right (+0x08, tagged)); keys compare entity then the int.
        std::uintptr_t FindRecord(std::uintptr_t entity)
        {
            std::uintptr_t node = 0;
            if (!g_tree || !GuardedCopy(&node, g_tree, sizeof(node)))
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

        // A volume's part (the volume initializer sets +0xB0 -1, +0xE4 u16 -1,
        // +0xB8 0, +0x60 direction; the volume copy copies them).
        constexpr std::uintptr_t kVolumeDir = 0x60;
        constexpr std::uintptr_t kVolumeOwnerFire = 0x70;
        constexpr std::uintptr_t kVolumeComponent = 0xB0;
        constexpr std::uintptr_t kVolumeMask = 0xB8;
        constexpr std::uintptr_t kVolumeSub = 0xE4;
        constexpr std::uintptr_t kVolumeFlags = 0xE6;
        constexpr std::uint8_t kVolumeNoFire = 0x04; // the clone: a copy without its fire

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

        // The engine's volume copy and link (the record copy's loop). The
        // clone allocates a volume, runs the volume initializer (it writes
        // every field written below: +0x10..+0x4F, +0x60, +0x70, +0xB0, +0xB8,
        // +0xBC..+0xE3, +0xE4, +0xE6), copies the source's part and, for
        // another record, drops the fire (+0x70) and sets flag 4 (+0xE6).
        const sig::Signature kBurnClone{"burn_clone",
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 48 83 EC 20 48 8B FA 48 8B E9 48 8D 50 20 49 8B F0 48 8D 0D "
            "?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74 79 48 8B CB E8 ?? ?? ?? ?? 48 8B D5 48 8B CB E8 ?? ?? ?? ??",
            0x126DE7C};
        const sig::Signature kBurnCloneNoFire{"burn_clone_no_fire", kBurnClone, sig::Offset{0x4D},
            "48 39 7B 78 74 2C 48 83 63 70 00 8A 85 E6 00 00 00 32 83 E6 00 00 00 24 20 32 83 E6 00 00 00 88 83 E6 00 00 "
            "00 48 85 F6 75 0D 0C 04 88 83 E6 00 00 00",
            0x126DEC9};
        const sig::Signature kBurnVolumeInit{"burn_volume_init", kBurnClone, sig::Rel32{0x39, 0x3D},
            "48 89 5C 24 10 57 48 83 EC 30 33 FF 0F 29 74 24 20 0F 57 F6 48 8B D9 0F 11 71 10 45 33 C0 0F 11 71 20 0F 11 "
            "71 30 8D 57 01 0F 11 71 40 0F 28 05 ?? ?? ?? ?? 0F 11 41 50 0F 28 0D ?? ?? ?? ?? 0F 11 49 60 48 89 79 70 48 "
            "89 79 78 48 89 B9 80 00 00 00 48 89 B9 88 00 00 00 48 89 B9 90 00 00 00 48 81 C1 98 00 00 00 E8 ?? ?? ?? ?? "
            "83 C8 FF 89 83 B0 00 00 00 89 BB B4 00 00 00 80 A3 E7 00 00 00 FE 66 89 83 E4 00 00 00 48 C7 83 BC 00 00 00 "
            "FF FF 7F 7F 48 89 BB C4 00 00 00 48 89 BB CC 00 00 00 48 89 BB D4 00 00 00 48 89 BB DC 00 00 00 40 88 BB E6 "
            "00 00 00 89 43 5C 0F 10 4B 10 0F 28 C1 0F C6 C6 FE 0F C6 C8 84 0F 11 4B 10 0F 10 53 20 0F 28 C2 0F C6 C6 FE "
            "0F 28 74 24 20 0F C6 D0 84 0F 11 53 20 48 89 7C 24 40 48 8B 44 24 40 48 89 03 48 8B C3 89 BB B8 00 00 00",
            0x1253160};
        // The link: the mask from the part, then the record's volume list
        // (+0xD0, linked by +0x00), its flags (+0x141) and count (+0xD8).
        const sig::Signature kBurnLink{"burn_link",
            "48 85 D2 0F 84 ?? ?? ?? ?? 48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 89 4A 78 41 8A "
            "E8",
            0x125B82C};
        const sig::Signature kBurnLinkList{"burn_link_list", kBurnLink, sig::Offset{0xE0},
            "48 8B 8B D0 00 00 00 80 8B 41 01 00 00 20 48 8B C1 48 85 C9 74 1F 48 8B C8 48 8B 00 48 85 C0 75 F5 48 85 C9 "
            "74 0F 48 89 7C 24 38 48 8B 44 24 38 48 89 01 EB 11 48 89 7C 24 38 48 8B 44 24 38 48 89 83 D0 00 00 00 48 FF "
            "83 D8 00 00 00",
            0x125B90C};
        // The engine's record copy onto another entity (used by
        // the fire spreading to a new entity: (manager, source
        // record, entity, copy volumes)); the copy takes the source's
        // record fields, not its volumes (last argument 0).
        const sig::Signature kBurnRecordCopy{"burn_record_copy",
            "48 8B C4 48 89 58 08 48 89 68 18 48 89 70 20 57 41 56 41 57 48 83 EC 70 33 DB 45 8A F9 49 8B F0 48 8B EA "
            "4C 8B F1",
            0x126E5DC};
        bool g_createOk = false;
        bool g_createWanted = true;
        std::uintptr_t g_clone = 0;
        std::uintptr_t g_link = 0;
        std::uintptr_t g_recordCopy = 0;

        std::uintptr_t GuardedRecordCopy(std::uintptr_t source, void* entity, std::uint32_t& code)
        {
            void* record = nullptr;
            if (!engine::Call(engine::kBurnRecordCopy, g_recordCopy, record, reinterpret_cast<void*>(g_manager),
                    reinterpret_cast<void*>(source), entity, 0))
            {
                engine::Failed(code);
                return 0;
            }
            return reinterpret_cast<std::uintptr_t>(record);
        }

        // Clones `source` (a volume of `record`) and sets the copy's part
        // before the engine links it (the link sets the mask from the part).
        // Returns the new volume or 0. Only POD locals (SEH).
        std::uintptr_t GuardedCreateVolume(std::uintptr_t record, std::uintptr_t source, std::int32_t component, std::uint16_t sub,
            const float* dir, std::uint32_t& code)
        {
            __try
            {
                void* cloned = nullptr;
                if (!engine::Call(engine::kBurnClone, g_clone, cloned, reinterpret_cast<void*>(source),
                        reinterpret_cast<void*>(record), nullptr))
                {
                    engine::Failed(code);
                    return 0;
                }
                const std::uintptr_t volume = reinterpret_cast<std::uintptr_t>(cloned);
                if (!volume)
                    return 0;
                // A copy of a volume of the same record keeps the source's
                // fire (the clone); the engine's cross-record copy has none
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
                if (!engine::Call(engine::kBurnLink, g_link, reinterpret_cast<void*>(record), reinterpret_cast<void*>(volume),
                        0))
                {
                    engine::Failed(code);
                    return 0;
                }
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

    std::uintptr_t RecordTree() { return sig::Find(kBurnTree); }

    bool CheckChar(std::string& error)
    {
        if (g_charChecked)
        {
            error = g_charError;
            return g_charEnabled;
        }
        g_charChecked = true;
        std::string ignored;
        Check(ignored); // the level checks run once, first
        g_tree = sig::Find(kBurnTree);
        g_manager = sig::Find(kBurnManager);
        g_remove = sig::Find(kBurnRemove);
        if (!g_tree)
            g_charError = "char_tree_site_signature_mismatch";
        else if (!g_manager)
            g_charError = "char_manager_site_signature_mismatch";
        else if (!g_remove)
            g_charError = "char_remove_signature_mismatch";
        g_charEnabled = g_charError.empty();
        // The shield only when the clear's skip test is as read.
        g_shieldEnabled = g_charEnabled && sig::Find(kBurnClearSkip);
        // The volume writes only when the engine's code shows the fields.
        g_volumeWrites = g_charEnabled && sig::Find(kBurnVolumeInit) && sig::Find(kBurnLinkList);
        g_clone = sig::Find(kBurnClone);
        g_link = sig::Find(kBurnLink);
        g_recordCopy = sig::Find(kBurnRecordCopy);
        g_createOk = g_volumeWrites && g_clone && g_link && g_recordCopy && sig::Find(kBurnCloneNoFire);
        if (g_charEnabled && !g_shieldEnabled)
            g_charError = "char_shield_signature_mismatch"; // reported, reads and writes stay on
        else if (g_charEnabled && !g_volumeWrites)
            g_charError = "char_volume_signature_mismatch"; // reported, reads stay on
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
        if (!g_charEnabled || !g_volumeWrites || !entity || !state.present || state.volumes.empty())
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
        bool GuardedRemove(void* entity, std::uint32_t& code)
        {
            return engine::Call(engine::kBurnRemove, g_remove, reinterpret_cast<void*>(g_manager), entity, -1, 1) ||
                engine::Failed(code);
        }
    }

    bool RemoveChar(void* entity, std::uint32_t& code)
    {
        code = 0;
        if (!g_charEnabled || !entity || !FindRecord(reinterpret_cast<std::uintptr_t>(entity)))
            return false;
        return GuardedRemove(entity, code) && !FindRecord(reinterpret_cast<std::uintptr_t>(entity));
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

