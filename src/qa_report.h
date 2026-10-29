#pragma once

// The report a player saves with one key (Shift+F1) in the game folder:
//
//   ReplayReports\report-YYYYMMDD-HHMMSS\
//     RedemptionEditor.log, RedemptionEditor.crash.log (when there is one),
//     ScriptHookRDR2.log, RedemptionEditor.ini (copies)
//     report.txt (the build, the time, the last clip's name: not the clip)
//
// The copies run on a thread (script.cpp, StartQaReport); this header has the
// pure parts. The report also takes the problem marks (Shift+F2, qa_marks.h),
// the clips they name, the crash dumps, the system info and ReShade.log, and
// is zipped into ReplayReports\report-YYYYMMDD-HHMMSS-<clip>.zip
// (src/qa_bundle.cpp; the folder is kept when the zip fails).
//
// Pure: unit tested in tests/mod_hygiene_tests.cpp.

#include <cstdio>
#include <string>
#include <vector>

namespace replay::qareport
{
    inline constexpr const char* kFolder = "ReplayReports";

    // Files of the game folder copied into the report; a missing one is
    // listed in report.txt as missing (the crash log only exists after a
    // fault).
    inline constexpr const char* kFiles[] = {"RedemptionEditor.log", "RedemptionEditor.crash.log", "ScriptHookRDR2.log", "RedemptionEditor.ini",
        "ReShade.log"}; // the add-on's load and its errors

    // "report-20261004-031502" (local time).
    inline std::string FolderName(unsigned year, unsigned month, unsigned day, unsigned hour, unsigned minute, unsigned second)
    {
        char text[40]{};
        std::snprintf(text, sizeof(text), "report-%04u%02u%02u-%02u%02u%02u", year % 10000u, month % 100u, day % 100u,
            hour % 100u, minute % 100u, second % 100u);
        return text;
    }

    struct Summary
    {
        std::string version;
        std::string asiSha256;
        std::string time;        // "YYYY-MM-DD hh:mm:ss"
        std::string lastClip;    // the last clip saved this session ("" none)
        std::string openClip;    // the clip open in the editor or playback ("" none)
        std::string state;       // "idle", "recording (buffer)", "playing"...
        std::string iniKeys;     // the [RedemptionEditor] section as read at load
        std::string marks;       // "3 (mark-...;mark-...)", "" none
        bool includeOpenClip = false; // ReportIncludeClips=1
    };

    // The clips a report takes. A player's report carries only the clips he marked
    // (Shift+F2 names the clip that plays), in mark order; the open clip joins them
    // only when he asks for it (ReportIncludeClips=1 in the ini).
    inline std::vector<std::string> ReportClips(const std::vector<std::string>& marked, const std::string& openClip,
        bool includeOpenClip)
    {
        std::vector<std::string> clips;
        for (const std::string& clip : marked)
        {
            bool seen = clip.empty();
            for (const std::string& kept : clips)
                seen = seen || kept == clip;
            if (!seen)
                clips.push_back(clip);
        }
        if (includeOpenClip && !openClip.empty())
        {
            bool seen = false;
            for (const std::string& kept : clips)
                seen = seen || kept == openClip;
            if (!seen)
                clips.push_back(openClip);
        }
        return clips;
    }

    // The "marks:" value from the mark folders the report actually took (the ones
    // on disk, this session's or an earlier one's: after a crash the session's own
    // list is empty).
    inline std::string MarksText(const std::vector<std::string>& folders)
    {
        if (folders.empty())
            return {};
        std::string joined;
        for (const std::string& folder : folders)
            joined += (joined.empty() ? "" : ";") + folder;
        return std::to_string(folders.size()) + " (" + joined + ")";
    }

    // The report.txt text with its "marks:" line replaced (added before the
    // files listing when absent).
    inline std::string ReplaceMarksLine(const std::string& text, const std::string& marks)
    {
        const std::string line = "marks: " + (marks.empty() ? std::string("none") : marks);
        // Only a line that starts with the key ("remarks: 5" is not it).
        for (std::size_t at = text.find("marks: "); at != std::string::npos; at = text.find("marks: ", at + 1))
        {
            if (at != 0 && text[at - 1] != '\n')
                continue;
            std::size_t end = text.find('\n', at);
            if (end == std::string::npos)
                end = text.size();
            else if (end > at && text[end - 1] == '\r')
                --end;
            return text.substr(0, at) + line + text.substr(end);
        }
        return text + line + "\r\n";
    }

    // The clips a mark or the open clip names that are not in ReplayClips any more
    // (deleted before the report). The "missing_clips:" line, placed like the
    // marks line; nothing when every clip was there.
    inline std::string AddMissingClipsLine(const std::string& text, const std::vector<std::string>& missing)
    {
        if (missing.empty())
            return text;
        std::string joined;
        for (const std::string& clip : missing)
            joined += (joined.empty() ? "" : ";") + clip;
        const std::string line = "missing_clips: " + std::to_string(missing.size()) + " (" + joined +
            "; named by a mark or open, not in ReplayClips when the report was made)";
        for (std::size_t at = text.find("marks: "); at != std::string::npos; at = text.find("marks: ", at + 1))
        {
            if (at != 0 && text[at - 1] != '\n')
                continue;
            const std::size_t end = text.find('\n', at);
            if (end == std::string::npos)
                return text + "\r\n" + line + "\r\n";
            return text.substr(0, end + 1) + line + "\r\n" + text.substr(end + 1);
        }
        return text + line + "\r\n";
    }

    // report.txt: one "key: value" per line, English (read by the team).
    inline std::string SummaryText(const Summary& summary)
    {
        const auto value = [](const std::string& text) { return text.empty() ? std::string("none") : text; };
        return "Redemption Editor QA report\r\n"
               "version: " + value(summary.version) + "\r\n"
               "asi_sha256: " + value(summary.asiSha256) + "\r\n"
               "time: " + value(summary.time) + "\r\n"
               "state: " + value(summary.state) + "\r\n"
               "last_clip: " + value(summary.lastClip) + "\r\n"
               "open_clip: " + value(summary.openClip) + "\r\n"
               "ini: " + value(summary.iniKeys) + "\r\n"
               "marks: " + value(summary.marks) + "\r\n"
               "clips: " + (summary.includeOpenClip ? "marked and open (ReportIncludeClips=1)" : "marked only") + "\r\n" +
               (summary.includeOpenClip ? "The clips named by the marks and the open clip are in clips\\ (compressed when they were not).\r\n"
                                        : "The clips named by the marks are in clips\\ (compressed when they were not).\r\n");
    }
}
