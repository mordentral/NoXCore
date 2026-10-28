// A mode's UI run with no server (ui_preview.hpp).
#include "noxcore/modes/script/ui_preview.hpp"

#include "noxcore/modes/script/ui_proto.hpp"

#include "value_codec.hpp"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace nox::modes::script {

namespace {

constexpr std::size_t kMaxSamples = 64;
constexpr std::size_t kMaxSentLines = 64;
constexpr std::size_t kDescribeCap = 300;

/// preview.luau's VM: small, short calls (it builds a few tables).
LuauVm::Limits previewLimits()
{
    LuauVm::Limits l;
    l.memoryBytes = 4u << 20;
    l.safepointsPerCall = 1'000'000;
    l.timePerCall = std::chrono::milliseconds(100);
    return l;
}

/// The value at `idx` as Luau-like text, short ({ clicked = "main" }).
void describeAt(lua_State* L, int idx, std::string& out, int depth)
{
    if (out.size() > kDescribeCap) {
        return;
    }
    switch (lua_type(L, idx)) {
    case LUA_TNIL: out += "nil"; break;
    case LUA_TBOOLEAN: out += lua_toboolean(L, idx) ? "true" : "false"; break;
    case LUA_TNUMBER: {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.14g", lua_tonumber(L, idx));
        out += buf;
        break;
    }
    case LUA_TSTRING: {
        size_t n = 0;
        const char* s = lua_tolstring(L, idx, &n);
        out += '"';
        out.append(s, std::min<size_t>(n, 80));
        out += n > 80 ? "...\"" : "\"";
        break;
    }
    case LUA_TTABLE: {
        if (depth >= 4) {
            out += "{...}";
            break;
        }
        const int t = lua_absindex(L, idx);
        out += "{ ";
        bool first = true;
        lua_pushnil(L);
        while (lua_next(L, t) != 0) {
            out += first ? "" : ", ";
            first = false;
            if (lua_type(L, -2) == LUA_TSTRING) {
                out += lua_tostring(L, -2);
                out += " = ";
            } else {
                out += "[";
                describeAt(L, -2, out, depth + 1);
                out += "] = ";
            }
            describeAt(L, -1, out, depth + 1);
            lua_pop(L, 1);
            if (out.size() > kDescribeCap) {
                lua_pop(L, 1);
                break;
            }
        }
        out += first ? "}" : " }";
        break;
    }
    default: out += lua_typename(L, lua_type(L, idx)); break;
    }
}

struct SampleVm {
    std::unique_ptr<LuauVm> vm = std::make_unique<LuauVm>(previewLimits());
};

SampleVm& describer()
{
    // (One VM for reading what UIs send, on the thread that previews: the editor's.)
    thread_local SampleVm v;
    return v;
}

} // namespace

UiPreview::UiPreview() = default;
UiPreview::~UiPreview() = default;

void UiPreview::load(const std::string& modeId, const std::string& modeName, std::string_view uiSource,
                     std::optional<std::string_view> previewSource, int api, std::shared_ptr<const ModAssets> assets)
{
    sent_.clear();
    ui_ = std::make_unique<ScriptUiClient>(); // (no server: what it sends comes here)
    ui_->setSender([this](std::vector<std::uint8_t> m) {
        const auto p = uimsg::parse(m);
        if (!p || sent_.size() >= kMaxSentLines) {
            return;
        }
        const auto& [sub, rest] = *p;
        uimsg::Reader r{rest};
        switch (sub) {
        case uimsg::kReady: sent_.push_back(std::string("ready") + (r.u8() ? " (it draws views)" : "")); break;
        case uimsg::kSend: sent_.push_back("send " + describe(rest)); break;
        case uimsg::kAnswer: sent_.push_back("answer " + std::to_string(r.u8())); break;
        case uimsg::kFailed: sent_.push_back("failed: " + r.str()); break;
        default: break;
        }
    });
    if (textWidth_) {
        ui_->setTextWidth(textWidth_);
    }
    if (things_) {
        ui_->setThings(things_);
    }
    // (nox.player: a stand-in, there being no game.)
    ui_->setPlayer(UiPlayer{.name = "Preview", .playerClass = 0, .level = 1, .health = 100, .maxHealth = 100, .mana = 50, .maxMana = 100});
    if (assets) {
        ui_->useAssets(std::move(assets));
    }
    ui_->runLocal(modeId, modeName, api, std::string(uiSource));
    samples_.clear();
    previewError_.clear();
    if (previewSource) {
        readSamples(*previewSource);
    }
}

