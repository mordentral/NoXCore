// The collaborative session's document on the wire and on the host: what a late joiner is sent (the
// published version's document, the base its working copy replays the op log on), built so the host
// and every client hold the very same document: the same ids (ObjIds, polygon and group keys), so
// every op means the same thing on each; the same keep-as-stored state, so every client writes the
// published map byte for byte as the host does (its CRC matches: no download).
#pragma once

#include "noxcore/mapedit/document.hpp"
#include "noxcore/mapedit/ops.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace nox::mapedit {

/// The document's bytes: its .map (writeMapBytes), each record's ObjId and order key in record
/// order, the polygon and group keys and the next ids, and the script source.
std::vector<std::uint8_t> encodeSessionDocument(const MapDocument& doc, const things::ThingDb& things);
/// The document back: the .map decoded (MapDocument::fromMapFile), then the ids and keys put back.
/// Throws FormatError for bytes encodeSessionDocument didn't write.
MapDocument decodeSessionDocument(std::span<const std::uint8_t> bytes, const things::ThingDb& things);
/// The document as every session member holds it: through its bytes and back (the host's own copy
/// is made so too, so it is identical to theirs).
MapDocument canonicalSessionDocument(const MapDocument& doc, const things::ThingDb& things);

struct IdMoves;

/// The new ids an op brings (objects, extents, waypoints marked fresh) moved off those `doc`
/// already uses: two editors adding at once picked the same next ids. Links inside the op (an
/// item's parent, a waypoint's links to another new one, a group naming one) follow. Returns
/// whether anything changed; `moves` gets what moved (session_merge.hpp).
bool freshenNewIds(const MapDocument& doc, Op& op, IdMoves* moves = nullptr);

} // namespace nox::mapedit
