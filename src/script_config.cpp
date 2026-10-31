// script_config.cpp: configuration, log, crash and QA: the crash evidence handler and the crash dumps, the ini reads and the log header, the free camera options, the language, the QA report.
// Split from script.cpp (moved code); shared internals in script_internal.h.

#include "../script_internal.h"
#include "pose_deadband_smooth.h"
#include "sha256.h"

namespace
{
    // ReportIncludeClips=1 adds the open clip to the Shift+F1 report; by default
    // it carries only the clips the player marked (Shift+F2).
    bool g_reportIncludeClips = false;

    // Recording modes: the always-on replay buffer (a ring of the last seconds,
    // saved on demand) and the manual recording (start, stop saves).
    // RedemptionEditor.ini [RedemptionEditor]:
    //   ReplayBuffer=1|0         the buffer runs whenever nothing plays (default on)
    //   ReplayBufferSeconds=60   the ring's window
    //   ReplayBufferMB=128       the ring's memory budget (the head moves on
    //                            when the recording holds more)
    //   ManualMaxSeconds=0       0: no time cap
    //   ManualMaxMB=900          a manual recording over this (32 to 960; with
    //                            the buffer on, over its budget when larger) is
    //                            saved as a part and continues in the next
    //                            (chained, buffer_coverage.h ManualPartBytes)
    // Clip compression (src/clip_compress.h):
    //   CompressClips=1          compress the clips not written nor opened
    //                            for CompressAfterDays days, in the background
    //   CompressAfterDays=1      1 to 60 (a save writes the clip already
    //                            packed with fast zstd blocks; the compressor
    //                            repacks it with LZMA after these days)
    //   CompressLevel=19         1 (fast) to 22 (smallest): the LZMA level of
    //                            the packed format (19 and up: 9), the zstd
    //                            level with CompressFormat=1
    //   CompressFormat=2         dev build only: 2 the packed container
    //                            (clip_pack.h; zstd clips are repacked), 1 the
    //                            zstd frame as before 1.0
    // The recording gate (src/record_gate.h):
    //   RecordCutscenes=0        0: the recording pauses while a cutscene
    //                            plays (the clip cuts past it); 1: kept
    //   LockCutsceneCamera=0     1: markers inside a recorded cutscene use
    //                            the recorded game camera
    //   RecordingIndicator=normal  the REC notice: normal, brief, hidden (out of
    //                            screen capture) or off (src/rec_indicator.h)
    //   PoseDetail=standard      the pose codec of the recording: standard or
    //                            maximum (src/pose_deadband_smooth.h Detail)
    // The clip's map state (src/replay_world.h):
    //   RestoreWorldState=1      apply the clip's IPLs and entity sets in its
    //                            area at playback and give them back on exit
    //                            (0: only the notice; the bench can turn it off)
    //   RestoreWorldStateInMission=0  1: apply even while a mission runs
    // Effects, speech and puppets:
    //   SpeechReplayRules=1      1: a line the replay asked passes the rules'
    //                            world context codes 28, 41 and 48 (rules
    //                            detour 0x2F972C4, speech_replay_rules.h); 0:
    //                            the engine's codes.
    //   Each of the following, at 0, turns its part off:
    //   FxPuppetNoWarp=1         clears the relocate mark the puppet placement
    //                            sets on our effects (small steps, normal play)
    //   FxPuppetNoWarpDetour=0   reserve: skips the marking (detour 0x143C180)
    //   FxSettingsReplay=1       writes the recorded settings before the start
    //   FxPuppetVelocity=1       the puppet's motion as the particles' inherited
    //                            velocity (detour 0x1405BA4)
    //   FxPauseFreeze=1          paused while the replay owns the world, the vfx
    //                            update gets a step of 0 (detour 0x145C17C)
    //   FxRelocateClearStill=1   with FxPuppetNoWarp, the relocate mark of our
    //                            effect on an owner the playback did not place
    //                            this tick (a still prop, a map object) is
    //                            cleared when that owner moved less than the
    //                            instance's limit and 1 m
    //   WeaponFxIntent=1         records the weapon system's requests for each
    //                            shot's muzzle flash and shell (detours
    //                            0x14309A8, 0x14308F0, section WFXI) before
    //                            the engine's range and camera tests; the shot
    //                            audit names the shots the engine culled. A
    //                            clip with WFXI calls the same wrappers on the
    //                            puppets per request (the recorded muzzle
    //                            effects and shell tracks of those shots are
    //                            not played) and measures the sound around
    //                            each call (WEAPON_FX_INTENT_PLAY). 0: neither.
    //   HorseAudioEvidence=0     1 logs the player's horse audio
    //                            (POSE_V2_HORSE_*), live horse against puppet
    //   FootLayerReplay=1        the recorded ground layer weights of each foot
    //                            (FOOT v6) on the puppet's foot before the
    //                            step plays; read at each playback start
    //   PuppetAudioLodRecorded=1 the puppets' audio LOD override set to the
    //                            recorded LOD of their steps instead of 0
    //   ObjectBurnLevel=1        the burn level of the recorded weapon objects,
    //                            props and scenery objects (the meat a camp's
    //                            script cooks with _SET_OBJECT_BURN_LEVEL) is
    //                            recorded (sections BLVL and SBLV) and written
    //                            on their puppets and stand-ins; 0: neither
    //   WeaponPassPose=1         the pose of a replay weapon (lever, cylinder,
    //                            hammer) written again right after the weapon
    //                            system's per-frame pass (detour 0x117D49C),
    //                            which writes the weapon object's own bones
    //                            over it; 0: not installed (WEAPON_PASS_POSE,
    //                            WEAPON_BONES_DRAWN measure either way)
    //                            never_animated=1) is not pose-armed; the
    //                            engine poses the clone attached to the same
    //                            bone, as live (POSE_V2_OBJECT_STILL); 0: armed
    //   ObjectDrawnCheck=0       (evidence) each tick the pose the last frame
    //                            drew on a posed or still object against the
    //                            expected one and the bind pose
    //                            (POSE_V2_OBJECT_DRAWN,
    //                            POSE_V2_OBJECT_DRAWN_SUMMARY, write_checks)
    //   ObjectLook=1             the custom texture (document text: letters,
    //                            notes, telegrams, newspapers, posters, photos;
    //                            object +0x420/+0x424/+0x428) and the tint
    //                            (entity +0xB8 bits 3..8) of the recorded
    //                            weapon objects, props and scenery objects are
    //                            read (guarded) and recorded (section OLOK) and
    //                            set back on their puppets and stand-ins with
    //                            REQUEST_STREAMED_TXD,
    //                            SET_CUSTOM_TEXTURES_ON_OBJECT and
    //                            SET_OBJECT_TINT_INDEX; 0: neither
    //   RefusePuppetOwnPersistent=1 a puppet's own persistent ped audio sound
    //                            (foley, clothing) from a call site the clip's
    //                            ESND track carries is refused (the recorded
    //                            one plays); 0: not refused
    //   FrameEnd=1               every placed ped, horse and vehicle puppet
    //                            is set back to this tick's placement at its
    //                            pre-render (an end-of-frame write), and a ped
    //                            the pose finalizer skipped gets its pose there
    //                            (POSE_V2_PRERENDER_SUMMARY); 0: riders only
    //   PhysicsFilter=1          a walking puppet's active body collides only
    //                            with plants (its physics instance's include
    //                            flags ANDed with collision::kDefaultKeep), no
    //                            pushed hold (POSE_V2_COLLISION_FILTER_SUMMARY);
    //                            0: world collision, the other puppets kept
    //                            off pair by pair
    //   ProfileSlowTickMs=50     ticks this long get a PROFILE_SLOW_TICK line
    //                            with their largest zones (5 to 1000)
    // The sound census (src/sound_census.h):
    //   SoundCheck=Debug         the engine sound census in its cheap mode in
    //                            every recording and playback, and at each
    //                            playback's end ReplayDebug\<clip>.sound-check.txt
    //                            (sounds missing, fewer, extra, refused);
    //                            default 1 only with Debug=1
    //   SoundCensus=0            1: also a SOUND_CENSUS line per sound (bench)
    // The sound port probes (src/soundfx_probe.h):
    //   SoundFxProbe=0           1: read probes of the sound and particle
    //                            ports. A read-only detour per probed port
    //                            (signature checked) counts calls, threads,
    //                            callers and values; nothing is recorded or
    //                            changed. ReplayDebug\soundfx-probe-<date-
    //                            time>.txt every 60 s and at the unload
    //                            (tools/soundfx_probe_report.py reads it).
    //                            0: no detour of the probe is installed
    // The generic sound effects (src/sound_generic.h):
    //   SoundGeneric=1           1: the generic sound effect recorder. Every
    //                            top-level sound effect made while recording
    //                            becomes create, adjust and end packets in
    //                            the clip (section SGEN) through the sound
    //                            port detours (the probe's hooks, installed
    //                            for the recorder's ports with this key);
    //                            the player plays them. SOUND_GENERIC log
    //                            line every 60 s and at the stop;
    //                            ReplayDebug\<clip>.sound-generic.txt when
    //                            the clip is saved. 0: nothing runs
    //   SoundGenericSlots=64     sounds the player holds at once (8 to 256)
    //   SoundGenericTake=        family:taker;... who plays a family in
    //                            the replay (takers generic, specific,
    //                            puppet; families world, owned, speech,
    //                            footstep, horse_step, movement, melee,
    //                            gunshot, explosion, collision,
    //                            entity_sound, sound_request, captured;
    //                            src/sound_owner_table.h has the defaults)
    //   SoundGenericMode=packets in the replay the engine's own sounds of the
    //                            puppets are refused at the creation ports
    //                            (S03, S04, S05, S06; the speech's sound
    //                            controller apart) and every recorded packet
    //                            of a replay entity plays from the clip;
    //                            speech, steps, melee, shots and explosions
    //                            keep their tracks. legacy: per-track rules
    //   SoundGenericBlock=packet the ports refuse a puppet's own create only
    //                            when the recording plays that sound then
    //                            (a packet of the same owner and sound alive,
    //                            or starting within 3 s) or another set at
    //                            the entry a recorded packet fills; the rest
    //                            is left to the puppet and counted. all:
    //                            every own create refused
    //   SoundGenericTrace=       owner@fromMs-toMs (0:1@45000-48000, or
    //                            *@... for every owner): one log line
    //                            SOUND_GENERIC_TRACE per packet event in the
    //                            window (at most 600 per playback)
    //   SoundGenericSkip=        read at the load: what the generic player
    //                            does not apply, items by ';' (S16;S18;S19;
    //                            S21;S22;S23;S24;S38;stop;forget;release),
    //                            every family; a skipped item calls no port
    //                            and is counted (skipped_by_key in
    //                            SOUND_GENERIC_PLAYER_ENGINE; trace: skipped)
    //   SoundGenericEntityDrivenSkip=S18,stop,forget,release  for the generic
    //                            packets whose sound updates with its entity
    //                            (flag 0x04, kept), what the player leaves to
    //                            the puppet's audio entity (SoundGenericSkip
    //                            items); empty: everything recorded applies
    //   SoundGenericMute=        owner@fromMs-toMs: those packets are not
    //                            created (ablation)
    // The capture probes (src/capture_probe.h):
    //   CaptureProbe=0           1: read probes 14 to 30 (camera, weather,
    //                            lightning, timecycle, water, vehicle fields,
    //                            map changes, pickups, explosion, glass, cloth,
    //                            lights, cutscene, audio scenes and streams,
    //                            RayFire, script screens). 30 read-only
    //                            detours plus polls by native and guarded
    //                            read; nothing is recorded or changed.
    //                            ReplayDebug\capture-probe-<date-time>.txt
    //                            every 60 s and at the unload
    //                            (tools/capture_probe_report.py reads it).
    //                            0: nothing is installed
    // Render targets (src/render_target.h):
    //   RenderTargetRecord=1     record named render targets (register, link
    //                            to a model, release) and the UI apps that
    //                            draw into them (open with entry, close):
    //                            4 detours that forward every call, a poll of
    //                            IS_UIAPP_ACTIVE_BY_HASH; clip section RTGT.
    //                            0: nothing installed
    //   RenderTargetPlayback=1   0 off; 1 re-register and relink at the
    //                            playhead; 2 also relaunch the UI app with
    //                            its entry during its span (experimental)
    // The pose fidelity judge (src/pose_fidelity.h):
    //   PoseFidelity=0           1: a recording reads the engine pose of every
    //                            recorded entity (16 key bones and the world
    //                            matrix) and at its stop logs the tracks' error
    //                            against it (POSE_FIDELITY_SUMMARY, mm and
    //                            mrad). Reads only: nothing recorded changes
    std::wstring CurrentClipPathForCensus();

