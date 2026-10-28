// Generated from the protocol spec, with messages.hpp.
#include "noxcore/net/messages.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"

#include <algorithm>

namespace nox::net {

namespace {

template <std::size_t N>
void readArray(ByteReader& r, std::array<std::uint8_t, N>& out)
{
    const auto b = r.bytes(N);
    std::copy(b.begin(), b.end(), out.begin());
}

constexpr OpInfo kOps[256] = {
    {"MSG_SERVER_CONNECT", "transport", 0x0, 0x0, 0x1}, // 0x00
    {"MSG_SERVER_ACCEPT", "transport", 0x0, 0x0, 0x6}, // 0x01
    {"MSG_CODE2", "transport", 0x0, 0x0, 0x1}, // 0x02
    {"MSG_CODE3", "transport", 0x0, 0x0, 0x1}, // 0x03
    {"MSG_CODE4", "transport", 0x0, 0x0, 0x1}, // 0x04
    {"MSG_CODE5", "transport", 0x0, 0x0, 0x5}, // 0x05
    {"MSG_CLIENT_PING", "transport", 0x0, 0x0, 0x5}, // 0x06
    {"MSG_CODE7", "transport", 0x0, 0x0, 0x5}, // 0x07
    {"MSG_CLIENT_PONG", "transport", 0x0, 0x0, 0x5}, // 0x08
    {"MSG_CODE9", "transport", 0x0, 0x0, 0x5}, // 0x09
    {"MSG_CLIENT_CLOSE", "transport", 0x0, 0x0, 0x1}, // 0x0A
    {"MSG_SERVER_CLOSE", "transport", 0x0, 0x0, 0x1}, // 0x0B
    {"MSG_SERVER_DISCOVER", "transport", 0x0, 0x0, 0xA}, // 0x0C
    {"MSG_SERVER_INFO", "transport", 0x0, 0x0, 0xFFFF}, // 0x0D
    {"MSG_SERVER_TRY_JOIN", "transport", 0x0, 0x0, 0x62}, // 0x0E
    {"MSG_PASSWORD_REQUIRED", "transport", 0x0, 0x0, 0x1}, // 0x0F
    {"MSG_SERVER_PING", "transport", 0x0, 0x0, 0x6}, // 0x10
    {"MSG_SERVER_PASSWORD", "transport", 0x0, 0x0, 0x14}, // 0x11
    {"MSG_SERVER_PONG", "transport", 0x0, 0x0, 0x6}, // 0x12
    {"MSG_SERVER_ERROR", "transport", 0x0, 0x0, 0x2}, // 0x13
    {"MSG_SERVER_JOIN_OK", "transport", 0x0, 0x0, 0x1}, // 0x14
    {"MSG_SERVER_JOIN_FAIL", "transport", 0x0, 0x0, 0x1}, // 0x15
    {"MSG_CODE22", "transport", 0x0, 0x0, 0x1}, // 0x16
    {"MSG_CODE23", "transport", 0x0, 0x0, 0x1}, // 0x17
    {"MSG_CODE24", "transport", 0x0, 0x0, 0x1}, // 0x18
    {"MSG_CODE25", "transport", 0x0, 0x0, 0x1}, // 0x19
    {"MSG_CODE26", "transport", 0x0, 0x0, 0x1}, // 0x1A
    {"MSG_CODE27", "transport", 0x0, 0x0, 0x1}, // 0x1B
    {"MSG_CODE28", "transport", 0x0, 0x0, 0x1}, // 0x1C
    {"MSG_CODE29", "transport", 0x0, 0x0, 0x1}, // 0x1D
    {"MSG_CODE30", "transport", 0x0, 0x0, 0x1}, // 0x1E
    {"MSG_ACCEPTED", "transport", 0x0, 0x0, 0x2}, // 0x1F
    {"MSG_CLIENT_ACCEPT", "transport", 0x0, 0x0, 0x9A}, // 0x20
    {"MSG_SERVER_CLOSE_ACK", "S->C", 0x1, 0x0, 0x1}, // 0x21
    {"MSG_CLIENT_CLOSE_ACK", "transport", 0x0, 0x0, 0x1}, // 0x22
    {"MSG_SPEED", "transport", 0x0, 0x0, 0x5}, // 0x23
    {"MSG_PING", "transport", 0x0, 0x0, 0x5}, // 0x24
    {"MSG_CODE37", "transport", 0x0, 0x0, 0x1}, // 0x25
    {"MSG_CODE38", "C->S", 0x0, 0x2, 0x2}, // 0x26
    {"MSG_TIMESTAMP", "S->C", 0x3, 0x0, 0x3}, // 0x27
    {"MSG_FULL_TIMESTAMP", "S->C", 0x5, 0x0, 0x5}, // 0x28
    {"MSG_NEED_TIMESTAMP", "C->S", 0x0, 0x1, 0x1}, // 0x29
    {"MSG_SIMULATED_TIMESTAMP", "S->C", 0x5, 0x0, 0x5}, // 0x2A
    {"MSG_USE_MAP", "S->C", 0x29, 0x0, 0x29}, // 0x2B
    {"MSG_JOIN_DATA", "S->C", 0x7, 0x0, 0x7}, // 0x2C
    {"MSG_NEW_PLAYER", "S->C", 0x81, 0x0, 0x81}, // 0x2D
    {"MSG_PLAYER_QUIT", "S->C", 0x3, 0x0, 0x3}, // 0x2E
    {"MSG_SIMPLE_OBJ", "S->C", 0x9, 0x0, 0x9}, // 0x2F
    {"MSG_COMPLEX_OBJ", "S->C", 0xB, 0x0, 0xB}, // 0x30
    {"MSG_DESTROY_OBJECT", "S->C", 0x3, 0x0, 0x3}, // 0x31
    {"MSG_OBJECT_OUT_OF_SIGHT", "S->C", 0x3, 0x0, 0x3}, // 0x32
    {"MSG_OBJECT_IN_SHADOWS", "S->C", 0x3, 0x0, 0x3}, // 0x33
    {"MSG_OBJECT_FRIEND_ADD", "S->C", 0x3, 0x0, 0x3}, // 0x34
    {"MSG_OBJECT_FRIEND_REMOVE", "S->C", 0x3, 0x0, 0x3}, // 0x35
    {"MSG_RESET_FRIENDS", "S->C", 0x1, 0x0, 0x1}, // 0x36
    {"MSG_ENABLE_OBJECT", "S->C", 0x3, 0x0, 0x3}, // 0x37
    {"MSG_DISABLE_OBJECT", "S->C", 0x3, 0x0, 0x3}, // 0x38
    {"MSG_DRAW_FRAME", "S->C", 0x4, 0x0, 0x4}, // 0x39
    {"MSG_DESTROY_WALL", "S->C", 0x3, 0x0, 0x3}, // 0x3A
    {"MSG_OPEN_WALL", "S->C", 0x3, 0x0, 0x3}, // 0x3B
    {"MSG_CLOSE_WALL", "S->C", 0x3, 0x0, 0x3}, // 0x3C
    {"MSG_CHANGE_OR_ADD_WALL_MAGIC", "S->C", 0x6, 0x0, 0x6}, // 0x3D
    {"MSG_REMOVE_WALL_MAGIC", "S->C", 0x3, 0x0, 0x3}, // 0x3E
    {"MSG_PLAYER_INPUT", "C->S", 0x0, 0xFFFF, 0xFFFF}, // 0x3F
    {"MSG_PLAYER_SET_WAYPOINT", "C->S", 0x0, 0x7, 0x7}, // 0x40
    {"MSG_REPORT_HEALTH", "S->C", 0x4, 0x0, 0x4}, // 0x41
    {"MSG_REPORT_HEALTH_DELTA", "S->C", 0x5, 0x0, 0x5}, // 0x42
    {"MSG_REPORT_PLAYER_HEALTH", "S->C", 0x3, 0x0, 0x3}, // 0x43
    {"MSG_REPORT_ITEM_HEALTH", "S->C", 0x7, 0x0, 0x7}, // 0x44
    {"MSG_REPORT_MANA", "S->C", 0x5, 0x0, 0x5}, // 0x45
    {"MSG_REPORT_POISON", "-", 0x0, 0x0, 0x1}, // 0x46
    {"MSG_REPORT_STAMINA", "S->C", 0x2, 0x0, 0x2}, // 0x47
    {"MSG_REPORT_STATS", "S->C", 0xE, 0x0, 0xE}, // 0x48
    {"MSG_REPORT_ARMOR_VALUE", "S->C", 0x5, 0x0, 0x5}, // 0x49
    {"MSG_REPORT_GOLD", "S->C", 0x5, 0x0, 0x5}, // 0x4A
    {"MSG_REPORT_PICKUP", "S->C", 0x5, 0x0, 0x5}, // 0x4B
    {"MSG_REPORT_MODIFIABLE_PICKUP", "S->C", 0x9, 0x0, 0x9}, // 0x4C
    {"MSG_REPORT_DROP", "S->C", 0x5, 0x0, 0x5}, // 0x4D
    {"MSG_REPORT_LESSON", "S->C", 0xB, 0x0, 0xB}, // 0x4E
    {"MSG_REPORT_MUNDANE_ARMOR_EQUIP", "S->C", 0x7, 0x0, 0x7}, // 0x4F
    {"MSG_REPORT_MUNDANE_WEAPON_EQUIP", "S->C", 0x7, 0x0, 0x7}, // 0x50
    {"MSG_REPORT_MODIFIABLE_WEAPON_EQUIP", "S->C", 0xB, 0x0, 0xB}, // 0x51
    {"MSG_REPORT_MODIFIABLE_ARMOR_EQUIP", "S->C", 0xB, 0x0, 0xB}, // 0x52
    {"MSG_REPORT_ARMOR_DEQUIP", "S->C", 0x7, 0x0, 0x7}, // 0x53
    {"MSG_REPORT_WEAPON_DEQUIP", "S->C", 0x7, 0x0, 0x7}, // 0x54
    {"MSG_REPORT_TREASURE_COUNT", "S->C", 0x7, 0x0, 0x7}, // 0x55
    {"MSG_REPORT_FLAG_BALL_WINNER", "S->C", 0x8, 0x0, 0x8}, // 0x56
    {"MSG_REPORT_FLAG_WINNER", "S->C", 0x8, 0x0, 0x8}, // 0x57
    {"MSG_REPORT_DEATHMATCH_WINNER", "S->C", 0x8, 0x0, 0x8}, // 0x58
    {"MSG_REPORT_DEATHMATCH_TEAM_WINNER", "S->C", 0x8, 0x0, 0x8}, // 0x59
    {"MSG_REPORT_ENCHANTMENT", "S->C", 0x7, 0x0, 0x7}, // 0x5A
    {"MSG_REPORT_ITEM_ENCHANTMENT", "S->C", 0x2, 0x0, 0x2}, // 0x5B
    {"MSG_REPORT_LIGHT_COLOR", "S->C", 0x6, 0x0, 0x6}, // 0x5C
    {"MSG_REPORT_LIGHT_INTENSITY", "S->C", 0x7, 0x0, 0x7}, // 0x5D
    {"MSG_REPORT_Z_PLUS", "S->C", 0x4, 0x0, 0x4}, // 0x5E
    {"MSG_REPORT_Z_MINUS", "S->C", 0x4, 0x0, 0x4}, // 0x5F
    {"MSG_REPORT_EQUIP", "S->C", 0x3, 0x0, 0x3}, // 0x60
    {"MSG_REPORT_DEQUIP", "S->C", 0x3, 0x0, 0x3}, // 0x61
    {"MSG_REPORT_ACQUIRE_SPELL", "-", 0x0, 0x0, 0x1}, // 0x62
    {"MSG_REPORT_TARGET", "-", 0x0, 0x0, 0x1}, // 0x63
    {"MSG_REPORT_CHARGES", "S->C", 0x5, 0x0, 0x5}, // 0x64
    {"MSG_REPORT_X_STATUS", "S->C", 0x7, 0x0, 0x7}, // 0x65
    {"MSG_REPORT_PLAYER_STATUS", "S->C", 0x5, 0x0, 0x5}, // 0x66
    {"MSG_REPORT_MODIFIER", "S->C", 0x7, 0x0, 0x7}, // 0x67
    {"MSG_REPORT_STAT_MODIFIER", "S->C", 0x8, 0x0, 0x8}, // 0x68
    {"MSG_REPORT_NPC", "S->C", 0x15, 0x0, 0x15}, // 0x69
    {"MSG_REPORT_CLIENT_STATUS", "S->C", 0x7, 0x0, 0x7}, // 0x6A
    {"MSG_REPORT_ANIMATION_FRAME", "S->C", 0x7, 0x0, 0x7}, // 0x6B
    {"MSG_REPORT_ACQUIRE_CREATURE", "S->C", 0x5, 0x0, 0x5}, // 0x6C
    {"MSG_REPORT_LOSE_CREATURE", "S->C", 0x3, 0x0, 0x3}, // 0x6D
    {"MSG_REPORT_EXPERIENCE", "S->C", 0x5, 0x0, 0x5}, // 0x6E
    {"MSG_REPORT_SPELL_AWARD", "S->C", 0x4, 0x0, 0x4}, // 0x6F
    {"MSG_REPORT_SPELL_START", "S->C", 0x2, 0x0, 0x2}, // 0x70
    {"MSG_REPORT_INVENTORY_LOADED", "S->C", 0x1, 0x0, 0x1}, // 0x71
    {"MSG_TRY_DROP", "C->S", 0x0, 0x7, 0x7}, // 0x72
    {"MSG_TRY_GET", "C->S", 0x0, 0x3, 0x3}, // 0x73
    {"MSG_TRY_USE", "C->S", 0x0, 0x3, 0x3}, // 0x74
    {"MSG_TRY_EQUIP", "C->S", 0x0, 0x3, 0x3}, // 0x75
    {"MSG_TRY_DEQUIP", "C->S", 0x0, 0x3, 0x3}, // 0x76
    {"MSG_TRY_TARGET", "-", 0x0, 0x0, 0x1}, // 0x77
    {"MSG_TRY_CREATURE_COMMAND", "C->S", 0x0, 0x4, 0x4}, // 0x78
    {"MSG_TRY_SPELL", "C->S", 0x0, 0x16, 0x16}, // 0x79
    {"MSG_TRY_ABILITY", "C->S", 0x0, 0x2, 0x2}, // 0x7A
    {"MSG_TRY_COLLIDE", "C->S", 0x0, 0x3, 0x3}, // 0x7B
    {"MSG_FX_PARTICLEFX", "S->C", 0xE, 0x0, 0xE}, // 0x7C
    {"MSG_FX_PLASMA", "S->C", 0x9, 0x0, 0x9}, // 0x7D
    {"MSG_FX_SUMMON", "S->C", 0xC, 0x0, 0xC}, // 0x7E
    {"MSG_FX_SUMMON_CANCEL", "S->C", 0x3, 0x0, 0x3}, // 0x7F
    {"MSG_FX_SHIELD", "S->C", 0x4, 0x0, 0x4}, // 0x80
    {"MSG_FX_BLUE_SPARKS", "S->C", 0x5, 0x0, 0x5}, // 0x81
    {"MSG_FX_YELLOW_SPARKS", "S->C", 0x5, 0x0, 0x5}, // 0x82
    {"MSG_FX_CYAN_SPARKS", "S->C", 0x5, 0x0, 0x5}, // 0x83
    {"MSG_FX_VIOLET_SPARKS", "S->C", 0x5, 0x0, 0x5}, // 0x84
    {"MSG_FX_EXPLOSION", "S->C", 0x5, 0x0, 0x5}, // 0x85
    {"MSG_FX_LESSER_EXPLOSION", "S->C", 0x5, 0x0, 0x5}, // 0x86
    {"MSG_FX_COUNTERSPELL_EXPLOSION", "S->C", 0x5, 0x0, 0x5}, // 0x87
    {"MSG_FX_THIN_EXPLOSION", "S->C", 0x5, 0x0, 0x5}, // 0x88
    {"MSG_FX_TELEPORT", "S->C", 0x5, 0x0, 0x5}, // 0x89
    {"MSG_FX_SMOKE_BLAST", "S->C", 0x5, 0x0, 0x5}, // 0x8A
    {"MSG_FX_DAMAGE_POOF", "S->C", 0x5, 0x0, 0x5}, // 0x8B
    {"MSG_FX_LIGHTNING", "S->C", 0x9, 0x0, 0x9}, // 0x8C
    {"MSG_FX_ENERGY_BOLT", "S->C", 0x9, 0x0, 0x9}, // 0x8D
    {"MSG_FX_CHAIN_LIGHTNING_BOLT", "S->C", 0x9, 0x0, 0x9}, // 0x8E
    {"MSG_FX_DRAIN_MANA", "S->C", 0x9, 0x0, 0x9}, // 0x8F
    {"MSG_FX_CHARM", "S->C", 0x9, 0x0, 0x9}, // 0x90
    {"MSG_FX_GREATER_HEAL", "S->C", 0x9, 0x0, 0x9}, // 0x91
    {"MSG_FX_MAGIC", "-", 0x0, 0x0, 0x1}, // 0x92
    {"MSG_FX_SPARK_EXPLOSION", "S->C", 0x6, 0x0, 0x6}, // 0x93
    {"MSG_FX_DEATH_RAY", "S->C", 0x9, 0x0, 0x9}, // 0x94
    {"MSG_FX_SENTRY_RAY", "S->C", 0x9, 0x0, 0x9}, // 0x95
    {"MSG_FX_RICOCHET", "S->C", 0x5, 0x0, 0x5}, // 0x96
    {"MSG_FX_JIGGLE", "S->C", 0x2, 0x0, 0x2}, // 0x97
    {"MSG_FX_GREEN_BOLT", "S->C", 0xB, 0x0, 0xB}, // 0x98
    {"MSG_FX_GREEN_EXPLOSION", "-", 0x0, 0x0, 0x1}, // 0x99
    {"MSG_FX_WHITE_FLASH", "S->C", 0x5, 0x0, 0x5}, // 0x9A
    {"MSG_FX_GENERATING_MAP", "S->C", 0x3, 0x0, 0x3}, // 0x9B
    {"MSG_FX_ASSEMBLING_MAP", "S->C", 0x3, 0x0, 0x3}, // 0x9C
    {"MSG_FX_POPULATING_MAP", "S->C", 0x3, 0x0, 0x3}, // 0x9D
    {"MSG_FX_DURATION_SPELL", "S->C", 0x7, 0x0, 0x7}, // 0x9E
    {"MSG_FX_DELTAZ_SPELL_START", "S->C", 0x6, 0x0, 0x6}, // 0x9F
    {"MSG_FX_TURN_UNDEAD", "S->C", 0x5, 0x0, 0x5}, // 0xA0
    {"MSG_FX_ARROW_TRAP", "S->C", 0x6, 0x0, 0x6}, // 0xA1
    {"MSG_FX_VAMPIRISM", "S->C", 0xB, 0x0, 0xB}, // 0xA2
    {"MSG_FX_MANA_BOMB_CANCEL", "S->C", 0x5, 0x0, 0x5}, // 0xA3
    {"MSG_UPDATE_STREAM", "S->C", 0xFFFF, 0x0, 0xFFFF}, // 0xA4
    {"MSG_NEW_ALIAS", "C->S", 0x0, 0xA, 0xA}, // 0xA5
    {"MSG_AUDIO_EVENT", "S->C", 0x4, 0x0, 0x4}, // 0xA6
    {"MSG_AUDIO_PLAYER_EVENT", "S->C", 0x4, 0x0, 0x4}, // 0xA7
    {"MSG_TEXT_MESSAGE", "S->C / C->S", 0xFFFF, 0xFFFF, 0xFFFF}, // 0xA8
    {"MSG_INFORM", "S->C", 0xFFFF, 0x0, 0xFFFF}, // 0xA9
    {"MSG_IMPORTANT", "S->C / C->S", 0x1, 0x5, 0x5}, // 0xAA
    {"MSG_IMPORTANT_ACK", "S->C / C->S", 0x5, 0x5, 0x5}, // 0xAB
    {"MSG_MOUSE", "C->S", 0x0, 0x5, 0x5}, // 0xAC
    {"MSG_INCOMING_CLIENT", "C->S", 0x0, 0x1, 0x1}, // 0xAD
    {"MSG_OUTGOING_CLIENT", "S->C / C->S", 0x3, 0x1, 0x3}, // 0xAE
    {"MSG_GAME_SETTINGS", "S->C", 0x14, 0x0, 0x14}, // 0xAF
    {"MSG_GAME_SETTINGS_2", "S->C", 0x31, 0x0, 0x31}, // 0xB0
    {"MSG_UPDATE_GUI_GAME_SETTINGS", "S->C", 0x3C, 0x0, 0x3C}, // 0xB1
    {"MSG_DOOR_ANGLE", "S->C", 0x4, 0x0, 0x4}, // 0xB2
    {"MSG_OBELISK_CHARGE", "S->C", 0x4, 0x0, 0x4}, // 0xB3
    {"MSG_PENTAGRAM_ACTIVATE", "S->C", 0x4, 0x0, 0x4}, // 0xB4
    {"MSG_CLIENT_PREDICT_LINEAR", "S->C", 0xE, 0x0, 0xE}, // 0xB5
    {"MSG_REQUEST_MAP", "C->S", 0x0, 0x1, 0x1}, // 0xB6
    {"MSG_CANCEL_MAP", "C->S", 0x0, 0x1, 0x1}, // 0xB7
    {"MSG_MAP_SEND_START", "S->C", 0x58, 0x0, 0x58}, // 0xB8
    {"MSG_MAP_SEND_PACKET", "S->C", 0xFFFF, 0x0, 0xFFFF}, // 0xB9
    {"MSG_MAP_SEND_ABORT", "S->C", 0x2, 0x0, 0x2}, // 0xBA
    {"MSG_SERVER_CMD", "C->S", 0x0, 0xFFFF, 0xFFFF}, // 0xBB
    {"MSG_SYSOP_PW", "C->S", 0x0, 0x15, 0x15}, // 0xBC
    {"MSG_SYSOP_RESULT", "S->C", 0x2, 0x0, 0x2}, // 0xBD
    {"MSG_KEEP_ALIVE", "C->S", 0x0, 0x1, 0x1}, // 0xBE
    {"MSG_RECEIVED_MAP", "C->S", 0x0, 0x1, 0x1}, // 0xBF
    {"MSG_CLIENT_READY", "C->S", 0x0, 0x1, 0x1}, // 0xC0
    {"MSG_REQUEST_SAVE_PLAYER", "C->S", 0x0, 0x3, 0x3}, // 0xC1
    {"MSG_XFER_MSG", "S->C / C->S", 0xFFFF, 0xFFFF, 0xFFFF}, // 0xC2
    {"MSG_PLAYER_OBJ", "S->C", 0xC, 0x0, 0xC}, // 0xC3
    {"MSG_TEAM_MSG", "S->C / C->S", 0xFFFF, 0xFFFF, 0xFFFF}, // 0xC4
    {"MSG_KICK_NOTIFICATION", "S->C", 0x1, 0x0, 0x1}, // 0xC5
    {"MSG_TIMEOUT_NOTIFICATION", "S->C", 0x1, 0x0, 0x1}, // 0xC6
    {"MSG_SERVER_QUIT", "S->C", 0x1, 0x0, 0x1}, // 0xC7
    {"MSG_SERVER_QUIT_ACK", "C->S", 0x0, 0x1, 0x1}, // 0xC8
    {"MSG_TRADE", "S->C / C->S", 0xFFFF, 0xFFFF, 0xFFFF}, // 0xC9
    {"MSG_CHAT_KILL", "S->C", 0x3, 0x0, 0x3}, // 0xCA
    {"MSG_MESSAGES_KILL", "S->C", 0x1, 0x0, 0x1}, // 0xCB
    {"MSG_SEQ_IMPORTANT", "S->C", 0xFFFF, 0x0, 0xFFFF}, // 0xCC
    {"MSG_REPORT_ABILITY_AWARD", "S->C", 0x3, 0x0, 0x3}, // 0xCD
    {"MSG_REPORT_ABILITY_STATE", "S->C", 0x3, 0x0, 0x3}, // 0xCE
    {"MSG_REPORT_ACTIVE_ABILITIES", "S->C", 0x3, 0x0, 0x3}, // 0xCF
    {"MSG_DIALOG", "S->C / C->S", 0xFFFF, 0xFFFF, 0xFFFF}, // 0xD0
    {"MSG_REPORT_GUIDE_AWARD", "S->C", 0x3, 0x0, 0x3}, // 0xD1
    {"MSG_INTERESTING_ID", "S->C", 0x7, 0x0, 0x7}, // 0xD2
    {"MSG_TIMER_STATUS", "S->C", 0xD, 0x0, 0xD}, // 0xD3
    {"MSG_REQUEST_TIMER_STATUS", "C->S", 0x0, 0x1, 0x1}, // 0xD4
    {"MSG_JOURNAL_MSG", "S->C", 0x44, 0x0, 0x44}, // 0xD5
    {"MSG_CHAPTER_END", "S->C", 0x3, 0x0, 0x3}, // 0xD6
    {"MSG_REPORT_ALL_LATENCY", "S->C", 0x5, 0x0, 0x5}, // 0xD7
    {"MSG_REPORT_FLAG_STATUS", "S->C", 0x6, 0x0, 0x6}, // 0xD8
    {"MSG_REPORT_BALL_STATUS", "S->C", 0x4, 0x0, 0x4}, // 0xD9
    {"MSG_REPORT_OBJECT_POISON", "S->C", 0x4, 0x0, 0x4}, // 0xDA
    {"MSG_REPORT_MONITOR_CREATURE", "S->C", 0x5, 0x0, 0x5}, // 0xDB
    {"MSG_REPORT_UNMONITOR_CREATURE", "S->C", 0x3, 0x0, 0x3}, // 0xDC
    {"MSG_REPORT_TOTAL_HEALTH", "S->C", 0x7, 0x0, 0x7}, // 0xDD
    {"MSG_REPORT_TOTAL_MANA", "S->C", 0x7, 0x0, 0x7}, // 0xDE
    {"MSG_REPORT_SPELL_STAT", "S->C", 0x6, 0x0, 0x6}, // 0xDF
    {"MSG_REPORT_SECONDARY_WEAPON", "S->C / C->S", 0x4, 0x3, 0x4}, // 0xE0
    {"MSG_REPORT_LAST_QUIVER", "S->C", 0x3, 0x0, 0x3}, // 0xE1
    {"MSG_INFO_BOOK_DATA", "S->C / C->S", 0x4, 0x4, 0x4}, // 0xE2
    {"MSG_SOCIAL", "C->S", 0x0, 0x2, 0x2}, // 0xE3
    {"MSG_FADE_BEGIN", "S->C", 0x3, 0x0, 0x3}, // 0xE4
    {"MSG_MUSIC_EVENT", "S->C", 0x3, 0x0, 0x3}, // 0xE5
    {"MSG_MUSIC_PUSH_EVENT", "S->C", 0x3, 0x0, 0x3}, // 0xE6
    {"MSG_MUSIC_POP_EVENT", "S->C", 0x3, 0x0, 0x3}, // 0xE7
    {"MSG_PLAYER_DIED", "S->C", 0x3, 0x0, 0x3}, // 0xE8
    {"MSG_PLAYER_RESPAWN", "S->C", 0x9, 0x0, 0x9}, // 0xE9
    {"MSG_FORGET_DRAWABLES", "S->C", 0x5, 0x0, 0x5}, // 0xEA
    {"MSG_RESET_ABILITIES", "S->C", 0x2, 0x0, 0x2}, // 0xEB
    {"MSG_RATE_CHANGE", "S->C", 0x2, 0x0, 0x2}, // 0xEC
    {"MSG_REPORT_CREATURE_CMD", "S->C", 0x2, 0x0, 0x2}, // 0xED
    {"MSG_VOTE", "S->C / C->S", 0xFFFF, 0xFFFF, 0xFFFF}, // 0xEE
    {"MSG_STAT_MULTIPLIERS", "S->C", 0x11, 0x0, 0x11}, // 0xEF
    {"MSG_GAUNTLET", "S->C / C->S", 0xFFFF, 0xFFFF, 0xFFFF}, // 0xF0
    {"MSG_INVENTORY_FAIL", "C->S", 0x0, 0x3, 0x3}, // 0xF1
    {"MSG_NOXCPP_SUBSTEP", "S->C", 0xFFFF, 0x0, 0xFFFF}, // 0xF2 (noxcpp's own: identified clients only)
    {"MSG_NOXCPP_MAPVOTE", "S->C / C->S", 0xFFFF, 0xFFFF, 0xFFFF}, // 0xF3 (noxcpp's own: identified clients only)
    {"MSG_NOXCPP_MODE_F4", "S->C / C->S", 0xFFFF, 0xFFFF, 0xFFFF}, // 0xF4 (noxcpp's own: identified clients only; a built-in mode's)
    {"MSG_NOXCPP_MODE_F5", "S->C / C->S", 0xFFFF, 0xFFFF, 0xFFFF}, // 0xF5 (noxcpp's own: identified clients only; a built-in mode's)
    {"MSG_NOXCPP_EDIT", "S->C / C->S", 0xFFFF, 0xFFFF, 0xFFFF}, // 0xF6 (noxcpp's own: identified clients only; the map editor's session)
    {"MSG_NOXCPP_COOP", "S->C / C->S", 0xFFFF, 0xFFFF, 0xFFFF}, // 0xF7 (noxcpp's own: identified clients only; a built-in mode's)
    {"MSG_NOXCPP_MODEUI", "S->C / C->S", 0xFFFF, 0xFFFF, 0xFFFF}, // 0xF8 (noxcpp's own: ModeUi clients only; a script mode's UI)
    {"", "-", 0, 0, 0}, // 0xF9
    {"", "-", 0, 0, 0}, // 0xFA
    {"", "-", 0, 0, 0}, // 0xFB
    {"", "-", 0, 0, 0}, // 0xFC
    {"", "-", 0, 0, 0}, // 0xFD
    {"", "-", 0, 0, 0}, // 0xFE
    {"", "-", 0, 0, 0}, // 0xFF
};

struct SubOpEntry {
    std::uint8_t op;
    std::uint8_t sub;
    SubOpInfo info;
};

constexpr SubOpEntry kSubOps[] = {
    {0xA9, 0x00, {"spell error", 0x6, 0x0}},
    {0xA9, 0x01, {"spell OK", 0x6, 0x0}},
    {0xA9, 0x02, {"ability", 0x6, 0x0}},
    {0xA9, 0x03, {"player timed out", 0x6, 0x0}},
    {0xA9, 0x04, {"flag retrieved", 0x6, 0x0}},
    {0xA9, 0x05, {"flag captured", 0xA, 0x0}},
    {0xA9, 0x06, {"flag picked up", 0xA, 0x0}},
    {0xA9, 0x07, {"flag dropped", 0xA, 0x0}},
    {0xA9, 0x08, {"flag respawned", 0x6, 0x0}},
    {0xA9, 0x09, {"ball scored", 0xA, 0x0}},
    {0xA9, 0x0A, {"crown picked up", 0xA, 0x0}},
    {0xA9, 0x0B, {"crown dropped", 0xA, 0x0}},
    {0xA9, 0x0C, {"observer mode", 0x6, 0x0}},
    {0xA9, 0x0D, {"player state", 0x6, 0x0}},
    {0xA9, 0x0E, {"kill feed", 0xB, 0x0}},
    {0xA9, 0x0F, {"string", 0xFFFF, 0x0}},
    {0xA9, 0x10, {"wrong team", 0x6, 0x0}},
    {0xA9, 0x11, {"class changed", 0x2, 0x0}},
    {0xA9, 0x12, {"player exited", 0x6, 0x0}},
    {0xA9, 0x13, {"player warped", 0x6, 0x0}},
    {0xA9, 0x14, {"player found secret", 0x6, 0x0}},
    {0xA9, 0x15, {"warp wait", 0x6, 0x0}},
    {0xC2, 0x00, {"start", 0x8C, 0x8C}},
    {0xC2, 0x01, {"accept", 0x4, 0x4}},
    {0xC2, 0x02, {"data", 0xFFFF, 0xFFFF}},
    {0xC2, 0x03, {"ack", 0x6, 0x6}},
    {0xC2, 0x04, {"done", 0x3, 0x3}},
    {0xC2, 0x05, {"cancel", 0x4, 0x4}},
    {0xC2, 0x06, {"abort", 0x4, 0x4}},
    {0xC4, 0x00, {"create/update team", 0xFFFF, 0x0}},
    {0xC4, 0x01, {"add member", 0xA, 0x0}},
    {0xC4, 0x02, {"remove from team", 0x6, 0x0}},
    {0xC4, 0x03, {"change team", 0xA, 0x0}},
    {0xC4, 0x04, {"rename team", 0x2E, 0x0}},
    {0xC4, 0x05, {"remove all members", 0x6, 0x0}},
    {0xC4, 0x06, {"remove team", 0x6, 0x0}},
    {0xC4, 0x07, {"remove all teams", 0x2, 0x0}},
    {0xC4, 0x08, {"set team score", 0xA, 0x0}},
    {0xC4, 0x09, {"reset all team scores", 0x2, 0x0}},
    {0xC4, 0x0A, {"request join team", 0x0, 0xA}},
    {0xC4, 0x0B, {"switch team", 0x0, 0xA}},
    {0xC4, 0x0C, {"member health", 0x5, 0x0}},
    {0xC9, 0x01, {"close P2P trade window", 0x2, 0x0}},
    {0xC9, 0x02, {"close shop window", 0x2, 0x0}},
    {0xC9, 0x03, {"accept state", 0x3, 0x0}},
    {0xC9, 0x04, {"add item to trade", 0xF, 0x0}},
    {0xC9, 0x05, {"remove item from trade", 0x4, 0x0}},
    {0xC9, 0x06, {"gold values", 0xE, 0x0}},
    {0xC9, 0x07, {"prepare P2P trade", 0x2, 0x0}},
    {0xC9, 0x08, {"shop: add item", 0x12, 0x0}},
    {0xC9, 0x09, {"shop: remove item", 0x4, 0x0}},
    {0xC9, 0x0C, {"start P2P trade", 0x34, 0x0}},
    {0xC9, 0x0D, {"start shop", 0x56, 0x0}},
    {0xC9, 0x0E, {"cancel P2P trade", 0x0, 0x2}},
    {0xC9, 0x0F, {"offer item", 0x0, 0x4}},
    {0xC9, 0x10, {"withdraw item", 0x0, 0x4}},
    {0xC9, 0x11, {"accept trade", 0x0, 0x2}},
    {0xC9, 0x12, {"exit shop", 0x0, 0x2}},
    {0xC9, 0x15, {"open shop", 0x0, 0x4}},
    {0xC9, 0x16, {"buy item", 0x0, 0x4}},
    {0xC9, 0x17, {"buy stack", 0x0, 0x5}},
    {0xC9, 0x18, {"sell item", 0x0, 0x4}},
    {0xC9, 0x19, {"sell stack", 0x0, 0x5}},
    {0xC9, 0x1A, {"repair / recharge item", 0x0, 0x4}},
    {0xC9, 0x1B, {"not enough gold", 0x4, 0x0}},
    {0xC9, 0x1C, {"ask sell price", 0x0, 0x4}},
    {0xC9, 0x1D, {"sell price offer", 0x8, 0x0}},
    {0xC9, 0x1E, {"ask repair cost", 0x0, 0x4}},
    {0xC9, 0x1F, {"repair cost offer", 0x8, 0x0}},
    {0xD0, 0x01, {"start dialog", 0x0, 0x4}},
    {0xD0, 0x02, {"answer", 0x0, 0x3}},
    {0xD0, 0x03, {"open NPC dialog", 0x87, 0x0}},
    {0xD0, 0x04, {"close NPC dialog", 0x2, 0x0}},
    {0xEE, 0x00, {"vote against player", 0x0, 0x34}},
    {0xEE, 0x01, {"vote (kind 1) against player", 0x0, 0x34}},
    {0xEE, 0x02, {"withdraw vote against player", 0x0, 0x34}},
    {0xEE, 0x03, {"withdraw vote (kind 1)", 0x0, 0x34}},
    {0xEE, 0x04, {"vote (kind 2, no target)", 0x0, 0x2}},
    {0xEE, 0x05, {"withdraw vote (kind 2)", 0x0, 0x2}},
    {0xEE, 0x06, {"vote state", 0x3, 0x0}},
    {0xEE, 0x07, {"vote reset", 0x2, 0x0}},
    {0xF0, 0x00, {"quest level start", 0x2, 0x0}},
    {0xF0, 0x01, {"player joined quest", 0x4, 0x0}},
    {0xF0, 0x02, {"game over", 0xE, 0x0}},
    {0xF0, 0x03, {"respawn (observer)", 0x0, 0x2}},
    {0xF0, 0x04, {"set lives", 0x5, 0x0}},
    {0xF0, 0x05, {"(ignored)", 0x4, 0x0}},
    {0xF0, 0x0B, {"(ignored)", 0x10, 0x0}},
    {0xF0, 0x0C, {"level won", 0x5A, 0x0}},
    {0xF0, 0x0D, {"briefing (next level)", 0x45, 0x0}},
    {0xF0, 0x0E, {"quest start briefing", 0x45, 0x0}},
    {0xF0, 0x0F, {"clear marker", 0x4, 0x0}},
    {0xF0, 0x10, {"green zap effect", 0xC, 0x0}},
    {0xF0, 0x11, {"revoke spell", 0x4, 0x0}},
    {0xF0, 0x12, {"disable ability", 0x4, 0x0}},
    {0xF0, 0x13, {"revoke field-guide creature", 0x4, 0x0}},
    {0xF0, 0x14, {"open rank window", 0x2, 0x0}},
    {0xF0, 0x15, {"set player score", 0x8, 0x0}},
    {0xF0, 0x16, {"set player byte 4824", 0x5, 0x0}},
    {0xF0, 0x17, {"set player byte 4825", 0x5, 0x0}},
    {0xF0, 0x18, {"toggle warp GUI", 0x3, 0x0}},
    {0xF0, 0x19, {"warp explosion FX", 0x7, 0x0}},
    {0xF0, 0x1A, {"point FX", 0x6, 0x0}},
    {0xF0, 0x1B, {"host: quest ready", 0x0, 0x2}},
    {0xF0, 0x1C, {"save transfer in progress", 0x3, 0x0}},
    {0xF0, 0x1D, {"highest quest stage", 0x4, 0x0}},
    {0xF0, 0x1E, {"spell awarded text", 0x5, 0x0}},
    {0xF0, 0x1F, {"field-guide awarded text", 0x5, 0x0}},
    {0xF0, 0x20, {"ability awarded text", 0x5, 0x0}},
    {0xF0, 0x21, {"formatted notice", 0x34, 0x0}},
};

} // namespace

const OpInfo& opInfo(Op op) noexcept
{
    return kOps[static_cast<std::uint8_t>(op)];
}

const SubOpInfo* subOpInfo(Op op, std::uint8_t sub) noexcept
{
    for (const auto& e : kSubOps) {
        if (e.op == static_cast<std::uint8_t>(op) && e.sub == sub) {
            return &e.info;
        }
    }
    return nullptr;
}

namespace msg {

ServerConnect ServerConnect::read(ByteReader& /*r*/)
{
    ServerConnect m;
    return m;
}

void ServerConnect::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

ServerAccept ServerAccept::read(ByteReader& r)
{
    ServerAccept m;
    m.playerId = r.u32();
    m.xorKey = r.u8();
    return m;
}

void ServerAccept::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u32(playerId);
    w.u8(xorKey);
}

Code2 Code2::read(ByteReader& /*r*/)
{
    Code2 m;
    return m;
}

void Code2::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

Code3 Code3::read(ByteReader& /*r*/)
{
    Code3 m;
    return m;
}

void Code3::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

Code4 Code4::read(ByteReader& /*r*/)
{
    Code4 m;
    return m;
}

void Code4::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

Code5 Code5::read(ByteReader& r)
{
    Code5 m;
    m.value = r.u32();
    return m;
}

void Code5::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u32(value);
}

