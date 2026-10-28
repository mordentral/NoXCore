// Writing catalog images to disk for upscaling: one folder per sequence, PNG frames, mask
// sidecars and an animation.json describing everything needed to put the images back.
//
// Layout (animation.json names its fields; the masks' encoding is written into it):
//   <out>/<Category>/<Entry>/entry.json              written by exportEntry / exportCategory
//   <out>/<Category>/<Entry>/<Sequence>/animation.json
//   <out>/<Category>/<Entry>/<Sequence>/<track>_<frame>.png   (<frame>.png without tracks)
//   <out>/<Category>/<Entry>/<Sequence>/masks/<same name>.png grey, value = code * 32
// Player sequences are grouped by state: <Entry>/<STATE>/<PART>/.
#pragma once

#include "catalog.hpp"
#include "image_source.hpp"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>

namespace videotools {

/// Shared between an export running on a worker thread and the UI showing it.
struct ExportProgress {
    std::atomic<std::size_t> done{0};  // images handled (written or failed)
    std::atomic<std::size_t> total{0}; // set by the caller
    std::atomic<std::size_t> failed{0};
    std::atomic<std::size_t> empty{0}; // 0 x 0 frames: recorded in the JSON, no PNG
    std::atomic<bool> cancel{false};

    void setCurrent(std::string s)
    {
        std::lock_guard lock(mutex_);
        current_ = std::move(s);
    }
    std::string current() const
    {
        std::lock_guard lock(mutex_);
        return current_;
    }

private:
    mutable std::mutex mutex_;
    std::string current_;
};

struct ExportContext {
    ImageSource& images;
    GamePaths paths; // recorded in the metadata
    ExportProgress* progress = nullptr;
};

/// A file or folder name safe on Windows: reserved characters become '_'.
std::string safeName(std::string_view name);

/// The PNG a frame is exported as, e.g. "S_03.png" (frame numbers are padded to the sequence's
/// longest track, at least two digits).
std::string frameFileName(const Sequence& seq, const Track& track, std::size_t frame);

std::filesystem::path entryDir(const std::filesystem::path& out, const Category& cat, const Entry& entry);
std::filesystem::path sequenceDir(const std::filesystem::path& out, const Category& cat, const Entry& entry,
                                  const Sequence& seq);

/// Each returns the number of PNGs written (empty frames and failures are recorded in the JSON
/// and counted in the progress). They stop early when progress->cancel is set.
std::size_t exportSequence(ExportContext& ctx, const Category& cat, const Entry& entry, const Sequence& seq,
                           const std::filesystem::path& out);
std::size_t exportEntry(ExportContext& ctx, const Category& cat, const Entry& entry, const std::filesystem::path& out);
std::size_t exportCategory(ExportContext& ctx, const Category& cat, const std::filesystem::path& out);

} // namespace videotools
