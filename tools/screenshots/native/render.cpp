#include <M5Unified.h>
#include <fstream>
#include <vector>
#include "PrinterState.h"
#include "PrinterScreen.h"
#if CAPTURE_KEYBOARD
#include "SetupUi.h"
#include "TouchKeyboard.h"
#endif

// These declarations and drawing functions are extracted unchanged from the tag.
#include "menu_types.inc"
PrinterState printerNow;
AppMode currentMode = CAPTURE_MODE;
bool printerModeAuto = CAPTURE_AUTO;
uint32_t modeMenuHintUntil = 0;
String modeMenuHint;
struct { bool isEnabled() const { return true; } } bambu;
struct HostConfig {
  bool commentaryVoice = true, autoPrinterMode = true;
  String bambuHost = "192.0.2.25";
};
struct {
  HostConfig value;
  const HostConfig& config() const { return value; }
  bool isConnected() const { return true; }
} configPortal;
struct { String SSID() const { return "StackChan-Demo"; } } WiFi;
M5Canvas& startupCanvas() { return M5.Display; }
#include "menu_draw.inc"

void save(M5Canvas& canvas, const std::string& destination) {
  std::vector<lgfx::bgr888_t> pixels(320 * 240);
  canvas.readRectRGB(0, 0, 320, 240, pixels.data());
  std::ofstream file(destination, std::ios::binary);
  file << "P6\n320 240\n255\n";
  for (const auto& p : pixels) {
    file.put(p.R8()); file.put(p.G8()); file.put(p.B8());
  }
  if (!file) throw std::runtime_error("Could not write framebuffer");
}

#if CAPTURE_KEYBOARD
// Includes the tag's original layout, icons, text and drawKeyboard function.
namespace keyboard_capture {
#include "keyboard_draw.inc"
void render(M5Canvas& canvas) {
  KeyboardOptions options;
  options.title = "Wi-Fi パスワード";
  options.hint = "StackChan-Demo";
  options.secret = true;
  State state;
  state.text = "sample-only";
  state.reveal = false;
  std::vector<Key> keys;
  buildLayout(Page::Letters, keys);
  const setupui::Button cancel = {4, 3, 30, 24, kCancelId};
  const setupui::Button reveal = {setupui::kScreenW - 58, 3, 54, 24, kRevealId};
  drawKeyboard(canvas, options, state, keys, -1, cancel, reveal);
}
}
#endif

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::string output = argv[1];
  auto& canvas = startupCanvas();
  canvas.setColorDepth(CAPTURE_DEPTH);
  if (!canvas.createSprite(320, 240)) return 3;
  printerNow.link = LinkState::Online;
  printerNow.synced = true;
  printerNow.phase = PrintPhase::Running;
  std::strcpy(printerNow.jobName, "StackChan スタンド");
  printerNow.percent = 62;
  printerNow.remainingMin = 42;
  printerNow.layer = 155;
  printerNow.totalLayers = 250;
  printerNow.stage = 0;
  printerNow.speed = SpeedLevel::Standard;
  printerNow.wifiDbm = -48;
  printerNow.nozzleTemp = printerNow.nozzleTarget = 220;
  printerNow.bedTemp = printerNow.bedTarget = 55;
  printerNow.amsCount = 1;
  printerNow.trayNow = 0;
  const char* materials[] = {"PLA", "PETG", "PLA"};
  const uint32_t colors[] = {0x60a5fa, 0xf8fafc, 0xf472b6};
  for (int i = 0; i < 3; ++i) {
    auto& tray = printerNow.trays[0][i];
    tray.present = true;
    tray.color = colors[i];
    std::strcpy(tray.type, materials[i]);
  }
  drawPrinterScreen(canvas, printerNow, "順調に印刷しているよ！", true);
  save(canvas, output + "/device-printer.ppm");
  drawModeMenu(ModeMenuButton::None);
  save(canvas, output + "/device-menu.ppm");
#if CAPTURE_KEYBOARD
  keyboard_capture::render(canvas);
  save(canvas, output + "/device-keyboard.ppm");
#endif
}