    // Crash evidence. A vectored handler writes one line per hardware fault to a
    // separate file with WriteFile (no stream shared with the script thread, whose
    // last lines may still sit in the ofstream buffer), then continues the search
    // so the game's own handlers still run. Logging only: no engine state is
    // touched. Guarded probes of the mod fault on purpose and appear here too; the
    // process dying right after a line is what tells a crash from a handled probe.
    // The handler must leave with the module: Windows keeps calling a registered
    // vectored handler for every exception in the process, so after a hot reload a
    // stale address would land in the new module's code. Removed in
    // OnModuleDetach, in-flight calls waited for.
    void* g_crashEvidenceHandle = nullptr;
    volatile LONG g_crashEvidenceInFlight = 0;
    struct CrashEvidenceInFlight
    {
        CrashEvidenceInFlight() { InterlockedIncrement(&g_crashEvidenceInFlight); }
        ~CrashEvidenceInFlight() { InterlockedDecrement(&g_crashEvidenceInFlight); }
    };

    // Module names for the handler without the loader lock: GetModuleFileNameA
    // takes it, and DllMain holds it while it removes this handler, so a fault in a
    // guarded read during a Ctrl+R would deadlock the unload. The table is filled
    // on the script thread; the handler only reads it.
    struct KnownModule
    {
        std::uintptr_t base = 0;
        std::uintptr_t size = 0;
        char name[48] = {};
    };
    KnownModule g_knownModules[256];
    volatile LONG g_knownModuleCount = 0;
}

    void RefreshKnownModules()
    {
        HMODULE modules[256]{};
        DWORD needed = 0;
        if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
            return;
        const int count = (std::min)(static_cast<int>(needed / sizeof(HMODULE)), 256);
        InterlockedExchange(&g_knownModuleCount, 0);
        int kept = 0;
        for (int n = 0; n < count; ++n)
        {
            MODULEINFO info{};
            char path[MAX_PATH]{};
            if (!K32GetModuleInformation(GetCurrentProcess(), modules[n], &info, sizeof(info)) ||
                !GetModuleFileNameA(modules[n], path, MAX_PATH))
                continue;
            const char* name = path;
            for (const char* p = path; *p; ++p)
                if (*p == '\\' || *p == '/')
                    name = p + 1;
            KnownModule& known = g_knownModules[kept++];
            known.base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
            known.size = info.SizeOfImage;
            strncpy_s(known.name, name, _TRUNCATE);
        }
        InterlockedExchange(&g_knownModuleCount, kept);
    }
namespace
{

    const char* KnownModuleName(std::uintptr_t address, std::uintptr_t& base)
    {
        const LONG count = InterlockedCompareExchange(&g_knownModuleCount, 0, 0);
        for (LONG n = 0; n < count; ++n)
        {
            const KnownModule& known = g_knownModules[n];
            if (address >= known.base && address < known.base + known.size)
            {
                base = known.base;
                return known.name;
            }
        }
        return "";
    }

    // ------------------------------------------------------------------
    // Crash evidence, per fatal first-chance exception (a stack captured inside the
    // handler itself stops at the bad RIP and loses the caller):
    //   EXCEPTION     the faulting address and stack line (the bench counts it),
    //                 plus the access (read/write/execute), every parameter, the
    //                 thread's name and whether a __try of this mod takes it
    //   CRASH_REGS    every general register of the faulting context
    //   CRASH_STACK   [RSP] and the 31 slots above it, module+offset when
    //                 they point into a loaded image (the return address of
    //                 a call that jumped into the heap is [RSP])
    //   CRASH_UNWIND  the faulting thread's own call stack, unwound from the
    //                 context (RtlVirtualUnwind; a frame without unwind data,
    //                 like code in the heap, is taken as a leaf: return at [RSP])
    // and, for a real crash (no __try of this mod on the faulting stack), a
    // minidump in %LOCALAPPDATA%\RedemptionEditor\crashes (the last 5 kept),
    // written by a thread made at load: the faulting thread only signals it
    // and waits (60 s at most). Nothing here allocates: static buffers under
    // a one-owner lock, snprintf into them, WriteFile. A guarded probe of the
    // mod (they fault on purpose) gets the first line only.
    // ------------------------------------------------------------------
    namespace crashdump
    {
        using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
            PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
        using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);

