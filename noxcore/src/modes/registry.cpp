// The modes a process knows (registry.hpp).
#include "noxcore/modes/registry.hpp"

#include "noxcore/util/text.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace nox::modes {

void Registry::add(Mode mode)
{
    if (mode.info.id.empty()) {
        throw std::invalid_argument("a mode needs an id");
    }
    // (The opcodes the host's and the client's receive switches hand to a mode, framed {op, sub,
    // u16 length}: 0xF4, 0xF5 and 0xF7. 0xF6 is the map editor's live session, 0xF8 the script
    // modes' UI's, the rest not framed by released clients.)
    for (const std::uint8_t op : mode.info.opcodes) {
        if (op < 0xF4 || op > 0xF7 || op == 0xF6) {
            throw std::invalid_argument("mode \"" + mode.info.id + "\": opcode " + std::to_string(op) + " isn't one a mode's messages can use (0xF4, 0xF5, 0xF7)");
        }
    }
    std::lock_guard lock(mutex_);
    for (const auto& m : modes_) {
        if (util::equalsNoCase(m->info.id, mode.info.id)) {
            throw std::invalid_argument("mode \"" + mode.info.id + "\" is registered already");
        }
        for (const std::uint8_t op : mode.info.opcodes) {
            if (std::ranges::find(m->info.opcodes, op) != m->info.opcodes.end()) {
                throw std::invalid_argument("mode \"" + mode.info.id + "\": opcode " + std::to_string(op) + " is mode \"" + m->info.id + "\"'s");
            }
        }
    }
    auto made = std::make_unique<Mode>(std::move(mode));
    const auto at = std::ranges::find_if(modes_, [&](const std::unique_ptr<Mode>& m) {
        return m->info.order != made->info.order ? made->info.order < m->info.order : made->info.id < m->info.id;
    });
    modes_.insert(at, std::move(made));
}

bool Registry::remove(std::string_view id)
{
    std::lock_guard lock(mutex_);
    // (Out of the list, never freed: find, all and byOpcode hand out pointers another thread may
    // still hold, a retired host's or a client's receive path. A mods folder read again between
    // games is the only remover: a few modes kept a process's life.)
    const auto gone = std::ranges::stable_partition(modes_, [&](const std::unique_ptr<Mode>& m) { return !util::equalsNoCase(m->info.id, id); });
    const bool any = gone.begin() != modes_.end();
    for (auto it = gone.begin(); it != modes_.end(); ++it) {
        removed_.push_back(std::move(*it));
    }
    modes_.erase(gone.begin(), modes_.end());
    return any;
}

std::vector<const Mode*> Registry::all() const
{
    std::lock_guard lock(mutex_);
    std::vector<const Mode*> out;
    out.reserve(modes_.size());
    for (const auto& m : modes_) {
        out.push_back(m.get());
    }
    return out;
}

const Mode* Registry::find(std::string_view id) const
{
    std::lock_guard lock(mutex_);
    for (const auto& m : modes_) {
        if (util::equalsNoCase(m->info.id, id)) {
            return m.get();
        }
    }
    return nullptr;
}

const Mode* Registry::byOpcode(std::uint8_t opcode) const
{
    std::lock_guard lock(mutex_);
    for (const auto& m : modes_) {
        if (std::ranges::find(m->info.opcodes, opcode) != m->info.opcodes.end()) {
            return m.get();
        }
    }
    return nullptr;
}

bool Registry::ownsMap(std::string_view mapName) const
{
    std::lock_guard lock(mutex_);
    return std::ranges::any_of(modes_, [&](const std::unique_ptr<Mode>& m) { return m->info.ownsMap && m->info.ownsMap(mapName); });
}

std::size_t Registry::size() const
{
    std::lock_guard lock(mutex_);
    return modes_.size();
}

Registry& registry()
{
    static Registry r;
    return r;
}

} // namespace nox::modes
