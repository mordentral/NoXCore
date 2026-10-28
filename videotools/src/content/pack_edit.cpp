// The content pack being edited (pack_edit.hpp).
#include "content/pack_edit.hpp"

#include "noxcore/audio/pcm.hpp"
#include "noxcore/content/audio_writer.hpp"
#include "noxcore/crypto/noxcrypt.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <exception>
#include <format>
#include <map>
#include <system_error>

namespace fs = std::filesystem;

namespace videotools::content {

namespace {

constexpr const char* kSections[] = {"WEAPON_DEFINITIONS", "ARMOR_DEFINITIONS", "EFFECTIVENESS", "MATERIAL", "ENCHANTMENT"};

std::string prefixed(GameKind kind, std::string_view key)
{
    return kind == GameKind::Both ? std::string(key) : std::string(nox::content::gameKindPrefix(kind)) + " " + std::string(key);
}

void writeText(const fs::path& file, const std::string& text)
{
    nox::writeFileBytes(file, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

} // namespace

std::string FieldEdit::label() const
{
    std::string s = section.empty() ? std::string() : section + "/";
    if (!record.empty()) {
        s += record + ".";
    }
    return s + prefixed(kind, key);
}

void PackEditor::openBase(const fs::path& gameDir)
{
    errors_.clear();
    for (const TableFormat f : nox::content::kTableFormats) {
        base_[index(f)].reset();
        stale_[index(f)] = true;
        try {
            const auto bytes = nox::readFileBytes(nox::findNoCase(gameDir, nox::content::tableFileName(f)));
            base_[index(f)] = TextTable::load(f, bytes);
        } catch (const std::exception& ex) {
            errors_.push_back(std::format("{}: {}", nox::content::tableFileName(f), ex.what()));
        }
    }
    gameDir_ = gameDir;
    // nox.csf as stored.
    baseStrings_.reset();
    stringsStale_ = true;
    try {
        baseStrings_ = nox::content::CsfDocument::parse(nox::readFileBytes(nox::findNoCase(gameDir, "nox.csf")));
    } catch (const std::exception& ex) {
        errors_.push_back(std::format("nox.csf: {}", ex.what()));
    }
    // thing.bin: its types and the layout to write them back in.
    baseThings_.reset();
    thingsStale_ = true;
    try {
        auto plain = nox::readFileBytes(nox::findNoCase(gameDir, "thing.bin"));
        nox::crypto::cipherForKey(nox::crypto::FileKey::ThingBin).decrypt(plain);
        baseThings_ = nox::things::parseThingDb(plain);
        thingLayout_ = nox::content::scanThingLayout(plain, *baseThings_);
    } catch (const std::exception& ex) {
        baseThings_.reset();
        errors_.push_back(std::format("thing.bin: {}", ex.what()));
    }
}

std::vector<std::string> PackEditor::openPack(const fs::path& contentDir, std::string id)
{
    std::vector<std::string> problems;
    contentDir_ = contentDir;
    packId_ = std::move(id);
    edits_.clear();
    for (auto& e : extra_) {
        e.clear();
    }
    for (const TableFormat f : nox::content::kTableFormats) {
        stale_[index(f)] = true;
        const fs::path file = nox::findNoCase(contentDir, nox::content::patchFileName(f));
        std::error_code ec;
        if (!fs::exists(file, ec)) {
            continue;
        }
        try {
            const auto bytes = nox::readFileBytes(file);
            const TextTable patch = TextTable::parsePatch(f, std::string(bytes.begin(), bytes.end()));
            const TextTable* b = base(f);
            for (const auto& r : patch.records()) {
                if (f == TableFormat::Monster && b && !b->find(r.name) && baseThings_ && !baseThings_->findThing(r.name)) {
                    // (GAME.EXE drops every monster over a name that isn't a type: not kept)
                    problems.push_back("monster.patch: " + r.name + " isn't a thing.bin type: refused (the original game would lose every monster over it)");
                    continue;
                }
                if (b && !b->find(r.name, r.section) && f != TableFormat::GameData) {
                    // A record the game's file doesn't have: kept as the patch writes it.
                    extra_[index(f)] += std::string_view(patch.text()).substr(r.whole.begin, r.whole.size());
                    extra_[index(f)] += "\n";
                    continue;
                }
                for (const auto& field : r.fields) {
                    FieldEdit e;
                    e.format = f;
                    e.section = r.section;
                    e.record = r.name;
                    e.kind = field.kind;
                    if (field.key.starts_with('-')) {
                        e.key = field.key.substr(1);
                    } else {
                        e.key = field.key;
                        e.value = field.value;
                    }
                    std::erase_if(edits_, [&](const FieldEdit& o) { return o.sameTarget(e); });
                    edits_.push_back(std::move(e));
                }
            }
        } catch (const std::exception& ex) {
            problems.push_back(std::format("{}: {}", nox::content::patchFileName(f), ex.what()));
        }
    }
    stringEdits_.clear();
    stringsStale_ = true;
    if (const fs::path file = nox::findNoCase(contentDir, "strings.patch"); fs::exists(file)) {
        try {
            const auto bytes = nox::readFileBytes(file);
            std::vector<std::string> parseProblems;
            const auto lines = nox::content::parseStringsPatch(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &parseProblems);
            for (auto& p : parseProblems) {
                problems.push_back(std::move(p));
            }
            for (const auto& l : lines) {
                auto it = std::ranges::find_if(stringEdits_, [&](const StringEdit& e) { return nox::util::equalsNoCase(e.label, l.label); });
                if (it == stringEdits_.end()) {
                    StringEdit e{l.label, {}, std::nullopt};
                    if (const auto* b = baseStrings_ ? baseStrings_->find(l.label) : nullptr; b && l.op != nox::content::StringsPatchLine::Set) {
                        for (const auto& v : b->variants) e.variants.push_back(v.text()); // (+= and .speech start from the game's own)
                    }
                    stringEdits_.push_back(std::move(e));
                    it = stringEdits_.end() - 1;
                }
                switch (l.op) {
                case nox::content::StringsPatchLine::Set:
                    if (std::ranges::count_if(lines, [&](const auto& o) { return &o < &l && o.op == l.op && nox::util::equalsNoCase(o.label, l.label); }) == 0) {
                        it->variants.assign(1, l.text);
                    } else {
                        it->variants.push_back(l.text);
                    }
                    break;
                case nox::content::StringsPatchLine::Add: it->variants.push_back(l.text); break;
                case nox::content::StringsPatchLine::Speech: it->speech = nox::strings::toUtf8(l.text); break;
                case nox::content::StringsPatchLine::Remove: it->variants.clear(); it->speech.reset(); break;
                }
            }
        } catch (const std::exception& ex) {
            problems.push_back(std::format("strings.patch: {}", ex.what()));
        }
    }
    soundEdits_.clear();
    for (const auto folder : {SoundEdit::Audio, SoundEdit::Dialog}) {
        const fs::path dir = nox::findNoCase(contentDir, folder == SoundEdit::Audio ? "audio" : "dialog");
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && nox::util::lowered(it->path().extension().string()) == ".wav") {
                soundEdits_.push_back({folder, it->path().stem().string(), it->path(), true});
            }
        }
    }
    thingEdits_.clear();
    newThings_.clear();
    thingsStale_ = true;
    if (const fs::path file = nox::findNoCase(contentDir, "thing.patch"); fs::exists(file)) {
        try {
            const auto bytes = nox::readFileBytes(file);
            std::vector<std::string> parseProblems;
            const auto patch = nox::content::parseThingPatch(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &parseProblems);
            for (auto& p : parseProblems) {
                problems.push_back(std::move(p));
            }
            // The new types first, as the loader takes them (each checked as it is: one it would
            // refuse is reported and left out, so its record goes when the pack is saved again).
            std::vector<std::string> refused;
            for (const auto& r : patch.records) {
                if (!r.isNew()) {
                    continue;
                }
                std::string why = nox::content::checkNewThingName(r.name);
                const auto taken = [&](std::string_view n) {
                    return (baseThings_ && std::ranges::any_of(baseThings_->things, [&](const auto& t) { return nox::util::equalsNoCase(t.name, n); })) ||
                           std::ranges::any_of(newThings_, [&](const NewThing& o) { return nox::util::equalsNoCase(o.name, n); });
                };
                if (why.empty() && taken(r.name)) {
                    why = "there's a type of that name already";
                } else if (why.empty() && !(baseThings_ && baseThings_->findThing(r.like)) && !isNewThing(r.like)) {
                    why = "there's no type " + r.like + " to make it like";
                }
                if (!why.empty()) {
                    problems.push_back("thing.patch: NEW " + r.name + ": " + why + " (left out)");
                    refused.push_back(r.name);
                } else {
                    newThings_.push_back({r.name, r.like});
                }
            }
            for (const auto& r : patch.records) {
                if (r.isNew() && std::ranges::find(refused, r.name) != refused.end()) {
                    continue;
                }
                if (!r.isNew() && baseThings_ && !baseThings_->findThing(r.name) && !isNewThing(r.name)) {
                    problems.push_back("thing.patch: thing.bin has no type " + r.name + " (a new one is added with NEW " + r.name + " LIKE <type>)");
                }
                for (const auto& l : r.lines) {
                    ThingEdit e{r.name, l.key, l.remove ? std::nullopt : std::optional<std::string>(l.hasEquals ? l.value : std::string())};
                    std::erase_if(thingEdits_, [&](const ThingEdit& o) { return o.sameTarget(e); });
                    thingEdits_.push_back(std::move(e));
                }
            }
        } catch (const std::exception& ex) {
            problems.push_back(std::format("thing.patch: {}", ex.what()));
        }
    }
    dirty_ = false;
    return problems;
}

const nox::content::CsfDocument* PackEditor::workingStrings() const
{
    if (!baseStrings_) {
        return nullptr;
    }
    if (stringsStale_) {
        stringsStale_ = false;
        workStrings_ = *baseStrings_;
        const std::string text = stringsPatchText();
        nox::content::applyStringsPatch(*workStrings_, nox::content::parseStringsPatch(text));
    }
    return &*workStrings_;
}

const StringEdit* PackEditor::stringEditOf(std::string_view label) const
{
    for (const auto& e : stringEdits_) {
        if (nox::util::equalsNoCase(e.label, label)) {
            return &e;
        }
    }
    return nullptr;
}

std::string PackEditor::setStrings(std::string_view label, std::vector<std::u16string> variants, std::optional<std::string> speech)
{
    // As the loader checks strings.patch (the game's limits, its printf codes).
    const auto* b = baseStrings_ ? baseStrings_->find(label) : nullptr;
    if (std::string why = nox::content::checkStringsLabel(label); !why.empty()) {
        return why;
    }
    if (variants.size() > nox::content::kMaxStringVariants) {
        return std::format("A label has at most {} texts.", nox::content::kMaxStringVariants);
    }
    const bool inverted = b && !b->variants.empty() && b->variants.front().inverted();
    const auto want = b && !b->variants.empty() ? nox::content::printfConversions(b->variants.front().text()) : std::vector<std::string>{};
    for (const auto& v : variants) {
        if (std::string why = nox::content::checkStringsText(v, inverted); !why.empty()) {
            return why;
        }
        const auto now = nox::content::printfConversions(v);
        if (std::ranges::any_of(now, [](const std::string& c) { return c.find('n') != std::string::npos; })) {
            return "%n isn't allowed in a text.";
        }
        if (now != want) {
            std::string codes;
            for (const auto& c : want) codes += (codes.empty() ? "" : " ") + c;
            return want.empty() ? "This text can't hold printf codes (a % other than %%): the game would read past its arguments."
                                : "The text must keep the game's own printf codes, in order: " + codes + " (the game fills them in).";
        }
    }
    if (speech) {
        if (std::string why = nox::content::checkSpeechName(*speech); !why.empty()) {
            return why;
        }
    }
    std::erase_if(stringEdits_, [&](const StringEdit& e) { return nox::util::equalsNoCase(e.label, label); });
    bool same = false;
    if (b && b->variants.size() == variants.size()) {
        same = true;
        for (std::size_t i = 0; i < variants.size(); ++i) {
            same = same && b->variants[i].text() == variants[i];
        }
        same = same && (!speech || (!b->variants.empty() && b->variants.front().speech == *speech));
    }
    if (!same && !variants.empty()) {
        stringEdits_.push_back({std::string(label), std::move(variants), std::move(speech)});
    }
    stringsStale_ = true;
    dirty_ = true;
    return {};
}

void PackEditor::removeString(std::string_view label)
{
    std::erase_if(stringEdits_, [&](const StringEdit& e) { return nox::util::equalsNoCase(e.label, label); });
    stringEdits_.push_back({std::string(label), {}, std::nullopt});
    stringsStale_ = true;
    dirty_ = true;
}

void PackEditor::revertString(std::size_t edit)
{
    if (edit < stringEdits_.size()) {
        stringEdits_.erase(stringEdits_.begin() + static_cast<std::ptrdiff_t>(edit));
        stringsStale_ = true;
        dirty_ = true;
    }
}

std::string PackEditor::stringsPatchText() const
{
    std::vector<nox::content::StringsPatchLine> lines;
    for (const auto& e : stringEdits_) {
        if (e.variants.empty()) {
            lines.push_back({e.label, nox::content::StringsPatchLine::Remove, {}});
            continue;
        }
        lines.push_back({e.label, nox::content::StringsPatchLine::Set, e.variants.front()});
        for (std::size_t i = 1; i < e.variants.size(); ++i) {
            lines.push_back({e.label, nox::content::StringsPatchLine::Add, e.variants[i]});
        }
        if (e.speech) {
            lines.push_back({e.label, nox::content::StringsPatchLine::Speech, nox::strings::fromUtf8(*e.speech)});
        }
    }
    return nox::content::writeStringsPatch(lines, std::format("# strings.patch: changes to nox.csf's texts (a NoX Core content pack{})\n",
                                                                packId_.empty() ? std::string() : ", " + packId_));
}

void PackEditor::setSound(SoundEdit::Folder folder, std::string_view name, const fs::path& source)
{
    std::erase_if(soundEdits_, [&](const SoundEdit& e) { return e.folder == folder && nox::util::equalsNoCase(e.name, name); });
    soundEdits_.push_back({folder, std::string(name), source, false});
    dirty_ = true;
}

void PackEditor::revertSound(std::size_t edit)
{
    if (edit < soundEdits_.size()) {
        // (a sound already in the pack's folder: removed from it when the pack is saved)
        if (soundEdits_[edit].saved) {
            soundEdits_[edit].source.clear();
            soundEdits_[edit].saved = false;
        } else {
            soundEdits_.erase(soundEdits_.begin() + static_cast<std::ptrdiff_t>(edit));
        }
        dirty_ = true;
    }
}

const SoundEdit* PackEditor::soundEditOf(SoundEdit::Folder folder, std::string_view name) const
{
    for (const auto& e : soundEdits_) {
        if (e.folder == folder && nox::util::equalsNoCase(e.name, name) && !e.source.empty()) {
            return &e;
        }
    }
    return nullptr;
}

const nox::things::ThingDb* PackEditor::workingThings() const
{
    if (!baseThings_) {
        return nullptr;
    }
    if (thingsStale_) {
        thingsStale_ = false;
        workThings_ = *baseThings_;
        thingProblems_ = nox::content::applyThingPatch(*workThings_, thingPatch()).problems; // (what the loader would refuse: Problems)
    }
    return &*workThings_;
}

const ThingEdit* PackEditor::thingEditOf(std::string_view type, std::string_view key) const
{
    for (const ThingEdit& e : thingEdits_) {
        if (e.type == type && nox::util::equalsNoCase(e.key, key)) {
            return &e;
        }
    }
    return nullptr;
}

std::string PackEditor::checkThingEdit(std::string_view type, std::string& key, std::string* value) const
{
    const auto trimmed = nox::util::trim(key);
    if (trimmed.empty() || std::ranges::any_of(trimmed, [](char c) { return c <= ' ' || c >= 0x7F || c == '='; })) {
        return "A key is one word of plain letters, without blanks or '='.";
    }
    key = std::string(nox::content::thingKeySpelling(trimmed).value_or(trimmed)); // (as GAME.EXE's readers spell it)
    if (key == "DRAW") {
        return "DRAW (the animation data) isn't changed by a pack.";
    }
    // A type the pack adds chooses its own CLASS, XFER, FLAGS, COLLIDE and MENUICON (its NEW record's).
    const bool fresh = isNewThing(type);
    if (nox::content::lockedThingKey(key) && !fresh) {
        return key + " of a type thing.bin has never changes: every player's game and the maps read its objects by it. Only a new type (New type...) chooses its own.";
    }
    const nox::things::ThingDb* db = fresh ? workingThings() : (baseThings_ ? &*baseThings_ : nullptr);
    const nox::things::ThingDef* t = db ? db->findThing(type) : nullptr;
    if (!value && t && !nox::content::thingValue(*t, key)) {
        return std::string(type) + " has no " + key + " to remove.";
    }
    if (key == "MENUICON" && t && t->menuIcon.has_value() != (value != nullptr) && !fresh) {
        return value ? "Only types with a MENUICON are kept in saves: one can't be added." : "Only types with a MENUICON are kept in saves: it can't be taken away.";
    }
    if (value) {
        if (std::string error = nox::content::checkThingValue(key, *value, t ? nox::content::thingValue(*t, key).value_or("") : std::string()); !error.empty()) {
            error[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(error[0])));
            return key + ": " + error + ".";
        }
    }
    return {};
}

