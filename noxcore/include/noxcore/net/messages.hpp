// Generated from the protocol spec (each message's fields and sizes, as the exe reads and
// writes them).
//
// Every 1.2b network message: the opcode enum, per-opcode sizes as each side decodes them,
// and one struct per message (sub-op messages in a namespace per family). Offsets count from
// the opcode byte. Sizes include the opcode (and sub-op) byte.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace nox {
class ByteReader;
class ByteWriter;
} // namespace nox

namespace nox::net {

/// Size of a message whose length depends on its contents.
inline constexpr std::size_t kVariableSize = 0xFFFF;

enum class Op : std::uint8_t {
    ServerConnect = 0x00, ///< MSG_SERVER_CONNECT (transport)
    ServerAccept = 0x01, ///< MSG_SERVER_ACCEPT (transport)
    Code2 = 0x02, ///< MSG_CODE2 (transport)
    Code3 = 0x03, ///< MSG_CODE3 (transport)
    Code4 = 0x04, ///< MSG_CODE4 (transport)
    Code5 = 0x05, ///< MSG_CODE5 (transport)
    ClientPing = 0x06, ///< MSG_CLIENT_PING (transport)
    Code7 = 0x07, ///< MSG_CODE7 (transport)
    ClientPong = 0x08, ///< MSG_CLIENT_PONG (transport)
    Code9 = 0x09, ///< MSG_CODE9 (transport)
    ClientClose = 0x0A, ///< MSG_CLIENT_CLOSE (transport)
    ServerClose = 0x0B, ///< MSG_SERVER_CLOSE (transport)
    ServerDiscover = 0x0C, ///< MSG_SERVER_DISCOVER (transport)
    ServerInfo = 0x0D, ///< MSG_SERVER_INFO (transport)
    ServerTryJoin = 0x0E, ///< MSG_SERVER_TRY_JOIN (transport)
    PasswordRequired = 0x0F, ///< MSG_PASSWORD_REQUIRED (transport)
    ServerPing = 0x10, ///< MSG_SERVER_PING (transport)
    ServerPassword = 0x11, ///< MSG_SERVER_PASSWORD (transport)
    ServerPong = 0x12, ///< MSG_SERVER_PONG (transport)
    ServerError = 0x13, ///< MSG_SERVER_ERROR (transport)
    ServerJoinOk = 0x14, ///< MSG_SERVER_JOIN_OK (transport)
    ServerJoinFail = 0x15, ///< MSG_SERVER_JOIN_FAIL (transport)
    Code22 = 0x16, ///< MSG_CODE22 (transport)
    Code23 = 0x17, ///< MSG_CODE23 (transport)
    Code24 = 0x18, ///< MSG_CODE24 (transport)
    Code25 = 0x19, ///< MSG_CODE25 (transport)
    Code26 = 0x1A, ///< MSG_CODE26 (transport)
    Code27 = 0x1B, ///< MSG_CODE27 (transport)
    Code28 = 0x1C, ///< MSG_CODE28 (transport)
    Code29 = 0x1D, ///< MSG_CODE29 (transport)
    Code30 = 0x1E, ///< MSG_CODE30 (transport)
    Accepted = 0x1F, ///< MSG_ACCEPTED (transport)
    ClientAccept = 0x20, ///< MSG_CLIENT_ACCEPT (transport)
    ServerCloseAck = 0x21, ///< MSG_SERVER_CLOSE_ACK (S->C)
    ClientCloseAck = 0x22, ///< MSG_CLIENT_CLOSE_ACK (transport)
    Speed = 0x23, ///< MSG_SPEED (transport)
    Ping = 0x24, ///< MSG_PING (transport)
    Code37 = 0x25, ///< MSG_CODE37 (transport)
    Code38 = 0x26, ///< MSG_CODE38 (C->S)
    Timestamp = 0x27, ///< MSG_TIMESTAMP (S->C)
    FullTimestamp = 0x28, ///< MSG_FULL_TIMESTAMP (S->C)
    NeedTimestamp = 0x29, ///< MSG_NEED_TIMESTAMP (C->S)
    SimulatedTimestamp = 0x2A, ///< MSG_SIMULATED_TIMESTAMP (S->C)
    UseMap = 0x2B, ///< MSG_USE_MAP (S->C)
    JoinData = 0x2C, ///< MSG_JOIN_DATA (S->C)
    NewPlayer = 0x2D, ///< MSG_NEW_PLAYER (S->C)
    PlayerQuit = 0x2E, ///< MSG_PLAYER_QUIT (S->C)
    SimpleObj = 0x2F, ///< MSG_SIMPLE_OBJ (S->C)
    ComplexObj = 0x30, ///< MSG_COMPLEX_OBJ (S->C)
    DestroyObject = 0x31, ///< MSG_DESTROY_OBJECT (S->C)
    ObjectOutOfSight = 0x32, ///< MSG_OBJECT_OUT_OF_SIGHT (S->C)
    ObjectInShadows = 0x33, ///< MSG_OBJECT_IN_SHADOWS (S->C)
    ObjectFriendAdd = 0x34, ///< MSG_OBJECT_FRIEND_ADD (S->C)
    ObjectFriendRemove = 0x35, ///< MSG_OBJECT_FRIEND_REMOVE (S->C)
    ResetFriends = 0x36, ///< MSG_RESET_FRIENDS (S->C)
    EnableObject = 0x37, ///< MSG_ENABLE_OBJECT (S->C)
    DisableObject = 0x38, ///< MSG_DISABLE_OBJECT (S->C)
    DrawFrame = 0x39, ///< MSG_DRAW_FRAME (S->C)
    DestroyWall = 0x3A, ///< MSG_DESTROY_WALL (S->C)
    OpenWall = 0x3B, ///< MSG_OPEN_WALL (S->C)
    CloseWall = 0x3C, ///< MSG_CLOSE_WALL (S->C)
    ChangeOrAddWallMagic = 0x3D, ///< MSG_CHANGE_OR_ADD_WALL_MAGIC (S->C)
    RemoveWallMagic = 0x3E, ///< MSG_REMOVE_WALL_MAGIC (S->C)
    PlayerInput = 0x3F, ///< MSG_PLAYER_INPUT (C->S)
    PlayerSetWaypoint = 0x40, ///< MSG_PLAYER_SET_WAYPOINT (C->S)
    ReportHealth = 0x41, ///< MSG_REPORT_HEALTH (S->C)
    ReportHealthDelta = 0x42, ///< MSG_REPORT_HEALTH_DELTA (S->C)
    ReportPlayerHealth = 0x43, ///< MSG_REPORT_PLAYER_HEALTH (S->C)
    ReportItemHealth = 0x44, ///< MSG_REPORT_ITEM_HEALTH (S->C)
    ReportMana = 0x45, ///< MSG_REPORT_MANA (S->C)
    ReportPoison = 0x46, ///< MSG_REPORT_POISON (-)
    ReportStamina = 0x47, ///< MSG_REPORT_STAMINA (S->C)
    ReportStats = 0x48, ///< MSG_REPORT_STATS (S->C)
    ReportArmorValue = 0x49, ///< MSG_REPORT_ARMOR_VALUE (S->C)
    ReportGold = 0x4A, ///< MSG_REPORT_GOLD (S->C)
    ReportPickup = 0x4B, ///< MSG_REPORT_PICKUP (S->C)
    ReportModifiablePickup = 0x4C, ///< MSG_REPORT_MODIFIABLE_PICKUP (S->C)
    ReportDrop = 0x4D, ///< MSG_REPORT_DROP (S->C)
    ReportLesson = 0x4E, ///< MSG_REPORT_LESSON (S->C)
    ReportMundaneArmorEquip = 0x4F, ///< MSG_REPORT_MUNDANE_ARMOR_EQUIP (S->C)
    ReportMundaneWeaponEquip = 0x50, ///< MSG_REPORT_MUNDANE_WEAPON_EQUIP (S->C)
    ReportModifiableWeaponEquip = 0x51, ///< MSG_REPORT_MODIFIABLE_WEAPON_EQUIP (S->C)
    ReportModifiableArmorEquip = 0x52, ///< MSG_REPORT_MODIFIABLE_ARMOR_EQUIP (S->C)
    ReportArmorDequip = 0x53, ///< MSG_REPORT_ARMOR_DEQUIP (S->C)
    ReportWeaponDequip = 0x54, ///< MSG_REPORT_WEAPON_DEQUIP (S->C)
    ReportTreasureCount = 0x55, ///< MSG_REPORT_TREASURE_COUNT (S->C)
    ReportFlagBallWinner = 0x56, ///< MSG_REPORT_FLAG_BALL_WINNER (S->C)
    ReportFlagWinner = 0x57, ///< MSG_REPORT_FLAG_WINNER (S->C)
    ReportDeathmatchWinner = 0x58, ///< MSG_REPORT_DEATHMATCH_WINNER (S->C)
    ReportDeathmatchTeamWinner = 0x59, ///< MSG_REPORT_DEATHMATCH_TEAM_WINNER (S->C)
    ReportEnchantment = 0x5A, ///< MSG_REPORT_ENCHANTMENT (S->C)
    ReportItemEnchantment = 0x5B, ///< MSG_REPORT_ITEM_ENCHANTMENT (S->C)
    ReportLightColor = 0x5C, ///< MSG_REPORT_LIGHT_COLOR (S->C)
    ReportLightIntensity = 0x5D, ///< MSG_REPORT_LIGHT_INTENSITY (S->C)
    ReportZPlus = 0x5E, ///< MSG_REPORT_Z_PLUS (S->C)
    ReportZMinus = 0x5F, ///< MSG_REPORT_Z_MINUS (S->C)
    ReportEquip = 0x60, ///< MSG_REPORT_EQUIP (S->C)
    ReportDequip = 0x61, ///< MSG_REPORT_DEQUIP (S->C)
    ReportAcquireSpell = 0x62, ///< MSG_REPORT_ACQUIRE_SPELL (-)
    ReportTarget = 0x63, ///< MSG_REPORT_TARGET (-)
    ReportCharges = 0x64, ///< MSG_REPORT_CHARGES (S->C)
    ReportXStatus = 0x65, ///< MSG_REPORT_X_STATUS (S->C)
    ReportPlayerStatus = 0x66, ///< MSG_REPORT_PLAYER_STATUS (S->C)
    ReportModifier = 0x67, ///< MSG_REPORT_MODIFIER (S->C)
    ReportStatModifier = 0x68, ///< MSG_REPORT_STAT_MODIFIER (S->C)
    ReportNpc = 0x69, ///< MSG_REPORT_NPC (S->C)
    ReportClientStatus = 0x6A, ///< MSG_REPORT_CLIENT_STATUS (S->C)
    ReportAnimationFrame = 0x6B, ///< MSG_REPORT_ANIMATION_FRAME (S->C)
    ReportAcquireCreature = 0x6C, ///< MSG_REPORT_ACQUIRE_CREATURE (S->C)
    ReportLoseCreature = 0x6D, ///< MSG_REPORT_LOSE_CREATURE (S->C)
    ReportExperience = 0x6E, ///< MSG_REPORT_EXPERIENCE (S->C)
    ReportSpellAward = 0x6F, ///< MSG_REPORT_SPELL_AWARD (S->C)
    ReportSpellStart = 0x70, ///< MSG_REPORT_SPELL_START (S->C)
    ReportInventoryLoaded = 0x71, ///< MSG_REPORT_INVENTORY_LOADED (S->C)
    TryDrop = 0x72, ///< MSG_TRY_DROP (C->S)
    TryGet = 0x73, ///< MSG_TRY_GET (C->S)
    TryUse = 0x74, ///< MSG_TRY_USE (C->S)
    TryEquip = 0x75, ///< MSG_TRY_EQUIP (C->S)
    TryDequip = 0x76, ///< MSG_TRY_DEQUIP (C->S)
    TryTarget = 0x77, ///< MSG_TRY_TARGET (-)
    TryCreatureCommand = 0x78, ///< MSG_TRY_CREATURE_COMMAND (C->S)
    TrySpell = 0x79, ///< MSG_TRY_SPELL (C->S)
    TryAbility = 0x7A, ///< MSG_TRY_ABILITY (C->S)
    TryCollide = 0x7B, ///< MSG_TRY_COLLIDE (C->S)
    FxParticlefx = 0x7C, ///< MSG_FX_PARTICLEFX (S->C)
    FxPlasma = 0x7D, ///< MSG_FX_PLASMA (S->C)
    FxSummon = 0x7E, ///< MSG_FX_SUMMON (S->C)
    FxSummonCancel = 0x7F, ///< MSG_FX_SUMMON_CANCEL (S->C)
    FxShield = 0x80, ///< MSG_FX_SHIELD (S->C)
    FxBlueSparks = 0x81, ///< MSG_FX_BLUE_SPARKS (S->C)
    FxYellowSparks = 0x82, ///< MSG_FX_YELLOW_SPARKS (S->C)
    FxCyanSparks = 0x83, ///< MSG_FX_CYAN_SPARKS (S->C)
    FxVioletSparks = 0x84, ///< MSG_FX_VIOLET_SPARKS (S->C)
    FxExplosion = 0x85, ///< MSG_FX_EXPLOSION (S->C)
    FxLesserExplosion = 0x86, ///< MSG_FX_LESSER_EXPLOSION (S->C)
    FxCounterspellExplosion = 0x87, ///< MSG_FX_COUNTERSPELL_EXPLOSION (S->C)
    FxThinExplosion = 0x88, ///< MSG_FX_THIN_EXPLOSION (S->C)
    FxTeleport = 0x89, ///< MSG_FX_TELEPORT (S->C)
    FxSmokeBlast = 0x8A, ///< MSG_FX_SMOKE_BLAST (S->C)
    FxDamagePoof = 0x8B, ///< MSG_FX_DAMAGE_POOF (S->C)
    FxLightning = 0x8C, ///< MSG_FX_LIGHTNING (S->C)
    FxEnergyBolt = 0x8D, ///< MSG_FX_ENERGY_BOLT (S->C)
    FxChainLightningBolt = 0x8E, ///< MSG_FX_CHAIN_LIGHTNING_BOLT (S->C)
    FxDrainMana = 0x8F, ///< MSG_FX_DRAIN_MANA (S->C)
    FxCharm = 0x90, ///< MSG_FX_CHARM (S->C)
    FxGreaterHeal = 0x91, ///< MSG_FX_GREATER_HEAL (S->C)
    FxMagic = 0x92, ///< MSG_FX_MAGIC (-)
    FxSparkExplosion = 0x93, ///< MSG_FX_SPARK_EXPLOSION (S->C)
    FxDeathRay = 0x94, ///< MSG_FX_DEATH_RAY (S->C)
    FxSentryRay = 0x95, ///< MSG_FX_SENTRY_RAY (S->C)
    FxRicochet = 0x96, ///< MSG_FX_RICOCHET (S->C)
    FxJiggle = 0x97, ///< MSG_FX_JIGGLE (S->C)
    FxGreenBolt = 0x98, ///< MSG_FX_GREEN_BOLT (S->C)
    FxGreenExplosion = 0x99, ///< MSG_FX_GREEN_EXPLOSION (-)
    FxWhiteFlash = 0x9A, ///< MSG_FX_WHITE_FLASH (S->C)
    FxGeneratingMap = 0x9B, ///< MSG_FX_GENERATING_MAP (S->C)
    FxAssemblingMap = 0x9C, ///< MSG_FX_ASSEMBLING_MAP (S->C)
    FxPopulatingMap = 0x9D, ///< MSG_FX_POPULATING_MAP (S->C)
    FxDurationSpell = 0x9E, ///< MSG_FX_DURATION_SPELL (S->C)
    FxDeltazSpellStart = 0x9F, ///< MSG_FX_DELTAZ_SPELL_START (S->C)
    FxTurnUndead = 0xA0, ///< MSG_FX_TURN_UNDEAD (S->C)
    FxArrowTrap = 0xA1, ///< MSG_FX_ARROW_TRAP (S->C)
    FxVampirism = 0xA2, ///< MSG_FX_VAMPIRISM (S->C)
    FxManaBombCancel = 0xA3, ///< MSG_FX_MANA_BOMB_CANCEL (S->C)
    UpdateStream = 0xA4, ///< MSG_UPDATE_STREAM (S->C)
    NewAlias = 0xA5, ///< MSG_NEW_ALIAS (C->S)
    AudioEvent = 0xA6, ///< MSG_AUDIO_EVENT (S->C)
    AudioPlayerEvent = 0xA7, ///< MSG_AUDIO_PLAYER_EVENT (S->C)
    TextMessage = 0xA8, ///< MSG_TEXT_MESSAGE (S->C / C->S)
    Inform = 0xA9, ///< MSG_INFORM (S->C)
    Important = 0xAA, ///< MSG_IMPORTANT (S->C / C->S)
    ImportantAck = 0xAB, ///< MSG_IMPORTANT_ACK (S->C / C->S)
    Mouse = 0xAC, ///< MSG_MOUSE (C->S)
    IncomingClient = 0xAD, ///< MSG_INCOMING_CLIENT (C->S)
    OutgoingClient = 0xAE, ///< MSG_OUTGOING_CLIENT (S->C / C->S)
    GameSettings = 0xAF, ///< MSG_GAME_SETTINGS (S->C)
    GameSettings2 = 0xB0, ///< MSG_GAME_SETTINGS_2 (S->C)
    UpdateGuiGameSettings = 0xB1, ///< MSG_UPDATE_GUI_GAME_SETTINGS (S->C)
    DoorAngle = 0xB2, ///< MSG_DOOR_ANGLE (S->C)
    ObeliskCharge = 0xB3, ///< MSG_OBELISK_CHARGE (S->C)
    PentagramActivate = 0xB4, ///< MSG_PENTAGRAM_ACTIVATE (S->C)
    ClientPredictLinear = 0xB5, ///< MSG_CLIENT_PREDICT_LINEAR (S->C)
    RequestMap = 0xB6, ///< MSG_REQUEST_MAP (C->S)
    CancelMap = 0xB7, ///< MSG_CANCEL_MAP (C->S)
    MapSendStart = 0xB8, ///< MSG_MAP_SEND_START (S->C)
    MapSendPacket = 0xB9, ///< MSG_MAP_SEND_PACKET (S->C)
    MapSendAbort = 0xBA, ///< MSG_MAP_SEND_ABORT (S->C)
    ServerCmd = 0xBB, ///< MSG_SERVER_CMD (C->S)
    SysopPw = 0xBC, ///< MSG_SYSOP_PW (C->S)
    SysopResult = 0xBD, ///< MSG_SYSOP_RESULT (S->C)
    KeepAlive = 0xBE, ///< MSG_KEEP_ALIVE (C->S)
    ReceivedMap = 0xBF, ///< MSG_RECEIVED_MAP (C->S)
    ClientReady = 0xC0, ///< MSG_CLIENT_READY (C->S)
    RequestSavePlayer = 0xC1, ///< MSG_REQUEST_SAVE_PLAYER (C->S)
    XferMsg = 0xC2, ///< MSG_XFER_MSG (S->C / C->S)
    PlayerObj = 0xC3, ///< MSG_PLAYER_OBJ (S->C)
    TeamMsg = 0xC4, ///< MSG_TEAM_MSG (S->C / C->S)
    KickNotification = 0xC5, ///< MSG_KICK_NOTIFICATION (S->C)
    TimeoutNotification = 0xC6, ///< MSG_TIMEOUT_NOTIFICATION (S->C)
    ServerQuit = 0xC7, ///< MSG_SERVER_QUIT (S->C)
    ServerQuitAck = 0xC8, ///< MSG_SERVER_QUIT_ACK (C->S)
    Trade = 0xC9, ///< MSG_TRADE (S->C / C->S)
    ChatKill = 0xCA, ///< MSG_CHAT_KILL (S->C)
    MessagesKill = 0xCB, ///< MSG_MESSAGES_KILL (S->C)
    SeqImportant = 0xCC, ///< MSG_SEQ_IMPORTANT (S->C)
    ReportAbilityAward = 0xCD, ///< MSG_REPORT_ABILITY_AWARD (S->C)
    ReportAbilityState = 0xCE, ///< MSG_REPORT_ABILITY_STATE (S->C)
    ReportActiveAbilities = 0xCF, ///< MSG_REPORT_ACTIVE_ABILITIES (S->C)
    Dialog = 0xD0, ///< MSG_DIALOG (S->C / C->S)
    ReportGuideAward = 0xD1, ///< MSG_REPORT_GUIDE_AWARD (S->C)
    InterestingId = 0xD2, ///< MSG_INTERESTING_ID (S->C)
    TimerStatus = 0xD3, ///< MSG_TIMER_STATUS (S->C)
    RequestTimerStatus = 0xD4, ///< MSG_REQUEST_TIMER_STATUS (C->S)
    JournalMsg = 0xD5, ///< MSG_JOURNAL_MSG (S->C)
    ChapterEnd = 0xD6, ///< MSG_CHAPTER_END (S->C)
    ReportAllLatency = 0xD7, ///< MSG_REPORT_ALL_LATENCY (S->C)
    ReportFlagStatus = 0xD8, ///< MSG_REPORT_FLAG_STATUS (S->C)
    ReportBallStatus = 0xD9, ///< MSG_REPORT_BALL_STATUS (S->C)
    ReportObjectPoison = 0xDA, ///< MSG_REPORT_OBJECT_POISON (S->C)
    ReportMonitorCreature = 0xDB, ///< MSG_REPORT_MONITOR_CREATURE (S->C)
    ReportUnmonitorCreature = 0xDC, ///< MSG_REPORT_UNMONITOR_CREATURE (S->C)
    ReportTotalHealth = 0xDD, ///< MSG_REPORT_TOTAL_HEALTH (S->C)
    ReportTotalMana = 0xDE, ///< MSG_REPORT_TOTAL_MANA (S->C)
    ReportSpellStat = 0xDF, ///< MSG_REPORT_SPELL_STAT (S->C)
    ReportSecondaryWeapon = 0xE0, ///< MSG_REPORT_SECONDARY_WEAPON (S->C / C->S)
    ReportLastQuiver = 0xE1, ///< MSG_REPORT_LAST_QUIVER (S->C)
    InfoBookData = 0xE2, ///< MSG_INFO_BOOK_DATA (S->C / C->S)
    Social = 0xE3, ///< MSG_SOCIAL (C->S)
    FadeBegin = 0xE4, ///< MSG_FADE_BEGIN (S->C)
    MusicEvent = 0xE5, ///< MSG_MUSIC_EVENT (S->C)
    MusicPushEvent = 0xE6, ///< MSG_MUSIC_PUSH_EVENT (S->C)
    MusicPopEvent = 0xE7, ///< MSG_MUSIC_POP_EVENT (S->C)
    PlayerDied = 0xE8, ///< MSG_PLAYER_DIED (S->C)
    PlayerRespawn = 0xE9, ///< MSG_PLAYER_RESPAWN (S->C)
    ForgetDrawables = 0xEA, ///< MSG_FORGET_DRAWABLES (S->C)
    ResetAbilities = 0xEB, ///< MSG_RESET_ABILITIES (S->C)
    RateChange = 0xEC, ///< MSG_RATE_CHANGE (S->C)
    ReportCreatureCmd = 0xED, ///< MSG_REPORT_CREATURE_CMD (S->C)
    Vote = 0xEE, ///< MSG_VOTE (S->C / C->S)
    StatMultipliers = 0xEF, ///< MSG_STAT_MULTIPLIERS (S->C)
    Gauntlet = 0xF0, ///< MSG_GAUNTLET (S->C / C->S)
    InventoryFail = 0xF1, ///< MSG_INVENTORY_FAIL (C->S)
    /// noxcpp's own (S->C, only to a client that identified itself: lobby/extended.hpp): the
    /// important objects' positions at a substep between frames. u8 substep k (1..m-1), u8 m,
    /// u8 count, u16 the frame it follows (its low 16 bits), then count x {u16 net code, u16 x,
    /// u16 y}. A stock client never gets one.
    NoxcppSubstep = 0xF2,
    /// noxcpp's own map vote (both ways, identified clients only: server/map_vote.hpp): u8 sub,
    /// u16 the rest's length, the rest.
    NoxcppMapVote = 0xF3,
    /// noxcpp's own: a built-in NoX Core mode's messages (both ways, identified clients only;
    /// the registry's mode with this opcode, modes/: what they hold is the mode's). u8 sub,
    /// u16 the rest's length, the rest.
    NoxcppModeF4 = 0xF4,
    /// noxcpp's own: a built-in NoX Core mode's messages (both ways, identified clients only;
    /// the registry's mode with this opcode, modes/: what they hold is the mode's). u8 sub,
    /// u16 the rest's length, the rest.
    NoxcppModeF5 = 0xF5,
    /// noxcpp's own collaborative map editor session messages (both ways, identified clients
    /// only: mapedit/session_proto.hpp): the session, roles, ops, locks, presence, publish,
    /// script diagnostics and the live view. u8 sub, u16 the rest's length, the rest.
    NoxcppEdit = 0xF6,
    /// noxcpp's own: a built-in NoX Core mode's messages (both ways, identified clients only;
    /// the registry's mode with this opcode, modes/: what they hold is the mode's). u8 sub,
    /// u16 the rest's length, the rest.
    NoxcppCoop = 0xF7,
    /// noxcpp's own script mode UI messages (both ways, only with a client whose extended
    /// query named ClientFeature::ModeUi: modes/script ui_proto.hpp): the mode's UI script in
    /// blocks, its data and views, and what the UI sends back; a UI of API 2 and its mod's
    /// assets (subs 6-8: decoded pictures and sounds, NXZ-packed) only with a client that also
    /// named ClientFeature::ModeUi2; a script mode's weather (sub 9, nox.weather), its book
    /// (sub 10, configure's bookIsTheMod) and its things' draw scales (sub 11,
    /// nox.setDrawScale). u8 sub, u16 the rest's length, the rest.
    NoxcppModeUi = 0xF8,
};

struct OpInfo {
    std::string_view name; ///< the exe's name, e.g. "MSG_USE_MAP"; empty if the opcode has none
    std::string_view direction; ///< "S->C", "C->S", "S->C / C->S", "transport" or "-"
    /// Size as the 1.2b client decoder (CliNet_OnPacket) reads it: 0 = the client doesn't decode it,
    /// kVariableSize = depends on the contents (see framing.hpp).
    std::uint16_t serverToClient;
    /// Size as the 1.2b server decoder (SrvNet_OnPacket) reads it, same conventions.
    std::uint16_t clientToServer;
    /// Size of the full layout in the protocol spec (transport messages, and the longer form of
    /// direction-dependent ones); kVariableSize for variable ones.
    std::uint16_t layoutSize;
};

const OpInfo& opInfo(Op op) noexcept;
inline const OpInfo& opInfo(std::uint8_t op) noexcept { return opInfo(static_cast<Op>(op)); }

struct SubOpInfo {
    std::string_view name;
    std::uint16_t serverToClient; ///< 0 = not sent that way; kVariableSize = variable
    std::uint16_t clientToServer;
};

/// Sub-op of a family message (INFORM, XFER, TEAM, TRADE, DIALOG, VOTE, GAUNTLET), or nullptr
/// if `op` has no sub-ops or `sub` is unknown.
const SubOpInfo* subOpInfo(Op op, std::uint8_t sub) noexcept;

namespace msg {

/// 0x00 MSG_SERVER_CONNECT (transport). C->S connect request; server answers 0x01 (sent with header byte 0 = 0x1F)
struct ServerConnect {
    static constexpr Op kOp = Op::ServerConnect;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static ServerConnect read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ServerConnect&) const = default;
};

/// 0x01 MSG_SERVER_ACCEPT (transport)
struct ServerAccept {
    static constexpr Op kOp = Op::ServerAccept;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint32_t playerId{}; ///< +1 your player index; becomes header byte 0
    std::uint8_t xorKey{}; ///< +5 every later datagram is XORed with this (0 = off)

