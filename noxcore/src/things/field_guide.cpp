// Field guide look-ups that need code (see field_guide.hpp for the creature list itself).
#include "noxcore/things/field_guide.hpp"

namespace nox::things {

namespace {

/// Creature 24, the spider, learns the albino, small albino, small and spitting spiders with it.
constexpr std::array<int, 4> kSpiders{7, 8, 25, 26};

} // namespace

std::span<const int> guideGroup(int creature)
{
    if (creature == 24) {
        return kSpiders;
    }
    return {};
}

int guideSize(const ThingDb& things, int creature)
{
    if (creature < 1 || creature > kGuideCreatures) {
        return 4;
    }
    const ThingDef* t = things.findThing(kGuideNames[static_cast<std::size_t>(creature - 1)]);
    if (!t) {
        return 4;
    }
    const auto sub = t->attribute("SUBCLASS");
    const std::string_view s = sub ? *sub : std::string_view{};
    // SUBCLASS bit 1 is SMALL_MONSTER, bit 2 MEDIUM_MONSTER.
    if (s.find("SMALL_MONSTER") != std::string_view::npos) {
        return 1;
    }
    if (s.find("MEDIUM_MONSTER") != std::string_view::npos) {
        return 2;
    }
    return 4;
}

} // namespace nox::things
