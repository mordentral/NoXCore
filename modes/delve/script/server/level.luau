-- Delving Mode's level: what's said and done as a level is played. The director watches (its
-- watchers: zones, lists, keepers, conditions and timers) and calls these as each fires; the
-- run's state is the owner's (told.run: owner.run's table, kept till it changes).
local data = require("data")
local words = require("words")
local told = require("told")
local rules = require("rules")
local draws = require("draws")

local text = data.text
local fill = words.fill

local level = {}

-- How long a gate's sign's line stays over it, in frames.
local SIGN_SHOWN = 150

-- The level's words, worked out on its first frame (level.begun: a map change's work, so a
-- guardian's fall or the level's line only sends them), kept by the run's key: each gate's sign
-- line and where it leads, the level's line. A gate past those worked out, or the run changed since, is worked out as it's asked.
local gates = { key = nil :: number?, line = {} :: { string }, where = {} :: { string }, levelLine = "" }

local function work(count: number)
    local run = told.run()
    gates.key = owner.runKey()
    gates.line, gates.where = {}, {}
    gates.levelLine = told.levelLine(run)
    for i = 0, count - 1 do
        gates.line[i + 1] = told.gateLine(run, i)
        gates.where[i + 1] = told.gateWhere(run, i)
    end
end

local function gateWords(gate: number): (string, string)
    if gates.key ~= owner.runKey() then
        work(#gates.line)
    end
    local line, where = gates.line[gate + 1], gates.where[gate + 1]
    if line == nil then
        local run = told.run()
        return told.gateLine(run, gate), told.gateWhere(run, gate)
    end
    return line, where
end

-- A level's first frame (its `count` gates' spots): its gates' words worked out.
function level.begun(count: number)
    work(math.max(count, 2))
    level.warm() -- (the spells' kinds, read once: a map change's work, not a campfire's)
end

-- A player near gate `gate`'s sign (0 the left; `sign` its id): where the gate leads, over it
-- (the director's zone: at most every so often).
function level.gateSign(gate: number, sign: number)
    local line = gateWords(gate)
    nox.unitSay(sign, line, SIGN_SHOWN)
end

-- A player (by name) through gate `gate` (0 the left), once a level: told to everyone, with where
-- it leads.
function level.gateTaken(gate: number, name: string)
    local _, where = gateWords(gate)
    nox.textToAll(fill(if gate == 0 then text.takesLeft else text.takesRight, { player = name, where = where }))
end

-- The guardian fallen, its `count` gates open: told, and where each leads.
function level.guardianFell(count: number)
    nox.textToAll(text.guardianFalls)
    for i = 0, count - 1 do
        local line = gateWords(i)
        nox.textToAll(line)
    end
end
-- (A whole number's digits, as the C++'s std::to_string.)
local function num(n: number): string
    return string.format("%d", n)
end

-- The level told, two seconds in with someone there to hear it (`t`: the party's units, the elites'
-- lines, the last level's Swift gold, the objective's kind, state, quarry and seconds, the deepest
-- on record and whether it's been told passed this run): its line, its elites, the Swift gold
-- paid, the bonus objective; past the deepest delve on record, that once a run. Whether that was
-- told now.
function level.told(t): boolean
    local run = told.run()
    if gates.key ~= owner.runKey() then
        work(#gates.line)
    end
    nox.textToAll(gates.levelLine)
    for _, line in t.elites do
        nox.textToAll(line)
    end
    if t.swiftGold > 0 then
        for _, unit in t.party do
            nox.setGold(unit, (nox.gold(unit) + t.swiftGold) % 4294967296)
        end
        nox.textToAll(fill(text.swiftDone, { gold = num(t.swiftGold) }))
    end
    if t.objective ~= 0 and t.objectiveState == 0 then
        nox.textToAll(fill(text.bonus, { objective = told.objectiveText(t.objective, t.quarry, t.seconds) }))
    end
    if not t.toldBest and 0 < t.best and t.best < run.depth then
        nox.textToAll(fill(text.newDeepest, { depth = num(run.depth), best = num(t.best) }))
        return true
    end
    return false
end

-- The whole party fallen (each at its quest's game over): told, the run's depth.
function level.fallen()
    nox.textToAll(fill(text.partyFallen, { depth = num(told.run().depth) }))
end

-- An elite's traits (their numbers, data.traits' order).
local SWIFT, VAMPIRIC, REFLECTIVE, TOUGH = 0, 1, 2, 3

-- A thing's type as told: "GiantSpider" -> "Giant Spider" (a space before each capital after a
-- small letter, as the C++'s spacedName).
local function spaced(type: string): string
    local out = {}
    local before = 0
    for i = 1, #type do
        local b = string.byte(type, i)
        if i > 1 and b >= 65 and b <= 90 and before >= 97 and before <= 122 then
            table.insert(out, " ")
        end
        table.insert(out, string.char(b))
        before = b
    end
    return table.concat(out)
end

-- A creature (its id, type and most health) made an elite of `traits`: Swift hasted, Vampiric
-- vampiric, Reflective a reflective shield, each for good at power 3; Tough half again its health
-- (rules.tough); Regenerating the director's beat. Its line ("An elite Wolf: swift and tough!"),
-- told with the level's, and its health if Tough raised it (0: not).
function level.makeElite(unit: number, type: string, maxHealth: number, traits: { number }): (string, number)
    local raised = 0
    local said = {}
    for _, t in traits do
        if t == SWIFT then
            nox.enchant(unit, "ENCHANT_HASTED", 0, 3)
        elseif t == VAMPIRIC then
            nox.enchant(unit, "ENCHANT_VAMPIRISM", 0, 3)
        elseif t == REFLECTIVE then
            nox.enchant(unit, "ENCHANT_REFLECTIVE_SHIELD", 0, 3)
        elseif t == TOUGH then
            maxHealth = rules.tough(maxHealth)
            nox.setHealth(unit, maxHealth, maxHealth)
            nox.touch(unit)
            raised = maxHealth
        end
        table.insert(said, data.traits[t + 1])
    end
    return fill(text.elite, { creature = spaced(type), traits = told.list(said) }), raised
end

-- The heat's bites this beat (the party's units, in its order): each one's lasting
-- invulnerability (the map's start's, till it attacks) taken off, a potion's kept; then the heat's
-- fire.
function level.heat(units: { number })
    for _, unit in units do
        if nox.enchanted(unit, "ENCHANT_INVULNERABLE") and nox.enchantTime(unit, "ENCHANT_INVULNERABLE") == 0 then
            nox.enchant(unit, "ENCHANT_INVULNERABLE", -1)
        end
        nox.damage(unit, nil, data.heat.damage, 0xC, 0)
    end
end

-- A reward's stage as nox.rewardAt takes it (0-1000): from 10 on every stage gives as 10 does
-- (activateRewardMarker's slots, its only use of the stage), so a run past 1000 is given the same.
local function rewardStage(stage: number): number
    return math.min(stage, 1000)
end

-- The bonus objective done (`units`: the party in play; its kind, the Hunt's quarry, Swift's
-- seconds): a find a stage up at each one's feet (a fresh RewardMarker's), and told.
function level.objectiveDone(units: { number }, kind: number, quarry: string, seconds: number)
    local stage = rewardStage(math.max(1, nox.questStage()) + 1)
    for _, unit in units do
        local x, y = nox.position(unit)
        nox.rewardAt(x, y, stage)
    end
    nox.textToAll(fill(text.bonusDone, { objective = told.objectiveText(kind, quarry, seconds) }))
end

-- Purge's count past half (and at least four, some left): told how many are left, once.
function level.purgeHalf(left: number)
    nox.textToAll(fill(text.purgeHalf, { left = num(left) }))
end

-- Swift's time run out: told the bonus is lost.
function level.tooSlow()
    nox.textToAll(text.tooSlow)
end

-- The campfire. Each spell's flags (thing.bin's: -1 none such) and whether it can be raised by
-- the fire (not a quest's once-only one, nor a group's member, raised with the spell heading it:
-- flags 0x2A000), kept as first asked (fixed).
local spells: { flags: { number }, raisable: { boolean } }? = nil
local function spellKinds()
    if not spells then
        local flags, raisable = {}, {}
        for s = 1, 136 do
            local info = nox.spellInfo(s)
            flags[s] = if info then info.flags else -1
            raisable[s] = info ~= nil and not info.onceOnly and bit32.band(info.flags, 0x2A000) == 0
        end
        spells = { flags = flags, raisable = raisable }
    end
    return spells :: { flags: { number }, raisable: { boolean } }
end

function level.warm()
    spellKinds()
end

-- Its spells the fire can raise: known, below 5 (the spells' tables hold five).
local function upgradable(unit: number): { { spell: number, level: number } }
    local raisable = spellKinds().raisable
    local book = nox.spells(unit)
    local list = {}
    for s = 1, 136 do
        local lv = book[s]
        if lv ~= 0 and lv < 5 and raisable[s] then
            table.insert(list, { spell = s, level = lv })
        end
    end
    return list
end

local function sameOffer(a, b): boolean
    if #a ~= #b then
        return false
    end
    for i, x in a do
        if x.spell ~= b[i].spell or x.level ~= b[i].level then
            return false
        end
    end
    return true
end

-- A spell's name as told (the game's title, its letters past ASCII as '?'; none or MISSING: its
-- thing.bin name; no spell: "spell N").
local function spellName(spell: number): string
    local title, id = owner.spellTitle(spell)
    if title == nil then
        return fill(text.spellUnknown, { id = num(spell) })
    end
    return if title == "" or string.sub(title, 1, 7) == "MISSING" then id else title
end

-- The watcher's kinds (WatchEvent::Kind): Enter, Leave, Changed.
local ENTER, LEAVE, CHANGED = 1, 2, 4

-- A player by the fire (its peer and unit, whether it's on NoX Core) as it comes, its spells
-- change, or it leaves: an offer of its spells to raise a level, once (the owner's: the offer
-- kept and sent to a NoX Core client, owner.setCampfireOffer; the fire done for it, owner.heal);
-- a stock client told it in lines, its answer by chat. Walked off: the offer put away.
function level.atCampfire(kind: number, peer: number, unit: number, identified: boolean)
    local offered = owner.campfireOffer(unit)
    if kind == LEAVE then
        if offered then
            owner.setCampfireOffer(unit, peer, nil)
        end
        return
    end
    if owner.healed(unit) then
        return
    end
    local list = upgradable(unit)
    if offered and sameOffer(offered, list) then
        return -- (as it stands; one that changed by the fire, a book read, told again)
    end
    if #list == 0 then
        if offered then
            owner.setCampfireOffer(unit, peer, nil)
        end
        owner.heal(unit) -- (nothing to strengthen: told once)
        nox.tell(unit, text.fireNothing)
        return
    end
    owner.setCampfireOffer(unit, peer, list)
    if identified then
        return
    end
    nox.tell(unit, text.fireOffer)
    local line = ""
    for i, u in list do
        local entry = fill(text.fireItem, { number = num(i), spell = spellName(u.spell), level = num(u.level), next = num(u.level + 1) })
        -- (Lines of at most 90 characters.)
        if line ~= "" and #line + #entry + #text.fireGap > 90 then
            nox.tell(unit, line)
            line = ""
        end
        line = line .. (if line == "" then "" else text.fireGap) .. entry
    end
    if line ~= "" then
        nox.tell(unit, line)
    end
end

-- A player's pick at the fire (its peer, unit and name, nil none; the spell): refused unless it's
-- offered. Up a level, and its group with it (a group's head, flags 0x1000 / 0x4000 / 0x10000,
-- raises those with 0x2000 / 0x8000 / 0x20000), each told to its client; the offer put away, the
-- fire done for it, the celebration (the owner's), and told to everyone.
function level.campfireUpgrade(peer: number, unit: number, name: string?, spell: number): boolean
    local offered = owner.campfireOffer(unit)
    local ok = false
    if offered then
        for _, u in offered do
            if u.spell == spell then
                ok = true
            end
        end
    end
    if not ok then
        return false
    end
    nox.raiseSpell(unit, spell)
    local flags = spellKinds().flags
    local f = flags[spell]
    local group = 0
    if f >= 0 then
        group = if bit32.band(f, 0x1000) ~= 0 then 0x2000 elseif bit32.band(f, 0x4000) ~= 0 then 0x8000 elseif bit32.band(f, 0x10000) ~= 0 then 0x20000 else 0
    end
    if group ~= 0 then
        for s = 1, 136 do
            if flags[s] >= 0 and bit32.band(flags[s], group) ~= 0 then
                nox.raiseSpell(unit, s)
            end
        end
    end
    owner.setCampfireOffer(unit, peer, nil)
    owner.heal(unit)
    owner.celebrate(unit)
    nox.textToAll(fill(text.fireRises, { player = name or text.someone, spell = spellName(spell), level = num(nox.spellLevel(unit, spell)) }))
    return true
end

-- The altar. The blessings that stack (Vigor, Wisdom, Might), and Wisdom's and Might's numbers
-- (data.blessings' order, from 0).
local STACKS = { [0] = true, [1] = true, [2] = true }
local WISDOM, MIGHT = 1, 2

-- A player by the altar (its peer, unit and name, whether it's on NoX Core, whether it casts
-- spells and uses abilities; the run's seed and the level's serial) as it comes or leaves: three
-- of the blessings drawn for it (the same three each time it comes up to it here: draws.altar),
-- of those that stack or it hasn't, Wisdom to a caster, Might to a fighter; offered (the owner's:
-- kept and sent to a NoX Core client), a stock client told it in lines, its answer by chat.
-- Walked off: the offer put away. One blessing an altar (the owner's heal).
function level.atAltar(kind: number, peer: number, unit: number, name: string, identified: boolean, casts: boolean, fights: boolean, run: number, serial: number)
    local offered = owner.hasBlessingOffer(unit)
    if kind == LEAVE then
        if offered then
            owner.setBlessingOffer(unit, peer, nil)
        end
        return
    end
    if offered or owner.healed(unit) then
        return
    end
    local held = owner.blessingsHeld(name)
    local can = {}
    for i = 0, 7 do
        local h = held[i + 1]
        if not ((h ~= 0 and not STACKS[i]) or (i == WISDOM and not casts) or (i == MIGHT and not fights)) then
            table.insert(can, { blessing = i, held = h })
        end
    end
    local drawn = {}
    for _, i in draws.altar(run, serial, name, #can) do
        table.insert(drawn, can[math.clamp(i, 1, #can)])
    end
    if #drawn == 0 then
        return
    end
    owner.setBlessingOffer(unit, peer, drawn)
    if identified then
        return
    end
    nox.tell(unit, text.altarOffer)
    for i, o in drawn do
        local b = data.blessings[o.blessing + 1]
        local line = fill(text.altarItem, { number = num(i), name = b.name, text = b.text })
        if o.held ~= 0 then
            line ..= fill(if o.held == 1 then text.altarHeldOne else text.altarHeldMany, { count = num(o.held) })
        end
        nox.tell(unit, line)
    end
end

-- A player (by name) blessed with blessing `blessing` (from 0): told to everyone.
function level.blessed(name: string, blessing: number)
    local b = data.blessings[blessing + 1]
    nox.textToAll(fill(text.blessed, { player = name, name = b.name, text = b.text }))
end

-- The shrine: a player near, hurt and up, is healed whole, once (the owner's heal), told.
function level.shrine(unit: number, name: string)
    if owner.healed(unit) then
        return
    end
    owner.heal(unit)
    local most = nox.object(unit).maxHealth
    nox.setHealth(unit, most, most)
    local _, mostMana = nox.mana(unit)
    nox.setMana(unit, mostMana, mostMana, "plain")
    nox.touch(unit)
    nox.sound(unit, "RestoreHealth")
    nox.textToAll(fill(text.shrine, { player = name }))
end

-- The blessings kept on their players. Vigor, Wisdom and Might raise health, mana and strength a
-- quarter each held (rules.raise); Renewal brings health and mana back every two seconds
-- (rules.renew); Fortune pays gold at each level's start (rules.fortuneGold). Each player's numbers
-- before and as raised (by name; 0: not yet), kept till the run's over (level.unbless).
local STATS, BEAT, ONCE = 5, 6, 7
local VIGOR = 0 -- (Wisdom's and Might's: the altar's above)
local raisedBy: { [string]: { [string]: number } } = {}

-- One number raised: unchanged (none held, or none to raise), or its base (the class's again
-- when it isn't what was raised) raised; whether it changed, the base, what it's raised to.
local function raise(now: number, base: number, raised: number, count: number, most: number): (boolean, number, number)
    if count <= 0 or now == 0 then
        return false, base, raised
    end
    if now ~= raised then
        base = now
    end
    local target = rules.raise(base, count, most)
    if target == now then
        return false, base, now
    end
    return true, base, target
end

-- A blessed player's keeping (its unit and name): Stats as its most health, most mana or
-- strength isn't what it was (a respawn's, a new map's: raised again, what it gains on top given
-- too; its client told all of it); Beat, Renewal's; Once, Fortune's. (Its numbers now, as the
-- director read them: health and most, mana and most, strength.)
function level.blessingKept(kind: number, unit: number, name: string, health: number, maxHealth: number, mana: number, mostMana: number, strength: number)
    local held = owner.blessingsHeld(name)
    if kind == STATS then
        local r = raisedBy[name]
        if not r then
            r = { healthBase = 0, maxHealth = 0, manaBase = 0, maxMana = 0, strengthBase = 0, strength = 0 }
            raisedBy[name] = r
        end
        local was = maxHealth
        local changed
        changed, r.healthBase, r.maxHealth = raise(was, r.healthBase, r.maxHealth, held[VIGOR + 1], 65535)
        if changed then
            nox.setPlayerHealth(unit, math.min(r.maxHealth, health + math.max(0, r.maxHealth - was)), r.maxHealth)
        end
        changed, r.manaBase, r.maxMana = raise(mostMana, r.manaBase, r.maxMana, held[WISDOM + 1], 65535)
        if changed then
            nox.setMana(unit, math.min(r.maxMana, mana + math.max(0, r.maxMana - mostMana)), r.maxMana)
        end
        changed, r.strengthBase, r.strength = raise(strength, r.strengthBase, r.strength, held[MIGHT + 1], 4294967295)
        if changed then
            nox.setStrength(unit, r.strength)
        end
    elseif kind == BEAT then
        if health < maxHealth then
            nox.setHealth(unit, math.min(maxHealth, health + rules.renew(maxHealth)), maxHealth)
            nox.touch(unit)
        end
        if mana < mostMana then
            nox.setMana(unit, math.min(mostMana, mana + rules.renew(mostMana)), mostMana, "plain")
        end
    elseif kind == ONCE then
        local gold = rules.fortuneGold(told.run().depth)
        nox.setGold(unit, (nox.gold(unit) + gold) % 4294967296)
        nox.tell(unit, fill(text.fortune, { gold = num(gold) }))
    end
end

-- Each blessing's enchantments, kept for good on its holder (by the blessing's number).
local BLESSING_ENCHANTS = {
    [3] = { "ENCHANT_HASTED" },
    [4] = { "ENCHANT_VAMPIRISM" },
    [5] = { "ENCHANT_PROTECT_FROM_FIRE", "ENCHANT_PROTECT_FROM_ELECTRICITY", "ENCHANT_PROTECT_FROM_POISON" },
}

-- The blessings gone (the run over; `players`: each one in with a blessed name, {unit, name}, in
-- the host's order): each number still as raised put back (its client told), the blessings'
-- enchantments for good taken off; every player's raises forgotten.
function level.unbless(players)
    for _, p in players do
        local r = raisedBy[p.name]
        if r then
            local o = nox.object(p.unit)
            if r.maxHealth ~= 0 and o.maxHealth == r.maxHealth then
                nox.setPlayerHealth(p.unit, math.min(o.health, r.healthBase), r.healthBase)
            end
            local mana, mostMana = nox.mana(p.unit)
            if r.maxMana ~= 0 and mostMana == r.maxMana then
                nox.setMana(p.unit, math.min(mana, r.manaBase), r.manaBase)
            end
            if r.strength ~= 0 and nox.strength(p.unit) == r.strength then
                nox.setStrength(p.unit, r.strengthBase)
            end
        end
        local held = owner.blessingsHeld(p.name)
        for i = 0, 7 do
            local enchants = BLESSING_ENCHANTS[i]
            if held[i + 1] ~= 0 and enchants then
                for _, e in enchants do
                    if nox.enchanted(p.unit, e) and nox.enchantTime(p.unit, e) == 0 then
                        nox.enchant(p.unit, e, -1)
                    end
                end
            end
        end
    end
    raisedBy = {}
end

-- A seal touched by one of the party in play (the seal's id and place; the count broken and of
-- how many; who, by name, nil none): switched off (its orb gone), its sound, told.
function level.sealBroken(seal: number, x: number, y: number, done: number, of: number, name: string?)
    nox.setOnOff(seal, false)
    nox.soundAt(x, y, "ForceOfNatureRelease")
    nox.textToAll(fill(text.sealBroken, { player = name or text.someone, done = num(done), of = num(of) }))
end

-- What the barrels broken this pass held (each {holds, x, y}, in the order they broke): low loot (a
-- fresh marker of weapons, armour, gold and potions, two stages down), a small creature hiding in
-- it (the first tier's), a gem; each drawn from the server's own stream (nox.gameRandom) as it
-- comes.
local LOOT, MONSTER, GEM = 1, 2, 3
function level.barrels(broken)
    for _, b in broken do
        if b.holds == LOOT then
            nox.rewardAt(b.x, b.y, rewardStage(math.max(1, nox.questStage() - 2)), 0x08 + 0x10 + 0x20 + 0x40)
        elseif b.holds == MONSTER then
            local tier = data.tiers[1]
            nox.create(tier[nox.gameRandom(#tier) + 1], b.x, b.y)
        elseif b.holds == GEM then
            local gems = data.barrelGems
            nox.create(gems[nox.gameRandom(#gems) + 1], b.x, b.y)
        end
    end
end

-- The spell bars. A player first in play in a run (its unit and name; whether it uses abilities,
-- casts spells, keeps a field guide: a conjurer's or a multiclass player's): each ability, spell and
-- creature it lacks taken off its client's bars (MSG_GAUNTLET 0x12 / 0x11 / 0x13, as the quest's
-- death penalty does: nox.revoke, sent 30 a frame by nox.sendRevokes), so they hold its spells
-- alone.
function level.clearBar(unit: number, name: string, fights: boolean, casts: boolean, guide: boolean)
    if fights then
        for a = 1, 5 do
            if nox.abilityLevel(unit, a) == 0 then
                nox.revoke(name, 0x12, a)
            end
        end
    end
    if casts then
        local book = nox.spells(unit)
        for s = 1, 136 do
            if book[s] == 0 then
                nox.revoke(name, 0x11, s)
            end
        end
        if guide then
            local kept = nox.fieldGuide(unit)
            for g = 1, 40 do
                if not kept[g] then
                    nox.revoke(name, 0x13, g)
                end
            end
        end
    end
end

-- The revokes owed sent (the director's once a frame, while any are owed): how many are still owed.
function level.sendRevokes(): number
    return nox.sendRevokes()
end

-- A fallen party's run over (each one in, {unit, name, first: its class's first spells' numbers},
-- in the host's order; the lobby's map): each one's things gone (deleted, worn and carried), its
-- gold none (told), its game over done with, its spells, abilities and guide none but its class's
-- first (each taken off its client's bars, the first spells granted again at level 1, not
-- revoked: a paced revoke after their award would take them off again); told, and back to the
-- lobby.
function level.wipe(players, lobby: string)
    for _, p in players do
        local unit = p.unit
        for _, item in nox.inventory(unit) do
            pcall(nox.delete, item.id) -- (one gone already: nothing)
        end
        nox.setGold(unit, 0, true)
        nox.setGameOverAt(unit, 0)
        local first = {}
        for _, s in p.first do
            first[s] = true
        end
        local book = nox.spells(unit)
        for s = 1, 136 do
            if book[s] ~= 0 and not first[s] then
                nox.revoke(p.name, 0x11, s)
            end
        end
        for a = 1, 5 do
            if nox.abilityLevel(unit, a) ~= 0 then
                nox.revoke(p.name, 0x12, a)
            end
        end
        local guide = nox.fieldGuide(unit)
        for g = 1, 40 do
            if guide[g] then
                nox.revoke(p.name, 0x13, g)
            end
        end
        nox.clearBook(unit)
        for _, s in p.first do
            nox.grantSpell(unit, s, 1, false, false)
        end
    end
    nox.textToAll(text.wiped)
    nox.exitTo(lobby)
end

return level
