// NoX Core's own text's language, once at start (language_pick.hpp).
#include "language_pick.hpp"

#include "noxcore/strings/csf_language.hpp"
#include "noxcore/util/text.hpp"

#include <SDL3/SDL.h>

#include <chrono>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace noxsdl {

std::filesystem::path languageDirectory()
{
    const char* base = SDL_GetBasePath();
    return std::filesystem::path(reinterpret_cast<const char8_t*>(base ? base : "")) / "lang";
}

nox::l10n::LoadReport pickLanguage(std::string_view configured, const nox::strings::StringTable* csf,
                                   nox::strings::LegacyCodepage codepage)
{
    const auto start = std::chrono::steady_clock::now();
    std::string csfLanguage;
    if (csf) {
        const auto g = nox::strings::guessCsfLanguage(*csf, codepage);
        csfLanguage = g.code;
        SDL_Log("nox.csf's language: %s (%s; %zu words, %.1f%% its own, %.1f%% the next)",
                g.code.empty() ? "unknown" : g.code.c_str(), g.reason, g.words, g.share * 100.0, g.runnerUp * 100.0);
    }
    std::vector<std::string> system;
    int count = 0;
    if (SDL_Locale** locales = SDL_GetPreferredLocales(&count)) {
        for (int i = 0; i < count && locales[i]; ++i) {
            if (locales[i]->language) {
                system.emplace_back(locales[i]->language);
            }
        }
        SDL_free(locales);
    }
    const std::filesystem::path dir = languageDirectory();
    const auto available = nox::l10n::availableLanguages(dir);
    const auto choice = nox::l10n::chooseLanguage(configured, csfLanguage, system, available);
    auto report = nox::l10n::setLanguage(choice.code, dir);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::string have;
    for (const auto& a : available) {
        have += (have.empty() ? "" : " ") + a;
    }
    if (choice.code == "en") {
        SDL_Log("language: English (%s; have: %s; %.1f ms)", choice.reason.c_str(), have.c_str(), ms);
    } else {
        SDL_Log("language: %s (%s): lang/%s.txt, %zu entries, %zu errors (%.1f ms)",
                std::string(nox::l10n::languageName(choice.code)).c_str(), choice.reason.c_str(), choice.code.c_str(),
                report.entries, report.errors.size(), ms);
        for (std::size_t i = 0; i < report.errors.size() && i < 10; ++i) {
            SDL_Log("lang/%s.txt:%s", choice.code.c_str(), report.errors[i].c_str());
        }
    }
    return report;
}

nox::l10n::LoadReport pickLanguageFor(const std::filesystem::path& gameDir)
{
    std::string configured, csfText;
    if (std::ifstream in(gameDir / "noxcore.cfg"); in) {
        std::string line;
        while (std::getline(in, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string name(nox::util::trim(std::string_view(line).substr(0, eq)));
            const std::string value = nox::util::lowered(nox::util::trim(std::string_view(line).substr(eq + 1)));
            if (name == "Language") configured = value;
            else if (name == "CsfText") csfText = value;
        }
    }
    std::optional<nox::strings::StringTable> table;
    try {
        table = nox::strings::StringTable::load(gameDir / "nox.csf");
    } catch (const std::exception& ex) {
        SDL_Log("nox.csf: %s (its language unknown)", ex.what());
    }
    auto codepage = nox::strings::LegacyCodepage::Latin1;
    if (csfText.starts_with("cp1251")) {
        codepage = nox::strings::LegacyCodepage::Cp1251;
    } else if (table && csfText != "latin1") {
        codepage = nox::strings::guessLegacyCodepage(*table).codepage;
    }
    return pickLanguage(configured, table ? &*table : nullptr, codepage);
}

} // namespace noxsdl
