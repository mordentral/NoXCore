// Content packs (content_pack.hpp).
#include "noxcore/content/content_pack.hpp"

#include "noxcore/content/csf_document.hpp"
#include "noxcore/content/thing_patch.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <exception>
#include <format>
#include <mutex>
#include <set>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace fs = std::filesystem;

namespace nox::content {

namespace {

std::mutex g_mutex;
std::vector<ContentPack> g_packs;
std::vector<std::string> g_log;
std::function<void(const std::string&)> g_sink;
std::set<std::string> g_logged; ///< lines logged since the packs were installed (a file loaded twice logs once)
NewThingTypes g_newTypes;       ///< the types the packs added as thing.bin last loaded

/// The appliers' lines to the log (the sink, else kept), each only once.
void forward(std::vector<std::string>& log)
{
    const std::lock_guard lock(g_mutex);
    for (auto& l : log) {
        if (!g_logged.insert(l).second) {
            continue;
        }
        if (g_sink) {
            g_sink(l);
        } else {
            g_log.push_back(std::move(l));
        }
    }
}

/// pack.txt's "title = ..." line, if any.
std::string titleOf(const fs::path& contentDir)
{
    std::error_code ec;
    const fs::path file = contentDir / "pack.txt";
    if (!fs::exists(file, ec)) {
        return {};
    }
    try {
        const auto bytes = readFileBytes(file);
        std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        while (!text.empty()) {
            const auto nl = text.find('\n');
            const std::string_view line = util::trim(text.substr(0, nl));
            text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
            const auto eq = line.find('=');
            if (eq != std::string_view::npos && util::equalsNoCase(util::trim(line.substr(0, eq)), "title")) {
                return std::string(util::trim(line.substr(eq + 1)));
            }
        }
    } catch (const std::exception&) {
    }
    return {};
}

} // namespace

std::string patchFileName(TableFormat format)
{
    const std::string file = tableFileName(format); // ("monster.bin" -> "monster.patch")
    return file.substr(0, file.find('.')) + ".patch";
}

std::vector<ContentPack> findContentPacks(const fs::path& modsDir)
{
    std::vector<ContentPack> out;
    std::error_code ec;
    for (fs::directory_iterator it(modsDir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) {
            continue;
        }
        const fs::path content = findNoCase(it->path(), "content");
        if (!fs::is_directory(content, ec)) {
            continue;
        }
        ContentPack p;
        p.id = it->path().filename().string();
        p.dir = content;
        for (const TableFormat f : kTableFormats) {
            if (fs::exists(findNoCase(content, patchFileName(f)), ec)) {
                p.tables.push_back(f);
            }
        }
        p.things = fs::exists(findNoCase(content, "thing.patch"), ec);
        p.strings = fs::exists(findNoCase(content, "strings.patch"), ec);
        p.audio = fs::is_directory(findNoCase(content, "audio"), ec);
        p.dialog = fs::is_directory(findNoCase(content, "dialog"), ec);
        p.music = fs::is_directory(findNoCase(content, "music"), ec);
        if (p.tables.empty() && !p.things && !p.strings && !p.audio && !p.dialog && !p.music) {
            continue;
        }
        p.title = titleOf(content);
        if (p.title.empty()) {
            p.title = p.id;
        }
        out.push_back(std::move(p));
    }
    std::ranges::sort(out, [](const ContentPack& a, const ContentPack& b) { return util::compareNoCase(a.id, b.id) < 0; });
    return out;
}

std::vector<std::string> installContentPacks(std::vector<ContentPack> packs)
{
    std::vector<std::string> lines;
    for (const ContentPack& p : packs) {
        std::string files;
        for (const TableFormat f : p.tables) {
            files += (files.empty() ? "" : ", ") + patchFileName(f);
        }
        if (p.things) {
            files += (files.empty() ? "" : ", ") + std::string("thing.patch");
        }
        if (p.strings) {
            files += (files.empty() ? "" : ", ") + std::string("strings.patch");
        }
        if (p.audio) {
            files += (files.empty() ? "" : ", ") + std::string("audio\\");
        }
        if (p.dialog) {
            files += (files.empty() ? "" : ", ") + std::string("dialog\\");
        }
        if (p.music) {
            files += (files.empty() ? "" : ", ") + std::string("music\\");
        }
        lines.push_back("content pack " + p.id + (p.title != p.id ? " (" + p.title + ")" : std::string()) + ": " + files);
    }
    {
        const std::lock_guard lock(g_mutex);
        g_packs = std::move(packs);
        g_logged.clear();
        g_newTypes = {}; // (the next thing.bin load's)
    }
    things::setAddedTypeBases({}); // (and their stock types: the next load's)
    return lines;
}

std::vector<std::string> installContentPacks(const fs::path& modsDir)
{
    return installContentPacks(findContentPacks(modsDir));
}

std::vector<ContentPack> installedContentPacks()
{
    const std::lock_guard lock(g_mutex);
    return g_packs;
}

bool applyContentPacks(TableFormat format, std::vector<std::uint8_t>& plain, const std::vector<ContentPack>& packs, std::vector<std::string>* log)
{
    bool any = false;
    for (const ContentPack& p : packs) {
        any = any || std::ranges::find(p.tables, format) != p.tables.end();
    }
    if (!any) {
        return false; // (the file as it is: nothing parsed, nothing copied)
    }
    TextTable base = TextTable::parse(format, std::string(plain.begin(), plain.end()));
    for (const ContentPack& p : packs) {
        if (std::ranges::find(p.tables, format) == p.tables.end()) {
            continue;
        }
        const fs::path file = findNoCase(p.dir, patchFileName(format));
        try {
            const auto bytes = readFileBytes(file);
            const TextTable patch = TextTable::parsePatch(format, std::string(bytes.begin(), bytes.end()));
            // On a copy, kept only when the whole patch went in (a pack is applied whole or not at all).
            TextTable trial = base;
            const PatchResult r = applyPatch(trial, patch);
            const std::string where = p.id + "/" + patchFileName(format);
            if (r.problems.empty()) {
                base = std::move(trial);
            }
            if (log) {
                if (r.problems.empty()) {
                    log->push_back(where + ": " + std::to_string(r.changed.size()) + " changed, " + std::to_string(r.added.size()) + " added, " +
                                   std::to_string(r.removed.size()) + " removed");
                } else {
                    log->push_back(where + ": not applied (" + std::to_string(r.problems.size()) + " problems)");
                }
                for (const auto& problem : r.problems) {
                    log->push_back(where + ": " + problem);
                }
            }
        } catch (const std::exception& ex) {
            if (log) {
                log->push_back(p.id + "/" + patchFileName(format) + ": not applied: " + ex.what());
            }
        }
    }
    // The text as the game's reader takes it (its zero padding kept; the readers stop at it).
    const std::string& text = base.text();
    plain.assign(text.begin(), text.end());
    return true;
}

bool applyContentPacks(TableFormat format, std::vector<std::uint8_t>& plain)
{
    std::vector<ContentPack> packs;
    {
        const std::lock_guard lock(g_mutex);
        if (g_packs.empty()) {
            return false;
        }
        packs = g_packs;
    }
    std::vector<std::string> log;
    const bool applied = applyContentPacks(format, plain, packs, &log);
    forward(log);

    return applied;
}

void setContentPackLog(std::function<void(const std::string&)> sink)
{
    const std::lock_guard lock(g_mutex);
    g_sink = std::move(sink);
}

std::uint64_t newThingTypesHash(const things::ThingDb& db, std::size_t first)
{
    if (first >= db.things.size()) {
        return 0;
    }
    std::uint64_t h = 14695981039346656037ull; // (FNV-1a)
    const auto bytes = [&](const void* data, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            h = (h ^ static_cast<const std::uint8_t*>(data)[i]) * 1099511628211ull;
        }
    };
    const auto text = [&](std::string_view s) {
        bytes(s.data(), s.size());
        bytes("", 1); // (each one ended, so "ab"+"c" isn't "a"+"bc")
    };
    const auto number = [&](std::int64_t v) {
        for (int i = 0; i < 8; ++i) {
            const auto b = static_cast<std::uint8_t>(static_cast<std::uint64_t>(v) >> (8 * i));
            bytes(&b, 1);
        }
    };
    const auto image = [&](const std::optional<things::ImageRef>& ref) {
        number(ref ? 1 : 0);
        if (ref) {
            number(ref->index);
            number(ref->type);
            text(ref->name);
        }
    };
    number(static_cast<std::int64_t>(first)); // (where they start: the same types after other ones aren't the same numbers)
    for (std::size_t i = first; i < db.things.size(); ++i) {
        const things::ThingDef& t = db.things[i];
        text(t.name);
        number(static_cast<std::int64_t>(t.attributes.size()));
        for (const auto& a : t.attributes) {
            text(a);
        }
        number(t.draw ? 1 : 0);
        if (t.draw) {
            text(t.draw->type);
            number(static_cast<std::int64_t>(t.draw->data.size()));
            bytes(t.draw->data.data(), t.draw->data.size());
        }
        image(t.menuIcon);
        image(t.prettyImage);
    }
    return h == 0 ? 1 : h;
}

