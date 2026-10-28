#include "exporter.hpp"

#include "json_writer.hpp"
#include "png_write.hpp"

#include "noxcore/io/file.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <set>

namespace fs = std::filesystem;

namespace videotools {

namespace {

constexpr int kFormatVersion = 1;
// Mask PNGs store code * 32 so the codes are visible and survive mild resampling
// (read back as round(value / 32)).
constexpr int kMaskScale = 32;

bool cancelled(const ExportContext& ctx) { return ctx.progress && ctx.progress->cancel.load(); }

bool isNumber(std::string_view s)
{
    if (s.starts_with('-')) {
        s.remove_prefix(1);
    }
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

void writeMask(const fs::path& path, int width, int height, const std::vector<std::uint8_t>& codes)
{
    std::vector<std::uint8_t> grey(codes.size());
    std::transform(codes.begin(), codes.end(), grey.begin(),
                   [](std::uint8_t c) { return static_cast<std::uint8_t>(std::min(255, c * kMaskScale)); });
    nox::writeFileBytes(path, encodePng(width, height, 1, grey));
}

void writeSource(JsonWriter& j, const ExportContext& ctx)
{
    j.key("source").beginObject();
    j.field("videoBag", ctx.paths.bag.generic_string());
    j.field("videoIdx", ctx.paths.idx.generic_string());
    j.field("thingBin", ctx.paths.thingBin.generic_string());
    j.endObject();
}

void writeEntry(JsonWriter& j, const Entry& e)
{
    j.key("entry").beginObject();
    j.field("name", e.name);
    j.field("source", e.source);
    if (!e.drawType.empty()) {
        j.field("drawType", e.drawType);
    }
    if (e.thingId) {
        j.field("thingId", e.thingId);
    }
    if (!e.attributes.empty()) {
        j.key("attributes").beginArray();
        for (const auto& a : e.attributes) {
            j.value(a);
        }
        j.endArray();
    }
    j.endObject();
}

void writeSequenceFields(JsonWriter& j, const Sequence& s)
{
    j.field("path", s.path());
    if (!s.group.empty()) {
        j.field("group", s.group);
    }
    j.field("name", s.name);
    if (!s.kind.empty()) {
        j.field("kind", s.kind);
    }
    if (s.frameDelay >= 0) {
        j.field("frameDelay", s.frameDelay);
        j.field("gameFramesPerFrame", s.frameDelay + 1);
    }
    if (s.state >= 0) {
        j.field("state", s.state);
    }
    if (!s.sound.empty()) {
        j.field("sound", s.sound);
    }
    j.field("trackKind", trackKindName(s.trackKind));
    j.key("tracks").beginArray();
    for (const auto& t : s.tracks) {
        j.beginObject().field("name", t.name).field("frames", t.frames.size()).endObject();
    }
    j.endArray();
    if (!s.info.empty()) {
        j.key("info").beginObject();
        for (const auto& [k, v] : s.info) {
            j.key(k);
            if (isNumber(v)) {
                j.raw(v);
            } else {
                j.value(v);
            }
        }
        j.endObject();
    }
}

} // namespace

std::string safeName(std::string_view name)
{
    std::string s;
    for (const char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        s += (c < 0x20 || std::string_view("<>:\"/\\|?*").find(ch) != std::string_view::npos) ? '_' : ch;
    }
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) {
        s.back() = '_';
    }
    if (s.empty()) {
        return "_";
    }
    // Device names Windows won't create as files, with or without an extension.
    static const std::set<std::string> kReserved = {"con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4",
                                                    "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3",
                                                    "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    if (kReserved.contains(lowered(s.substr(0, s.find('.'))))) {
        s.insert(s.begin(), '_');
    }
    return s;
}

std::string frameFileName(const Sequence& seq, const Track& track, std::size_t frame)
{
    std::size_t longest = 0;
    for (const auto& t : seq.tracks) {
        longest = std::max(longest, t.frames.size());
    }
    int digits = 2;
    for (std::size_t n = 100; n < longest; n *= 10) {
        ++digits;
    }
    const std::string number = std::format("{:0{}}", frame, digits);
    return track.name.empty() ? number + ".png" : safeName(track.name) + "_" + number + ".png";
}

fs::path entryDir(const fs::path& out, const Category& cat, const Entry& entry)
{
    return out / safeName(cat.name) / safeName(entry.name);
}

fs::path sequenceDir(const fs::path& out, const Category& cat, const Entry& entry, const Sequence& seq)
{
    fs::path dir = entryDir(out, cat, entry);
    if (!seq.group.empty()) {
        dir /= safeName(seq.group);
    }
    return dir / safeName(seq.name);
}

std::size_t exportSequence(ExportContext& ctx, const Category& cat, const Entry& entry, const Sequence& seq,
                           const fs::path& out)
{
    const fs::path dir = sequenceDir(out, cat, entry, seq);
    fs::create_directories(dir);
    if (ctx.progress) {
        ctx.progress->setCurrent(entry.name + "/" + seq.path());
    }

    JsonWriter j;
    j.beginObject();
    j.field("format", "noxvideo-export");
    j.field("formatVersion", kFormatVersion);
    writeSource(j, ctx);
    j.field("category", cat.name);
    writeEntry(j, entry);
    j.key("animation").beginObject();
    writeSequenceFields(j, seq);
    j.endObject();
    j.key("masks").beginObject();
    j.field("encoding", std::format("8-bit grey PNG, value = code * {}", kMaskScale));
    j.field("material", "0 plain colour, n = material slot n-1: the pixel holds a grey intensity the game "
                        "multiplies by that slot's colour");
    j.field("edge", "0 leave what is there, 1 the edge's own colour, 2 the floor being blended in");
    j.endObject();

    std::size_t written = 0;
    bool stopped = false;
    j.key("frames").beginArray();
    for (const auto& track : seq.tracks) {
        for (std::size_t f = 0; f < track.frames.size() && !stopped; ++f) {
            if (cancelled(ctx)) {
                stopped = true;
                break;
            }
            const std::int32_t index = track.frames[f];
            const std::string file = frameFileName(seq, track, f);
            j.beginObject();
            if (!track.name.empty()) {
                j.field("track", track.name);
            }
            j.field("frame", f);
            j.field("bagIndex", index);
            if (f < track.drawOffsets.size()) {
                j.key("drawOffset").beginArray().value(track.drawOffsets[f][0]).value(track.drawOffsets[f][1]).endArray();
            }
            try {
                if (index < 0) {
                    throw std::runtime_error("image reference didn't resolve to a video.bag image");
                }
                const auto& info = ctx.images.bag().images()[static_cast<std::size_t>(index)];
                const auto img = ctx.images.decode(index);
                // 0 x 0 sprites (NULL0001.pcx and the like) are frames that draw nothing.
                const bool empty = img.width <= 0 || img.height <= 0;
                if (empty) {
                    j.field("file", nullptr);
                    j.field("empty", true);
                    if (ctx.progress) {
                        ++ctx.progress->empty;
                    }
                } else {
                    nox::writeFileBytes(dir / file, encodePng(img.width, img.height, 4, img.rgba));
                    j.field("file", file);
                }
                j.field("bagName", info.name);
                j.field("imageType", info.type);
                j.field("width", img.width);
                j.field("height", img.height);
                j.field("offsetX", img.offsetX);
                j.field("offsetY", img.offsetY);
                j.field("headerByte", img.unknown);

                std::array<bool, 256> slots{};
                bool anyMaterial = false;
                for (const auto m : img.material) {
                    slots[m] = true;
                    anyMaterial = anyMaterial || m != 0;
                }
                if (anyMaterial && !empty) {
                    fs::create_directories(dir / "masks");
                    writeMask(dir / "masks" / file, img.width, img.height, img.material);
                    j.field("materialMask", "masks/" + file);
                    j.key("materialSlots").beginArray();
                    for (int s = 1; s < 256; ++s) {
                        if (slots[static_cast<std::size_t>(s)]) {
                            j.value(s - 1);
                        }
                    }
                    j.endArray();
                }
                if (!img.edgeMask.empty() && !empty) {
                    fs::create_directories(dir / "masks");
                    const std::string edgeFile = file.substr(0, file.size() - 4) + ".edge.png";
                    writeMask(dir / "masks" / edgeFile, img.width, img.height, img.edgeMask);
                    j.field("edgeMask", "masks/" + edgeFile);
                }
                written += empty ? 0 : 1;
            } catch (const std::exception& ex) {
                j.field("file", nullptr);
                j.field("error", ex.what());
                if (ctx.progress) {
                    ++ctx.progress->failed;
                }
            }
            j.endObject();
            if (ctx.progress) {
                ++ctx.progress->done;
            }
        }
    }
    j.endArray();
    if (stopped) {
        j.field("incomplete", true);
    }
    j.endObject();
    const std::string& text = j.str();
    nox::writeFileBytes(dir / "animation.json",
                        std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    return written;
}

std::size_t exportEntry(ExportContext& ctx, const Category& cat, const Entry& entry, const fs::path& out)
{
    std::size_t written = 0;
    for (const auto& seq : entry.sequences) {
        if (cancelled(ctx)) {
            break;
        }
        written += exportSequence(ctx, cat, entry, seq, out);
    }

    JsonWriter j;
    j.beginObject();
    j.field("format", "noxvideo-export");
    j.field("formatVersion", kFormatVersion);
    writeSource(j, ctx);
    j.field("category", cat.name);
    writeEntry(j, entry);
    j.field("icon", entry.icon);
    j.key("animations").beginArray();
    for (const auto& seq : entry.sequences) {
        fs::path rel = seq.group.empty() ? fs::path(safeName(seq.name)) : fs::path(safeName(seq.group)) / safeName(seq.name);
        j.beginObject();
        j.field("folder", rel.generic_string());
        writeSequenceFields(j, seq);
        j.endObject();
    }
    j.endArray();
    j.endObject();
    const std::string& text = j.str();
    const fs::path dir = entryDir(out, cat, entry);
    fs::create_directories(dir);
    nox::writeFileBytes(dir / "entry.json", std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    return written;
}

std::size_t exportCategory(ExportContext& ctx, const Category& cat, const fs::path& out)
{
    std::size_t written = 0;
    for (const auto& entry : cat.entries) {
        if (cancelled(ctx)) {
            break;
        }
        written += exportEntry(ctx, cat, entry, out);
    }
    return written;
}

} // namespace videotools