    /// Reads the fields; `r` is positioned after the opcode.
    static ServerAccept read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ServerAccept&) const = default;
};

/// 0x02 MSG_CODE2 (transport). S->C join refused (client id -18)
struct Code2 {
    static constexpr Op kOp = Op::Code2;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static Code2 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Code2&) const = default;
};

/// 0x03 MSG_CODE3 (transport). S->C refused (client id -12)
struct Code3 {
    static constexpr Op kOp = Op::Code3;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static Code3 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Code3&) const = default;
};

/// 0x04 MSG_CODE4 (transport). S->C already joined (client id -13)
struct Code4 {
    static constexpr Op kOp = Op::Code4;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static Code4 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Code4&) const = default;
};

/// 0x05 MSG_CODE5 (transport)
struct Code5 {
    static constexpr Op kOp = Op::Code5;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint32_t value{}; ///< +1 pre-join ping; answered with 0x07 echoing value

    /// Reads the fields; `r` is positioned after the opcode.
    static Code5 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Code5&) const = default;
};

/// 0x06 MSG_CLIENT_PING (transport)
struct ClientPing {
    static constexpr Op kOp = Op::ClientPing;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint32_t timeMs{}; ///< +1 ping; answered with 0x08 echoing timeMs

    /// Reads the fields; `r` is positioned after the opcode.
    static ClientPing read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ClientPing&) const = default;
};

/// 0x07 MSG_CODE7 (transport)
struct Code7 {
    static constexpr Op kOp = Op::Code7;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint32_t value{}; ///< +1 answer to 0x05; receiver computes link speed (MSG_SPEED)

    /// Reads the fields; `r` is positioned after the opcode.
    static Code7 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Code7&) const = default;
};

/// 0x08 MSG_CLIENT_PONG (transport)
struct ClientPong {
    static constexpr Op kOp = Op::ClientPong;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint32_t timeMs{}; ///< +1 echo of 0x06; sender computes round-trip (MSG_PING)

    /// Reads the fields; `r` is positioned after the opcode.
    static ClientPong read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ClientPong&) const = default;
};

/// 0x09 MSG_CODE9 (transport)
struct Code9 {
    static constexpr Op kOp = Op::Code9;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint32_t timeMs{}; ///< +1 timing sample; last 5 deltas averaged

    /// Reads the fields; `r` is positioned after the opcode.
    static Code9 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Code9&) const = default;
};

/// 0x0A MSG_CLIENT_CLOSE (transport). client disconnect; server answers 0x22
struct ClientClose {
    static constexpr Op kOp = Op::ClientClose;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static ClientClose read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ClientClose&) const = default;
};

/// 0x0B MSG_SERVER_CLOSE (transport). server disconnect; client answers 0x21
struct ServerClose {
    static constexpr Op kOp = Op::ServerClose;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static ServerClose read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ServerClose&) const = default;
};

/// 0x0C MSG_SERVER_DISCOVER (transport). LAN/lobby discovery broadcast; handled outside the connection
struct ServerDiscover {
    static constexpr Op kOp = Op::ServerDiscover;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 10;

    std::array<std::uint8_t, 5> unk{}; ///< +1
    std::uint32_t token{}; ///< +6 client tick count; echoed in SERVER_INFO +42

    /// Reads the fields; `r` is positioned after the opcode.
    static ServerDiscover read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ServerDiscover&) const = default;
};

/// 0x0D MSG_SERVER_INFO (transport). 72 + strlen(name) + 1 bytes (from server_makeServerInfoPacket)
struct ServerInfo {
    static constexpr Op kOp = Op::ServerInfo;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = kVariableSize;

    std::uint8_t players{}; ///< +1 current players (host excluded on a dedicated server)
    std::uint8_t maxPlayers{}; ///< +2
    std::uint8_t settings101lo{}; ///< +3 the server settings & 0xF
    std::uint8_t settings101hi{}; ///< +4 the server settings >> 4
    std::array<std::uint8_t, 3> gameData44{}; ///< +5 low 3 bytes of the game options (4th byte overwritten by the map name)
    std::array<std::uint8_t, 9> mapName{}; ///< +8 ASCII, NUL-terminated
    std::uint8_t videoFlags{}; ///< +17 the server settings | video mode id
    std::uint8_t serverFlags{}; ///< +18 1 no warrior, 2 no wizard, 4 no conjurer, 0x10 closed, 0x20 password
    std::uint8_t closed{}; ///< +19 serverFlags & 0x10
    std::array<std::uint8_t, 2> pad{}; ///< +20
    std::uint32_t settingsVersion{}; ///< +22 the server settings
    std::uint32_t gameFlags{}; ///< +26 quest: |0x1000, &~0x80
    std::uint32_t gameData48{}; ///< +30 the game options
    std::uint16_t pingMin{}; ///< +34 restricted ping min
    std::uint16_t pingMax{}; ///< +36 restricted ping max
    std::uint32_t settings44{}; ///< +38 the server settings
    std::uint32_t token{}; ///< +42 echo of DISCOVER token (client uses it for ping)
    std::array<std::uint8_t, 20> allowedSpells{}; ///< +46 the game options spell bitmap
    std::uint16_t questStage{}; ///< +66 quest games only
    std::array<std::uint8_t, 2> pad2{}; ///< +68
    std::vector<std::uint8_t> serverName{}; ///< +70 ASCII, NUL-terminated

    /// Reads the fields; `r` is positioned after the opcode.
    static ServerInfo read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ServerInfo&) const = default;
};

/// 0x0E MSG_SERVER_TRY_JOIN (transport). 98 bytes; sent from the lobby socket with header 00 00; answered with 0x14, 0x15, 0x0F, 0x13 or, when the server restricts ping, a series of 0x10 pings first
struct ServerTryJoin {
    static constexpr Op kOp = Op::ServerTryJoin;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 98;

    std::uint8_t unk1{}; ///< +1 not initialised by the 1.2b client
    std::array<std::uint8_t, 50> playerName{}; ///< +2 wchar[25]
    std::uint8_t playerClass{}; ///< +52 rejected with error 7 if the server bans this class
    std::uint8_t unk53{}; ///< +53 a client global
    std::array<std::uint8_t, 24> serial{}; ///< +54 ASCII CD key/serial; error 12 if another player with the same noSoloMaps has it
    std::uint32_t version{}; ///< +78 0x1039A; anything else gets error 13
    std::uint32_t team{}; ///< +82 requested team (0 = none)
    std::array<std::uint8_t, 10> account{}; ///< +86 ASCII WOL account name
    std::uint8_t noSoloMaps{}; ///< +96 1 when the client has no solo maps installed
    std::uint8_t unk97{}; ///< +97 not initialised by the 1.2b client

    /// Reads the fields; `r` is positioned after the opcode.
    static ServerTryJoin read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ServerTryJoin&) const = default;
};

/// 0x0F MSG_PASSWORD_REQUIRED (transport). S->C: password required; client sends 0x11
struct PasswordRequired {
    static constexpr Op kOp = Op::PasswordRequired;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static PasswordRequired read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const PasswordRequired&) const = default;
};

/// 0x10 MSG_SERVER_PING (transport). server ping (lobby)
struct ServerPing {
    static constexpr Op kOp = Op::ServerPing;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint8_t index{}; ///< +1
    std::uint32_t timeMs{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode.
    static ServerPing read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ServerPing&) const = default;
};

/// 0x11 MSG_SERVER_PASSWORD (transport)
struct ServerPassword {
    static constexpr Op kOp = Op::ServerPassword;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 20;

    std::uint8_t unk0{}; ///< +1
    std::array<std::uint8_t, 18> password{}; ///< +2 wchar[9]

    /// Reads the fields; `r` is positioned after the opcode.
    static ServerPassword read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ServerPassword&) const = default;
};

/// 0x12 MSG_SERVER_PONG (transport)
struct ServerPong {
    static constexpr Op kOp = Op::ServerPong;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint8_t index{}; ///< +1
    std::uint32_t timeMs{}; ///< +2 echo of 0x10

    /// Reads the fields; `r` is positioned after the opcode.
    static ServerPong read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ServerPong&) const = default;
};

/// 0x13 MSG_SERVER_ERROR (transport)
struct ServerError {
    static constexpr Op kOp = Op::ServerError;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    std::uint8_t error{}; ///< +1 0 low ping, 1 high ping, 2 low level, 3 high level, 4 closed, 5 banned, 6 wrong password, 7 illegal class, 8 timeout, 9 find failed, 10 need refresh, 11 full, 12 duplicate serial, 13 wrong version

    /// Reads the fields; `r` is positioned after the opcode.
    static ServerError read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ServerError&) const = default;
};

/// 0x14 MSG_SERVER_JOIN_OK (transport). S->C: join OK; client continues with 0x00 connect
struct ServerJoinOk {
    static constexpr Op kOp = Op::ServerJoinOk;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static ServerJoinOk read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ServerJoinOk&) const = default;
};

/// 0x15 MSG_SERVER_JOIN_FAIL (transport). S->C: join failed
struct ServerJoinFail {
    static constexpr Op kOp = Op::ServerJoinFail;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static ServerJoinFail read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ServerJoinFail&) const = default;
};

/// 0x16 MSG_CODE22 (transport). unused
struct Code22 {
    static constexpr Op kOp = Op::Code22;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static Code22 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Code22&) const = default;
};

/// 0x17 MSG_CODE23 (transport). unused
struct Code23 {
    static constexpr Op kOp = Op::Code23;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static Code23 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Code23&) const = default;
};

/// 0x18 MSG_CODE24 (transport). unused
struct Code24 {
    static constexpr Op kOp = Op::Code24;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static Code24 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Code24&) const = default;
};

/// 0x19 MSG_CODE25 (transport). unused
struct Code25 {
    static constexpr Op kOp = Op::Code25;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static Code25 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Code25&) const = default;
};

/// 0x1A MSG_CODE26 (transport). unused
struct Code26 {
    static constexpr Op kOp = Op::Code26;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static Code26 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Code26&) const = default;
};

/// 0x1B MSG_CODE27 (transport). unused
struct Code27 {
    static constexpr Op kOp = Op::Code27;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static Code27 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Code27&) const = default;
};

/// 0x1C MSG_CODE28 (transport). unused
struct Code28 {
    static constexpr Op kOp = Op::Code28;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static Code28 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Code28&) const = default;
};

/// 0x1D MSG_CODE29 (transport). unused
struct Code29 {
    static constexpr Op kOp = Op::Code29;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static Code29 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Code29&) const = default;
};

/// 0x1E MSG_CODE30 (transport). unused
struct Code30 {
    static constexpr Op kOp = Op::Code30;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static Code30 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Code30&) const = default;
};

/// 0x1F MSG_ACCEPTED (transport)
struct Accepted {
    static constexpr Op kOp = Op::Accepted;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    std::uint8_t seq{}; ///< +1 reliable sequence resync; also used as header byte 0 (id 31) before an id is assigned

    /// Reads the fields; `r` is positioned after the opcode.
    static Accepted read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Accepted&) const = default;
};

/// 0x20 MSG_CLIENT_ACCEPT (transport). 154 bytes; client -> server after 0x01. The first 97 payload bytes are the PlayerInfo block (same as NEW_PLAYER). Parsed by Plr_CreateFromJoinData.
struct ClientAccept {
    static constexpr Op kOp = Op::ClientAccept;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 154;

    std::array<std::uint8_t, 50> playerName{}; ///< +1 wchar[25]
    std::uint32_t speed{}; ///< +51 the player info
    std::uint32_t strength{}; ///< +55 +2239
    std::uint32_t maxMana{}; ///< +59 +2243
    std::uint32_t maxHealth{}; ///< +63 +2247
    std::uint8_t classId{}; ///< +67 0 warrior, 1 wizard, 2 conjurer; rejected if banned by the game options class mask
    std::uint8_t isFemale{}; ///< +68
    std::array<std::uint8_t, 15> colors{}; ///< +69 5 x rgb: hair, skin, mustache, goatee, beard
    std::array<std::uint8_t, 5> clothing{}; ///< +84 pants, shirt1, shirt2, shoes1, shoes2 colour indexes
    std::uint8_t unk88{}; ///< +89 the player info
    std::array<std::uint8_t, 8> nameSuffix{}; ///< +90 wchar[4]
    std::uint32_t screenWidth{}; ///< +98 half stored as Player view width
    std::uint32_t screenHeight{}; ///< +102
    std::array<std::uint8_t, 22> serial{}; ///< +106 ASCII CD key/serial
    std::uint8_t unk127{}; ///< +128
    std::array<std::uint8_t, 10> account{}; ///< +129 ASCII WOL account name -> the player record
    std::uint32_t joinToken{}; ///< +139 the player record; echoed back in JOIN_DATA (0x2C)
    std::array<std::uint8_t, 10> account2{}; ///< +143 ASCII -> the player record
    std::uint8_t flags{}; ///< +153 bit 7 = quest client; must match the server's quest mode

    /// Reads the fields; `r` is positioned after the opcode.
    static ClientAccept read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ClientAccept&) const = default;
};

/// 0x21 MSG_SERVER_CLOSE_ACK (S->C). server-close ack (also injected locally)
struct ServerCloseAck {
    static constexpr Op kOp = Op::ServerCloseAck;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static ServerCloseAck read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ServerCloseAck&) const = default;
};

/// 0x22 MSG_CLIENT_CLOSE_ACK (transport). client-close ack
struct ClientCloseAck {
    static constexpr Op kOp = Op::ClientCloseAck;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static ClientCloseAck read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ClientCloseAck&) const = default;
};

/// 0x23 MSG_SPEED (transport)
struct Speed {
    static constexpr Op kOp = Op::Speed;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::int32_t speed{}; ///< +1 256000 / ms, or -1; injected into the game queue by the transport

    /// Reads the fields; `r` is positioned after the opcode.
    static Speed read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Speed&) const = default;
};

/// 0x24 MSG_PING (transport)
struct Ping {
    static constexpr Op kOp = Op::Ping;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint32_t pingMs{}; ///< +1 injected into the game queue by the transport

    /// Reads the fields; `r` is positioned after the opcode.
    static Ping read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Ping&) const = default;
};

/// 0x25 MSG_CODE37 (transport). injected: ping received
struct Code37 {
    static constexpr Op kOp = Op::Code37;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static Code37 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Code37&) const = default;
};

/// 0x26 MSG_CODE38 (C->S)
struct Code38 {
    static constexpr Op kOp = Op::Code38;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    std::uint8_t seq{}; ///< +1 reliable packet acknowledged (injected by the transport)

    /// Reads the fields; `r` is positioned after the opcode.
    static Code38 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Code38&) const = default;
};

/// 0x27 MSG_TIMESTAMP (S->C)
struct Timestamp {
    static constexpr Op kOp = Op::Timestamp;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t frame16{}; ///< +1 low 16 bits of server frame; top 2 bits are a wrap counter

