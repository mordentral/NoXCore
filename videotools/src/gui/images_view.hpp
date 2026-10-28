// The editor view: every image by index, the video.bag image and the pack's HD image side by side
// with their facts, and extract / replace / remove / revert for the selection.
#pragma once

#include "gui/compare_view.hpp"
#include "gui/session.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace videotools::gui {

class ImagesView {
public:
    explicit ImagesView(Session& session) : s_(session), compare_(session) {}

    void draw(float height);
    /// Shows image `index` (selected alone, scrolled into view).
    void show(std::int32_t index);
    /// The list must be built again (documents opened or edited).
    void refresh() { filterUsed_ = "\x01"; }
    /// CompareView's mode (kept in the settings, shared with the Catalog).
    void setView(int v) { compare_.setMode(v); }
    /// Whether its keys (Tab) act: while its panel has the focus.
    void setKeys(bool on) { keys_ = on; }

private:
    void rebuild();
    void listPane(float height);
    void detailPane(float height);
    void factsTable(const char* id, const Tex* t, bool hd, std::int32_t index);
    void actions();
    void pollDialogs();
    std::vector<std::int32_t> selection() const;

    Session& s_;
    CompareView compare_;
    char filter_[128] = {};
    std::string filterUsed_ = "\x01";
    int typeFilter_ = -1; // -1 all
    int hdFilter_ = 0;    // 0 all, 1 with HD, 2 without
    bool editedOnly_ = false;
    std::vector<std::int32_t> list_;
    std::set<std::int32_t> selected_;
    std::int32_t focus_ = -1;
    std::int32_t anchor_ = -1;
    bool scrollToFocus_ = false;
    std::size_t knownEdits_ = 0;
    bool pendingBagReplace_ = false;
    std::int32_t replaceIndex_ = -1;
    int extractWhat_ = 0; // 1 bag, 2 pack, 3 both
    bool keys_ = true;
};

} // namespace videotools::gui
