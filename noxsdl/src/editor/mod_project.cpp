// A mod project (mod_project.hpp).
#include "editor/mod_project.hpp"

#include "noxcore/mapedit/mod_files.hpp"
#include "noxcore/modes/registry.hpp"
#include "noxcore/modes/script/manifest.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace noxsdl::editor {

namespace {

/// mode.json's members in the order a new one is put among them.
constexpr std::string_view kOrder[] = {"api",    "id",     "name", "description", "order",     "players", "stockClients",
                                       "levels", "mapPrefix", "server", "ui",  "uiApi",     "looksLike", "options"};

fs::path u8path(std::string_view s)
{
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string u8text(const fs::path& p)
{
    const std::u8string s = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

/// `key` set in `doc`: in place, else after the last member that comes before it in kOrder.
void put(json::Value& doc, std::string_view key, json::Value v)
{
    if (doc.find(key)) {
        doc.set(key, std::move(v));
        return;
    }
    std::string_view after;
    for (const std::string_view k : kOrder) {
        if (k == key) {
            break;
        }
        if (doc.find(k)) {
            after = k;
        }
    }
    if (after.empty()) {
        // (Nothing of ours before it: first among the members.)
        doc.kind = json::Value::Kind::Object;
        doc.members.emplace(doc.members.begin(), std::string(key), std::move(v));
    } else {
        doc.setAfter(key, std::move(v), after);
    }
}

std::string textOf(const json::Value& doc, std::string_view key, std::string fallback)
{
    const json::Value* v = doc.find(key);
    return v && v->isString() ? v->text : fallback;
}

int intOf(const json::Value* v, int fallback)
{
    if (!v || !v->isNumber()) {
        return fallback;
    }
    const double d = v->number();
    return d >= -1e9 && d <= 1e9 ? static_cast<int>(std::lround(d)) : fallback;
}

ModOption optionOf(const json::Value& o)
{
    ModOption m;
    m.key = textOf(o, "key", "");
    m.label = textOf(o, "label", m.key);
    const std::string kind = textOf(o, "kind", "toggle");
    m.kind = kind == "choice" ? ModOption::Kind::Choice : kind == "number" ? ModOption::Kind::Number : kind == "text" ? ModOption::Kind::Text : ModOption::Kind::Toggle;
    if (const json::Value* c = o.find("choices"); c && c->isArray()) {
        for (const json::Value& v : c->items) {
            m.choices.push_back(v.isString() ? v.text : std::string());
        }
    }
    m.min = intOf(o.find("min"), 0);
    m.max = intOf(o.find("max"), 100);
    m.defaultValue = intOf(o.find("default"), m.kind == ModOption::Kind::Number ? m.min : 0);
    return m;
}

json::Value optionValue(const ModOption& m)
{
    json::Value o = json::Value::object();
    o.set("key", json::Value::of(m.key));
    if (!m.label.empty() && m.label != m.key) {
        o.set("label", json::Value::of(m.label));
    }
    switch (m.kind) {
    case ModOption::Kind::Toggle:
        o.set("kind", json::Value::of("toggle"));
        o.set("default", json::Value::of(m.defaultValue ? 1 : 0));
        break;
    case ModOption::Kind::Choice: {
        o.set("kind", json::Value::of("choice"));
        json::Value list = json::Value::array();
        for (const std::string& c : m.choices) {
            list.items.push_back(json::Value::of(c));
        }
        o.set("choices", std::move(list));
        o.set("default", json::Value::of(m.defaultValue));
        break;
    }
    case ModOption::Kind::Number:
        o.set("kind", json::Value::of("number"));
        o.set("min", json::Value::of(m.min));
        o.set("max", json::Value::of(m.max));
        o.set("default", json::Value::of(m.defaultValue));
        break;
    case ModOption::Kind::Text: o.set("kind", json::Value::of("text")); break;
    }
    return o;
}

bool writeWhole(const fs::path& path, std::string_view text, std::string& error)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".saving";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            error = "can't write " + u8text(path.filename());
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        error = "can't write " + u8text(path.filename()) + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::optional<std::string> readWhole(const fs::path& path, std::uintmax_t cap)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) > cap || ec) {
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// ---- The template: the smallest mode that does something, a UI window and its preview.

std::string templateManifest(const std::string& id, const std::string& name)
{
    json::Value doc = json::Value::object();
    doc.set("api", json::Value::of(1));
    doc.set("id", json::Value::of(id));
    doc.set("name", json::Value::of(name));
    doc.set("description", json::Value::of("A new game mode."));
    doc.set("players", json::Value::array({json::Value::of(1), json::Value::of(16)}));
    doc.set("stockClients", json::Value::of(true));
    doc.set("levels", json::Value::of("mapFiles"));
    doc.set("server", json::Value::of("server/main.luau"));
    doc.set("ui", json::Value::of("ui/main.luau"));
    return json::write(doc);
}

constexpr std::string_view kServerTemplate = R"(-- The server script: it runs on the host, in the game's frame. It returns a table of the hooks it
-- wants (the Mod Reference panel lists them and the toolbox, nox.*).
local mode = {}

-- A player came in: { peer, name, identified }.
function mode.onJoin(player)
    nox.say(player.peer, "Welcome, " .. player.name .. "!")
end

-- A chat line: true takes it (it isn't shown to the others).
function mode.onChat(player, text)
    if text == "hello" then
        nox.sayAll(player.name .. " says hello.")
        return true
    end
    return false
end

-- What the mode's UI sent (nox.send there).
function mode.onUiMessage(player, value)
    nox.send(player.peer, { text = player.name .. " clicked" })
end

return mode
)";

constexpr std::string_view kUiTemplate = R"(-- The UI script: it runs on each NoX Core player's client (stock players get the views as text).
-- It draws in its own windows, in the game's 640 x 480 units.
local ui = { text = "Hello" }

function ui.start()
    nox.window("main", { title = "Hello", x = 20, y = 60, w = 160, h = 40 })
end

-- What the server sent (nox.send there).
function ui.onMessage(value)
    if type(value) == "table" and value.text then
        ui.text = tostring(value.text)
    end
end

function ui.draw(window)
    nox.box(0, 0, 160, 40, { 0, 0, 0, 160 })
    nox.text(ui.text, 6, 14, "gold")
end

function ui.click(window, x, y)
    nox.send({ clicked = window })
    return true
end

return ui
)";

constexpr std::string_view kPreviewTemplate = R"(-- The editor's UI preview (never sent to players): test messages and views, each sent to the UI
-- with a click in the Mod Preview panel. A message goes to ui.onMessage, a view to ui.onView (a UI
-- without onView gets nothing: in a game, its player would get the view as text).
return {
    { name = "A message", message = { text = "Hello from the preview" } },
    { name = "A choice", view = { kind = "choice", title = "Pick a side", options = { "Red", "Blue" } } },
}
)";

} // namespace