void UiPreview::readSamples(std::string_view source)
{
    // A VM of its own each time: preview.luau's state never lives on.
    LuauVm vm(previewLimits());
    lua_State* L = vm.thread();
    std::string why;
    try {
        if (!vm.run("ui/preview.luau", source, 1, why)) {
            previewError_ = why;
            return;
        }
        if (!lua_istable(L, -1)) {
            previewError_ = "preview.luau must return a list of { name = ..., message = ... } or { name = ..., view = ... }";
            lua_pop(L, 1);
            return;
        }
        const int n = std::min<int>(lua_objlen(L, -1), static_cast<int>(kMaxSamples));
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            Sample s;
            if (!lua_istable(L, -1)) {
                s.name = "#" + std::to_string(i);
                s.problem = "not a table";
            } else {
                lua_rawgetfield(L, -1, "name");
                s.name = lua_isstring(L, -1) ? std::string(lua_tostring(L, -1)).substr(0, 64) : "#" + std::to_string(i);
                lua_pop(L, 1);
                lua_rawgetfield(L, -1, "view");
                s.view = !lua_isnil(L, -1);
                if (!s.view) {
                    lua_pop(L, 1);
                    lua_rawgetfield(L, -1, "message");
                }
                if (lua_isnil(L, -1)) {
                    s.problem = "it has neither a message nor a view";
                } else if (!encodeValue(L, lua_gettop(L), s.value, why)) {
                    s.value.clear();
                    s.problem = s.view ? why + " (a server sends such a view as text)" : why;
                } else if (s.view && !lua_istable(L, -1)) {
                    s.value.clear();
                    s.problem = "a view is a table ({ kind = \"say\", text = ... })";
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
            samples_.push_back(std::move(s));
        }
        lua_pop(L, 1);
    } catch (const std::exception& e) {
        previewError_ = std::string("preview.luau: ") + e.what();
    }
}

std::string UiPreview::describe(std::span<const std::uint8_t> value)
{
    lua_State* L = describer().vm->thread();
    std::string out;
    try {
        if (!decodeValue(L, value)) {
            return "(not a value)";
        }
        describeAt(L, -1, out, 0);
        lua_pop(L, 1);
    } catch (const std::exception&) {
        lua_settop(L, 0);
        return "(not a value)";
    }
    return out;
}

void UiPreview::setTextWidth(std::function<int(std::string_view)> measure)
{
    textWidth_ = std::move(measure);
    if (ui_) {
        ui_->setTextWidth(textWidth_);
    }
}

void UiPreview::frame(std::uint64_t nowMs)
{
    if (ui_) {
        ui_->frame(nowMs);
    }
}

const std::vector<UiWindow>& UiPreview::windows() const noexcept
{
    static const std::vector<UiWindow> none;
    return ui_ ? ui_->windows() : none;
}

bool UiPreview::click(std::string_view windowId, float x, float y, int button)
{
    return ui_ && ui_->click(windowId, x, y, button);
}

bool UiPreview::key(std::string_view windowId, std::string_view keyName)
{
    return ui_ && ui_->key(windowId, keyName);
}

void UiPreview::setMouse(std::optional<std::pair<float, float>> at)
{
    if (ui_) {
        ui_->setMouse(at);
    }
}

int UiPreview::api() const noexcept
{
    return ui_ ? ui_->api() : 0;
}

std::shared_ptr<const ModAssets> UiPreview::assets() const
{
    return ui_ ? ui_->assets() : nullptr;
}

bool UiPreview::running() const noexcept
{
    return ui_ && ui_->running();
}

const std::string& UiPreview::error() const noexcept
{
    return ui_ ? ui_->error() : noError_;
}

const std::string& UiPreview::modeName() const noexcept
{
    return ui_ ? ui_->modeName() : noName_;
}

bool UiPreview::send(std::size_t index)
{
    if (!ui_ || index >= samples_.size() || samples_[index].value.empty()) {
        return false;
    }
    const Sample& s = samples_[index];
    ui_->receive(uimsg::message(s.view ? uimsg::kView : uimsg::kData, s.value));
    return true;
}

std::vector<std::string> UiPreview::takeSent()
{
    if (ui_) {
        // (No sound plays in the editor: each one asked for is a line.)
        for (const UiSound& s : ui_->takeSounds()) {
            if (sent_.size() < kMaxSentLines) {
                const auto set = ui_->assets();
                const bool mine = !s.game && set && s.index >= 0 && static_cast<std::size_t>(s.index) < set->list.size();
                sent_.push_back(std::string(s.game ? "game sound " : "sound ") + (mine ? set->list[static_cast<std::size_t>(s.index)].name : s.name) +
                                (s.volume != 100 ? " at " + std::to_string(s.volume) : ""));
            }
        }
    }
    return std::exchange(sent_, {});
}

} // namespace nox::modes::script
