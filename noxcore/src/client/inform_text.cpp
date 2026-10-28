// Turns MSG_INFORM (0xA9) and the 0xF0 award/lock reports into what the client shows:
// centred lines, a sound and the message box. The codes are listed in inform_text.hpp.
#include "noxcore/client/inform_text.hpp"

#include <array>

namespace nox::client::inform {

namespace {

// Little-endian u32 at `at`; 0 when the message is too short to hold it.
std::uint32_t u32At(std::span<const std::uint8_t> m, std::size_t at)
{
    if (m.size() < at + 4) {
        return 0;
    }
    return static_cast<std::uint32_t>(m[at] | (m[at + 1] << 8) | (m[at + 2] << 16)) | static_cast<std::uint32_t>(m[at + 3]) << 24;
}

/// The spell results' labels, the unprefixed ones plyrspel.c's.
constexpr std::array<std::string_view, 18> kSpellResults{
    "plyrspel.c:SpellOK",
    "plyrspel.c:SpellInUse",
    "execspel.c:UnseenTarget",
    "plyrspel.c:TooManySpells",
    "summon.c:CreatureControlFailed",
    "glyph.c:TooManyGlyphs",
    "Spell.c:DuplicateGlyphSpell",
    "glyph.c:CantCastGlyph",
    "plyrspel.c:BadTarget",
    "plyrspel.c:BadSkill",
    "plyrspel.c:Illegal",
    "plyrspel.c:NotEnoughManaCast",
    "spell.c:NotEnoughManaGlyph",
    "plyrspel.c:SpellRestrictedByFlag",
    "plyrspel.c:SpellNotStartedWarCry",
    "spell.c:SpellCancelledByWarCry",
    "plyrspel.c:SpellRestrictedByBall",
    "plyrspel.c:SpellRestrictedByCrown",
};

/// The ability results' labels, the unprefixed ones ability.c's.
constexpr std::array<std::string_view, 8> kAbilityResults{
    "ability.c:AbilityOK",
    "ability.c:AbilityInUse",
    "ability.c:AbilityNotReady",
    "ability.c:BadSkill",
    "ability.c:Illegal",
    "ability.c:AbilityRestrictedByFlag",
    "ability.c:AbilityRestrictedWhileJumping",
    "player.c:TooHeavy",
};

/// Team_GetColorTitle's table ({label, title, colour, colour value}, colours 0-9).
constexpr std::array<std::string_view, 10> kColorTitles{
    "advserv.wnd:None",
    "modifier.db:MaterialTeamRedDesc",
    "modifier.db:MaterialTeamBlueDesc",
    "modifier.db:MaterialTeamGreenDesc",
    "modifier.db:MaterialTeamCyanDesc",
    "modifier.db:MaterialTeamYellowDesc",
    "modifier.db:MaterialTeamVioletDesc",
    "modifier.db:MaterialTeamBlackDesc",
    "modifier.db:MaterialTeamWhiteDesc",
    "modifier.db:MaterialTeamOrangeDesc",
};

} // namespace

std::string format(std::string_view fmt, std::initializer_list<std::string_view> values)
{
    std::string out;
    out.reserve(fmt.size());
    auto value = values.begin();
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == '%' && i + 1 < fmt.size()) {
            const char kind = fmt[i + 1];
            if (kind == '%') {
                out += '%';
                ++i;
                continue;
            }
            if (kind == 's' || kind == 'S' || kind == 'd') {
                if (value != values.end()) {
                    out += *value++;
                }
                ++i;
                continue;
            }
        }
        out += fmt[i];
    }
    return out;
}

