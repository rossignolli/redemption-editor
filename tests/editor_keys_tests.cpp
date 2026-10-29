// The remappable key table (src/editor_keys.h): the parser, the defaults,
// the reserved keys, the duplicates, the matching rule and the texts of the
// key bar and the log.
#include "editor_keys.h"
#include "editor_ui_qa.h"

#include <cstdio>
#include <map>
#include <string>

namespace
{
    using namespace replay::keys;

    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    Binding Key(std::uint8_t code, bool ctrl = false, bool shift = false)
    {
        Binding binding;
        binding.vk = code;
        binding.ctrl = ctrl;
        binding.shift = shift;
        return binding;
    }

    LoadResult LoadMap(const std::map<std::string, std::string>& ini)
    {
        return Load([&](const char* key, bool& present) {
            const auto it = ini.find(key);
            present = it != ini.end();
            return present ? it->second : std::string();
        });
    }

    bool HasIssue(const LoadResult& result, Action action, const std::string& reason)
    {
        for (const Issue& issue : result.issues)
        {
            if (issue.action == action && issue.reason == reason)
                return true;
        }
        return false;
    }

    void TestParse()
    {
        Binding b;
        Check(Parse("K", b) == ParseError::None && b == Key('K'), "a letter");
        Check(Parse("k", b) == ParseError::None && b == Key('K'), "a letter, any case");
        Check(Parse("Comma", b) == ParseError::None && b == Key(vk::Comma), "Comma");
        Check(Parse("period", b) == ParseError::None && b == Key(vk::Period), "Period, any case");
        Check(Parse("LeftBracket", b) == ParseError::None && b == Key(vk::LeftBracket), "LeftBracket");
        Check(Parse("[", b) == ParseError::None && b == Key(vk::LeftBracket), "[ as a character");
        Check(Parse("F11", b) == ParseError::None && b == Key(vk::F(11)), "F11");
        Check(Parse("Shift+F4", b) == ParseError::None && b == Key(vk::F(4), false, true), "Shift+F4");
        Check(Parse(" ctrl + left ", b) == ParseError::None && b == Key(vk::Left, true), "Ctrl+Left with spaces");
        Check(Parse("Ctrl+Shift+J", b) == ParseError::None && b == Key('J', true, true), "two modifiers");
        Check(Parse("Num4", b) == ParseError::None && b == Key(vk::Numpad0 + 4), "Num4");
        Check(Parse("Ctrl++", b) == ParseError::None && b == Key(vk::Plus, true), "Ctrl and the + key");
        Check(Parse("7", b) == ParseError::None && b == Key('7'), "a digit");
        Check(Parse("Alt+J", b) == ParseError::Alt, "Alt refused");
        Check(Parse("Ctrl", b) == ParseError::ModifierOnly, "a modifier alone");
        Check(Parse("Shift+", b) == ParseError::ModifierOnly, "a modifier with no key");
        Check(Parse("Banana", b) == ParseError::UnknownKey, "an unknown name");
        Check(Parse("F25", b) == ParseError::UnknownKey, "F25 does not exist");
        Check(Parse("", b) == ParseError::Empty, "empty");
    }

    void TestLabels()
    {
        Check(Label(Key(vk::Comma)) == ",", "Comma shows ,");
        Check(Label(Key(vk::RightBracket)) == "]", "RightBracket shows ]");
        Check(Label(Key(vk::F(4), false, true)) == "Shift+F4", "Shift+F4");
        Check(Label(Key(vk::Left, true)) == "Ctrl+Left", "Ctrl+Left");
        Check(Label(Key(vk::Numpad0 + 4)) == "Num 4", "Num 4");
        Check(Label(Key('J')) == "J", "J");
        // Every name parses back to the same key.
        for (const char* name : {"J", "Comma", "Period", "LeftBracket", "Semicolon", "Num7", "F9", "Shift+F2", "Ctrl+Up"})
        {
            Binding parsed, again;
            Parse(name, parsed);
            const std::string label = Label(parsed);
            const bool labelParses = Parse(label, again) == ParseError::None && again == parsed;
            const bool isSpaced = label.find(' ') != std::string::npos; // "Num 7" is a display text
            Check(labelParses || isSpaced, "a label reads back as its key");
        }
    }