        constexpr LONG kMaxDumpsPerLoad = 2;
        constexpr std::size_t kKeepDumps = 5;
        constexpr DWORD kDumpWaitMs = 60000;
        // dbghelp's own size choice: the stacks plus the memory their pointers reach
        // (1 KB around each), thread times and start addresses, the unloaded modules
        // (hot reloads). No data segments, no heap: tens of MB for RDR2's ~150
        // threads, not gigabytes. And the MemoryInfoList (MiniDumpWithFullMemoryInfo:
        // every region's base, allocation base, state, protection and type, no
        // contents; a few hundred KB), so a dump tells a freed block from a never
        // mapped one.
        constexpr MINIDUMP_TYPE kDumpType = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
            MiniDumpWithUnloadedModules | MiniDumpIgnoreInaccessibleMemory | MiniDumpWithFullMemoryInfo);

        MiniDumpWriteDumpFn g_writeDump = nullptr;
        GetThreadDescriptionFn g_getDescription = nullptr;
        wchar_t g_folder[MAX_PATH] = {};
        HANDLE g_request = nullptr; // auto reset: the faulting thread asks
        HANDLE g_done = nullptr;    // auto reset: the dump thread answers
        HANDLE g_thread = nullptr;
        DWORD g_threadId = 0;
        std::atomic<bool> g_threadExited{false};
        volatile LONG g_stop = 0;
        volatile LONG g_busy = 0;
        volatile LONG g_written = 0;
        // The request's own copy of the exception (crash_dump_copy.h): the
        // faulting thread's records may be reused once its handler returned.
        replay::crash::OwnedException g_requestException = {};
        EXCEPTION_POINTERS* g_requestPointers = nullptr; // &g_requestException.pointers, or null
        DWORD g_requestThread = 0;
        char g_requestEvidence[12288] = {};
        int g_requestLength = 0;
    }

    // One owner at a time formats into the static buffers; a fault inside
    // the handler (a guarded read below) finds itself the owner and leaves.
    volatile LONG g_crashFormatOwner = 0;
    char g_crashText[12288] = {};
    CONTEXT g_crashUnwind = {};

    bool AcquireCrashFormat()
    {
        const LONG self = static_cast<LONG>(GetCurrentThreadId());
        for (int spin = 0; spin < 3000; ++spin)
        {
            const LONG owner = InterlockedCompareExchange(&g_crashFormatOwner, self, 0);
            if (owner == 0)
                return true;
            if (owner == self)
                return false;
            Sleep(spin < 100 ? 0 : 1);
        }
        return false;
    }

    void ReleaseCrashFormat() { InterlockedExchange(&g_crashFormatOwner, 0); }

    // The image's name from its export directory (an image loaded after the
    // last RefreshKnownModules): no loader lock, guarded reads.
    const char* ExportDirectoryName(std::uintptr_t base)
    {
        static std::uintptr_t bases[16] = {};
        static char names[16][40] = {};
        static volatile LONG next = 0;
        for (int n = 0; n < 16; ++n)
            if (bases[n] == base)
                return names[n];
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!replay::mem::Copy(&dos, base, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            !replay::mem::Copy(&nt, base + static_cast<std::uintptr_t>(dos.e_lfanew), sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE)
            return "";
        const IMAGE_DATA_DIRECTORY& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY))
            return "";
        IMAGE_EXPORT_DIRECTORY exports{};
        if (!replay::mem::Copy(&exports, base + dir.VirtualAddress, sizeof(exports)) || !exports.Name)
            return "";
        const int slot = static_cast<int>(InterlockedIncrement(&next) & 15);
        char* name = names[slot];
        name[0] = 0;
        if (!replay::mem::Copy(name, base + exports.Name, sizeof(names[slot]) - 1))
            return "";
        name[sizeof(names[slot]) - 1] = 0;
        bases[slot] = base;
        return name;
    }

    // "RDR2.exe+0x1A2B" for an address inside a loaded image, "" otherwise.
    const char* ImageOf(std::uintptr_t address, std::uintptr_t& base)
    {
        base = 0;
        if (address < replay::mem::kLowest || address >= replay::mem::kUserEnd)
            return "";
        const char* name = KnownModuleName(address, base);
        if (*name)
            return name;
        PVOID header = nullptr;
        if (!RtlPcToFileHeader(reinterpret_cast<PVOID>(address), &header) || !header)
            return "";
        base = reinterpret_cast<std::uintptr_t>(header);
        name = ExportDirectoryName(base);
        return *name ? name : "image";
    }

    int FormatAddress(char* out, std::size_t size, std::uintptr_t address)
    {
        std::uintptr_t base = 0;
        const char* name = ImageOf(address, base);
        if (*name)
            return std::snprintf(out, size, "%.40s+0x%llX", name, static_cast<unsigned long long>(address - base));
        return std::snprintf(out, size, "0x%llX", static_cast<unsigned long long>(address));
    }

    // Appends to g_crashText (never past its end).
    struct CrashText
    {
        int used = 0;
        void Add(const char* format, ...)
        {
            if (used >= static_cast<int>(sizeof(g_crashText)) - 1)
                return;
            va_list args;
            va_start(args, format);
            const int wrote = std::vsnprintf(g_crashText + used, sizeof(g_crashText) - static_cast<std::size_t>(used), format, args);
            va_end(args);
            if (wrote > 0)
                used = (std::min)(used + wrote, static_cast<int>(sizeof(g_crashText)) - 1);
        }
        void Address(std::uintptr_t address)
        {
            if (used >= static_cast<int>(sizeof(g_crashText)) - 1)
                return;
            const int wrote = FormatAddress(g_crashText + used, sizeof(g_crashText) - static_cast<std::size_t>(used), address);
            if (wrote > 0)
                used = (std::min)(used + wrote, static_cast<int>(sizeof(g_crashText)) - 1);
        }
    };

    // One unwind step of g_crashUnwind (the faulting thread's context, on its
    // own stack). `guarded`: this frame is in this module and inside one of
    // its __except scopes. False when the frame cannot be unwound. The
    // memory reads are guarded (a fault re-enters the handler, which leaves
    // at once: the format lock is this thread's).
    bool UnwindCrashFrame(std::uintptr_t stackLow, std::uintptr_t stackHigh, bool& guarded)
    {
        guarded = false;
        __try
        {
            CONTEXT& context = g_crashUnwind;
            const DWORD64 pc = context.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(pc, &imageBase, nullptr);
            if (!function)
            {
                // A leaf, or code without unwind data (the heap): return at [rsp].
                if (context.Rsp < stackLow || context.Rsp + 8 > stackHigh)
                    return false;
                context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                context.Rsp += 8;
                return context.Rip != 0;
            }
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            const PEXCEPTION_ROUTINE handler =
                RtlVirtualUnwind(UNW_FLAG_EHANDLER, imageBase, pc, function, &context, &handlerData, &establisher, nullptr);
            const std::uintptr_t self = reinterpret_cast<std::uintptr_t>(&__ImageBase);
            if (handler && handlerData && imageBase == self)
            {
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(self + __ImageBase.e_lfanew);
                const std::uint32_t count = *static_cast<const std::uint32_t*>(handlerData);
                const auto* records = reinterpret_cast<const replay::crash::ScopeRecord*>(static_cast<const std::uint8_t*>(handlerData) + 4);
                guarded = replay::crash::ScopeCoversPc(count, records, nt->OptionalHeader.SizeOfImage, static_cast<std::uint32_t>(pc - imageBase));
            }
            return context.Rsp >= stackLow && context.Rsp < stackHigh && context.Rip != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadStackSlot(std::uintptr_t address, std::uint64_t& value)
    {
        return replay::mem::Copy(&value, address, sizeof(value));
    }

    // The crash log's path in the base folder, set at load (the handler builds no string).
    wchar_t g_crashLogPath[MAX_PATH * 2]{};

    void AppendCrashLog(const char* text, int length)
    {
        if (length <= 0)
            return;
        HANDLE file = g_crashLogPath[0]
            ? CreateFileW(g_crashLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)
            : CreateFileA(replay::identity::kCrashLogFile, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;
        DWORD wrote = 0;
        WriteFile(file, text, static_cast<DWORD>(length), &wrote, nullptr);
        CloseHandle(file);
    }

    // The dump thread (made at load, so the crash needs no thread creation).
    // It runs MiniDumpWriteDump for the thread that faulted while that one
    // waits on g_done: the dump then has the exception record and the
    // faulting thread's real context, and the work runs on a healthy stack
    // (a stack overflow has none left).
    void WriteRequestedDump()
    {
        using namespace crashdump;
        const ULONGLONG started = GetTickCount64();
        SYSTEMTIME now{};
        GetLocalTime(&now);
        // Keep the newest kKeepDumps - 1 of the old ones.
        {
            wchar_t pattern[MAX_PATH + 8]{};
            swprintf_s(pattern, L"%ls*.dmp", g_folder);
            WIN32_FIND_DATAW found{};
            wchar_t names[64][MAX_PATH]{};
            std::uint64_t stamps[64]{};
            std::size_t count = 0;
            HANDLE find = FindFirstFileW(pattern, &found);
            if (find != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if (count < 64 && !(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    {
                        wcsncpy_s(names[count], found.cFileName, _TRUNCATE);
                        stamps[count] = (static_cast<std::uint64_t>(found.ftLastWriteTime.dwHighDateTime) << 32) | found.ftLastWriteTime.dwLowDateTime;
                        ++count;
                    }
                } while (FindNextFileW(find, &found));
                FindClose(find);
            }
            std::size_t remove[64]{};
            const std::size_t removing = replay::crash::DumpsToDelete(stamps, count, kKeepDumps, remove, 64);
            for (std::size_t n = 0; n < removing; ++n)
            {
                wchar_t path[MAX_PATH * 2]{};
                swprintf_s(path, L"%ls%ls", g_folder, names[remove[n]]);
                DeleteFileW(path);
                // its evidence text too
                wchar_t text[MAX_PATH * 2 + 8]{};
                swprintf_s(text, L"%ls.txt", path);
                DeleteFileW(text);
            }
        }
        wchar_t name[160]{};
        replay::crash::DumpFileName(name, 160, replay::build::kVersion, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
            GetCurrentProcessId(), g_requestThread);
        wchar_t path[MAX_PATH * 2]{};
        swprintf_s(path, L"%ls%ls", g_folder, name);
        // The faulting thread's description (the editor core's threads and
        // the game's named threads have one; ours are in the table too).
        char description[96] = "none";
        if (g_getDescription)
        {
            if (HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, g_requestThread))
            {
                PWSTR text = nullptr;
                if (SUCCEEDED(g_getDescription(thread, &text)) && text)
                {
                    if (text[0])
                        WideCharToMultiByte(CP_UTF8, 0, text, -1, description, sizeof(description) - 1, nullptr, nullptr);
                    LocalFree(text);
                }
                CloseHandle(thread);
            }
        }
        DWORD error = 0;
        bool ok = false;
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            error = GetLastError();
        else
        {
            MINIDUMP_EXCEPTION_INFORMATION exception{};
            exception.ThreadId = g_requestThread;
            exception.ExceptionPointers = g_requestPointers;
            exception.ClientPointers = FALSE;
            ok = g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, kDumpType, g_requestPointers ? &exception : nullptr, nullptr,
                     nullptr) != FALSE;
            if (!ok)
                error = GetLastError();
            CloseHandle(file);
            if (!ok)
                DeleteFileW(path);
        }
        LARGE_INTEGER size{};
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (ok && GetFileAttributesExW(path, GetFileExInfoStandard, &data))
            {
                size.HighPart = static_cast<LONG>(data.nFileSizeHigh);
                size.LowPart = data.nFileSizeLow;
            }
        }
        char pathUtf8[MAX_PATH * 2]{};
        WideCharToMultiByte(CP_UTF8, 0, path, -1, pathUtf8, sizeof(pathUtf8) - 1, nullptr, nullptr);
        char line[1024]{};
        const int length = std::snprintf(line, sizeof(line),
            "%04u-%02u-%02u %02u:%02u:%02u.%03u CRASH_DUMP,ok=%d,thread=%lu,thread_description=%s,path=%s,bytes=%lld,ms=%llu,error=%lu\n",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, ok ? 1 : 0,
            static_cast<unsigned long>(g_requestThread), description, ok ? pathUtf8 : "none", static_cast<long long>(size.QuadPart),
            static_cast<unsigned long long>(GetTickCount64() - started), static_cast<unsigned long>(error));
        AppendCrashLog(line, length);
        // The evidence lines and the thread names next to the dump.
        if (ok)
        {
            wchar_t textPath[MAX_PATH * 2 + 8]{};
            swprintf_s(textPath, L"%ls.txt", path);
            HANDLE text = CreateFileW(textPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (text != INVALID_HANDLE_VALUE)
            {
                DWORD wrote = 0;
                WriteFile(text, g_requestEvidence, static_cast<DWORD>(g_requestLength), &wrote, nullptr);
                WriteFile(text, line, static_cast<DWORD>(length > 0 ? length : 0), &wrote, nullptr);
                char names[2048]{};
                const int namesLength = std::snprintf(names, sizeof(names), "THREAD_NAMES,module=RedemptionEditor.asi,%s\n",
                    replay::threads::MapText().c_str());
                WriteFile(text, names, static_cast<DWORD>(namesLength > 0 ? namesLength : 0), &wrote, nullptr);
                CloseHandle(text);
            }
        }
    }

    DWORD WINAPI CrashDumpThreadMain(void*)
    {
        using namespace crashdump;
        replay::threads::Name("crash_dump");
        for (;;)
        {
            WaitForSingleObject(g_request, INFINITE);
            if (InterlockedCompareExchange(&g_stop, 0, 0) != 0)
                break;
            WriteRequestedDump();
            SetEvent(g_done);
        }
        replay::threads::Forget();
        g_threadExited.store(true, std::memory_order_release); // last statement (module_lifetime.h)
        return 0;
    }

    // From the faulting thread, under the format lock: takes the one
    // request slot (false: no dump thread, the load's dumps are used up, or
    // another thread's dump runs; this thread's stack is in that one).
    bool ClaimCrashDump(EXCEPTION_POINTERS* pointers, const char* evidence, int length)
    {
        using namespace crashdump;
        if (!g_thread || !g_writeDump || GetCurrentThreadId() == g_threadId)
            return false;
        if (InterlockedCompareExchange(&g_written, 0, 0) >= kMaxDumpsPerLoad)
            return false;
        if (InterlockedCompareExchange(&g_busy, 1, 0) != 0)
            return false;
        InterlockedIncrement(&g_written);
        // Copied before the dump thread is signalled; after a timeout the
        // handler returns and only these copies are read (g_busy keeps them).
        g_requestPointers = replay::crash::CopyException(pointers, g_requestException) ? &g_requestException.pointers : nullptr;
        g_requestThread = GetCurrentThreadId();
        g_requestLength = (std::min)(length, static_cast<int>(sizeof(g_requestEvidence)));
        if (g_requestLength > 0)
            std::memcpy(g_requestEvidence, evidence, static_cast<std::size_t>(g_requestLength));
        return true;
    }

    // Then, with the lock released: the dump thread writes, this one waits.
    void WaitCrashDump()
    {
        using namespace crashdump;
        SetEvent(g_request);
        if (WaitForSingleObject(g_done, kDumpWaitMs) == WAIT_OBJECT_0)
            InterlockedExchange(&g_busy, 0);
        // On a timeout g_busy stays set: the dump thread may still read the
        // request, nothing else may overwrite it.
    }

    LONG WINAPI CrashEvidenceHandler(EXCEPTION_POINTERS* pointers)
    {
        CrashEvidenceInFlight inFlight;
        if (InterlockedCompareExchange(&g_moduleDetaching, 0, 0) != 0)
            return EXCEPTION_CONTINUE_SEARCH;
        static volatile LONG guardedLines = 0;
        static volatile LONG crashLines = 0;
        if (!pointers || !pointers->ExceptionRecord || !pointers->ContextRecord)
            return EXCEPTION_CONTINUE_SEARCH;
        const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
        const DWORD code = record.ExceptionCode;
        const bool fatalClass = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
            code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_PRIV_INSTRUCTION ||
            code == EXCEPTION_IN_PAGE_ERROR || code == 0xC0000409 /* fail fast */;
        if (!fatalClass || (crashLines >= 16 && guardedLines >= 32))
            return EXCEPTION_CONTINUE_SEARCH;
        if (!AcquireCrashFormat())
            return EXCEPTION_CONTINUE_SEARCH; // a fault inside this handler, or another thread stuck in it
        const CONTEXT& context = *pointers->ContextRecord;
        const DWORD threadId = GetCurrentThreadId();
        const char* threadName = replay::threads::Lookup(threadId);
        // The faulting thread's stack (the handler runs on it; a fiber's TIB
        // holds the fiber's stack).
        const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
        const std::uintptr_t stackLow = reinterpret_cast<std::uintptr_t>(tib->StackLimit);
        const std::uintptr_t stackHigh = reinterpret_cast<std::uintptr_t>(tib->StackBase);

        // Unwind first (program counters only): it decides between a guarded
        // probe and a crash before anything is formatted.
        static std::uintptr_t pcs[40];
        int pcCount = 0;
        bool guarded = false;
        int guardFrame = -1;
        {
            std::memcpy(&g_crashUnwind, &context, sizeof(CONTEXT));
            for (int n = 0; n < 40; ++n)
            {
                pcs[pcCount++] = static_cast<std::uintptr_t>(g_crashUnwind.Rip);
                bool here = false;
                const bool more = UnwindCrashFrame(stackLow, stackHigh, here);
                if (here)
                {
                    guarded = true;
                    guardFrame = n;
                    break; // a __try of this mod takes it: the rest is not needed
                }
                if (!more)
                    break;
            }
        }
        if (guarded ? InterlockedIncrement(&guardedLines) > 32 : InterlockedIncrement(&crashLines) > 16)
        {
            ReleaseCrashFormat();
            return EXCEPTION_CONTINUE_SEARCH;
        }
        char frames[1600]{};
        if (!guarded)
        {
            int used = 0;
            for (int n = 0; n < pcCount && used < static_cast<int>(sizeof(frames)) - 64; ++n)
            {
                if (n)
                    frames[used++] = '|';
                const int wrote = FormatAddress(frames + used, sizeof(frames) - static_cast<std::size_t>(used), pcs[n]);
                if (wrote > 0)
                    used = (std::min)(used + wrote, static_cast<int>(sizeof(frames)) - 1);
            }
        }

        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(record.ExceptionAddress);
        std::uintptr_t moduleBase = 0;
        const char* name = KnownModuleName(address, moduleBase);
        if (!*name)
            name = ImageOf(address, moduleBase);
        std::uintptr_t fault = 0;
        if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) && record.NumberParameters >= 2)
            fault = static_cast<std::uintptr_t>(record.ExceptionInformation[1]);
        const char* access = replay::crash::AccessName(code, record.NumberParameters,
            record.NumberParameters ? static_cast<std::uint64_t>(record.ExceptionInformation[0]) : 0);
        SYSTEMTIME now{};
        GetLocalTime(&now);
        // The handler's own stack (through the dispatcher).
        char handlerStack[640]{};
        {
            void* stack[24]{};
            const USHORT captured = RtlCaptureStackBackTrace(0, 24, stack, nullptr);
            int used = 0;
            for (USHORT n = 0; n < captured && used < static_cast<int>(sizeof(handlerStack)) - 48; ++n)
            {
                std::uintptr_t frameBase = 0;
                const char* frameName = KnownModuleName(reinterpret_cast<std::uintptr_t>(stack[n]), frameBase);
                const int wrote = std::snprintf(handlerStack + used, sizeof(handlerStack) - used, "%s%.12s+0x%llX", n ? "|" : "",
                    *frameName ? frameName : "?", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(stack[n]) - frameBase));
                if (wrote <= 0)
                    break;
                used += wrote;
            }
        }
        CrashText text;
        const unsigned long thread = static_cast<unsigned long>(threadId);
        text.Add("%04u-%02u-%02u %02u:%02u:%02u.%03u EXCEPTION,code=0x%08lX,address=0x%llX,module=%s,offset=0x%llX,fault_address=0x%llX,"
                 "access=%s,flags=0x%lX,params=%lu",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, static_cast<unsigned long>(code),
            static_cast<unsigned long long>(address), *name ? name : "unknown", static_cast<unsigned long long>(moduleBase ? address - moduleBase : 0),
            static_cast<unsigned long long>(fault), access, static_cast<unsigned long>(record.ExceptionFlags),
            static_cast<unsigned long>(record.NumberParameters));
        for (DWORD n = 0; n < record.NumberParameters && n < EXCEPTION_MAXIMUM_PARAMETERS; ++n)
            text.Add(",info%lu=0x%llX", static_cast<unsigned long>(n), static_cast<unsigned long long>(record.ExceptionInformation[n]));
        text.Add(",thread=%lu,thread_name=%s,guarded=%d,guard_frame=%d,build=%s,stack=%s\n", thread, *threadName ? threadName : "unknown",
            guarded ? 1 : 0, guardFrame, replay::build::kVersion, handlerStack);
        if (!guarded)
        {
            text.Add("CRASH_REGS,thread=%lu,rip=", thread);
            text.Address(static_cast<std::uintptr_t>(context.Rip));
            text.Add(",rsp=0x%llX,rbp=0x%llX,rax=0x%llX,rbx=0x%llX,rcx=0x%llX,rdx=0x%llX,rsi=0x%llX,rdi=0x%llX,r8=0x%llX,r9=0x%llX,"
                     "r10=0x%llX,r11=0x%llX,r12=0x%llX,r13=0x%llX,r14=0x%llX,r15=0x%llX,eflags=0x%lX,stack_low=0x%llX,stack_high=0x%llX\n",
                context.Rsp, context.Rbp, context.Rax, context.Rbx, context.Rcx, context.Rdx, context.Rsi, context.Rdi, context.R8, context.R9,
                context.R10, context.R11, context.R12, context.R13, context.R14, context.R15, static_cast<unsigned long>(context.EFlags),
                static_cast<unsigned long long>(stackLow), static_cast<unsigned long long>(stackHigh));
            // [RSP] first: after a call into a bad pointer it is the return
            // address of that call, i.e. the caller.
            text.Add("CRASH_STACK,thread=%lu,rsp=0x%llX", thread, context.Rsp);
            for (int slot = 0; slot < 32; ++slot)
            {
                const std::uintptr_t at = static_cast<std::uintptr_t>(context.Rsp) + static_cast<std::uintptr_t>(slot) * 8;
                std::uint64_t value = 0;
                text.Add(",+%02X=", slot * 8);
                if (at < stackLow || at + 8 > stackHigh || !ReadStackSlot(at, value))
                {
                    text.Add("unreadable");
                    continue;
                }
                text.Address(static_cast<std::uintptr_t>(value));
            }
            text.Add("\nCRASH_UNWIND,thread=%lu,frames=%s\n", thread, frames);
            // The page of the fault address as the system has it now (VirtualQuery: no
            // allocation, safe here). free = never mapped or released; reserve = an
            // allocation whose pages were decommitted; commit with PAGE_NOACCESS or a
            // guard = protected.
            if (fault)
            {
                MEMORY_BASIC_INFORMATION page{};
                const SIZE_T got = VirtualQuery(reinterpret_cast<const void*>(fault), &page, sizeof(page));
                const char* stateName = !got ? "unknown" : page.State == MEM_COMMIT ? "commit"
                    : page.State == MEM_RESERVE ? "reserve" : page.State == MEM_FREE ? "free" : "other";
                const char* typeName = !got || page.State == MEM_FREE ? "none" : page.Type == MEM_IMAGE ? "image"
                    : page.Type == MEM_MAPPED ? "mapped" : page.Type == MEM_PRIVATE ? "private" : "other";
                text.Add("CRASH_FAULT_PAGE,thread=%lu,fault_address=0x%llX,query=%d,base=0x%llX,allocation_base=0x%llX,"
                         "region_size=0x%llX,state=%s,state_raw=0x%lX,protect=0x%lX,allocation_protect=0x%lX,type=%s,type_raw=0x%lX\n",
                    thread, static_cast<unsigned long long>(fault), got ? 1 : 0,
                    static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(page.BaseAddress)),
                    static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(page.AllocationBase)),
                    static_cast<unsigned long long>(page.RegionSize), stateName, static_cast<unsigned long>(page.State),
                    static_cast<unsigned long>(page.Protect), static_cast<unsigned long>(page.AllocationProtect), typeName,
                    static_cast<unsigned long>(page.Type));
            }
            // The last DRAWSTORE_SLOT line (a copy kept by the script thread; no read of
            // the engine here).
            {
                char drawStore[1024];
                if (replay::drawstore::CopyLastLine(drawStore, static_cast<int>(sizeof(drawStore))) > 0)
                    text.Add("CRASH_DRAWSTORE,last=%s\n", drawStore);
            }
        }
        AppendCrashLog(g_crashText, text.used);
        // A real crash: the request (and its copy of these lines) is taken
        // under the format lock, the wait for the dump outside it.
        const bool requested = !guarded && ClaimCrashDump(pointers, g_crashText, text.used);
        ReleaseCrashFormat();
        if (requested)
            WaitCrashDump();
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // The dump folder, dbghelp and the dump thread, once per load.
    void StartCrashDumps()
    {
        using namespace crashdump;
        if (g_thread)
            return;
        wchar_t local[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        if (length == 0 || length >= MAX_PATH - 40)
            return;
        swprintf_s(g_folder, L"%ls\\RedemptionEditor", local);
        CreateDirectoryW(g_folder, nullptr);
        swprintf_s(g_folder, L"%ls\\RedemptionEditor\\crashes\\", local);
        CreateDirectoryW(g_folder, nullptr);
        if (HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            g_writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
        g_getDescription = reinterpret_cast<GetThreadDescriptionFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
        if (!g_writeDump)
            return;
        g_request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_request || !g_done)
            return;
        InterlockedExchange(&g_stop, 0);
        g_threadExited.store(false, std::memory_order_release);
        g_thread = CreateThread(nullptr, 0, &CrashDumpThreadMain, nullptr, 0, &g_threadId);
    }

    void StopCrashDumps()
    {
        using namespace crashdump;
        if (g_thread)
        {
            InterlockedExchange(&g_stop, 1);
            SetEvent(g_request);
            if (!replay::lifetime::StopWait(g_thread, g_threadExited, 2000, "crash_dump"))
                replay::lifetime::NoteUnloadTimeout("crash_dump");
            CloseHandle(g_thread);
            g_thread = nullptr;
            g_threadId = 0;
        }
        if (g_request)
            CloseHandle(g_request);
        if (g_done)
            CloseHandle(g_done);
        g_request = nullptr;
        g_done = nullptr;
    }
}

    void InstallCrashEvidence()
    {
        if (!g_crashLogPath[0])
            wcsncpy_s(g_crashLogPath, replay::storage::Path(replay::identity::AsciiW(replay::identity::kCrashLogFile)).c_str(), _TRUNCATE);
        RefreshKnownModules();
        replay::threads::NoteForeign("script"); // the game's main thread (the script fiber runs on it)
        StartCrashDumps();
        if (!g_crashEvidenceHandle)
            g_crashEvidenceHandle = AddVectoredExceptionHandler(0, &CrashEvidenceHandler); // last in line: the game's handlers see the fault first
    }

    void RemoveCrashEvidence()
    {
        if (!g_crashEvidenceHandle)
            return;
        RemoveVectoredExceptionHandler(g_crashEvidenceHandle);
        g_crashEvidenceHandle = nullptr;
        for (int wait = 0; wait < 200 && InterlockedCompareExchange(&g_crashEvidenceInFlight, 0, 0) > 0; ++wait)
            Sleep(1);
        StopCrashDumps();
    }
