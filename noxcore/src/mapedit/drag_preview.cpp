// A drag's live preview (drag_preview.hpp).
#include "noxcore/mapedit/drag_preview.hpp"

#include <utility>

namespace nox::mapedit {

void DragPreview::show(MapDocument& doc, Op op)
{
    clear(doc);
    if (op.empty()) {
        return;
    }
    inverse_ = applyOp(doc, op); // (throws with the document untouched: nothing shown)
    shown_ = std::move(op);
}

Op DragPreview::clear(MapDocument& doc)
{
    if (!inverse_) {
        return {};
    }
    applyOp(doc, *inverse_);
    inverse_.reset();
    return std::exchange(shown_, Op{});
}

} // namespace nox::mapedit
