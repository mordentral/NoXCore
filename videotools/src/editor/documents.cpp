#include "editor/documents.hpp"

#include "common/bc7_encode.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/video/sprite_encode.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <stdexcept>

namespace fs = std::filesystem;

namespace videotools::editor {

namespace {

std::string utf8(const fs::path& p)
{
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

bool sameFile(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    if (fs::exists(a, ec) && fs::exists(b, ec)) {
        return fs::equivalent(a, b, ec);
    }
    return fs::weakly_canonical(a, ec) == fs::weakly_canonical(b, ec);
}

bool holdsGameExe(const fs::path& dir)
{
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string name = utf8(e.path().filename());
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (name == "game.exe") {
            return true;
        }
    }
    return false;
}

fs::path partPath(const fs::path& p)
{
    fs::path t = p;
    t += ".part";
    return t;
}

} // namespace

std::string checkSaveTarget(const fs::path& target, const std::vector<fs::path>& sources,
                            const std::vector<fs::path>& protectedDirs)
{
    if (target.empty() || !target.has_filename()) {
        return "no file name given";
    }
    for (const auto& s : sources) {
        if (!s.empty() && sameFile(target, s)) {
            return utf8(target) + " is an open file; save to a new one";
        }
    }
    std::error_code ec;
    const fs::path dir = fs::weakly_canonical(fs::absolute(target, ec).parent_path(), ec);
    for (const auto& d : protectedDirs) {
        if (!d.empty() && sameFile(dir, d)) {
            return utf8(dir) + " is the game's folder; save somewhere else (a copy of the game, say)";
        }
    }
    if (holdsGameExe(dir)) {
        return utf8(dir) + " holds GAME.EXE (a game install); save somewhere else";
    }
    return {};
}

fs::path idxBeside(const fs::path& bag)
{
    fs::path idx = bag;
    idx.replace_extension(".idx");
    return idx;
}

std::uint32_t idxCrc(const fs::path& idx)
{
    return nox::video::packCrc(nox::readFileBytes(idx));
}

// ------------------------------------------------------------------------------------------ bag

BagDocument::BagDocument(const GamePaths& paths) : paths_(paths), source_(paths) {}

nox::video::DecodedImage BagDocument::decode(std::int32_t index)
{
    if (const auto it = edits_.find(std::uint32_t(index)); index >= 0 && it != edits_.end()) {
        return nox::video::decodeImage(it->second, bag().images()[std::size_t(index)].type);
    }
    return source_.decode(index);
}

nox::video::DecodedImage BagDocument::decodeOriginal(std::int32_t index)
{
    return source_.decode(index);
}

nox::video::DecodedImage BagDocument::replace(std::int32_t index, nox::video::DecodedImage image)
{
    if (index < 0 || std::size_t(index) >= size()) {
        throw nox::FormatError(std::format("video.bag has no image #{}", index));
    }
    image.type = bag().images()[std::size_t(index)].type;
    auto bytes = nox::video::encodeImage(image);
    auto stored = nox::video::decodeImage(bytes, image.type);
    // An edit that gives back video.bag's own bytes is no edit.
    const auto original = source_.imageBytes(index);
    if (original == bytes) {
        edits_.erase(std::uint32_t(index));
    } else {
        edits_[std::uint32_t(index)] = std::move(bytes);
    }
    return stored;
}

void BagDocument::revert(std::int32_t index)
{
    edits_.erase(std::uint32_t(index));
}

nox::video::BagWriteResult BagDocument::save(const GamePaths& paths,
                                             const std::map<std::uint32_t, std::vector<std::uint8_t>>& edits,
                                             const fs::path& bag, const Progress& progress)
{
    nox::video::VideoBag source(paths.bag, paths.idx);
    return nox::video::writeVideoBag(source, edits, bag, idxBeside(bag), progress);
}

// ----------------------------------------------------------------------------------------- pack

nox::video::EncodedImage encodeForPack(const nox::video::PackImage& image, const PackEncodeOptions& options)
{
    if (!options.bc7 || image.pixelWidth <= 0 || image.pixelHeight <= 0) {
        return nox::video::encodePackImage(image, options.level);
    }
    nox::video::PackImage p = image;
    p.bc7 = encodeBc7Chain(p.rgba, p.pixelWidth, p.pixelHeight, options.bc7Lambda);
    p.rgba.clear();
    return nox::video::encodePackImage(p, options.level);
}

PackDocument::PackDocument(const fs::path& path)
    : path_(path), reader_(std::make_unique<nox::video::ImagePackReader>(path))
{
    slots_ = reader_->size();
    sourceImages_ = reader_->sourceImages();
    sourceIdxCrc_ = reader_->sourceIdxCrc();
    for (std::uint32_t i = 0; i < slots_ && !hasBc7_; ++i) {
        hasBc7_ = reader_->entry(i).present() && reader_->entry(i).codec == nox::video::kCodecBc7;
    }
}

PackDocument::PackDocument(std::uint32_t slots, std::uint32_t sourceImages, std::uint32_t sourceIdxCrc)
    : slots_(slots), sourceImages_(sourceImages), sourceIdxCrc_(sourceIdxCrc), hasBc7_(true)
{
}

std::uint32_t PackDocument::size() const
{
    std::uint32_t n = slots_;
    if (!edits_.empty()) {
        n = std::max(n, edits_.rbegin()->first + 1);
    }
    return n;
}

const nox::video::PackEntry* PackDocument::entry(std::uint32_t index) const
{
    if (const auto it = edits_.find(index); it != edits_.end()) {
        return it->second.encoded ? &it->second.encoded->entry : nullptr;
    }
    if (reader_ && index < slots_ && reader_->entry(index).present()) {
        return &reader_->entry(index);
    }
    return nullptr;
}

std::string PackDocument::name(std::uint32_t index) const
{
    if (const auto it = edits_.find(index); it != edits_.end()) {
        return it->second.encoded ? it->second.encoded->name : std::string();
    }
    if (reader_ && index < slots_ && reader_->entry(index).present()) {
        return std::string(reader_->name(index));
    }
    return {};
}

bool PackDocument::present(std::uint32_t index) const
{
    return entry(index) != nullptr;
}

std::optional<nox::video::PackImage> PackDocument::read(std::uint32_t index)
{
    if (const auto it = edits_.find(index); it != edits_.end()) {
        if (!it->second.encoded || !it->second.image) {
            return std::nullopt;
        }
        return *it->second.image;
    }
    if (!reader_ || index >= slots_ || !reader_->entry(index).present()) {
        return std::nullopt;
    }
    return reader_->read(index);
}

void PackDocument::put(std::uint32_t index, nox::video::EncodedImage encoded, std::shared_ptr<const nox::video::PackImage> image)
{
    encoded.entry.flags |= nox::video::kEntryPresent;
    edits_[index] = PackEdit{std::move(encoded), std::move(image)};
}

void PackDocument::remove(std::uint32_t index)
{
    if (reader_ && index < slots_ && reader_->entry(index).present()) {
        edits_[index] = PackEdit{};
    } else {
        edits_.erase(index); // nothing stored there: dropping the edit empties it
    }
}

PackSaveResult PackDocument::save(const fs::path& source, std::uint32_t minSlots, std::uint32_t sourceImages, std::uint32_t sourceIdxCrc,
                                  const std::map<std::uint32_t, PackEdit>& edits, const fs::path& out,
                                  const PackSaveOptions& options, const Progress& progress)
{
    std::optional<nox::video::ImagePackReader> reader;
    std::uint32_t slots = minSlots;
    if (!source.empty()) {
        reader.emplace(source);
        slots = std::max(slots, reader->size());
    }
    if (!edits.empty()) {
        slots = std::max(slots, edits.rbegin()->first + 1);
    }
    PackSaveResult result;
    result.slots = slots;
    const fs::path part = partPath(out);
    try {
        nox::video::ImagePackWriter writer(part, slots, sourceImages, options.idxCrc.value_or(sourceIdxCrc));
        for (std::uint32_t i = 0; i < slots; ++i) {
            if (progress && i % 512 == 0) {
                progress(i, slots);
            }
            if (const auto it = edits.find(i); it != edits.end()) {
                if (it->second.encoded) {
                    writer.put(i, *it->second.encoded);
                    ++result.written;
                } else {
                    ++result.removed;
                }
                continue;
            }
            if (reader && i < reader->size() && reader->entry(i).present()) {
                writer.putStored(i, reader->entry(i), reader->name(i), reader->readStored(i));
                ++result.copied;
            }
        }
        writer.finish();
    } catch (...) {
        std::error_code ec;
        fs::remove(part, ec);
        throw;
    }
    fs::rename(part, out);
    if (options.copySidecar && !source.empty()) {
        fs::path from = source;
        from += ".animations.json";
        fs::path to = out;
        to += ".animations.json";
        if (fs::exists(from)) {
            fs::copy_file(from, to, fs::copy_options::overwrite_existing);
            result.sidecarCopied = true;
        }
    }
    if (progress) {
        progress(slots, slots);
    }
    return result;
}

} // namespace videotools::editor
