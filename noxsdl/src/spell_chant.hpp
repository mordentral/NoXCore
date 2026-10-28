// The spell chant: when the server says our cast has started (MSG_REPORT_SPELL_START,
// Cli_StartSpellChant), the client says the spell's phonemes itself, one every three
// game frames, each with its sound (SpellPhonemeUp... for men, the Female ones for
// women; spellGetPhoneme) and its hand gesture: one of eight images (SpellGestureNW ... SE) in a ring
// round the player, each shown for three frames after its phoneme. Phoneme 4 (the
// centre) has no sound and lights the north gesture, as the exe's table has it.
#pragma once

#include "camera.hpp"
#include "sprite_cache.hpp"

#include "noxcore/things/thing_db.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace noxsdl {

class SpellChant {
public:
    explicit SpellChant(const nox::things::ThingDb& things);

    /// Starts chanting spell `spellId` at game frame `frame` (a new cast replaces the last).
    void start(std::uint8_t spellId, std::uint32_t frame);
    /// Stops it and clears the gestures (leaving a game).
    void clear();

    /// Each frame at the game clock: says the phonemes that are due through `play` (a sound id).
    void update(std::uint32_t frame, bool female, const std::function<void(std::uint32_t)>& play);

    /// The gestures lit at `frame`, round the player at world point (x, y): the game draws them
    /// at (16, 41) up-left of the view centre plus each one's offset, at its own scale.
    void draw(SDL_Renderer* renderer, SpriteCache& sprites, const Camera& camera, float x, float y,
              std::uint32_t frame) const;

    /// The sound a phoneme makes (spellGetPhoneme), 0 for none.
    static std::uint32_t phonemeSound(std::uint8_t phoneme, bool female);

private:
    std::vector<std::vector<std::uint8_t>> phonemes_; ///< by spell id
    std::array<std::optional<nox::things::ImageRef>, 8> images_{};
    std::uint8_t spell_ = 0;
    std::size_t next_ = 0;            ///< g_cliChantPhonemeIndex
    std::uint32_t nextFrame_ = 0;     ///< g_cliChantNextFrame
    std::array<std::uint32_t, 8> lit_{}; ///< the frame each gesture was lit, 0 for off
};

} // namespace noxsdl
