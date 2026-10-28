#include "gui/session.hpp"

#include "editor/insert.hpp"

#include <imgui.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>

namespace fs = std::filesystem;

namespace videotools::gui {

namespace {

constexpr SDL_DialogFileFilter kBagFilter[] = {{"video.bag", "bag"}};
constexpr SDL_DialogFileFilter kPackFilter[] = {{"Image pack", "nxp"}};

std::string crcText(std::uint32_t crc)
{
    return std::format("{:08X}", crc);
}

} // namespace

fs::path pathFromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string utf8(const fs::path& p)
{
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

void loadSettings(const fs::path& file, Settings& s)
{
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        const auto number = [&](int& v, int lo, int hi) {
            int n = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), n).ec == std::errc{}) {
                v = std::clamp(n, lo, hi);
            }
        };
        if (key == "gameDir") {
            s.gameDir = value;
        } else if (key == "packPath") {
            s.packPath = value;
        } else if (key == "outDir") {
            s.outDir = value;
        } else if (key == "extractDir") {
            s.extractDir = value;
        } else if (key == "saveDir") {
            s.saveDir = value;
        } else if (key == "zoom") {
            number(s.zoom, 1, 16);
        } else if (key == "background") {
            number(s.background, 0, int(std::size(kBackgrounds)) - 1);
        } else if (key == "speed") {
            number(s.speed, 0, 4);
        } else if (key == "compare") {
            number(s.compare, 0, 2);
        } else if (key == "theme") {
            s.theme = value;
        } else if (key == "uiScale") {
            number(s.uiScalePercent, 50, 300);
        } else if (key == "floating") {
            s.floating = value == "1";
        } else if (key == "modsDir") {
            s.modsDir = value;
        } else if (key == "charactersDir") {
            s.charactersDir = value;
        } else if (key == "packId") {
            s.packId = value;
        }
    }
}

void saveSettings(const fs::path& file, const Settings& s)
{
    std::ofstream out(file, std::ios::trunc);
    out << "gameDir=" << s.gameDir << "\npackPath=" << s.packPath << "\noutDir=" << s.outDir
        << "\nextractDir=" << s.extractDir << "\nsaveDir=" << s.saveDir << "\nzoom=" << s.zoom
        << "\nbackground=" << s.background << "\nspeed=" << s.speed << "\ncompare=" << s.compare
        << "\ntheme=" << s.theme << "\nuiScale=" << s.uiScalePercent << "\nfloating=" << (s.floating ? 1 : 0)
        << "\nmodsDir=" << s.modsDir << "\ncharactersDir=" << s.charactersDir << "\npackId=" << s.packId << "\n";
}

// ------------------------------------------------------------------------------------- dialogs

void Dialogs::request(int id, Kind kind, SDL_Window* window, const std::string& defaultPath,
                      const SDL_DialogFileFilter* filters, int filterCount)
{
    auto* pending = new Pending{this, id};
    const char* where = defaultPath.empty() ? nullptr : defaultPath.c_str();
    switch (kind) {
    case Open:
        SDL_ShowOpenFileDialog(answered, pending, window, filters, filterCount, where, false);
        break;
    case Save:
        SDL_ShowSaveFileDialog(answered, pending, window, filters, filterCount, where);
        break;
    case Folder:
        SDL_ShowOpenFolderDialog(answered, pending, window, where, false);
        break;
    }
}

void SDLCALL Dialogs::answered(void* userdata, const char* const* files, int)
{
    std::unique_ptr<Pending> pending(static_cast<Pending*>(userdata));
    if (files && files[0]) {
        std::lock_guard lock(pending->self->mutex_);
        pending->self->answers_[pending->id] = files[0];
    }
}

std::optional<std::string> Dialogs::take(int id)
{
    std::lock_guard lock(mutex_);
    const auto it = answers_.find(id);
    if (it == answers_.end()) {
        return std::nullopt;
    }
    std::string s = std::move(it->second);
    answers_.erase(it);
    return s;
}

// ------------------------------------------------------------------------------------- session

Session::Session(SDL_Window* window, SDL_Renderer* renderer, float scale, Settings& settings)
    : window_(window), scale_(scale), settings_(settings), sd_(renderer), hd_(renderer, 12)
{
}

Session::~Session()
{
    if (job_) {
        job_->progress.cancel = true;
        job_->thread.join();
    }
    for (auto& e : encoding_) {
        e.encoded.wait();
    }
}