    /// Reads the fields; `r` is positioned after the opcode.
    static Timestamp read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Timestamp&) const = default;
};

/// 0x28 MSG_FULL_TIMESTAMP (S->C)
struct FullTimestamp {
    static constexpr Op kOp = Op::FullTimestamp;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint32_t frame{}; ///< +1 full server frame

    /// Reads the fields; `r` is positioned after the opcode.
    static FullTimestamp read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FullTimestamp&) const = default;
};

/// 0x29 MSG_NEED_TIMESTAMP (C->S). client asks the server for a full timestamp; no payload
struct NeedTimestamp {
    static constexpr Op kOp = Op::NeedTimestamp;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static NeedTimestamp read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const NeedTimestamp&) const = default;
};

/// 0x2A MSG_SIMULATED_TIMESTAMP (S->C)
struct SimulatedTimestamp {
    static constexpr Op kOp = Op::SimulatedTimestamp;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint32_t frame{}; ///< +1 accepted only if not behind the last full timestamp

    /// Reads the fields; `r` is positioned after the opcode.
    static SimulatedTimestamp read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const SimulatedTimestamp&) const = default;
};

/// 0x2B MSG_USE_MAP (S->C)
struct UseMap {
    static constexpr Op kOp = Op::UseMap;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 41;

    std::array<std::uint8_t, 32> mapName{}; ///< +1 NUL-padded map name
    std::uint32_t crc{}; ///< +33 map CRC
    std::uint32_t frame{}; ///< +37 server frame of the map change

    /// Reads the fields; `r` is positioned after the opcode.
    static UseMap read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const UseMap&) const = default;
};

/// 0x2C MSG_JOIN_DATA (S->C)
struct JoinData {
    static constexpr Op kOp = Op::JoinData;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 7;

    std::uint16_t playerNetCode{}; ///< +1 your player netcode
    std::uint32_t joinToken{}; ///< +3 the player record, echo of CLIENT_ACCEPT +139

    /// Reads the fields; `r` is positioned after the opcode.
    static JoinData read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const JoinData&) const = default;
};

/// 0x2D MSG_NEW_PLAYER (S->C)
struct NewPlayer {
    static constexpr Op kOp = Op::NewPlayer;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 129;

    std::uint16_t netCode{}; ///< +1 player netcode
    std::array<std::uint8_t, 97> playerInfo{}; ///< +3 PlayerInfo block: wchar name[25], u32 speed, strength, maxMana, maxHealth, u8 class, isFemale, 5 rgb colors, 5 clothing colors, u8, wchar nameSuffix[4]
    std::int16_t lessons{}; ///< +100 score/lessons
    std::int16_t field2140{}; ///< +102
    std::uint32_t armorEquipFlags{}; ///< +104 equipped armor class bits
    std::uint32_t weaponEquipFlags{}; ///< +108 equipped weapon class bits
    std::uint32_t statusFlags{}; ///< +112 the player record status bits
    std::uint8_t field2152{}; ///< +116
    std::uint8_t field2156{}; ///< +117
    std::uint8_t pad{}; ///< +118
    std::array<std::uint8_t, 10> accountName{}; ///< +119 the player record string

    /// Reads the fields; `r` is positioned after the opcode.
    static NewPlayer read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const NewPlayer&) const = default;
};

/// 0x2E MSG_PLAYER_QUIT (S->C)
struct PlayerQuit {
    static constexpr Op kOp = Op::PlayerQuit;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t netCode{}; ///< +1 player who left

    /// Reads the fields; `r` is positioned after the opcode.
    static PlayerQuit read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const PlayerQuit&) const = default;
};

/// 0x2F MSG_SIMPLE_OBJ (S->C). creates/updates a simple drawable
struct SimpleObj {
    static constexpr Op kOp = Op::SimpleObj;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 9;

    std::uint16_t netCode{}; ///< +1 object netcode
    std::uint16_t typeId{}; ///< +3 thing type
    std::uint16_t x{}; ///< +5
    std::uint16_t y{}; ///< +7

    /// Reads the fields; `r` is positioned after the opcode.
    static SimpleObj read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const SimpleObj&) const = default;
};

/// 0x30 MSG_COMPLEX_OBJ (S->C). netCode 0 and typeId 0 = camera position only (x,y)
struct ComplexObj {
    static constexpr Op kOp = Op::ComplexObj;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 11;

    std::uint16_t netCode{}; ///< +1
    std::uint16_t typeId{}; ///< +3
    std::uint16_t x{}; ///< +5
    std::uint16_t y{}; ///< +7
    std::uint8_t animState{}; ///< +9 bits 0-3 animation index, bits 4-6 direction (values >3 shift by 1)
    std::uint8_t frame{}; ///< +10 animation frame

    /// Reads the fields; `r` is positioned after the opcode.
    static ComplexObj read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ComplexObj&) const = default;
};

/// 0x31 MSG_DESTROY_OBJECT (S->C). sent only to players whose bit is set in the object's "client knows this object" mask. Opcode = ((the object's status byte) >> 6) | 0x31, so objects with status bit 0x80 go out as 0x33 instead
struct DestroyObject {
    static constexpr Op kOp = Op::DestroyObject;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t netCode{}; ///< +1 drawable to destroy

    /// Reads the fields; `r` is positioned after the opcode.
    static DestroyObject read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const DestroyObject&) const = default;
};

/// 0x32 MSG_OBJECT_OUT_OF_SIGHT (S->C)
struct ObjectOutOfSight {
    static constexpr Op kOp = Op::ObjectOutOfSight;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t netCode{}; ///< +1 drawable left view (deleted unless it is a persistent anim)

    /// Reads the fields; `r` is positioned after the opcode.
    static ObjectOutOfSight read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ObjectOutOfSight&) const = default;
};

/// 0x33 MSG_OBJECT_IN_SHADOWS (S->C)
struct ObjectInShadows {
    static constexpr Op kOp = Op::ObjectInShadows;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t netCode{}; ///< +1 drawable is in shadow: fades out

    /// Reads the fields; `r` is positioned after the opcode.
    static ObjectInShadows read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ObjectInShadows&) const = default;
};

/// 0x34 MSG_OBJECT_FRIEND_ADD (S->C)
struct ObjectFriendAdd {
    static constexpr Op kOp = Op::ObjectFriendAdd;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t netCode{}; ///< +1 add to friends list (ally marker)

    /// Reads the fields; `r` is positioned after the opcode.
    static ObjectFriendAdd read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ObjectFriendAdd&) const = default;
};

/// 0x35 MSG_OBJECT_FRIEND_REMOVE (S->C)
struct ObjectFriendRemove {
    static constexpr Op kOp = Op::ObjectFriendRemove;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t netCode{}; ///< +1 remove from friends list

    /// Reads the fields; `r` is positioned after the opcode.
    static ObjectFriendRemove read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ObjectFriendRemove&) const = default;
};

/// 0x36 MSG_RESET_FRIENDS (S->C). clears the friends list; no payload
struct ResetFriends {
    static constexpr Op kOp = Op::ResetFriends;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static ResetFriends read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ResetFriends&) const = default;
};

/// 0x37 MSG_ENABLE_OBJECT (S->C)
struct EnableObject {
    static constexpr Op kOp = Op::EnableObject;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t netCode{}; ///< +1 set drawable flag 0x1000000 (enabled)

    /// Reads the fields; `r` is positioned after the opcode.
    static EnableObject read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const EnableObject&) const = default;
};

/// 0x38 MSG_DISABLE_OBJECT (S->C)
struct DisableObject {
    static constexpr Op kOp = Op::DisableObject;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t netCode{}; ///< +1 clear enabled flag

    /// Reads the fields; `r` is positioned after the opcode.
    static DisableObject read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const DisableObject&) const = default;
};

/// 0x39 MSG_DRAW_FRAME (S->C)
struct DrawFrame {
    static constexpr Op kOp = Op::DrawFrame;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t netCode{}; ///< +1
    std::uint8_t frame{}; ///< +3 animation frame to show

    /// Reads the fields; `r` is positioned after the opcode.
    static DrawFrame read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const DrawFrame&) const = default;
};

/// 0x3A MSG_DESTROY_WALL (S->C)
struct DestroyWall {
    static constexpr Op kOp = Op::DestroyWall;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t wallId{}; ///< +1 breakable wall index destroyed

    /// Reads the fields; `r` is positioned after the opcode.
    static DestroyWall read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const DestroyWall&) const = default;
};

/// 0x3B MSG_OPEN_WALL (S->C)
struct OpenWall {
    static constexpr Op kOp = Op::OpenWall;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t wallId{}; ///< +1 secret wall starts opening

    /// Reads the fields; `r` is positioned after the opcode.
    static OpenWall read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const OpenWall&) const = default;
};

/// 0x3C MSG_CLOSE_WALL (S->C)
struct CloseWall {
    static constexpr Op kOp = Op::CloseWall;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t wallId{}; ///< +1 secret wall starts closing

    /// Reads the fields; `r` is positioned after the opcode.
    static CloseWall read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const CloseWall&) const = default;
};

/// 0x3D MSG_CHANGE_OR_ADD_WALL_MAGIC (S->C). creates the wall if missing (Wall spell)
struct ChangeOrAddWallMagic {
    static constexpr Op kOp = Op::ChangeOrAddWallMagic;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint8_t tile{}; ///< +1 wall tile/def index
    std::uint8_t dir{}; ///< +2 wall direction
    std::uint8_t variation{}; ///< +3
    std::uint8_t gridX{}; ///< +4
    std::uint8_t gridY{}; ///< +5

    /// Reads the fields; `r` is positioned after the opcode.
    static ChangeOrAddWallMagic read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ChangeOrAddWallMagic&) const = default;
};

/// 0x3E MSG_REMOVE_WALL_MAGIC (S->C). removes a magic wall
struct RemoveWallMagic {
    static constexpr Op kOp = Op::RemoveWallMagic;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint8_t gridX{}; ///< +1
    std::uint8_t gridY{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode.
    static RemoveWallMagic read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const RemoveWallMagic&) const = default;
};

/// 0x3F MSG_PLAYER_INPUT (C->S)
struct PlayerInput {
    static constexpr Op kOp = Op::PlayerInput;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = kVariableSize;

    std::uint8_t size{}; ///< +1 bytes of entries that follow
    std::vector<std::uint8_t> entries{}; ///< +2 see player_input.hpp

    /// Reads the fields; `r` is positioned after the opcode.
    static PlayerInput read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const PlayerInput&) const = default;
};

/// 0x40 MSG_PLAYER_SET_WAYPOINT (C->S). sets the player custom waypoint (map marker)
struct PlayerSetWaypoint {
    static constexpr Op kOp = Op::PlayerSetWaypoint;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 7;

    std::uint16_t unused{}; ///< +1
    std::uint16_t x{}; ///< +3
    std::uint16_t y{}; ///< +5

    /// Reads the fields; `r` is positioned after the opcode.
    static PlayerSetWaypoint read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const PlayerSetWaypoint&) const = default;
};

/// 0x41 MSG_REPORT_HEALTH (S->C)
struct ReportHealth {
    static constexpr Op kOp = Op::ReportHealth;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t netCode{}; ///< +1 ally unit
    std::uint8_t healthHalf{}; ///< +3 health / 2 (client multiplies by 2)

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportHealth read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportHealth&) const = default;
};

/// 0x42 MSG_REPORT_HEALTH_DELTA (S->C)
struct ReportHealthDelta {
    static constexpr Op kOp = Op::ReportHealthDelta;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint16_t netCode{}; ///< +1
    std::int16_t delta{}; ///< +3 floating damage/heal number

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportHealthDelta read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportHealthDelta&) const = default;
};

/// 0x43 MSG_REPORT_PLAYER_HEALTH (S->C)
struct ReportPlayerHealth {
    static constexpr Op kOp = Op::ReportPlayerHealth;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::int16_t health{}; ///< +1 your current health

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportPlayerHealth read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportPlayerHealth&) const = default;
};

/// 0x44 MSG_REPORT_ITEM_HEALTH (S->C)
struct ReportItemHealth {
    static constexpr Op kOp = Op::ReportItemHealth;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 7;

    std::uint16_t netCode{}; ///< +1 inventory item
    std::uint16_t health{}; ///< +3
    std::uint16_t maxHealth{}; ///< +5

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportItemHealth read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportItemHealth&) const = default;
};

/// 0x45 MSG_REPORT_MANA (S->C)
struct ReportMana {
    static constexpr Op kOp = Op::ReportMana;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint16_t netCode{}; ///< +1 must be your player
    std::uint16_t mana{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportMana read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportMana&) const = default;
};

/// 0x46 MSG_REPORT_POISON (-). name only; no handler in either 1.2b decoder
struct ReportPoison {
    static constexpr Op kOp = Op::ReportPoison;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportPoison read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportPoison&) const = default;
};

/// 0x47 MSG_REPORT_STAMINA (S->C)
struct ReportStamina {
    static constexpr Op kOp = Op::ReportStamina;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    std::uint8_t stamina{}; ///< +1

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportStamina read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportStamina&) const = default;
};

/// 0x48 MSG_REPORT_STATS (S->C)
struct ReportStats {
    static constexpr Op kOp = Op::ReportStats;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 14;

    std::uint16_t netCode{}; ///< +1 must be your player
    std::uint16_t maxHealth{}; ///< +3
    std::uint16_t maxMana{}; ///< +5
    std::uint16_t carryCapacity{}; ///< +7 the player record
    std::uint16_t speed{}; ///< +9
    std::uint16_t strength{}; ///< +11
    std::uint8_t level{}; ///< +13

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportStats read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportStats&) const = default;
};

/// 0x49 MSG_REPORT_ARMOR_VALUE (S->C)
struct ReportArmorValue {
    static constexpr Op kOp = Op::ReportArmorValue;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    float armorValue{}; ///< +1 damage fraction absorbed; the stats screen shows round(value x 1000)

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportArmorValue read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportArmorValue&) const = default;
};

/// 0x4A MSG_REPORT_GOLD (S->C)
struct ReportGold {
    static constexpr Op kOp = Op::ReportGold;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint32_t gold{}; ///< +1

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportGold read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportGold&) const = default;
};

/// 0x4B MSG_REPORT_PICKUP (S->C)
struct ReportPickup {
    static constexpr Op kOp = Op::ReportPickup;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint16_t netCode{}; ///< +1 item picked up
    std::uint16_t typeId{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportPickup read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportPickup&) const = default;
};

/// 0x4C MSG_REPORT_MODIFIABLE_PICKUP (S->C)
struct ReportModifiablePickup {
    static constexpr Op kOp = Op::ReportModifiablePickup;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 9;

    std::uint16_t netCode{}; ///< +1
    std::uint16_t typeId{}; ///< +3
    std::uint8_t effectiveness{}; ///< +5 modifier id (modifier.bin)
    std::uint8_t material{}; ///< +6 modifier id
    std::uint8_t enchant1{}; ///< +7 modifier id
    std::uint8_t enchant2{}; ///< +8 modifier id

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportModifiablePickup read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportModifiablePickup&) const = default;
};

/// 0x4D MSG_REPORT_DROP (S->C)
struct ReportDrop {
    static constexpr Op kOp = Op::ReportDrop;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint16_t netCode{}; ///< +1 item dropped
    std::uint16_t typeId{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportDrop read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportDrop&) const = default;
};

/// 0x4E MSG_REPORT_LESSON (S->C)
struct ReportLesson {
    static constexpr Op kOp = Op::ReportLesson;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 11;

    std::uint16_t netCode{}; ///< +1 player
    std::uint32_t lessons{}; ///< +3 score
    std::uint32_t kills{}; ///< +7 compared to gamedata limit (elimination mode)

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportLesson read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportLesson&) const = default;
};

/// 0x4F MSG_REPORT_MUNDANE_ARMOR_EQUIP (S->C)
struct ReportMundaneArmorEquip {
    static constexpr Op kOp = Op::ReportMundaneArmorEquip;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 7;

    std::uint16_t netCodeFlags{}; ///< +1 bit15 = applies to the local player list; bits 0-14 = player netcode
    std::uint32_t classFlags{}; ///< +3 armor class bit(s) now equipped

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportMundaneArmorEquip read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportMundaneArmorEquip&) const = default;
};

/// 0x50 MSG_REPORT_MUNDANE_WEAPON_EQUIP (S->C)
struct ReportMundaneWeaponEquip {
    static constexpr Op kOp = Op::ReportMundaneWeaponEquip;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 7;

    std::uint16_t netCodeFlags{}; ///< +1 bit15 = applies to the local player list; bits 0-14 = player netcode
    std::uint32_t classFlags{}; ///< +3 weapon class bit(s) now equipped

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportMundaneWeaponEquip read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportMundaneWeaponEquip&) const = default;
};

/// 0x51 MSG_REPORT_MODIFIABLE_WEAPON_EQUIP (S->C)
struct ReportModifiableWeaponEquip {
    static constexpr Op kOp = Op::ReportModifiableWeaponEquip;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 11;

    std::uint16_t netCodeFlags{}; ///< +1 bit15 = local; bits 0-14 = netcode
    std::uint32_t classFlags{}; ///< +3 weapon class bits
    std::uint8_t effectiveness{}; ///< +7 modifier id
    std::uint8_t material{}; ///< +8 modifier id
    std::uint8_t enchant1{}; ///< +9 modifier id
    std::uint8_t enchant2{}; ///< +10 modifier id

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportModifiableWeaponEquip read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportModifiableWeaponEquip&) const = default;
};

/// 0x52 MSG_REPORT_MODIFIABLE_ARMOR_EQUIP (S->C)
struct ReportModifiableArmorEquip {
    static constexpr Op kOp = Op::ReportModifiableArmorEquip;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 11;

    std::uint16_t netCodeFlags{}; ///< +1 bit15 = local; bits 0-14 = netcode
    std::uint32_t classFlags{}; ///< +3 armor class bits
    std::uint8_t effectiveness{}; ///< +7 modifier id
    std::uint8_t material{}; ///< +8 modifier id
    std::uint8_t enchant1{}; ///< +9 modifier id
    std::uint8_t enchant2{}; ///< +10 modifier id

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportModifiableArmorEquip read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportModifiableArmorEquip&) const = default;
};

/// 0x53 MSG_REPORT_ARMOR_DEQUIP (S->C)
struct ReportArmorDequip {
    static constexpr Op kOp = Op::ReportArmorDequip;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 7;

    std::uint16_t netCode{}; ///< +1
    std::uint32_t classFlags{}; ///< +3 armor class bits removed

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportArmorDequip read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportArmorDequip&) const = default;
};

/// 0x54 MSG_REPORT_WEAPON_DEQUIP (S->C)
struct ReportWeaponDequip {
    static constexpr Op kOp = Op::ReportWeaponDequip;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 7;

    std::uint16_t netCode{}; ///< +1
    std::uint32_t classFlags{}; ///< +3 weapon class bits removed

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportWeaponDequip read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportWeaponDequip&) const = default;
};

/// 0x55 MSG_REPORT_TREASURE_COUNT (S->C)
struct ReportTreasureCount {
    static constexpr Op kOp = Op::ReportTreasureCount;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 7;

    std::uint16_t netCode{}; ///< +1 player
    std::uint16_t treasures{}; ///< +3 collected
    std::uint16_t needed{}; ///< +5 to win (Treasure Hunt)

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportTreasureCount read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportTreasureCount&) const = default;
};

/// 0x56 MSG_REPORT_FLAG_BALL_WINNER (S->C)
struct ReportFlagBallWinner {
    static constexpr Op kOp = Op::ReportFlagBallWinner;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 8;

    std::uint16_t teamId{}; ///< +1 winning team (0 = none)
    std::uint8_t byTimeLimit{}; ///< +3 1 = time limit reached
    std::uint32_t frame{}; ///< +4 ignored if not newer than the last map change

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportFlagBallWinner read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportFlagBallWinner&) const = default;
};

/// 0x57 MSG_REPORT_FLAG_WINNER (S->C)
struct ReportFlagWinner {
    static constexpr Op kOp = Op::ReportFlagWinner;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 8;

    std::uint16_t teamId{}; ///< +1 winning team (0 = tie)
    std::uint8_t byTimeLimit{}; ///< +3
    std::uint32_t frame{}; ///< +4

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportFlagWinner read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportFlagWinner&) const = default;
};

/// 0x58 MSG_REPORT_DEATHMATCH_WINNER (S->C)
struct ReportDeathmatchWinner {
    static constexpr Op kOp = Op::ReportDeathmatchWinner;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 8;

    std::uint16_t netCode{}; ///< +1 winning player (0 = tie)
    std::uint8_t byTimeLimit{}; ///< +3
    std::uint32_t frame{}; ///< +4

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportDeathmatchWinner read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportDeathmatchWinner&) const = default;
};

/// 0x59 MSG_REPORT_DEATHMATCH_TEAM_WINNER (S->C)
struct ReportDeathmatchTeamWinner {
    static constexpr Op kOp = Op::ReportDeathmatchTeamWinner;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 8;

    std::uint16_t teamId{}; ///< +1 winning team (0 = tie)
    std::uint8_t byTimeLimit{}; ///< +3
    std::uint32_t frame{}; ///< +4

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportDeathmatchTeamWinner read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportDeathmatchTeamWinner&) const = default;
};

