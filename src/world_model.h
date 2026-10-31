#pragma once

// The world model for the replay, as a replay editor usually does it: the
// world is cleared when the replay takes it (peds, vehicles, objects over a
// radius of 1,000,000 around the origin, the player kept) and, when the
// replay is left, the most recent save is loaded; the progress since that
// save is lost.
//
// Off by default (RedemptionEditor.ini [RedemptionEditor] WorldModel=gta
// turns it on). Steps, each one testable alone:
//   (a) capture the whole ped pool (capture_capacity.h);
//   (b) clear the world on the first playback: FORCE_CLEANUP(512) for the
//       scripts (the replay bit the game's startup_sp uses), then
//       CLEAR_AREA(0, 0, 0, 1e6, flags) before any puppet exists;
//       only when a save exists (the engine's own slot table read under SEH)
//       and the reload path passed its signature check;
//   (c) reload the most recent save when the replay is left: the orphan
//       queue operation (kLoadOp, .data) pushed by the savegame queue push
//       (kPush), or plan B, LoadFromSlot (kLoadFromSlot, the pause menu's
//       path) with the newest slot;
//   (d) in a mission nothing is cleared nor reloaded.
// This header is pure (no engine access): the decisions, the layouts, the
// signatures of the engine code (game_sig_core.h) and the checks over bytes
// already read; script.cpp reads and calls.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "game_sig_core.h"
#include "i18n.h" // the refusal notices in the player's language

namespace replay::worldmodel
{
    // ---- ini ---------------------------------------------------------------

    enum class Mode : std::uint8_t
    {
        Off = 0,
        Gta = 1,
    };

    // WorldModel=gta (or 1) turns the model on; anything else is off.
    inline Mode ModeFromIni(const char* text)
    {
        if (!text)
            return Mode::Off;
        if (std::strcmp(text, "1") == 0)
            return Mode::Gta;
        const char want[] = "gta";
        for (std::size_t index = 0; index < 3; ++index)
        {
            char c = text[index];
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
            if (c != want[index])
                return Mode::Off;
        }
        return text[3] == '\0' ? Mode::Gta : Mode::Off;
    }

    // The steps of the model (the replay always runs 3; the ladder is kept
    // for the tests). 1: only (a), the whole pool recorded; 2: (a) and (b), the world
    // cleared and the reload left to the user (pause menu, Load Game);
    // 3 (default): (a), (b) and (c).
    constexpr std::uint32_t kStepPool = 1;
    constexpr std::uint32_t kStepClear = 2;
    constexpr std::uint32_t kStepReload = 3;

    // The save reload: the menu's LoadFromSlot with the newest slot (Slot,
    // what the replay uses) or the orphan load-most-recent
    // operation (Queue, the first plan; kept for the tests).
    enum class ReloadPath : std::uint8_t
    {
        Queue = 0,
        Slot = 1,
    };

    inline const char* ReloadPathName(ReloadPath path)
    {
        return path == ReloadPath::Slot ? "slot" : "queue";
    }

    // ---- (b) clearing ------------------------------------------------------

    // CLEAR_AREA bits decoded in the native's core.
    constexpr std::uint32_t kClearProjectiles = 0x1;   // the projectiles
    constexpr std::uint32_t kClearVehicles = 0x8;      // over the vehicle pool
    constexpr std::uint32_t kClearObjects = 0x200;     // over the object pool
    constexpr std::uint32_t kClearPeds = 0x4000;       // over the ped pool
    constexpr std::uint32_t kClearFires = 0x10000;     // the fire area
    constexpr std::uint32_t kClearBroadcast = 0x80000; // a network event: never
    constexpr std::uint32_t kDefaultClearFlags = kClearProjectiles | kClearVehicles | kClearObjects | kClearPeds | kClearFires;
    // The core tests the 2D distance, so this radius covers the map.
    // Centre (0, 0, 0).
    constexpr float kClearRadius = 1000000.0f;

    // A set of clear bits (the startup_sp set 2442122) without the broadcast
    // bit.
    constexpr std::uint32_t SanitizeClearFlags(std::uint32_t flags)
    {
        return flags & ~kClearBroadcast;
    }