// ---- The form ---------------------------------------------------------------------------------

ModManifest manifestForm(const json::Value& doc)
{
    ModManifest m;
    m.id = textOf(doc, "id", "");
    m.name = textOf(doc, "name", m.id);
    m.description = textOf(doc, "description", "");
    if (const json::Value* p = doc.find("players"); p && p->isArray() && p->items.size() == 2) {
        m.minPlayers = intOf(&p->items[0], 1);
        m.maxPlayers = intOf(&p->items[1], 16);
    }
    if (const json::Value* s = doc.find("stockClients"); s && s->isBool()) {
        m.stockClients = s->boolean;
    }
    m.generatedLevels = textOf(doc, "levels", "mapFiles") == "generated";
    m.mapPrefix = textOf(doc, "mapPrefix", "");
    m.serverScript = textOf(doc, "server", "server/main.luau");
    m.uiScript = textOf(doc, "ui", "");
    m.uiApi = intOf(doc.find("uiApi"), 1);
    m.looksLike = textOf(doc, "looksLike", "");
    if (const json::Value* o = doc.find("options"); o && o->isArray()) {
        for (const json::Value& v : o->items) {
            m.options.push_back(optionOf(v));
        }
    }
    return m;
}

void applyManifestForm(json::Value& doc, const ModManifest& form)
{
    const ModManifest was = manifestForm(doc);
    if (!doc.isObject()) {
        doc = json::Value::object();
    }
    if (!doc.find("api")) {
        put(doc, "api", json::Value::of(1));
    }
    const auto text = [&](std::string_view key, const std::string& now, const std::string& before, std::string_view absent) {
        if (now == before) {
            return;
        }
        if (now == absent && doc.find(key)) {
            doc.erase(key); // (back to the default: the field goes)
        } else {
            put(doc, key, json::Value::of(now));
        }
    };
    text("id", form.id, was.id, "\x01");
    if (form.name != was.name) {
        put(doc, "name", json::Value::of(form.name));
    }
    text("description", form.description, was.description, "\x01");
    if (form.minPlayers != was.minPlayers || form.maxPlayers != was.maxPlayers) {
        put(doc, "players", json::Value::array({json::Value::of(form.minPlayers), json::Value::of(form.maxPlayers)}));
    }
    if (form.stockClients != was.stockClients) {
        put(doc, "stockClients", json::Value::of(form.stockClients));
    }
    if (form.generatedLevels != was.generatedLevels) {
        put(doc, "levels", json::Value::of(form.generatedLevels ? "generated" : "mapFiles"));
    }
    text("mapPrefix", form.mapPrefix, was.mapPrefix, "");
    text("server", form.serverScript, was.serverScript, "\x01");
    text("ui", form.uiScript, was.uiScript, "");
    if (form.uiApi != was.uiApi) {
        if (form.uiApi == 1 && doc.find("uiApi")) {
            doc.erase("uiApi");
        } else {
            put(doc, "uiApi", json::Value::of(form.uiApi));
        }
    }
    text("looksLike", form.looksLike, was.looksLike, "");
    if (form.options != was.options) {
        if (form.options.empty()) {
            doc.erase("options");
        } else {
            json::Value list = json::Value::array();
            const json::Value* old = doc.find("options");
            for (std::size_t i = 0; i < form.options.size(); ++i) {
                // (An option the form left as it was keeps its own text, unknown fields and all.)
                const bool same = old && old->isArray() && i < was.options.size() && i < old->items.size() && was.options[i] == form.options[i];
                list.items.push_back(same ? old->items[i] : optionValue(form.options[i]));
            }
            put(doc, "options", std::move(list));
        }
    }
}

