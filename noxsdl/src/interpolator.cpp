// The Interpolator (interpolator.hpp): buffers the server's snapshots, fits a render clock to their
// arrivals, and answers where each drawable and the camera are drawn this render frame.
#include "interpolator.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace noxsdl {

namespace {

constexpr std::size_t kKeep = 64;        // snapshots buffered (16 frames' at 30 a second; 4 at 120)
constexpr double kWindowMs = 4000;       // arrivals the clock is fitted to

constexpr double kMaxDelay = 5.0;  // the most the render clock may trail the server, in frames
constexpr double kMargin = 0.15;         // frames over the measured lateness
constexpr double kStall = 6.0;           // frames late: a stall, not jitter (left out of the measure)
constexpr double kMaxSteer = 0.05;       // the render clock runs at most 5% fast or slow
constexpr double kSnapFrames = 4.0;      // further off than this the clock jumps
// Further than this between snapshots is a jump, not movement (running is ~8 units a frame).
constexpr float kSnapDistance = 64;
constexpr float kCameraSnapDistance = 200;

float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

bool farApart(float ax, float ay, float bx, float by, float limit)
{
    return std::abs(bx - ax) > limit || std::abs(by - ay) > limit;
}

} // namespace

void Interpolator::fitClock()
{
    // The server's rate: a least-squares line through the window's arrivals (the jitter averages
    // out over a few seconds); the nominal 30 a second until there's a second of them.
    const auto& first = arrivals_.front();
    const auto& last = arrivals_.back();
    if (last.ms - first.ms >= 1000 && arrivals_.size() >= 20) {
        // Least squares: the slope of server frame against arrival time.
        double mx = 0, mf = 0;
        for (const auto& a : arrivals_) {
            mx += a.ms - first.ms;
            mf += a.frame - first.frame;
        }
        mx /= static_cast<double>(arrivals_.size());
        mf /= static_cast<double>(arrivals_.size());
        double sxx = 0, sxf = 0;
        for (const auto& a : arrivals_) {
            const double dx = a.ms - first.ms - mx;
            sxx += dx * dx;
            sxf += dx * (a.frame - first.frame - mf);
        }
        if (sxx > 0) {
            framesPerMs_ = std::clamp(sxf / sxx, 25.0 / 1000, 35.0 / 1000);  // held to 25-35 frames a second
        }
    }
    // The earliest-arrival line through the newest arrival's time: the most any arrival is ahead
    // of the fitted rate. Each one's lateness is how far it fell behind that.
    leadMs_ = last.ms;
    lead_ = -1e18;
    for (const auto& a : arrivals_) {
        lead_ = std::max(lead_, a.frame + (last.ms - a.ms) * framesPerMs_);
    }
    std::vector<double> late;
    late.reserve(arrivals_.size());
    for (const auto& a : arrivals_) {
        const double l = lead_ - (a.frame + (last.ms - a.ms) * framesPerMs_);
        if (l < kStall) {
            late.push_back(l);
        }
    }
    double jitter = 0.5; // until there's enough to measure
    if (late.size() >= 20) {
        // The 95th percentile of the lateness.
        const auto k = static_cast<std::size_t>(static_cast<double>(late.size() - 1) * 0.95);
        std::nth_element(late.begin(), late.begin() + static_cast<std::ptrdiff_t>(k), late.end());
        jitter = late[k];
    }
    // (a blend needs the next snapshot in hand: one snapshot's interval at least)
    targetDelay_ = std::clamp(minDelay_ + jitter + kMargin, minDelay_, kMaxDelay);
}