std::string PackEditor::setThing(std::string_view type, std::string_view key, std::string_view value)
{
    std::string k(key);
    std::string v(nox::util::trim(value));
    if (std::string error = checkThingEdit(type, k, &v); !error.empty()) {
        return error;
    }
    ThingEdit e{std::string(type), k, v};
    std::erase_if(thingEdits_, [&](const ThingEdit& o) { return o.type == e.type && nox::util::equalsNoCase(o.key, e.key); });
    bool same = false;
    if (baseThings_ && !isNewThing(type)) { // (a new type keeps every line it's given)
        if (const auto* t = baseThings_->findThing(type)) {
            same = nox::content::thingValue(*t, k) == e.value;
        }
    }
    if (!same) {
        thingEdits_.push_back(std::move(e));
    }
    thingsStale_ = true;
    dirty_ = true;
    return {};
}

std::string PackEditor::removeThing(std::string_view type, std::string_view key)
{
    std::string k(key);
    if (std::string error = checkThingEdit(type, k, nullptr); !error.empty()) {
        return error;
    }
    ThingEdit e{std::string(type), k, std::nullopt};
    std::erase_if(thingEdits_, [&](const ThingEdit& o) { return o.type == e.type && nox::util::equalsNoCase(o.key, e.key); });
    thingEdits_.push_back(std::move(e));
    thingsStale_ = true;
    dirty_ = true;
    return {};
}