bool applyContentPacksToThings(things::ThingDb& db, const std::vector<ContentPack>& packs, std::vector<std::string>* log, NewThingTypes* added)
{
    bool any = false;
    const std::size_t first = db.things.size();
    NewThingTypes types;
    std::unordered_map<std::string, std::size_t> baseOf; ///< an added type's entry in types.bases
    for (const ContentPack& p : packs) {
        if (!p.things) {
            continue;
        }
        any = true;
        try {
            const auto bytes = readFileBytes(findNoCase(p.dir, "thing.patch"));
            std::vector<std::string> problems;
            const ThingPatch patch = parseThingPatch(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &problems);
            // On a copy, kept only when the whole patch went in.
            things::ThingDb trial = db;
            const ThingPatchResult r = applyThingPatch(trial, patch);
            const bool clean = problems.empty() && r.problems.empty();
            if (clean) {
                db = std::move(trial);
                if (!r.added.empty()) {
                    types.packs.push_back(p.id);
                }
                // Each NEW record's type (all added: the patch is clean) and the stock type it's
                // made like: its LIKE's, when that's a type added before it, else its LIKE.
                for (const ThingPatchRecord& rec : patch.records) {
                    if (!rec.isNew()) {
                        continue;
                    }
                    const auto earlier = baseOf.find(rec.like);
                    std::string stock = earlier != baseOf.end() ? types.bases[earlier->second].second : rec.like;
                    baseOf.try_emplace(rec.name, types.bases.size());
                    types.bases.emplace_back(rec.name, std::move(stock));
                }
            }
            if (log) {
                log->push_back(p.id + "/thing.patch: " +
                               (clean ? std::to_string(r.changed.size()) + " changed" +
                                            (r.added.empty() ? std::string() : ", " + std::to_string(r.added.size()) + " types added")
                                      : "not applied (" + std::to_string(problems.size() + r.problems.size()) + " problems)"));
                for (const auto& s : problems) log->push_back(p.id + "/" + s);
                for (const auto& s : r.problems) log->push_back(p.id + "/thing.patch: " + s);
                for (const auto& s : r.warnings) log->push_back(p.id + "/thing.patch: warning: " + s);
            }
        } catch (const std::exception& ex) {
            if (log) {
                log->push_back(p.id + "/thing.patch: not applied: " + ex.what());
            }
        }
    }
    if (db.things.size() > first) {
        types.count = static_cast<std::uint32_t>(db.things.size() - first);
        types.hash = newThingTypesHash(db, first);
        if (log) {
            std::string ids;
            for (const auto& id : types.packs) {
                ids += (ids.empty() ? "" : ", ") + id;
            }
            log->push_back(std::format("content packs add {} object types ({}): only NoX Core games with the same packs can join a game this one "
                                       "hosts, and this one can join only hosts with the same packs or with none that add types",
                                       types.count, ids));
        }
    }
    if (added) {
        *added = std::move(types);
    }
    return any;
}

