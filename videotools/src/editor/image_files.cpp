#include "editor/image_files.hpp"

#include "common/image.hpp"
#include "exporter.hpp"
#include "png_write.hpp"

#include "noxcore/io/file.hpp"
#include "noxcore/video/tile_edges.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <stdexcept>

namespace fs = std::filesystem;

namespace videotools::editor {

namespace {

constexpr int kMaskScale = 32;

std::string utf8(const fs::path& p)
{
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path pathFromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

/// <dir>/<stem><suffix>
fs::path beside(const fs::path& png, std::string_view suffix)
{
    fs::path p = png.parent_path() / png.stem();
    p += std::string(suffix);
    return p;
}

void writeRgba(const fs::path& path, int w, int h, const std::vector<std::uint8_t>& rgba)
{
    nox::writeFileBytes(path, encodePng(w, h, 4, rgba));
}

} // namespace

std::string_view targetName(Target t)
{
    return t == Target::Bag ? "bag" : "pack";
}

Sidecar readSidecar(const fs::path& json)
{
    const auto bytes = nox::readFileBytes(json);
    const auto j = nlohmann::json::parse(bytes.begin(), bytes.end());
    if (j.value("format", std::string()) != "noxvideo-image") {
        throw std::runtime_error(utf8(json) + ": not an image sidecar");
    }
    Sidecar s;
    s.target = j.value("target", std::string("bag")) == "pack" ? Target::Pack : Target::Bag;
    s.index = j.value("index", -1);
    s.name = j.value("name", std::string());
    s.type = j.value("type", 0);
    s.width = j.value("width", 0);
    s.height = j.value("height", 0);
    s.offsetX = j.value("offsetX", 0);
    s.offsetY = j.value("offsetY", 0);
    s.headerByte = j.value("headerByte", 0);
    s.pixelWidth = j.value("pixelWidth", s.width);
    s.pixelHeight = j.value("pixelHeight", s.height);
    s.materialMask = j.value("materialMask", std::string());
    s.edgeMask = j.value("edgeMask", std::string());
    return s;
}

void writeSidecar(const fs::path& json, const Sidecar& s)
{
    nlohmann::ordered_json j = {
        {"format", "noxvideo-image"},
        {"formatVersion", 1},
        {"target", std::string(targetName(s.target))},
        {"index", s.index},
        {"name", s.name},
        {"type", s.type},
        {"width", s.width},
        {"height", s.height},
        {"offsetX", s.offsetX},
        {"offsetY", s.offsetY},
        {"headerByte", s.headerByte},
        {"pixelWidth", s.pixelWidth},
        {"pixelHeight", s.pixelHeight},
    };
    if (!s.materialMask.empty()) {
        j["materialMask"] = s.materialMask;
    }
    if (!s.edgeMask.empty()) {
        j["edgeMask"] = s.edgeMask;
    }
    j["note"] = "width/height: size in original pixels (where and how big it draws); offsetX/offsetY: draw "
                "offset in original pixels; masks: grey PNGs, value = code * 32";
    const std::string text = j.dump(1);
    nox::writeFileBytes(json, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::string fileStem(std::int32_t index, std::string_view name, Target target)
{
    return std::format("{:06}_{}{}", index, safeName(name.empty() ? std::string_view("image") : name),
                       target == Target::Pack ? ".hd" : "");
}

WrittenFiles extractBagImage(const nox::video::DecodedImage& image, std::int32_t index, std::string_view name,
                             const fs::path& dir)
{
    fs::create_directories(dir);
    const std::string stem = fileStem(index, name, Target::Bag);
    WrittenFiles out;
    Sidecar s;
    s.target = Target::Bag;
    s.index = index;
    s.name = std::string(name);
    s.type = image.type;
    s.width = s.pixelWidth = image.width;
    s.height = s.pixelHeight = image.height;
    s.offsetX = image.offsetX;
    s.offsetY = image.offsetY;
    s.headerByte = image.unknown;
    if (image.width > 0 && image.height > 0) {
        out.png = dir / pathFromUtf8(stem + ".png");
        writeRgba(out.png, image.width, image.height, image.rgba);
        if (!image.material.empty()) {
            out.mask = dir / pathFromUtf8(stem + ".mask.png");
            writeMask(out.mask, image.width, image.height, image.material);
            s.materialMask = utf8(out.mask.filename());
        }
        if (!image.edgeMask.empty()) {
            out.edge = dir / pathFromUtf8(stem + ".edge.png");
            writeMask(out.edge, image.width, image.height, image.edgeMask);
            s.edgeMask = utf8(out.edge.filename());
        }
    }
    out.json = dir / pathFromUtf8(stem + ".json");
    writeSidecar(out.json, s);
    return out;
}

WrittenFiles extractPackImage(const nox::video::PackImage& image, std::int32_t index, const fs::path& dir)
{
    fs::create_directories(dir);
    const std::string stem = fileStem(index, image.name, Target::Pack);
    WrittenFiles out;
    Sidecar s;
    s.target = Target::Pack;
    s.index = index;
    s.name = image.name;
    s.type = image.type;
    s.width = image.width;
    s.height = image.height;
    s.offsetX = image.offsetX;
    s.offsetY = image.offsetY;
    s.headerByte = image.headerByte;
    s.pixelWidth = image.pixelWidth;
    s.pixelHeight = image.pixelHeight;
    if (image.pixelWidth > 0 && image.pixelHeight > 0) {
        out.png = dir / pathFromUtf8(stem + ".png");
        writeRgba(out.png, image.pixelWidth, image.pixelHeight, image.rgba);
        if (!image.material.empty()) {
            out.mask = dir / pathFromUtf8(stem + ".mask.png");
            writeMask(out.mask, image.pixelWidth, image.pixelHeight, image.material);
            s.materialMask = utf8(out.mask.filename());
        }
        if (!image.edgeMask.empty()) {
            out.edge = dir / pathFromUtf8(stem + ".edge.png");
            writeMask(out.edge, image.pixelWidth, image.pixelHeight, image.edgeMask);
            s.edgeMask = utf8(out.edge.filename());
        }
    }
    out.json = dir / pathFromUtf8(stem + ".json");
    writeSidecar(out.json, s);
    return out;
}

ImageFile findImageFile(const fs::path& png)
{
    ImageFile f;
    f.png = png;
    const fs::path json = beside(png, ".json");
    if (fs::exists(json)) {
        f.sidecar = readSidecar(json);
    }
    // The sidecar names its masks; else the usual names.
    const auto mask = [&](const std::string& named, std::string_view suffix) {
        const fs::path p = named.empty() ? beside(png, suffix) : png.parent_path() / pathFromUtf8(named);
        return fs::exists(p) ? p : fs::path();
    };
    f.mask = mask(f.sidecar ? f.sidecar->materialMask : std::string(), ".mask.png");
    f.edge = mask(f.sidecar ? f.sidecar->edgeMask : std::string(), ".edge.png");
    return f;
}

std::vector<ImageFile> findImageFiles(const fs::path& dir)
{
    std::vector<fs::path> pngs;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file()) {
            continue;
        }
        const std::string name = utf8(e.path().filename());
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (!lower.ends_with(".png") || lower.ends_with(".mask.png") || lower.ends_with(".edge.png")) {
            continue;
        }
        pngs.push_back(e.path());
    }
    std::sort(pngs.begin(), pngs.end());
    std::vector<ImageFile> out;
    for (const auto& p : pngs) {
        out.push_back(findImageFile(p));
    }
    return out;
}

std::optional<std::pair<Target, std::int32_t>> fileSlot(const ImageFile& file)
{
    if (file.sidecar && file.sidecar->index >= 0) {
        return std::pair(file.sidecar->target, file.sidecar->index);
    }
    const std::string name = utf8(file.png.filename());
    std::int32_t index = -1;
    const auto r = std::from_chars(name.data(), name.data() + name.size(), index);
    if (r.ec != std::errc{} || r.ptr == name.data() || index < 0 || *r.ptr != '_') {
        return std::nullopt;
    }
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return std::pair(lower.ends_with(".hd.png") ? Target::Pack : Target::Bag, index);
}

std::vector<std::uint8_t> readMask(const fs::path& png, int width, int height)
{
    const RgbaImage img = readPng(png);
    if (img.width != width || img.height != height) {
        throw std::runtime_error(std::format("{}: {}x{}, the image is {}x{}", utf8(png.filename()), img.width,
                                             img.height, width, height));
    }
    std::vector<std::uint8_t> codes(std::size_t(width) * std::size_t(height));
    for (std::size_t i = 0; i < codes.size(); ++i) {
        codes[i] = std::uint8_t((img.rgba[i * 4] + kMaskScale / 2) / kMaskScale);
    }
    return codes;
}

void writeMask(const fs::path& png, int width, int height, const std::vector<std::uint8_t>& codes)
{
    std::vector<std::uint8_t> grey(codes.size());
    std::transform(codes.begin(), codes.end(), grey.begin(),
                   [](std::uint8_t c) { return std::uint8_t(std::min(255, c * kMaskScale)); });
    nox::writeFileBytes(png, encodePng(width, height, 1, grey));
}

nox::video::DecodedImage bagImageFromFile(const ImageFile& file, const nox::video::DecodedImage& current,
                                          std::vector<std::string>& warnings)
{
    const RgbaImage png = readPng(file.png);
    nox::video::DecodedImage img;
    img.type = current.type;
    img.unknown = current.unknown;
    img.width = png.width;
    img.height = png.height;
    img.rgba = png.rgba;
    img.offsetX = file.sidecar ? file.sidecar->offsetX : current.offsetX;
    img.offsetY = file.sidecar ? file.sidecar->offsetY : current.offsetY;
    if (img.type == 0 || img.type == 1) {
        img.offsetX = img.offsetY = 0; // tiles have no header
    }
    const bool sameSize = png.width == current.width && png.height == current.height;
    if (img.type == 1) {
        if (!file.edge.empty()) {
            img.edgeMask = readMask(file.edge, png.width, png.height);
        } else if (sameSize && !current.edgeMask.empty()) {
            img.edgeMask = current.edgeMask;
            warnings.push_back("no edge mask beside the PNG: kept the slot's");
        }
    } else if (!file.edge.empty()) {
        warnings.push_back("edge mask ignored: only tile edges (type 1) have one");
    }
    if (img.type >= 3 && img.type <= 6) {
        if (!file.mask.empty()) {
            img.material = readMask(file.mask, png.width, png.height);
        } else if (sameSize && !current.material.empty()) {
            img.material = current.material;
            warnings.push_back("no material mask beside the PNG: kept the slot's");
        } else if (!current.material.empty()) {
            warnings.push_back("the slot's image has material-shaded pixels, but the PNG has a new size and no mask: "
                               "none kept");
        }
    } else if (!file.mask.empty()) {
        warnings.push_back(std::format("material mask ignored: type {} has no material pixels", int(img.type)));
    }
    if (img.type == 8 || img.type == 2 || img.type == 7) {
        // 1-bit alpha in these types.
        std::size_t partial = 0;
        for (std::size_t i = 3; i < img.rgba.size(); i += 4) {
            partial += img.rgba[i] != 0 && img.rgba[i] != 255;
        }
        if (partial) {
            warnings.push_back(std::format("{} pixels with partial alpha: type {} has only on/off (cut at 128)", partial,
                                           int(img.type)));
        }
    }
    return img;
}

nox::video::PackImage packImageFromFile(const ImageFile& file, const nox::video::DecodedImage& bagImage,
                                        std::string_view name, const std::optional<nox::video::PackImage>& packImage,
                                        std::vector<std::string>& warnings)
{
    const RgbaImage png = readPng(file.png);
    nox::video::PackImage img;
    img.name = std::string(name);
    img.type = bagImage.type;
    img.headerByte = bagImage.unknown;
    img.width = bagImage.width;
    img.height = bagImage.height;
    img.offsetX = bagImage.offsetX;
    img.offsetY = bagImage.offsetY;
    if (packImage) {
        img.width = packImage->width;
        img.height = packImage->height;
        img.offsetX = packImage->offsetX;
        img.offsetY = packImage->offsetY;
    }
    if (file.sidecar && file.sidecar->width > 0 && file.sidecar->height > 0) {
        img.width = file.sidecar->width;
        img.height = file.sidecar->height;
        img.offsetX = file.sidecar->offsetX;
        img.offsetY = file.sidecar->offsetY;
    }
    img.pixelWidth = png.width;
    img.pixelHeight = png.height;
    img.rgba = png.rgba;
    if (img.width <= 0 || img.height <= 0) {
        // (A 0 x 0 slot: draw the PNG at its own size.)
        img.width = png.width;
        img.height = png.height;
    }
    const double sx = double(png.width) / img.width;
    const double sy = double(png.height) / img.height;
    if (std::abs(sx - sy) > 0.02 * std::max(sx, sy)) {
        warnings.push_back(std::format("the PNG is {}x{}, not in proportion to the image's {}x{}: it will be stretched",
                                       png.width, png.height, img.width, img.height));
    } else if (std::abs(sx - std::round(sx)) > 1e-9 || std::abs(sy - std::round(sy)) > 1e-9) {
        warnings.push_back(std::format("{:.3g}x the original size: not a whole scale", sx));
    }
    if (sx <= 1 && sy <= 1) {
        warnings.push_back("not larger than the original: NoX Core draws an HD image only when it's bigger");
    }
    if (!file.mask.empty()) {
        img.material = readMask(file.mask, png.width, png.height);
    } else if (!bagImage.material.empty() && bagImage.width > 0 && bagImage.height > 0) {
        // The original's plane, enlarged (nearest).
        img.material.resize(std::size_t(png.width) * std::size_t(png.height));
        for (int y = 0; y < png.height; ++y) {
            const int oy = std::min(bagImage.height - 1, int(std::floor((y + 0.5) * bagImage.height / png.height)));
            for (int x = 0; x < png.width; ++x) {
                const int ox = std::min(bagImage.width - 1, int(std::floor((x + 0.5) * bagImage.width / png.width)));
                img.material[std::size_t(y) * std::size_t(png.width) + std::size_t(x)] =
                    bagImage.material[std::size_t(oy) * std::size_t(bagImage.width) + std::size_t(ox)];
            }
        }
        warnings.push_back("no material mask beside the PNG: the original's enlarged");
    }
    if (!img.material.empty()) {
        // Code 0 where nothing is drawn.
        for (std::size_t i = 0; i < img.material.size(); ++i) {
            if (img.rgba[i * 4 + 3] == 0) {
                img.material[i] = 0;
            }
        }
        if (std::all_of(img.material.begin(), img.material.end(), [](std::uint8_t c) { return c == 0; })) {
            img.material.clear();
        }
    }
    if (img.type == 1) {
        // A tile edge: noxsdl blends its colour in by the original's mask made smooth, so the
        // colour goes past the opaque pixels (as the HD pack's edges are).
        std::vector<std::uint8_t> known(std::size_t(png.width) * std::size_t(png.height));
        for (std::size_t i = 0; i < known.size(); ++i) {
            known[i] = img.rgba[i * 4 + 3] >= 128 ? 1 : 0;
        }
        nox::video::spreadColour(img.rgba, known, png.width, png.height);
    }
    return img;
}

} // namespace videotools::editor