namespace
{

    // The CRASH_DUMPS line of the log header: where the dumps go.
    std::string CrashDumpFolderUtf8()
    {
        char text[MAX_PATH * 2]{};
        WideCharToMultiByte(CP_UTF8, 0, crashdump::g_folder, -1, text, sizeof(text) - 1, nullptr, nullptr);
        return text[0] ? text : "none";
    }

    // The ASI file this module was loaded from and its SHA-256 (src/sha256.h,
    // read once at load), so a log and a report name the exact build.
    std::string g_asiSha256;
    std::wstring g_asiPath;
    std::string g_iniKeysText; // the [RedemptionEditor] section as read at load (report.txt)

    std::string FileSha256(const std::wstring& path, std::uint64_t& bytes)
    {
        bytes = 0;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return "unreadable";
        const std::string text = replay::sha256::HashOpenFile(file, bytes, 1 << 16);
        CloseHandle(file);
        return text;
    }

    // The [RedemptionEditor] section as read ("key=value;..."), so a report shows
    // the settings the session ran with.
    std::string IniSectionText(const std::string& configPath, bool& found)
    {
        found = GetFileAttributesA(configPath.c_str()) != INVALID_FILE_ATTRIBUTES;
        std::vector<char> section(8192);
        const DWORD length = replay::identity::IniSection(section.data(), static_cast<DWORD>(section.size()), configPath.c_str());
        std::string text;
        for (DWORD at = 0; at < length && section[at];)
        {
            const std::string pair(section.data() + at);
            text += (text.empty() ? "" : ";") + pair;
            at += static_cast<DWORD>(pair.size()) + 1;
        }
        return text;
    }
}

    // The free camera's options, FreeCam* keys of [RedemptionEditor] (the editor's
    // Free Camera rows write them back) and SetFreeCamOption's fields.
    float g_fcNearClip = 0.1f; // FreeCamNearClip, the flying free camera's near clip
