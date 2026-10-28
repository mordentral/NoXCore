// The Sprites view (the editor's first browser, the Catalog): thing.bin's categories, entries and
// animations, played as the game plays them, exported with their metadata.
#pragma once

#include "catalog.hpp"
#include "common/registration.hpp"
#include "exporter.hpp"
#include "gui/compare_view.hpp"
#include "gui/session.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace videotools::gui {

struct Pick {
    int cat = -1;
    int entry = -1;
    bool operator==(const Pick&) const = default;
};

class CatalogView {
public:
    CatalogView(Session& session, std::function<void(std::int32_t)> openInEditor)
        : s_(session), compare_(session), openInEditor_(std::move(openInEditor))
    {
    }

    /// The catalog made again from `things` (the open game's thing.bin; null with why) and the
    /// open video.bag.
    void load(const nox::things::ThingDb* things, const std::string& thingsError = {});
    void draw(float height, double dt);

    void setOverlays(bool registration, bool onion)
    {
        showRegistration_ = registration;
        onionSkin_ = onion;
    }
    void setTint(bool on) { compare_.setTint(on); }
    void setFillFromBag(bool on) { compare_.setFillFromBag(on); }
    /// Whether its keys (Space, the arrows, Tab) act: while its panel has the focus.
    void setKeys(bool on) { keys_ = on; }
    /// Stops on frame `f` of the animation.
    void setFrame(int f)
    {
        frame_ = std::max(0, f);
        animTime_ = frame_;
        playing_ = false;
    }
    char* outBuf() { return outBuf_; }
    std::size_t outBufSize() const { return sizeof outBuf_; }
    const Catalog& catalog() const { return catalog_; }
    void exportEverything();

    /// Selects "Entry" or "Category/Entry", and optionally one of its animations by path.
    bool selectByName(std::string_view name, std::string_view animation, std::string_view track = {});
    /// The entry drawing thing type `thingId` (thing.bin index + 1), or nullptr.
    const Entry* entryForThing(int thingId) const;
    /// Selects that entry; false if there's none.
    bool selectThing(int thingId);

private:
    void refreshList();
    void select(Pick p);
    void selectSequence(int s);
    const Category* category() const { return pick_.cat >= 0 ? &catalog_.categories[std::size_t(pick_.cat)] : nullptr; }
    const Entry* entry() const
    {
        const Category* c = category();
        return c && pick_.entry >= 0 ? &c->entries[std::size_t(pick_.entry)] : nullptr;
    }
    const Sequence* sequence() const
    {
        const Entry* e = entry();
        return e && seq_ >= 0 && std::size_t(seq_) < e->sequences.size() ? &e->sequences[std::size_t(seq_)] : nullptr;
    }
    void leftPane(float height);
    void middlePane(float height);
    void previewPane(float height);
    std::optional<Registration> trackRegistration(const Track& track);
    void canvas(const Track& track, ImVec2 size);
    void frameStrip(const Track& track, float height);
    void frameInfo(const Sequence& seq, const Track& track);
    void handleKeys(const Sequence* seq);
    void advance(double dt);
    std::filesystem::path outDir() const { return pathFromUtf8(outBuf_); }
    void startExport(std::string label, std::filesystem::path folder, std::size_t total, std::function<std::size_t(ExportContext&)> run);

    Session& s_;
    CompareView compare_;
    std::function<void(std::int32_t)> openInEditor_;
    Catalog catalog_;
    std::string catalogError_;
    char outBuf_[1024] = {};
    char filterBuf_[128] = {};
    std::string filterUsed_ = "\x01";
    int listCat_ = 0;
    std::vector<Pick> list_;
    bool scrollListTop_ = false;
    bool scrollToPick_ = false;
    Pick pick_;
    int seq_ = -1;
    int track_ = 0;
    int frame_ = 0;
    bool playing_ = true;
    bool showRegistration_ = false;
    bool onionSkin_ = false;
    double animTime_ = 0;
    bool keys_ = true;
};

} // namespace videotools::gui
