// The HD movies (noxsdl's, not the exe's): HDVideo\<NAME>.mkv beside noxcore.exe, AV1 10-bit (or
// HEVC Main 10) at 1920 x 1200 and 15 fps, frame for frame with the VQA, with FLAC sound at
// 22050 Hz. Decoded by FFmpeg's LGPL shared libraries (avformat, avcodec, avutil, swscale,
// swresample), which ship in HDVideo\ with the files and are loaded from there only when an HD
// movie is about to play (Windows: linked /DELAYLOAD, each DLL loaded by its full path first and
// its imports checked, so a missing or wrong DLL means the VQA plays instead, never a crash).
// Built without NOXSDL_HD_VIDEO (other platforms), available() is false and the VQAs play.
#pragma once

#include "noxcore/audio/pcm.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace noxsdl {

class HdVideo {
public:
    /// Whether FFmpeg's DLLs load from `dllDir` (tried once; the answer is kept).
    static bool available(const std::filesystem::path& dllDir);
    /// Opens a movie; nullptr (and `error`) if it can't be played.
    static std::unique_ptr<HdVideo> open(const std::filesystem::path& file, std::string& error);
    ~HdVideo();
    HdVideo(const HdVideo&) = delete;
    HdVideo& operator=(const HdVideo&) = delete;

    int width() const { return width_; }
    int height() const { return height_; }
    double fps() const { return fps_; }
    /// The number of frames, as far as the file says (0 if it doesn't).
    int frameCount() const { return frameCount_; }
    /// The sound, decoded whole at open (interleaved 16-bit; empty for none).
    nox::audio::Pcm takeSound() { return std::move(sound_); }
    /// Decodes the next frame into `rgba` (width x height x 4); false at the end.
    bool nextFrame(std::vector<std::uint8_t>& rgba);
    /// The frame last decoded (-1 before the first).
    int frame() const { return frame_; }

private:
    HdVideo() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int width_ = 0;
    int height_ = 0;
    double fps_ = 15;
    int frameCount_ = 0;
    int frame_ = -1;
    nox::audio::Pcm sound_;
};

} // namespace noxsdl
