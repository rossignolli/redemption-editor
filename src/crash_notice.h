#pragma once

// The notice after a crash.
//
// The crash handler (script.cpp WriteRequestedDump) writes one minidump per
// real crash (a fatal exception no __try of the mod takes) into
// %LOCALAPPDATA%\RedemptionEditor\crashes, named by crash_evidence_rules.h
// DumpFileName: RDR2_<build>_<yyyymmdd-hhmmss>_pid<pid>_t<thread>.dmp. On the
// next game start the editor (replay_editor_addon.cpp) lists that folder and
// asks Decide: a dump of another process than this one, newer than the last
// dump it told the player about and at most kMaxAgeDays old (the report ZIP
// carries the dumps of the last 7 days, qa_bundle.h) shows the notice, once;
// the newest such dump's time is then written to
// %LOCALAPPDATA%\RedemptionEditor\crash-notice.txt, so the same crash is never
// told twice and the next one is.
//
// Pure (no Windows header): times are FILETIME values (100 ns since 1601).

#include <cstdint>
#include <cstdio>
#include <string>

#include "editor_keys.h" // the report and mark keys the [Keys] table holds
#include "i18n.h"        // the notice in the player's language
#include <vector>

namespace replay::crashnotice
{
    constexpr std::uint64_t kTicksPerSecond = 10000000ull;
    constexpr std::uint64_t kTicksPerDay = 86400ull * kTicksPerSecond;
    constexpr std::uint32_t kMaxAgeDays = 7;
    // How long the notice stays on screen (the report key works the whole time);
    // opening the editor dismisses it.
    constexpr std::uint64_t kShowMs = 15000;
    constexpr char kAckFileName[] = "crash-notice.txt";

    struct Dump
    {
        std::string name;         // the file name, no folder
        std::uint64_t written = 0; // its last write time (FILETIME)
    };

    struct DumpName
    {
        bool valid = false;
        std::string build;   // the mod's version that crashed
        std::string stamp;   // yyyymmdd-hhmmss, local time
        unsigned long pid = 0;
    };