void Interpolator::averageSelf(Snapshot& s)
{
    // A stock server's header puts us at an even place, so a run comes in steps of 6 and 8 units
    // (a 7-unit pace rounded): drawn as they come, the pace beats. With smoothing on, our place
    // and the camera on it are the mean of this snapshot's and the last one's (half a frame
    // later, an even pace), unless one is a jump or they aren't a frame or so apart.
    const auto self = s.positions.find(self_);
    const std::optional<Position> raw = self != s.positions.end() ? std::optional(self->second) : std::nullopt;
    const auto rawCamera = s.camera;
    const bool follows = rawFrame_ < s.serverFrame && s.serverFrame - rawFrame_ <= 3;
    if (selfRounded_ && (smoothing_ || scrolling_ || easing_) && follows) {
        if (raw && rawSelf_ && !farApart(raw->x, raw->y, rawSelf_->x, rawSelf_->y, kSnapDistance)) {
            self->second.x = (raw->x + rawSelf_->x) / 2;
            self->second.y = (raw->y + rawSelf_->y) / 2;
        }
        if (rawCamera && rawCamera_ && !farApart(rawCamera->first, rawCamera->second, rawCamera_->first, rawCamera_->second, kSnapDistance)) {
            s.camera = std::pair{(rawCamera->first + rawCamera_->first) / 2, (rawCamera->second + rawCamera_->second) / 2};
        }
    }
    if (s.serverFrame != rawFrame_) {
        rawSelf_ = raw;
        rawCamera_ = rawCamera;
        rawFrame_ = s.serverFrame;
    }
}

Interpolator::Position Interpolator::Ease::at(double nowMs) const
{
    const auto t = static_cast<float>(std::clamp((nowMs - startMs) / ms, 0.0, 1.0));
    return {lerp(from.x, to.x, t), lerp(from.y, to.y, t), lerp(from.z, to.z, t)};
}

void Interpolator::startEases(double nowMs)
{
    // Smooth Movement: the newest snapshot's positions, each from where it's drawn now over the
    // server frames since the last (one, ~33 ms at the fitted rate, as they come; at most three).
    const Snapshot& s = snapshots_.back();
    const double frames =
        easedFrame_ != 0 && s.serverFrame > easedFrame_ ? std::min(s.serverFrame - easedFrame_, 3.0) : 1.0;
    const double ms = frames / framesPerMs_;
    easedFrame_ = s.serverFrame;
    for (auto it = eases_.begin(); it != eases_.end();) {
        it = s.positions.contains(it->first) ? std::next(it) : eases_.erase(it);
    }
    for (const auto& [netCode, to] : s.positions) {
        const auto [it, fresh] = eases_.try_emplace(netCode);
        Ease& e = it->second;
        Position from = fresh ? to : e.at(nowMs);
        if (farApart(from.x, from.y, to.x, to.y, kSnapDistance)) {
            from = to; // (just seen, or a jump: there at once)
        }
        e = Ease{from, to, nowMs, ms};
    }
    if (!s.camera) {
        cameraEase_.reset();
        return;
    }
    const Position to{static_cast<float>(s.camera->first), static_cast<float>(s.camera->second), 0};
    Position from = cameraEase_ ? cameraEase_->at(nowMs) : to;
    if (farApart(from.x, from.y, to.x, to.y, kCameraSnapDistance)) {
        from = to;
    }
    cameraEase_ = Ease{from, to, nowMs, ms};
}