    // FORCE_CLEANUP's replay bit (the startup_sp fires it when the replay
    // asks; 1519 of the 1639 scripts carry it).
    constexpr int kForceCleanupReplay = 512;
    // Up to 5 s for the scripts to clean up.
    constexpr std::uint64_t kScriptCleanupWaitMs = 5000;

    // ---- (b) the save slots ------------------------------------------------

    // The engine's slot headers (17 of 0x134 bytes; "SRDR3" first) and the
    // slot table (17 of 0x48 bytes; the save's date as six ints at +0x2C),
    // read by the newest-slot sort that the load operation uses (kNewestSlot):
    // empty when the first header byte is 0, skipped unless the header starts
    // with SRDR3, dates compared field by field, strictly greater wins (the
    // date compare), so the lowest index wins a tie.
    constexpr std::uint32_t kSlotCount = 17;
    constexpr std::uint32_t kHeaderStride = 0x134;
    constexpr std::uint32_t kSlotStride = 0x48;
    constexpr std::uint32_t kSlotDateOffset = 0x2C;
    constexpr char kHeaderMagic[5] = {'S', 'R', 'D', 'R', '3'};

    struct Slot
    {
        bool used = false;  // header byte 0 not zero
        bool magic = false; // header starts with SRDR3
        std::int32_t date[6] = {}; // year, month, day, hour, minute, second
    };

    inline Slot SlotFromBytes(const std::uint8_t* header, std::size_t headerSize, const std::int32_t (&date)[6])
    {
        Slot slot;
        slot.used = headerSize > 0 && header[0] != 0;
        slot.magic = headerSize >= sizeof(kHeaderMagic) && std::memcmp(header, kHeaderMagic, sizeof(kHeaderMagic)) == 0;
        for (int index = 0; index < 6; ++index)
            slot.date[index] = date[index];
        return slot;
    }

    // The date compare: a newer than b (strictly).
    inline bool Newer(const std::int32_t (&a)[6], const std::int32_t (&b)[6])
    {
        for (int index = 0; index < 6; ++index)
        {
            if (a[index] > b[index])
                return true;
            if (a[index] != b[index])
                return false;
        }
        return false;
    }

    struct SlotSummary
    {
        int newest = -1;         // the slot the load operation would pick (-1: none, it would start a new game)
        std::uint32_t used = 0;  // headers not empty
        std::uint32_t valid = 0; // with SRDR3
        std::int32_t date[6] = {};
    };

    inline SlotSummary Summarize(const Slot* slots, std::uint32_t count)
    {
        SlotSummary summary;
        for (std::uint32_t index = 0; index < count && index < kSlotCount; ++index)
        {
            const Slot& slot = slots[index];
            if (!slot.used)
                continue;
            ++summary.used;
            if (!slot.magic)
                continue;
            ++summary.valid;
            if (summary.newest < 0 || Newer(slot.date, summary.date))
            {
                summary.newest = static_cast<int>(index);
                for (int field = 0; field < 6; ++field)
                    summary.date[field] = slot.date[field];
            }
        }
        return summary;
    }

    // ---- (c) the load operation ----------------------------------------------

    // The orphan "load most recent save" operation: a static object with no
    // caller in the retail code; its Process (kProcess) enumerates, sorts,
    // loads the newest, and without one starts a new game.
    constexpr std::uint32_t kLoadOpType = 0x17;
    constexpr std::size_t kLoadOpSize = 0x20;
    constexpr std::size_t kOpVtable = 0x00;
    constexpr std::size_t kOpStatus = 0x08;  // 0 ready, 1 queued, 2 error (the push writes 1)
    constexpr std::size_t kOpType = 0x0C;
    constexpr std::size_t kOpState = 0x10;   // 0 start, 1 enumerate, 2 choose, 3 load, 4 wait
    constexpr std::size_t kOpSlot = 0x14;    // -1
    constexpr std::size_t kOpSpare = 0x18;   // -1
    constexpr std::size_t kOpLoadArg = 0x1C; // byte 1, passed to BeginLoad
    // The fields the reset writes: +0x10 to +0x1C (the ctor kLoadOpCtor writes
    // +0x14, +0x18 and the byte +0x1C; the state +0x10 is left by a finished
    // run at 4, so a second exit would start waiting: 0 here).
    constexpr std::size_t kOpResetBegin = kOpState;
    constexpr std::size_t kOpResetEnd = kOpLoadArg + 1;