    void TestDefaultsAreToday()
    {
        const LoadResult result = LoadMap({});
        const Table& t = result.table;
        Check(result.issues.empty() && result.configured == 0, "no ini: no issue");
        Check(t.Of(Action::PlayPause)[0] == Key('K'), "K plays and pauses");
        Check(t.Of(Action::FrameBack)[0] == Key(vk::Comma), ", is a frame back");
        Check(t.Of(Action::FrameForward)[0] == Key(vk::Period), ". is a frame forward");
        Check(!t.Bound(Action::Rewind) && !t.Bound(Action::FastForward), "rewind and fast forward ride on , . by default");
        Check(t.Of(Action::SpeedDown)[0] == Key(vk::LeftBracket) && t.Of(Action::SpeedUp)[0] == Key(vk::RightBracket), "[ ] speed");
        Check(t.Of(Action::FreeCam)[0] == Key(vk::F(11)), "F11 free camera");
        Check(t.Of(Action::HideUi)[0] == Key('H'), "H hides the UI");
        Check(t.Of(Action::Editor)[0] == Key(vk::F(2)), "F2 editor");
        Check(t.Of(Action::Stop)[0] == Key(vk::F(7)), "F7 play / stop");
        Check(t.Of(Action::Record)[0] == Key(vk::F(4)), "F4 record");
        Check(t.Of(Action::SaveBuffer)[0] == Key(vk::F(4), false, true), "Shift+F4 saves the buffer");
        Check(t.Of(Action::MarkProblem)[0] == Key(vk::F(2), false, true), "Shift+F2 marks a problem");
        Check(t.Of(Action::Report)[0] == Key(vk::F(1), false, true), "Shift+F1 saves the report");
        for (std::size_t index = 0; index < kActionCount; ++index)
            Check(!t.keys[index][1].Valid(), "one key per action by default");
        // Every default is free of the reserved list (the defaults never refuse).
        for (std::size_t index = 0; index < kActionCount; ++index)
            Check(!t.keys[index][0].Valid() || ReservedFor(t.keys[index][0]) == nullptr, "a default is not reserved");
    }

    void TestRemap()
    {
        const LoadResult result = LoadMap({{"FrameBack", "J"}, {"FrameForward", "L, Num6"}, {"PlayPause", "Space, K"}});
        const Table& t = result.table;
        Check(t.Of(Action::FrameBack)[0] == Key('J') && !t.Of(Action::FrameBack)[1].Valid(), "frame back on J");
        Check(t.Of(Action::FrameForward)[0] == Key('L') && t.Of(Action::FrameForward)[1] == Key(vk::Numpad0 + 6),
            "two keys for frame forward");
        Check(t.Of(Action::PlayPause)[0] == Key('K') && !t.Of(Action::PlayPause)[1].Valid(),
            "Space refused (the editor's), K kept");
        Check(HasIssue(result, Action::PlayPause, "reserved_editor_fixed"), "Space reported");
        Check(result.configured == 3, "three actions from the ini");
        Check(Keys(t, Action::FrameForward) == "L / Num 6", "key text");
        Check(Pair(t, Action::FrameBack, Action::FrameForward) == "J L", "the pair on one key cap");
    }