Notice decodeReport(std::span<const std::uint8_t> m, const Lookups& l)
{
    Notice n;
    if (m.size() < 2 || m[0] != 0xF0 || !l.text) {
        return n;
    }
    const auto text = [&](std::string_view label) { return l.text(label); };
    switch (m[1]) {
    case 0x1E:
    case 0x1F:
    case 0x20: {
        if (m.size() < 5) {
            break;
        }
        // Layout after the two code bytes: u8 id (spell, beast scroll or ability), u16 net code.
        const std::uint32_t id = m[2];
        const std::uint32_t nc = static_cast<std::uint32_t>(m[3] | (m[4] << 8));
        const auto who = l.playerName ? l.playerName(nc) : std::nullopt;
        if (!who) {
            break;
        }
        const bool us = nc == l.localNetCode;
        std::string title;
        std::string_view mine;
        std::string_view other;
        if (m[1] == 0x1E) {
            title = l.spellTitle ? l.spellTitle(id) : std::string();
            mine = "plyrspel.c:AwardSpell";
            other = "plyrspel.c:AwardSpellToOther";
        } else if (m[1] == 0x1F) {
            title = l.guideTitle ? l.guideTitle(id) : std::string();
            mine = "PlyrGide.c:AwardGuide";
            other = "PlyrGide.c:AwardGuideToOther";
        } else {
            title = l.abilityTitle ? l.abilityTitle(id) : std::string();
            mine = "ComAblty.c:AwardAbility";
            other = "ComAblty.c:AwardAbilityToOther";
        }
        n.centred.push_back(us ? format(text(mine), {title}) : format(text(other), {*who, title}));
        break;
    }
    case 0x21: {
        if (m.size() < 0x34) {
            break;
        }
        // Layout: a zero-terminated string label from +2, the key's kind byte at +0x33.
        static constexpr std::array<std::string_view, 5> kKeys{"objcoll.c:NullKey", "objcoll.c:SilverKey", "objcoll.c:GoldKey",
                                                               "objcoll.c:RubyKey", "objcoll.c:SapphireKey"};
        std::string label;
        for (std::size_t i = 2; i < 0x33 && m[i] != 0; ++i) {
            label += static_cast<char>(m[i]);
        }
        const std::uint8_t kind = m[0x33];
        const std::string key = kind < kKeys.size() ? text(kKeys[kind]) : std::string(); // (past the table: the exe's stack)
        n.centred.push_back(format(text(label), {key}));
        break;
    }
    default:
        break;
    }
    return n;
}

std::string colorTitleLabel(std::uint32_t color)
{
    return color < kColorTitles.size() ? std::string(kColorTitles[color]) : std::string("team.c:NoTeam");
}

