// Smooth motion between the server's world snapshots.
//
// The game runs at 30 frames a second, so positions (and the camera, which is our player) change
// in steps ~33 ms apart, and the datagrams carrying them arrive unevenly. The renderer runs at the
// display's rate and draws the world as it was at a render time a little behind the newest
// snapshot, blending between the two buffered snapshots around it.
//
// Time is measured in server frames, not arrival times: each snapshot is stamped with its server
// frame, which advance evenly however the packets bunch. The delay adapts to the connection, as
// a VoIP jitter buffer does:
//   - the last few seconds of arrivals are fitted to the server's clock: its rate (a server runs
//     a little off 30 a second) and the earliest-arrival line, the time a frame shows up when
//     nothing delays it;
//   - each snapshot's lateness is how far behind that line it came; the delay is one frame (a
//     blend needs the snapshot after the render time in hand) plus the lateness 95% of recent
//     snapshots stay under, plus a small margin;
//   - the render clock runs at the server's rate, sped up or slowed by at most 5% to reach the
//     wanted delay, so a change of delay never shows as a jump.
// On a steady connection that's about a frame (~35 ms) behind the newest; a jittery one gets
// more, and after a stall the clock jumps ahead rather than crawling. Objects that just
// appeared, or jumped further than anything walks in one frame (teleports, respawns), snap to
// their new place.
//
// Options' Smoothing:
//   Off               the newest snapshot as it comes (as vanilla draws: in 30 fps steps);
//   Lock to 30 FPS    the same, with frames drawn only as snapshots come (main.cpp);
//   Smooth Movement   no clock and no buffering: when a snapshot brings a new position, the
//                     camera and every drawable move from where they're drawn to it over one
//                     server frame (~33 ms, by the fitted rate, at whatever the frame rate), then
//                     hold there till the next: each 30 fps step spread over the frames between,
//                     about half a server frame behind on average, never past where the server
//                     put it (a late snapshot holds, a teleport snaps);
//   Smooth Network    (the default) everything drawn on the clock above, between snapshots.
// With
// --predict-movement (an experiment) our own player and the camera come from the MovePredictor
// instead: ahead of the server, not behind it.
//
// Everything else (--other-units, an experiment; with smoothing):
//   Smooth   as above.
//   Newest   the newest snapshot as it is (as vanilla): no smoothing delay, stepping at 30 fps.
//   Ahead    players and monsters where they'll be when our input reaches the server: their
//            smooth position moved on by their velocity (over the last 3 snapshots, eased) for
//            the smoothing delay plus the input-to-snapshot lag (MovePredictor learns it while
//            smoothing is on). What we aim at is then where it'll be; one that turns or stops is
//            drawn past it for a moment.
#pragma once

#include "move_predictor.hpp"

#include "noxcore/client/headless_client.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <unordered_map>

namespace noxsdl {

class Interpolator {
public:
    struct Position {
        float x = 0;
        float y = 0;
        float z = 0;
    };

    /// Call every render frame after pumping the client, with the render clock in ms.
    void update(const nox::client::HeadlessClient& client, double nowMs);
    /// Smoothing = Smooth Network: everything but us drawn behind and blended, as above (Other Units
    /// says how). Off: the newest snapshot as it is, in whole server frames, as vanilla draws.
    void setSmoothing(bool on)
    {
        smoothing_ = on;
        predictor_.setEnabled(smoothing_); // (it learns the lag Ahead needs, predicting or not)
    }
    /// Smoothing = Smooth Movement or Network: the camera (and we, at its centre) blended on the same clock.
    /// Off: in the server's steps, as vanilla scrolls.
    void setScrolling(bool on) { scrolling_ = on; }
    bool scrolling() const { return scrolling_; }
    /// Smoothing = Smooth Movement: the camera and everything eased to each new position over a
    /// server frame (above); neither smoothing nor scrolling then.
    void setEasing(bool on) { easing_ = on; }
    bool easing() const { return easing_; }
    /// --predict-movement (an experiment, not in Options: it sped up and slowed down while
    /// running): our own movement predicted (MovePredictor); only with smoothing.
    void setPrediction(bool on) { predict_ = on; }
    /// The server rounds our own place to even units (a stock server: the update stream's
    /// header): with smoothing, each snapshot's place for us (and the camera on us) is the mean
    /// of it and the one before, so a run's 6 / 8 unit steps are drawn as 7 / 7. A noxcpp server
    /// sends us our whole place: off there (no added half frame).
    void setSelfRounded(bool on) { selfRounded_ = on; }
    bool predicting() const { return predict_ && smoothing_ && predictor_.active(); }
    /// Options' Other Units (see above); only with smoothing.
    enum class Others : std::uint8_t { Smooth, Newest, Ahead };
    void setOthers(Others others) { others_ = others; }
    /// Which drawables Ahead moves on (players and monsters; set by the caller, which knows the
    /// things' classes).
    void setAheadFilter(std::function<bool(const nox::client::Drawable&)> filter) { aheadFilter_ = std::move(filter); }
    /// How many frames Ahead moves them on (0 when it's not in use).
    double aheadFrames() const { return others_ == Others::Ahead && smoothing_ ? aheadFrames_ : 0; }
    const MovePredictor& predictor() const { return predictor_; }
    bool smoothing() const { return smoothing_; }

