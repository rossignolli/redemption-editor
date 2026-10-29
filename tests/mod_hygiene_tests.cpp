// The log policy (what Debug=0 keeps, the shared LogLimit with a session reset),
// the one microsecond clock (no 21-day overflow), the guarded memory copy,
// the VirtualQuery region cache of the clip pointer audit and the rejected
// ropes of the rope recorder.
#include "guarded_memory.h"
#include "log_policy.h"
#include "mod_clock.h"
#include "mod_identity.h"
#include "qa_report.h"
#include "region_cache.h"
#include "rope_track.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int g_hygieneFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_hygieneFailures;
    }

    void TestEssentialLines()
    {
        using replay::log::EssentialLine;
        // Kept with Debug=0.
        Check(EssentialLine("=== Redemption Editor loaded ===\n"), "the load banner is kept");
        Check(EssentialLine("buildVersion=0.170.0\n"), "header lines (lower case) are kept");
        Check(EssentialLine("RECORDING_START,kind=manual\n"), "recording lines are kept");
        Check(EssentialLine("RECORD_GATE_TOTAL,paused_ms=0\n"), "the recording gate is kept");
        Check(EssentialLine("REC_STOP_TIMING,recorder_stop_ms=8\n"), "the stop timing is kept");
        Check(EssentialLine("CLIP_LOAD,path=x,ok=1\n"), "clip loads are kept");
        Check(EssentialLine("CLIP_LIST,clips=3\n"), "the clip list is kept (editor_e2e.py)");
        Check(EssentialLine("BUFFER_SAVE,ok=1\n"), "buffer saves are kept");
        Check(EssentialLine("POSE_V2_STOP_TIMING,events_end_us=316\n"), "*_TIMING is kept");
        Check(EssentialLine("POSE_V2_BLOOD_TOTAL,recorded=3\n"), "*_TOTAL is kept");
        Check(EssentialLine("PUPPET_WATCH_SUMMARY,episodes=0\n"), "*_SUMMARY is kept");
        Check(EssentialLine("POSE_V2_AUDIO_OBSERVER,status=ready\n"), "observer install lines are kept");
        Check(EssentialLine("CLIP_COMPRESS_ERROR,exception=bad allocation\n"), "errors are kept");
        Check(EssentialLine("POSE_V2_PLAYBACK_ARM_FAILED,id=1\n"), "failures are kept");
        Check(EssentialLine("DETACH_STEP,step=hooks\n"), "unload steps are kept (crash evidence)");
        Check(EssentialLine("RING_PROTOCOL,writer_dropped=0,torn=0,lost=2,waited=0\n"), "the ring counts at unload are kept");
        Check(EssentialLine("PROFILE_TOTAL_FRAME,mode=play\n"), "session profiler totals are kept");
        Check(EssentialLine("EDITOR_COMMAND,kind=play\n"), "user commands are kept (editor_e2e.py)");
        Check(EssentialLine("JUMP_LOAD,ms=63\n"), "jump loads are kept (editor_e2e.py)");
        Check(EssentialLine("PLAYBACK_STOP,reason=user\n"), "playback stops are kept");
        // The public build's shake lines (release doc: they stay in both builds).
        Check(EssentialLine("SHAKE_MIGRATION,legacy_markers=2,events=0,on_markers=0,breakpoints=0,markers_added=0,pruned=0,"
                            "inexact=0,proof=skipped,source=load,verify_ms=0,clip=a.rpclip\n"),
            "the shake migration line is kept");
        Check(EssentialLine("CAMERA_SHAKE,action=in_force,marker=3,active=1\n") &&
                EssentialLine("CAMERA_SHAKE,action=set,id=3,done=1\n"),
            "the camera shake lines are kept");
        Check(!EssentialLine("POSE_V2_PLAYBACK_PUPPET,id=1\n"), "per-puppet playback lines are evidence");
        Check(EssentialLine(""), "an empty line is kept");
        // QA log, Debug=0.
        Check(EssentialLine("ASI_IDENTITY,version=1.0.0-dev,sha256=ab\n"), "the ASI identity is kept");
        Check(EssentialLine("CONFIG_INI,found=1,keys=Debug=0\n"), "the ini as read is kept");
        Check(EssentialLine("REPORT_SAVE,ok=1,dir=ReplayReports\\report-x\n"), "the report hotkey is kept");
        Check(EssentialLine("POSE_FINALIZER,refused=held\n"), "the finalizer refusals are kept");
        Check(EssentialLine("POSE_V2_FIRE_CHAR,status=disabled,error=char_remove_signature_mismatch\n"),
            "a signature mismatch is kept whatever the tag");
        Check(EssentialLine("POSE_V2_VEGETATION,status=failed,error=mh_create_MH_ERROR_NOT_EXECUTABLE\n"),
            "a MinHook failure is kept whatever the tag");
        Check(EssentialLine("POSE_V2_FIRE_STATE,last_error=exception_C0000005@rva_0x1\n"), "a guarded fault is kept");
        Check(!EssentialLine("POSE_V2_VEGETATION,status=installed,error=none\n"), "an install that worked is evidence");
        // Dropped with Debug=0.
        Check(!EssentialLine("POSE_V2_ROPE_FOLLOW,side=record,engine_id=1\n"), "rope follow is evidence");
        Check(!EssentialLine("POSE_V2_ANIM_EVENT,id=1\n"), "anim events are evidence");
        Check(!EssentialLine("PROFILE_ZONE,mode=idle,zone=tick\n"), "profiler window lines are evidence");
        Check(!EssentialLine("POSE_V2_CLIP_POINTER_AUDIT,blobs=\n"), "the pointer audit is evidence");
        Check(!EssentialLine("POSE_V2_PED_AUDIO_FORCE,id=1\n"), "ped audio lines are evidence");
        // The tag ends at the first separator: a word later in the line
        // does not make it essential.
        Check(!EssentialLine("POSE_V2_SURFACE,status=failed\n"), "only the tag is matched");
        Check(replay::log::TagOf("ABC_DEF,x=1") == "ABC_DEF", "TagOf stops at the comma");
        Check(replay::log::TagOf("ABC") == "ABC", "TagOf of a bare tag");
    }

    // QA package: the report folder and report.txt.
    void TestQaReport()
    {
        Check(replay::qareport::FolderName(2026, 10, 4, 3, 5, 9) == "report-20261004-030509", "report: folder name");
        replay::qareport::Summary summary;
        summary.version = "1.0.0-dev";
        summary.lastClip = "042-Valentine.rpclip";
        const std::string text = replay::qareport::SummaryText(summary);
        Check(text.find("version: 1.0.0-dev\r\n") != std::string::npos, "report: the version");
        Check(text.find("last_clip: 042-Valentine.rpclip\r\n") != std::string::npos, "report: the last clip's name");
        Check(text.find("open_clip: none\r\n") != std::string::npos, "report: an empty field says none");
        bool hasLog = false, hasIni = false;
        for (const char* file : replay::qareport::kFiles)
        {
            hasLog = hasLog || std::string(file) == "RedemptionEditor.log";
            hasIni = hasIni || std::string(file) == "RedemptionEditor.ini";
            Check(std::string(file).find("ReplayProbe") == std::string::npos, "report: no old file name");
        }
        Check(hasLog && hasIni, "report: the log and the ini are copied");
    }

    void TestLogLimit()
    {
        replay::log::NewSession();
        replay::log::LogLimit limit;
        int taken = 0;
        for (int n = 0; n < 10; ++n)
            taken += limit.Take(3) ? 1 : 0;
        Check(taken == 3, "a cap of 3 takes 3");
        Check(limit.Seen() == 10, "Seen counts the refused calls too");
        // A second playback (NewSession) logs again: static counters must be
        // reset with the session.
        replay::log::NewSession();
        Check(limit.Seen() == 0, "a new session starts from zero");
        taken = 0;
        for (int n = 0; n < 10; ++n)
            taken += limit.Take(3) ? 1 : 0;
        Check(taken == 3, "a new session takes its cap again");
        // First N, then one in `every`.
        replay::log::NewSession();
        replay::log::LogLimit sparse;
        std::vector<int> yes;
        for (int n = 0; n < 20; ++n)
            if (sparse.Take(2, 5))
                yes.push_back(n);
        Check(yes.size() == 5 && yes[0] == 0 && yes[1] == 1 && yes[2] == 6 && yes[3] == 11 && yes[4] == 16,
            "first 2, then one in 5");
        // Verbose defaults on (the bench, the tests) and follows SetVerbose.
        Check(replay::log::Verbose(), "verbose by default");
        replay::log::SetVerbose(false);
        Check(!replay::log::Verbose(), "SetVerbose(false)");
        replay::log::SetVerbose(true);
    }

    void TestClock()
    {
        using replay::clock::TicksToUs;
        constexpr std::uint64_t k10MHz = 10000000ull;
        Check(TicksToUs(0, k10MHz) == 0, "zero ticks");
        Check(TicksToUs(k10MHz, k10MHz) == 1000000ull, "one second");
        Check(TicksToUs(15, k10MHz) == 1, "1.5 us rounds down like the old formula");
        // 22 days at 10 MHz: the naive `ticks * 1000000 / frequency` overflows
        // 2^64 at ~21.35 days and comes back near zero.
        const std::uint64_t days22 = 22ull * 86400ull * k10MHz;
        const std::uint64_t us = TicksToUs(days22, k10MHz);
        Check(us == 22ull * 86400ull * 1000000ull, "22 days at 10 MHz does not overflow");
        volatile std::uint64_t scale = 1000000ull; // not folded at compile time
        const std::uint64_t oldFormula = days22 * scale / k10MHz; // wraps
        Check(oldFormula != us, "the old formula did overflow there (the bug this fixes)");
        // Same result as the naive formula wherever it does not overflow.
        for (std::uint64_t ticks : {1ull, 999ull, 123456789ull, 987654321987ull})
            Check(TicksToUs(ticks, 3579545ull) == ticks * 1000000ull / 3579545ull, "same value below the overflow");
        Check(TicksToUs(123, 0) == 0, "no frequency, no time");
        Check(replay::clock::NowUs() > 0, "the live clock runs");
    }

    void TestGuardedCopy()
    {
        using namespace replay::mem;
        std::uint64_t source = 0x1122334455667788ull;
        std::uint64_t out = 0;
        Check(Copy(&out, &source, sizeof(out)) && out == source, "a readable copy");
        Check(Read(reinterpret_cast<std::uintptr_t>(&source), out) && out == source, "Read<T>");
        Check(!Copy(&out, static_cast<const void*>(nullptr), 8), "null is refused");
        Check(!Copy(&out, std::uintptr_t{0x1000}, 8), "the null region is refused without a fault");
        Check(!Copy(&out, std::uintptr_t{0xFFFF800000000000ull}, 8), "kernel addresses are refused");
        Check(!Copy(&out, std::uintptr_t{0x7FFFFFFFFFFCull}, 8), "a range crossing the user-mode end is refused");
        Check(Copy(&out, static_cast<const void*>(nullptr), 0), "a zero-size copy succeeds");
        // The audio floor: below 4 GB is refused with kGameHeapLowest.
        Check(!InUserRange(0x7FFE0000ull, 8, kGameHeapLowest), "the audio floor refuses below 4 GB");
        Check(InUserRange(0x7FFE0000ull, 8), "the default floor accepts it");
        // A committed but no-access page faults and is caught.
        void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
        Check(page != nullptr, "a no-access page");
        if (page)
        {
            Check(!Copy(&out, page, 8), "a faulting read returns false");
            Check(!Put(reinterpret_cast<std::uintptr_t>(page), &source, 8), "a faulting write returns false");
            VirtualFree(page, 0, MEM_RELEASE);
        }
        std::uint64_t target = 0;
        Check(Put(reinterpret_cast<std::uintptr_t>(&target), &source, 8) && target == source, "a guarded write");
        const std::uint8_t signature[] = {0x88, 0x77, 0x66};
        Check(Matches(&source, signature, sizeof(signature)), "signature match (little endian)");
        const std::uint8_t other[] = {0x88, 0x77, 0x00};
        Check(!Matches(&source, other, sizeof(other)), "signature mismatch");
    }

    void TestRegionCache()
    {
        // Two regions: [0x10000, 0x20000) committed, [0x20000, 0x30000) not.
        int queries = 0;
        const auto query = [&queries](std::uintptr_t address, std::uintptr_t& start, std::uintptr_t& end, bool& committed) {
            ++queries;
            if (address >= 0x10000 && address < 0x20000)
            {
                start = 0x10000;
                end = 0x20000;
                committed = true;
                return true;
            }
            if (address >= 0x20000 && address < 0x30000)
            {
                start = 0x20000;
                end = 0x30000;
                committed = false;
                return true;
            }
            return false;
        };
        replay::mem::RegionCache cache;
        int committed = 0;
        for (std::uintptr_t address = 0x10000; address < 0x30000; address += 8)
            committed += cache.Committed(address, query) ? 1 : 0;
        Check(committed == 0x10000 / 8, "every word of the committed region counts");
        Check(queries == 2 && cache.Queries() == 2, "one query per region, not one per word");
        Check(cache.Hits() == 0x20000 / 8 - 2, "the rest are cache hits");
        Check(!cache.Committed(0x40000, query) && queries == 3, "an unknown address is asked and not committed");
        Check(!cache.Committed(0x40000, query) && queries == 4, "a failed query is not cached");
    }

    void TestRejectedRopes()
    {
        using replay::ropes::RejectedRopes;
        RejectedRopes rejected;
        Check(rejected.BeginFrame(0), "the first frame rechecks");
        Check(!rejected.Skip(7, 111), "an unknown rope is evaluated");
        rejected.Reject(7, 111);
        Check(!rejected.BeginFrame(100), "no recheck before 250 ms");
        Check(rejected.Skip(7, 111), "the same rope with the same entries is skipped");
        Check(!rejected.Skip(7, 222), "changed entries are evaluated again");
        rejected.Reject(7, 222);
        Check(rejected.Size() == 1 && rejected.Skip(7, 222), "the new key replaces the old one");
        Check(rejected.BeginFrame(250), "the recheck comes at 250 ms");
        Check(!rejected.Skip(7, 222) && rejected.Size() == 0, "a recheck evaluates every rope again");
        // The recording clock restarting (a new recording) rechecks at once.
        rejected.Reject(9, 1);
        Check(rejected.BeginFrame(10), "a clock that went back rechecks");
        rejected.Reset();
        Check(rejected.BeginFrame(0) && rejected.Size() == 0, "Reset empties and rechecks");

        using replay::ropes::AttachKey;
        const std::uint64_t entities[2] = {0x1000, 0x2000};
        const std::int32_t bones[2] = {5, -1};
        const std::uint64_t swapped[2] = {0x2000, 0x1000};
        const std::int32_t otherBones[2] = {6, -1};
        Check(AttachKey(entities, bones, 2) == AttachKey(entities, bones, 2), "the key is stable");
        Check(AttachKey(entities, bones, 2) != AttachKey(swapped, bones, 2), "entry order matters");
        Check(AttachKey(entities, bones, 2) != AttachKey(entities, otherBones, 2), "bones matter");
        Check(AttachKey(entities, bones, 1) != AttachKey(entities, bones, 2), "the count matters");
    }
}

