// Which maps use which object types (map_usage.hpp).
#include "content/map_usage.hpp"

#include "noxcore/map/map_file.hpp"
#include "noxcore/map/sections.hpp"
#include "noxcore/util/text.hpp"

#include <exception>

namespace videotools::content {

MapUsage MapUsage::scan(const std::vector<std::filesystem::path>& maps)
{
    MapUsage u;
    for (const auto& path : maps) {
        const std::size_t index = u.names_.size();
        u.names_.push_back(path.stem().string());
        try {
            const nox::map::MapFile map = nox::map::loadMapFile(path);
            const nox::map::RawSection* toc = map.find(nox::map::ObjectTOC::kName);
            if (!toc) {
                continue;
            }
            for (const auto& e : nox::map::ObjectTOC::parse(toc->data).entries) {
                auto& list = u.byType_[nox::util::lowered(e.name)];
                if (list.empty() || list.back() != index) {
                    list.push_back(index);
                }
            }
        } catch (const std::exception&) {
            ++u.failed_; // (an .nxz, or a broken map: not counted)
        }
    }
    return u;
}

const std::vector<std::size_t>* MapUsage::mapsUsing(std::string_view type) const
{
    const auto it = byType_.find(nox::util::lowered(type));
    return it == byType_.end() ? nullptr : &it->second;
}

} // namespace videotools::content
