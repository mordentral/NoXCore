// The video.bag image and the pack's HD image side by side, or one at a time, both at the logical
// size (so an HD image covers its original exactly), with Fit / zoom, background and tint. The
// Images and Catalog views both draw their pictures with it.
#pragma once

#include "gui/session.hpp"

#include <functional>

namespace videotools::gui {

/// What one pane shows.
struct PaneView {
    const char* title = "";
    const Tex* image = nullptr; // nullptr: loading, or no pack open (HD pane)
    bool hd = false;            // the HD pane (its notes speak of the pack)
    bool standIn = false;       // `image` is the video.bag one, standing in for a missing HD image
    const char* note = nullptr; // said instead of the image's own note
    // The logical box that Fit and centring use (0 x 0: the image's own size), and where the
    // image's top-left goes in it.
    int canvasWidth = 0;
    int canvasHeight = 0;
    float x = 0;
    float y = 0;
    /// Drawn under / over the image: the box's top-left on screen and the zoom.
    std::function<void(ImDrawList*, ImVec2 origin, float zoom)> under;
    std::function<void(ImDrawList*, ImVec2 origin, float zoom)> over;
};

class CompareView {
public:
    enum Mode { SideBySide = 0, BagOnly = 1, HdOnly = 2 };

    explicit CompareView(Session& session) : s_(session) {}

    /// The mode is kept in the settings (one for both views, like zoom and background).
    int mode() const { return s_.settings().compare; }
    void setMode(int m) { s_.settings().compare = m; }
    bool tint() const { return tint_; }
    void setTint(bool on) { tint_ = on; }
    /// The Catalog's choice: the HD pane shows the video.bag image where the pack has none.
    bool fillFromBag() const { return fillFromBag_; }
    void setFillFromBag(bool on) { fillFromBag_ = on; }

    /// On the current line: the mode buttons, Fit, zoom, background and Tint (and "Fill from
    /// video.bag" if asked for).
    void controls(bool fillOption = false);
    /// Tab switches between video.bag and HD (when one is shown alone).
    void keys();
    /// The panes the mode shows, filling `size`.
    void draw(const PaneView& bag, const PaneView& hd, ImVec2 size);
    /// One pane.
    void pane(const PaneView& v, ImVec2 size);

private:
    Session& s_;
    bool fit_ = true;
    bool tint_ = false;
    bool fillFromBag_ = false;
};

} // namespace videotools::gui
