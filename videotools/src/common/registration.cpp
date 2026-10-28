#include "common/registration.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace videotools {

IRect unite(const IRect& a, const IRect& b)
{
    if (a.empty()) {
        return b;
    }
    if (b.empty()) {
        return a;
    }
    const int x0 = std::min(a.x, b.x);
    const int y0 = std::min(a.y, b.y);
    const int x1 = std::max(a.x + a.w, b.x + b.w);
    const int y1 = std::max(a.y + a.h, b.y + b.h);
    return {x0, y0, x1 - x0, y1 - y0};
}

IRect Registration::canvasRect(std::size_t i) const
{
    const auto& f = frames.at(i);
    if (f.empty()) {
        return {};
    }
    return {f.offsetX - originX, f.offsetY - originY, f.width, f.height};
}

IRect Registration::contentRect() const
{
    IRect r;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        r = unite(r, canvasRect(i));
    }
    return r;
}

Registration registerFrames(std::span<const FrameGeometry> frames, const RegistrationOptions& options)
{
    int minX = std::numeric_limits<int>::max();
    int minY = minX;
    int maxX = std::numeric_limits<int>::min();
    int maxY = maxX;
    for (const auto& f : frames) {
        if (f.empty()) {
            continue;
        }
        minX = std::min(minX, f.offsetX);
        minY = std::min(minY, f.offsetY);
        maxX = std::max(maxX, f.offsetX + f.width);
        maxY = std::max(maxY, f.offsetY + f.height);
    }
    if (minX > maxX) {
        throw std::invalid_argument("registerFrames: every frame is empty");
    }
    const int pad = std::max(0, options.padding);
    const int align = std::max(1, options.align);
    const auto roundUp = [align](int v) { return (v + align - 1) / align * align; };

    Registration reg;
    reg.options = options;
    reg.frames.assign(frames.begin(), frames.end());
    reg.originX = minX - pad;
    reg.originY = minY - pad;
    reg.width = roundUp(maxX - minX + 2 * pad);
    reg.height = roundUp(maxY - minY + 2 * pad);
    return reg;
}

RgbaImage placeOnCanvas(const Registration& reg, std::size_t i, const RgbaImage& cropped)
{
    RgbaImage canvas(reg.width, reg.height);
    const IRect r = reg.canvasRect(i);
    if (!r.empty()) {
        if (cropped.width != r.w || cropped.height != r.h) {
            throw std::invalid_argument("placeOnCanvas: image size doesn't match the frame's geometry");
        }
        paste(canvas, cropped, r.x, r.y);
    }
    return canvas;
}

RgbaImage cropFromCanvas(const RgbaImage& canvasImage, const IRect& rect, int scale)
{
    const IRect s = rect.scaled(scale);
    return crop(canvasImage, s.x, s.y, s.w, s.h);
}

nlohmann::json toJson(const Registration& reg)
{
    nlohmann::json frames = nlohmann::json::array();
    for (std::size_t i = 0; i < reg.frames.size(); ++i) {
        const auto& f = reg.frames[i];
        const IRect r = reg.canvasRect(i);
        frames.push_back({{"frame", i},
                          {"empty", f.empty()},
                          {"offsetX", f.offsetX},
                          {"offsetY", f.offsetY},
                          {"width", f.width},
                          {"height", f.height},
                          {"canvasRect", {r.x, r.y, r.w, r.h}}});
    }
    const IRect c = reg.contentRect();
    return {{"coordinates", "canvas = noxOffset - origin; hd = canvas * scale; frame i's crop is canvasRect * scale"},
            {"origin", {reg.originX, reg.originY}},
            {"canvasSize", {reg.width, reg.height}},
            {"padding", reg.options.padding},
            {"align", reg.options.align},
            {"contentRect", {c.x, c.y, c.w, c.h}},
            {"frames", frames}};
}

} // namespace videotools
