// thing.bin's patch files (a content pack's content\thing.patch): changes to object types'
// attribute lines, written as thing.bin's own lines are, and new types:
//
//   # comments ('#' or "//" starting a line, or '#' after a blank)
//   GruntAxe                  a type by its name (the first of that name, as the game finds it)
//       HEALTH = 200          every HEALTH line of the type gets the value (added if it has none)
//       -AUDIOLOOP            every AUDIOLOOP line removed
//       MENUICON = 1234       the menu icon: a video.bag image index (PRETTYIMAGE too)
//   END                       (any case)
//
//   NEW FireGrunt LIKE GruntAxe   a new type: a copy of GruntAxe (its lines, DRAW and pictures),
//       CLASS = MONSTER           then these lines; NEW and LIKE in any case
//       HEALTH = 300
//   END
//
// A type's number is its place in thing.bin, which the network carries and the maps' type lists
// depend on, so types are never removed or reordered: new ones go after every type there is, in
// the order the packs and their records add them (newThingTypes). DRAW (the animation data)
// isn't patched. A type's CLASS, XFER, FLAGS and COLLIDE (lockedThingKey) are chosen only in the
// NEW record that adds it, and so is whether it has a MENUICON (saves keep only the types with
// one): on any other type they never change.
//
// Lines are checked against GAME.EXE's readers, so an exported thing.bin reads the same there:
// keys are spelt as the exe's tables have them (a key neither reads is a warning), flag lists
// (CLASS, SUBCLASS, FLAGS, MATERIAL) are known names joined with '+', SPEED / HEALTH / WEIGHT
// whole numbers, MASS / EXPERIENCE numbers, and no value holds '=' or " #".
#pragma once

#include "noxcore/things/thing_db.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace nox::content {

struct ThingPatchLine {
    std::string key;   ///< as written
    std::string value; ///< after the '=', trimmed ("" for a line without one)
    bool hasEquals = false;
    bool remove = false;
};

struct ThingPatchRecord {
    std::string name;
    std::vector<ThingPatchLine> lines;
    std::string like; ///< a NEW record's base type ("" for a change to a type there is)
    bool isNew() const { return !like.empty(); }
};

/// The most object types a thing.bin may have with new ones added: a type travels as its number
/// plus one, which the clients read in 15 bits.
inline constexpr std::size_t kMaxThingTypes = 0x7FFE;
/// The longest name a new type may have.
inline constexpr std::size_t kMaxNewThingName = 63;
/// "" or why `name` can't name a new type (letters, digits and '_', starting with a letter, at most
/// kMaxNewThingName; not NEW, LIKE or END).
std::string checkNewThingName(std::string_view name);

struct ThingPatch {
    std::vector<ThingPatchRecord> records;
};

/// Reads a thing.patch. Problems (a record without END, a line outside a record) go to `problems`.
ThingPatch parseThingPatch(std::string_view text, std::vector<std::string>* problems = nullptr);
/// Writes one ("" for none).
std::string writeThingPatch(const ThingPatch& patch, std::string_view header = {});

/// The keys only a type's NEW record sets: CLASS, XFER, FLAGS, COLLIDE (a different CLASS
/// desyncs every other game's reading of the type's objects; XFER changes how maps, saves and
/// characters are read; FLAGS and COLLIDE make objects double or vanish between players).
bool lockedThingKey(std::string_view key);
/// A key as GAME.EXE's thing.bin readers spell it ("health" -> "HEALTH"); nullopt for one
/// neither reads, or one that isn't a plain word.
std::optional<std::string_view> thingKeySpelling(std::string_view key);
/// Checks `value` for `key` (spelt as thingKeySpelling gives it) as applyThingPatch does: ""
/// with `value` in the readers' spelling ("monster+complex" -> "MONSTER+COMPLEX"), or what's
/// wrong with it. A flag list may keep a name the tables haven't (one the game ignores) only
/// where `stock`, the type's own value, has it.
std::string checkThingValue(std::string_view key, std::string& value, std::string_view stock = {});

/// The value an attribute line `key` has in `t` as the game reads it (the last such line), or
/// for MENUICON / PRETTYIMAGE the image index; nullopt if it has none.
std::optional<std::string> thingValue(const things::ThingDef& t, std::string_view key);

/// Applies `patch` to `db`'s types: its NEW records first, in order (each added after the types
/// there are), then its changes; then db.reindex(), so every index the db keeps is of the changed
/// types. Returns what it added ("FireGrunt"), what it changed ("GruntAxe.HEALTH"), what it
/// couldn't do (a line or a new type refused), and warnings (made, but the stock game won't read
/// it).
struct ThingPatchResult {
    std::vector<std::string> added;
    std::vector<std::string> changed;
    std::vector<std::string> problems;
    std::vector<std::string> warnings;
};
ThingPatchResult applyThingPatch(things::ThingDb& db, const ThingPatch& patch);

} // namespace nox::content