    constexpr std::int32_t kStatusReady = 0;
    constexpr std::int32_t kStatusQueued = 1;
    constexpr std::int32_t kStatusError = 2;

    // The savegame queue: a ring of 24, its count after it (kQueueCount).
    constexpr std::int32_t kQueueCapacity = 0x18;

    // The menu's operation (plan B): LoadFromSlot returns at once when its
    // status at +0x08 (kMenuOpStatus) is 1.

    // ---- the engine code, by signature ---------------------------------------
    //
    // The functions and globals of the reload path (game_sig_core.h). One not
    // found disables the reload for the session, and without a reload the
    // world is never cleared.
    //
    // The push: a null operation is refused, status 2 on error; cmp [the
    // queue count], 0x18 (the capacity).
    inline const sig::Signature kPush{"worldmodel_push",
        "45 33 C0 48 8B D1 48 85 C9 74 55 C7 41 08 02 00 00 00 83 3D ?? ?? ?? ?? 18", 0xBDBF58};
    inline const sig::Signature kQueueCount{"worldmodel_queue_count", kPush, sig::Rel32{0x14, 0x19}, "", 0x49F1E88};
    // The load operation's Process, its switch on the state at +0x10 (the
    // field the reset writes), and the queue's update.
    inline const sig::Signature kProcess{"worldmodel_load_most_recent_process",
        "48 89 5C 24 10 56 48 83 EC 20 48 8B D9 BE 02 00 00 00", 0xBBA2FC};
    inline const sig::Signature kProcessState{"worldmodel_process_state", kProcess, sig::Offset{0x12},
        "8B 49 10 85 C9 0F 84 ?? ?? ?? ?? 83 E9 01 0F 84 ?? ?? ?? ?? 83 E9 01 0F 84 ?? ?? ?? ?? 83 E9 01", 0xBBA30E};
    inline const sig::Signature kQueueUpdate{"worldmodel_savegame_queue_update",
        "40 53 48 83 EC 20 33 DB C6 05 ?? ?? ?? ?? 01 39 1D ?? ?? ?? ??", 0xBE4170};
    // The newest-slot sort, which reads the slot headers (lea rbp, [headers]),
    // and the date getter (slot < 17; lea rax, [table + 0x2C]).
    inline const sig::Signature kNewestSlot{"worldmodel_newest_slot_sort",
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 81 EC C0 01 00 00 41 B8 11 00 00 "
        "00 48 8D 44 24 28 41 8B F8 4C 8B F1 83 60 F8 00 83 60 FC 00 83 20 00 83 60 04 00 83 60 08 00 83 60 0C 00 48 "
        "8D 40 18 48 83 EF 01 75 DF 85 D2 74 0B 8B CA 49 8B FE 48 83 C8 FF F3 AB 41 3B D0 48 8D 2D ?? ?? ?? ??",
        0xBC9A30};
    inline const sig::Signature kSlotHeaders{"worldmodel_slot_headers", kNewestSlot, sig::Rel32{0x66, 0x6A}, "",
        0x49BD388};
    inline const sig::Signature kSlotDateGetter{"worldmodel_slot_date_getter",
        "83 F9 11 73 13 48 63 C1 48 8D 0C C0 48 8D 05 ?? ?? ?? ?? 48 8D 04 C8 C3", 0xBCC264};
    inline const sig::Signature kSlotDates{"worldmodel_slot_dates", kSlotDateGetter, sig::Rel32{0x0F, 0x13}, "",
        0x49F769C};
    // LoadFromSlot returns at once when the menu's operation status is 1:
    // cmp [status], 1.
    inline const sig::Signature kLoadFromSlot{"worldmodel_load_from_slot",
        "40 53 48 83 EC 60 83 3D ?? ?? ?? ?? 01 0F 84 ?? ?? ?? ?? 85 C9",
        0xBDC1B8};
    inline const sig::Signature kMenuOpStatus{"worldmodel_menu_op_status", kLoadFromSlot, sig::Rel32{0x08, 0x0D}, "",
        0x3A23D10};
    // The operation's constructor: its vtable, -1 at +0x14 and +0x18, the type
    // 0x17 at +0x0C and the load byte 1 at +0x1C (the fields the reset writes).
    inline const sig::Signature kLoadOpCtor{"worldmodel_load_op_ctor",
        "83 61 08 00 48 8D 05 ?? ?? ?? ?? 48 89 01 83 C8 FF 89 41 14 89 41 18 48 8B C1 C7 41 0C 17 00 00 00 C6 41 1C "
        "01 C3",
        0xBBF1E0};
    inline const sig::Signature kLoadOpVtable{"worldmodel_load_op_vtable", kLoadOpCtor, sig::Rel32{0x07, 0x0B}, "",
        0x3422360};
    // The operation itself: a static object (constant initialized; no code
    // points at it but its exit destructor), found by its vtable: the one
    // object the exe file holds that starts with kLoadOpVtable and has the
    // type 0x17 at +0x0C (its status at +0x08 may be anything). A second one
    // of the class, which a static constructor builds at start in the
    // zero-filled tail of .data, is not it.
    inline const sig::Signature kLoadOp{"worldmodel_load_op", kLoadOpVtable, sig::Instance{true},
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 17 00 00 00", 0x3A24360};
    struct OpView
    {
        std::uint64_t vtable = 0;
        std::int32_t status = 0;
        std::int32_t type = 0;
        std::int32_t state = 0;
        std::int32_t slot = 0;
        std::int32_t spare = 0;
        std::uint8_t loadArg = 0;
    };

    inline OpView ParseOp(const std::uint8_t (&bytes)[kLoadOpSize])
    {
        OpView view;
        std::memcpy(&view.vtable, bytes + kOpVtable, sizeof(view.vtable));
        std::memcpy(&view.status, bytes + kOpStatus, sizeof(view.status));
        std::memcpy(&view.type, bytes + kOpType, sizeof(view.type));
        std::memcpy(&view.state, bytes + kOpState, sizeof(view.state));
        std::memcpy(&view.slot, bytes + kOpSlot, sizeof(view.slot));
        std::memcpy(&view.spare, bytes + kOpSpare, sizeof(view.spare));
        view.loadArg = bytes[kOpLoadArg];
        return view;
    }

    enum class OpCheck : std::uint8_t
    {
        Ok,
        WrongVtable,
        WrongType,
        Queued, // status 1: already in the queue
    };

    inline const char* OpCheckName(OpCheck check)
    {
        switch (check)
        {
        case OpCheck::Ok: return "ok";
        case OpCheck::WrongVtable: return "wrong_vtable";
        case OpCheck::WrongType: return "wrong_type";
        case OpCheck::Queued: return "queued";
        }
        return "?";
    }

    // `vtable`: where the game has the operation's vtable (kLoadOpVtable).
    inline OpCheck CheckOp(const OpView& view, std::uint64_t vtable)
    {
        if (!vtable || view.vtable != vtable)
            return OpCheck::WrongVtable;
        if (view.type != static_cast<std::int32_t>(kLoadOpType))
            return OpCheck::WrongType;
        if (view.status == kStatusQueued)
            return OpCheck::Queued;
        return OpCheck::Ok;
    }

    // The bytes +0x10 to +0x1C as the ctor leaves them (state 0, both slots
    // -1, the load byte 1). Writes only that range of `op`.
    inline void ResetOpFields(std::uint8_t* op)
    {
        const std::int32_t start = 0;
        const std::int32_t none = -1;
        std::memcpy(op + kOpState, &start, sizeof(start));
        std::memcpy(op + kOpSlot, &none, sizeof(none));
        std::memcpy(op + kOpSpare, &none, sizeof(none));
        op[kOpLoadArg] = 1;
    }

    // ---- decisions -----------------------------------------------------------

    // (b) and (d): clear the world at this playback?
    struct EnterState
    {
        Mode mode = Mode::Off;
        std::uint32_t step = kStepReload;
        bool alreadyCleared = false; // an earlier playback of this replay session cleared it
        bool inMission = false;      // GET_MISSION_FLAG
        bool online = false;         // NETWORK_IS_GAME_IN_PROGRESS
        bool slotsRead = false;      // the slot table read without a fault
        int newestSlot = -1;         // Summarize(...).newest
        bool reloadReady = false;    // the chosen reload path passed its checks
    };

    enum class EnterVerdict : std::uint8_t
    {
        Off,
        StepPoolOnly, // WorldModelStep=1
        AlreadyCleared,
        RefuseMission,
        RefuseOnline,
        RefuseSlotRead,
        RefuseNoSave,
        RefuseReloadPath,
        Clear,
    };

    inline const char* EnterVerdictName(EnterVerdict verdict)
    {
        switch (verdict)
        {
        case EnterVerdict::Off: return "off";
        case EnterVerdict::StepPoolOnly: return "step_pool_only";
        case EnterVerdict::AlreadyCleared: return "already_cleared";
        case EnterVerdict::RefuseMission: return "refuse_mission";
        case EnterVerdict::RefuseOnline: return "refuse_online";
        case EnterVerdict::RefuseSlotRead: return "refuse_slot_read";
        case EnterVerdict::RefuseNoSave: return "refuse_no_save";
        case EnterVerdict::RefuseReloadPath: return "refuse_reload_path";
        case EnterVerdict::Clear: return "clear";
        }
        return "?";
    }

    // The order matters only for the log: every refusal leaves the world
    // hidden, not cleared, and nothing is reloaded.
    inline EnterVerdict DecideEnter(const EnterState& s)
    {
        if (s.mode != Mode::Gta)
            return EnterVerdict::Off;
        if (s.step < kStepClear)
            return EnterVerdict::StepPoolOnly;
        if (s.alreadyCleared)
            return EnterVerdict::AlreadyCleared;
        if (s.inMission)
            return EnterVerdict::RefuseMission; // (d): missions are left alone
        if (s.online)
            return EnterVerdict::RefuseOnline;
        if (!s.slotsRead)
            return EnterVerdict::RefuseSlotRead;
        if (s.newestSlot < 0)
            return EnterVerdict::RefuseNoSave; // the engine would start a new game
        if (!s.reloadReady && s.step >= kStepReload)
            return EnterVerdict::RefuseReloadPath; // step 2 reloads by hand: the path is not needed
        return EnterVerdict::Clear;
    }

    // The notice of a refusal. In a mission it shows at every playback
    // start (the mission's leftovers are in every playback); the other
    // refusals show only on the session's first refusal. Empty: none.
    constexpr const char* kMissionNotice =
        "Redemption Editor: you are in a mission. The replay may show mission leftovers (extra objects, doors, props) and "
        "some things may not look right.";

    inline std::string EnterNotice(EnterVerdict verdict, std::uint32_t refusalsBefore)
    {
        if (verdict == EnterVerdict::RefuseMission) // kMissionNotice in English (the tests compare them)
            return TR("hud.world.mission", "Redemption Editor: you are in a mission. The replay may show mission leftovers (extra objects, doors, props) and "
                "some things may not look right.");
        if (refusalsBefore != 0)
            return {};
        const char* why = verdict == EnterVerdict::RefuseOnline ? TR("hud.world.online", "online")
            : verdict == EnterVerdict::RefuseNoSave             ? TR("hud.world.no_save", "no save found to come back to")
            : verdict == EnterVerdict::RefuseSlotRead || verdict == EnterVerdict::RefuseReloadPath
                ? TR("hud.world.no_reload", "the save reload is not available")
                : nullptr;
        if (!why)
            return {};
        return TRF("hud.world.refused", "Redemption Editor: WorldModel=gta did not clear the world ({why}); the replay runs as before.", {{"why", why}});
    }

    // (c): the replay is left once it neither plays nor shows the editor
    // nor opens a clip, for this long (a clip switch, a project's next clip,
    // the editor's gallery between two clips keep the world cleared).
    constexpr std::uint64_t kExitSettleMs = 1000;
    // After the push: the status is logged until the scripts restart; past
    // this the load is reported as stuck (the user loads by hand).
    constexpr std::uint64_t kReloadWatchMs = 30000;

    struct ExitState
    {
        Mode mode = Mode::Off;
        std::uint32_t step = kStepReload;
        bool cleared = false;      // this session cleared the world
        bool reloadStarted = false; // pushed (or failed): nothing more to decide
        bool playing = false;      // the transport has a clip open
        bool editorOpen = false;   // the editor covers the screen
        bool clipOpening = false;  // a clip or project read, a clip load waiting, a project clip pending
        bool recording = false;
        bool exitFading = false;   // the exit load wait still runs
        std::uint64_t idleMs = 0;  // how long all of the above has been false
    };

    enum class ExitVerdict : std::uint8_t
    {
        None,   // nothing to do (model off, world not cleared, already pushed)
        InReplay,
        Settle, // out of the replay, waiting kExitSettleMs
        Reload,
        Manual, // WorldModelStep=2: the user loads the save from the menu
    };

    inline ExitVerdict DecideExit(const ExitState& s)
    {
        if (s.mode != Mode::Gta || !s.cleared || s.reloadStarted)
            return ExitVerdict::None;
        if (s.playing || s.editorOpen || s.clipOpening || s.recording || s.exitFading)
            return ExitVerdict::InReplay;
        if (s.idleMs < kExitSettleMs)
            return ExitVerdict::Settle;
        return s.step >= kStepReload ? ExitVerdict::Reload : ExitVerdict::Manual;
    }

    // The checks right before the push (or the plan B call).
    struct PushState
    {
        ReloadPath path = ReloadPath::Queue;
        bool signaturesOk = false;   // the path's functions
        bool opRead = false;         // queue path: the operation read without a fault
        OpCheck op = OpCheck::Ok;    // queue path
        bool queueRead = false;
        std::int32_t queueCount = 0; // queue path
        bool menuOpBusy = false;     // slot path: the menu's operation status is 1
        bool savePending = false;    // SAVEGAME_IS_SAVE_PENDING
        bool loadingScreen = false;  // IS_LOADING_SCREEN_VISIBLE
        int newestSlot = -1;         // read again now
    };

    enum class PushVerdict : std::uint8_t
    {
        Push,
        Wait, // a save or a loading screen in the way: try again next tick
        RefuseSignature,
        RefuseOpRead,
        RefuseOp,
        RefuseQueue,
        RefuseMenuBusy,
        RefuseNoSave,
    };

    inline const char* PushVerdictName(PushVerdict verdict)
    {
        switch (verdict)
        {
        case PushVerdict::Push: return "push";
        case PushVerdict::Wait: return "wait";
        case PushVerdict::RefuseSignature: return "refuse_signature";
        case PushVerdict::RefuseOpRead: return "refuse_op_read";
        case PushVerdict::RefuseOp: return "refuse_op";
        case PushVerdict::RefuseQueue: return "refuse_queue";
        case PushVerdict::RefuseMenuBusy: return "refuse_menu_busy";
        case PushVerdict::RefuseNoSave: return "refuse_no_save";
        }
        return "?";
    }

    inline PushVerdict DecidePush(const PushState& s)
    {
        if (!s.signaturesOk)
            return PushVerdict::RefuseSignature;
        if (s.savePending || s.loadingScreen)
            return PushVerdict::Wait;
        if (s.newestSlot < 0)
            return PushVerdict::RefuseNoSave;
        if (s.path == ReloadPath::Queue)
        {
            if (!s.opRead)
                return PushVerdict::RefuseOpRead;
            if (s.op != OpCheck::Ok)
                return PushVerdict::RefuseOp;
            // An empty queue only: an operation ahead of ours (a save) could
            // fail and the order of the two is the engine's.
            if (!s.queueRead || s.queueCount != 0)
                return PushVerdict::RefuseQueue;
            return PushVerdict::Push;
        }
        if (s.menuOpBusy)
            return PushVerdict::RefuseMenuBusy;
        return PushVerdict::Push;
    }

    // While a save or a loading screen holds the push: give up after this.
    constexpr std::uint64_t kPushWaitMs = 15000;
}
