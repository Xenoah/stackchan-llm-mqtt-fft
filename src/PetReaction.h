#pragma once

#include <stdint.h>

// Head stroking, excitement and one pending reply. No Arduino, allocations or I/O.
class PetReaction {
 public:
  static constexpr uint32_t kDurationMs = 6000;
  static constexpr uint32_t kSpeechIntervalMs = 5500;
  struct Pose { int yaw; int pitch; };  // offsets, tenths of a degree

  // zones: bit 0=front, 1=middle, 2=back. A stationary touch is not a stroke.
  bool update(uint32_t now, uint8_t zones, bool hardwareSwipe, bool enabled);
  bool active(uint32_t now) const;
  uint32_t remaining(uint32_t now) const;
  uint8_t level() const { return level_; }
  bool suppressesClicks(uint32_t now) const;
  bool speechReady(uint32_t now) const;
  bool takeSpeech(uint32_t now);
  Pose pose(uint32_t now) const;
  void cancel();

 private:
  bool stroke(uint32_t now);
  bool touching_ = false, blockedContact_ = false, consumedContact_ = false;
  bool haveRelease_ = false, haveStroke_ = false, haveSpeech_ = false;
  bool pendingSpeech_ = false;
  float origin_ = 0;
  uint8_t level_ = 0;
  uint32_t originAt_ = 0, releasedAt_ = 0, startedAt_ = 0;
  uint32_t lastStrokeAt_ = 0, lastSpeechAt_ = 0;
};
