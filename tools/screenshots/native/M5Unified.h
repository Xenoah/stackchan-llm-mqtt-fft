#pragma once
#include "Arduino.h"
#include <lgfx/v1/LGFX_Sprite.hpp>

// M5Canvas itself derives from LGFX_Sprite; all rasterization and fonts below
// are the original M5GFX implementation. Only hardware I/O is replaced.
class M5Canvas : public lgfx::LGFX_Sprite {
 public:
  bool getTouch(int16_t*, int16_t*) { return false; }
  void pushSprite(int32_t, int32_t) {} // No physical LCD; export the framebuffer.
};
struct HostM5 { M5Canvas Display; };
inline HostM5 M5;