ClientPing ClientPing::read(ByteReader& r)
{
    ClientPing m;
    m.timeMs = r.u32();
    return m;
}

void ClientPing::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u32(timeMs);
}

Code7 Code7::read(ByteReader& r)
{
    Code7 m;
    m.value = r.u32();
    return m;
}

void Code7::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u32(value);
}

ClientPong ClientPong::read(ByteReader& r)
{
    ClientPong m;
    m.timeMs = r.u32();
    return m;
}

void ClientPong::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u32(timeMs);
}

Code9 Code9::read(ByteReader& r)
{
    Code9 m;
    m.timeMs = r.u32();
    return m;
}

void Code9::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u32(timeMs);
}

ClientClose ClientClose::read(ByteReader& /*r*/)
{
    ClientClose m;
    return m;
}

void ClientClose::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

ServerClose ServerClose::read(ByteReader& /*r*/)
{
    ServerClose m;
    return m;
}

void ServerClose::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

ServerDiscover ServerDiscover::read(ByteReader& r)
{
    ServerDiscover m;
    readArray(r, m.unk);
    m.token = r.u32();
    return m;
}

void ServerDiscover::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.bytes(unk);
    w.u32(token);
}

ServerInfo ServerInfo::read(ByteReader& r)
{
    ServerInfo m;
    m.players = r.u8();
    m.maxPlayers = r.u8();
    m.settings101lo = r.u8();
    m.settings101hi = r.u8();
    readArray(r, m.gameData44);
    readArray(r, m.mapName);
    m.videoFlags = r.u8();
    m.serverFlags = r.u8();
    m.closed = r.u8();
    readArray(r, m.pad);
    m.settingsVersion = r.u32();
    m.gameFlags = r.u32();
    m.gameData48 = r.u32();
    m.pingMin = r.u16();
    m.pingMax = r.u16();
    m.settings44 = r.u32();
    m.token = r.u32();
    readArray(r, m.allowedSpells);
    m.questStage = r.u16();
    readArray(r, m.pad2);
    { const auto tail = r.bytes(r.remaining()); m.serverName.assign(tail.begin(), tail.end()); }
    return m;
}

