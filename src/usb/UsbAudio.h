#pragma once
#include <cstddef>
#include <cstdint>

namespace usb_audio {
struct Diagnostics {
  bool ready, mounted, active, hostMuted, localMuted;
  uint8_t alternate;
  int16_t volumeDb;
  uint32_t opens, controls, controlFailures, transfers, feedbackTransfers;
  uint32_t callbacks, receivedBytes, speakerBytes, droppedBytes, queuedBytes;
  uint32_t malformedPackets, lastPacketBytes, lastPeak;
  uint32_t failedRequest[2], failedStage;
  uint8_t outEndpoint, feedbackEndpoint;
};
Diagnostics diagnostics();
using Receiver = void (*)(const int16_t* stereo, size_t frames);
bool begin(Receiver receiver);
bool mounted();
bool active();
void setLocalMute(bool muted);
bool localMute();
void setSpeechActive(bool speaking);
uint32_t droppedBytes();
}  // namespace usb_audio
