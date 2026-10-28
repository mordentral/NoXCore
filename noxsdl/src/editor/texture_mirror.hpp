// Copies of textures for the editor's floating panels. An SDL_Renderer's textures draw only with
// that renderer, and each panel window that floats outside the main window has a renderer of its
// own: what the panels show (Dear ImGui's font atlas, thumbnails, previews, the minimap) is copied
// to each extra renderer on first use, from pixels kept here (or, for ImGui's own textures, its
// ImTextureData's), and kept up to date.
//
// This part knows nothing of SDL: the uploads go through a TextureUploader (SDL's in
// imgui_host.cpp, a fake in the tests).
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace noxsdl::editor {

/// A rectangle of texels (empty when w or h is 0).
struct TexelRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    /// The smallest rectangle holding both.
    TexelRect unite(const TexelRect& o) const;
    /// Cut to a texture of `width` x `height`.
    TexelRect clip(int width, int height) const;
};

/// Pixels to upload: `width` x `height` texels, rows `pitch` bytes apart, `bytesPerPixel` each (4:
/// `format`'s layout, the uploader's own code for it; 1: an alpha-only texture, white).
struct TexturePixels {
    const std::uint8_t* data = nullptr;
    int width = 0, height = 0, pitch = 0;
    int bytesPerPixel = 4;
    std::uint32_t format = 0;
    const std::uint8_t* at(int x, int y) const
    {
        return data + static_cast<std::ptrdiff_t>(y) * pitch + static_cast<std::ptrdiff_t>(x) * bytesPerPixel;
    }
};

/// Makes, updates and frees one renderer's textures. Handles and renderers are opaque numbers
/// (0 is none).
class TextureUploader {
public:
    virtual ~TextureUploader() = default;
    /// A texture on `renderer` holding all of `pixels`, or 0 if it can't be made.
    virtual std::uintptr_t create(std::uintptr_t renderer, const TexturePixels& pixels) = 0;
    /// `rect` of `texture` (made from pixels of the same size) from those pixels.
    virtual bool update(std::uintptr_t renderer, std::uintptr_t texture, const TexturePixels& pixels, const TexelRect& rect) = 0;
    virtual void destroy(std::uintptr_t renderer, std::uintptr_t texture) = 0;
};

/// Source textures (keyed by the main renderer's texture, or ImGui's ImTextureData) and their
/// copies on each extra renderer, made when first asked for and brought up to date as the source
/// changes. Main thread only.
class TextureMirror {
public:
    using Key = std::uintptr_t;
    using Renderer = std::uintptr_t;
    using Handle = std::uintptr_t;

    TextureMirror() = default;
    TextureMirror(const TextureMirror&) = delete;
    TextureMirror& operator=(const TextureMirror&) = delete;
    ~TextureMirror() { clear(); }

    /// Who makes and frees the copies (null: none can be made; copies already made are forgotten
    /// as their renderers go).
    void setUploader(TextureUploader* uploader) { uploader_ = uploader; }

    // --- Sources whose pixels are kept here (the panels' own images) ---

    /// `key`'s whole picture (made, or replaced: every copy made again).
    void setPixels(Key key, const TexturePixels& pixels);
    /// Part of `key`'s picture changed: the kept pixels updated and the copies told.
    void updatePixels(Key key, const TexelRect& rect, const std::uint8_t* data, int pitch);

    // --- Sources whose pixels live elsewhere (ImGui's ImTextureData), given to resolve() ---

    /// `key` is a texture of `width` x `height` whose pixels resolve() is given (a new size makes
    /// every copy again).
    void setExternal(Key key, int width, int height);
    /// Every copy of `key` made again from scratch next time (its texture was made anew).
    void invalidate(Key key);

    // --- Either kind ---

    /// Part of `key` changed (the copies upload that much next time).
    void markDirty(Key key, const TexelRect& rect);
    /// `key` gone: its copies freed on every renderer.
    void remove(Key key);
    bool has(Key key) const { return sources_.contains(key); }

    /// `key`'s copy on `renderer`, made or brought up to date now; 0 when there is no such source,
    /// its pixels aren't to hand, or the copy can't be made (tried again only once the source
    /// changes). `external`: an external source's pixels.
    Handle resolve(Renderer renderer, Key key, const TexturePixels* external = nullptr);

    /// A renderer gone (its window closed): its copies freed.
    void dropRenderer(Renderer renderer);
    /// Everything freed and forgotten.
    void clear();

    /// (Tests and the log.) The copies made on `renderer`, and the sources.
    std::size_t copies(Renderer renderer) const;
    std::size_t sources() const { return sources_.size(); }

private:
    struct Copy {
        Handle handle = 0;
        std::uint64_t generation = 0;      ///< the source's when made
        std::uint64_t failedGeneration = 0; ///< a create that failed at this generation (not retried)
        TexelRect dirty;
    };
    struct Source {
        int width = 0, height = 0;
        bool external = false;
        std::uint64_t generation = 0;
        std::vector<std::uint8_t> pixels; ///< tightly packed (width * bytesPerPixel a row); kept sources only
        int bytesPerPixel = 4;
        std::uint32_t format = 0;
        std::vector<std::pair<Renderer, Copy>> copies;
    };

    Copy* findCopy(Source& s, Renderer r);
    void freeCopies(Source& s);

    std::unordered_map<Key, Source> sources_;
    TextureUploader* uploader_ = nullptr;
    std::uint64_t nextGeneration_ = 1;
};

} // namespace noxsdl::editor
