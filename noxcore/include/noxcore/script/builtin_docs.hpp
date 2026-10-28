// The NoxScript builtins' documentation, for the map editor's Script Reference: a tooltip line
// and a long description of each builtin (src/script/builtin_docs.txt, written from what the
// server's script host does), and how the stock maps use it (src/script/builtin_usage.txt,
// made by builtinUsageReport over the game's maps). Both files are compiled in.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace nox::script {

struct BuiltinParamDoc {
    std::string type; ///< the compiler's ("int", "object"...)
    std::string name;
    std::string text;
};

struct BuiltinDoc {
    std::uint32_t id = 0;
    std::string name;
    std::string prototype; ///< with parameter names: "int SecondTimer(int seconds, function callback)"
    std::string category;
    std::vector<std::string> related;
    std::string shortText;   ///< the tooltip
    std::string description; ///< paragraphs, "\n\n" between them; "- " lines are bullets
    std::vector<BuiltinParamDoc> params;
    std::string returns;
    std::string edge;
    std::string example; ///< NoxScript, as written
    // How the stock maps use it (builtin_usage.txt).
    int usageMaps = 0;
    int usageCalls = 0;
    std::vector<std::string> usageExamples; ///< up to 5 maps, the most calls first
};

struct BuiltinDocSet {
    std::vector<BuiltinDoc> docs;     ///< by builtin id; an id without an entry has an empty name
    std::vector<std::string> errors;  ///< what's wrong with the text (tests: none)
    int usageScripts = 0;             ///< how many stock map scripts the usage counted
};

/// Parses the documentation text and the usage text (their formats are in the files' headers),
/// checking each entry against the compiler's builtin table.
BuiltinDocSet parseBuiltinDocs(std::string_view docsText, std::string_view usageText);

/// The compiled-in documentation (parsed once).
const BuiltinDocSet& builtinDocs();
/// Builtin `id`'s entry, or null if it has none.
const BuiltinDoc* builtinDoc(std::uint32_t id);

std::string_view embeddedBuiltinDocsText();
std::string_view embeddedBuiltinUsageText();

/// The prototype the compiler's table gives builtin `id`, with no parameter names
/// ("int SecondTimer(int, function)").
std::string builtinTypePrototype(std::uint32_t id);
/// A prototype with its parameter names taken out ("int F(int a, float b)" -> "int F(int, float)").
std::string stripParamNames(std::string_view prototype);

/// builtin_usage.txt for the maps under `mapsDir` (each map in a folder of its name): every map's
/// script decompiled, each builtin's calls in the source counted, with the maps that use it. The
/// count from the source is checked against the script's own CallBuiltin instructions; a map
/// where they differ goes into `errors`.
std::string builtinUsageReport(const std::filesystem::path& mapsDir, std::vector<std::string>* errors = nullptr);

/// A paragraph text as display lines: soft-wrapped lines joined, paragraphs kept, bullets kept
/// ("- " lines start a new line).
std::string joinParagraphs(std::string_view text);

} // namespace nox::script
