// monster.bin reader (format in monster_db.hpp): whitespace-separated KEY value pairs per monster.
#include "noxcore/things/monster_db.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/x87.hpp"

#include "noxcore/content/content_pack.hpp"
#include "noxcore/crypto/noxcrypt.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <string>

namespace nox::things {

namespace {

using util::equalsNoCase;

/// readStr's tokens, and the exe's rest-of-the-line read. A "//" skips to the line's end, as
/// readStr's does; where it comes inside a word, the word's letters before it are kept (the game's
/// lose them and read on into the next line: `cut` says it happened, for the problems' list).
struct Tokens {
    std::string_view text;
    std::size_t at = 0;
    bool cut = false; ///< the last word was ended by a "//" inside it

    static bool space(char c) { return std::isspace(static_cast<unsigned char>(c)) || c == '\0'; }
    bool comment() const { return at + 1 < text.size() && text[at] == '/' && text[at + 1] == '/'; }

    bool next(std::string& out)
    {
        cut = false;
        for (;;) {
            while (at < text.size() && space(text[at])) {
                ++at;
            }
            if (!comment()) {
                break;
            }
            skipLine();
        }
        if (at >= text.size()) {
            return false;
        }
        const std::size_t start = at;
        while (at < text.size() && !space(text[at]) && !comment()) {
            ++at;
        }
        out.assign(text.substr(start, at - start));
        if (comment()) {
            cut = true;
            skipLine();
        }
        return true;
    }
    void skipLine()
    {
        while (at < text.size() && text[at] != '\n') {
            ++at;
        }
    }
    /// The line `at` is on (1 first), for the problems' list.
    std::size_t line() const { return 1 + static_cast<std::size_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(std::min(at, text.size())), '\n')); }
};

/// The problems listed at most (MonsterDb::problems): a hand-edited or patched file has a few; past
/// this the rest go unlisted (a garbled file's every word would be one).
constexpr std::size_t kMaxProblems = 100;

// The game's strike, die and dead function tables (their names; the readers compare them with
// case, NULL without).
constexpr std::string_view kStrikeNames[] = {"OgreStrike", "ScorpionStrike", "VileZombieStrike", "StoneGolemStrike", "MechGolemStrike", "WaspStrike",
                                             "SpiderStrike", "SpittingSpiderStrike", "GhostStrike", "BomberStrike", "MonsterStrike"};
constexpr std::string_view kDieNames[] = {"ArcherDie", "OgreDie", "SwordsmanDie", "UrchinShamanDie", "OgreWarlordDie"};
constexpr std::string_view kDeadNames[] = {"EmberDemonDead", "DemonDead", "ImpDead",      "MechGolemDead",    "GolemDead",
                                           "BomberDead",     "SpiderDead", "SkeletonDead", "SkeletonLordDead", "TrollDead"};

// STATUS names by bit number (index i sets bit 1 << i); bit 4 is unused.
constexpr const char* kStatus[] = {"DESTROY_WHEN_DEAD", "CHECK", "CAN_BLOCK", "CAN_DODGE", "unused", "CAN_CAST_SPELLS",
                                   "HOLD_YOUR_GROUND", "SUMMONED", "ALERT", "INJURED", "CAN_SEE_FRIENDS", "CAN_HEAL_SELF",
                                   "CAN_HEAL_OTHERS", "CAN_RUN", "RUNNING", "ALWAYS_RUN", "NEVER_RUN", "BOT", "MORPHED",
                                   "ON_FIRE", "STAY_DEAD", "FRUSTRATED"};

// MELEE_ATTACK_DAMAGE_TYPE names by damage type number.
constexpr const char* kDamage[] = {"BLADE", "FLAME", "CRUSH", "IMPALE", "DRAIN", "POISON", "DISPEL_UNDEAD", "EXPLOSION", "BITE",
                                   "ELECTRIC", "CLAW", "IMPACT", "LAVA", "DEATH_MAGIC", "PLASMA", "MANA_BOMB", "ZAP_RAY",
                                   "AIRBORNE_ELECTRIC"};

} // namespace

std::string monsterStatusText(std::uint32_t bits)
{
    std::string out;
    for (std::size_t b = 0; b < std::size(kStatus); ++b) {
        if (bits & (1u << b)) {
            out += (out.empty() ? "" : "+") + std::string(kStatus[b]);
        }
    }
    return out.empty() ? std::string("NULL") : out;
}

std::string_view monsterDamageTypeName(int type)
{
    return type >= 0 && static_cast<std::size_t>(type) < std::size(kDamage) ? std::string_view(kDamage[type]) : std::string_view();
}

void MonsterDb::reindex()
{
    index.clear();
    index.reserve(monsters.size());
    for (std::size_t i = 0; i < monsters.size(); ++i) {
        index.try_emplace(monsters[i].name, static_cast<std::uint32_t>(i)); // (one a name, as parseMonsterDb leaves them)
    }
    indexedOf = monsters.size();
}

