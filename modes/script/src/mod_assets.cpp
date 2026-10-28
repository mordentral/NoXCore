// A mod's pictures and sounds (mod_assets.hpp).
#include "noxcore/modes/script/mod_assets.hpp"

#include "noxcore/audio/pcm.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <system_error>

// PNGs alone, from memory, every function local to this file (another library of the program may
// build stb_image too).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_FAILURE_USERMSG
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace nox::modes::script {

namespace fs = std::filesystem;

namespace {

/// A file under assets\ read whole: one at most this big (an image's 4 MB of pixels or the sounds'
/// 8 MB, with room for their containers).
constexpr std::uintmax_t kMaxAssetFile = 16u << 20;
/// How deep under assets\ its folders go.
constexpr int kMaxDepth = 8;

bool ignoredFile(const std::string& leaf)
{
    // (What Windows and other tools leave in a folder of pictures: not the mod's.)
    std::string lower = leaf;
    std::ranges::transform(lower, lower.begin(), [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
    return leaf.starts_with('.') || lower == "desktop.ini" || lower == "thumbs.db";
}

/// A PNG's sides from its signature and IHDR (its first 24 bytes; stb_image's own look reads on
/// past the chunks after it); false: not a PNG.
bool pngSides(std::span<const std::uint8_t> head, int& w, int& h)
{
    static constexpr std::uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (head.size() < 24 || !std::equal(std::begin(kSignature), std::end(kSignature), head.begin()) || head[12] != 'I' ||
        head[13] != 'H' || head[14] != 'D' || head[15] != 'R') {
        return false;
    }
    const auto be = [&](std::size_t at) {
        return static_cast<std::uint32_t>(head[at]) << 24 | static_cast<std::uint32_t>(head[at + 1]) << 16 |
               static_cast<std::uint32_t>(head[at + 2]) << 8 | head[at + 3];
    };
    if (be(8) != 13) {
        return false;
    }
    w = static_cast<int>(std::min<std::uint32_t>(be(16), 1u << 30));
    h = static_cast<int>(std::min<std::uint32_t>(be(20), 1u << 30));
    return true;
}

std::string sidesOver(int w, int h)
{
    return std::to_string(w) + " x " + std::to_string(h) + ": an image is at most " + std::to_string(kMaxAssetSide) + " x " +
           std::to_string(kMaxAssetSide);
}

std::uint64_t fnv(std::uint64_t h, const void* data, std::size_t n)
{
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) {
        h = (h ^ p[i]) * 0x100000001B3ull;
    }
    return h;
}

} // namespace

int ModAssets::find(std::string_view name, ModAsset::Kind kind) const
{
    const auto it = std::ranges::lower_bound(list, name, std::less<>{}, [](const ModAsset& a) { return std::string_view(a.name); });
    if (it == list.end() || it->name != name || it->kind != kind) {
        return -1;
    }
    return static_cast<int>(it - list.begin());
}

void ModAssets::tally()
{
    imageBytes = 0;
    soundBytes = 0;
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (const ModAsset& a : list) {
        (a.kind == ModAsset::Kind::Image ? imageBytes : soundBytes) += a.bytes();
        const std::uint32_t head[6] = {static_cast<std::uint32_t>(a.name.size()), static_cast<std::uint32_t>(a.kind),
                                       static_cast<std::uint32_t>(a.width), static_cast<std::uint32_t>(a.height), a.rate, a.channels};
        h = fnv(h, head, sizeof head);
        h = fnv(h, a.name.data(), a.name.size());
        h = a.kind == ModAsset::Kind::Image ? fnv(h, a.rgba.data(), a.rgba.size()) : fnv(h, a.samples.data(), a.samples.size() * 2);
    }
    hash = h;
}

bool validAssetName(std::string_view name)
{
    if (name.empty() || name.size() > kMaxAssetName || name.front() == '/' || name.back() == '/' || name.find("//") != std::string_view::npos) {
        return false;
    }
    return std::ranges::all_of(name, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '/'; });
}

std::string checkAsset(const ModAsset& a)
{
    if (!validAssetName(a.name)) {
        return "a name is lowercase a-z, 0-9, _ and / (at most " + std::to_string(kMaxAssetName) + ")";
    }
    if (a.kind == ModAsset::Kind::Image) {
        if (a.width < 1 || a.height < 1 || a.width > kMaxAssetSide || a.height > kMaxAssetSide) {
            return "an image is at most " + std::to_string(kMaxAssetSide) + " x " + std::to_string(kMaxAssetSide);
        }
        if (a.rgba.size() != static_cast<std::size_t>(a.width) * static_cast<std::size_t>(a.height) * 4) {
            return "an image's pixels aren't its size";
        }
        return {};
    }
    if (a.kind == ModAsset::Kind::Sound) {
        if (a.channels < 1 || a.channels > 2 || a.rate < 1 || a.rate > kMaxSoundRate) {
            return "a sound is mono or stereo, up to " + std::to_string(kMaxSoundRate) + " Hz";
        }
        if (a.samples.empty() || a.samples.size() % a.channels != 0) {
            return "a sound has no samples, or a part of one";
        }
        return {};
    }
    return "an asset is an image or a sound";
}

bool decodeAssetPng(std::span<const std::uint8_t> file, ModAsset& out, std::string& error)
{
    if (file.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "too big";
        return false;
    }
    const int size = static_cast<int>(file.size());
    int w = 0, h = 0, comp = 0;
    // (Its size first: a picture over the cap is never decoded.)
    if (!pngSides(file, w, h)) {
        error = "not a PNG";
        return false;
    }
    if (w < 1 || h < 1 || w > kMaxAssetSide || h > kMaxAssetSide) {
        error = sidesOver(w, h);
        return false;
    }
    stbi_uc* pixels = stbi_load_from_memory(file.data(), size, &w, &h, &comp, 4);
    if (!pixels) {
        error = std::string("a bad PNG (") + stbi_failure_reason() + ")";
        return false;
    }
    out.kind = ModAsset::Kind::Image;
    out.width = w;
    out.height = h;
    out.rgba.assign(pixels, pixels + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    stbi_image_free(pixels);
    return true;
}

bool decodeAssetWav(std::span<const std::uint8_t> file, ModAsset& out, std::string& error)
{
    std::optional<audio::WavInfo> info;
    try {
        info = audio::parseWav(file);
    } catch (const std::exception& e) {
        error = std::string("a bad WAV (") + e.what() + ")";
        return false;
    }
    if (!info) {
        error = "not a WAV";
        return false;
    }
    if (info->codec != audio::WavCodec::Pcm8 && info->codec != audio::WavCodec::Pcm16) {
        error = "a sound is PCM, 8 or 16 bit (not compressed)";
        return false;
    }
    if (info->channels < 1 || info->channels > 2 || info->rate > kMaxSoundRate) {
        error = "a sound is mono or stereo, up to " + std::to_string(kMaxSoundRate) + " Hz";
        return false;
    }
    const auto data = file.subspan(info->dataOffset, info->dataSize);
    out.kind = ModAsset::Kind::Sound;
    out.rate = info->rate;
    out.channels = static_cast<std::uint8_t>(info->channels);
    out.samples.clear();
    if (info->codec == audio::WavCodec::Pcm8) {
        out.samples.reserve(data.size());
        for (const std::uint8_t b : data) {
            out.samples.push_back(static_cast<std::int16_t>((static_cast<int>(b) - 128) * 256));
        }
    } else {
        out.samples.reserve(data.size() / 2);
        for (std::size_t i = 0; i + 1 < data.size(); i += 2) {
            out.samples.push_back(static_cast<std::int16_t>(data[i] | data[i + 1] << 8));
        }
    }
    out.samples.resize(out.samples.size() - out.samples.size() % out.channels); // (whole frames)
    if (out.samples.empty()) {
        error = "a sound with no samples";
        return false;
    }
    return true;
}

fs::file_time_type newestAsset(const fs::path& assetsDir)
{
    fs::file_time_type t{};
    std::error_code ec;
    for (fs::recursive_directory_iterator it(assetsDir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it.depth() > kMaxDepth) {
            it.disable_recursion_pending();
            continue;
        }
        t = std::max(t, it->last_write_time(ec));
    }
    return t;
}

namespace {

std::string failed(const std::string& rel, const std::string& why)
{
    return "assets/" + rel + ": " + why;
}

/// The folder's assets by name (in byte order: an asset's index is its place), their names and
/// count checked; the error otherwise.
std::string listAssetFiles(const fs::path& assetsDir, std::map<std::string, fs::path>& files)
{
    std::error_code ec;
    const auto fail = failed;
    for (fs::recursive_directory_iterator it(assetsDir, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            return "assets: " + ec.message();
        }
        const fs::path rel = it->path().lexically_relative(assetsDir);
        const std::string relText = rel.generic_string();
        if (ignoredFile(it->path().filename().string())) {
            if (it->is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (it->is_directory(ec)) {
            if (it.depth() >= kMaxDepth) {
                return fail(relText, "folders go " + std::to_string(kMaxDepth) + " deep at most");
            }
            continue;
        }
        if (!it->is_regular_file(ec)) {
            return fail(relText, "not a file");
        }
        const std::string ext = rel.extension().string();
        if (ext != ".png" && ext != ".wav") {
            return fail(relText, "an asset is a .png image or a .wav sound");
        }
        std::string name = relText.substr(0, relText.size() - ext.size());
        if (!validAssetName(name)) {
            return fail(relText, "a name is lowercase a-z, 0-9, _ and / (at most " + std::to_string(kMaxAssetName) + ")");
        }
        if (files.contains(name)) {
            return fail(relText, "\"" + name + "\" is another asset's name");
        }
        files.emplace(std::move(name), it->path());
        if (files.size() > kMaxAssets) {
            return fail(relText, "more than " + std::to_string(kMaxAssets) + " assets");
        }
    }
    return {};
}

/// The caps' sums: "" or why the set is over one.
std::string overCaps(const std::string& rel, std::size_t imageBytes, std::size_t soundBytes)
{
    if (imageBytes > kMaxImageBytes) {
        return failed(rel, "the images come to over " + std::to_string(kMaxImageBytes >> 20) + " MB decoded");
    }
    if (soundBytes > kMaxSoundBytes) {
        return failed(rel, "the sounds come to over " + std::to_string(kMaxSoundBytes >> 20) + " MB decoded");
    }
    return {};
}

/// A WAV's sizes from its chunks' headers alone (the data not read): its decoded bytes, or why
/// it isn't a sound mod_assets takes.
std::optional<std::size_t> peekWav(std::ifstream& f, std::uintmax_t size, std::string& why)
{
    const auto u16At = [](const char* p) { return static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[0]) | static_cast<std::uint8_t>(p[1]) << 8); };
    const auto u32At = [&](const char* p) { return u16At(p) | u16At(p + 2) << 16; };
    char head[12] = {};
    if (size < 12 || !f.read(head, 12) || std::string_view(head, 4) != "RIFF" || std::string_view(head + 8, 4) != "WAVE") {
        why = "not a WAV";
        return std::nullopt;
    }
    std::uintmax_t at = 12;
    std::uint32_t format = 0, channels = 0, rate = 0, bits = 0;
    bool fmt = false;
    while (at + 8 <= size) {
        char chunk[8];
        f.seekg(static_cast<std::streamoff>(at));
        if (!f.read(chunk, 8)) {
            break;
        }
        const std::string_view id(chunk, 4);
        const std::uint32_t length = u32At(chunk + 4);
        at += 8;
        if (id == "fmt ") {
            char b[16];
            if (length < 16 || length > size - at || !f.read(b, 16)) {
                why = "a bad WAV (short fmt chunk)";
                return std::nullopt;
            }
            format = u16At(b);
            channels = u16At(b + 2);
            rate = u32At(b + 4);
            bits = u16At(b + 14);
            fmt = true;
        } else if (id == "data") {
            if (!fmt) {
                why = "a bad WAV (data before fmt)";
                return std::nullopt;
            }
            if (format != 1) {
                why = "a sound is PCM, 8 or 16 bit (not compressed)";
                return std::nullopt;
            }
            if (bits != 8 && bits != 16) {
                why = "a bad WAV (unsupported PCM sample size " + std::to_string(bits) + ")";
                return std::nullopt;
            }
            if (channels < 1 || channels > 2 || rate < 1 || rate > kMaxSoundRate) {
                why = "a sound is mono or stereo, up to " + std::to_string(kMaxSoundRate) + " Hz";
                return std::nullopt;
            }
            const auto data = static_cast<std::size_t>(std::min<std::uintmax_t>(length, size - at)); // (cut to the file)
            std::size_t samples = data / (bits / 8);
            samples -= samples % channels;
            if (samples == 0) {
                why = "a sound with no samples";
                return std::nullopt;
            }
            return samples * 2;
        }
        at += std::min<std::uintmax_t>(length, size - at) + (length & 1);
    }
    why = "a bad WAV (no fmt or data chunk)";
    return std::nullopt;
}

} // namespace

std::string checkModAssets(const fs::path& assetsDir)
{
    std::error_code ec;
    if (!fs::is_directory(assetsDir, ec)) {
        return {};
    }
    std::map<std::string, fs::path> files;
    if (std::string error = listAssetFiles(assetsDir, files); !error.empty()) {
        return error;
    }
    std::size_t images = 0, sounds = 0;
    for (const auto& [name, path] : files) {
        const std::string relText = name + path.extension().string();
        const auto size = fs::file_size(path, ec);
        if (ec || size > kMaxAssetFile) {
            return failed(relText, ec ? "can't be read" : "over " + std::to_string(kMaxAssetFile >> 20) + " MB");
        }
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            return failed(relText, "can't be read");
        }
        std::string why;
        if (path.extension() == ".png") {
            // (Its signature and IHDR: the first 24 bytes.)
            std::array<std::uint8_t, 24> head{};
            f.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
            int w = 0, h = 0;
            if (!pngSides(std::span(head.data(), static_cast<std::size_t>(f.gcount())), w, h)) {
                return failed(relText, "not a PNG");
            }
            if (w < 1 || h < 1 || w > kMaxAssetSide || h > kMaxAssetSide) {
                return failed(relText, sidesOver(w, h));
            }
            images += static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
        } else {
            const auto bytes = peekWav(f, size, why);
            if (!bytes) {
                return failed(relText, why);
            }
            sounds += *bytes;
        }
        if (std::string over = overCaps(relText, images, sounds); !over.empty()) {
            return over;
        }
    }
    return {};
}

ModAssetsResult loadModAssets(const fs::path& assetsDir)
{
    ModAssetsResult out;
    auto set = std::make_shared<ModAssets>();
    std::error_code ec;
    if (!fs::is_directory(assetsDir, ec)) {
        set->tally();
        out.assets = std::move(set);
        return out;
    }
    // Each file's name first (by name, in order: an asset's index is its place), then decoded.
    std::map<std::string, fs::path> files;
    if (out.error = listAssetFiles(assetsDir, files); !out.error.empty()) {
        return out;
    }
    const auto fail = [&](const std::string& rel, const std::string& why) {
        out.error = failed(rel, why);
        return out;
    };
    for (const auto& [name, path] : files) {
        const std::string relText = name + path.extension().string();
        const auto size = fs::file_size(path, ec);
        if (ec || size > kMaxAssetFile) {
            return fail(relText, ec ? "can't be read" : "over " + std::to_string(kMaxAssetFile >> 20) + " MB");
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        std::ifstream f(path, std::ios::binary);
        if (!f || !f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
            return fail(relText, "can't be read");
        }
        ModAsset a;
        a.name = name;
        std::string why;
        const bool ok = path.extension() == ".png" ? decodeAssetPng(bytes, a, why) : decodeAssetWav(bytes, a, why);
        if (!ok) {
            return fail(relText, why);
        }
        if (const std::string bad = checkAsset(a); !bad.empty()) {
            return fail(relText, bad);
        }
        (a.kind == ModAsset::Kind::Image ? set->imageBytes : set->soundBytes) += a.bytes();
        if (out.error = overCaps(relText, set->imageBytes, set->soundBytes); !out.error.empty()) {
            return out;
        }
        set->list.push_back(std::move(a));
    }
    set->tally();
    out.assets = std::move(set);
    return out;
}

} // namespace nox::modes::script
