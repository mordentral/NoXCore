// The editor's sidecar file, <map>.nxe beside the .map: editor-only data, never in the .map
// (the game fails to load a map with a section it doesn't know).
//
// JSON, one object: the bookkeeping as fields, the rest (script debug info, the paint settings...)
// as values under their own keys. Readers keep keys they don't know, so an older editor doesn't
// drop a newer one's data.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace nox::mapedit {

struct Sidecar {
    static constexpr int kFormatVersion = 1;

    int formatVersion = kFormatVersion;
    std::string origin;       // the map this one was made from ("" for a new map)
    bool originStock = false; // whether that was a map that ships with the game
    std::uint32_t revision = 0; // bumped on every save
    /// Keys this version doesn't know, kept as their JSON text.
    std::map<std::string, std::string> other;

    /// A text value kept under `key` among the others (the paint settings...), if there.
    std::optional<std::string> text(std::string_view key) const;
    void setText(const std::string& key, std::string_view value);

    std::string toJson() const;
    /// Throws FormatError for text that isn't a JSON object.
    static Sidecar fromJson(std::string_view text);
};

} // namespace nox::mapedit