// ---- The project ------------------------------------------------------------------------------

std::optional<ModProject> ModProject::open(const fs::path& folder, std::string& error)
{
    const auto text = readWhole(folder / "mode.json", kMaxFileBytes);
    if (!text) {
        error = "no mode.json in " + u8text(folder.filename());
        return std::nullopt;
    }
    ModProject p;
    p.folder_ = folder;
    try {
        p.manifest_ = json::parse(*text);
    } catch (const json::ParseError& e) {
        error = std::string("mode.json: ") + e.what();
        return std::nullopt;
    }
    if (!p.manifest_.isObject()) {
        error = "mode.json must hold one object";
        return std::nullopt;
    }
    return p;
}

std::optional<ModProject> ModProject::create(const fs::path& modsDir, const std::string& id, const std::string& name, std::string& error)
{
    if (!nox::modes::script::validModeId(id) || id.size() > 24) {
        error = "an id is 1-24 of a-z, 0-9 and _";
        return std::nullopt;
    }
    const fs::path folder = modsDir / id;
    std::error_code ec;
    if (fs::exists(folder, ec)) {
        error = "mods\\" + id + " is there already";
        return std::nullopt;
    }
    const std::string title = name.empty() ? id : name.substr(0, 32);
    if (!writeWhole(folder / "mode.json", templateManifest(id, title), error) ||
        !writeWhole(folder / "server" / "main.luau", kServerTemplate, error) || !writeWhole(folder / "ui" / "main.luau", kUiTemplate, error) ||
        !writeWhole(folder / "ui" / "preview.luau", kPreviewTemplate, error)) {
        return std::nullopt;
    }
    fs::create_directories(folder / "assets", ec);
    return open(folder, error);
}

std::optional<ModProject> ModProject::copyOf(const fs::path& source, const fs::path& modsDir, const std::string& id, const std::string& name,
                                             std::string& error)
{
    if (!nox::modes::script::validModeId(id) || id.size() > 24) {
        error = "an id is 1-24 of a-z, 0-9 and _";
        return std::nullopt;
    }
    std::optional<ModProject> from = open(source, error);
    if (!from) {
        return std::nullopt;
    }
    const fs::path folder = modsDir / id;
    std::error_code ec;
    if (fs::exists(folder, ec)) {
        error = "mods\\" + id + " is there already";
        return std::nullopt;
    }
    for (const Entry& e : from->files()) {
        const fs::path to = folder / u8path(e.path);
        if (e.folder) {
            fs::create_directories(to, ec);
        } else if (e.path != "mode.json") {
            const auto bytes = from->read(e.path);
            if (!bytes || !writeWhole(to, *bytes, error)) {
                error = "can't copy " + e.path + (error.empty() ? "" : ": " + error);
                return std::nullopt;
            }
        }
    }
    json::Value doc = from->manifest_;
    doc.set("id", json::Value::of(id));
    doc.set("name", json::Value::of(name.empty() ? id : name.substr(0, 32)));
    doc.erase("reference");
    if (!writeWhole(folder / "mode.json", json::write(doc), error)) {
        return std::nullopt;
    }
    return open(folder, error);
}

