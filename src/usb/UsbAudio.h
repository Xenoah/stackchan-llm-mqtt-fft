#pragma once
#include <cstddef>
#include <cstdint>

namespace usb_audio {
using Receiver = void (*)(const int16_t* stereo, size_t frames);
bool begin(Receiver receiver);
bool mounted();
bool active();
void setLocalMute(bool muted);
bool localMute();
void setSpeechActive(bool speaking);
uint32_t droppedBytes();
}  // namespace usb_audio
