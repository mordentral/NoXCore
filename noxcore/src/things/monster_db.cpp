// monster.bin reader (format in monster_db.hpp): whitespace-separated KEY value pairs per monster.
#include "noxcore/things/monster_db.hpp"
#include "noxcore/util/x87.hpp"

#include "noxcore/content/content_pack.hpp"
#include "noxcore/crypto/noxcrypt.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace nox::things {

namespace {

using util::equalsNoCase;

/// readStr's tokens, and the exe's rest-of-the-line read.
struct Tokens {
    std::string_view text;
    std::size_t at = 0;

    bool next(std::string& out)
    {
        while (at < text.size() && (std::isspace(static_cast<unsigned char>(text[at])) || text[at] == '\0')) {
            ++at;
        }
        if (at >= text.size()) {
            return false;
        }
        const std::size_t start = at;
        while (at < text.size() && !std::isspace(static_cast<unsigned char>(text[at])) && text[at] != '\0') {
            ++at;
        }
        out.assign(text.substr(start, at - start));
        return true;
    }
    void skipLine()
    {
        while (at < text.size() && text[at] != '\n') {
            ++at;
        }
    }
};

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

void MonsterDb::reindex()
{
    index.clear();
    index.reserve(monsters.size());
    for (std::size_t i = 0; i < monsters.size(); ++i) {
        index.try_emplace(monsters[i].name, static_cast<std::uint32_t>(i)); // (the first of a name kept)
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
    // One record per monster: its name, then KEY value pairs up to END.
    while (t.next(tok)) {
        MonsterDef m;
        m.name = tok;
        while (t.next(tok) && !equalsNoCase(tok, "END")) {
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
            t.next(v);
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
                for (std::size_t d = 0; d < std::size(kDamage); ++d) {
                    if (equalsNoCase(v, kDamage[d])) {
                        m.meleeDamageType = static_cast<int>(d);
                    }
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
            else if (equalsNoCase(key, "MISSILE_NAME")) m.missile = equalsNoCase(v, "NULL") ? std::string() : v;
            else if (equalsNoCase(key, "MISSILE_ATTACK_RANGE")) m.missileRange = f();
            else if (equalsNoCase(key, "MISSILE_ATTACK_FRAME")) m.missileFrame = i();
            else if (equalsNoCase(key, "MISSILE_ATTACK_MIN_DELAY")) m.missileMinDelay = i();
            else if (equalsNoCase(key, "MISSILE_ATTACK_MAX_DELAY")) m.missileMaxDelay = i();
            else if (equalsNoCase(key, "MELEE_STRIKE_FUNCTION")) m.strikeFn = equalsNoCase(v, "NULL") ? std::string() : v;
            else if (equalsNoCase(key, "DIE_FUNCTION")) m.dieFn = equalsNoCase(v, "NULL") ? std::string() : v;
            else if (equalsNoCase(key, "DEAD_FUNCTION")) m.deadFn = equalsNoCase(v, "NULL") ? std::string() : v;
            // (An unknown key: the exe gives up on the file there; here it's passed over.)
        }
        db.monsters.push_back(std::move(m));
    }
    db.reindex();
    return db;
}

MonsterDb loadMonsterDb(const std::filesystem::path& path, bool solo)
{
    auto data = readFileBytes(path);
    crypto::cipherForKey(crypto::FileKey::MonsterBin).decrypt(data);
    content::applyContentPacks(content::TableFormat::Monster, data); // (the installed content packs' changes, once, as it loads; none installed: nothing)
    return parseMonsterDb(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), solo);
}

} // namespace nox::things
