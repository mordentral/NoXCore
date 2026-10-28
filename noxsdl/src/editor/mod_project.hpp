// A mod project in the map editor: a mod's folder,
// mods\<id>\ beside noxcore.exe, the folder the game loads, so a saved project is playable at
// once. New mod copies a small template into it; Open lists the folders in mods\; a reference mod
// (mode.json "reference": true: Arena's and CTF's) opens as a copy, never in place.
//
// The project is its folder's files (server/, ui/, assets/ and anything else) and its mode.json,
// edited through a form of the fields the game reads (ModManifest) while every other field stays
// as the file had it, in its place (ordered_json.hpp). Every path is '/' separated and kept inside
// the folder. No SDL or ImGui: the Mod windows draw it (mod_panel.hpp), the tests use it alone.
#pragma once

#include "editor/ordered_json.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nox::modes {
class Registry;
}

namespace noxsdl::editor {

/// One of mode.json's "options" (a setting shown under the mode's row in Host Custom).
struct ModOption {
    enum class Kind : std::uint8_t { Toggle, Choice, Number, Text };
    std::string key;
    std::string label;
    Kind kind = Kind::Toggle;
    std::vector<std::string> choices; ///< (Choice)
    int min = 0, max = 100;           ///< (Number)
    int defaultValue = 0;             ///< (Toggle 0/1, Choice an index, Number a value)
    bool operator==(const ModOption&) const = default;
};

/// mode.json's fields as the form edits them (modes/script/manifest.hpp says what each does).
struct ModManifest {
    std::string id;
    std::string name;
    std::string description;
    int minPlayers = 1, maxPlayers = 16;
    bool stockClients = true;
    bool generatedLevels = false;           ///< "levels": "generated" (else "mapFiles")
    std::string serverScript = "server/main.luau";
    std::string uiScript;                   ///< empty: none
    int uiApi = 1;                          ///< the UI API it asks for (1 to 3)
    std::string looksLike;                  ///< "", "arena" or "ctf"
    /// "mapPrefix": 2-3 capital letters its generated maps' names start with ("DSC" -> "DSC_..."),
    /// "" none. (The form checks it: mapPrefixProblem.)
    std::string mapPrefix;
    std::vector<ModOption> options;
    bool operator==(const ModManifest&) const = default;
};

/// The form's fields from mode.json's value (what isn't there: the defaults).
ModManifest manifestForm(const json::Value& doc);
/// The form's fields into mode.json's value: a field the form changed is set in place (a new one
/// at its usual place among the others); every other member, unknown ones too, kept as it was.
void applyManifestForm(json::Value& doc, const ModManifest& form);

class ModProject {
public:
    /// A file's size cap (the game reads at most 1 MB a file: ModPackage::read), and the most
    /// files a project lists.
    static constexpr std::uintmax_t kMaxFileBytes = 1u << 20;
    static constexpr std::size_t kMaxEntries = 1024;

    struct Entry {
        std::string path;        ///< '/' separated, from the folder
        bool folder = false;
        std::uintmax_t size = 0; ///< (files)
        bool operator==(const Entry&) const = default;
    };

    /// The project in `folder` (its mode.json read; none or bad JSON: the error).
    static std::optional<ModProject> open(const std::filesystem::path& folder, std::string& error);
    /// A new mod from the template in `modsDir`/`id` (the folder mustn't be there yet).
    static std::optional<ModProject> create(const std::filesystem::path& modsDir, const std::string& id, const std::string& name,
                                            std::string& error);
    /// A copy of the mod in `source` as `modsDir`/`id` (a reference mod's way in): its files
    /// copied, mode.json's id and name made the copy's, its "reference" mark taken off.
    static std::optional<ModProject> copyOf(const std::filesystem::path& source, const std::filesystem::path& modsDir,
                                            const std::string& id, const std::string& name, std::string& error);

    const std::filesystem::path& folder() const noexcept { return folder_; }
    /// The folder's name (the id the game registers it under is mode.json's: idMismatch says).
    std::string folderName() const;

    // ---- mode.json
    json::Value& manifestDoc() noexcept { return manifest_; }
    const json::Value& manifestDoc() const noexcept { return manifest_; }
    ModManifest form() const { return manifestForm(manifest_); }
    void setForm(const ModManifest& form) { applyManifestForm(manifest_, form); }
    /// mode.json's text as it would be saved.
    std::string manifestText() const { return json::write(manifest_); }
    /// What the game would say of it ("" when it loads: modes/script readManifest).
    std::string manifestProblem() const;
    /// mode.json written; false: why in `error`.
    bool saveManifest(std::string& error) const;

    // ---- files (relative paths, '/' separated, inside the folder)
    /// Every file and folder, folders before their files, sorted; at most kMaxEntries, 8 deep.
    std::vector<Entry> files() const;
    std::optional<std::string> read(std::string_view rel) const;
    bool write(std::string_view rel, std::string_view text, std::string& error) const;
    /// A new file (its folders made), refused when it's there already.
    bool newFile(std::string_view rel, std::string_view text, std::string& error) const;
    bool newFolder(std::string_view rel, std::string& error) const;
    /// A file or folder renamed or moved inside the project; mode.json's "server" / "ui" follow a
    /// script they name (saved).
    bool rename(std::string_view from, std::string_view to, std::string& error);
    /// A file or folder (and what's in it) deleted; mode.json itself never.
    bool remove(std::string_view rel, std::string& error) const;

    /// Whether `rel` is a path the project takes: '/' separated parts, none empty, "." or "..",
    /// no drive, '\\' or characters Windows refuses in names; at most 128 bytes.
    static bool validPath(std::string_view rel);
    /// `rel` under the folder; none: not a valid path, or one that leads out of the folder.
    std::optional<std::filesystem::path> pathOf(std::string_view rel) const;

private:
    ModProject() = default;
    std::filesystem::path folder_;
    json::Value manifest_;
};

/// A folder in mods\ as Open lists it.
struct ModListing {
    std::filesystem::path folder;
    std::string id;          ///< mode.json's (else the folder's name)
    std::string name;        ///< mode.json's (else the id)
    bool reference = false;  ///< opens as a copy
    std::string problem;     ///< why the game doesn't load it ("" when it does)
    std::string mapPrefix;   ///< its mode.json's "mapPrefix" ("" none)
};
/// Each folder in `modsDir`, sorted by folder name.
std::vector<ModListing> listMods(const std::filesystem::path& modsDir);
/// An id for a copy of `id` that no folder in `modsDir` has yet ("deep_delve_copy", "..._copy2").
std::string freeCopyId(const std::filesystem::path& modsDir, const std::string& id);

/// What's wrong with a mod's map prefix ("" when it's fine, or none): not 2-3 of A-Z, or one
/// another mod in `modsDir` (not the folder `self`) names, or one whose maps another registered
/// mode (not `selfId`) calls its generated ones (its ownsMap, asked of "<prefix>_1").
std::string mapPrefixProblem(std::string_view prefix, const std::filesystem::path& modsDir, const std::filesystem::path& self,
                             std::string_view selfId, const nox::modes::Registry& registry);

} // namespace noxsdl::editor
