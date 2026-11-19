// The map editor's op labels: every template an Op's label is made from, as data, and the label
// shown in the player's language.
//
// An Op's label is ENGLISH where it is made: it travels in the op's bytes (serializeOp) to a
// session's other editors, which may run another language or an older build, and into the host's
// log. Its text is made from a template here with opLabel; what is drawn (the History window, the
// status line, the session's messages) goes through shownOpLabel, which finds the template the
// English came from and fills that template's translation with the same values. A label no
// template matches (an older or newer build's) is shown as it is.
//
// A template's {n} is a count (a number); a template made with NOX_TRPK is chosen by it (English's
// rule where the label is made, the language's where it is shown). Some values are themselves
// texts of this table ("Edit {polygon}" with "polygon 3", "Paint {floor} ({tiles})" with "5
// tiles"): shown, they are translated too (the placeholders kNestedNames lists).
#pragma once

#include "noxcore/strings/l10n.hpp"

#include <initializer_list>
#include <span>
#include <string>
#include <string_view>

namespace nox::mapedit {

namespace oplabels {
using nox::l10n::Key;
using nox::l10n::PluralKey;

// --- Waypoints
inline constexpr Key kPlaceWaypoint = NOX_TRK("editor.op.place_waypoint", "Place waypoint {id}");
inline constexpr Key kPlaceWaypointLinked = NOX_TRK("editor.op.place_waypoint_linked", "Place waypoint {id} linked from {from}");
inline constexpr Key kLinkWaypointsBoth = NOX_TRK("editor.op.link_waypoints_both", "Link both ways waypoint {from} to {to}");
inline constexpr Key kLinkWaypoints = NOX_TRK("editor.op.link_waypoints", "Link waypoint {from} to {to}");
inline constexpr Key kUnlinkWaypoints = NOX_TRK("editor.op.unlink_waypoints", "Unlink waypoint {from} from {to}");
inline constexpr Key kSetLinkFlags = NOX_TRK("editor.op.set_link_flags", "Set link flags {from} to {to} to {flags}");
inline constexpr PluralKey kEnableWaypoints = NOX_TRPK("editor.op.enable_waypoints", "Enable {n} waypoint", "Enable {n} waypoints");
inline constexpr PluralKey kDisableWaypoints = NOX_TRPK("editor.op.disable_waypoints", "Disable {n} waypoint", "Disable {n} waypoints");
inline constexpr Key kEditWaypoint = NOX_TRK("editor.op.edit_waypoint", "Edit waypoint {id}");
inline constexpr PluralKey kSetWaypointFlag = NOX_TRPK("editor.op.set_waypoint_flag", "Set flag {flag} on {n} waypoint", "Set flag {flag} on {n} waypoints");
inline constexpr PluralKey kClearWaypointFlag =
    NOX_TRPK("editor.op.clear_waypoint_flag", "Clear flag {flag} on {n} waypoint", "Clear flag {flag} on {n} waypoints");
inline constexpr Key kLinkChain = NOX_TRK("editor.op.link_chain", "Link {n} waypoints in a chain");
inline constexpr Key kRenameWaypoint = NOX_TRK("editor.op.rename_waypoint", "Rename waypoint {id}");
inline constexpr Key kMoveWaypoint = NOX_TRK("editor.op.move_waypoint", "Move waypoint {id}");

// --- Polygons
inline constexpr Key kPolygonNumbered = NOX_TRK("editor.op.polygon_numbered", "polygon {n}"); ///< (a {polygon} value)
inline constexpr Key kPolygonNamed = NOX_TRK("editor.op.polygon_named", "polygon {name}");    ///< (a {polygon} value)
inline constexpr Key kAddPolygon = NOX_TRK("editor.op.add_polygon", "Add polygon {name}");
inline constexpr Key kEditPolygon = NOX_TRK("editor.op.edit_polygon", "Edit {polygon}");
inline constexpr Key kMoveCorner = NOX_TRK("editor.op.move_corner", "Move a corner of {polygon}");
inline constexpr Key kAddCorner = NOX_TRK("editor.op.add_corner", "Add a corner to {polygon}");
inline constexpr Key kRemoveCorner = NOX_TRK("editor.op.remove_corner", "Remove a corner of {polygon}");
inline constexpr Key kReorderPolygon = NOX_TRK("editor.op.reorder_polygon", "Move {polygon} to position {n}");
inline constexpr PluralKey kDeletePolygons = NOX_TRPK("editor.op.delete_polygons", "Delete {n} polygon", "Delete {n} polygons");
inline constexpr PluralKey kMovePolygons = NOX_TRPK("editor.op.move_polygons", "Move {n} polygon", "Move {n} polygons");
inline constexpr Key kRenamePolygon = NOX_TRK("editor.op.rename_polygon", "Rename polygon {name}");
inline constexpr Key kPolygonAmbient = NOX_TRK("editor.op.polygon_ambient", "Polygon ambient light");
inline constexpr Key kPolygonMinimapGroup = NOX_TRK("editor.op.polygon_minimap_group", "Polygon minimap group {n}");
inline constexpr Key kPolygonMinimapNone = NOX_TRK("editor.op.polygon_minimap_none", "Polygon minimap group none");
inline constexpr Key kPolygonPlayerHandlerNone = NOX_TRK("editor.op.polygon_player_handler_none", "Polygon player-enter handler none");
inline constexpr Key kPolygonPlayerHandler = NOX_TRK("editor.op.polygon_player_handler", "Polygon player-enter handler {function}");
inline constexpr Key kPolygonMonsterHandlerNone = NOX_TRK("editor.op.polygon_monster_handler_none", "Polygon monster-enter handler none");
inline constexpr Key kPolygonMonsterHandler = NOX_TRK("editor.op.polygon_monster_handler", "Polygon monster-enter handler {function}");
inline constexpr Key kPolygonFlagSet = NOX_TRK("editor.op.polygon_flag_set", "Set polygon flag {bit}");
inline constexpr Key kPolygonFlagClear = NOX_TRK("editor.op.polygon_flag_clear", "Clear polygon flag {bit}");

// --- Groups
inline constexpr Key kGroupKindObjects = NOX_TRK("editor.group_kind.objects", "Objects");       ///< (a {kind} value)
inline constexpr Key kGroupKindWaypoints = NOX_TRK("editor.group_kind.waypoints", "Waypoints"); ///< (a {kind} value)
inline constexpr Key kGroupKindWalls = NOX_TRK("editor.group_kind.walls", "Walls");             ///< (a {kind} value)
inline constexpr Key kGroupKindGroups = NOX_TRK("editor.group_kind.groups", "Groups");          ///< (a {kind} value)
inline constexpr Key kAddGroup = NOX_TRK("editor.op.add_group", "Add group {name}");
inline constexpr Key kEditGroup = NOX_TRK("editor.op.edit_group", "Edit group {name}");
inline constexpr Key kDeleteGroup = NOX_TRK("editor.op.delete_group", "Delete group {name}");
inline constexpr Key kAddGroupMembers = NOX_TRK("editor.op.add_group_members", "Add members to group {name}");
inline constexpr Key kRemoveGroupMembers = NOX_TRK("editor.op.remove_group_members", "Remove members from group {name}");
inline constexpr Key kGroupKind = NOX_TRK("editor.op.group_kind", "Group {name} kind {kind}");
inline constexpr Key kRenameGroup = NOX_TRK("editor.op.rename_group", "Rename group {name}");
inline constexpr Key kGroupId = NOX_TRK("editor.op.group_id", "Group {name} id {id}");
inline constexpr Key kGroupRemoveMember = NOX_TRK("editor.op.group_remove_member", "Remove a member from group {name}");

// --- Map info
inline constexpr Key kMapInfoTitle = NOX_TRK("editor.map_info.title", "Title");                   ///< (a {field} value)
inline constexpr Key kMapInfoDescription = NOX_TRK("editor.map_info.description", "Description"); ///< (a {field} value)
inline constexpr Key kMapInfoMapVersion = NOX_TRK("editor.map_info.version", "Version");          ///< (a {field} value)
inline constexpr Key kMapInfoDate = NOX_TRK("editor.map_info.date", "Date");                      ///< (a {field} value)
inline constexpr Key kMapInfoCopyright = NOX_TRK("editor.map_info.copyright", "Copyright");       ///< (a {field} value)
inline constexpr Key kMapInfoAuthor = NOX_TRK("editor.map_info.author", "Author");                ///< (a {field} value)
inline constexpr Key kMapInfoEmail = NOX_TRK("editor.map_info.email", "E-mail");                  ///< (a {field} value)
inline constexpr Key kMapInfoAuthor2 = NOX_TRK("editor.map_info.author2", "Second author");       ///< (a {field} value)
inline constexpr Key kMapInfoEmail2 = NOX_TRK("editor.map_info.email2", "Second e-mail");         ///< (a {field} value)
inline constexpr Key kMapInfoField = NOX_TRK("editor.op.map_info_field", "Map info: {field}");
inline constexpr Key kMapInfoGameTypeAdd = NOX_TRK("editor.op.map_info_game_type_add", "Map info: add game type {bit}");
inline constexpr Key kMapInfoGameTypeRemove = NOX_TRK("editor.op.map_info_game_type_remove", "Map info: remove game type {bit}");
inline constexpr Key kMapInfoVersion = NOX_TRK("editor.op.map_info_version", "Map info: version {n}");
inline constexpr Key kMapInfoMinPlayers = NOX_TRK("editor.op.map_info_min_players", "Map info: min players {n}");
inline constexpr Key kMapInfoMaxPlayers = NOX_TRK("editor.op.map_info_max_players", "Map info: max players {n}");
inline constexpr Key kMapInfoQuestIntro = NOX_TRK("editor.op.map_info_quest_intro", "Map info: quest intro");
inline constexpr Key kMapInfoQuestPicture = NOX_TRK("editor.op.map_info_quest_picture", "Map info: quest picture");
inline constexpr Key kMapInfoQuestPictureName = NOX_TRK("editor.op.map_info_quest_picture_name", "Map info: quest picture {name}");
inline constexpr Key kMapAmbient = NOX_TRK("editor.op.map_ambient", "Map ambient light");
inline constexpr Key kMapIntro = NOX_TRK("editor.op.map_intro", "Map intro text");

// --- Floors and edges
inline constexpr PluralKey kTiles = NOX_TRPK("editor.op.tiles", "{n} tile", "{n} tiles"); ///< (a {tiles} value)
inline constexpr Key kFloorUnnamed = NOX_TRK("editor.op.floor_unnamed", "floor");          ///< (a {floor} value)
inline constexpr PluralKey kAutoEdge = NOX_TRPK("editor.op.auto_edge", "Auto-edge {n} tile", "Auto-edge {n} tiles");
inline constexpr Key kPaintFloor = NOX_TRK("editor.op.paint_floor", "Paint {floor} ({tiles})");
inline constexpr Key kEraseFloor = NOX_TRK("editor.op.erase_floor", "Erase floor ({tiles})");
inline constexpr Key kReplaceFloor = NOX_TRK("editor.op.replace_floor", "Replace with {floor} ({tiles})");
inline constexpr Key kFillFloor = NOX_TRK("editor.op.fill_floor", "Fill with {floor} ({tiles})");
inline constexpr Key kSmoothFloor = NOX_TRK("editor.op.smooth_floor", "Smooth floor ({tiles})");
inline constexpr Key kRecomputeEdges = NOX_TRK("editor.op.recompute_edges", "Recompute edges ({tiles})");
inline constexpr Key kRemoveEdges = NOX_TRK("editor.tools.op.remove_edges", "Remove {name} edges");
inline constexpr Key kRemoveEdge = NOX_TRK("editor.tools.op.remove_edge", "Remove an edge");
inline constexpr Key kAddEdge = NOX_TRK("editor.tools.op.add_edge", "Add a {name} edge");

// --- Walls
inline constexpr Key kWallUnnamed = NOX_TRK("editor.op.wall_unnamed", "wall");
inline constexpr PluralKey kPlaceWalls = NOX_TRPK("editor.op.place_walls", "Place {n} wall ({name})", "Place {n} walls ({name})");
inline constexpr PluralKey kRemoveWalls = NOX_TRPK("editor.op.remove_walls", "Remove {n} wall", "Remove {n} walls");
inline constexpr Key kWallOutline = NOX_TRK("editor.tools.op.wall_outline", "Wall outline ({n} walls)");
inline constexpr Key kWallMaterial = NOX_TRK("editor.tools.op.wall_material", "Wall material {name}");
inline constexpr Key kWallFacing = NOX_TRK("editor.tools.op.wall_facing", "Wall facing");
inline constexpr Key kWallVariant = NOX_TRK("editor.tools.op.wall_variant", "Wall variant");
inline constexpr Key kWallGroup = NOX_TRK("editor.tools.op.wall_group", "Wall minimap group");
inline constexpr Key kWallGroupPolygons = NOX_TRK("editor.tools.op.wall_group_polygons", "Wall minimap group from polygons");
inline constexpr Key kMakeWindow = NOX_TRK("editor.tools.op.make_window", "Make a window");
inline constexpr Key kNoWindow = NOX_TRK("editor.tools.op.no_window", "No window");
inline constexpr Key kMakeBreakable = NOX_TRK("editor.tools.op.make_breakable", "Make breakable");
inline constexpr Key kNotBreakable = NOX_TRK("editor.tools.op.not_breakable", "Not breakable");
inline constexpr Key kMakeSecret = NOX_TRK("editor.tools.op.make_secret", "Make secret");
inline constexpr Key kNotSecret = NOX_TRK("editor.tools.op.not_secret", "Not secret");
inline constexpr Key kSecretFlags = NOX_TRK("editor.tools.op.secret_flags", "Secret wall flags");
inline constexpr Key kSecretWait = NOX_TRK("editor.tools.op.secret_wait", "Secret wall wait");
inline constexpr Key kSecretState = NOX_TRK("editor.tools.op.secret_state", "Secret wall state");
inline constexpr Key kSecretOpening = NOX_TRK("editor.tools.op.secret_opening", "Secret wall opening");
inline constexpr Key kSecretTimer = NOX_TRK("editor.tools.op.secret_timer", "Secret wall timer");

// --- Objects, the selection
inline constexpr Key kPlaceOne = NOX_TRK("editor.op.place_one", "Place {type}");
inline constexpr Key kPlaceObjects = NOX_TRK("editor.op.place_objects", "Place {n} objects");
inline constexpr Key kScatter = NOX_TRK("editor.op.scatter", "Scatter {n} objects");
inline constexpr Key kPlacePrefab = NOX_TRK("editor.op.place_prefab", "Place prefab {name}");
inline constexpr PluralKey kDelete = NOX_TRPK("editor.op.delete", "Delete {n} item", "Delete {n} items");
inline constexpr Key kDeleteItems = NOX_TRK("editor.op.delete_items", "Delete {n} items");
inline constexpr PluralKey kMove = NOX_TRPK("editor.op.move", "Move {n} item", "Move {n} items");
inline constexpr PluralKey kPaste = NOX_TRPK("editor.op.paste", "Paste {n} item", "Paste {n} items");
inline constexpr PluralKey kDuplicate = NOX_TRPK("editor.op.duplicate", "Duplicate {n} item", "Duplicate {n} items");
inline constexpr PluralKey kRotateObjects = NOX_TRPK("editor.op.rotate_objects", "Rotate {n} object", "Rotate {n} objects");
inline constexpr PluralKey kChangeType = NOX_TRPK("editor.op.change_type", "Change {n} object to {type}", "Change {n} objects to {type}");
inline constexpr Key kResetTypeData = NOX_TRK("editor.op.reset_type_data", "Reset type data");
inline constexpr Key kSetExtent = NOX_TRK("editor.op.set_extent", "Set extent of {type} to {extent}");
inline constexpr Key kReorderExtents = NOX_TRK("editor.op.reorder_extents", "Reorder extents from {extent}");
inline constexpr Key kAddDetailsOf = NOX_TRK("editor.op.add_details_of", "Add header details of {type}");
inline constexpr Key kRemoveDetailsOf = NOX_TRK("editor.op.remove_details_of", "Remove header details of {type}");
inline constexpr Key kAddItem = NOX_TRK("editor.op.add_item", "Add {item} to {type}'s inventory");
inline constexpr Key kCloneItem = NOX_TRK("editor.op.clone_item", "Clone {type}");
inline constexpr Key kMoveItem = NOX_TRK("editor.op.move_item", "Move {type} to slot {n}");
inline constexpr Key kEquip = NOX_TRK("editor.op.equip", "Equip {type}");
inline constexpr Key kUnequip = NOX_TRK("editor.op.unequip", "Unequip {type}");
inline constexpr Key kGeneratorMonsterEdit = NOX_TRK("editor.op.generator_monster_edit", "{label} (generator monster)");
inline constexpr Key kSetFieldOf = NOX_TRK("editor.op.set_field_of", "Set {field} of {type}");
inline constexpr Key kSetFieldOn = NOX_TRK("editor.op.set_field_on", "Set {field} on {n} objects");

// --- The Properties panel's own
inline constexpr Key kAddDetails = NOX_TRK("editor.props.op.add_details", "Add header details");
inline constexpr Key kRemoveDetails = NOX_TRK("editor.props.op.remove_details", "Remove header details");
inline constexpr Key kUpgrade = NOX_TRK("editor.props.op.upgrade", "Bring records up to date");
inline constexpr Key kLink = NOX_TRK("editor.props.op.link", "Link");
inline constexpr Key kLinkBothWays = NOX_TRK("editor.props.link.both_ways", "Link both ways");
inline constexpr Key kTurn = NOX_TRK("editor.props.op.turn", "Turn {n} degrees");
inline constexpr Key kSetSpell = NOX_TRK("editor.props.op.set_spell", "Set spell");
inline constexpr Key kSetSpellUse = NOX_TRK("editor.props.op.set_spell_use", "Set spell use");
inline constexpr Key kRemoveSpell = NOX_TRK("editor.props.op.remove_spell", "Remove spell");
inline constexpr Key kAddSpell = NOX_TRK("editor.props.op.add_spell", "Add spell {name}");
inline constexpr Key kSetEnchantment = NOX_TRK("editor.props.op.set_enchantment", "Set enchantment");
inline constexpr Key kSetEnchantmentLevel = NOX_TRK("editor.props.op.set_enchantment_level", "Set enchantment level");
inline constexpr Key kSetEnchantmentTime = NOX_TRK("editor.props.op.set_enchantment_time", "Set enchantment time");
inline constexpr Key kSetShieldHealth = NOX_TRK("editor.props.op.set_shield_health", "Set shield health");
inline constexpr Key kRemoveEnchantment = NOX_TRK("editor.props.op.remove_enchantment", "Remove enchantment");
inline constexpr Key kAddEnchantment = NOX_TRK("editor.props.op.add_enchantment", "Add enchantment {name}");
inline constexpr Key kSetShopCount = NOX_TRK("editor.props.op.set_shop_count", "Set shop count");
inline constexpr Key kSetShopItem = NOX_TRK("editor.props.op.set_shop_item", "Set shop item");
inline constexpr Key kMoveShopItem = NOX_TRK("editor.props.op.move_shop_item", "Move shop item");
inline constexpr Key kRemoveShopItem = NOX_TRK("editor.props.op.remove_shop_item", "Remove shop item");
inline constexpr Key kSetShopBook = NOX_TRK("editor.props.op.set_shop_book", "Set shop book");
inline constexpr Key kSetShopModifier = NOX_TRK("editor.props.op.set_shop_modifier", "Set shop item modifier");
inline constexpr Key kAddShopItem = NOX_TRK("editor.props.op.add_shop_item", "Add shop item {type}");
inline constexpr Key kSetGlyphSpell = NOX_TRK("editor.props.op.set_glyph_spell", "Set glyph spell");
inline constexpr Key kRemoveGlyphSpell = NOX_TRK("editor.props.op.remove_glyph_spell", "Remove glyph spell");
inline constexpr Key kAddGlyphSpell = NOX_TRK("editor.props.op.add_glyph_spell", "Add glyph spell {name}");
inline constexpr Key kRewardSpells = NOX_TRK("editor.props.rewards.spells", "Spells");                 ///< (a {list} value)
inline constexpr Key kRewardAbilities = NOX_TRK("editor.props.rewards.abilities", "Abilities");        ///< (a {list} value)
inline constexpr Key kRewardFieldGuides = NOX_TRK("editor.props.rewards.field_guides", "Field guides"); ///< (a {list} value)
inline constexpr Key kRemoveFromList = NOX_TRK("editor.props.op.remove_from_list", "Remove from {list}");
inline constexpr Key kAddToList = NOX_TRK("editor.props.op.add_to_list", "Add to {list}");
inline constexpr Key kSetGeneratorMonster = NOX_TRK("editor.props.op.set_generator_monster", "Set generator monster to {type}");
inline constexpr Key kRemoveGeneratorMonster = NOX_TRK("editor.props.op.remove_generator_monster", "Remove generator monster");
inline constexpr Key kSetGeneratorField = NOX_TRK("editor.props.op.set_generator_field", "Set generator monster's {field}");
inline constexpr Key kAddGeneratorMonster = NOX_TRK("editor.props.op.add_generator_monster", "Add generator monster {type}");
inline constexpr Key kRemoveItem = NOX_TRK("editor.props.op.remove_item", "Remove {type} from the inventory");
inline constexpr Key kSetColourKey = NOX_TRK("editor.props.op.set_colour_key", "Set light colour key");
inline constexpr Key kRemoveColourKey = NOX_TRK("editor.props.op.remove_colour_key", "Remove light colour key");
inline constexpr Key kSetIntensityKeys = NOX_TRK("editor.props.op.set_intensity_keys", "Set Intensity keys");
inline constexpr Key kSetConeKeys = NOX_TRK("editor.props.op.set_cone_keys", "Set Cone keys");
inline constexpr Key kSetLightKey = NOX_TRK("editor.props.op.set_light_key", "Set light key");

// --- The script
inline constexpr Key kCompileScript = NOX_TRK("editor.op.compile_script", "Compile script");
inline constexpr Key kDecompileScript = NOX_TRK("editor.script.op.decompile", "Decompile script");
inline constexpr Key kEditScript = NOX_TRK("editor.script.op.edit", "Edit script");
inline constexpr Key kImportScript = NOX_TRK("editor.script.op.import", "Import script");
inline constexpr Key kScriptChangedOnDisk = NOX_TRK("editor.script.op.changed_on_disk", "Script changed on disk");
inline constexpr Key kAddFunction = NOX_TRK("editor.script.op.add_function", "Add function {name}");

// --- A session's
inline constexpr Key kSnapshot = NOX_TRK("editor.op.snapshot", "Snapshot");

/// The placeholders whose values are texts of the table themselves (translated when shown).
inline constexpr std::string_view kNestedNames[] = {"polygon", "tiles", "floor", "kind", "field", "list", "label"};
} // namespace oplabels

/// A label from its template, in English (what an Op carries). A counted template takes the form
/// English uses for its "n" argument.
std::string opLabel(const nox::l10n::Key& text, std::initializer_list<nox::l10n::Arg> args = {});
std::string opLabel(const nox::l10n::PluralKey& text, std::initializer_list<nox::l10n::Arg> args);

/// An op's label as shown, in the language in use: the template it was made from translated and
/// filled with its values; a label no template matches, as it is. Cached by label (made again when
/// the language changes): cheap to call for what is drawn. The reference holds until the next call.
const std::string& shownOpLabel(std::string_view english);

/// Every template (for the tests: each one's English, made and shown, comes back).
struct OpLabelTemplate {
    const nox::l10n::Key* key = nullptr;          ///< a fixed one's
    const nox::l10n::PluralKey* plural = nullptr; ///< a counted one's
};
std::span<const OpLabelTemplate> opLabelTemplates();

} // namespace nox::mapedit
