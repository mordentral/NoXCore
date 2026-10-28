// The game's audio ramps: a value that moves toward its target at a fixed
// rate on the millisecond clock, and the volume / pitch / pan triple every sound, channel and
// master level carries. Values are the exe's: volume 0-0x4000 (full), pitch in percent (100 =
// the sample's own rate), pan 0-0x4000 (0x2000 centre). Times are milliseconds.
#pragma once

#include <algorithm>
#include <cstdint>

namespace nox::audio {

class Ramp {
public:
    /// at `value` now, moving a full 0x4000 in a second.
    void init(std::int32_t value, std::uint64_t nowMs)
    {
        flags_ = 0;
        last_ = nowMs;
        setRate(1000, 0x4000);
        set(value);
        update(nowMs);
    }
    /// moves `range` every `ms` milliseconds; a step never spans more than `ms`.
    void setRate(std::uint32_t ms, std::int32_t range)
    {
        maxStep_ = ms;
        rate_ = static_cast<std::int32_t>((static_cast<std::uint32_t>(range) << 16) / ms);
    }
    /// jump to `value` at the next update (and every later set() jumps too).
    void set(std::int32_t value)
    {
        flags_ |= kJump;
        target_ = static_cast<std::int32_t>(static_cast<std::uint32_t>(value) << 16);
    }
    /// move toward `value` at the ramp's rate.
    void moveTo(std::int32_t value, std::uint64_t nowMs)
    {
        flags_ &= ~kJump;
        if (target_ == current_) {
            last_ = nowMs; // a ramp at rest starts counting from now
        }
        target_ = static_cast<std::int32_t>(static_cast<std::uint32_t>(value) << 16);
    }
    /// steps toward the target; true (and changed()) if the value moved.
    bool update(std::uint64_t nowMs)
    {
        const std::int32_t diff = target_ - current_;
        if (diff == 0) {
            return false;
        }
        if (flags_ & kJump) {
            current_ = target_;
        } else {
            const std::uint64_t elapsed = std::min<std::uint64_t>(nowMs - last_, maxStep_);
            last_ = nowMs;
            const std::int32_t step = rate_ * static_cast<std::int32_t>(elapsed);
            if (diff >= 0) {
                current_ += std::min(step, diff);
            } else {
                current_ -= std::min(step, -diff);
            }
        }
        flags_ |= kChanged;
        return true;
    }

    /// The current value in the exe's units (the 16.16 value's whole part).
    std::int32_t value() const { return static_cast<std::int32_t>(static_cast<std::uint32_t>(current_) >> 16); }
    std::int32_t target() const { return static_cast<std::int32_t>(static_cast<std::uint32_t>(target_) >> 16); }
    bool changed() const { return (flags_ & kChanged) != 0; }
    void clearChanged() { flags_ &= ~kChanged; }

private:
    static constexpr std::uint32_t kJump = 1;
    static constexpr std::uint32_t kChanged = 2;
    std::uint32_t flags_ = 0;
    std::int32_t current_ = 0; // 16.16
    std::int32_t target_ = 0;
    std::int32_t rate_ = 0;    // 16.16 per millisecond
    std::uint32_t maxStep_ = 1000;
    std::uint64_t last_ = 0;
};

/// Volume, pitch and pan: full volume, 100% pitch, centred.
struct RampSet {
    Ramp volume;
    Ramp pitch;
    Ramp pan;

    static constexpr std::int32_t kFull = 0x4000;
    static constexpr std::int32_t kCentre = 0x2000;

    void init(std::uint64_t nowMs)
    {
        volume.init(kFull, nowMs);
        pitch.init(100, nowMs);
        pan.init(kCentre, nowMs);
        pitch.setRate(1000, 10);
        volume.setRate(1000, kFull);
        pan.setRate(1000, kFull);
        volume.set(kFull);
    }
    /// steps all three toward their targets.
    void update(std::uint64_t nowMs)
    {
        volume.update(nowMs);
        pitch.update(nowMs);
        pan.update(nowMs);
    }
    /// any of the three moved since clearChanged().
    bool changed() const { return volume.changed() || pitch.changed() || pan.changed(); }
    /// clears all three's changed flags.
    void clearChanged()
    {
        volume.clearChanged();
        pitch.clearChanged();
        pan.clearChanged();
    }
    /// scales this one by `by` (volumes multiply, pitches multiply as percentages,
    /// pans add their offsets from the centre), applied at once.
    void combine(const RampSet& by, std::uint64_t nowMs)
    {
        volume.set(static_cast<std::int32_t>(
            (static_cast<std::uint32_t>(volume.value()) * static_cast<std::uint32_t>(by.volume.value())) >> 14));
        volume.update(nowMs);
        pitch.set(pitch.value() * by.pitch.value() / 100);
        pitch.update(nowMs);
        if (const std::int32_t offset = by.pan.value() - kCentre; offset != 0) {
            pan.set(std::clamp(pan.value() + offset, 0, kFull));
        }
        pan.update(nowMs);
    }
};

} // namespace nox::audio
