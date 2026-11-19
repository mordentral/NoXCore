// NoX Core's own text in the player's language (the game's own comes from nox.csf).
//
// English is the source: each piece of text is written where it is used, with a stable key,
//   w->text = NOX_TR("menus.main.game_modes", "Game Modes");
// and the language files (lang/<code>.txt beside the program, UTF-8) give it in other languages:
//   #@ 3f1c2a9b mt: Game Modes
//   menus.main.game_modes = Spielmodi
// A key a file lacks, or whose "#@" hash isn't the English's (the English changed since it was
// translated: stale), shows the English. The key and its English must be literals at the call site:
// noxsdl/l10n/l10n.py reads them from the source (the English catalog, lang/en.txt, and the checks
// that keep every language current). Not translated: log lines, the developer console, command
// line help, and text whose bytes a protocol or file format fixes.
//
// Each call site resolves once: its macro keeps a reference to the key's slot (a static, made the
// first time it runs), and the slot holds the text in the language in use. setLanguage rewrites
// every slot once; nothing is looked up per frame. Text already copied into a window keeps the
// language it was copied in (the language is chosen at start, before the menus are built).
//
// Placeholders: {name} (a-z, 0-9 and _), filled by format() by name, so a translation may put them
// in any order. Plurals: NOX_TRP's forms, chosen by the language's rule (one, few, many, other).
// Dear ImGui labels: NOX_TRI keeps the widget's id the English one ("Datei###File"), so a
// translation never changes which widget is which, nor a saved layout.
#pragma once

#include <array>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nox::l10n {

/// Plural categories, as the language files name them ([one], [few], [many], [other]).
enum class PluralForm : std::uint8_t { One, Few, Many, Other };
inline constexpr std::size_t kPluralForms = 4;

/// One piece of text: its key, its English and what is shown now.
struct Slot {
    std::string id;
    std::string en;      ///< the English (a plural's [one] form)
    std::string enOther; ///< a plural's English [other] form; empty otherwise
    bool plural = false;
    std::uint32_t enHash = 0; ///< hashEnglish(en[, enOther])
    std::string text;         ///< shown: the translation, or the English
    std::string imgui;        ///< as a Dear ImGui label: text, with "###<English>" when they differ
    std::array<std::string, kPluralForms> forms; ///< a plural's shown forms (English's in one and other)
    bool translated = false;
};

/// The short hash of an English text that a language file's "#@" line keeps (FNV-1a, 32 bits): a
/// plural's two forms joined by U+001F.
std::uint32_t hashEnglish(std::string_view en, std::string_view enOther = {});
/// As 8 lower-case hex digits.
std::string hashText(std::uint32_t hash);

/// The slot for a key, made on first use (from its English). Thread-safe; the reference stays
/// valid for the program's life. A key met again with other English keeps its first.
const Slot& slot(std::string_view id, std::string_view en);
const Slot& pluralSlot(std::string_view id, std::string_view one, std::string_view other);

/// A key and its English as data (a table's entries), for text picked at run time: resolved with
/// tr(), a lookup, so resolve them when a window is built, never in a frame's drawing.
struct Key {
    const char* id;
    const char* en;
};
const std::string& tr(const Key& key);
const char* trImGui(const Key& key);
/// A plural's key and English forms as data (NOX_TRPK), resolved as Key is.
struct PluralKey {
    const char* id;
    const char* one;
    const char* other;
};
const Slot& slotOf(const PluralKey& key);
inline const Slot& slotOf(const Key& key) { return slot(key.id, key.en); }

/// The form of a plural for `n` under the language in use.
const std::string& plural(const Slot& s, long long n);
/// The category of `n` in a language ("ru", "pl", "cs": one, few, many; "fr": 0 and 1 are one).
PluralForm pluralForm(std::string_view language, long long n);

/// A placeholder's value for format().
struct Arg {
    std::string_view name;
    std::string value;
    Arg(std::string_view n, std::string_view v) : name(n), value(v) {}
    Arg(std::string_view n, const std::string& v) : name(n), value(v) {}
    Arg(std::string_view n, const char* v) : name(n), value(v ? v : "") {}
    template <std::integral T>
    Arg(std::string_view n, T v) : name(n), value(std::to_string(v))
    {
    }
};
/// `pattern` with each {name} replaced by its Arg's value. A {name} without an Arg, and any other
/// brace, stays as it is.
std::string format(std::string_view pattern, std::initializer_list<Arg> args);
/// The same with the values made at run time.
std::string format(std::string_view pattern, std::span<const Arg> args);
/// The {name} placeholders in a text, in order (the checks compare them).
std::vector<std::string> placeholders(std::string_view text);

// ------------------------------------------------------------------- language files

