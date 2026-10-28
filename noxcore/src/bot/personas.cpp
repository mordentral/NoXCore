// The roster's data and lookups (personas.hpp).
#include "noxcore/bot/personas.hpp"

#include "noxcore/util/text.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <vector>

namespace nox::bot {

namespace {

using T = Tier;
using M = Temper;
using S = Style;

// One name each (the legendary ones aside: Jack Mower), all different, so a line names one.
// The campaign's own people (Jandor, Horrendous, Gearhart, Jack Mower, Horvath, Hecubah, Aldwyn,
// Theogrin) are the hardest.
// Each: name, class (0 warrior, 1 wizard, 2 conjurer), tier, temper, style, life before, now.
constexpr std::array<Persona, 75> kPersonas{{
    // ---- Warriors: the Fire Knights' kind
    {"Perrin", 0, T::Easy, M::Cheerful, S::Balanced, "a baker's boy in Brin", "Took up a sword when the bread ran out."},
    {"Quill", 0, T::Easy, M::Friendly, S::Balanced, "a shepherd in the hills", "Still counts heads like sheep after a fight."},
    {"Ivo", 0, T::Easy, M::Cheerful, S::Balanced, "a stable hand", "Always a step late and cheerful about it."},
    {"Garrick", 0, T::Easy, M::Gruff, S::Balanced, "a ditch digger", "Swings an axe like a spade."},
    {"Bram", 0, T::Easy, M::Friendly, S::Balanced, "a tavern keeper", "Misses the taproom more than he admits."},
    {"Torvald", 0, T::Medium, M::Proud, S::Balanced, "a smith's apprentice", "Forged his own armour and never lets anyone forget it."},
    {"Rurik", 0, T::Medium, M::Gruff, S::Balanced, "a quarryman", "Speaks little and hits hard."},
    {"Magnus", 0, T::Medium, M::Cheerful, S::Balanced, "a caravan guard", "Loves a good brawl and a better story."},
    {"Isolde", 0, T::Medium, M::Proud, S::Balanced, "a woodcutter's daughter", "Felled trees first, now fells foes."},
    {"Hilda", 0, T::Medium, M::Mean, S::Balanced, "a butcher", "Has no patience for weaklings."},
    {"Ulric", 0, T::Medium, M::Calm, S::Balanced, "a gate warden of Dun Mir", "Served the Fire Knights' gate for years."},
    {"Wulfric", 0, T::Medium, M::Friendly, S::Balanced, "a candlemaker", "Brightens up a dark fight with bad jokes."},
    {"Dorn", 0, T::Medium, M::Calm, S::Balanced, "a monk's porter", "Says almost nothing; his hammer speaks."},
    {"Nils", 0, T::Medium, M::Gruff, S::Balanced, "a miner", "Used to the dark and the heavy work."},
    {"Kara", 0, T::Medium, M::Proud, S::Balanced, "one of the Queen's guard", "Stood guard at court before the war called her."},
    {"Jandor", 0, T::Hard, M::Calm, S::Balanced, "the hero who beat the Necromancers with the Staff of Oblivion",
     "Trapped their souls in the Orb long ago; now an old airship captain."},
    {"Horrendous", 0, T::Hard, M::Proud, S::Balanced, "Warlord of the Fire Knights of Dun Mir",
     "Keeps the Halberd of Horrendous, the warriors' piece of the Staff."},
    {"Gearhart", 0, T::Hard, M::Gruff, S::Balanced, "the engineer of Dun Mir", "Builds what the Fire Knights need, then fights with it."},
    {"Jack Mower", 0, T::Hard, M::Cheerful, S::Balanced, "a man from another world, pulled into Nox by the Orb",
     "Lived in a trailer with Tina; now the one who took down Hecubah."},
    {"Thane", 0, T::Hard, M::Proud, S::Balanced, "a hunter of the Grok Torr ogres", "Wears an ogre's tooth for each one he felled."},
    {"Morgrin", 0, T::Hard, M::Mean, S::Balanced, "a pit fighter", "Fights dirty and laughs about it."},
    {"Brannoc", 0, T::Hard, M::Calm, S::Balanced, "a veteran of the Field of Valor", "Old, scarred and patient as a stone."},
    {"Varek", 0, T::Hard, M::Gruff, S::Balanced, "a sellsword", "Fights for whoever pays and never loses."},
    {"Kael", 0, T::Hard, M::Mean, S::Balanced, "a raider from the Dismal Swamp", "Takes no prisoners and gives no quarter."},
    {"Ragna", 0, T::Hard, M::Proud, S::Balanced, "a champion of the arena", "Has never lost a duel she wanted to win."},
    // ---- Wizards: Castle Galava's kind
    {"Ysolde", 1, T::Easy, M::Friendly, S::Protective, "a scribe", "Copies spells more than she casts them."},
    {"Finnick", 1, T::Easy, M::Cheerful, S::Balanced, "a candlemaker's apprentice", "Set his first fire by accident."},
    {"Edda", 1, T::Easy, M::Gruff, S::Combative, "a washerwoman", "Learned fire to keep warm, now uses it to win."},
    {"Mira", 1, T::Easy, M::Friendly, S::Protective, "an herbalist's daughter", "Would rather shield a friend than burn a foe."},
    {"Pell", 1, T::Easy, M::Calm, S::Balanced, "a copyist at Galava", "Never quite gets the ink off his fingers."},
    {"Sable", 1, T::Medium, M::Mean, S::Combative, "a fortune teller", "Sharp tongue, sharper lightning."},
    {"Kestrel", 1, T::Medium, M::Calm, S::Balanced, "a lamplighter", "Moves quietly and strikes from far off."},
    {"Lyssa", 1, T::Medium, M::Friendly, S::Protective, "a midwife", "Keeps her friends alive first."},
    {"Selka", 1, T::Medium, M::Proud, S::Combative, "a noble's tutor", "Thinks the warriors are brutes."},
    {"Elric", 1, T::Medium, M::Gruff, S::Combative, "a glassblower", "Burned hands, burning temper."},
    {"Cormac", 1, T::Medium, M::Cheerful, S::Balanced, "a wandering map seller", "Gets lost often, never in a fight."},
    {"Gwyn", 1, T::Medium, M::Proud, S::Protective, "a page at Castle Galava", "Proud of the castle and its Heart of Nox."},
    {"Lothar", 1, T::Medium, M::Mean, S::Balanced, "a tax collector", "Collects debts in fire now."},
    {"Fenna", 1, T::Medium, M::Calm, S::Protective, "a librarian", "Reads her enemies like a book."},
    {"Hesta", 1, T::Medium, M::Gruff, S::Combative, "a fishwife", "Cold eyes, hot fireballs."},
    {"Horvath", 1, T::Hard, M::Proud, S::Balanced, "Arch-Wizard of Castle Galava",
     "Sent the Orb away to the other world long ago; keeps the Heart of Nox."},
    {"Hecubah", 1, T::Hard, M::Mean, S::Combative, "the Queen of the Necromancers, raised by Ogres",
     "Summoned the Orb back to Nox; holds a grudge against everyone."},
    {"Grimwald", 1, T::Hard, M::Gruff, S::Combative, "a lighthouse keeper whose lamp went out", "Bitter, and deadly at range."},
    {"Mordrake", 1, T::Hard, M::Mean, S::Combative, "a disgraced court mage", "Wants to prove Galava wrong for casting him out."},
    {"Azrael", 1, T::Hard, M::Proud, S::Balanced, "an astronomer", "Counts stars and kills."},
    {"Valdris", 1, T::Hard, M::Calm, S::Protective, "a hermit of the marshes", "Patient, warded, and never in a hurry."},
    {"Seraphine", 1, T::Hard, M::Proud, S::Combative, "a duelist of the wizards' circle", "Has burned more rivals than she can name."},
    {"Ozric", 1, T::Hard, M::Cheerful, S::Combative, "a weather watcher", "Laughs when the lightning comes."},
    {"Malakar", 1, T::Hard, M::Mean, S::Combative, "a gravedigger", "Spent too long near the dead."},
    {"Ilsabet", 1, T::Hard, M::Calm, S::Protective, "a weaver", "Weaves shields as tight as her cloth."},
    // ---- Conjurers: the Temple of Ix's kind
    {"Gorrin", 2, T::Easy, M::Friendly, S::Balanced, "a goatherd", "Talks to animals more than people."},
    {"Jarek", 2, T::Easy, M::Calm, S::Protective, "a beekeeper", "Walks with a stick; his bees do the rest."},
    {"Quinn", 2, T::Easy, M::Cheerful, S::Summoner, "a farmer", "Calls the critters of the field to help."},
    {"Tobin", 2, T::Easy, M::Friendly, S::Balanced, "a fisherman", "Patient as a man with a line in the water."},
    {"Nessa", 2, T::Easy, M::Gruff, S::Combative, "a shepherdess", "Fierce for one who minded sheep."},
    {"Fenwick", 2, T::Medium, M::Calm, S::Summoner, "a forester", "The wolves of the wood answer him."},
    {"Bryn", 2, T::Medium, M::Friendly, S::Protective, "a healer's helper", "Mends friends before hurting foes."},
    {"Halvard", 2, T::Medium, M::Proud, S::Summoner, "a huntsman", "Never hunts alone."},
    {"Marta", 2, T::Medium, M::Gruff, S::Combative, "a tanner", "Tough as old leather."},
    {"Silas", 2, T::Medium, M::Mean, S::Combative, "a poisoner's apprentice", "Knows every poison of the swamp."},
    {"Wren", 2, T::Medium, M::Cheerful, S::Summoner, "a bird catcher", "Friends with every bat and wasp."},
    {"Corwin", 2, T::Medium, M::Calm, S::Balanced, "a charcoal burner", "Quiet, steady and hard to kill."},
    {"Ida", 2, T::Medium, M::Friendly, S::Protective, "a midwife of Ix", "Keeps everyone breathing."},
    {"Rowan", 2, T::Medium, M::Proud, S::Balanced, "a deer hunter", "Never misses with a crossbow."},
    {"Elsbeth", 2, T::Medium, M::Calm, S::Summoner, "a swamp guide", "The swamp's creatures follow her."},
    {"Aldwyn", 2, T::Hard, M::Calm, S::Summoner, "the Conjurer of the Temple of Ix",
     "Keeps the Weirdling Beast, the conjurers' piece of the Staff."},
    {"Theogrin", 2, T::Hard, M::Proud, S::Protective, "Mayor of the Village of Ix", "Runs Ix and won't let anyone forget it."},
    {"Morrigan", 2, T::Hard, M::Mean, S::Combative, "a witch of the Dismal Swamp", "Curses first, asks never."},
    {"Kirrin", 2, T::Hard, M::Proud, S::Summoner, "a tamer of beasts", "Fights behind a wall of fangs."},
    {"Ulfgar", 2, T::Hard, M::Gruff, S::Summoner, "a trapper", "Lives with the bears and fights like one."},
    {"Sylvara", 2, T::Hard, M::Calm, S::Summoner, "a wolf keeper", "Her wolves never leave her side."},
    {"Draven", 2, T::Hard, M::Mean, S::Combative, "a grave robber", "Cold, clever and cruel."},
    {"Thalia", 2, T::Hard, M::Proud, S::Combative, "a dragon-watcher", "Brings the fist of heaven down on anyone."},
    {"Varn", 2, T::Hard, M::Gruff, S::Balanced, "a horn blower of Ix", "Loud, stubborn and never beaten for long."},
    {"Oriel", 2, T::Hard, M::Cheerful, S::Summoner, "a keeper of pixies", "Never alone: the swarm is always with her."},
}};

using util::lowered;

} // namespace

std::span<const Persona> personas()
{
    return kPersonas;
}

bool isLegendary(const Persona& p)
{
    static constexpr std::array<std::string_view, 8> kLegends{"Jandor",  "Horrendous", "Gearhart", "Jack Mower",
                                                              "Horvath", "Hecubah",    "Aldwyn",   "Theogrin"};
    return std::ranges::find(kLegends, p.name) != kLegends.end();
}

std::string playerNameOf(const Persona& p)
{
    return "[BOT] " + std::string(p.name) + (isLegendary(p) ? "*" : "");
}

const Persona* personaNamed(std::string_view name)
{
    if (name.starts_with("[BOT] ")) {
        name.remove_prefix(6);
    }
    if (name.ends_with('*')) {
        name.remove_suffix(1); // (the legendary mark)
    }
    const auto it = std::ranges::find_if(kPersonas, [&](const Persona& p) { return lowered(p.name) == lowered(name); });
    return it != kPersonas.end() ? &*it : nullptr;
}

const Persona* pickPersona(std::uint8_t playerClass, Tier minTier, Tier maxTier, const std::set<std::string>& taken,
                           std::mt19937& rng)
{
    if (maxTier < minTier) {
        std::swap(minTier, maxTier);
    }
    std::set<std::string> takenLower;
    for (const auto& t : taken) {
        takenLower.insert(lowered(t));
    }
    std::vector<const Persona*> free;
    for (const auto& p : kPersonas) {
        // Free: not in play under either form of its name (with or without the legendary mark).
        if (p.playerClass == playerClass && p.tier >= minTier && p.tier <= maxTier &&
            !takenLower.contains(lowered(playerNameOf(p))) && !takenLower.contains(lowered("[BOT] " + std::string(p.name)))) {
            free.push_back(&p);
        }
    }
    if (free.empty()) {
        return nullptr;
    }
    return free[nox::util::uniform_int_distribution<std::size_t>(0, free.size() - 1)(rng)];
}

std::string_view tierName(Tier tier)
{
    switch (tier) {
    case Tier::Easy: return "easy";
    case Tier::Medium: return "medium";
    case Tier::Hard: return "hard";
    }
    return "medium";
}

std::optional<Tier> tierNamed(std::string_view name)
{
    const std::string n = lowered(name);
    if (n == "easy") {
        return Tier::Easy;
    }
    if (n == "medium" || n == "normal") {
        return Tier::Medium;
    }
    if (n == "hard") {
        return Tier::Hard;
    }
    return std::nullopt;
}

} // namespace nox::bot