/// 0x5A MSG_REPORT_ENCHANTMENT (S->C)
struct ReportEnchantment {
    static constexpr Op kOp = Op::ReportEnchantment;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 7;

    std::uint16_t netCode{}; ///< +1
    std::uint32_t buffs{}; ///< +3 enchantment bit set (bit n = ENCHANT n)

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportEnchantment read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportEnchantment&) const = default;
};

/// 0x5B MSG_REPORT_ITEM_ENCHANTMENT (S->C)
struct ReportItemEnchantment {
    static constexpr Op kOp = Op::ReportItemEnchantment;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    std::uint8_t itemBuffs{}; ///< +1 local player item-enchant bits

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportItemEnchantment read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportItemEnchantment&) const = default;
};

/// 0x5C MSG_REPORT_LIGHT_COLOR (S->C)
struct ReportLightColor {
    static constexpr Op kOp = Op::ReportLightColor;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint16_t netCode{}; ///< +1
    std::uint8_t r{}; ///< +3
    std::uint8_t g{}; ///< +4
    std::uint8_t b{}; ///< +5

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportLightColor read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportLightColor&) const = default;
};

/// 0x5D MSG_REPORT_LIGHT_INTENSITY (S->C)
struct ReportLightIntensity {
    static constexpr Op kOp = Op::ReportLightIntensity;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 7;

    std::uint16_t netCode{}; ///< +1
    float intensity{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportLightIntensity read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportLightIntensity&) const = default;
};

/// 0x5E MSG_REPORT_Z_PLUS (S->C)
struct ReportZPlus {
    static constexpr Op kOp = Op::ReportZPlus;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t netCode{}; ///< +1
    std::uint8_t z{}; ///< +3 height above ground

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportZPlus read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportZPlus&) const = default;
};

/// 0x5F MSG_REPORT_Z_MINUS (S->C)
struct ReportZMinus {
    static constexpr Op kOp = Op::ReportZMinus;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t netCode{}; ///< +1
    std::uint8_t z{}; ///< +3 depth (stored negated)

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportZMinus read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportZMinus&) const = default;
};

/// 0x60 MSG_REPORT_EQUIP (S->C)
struct ReportEquip {
    static constexpr Op kOp = Op::ReportEquip;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t netCode{}; ///< +1 inventory item equipped

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportEquip read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportEquip&) const = default;
};

/// 0x61 MSG_REPORT_DEQUIP (S->C)
struct ReportDequip {
    static constexpr Op kOp = Op::ReportDequip;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t netCode{}; ///< +1 inventory item unequipped

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportDequip read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportDequip&) const = default;
};

/// 0x62 MSG_REPORT_ACQUIRE_SPELL (-). name only; no handler in either 1.2b decoder
struct ReportAcquireSpell {
    static constexpr Op kOp = Op::ReportAcquireSpell;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportAcquireSpell read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportAcquireSpell&) const = default;
};

/// 0x63 MSG_REPORT_TARGET (-). name only; no handler in either 1.2b decoder
struct ReportTarget {
    static constexpr Op kOp = Op::ReportTarget;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportTarget read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportTarget&) const = default;
};

/// 0x64 MSG_REPORT_CHARGES (S->C)
struct ReportCharges {
    static constexpr Op kOp = Op::ReportCharges;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint16_t netCode{}; ///< +1 wand/staff
    std::uint8_t charges{}; ///< +3
    std::uint8_t maxCharges{}; ///< +4

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportCharges read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportCharges&) const = default;
};

/// 0x65 MSG_REPORT_X_STATUS (S->C)
struct ReportXStatus {
    static constexpr Op kOp = Op::ReportXStatus;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 7;

    std::uint16_t netCode{}; ///< +1
    std::uint32_t xStatus{}; ///< +3 the drawable extra status bits (0x400 = special effect start, 0x800 = hide)

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportXStatus read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportXStatus&) const = default;
};

/// 0x66 MSG_REPORT_PLAYER_STATUS (S->C)
struct ReportPlayerStatus {
    static constexpr Op kOp = Op::ReportPlayerStatus;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint32_t statusFlags{}; ///< +1 local player drawable flags (the drawable)

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportPlayerStatus read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportPlayerStatus&) const = default;
};

/// 0x67 MSG_REPORT_MODIFIER (S->C)
struct ReportModifier {
    static constexpr Op kOp = Op::ReportModifier;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 7;

    std::uint16_t netCode{}; ///< +1 item
    std::uint8_t effectiveness{}; ///< +3 modifier id
    std::uint8_t material{}; ///< +4 modifier id
    std::uint8_t enchant1{}; ///< +5 modifier id
    std::uint8_t enchant2{}; ///< +6 modifier id

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportModifier read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportModifier&) const = default;
};

/// 0x68 MSG_REPORT_STAT_MODIFIER (S->C)
struct ReportStatModifier {
    static constexpr Op kOp = Op::ReportStatModifier;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 8;

    std::uint16_t netCode{}; ///< +1 must be your player
    float value{}; ///< +3
    std::uint8_t stat{}; ///< +7 which stat multiplier

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportStatModifier read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportStatModifier&) const = default;
};

/// 0x69 MSG_REPORT_NPC (S->C). 21 bytes; creates or re-inits the NPC entry
struct ReportNpc {
    static constexpr Op kOp = Op::ReportNpc;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 21;

    std::uint16_t npcIdFlags{}; ///< +1 bit15 flag, bits 0-14 = NPC id
    std::array<std::uint8_t, 18> colors{}; ///< +3 6 x rgb colors for the NPC model

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportNpc read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportNpc&) const = default;
};

/// 0x6A MSG_REPORT_CLIENT_STATUS (S->C)
struct ReportClientStatus {
    static constexpr Op kOp = Op::ReportClientStatus;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 7;

    std::uint16_t netCode{}; ///< +1 player
    std::uint32_t status{}; ///< +3 the player record bits, masked 0x423: 1 observer, 2 following a target, 0x400 poisoned (0x200 = in NPC dialog is never sent)

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportClientStatus read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportClientStatus&) const = default;
};

/// 0x6B MSG_REPORT_ANIMATION_FRAME (S->C)
struct ReportAnimationFrame {
    static constexpr Op kOp = Op::ReportAnimationFrame;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 7;

    std::uint16_t netCode{}; ///< +1
    std::uint32_t frame{}; ///< +3 animation frame

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportAnimationFrame read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportAnimationFrame&) const = default;
};

/// 0x6C MSG_REPORT_ACQUIRE_CREATURE (S->C). you gained a summoned/charmed creature
struct ReportAcquireCreature {
    static constexpr Op kOp = Op::ReportAcquireCreature;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint16_t netCode{}; ///< +1 creature
    std::uint16_t typeFlags{}; ///< +3 bit15 flag, bits 0-14 = creature type id

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportAcquireCreature read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportAcquireCreature&) const = default;
};

/// 0x6D MSG_REPORT_LOSE_CREATURE (S->C). 3 bytes
struct ReportLoseCreature {
    static constexpr Op kOp = Op::ReportLoseCreature;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t netCodeFlags{}; ///< +1 bit15 = banished (vs died), bits 0-14 = creature netcode

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportLoseCreature read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportLoseCreature&) const = default;
};

/// 0x6E MSG_REPORT_EXPERIENCE (S->C)
struct ReportExperience {
    static constexpr Op kOp = Op::ReportExperience;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint32_t experience{}; ///< +1

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportExperience read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportExperience&) const = default;
};

/// 0x6F MSG_REPORT_SPELL_AWARD (S->C)
struct ReportSpellAward {
    static constexpr Op kOp = Op::ReportSpellAward;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint8_t spellId{}; ///< +1
    std::uint8_t level{}; ///< +2
    std::uint8_t flags{}; ///< +3 bit7 = show award message; low 7 bits passed through

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportSpellAward read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportSpellAward&) const = default;
};

/// 0x70 MSG_REPORT_SPELL_START (S->C)
struct ReportSpellStart {
    static constexpr Op kOp = Op::ReportSpellStart;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    std::uint8_t spellId{}; ///< +1 spell cast started (plays casting effect)

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportSpellStart read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportSpellStart&) const = default;
};

/// 0x71 MSG_REPORT_INVENTORY_LOADED (S->C). inventory fully loaded; no payload
struct ReportInventoryLoaded {
    static constexpr Op kOp = Op::ReportInventoryLoaded;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportInventoryLoaded read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportInventoryLoaded&) const = default;
};

/// 0x72 MSG_TRY_DROP (C->S)
struct TryDrop {
    static constexpr Op kOp = Op::TryDrop;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 7;

    std::uint16_t netCode{}; ///< +1 item to drop
    std::uint16_t x{}; ///< +3
    std::uint16_t y{}; ///< +5

    /// Reads the fields; `r` is positioned after the opcode.
    static TryDrop read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const TryDrop&) const = default;
};

/// 0x73 MSG_TRY_GET (C->S)
struct TryGet {
    static constexpr Op kOp = Op::TryGet;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t netCode{}; ///< +1 object to pick up

    /// Reads the fields; `r` is positioned after the opcode.
    static TryGet read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const TryGet&) const = default;
};

/// 0x74 MSG_TRY_USE (C->S)
struct TryUse {
    static constexpr Op kOp = Op::TryUse;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t netCode{}; ///< +1 inventory item to use

    /// Reads the fields; `r` is positioned after the opcode.
    static TryUse read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const TryUse&) const = default;
};

/// 0x75 MSG_TRY_EQUIP (C->S)
struct TryEquip {
    static constexpr Op kOp = Op::TryEquip;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t netCode{}; ///< +1 inventory item to equip

    /// Reads the fields; `r` is positioned after the opcode.
    static TryEquip read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const TryEquip&) const = default;
};

/// 0x76 MSG_TRY_DEQUIP (C->S)
struct TryDequip {
    static constexpr Op kOp = Op::TryDequip;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t netCode{}; ///< +1 inventory item to unequip

    /// Reads the fields; `r` is positioned after the opcode.
    static TryDequip read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const TryDequip&) const = default;
};

/// 0x77 MSG_TRY_TARGET (-). name only; no handler in either 1.2b decoder
struct TryTarget {
    static constexpr Op kOp = Op::TryTarget;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static TryTarget read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const TryTarget&) const = default;
};

/// 0x78 MSG_TRY_CREATURE_COMMAND (C->S)
struct TryCreatureCommand {
    static constexpr Op kOp = Op::TryCreatureCommand;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t netCode{}; ///< +1 target creature (0 = all your creatures)
    std::uint8_t command{}; ///< +3 creature order

    /// Reads the fields; `r` is positioned after the opcode.
    static TryCreatureCommand read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const TryCreatureCommand&) const = default;
};

/// 0x79 MSG_TRY_SPELL (C->S). cast the listed spell(s); 1 spell = normal cast, several = trap/combo
struct TrySpell {
    static constexpr Op kOp = Op::TrySpell;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 22;

    std::uint32_t spell1{}; ///< +1 spell id (0 = empty)
    std::uint32_t spell2{}; ///< +5
    std::uint32_t spell3{}; ///< +9
    std::uint32_t spell4{}; ///< +13
    std::uint32_t spell5{}; ///< +17
    std::uint8_t targetFlag{}; ///< +21 passed to the spell book (target/self)

    /// Reads the fields; `r` is positioned after the opcode.
    static TrySpell read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const TrySpell&) const = default;
};

/// 0x7A MSG_TRY_ABILITY (C->S)
struct TryAbility {
    static constexpr Op kOp = Op::TryAbility;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    std::uint8_t ability{}; ///< +1 ability id (1 berserk, 2 warcry, 3 harpoon, 4 tread lightly, 5 eye of the wolf)

    /// Reads the fields; `r` is positioned after the opcode.
    static TryAbility read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const TryAbility&) const = default;
};

/// 0x7B MSG_TRY_COLLIDE (C->S)
struct TryCollide {
    static constexpr Op kOp = Op::TryCollide;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t netCode{}; ///< +1 object you walked into/clicked (calls its collide handler)

    /// Reads the fields; `r` is positioned after the opcode.
    static TryCollide read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const TryCollide&) const = default;
};

/// 0x7C MSG_FX_PARTICLEFX (S->C)
struct FxParticlefx {
    static constexpr Op kOp = Op::FxParticlefx;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 14;

    std::uint8_t fxCode{}; ///< +1 particle effect id
    std::uint16_t count{}; ///< +2
    std::uint16_t flag{}; ///< +4 non-zero = flag
    std::uint16_t max{}; ///< +6
    std::uint16_t netCode{}; ///< +8 attached drawable
    std::array<std::uint8_t, 4> unused{}; ///< +10 not read by the client

    /// Reads the fields; `r` is positioned after the opcode.
    static FxParticlefx read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxParticlefx&) const = default;
};

/// 0x7D MSG_FX_PLASMA (S->C). ray effect between two points (lightning, bolt, drain, charm, heal, plasma)
struct FxPlasma {
    static constexpr Op kOp = Op::FxPlasma;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 9;

    std::uint16_t x1{}; ///< +1
    std::uint16_t y1{}; ///< +3
    std::uint16_t x2{}; ///< +5
    std::uint16_t y2{}; ///< +7

    /// Reads the fields; `r` is positioned after the opcode.
    static FxPlasma read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxPlasma&) const = default;
};

/// 0x7E MSG_FX_SUMMON (S->C). 12 bytes; SummonEffect sprite with a ghost of the creature
struct FxSummon {
    static constexpr Op kOp = Op::FxSummon;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 12;

    std::uint16_t x{}; ///< +1 summon point
    std::uint16_t y{}; ///< +3
    std::uint16_t summonId{}; ///< +5 matched by FX_SUMMON_CANCEL (the drawable)
    std::uint16_t typeId{}; ///< +7 creature being summoned
    std::uint8_t dir{}; ///< +9 facing (0-255 -> 8 directions)
    std::uint16_t duration{}; ///< +10 frames

    /// Reads the fields; `r` is positioned after the opcode.
    static FxSummon read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxSummon&) const = default;
};

/// 0x7F MSG_FX_SUMMON_CANCEL (S->C)
struct FxSummonCancel {
    static constexpr Op kOp = Op::FxSummonCancel;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t summonId{}; ///< +1 same id as in FX_SUMMON

    /// Reads the fields; `r` is positioned after the opcode.
    static FxSummonCancel read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxSummonCancel&) const = default;
};

/// 0x80 MSG_FX_SHIELD (S->C)
struct FxShield {
    static constexpr Op kOp = Op::FxShield;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t netCode{}; ///< +1
    std::uint8_t shieldType{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode.
    static FxShield read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxShield&) const = default;
};

/// 0x81 MSG_FX_BLUE_SPARKS (S->C). point effect at x,y
struct FxBlueSparks {
    static constexpr Op kOp = Op::FxBlueSparks;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::int16_t x{}; ///< +1
    std::int16_t y{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode.
    static FxBlueSparks read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxBlueSparks&) const = default;
};

/// 0x82 MSG_FX_YELLOW_SPARKS (S->C). point effect at x,y
struct FxYellowSparks {
    static constexpr Op kOp = Op::FxYellowSparks;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::int16_t x{}; ///< +1
    std::int16_t y{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode.
    static FxYellowSparks read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxYellowSparks&) const = default;
};

/// 0x83 MSG_FX_CYAN_SPARKS (S->C). point effect at x,y
struct FxCyanSparks {
    static constexpr Op kOp = Op::FxCyanSparks;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::int16_t x{}; ///< +1
    std::int16_t y{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode.
    static FxCyanSparks read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxCyanSparks&) const = default;
};

/// 0x84 MSG_FX_VIOLET_SPARKS (S->C). point effect at x,y
struct FxVioletSparks {
    static constexpr Op kOp = Op::FxVioletSparks;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::int16_t x{}; ///< +1
    std::int16_t y{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode.
    static FxVioletSparks read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxVioletSparks&) const = default;
};

/// 0x85 MSG_FX_EXPLOSION (S->C). point effect at x,y
struct FxExplosion {
    static constexpr Op kOp = Op::FxExplosion;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::int16_t x{}; ///< +1
    std::int16_t y{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode.
    static FxExplosion read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxExplosion&) const = default;
};

/// 0x86 MSG_FX_LESSER_EXPLOSION (S->C). point effect at x,y
struct FxLesserExplosion {
    static constexpr Op kOp = Op::FxLesserExplosion;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::int16_t x{}; ///< +1
    std::int16_t y{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode.
    static FxLesserExplosion read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxLesserExplosion&) const = default;
};

/// 0x87 MSG_FX_COUNTERSPELL_EXPLOSION (S->C). point effect at x,y
struct FxCounterspellExplosion {
    static constexpr Op kOp = Op::FxCounterspellExplosion;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::int16_t x{}; ///< +1
    std::int16_t y{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode.
    static FxCounterspellExplosion read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxCounterspellExplosion&) const = default;
};

/// 0x88 MSG_FX_THIN_EXPLOSION (S->C). point effect at x,y
struct FxThinExplosion {
    static constexpr Op kOp = Op::FxThinExplosion;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::int16_t x{}; ///< +1
    std::int16_t y{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode.
    static FxThinExplosion read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxThinExplosion&) const = default;
};

/// 0x89 MSG_FX_TELEPORT (S->C). point effect at x,y
struct FxTeleport {
    static constexpr Op kOp = Op::FxTeleport;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::int16_t x{}; ///< +1
    std::int16_t y{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode.
    static FxTeleport read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxTeleport&) const = default;
};

/// 0x8A MSG_FX_SMOKE_BLAST (S->C). point effect at x,y
struct FxSmokeBlast {
    static constexpr Op kOp = Op::FxSmokeBlast;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::int16_t x{}; ///< +1
    std::int16_t y{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode.
    static FxSmokeBlast read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxSmokeBlast&) const = default;
};

/// 0x8B MSG_FX_DAMAGE_POOF (S->C). point effect at x,y
struct FxDamagePoof {
    static constexpr Op kOp = Op::FxDamagePoof;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::int16_t x{}; ///< +1
    std::int16_t y{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode.
    static FxDamagePoof read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxDamagePoof&) const = default;
};

/// 0x8C MSG_FX_LIGHTNING (S->C). ray effect between two points (lightning, bolt, drain, charm, heal, plasma)
struct FxLightning {
    static constexpr Op kOp = Op::FxLightning;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 9;

    std::uint16_t x1{}; ///< +1
    std::uint16_t y1{}; ///< +3
    std::uint16_t x2{}; ///< +5
    std::uint16_t y2{}; ///< +7

    /// Reads the fields; `r` is positioned after the opcode.
    static FxLightning read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxLightning&) const = default;
};

/// 0x8D MSG_FX_ENERGY_BOLT (S->C). ray effect between two points (lightning, bolt, drain, charm, heal, plasma)
struct FxEnergyBolt {
    static constexpr Op kOp = Op::FxEnergyBolt;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 9;

    std::uint16_t x1{}; ///< +1
    std::uint16_t y1{}; ///< +3
    std::uint16_t x2{}; ///< +5
    std::uint16_t y2{}; ///< +7

    /// Reads the fields; `r` is positioned after the opcode.
    static FxEnergyBolt read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxEnergyBolt&) const = default;
};

/// 0x8E MSG_FX_CHAIN_LIGHTNING_BOLT (S->C). ray effect between two points (lightning, bolt, drain, charm, heal, plasma)
struct FxChainLightningBolt {
    static constexpr Op kOp = Op::FxChainLightningBolt;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 9;

    std::uint16_t x1{}; ///< +1
    std::uint16_t y1{}; ///< +3
    std::uint16_t x2{}; ///< +5
    std::uint16_t y2{}; ///< +7

    /// Reads the fields; `r` is positioned after the opcode.
    static FxChainLightningBolt read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxChainLightningBolt&) const = default;
};

/// 0x8F MSG_FX_DRAIN_MANA (S->C). ray effect between two points (lightning, bolt, drain, charm, heal, plasma)
struct FxDrainMana {
    static constexpr Op kOp = Op::FxDrainMana;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 9;

    std::uint16_t x1{}; ///< +1
    std::uint16_t y1{}; ///< +3
    std::uint16_t x2{}; ///< +5
    std::uint16_t y2{}; ///< +7

    /// Reads the fields; `r` is positioned after the opcode.
    static FxDrainMana read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxDrainMana&) const = default;
};

/// 0x90 MSG_FX_CHARM (S->C). ray effect between two points (lightning, bolt, drain, charm, heal, plasma)
struct FxCharm {
    static constexpr Op kOp = Op::FxCharm;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 9;

    std::uint16_t x1{}; ///< +1
    std::uint16_t y1{}; ///< +3
    std::uint16_t x2{}; ///< +5
    std::uint16_t y2{}; ///< +7

    /// Reads the fields; `r` is positioned after the opcode.
    static FxCharm read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxCharm&) const = default;
};

/// 0x91 MSG_FX_GREATER_HEAL (S->C). ray effect between two points (lightning, bolt, drain, charm, heal, plasma)
struct FxGreaterHeal {
    static constexpr Op kOp = Op::FxGreaterHeal;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 9;

    std::uint16_t x1{}; ///< +1
    std::uint16_t y1{}; ///< +3
    std::uint16_t x2{}; ///< +5
    std::uint16_t y2{}; ///< +7

    /// Reads the fields; `r` is positioned after the opcode.
    static FxGreaterHeal read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxGreaterHeal&) const = default;
};