namespace
{
    struct FreeCamFieldInfo
    {
        const char* iniKey;
        float minimum;
        float maximum;
    };
    constexpr FreeCamFieldInfo kFreeCamFields[static_cast<std::size_t>(replay::editor::FreeCamField::Count)] = {
        {"FreeCamMotion", 0.0f, 1.0f},          // 0 Classic, 1 Drone
        {"", 0.0f, 1.0f},                       // Acrobatic: not in the ini (off at start; the N key and the menu, this session only)
        {"FreeCamWalk", 0.0f, 1.0f},
        {"FreeCamWalkHeight", 0.3f, 5.0f},
        {"FreeCamLockAltitude", 0.0f, 1.0f},
        {"FreeCamCollision", 0.0f, 1.0f},
        {"", 0.0f, 2.0f},                       // Follow: not saved (a target is per clip)
        {"FreeCamDroneDrag", 0.2f, 12.0f},
        {"FreeCamDroneAcceleration", 2.0f, 60.0f},
        {"FreeCamDroneGravity", 0.0f, 20.0f},
        {"FreeCamDroneBanking", 0.0f, 45.0f},
        {"FreeCamDroneRotSmoothing", 1.0f, 30.0f},
        {"FreeCamDroneFovSmoothing", 0.0f, 20.0f},
        {"FreeCamLookSensitivity", 2.0f, 30.0f},
        {"", 0.0f, 1.0f},                       // ResetDefaults
        {"FreeCamNearClip", 0.01f, 1.0f},       // metres
    };
}

    float FreeCamValue(replay::editor::FreeCamField field)
    {
        using F = replay::editor::FreeCamField;
        const replay::freecam::Settings& s = g_fcSettings;
        switch (field)
        {
        case F::Motion: return s.motion == replay::freecam::Motion::Drone ? 1.0f : 0.0f;
        case F::Acrobatic: return s.acrobatic ? 1.0f : 0.0f;
        case F::Walk: return s.walk ? 1.0f : 0.0f;
        case F::WalkHeight: return s.walkHeight;
        case F::LockAltitude: return s.lockAltitude ? 1.0f : 0.0f;
        case F::Collision: return s.collision ? 1.0f : 0.0f;
        case F::Follow: return static_cast<float>(g_fcFollow);
        case F::DroneDrag: return s.droneDrag;
        case F::DroneAcceleration: return s.droneAcceleration;
        case F::DroneGravity: return s.droneGravity;
        case F::DroneBanking: return s.droneBanking;
        case F::DroneRotSmoothing: return s.droneRotSmoothing;
        case F::DroneFovSmoothing: return s.droneFovSmoothing;
        case F::LookSensitivity: return s.lookSensitivity;
        case F::NearClip: return g_fcNearClip;
        default: return 0.0f;
        }
    }

    // The value held in the field's range; false when the field is unknown.
    bool SetFreeCamValue(replay::editor::FreeCamField field, float value)
    {
        using F = replay::editor::FreeCamField;
        const std::size_t index = static_cast<std::size_t>(field);
        if (index >= static_cast<std::size_t>(F::Count) || !std::isfinite(value))
            return false;
        if (field == F::ResetDefaults)
        {
            const float speedScale = g_fcSettings.speedScale;
            g_fcSettings = replay::freecam::Settings{};
            g_fcSettings.speedScale = speedScale;
            g_fcNearClip = replay::editor::kNearClipDefault;
            return true;
        }
        value = std::clamp(value, kFreeCamFields[index].minimum, kFreeCamFields[index].maximum);
        replay::freecam::Settings& s = g_fcSettings;
        const bool on = value >= 0.5f;
        switch (field)
        {
        case F::Motion: s.motion = on ? replay::freecam::Motion::Drone : replay::freecam::Motion::Classic; break;
        case F::Acrobatic: s.acrobatic = on; break;
        case F::Walk: s.walk = on; break;
        case F::WalkHeight: s.walkHeight = value; break;
        case F::LockAltitude: s.lockAltitude = on; break;
        case F::Collision: s.collision = on; break;
        case F::Follow:
            g_fcFollow = static_cast<replay::freecam::Follow>(static_cast<int>(value + 0.5f));
            g_fcTargetFrameValid = false;
            break;
        case F::DroneDrag: s.droneDrag = value; break;
        case F::DroneAcceleration: s.droneAcceleration = value; break;
        case F::DroneGravity: s.droneGravity = value; break;
        case F::DroneBanking: s.droneBanking = value; break;
        case F::DroneRotSmoothing: s.droneRotSmoothing = value; break;
        case F::DroneFovSmoothing: s.droneFovSmoothing = value; break;
        case F::LookSensitivity: s.lookSensitivity = value; break;
        case F::NearClip: g_fcNearClip = value; break;
        default: return false;
        }
        return true;
    }
namespace
{

    void LoadFreeCamOptions(const char* configPath)
    {
        for (std::size_t index = 0; index < static_cast<std::size_t>(replay::editor::FreeCamField::Count); ++index)
        {
            const FreeCamFieldInfo& info = kFreeCamFields[index];
            if (!info.iniKey[0])
                continue;
            char text[32]{};
            replay::identity::IniString(info.iniKey, "", text, sizeof(text), configPath);
            if (!text[0])
                continue;
            float value = 0.0f;
            if (_stricmp(text, "drone") == 0)
                value = 1.0f;
            else if (_stricmp(text, "classic") == 0)
                value = 0.0f;
            else
            {
                char* end = nullptr;
                value = std::strtof(text, &end);
                if (end == text)
                    continue;
            }
            SetFreeCamValue(static_cast<replay::editor::FreeCamField>(index), value);
        }
    }
}

    void SaveFreeCamOptions()
    {
        char workingDirectory[MAX_PATH]{};
        GetCurrentDirectoryA(MAX_PATH, workingDirectory);
        const std::string path = replay::identity::IniPathIn(workingDirectory);
        for (std::size_t index = 0; index < static_cast<std::size_t>(replay::editor::FreeCamField::Count); ++index)
        {
            const FreeCamFieldInfo& info = kFreeCamFields[index];
            if (!info.iniKey[0])
                continue;
            const auto field = static_cast<replay::editor::FreeCamField>(index);
            char text[32]{};
            if (field == replay::editor::FreeCamField::Motion)
                std::snprintf(text, sizeof(text), "%s", g_fcSettings.motion == replay::freecam::Motion::Drone ? "drone" : "classic");
            else
                std::snprintf(text, sizeof(text), "%g", FreeCamValue(field));
            replay::identity::IniWrite(info.iniKey, text, path.c_str());
        }
    }
namespace
{

    void LoadSplineDefaults(const char* configPath)
    {
        namespace sp = replay::camera::spline;
        replay::editor::SplineTuning d = replay::editor::DefaultSplineTuning();
        for (std::size_t index = 0; index < sp::kIniKeyCount; ++index)
        {
            char text[32]{};
            replay::identity::IniString(sp::kIniKeys[index], "", text, sizeof(text), configPath);
            sp::IniRead(d, index, text);
        }
        g_splineDefaults = d;
    }
}

    void SaveSplineDefaults()
    {
        namespace sp = replay::camera::spline;
        char workingDirectory[MAX_PATH]{};
        GetCurrentDirectoryA(MAX_PATH, workingDirectory);
        const std::string path = replay::identity::IniPathIn(workingDirectory);
        for (std::size_t index = 0; index < sp::kIniKeyCount; ++index)
            replay::identity::IniWrite(sp::kIniKeys[index], sp::IniValue(g_splineDefaults, index).c_str(), path.c_str());
    }

    void LogSplineDefaults(const char* why)
    {
        const replay::editor::SplineTuning& d = g_splineDefaults;
        LogFile() << "CAMERA_SPLINE,defaults,why=" << why << ",pacing="
                  << replay::camera::spline::PacingName(static_cast<replay::editor::SplinePacing>(d.pacing)) << ",alpha=" << d.alpha
                  << ",ease_in=" << d.easeIn << ",ease_out=" << d.easeOut << ",weight=" << d.weight
                  << ",channels=" << static_cast<int>(d.channels) << '\n';
    }

    void SaveOverlayOptions()
    {
        char workingDirectory[MAX_PATH]{};
        GetCurrentDirectoryA(MAX_PATH, workingDirectory);
        const std::string path = replay::identity::IniPathIn(workingDirectory);
        replay::identity::IniWrite("CameraOverlay", g_camOverlayEnabled ? "1" : "0", path.c_str());
        replay::identity::IniWrite("CameraOverlayWhilePlaying", g_camOverlayWhilePlaying ? "1" : "0", path.c_str());
        replay::identity::IniWrite("CameraOverlayTimeLabels", g_camOverlayTimeLabels ? "1" : "0", path.c_str());
        replay::identity::IniWrite("CameraOverlayTrail", g_camOverlayTrail ? "1" : "0", path.c_str());
    }

    void LogFreeCamConfig(const char* why)
    {
        const replay::freecam::Settings& s = g_fcSettings;
        LogFile() << "FREECAM_CONFIG,why=" << why << ",motion=" << (s.motion == replay::freecam::Motion::Drone ? "drone" : "classic")
                  << ",acrobatic=" << (s.acrobatic ? 1 : 0) << ",walk=" << (s.walk ? 1 : 0) << ",walk_m=" << s.walkHeight
                  << ",lock_altitude=" << (s.lockAltitude ? 1 : 0) << ",collision=" << (s.collision ? 1 : 0)
                  << ",follow=" << static_cast<int>(g_fcFollow) << ",target=" << g_fcTarget << ",drag=" << s.droneDrag
                  << ",accel=" << s.droneAcceleration << ",gravity=" << s.droneGravity << ",banking=" << s.droneBanking
                  << ",rot_smooth=" << s.droneRotSmoothing << ",fov_smooth=" << s.droneFovSmoothing
                  << ",sensitivity=" << s.lookSensitivity << ",overlay=" << (g_camOverlayEnabled ? 1 : 0)
                  << ",overlay_playing=" << (g_camOverlayWhilePlaying ? 1 : 0) << ",overlay_labels=" << (g_camOverlayTimeLabels ? 1 : 0)
                  << ",overlay_trail=" << (g_camOverlayTrail ? 1 : 0) << '\n';
    }

    // The script timecycle modifier and the weather variations are recorded and
    // played back (timecycle_track.h). The sky object's moon angles are recorded
    // (section MOON) and, while a clip sets the clock, kept from moving the
    // session's moon and played back from the clip (MoonReplay, default on;
    // moon_track.h).
    bool g_moonReplay = true;
namespace
{

    // ---------------- Language (src/i18n.h) ----------------
    // RedemptionEditor.ini Language (auto: the game's language), and the game's
    // language read with GET_CURRENT_LANGUAGE every few seconds on this thread;
    // the editor gets it in the snapshot and loads the same file.
    std::string g_languageSetting;
    bool g_languageRead = false;
}
    std::uint32_t g_gameLanguage = 0; // GET_CURRENT_LANGUAGE + 1, 0 not read yet
