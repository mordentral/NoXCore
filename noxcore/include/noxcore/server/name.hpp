// An interned name: what ServerObject keeps of its
// type's and functions' names (typeName, updateFn, collideFn...). A Name is one pointer to an
// entry made once a process for each text and never freed, so copies are cheap, two Names are
// equal when their pointers are, and the names the frame's code asks about by kind are known by
// an id (NameId) set as the entry is made: `o.updateFn == NameId::PixieUpdate` reads one number.
// The text is the same text in and out (saves, messages, scripts): only its keeping changes.
//
// Making a Name from text (interning) is safe from any thread; reading one never locks.
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>
#include <string_view>
#include <type_traits>

namespace nox::server {

/// The names hot code asks about by kind (each the text of its identifier). Any other text's
/// entry has NameId::None, as has the empty name. (Add one here to compare against it by id.)
#define NOX_SERVER_NAMES(X)                                                                       \
    /* UPDATE functions */                                                                         \
    X(ArachnaphobiaUpdate) X(BallUpdate) X(BlackPowderBarrelUpdate) X(BlackPowderBurnUpdate)      \
    X(BlowUpdate) X(BreakAndRemoveUpdate) X(BreakUpdate) X(ChakramInMotionUpdate) X(CrownUpdate) X(DeathBallFragmentUpdate) \
    X(DeathBallUpdate) X(ElevatorShaftUpdate) X(ElevatorUpdate) X(ExpireUpdate) X(FistUpdate)      \
    X(FlagUpdate) X(FlameCleanseUpdate) X(GlyphChainUpdate) X(HarpoonUpdate)                       \
    X(HomingProjectileUpdate) X(InvisiblePentagramUpdate) X(LifetimeUpdate) X(LoopAndDamageUpdate) \
    X(MagicMissileUpdate) X(MeteorShowerUpdate) X(MeteorUpdate) X(MonsterGeneratorUpdate)          \
    X(MoonglowUpdate) X(MoverUpdate) X(ObeliskUpdate) X(OneSecondDieUpdate) X(OpenUpdate)          \
    X(PentagramUpdate) X(PixieUpdate) X(SelfDestructUpdate) X(SentryGlobeUpdate) X(SkullUpdate)    \
    X(SmallToxicCloudUpdate) X(SpellProjectileUpdate) X(SwitchUpdate) X(TelekinesisUpdate)         \
    X(ToggleUpdate) X(ToxicCloudUpdate) X(TrapDoorUpdate) X(TriggerUpdate) X(UndeadKillerUpdate)   \
    X(WaterBarrelUpdate)                                                                           \
    /* COLLIDE functions */                                                                        \
    X(AnkhCollide) X(ArrowCollide) X(AudioEventCollide) X(AwardSpellCollide) X(BallCollide)       \
    X(BearTrapCollide) X(BombCollide) X(BoomCollide) X(ChakramInMotionCollide) X(ChestCollide)    \
    X(CrownCollide) X(DamageCollide) X(DeathBallCollide) X(DeathBallFragmentCollide) X(DieCollide) \
    X(DoorCollide) X(ExitCollide) X(FistCollide) X(FlagCollide) X(GlyphCollide) X(HarpoonCollide) \
    X(HomeBaseCollide) X(ManaDrainCollide) X(MimicCollide) X(MonsterArrowCollide)                 \
    X(MonsterCollide) X(MonsterGeneratorCollide) X(OwnCollide) X(PentagramCollide)                \
    X(PickupCollide) X(PixieCollide) X(PoisonGasTrapCollide) X(ProjectileCollide) X(SignCollide)  \
    X(SoulGateCollide) X(SparkExplosionCollide) X(SpellProjectileCollide) X(SpiderSpitCollide)    \
    X(TeleportWakeCollide) X(TrapDoorCollide) X(TriggerCollide) X(UndeadKillerCollide)            \
    X(WallReflectCollide) X(YellowStarShotCollide)                                                 \
    /* DAMAGE functions */                                                                         \
    X(ArmorDamage) X(BallDamage) X(BlackPowderDamage) X(FlammableDamage) X(MechGolemDamage)       \
    X(MonsterGeneratorDamage) X(PlayerDamage) X(SkeletonDamage) X(WeaponDamage)                   \
    /* types */                                                                                    \
    X(BlackPowder) X(DeathBall) X(FishBig) X(FishSmall) X(GameBall) X(Glyph) X(GreenFrog) X(Hecubah) \
    X(ImpShot) X(Magic)                                                                            \
    X(LargeFist) X(MediumFist) X(Meteor) X(Necromancer) X(PeriodicSpike) X(Pixie) X(PlayerStart) X(Polyp) \
    X(PressurePlate) X(Rat) X(SmallFist) X(SmallToxicCloud) X(Spike) X(TelekinesisHand)           \
    X(TeleportPentagram) X(ThrowingStone) X(ToxicCloud) X(Trigger) X(WillOWisp)                  \
    X(Crown) X(GameBallStart) X(GoldKey) X(SilverKey)                                              \
    /* noxcpp's own: a mode's COLLIDE (ModeHooks::collide), set on its objects by the mode */      \
    X(ModeCollide)

enum class NameId : std::uint16_t {
    None = 0,
#define NOX_NAME_ID(n) n,
    NOX_SERVER_NAMES(NOX_NAME_ID)
#undef NOX_NAME_ID
    Count
};

/// What's worked out once from a name's text, for checks the frame makes often.
namespace NameTrait {
inline constexpr std::uint8_t FlameCleanse = 0x1; ///< ends with "FlameCleanse" (the cleansing flames)
} // namespace NameTrait

/// One interned name: made once, never freed, never changed.
struct NameEntry {
    std::string text;
    std::size_t hash = 0;
    NameId id = NameId::None;
    std::uint8_t traits = 0;
    const NameEntry* next = nullptr; ///< (the table's chain: set before the entry's published)
};

class Name {
public:
    constexpr Name() noexcept = default;
    Name(std::string_view text) : e_(intern(text)) {}
    Name(const std::string& text) : e_(intern(text)) {}
    Name(const char* text) : e_(text ? intern(text) : nullptr) {}