/// 0x92 MSG_FX_MAGIC (-). name only; no handler in either 1.2b decoder
struct FxMagic {
    static constexpr Op kOp = Op::FxMagic;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static FxMagic read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxMagic&) const = default;
};

/// 0x93 MSG_FX_SPARK_EXPLOSION (S->C)
struct FxSparkExplosion {
    static constexpr Op kOp = Op::FxSparkExplosion;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::int16_t x{}; ///< +1
    std::int16_t y{}; ///< +3
    std::uint8_t size{}; ///< +5 >170 uses FireBoom, else MediumFireBoom; also spark count

    /// Reads the fields; `r` is positioned after the opcode.
    static FxSparkExplosion read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxSparkExplosion&) const = default;
};

/// 0x94 MSG_FX_DEATH_RAY (S->C)
struct FxDeathRay {
    static constexpr Op kOp = Op::FxDeathRay;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 9;

    std::uint16_t x1{}; ///< +1
    std::uint16_t y1{}; ///< +3
    std::uint16_t x2{}; ///< +5
    std::uint16_t y2{}; ///< +7

    /// Reads the fields; `r` is positioned after the opcode.
    static FxDeathRay read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxDeathRay&) const = default;
};

/// 0x95 MSG_FX_SENTRY_RAY (S->C)
struct FxSentryRay {
    static constexpr Op kOp = Op::FxSentryRay;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 9;

    std::uint16_t x1{}; ///< +1 sentry globe
    std::uint16_t y1{}; ///< +3
    std::uint16_t x2{}; ///< +5 hit point
    std::uint16_t y2{}; ///< +7

    /// Reads the fields; `r` is positioned after the opcode.
    static FxSentryRay read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxSentryRay&) const = default;
};

/// 0x96 MSG_FX_RICOCHET (S->C)
struct FxRicochet {
    static constexpr Op kOp = Op::FxRicochet;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::int16_t x{}; ///< +1
    std::int16_t y{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode.
    static FxRicochet read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxRicochet&) const = default;
};

/// 0x97 MSG_FX_JIGGLE (S->C)
struct FxJiggle {
    static constexpr Op kOp = Op::FxJiggle;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    std::uint8_t amount{}; ///< +1 screen shake; viewport jiggle = amount/3

    /// Reads the fields; `r` is positioned after the opcode.
    static FxJiggle read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxJiggle&) const = default;
};

/// 0x98 MSG_FX_GREEN_BOLT (S->C)
struct FxGreenBolt {
    static constexpr Op kOp = Op::FxGreenBolt;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 11;

    std::uint16_t x1{}; ///< +1
    std::uint16_t y1{}; ///< +3
    std::uint16_t x2{}; ///< +5
    std::uint16_t y2{}; ///< +7
    std::uint16_t duration{}; ///< +9

    /// Reads the fields; `r` is positioned after the opcode.
    static FxGreenBolt read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxGreenBolt&) const = default;
};

/// 0x99 MSG_FX_GREEN_EXPLOSION (-). name only; no handler in either 1.2b decoder
struct FxGreenExplosion {
    static constexpr Op kOp = Op::FxGreenExplosion;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static FxGreenExplosion read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxGreenExplosion&) const = default;
};

/// 0x9A MSG_FX_WHITE_FLASH (S->C)
struct FxWhiteFlash {
    static constexpr Op kOp = Op::FxWhiteFlash;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::array<std::uint8_t, 4> unused{}; ///< +1 screen white flash; payload ignored

    /// Reads the fields; `r` is positioned after the opcode.
    static FxWhiteFlash read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxWhiteFlash&) const = default;
};

/// 0x9B MSG_FX_GENERATING_MAP (S->C). 3 bytes
struct FxGeneratingMap {
    static constexpr Op kOp = Op::FxGeneratingMap;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t progress{}; ///< +1 redraws the map-generation screen when it changes

    /// Reads the fields; `r` is positioned after the opcode.
    static FxGeneratingMap read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxGeneratingMap&) const = default;
};

/// 0x9C MSG_FX_ASSEMBLING_MAP (S->C). 3 bytes
struct FxAssemblingMap {
    static constexpr Op kOp = Op::FxAssemblingMap;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t progress{}; ///< +1 redraws the map-generation screen when it changes

    /// Reads the fields; `r` is positioned after the opcode.
    static FxAssemblingMap read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxAssemblingMap&) const = default;
};

/// 0x9D MSG_FX_POPULATING_MAP (S->C). 3 bytes
struct FxPopulatingMap {
    static constexpr Op kOp = Op::FxPopulatingMap;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t progress{}; ///< +1 redraws the map-generation screen when it changes

    /// Reads the fields; `r` is positioned after the opcode.
    static FxPopulatingMap read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxPopulatingMap&) const = default;
};

/// 0x9E MSG_FX_DURATION_SPELL (S->C). 7 bytes; duration-spell beam between two objects
struct FxDurationSpell {
    static constexpr Op kOp = Op::FxDurationSpell;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 7;

    std::uint8_t kind{}; ///< +1 1-7 add ray effect, 8-14 remove the matching ray (removal matches the two netCodes only)
    std::uint8_t level{}; ///< +2 spell level; caster direction for plasma
    std::uint16_t netCode1{}; ///< +3 greater heal: target first
    std::uint16_t netCode2{}; ///< +5

    /// Reads the fields; `r` is positioned after the opcode.
    static FxDurationSpell read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxDurationSpell&) const = default;
};

/// 0x9F MSG_FX_DELTAZ_SPELL_START (S->C)
struct FxDeltazSpellStart {
    static constexpr Op kOp = Op::FxDeltazSpellStart;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint16_t netCode{}; ///< +1
    std::uint8_t startZ{}; ///< +3
    std::int8_t deltaZ{}; ///< +4
    std::uint8_t speed{}; ///< +5

    /// Reads the fields; `r` is positioned after the opcode.
    static FxDeltazSpellStart read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxDeltazSpellStart&) const = default;
};

/// 0xA0 MSG_FX_TURN_UNDEAD (S->C)
struct FxTurnUndead {
    static constexpr Op kOp = Op::FxTurnUndead;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::int16_t x{}; ///< +1
    std::int16_t y{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode.
    static FxTurnUndead read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxTurnUndead&) const = default;
};

/// 0xA1 MSG_FX_ARROW_TRAP (S->C)
struct FxArrowTrap {
    static constexpr Op kOp = Op::FxArrowTrap;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::int16_t x{}; ///< +1
    std::int16_t y{}; ///< +3
    std::uint8_t variant{}; ///< +5 1 = ArrowTrap1Smoke, else ArrowTrap2Smoke

    /// Reads the fields; `r` is positioned after the opcode.
    static FxArrowTrap read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxArrowTrap&) const = default;
};

/// 0xA2 MSG_FX_VAMPIRISM (S->C)
struct FxVampirism {
    static constexpr Op kOp = Op::FxVampirism;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 11;

    std::uint16_t x1{}; ///< +1 from
    std::uint16_t y1{}; ///< +3
    std::uint16_t x2{}; ///< +5 to
    std::uint16_t y2{}; ///< +7
    std::uint16_t amount{}; ///< +9 orb count = amount/4 (max 8)

    /// Reads the fields; `r` is positioned after the opcode.
    static FxVampirism read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxVampirism&) const = default;
};

/// 0xA3 MSG_FX_MANA_BOMB_CANCEL (S->C)
struct FxManaBombCancel {
    static constexpr Op kOp = Op::FxManaBombCancel;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::int16_t x{}; ///< +1
    std::int16_t y{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode.
    static FxManaBombCancel read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FxManaBombCancel&) const = default;
};

/// 0xA4 MSG_UPDATE_STREAM (S->C)
struct UpdateStream {
    static constexpr Op kOp = Op::UpdateStream;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = kVariableSize;

    std::vector<std::uint8_t> stream{}; ///< +1 see update_stream.hpp

    /// Reads the fields; `r` is positioned after the opcode.
    static UpdateStream read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const UpdateStream&) const = default;
};

/// 0xA5 MSG_NEW_ALIAS (C->S). C->S: sent when an UPDATE_STREAM entry used the full 0xFF form, so the server can use the alias from then on
struct NewAlias {
    static constexpr Op kOp = Op::NewAlias;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 10;

    std::uint8_t alias{}; ///< +1 alias slot 0-254 the client assigned
    std::uint16_t netCode{}; ///< +2
    std::uint16_t typeId{}; ///< +4
    std::uint32_t expireFrame{}; ///< +6 frame+60 for stream entries; 0xFFFFFFFF for the header object (never expires)

    /// Reads the fields; `r` is positioned after the opcode.
    static NewAlias read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const NewAlias&) const = default;
};

/// 0xA6 MSG_AUDIO_EVENT (S->C)
struct AudioEvent {
    static constexpr Op kOp = Op::AudioEvent;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::int8_t pan{}; ///< +1 stereo pan, clamped -50..50
    std::uint16_t soundBits{}; ///< +2 bits 0-9 sound id, (bits >> 9) & 0x7E = volume

    /// Reads the fields; `r` is positioned after the opcode.
    static AudioEvent read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const AudioEvent&) const = default;
};

/// 0xA7 MSG_AUDIO_PLAYER_EVENT (S->C)
struct AudioPlayerEvent {
    static constexpr Op kOp = Op::AudioPlayerEvent;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::int8_t pan{}; ///< +1 stereo pan, clamped -50..50
    std::uint16_t soundBits{}; ///< +2 bits 0-9 sound id, (bits >> 9) & 0x7E = volume

    /// Reads the fields; `r` is positioned after the opcode.
    static AudioPlayerEvent read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const AudioPlayerEvent&) const = default;
};

/// 0xA8 MSG_TEXT_MESSAGE (S->C / C->S)
struct TextMessage {
    static constexpr Op kOp = Op::TextMessage;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = kVariableSize;

    std::uint16_t netCode{}; ///< +1 speaker
    std::uint8_t flags{}; ///< +3 1 team, 2 8-bit text, 4 UTF-16 text, 8 localized string id, 0x10 notice
    std::uint16_t x{}; ///< +4 speech-bubble position
    std::uint16_t y{}; ///< +6
    std::uint8_t size{}; ///< +8 characters or bytes, including the terminator
    std::uint16_t duration{}; ///< +9 frames
    std::vector<std::uint8_t> text{}; ///< +11 size x UTF-16, or size bytes

    /// Reads the fields; `r` is positioned after the opcode.
    static TextMessage read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const TextMessage&) const = default;
};

/// 0xAA MSG_IMPORTANT (S->C / C->S)
struct Important {
    static constexpr Op kOp = Op::Important;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint32_t id{}; ///< +1 S->C: only present when the receiver is the host (1 byte otherwise); C->S: always 5 bytes

    /// Reads the fields; `r` is positioned after the opcode. Fields missing from a shorter direction's form are left zero.
    static Important read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Important&) const = default;
};

/// 0xAB MSG_IMPORTANT_ACK (S->C / C->S)
struct ImportantAck {
    static constexpr Op kOp = Op::ImportantAck;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint32_t id{}; ///< +1 important-message id being acknowledged

    /// Reads the fields; `r` is positioned after the opcode.
    static ImportantAck read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ImportantAck&) const = default;
};

/// 0xAC MSG_MOUSE (C->S)
struct Mouse {
    static constexpr Op kOp = Op::Mouse;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint16_t x{}; ///< +1 cursor world x
    std::uint16_t y{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode.
    static Mouse read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Mouse&) const = default;
};

/// 0xAD MSG_INCOMING_CLIENT (C->S). client finished loading and is entering the game; no payload
struct IncomingClient {
    static constexpr Op kOp = Op::IncomingClient;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static IncomingClient read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const IncomingClient&) const = default;
};

/// 0xAE MSG_OUTGOING_CLIENT (S->C / C->S)
struct OutgoingClient {
    static constexpr Op kOp = Op::OutgoingClient;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t netCode{}; ///< +1 S->C only (3 bytes): player leaving; C->S is opcode only (1 byte)

    /// Reads the fields; `r` is positioned after the opcode. Fields missing from a shorter direction's form are left zero.
    static OutgoingClient read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const OutgoingClient&) const = default;
};

/// 0xAF MSG_GAME_SETTINGS (S->C). 20 bytes; sent to every player except the host (target 0x9F)
struct GameSettings {
    static constexpr Op kOp = Op::GameSettings;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 20;

    std::uint32_t settingsSerial{}; ///< +1 server writes its current frame; applied only when newer than the client's copy
    std::uint32_t version{}; ///< +5 server game version
    std::uint32_t gameFlags{}; ///< +9 gameFlags; low 16 bits also stored in the game options when bit 7 is clear
    std::uint32_t serverSubFlags{}; ///< +13
    std::uint8_t playerLimit{}; ///< +17
    std::uint8_t fragLimit{}; ///< +18 the game options
    std::uint8_t timeLimit{}; ///< +19 minutes; the game options

    /// Reads the fields; `r` is positioned after the opcode.
    static GameSettings read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const GameSettings&) const = default;
};

/// 0xB0 MSG_GAME_SETTINGS_2 (S->C). 49 bytes; prints "OptionsChanged"
struct GameSettings2 {
    static constexpr Op kOp = Op::GameSettings2;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 49;

    std::array<std::uint8_t, 16> gameName{}; ///< +1 ASCII, 15 chars used; the game options
    std::array<std::uint8_t, 20> allowedSpells{}; ///< +17 bitmap of spells 1-136 (bit n = spell n enabled); the game options
    std::uint32_t gameData44{}; ///< +37 the game options
    std::uint32_t gameData48{}; ///< +41 the game options
    std::uint32_t timeRemainingMs{}; ///< +45 non-zero: starts the game timer (same setter as TIMER_STATUS)

    /// Reads the fields; `r` is positioned after the opcode.
    static GameSettings2 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const GameSettings2&) const = default;
};

/// 0xB1 MSG_UPDATE_GUI_GAME_SETTINGS (S->C). 60 bytes
struct UpdateGuiGameSettings {
    static constexpr Op kOp = Op::UpdateGuiGameSettings;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 60;

    std::uint8_t slot{}; ///< +1 gamedata slot; 1 = current
    std::array<std::uint8_t, 58> gameData{}; ///< +2 whole gamedata block: char mapName[9], char gameName[15], u32 allowedSpells[5], u32 +44, u32 +48, u16 +52 flags (bit 5 checked), u16 +54, u8 +56, u8 +57

    /// Reads the fields; `r` is positioned after the opcode.
    static UpdateGuiGameSettings read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const UpdateGuiGameSettings&) const = default;
};

/// 0xB2 MSG_DOOR_ANGLE (S->C)
struct DoorAngle {
    static constexpr Op kOp = Op::DoorAngle;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t netCode{}; ///< +1 door
    std::uint8_t angle{}; ///< +3 door rotation frame

    /// Reads the fields; `r` is positioned after the opcode.
    static DoorAngle read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const DoorAngle&) const = default;
};

/// 0xB3 MSG_OBELISK_CHARGE (S->C)
struct ObeliskCharge {
    static constexpr Op kOp = Op::ObeliskCharge;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t netCode{}; ///< +1 obelisk
    std::uint8_t charge{}; ///< +3 0-50; light = 16*c/10, frame = 8*c/50

    /// Reads the fields; `r` is positioned after the opcode.
    static ObeliskCharge read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ObeliskCharge&) const = default;
};

/// 0xB4 MSG_PENTAGRAM_ACTIVATE (S->C). sent for PressurePlate objects; TeleportPentagram objects use DRAW_FRAME (0x39)
struct PentagramActivate {
    static constexpr Op kOp = Op::PentagramActivate;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t netCode{}; ///< +1 PressurePlate object
    std::uint8_t state{}; ///< +3 pressed state/frame

    /// Reads the fields; `r` is positioned after the opcode.
    static PentagramActivate read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const PentagramActivate&) const = default;
};

/// 0xB5 MSG_CLIENT_PREDICT_LINEAR (S->C). 14 bytes; client-side linear prediction for missiles
struct ClientPredictLinear {
    static constexpr Op kOp = Op::ClientPredictLinear;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 14;

    std::uint16_t netCode{}; ///< +1
    std::uint16_t typeId{}; ///< +3
    std::uint16_t x{}; ///< +5
    std::uint16_t y{}; ///< +7
    std::uint16_t direction{}; ///< +9 the object; client stores it at the drawable
    std::int8_t vz{}; ///< +11 the object x 16; client divides by 16 and uses it as friction
    std::int8_t vx{}; ///< +12 /16
    std::int8_t vy{}; ///< +13 /16

    /// Reads the fields; `r` is positioned after the opcode.
    static ClientPredictLinear read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ClientPredictLinear&) const = default;
};

/// 0xB6 MSG_REQUEST_MAP (C->S). C->S: request map download; no payload
struct RequestMap {
    static constexpr Op kOp = Op::RequestMap;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static RequestMap read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const RequestMap&) const = default;
};

/// 0xB7 MSG_CANCEL_MAP (C->S). C->S: cancel map download; no payload
struct CancelMap {
    static constexpr Op kOp = Op::CancelMap;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static CancelMap read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const CancelMap&) const = default;
};

/// 0xB8 MSG_MAP_SEND_START (S->C). 88 bytes
struct MapSendStart {
    static constexpr Op kOp = Op::MapSendStart;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 88;

    std::array<std::uint8_t, 3> unk{}; ///< +1
    std::uint32_t mapSize{}; ///< +4 compressed size in bytes
    std::array<std::uint8_t, 80> mapName{}; ///< +8 ASCII, NUL-padded

    /// Reads the fields; `r` is positioned after the opcode.
    static MapSendStart read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const MapSendStart&) const = default;
};

/// 0xB9 MSG_MAP_SEND_PACKET (S->C)
struct MapSendPacket {
    static constexpr Op kOp = Op::MapSendPacket;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = kVariableSize;

    std::uint8_t unk{}; ///< +1
    std::uint16_t block{}; ///< +2 block index
    std::uint16_t size{}; ///< +4 data bytes that follow
    std::vector<std::uint8_t> data{}; ///< +6 size bytes

    /// Reads the fields; `r` is positioned after the opcode.
    static MapSendPacket read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const MapSendPacket&) const = default;
};

/// 0xBA MSG_MAP_SEND_ABORT (S->C)
struct MapSendAbort {
    static constexpr Op kOp = Op::MapSendAbort;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    std::uint8_t reason{}; ///< +1 abort code

    /// Reads the fields; `r` is positioned after the opcode.
    static MapSendAbort read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const MapSendAbort&) const = default;
};

/// 0xBB MSG_SERVER_CMD (C->S)
struct ServerCmd {
    static constexpr Op kOp = Op::ServerCmd;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = kVariableSize;

    std::uint8_t cmdId{}; ///< +1 0 observer, 1 exec script function, 2 remote sysop command, 3 print to all, 4 watch player, 5 broadcast
    std::uint16_t netCode{}; ///< +2 sender
    std::uint8_t chars{}; ///< +4 UTF-16 char count including the NUL
    std::vector<std::uint8_t> cmd{}; ///< +5 UTF-16 text; the 1.2b client sends chars*2+7 bytes, i.e. 2 bytes more than header+string. Text \uF00D\uF0AD = automatic idle observer

    /// Reads the fields; `r` is positioned after the opcode.
    static ServerCmd read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ServerCmd&) const = default;
};

/// 0xBC MSG_SYSOP_PW (C->S). 21 bytes
struct SysopPw {
    static constexpr Op kOp = Op::SysopPw;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 21;

    std::array<std::uint8_t, 16> password{}; ///< +1 wchar[8] UTF-16 (wcsncpy, 8 chars)
    std::uint16_t zero{}; ///< +17 always 0
    std::uint16_t netCode{}; ///< +19 sender

    /// Reads the fields; `r` is positioned after the opcode.
    static SysopPw read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const SysopPw&) const = default;
};

/// 0xBD MSG_SYSOP_RESULT (S->C)
struct SysopResult {
    static constexpr Op kOp = Op::SysopResult;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    std::uint8_t granted{}; ///< +1 1 = sysop access granted

    /// Reads the fields; `r` is positioned after the opcode.
    static SysopResult read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const SysopResult&) const = default;
};

/// 0xBE MSG_KEEP_ALIVE (C->S). keep-alive; no payload
struct KeepAlive {
    static constexpr Op kOp = Op::KeepAlive;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static KeepAlive read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const KeepAlive&) const = default;
};

/// 0xBF MSG_RECEIVED_MAP (C->S). C->S: map download finished; no payload
struct ReceivedMap {
    static constexpr Op kOp = Op::ReceivedMap;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static ReceivedMap read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReceivedMap&) const = default;
};

/// 0xC0 MSG_CLIENT_READY (C->S). C->S: client ready; no payload
struct ClientReady {
    static constexpr Op kOp = Op::ClientReady;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static ClientReady read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ClientReady&) const = default;
};

/// 0xC1 MSG_REQUEST_SAVE_PLAYER (C->S). 3 bytes
struct RequestSavePlayer {
    static constexpr Op kOp = Op::RequestSavePlayer;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t netCode{}; ///< +1 player

    /// Reads the fields; `r` is positioned after the opcode.
    static RequestSavePlayer read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const RequestSavePlayer&) const = default;
};

/// 0xC3 MSG_PLAYER_OBJ (S->C). 12 bytes; COMPLEX_OBJ + animation index
struct PlayerObj {
    static constexpr Op kOp = Op::PlayerObj;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 12;

