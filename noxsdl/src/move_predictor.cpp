// Predicting our own movement (move_predictor.hpp explains the model): stepping a local copy of us
// per input frame, and reconciling it with each snapshot.
#include "move_predictor.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace noxsdl {

namespace {

constexpr float kAccel = 2.0f;       // units a frame, each frame: speeding up, slowing, turning
constexpr float kRunCursor = 100.0f; // the cursor further than this starts a run (= 100^2)
constexpr double kBlendMs = 60;      // a correction's time constant (~95% gone in 180 ms)
constexpr float kSnap = 64;          // a correction further than this jumps (a teleport, a respawn)
constexpr std::size_t kKeep = 32;    // steps kept (more than the longest lag)
constexpr std::size_t kLagSamples = 7;
constexpr double kPi = 3.14159265358979323846;
// Enchantments that keep a player from moving as asked (updateObjectsVelocity
// skips the push for 5 held, 25 frozen and 28 charming; 3 confused steers at random).
constexpr std::uint32_t kStill = (1u << 3) | (1u << 5) | (1u << 25) | (1u << 28);

float length(float x, float y)
{
    return std::sqrt(x * x + y * y);
}

} // namespace

void MovePredictor::reset()
{
    active_ = false;
    steps_.clear();
    serverTrail_.clear();
    startedAt_.reset();
    stoppedAt_.reset();
    heldSnapshots_ = 0;
    held_.reset();
    offset_ = {};
}

// Whether world (x, y) is in a wall cell of the client's nav grid (no grid: nothing is).
bool MovePredictor::blocked(float x, float y) const
{
    if (!nav_ || nav_->empty()) {
        return false;
    }
    const int cx = static_cast<int>(std::floor(x / nox::client::NavGrid::kCellSize));
    const int cy = static_cast<int>(std::floor(y / nox::client::NavGrid::kCellSize));
    return nav_->blocked(cx, cy);
}

// One frame of our movement from `from` under `input`: the velocity moves toward the input's by at
// most kAccel, its speed capped by `speedCap` (the server's while we're held).
MovePredictor::Step MovePredictor::simulate(const Step& from, const nox::client::HeadlessClient::SentInput& input,
                                            std::uint32_t frame, float speedCap) const
{
    Step s;
    s.frame = frame;
    s.input = input;
    // Walking or running, as Plr_UpdateState decides it: running once the cursor is over 100
    // units off, and on until we stop.
    bool far = false;
    if (input.cursor) {
        far = length(static_cast<float>(input.cursor->first) - from.pos.x,
                     static_cast<float>(input.cursor->second) - from.pos.y) > kRunCursor;
    }
    s.running = input.move && (from.running || far);
    Vec target;
    if (input.move && !input.attack) {  // no push while attacking
        const float speed = std::min(s.running ? runSpeed_ : runSpeed_ / 2, speedCap);
        const double a = input.facing * 2 * kPi / 256; // 0 east, 64 south
        target = {static_cast<float>(std::cos(a)) * speed, static_cast<float>(std::sin(a)) * speed};
    }
    float dx = target.x - from.vel.x;
    float dy = target.y - from.vel.y;
    if (const float d = length(dx, dy); d > kAccel) {
        dx *= kAccel / d;
        dy *= kAccel / d;
    }
    s.vel = {from.vel.x + dx, from.vel.y + dy};
    s.pos = {from.pos.x + s.vel.x, from.pos.y + s.vel.y};
    // Into a wall's cell from outside one: slide along it if one axis is free, else stop.
    if (blocked(s.pos.x, s.pos.y) && !blocked(from.pos.x, from.pos.y)) {
        if (!blocked(s.pos.x, from.pos.y)) {
            s.pos.y = from.pos.y;
            s.vel.y = 0;
        } else if (!blocked(from.pos.x, s.pos.y)) {
            s.pos.x = from.pos.x;
            s.vel.x = 0;
        } else {
            s.pos = from.pos;
            s.vel = {};
        }
    }
    return s;
}

// The step taken at input frame `frame`; nullptr if it isn't kept.
const MovePredictor::Step* MovePredictor::stepAt(std::uint32_t frame) const
{
    if (steps_.empty() || frame < steps_.front().frame || frame > steps_.back().frame) {
        return nullptr;
    }
    return &steps_[frame - steps_.front().frame];
}

// A measured lag joins the last kLagSamples; the lag used is their median.
void MovePredictor::learnLag(int frames)
{
    if (frames < 1 || frames > kMaxLag) {
        return;
    }
    lagSamples_.push_back(frames);
    while (lagSamples_.size() > kLagSamples) {
        lagSamples_.pop_front();
    }
    std::vector<int> sorted(lagSamples_.begin(), lagSamples_.end());
    std::ranges::sort(sorted);
    lag_ = sorted[sorted.size() / 2];
}

