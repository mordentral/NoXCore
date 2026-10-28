// What the GUI's views share: the open video.bag and image pack with their edits, their images as
// textures, the background job (export, extract, save), file dialogs, questions to the user, and
// putting PNGs in.
#pragma once

#include "editor/documents.hpp"
#include "editor/image_files.hpp"
#include "exporter.hpp"
#include "gui/textures.hpp"
#include "image_source.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace videotools::gui {

/// Kept between runs (in the per-user settings folder).
struct Settings {
    std::string gameDir;
    std::string packPath;
    std::string outDir;     // catalog exports
    std::string extractDir; // the editor's extracts
    std::string saveDir;    // where Save As starts
    int zoom = 3;
    int background = 0;
    int speed = 2;
    int compare = 0; // video.bag and HD: 0 side by side, 1 video.bag only, 2 HD only
    std::string theme = "dark";  // the map editor's themes: dark, medium, light
    int uiScalePercent = 100;    // times the display's own scale
    bool floating = true;        // panels may float out of the window (onto another monitor)
    std::string modsDir;         // mods and content packs (empty: mods beside this program)
    std::string charactersDir;   // .plr characters (empty: the game's Save folder)
    std::string packId;          // the content pack edited last (mods\<id>\content)
};

void loadSettings(const std::filesystem::path& file, Settings& s);
void saveSettings(const std::filesystem::path& file, const Settings& s);

std::filesystem::path pathFromUtf8(const std::string& s);
std::string utf8(const std::filesystem::path& p);

/// SDL's file dialogs, which may answer on another thread: each request has an id, and take()
/// hands over the answer once (nothing if cancelled).
class Dialogs {
public:
    enum Kind { Open, Save, Folder };
    void request(int id, Kind kind, SDL_Window* window, const std::string& defaultPath,
                 const SDL_DialogFileFilter* filters = nullptr, int filterCount = 0);
    std::optional<std::string> take(int id);

private:
    struct Pending {
        Dialogs* self;
        int id;
    };
    static void SDLCALL answered(void* userdata, const char* const* files, int filter);
    std::mutex mutex_;
    std::map<int, std::string> answers_;
};

/// A background job (one at a time).
struct Job {
    std::string label;
    std::filesystem::path folder; // "Open folder" afterwards
    ExportProgress progress;
    std::thread thread;
    std::atomic<bool> finished{false};
    std::string message; // the work's summary
    std::string error;
    std::function<void()> onDone; // on the UI thread, after a success
};

/// A PNG to put into a slot.
struct InsertTask {
    editor::ImageFile file;
    editor::Target target = editor::Target::Bag;
    std::int32_t index = -1;
};

class Session {
public:
    Session(SDL_Window* window, SDL_Renderer* renderer, float scale, Settings& settings);
    ~Session();

    SDL_Window* window() const { return window_; }
    float scale() const { return scale_; }
    Settings& settings() { return settings_; }

    // ---- files
    /// Opens a game folder's video.bag (dropping the pack's edits only if asked... callers ask first).
    void openGame(const std::filesystem::path& dir);
    void openPack(const std::filesystem::path& path);
    void newPack();
    void closePack();
    bool bagOpen() const { return bag_ != nullptr; }
    bool packOpen() const { return pack_ != nullptr; }
    editor::BagDocument* bag() { return bag_.get(); }
    editor::PackDocument* pack() { return pack_.get(); }
    const GamePaths& paths() const { return paths_; }
    const std::filesystem::path& gameDir() const { return gameDir_; }
    const std::string& loadError() const { return loadError_; }
    std::uint32_t bagIdxCrc() const { return bagIdxCrc_; }
    std::optional<std::uint32_t> savedIdxCrc() const { return savedIdxCrc_; }
    /// Whether the pack was made for the open video.idx (or doesn't say).
    bool packMatchesBag() const;
    std::size_t imageCount() const;

