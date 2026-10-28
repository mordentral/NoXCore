// The content pack being edited: the game's own text tables (never written), the pack's changes
// to them field by field, the tables as they read with those changes, and saving the changes as
// the pack's patch files (mods\<id>\content\*.patch) or exporting whole files.
#pragma once

#include "content/schema.hpp"

#include "noxcore/content/content_pack.hpp"
#include "noxcore/content/csf_document.hpp"
#include "noxcore/content/text_table.hpp"
#include "noxcore/content/thing_patch.hpp"
#include "noxcore/content/thing_writer.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace videotools::content {

using nox::content::GameKind;
using nox::content::TableFormat;
using nox::content::TextTable;

/// One change: a record's field (of one kind of game) set to a value, or its lines removed.
struct FieldEdit {
    TableFormat format = TableFormat::Monster;
    std::string section; ///< modifier.bin's
    std::string record;  ///< gamedata.bin: ""
    std::string key;
    GameKind kind = GameKind::Both;
    std::optional<std::string> value; ///< nullopt: removed
    bool operator==(const FieldEdit&) const = default;

    bool sameTarget(const FieldEdit& o) const
    {
        return format == o.format && section == o.section && record == o.record && key == o.key && kind == o.kind;
    }
    /// "GruntAxe.SOLO SPEED", "FireballSpeedCoeff", "WEAPON_DEFINITIONS/Longsword.DAMAGE_MIN".
    std::string label() const;
};

/// A change to a thing.bin type: an attribute line (or MENUICON / PRETTYIMAGE) set, or removed.
struct ThingEdit {
    std::string type;
    std::string key;
    std::optional<std::string> value; ///< nullopt: removed
    bool sameTarget(const ThingEdit& o) const { return type == o.type && key == o.key; }
    std::string label() const { return type + "." + key; }
};

/// A type the pack adds to thing.bin (its NEW record): a copy of `like` as thing.bin has it, then
/// its own ThingEdits. It goes after every type there is, in the order the pack adds them.
struct NewThing {
    std::string name;
    std::string like;
    bool operator==(const NewThing&) const = default;
};

/// A label's texts in nox.csf as the pack has them (or the label removed).
struct StringEdit {
    std::string label;
    std::vector<std::u16string> variants; ///< empty: removed
    std::optional<std::string> speech;    ///< the first variant's speech file, when the pack sets it
};

/// A sound file the pack brings: content\audio\<name>.wav (an Audio.bag sample) or
/// content\dialog\<name>.wav (a spoken line), from `source` (empty: removed from the pack).
struct SoundEdit {
    enum Folder : std::uint8_t { Audio, Dialog } folder = Audio;
    std::string name; ///< without .wav
    std::filesystem::path source;
    bool saved = false; ///< already in the pack's folder (as read, or saved)
    std::string label() const { return std::string(folder == Audio ? "audio\\" : "dialog\\") + name + ".wav"; }
};

class PackEditor {
public:
    /// The game folder's tables (each one that fails is left out, with why in errors()).
    void openBase(const std::filesystem::path& gameDir);
    /// The pack to edit: `contentDir` is mods\<id>\content (it needn't exist yet). Its patch files'
    /// changes become the edits. Returns problems reading them.
    std::vector<std::string> openPack(const std::filesystem::path& contentDir, std::string id);
    void closePack();

    bool hasBase(TableFormat f) const { return base_[index(f)].has_value(); }
    const TextTable* base(TableFormat f) const { return base_[index(f)] ? &*base_[index(f)] : nullptr; }
    /// The table as it reads with the pack's changes.
    const TextTable* working(TableFormat f) const;
    const std::vector<std::string>& errors() const { return errors_; }

    bool packOpen() const { return !contentDir_.empty(); }
    const std::string& packId() const { return packId_; }
    const std::filesystem::path& contentDir() const { return contentDir_; }

    const std::vector<FieldEdit>& edits() const { return edits_; }
    bool dirty() const { return dirty_; }
    /// Sets a field; an edit that gives back the game's own value is dropped.
    /// False (nothing changed) for a value the table's reader can't take back (TextTable::valueFor).
    bool set(TableFormat f, std::string_view section, std::string_view record, std::string_view key, GameKind kind, std::string_view value);
    void remove(TableFormat f, std::string_view section, std::string_view record, std::string_view key, GameKind kind);
    void revert(std::size_t edit);
    void revertAll();
    /// The edit of that field (nullptr if it has none).
    const FieldEdit* editOf(TableFormat f, std::string_view section, std::string_view record, std::string_view key, GameKind kind) const;

    /// The patch file's text for a format (empty: no changes to it).
    std::string patchText(TableFormat f) const;
    /// Writes the pack's patch files into contentDir (removing those it no longer needs). Throws.
    void save();
    /// Writes whole files (the game's own format, encrypted) for every changed table into `dir`.
    /// Returns the files written. Throws.
    std::vector<std::filesystem::path> exportFull(const std::filesystem::path& dir) const;

