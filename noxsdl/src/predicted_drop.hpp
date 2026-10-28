// The client-predicted drawables a frame drops once they're out of sight (World::dropPredicted),
// and with each the scene's items that point into it. (Plain templates, no SDL: the tests use it.)
//
// The frame builds its scene items (MapView::SceneSprite) from the client's drawables first, each
// drawable's body, enchantments and overlays pointing at its Fade Objects marks
// (SceneSprite::fade) and its last drawn image (SceneSprite::drawnImage); the sight comes after,
// and with it the drop. A drawable dropped then is freed, but its items are still in the list the
// scene draw (or the 3D view) goes through next, which reads and writes through those pointers. So
// its items leave the list with it, as it's dropped (it isn't drawn: the game's client drops it
// before it's drawn out of sight too).
#pragma once

#include "noxcore/client/world.hpp"

#include <vector>

namespace noxsdl {

/// Drops the client-predicted drawables `gone` says to (Client::dropPredicted) and every item of
/// `items` pointing into one of them (`Item::fade`, `Item::drawnImage`).
template <class Client, class Item, class Gone>
void dropPredictedWithItems(Client& client, std::vector<Item>& items, Gone&& gone)
{
    client.dropPredicted([&](const nox::client::Drawable& d) {
        if (!gone(d)) {
            return false;
        }
        std::erase_if(items, [&](const Item& s) { return s.fade == &d.fade || s.drawnImage == &d.drawn; });
        return true;
    });
}

} // namespace noxsdl
