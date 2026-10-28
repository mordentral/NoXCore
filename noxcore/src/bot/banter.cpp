// The bots' lines (banter.hpp): taunts, whines and nudges for the host, and the pick of one.
#include "noxcore/bot/banter.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <array>
#include <span>

namespace nox::bot {

namespace {

// Said over a person a bot killed; "{name}" is that person's name.
constexpr std::string_view kTaunts[] = {
    "Back to the Fields of Valor with you, {name}.",
    "Was that your best, {name}?",
    "Another one for the graveyard.",
    "You fight like a Galava scribe.",
    "Rest easy, {name}. Respawn quicker.",
    "Too slow.",
    "Should've drunk that potion.",
    "Get up. Try again.",
    "Ha!",
    "Next time, bring friends.",
    "Hecubah sends her regards.",
    "Not even close, {name}.",
    "Nice try.",
    "Mind the floor, {name}. You'll be seeing a lot of it.",
    "One more for the tally.",
    "Even the urchins hit harder.",
    "Is that what they teach in Dun Mir?",
    "Sit down, {name}.",
    "Back to your trailer, {name}.",
    "Horrendous could do better, {name}.",
    "Even Captain Hacker would be embarrassed.",
    "Aldwyn would weep, {name}.",
    "Horvath taught me that one.",
    "The Necromancers will want you back.",
    "Your armour called. It wants a refund.",
    "That's a lot of red on the floor.",
    "Fireball: 1. {name}: 0.",
    "Ouch. That looked expensive.",
    "Learn to dodge, {name}.",
    "Have you tried not dying?",
    "Mana well spent.",
    "The wisps will keep you company.",
    "Shouldn't have charged me, {name}.",
    "Bad day to be {name}.",
    "Down you go.",
    "Stay down this time.",
    "Free kill. Thanks, {name}.",
    "You call that a shield?",
    "Your sword's just for show, then?",
    "I barely broke a sweat.",
    "Practise in the Gauntlet first.",
    "Even the imps fight back, {name}.",
    "Respawn and try again. I'll wait.",
    "Tell the Lich I said hello.",
    "You dropped something. Your life.",
    "Again? Really, {name}?",
    "I've squashed spiders tougher than you.",
    "Mind your step, {name}.",
    "Did nobody tell you to block?",
    "Lovely day for a funeral.",
    "Bury them with their boots on.",
    "Easy points.",
    "Thanks for the practice, {name}.",
    "Better gate yourself home, {name}.",
    "You'll want a lesser heal for that. Several.",
    "That one's going on my mantel.",
    "Someone fetch a priest. No, two.",
    "Nothing personal, {name}. Okay, a bit.",
    "The Fields of Valor are lovely this time of year.",
    "Next!",
    "That's what the war cry was for.",
    "Hit the deck, {name}.",
    "My grandmother casts faster.",
    "Swing harder next time.",
    "That armour's mostly decorative, huh?",
    "Shock and awe, {name}. Mostly shock.",
    "Was that a dodge or a dance?",
    "Sleep tight, {name}.",
    "Should've stayed in Dun Mir.",
    "Your ghost can watch the rest.",
    "Too easy. Send someone else.",
    "Hecubah wants a word, {name}.",
    "Even a mimic would've seen that coming.",
    "I'd say good game, but...",
    "Is your shield on backwards?",
    "Gravity wins again.",
    "You make this too easy, {name}.",
    "That's how it's done in Galava.",
    "Better luck next life.",
    "Pick up your things, {name}. Slowly.",
    "Who's next?",
    "Down goes {name}!",
    "Should've brought a bigger sword.",
    "Read the spellbook next time.",
    "Hear that? That was you losing.",
    "Run home, {name}. Oh, wait.",
    "Write that one down, {name}.",
    "I'll be here all game.",
    "Tread lightly next time.",
    "Victory tastes like red potions.",
    "The Ogre Warlords would be proud of me.",
    "Did that tickle, {name}?",
    "Oh, were you trying?",
};

// Said at a person who killed the bot.
constexpr std::string_view kWhines[] = {
    "Lag!",
    "Oh come on.",
    "That was lucky, {name}.",
    "I tripped.",
    "Ugh. Again?",
    "I had you, {name}!",
    "Cheap shot.",
    "Who taught you that?",
    "That one hurt.",
    "I'll remember that, {name}.",
    "My potions were in the other bag.",
    "Rigged.",
    "Next time, {name}. Next time.",
    "So much for my armour.",
    "How?!",
    "I wasn't ready.",
    "My hands slipped.",
    "Fine. You win this one.",
    "My mouse died.",
    "I was AFK. Ish.",
    "My shield was up! It was UP!",
    "Spawn camper.",
    "Did you see that hitbox?",
    "Out of mana. Of course.",
    "Who put that wall there?",
    "That's not fair, {name}.",
    "I was reading the spellbook!",
    "My cat walked on the keyboard.",
    "I blame the ogres.",
    "The server hates me.",
    "Okay, okay. Lucky shot, {name}.",
    "Wow. Just wow.",
    "You're on my list now, {name}.",
    "I was going to win that.",
    "Next round you're mine, {name}.",
    "How much health do you HAVE?!",
    "Hacks!",
    "I wasn't even fighting you!",
    "Is that even allowed?",
    "Wait, what hit me?",
    "I'll get you back, {name}.",
    "That spell's overpowered.",
    "I was admiring the scenery.",
    "The sun was in my eyes.",
    "I meant to do that.",
    "Tactical respawn.",
    "I was testing your reflexes.",
    "My armour just gave up.",
    "Ugh, the floor again.",
    "Not the face!",
    "I demand a rematch, {name}.",
    "Respawning in shame.",
    "Did anyone get the name of that fireball?",
    "That's the third time!",
    "My spellbook was upside down.",
    "I lagged into that.",
    "Why is it always me?",
    "Who's even counting?",
    "I had the flag! I HAD it!",
    "Nobody saw that. Right?",
    "It's the boots. Bad boots.",
    "Brb, crying.",
    "You got lucky. Twice.",
    "I was just warming up.",
    "Alright, {name}. Now it's personal.",
    "Rude.",
    "I thought we were friends, {name}.",
    "That came out of nowhere.",
    "You're cheating, {name}. Probably.",
    "My sword got stuck in the scenery.",
    "My teleport went the wrong way.",
    "Who stole my mana?",
    "That's going in my diary.",
    "Aaand I'm dead.",
    "I was sure that would work.",
    "Typical.",
    "I really should've blocked.",
    "Note to self: don't do that.",
    "Tell my obelisk I loved it.",
    "Every time!",
    "Pfft. Beginner's luck, {name}.",
    "I slipped on a potion.",
    "Bah! A mere flesh wound.",
    "Okay, that one was clever.",
    "Unbelievable.",
    "I'm setting the Necromancers on you.",
    "I've had better days.",
    "This keyboard is broken.",
    "I'll be back, {name}.",
    "My dog ate my mana.",
    "Was that really necessary, {name}?",
    "Hey! I was buffing!",
};

// On a chat map with people in, nudging the host to start a game.
constexpr std::string_view kWaits[] = {
    "hurry up host",
    "do it already",
    "come on host",
    "start it!",
    "any day now",
    "host pls",
    "zzz",
    "we waiting for something?",
    "start the game already",
    "did the host fall asleep?",
    "ready when you are, host",
    "GO GO GO",
    "this chat map again...",
    "pick a map, any map",
    "host?",
    "hellooo?",
    "come on, the map's waiting",
    "any minute now...",
    "host, the button is right there",
    "did the host go make a sandwich?",
    "we're getting old here",
    "I could've cleared Dun Mir by now",
    "I've counted every tile on this map",
    "host, wake up",
    "can we play yet?",
    "pls start",
    "I'm bored",
    "is the host still breathing?",
    "tick tock",
    "I came here to fight, not to chat",
    "start it before I fall asleep",
    "waiting... still waiting...",
    "hosts these days",
    "I've read the whole spellbook twice",
    "my sword is getting rusty",
    "just pick one, host",
    "CapFlag? Arena? Anything?",
    "the ogres have started without us",
    "I've polished my armour three times",
    "host, you there?",
    "press the button, host",
    "it's not that hard, host",
    "come onnnn",
    "we're ready! are you?",
    "my mana's full, let's go",
    "waiting for Hecubah to start it?",
    "so... are we playing?",
    "I'm growing a beard here",
    "hurry, before I lose interest",
    "let's goooo",
    "faster, host",
    "the suspense is killing me",
    "another minute and I'm napping",
    "*yawn*",
    "is this a staring contest?",
    "start! start! start!",
    "even the urchins are bored",
    "host has gone to Galava for supplies",
    "I'll just stand here then",
    "this is the longest chat map ever",
    "my potions are going off",
};

std::span<const std::string_view> linesOf(Banter::Kind kind)
{
    switch (kind) {
    case Banter::Kind::Taunt: return kTaunts;
    case Banter::Kind::Whine: return kWhines;
    case Banter::Kind::Waiting: return kWaits;
    }
    return kWaits;
}

} // namespace

std::size_t Banter::count(Kind kind)
{
    return linesOf(kind).size();
}

std::string_view Banter::raw(Kind kind, std::size_t i)
{
    return linesOf(kind)[i];
}

std::string Banter::line(Kind kind, std::string_view person, std::mt19937& rng)
{
    const auto lines = linesOf(kind);
    std::string_view pick;
    // A random line, rerolled (up to 20 times) while it's one of the last kRecent said.
    for (int tries = 0; tries < 20; ++tries) {
        pick = lines[nox::util::uniform_int_distribution<std::size_t>(0, lines.size() - 1)(rng)];
        if (std::ranges::find(recent_, pick) == recent_.end()) {
            break;
        }
    }
    recent_.push_back(pick);
    if (recent_.size() > kRecent) {
        recent_.pop_front();
    }
    std::string out(pick);
    // Only the first "{name}" is replaced (6: its length).
    if (const auto at = out.find("{name}"); at != std::string::npos) {
        out.replace(at, 6, person);
    }
    return out;
}

} // namespace nox::bot