// A new snapshot: the server's position and speed noted; the lag, the held state and the running
// speed learnt from it; then the steps replayed from it.
void MovePredictor::reconcile(Vec server, std::uint32_t serverFrame)
{
    if (!serverTrail_.empty() && serverFrame <= serverTrail_.back().frame) {
        serverTrail_.back().pos = server; // a repeated stamp
    } else {
        serverTrail_.push_back({serverFrame, server});
    }
    while (serverTrail_.size() > 6) {  // the last 6 snapshots
        serverTrail_.pop_front();
    }
    serverSpeed_ = 0;
    if (serverTrail_.size() >= 2) {
        const auto& a = serverTrail_[serverTrail_.size() - 2];
        const auto& b = serverTrail_.back();
        serverSpeed_ = length(b.pos.x - a.pos.x, b.pos.y - a.pos.y) / static_cast<float>(std::max(1u, b.frame - a.frame));
    }
    const std::uint32_t now = steps_.back().frame;  // the newest input frame

    // The lag: from the input that started or stopped us to the snapshot that first shows it.
    if (startedAt_ && serverSpeed_ > 0.5f) {
        learnLag(static_cast<int>(now - *startedAt_));
        startedAt_.reset();
    }
    if (stoppedAt_ && serverSpeed_ < stopSpeed_ * 0.75f) {
        learnLag(static_cast<int>(now - *stoppedAt_));
        stoppedAt_.reset();
    }
    if (startedAt_ && now - *startedAt_ > kMaxLag) {
        startedAt_.reset(); // the server never moved us (blocked, or not allowed)
    }
    if (stoppedAt_ && now - *stoppedAt_ > kMaxLag) {
        stoppedAt_.reset();
    }

    // The step this snapshot answers: the lag back from the newest input.
    const std::uint32_t baseFrame = now >= static_cast<std::uint32_t>(lag_) ? now - static_cast<std::uint32_t>(lag_) : 0;
    // (deque elements aren't contiguous: index by frame, not by pointer)
    const std::size_t baseIndex =
        baseFrame <= steps_.front().frame ? 0 : static_cast<std::size_t>(baseFrame - steps_.front().frame);
    const Step* base = &steps_[baseIndex];

    // Held: the server shows us (nearly) still for a while though we'd been asking to move the
    // same way for a few frames (something's in the way). The replay then keeps to its speed.
    const Step* earlier = stepAt(base->frame >= 4 ? base->frame - 4 : 0);
    const float expected = base->running ? runSpeed_ : runSpeed_ / 2;
    // Steady: moving the same way (facings, 256 a turn, within 8) as 4 frames before, not attacking.
    const bool steady = base->input.move && !base->input.attack && earlier && earlier->input.move &&
                        std::abs(static_cast<int>(static_cast<std::int8_t>(base->input.facing - earlier->input.facing))) < 8;
    if (steady && serverSpeed_ < expected * 0.3f) {
        if (++heldSnapshots_ >= 3) {  // three in a row: held to the server's speed
            held_ = serverSpeed_;
        }
    } else {
        heldSnapshots_ = 0;
        if (!base->input.move || serverSpeed_ >= expected * 0.6f || !steady) {
            held_.reset();
        }
    }

    // The running speed, from a steady run: four snapshots of it in a straight line at an even
    // speed (not slowed by a wall or a crowd).
    if (steady && base->running && !held_ && serverTrail_.size() >= 5) {
        const auto& a = serverTrail_[serverTrail_.size() - 5];
        const auto& b = serverTrail_.back();
        const float s = length(b.pos.x - a.pos.x, b.pos.y - a.pos.y) / static_cast<float>(std::max(1u, b.frame - a.frame));
        float path = 0, slowest = 1e9f, fastest = 0;
        for (std::size_t i = serverTrail_.size() - 4; i < serverTrail_.size(); ++i) {
            const auto& p = serverTrail_[i - 1];
            const auto& q = serverTrail_[i];
            const float step = length(q.pos.x - p.pos.x, q.pos.y - p.pos.y) / static_cast<float>(std::max(1u, q.frame - p.frame));
            path += step;
            slowest = std::min(slowest, step);
            fastest = std::max(fastest, step);
        }
        // (positions come in steps of 2 units: a frame's speed is +-2 off)
        const bool straight = path > 0 && s * 4 > path * 0.9f && fastest - slowest <= 4.5f;
        const Step* runStart = stepAt(base->frame >= 8 ? base->frame - 8 : 0);
        // Learnt only well into a run (running 8 frames back) and near the current guess, a tenth of
        // the way each time.
        if (s > 2 && straight && runStart && runStart->running && std::abs(s - runSpeed_) < runSpeed_ * 0.6f) {
            runSpeed_ += (std::clamp(s, 2.0f, 24.0f) - runSpeed_) * 0.1f;
        }
    }

    // Restart from the server's position at the base frame and replay the inputs since.
    const Vec before = steps_.back().pos;
    Step& b = steps_[baseIndex];
    b.pos = server;
    if (held_ && serverTrail_.size() >= 2) {
        const auto& p = serverTrail_[serverTrail_.size() - 2];
        const float frames = static_cast<float>(std::max(1u, serverTrail_.back().frame - p.frame));
        b.vel = {(server.x - p.pos.x) / frames, (server.y - p.pos.y) / frames};
    }
    const float cap = held_ ? *held_ + 0.5f : 1e9f;  // held: no faster than the server shows us
    for (std::size_t i = baseIndex + 1; i < steps_.size(); ++i) {
        steps_[i] = simulate(steps_[i - 1], steps_[i].input, steps_[i].frame, cap);
    }
    const Vec after = steps_.back().pos;
    // How far the replay moved where we'd be: blended out of the drawn position, unless it's a jump.
    const float dx = before.x - after.x;
    const float dy = before.y - after.y;
    if (std::abs(dx) > kSnap || std::abs(dy) > kSnap) {
        offset_ = {};
    } else {
        offset_.x += dx;
        offset_.y += dy;
    }
}