namespace
{
    std::string g_languageCode = "en";
    ULONGLONG g_languageCheckedAt = 0;
}

    void UpdateLanguage(ULONGLONG now)
    {
        if (!g_languageRead)
        {
            g_languageRead = true;
            char workingDirectory[MAX_PATH]{};
            GetCurrentDirectoryA(MAX_PATH, workingDirectory);
            const std::string configPath = replay::identity::IniPathIn(workingDirectory);
            char value[32]{};
            replay::identity::IniString("Language", "auto", value, sizeof(value), configPath.c_str());
            g_languageSetting = value;
        }
        if (g_languageCheckedAt && now - g_languageCheckedAt < 2000)
            return;
        g_languageCheckedAt = now;
        g_gameLanguage = static_cast<std::uint32_t>(LOCALIZATION::GET_CURRENT_LANGUAGE()) + 1u;
        const std::string code = replay::i18n::Choose(g_languageSetting, g_gameLanguage);
        if (code == g_languageCode)
            return;
        g_languageCode = code;
        // RedemptionEditor\lang next to RDR2.exe.
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring folder = exe;
        folder = folder.substr(0, folder.find_last_of(L"\\/") + 1) + L"RedemptionEditor\\lang\\";
        std::string loaded;
        std::unique_ptr<replay::i18n::Table> table = replay::i18n::Load(folder, code, &loaded);
        LogFile() << "LANGUAGE,code=" << code << ",game=" << static_cast<int>(g_gameLanguage) - 1 << ",setting=" << g_languageSetting
                  << ",file=" << (loaded.empty() ? "none" : loaded) << ",keys=" << (table ? table->Size() : 0u)
                  << ",problems=" << (table ? table->problems.size() : 0u) << '\n';
        if (table)
            for (const std::string& problem : table->problems)
                LogFile() << "LANGUAGE_PROBLEM,file=" << loaded << "," << problem << '\n';
        replay::i18n::SetActive(std::move(table));
    }

    void WriteLogHeader()
    {
        char workingDirectory[MAX_PATH]{};
        GetCurrentDirectoryA(MAX_PATH, workingDirectory);

        const std::string configPath = replay::identity::IniPathIn(workingDirectory);
        g_debugMode = replay::identity::DevIniInt("Debug", 0, configPath.c_str()) != 0;
        UpdateLanguage(GetTickCount64()); // before the [Keys] notice is worded
        // [Keys], the same table the add-on loads (editor_keys.h); logged after the
        // header line.
        std::string keysLog;
        {
            const replay::keys::LoadResult keys = replay::keys::LoadFromIni(configPath.c_str());
            for (const replay::keys::Issue& issue : keys.issues)
                keysLog += replay::keys::LogLine(issue) + "\n";
            std::string table;
            for (const replay::keys::ActionInfo& info : replay::keys::kActions)
            {
                const replay::keys::Binding* bound = keys.table.Of(info.action);
                table += (table.empty() ? "" : ";") + std::string(info.iniKey) + ":";
                if (!bound[0].Valid() && !bound[1].Valid())
                    table += "None";
                for (std::size_t slot = 0; slot < replay::keys::kSlots; ++slot)
                {
                    if (bound[slot].Valid())
                        table += (slot ? "|" : "") + replay::keys::IniName(bound[slot]);
                }
            }
            keysLog += "KEYS_TABLE,configured=" + std::to_string(keys.configured) + ",refused=" +
                std::to_string(keys.issues.size()) + ",keys=" + table + "\n";
            g_keysNotice = replay::keys::NoticeText(keys.issues);
        }
        // FullLog=1 (the default) keeps the whole log; 0 only the essential lines.
        replay::log::SetVerbose(replay::identity::IniInt("FullLog", 1, configPath.c_str()) != 0);
        g_reportIncludeClips = replay::identity::IniInt("ReportIncludeClips", 0, configPath.c_str()) != 0; // release 1.0
        // FREC volume creation on puppets; 0 turns it off.
        replay::burn::SetCharCreate(replay::identity::IniInt("FireCharCreate", 1, configPath.c_str()) != 0);
        // End-of-frame placement and the plants-only body.
        {
            const bool frameEnd = replay::identity::IniInt("FrameEnd", 1, configPath.c_str()) != 0;
            const bool physicsFilter = replay::identity::IniInt("PhysicsFilter", 1, configPath.c_str()) != 0;
            replay::mount::SetFrameEnd(frameEnd);
            replay::collision::Configure(physicsFilter, replay::collision::kDefaultKeep);
            replay::mount::SetPhysicsFilterNote(physicsFilter);
        }
        replay::soundgeneric::Configure(configPath); // SoundGeneric, off by default
        replay::exportprobe::Configure(configPath); // render export sketch: [Export] Enabled, off by default
        g_recConfig.buffer = replay::identity::IniInt("ReplayBuffer", 1, configPath.c_str()) != 0; // on by default
        g_recConfig.bufferSeconds = (std::max)(10u, replay::identity::IniInt("ReplayBufferSeconds", 60, configPath.c_str()));
        g_recConfig.bufferMB = replay::buffercov::ClampBufferMB(replay::identity::IniInt("ReplayBufferMB", 128, configPath.c_str()));
        g_recConfig.manualMaxSeconds = replay::identity::IniInt("ManualMaxSeconds", 0, configPath.c_str());
        g_recConfig.manualMaxMB = replay::buffercov::ClampManualMB(replay::identity::IniInt("ManualMaxMB", 900, configPath.c_str()));
        // clip_start_gate.h.
        {
            replay::clipstart::Config& c = g_clipStartConfig;
            c.worldWait = replay::identity::DevIniInt("ClipWorldWait", 0, configPath.c_str()) != 0;
            c.worldCapMs = std::clamp(replay::identity::DevIniInt("ClipWorldWaitMs", 20000, configPath.c_str()), 3000u, 120000u);
        }
        g_recConfig.replayDeadEye = replay::identity::IniInt("ReplayDeadEye", 0, configPath.c_str()) != 0;
        g_recConfig.compressClips = replay::identity::IniInt("CompressClips", 1, configPath.c_str()) != 0;
        g_recConfig.compressAfterDays = std::clamp(
            replay::identity::IniInt("CompressAfterDays", static_cast<int>(replay::clipcompress::kDefaultDays), configPath.c_str()), 1u, 60u);
        g_recConfig.compressLevel = static_cast<int>(std::clamp(
            replay::identity::IniInt("CompressLevel", replay::clipcompress::kDefaultLevel, configPath.c_str()), 1u, 22u));
        g_recConfig.compressFormat = replay::identity::DevIniInt("CompressFormat", 2, configPath.c_str()) == 1 ? 1 : 2;
        g_recConfig.recordCutscenes = replay::identity::IniInt("RecordCutscenes", 0, configPath.c_str()) != 0;
        g_recConfig.lockCutsceneCamera = replay::identity::IniInt("LockCutsceneCamera", 0, configPath.c_str()) != 0;
        g_recConfig.restoreWorldState = replay::identity::IniInt("RestoreWorldState", 1, configPath.c_str()) != 0;
        g_recConfig.restoreWorldStateInMission =
            replay::identity::IniInt("RestoreWorldStateInMission", 0, configPath.c_str()) != 0;
        g_recConfig.freezeWorldOnPause = replay::identity::IniInt("FreezeWorldOnPause", 1, configPath.c_str()) != 0;
        {
            char value[32]{};
            replay::identity::IniString("RecordingIndicator", "normal", value, sizeof(value), configPath.c_str());
            g_recConfig.recordingIndicator = replay::recindicator::Parse(value);
        }
        {
            char value[32]{};
            replay::identity::IniString("PoseDetail", "standard", value, sizeof(value), configPath.c_str());
            g_recConfig.poseDetail = replay::pose::smooth::DetailFromText(value);
        }
        // The door ratios are read and driven by natives (vehicle_doors_engine.h);
        // this key writes the current ratio of the puppet's own door when the native's
        // step left it off (rung 3).
        replay::vehdoors::SetWrite(replay::identity::IniInt("VehicleDoorWrite", 1, configPath.c_str()) != 0);
        // The pose fidelity judge (off by default).
        replay::pose::fidelity::Configure(replay::identity::DevIniInt("PoseFidelity", 0, configPath.c_str()) != 0,
            replay::pose::fidelity::kDefaultHz);
        {
            // The particle keys.
            const auto key = [&](const char* name, int fallback) {
                return replay::identity::IniInt(name, fallback, configPath.c_str());
            };
            replay::events::FxParticlesConfig fx;
            fx.puppetNoWarp = key("FxPuppetNoWarp", 1) != 0;
            fx.settingsReplay = key("FxSettingsReplay", 1) != 0;
            fx.puppetVelocity = key("FxPuppetVelocity", 1) != 0;
            fx.pauseFreeze = key("FxPauseFreeze", 1) != 0;
            fx.relocateClearStill = key("FxRelocateClearStill", 1) != 0;
            fx.weaponFxIntent = key("WeaponFxIntent", 1) != 0;
            replay::events::SetFxParticlesConfig(fx);
        }
        // Evidence lines of the player's horse audio, record against playback.
        replay::horseaudio::SetEnabled(
            replay::identity::DevIniInt("HorseAudioEvidence", 0, configPath.c_str()) != 0);
        // The mix probe, read only (where each sound is heard from, record against
        // replay).
        replay::mixprobe::SetEnabled(replay::identity::DevIniInt("MixProbe", 0, configPath.c_str()) != 0);
        {
            // Named render targets and the UI apps drawing into them (the camp ledger).
            replay::rendertarget::Settings rt;
            rt.record = replay::identity::IniInt("RenderTargetRecord", 1, configPath.c_str()) != 0;
            rt.mode = replay::rendertarget::ModeFromInt(
                static_cast<int>(replay::identity::IniInt("RenderTargetPlayback", 1, configPath.c_str())));
            replay::rendertarget::Configure(rt);
        }
        // The live engine volume listener is recorded (LSTN); the per second LISTENER
        // line (defaults to MixProbe).
        replay::gamelistener::Configure(
            replay::identity::DevIniInt("GameListenerLog", replay::mixprobe::Enabled() ? 1 : 0, configPath.c_str()) != 0);
        {
            // LSTN v2 and the recorded microphone put back (game_listener.h).
            replay::gamelistener::ConfigureMic(replay::identity::IniInt("ListenerMicReplay", 1, configPath.c_str()) != 0);
        }
        // The mixer scenes and mix groups recorded (MIXS), and the speech level probe
        // (read only).
        {
            replay::mixscene::Config mix;
            mix.probe = replay::identity::DevIniInt("SpeechLevelProbe", 0, configPath.c_str()) != 0;
            replay::mixscene::Configure(mix);
            replay::speechlevel::SetEnabled(mix.probe);
        }
        {
            // The sound categories' controller values recorded (CATS) and put back while
            // the replay owns the world; the sliders, the music and BASE stay live.
            replay::catstate::Config cats;
            cats.replay = replay::identity::IniInt("CategoryStateReplay", 1, configPath.c_str()) != 0;
            cats.keepLive = replay::mixscene::ParseHashList(replay::catstate::kDefaultKeepLiveText);
            cats.log = replay::identity::DevIniInt("CategoryStateLog", 0, configPath.c_str()) != 0;
            cats.watch = replay::mixscene::ParseHashList(replay::catstate::kDefaultWatchText);
            replay::catstate::Configure(cats);
            // The engine's speech duck fed with the recorded inputs.
            replay::speechduck::Config duck;
            duck.replay = replay::identity::IniInt("SpeechDuckReplay", 1, configPath.c_str()) != 0;
            replay::speechduck::Configure(duck);
            LogFile() << "CATEGORY_STATE,phase=config,replay=" << (cats.replay ? 1 : 0)
                      << ",fields=volume,keep_live=" << cats.keepLive.size()
                      << ",log=" << (cats.log ? 1 : 0) << ",watch=" << cats.watch.size() << ",speech_duck=" << (duck.replay ? 1 : 0)
                      << ",derived_flags=" << static_cast<int>(duck.derivedFlags)
                      << '\n';
        }
        {
            // The volume probe, a measurement build (read only): every voice's gain by
            // stage and class, recording against replay.
            replay::volumeprobe::Config vp;
            vp.enabled = replay::identity::DevIniInt("VolumeProbe", 0, configPath.c_str()) != 0;
            replay::volumeprobe::Configure(vp);
            // The world sound probe (read only): the engine's particle audio slots and
            // horse group audio, recording against replay.
            replay::worldsoundprobe::Config ws;
            ws.enabled = vp.enabled; // with the volume probe
            replay::worldsoundprobe::Configure(ws);
        }
        {
            // The weather audio's "is the world loaded about the player" gate asks about
            // the replay's scene while the replay owns the world (WindReplay), and the
            // recorded wind strength is logged against the replay's (section WNDA).
            replay::windreplay::Config wind;
            wind.fix = replay::identity::IniInt("WindReplay", 1, configPath.c_str()) != 0;
            replay::windreplay::Configure(wind);
            LogFile() << "WIND_REPLAY,phase=config,fix=" << (wind.fix ? 1 : 0) << '\n';
        }
        // The weapons' own bones. Our own logic, no ini key (keys only for engine
        // detours and calls).
        replay::objects::SetWeaponBonePose(true); // every weapon in a hand, the free capture slots
        // A detour, so a key.
        replay::objects::SetWeaponPassPose(replay::identity::IniInt("WeaponPassPose", 1, configPath.c_str()) != 0);
        // The weapon look is our logic and natives (no key); the rust and sixth wear
        // channel are plain writes on the replay's own weapon (rung 3), so a key.
        replay::objects::SetWeaponWearWrite(replay::identity::IniInt("WeaponWearWrite", 1, configPath.c_str()) != 0);
        // The reins hold mode detour (reins_guard.h), so a key.
        replay::reins::SetHoldGuard(replay::identity::IniInt("ReinsHoldGuard", 1, configPath.c_str()) != 0);
        replay::objects::SetAmmoScanEveryTick(true); // (C)
        // The world model.
        {
            char text[32]{};
            // The closed theater: the replay always takes the world over; WorldModel=off
            // turns the model off.
            replay::identity::IniString("WorldModel", "gta", text, sizeof(text), configPath.c_str());
            g_recConfig.worldModel = replay::worldmodel::ModeFromIni(text);
            // Closed theater: each engine step has its own key (1 on, 0 off).
            replay::theatre::Config& theatre = g_recConfig.theatre;
            const auto key = [&](const char* name, bool fallback) {
                return replay::identity::IniInt(name, fallback ? 1 : 0, configPath.c_str()) != 0;
            };
            // A developer key (the public build takes the default).
            const auto devKey = [&](const char* name, bool fallback) {
                return replay::identity::DevIniInt(name, fallback ? 1 : 0, configPath.c_str()) != 0;
            };
            theatre.perschar = key("TheatrePerschar", theatre.perschar);
            theatre.vehicles = key("TheatreVehicles", theatre.vehicles);
            theatre.peds = key("TheatrePeds", theatre.peds);
            theatre.pauseScripts = key("TheatrePauseScripts", theatre.pauseScripts);
            theatre.frontendPrompts = devKey("FrontendPrompts", theatre.frontendPrompts);
            theatre.frontendScripts = devKey("TheatreFrontendScripts", theatre.frontendScripts);
            theatre.denySpawn = key("TheatreDenySpawn", theatre.denySpawn);
            theatre.emitters = key("TheatreEmitters", theatre.emitters);
            theatre.hooks = static_cast<std::uint32_t>(replay::identity::IniInt("TheatreHooks", static_cast<INT>(theatre.hooks), configPath.c_str()));
            // LOD 0 through the detour of 0x2BD410C (anim_lod_hook.h): the recorded
            // peds while recording and the puppets in the replay; one support switch
            // for both ends of the same detour.
            const bool animLod0 = key("RecordPedAnimLod0", true);
            // Puppet LOD (puppet_lod.h).
            replay::lod::Config lod;
            lod.scriptPose = key("PuppetScriptPose", lod.scriptPose);
            lod.animLod0 = animLod0;
            // The pose codec's deadband trembling (pose_deadband_smooth.h): the
            // recorder's codec (PoseDetail; PoseCodecFine=0 the old coarse
            // one), and the coarse tracks smoothed at load.
            g_recConfig.poseCodecCoarse = !devKey("PoseCodecFine", true);
            replay::pose::smooth::SetDetail(
                g_recConfig.poseCodecCoarse ? replay::pose::smooth::Detail::Coarse : g_recConfig.poseDetail);
            replay::pose::smooth::SetLoadSmoothing(devKey("PoseDeadbandSmooth", true));
            replay::playback::SetPuppetLod(lod);
            // The horses' muscles (shader_vars_replay.h): recorded (SHVR) and written
            // into the puppets' shader vars component before the shader reads them.
            // MuscleReplay is a support switch (the detour of 0xB2F0D0).
            replay::muscles::Config muscles;
            muscles.replay = key("MuscleReplay", muscles.replay);
            replay::muscles::SetConfig(muscles);
            // The puppets' "mount" sound chains through the replay's mount (entity_query.h).
            replay::entityquery::SetRelation(key("EntityQueryRelation", true));
            // ... and the engine's answers recorded with the clip (EQRY), given back to the puppets.
            replay::entityquery::SetRecordAnswers(key("EntityQueryRecord", true));
            replay::entityquery::SetReplayAnswers(key("EntityQueryReplay", true));
            // The puppets' PED_VARIABLES speed (a puppet reads 0 where the live ped reads
            // its speed): recorded (PSPD) and written back.
            replay::events::SetPedSpeed(key("PedSpeedReplay", true));
            // The secondary steps (the hoof that lifts or lands) recorded as events (FSEV)
            // and queued again on the puppets, and the movement component's own speed on
            // the puppets (the gait and the step volume read it).
            replay::events::SetFootEvents(key("FootEventReplay", true));
            replay::events::SetMotionSpeed(key("MotionSpeedReplay", true));
            // The holder's environment group on weapon sounds.
            replay::events::SetShotEnvGroup(key("ShotEnvGroupReplay", true));
            // The movement volume result per step recorded (FMOV) and given back.
            replay::events::SetFootMovement(key("FootMovementReplay", true));
            // The moon's angles (section MOON, moon_track.h).
            g_moonReplay = key("MoonReplay", true);
            {
                // replay_capture.h FullAnimConfig.
                replay::recorder::FullAnimConfig fullAnim;
                // The engine's "animate as if visible" byte (anim_visible.h) on every recorded
                // ped and animal, so hat fit, face and springs are computed off screen.
                fullAnim.animVisible = key("RecordPedAnimVisible", true);
                // ... and LOD 0 (faces far from the camera too).
                fullAnim.animLod0 = animLod0;
                replay::recorder::SetFullAnim(fullAnim);
            }
            replay::doors::Config doors;
            doors.mapWaitMs = std::min<std::uint32_t>(8000u, static_cast<std::uint32_t>(replay::identity::DevIniInt("MapObjectsWaitMs", static_cast<INT>(doors.mapWaitMs), configPath.c_str())));
            replay::doors::Configure(doors);
            LogFile() << "CONFIG_CAMERA2,prefetch_m=" << replay::camera::kPrefetchMinMeters
                      << ",prefetch_lead_ms=" << replay::camera::kPrefetchLeadMs << '\n';
            // The free camera's options (the editor saves them back) and the world overlay.
            LoadFreeCamOptions(configPath.c_str());
            g_camOverlayEnabled = key("CameraOverlay", true);
            g_camOverlayWhilePlaying = key("CameraOverlayWhilePlaying", false);
            g_camOverlayTimeLabels = key("CameraOverlayTimeLabels", true);
            g_camOverlayTrail = key("CameraOverlayTrail", true);
            LogFreeCamConfig("config");
            LoadSplineDefaults(configPath.c_str());
            LogSplineDefaults("config");
            LogFile() << "CAMERA_RELEASE,phase=config,mode=" << replay::camcut::ReleaseName(kCamRelease) << '\n';
            LogFile() << "CONFIG_LOD_DOORS_CAMERA,puppet_lod_multiplier=" << replay::lod::kLodMultiplier
                      << ",puppet_script_pose=" << (lod.scriptPose ? 1 : 0)
                      << ",door_rebind_ms=" << doors.rebindMs
                      << ",map_objects_wait_ms=" << doors.mapWaitMs << '\n';
        }
        const bool wholePedPool = g_recConfig.worldModel == replay::worldmodel::Mode::Gta;
        // The ped class capacity: 56, or 148 with WorldModel=gta (the pool, 150).
        const int requestedPedCapacity = static_cast<int>(replay::capacity::DefaultPedCapacity(wholePedPool));
        replay::capacity::SetPedCapacity(replay::capacity::ClampPedCapacity(requestedPedCapacity, wholePedPool));
        // The vehicle class: 8, or the vehicle pool less 1 with WorldModel=gta.
        replay::capacity::SetVehicleCapacity(replay::capacity::VehicleCapacityFor(wholePedPool));
        // The object burn level track.
        replay::objects::SetObjectBurnLevel(
            replay::identity::IniInt("ObjectBurnLevel", 1, configPath.c_str()) != 0);
        // The drawn check of posed objects (the still rule is always on).
        replay::objects::SetObjectDrawnCheck(replay::identity::DevIniInt("ObjectDrawnCheck", 0, configPath.c_str()) != 0);
        // The object custom texture and tint, and the stand-in to prop handoff overlap.
        replay::objlook::SetRecordAndPlay(replay::identity::IniInt("ObjectLook", 1, configPath.c_str()) != 0);
        // The puppets' own persistent ped audio sounds.
        replay::audio::SetRefusePuppetOwnPersistent(
            replay::identity::IniInt("RefusePuppetOwnPersistent", 1, configPath.c_str()) != 0);
        // SoundCheck, SoundCensus and SoundCapture are read by the census itself
        // (sound_census.h); the report needs the clip's path.
        replay::census::SetClipPathSource(&CurrentClipPathForCensus);
        replay::prof::SetSlowFrameUs(1000.0 *
            std::clamp(static_cast<int>(replay::identity::DevIniInt("ProfileSlowTickMs", 50, configPath.c_str())), 5, 1000));
        replay::hud::SetObjectivesEnabled(g_debugMode);

        std::ofstream& log = LogFile();
        log << "\n=== Redemption Editor loaded ===\n";
        log << "debugMode=" << (g_debugMode ? 1 : 0) << "\n";
        log << keysLog;
        log << "RECORDING_CONFIG,buffer=" << (g_recConfig.buffer ? 1 : 0) << ",buffer_s=" << g_recConfig.bufferSeconds
            << ",buffer_mb=" << g_recConfig.bufferMB << ",manual_max_s=" << g_recConfig.manualMaxSeconds
            << ",manual_max_mb=" << g_recConfig.manualMaxMB
            << ",clip_world_wait=" << (g_clipStartConfig.worldWait ? 1 : 0)
            << ",clip_world_wait_ms=" << g_clipStartConfig.worldCapMs
            << ",compress_clips=" << (g_recConfig.compressClips ? 1 : 0)
            << ",compress_after_days=" << g_recConfig.compressAfterDays << ",compress_level=" << g_recConfig.compressLevel
            << ",compress_format=" << (g_recConfig.compressFormat == 1 ? "zstd" : "packed")
            << ",pose_detail=" << (g_recConfig.poseCodecCoarse ? "coarse_dev" : replay::pose::smooth::DetailText(g_recConfig.poseDetail))
            << ",pose_smooth_load=" << (replay::pose::smooth::LoadSmoothing() ? 1 : 0)
            << ",record_cutscenes=" << (g_recConfig.recordCutscenes ? 1 : 0)
            << ",lock_cutscene_camera=" << (g_recConfig.lockCutsceneCamera ? 1 : 0)
            << ",restore_world_state=" << (g_recConfig.restoreWorldState ? 1 : 0)
            << ",restore_world_state_in_mission=" << (g_recConfig.restoreWorldStateInMission ? 1 : 0)
            << ",recording_indicator=" << replay::recindicator::IniValue(g_recConfig.recordingIndicator) << "\n";
        // The capture capacity of this session.
        log << "POSE_V2_CAPACITY,requested=" << requestedPedCapacity << ",ped_capacity=" << replay::capacity::PedCapacity()
            << ",vehicle_capacity=" << replay::capacity::VehicleCapacity()
            << ",live_targets=" << replay::capacity::LiveTargets(replay::capacity::PedCapacity(), replay::capacity::VehicleCapacity())
            << ",slots=" << replay::capacity::kLiveTargetSlots
            << ",override_slots=" << replay::pose::capture::kMaxOverrides
            << ",weapon_capacity=" << replay::capacity::WeaponCapacity(replay::capacity::PedCapacity())
            << ",pool_ped_budget=" << replay::capacity::PoolPedBudget(replay::capacity::PedCapacity())
            << ",pool_vehicle_budget=" << replay::capacity::PoolVehicleBudget(replay::capacity::VehicleCapacity()) << "\n";
        // The world model of this session.
        {
            char flags[16]{};
            std::snprintf(flags, sizeof(flags), "0x%X", replay::worldmodel::kDefaultClearFlags);
            log << "WORLD_MODEL,phase=config,mode=" << (wholePedPool ? "gta" : "off")
                << ",reload_path=slot,step=" << replay::worldmodel::kStepReload << ",clear_flags=" << flags
                << ",ped_capacity_limit=" << replay::capacity::PedCapacityLimit(wholePedPool) << "\n";
            const replay::theatre::Config& theatre = g_recConfig.theatre;
            log << "THEATRE_CONFIG,perschar=" << theatre.perschar << ",vehicles=" << theatre.vehicles
                << ",peds=" << theatre.peds << ",pause_scripts=" << theatre.pauseScripts
                << ",deny_spawn=" << theatre.denySpawn
                << ",emitters=" << theatre.emitters << ",hooks=" << theatre.hooks << "\n";
        }
        log << "buildVersion=" << replay::build::kVersion << "\n";
        // Public (the player's keys only) or dev (REDEMPTION_DEV: the research probes
        // and every ini key).
        log << "CONFIG_BUILD," << replay::identity::kBuildFlavor
            << ",report_include_clips=" << (g_reportIncludeClips ? 1 : 0) << "\n";
        {
            // What was loaded and with which settings.
            wchar_t modulePath[MAX_PATH]{};
            GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), modulePath, MAX_PATH);
            g_asiPath = modulePath;
            std::uint64_t asiBytes = 0;
            g_asiSha256 = FileSha256(g_asiPath, asiBytes);
            log << "ASI_IDENTITY,version=" << replay::build::kVersion << ",sha256=" << g_asiSha256 << ",bytes=" << asiBytes
                << ",path=" << Narrow(g_asiPath) << "\n";
            // Where a crash's minidump goes, and the ids of the named threads
            // (thread_names.h; the workers made later are logged when the map changes,
            // THREADS lines).
            log << "CRASH_DUMPS,folder=" << CrashDumpFolderUtf8() << ",dbghelp=" << (crashdump::g_writeDump ? 1 : 0)
                << ",dump_thread=" << crashdump::g_threadId << ",keep=" << crashdump::kKeepDumps << "\n";
            log << "THREADS,module=RedemptionEditor.asi," << replay::threads::MapText() << "\n";
            bool iniFound = false;
            const std::string iniKeys = IniSectionText(configPath, iniFound);
            g_iniKeysText = iniKeys;
            log << "CONFIG_INI,path=" << configPath << ",found=" << (iniFound ? 1 : 0) << ",debug=" << (g_debugMode ? 1 : 0)
                << ",replay_buffer=" << (g_recConfig.buffer ? 1 : 0)
                << ",pose_fidelity=" << (replay::identity::DevIniInt("PoseFidelity", 0, configPath.c_str()) != 0 ? 1 : 0)
                << ",keys=" << (iniKeys.empty() ? "none" : iniKeys) << "\n";
        }
        log << "gameVersion=" << static_cast<int>(getGameVersion()) << "\n";
        // ASI_GAME (the game build against the one the mod's offline addresses come
        // from) and the mark key.
        replay::qa::kit::Configure(configPath, log, static_cast<int>(getGameVersion()));
        // The exe and how every game signature was found (SIG_SUMMARY).
        replay::sig::Report(log);
        log << "workingDirectory=" << workingDirectory << "\n";
        // Where clips, projects, reports, the log and the changed settings go.
        log << replay::storage::LogLine() << "\n";
        if (!replay::storage::Current().userIni.empty())
            log << "STORAGE_SETTINGS,game_ini=" << configPath << ",user_ini=" << replay::storage::Utf8(replay::storage::Current().userIni)
                << ",user_ini_found=" << (replay::storage::Exists(replay::storage::Current().userIni) ? 1 : 0) << "\n";
        log.flush();
    }