void ServerInfo::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(players);
    w.u8(maxPlayers);
    w.u8(settings101lo);
    w.u8(settings101hi);
    w.bytes(gameData44);
    w.bytes(mapName);
    w.u8(videoFlags);
    w.u8(serverFlags);
    w.u8(closed);
    w.bytes(pad);
    w.u32(settingsVersion);
    w.u32(gameFlags);
    w.u32(gameData48);
    w.u16(pingMin);
    w.u16(pingMax);
    w.u32(settings44);
    w.u32(token);
    w.bytes(allowedSpells);
    w.u16(questStage);
    w.bytes(pad2);
    w.bytes(serverName);
}

ServerTryJoin ServerTryJoin::read(ByteReader& r)
{
    ServerTryJoin m;
    m.unk1 = r.u8();
    readArray(r, m.playerName);
    m.playerClass = r.u8();
    m.unk53 = r.u8();
    readArray(r, m.serial);
    m.version = r.u32();
    m.team = r.u32();
    readArray(r, m.account);
    m.noSoloMaps = r.u8();
    m.unk97 = r.u8();
    return m;
}

void ServerTryJoin::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(unk1);
    w.bytes(playerName);
    w.u8(playerClass);
    w.u8(unk53);
    w.bytes(serial);
    w.u32(version);
    w.u32(team);
    w.bytes(account);
    w.u8(noSoloMaps);
    w.u8(unk97);
}

PasswordRequired PasswordRequired::read(ByteReader& /*r*/)
{
    PasswordRequired m;
    return m;
}

void PasswordRequired::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

ServerPing ServerPing::read(ByteReader& r)
{
    ServerPing m;
    m.index = r.u8();
    m.timeMs = r.u32();
    return m;
}

void ServerPing::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(index);
    w.u32(timeMs);
}

ServerPassword ServerPassword::read(ByteReader& r)
{
    ServerPassword m;
    m.unk0 = r.u8();
    readArray(r, m.password);
    return m;
}

void ServerPassword::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(unk0);
    w.bytes(password);
}

ServerPong ServerPong::read(ByteReader& r)
{
    ServerPong m;
    m.index = r.u8();
    m.timeMs = r.u32();
    return m;
}

void ServerPong::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(index);
    w.u32(timeMs);
}

ServerError ServerError::read(ByteReader& r)
{
    ServerError m;
    m.error = r.u8();
    return m;
}

void ServerError::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(error);
}

ServerJoinOk ServerJoinOk::read(ByteReader& /*r*/)
{
    ServerJoinOk m;
    return m;
}

void ServerJoinOk::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

ServerJoinFail ServerJoinFail::read(ByteReader& /*r*/)
{
    ServerJoinFail m;
    return m;
}

void ServerJoinFail::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

Code22 Code22::read(ByteReader& /*r*/)
{
    Code22 m;
    return m;
}

void Code22::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

Code23 Code23::read(ByteReader& /*r*/)
{
    Code23 m;
    return m;
}

void Code23::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

Code24 Code24::read(ByteReader& /*r*/)
{
    Code24 m;
    return m;
}

void Code24::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

Code25 Code25::read(ByteReader& /*r*/)
{
    Code25 m;
    return m;
}

void Code25::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

Code26 Code26::read(ByteReader& /*r*/)
{
    Code26 m;
    return m;
}

void Code26::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

Code27 Code27::read(ByteReader& /*r*/)
{
    Code27 m;
    return m;
}

void Code27::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

Code28 Code28::read(ByteReader& /*r*/)
{
    Code28 m;
    return m;
}

void Code28::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

Code29 Code29::read(ByteReader& /*r*/)
{
    Code29 m;
    return m;
}

void Code29::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

Code30 Code30::read(ByteReader& /*r*/)
{
    Code30 m;
    return m;
}

void Code30::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

Accepted Accepted::read(ByteReader& r)
{
    Accepted m;
    m.seq = r.u8();
    return m;
}

void Accepted::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(seq);
}

ClientAccept ClientAccept::read(ByteReader& r)
{
    ClientAccept m;
    readArray(r, m.playerName);
    m.speed = r.u32();
    m.strength = r.u32();
    m.maxMana = r.u32();
    m.maxHealth = r.u32();
    m.classId = r.u8();
    m.isFemale = r.u8();
    readArray(r, m.colors);
    readArray(r, m.clothing);
    m.unk88 = r.u8();
    readArray(r, m.nameSuffix);
    m.screenWidth = r.u32();
    m.screenHeight = r.u32();
    readArray(r, m.serial);
    m.unk127 = r.u8();
    readArray(r, m.account);
    m.joinToken = r.u32();
    readArray(r, m.account2);
    m.flags = r.u8();
    return m;
}