    /// Where to draw a drawable (its latest position if it isn't in the bracketing snapshots).
    Position position(std::uint16_t netCode, const nox::client::Drawable& d) const;
    /// The camera centre (our position from the update stream), interpolated.
    std::optional<SDL_FPoint> camera() const;
    /// The server frame being shown, with a fraction, for animation clocks.
    double gameTime() const { return smoothing_ ? renderFrame_ : newest_; }
    /// How far the shown frame is behind the server's current one (estimated), in ms, and the
    /// delay the clock is steering for; 0 with smoothing off.
    double delayMs() const;
    double targetDelayMs() const;
    /// Render frames that wanted a snapshot that hadn't come yet (the picture held): stutters.
    std::uint32_t holds() const { return holds_; }

private:
    struct Snapshot {
        double serverFrame = 0; ///< (a noxcpp server's substep updates: with a fraction)
        std::optional<std::pair<float, float>> camera; ///< (halves: our place averaged, setSelfRounded)
        std::unordered_map<std::uint16_t, Position> positions;
    };
    struct Arrival {
        double ms = 0;      ///< local time it was seen
        double frame = 0;   ///< its server frame (unwrapped)
    };
    /// Smooth Movement's: from where it was drawn as the newest position came to that position.
    struct Ease {
        Position from;
        Position to;
        double startMs = 0;
        double ms = 1; ///< how long the move takes
        Position at(double nowMs) const;
    };

    void fitClock();
    void updateVelocities();
    void startEases(double nowMs);
    double serverNow(double nowMs) const { return lead_ + (nowMs - leadMs_) * framesPerMs_; }

    std::uint32_t seen_ = 0;
    std::deque<Snapshot> snapshots_; ///< oldest first
    std::deque<Arrival> arrivals_;   ///< the last few seconds' (fitClock)
    double framesPerMs_ = 30.0 / 1000; ///< the server's frame rate, fitted
    double lead_ = 0;      ///< the earliest-arrival line's server frame at leadMs_
    double leadMs_ = 0;
    double targetDelay_ = 1.5; ///< frames
    double minDelay_ = 1.0;    ///< frames: a snapshot's interval (a frame; less with a noxcpp server's substeps)
    bool clockSet_ = false;
    double lastMs_ = 0;
    double nowFrame_ = 0;      ///< serverNow() at the last update
    double renderFrame_ = 0;
    const Snapshot* from_ = nullptr; ///< the snapshots around renderFrame_
    const Snapshot* to_ = nullptr;
    float t_ = 1; ///< 0 at from_, 1 at to_
    bool smoothing_ = true;
    bool scrolling_ = true;
    bool easing_ = false;
    std::unordered_map<std::uint16_t, Ease> eases_; ///< by net code
    std::optional<Ease> cameraEase_;
    double easeNowMs_ = 0;     ///< the render clock at the last update
    double easedFrame_ = 0; ///< the newest snapshot's server frame the eases head for
    double newest_ = 0; ///< the newest snapshot's frame
    bool predict_ = false;
    Others others_ = Others::Smooth;
    std::function<bool(const nox::client::Drawable&)> aheadFilter_;
    std::unordered_map<std::uint16_t, Position> velocity_; ///< units a frame, eased (Ahead)
    double aheadFrames_ = 0;
    std::uint16_t self_ = 0;
    bool selfRounded_ = false;
    std::optional<Position> rawSelf_;                     ///< our place as the last snapshot had it, unaveraged
    std::optional<std::pair<float, float>> rawCamera_;
    double rawFrame_ = 0;
    void averageSelf(Snapshot& s);
    MovePredictor predictor_;
    std::uint32_t holds_ = 0;
};

} // namespace noxsdl