void MovePredictor::update(const nox::client::HeadlessClient& client, double nowMs)
{
    using Stage = nox::client::HeadlessClient::Stage;
    netCode_ = client.netCode();
    const auto& world = client.world();
    const auto* self = world.drawable(netCode_);
    const auto me = world.players().find(netCode_);
    const auto camera = client.position();
    // Only us, alive and playing, with the camera on us and nothing holding us.
    // (status bit 0x1: observing; the camera within 4 units of our drawable; kStill: held in place.)
    const bool can = enabled_ && client.stage() == Stage::InGame && self && camera && me != world.players().end() &&
                     !me->second.dead && !(me->second.status & 1) && std::abs(camera->first - self->x) <= 4 &&
                     std::abs(camera->second - self->y) <= 4 && !(self->buffs & kStill);
    if (!can) {
        if (active_) {
            reset();
        }
        lastMs_ = nowMs;
        return;
    }
    const Vec server{static_cast<float>(camera->first), static_cast<float>(camera->second)};
    nav_ = &client.nav();
    // The first frame: start from where the server has us.
    if (!active_) {
        active_ = true;
        Step s;
        s.frame = client.inputFrames();
        s.input = client.lastInput();
        s.pos = server;
        steps_.assign(1, s);
        seenInputs_ = client.inputFrames();
        seenSnapshots_ = client.snapshotCount();
        stepMs_ = nowMs;
        lastMs_ = nowMs;
        display_ = {server.x, server.y};
        return;
    }

    // This frame's input (more than one if the render stalled: the last again).
    while (seenInputs_ != client.inputFrames()) {
        ++seenInputs_;
        const Step& last = steps_.back();
        const auto& input = client.lastInput();
        // A start or a stop to time the lag by: the input that changes moving while the server shows us still (or moving).
        if (input.move && !last.input.move && serverSpeed_ < 0.5f && !startedAt_) {
            startedAt_ = seenInputs_;
        }
        if (!input.move && last.input.move && serverSpeed_ > 2 && !stoppedAt_) {
            stoppedAt_ = seenInputs_;
            stopSpeed_ = serverSpeed_;
        }
        steps_.push_back(simulate(last, input, seenInputs_, held_ ? *held_ + 0.5f : 1e9f));
        while (steps_.size() > kKeep) {
            steps_.pop_front();
        }
        // The render clock's length of a game frame, smoothed (for drawing between steps).
        if (nowMs > stepMs_) {
            frameMs_ += (std::clamp(nowMs - stepMs_, 20.0, 50.0) - frameMs_) * 0.1;
        }
        stepMs_ = nowMs;
    }
    if (client.snapshotCount() != seenSnapshots_) {
        seenSnapshots_ = client.snapshotCount();
        reconcile(server, client.frame());
    }

    // Drawn between the last two steps by the time since the newest, with the corrections
    // blending out.
    const double t = std::clamp((nowMs - stepMs_) / frameMs_, 0.0, 1.0);
    const Step& to = steps_.back();
    const Step& from = steps_.size() >= 2 ? steps_[steps_.size() - 2] : to;
    const float k = static_cast<float>(std::exp(-std::max(0.0, nowMs - lastMs_) / kBlendMs));  // the correction decays as e^(-t / kBlendMs)
    offset_.x *= k;
    offset_.y *= k;
    lastMs_ = nowMs;
    display_ = {from.pos.x + (to.pos.x - from.pos.x) * static_cast<float>(t) + offset_.x,
                from.pos.y + (to.pos.y - from.pos.y) * static_cast<float>(t) + offset_.y};
}

} // namespace noxsdl