void PackEditor::revertThing(std::size_t edit)
{
    if (edit < thingEdits_.size()) {
        thingEdits_.erase(thingEdits_.begin() + static_cast<std::ptrdiff_t>(edit));
        thingsStale_ = true;
        dirty_ = true;
    }
}

nox::content::ThingPatch PackEditor::thingPatch() const
{
    nox::content::ThingPatch patch;
    for (const NewThing& n : newThings_) {
        nox::content::ThingPatchRecord r;
        r.name = n.name;
        r.like = n.like;
        patch.records.push_back(std::move(r));
    }
    for (const ThingEdit& e : thingEdits_) {
        auto it = std::ranges::find_if(patch.records, [&](const auto& r) { return r.name == e.type; });
        if (it == patch.records.end()) {
            nox::content::ThingPatchRecord r;
            r.name = e.type;
            patch.records.push_back(std::move(r));
            it = patch.records.end() - 1;
        }
        nox::content::ThingPatchLine l;
        l.key = e.key;
        l.value = e.value.value_or("");
        l.hasEquals = e.value.has_value();
        l.remove = !e.value.has_value();
        it->lines.push_back(std::move(l));
    }
    return patch;
}

std::string PackEditor::thingPatchText() const
{
    return nox::content::writeThingPatch(thingPatch(), std::format("# thing.patch: changes to thing.bin's object types{} (a NoX Core content pack{})\n",
                                                                   newThings_.empty() ? "" : ", and new ones",
                                                                   packId_.empty() ? std::string() : ", " + packId_));
}