void ClientAccept::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.bytes(playerName);
    w.u32(speed);
    w.u32(strength);
    w.u32(maxMana);
    w.u32(maxHealth);
    w.u8(classId);
    w.u8(isFemale);
    w.bytes(colors);
    w.bytes(clothing);
    w.u8(unk88);
    w.bytes(nameSuffix);
    w.u32(screenWidth);
    w.u32(screenHeight);
    w.bytes(serial);
    w.u8(unk127);
    w.bytes(account);
    w.u32(joinToken);
    w.bytes(account2);
    w.u8(flags);
}

ServerCloseAck ServerCloseAck::read(ByteReader& /*r*/)
{
    ServerCloseAck m;
    return m;
}

void ServerCloseAck::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

ClientCloseAck ClientCloseAck::read(ByteReader& /*r*/)
{
    ClientCloseAck m;
    return m;
}

void ClientCloseAck::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

Speed Speed::read(ByteReader& r)
{
    Speed m;
    m.speed = r.i32();
    return m;
}

void Speed::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.i32(speed);
}

Ping Ping::read(ByteReader& r)
{
    Ping m;
    m.pingMs = r.u32();
    return m;
}

void Ping::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u32(pingMs);
}

Code37 Code37::read(ByteReader& /*r*/)
{
    Code37 m;
    return m;
}

void Code37::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

Code38 Code38::read(ByteReader& r)
{
    Code38 m;
    m.seq = r.u8();
    return m;
}

void Code38::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(seq);
}

Timestamp Timestamp::read(ByteReader& r)
{
    Timestamp m;
    m.frame16 = r.u16();
    return m;
}

void Timestamp::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(frame16);
}

FullTimestamp FullTimestamp::read(ByteReader& r)
{
    FullTimestamp m;
    m.frame = r.u32();
    return m;
}

void FullTimestamp::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u32(frame);
}

NeedTimestamp NeedTimestamp::read(ByteReader& /*r*/)
{
    NeedTimestamp m;
    return m;
}

void NeedTimestamp::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

SimulatedTimestamp SimulatedTimestamp::read(ByteReader& r)
{
    SimulatedTimestamp m;
    m.frame = r.u32();
    return m;
}

void SimulatedTimestamp::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u32(frame);
}

UseMap UseMap::read(ByteReader& r)
{
    UseMap m;
    readArray(r, m.mapName);
    m.crc = r.u32();
    m.frame = r.u32();
    return m;
}

void UseMap::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.bytes(mapName);
    w.u32(crc);
    w.u32(frame);
}

JoinData JoinData::read(ByteReader& r)
{
    JoinData m;
    m.playerNetCode = r.u16();
    m.joinToken = r.u32();
    return m;
}

void JoinData::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(playerNetCode);
    w.u32(joinToken);
}

NewPlayer NewPlayer::read(ByteReader& r)
{
    NewPlayer m;
    m.netCode = r.u16();
    readArray(r, m.playerInfo);
    m.lessons = r.i16();
    m.field2140 = r.i16();
    m.armorEquipFlags = r.u32();
    m.weaponEquipFlags = r.u32();
    m.statusFlags = r.u32();
    m.field2152 = r.u8();
    m.field2156 = r.u8();
    m.pad = r.u8();
    readArray(r, m.accountName);
    return m;
}

void NewPlayer::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.bytes(playerInfo);
    w.i16(lessons);
    w.i16(field2140);
    w.u32(armorEquipFlags);
    w.u32(weaponEquipFlags);
    w.u32(statusFlags);
    w.u8(field2152);
    w.u8(field2156);
    w.u8(pad);
    w.bytes(accountName);
}

PlayerQuit PlayerQuit::read(ByteReader& r)
{
    PlayerQuit m;
    m.netCode = r.u16();
    return m;
}

void PlayerQuit::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
}

SimpleObj SimpleObj::read(ByteReader& r)
{
    SimpleObj m;
    m.netCode = r.u16();
    m.typeId = r.u16();
    m.x = r.u16();
    m.y = r.u16();
    return m;
}

void SimpleObj::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u16(typeId);
    w.u16(x);
    w.u16(y);
}

ComplexObj ComplexObj::read(ByteReader& r)
{
    ComplexObj m;
    m.netCode = r.u16();
    m.typeId = r.u16();
    m.x = r.u16();
    m.y = r.u16();
    m.animState = r.u8();
    m.frame = r.u8();
    return m;
}

void ComplexObj::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u16(typeId);
    w.u16(x);
    w.u16(y);
    w.u8(animState);
    w.u8(frame);
}

DestroyObject DestroyObject::read(ByteReader& r)
{
    DestroyObject m;
    m.netCode = r.u16();
    return m;
}

void DestroyObject::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
}

ObjectOutOfSight ObjectOutOfSight::read(ByteReader& r)
{
    ObjectOutOfSight m;
    m.netCode = r.u16();
    return m;
}

void ObjectOutOfSight::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
}

ObjectInShadows ObjectInShadows::read(ByteReader& r)
{
    ObjectInShadows m;
    m.netCode = r.u16();
    return m;
}

void ObjectInShadows::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
}

ObjectFriendAdd ObjectFriendAdd::read(ByteReader& r)
{
    ObjectFriendAdd m;
    m.netCode = r.u16();
    return m;
}

void ObjectFriendAdd::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
}

ObjectFriendRemove ObjectFriendRemove::read(ByteReader& r)
{
    ObjectFriendRemove m;
    m.netCode = r.u16();
    return m;
}

void ObjectFriendRemove::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
}

ResetFriends ResetFriends::read(ByteReader& /*r*/)
{
    ResetFriends m;
    return m;
}

void ResetFriends::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

EnableObject EnableObject::read(ByteReader& r)
{
    EnableObject m;
    m.netCode = r.u16();
    return m;
}

void EnableObject::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
}

DisableObject DisableObject::read(ByteReader& r)
{
    DisableObject m;
    m.netCode = r.u16();
    return m;
}

void DisableObject::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
}

DrawFrame DrawFrame::read(ByteReader& r)
{
    DrawFrame m;
    m.netCode = r.u16();
    m.frame = r.u8();
    return m;
}

void DrawFrame::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u8(frame);
}

DestroyWall DestroyWall::read(ByteReader& r)
{
    DestroyWall m;
    m.wallId = r.u16();
    return m;
}

void DestroyWall::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(wallId);
}

OpenWall OpenWall::read(ByteReader& r)
{
    OpenWall m;
    m.wallId = r.u16();
    return m;
}

void OpenWall::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(wallId);
}

CloseWall CloseWall::read(ByteReader& r)
{
    CloseWall m;
    m.wallId = r.u16();
    return m;
}

void CloseWall::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(wallId);
}

ChangeOrAddWallMagic ChangeOrAddWallMagic::read(ByteReader& r)
{
    ChangeOrAddWallMagic m;
    m.tile = r.u8();
    m.dir = r.u8();
    m.variation = r.u8();
    m.gridX = r.u8();
    m.gridY = r.u8();
    return m;
}

void ChangeOrAddWallMagic::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(tile);
    w.u8(dir);
    w.u8(variation);
    w.u8(gridX);
    w.u8(gridY);
}

RemoveWallMagic RemoveWallMagic::read(ByteReader& r)
{
    RemoveWallMagic m;
    m.gridX = r.u8();
    m.gridY = r.u8();
    return m;
}

void RemoveWallMagic::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(gridX);
    w.u8(gridY);
}

PlayerInput PlayerInput::read(ByteReader& r)
{
    PlayerInput m;
    m.size = r.u8();
    { const auto tail = r.bytes(r.remaining()); m.entries.assign(tail.begin(), tail.end()); }
    return m;
}

void PlayerInput::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(size);
    w.bytes(entries);
}

PlayerSetWaypoint PlayerSetWaypoint::read(ByteReader& r)
{
    PlayerSetWaypoint m;
    m.unused = r.u16();
    m.x = r.u16();
    m.y = r.u16();
    return m;
}

void PlayerSetWaypoint::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(unused);
    w.u16(x);
    w.u16(y);
}

ReportHealth ReportHealth::read(ByteReader& r)
{
    ReportHealth m;
    m.netCode = r.u16();
    m.healthHalf = r.u8();
    return m;
}

void ReportHealth::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u8(healthHalf);
}

ReportHealthDelta ReportHealthDelta::read(ByteReader& r)
{
    ReportHealthDelta m;
    m.netCode = r.u16();
    m.delta = r.i16();
    return m;
}

void ReportHealthDelta::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.i16(delta);
}

ReportPlayerHealth ReportPlayerHealth::read(ByteReader& r)
{
    ReportPlayerHealth m;
    m.health = r.i16();
    return m;
}

void ReportPlayerHealth::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.i16(health);
}

ReportItemHealth ReportItemHealth::read(ByteReader& r)
{
    ReportItemHealth m;
    m.netCode = r.u16();
    m.health = r.u16();
    m.maxHealth = r.u16();
    return m;
}

void ReportItemHealth::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u16(health);
    w.u16(maxHealth);
}

ReportMana ReportMana::read(ByteReader& r)
{
    ReportMana m;
    m.netCode = r.u16();
    m.mana = r.u16();
    return m;
}

void ReportMana::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u16(mana);
}

ReportPoison ReportPoison::read(ByteReader& /*r*/)
{
    ReportPoison m;
    return m;
}

void ReportPoison::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

ReportStamina ReportStamina::read(ByteReader& r)
{
    ReportStamina m;
    m.stamina = r.u8();
    return m;
}

void ReportStamina::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(stamina);
}

ReportStats ReportStats::read(ByteReader& r)
{
    ReportStats m;
    m.netCode = r.u16();
    m.maxHealth = r.u16();
    m.maxMana = r.u16();
    m.carryCapacity = r.u16();
    m.speed = r.u16();
    m.strength = r.u16();
    m.level = r.u8();
    return m;
}

void ReportStats::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u16(maxHealth);
    w.u16(maxMana);
    w.u16(carryCapacity);
    w.u16(speed);
    w.u16(strength);
    w.u8(level);
}

ReportArmorValue ReportArmorValue::read(ByteReader& r)
{
    ReportArmorValue m;
    m.armorValue = r.f32();
    return m;
}

void ReportArmorValue::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.f32(armorValue);
}

ReportGold ReportGold::read(ByteReader& r)
{
    ReportGold m;
    m.gold = r.u32();
    return m;
}

void ReportGold::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u32(gold);
}

ReportPickup ReportPickup::read(ByteReader& r)
{
    ReportPickup m;
    m.netCode = r.u16();
    m.typeId = r.u16();
    return m;
}

void ReportPickup::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u16(typeId);
}

ReportModifiablePickup ReportModifiablePickup::read(ByteReader& r)
{
    ReportModifiablePickup m;
    m.netCode = r.u16();
    m.typeId = r.u16();
    m.effectiveness = r.u8();
    m.material = r.u8();
    m.enchant1 = r.u8();
    m.enchant2 = r.u8();
    return m;
}

void ReportModifiablePickup::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u16(typeId);
    w.u8(effectiveness);
    w.u8(material);
    w.u8(enchant1);
    w.u8(enchant2);
}

ReportDrop ReportDrop::read(ByteReader& r)
{
    ReportDrop m;
    m.netCode = r.u16();
    m.typeId = r.u16();
    return m;
}

void ReportDrop::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u16(typeId);
}

ReportLesson ReportLesson::read(ByteReader& r)
{
    ReportLesson m;
    m.netCode = r.u16();
    m.lessons = r.u32();
    m.kills = r.u32();
    return m;
}

void ReportLesson::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u32(lessons);
    w.u32(kills);
}

ReportMundaneArmorEquip ReportMundaneArmorEquip::read(ByteReader& r)
{
    ReportMundaneArmorEquip m;
    m.netCodeFlags = r.u16();
    m.classFlags = r.u32();
    return m;
}

void ReportMundaneArmorEquip::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCodeFlags);
    w.u32(classFlags);
}

ReportMundaneWeaponEquip ReportMundaneWeaponEquip::read(ByteReader& r)
{
    ReportMundaneWeaponEquip m;
    m.netCodeFlags = r.u16();
    m.classFlags = r.u32();
    return m;
}

void ReportMundaneWeaponEquip::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCodeFlags);
    w.u32(classFlags);
}

ReportModifiableWeaponEquip ReportModifiableWeaponEquip::read(ByteReader& r)
{
    ReportModifiableWeaponEquip m;
    m.netCodeFlags = r.u16();
    m.classFlags = r.u32();
    m.effectiveness = r.u8();
    m.material = r.u8();
    m.enchant1 = r.u8();
    m.enchant2 = r.u8();
    return m;
}

void ReportModifiableWeaponEquip::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCodeFlags);
    w.u32(classFlags);
    w.u8(effectiveness);
    w.u8(material);
    w.u8(enchant1);
    w.u8(enchant2);
}

ReportModifiableArmorEquip ReportModifiableArmorEquip::read(ByteReader& r)
{
    ReportModifiableArmorEquip m;
    m.netCodeFlags = r.u16();
    m.classFlags = r.u32();
    m.effectiveness = r.u8();
    m.material = r.u8();
    m.enchant1 = r.u8();
    m.enchant2 = r.u8();
    return m;
}

void ReportModifiableArmorEquip::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCodeFlags);
    w.u32(classFlags);
    w.u8(effectiveness);
    w.u8(material);
    w.u8(enchant1);
    w.u8(enchant2);
}

ReportArmorDequip ReportArmorDequip::read(ByteReader& r)
{
    ReportArmorDequip m;
    m.netCode = r.u16();
    m.classFlags = r.u32();
    return m;
}

void ReportArmorDequip::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u32(classFlags);
}

ReportWeaponDequip ReportWeaponDequip::read(ByteReader& r)
{
    ReportWeaponDequip m;
    m.netCode = r.u16();
    m.classFlags = r.u32();
    return m;
}

void ReportWeaponDequip::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u32(classFlags);
}

ReportTreasureCount ReportTreasureCount::read(ByteReader& r)
{
    ReportTreasureCount m;
    m.netCode = r.u16();
    m.treasures = r.u16();
    m.needed = r.u16();
    return m;
}

void ReportTreasureCount::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u16(treasures);
    w.u16(needed);
}

ReportFlagBallWinner ReportFlagBallWinner::read(ByteReader& r)
{
    ReportFlagBallWinner m;
    m.teamId = r.u16();
    m.byTimeLimit = r.u8();
    m.frame = r.u32();
    return m;
}

void ReportFlagBallWinner::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(teamId);
    w.u8(byTimeLimit);
    w.u32(frame);
}

ReportFlagWinner ReportFlagWinner::read(ByteReader& r)
{
    ReportFlagWinner m;
    m.teamId = r.u16();
    m.byTimeLimit = r.u8();
    m.frame = r.u32();
    return m;
}

void ReportFlagWinner::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(teamId);
    w.u8(byTimeLimit);
    w.u32(frame);
}

ReportDeathmatchWinner ReportDeathmatchWinner::read(ByteReader& r)
{
    ReportDeathmatchWinner m;
    m.netCode = r.u16();
    m.byTimeLimit = r.u8();
    m.frame = r.u32();
    return m;
}

void ReportDeathmatchWinner::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u8(byTimeLimit);
    w.u32(frame);
}

ReportDeathmatchTeamWinner ReportDeathmatchTeamWinner::read(ByteReader& r)
{
    ReportDeathmatchTeamWinner m;
    m.teamId = r.u16();
    m.byTimeLimit = r.u8();
    m.frame = r.u32();
    return m;
}

void ReportDeathmatchTeamWinner::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(teamId);
    w.u8(byTimeLimit);
    w.u32(frame);
}

ReportEnchantment ReportEnchantment::read(ByteReader& r)
{
    ReportEnchantment m;
    m.netCode = r.u16();
    m.buffs = r.u32();
    return m;
}

