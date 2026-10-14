#pragma once
// The STB on-screen display drawn with cairo on every frame (spec 5.20.2): the channel list on the left half of the
// screen and the channel banner (service name, present/following) at the bottom.
#include <cairo.h>

#include "model.hpp"

namespace atsc3::stb {

void drawUi(cairo_t* cr, int width, int height, const UiSnapshot& ui);

}  // namespace atsc3::stb