    void TestReserved()
    {
        const LoadResult result = LoadMap({{"FrameBack", "Shift+Tab"}, {"FrameForward", "Home"}, {"Rewind", "Ctrl+F6"},
            {"FastForward", "F12"}, {"SpeedDown", "Ctrl+R"}, {"SpeedUp", "F1"}, {"Report", "Alt+J"}});
        const Table& t = result.table;
        Check(HasIssue(result, Action::FrameBack, "reserved_steam_overlay_or_markers"), "Shift+Tab refused (Steam overlay)");
        Check(HasIssue(result, Action::FrameForward, "reserved_social_club"), "Home refused (Social Club)");
        Check(HasIssue(result, Action::Rewind, "reserved_photo_mode"), "F6 refused with any modifier (Photo Mode)");
        Check(HasIssue(result, Action::FastForward, "reserved_steam_screenshot"), "F12 refused");
        Check(HasIssue(result, Action::SpeedDown, "reserved_scripthook_reload"), "Ctrl+R refused");
        Check(HasIssue(result, Action::SpeedUp, "reserved_help"), "F1 refused (help)");
        Check(HasIssue(result, Action::Report, "alt_modifier"), "Alt refused");
        Check(t.Of(Action::FrameBack)[0] == Key(vk::Comma) && t.Of(Action::FrameForward)[0] == Key(vk::Period),
            "a refused value keeps the default");
        Check(HasIssue(result, Action::FrameBack, "kept_default"), "the fallback is said");
        Check(!t.Bound(Action::Rewind), "Rewind keeps its default (none)");
        Check(t.Of(Action::Report)[0] == Key(vk::F(1), false, true), "Shift+F1 stays the report");
        Binding b;
        Parse("Shift+F1", b);
        Check(ReservedFor(b) == nullptr, "Shift+F1 is not the help");
        for (const char* key : {"W", "A", "S", "D", "Q", "E", "Z", "C", "PageUp", "PageDown", "Left", "Ctrl+Left", "Esc", "Enter",
                 "F3", "F5", "F10", "Insert", "Delete", "End", "Ctrl+S", "T", "P"})
        {
            Parse(key, b);
            Check(ReservedFor(b) != nullptr, "a key the game, an overlay or the mod already uses is reserved");
        }
        for (const char* key : {"J", "L", "U", "Num4", "Ctrl+J", "Shift+K", "Semicolon", "F", "G", "M"})
        {
            Parse(key, b);
            Check(ReservedFor(b) == nullptr, "a free key is accepted");
        }
        // F8 and F9 are the debug HUD's keys: reserved in the developer build
        // only, free to bind in the public one.
        for (const char* key : {"F8", "F9"})
        {
            Parse(key, b);
#if defined(REDEMPTION_DEV)
            Check(ReservedFor(b) != nullptr, "the debug HUD's key is reserved in the developer build");
#else
            Check(ReservedFor(b) == nullptr, "the debug HUD's key is free in the public build");
#endif
        }
    }

    void TestDuplicates()
    {
        // An ini key wins over a default: PlayPause on Comma leaves
        // FrameBack without a key (reported).
        LoadResult result = LoadMap({{"PlayPause", "Comma"}});
        Check(result.table.Of(Action::PlayPause)[0] == Key(vk::Comma), "the ini's key wins");
        Check(!result.table.Bound(Action::FrameBack), "the default that collides loses it");
        Check(HasIssue(result, Action::FrameBack, "duplicate_PlayPause"), "the collision is reported");
        // Two ini values on one key: the first action of the table keeps it.
        result = LoadMap({{"FrameBack", "J"}, {"Rewind", "J, U"}});
        Check(result.table.Of(Action::FrameBack)[0] == Key('J'), "the first keeps J");
        Check(result.table.Of(Action::Rewind)[0] == Key('U') && !result.table.Of(Action::Rewind)[1].Valid(),
            "the second keeps its other key");
        Check(HasIssue(result, Action::Rewind, "duplicate_FrameBack"), "reported");
        // Unbinding and too many keys.
        result = LoadMap({{"HideUi", ""}, {"Stop", "None"}, {"Record", "J, U, M"}});
        Check(!result.table.Bound(Action::HideUi) && !result.table.Bound(Action::Stop), "empty or None unbinds");
        Check(HasIssue(result, Action::Record, "too_many_keys"), "a third key is refused");
        Check(result.table.Of(Action::Record)[0] == Key('J') && result.table.Of(Action::Record)[1] == Key('U'), "two kept");
        // An unbound default frees its key for another action.
        result = LoadMap({{"HideUi", "None"}, {"PlayPause", "H"}});
        Check(result.table.Of(Action::PlayPause)[0] == Key('H') && result.issues.empty(), "H moves to play / pause");
    }

