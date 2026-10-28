// Builds the sound event table from thing.bin's AUD and AVNT records (see sound_events.hpp).
#include "noxcore/audio/sound_events.hpp"

#include "noxcore/audio/audio_bag.hpp"
#include "noxcore/audio/sound_names.hpp"
#include "noxcore/things/thing_db.hpp"

namespace nox::audio {

namespace {

constexpr std::size_t kMaxSamples = 32; // the event's 32 u16 file slots

// The event's files as Audio.bag indexes (at most 32); names the bag lacks go to `missing`.
void addSamples(SoundEventDef& ev, const std::vector<std::string>& files, const AudioBag* bag,
                std::vector<std::string>& missing)
{
    ev.samples.clear();
    for (const auto& file : files) {
        if (ev.samples.size() == kMaxSamples) {
            break;
        }
        const auto index = bag ? bag->find(file) : std::nullopt;
        if (index) {
            ev.samples.push_back(static_cast<std::uint16_t>(*index));
        } else if (bag) {
            missing.push_back(file);
        }
    }
}

} // namespace

SoundEventTable buildSoundEvents(const things::ThingDb& db, const AudioBag* bag)
{
    SoundEventTable table;
    // AUD: every field at once, and the random-file flag.
    for (const auto& a : db.audio) {
        const auto id = soundId(a.name);
        // Names the sound table doesn't know, and NULL (id 0), are skipped.
        if (!id || *id == 0) {
            continue;
        }
        auto& ev = table.events[*id];
        ev.flags = SoundEventDef::kRandom;
        ev.hearing = a.hearing;
        // Volumes 0-100 become 0-0x4000 (x 163) and ranges are scaled by 15.
        ev.volume = a.volume * 163;
        if (a.range > 0) {
            ev.range = a.range * 15;
        }
        ev.maxInstances = a.maxInstances;
        ev.pitchMin = a.pitchMin;
        ev.pitchMax = a.pitchMax;
        ev.priority = a.priority;
        addSamples(ev, a.files, bag, table.missingSamples);
        ev.defined = true;
    }
    // AVNT: only the fields it gives.
    // Applied after every AUD, so an AVNT's fields override the same event's AUD values.
    for (const auto& a : db.avnts) {
        const auto id = soundId(a.name);
        if (!id || *id == 0) {
            continue;
        }
        auto& ev = table.events[*id];
        if (a.priority) {
            ev.priority = *a.priority;
        }
        if (a.flags) {
            ev.flags = *a.flags;
        }
        if (a.volume) {
            ev.volume = *a.volume * 163;
        }
        if (a.maxInstances) {
            ev.maxInstances = *a.maxInstances;
        }
        if (a.loops) {
            ev.loops = *a.loops;
        }
        if (a.pitch) {
            ev.pitchMin = a.pitch->first;
            ev.pitchMax = a.pitch->second;
        }
        if (a.files) {
            addSamples(ev, *a.files, bag, table.missingSamples);
        }
        if (a.delay) {
            ev.delayMin = a.delay->first;
            ev.delayMax = a.delay->second;
        }
        if (a.range && *a.range > 0) {
            ev.range = *a.range * 15;
        }
        if (a.hearing) {
            ev.hearing = *a.hearing;
        }
        ev.defined = true;
    }
    return table;
}

} // namespace nox::audio