void ReportEnchantment::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u32(buffs);
}

ReportItemEnchantment ReportItemEnchantment::read(ByteReader& r)
{
    ReportItemEnchantment m;
    m.itemBuffs = r.u8();
    return m;
}

void ReportItemEnchantment::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(itemBuffs);
}

ReportLightColor ReportLightColor::read(ByteReader& r)
{
    ReportLightColor m;
    m.netCode = r.u16();
    m.r = r.u8();
    m.g = r.u8();
    m.b = r.u8();
    return m;
}

void ReportLightColor::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u8(r);
    w.u8(g);
    w.u8(b);
}

ReportLightIntensity ReportLightIntensity::read(ByteReader& r)
{
    ReportLightIntensity m;
    m.netCode = r.u16();
    m.intensity = r.f32();
    return m;
}

void ReportLightIntensity::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.f32(intensity);
}

ReportZPlus ReportZPlus::read(ByteReader& r)
{
    ReportZPlus m;
    m.netCode = r.u16();
    m.z = r.u8();
    return m;
}

void ReportZPlus::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u8(z);
}

ReportZMinus ReportZMinus::read(ByteReader& r)
{
    ReportZMinus m;
    m.netCode = r.u16();
    m.z = r.u8();
    return m;
}

void ReportZMinus::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u8(z);
}

ReportEquip ReportEquip::read(ByteReader& r)
{
    ReportEquip m;
    m.netCode = r.u16();
    return m;
}

void ReportEquip::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
}

ReportDequip ReportDequip::read(ByteReader& r)
{
    ReportDequip m;
    m.netCode = r.u16();
    return m;
}

void ReportDequip::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
}

ReportAcquireSpell ReportAcquireSpell::read(ByteReader& /*r*/)
{
    ReportAcquireSpell m;
    return m;
}

void ReportAcquireSpell::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

ReportTarget ReportTarget::read(ByteReader& /*r*/)
{
    ReportTarget m;
    return m;
}

void ReportTarget::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

ReportCharges ReportCharges::read(ByteReader& r)
{
    ReportCharges m;
    m.netCode = r.u16();
    m.charges = r.u8();
    m.maxCharges = r.u8();
    return m;
}

void ReportCharges::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u8(charges);
    w.u8(maxCharges);
}

ReportXStatus ReportXStatus::read(ByteReader& r)
{
    ReportXStatus m;
    m.netCode = r.u16();
    m.xStatus = r.u32();
    return m;
}

void ReportXStatus::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u32(xStatus);
}

ReportPlayerStatus ReportPlayerStatus::read(ByteReader& r)
{
    ReportPlayerStatus m;
    m.statusFlags = r.u32();
    return m;
}

void ReportPlayerStatus::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u32(statusFlags);
}

ReportModifier ReportModifier::read(ByteReader& r)
{
    ReportModifier m;
    m.netCode = r.u16();
    m.effectiveness = r.u8();
    m.material = r.u8();
    m.enchant1 = r.u8();
    m.enchant2 = r.u8();
    return m;
}

void ReportModifier::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u8(effectiveness);
    w.u8(material);
    w.u8(enchant1);
    w.u8(enchant2);
}

ReportStatModifier ReportStatModifier::read(ByteReader& r)
{
    ReportStatModifier m;
    m.netCode = r.u16();
    m.value = r.f32();
    m.stat = r.u8();
    return m;
}

void ReportStatModifier::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.f32(value);
    w.u8(stat);
}

ReportNpc ReportNpc::read(ByteReader& r)
{
    ReportNpc m;
    m.npcIdFlags = r.u16();
    readArray(r, m.colors);
    return m;
}

void ReportNpc::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(npcIdFlags);
    w.bytes(colors);
}

ReportClientStatus ReportClientStatus::read(ByteReader& r)
{
    ReportClientStatus m;
    m.netCode = r.u16();
    m.status = r.u32();
    return m;
}

void ReportClientStatus::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u32(status);
}

ReportAnimationFrame ReportAnimationFrame::read(ByteReader& r)
{
    ReportAnimationFrame m;
    m.netCode = r.u16();
    m.frame = r.u32();
    return m;
}

void ReportAnimationFrame::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u32(frame);
}

ReportAcquireCreature ReportAcquireCreature::read(ByteReader& r)
{
    ReportAcquireCreature m;
    m.netCode = r.u16();
    m.typeFlags = r.u16();
    return m;
}

void ReportAcquireCreature::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u16(typeFlags);
}

ReportLoseCreature ReportLoseCreature::read(ByteReader& r)
{
    ReportLoseCreature m;
    m.netCodeFlags = r.u16();
    return m;
}

void ReportLoseCreature::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCodeFlags);
}

ReportExperience ReportExperience::read(ByteReader& r)
{
    ReportExperience m;
    m.experience = r.u32();
    return m;
}

void ReportExperience::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u32(experience);
}

ReportSpellAward ReportSpellAward::read(ByteReader& r)
{
    ReportSpellAward m;
    m.spellId = r.u8();
    m.level = r.u8();
    m.flags = r.u8();
    return m;
}

void ReportSpellAward::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(spellId);
    w.u8(level);
    w.u8(flags);
}

ReportSpellStart ReportSpellStart::read(ByteReader& r)
{
    ReportSpellStart m;
    m.spellId = r.u8();
    return m;
}

void ReportSpellStart::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(spellId);
}

ReportInventoryLoaded ReportInventoryLoaded::read(ByteReader& /*r*/)
{
    ReportInventoryLoaded m;
    return m;
}

void ReportInventoryLoaded::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

TryDrop TryDrop::read(ByteReader& r)
{
    TryDrop m;
    m.netCode = r.u16();
    m.x = r.u16();
    m.y = r.u16();
    return m;
}

void TryDrop::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u16(x);
    w.u16(y);
}

TryGet TryGet::read(ByteReader& r)
{
    TryGet m;
    m.netCode = r.u16();
    return m;
}

void TryGet::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
}

TryUse TryUse::read(ByteReader& r)
{
    TryUse m;
    m.netCode = r.u16();
    return m;
}

void TryUse::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
}

TryEquip TryEquip::read(ByteReader& r)
{
    TryEquip m;
    m.netCode = r.u16();
    return m;
}

void TryEquip::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
}

TryDequip TryDequip::read(ByteReader& r)
{
    TryDequip m;
    m.netCode = r.u16();
    return m;
}

void TryDequip::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
}

TryTarget TryTarget::read(ByteReader& /*r*/)
{
    TryTarget m;
    return m;
}

void TryTarget::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

TryCreatureCommand TryCreatureCommand::read(ByteReader& r)
{
    TryCreatureCommand m;
    m.netCode = r.u16();
    m.command = r.u8();
    return m;
}

void TryCreatureCommand::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u8(command);
}

TrySpell TrySpell::read(ByteReader& r)
{
    TrySpell m;
    m.spell1 = r.u32();
    m.spell2 = r.u32();
    m.spell3 = r.u32();
    m.spell4 = r.u32();
    m.spell5 = r.u32();
    m.targetFlag = r.u8();
    return m;
}

void TrySpell::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u32(spell1);
    w.u32(spell2);
    w.u32(spell3);
    w.u32(spell4);
    w.u32(spell5);
    w.u8(targetFlag);
}

TryAbility TryAbility::read(ByteReader& r)
{
    TryAbility m;
    m.ability = r.u8();
    return m;
}

void TryAbility::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(ability);
}

TryCollide TryCollide::read(ByteReader& r)
{
    TryCollide m;
    m.netCode = r.u16();
    return m;
}

void TryCollide::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
}

FxParticlefx FxParticlefx::read(ByteReader& r)
{
    FxParticlefx m;
    m.fxCode = r.u8();
    m.count = r.u16();
    m.flag = r.u16();
    m.max = r.u16();
    m.netCode = r.u16();
    readArray(r, m.unused);
    return m;
}

void FxParticlefx::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(fxCode);
    w.u16(count);
    w.u16(flag);
    w.u16(max);
    w.u16(netCode);
    w.bytes(unused);
}

FxPlasma FxPlasma::read(ByteReader& r)
{
    FxPlasma m;
    m.x1 = r.u16();
    m.y1 = r.u16();
    m.x2 = r.u16();
    m.y2 = r.u16();
    return m;
}

void FxPlasma::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(x1);
    w.u16(y1);
    w.u16(x2);
    w.u16(y2);
}

FxSummon FxSummon::read(ByteReader& r)
{
    FxSummon m;
    m.x = r.u16();
    m.y = r.u16();
    m.summonId = r.u16();
    m.typeId = r.u16();
    m.dir = r.u8();
    m.duration = r.u16();
    return m;
}

void FxSummon::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(x);
    w.u16(y);
    w.u16(summonId);
    w.u16(typeId);
    w.u8(dir);
    w.u16(duration);
}

FxSummonCancel FxSummonCancel::read(ByteReader& r)
{
    FxSummonCancel m;
    m.summonId = r.u16();
    return m;
}

void FxSummonCancel::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(summonId);
}

FxShield FxShield::read(ByteReader& r)
{
    FxShield m;
    m.netCode = r.u16();
    m.shieldType = r.u8();
    return m;
}

void FxShield::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u8(shieldType);
}

FxBlueSparks FxBlueSparks::read(ByteReader& r)
{
    FxBlueSparks m;
    m.x = r.i16();
    m.y = r.i16();
    return m;
}

void FxBlueSparks::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.i16(x);
    w.i16(y);
}

FxYellowSparks FxYellowSparks::read(ByteReader& r)
{
    FxYellowSparks m;
    m.x = r.i16();
    m.y = r.i16();
    return m;
}

void FxYellowSparks::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.i16(x);
    w.i16(y);
}

FxCyanSparks FxCyanSparks::read(ByteReader& r)
{
    FxCyanSparks m;
    m.x = r.i16();
    m.y = r.i16();
    return m;
}

void FxCyanSparks::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.i16(x);
    w.i16(y);
}

FxVioletSparks FxVioletSparks::read(ByteReader& r)
{
    FxVioletSparks m;
    m.x = r.i16();
    m.y = r.i16();
    return m;
}

void FxVioletSparks::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.i16(x);
    w.i16(y);
}

FxExplosion FxExplosion::read(ByteReader& r)
{
    FxExplosion m;
    m.x = r.i16();
    m.y = r.i16();
    return m;
}

void FxExplosion::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.i16(x);
    w.i16(y);
}

FxLesserExplosion FxLesserExplosion::read(ByteReader& r)
{
    FxLesserExplosion m;
    m.x = r.i16();
    m.y = r.i16();
    return m;
}

void FxLesserExplosion::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.i16(x);
    w.i16(y);
}

FxCounterspellExplosion FxCounterspellExplosion::read(ByteReader& r)
{
    FxCounterspellExplosion m;
    m.x = r.i16();
    m.y = r.i16();
    return m;
}

void FxCounterspellExplosion::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.i16(x);
    w.i16(y);
}

FxThinExplosion FxThinExplosion::read(ByteReader& r)
{
    FxThinExplosion m;
    m.x = r.i16();
    m.y = r.i16();
    return m;
}

void FxThinExplosion::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.i16(x);
    w.i16(y);
}

FxTeleport FxTeleport::read(ByteReader& r)
{
    FxTeleport m;
    m.x = r.i16();
    m.y = r.i16();
    return m;
}

void FxTeleport::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.i16(x);
    w.i16(y);
}

FxSmokeBlast FxSmokeBlast::read(ByteReader& r)
{
    FxSmokeBlast m;
    m.x = r.i16();
    m.y = r.i16();
    return m;
}

void FxSmokeBlast::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.i16(x);
    w.i16(y);
}

FxDamagePoof FxDamagePoof::read(ByteReader& r)
{
    FxDamagePoof m;
    m.x = r.i16();
    m.y = r.i16();
    return m;
}

void FxDamagePoof::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.i16(x);
    w.i16(y);
}

FxLightning FxLightning::read(ByteReader& r)
{
    FxLightning m;
    m.x1 = r.u16();
    m.y1 = r.u16();
    m.x2 = r.u16();
    m.y2 = r.u16();
    return m;
}

void FxLightning::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(x1);
    w.u16(y1);
    w.u16(x2);
    w.u16(y2);
}

FxEnergyBolt FxEnergyBolt::read(ByteReader& r)
{
    FxEnergyBolt m;
    m.x1 = r.u16();
    m.y1 = r.u16();
    m.x2 = r.u16();
    m.y2 = r.u16();
    return m;
}

void FxEnergyBolt::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(x1);
    w.u16(y1);
    w.u16(x2);
    w.u16(y2);
}

FxChainLightningBolt FxChainLightningBolt::read(ByteReader& r)
{
    FxChainLightningBolt m;
    m.x1 = r.u16();
    m.y1 = r.u16();
    m.x2 = r.u16();
    m.y2 = r.u16();
    return m;
}

void FxChainLightningBolt::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(x1);
    w.u16(y1);
    w.u16(x2);
    w.u16(y2);
}

FxDrainMana FxDrainMana::read(ByteReader& r)
{
    FxDrainMana m;
    m.x1 = r.u16();
    m.y1 = r.u16();
    m.x2 = r.u16();
    m.y2 = r.u16();
    return m;
}

void FxDrainMana::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(x1);
    w.u16(y1);
    w.u16(x2);
    w.u16(y2);
}

FxCharm FxCharm::read(ByteReader& r)
{
    FxCharm m;
    m.x1 = r.u16();
    m.y1 = r.u16();
    m.x2 = r.u16();
    m.y2 = r.u16();
    return m;
}

void FxCharm::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(x1);
    w.u16(y1);
    w.u16(x2);
    w.u16(y2);
}

FxGreaterHeal FxGreaterHeal::read(ByteReader& r)
{
    FxGreaterHeal m;
    m.x1 = r.u16();
    m.y1 = r.u16();
    m.x2 = r.u16();
    m.y2 = r.u16();
    return m;
}

void FxGreaterHeal::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(x1);
    w.u16(y1);
    w.u16(x2);
    w.u16(y2);
}

FxMagic FxMagic::read(ByteReader& /*r*/)
{
    FxMagic m;
    return m;
}

void FxMagic::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

FxSparkExplosion FxSparkExplosion::read(ByteReader& r)
{
    FxSparkExplosion m;
    m.x = r.i16();
    m.y = r.i16();
    m.size = r.u8();
    return m;
}

void FxSparkExplosion::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.i16(x);
    w.i16(y);
    w.u8(size);
}

FxDeathRay FxDeathRay::read(ByteReader& r)
{
    FxDeathRay m;
    m.x1 = r.u16();
    m.y1 = r.u16();
    m.x2 = r.u16();
    m.y2 = r.u16();
    return m;
}

void FxDeathRay::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(x1);
    w.u16(y1);
    w.u16(x2);
    w.u16(y2);
}

FxSentryRay FxSentryRay::read(ByteReader& r)
{
    FxSentryRay m;
    m.x1 = r.u16();
    m.y1 = r.u16();
    m.x2 = r.u16();
    m.y2 = r.u16();
    return m;
}

void FxSentryRay::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(x1);
    w.u16(y1);
    w.u16(x2);
    w.u16(y2);
}

FxRicochet FxRicochet::read(ByteReader& r)
{
    FxRicochet m;
    m.x = r.i16();
    m.y = r.i16();
    return m;
}

void FxRicochet::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.i16(x);
    w.i16(y);
}

FxJiggle FxJiggle::read(ByteReader& r)
{
    FxJiggle m;
    m.amount = r.u8();
    return m;
}

void FxJiggle::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(amount);
}