Reach PackEditor::reachOf(const ThingEdit& e) const
{
    if (isNewThing(e.type)) {
        return Reach::Structural; // (a new type's every line: only games with the same types have it)
    }
    const FieldSpec* spec = thingField(e.key);
    return spec ? content::reachOf(*spec) : Reach::ServerOnly;
}

bool PackEditor::isNewThing(std::string_view name) const
{
    return std::ranges::any_of(newThings_, [&](const NewThing& n) { return n.name == name; });
}

std::string PackEditor::addThing(std::string_view name, std::string_view like)
{
    const std::string n(nox::util::trim(name));
    const std::string base(nox::util::trim(like));
    if (std::string why = nox::content::checkNewThingName(n); !why.empty()) {
        why[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(why[0])));
        return why + ".";
    }
    const nox::things::ThingDb* work = workingThings();
    if (!work) {
        return "thing.bin didn't load.";
    }
    if (std::ranges::any_of(work->things, [&](const auto& t) { return nox::util::equalsNoCase(t.name, n); }) ||
        std::ranges::any_of(newThings_, [&](const NewThing& o) { return nox::util::equalsNoCase(o.name, n); })) {
        return "There's a type called " + n + " already (names are compared without case).";
    }
    if (!work->findThing(base)) {
        return "There's no type " + base + " to make it like.";
    }
    if (work->things.size() >= nox::content::kMaxThingTypes) {
        return std::format("thing.bin can't have more than {} types.", nox::content::kMaxThingTypes);
    }
    newThings_.push_back({n, base});
    thingsStale_ = true;
    dirty_ = true;
    return {};
}

