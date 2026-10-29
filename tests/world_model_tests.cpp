// The world model's pure parts (world_model.h, capture_capacity.h): the clear
// and reload decisions from the session's state, the newest save as the
// engine's sort picks it, the load operation's layout and the bytes the reset
// writes, the signatures and field sites against the offline dump's addresses,
// and the ini parsing.
#include "capture_capacity.h"
#include "log_policy.h"
#include "world_model.h"

#include <cstdio>
#include <cstring>

namespace
{
    using namespace replay::worldmodel;

    int g_worldModelFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_worldModelFailures;
    }

    void TestIni()
    {
        Check(ModeFromIni("gta") == Mode::Gta && ModeFromIni("GTA") == Mode::Gta && ModeFromIni("Gta") == Mode::Gta &&
                ModeFromIni("1") == Mode::Gta,
            "ini: WorldModel=gta (any case) or 1 turns the model on");
        Check(ModeFromIni("off") == Mode::Off && ModeFromIni("") == Mode::Off && ModeFromIni(nullptr) == Mode::Off &&
                ModeFromIni("gtav") == Mode::Off && ModeFromIni("0") == Mode::Off && ModeFromIni("gt") == Mode::Off,
            "ini: anything else is off (the default)");
        Check(ReloadPathFromIni("queue") == ReloadPath::Queue && ReloadPathFromIni("") == ReloadPath::Queue &&
                ReloadPathFromIni(nullptr) == ReloadPath::Queue && ReloadPathFromIni("slot") == ReloadPath::Slot &&
                ReloadPathFromIni("Slot") == ReloadPath::Slot,
            "ini: WorldReloadPath queue by default, slot is plan B");
        Check(kDefaultClearFlags == 0x14209, "clear: projectiles, vehicles, objects, peds and fires (research 03)");
        Check(SanitizeClearFlags(0x2442122u | 0x80000u) == 0x2442122u && SanitizeClearFlags(kDefaultClearFlags) == kDefaultClearFlags,
            "clear: the broadcast bit never reaches the native");
        Check(kClearRadius == 1000000.0f && kForceCleanupReplay == 512 && kScriptCleanupWaitMs == 5000,
            "clear: the reference's radius, the replay cleanup bit, 5 s for the scripts");
        Check(replay::log::EssentialLine("WORLD_MODEL,phase=clear,verdict=clear"), "log: WORLD_MODEL lines kept with Debug=0");
    }

    void TestCapacityWithModel()
    {
        using namespace replay::capacity;
        const bool on = ModeFromIni("gta") == Mode::Gta;
        Check(DefaultPedCapacity(on) == 148 && ClampPedCapacity(150, on) == 150 && ClampPedCapacity(200, on) == 150,
            "capacity (a): WorldModel=gta records up to the pool");
        Check(DefaultPedCapacity(false) == 56 && ClampPedCapacity(150, false) == 120,
            "capacity (a): off keeps 56 by default and 120 at most");
    }

    Slot MakeSlot(bool used, const char* magic, int y, int mo, int d, int h, int mi, int s)
    {
        std::uint8_t header[5]{};
        if (used)
            std::memcpy(header, magic, 5);
        const std::int32_t date[6] = {y, mo, d, h, mi, s};
        return SlotFromBytes(header, sizeof(header), date);
    }

    void TestSlots()
    {
        Slot slots[kSlotCount];
        SlotSummary none = Summarize(slots, kSlotCount);
        Check(none.newest == -1 && none.used == 0 && none.valid == 0, "slots: an empty table has no save (never enumerated reads the same)");

        slots[2] = MakeSlot(true, "SRDR3", 2026, 10, 6, 20, 0, 0);
        slots[5] = MakeSlot(true, "SRDR3", 2026, 10, 7, 9, 15, 0);
        slots[7] = MakeSlot(true, "SRDR3", 2026, 10, 7, 9, 14, 59);
        slots[9] = MakeSlot(true, "XXXXX", 2027, 1, 1, 0, 0, 0); // not a story save: skipped
        SlotSummary summary = Summarize(slots, kSlotCount);
        Check(summary.newest == 5 && summary.used == 4 && summary.valid == 3, "slots: the newest SRDR3 save wins, others skipped");
        Check(summary.date[0] == 2026 && summary.date[2] == 7 && summary.date[3] == 9 && summary.date[4] == 15, "slots: its date is kept");

        slots[11] = MakeSlot(true, "SRDR3", 2026, 10, 7, 9, 15, 0); // a tie: the lower index stays (0xBC1500 is strictly greater)
        Check(Summarize(slots, kSlotCount).newest == 5, "slots: a tie keeps the lower index, as the engine's sort");
        slots[16] = MakeSlot(true, "SRDR3", 2026, 10, 7, 9, 15, 1);
        Check(Summarize(slots, kSlotCount).newest == 16, "slots: one second newer wins, last slot included");
        Check(Summarize(slots, 10).newest == 5, "slots: only the slots given are read");

        const std::int32_t a[6] = {2026, 1, 1, 0, 0, 0};
        const std::int32_t b[6] = {2025, 12, 31, 23, 59, 59};
        Check(Newer(a, b) && !Newer(b, a) && !Newer(a, a), "slots: date compare field by field, year first");

        std::uint8_t header[5] = {'S', 'R', 'D', 'R', '2'};
        const std::int32_t date[6] = {};
        const Slot other = SlotFromBytes(header, 5, date);
        Check(other.used && !other.magic, "slots: a header that is not SRDR3 is used but not valid");
        Check(kSlotCount == 17 && kHeaderStride == 0x134 && kSlotStride == 0x48 && kSlotDateOffset == 0x2C,
            "slots: 17 headers of 0x134 bytes, 17 entries of 0x48 with the date at +0x2C");
    }

    void TestOpLayout()
    {
        // The object as the exe on disk has it, with the module at the
        // dump's base.
        const std::uint64_t base = 0x7FF71E410000ull;
        std::uint8_t op[kLoadOpSize] = {0x60, 0x23, 0x83, 0x21, 0xF7, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x17, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00};
        OpView view = ParseOp(op);
        Check(view.vtable == base + kLoadOpVtableRva && view.type == 0x17 && view.status == 0 && view.state == 0 &&
                view.slot == -1 && view.spare == -1 && view.loadArg == 1,
            "op: the static object parses as the dump shows it");
        Check(CheckOp(view, base) == OpCheck::Ok, "op: vtable and type match");
        Check(CheckOp(view, base + 0x1000) == OpCheck::WrongVtable, "op: another vtable is refused");

        // A finished run: state 4, slots set, status 0. The reset writes
        // +0x10..+0x1C only.
        std::uint8_t used[kLoadOpSize];
        std::memcpy(used, op, sizeof(used));
        const std::int32_t four = 4, three = 3, minus = -1;
        std::memcpy(used + kOpState, &four, 4);
        std::memcpy(used + kOpSlot, &three, 4);
        std::memcpy(used + kOpSpare, &minus, 4);
        used[kOpLoadArg] = 0;
        used[kOpLoadArg + 1] = 0xAB; // past the range: untouched
        std::uint8_t before[kLoadOpSize];
        std::memcpy(before, used, sizeof(before));
        ResetOpFields(used);
        const OpView reset = ParseOp(used);
        Check(reset.state == 0 && reset.slot == -1 && reset.spare == -1 && reset.loadArg == 1, "op: reset as the ctor leaves it, state 0");
        Check(std::memcmp(used, before, kOpResetBegin) == 0 && std::memcmp(used + kOpResetEnd, before + kOpResetEnd, kLoadOpSize - kOpResetEnd) == 0,
            "op: nothing outside +0x10..+0x1C is written (vtable, status, type, the bytes past +0x1C)");
        Check(kOpResetBegin == 0x10 && kOpResetEnd == 0x1D, "op: the written range is +0x10 to +0x1C");
        Check(std::memcmp(used + kOpResetBegin, op + kOpResetBegin, kOpResetEnd - kOpResetBegin) == 0,
            "op: the reset gives back the bytes the exe ships");

        std::int32_t queued = kStatusQueued;
        std::memcpy(op + kOpStatus, &queued, 4);
        Check(CheckOp(ParseOp(op), base) == OpCheck::Queued, "op: status 1 is already in the queue");
        std::int32_t otherType = 0x18;
        std::memcpy(op + kOpType, &otherType, 4);
        Check(CheckOp(ParseOp(op), base) == OpCheck::WrongType, "op: type 0x18 (the mission replay snapshot) is refused");
    }

    void TestSites()
    {
        Check(SiteTarget(kHeaderSite) == kHeaderRva && SiteTarget(kHeaderSite) == kHeaderSite.target, "site: the newest-slot sort reads the headers at 0x49BD388");
        Check(SiteTarget(kSlotDateSite) == 0x49F769C && SiteTarget(kSlotDateSite) == kSlotDateSite.target, "site: the date getter reads 0x49F7670+0x2C");
        Check(SiteTarget(kQueueCountSite) == kQueueCountRva, "site: the push compares the queue count at 0x49F1E88 with 24");
        Check(kQueueCountSite.bytes[6] == kQueueCapacity, "site: the push's imm8 is the queue capacity (24)");
        Check(SiteTarget(kOpVtableSite) == kLoadOpVtableRva, "site: the ctor stores the vtable 0x3422360");
        Check(SiteTarget(kMenuBusySite) == kMenuLoadOpStatusRva && kMenuBusySite.bytes[6] == kStatusQueued,
            "site: LoadFromSlot compares the menu operation's status with 1");
        Check(kMenuBusySite.rva == kLoadFromSlotSignature.rva + 6 &&
                std::memcmp(kMenuBusySite.bytes, kLoadFromSlotSignature.bytes + 6, 7) == 0,
            "site: the menu site is inside LoadFromSlot's checked prologue");
        Check(kQueueCountSite.rva == kPushSignature.rva + 0x12, "site: the queue site follows the push's prologue");
        std::uint8_t code[16];
        std::memcpy(code, kPushSignature.bytes, 16);
        Check(SignatureMatches(kPushSignature, code, sizeof(code)), "signature: the same bytes match");
        code[3] ^= 0xFF;
        Check(!SignatureMatches(kPushSignature, code, sizeof(code)) && !SignatureMatches(kPushSignature, code, 8) &&
                !SignatureMatches(kPushSignature, nullptr, 16),
            "signature: a changed byte, a short read or no bytes is refused");
    }

    void TestEnter()
    {
        EnterState s;
        Check(DecideEnter(s) == EnterVerdict::Off, "enter: model off, nothing");
        s.mode = Mode::Gta;
        s.slotsRead = true;
        s.newestSlot = 3;
        s.reloadReady = true;
        Check(DecideEnter(s) == EnterVerdict::Clear, "enter: a save and the reload path ready: clear");
        EnterState mission = s;
        mission.inMission = true;
        Check(DecideEnter(mission) == EnterVerdict::RefuseMission, "enter (d): in a mission, nothing cleared");
        // The mission notice at every playback start, the others once.
        Check(EnterNotice(EnterVerdict::RefuseMission, 0) == kMissionNotice &&
                EnterNotice(EnterVerdict::RefuseMission, 5) == kMissionNotice,
            "notice: in a mission, every playback start");
        Check(EnterNotice(EnterVerdict::RefuseMission, 0).find("you are in a mission") != std::string::npos,
            "notice: plain words for players");
        Check(EnterNotice(EnterVerdict::RefuseOnline, 0).find("(online)") != std::string::npos &&
                EnterNotice(EnterVerdict::RefuseOnline, 1).empty(),
            "notice: other refusals once per session, as before");
        Check(EnterNotice(EnterVerdict::RefuseReloadPath, 0).find("save reload is not available") != std::string::npos,
            "notice: the reload path text kept");
        Check(EnterNotice(EnterVerdict::Clear, 0).empty() && EnterNotice(EnterVerdict::AlreadyCleared, 0).empty(),
            "notice: none when nothing was refused");
        EnterState noSave = s;
        noSave.newestSlot = -1;
        Check(DecideEnter(noSave) == EnterVerdict::RefuseNoSave, "enter: no save, nothing cleared (the engine would start a new game)");
        EnterState unread = s;
        unread.slotsRead = false;
        Check(DecideEnter(unread) == EnterVerdict::RefuseSlotRead, "enter: the slot table unreadable, nothing cleared");
        EnterState noPath = s;
        noPath.reloadReady = false;
        Check(DecideEnter(noPath) == EnterVerdict::RefuseReloadPath, "enter: a signature mismatch, nothing cleared");
        EnterState online = s;
        online.online = true;
        Check(DecideEnter(online) == EnterVerdict::RefuseOnline, "enter: online, nothing cleared");
        EnterState again = s;
        again.alreadyCleared = true;
        again.inMission = true;
        Check(DecideEnter(again) == EnterVerdict::AlreadyCleared, "enter: the second playback finds it cleared");
        EnterState pool = s;
        pool.step = kStepPool;
        Check(DecideEnter(pool) == EnterVerdict::StepPoolOnly, "enter: WorldModelStep=1 records the pool only, nothing cleared");
        EnterState step2 = noPath;
        step2.step = kStepClear;
        Check(DecideEnter(step2) == EnterVerdict::Clear, "enter: WorldModelStep=2 clears without the reload path (reload by hand)");
        step2.newestSlot = -1;
        Check(DecideEnter(step2) == EnterVerdict::RefuseNoSave, "enter: WorldModelStep=2 still needs a save to go back to");
        Check(StepFromIni(0) == 1 && StepFromIni(2) == 2 && StepFromIni(9) == 3 && kStepReload == 3, "ini: WorldModelStep 1..3");
        Check(std::strcmp(EnterVerdictName(EnterVerdict::RefuseMission), "refuse_mission") == 0 &&
                std::strcmp(EnterVerdictName(EnterVerdict::Clear), "clear") == 0,
            "enter: verdict names for the log");
    }

    void TestExit()
    {
        ExitState s;
        s.mode = Mode::Gta;
        Check(DecideExit(s) == ExitVerdict::None, "exit: the world not cleared, no reload");
        s.cleared = true;
        s.playing = true;
        Check(DecideExit(s) == ExitVerdict::InReplay, "exit: playing, still in the replay");
        s.playing = false;
        s.editorOpen = true;
        Check(DecideExit(s) == ExitVerdict::InReplay, "exit: the editor open (gallery between clips), still in the replay");
        s.editorOpen = false;
        s.clipOpening = true;
        Check(DecideExit(s) == ExitVerdict::InReplay, "exit: a clip opening, still in the replay");
        s.clipOpening = false;
        s.exitFading = true;
        Check(DecideExit(s) == ExitVerdict::InReplay, "exit: the exit wait still runs");
        s.exitFading = false;
        s.idleMs = kExitSettleMs - 1;
        Check(DecideExit(s) == ExitVerdict::Settle, "exit: a clip switch has a second to start the next one");
        s.idleMs = kExitSettleMs;
        Check(DecideExit(s) == ExitVerdict::Reload, "exit: out of the replay for a second: reload");
        s.step = kStepClear;
        Check(DecideExit(s) == ExitVerdict::Manual, "exit: WorldModelStep=2 leaves the reload to the player");
        s.step = kStepReload;
        s.reloadStarted = true;
        Check(DecideExit(s) == ExitVerdict::None, "exit: one push only");
        s.reloadStarted = false;
        s.mode = Mode::Off;
        Check(DecideExit(s) == ExitVerdict::None, "exit: model off, never");
    }

    void TestPush()
    {
        PushState s;
        s.signaturesOk = true;
        s.opRead = true;
        s.queueRead = true;
        s.newestSlot = 4;
        Check(DecidePush(s) == PushVerdict::Push, "push: checks pass, queue empty: push");
        PushState sig = s;
        sig.signaturesOk = false;
        Check(DecidePush(sig) == PushVerdict::RefuseSignature, "push: a signature mismatch never calls");
        PushState saving = s;
        saving.savePending = true;
        Check(DecidePush(saving) == PushVerdict::Wait, "push: a save pending waits");
        PushState loading = s;
        loading.loadingScreen = true;
        Check(DecidePush(loading) == PushVerdict::Wait, "push: a loading screen waits");
        PushState queued = s;
        queued.op = OpCheck::Queued;
        Check(DecidePush(queued) == PushVerdict::RefuseOp, "push: the operation already queued is not pushed twice");
        PushState busy = s;
        busy.queueCount = 1;
        Check(DecidePush(busy) == PushVerdict::RefuseQueue, "push: another operation in the queue: refused");
        PushState unread = s;
        unread.opRead = false;
        Check(DecidePush(unread) == PushVerdict::RefuseOpRead, "push: the operation unreadable: refused");
        PushState gone = s;
        gone.newestSlot = -1;
        Check(DecidePush(gone) == PushVerdict::RefuseNoSave, "push: the save gone since the clear: refused (no new game)");
        PushState slot = s;
        slot.path = ReloadPath::Slot;
        slot.opRead = false;
        slot.queueCount = 3;
        Check(DecidePush(slot) == PushVerdict::Push, "push (plan B): the queue checks do not apply");
        slot.menuOpBusy = true;
        Check(DecidePush(slot) == PushVerdict::RefuseMenuBusy, "push (plan B): the menu operation busy: refused");
    }
}

int RunWorldModelTests()
{
    g_worldModelFailures = 0;
    TestIni();
    TestCapacityWithModel();
    TestSlots();
    TestOpLayout();
    TestSites();
    TestEnter();
    TestExit();
    TestPush();
    return g_worldModelFailures;
}
