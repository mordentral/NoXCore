// A small zip writer and reader for stored (uncompressed) archives: the map editor's "Export
// mod..." writes a mod's folder as a zip someone unzips into their own mods\. Plain zip (PKWARE APPNOTE): local headers, the central
// directory, its end record; method 0 (stored), CRC-32, names in UTF-8 (flag bit 11), no zip64 (an
// archive and each file under 4 GB, at most 65535 entries). The reader takes stored archives only
// (its own, and the tests' check of them).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::io {

class ZipWriter {
public:
    /// A file's entry at `name` ('/' separated, relative; a folder's entry ends in '/', no data).
    /// `dosTime` / `dosDate` are MS-DOS's (the default: 1980-01-01 00:00, so an archive of the
    /// same files is the same bytes). Throws std::invalid_argument for a bad name, one there
    /// already, or past the format's limits.
    void add(std::string_view name, std::span<const std::uint8_t> data, std::uint16_t dosTime = 0, std::uint16_t dosDate = 0x21);
    void add(std::string_view name, std::string_view text) { add(name, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())); }
    /// The archive: the entries so far, then the central directory and its end.
    std::vector<std::uint8_t> finish() const;
    std::size_t entries() const noexcept { return central_.size(); }

private:
    struct Central {
        std::string name;
        std::uint32_t crc = 0;
        std::uint32_t size = 0;
        std::uint32_t offset = 0;
        std::uint16_t time = 0, date = 0;
    };
    std::vector<std::uint8_t> body_; ///< the local headers and data
    std::vector<Central> central_;
};

/// The standard CRC-32 (zip's, PNG's) of `data`.
std::uint32_t crc32(std::span<const std::uint8_t> data) noexcept;

struct ZipEntry {
    std::string name;
    std::vector<std::uint8_t> data;
};

/// A stored archive's entries in its central directory's order, each one's CRC checked; none,
/// with why in `error`: not a zip, one that's cut short or wrong, or an entry that's compressed.
std::optional<std::vector<ZipEntry>> readStoredZip(std::span<const std::uint8_t> bytes, std::string& error);

} // namespace nox::io