std::string PackEditor::removeNewThing(std::string_view name)
{
    if (const auto other = std::ranges::find_if(newThings_, [&](const NewThing& n) { return n.like == name; }); other != newThings_.end()) {
        return other->name + " is made like it: take that one out first.";
    }
    if (std::erase_if(newThings_, [&](const NewThing& n) { return n.name == name; }) == 0) {
        return std::string(name) + " isn't a type the pack adds.";
    }
    std::erase_if(thingEdits_, [&](const ThingEdit& e) { return e.type == name; });
    thingsStale_ = true;
    dirty_ = true;
    return {};
}

void PackEditor::closePack()
{
    contentDir_.clear();
    packId_.clear();
    edits_.clear();
    thingEdits_.clear();
    newThings_.clear();
    thingsStale_ = true;
    stringEdits_.clear();
    soundEdits_.clear();
    stringsStale_ = true;
    for (auto& e : extra_) {
        e.clear();
    }
    stale_ = {true, true, true, true};
    dirty_ = false;
}

void PackEditor::rebuild(TableFormat f) const
{
    const std::size_t i = index(f);
    stale_[i] = false;
    working_[i].reset();
    if (!base_[i]) {
        return;
    }
    TextTable t = *base_[i];
    for (const FieldEdit& e : edits_) {
        if (e.format != f) {
            continue;
        }
        const auto r = t.find(e.record, e.section);
        if (!r) {
            continue;
        }
        if (e.value) {
            t.set(*r, e.key, e.kind, *e.value);
        } else {
            t.remove(*r, e.key, e.kind);
        }
    }
    if (!extra_[i].empty()) {
        t.appendRecord(extra_[i]);
    }
    working_[i] = std::move(t);
}

