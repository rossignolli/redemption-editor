#pragma once

// Remappable keys. One table of actions, read from the [Keys] section of
// RedemptionEditor.ini by both sides: the core (script.cpp, OnKeyboardMessage,
// the ScriptHook keyboard) and the add-on (the editor, ReShade's keyboard).
// Both build the table with the same pure Load from the same file, so a key
// remapped works with the editor open and closed, and the key bar, the F1
// help and every text that names a key (Fill: {key_report}) print the key
// the table holds.
//
// RDR2 has no replay actions in the game's control system (a replay editor
// usually binds INPUT_REPLAY_* actions there): the table is ours.
//
// Each action takes up to two keys, each a virtual key with Ctrl and/or
// Shift. Keys the game, an overlay or the editor itself already use are
// refused with a reason (log line KEYS and one notice in game); a key given
// to two actions stays with the first one the ini names. An action whose ini
// value is refused entirely keeps its default.
//
// Pure (no Windows header, no ImGui): unit tested in ReplayCoreTests
// (tests/editor_keys_tests.cpp). The ini reader is editor_keys_ini.h.

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

#include "i18n.h" // the notice's words in the player's language

namespace replay::keys
{
    enum class Action : std::uint8_t
    {
        PlayPause,
        FrameBack,    // tap: one frame; held: rewind (the core scrubs while it is down)
        FrameForward, // tap: one frame; held: fast forward
        Rewind,       // a key of its own for the held scrub (none by default: FrameBack does it)
        FastForward,
        SpeedDown,
        SpeedUp,
        FreeCam,
        HideUi,
        Editor,
        Stop, // plays the last clip, stops the one playing
        Record,
        SaveBuffer,
        MarkProblem,
        Report,
        // The free camera's modes and its target, and the markers drawn in the
        // world. The free camera ones act only while it flies (the editor's UI
        // hidden or closed).
        FreeCamDrone,
        FreeCamAcrobatic,
        FreeCamWalk,
        FreeCamCollision,
        FreeCamLockAltitude,
        FreeCamFollow,
        FreeCamPick,
        FreeCamPickNearest,
        FreeCamLevel,
        FreeCamBehind,
        FreeCamLock,
        CameraOverlay,
        Count
    };
    constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::Count);
    constexpr std::size_t kSlots = 2; // keys per action

    // Virtual key codes (winuser.h values, kept here so the header stays pure).
    namespace vk
    {
        constexpr std::uint8_t Back = 0x08, Tab = 0x09, Return = 0x0D, Shift = 0x10, Control = 0x11, Menu = 0x12,
                               Pause = 0x13, Escape = 0x1B, Space = 0x20, Prior = 0x21, Next = 0x22, End = 0x23,
                               Home = 0x24, Left = 0x25, Up = 0x26, Right = 0x27, Down = 0x28, Insert = 0x2D,
                               Delete = 0x2E, Numpad0 = 0x60, Multiply = 0x6A, Add = 0x6B, Subtract = 0x6D,
                               Decimal = 0x6E, Divide = 0x6F, F1 = 0x70, F24 = 0x87, Semicolon = 0xBA, Plus = 0xBB,
                               Comma = 0xBC, Minus = 0xBD, Period = 0xBE, Slash = 0xBF, Backquote = 0xC0,
                               LeftBracket = 0xDB, Backslash = 0xDC, RightBracket = 0xDD, Quote = 0xDE;
        constexpr std::uint8_t F(int n) { return static_cast<std::uint8_t>(F1 + n - 1); }
    }

    struct Binding
    {
        std::uint8_t vk = 0;
        bool ctrl = false;
        bool shift = false;

        bool Valid() const { return vk != 0; }
        bool operator==(const Binding& other) const
        {
            return vk == other.vk && ctrl == other.ctrl && shift == other.shift;
        }
        bool operator!=(const Binding& other) const { return !(*this == other); }
    };

    struct ActionInfo
    {
        Action action;
        const char* iniKey;   // the [Keys] key
        const char* defaults; // today's keys (empty: none of its own)
        const char* help;     // the F1 help's text
        const char* text;     // a text names its keys {key_<text>} (Fill)
    };

    // Defaults.
    inline constexpr ActionInfo kActions[kActionCount] = {
        {Action::PlayPause, "PlayPause", "K", "Play, pause", "play_pause"},
        {Action::FrameBack, "FrameBack", "Comma", "One frame back (hold: rewind)", "frame_back"},
        {Action::FrameForward, "FrameForward", "Period", "One frame forward (hold: fast forward)", "frame_forward"},
        {Action::Rewind, "Rewind", "", "Rewind while held", "rewind"},
        {Action::FastForward, "FastForward", "", "Fast forward while held", "fast_forward"},
        {Action::SpeedDown, "SpeedDown", "LeftBracket", "Slower playback", "speed_down"},
        {Action::SpeedUp, "SpeedUp", "RightBracket", "Faster playback", "speed_up"},
        {Action::FreeCam, "FreeCam", "F11", "Free camera", "free_cam"},
        {Action::HideUi, "HideUi", "H", "Hide the replay UI (the keys keep working)", "hide"},
        {Action::Editor, "Editor", "F2", "Open, close the editor", "editor"},
        {Action::Stop, "Stop", "F7", "Play / stop the last clip", "stop"},
        {Action::Record, "Record", "F4", "Record / mark a recording", "record"},
        {Action::SaveBuffer, "SaveBuffer", "Shift+F4", "Save the replay buffer", "save_buffer"},
        {Action::MarkProblem, "MarkProblem", "Shift+F2", "Mark a problem (QA report)", "mark"},
        {Action::Report, "Report", "Shift+F1", "Save the QA report (one zip, every mark and its clips)", "report"},
        {Action::FreeCamDrone, "FreeCamDrone", "V", "Free camera: Drone flight (momentum, banking) on/off", "drone"},
        {Action::FreeCamAcrobatic, "FreeCamAcrobatic", "N", "Free camera: acrobatic rotation (full rolls and loops) on/off", "acrobatic"},
        {Action::FreeCamWalk, "FreeCamWalk", "B", "Free camera: Walk (the ground at eye height) on/off", "walk"},
        {Action::FreeCamCollision, "FreeCamCollision", "Y", "Free camera: collision with the world on/off", "collision"},
        {Action::FreeCamLockAltitude, "FreeCamLockAltitude", "U", "Free camera: keep the altitude on/off", "altitude"},
        {Action::FreeCamFollow, "FreeCamFollow", "F", "Free camera: follow the target (rigid, position only, off)", "follow"},
        {Action::FreeCamPick, "FreeCamPick", "G", "Free camera: the target under the crosshair", "pick"},
        {Action::FreeCamPickNearest, "FreeCamPickNearest", "Shift+G", "Free camera: the nearest target", "pick_nearest"},
        {Action::FreeCamLevel, "FreeCamLevel", "Num5", "Free camera: level the horizon", "level"},
        {Action::FreeCamBehind, "FreeCamBehind", "Num0", "Free camera: behind the main character", "behind"},
        {Action::FreeCamLock, "FreeCamLock", "L", "Free camera: hold still on/off", "hold"},
        {Action::CameraOverlay, "CameraOverlay", "Backquote", "Camera markers and path in the world on/off", "overlay"},
    };

    inline const ActionInfo& Info(Action action) { return kActions[static_cast<std::size_t>(action)]; }

    struct Table
    {
        Binding keys[kActionCount][kSlots]{};

        const Binding* Of(Action action) const { return keys[static_cast<std::size_t>(action)]; }
        Binding* Of(Action action) { return keys[static_cast<std::size_t>(action)]; }
        bool Bound(Action action) const { return Of(action)[0].Valid() || Of(action)[1].Valid(); }
    };

    // ---------------- Names ----------------

    namespace detail
    {
        struct Name
        {
            const char* name;
            std::uint8_t vk;
            const char* label; // what the key bar shows
        };

        inline constexpr Name kNames[] = {
            {"Comma", vk::Comma, ","}, {"Period", vk::Period, "."}, {"Dot", vk::Period, "."},
            {"LeftBracket", vk::LeftBracket, "["}, {"RightBracket", vk::RightBracket, "]"},
            {"Semicolon", vk::Semicolon, ";"}, {"Slash", vk::Slash, "/"}, {"Backslash", vk::Backslash, "\\"},
            {"Quote", vk::Quote, "'"}, {"Minus", vk::Minus, "-"}, {"Equals", vk::Plus, "="}, {"Plus", vk::Plus, "="},
            {"Backquote", vk::Backquote, "`"}, {"Tilde", vk::Backquote, "`"},
            {"Left", vk::Left, "Left"}, {"Right", vk::Right, "Right"}, {"Up", vk::Up, "Up"}, {"Down", vk::Down, "Down"},
            {"Space", vk::Space, "Space"}, {"Insert", vk::Insert, "Ins"}, {"Delete", vk::Delete, "Del"},
            {"Del", vk::Delete, "Del"}, {"Home", vk::Home, "Home"}, {"End", vk::End, "End"},
            {"PageUp", vk::Prior, "PgUp"}, {"PgUp", vk::Prior, "PgUp"}, {"PageDown", vk::Next, "PgDn"},
            {"PgDn", vk::Next, "PgDn"}, {"Tab", vk::Tab, "Tab"}, {"Enter", vk::Return, "Enter"},
            {"Esc", vk::Escape, "Esc"}, {"Escape", vk::Escape, "Esc"}, {"Backspace", vk::Back, "Backspace"},
            {"Pause", vk::Pause, "Pause"}, {"NumAdd", vk::Add, "Num +"}, {"NumPlus", vk::Add, "Num +"},
            {"NumSubtract", vk::Subtract, "Num -"}, {"NumMinus", vk::Subtract, "Num -"},
            {"NumMultiply", vk::Multiply, "Num *"}, {"NumDivide", vk::Divide, "Num /"},
            {"NumDecimal", vk::Decimal, "Num ."},
        };

        // Single characters accepted as names (',' only through Parse: in
        // an ini value it separates the two keys, so the ini says Comma).
        inline constexpr Name kChars[] = {
            {",", vk::Comma, ","}, {".", vk::Period, "."}, {"[", vk::LeftBracket, "["}, {"]", vk::RightBracket, "]"},
            {";", vk::Semicolon, ";"}, {"/", vk::Slash, "/"}, {"\\", vk::Backslash, "\\"}, {"'", vk::Quote, "'"},
            {"-", vk::Minus, "-"}, {"=", vk::Plus, "="}, {"`", vk::Backquote, "`"},
        };

        inline bool SameText(const std::string& a, const char* b)
        {
            std::size_t n = 0;
            for (; n < a.size() && b[n]; ++n)
            {
                if (std::tolower(static_cast<unsigned char>(a[n])) != std::tolower(static_cast<unsigned char>(b[n])))
                    return false;
            }
            return n == a.size() && b[n] == '\0';
        }

        inline std::string Trim(const std::string& text)
        {
            std::size_t begin = 0, end = text.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
                ++begin;
            while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
                --end;
            return text.substr(begin, end - begin);
        }

        // The key part of a token (no modifiers): 0 when unknown.
        inline std::uint8_t KeyOf(const std::string& name)
        {
            if (name.size() == 1)
            {
                const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                    return static_cast<std::uint8_t>(c);
                for (const Name& entry : kChars)
                {
                    if (name == entry.name)
                        return entry.vk;
                }
                return 0;
            }
            if ((name[0] == 'F' || name[0] == 'f') && name.size() <= 3)
            {
                int n = 0;
                for (std::size_t i = 1; i < name.size(); ++i)
                {
                    if (!std::isdigit(static_cast<unsigned char>(name[i])))
                        return 0;
                    n = n * 10 + (name[i] - '0');
                }
                return n >= 1 && n <= 24 ? vk::F(n) : 0;
            }
            if (name.size() == 4 && (SameText(name.substr(0, 3), "Num")) && std::isdigit(static_cast<unsigned char>(name[3])))
                return static_cast<std::uint8_t>(vk::Numpad0 + (name[3] - '0'));
            for (const Name& entry : kNames)
            {
                if (SameText(name, entry.name))
                    return entry.vk;
            }
            return 0;
        }
    }

    enum class ParseError : std::uint8_t
    {
        None,
        Empty,
        UnknownKey,
        Alt,          // Alt+ combinations are refused (Alt+F4, Alt+Tab)
        ModifierOnly, // "Ctrl" alone
    };

    inline const char* ErrorName(ParseError error)
    {
        switch (error)
        {
        case ParseError::Empty: return "empty";
        case ParseError::UnknownKey: return "unknown_key";
        case ParseError::Alt: return "alt_modifier";
        case ParseError::ModifierOnly: return "modifier_only";
        default: return "none";
        }
    }

    // "Ctrl+Left", "Shift+F4", "Comma", "k", "Num4", "[".
    inline ParseError Parse(const std::string& text, Binding& out)
    {
        out = {};
        std::string rest = detail::Trim(text);
        if (rest.empty())
            return ParseError::Empty;
        Binding binding;
        while (true)
        {
            // A '+' that is the key itself ("Ctrl++") is not a separator.
            const std::size_t plus = rest.size() > 1 ? rest.find('+') : std::string::npos;
            if (plus == std::string::npos || plus == 0)
                break;
            const std::string modifier = detail::Trim(rest.substr(0, plus));
            if (detail::SameText(modifier, "Ctrl") || detail::SameText(modifier, "Control"))
                binding.ctrl = true;
            else if (detail::SameText(modifier, "Shift"))
                binding.shift = true;
            else if (detail::SameText(modifier, "Alt"))
                return ParseError::Alt;
            else
                break; // not a modifier: the whole rest is the key name
            rest = detail::Trim(rest.substr(plus + 1));
            if (rest.empty())
                return ParseError::ModifierOnly;
        }
        if (detail::SameText(rest, "Ctrl") || detail::SameText(rest, "Control") || detail::SameText(rest, "Shift"))
            return ParseError::ModifierOnly;
        if (detail::SameText(rest, "Alt"))
            return ParseError::Alt;
        if (rest == "+")
            binding.vk = vk::Plus;
        else
            binding.vk = detail::KeyOf(rest);
        if (!binding.vk)
            return ParseError::UnknownKey;
        out = binding;
        return ParseError::None;
    }

    // The key cap's text: "Ctrl+Left", ",", "Shift+F4", "Num 4".
    inline std::string Label(const Binding& binding)
    {
        if (!binding.Valid())
            return std::string();
        std::string text;
        if (binding.ctrl)
            text += "Ctrl+";
        if (binding.shift)
            text += "Shift+";
        const std::uint8_t key = binding.vk;
        if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
            text += static_cast<char>(key);
        else if (key >= vk::F1 && key <= vk::F24)
            text += "F" + std::to_string(key - vk::F1 + 1);
        else if (key >= vk::Numpad0 && key <= vk::Numpad0 + 9)
            text += "Num " + std::to_string(key - vk::Numpad0);
        else
        {
            const char* name = nullptr;
            for (const detail::Name& entry : detail::kNames)
            {
                if (entry.vk == key)
                {
                    name = entry.label;
                    break;
                }
            }
            text += name ? name : "?";
        }
        return text;
    }

    // The ini's spelling of a key: "Comma", "Shift+F4", "Num4" (log lines,
    // where a "," of its own would split the field).
    inline std::string IniName(const Binding& binding)
    {
        if (!binding.Valid())
            return std::string();
        std::string text;
        if (binding.ctrl)
            text += "Ctrl+";
        if (binding.shift)
            text += "Shift+";
        const std::uint8_t key = binding.vk;
        if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
            text += static_cast<char>(key);
        else if (key >= vk::F1 && key <= vk::F24)
            text += "F" + std::to_string(key - vk::F1 + 1);
        else if (key >= vk::Numpad0 && key <= vk::Numpad0 + 9)
            text += "Num" + std::to_string(key - vk::Numpad0);
        else
        {
            const char* name = "?";
            for (const detail::Name& entry : detail::kNames)
            {
                if (entry.vk == key)
                {
                    name = entry.name;
                    break;
                }
            }
            text += name;
        }
        return text;
    }

    // ---------------- Reserved keys ----------------

    struct Reserved
    {
        std::uint8_t vk;
        bool anyModifier; // refused with or without Ctrl/Shift
        bool ctrl;        // otherwise: these exact modifiers
        bool shift;
        const char* reason; // log word
        const char* whyKey; // the notice's words: their lang key and English
        const char* why;
    };

    // What the game, the overlays and the mod's fixed keys already use:
    // Shift+Tab opens the Steam overlay, Home the Social Club (and ReShade by
    // default), F6 the game's Photo Mode with any modifier, F12 the Steam
    // screenshot, Ctrl+R reloads ScriptHook, F3 F5 F10 are common keys of
    // other mods.
    inline constexpr Reserved kReserved[] = {
        {vk::Tab, true, false, false, "steam_overlay_or_markers", TR_NOOP("keys.reserved.tab_moves_between_markers_and", "Tab moves between markers and Shift+Tab opens the Steam overlay")},
        {vk::Home, true, false, false, "social_club", TR_NOOP("keys.reserved.home_opens_the_social_club", "Home opens the Social Club overlay and ReShade")},
        {vk::F(6), true, false, false, "photo_mode", TR_NOOP("keys.reserved.f6_opens_the_game_s", "F6 opens the game's Photo Mode")},
        {vk::F(12), true, false, false, "steam_screenshot", TR_NOOP("keys.reserved.f12_is_the_steam_screenshot", "F12 is the Steam screenshot")},
        {vk::F(1), false, false, false, "help", TR_NOOP("keys.reserved.f1_is_the_editor_s", "F1 is the editor's key help")},
        {vk::F(3), true, false, false, "other_mod", TR_NOOP("keys.reserved.f3_belongs_to_another_mod", "F3 belongs to another mod")},
        {vk::F(5), true, false, false, "other_mod", TR_NOOP("keys.reserved.f5_belongs_to_another_mod", "F5 belongs to another mod")},
        {vk::F(10), true, false, false, "other_mod", TR_NOOP("keys.reserved.f10_belongs_to_another_mod", "F10 belongs to another mod")},
        {'R', false, true, false, "scripthook_reload", TR_NOOP("keys.reserved.ctrl_r_reloads_scripthook", "Ctrl+R reloads ScriptHook")},
        {vk::Escape, true, false, false, "editor_fixed", TR_NOOP("keys.reserved.esc_is_the_editor_s", "Esc is the editor's Back")},
        {vk::Return, true, false, false, "editor_fixed", TR_NOOP("keys.reserved.enter_is_the_editor_s", "Enter is the editor's Select")},
        {vk::Back, true, false, false, "editor_fixed", TR_NOOP("keys.reserved.backspace_is_the_editor_s", "Backspace is the editor's Back")},
        {vk::Space, true, false, false, "editor_fixed", TR_NOOP("keys.reserved.space_is_the_editor_s", "Space is the editor's play key")},
        {vk::Left, true, false, false, "editor_fixed", TR_NOOP("keys.reserved.the_arrows_move_in_the", "the arrows move in the editor and turn the free camera")},
        {vk::Right, true, false, false, "editor_fixed", TR_NOOP("keys.reserved.the_arrows_move_in_the", "the arrows move in the editor and turn the free camera")},
        {vk::Up, true, false, false, "editor_fixed", TR_NOOP("keys.reserved.the_arrows_move_in_the", "the arrows move in the editor and turn the free camera")},
        {vk::Down, true, false, false, "editor_fixed", TR_NOOP("keys.reserved.the_arrows_move_in_the", "the arrows move in the editor and turn the free camera")},
        {vk::Prior, true, false, false, "free_camera", TR_NOOP("keys.reserved.pageup_zooms_the_free_camera", "PageUp zooms the free camera")},
        {vk::Next, true, false, false, "free_camera", TR_NOOP("keys.reserved.pagedown_zooms_the_free_camera", "PageDown zooms the free camera")},
        {'W', false, false, false, "free_camera", TR_NOOP("keys.reserved.w_moves_the_free_camera", "W moves the free camera")},
        {'A', false, false, false, "free_camera", TR_NOOP("keys.reserved.a_moves_the_free_camera", "A moves the free camera and adds a clip")},
        {'S', false, false, false, "free_camera", TR_NOOP("keys.reserved.s_moves_the_free_camera", "S moves the free camera")},
        {'D', false, false, false, "free_camera", TR_NOOP("keys.reserved.d_moves_the_free_camera", "D moves the free camera and duplicates a clip")},
        {'Q', false, false, false, "free_camera", TR_NOOP("keys.reserved.q_moves_the_free_camera", "Q moves the free camera")},
        {'E', false, false, false, "free_camera", TR_NOOP("keys.reserved.e_moves_the_free_camera", "E moves the free camera and opens the editor")},
        {'Z', false, false, false, "free_camera", TR_NOOP("keys.reserved.z_rolls_the_free_camera", "Z rolls the free camera")},
        {'C', false, false, false, "free_camera", TR_NOOP("keys.reserved.c_rolls_the_free_camera", "C rolls the free camera")},
        {'T', false, false, false, "editor_fixed", TR_NOOP("keys.reserved.t_takes_the_clip_thumbnail", "T takes the clip thumbnail")},
        {'P', false, false, false, "editor_fixed", TR_NOOP("keys.reserved.p_takes_a_photo", "P takes a photo")},
        {'X', false, false, false, "editor_fixed", TR_NOOP("keys.reserved.x_cuts_a_project_clip", "X cuts a project clip")},
        {'I', false, false, false, "editor_fixed", TR_NOOP("keys.reserved.i_trims_a_project_clip", "I trims a project clip")},
        {'O', false, false, false, "editor_fixed", TR_NOOP("keys.reserved.o_trims_a_project_clip", "O trims a project clip")},
        {'R', false, false, false, "editor_fixed", TR_NOOP("keys.reserved.r_renames_a_clip", "R renames a clip")},
        {'S', false, true, false, "editor_fixed", TR_NOOP("keys.reserved.ctrl_s_saves", "Ctrl+S saves")},
        {'Z', false, true, false, "editor_fixed", TR_NOOP("keys.reserved.ctrl_z_undoes", "Ctrl+Z undoes")},
        {'Y', false, true, false, "editor_fixed", TR_NOOP("keys.reserved.ctrl_y_redoes", "Ctrl+Y redoes")},
        {'E', false, true, false, "editor_fixed", TR_NOOP("keys.reserved.ctrl_e_exports", "Ctrl+E exports")},
        {'C', false, true, false, "editor_fixed", TR_NOOP("keys.reserved.ctrl_c_copies_a_marker", "Ctrl+C copies a marker")},
        {'V', false, true, false, "editor_fixed", TR_NOOP("keys.reserved.ctrl_v_pastes_a_marker", "Ctrl+V pastes a marker")},
        {vk::Insert, true, false, false, "core_fixed", TR_NOOP("keys.reserved.insert_adds_a_camera_key", "Insert adds a camera key")},
        {vk::Delete, true, false, false, "core_fixed", TR_NOOP("keys.reserved.delete_removes_a_camera_key", "Delete removes a camera key or a marker")},
        {vk::End, true, false, false, "core_fixed", TR_NOOP("keys.reserved.end_turns_the_camera_track", "End turns the camera track on and off")},
#if defined(REDEMPTION_DEV)
        // The debug HUD's keys (Debug=1): the developer build only, so the
        // public build neither reserves them nor says they do something.
        {vk::F(8), true, false, false, "core_fixed", TR_NOOP("keys.reserved.f8_loads_the_next_clip", "F8 loads the next clip")},
        {vk::F(9), true, false, false, "core_fixed", TR_NOOP("keys.reserved.f9_is_the_debug_overlay", "F9 is the debug overlay")},
#endif
    };

    // The reserved entry the binding hits, or nullptr.
    inline const Reserved* ReservedFor(const Binding& binding)
    {
        for (const Reserved& entry : kReserved)
        {
            if (entry.vk != binding.vk)
                continue;
            if (entry.anyModifier || (entry.ctrl == binding.ctrl && entry.shift == binding.shift))
                return &entry;
        }
        return nullptr;
    }

    // ---------------- Fixed keys ----------------

    // The keys no [Keys] entry moves: the editor's own (Back, Select, Save...)
    // and the free camera's flight (kReserved says why each one is taken and
    // refuses it to an action). A text names one by {key_<name>} like an
    // action's key (Fill), and the key bar takes it from here, so moving one
    // is a change here and in its handler, not in every language.
    namespace fixed
    {
        inline constexpr char Help[] = "F1", Back[] = "Esc", Select[] = "Enter", Cancel[] = "Backspace", Play[] = "Space",
                              Save[] = "Ctrl+S", Export[] = "Ctrl+E", Copy[] = "Ctrl+C", Paste[] = "Ctrl+V", NextMarker[] = "Tab",
                              Delete[] = "Delete", PageUp[] = "PageUp", PageDown[] = "PageDown", Fly[] = "WASD", FlyDown[] = "Q",
                              FlyUp[] = "E", RollLeft[] = "Z", RollRight[] = "C", AddClip[] = "A", Edit[] = "E";
    }

    struct FixedKey
    {
        const char* text; // {key_<text>}
        const char* name; // as the F1 help writes it
    };

    inline constexpr FixedKey kFixed[] = {
        {"help", fixed::Help}, {"back", fixed::Back}, {"select", fixed::Select}, {"cancel", fixed::Cancel}, {"play", fixed::Play},
        {"save", fixed::Save}, {"export", fixed::Export}, {"copy", fixed::Copy}, {"paste", fixed::Paste},
        {"next_marker", fixed::NextMarker}, {"delete", fixed::Delete}, {"page_up", fixed::PageUp}, {"page_down", fixed::PageDown},
        {"fly", fixed::Fly}, {"fly_down", fixed::FlyDown}, {"fly_up", fixed::FlyUp}, {"roll_left", fixed::RollLeft},
        {"roll_right", fixed::RollRight}, {"add_clip", fixed::AddClip}, {"edit", fixed::Edit},
    };

    // ---------------- Load ----------------

    struct Issue
    {
        Action action = Action::Count;
        std::string value;  // the ini value as read
        std::string token;  // the refused key ("" for a whole-value issue)
        std::string reason; // log word: unknown_key, alt_modifier, reserved_<why>, duplicate_<Action>, too_many_keys
        std::string why;    // words for the notice (English)
        std::string whyKey;      // ... their lang key (empty: why as it is)
        std::string whyTemplate; // ... the English with {action} (the action named in whyArg)
        std::string whyArg;
        std::string kept;   // the keys the action ended with (labels, for the notice)
        std::string keptIni; // the same in the ini's spelling (for the log)
    };

    namespace detail
    {
        // An issue's words: the English (why, {action} filled in) and what the
        // notice needs to word it in the player's language.
        inline void SetWhy(Issue& issue, const char* key, const char* english, const std::string& action = std::string())
        {
            issue.whyKey = key;
            issue.whyTemplate = english;
            issue.whyArg = action;
            issue.why = replay::i18n::Format(english, {{"action", action}});
        }
    }

    struct LoadResult
    {
        Table table;
        std::vector<Issue> issues;
        std::uint32_t configured = 0; // actions the ini named
    };

    // `read(iniKey, present)` returns the [Keys] value and whether the key
    // is in the file at all. An empty value ("Rewind=") or "None" unbinds.
    inline LoadResult Load(const std::function<std::string(const char* iniKey, bool& present)>& read)
    {
        LoadResult result;
        bool configured[kActionCount]{};
        std::string values[kActionCount];
        Binding wanted[kActionCount][kSlots]{};
        std::vector<Issue> pending; // issues whose `kept` is filled at the end

        for (std::size_t index = 0; index < kActionCount; ++index)
        {
            const ActionInfo& info = kActions[index];
            bool present = false;
            std::string value = read ? read(info.iniKey, present) : std::string();
            const bool fromIni = present;
            if (!present)
                value = info.defaults;
            values[index] = value;
            const std::string trimmed = detail::Trim(value);
            if (trimmed.empty() || detail::SameText(trimmed, "None"))
            {
                configured[index] = fromIni;
                continue; // unbound
            }
            std::size_t slot = 0;
            std::size_t refused = 0;
            std::size_t at = 0;
            while (at <= value.size())
            {
                const std::size_t comma = value.find(',', at);
                const std::string token = detail::Trim(value.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
                at = comma == std::string::npos ? value.size() + 1 : comma + 1;
                if (token.empty())
                    continue;
                Issue issue;
                issue.action = info.action;
                issue.value = value;
                issue.token = token;
                Binding binding;
                const ParseError error = Parse(token, binding);
                if (error != ParseError::None)
                {
                    issue.reason = ErrorName(error);
                    if (error == ParseError::Alt)
                        detail::SetWhy(issue, TR_NOOP("keys.issue.alt", "Alt combinations are not used"));
                    else
                        detail::SetWhy(issue, TR_NOOP("keys.issue.not_key", "not a key name"));
                }
                else if (const Reserved* reserved = ReservedFor(binding))
                {
                    issue.reason = std::string("reserved_") + reserved->reason;
                    detail::SetWhy(issue, reserved->whyKey, reserved->why);
                }
                else if (slot >= kSlots)
                {
                    issue.reason = "too_many_keys";
                    detail::SetWhy(issue, TR_NOOP("keys.issue.too_many", "two keys per action at most"));
                }
                else
                {
                    bool repeated = false;
                    for (std::size_t s = 0; s < slot; ++s)
                        repeated = repeated || wanted[index][s] == binding;
                    if (!repeated)
                        wanted[index][slot++] = binding;
                    continue;
                }
                ++refused;
                pending.push_back(issue);
            }
            if (fromIni && slot == 0)
            {
                // Every key refused: the default stays (it is valid by construction).
                Issue issue;
                issue.action = info.action;
                issue.value = value;
                issue.reason = "kept_default";
                detail::SetWhy(issue, TR_NOOP("keys.issue.no_key", "no usable key"));
                pending.push_back(issue);
                const std::string defaults = info.defaults;
                std::size_t from = 0;
                while (from <= defaults.size() && slot < kSlots)
                {
                    const std::size_t comma = defaults.find(',', from);
                    const std::string token = defaults.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
                    from = comma == std::string::npos ? defaults.size() + 1 : comma + 1;
                    Binding binding;
                    if (Parse(token, binding) == ParseError::None)
                        wanted[index][slot++] = binding;
                }
                configured[index] = false; // it competes as a default
            }
            else
                configured[index] = fromIni;
            (void)refused;
        }

        // Duplicates: the actions the ini names first (table order), then the
        // defaults; a key already taken is dropped from the later action.
        std::vector<std::pair<Binding, Action>> taken;
        for (int pass = 0; pass < 2; ++pass)
        {
            for (std::size_t index = 0; index < kActionCount; ++index)
            {
                if (configured[index] != (pass == 0))
                    continue;
                std::size_t out = 0;
                for (std::size_t slot = 0; slot < kSlots; ++slot)
                {
                    const Binding binding = wanted[index][slot];
                    if (!binding.Valid())
                        continue;
                    const Action* owner = nullptr;
                    for (const auto& entry : taken)
                    {
                        if (entry.first == binding)
                            owner = &entry.second;
                    }
                    if (owner)
                    {
                        Issue issue;
                        issue.action = kActions[index].action;
                        issue.value = values[index];
                        issue.token = Label(binding);
                        issue.reason = std::string("duplicate_") + Info(*owner).iniKey;
                        detail::SetWhy(issue, TR_NOOP("keys.issue.used_by", "already used by {action}"), Info(*owner).iniKey);
                        pending.push_back(issue);
                        continue;
                    }
                    taken.push_back({binding, kActions[index].action});
                    result.table.keys[index][out++] = binding;
                }
            }
        }
        for (std::size_t index = 0; index < kActionCount; ++index)
            result.configured += configured[index] ? 1u : 0u;
        for (Issue& issue : pending)
        {
            const Binding* keys = result.table.Of(issue.action);
            for (std::size_t slot = 0; slot < kSlots; ++slot)
            {
                if (keys[slot].Valid())
                    issue.kept += (issue.kept.empty() ? "" : " / ") + Label(keys[slot]);
            }
            for (std::size_t slot = 0; slot < kSlots; ++slot)
            {
                if (keys[slot].Valid())
                    issue.keptIni += (issue.keptIni.empty() ? "" : "|") + IniName(keys[slot]);
            }
            if (issue.kept.empty())
                issue.kept = "none";
            if (issue.keptIni.empty())
                issue.keptIni = "None";
            result.issues.push_back(issue);
        }
        return result;
    }

    // The defaults alone (no ini).
    inline Table DefaultTable()
    {
        return Load([](const char*, bool& present) {
            present = false;
            return std::string();
        }).table;
    }

    // ---------------- Matching ----------------

    // Whether a press of `key` with these modifiers is `action`. Ctrl must
    // match; Shift must match too, except that a key bound without Shift
    // also answers with Shift down when no action has the Shift variant
    // (Shift is the free camera's fast key: K with Shift held still pauses;
    // Shift+F4 is SaveBuffer, not Record).
    inline bool Matches(const Table& table, Action action, unsigned key, bool ctrl, bool shift)
    {
        if (key == 0 || key > 0xFF)
            return false;
        const Binding* keys = table.Of(action);
        for (std::size_t slot = 0; slot < kSlots; ++slot)
        {
            const Binding& binding = keys[slot];
            if (!binding.Valid() || binding.vk != key || binding.ctrl != ctrl)
                continue;
            if (binding.shift == shift)
                return true;
            if (!binding.shift && shift)
            {
                bool shiftVariant = false;
                for (std::size_t other = 0; other < kActionCount && !shiftVariant; ++other)
                {
                    for (std::size_t s = 0; s < kSlots; ++s)
                    {
                        const Binding& b = table.keys[other][s];
                        shiftVariant = shiftVariant || (b.vk == key && b.ctrl == ctrl && b.shift);
                    }
                }
                if (!shiftVariant)
                    return true;
            }
        }
        return false;
    }

    // The action a press is (Action::Count: none).
    inline Action Find(const Table& table, unsigned key, bool ctrl, bool shift)
    {
        for (std::size_t index = 0; index < kActionCount; ++index)
        {
            if (Matches(table, kActions[index].action, key, ctrl, shift))
                return kActions[index].action;
        }
        return Action::Count;
    }

    // Whether `key` is one of the action's keys, whatever the modifiers (a
    // held key's release).
    inline bool UsesKey(const Table& table, Action action, unsigned key)
    {
        const Binding* keys = table.Of(action);
        return key != 0 && ((keys[0].Valid() && keys[0].vk == key) || (keys[1].Valid() && keys[1].vk == key));
    }

    // ---------------- Text ----------------

    // "K", "F2 / E" (with `extra`, a fixed key of the screen), "none".
    inline std::string Keys(const Table& table, Action action, const char* separator = " / ", const char* extra = nullptr)
    {
        std::string text;
        const Binding* keys = table.Of(action);
        for (std::size_t slot = 0; slot < kSlots; ++slot)
        {
            if (keys[slot].Valid())
                text += (text.empty() ? "" : separator) + Label(keys[slot]);
        }
        if (extra && *extra)
            text = text.empty() ? std::string(extra) : text + separator + extra;
        return text;
    }

    // A pair of actions on one key cap: ", ." or "[ ]" by default.
    inline std::string Pair(const Table& table, Action first, Action second)
    {
        const std::string a = Label(table.Of(first)[0]);
        const std::string b = Label(table.Of(second)[0]);
        if (a.empty() || b.empty())
            return a + b;
        return a + " " + b;
    }

    // A text the player reads never writes a key's name: it says
    // {key_<text>}, an action's (ActionInfo::text: {key_report}, {key_mark})
    // or a fixed key's (kFixed: {key_save}). Fill puts in the action's keys
    // as the key bar shows them ("Shift+F1", "F2 / J", "-" when unbound) and
    // the fixed key's name, from `table` when the text is shown. Every other
    // {name}, and {{ }}, stay for i18n::Format.
    inline std::string Fill(const char* text, const Table& table)
    {
        std::string out;
        for (const char* p = text; p && *p; ++p)
        {
            if ((*p == '{' && p[1] == '{') || (*p == '}' && p[1] == '}'))
            {
                out.append(p, 2);
                ++p;
                continue;
            }
            const char* close = *p == '{' && std::strncmp(p + 1, "key_", 4) == 0 ? std::strchr(p + 5, '}') : nullptr;
            if (close)
            {
                const std::string name(p + 5, close);
                const char* found = nullptr;
                std::string keys;
                for (const ActionInfo& info : kActions)
                {
                    if (name == info.text)
                    {
                        keys = Keys(table, info.action);
                        found = keys.empty() ? "-" : keys.c_str();
                    }
                }
                for (const FixedKey& key : kFixed)
                {
                    if (!found && name == key.text)
                        found = key.name;
                }
                if (found)
                {
                    out += found;
                    p = close;
                    continue;
                }
            }
            out += *p;
        }
        return out;
    }

    // Fill, then the text's other {names} (TRF's values).
    inline std::string Format(const char* text, const Table& table, std::initializer_list<replay::i18n::Arg> args)
    {
        return replay::i18n::Format(Fill(text, table).c_str(), args);
    }

    inline std::string LogLine(const Issue& issue)
    {
        std::string value = issue.value;
        for (char& c : value)
        {
            if (c == ',')
                c = '|';
        }
        std::string token = issue.token;
        Binding parsed;
        if (Parse(token, parsed) == ParseError::None)
            token = IniName(parsed);
        return std::string("KEYS,action=") + Info(issue.action).iniKey + ",value=" + value +
            (token.empty() ? std::string() : ",key=" + token) + ",refused=" + issue.reason + ",kept=" + issue.keptIni;
    }

    // One notice for the whole file: the first issue and how many more.
    inline std::string NoticeText(const std::vector<Issue>& issues)
    {
        if (issues.empty())
            return std::string();
        const Issue& first = issues.front();
        const std::string why = first.whyKey.empty() ? first.why
                                                     : replay::i18n::Format(TR(first.whyKey.c_str(), first.whyTemplate.c_str()), {{"action", first.whyArg}});
        std::string text = TRF("keys.notice", "Redemption Editor: [Keys] {action}={value}: {refused}{why}; keys: {kept}.",
            {{"action", Info(first.action).iniKey}, {"value", first.value},
                {"refused", first.token.empty() ? std::string() : TRF("keys.notice.refused", "{key} refused, ", {{"key", first.token}})},
                {"why", why}, {"kept", first.kept == "none" ? std::string(TR("keys.notice.none", "none")) : first.kept}});
        if (issues.size() > 1)
            text += TRF("keys.notice.more", " {count} more in RedemptionEditor.log (KEYS).", {{"count", std::to_string(issues.size() - 1)}});
        return text;
    }

    // The process's table (the core and the add-on each hold one, loaded
    // once from the same file).
    inline Table& Current()
    {
        static Table table = DefaultTable();
        return table;
    }
}