FxGreenBolt FxGreenBolt::read(ByteReader& r)
{
    FxGreenBolt m;
    m.x1 = r.u16();
    m.y1 = r.u16();
    m.x2 = r.u16();
    m.y2 = r.u16();
    m.duration = r.u16();
    return m;
}

void FxGreenBolt::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(x1);
    w.u16(y1);
    w.u16(x2);
    w.u16(y2);
    w.u16(duration);
}

FxGreenExplosion FxGreenExplosion::read(ByteReader& /*r*/)
{
    FxGreenExplosion m;
    return m;
}

void FxGreenExplosion::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

FxWhiteFlash FxWhiteFlash::read(ByteReader& r)
{
    FxWhiteFlash m;
    readArray(r, m.unused);
    return m;
}

void FxWhiteFlash::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.bytes(unused);
}

FxGeneratingMap FxGeneratingMap::read(ByteReader& r)
{
    FxGeneratingMap m;
    m.progress = r.u16();
    return m;
}

void FxGeneratingMap::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(progress);
}

FxAssemblingMap FxAssemblingMap::read(ByteReader& r)
{
    FxAssemblingMap m;
    m.progress = r.u16();
    return m;
}

void FxAssemblingMap::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(progress);
}

FxPopulatingMap FxPopulatingMap::read(ByteReader& r)
{
    FxPopulatingMap m;
    m.progress = r.u16();
    return m;
}

void FxPopulatingMap::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(progress);
}

FxDurationSpell FxDurationSpell::read(ByteReader& r)
{
    FxDurationSpell m;
    m.kind = r.u8();
    m.level = r.u8();
    m.netCode1 = r.u16();
    m.netCode2 = r.u16();
    return m;
}

void FxDurationSpell::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kind);
    w.u8(level);
    w.u16(netCode1);
    w.u16(netCode2);
}

FxDeltazSpellStart FxDeltazSpellStart::read(ByteReader& r)
{
    FxDeltazSpellStart m;
    m.netCode = r.u16();
    m.startZ = r.u8();
    m.deltaZ = r.i8();
    m.speed = r.u8();
    return m;
}

void FxDeltazSpellStart::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u8(startZ);
    w.i8(deltaZ);
    w.u8(speed);
}

FxTurnUndead FxTurnUndead::read(ByteReader& r)
{
    FxTurnUndead m;
    m.x = r.i16();
    m.y = r.i16();
    return m;
}

void FxTurnUndead::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.i16(x);
    w.i16(y);
}

FxArrowTrap FxArrowTrap::read(ByteReader& r)
{
    FxArrowTrap m;
    m.x = r.i16();
    m.y = r.i16();
    m.variant = r.u8();
    return m;
}

void FxArrowTrap::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.i16(x);
    w.i16(y);
    w.u8(variant);
}

FxVampirism FxVampirism::read(ByteReader& r)
{
    FxVampirism m;
    m.x1 = r.u16();
    m.y1 = r.u16();
    m.x2 = r.u16();
    m.y2 = r.u16();
    m.amount = r.u16();
    return m;
}

void FxVampirism::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(x1);
    w.u16(y1);
    w.u16(x2);
    w.u16(y2);
    w.u16(amount);
}

FxManaBombCancel FxManaBombCancel::read(ByteReader& r)
{
    FxManaBombCancel m;
    m.x = r.i16();
    m.y = r.i16();
    return m;
}

void FxManaBombCancel::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.i16(x);
    w.i16(y);
}

UpdateStream UpdateStream::read(ByteReader& r)
{
    UpdateStream m;
    { const auto tail = r.bytes(r.remaining()); m.stream.assign(tail.begin(), tail.end()); }
    return m;
}

void UpdateStream::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.bytes(stream);
}

NewAlias NewAlias::read(ByteReader& r)
{
    NewAlias m;
    m.alias = r.u8();
    m.netCode = r.u16();
    m.typeId = r.u16();
    m.expireFrame = r.u32();
    return m;
}

void NewAlias::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(alias);
    w.u16(netCode);
    w.u16(typeId);
    w.u32(expireFrame);
}

AudioEvent AudioEvent::read(ByteReader& r)
{
    AudioEvent m;
    m.pan = r.i8();
    m.soundBits = r.u16();
    return m;
}

void AudioEvent::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.i8(pan);
    w.u16(soundBits);
}

AudioPlayerEvent AudioPlayerEvent::read(ByteReader& r)
{
    AudioPlayerEvent m;
    m.pan = r.i8();
    m.soundBits = r.u16();
    return m;
}

void AudioPlayerEvent::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.i8(pan);
    w.u16(soundBits);
}

TextMessage TextMessage::read(ByteReader& r)
{
    TextMessage m;
    m.netCode = r.u16();
    m.flags = r.u8();
    m.x = r.u16();
    m.y = r.u16();
    m.size = r.u8();
    m.duration = r.u16();
    { const auto tail = r.bytes(r.remaining()); m.text.assign(tail.begin(), tail.end()); }
    return m;
}

void TextMessage::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u8(flags);
    w.u16(x);
    w.u16(y);
    w.u8(size);
    w.u16(duration);
    w.bytes(text);
}

Important Important::read(ByteReader& r)
{
    Important m;
    if (!r.atEnd()) m.id = r.u32();
    return m;
}

void Important::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u32(id);
}

ImportantAck ImportantAck::read(ByteReader& r)
{
    ImportantAck m;
    m.id = r.u32();
    return m;
}

void ImportantAck::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u32(id);
}

Mouse Mouse::read(ByteReader& r)
{
    Mouse m;
    m.x = r.u16();
    m.y = r.u16();
    return m;
}

void Mouse::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(x);
    w.u16(y);
}

IncomingClient IncomingClient::read(ByteReader& /*r*/)
{
    IncomingClient m;
    return m;
}

void IncomingClient::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

OutgoingClient OutgoingClient::read(ByteReader& r)
{
    OutgoingClient m;
    if (!r.atEnd()) m.netCode = r.u16();
    return m;
}

void OutgoingClient::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
}

GameSettings GameSettings::read(ByteReader& r)
{
    GameSettings m;
    m.settingsSerial = r.u32();
    m.version = r.u32();
    m.gameFlags = r.u32();
    m.serverSubFlags = r.u32();
    m.playerLimit = r.u8();
    m.fragLimit = r.u8();
    m.timeLimit = r.u8();
    return m;
}

void GameSettings::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u32(settingsSerial);
    w.u32(version);
    w.u32(gameFlags);
    w.u32(serverSubFlags);
    w.u8(playerLimit);
    w.u8(fragLimit);
    w.u8(timeLimit);
}

GameSettings2 GameSettings2::read(ByteReader& r)
{
    GameSettings2 m;
    readArray(r, m.gameName);
    readArray(r, m.allowedSpells);
    m.gameData44 = r.u32();
    m.gameData48 = r.u32();
    m.timeRemainingMs = r.u32();
    return m;
}

void GameSettings2::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.bytes(gameName);
    w.bytes(allowedSpells);
    w.u32(gameData44);
    w.u32(gameData48);
    w.u32(timeRemainingMs);
}

UpdateGuiGameSettings UpdateGuiGameSettings::read(ByteReader& r)
{
    UpdateGuiGameSettings m;
    m.slot = r.u8();
    readArray(r, m.gameData);
    return m;
}

void UpdateGuiGameSettings::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(slot);
    w.bytes(gameData);
}

DoorAngle DoorAngle::read(ByteReader& r)
{
    DoorAngle m;
    m.netCode = r.u16();
    m.angle = r.u8();
    return m;
}

void DoorAngle::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u8(angle);
}

ObeliskCharge ObeliskCharge::read(ByteReader& r)
{
    ObeliskCharge m;
    m.netCode = r.u16();
    m.charge = r.u8();
    return m;
}

void ObeliskCharge::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u8(charge);
}

PentagramActivate PentagramActivate::read(ByteReader& r)
{
    PentagramActivate m;
    m.netCode = r.u16();
    m.state = r.u8();
    return m;
}

void PentagramActivate::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u8(state);
}

ClientPredictLinear ClientPredictLinear::read(ByteReader& r)
{
    ClientPredictLinear m;
    m.netCode = r.u16();
    m.typeId = r.u16();
    m.x = r.u16();
    m.y = r.u16();
    m.direction = r.u16();
    m.vz = r.i8();
    m.vx = r.i8();
    m.vy = r.i8();
    return m;
}

void ClientPredictLinear::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u16(typeId);
    w.u16(x);
    w.u16(y);
    w.u16(direction);
    w.i8(vz);
    w.i8(vx);
    w.i8(vy);
}

RequestMap RequestMap::read(ByteReader& /*r*/)
{
    RequestMap m;
    return m;
}

void RequestMap::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

CancelMap CancelMap::read(ByteReader& /*r*/)
{
    CancelMap m;
    return m;
}

void CancelMap::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

MapSendStart MapSendStart::read(ByteReader& r)
{
    MapSendStart m;
    readArray(r, m.unk);
    m.mapSize = r.u32();
    readArray(r, m.mapName);
    return m;
}

void MapSendStart::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.bytes(unk);
    w.u32(mapSize);
    w.bytes(mapName);
}

MapSendPacket MapSendPacket::read(ByteReader& r)
{
    MapSendPacket m;
    m.unk = r.u8();
    m.block = r.u16();
    m.size = r.u16();
    { const auto tail = r.bytes(r.remaining()); m.data.assign(tail.begin(), tail.end()); }
    return m;
}

void MapSendPacket::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(unk);
    w.u16(block);
    w.u16(size);
    w.bytes(data);
}

MapSendAbort MapSendAbort::read(ByteReader& r)
{
    MapSendAbort m;
    m.reason = r.u8();
    return m;
}

void MapSendAbort::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(reason);
}

ServerCmd ServerCmd::read(ByteReader& r)
{
    ServerCmd m;
    m.cmdId = r.u8();
    m.netCode = r.u16();
    m.chars = r.u8();
    { const auto tail = r.bytes(r.remaining()); m.cmd.assign(tail.begin(), tail.end()); }
    return m;
}

void ServerCmd::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(cmdId);
    w.u16(netCode);
    w.u8(chars);
    w.bytes(cmd);
}

SysopPw SysopPw::read(ByteReader& r)
{
    SysopPw m;
    readArray(r, m.password);
    m.zero = r.u16();
    m.netCode = r.u16();
    return m;
}

void SysopPw::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.bytes(password);
    w.u16(zero);
    w.u16(netCode);
}

SysopResult SysopResult::read(ByteReader& r)
{
    SysopResult m;
    m.granted = r.u8();
    return m;
}

void SysopResult::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(granted);
}

KeepAlive KeepAlive::read(ByteReader& /*r*/)
{
    KeepAlive m;
    return m;
}

void KeepAlive::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

ReceivedMap ReceivedMap::read(ByteReader& /*r*/)
{
    ReceivedMap m;
    return m;
}

void ReceivedMap::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

ClientReady ClientReady::read(ByteReader& /*r*/)
{
    ClientReady m;
    return m;
}

void ClientReady::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

RequestSavePlayer RequestSavePlayer::read(ByteReader& r)
{
    RequestSavePlayer m;
    m.netCode = r.u16();
    return m;
}

void RequestSavePlayer::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
}

PlayerObj PlayerObj::read(ByteReader& r)
{
    PlayerObj m;
    m.netCode = r.u16();
    m.typeId = r.u16();
    m.x = r.u16();
    m.y = r.u16();
    m.animState = r.u8();
    m.frame = r.u8();
    m.animIndex = r.u8();
    return m;
}

void PlayerObj::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u16(typeId);
    w.u16(x);
    w.u16(y);
    w.u8(animState);
    w.u8(frame);
    w.u8(animIndex);
}

KickNotification KickNotification::read(ByteReader& /*r*/)
{
    KickNotification m;
    return m;
}

void KickNotification::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

TimeoutNotification TimeoutNotification::read(ByteReader& /*r*/)
{
    TimeoutNotification m;
    return m;
}

void TimeoutNotification::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

ServerQuit ServerQuit::read(ByteReader& /*r*/)
{
    ServerQuit m;
    return m;
}

void ServerQuit::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

ServerQuitAck ServerQuitAck::read(ByteReader& /*r*/)
{
    ServerQuitAck m;
    return m;
}

void ServerQuitAck::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

ChatKill ChatKill::read(ByteReader& r)
{
    ChatKill m;
    m.netCode = r.u16();
    return m;
}

void ChatKill::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
}

MessagesKill MessagesKill::read(ByteReader& /*r*/)
{
    MessagesKill m;
    return m;
}

void MessagesKill::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

SeqImportant SeqImportant::read(ByteReader& r)
{
    SeqImportant m;
    m.id = r.u16();
    m.size = r.u8();
    { const auto tail = r.bytes(r.remaining()); m.message.assign(tail.begin(), tail.end()); }
    return m;
}

void SeqImportant::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(id);
    w.u8(size);
    w.bytes(message);
}

ReportAbilityAward ReportAbilityAward::read(ByteReader& r)
{
    ReportAbilityAward m;
    m.ability = r.u8();
    m.level = r.u8();
    return m;
}

void ReportAbilityAward::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(ability);
    w.u8(level);
}

ReportAbilityState ReportAbilityState::read(ByteReader& r)
{
    ReportAbilityState m;
    m.ability = r.u8();
    m.ready = r.u8();
    return m;
}

void ReportAbilityState::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(ability);
    w.u8(ready);
}

ReportActiveAbilities ReportActiveAbilities::read(ByteReader& r)
{
    ReportActiveAbilities m;
    m.ability = r.u8();
    m.active = r.u8();
    return m;
}

void ReportActiveAbilities::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(ability);
    w.u8(active);
}

ReportGuideAward ReportGuideAward::read(ByteReader& r)
{
    ReportGuideAward m;
    m.creature = r.u8();
    m.level = r.u8();
    return m;
}

void ReportGuideAward::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(creature);
    w.u8(level);
}

InterestingId InterestingId::read(ByteReader& r)
{
    InterestingId m;
    m.netCode = r.u16();
    m.typeId = r.u16();
    m.add = r.u8();
    m.kind = r.u8();
    return m;
}

void InterestingId::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u16(typeId);
    w.u8(add);
    w.u8(kind);
}

TimerStatus TimerStatus::read(ByteReader& r)
{
    TimerStatus m;
    m.running = r.u32();
    m.timeRemainingMs = r.u32();
    m.frame = r.u32();
    return m;
}

void TimerStatus::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u32(running);
    w.u32(timeRemainingMs);
    w.u32(frame);
}

RequestTimerStatus RequestTimerStatus::read(ByteReader& /*r*/)
{
    RequestTimerStatus m;
    return m;
}

void RequestTimerStatus::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
}

JournalMsg JournalMsg::read(ByteReader& r)
{
    JournalMsg m;
    m.action = r.u8();
    readArray(r, m.entry);
    m.type = r.u16();
    return m;
}

void JournalMsg::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(action);
    w.bytes(entry);
    w.u16(type);
}

ChapterEnd ChapterEnd::read(ByteReader& r)
{
    ChapterEnd m;
    m.chapter = r.u8();
    m.kind = r.u8();
    return m;
}

void ChapterEnd::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(chapter);
    w.u8(kind);
}

ReportAllLatency ReportAllLatency::read(ByteReader& r)
{
    ReportAllLatency m;
    m.playerId = r.u16();
    m.latency = r.u16();
    return m;
}

void ReportAllLatency::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(playerId);
    w.u16(latency);
}

ReportFlagStatus ReportFlagStatus::read(ByteReader& r)
{
    ReportFlagStatus m;
    m.state = r.u8();
    m.flagIndex = r.u8();
    m.side = r.u8();
    m.carrier = r.u16();
    return m;
}

void ReportFlagStatus::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(state);
    w.u8(flagIndex);
    w.u8(side);
    w.u16(carrier);
}