    std::uint16_t netCode{}; ///< +1
    std::uint16_t typeId{}; ///< +3
    std::uint16_t x{}; ///< +5
    std::uint16_t y{}; ///< +7
    std::uint8_t animState{}; ///< +9 bits 0-3 animation, bits 4-6 direction
    std::uint8_t frame{}; ///< +10
    std::uint8_t animIndex{}; ///< +11 player action sequence

    /// Reads the fields; `r` is positioned after the opcode.
    static PlayerObj read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const PlayerObj&) const = default;
};

/// 0xC5 MSG_KICK_NOTIFICATION (S->C). S->C: you were kicked; no payload
struct KickNotification {
    static constexpr Op kOp = Op::KickNotification;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static KickNotification read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const KickNotification&) const = default;
};

/// 0xC6 MSG_TIMEOUT_NOTIFICATION (S->C). S->C: you timed out; no payload
struct TimeoutNotification {
    static constexpr Op kOp = Op::TimeoutNotification;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static TimeoutNotification read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const TimeoutNotification&) const = default;
};

/// 0xC7 MSG_SERVER_QUIT (S->C). S->C: server shutting down; client replies 0xC8
struct ServerQuit {
    static constexpr Op kOp = Op::ServerQuit;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static ServerQuit read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ServerQuit&) const = default;
};

/// 0xC8 MSG_SERVER_QUIT_ACK (C->S). C->S: acknowledges SERVER_QUIT
struct ServerQuitAck {
    static constexpr Op kOp = Op::ServerQuitAck;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static ServerQuitAck read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ServerQuitAck&) const = default;
};

/// 0xCA MSG_CHAT_KILL (S->C)
struct ChatKill {
    static constexpr Op kOp = Op::ChatKill;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t netCode{}; ///< +1 remove this netCode's chat bubble; 0xDEAD = remove all chat bubbles

    /// Reads the fields; `r` is positioned after the opcode.
    static ChatKill read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ChatKill&) const = default;
};

/// 0xCB MSG_MESSAGES_KILL (S->C). S->C: clear on-screen message lines; no payload
struct MessagesKill {
    static constexpr Op kOp = Op::MessagesKill;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static MessagesKill read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const MessagesKill&) const = default;
};

/// 0xCC MSG_SEQ_IMPORTANT (S->C). length = 4 + size
struct SeqImportant {
    static constexpr Op kOp = Op::SeqImportant;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = kVariableSize;

    std::uint16_t id{}; ///< +1 sequence id
    std::uint8_t size{}; ///< +3 bytes in the nested message
    std::vector<std::uint8_t> message{}; ///< +4 one complete message (opcode + payload)

    /// Reads the fields; `r` is positioned after the opcode.
    static SeqImportant read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const SeqImportant&) const = default;
};

/// 0xCD MSG_REPORT_ABILITY_AWARD (S->C)
struct ReportAbilityAward {
    static constexpr Op kOp = Op::ReportAbilityAward;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint8_t ability{}; ///< +1 ability id
    std::uint8_t level{}; ///< +2 bits 0-6 level; bit 7 = print the award text

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportAbilityAward read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportAbilityAward&) const = default;
};

/// 0xCE MSG_REPORT_ABILITY_STATE (S->C)
struct ReportAbilityState {
    static constexpr Op kOp = Op::ReportAbilityState;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint8_t ability{}; ///< +1
    std::uint8_t ready{}; ///< +2 1 = ready (clears cooldown timer), 0 = cooling down

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportAbilityState read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportAbilityState&) const = default;
};

/// 0xCF MSG_REPORT_ACTIVE_ABILITIES (S->C)
struct ReportActiveAbilities {
    static constexpr Op kOp = Op::ReportActiveAbilities;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint8_t ability{}; ///< +1
    std::uint8_t active{}; ///< +2 sets/clears bit (1<<ability) in the active-ability mask

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportActiveAbilities read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportActiveAbilities&) const = default;
};

/// 0xD1 MSG_REPORT_GUIDE_AWARD (S->C)
struct ReportGuideAward {
    static constexpr Op kOp = Op::ReportGuideAward;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint8_t creature{}; ///< +1 field-guide creature id
    std::uint8_t level{}; ///< +2 bits 0-6; bit 7 unused

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportGuideAward read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportGuideAward&) const = default;
};

/// 0xD2 MSG_INTERESTING_ID (S->C). 7 bytes; marks an object as "interesting" (health bar, map marker)
struct InterestingId {
    static constexpr Op kOp = Op::InterestingId;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 7;

    std::uint16_t netCode{}; ///< +1 bit 15 set = static (map) object
    std::uint16_t typeId{}; ///< +3 used to create the drawable if unknown
    std::uint8_t add{}; ///< +5 1 = add, else remove
    std::uint8_t kind{}; ///< +6 health-bar / marker kind

    /// Reads the fields; `r` is positioned after the opcode.
    static InterestingId read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const InterestingId&) const = default;
};

/// 0xD3 MSG_TIMER_STATUS (S->C). 13 bytes
struct TimerStatus {
    static constexpr Op kOp = Op::TimerStatus;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 13;

    std::uint32_t running{}; ///< +1 non-zero = timer running
    std::uint32_t timeRemainingMs{}; ///< +5
    std::uint32_t frame{}; ///< +9 server frame; if the client is >=30 frames behind it re-requests with 0xD4

    /// Reads the fields; `r` is positioned after the opcode.
    static TimerStatus read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const TimerStatus&) const = default;
};

/// 0xD4 MSG_REQUEST_TIMER_STATUS (C->S). C->S: request TIMER_STATUS; no payload
struct RequestTimerStatus {
    static constexpr Op kOp = Op::RequestTimerStatus;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 1;

    /// Reads the fields; `r` is positioned after the opcode.
    static RequestTimerStatus read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const RequestTimerStatus&) const = default;
};

/// 0xD5 MSG_JOURNAL_MSG (S->C). 68 bytes
struct JournalMsg {
    static constexpr Op kOp = Op::JournalMsg;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 68;

    std::uint8_t action{}; ///< +1 1 add, 2 remove, 3 update
    std::array<std::uint8_t, 64> entry{}; ///< +2 journal entry string id, ASCII NUL-padded
    std::uint16_t type{}; ///< +66 2 quest, 4 completed, 8 hint (left uninitialised by the 1.2b remove sender)

    /// Reads the fields; `r` is positioned after the opcode.
    static JournalMsg read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const JournalMsg&) const = default;
};

/// 0xD6 MSG_CHAPTER_END (S->C). locks the screen for the chapter-end briefing
struct ChapterEnd {
    static constexpr Op kOp = Op::ChapterEnd;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint8_t chapter{}; ///< +1
    std::uint8_t kind{}; ///< +2 briefing screen kind

    /// Reads the fields; `r` is positioned after the opcode.
    static ChapterEnd read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ChapterEnd&) const = default;
};

/// 0xD7 MSG_REPORT_ALL_LATENCY (S->C)
struct ReportAllLatency {
    static constexpr Op kOp = Op::ReportAllLatency;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint16_t playerId{}; ///< +1
    std::uint16_t latency{}; ///< +3 ms; the player info

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportAllLatency read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportAllLatency&) const = default;
};

/// 0xD8 MSG_REPORT_FLAG_STATUS (S->C). 6 bytes
struct ReportFlagStatus {
    static constexpr Op kOp = Op::ReportFlagStatus;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint8_t state{}; ///< +1 0 at home, 1 carried, 2 dropped
    std::uint8_t flagIndex{}; ///< +2 CTF GUI slot
    std::uint8_t side{}; ///< +3 1 = own team flag, 2 = enemy flag
    std::uint16_t carrier{}; ///< +4 netCode of the carrier when state = 1

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportFlagStatus read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportFlagStatus&) const = default;
};

/// 0xD9 MSG_REPORT_BALL_STATUS (S->C)
struct ReportBallStatus {
    static constexpr Op kOp = Op::ReportBallStatus;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint8_t state{}; ///< +1 0/1 at home or dropped (carrier cleared), 2/4 carried
    std::uint16_t carrier{}; ///< +2 netCode when carried

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportBallStatus read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportBallStatus&) const = default;
};

/// 0xDA MSG_REPORT_OBJECT_POISON (S->C)
struct ReportObjectPoison {
    static constexpr Op kOp = Op::ReportObjectPoison;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t netCode{}; ///< +1 ally creature
    std::uint8_t poison{}; ///< +3 poison level; 0 = cured

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportObjectPoison read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportObjectPoison&) const = default;
};

/// 0xDB MSG_REPORT_MONITOR_CREATURE (S->C). 5 bytes; start showing a health bar for a creature
struct ReportMonitorCreature {
    static constexpr Op kOp = Op::ReportMonitorCreature;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint16_t netCode{}; ///< +1
    std::uint16_t typeId{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportMonitorCreature read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportMonitorCreature&) const = default;
};

/// 0xDC MSG_REPORT_UNMONITOR_CREATURE (S->C)
struct ReportUnmonitorCreature {
    static constexpr Op kOp = Op::ReportUnmonitorCreature;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t netCode{}; ///< +1

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportUnmonitorCreature read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportUnmonitorCreature&) const = default;
};

/// 0xDD MSG_REPORT_TOTAL_HEALTH (S->C)
struct ReportTotalHealth {
    static constexpr Op kOp = Op::ReportTotalHealth;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 7;

    std::uint16_t netCode{}; ///< +1 your player, or a monitored creature
    std::uint16_t health{}; ///< +3
    std::uint16_t maxHealth{}; ///< +5

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportTotalHealth read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportTotalHealth&) const = default;
};

/// 0xDE MSG_REPORT_TOTAL_MANA (S->C)
struct ReportTotalMana {
    static constexpr Op kOp = Op::ReportTotalMana;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 7;

    std::uint16_t netCode{}; ///< +1 must be your player
    std::uint16_t mana{}; ///< +3
    std::uint16_t maxMana{}; ///< +5

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportTotalMana read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportTotalMana&) const = default;
};

/// 0xDF MSG_REPORT_SPELL_STAT (S->C)
struct ReportSpellStat {
    static constexpr Op kOp = Op::ReportSpellStat;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint32_t spell{}; ///< +1 spell id 0-139; NOT bounds-checked by the 1.2b client
    std::uint8_t value{}; ///< +5 per-spell byte table

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportSpellStat read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportSpellStat&) const = default;
};

/// 0xE0 MSG_REPORT_SECONDARY_WEAPON (S->C / C->S). secondary (throwing) weapon
struct ReportSecondaryWeapon {
    static constexpr Op kOp = Op::ReportSecondaryWeapon;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t item{}; ///< +1 netCode; 0 = none
    std::uint8_t fallback{}; ///< +3 S->C only (4 bytes): 1 = item unusable -> print Weapon2CantUse and revert to the previous secondary weapon; C->S is 3 bytes

    /// Reads the fields; `r` is positioned after the opcode. Fields missing from a shorter direction's form are left zero.
    static ReportSecondaryWeapon read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportSecondaryWeapon&) const = default;
};

/// 0xE1 MSG_REPORT_LAST_QUIVER (S->C)
struct ReportLastQuiver {
    static constexpr Op kOp = Op::ReportLastQuiver;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t netCode{}; ///< +1 last quiver used

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportLastQuiver read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportLastQuiver&) const = default;
};

/// 0xE2 MSG_INFO_BOOK_DATA (S->C / C->S)
struct InfoBookData {
    static constexpr Op kOp = Op::InfoBookData;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t netCode{}; ///< +1 book item
    std::uint8_t value{}; ///< +3 C->S: 1 spell book, 2 lore scroll / field guide, 4 ability book (client stores 0x89/0x29/6 in the drawable while waiting); S->C: book/guide index

    /// Reads the fields; `r` is positioned after the opcode.
    static InfoBookData read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const InfoBookData&) const = default;
};

/// 0xE3 MSG_SOCIAL (C->S)
struct Social {
    static constexpr Op kOp = Op::Social;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    std::uint8_t gesture{}; ///< +1 1 laugh, 2 shake fist, 4 point

    /// Reads the fields; `r` is positioned after the opcode.
    static Social read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Social&) const = default;
};

/// 0xE4 MSG_FADE_BEGIN (S->C)
struct FadeBegin {
    static constexpr Op kOp = Op::FadeBegin;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint8_t out{}; ///< +1 1 = fade out, 0 = fade in
    std::uint8_t menu{}; ///< +2 1 = menu fade

    /// Reads the fields; `r` is positioned after the opcode.
    static FadeBegin read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FadeBegin&) const = default;
};

/// 0xE5 MSG_MUSIC_EVENT (S->C)
struct MusicEvent {
    static constexpr Op kOp = Op::MusicEvent;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint8_t track{}; ///< +1 music index
    std::uint8_t volume{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode.
    static MusicEvent read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const MusicEvent&) const = default;
};

/// 0xE6 MSG_MUSIC_PUSH_EVENT (S->C). push the current music onto the stack
struct MusicPushEvent {
    static constexpr Op kOp = Op::MusicPushEvent;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::array<std::uint8_t, 2> unused{}; ///< +1

    /// Reads the fields; `r` is positioned after the opcode.
    static MusicPushEvent read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const MusicPushEvent&) const = default;
};

/// 0xE7 MSG_MUSIC_POP_EVENT (S->C). pop music from the stack
struct MusicPopEvent {
    static constexpr Op kOp = Op::MusicPopEvent;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::array<std::uint8_t, 2> unused{}; ///< +1

    /// Reads the fields; `r` is positioned after the opcode.
    static MusicPopEvent read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const MusicPopEvent&) const = default;
};

/// 0xE8 MSG_PLAYER_DIED (S->C)
struct PlayerDied {
    static constexpr Op kOp = Op::PlayerDied;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t playerId{}; ///< +1

    /// Reads the fields; `r` is positioned after the opcode.
    static PlayerDied read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const PlayerDied&) const = default;
};

/// 0xE9 MSG_PLAYER_RESPAWN (S->C). 9 bytes
struct PlayerRespawn {
    static constexpr Op kOp = Op::PlayerRespawn;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 9;

    std::uint16_t netCode{}; ///< +1 player
    std::uint32_t frame{}; ///< +3 own player only (flag 0x2000): drop dynamic, non-player drawables not seen since this frame (never SummonEffect)
    std::uint8_t equipMask{}; ///< +7 which default items to re-equip: 1 weapon, 2/4 armor pieces, 8/0x10/0x20/0x40/0x80 class-specific
    std::uint8_t flags{}; ///< +8 bit 0 = respawn

    /// Reads the fields; `r` is positioned after the opcode.
    static PlayerRespawn read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const PlayerRespawn&) const = default;
};

/// 0xEA MSG_FORGET_DRAWABLES (S->C)
struct ForgetDrawables {
    static constexpr Op kOp = Op::ForgetDrawables;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint32_t frame{}; ///< +1 drop dynamic, non-player drawables whose last-seen frame (the drawable) is older (only with game flag 0x2000); SummonEffect is kept

    /// Reads the fields; `r` is positioned after the opcode.
    static ForgetDrawables read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ForgetDrawables&) const = default;
};

/// 0xEB MSG_RESET_ABILITIES (S->C)
struct ResetAbilities {
    static constexpr Op kOp = Op::ResetAbilities;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    std::uint8_t ability{}; ///< +1 6 = all

    /// Reads the fields; `r` is positioned after the opcode.
    static ResetAbilities read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ResetAbilities&) const = default;
};

/// 0xEC MSG_RATE_CHANGE (S->C)
struct RateChange {
    static constexpr Op kOp = Op::RateChange;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    std::uint8_t rate{}; ///< +1 network send rate

    /// Reads the fields; `r` is positioned after the opcode.
    static RateChange read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const RateChange&) const = default;
};

/// 0xED MSG_REPORT_CREATURE_CMD (S->C)
struct ReportCreatureCmd {
    static constexpr Op kOp = Op::ReportCreatureCmd;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    std::uint8_t command{}; ///< +1 creature command GUI state (3, 4, 5 ...)

    /// Reads the fields; `r` is positioned after the opcode.
    static ReportCreatureCmd read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ReportCreatureCmd&) const = default;
};

/// 0xEF MSG_STAT_MULTIPLIERS (S->C). 17 bytes; wire order health, mana, strength, speed (SrvNet_SendStatMultipliers)
struct StatMultipliers {
    static constexpr Op kOp = Op::StatMultipliers;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 17;

    float health{}; ///< +1 class stat multipliers
    float mana{}; ///< +5
    float strength{}; ///< +9
    float speed{}; ///< +13

    /// Reads the fields; `r` is positioned after the opcode.
    static StatMultipliers read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const StatMultipliers&) const = default;
};

/// 0xF1 MSG_INVENTORY_FAIL (C->S)
struct InventoryFail {
    static constexpr Op kOp = Op::InventoryFail;
    /// Whole message including the opcode; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint16_t item{}; ///< +1 netCode of the equipped item that could not be carried; server drops it

    /// Reads the fields; `r` is positioned after the opcode.
    static InventoryFail read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const InventoryFail&) const = default;
};

} // namespace msg

namespace msg::dialog {

/// DIALOG 0x01 (C->S): start dialog. 4 bytes; ignored while the player is busy
struct StartDialog {
    static constexpr Op kOp = Op::Dialog;
    static constexpr std::uint8_t kSub = 0x01;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t netCode{}; ///< +2 NPC

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static StartDialog read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const StartDialog&) const = default;
};

/// DIALOG 0x02 (C->S): answer. 3 bytes
struct Answer {
    static constexpr Op kOp = Op::Dialog;
    static constexpr std::uint8_t kSub = 0x02;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint8_t answer{}; ///< +2 button chosen

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static Answer read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Answer&) const = default;
};

/// DIALOG 0x03 (S->C): open NPC dialog. 135 bytes
struct OpenNPCDialog {
    static constexpr Op kOp = Op::Dialog;
    static constexpr std::uint8_t kSub = 0x03;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 135;

    std::array<std::uint8_t, 32> textId{}; ///< +2 strman id, ASCII
    std::array<std::uint8_t, 64> title{}; ///< +34 wchar[32]
    std::uint32_t sound{}; ///< +98 sound id
    std::array<std::uint8_t, 32> portrait{}; ///< +102 image name, ASCII
    std::uint8_t flags{}; ///< +134 dialog flags (yes/no, next ...)

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static OpenNPCDialog read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const OpenNPCDialog&) const = default;
};

/// DIALOG 0x04 (S->C): close NPC dialog. 2 bytes
struct CloseNPCDialog {
    static constexpr Op kOp = Op::Dialog;
    static constexpr std::uint8_t kSub = 0x04;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static CloseNPCDialog read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const CloseNPCDialog&) const = default;
};

} // namespace msg::dialog

namespace msg::gauntlet {

/// GAUNTLET 0x00 (S->C): quest level start. 2 bytes; reloads the player file on a solo host
struct QuestLevelStart {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x00;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static QuestLevelStart read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const QuestLevelStart&) const = default;
};

/// GAUNTLET 0x01 (S->C): player joined quest. 4 bytes; plays sound 1008
struct PlayerJoinedQuest {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x01;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t playerId{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static PlayerJoinedQuest read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const PlayerJoinedQuest&) const = default;
};

/// GAUNTLET 0x02 (S->C): game over. 14 bytes; built by Plr_Die
struct GameOver {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x02;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 14;

    std::uint16_t generatorsDestroyed{}; ///< +2
    std::uint16_t secretsFound{}; ///< +4
    std::uint16_t kills{}; ///< +6
    std::uint16_t stage{}; ///< +8 quest stage (the player record)
    std::array<std::uint8_t, 4> unused{}; ///< +10

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static GameOver read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const GameOver&) const = default;
};

/// GAUNTLET 0x03 (C->S): respawn (observer). 2 bytes
struct RespawnObserver {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x03;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static RespawnObserver read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const RespawnObserver&) const = default;
};

/// GAUNTLET 0x04 (S->C): set lives. 5 bytes; the player info
struct SetLives {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x04;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint8_t lives{}; ///< +2
    std::uint16_t playerId{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static SetLives read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const SetLives&) const = default;
};

/// GAUNTLET 0x05 (S->C): (ignored). subs 5-10 are 4 bytes and ignored by the client
struct Unused05 {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x05;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t unused{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static Unused05 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Unused05&) const = default;
};

/// GAUNTLET 0x0B (S->C): (ignored). 16 bytes
struct Unused0B {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x0B;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 16;

    std::array<std::uint8_t, 14> unused{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static Unused0B read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Unused0B&) const = default;
};

/// GAUNTLET 0x0C (S->C): level won. 90 bytes
struct LevelWon {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x0C;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 90;

    std::uint16_t secretsTotal{}; ///< +2
    std::uint16_t stage{}; ///< +4 quest stage
    std::array<std::uint8_t, 84> players{}; ///< +6 6 x {u16 playerId; u16 generatorsDestroyed; u16 secretsFound; u16 secretsCredited (summed for the SecretsFound line); u16 kills; u32 totalScore}

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static LevelWon read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const LevelWon&) const = default;
};

/// GAUNTLET 0x0D (S->C): briefing (next level). 69 bytes
struct BriefingNextLevel {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x0D;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 69;

