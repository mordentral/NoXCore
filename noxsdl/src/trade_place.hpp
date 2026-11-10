// Where the shop and the NPC dialog sit in the 640 x 480 layout (trade_screens.cpp). The game moves
// both, once loaded, to the screen's bottom right (the screen's size less theirs: 0, 360 for their
// 640 x 120), not where Shop.wnd and Dialog.wnd put them (0, 359). Their art, ShopBase, is drawn
// at the layout's corner and carries its own offset (0, 326); its picture frame is a 100 x 100
// hole at (521, 38) in it, and the window's picture (a shopkeeper's or a talker's, all 100 x 100,
// the HD pack's too) is at (521, 4) in the window. Only at 360 does the picture fill the hole: at
// the file's 359 it covered the frame's top row and left a row of the world showing under it.
#pragma once

namespace noxsdl {

struct LayoutPlace {
    int x = 0;
    int y = 0;
};

/// A window `width` x `height` at the layout's bottom right, as the game places the shop and the dialog.
constexpr LayoutPlace bottomRightPlace(int width, int height)
{
    return {640 - width, 480 - height};
}

/// ShopBase's offset, and the hole for the picture in it (its image's own pixels).
constexpr LayoutPlace kShopBaseOffset{0, 326};
constexpr LayoutPlace kShopBaseHole{521, 38};
constexpr int kShopBaseHoleSize = 100;

} // namespace noxsdl