const TextTable* PackEditor::working(TableFormat f) const
{
    if (stale_[index(f)]) {
        rebuild(f);
    }
    return working_[index(f)] ? &*working_[index(f)] : nullptr;
}

const FieldEdit* PackEditor::editOf(TableFormat f, std::string_view section, std::string_view record, std::string_view key, GameKind kind) const
{
    for (const FieldEdit& e : edits_) {
        if (e.format == f && e.section == section && e.record == record && nox::util::equalsNoCase(e.key, key) && e.kind == kind) {
            return &e;
        }
    }
    return nullptr;
}

bool PackEditor::set(TableFormat f, std::string_view section, std::string_view record, std::string_view key, GameKind kind, std::string_view value)
{
    const auto taken = TextTable::valueFor(f, value);
    if (!taken) {
        return false;
    }
    FieldEdit e;
    e.format = f;
    e.section = std::string(section);
    e.record = std::string(record);
    e.key = std::string(key);
    e.kind = kind;
    e.value = *taken;
    std::erase_if(edits_, [&](const FieldEdit& o) { return o.sameTarget(e); });
    // Back to the game's own value: no edit.
    bool same = false;
    if (const TextTable* b = base(f)) {
        if (const auto r = b->find(e.record, e.section)) {
            same = b->records()[*r].effective(e.key, kind) == e.value;
        }
    }
    if (!same) {
        edits_.push_back(std::move(e));
    }
    stale_[index(f)] = true;
    dirty_ = true;
    return true;
}

void PackEditor::remove(TableFormat f, std::string_view section, std::string_view record, std::string_view key, GameKind kind)
{
    FieldEdit e;
    e.format = f;
    e.section = std::string(section);
    e.record = std::string(record);
    e.key = std::string(key);
    e.kind = kind;
    std::erase_if(edits_, [&](const FieldEdit& o) { return o.sameTarget(e); });
    edits_.push_back(std::move(e));
    stale_[index(f)] = true;
    dirty_ = true;
}