bool applyContentPacksToThings(things::ThingDb& db)
{
    std::vector<ContentPack> packs;
    {
        const std::lock_guard lock(g_mutex);
        if (g_packs.empty()) {
            return false;
        }
        packs = g_packs;
    }
    std::vector<std::string> log;
    NewThingTypes added;
    const bool applied = applyContentPacksToThings(db, packs, &log, &added);
    forward(log);
    things::setAddedTypeBases(added.bases);
    const std::lock_guard lock(g_mutex);
    g_newTypes = std::move(added);
    return applied;
}

NewThingTypes newThingTypes()
{
    const std::lock_guard lock(g_mutex);
    return g_newTypes;
}

bool applyContentPacksToStrings(std::vector<std::uint8_t>& csf, const std::vector<ContentPack>& packs, std::vector<std::string>* log)
{
    if (std::ranges::none_of(packs, [](const ContentPack& p) { return p.strings; })) {
        return false;
    }
    try {
        CsfDocument doc = CsfDocument::parse(csf);
        for (const ContentPack& p : packs) {
            if (!p.strings) {
                continue;
            }
            try {
                const auto bytes = readFileBytes(findNoCase(p.dir, "strings.patch"));
                std::vector<std::string> problems;
                const auto patch = parseStringsPatch(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &problems);
                CsfDocument trial = doc; // (applied whole or not at all)
                const StringsPatchResult r = applyStringsPatch(trial, patch);
                const bool clean = problems.empty() && r.problems.empty();
                if (clean) {
                    doc = std::move(trial);
                }
                if (log) {
                    log->push_back(p.id + "/strings.patch: " +
                                   (clean ? std::to_string(r.changed.size()) + " changed, " + std::to_string(r.added.size()) + " added"
                                          : "not applied (" + std::to_string(problems.size() + r.problems.size()) + " problems)"));
                    for (const auto& s : problems) log->push_back(p.id + "/" + s);
                    for (const auto& s : r.problems) log->push_back(p.id + "/strings.patch: " + s);
                }
            } catch (const std::exception& ex) {
                if (log) {
                    log->push_back(p.id + "/strings.patch: not applied: " + ex.what());
                }
            }
        }
        csf = doc.write();
    } catch (const std::exception& ex) {
        if (log) {
            log->push_back(std::string("nox.csf: the content packs' strings not applied: ") + ex.what());
        }
    }
    return true;
}