std::string ModProject::folderName() const
{
    return u8text(folder_.filename());
}

std::string ModProject::manifestProblem() const
{
    const auto read = nox::modes::script::readManifest(manifestText());
    return read.manifest ? std::string() : read.error;
}

bool ModProject::saveManifest(std::string& error) const
{
    return writeWhole(folder_ / "mode.json", manifestText(), error);
}

bool ModProject::validPath(std::string_view rel)
{
    return nox::mapedit::modPathValid(rel); // (the rules a session's mod files keep too)
}

std::optional<fs::path> ModProject::pathOf(std::string_view rel) const
{
    // (Nothing on the way may lead out of the folder, a link or junction inside it neither; a name
    // the file system can't take is refused, never thrown.)
    return nox::mapedit::modPathIn(folder_, rel);
}

std::vector<ModProject::Entry> ModProject::files() const
{
    std::vector<Entry> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(folder_, fs::directory_options::none, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (out.size() >= kMaxEntries) {
            break;
        }
        const fs::directory_entry& e = *it;
        if (e.is_symlink(ec)) {
            it.disable_recursion_pending(); // (never followed out of the folder)
            continue;
        }
        const bool folder = e.is_directory(ec);
        if (folder && it.depth() >= 7) {
            it.disable_recursion_pending();
        }
        std::string rel;
        try {
            rel = u8text(fs::relative(e.path(), folder_, ec));
        } catch (const std::exception&) {
            continue; // (a name that can't be made text: left out)
        }
        if (ec || rel.empty() || rel.ends_with(".saving")) {
            ec.clear();
            continue;
        }
        out.push_back({rel, folder, folder ? 0 : e.file_size(ec)});
        ec.clear();
    }
    // A tree's order: each path as its parts, a folder's before a file's at each level, then by
    // name (a folder comes just before what's in it).
    const auto key = [](const Entry& e) {
        std::vector<std::pair<int, std::string_view>> parts;
        const std::string_view p = e.path;
        for (std::size_t i = 0; i <= p.size();) {
            const std::size_t slash = std::min(p.find('/', i), p.size());
            parts.emplace_back(slash < p.size() || e.folder ? 0 : 1, p.substr(i, slash - i));
            i = slash + 1;
        }
        return parts;
    };
    std::ranges::sort(out, [&](const Entry& a, const Entry& b) { return key(a) < key(b); });
    return out;
}

std::optional<std::string> ModProject::read(std::string_view rel) const
{
    const auto p = pathOf(rel);
    return p ? readWhole(*p, kMaxFileBytes) : std::nullopt;
}

bool ModProject::write(std::string_view rel, std::string_view text, std::string& error) const
{
    const auto p = pathOf(rel);
    if (!p) {
        error = "\"" + std::string(rel) + "\" isn't a path inside the mod";
        return false;
    }
    if (text.size() > kMaxFileBytes) {
        error = "over the 1 MB a file the game reads";
        return false;
    }
    return writeWhole(*p, text, error);
}

bool ModProject::newFile(std::string_view rel, std::string_view text, std::string& error) const
{
    const auto p = pathOf(rel);
    std::error_code ec;
    if (p && fs::exists(*p, ec)) {
        error = std::string(rel) + " is there already";
        return false;
    }
    return write(rel, text, error);
}

