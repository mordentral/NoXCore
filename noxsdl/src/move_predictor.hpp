// Predicting our own movement (--predict-movement, an experiment: taken out of Options, it sped up
// and slowed down while running; with Smoothing = Smooth Network): our player and
// the camera move as soon as the input goes out, instead of a round trip later when the
// server's snapshots show it.
//
// The server moves a player (Plr_UpdateState, updateObjectsVelocity):
// each frame its MoveForward control pushes the player along the facing its Orientation control
// set (no turning rate), at twice the walking push when running; running starts when the cursor
// (MSG_MOUSE) is over 100 units off and lasts until the player stops. The push and the friction
// bring it to full speed in a few frames and to a stop in a few; walls and other objects stop
// or deflect it. Measured on a live server: running is 8 units a frame, a start or a stop takes
// about four frames (~2 units a frame each frame), and input shows in the snapshots ~7 frames
// after it's sent.
//
// Here: each game frame's input (HeadlessClient::lastInput) steps a local copy of us: velocity
// toward the input's speed and direction by at most kAccel a frame, stopped or slid along by the
// map's wall cells. The input-to-snapshot lag isn't known up front (ping plus the server's frame
// alignment), so it's learnt from our starts and stops: the frames from the input that started
// (or stopped) us to the snapshot that first shows it, the median of the last few. On each
// snapshot we restart
// from the server's position that many frames back and replay the inputs since; the difference
// that makes to where we're drawn is blended out over ~100 ms, so a correction never jumps (a
// big one, a teleport, does). The running speed is learnt from the snapshots too (haste,
// slowness, weight). Not modelled: other objects in the way (when the server shows us held
// while we'd be moving, the replay keeps to the server's speed until we turn or it frees),
// pushes, confusion and charges (corrected as they come).
#pragma once

#include "noxcore/client/headless_client.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <deque>
#include <optional>

namespace noxsdl {

class MovePredictor {
public:
    /// Tracking us (and learning the lag and our speed), whether or not what it predicts is drawn.
    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    /// Call every render frame after the client's tick, with the render clock in ms.
    void update(const nox::client::HeadlessClient& client, double nowMs);
    /// Whether our player is being predicted now (enabled, alive, playing, the camera on us).
    bool active() const { return active_; }
    std::uint16_t netCode() const { return netCode_; }
    /// Where to draw us, and the camera (on us).
    SDL_FPoint position() const { return {display_.x, display_.y}; }
    /// The learnt input-to-snapshot lag, in frames (0 while not active), and as it stands.
    int lagFrames() const { return active_ ? lag_ : 0; }
    int learnedLag() const { return lag_; }

private:
    struct Vec {
        float x = 0, y = 0;
    };
    struct Step {
        std::uint32_t frame = 0; ///< the input frame (HeadlessClient::inputFrames)
        nox::client::HeadlessClient::SentInput input;
        Vec pos;                 ///< after this frame's input
        Vec vel;
        bool running = false;
    };
    static constexpr int kMaxLag = 24;

    void reset();
    Step simulate(const Step& from, const nox::client::HeadlessClient::SentInput& input, std::uint32_t frame,
                  float speedCap) const;
    bool blocked(float x, float y) const;
    void reconcile(Vec server, std::uint32_t serverFrame);
    void learnLag(int frames);
    const Step* stepAt(std::uint32_t frame) const;

    bool enabled_ = false;
    bool active_ = false;
    std::uint16_t netCode_ = 0;
    const nox::client::NavGrid* nav_ = nullptr;
    std::deque<Step> steps_;     ///< the last kMaxLag + 8 frames, oldest first
    std::uint32_t seenInputs_ = 0;
    std::uint32_t seenSnapshots_ = 0;
    std::deque<int> lagSamples_; ///< the last few measured lags, frames
    int lag_ = 6;
    std::optional<std::uint32_t> startedAt_; ///< the input frame that started us, until the server shows it
    std::optional<std::uint32_t> stoppedAt_; ///< the one that stopped us, likewise
    float stopSpeed_ = 0;        ///< the server's speed when we stopped
    float runSpeed_ = 8;         ///< units a frame running (walking is half)
    struct Seen {
        std::uint32_t frame = 0; ///< the server's
        Vec pos;
    };
    std::deque<Seen> serverTrail_; ///< our last few snapshot positions, newest last
    float serverSpeed_ = 0;      ///< units a frame between the last two
    int heldSnapshots_ = 0;      ///< snapshots in a row showing us held while we'd move
    std::optional<float> held_;  ///< the server's speed while held: the replay keeps to it
    Vec offset_;                 ///< the last corrections still being blended out
    Vec display_;
    double stepMs_ = 0;          ///< when the newest step was taken
    double frameMs_ = 1000.0 / 30;
    double lastMs_ = 0;
};

} // namespace noxsdl