void Session::openGame(const fs::path& dir)
{
    sd_.setLoader({});
    hd_.setLoader({});
    bag_.reset();
    loadError_.clear();
    gameDir_ = dir;
    savedIdxCrc_.reset();
    try {
        paths_ = GamePaths::inDir(dir);
        bag_ = std::make_unique<editor::BagDocument>(paths_);
        bagIdxCrc_ = editor::idxCrc(paths_.idx);
    } catch (const std::exception& ex) {
        bag_.reset();
        loadError_ = ex.what();
        return;
    }
    sd_.setLoader([this](std::int32_t index) -> std::optional<LoadedImage> {
        if (!bag_ || index < 0 || std::size_t(index) >= bag_->size()) {
            return std::nullopt;
        }
        auto img = bag_->decode(index);
        LoadedImage l;
        l.pixelWidth = l.width = img.width;
        l.pixelHeight = l.height = img.height;
        l.offsetX = img.offsetX;
        l.offsetY = img.offsetY;
        l.type = img.type;
        l.headerByte = img.unknown;
        l.rgba = std::move(img.rgba);
        l.material = std::move(img.material);
        l.edgeMask = !img.edgeMask.empty();
        return l;
    });
    if (pack_) {
        openPack({}); // (the pack and its edits stay; its loader again)
    }
}

void Session::openPack(const fs::path& path)
{
    hd_.setLoader({});
    if (path.empty()) {
        if (!pack_) {
            return;
        }
    } else {
        try {
            pack_ = std::make_unique<editor::PackDocument>(path);
        } catch (const std::exception& ex) {
            pack_.reset();
            error(std::format("Couldn't open the pack {}: {}", utf8(path), ex.what()));
            return;
        }
        encode_.bc7 = pack_->hasBc7();
        settings_.packPath = utf8(path);
    }
    hd_.setLoader([this](std::int32_t index) -> std::optional<LoadedImage> {
        if (!pack_ || index < 0) {
            return std::nullopt;
        }
        auto img = pack_->read(std::uint32_t(index));
        if (!img) {
            return std::nullopt;
        }
        LoadedImage l;
        l.pixelWidth = img->pixelWidth;
        l.pixelHeight = img->pixelHeight;
        l.width = img->width;
        l.height = img->height;
        l.offsetX = img->offsetX;
        l.offsetY = img->offsetY;
        l.type = img->type;
        l.headerByte = img->headerByte;
        l.rgba = std::move(img->rgba);
        l.material = std::move(img->material);
        l.edgeMask = !img->edgeMask.empty();
        if (const auto* e = pack_->entry(std::uint32_t(index))) {
            static constexpr const char* kCodecs[] = {"stored", "zstd", "zstd + gradient", "BC7"};
            l.extra = std::format("{}{}", e->codec < 4 ? kCodecs[e->codec] : "?",
                                  e->codec == nox::video::kCodecBc7 ? std::format(", {} mip levels", e->levels) : "");
        }
        return l;
    });
}

void Session::newPack()
{
    if (!bag_) {
        return;
    }
    hd_.setLoader({});
    pack_ = std::make_unique<editor::PackDocument>(std::uint32_t(bag_->size()), std::uint32_t(bag_->size()), bagIdxCrc_);
    encode_.bc7 = true;
    openPack({});
    setStatus("New empty image pack: put HD images in and save it as a new file");
}

void Session::closePack()
{
    hd_.setLoader({});
    pack_.reset();
    settings_.packPath.clear();
}

bool Session::packMatchesBag() const
{
    return !pack_ || pack_->sourceIdxCrc() == 0 || pack_->sourceIdxCrc() == bagIdxCrc_;
}

std::size_t Session::imageCount() const
{
    return std::max<std::size_t>(bag_ ? bag_->size() : 0, pack_ ? pack_->size() : 0);
}

void Session::changed(std::int32_t index)
{
    sd_.forget(index);
    hd_.forget(index);
}

std::vector<fs::path> Session::sources() const
{
    std::vector<fs::path> s = {paths_.bag, paths_.idx};
    if (pack_ && !pack_->path().empty()) {
        s.push_back(pack_->path());
    }
    return s;
}

// ---------------------------------------------------------------------------------------- edits

void Session::replaceBag(std::int32_t index, const editor::ImageFile& file)
{
    if (!bag_) {
        return;
    }
    try {
        std::vector<std::string> warnings;
        bag_->replace(index, editor::bagImageForSlot(file, index, *bag_, warnings));
        for (const auto& w : warnings) {
            log(std::format("#{}: {}", index, w));
        }
        changed(index);
        setStatus(std::format("#{}: video.bag image replaced from {}{}", index, utf8(file.png.filename()),
                              bag_->edited(index) ? "" : " (the same as the original: no change)"));
    } catch (const std::exception& ex) {
        error(std::format("Couldn't put {} into video.bag #{}: {}", utf8(file.png.filename()), index, ex.what()));
    }
}

