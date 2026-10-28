// The "who's editing" pencil (pencil_icon.hpp's shape) for the editor's ImGui draws: the live
// view's editors, other editors' cursors and the Session panel's People rows.
#pragma once

#include "pencil_icon.hpp"

#include "noxcore/mapedit/session_proto.hpp"

#include <imgui.h>

namespace noxsdl::editor {

/// An editor's colour (lock outlines, cursors, pencils) as ImGui's.
inline ImU32 peerColourU32(std::uint32_t peer, std::uint8_t alpha = 255)
{
    const PeerRgb c = peerRgb(peer);
    return IM_COL32(c.r, c.g, c.b, alpha);
}

/// The pencil centred at `c`, `size` pixels long: the editor's colour, greyed and fainter when idle.
inline void pencilMark(ImDrawList* dl, ImVec2 c, float size, std::uint32_t peer, bool idle)
{
    const PencilShape s = pencilShape(c.x, c.y, size);
    const auto a = static_cast<int>(idle ? 150 : 255);
    const PeerRgb body = idle ? idleRgb(peerRgb(peer)) : peerRgb(peer);
    const auto v = [](PencilShape::P p) { return ImVec2(p.x, p.y); };
    ImVec2 edge[6];
    for (int i = 0; i < 6; ++i) edge[i] = v(s.outline[static_cast<std::size_t>(i)]);
    dl->AddPolyline(edge, 6, IM_COL32(10, 10, 12, a * 85 / 100), ImDrawFlags_Closed, 2.0f);
    dl->AddQuadFilled(v(s.cone[0]), v(s.cone[1]), v(s.cone[2]), v(s.cone[3]), IM_COL32(242, 204, 140, a));
    dl->AddTriangleFilled(v(s.lead[0]), v(s.lead[1]), v(s.lead[2]), IM_COL32(46, 43, 43, a));
    dl->AddQuadFilled(v(s.body[0]), v(s.body[1]), v(s.body[2]), v(s.body[3]), IM_COL32(body.r, body.g, body.b, a));
    dl->AddQuadFilled(v(s.cap[0]), v(s.cap[1]), v(s.cap[2]), v(s.cap[3]), IM_COL32(242, 148, 158, a));
}

/// "editing", "idle", "playing".
inline const char* activityText(nox::mapedit::session::Activity a)
{
    return nox::mapedit::session::activityName(a);
}

} // namespace noxsdl::editor
