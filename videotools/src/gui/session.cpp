#include "gui/session.hpp"

#include "editor/insert.hpp"

#include "noxcore/strings/l10n.hpp"

#include <imgui.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>

namespace fs = std::filesystem;

namespace videotools::gui {

namespace {

constexpr SDL_DialogFileFilter kBagFilter[] = {{"video.bag", "bag"}};

std::string crcText(std::uint32_t crc)
{
    return std::format("{:08X}", crc);
}

} // namespace

const SDL_DialogFileFilter* packFileFilter()
{
    static const SDL_DialogFileFilter filter[] = {{NOX_TRC("content.dialog.image_pack_filter", "Image pack"), "nxp"}};
    return filter;
}

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
            number(s.background, 0, kBackgroundCount - 1);
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
            error(nox::l10n::format(NOX_TR("content.session.cant_open_pack", "Couldn't open the pack {file}: {error}"), {{"file", utf8(path)}, {"error", ex.what()}}));
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
            const char* codec = e->codec == 0   ? NOX_TRC("content.session.codec_stored", "stored")
                                : e->codec == 1 ? "zstd"
                                : e->codec == 2 ? NOX_TRC("content.session.codec_gradient", "zstd + gradient")
                                : e->codec == 3 ? "BC7"
                                                : "?";
            l.extra = std::format("{}{}", codec,
                                  e->codec == nox::video::kCodecBc7
                                      ? nox::l10n::format(NOX_TR("content.session.mip_levels", ", {n} mip levels"), {{"n", e->levels}})
                                      : std::string());
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
    setStatus(NOX_TR("content.session.new_pack", "New empty image pack: put HD images in and save it as a new file"));
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
        setStatus(nox::l10n::format(bag_->edited(index) ? NOX_TR("content.session.bag_replaced", "#{index}: video.bag image replaced from {file}")
                                                        : NOX_TR("content.session.bag_replaced_same", "#{index}: video.bag image replaced from {file} (the same as the original: no change)"),
                                      {{"index", index}, {"file", utf8(file.png.filename())}}));
    } catch (const std::exception& ex) {
        error(nox::l10n::format(NOX_TR("content.session.cant_put_bag", "Couldn't put {file} into video.bag #{index}: {error}"),
                                {{"file", utf8(file.png.filename())}, {"index", index}, {"error", ex.what()}}));
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
    setStatus(nox::l10n::format(NOX_TRP("content.session.removed_hd", "Removed {n} HD image from the pack (saved with it)", "Removed {n} HD images from the pack (saved with it)", n),
                                  {{"n", n}}));
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
    setStatus(nox::l10n::format(NOX_TRP("content.session.reverted", "Reverted {n} edit", "Reverted {n} edits", n), {{"n", n}}));
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
                    throw std::runtime_error(NOX_TR("content.session.no_bag_open", "no video.bag open"));
                }
                bag_->replace(t.index, editor::bagImageForSlot(t.file, t.index, *bag_, warnings));
                changed(t.index);
                ++insertDone_;
            } else {
                if (!pack_) {
                    throw std::runtime_error(NOX_TR("content.session.no_pack_open", "an HD image, but no pack is open (open one or make a new one)"));
                }
                if (!bag_) {
                    throw std::runtime_error(NOX_TR("content.session.no_bag_open", "no video.bag open"));
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
        setStatus(nox::l10n::format(NOX_TRP("content.session.put_in", "Put in {n} image", "Put in {n} images", insertDone_), {{"n", insertDone_}}) +
                  (insertFailed_ ? nox::l10n::format(NOX_TR("content.session.put_in_failed", ", {n} failed (see Messages)"), {{"n", insertFailed_}}) : std::string()));
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
            error(nox::l10n::format(NOX_TR("content.common.not_saved", "Not saved: {why}"), {{"why", why}}));
            return;
        }
    }
    settings_.saveDir = utf8(bagPath.parent_path());
    auto save = [this, bagPath] {
        const auto edits = bag_->bagEdits();
        const auto paths = paths_;
        auto crc = std::make_shared<std::uint32_t>(0);
        startJob(
            nox::l10n::format(NOX_TR("content.session.saving", "Saving {file}"), {{"file", utf8(bagPath.filename())}}), bagPath.parent_path(), bag_->bag().segments().size(),
            [paths, edits, bagPath, crc](ExportProgress& p) {
                const auto r = editor::BagDocument::save(paths, edits, bagPath, [&p](std::size_t done, std::size_t total) {
                    p.done = done;
                    p.total = total;
                });
                *crc = r.idxCrc;
                return nox::l10n::format(NOX_TRP("content.session.saved_bag",
                                                 "Saved {bag} and {idx}: {n} image replaced ({rebuilt} segments rebuilt, {copied} copied); video.idx CRC {crc}",
                                                 "Saved {bag} and {idx}: {n} images replaced ({rebuilt} segments rebuilt, {copied} copied); video.idx CRC {crc}",
                                                 r.imagesReplaced),
                                         {{"bag", utf8(bagPath)}, {"idx", utf8(editor::idxBeside(bagPath).filename())}, {"n", r.imagesReplaced},
                                          {"rebuilt", r.segmentsRebuilt}, {"copied", r.segmentsCopied}, {"crc", std::format("{:08X}", r.idxCrc)}});
            },
            [this, crc] {
                savedIdxCrc_ = *crc;
                if (pack_ && pack_->sourceIdxCrc() != 0 && pack_->sourceIdxCrc() != *crc) {
                    log(NOX_TR("content.session.idx_differs", "The saved video.idx differs from the one the pack was made for: NoX Core uses a pack only with "
                                                              "its own video.idx. Save the pack too, stamped for the saved video.idx."));
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
        confirm(NOX_TR("content.common.overwrite_title", "Overwrite?"),
                nox::l10n::format(NOX_TR("content.common.files_exist", "These files exist and will be replaced:{files}"), {{"files", exists}}), save);
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
        ImGui::OpenPopup(NOX_TRI("content.session.save_pack_title", "Save the image pack"));
        packOptionsOpen_ = false;
    }
    if (!ImGui::BeginPopupModal(NOX_TRI("content.session.save_pack_title", "Save the image pack"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextUnformatted(NOX_TRC("content.session.stamp_for", "NoX Core uses a pack only with the video.idx it was made for. Stamp it for:"));
    ImGui::RadioButton((nox::l10n::format(NOX_TR("content.session.stamp_made_for", "the video.idx it was made for ({crc})"), {{"crc", crcText(pack_->sourceIdxCrc())}}) +
                        "###stampMadeFor")
                           .c_str(),
                       &stampChoice_, 0);
    ImGui::RadioButton((nox::l10n::format(NOX_TR("content.session.stamp_open", "the open game folder's video.idx ({crc})"), {{"crc", crcText(bagIdxCrc_)}}) +
                        "###stampOpen")
                           .c_str(),
                       &stampChoice_, 1);
    ImGui::BeginDisabled(!savedIdxCrc_);
    ImGui::RadioButton((nox::l10n::format(NOX_TR("content.session.stamp_saved", "the video.bag saved last ({crc})"),
                                          {{"crc", savedIdxCrc_ ? crcText(*savedIdxCrc_) : NOX_TR("content.session.none_yet", "none yet")}}) +
                        "###stampSaved")
                           .c_str(),
                       &stampChoice_, 2);
    ImGui::EndDisabled();
    if (!pack_->path().empty()) {
        ImGui::Checkbox(NOX_TRI("content.session.copy_sidecar", "Copy its animations sidecar (<pack>.animations.json) beside the new pack"), &copySidecar_);
    }
    ImGui::Spacing();
    if (ImGui::Button(NOX_TRI("content.session.choose_file", "Choose the file..."))) {
        const fs::path start = settings_.saveDir.empty() ? fs::path() : pathFromUtf8(settings_.saveDir) / "video_hd.nxp";
        dialogs_.request(kDialogSavePack, Dialogs::Save, window_, utf8(start), packFileFilter(), 1);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(NOX_TRI("content.common.cancel", "Cancel"))) {
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
        error(nox::l10n::format(NOX_TR("content.common.not_saved", "Not saved: {why}"), {{"why", why}}));
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
        startJob(nox::l10n::format(NOX_TR("content.session.saving", "Saving {file}"), {{"file", utf8(out.filename())}}), out.parent_path(), slots,
                 [=](ExportProgress& p) {
                     const auto r = editor::PackDocument::save(source, slots, images, crc, edits, out, options,
                                                               [&p](std::size_t done, std::size_t total) {
                                                                   p.done = done;
                                                                   p.total = total;
                                                               });
                     return nox::l10n::format(NOX_TR("content.session.saved_pack", "Saved {file}: {written} written, {removed} removed, {copied} copied as stored"),
                                              {{"file", utf8(out)}, {"written", r.written}, {"removed", r.removed}, {"copied", r.copied}}) +
                            (r.sidecarCopied ? NOX_TR("content.session.sidecar_copied", ", animations sidecar copied") : std::string());
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
        confirm(NOX_TR("content.common.overwrite_title", "Overwrite?"),
                nox::l10n::format(NOX_TR("content.common.files_exist", "These files exist and will be replaced:{files}"), {{"files", exists}}), save);
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
        error(nox::l10n::format(NOX_TR("content.session.job_failed_because", "{job} failed: {error}"), {{"job", job_->label}, {"error", job_->error}}));
        setStatus(nox::l10n::format(NOX_TR("content.session.job_failed", "{job} failed"), {{"job", job_->label}}));
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
    log(nox::l10n::format(NOX_TR("content.session.log_error", "error: {text}"), {{"text", errorText_}}));
}

void Session::confirm(std::string title, std::string text, std::function<void()> yes)
{
    question_ = Question{std::move(title), std::move(text), std::move(yes)};
    questionOpen_ = true;
}

void Session::popups()
{
    if (errorOpen_) {
        ImGui::OpenPopup(NOX_TRI("content.session.problem_title", "Problem"));
        errorOpen_ = false;
    }
    if (ImGui::BeginPopupModal(NOX_TRI("content.session.problem_title", "Problem"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40);
        ImGui::TextUnformatted(errorText_.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button(NOX_TRI("content.common.ok", "OK")) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
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
        if (ImGui::Button(NOX_TRI("content.common.yes", "Yes"))) {
            yes = question_->yes;
            close = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(NOX_TRI("content.common.no", "No")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
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
