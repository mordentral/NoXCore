// Decoding and encoding of the MSG_FX_* effect messages as one variant (see fx.hpp).
#include "noxcore/net/fx.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"

#include <cctype>
#include <exception>
#include <utility>

namespace nox::net {

namespace {

/// Decodes `message` as alternative I of FxMessage if its opcode matches, trying each in turn.
template <std::size_t I = 0>
std::optional<FxMessage> decodeAs(std::uint8_t op, std::span<const std::uint8_t> message)
{
    if constexpr (I == std::variant_size_v<FxMessage>) {
        return std::nullopt;
    } else {
        using M = std::variant_alternative_t<I, FxMessage>;
        constexpr bool family = requires { M::kSub; };
        if (op != static_cast<std::uint8_t>(M::kOp)) {
            return decodeAs<I + 1>(op, message);
        }
        if constexpr (family) {
            if (message.size() < 2 || message[1] != M::kSub) {
                return decodeAs<I + 1>(op, message);
            }
        }
        if (message.size() < M::kSize) {
            return std::nullopt;
        }
        try {
            // Skip the opcode, and the sub-op too for a family message; kSize counts both.
            constexpr std::size_t skip = family ? 2 : 1;
            ByteReader r(message.subspan(skip, M::kSize - skip), opInfo(M::kOp).name);
            return FxMessage{M::read(r)};
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
}

} // namespace

std::optional<FxMessage> decodeFx(std::span<const std::uint8_t> message)
{
    if (message.empty() || !(isFxOp(message[0]) || message[0] == static_cast<std::uint8_t>(Op::Gauntlet))) {
        return std::nullopt;
    }
    return decodeAs(message[0], message);
}

std::vector<std::uint8_t> encodeFx(const FxMessage& fx)
{
    ByteWriter w;
    std::visit([&](const auto& m) { m.write(w); }, fx);
    return std::move(w).take();
}

Op fxOp(const FxMessage& fx)
{
    return std::visit([](const auto& m) { return std::decay_t<decltype(m)>::kOp; }, fx);
}

// Looks up an FX opcode by its name without the MSG_FX_ prefix, ignoring case
// (e.g. "explosion" for MSG_FX_EXPLOSION).
std::optional<Op> fxOpByName(std::string_view name)
{
    constexpr std::string_view kPrefix = "MSG_FX_";
    for (auto op = static_cast<unsigned>(Op::FxParticlefx); op <= static_cast<unsigned>(Op::FxManaBombCancel); ++op) {
        const std::string_view full = opInfo(static_cast<std::uint8_t>(op)).name;
        if (!full.starts_with(kPrefix) || full.size() - kPrefix.size() != name.size()) {
            continue;
        }
        bool same = true;
        for (std::size_t i = 0; i < name.size() && same; ++i) {
            same = std::toupper(static_cast<unsigned char>(name[i])) == full[kPrefix.size() + i];
        }
        if (same) {
            return static_cast<Op>(op);
        }
    }
    return std::nullopt;
}

} // namespace nox::net
