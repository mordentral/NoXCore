// Visual-effect commands: the MSG_FX_* messages (0x7C-0xA3) as typed values.
//
// In 1.2b every effect the world shows goes through these: a spell or trap (server code), a map
// script's Effect("SPARK_EXPLOSION", x, y, ...) builtin, or the server of a listen game all end
// in a netSendFx call that the client then plays. So they're the one currency for effects here
// too: a client decodes them from the network (World::takeEffects), and the future server and
// script VM will produce the same values for local and remote players. Renderers take these,
// never raw bytes.
//
// Effects an object makes on its own (projectile trails, glows, orbs; a thing's CLIENTUPDATE
// and DRAW functions) aren't messages: the client runs them for every drawable of that type.
#pragma once

#include "noxcore/net/messages.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace nox::net {

using FxMessage = std::variant<
    msg::FxParticlefx, msg::FxPlasma, msg::FxSummon, msg::FxSummonCancel, msg::FxShield, msg::FxBlueSparks,
    msg::FxYellowSparks, msg::FxCyanSparks, msg::FxVioletSparks, msg::FxExplosion, msg::FxLesserExplosion,
    msg::FxCounterspellExplosion, msg::FxThinExplosion, msg::FxTeleport, msg::FxSmokeBlast, msg::FxDamagePoof,
    msg::FxLightning, msg::FxEnergyBolt, msg::FxChainLightningBolt, msg::FxDrainMana, msg::FxCharm,
    msg::FxGreaterHeal, msg::FxMagic, msg::FxSparkExplosion, msg::FxDeathRay, msg::FxSentryRay, msg::FxRicochet,
    msg::FxJiggle, msg::FxGreenBolt, msg::FxGreenExplosion, msg::FxWhiteFlash, msg::FxGeneratingMap,
    msg::FxAssemblingMap, msg::FxPopulatingMap, msg::FxDurationSpell, msg::FxDeltazSpellStart, msg::FxTurnUndead,
    msg::FxArrowTrap, msg::FxVampirism, msg::FxManaBombCancel,
    // The quest's own (MSG_GAUNTLET 0x10 / 0x19 / 0x1A, CliNet_OnPacket's 0xF0): the GreenZap, a
    // warp's explosion, a spark burst. Their op is Op::Gauntlet; told apart by their type.
    msg::gauntlet::GreenZapEffect, msg::gauntlet::WarpExplosionFX, msg::gauntlet::PointFX>;

/// Whether `op` is one of the effect messages.
constexpr bool isFxOp(std::uint8_t op)
{
    return op >= static_cast<std::uint8_t>(Op::FxParticlefx) && op <= static_cast<std::uint8_t>(Op::FxManaBombCancel);
}

/// Decodes a whole message (opcode first); nullopt if it isn't an effect (an FX op, or MSG_GAUNTLET
/// 0x10 / 0x19 / 0x1A) or is too short.
std::optional<FxMessage> decodeFx(std::span<const std::uint8_t> message);
/// The message's bytes, opcode first, as the server sends it.
std::vector<std::uint8_t> encodeFx(const FxMessage& fx);
/// The opcode of an effect.
Op fxOp(const FxMessage& fx);

/// An effect's opcode from the name scripts use for it: the message name without "MSG_FX_"
/// ("SPARK_EXPLOSION" for MSG_FX_SPARK_EXPLOSION), case-insensitive. nullopt if there's none.
std::optional<Op> fxOpByName(std::string_view name);

} // namespace nox::net
