// The map editor's controls that are held or dragged (no ImGui here: the panels feed these each
// frame what their widget did, and tests feed them a hold or a drag frame by frame).
//
// HeldValue: a number field with - / + buttons. While the field is active (typed into, or a button
// held down and repeating) it keeps its own value, counting each repeat; let go, that value is
// committed once: one op, one undo entry, one session message. (Read back from the document every
// frame, a held button's repeats were lost: it went up by one however long it was held, or, where
// the field committed only on release, by none at all.)
//
// LiveDrag: a slider whose value is shown live on the map while it's dragged (the wall Variant):
// each new value is previewed in place (the editor's DragPreview), nothing goes to the history or
// a session until it's let go, then one op; Escape takes the preview back and does nothing.
//
// PickedColour: a colour picker holding what's being picked till the picking ends (the swatch's
// popup never activates the item, so a colour read back from the document each frame undid every
// pick), then one edit, not one a frame.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace noxsdl::editor {

template <class T>
class HeldValue {
public:
    /// Before the widget: the value it edits (the document's, unless it's being held or typed in).
    T& show(T current)
    {
        if (!held_) value_ = current;
        return value_;
    }
    /// After the widget: `active` it (its text or a button) is active, `deactivatedAfterEdit` it
    /// was let go after a change. The value to commit, on release only.
    std::optional<T> after(bool active, bool deactivatedAfterEdit, T current)
    {
        held_ = active;
        if (deactivatedAfterEdit) {
            held_ = false;
            if (value_ != current) return value_;
        }
        return std::nullopt;
    }
    /// Dropped (the widget wasn't drawn last frame: the field went, the selection changed).
    void reset() { held_ = false; }
    bool held() const { return held_; }

private:
    T value_{};
    bool held_ = false;
};

class LiveDrag {
public:
    enum class Step {
        None,    ///< nothing to do
        Show,    ///< preview value() in place of the last preview
        Cancel,  ///< take the preview back (Escape, or let go where it started)
        Commit,  ///< take the preview back, then perform the one op for value()
    };
    /// The value the slider shows: the dragged one while dragging, else the document's.
    int shown(int stored) const { return dragged_.value_or(stored); }
    /// After the slider each frame: `active` it's being dragged, `deactivated` it was let go this
    /// frame, `escape` Escape was pressed this frame, `value` the slider's value.
    Step frame(bool active, bool deactivated, bool escape, int value)
    {
        if (cancelled_) {
            // Escape: nothing more till it's let go.
            if (!active) cancelled_ = false;
            return Step::None;
        }
        if (escape && (active || deactivated) && dragged_) {
            cancelled_ = active;
            dragged_.reset();
            return Step::Cancel;
        }
        if (active) {
            const bool moved = dragged_ != value;
            dragged_ = value;
            return moved ? Step::Show : Step::None;
        }
        if (deactivated && dragged_) {
            dragged_.reset();
            value_ = value;
            return Step::Commit;
        }
        dragged_.reset();
        return Step::None;
    }
    /// The value to show (Show) or commit (Commit).
    int value() const { return dragged_.value_or(value_); }
    bool dragging() const { return dragged_.has_value(); }

private:
    std::optional<int> dragged_;
    int value_ = 0;
    bool cancelled_ = false;
};

class PickedColour {
public:
    using Rgb = std::array<std::uint8_t, 3>;
    /// Before the picker: the colour it edits, 0-1 a channel (the document's, unless it's being picked).
    std::array<float, 3>& show(Rgb current)
    {
        if (!picking_) {
            for (std::size_t k = 0; k < 3; ++k) f_[k] = static_cast<float>(current[k]) / 255.0f;
        }
        return f_;
    }
    /// After the picker: `changed` it changed the colour this frame, `ended` nothing is held any
    /// more (no item active, the mouse up). The colour to commit, once, when a picking ends with a
    /// new one.
    std::optional<Rgb> after(bool changed, bool ended, Rgb current)
    {
        if (changed) picking_ = true;
        if (!picking_ || !ended) return std::nullopt;
        picking_ = false;
        Rgb out{};
        for (std::size_t k = 0; k < 3; ++k) out[k] = static_cast<std::uint8_t>(std::lround(std::clamp(f_[k], 0.0f, 1.0f) * 255));
        if (out != current) return out;
        return std::nullopt;
    }
    /// Dropped (the picker wasn't drawn last frame).
    void reset() { picking_ = false; }
    bool picking() const { return picking_; }

private:
    std::array<float, 3> f_{};
    bool picking_ = false;
};

} // namespace noxsdl::editor