void Session::replacePack(std::int32_t index, const editor::ImageFile& file)
{
    queueInserts({InsertTask{file, editor::Target::Pack, index}});
}

void Session::removePack(const std::vector<std::int32_t>& indexes)
{
    if (!pack_) {
        return;
    }
    std::size_t n = 0;
    for (const auto i : indexes) {
        if (pack_->present(std::uint32_t(i))) {
            pack_->remove(std::uint32_t(i));
            changed(i);
            ++n;
        }
    }
    setStatus(std::format("Removed {} HD image{} from the pack (saved with it)", n, n == 1 ? "" : "s"));
}

void Session::revert(const std::vector<std::int32_t>& indexes)
{
    std::size_t n = 0;
    for (const auto i : indexes) {
        if (bag_ && bag_->edited(i)) {
            bag_->revert(i);
            ++n;
        }
        if (pack_ && pack_->edited(std::uint32_t(i))) {
            pack_->revert(std::uint32_t(i));
            ++n;
        }
        changed(i);
    }
    setStatus(std::format("Reverted {} edit{}", n, n == 1 ? "" : "s"));
}

void Session::queueInserts(std::vector<InsertTask> tasks)
{
    if (insertQueue_.empty() && encoding_.empty()) {
        insertDone_ = insertFailed_ = 0;
    }
    for (auto& t : tasks) {
        insertQueue_.push_back(std::move(t));
    }
}