    std::uint16_t stage{}; ///< +2
    std::uint8_t flags{}; ///< +4 bit 0 = lock screen, bit 1 = extra flag
    std::array<std::uint8_t, 32> image{}; ///< +5 ASCII
    std::array<std::uint8_t, 32> textId{}; ///< +37 strman id; empty = default

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static BriefingNextLevel read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const BriefingNextLevel&) const = default;
};

/// GAUNTLET 0x0E (S->C): quest start briefing. 69 bytes
struct QuestStartBriefing {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x0E;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 69;

    std::uint16_t stage{}; ///< +2
    std::uint8_t flags{}; ///< +4
    std::array<std::uint8_t, 32> image{}; ///< +5
    std::array<std::uint8_t, 32> textId{}; ///< +37

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static QuestStartBriefing read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const QuestStartBriefing&) const = default;
};

/// GAUNTLET 0x0F (S->C): clear marker. 4 bytes; the drawable = 0
struct ClearMarker {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x0F;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t netCode{}; ///< +2 static object

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static ClearMarker read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ClearMarker&) const = default;
};

/// GAUNTLET 0x10 (S->C): green zap effect. 12 bytes
struct GreenZapEffect {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x10;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 12;

    std::uint16_t x1{}; ///< +2 stored with y1 in the drawable
    std::uint16_t y1{}; ///< +4
    std::uint16_t x2{}; ///< +6 spawn point
    std::uint16_t y2{}; ///< +8
    std::uint16_t duration{}; ///< +10 fade frames

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static GreenZapEffect read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const GreenZapEffect&) const = default;
};

/// GAUNTLET 0x11 (S->C): revoke spell. 4 bytes; quest death penalty: zeroes the spell level and removes it from the quickbar
struct RevokeSpell {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x11;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t spell{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static RevokeSpell read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const RevokeSpell&) const = default;
};

/// GAUNTLET 0x12 (S->C): disable ability. 4 bytes
struct DisableAbility {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x12;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t ability{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static DisableAbility read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const DisableAbility&) const = default;
};

/// GAUNTLET 0x13 (S->C): revoke field-guide creature. 4 bytes; quest death penalty
struct RevokeFieldGuideCreature {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x13;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t creature{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static RevokeFieldGuideCreature read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const RevokeFieldGuideCreature&) const = default;
};

/// GAUNTLET 0x14 (S->C): open rank window. 2 bytes; the scoreboard (guirank.c)
struct OpenRankWindow {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x14;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static OpenRankWindow read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const OpenRankWindow&) const = default;
};

/// GAUNTLET 0x15 (S->C): set player score. 8 bytes; the player info
struct SetPlayerScore {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x15;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 8;

    std::uint32_t value{}; ///< +2
    std::uint16_t playerId{}; ///< +6

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static SetPlayerScore read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const SetPlayerScore&) const = default;
};

/// GAUNTLET 0x16 (S->C): set player byte 4824. 5 bytes
struct SetPlayerByte4824 {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x16;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint8_t value{}; ///< +2
    std::uint16_t playerId{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static SetPlayerByte4824 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const SetPlayerByte4824&) const = default;
};

/// GAUNTLET 0x17 (S->C): set player byte 4825. 5 bytes
struct SetPlayerByte4825 {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x17;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint8_t value{}; ///< +2
    std::uint16_t playerId{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static SetPlayerByte4825 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const SetPlayerByte4825&) const = default;
};

/// GAUNTLET 0x18 (S->C): toggle warp GUI. 3 bytes
struct ToggleWarpGUI {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x18;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint8_t on{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static ToggleWarpGUI read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ToggleWarpGUI&) const = default;
};

/// GAUNTLET 0x19 (S->C): warp explosion FX. 7 bytes
struct WarpExplosionFX {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x19;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 7;

    std::int16_t x{}; ///< +2
    std::int16_t y{}; ///< +4
    std::uint8_t count{}; ///< +6 GreenSpark count

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static WarpExplosionFX read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const WarpExplosionFX&) const = default;
};

/// GAUNTLET 0x1A (S->C): point FX. 6 bytes
struct PointFX {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x1A;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::int16_t x{}; ///< +2
    std::int16_t y{}; ///< +4

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static PointFX read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const PointFX&) const = default;
};

/// GAUNTLET 0x1B (C->S): host: quest ready. 2 bytes
struct HostQuestReady {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x1B;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static HostQuestReady read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const HostQuestReady&) const = default;
};

/// GAUNTLET 0x1C (S->C): save transfer in progress. 3 bytes
struct SaveTransferInProgress {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x1C;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint8_t active{}; ///< +2 1 while the save file is sent, then 0

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static SaveTransferInProgress read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const SaveTransferInProgress&) const = default;
};

/// GAUNTLET 0x1D (S->C): highest quest stage. 4 bytes
struct HighestQuestStage {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x1D;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t stage{}; ///< +2 the player record

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static HighestQuestStage read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const HighestQuestStage&) const = default;
};

/// GAUNTLET 0x1E (S->C): spell awarded text. 5 bytes
struct SpellAwardedText {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x1E;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint8_t spell{}; ///< +2
    std::uint16_t playerId{}; ///< +3 you -> AwardSpell, else AwardSpellToOther

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static SpellAwardedText read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const SpellAwardedText&) const = default;
};

/// GAUNTLET 0x1F (S->C): field-guide awarded text. 5 bytes
struct FieldGuideAwardedText {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x1F;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint8_t creature{}; ///< +2
    std::uint16_t playerId{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static FieldGuideAwardedText read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FieldGuideAwardedText&) const = default;
};

/// GAUNTLET 0x20 (S->C): ability awarded text. 5 bytes
struct AbilityAwardedText {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x20;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint8_t ability{}; ///< +2
    std::uint16_t playerId{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static AbilityAwardedText read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const AbilityAwardedText&) const = default;
};

/// GAUNTLET 0x21 (S->C): formatted notice. 52 bytes
struct FormattedNotice {
    static constexpr Op kOp = Op::Gauntlet;
    static constexpr std::uint8_t kSub = 0x21;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 52;

    std::array<std::uint8_t, 49> textId{}; ///< +2 strman format id, ASCII
    std::uint8_t argIndex{}; ///< +51 0-4: which built-in string fills %s

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static FormattedNotice read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FormattedNotice&) const = default;
};

} // namespace msg::gauntlet

namespace msg::inform {

/// INFORM 0x00 (S->C): spell error. 6 bytes
struct SpellError {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x00;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint32_t index{}; ///< +2 spell / ability id

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static SpellError read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const SpellError&) const = default;
};

/// INFORM 0x01 (S->C): spell OK. 6 bytes
struct SpellOK {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x01;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint32_t index{}; ///< +2 spell / ability id

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static SpellOK read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const SpellOK&) const = default;
};

/// INFORM 0x02 (S->C): ability. 6 bytes
struct Ability {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x02;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint32_t index{}; ///< +2 spell / ability id

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static Ability read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Ability&) const = default;
};

/// INFORM 0x03 (S->C): player timed out. 6 bytes
struct PlayerTimedOut {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x03;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint32_t playerId{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static PlayerTimedOut read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const PlayerTimedOut&) const = default;
};

/// INFORM 0x04 (S->C): flag retrieved. 6 bytes
struct FlagRetrieved {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x04;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint32_t playerId{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static FlagRetrieved read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FlagRetrieved&) const = default;
};

/// INFORM 0x05 (S->C): flag captured. 10 bytes
struct FlagCaptured {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x05;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 10;

    std::uint32_t playerId{}; ///< +2
    std::uint32_t teamId{}; ///< +6

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static FlagCaptured read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FlagCaptured&) const = default;
};

/// INFORM 0x06 (S->C): flag picked up. 10 bytes
struct FlagPickedUp {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x06;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 10;

    std::uint32_t playerId{}; ///< +2
    std::uint32_t teamId{}; ///< +6

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static FlagPickedUp read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FlagPickedUp&) const = default;
};

/// INFORM 0x07 (S->C): flag dropped. 10 bytes
struct FlagDropped {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x07;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 10;

    std::uint32_t playerId{}; ///< +2
    std::uint32_t teamId{}; ///< +6

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static FlagDropped read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FlagDropped&) const = default;
};

/// INFORM 0x08 (S->C): flag respawned. 6 bytes
struct FlagRespawned {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x08;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint32_t teamId{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static FlagRespawned read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const FlagRespawned&) const = default;
};

/// INFORM 0x09 (S->C): ball scored. 10 bytes
struct BallScored {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x09;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 10;

    std::uint32_t playerId{}; ///< +2
    std::uint32_t teamId{}; ///< +6

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static BallScored read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const BallScored&) const = default;
};

/// INFORM 0x0A (S->C): crown picked up. 10 bytes
struct CrownPickedUp {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x0A;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 10;

    std::uint32_t playerId{}; ///< +2
    std::uint32_t teamId{}; ///< +6

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static CrownPickedUp read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const CrownPickedUp&) const = default;
};

/// INFORM 0x0B (S->C): crown dropped. 10 bytes
struct CrownDropped {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x0B;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 10;

    std::uint32_t playerId{}; ///< +2
    std::uint32_t teamId{}; ///< +6

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static CrownDropped read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const CrownDropped&) const = default;
};

/// INFORM 0x0C (S->C): observer mode. 6 bytes
struct ObserverMode {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x0C;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint32_t canJoin{}; ///< +2 non-zero adds "press jump"

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static ObserverMode read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ObserverMode&) const = default;
};

/// INFORM 0x0D (S->C): player state. 6 bytes
struct PlayerState {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x0D;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint32_t state{}; ///< +2 0 stunned, 1 confused, 2 poisoned, 3 overweight

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static PlayerState read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const PlayerState&) const = default;
};

/// INFORM 0x0E (S->C): kill feed. 11 bytes
struct KillFeed {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x0E;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 11;

    std::uint16_t attacker{}; ///< +2 playerId
    std::uint16_t assist{}; ///< +4 playerId
    std::uint16_t victim{}; ///< +6 playerId
    std::uint16_t weapon{}; ///< +8 thing type (kind 1) or damage type (kind 2)
    std::uint8_t kind{}; ///< +10

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static KillFeed read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const KillFeed&) const = default;
};

/// INFORM 0x0F (S->C): string. 3 + strlen + 1 bytes
struct String {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x0F;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = kVariableSize;

    std::uint8_t isSign{}; ///< +2 1 = "sign says:" prefix
    std::vector<std::uint8_t> textId{}; ///< +3 ASCII strman id, NUL-terminated

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static String read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const String&) const = default;
};

/// INFORM 0x10 (S->C): wrong team. 6 bytes
struct WrongTeam {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x10;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint32_t teamId{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static WrongTeam read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const WrongTeam&) const = default;
};

/// INFORM 0x11 (S->C): class changed. 2 bytes
struct ClassChanged {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x11;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static ClassChanged read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ClassChanged&) const = default;
};

/// INFORM 0x12 (S->C): player exited. 6 bytes
struct PlayerExited {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x12;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint32_t playerId{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static PlayerExited read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const PlayerExited&) const = default;
};

/// INFORM 0x13 (S->C): player warped. 6 bytes
struct PlayerWarped {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x13;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint32_t playerId{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static PlayerWarped read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const PlayerWarped&) const = default;
};

/// INFORM 0x14 (S->C): player found secret. 6 bytes
struct PlayerFoundSecret {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x14;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint32_t playerId{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static PlayerFoundSecret read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const PlayerFoundSecret&) const = default;
};

/// INFORM 0x15 (S->C): warp wait. 6 bytes
struct WarpWait {
    static constexpr Op kOp = Op::Inform;
    static constexpr std::uint8_t kSub = 0x15;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint32_t stage{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static WarpWait read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const WarpWait&) const = default;
};

} // namespace msg::inform

namespace msg::team {

/// TEAM 0x00 (S->C): create/update team. 18 + 2xnameLen bytes
struct CreateUpdateTeam {
    static constexpr Op kOp = Op::TeamMsg;
    static constexpr std::uint8_t kSub = 0x00;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = kVariableSize;

    std::uint32_t teamId{}; ///< +2
    std::uint32_t field60{}; ///< +6 the team record (set from the creator's player info)
    std::uint32_t score{}; ///< +10 the team record
    std::uint8_t flags{}; ///< +14 bit 0 = game flag 0x200 (co-op team)
    std::uint8_t nameLen{}; ///< +15 UTF-16 chars
    std::uint8_t color{}; ///< +16 the team record
    std::uint8_t useTitle{}; ///< +17 the team record
    std::vector<std::uint8_t> name{}; ///< +18 nameLen x UTF-16

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static CreateUpdateTeam read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const CreateUpdateTeam&) const = default;
};

/// TEAM 0x01 (S->C): add member. 10 bytes
struct AddMember {
    static constexpr Op kOp = Op::TeamMsg;
    static constexpr std::uint8_t kSub = 0x01;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 10;

    std::uint32_t teamId{}; ///< +2
    std::uint16_t netCode{}; ///< +6
    std::uint16_t typeId{}; ///< +8

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static AddMember read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const AddMember&) const = default;
};

/// TEAM 0x02 (S->C): remove from team. 6 bytes
struct RemoveFromTeam {
    static constexpr Op kOp = Op::TeamMsg;
    static constexpr std::uint8_t kSub = 0x02;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint32_t netCode{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static RemoveFromTeam read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const RemoveFromTeam&) const = default;
};

/// TEAM 0x03 (S->C): change team. 10 bytes
struct ChangeTeam {
    static constexpr Op kOp = Op::TeamMsg;
    static constexpr std::uint8_t kSub = 0x03;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 10;

    std::uint32_t teamId{}; ///< +2
    std::uint16_t netCode{}; ///< +6
    std::array<std::uint8_t, 2> pad{}; ///< +8

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static ChangeTeam read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ChangeTeam&) const = default;
};

/// TEAM 0x04 (S->C): rename team. 46 bytes
struct RenameTeam {
    static constexpr Op kOp = Op::TeamMsg;
    static constexpr std::uint8_t kSub = 0x04;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 46;

    std::uint32_t teamId{}; ///< +2
    std::array<std::uint8_t, 40> name{}; ///< +6 wchar[20]

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static RenameTeam read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const RenameTeam&) const = default;
};

/// TEAM 0x05 (S->C): remove all members. 6 bytes
struct RemoveAllMembers {
    static constexpr Op kOp = Op::TeamMsg;
    static constexpr std::uint8_t kSub = 0x05;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint32_t teamId{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static RemoveAllMembers read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const RemoveAllMembers&) const = default;
};

/// TEAM 0x06 (S->C): remove team. 6 bytes
struct RemoveTeam {
    static constexpr Op kOp = Op::TeamMsg;
    static constexpr std::uint8_t kSub = 0x06;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint32_t teamId{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static RemoveTeam read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const RemoveTeam&) const = default;
};

/// TEAM 0x07 (S->C): remove all teams. 2 bytes
struct RemoveAllTeams {
    static constexpr Op kOp = Op::TeamMsg;
    static constexpr std::uint8_t kSub = 0x07;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static RemoveAllTeams read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const RemoveAllTeams&) const = default;
};

/// TEAM 0x08 (S->C): set team score. 10 bytes
struct SetTeamScore {
    static constexpr Op kOp = Op::TeamMsg;
    static constexpr std::uint8_t kSub = 0x08;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 10;

    std::uint32_t teamId{}; ///< +2
    std::uint32_t score{}; ///< +6 the team record

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static SetTeamScore read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const SetTeamScore&) const = default;
};

/// TEAM 0x09 (S->C): reset all team scores. 2 bytes
struct ResetAllTeamScores {
    static constexpr Op kOp = Op::TeamMsg;
    static constexpr std::uint8_t kSub = 0x09;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static ResetAllTeamScores read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ResetAllTeamScores&) const = default;
};

/// TEAM 0x0A (C->S): request join team. 10 bytes
struct RequestJoinTeam {
    static constexpr Op kOp = Op::TeamMsg;
    static constexpr std::uint8_t kSub = 0x0A;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 10;

    std::uint32_t teamId{}; ///< +2
    std::uint16_t netCode{}; ///< +6
    std::array<std::uint8_t, 2> pad{}; ///< +8

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static RequestJoinTeam read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const RequestJoinTeam&) const = default;
};

/// TEAM 0x0B (C->S): switch team. 10 bytes; server moves the player to a start point and calls Team_MoveMember
struct SwitchTeam {
    static constexpr Op kOp = Op::TeamMsg;
    static constexpr std::uint8_t kSub = 0x0B;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 10;

    std::uint32_t teamId{}; ///< +2
    std::uint16_t netCode{}; ///< +6
    std::array<std::uint8_t, 2> pad{}; ///< +8

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static SwitchTeam read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const SwitchTeam&) const = default;
};

/// TEAM 0x0C (S->C): member health. 5 bytes; quest mode
struct MemberHealth {
    static constexpr Op kOp = Op::TeamMsg;
    static constexpr std::uint8_t kSub = 0x0C;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint16_t netCode{}; ///< +2
    std::uint8_t healthPct{}; ///< +4 health x 100 / maxHealth; client stores it in the player info

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static MemberHealth read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const MemberHealth&) const = default;
};

} // namespace msg::team

namespace msg::trade {

/// TRADE 0x01 (S->C): close P2P trade window. 2 bytes
struct CloseP2PTradeWindow {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x01;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static CloseP2PTradeWindow read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const CloseP2PTradeWindow&) const = default;
};

/// TRADE 0x02 (S->C): close shop window. 2 bytes
struct CloseShopWindow {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x02;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static CloseShopWindow read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const CloseShopWindow&) const = default;
};

/// TRADE 0x03 (S->C): accept state. 3 bytes
struct AcceptState {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x03;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint8_t accepted{}; ///< +2 bit 0 = you accepted, bit 1 = partner accepted

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static AcceptState read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const AcceptState&) const = default;
};

/// TRADE 0x04 (S->C): add item to trade. 15 bytes
struct AddItemToTrade {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x04;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 15;

    std::uint8_t side{}; ///< +2 1 = your offer, else partner
    std::uint16_t typeId{}; ///< +3
    std::uint16_t netCode{}; ///< +5
    std::uint32_t value{}; ///< +7 added to the side total
    std::array<std::uint8_t, 4> modifiers{}; ///< +11 enchant ids, 0xFF = none

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static AddItemToTrade read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const AddItemToTrade&) const = default;
};

/// TRADE 0x05 (S->C): remove item from trade. 4 bytes
struct RemoveItemFromTrade {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x05;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t netCode{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static RemoveItemFromTrade read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const RemoveItemFromTrade&) const = default;
};

/// TRADE 0x06 (S->C): gold values. 14 bytes
struct GoldValues {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x06;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 14;

    std::uint32_t gold1{}; ///< +2 shown in window control 3711
    std::uint32_t gold2{}; ///< +6 control 3712, shown as "(n)"; blank when 0
    std::uint32_t gold3{}; ///< +10 control 3713

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static GoldValues read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const GoldValues&) const = default;
};

/// TRADE 0x07 (S->C): prepare P2P trade. 2 bytes
struct PrepareP2PTrade {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x07;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static PrepareP2PTrade read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const PrepareP2PTrade&) const = default;
};

/// TRADE 0x08 (S->C): shop: add item. 18 bytes
struct ShopAddItem {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x08;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 18;

    std::uint16_t typeId{}; ///< +2
    std::uint16_t netCode{}; ///< +4
    std::uint32_t price{}; ///< +6
    std::uint32_t durability{}; ///< +10 low 16 bits -> Drawable health cur/max
    std::array<std::uint8_t, 4> modifiers{}; ///< +14 0xFF = none

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static ShopAddItem read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ShopAddItem&) const = default;
};

/// TRADE 0x09 (S->C): shop: remove item. 4 bytes
struct ShopRemoveItem {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x09;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t netCode{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static ShopRemoveItem read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ShopRemoveItem&) const = default;
};

/// TRADE 0x0C (S->C): start P2P trade. 52 bytes
struct StartP2PTrade {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x0C;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 52;

    std::array<std::uint8_t, 50> partnerName{}; ///< +2 wchar[25]

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static StartP2PTrade read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const StartP2PTrade&) const = default;
};

/// TRADE 0x0D (S->C): start shop. 86 bytes
struct StartShop {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x0D;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 86;

    std::uint16_t shopkeeperType{}; ///< +2 typeId; selects the shop picture
    std::array<std::uint8_t, 50> shopName{}; ///< +4 wchar[25]
    std::array<std::uint8_t, 32> greeting{}; ///< +54 strman id / text, ASCII

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static StartShop read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const StartShop&) const = default;
};

/// TRADE 0x0E (C->S): cancel P2P trade. 2 bytes
struct CancelP2PTrade {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x0E;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static CancelP2PTrade read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const CancelP2PTrade&) const = default;
};

/// TRADE 0x0F (C->S): offer item. 4 bytes
struct OfferItem {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x0F;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t item{}; ///< +2 netCode from your inventory

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static OfferItem read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const OfferItem&) const = default;
};

/// TRADE 0x10 (C->S): withdraw item. 4 bytes
struct WithdrawItem {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x10;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t item{}; ///< +2 returned to your inventory

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static WithdrawItem read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const WithdrawItem&) const = default;
};

/// TRADE 0x11 (C->S): accept trade. 2 bytes
struct AcceptTrade {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x11;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static AcceptTrade read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const AcceptTrade&) const = default;
};

/// TRADE 0x12 (C->S): exit shop. 2 bytes
struct ExitShop {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x12;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static ExitShop read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const ExitShop&) const = default;
};

