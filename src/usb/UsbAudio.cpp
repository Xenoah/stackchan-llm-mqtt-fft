#include "UsbAudioConfig.h"
#include "UsbAudio.h"
#include <M5Unified.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include "esp32-hal-tinyusb.h"
#include "esp_timer.h"
#include "freertos/stream_buffer.h"
#include "tusb.h"
#include "device/usbd_pvt.h"

#if TUSB_VERSION_MAJOR != 0 || TUSB_VERSION_MINOR != 16
#error Review the vendored UAC2 driver before changing Arduino/TinyUSB versions.
#endif

namespace {
constexpr uint8_t kClock = 4, kInput = 1, kFeature = 2, kOutput = 3;
constexpr uint8_t kSpeakerChannel = 1;  // Existing TTS uses channel 0.
constexpr size_t kBlockBytes = 1920, kQueueBytes = kBlockBytes * 6;
constexpr size_t kTargetBytes = kBlockBytes * 2;
StreamBufferHandle_t stream = nullptr;
std::atomic<usb_audio::Receiver> receiver{nullptr};
std::atomic<bool> ready{false}, muted{false}, hostMuted{false}, speech{false};
std::atomic<int16_t> volumeDb{0};
std::atomic<uint32_t> dropped{0};
std::atomic<uint32_t> lastPacketMs{0};
uint8_t streamInterface = 0;

uint16_t loadDescriptor(uint8_t* dst, uint8_t* itf) {
  const uint8_t out = tinyusb_get_free_out_endpoint();
  const uint8_t feedback = tinyusb_get_free_in_endpoint();
  if (!out || !feedback) return 0;
  const uint8_t name = tinyusb_add_string_descriptor("StackChan USB Spectrum");
  streamInterface = *itf + 1;
  const uint8_t descriptor[] = {
    TUD_AUDIO_DESC_IAD(*itf, 2, 0),
    TUD_AUDIO_DESC_STD_AC(*itf, 0, name),
    TUD_AUDIO_DESC_CS_AC(0x0200, AUDIO_FUNC_DESKTOP_SPEAKER,
      TUD_AUDIO_DESC_CLK_SRC_LEN + TUD_AUDIO_DESC_INPUT_TERM_LEN +
      TUD_AUDIO_DESC_FEATURE_UNIT_TWO_CHANNEL_LEN + TUD_AUDIO_DESC_OUTPUT_TERM_LEN, 0),
    // Fixed 48 kHz clock, frequency and clock-valid are read-only.
    TUD_AUDIO_DESC_CLK_SRC(kClock, 1, 5, 0, 0),
    TUD_AUDIO_DESC_INPUT_TERM(kInput, AUDIO_TERM_TYPE_USB_STREAMING, 0,
      kClock, 2, 3, 0, 0, 0),
    TUD_AUDIO_DESC_FEATURE_UNIT_TWO_CHANNEL(kFeature, kInput,
      (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_MUTE_POS) |
      (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_VOLUME_POS), 0, 0, 0),
    TUD_AUDIO_DESC_OUTPUT_TERM(kOutput, AUDIO_TERM_TYPE_OUT_DESKTOP_SPEAKER,
      0, kFeature, kClock, 0, 0),
    TUD_AUDIO_DESC_STD_AS_INT(streamInterface, 0, 0, name),
    TUD_AUDIO_DESC_STD_AS_INT(streamInterface, 1, 2, name),
    TUD_AUDIO_DESC_CS_AS_INT(kInput, 0, AUDIO_FORMAT_TYPE_I,
      AUDIO_DATA_FORMAT_TYPE_I_PCM, 2, 3, 0),
    TUD_AUDIO_DESC_TYPE_I_FORMAT(2, 16),
    TUD_AUDIO_DESC_STD_AS_ISO_EP(out,
      TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS | TUSB_ISO_EP_ATT_DATA,
      CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX, 1),
    TUD_AUDIO_DESC_CS_AS_ISO_EP(AUDIO_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK,
      0, AUDIO_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0),
    TUD_AUDIO_DESC_STD_AS_ISO_EP(uint8_t(0x80 | feedback),
      TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_NO_SYNC | TUSB_ISO_EP_ATT_EXPLICIT_FB, 4, 4),
  };
  static_assert(sizeof(descriptor) == CFG_TUD_AUDIO_FUNC_1_DESC_LEN, "UAC descriptor length");
  memcpy(dst, descriptor, sizeof(descriptor));
  *itf += 2;
  return sizeof(descriptor);
}

// Registration must precede Arduino's automatic USB.begin() in app_main.
// This only touches the C HAL's static descriptor registry, not the USB object.
const esp_err_t registration = tinyusb_enable_interface(
    USB_INTERFACE_CUSTOM, CFG_TUD_AUDIO_FUNC_1_DESC_LEN, loadDescriptor);

void speakerTask(void*) {
  alignas(4) static int16_t blocks[3][kBlockBytes / 2];
  unsigned index = 0;
  bool priming = true;
  uint32_t primingSince = 0;
  for (;;) {
    if (priming) {
      const size_t queued = xStreamBufferBytesAvailable(stream);
      const uint32_t now = millis();
      if (!queued) primingSince = 0;
      else if (!primingSince) primingSince = now;
      // Also play short UI sounds: do not require a full 20 ms of samples.
      if (!queued || (queued < kTargetBytes && now - primingSince < 20)) {
        vTaskDelay(pdMS_TO_TICKS(2));
        continue;
      }
      priming = false; primingSince = 0;
    }
    const size_t bytes = xStreamBufferReceive(stream, blocks[index], sizeof(blocks[0]), pdMS_TO_TICKS(25));
    if (!bytes) { priming = true; continue; }
    // Independent channel gain preserves the configured TTS master volume.
    // M5's channel gain is squared. Cap USB to the source project's level.
    const float gain = std::pow(10.f, volumeDb.load() / (256.f * 20.f));
    const bool silent = muted.load() || hostMuted.load();
    M5.Speaker.setChannelVolume(kSpeakerChannel,
        silent ? 0 : uint8_t(90.f * std::sqrt(gain) * (speech.load() ? .5f : 1.f)));
    if (!M5.Speaker.playRaw(blocks[index], bytes / 2, 48000, true, 1, kSpeakerChannel, false)) {
      dropped.fetch_add(bytes);
    }
    index = (index + 1) % 3;
  }
}
}  // namespace

