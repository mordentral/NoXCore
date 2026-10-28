// The generator's biome table, read once from its script (modes/common/script/mapgen/biomes.luau,
// built in under "mapgen") into the Biome records biome.hpp
// describes. Read the first time it's asked for, on whichever thread (a level built ahead, the
// server's), and kept for the program's life.
#include "noxcore/mapgen/biome.hpp"

#include "noxcore/modes/script/script_data.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <stdexcept>

namespace nox::mapgen {

namespace {

using modes::script::DataValue;

struct Table {
    DataValue root; ///< what the script returned: the records' text points into it
    std::vector<Biome> all;
    std::vector<std::string_view> harming;
};

std::vector<std::string_view> views(const DataValue& list)
{
    std::vector<std::string_view> out;
    out.reserve(list.size());
    for (const DataValue& v : list.list()) {
        out.emplace_back(v.asString());
    }
    return out;
}

template <class Enum, std::size_t N>
Enum named(const DataValue& v, const std::pair<std::string_view, Enum> (&names)[N])
{
    for (const auto& [name, value] : names) {
        if (v.asString() == name) {
            return value;
        }
    }
    throw std::runtime_error("mapgen's biomes: " + v.where() + " is no known name (" + v.asString() + ")");
}

/// (Filled where it stays: the records' views point into root's strings, which a move of a
/// short one would leave behind.)
void read(Table& t)
{
    static constexpr std::pair<std::string_view, Climate> kClimates[] = {
        {"Mild", Climate::Mild}, {"Freezing", Climate::Freezing}, {"Scorching", Climate::Scorching}};
    static constexpr std::pair<std::string_view, LayoutStyle> kStyles[] = {
        {"Built", LayoutStyle::Built}, {"Natural", LayoutStyle::Natural}, {"Open", LayoutStyle::Open}};
    t.root = modes::script::readBuiltinData("mapgen", "mapgen/biomes.luau");
    for (const DataValue& b : t.root["biomes"].list()) {
        Biome out;
        out.name = b["name"].asString();
        out.title = b["title"].asString();
        // (Its walls, floors, light and scenery: the script's own, checked here as they're read.)
        (void)b["wallSet"].asString();
        (void)b["floor"].asString();
        (void)views(b["patches"]);
        (void)b["path"].asString();
        (void)b["blend"].asBool();
        (void)b["edgeMask"].asString();
        (void)b["ambient"].asUint();
        for (const char* size : {"large", "medium", "small", "wall"}) {
            (void)views(b[size]);
        }
        for (const char* every : {"largeEvery", "mediumEvery", "smallEvery", "wallEvery"}) {
            (void)b[every].asInt();
        }
        out.weight = b["weight"].asInt();
        out.climate = named(b["climate"], kClimates);
        out.style = named(b["style"], kStyles);
        out.chests = views(b["chests"]);
        t.all.push_back(std::move(out));
    }
    t.harming = views(t.root["harmingFloors"]);
}

struct Loaded : Table {
    Loaded() { read(*this); }
    Loaded(const Loaded&) = delete;
};

const Table& table()
{
    // (never destroyed (made on the heap, never freed): a Delve level still being built on a
    // worker as the program exits must not read it after its destruction)
    static const Loaded& t = *new Loaded();
    return t;
}

} // namespace

bool harmingFloor(std::string_view floor)
{
    return std::ranges::find(table().harming, floor) != table().harming.end();
}

std::span<const Biome> biomes()
{
    return table().all;
}

// By name, ignoring case; null when there is none.
const Biome* findBiome(std::string_view name)
{
    const auto& all = table().all;
    const auto it = std::ranges::find_if(all, [&](const Biome& b) { return util::equalsNoCase(name, b.name); });
    return it == all.end() ? nullptr : &*it;
}

} // namespace nox::mapgen