// Bag images go in on this thread; pack images are made here and BC7-encoded on worker threads,
// a few at a time, then put in as they finish.
void Session::pumpInserts()
{
    for (auto it = encoding_.begin(); it != encoding_.end();) {
        if (it->encoded.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        try {
            if (pack_) {
                pack_->put(std::uint32_t(it->index), it->encoded.get(), it->image);
                changed(it->index);
                ++insertDone_;
            }
        } catch (const std::exception& ex) {
            log(std::format("{}: {}", it->png, ex.what()));
            ++insertFailed_;
        }
        it = encoding_.erase(it);
    }
    const std::size_t parallel = std::max(1u, std::thread::hardware_concurrency());
    const std::uint64_t start = SDL_GetTicksNS();
    while (!insertQueue_.empty() && encoding_.size() < parallel && SDL_GetTicksNS() - start < 12'000'000) {
        InsertTask t = std::move(insertQueue_.front());
        insertQueue_.pop_front();
        const std::string png = utf8(t.file.png.filename());
        try {
            std::vector<std::string> warnings;
            if (t.target == editor::Target::Bag) {
                if (!bag_) {
                    throw std::runtime_error("no video.bag open");
                }
                bag_->replace(t.index, editor::bagImageForSlot(t.file, t.index, *bag_, warnings));
                changed(t.index);
                ++insertDone_;
            } else {
                if (!pack_) {
                    throw std::runtime_error("an HD image, but no pack is open (open one or make a new one)");
                }
                if (!bag_) {
                    throw std::runtime_error("no video.bag open");
                }
                auto image = std::make_shared<const nox::video::PackImage>(
                    editor::packImageForSlot(t.file, t.index, *bag_, *pack_, warnings));
                const auto options = encode_;
                encoding_.push_back({t.index, image,
                                     std::async(std::launch::async, [image, options] { return editor::encodeForPack(*image, options); }),
                                     png});
            }
            for (const auto& w : warnings) {
                log(std::format("{} -> #{}: {}", png, t.index, w));
            }
        } catch (const std::exception& ex) {
            log(std::format("{} -> #{}: {}", png, t.index, ex.what()));
            ++insertFailed_;
        }
    }
    if (insertQueue_.empty() && encoding_.empty() && (insertDone_ || insertFailed_)) {
        setStatus(std::format("Put in {} image{}{}", insertDone_, insertDone_ == 1 ? "" : "s",
                              insertFailed_ ? std::format(", {} failed (see Messages)", insertFailed_) : std::string()));
        insertDone_ = insertFailed_ = 0;
    }
}

// --------------------------------------------------------------------------------------- saving

void Session::saveBagAs()
{
    if (!bag_ || busy()) {
        return;
    }
    const fs::path start = settings_.saveDir.empty() ? fs::path() : pathFromUtf8(settings_.saveDir) / "video.bag";
    dialogs_.request(kDialogSaveBag, Dialogs::Save, window_, utf8(start), kBagFilter, 1);
}

void Session::chosenBagTarget(const fs::path& chosen)
{
    fs::path bagPath = chosen;
    if (!bagPath.has_extension()) {
        bagPath += ".bag";
    }
    const fs::path idxPath = editor::idxBeside(bagPath);
    for (const auto& target : {bagPath, idxPath}) {
        const std::string why = editor::checkSaveTarget(target, sources(), {gameDir_});
        if (!why.empty()) {
            error("Not saved: " + why);
            return;
        }
    }
    settings_.saveDir = utf8(bagPath.parent_path());
    auto save = [this, bagPath] {
        const auto edits = bag_->bagEdits();
        const auto paths = paths_;
        auto crc = std::make_shared<std::uint32_t>(0);
        startJob(
            "Saving " + utf8(bagPath.filename()), bagPath.parent_path(), bag_->bag().segments().size(),
            [paths, edits, bagPath, crc](ExportProgress& p) {
                const auto r = editor::BagDocument::save(paths, edits, bagPath, [&p](std::size_t done, std::size_t total) {
                    p.done = done;
                    p.total = total;
                });
                *crc = r.idxCrc;
                return std::format("Saved {} and {}: {} image{} replaced ({} segments rebuilt, {} copied); video.idx CRC "
                                   "{:08X}",
                                   utf8(bagPath), utf8(editor::idxBeside(bagPath).filename()), r.imagesReplaced,
                                   r.imagesReplaced == 1 ? "" : "s", r.segmentsRebuilt, r.segmentsCopied, r.idxCrc);
            },
            [this, crc] {
                savedIdxCrc_ = *crc;
                if (pack_ && pack_->sourceIdxCrc() != 0 && pack_->sourceIdxCrc() != *crc) {
                    log("The saved video.idx differs from the one the pack was made for: NoX Core uses a pack only with "
                        "its own video.idx. Save the pack too, stamped for the saved video.idx.");
                }
            });
    };
    std::string exists;
    for (const auto& target : {bagPath, idxPath}) {
        if (fs::exists(target)) {
            exists += "\n  " + utf8(target);
        }
    }
    if (!exists.empty()) {
        confirm("Overwrite?", "These files exist and will be replaced:" + exists, save);
    } else {
        save();
    }
}

void Session::savePackAs()
{
    if (!pack_ || busy()) {
        return;
    }
    stampChoice_ = savedIdxCrc_ ? 2 : (packMatchesBag() ? 0 : 1);
    copySidecar_ = true;
    packOptionsOpen_ = true;
}

void Session::savePackOptions()
{
    if (packOptionsOpen_) {
        ImGui::OpenPopup("Save the image pack");
        packOptionsOpen_ = false;
    }
    if (!ImGui::BeginPopupModal("Save the image pack", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextUnformatted("NoX Core uses a pack only with the video.idx it was made for. Stamp it for:");
    ImGui::RadioButton(std::format("the video.idx it was made for ({})", crcText(pack_->sourceIdxCrc())).c_str(),
                       &stampChoice_, 0);
    ImGui::RadioButton(std::format("the open game folder's video.idx ({})", crcText(bagIdxCrc_)).c_str(), &stampChoice_, 1);
    ImGui::BeginDisabled(!savedIdxCrc_);
    ImGui::RadioButton(std::format("the video.bag saved last ({})", savedIdxCrc_ ? crcText(*savedIdxCrc_) : std::string("none yet"))
                           .c_str(),
                       &stampChoice_, 2);
    ImGui::EndDisabled();
    if (!pack_->path().empty()) {
        ImGui::Checkbox("Copy its animations sidecar (<pack>.animations.json) beside the new pack", &copySidecar_);
    }
    ImGui::Spacing();
    if (ImGui::Button("Choose the file...")) {
        const fs::path start = settings_.saveDir.empty() ? fs::path() : pathFromUtf8(settings_.saveDir) / "video_hd.nxp";
        dialogs_.request(kDialogSavePack, Dialogs::Save, window_, utf8(start), kPackFilter, 1);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Session::chosenPackTarget(const fs::path& chosen)
{
    fs::path out = chosen;
    if (!out.has_extension()) {
        out += ".nxp";
    }
    const std::string why = editor::checkSaveTarget(out, sources(), {gameDir_});
    if (!why.empty()) {
        error("Not saved: " + why);
        return;
    }
    settings_.saveDir = utf8(out.parent_path());
    editor::PackSaveOptions options;
    options.copySidecar = copySidecar_;
    if (stampChoice_ == 1) {
        options.idxCrc = bagIdxCrc_;
    } else if (stampChoice_ == 2 && savedIdxCrc_) {
        options.idxCrc = *savedIdxCrc_;
    }
    auto save = [this, out, options] {
        const fs::path source = pack_->path();
        const std::uint32_t slots = pack_->size();
        const std::uint32_t images = pack_->sourceImages();
        const std::uint32_t crc = pack_->sourceIdxCrc();
        const auto edits = pack_->packEdits();
        startJob("Saving " + utf8(out.filename()), out.parent_path(), slots,
                 [=](ExportProgress& p) {
                     const auto r = editor::PackDocument::save(source, slots, images, crc, edits, out, options,
                                                               [&p](std::size_t done, std::size_t total) {
                                                                   p.done = done;
                                                                   p.total = total;
                                                               });
                     return std::format("Saved {}: {} written, {} removed, {} copied as stored{}", utf8(out), r.written,
                                        r.removed, r.copied, r.sidecarCopied ? ", animations sidecar copied" : "");
                 });
    };
    std::string exists;
    fs::path sidecar = out;
    sidecar += ".animations.json";
    for (const auto& target : {out, sidecar}) {
        if (fs::exists(target)) {
            exists += "\n  " + utf8(target);
        }
    }
    if (!exists.empty()) {
        confirm("Overwrite?", "These files exist and will be replaced:" + exists, save);
    } else {
        save();
    }
}

// ----------------------------------------------------------------------------------------- jobs

void Session::startJob(std::string label, fs::path folder, std::size_t total, std::function<std::string(ExportProgress&)> work,
                       std::function<void()> onDone)
{
    if (job_) {
        return;
    }
    job_ = std::make_unique<Job>();
    job_->label = std::move(label);
    job_->folder = std::move(folder);
    job_->progress.total = total;
    job_->onDone = std::move(onDone);
    job_->thread = std::thread([job = job_.get(), work = std::move(work)] {
        try {
            job->message = work(job->progress);
        } catch (const std::exception& ex) {
            job->error = ex.what();
        }
        job->finished = true;
    });
}

void Session::pollJob()
{
    if (!job_ || !job_->finished) {
        return;
    }
    job_->thread.join();
    if (!job_->error.empty()) {
        error(job_->label + " failed: " + job_->error);
        setStatus(job_->label + " failed");
    } else {
        setStatus(job_->message, job_->folder);
        log(job_->message);
        if (job_->onDone) {
            job_->onDone();
        }
    }
    job_.reset();
}

// ------------------------------------------------------------------------------------- the user

void Session::setStatus(std::string s, fs::path folder)
{
    status_ = std::move(s);
    statusFolder_ = std::move(folder);
    if (!status_.empty() && (log_.empty() || log_.back() != status_)) {
        log(status_); // (the Log panel keeps what the status bar said)
    }
}

void Session::log(std::string line)
{
    log_.push_back(std::move(line));
    while (log_.size() > 500) {
        log_.pop_front();
    }
}

void Session::error(std::string text)
{
    errorText_ = std::move(text);
    errorOpen_ = true;
    log("error: " + errorText_);
}

void Session::confirm(std::string title, std::string text, std::function<void()> yes)
{
    question_ = Question{std::move(title), std::move(text), std::move(yes)};
    questionOpen_ = true;
}

void Session::popups()
{
    if (errorOpen_) {
        ImGui::OpenPopup("Problem");
        errorOpen_ = false;
    }
    if (ImGui::BeginPopupModal("Problem", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40);
        ImGui::TextUnformatted(errorText_.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (questionOpen_ && question_) {
        ImGui::OpenPopup(question_->title.c_str());
        questionOpen_ = false;
    }
    if (question_ && ImGui::BeginPopupModal(question_->title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(question_->text.c_str());
        ImGui::Spacing();
        bool close = false;
        std::function<void()> yes;
        if (ImGui::Button("Yes")) {
            yes = question_->yes;
            close = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("No") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            close = true;
        }
        if (close) {
            ImGui::CloseCurrentPopup();
            question_.reset();
        }
        ImGui::EndPopup();
        if (yes) {
            yes();
        }
    }
    if (pack_) {
        savePackOptions();
    }
}

void Session::frame()
{
    sd_.newFrame();
    hd_.newFrame();
    pollJob();
    pumpInserts();
    if (const auto p = dialogs_.take(kDialogSaveBag); p && bag_) {
        chosenBagTarget(pathFromUtf8(*p));
    }
    if (const auto p = dialogs_.take(kDialogSavePack); p && pack_) {
        chosenPackTarget(pathFromUtf8(*p));
    }
    popups();
}

} // namespace videotools::gui
