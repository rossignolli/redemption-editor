// The public ini whitelist (src/ini_public.h), the developer reads
// (mod_identity.h DevIniInt, DevIniString), the shipped ini written on the
// first run, the clips a report takes (qa_report.h ReportClips) and the crash
// notice decision (src/crash_notice.h).
#include "crash_notice.h"
#include "editor_keys.h"
#include "ini_public.h"
#include "mod_identity.h"
#include "qa_report.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    void CheckKey(bool condition, const char* what, const std::string& key)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s: %s\n", what, key.c_str());
        ++g_failures;
    }

    using replay::inipublic::IniLine;

    std::vector<IniLine> Shipped() { return replay::inipublic::Parse(replay::inipublic::kShippedIni); }

    bool IsSupportSwitch(const char* section, const char* key)
    {
        return std::strcmp(section, "RedemptionEditor") == 0 && replay::inipublic::FindSupportSwitch(key) != nullptr;
    }

    // Every key the public build reads is in the shipped ini, as an active
    // line or a documented (";Key=") one, or is a support switch (read, not
    // shipped), never both.
    void TestWhitelistDocumented()
    {
        const std::vector<IniLine> lines = Shipped();
        Check(lines.size() > 70, "the shipped ini has its keys");
        for (const auto& read : replay::inipublic::kPublicReadKeys)
        {
            const bool shippedLine = replay::inipublic::Find(lines, read.section, read.key) != nullptr;
            const bool support = IsSupportSwitch(read.section, read.key);
            CheckKey(shippedLine || support, "a public read neither in the shipped ini nor a support switch",
                std::string(read.section) + "." + read.key);
            CheckKey(!(shippedLine && support), "a support switch in the shipped ini", std::string(read.section) + "." + read.key);
        }
        // No key twice in the whitelist.
        std::set<std::string> seen;
        for (const auto& read : replay::inipublic::kPublicReadKeys)
            CheckKey(seen.insert(std::string(read.section) + "." + read.key).second, "a key twice in kPublicReadKeys",
                std::string(read.section) + "." + read.key);
        // Every shipped line is a key the public build reads (no dead line in the file).
        for (const IniLine& line : lines)
        {
            bool read = false;
            for (const auto& entry : replay::inipublic::kPublicReadKeys)
                read = read || (line.key == entry.key && replay::inipublic::SameSection(line.section, entry.section));
            CheckKey(read, "a shipped line no public read uses", line.section + "." + line.key);
        }
        // One line per key and section family.
        std::set<std::string> lineKeys;
        for (const IniLine& line : lines)
        {
            const std::string family = line.section == "Troubleshooting" ? "RedemptionEditor" : line.section;
            CheckKey(lineKeys.insert(family + "." + line.key).second, "a key twice in the shipped ini", line.key);
        }
    }

    // The engine safety switches (support switches): out of the shipped
    // ini, read by the public build, on.
    void TestSupportSwitches()
    {
        const std::vector<IniLine> lines = Shipped();
        for (const IniLine& line : lines)
            CheckKey(line.section != "Troubleshooting", "the shipped ini has a [Troubleshooting] line", line.key);
        Check(replay::inipublic::kSupportSwitchCount >= 40, "the engine safety switches are listed");
        std::set<std::string> seen;
        for (const auto& entry : replay::inipublic::kSupportSwitches)
        {
            const std::string value = entry.value;
            CheckKey(seen.insert(entry.key).second, "a support switch twice", entry.key);
            CheckKey(!value.empty() && value != "0" && value != "off", "a support switch is not on", entry.key);
            CheckKey(replay::inipublic::Find(lines, "RedemptionEditor", entry.key) == nullptr, "a support switch in the shipped ini", entry.key);
            bool read = false;
            for (const auto& key : replay::inipublic::kPublicReadKeys)
                read = read || (std::strcmp(key.section, "RedemptionEditor") == 0 && std::strcmp(key.key, entry.key) == 0);
            CheckKey(read, "a support switch the public build does not read", entry.key);
        }
        // The switches that must stay readable.
        const char* const named[] = {"TheatreHooks", "ReinsHoldGuard", "VehicleDoorWrite", "WeaponWearWrite", "DecalGate", "WorldModel"};
        for (const char* key : named)
            CheckKey(replay::inipublic::FindSupportSwitch(key) != nullptr, "a named engine switch is not a support switch", key);
        // Reports keep their keys in the shipped ini.
        for (const char* key : {"FullLog", "QaMarks"})
        {
            const IniLine* line = replay::inipublic::Find(lines, "RedemptionEditor", key);
            CheckKey(line && !line->commented && line->section == "RedemptionEditor", "a report key missing from the shipped ini", key);
        }
        // Research probes are never in the shipped ini nor support switches.
        const char* const probes[] = {"VolumeProbe", "SoundFxProbe", "MixProbe", "WaterGridProbe", "SunProbe", "CaptureProbe",
            "SpeechLevelProbe", "SoundCensus", "WorldSoundProbe", "CategoryStateLog", "HorseAudioEvidence", "ObjectLookEvidence",
            "ClipSaveFailTest", "Debug", "SunOverride", "ReplayGameListener", "MixSceneReplay"};
        for (const char* key : probes)
            CheckKey(replay::inipublic::Find(lines, "RedemptionEditor", key) == nullptr, "a developer key in the shipped ini", key);
        for (const char* key : probes)
            CheckKey(replay::inipublic::FindSupportSwitch(key) == nullptr, "a developer key among the support switches", key);
    }

    // [Keys]: every action is listed with its default keys, and the
    // shipped values load to the very table the defaults give, no issue.
    void TestShippedKeys()
    {
        using namespace replay::keys;
        const std::vector<IniLine> lines = Shipped();
        for (const ActionInfo& info : kActions)
        {
            const IniLine* line = replay::inipublic::Find(lines, "Keys", info.iniKey);
            CheckKey(line && !line->commented, "an action missing from the shipped [Keys]", info.iniKey);
            if (line)
                CheckKey(line->value == info.defaults, "a shipped key that is not the default", info.iniKey);
            bool listed = false;
            for (const auto& read : replay::inipublic::kPublicReadKeys)
                listed = listed || (std::strcmp(read.section, "Keys") == 0 && std::strcmp(read.key, info.iniKey) == 0);
            CheckKey(listed, "an action missing from kPublicReadKeys", info.iniKey);
        }
        const LoadResult absent = Load([](const char*, bool& present) {
            present = false;
            return std::string();
        });
        const LoadResult shipped = Load([&lines](const char* key, bool& present) {
            const IniLine* line = replay::inipublic::Find(lines, "Keys", key);
            present = line && !line->commented;
            return present ? line->value : std::string();
        });
        Check(shipped.issues.empty(), "the shipped [Keys] load without an issue");
        Check(shipped.configured == kActionCount, "the shipped [Keys] name every action");
        bool same = true;
        for (std::size_t action = 0; action < kActionCount; ++action)
            for (std::size_t slot = 0; slot < kSlots; ++slot)
                same = same && absent.table.keys[action][slot] == shipped.table.keys[action][slot];
        Check(same, "the shipped [Keys] give the default table");
    }

    // The active values that are plain numbers are the code's defaults
    // (tools/check_public_build.py compares them with the read sites; a few
    // known ones are pinned here too).
    void TestShippedDefaults()
    {
        const std::vector<IniLine> lines = Shipped();
        const auto value = [&lines](const char* section, const char* key) {
            const IniLine* line = replay::inipublic::Find(lines, section, key);
            if (line)
                return line->value;
            const replay::inipublic::SupportSwitch* support =
                std::strcmp(section, "RedemptionEditor") == 0 ? replay::inipublic::FindSupportSwitch(key) : nullptr;
            return support ? std::string(support->value) : std::string("<missing>");
        };
        Check(value("RedemptionEditor", "ReplayBuffer") == "1", "ReplayBuffer=1 (default on)");
        Check(value("RedemptionEditor", "ReplayBufferSeconds") == "60", "ReplayBufferSeconds=60");
        Check(value("RedemptionEditor", "ReplayBufferMB") == "128", "ReplayBufferMB=128");
        Check(value("RedemptionEditor", "CompressAfterDays") == "1", "CompressAfterDays=1 (clip_compress.h kDefaultDays)");
        Check(value("RedemptionEditor", "CompressLevel") == "19", "CompressLevel=19 (clip_compress.h kDefaultLevel)");
        Check(value("RedemptionEditor", "FullLog") == "1", "FullLog=1: reports come with the whole log");
        Check(value("RedemptionEditor", "ReportIncludeClips") == "0", "ReportIncludeClips=0 (marked clips only)");
        Check(value("RedemptionEditor", "TheatreHooks") == "31", "TheatreHooks=31 (world_theatre.h Config::hooks)");
        Check(value("Export", "Enabled") == "1", "[Export] Enabled=1 (release 1.0: on, as the QA package)");
        Check(value("RedemptionEditor", "SoundGeneric") == "1", "SoundGeneric=1 (release 1.0: on, as the QA package)");
    }

    std::string TempFolder(const char* name)
    {
        char temp[MAX_PATH]{};
        GetTempPathA(MAX_PATH, temp);
        std::string folder = std::string(temp) + name + std::to_string(GetCurrentProcessId());
        CreateDirectoryA(folder.c_str(), nullptr);
        return folder;
    }

    // The developer reads: the public build takes the default without
    // opening the file; the developer build reads the file.
    void TestDevReads()
    {
        const std::string folder = TempFolder("re_ini_dev_");
        const std::string ini = folder + "\\RedemptionEditor.ini";
        WritePrivateProfileStringA("RedemptionEditor", "VolumeProbe", "1", ini.c_str());
        WritePrivateProfileStringA("RedemptionEditor", "WorldModel", "off", ini.c_str());
        WritePrivateProfileStringA("Troubleshooting", "ReinsHoldGuard", "0", ini.c_str());
        char text[16]{};
        const UINT probe = replay::identity::DevIniInt("VolumeProbe", 0, ini.c_str());
        const DWORD length = replay::identity::DevIniString("WorldModel", "gta", text, sizeof(text), ini.c_str());
        if constexpr (replay::identity::kDevBuild)
        {
            Check(probe == 1, "dev build: DevIniInt reads the file");
            Check(std::strcmp(text, "off") == 0 && length == 3, "dev build: DevIniString reads the file");
        }
        else
        {
            Check(probe == 0, "public build: DevIniInt keeps the default");
            Check(std::strcmp(text, "gta") == 0 && length == 3, "public build: DevIniString copies the default");
        }
        // The default is cut like GetPrivateProfileString cuts it.
        char cutBuffer[4]{};
        const DWORD cut = replay::identity::DevIniString("NoSuchKey", "abcdef", cutBuffer, sizeof(cutBuffer), ini.c_str());
        Check(cut == 3 && std::strcmp(cutBuffer, "abc") == 0, "DevIniString cuts the default to the buffer");
        Check(static_cast<int>(replay::identity::DevIniInt("NoSuchKey", -1, ini.c_str())) == -1, "DevIniInt keeps a negative default");
        // [Troubleshooting] is read for the [RedemptionEditor] keys.
        Check(replay::identity::IniInt("ReinsHoldGuard", 1, ini.c_str()) == 0, "a switch set in [Troubleshooting] is read");
        Check(replay::identity::IniInt("VehicleDoorWrite", 1, ini.c_str()) == 1, "a switch left out keeps its default");
        WritePrivateProfileStringA("RedemptionEditor", "ReinsHoldGuard", "1", ini.c_str());
        Check(replay::identity::IniInt("ReinsHoldGuard", 0, ini.c_str()) == 1, "[RedemptionEditor] wins over [Troubleshooting]");
        DeleteFileA(ini.c_str());
        RemoveDirectoryA(folder.c_str());
    }

    // The first run: the shipped text, CRLF, once; it reads as the defaults.
    void TestWriteDefaultIni()
    {
        const std::string folder = TempFolder("re_ini_first_");
        const std::string ini = folder + "\\RedemptionEditor.ini";
        DeleteFileA(ini.c_str());
        Check(replay::identity::WriteDefaultIni(folder, replay::inipublic::kShippedIni), "the first run writes the shipped ini");
        Check(!replay::identity::WriteDefaultIni(folder, replay::inipublic::kShippedIni), "a second run keeps the file");
        std::string body;
        {
            HANDLE file = CreateFileA(ini.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            char buffer[4096];
            DWORD read = 0;
            while (file != INVALID_HANDLE_VALUE && ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0)
                body.append(buffer, read);
            if (file != INVALID_HANDLE_VALUE)
                CloseHandle(file);
        }
        std::string expected;
        for (const char* c = replay::inipublic::kShippedIni; *c; ++c)
        {
            if (*c == '\n')
                expected += '\r';
            expected += *c;
        }
        Check(body == expected, "the written ini is the shipped text with CRLF");
        Check(replay::identity::IniInt("ReplayBuffer", 0, ini.c_str()) == 1, "the written ini reads ReplayBuffer=1");
        Check(replay::identity::IniInt("ReplayBufferSeconds", 0, ini.c_str()) == 60, "the written ini reads ReplayBufferSeconds=60");
        Check(replay::identity::IniInt("TheatreHooks", 31, ini.c_str()) == 31, "a documented switch keeps the default");
        char key[32]{};
        GetPrivateProfileStringA("Keys", "SaveBuffer", "", key, sizeof(key), ini.c_str());
        Check(std::strcmp(key, "Shift+F4") == 0, "the written ini reads [Keys] SaveBuffer");
        char folderValue[8] = "x";
        GetPrivateProfileStringA("Export", "Folder", "", folderValue, sizeof(folderValue), ini.c_str());
        Check(folderValue[0] == 0, "an empty value reads as the empty default");
        DeleteFileA(ini.c_str());
        // An old ReplayProbe.ini: no new file (MigrateLegacyIni copies it).
        const std::string legacy = folder + "\\ReplayProbe.ini";
        WritePrivateProfileStringA("ReplayPoC", "ReplayBuffer", "0", legacy.c_str());
        Check(!replay::identity::WriteDefaultIni(folder, replay::inipublic::kShippedIni), "an old ReplayProbe.ini is kept, nothing written");
        Check(GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES, "no RedemptionEditor.ini next to the old one");
        DeleteFileA(legacy.c_str());
        RemoveDirectoryA(folder.c_str());
    }

    // The report's clips: the marked ones; the open one only on request.
    void TestReportClips()
    {
        using replay::qareport::ReportClips;
        const std::vector<std::string> marked = {"a.rpclip", "b.rpclip", "a.rpclip", ""};
        const std::vector<std::string> only = ReportClips(marked, "open.rpclip", false);
        Check(only.size() == 2 && only[0] == "a.rpclip" && only[1] == "b.rpclip", "default: the marked clips only, once each, in order");
        const std::vector<std::string> with = ReportClips(marked, "open.rpclip", true);
        Check(with.size() == 3 && with[2] == "open.rpclip", "ReportIncludeClips=1 adds the open clip");
        Check(ReportClips(marked, "b.rpclip", true).size() == 2, "the open clip already marked goes in once");
        Check(ReportClips({}, "open.rpclip", false).empty(), "no mark, no clip");
        Check(ReportClips({}, "", true).empty(), "no open clip, nothing added");
        replay::qareport::Summary summary;
        Check(replay::qareport::SummaryText(summary).find("clips: marked only\r\n") != std::string::npos, "report.txt says marked only");
        summary.includeOpenClip = true;
        Check(replay::qareport::SummaryText(summary).find("clips: marked and open") != std::string::npos, "report.txt says marked and open");
    }

    // The crash notice decision.
    void TestCrashNotice()
    {
        using namespace replay::crashnotice;
        const DumpName name = ParseDumpName("RDR2_1.0.0-dev_20261012-222659_pid32424_t13320.dmp");
        Check(name.valid && name.build == "1.0.0-dev" && name.stamp == "20261012-222659" && name.pid == 32424,
            "a dump name is read");
        Check(ParseDumpName("RDR2_1.0.0_20261025-010203_pid7_t9.dmp").pid == 7, "a short pid");
        Check(ParseDumpName("RDR2_a_b_20261025-010203_pid7_t9.dmp").build == "a_b", "a build with '_'");
        Check(!ParseDumpName("RDR2_1.0.0_20261025-010203_pid7_t9.dmp.txt").valid, "the evidence text is not a dump");
        Check(!ParseDumpName("other.dmp").valid, "a dump of something else");
        Check(!ParseDumpName("RDR2_1.0.0_2026102-010203_pid7_t9.dmp").valid, "a bad stamp");
        Check(!ParseDumpName("RDR2_1.0.0_20261025-010203_pidx_t9.dmp").valid, "a bad pid");

        const std::uint64_t now = 133000000000000000ull;
        const std::uint64_t hour = 3600ull * kTicksPerSecond;
        std::vector<Dump> dumps = {
            {"RDR2_1.0.0_20261024-200000_pid100_t1.dmp", now - 20 * hour},
            {"RDR2_1.0.0_20261024-230000_pid200_t2.dmp", now - 2 * hour},
            {"RDR2_1.0.0_20261024-230000_pid200_t2.dmp.txt", now - 2 * hour},
            {"RDR2_1.0.0_20261010-120000_pid50_t3.dmp", now - 15 * 24 * hour},
        };
        Decision d = Decide(dumps, 999, 0, now);
        Check(d.show && d.crashes == 2 && d.newest == now - 2 * hour && d.stamp == "20261024-230000",
            "a crash of an earlier session shows the notice (the two recent dumps, not the old one)");
        Check(!Decide(dumps, 999, d.newest, now).show, "told once: the ack hides it");
        Check(Decide(dumps, 999, now - 10 * hour, now).crashes == 1, "only the dumps after the ack count");
        Check(!Decide(dumps, 200, now - 10 * hour, now).show, "this session's own dump is not a past crash");
        Check(!Decide({{"RDR2_1.0.0_20261010-120000_pid50_t3.dmp", now - 8 * 24 * hour}}, 999, 0, now).show,
            "a dump older than the report's 7 days is not told");
        Check(!Decide({}, 999, 0, now).show, "no dump, no notice");
        dumps.push_back({"RDR2_1.0.1_20261025-010000_pid300_t4.dmp", now - hour});
        const Decision next = Decide(dumps, 999, d.newest, now);
        Check(next.show && next.crashes == 1 && next.build == "1.0.1", "a new crash after the ack shows again");

        Check(ParseAck(AckText(d.newest)) == d.newest, "the ack round trips");
        Check(ParseAck("") == 0 && ParseAck("garbage") == 0 && ParseAck("99999999999999999999999") == 0, "a bad ack reads as none");
        Check(FormatStamp("20261024-230000") == "2026-10-24 23:00", "the crash time is shown");

        const Text text = NoticeText(d, replay::keys::DefaultTable(), "D:\\Games\\RDR2\\ReplayReports", false);
        Check(text.title == "Redemption Editor crashed last time", "the notice's title");
        Check(text.body.find("Shift+F1") != std::string::npos, "the notice names the report key");
        Check(text.body.find("D:\\Games\\RDR2\\ReplayReports") != std::string::npos, "the notice says where the ZIP goes");
        Check(text.body.find("only when you marked them with Shift+F2") != std::string::npos, "the notice says which clips go in");
        Check(text.body.find("(2 crashes, the last on 2026-10-24 23:00)") != std::string::npos, "two crashes: the notice counts them");
        replay::keys::Table remapped = replay::keys::DefaultTable();
        remapped.Of(replay::keys::Action::Report)[0] = {replay::keys::vk::F(11), true, false};
        remapped.Of(replay::keys::Action::MarkProblem)[0] = {'M', true, false};
        const Text open = NoticeText(next, remapped, "C:\\x\\ReplayReports", true);
        Check(open.body.find("press Ctrl+F11 to save") != std::string::npos && open.body.find("marked them with Ctrl+M") != std::string::npos &&
                open.body.find("Shift+F") == std::string::npos && open.body.find("plus the clip open in the editor") != std::string::npos,
            "the remapped keys of the table and ReportIncludeClips=1 are said");
        replay::keys::Table unbound = replay::keys::DefaultTable();
        unbound.Of(replay::keys::Action::Report)[0] = {};
        Check(NoticeText(next, unbound, "C:\\x", false).body.find("press the Report key to save") != std::string::npos,
            "an unbound report key is named in words");
        for (const char c : text.title + text.body)
            Check(static_cast<unsigned char>(c) < 0x80, "the notice is plain ASCII (the editor's font)");
    }
}

int RunIniPublicTests()
{
    g_failures = 0;
    TestWhitelistDocumented();
    TestSupportSwitches();
    TestShippedKeys();
    TestShippedDefaults();
    TestDevReads();
    TestWriteDefaultIni();
    TestReportClips();
    TestCrashNotice();
    std::printf("ini public tests: %s (%d failures)\n", g_failures ? "FAILED" : "ok", g_failures);
    return g_failures;
}
