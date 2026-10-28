// The movie player (movie_player.hpp): opens a VQA or its HD replacement, keeps the frames in step
// with the movie's sound, draws them letterboxed and handles skipping.
#include "movie_player.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <optional>

namespace noxsdl {

namespace {

namespace movies = nox::client::movies;

/// The HD movie's sound, decoded whole, as a stream for the mixer.
class PcmStream : public nox::audio::AudioDecoder {
public:
    explicit PcmStream(nox::audio::Pcm pcm) : pcm_(std::move(pcm)) {}
    std::uint32_t rate() const override { return pcm_.rate; }
    std::uint16_t channels() const override { return pcm_.channels; }
    std::size_t read(std::int16_t* out, std::size_t frames) override
    {
        const std::size_t n = std::min<std::size_t>(frames, pcm_.frames() - static_cast<std::size_t>(pos_));
        std::copy_n(pcm_.samples.data() + pos_ * pcm_.channels, n * pcm_.channels, out);
        pos_ += n;
        return n;
    }
    void seek(std::uint64_t frame) override { pos_ = std::min<std::uint64_t>(frame, pcm_.frames()); }
    std::uint64_t position() const override { return pos_; }

private:
    nox::audio::Pcm pcm_;
    std::uint64_t pos_ = 0;
};

constexpr std::uint64_t kMaxWaitMs = 3000; ///< (the exe waits for the music as long as it takes)

} // namespace

MoviePlayer::MoviePlayer(SDL_Renderer* renderer, Audio* audio, std::filesystem::path gameDir, std::filesystem::path hdDir)
    : renderer_(renderer), audio_(audio), gameDir_(std::move(gameDir)), hdDir_(std::move(hdDir))
{
}

MoviePlayer::~MoviePlayer()
{
    if (texture_) {
        SDL_DestroyTexture(texture_);
    }
}

bool MoviePlayer::available(const std::string& name) const
{
    return movies::findMovie(gameDir_, name).has_value() || (hdAllowed_ && movies::findHdMovie(hdDir_, name).has_value());
}

// Opens the HD file (FFmpeg's libraries must be there); its sound is decoded whole for the mixer.
// False: the VQA plays instead.
bool MoviePlayer::openHd(const std::filesystem::path& file)
{
    if (!HdVideo::available(hdDir_)) {
        return false;
    }
    std::string error;
    hd_ = HdVideo::open(file, error);
    if (!hd_) {
        SDL_Log("HD video: %s: %s; the VQA plays", file.string().c_str(), error.c_str());
        return false;
    }
    fps_ = hd_->fps() > 0 ? hd_->fps() : 15;
    frames_ = hd_->frameCount();
    width_ = hd_->width();
    height_ = hd_->height();
    nox::audio::Pcm sound = hd_->takeSound();
    hasSound_ = sound.frames() > 0;
    soundRate_ = sound.rate;
    if (hasSound_) {
        hdSound_ = std::make_unique<PcmStream>(std::move(sound));
    }
    return true;
}

void MoviePlayer::vqaTiming(const std::filesystem::path& file)
{
    // The HD movie keeps the VQA's time: its frame rate and frame count from the VQA's VQHD
    // (the HD file's own may be rounded, or missing: Matroska keeps no frame count).
    try {
        std::vector<std::uint8_t> start(256);
        std::ifstream in(file, std::ios::binary);
        in.read(reinterpret_cast<char*>(start.data()), static_cast<std::streamsize>(start.size()));
        start.resize(static_cast<std::size_t>(in.gcount()));
        const nox::video::VqaHeader h = nox::video::readVqaHeader(start);
        if (h.fps > 0 && h.frames > 0) {
            if (h.fps != fps_ || h.frames != frames_) {
                SDL_Log("HD video: %d frames at %.3g fps; timed as the VQA's %d at %d", frames_, fps_, h.frames, h.fps);
            }
            fps_ = h.fps;
            frames_ = h.frames;
        }
    } catch (const std::exception& ex) {
        SDL_Log("movie %s: %s; the HD movie keeps its own timing", file.string().c_str(), ex.what());
    }
}

// Reads the whole VQA; the picture and sound decoders share its bytes.
bool MoviePlayer::openVqa(const std::filesystem::path& file)
{
    try {
        std::ifstream in(file, std::ios::binary);
        auto bytes = std::make_shared<std::vector<std::uint8_t>>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        std::shared_ptr<const std::vector<std::uint8_t>> data = bytes;
        vqa_ = std::make_unique<nox::video::VqaDecoder>(data);
        vqaSound_ = std::make_unique<nox::video::VqaAudioDecoder>(data);
    } catch (const std::exception& ex) {
        SDL_Log("movie %s: %s", file.string().c_str(), ex.what());
        vqa_.reset();
        vqaSound_.reset();
        return false;
    }
    const auto& h = vqa_->header();
    fps_ = h.fps ? h.fps : 15;
    frames_ = h.frames;
    width_ = h.width;
    height_ = h.height;
    hasSound_ = vqaSound_->hasSound();
    soundRate_ = vqaSound_->rate();
    return true;
}

void MoviePlayer::play(const std::string& name, movies::Skip skip, std::function<void()> done)
{
    skip_ = skip;
    done_ = std::move(done);
    skipped_ = false;
    decodedFrame_ = -1;
    shownFrame_ = -1;
    decoderDone_ = false;
    hd_.reset();
    hdSound_.reset();
    vqa_.reset();
    vqaSound_.reset();
    // Nothing plays under engine flag 0x200 (-safe; here --no-movies) or with Shift
    // held; the callback still runs.
    if (disabled_ || (SDL_GetModState() & SDL_KMOD_SHIFT) != 0) {
        SDL_Log("movie %s not played (%s)", name.c_str(), disabled_ ? "--no-movies" : "Shift held");
        state_ = State::Done;
        return;
    }
    // The HD file first when allowed (timed as the VQA when that's there too), else the VQA.
    bool opened = false;
    const auto vqaFile = movies::findMovie(gameDir_, name);
    if (hdAllowed_) {
        if (const auto hd = movies::findHdMovie(hdDir_, name)) {
            opened = openHd(*hd);
            if (opened && vqaFile) {
                vqaTiming(*vqaFile);
            }
        }
    }
    if (!opened && vqaFile) {
        opened = openVqa(*vqaFile);
    }
    if (!opened) {
        state_ = State::Done;
        return;
    }
    SDL_Log("movie %s: %s, %dx%d, %d frames at %.3g fps", name.c_str(), hd_ ? "HD" : "VQA", width_, height_, frames_, fps_);
    if (texture_) {
        SDL_DestroyTexture(texture_);
    }
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, width_, height_);
    if (texture_) {
        SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_LINEAR);
    }
    rgba_.assign(static_cast<std::size_t>(width_) * height_ * 4, 0);
    if (texture_) {
        SDL_UpdateTexture(texture_, nullptr, rgba_.data(), width_ * 4);
    }
    if (audio_) {
        audio_->beginMovie();
    }
    state_ = State::Waiting;
    waitSince_ = SDL_GetTicks();
}