ReportBallStatus ReportBallStatus::read(ByteReader& r)
{
    ReportBallStatus m;
    m.state = r.u8();
    m.carrier = r.u16();
    return m;
}

void ReportBallStatus::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(state);
    w.u16(carrier);
}

ReportObjectPoison ReportObjectPoison::read(ByteReader& r)
{
    ReportObjectPoison m;
    m.netCode = r.u16();
    m.poison = r.u8();
    return m;
}

void ReportObjectPoison::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u8(poison);
}

ReportMonitorCreature ReportMonitorCreature::read(ByteReader& r)
{
    ReportMonitorCreature m;
    m.netCode = r.u16();
    m.typeId = r.u16();
    return m;
}

void ReportMonitorCreature::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u16(typeId);
}

ReportUnmonitorCreature ReportUnmonitorCreature::read(ByteReader& r)
{
    ReportUnmonitorCreature m;
    m.netCode = r.u16();
    return m;
}

void ReportUnmonitorCreature::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
}

ReportTotalHealth ReportTotalHealth::read(ByteReader& r)
{
    ReportTotalHealth m;
    m.netCode = r.u16();
    m.health = r.u16();
    m.maxHealth = r.u16();
    return m;
}

void ReportTotalHealth::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u16(health);
    w.u16(maxHealth);
}

ReportTotalMana ReportTotalMana::read(ByteReader& r)
{
    ReportTotalMana m;
    m.netCode = r.u16();
    m.mana = r.u16();
    m.maxMana = r.u16();
    return m;
}

void ReportTotalMana::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u16(mana);
    w.u16(maxMana);
}

ReportSpellStat ReportSpellStat::read(ByteReader& r)
{
    ReportSpellStat m;
    m.spell = r.u32();
    m.value = r.u8();
    return m;
}

void ReportSpellStat::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u32(spell);
    w.u8(value);
}

ReportSecondaryWeapon ReportSecondaryWeapon::read(ByteReader& r)
{
    ReportSecondaryWeapon m;
    if (!r.atEnd()) m.item = r.u16();
    if (!r.atEnd()) m.fallback = r.u8();
    return m;
}

void ReportSecondaryWeapon::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(item);
    w.u8(fallback);
}

ReportLastQuiver ReportLastQuiver::read(ByteReader& r)
{
    ReportLastQuiver m;
    m.netCode = r.u16();
    return m;
}

void ReportLastQuiver::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
}

InfoBookData InfoBookData::read(ByteReader& r)
{
    InfoBookData m;
    m.netCode = r.u16();
    m.value = r.u8();
    return m;
}

void InfoBookData::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u8(value);
}

Social Social::read(ByteReader& r)
{
    Social m;
    m.gesture = r.u8();
    return m;
}

void Social::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(gesture);
}

FadeBegin FadeBegin::read(ByteReader& r)
{
    FadeBegin m;
    m.out = r.u8();
    m.menu = r.u8();
    return m;
}

void FadeBegin::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(out);
    w.u8(menu);
}

MusicEvent MusicEvent::read(ByteReader& r)
{
    MusicEvent m;
    m.track = r.u8();
    m.volume = r.u8();
    return m;
}

void MusicEvent::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(track);
    w.u8(volume);
}

MusicPushEvent MusicPushEvent::read(ByteReader& r)
{
    MusicPushEvent m;
    readArray(r, m.unused);
    return m;
}

void MusicPushEvent::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.bytes(unused);
}

MusicPopEvent MusicPopEvent::read(ByteReader& r)
{
    MusicPopEvent m;
    readArray(r, m.unused);
    return m;
}

void MusicPopEvent::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.bytes(unused);
}

PlayerDied PlayerDied::read(ByteReader& r)
{
    PlayerDied m;
    m.playerId = r.u16();
    return m;
}

void PlayerDied::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(playerId);
}

PlayerRespawn PlayerRespawn::read(ByteReader& r)
{
    PlayerRespawn m;
    m.netCode = r.u16();
    m.frame = r.u32();
    m.equipMask = r.u8();
    m.flags = r.u8();
    return m;
}

void PlayerRespawn::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(netCode);
    w.u32(frame);
    w.u8(equipMask);
    w.u8(flags);
}

ForgetDrawables ForgetDrawables::read(ByteReader& r)
{
    ForgetDrawables m;
    m.frame = r.u32();
    return m;
}

void ForgetDrawables::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u32(frame);
}

ResetAbilities ResetAbilities::read(ByteReader& r)
{
    ResetAbilities m;
    m.ability = r.u8();
    return m;
}

void ResetAbilities::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(ability);
}

RateChange RateChange::read(ByteReader& r)
{
    RateChange m;
    m.rate = r.u8();
    return m;
}

void RateChange::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(rate);
}

ReportCreatureCmd ReportCreatureCmd::read(ByteReader& r)
{
    ReportCreatureCmd m;
    m.command = r.u8();
    return m;
}

void ReportCreatureCmd::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(command);
}

StatMultipliers StatMultipliers::read(ByteReader& r)
{
    StatMultipliers m;
    m.health = r.f32();
    m.mana = r.f32();
    m.strength = r.f32();
    m.speed = r.f32();
    return m;
}

void StatMultipliers::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.f32(health);
    w.f32(mana);
    w.f32(strength);
    w.f32(speed);
}

InventoryFail InventoryFail::read(ByteReader& r)
{
    InventoryFail m;
    m.item = r.u16();
    return m;
}

void InventoryFail::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u16(item);
}

} // namespace msg

namespace msg::dialog {

StartDialog StartDialog::read(ByteReader& r)
{
    StartDialog m;
    m.netCode = r.u16();
    return m;
}

void StartDialog::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(netCode);
}

Answer Answer::read(ByteReader& r)
{
    Answer m;
    m.answer = r.u8();
    return m;
}

void Answer::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u8(answer);
}

OpenNPCDialog OpenNPCDialog::read(ByteReader& r)
{
    OpenNPCDialog m;
    readArray(r, m.textId);
    readArray(r, m.title);
    m.sound = r.u32();
    readArray(r, m.portrait);
    m.flags = r.u8();
    return m;
}

void OpenNPCDialog::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.bytes(textId);
    w.bytes(title);
    w.u32(sound);
    w.bytes(portrait);
    w.u8(flags);
}

CloseNPCDialog CloseNPCDialog::read(ByteReader& /*r*/)
{
    CloseNPCDialog m;
    return m;
}

void CloseNPCDialog::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
}

} // namespace msg::dialog

namespace msg::gauntlet {

QuestLevelStart QuestLevelStart::read(ByteReader& /*r*/)
{
    QuestLevelStart m;
    return m;
}

void QuestLevelStart::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
}

PlayerJoinedQuest PlayerJoinedQuest::read(ByteReader& r)
{
    PlayerJoinedQuest m;
    m.playerId = r.u16();
    return m;
}

void PlayerJoinedQuest::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(playerId);
}

GameOver GameOver::read(ByteReader& r)
{
    GameOver m;
    m.generatorsDestroyed = r.u16();
    m.secretsFound = r.u16();
    m.kills = r.u16();
    m.stage = r.u16();
    readArray(r, m.unused);
    return m;
}

void GameOver::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(generatorsDestroyed);
    w.u16(secretsFound);
    w.u16(kills);
    w.u16(stage);
    w.bytes(unused);
}

RespawnObserver RespawnObserver::read(ByteReader& /*r*/)
{
    RespawnObserver m;
    return m;
}

void RespawnObserver::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
}

SetLives SetLives::read(ByteReader& r)
{
    SetLives m;
    m.lives = r.u8();
    m.playerId = r.u16();
    return m;
}

void SetLives::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u8(lives);
    w.u16(playerId);
}

Unused05 Unused05::read(ByteReader& r)
{
    Unused05 m;
    m.unused = r.u16();
    return m;
}

void Unused05::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(unused);
}

Unused0B Unused0B::read(ByteReader& r)
{
    Unused0B m;
    readArray(r, m.unused);
    return m;
}

void Unused0B::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.bytes(unused);
}

LevelWon LevelWon::read(ByteReader& r)
{
    LevelWon m;
    m.secretsTotal = r.u16();
    m.stage = r.u16();
    readArray(r, m.players);
    return m;
}

void LevelWon::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(secretsTotal);
    w.u16(stage);
    w.bytes(players);
}

BriefingNextLevel BriefingNextLevel::read(ByteReader& r)
{
    BriefingNextLevel m;
    m.stage = r.u16();
    m.flags = r.u8();
    readArray(r, m.image);
    readArray(r, m.textId);
    return m;
}

void BriefingNextLevel::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(stage);
    w.u8(flags);
    w.bytes(image);
    w.bytes(textId);
}

QuestStartBriefing QuestStartBriefing::read(ByteReader& r)
{
    QuestStartBriefing m;
    m.stage = r.u16();
    m.flags = r.u8();
    readArray(r, m.image);
    readArray(r, m.textId);
    return m;
}

void QuestStartBriefing::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(stage);
    w.u8(flags);
    w.bytes(image);
    w.bytes(textId);
}

ClearMarker ClearMarker::read(ByteReader& r)
{
    ClearMarker m;
    m.netCode = r.u16();
    return m;
}

void ClearMarker::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(netCode);
}

GreenZapEffect GreenZapEffect::read(ByteReader& r)
{
    GreenZapEffect m;
    m.x1 = r.u16();
    m.y1 = r.u16();
    m.x2 = r.u16();
    m.y2 = r.u16();
    m.duration = r.u16();
    return m;
}

void GreenZapEffect::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(x1);
    w.u16(y1);
    w.u16(x2);
    w.u16(y2);
    w.u16(duration);
}

RevokeSpell RevokeSpell::read(ByteReader& r)
{
    RevokeSpell m;
    m.spell = r.u16();
    return m;
}

void RevokeSpell::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(spell);
}

DisableAbility DisableAbility::read(ByteReader& r)
{
    DisableAbility m;
    m.ability = r.u16();
    return m;
}

void DisableAbility::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(ability);
}

RevokeFieldGuideCreature RevokeFieldGuideCreature::read(ByteReader& r)
{
    RevokeFieldGuideCreature m;
    m.creature = r.u16();
    return m;
}

void RevokeFieldGuideCreature::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(creature);
}

OpenRankWindow OpenRankWindow::read(ByteReader& /*r*/)
{
    OpenRankWindow m;
    return m;
}

void OpenRankWindow::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
}

SetPlayerScore SetPlayerScore::read(ByteReader& r)
{
    SetPlayerScore m;
    m.value = r.u32();
    m.playerId = r.u16();
    return m;
}

void SetPlayerScore::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(value);
    w.u16(playerId);
}

SetPlayerByte4824 SetPlayerByte4824::read(ByteReader& r)
{
    SetPlayerByte4824 m;
    m.value = r.u8();
    m.playerId = r.u16();
    return m;
}

void SetPlayerByte4824::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u8(value);
    w.u16(playerId);
}

SetPlayerByte4825 SetPlayerByte4825::read(ByteReader& r)
{
    SetPlayerByte4825 m;
    m.value = r.u8();
    m.playerId = r.u16();
    return m;
}

void SetPlayerByte4825::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u8(value);
    w.u16(playerId);
}

ToggleWarpGUI ToggleWarpGUI::read(ByteReader& r)
{
    ToggleWarpGUI m;
    m.on = r.u8();
    return m;
}

void ToggleWarpGUI::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u8(on);
}

WarpExplosionFX WarpExplosionFX::read(ByteReader& r)
{
    WarpExplosionFX m;
    m.x = r.i16();
    m.y = r.i16();
    m.count = r.u8();
    return m;
}

void WarpExplosionFX::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.i16(x);
    w.i16(y);
    w.u8(count);
}

PointFX PointFX::read(ByteReader& r)
{
    PointFX m;
    m.x = r.i16();
    m.y = r.i16();
    return m;
}

void PointFX::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.i16(x);
    w.i16(y);
}

HostQuestReady HostQuestReady::read(ByteReader& /*r*/)
{
    HostQuestReady m;
    return m;
}

void HostQuestReady::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
}

SaveTransferInProgress SaveTransferInProgress::read(ByteReader& r)
{
    SaveTransferInProgress m;
    m.active = r.u8();
    return m;
}

void SaveTransferInProgress::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u8(active);
}

HighestQuestStage HighestQuestStage::read(ByteReader& r)
{
    HighestQuestStage m;
    m.stage = r.u16();
    return m;
}

void HighestQuestStage::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(stage);
}

SpellAwardedText SpellAwardedText::read(ByteReader& r)
{
    SpellAwardedText m;
    m.spell = r.u8();
    m.playerId = r.u16();
    return m;
}

void SpellAwardedText::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u8(spell);
    w.u16(playerId);
}

FieldGuideAwardedText FieldGuideAwardedText::read(ByteReader& r)
{
    FieldGuideAwardedText m;
    m.creature = r.u8();
    m.playerId = r.u16();
    return m;
}

void FieldGuideAwardedText::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u8(creature);
    w.u16(playerId);
}

AbilityAwardedText AbilityAwardedText::read(ByteReader& r)
{
    AbilityAwardedText m;
    m.ability = r.u8();
    m.playerId = r.u16();
    return m;
}

void AbilityAwardedText::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u8(ability);
    w.u16(playerId);
}

FormattedNotice FormattedNotice::read(ByteReader& r)
{
    FormattedNotice m;
    readArray(r, m.textId);
    m.argIndex = r.u8();
    return m;
}

void FormattedNotice::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.bytes(textId);
    w.u8(argIndex);
}

} // namespace msg::gauntlet

namespace msg::inform {

SpellError SpellError::read(ByteReader& r)
{
    SpellError m;
    m.index = r.u32();
    return m;
}

void SpellError::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(index);
}

SpellOK SpellOK::read(ByteReader& r)
{
    SpellOK m;
    m.index = r.u32();
    return m;
}

void SpellOK::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(index);
}

Ability Ability::read(ByteReader& r)
{
    Ability m;
    m.index = r.u32();
    return m;
}

void Ability::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(index);
}

PlayerTimedOut PlayerTimedOut::read(ByteReader& r)
{
    PlayerTimedOut m;
    m.playerId = r.u32();
    return m;
}

void PlayerTimedOut::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(playerId);
}

FlagRetrieved FlagRetrieved::read(ByteReader& r)
{
    FlagRetrieved m;
    m.playerId = r.u32();
    return m;
}

void FlagRetrieved::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(playerId);
}

FlagCaptured FlagCaptured::read(ByteReader& r)
{
    FlagCaptured m;
    m.playerId = r.u32();
    m.teamId = r.u32();
    return m;
}

void FlagCaptured::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(playerId);
    w.u32(teamId);
}

FlagPickedUp FlagPickedUp::read(ByteReader& r)
{
    FlagPickedUp m;
    m.playerId = r.u32();
    m.teamId = r.u32();
    return m;
}

void FlagPickedUp::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(playerId);
    w.u32(teamId);
}

FlagDropped FlagDropped::read(ByteReader& r)
{
    FlagDropped m;
    m.playerId = r.u32();
    m.teamId = r.u32();
    return m;
}

void FlagDropped::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(playerId);
    w.u32(teamId);
}

FlagRespawned FlagRespawned::read(ByteReader& r)
{
    FlagRespawned m;
    m.teamId = r.u32();
    return m;
}

void FlagRespawned::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(teamId);
}

BallScored BallScored::read(ByteReader& r)
{
    BallScored m;
    m.playerId = r.u32();
    m.teamId = r.u32();
    return m;
}

void BallScored::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(playerId);
    w.u32(teamId);
}

CrownPickedUp CrownPickedUp::read(ByteReader& r)
{
    CrownPickedUp m;
    m.playerId = r.u32();
    m.teamId = r.u32();
    return m;
}

