#include "PetReaction.h"

#include <math.h>

bool PetReaction::update(uint32_t now, uint8_t zones, bool hardwareSwipe,
                         bool enabled) {
  zones &= 7;
  if (!enabled) {
    cancel();
    touching_ = false;
    blockedContact_ = zones != 0;
    consumedContact_ = zones != 0;
    releasedAt_ = now;
    haveRelease_ = true;
    return false;
  }
  if (zones == 0) {
    if (consumedContact_) {
      releasedAt_ = now;
      haveRelease_ = true;
    }
    touching_ = blockedContact_ = consumedContact_ = false;
    return false;
  }
  if (blockedContact_) return false;  // a touch begun in settings stays ignored

  int sum = 0, count = 0;
  for (int i = 0; i < 3; ++i) {
    if (zones & (1 << i)) { sum += i; ++count; }
  }
  const float position = static_cast<float>(sum) / count;
  if (!touching_) {
    touching_ = true;
    origin_ = position;
    originAt_ = now;
  }
  const uint32_t travelMs = now - originAt_;
  // Also recognize slower/partial strokes that the BSP's three-zone swipe misses.
  const bool travelled = fabsf(position - origin_) >= 0.9f &&
                         travelMs >= 60 && travelMs <= 1400;
  if (hardwareSwipe || travelled) {
    origin_ = position;
    originAt_ = now;
    consumedContact_ = true;
    return stroke(now);
  }
  if (travelMs > 1400) {
    origin_ = position;
    originAt_ = now;
  }
  return false;
}

bool PetReaction::stroke(uint32_t now) {
  if (haveStroke_ && now - lastStrokeAt_ < 300) return false;
  const bool continuing = active(now);
  level_ = continuing && now - lastStrokeAt_ < 2500
               ? (level_ < 3 ? level_ + 1 : 3) : 1;
  if (!continuing) startedAt_ = now;  // never restart the swing on every stroke
  lastStrokeAt_ = now;
  haveStroke_ = pendingSpeech_ = true;
  return true;
}

bool PetReaction::active(uint32_t now) const {
  return haveStroke_ && now - lastStrokeAt_ < kDurationMs;
}

uint32_t PetReaction::remaining(uint32_t now) const {
  return active(now) ? kDurationMs - (now - lastStrokeAt_) : 0;
}

bool PetReaction::suppressesClicks(uint32_t now) const {
  return consumedContact_ || blockedContact_ ||
         (haveRelease_ && now - releasedAt_ < 750);
}

bool PetReaction::speechReady(uint32_t now) const {
  // Coalesce repeated strokes. Do not say an old reply after unrelated speech.
  return pendingSpeech_ && active(now) && now - lastStrokeAt_ <= 1800 &&
         (!haveSpeech_ || now - lastSpeechAt_ >= kSpeechIntervalMs);
}

bool PetReaction::takeSpeech(uint32_t now) {
  if (!speechReady(now)) return false;
  pendingSpeech_ = false;
  haveSpeech_ = true;
  lastSpeechAt_ = now;
  return true;
}

PetReaction::Pose PetReaction::pose(uint32_t now) const {
  if (!active(now)) return {0, 0};
  const float elapsed = (now - startedAt_) * 0.001f;
  float envelope = fminf(1.0f, elapsed / 0.5f);
  envelope *= fminf(1.0f, remaining(now) / 1200.0f);
  constexpr float pi = 3.14159265358979323846f;
  // Side-to-side delight with little nods. The caller clamps and rate-limits.
  return {static_cast<int>(lroundf((190 + 50 * level_) * envelope *
                                  sinf(2 * pi * elapsed / 1.45f))),
          static_cast<int>(lroundf((40 + 20 * level_) * envelope *
                                  sinf(2 * pi * elapsed / 0.72f)))};
}

void PetReaction::cancel() {
  haveStroke_ = pendingSpeech_ = false;
  level_ = 0;
  // Keep click suppression and speech cooldown through UI interruptions.
}
