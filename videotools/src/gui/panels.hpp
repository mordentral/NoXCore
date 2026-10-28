// The Content Editor's panels other than the image views: the content browser, the record editor,
// the preview and references, and the Changes / Problems / Log panels (shell.hpp lays them out).
#pragma once

#include "content/character.hpp"
#include "content/font_palette.hpp"
#include "content/text_files.hpp"
#include "gui/shell.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace videotools::gui {

/// Content (left): every section of records, with one search over all of them.
class ContentBrowser {
public:
    explicit ContentBrowser(Workspace& ws) : ws_(ws) {}
    void draw();
    /// Fills the search box (as if typed).
    void setSearch(std::string_view text);

private:
    void row(const content::Record& r, bool showSection);
    void open(const content::Record& r);
    void sectionList(const content::Section& s, const std::vector<std::uint32_t>* only);

    Workspace& ws_;
    char search_[128] = {};
    std::string searchUsed_ = "\x01";
    std::vector<content::RecordRef> results_;
    bool changedOnly_ = false;
    float rowH_ = 0;
};

/// Record (centre): the selected record's fields as a form, and its raw text where it has one.
class RecordPanel {
public:
    explicit RecordPanel(Workspace& ws) : ws_(ws) {}
    ~RecordPanel();
    RecordPanel(const RecordPanel&) = delete;
    RecordPanel& operator=(const RecordPanel&) = delete;
    void draw();

private:
    /// The fields as a table: the rest of the panel, or (fit) only as tall as its rows.
    void form(const std::vector<content::Field>& fields, bool fit = false);
    /// A text table's record as the pack edits it (table_form.cpp); false for other records.
    bool tableForm(const content::Record& r);
    /// A thing.bin type's attribute lines as the pack edits them (table_form.cpp); false for other records.
    bool thingForm(const content::Record& r);
    /// A nox.csf label's texts as the pack edits them; false for other records.
    bool stringForm(const content::Record& r);
    /// An Audio.bag sample or a dialog line: the pack's .wav for it; false for other records.
    bool soundForm(const content::Record& r);
    /// A character (.plr): its name and colours, with the doll (character_form.cpp); false for other records.
    bool characterForm(const content::Record& r);
    void rebuildDoll();
    /// A rule set, the map cycle, the MOTD, a window file or another text file: edited as text, with
    /// what the game reads of it (text_form.cpp); false for other records.
    bool textForm(const content::Record& r);
    /// A text file edited, with what the game reads of it (textForm's, and a mod's mode.json).
    void textEditor(const std::filesystem::path& file);
    /// A mod folder: its content pack, its game mode's mode.json, a test launch (mod_form.cpp).
    bool modForm(const content::Record& r);
    /// A font (.fnt) or default.pal: what it holds, drawn (font_view.cpp); false for other records.
    bool fontForm(const content::Record& r);
    void dropViewerTextures();
    /// That record's text in the table as the pack has it ("" for other records).
    std::string tableText(const content::Record& r) const;
    void fileView(const content::Record& r);

    Workspace& ws_;
    std::optional<content::RecordRef> shown_;
    std::vector<content::Field> fields_;
    std::string text_;
    std::string fileText_;
    std::optional<std::pair<bool, std::string>> soundTarget_; ///< (a sample, else a dialog line; its name) a file dialog is choosing for
    // The character shown, and its doll.
    std::filesystem::path characterFile_;
    std::optional<content::Character> character_;
    std::string characterError_;
    SDL_Texture* doll_ = nullptr;
    int dollW_ = 0;
    int dollH_ = 0;
    bool dollStale_ = true;
    std::unordered_map<std::string, std::int32_t> imageByName_; ///< thing.bin's named images: video.bag index
    // The text file shown.
    std::filesystem::path textFile_;
    content::TextKind textKind_ = content::TextKind::Other;
    std::string textOriginal_;
    std::string textEdit_;
    content::TextReport textReport_;
    bool textStale_ = false;
    double textEditedAt_ = 0;
    // The font or palette shown.
    std::filesystem::path viewerFile_;
    std::string viewerError_;
    std::optional<content::BitmapFont> font_;
    std::optional<content::Palette> palette_;
    SDL_Texture* fontSheet_ = nullptr;
    int fontSheetW_ = 0;
    int fontSheetH_ = 0;
    SDL_Texture* fontSample_ = nullptr;
    int fontSampleW_ = 0;
    int fontSampleH_ = 0;
    char sampleText_[128] = "The quick brown fox, 0123456789";
    std::string sampleShown_;
    SDL_Texture* shadeTex_ = nullptr;
    SDL_Texture* blendTex_ = nullptr;
    std::string testMap_; ///< the map a test launch hosts
    // "New type like this..." (thingForm's popup): the name being typed, the type it's made like.
    void newThingPopup();
    char newThingName_[64] = {};
    std::string newThingLike_;
    std::string newThingError_;
};

/// Preview (right): the record's sprite or animation, its sounds, its text.
class PreviewPanel {
public:
    explicit PreviewPanel(Workspace& ws) : ws_(ws) {}
    void draw(double dt);

private:
    void refresh();

    Workspace& ws_;
    std::optional<content::RecordRef> shown_;
    content::PreviewInfo info_;
    const Entry* entry_ = nullptr; ///< the Sprites view's entry for a thing
    std::int32_t thingIndex_ = -1; ///< its thing
    std::vector<std::string> events_; ///< its sound set's sound events
    int sequence_ = 0;
    int track_ = 0;
    double time_ = 0;
    bool playing_ = true;
};

/// References (right, under the preview).
class ReferencesPanel {
public:
    explicit ReferencesPanel(Workspace& ws) : ws_(ws) {}
    void draw();

private:
    Workspace& ws_;
    std::optional<content::RecordRef> shown_;
    std::vector<content::Reference> refs_;
};

/// Plays a sound event (thing.bin's AUD / AVNT record of that name): one of its Audio.bag files.
void playSoundEvent(Workspace& ws, std::string_view name);
/// Plays an Audio.bag sample by its name.
void playSample(Workspace& ws, const std::string& name);
/// Plays a sound file (a dialog line, a pack's .wav).
void playSoundFile(Workspace& ws, const std::filesystem::path& file);
/// The sound playing, if any: how far along, and Stop.
void playingBar(Workspace& ws);

/// The loose file a File record stands for (nullptr for other records).
const std::filesystem::path* recordFile(const content::GameFiles& files, const content::RecordRef& ref);

/// Changes, Problems, Log (bottom).
void drawChanges(Workspace& ws, const std::vector<ChangeRow>& rows);
void drawProblems(Workspace& ws, const std::vector<ProblemRow>& rows);
void drawLog(Workspace& ws);

} // namespace videotools::gui
