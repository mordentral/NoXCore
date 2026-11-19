// NoX Core's own text's language, once at start (the game client's and the Content Editor's):
// noxcore.cfg's Language, else nox.csf's, else the system's (SDL's preferred locales), else English
// (nox::l10n::chooseLanguage), from the lang folder beside the program. Logged.
#pragma once

#include "noxcore/strings/l10n.hpp"
#include "noxcore/strings/string_table.hpp"

#include <filesystem>
#include <string_view>

namespace noxsdl {

/// The lang folder beside the program.
std::filesystem::path languageDirectory();

/// Chooses and sets the language (nox::l10n::setLanguage). `csf` may be null; `codepage` is the
/// text convention its text was read under.
nox::l10n::LoadReport pickLanguage(std::string_view configured, const nox::strings::StringTable* csf,
                                   nox::strings::LegacyCodepage codepage);

/// The same for a program that hasn't read the game's files yet (the Content Editor): the game
/// folder's noxcore.cfg (Language, CsfText) and nox.csf read for it.
nox::l10n::LoadReport pickLanguageFor(const std::filesystem::path& gameDir);

} // namespace noxsdl