bool ModProject::newFolder(std::string_view rel, std::string& error) const
{
    const auto p = pathOf(rel);
    if (!p) {
        error = "\"" + std::string(rel) + "\" isn't a path inside the mod";
        return false;
    }
    std::error_code ec;
    if (fs::exists(*p, ec)) {
        error = std::string(rel) + " is there already";
        return false;
    }
    fs::create_directories(*p, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    return true;
}

bool ModProject::rename(std::string_view from, std::string_view to, std::string& error)
{
    const auto a = pathOf(from);
    const auto b = pathOf(to);
    if (!a || !b) {
        error = "\"" + std::string(a ? to : from) + "\" isn't a path inside the mod";
        return false;
    }
    if (from == "mode.json") {
        error = "mode.json keeps its name (the game looks for it)";
        return false;
    }
    std::error_code ec;
    if (!fs::exists(*a, ec)) {
        error = std::string(from) + " isn't there";
        return false;
    }
    if (fs::exists(*b, ec)) {
        error = std::string(to) + " is there already";
        return false;
    }
    if (std::string(to).starts_with(std::string(from) + "/")) {
        error = "a folder can't go inside itself";
        return false;
    }
    fs::create_directories(b->parent_path(), ec);
    fs::rename(*a, *b, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    // A script mode.json names follows it (moved itself, or inside a folder moved).
    bool changed = false;
    for (const std::string_view key : {std::string_view("server"), std::string_view("ui")}) {
        json::Value* v = manifest_.find(key);
        const std::string named = v && v->isString() ? v->text : key == "server" ? "server/main.luau" : "";
        if (named.empty()) {
            continue;
        }
        std::string now;
        if (named == from) {
            now = std::string(to);
        } else if (named.starts_with(std::string(from) + "/")) {
            now = std::string(to) + named.substr(from.size());
        } else {
            continue;
        }
        put(manifest_, key, json::Value::of(now));
        changed = true;
    }
    return !changed || saveManifest(error);
}

bool ModProject::remove(std::string_view rel, std::string& error) const
{
    if (rel == "mode.json") {
        error = "mode.json is the mod itself: it stays";
        return false;
    }
    const auto p = pathOf(rel);
    if (!p) {
        error = "\"" + std::string(rel) + "\" isn't a path inside the mod";
        return false;
    }
    std::error_code ec;
    fs::remove_all(*p, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    return true;
}

// ---- The mods folder --------------------------------------------------------------------------

std::vector<ModListing> listMods(const fs::path& modsDir)
{
    std::vector<ModListing> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(modsDir, ec)) {
        if (!e.is_directory(ec)) {
            continue;
        }
        ModListing l;
        l.folder = e.path();
        l.id = u8text(e.path().filename());
        l.name = l.id;
        std::string error;
        if (const auto p = ModProject::open(e.path(), error)) {
            const ModManifest m = p->form();
            l.id = m.id.empty() ? l.id : m.id;
            l.name = m.name.empty() ? l.id : m.name;
            const json::Value* ref = p->manifestDoc().find("reference");
            l.reference = ref && ref->isBool() && ref->boolean;
            l.problem = p->manifestProblem();
            l.mapPrefix = p->form().mapPrefix;
        } else {
            l.problem = error;
        }
        out.push_back(std::move(l));
    }
    std::ranges::sort(out, {}, [](const ModListing& l) { return u8text(l.folder.filename()); });
    return out;
}

std::string mapPrefixProblem(std::string_view prefix, const fs::path& modsDir, const fs::path& self, std::string_view selfId,
                             const nox::modes::Registry& registry)
{
    if (prefix.empty()) {
        return {};
    }
    if (prefix.size() < 2 || prefix.size() > 3 || !std::ranges::all_of(prefix, [](char c) { return c >= 'A' && c <= 'Z'; })) {
        return "a map prefix is 2-3 capital letters (A-Z)";
    }
    std::error_code ec;
    for (const ModListing& l : listMods(modsDir)) {
        if (l.mapPrefix == prefix && !fs::equivalent(l.folder, self, ec)) {
            return "mods\\" + u8text(l.folder.filename()) + " names its maps " + std::string(prefix) + "_ too";
        }
    }
    const std::string probe = std::string(prefix) + "_1";
    for (const nox::modes::Mode* m : registry.all()) {
        if (m->info.id != selfId && m->info.ownsMap && m->info.ownsMap(probe)) {
            return "the mode " + m->info.name + " names its maps " + std::string(prefix) + "_...";
        }
    }
    return {};
}

std::string freeCopyId(const fs::path& modsDir, const std::string& id)
{
    std::error_code ec;
    for (int n = 1; n < 1000; ++n) {
        const std::string tail = n == 1 ? "_copy" : "_copy" + std::to_string(n);
        const std::string candidate = id.substr(0, 24 - std::min<std::size_t>(24, tail.size())) + tail;
        if (!fs::exists(modsDir / candidate, ec)) {
            return candidate;
        }
    }
    return id;
}

} // namespace noxsdl::editor
