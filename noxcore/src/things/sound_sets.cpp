// soundset.bin reader (format in sound_sets.hpp).
#include "noxcore/things/sound_sets.hpp"

#include "noxcore/audio/sound_names.hpp"
#include "noxcore/content/content_pack.hpp"
#include "noxcore/crypto/noxcrypt.hpp"
#include "noxcore/io/file.hpp"

#include <cctype>

namespace nox::things {

namespace {

/// parseString's words: split by white space (the cipher's zero padding counts as
/// space here), "//" to the end of the line a comment; a word cut off by the file's end is lost.
struct Words {
    std::string_view text;
    std::size_t at = 0;

    bool next(std::string& out)
    {
        out.clear();
        while (at < text.size()) {
            const char c = text[at++];
            if (std::isspace(static_cast<unsigned char>(c)) || c == '\0') {
                if (!out.empty()) {
                    return true;
                }
                continue;
            }
            if (c == '/' && !out.empty() && out.back() == '/') {
                // A comment: the rest of the line, and the word begun so far, go.
                while (at < text.size() && text[at] != '\n') {
                    ++at;
                }
                out.clear();
                continue;
            }
            out.push_back(c);
        }
        return false;
    }
};

// The keys in slot order: kKeys[i] fills SoundSlot i + 1 (slot 0 is unused).
constexpr std::string_view kKeys[] = {
    "LISTEN_SOUND",       "TAUNT_SOUND",          "THREATEN_SOUND",          "IDLE_SOUND",
    "ENGAGE_SOUND",       "MELEE_ATTACK_INIT_SOUND", "MELEE_ATTACK_SOUND",   "MELEE_ATTACK_HITTING_SOUND",
    "MELEE_ATTACK_MISSING_SOUND", "MISSILE_ATTACK_INIT_SOUND", "MISSILE_ATTACK_SOUND", "FLEE_SOUND",
    "RETREAT_SOUND",      "SPELL_INIT_SOUND",     "DIE_SOUND",               "INJURED_SOUND",
    "RECOGNIZE_SOUND",    "MOVE_SOUND",
};

} // namespace

void SoundSets::reindex()
{
    index.clear();
    index.reserve(sets.size());
    for (std::size_t i = 0; i < sets.size(); ++i) {
        index.insert_or_assign(sets[i].name, static_cast<std::uint32_t>(i)); // (the last of a name kept)
    }
    indexedOf = sets.size();
}

const SoundSet* SoundSets::find(std::string_view name) const
{
    if (indexedOf == sets.size()) {
        const auto it = index.find(name);
        return it == index.end() ? nullptr : &sets[it->second];
    }
    // Searched from the back so a name given twice finds the last, as the exe's newest-first list does.
    for (auto it = sets.rbegin(); it != sets.rend(); ++it) {
        if (it->name == name) {
            return &*it;
        }
    }
    return nullptr;
}

SoundSets parseSoundSets(std::string_view text)
{
    SoundSets out;
    Words words{text};
    std::string word;
    std::string value;
    while (words.next(word)) {
        SoundSet set;
        set.name = word;
        bool ok = true;
        while (words.next(word)) {
            if (word == "END") {
                break;
            }
            if (!words.next(value)) {
                break;
            }
            std::size_t slot = 0;
            for (std::size_t i = 0; i < std::size(kKeys); ++i) {
                if (word == kKeys[i]) {
                    slot = i + 1;
                }
            }
            if (slot == 0) {
                ok = false; // (fails: the reading stops)
                break;
            }
            set.sounds[slot] = audio::soundId(value).value_or(0);
        }
        out.sets.push_back(std::move(set));
        if (!ok) {
            break;
        }
    }
    out.reindex();
    return out;
}

SoundSets loadSoundSets(const std::filesystem::path& path)
{
    auto data = readFileBytes(path);
    crypto::cipherForKey(crypto::FileKey::SoundSetBin).decrypt(data);
    content::applyContentPacks(content::TableFormat::SoundSet, data); // (the installed content packs' changes, once, as it loads; none installed: nothing)
    return parseSoundSets(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

} // namespace nox::things