// The movie begins: its sound handed to the mixer's movie stream, the clock from now.
void MoviePlayer::start(std::uint64_t nowMs)
{
    state_ = State::Playing;
    startMs_ = nowMs;
    soundAt_ = 0;
    soundAtMs_ = nowMs;
    if (audio_ && hasSound_) {
        if (hd_) {
            audio_->playMovieSound(std::move(hdSound_));
        } else {
            audio_->playMovieSound(std::move(vqaSound_));
        }
    } else {
        hasSound_ = false;
    }
}

// One more frame decoded; false once the decoder has none left.
bool MoviePlayer::decodeNext()
{
    if (decoderDone_) {
        return false;
    }
    bool ok = false;
    if (hd_) {
        ok = hd_->nextFrame(rgba_);
    } else if (vqa_) {
        ok = vqa_->nextFrame();
    }
    if (!ok) {
        decoderDone_ = true;
        return false;
    }
    ++decodedFrame_;
    return true;
}

// The last frame decoded into the texture (a VQA's converted to RGBA first).
void MoviePlayer::upload()
{
    if (vqa_) {
        vqa_->toRgba(rgba_);
    }
    if (texture_ && !rgba_.empty()) {
        SDL_UpdateTexture(texture_, nullptr, rgba_.data(), width_ * 4);
    }
    shownFrame_ = decodedFrame_;
}

void MoviePlayer::finish()
{
    SDL_Log("movie %s at frame %d", skipped_ ? "stopped" : "over", shownFrame_);
    if (audio_) {
        audio_->endMovie();
    }
    hd_.reset();
    hdSound_.reset();
    vqa_.reset();
    vqaSound_.reset();
    state_ = State::Done;
}