void Interpolator::update(const nox::client::HeadlessClient& client, double nowMs)
{
    predictor_.update(client, nowMs);
    self_ = client.netCode();
    easeNowMs_ = nowMs;
    if (client.snapshotCount() != seen_) {
        seen_ = client.snapshotCount();
        Snapshot s;
        s.serverFrame = client.frameTime();
        minDelay_ = 1.0 / std::max(1, client.substepsPerFrame());  // one snapshot's interval, in frames
        if (const auto c = client.position()) {
            s.camera = std::pair{static_cast<float>(c->first), static_cast<float>(c->second)};
        }
        for (const auto& [netCode, d] : client.world().drawables()) {
            s.positions[netCode] = {static_cast<float>(d.x), static_cast<float>(d.y), static_cast<float>(d.z)};
        }
        averageSelf(s);
        // Server frames only go forward; a stale or repeated stamp replaces the newest entry,
        // and a big step back (a new game) starts the clock over.
        if (!snapshots_.empty() && snapshots_.back().serverFrame > s.serverFrame + 30) {
            snapshots_.clear();
            arrivals_.clear();
            clockSet_ = false;
        }
        while (!snapshots_.empty() && snapshots_.back().serverFrame >= s.serverFrame) {
            snapshots_.pop_back();
        }
        snapshots_.push_back(std::move(s));
        while (snapshots_.size() > kKeep) {
            snapshots_.pop_front();
        }
        updateVelocities();

        // A jump in the server's frames far from what the clock expects (a stall, a new game)
        // starts the fit over; otherwise this arrival joins the window.
        const double frame = snapshots_.back().serverFrame;
        if (!arrivals_.empty() && std::abs(frame - serverNow(nowMs)) > 30) {
            arrivals_.clear();
        }
        if (!arrivals_.empty() && arrivals_.back().frame >= frame) {
            arrivals_.pop_back();
        }
        arrivals_.push_back({nowMs, frame});
        while (arrivals_.size() > 2 && arrivals_.back().ms - arrivals_.front().ms > kWindowMs) {
            arrivals_.pop_front();
        }
        fitClock();
        if (easing_) {
            startEases(nowMs);
        }
    }
    from_ = to_ = nullptr;
    if (snapshots_.empty() || arrivals_.empty()) {
        return;
    }

    const double newest = snapshots_.back().serverFrame;
    const double oldest = snapshots_.front().serverFrame;
    nowFrame_ = serverNow(nowMs);
    // Ahead: from the frame shown to the one our input now reaches the server in.
    aheadFrames_ = std::clamp(nowFrame_ - renderFrame_, 0.0, 6.0) + predictor_.learnedLag();
    newest_ = newest;
    to_ = &snapshots_.back();
    from_ = to_;
    t_ = 1;
    if (!smoothing_ && !scrolling_) {
        renderFrame_ = newest; // the newest as it is, no delay
        clockSet_ = false;
        return;
    }

    // The render clock: runs at the server's rate, steered toward the wanted delay.
    const double target = nowFrame_ - targetDelay_;
    if (!clockSet_ || std::abs(target - renderFrame_) > kSnapFrames) {
        renderFrame_ = target;
        clockSet_ = true;
    } else {
        // A frame's worth of the server's time, then up to 5% of it more or less toward the target.
        const double advance = std::max(0.0, nowMs - lastMs_) * framesPerMs_;
        renderFrame_ += advance;
        renderFrame_ += std::clamp(target - renderFrame_, -kMaxSteer * advance, kMaxSteer * advance);
    }
    lastMs_ = nowMs;
    // Past the newest we hold it (no extrapolation), and the clock waits there; before the
    // oldest we show the oldest.
    if (renderFrame_ > newest) {
        renderFrame_ = newest;
        ++holds_;
    }
    const double shown = std::max(renderFrame_, oldest);
    // The pair of snapshots around the frame shown, and how far between them it is.
    for (std::size_t i = 1; i < snapshots_.size(); ++i) {
        if (snapshots_[i].serverFrame >= shown) {
            from_ = &snapshots_[i - 1];
            to_ = &snapshots_[i];
            const double span = static_cast<double>(to_->serverFrame) - from_->serverFrame;
            t_ = static_cast<float>(std::clamp((shown - from_->serverFrame) / span, 0.0, 1.0));
            break;
        }
    }
}

double Interpolator::delayMs() const
{
    return (smoothing_ || scrolling_) && clockSet_ ? (nowFrame_ - renderFrame_) / framesPerMs_ : 0;
}

double Interpolator::targetDelayMs() const
{
    return smoothing_ || scrolling_ ? targetDelay_ / framesPerMs_ : 0;
}