/// TRADE 0x15 (C->S): open shop. 4 bytes
struct OpenShop {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x15;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t shopkeeper{}; ///< +2 netCode; object must have class flag 8

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static OpenShop read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const OpenShop&) const = default;
};

/// TRADE 0x16 (C->S): buy item. 4 bytes
struct BuyItem {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x16;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t item{}; ///< +2 shop item netCode

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static BuyItem read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const BuyItem&) const = default;
};

/// TRADE 0x17 (C->S): buy stack. 5 bytes. 1.2b bug: gold is read once, so a player who can afford one item gets the whole stack
struct BuyStack {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x17;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint16_t typeId{}; ///< +2 matched by type, not netCode
    std::uint8_t count{}; ///< +4

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static BuyStack read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const BuyStack&) const = default;
};

/// TRADE 0x18 (C->S): sell item. 4 bytes
struct SellItem {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x18;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t item{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static SellItem read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const SellItem&) const = default;
};

/// TRADE 0x19 (C->S): sell stack. 5 bytes; skips the quest-item/glyph refusals of single sell
struct SellStack {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x19;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 5;

    std::uint16_t typeId{}; ///< +2 matched by type, not netCode
    std::uint8_t count{}; ///< +4

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static SellStack read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const SellStack&) const = default;
};

/// TRADE 0x1A (C->S): repair / recharge item. 4 bytes
struct RepairRechargeItem {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x1A;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t item{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static RepairRechargeItem read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const RepairRechargeItem&) const = default;
};

/// TRADE 0x1B (S->C): not enough gold. 4 bytes
struct NotEnoughGold {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x1B;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t shortBy{}; ///< +2 gold missing

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static NotEnoughGold read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const NotEnoughGold&) const = default;
};

/// TRADE 0x1C (C->S): ask sell price. 4 bytes; server replies 0x1D
struct AskSellPrice {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x1C;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t item{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static AskSellPrice read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const AskSellPrice&) const = default;
};

/// TRADE 0x1D (S->C): sell price offer. 8 bytes; reply to C->S 0x1C
struct SellPriceOffer {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x1D;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 8;

    std::uint16_t item{}; ///< +2 netCode
    std::uint32_t price{}; ///< +4

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static SellPriceOffer read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const SellPriceOffer&) const = default;
};

/// TRADE 0x1E (C->S): ask repair cost. 4 bytes; server replies 0x1F
struct AskRepairCost {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x1E;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint16_t item{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static AskRepairCost read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const AskRepairCost&) const = default;
};

/// TRADE 0x1F (S->C): repair cost offer. 8 bytes; reply to C->S 0x1E
struct RepairCostOffer {
    static constexpr Op kOp = Op::Trade;
    static constexpr std::uint8_t kSub = 0x1F;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 8;

    std::uint16_t item{}; ///< +2 netCode
    std::uint32_t cost{}; ///< +4

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static RepairCostOffer read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const RepairCostOffer&) const = default;
};

} // namespace msg::trade

namespace msg::vote {

/// VOTE 0x00 (C->S): vote against player. 52 bytes; vote kind 0 (3 in quest)
struct VoteAgainstPlayer {
    static constexpr Op kOp = Op::Vote;
    static constexpr std::uint8_t kSub = 0x00;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 52;

    std::array<std::uint8_t, 50> playerName{}; ///< +2 wchar[25]

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static VoteAgainstPlayer read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const VoteAgainstPlayer&) const = default;
};

/// VOTE 0x01 (C->S): vote (kind 1) against player. 52 bytes
struct VoteKind1AgainstPlayer {
    static constexpr Op kOp = Op::Vote;
    static constexpr std::uint8_t kSub = 0x01;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 52;

    std::array<std::uint8_t, 50> playerName{}; ///< +2 wchar[25]

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static VoteKind1AgainstPlayer read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const VoteKind1AgainstPlayer&) const = default;
};

/// VOTE 0x02 (C->S): withdraw vote against player. 52 bytes
struct WithdrawVoteAgainstPlayer {
    static constexpr Op kOp = Op::Vote;
    static constexpr std::uint8_t kSub = 0x02;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 52;

    std::array<std::uint8_t, 50> playerName{}; ///< +2 wchar[25]

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static WithdrawVoteAgainstPlayer read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const WithdrawVoteAgainstPlayer&) const = default;
};

/// VOTE 0x03 (C->S): withdraw vote (kind 1). 52 bytes
struct WithdrawVoteKind1 {
    static constexpr Op kOp = Op::Vote;
    static constexpr std::uint8_t kSub = 0x03;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 52;

    std::array<std::uint8_t, 50> playerName{}; ///< +2 wchar[25]

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static WithdrawVoteKind1 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const WithdrawVoteKind1&) const = default;
};

/// VOTE 0x04 (C->S): vote (kind 2, no target). 2 bytes; sent when the kick-window checkbox is toggled
struct VoteKind2NoTarget {
    static constexpr Op kOp = Op::Vote;
    static constexpr std::uint8_t kSub = 0x04;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static VoteKind2NoTarget read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const VoteKind2NoTarget&) const = default;
};

/// VOTE 0x05 (C->S): withdraw vote (kind 2). 2 bytes; sent when the kick-window checkbox is toggled back
struct WithdrawVoteKind2 {
    static constexpr Op kOp = Op::Vote;
    static constexpr std::uint8_t kSub = 0x05;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static WithdrawVoteKind2 read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const WithdrawVoteKind2&) const = default;
};

/// VOTE 0x06 (S->C): vote state. 3 bytes
struct VoteState {
    static constexpr Op kOp = Op::Vote;
    static constexpr std::uint8_t kSub = 0x06;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint8_t state{}; ///< +2 vote GUI state

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static VoteState read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const VoteState&) const = default;
};

/// VOTE 0x07 (S->C): vote reset. 2 bytes
struct VoteReset {
    static constexpr Op kOp = Op::Vote;
    static constexpr std::uint8_t kSub = 0x07;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 2;

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static VoteReset read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const VoteReset&) const = default;
};

} // namespace msg::vote

namespace msg::xfer {

/// XFER 0x00 (both): start. 140 bytes
struct Start {
    static constexpr Op kOp = Op::XferMsg;
    static constexpr std::uint8_t kSub = 0x00;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 140;

    std::uint8_t action{}; ///< +2 1 MOTD, 2 SAVEDATA, 3 SAVE_SERVER (OpenNox: 10 = CBOR extension)
    std::uint8_t unk{}; ///< +3
    std::uint32_t size{}; ///< +4 total bytes
    std::array<std::uint8_t, 128> type{}; ///< +8 ASCII type name, e.g. "MOTD", "SAVEDATA"
    std::uint8_t sendId{}; ///< +136 sender stream id
    std::array<std::uint8_t, 3> unk2{}; ///< +137

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static Start read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Start&) const = default;
};

/// XFER 0x01 (both): accept. 4 bytes
struct Accept {
    static constexpr Op kOp = Op::XferMsg;
    static constexpr std::uint8_t kSub = 0x01;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint8_t recvId{}; ///< +2 receiver stream id
    std::uint8_t sendId{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static Accept read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Accept&) const = default;
};

/// XFER 0x02 (both): data. 8 + size bytes
struct Data {
    static constexpr Op kOp = Op::XferMsg;
    static constexpr std::uint8_t kSub = 0x02;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = kVariableSize;

    std::uint8_t recvId{}; ///< +2
    std::uint8_t token{}; ///< +3
    std::uint16_t chunk{}; ///< +4 block index (512-byte blocks)
    std::uint16_t size{}; ///< +6
    std::vector<std::uint8_t> data{}; ///< +8 size bytes

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static Data read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Data&) const = default;
};

/// XFER 0x03 (both): ack. 6 bytes
struct Ack {
    static constexpr Op kOp = Op::XferMsg;
    static constexpr std::uint8_t kSub = 0x03;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 6;

    std::uint8_t recvId{}; ///< +2
    std::uint8_t token{}; ///< +3
    std::uint16_t chunk{}; ///< +4

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static Ack read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Ack&) const = default;
};

/// XFER 0x04 (both): done. 3 bytes
struct Done {
    static constexpr Op kOp = Op::XferMsg;
    static constexpr std::uint8_t kSub = 0x04;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 3;

    std::uint8_t recvId{}; ///< +2

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static Done read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Done&) const = default;
};

/// XFER 0x05 (both): cancel. 4 bytes
struct Cancel {
    static constexpr Op kOp = Op::XferMsg;
    static constexpr std::uint8_t kSub = 0x05;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint8_t recvId{}; ///< +2
    std::uint8_t reason{}; ///< +3 1 cancelled, 2 send timeout, 3 recv timeout

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static Cancel read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Cancel&) const = default;
};

/// XFER 0x06 (both): abort. 4 bytes
struct Abort {
    static constexpr Op kOp = Op::XferMsg;
    static constexpr std::uint8_t kSub = 0x06;
    /// Whole message including the opcode and sub-op; kVariableSize if it has a variable tail.
    static constexpr std::size_t kSize = 4;

    std::uint8_t recvId{}; ///< +2
    std::uint8_t reason{}; ///< +3

    /// Reads the fields; `r` is positioned after the opcode and sub-op.
    static Abort read(ByteReader& r);
    /// Writes the whole message, opcode first.
    void write(ByteWriter& w) const;

    bool operator==(const Abort&) const = default;
};

} // namespace msg::xfer

template <class... Ts>
struct TypeList {};

/// Every message struct above.
using AllMessages = TypeList<
    msg::ServerConnect,
    msg::ServerAccept,
    msg::Code2,
    msg::Code3,
    msg::Code4,
    msg::Code5,
    msg::ClientPing,
    msg::Code7,
    msg::ClientPong,
    msg::Code9,
    msg::ClientClose,
    msg::ServerClose,
    msg::ServerDiscover,
    msg::ServerInfo,
    msg::ServerTryJoin,
    msg::PasswordRequired,
    msg::ServerPing,
    msg::ServerPassword,
    msg::ServerPong,
    msg::ServerError,
    msg::ServerJoinOk,
    msg::ServerJoinFail,
    msg::Code22,
    msg::Code23,
    msg::Code24,
    msg::Code25,
    msg::Code26,
    msg::Code27,
    msg::Code28,
    msg::Code29,
    msg::Code30,
    msg::Accepted,
    msg::ClientAccept,
    msg::ServerCloseAck,
    msg::ClientCloseAck,
    msg::Speed,
    msg::Ping,
    msg::Code37,
    msg::Code38,
    msg::Timestamp,
    msg::FullTimestamp,
    msg::NeedTimestamp,
    msg::SimulatedTimestamp,
    msg::UseMap,
    msg::JoinData,
    msg::NewPlayer,
    msg::PlayerQuit,
    msg::SimpleObj,
    msg::ComplexObj,
    msg::DestroyObject,
    msg::ObjectOutOfSight,
    msg::ObjectInShadows,
    msg::ObjectFriendAdd,
    msg::ObjectFriendRemove,
    msg::ResetFriends,
    msg::EnableObject,
    msg::DisableObject,
    msg::DrawFrame,
    msg::DestroyWall,
    msg::OpenWall,
    msg::CloseWall,
    msg::ChangeOrAddWallMagic,
    msg::RemoveWallMagic,
    msg::PlayerInput,
    msg::PlayerSetWaypoint,
    msg::ReportHealth,
    msg::ReportHealthDelta,
    msg::ReportPlayerHealth,
    msg::ReportItemHealth,
    msg::ReportMana,
    msg::ReportPoison,
    msg::ReportStamina,
    msg::ReportStats,
    msg::ReportArmorValue,
    msg::ReportGold,
    msg::ReportPickup,
    msg::ReportModifiablePickup,
    msg::ReportDrop,
    msg::ReportLesson,
    msg::ReportMundaneArmorEquip,
    msg::ReportMundaneWeaponEquip,
    msg::ReportModifiableWeaponEquip,
    msg::ReportModifiableArmorEquip,
    msg::ReportArmorDequip,
    msg::ReportWeaponDequip,
    msg::ReportTreasureCount,
    msg::ReportFlagBallWinner,
    msg::ReportFlagWinner,
    msg::ReportDeathmatchWinner,
    msg::ReportDeathmatchTeamWinner,
    msg::ReportEnchantment,
    msg::ReportItemEnchantment,
    msg::ReportLightColor,
    msg::ReportLightIntensity,
    msg::ReportZPlus,
    msg::ReportZMinus,
    msg::ReportEquip,
    msg::ReportDequip,
    msg::ReportAcquireSpell,
    msg::ReportTarget,
    msg::ReportCharges,
    msg::ReportXStatus,
    msg::ReportPlayerStatus,
    msg::ReportModifier,
    msg::ReportStatModifier,
    msg::ReportNpc,
    msg::ReportClientStatus,
    msg::ReportAnimationFrame,
    msg::ReportAcquireCreature,
    msg::ReportLoseCreature,
    msg::ReportExperience,
    msg::ReportSpellAward,
    msg::ReportSpellStart,
    msg::ReportInventoryLoaded,
    msg::TryDrop,
    msg::TryGet,
    msg::TryUse,
    msg::TryEquip,
    msg::TryDequip,
    msg::TryTarget,
    msg::TryCreatureCommand,
    msg::TrySpell,
    msg::TryAbility,
    msg::TryCollide,
    msg::FxParticlefx,
    msg::FxPlasma,
    msg::FxSummon,
    msg::FxSummonCancel,
    msg::FxShield,
    msg::FxBlueSparks,
    msg::FxYellowSparks,
    msg::FxCyanSparks,
    msg::FxVioletSparks,
    msg::FxExplosion,
    msg::FxLesserExplosion,
    msg::FxCounterspellExplosion,
    msg::FxThinExplosion,
    msg::FxTeleport,
    msg::FxSmokeBlast,
    msg::FxDamagePoof,
    msg::FxLightning,
    msg::FxEnergyBolt,
    msg::FxChainLightningBolt,
    msg::FxDrainMana,
    msg::FxCharm,
    msg::FxGreaterHeal,
    msg::FxMagic,
    msg::FxSparkExplosion,
    msg::FxDeathRay,
    msg::FxSentryRay,
    msg::FxRicochet,
    msg::FxJiggle,
    msg::FxGreenBolt,
    msg::FxGreenExplosion,
    msg::FxWhiteFlash,
    msg::FxGeneratingMap,
    msg::FxAssemblingMap,
    msg::FxPopulatingMap,
    msg::FxDurationSpell,
    msg::FxDeltazSpellStart,
    msg::FxTurnUndead,
    msg::FxArrowTrap,
    msg::FxVampirism,
    msg::FxManaBombCancel,
    msg::UpdateStream,
    msg::NewAlias,
    msg::AudioEvent,
    msg::AudioPlayerEvent,
    msg::TextMessage,
    msg::Important,
    msg::ImportantAck,
    msg::Mouse,
    msg::IncomingClient,
    msg::OutgoingClient,
    msg::GameSettings,
    msg::GameSettings2,
    msg::UpdateGuiGameSettings,
    msg::DoorAngle,
    msg::ObeliskCharge,
    msg::PentagramActivate,
    msg::ClientPredictLinear,
    msg::RequestMap,
    msg::CancelMap,
    msg::MapSendStart,
    msg::MapSendPacket,
    msg::MapSendAbort,
    msg::ServerCmd,
    msg::SysopPw,
    msg::SysopResult,
    msg::KeepAlive,
    msg::ReceivedMap,
    msg::ClientReady,
    msg::RequestSavePlayer,
    msg::PlayerObj,
    msg::KickNotification,
    msg::TimeoutNotification,
    msg::ServerQuit,
    msg::ServerQuitAck,
    msg::ChatKill,
    msg::MessagesKill,
    msg::SeqImportant,
    msg::ReportAbilityAward,
    msg::ReportAbilityState,
    msg::ReportActiveAbilities,
    msg::ReportGuideAward,
    msg::InterestingId,
    msg::TimerStatus,
    msg::RequestTimerStatus,
    msg::JournalMsg,
    msg::ChapterEnd,
    msg::ReportAllLatency,
    msg::ReportFlagStatus,
    msg::ReportBallStatus,
    msg::ReportObjectPoison,
    msg::ReportMonitorCreature,
    msg::ReportUnmonitorCreature,
    msg::ReportTotalHealth,
    msg::ReportTotalMana,
    msg::ReportSpellStat,
    msg::ReportSecondaryWeapon,
    msg::ReportLastQuiver,
    msg::InfoBookData,
    msg::Social,
    msg::FadeBegin,
    msg::MusicEvent,
    msg::MusicPushEvent,
    msg::MusicPopEvent,
    msg::PlayerDied,
    msg::PlayerRespawn,
    msg::ForgetDrawables,
    msg::ResetAbilities,
    msg::RateChange,
    msg::ReportCreatureCmd,
    msg::StatMultipliers,
    msg::InventoryFail,
    msg::inform::SpellError,
    msg::inform::SpellOK,
    msg::inform::Ability,
    msg::inform::PlayerTimedOut,
    msg::inform::FlagRetrieved,
    msg::inform::FlagCaptured,
    msg::inform::FlagPickedUp,
    msg::inform::FlagDropped,
    msg::inform::FlagRespawned,
    msg::inform::BallScored,
    msg::inform::CrownPickedUp,
    msg::inform::CrownDropped,
    msg::inform::ObserverMode,
    msg::inform::PlayerState,
    msg::inform::KillFeed,
    msg::inform::String,
    msg::inform::WrongTeam,
    msg::inform::ClassChanged,
    msg::inform::PlayerExited,
    msg::inform::PlayerWarped,
    msg::inform::PlayerFoundSecret,
    msg::inform::WarpWait,
    msg::xfer::Start,
    msg::xfer::Accept,
    msg::xfer::Data,
    msg::xfer::Ack,
    msg::xfer::Done,
    msg::xfer::Cancel,
    msg::xfer::Abort,
    msg::team::CreateUpdateTeam,
    msg::team::AddMember,
    msg::team::RemoveFromTeam,
    msg::team::ChangeTeam,
    msg::team::RenameTeam,
    msg::team::RemoveAllMembers,
    msg::team::RemoveTeam,
    msg::team::RemoveAllTeams,
    msg::team::SetTeamScore,
    msg::team::ResetAllTeamScores,
    msg::team::RequestJoinTeam,
    msg::team::SwitchTeam,
    msg::team::MemberHealth,
    msg::trade::CloseP2PTradeWindow,
    msg::trade::CloseShopWindow,
    msg::trade::AcceptState,
    msg::trade::AddItemToTrade,
    msg::trade::RemoveItemFromTrade,
    msg::trade::GoldValues,
    msg::trade::PrepareP2PTrade,
    msg::trade::ShopAddItem,
    msg::trade::ShopRemoveItem,
    msg::trade::StartP2PTrade,
    msg::trade::StartShop,
    msg::trade::CancelP2PTrade,
    msg::trade::OfferItem,
    msg::trade::WithdrawItem,
    msg::trade::AcceptTrade,
    msg::trade::ExitShop,
    msg::trade::OpenShop,
    msg::trade::BuyItem,
    msg::trade::BuyStack,
    msg::trade::SellItem,
    msg::trade::SellStack,
    msg::trade::RepairRechargeItem,
    msg::trade::NotEnoughGold,
    msg::trade::AskSellPrice,
    msg::trade::SellPriceOffer,
    msg::trade::AskRepairCost,
    msg::trade::RepairCostOffer,
    msg::dialog::StartDialog,
    msg::dialog::Answer,
    msg::dialog::OpenNPCDialog,
    msg::dialog::CloseNPCDialog,
    msg::vote::VoteAgainstPlayer,
    msg::vote::VoteKind1AgainstPlayer,
    msg::vote::WithdrawVoteAgainstPlayer,
    msg::vote::WithdrawVoteKind1,
    msg::vote::VoteKind2NoTarget,
    msg::vote::WithdrawVoteKind2,
    msg::vote::VoteState,
    msg::vote::VoteReset,
    msg::gauntlet::QuestLevelStart,
    msg::gauntlet::PlayerJoinedQuest,
    msg::gauntlet::GameOver,
    msg::gauntlet::RespawnObserver,
    msg::gauntlet::SetLives,
    msg::gauntlet::Unused05,
    msg::gauntlet::Unused0B,
    msg::gauntlet::LevelWon,
    msg::gauntlet::BriefingNextLevel,
    msg::gauntlet::QuestStartBriefing,
    msg::gauntlet::ClearMarker,
    msg::gauntlet::GreenZapEffect,
    msg::gauntlet::RevokeSpell,
    msg::gauntlet::DisableAbility,
    msg::gauntlet::RevokeFieldGuideCreature,
    msg::gauntlet::OpenRankWindow,
    msg::gauntlet::SetPlayerScore,
    msg::gauntlet::SetPlayerByte4824,
    msg::gauntlet::SetPlayerByte4825,
    msg::gauntlet::ToggleWarpGUI,
    msg::gauntlet::WarpExplosionFX,
    msg::gauntlet::PointFX,
    msg::gauntlet::HostQuestReady,
    msg::gauntlet::SaveTransferInProgress,
    msg::gauntlet::HighestQuestStage,
    msg::gauntlet::SpellAwardedText,
    msg::gauntlet::FieldGuideAwardedText,
    msg::gauntlet::AbilityAwardedText,
    msg::gauntlet::FormattedNotice>;

} // namespace nox::net
