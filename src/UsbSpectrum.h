#pragma once
#include <cstdint>

namespace usb_spectrum {
struct Diagnostics {
  bool ready, active, open, hold, waveform;
  uint32_t receivedFrames, analyses, draws;
  float peakHz, peakDb, leftDb, rightDb;
};
// Read from the application loop, like update().
Diagnostics diagnostics();
bool begin();
// Called only by the application loop (also during settings/TTS servicing).
void update(bool homeVisible);
bool open();
void close();
bool isOpen();
bool isReady();
bool isActive();
void tap(int16_t x, int16_t y);
void copyBars(uint8_t (&bars)[32]);
}  // namespace usb_spectrum
