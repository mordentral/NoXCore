#include "editor/insert.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <memory>
#include <stdexcept>

namespace videotools::editor {

std::vector<std::int32_t> parseIndexList(std::string_view text, std::int32_t count)
{
    std::vector<std::int32_t> out;
    const auto number = [&](std::string_view s) {
        while (!s.empty() && s.front() == ' ') {
            s.remove_prefix(1);
        }
        while (!s.empty() && s.back() == ' ') {
            s.remove_suffix(1);
        }
        if (!s.empty() && s.front() == '#') {
            s.remove_prefix(1);
        }
        std::int32_t n = 0;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), n);
        if (s.empty() || r.ec != std::errc{} || r.ptr != s.data() + s.size() || n < 0) {
            throw std::invalid_argument("not an index: " + std::string(s));
        }
        return n;
    };
    for (std::size_t p = 0; p <= text.size();) {
        const std::size_t comma = std::min(text.find(',', p), text.size());
        const std::string_view part = text.substr(p, comma - p);
        p = comma + 1;
        if (part.find_first_not_of(' ') == std::string_view::npos) {
            continue;
        }
        const std::size_t dash = part.find('-');
        const std::int32_t a = number(part.substr(0, dash));
        const std::int32_t b = dash == std::string_view::npos ? a : number(part.substr(dash + 1));
        for (std::int32_t i = a; i <= std::min(b, count - 1); ++i) {
            out.push_back(i);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

nox::video::DecodedImage bagImageForSlot(const ImageFile& file, std::int32_t index, BagDocument& bag,
                                         std::vector<std::string>& warnings)
{
    if (index < 0 || std::size_t(index) >= bag.size()) {
        throw std::runtime_error(std::format("video.bag has no image #{}", index));
    }
    return bagImageFromFile(file, bag.decode(index), warnings);
}

nox::video::PackImage packImageForSlot(const ImageFile& file, std::int32_t index, BagDocument& bag, PackDocument& pack,
                                       std::vector<std::string>& warnings)
{
    if (index < 0) {
        throw std::runtime_error("no slot");
    }
    const auto current = pack.read(std::uint32_t(index));
    if (std::size_t(index) < bag.size()) {
        return packImageFromFile(file, bag.decode(index), bag.bag().images()[std::size_t(index)].name, current, warnings);
    }
    // An extra slot: the pack's own image, else the sidecar, says what it is.
    nox::video::DecodedImage meta;
    std::string name;
    if (current) {
        meta.type = current->type;
        meta.unknown = current->headerByte;
        meta.width = current->width;
        meta.height = current->height;
        meta.offsetX = current->offsetX;
        meta.offsetY = current->offsetY;
        name = current->name;
    } else if (file.sidecar && file.sidecar->width > 0) {
        meta.type = std::uint8_t(file.sidecar->type);
        meta.unknown = std::uint8_t(file.sidecar->headerByte);
        meta.width = file.sidecar->width;
        meta.height = file.sidecar->height;
        meta.offsetX = file.sidecar->offsetX;
        meta.offsetY = file.sidecar->offsetY;
        name = file.sidecar->name;
    } else {
        throw std::runtime_error(std::format("slot {} is past video.bag's images and empty: give it a sidecar "
                                             "(type, width, height, offsets)",
                                             index));
    }
    return packImageFromFile(file, meta, name, current, warnings);
}

std::vector<InsertOutcome> insertFiles(const std::vector<ImageFile>& files, BagDocument& bag, PackDocument* pack,
                                       const PackEncodeOptions& encode)
{
    std::vector<InsertOutcome> out;
    for (const auto& file : files) {
        InsertOutcome o;
        o.png = file.png;
        try {
            const auto slot = fileSlot(file);
            if (!slot) {
                throw std::runtime_error("no sidecar and no index in the name (<index>_name.png)");
            }
            o.target = slot->first;
            o.index = slot->second;
            if (slot->first == Target::Bag) {
                bag.replace(o.index, bagImageForSlot(file, o.index, bag, o.warnings));
            } else {
                if (!pack) {
                    throw std::runtime_error("an HD image, but no pack is open");
                }
                auto image = std::make_shared<nox::video::PackImage>(packImageForSlot(file, o.index, bag, *pack, o.warnings));
                pack->put(std::uint32_t(o.index), encodeForPack(*image, encode), image);
            }
        } catch (const std::exception& ex) {
            o.error = ex.what();
        }
        out.push_back(std::move(o));
    }
    return out;
}

ExtractCounts extractImages(const ExtractRequest& request, const Progress& progress, const std::atomic<bool>* cancel)
{
    ExtractCounts counts;
    std::optional<ImageSource> bag;
    if (request.fromBag) {
        bag.emplace(request.paths);
    }
    std::optional<nox::video::ImagePackReader> pack;
    if (request.fromPack && !request.packPath.empty()) {
        pack.emplace(request.packPath);
    }
    const auto failed = [&](std::int32_t index, const std::string& what) {
        ++counts.failed;
        if (counts.errors.size() < 20) {
            counts.errors.push_back(std::format("#{}: {}", index, what));
        }
    };
    for (std::size_t k = 0; k < request.indexes.size(); ++k) {
        if (cancel && cancel->load()) {
            break;
        }
        if (progress && k % 16 == 0) {
            progress(k, request.indexes.size());
        }
        const std::int32_t i = request.indexes[k];
        if (bag && std::size_t(i) < bag->bag().images().size()) {
            try {
                const auto& info = bag->bag().images()[std::size_t(i)];
                const auto it = request.bagEdits.find(std::uint32_t(i));
                const auto img = it != request.bagEdits.end() ? nox::video::decodeImage(it->second, info.type) : bag->decode(i);
                extractBagImage(img, i, info.name, request.dir);
                ++counts.bag;
            } catch (const std::exception& ex) {
                failed(i, ex.what());
            }
        }
        if (request.fromPack) {
            try {
                std::optional<nox::video::PackImage> img;
                if (const auto it = request.packEdits.find(std::uint32_t(i)); it != request.packEdits.end()) {
                    if (it->second.encoded && it->second.image) {
                        img = *it->second.image;
                    }
                } else if (pack && std::uint32_t(i) < pack->size() && pack->entry(std::uint32_t(i)).present()) {
                    img = pack->read(std::uint32_t(i));
                }
                if (img) {
                    extractPackImage(*img, i, request.dir);
                    ++counts.pack;
                }
            } catch (const std::exception& ex) {
                failed(i, ex.what());
            }
        }
    }
    if (progress) {
        progress(request.indexes.size(), request.indexes.size());
    }
    return counts;
}

} // namespace videotools::editor