/// One entry of a language file.
struct Entry {
    std::optional<std::uint32_t> hash; ///< its "#@" line's: the English it was translated from
    bool machine = false;              ///< flagged "mt": a machine translation not yet reviewed
    std::string text;                  ///< the plain entry's text
    std::array<std::string, kPluralForms> forms; ///< a plural's [one] [few] [many] [other]
    bool plural = false;
    int line = 0;
};

/// A language file read: entries by key, and what couldn't be read (line: why).
struct Catalog {
    std::unordered_map<std::string, Entry> entries;
    std::vector<std::string> errors;
};

/// Reads a language file's text (UTF-8, a BOM allowed). Lines: "# comment", "#@ <hash> [mt]: <English>"
/// (for the entry after it), "key = text" and "key[form] = text". In a text, \n is a new line, \t a
/// tab, \s a space (for one at either end: the text is trimmed), \\ a backslash.
Catalog parseCatalog(std::string_view fileText);
/// A text as a language file writes it (parseCatalog's escapes).
std::string escapeText(std::string_view text);

/// What setLanguage did, for the log.
struct LoadReport {
    std::string language;
    std::filesystem::path file;
    bool loaded = false;      ///< the file was there and read
    std::size_t entries = 0;  ///< in the file
    std::size_t applied = 0;  ///< slots shown translated (so far: slots are made as text is first used)
    std::size_t stale = 0;    ///< entries left English because the English changed since
    std::vector<std::string> errors;
    double milliseconds = 0;  ///< reading and applying
};

/// The language in use: "en" (the built-in English, the default) or a code whose file
/// `dir`/<code>.txt is read. Every slot made so far is rewritten, and later ones follow it. Call it
/// once at start, before the menus are built.
LoadReport setLanguage(std::string_view code, const std::filesystem::path& dir);
/// The same from a catalog already read (tests).
LoadReport setLanguage(std::string_view code, Catalog catalog);
std::string_view language();
/// Bumped by every setLanguage (for caches of text made in the language in use).
std::uint64_t languageRevision();

/// The languages whose files are in `dir` (their codes, sorted), "en" always among them.
std::vector<std::string> availableLanguages(const std::filesystem::path& dir);

/// A language's own name ("Deutsch"), or the code.
std::string_view languageName(std::string_view code);

/// Which language to use, and why (for the log): noxcore.cfg's Language when it names one we have
/// ("en" included); else the one nox.csf is in, when it isn't English and we have it; else the
/// first of the system's UI languages we have; else English.
struct LanguageChoice {
    std::string code = "en";
    std::string reason;
};
LanguageChoice chooseLanguage(std::string_view configured, std::string_view csfLanguage,
                              std::span<const std::string> systemLanguages, std::span<const std::string> available);
/// A locale name to its language code: "de_DE.UTF-8", "de-AT" and "DE" to "de".
std::string languageOfLocale(std::string_view locale);

} // namespace nox::l10n

// The call sites. `id` and `en` must be string literals (noxsdl/l10n/l10n.py reads them).
/// The text, as const std::string&.
#define NOX_TR(id, en)                                                                                                 \
    ([]() -> const ::std::string& {                                                                                    \
        static const ::nox::l10n::Slot& nox_l10n_slot_ = ::nox::l10n::slot(id, en);                                   \
        return nox_l10n_slot_.text;                                                                                    \
    }())
/// The text, as const char*.
#define NOX_TRC(id, en) (NOX_TR(id, en).c_str())
/// A Dear ImGui widget's or window's label (const char*): the translation with the English as its id.
#define NOX_TRI(id, en)                                                                                                \
    ([]() -> const char* {                                                                                             \
        static const ::nox::l10n::Slot& nox_l10n_slot_ = ::nox::l10n::slot(id, en);                                   \
        return nox_l10n_slot_.imgui.c_str();                                                                           \
    }())
/// A plural's form for `n` (const std::string&; fill its {n} with format()).
#define NOX_TRP(id, one, other, n)                                                                                     \
    (::nox::l10n::plural(                                                                                              \
        []() -> const ::nox::l10n::Slot& {                                                                             \
            static const ::nox::l10n::Slot& nox_l10n_slot_ = ::nox::l10n::pluralSlot(id, one, other);                 \
            return nox_l10n_slot_;                                                                                     \
        }(),                                                                                                           \
        static_cast<long long>(n)))
/// A key and its English as data (Key), for tables: tr(key) when the window is built.
#define NOX_TRK(id, en) (::nox::l10n::Key{id, en})
/// A plural's key and English forms as data (PluralKey): plural(slotOf(key), n) when used.
#define NOX_TRPK(id, one, other) (::nox::l10n::PluralKey{id, one, other})