    void TestMatching()
    {
        const Table t = DefaultTable();
        Check(Matches(t, Action::PlayPause, 'K', false, false), "K");
        Check(Matches(t, Action::PlayPause, 'K', false, true), "Shift+K still pauses (free camera fast key)");
        Check(!Matches(t, Action::PlayPause, 'K', true, false), "Ctrl+K is not K");
        Check(Matches(t, Action::Record, vk::F(4), false, false), "F4 records");
        Check(!Matches(t, Action::Record, vk::F(4), false, true), "Shift+F4 does not record");
        Check(Matches(t, Action::SaveBuffer, vk::F(4), false, true), "Shift+F4 saves the buffer");
        Check(Matches(t, Action::Editor, vk::F(2), false, false) && !Matches(t, Action::Editor, vk::F(2), false, true),
            "F2 is the editor, Shift+F2 is not");
        Check(Find(t, vk::F(2), false, true) == Action::MarkProblem, "Shift+F2 marks");
        Check(Find(t, 'J', false, false) == Action::Count, "J is nothing by default");
        Check(UsesKey(t, Action::FrameBack, vk::Comma) && !UsesKey(t, Action::FrameBack, vk::Period), "held key release");
        const Table remapped = LoadMap({{"FrameBack", "Ctrl+J"}}).table;
        Check(Matches(remapped, Action::FrameBack, 'J', true, false) && !Matches(remapped, Action::FrameBack, 'J', false, false),
            "a Ctrl binding needs Ctrl");
        Check(!Matches(remapped, Action::FrameBack, vk::Comma, false, false), "the old key is free");
    }

    void TestTexts()
    {
        const LoadResult result = LoadMap({{"FrameBack", "Home"}});
        Check(result.issues.size() == 2, "the refused key and the kept default");
        const std::string line = LogLine(result.issues.front());
        Check(line == "KEYS,action=FrameBack,value=Home,key=Home,refused=reserved_social_club,kept=Comma", "log line");
        Check(IniName(Key(vk::F(4), false, true)) == "Shift+F4" && IniName(Key(vk::Numpad0 + 6)) == "Num6", "ini names");
        const std::string notice = NoticeText(result.issues);
        Check(notice.find("[Keys] FrameBack=Home") != std::string::npos && notice.find("Social Club") != std::string::npos &&
                notice.find("keys: ,") != std::string::npos && notice.find("1 more") != std::string::npos,
            "notice: the key, the reason, what is kept, the rest in the log");
        Check(NoticeText({}).empty(), "no notice without an issue");
        const Table t = DefaultTable();
        Check(Pair(t, Action::FrameBack, Action::FrameForward) == ", .", "the bar's , .");
        Check(Pair(t, Action::SpeedDown, Action::SpeedUp) == "[ ]", "the bar's [ ]");
        Check(Keys(t, Action::Editor, " / ", "E") == "F2 / E", "a fixed key after the table's");
        Check(Keys(t, Action::Rewind).empty(), "an unbound action has no text");
        char text[160];
        const Table report = LoadMap({{"Report", "Ctrl+J"}}).table;
        replay::editor::uiqa::FormatMarkNotice(text, sizeof(text), 2, report);
        Check(std::string(text) == "Problem marked (2 waiting). Ctrl+J saves the report when you are done.",
            "the mark notice names the report key of the table");
        replay::editor::uiqa::FormatStopReminder(text, sizeof(text), 1, report);
        Check(std::string(text) == "1 problem mark not sent yet. Press Ctrl+J to save the report.", "the reminder too");
    }