namespace
{
    // The sound report goes next to the open clip (sound_census.h).
    std::wstring CurrentClipPathForCensus() { return g_currentClipPath; }
}
    std::unique_ptr<QaReportJob> g_qaReport;
    ULONGLONG g_qaReportWaitSince = 0; // Shift+F1 waits for an open mark
    bool g_qaReportWaitSaid = false;
namespace
{

    bool CopySharedFile(const std::wstring& from, const std::wstring& to, std::uint64_t& bytes, std::string& error)
    {
        bytes = 0;
        HANDLE source = CreateFileW(from.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (source == INVALID_HANDLE_VALUE)
        {
            error = "open_" + std::to_string(GetLastError());
            return false;
        }
        HANDLE target = CreateFileW(to.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (target == INVALID_HANDLE_VALUE)
        {
            error = "create_" + std::to_string(GetLastError());
            CloseHandle(source);
            return false;
        }
        std::vector<char> buffer(1 << 20);
        bool ok = true;
        DWORD read = 0;
        while (ReadFile(source, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0)
        {
            DWORD wrote = 0;
            if (!WriteFile(target, buffer.data(), read, &wrote, nullptr) || wrote != read)
            {
                error = "write_" + std::to_string(GetLastError());
                ok = false;
                break;
            }
            bytes += read;
            if (InterlockedCompareExchange(&g_moduleDetaching, 0, 0) != 0)
            {
                error = "unload";
                ok = false;
                break;
            }
        }
        CloseHandle(target);
        CloseHandle(source);
        return ok;
    }
}

    void StartQaReport()
    {
        if (g_qaReport)
        {
            replay::hud::ShowNotice(TR("hud.report_busy", "Redemption Editor: a report is still being saved."));
            return;
        }
        SYSTEMTIME now{};
        GetLocalTime(&now);
        const std::string folderName = replay::qareport::FolderName(now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        auto job = std::make_unique<QaReportJob>();
        job->startedAt = GetTickCount64();
        // In the base folder (storage_paths.h).
        job->folder = replay::storage::Path(replay::storage::kReports) + L"\\" + std::wstring(folderName.begin(), folderName.end());
        replay::qareport::Summary summary;
        summary.version = replay::build::kVersion;
        summary.asiSha256 = g_asiSha256;
        char time[32]{};
        std::snprintf(time, sizeof(time), "%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
            now.wSecond);
        summary.time = time;
        summary.lastClip = g_lastWrittenClipName;
        summary.openClip = g_currentClipPath.empty() ? std::string() : ClipFileName(g_currentClipPath);
        summary.state = g_recording ? (g_recordKind == RecordKind::Buffer ? (g_manualInBuffer ? "recording (buffer, manual mark)" : "recording (buffer)")
                                                                            : "recording (manual)")
                                    : g_transport.Playing() ? "playing" : "idle";
        summary.iniKeys = g_iniKeysText;
        {
            // The marks of this session go in (the folders of older sessions too: the
            // report thread moves every one it finds and rewrites the "marks:" line from
            // them, so a report saved after a crash lists them).
            const std::vector<std::string> marks = replay::qa::kit::PendingMarkFolders();
            for (const std::string& mark : marks)
                summary.marks += (summary.marks.empty() ? "" : ";") + mark;
            if (!marks.empty())
                summary.marks = std::to_string(marks.size()) + " (" + summary.marks + ")";
            replay::qa::bundle::Input& input = job->bundleInput;
            input.reportFolder = job->folder;
            input.reportName = folderName;
            // The clips the player marked; the open clip only with ReportIncludeClips=1.
            summary.includeOpenClip = g_reportIncludeClips;
            input.clips = replay::qareport::ReportClips(replay::qa::kit::MarkedClips(), summary.openClip, g_reportIncludeClips);
            input.zipClip = !input.clips.empty() ? input.clips.front() : summary.lastClip;
            input.crashFolder = crashdump::g_folder;
            input.dataRoot = replay::storage::Current().base;
            input.readRoots = replay::storage::Current().roots;
            input.asiVersion = replay::build::kVersion;
            input.asiSha256 = g_asiSha256;
            input.asiPath = g_asiPath;
            input.scriptHookGameVersion = static_cast<int>(getGameVersion());
            input.cancel = &replay::qa::kit::CancelFlag();
            replay::qa::kit::MarksReported();
        }
        job->summary = replay::qareport::SummaryText(summary);
        job->bundleInput.reportText = job->summary; // the marks line is rewritten from the folders moved
        // The start line goes into the copied log.
        LogFile() << "REPORT_SAVE,phase=start,dir=" << Narrow(job->folder) << ",last_clip=" << (summary.lastClip.empty() ? "none" : summary.lastClip)
                  << ",state=" << summary.state << ",clips=" << job->bundleInput.clips.size()
                  << ",clips_policy=" << (g_reportIncludeClips ? "marked_and_open" : "marked") << '\n';
        LogFile().flush();
        QaReportJob* raw = job.get();
        raw->worker = std::thread([raw]() {
            replay::threads::Scope threadName("report_copy");
            // An exception leaving a std::thread is std::terminate.
            try
            {
                replay::storage::Folder(replay::storage::kReports);
                if (!CreateDirectoryW(raw->folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                    raw->error = "mkdir_" + std::to_string(GetLastError());
                const replay::storage::Layout& storage = replay::storage::Current();
                std::string listing = "storage: " + replay::storage::Utf8(storage.base) + " (game_folder_writable=" +
                    (storage.gameWritable ? "1" : "0") + ", " + storage.reason + ")\r\n";
                // The game folder's settings, and the player's own file over them when the
                // game folder takes no writes (as RedemptionEditor.user.ini).
                const std::wstring userIni = storage.userIni;
                std::vector<const char*> files(std::begin(replay::qareport::kFiles), std::end(replay::qareport::kFiles));
                if (!userIni.empty())
                    files.push_back("RedemptionEditor.user.ini");
                for (const char* file : files)
                {
                    if (!raw->error.empty())
                        break;
                    const std::wstring name(file, file + std::strlen(file));
                    // The log and the crash log in the base folder; the ini, ScriptHook's and
                    // ReShade's logs in the game folder.
                    const bool userCopy = !userIni.empty() && name == L"RedemptionEditor.user.ini";
                    const std::wstring source = userCopy ? userIni
                        : name == replay::identity::kIniFileW ? storage.game + name
                                                              : replay::storage::Find(name);
                    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES)
                    {
                        listing += std::string(file) + ": missing\r\n";
                        continue;
                    }
                    std::uint64_t bytes = 0;
                    std::string error;
                    const bool ok = CopySharedFile(source, raw->folder + L"\\" + name, bytes, error);
                    char size[32]{};
                    std::snprintf(size, sizeof(size), "%.1fMB", static_cast<double>(bytes) / (1024.0 * 1024.0));
                    listing += std::string(file) + ": " + (ok ? size : "failed " + error) + "\r\n";
                    raw->copied += (raw->copied.empty() ? "" : ";") + std::string(file) + ":" + (ok ? size : "failed_" + error);
                    raw->bytes += bytes;
                    raw->files += ok ? 1 : 0;
                    if (!ok && error == "unload")
                        raw->error = error;
                }
                if (raw->error.empty())
                {
                    const std::string text = raw->summary + "\r\nfiles:\r\n" + listing;
                    raw->bundleInput.reportText = text; // the bundle rewrites its marks line from the folders it moves
                    HANDLE file = CreateFileW((raw->folder + L"\\report.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
                    DWORD wrote = 0;
                    if (file == INVALID_HANDLE_VALUE || !WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr))
                        raw->error = "report_txt_" + std::to_string(GetLastError());
                    if (file != INVALID_HANDLE_VALUE)
                        CloseHandle(file);
                }
                // Marks, clips, dumps, sysinfo and the zip.
                if (raw->error.empty())
                    replay::qa::bundle::Extend(raw->bundleInput, raw->bundle);
            }
            catch (const std::exception& exception)
            {
                raw->error = std::string("exception_") + exception.what();
            }
            catch (...)
            {
                raw->error = "unknown_exception";
            }
            raw->done.store(true);
        });
        g_qaReport = std::move(job);
    }

    // Each tick (and with `wait` on unload): the finished report is said.
    void ServiceQaReport(bool wait)
    {
        if (!g_qaReport || (!wait && !g_qaReport->done.load()))
            return;
        g_qaReport->worker.join();
        const std::unique_ptr<QaReportJob> job = std::move(g_qaReport);
        const bool ok = job->error.empty();
        char mb[24]{};
        std::snprintf(mb, sizeof(mb), "%.1f", static_cast<double>(job->bytes) / (1024.0 * 1024.0));
        const replay::qa::bundle::Result& bundle = job->bundle;
        LogFile() << "REPORT_SAVE,phase=done,ok=" << (ok ? 1 : 0) << ",dir=" << Narrow(job->folder) << ",files=" << job->files
                  << ",mb=" << mb << ",ms=" << (GetTickCount64() - job->startedAt) << ",copied=" << (job->copied.empty() ? "none" : job->copied)
                  << ",error=" << (ok ? "none" : job->error.c_str()) << ",marks=" << bundle.marks << ",debug_files=" << bundle.debugFiles << ",clips=" << bundle.clips
                  << ",clips_compressed=" << bundle.clipsCompressed << ",dumps=" << bundle.dumps
                  << ",zip=" << (bundle.zipped ? Narrow(bundle.zipPath) : std::string("none")) << ",zip_mb="
                  << replay::qa::MegabytesText(bundle.zipBytes) << ",zip_entries=" << bundle.zipEntries << ",zip_deflated=" << bundle.zipDeflated
                  << ",zlib=" << (bundle.zlib.empty() ? "none" : bundle.zlib) << ",zip_error=" << (bundle.zipError.empty() ? "none" : bundle.zipError)
                  << ",bundle_error=" << (bundle.error.empty() ? "none" : bundle.error) << ",game_exe=" << (bundle.gameExeVersion.empty() ? "?" : bundle.gameExeVersion)
                  << ",game_build_match=" << bundle.gameBuildMatches << '\n';
        if (ok && bundle.zipped)
            replay::hud::ShowNotice(TRF("hud.report_saved", "Report saved: {path} ({mb} MB)",
                {{"path", Narrow(bundle.zipPath)}, {"mb", replay::qa::MegabytesText(bundle.zipBytes)}}).c_str());
        else if (ok)
            replay::hud::ShowNotice(TRF("hud.report_saved_folder", "Report saved: {path} (folder; the zip failed: {error})",
                {{"path", Narrow(job->folder)}, {"error", bundle.zipError.empty() ? std::string("?") : bundle.zipError}}).c_str());
        else
            replay::hud::ShowNotice(TRF("hud.report_failed", "Redemption Editor: the report could not be saved ({error}).", {{"error", job->error}}).c_str());
    }

    // Clip picker: F8 opens the list of ReplayClips (newest first), each F8 moves
    // down, and 1.5 s without a press loads the one selected.
    bool g_clipPickerOpen = false;
    std::vector<std::wstring> g_clipPickerFiles;
    std::size_t g_clipPickerIndex = 0;
    ULONGLONG g_clipPickerLastPress = 0;
