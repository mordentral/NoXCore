// The Server Settings window's Advanced... (servopts.wnd's 0x27A8: loadAdvancedWnd,
// its proc windowAdvancedServProc): advanced.wnd, its own window over the others
// taking the input, with a page at a time: the map's rule sets (rulelist.wnd),
// the spells (spelllst.wnd, guiSpelllistLoad), the weapons and the
// armour (objlst.wnd, guiObjlistLoad). The pages work on their own copy of the
// settings' rules (at opening); Done (0x27A4) puts it
// back; Go writes them into the map's user.rul.
#include "server_options.hpp"

#include "noxcore/server/rules.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <cctype>

namespace noxsdl {

namespace {

// advanced.wnd
constexpr std::int32_t kAdvRules = 10167, kAdvSpells = 10164, kAdvWeapons = 10165,
                       kAdvArmour = 10166, kAdvDone = 10148;
// rulelist.wnd
constexpr std::int32_t kRulePage = 10169, kRuleList = 10170, kRuleEntry = 10171, kRuleSaveAs = 10172,
                       kRuleSave = 10173, kRuleLoad = 10174, kRuleDelete = 10175, kRuleUp = 10177,
                       kRuleDown = 10178, kRuleSlider = 10179;
// spelllst.wnd
constexpr std::int32_t kSpellPage = 1100, kSpellChecks = 1120, kSpellList = 1110, kSchoolList = 1112,
                       kSpellUp = 1113, kSpellDown = 1114, kSpellAll = 1115, kSpellNone = 1116;
// objlst.wnd
constexpr std::int32_t kObjectPage = 1500, kObjectChecks = 1520, kObjectList = 1510, kObjectUp = 1513,
                       kObjectDown = 1514, kObjectAll = 1515, kObjectNone = 1516;
constexpr int kChecks = 14; // (0x460-0x46D, 0x5F0-0x5FD)
// dlg.wnd
constexpr std::int32_t kDlgOk = 4001, kDlgCancel = 4002, kDlgText = 4004, kDlgTitle = 4005, kDlgNo = 4006, kDlgYes = 4007;
constexpr int kWall = 0x84;  // (the Wall spell, refused in flagball)

// A list's font height: large 16, small 10.
int fontHeight(const gui::Window& w)
{
    return w.largeFont ? 16 : 10;
}

/// How many lines a list shows (under its title; gui.cpp's line step).
int shownLines(const gui::Window& list)
{
    const int fh = fontHeight(list);
    const auto* d = std::get_if<nox::gui::ListBoxData>(&list.data);
    const int step = std::max(fh, d ? static_cast<int>(d->lineHeight) : 0) + 1;
    return std::max(1, (list.rect.h - (list.text.empty() ? 0 : fh + 1)) / step);
}

void scrollList(gui::Window* list, int to)
{
    if (list) {
        const int most = std::max(0, static_cast<int>(list->lines.size()) - shownLines(*list));
        list->scroll = std::clamp(to, 0, most);
    }
}

// Equal ignoring case (rule set names).
using nox::util::equalsNoCase;

} // namespace

void ServerOptions::readPendingRules()
{
    // client_guiserv_updateMapList / 0x2782's: the chosen map's user.rul (else its own)
    // for the type into the settings (flags 7).
    rules_ = callbacks_.rulesFor && !settings_.map.empty() ? callbacks_.rulesFor(settings_.map, settings_.mode, std::nullopt)
                                                          : RuleSet{};
    work_ = rules_; // (the pages' copy)
}

void ServerOptions::openAdvanced()
{
    // loadAdvancedWnd: the host's opens on the rule sets (the Rules
    // radio chosen); the pages' copy of the rules.
    closeAdvanced();
    if (!callbacks_.rulesFor) {
        return;
    }
    nox::gui::WindowDef def;
    try {
        def = nox::gui::loadWindowFile(windowDir_ / "advanced.wnd");
        // (the exe makes a page's window as it's chosen, here all are loaded hidden)
        for (const char* file : {"rulelist.wnd", "spelllst.wnd", "objlst.wnd"}) {
            nox::gui::WindowDef page = nox::gui::loadWindowFile(windowDir_ / file);
            page.status |= nox::gui::status::Hidden;
            def.children.push_back(std::move(page));
        }
    } catch (const std::exception&) {
        return;
    }
    const gui::Proc proc = [this](gui::Window&, int message, gui::Window& from) { return onAdvanced(from, message); };
    advanced_ = gui_.load(def, proc);
    const std::function<void(gui::Window&)> hear = [&](gui::Window& w) {
        if (!w.children.empty()) {
            w.proc = proc;
        }
        for (auto& c : w.children) {
            hear(*c);
        }
    };
    hear(*advanced_);
    advanced_->status |= nox::gui::status::Above;
    // The rule list's slider thumb (with UISlider / UISliderLit).
    gui::useSliderThumb(advanced_->find(kRuleSlider));
    // The lists' lines a font height and a pixel apart, as the checkboxes beside them are
    // (wndListboxDrawNoImage steps by each line's own height; the file's 16 isn't it).
    for (const auto id : {kSpellList, kSchoolList, kObjectList}) {
        if (gui::Window* list = advanced_->find(id)) {
            if (auto* d = std::get_if<nox::gui::ListBoxData>(&list->data)) {
                d->lineHeight = 0;
            }
        }
    }
    work_ = rules_;
    gui::setChecked(advanced_->find(kAdvRules), true);
    gui_.setFocus(nullptr);
    gui_.setModal(advanced_);
    showAdvancedPage(0);
}

void ServerOptions::closeAdvanced()
{
    closeNotice();
    if (!advanced_) {
        return;
    }
    if (gui_.focus() && sdlWindow_) {
        SDL_StopTextInput(sdlWindow_);
    }
    gui_.setFocus(nullptr);
    gui_.setModal(nullptr);
    gui_.destroy(advanced_);
    advanced_ = nullptr;
}

void ServerOptions::showAdvancedPage(int page)
{
    // The old page's window gone, the new one's made.
    if (gui_.focus() && sdlWindow_) {
        SDL_StopTextInput(sdlWindow_);
    }
    gui_.setFocus(nullptr);
    advancedPage_ = page;
    if (gui::Window* w = advanced_->find(kRulePage)) {
        w->show(page == 0);
    }
    if (gui::Window* w = advanced_->find(kSpellPage)) {
        w->show(page == 1);
    }
    if (gui::Window* w = advanced_->find(kObjectPage)) {
        w->show(page >= 2);
    }
    switch (page) {
    case 0: fillRulePage(); break;
    case 1: fillSpellPage(); break;
    default: fillObjectPage(page == 3); break;
    }
}

bool ServerOptions::onAdvanced(gui::Window& from, int message)
{
    if (message == gui::SliderChanged && from.id == kRuleSlider) {
        gui::Window* list = advanced_->find(kRuleList);
        if (const auto* sl = std::get_if<nox::gui::SliderData>(&from.data); list && sl) {
            const int most = std::max(0, static_cast<int>(list->lines.size()) - shownLines(*list));
            list->scroll = gui::scrollFromSlider(*sl, from.sliderValue, most);
        }
        return true;
    }
    if (message != gui::Clicked) {
        return true; // (the window takes everything)
    }
    switch (from.id) {
    case kAdvDone:
        // 0x27A4: the pages' spells, weapons and armour into the settings,
        // the window gone.
        gui_.resources().sound(0x2FE);
        deferred_.push_back([this] {
            rules_.spells = work_.spells;
            rules_.weapons = work_.weapons;
            rules_.armor = work_.armor;
            SettingsEdit e; // (into the current slot)
            e.rules = rules_;
            editSlot(e);
            closeAdvanced();
        });
        return true;
    case kAdvRules:
    case kAdvSpells:
    case kAdvWeapons:
    case kAdvArmour: {
        // 0x27B7, 0x27B4, 0x27B5, 0x27B6.
        gui_.resources().sound(0x2FE);
        const int page = from.id == kAdvRules ? 0 : from.id == kAdvSpells ? 1 : from.id == kAdvWeapons ? 2 : 3;
        deferred_.push_back([this, page] { showAdvancedPage(page); });
        return true;
    }
    default:
        break;
    }
    if (advancedPage_ == 0) {
        return onRulePage(from, message);
    }
    if (advancedPage_ == 1) {
        return onSpellPage(from, message);
    }
    return onObjectPage(from, message);
}

// ---- the rule sets (rulelist.wnd)

void ServerOptions::fillRulePage()
{
    // maps\<map>\*.rul but the map's own and user.rul, without ".rul"; nothing
    // chosen, the name empty.
    gui::Window* list = advanced_->find(kRuleList);
    if (!list) {
        return;
    }
    list->lines.clear();
    if (callbacks_.ruleSets && !settings_.map.empty()) {
        for (const auto& name : callbacks_.ruleSets(settings_.map)) {
            list->lines.push_back({name, list->textColor});
        }
    }
    list->selectedLine = -1;
    list->scroll = 0;
    if (gui::Window* entry = advanced_->find(kRuleEntry)) {
        entry->entry.clear();
    }
    ruleButtons();
}

void ServerOptions::ruleButtons()
{
    // The page's draw: Save, Load and Delete with a rule set chosen; Save As with a
    // name typed, not the map's own nor "user" (0x4003's).
    if (!advanced_ || advancedPage_ != 0) {
        return;
    }
    gui::Window* list = advanced_->find(kRuleList);
    const bool chosen = list && list->selectedLine >= 0 && list->selectedLine < static_cast<int>(list->lines.size());
    for (const auto id : {kRuleSave, kRuleLoad, kRuleDelete}) {
        gui::setEnabled(advanced_->find(id), chosen);
    }
    gui::Window* entry = advanced_->find(kRuleEntry);
    const std::string name = entry ? nox::strings::toUtf8(entry->entry) : std::string();
    const std::string folder = settings_.map.substr(0, 8);
    gui::setEnabled(advanced_->find(kRuleSaveAs), !name.empty() && !equalsNoCase(name, folder) && !equalsNoCase(name, "user"));
}

// The Advanced window's rule-set page: the chosen map's saved .rul files listed, scrolled,
// saved as, saved, loaded and deleted. Always reports the message as handled.
bool ServerOptions::onRulePage(gui::Window& from, int message)
{
    (void)message;
    gui::Window* list = advanced_->find(kRuleList);
    if (!list) {
        return true;
    }
    const auto chosen = [&]() -> std::string {
        return list->selectedLine >= 0 && list->selectedLine < static_cast<int>(list->lines.size())
                   ? list->lines[static_cast<std::size_t>(list->selectedLine)].text
                   : std::string();
    };
    // The slot's edit, then the file's write (with no kept lines): the pages' rules into the settings and
    // the file for the chosen map and type.
    const auto write = [&](const std::string& name) {
        rules_.spells = work_.spells;
        rules_.weapons = work_.weapons;
        rules_.armor = work_.armor;
        SettingsEdit e; // (into the current slot)
        e.fill = windowState();
        editSlot(e);
        RuleSet out = work_;
        out.other.clear();
        if (callbacks_.saveRuleSet && !settings_.map.empty()) {
            callbacks_.saveRuleSet(settings_.map, name, out, settings_.mode);
        }
    };
    switch (from.id) {
    case kRuleList:
        gui_.resources().sound(0x2FE);
        ruleButtons();
        return true;
    case kRuleUp:
    case kRuleDown:
        gui_.resources().sound(0x2FE);
        scrollList(list, list->scroll + (from.id == kRuleUp ? -1 : 1));
        return true;
    case kRuleSaveAs: {
        // 0x27BC: written (over one of the name too), then onto the list if it isn't there; the
        // name emptied.
        gui_.resources().sound(0x2FE);
        gui::Window* entry = advanced_->find(kRuleEntry);
        if (!entry || entry->entry.empty()) {
            return true;
        }
        const std::string name = nox::strings::toUtf8(entry->entry);
        write(name);
        entry->entry.clear();
        if (std::ranges::none_of(list->lines, [&](const auto& l) { return equalsNoCase(l.text, name); })) {
            list->lines.push_back({name, list->textColor});
        }
        ruleButtons();
        return true;
    }
    case kRuleSave:
        // 0x27BD: the chosen one written; nothing chosen after.
        gui_.resources().sound(0x2FE);
        if (const std::string name = chosen(); !name.empty()) {
            write(name);
        }
        list->selectedLine = -1;
        ruleButtons();
        return true;
    case kRuleLoad:
        // 0x27BE: the chosen one read into the settings for their type (flags 7),
        // the pages' copy from them.
        gui_.resources().sound(0x2FE);
        if (const std::string name = chosen(); !name.empty() && callbacks_.rulesFor) {
            rules_ = callbacks_.rulesFor(settings_.map, settings_.mode, name + ".rul");
            work_ = rules_;
            SettingsEdit e; // (into the current slot, its edit with 1)
            e.rules = rules_;
            e.changed = true;
            editSlot(e);
        }
        list->selectedLine = -1;
        ruleButtons();
        return true;
    case kRuleDelete:
        // 0x27BF: the file and its line.
        gui_.resources().sound(0x2FE);
        if (const std::string name = chosen(); !name.empty()) {
            if (callbacks_.deleteRuleSet) {
                callbacks_.deleteRuleSet(settings_.map, name);
            }
            list->lines.erase(list->lines.begin() + list->selectedLine);
            list->selectedLine = -1;
            scrollList(list, list->scroll);
        }
        ruleButtons();
        return true;
    default:
        return true;
    }
}

// ---- the spells (spelllst.wnd)

int ServerOptions::spellByTitle(const std::string& title) const
{
    // spellByTitle: the first spell of that title.
    if (!callbacks_.things || title.empty()) {
        return 0;
    }
    for (std::uint32_t id = 1; id <= 0x88; ++id) {
        const auto* def = callbacks_.things->findSpell(id);
        if (def && !def->title.empty() && gui_.text(def->title) == title) {
            return static_cast<int>(id);
        }
    }
    return 0;
}

void ServerOptions::fillSpellPage()
{
    // guiSpelllistLoad: its setup (the lists 15 lines high, the checkboxes a
    // line apart beside them), then each spell (1-0x88) that isn't 0x15000's and is a class's:
    // its title, and its class (Common for 0x1000000 or both classes; Wiz, Conj).
    gui::Window* spells = advanced_->find(kSpellList);
    gui::Window* schools = advanced_->find(kSchoolList);
    if (!spells || !schools) {
        return;
    }
    const int fh = fontHeight(*spells);
    spells->rect.h = (fh + 1) * 15 + 2;
    schools->rect.h = spells->rect.h;
    for (int k = 0; k < kChecks; ++k) {
        if (gui::Window* box = advanced_->find(kSpellChecks + k)) {
            box->rect.y = spells->rect.y + 2 + (fh + 1) * (k + 1);
        }
    }
    spells->lines.clear();
    schools->lines.clear();
    spells->scroll = 0;
    schools->scroll = 0;
    for (std::uint32_t id = 1; callbacks_.things && id <= 0x88; ++id) {
        const auto* def = callbacks_.things->findSpell(id);
        if (!def || (def->flags & 0x15000u)) {
            continue;
        }
        const std::uint32_t f = def->flags;
        std::string school;
        // The class bits, low to high: common, wizard, conjurer; both classes count as common.
        if ((f & 0x1000000u) || ((f & 0x2000000u) && (f & 0x4000000u))) {
            school = gui_.text("spelllst.c:Common");
        } else if (f & 0x6000000u) {
            if (f & 0x2000000u) {
                school += gui_.text("spelllst.c:SpellWizard");
            }
            if (f & 0x4000000u) {
                school += gui_.text("spelllst.c:SpellConjurer");
            }
        } else {
            continue;
        }
        schools->lines.push_back({school, schools->textColor});
        spells->lines.push_back({def->title.empty() ? std::string() : gui_.text(def->title), spells->textColor});
    }
    spellChecks();
}

void ServerOptions::spellChecks()
{
    // A checkbox a line shown, ticked for a spell allowed; none past the lines.
    gui::Window* spells = advanced_->find(kSpellList);
    if (!spells) {
        return;
    }
    for (int k = 0; k < kChecks; ++k) {
        gui::Window* box = advanced_->find(kSpellChecks + k);
        if (!box) {
            continue;
        }
        const auto line = static_cast<std::size_t>(spells->scroll + k);
        const std::string title = line < spells->lines.size() ? spells->lines[line].text : std::string();
        box->show(!title.empty());
        const int spell = spellByTitle(title);
        gui::setChecked(box, spell != 0 && work_.spellAllowed(spell));
    }
}

bool ServerOptions::onSpellPage(gui::Window& from, int message)
{
    (void)message;
    gui::Window* spells = advanced_->find(kSpellList);
    gui::Window* schools = advanced_->find(kSchoolList);
    if (!spells || !schools) {
        return true;
    }
    const bool flagball = (startMode_ & 0x40) || (settings_.mode & 0x40); // (Game_HasFlags(0x40), the settings' type)
    const std::optional<RuleSet> internet = callbacks_.internetRules ? callbacks_.internetRules() : std::nullopt;
    if (from.id == kSpellUp || from.id == kSpellDown) {
        // 0x459 / 0x45A: both lists.
        gui_.resources().sound(0x2FE);
        scrollList(spells, spells->scroll + (from.id == kSpellUp ? -1 : 1));
        schools->scroll = spells->scroll;
        spellChecks();
        return true;
    }
    if (from.id == kSpellAll || from.id == kSpellNone) {
        // 0x45B / 0x45C: every listed spell on (but Wall in flagball) or off; in a WOL game
        // internet.rul's off stay off.
        gui_.resources().sound(0x2FE);
        for (const auto& line : spells->lines) {
            const int spell = spellByTitle(line.text);
            if (spell == 0 || (from.id == kSpellAll && flagball && spell == kWall)) {
                continue;
            }
            work_.setSpell(spell, from.id == kSpellAll);
        }
        if (internet) {
            for (std::size_t i = 0; i < work_.spells.size(); ++i) {
                work_.spells[i] &= internet->spells[i];
            }
        }
        spellChecks();
        return true;
    }
    if (from.id < kSpellChecks || from.id >= kSpellChecks + kChecks) {
        return true;
    }
    // 0x460-0x46D: the line's spell on or off as the box now is; in a WOL game one internet.rul
    // leaves out can't be changed, nor Wall in flagball (the box put back, a notice).
    gui_.resources().sound(0x2FE);
    const auto line = static_cast<std::size_t>(spells->scroll + (from.id - kSpellChecks));
    const int spell = line < spells->lines.size() ? spellByTitle(spells->lines[line].text) : 0;
    if (spell == 0) {
        return true;
    }
    if (internet && !internet->spellAllowed(spell)) {
        gui::setChecked(&from, !gui::isChecked(from));
        deferred_.push_back([this] { openNotice(gui_.text("spelllst.c:Notice"), gui_.text("spelllst.c:NotInternet")); });
        return true;
    }
    if (flagball && spell == kWall) {
        gui::setChecked(&from, !gui::isChecked(from));
        deferred_.push_back([this] { openNotice(gui_.text("spelllst.c:Notice"), gui_.text("plyrspel.c:Illegal")); });
        return true;
    }
    work_.setSpell(spell, gui::isChecked(from));
    editSlot({.changed = true}); // (the slot's edit with 1)
    return true;
}

// ---- the weapons and armour (objlst.wnd)

void ServerOptions::fillObjectPage(bool armour)
{
    // guiObjlistLoad: its setup (the list 15 lines high, the checkboxes beside
    // it), the labels by bit (weapons from 4, 0x19 of them; armour from 1, 0x1A), the title.
    armourPage_ = armour;
    gui::Window* list = advanced_->find(kObjectList);
    if (!list) {
        return;
    }
    const int fh = fontHeight(*list);
    list->rect.h = (fh + 1) * 15 + 2;
    for (int k = 0; k < kChecks; ++k) {
        if (gui::Window* box = advanced_->find(kObjectChecks + k)) {
            box->rect.y = list->rect.y + 2 + (fh + 1) * (k + 1);
        }
    }
    list->lines.clear();
    list->scroll = 0;
    for (const auto* item : armour ? nox::server::ruleArmourByBit() : nox::server::ruleWeaponsByBit()) {
        list->lines.push_back({gui_.text(item->label), list->textColor});
    }
    list->text = armour ? gui_.text("servopts.wnd:Armor") : gui_.text("objlst.c:Weapons");
    objectChecks();
}

void ServerOptions::objectChecks()
{
    // As the exe has it (an original bug): the boxes count bits from the list's
    // first line shown, skipping bits 0, 1, 4 and 5, and stop at the lines' count. For the weapons
    // from the top that's their lines' bits; scrolled, or for the armour, the boxes show other
    // bits' states than their lines', and the last weapons get none.
    gui::Window* list = advanced_->find(kObjectList);
    if (!list) {
        return;
    }
    const auto count = static_cast<int>(list->lines.size());
    const std::uint32_t mask = armourPage_ ? work_.armor : work_.weapons;
    int bit = list->scroll;
    for (int k = 0; k < kChecks; ++k, ++bit) {
        gui::Window* box = advanced_->find(kObjectChecks + k);
        while ((1u << (bit & 31)) & 0x33u) {  // (0x33: bits 0, 1, 4 and 5)
            ++bit;
        }
        if (!box) {
            continue;
        }
        box->show(bit < count);
        if (bit < count) {
            gui::setChecked(box, (mask & (1u << (bit & 31))) != 0);
        }
    }
}

bool ServerOptions::onObjectPage(gui::Window& from, int message)
{
    (void)message;
    gui::Window* list = advanced_->find(kObjectList);
    if (!list) {
        return true;
    }
    std::uint32_t& mask = armourPage_ ? work_.armor : work_.weapons;
    switch (from.id) {
    case kObjectUp:
    case kObjectDown:
        gui_.resources().sound(0x2FE);
        scrollList(list, list->scroll + (from.id == kObjectUp ? -1 : 1));
        objectChecks();
        return true;
    case kObjectAll:
    case kObjectNone:
        // 0x5EB / 0x5EC: the whole mask (the bits not listed too).
        gui_.resources().sound(0x2FE);
        mask = from.id == kObjectAll ? 0xFFFFFFFFu : 0u;
        objectChecks();
        return true;
    default:
        break;
    }
    if (from.id < kObjectChecks || from.id >= kObjectChecks + kChecks) {
        return true;
    }
    // 0x5F0-0x5FD: the line beside the box, its bit by its label
    // (the table's first of that label), on or off as the box now is (the boxes not refilled).
    gui_.resources().sound(0x2FE);
    const auto line = static_cast<std::size_t>(list->scroll + (from.id - kObjectChecks));
    if (line >= list->lines.size()) {
        return true;
    }
    std::uint32_t bit = 0;
    for (const auto& item : armourPage_ ? nox::server::ruleArmour() : nox::server::ruleWeapons()) {
        if (gui_.text(item.label) == list->lines[line].text) {
            bit = item.bit;
            break;
        }
    }
    mask = gui::isChecked(from) ? (mask | bit) : (mask & ~bit);
    editSlot({.changed = true}); // (the slot's edit with 1)
    return true;
}

// ---- the notice (Gui_Dialog_Create with OK)

void ServerOptions::openNotice(const std::string& title, const std::string& message, std::uint32_t flags,
                               std::function<void()> onOk)
{
    closeNotice();
    try {
        notice_ = gui_.load(windowDir_ / "dlg.wnd", [this, onOk](gui::Window&, int msg, gui::Window& from) {
            if (msg != gui::Clicked) {
                return true;
            }
            const bool yes = from.id == kDlgOk || from.id == kDlgYes;
            if (yes || from.id == kDlgCancel || from.id == kDlgNo) {
                gui_.resources().sound(0x2FE);
                deferred_.push_back([this, then = yes ? onOk : std::function<void()>{}] {
                    closeNotice();
                    if (then) {
                        then();
                    }
                });
            }
            return true;
        });
    } catch (const std::exception&) {
        notice_ = nullptr;
        return;
    }
    // Centred on the 640 x 480 layout.
    gui::centreOnScreen(*notice_);
    notice_->status |= nox::gui::status::Above;
    const std::pair<std::uint32_t, std::int32_t> buttons[] = {{1, kDlgOk}, {2, kDlgCancel}, {8, kDlgYes}, {0x10, kDlgNo}};
    for (auto& c : notice_->children) {
        bool shown = c->id == kDlgText || c->id == kDlgTitle;
        for (const auto& [bit, id] : buttons) {
            shown = shown || (c->id == id && (flags & bit) != 0);
        }
        c->show(shown);
    }
    if (gui::Window* t = notice_->find(kDlgTitle)) {
        t->text = title;
    }
    if (gui::Window* t = notice_->find(kDlgText)) {
        t->text = message;
    }
    // The dialog's fonts (1) after each of them (the spell list's NotInternet / plyrspel.c:Illegal,
    // guiServerOptionsProcPre's NeedTeams and TooManyTeams):
    // the title and text large, where the game's windows are small.
    gui::setDialogFonts(*notice_, true);
    gui_.setModal(notice_);
}

void ServerOptions::closeNotice()
{
    if (!notice_) {
        return;
    }
    gui_.destroy(notice_);
    notice_ = nullptr;
    gui_.setModal(advanced_);
}

} // namespace noxsdl