void MoviePlayer::update(std::uint64_t nowMs)
{
    switch (state_) {
    case State::Idle:
        return;
    case State::Done: {
        // Done: the screen cleared (draw() draws black now), the callback run.
        state_ = State::Idle;
        auto done = std::move(done_);
        done_ = {};
        if (done) {
            done();
        }
        return;
    }
    case State::Waiting:
        // Starts once the music and speech have gone quiet, or after kMaxWaitMs at most.
        if (!audio_ || audio_->quietForMovie() || nowMs - waitSince_ >= kMaxWaitMs) {
            start(nowMs);
        } else {
            return;
        }
        break;
    case State::Playing:
        break;
    }
    if (skipped_) {
        finish();
        return;
    }
    // Where the movie is: by its sound while it has some, then by the clock.
    double seconds = static_cast<double>(nowMs - startMs_) / 1000.0;
    if (hasSound_ && audio_) {
        const Audio::MovieSound s = audio_->movieSound();
        if (s.open && !s.ended) {
            seconds = static_cast<double>(s.heard) / soundRate_;
            soundAt_ = seconds;
            soundAtMs_ = nowMs;
        } else {
            // The sound gone (ended or closed): the clock runs on from where it was last heard.
            seconds = soundAt_ + static_cast<double>(nowMs - soundAtMs_) / 1000.0;
        }
    }
    // The frame due by now; every one up to it is decoded, only the last uploaded.
    const int due = static_cast<int>(std::floor(seconds * fps_ + 1e-9));
    bool fresh = false;
    if (decodedFrame_ < 0 && decodeNext()) {
        fresh = true; // (frame 0 at once)
    }
    while (decodedFrame_ < due && decodeNext()) {
        fresh = true;
    }
    if (fresh) {
        upload();
    }
    // The end (the loader's -2): every frame shown for its time.
    const bool allShown = decoderDone_ || (frames_ > 0 && decodedFrame_ + 1 >= frames_);
    const int count = decoderDone_ ? decodedFrame_ + 1 : frames_;
    if (allShown && seconds >= static_cast<double>(count) / fps_) {
        finish();
    }
}
void MoviePlayer::draw(int width, int height)
{
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer_);
    if (state_ != State::Playing || !texture_ || shownFrame_ < 0 || width_ <= 0 || height_ <= 0) {
        return;
    }
    // The exe's 640 x 480 screen, scaled to fit and centred as the menus are; the movie centred
    // in it. An HD movie takes the VQA's place: 640 wide, its own shape.
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    const float scale = std::min(w / 640.0f, h / 480.0f);
    const float ox = std::floor((w - 640 * scale) / 2);
    const float oy = std::floor((h - 480 * scale) / 2);
    float mw = static_cast<float>(width_);
    float mh = static_cast<float>(height_);
    // Wider than the exe's screen (or HD): fitted to its 640-pixel width, keeping the movie's shape.
    if (hd_ || mw > 640) {
        mh = 640.0f * mh / mw;
        mw = 640.0f;
    }
    const float mx = std::floor((640.0f - mw) / 2);
    const float my = std::floor((480.0f - mh) / 2);
    const SDL_FRect dst{ox + mx * scale, oy + my * scale, mw * scale, mh * scale};
    SDL_RenderTexture(renderer_, texture_, nullptr, &dst);
}

bool MoviePlayer::handleEvent(const SDL_Event& event)
{
    if (state_ == State::Idle) {
        return false;
    }
    if (state_ != State::Playing) {
        return true; // (the playback clears the stop flag as it starts)
    }
    std::optional<movies::Input> input;
    if (event.type == SDL_EVENT_KEY_DOWN) {
        // Windows' WM_KEYDOWN, repeats and all; F10, Alt and Alt + a key (without Ctrl) are
        // WM_SYSKEYDOWN instead, which the game's key handler doesn't look at.
        const SDL_Keycode k = event.key.key;
        const bool alt = k == SDLK_LALT || k == SDLK_RALT || (event.key.mod & SDL_KMOD_ALT) != 0;
        const bool system = k == SDLK_F10 || (alt && (event.key.mod & SDL_KMOD_CTRL) == 0);
        input = movies::keyInput(k == SDLK_ESCAPE, system);
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
        input = movies::Input::LeftButton;
    }
    if (input && movies::skips(*input, skip_)) {
        skipped_ = true;
    }
    return true;
}

} // namespace noxsdl