bool applyContentPacksToStrings(std::vector<std::uint8_t>& csf)
{
    std::vector<ContentPack> packs;
    {
        const std::lock_guard lock(g_mutex);
        if (g_packs.empty()) {
            return false;
        }
        packs = g_packs;
    }
    std::vector<std::string> log;
    const bool applied = applyContentPacksToStrings(csf, packs, &log);
    forward(log);

    return applied;
}

std::vector<fs::path> contentPackDirs()
{
    const std::lock_guard lock(g_mutex);
    std::vector<fs::path> out;
    for (const ContentPack& p : g_packs) {
        out.push_back(p.dir);
    }
    return out;
}

std::vector<fs::path> contentPackAudioDirs()
{
    const std::lock_guard lock(g_mutex);
    std::vector<fs::path> out;
    for (const ContentPack& p : g_packs) {
        if (p.audio) {
            out.push_back(findNoCase(p.dir, "audio"));
        }
    }
    return out;
}

void writeContentPackLog(const std::string& line)
{
    const std::lock_guard lock(g_mutex);
    if (g_sink) {
        g_sink(line);
    } else {
        g_log.push_back(line);
    }
}

std::vector<std::string> takeContentPackLog()
{
    const std::lock_guard lock(g_mutex);
    return std::exchange(g_log, {});
}

} // namespace nox::content