    /// The entry for `text`, made if it's new (thread-safe; nullptr for the empty name).
    static const NameEntry* intern(std::string_view text);

    std::string_view view() const noexcept { return e_ ? std::string_view(e_->text) : std::string_view(); }
    const std::string& str() const noexcept { return e_ ? e_->text : emptyString(); }
    const char* c_str() const noexcept { return e_ ? e_->text.c_str() : ""; }
    const char* data() const noexcept { return c_str(); }
    std::size_t size() const noexcept { return e_ ? e_->text.size() : 0; }
    std::size_t length() const noexcept { return size(); }
    bool empty() const noexcept { return e_ == nullptr; }
    NameId id() const noexcept { return e_ ? e_->id : NameId::None; }
    bool has(std::uint8_t trait) const noexcept { return e_ && (e_->traits & trait) != 0; }
    const NameEntry* entry() const noexcept { return e_; }
    void clear() noexcept { e_ = nullptr; }

    operator std::string_view() const noexcept { return view(); }
    operator const std::string&() const noexcept { return str(); }

    // (std::string_view's look-ups, for code that reads the text.)
    char operator[](std::size_t i) const noexcept { return view()[i]; }
    char front() const noexcept { return view().front(); }
    char back() const noexcept { return view().back(); }
    bool starts_with(std::string_view s) const noexcept { return view().starts_with(s); }
    bool ends_with(std::string_view s) const noexcept { return view().ends_with(s); }
    bool contains(std::string_view s) const noexcept { return view().find(s) != std::string_view::npos; }
    std::size_t find(std::string_view s, std::size_t pos = 0) const noexcept { return view().find(s, pos); }
    std::size_t find(char c, std::size_t pos = 0) const noexcept { return view().find(c, pos); }
    std::size_t rfind(std::string_view s, std::size_t pos = std::string_view::npos) const noexcept { return view().rfind(s, pos); }
    std::size_t rfind(char c, std::size_t pos = std::string_view::npos) const noexcept { return view().rfind(c, pos); }
    std::string substr(std::size_t pos = 0, std::size_t n = std::string::npos) const { return str().substr(pos, n); }
    std::string_view::const_iterator begin() const noexcept { return view().begin(); }
    std::string_view::const_iterator end() const noexcept { return view().end(); }

    friend bool operator==(const Name& a, const Name& b) noexcept { return a.e_ == b.e_; }
    friend bool operator==(const Name& a, NameId id) noexcept { return a.id() == id; }
    friend bool operator==(const Name& a, std::string_view b) noexcept { return a.view() == b; }
    friend bool operator==(const Name& a, const std::string& b) noexcept { return a.view() == b; }
    friend bool operator==(const Name& a, const char* b) noexcept { return a.view() == (b ? std::string_view(b) : std::string_view()); }
    /// Ordered by text (as the strings were), for sorting and keyed maps.
    friend std::strong_ordering operator<=>(const Name& a, const Name& b) noexcept
    {
        return a.e_ == b.e_ ? std::strong_ordering::equal : a.view() <=> b.view();
    }
    friend std::strong_ordering operator<=>(const Name& a, std::string_view b) noexcept { return a.view() <=> b; }

    /// Joined to other text, as the string's was.
    friend std::string operator+(const Name& a, std::string_view b)
    {
        std::string s(a.view());
        s += b;
        return s;
    }
    friend std::string operator+(std::string_view a, const Name& b)
    {
        std::string s(a);
        s += b.view();
        return s;
    }

private:
    explicit Name(const NameEntry* e) noexcept : e_(e) {}
    static const std::string& emptyString() noexcept;

    const NameEntry* e_ = nullptr;
};

std::ostream& operator<<(std::ostream& os, const Name& n);

} // namespace nox::server

/// A Name and a string (or string view) meet as text: so the ranges' algorithms (find, count)
/// compare a Name with a list of strings as they did the string it was.
template <template <class> class TQual, template <class> class UQual>
struct std::basic_common_reference<nox::server::Name, std::string, TQual, UQual> {
    using type = std::string_view;
};
template <template <class> class TQual, template <class> class UQual>
struct std::basic_common_reference<std::string, nox::server::Name, TQual, UQual> {
    using type = std::string_view;
};
template <template <class> class TQual, template <class> class UQual>
struct std::basic_common_reference<nox::server::Name, std::string_view, TQual, UQual> {
    using type = std::string_view;
};
template <template <class> class TQual, template <class> class UQual>
struct std::basic_common_reference<std::string_view, nox::server::Name, TQual, UQual> {
    using type = std::string_view;
};

/// (By the text's hash, not the entry's address: a hashed container's order the same each run.)
template <>
struct std::hash<nox::server::Name> {
    std::size_t operator()(const nox::server::Name& n) const noexcept { return n.entry() ? n.entry()->hash : 0; }
};
