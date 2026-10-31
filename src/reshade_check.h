#pragma once

// Why the editor add-on did not load, told on screen.
//
// The editor (F2) is RedemptionEditor.addon64, which ReShade loads from the
// game folder. When it is not there the mod still records and plays, and
// without this check F2 did nothing and nothing said why. Once the game is
// playable (script.cpp ServiceReShadeCheck) and again on the editor key, the
// script reads what Windows and ReShade say: is the add-on module loaded, is
// a ReShade running in this game (a module with ReShade's add-on export, or a
// ReShade.log written since the game started), what ReShade.log says about
// the add-on, and whether ReShade.ini turned it off (Home > Add-ons). Classify
// names the cause; NoticeText tells the player what to do.
//
// The standard ReShade download loads no add-on file; its log then says
// "Skipped loading add-on from '...RedemptionEditor.addon64' because this
// build of ReShade has only limited add-on functionality." (ReShade's
// addon_manager.cpp). That is the common case: the player has ReShade and
// does not know the "full add-on support" download is another one.
//
// The editor's own overlay (RedemptionEditorHost.dll, ReShade optional) also
// draws the editor: with it loaded nothing is told.
//
// Pure (no Windows header): the script gathers the Facts.

#include <string>

#include "editor_keys.h" // the editor key the notice names
#include "i18n.h"        // the notice in the player's language

namespace replay::reshadecheck
{
    // ReShade's name for the add-on (editor_loader.cpp NAME), as
    // ReShade.ini's DisabledAddons lists it.
    constexpr char kAddonName[] = "Redemption Editor";
    constexpr char kAddonFile[] = "RedemptionEditor.addon64";
    constexpr char kLogFile[] = "ReShade.log";
    constexpr char kIniFile[] = "ReShade.ini";
    // The most of ReShade.log read: the add-on lines are among its first.
    constexpr std::size_t kLogReadMax = 4u << 20;
    // The editor key's notice, at most once in this long.
    constexpr unsigned long long kKeyNoticeGapMs = 8000;

    enum class State
    {
        Loaded,        // the add-on module is in the game
        Host,          // the editor's own overlay (RedemptionEditorHost.dll) is in the game
        NoReShade,     // no ReShade runs in this game
        StandardBuild, // ReShade's log: the standard download skipped the add-on
        AddonMissing,  // RedemptionEditor.addon64 is not in the game folder
        AddonOff,      // ReShade.ini DisabledAddons names it
        NotLoaded,     // none of the above: ReShade.log says why
    };

    struct Facts
    {
        bool addonLoaded = false;      // RedemptionEditor.addon64 is a module of the game
        bool hostLoaded = false;       // RedemptionEditorHost.dll is a module of the game
        bool reshadeRunning = false;   // a ReShade is loaded, or its log was written this session
        bool addonFilePresent = false; // RedemptionEditor.addon64 is in the game folder
        std::string log;               // ReShade.log as written this session ("" when stale or missing)
        std::string ini;               // ReShade.ini ("" when missing)
    };

    inline const char* Name(State state)
    {
        switch (state)
        {
        case State::Loaded: return "loaded";
        case State::Host: return "host";
        case State::NoReShade: return "no_reshade";
        case State::StandardBuild: return "standard_build";
        case State::AddonMissing: return "addon_missing";
        case State::AddonOff: return "addon_off";
        case State::NotLoaded: return "not_loaded";
        }
        return "?";
    }

    // One line of ReShade.log skipping our add-on for the limited build.
    inline bool LogSaysStandardBuild(const std::string& log)
    {
        std::size_t start = 0;
        while (start < log.size())
        {
            std::size_t end = log.find('\n', start);
            if (end == std::string::npos)
                end = log.size();
            const std::string line = log.substr(start, end - start);
            if (line.find("Skipped loading add-on") != std::string::npos && line.find(kAddonFile) != std::string::npos &&
                line.find("limited add-on functionality") != std::string::npos)
                return true;
            start = end + 1;
        }
        return false;
    }

    inline std::string Trim(const std::string& text)
    {
        const std::size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return std::string();
        const std::size_t last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
    }

    // ReShade.ini [ADDON] DisabledAddons: a comma list of "Name" or
    // "Name@file" (ReShade's addon_manager.cpp).
    inline bool IniTurnsAddonOff(const std::string& ini)
    {
        bool inAddon = false;
        std::size_t start = 0;
        while (start < ini.size())
        {
            std::size_t end = ini.find('\n', start);
            if (end == std::string::npos)
                end = ini.size();
            const std::string line = Trim(ini.substr(start, end - start));
            start = end + 1;
            if (!line.empty() && line.front() == '[')
            {
                inAddon = line == "[ADDON]";
                continue;
            }
            const std::size_t equals = line.find('=');
            if (!inAddon || equals == std::string::npos || Trim(line.substr(0, equals)) != "DisabledAddons")
                continue;
            const std::string list = line.substr(equals + 1);
            std::size_t from = 0;
            while (from <= list.size())
            {
                std::size_t comma = list.find(',', from);
                if (comma == std::string::npos)
                    comma = list.size();
                std::string entry = Trim(list.substr(from, comma - from));
                const std::size_t at = entry.find('@');
                if (at != std::string::npos)
                    entry = Trim(entry.substr(0, at));
                if (entry == kAddonName)
                    return true;
                from = comma + 1;
            }
        }
        return false;
    }

    inline State Classify(const Facts& facts)
    {
        if (facts.addonLoaded)
            return State::Loaded;
        if (facts.hostLoaded)
            return State::Host;
        if (!facts.reshadeRunning)
            return State::NoReShade;
        if (LogSaysStandardBuild(facts.log))
            return State::StandardBuild;
        if (!facts.addonFilePresent)
            return State::AddonMissing;
        if (IniTurnsAddonOff(facts.ini))
            return State::AddonOff;
        return State::NotLoaded;
    }

    // "" for Loaded and Host.
    inline std::string NoticeText(State state, const replay::keys::Table& keys)
    {
        switch (state)
        {
        case State::Loaded:
        case State::Host:
            return std::string();
        case State::NoReShade:
            return replay::keys::Fill(TR("reshade.no_reshade", "Redemption Editor: the editor ({key_editor}) needs ReShade with full add-on support, "
                "and no ReShade runs in this game. Install it from reshade.me (the \"full add-on support\" download). Recording and playback work without it."),
                keys);
        case State::StandardBuild:
            return replay::keys::Fill(TR("reshade.standard_build", "Redemption Editor: the editor ({key_editor}) did not load. Your ReShade is the "
                "standard download, which loads no add-ons: reinstall ReShade from reshade.me with the \"full add-on support\" download. "
                "Your presets and shaders stay."),
                keys);
        case State::AddonMissing:
            return replay::keys::Fill(TR("reshade.addon_missing", "Redemption Editor: the editor ({key_editor}) did not load: RedemptionEditor.addon64 "
                "is not in the game folder. Copy it from the mod's ZIP next to RDR2.exe."),
                keys);
        case State::AddonOff:
            return replay::keys::Fill(TR("reshade.addon_off", "Redemption Editor: the editor ({key_editor}) is turned off in ReShade. Open the ReShade "
                "menu, tick Redemption Editor under Add-ons and restart the game."),
                keys);
        case State::NotLoaded:
            return replay::keys::Fill(TR("reshade.not_loaded", "Redemption Editor: ReShade did not load the editor ({key_editor}). ReShade.log in the "
                "game folder says why."),
                keys);
        }
        return std::string();
    }
}
