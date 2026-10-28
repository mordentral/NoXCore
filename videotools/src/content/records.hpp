// The content browser's model: every file the editor knows as a section of records (thing.bin's
// object types, sounds, floors...; modifier.bin's weapons and enchantments; gamedata.bin's
// numbers; nox.csf's labels; the loose files), grouped where the file groups them, with one
// search over all of them. A record's fields (as its file holds them) and the records it refers to
// or that refer to it are worked out on demand.
#pragma once

#include "content/game_files.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace videotools::content {

/// The lists a file holds. A record is (list, index into that list as the reader keeps it).
enum class RecordList : std::uint8_t {
    Thing,         ///< thing.bin THNG (ThingDb::things)
    Spell,         ///< thing.bin SPEL
    Ability,       ///< thing.bin ABIL
    SoundEvent,    ///< thing.bin AUD (ThingDb::audio)
    SoundEventAvnt,///< thing.bin AVNT
    Floor,         ///< thing.bin FLOR
    Edge,          ///< thing.bin EDGE
    Wall,          ///< thing.bin WALL
    NamedImage,    ///< thing.bin IMAG
    Weapon,        ///< modifier.bin WEAPON_DEFINITIONS
    Armor,         ///< modifier.bin ARMOR_DEFINITIONS
    Modifier,      ///< modifier.bin EFFECTIVENESS / MATERIAL / ENCHANTMENT
    Tuning,        ///< gamedata.bin's entries
    Monster,       ///< monster.bin (by the solo reading's order)
    SoundSet,      ///< soundset.bin
    String,        ///< nox.csf
    Sample,        ///< Audio.bag
    File,          ///< a loose file (FileKind tells which list: dialog, windows, maps...)
    View,          ///< a panel rather than a record (Images, Sprites, HD pack)
};

struct RecordRef {
    FileKind kind = FileKind::Things;
    RecordList list = RecordList::Thing;
    std::int32_t index = -1;
    bool operator==(const RecordRef&) const = default;
    bool valid() const { return index >= 0; }
};

struct Record {
    RecordRef ref;
    std::string name;
    std::string detail;       ///< a short second line (class, section, count...)
    std::int32_t icon = -1;   ///< a video.bag image to show beside it, -1 none
    std::string searchKey;    ///< name, lower case
};

/// A named run of a section's records (a class of things, a file of strings...).
struct RecordGroup {
    std::string name;
    std::vector<std::uint32_t> records; ///< into Section::records
};

struct Section {
    FileKind kind = FileKind::Things;
    std::string name;
    std::vector<Record> records;
    std::vector<RecordGroup> groups; ///< empty: records listed flat
    bool loaded = true;              ///< false: its file is missing or broken (FileStatus says why)
};

class ContentIndex {
public:
    /// Every section in the browser's order (sections of files that didn't load are kept, empty).
    static ContentIndex build(const GameFiles& files);

    const std::vector<Section>& sections() const { return sections_; }
    std::size_t recordCount() const;
    /// The record `ref` names, or nullptr.
    const Record* find(const RecordRef& ref) const;
    /// The section holding it, or nullptr.
    const Section* sectionOf(const RecordRef& ref) const;
    /// A record by list and name (exact; Thing, Monster, SoundSet, String are case-insensitive as
    /// the game finds them), or nullopt.
    std::optional<RecordRef> findByName(RecordList list, std::string_view name) const;

    /// Records whose name holds `text` (any case), across every section, at most `limit`.
    std::vector<RecordRef> search(std::string_view text, std::size_t limit = 5000) const;

    /// The types a content pack adds, listed in Things (a group of their own): `things` is the db
    /// with them, their records index it from `first` on (thing.bin's own count). Those of an
    /// earlier call are replaced.
    void setNewThings(const nox::things::ThingDb& things, std::size_t first);
    static constexpr const char* kNewThingsGroup = "New types (this pack)";

private:
    void buildLookups();
    static std::uint64_t key(const RecordRef& ref)
    {
        return (std::uint64_t(ref.kind) << 40) | (std::uint64_t(ref.list) << 32) | std::uint32_t(ref.index);
    }

    std::vector<Section> sections_;
    std::unordered_map<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>> where_; ///< ref -> (section, record)
    /// (list, lower-case name) -> the first record of that name; exact names under (list | 0x80).
    std::unordered_map<std::string, RecordRef> byName_;
};

/// Which side of a game reads a field: the server (gameplay), the client (drawing, sounds,
/// names), or both. Unknown until the field is checked against NoX Core's readers.
enum class Side : std::uint8_t { Unknown, Server, Client, Both };

/// One of a record's fields as its file holds it (read-only text for now).
struct Field {
    std::string label;
    std::string value;
    Side side = Side::Unknown;
    std::string note; ///< a tooltip
    /// What in it can be played (the Record panel gives it Play buttons): the value names a sound
    /// event, or it lists Audio.bag samples (in `samples`).
    enum class Plays : std::uint8_t { Nothing, SoundEvent, Samples } plays = Plays::Nothing;
    std::vector<std::string> samples;
};

/// What a record holds, field by field, as its file does.
std::vector<Field> describe(const GameFiles& files, const RecordRef& ref);
/// The record as raw text, for the text-format files (gamedata, monster, soundset, modifier,
/// thing.bin attributes); empty when there's no text form.
std::string rawText(const GameFiles& files, const RecordRef& ref);

/// A link from a record to another (or to a file): what it is and where it goes.
struct Reference {
    std::string text;
    std::optional<RecordRef> target;
};
/// The records this one names and the ones that name it, the cheap ones (no map scan).
std::vector<Reference> references(const GameFiles& files, const ContentIndex& index, const RecordRef& ref);

/// What the preview shows for a record: a video.bag image (or animation's frames), a sound.
struct PreviewInfo {
    std::vector<std::int32_t> frames; ///< video.bag images, played in order; one: a still
    int frameDelay = 0;               ///< game frames per frame
    std::string catalogEntry;         ///< the Sprites view's entry for it ("Category/Name"), if any
    std::vector<std::string> sounds;  ///< Audio.bag sample names to offer (a sound event's files)
    std::string text;                 ///< a string's text
};
PreviewInfo previewOf(const GameFiles& files, const RecordRef& ref);

/// A sound id (audio/sound_names.hpp) as text: its name, or "none".
std::string soundLabel(std::uint32_t id);

} // namespace videotools::content