    TextureCache& sd() { return sd_; }
    TextureCache& hd() { return hd_; }
    /// An image changed: its textures go.
    void changed(std::int32_t index);

    // ---- edits
    void replaceBag(std::int32_t index, const editor::ImageFile& file);
    void replacePack(std::int32_t index, const editor::ImageFile& file);
    void removePack(const std::vector<std::int32_t>& indexes);
    void revert(const std::vector<std::int32_t>& indexes);
    void queueInserts(std::vector<InsertTask> tasks);
    bool inserting() const { return !insertQueue_.empty() || !encoding_.empty(); }
    std::size_t insertsLeft() const { return insertQueue_.size() + encoding_.size(); }
    editor::PackEncodeOptions& encodeOptions() { return encode_; }

    // ---- saving (each asks for a file, checks it, asks before overwriting, then runs as a job)
    void saveBagAs();
    void savePackAs(); // opens the options first

    // ---- jobs
    bool busy() const { return job_ != nullptr; }
    void startJob(std::string label, std::filesystem::path folder, std::size_t total,
                  std::function<std::string(ExportProgress&)> work, std::function<void()> onDone = {});
    Job* job() { return job_.get(); }

    // ---- talking to the user
    void setStatus(std::string s, std::filesystem::path folder = {});
    const std::string& status() const { return status_; }
    const std::filesystem::path& statusFolder() const { return statusFolder_; }
    void log(std::string line);
    const std::deque<std::string>& messages() const { return log_; }
    void error(std::string text);
    void confirm(std::string title, std::string text, std::function<void()> yes);
    Dialogs& dialogs() { return dialogs_; }

    /// Once a frame: job, inserts, dialog answers, popups.
    void frame();

private:
    void pollJob();
    void pumpInserts();
    void popups();
    void savePackOptions();
    void chosenBagTarget(const std::filesystem::path& path);
    void chosenPackTarget(const std::filesystem::path& path);
    std::vector<std::filesystem::path> sources() const;

    SDL_Window* window_;
    float scale_;
    Settings& settings_;
    std::filesystem::path gameDir_;
    GamePaths paths_;
    std::unique_ptr<editor::BagDocument> bag_;
    std::unique_ptr<editor::PackDocument> pack_;
    std::string loadError_;
    std::uint32_t bagIdxCrc_ = 0;
    std::optional<std::uint32_t> savedIdxCrc_; // of the video.idx saved last
    TextureCache sd_;
    TextureCache hd_;
    editor::PackEncodeOptions encode_;

    std::deque<InsertTask> insertQueue_;
    struct Encoding {
        std::int32_t index;
        std::shared_ptr<const nox::video::PackImage> image;
        std::future<nox::video::EncodedImage> encoded;
        std::string png;
    };
    std::vector<Encoding> encoding_;
    std::size_t insertDone_ = 0;
    std::size_t insertFailed_ = 0;

    std::unique_ptr<Job> job_;
    Dialogs dialogs_;
    std::string status_;
    std::filesystem::path statusFolder_;
    std::deque<std::string> log_;
    std::string errorText_;
    bool errorOpen_ = false;
    struct Question {
        std::string title;
        std::string text;
        std::function<void()> yes;
    };
    std::optional<Question> question_;
    bool questionOpen_ = false;
    bool packOptionsOpen_ = false;
    int stampChoice_ = 0; // 0 keep the pack's, 1 the open video.idx, 2 the video.idx saved last
    bool copySidecar_ = true;
};

/// Dialog ids.
enum DialogId {
    kDialogGameDir = 1,
    kDialogExportDir,
    kDialogPack,
    kDialogSaveBag,
    kDialogSavePack,
    kDialogExtract,
    kDialogReplaceBag,
    kDialogReplacePack,
    kDialogInsertFolder,
    kDialogExportFull,
    kDialogReplaceSound,
    kDialogSaveCharacter,
    kDialogSaveText,
};

} // namespace videotools::gui