void PackEditor::revert(std::size_t edit)
{
    if (edit >= edits_.size()) {
        return;
    }
    stale_[index(edits_[edit].format)] = true;
    edits_.erase(edits_.begin() + static_cast<std::ptrdiff_t>(edit));
    dirty_ = true;
}

void PackEditor::revertAll()
{
    edits_.clear();
    thingEdits_.clear();
    newThings_.clear();
    thingsStale_ = true;
    for (auto& e : extra_) {
        e.clear();
    }
    stale_ = {true, true, true, true};
    dirty_ = true;
}

std::string PackEditor::patchText(TableFormat f) const
{
    std::vector<const FieldEdit*> mine;
    for (const FieldEdit& e : edits_) {
        if (e.format == f) {
            mine.push_back(&e);
        }
    }
    if (mine.empty() && extra_[index(f)].empty()) {
        return {};
    }
    const char* file = nox::content::tableFileName(f);
    std::string out = std::format("# {}: changes to {} (a NoX Core content pack{})\n", nox::content::patchFileName(f), file,
                                  packId_.empty() ? std::string() : ", " + packId_);
    const auto line = [&](const FieldEdit& e, std::string_view indent) {
        std::string l(indent);
        const std::string key = (e.value ? "" : "-") + e.key;
        l += prefixed(e.kind, key);
        switch (f) {
        case TableFormat::GameData:
        case TableFormat::Modifier:
            l += " = " + e.value.value_or("") + ";";
            break;
        default:
            l += " " + e.value.value_or("-"); // (a removal still takes a word where the value goes)
            break;
        }
        return l + "\n";
    };
    if (f == TableFormat::GameData) {
        for (const FieldEdit* e : mine) {
            out += line(*e, "");
        }
    } else {
        // By record, in the order the records were first changed; modifier.bin's within their section.
        std::vector<std::pair<std::string, std::string>> order; // (section, record)
        for (const FieldEdit* e : mine) {
            const std::pair key{e->section, e->record};
            if (std::ranges::find(order, key) == order.end()) {
                order.push_back(key);
            }
        }
        const auto records = [&](std::string_view section, std::string_view indent) {
            std::string block;
            for (const auto& [sec, rec] : order) {
                if (sec != section) {
                    continue;
                }
                block += std::string(indent) + rec + "\n";
                for (const FieldEdit* e : mine) {
                    if (e->section == sec && e->record == rec) {
                        block += line(*e, std::string(indent) + "    ");
                    }
                }
                block += std::string(indent) + "END\n";
            }
            return block;
        };
        if (f == TableFormat::Modifier) {
            for (const char* section : kSections) {
                const std::string block = records(section, "    ");
                if (!block.empty()) {
                    out += std::string(section) + "\n" + block + "END\n";
                }
            }
        } else {
            out += records("", "");
        }
    }
    out += extra_[index(f)];
    return out;
}

void PackEditor::save()
{
    if (contentDir_.empty()) {
        throw std::runtime_error("no content pack is open");
    }
    fs::create_directories(contentDir_);
    for (const TableFormat f : nox::content::kTableFormats) {
        const fs::path file = contentDir_ / nox::content::patchFileName(f);
        const std::string text = patchText(f);
        std::error_code ec;
        if (text.empty()) {
            fs::remove(nox::findNoCase(contentDir_, nox::content::patchFileName(f)), ec);
        } else {
            writeText(file, text);
        }
    }
    {
        const std::string text = stringsPatchText();
        std::error_code ec;
        if (text.empty()) {
            fs::remove(nox::findNoCase(contentDir_, "strings.patch"), ec);
        } else {
            writeText(contentDir_ / "strings.patch", text);
        }
    }
    // The sounds: each new one copied in, each one taken out removed from the folder.
    for (auto it = soundEdits_.begin(); it != soundEdits_.end();) {
        const fs::path dir = contentDir_ / (it->folder == SoundEdit::Audio ? "audio" : "dialog");
        const fs::path file = dir / (it->name + ".wav");
        std::error_code ec;
        if (it->source.empty()) {
            fs::remove(file, ec);
            it = soundEdits_.erase(it);
            continue;
        }
        if (!it->saved) {
            fs::create_directories(dir);
            if (!fs::equivalent(it->source, file, ec)) {
                fs::copy_file(it->source, file, fs::copy_options::overwrite_existing);
            }
            it->source = file;
            it->saved = true;
        }
        ++it;
    }
    {
        const std::string text = thingPatchText();
        std::error_code ec;
        if (text.empty()) {
            fs::remove(nox::findNoCase(contentDir_, "thing.patch"), ec);
        } else {
            writeText(contentDir_ / "thing.patch", text);
        }
    }
    if (std::error_code ec; !fs::exists(contentDir_ / "pack.txt", ec)) {
        writeText(contentDir_ / "pack.txt", "title = " + packId_ + "\n");
    }
    dirty_ = false;
}

