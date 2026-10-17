#include "tusb.h"
#include <cstring>

// Arduino 2.0.17 assigns CDC to endpoints OUT3/IN4/IN5. Its old ESP32 DCD
// special-cases IN5 as a notification endpoint with no physical TX FIFO.
// The DWC2 driver uses one TX FIFO per IN endpoint, so keep every real IN
// endpoint below 5: audio OUT1/IN1, CDC OUT2/IN2 and notification IN3.
// Only the descriptor is remapped; TinyUSB CDC reads its endpoint addresses
// from it. Arduino's CDC implementation and the shared USB task stay intact.
extern "C" const uint8_t* __real_tud_descriptor_configuration_cb(uint8_t index);
extern "C" const uint8_t* __wrap_tud_descriptor_configuration_cb(uint8_t index) {
  const uint8_t* source = __real_tud_descriptor_configuration_cb(index);
  static uint8_t descriptor[512];
  if (!source) return nullptr;
  const uint16_t length = uint16_t(source[2]) | uint16_t(source[3]) << 8;
  if (length < 9 || length > sizeof(descriptor)) return nullptr;
  memcpy(descriptor, source, length);
  uint8_t interfaceClass = 0;
  for (unsigned offset = 0; offset < length;) {
    uint8_t* item = descriptor + offset;
    if (item[0] < 2 || offset + item[0] > length) return nullptr;
    if (item[1] == TUSB_DESC_INTERFACE && item[0] >= 9) interfaceClass = item[5];
    if (item[1] == TUSB_DESC_ENDPOINT && item[0] >= 7) {
      if (interfaceClass == TUSB_CLASS_CDC) item[2] = 0x83;
      if (interfaceClass == TUSB_CLASS_CDC_DATA) item[2] = (item[2] & 0x80) | 2;
    }
    offset += item[0];
  }
  return descriptor;
}
