// A mode's UI run with no server: the map editor's Mod Preview. The UI script runs in the client's own ScriptUiClient (script_ui.hpp) made without a
// server: started from its source as a server's would be (runLocal), the mod's assets handed to it
// (useAssets), and what it sends back (nox.send, nox.answer) shown as text instead of going
// anywhere.
//
// Test messages come from the mod's optional ui/preview.luau, run in a sandboxed VM of its own:
//
//   return {
//       { name = "Score 5", message = { score = 5 } },                 -- to ui.onMessage
//       { name = "A choice", view = { kind = "choice", title = "Pick", options = { "Red", "Blue" } } },
//   }
//
// each sent to the UI when picked (a view goes to ui.onView, as a server sends it to a UI that has
// onView; a UI without it gets nothing, where a player would get the text form).
#pragma once

#include "noxcore/modes/script/manifest.hpp"
#include "noxcore/modes/script/mod_assets.hpp"
#include "noxcore/modes/script/script_ui.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace nox::modes::script {

class UiPreview {
public:
    /// A test message of preview.luau's: its name, a view or a message, and its bytes as a
    /// server sends them (empty: it can't be sent, `problem` says why).
    struct Sample {
        std::string name;
        bool view = false;
        std::vector<std::uint8_t> value;
        std::string problem;
    };

    UiPreview();
    ~UiPreview();
    UiPreview(const UiPreview&) = delete;
    UiPreview& operator=(const UiPreview&) = delete;

    /// The UI started afresh from `uiSource` (as a server's would start it: its id and name the
    /// mode's; `api` the UI API mode.json names), with the mod's `assets` (null: none), and the
    /// samples read from `previewSource` (none: no samples).
    void load(const std::string& modeId, const std::string& modeName, std::string_view uiSource,
              std::optional<std::string_view> previewSource, int api = 1, std::shared_ptr<const ModAssets> assets = {});
    /// How wide text draws (unset: 6 a letter), as ScriptUiClient::setTextWidth.
    void setTextWidth(std::function<int(std::string_view)> measure);
    /// The thing types nox.thing names (API 2), from the next load.
    void setThings(const things::ThingDb* things) { things_ = things; }

    /// Once a frame: what was sent handed to the UI, then its windows drawn.
    void frame(std::uint64_t nowMs);
    const std::vector<UiWindow>& windows() const noexcept;
    /// A click inside window `id` (its own units; button 1 left, 2 right): whether the UI took it.
    bool click(std::string_view windowId, float x, float y, int button = 1);
    /// (API 2) A key over window `id` (script_ui.hpp's names): whether the UI took it.
    bool key(std::string_view windowId, std::string_view keyName);
    /// (API 2) Where the mouse is, in the UI's units (none: off the screen).
    void setMouse(std::optional<std::pair<float, float>> at);
    /// (API 3) A drag, a button let go and the wheel, as ScriptUiClient's.
    bool drag(std::string_view windowId, UiDrag phase, float x, float y, int button);
    bool mouseUp(std::string_view windowId, float x, float y, int button);
    bool wheel(std::string_view windowId, float x, float y, int steps);
    /// The window the mouse at (x, y) is over (ScriptUiClient::windowAt); none: none.
    const UiWindow* windowAt(float x, float y) const;
    /// (API 3) This frame's tooltip, and the labels (drawn at stand-in spots: there's no world).
    const std::vector<UiTooltipLine>& tooltip() const noexcept;
    const std::vector<UiLabel>& labels() const noexcept;
    bool running() const noexcept;
    /// Why the UI stopped (a compile or run error), or "" while it runs.
    const std::string& error() const noexcept;
    const std::string& modeName() const noexcept;
    /// The running UI's API (0: none).
    int api() const noexcept;
    /// The mod's assets the UI has (Image draws index them); null: none.
    std::shared_ptr<const ModAssets> assets() const;

    const std::vector<Sample>& samples() const noexcept { return samples_; }
    /// What's wrong with preview.luau ("" when it ran, or there's none).
    const std::string& previewError() const noexcept { return previewError_; }
    /// Sample `index` sent to the UI (false: no such sample, or it can't be sent).
    bool send(std::size_t index);

    /// What the UI sent its server since the last call, a line each ("send { clicked = "main" }",
    /// "answer 2", "ready", "failed: why"), and the sounds it asked for ("sound ding").
    std::vector<std::string> takeSent();

private:
    void readSamples(std::string_view source);
    std::string describe(std::span<const std::uint8_t> value);

    std::unique_ptr<ScriptUiClient> ui_;
    std::function<int(std::string_view)> textWidth_;
    const things::ThingDb* things_ = nullptr;
    std::vector<Sample> samples_;
    std::string previewError_;
    std::vector<std::string> sent_;
    std::string noName_;
    std::string noError_;
};

} // namespace nox::modes::script