std::vector<fs::path> PackEditor::exportFull(const fs::path& dir) const
{
    std::vector<fs::path> written;
    fs::create_directories(dir);
    for (const TableFormat f : nox::content::kTableFormats) {
        const bool changed = std::ranges::any_of(edits_, [&](const FieldEdit& e) { return e.format == f; }) || !extra_[index(f)].empty();
        const TextTable* t = changed ? working(f) : nullptr;
        if (!t) {
            continue;
        }
        const fs::path file = dir / nox::content::tableFileName(f);
        nox::writeFileBytes(file, t->encrypt());
        written.push_back(file);
    }
    if (!stringEdits_.empty() && workingStrings()) {
        const fs::path file = dir / "nox.csf";
        nox::writeFileBytes(file, workingStrings()->write());
        written.push_back(file);
    }
    // Audio.bag and Audio.idx with the pack's samples; its spoken lines as Dialog\ files.
    std::vector<nox::content::AudioReplacement> replaced;
    for (const auto& s : soundEdits_) {
        if (s.source.empty()) {
            continue;
        }
        if (s.folder == SoundEdit::Audio) {
            replaced.push_back({s.name, nox::audio::decodeFile(nox::readFileBytes(s.source))});
        } else {
            fs::create_directories(dir / "Dialog");
            const fs::path file = dir / "Dialog" / (s.name + ".wav");
            fs::copy_file(s.source, file, fs::copy_options::overwrite_existing);
            written.push_back(file);
        }
    }
    if (!replaced.empty()) {
        const auto index = nox::content::AudioIndex::parse(nox::readFileBytes(nox::findNoCase(gameDir_, "Audio.idx")));
        nox::content::writeAudioBag(nox::findNoCase(gameDir_, "Audio.bag"), index, replaced, dir / "Audio.bag", dir / "Audio.idx");
        written.push_back(dir / "Audio.bag");
        written.push_back(dir / "Audio.idx");
    }
    if ((!thingEdits_.empty() || !newThings_.empty()) && workingThings() && thingLayout_.valid) {
        // (the new types after the file's: its layout's, each in a THNG section of its own)
        const fs::path file = dir / "thing.bin";
        nox::writeFileBytes(file, nox::content::encryptThingDb(*workingThings(), thingLayout_));
        written.push_back(file);
    }
    return written;
}

Reach PackEditor::reachOf(const FieldEdit& e)
{
    const FieldSpec* spec = tableField(e.format, e.key, e.section);
    return spec ? content::reachOf(spec->readBy) : Reach::ServerOnly;
}

std::optional<Reach> PackEditor::reach() const
{
    std::optional<Reach> r;
    for (const FieldEdit& e : edits_) {
        r = r ? stronger(*r, reachOf(e)) : reachOf(e);
    }
    for (const ThingEdit& e : thingEdits_) {
        r = r ? stronger(*r, reachOf(e)) : reachOf(e);
    }
    if (!newThings_.empty()) {
        r = Reach::Structural; // (the strongest there is)
    }
    // Samples and spoken lines are played by the players' own games only. Texts too, but those the
    // host sends as text (scripts' Print / PrintToAll / Chat, the server's messages) are its own,
    // shown to everyone: the host's pack decides them.
    if (std::ranges::any_of(soundEdits_, [](const SoundEdit& s) { return !s.source.empty(); })) {
        r = r ? stronger(*r, Reach::ClientVisible) : Reach::ClientVisible;
    }
    if (!stringEdits_.empty()) {
        r = r ? stronger(*r, Reach::ServerShown) : Reach::ServerShown;
    }
    return r;
}

} // namespace videotools::content