    // The keys a text names ({key_report}, {key_save}) come from the table.
    void TestFill()
    {
        const LoadResult result = LoadMap({{"Report", "Ctrl+F11"}, {"MarkProblem", "Ctrl+M"}, {"Record", "J, Ctrl+J"}, {"SaveBuffer", "Shift+J"},
            {"FrameBack", "Num4"}, {"SpeedDown", "Minus"}});
        Check(result.issues.empty(), "the remap is valid");
        const Table& t = result.table;
        Check(Fill("Press {key_report} to save the report.", t) == "Press Ctrl+F11 to save the report.", "an action's key");
        Check(Fill("{key_mark}, {key_record}, {key_save_buffer}", t) == "Ctrl+M, J / Ctrl+J, Shift+J", "two keys as the key bar shows them");
        Check(Fill("{key_frame_back} {key_speed_down} {key_overlay}", t) == "Num 4 - `", "the key cap's names");
        Check(Fill("{key_rewind}", t) == "-", "an unbound action reads as the key bar's '-'");
        Check(Fill("{key_save}, {key_select}, {key_help} or {key_back}", t) == "Ctrl+S, Enter, F1 or Esc", "the fixed keys");
        Check(Fill("{count} of {key_nope} {{key_report}} {key_report", t) == "{count} of {key_nope} {{key_report}} {key_report",
            "other names, escapes and an unclosed brace stay");
        Check(Format("{count} marks. {key_report} saves {{them}}.", t, {{"count", "3"}}) == "3 marks. Ctrl+F11 saves {them}.",
            "Format fills the keys, then the values");
        Check(Format("{name}", t, {{"name", "{key_report}"}}) == "{key_report}", "a value is never read as a key");
        const Table defaults = DefaultTable();
        Check(Fill("Save what the replay buffer holds as a clip ({key_save_buffer}). Stop with {key_record}.", defaults) ==
                "Save what the replay buffer holds as a clip (Shift+F4). Stop with F4.",
            "the defaults give today's English");
        Check(Fill("{key_report} {key_mark} {key_editor} {key_stop} {key_free_cam} {key_hide} {key_play_pause}", defaults) ==
                "Shift+F1 Shift+F2 F2 F7 F11 H K",
            "every default key as the key bar writes it");
        // Every action has a placeholder of its own, none a fixed key's.
        for (std::size_t a = 0; a < kActionCount; ++a)
        {
            Check(kActions[a].text && *kActions[a].text, "an action has a text name");
            for (std::size_t b = a + 1; b < kActionCount; ++b)
                Check(std::string(kActions[a].text) != kActions[b].text, "text names are unique");
            for (const FixedKey& key : kFixed)
                Check(std::string(kActions[a].text) != key.text, "an action's text name is not a fixed key's");
        }
        // A fixed key is one the table refuses (kReserved), so no remap moves it.
        for (const FixedKey& key : kFixed)
        {
            Binding binding;
            const std::string name = std::string(key.name) == "WASD" ? "W" : key.name;
            Check(Parse(name, binding) == ParseError::None && ReservedFor(binding) != nullptr, "a fixed key is reserved");
        }
    }

    void TestIniKeysUnique()
    {
        for (std::size_t a = 0; a < kActionCount; ++a)
        {
            Check(static_cast<std::size_t>(kActions[a].action) == a, "the table is in enum order");
            for (std::size_t b = a + 1; b < kActionCount; ++b)
                Check(std::string(kActions[a].iniKey) != kActions[b].iniKey, "ini keys are unique");
        }
    }
}

int RunEditorKeysTests()
{
    TestParse();
    TestLabels();
    TestDefaultsAreToday();
    TestRemap();
    TestReserved();
    TestDuplicates();
    TestMatching();
    TestTexts();
    TestFill();
    TestIniKeysUnique();
    if (g_failures == 0)
        std::printf("editor keys tests passed\n");
    return g_failures;
}