const MonsterDef* MonsterDb::find(std::string_view name) const
{
    if (indexedOf == monsters.size()) {
        const auto it = index.find(name);
        return it == index.end() ? nullptr : &monsters[it->second];
    }
    for (const auto& m : monsters) {
        if (equalsNoCase(m.name, name)) {
            return &m;
        }
    }
    return nullptr;
}

MonsterDb parseMonsterDb(std::string_view text, bool solo)
{
    const util::RoundingScope nearest(false); // (the numbers read to nearest whatever thread reads them: util/x87.hpp)
    MonsterDb db;
    Tokens t{text};
    std::string tok;
    // The game's reader stops the whole file at an unknown key, function name or damage type, and
    // takes the last of two records of one name. Ours reads on (a modded file keeps its other
    // monsters) and notes each such place in `problems`: the shipped file has none, so with an
    // empty list the file read as the game reads it (docs/vanilla_deviations.md).
    const auto problem = [&](std::string_view monster, const std::string& what) {
        if (db.problems.size() < kMaxProblems) {
            db.problems.push_back("monster.bin line " + std::to_string(t.line()) + ", " + std::string(monster) + ": " + what);
        } else if (db.problems.size() == kMaxProblems) {
            db.problems.push_back("monster.bin: more problems, not listed");
        }
    };
    // Each name's record so far (two of one name: the later stands).
    std::unordered_map<std::string, std::size_t, util::NoCaseHash, util::NoCaseEqual> seen;
    const auto cutNote = [&](std::string_view monster) {
        if (t.cut) {
            problem(monster, "a // comment inside a word: the letters before it kept");
        }
    };
    // A function's name: one of the game's table (with case), NULL (without) for none; any other
    // name is none here (the game's reader stops the file there).
    const auto function = [&](std::string_view monster, std::string_view key, const std::string& v, auto& table) -> std::string {
        if (equalsNoCase(v, "NULL")) {
            return {};
        }
        if (std::ranges::find(table, std::string_view(v)) != std::end(table)) {
            return v;
        }
        problem(monster, std::string(key) + " " + v + " is no function of the game's: none");
        return {};
    };
    // One record per monster: its name, then KEY value pairs up to END.
    while (t.next(tok)) {
        MonsterDef m;
        m.name = tok;
        cutNote(m.name);
        while (t.next(tok) && !equalsNoCase(tok, "END")) {
            cutNote(m.name);
            // The other kind of game's line: skipped; its own prefix: passed over.
            if (equalsNoCase(tok, solo ? "ARENA" : "SOLO")) {
                t.skipLine();
                continue;
            }
            if (equalsNoCase(tok, solo ? "SOLO" : "ARENA")) {
                continue;
            }
            // Every remaining key takes exactly one value token; ints and floats parse as atoi/atof do (bad text gives 0).
            const std::string key = tok;
            std::string v;
            if (!t.next(v)) {
                problem(m.name, key + " has no value: passed over");
                break;
            }
            cutNote(m.name);
            const auto i = [&] { return std::atoi(v.c_str()); };
            const auto f = [&] { return static_cast<float>(std::atof(v.c_str())); };
            if (equalsNoCase(key, "EXPERIENCE")) m.experience = i();
            else if (equalsNoCase(key, "HEALTH")) m.health = i();
            else if (equalsNoCase(key, "QUESTHEALTH")) m.questHealth = i();
            else if (equalsNoCase(key, "SPEED")) m.speed = i();
            else if (equalsNoCase(key, "RUN_MULTIPLIER")) m.runMultiplier = f();
            else if (equalsNoCase(key, "RETREAT_RATIO")) m.retreatRatio = f();
            else if (equalsNoCase(key, "RESUME_RATIO")) m.resumeRatio = f();
            else if (equalsNoCase(key, "STATUS")) {
                std::uint32_t bits = 0;
                std::string_view rest = v;
                // '+'-separated names; unknown names add nothing.
                while (!rest.empty()) {
                    const auto plus = rest.find('+');
                    const std::string_view name = rest.substr(0, plus);
                    for (std::size_t b = 0; b < std::size(kStatus); ++b) {
                        if (equalsNoCase(name, kStatus[b])) {
                            bits |= 1u << b;
                        }
                    }
                    rest = plus == std::string_view::npos ? std::string_view{} : rest.substr(plus + 1);
                }
                m.status = static_cast<std::uint16_t>(bits);
            }
            else if (equalsNoCase(key, "MOVE_SOUND_FRAME_A")) m.moveSoundFrameA = i();
            else if (equalsNoCase(key, "MOVE_SOUND_FRAME_B")) m.moveSoundFrameB = i();
            else if (equalsNoCase(key, "FLEE_RANGE")) m.fleeRange = f();
            else if (equalsNoCase(key, "MELEE_ATTACK_DAMAGE")) m.meleeDamage = i();
            else if (equalsNoCase(key, "MELEE_ATTACK_DAMAGE_TYPE")) {
                bool known = false;
                for (std::size_t d = 0; d < std::size(kDamage); ++d) {
                    if (equalsNoCase(v, kDamage[d])) {
                        m.meleeDamageType = static_cast<int>(d);
                        known = true;
                    }
                }
                if (!known) { // (the game's reader stops the file there)
                    problem(m.name, "MELEE_ATTACK_DAMAGE_TYPE " + v + " is no damage type: kept as " + kDamage[m.meleeDamageType]);
                }
            }
            else if (equalsNoCase(key, "MELEE_ATTACK_IMPACT")) m.meleeImpact = f();
            else if (equalsNoCase(key, "MELEE_ATTACK_RANGE")) m.meleeRange = f();
            else if (equalsNoCase(key, "MELEE_ATTACK_FRAME")) m.meleeFrame = i();
            else if (equalsNoCase(key, "MELEE_ATTACK_MIN_DELAY")) m.meleeMinDelay = i();
            else if (equalsNoCase(key, "MELEE_ATTACK_MAX_DELAY")) m.meleeMaxDelay = i();
            else if (equalsNoCase(key, "MELEE_ATTACK_POISON_CHANCE")) m.poisonChance = i();
            else if (equalsNoCase(key, "MELEE_ATTACK_POISON_STRENGTH")) m.poisonStrength = i();
            else if (equalsNoCase(key, "MELEE_ATTACK_POISON_MAX")) m.poisonMax = i();
            else if (equalsNoCase(key, "MISSILE_NAME")) {
                // NULL: none. (The game's compares with case: "null" is a missile's name to it,
                // one no type has: its monster takes aim and fires nothing. Ours: none, noted.)
                if (equalsNoCase(v, "NULL") && v != "NULL") {
                    problem(m.name, "MISSILE_NAME " + v + ": none, as NULL");
                }
                m.missile = equalsNoCase(v, "NULL") ? std::string() : v;
            }
            else if (equalsNoCase(key, "MISSILE_ATTACK_RANGE")) m.missileRange = f();
            else if (equalsNoCase(key, "MISSILE_ATTACK_FRAME")) m.missileFrame = i();
            else if (equalsNoCase(key, "MISSILE_ATTACK_MIN_DELAY")) m.missileMinDelay = i();
            else if (equalsNoCase(key, "MISSILE_ATTACK_MAX_DELAY")) m.missileMaxDelay = i();
            else if (equalsNoCase(key, "MELEE_STRIKE_FUNCTION")) m.strikeFn = function(m.name, key, v, kStrikeNames);
            else if (equalsNoCase(key, "DIE_FUNCTION")) m.dieFn = function(m.name, key, v, kDieNames);
            else if (equalsNoCase(key, "DEAD_FUNCTION")) m.deadFn = function(m.name, key, v, kDeadNames);
            // (An unknown key: the exe gives up on the file there; here it and its value are passed over.)
            else problem(m.name, "unknown key " + key + " (its value " + v + ") passed over");
        }
        // Two records of one name: the later stands, as the game's list (built from the front,
        // looked up from the front) has it; in the earlier's place.
        if (const auto [it, fresh] = seen.try_emplace(m.name, db.monsters.size()); !fresh) {
            problem(m.name, "a second record of the name: it replaces the first");
            db.monsters[it->second] = std::move(m);
        } else {
            db.monsters.push_back(std::move(m));
        }
    }
    db.reindex();
    return db;
}

