// The file a system file dialog picked, kept for the panel that opened it to take on its next frame.
// No SDL: the tests use it too (ui_helpers.hpp's openFileDialog / saveFileDialog show the dialogs).
//
// SDL's dialogs answer later, maybe on another thread, maybe after the panel (or the whole editor)
// that opened one is gone. So the place the answer goes is shared: each open dialog holds its own
// reference to it (ask()), dropped when the answer comes (answer()), and a panel gone by then
// leaves the answer in a slot nobody reads instead of in freed memory.
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace noxsdl::editor {

class FilePick {
public:
    /// The dialog's userdata: a reference to the slot, held till answer() takes it (exactly once).
    void* ask() { return new std::shared_ptr<Slot>(slot_); }

    /// A dialog's answer (`files` null on an error, empty when cancelled: nothing kept), the
    /// first file kept for take(); the dialog's reference let go.
    static void answer(void* userdata, const char* const* files)
    {
        const std::unique_ptr<std::shared_ptr<Slot>> held(static_cast<std::shared_ptr<Slot>*>(userdata));
        if (!held || !files || !files[0]) {
            return;
        }
        const std::lock_guard lock((*held)->mutex);
        (*held)->file = files[0];
    }

    /// The file picked since the last call, if any.
    std::optional<std::string> take()
    {
        const std::lock_guard lock(slot_->mutex);
        return std::exchange(slot_->file, std::nullopt);
    }

private:
    struct Slot {
        std::mutex mutex;
        std::optional<std::string> file;
    };
    std::shared_ptr<Slot> slot_ = std::make_shared<Slot>();
};

} // namespace noxsdl::editor
