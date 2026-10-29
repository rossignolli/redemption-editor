#pragma once

// A project's name and its file in ReplayProjects ("<name>.rpproj"): the pure
// rules of naming and renaming a project, shared by the core (script.cpp,
// ProjectNew and ProjectRename) and the editor's name screen, which shows the
// file a typed name gives and refuses a taken one before it sends it.
//
// A name holds letters, digits, spaces, - and _ (replay_project.h ValidName),
// at most kMaxNameBytes, so it is always a valid Windows file name.

#include <cstddef>
#include <string>
#include <vector>

namespace replay::project
{
    // A project name: what the file is called too (ReplayProjects\<name>.rpproj).
    constexpr std::size_t kMaxNameBytes = 48;
    constexpr const char* kExtension = ".rpproj";

    // A Windows device name, whatever its case: CON, PRN, AUX, NUL, COM1-9,
    // LPT1-9 (no file can be called that). ValidName refuses one.
    inline bool DeviceName(const std::string& name)
    {
        std::string upper;
        for (const char c : name)
            upper += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
        if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL")
            return true;
        return upper.size() == 4 && (upper.compare(0, 3, "COM") == 0 || upper.compare(0, 3, "LPT") == 0) && upper[3] >= '1' &&
            upper[3] <= '9';
    }

    // A typed name made a valid one: every character a name cannot hold (a
    // file name's \ / : * ? " < > |, other punctuation, a control or a
    // non-ASCII byte) becomes a space, runs of spaces become one, the ends are
    // trimmed, it is cut to kMaxNameBytes, and a device name gets a "_" after
    // it. Empty when nothing is left.
    inline std::string CleanName(const std::string& typed)
    {
        std::string name;
        for (const char c : typed)
        {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
            if (ok)
                name += c;
            else if (!name.empty() && name.back() != ' ')
                name += ' ';
        }
        if (name.size() > kMaxNameBytes)
            name.resize(kMaxNameBytes);
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        if (DeviceName(name))
            name += '_';
        return name;
    }

    // The project file of a name.
    inline std::string NameFile(const std::string& name) { return name + kExtension; }

    // Two file names that are one file on Windows (ASCII case ignored).
    inline bool SameFile(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size())
            return false;
        const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
        for (std::size_t n = 0; n < a.size(); ++n)
            if (lower(a[n]) != lower(b[n]))
                return false;
        return true;
    }

    // Renaming the project file `fromFile` to `name` (already cleaned), given
    // the files of ReplayProjects.
    enum class RenameCheck
    {
        Ok,       // a new file: written first, the old one removed after
        Empty,    // no name left
        Same,     // that file already: nothing to do
        CaseOnly, // the same file with other capitals: saved in place, then its capitals changed
        Exists    // another project has that file: refused, never overwritten
    };

    inline RenameCheck CheckRename(const std::string& fromFile, const std::string& name, const std::vector<std::string>& files)
    {
        if (name.empty())
            return RenameCheck::Empty;
        const std::string to = NameFile(name);
        if (to == fromFile)
            return RenameCheck::Same;
        if (SameFile(to, fromFile))
            return RenameCheck::CaseOnly;
        for (const std::string& file : files)
            if (!SameFile(file, fromFile) && SameFile(file, to))
                return RenameCheck::Exists;
        return RenameCheck::Ok;
    }
}