Interpolator::Position Interpolator::position(std::uint16_t netCode, const nox::client::Drawable& d) const
{
    const Position latest{static_cast<float>(d.x), static_cast<float>(d.y), static_cast<float>(d.z)};
    if (predicting() && netCode == predictor_.netCode()) {
        const SDL_FPoint p = predictor_.position();
        return {p.x, p.y, latest.z};
    }
    if (easing_) {
        // Smooth Movement: on its way to the newest position; we as the camera is (it's on us).
        if (netCode == self_ && cameraEase_ && std::abs(cameraEase_->to.x - static_cast<float>(d.x)) <= 4 &&
            std::abs(cameraEase_->to.y - static_cast<float>(d.y)) <= 4) {
            const Position c = cameraEase_->at(easeNowMs_);
            return {c.x, c.y, latest.z};
        }
        const auto it = eases_.find(netCode);
        return it != eases_.end() ? it->second.at(easeNowMs_) : latest;
    }
    if (!from_ || !to_) {
        return latest;
    }
    // We're where the camera is (it follows us), smooth or not as the scrolling is.
    if (netCode == self_) {
        if (const auto& cam = snapshots_.back().camera;
            cam && std::abs(cam->first - d.x) <= 4 && std::abs(cam->second - d.y) <= 4) {
            if (const auto c = camera()) {
                return {c->x, c->y, latest.z};
            }
        }
    }
    if (!smoothing_) {
        return latest; // as the server last said (Smooth Motion off)
    }
    const bool other = netCode != self_;
    if (other && others_ == Others::Newest) {
        return latest;
    }
    // Between the two snapshots around the frame shown.
    const auto b = to_->positions.find(netCode);
    const auto a = from_->positions.find(netCode);
    if (b == to_->positions.end() || a == from_->positions.end()) {
        return latest;
    }
    if (farApart(a->second.x, a->second.y, b->second.x, b->second.y, kSnapDistance)) {
        return b->second;
    }
    Position p{lerp(a->second.x, b->second.x, t_), lerp(a->second.y, b->second.y, t_), lerp(a->second.z, b->second.z, t_)};
    // Ahead: pushed on along its velocity by the frames we're behind.
    if (other && others_ == Others::Ahead && (!aheadFilter_ || aheadFilter_(d))) {
        if (const auto v = velocity_.find(netCode); v != velocity_.end()) {
            const auto ahead = static_cast<float>(aheadFrames_);
            p.x += v->second.x * ahead;
            p.y += v->second.y * ahead;
        }
    }
    return p;
}

void Interpolator::updateVelocities()
{
    // Over the last 3 frames (positions come in steps of 2 units), eased so a step in the
    // estimate doesn't jump what's drawn by it times the frames ahead.
    if (snapshots_.size() < 4) {
        velocity_.clear();
        return;
    }
    const Snapshot& b = snapshots_.back();
    const Snapshot& a = snapshots_[snapshots_.size() - 4];
    const auto frames = static_cast<float>(b.serverFrame - a.serverFrame);
    for (auto it = velocity_.begin(); it != velocity_.end();) {
        it = b.positions.contains(it->first) ? std::next(it) : velocity_.erase(it);
    }
    for (const auto& [netCode, q] : b.positions) {
        const auto p = a.positions.find(netCode);
        Position v;
        if (p != a.positions.end() && frames > 0 && !farApart(p->second.x, p->second.y, q.x, q.y, kSnapDistance * 3)) {
            v = {(q.x - p->second.x) / frames, (q.y - p->second.y) / frames, 0};
        }
        auto& e = velocity_[netCode];
        // Eased halfway toward the new estimate each snapshot.
        e.x += (v.x - e.x) * 0.5f;
        e.y += (v.y - e.y) * 0.5f;
    }
}

std::optional<SDL_FPoint> Interpolator::camera() const
{
    if (predicting()) {
        return predictor_.position(); // the camera is on us (MovePredictor checks)
    }
    if (easing_) {
        if (!cameraEase_) {
            return std::nullopt;
        }
        const Position c = cameraEase_->at(easeNowMs_);
        return SDL_FPoint{c.x, c.y};
    }
    if (!from_ || !to_ || !to_->camera) {
        return std::nullopt;
    }
    if (!scrolling_) {
        const auto& c = snapshots_.back().camera; // the newest, in the server's steps
        if (!c) {
            return std::nullopt;
        }
        return SDL_FPoint{static_cast<float>(c->first), static_cast<float>(c->second)};
    }
    const float bx = static_cast<float>(to_->camera->first);
    const float by = static_cast<float>(to_->camera->second);
    if (!from_->camera) {
        return SDL_FPoint{bx, by};
    }
    const float ax = static_cast<float>(from_->camera->first);
    const float ay = static_cast<float>(from_->camera->second);
    if (farApart(ax, ay, bx, by, kCameraSnapDistance)) {
        return SDL_FPoint{bx, by};
    }
    return SDL_FPoint{lerp(ax, bx, t_), lerp(ay, by, t_)};
}

} // namespace noxsdl