void MonsterDb::keepThingTypes(const ThingDb& things)
{
    const auto isThingType = [&](std::string_view name) { return things.findThing(name) != nullptr; };
    // The game's (monsterList) frees every record when one names no thing type: each monster then
    // without its numbers. Ours drops that record alone; a MISSILE_NAME no type has is none.
    std::erase_if(monsters, [&](const MonsterDef& m) {
        if (isThingType(m.name)) {
            return false;
        }
        problems.push_back("monster.bin, " + m.name + ": no thing type of the name: the record dropped");
        return true;
    });
    for (MonsterDef& m : monsters) {
        if (!m.missile.empty() && !isThingType(m.missile)) {
            problems.push_back("monster.bin, " + m.name + ": MISSILE_NAME " + m.missile + " is no thing type: none");
            m.missile.clear();
        }
    }
    reindex();
}

MonsterDb loadMonsterDb(const std::filesystem::path& path, bool solo)
{
    return monsterDbFromFile(readFileBytes(path), solo, true);
}

MonsterDb monsterDbFromFile(std::vector<std::uint8_t> data, bool solo, bool contentPacks)
{
    crypto::cipherForKey(crypto::FileKey::MonsterBin).decrypt(data);
    if (contentPacks) {
        content::applyContentPacks(content::TableFormat::Monster, data); // (the installed content packs' changes, once, as it loads; none installed: nothing)
    }
    return parseMonsterDb(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), solo);
}

} // namespace nox::things
