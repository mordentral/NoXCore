// The editor's open files: a game folder's video.bag (+ video.idx) and an image pack (.nxp),
// each with its pending changes, saved only to new files (Save As).
//
// Edits are kept in memory as what the file will hold (a bag image's bytes in its slot's type, a
// pack image encoded), so a save copies everything else as stored and writes only those. Saving
// opens the source again, so it can run on another thread while the document is browsed (give
// it a snapshot of the edits: bagEdits() / packEdits()).
#pragma once

#include "image_source.hpp"

#include "noxcore/video/image_pack.hpp"
#include "noxcore/video/sprite.hpp"
#include "noxcore/video/video_bag_writer.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace videotools::editor {

using Progress = std::function<void(std::size_t done, std::size_t total)>;

/// Refuses writing over a file that's open (`sources`), into the game install (a folder holding
/// GAME.EXE or one of `protectedDirs`), or to a target with no file name. Returns the reason, or
/// an empty string if `target` may be written.
std::string checkSaveTarget(const std::filesystem::path& target, const std::vector<std::filesystem::path>& sources,
                            const std::vector<std::filesystem::path>& protectedDirs);

/// The .idx written beside a bag: same name, extension .idx.
std::filesystem::path idxBeside(const std::filesystem::path& bag);

class BagDocument {
public:
    /// Opens video.bag / video.idx. Throws.
    explicit BagDocument(const GamePaths& paths);

    const GamePaths& paths() const { return paths_; }
    const nox::video::VideoBag& bag() const { return source_.bag(); }
    std::size_t size() const { return bag().images().size(); }

    /// Image `index` as it will be saved (an edit, else video.bag's). Throws.
    nox::video::DecodedImage decode(std::int32_t index);
    /// video.bag's own image, ignoring edits. Throws.
    nox::video::DecodedImage decodeOriginal(std::int32_t index);

    /// Replaces image `index` with `image` (encoded in the slot's type; the image's own type is
    /// ignored). Returns the image as the bag will hold it (colours rounded to the type's).
    /// Throws FormatError if the slot's type can't take it.
    nox::video::DecodedImage replace(std::int32_t index, nox::video::DecodedImage image);
    /// Drops the edit of image `index`.
    void revert(std::int32_t index);
    void revertAll() { edits_.clear(); }
    bool edited(std::int32_t index) const { return edits_.count(std::uint32_t(index)) != 0; }
    std::size_t editCount() const { return edits_.size(); }
    const std::map<std::uint32_t, std::vector<std::uint8_t>>& bagEdits() const { return edits_; }

    /// Writes the bag with `edits` to `bag` and idxBeside(bag). Opens `paths` again (thread-safe
    /// apart from the document). Throws.
    static nox::video::BagWriteResult save(const GamePaths& paths, const std::map<std::uint32_t, std::vector<std::uint8_t>>& edits,
                                           const std::filesystem::path& bag, const Progress& progress = {});

private:
    GamePaths paths_;
    ImageSource source_;
    std::map<std::uint32_t, std::vector<std::uint8_t>> edits_;
};

/// A pack edit: an image to store (encoded, with the image itself for showing), or a removal.
struct PackEdit {
    std::optional<nox::video::EncodedImage> encoded; // nullopt: the slot is emptied
    std::shared_ptr<const nox::video::PackImage> image;
};

struct PackSaveOptions {
    /// The video.idx CRC to stamp in the pack's header (noxsdl uses a pack only with the
    /// video.idx it was made from); nullopt keeps the pack's.
    std::optional<std::uint32_t> idxCrc;
    /// Copy <pack>.animations.json (the HD animations' sidecar) beside the new pack.
    bool copySidecar = true;
};

struct PackSaveResult {
    std::size_t copied = 0;   // slots copied as stored
    std::size_t written = 0;  // edited slots written
    std::size_t removed = 0;  // edited slots emptied
    std::uint32_t slots = 0;
    bool sidecarCopied = false;
};

/// How a pack image is encoded when it's put in.
struct PackEncodeOptions {
    bool bc7 = true;        // BC7 with its mip chain (what noxsdl's HD pack uses), else lossless zstd
    float bc7Lambda = 0.0f; // RDO (smaller, a little lossier); 0 off
    int level = 19;         // zstd level
};

/// Encodes an image for a pack (BC7 or lossless, by `options`). Thread-safe.
nox::video::EncodedImage encodeForPack(const nox::video::PackImage& image, const PackEncodeOptions& options);

class PackDocument {
public:
    /// Opens a pack. Throws.
    explicit PackDocument(const std::filesystem::path& path);
    /// A new empty pack for a bag: `slots` slots, stamped with the bag's image count and idx CRC.
    PackDocument(std::uint32_t slots, std::uint32_t sourceImages, std::uint32_t sourceIdxCrc);

    const std::filesystem::path& path() const { return path_; } // empty for a new pack
    std::uint32_t size() const;                                  // slots, edits included
    std::uint32_t sourceImages() const { return sourceImages_; }
    std::uint32_t sourceIdxCrc() const { return sourceIdxCrc_; }
    /// Whether slot `index` holds an image (as it will be saved).
    bool present(std::uint32_t index) const;
    /// The slot's entry as it will be saved (nullptr if empty).
    const nox::video::PackEntry* entry(std::uint32_t index) const;
    /// The slot's image name as it will be saved (empty if none).
    std::string name(std::uint32_t index) const;
    /// Whether any stored image is BC7 (the default for new ones then).
    bool hasBc7() const { return hasBc7_; }

    /// The slot's image as it will be saved, or nullopt. Throws on corrupt data.
    std::optional<nox::video::PackImage> read(std::uint32_t index);

    /// Puts an encoded image in slot `index` (the pack grows to hold it).
    void put(std::uint32_t index, nox::video::EncodedImage encoded, std::shared_ptr<const nox::video::PackImage> image);
    /// Empties slot `index`.
    void remove(std::uint32_t index);
    void revert(std::uint32_t index) { edits_.erase(index); }
    void revertAll() { edits_.clear(); }
    bool edited(std::uint32_t index) const { return edits_.count(index) != 0; }
    std::size_t editCount() const { return edits_.size(); }
    const std::map<std::uint32_t, PackEdit>& packEdits() const { return edits_; }

    /// Writes `source` (empty: a new pack) with `edits` to `out`, with at least `slots` slots.
    /// Thread-safe apart from documents.
    static PackSaveResult save(const std::filesystem::path& source, std::uint32_t slots, std::uint32_t sourceImages, std::uint32_t sourceIdxCrc,
                               const std::map<std::uint32_t, PackEdit>& edits, const std::filesystem::path& out,
                               const PackSaveOptions& options = {}, const Progress& progress = {});
    PackSaveResult saveAs(const std::filesystem::path& out, const PackSaveOptions& options = {},
                          const Progress& progress = {}) const
    {
        return save(path_, slots_, sourceImages_, sourceIdxCrc_, edits_, out, options, progress);
    }

private:
    std::filesystem::path path_;
    std::unique_ptr<nox::video::ImagePackReader> reader_;
    std::uint32_t slots_ = 0;
    std::uint32_t sourceImages_ = 0;
    std::uint32_t sourceIdxCrc_ = 0;
    bool hasBc7_ = false;
    std::map<std::uint32_t, PackEdit> edits_;
};

/// CRC-32 of a video.idx file (as packs record it).
std::uint32_t idxCrc(const std::filesystem::path& idx);

} // namespace videotools::editor