    // RDR2_<build>_<yyyymmdd-hhmmss>_pid<pid>_t<thread>.dmp (the build may
    // hold '_' too: the fields are read from the end).
    inline DumpName ParseDumpName(const std::string& name)
    {
        DumpName out;
        const std::string prefix = "RDR2_";
        const std::string suffix = ".dmp";
        if (name.size() <= prefix.size() + suffix.size() || name.compare(0, prefix.size(), prefix) != 0 ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            return out;
        const std::string body = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
        const std::size_t thread = body.rfind("_t");
        if (thread == std::string::npos)
            return out;
        const std::size_t pid = body.rfind("_pid", thread);
        if (pid == std::string::npos || pid + 4 >= thread)
            return out;
        const std::size_t stamp = body.rfind('_', pid - 1);
        if (stamp == std::string::npos || pid - stamp - 1 != 15 || body[stamp + 9] != '-')
            return out;
        unsigned long value = 0;
        for (std::size_t n = pid + 4; n < thread; ++n)
        {
            const char c = body[n];
            if (c < '0' || c > '9')
                return out;
            value = value * 10 + static_cast<unsigned long>(c - '0');
        }
        for (std::size_t n = thread + 2; n < body.size(); ++n)
            if (body[n] < '0' || body[n] > '9')
                return out;
        for (std::size_t n = stamp + 1; n < pid; ++n)
            if (n != stamp + 9 && (body[n] < '0' || body[n] > '9'))
                return out;
        out.build = body.substr(0, stamp);
        out.stamp = body.substr(stamp + 1, 15);
        out.pid = value;
        out.valid = !out.build.empty();
        return out;
    }

    struct Decision
    {
        bool show = false;
        std::uint32_t crashes = 0;   // dumps not told yet (1 or more when shown)
        std::uint64_t newest = 0;    // the newest of them (what the ack file keeps)
        std::string build;           // ... its build
        std::string stamp;           // ... and its time (yyyymmdd-hhmmss)
    };

    // `acknowledged`: the time in the ack file (0 when there is none);
    // `now`: the current FILETIME.
    inline Decision Decide(const std::vector<Dump>& dumps, unsigned long currentPid, std::uint64_t acknowledged, std::uint64_t now)
    {
        Decision out;
        const std::uint64_t oldest = now > kMaxAgeDays * kTicksPerDay ? now - kMaxAgeDays * kTicksPerDay : 0;
        for (const Dump& dump : dumps)
        {
            const DumpName parsed = ParseDumpName(dump.name);
            if (!parsed.valid || parsed.pid == currentPid)
                continue; // not a dump of ours, or this session's
            if (dump.written <= acknowledged || dump.written < oldest)
                continue; // told already, or too old for the report
            ++out.crashes;
            if (dump.written > out.newest)
            {
                out.newest = dump.written;
                out.build = parsed.build;
                out.stamp = parsed.stamp;
            }
        }
        out.show = out.crashes > 0;
        return out;
    }

    // The ack file: one line, the decimal FILETIME of the newest dump told.
    inline std::string AckText(std::uint64_t newest)
    {
        return std::to_string(newest) + "\r\n";
    }

    inline std::uint64_t ParseAck(const std::string& text)
    {
        std::uint64_t value = 0;
        bool digits = false;
        for (const char c : text)
        {
            if (c >= '0' && c <= '9')
            {
                if (value > (~0ull - 9) / 10)
                    return 0; // not a FILETIME of ours
                value = value * 10 + static_cast<std::uint64_t>(c - '0');
                digits = true;
            }
            else if (c == '\r' || c == '\n' || c == ' ')
            {
                if (digits)
                    break;
            }
            else
                return 0;
        }
        return digits ? value : 0;
    }

    // yyyymmdd-hhmmss -> "yyyy-mm-dd hh:mm" ("" when malformed).
    inline std::string FormatStamp(const std::string& stamp)
    {
        if (stamp.size() != 15 || stamp[8] != '-')
            return std::string();
        return stamp.substr(0, 4) + "-" + stamp.substr(4, 2) + "-" + stamp.substr(6, 2) + " " + stamp.substr(9, 2) + ":" +
            stamp.substr(11, 2);
    }

    struct Text
    {
        std::string title;
        std::string body;
    };

    // The notice. `keys`: the [Keys] table its report and mark keys are read
    // from (an unbound one is named in words); `reportsFolder`: where the ZIP
    // goes; `includeOpenClip`: ReportIncludeClips=1.
    inline Text NoticeText(const Decision& decision, const replay::keys::Table& keys, const std::string& reportsFolder, bool includeOpenClip)
    {
        using replay::keys::Action;
        Text out;
        out.title = TR("crash.title", "Redemption Editor crashed last time");
        const std::string when = FormatStamp(decision.stamp);
        std::string whenText;
        if (!when.empty())
            whenText = decision.crashes > 1 ? TRF("crash.when_many", " ({count} crashes, the last on {when})",
                                                  {{"count", std::to_string(decision.crashes)}, {"when", when}})
                                            : TRF("crash.when", " ({when})", {{"when", when}});
        out.body = TRF("crash.body", "The game closed with an error while the editor was running{when}. Please press {key_report} to save a report "
            "ZIP for us. It is saved in {folder}. It holds the editor's log, the crash files and your PC's details. Clips go in only when you "
            "marked them with {key_mark}{clips} Attach the ZIP to a crash report on the project's GitHub page.",
            {{"when", whenText},
                {"key_report", keys.Bound(Action::Report) ? replay::keys::Keys(keys, Action::Report) : TR("crash.report_key", "the Report key")},
                {"folder", reportsFolder},
                {"key_mark", keys.Bound(Action::MarkProblem) ? replay::keys::Keys(keys, Action::MarkProblem) : TR("crash.mark_key", "the Mark key")},
                {"clips", includeOpenClip ? TR("crash.clips_open", " during a replay, plus the clip open in the editor (ReportIncludeClips=1).")
                                          : TR("crash.clips", " during a replay (ReportIncludeClips=1 in RedemptionEditor.ini also adds the open clip).")}});
        return out;
    }
}