    // --- thing.bin (thing.patch) ---
    const nox::things::ThingDb* baseThings() const { return baseThings_ ? &*baseThings_ : nullptr; }
    /// The types as they read with the pack's changes.
    const nox::things::ThingDb* workingThings() const;
    const std::vector<ThingEdit>& thingEdits() const { return thingEdits_; }
    /// Sets an attribute of a type (every line of the key, as thing.patch does); one that gives
    /// back the game's own value drops its edit. Both check the line as thing.patch's loader
    /// does (keys spelt as GAME.EXE's readers have them, values as it reads them, no MENUICON
    /// added or taken away) and return what's wrong ("" when made).
    std::string setThing(std::string_view type, std::string_view key, std::string_view value);
    std::string removeThing(std::string_view type, std::string_view key);
    void revertThing(std::size_t edit);
    const ThingEdit* thingEditOf(std::string_view type, std::string_view key) const;
    std::string thingPatchText() const;
    /// A type's edit's reach (a new type's: structural).
    Reach reachOf(const ThingEdit& e) const;

    /// The types the pack adds, in order (their numbers: thing.bin's count + 1 on).
    const std::vector<NewThing>& newThings() const { return newThings_; }
    /// A new type made like `like` (a type thing.bin has, or one the pack adds before it): "" when
    /// added, or what's wrong (its name taken, not a name a type can have, no such type...).
    std::string addThing(std::string_view name, std::string_view like);
    /// Takes a new type out of the pack, its edits with it: "" or why not (another new type is made
    /// like it).
    std::string removeNewThing(std::string_view name);
    bool isNewThing(std::string_view name) const;
    /// What thing.patch's loader would refuse in the pack's types as they stand (it would then
    /// leave the whole file out).
    const std::vector<std::string>& thingProblems() const
    {
        workingThings();
        return thingProblems_;
    }
    // --- nox.csf (strings.patch) ---
    const nox::content::CsfDocument* baseStrings() const { return baseStrings_ ? &*baseStrings_ : nullptr; }
    const nox::content::CsfDocument* workingStrings() const;
    const std::vector<StringEdit>& stringEdits() const { return stringEdits_; }
    /// Sets a label's texts (and its speech file); the game's own drops the edit.
    /// Checked as strings.patch's loader checks it (the game's limits, the label's printf codes):
    /// what's wrong, or "" when made.
    std::string setStrings(std::string_view label, std::vector<std::u16string> variants, std::optional<std::string> speech = std::nullopt);
    void removeString(std::string_view label);
    void revertString(std::size_t edit);
    const StringEdit* stringEditOf(std::string_view label) const;
    std::string stringsPatchText() const;

    // --- sound files (content\audio, content\dialog) ---
    const std::vector<SoundEdit>& soundEdits() const { return soundEdits_; }
    /// The pack's .wav for a sample or a spoken line: `source` copied in when the pack is saved.
    void setSound(SoundEdit::Folder folder, std::string_view name, const std::filesystem::path& source);
    void revertSound(std::size_t edit);
    const SoundEdit* soundEditOf(SoundEdit::Folder folder, std::string_view name) const;

    /// Every change (tables, types, new types, strings, sounds).
    std::size_t changeCount() const { return edits_.size() + thingEdits_.size() + newThings_.size() + stringEdits_.size() + soundEdits_.size(); }

    /// The strongest reach of the changes (nullopt: none).
    std::optional<Reach> reach() const;
    /// A field's reach (from the schema; a key the game doesn't read: server-only, as a mod reads it).
    static Reach reachOf(const FieldEdit& e);

private:
    static std::size_t index(TableFormat f) { return static_cast<std::size_t>(f); }
    void rebuild(TableFormat f) const;
    /// setThing / removeThing's check (`key` spelt as the readers have it, `value` normalised).
    std::string checkThingEdit(std::string_view type, std::string& key, std::string* value) const;
    /// The pack's thing.patch: its NEW records (with their own edits), then its changes.
    nox::content::ThingPatch thingPatch() const;

    std::array<std::optional<TextTable>, 4> base_;
    mutable std::array<std::optional<TextTable>, 4> working_;
    mutable std::array<bool, 4> stale_{true, true, true, true};
    std::vector<FieldEdit> edits_;
    std::array<std::string, 4> extra_; ///< records the patches add whole (kept as written)
    std::optional<nox::things::ThingDb> baseThings_;
    nox::content::ThingLayout thingLayout_;
    mutable std::optional<nox::things::ThingDb> workThings_;
    mutable bool thingsStale_ = true;
    mutable std::vector<std::string> thingProblems_;
    std::vector<ThingEdit> thingEdits_;
    std::vector<NewThing> newThings_;
    std::optional<nox::content::CsfDocument> baseStrings_;
    mutable std::optional<nox::content::CsfDocument> workStrings_;
    mutable bool stringsStale_ = true;
    std::vector<StringEdit> stringEdits_;
    std::vector<SoundEdit> soundEdits_;
    std::filesystem::path gameDir_;
    std::filesystem::path contentDir_;
    std::string packId_;
    std::vector<std::string> errors_;
    bool dirty_ = false;
};

} // namespace videotools::content
