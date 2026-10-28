// A drag's live preview in the map editor: the op the drag would perform on release, applied to
// the document in place while the mouse is held (so the view draws the things moving under it),
// and taken back before the drag's one op is performed on release, or for good when the drag is
// cancelled. It never reaches the history or a session: a drag is one op, one undo step, and a
// session's editors see it once it's let go (the editor holds the session's news while a preview
// is shown, as it does for a wall stroke).
//
// The op each frame is built from the document as it was (taken back first): show() after clear().
#pragma once

#include "noxcore/mapedit/ops.hpp"

#include <optional>

namespace nox::mapedit {

class DragPreview {
public:
    /// Shows `op`, built from the document without a preview (the one shown, if any, is taken back
    /// first). An empty op shows nothing. Throws FormatError, showing nothing, when the op doesn't
    /// fit the document.
    void show(MapDocument& doc, Op op);
    /// Takes the preview back, the document as it was before it; returns the op that was shown
    /// (empty if none), whose parts the view redraws.
    Op clear(MapDocument& doc);

    bool active() const noexcept { return inverse_.has_value(); }
    /// The op shown (empty if none).
    const Op& shown() const noexcept { return shown_; }

private:
    Op shown_;
    std::optional<Op> inverse_;
};

} // namespace nox::mapedit
