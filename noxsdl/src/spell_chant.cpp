// SpellChant (spell_chant.hpp): a cast's phonemes played and their gestures lit in a ring round
// the caster, one every three game frames.
#include "spell_chant.hpp"

#include "noxcore/things/spell_ids.hpp"

namespace noxsdl {

namespace {

/// Phoneme (0-8) to gesture image.
constexpr std::array<std::size_t, 9> kGesture{0, 1, 2, 3, 1, 4, 5, 6, 7};

/// Each gesture's place in the ring, in game pixels.
constexpr std::array<SDL_Point, 8> kRing{{
    {-35, -35}, {0, -50}, {35, -35}, {-50, 0}, {50, 0}, {-35, 35}, {0, 50}, {35, 35}}};

constexpr std::array<const char*, 8> kImages{"SpellGestureNW", "SpellGestureN", "SpellGestureNE", "SpellGestureW",
                                             "SpellGestureE",  "SpellGestureSW", "SpellGestureS", "SpellGestureSE"};

constexpr std::uint32_t kFramesBetween = 3; ///< a phoneme every three frames
constexpr std::uint32_t kShownFor = 4;      ///< drawn for frames lit .. lit + 4 (cleared once over 3, after drawing)

} // namespace

SpellChant::SpellChant(const nox::things::ThingDb& things)
{
    phonemes_.resize(nox::things::kSpellCount);
    for (const auto& s : things.spells) {
        if (const auto id = nox::things::spellId(s.id); id && *id < phonemes_.size()) {
            phonemes_[*id] = s.phonemes;
        }
    }
    for (std::size_t i = 0; i < kImages.size(); ++i) {
        for (const auto& img : things.images) {
            if (img.name == kImages[i]) {
                if (img.image) {
                    images_[i] = *img.image;
                } else if (img.animation && !img.animation->frames.empty()) {
                    images_[i] = img.animation->frames.front();
                }
                break;
            }
        }
    }
}

std::uint32_t SpellChant::phonemeSound(std::uint8_t phoneme, bool female)
{
    // spellGetPhoneme: SpellPhonemeUpLeft, Up, UpRight, Left, -, Right, DownLeft, Down, DownRight, and
    // the Female set (0xC2-0xC9) in the same order.
    constexpr std::array<std::uint32_t, 9> male{0xC1, 0xBA, 0xBB, 0xC0, 0, 0xBC, 0xBF, 0xBE, 0xBD};
    constexpr std::array<std::uint32_t, 9> woman{0xC9, 0xC2, 0xC3, 0xC8, 0, 0xC4, 0xC7, 0xC6, 0xC5};
    if (phoneme >= male.size()) {
        return 0;
    }
    return female ? woman[phoneme] : male[phoneme];
}

void SpellChant::start(std::uint8_t spellId, std::uint32_t frame)
{
    spell_ = spellId;
    next_ = 0;
    nextFrame_ = frame;
}

void SpellChant::clear()
{
    spell_ = 0;
    lit_.fill(0);
}

void SpellChant::update(std::uint32_t frame, bool female, const std::function<void(std::uint32_t)>& play)
{
    // once a game frame.
    for (auto& f : lit_) {
        if (f != 0 && frame - f > kShownFor) {
            f = 0;
        }
    }
    if (spell_ == 0 || spell_ >= phonemes_.size()) {
        return;
    }
    const auto& seq = phonemes_[spell_];
    if (next_ < seq.size() && nextFrame_ <= frame) {
        const std::uint8_t p = seq[next_];
        if (const auto sound = phonemeSound(p, female); sound != 0 && play) {
            play(sound);
        }
        if (p < kGesture.size()) {
            lit_[kGesture[p]] = frame == 0 ? 1 : frame;  // (0 means unlit)
        }
        nextFrame_ = frame + kFramesBetween;
        ++next_;
    }
    if (next_ >= seq.size()) {
        spell_ = 0; // the spell tree reached the spell: done
    }
}

void SpellChant::draw(SDL_Renderer* renderer, SpriteCache& sprites, const Camera& camera, float x, float y,
                      std::uint32_t frame) const
{
    for (std::size_t i = 0; i < lit_.size(); ++i) {
        if (lit_[i] == 0 || frame - lit_[i] > kShownFor || !images_[i]) {
            continue;
        }
        const Sprite* sp = sprites.get(*images_[i]);
        if (!sp) {
            continue;
        }
        // The ring centred 16 left of and 41 above the caster's point.
        const float wx = x - 16 + static_cast<float>(kRing[i].x) + sp->offsetX;
        const float wy = y - 41 + static_cast<float>(kRing[i].y) + sp->offsetY;
        const SDL_FRect dst{(wx - camera.left()) * camera.zoom, (wy - camera.top()) * camera.zoom,
                            sp->width * camera.zoom, sp->height * camera.zoom};
        sp->scale = sp->hd ? SDL_SCALEMODE_LINEAR : camera.artScaleMode();
        drawSprite(renderer, *sp, nullptr, dst);
    }
}

} // namespace noxsdl
