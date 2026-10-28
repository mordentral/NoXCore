// The map editor's property tables: every field the Properties panel shows for an object, as data.
// A field has a kind (how it's edited), its range or names, a help text from what the game does
// with it, and a getter and a setter over the object's typed record; the panel draws any field from
// its kind, multi-selection compares and sets fields by id, and the tests walk every field of every
// xfer (edit, undo, redo).
//
// Field meanings are GAME.EXE's (the xfer readers and the code that uses each field, as our
// server port reads them), never the old community editor's labels. Fields a record's version
// doesn't hold aren't listed for it (the panel offers to bring the record up to the current
// version). Bits a flags field doesn't name are shown raw and kept: an edit changes only the bits
// it touches.
//
// Lists (a monster's spells and buffs, a shop's items, a glyph's spells, a reward marker's lists, a
// generator's templates, a light's colour sequence, an inventory) have their own editors in the
// panel; their edits are whole-object ops too.
#pragma once

#include "noxcore/mapedit/document.hpp"
#include "noxcore/mapedit/markers.hpp"
#include "noxcore/mapedit/ops.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nox::things {
struct ThingDb;
struct ModifierDb;
struct SoundSets;
} // namespace nox::things

namespace nox::mapedit {

enum class FieldKind : std::uint8_t {
    Int,       ///< a whole number in [min, max]
    Float,     ///< a number in [min, max]
    Bool,      ///< a checkbox (the value 0 or 1, or `onValue`)
    Flags,     ///< bits: `names` the known ones (each a FlagBit); unknown bits kept
    Enum,      ///< one of `names` (by value); a value not named is shown as its number
    Text,      ///< 8-bit text, at most `max` bytes
    Function,  ///< a script function's name (the script panel's picker)
    Thing,     ///< a thing.bin type name (`thingClasses` filters: CLASS bits)
    Spell,     ///< a SPELL_* name (empty: none)
    Ability,   ///< an ABILITY_* name
    Creature,  ///< a field guide creature's name
    Modifier,  ///< a modifier.bin name for the object's item (`modifierSlot`: 0 effectiveness,
               ///< 1 material, 2 primary enchantment, 3 secondary enchantment); empty: none
    Action,    ///< an ACTION_* name (a creature's starting action)
    Voice,     ///< a sound set's name (soundset.bin)
    MapName,   ///< a map's name (the maps folder)
    StringKey, ///< a string table key (nox.csf), with the text's preview
    Color,     ///< an RGB colour
    Extent,    ///< another object's extent (picked in the view; 0 none)
    Waypoint,  ///< a waypoint id (picked in the view; 0 none)
    Point,     ///< a world point (x, y: picked in the view)
    Angle,     ///< radians (a dial), shown in degrees
    Direction, ///< a creature's facing: one of the 8 grid steps (0 east, clockwise)
};

/// A field's value: which members mean something depends on the kind.
struct FieldValue {
    std::int64_t i = 0;  ///< Int, Bool, Flags, Enum, Extent, Waypoint, Direction
    double f = 0;        ///< Float, Angle; Point's x
    double g = 0;        ///< Point's y
    std::string s;       ///< Text and every kind named by a string
    std::array<std::uint8_t, 3> rgb{}; ///< Color
    bool operator==(const FieldValue&) const = default;
};

struct NamedValue {
    std::int64_t value = 0;
    std::string name;
    std::string help;
};

struct FieldDesc {
    std::string id;    ///< stable: "door.lock", "header.team" (multi-select and tests match by it)
    std::string group; ///< the panel's section ("Header", "Combat", "Spells"...)
    std::string label;
    std::string help;  ///< what the game does with it
    FieldKind kind = FieldKind::Int;
    double min = 0;    ///< Int / Float: the range; Text: unused
    double max = 0;    ///< Int / Float: the range; Text: the most bytes
    std::vector<NamedValue> names; ///< Enum: its values; Flags: its known bits
    std::uint32_t thingClasses = 0; ///< Thing: CLASS bits one of which a type must have (0: any)
    int modifierSlot = -1;          ///< Modifier: the slot
    bool advanced = false; ///< shown under "Advanced" (fields whose meaning is partly known)
    bool readOnly = false; ///< the game rewrites it (shown, not edited)
    /// Header fields with an op of their own (not a whole-record change): the panel calls their op.
    enum class Special : std::uint8_t { None, Type, Extent, Details } special = Special::None;
    std::function<FieldValue(const DocObject&)> get;
    std::function<void(DocObject&, const FieldValue&)> set;
};

/// What the tables need from the game data (all optional: a list that's missing is free text).
struct FieldContext {
    const things::ThingDb* things = nullptr;
    const things::ModifierDb* modifiers = nullptr;
    const things::SoundSets* soundSets = nullptr;
};

/// The object's fields: the header's, then its type's (those its record's versions hold).
std::vector<FieldDesc> objectFields(const DocObject& o, const FieldContext& ctx);

/// A field the selected objects share (same id in each), its value, and whether they differ.
struct SharedField {
    FieldDesc desc;   ///< (the first object's)
    FieldValue value; ///< the first object's
    bool mixed = false;
    std::uint64_t mixedBits = 0; ///< Flags: the bits that differ between them
};
/// The fields every one of the objects has, in the first one's order.
std::vector<SharedField> sharedFields(const MapDocument& doc, const std::vector<ObjId>& ids, const FieldContext& ctx);

/// Sets a field on every object that has it: one SetObjects op (empty if nothing changes). For
/// Flags, only the bits in `mask` are set from `value` (the others kept per object).
Op setFieldOp(const MapDocument& doc, const std::vector<ObjId>& ids, const std::string& fieldId, const FieldValue& value,
              const FieldContext& ctx, std::uint64_t mask = ~0ull);

/// The header's create flags as the game names them, the authoring ones (the game's save writes
/// them: kSavedObjectFlags) and the runtime-only ones (set while it plays; a map may still carry them).
const std::vector<FlagBit>& objectFlagBits();
/// The header's xStatus bits.
const std::vector<FlagBit>& xStatusBits();
/// The teams by number, with their colours (0: none).
struct TeamName {
    std::uint8_t team = 0;
    const char* name = "";
    std::array<std::uint8_t, 3> rgb{};
};
const std::vector<TeamName>& teamNames();
/// The monster status flags (MonsterStatus) and the spell usage flags.
const std::vector<FlagBit>& monsterStatusBits();
const std::vector<FlagBit>& spellUsageBits();
/// The creature starting actions (ACTION_* names the map may name).
std::vector<std::string> actionNames();
/// The names a field of this kind picks from (spells, abilities, creatures, enchantments...);
/// Modifier: those modifier.bin allows on `item` in `slot`.
std::vector<std::string> namesFor(FieldKind kind, const FieldContext& ctx, std::string_view item = {}, int slot = -1);
/// ENCHANT_* names (buffs).
std::vector<std::string> enchantNames();

/// The xfer version each xfer writes now (what "bring up to date" sets), and whether a record's
/// version is older than that.
std::uint16_t currentXferVersion(std::string_view xfer);
/// The record brought up to the current header version (0x40) and its xfer's current version,
/// with the fields the newer versions add at the game's fresh values. Throws for a Raw record.
DocObject upgradedRecord(const DocObject& o, const things::ThingDb& things);

} // namespace nox::mapedit
