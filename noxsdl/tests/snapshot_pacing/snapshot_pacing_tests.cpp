#include "snapshot_pacing.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void updateRate(int multiplier, bool matchServer)
{
    std::uint32_t frames = 0, snapshots = 0, drawn = 0;
    std::uint64_t drawnAt = 0;
    int renders = 0;
    for (int step = 0; step < 30 * multiplier; ++step) {
        const auto now = 1000 + static_cast<std::uint64_t>(step) * 1000 / (30 * multiplier);
        if (step % multiplier == 0) {
            ++frames;
        }
        ++snapshots;
        const auto update = noxsdl::renderUpdateCount(matchServer, frames, snapshots);
        if (noxsdl::snapshotFrameDue(update, drawn, now, drawnAt)) {
            ++renders;
            drawn = update;
            drawnAt = now;
        }
        require(!noxsdl::snapshotFrameDue(update, drawn, now + 1, drawnAt),
                "polling without another update must not render another frame");
    }
    require(renders == (matchServer ? 30 * multiplier : 30),
            "Match Server follows all substeps; Lock to 30 FPS follows only base frames");
}

} // namespace

int main()
{
    for (int multiplier = 1; multiplier <= 4; ++multiplier) {
        updateRate(multiplier, false);
        updateRate(multiplier, true);
    }
    using noxsdl::snapshotFrameDue;
    require(!snapshotFrameDue(12, 12, 1049, 1000), "a paused stream waits before the fallback");
    require(snapshotFrameDue(12, 12, 1050, 1000), "a paused stream redraws after 50 ms");
    require(snapshotFrameDue(13, 12, 1001, 1000), "a new movement update is drawn without buffering");
    require(snapshotFrameDue(17, 12, 1030, 1000), "batched updates draw the newest state once");
    require(snapshotFrameDue(1, 12, 1001, 1000), "a reset stream does not wait on the old counter");
    std::cout << "Snapshot pacing: 30/60/90/120 Hz, classic frames, stalls and reset passed\n";
}
