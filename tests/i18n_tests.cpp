// The languages (src/i18n.h): the .lang parser (comments, quotes, escapes,
// bad lines), the values in a text, the fallbacks to English (a missing key,
// a missing file, a base language), UTF-8 (decoding, cutting, capitals), the
// measured cut of a name (ui_layout_check.h Ellipsize), the choice of the
// language, the pseudo language, and the shipped files (editor\lang).
#include "i18n.h"
#include "ui_layout_check.h"
#include "world_notice.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
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

    void CheckText(const std::string& got, const std::string& want, const char* message)
    {
        if (got == want)
            return;
        std::fprintf(stderr, "FAIL: %s: got \"%s\", want \"%s\"\n", message, got.c_str(), want.c_str());
        ++g_failures;
    }

    using replay::i18n::Table;

    void TestParser()
    {
        Table t;
        const bool clean = replay::i18n::Parse("\xEF\xBB\xBF# a comment\r\n"
                                               "\r\n"
                                               "   # an indented comment\n"
                                               "home.title = Clipes gravados\r\n"
                                               "spaced=\"  kept  \"\n"
                                               "escapes = a\\nb\\tc \\\\ \\\"q\\\"\n"
                                               "inner = say \"hi\" here\n"
                                               "trim =   both ends   \n"
                                               "empty =\n"
                                               "dup = first\n"
                                               "dup = second\n"
                                               "values = {count} de {total}\n",
            t);
        Check(clean && t.problems.empty(), "a good file has no problem (the BOM skipped)");
        CheckText(t.Find("home.title"), "Clipes gravados", "a plain text");
        CheckText(t.Find("spaced"), "  kept  ", "a quoted text keeps its spaces");
        CheckText(t.Find("escapes"), "a\nb\tc \\ \"q\"", "\\n \\t \\\\ \\\"");
        CheckText(t.Find("inner"), "say \"hi\" here", "a quote inside an unquoted text");
        CheckText(t.Find("trim"), "both ends", "an unquoted text is trimmed");
        CheckText(t.Find("empty"), "", "an empty text");
        CheckText(t.Find("dup"), "second", "a later line wins");
        CheckText(t.Find("values"), "{count} de {total}", "values are kept as written");
        Check(t.Find("missing") == nullptr, "a key the file lacks");
        Check(t.Size() == 8, "eight keys");

        Table bad;
        const bool ok = replay::i18n::Parse("no equals here\n= no key\nbad = a \\q escape\nquoted = \"a \" quote\"\ngood = yes\ntail = ends \\\n", bad);
        Check(!ok && bad.problems.size() == 5, "five bad lines, each a problem");
        Check(bad.problems.front().find("line 1") == 0, "a problem names its line");
        CheckText(bad.Find("good") ? bad.Find("good") : "", "yes", "the good line of a bad file is kept");
        Check(bad.Find("bad") == nullptr && bad.Find("tail") == nullptr, "a refused line adds no key");
    }

    void TestFormat()
    {
        using replay::i18n::Format;
        CheckText(Format("{count} of {total}", {{"count", "3"}, {"total", std::string("12")}}), "3 of 12", "two values");
        CheckText(Format("{total} / {count}", {{"count", "3"}, {"total", "12"}}), "12 / 3", "a translation reorders them");
        CheckText(Format("left {unknown} as is", {{"count", "3"}}), "left {unknown} as is", "a name with no value stays");
        CheckText(Format("{{literal}} and {x}", {{"x", "1"}}), "{literal} and 1", "{{ and }} are braces");
        CheckText(Format("no close {x", {{"x", "1"}}), "no close {x", "an open brace stays");
        CheckText(replay::i18n::Printf("%.1f s, %u", 2.4, 7u), "2.4 s, 7", "Printf as the English formatted it");
        const std::vector<std::string> names = replay::i18n::Placeholders("{a} {{no}} {b} {a}");
        Check(names.size() == 3 && names[0] == "a" && names[1] == "b" && names[2] == "a", "the names of a text");
        // TRF with English active: the English filled in.
        CheckText(TRF("test.trf", "{n} clips", {{"n", "4"}}), "4 clips", "TRF in English");
    }

    std::wstring TempFolder()
    {
        wchar_t base[MAX_PATH]{};
        GetTempPathW(MAX_PATH, base);
        const std::wstring folder = std::wstring(base) + L"redemption_i18n_tests_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
        CreateDirectoryW(folder.c_str(), nullptr);
        return folder;
    }

    void WriteText(const std::wstring& path, const char* text)
    {
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"wb") == 0 && file)
        {
            std::fwrite(text, 1, std::strlen(text), file);
            std::fclose(file);
        }
    }

    void TestFallbacks()
    {
        CheckText(TR("test.key", "English"), "English", "no language: the English");
        const std::wstring folder = TempFolder();
        WriteText(folder + L"pt.lang", "test.key = Portugu\xC3\xAAs\n");
        WriteText(folder + L"fr.lang", "other.key = autre\n");
        std::string loaded;
        Check(replay::i18n::Load(folder, "en", &loaded) == nullptr && loaded.empty(), "en loads nothing (the code's English)");
        Check(replay::i18n::Load(folder, "de", &loaded) == nullptr && loaded.empty(), "a language with no file: English");
        std::unique_ptr<Table> portuguese = replay::i18n::Load(folder, "pt-BR", &loaded);
        Check(portuguese && loaded == "pt.lang" && portuguese->code == "pt", "pt-BR with only pt.lang reads the base language");
        std::unique_ptr<Table> french = replay::i18n::Load(folder, "fr", &loaded);
        Check(french && loaded == "fr.lang", "a file of the code");
        replay::i18n::SetActive(std::move(portuguese));
        CheckText(TR("test.key", "English"), "Portugu\xC3\xAAs", "a key the file has");
        CheckText(TR("test.absent", "Still English"), "Still English", "a key the file lacks: the English");
        CheckText(replay::i18n::ActiveCode(), "pt", "the active code");
        replay::i18n::SetActive(std::move(french));
        CheckText(TR("test.key", "English"), "English", "another language without the key");
        replay::i18n::SetActive(nullptr);
        CheckText(TR("test.key", "English"), "English", "back to English");
        CheckText(replay::i18n::ActiveCode(), "en", "English's code");
        DeleteFileW((folder + L"pt.lang").c_str());
        DeleteFileW((folder + L"fr.lang").c_str());
        RemoveDirectoryW(folder.c_str());

        const std::vector<std::string> candidates = replay::i18n::FileCandidates("pt-BR");
        Check(candidates.size() == 2 && candidates[0] == "pt-BR" && candidates[1] == "pt", "pt-BR, then pt");
        Check(replay::i18n::FileCandidates("en").empty() && replay::i18n::FileCandidates("pseudo").empty(), "no file for en and pseudo");
    }

    void TestUtf8()
    {
        using replay::i18n::Decode;
        const char* text = "A\xC3\xA7\xE2\x82\xAC\xF0\x9F\x98\x80";
        const char* p = text;
        Check(Decode(p) == 'A' && Decode(p) == 0xE7 && Decode(p) == 0x20AC && Decode(p) == 0x1F600 && *p == 0, "1 to 4 bytes");
        const char* stray = "\x80\xC3";
        p = stray;
        Check(Decode(p) == replay::i18n::kReplacement && p == stray + 1, "a stray continuation byte: U+FFFD, one byte");
        Check(Decode(p) == replay::i18n::kReplacement && *p == 0, "a cut sequence at the end: U+FFFD");
        const char* overlong = "\xC0\xAF";
        p = overlong;
        Check(Decode(p) == replay::i18n::kReplacement, "an overlong sequence");
        const char* surrogate = "\xED\xA0\x80";
        p = surrogate;
        Check(Decode(p) == replay::i18n::kReplacement, "a surrogate");
        const char* bounded = "\xC3\xA7";
        p = bounded;
        Check(Decode(p, bounded + 1) == replay::i18n::kReplacement, "a sequence past `end`");

        std::string built;
        replay::i18n::Append(built, 0xE7);
        replay::i18n::Append(built, 0x41F);
        replay::i18n::Append(built, 0x1F600);
        CheckText(built, "\xC3\xA7\xD0\x9F\xF0\x9F\x98\x80", "Append writes UTF-8");

        const std::string word = "a\xC3\xA7\xC3\xA3o"; // "acao" with c-cedilla and a-tilde
        Check(replay::i18n::Boundary(word, 2) == 1 && replay::i18n::Boundary(word, 3) == 3 && replay::i18n::Boundary(word, 99) == word.size(),
            "a cut moves back to a character's start");
        char shortBuffer[4];
        replay::i18n::CopyTo(shortBuffer, sizeof(shortBuffer), word);
        CheckText(shortBuffer, "a\xC3\xA7", "CopyTo never leaves half a character");
        char tiny[2];
        replay::i18n::CopyTo(tiny, sizeof(tiny), "\xC3\xA7");
        Check(tiny[0] == '\0', "a buffer too small for the first character stays empty");

        using replay::i18n::ToUpper;
        Check(ToUpper('a') == 'A' && ToUpper('Z') == 'Z' && ToUpper('1') == '1', "ASCII capitals");
        Check(ToUpper(0xE7) == 0xC7 && ToUpper(0xE3) == 0xC3 && ToUpper(0xF7) == 0xF7 && ToUpper(0xFF) == 0x178 && ToUpper(0xDF) == 0xDF,
            "Latin-1: c-cedilla, a-tilde; the division sign and sharp s stay; y-diaeresis");
        Check(ToUpper(0x142) == 0x141 && ToUpper(0x141) == 0x141 && ToUpper(0x15B) == 0x15A && ToUpper(0x17C) == 0x17B && ToUpper(0x131) == 'I',
            "Latin Extended-A: l-stroke, s-acute, z-dot; dotless i");
        Check(ToUpper(0x105) == 0x104 && ToUpper(0x11B) == 0x11A && ToUpper(0x159) == 0x158, "Latin Extended-A: a-ogonek, e-caron, r-caron");
        Check(ToUpper(0x430) == 0x410 && ToUpper(0x44F) == 0x42F && ToUpper(0x451) == 0x401 && ToUpper(0x410) == 0x410, "Cyrillic");
        Check(ToUpper(0x4E2D) == 0x4E2D, "a CJK ideograph stays");
    }

    // CJK: which code points break a line between them, and the kinsoku rules.
    void TestCjk()
    {
        using replay::i18n::IsCjk;
        Check(IsCjk(0x4E2D) && IsCjk(0x3042) && IsCjk(0x30AB) && IsCjk(0xAC00) && IsCjk(0x3002) && IsCjk(0xFF0C), "ideograph, kana, hangul, punctuation");
        Check(!IsCjk('A') && !IsCjk(0xE7) && !IsCjk(0x416), "Latin and Cyrillic are not");
        Check(replay::i18n::NoLineStart(0x3002) && replay::i18n::NoLineStart(0xFF0C) && replay::i18n::NoLineStart(0x30FC) && replay::i18n::NoLineStart(')'),
            "no line starts with a full stop, a comma, a long vowel mark, a closing bracket");
        Check(replay::i18n::NoLineEnd(0x300C) && replay::i18n::NoLineEnd(0xFF08) && replay::i18n::NoLineEnd('('), "no line ends with an opening bracket");
        Check(!replay::i18n::NoLineStart(0x4E2D) && !replay::i18n::NoLineEnd(0x4E2D), "an ideograph goes anywhere");
        // A CJK table gives the fonts its ideographs, no whole block.
        Table t;
        replay::i18n::Parse("a = \xE7\x89\x87\xE6\xAE\xB5\n", t); // two ideographs
        const std::vector<std::uint32_t> glyphs = replay::i18n::LanguageGlyphs(&t);
        Check(glyphs.size() == 2 && IsCjk(glyphs[0]) && IsCjk(glyphs[1]), "only the table's ideographs");
    }

    // Ellipsize (ui_layout_check.h) with a width per character: a cut name
    // never ends with half a character.
    void TestMeasuredCut()
    {
        const auto widthOf = [](const std::string& text) {
            float width = 0.0f;
            for (const char* p = text.c_str(); *p;)
            {
                replay::i18n::Decode(p);
                width += 1.0f;
            }
            return width;
        };
        const std::string name = "Lembran\xC3\xA7\xC3\xA3o do rancho";
        for (float room = 3.0f; room <= 20.0f; room += 1.0f)
        {
            const std::string cut = replay::editor::uilayout::Ellipsize(name, room, widthOf);
            Check(widthOf(cut) <= room || cut == "...", "the cut fits");
            bool whole = true;
            for (const char* p = cut.c_str(); *p;)
                whole = whole && replay::i18n::Decode(p) != replay::i18n::kReplacement;
            Check(whole, "the cut keeps whole characters");
        }
        CheckText(replay::editor::uilayout::Ellipsize(std::string("Short"), 10.0f, widthOf), "Short", "a fitting name is whole");
        CheckText(replay::editor::uilayout::Ellipsize(std::string("Ascii name here"), 8.0f, widthOf), "Ascii...", "ASCII cuts as before");
    }

    void TestChoice()
    {
        using replay::i18n::Choose;
        CheckText(Choose("", 0), "en", "nothing known: English");
        CheckText(Choose("auto", 0), "en", "auto before the core said the game's language");
        CheckText(Choose("auto", 6), "pt-BR", "auto: the game's (5 brazilian)");
        CheckText(Choose("AUTO", 8), "ru", "auto in any case");
        CheckText(Choose("", 13), "zh-CN", "chinesesimp");
        CheckText(Choose("pt_br", 1), "pt-BR", "the ini wins, normalised");
        CheckText(Choose(" fr ", 6), "fr", "spaces around the value");
        CheckText(Choose("auto", 200), "en", "an id the list does not have");
        CheckText(Choose("pseudo", 6), "pseudo", "the pseudo language by name");
        CheckText(replay::i18n::GameLanguageCode(11), "es-MX", "mexican");
        CheckText(Choose("auto", 5), "es", "auto: spanish (4) is Spain's");
        CheckText(Choose("zh-Hans", 0), "zh-CN", "zh-Hans is the game's zh-CN file");
        CheckText(Choose("zh_hant", 0), "zh-TW", "zh-Hant is the game's zh-TW file");
        CheckText(Choose("auto", 10), "zh-TW", "auto: chinese (9) is traditional");
        CheckText(Choose("auto", 12), "es-MX", "auto: mexican (11) is Latin America's");
        Check(replay::i18n::FileCandidates("es-MX").size() == 2 && replay::i18n::FileCandidates("es-MX")[1] == "es", "es-MX, then es");
        CheckText(replay::i18n::Normalize("ES-mx"), "es-MX", "language lower case, region upper case");
    }

    void TestPseudo()
    {
        const std::string pseudo = replay::i18n::Pseudo("Edit Camera");
        Check(pseudo.front() == '[' && pseudo.back() == ']', "brackets show a cut text");
        std::size_t chars = 0;
        bool accented = false;
        for (const char* p = pseudo.c_str(); *p;)
        {
            accented = accented || static_cast<unsigned char>(*p) >= 0x80;
            replay::i18n::Decode(p);
            ++chars;
        }
        Check(accented, "accented letters");
        Check(chars >= 15 && chars <= 17, "about 140 % of 11 characters");
        const std::string valued = replay::i18n::Pseudo("{count} of {total} clips");
        Check(valued.find("{count}") != std::string::npos && valued.find("{total}") != std::string::npos, "values kept");
        replay::i18n::SetActive(replay::i18n::Load(L"", "pseudo"));
        CheckText(replay::i18n::ActiveCode(), "pseudo", "the pseudo language is active");
        const std::string shown = TRF("test.values", "{n} clips", {{"n", "5"}});
        Check(shown.find('5') != std::string::npos && shown.front() == '[', "TRF in the pseudo language");
        const std::vector<std::uint32_t> glyphs = replay::i18n::LanguageGlyphs(replay::i18n::Active());
        Check(!glyphs.empty() && glyphs.front() >= 0x80, "the pseudo language's letters go to the fonts");
        replay::i18n::SetActive(nullptr);
        Check(replay::i18n::LanguageGlyphs(nullptr).empty(), "English bakes no letter past ASCII");
    }

    // The notices built in the shared headers stay the English when no
    // language is set, and take a translation when one is.
    void TestNoticeTranslation()
    {
        char text[256];
        Check(replay::world::NoticeText(3, 5, 0, false, text, sizeof(text)) &&
                std::strcmp(text, "Recorded in Chapter 3, you are in Chapter 5: the world may look different") == 0,
            "the world notice in English");
        auto table = std::make_unique<Table>();
        table->code = "pt-BR";
        replay::i18n::Parse("world.chapter = Cap\xC3\xADtulo {n}\n"
                            "world.notice.chapter = Gravado no {recorded}, voc\xC3\xAA est\xC3\xA1 no {current}: {what}\n",
            *table);
        replay::i18n::SetActive(std::move(table));
        Check(replay::world::NoticeText(3, 5, 0, false, text, sizeof(text)) &&
                std::strcmp(text, "Gravado no Cap\xC3\xADtulo 3, voc\xC3\xAA est\xC3\xA1 no Cap\xC3\xADtulo 5: the world may look different") == 0,
            "the world notice translated (a key left out stays English)");
        char chapter[24];
        replay::world::ChapterName(3, chapter, sizeof(chapter));
        CheckText(chapter, "Chapter 3", "the log's chapter name stays English");
        replay::i18n::SetActive(nullptr);
    }

    std::string ReadFile(const char* path)
    {
        std::string text;
        FILE* file = nullptr;
        if (fopen_s(&file, path, "rb") != 0 || !file)
            return text;
        char buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
            text.append(buffer, read);
        std::fclose(file);
        return text;
    }

    std::multiset<std::string> NameSet(const char* text)
    {
        const std::vector<std::string> names = replay::i18n::Placeholders(text);
        return std::multiset<std::string>(names.begin(), names.end());
    }

    // One shipped translation against en.lang: it parses clean, has every
    // English key and no other, keeps every {name}, has no em or en dash.
    void CheckTranslation(const Table& en, const std::string& name)
    {
        const std::string text = ReadFile(("editor\\lang\\" + name).c_str());
        Table t;
        const bool clean = replay::i18n::Parse(text, t) && t.problems.empty();
        std::size_t missing = 0, extra = 0, values = 0;
        for (const std::string& key : en.Keys())
        {
            const char* translated = t.Find(key.c_str());
            if (!translated)
                ++missing;
            else if (NameSet(translated) != NameSet(en.Find(key.c_str())))
            {
                std::fprintf(stderr, "  %s %s: other {names} than the English\n", name.c_str(), key.c_str());
                ++values;
            }
        }
        for (const std::string& key : t.Keys())
            extra += en.Find(key.c_str()) ? 0 : 1;
        const bool dash = text.find("\xE2\x80\x93") != std::string::npos || text.find("\xE2\x80\x94") != std::string::npos;
        if (!clean || missing || extra || values || dash)
            std::fprintf(stderr, "FAIL: %s: clean %d, missing %zu, extra %zu, other names %zu, dash %d\n", name.c_str(), clean ? 1 : 0, missing,
                extra, values, dash ? 1 : 0);
        g_failures += (!clean || missing || extra || values || dash) ? 1 : 0;
    }

    // The files in editor\lang (the tests run from the project root): en.lang
    // and every translation there (pt-BR at least).
    void TestShippedFiles()
    {
        const std::string english = ReadFile("editor\\lang\\en.lang");
        Table en;
        Check(!english.empty() && replay::i18n::Parse(english, en) && en.problems.empty(), "en.lang is there and parses clean");
        Check(en.Size() > 900, "en.lang has the editor's and the notices' texts");
        Check(english.find("\xE2\x80\x93") == std::string::npos && english.find("\xE2\x80\x94") == std::string::npos, "no em or en dash");
        std::vector<std::string> names;
        WIN32_FIND_DATAA found{};
        HANDLE find = FindFirstFileA("editor\\lang\\*.lang", &found);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
                if (std::string(found.cFileName) != "en.lang")
                    names.push_back(found.cFileName);
            while (FindNextFileA(find, &found));
            FindClose(find);
        }
        Check(std::find(names.begin(), names.end(), "pt-BR.lang") != names.end(), "pt-BR.lang is shipped");
        for (const std::string& name : names)
            CheckTranslation(en, name);
        // Every pt-BR letter is one the fonts get (LanguageGlyphs: no CJK, nothing past Cyrillic).
        Table pt;
        replay::i18n::Parse(ReadFile("editor\\lang\\pt-BR.lang"), pt);
        const std::vector<std::uint32_t> glyphs = replay::i18n::LanguageGlyphs(&pt);
        bool latin = !glyphs.empty();
        for (const std::uint32_t code : glyphs)
            latin = latin && code < 0x400;
        Check(latin, "pt-BR's letters are Latin");
    }
}

int RunI18nTests()
{
    g_failures = 0;
    TestParser();
    TestFormat();
    TestFallbacks();
    TestUtf8();
    TestMeasuredCut();
    TestChoice();
    TestCjk();
    TestPseudo();
    TestNoticeTranslation();
    TestShippedFiles();
    replay::i18n::SetActive(nullptr);
    std::printf("i18n tests: %s (%d failures)\n", g_failures ? "FAILED" : "ok", g_failures);
    return g_failures;
}