void CrownPickedUp::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(playerId);
    w.u32(teamId);
}

CrownDropped CrownDropped::read(ByteReader& r)
{
    CrownDropped m;
    m.playerId = r.u32();
    m.teamId = r.u32();
    return m;
}

void CrownDropped::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(playerId);
    w.u32(teamId);
}

ObserverMode ObserverMode::read(ByteReader& r)
{
    ObserverMode m;
    m.canJoin = r.u32();
    return m;
}

void ObserverMode::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(canJoin);
}

PlayerState PlayerState::read(ByteReader& r)
{
    PlayerState m;
    m.state = r.u32();
    return m;
}

void PlayerState::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(state);
}

KillFeed KillFeed::read(ByteReader& r)
{
    KillFeed m;
    m.attacker = r.u16();
    m.assist = r.u16();
    m.victim = r.u16();
    m.weapon = r.u16();
    m.kind = r.u8();
    return m;
}

void KillFeed::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(attacker);
    w.u16(assist);
    w.u16(victim);
    w.u16(weapon);
    w.u8(kind);
}

String String::read(ByteReader& r)
{
    String m;
    m.isSign = r.u8();
    { const auto tail = r.bytes(r.remaining()); m.textId.assign(tail.begin(), tail.end()); }
    return m;
}

void String::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u8(isSign);
    w.bytes(textId);
}

WrongTeam WrongTeam::read(ByteReader& r)
{
    WrongTeam m;
    m.teamId = r.u32();
    return m;
}

void WrongTeam::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(teamId);
}

ClassChanged ClassChanged::read(ByteReader& /*r*/)
{
    ClassChanged m;
    return m;
}

void ClassChanged::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
}

PlayerExited PlayerExited::read(ByteReader& r)
{
    PlayerExited m;
    m.playerId = r.u32();
    return m;
}

void PlayerExited::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(playerId);
}

PlayerWarped PlayerWarped::read(ByteReader& r)
{
    PlayerWarped m;
    m.playerId = r.u32();
    return m;
}

void PlayerWarped::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(playerId);
}

PlayerFoundSecret PlayerFoundSecret::read(ByteReader& r)
{
    PlayerFoundSecret m;
    m.playerId = r.u32();
    return m;
}

void PlayerFoundSecret::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(playerId);
}

WarpWait WarpWait::read(ByteReader& r)
{
    WarpWait m;
    m.stage = r.u32();
    return m;
}

void WarpWait::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(stage);
}

} // namespace msg::inform

namespace msg::team {

CreateUpdateTeam CreateUpdateTeam::read(ByteReader& r)
{
    CreateUpdateTeam m;
    m.teamId = r.u32();
    m.field60 = r.u32();
    m.score = r.u32();
    m.flags = r.u8();
    m.nameLen = r.u8();
    m.color = r.u8();
    m.useTitle = r.u8();
    { const auto tail = r.bytes(r.remaining()); m.name.assign(tail.begin(), tail.end()); }
    return m;
}

void CreateUpdateTeam::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(teamId);
    w.u32(field60);
    w.u32(score);
    w.u8(flags);
    w.u8(nameLen);
    w.u8(color);
    w.u8(useTitle);
    w.bytes(name);
}

AddMember AddMember::read(ByteReader& r)
{
    AddMember m;
    m.teamId = r.u32();
    m.netCode = r.u16();
    m.typeId = r.u16();
    return m;
}

void AddMember::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(teamId);
    w.u16(netCode);
    w.u16(typeId);
}

RemoveFromTeam RemoveFromTeam::read(ByteReader& r)
{
    RemoveFromTeam m;
    m.netCode = r.u32();
    return m;
}

void RemoveFromTeam::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(netCode);
}

ChangeTeam ChangeTeam::read(ByteReader& r)
{
    ChangeTeam m;
    m.teamId = r.u32();
    m.netCode = r.u16();
    readArray(r, m.pad);
    return m;
}

void ChangeTeam::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(teamId);
    w.u16(netCode);
    w.bytes(pad);
}

RenameTeam RenameTeam::read(ByteReader& r)
{
    RenameTeam m;
    m.teamId = r.u32();
    readArray(r, m.name);
    return m;
}

void RenameTeam::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(teamId);
    w.bytes(name);
}

RemoveAllMembers RemoveAllMembers::read(ByteReader& r)
{
    RemoveAllMembers m;
    m.teamId = r.u32();
    return m;
}

void RemoveAllMembers::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(teamId);
}

RemoveTeam RemoveTeam::read(ByteReader& r)
{
    RemoveTeam m;
    m.teamId = r.u32();
    return m;
}

void RemoveTeam::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(teamId);
}

RemoveAllTeams RemoveAllTeams::read(ByteReader& /*r*/)
{
    RemoveAllTeams m;
    return m;
}

void RemoveAllTeams::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
}

SetTeamScore SetTeamScore::read(ByteReader& r)
{
    SetTeamScore m;
    m.teamId = r.u32();
    m.score = r.u32();
    return m;
}

void SetTeamScore::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(teamId);
    w.u32(score);
}

ResetAllTeamScores ResetAllTeamScores::read(ByteReader& /*r*/)
{
    ResetAllTeamScores m;
    return m;
}

void ResetAllTeamScores::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
}

RequestJoinTeam RequestJoinTeam::read(ByteReader& r)
{
    RequestJoinTeam m;
    m.teamId = r.u32();
    m.netCode = r.u16();
    readArray(r, m.pad);
    return m;
}

void RequestJoinTeam::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(teamId);
    w.u16(netCode);
    w.bytes(pad);
}

SwitchTeam SwitchTeam::read(ByteReader& r)
{
    SwitchTeam m;
    m.teamId = r.u32();
    m.netCode = r.u16();
    readArray(r, m.pad);
    return m;
}

void SwitchTeam::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(teamId);
    w.u16(netCode);
    w.bytes(pad);
}

MemberHealth MemberHealth::read(ByteReader& r)
{
    MemberHealth m;
    m.netCode = r.u16();
    m.healthPct = r.u8();
    return m;
}

void MemberHealth::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(netCode);
    w.u8(healthPct);
}

} // namespace msg::team

namespace msg::trade {

CloseP2PTradeWindow CloseP2PTradeWindow::read(ByteReader& /*r*/)
{
    CloseP2PTradeWindow m;
    return m;
}

void CloseP2PTradeWindow::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
}

CloseShopWindow CloseShopWindow::read(ByteReader& /*r*/)
{
    CloseShopWindow m;
    return m;
}

void CloseShopWindow::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
}

AcceptState AcceptState::read(ByteReader& r)
{
    AcceptState m;
    m.accepted = r.u8();
    return m;
}

void AcceptState::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u8(accepted);
}

AddItemToTrade AddItemToTrade::read(ByteReader& r)
{
    AddItemToTrade m;
    m.side = r.u8();
    m.typeId = r.u16();
    m.netCode = r.u16();
    m.value = r.u32();
    readArray(r, m.modifiers);
    return m;
}

void AddItemToTrade::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u8(side);
    w.u16(typeId);
    w.u16(netCode);
    w.u32(value);
    w.bytes(modifiers);
}

RemoveItemFromTrade RemoveItemFromTrade::read(ByteReader& r)
{
    RemoveItemFromTrade m;
    m.netCode = r.u16();
    return m;
}

void RemoveItemFromTrade::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(netCode);
}

GoldValues GoldValues::read(ByteReader& r)
{
    GoldValues m;
    m.gold1 = r.u32();
    m.gold2 = r.u32();
    m.gold3 = r.u32();
    return m;
}

void GoldValues::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u32(gold1);
    w.u32(gold2);
    w.u32(gold3);
}

PrepareP2PTrade PrepareP2PTrade::read(ByteReader& /*r*/)
{
    PrepareP2PTrade m;
    return m;
}

void PrepareP2PTrade::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
}

ShopAddItem ShopAddItem::read(ByteReader& r)
{
    ShopAddItem m;
    m.typeId = r.u16();
    m.netCode = r.u16();
    m.price = r.u32();
    m.durability = r.u32();
    readArray(r, m.modifiers);
    return m;
}

void ShopAddItem::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(typeId);
    w.u16(netCode);
    w.u32(price);
    w.u32(durability);
    w.bytes(modifiers);
}

ShopRemoveItem ShopRemoveItem::read(ByteReader& r)
{
    ShopRemoveItem m;
    m.netCode = r.u16();
    return m;
}

void ShopRemoveItem::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(netCode);
}

StartP2PTrade StartP2PTrade::read(ByteReader& r)
{
    StartP2PTrade m;
    readArray(r, m.partnerName);
    return m;
}

void StartP2PTrade::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.bytes(partnerName);
}

StartShop StartShop::read(ByteReader& r)
{
    StartShop m;
    m.shopkeeperType = r.u16();
    readArray(r, m.shopName);
    readArray(r, m.greeting);
    return m;
}

void StartShop::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(shopkeeperType);
    w.bytes(shopName);
    w.bytes(greeting);
}

CancelP2PTrade CancelP2PTrade::read(ByteReader& /*r*/)
{
    CancelP2PTrade m;
    return m;
}

void CancelP2PTrade::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
}

OfferItem OfferItem::read(ByteReader& r)
{
    OfferItem m;
    m.item = r.u16();
    return m;
}

void OfferItem::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(item);
}

WithdrawItem WithdrawItem::read(ByteReader& r)
{
    WithdrawItem m;
    m.item = r.u16();
    return m;
}

void WithdrawItem::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(item);
}

AcceptTrade AcceptTrade::read(ByteReader& /*r*/)
{
    AcceptTrade m;
    return m;
}

void AcceptTrade::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
}

ExitShop ExitShop::read(ByteReader& /*r*/)
{
    ExitShop m;
    return m;
}

void ExitShop::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
}

OpenShop OpenShop::read(ByteReader& r)
{
    OpenShop m;
    m.shopkeeper = r.u16();
    return m;
}

void OpenShop::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(shopkeeper);
}

BuyItem BuyItem::read(ByteReader& r)
{
    BuyItem m;
    m.item = r.u16();
    return m;
}

void BuyItem::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(item);
}

BuyStack BuyStack::read(ByteReader& r)
{
    BuyStack m;
    m.typeId = r.u16();
    m.count = r.u8();
    return m;
}

void BuyStack::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(typeId);
    w.u8(count);
}

SellItem SellItem::read(ByteReader& r)
{
    SellItem m;
    m.item = r.u16();
    return m;
}

void SellItem::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(item);
}

SellStack SellStack::read(ByteReader& r)
{
    SellStack m;
    m.typeId = r.u16();
    m.count = r.u8();
    return m;
}

void SellStack::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(typeId);
    w.u8(count);
}

RepairRechargeItem RepairRechargeItem::read(ByteReader& r)
{
    RepairRechargeItem m;
    m.item = r.u16();
    return m;
}

void RepairRechargeItem::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(item);
}

NotEnoughGold NotEnoughGold::read(ByteReader& r)
{
    NotEnoughGold m;
    m.shortBy = r.u16();
    return m;
}

void NotEnoughGold::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(shortBy);
}

AskSellPrice AskSellPrice::read(ByteReader& r)
{
    AskSellPrice m;
    m.item = r.u16();
    return m;
}

void AskSellPrice::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(item);
}

SellPriceOffer SellPriceOffer::read(ByteReader& r)
{
    SellPriceOffer m;
    m.item = r.u16();
    m.price = r.u32();
    return m;
}

void SellPriceOffer::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(item);
    w.u32(price);
}

AskRepairCost AskRepairCost::read(ByteReader& r)
{
    AskRepairCost m;
    m.item = r.u16();
    return m;
}

void AskRepairCost::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(item);
}

RepairCostOffer RepairCostOffer::read(ByteReader& r)
{
    RepairCostOffer m;
    m.item = r.u16();
    m.cost = r.u32();
    return m;
}

void RepairCostOffer::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u16(item);
    w.u32(cost);
}

} // namespace msg::trade

namespace msg::vote {

VoteAgainstPlayer VoteAgainstPlayer::read(ByteReader& r)
{
    VoteAgainstPlayer m;
    readArray(r, m.playerName);
    return m;
}

void VoteAgainstPlayer::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.bytes(playerName);
}

VoteKind1AgainstPlayer VoteKind1AgainstPlayer::read(ByteReader& r)
{
    VoteKind1AgainstPlayer m;
    readArray(r, m.playerName);
    return m;
}

void VoteKind1AgainstPlayer::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.bytes(playerName);
}

WithdrawVoteAgainstPlayer WithdrawVoteAgainstPlayer::read(ByteReader& r)
{
    WithdrawVoteAgainstPlayer m;
    readArray(r, m.playerName);
    return m;
}

void WithdrawVoteAgainstPlayer::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.bytes(playerName);
}

WithdrawVoteKind1 WithdrawVoteKind1::read(ByteReader& r)
{
    WithdrawVoteKind1 m;
    readArray(r, m.playerName);
    return m;
}

void WithdrawVoteKind1::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.bytes(playerName);
}

VoteKind2NoTarget VoteKind2NoTarget::read(ByteReader& /*r*/)
{
    VoteKind2NoTarget m;
    return m;
}

void VoteKind2NoTarget::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
}

WithdrawVoteKind2 WithdrawVoteKind2::read(ByteReader& /*r*/)
{
    WithdrawVoteKind2 m;
    return m;
}

void WithdrawVoteKind2::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
}

VoteState VoteState::read(ByteReader& r)
{
    VoteState m;
    m.state = r.u8();
    return m;
}

void VoteState::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u8(state);
}

VoteReset VoteReset::read(ByteReader& /*r*/)
{
    VoteReset m;
    return m;
}

void VoteReset::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
}

} // namespace msg::vote

namespace msg::xfer {

Start Start::read(ByteReader& r)
{
    Start m;
    m.action = r.u8();
    m.unk = r.u8();
    m.size = r.u32();
    readArray(r, m.type);
    m.sendId = r.u8();
    readArray(r, m.unk2);
    return m;
}

void Start::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u8(action);
    w.u8(unk);
    w.u32(size);
    w.bytes(type);
    w.u8(sendId);
    w.bytes(unk2);
}

Accept Accept::read(ByteReader& r)
{
    Accept m;
    m.recvId = r.u8();
    m.sendId = r.u8();
    return m;
}

void Accept::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u8(recvId);
    w.u8(sendId);
}

Data Data::read(ByteReader& r)
{
    Data m;
    m.recvId = r.u8();
    m.token = r.u8();
    m.chunk = r.u16();
    m.size = r.u16();
    { const auto tail = r.bytes(r.remaining()); m.data.assign(tail.begin(), tail.end()); }
    return m;
}

void Data::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u8(recvId);
    w.u8(token);
    w.u16(chunk);
    w.u16(size);
    w.bytes(data);
}

Ack Ack::read(ByteReader& r)
{
    Ack m;
    m.recvId = r.u8();
    m.token = r.u8();
    m.chunk = r.u16();
    return m;
}

void Ack::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u8(recvId);
    w.u8(token);
    w.u16(chunk);
}

Done Done::read(ByteReader& r)
{
    Done m;
    m.recvId = r.u8();
    return m;
}

void Done::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u8(recvId);
}

Cancel Cancel::read(ByteReader& r)
{
    Cancel m;
    m.recvId = r.u8();
    m.reason = r.u8();
    return m;
}

void Cancel::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u8(recvId);
    w.u8(reason);
}

Abort Abort::read(ByteReader& r)
{
    Abort m;
    m.recvId = r.u8();
    m.reason = r.u8();
    return m;
}

void Abort::write(ByteWriter& w) const
{
    w.u8(static_cast<std::uint8_t>(kOp));
    w.u8(kSub);
    w.u8(recvId);
    w.u8(reason);
}

} // namespace msg::xfer

} // namespace nox::net