Notice decode(std::span<const std::uint8_t> m, const Lookups& l)
{
    Notice n;
    if (m.size() < 2 || !l.text) {
        return n;
    }
    const auto text = [&](std::string_view label) { return l.text(label); };
    // Field readers: a player's net code (u32) and a team id or colour (u32) at the
    // offsets each code uses, normally the player at +2 and the team or colour at +6.
    const auto player = [&](std::size_t at) -> std::optional<std::string> {
        return l.playerName ? l.playerName(u32At(m, at)) : std::nullopt;
    };
    const auto team = [&](std::size_t at) -> std::optional<std::string> {
        return l.teamName ? l.teamName(u32At(m, at)) : std::nullopt;
    };
    const auto color = [&](std::size_t at) { return text(colorTitleLabel(u32At(m, at))); };
    const auto say = [&](std::string s) { n.centred.push_back(std::move(s)); };
    // Most codes carry one u32 at +2: a result index, a spell id, a flag or a number.
    const std::uint32_t a = u32At(m, 2);
    switch (m[1]) {
    // Spell, ability and status results index the label tables; out-of-range indexes print nothing.
    case 0:
        if (a < kSpellResults.size()) {
            say(text(kSpellResults[a]));
        }
        break;
    case 1:
        say(format(text("plyrspel.c:SpellCastSuccess"), {l.spellTitle ? l.spellTitle(a) : std::string()}));
        break;
    case 2:
        if (a < kAbilityResults.size()) {
            say(text(kAbilityResults[a]));
        }
        break;
    // Codes naming a player by net code print nothing when the client doesn't know that player.
    case 3:
        if (const auto p = player(2)) {
            say(format(text("netserv.c:PlayerTimeout"), {*p}));
        }
        break;
    case 4:
        if (const auto p = player(2)) {
            say(format(text("objcoll.c:FlagRetrieveNotice"), {*p}));
            n.sound = 0x131;  // FlagRespawn
        }
        break;
    case 5:
        if (const auto p = player(2)) {
            say(format(text("objcoll.c:FlagCaptureNotice"), {*p, color(6)}));
            n.sound = 0x132;  // FlagCapture
        }
        break;
    case 6:
        if (const auto p = player(2)) {
            say(format(text("objcoll.c:FlagPickupNotice"), {*p, color(6)}));
            n.sound = 0x12F;  // FlagPickup
        }
        break;
    case 7:
        if (const auto p = player(2)) {
            say(format(text("drop.c:FlagDropNotice"), {*p, color(6)}));
            n.sound = 0x130;  // FlagDrop
        }
        break;
    case 8:
        say(format(text("update.c:FlagRespawnNotice"), {color(2)}));
        n.sound = 0x131;  // FlagRespawn
        break;
    case 9: {
        const auto t = team(6);
        if (!t) {
            break;
        }
        if (const auto p = player(2)) {
            say(format(text("objcoll.c:FlagBallNotice"), {*p, *t}));
        } else {
            say(format(text("objcoll.c:FlagBallUnknownNotice"), {*t}));
        }
        break;
    }
    case 0x0A:
    case 0x0B: {
        const bool pickup = m[1] == 0x0A;
        const auto p = player(2);
        if (!p) {
            break;
        }
        if (const auto t = team(6)) {
            say(format(text(pickup ? "pickup.c:PickUpTeamCrown" : "drop.c:DropTeamCrown"), {*p, *t}));
        } else {
            say(format(text(pickup ? "pickup.c:PickUpCrown" : "drop.c:DropCrown"), {*p}));
        }
        break;
    }
    case 0x0C:
        say(text("Netserv.c:InObservationMode"));
        // Nonzero: the observer may join now.
        if (a != 0) {
            say(text("Netserv.c:PressJump"));
        }
        break;
    case 0x0D: {
        static constexpr std::array<std::string_view, 4> kWhat{"atckexec.c:PlayerStunned", "atckexec.c:PlayerConfused",
                                                               "atckexec.c:PlayerPoisoned", "player.c:TooHeavy"};
        if (a < kWhat.size()) {
            say(text(kWhat[a]));
        }
        break;
    }
    case 0x0F: {
        if (m.size() < 3 || !l.inGame) {
            break;
        }
        // +2 nonzero: the label is a sign's text (wrapped in use.c:SignSays); the label itself from +3.
        std::string label;
        for (std::size_t i = 3; i < m.size() && m[i] != 0; ++i) {
            label += static_cast<char>(m[i]);
        }
        if (m[2] != 0) {
            say(format(text("use.c:SignSays"), {text(label)}));
        } else {
            say(text(label));
        }
        break;
    }
    case 0x10:
        say(format(text("pickup.c:WrongTeam"), {color(2)}));
        break;
    case 0x11:
        n.dialog = true;
        n.dialogTitle = text("guiserv.c:Notice");
        n.dialogText = text("Noxworld.c:ErrChangedClass");
        break;
    case 0x12:
        if (const auto p = player(2)) {
            say(format(text("objcoll.c:PlayerExited"), {*p}));
        }
        break;
    case 0x13:
        if (const auto p = player(2)) {
            say(format(text("objcoll.c:PlayerExitedWarp"), {*p}));
        }
        break;
    case 0x14:
        if (const auto p = player(2)) {
            say(format(text("GeneralPrint:SecretFoundOther"), {*p}));
        }
        break;
    case 0x15:
        say(format(text("use.c:SignSays"), {format(text("GeneralPrint:AdvanceToStage1"), {std::to_string(static_cast<std::int32_t>(a))})}));
        break;
    default:
        break;
    }
    return n;
}

} // namespace nox::client::inform
