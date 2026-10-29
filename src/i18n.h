#pragma once

// The mod's languages. Every text the player sees goes through TR (a fixed
// text) or TRF (a text with {named} values), with its English at the call
// site: English is the source and needs no file. Another language is a UTF-8
// file next to RDR2.exe, RedemptionEditor\lang\<code>.lang:
//
//   # A comment: a line whose first character is '#'.
//   home.clips = Clips grabados
//   gallery.count = {shown} de {total} clips
//   edit.spaced = "  quoted: the spaces are kept  "
//
// key = text. The text is trimmed unless it is quoted; \n, \t, \\ and \" are
// escapes; {name} is a value the code puts in ({{ and }} are braces). A key
// the file lacks, or a missing file, reads the English. lang/en.lang is the
// translators' template (tools/i18n/lang_table.py writes it from the TR and
// TRF calls); the mod never reads it.
//
// The language: RedemptionEditor.ini Language (auto, the default, is the
// game's language: LOCALIZATION GET_CURRENT_LANGUAGE on the core's thread,
// published to the editor in Snapshot::gameLanguage), then the file of that
// code, then the file of its base language (pt-BR, then pt), then English.
// "pseudo" is the pseudo language of the layout tests: every English text
// accented, bracketed and about 40 % longer.
//
// A loaded table is never freed (a language changes a few times a session at
// most), so a text Tr returned stays valid; the active table is swapped
// atomically, so any thread may call Tr.

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace replay::i18n
{
    // ------------------------------------------------------------------
    // UTF-8
    // ------------------------------------------------------------------
    constexpr std::uint32_t kReplacement = 0xFFFD;

    // The length of the sequence a lead byte starts (1 for a stray byte).
    inline std::size_t SequenceLength(unsigned char lead)
    {
        if (lead < 0xC2)
            return 1; // ASCII, a continuation byte or an overlong lead
        if (lead < 0xE0)
            return 2;
        if (lead < 0xF0)
            return 3;
        if (lead < 0xF5)
            return 4;
        return 1;
    }

    // The code point at `p`, which moves past it; a broken sequence reads as
    // U+FFFD and moves one byte. `end` null: the text ends at its NUL.
    inline std::uint32_t Decode(const char*& p, const char* end = nullptr)
    {
        const unsigned char lead = static_cast<unsigned char>(*p);
        const std::size_t length = SequenceLength(lead);
        if (length == 1)
        {
            ++p;
            return lead < 0x80 ? lead : kReplacement;
        }
        std::uint32_t code = lead & (0xFFu >> (length + 1));
        for (std::size_t n = 1; n < length; ++n)
        {
            const char* at = p + n;
            const unsigned char c = static_cast<unsigned char>(*at);
            if ((end && at >= end) || (c & 0xC0) != 0x80)
            {
                ++p;
                return kReplacement;
            }
            code = (code << 6) | (c & 0x3F);
        }
        const std::uint32_t least = length == 2 ? 0x80 : length == 3 ? 0x800 : 0x10000;
        if (code < least || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
        {
            ++p;
            return kReplacement;
        }
        p += length;
        return code;
    }

    inline void Append(std::string& out, std::uint32_t code)
    {
        if (code < 0x80)
            out += static_cast<char>(code);
        else if (code < 0x800)
        {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
        else if (code < 0x10000)
        {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    // The last character boundary at or before byte `at` (a cut never splits
    // a character).
    inline std::size_t Boundary(const std::string& text, std::size_t at)
    {
        if (at >= text.size())
            return text.size();
        while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80)
            --at;
        return at;
    }

    // `text` into a buffer of `size` bytes, cut on a character's start.
    inline void CopyTo(char* out, std::size_t size, const std::string& text)
    {
        if (!out || !size)
            return;
        const std::size_t length = Boundary(text, std::min(text.size(), size - 1));
        std::memcpy(out, text.data(), length);
        out[length] = '\0';
    }

    // The code points of a text.
    inline std::vector<std::uint32_t> CodePoints(const char* text)
    {
        std::vector<std::uint32_t> out;
        for (const char* p = text; p && *p;)
            out.push_back(Decode(p));
        return out;
    }

    // The capital of a letter of Latin-1, Latin Extended-A and Cyrillic (the
    // title font draws capitals); any other code point as it is.
    inline std::uint32_t ToUpper(std::uint32_t c)
    {
        if (c >= 'a' && c <= 'z')
            return c - 32;
        if (c < 0xE0)
            return c;
        if (c <= 0xFE)
            return c == 0xF7 ? c : c - 32;
        if (c == 0xFF)
            return 0x178;
        if (c >= 0x100 && c <= 0x17F)
        {
            if (c == 0x131)
                return 'I';
            if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
                return (c & 1) ? c : c - 1; // odd capitals
            if (c == 0x130 || c == 0x138 || c == 0x149 || c == 0x178 || c == 0x17F)
                return c;
            return (c & 1) ? c - 1 : c; // even capitals
        }
        if (c >= 0x430 && c <= 0x44F)
            return c - 32;
        if (c >= 0x450 && c <= 0x45F)
            return c - 80;
        if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x4FF))
            return (c & 1) ? c - 1 : c;
        if (c >= 0x4C1 && c <= 0x4CE)
            return (c & 1) ? c : c - 1;
        return c;
    }

    // CJK: the ideographs, kana, hangul and their punctuation (a line may
    // break between two of them; the fonts take them from a Windows face).
    inline bool IsCjk(std::uint32_t c)
    {
        return (c >= 0x1100 && c <= 0x11FF) || (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xA960 && c <= 0xA97F) ||
            (c >= 0xAC00 && c <= 0xD7FF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFFEF);
    }

    // The usual line breaking rules (kinsoku): these never start a line...
    inline bool NoLineStart(std::uint32_t c)
    {
        static const char32_t kChars[] = U"!%),.:;?]}\u3001\u3002\uFF0C\uFF0E\uFF01\uFF1F\uFF1A\uFF1B\uFF09\u300D\u300F\u3011\u3015\u3009\u300B\u3017\u3019"
                                         U"\u30FC\u3005\u309D\u309E\u30FD\u30FE\u3041\u3043\u3045\u3047\u3049\u3063\u3083\u3085\u3087\u308E"
                                         U"\u30A1\u30A3\u30A5\u30A7\u30A9\u30C3\u30E3\u30E5\u30E7\u30EE\u30F5\u30F6\u30FB\u2026\u2025\uFF5D\uFF3D\u201D\u2019";
        for (const char32_t* p = kChars; *p; ++p)
            if (static_cast<std::uint32_t>(*p) == c)
                return true;
        return false;
    }

    // ... and these never end one.
    inline bool NoLineEnd(std::uint32_t c)
    {
        static const char32_t kChars[] = U"([{\uFF08\u300C\u300E\u3010\u3014\u3008\u300A\u3016\u3018\uFF5B\uFF3B\u201C\u2018";
        for (const char32_t* p = kChars; *p; ++p)
            if (static_cast<std::uint32_t>(*p) == c)
                return true;
        return false;
    }

    // ------------------------------------------------------------------
    // A language's table
    // ------------------------------------------------------------------
    class Table
    {
    public:
        std::string code;                  // "pt-BR"; "pseudo"
        std::vector<std::string> problems; // "line 12: no '='" (logged by the loader)
        bool pseudo = false;

        void Set(const std::string& key, const std::string& text)
        {
            m_store.push_back(key);
            const std::string_view view(m_store.back());
            m_store.push_back(text);
            m_index[view] = m_store.back().c_str();
        }

        // The text of `key`, null when the table has none.
        const char* Find(const char* key) const
        {
            const auto it = m_index.find(std::string_view(key));
            return it == m_index.end() ? nullptr : it->second;
        }

        std::size_t Size() const { return m_index.size(); }

        // Every key, in no order (the tests).
        std::vector<std::string> Keys() const
        {
            std::vector<std::string> out;
            for (const auto& entry : m_index)
                out.emplace_back(entry.first);
            return out;
        }

        // f(key, text) for every entry.
        template <class F>
        void Each(F f) const
        {
            for (const auto& entry : m_index)
                f(entry.first, entry.second);
        }

    private:
        std::deque<std::string> m_store; // keys and texts; a deque never moves them
        std::unordered_map<std::string_view, const char*> m_index;
    };

    namespace detail
    {
        inline bool KeyChar(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        }

        inline bool Space(char c) { return c == ' ' || c == '\t'; }

        inline std::string LineProblem(std::size_t line, const char* what)
        {
            return "line " + std::to_string(line) + ": " + what;
        }
    }

    // A .lang text into `out` (its keys added; a later line wins). False when
    // a line was refused (each one in out.problems); the good lines are kept.
    inline bool Parse(const std::string& text, Table& out)
    {
        const std::size_t problemsBefore = out.problems.size();
        std::size_t at = 0;
        if (text.compare(0, 3, "\xEF\xBB\xBF") == 0)
            at = 3; // the BOM a Windows editor may write
        std::size_t lineNumber = 0;
        while (at < text.size())
        {
            std::size_t end = text.find('\n', at);
            if (end == std::string::npos)
                end = text.size();
            std::string line = text.substr(at, end - at);
            at = end + 1;
            ++lineNumber;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            std::size_t p = 0;
            while (p < line.size() && detail::Space(line[p]))
                ++p;
            if (p == line.size() || line[p] == '#')
                continue;
            const std::size_t keyBegin = p;
            while (p < line.size() && detail::KeyChar(line[p]))
                ++p;
            const std::string key = line.substr(keyBegin, p - keyBegin);
            while (p < line.size() && detail::Space(line[p]))
                ++p;
            if (key.empty() || p == line.size() || line[p] != '=')
            {
                out.problems.push_back(detail::LineProblem(lineNumber, key.empty() ? "no key" : "no '=' after the key"));
                continue;
            }
            ++p;
            std::size_t valueEnd = line.size();
            while (p < valueEnd && detail::Space(line[p]))
                ++p;
            while (valueEnd > p && detail::Space(line[valueEnd - 1]))
                --valueEnd;
            bool quoted = false;
            if (valueEnd - p >= 2 && line[p] == '"' && line[valueEnd - 1] == '"')
            {
                quoted = true;
                ++p;
                --valueEnd;
            }
            std::string value;
            bool bad = false;
            for (std::size_t n = p; n < valueEnd; ++n)
            {
                const char c = line[n];
                if (c != '\\')
                {
                    if (c == '"' && quoted)
                        bad = true; // a quote inside a quoted text must be \"
                    value += c;
                    continue;
                }
                if (n + 1 >= valueEnd)
                {
                    bad = true;
                    break;
                }
                const char e = line[++n];
                if (e == 'n')
                    value += '\n';
                else if (e == 't')
                    value += '\t';
                else if (e == '\\' || e == '"')
                    value += e;
                else
                {
                    bad = true;
                    value += '\\';
                    value += e;
                }
            }
            if (bad)
            {
                out.problems.push_back(detail::LineProblem(lineNumber, "a bad escape or quote in the text"));
                continue;
            }
            out.Set(key, value);
        }
        return out.problems.size() == problemsBefore;
    }

    // ------------------------------------------------------------------
    // Values in a text
    // ------------------------------------------------------------------
    struct Arg
    {
        const char* name;
        std::string value;
        Arg(const char* n, std::string v) : name(n), value(std::move(v)) {}
        Arg(const char* n, const char* v) : name(n), value(v ? v : "") {}
    };

    // `text` with each {name} replaced by its value ({{ and }} are braces; a
    // name with no value stays as it is).
    inline std::string Format(const char* text, std::initializer_list<Arg> args)
    {
        std::string out;
        for (const char* p = text; p && *p; ++p)
        {
            if (*p == '{' && p[1] == '{')
            {
                out += '{';
                ++p;
                continue;
            }
            if (*p == '}' && p[1] == '}')
            {
                out += '}';
                ++p;
                continue;
            }
            if (*p == '{')
            {
                const char* close = std::strchr(p + 1, '}');
                if (close)
                {
                    const std::string_view name(p + 1, static_cast<std::size_t>(close - p - 1));
                    const Arg* found = nullptr;
                    for (const Arg& arg : args)
                        if (name == arg.name)
                            found = &arg;
                    if (found)
                    {
                        out += found->value;
                        p = close;
                        continue;
                    }
                }
            }
            out += *p;
        }
        return out;
    }

    // printf into a string (a number for a {value}, as the English formats it).
    inline std::string Printf(const char* format, ...)
    {
        char head[128];
        va_list args;
        va_start(args, format);
        const int length = std::vsnprintf(head, sizeof(head), format, args);
        va_end(args);
        if (length < 0)
            return {};
        if (static_cast<std::size_t>(length) < sizeof(head))
            return std::string(head, static_cast<std::size_t>(length));
        std::string out(static_cast<std::size_t>(length) + 1, '\0');
        va_start(args, format);
        std::vsnprintf(&out[0], out.size(), format, args);
        va_end(args);
        out.resize(static_cast<std::size_t>(length));
        return out;
    }

    // The {names} of a text, in order (the tests: a translation keeps them).
    inline std::vector<std::string> Placeholders(const char* text)
    {
        std::vector<std::string> out;
        for (const char* p = text; p && *p; ++p)
        {
            if ((*p == '{' && p[1] == '{') || (*p == '}' && p[1] == '}'))
            {
                ++p;
                continue;
            }
            if (*p != '{')
                continue;
            const char* close = std::strchr(p + 1, '}');
            if (!close)
                break;
            out.emplace_back(p + 1, close);
            p = close;
        }
        return out;
    }

    // ------------------------------------------------------------------
    // The pseudo language
    // ------------------------------------------------------------------
    constexpr char kPseudoCode[] = "pseudo";

    // "Edit Camera" -> "[Edit Camera ~~]" with every letter that has one in
    // both fonts accented (e with a diaeresis, c with a cedilla...), the
    // {values} kept, and as many '~' as bring it to about 140 % of its
    // length, brackets included, so a cut text shows.
    inline std::string Pseudo(const char* english)
    {
        static const char* const kLower[26] = {"\xC3\xA0", "b", "\xC3\xA7", "\xC4\x8F", "\xC3\xA9", "f", "\xC4\x9F", "h", "\xC3\xAE", "j",
            "\xC4\xB7", "\xC4\xBE", "m", "\xC3\xB1", "\xC3\xB6", "p", "q", "\xC5\x99", "\xC5\xA1", "\xC5\xA3", "\xC3\xBC", "v", "w", "x",
            "\xC3\xBD", "\xC5\xBE"};
        static const char* const kUpper[26] = {"\xC3\x85", "B", "\xC3\x87", "\xC4\x8E", "\xC3\x8B", "F", "\xC4\x9E", "H", "\xC3\x8F", "J",
            "\xC4\xB6", "\xC4\xBD", "M", "\xC3\x91", "\xC3\x96", "P", "Q", "\xC5\x98", "\xC5\xA0", "\xC5\xA2", "\xC3\x9C", "V", "W", "X",
            "\xC3\x9D", "\xC5\xBD"};
        std::string body;
        std::size_t count = 0;
        for (const char* p = english; p && *p;)
        {
            if (*p == '{' && p[1] != '{')
            {
                const char* close = std::strchr(p + 1, '}');
                if (close)
                {
                    body.append(p, close + 1);
                    count += 3; // a value: a few characters on screen
                    p = close + 1;
                    continue;
                }
            }
            const char c = *p;
            if (c >= 'a' && c <= 'z')
                body += kLower[c - 'a'];
            else if (c >= 'A' && c <= 'Z')
                body += kUpper[c - 'A'];
            else
            {
                const char* start = p;
                Decode(p);
                body.append(start, p);
                ++count;
                continue;
            }
            ++p;
            ++count;
        }
        if (body.empty())
            return body;
        const std::size_t target = (count * 14 + 9) / 10;
        const std::size_t tildes = target > count + 3 ? target - count - 3 : 0;
        return "[" + body + (tildes ? " " + std::string(tildes, '~') : std::string()) + "]";
    }

    // The code points past ASCII a language draws, for the fonts
    // (display_font.h SetLanguageGlyphs): its texts', and the letters of its
    // script (Latin-1, or basic Cyrillic), so a name typed in it shows too.
    // Null (English): none.
    inline std::vector<std::uint32_t> LanguageGlyphs(const Table* table)
    {
        std::vector<std::uint32_t> out;
        if (!table)
            return out;
        const auto add = [&out](const char* text) {
            for (const std::uint32_t code : CodePoints(text))
                if (code >= 0x80 && code != kReplacement)
                    out.push_back(code);
        };
        if (table->pseudo)
            add(Pseudo("abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ").c_str());
        table->Each([&add](std::string_view, const char* text) { add(text); });
        bool latin = false, cyrillic = false;
        for (const std::uint32_t code : out)
        {
            latin = latin || (code >= 0xC0 && code <= 0x24F);
            cyrillic = cyrillic || (code >= 0x400 && code <= 0x4FF);
        }
        if (latin)
            for (std::uint32_t code = 0xC0; code <= 0xFF; ++code)
                if (code != 0xD7 && code != 0xF7)
                    out.push_back(code);
        if (cyrillic)
        {
            for (std::uint32_t code = 0x410; code <= 0x44F; ++code)
                out.push_back(code);
            out.push_back(0x401);
            out.push_back(0x451);
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    // ------------------------------------------------------------------
    // The active language
    // ------------------------------------------------------------------
    namespace detail
    {
        inline std::atomic<const Table*> g_active{nullptr};
        inline std::mutex g_mutex; // the kept tables and the pseudo texts
        inline std::vector<std::unique_ptr<Table>>& Kept()
        {
            static std::vector<std::unique_ptr<Table>> tables;
            return tables;
        }
        inline std::unordered_map<std::string, std::string>& PseudoTexts()
        {
            static std::unordered_map<std::string, std::string> texts;
            return texts;
        }
    }

    // `table` becomes the language (null: English). Kept until the process ends.
    inline void SetActive(std::unique_ptr<Table> table)
    {
        const Table* raw = table.get();
        if (table)
        {
            std::lock_guard<std::mutex> lock(detail::g_mutex);
            detail::Kept().push_back(std::move(table));
        }
        detail::g_active.store(raw, std::memory_order_release);
    }

    inline const Table* Active() { return detail::g_active.load(std::memory_order_acquire); }

    // "en" when English is active.
    inline const char* ActiveCode()
    {
        const Table* table = Active();
        return table ? table->code.c_str() : "en";
    }

    // The text of `key` in the active language, else the English.
    inline const char* Tr(const char* key, const char* english)
    {
        const Table* table = Active();
        if (!table)
            return english;
        if (table->pseudo)
        {
            std::lock_guard<std::mutex> lock(detail::g_mutex);
            auto& texts = detail::PseudoTexts();
            auto it = texts.find(english);
            if (it == texts.end())
                it = texts.emplace(english, Pseudo(english)).first;
            return it->second.c_str();
        }
        const char* text = table->Find(key);
        return text ? text : english;
    }

    // ------------------------------------------------------------------
    // Choosing the language
    // ------------------------------------------------------------------
    // A language of GET_CURRENT_LANGUAGE (0 american ... 12 chinesesimp) as a
    // file code; English for an id the list does not have.
    inline const char* GameLanguageCode(std::uint32_t id)
    {
        static const char* const kCodes[] = {"en", "fr", "de", "it", "es", "pt-BR", "pl", "ru", "ko", "zh-TW", "ja", "es-MX", "zh-CN"};
        return id < sizeof(kCodes) / sizeof(kCodes[0]) ? kCodes[id] : "en";
    }

    // "PT_br " -> "pt-BR": the language lower case, a region upper case.
    inline std::string Normalize(const std::string& code)
    {
        std::string out;
        bool region = false;
        for (const char c : code)
        {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                continue;
            if (c == '-' || c == '_')
            {
                region = true;
                out += '-';
                continue;
            }
            out += region ? static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c) : static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        }
        return out;
    }

    // The ini's Language (empty or auto: the game's), and the game's language
    // (Snapshot::gameLanguage: GET_CURRENT_LANGUAGE + 1, 0 not known yet).
    inline std::string Choose(const std::string& setting, std::uint32_t gameLanguage)
    {
        const std::string code = Normalize(setting);
        if (code == "zh-HANS")
            return "zh-CN"; // the script's name for the game's file code
        if (code == "zh-HANT")
            return "zh-TW";
        if (!code.empty() && code != "auto")
            return code;
        return gameLanguage ? GameLanguageCode(gameLanguage - 1) : "en";
    }

    // The files a code reads, best first: "pt-BR" -> pt-BR, pt.
    inline std::vector<std::string> FileCandidates(const std::string& code)
    {
        std::vector<std::string> out;
        if (code.empty() || code == "en" || code == kPseudoCode)
            return out;
        out.push_back(code);
        const std::size_t dash = code.find('-');
        if (dash != std::string::npos && dash > 0)
            out.push_back(code.substr(0, dash));
        return out;
    }

    // The table of `code`: "en" and an unknown code with no file give null
    // (English); "pseudo" the pseudo language; else the first file of
    // FileCandidates in `folder` (with its trailing backslash). `loaded` says
    // which file was read (empty: none).
    inline std::unique_ptr<Table> Load(const std::wstring& folder, const std::string& code, std::string* loaded = nullptr)
    {
        if (loaded)
            loaded->clear();
        if (code == kPseudoCode)
        {
            auto table = std::make_unique<Table>();
            table->code = kPseudoCode;
            table->pseudo = true;
            return table;
        }
        for (const std::string& candidate : FileCandidates(code))
        {
            const std::wstring path = folder + std::wstring(candidate.begin(), candidate.end()) + L".lang";
            FILE* file = nullptr;
            if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
                continue;
            std::string text;
            char buffer[4096];
            std::size_t read = 0;
            while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0 && text.size() < 4u * 1024 * 1024)
                text.append(buffer, read);
            std::fclose(file);
            auto table = std::make_unique<Table>();
            table->code = candidate;
            Parse(text, *table);
            if (loaded)
                *loaded = candidate + ".lang";
            return table;
        }
        return nullptr;
    }
}

// A text the player sees: its key in lang\<code>.lang and its English.
#define TR(key, english) ::replay::i18n::Tr(key, english)
// The same with {named} values: TRF("k", "{n} clips", {{"n", count}}) (a std::string).
#define TRF(key, english, ...) ::replay::i18n::Format(::replay::i18n::Tr(key, english), __VA_ARGS__)
// A key and its English in a table of texts, looked up where they are drawn
// (TR(entry.key, entry.label)): expands to `key, english`.
#define TR_NOOP(key, english) key, english
