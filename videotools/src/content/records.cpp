// The content browser's model (records.hpp).
#include "content/records.hpp"

#include "noxcore/audio/sound_names.hpp"
#include "noxcore/strings/l10n.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>

namespace fs = std::filesystem;
using nox::util::lowered;

namespace videotools::content {

namespace {

Record make(FileKind kind, RecordList list, std::size_t index, std::string name, std::string detail = {}, std::int32_t icon = -1)
{
    Record r;
    r.ref = {kind, list, static_cast<std::int32_t>(index)};
    r.searchKey = lowered(name);
    r.name = std::move(name);
    r.detail = std::move(detail);
    r.icon = icon;
    return r;
}

/// Groups a section's records by a name per record, in first-seen order unless `sorted`.
void groupBy(Section& s, const std::vector<std::string>& keys, bool sorted)
{
    std::map<std::string, std::size_t> at;
    for (std::size_t i = 0; i < s.records.size(); ++i) {
        const auto [it, added] = at.emplace(keys[i], s.groups.size());
        if (added) {
            s.groups.push_back({keys[i], {}});
        }
        s.groups[it->second].records.push_back(static_cast<std::uint32_t>(i));
    }
    if (sorted) {
        std::ranges::sort(s.groups, [](const RecordGroup& a, const RecordGroup& b) { return lowered(a.name) < lowered(b.name); });
    }
}

/// The class a thing is browsed under: the first of these it has, else its first class by bit.
std::string thingGroup(const nox::things::ThingDef& t)
{
    using namespace nox::things::ObjectClass;
    const std::uint32_t c = nox::things::thingClasses(t);
    static constexpr std::pair<std::uint32_t, nox::l10n::Key> kOrder[] = {
        {Player, NOX_TRK("content.group.players", "Players")},
        {Monster, NOX_TRK("content.group.monsters", "Monsters")},
        {Weapon, NOX_TRK("content.group.weapons", "Weapons")},
        {Armor, NOX_TRK("content.group.armour", "Armour")},
        {Wand, NOX_TRK("content.group.wands", "Wands")},
        {Food, NOX_TRK("content.group.food", "Food & potions")},
        {Missile, NOX_TRK("content.group.missiles", "Missiles")},
        {Door, NOX_TRK("content.group.doors", "Doors")},
        {Key, NOX_TRK("content.group.keys", "Keys")},
        {InfoBook, NOX_TRK("content.group.books", "Books")},
        {MonsterGenerator, NOX_TRK("content.group.generators", "Generators")},
        {Exit, NOX_TRK("content.group.exits", "Exits")},
        {Transporter, NOX_TRK("content.group.teleporters", "Teleporters")},
        {Elevator, NOX_TRK("content.group.elevators", "Elevators")},
        {Trigger, NOX_TRK("content.group.triggers", "Triggers")},
        {Treasure, NOX_TRK("content.group.treasure", "Treasure")},
        {Light, NOX_TRK("content.group.lights", "Lights")},
        {Fire, NOX_TRK("content.group.fire", "Fire")},
        {Obstacle, NOX_TRK("content.group.obstacles", "Obstacles")},
        {Immobile, NOX_TRK("content.group.scenery", "Scenery")},
    };
    for (const auto& [bit, name] : kOrder) {
        if (c & bit) {
            return nox::l10n::tr(name);
        }
    }
    for (int b = 0; b < 32; ++b) {
        if (c & (1u << b)) {
            return std::string(nox::things::kClassNames[b]);
        }
    }
    return NOX_TR("content.group.no_class", "No class");
}

std::string str(const nox::strings::StringTable* strings, std::string_view label)
{
    if (!strings || label.empty()) {
        return std::string(label);
    }
    if (const auto t = strings->text(label)) {
        return nox::strings::toUtf8(*t);
    }
    return std::string(label);
}

std::string rgb(const nox::things::Rgb& c)
{
    return std::format("{} {} {}", c[0], c[1], c[2]);
}

std::string imageText(const nox::things::ImageRef& r)
{
    return r.index >= 0 ? std::format("#{}", r.index) : r.name.empty() ? NOX_TR("content.record.none", "none") : r.name;
}

std::string joined(const std::vector<std::string>& v, std::string_view sep = ", ")
{
    std::string out;
    for (const auto& s : v) {
        out += (out.empty() ? "" : std::string(sep)) + s;
    }
    return out;
}

std::string floats(const std::vector<float>& v)
{
    std::string out;
    for (float f : v) {
        out += (out.empty() ? "" : ", ") + std::format("{}", f);
    }
    return out;
}

const char* gameDataSection(nox::things::GameData::Section s)
{
    switch (s) {
    case nox::things::GameData::Section::Solo: return "SOLO";
    case nox::things::GameData::Section::Arena: return "ARENA";
    default: return NOX_TRC("content.record.game_both", "both");
    }
}

std::string_view modifierSection(const nox::things::ModifierDef& m)
{
    return m.section;
}

/// A monster's numbers as fields, the solo and arena readings side by side.
void monsterFields(std::vector<Field>& out, const nox::things::MonsterDef* solo, const nox::things::MonsterDef* arena)
{
    const auto row = [&](const char* label, auto get) {
        const std::string s = solo ? get(*solo) : std::string("-");
        const std::string a = arena ? get(*arena) : std::string("-");
        Field& f = out.emplace_back();
        f.label = label;
        f.value = s == a ? s : nox::l10n::format(NOX_TR("content.record.solo_arena", "solo {solo}   arena {arena}"), {{"solo", s}, {"arena", a}});
    };
    const auto num = [](auto member) { return [member](const nox::things::MonsterDef& m) { return std::format("{}", m.*member); }; };
    using M = nox::things::MonsterDef;
    row("EXPERIENCE", num(&M::experience));
    row("HEALTH", num(&M::health));
    row("QUESTHEALTH", num(&M::questHealth));
    row("SPEED", num(&M::speed));
    row("RUN_MULTIPLIER", num(&M::runMultiplier));
    row("RETREAT_RATIO", num(&M::retreatRatio));
    row("RESUME_RATIO", num(&M::resumeRatio));
    row("FLEE_RANGE", num(&M::fleeRange));
    row("STATUS", [](const M& m) { return std::format("0x{:X}", m.status); });
    row("MELEE_ATTACK_FRAME", num(&M::meleeFrame));
    row("MELEE_ATTACK_RANGE", num(&M::meleeRange));
    row("MELEE_ATTACK_DAMAGE", num(&M::meleeDamage));
    row("MELEE_ATTACK_IMPACT", num(&M::meleeImpact));
    row("MELEE_ATTACK_DAMAGE_TYPE", num(&M::meleeDamageType));
    row("MELEE_ATTACK_MIN_DELAY", num(&M::meleeMinDelay));
    row("MELEE_ATTACK_MAX_DELAY", num(&M::meleeMaxDelay));
    row("MELEE_ATTACK_POISON_CHANCE", num(&M::poisonChance));
    row("MELEE_ATTACK_POISON_STRENGTH", num(&M::poisonStrength));
    row("MELEE_ATTACK_POISON_MAX", num(&M::poisonMax));
    row("MISSILE_NAME", [](const M& m) { return m.missile.empty() ? std::string("NULL") : m.missile; });
    row("MISSILE_ATTACK_RANGE", num(&M::missileRange));
    row("MISSILE_ATTACK_FRAME", num(&M::missileFrame));
    row("MISSILE_ATTACK_MIN_DELAY", num(&M::missileMinDelay));
    row("MISSILE_ATTACK_MAX_DELAY", num(&M::missileMaxDelay));
    row("MOVE_SOUND_FRAME_A", num(&M::moveSoundFrameA));
    row("MOVE_SOUND_FRAME_B", num(&M::moveSoundFrameB));
    row("MELEE_STRIKE_FUNCTION", [](const M& m) { return m.strikeFn; });
    row("DIE_FUNCTION", [](const M& m) { return m.dieFn; });
    row("DEAD_FUNCTION", [](const M& m) { return m.deadFn; });
}

// soundset.bin's keys, by slot (sound_sets.hpp's order).
constexpr const char* kSoundSlotKeys[] = {
    "",
    "LISTEN_SOUND",               "TAUNT_SOUND",               "THREATEN_SOUND",       "IDLE_SOUND",
    "ENGAGE_SOUND",               "MELEE_ATTACK_INIT_SOUND",   "MELEE_ATTACK_SOUND",   "MELEE_ATTACK_HITTING_SOUND",
    "MELEE_ATTACK_MISSING_SOUND", "MISSILE_ATTACK_INIT_SOUND", "MISSILE_ATTACK_SOUND", "FLEE_SOUND",
    "RETREAT_SOUND",              "SPELL_INIT_SOUND",          "DIE_SOUND",            "INJURED_SOUND",
    "RECOGNIZE_SOUND",            "MOVE_SOUND",
};

std::string fileName(const fs::path& p)
{
    return p.filename().string();
}

} // namespace

std::string soundLabel(std::uint32_t id)
{
    if (id == 0) {
        return NOX_TR("content.record.none", "none");
    }
    const std::string_view n = nox::audio::soundName(id);
    return n.empty() ? std::format("#{}", id) : std::string(n);
}

// ---------------------------------------------------------------------------------------------
// The index

ContentIndex ContentIndex::build(const GameFiles& files)
{
    ContentIndex ix;
    const auto loaded = [&](FileKind k) {
        const FileStatus* s = files.statusOf(k);
        return !s || s->loaded;
    };
    const auto section = [&](FileKind kind, std::string name) -> Section& {
        Section s;
        s.kind = kind;
        s.name = std::move(name);
        s.loaded = loaded(kind);
        ix.sections_.push_back(std::move(s));
        return ix.sections_.back();
    };

    // thing.bin: its object types by class, then its other lists.
    {
        Section& s = section(FileKind::Things, fileKindLabel(FileKind::Things));
        std::vector<std::string> groups;
        if (const auto* db = files.things()) {
            const std::string& typeN = NOX_TR("content.record.type_n", "type {n}");
            for (std::size_t i = 0; i < db->things.size(); ++i) {
                const auto& t = db->things[i];
                const std::int32_t icon = t.menuIcon && t.menuIcon->index >= 0 ? t.menuIcon->index : -1;
                s.records.push_back(make(FileKind::Things, RecordList::Thing, i, t.name, nox::l10n::format(typeN, {{"n", i + 1}}), icon));
                groups.push_back(thingGroup(t));
            }
        }
        groupBy(s, groups, true);
    }
    if (const auto* db = files.things()) {
        Section& sp = section(FileKind::Things, NOX_TR("content.section.spells", "Spells"));
        for (std::size_t i = 0; i < db->spells.size(); ++i) {
            const auto& d = db->spells[i];
            sp.records.push_back(make(FileKind::Things, RecordList::Spell, i, d.id, nox::l10n::format(NOX_TR("content.record.mana_n", "{n} mana"), {{"n", d.manaCost}}),
                                      d.icon.index));
        }
        Section& ab = section(FileKind::Things, NOX_TR("content.section.abilities", "Abilities"));
        for (std::size_t i = 0; i < db->abilities.size(); ++i) {
            ab.records.push_back(make(FileKind::Things, RecordList::Ability, i, db->abilities[i].name, {}, db->abilities[i].icons[0].index));
        }
        Section& snd = section(FileKind::Things, NOX_TR("content.section.sound_events", "Sound events"));
        for (std::size_t i = 0; i < db->audio.size(); ++i) {
            const auto& a = db->audio[i];
            snd.records.push_back(make(FileKind::Things, RecordList::SoundEvent, i, a.name,
                                       nox::l10n::format(NOX_TR("content.record.files_n", "{n} files"), {{"n", a.files.size()}})));
        }
        for (std::size_t i = 0; i < db->avnts.size(); ++i) {
            snd.records.push_back(make(FileKind::Things, RecordList::SoundEventAvnt, i, db->avnts[i].name, "AVNT"));
        }
        Section& fl = section(FileKind::Things, NOX_TR("content.section.floors", "Floors"));
        for (std::size_t i = 0; i < db->floors.size(); ++i) {
            const auto& f = db->floors[i];
            fl.records.push_back(make(FileKind::Things, RecordList::Floor, i, f.name, nox::l10n::format(NOX_TR("content.record.floor_n", "floor {n}"), {{"n", i}}),
                                      f.images.empty() ? -1 : f.images.front().index));
        }
        Section& ed = section(FileKind::Things, NOX_TR("content.section.edges", "Edges"));
        for (std::size_t i = 0; i < db->edges.size(); ++i) {
            const auto& e = db->edges[i];
            ed.records.push_back(make(FileKind::Things, RecordList::Edge, i, e.name, nox::l10n::format(NOX_TR("content.record.edge_n", "edge {n}"), {{"n", i}}),
                                      e.images.empty() ? -1 : e.images.front().index));
        }
        Section& wa = section(FileKind::Things, NOX_TR("content.section.walls", "Walls"));
        for (std::size_t i = 0; i < db->walls.size(); ++i) {
            const auto& w = db->walls[i];
            std::int32_t icon = -1;
            for (const auto& dir : w.directions) {
                if (!dir.empty() && icon < 0) {
                    icon = dir.front()[0].image.index;
                }
            }
            wa.records.push_back(make(FileKind::Things, RecordList::Wall, i, w.name, nox::l10n::format(NOX_TR("content.record.wall_n", "wall {n}"), {{"n", i}}), icon));
        }
        Section& im = section(FileKind::Things, NOX_TR("content.section.named_images", "Named images"));
        for (std::size_t i = 0; i < db->images.size(); ++i) {
            const auto& d = db->images[i];
            std::int32_t icon = d.image ? d.image->index : d.animation && !d.animation->frames.empty() ? d.animation->frames.front().index : -1;
            im.records.push_back(make(FileKind::Things, RecordList::NamedImage, i, d.name,
                                      d.animation ? NOX_TR("content.record.animation", "animation") : NOX_TR("content.record.image", "image"), icon));
        }
    }

    // modifier.bin
    {
        Section& s = section(FileKind::Modifiers, fileKindLabel(FileKind::Modifiers));
        std::vector<std::string> groups;
        if (const auto* db = files.modifiers()) {
            for (std::size_t i = 0; i < db->weapons.size(); ++i) {
                s.records.push_back(make(FileKind::Modifiers, RecordList::Weapon, i, db->weapons[i].name, NOX_TR("content.record.weapon", "weapon")));
                groups.push_back(NOX_TR("content.group.weapons", "Weapons"));
            }
            for (std::size_t i = 0; i < db->armor.size(); ++i) {
                s.records.push_back(make(FileKind::Modifiers, RecordList::Armor, i, db->armor[i].name, NOX_TR("content.record.armour", "armour")));
                groups.push_back(NOX_TR("content.group.armour", "Armour"));
            }
            for (std::size_t i = 0; i < db->modifiers.size(); ++i) {
                const auto& m = db->modifiers[i];
                s.records.push_back(make(FileKind::Modifiers, RecordList::Modifier, i, m.name, nox::l10n::format(NOX_TR("content.record.id_n", "id {n}"), {{"n", m.id}})));
                const std::string_view sec = modifierSection(m);
                groups.push_back(sec == "EFFECTIVENESS" ? NOX_TR("content.group.effectiveness", "Effectiveness")
                                 : sec == "MATERIAL"    ? NOX_TR("content.group.materials", "Materials")
                                                        : NOX_TR("content.group.enchantments", "Enchantments"));
            }
        }
        groupBy(s, groups, false);
    }
    // gamedata.bin, by the first word of each name (Fireball..., Trap...).
    {
        Section& s = section(FileKind::GameData, fileKindLabel(FileKind::GameData));
        if (const auto* gd = files.gameData()) {
            // One record a name (its SOLO, ARENA and unprefixed lines together), at its first line.
            std::set<std::string> seen;
            for (std::size_t i = 0; i < gd->entries.size(); ++i) {
                const auto& e = gd->entries[i];
                if (!seen.insert(lowered(e.name)).second) {
                    continue;
                }
                s.records.push_back(make(FileKind::GameData, RecordList::Tuning, i, e.name, floats(e.values)));
            }
        }
    }
    {
        Section& s = section(FileKind::Monsters, fileKindLabel(FileKind::Monsters));
        if (const auto* db = files.monstersSolo()) {
            const std::string& healthN = NOX_TR("content.record.health_n", "{n} health");
            for (std::size_t i = 0; i < db->monsters.size(); ++i) {
                const auto& m = db->monsters[i];
                s.records.push_back(make(FileKind::Monsters, RecordList::Monster, i, m.name, nox::l10n::format(healthN, {{"n", std::format("{}", m.health)}})));
            }
        }
    }
    {
        Section& s = section(FileKind::SoundSets, fileKindLabel(FileKind::SoundSets));
        if (const auto* ss = files.soundSets()) {
            for (std::size_t i = 0; i < ss->sets.size(); ++i) {
                s.records.push_back(make(FileKind::SoundSets, RecordList::SoundSet, i, ss->sets[i].name));
            }
        }
    }
    // nox.csf, by the label's file ("QuitMenu.wnd:Quit" under QuitMenu.wnd).
    {
        Section& s = section(FileKind::Strings, fileKindLabel(FileKind::Strings));
        std::vector<std::string> groups;
        if (const auto* st = files.strings()) {
            const auto& es = st->entries();
            for (std::size_t i = 0; i < es.size(); ++i) {
                const auto& e = es[i];
                std::string first = e.variants.empty() ? std::string() : nox::strings::toUtf8(e.variants.front().text);
                if (first.size() > 60) {
                    first = first.substr(0, nox::util::utf8CutLength(first, 57)) + "..."; // (between letters)
                }
                s.records.push_back(make(FileKind::Strings, RecordList::String, i, e.label, first));
                const auto colon = e.label.find(':');
                groups.push_back(colon == std::string::npos ? NOX_TR("content.group.no_file", "(no file)") : e.label.substr(0, colon));
            }
        }
        groupBy(s, groups, true);
    }
    // The images: panels, not records.
    {
        Section& s = section(FileKind::Images, fileKindLabel(FileKind::Images));
        s.records.push_back(make(FileKind::Images, RecordList::View, 0, NOX_TR("content.record.images_view", "Images (video.bag and HD pack)"),
                                 NOX_TR("content.record.images_view_detail", "every image by index")));
        s.records.push_back(make(FileKind::Images, RecordList::View, 1, NOX_TR("content.record.sprites_view", "Sprites (by thing)"),
                                 NOX_TR("content.record.sprites_view_detail", "animations, as thing.bin draws them")));
    }
    {
        Section& s = section(FileKind::Audio, fileKindLabel(FileKind::Audio));
        if (auto* bag = files.audio()) {
            const auto& samples = bag->samples();
            const std::string& hz = NOX_TR("content.record.sample_detail", "{rate} Hz{stereo}{adpcm}");
            const std::string& stereo = NOX_TR("content.record.stereo", " stereo");
            for (std::size_t i = 0; i < samples.size(); ++i) {
                const auto& b = samples[i];
                s.records.push_back(make(FileKind::Audio, RecordList::Sample, i, b.name,
                                         nox::l10n::format(hz, {{"rate", b.rate}, {"stereo", b.channels() == 2 ? stereo : std::string()},
                                                                {"adpcm", b.flags & b.kAdpcm ? " ADPCM" : ""}})));
            }
        }
    }
    const auto loose = [&](FileKind kind, const std::vector<fs::path>& list, auto detail) {
        Section& s = section(kind, fileKindLabel(kind));
        for (std::size_t i = 0; i < list.size(); ++i) {
            s.records.push_back(make(kind, RecordList::File, i, fileName(list[i]), detail(list[i])));
        }
        s.loaded = true;
    };
    const auto none = [](const fs::path&) { return std::string(); };
    const auto parent = [](const fs::path& p) { return p.parent_path().filename().string(); };
    loose(FileKind::Dialog, files.dialog(), none);
    loose(FileKind::Windows, files.windows(), none);
    loose(FileKind::Maps, files.maps(), none);
    loose(FileKind::Rules, files.rules(), parent);
    loose(FileKind::Characters, files.characters(), none);
    loose(FileKind::Mods, files.mods(), [](const fs::path& p) {
        std::error_code ec;
        return fs::exists(p / "content", ec) ? NOX_TR("content.record.content_pack", "content pack") : NOX_TR("content.record.mod", "mod");
    });
    loose(FileKind::Text, files.texts(), none);
    loose(FileKind::Fonts, files.fonts(), none);
    loose(FileKind::Palette, files.palettes(), none);
    ix.buildLookups();
    return ix;
}

namespace {

/// Lists whose names the game looks up without case.
bool namesNoCase(RecordList list)
{
    return list == RecordList::Thing || list == RecordList::Monster || list == RecordList::SoundSet || list == RecordList::String ||
           list == RecordList::Sample || list == RecordList::SoundEvent || list == RecordList::File;
}

std::string nameKey(RecordList list, std::string_view name)
{
    std::string k(1, static_cast<char>(static_cast<std::uint8_t>(list) | (namesNoCase(list) ? 0 : 0x80)));
    k += namesNoCase(list) ? lowered(name) : std::string(name);
    return k;
}

} // namespace

void ContentIndex::buildLookups()
{
    where_.clear();
    byName_.clear();
    for (std::size_t s = 0; s < sections_.size(); ++s) {
        for (std::size_t r = 0; r < sections_[s].records.size(); ++r) {
            const Record& rec = sections_[s].records[r];
            where_.emplace(key(rec.ref), std::pair(std::uint32_t(s), std::uint32_t(r)));
            byName_.emplace(nameKey(rec.ref.list, rec.name), rec.ref); // (the first of a name stays)
        }
    }
}

const std::string& ContentIndex::newThingsGroup()
{
    return NOX_TR("content.group.new_types", "New types (this pack)");
}

void ContentIndex::setNewThings(const nox::things::ThingDb& things, std::size_t first)
{
    const auto s = std::ranges::find_if(sections_, [](const Section& x) { return x.kind == FileKind::Things && x.name == fileKindLabel(FileKind::Things); });
    if (s == sections_.end()) {
        return;
    }
    // The earlier call's taken out: they're the records past `first` (always last, in one group).
    const std::size_t keep = std::min(first, s->records.size());
    s->records.resize(keep);
    std::erase_if(s->groups, [](const RecordGroup& g) { return g.name == newThingsGroup(); });
    RecordGroup group{newThingsGroup(), {}};
    for (std::size_t i = first; i < things.things.size(); ++i) {
        const auto& t = things.things[i];
        const std::int32_t icon = t.menuIcon && t.menuIcon->index >= 0 ? t.menuIcon->index : -1;
        group.records.push_back(static_cast<std::uint32_t>(s->records.size()));
        s->records.push_back(make(FileKind::Things, RecordList::Thing, i, t.name,
                                  nox::l10n::format(NOX_TR("content.record.type_n_new", "type {n}, new"), {{"n", i + 1}}), icon));
    }
    if (!group.records.empty()) {
        s->groups.insert(s->groups.begin(), std::move(group));
    }
    buildLookups();
}

std::size_t ContentIndex::recordCount() const
{
    return where_.size();
}

const Record* ContentIndex::find(const RecordRef& ref) const
{
    const auto it = where_.find(key(ref));
    return it == where_.end() ? nullptr : &sections_[it->second.first].records[it->second.second];
}

const Section* ContentIndex::sectionOf(const RecordRef& ref) const
{
    const auto it = where_.find(key(ref));
    return it == where_.end() ? nullptr : &sections_[it->second.first];
}

std::optional<RecordRef> ContentIndex::findByName(RecordList list, std::string_view name) const
{
    const auto it = byName_.find(nameKey(list, name));
    return it == byName_.end() ? std::nullopt : std::optional<RecordRef>(it->second);
}

std::vector<RecordRef> ContentIndex::search(std::string_view text, std::size_t limit) const
{
    std::vector<RecordRef> out;
    const std::string key = lowered(text);
    for (const auto& s : sections_) {
        for (const auto& r : s.records) {
            if (r.searchKey.find(key) != std::string::npos) {
                out.push_back(r.ref);
                if (out.size() >= limit) {
                    return out;
                }
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Fields

std::vector<Field> describe(const GameFiles& files, const RecordRef& ref)
{
    std::vector<Field> f;
    const auto add = [&](std::string label, std::string value, std::string note = {}) {
        Field& x = f.emplace_back();
        x.label = std::move(label);
        x.value = std::move(value);
        x.note = std::move(note);
    };
    const auto addEvent = [&](std::string label, std::string value) {
        add(std::move(label), std::move(value));
        f.back().plays = Field::Plays::SoundEvent;
    };
    const auto addFiles = [&](const std::vector<std::string>& files) {
        add(NOX_TR("content.label.files", "Files"), joined(files));
        f.back().plays = Field::Plays::Samples;
        for (const auto& file : files) {
            f.back().samples.push_back(fs::path(file).stem().string());
        }
    };
    const auto* db = files.things();
    const auto* strings = files.strings();
    const auto at = [&](const auto& list) -> decltype(&list[0]) {
        return ref.index >= 0 && std::size_t(ref.index) < list.size() ? &list[std::size_t(ref.index)] : nullptr;
    };
    switch (ref.list) {
    case RecordList::Thing:
        if (const auto* t = db ? at(db->things) : nullptr) {
            add(NOX_TR("content.label.type_id", "Type id"), std::to_string(ref.index + 1), NOX_TR("content.note.type_id", "The number maps and the network use for this type: its place in thing.bin."));
            for (const auto& a : t->attributes) {
                const auto eq = a.find('=');
                if (eq == std::string::npos) {
                    add(a, NOX_TR("content.record.set", "(set)"));
                } else {
                    add(std::string(nox::util::trim(std::string_view(a).substr(0, eq))), std::string(nox::util::trim(std::string_view(a).substr(eq + 1))));
                }
            }
            if (t->draw) {
                add("DRAW", nox::l10n::format(NOX_TR("content.record.draw_bytes", "{type} ({n} bytes)"), {{"type", t->draw->type}, {"n", t->draw->data.size()}}));
            }
            if (t->menuIcon) {
                add("MENUICON", imageText(*t->menuIcon));
            }
            if (t->prettyImage) {
                add("PRETTYIMAGE", imageText(*t->prettyImage));
            }
        }
        break;
    case RecordList::Spell:
        if (const auto* s = db ? at(db->spells) : nullptr) {
            add(NOX_TR("content.label.id", "Id"), s->id);
            add(NOX_TR("content.label.mana_cost", "Mana cost"), std::to_string(s->manaCost));
            add(NOX_TR("content.label.price", "Price"), std::to_string(s->price));
            std::string ph;
            for (auto p : s->phonemes) {
                ph += (ph.empty() ? "" : " ") + std::to_string(p);
            }
            add(NOX_TR("content.label.phonemes", "Phonemes"), ph.empty() ? NOX_TR("content.record.none", "none") : ph);
            add(NOX_TR("content.label.flags", "Flags"), std::format("0x{:X}", s->flags));
            add(NOX_TR("content.label.title", "Title"), s->title, str(strings, s->title));
            add(NOX_TR("content.label.description", "Description"), s->description, str(strings, s->description));
            add(NOX_TR("content.label.icon", "Icon"), imageText(s->icon));
            add(NOX_TR("content.label.icon_enabled", "Icon (enabled)"), imageText(s->iconEnabled));
            addEvent(NOX_TR("content.label.cast_sound", "Cast sound"), s->castSound);
            addEvent(NOX_TR("content.label.on_sound", "On sound"), s->onSound);
            addEvent(NOX_TR("content.label.off_sound", "Off sound"), s->offSound);
        }
        break;
    case RecordList::Ability:
        if (const auto* a = db ? at(db->abilities) : nullptr) {
            add(NOX_TR("content.label.name", "Name"), a->name);
            add(NOX_TR("content.label.title", "Title"), a->title, str(strings, a->title));
            add(NOX_TR("content.label.description", "Description"), a->description, str(strings, a->description));
            for (int i = 0; i < 3; ++i) {
                add(nox::l10n::format(NOX_TR("content.label.icon_n", "Icon {n}"), {{"n", i + 1}}), imageText(a->icons[std::size_t(i)]));
                addEvent(nox::l10n::format(NOX_TR("content.label.sound_n", "Sound {n}"), {{"n", i + 1}}), a->sounds[std::size_t(i)]);
            }
        }
        break;
    case RecordList::SoundEvent:
        if (const auto* a = db ? at(db->audio) : nullptr) {
            add(NOX_TR("content.label.volume", "Volume"), std::format("{}%", a->volume));
            add(NOX_TR("content.label.range", "Range"), std::to_string(a->range), NOX_TR("content.note.range", "In 15-unit steps; 0 or less keeps the default 600 units."));
            add(NOX_TR("content.label.hearing", "Hearing"), std::to_string(a->hearing));
            add(NOX_TR("content.label.max_instances", "Max instances"), std::to_string(a->maxInstances), NOX_TR("content.note.max_instances", "Voices at once, 0 = no limit."));
            add(NOX_TR("content.label.pitch", "Pitch"), std::format("{} .. {}", a->pitchMin, a->pitchMax), NOX_TR("content.note.pitch", "Playback-rate percent added to 100, picked per play."));
            add(NOX_TR("content.label.priority", "Priority"), std::to_string(a->priority));
            addFiles(a->files);
        }
        break;
    case RecordList::SoundEventAvnt:
        if (const auto* a = db ? at(db->avnts) : nullptr) {
            const auto opt = [&](const std::string& label, const auto& v) {
                if (v) {
                    add(label, std::format("{}", *v));
                }
            };
            opt(NOX_TR("content.label.priority", "Priority"), a->priority);
            opt(NOX_TR("content.label.flags", "Flags"), a->flags);
            opt(NOX_TR("content.label.volume", "Volume"), a->volume);
            opt(NOX_TR("content.label.max_instances", "Max instances"), a->maxInstances);
            opt(NOX_TR("content.label.loops", "Loops"), a->loops);
            if (a->pitch) {
                add(NOX_TR("content.label.pitch", "Pitch"), std::format("{} .. {}", a->pitch->first, a->pitch->second));
            }
            if (a->files) {
                addFiles(*a->files);
            }
            if (a->delay) {
                add(NOX_TR("content.label.delay", "Delay"), nox::l10n::format(NOX_TR("content.record.delay_ms", "{min} .. {max} ms"), {{"min", std::format("{}", a->delay->first)}, {"max", std::format("{}", a->delay->second)}}));
            }
            opt(NOX_TR("content.label.range", "Range"), a->range);
            opt(NOX_TR("content.label.hearing", "Hearing"), a->hearing);
        }
        break;
    case RecordList::Floor:
        if (const auto* fl = db ? at(db->floors) : nullptr) {
            add(NOX_TR("content.label.index", "Index"), std::to_string(ref.index), NOX_TR("content.note.floor_index", "Maps' floor tiles name the floor by this number."));
            add(NOX_TR("content.label.minimap_colour", "Minimap colour"), std::format("{} {} {}", fl->minimapColor[0], fl->minimapColor[1], fl->minimapColor[2]));
            add(NOX_TR("content.label.material", "Material"), std::format("0x{:X}", fl->material));
            add(NOX_TR("content.label.flags", "Flags"), std::format("0x{:X}", fl->flags));
            add(NOX_TR("content.label.columns_x_rows", "Columns x rows"), std::format("{} x {}", fl->cols, fl->rows));
            add(NOX_TR("content.label.frames", "Frames"), std::to_string(fl->frames));
            add(NOX_TR("content.label.images", "Images"), std::to_string(fl->images.size()));
        }
        break;
    case RecordList::Edge:
        if (const auto* e = db ? at(db->edges) : nullptr) {
            add(NOX_TR("content.label.index", "Index"), std::to_string(ref.index));
            add(NOX_TR("content.label.material", "Material"), std::format("0x{:X}", e->material));
            add(NOX_TR("content.label.flags", "Flags"), std::format("0x{:X}", e->flags));
            add(NOX_TR("content.label.columns_x_rows", "Columns x rows"), std::format("{} x {}", e->cols, e->rows));
            add(NOX_TR("content.label.frames", "Frames"), std::to_string(e->frames));
            add(NOX_TR("content.label.images", "Images"), std::to_string(e->images.size()));
        }
        break;
    case RecordList::Wall:
        if (const auto* w = db ? at(db->walls) : nullptr) {
            add(NOX_TR("content.label.index", "Index"), std::to_string(ref.index), NOX_TR("content.note.wall_index", "Maps' walls name the material by this number."));
            add(NOX_TR("content.label.flags", "Flags"), std::format("0x{:X}", w->flags), NOX_TR("content.note.wall_flags", "1 blocks sight, 2 ignored by the line trace, 4 see-through and sprite-sorted."));
            add(NOX_TR("content.label.material", "Material"), std::format("0x{:X}", static_cast<std::uint32_t>(w->material)));
            add(NOX_TR("content.label.health", "Health"), std::to_string(w->health));
            add(NOX_TR("content.label.debris", "Debris"), joined(w->debris));
            addEvent(NOX_TR("content.label.open_sound", "Open sound"), w->openSound);
            addEvent(NOX_TR("content.label.close_sound", "Close sound"), w->closeSound);
            addEvent(NOX_TR("content.label.break_sound", "Break sound"), w->breakSound);
            add(NOX_TR("content.label.random_variations", "Random variations"), std::to_string(w->randomVariations));
            std::size_t variants = 0;
            for (const auto& d : w->directions) {
                variants += d.size();
            }
            add(NOX_TR("content.label.variants", "Variants"), std::to_string(variants));
        }
        break;
    case RecordList::NamedImage:
        if (const auto* im = db ? at(db->images) : nullptr) {
            if (im->image) {
                add(NOX_TR("content.label.image", "Image"), imageText(*im->image));
            }
            if (im->animation) {
                add(NOX_TR("content.label.kind", "Kind"), im->animation->kind);
                add(NOX_TR("content.label.frame_delay", "Frame delay"), std::to_string(im->animation->frameDelay));
                add(NOX_TR("content.label.frames", "Frames"), std::to_string(im->animation->frames.size()));
            }
        }
        break;
    case RecordList::Weapon:
    case RecordList::Armor:
        if (const auto* md = files.modifiers()) {
            const auto& list = ref.list == RecordList::Weapon ? md->weapons : md->armor;
            if (const auto* e = at(list)) {
                for (int i = 0; i < 6; ++i) {
                    add(std::format("COLOR{}", i + 1), rgb(e->colors[std::size_t(i)]));
                }
                static constexpr const char* kSlot[] = {"EFFECTIVENESS", "MATERIAL", "PRIMARYENCHANTMENT", "SECONDARYENCHANTMENT"};
                for (int i = 0; i < 4; ++i) {
                    add(kSlot[i], e->modifierSlots[std::size_t(i)] ? std::format("COLOR{}", e->modifierSlots[std::size_t(i)]) : NOX_TR("content.record.none", "none"));
                }
                add("CLASSUSE", e->classUse ? std::format("{}", *e->classUse) : NOX_TR("content.record.anyone", "anyone"));
                add("DESC", e->desc, str(strings, e->desc));
                if (ref.list == RecordList::Armor) {
                    add("ARMOR_VALUE", std::format("{}", e->armorValue));
                } else {
                    add("DAMAGE_MIN", std::to_string(e->damageMin));
                    add("DAMAGE_COEFFICIENT", std::format("{}", e->damageCoefficient));
                    add("REQUIRED_STRENGTH", std::to_string(e->requiredStrength));
                    add("RANGE", std::format("{}", e->range));
                }
                add("DURABILITY", std::to_string(e->durability));
            }
        }
        break;
    case RecordList::Modifier:
        if (const auto* md = files.modifiers()) {
            if (const auto* m = at(md->modifiers)) {
                add(NOX_TR("content.label.section", "Section"), m->section);
                add(NOX_TR("content.label.id", "Id"), std::to_string(m->id), NOX_TR("content.note.modifier_id", "The number items carry for it (network and saves): its place among all modifiers."));
                add("COLOR", rgb(m->color));
                add("DESC", m->desc, str(strings, m->desc));
                if (!m->secondaryDesc.empty()) {
                    add("SECONDARYDESC", m->secondaryDesc, str(strings, m->secondaryDesc));
                }
                if (!m->identifyDesc.empty()) {
                    add("IDENTIFYDESC", m->identifyDesc, str(strings, m->identifyDesc));
                }
                for (const auto& [name, value] : m->effects) {
                    add(NOX_TR("content.label.effect", "Effect"), std::format("{} {}", name, value));
                }
                add("ALLOWED_WEAPONS", std::format("0x{:X}", m->allowedWeapons));
                add("ALLOWED_ARMOR", std::format("0x{:X}", m->allowedArmor));
                add("ALLOWED_POSITION", std::to_string(m->allowedPosition));
                add("WORTH", std::to_string(m->worth));
            }
        }
        break;
    case RecordList::Tuning:
        if (const auto* gd = files.gameData()) {
            if (const auto* e = at(gd->entries)) {
                add(NOX_TR("content.label.name", "Name"), e->name);
                add(NOX_TR("content.label.game", "Game"), gameDataSection(e->section), NOX_TR("content.note.game", "SOLO lines are for the solo game, ARENA lines for network games, unprefixed lines for both."));
                for (std::size_t i = 0; i < e->values.size(); ++i) {
                    add(e->values.size() == 1 ? NOX_TR("content.label.value", "Value") : nox::l10n::format(NOX_TR("content.label.value_n", "Value {n}"), {{"n", i + 1}}), std::format("{}", e->values[i]));
                }
            }
        }
        break;
    case RecordList::Monster: {
        const auto* solo = files.monstersSolo() && ref.index >= 0 && std::size_t(ref.index) < files.monstersSolo()->monsters.size()
                               ? &files.monstersSolo()->monsters[std::size_t(ref.index)]
                               : nullptr;
        const auto* arena = solo && files.monstersArena() ? files.monstersArena()->find(solo->name) : nullptr;
        monsterFields(f, solo, arena);
        break;
    }
    case RecordList::SoundSet:
        if (const auto* ss = files.soundSets()) {
            if (const auto* s = at(ss->sets)) {
                for (int slot = 1; slot <= 18; ++slot) {
                    add(kSoundSlotKeys[slot], soundLabel(s->sounds[std::size_t(slot)]));
                }
            }
        }
        break;
    case RecordList::String:
        if (strings) {
            if (const auto* e = at(strings->entries())) {
                add(NOX_TR("content.label.label", "Label"), e->label);
                for (std::size_t i = 0; i < e->variants.size(); ++i) {
                    const auto& v = e->variants[i];
                    add(e->variants.size() == 1 ? NOX_TR("content.label.text", "Text") : nox::l10n::format(NOX_TR("content.label.variant_n", "Variant {n}"), {{"n", i + 1}}), nox::strings::toUtf8(v.text));
                    if (!v.extra.empty()) {
                        add(NOX_TR("content.label.speech_file", "Speech file"), v.extra);
                    }
                }
            }
        }
        break;
    case RecordList::Sample:
        if (auto* bag = files.audio()) {
            if (const auto* b = at(bag->samples())) {
                add(NOX_TR("content.label.name", "Name"), b->name);
                add(NOX_TR("content.label.rate", "Rate"), nox::l10n::format(NOX_TR("content.record.hz", "{n} Hz"), {{"n", b->rate}}));
                add(NOX_TR("content.label.channels", "Channels"), std::to_string(b->channels()));
                add(NOX_TR("content.label.format", "Format"), b->flags & b->kAdpcm ? nox::l10n::format(NOX_TR("content.record.adpcm_blocks", "IMA ADPCM, {n}-byte blocks"), {{"n", b->blockAlign}}) : b->flags & b->k16Bit ? NOX_TR("content.record.pcm16", "16-bit PCM") : NOX_TR("content.record.pcm8", "8-bit PCM"));
                add(NOX_TR("content.label.size", "Size"), nox::l10n::format(NOX_TR("content.record.bytes", "{n} bytes"), {{"n", b->size}}));
                // How long and how loud, decoded as the game plays it.
                try {
                    const auto pcm = bag->decode(std::size_t(ref.index));
                    double peak = 0, sum = 0;
                    for (const std::int16_t s : pcm.samples) {
                        const double v = std::abs(double(s)) / 32768.0;
                        peak = std::max(peak, v);
                        sum += v * v;
                    }
                    const double rms = pcm.samples.empty() ? 0 : std::sqrt(sum / double(pcm.samples.size()));
                    const auto decibels = [](double v) { return v > 0 ? 20 * std::log10(v) : -120.0; };
                    add(NOX_TR("content.label.length", "Length"), nox::l10n::format(NOX_TR("content.record.seconds", "{n} s"), {{"n", std::format("{:.2f}", double(pcm.frames()) / std::max(1u, pcm.rate))}}));
                    add(NOX_TR("content.label.loudness", "Loudness"), nox::l10n::format(NOX_TR("content.record.loudness", "peak {peak} dB, RMS {rms} dB"), {{"peak", std::format("{:.1f}", decibels(peak))}, {"rms", std::format("{:.1f}", decibels(rms))}}), NOX_TR("content.note.loudness", "Full scale is 0 dB."));
                } catch (const std::exception& ex) {
                    add(NOX_TR("content.label.loudness", "Loudness"), nox::l10n::format(NOX_TR("content.record.cant_decode", "can't decode: {error}"), {{"error", ex.what()}}));
                }
            }
        }
        break;
    case RecordList::File:
    case RecordList::View:
        break;
    }
    return f;
}

std::string rawText(const GameFiles& files, const RecordRef& ref)
{
    if (ref.list == RecordList::Thing && files.things() && ref.index >= 0 && std::size_t(ref.index) < files.things()->things.size()) {
        const auto& t = files.things()->things[std::size_t(ref.index)];
        std::string out = t.name + "\n";
        for (const auto& a : t.attributes) {
            out += "    " + a + "\n";
        }
        return out;
    }
    return {};
}

// ---------------------------------------------------------------------------------------------
// References

std::vector<Reference> references(const GameFiles& files, const ContentIndex& index, const RecordRef& ref)
{
    std::vector<Reference> out;
    const auto link = [&](std::string text, RecordList list, std::string_view name) {
        if (name.empty()) {
            return;
        }
        out.push_back({std::move(text), index.findByName(list, name)});
    };
    const Record* self = index.find(ref);
    if (!self) {
        return out;
    }
    const std::string& name = self->name;
    switch (ref.list) {
    case RecordList::Thing:
        if (const auto* m = files.monstersSolo() ? files.monstersSolo()->find(name) : nullptr) {
            link(nox::l10n::format(NOX_TR("content.ref.monster_bin", "monster.bin: {name}"), {{"name", m->name}}), RecordList::Monster, m->name);
        }
        if (files.soundSets() && files.soundSets()->find(name)) {
            link(nox::l10n::format(NOX_TR("content.ref.sound_set", "sound set: {name}"), {{"name", name}}), RecordList::SoundSet, name);
        }
        if (const auto* md = files.modifiers()) {
            // (By the entry's own name: a pack's type without one of its own has its stock type's.)
            if (const auto* weapon = md->findWeapon(name)) {
                link(nox::l10n::format(NOX_TR("content.ref.weapon", "modifier.bin weapon: {name}"), {{"name", weapon->name}}), RecordList::Weapon, weapon->name);
            }
            if (const auto* armour = md->findArmor(name)) {
                link(nox::l10n::format(NOX_TR("content.ref.armour", "modifier.bin armour: {name}"), {{"name", armour->name}}), RecordList::Armor, armour->name);
            }
        }
        if (const auto* db = files.things(); db && std::size_t(ref.index) < db->things.size()) { // (not a pack's new type)
            const auto& t = db->things[std::size_t(ref.index)];
            // Its texts (PRETTYNAME and DESCRIPTION name string ids) and sounds.
            for (const char* key : {"PRETTYNAME", "DESCRIPTION"}) {
                if (const auto v = t.attribute(key)) {
                    link(nox::l10n::format(NOX_TR("content.ref.string_key", "string ({key}): {name}"), {{"key", key}, {"name", *v}}), RecordList::String, *v);
                }
            }
            if (const auto v = t.attribute("AUDIOLOOP")) {
                link(nox::l10n::format(NOX_TR("content.ref.audioloop", "sound event (AUDIOLOOP): {name}"), {{"name", *v}}), RecordList::SoundEvent, *v);
            }
        }
        break;
    case RecordList::Monster:
        link(nox::l10n::format(NOX_TR("content.ref.thing", "thing: {name}"), {{"name", name}}), RecordList::Thing, name);
        if (files.soundSets() && files.soundSets()->find(name)) {
            link(nox::l10n::format(NOX_TR("content.ref.sound_set", "sound set: {name}"), {{"name", name}}), RecordList::SoundSet, name);
        }
        if (const auto* m = files.monstersSolo() ? files.monstersSolo()->find(name) : nullptr; m && !m->missile.empty()) {
            link(nox::l10n::format(NOX_TR("content.ref.missile", "missile: {name}"), {{"name", m->missile}}), RecordList::Thing, m->missile);
        }
        break;
    case RecordList::SoundSet: {
        link(nox::l10n::format(NOX_TR("content.ref.thing", "thing: {name}"), {{"name", name}}), RecordList::Thing, name);
        if (files.monstersSolo() && files.monstersSolo()->find(name)) {
            link(nox::l10n::format(NOX_TR("content.ref.monster_bin", "monster.bin: {name}"), {{"name", name}}), RecordList::Monster, name);
        }
        if (const auto* s = files.soundSets() ? &files.soundSets()->sets[std::size_t(ref.index)] : nullptr) {
            std::set<std::string> seen;
            for (int slot = 1; slot <= 18; ++slot) {
                const std::uint32_t id = s->sounds[std::size_t(slot)];
                const std::string n = id ? std::string(nox::audio::soundName(id)) : std::string();
                if (!n.empty() && seen.insert(n).second) {
                    link(nox::l10n::format(NOX_TR("content.ref.sound_event", "sound event: {name}"), {{"name", n}}), RecordList::SoundEvent, n);
                }
            }
        }
        break;
    }
    case RecordList::Weapon:
    case RecordList::Armor:
        link(nox::l10n::format(NOX_TR("content.ref.thing", "thing: {name}"), {{"name", name}}), RecordList::Thing, name);
        break;
    case RecordList::SoundEvent:
        if (const auto* db = files.things()) {
            for (const auto& f : db->audio[std::size_t(ref.index)].files) {
                const std::string base = fs::path(f).stem().string();
                link(nox::l10n::format(NOX_TR("content.ref.audio_bag", "Audio.bag: {name}"), {{"name", base}}), RecordList::Sample, base);
            }
        }
        break;
    case RecordList::Spell:
        if (const auto* db = files.things()) {
            const auto& s = db->spells[std::size_t(ref.index)];
            link(nox::l10n::format(NOX_TR("content.ref.string", "string: {name}"), {{"name", s.title}}), RecordList::String, s.title);
            link(nox::l10n::format(NOX_TR("content.ref.string", "string: {name}"), {{"name", s.description}}), RecordList::String, s.description);
            for (const auto* snd : {&s.castSound, &s.onSound, &s.offSound}) {
                if (*snd != "NULL") {
                    link(nox::l10n::format(NOX_TR("content.ref.sound_event", "sound event: {name}"), {{"name", *snd}}), RecordList::SoundEvent, *snd);
                }
            }
        }
        break;
    case RecordList::Modifier:
        if (const auto* md = files.modifiers()) {
            const auto& m = md->modifiers[std::size_t(ref.index)];
            link(nox::l10n::format(NOX_TR("content.ref.string", "string: {name}"), {{"name", m.desc}}), RecordList::String, m.desc);
            link(nox::l10n::format(NOX_TR("content.ref.string", "string: {name}"), {{"name", m.identifyDesc}}), RecordList::String, m.identifyDesc);
        }
        break;
    case RecordList::String:
        if (const auto* st = files.strings(); st && std::size_t(ref.index) < st->entries().size()) {
            const auto& e = st->entries()[std::size_t(ref.index)];
            // Its spoken line (a W variant's speech file), the window it belongs to, the types naming it.
            for (const auto& v : e.variants) {
                if (!v.extra.empty()) {
                    link(nox::l10n::format(NOX_TR("content.ref.dialog", "dialog: {name}"), {{"name", v.extra + ".wav"}}), RecordList::File, fs::path(v.extra).stem().string() + ".wav");
                }
            }
            if (const auto colon = e.label.find(':'); colon != std::string::npos && lowered(e.label.substr(0, colon)).ends_with(".wnd")) {
                link(nox::l10n::format(NOX_TR("content.ref.window", "window: {name}"), {{"name", e.label.substr(0, colon)}}), RecordList::File, e.label.substr(0, colon));
            }
            if (const auto* db = files.things()) {
                for (const auto& t : db->things) {
                    for (const char* key : {"PRETTYNAME", "DESCRIPTION"}) {
                        if (const auto v = t.attribute(key); v && nox::util::equalsNoCase(*v, e.label)) {
                            link(nox::l10n::format(NOX_TR("content.ref.thing_key", "thing ({key}): {name}"), {{"key", key}, {"name", t.name}}), RecordList::Thing, t.name);
                        }
                    }
                }
            }
        }
        break;
    case RecordList::File:
        if (ref.kind == FileKind::Dialog && std::size_t(ref.index) < files.dialog().size()) {
            // The labels that speak it.
            const std::string stem = files.dialog()[std::size_t(ref.index)].stem().string();
            if (const auto* st = files.strings()) {
                for (const auto& e : st->entries()) {
                    for (const auto& v : e.variants) {
                        if (!v.extra.empty() && nox::util::equalsNoCase(fs::path(v.extra).stem().string(), stem)) {
                            link(nox::l10n::format(NOX_TR("content.ref.string", "string: {name}"), {{"name", e.label}}), RecordList::String, e.label);
                            break;
                        }
                    }
                }
            }
        }
        break;
    default:
        break;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Preview

PreviewInfo previewOf(const GameFiles& files, const RecordRef& ref)
{
    PreviewInfo p;
    const auto* db = files.things();
    const auto in = [&](const auto& list) { return ref.index >= 0 && std::size_t(ref.index) < list.size(); };
    switch (ref.list) {
    case RecordList::Thing:
        if (db && in(db->things)) {
            const auto& t = db->things[std::size_t(ref.index)];
            if (t.menuIcon && t.menuIcon->index >= 0) {
                p.frames.push_back(t.menuIcon->index);
            }
        }
        break;
    case RecordList::Spell:
        if (db && in(db->spells)) {
            p.frames.push_back(db->spells[std::size_t(ref.index)].icon.index);
        }
        break;
    case RecordList::Ability:
        if (db && in(db->abilities)) {
            for (const auto& i : db->abilities[std::size_t(ref.index)].icons) {
                p.frames.push_back(i.index);
            }
            p.frameDelay = 15;
        }
        break;
    case RecordList::Floor:
        if (db && in(db->floors)) {
            for (const auto& i : db->floors[std::size_t(ref.index)].images) {
                p.frames.push_back(i.index);
            }
            p.frameDelay = 15;
        }
        break;
    case RecordList::Edge:
        if (db && in(db->edges)) {
            for (const auto& i : db->edges[std::size_t(ref.index)].images) {
                p.frames.push_back(i.index);
            }
            p.frameDelay = 15;
        }
        break;
    case RecordList::Wall:
        if (db && in(db->walls)) {
            for (const auto& d : db->walls[std::size_t(ref.index)].directions) {
                for (const auto& v : d) {
                    p.frames.push_back(v[0].image.index);
                }
            }
            p.frameDelay = 15;
        }
        break;
    case RecordList::NamedImage:
        if (db && in(db->images)) {
            const auto& im = db->images[std::size_t(ref.index)];
            if (im.image) {
                p.frames.push_back(im.image->index);
            } else if (im.animation) {
                for (const auto& f : im.animation->frames) {
                    p.frames.push_back(f.index);
                }
                p.frameDelay = im.animation->frameDelay;
            }
        }
        break;
    case RecordList::SoundEvent:
        if (db && in(db->audio)) {
            for (const auto& f : db->audio[std::size_t(ref.index)].files) {
                p.sounds.push_back(fs::path(f).stem().string());
            }
        }
        break;
    case RecordList::SoundEventAvnt:
        if (db && in(db->avnts) && db->avnts[std::size_t(ref.index)].files) {
            for (const auto& f : *db->avnts[std::size_t(ref.index)].files) {
                p.sounds.push_back(fs::path(f).stem().string());
            }
        }
        break;
    case RecordList::Sample:
        if (auto* bag = files.audio(); bag && in(bag->samples())) {
            p.sounds.push_back(bag->samples()[std::size_t(ref.index)].name);
        }
        break;
    case RecordList::String:
        if (const auto* st = files.strings(); st && in(st->entries())) {
            const auto& e = st->entries()[std::size_t(ref.index)];
            if (!e.variants.empty()) {
                p.text = nox::strings::toUtf8(e.variants.front().text);
            }
        }
        break;
    default:
        break;
    }
    std::erase(p.frames, -1);
    return p;
}

} // namespace videotools::content