namespace
{
    // The current names and the upgrade from an install with ReplayProbe.ini
    // [ReplayPoC].
    bool WriteText(const std::string& path, const std::string& text)
    {
        FILE* file = nullptr;
        if (fopen_s(&file, path.c_str(), "wb") != 0 || !file)
            return false;
        const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
        return std::fclose(file) == 0 && ok;
    }

    std::string ReadText(const std::string& path)
    {
        std::string text;
        FILE* file = nullptr;
        if (fopen_s(&file, path.c_str(), "rb") == 0 && file)
        {
            char buffer[512];
            std::size_t read = 0;
            while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
                text.append(buffer, read);
            std::fclose(file);
        }
        return text;
    }

    void TestIdentity()
    {
        using namespace replay::identity;
        Check(std::string(kAsiFile) == "RedemptionEditor.asi" && std::string(kIniFile) == "RedemptionEditor.ini" &&
                  std::string(kLogFile) == "RedemptionEditor.log" && std::string(kCrashLogFile) == "RedemptionEditor.crash.log" &&
                  std::wstring(kCoreFileW) == L"RedemptionEditorCore.dll" && std::wstring(kAddonFileW) == L"RedemptionEditor.addon64",
            "identity: the shipped names");
        Check(RenameLegacySection("; c\r\n[ReplayPoC]\r\nDebug=1\r\n[Export]\r\nEnabled=1\r\n") ==
                  "; c\r\n[RedemptionEditor]\r\nDebug=1\r\n[Export]\r\nEnabled=1\r\n",
            "identity: [ReplayPoC] renamed, [Export] kept (CRLF)");
        Check(RenameLegacySection("  [replaypoc]  \nA=1") == "[RedemptionEditor]\nA=1", "identity: any case, spaces, LF, no final newline");
        Check(RenameLegacySection("[ReplayPoC2]\n") == "[ReplayPoC2]\n", "identity: another section is not touched");

        char temp[MAX_PATH]{};
        GetTempPathA(MAX_PATH, temp);
        const std::string folder = std::string(temp) + "redemption-identity-test-" + std::to_string(GetCurrentProcessId());
        CreateDirectoryA(folder.c_str(), nullptr);
        const std::string legacy = folder + "\\ReplayProbe.ini";
        const std::string current = folder + "\\RedemptionEditor.ini";
        DeleteFileA(legacy.c_str());
        DeleteFileA(current.c_str());

        Check(IniPathIn(folder) == current, "identity: no ini at all: the new name");
        Check(MigrateLegacyIni(folder) == IniMigration::None, "identity: nothing to migrate");
        Check(WriteText(legacy, "[ReplayPoC]\r\nDebug=1\r\nReplayBuffer=0\r\n[Export]\r\nEnabled=1\r\n"), "identity: old ini written");
        Check(IniPathIn(folder) == legacy, "identity: only the old ini: read it");
        Check(IniInt("Debug", 0, legacy.c_str()) == 1, "identity: a key of the old section is read");
        Check(IniInt("Missing", 7, legacy.c_str()) == 7, "identity: a missing key keeps its default");
        Check(MigrateLegacyIni(folder) == IniMigration::Copied, "identity: the old ini is copied");
        Check(IniPathIn(folder) == current, "identity: after the copy the new ini is read");
        const std::string copied = ReadText(current);
        Check(copied.find("[RedemptionEditor]\r\nDebug=1") != std::string::npos && copied.find("[ReplayPoC]") == std::string::npos &&
                  copied.find("[Export]\r\nEnabled=1") != std::string::npos,
            "identity: the copy has the new section and keeps [Export]");
        Check(ReadText(legacy).find("[ReplayPoC]") != std::string::npos, "identity: the old ini is left as it was");
        Check(MigrateLegacyIni(folder) == IniMigration::None, "identity: copied once only");
        Check(IniInt("ReplayBuffer", 1, current.c_str()) == 0, "identity: the new section is read");
        IniWrite("ReplayBuffer", "1", current.c_str());
        Check(IniInt("ReplayBuffer", 0, current.c_str()) == 1, "identity: a write goes to the new section");

        // A hand-made ini with both sections: the new one wins, the old fills in.
        Check(WriteText(current, "[ReplayPoC]\r\nDebug=1\r\nQaMarks=0\r\n[RedemptionEditor]\r\nDebug=0\r\n"), "identity: mixed ini written");
        Check(IniInt("Debug", 5, current.c_str()) == 0, "identity: the new section wins");
        Check(IniInt("QaMarks", 1, current.c_str()) == 0, "identity: the old section is the fallback");
        std::vector<char> section(512);
        const DWORD length = IniSection(section.data(), static_cast<DWORD>(section.size()), current.c_str());
        Check(length > 0 && std::string(section.data()) == "Debug=0", "identity: the section text is the new one's");

        DeleteFileA(legacy.c_str());
        DeleteFileA(current.c_str());
        RemoveDirectoryA(folder.c_str());

        const char* other = "";
        Check(!AnotherCopyLoaded(other), "identity: the first copy runs");
        // ScriptMain run again in the same module (scripts restarted by the game).
        bool restarted = false;
        Check(!AnotherCopyLoaded(other, &restarted) && restarted, "identity: a script restart in the same module runs again");
        HANDLE first = g_copyMutex;
        g_copyMutex = nullptr;
        Check(AnotherCopyLoaded(other) && std::string(other) == "another RedemptionEditor.asi", "identity: a second copy is refused");
        g_copyMutex = first;
        ReleaseCopyMutex();
        Check(!AnotherCopyLoaded(other), "identity: after the unload (Ctrl+R) the next copy runs");
        ReleaseCopyMutex();
    }
}

int RunModHygieneTests()
{
    TestIdentity();
    TestEssentialLines();
    TestQaReport();

    TestLogLimit();
    TestClock();
    TestGuardedCopy();
    TestRegionCache();
    TestRejectedRopes();
    if (g_hygieneFailures == 0)
        std::printf("Mod hygiene tests passed.\n");
    return g_hygieneFailures;
}