namespace usb_audio {
bool begin(Receiver callback) {
  if (ready.load()) return true;
  if (registration != ESP_OK || !callback) return false;
  stream = xStreamBufferCreate(kQueueBytes, kBlockBytes);
  if (!stream) return false;
  if (xTaskCreatePinnedToCore(speakerTask, "usbSpeaker", 4096, nullptr, 3, nullptr, 0) != pdPASS) {
    vStreamBufferDelete(stream); stream = nullptr; return false;
  }
  receiver.store(callback);
  ready.store(true);
  return true;
}
bool mounted() { return tud_mounted(); }
bool active() {
  const uint32_t last = lastPacketMs.load();
  return last && uint32_t(esp_timer_get_time() / 1000 - last) < 250;
}
void setLocalMute(bool value) { muted.store(value); }
bool localMute() { return muted.load(); }
void setSpeechActive(bool value) { speech.store(value); }
uint32_t droppedBytes() { return dropped.load(); }
}  // namespace usb_audio

extern "C" {
usbd_class_driver_t const* usbd_app_driver_get_cb(uint8_t* count) {
  static const usbd_class_driver_t driver = {
#if CFG_TUSB_DEBUG >= CFG_TUD_LOG_LEVEL
    "StackChan UAC2",
#endif
    audiod_init, audiod_reset, audiod_open, audiod_control_xfer_cb,
    audiod_xfer_cb, audiod_sof_isr
  };
  *count = 1;
  return &driver;
}

bool tud_audio_rx_done_post_read_cb(uint8_t, uint16_t, uint8_t, uint8_t, uint8_t) {
  alignas(4) int16_t packet[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX / 2];
  const uint16_t bytes = tud_audio_read(packet, sizeof(packet));
  if (!ready.load() || !bytes || bytes % 4) return true;
  lastPacketMs.store(uint32_t(esp_timer_get_time() / 1000));
  receiver.load()(packet, bytes / 4);
  // Never enqueue partial stereo frames on overflow.
  if (xStreamBufferSpacesAvailable(stream) >= bytes) {
    xStreamBufferSend(stream, packet, bytes, 0);
  } else {
    dropped.fetch_add(bytes);
  }
  // Clock feedback keeps the speaker queue near 20 ms despite host/I2S drift.
  // Bound the correction to +/- 200 Hz; advertise room for 49 frames/ms.
  float correction = (int(xStreamBufferBytesAvailable(stream)) - int(kTargetBytes)) / 32000.f;
  correction = std::max(-.2f, std::min(.2f, correction));
  tud_audio_fb_set(uint32_t((48.f - correction) * 65536.f));
  return true;
}

bool tud_audio_set_itf_cb(uint8_t, tusb_control_request_t const* request) {
  if (uint8_t(request->wIndex) == streamInterface) {
    lastPacketMs.store(0);
    if (request->wValue == 1) tud_audio_fb_set(48u << 16);
  }
  return true;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const* request) {
  const uint8_t entity = request->wIndex >> 8;
  const uint8_t selector = request->wValue >> 8;
  const uint8_t channel = request->wValue & 0xff;
  auto reply = [&](const void* data, uint16_t size) {
    return tud_audio_buffer_and_schedule_control_xfer(rhport, request,
        const_cast<void*>(data), size);
  };
  if (channel != 0) return false;
  if (entity == kClock) {
    if (selector == AUDIO_CS_CTRL_CLK_VALID && request->bRequest == AUDIO_CS_REQ_CUR) {
      const uint8_t valid = 1; return reply(&valid, sizeof(valid));
    }
    if (selector != AUDIO_CS_CTRL_SAM_FREQ) return false;
    if (request->bRequest == AUDIO_CS_REQ_CUR) {
      const uint32_t rate = 48000; return reply(&rate, sizeof(rate));
    }
    if (request->bRequest == AUDIO_CS_REQ_RANGE) {
      const audio_control_range_4_n_t(1) range = {1, {{48000, 48000, 0}}};
      return reply(&range, sizeof(range));
    }
  }
  if (entity == kFeature) {
    if (selector == AUDIO_FU_CTRL_MUTE && request->bRequest == AUDIO_CS_REQ_CUR) {
      const uint8_t value = hostMuted.load(); return reply(&value, sizeof(value));
    }
    if (selector == AUDIO_FU_CTRL_VOLUME) {
      if (request->bRequest == AUDIO_CS_REQ_CUR) {
        const int16_t value = volumeDb.load(); return reply(&value, sizeof(value));
      }
      if (request->bRequest == AUDIO_CS_REQ_RANGE) {
        const audio_control_range_2_n_t(1) range = {1, {{-50 * 256, 0, 256}}};
        return reply(&range, sizeof(range));
      }
    }
  }
  return false;
}

bool tud_audio_set_req_entity_cb(uint8_t, tusb_control_request_t const* request, uint8_t* data) {
  if (request->bRequest != AUDIO_CS_REQ_CUR || (request->wValue & 0xff) != 0) return false;
  const uint8_t entity = request->wIndex >> 8, selector = request->wValue >> 8;
  if (entity != kFeature) return false;
  if (selector == AUDIO_FU_CTRL_MUTE && request->wLength == 1) {
    hostMuted.store(data[0] != 0); return true;
  }
  if (selector == AUDIO_FU_CTRL_VOLUME && request->wLength == 2) {
    int16_t value; memcpy(&value, data, sizeof(value));
    if (value < -50 * 256 || value > 0) return false;
    volumeDb.store(value); return true;
  }
  return false;
}
}  // extern "C"
